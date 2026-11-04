# What stock XP's `d3d9.dll` actually requires for a native DDI-v4 HAL

## 2026-07-31 diagnostic result and ordinary boundary

The measured contract derived below is deliberately diagnostic. Stock XP SP3
System32 `d3d9.dll` accepts DDI version 4 only when the separately signed
package enables `QXP_DIAGNOSTIC_DDI4`,
`QXP_DIAGNOSTIC_STREAMOFFSET`, `QXP_DIAGNOSTIC_MAXSTREAMS8`, and
`QXP_DIAGNOSTIC_SM2_CAPS`. The resulting profile reports
`D3DDEVCAPS2_STREAMOFFSET`, eight streams, VS2, and PS2.

The exact diagnostic run at
`artifacts/runs/20260731-abi61-diagnostic-boundary-r143/D3d9Multistream7/`
creates the HAL, accepts advertised VS1.1, executes fixed-function/SM1.x and
native SM2 draws, checks exact managed/default/indexed/texture pixels,
presents, resets, and tears down. It samples s4 and s15 independently, then
uses one stock-runtime-created `ps_2_0` program to declare and sample all
sixteen independently bound managed textures (`mask=ffff`, exact center
`ffff2040`). The same run passes the stream-0+7 release-before-Reset oracle.

Ordinary ABI-61 packages do not return DDI version 4. The exact fallback run
at `artifacts/runs/20260731-abi61-d3d8-specular-fix-r140/D3d9/` records
`QXPDISP GD9 FALLBACK`, `diagnostic=false`, VS1.1/PS1.3, and explicit
PS2/SM2 skips. That frozen package also passes the corrected 800x600 D3D8
visual and D3D8 interface matrices. The earlier r135 `diagnostic=false`
label described the old harness invocation, not an ordinary binary; r143
binds the diagnostic marker, tier, package, and capability expectation
together.


Neither variant advertises volume textures, MRT, queries, multisampling, or
other optional envelopes. Historical gates, addresses, and failure analysis
below remain provenance for the measured native-v4 contract.

Derived by disassembling the guest's own runtime rather than by inference.
`WINDOWS/system32/d3d9.dll` (1,689,088 bytes, dated 2008-04-14, XP SP3) was
extracted read-only from the guest image with `qemu-img convert` plus `7z`,
while no VM was running, and disassembled with `objdump -d -M intel`. Addresses
below are file-relative from that disassembly.

## The capability handshake

The runtime builds a `DD_GETDRIVERINFO2DATA` on the stack and issues two
queries in a fixed order (`0x4fe03ada` onward):

1. **`GETD3DCAPS9`**, type `0x10`, inner `dwExpectedSize` = `0x130` (304).
2. **`GETDDIVERSION`**, type `0x23`, with `dwDXVersion` set to `9` on input.

The outer `DD_GETDRIVERINFODATA` is at `[ebp-0x2c]`, giving `dwActualSize` at
`[ebp-0xc]` and `ddRVal` at `[ebp-0x8]`, which matches the WDK layout
(`dwSize`, `dwFlags`, `guidInfo[16]`, `dwExpectedSize`, `lpvData`,
`dwActualSize`, `ddRVal`).

Every one of these must hold, or the runtime abandons the v4 path:

| Check | Address | Requirement |
| --- | --- | --- |
| thunk result | `0x4fe03b10` | `eax == 1` for the caps9 call |
| caps9 status | `0x4fe03b1c` | `ddRVal == 0` |
| thunk result | `0x4fe03ba8` | `eax == 1` for the version call |
| version status | `0x4fe03bb0` | `ddRVal == 0` |
| **DDI version** | `0x4fe03bb4` | **`dwDDIVersion == 4` exactly** |
| caps9 status | `0x4fe03bc0` | `ddRVal == 0` again |
| **returned size** | `0x4fe03bc4` | **`dwActualSize == 0x130` exactly** |

On success (`0x4fe03bd1`) it copies `0x4c` (76) dwords — the full 304 bytes —
**starting at the gdi2 header address**, confirming that the driver is expected
to overwrite the header with the capability payload, which is what this driver
does. It then sets a flag, `[eax+0x154] |= 2`, marking the adapter as carrying
Direct3D 9 capabilities.

## The failure path identifies itself

Any failed check jumps to `0x4fe03c16`, which re-issues the query with type
`0x01` and `dwExpectedSize` `0xd4` (212) — that is **`GETD3DCAPS8`**.

That is exactly the `G9` → `GD9 FALLBACK` → `G8` sequence observed in the
shipping driver's logs. It also means the presence or absence of a caps8 query
is a reliable, externally observable indicator of whether the v4 handshake
succeeded.

## The consequence: the handshake already succeeds

The instrumented enumeration run
(`artifacts/runs/20260725-ddi4-gdi2-enumeration-t2/`) recorded all 127
`GetDriverInfo2` requests the runtime made while the driver reported version 4.
`GETD3DCAPS8` (type `0x01`) **never appears** in that list.

Since caps8 is only requested on the failure path decoded above, its absence
proves that **every check in the table passed**: the thunk succeeded, both
`ddRVal`s were zero, `dwDDIVersion` was exactly 4, and `dwActualSize` was
exactly `0x130`. The driver's Direct3D 9 capability block was accepted and
copied into the adapter.

This is consistent with, and explains, the ten prior eliminations — including
the strongest one, where handing the runtime back its own capability block
still produced `D3DERR_NOTAVAILABLE`. No capability value could ever have
mattered, because the capability handshake was never what failed.

## Where the blocker is not, and where it must be

Not the capability handshake, not any capability value, not the format list,
not an unanswered query, not marshalling. `GetDeviceCaps(D3DDEVTYPE_HAL)`
returns `D3DERR_NOTAVAILABLE` *after* the adapter has successfully acquired
Direct3D 9 capabilities.

So the remaining surface is what the runtime does **between** accepting those
capabilities and deciding a HAL device exists. The next investigation should
start from the adapter flag written at `[eax+0x154] |= 2` and the neighbouring
test of `[eax+0x1c] & 0x8000000`, tracing which additional condition gates HAL
availability, and should look at the other 30 sites referencing `0x8876086a` in
this disassembly for the one reachable from `GetDeviceCaps`.

## Refinement: the failure is a null cached device-caps pointer

Three of the thirty-one `0x8876086a` sites use the same compiled idiom:

```
4fdf79c9: mov  esi,DWORD PTR [edi+0x2288]
4fdf79cf: xor  eax,eax
4fdf79d1: test esi,esi
4fdf79d3: setne al
4fdf79d9: dec  eax
4fdf79da: and  eax,0x8876086a          ; return ptr ? S_OK : D3DERR_NOTAVAILABLE
```

with a sibling at `4fdf7a0e` testing `[eax+0x3428]`, and a third variant at
`4fdf7a3c`. Two different cached pointers on the same object, each guarded the
same way, is the shape one expects for per-device-type caps — plausibly HAL
versus REF/HEL.

That points at the failure being a **null cached device-caps pointer**: the
adapter successfully acquired Direct3D 9 capabilities (proved above), but the
per-device caps object was never constructed, so the accessor returns
`D3DERR_NOTAVAILABLE`.

This is a hypothesis, not yet proof: these three sites have not been shown to be
reachable from `IDirect3D9::GetDeviceCaps`, because the vtable has not been
located in this stripped binary. It is recorded as the most probable reading of
the evidence, and the next investigation should confirm or refute it by locating
the `IDirect3D9` vtable (GetDeviceCaps is slot 14) and checking whether it
reaches one of these accessors.

> **REFUTED — see the next section.** The accessor was fully decoded and its
> HAL branch cannot return `D3DERR_NOTAVAILABLE` for any driver-controlled
> reason. The "null cached device-caps pointer" reading applies only to the
> REF and SW device types, which are not what we are failing on.

## Refutation: the caps accessor is fully decoded, and HAL cannot fail there

The function containing all three sites is `0x4fdf7970`. It is `thiscall` with
`ret 0xc`, i.e. `(this, Adapter, DeviceType, ppCaps)` — exactly the
`IDirect3D9::GetDeviceCaps` shape, but it returns a *pointer* into a preallocated
array rather than copying, so it is the internal adapter-caps accessor. It
dispatches on `DeviceType` at `[ebp+0xc]` (`0x4fdf7975`):

| DeviceType | Path | Can return `NOTAVAILABLE`? |
| --- | --- | --- |
| `2` REF | per-adapter record stride `0x178`; cached ptr `[rec+0x2288]`; if null, constructs via `0x4fdf7560`; returns `ptr ? S_OK : NOTAVAILABLE` (`0x4fdf79c9`) | yes |
| `3` SW | requires `[this+0x4680] != 0`; cached ptr `[rec+0x3428]` (`0x4fdf7a0e`) | yes |
| `1` HAL | requires `[this+0x4684] == 0`; then `*ppCaps = this + Adapter*0x2bc + 0x16c` and **returns `S_OK` unconditionally** (`0x4fdf7a46`–`0x4fdf7a5e`) | **only via `0x4684`** |
| other | `mov eax,0x8876086b` = `D3DERR_INVALIDDEVICE` (`0x4fdf7a63`) | n/a |

So for HAL there is exactly one gate: `[this+0x4684]`. That field is written
only inside `Direct3DCreate9` (`0x4fdfaed0`, export ordinal 14): cleared to zero
at `0x4fdfae42`, and set to `1` at `0x4fdfae99` only when a registry read
succeeds:

```
RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\Microsoft\Direct3D\Drivers", ...)
RegQueryValueExA(hKey, "SoftwareOnly", ...)   ; type must be REG_DWORD(4), data != 0
```

This is a machine-wide policy switch that lives in `d3d9.dll` alone — it would
block D3D9 HAL while leaving D3D7/D3D8 working, which matches our symptoms
exactly. It was therefore checked directly against the guest's own hive:

```
$ python3 tools/check-d3d9-softwareonly.py
hive bytes: 9699328
occurrences of 'SoftwareOnly': 1
  vk @0x8f7374 name_len=12 type=4 inline=True len=4 VALUE=0x0
  RESULT: SoftwareOnly = 0x0 -> HAL allowed
```

The value exists and is **zero**. `[this+0x4684] == 0`, so the HAL branch
returns `S_OK`. **`0x4fdf7970` is not the source of our `0x8876086a`.**

Two neighbouring functions were also misread earlier and are corrected here:

- `0x4fdf7920` is **not** a device-type validator. It is
  `IsValidDisplayFormat(D3DFORMAT)`: `fmt - 20 <= 15`, i.e. formats `20..35`
  (`D3DFMT_R8G8B8` .. `D3DFMT_A2R10G10B10`), via a byte index table at
  `0x4fdf7958` and a jump table at `0x4fdf7950`.
- `0x4fdf78f0` is the adapter-ordinal validator.

The `ret 0x18` function at `0x4fdf8d40` is `CheckDeviceFormat`-shaped and
returns `NOTAVAILABLE` when either validator fails (`0x4fdf8d87`); it is not
`GetDeviceCaps` either.

### What this eliminates, and what remains

Eliminated: the entire caps-accessor family, plus the `SoftwareOnly` policy.
That is four of the thirty-one `0x8876086a` sites, including all three that the
previous section proposed.

Remaining: the public `IDirect3D9::GetDeviceCaps` has still not been located.
It is *not* the `0x130`-byte copy at `0x4fe497ca` — that belongs to the DDI
query helper family around the handshake at `0x4fe03bc4`. Locating the
`IDirect3D9` vtable in `.rdata` remains the way to settle this; the caller-set
heuristics tried so far (callers of the accessor, `0x130` copies) have each
landed on a different function.

## The actual gate: `D3DFORMAT_OP_3DACCELERATION`

The vtable was located, and with it the real decision site. This section
supersedes the search above.

### Finding the vtable

This binary has no `.rdata`; read-only data is merged into `.text`, so a scan
restricted to data sections finds nothing. Scanning `.text` for runs of code
pointers yields 88 candidates, almost all jump tables. The discriminator that
works is that a COM slot points at a *function entry*, which in XP system
binaries carries the hotpatch prologue `8b ff 55 8b ec` (`mov edi,edi; push
ebp; mov ebp,esp`); jump-table targets sit mid-function and fail it. That
reduces the field to 21 candidates, and the first has exactly 17 slots — the
`IDirect3D9` method count — all pointing into `0x4fdf6cd0`–`0x4fdf9e40`:

```
vtable @ 0x4fdd1a98
  [10] 0x4fdf88d0   CheckDeviceFormat
  [14] 0x4fdf96e0   GetDeviceCaps
```

`tools/find-d3d9-vtable.py` reproduces this. Slot 14 is confirmed by shape
(`ret 0x10`, i.e. `this` + three stack args) and by containing the call to the
internal accessor at `0x4fdf9742`.

### The decision

`IDirect3D9::GetDeviceCaps` (`0x4fdf96e0`):

1. `Adapter >= [this+0x2c]` (adapter count) → `D3DERR_INVALIDCALL` (`0x4fdf972b`).
2. Call the internal accessor `0x4fdf7970`; on failure zero `0x4c` dwords of
   `pCaps` and propagate (`0x4fdf9747`).
3. **Scan the adapter's format-operations list.** Count at `[caps+0x158]`,
   array at `[caps+0x15c]`, element stride `0x6c`, flags dword at element
   `+0x58`. The test is `test BYTE PTR [ecx+1],0x8` on the byte at `+0x59`,
   i.e. bit 11 of the flags dword — `0x800`.
4. If **no** element has that bit, zero `0x4c` dwords of `pCaps` and return
   `mov eax,0x8876086a` at `0x4fdf97b3` — `D3DERR_NOTAVAILABLE`.
5. Otherwise fall through to the success path at `0x4fdf97bf`.

`0x800` is `D3DFORMAT_OP_3DACCELERATION` (`ddrawi.h:174`, `ddrawint.h:130`,
and `FORMATOP_3DACCELERATION` in `d3dumddi.h:2118`).

> **Proven contract.** `IDirect3D9::GetDeviceCaps(Adapter, D3DDEVTYPE_HAL, ...)`
> returns `D3DERR_NOTAVAILABLE` if and only if no entry in the adapter's
> format-operations list advertises `D3DFORMAT_OP_3DACCELERATION`.

This is the first driver-controlled, falsifiable condition found for the HAL
failure. Every earlier candidate was either version-independent or
unreachable.

### What this predicts

The driver already reports the bit. `qxp_fill_operation_format` sets
`D3DFORMAT_OP_3DACCELERATION` on `X8R8G8B8` (`directdraw.c:13546`) and
`R5G6B5` (`directdraw.c:13562`), and the measured `GETFORMAT` records confirm
it reaches the runtime: records 0 and 2 both decode as
`0x4c1d = T|C|ORT|SRT|D|3D|P` (`docs/design/d3d9-caps-audit.md:133,135`).

So the list *content* is right, and the remaining question is whether the list
is *populated at all* in the failing configuration. `GETFORMAT` is a D3D9-DDI
query; if the runtime does not issue it when the driver declines DDI v4, the
operations list is empty, no element carries `0x800`, and step 4 fails. That
is consistent with every observation to date: D3D9 HAL has never worked, the
failure is insensitive to capability *content*, and `GETFORMAT` records were
only ever measured under v4 probes.

This yields a concrete falsifiable prediction rather than another hypothesis
about intent: **with DDI v4 advertised and the 27 `GETFORMAT` records
delivered, the `0x800` scan should pass and `GetDeviceCaps(HAL)` should
succeed.** The prior v4 experiments that still failed are not yet evidence
against this, because they were not instrumented at this specific scan; they
varied capability content, which step 4 does not read. The next test should
measure the count at `[caps+0x158]` and whether `GETFORMAT` is issued at all,
rather than varying caps.

### The list builder validates, and discards everything on failure

The list is written at `0x4fdfc3f6` (`[adapter+0x15c]` = array) and
`0x4fdfc403` (`[adapter+0x158]` = count), at the end of a builder loop that
walks the driver's records at stride `0x6c` from `0x4fdfc282`. Two other
sites touch these fields but do not originate them: `0x4fdf75f0` is the
destructor, and `0x4fdf7310` is a clone that `rep movs` `0x5e` dwords (`0x178`,
the per-adapter record stride) and reallocates `count * 0x6c`.

The builder maintains an error flag in `[ebp-0x4]`. At `0x4fdfc3d6` it takes
the success path (store the list) only when that flag is **zero**; otherwise it
frees the buffer and bails, which leaves the adapter with **no format list at
all** — and therefore no `0x800`, and therefore `NOTAVAILABLE`. So any single
rejected record discards the whole list, not just that record.

Checks decoded so far, all cross-record consistency rules rather than
per-record capability rules:

| Site | Rule | QXP status |
| --- | --- | --- |
| `0x4fdfc2cc` | if `R5G6B5(23)` has `OP_DISPLAYMODE(0x400)`, then `X1R5G5B5(24)` must not also have it | passes — our `X1R5G5B5` is `0x4005 = T\|C\|P`, no `D` |
| `0x4fdfc2f5`, `0x4fdfc32d`, `0x4fdfc33d`, `0x4fdfc34d` | a depth format `D15S1(73)`, `D24S8(75)`, `D24X8(77)`, `D24X4S4(79)` must not be reported alongside its legacy mirror `S1D15(72)`, `S8D24(74)`, `X8D24(76)`, `X4S4D24(78)` | passes — we report only `D16(80)`, `D24S8(75)`, `D24X8(77)` |
| `0x4fdfc36e` | a record carrying `D3DFORMAT_MEMBEROFGROUP_ARGB(0x80000)` must have a format in the ARGB group `{20,21,22,23,24,25,31,32,33,35,36,113,116}` | passes vacuously — no QXP record sets `0x80000` |

**Partial:** the loop from `0x4fdfc282` has further branches that were not
decoded, including one at `0x4fdfc307` that stores `0x2000000` into
`[adapter+0x8]` and bails. The three rules above are the ones read end to end;
the list is not exhaustive, and no claim is made that QXP passes checks that
have not been read.

This matters for test design: because a single bad record discards everything,
the symptom of a rejected record is identical to the symptom of a list that was
never requested. Measuring `[caps+0x158]` distinguishes "empty because never
populated" from "empty because validation rejected it", but not which record
was at fault — for that, the format-subset probe already in
`docs/design/d3d9-caps-audit.md:176` is the right instrument.

### Refuted: the list is delivered, and HAL still fails

The prediction above — that delivering the 27 `GETFORMAT` records under v4
would satisfy the `0x800` scan — is **false**, and existing evidence already
showed it. In `artifacts/runs/20260725-ddi4-gdi2-enumeration-t2/D3d9/driver.log`
(DDI v4 advertised: 6 × `QXPDISP GD9 DIAG4`, 6 × `QXPDISP G9`):

```
6  QXPDISP FC     GETFORMATCOUNT issued
81 QXPDISP F      = 3 x 27 records, the complete list, three times
```

and the probe still reports

```
QXP_D3D9_RESULT rc=6 hr=0x8876086a stage=hal-caps caps2=00000000 devcaps=00000000 ...
```

The all-zero capability block is the fingerprint of the step-4 path
(`rep stos` of `0x4c` dwords at `0x4fdf9799`, then `0x4fdf97b3`), and
`qxpd3d9probe.c:650` confirms the failing call is exactly
`IDirect3D9::GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps)`. So the
records are requested and answered in full, and the scan still finds no `0x800`.

### Two further rules, both of which QXP satisfies

Re-reading the loop head at `0x4fdfc29d` (records carry format at `+0x50`, bit
count at `+0x54`, operations at `+0x58`, matching the scan):

| Site | Rule | QXP status |
| --- | --- | --- |
| `0x4fdfc2a9` | a record with `OP_3DACCELERATION(0x800)` **must** also carry `OP_DISPLAYMODE(0x400)` | passes — both `0x800` records are `0x4c1d`, which contains `0x400` |
| `0x4fdfc2b9` | a record with `OP_3DACCELERATION(0x800)` **must not** be `A8R8G8B8(21)` or `A1R5G5B5(25)` | passes — our `0x800` records are `X8R8G8B8(22)` and `R5G6B5(23)` |

These are genuine general-contract rules and QXP is compliant with them, as it
is with the three rules in the previous table. Five rules are now read end to
end and none rejects our list.

### Where that leaves it

The list is delivered in full, satisfies every validation decoded so far, and
the scan still fails. The remaining explanations, in order of how cheaply they
can be discriminated:

1. The builder never runs for the **HAL** capability block. The HAL blocks are
   based at `this+0x16c` with stride `0x2bc`; the REF blocks are a different
   array (`this+0x2130`, stride `0x178`, cached pointer at `+0x158` within the
   block). If the builder populates only one of these, the HAL scan reads an
   untouched field.
2. An undecoded branch of the loop sets the error flag — `0x4fdfc307` remains
   unread, and stores `0x2000000` into `[adapter+0x8]` before bailing.
3. The delivered records reach a different consumer than this list.

Measuring the count at `[caps+0x158]` no longer distinguishes 1 from 2, but it
does separate {1,2} from 3, and it is still the cheapest next measurement.

### The builder writes the same object GetDeviceCaps scans

Explanation 1 is eliminated. The builder is `0x4fdfb500`, called from
`0x4fdf75f0`, which first frees and zeroes `+0x158`/`+0x15c` (and the sibling
arrays at `+0x164`/`+0x168` and `+0x170`/`+0x174`), clears `[esi+0x1c]`, and
clears bit 1 of `[esi+0x154]` — the same "D3D9 caps acquired" bit the handshake
sets at `0x4fe03bd1`. So `esi` is the adapter record that `GetDeviceCaps`
receives from the accessor, and the builder writes exactly the fields the scan
reads. It is a free-then-rebuild, not a destructor.

This also fixes the record layout: the adapter record's **first `0x130` bytes
are the `D3DCAPS9` block**, so the builder's `[ebx+...]` accesses name caps
fields directly:

| Offset | `D3DCAPS9` field |
| --- | --- |
| `+0x08` | `Caps` |
| `+0x10` | `Caps3` |
| `+0xd4` | `DevCaps2` |

### Enforcement of `D3DDEVCAPS2_STREAMOFFSET`

At `0x4fdfc1a9` the builder reads `DevCaps2` and, if it is nonzero, branches to
`0x4fdfc307`:

```
if (DevCaps2 != 0) {
    if ((DevCaps2 & 1) == 0) { Caps = 0x2000000; return; }   /* no list built */
    if (Caps3 & 0x40)        { Caps = 0x2000000; return; }   /* no list built */
    /* else fall into the normal build */
}
```

Bit 0 is `D3DDEVCAPS2_STREAMOFFSET`. This is the mechanism behind the
`d3d9caps.h:536` comment "Must be set by DX9 drivers": a driver that reports
any `DevCaps2` bits **without** `STREAMOFFSET` has its entire format list
silently discarded, and every later `GetDeviceCaps(HAL)` fails with
`D3DERR_NOTAVAILABLE`. The failure is reported nowhere near the offending
capability.

This matters for G02. QXP currently reports `DevCaps2 = 0`, so this bail is
**skipped** and it is not the current failure. But it constrains how G02 must
be done: `STREAMOFFSET` must be set in the same change as any other `DevCaps2`
bit, and `Caps3 & 0x40` must stay clear.

### Operation normalisation happens before validation

The first loop (`0x4fdfc240`) rewrites each record's operations in place before
any rule is applied:

| Condition | Effect |
| --- | --- |
| `ops & 0x8` (`OFFSCREEN_RENDERTARGET`) | `ops \|= 0x110` (`SAME_FORMAT_RT` + `ART`) |
| `ops & 0x100` (`ART`) | `ops \|= 0x10` (`SAME_FORMAT_RT`) |
| `ops & 0x80` (`ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`) | `ops \|= 0x40` (`ZSTENCIL`) |

So the operations the rules see are not the operations the driver reported, and
reasoning about rule compliance must use the normalised values.

## Correction: D3D9 HAL is not broken; advertising v4 breaks it

Measured on the clean shipping (non-diagnostic) package, run
`artifacts/runs/20260725-format-list-observability-t2`, with a new
observational `format-list` stage added to `guest/acceptance/qxpd3d9probe.c`:

```
stage=format-list modecount=8 devtype=0x00000000 tex=0x00000000 rt=0x00000000
stage=hal-caps   hr=0x00000000 caps2=20000000 devcaps=0009af50 vs=fffe0101 ps=ffff0103 rts=1
QXP_D3D9_RESULT  rc=0 ... stage=complete ... ps_1_1=1 ps_1_2=1 ps_1_3=1 render-target=1
```

`GetDeviceCaps(D3DDEVTYPE_HAL)` **succeeds**, the adapter format list is
populated and healthy (`CheckDeviceType` and both `CheckDeviceFormat` probes
return `S_OK`, 8 enumerable modes), and the full D3D9 probe passes: device
creation, `ps_1_1`/`ps_1_2`/`ps_1_3`, render targets, and `Reset`. The D3d9
counters `21/0/10/20/0` match the documented baseline.

This corrects the framing used in the preceding sections of this document. The
`0x8876086a` failure is **not** a standing property of D3D9 on this driver. It
was observed only in the `QXP_DIAGNOSTIC_DDI4` builds, and it is *caused by*
advertising DDI v4. Earlier analysis treated a diagnostic-only failure as the
baseline state.

The corrected problem statement is therefore:

> Under DDI v3 the adapter format list is built from the legacy path, passes
> validation, and D3D9 works up to Shader Model 1.3 — but SM2 is unreachable.
> Advertising DDI v4, which is required to reach SM2, causes the runtime to
> discard the adapter format list, so `GetDeviceCaps(HAL)` fails and D3D9 stops
> working altogether.

Everything decoded above remains valid and is now pointed at the right
question: the `0x800` scan, the all-or-nothing builder, and its validation
rules describe exactly *how* the v4 path loses the list. What remains is to
find which rule the v4 operations list trips, remembering that the rules run on
the **normalised** operations, not the ones the driver reported.

The capability content under v3 is authored by the runtime from the caps8
block, so the caps9 `MaxStreams` divergence recorded elsewhere is latent rather
than active on this path.

## Second correction: advertising v4 does not break D3D9 either

A diagnostic package was rebuilt from current HEAD with `QXP_DIAGNOSTIC_DDI4=1`
(a `sources` gate, off by default, matching the existing `QXP_D3D_PROFILE`
pattern) so `GETDDIVERSION` answers `4`. Run
`artifacts/runs/20260725-format-list-ddi4-t2`, with `QXPDISP GD9 DIAG4` = 6 and
`QXPDISP GD9 FALLBACK` = 0:

```
stage=format-list modecount=8 devtype=0x00000000 tex=0x00000000 rt=0x00000000
stage=hal-caps   hr=0x00000000 caps2=20000000 devcaps=0009af50 vs=fffe0101 ps=ffff0103 rts=1
QXP_D3D9_RESULT  rc=0 ... stage=complete ... ps_1_1=1 ps_1_2=1 ps_1_3=1 render-target=1
```

Identical to the v3 run: the format list is healthy, `GetDeviceCaps(HAL)`
succeeds, and the full probe passes including device creation, `ps_1_1`
through `ps_1_3`, render targets and `Reset`. The caps9 dump is byte-identical
across the two runs, and the D3d9 counters are the same `21/0/10/20/0`.

So the `0x8876086a` failure is **not** caused by advertising v4 either. It was
an artifact of the older diagnostic builds, which changed capability content as
well as the DDI version. Both of this document's earlier framings were wrong,
and the whole `0x800`/builder investigation was chasing a defect that the
current driver does not have.

### The D3d9 mode still reports failure, for one honest reason

The harness computes `D3d9TransportOk` as a conjunction at
`qemu/harness/run-xp-acceptance.ps1:2599`, and one term is `hasDdiFallback`,
which requires the literal token `QXPDISP GD9 FALLBACK` in the debug log. Under
the diagnostic build that token is replaced by `QXPDISP GD9 DIAG4`, so the term
is false and the mode fails. Every other term — context, caps9, host session,
shader define, shader delete, draw, present, and all six failure predicates —
is satisfied.

That oracle encodes the current capability-honesty policy: *the driver must be
observed declining v4*. It is doing its job. When v4 is adopted for real (G01)
the oracle must be updated to assert the intended version rather than the
fallback — changed to match the new intent, not relaxed.

### What is still not established

The runtime queried `GETD3DCAPS8` six times in **both** runs, so that is not a
discriminator for whether the v4 handshake was accepted. Our caps9 is built by
copying the caps8 shader versions (`directdraw.c:13838`, `13843`), so the
identical caps9 dump cannot discriminate either. The two configurations are
observably identical apart from the `GD9` token.

So this run proves advertising v4 is **safe**, not that it is **effective**.
`ps20-create` still reports `skipped-not-advertised` and the runtime still
exposes `vs_1_1`/`ps_1_3`, so SM2 remains unreachable by a stock application.
Reaching it requires the caps9 block to advertise SM2 and the runtime to be
shown consuming it — which needs a discriminating measurement, not another
identical run.

## The discriminating measurement: the runtime ignores our caps9

A second diagnostic gate, `QXP_DIAGNOSTIC_SM2_CAPS`, was added independently of
`QXP_DIAGNOSTIC_DDI4` so the DDI version and the capability content stay
separately controllable — conflating them is what made the older diagnostic
builds uninterpretable. It sets `VertexShaderVersion = 0xFFFE0200`,
`PixelShaderVersion = 0xFFFF0200`, `MaxVertexShaderConst = 256`, and minimum
legal `VS20Caps`/`PS20Caps`. Both defines were confirmed present in
`buildfre_wxp_x86.log`, and the `ps_2_0` immediate is present in the packaged
binary.

Run `artifacts/runs/20260725-sm2-caps-discriminate-t2`, with
`QXPDISP GD9 DIAG4` = 6 and `QXPDISP G9` = 6:

```
caps9-dump caps9[48] 000000ff fffe0101 00000060 ffff0103 ...
stage=hal-caps  vs=fffe0101 ps=ffff0103
stage=ps20-create skipped-not-advertised
```

**Unchanged, and byte-identical to both previous runs.** The runtime asks for
our Direct3D 9 capability block six times, receives one advertising `vs_2_0`
and `ps_2_0`, and then reports `vs_1_1`/`ps_1_3` to the application.

> **Measured conclusion.** The runtime does not consume our `GETD3DCAPS9`
> block. The capabilities it exposes are derived from caps8 regardless of the
> DDI version we report.

This contradicts the conclusion recorded earlier in this document that "the v4
capability handshake already SUCCEEDS", which was inferred from the absence of
`GETD3DCAPS8` in a v4 query trace. That inference was wrong: `GETD3DCAPS8` is
queried six times in every run measured here, v3 and v4 alike.

### Why this is progress

The handshake at `0x4fe03ada` requires all of: the `GetDriverInfo2` thunk
returning `DDHAL_DRIVER_HANDLED` (`1`), `ddRVal == 0`, `dwDDIVersion == 4`, and
`dwActualSize == 0x130` for the caps9 reply. We satisfy the version and the
`0x130` size, so one of the remaining conditions, or an unread validation of
the block's *content*, is rejecting it.

The important gain is the oracle. Until now "is v4 active?" had no direct
signal. It now has two, both cheap and unambiguous:

1. `caps9-dump` word 49 / word 51 — if the runtime ever consumes our block,
   these become `fffe0200`/`ffff0200`.
2. `ps20-create` — leaves `skipped-not-advertised` the moment SM2 is really
   exposed.

Any future handshake change can be tested against these directly instead of
being argued from disassembly. That is the instrument the previous several
turns of static analysis lacked.

## Root cause: the GETDDIVERSION reply was malformed

Re-reading the handshake at `0x4fe03ada` shows the two request blocks the
runtime builds on its own stack:

| Query | `dwType` | `dwExpectedSize` | Extra |
| --- | --- | --- | --- |
| `GETD3DCAPS9` | `0x10` | `0x130` | — |
| `GETDDIVERSION` | `0x23` | **`0`** | `dwDXVersion = 9` at `+0x10` |

The acceptance test at `0x4fe03bb4` is
`cmp DWORD PTR [ebp-0xbf8],0x4` — offset `+0x14` of the GETDDIVERSION block.
That matches `DDNT_GETDDIVERSIONDATA` (`d3dnthal.h:1894`): a 16-byte
`gdi2` header, then `dwDXVersion` `[in]` at `+0x10`, then `dwDDIVersion`
`[out]` at `+0x14`.

Our handler answered it with `qxp_copy_result`, which is correct for every
*other* GetDriverInfo2 reply — those carry a real `dwExpectedSize` and the
driver overwrites the header with the payload. For GETDDIVERSION it is wrong
twice over:

1. `dwExpectedSize` is `0`, so `bytes = min(4, 0) = 0` and **nothing was
   written at all**.
2. Even with a nonzero size it would have written at offset `0`, over the
   header, not at the `[out]` member at `+0x14`.

So `dwDDIVersion` stayed `0`, the `== 4` test failed, and the runtime took the
failure branch at `0x4fe03c16` — which re-queries `GETD3DCAPS8`. That is the
source of the `GETD3DCAPS8` traffic seen in every earlier run.

The fix writes the member in place:

```c
((QXP_D3D9_DDI_VERSION_DATA *)data->lpvData)->dwDDIVersion = 4;
```

### Measured effect

Run `artifacts/runs/20260725-ddi4-handshake-fix-t2`:

| Token | before | after |
| --- | ---: | ---: |
| `QXPDISP GD9 DIAG4` | 6 | 6 |
| `QXPDISP G9` | 6 | 6 |
| **`QXPDISP G8`** | **6** | **0** |

`GETD3DCAPS8` drops to zero. The runtime stops falling back and stays on its
native Direct3D 9 DDI stream. **This is the first time the v4 capability
handshake has been shown to succeed.**

### Correction, again: v4 is not "safe"

The previous section concluded that advertising v4 was safe. That conclusion
was an artifact of v4 never actually being active. With the handshake genuinely
accepted, the same probe reports:

```
stage=format-list modecount=8 devtype=0x8876086a tex=0x8876086a rt=0x8876086a
stage=hal-caps   hr=0x8876086a caps2=00000000 devcaps=00000000 vs=00000000 ps=00000000
QXP_D3D9_RESULT  rc=6 stage=hal-caps
```

`CheckDeviceType` and both `CheckDeviceFormat` probes now fail with
`D3DERR_NOTAVAILABLE`, while `GetAdapterModeCount` still returns 8. The
adapter format-operations list is gone.

And it is not for want of asking: the same run recorded `QXPDISP FC` = 6 and
`QXPDISP F` = 81 = 3 x 27. **The runtime fetched every one of our 27 format
records and then discarded the list.**

That is exactly the all-or-nothing rejection decoded earlier in this document,
and everything in the `0x800` scan and builder-validation sections is now live
and directly relevant rather than describing a phantom.

### The well-posed remaining problem

One of the builder's validations rejects our operations list. The five rules
decoded end to end all appear satisfied, including after the documented
operation normalisation (`ORT` implies `0x110`, `ART` implies `0x10`, `ZA`
implies `0x40`):

- both `0x800` records (`X8R8G8B8`, `R5G6B5`) also carry `0x400`;
- neither `0x800` record is `A8R8G8B8(21)` or `A1R5G5B5(25)`;
- `X1R5G5B5` does not claim `DISPLAYMODE`;
- no depth format is paired with its legacy mirror;
- no record sets `MEMBEROFGROUP_ARGB`.

So an undecoded branch of the loop from `0x4fdfc282` is responsible. The
efficient next step is empirical rather than more disassembly: bisect the 27
records using the truthful-subset method already sketched in
`docs/design/d3d9-caps-audit.md:176`, with `format-list` and `hal-caps` as the
oracle. Removing support is conservative and safe; adding unsupported
operations is forbidden.

### The subset experiment: record content is probably not the cause

`QXP_DIAGNOSTIC_FORMAT_SUBSET` advertises only the truthful nine-record core —
the two display formats, the alpha render-target format, the three depth
formats and the three buffer formats. It only removes claims; no record is
altered and nothing unsupported is added.

Run `artifacts/runs/20260725-format-subset-bisect-t2`, with the v4 handshake
fix also in place:

```
QXPDISP F  = 27   (= 3 x 9, down from 81 = 3 x 27, so the subset was active)
QXPDISP G8 = 0    (handshake still accepted)

stage=format-list modecount=8 devtype=0x8876086a tex=0x8876086a rt=0x8876086a
stage=hal-caps   hr=0x8876086a
```

**Unchanged.** The reduced core list is rejected exactly like the full one.

That is a meaningful negative. Combined with the five decoded rules all being
satisfied, and the record layout being confirmed correct
(`dwPrivateFormatBitCount` at `+0x0c` and `dwOperations` at `+0x10` of
`DDPIXELFORMAT`, which land on the runtime record's `+0x54` and `+0x58`), the
"one malformed record poisons the list" hypothesis is now weak. Bisecting
further is unlikely to pay.

The more probable reading is that under a genuine v4 handshake the adapter
never completes initialisation for some *other* reason, and the empty format
list is a symptom rather than the cause. Candidates, all of which are v4-only
and all of which QXP currently answers with zeroes:

| Query | Token | QXP answer |
| --- | --- | --- |
| `GETEXTENDEDMODECOUNT` | `EM9` | zeroed, `dwModeCount = 0` |
| `GETD3DQUERYCOUNT` | `QC9` | zeroed, `dwNumQueries = 0` |
| `GETADAPTERGROUP` | `AG9` | `ulUniqueAdapterGroupId = 1` |

Under v3 these were never consulted, so their content has never been exercised.
An extended mode count of zero is the most suspicious: the v3 path reports 8
enumerable modes through the legacy route, `GetAdapterModeCount` still returns
8 under v4, but a v4 adapter that reports zero *extended* modes may fail to
finish building its format and mode tables.

The next measurement should therefore vary those three replies rather than the
format records.

### Eliminations after the handshake fix

With the v4 handshake genuinely accepted (`G8` = 0 in all of these), the
following have each been ruled out:

| Hypothesis | How eliminated | Result |
| --- | --- | --- |
| A malformed operation-format record poisons the list | `QXP_DIAGNOSTIC_FORMAT_SUBSET`, nine truthful records (`F` 81 -> 27) | `format-list` and `hal-caps` unchanged |
| `GETFORMAT` / `GETFORMATCOUNT` replies are misplaced like `GETDDIVERSION` was | static: their `[out]` members sit at `+0x10`/`+0x14`, which the full-struct copy from offset 0 does land on; and changing the advertised count demonstrably changed `F` | replies are read correctly |
| The runtime issues a v4 query we do not handle | `GDI2 UNKNOWN` count in the v4 runs | **0** — every query answered |
| Missing `D3DDEVCAPS2_STREAMOFFSET`, which `d3d9caps.h:536` says DX9 drivers must set | `QXP_DIAGNOSTIC_STREAMOFFSET` sets `DevCaps2 = 1` | `format-list` and `hal-caps` unchanged (`artifacts/runs/20260725-streamoffset-devcaps2-t2`) |

So: the handshake is accepted, every query is answered, all 27 records are
fetched, the records are well formed and satisfy every decoded rule, reducing
them to a truthful core changes nothing, and the one documented capability
requirement we were violating is not enforced here.

The adapter still ends v4 initialisation with an empty operation-format list
while its *mode* list is intact (`GetAdapterModeCount` returns 8 throughout,
and that list lives in the separate `+0x164`/`+0x168` pair). Whatever fails is
therefore specific to the operations list and is not any of the above.

The untested v4-only replies remain `GETEXTENDEDMODECOUNT` (`dwModeCount = 0`)
and `GETD3DQUERYCOUNT` (`dwNumQueries = 0`); both are answered with zeroes and
neither has ever been exercised, because the v3 path never consulted them.

### `GETEXTENDEDMODECOUNT` is tolerated

Decoded at `0x4fe03fb1`: the request carries `dwExpectedSize = 0`, and the
reply must satisfy `dwActualSize == 0` or the runtime takes the fatal
size-mismatch exit at `0x4fe04c10` — the same exit the caps9 `0x130` check
uses. Our handler passes `expected = 0` to `qxp_copy_result`, which copies
`min(size, 0) = 0` bytes and reports `dwActualSize = 0`, so it satisfies the
check by accident. A zero mode count then simply skips the extended-mode
enumeration (`0x4fe04023`) rather than failing. So this reply is not the cause.

### A second normalisation pass, and a refuted hypothesis

There is a second normalisation pass at `0x4fdfbd67`, before the one already
documented:

```
for each record carrying OP_3DACCELERATION (0x800):
    if lookup(format, list, count, OP_DISPLAYMODE) != 0:  ops |= 0x400
    else:                                                 ops &= ~0x400
```

So `OP_DISPLAYMODE` is **recomputed**, not taken from the driver. Combined with
Rule A at `0x4fdfc2a9` — which rejects any record carrying `0x800` without
`0x400` — this means a format advertised as an accelerated display format that
is not actually enumerable would have its `0x400` stripped and would then
poison the entire list.

QXP claims both bits on `X8R8G8B8` and `R5G6B5`, so this predicted that one of
them enumerates no modes. The probe's `format-list` stage was extended to
report `GetAdapterModeCount` for `R5G6B5` alongside `X8R8G8B8`, and
`artifacts/runs/20260725-modecount16-observe-t2` reports:

```
stage=format-list modecount=8 modecount16=8 devtype=0x8876086a ...
```

Both enumerate 8 modes. **The hypothesis is refuted** — both accelerated
display formats are genuinely enumerable, the lookup finds them, and their
`0x400` bits survive the recomputation. The extra mode count is kept as a
permanent observation since it is exactly the quantity this rule depends on.

## The builder's entry gate and its first bail

The previously unread head of `0x4fdfb500` turns out to matter more than the
validation loop everything so far has focused on.

The builder issues **no** `GetDriverInfo2` calls of its own — there is no call
to the `0x4fe4f678` thunk anywhere in it. It receives the already-fetched
records. So `GETFORMATCOUNT`/`GETFORMAT` traffic proves the records were
collected, but says nothing about whether the builder ran.

Its entry gate is at `0x4fdfb580`:

```
esi = 0
if ((adapter[0x154] & 3) != 0 && adapter[0x1c] != 0)   /* caps acquired && DevCaps */
    esi = 1
count = 0
if (esi == 0) goto 0x4fdfb651        /* skip building formats entirely */
```

`[adapter+0x1c]` is `D3DCAPS9.DevCaps`. QXP copies it from caps8
(`directdraw.c:13821`), so it is nonzero and this gate passes.

Immediately after, at `0x4fdfb5ac`:

```
if (!helper_0x4fe014f0(obj, &out)) goto 0x4fdfc537;   /* return, no list built */
```

`0x4fdfc537` is the bare function epilogue and `0x4fdfc4fa` is the cleanup
path; both leave the adapter with no operation-format list. That is a third
bail, earlier than the DevCaps2 bail and the validation error flag already
documented.

The helper resolves to an imported thunk:

```
0x4fe014f0 -> 0x4fe4f6a8 -> jmp [0x4fdd1308]
0x4fdd1308 = d3d8thk.dll!OsThunkDdReenableDirectDrawObject
```

It succeeds if that call returns nonzero, and otherwise falls back to accepting
the object when `[obj+0x94]` is `2` or `3` **and** `[obj+0x84] & 0xfdffffff`
is zero.

This is the first bail on the path that is reached through a **driver-serviced
DirectDraw entry point** rather than through capability content, which makes it
the most promising remaining lead: everything ruled out so far has been
capability data, and this is behaviour. The next step is to instrument the
driver's DirectDraw re-enable path and compare what it returns under v3 and
under an accepted v4 handshake.

### Corroboration: v4 drives a DirectDraw re-enable cycle, v3 does not

Comparing driver-log token histograms between the passing v3 run
(`20260725-format-list-observability-t2`) and the failing v4 run
(`20260725-ddi4-handshake-fix-t2`) shows the v4 run issuing **far more**
DirectDraw traffic despite aborting much earlier, at `hal-caps`, while the v3
run completed the entire probe:

| Token | meaning | v3 (pass, full run) | v4 (fail, early abort) |
| --- | --- | ---: | ---: |
| `QXPDISP DD` | `DrvGetDirectDrawInfo` | 4 | **16** |
| `QXPDISP C` | surface create | 1 | **10** |
| `QXPDISP S` | | 2 | **8** |
| `QXPDISP P` | | 2 | **8** |
| `QXPDISP DX` | **`DrvDisableDirectDraw`** | **0** | **3** |

The decisive one is `QXPDISP DX` (`directdraw.c:14285`): `DrvDisableDirectDraw`
is called three times under v4 and **never** under v3 during this phase. The
surrounding log context is a repeating cycle:

```
QXPDISP C
QXPDISP C
QXPDISP DX      <- DirectDraw disabled
QXPDISP DX9     <- DXVERSION re-queried
```

So under an accepted v4 handshake the runtime tears the DirectDraw object down
and brings it back up repeatedly, which is exactly the behaviour
`DdReenableDirectDrawObject` exists to service, and exactly the call whose
failure returns from the builder before any format list is created.

This is independent corroboration of the static trace, from data already
collected, and it converts the lead from "a bail that exists in the
disassembly" into "a bail on a path the runtime demonstrably takes only in the
failing configuration". The instrumentation to add next is a logged return
value on `DrvEnableDirectDraw` and on each failure return of
`DrvGetDirectDrawInfo`, since those are what a re-enable actually exercises.

### Measured: the driver never refuses the re-enable

Both silent `FALSE` returns were given log tokens (`QXPDISP DD-` for
`DrvGetDirectDrawInfo`, `QXPDISP DE-` for `DrvEnableDirectDraw`) and the v4
configuration was re-run as
`artifacts/runs/20260725-ddraw-reenable-probe-t2`:

```
QXPDISP DD-        0      <- DrvGetDirectDrawInfo never returned FALSE
QXPDISP DE-        0      <- DrvEnableDirectDraw never returned FALSE
QXPDISP DD         8
QXPDISP DE+        4
QXPDISP DX         3
QXPDISP GD9 DIAG4  6
```

`hal-caps` still fails. So the DirectDraw re-enable cycle happens, the driver
services every call, and **every driver entry point involved succeeds**. If
`DdReenableDirectDrawObject` is failing, it is not because this driver refused
it. That eliminates the lead.

**Correction to the histogram comparison above.** This v4 run reports
`QXPDISP DD` = 8 where the earlier v4 run reported 16, with no relevant change
between them. Those counts therefore vary run to run, and the `DD` 4-vs-16,
`C` 1-vs-10, `S` and `P` rows in the preceding table are not a reliable signal.
The only difference in that comparison that holds up is the qualitative one:
`QXPDISP DX` is present under v4 (3) and absent under v3 (0), i.e. the
DirectDraw object is torn down and re-enabled only in the v4 configuration.
That much is still true; the magnitudes are not evidence.

### Where this leaves the v4 blocker

Everything reachable from the driver side has now been measured and excluded:
the handshake is accepted, no query goes unanswered, every format record is
fetched, records are well formed and satisfy every decoded validation rule,
reducing them to a truthful core changes nothing, the documented
`STREAMOFFSET` requirement is not enforced, both accelerated display formats
are genuinely enumerable, and no DirectDraw entry point ever fails.

The adapter still finishes v4 initialisation with an empty operation-format
list. No remaining hypothesis is supported by evidence, so the next attempt
should not be another guess. The two options with actual information content
are a kernel-side trace of `DdReenableDirectDrawObject`'s return value, or
single-stepping the builder from `0x4fdfb5ac` under a debugger to observe which
bail is actually taken — rather than inferring it from the outside.

## The capability-gather contract

The builder has **three** early exits to the bare epilogue `0x4fdfc537`, not
one. Only the middle one was examined earlier:

| Site | Condition |
| --- | --- |
| `0x4fdfb576` | a pointer produced by `0x4fe04c50` is null |
| `0x4fdfb5b3` | the `DdReenableDirectDrawObject` wrapper returns 0 |
| `0x4fdfb608` | **`0x4fe036c0` returns 0** |

`0x4fe036c0` is the whole capability-and-format gather routine: the v4
handshake at `0x4fe03ada`, the format queries, the extended-mode queries and
the adapter-group query all live inside it. If it returns 0 the builder
returns immediately and the adapter is left with no operation-format list.

It has a single failure exit, `0x4fe04c10`, which is `xor eax,eax; ret 0x3c`.
Fourteen branches reach it. The size-checked ones give a hard contract that is
not documented anywhere in the DDK headers:

| `dwType` | Query | Required `dwActualSize` | QXP reply size |
| ---: | --- | ---: | --- |
| `0x04` | `DXVERSION` | — | `0x18` |
| `0x10` | `GETD3DCAPS9` | `0x130` | `0x130` ok |
| `0x01` | `GETD3DCAPS8` | `0xd4` | fallback path only |
| `0x02` | `GETFORMATCOUNT` | `0x18` | `0x18` ok |
| `0x03` | `GETFORMAT` | `0x34` | `0x34` ok |
| `0x11` | `GETEXTENDEDMODECOUNT` | `0x18` | `0x18` ok |
| `0x12` | `GETEXTENDEDMODE` | `0x24` | never issued (count 0) |
| `0x13` | `GETADAPTERGROUP` | `0x1c` | `0x1c` ok |
| `0x21` | `GETD3DQUERYCOUNT` | `0x14` | `0x14` ok |
| `0x22` | `GETD3DQUERY` | `0x14` | never issued (count 0) |

> **Contract.** A reply whose `dwActualSize` does not exactly equal the size
> the runtime asked for aborts the entire capability gather, and the adapter
> silently ends up with no format list. The failure surfaces much later as
> `GetDeviceCaps(HAL)` returning `D3DERR_NOTAVAILABLE`, with nothing pointing
> back at the offending reply.

This is the same failure mode as the `GETDDIVERSION` bug already fixed, and it
is worth recording precisely because the symptom is so far removed from the
cause. Every query QXP actually answers matches its required size, so this is
not the current defect — but it is a trap for any future reply.

After the D3DGDI2 queries the routine makes one further call at `0x4fe04416`
that is **not** a D3DGDI2 query: a plain GUID `GetDriverInfo` with
`dwExpectedSize = 0x74` and `ddRVal` preset to `E_FAIL`, requiring the thunk to
return `DDHAL_DRIVER_HANDLED` and `ddRVal` to be cleared. The GUID bytes at
`0x4fdd1e34` decode to `{7de41f80-9d93-11d0-89ab-00a0c9054129}`, which is
exactly QXP's `qxp_guid_extended_caps` (`GUID_D3DExtendedCaps`,
`directdraw.c:669`).

**Loose end, recorded rather than assumed:** QXP's handler for that GUID logs
`QXPDISP GX`, and that token appears **zero** times in both the passing v3 run
and the failing v4 run. So either the query never reaches this driver in either
configuration, or it is serviced without passing through that handler. Since
both configurations behave identically here, it does not explain the v4
regression, and no conclusion is drawn from it.

### The gather completes; the query sequence is not the difference

The driver logs already record the query order, so where the gather stops can
be observed without a debugger. Collapsing the per-record `F` runs, every pass
in the failing v4 run is:

```
DX9 -> G9 -> GD9 DIAG4 -> FC -> [F..] -> EM9 -> AG9 -> QC9
```

and every pass in the passing v3 run is the same with the caps8 fallback
inserted:

```
DX9 -> G9 -> GD9 FALLBACK -> G8 -> FC -> [F..] -> EM9 -> AG9 -> QC9
```

Both run the gather to completion of every D3DGDI2 query and both stop at
`QC9`. So the gather is not aborting early in the failing configuration, and
the query sequence is not the difference. Whatever diverges does so on
capability *content*.

### Which capability words the builder actually reads

The builder reads 32 words of the capability block: `0`, `2`-`21`, `34`-`36`,
`39`, `44`, `53`, `54`, `61`, `62`, `66`, `71` — i.e. `DeviceType`, `Caps`,
`Caps2`, `Caps3`, the whole `PresentationIntervals`..`LineCaps` run,
`StencilCaps`, `FVFCaps`, `TextureOpCaps`, `VertexProcessingCaps`,
`MaxPointSize`, `DevCaps2`, `MaxNpatchTessellationLevel`,
`StretchRectFilterCaps`, `VS20Caps.Caps`, `PS20Caps.Caps` and
`VertexTextureFilterCaps`.

Two useful consequences:

- It does **not** read `MaxStreams` (word 47), so the known caps9
  `MaxStreams = 1` divergence is a real defect but not this one.
- Every field QXP leaves unassigned (`CursorCaps`, `VolumeTextureFilterCaps`,
  `VolumeTextureAddressCaps`, `MaxNpatchTessellationLevel`,
  `StretchRectFilterCaps`, `VertexTextureFilterCaps`) is also zero in the
  runtime's own working v3 block, so no read word is accidentally left unset.

### The capability-honesty masks are not the cause

That left the six honesty masks as the one place where QXP's block knowingly
differs from the runtime's own caps8-to-caps9 derivation — the runtime's
`RasterCaps` is `0x216191`, which retains the `0x4000` bit QXP clears, and the
builder reads `RasterCaps`, `ShadeCaps`, `Caps2` and `DevCaps`.

`QXP_DIAGNOSTIC_NO_CAPS_MASK` skips all six masks. Run
`artifacts/runs/20260725-caps-mask-v4-t2` (`GD9 DIAG4` = 6, `G8` = 0, `FC` = 6,
`F` = 81):

```
stage=format-list modecount=8 modecount16=8 devtype=0x8876086a tex=0x8876086a rt=0x8876086a
stage=hal-caps   hr=0x8876086a
```

**Unchanged.** The capability-honesty corrections are not what the v4 builder
rejects, and they stand as written.

### Declined GUID queries are now attributable

The GUID chain in `QxpGetDriverInfo` ended with a bare
`return DDHAL_DRIVER_HANDLED;`, so every GUID this driver declines left no
trace — the same silent-failure shape already corrected for
`DrvGetDirectDrawInfo` (`DD-`) and `DrvEnableDirectDraw` (`DE-`). That matters
here specifically because a declined query leaves the caller's `ddRVal`
untouched, and the capability gather preloads that field with `E_FAIL` and
treats it as fatal, so an unanswered GUID can abort adapter initialisation with
no diagnostic anywhere.

Declining now logs `QXPDISP GUID-` followed by the leading GUID word. This is
permanent observability, not a diagnostic gate. Run
`artifacts/runs/20260725-declined-guid-v4-t2` records ten distinct declined
GUIDs, four times each:

| `Data1` | Query |
| --- | --- |
| `0xefd60cc0` | `GUID_MiscellaneousCallbacks` |
| `0xefd60cc2` | `GUID_VideoPortCallbacks` |
| `0x6fe9ecde` | `GUID_NTCallbacks` |
| `0xb1122b40` | `GUID_MotionCompCallbacks` |
| `0x42e02f16` | `GUID_UpdateNonLocalHeap` |
| `0x3b8a0466` | `GUID_DDMoreSurfaceCaps` |
| `0xfad16a23` | `GUID_NTPrivateDriverCaps` |
| `0xffaa7540`, `0x880baf30`, `0x80863800` | further optional DirectDraw extensions |

All are optional DirectDraw extension interfaces; declining them is correct for
this driver and none is required to build an operation-format list.

**The extended-caps thread is closed.** `GUID_D3DExtendedCaps`
(`0x7de41f80`) is **absent** from the declined list, and `QXPDISP GX` never
fires either. The runtime therefore never asks this driver for extended caps in
either configuration, so the `dwExpectedSize = 0x74` call at `0x4fe04416` is
serviced without reaching the display driver and cannot be the v4 failure. This
is now established by measurement rather than left as an open loose end.

## Reproducing this analysis

```
qemu-img convert -O raw <base>.qcow2 /tmp/xp.raw     # read-only on the source
7z x /tmp/xp.raw -o/tmp/part                          # extracts the NTFS tree
objdump -d -M intel /tmp/part/WINDOWS/system32/d3d9.dll > /tmp/d3d9.asm
```

No VM may be running. The extraction touches only reads on the qcow2 chain.

The same extraction yields the registry hive, which
`tools/check-d3d9-softwareonly.py` parses offline (it locates the `vk` record 20
bytes before the value name and decodes the inline REG_DWORD):

```
python3 tools/check-d3d9-softwareonly.py /tmp/part/WINDOWS/system32/config/software
```

## Correction, then confirmation: the list really is unpopulated

Earlier sections describe the v4 failure as the adapter ending initialisation
with an **empty** operation-format list. That was asserted on a flawed
inference. It has now been retested with a designed discriminator and the
conclusion survives, but the reasoning behind it has changed and is worth
recording, because the flawed step nearly sent the search in the wrong
direction.

### The flawed inference

The original argument was that the probe's `tex` and `rt` stages, which call
`CheckDeviceFormat`, also return `0x8876086a`, so a populated list would have
answered at least one of them.

Decoding `CheckDeviceFormat` (`0x4fdf88d0`) shows that does not follow. After
validating the adapter ordinal (`[edi+0x2c]`), the device type (only 1, 2 and 3
are accepted) and the display format (`IsValidDisplayFormat` at `0x4fdf7920`),
it dispatches through a jump table at `0x4fdf8cac` on the resource type and
applies **per-capability** tests. One arm at `0x4fdf8ada` reads `[esi+0x3c]`
and tests bit `0x4000`, returning `NOTAVAILABLE` at `0x4fdf8aec` when clear; a
second `NOTAVAILABLE` site sits at `0x4fdf8b09`. Both existing probe queries
carry usage flags or a resource type that can reach such an arm, so their
failure was never evidence about list population.

What `GetDeviceCaps` failing at `0x4fdf97b3` proves on its own is only the
weaker statement: **no list entry carries `D3DFORMAT_OP_3DACCELERATION`** —
satisfied by an empty list, but equally by a populated list stripped of that
bit. Those have completely different causes.

### The discriminator

The probe's `format-list` stage gained a query chosen to have no capability arm
that can reject it: `D3DDEVTYPE_HAL`, adapter format `D3DFMT_X8R8G8B8`,
`Usage = 0`, `RType = D3DRTYPE_SURFACE`, `CheckFormat = D3DFMT_X8R8G8B8`. Only
list membership should decide it.

| Query | v3 (working) | v4 (failing) |
| --- | --- | --- |
| `devtype` | `0x00000000` | `0x8876086a` |
| `tex` | `0x00000000` | `0x8876086a` |
| `rt` | `0x00000000` | `0x8876086a` |
| `surf` | `0x00000000` | `0x8876086a` |

Runs `20260725-v4-list-discriminator-v3-baseline` (full pass, `21/0/10/20/0`)
and `20260725-v4-list-discriminator-t2`.

The plainest query succeeds in the working configuration and fails under v4.
Combined with the identical `modecount=8 modecount16=8` in both — the mode list
is intact either way — this supports the conclusion that under v4 the adapter
has **no usable operation-format list at all**, rather than a list that merely
lost the acceleration bit.

The residual uncertainty is honest and small: the `D3DRTYPE_SURFACE` arm of
`CheckDeviceFormat` has not been decoded instruction by instruction, so a
capability-driven rejection cannot be excluded with total certainty. But every
capability word the *builder* reads has been compared between the two
configurations and matches, so a capability explanation would require
`CheckDeviceFormat` to read a field the builder does not.

### What this means for the search

The failure is in the builder, not in record validation. The three early exits
at `0x4fdfb576`, `0x4fdfb5b3` and `0x4fdfb608` remain the whole search space,
and normalisation and the five per-record validation rules are no longer
suspects. That is a genuine narrowing: it retires the entire
record-content line of investigation.

## Which builder exit is taken: exit 3

The three early exits can be discriminated from the existing logs, without a
debugger, because of the order the builder does things:

```
0x4fdfb576   exit 1   (pointer from 0x4fe04c50 is null)
0x4fdfb5ac   call the DdReenableDirectDrawObject wrapper
0x4fdfb5b3   exit 2   (that wrapper returned 0)
0x4fdfb5bd   call 0x4fe036c0, the capability-and-format gather
0x4fdfb608   exit 3   (the gather returned 0)
```

The gather is called **after** both earlier checks. So if the gather's query
burst is observable, exits 1 and 2 were passed. Every D3DGDI2 query the gather
issues is answered by this driver and logged, which makes the gather directly
countable.

Counting complete gather passes in the two runs
(`20260725-v4-list-discriminator-v3-baseline` and `-t2`):

| Token | v3 (working) | v4 (failing) |
| --- | ---: | ---: |
| `QXPDISP G9` (GETD3DCAPS9) | 6 | 6 |
| `QXPDISP FC` (GETFORMATCOUNT) | 6 | 6 |
| `QXPDISP AG9` (GETADAPTERGROUP) | 6 | 6 |
| `QXPDISP QC9` (GETD3DQUERYCOUNT) | 6 | 6 |
| `QXPDISP F` (GETFORMAT, bare token) | 81 | 81 |

Identical. The gather runs the same number of times and receives the same
number of format records in both configurations.

**Therefore exit 3 is the one taken.** The builder passes the null check and the
re-enable check, calls the gather, the gather issues every query and gets every
answer, and then returns 0 anyway. The failure is a late check inside
`0x4fe036c0`, after all driver traffic has completed.

This also retires the record-content line of investigation completely: the
driver serves byte-identical format data in both configurations, so no property
of the records themselves can explain the difference.

### The v4-only branches inside the gather

The handshake success path sets `[eax+0x154] |= 2` at `0x4fe03bd1`, a flag v3
never sets because its handshake falls back to caps8. Two places in the gather
read it:

- `0x4fe03cfc`, guarding GETFORMATCOUNT
- `0x4fe03df5`, guarding GETFORMAT

Both do the same thing: when the flag is set they populate an extra request
field at `+0x14` from `[ebp-0x80]` before issuing the query (`0x4fe03d2a`).
That is the same structure member the `GETDDIVERSION` reply bug involved, which
is consistent with `+0x14` being a general-purpose in/out member in this
request family.

QXP ignores that member for these two queries and returns the same counts and
records regardless, which the identical `FC` and `F` totals confirm. So the
v4-only branches change what the runtime *asks*, not what it *gets*.

### Where that leaves the search

The remaining failure must be one of the checks in `0x4fe036c0` that run after
the last D3DGDI2 query. From the earlier enumeration of the fourteen branches
to the failure epilogue `0x4fe04c10`, the ones after `GETD3DQUERYCOUNT` are the
call at `0x4fe04416` — a GUID `GetDriverInfo` for `GUID_D3DExtendedCaps` with
`dwExpectedSize = 0x74`, requiring the thunk to return
`DDHAL_DRIVER_HANDLED` and `ddRVal` to be cleared — and its two guards at
`0x4fe0441b` and `0x4fe04427`.

That call was previously set aside because `QXPDISP GX` never appears in either
configuration. That reasoning has the same shape as the `CheckDeviceFormat`
mistake corrected above: the absence of a driver-side log token does not
establish that the query did not happen or did not matter, only that it did not
reach this driver's handler. It should be treated as the leading candidate
again, not as closed.

## Elimination inside the gather, and the contradiction it produces

The gather has exactly two return sites: `0x4fe04c20` returning 0, reached only
by the fourteen enumerated branches, and `0x4fe04c3f` returning 1. There is no
early success return — the only jumps into the success epilogue are at
`0x4fe04bff` and `0x4fe04c04`, at the very end — so both configurations
traverse the whole function.

Mapping each branch against what the logs show was answered:

| Branch | Check | Status |
| --- | --- | --- |
| `0x4fe03817`, `0x4fe03901` | pre-handshake null/allocation | passed in both (the handshake is reached) |
| `0x4fe03bcb` | caps9 reply is `0x130` | v4 only; passed, since later queries follow |
| `0x4fe03ca8` | caps8 reply is `0xd4` | v3 only |
| `0x4fe03d98` | format count reply is `0x18` | passed in both (`FC` = 6) |
| `0x4fe03e8f` | format reply is `0x34` | passed in both (`F` = 81) |
| `0x4fe03f53` | allocation | passed in both |
| `0x4fe03fea` | extended mode count reply is `0x18` | passed in both (`EM9`) |
| `0x4fe040f2` | extended mode reply is `0x24` | never issued, count is zero |
| `0x4fe041c9` | adapter group reply is `0x1c` | passed in both (`AG9` = 6) |
| `0x4fe04275` | query count reply is `0x14` | passed in both (`QC9` = 6) |
| `0x4fe0435d` | query reply is `0x14` | never issued, count is zero |
| `0x4fe0441e`, `0x4fe04429` | the `GUID_D3DExtendedCaps` call | the only checks left after `QC9` |

No pass is truncated: `G9`, `FC`, `AG9` and `QC9` all appear exactly six times,
so every pass that started also finished. A pass failing at the caps9 size check
would show `G9` without a following `FC`.

By elimination the gather can only be failing at the extended-caps call. **But
that conclusion contradicts the evidence**, because the same call, with the same
parameters, must succeed in the v3 run where everything works.

### `QXPDISP GX` does fire — just not here

An earlier note recorded that the extended-caps token never appears anywhere.
That was wrong, and the error was one of search scope: it appears in
`D3dVisual` stage logs (for example
`artifacts/runs/20260725-caps-honesty-regression-t2/D3dVisual/driver.log`),
where a legacy Direct3D device is created. It appears in neither the `Boot` nor
the `D3d9` stage of either configuration.

So during the D3D9 probe the runtime never asks this driver for extended caps,
in the working configuration either — yet the working configuration completes
the gather. The call at `0x4fe04416` is therefore satisfied without reaching
this display driver, identically in both.

### Where this leaves things

Every check in the gather either passes on identical data in both
configurations, or is skipped in both, or is satisfied without involving this
driver. Static analysis says the function should return 1 under v4, and it
demonstrably returns 0.

One premise must be wrong, and the remaining candidates are not separable from
the outside: a parameter to the extended-caps call differing by configuration,
or state written earlier diverging. Note that the two paths do set different
flags — v4's handshake success does `[eax+0x154] |= 2` at `0x4fe03bd1`, while
v3's caps8 path does `or [eax+0x154],0x1` at `0x4fe03ce3` — so downstream
behaviour keyed on that word is a live possibility.

**Static analysis is exhausted.** Resolving this now requires observing the
actual failing branch: a breakpoint on `0x4fe036c0` with a watch on the fourteen
branch targets, or single-stepping from `0x4fe04275` to the return. WHPX
supports breakpoints by INT1 injection, so QEMU's gdbstub is viable; the
practical difficulty is inserting the breakpoint while `d3d9.dll` is resident in
the probe process.

## The v4-only format-request field decoded

The only version-dependent code inside the capability gather `0x4fe036c0` is two
writes guarded by `test BYTE PTR [edx+0x154],0x2`, the flag set at `0x4fe03bd1`
when caps came from `GETD3DCAPS9`. Both write the same local, `[ebp-0x80]`,
which is loaded with the constant **`0x902`** at `0x4fe037b8` — the DirectX
runtime version 9.02.

Matching the writes against the WDK 7.1 structures in
`inc/ddk/d3dhal.h`, where `DD_GETDRIVERINFO2DATA` occupies the first four
DWORDs:

| Query | Struct | Offset `+0x14` | v4 writes |
|---|---|---|---|
| `GETFORMATCOUNT` (2) | `DD_GETFORMATCOUNTDATA` | `dwReserved` | `0x902` at `0x4fe03d2a` |
| `GETFORMAT` (3) | `DD_GETFORMATDATA` | `format.dwSize` | `0x902` at `0x4fe03e28` |

So under DDI v4 the runtime stamps its version into a field the WDK documents
as Reserved, and into the `dwSize` of the output pixel format. QXP ignores both
and returns identical data in either configuration.

### This is not the failure, and that is now measured rather than inferred

Reading the code after each call:

- `0x4fe03d98` compares the returned size against `0x18` and `0x4fe03e8f`
  compares it against `0x34`. Both are plain size checks, identical in v3 and
  v4, and both pass on the data QXP returns.
- A failed format query does **not** fail the gather. It jumps to `0x4fe03f05`,
  which tries a fallback via `0x4fe4f6c0`; only if that fallback fails does
  `0x4fe03f53` reach the failure epilogue. Since the query counts are identical
  across configurations, this path is not taken at all.
- After `GETFORMAT` the runtime copies the 0x20-byte pixel format out of
  `+0x14` and tests flags in it. It never compares the field against the
  `0x902` it wrote, so a driver that overwrites it is not penalised.

The gather also builds exactly ten query types — 4, 0x10, 1, 2, 3, 0x11, 0x12,
0x13, 0x21, 0x22 — every one of which QXP answers. In particular it never
issues `GETMULTISAMPLEQUALITYLEVELS` (0x16), so an unanswered query type is
excluded as a cause.

This closes the format-query path by direct reading rather than by elimination.
The remaining branches still require runtime observation.

### Reproducing this analysis

The disassembly is not in the repository and `artifacts/` is git-ignored, so it
must be regenerated:

```
wsl bash tools/extract-guest-d3d9.sh          # pulls d3d9.dll from the image
wsl bash tools/d3d9-disasm.sh 4fe036c0 40     # prints a region
```

Run both under WSL. The disassembly lives in WSL's `/tmp`, which is a different
directory from git-bash's `/tmp`; a script invoked from git-bash will not find
it. Image base is `0x4fdd0000` and the DLL is 1,689,088 bytes,
md5 `0607cbc6fa20114cb491efe4b2f9efad`.

## The builder masks format operations differently per version

The gather is not the only version-dependent code. The **format-list builder**
`0x4fdfb500` branches on the same flag at `0x4fdfbca4`:

```
4fdfbca4: test BYTE PTR [ebx+0x154],0x2   ; v4?
4fdfbcab: mov  ecx,[eax+0x58]             ; the entry's format operations
4fdfbcae: je   0x4fdfbcb8
4fdfbcb0: and  ecx,0x1fffddf              ; v4 mask
4fdfbcb6: jmp  0x4fdfbcbe
4fdfbcb8: and  ecx,0x1ddf                 ; v3 mask
4fdfbcbe: mov  [eax+0x58],ecx
```

The v3 mask keeps nothing above bit `0x1000`. The v4 mask keeps bits up to
`0x1000000`. Checked against the WDK constants:

| Operation | Value | v3 `0x1ddf` | v4 `0x1fffddf` |
|---|---|---|---|
| `D3DFORMAT_OP_3DACCELERATION` | `0x00000800` | kept | kept |
| `D3DFORMAT_OP_DISPLAYMODE` | `0x00000400` | kept | kept |
| `D3DFORMAT_OP_OFFSCREENPLAIN` | `0x00004000` | **stripped** | **kept** |
| `D3DFORMAT_OP_BUMPMAP` | `0x00010000` | **stripped** | **kept** |

QXP sets `D3DFORMAT_OP_OFFSCREENPLAIN` on essentially every format it reports
(`directdraw.c:14538`, `:14545`, `:14554`) and `D3DFORMAT_OP_BUMPMAP` on others.

### Why this is the first hypothesis that fits the evidence

Every measurement so far showed the driver returning byte-identical data in both
configurations, with identical query counts and identical reply sizes, yet v4
failing. That is explained if the difference is not in what the driver sends but
in **what the runtime keeps**: under v3 these bits are silently discarded, and
under v4 they survive into the all-or-nothing validation that follows. A rule
violated only by a surviving bit would reject the entire list, producing exactly
the observed symptoms — an empty format list, `GetDeviceCaps` returning
`NOTAVAILABLE` because no entry carries `0x800`, and `CheckDeviceFormat`
failing even for a plain surface.

This also explains why the driver-side evidence looked so innocent: it is
innocent. The divergence happens after the data arrives.

### The reasoning error this corrects

The earlier conclusion that the builder fails at exit 3, and therefore that the
gather returns zero, was inferred from query counts being identical across
configurations. That inference does not hold. Identical counts are equally
consistent with the gather succeeding and the builder failing later, because
every query is issued before either outcome. The builder's own version-dependent
code at `0x4fdfbca4` sits after the gather call at `0x4fdfb5bd`.

This is the third time in this investigation that an argument from a count or
from an absent log token has pointed the wrong way. Counts and absences
constrain; they do not identify.

### Next step: a decisive experiment

The hypothesis is cheap to test. QXP controls the operation bits it reports, so
building with the DDI v4 gate plus a variant that withholds
`D3DFORMAT_OP_OFFSCREENPLAIN` and `D3DFORMAT_OP_BUMPMAP` should, if the
hypothesis is right, populate the format list and make `hal-caps` and `surf`
succeed under v4. If the list is still empty, the surviving bits are exonerated
and the five validation rules at `0x4fdfc240` must be read directly.

### Measured: the surviving bits are NOT the cause

The experiment was run. A new default-off gate,
`QXP_DIAGNOSTIC_FORMAT_OPS_V3MASK`, applies the v3 mask `0x1ddf` to every
reported format at the single place the driver writes operation bits
(`qxp_fill_operation_format`), so that both configurations present identical
surviving bits. Built together with `QXP_DIAGNOSTIC_DDI4`, both defines
confirmed present in the build log.

Run `artifacts/runs/20260725-v4-formatops-v3mask`, DDI v4 active:

```
stage=format-list modecount=8 modecount16=8 devtype=0x8876086a
    tex=0x8876086a rt=0x8876086a surf=0x8876086a
stage=hal-caps hr=0x8876086a caps2=00000000 devcaps=00000000
```

Identical to the v4 baseline. **The hypothesis is refuted.**
`D3DFORMAT_OP_OFFSCREENPLAIN` and `D3DFORMAT_OP_BUMPMAP` surviving into v4 are
not what the validation rejects, and the mask difference at `0x4fdfbca4`, while
real, is not causal.

This is a clean negative and it eliminates a whole class: no difference in the
*content* of the operation bits explains the failure, because the two
configurations were made bit-identical and still diverged.

What remains are the other places the flag is consumed. Inside the builder
those are `0x4fdfb580` (`test ...,0x3`, which does not discriminate between the
two versions since it accepts either bit) and the reads and writes at
`0x4fdfb7e8`, `0x4fdfb831` and `0x4fdfb861`, plus `0x4fdf808d` outside it. Those
should be read before any further experiment, and the five validation rules at
`0x4fdfc240` remain unread.

## The real shape of the v4 blocker: a minimum capability contract

The failure is not a protocol bug. The format-list builder contains a
**v4-only chain of capability requirements**, entered at `0x4fdfb7e8`:

```
4fdfb7e8: mov  eax,[ebx+0x154]
4fdfb7ee: test al,0x3          ; either version flag?
4fdfb7fa: jne  0x4fdfb86c
...
4fdfb86c: test al,0x2          ; v4 specifically
4fdfb86e: je   0x4fdfba0c      ; v3 skips the entire chain
```

Everything between `0x4fdfb874` and `0x4fdfba0c` therefore runs **only under
v4**, which is why none of it is observable in a v3 run and why every
driver-side measurement looked identical.

The caps9 block is `rep movs`'d to offset 0 of this object at `0x4fe03be7`, so
`[ebx+N]` addresses `D3DCAPS9` fields directly.

### Gate 1 — `DevCaps2` must carry `D3DDEVCAPS2_STREAMOFFSET`

```
4fdfb874: mov  eax,[ebx+0xd4]   ; D3DCAPS9.DevCaps2, offset asserted in our own source
4fdfb87a: test al,0x1           ; D3DDEVCAPS2_STREAMOFFSET
4fdfb87f: jne  0x4fdfb88d       ; set -> continue
4fdfb881: mov  DWORD PTR [ebx+0x8],0x2000000
4fdfb888: jmp  0x4fdfc4f7       ; clear -> abandon the builder
```

QXP reports `DevCaps2 = 0`, so it fails this unconditionally. This matches
`d3d9caps.h`, which says the bit "Must be set by DX9 drivers".

**A comment in `directdraw.c` previously claimed the builder "only bails when
DevCaps2 is nonzero and lacks this bit", so that setting it would be inert.
That was wrong and has been corrected**: there is no nonzero precondition.

### Normalisations, not gates

`MaxPointSize` (`+0xb0`) is compared against a constant and clamped to zero, and
a `StencilCaps` bit is cleared when `PrimitiveMiscCaps` (`+0x20`) lacks `0x10`.
Neither abandons the builder.

### Gate 2 — the timestamp query triple must be all-or-none

`0x4fdfb490` scans a list for the values 10, 11 and 12 and returns nonzero only
if all three are present or all three absent; a partial subset returns zero and
bails at `0x4fdfb8d4`. Those are `D3DQUERYTYPE_TIMESTAMP`,
`TIMESTAMPDISJOINT` and `TIMESTAMPFREQ`, matching the documented D3D9 rule that
the triple is supported together. QXP reports a zero-length query list, and with
a count of zero the function returns 1, so this gate currently passes.

### Measured

Building with `QXP_DIAGNOSTIC_DDI4` and `QXP_DIAGNOSTIC_STREAMOFFSET` together,
both defines confirmed in the build log
(`artifacts/runs/20260725-v4-streamoffset`), still yields
`surf=0x8876086a` and an empty list. So gate 1 is **necessary but not
sufficient**, and at least one further requirement fails after it. The chain
continues by reading `CursorCaps` (`+0x18`), `CubeTextureFilterCaps` (`+0x44`),
`TextureAddressCaps` (`+0x4c`), `LineCaps` (`+0x54`), `TextureOpCaps` (`+0x90`)
and `PS20Caps` (`+0x108`) before rejoining at `0x4fdfba0c`.

### What this means for the project

The v4 blocker is an **honest capability gap, not a defect to repair**. The
runtime is enforcing the DX9 minimum contract, and the original decision to
decline v4 was correct for the right reason. Reaching v4 means satisfying these
requirements truthfully, which for `D3DDEVCAPS2_STREAMOFFSET` means implementing
stream offsets rather than asserting the bit. The remaining chain members should
be decoded next, from `0x4fdfb8e0`, to complete the requirement list.

### The chain is complete: two gates, then capability masking

Decoding the rest of the chain corrects the previous note, which said it
"continues through several more capability fields" as if more gates followed.
It does not. From `0x4fdfb8e0` to `0x4fdfba07` there is **no further bail**:
the region is a long run of `and` operations writing masked values back into
the capability block, for example `and DWORD PTR [ebx+0x3c],0x27ede7` on
`TextureCaps`. It then jumps to `0x4fdfbaf4`.

The v3 branch at `0x4fdfba0c` performs the same kind of masking with different
constants, such as `and DWORD PTR [ebx+0x20],0x3ff6` on `PrimitiveMiscCaps`,
and converges at the same `0x4fdfbaf4`.

So the complete v4-only chain is:

1. **Gate**: `DevCaps2 & D3DDEVCAPS2_STREAMOFFSET`, else abandon.
2. Normalise `MaxPointSize`; normalise a `StencilCaps` bit from
   `PrimitiveMiscCaps`.
3. **Gate**: timestamp query triple all-or-none, else abandon.
4. Mask the capability block with the v4 constant set.

### Where the failure now sits

With `QXP_DIAGNOSTIC_DDI4` and `QXP_DIAGNOSTIC_STREAMOFFSET` both enabled, gate
1 is satisfied and gate 2 passes trivially on our empty query list, so the chain
completes — yet the format list is still empty. The failure is therefore
**downstream of `0x4fdfbaf4`**, in code shared with v3, reached with a
capability block that was masked by the v4 constants rather than the v3 ones.

That is a coherent and testable position: the two versions run the same later
code over differently masked capabilities. The next step is to diff the two mask
sets field by field, identify which capability the v4 masking leaves in a state
the later validation rejects, and check that field against what QXP reports. The
five validation rules at `0x4fdfc240` remain unread and are the other half of
that comparison.

### The two mask sets differ in which fields they touch at all

Pairing each `and` with the store that follows it:

| Field | offset | v3 branch | v4 branch |
|---|---|---|---|
| `PresentationIntervals` | `0x14` | `& 0x8000000f` | `& 0x8000000f` |
| `CursorCaps` | `0x18` | `& 0x3` | `& 0x3` |
| `DevCaps` | `0x1c` | `& 0xffbfff0` | `& 0xffbfff0` |
| `PrimitiveMiscCaps` | `0x20` | `& 0x3ff6` | `& 0x1ffff6` |
| `RasterCaps` | `0x24` | `& 0xf7f19f` | `& 0xf77e191` |
| `ShadeCaps` | `0x38` | `& 0x84208` | `& 0x84208` |
| `TextureCaps` | `0x3c` | `& 0x7ede7` | `& 0x27ede7` |
| `StencilCaps` | `0x88` | written | `& 0x1ff` |
| `FVFCaps` | `0x8c` | `& 0x18ffff` | `& 0x18ffff` |
| `TextureOpCaps` | `0x90` | `& 0x3ffffff` | `& 0x3ffffff` |
| `VertexProcessingCaps` | `0x9c` | `& 0xfb` | **not written** |
| `DevCaps2` | `0xd4` | **not written** | `& 0x6f` |
| `VS20Caps` | `0xf8` | **not written** | `& 0x1` |
| `PS20Caps` | `0x108` | **not written** | written |

The interesting differences are not the widths but **which fields each version
touches at all**. The v4 branch propagates `DevCaps2`, `VS20Caps` and
`PS20Caps` — the Shader Model 2 capability structures — which the v3 branch
never reads. The v3 branch masks `VertexProcessingCaps`, which v4 leaves alone.

`DevCaps2 & 0x6f` preserves bit 0, so a driver that sets
`D3DDEVCAPS2_STREAMOFFSET` keeps it through masking.

This is consistent with the reading that v4 is the Shader Model 2 contract: it
carries the SM2 capability structures forward into the shared code, where v3
carries nothing. QXP derives its shader versions from the DX8 capability block
and reports empty SM2 structures, so under v4 the shared code downstream
receives a capability block claiming a shader version with no matching
capability structure behind it. That is the most probable remaining
inconsistency and is where the next investigation should start, paired with the
still-unread validation rules at `0x4fdfc240`.

### Third refutation, and an ambiguity these experiments cannot resolve

Building with `QXP_DIAGNOSTIC_DDI4`, `QXP_DIAGNOSTIC_STREAMOFFSET` and
`QXP_DIAGNOSTIC_SM2_CAPS` together — all three defines confirmed in the build
log, run `artifacts/runs/20260725-v4-sm2caps` — again yields
`surf=0x8876086a` and an empty list, identical to every other v4 run. So the
shader-version and SM2-capability-structure inconsistency is **not** the cause
either. Three hypotheses are now refuted by measurement: the surviving format
operation bits, the stream offset requirement alone, and SM2 capability
consistency.

**These experiments cannot distinguish the two remaining possibilities**, and
saying otherwise would repeat the mistake this investigation keeps making.

The capability chain lives at `0x4fdfb7e8`, which is reached only if the builder
gets past the gather call and its immediate failure exit at `0x4fdfb608`. So
either:

1. the gather still returns zero, the builder exits at `0x4fdfb608`, and the
   chain never executes — in which case no capability change can possibly help,
   which is exactly what three negative experiments look like; or
2. the gather succeeds, the chain runs, and something after it rejects the list.

Every capability experiment produces an identical result under both, so no
further blind capability change can discriminate. The earlier claim that the
gather fails was inferred from query counts and does not hold; the claim that it
succeeds has never been positively established either. **Both remain open, and
the evidence available cannot separate them.**

### This is now genuinely a debugger problem

The remaining question is a single bit of control flow: does execution reach
`0x4fdfb874`? That is one breakpoint. No amount of additional static reading or
capability guessing answers it, and each guess costs a full virtual machine
cycle to produce the same uninformative result.

The next action should be a QEMU gdbstub breakpoint at `0x4fdfb608` and
`0x4fdfb874`, synchronised with `d3d9.dll` residency in the probe process. Until
that observation exists, further v4 capability work is unfounded.

## The gather does succeed under v4 — the ambiguity resolves by enumeration

The previous section left two possibilities open: the gather fails and the
capability chain never runs, or the gather succeeds and something later rejects
the list. That can now be decided without a debugger.

### The extended-caps call is conditional, and skipped

`0x4fe04416` is guarded:

```
4fe0438d: mov  eax,[ebp-0x5c8]
4fe04393: test eax,eax
4fe04395: je   0x4fe04432        ; skip the extended-caps query
4fe0439b: mov  ecx,[ebp-0x828]
4fe043a1: test ecx,ecx
4fe043a3: je   0x4fe04432        ; skip
```

Both buffers are filled only by the fallback helper `0x4fe4f6c0`, which runs on
the *format-query-failed* path at `0x4fe03f05`. Our format queries succeed, so
the buffers stay zero and the whole block is jumped over.

This is confirmed by measurement: across all three v4 runs this session, the
driver logs exactly ten declined GUIDs and `GUID_D3DExtendedCaps`
(`0x7de41f80`) is not among them, so it is never issued.

**This retires a recorded contradiction.** The earlier elimination concluded
that only the extended-caps call could be failing, "which contradicts the
evidence". The elimination was simply wrong: it assumed the call always
happens, when it is conditional and in our configuration never runs.

### No failure branch remains after the query section

`GETADAPTERGROUP` and `GETD3DQUERYCOUNT` are issued late in the gather, and both
are logged six times in v3 *and* v4, so the gather reaches its final section in
both. After that point the surviving failure branches are:

- `0x4fe04275` and `0x4fe0435d` — inside the per-query loop. QXP reports a query
  count of zero, so the loop body never executes.
- `0x4fe0441e` and `0x4fe04429` — inside the extended-caps block, which is
  jumped over as shown above.

With every remaining branch unreachable, **the gather returns success under
v4**. The builder therefore proceeds past `0x4fdfb608` and executes the
capability chain.

### Caveat, stated plainly

This rests on query counts showing the gather reached its final section, and
count arguments have misled this investigation three times. The difference here
is that counts are not being used alone to identify a culprit: they establish a
lower bound on progress, and the branches *after* that point are then excluded
individually by reading them. The conclusion fails only if some failure path
exists that is neither in the query loop nor the extended-caps block, so that
enumeration is the thing to re-check before relying on this.

### Consequence

Gate 1, `DevCaps2 & D3DDEVCAPS2_STREAMOFFSET`, was genuinely failing in the
default v4 configuration, because the chain does run and QXP reports
`DevCaps2 = 0`. Setting it via the diagnostic gate satisfies gate 1, and the
timestamp triple gate passes on an empty query list, so the chain now completes
and the remaining failure lies **downstream of `0x4fdfbaf4`**, in code shared
with v3 but reached with a v4-masked capability block. That is where the next
investigation belongs, together with the unread validation rules at
`0x4fdfc240`.

## The downstream validation rules, located and partly decoded

With the failure localised past `0x4fdfbaf4`, the validation rules are at
`0x4fdfc2a0`. There are exactly three failure stores downstream, all writing
error `0x2000000`: `0x4fdfc30b`, `0x4fdfc321` and `0x4fdfc4da`.

### Format-operation consistency rules

Each format entry is checked against its operation bits, setting a local flag
when an inconsistency is found:

- a format carrying `D3DFORMAT_OP_3DACCELERATION` (`0x800`) but **not**
  `D3DFORMAT_OP_DISPLAYMODE` (`0x400`) is flagged;
- a format carrying `3DACCELERATION` whose code is `0x15` (`D3DFMT_A8R8G8B8`)
  or `0x19` (`D3DFMT_A1R5G5B5`) is flagged;
- a format carrying `DISPLAYMODE` whose code is `0x17` (`D3DFMT_R5G6B5`) is
  cross-checked through the helper `0x4fdf78a0`.

### Depth/stencil pairing rules

The two bails at `0x4fdfc30b` and `0x4fdfc321` sit in the handling of format
code `0x49`, which is `D3DFMT_D15S1`; the following arm handles `0x4b`,
`D3DFMT_D24S8`, and passes `0x4a` to the same helper. So these rules validate
that the reported depth/stencil formats are consistent with their companions.
The second bail is guarded by `test BYTE PTR [ebx+0x10],0x40`, reading
`D3DCAPS9.Caps3`, a field **neither** version's branch masks, so it passes
through exactly as the driver reports it.

### Why this is the right place to look next

These rules run in code shared with v3, so on their own they cannot explain a
v4-only failure — but they consume the capability block, and that block is
masked differently by each version before reaching them. The concrete next step
is to determine which reported depth/stencil formats and which capability bits
QXP presents here, and to check them against these rules with the v4 mask set
applied rather than the v3 one.

The third bail at `0x4fdfc4da` has not been decoded and should be read as well.

## Correction: the builder gathers twice

The third downstream error store at `0x4fdfc4da` is not a validation rule. It is
a shared error epilogue — `pop edi; pop esi; store 0x2000000; pop ebx; ret` —
and exactly two branches jump into it:

```
4fdfb6b0: call 0x4fe014f0      ; a SECOND DirectDraw re-enable
4fdfb6b7: je   0x4fdfc4d8
...
4fdfb6ff: call 0x4fe036c0      ; a SECOND capability gather
4fdfb706: je   0x4fdfc4d8
```

So the builder performs **two** re-enable and gather passes, the second writing
into a different output area (`[ebx+0x178]`). The v4 capability chain at
`0x4fdfb7e8` comes after both, so it is reached only if **both** gathers
succeed.

### What this does to the earlier conclusion

The previous section concluded the gather succeeds under v4 by enumerating the
failure branches reachable after the query section. That enumeration is still
correct **for a single invocation**, but it was written as though there were
only one. With two call sites, the recorded token counts of six per query type
are consistent with several splits across the two passes, so they no longer pin
down how far the second pass gets.

The conclusion is therefore downgraded from established to **probable**: the
first gather very likely succeeds, and the second is now an untested candidate
for the failure. This is the fourth time in this investigation that an argument
resting on aggregate counts has needed correction once the control flow was read
properly.

### Rules that are now excluded

Both remaining downstream bails, `0x4fdfc30b` and `0x4fdfc321`, sit inside the
arm selected by format code `0x49`, which is `D3DFMT_D15S1`. QXP reports only
`D16` (80), `D24S8` (75) and `D24X8` (77) and never reports `D15S1`
(`directdraw.c:14626`, `:14630`, `:14657`), so neither bail can fire for our
format set.

The surrounding arms form a pairing table in which each odd depth code looks up
its even partner through the helper `0x4fdf78a0`: `0x4b`→`0x4a`, `0x4d`→`0x4c`,
`0x4f`→`0x4e`. Our `D24S8` and `D24X8` entries do enter this table, and what it
does with a missing partner has not yet been read.

### Next step

Distinguish the two gather calls. They are separate call sites, so a breakpoint
or a driver-side counter that changes behaviour between the first and second
pass would separate them, and the second pass is now the prime suspect.

## Correction to the correction: the two gathers are alternatives, not passes

The previous section said the builder "gathers twice" and that the capability
chain runs only if both succeed. Reading the control flow around both call sites
shows that is wrong. They are **mutually exclusive paths**, selected at entry:

```
4fdfb580: test BYTE PTR [ebx+0x154],0x3   ; is a version flag already set?
4fdfb587: je   0x4fdfb593                 ; no  -> esi stays 0
4fdfb589: cmp  DWORD PTR [ebx+0x1c],edx   ; DevCaps zero?
4fdfb58c: je   0x4fdfb593                 ; yes -> esi stays 0
4fdfb58e: mov  esi,0x1
4fdfb593: cmp  esi,edx
4fdfb5a1: je   0x4fdfb651                 ; esi == 0 -> Path B
```

- **Path A** (`esi == 1`, taken when a version flag is already set and `DevCaps`
  is nonzero — i.e. this is a *rebuild*): re-enable, gather, store five values
  into `[ebx+0x130]`..`[ebx+0x140]`, and `ret` at `0x4fdfb650`. Its gather
  failure exits to `0x4fdfc537`. **The capability chain is never reached on this
  path.**
- **Path B** (`esi == 0`, the first build, when the flag is still clear): free
  the previous buffers through `0x4fe0a460`, re-enable at `0x4fdfb6b0`, gather at
  `0x4fdfb6ff`, then fall into the capability chain at `0x4fdfb7e8`. Its
  failures exit through the shared epilogue `0x4fdfc4d8`.

Since the flag starts clear, the **first** build always takes Path B, which is
the path that contains the v4 capability chain and the format-list construction.

### Net effect on the conclusion

The enumeration that concluded the gather succeeds applies to a single
invocation, and Path B contains exactly one — the call at `0x4fdfb6ff`. So the
conclusion stands for the build that matters, and the "downgraded to probable"
qualification is withdrawn. What the aggregate token counts cannot do is tell us
how many of the six occurrences belong to Path A rebuilds versus Path B first
builds, so they should not be used to reason about either path individually.

This is the fifth correction in this area. Every one of them came from reading
control flow that an earlier step had assumed rather than checked, and each
assumption looked reasonable at the time. The recurring failure mode is not
carelessness about data; it is treating structure as known when only behaviour
was observed.

## The five validation rules, decoded and checked against our data

The all-or-nothing validation is now fully decoded, including its consequence.
Each format is examined in a loop that accumulates a single inconsistency flag
in `[ebp-0x4]`, and at `0x4fdfc3d6`:

```
4fdfc3d6: mov  eax,[ebp-0x4]
4fdfc3d9: test eax,eax
4fdfc3db: je   0x4fdfc3f3        ; flag clear -> store the list (success)
4fdfc3e3: call 0x4fe013e0        ; flag set   -> FREE the format list
4fdfc3e8: mov  DWORD PTR [edi],0 ;               and null the pointer
4fdfc3ee: jmp  0x4fdfc4fa
```

So **any single rule firing discards every format**, which is precisely the
observed symptom: an empty list, no entry carrying `0x800`, `GetDeviceCaps`
returning `NOTAVAILABLE`, and `CheckDeviceFormat` failing for a plain surface.

The five sites that set the flag, checked against what QXP actually reports:

| # | Site | Rule | QXP's data | Fires? |
|---|---|---|---|---|
| 1 | `0x4fdfc2ae` | `3DACCELERATION` without `DISPLAYMODE` | only `X8R8G8B8` and `R5G6B5` carry `3DACCELERATION`, and both carry `DISPLAYMODE` | no |
| 2 | `0x4fdfc2c5` | `3DACCELERATION` on `A8R8G8B8` (`0x15`) or `A1R5G5B5` (`0x19`) | neither carries it | no |
| 3 | `0x4fdfc2ee` | `DISPLAYMODE` `R5G6B5` while `X1R5G5B5` (`0x18`) also has `DISPLAYMODE` | `X1R5G5B5` is reported `TEXTURE\|CUBETEXTURE\|OFFSCREENPLAIN`, without `DISPLAYMODE`, so the lookup fails | no |
| 4 | `0x4fdfc367` | depth pairing: flag when the even partner **is found** | QXP reports `D16`, `D24S8`, `D24X8` and no `0x4a`/`0x4c` partners | no |
| 5 | `0x4fdfc3ba` | inside the `MEMBEROFGROUP_ARGB` (`0x80000`) block | QXP never sets that operation | no |

Note rule 4 is the opposite of what was guessed last time: the flag is set when
the partner is **present**, so a missing partner is harmless.

### What this means

**None of the five rules fires for our format set**, which is consistent with v3
working, since this validation is shared code. So the discard at `0x4fdfc3d6` is
probably not where the v4 list is lost, and the empty list more likely comes from
the list being emptied or never populated earlier.

The prime remaining candidate is the filter at `0x4fdfbc60`:

```
4fdfbc60: cmp DWORD PTR [eax+0x4c],0x200000
4fdfbc67: je  0x4fdfbca4        ; keep
...                             ; otherwise compact the entry away
```

`0x200000` is `DDPF_D3DFORMAT`, and this is an **equality** test on the pixel
format flags, not a bit test, so any entry whose `dwFlags` is not exactly
`DDPF_D3DFORMAT` is removed. That filter runs immediately before the
version-dependent operation masking, and is where the next reading should go.

## The format-record layout, and the filter excluded

The gather builds each record at `0x4fe03ea6`: `mov DWORD PTR [esi],0x6c` with
the returned `DDPIXELFORMAT` copied eight dwords to `esi+0x48`. Since
`DDPIXELFORMAT` is `dwSize, dwFlags, dwFourCC, {…bit count}, {…dwOperations}`,
the record offsets are:

| Record offset | Field |
|---|---|
| `+0x00` | record size, `0x6c` |
| `+0x48` | `ddpf.dwSize` |
| `+0x4c` | `ddpf.dwFlags` |
| `+0x58` | `ddpf.dwOperations` (the `+0x10` union member) |

This confirms both loops operate on the same array: the filter at `0x4fdfbc60`
tests `dwFlags`, and the masking at `0x4fdfbca4` rewrites `dwOperations`.

The filter keeps an entry only when `dwFlags == DDPF_D3DFORMAT` exactly. QXP's
`qxp_fill_operation_format` sets exactly that, so those entries survive;
`qxp_fill_fourcc_format` sets `DDPF_FOURCC` and those entries are removed. That
behaviour is **version-independent**, so the filter is excluded as the v4
discriminator.

### Note on where `D3DFORMAT_OP_3DACCELERATION` comes from

The masking clears bit `0x20` from `dwOperations`, and a later pass converts a
surviving `0x20` marker into `0x800`. Read alone that looks like it would leave
no entry with `0x800` in either version. It does not, because QXP sets
`D3DFORMAT_OP_3DACCELERATION` **directly** on `X8R8G8B8` and `R5G6B5`, and both
mask sets preserve bit `0x800`. The `0x20` conversion is an additional source of
that bit, not the only one.

## Status: static analysis is exhausted

Every reachable difference has now been read or measured:

| Candidate | Outcome |
|---|---|
| Format-query path and reply sizes | read, identical, excluded |
| Unanswered query type | excluded, all ten answered |
| Surviving format-operation bits | **measured**, refuted |
| `DevCaps2` / `STREAMOFFSET` gate | necessary; **measured** insufficient |
| Timestamp query triple gate | passes on an empty query list |
| SM2 capability consistency | **measured**, refuted |
| All five validation rules | read and checked against our data, none fires |
| `DDPF_D3DFORMAT` filter | version-independent, excluded |
| Extended-caps GUID call | conditional and never issued |
| Path A versus Path B | resolved; the first build takes Path B |

What remains version-dependent is only the capability mask sets, and the two
format requests that carry the DX version in a reserved field. Neither survives
as an explanation.

Further progress requires observing execution rather than reading it: a QEMU
gdbstub breakpoint at `0x4fdfb6ff` (the Path B gather) and `0x4fdfb874` (the
first capability gate), to establish which is reached and what the gather
returns. Every remaining static hypothesis has been eliminated, and each
additional blind capability experiment costs a full virtual machine cycle to
produce the same uninformative empty list.

## Measured: the gdb stub in this build cannot set breakpoints

The observation route was attempted rather than assumed. An opt-in `-GdbPort`
was threaded through the overlay generator and the cycle script, and a minimal
remote-protocol client was written (`tools/gdb-rsp.py`), because no debugger is
installed here and installing one needs `sudo`, which times out.

Run `artifacts/runs/20260725-v4-gdb-probe`, with the DDI v4 gate enabled:

```
CONNECTED 127.0.0.1:49200
STOP-REASON T05thread:01;
BREAK Z1 0x4fdfb6ff UNSUPPORTED
BREAK Z0 0x4fdfb6ff UNSUPPORTED
```

The stub attaches, halts the guest and answers `?` correctly, so the transport
works. But **both breakpoint kinds are rejected**: `Z1` (hardware, debug
registers) and `Z0` (software, trap byte) each return the empty packet, which is
the protocol's "unsupported" answer. This build's accelerator does not implement
the debug hooks the stub needs, so the planned breakpoint at `0x4fdfb6ff` and
`0x4fdfb874` cannot be set at all.

**The gdbstub route is therefore closed for this build**, and the repeated
recommendation to "just set a breakpoint" was wrong about what the tooling can
do. Reaching it would need QEMU rebuilt with accelerator debug support, or the
TCG accelerator instead of WHPX, which would be far slower but does implement
breakpoints.

### A tool that can freeze the guest

Attaching halts the virtual machine. The first version of the client returned
without resuming when it could not arm anything, which left the guest frozen and
stalled the owning test cycle at its Boot stage. Sending a detach resumed it and
the cycle completed normally, confirming the diagnosis. The client now always
detaches before leaving that path. Two further faults found on first use: a
process-exit reply (`W00`) was reported as a breakpoint hit, and a connection
reset was an unhandled traceback rather than a message.

### Where this leaves the v4 investigation

Static analysis is exhausted and the runtime-observation route is unavailable
without rebuilding the emulator. The honest position is that the v4 format-list
failure is **not diagnosable with the tools currently available**, and the
capability contract it enforces is real work regardless: `DevCaps2` must carry
`D3DDEVCAPS2_STREAMOFFSET`, which means implementing stream offsets rather than
asserting the bit.

## Runtime observation: the v4 bail is positively identified

Earlier this investigation recorded that runtime observation was "measured
unavailable" because both `Z0` and `Z1` returned the empty packet. That
conclusion was correct about the accelerator in use and wrong as a general
statement, and the difference matters.

### Why breakpoints appeared unsupported

`gdbstub/system.c:644` dispatches breakpoint insertion through
`ops->insert_breakpoint`. In this QEMU tree the KVM, HVF and TCG accelerators
all register that op; **WHPX does not**. `accel/whpx/whpx-accel-ops.c` registers
`supports_guest_debug` and the synchronisation ops but never
`insert_breakpoint`, so every `Z` packet fell through to the not-implemented
path. The low-level WHPX breakpoint machinery in `target/i386/whpx/whpx-all.c`
(INT1 injection, `whpx_translate_cpu_breakpoints`, `whpx_apply_breakpoints`)
exists but is unreachable from the stub.

The command line already carried `-accel whpx,ssd=off -accel tcg`, so the fix
needed no host code at all: run the diagnostic guest under `-accel tcg` alone.
TCG breakpoints are virtual-address matches applied during translation, so they
arm before `d3d9.dll` is even loaded.

### The measurement

Guest installed under WHPX for speed, then re-run against the same overlay with
a TCG plus gdb command line. Breaking immediately **after** the capability load:

```
0x4fdfb874: mov eax,DWORD PTR [ebx+0xd4]   ; DevCaps2
0x4fdfb87a: test al,0x1                    ; D3DDEVCAPS2_STREAMOFFSET
0x4fdfb87f: jne 0x4fdfb88d                 ; continue
0x4fdfb881: mov DWORD PTR [ebx+0x8],0x2000000
0x4fdfb888: jmp 0x4fdfc4f7                 ; bail
```

At `0x4fdfb87a`, three consecutive hits: **`RAX = 0x00000000`**, `RBX =
0x24d8d8`. `DevCaps2` is zero, bit 0 is clear, `jne` is not taken, and the
builder stores error `0x2000000` and bails.

`ebx` is confirmed to be the `D3DCAPS9` structure by two independent offsets:
`0xd4` is `DevCaps2` and `0xb0` is `MaxPointSize`, which is exactly what the
`fld DWORD PTR [ebx+0xb0]` normalisation immediately after the gate operates on.

**This is the first positive identification of a v4 failure after eleven
eliminated hypotheses.** Static analysis had located this gate; runtime
measurement proves it is the one that fires.

### What it does not yet prove

Gate 1 is the *first* failure, not necessarily the only one. A previous
diagnostic run that forced the `STREAMOFFSET` bit still failed, which means at
least one later gate also rejects our capabilities; gate 2 is the timestamp
query triple at `0x4fdfb490`. The same technique can now walk the gates one at a
time, which was impossible before.

Satisfying gate 1 honestly means implementing stream offsets, not asserting the
bit.

### Two measurement traps this run walked into

Both were caught before they became false findings, and both are the same
mistake in different clothing: reading a number without checking what produced
it.

1. **Breaking on an instruction reads the state before it executes.** The first
   attempt broke at `0x4fdfb874`, the `mov` itself, and read `RAX = 0x2` — a
   stale value, not `DevCaps2`. The gate address and the address where the value
   is live differ by six bytes.
2. **Absence of a hit is not absence of execution.** Arming both the gather and
   gate 1 produced eight consecutive hits at the gather and none at gate 1,
   which looks like "gate 1 is never reached". The window clock did not advance
   between those hits, revealing a re-trap loop at the same PC. Arming gate 1
   *alone* hit it immediately. Counts and absences constrain; they do not
   identify.

## Walking the gates: both pass, and v4 still fails

With `QXP_DIAGNOSTIC_DDI4` and `QXP_DIAGNOSTIC_STREAMOFFSET` built together and
installed, the gates were measured one per run, each time breaking **after** the
instruction that produces the value.

| Gate | Address read | Measured | Verdict |
|---|---|---|---|
| 1, `DevCaps2 & STREAMOFFSET` | `0x4fdfb87a` (shipping caps) | `RAX = 0` | bails |
| 2, timestamp query triple | `0x4fdfb8d0` (forced bit) | `RAX = 1` | **passes** |

Gate 2's address lies inside the chain entered only under v4, so that hit is
attributable to the v4 build by construction. Reaching it at all also proves
gate 1 passed, since gate 1 bails to the epilogue.

End to end, with both gates satisfied, stock D3D9 **still** declines:

```
QXP_D3D9_RESULT rc=6 hr=0x8876086a stage=hal-caps
caps2=00000000 devcaps=00000000 vs=00000000 ps=00000000 rts=0
```

So the `STREAMOFFSET` requirement is **necessary but not sufficient**. At least
one further rejection exists downstream of both gates, and the earlier static
note claiming "then capability masking; no further bails" is wrong.

### One measurement that is NOT attributable

The validation flag at `0x4fdfc3d9` read `RAX = 0`, meaning the flag is clear,
the `je` is taken and the format list is kept. That matches the static analysis
of the five rules, none of which fire for our format set.

**But this address sits in the convergent section that both the v3 and the v4
build reach**, and the hit arrived on a different thread from the gate 2 hits.
The v3 build succeeds, so a clear flag is exactly what a v3 pass would show. The
measurement therefore does not establish anything about v4 and is recorded here
only so it is not mistaken later for v4 evidence.

Attributing a convergent address to one caller needs either a v4-only address
downstream of `0x4fdfb8e0` and before the convergence at `0x4fdfbaf4`, or a way
to read the version discriminator at the stop. Neither is done yet.

### Next question

`0x8876086a` is `D3DERR_NOTAVAILABLE`, reported at `stage=hal-caps`. The
builder's own gates are now known to pass, so the next step is to find where
that HRESULT originates rather than assuming it is still the format list. The
technique to do so now exists and is cheap.

## The proximate cause of the capability failure, measured

`stage=hal-caps` is `IDirect3D9::GetDeviceCaps(HAL)`. The runtime answers it by
scanning the stored format array:

```
4fdf976f: mov edx,[esi+0x158]      ; record count
4fdf9777: jbe 0x4fdf9790           ; count zero skips the loop
4fdf9779: mov ecx,[esi+0x15c]      ; record base
4fdf977f: add ecx,0x58             ; first record's dwOperations
4fdf9782: test BYTE PTR [ecx+1],0x8 ; bit 0x800, D3DFORMAT_OP_3DACCELERATION
4fdf9789: add ecx,0x6c             ; record stride
4fdf9792: jne 0x4fdf97bf           ; a match continues to the real work
4fdf979e: rep stos                 ; no match zeroes the whole D3DCAPS9
4fdf97b3: mov eax,0x8876086a       ; and returns D3DERR_NOTAVAILABLE
```

`dwOperations` sits at record `+0x58` because it shares a union with
`dwRBitMask` at offset `0x10` inside the `DDPIXELFORMAT` at record `+0x48`.

Measured at `0x4fdf9782`, count `= 2`, both records dumped:

| Field | Record 0 | Record 1 | Driver writes |
|---|---|---|---|
| `dwSize` | `0` | `0` | `sizeof(DDPIXELFORMAT)` = 32 |
| `dwFlags` | `0x200000` | `0x200000` | `DDPF_D3DFORMAT` |
| `dwFourCC` | `0x17` X8R8G8B8 | `0x16` R5G6B5 | same |
| bit count | `0` | `0` | 32 and 16 |
| **`dwOperations`** | **`0x400`** | **`0x400`** | TEXTURE, CUBETEXTURE, OFFSCREEN_RENDERTARGET, SAME_FORMAT_RENDERTARGET, DISPLAYMODE, **3DACCELERATION**, OFFSCREENPLAIN |

`0x400` is `D3DFORMAT_OP_DISPLAYMODE` alone. `test byte 0x04, 0x8` is zero, no
record matches, the caps block is zeroed and the call returns
`D3DERR_NOTAVAILABLE`. **That is the proximate cause of the `hal-caps`
failure.**

This falsifies an earlier note in this document claiming the driver's
`3DACCELERATION` bit reaches the runtime and that both version masks preserve
it. The masks do preserve bit 11; the bit is simply not present in these
records.

Three fields disagree with what `qxp_fill_operation_format` writes: size, bit
count and operations. So **these two records are not the driver's
operation-format entries**. Their shape - exactly the two display modes we
support, carrying `DISPLAYMODE` and nothing else, with no bit count - is what a
list built from the display mode enumeration would look like.

### The open question, stated precisely

Why does the stored array hold display-mode-shaped records instead of the
driver's operation-format entries? Either the driver's entries never reach the
builder's output, or the builder discards them and rebuilds from modes.

### Attribution caveat

The builder runs for both the v3 and the v4 attempt, and this measurement was
taken on `thread:01` while the attributable gate 2 measurement was on
`thread:02`. So this record content is **not yet attributed to the v4 build**.
It is nonetheless the proximate cause of the observed `GetDeviceCaps` failure,
because that call is the one the probe reports failing. Attributing it needs a
v4-only address downstream of `0x4fdfb8e0` and before the convergence at
`0x4fdfbaf4`.

## What the driver sends versus what the runtime stores

Two independent sources agree on the driver side.

The debug log of a live v4 run shows the format queries answered twice, once for
the v4 attempt and once for the v3 fallback, each round being one count query
followed by **27 record replies** (`QXPDISP FC` then 27 `QXPDISP F`).

The source agrees: `qxp_d3d_texture_formats` holds 44 entries, of which the
first 17 are legacy records and the remaining **27 are the operation records**
returned by `GETFORMATCOUNT`/`GETFORMAT`, which index `17 + i`
(`directdraw.c:233`, `15020-15038`, `15474-15491`). Query indices 0 and 2 are
`X8R8G8B8` and `R5G6B5`, carrying `DISPLAYMODE | 3DACCELERATION` plus a nonzero
`dwSize` and bit count (`directdraw.c:14902-14926`).

Nothing in the driver ever writes `DISPLAYMODE` alone, and no legacy red mask
equals `0x400`, so a union alias cannot manufacture the observed value.

**So the driver sends 27 records including the acceleration bit, and the runtime
stores 2 records without it.**

### The strip hypothesis, refuted by measurement

Static reading found a route that produces exactly `0x400`: at `0x4fdfbd69` the
builder tests for an existing `0x800`, calls a check at `0x4fdf78a0`, and on
failure clears only that bit at `0x4fdfbdaa`, leaving `0x400`.

That route does not run. Breakpoints at `0x4fdfbdaa` and then at the enclosing
block entry `0x4fdfbd69` were **never hit**; in both runs the guest ran to
completion and exited (`W00`) with the usual `hr=0x8876086a`. Four other builder
addresses have hit cleanly in other runs, so the instrument works and this is a
supported negative rather than an unexplained one. That block appears to be
reached only for adapter types 2 and 3.

### What remains unexplained

The observed records match **synthesized** records in two respects: the builder
zeroes the bit count when it synthesizes and never initialises `dwSize`, which
matches the observed `0` in both fields. But synthesis writes `0xc00`
(`DISPLAYMODE | 3DACCELERATION`) at `0x4fdfc0d5` and `0x4fdfc180`, not `0x400`.

So neither candidate explains all three fields:

| Candidate | `dwSize`/bitcount | `dwOperations` |
|---|---|---|
| our gathered records | would be 32 and 32/16 | would retain `0x800` |
| builder-synthesized | matches `0` and `0` | would be `0xc00`, not `0x400` |

The next measurement should read the record in memory at the moment of
synthesis, and then again after each subsequent pass over the array, to find the
instruction that clears bit 11. The v4-only addresses `0x4fdfb8e0` through
`0x4fdfba07` are available for attribution when that matters.

### A note on reading negatives

One run reported neither a hit nor a miss and looked like a tool fault. It was
not: the client printed `GUEST-EXITED W00` and the shell filter used to read its
output simply lacked that token. Capturing the unfiltered output resolved it
immediately. When a negative result is unexplained, widen the output before
doubting the instrument.

## The driver's data arrives intact: the loss is inside the runtime

Breaking at `0x4fdfbb8c`, immediately after `esi` is loaded with the gathered
buffer and before the filter at `0x4fdfbc60` or the mask at `0x4fdfbca4`, and
dumping consecutive `0x6c` records:

| Record | `dwSize` | `dwFourCC` | bit count | `dwOperations` |
|---|---|---|---|---|
| `0x24e258` | `0x20` | `0x16` X8R8G8B8 | 32 | **`0x4c1d`** |
| `0x24e2c4` | `0x20` | `0x15` A8R8G8B8 | 32 | `0x410d` |
| `0x24e330` | `0x20` | `0x17` R5G6B5 | 16 | **`0x4c1d`** |

`0x4c1d` decomposes to `OFFSCREENPLAIN | 3DACCELERATION | DISPLAYMODE |
SAMETEXTURE_RENDERTARGET | OFFSCREEN_RENDERTARGET | CUBETEXTURE | TEXTURE`,
which is exactly what `qxp_fill_operation_format` is given for those two
formats, with the correct sizes and bit counts.

**So the driver's format data reaches `d3d9.dll` complete and correct, bit 11
included.** Every remaining fault is inside the runtime's own processing.

Neither mask removes it: `0x4c1d & 0x1ddf` is `0xc1d` and `0x4c1d & 0x1fffddf`
is `0x4c1d`, both retaining `0x800`.

### Correction to an earlier entry

An earlier section in this document labelled the stored records as `0x17` =
X8R8G8B8 and `0x16` = R5G6B5. That is backwards. `D3DFMT_X8R8G8B8` is 22
(`0x16`) and `D3DFMT_R5G6B5` is 23 (`0x17`). The substance is unaffected, since
both are our two display formats and both carry `3DACCELERATION` on the wire,
but the labels there should be read swapped.

### The gap is now bounded

The gathered records have `dwSize = 0x20` and correct bit counts. The stored
records have `dwSize = 0` and bit count `0`. **They are therefore not the same
records**, which rules out "our records were stripped in place" and leaves the
stored pair as runtime-synthesized entries.

But the three synthesis sites in the builder, at `0x4fdfc0d5`, `0x4fdfc12c` and
`0x4fdfc180`, all write `0xc00`, and `eax` is not modified between them, so
synthesis emits `DISPLAYMODE | 3DACCELERATION`, not `DISPLAYMODE` alone. No
instruction in the builder range writes `0x400` into an operations field.

So the stored records match synthesized records in two fields and match no known
write in the third. The next measurement should therefore stop assuming these
records were produced by this builder at all, and instead confirm whether the
object `GetDeviceCaps` reads (`[ebp+0xc]`, fields `+0x158` and `+0x15c`) is the
same object this builder writes (`ebx`, same fields). If they are different
adapter objects, the entire search has been in the wrong function.

## The search was in the wrong function: two different adapter objects

`GetDeviceCaps` scans `[[ebp+0xc]+0x158]` and `[[ebp+0xc]+0x15c]`. The builder at
`0x4fdfb500` writes those same two offsets on `ebx`. It was assumed throughout
this investigation that those were the same object. **They are not.**

Measured, in the same run and the same process:

| Site | Object pointer |
|---|---|
| `GetDeviceCaps` scan, `esi` at `0x4fdf9775` | **`0x24926c`** |
| builder, `ebx` at `0x4fdfb87a` and `0x4fdfbb8c` | `0x24d8d8` |
| builder, `ebx` at `0x4fdfb8d0` and `0x4fdfc3d9` | `0x24d8c0` |

Two distinct pointers were already visible in earlier register dumps without
being noticed; a pointer does not move, so those were always two objects.

The object `GetDeviceCaps` reads holds count `2` and a list pointer, and its
records carry `dwOperations = 0x400` with zero size and bit count. The object the
builder fills receives our 27 gathered records, which are intact and carry
`0x4c1d` including the acceleration bit.

**This explains every failed candidate.** No instruction in the builder writes
`0x400`, no strip path executes, and the synthesis sites all write `0xc00` -
because the list being scanned was never produced by that builder at all.

### What this changes

The question is no longer "which instruction clears bit 11". It is:

1. What populates `0x24926c` at `+0x158` and `+0x15c`, and why does it hold two
   display-mode-shaped records?
2. Why does `GetDeviceCaps(D3DDEVTYPE_HAL)` consult that object rather than the
   one carrying the driver's format data?

A plausible reading, not yet evidence, is that the two objects correspond to
different device types or to a per-adapter object versus a per-device object,
and that the driver's gathered list is committed to one while the capability
query reads the other. Confirming that requires finding the writer of
`[0x24926c+0x15c]`, which a watchpoint would answer directly if the stub
supports one, and otherwise by locating other writers of `+0x15c` in the
disassembly.

### The methodological point

Three separate hypotheses inside the builder were each measured and refuted
before this was found. Every one of them was reasonable given the assumption
that there was a single adapter object, and that assumption was never itself
measured, only inherited. The refutations were not wasted, but the cheaper move
would have been to verify the shared premise first, since it was a single
register comparison and it invalidates the whole search space at once.

## Watchpoints also work under TCG

`Z2` arms successfully and stop replies carry `watch:<addr>`, so the interpreter
backend supports data watchpoints as well as breakpoints. This is worth
recording as a capability: it identifies *a writer* without having to guess
which function it lives in, which is precisely the assumption that misdirected
this investigation.

Watching the scanned object's list pointer caught six writers:

| RIP | Module | What it is |
|---|---|---|
| `0x77e93518` (x3) | system DLL | generic heap traffic; this address is reused memory |
| `0x7c910741` | ntdll | generic copy |
| `0x4fdfa6df` | d3d9 | `rep stos` clearing `0x834` dwords from `[ebx+0x30]` |
| `0x4fdf74ae` | d3d9 | bulk copy, below |

```asm
4fdf749f: mov  esi,[ebp+0xc]        ; source template
4fdf74a3: lea  edi,[ebx+0x13c]      ; destination spans +0x158 and +0x15c
4fdf74a9: mov  ecx,0x5e             ; 0x5e dwords, 376 bytes
4fdf74ae: rep movs                  ; the watchpoint fired here
4fdf74b0: xor  esi,esi
4fdf74b7: mov  [ebx+0x158],esi      ; then zeroes the record count
4fdf74bd: call 0x4fe0a440           ; allocates 0xd8 bytes
```

So both d3d9 writers are **initialisation**: one clears the object wholesale,
the other copies a template across the region and then explicitly resets the
count to zero. Neither is the write that stores a two-record list.

### Honest limit of this measurement

The six-hit window was consumed by initialisation traffic, so the store of the
two-record list happened outside it. This does not show that no such store
exists; it shows only that it is not among the first six writes. A longer hit
budget, or a watchpoint armed after initialisation has finished, is needed.

Note also that a watchpoint on a heap address catches every use of that memory,
including uses by unrelated code before or after it belongs to the adapter
object. Three of the six hits are exactly that. Watchpoint hits must be filtered
by module and by whether the object is live at the time, or they will generate
the same class of false lead this investigation has already produced twice.

## Identified: the scanned list is a runtime fallback, not our data

Filtering watchpoint stops to the `d3d9.dll` range (4 unrelated heap writes were
skipped) surfaced a third writer, `0x4fdf7547`, holding `edx = 0x200000`
(`DDPF_D3DFORMAT`). It sits in the same function as the template copy and is the
end of a short hand-built list:

```asm
4fdf7516: mov  ecx,0x400              ; D3DFORMAT_OP_DISPLAYMODE
4fdf751b: mov  [eax+0x58],ecx         ; record 0 dwOperations = 0x400
4fdf751e: mov  [eax+0xb8],edx         ; record 1 dwFlags = DDPF_D3DFORMAT
4fdf7524: mov  [eax+0xbc],0x16        ; record 1 dwFourCC = X8R8G8B8
4fdf752e: mov  [eax+0xc4],ecx         ; record 1 dwOperations = 0x400
4fdf7541: mov  [ebx+0x298],eax        ; list pointer
4fdf7547: mov  [ebx+0x294],0x2        ; count = 2
```

Record 1's fields are exactly `0x6c` above record 0's (`0xc4-0x58`,
`0xb8-0x4c`, `0xbc-0x50`), which is the record stride. Only flags, fourCC and
operations are written, so `dwSize` and the bit count stay zero.

**That accounts for all four observed properties**: two records, both display
formats, `dwOperations = 0x400`, and zero size and bit count.

### The two objects reconciled

`ebx + 0x294` is `(ebx + 0x13c) + 0x158`, and `ebx + 0x298` is
`(ebx + 0x13c) + 0x15c`. The sub-object at `+0x13c` is the template-copy
destination from `lea edi,[ebx+0x13c]`. Measured, `ebx = 0x249130`, and
`0x249130 + 0x13c = 0x24926c`, which is exactly the pointer `GetDeviceCaps`
scans.

So the runtime keeps a **built-in fallback format list** describing just the two
display modes, with no acceleration bit, in the adapter sub-object. Meanwhile
the real 27-record list derived from our driver, which does carry
`3DACCELERATION`, is built by `0x4fdfb500` against a *different* object
(`0x24d8d8` / `0x24d8c0`) and never reaches the scanned one.

`GetDeviceCaps(D3DDEVTYPE_HAL)` therefore scans the fallback, finds no record
with bit `0x800`, zeroes the caps and returns `D3DERR_NOTAVAILABLE`.

### The question this leaves

Why is the driver-derived list never committed into the scanned sub-object? The
builder does store a count and pointer at `+0x158`/`+0x15c` of *its* object, so
either that object is later copied into the adapter and the copy is skipped
under v4, or the builder is invoked against the wrong object in the first place.

This is now a single, well-posed question about one commit step, rather than a
search for a corrupting instruction. Note that the fallback is not evidence of a
driver fault: our data is correct and complete when it arrives, and the runtime
simply never adopts it.

## A commit path exists and is never taken for the queried object

`0x4fdf7310` deep-copies one adapter descriptor onto another, including the
format list:

```asm
4fdf7316: mov  ebx,[ebp+0x8]        ; destination adapter
4fdf7319: mov  eax,[ebx+0x15c]      ; free the destination's old list
4fdf7350: mov  esi,[ebp+0xc]        ; source adapter
4fdf7353: mov  ecx,0x5e
4fdf735a: rep movs                  ; copy 376 bytes, spanning +0x158/+0x15c
4fdf735c: mov  esi,[ebx+0x158]      ; record count, now the source's
4fdf7362: imul esi,esi,0x6c
4fdf7366: call 0x4fe0a440           ; allocate count * 0x6c
4fdf7372: mov  [ebx+0x15c],eax      ; install the new list
4fdf7378: je   0x4fdf7397           ; allocation failed -> count = 0
4fdf737f: mov  esi,[esi+0x15c]      ; source records
4fdf738c: rep movs                  ; deep copy them
```

So the runtime is perfectly capable of installing a driver-derived list into the
adapter the capability query reads.

**It never does so here.** With watchpoint stops filtered to the `d3d9.dll`
range, the only writers of the queried object's list pointer were the wholesale
clear at `0x4fdfa6df`, the template copy at `0x4fdf74ae`, and the fallback store
at `0x4fdf7547`. `0x4fdf7372` did not appear, and the window ran through to the
`GetDeviceCaps` scan itself, so it covers the entire period in which a commit
would have had to happen.

Meanwhile the builder does complete: the discard flag reads clear, so control
reaches `0x4fdfc3f3` and the list is stored - into **its own** object.

### Current best statement of the fault

The driver's 27 records arrive intact and carry `3DACCELERATION`. The builder
consumes them and stores a list on the object it was given. A different adapter
object, the one `IDirect3D9::GetDeviceCaps` consults, retains a built-in
fallback list of two display formats with `DISPLAYMODE` only. Nothing copies the
former into the latter, so the query finds no accelerated format and returns
`D3DERR_NOTAVAILABLE`.

This is a whole-object adoption failure, not a per-bit corruption. Every earlier
hypothesis in this document searched for a corrupting instruction, and all of
them were refuted, because the list being scanned was never derived from our
data in the first place.

### Caveat on the negative

"`0x4fdf7372` never fires" rests on one run with twelve reported stops and four
filtered ones. That is good coverage of the relevant window but it is still a
single run, and this investigation has twice been misled by absences. It should
be repeated before being treated as settled, ideally watching the builder's own
object so that a copy *out* of it would also be visible.

## Root cause: the adapter validation gate, and the checklist behind it

Every call site that adopts a driver-derived adapter descriptor is guarded by the
same predicate:

```asm
call 0x4fdf6d20
test eax,eax
je   <skip the commit>
```

seen at `0x4fdf759e`, `0x4fdfa42b` and `0x4fdfa570`. `0x4fdf6d20` is therefore
the single gate that decides whether our data is adopted at all. Its first act:

```asm
4fdf6d29: mov  esi,[ebp+0x8]
4fdf6d2c: test DWORD PTR [esi+0x8],0x2000000
4fdf6d33: je   0x4fdf6d3c          ; clean -> keep validating
4fdf6d35: xor  eax,eax             ; marked -> reject the adapter
4fdf6d3b: ret
```

`0x2000000` is exactly the value the capability builder stores on **both** of its
bail paths, at `0x4fdfb881` (gate 1) and `0x4fdfb8d4` (gate 2).

### The chain, end to end

1. `DevCaps2` lacks `D3DDEVCAPS2_STREAMOFFSET`, measured as `RAX = 0` at
   `0x4fdfb87a`.
2. The builder bails and writes `0x2000000` into the object's `+0x8`.
3. `0x4fdf6d20` sees that marker and returns zero.
4. Every adoption site is guarded by it, so the commit at `0x4fdf7310` is
   skipped. Measured: `0x4fdf7372` never fires.
5. The adapter keeps its built-in fallback list of two display formats carrying
   `DISPLAYMODE` only, written at `0x4fdf7516`-`0x4fdf7547`.
6. `GetDeviceCaps(D3DDEVTYPE_HAL)` scans that list for `3DACCELERATION`, finds
   none, zeroes the whole `D3DCAPS9` and returns `D3DERR_NOTAVAILABLE`.
7. The probe reports `rc=6 hr=0x8876086a stage=hal-caps caps2=0 devcaps=0`.

Every link is either measured in the running guest or read directly from the
disassembly. This supersedes all earlier hypotheses in this document.

### The rest of the gate is a v4 acceptance checklist

Continuing past the error marker, on the `D3DCAPS9`-shaped object:

| Offset | Field | Requirement |
|---|---|---|
| `+0x08` | `Caps` | private bit `0x2000000` must be clear |
| `+0x3c` | `TextureCaps` | nonzero |
| `+0x8c` | `FVFCaps` | nonzero |
| `+0x19c` | private | nonzero |
| `+0xb8` | `MaxVertexIndex` | **>= 0xfffe** |
| `+0xbc` | `MaxStreams` | nonzero takes the modern path at `0x4fdf6def`; zero enters a long legacy-compatibility sequence checking `MaxPointSize`, `+0xc4 == 0xfffe0000`, `+0xcc == 0xffff0000`, `VolumeTextureFilterCaps == 0`, and more |

We report `MaxStreams = 1`, so the legacy block is not ours and the checks that
matter are the five above plus whatever follows `0x4fdf6def`.

This turns the remaining work from a search into a checklist. It also explains
why forcing `STREAMOFFSET` alone did not help: it clears step 1, but any later
failing check writes the same marker or fails the same gate, and the adapter is
rejected just the same.

Note again that none of this is a driver data fault. Our 27 records are correct
and carry `3DACCELERATION`; they are simply never adopted because the capability
block is rejected first.

## Measured: the checklist passes and the gate still rejects

With `QXP_DIAGNOSTIC_DDI4` and `QXP_DIAGNOSTIC_STREAMOFFSET`, breaking at
`0x4fdf6d2c` and dumping every field the gate's opening checks read:

| Offset | Field | Value | Requirement | Result |
|---|---|---|---|---|
| `+0x08` | `Caps` | `0x41` | `0x2000000` clear | pass, **no bail marker** |
| `+0x0c` | `Caps2` | `0x20000000` | `DYNAMICTEXTURES` | pass |
| `+0x1c` | `DevCaps` | `0x9af50` | - | `0x1000000` clear, float check skipped |
| `+0x3c` | `TextureCaps` | `0x14c85` | nonzero | pass |
| `+0x8c` | `FVFCaps` | `0x4` | nonzero | pass |
| `+0xb8` | `MaxVertexIndex` | `0x100000` | `>= 0xfffe` | pass |
| `+0xbc` | `MaxStreams` | `1` | nonzero | pass, modern path |
| `+0xd4` | `DevCaps2` | `0x11` | nonzero | pass |
| `+0xf0` | `NumSimultaneousRTs` | `1` | nonzero | pass |
| `+0x19c` | private | `0x4fdff3e0` | nonzero | pass |

So with the stream offset bit set the builder does **not** bail, and every check
enumerated so far is satisfied.

**The gate rejects anyway.** A breakpoint on its reject epilogue `0x4fdf6de8`
hits four times, with `esi = 0x24d8c0`, the builder's object. So the failure is
in a part of the routine not yet read. Note also that `0x4fdfa433` never hits,
so the gate is reached from one of the other call sites, not the adoption path
inspected earlier.

### Where the failure most likely is

The routine is large: 74 separate branches lead to that one reject. Reading the
region past the checks above shows what it is doing there:

```asm
4fdf6e59: cmp  DWORD PTR [esi+0xcc],0xffff0200   ; PixelShaderVersion vs ps_2_0
4fdf6fa8: cmp  DWORD PTR [esi+0x100],0x20        ; VS20Caps.NumTemps
4fdf6fb5: cmp  DWORD PTR [esi+0x104],0x4         ; VS20Caps.StaticFlowControlDepth
4fdf6fc2: test BYTE PTR [esi+0xf8],0x1           ; VS20Caps.Caps predication
```

By the standard `D3DCAPS9` layout those offsets are `PixelShaderVersion`,
`VS20Caps` and `PS20Caps`. **The gate enforces shader-model capability
consistency**: a declared shader version obliges a matching, self-consistent set
of shader capability fields.

That is a coherent explanation for a rejection while every general field is
valid, and it is directly checkable: dump `+0xc4`, `+0xcc`, and `+0xf8` through
`+0x12c` at `0x4fdf6d2c` and compare against what the runtime demands for the
version we declare.

It also fits an older, then-puzzling result recorded above, that enabling the
SM2 capability diagnostic together with the v4 and stream offset gates did not
help: partial or inconsistent shader capabilities would fail this same routine
just as surely as declaring none.

## The exact blocker: D3DPRASTERCAPS_SCISSORTEST

Single-stepping the gate from its entry to its reject epilogue identified the
failing branch in one run, replacing a bisection over 74 candidate branches:

```
4fdf6d2c ... 4fdf6e39 4fdf6e3c 4fdf6e3e 4fdf6e41 4fdf6e46 4fdf6e49 -> 4fdf6de6
```

```asm
4fdf6e3e: mov  eax,DWORD PTR [esi+0x24]   ; RasterCaps
4fdf6e41: test eax,0x1000000
4fdf6e46: mov  DWORD PTR [ebp+0x8],eax
4fdf6e49: je   0x4fdf6de6                 ; bit clear -> reject the adapter
```

`0x01000000` is `D3DPRASTERCAPS_SCISSORTEST`. **The DDI v4 adapter gate requires
scissor test support**, and `qxp_fill_primitive_caps` does not advertise it
(`directdraw.c:14708`, which lists DITHER, ZTEST, SUBPIXEL, ZBIAS, FOGTABLE,
FOGVERTEX, FOGRANGE, ZFOG and MIPMAPLODBIAS).

This is the first concrete, implementable requirement found for v4 acceptance.

### Why the bit must not simply be set

The miniport already drives `GL_SCISSOR_TEST` and `glScissor`
(`qxpmp.c:6226-6249`), so the mechanism works and is proven by the Clear path.
But it is used **only** to bound `Clear` to its rectangle list: enable, set the
rectangle, clear, disable. `qxp_d3d_transport.h` contains no scissor fields at
all, so `D3DRS_SCISSORTESTENABLE` and `SetScissorRect` have no route from the
runtime to the driver, and no draw is ever scissored.

Advertising the cap now would therefore be a capability lie of exactly the kind
this project forbids, and it would be discovered by any application that sets a
scissor rectangle and expects clipping.

### The honest implementation

1. Add a scissor rectangle and enable flag to the draw request in
   `qxp_d3d_transport.h`, with an ABI bump.
2. Capture `D3DRS_SCISSORTESTENABLE` and the scissor rectangle in the display
   driver's render-state handling.
3. Apply them in the miniport's draw path, mirroring the existing Clear code
   including its render-target versus present Y-flip, which is already correct
   there and is easy to get wrong.
4. Prove it with a self-test phase in the style of the depth phase: draw a
   primitive that would cover a sample point, set a scissor rectangle that
   excludes it, and assert the pixel is unchanged; then move the rectangle to
   include it and assert it changes. Two-sided, exact pixels.
5. Only then advertise `D3DPRASTERCAPS_SCISSORTEST` and re-measure the gate.

Step 4 matters as much as the others: it is what converts the advertised
capability from a claim into evidence, and it is the same pattern that made the
depth phase trustworthy.

### Expect more requirements behind this one

The gate continues past this branch to compare `VertexShaderVersion` against
`0xfffe0200` and to validate the shader capability structures. Scissor is the
current blocker, not necessarily the last one. The single-step trace makes each
subsequent one a single run to identify.

## Breakthrough: the adapter is accepted and the runtime drives our DDI

With `D3DPRASTERCAPS_SCISSORTEST` advertised (and the stream offset diagnostic
still forcing `DevCaps2`), the validation gate no longer rejects us.

Single-stepping from the gate entry with `--trace-until` set to the reject
epilogue **never reaches it**. Instead the trace ends inside `0x4fdf738c`, the
`rep movs` in the commit function `0x4fdf7310` that deep-copies the format
records into the adapter. The adapter is being **adopted**, which is the step
that had never happened before.

End to end, the probe result moves:

| | Before | After |
|---|---|---|
| result | `rc=6` | `rc=10` |
| HRESULT | `0x8876086a` `D3DERR_NOTAVAILABLE` | `0x88760827` `D3DERR_DRIVERINTERNALERROR` |
| stage | `hal-caps` | **`clear-draw-present`** |

So stock `d3d9.dll` now accepts the adapter, **creates a HAL device**, and
issues real rendering commands to this driver. The guest log shows DDI traffic
that has never appeared before, including 101 `QXPDISP D3D` records plus
context, surface, present and shader operations, and a clean `QXPMP D3D CLOSE+`
at teardown.

This is the first time any part of this driver has been exercised through the
stock Direct3D 9 runtime rather than through `DrvEscape`.

### What is now honest, and what is not

**Honest:** `D3DPRASTERCAPS_SCISSORTEST`. Scissor is implemented in both draw
paths, wired to the real render state and DP2 rectangle command, and proven on
the GPU by self-test phase six with a two-sided exact-pixel oracle.

**Not honest yet:** `D3DDEVCAPS2_STREAMOFFSET` is still supplied by
`QXP_DIAGNOSTIC_STREAMOFFSET`, a diagnostic that asserts a bit we do not
implement. Stream offsets are not implemented, so this configuration must not
ship and is not a shipping claim. Implementing them is the next honesty debt,
and it is now the only capability lie standing between here and a legitimate v4.

The shipping package remains unchanged and still declines v4: verified
`GD9 FALLBACK` with zero diagnostic defines after restoring it.

### The remaining work is our own DDI v4 path

`D3DERR_DRIVERINTERNALERROR` at `clear-draw-present` is a failure inside code
that had never run before. Every earlier note in this document recording that
"no DP2 command is ever emitted", and that opcodes 71-76 and stream offsets were
unreachable, described a consequence of the rejection. That is no longer true:
DP2 commands now arrive, and the gaps they meet are real work rather than
blocked work.

## First real DDI v4 fault: SETRENDERTARGET2 target bind

With the adapter accepted, the guest log shows the runtime issuing DP2 commands
and the first failure is precise:

```
QXPDISP TARGET TRANSITION
0x00000023   render_target_handle
0x00000002   present_target_handle
0x00000000   present
0x00000023   texture_name
0x00000003   depth_handle
0x00000002   depth_name
QXPDISP DP2 TARGET2 FAILED
```

The failing opcode is `SETRENDERTARGET2` (85), handled at `directdraw.c:13203`.

The diagnostic is emitted *after* validation, and `texture_name` is nonzero, so
the render surface was found in our tables and `RTIndex` was zero. Both of the
handler's own rejection conditions therefore passed. The failure is the call
immediately after the log, `qxp_d3d_transport_target`, meaning **the miniport
rejected the bind**.

The most likely cause is `depth_format`. The miniport's bind-target handler
accepts only `D16`, `D24S8` and `D24X8` and fails closed otherwise, and the
transition log does not currently print the format, so the value the D3D9
runtime's depth surface carries is unknown. `depth_handle` is 3 and resolves to
`depth_name` 2, so the depth surface itself exists and has a texture name.

### Next step

Add `depth_format`, `width` and `height` to the `TARGET TRANSITION` diagnostic
and re-run. That converts this from a guess into a measurement, exactly as the
draw-rejection instrument did for the SM2 phases. Do not fix the format mapping
speculatively: the failing value should be read first, because the miniport
rejects for several distinct reasons and only one of them is the format.

This is the first genuine gap in our own DDI v4 implementation, as opposed to a
capability negotiation failure. It is reachable, reproducible and localised.

## Second DDI v4 fault: fixed-function draws have no vertex binding

With the depth-size bind fixed, the failure advances to the draw itself:

```
QXPDISP DP2 D3D9 DRAW GATE REJECTED
0x00000005
```

Code 5 is `QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BINDING`: the prepared draw has no
bound vertex shader. The probe's own stage name says the same thing from the
other side, `stage=clear-draw-present fixed-function`.

So the runtime is issuing a **fixed-function** draw, and our D3D9 draw path
requires a programmable vertex binding. That path was built for the Shader Model
2 work, where a vertex shader is always present.

### This is G03, previously recorded as unreachable

`docs/design/d3d9-ddi-v4-gap-matrix.md` lists G03, v4 FVF and `vs_1_1` through
the split opcodes 71-76, as unreachable because "no DP2 command is ever
emitted". That was a consequence of the adapter rejection, not a property of the
driver. DP2 commands now arrive, so G03 is reachable and is the next
implementation item rather than a blocked one.

The same applies to G02. Both were parked behind a rejection that no longer
happens.

### Order of work

Fixed-function support is the larger piece and gates any real application, since
almost no XP-era title starts by binding a `vs_2_0` shader. Stream offsets
remain the outstanding honesty debt and must be implemented before any of this
can ship, but they do not block progress on the draw path, because the
diagnostic keeps the adapter accepted while that work proceeds.

Neither is a research problem now. Both are ordinary implementation against a
runtime that talks to us and a fault reporter that names its own reasons.

### Measured: the declaration is an object, not an FVF

The gate refuses at `QxpD3d9DrawStateRequiresUnsupportedExecution`, which
returns TRUE whenever `declaration_mode != QXP_D3D9_DRAW_DECLARATION_NONE` or a
vertex function is bound. Direct3D 9 always sets a declaration, so every
fixed-function draw takes the Shader Model 2 preparation path and then fails for
want of a vertex shader.

The draw state already distinguishes `FVF` mode, carrying a `legacy_fvf` value
with no shader object, from `OBJECT` mode. Which one the runtime uses decides
how large this work is, so it was measured rather than assumed:

```
QXPDISP DP2 D3D9 DRAW GATE REJECTED
0x00000005   gate status: VERTEX_BINDING
0x00000002   declaration_mode: OBJECT
0x00000000   legacy_fvf: none
0x00000000   vertex shader handle: none
```

**Declaration object, no shader.** So this is not a routing relaxation. The
declaration object must be translated into fixed-function vertex processing, by
mapping its elements onto the legacy path's vertex layout, before such a draw
can execute.

`qxp_vdecl9.c` already parses D3D9 declarations, so the parsing exists; what is
missing is the mapping from parsed elements to the legacy fixed-function
pipeline and the routing of shaderless declaration draws onto it.

That is the next implementation task, and it is now specified by measurement
rather than estimated.

### Declaration to FVF translation landed; the object is not registered

Fixed-function draws now have a translation path: an FVF-expressible D3D9 vertex
declaration is converted to a legacy FVF and stride, cached on the declaration
object, published into the draw state, and the gate admits such a draw to the
legacy fixed-function path. The translation refuses anything it cannot express
exactly, including non-zero streams, non-default methods, unsupported usages,
wrong types, out-of-order offsets, gaps, overlaps and duplicate usages. Refusing
is essential: a wrong FVF renders garbage silently, which is worse than a
refused draw.

Verified with a full eight-mode cycle on the shipping package, all passing with
every counter baseline unchanged.

The v4 draw is still refused, and the reason is one layer deeper:

```
QXPDISP D3D9 DECL TRANSLATE MISS
0x00000000   declaration object found: no
0x00000000   object generation
0x00000000   binding generation
0x00000000   legacy_fvf_valid
0x00000000   legacy_fvf
0x00000000   legacy_vertex_stride
```

The draw state records `declaration_mode = OBJECT`, but the binding does not
resolve to any declaration object and its generation is zero. So the runtime's
declaration is not being registered as an object we can find: the translation
has nothing to translate.

The next step is to find how the D3D9 runtime creates the declaration under this
DDI and why our object table has no entry for it, then register it so the
existing translation can run. Measure before changing, as before; the binding
generation being zero suggests the binding is never populated rather than
populated with a stale handle, and those have different causes.

### A build-gate note

The first version of the diagnostic above passed the host `preinstall-test` and
then failed the WDK build with `C4242`, a 64-bit generation counter narrowed to
a 32-bit debug value under `/W4 /WX`. The host tests do not cover the driver
build, so the WDK build is the authority for driver code and must be run before
any claim that a change compiles.

### Correction: the declaration opcodes are handled

An earlier step in this investigation concluded that no handler existed for the
D3D9 split opcodes 71-76, on the strength of grepping `directdraw.c` for those
numbers and for direct assignments to `declaration_binding`. **That was wrong**,
and the method was at fault twice over:

- The opcodes are handled, under names that spell out their numbers elsewhere:
  `QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION` is 71,
  `..._SET_VERTEX_DECLARATION` is 73, and so on through 76
  (`qxp_d3d9_shader_objects.h:13-18`). They are called "private" only because
  the DX8 headers this driver compiles against do not declare them.
- `declaration_binding` is never assigned by name in `directdraw.c` because it
  is passed **by pointer** into `QxpD3d9ProcessShaderObjectBatch`
  (`directdraw.c:12857-12874`), which writes it from another module. Searching
  for `x.y = ` finds nothing when the write happens through a pointer.

This is the same failure mode recorded repeatedly in this document: concluding
from an absence in one file that a thing does not exist anywhere. The two
searches were consistent with each other and both wrong.

### The refined question

The runtime does drive the declaration opcodes, and `declaration_mode` becomes
`OBJECT` because a handle with the private bit is recorded. But the object
lookup finds nothing and the binding's generation is zero, which means the
binding was recorded without resolving to a live object.

So the question is no longer "are the opcodes handled" but: does opcode 71
CREATE arrive before opcode 73 SET, does the create succeed, and does the SET
resolve against the same owner the draw later uses? A generation of zero points
at a binding recorded without a successful create rather than at a stale handle.

The measurement is to report, inside the shader object batch, the opcode
sequence actually received and the create result, once per driver load.

### Correction: create and set both succeed

Reporting the first object batches settles the previous question and **overturns
the previous entry's conclusion**:

```
QXPDISP D3D9 OBJECT BATCH
0x00000047   opcode 71, CREATE_VERTEX_DECLARATION
0x00000001   one record
0x00000001   result: success
0x00000000   status: none
0x00000000   declaration_binding.handle   (create does not bind)
0x00000000   declaration_binding.generation

QXPDISP D3D9 OBJECT BATCH
0x00000049   opcode 73, SET_VERTEX_DECLARATION
0x00000001   one record
0x00000001   result: success
0x00000000   status: none
0x00000003   declaration_binding.handle
0x00000001   declaration_binding.generation
```

Both opcodes arrive, in the right order, and both succeed. The binding ends up
populated with a live handle and a nonzero generation.

The previous entry concluded from a zero generation that the binding had been
"recorded without a successful create". That was wrong. The diagnostic that
produced it logs **once** per driver load, and it fired on the refresh that runs
immediately after the create batch, when the binding is legitimately still
empty because create does not bind. A normal intermediate state was read as the
fault.

**A once-per-load diagnostic samples whichever occurrence happens first, which
is not necessarily the interesting one.** The draw-rejection instruments in this
driver get away with it because their first occurrence is the failure; a
diagnostic placed on a path that also runs during normal setup does not.

### What actually remains

The binding is live, so the gate's refusal must come from the translation: the
gate admits an OBJECT-mode draw only when a translated FVF and stride are
published, and publishes them only when the declaration is exactly FVF
expressible. The runtime's declaration is evidently being refused by
`QxpTranslateVertexDeclaration9ToFvf`.

That is the fail-closed behaviour working as designed, not a defect. The next
measurement is to report the declaration's elements and the specific refusal
reason, then decide per element whether it is genuinely inexpressible, in which
case fixed-function draws need a real vertex-processing path rather than an FVF
alias, or whether the translator is stricter than it needs to be.

## The remaining blocker is the capability lie itself

The declaration translation works. At the moment of refusal the draw state holds:

```
QXPDISP DP2 D3D9 DRAW GATE REJECTED
0x00000005   gate status: VERTEX_BINDING
0x00000002   declaration_mode: OBJECT
0x00000000   legacy_fvf
0x00000000   vertex shader handle: none
0x00000044   translated_declaration_fvf: XYZRHW | DIFFUSE
0x00000014   translated_vertex_stride: 20 bytes
0x00000003   declaration_binding.handle
0x00000003   translated_declaration_binding.handle (matches)
0x00000000   pixel_binding.sm2.generation: not SM2
0x00000001   streams[0].source2
0x00000001   streams[0].divider
0x00000001   streams[1].divider
```

The declaration is translated to a sensible transformed layout, the bindings
agree, and the pixel binding is not SM2. The refusal comes from
`streams[0].source2`, and `qxp_d3d9_draw_state.c:550` defines that field as
`state->streams[stream].source2 = offset != 0`.

**So the runtime is setting a stream source with a nonzero byte offset**, and
`QxpD3d9DrawStateRequiresUnsupportedExecution` correctly refuses it, because
stream offsets are not implemented.

### Why this is the right failure to have

The runtime uses stream offsets because we advertise
`D3DDEVCAPS2_STREAMOFFSET`, and we advertise it only because
`QXP_DIAGNOSTIC_STREAMOFFSET` asserts a bit for a feature that does not exist.
The claim was necessary to get past the v4 capability gate, and it has been
recorded throughout as the outstanding honesty debt and the reason this
configuration must not ship.

It has now produced exactly the failure that a capability lie is supposed to
produce: the runtime took us at our word, used the feature, and we could not
honour it. The refusal is fail-closed and correct; nothing renders wrongly.

### The next work item is therefore unambiguous

Implement stream offsets properly: carry the per-stream byte offset through the
transport, apply it when binding vertex data, and prove it with a self-test
phase that draws the same geometry from the same buffer at two different offsets
and asserts different, exact results. That single piece of work both retires the
last capability lie and unblocks the first fixed-function draw, which is a
better position than having to choose between them.

## The first draw reaches the GPU

With stream offsets implemented, the transport refusals fall in sequence. Each
was a separate place that read a binding from the legacy arrays, which a
`SETSTREAMSOURCE2` binding deliberately does not populate:

- reject 2, stride smaller than the decoded vertex. The draw state held the
  correct stride of 20 for `XYZRHW | DIFFUSE`, but the transport read the legacy
  array, found zero, and fell back to `dwVertexSize`, which was 1.
- reject 6, no vertex source. Same cause, for the stream handle.
- reject 7, wrapping source. A D3D9 vertex buffer reaches this driver as a
  system-memory execute buffer, which is exactly how a legacy circular vertex
  pool appears, so the capability bits cannot distinguish them.

The last one deserves care rather than a blanket allowance. The wrap machinery
reconstructs a vertex straddling the end of a circular pool. A stream bound at
a byte offset is read linearly, and the range check proves the whole span lies
inside the surface before any read, so where that check passes a linear read is
exactly correct and wrapping cannot occur. Where it fails the draw is refused
rather than wrapped, because a wrapped read of an offset stream is not a meaning
D3D9 assigns.

With those three fixed, the guest log shows `QXPMP D3D DRAW` three times,
`DRAW+`, `CLEARS` and four `PRESENTS`. **A draw issued by the stock XP Direct3D
9 runtime has executed on the host GPU.** Every previous pixel in this project
came from `DrvEscape` self-tests.

### The next fault, measured not guessed

```
QXPMP D3D TARGET REJECT
0x00000010   reason 16: existing depth mismatch
0x00000003   texture_name
0x00000320   width 800
0x00000258   height 600
0x00000024   depth_name 36
...
0x00000040   existing depth width 64
0x00000040   existing depth height 64
```

This is the inverse of the fault fixed earlier, where a 64x64 target was refused
against an 800x600 depth and the comparison was relaxed to require only that the
depth be at least as large. Here the target is 800x600 and the depth is recorded
as 64x64, which is genuinely too small; D3D9 requires the depth to be at least
the render target in each dimension, so refusing is correct **if the record is
accurate**.

That is the open question, and it must be measured rather than assumed. Either
the depth surface really is 64x64, left over from the dynamic texture stage, in
which case the runtime is binding a mismatched pair and the driver is right to
refuse; or the surface has since been recreated at 800x600 and our recorded
dimensions are stale, in which case the driver is refusing a valid bind. The
distinguishing evidence is the surface's own dimensions at bind time, not the
dimensions we cached when we first saw it.

## Binding the target lazily, and what that broke

The depth refusal was not a depth bug. Printing the bind transitions showed the
runtime setting colour and depth as two separate DP2 commands while the driver
bound eagerly on each, so a mismatched pair was always visible between them:

```
(35, 2, 0, 35, 3,  2)   colour -> 35 (64x64), depth still 2 (800x600)
(35, 2, 0, 35, 36, 36)  depth  -> 36 (64x64); pair consistent
(2,  2, 1, 3,  36, 36)  colour -> primary (800x600), depth still 36 (64x64)  refused
```

The first line is the same defect in its harmless direction, and an earlier
change relaxed the miniport comparison so that case would pass. **That
relaxation treated a symptom of this bug.** D3D9 requires the pair to be valid
at draw time, not between two state commands, so the fix is to record intent and
bind before anything that observes the target.

Two real defects were fixed underneath it: the depth renderbuffer was allocated
and recorded at the *render target's* dimensions rather than the depth surface's
own, so a depth first seen against a 64x64 target became a physically 64x64
renderbuffer. The surface's true dimensions are now transmitted (ABI 57) and a
changed size recreates the renderbuffer.

### The regression this caused, and why the oracle mattered

Deferred binding failed the multi-context oracle. **The miniport binds one
target per device, not one per context.** Before deferral, every
`DrawPrimitives2` rebound its context's target unconditionally, which silently
guaranteed the right target was current; dropping that let one context draw into
whichever target another context had bound last. The per-context "unchanged"
test was not sufficient, so the device binding owner is now tracked and a switch
between contexts counts as dirty.

This is the strongest argument yet for the existing oracles. The change passed
every unit test, the WDK build, and six of eight acceptance modes; only the
multi-context visual comparison caught it. Had that oracle not existed, this
would have shipped as an intermittent wrong-target bug of the kind that is
nearly impossible to diagnose from a bug report.

### The next gap

With those fixed the run shows no driver refusal of any kind, and the failure is
now a command we have never implemented:

```
QXPDISP DP2 UNKNOWN
0x00000026   opcode 38, D3DDP2OP_TEXBLT
```

`TEXBLT` is how the runtime uploads dynamic texture content, which is exactly
what the probe stage before the failure was doing. A surface copy transport
already exists and is the obvious route.

## TEXBLT, and a refusal that is correct

Opcode 38, `D3DDP2OP_TEXBLT`, is now implemented: records are parsed and each is
validated and routed through the existing surface-copy transport, with thirty
distinct reason codes and one-shot reporting. Only standalone level-zero 2D
textures are handled; cube, complex and mipmapped resources are refused rather
than approximated, because the request names a texture but carries no level or
face selector and guessing would write incorrect texels silently.

The probe now reaches TEXBLT and is refused for reason 15, source format:

```
QXPDISP DP2 TEXBLT FORMATS
0x0000000F   reason 15: source format
0x00000001   source format_valid: yes
0x0000003C   source format: 60
0x00000004   4 x 4
0x00000004
0x00000001   destination format_valid: yes
0x0000003C   destination format: 60
0x00000001   source is system memory
0x00000000   source is not mipmapped
```

Format 60 is `D3DFMT_V8U8`, and the first instinct was that the resolver had
misclassified a colour texture. It had not: `qxpd3d9probe.c:780` queries
`D3DUSAGE_QUERY_LEGACYBUMPMAP` for `D3DFMT_V8U8`, so this is the probe's bump
mapping coverage and the format is genuinely what it says. Both surfaces agree
on it and both resolved cleanly. The refusal is correct: the copy transport
accepts only three colour formats, so refusing a fourth is the honest outcome
rather than a defect.

### The next step is smaller than it looks

The source is system memory. A TEXBLT from a system-memory staging surface to a
default-pool texture is not really a host-side blit at all; it is an upload, and
this driver already uploads texture levels for sampling in a format-agnostic
way. Implementing that case as a sub-rectangle upload from the source's guest
memory avoids the copy transport's colour-format restriction entirely, and
avoids asking a host framebuffer blit to operate on a format that may not be
colour-renderable.

The host blit remains the right mechanism for video-to-video copies. The two
cases are genuinely different operations that happen to share one DDI command,
and treating them identically is what produced a restriction that does not need
to exist.
