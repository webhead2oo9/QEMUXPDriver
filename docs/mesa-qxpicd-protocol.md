# Mesa/QXPICD ownership protocol (implemented v1)

This document records the protocol implemented by the current uncommitted
XPDriver/QXPICD and qemu-libretro working trees. It is an implementation snapshot,
not a statement that every behavior below is desirable or frozen. In particular,
the items under **Known gaps** are observable properties of the current code that
must be resolved or deliberately accepted before release.

The broader refusal requirements and their test history remain in
[`refusal-semantics.md`](refusal-semantics.md). This document is the shorter wire
reference for code that implements the guest transport.

Implementation sources inspected for this snapshot are
`guest/qxpicd/{transport.c,qxpicd.c}` in XPDriver at `dd3fc4c80d` plus its working
tree, and `hw/mesa/{mesapt_mm.c,mglfuncs.h}` plus `ui/libretro-3dfx.c` in
qemu-libretro at `4636110354` plus its working tree.

## Address spaces and access rules

QXPICD maps these guest-physical ranges through XPDriver's `qxpmap.sys`
(`IOCTL_QXPMAP_MAP_WINDOW`, windows 1-4 in `guest/qxpmap/qxpmap_ioctl.h`; the
driver serves only these fixed windows). Until the `qxpmap` cutover it used the
qemu-3dfx `MAPMEM` driver. `qxpmap` is not exclusive, so the `0xFB4`
reservation below is the only thing that keeps a second process off the FIFO:
nothing writes the FIFO or `fbtm` before a reservation is won.

| Range | Size | Purpose |
|---|---:|---|
| `0xefffe000` | `0x1000` | Mesa passthrough MMIO registers (`ptm`) |
| `0xec000000` | `0x03ffc000` | command FIFO, data ring, and scratch pages |
| `0xea000000` | `0x02000000` | large bulk-transfer aperture (`fbtm`) |
| `0xe0000000` | `0x08000000` | mapped buffer-object aperture |

Register offsets below are relative to `0xefffe000`. The device accepts 32-bit
native-endian accesses. A doorbell whose result is read back must be issued through
the paired-access rule in [SMP reply ownership](#smp-reply-ownership).

## Register map

| Offset | Direction | Implemented behavior |
|---:|---|---|
| `0xFA4` | R | Global synchronous-GL result for the kernel XPDM Direct3D transport. The miniport globally serializes each result-bearing `0xFC0` doorbell and this read with `VideoPortSynchronizeExecution`; unlike the ordinary `0xFC0` readback, the value is independent of the reading vCPU. |
| `0xFA8` | R | Global activation-completion value for the kernel XPDM Direct3D transport. `0x000320A0` means the reserved `QXPD3D` client initialized Mesa successfully. This is not a per-vCPU reply slot; the driver confirms the subsequent bind through the global `0xFB4` ownership state. |
| `0xFD4` | W | Publish the completed Mesa frame without exchanging the host double buffers (`MESAGL_MAGIC`). The XPDM Direct3D transport uses this for windowed COPY presentation so the backbuffer remains intact. |
| `0xFB0` | W | Owner heartbeat. A nonzero 16-bit value renews the lease only when it equals the current `OWNED` session's holder token. |
| `0xFB4` | R/W | Read the versioned session probe; write a nonzero 16-bit token to reserve or re-arm the same reservation. |
| `0xFB8` | R | Legacy window-ready register. Returns `glwnd_ready()` when nonzero. When it is zero, the host substitutes `MESA_BUSY` only if this vCPU's reply slot is marked refused. QXPICD does not use this register. |
| `0xFBC` | W/R | `0xA0320` activates the guest DLL after signature validation; `0xD0320` unloads and releases. Readback is the per-vCPU `mesa_ver` latch. Successful activation reads `0x000320A0`; activation while `OWNED` reads `MESA_BUSY`. |
| `0xFC0` | W/R | Synchronous GL function doorbell and per-vCPU `FRet` readback. |
| `0xFEC` | W/R | Legacy ChoosePixelFormat doorbell and per-vCPU pixel-format readback. While `OWNED`, the host preserves the incumbent answer instead of reconfiguring the window. |
| `0xFE8` | W/R | Legacy DescribePixelFormat doorbell and per-vCPU format-count readback. While `OWNED`, it returns zero and marks that vCPU refused. |
| `0xFE4` | W/R | SetPixelFormat/bind doorbell and per-vCPU `procRet` readback. Success is `MESAGL_MAGIC`; refusal is zero. This is where `FREE`/`RESERVED` becomes `OWNED`. |
| `0xFE0` | W/R | Extension-name query and per-vCPU `procRet` readback. |
| `0xFF0` | W | Process queued commands, publish the completed frame, and exchange the host double buffers (`MESAGL_MAGIC`). The XPDM Direct3D transport uses this for hardware Flip. |
| `0xFF4` | W | Delete context. A level-0 `MESAGL_MAGIC` write also clears the ownership record. |
| `0xFF8` | W | Make current; for a tokened owner, a zero context in scratch releases current without deleting it. |
| `0xFFC` | W | Create the ordinary host context (`MESAGL_MAGIC`). |

`MESAGL_MAGIC` is `0x5B5EB5E5`. `MESA_BUSY` is `0xB5B5B5B5` and is a refusal/busy
sentinel, not a ready or success value. Legacy code only needs `0xFB8` to be nonzero
to escape its ready loop, then observes failure from the following pixel-format
readback. New code must compare exact success values rather than treating arbitrary
nonzero values as success.

## Probe value and reservation

A read of `0xFB4` returns:

```text
31              24 23      20 19      16 15                 0
+----------------+----------+----------+--------------------+
| tag = 0x3D     | version  | state    | holder token       |
+----------------+----------+----------+--------------------+
                         v1       0/1/2        unsigned 16-bit
```

Equivalently:

```text
(0x3D << 24) | (1 << 20) | (state << 16) | holder_token
```

The v1 states are `FREE=0`, `RESERVED=1`, and `OWNED=2`. A free v1 device reads
`0x3D100000`. Old hosts do not implement the register and read as zero; QXPICD
therefore requires both tag `0x3D` and version `1` before advertising a format.

Reservation writes accept only `1..0xFFFF`:

- `FREE` + token `T` becomes `RESERVED(T)`;
- `RESERVED(T)` + the same `T` re-arms the reservation;
- a different reservation, any `OWNED` session, zero, or a value above `0xFFFF`
  is left unchanged.

The reservation TTL is `2000 ms`. Expiry is lazy: it is evaluated on probe
reads/writes, DLL activation, and SetPixelFormat binding rather than by a timer.
The current implementation measures it with `QEMU_CLOCK_VIRTUAL`, so time spent
with the VM paused does not expire an in-progress reservation. Reservation and
heartbeat leases therefore use the same clock domain.

## Signature, token, and bind payload

QXPICD derives `T` from the low 16 bits of `GetCurrentProcessId()` and substitutes
`1` if the result is zero. It is a collision-avoidance cookie, not an authenticated
process identity.

Before activation a client zeroes the final 16 bytes of `fbtm` and writes one of
these exact eight-byte signatures at `fbtm + 0x02000000 - 16`:

```text
51 58 50 49 43 44 00 01     "QXPICD\0" + protocol ABI 1
51 58 50 44 33 44 00 01     "QXPD3D\0" + attachment ABI 1
51 58 50 44 33 44 00 02     "QXPD3D\0" + attachment ABI 2
51 58 50 44 33 44 00 03     "QXPD3D\0" + attachment ABI 3
```

The host accepts the QXPICD signature, all three listed QXPD3D signatures, or
the legacy wrapper's exact eight-byte build stamp. QXPD3D attachment ABI 1
retains separate-array behavior. ABIs 2 and 3 both enable the bounded shared
interleaved host range; ABI 3 accompanies guest draw-packet ABI 47's one-copy
compact-indexed stream. QXPD3D uses the same reservation/bind lifecycle but
reads global activation completion from `0xFA8` and confirms bind through the
global `0xFB4` probe.

For `0xFE4`, the scratch block at the final 4 KiB of the FIFO mapping contains:

| Word/bytes | Meaning |
|---|---|
| word 0 | pixel-format index |
| word 1 | guest virtual address of `ptm`; the host compares its page-aligned value as `owner_ptm` |
| bytes at word 2 | `PIXELFORMATDESCRIPTOR` |
| word 12 | reservation token (QXPICD extension; legacy wrappers do not intentionally populate it) |

A bind is granted when one of the current host's conditions is true:

- state is `FREE` (legacy-compatible direct bind);
- state is `RESERVED(T)` and scratch word 12 equals `T`; or
- state is already `OWNED`, the normalized `ptm` address matches `owner_ptm`, and
  the shared ring refcount is at most one (incumbent rebind heuristic).

On a tokened reservation grant, the host initializes a fresh ring claim and
refcount before calling the host SetPixelFormat path. If that call returns
`MESAGL_MAGIC`, the host records `owner_ptm`, the attestation snapshot, whether the
grant was tokened, increments the session generation, and enters `OWNED`. A failed
host SetPixelFormat rolls back the newly created tokened ring claim and marks the
submitting vCPU refused.

## Session lifecycle and cleanup

The QXPICD path normally follows this sequence:

1. Map the four ranges and require a compatible `0xFB4` probe.
2. During `DrvDescribePixelFormat`, reserve or re-arm `T`. If that fails, report
   zero accelerated formats so stock XP selects GDI Generic.
3. During `DrvSetPixelFormat`, reserve again, publish the QXPICD signature, activate
   through `0xFBC`, and bind through tokened `0xFE4`.
4. The host starts the five-second tokened lease as part of the grant. After the
   grant, the ICD starts a worker that writes `T` to `0xFB0` immediately and then
   once per second.
5. Create/make current/render through the ordinary Mesa passthrough registers.
6. On context deletion, stop the heartbeat, write level-0 delete at `0xFF4`, then
   write `0xD0320` to unload. QXPICD returns to its device-ready state and may later
   reserve and bind a fresh session.

The heartbeat TTL is `5000 ms` on `QEMU_CLOCK_VIRTUAL`. A tokened grant enables the
lease immediately, closing the process-death window before the heartbeat worker's
first scheduling slice; matching heartbeats renew it. The guest ICD sends a
one-second idle heartbeat and also piggybacks a heartbeat on active transport
calls, throttled to one per 500 ms so sustained rendering cannot starve lease
renewal behind the transport lock. Completion of an accepted owner command that
belongs to the current, unreclaimed ownership generation also renews the lease;
this covers a cold host GL dispatch or display-thread stall that takes longer
than the TTL. A paused VM therefore does
not lose a live heartbeat-enabled owner. On expiry the host runs cleanup on its
main/display thread: it resets the FIFO claim and ring refcount, deletes the host
context and display timer, releases the window, finalizes Mesa state, clears result
state, and returns the ownership record to `FREE`.

The host also clears the ownership record on level-0 context delete, `0xD0320`,
device reset, and device finalization. These paths are not equivalent cleanup
operations. A tokened level-0 delete resets the FIFO/data ring before releasing the
session, so QXPICD clean teardown does not leave a stale claim. A legacy level-0
delete retains the historical ring behavior for compatibility.

Untokened legacy owners cannot opt into the heartbeat lease, so the host retains a
best-effort `MESAGL_CRASH_RC` recovery heuristic. Its `3000 ms` activity timestamp
uses `QEMU_CLOCK_REALTIME` and is checked only when a challenger reaches activation
(`0xA0320`) or SetPixelFormat (`0xFE4`). An expired owner at activation receives a
full reclaim (ring reset, GL/context/window cleanup, result reset, and `FREE`) before
normal activation continues. At `0xFE4`, the host performs only a session release,
deliberately retaining the initialized Mesa/window and ring so the historical warp
and rebind path can recover a sniff-sharing legacy loader. This compatibility path
can reclaim a live but sufficiently inactive legacy owner. The timestamp is refreshed
by a successful SetPixelFormat, make-current activity, swap, `glFinish`/`glFlush`,
and foreground activation; modern tokened QXPICD owners are excluded from it.

## Refusal state and `MESA_BUSY`

Each vCPU reply slot contains the synchronous return latches plus a `refused` bit.
Rejected activation, DescribePixelFormat, or SetPixelFormat operations set the bit;
selected successful operations clear it. It exists so a rejected legacy client can
observe nonzero `0xFB8` and escape its otherwise unbounded window-ready loop. The
actual operation still reports failure through its exact readback (`MESA_BUSY` for
activation, zero formats/count/result where applicable).

The bit identifies the vCPU that submitted the rejected operation, not a process,
thread, or durable session. It must not be treated as authentication or as a general
ownership query. QXPICD uses the explicit `0xFB4` state instead.

## SMP reply ownership

The host executes Mesa MMIO writes synchronously on the display/main-loop thread.
While waiting, the vCPU-side handler may release the BQL, allowing another vCPU to
submit work. To prevent one vCPU from overwriting another's return value, the host
stores replies for `0xFBC`, `0xFC0`, `0xFEC`, `0xFE8`, `0xFE4`, and `0xFE0` in a
slot indexed by the submitting `current_cpu->cpu_index`.

That isolation creates a guest-visible requirement:

> A guest transport MUST execute a doorbell write and its matching readback on the
> same guest vCPU.

The write is complete before the MMIO store returns, but the pair is not one atomic
instruction. If the guest scheduler migrates the thread between the store and load,
the load selects another vCPU's slot and can return a stale or zero value. This was
observed on real XP with `-smp 2`.

Current QXPICD meets the rule by taking a transport lock, temporarily
pinning the calling thread to the lowest processor in its process affinity mask with
`SetThreadAffinityMask`, issuing the MMIO write/read pair, and restoring the old
mask immediately. If either affinity operation fails, the helper fails without
issuing the doorbell. Every future paired register access must use that helper.

The protocol does not require a particular mechanism. A kernel-mode transport may
pin execution to the current processor or perform both accesses inside one
non-migratable driver operation. Raising IRQL to `DISPATCH_LEVEL` can satisfy the
no-migration interval only in kernel mode and only if the entire access path is safe
at that IRQL; it is not available to the user-mode ICD and is not by itself a wire
protocol feature. The invariant to test is same-vCPU execution of the pair.

The XPDM Direct3D transport checks activation at global `0xFA8`,
reservation/bind through global `0xFB4`, and synchronous GL results through global
`0xFA4`. It globally serializes each result-bearing `0xFC0` doorbell and `0xFA4`
read with `VideoPortSynchronizeExecution(VpHighPriority)`. This avoids depending on
the scheduling vCPU at all: a later real `-smp 2` Max Payne run proved that the
earlier same-vCPU assumption could still return stale, non-GL values despite the
high-priority callback. Future kernel XPDM readbacks must use the same serialized
global-result pattern. User-mode QXPICD must continue using its affinity-pinned
per-vCPU reply helper; `0xFA4` is not a general multi-client reply channel.

## XPDM Direct3D draw-packet ABI 47

The display-to-miniport IOCTL contract is versioned separately from the host
attachment. ABI 47 changes only the compact-indexed variable payload: one byte
per expanded index, zero padding through the next eight-byte boundary, then one
tight transformed-FVF record per declared unique vertex. The miniport validates
all index bounds, requires every declared compact vertex to be referenced,
requires every padding byte to be zero, and validates every finite vertex record
before publication. It then reserves one FIFO data span and copies the complete
aligned index-plus-vertex stream once. This matches the host decoder's existing
consumption order, so no GL FIFO ordinal or argument layout changes. Attachment
ABI 3 identifies the paired implementation; the host continues to accept
QXPD3D/1 and /2 with their established behavior.

XPDM Direct3D transport version 31 distinguishes presentation semantics instead
of treating every Present as a host buffer exchange. DirectDraw hardware Flip uses
`0xFF0`; windowed Unlock/Blt presentation uses publish-only `0xFD4`, preserving the
backbuffer required by `D3DSWAPEFFECT_COPY`. Its Clear request also carries the
target dimensions and up to 32 clipped D3D rectangles. A zero-rectangle D3D Clear
is expanded to the active viewport (or the full target when no viewport is set),
and larger lists are submitted in bounded chunks. The miniport enables OpenGL
scissoring for every color, depth, or stencil clear. Default-framebuffer rectangles
convert D3D's top-left y coordinate to OpenGL's bottom-left origin; offscreen FBO
rectangles retain the established render-target row-zero mapping.

## Threat model

The v1 ownership protocol contains cooperating but buggy guest clients; it does not
provide isolation against a hostile guest process. Specifically:

- `0xFB4` publishes the holder token to every reader;
- possession of that value is enough to re-arm a reservation or heartbeat lease;
- PID-derived 16-bit tokens can collide or be reused;
- rebind uses `owner_ptm` plus shared refcount as an identity heuristic; and
- teardown commands such as `0xD0320` carry no authenticated owner identity.

Accordingly, “exclusive owner” means host-enforced arbitration among conforming
QXPICD and legacy-wrapper clients, not a security boundary inside the XP guest.
Changing that threat model requires a non-readable capability or host-maintained
caller identity plus authenticated teardown; merely hiding the probe token would
not fix every authority-free command.

## Known gaps in the current implementation

1. **Legacy liveness is heuristic:** untokened wrappers have no heartbeat. Their
   recovery uses the historical three-second realtime activity timestamp when a
   challenger reaches activation or SetPixelFormat. It recovers a crashed legacy
   owner, but can also reclaim a live legacy owner that has produced no tracked
   activity for long enough. VM pause can age this heuristic.
2. **Token disclosure and spoofing:** the holder token is readable and replayable;
   v1 is not adversarially secure, as documented above.
3. **Authority-free teardown:** the host honors level-0 delete and unload without a
   token or process identity. This is compatible with the old ABI but reinforces
   that v1 is cooperative arbitration, not isolation.

These gaps should be resolved in code and regression-tested before this document is
promoted from “implemented snapshot” to a frozen protocol.
