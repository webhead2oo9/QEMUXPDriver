# QxpMap: guest memory windows and QD3D slot lifetime

`guest/qxpmap/` is XPDriver's GPL-2.0-only, Windows XP SP3 x86 kernel
control driver. Its service/image/device names are `qxpmap`, `qxpmap.sys`,
`\Device\QxpMap`, and `\\.\QXPMAP` (`\DosDevices\QXPMAP`). It is for the
paired qemu-libretro hardware layout, not arbitrary physical hardware.

## Why replace MapMem

The supplied 3,836-byte `fxptl.sys` identifies 3Dfx Interactive and has no
available source. Its disassembly shows:

- `IoCreateDevice(..., Exclusive=TRUE, ...)`: only one open handle system-wide,
  even when callers request sharing. The legacy OpenGL ICD holds that handle
  for its entire lifetime; short-lived QD3D opens cannot solve that conflict.
- IOCTL `0x80002000` opens `\Device\PhysicalMemory` and maps a caller-selected
  physical range into the caller with `ZwMapViewOfSection`.
- `0x80002004` calls `ZwUnmapViewOfSection(-1, supplied_base)`.
- `0x80002008` and `0x8000200c` execute caller-selected `rdmsr` and `wrmsr`.

QxpMap is nonexclusive, accepts no caller-selected physical address or size,
provides no CPU-register access, and owns the mappings and QD3D slot through a
file object. QD3D no longer depends on MapMem or global named-event slot claims
in the VM transport. The development loopback transport is separate.

The disassembly used for the cache decision is
`~/qd3d-extract/fxptl.asm` in WSL: at `0x10648` it pushes `4` as the final
`ZwMapViewOfSection` argument (`Win32Protect = PAGE_READWRITE`); the call is at
`0x10666`. It uses allocation type zero and `ViewShare` (1). These are static
observations, not a throughput measurement.

## Interface and fixed hardware layout

`guest/qxpmap/qxpmap_ioctl.h` is the public contract. All five operations are
`METHOD_BUFFERED`, `FILE_ANY_ACCESS`, using device type `0x8317`. Structures use
fixed-width Windows types and fixed sizes; the driver asserts sizes and field
offsets, and checks input/output lengths and system-buffer alignment before
access. Unknown requests, invalid IDs, nonzero reserved input, and a forged
UNMAP tuple fail without mapping a caller-chosen range.

| Operation | Meaning |
| --- | --- |
| `QUERY` | Return interface version 1, detected QD3D slots/free slots, 32-MiB slot size, and supported Mesa-window mask. `Qd3dAbi` is zero: the register page has no ABI register. Reserved fields are zero. |
| `QD3D_CLAIM` | No input; select the lowest free slot and map only its channel plus the register page. Return slot, channel size, and two user virtual addresses. One slot per handle. |
| `QD3D_RELEASE` | No buffers; unmap the slot views, notify the host, and free ownership. Idempotent when no slot is held. Does not release Mesa mappings. |
| `MAP_WINDOW` | Accept one fixed Mesa-window ID and zero `Reserved`; return address, fixed size, and ID. At most one mapping of each window per handle. |
| `UNMAP_WINDOW` | Accept exactly the returned address/size/ID tuple for a mapping owned by this file object and process. No arbitrary address unmap. |

CLAIM returns `STATUS_DEVICE_BUSY` when full (`ERROR_BUSY`),
`STATUS_NO_SUCH_DEVICE` when no supported QD3D register page is detected, and
`STATUS_INVALID_DEVICE_REQUEST` for a repeated claim (`ERROR_INVALID_FUNCTION`).
MAP accepts IDs 1 through 4 only; QD3D channels and registers are never MAP
windows, including when a physical address is supplied as the ID.

| Window | Guest physical address | Size | User mapping policy |
| --- | --- | --- | --- |
| QD3D channel *i*, 0–3 | `0xd0000000 + i * 0x02000000` | 32 MiB | `PAGE_READWRITE` |
| QD3D registers | `0xd8000000` | 4 KiB | `PAGE_READWRITE | PAGE_NOCACHE` |
| Mesa registers, ID 1 | `0xefffe000` | 4 KiB | `PAGE_READWRITE | PAGE_NOCACHE` |
| Mesa FIFO, ID 2 | `0xec000000` | `0x03ffc000` | `PAGE_READWRITE` |
| Mesa FBTM, ID 3 | `0xea000000` | 32 MiB | `PAGE_READWRITE` |
| Mesa MBUFO, ID 4 | `0xe0000000` | 128 MiB | `PAGE_READWRITE` |

The QD3D registers are KICK `0x000`, ID `0x004` (magic `0x44334451`),
SLOTS `0x008`, and the new write-only RELEASE `0x00c`. ID and SLOTS are read
on QUERY/CLAIM, so loading the driver without a QD3D device is allowed. A slot
count of zero or greater than the four supported slots is treated as absent.
The supported-window mask is not a claim that Mesa hardware is present.

The wire ABI is **4**, including the host release semantics. The runtime checks
the channel's wire ABI before submitting, rather than treating `QUERY.Qd3dAbi`
as a version check. `qxpmap_layout.h` duplicates only C89-compatible hardware
constants because the protocol header uses `stdint.h` and inline functions.
The probe's compile-time assertions compare every QD3D address, size, count,
register offset, and magic against `qd3d/proto/qd3d_proto.h`, and require ABI 4.

## Lifetime, process context, and ordering

DriverEntry creates the nonexclusive device and DOS link, opens
`\Device\PhysicalMemory` once with `OBJ_KERNEL_HANDLE` and only section-map
read/write rights, and maps the QD3D register page in the kernel with
`MmMapIoSpace(..., MmNonCached)`. Failure unwinds the objects already created;
unload removes the link, kernel view, section handle, and device.

Each file object's `FsContext` holds a referenced owning process, one optional
slot, the two QD3D views, and a bounded list of four Mesa views. A single
`KMUTEX` serializes the slot table and file-state transitions. A fast mutex or
spinlock would raise IRQL and is unsuitable around the PASSIVE_LEVEL section
mapping calls. No worker thread, queued IRP, or attached-process mapping path
exists.

CREATE and DEVICE_CONTROL require PASSIVE_LEVEL, a user-originated IRP, the
current thread to be the IRP's issuing thread, the IRP's requestor process to
be the current process, and no top-level filesystem IRP. IOCTLs also require
the current process to equal the file's owner and require METHOD_BUFFERED.
`ZwMapViewOfSection` therefore uses `ZwCurrentProcess()` only after the caller
context has been established. The section handle is never returned to users.

Mappings use `ViewUnmap`, not MapMem's inheritable `ViewShare`. XP x86's
`MEM_DOS_LIM` requests page granularity: the Mesa register page must not expose
surrounding pages merely because its address is not 64-KiB aligned. The driver
rejects and unmaps a returned view if its physical offset or size differs from
the exact fixed window, or its address is not a page-aligned user range.
This XP/x86-specific mapping behavior needs the real XP probe, not just a
compiler check.

Explicit release and `IRP_MJ_CLEANUP` unmap in the owning process's context.
For a foreign-context cleanup, `KeStackAttachProcess` temporarily enters the
referenced owner's address space, unmaps each view, and immediately detaches.
No request completion, host notification, or application-dependent wait occurs
while attached. This also revokes a live owner's views when a duplicated or
inherited handle is closed last in another process. Mesa views follow the same
rule. An already-unmapped view or an exiting process counts as revoked.
Only successful revocation permits RELEASE and slot reuse. Unexpected errors
retain ownership: explicit RELEASE can be retried and CLOSE retries CLEANUP.
An unexpected persistent unmap failure at final CLOSE fails closed, retaining
the context/mappings/slot and disabling driver unload until reboot rather than
reassigning a still-writable channel. This is not a silent successful release.

After unmapping, the driver calls `WRITE_REGISTER_ULONG(RELEASE, slot)`, then
frees the slot while still holding the mutex. The host snapshots the old
producer's head as part of this MMIO write. Its executor later tears down the
old session and skips old packets only through that captured boundary. A new
claimant may submit HELLO after the write returns without having that HELLO
mistakenly discarded. The driver never waits for D3D teardown in the kernel.
CLEANUP is idempotent; CLOSE supplies defensive cleanup, dereferences the
process, and frees the context. A separate owner reference acquired by CLAIM
is dropped after the RELEASE write and slot-table update; the file's creator
reference remains until CLOSE so a still-open handle can re-CLAIM safely.
Normal process death closes the handle and causes release without user-mode
cleanup code.

## Security model and limits

The device uses `IoCreateDevice` with the operating system's default device
security, nonexclusive access, and `FILE_DEVICE_SECURE_OPEN`; nonempty trailing
file names are rejected. There is no custom permissive NULL DACL.
[Microsoft documents](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/controlling-device-access)
the default for most device types/characteristics as Administrators full
access and everyone else read/write/execute access. That is the intended XP
control-device policy, allowing ordinary users to render. The actual XP ACL
and limited-user access have not been measured by a build: run the probe both
as administrator and as a limited user in the target guest. Registry/security
policy can change the granted rights. This legacy non-PnP default intentionally
follows the requested XP policy rather than adding a separate security class;
modern general-purpose non-PnP drivers should use `IoCreateDeviceSecure`.

The driver removes MapMem's arbitrary-physical-memory and MSR interfaces, but
**it is not a hostile-client isolation boundary**:

- All claimants map the same writable QD3D register page. A malicious client
  can ring KICK or write RELEASE for another slot. The protocol does not
  authenticate register writes. A kernel-only release/kick interface would
  require a different user contract; it is not part of this cutover.
- Inherited/duplicated handles are supported-but-revoking: foreign-process
  IOCTLs are denied, and last-close cleanup attaches to the original owner and
  unmaps its views before releasing the slot. The shipping QD3D client opens
  non-inheritable handles and never duplicates them. A deliberately retained
  duplicate can still postpone the file's last-close cleanup.
- User code must quiesce its producer before explicit RELEASE and must not
  independently unmap/reuse the returned virtual addresses. UNMAP checks the
  stored tuple, not the identity of a replacement VAD the caller installs at
  the same virtual address.
- Mesa windows are global, not per-client allocations. Their protocol's
  reservation rules, not QxpMap, arbitrate Mesa use. A process can open four
  QD3D handles and exhaust all four slots; there is no fairness policy.
- The fixed apertures must be reserved by the paired emulator and must not
  overlap guest RAM or unrelated hardware. The driver is not a PCI/PnP
  resource-discovery driver and must not be installed on a physical machine
  or an unrelated VM layout. It does not validate host command execution.

## Cache attributes

QD3D channels retain MapMem's plain `PAGE_READWRITE` policy: no blanket
`PAGE_NOCACHE` or write-combining change on the RAM-backed command/reply path.
The Mesa bulk windows use the same policy for the later ICD migration.
This preserves the requested mapping attributes; actual performance remains
**Unknown** until measured in the same scene and guest.

Both kernel and user QD3D register aliases are noncached; Mesa's doorbell page
is also noncached. Volatile user accesses and kernel register accessors provide
the required MMIO operations; a normal cached register page is not acceptable.
[ZwMapViewOfSection's cache-alias restrictions](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwmapviewofsection)
apply: do not map these same pages concurrently through legacy MapMem with a
different cache type. Only the paired device apertures are supported. The
probe must run without a live legacy OpenGL session.

## Installation and replacement

The signed XPDM package and QD3D tools-CD installer share service `qxpmap`,
display name `XPDriver guest memory-window driver`, with
`SERVICE_KERNEL_DRIVER`, `SERVICE_SYSTEM_START`, `SERVICE_ERROR_NORMAL`,
and image `%SystemRoot%\System32\drivers\qxpmap.sys`. The normal SYSTEM_START
policy makes the mapping device available before ordinary desktop clients
connect, without making it a boot-critical driver. The driver can load when
QD3D is absent; QUERY reports zero slots. There is no need for a boot-start
storage-class ordering rule.

The QD3D installer accepts the service ImagePath spellings
`\SystemRoot\system32\drivers\qxpmap.sys`,
`system32\drivers\qxpmap.sys` (SetupAPI's `%12%\qxpmap.sys`),
`%SystemRoot%\system32\drivers\qxpmap.sys`,
`\??\<windir>\system32\drivers\qxpmap.sys`, and
`<windir>\system32\drivers\qxpmap.sys`, case-insensitively. Any other image
path or service type remains a conflict, refused without changes.

The driver's numeric VERSIONINFO **FILEVERSION** is the ordering key for both
distribution paths: neither replaces a higher installed version with an
older carried image. QD3D reports the newer installed version and still
ensures the service exists, has SYSTEM_START and is running. Equal or lower
installed versions retain byte-compare replacement, so equal-version
development builds can update. When an otherwise valid installed driver's
fixed FILEVERSION is unreadable, QD3D reports that fact and falls back to byte
comparison. A carried driver without readable FILEVERSION is refused.

The registration key
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd`
marks the OpenGL ICD's dependency on this shared service. QD3D uninstall
restores its stock DLLs but leaves the service and image untouched while that
key exists, reports that the XPDriver OpenGL driver uses it, and succeeds.
After OpenGL is unregistered, QD3D uninstall removes the service/image with
its existing idle or deferred-removal behavior. Registry lookup failures
other than an absent key refuse removal rather than risking the shared driver.

For an in-use replacement, SYSTEM_START precedes smss's
`PendingFileRenameOperations` and could load the old driver before its queued
replacement. The installer temporarily selects **`SERVICE_AUTO_START`** for
that replacement boot, allowing smss to replace the image before SCM loads it.
The next successful installer `driver`/`install` operation restores the normal
SYSTEM_START setting; there is no RunOnce helper. Rollback restores the old
service configuration. Do not stop/unload the driver while clients hold views.
The XPDM co-installer promotes only DEMAND_START or DISABLED services to
SYSTEM_START, preserving existing BOOT_START, SYSTEM_START, and AUTO_START
settings while normalizing error control to NORMAL so it cannot undo that
replacement ordering. `qxpsetup` accepts AUTO_START only when
`PendingFileRenameOperations` contains an entry naming `qxpmap.sys`; otherwise
it requires SYSTEM_START.

The image's VERSIONINFO identifies `CompanyName=XPDriver project`,
`ProductName=XPDriver`, `OriginalFilename=qxpmap.sys`,
`FileDescription=XPDriver guest memory-window driver`, version `1.0.0.0`, and
`GPL-2.0-only`. Build/install only from the paired source/package. Never create
or start this service on the Windows development host.

## Build and probe

Authentic kernel gate, from a Windows command prompt:

```bat
guest\qxpmap\build-ddk.cmd C:\WinDDK\7600.16385.1
```

The wrapper selects `setenv.bat ... fre WXP no_oacr`, runs `build.exe -cZ`
with `/W4 /WX`, rejects nonempty fresh `.err` or `.wrn` logs, and runs:

```bat
py -3 guest\xpdm\validate_ddk_pe.py --native-driver guest\qxpmap\objfre_wxp_x86\i386\qxpmap.sys
```

`--native-driver` is the validator's new non-video mode: i386 PE32 driver,
native subsystem XP 5.1, nonzero entry, valid nonzero PE checksum, no exports,
and imports restricted to `NTOSKRNL.EXE` and optional `HAL.DLL`. Existing
`--kernel` remains the video-miniport mode (including its required
`VIDEOPRT.SYS` import); the GDI default remains unchanged. The MinGW DDK headers
may be used with `-std=c89 -Wdeclaration-after-statement -Wall -Wextra -Werror`
as a syntax aid, but neither that compile nor a MinGW-linked image satisfies
the authentic kernel gate.

The tested syntax-aid command (not a shipped driver build) is:

```sh
i686-w64-mingw32-gcc -std=c89 -Wall -Wextra -Werror \
  -Wdeclaration-after-statement -D_X86_=1 -D_WIN32_WINNT=0x0501 \
  -DWINVER=0x0501 -DNTDDI_VERSION=0x05010300 \
  -DNTDDI_WINVISTA=0x06000000 \
  -isystem /usr/i686-w64-mingw32/include/ddk \
  -fsyntax-only guest/qxpmap/qxpmap.c
```

The command-only `NTDDI_WINVISTA` definition corrects an installed MinGW DDK
header guard typo (`NTDDI_WINVISTA` versus `NTDDI_VISTA`) without raising the
XP target or modifying the driver/WDK headers. No compatibility shim is shipped.

Build the in-VM probe under WSL from the repository root:

```sh
make -C guest/qxpmap/test
```

This builds `guest/qxpmap/test/build/qxpmap_probe.exe` using i686 MinGW,
`-Wall -Wextra -Werror`, XP 5.1 PE metadata, and runs
`qd3d/tools/check_imports.py` against the XP SP3 export list. It also provides
the compile-time protocol/layout consistency check.

In an **idle XP VM**, with the paired ABI-4 core and driver already loaded,
run `qxpmap_probe.exe`. It prints individual PASS/FAIL markers, a final
`RESULT: PASS qxpmap_probe (0 failures)` on success, and exits zero only then.
It checks:

1. QUERY and the short-output rejection; five simultaneous nonexclusive opens.
2. Four distinct claims, each with readable QD3D ID and a writable channel.
   Foreign-process last-close also checks that a living owner's channel,
   register page, and Mesa view are revoked before the slot can be reclaimed.
3. Busy fifth claim and refusal of a second claim on one handle.
4. RELEASE/re-CLAIM and last-handle-close reclamation.
5. A child claiming the only free slot, parent `TerminateProcess`, child
   process exit, and parent reclaiming that exact slot.
6. Unknown MAP IDs, nonzero Reserved, and all QD3D physical addresses rejected.
7. Fixed Mesa window mapping/unmapping, address/size/protection, changed-tuple
   rejection, and repeated UNMAP rejection. Mesa identity is read only. An
   absent/incompatible Mesa device produces an explicit SKIP for Mesa execution,
   not a claim that absent hardware was exercised.
8. All probe slots free after its handles close.

The initial QUERY requires all four slots free; otherwise the probe stops
without claiming. It never reclaims another process's slot, writes KICK,
submits HELLO/rendering commands, or writes Mesa registers. It temporarily
writes/restores one word in each channel it owns. Four slots are held during
some steps, so concurrent clients can temporarily fall back: do not run this
against a live QD3D/OpenGL workload. The probe creates and kills only its own
child, not any existing application.

## Phase 2: the OpenGL ICD

`guest/qxpicd/transport.c` opens `\\.\QXPMAP` with read/write sharing, maps
windows 1-4 with `IOCTL_QXPMAP_MAP_WINDOW` and checks each returned window id,
size and address; any failure closes the handle, which undoes every view it
made. The ICD holds the handle for the life of the process (process exit
unmaps). MapMem's device was exclusive, so a second OpenGL process could not
even open it; QxpMap is not, so the host's `0xFB4` reservation alone keeps a
second process off the FIFO. That holds because nothing writes the FIFO or
`fbtm` before `qxp_transport_reserve` succeeds, and the reservation token is
the owner's process id, which no two live processes share. A new process
reusing a dead owner's id within the host's 5-second lease can still inherit
its reservation; that predates QxpMap (MapMem's handle closed at the owner's
death too).

XPDriver ships no MapMem driver. Once the ICD runs on QxpMap, `fxptl.sys` in a
VM image serves only qemu-3dfx's own wrappers (its Glide and OpenGL DLLs);
removing it from an image is safe only where those are not used.

Still unqualified: limited-user access, service replacement across a reboot
(the deferred path), the canonical fresh-child harness cycle (blocked on this
machine by missing local harness assets), and the ICD's direct test from the
tools CD (`RUNTESTS.CMD`, not yet run in XP).

## Diagnostics

The driver writes one-off lines to QEMU's debug console (I/O port `0xe9`,
`-debugcon file:...`, the channel the XPDM drivers also use): `QXPMAP load`,
then `create-device`, `open-physical-memory`, `map-registers` (kernel address),
`qd3d-id` (expect `44334451`), `qd3d-slots` and `create-link` with their
status, `ready`, and `claim-failed <NTSTATUS>` for each refused claim. Nothing
is written per frame or per successful call.

## Validation in XP (2026-11-02, child overlay of the QD3D session disk)

- Installed by the tools CD's `UPDATE.CMD` (`qd3d_install.exe driver`); after
  a restart the driver loaded at boot (`SERVICE_SYSTEM_START`) with the trace
  above ending in `ready`.
- `qxpmap_probe`: 0 failures, including four distinct claims, the busy fifth
  claim (`claim-failed 80000011`), a second claim on one handle
  (`claim-failed c0000010`), release on close, foreign last-close revocation,
  TerminateProcess releasing the child's slot, and the Mesa window checks.
- Every smoke test passed through the driver: Direct3D 9 19/19, Direct3D 8
  17/17, DirectDraw 11/11 windowed and 12/12 fullscreen, Direct3D 7 22/22
  windowed and 23/23 fullscreen, Direct3D 1-6 21/21.
- Prince of Persia: The Sands of Time ran launcher (slot 0) and game (slot 1)
  at 527-647 FPS (800x600); on exit the host logged `slot release` for the
  game and then the launcher, so the desktop returned without the timeout.
- Two field lessons: XP's `sc.exe` (5.1.2600.0) exits 0 for a missing
  service, so scripts must not test its exit code (`UPDATE.CMD` now always
  runs the idempotent `driver` command); and one earlier install of the
  pre-trace build failed to start with `ERROR_BAD_DRIVER` (2001) while the
  traced build of the same code loaded. That binary was overwritten before it
  could be compared, so the cause is unknown; the trace makes a repeat
  diagnosable.
- Phase 2, same child overlay: package `1.0.0.306`, installed over an older
  XPDM package by `qxpsetup.exe` (`FILES_OK=1 QXPMAP_SERVICE_OK=1`), put the
  new `qxpicd.dll` and `qxpmap.sys` in place byte for byte; after the restart
  `qxpwgltest.exe --native` rendered through stock `opengl32`, the display
  driver's escape, the ICD and QxpMap on the host RTX 4090 (`rc=0`, twice),
  while its competing child got `GDI Generic` (`rc=0`): the host reservation
  alone keeps a second process off the FIFO. Two Direct3D 9 games ran over
  QD3D in the same boot.
- Upgrade lesson: setupapi.log showed the new `qxpcoinst.dll` queued behind
  the in-use old one (`SETD.tmp`, moved at reboot), so the old co-installer
  ran `DIF_INSTALLDEVICE` and the new service code did not. The co-installer
  is therefore `qxpcoin2.dll` (package `1.0.0.307`); rename it again whenever
  its behavior changes. With `qxpmap` stopped and deleted first, the `.307`
  upgrade copied `qxpcoin2.dll` directly, the co-installer recreated the
  service with the contract identity and started it (a second
  `QXPMAP load ... ready` before the reboot), the driver loaded at boot, and
  the native OpenGL test and its fallback child passed again.
