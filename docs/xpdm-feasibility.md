# Step 5 / Phase 2 — thin XPDM display driver: feasibility (2026-07-10)

**Question:** is the "miniport + display DLL whose only jobs are mode-set, flat
framebuffer, and answering the OpenGL ICD-discovery escape" (`README.md:82-84`)
buildable at moderate effort, and from which base?

**Inputs:** four source-mapper passes over `refs/virtualbox/.../Video/{mp,disp}` (XPDM
subtrees), `refs/reactos/win32ss/drivers/{displays,miniport,videoprt}`, and the
adapter-selection question against qemu-libretro; reconciled against
`docs/icd-fallback-analysis.md` (loader-side escape contract). Load-bearing
negatives and line refs below were re-verified by direct grep/read this session,
not taken on faith from the mappers.

**Verdict up front: feasible, moderate (~3-5 calendar weeks, ~3.0-3.5k LOC of which
only ~150-250 LOC is genuinely new code), with two corrections to the README plan:**
(1) the crib base should be ReactOS `framebuf`+`bochs`, *not* the VirtualBox pair the
README ranks first — the VBox tree contains **zero** escape-answering code and is ~4×
the size; (2) the rig should move from `-vga cirrus` to `-vga std`.

---

## 1. Minimal driver scope — the complete job list

Two binaries plus an INF. Working names: `qxpmp.sys` (miniport), `qxpdisp.dll`
(display driver). The ICD itself (`qxpicd.dll`) is Step 4's deliverable and is *not*
part of this driver — the driver only names it.

### Miniport (`qxpmp.sys`) — must do

| Job | Mechanism |
|---|---|
| Adapter detection | `HwFindAdapter`: probe Bochs VBE DISPI ID at I/O ports 0x1CE/0x1CF, read VRAM size, `VideoPortGetAccessRanges` for the LFB physical address (pattern: `refs/reactos/win32ss/drivers/miniport/bochs/bochsmp.c:126-173`; same probe in `refs/virtualbox/.../mp/xpdm/VBoxMPDriver.cpp:88-103`) |
| Mode table | Static resolution matrix × bpp (8/16/32), filtered by "fits in VRAM" only — no host negotiation (the VBox equivalent's VMMDev round-trips collapse to constants on QEMU) |
| Mode set | ~10 word-pokes to `VBE_DISPI_INDEX/DATA` — a complete working reference for this exact hardware concept is `refs/vmdisp9x/vxd_vbe.c:220-284` (`VBE_setmode`), register defs `refs/vmdisp9x/boxvint.h:89-127` |
| System VRPs | `IOCTL_VIDEO_MAP/UNMAP/SHARE/UNSHARE_VIDEO_MEMORY`, `SET/QUERY_CURRENT_MODE`, `QUERY_NUM_AVAIL_MODES`/`QUERY_AVAIL_MODES`, `RESET_DEVICE`, `SET_COLOR_REGISTERS` (8bpp DAC writes) |
| Power / reset | `HwSetPowerState`/`HwGetPowerState` stubs returning `NO_ERROR`; `HwResetHw` disables VBE extended mode and returns FALSE (fall back to VGA BIOS text mode) — both shapes verbatim from `VBoxMPDriver.cpp:605-694` |

### Display DLL (`qxpdisp.dll`) — must do

| Job | Mechanism |
|---|---|
| DDI registration | `DrvEnableDriver` returning a ~10-12-entry `DRVFN` table (the unbuilt MS DDK sample `displays/vga_new/enable.c` proves 10 entries suffice: EnablePDEV/CompletePDEV/DisablePDEV/EnableSurface/DisableSurface/AssertMode/SetPalette/MovePointer/SetPointerShape/GetModes) |
| Mode enumeration | `DrvGetModes` round-tripping `QUERY_NUM_AVAIL_MODES`/`QUERY_AVAIL_MODES` to the miniport (`refs/virtualbox/.../disp/xpdm/VBoxDispMini.cpp:48-151` is the clean template) |
| LFB exposure | `DrvEnableSurface`: `IOCTL_VIDEO_SET_CURRENT_MODE` + `IOCTL_VIDEO_MAP_VIDEO_MEMORY`, then `EngCreateBitmap(..., BMF_TOPDOWN, FrameBufferBase)` + `EngAssociateSurface` — GDI draws directly into VRAM; the driver implements **no** drawing code (`VBoxDispDriver.cpp:679-713` and `displays/framebuf` both demonstrate this) |
| **The ICD escape** | `DrvEscape` answering `QUERYESCSUPPORT(OPENGL_GETINFO)` and `OPENGL_GETINFO` — §4. **This is the entire point of Phase 2 and the only genuinely new code.** |
| 8bpp palette | `DrvSetPalette` → `SET_COLOR_REGISTERS` (needed because 8bpp modes are advertised; drop the modes if we'd rather drop the code) |

### INF + registration

- Bind `PCI\VEN_1234&DEV_1111` (std-vga; see §2). Template: `refs/virtualbox/.../mp/xpdm/VBoxVideo.inf` — verified structure: HW match at `:47`, `InstalledDisplayDrivers` at `:83`, `Acceleration.Level` at `:85`.
- The same INF's `AddReg` writes `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd` (`Dll`, `Version`, `DriverVersion`, `Flags`) — closing the loop that lets stock `opengl32.dll` find `qxpicd.dll` with no `system32\opengl32.dll` swap (`docs/icd-fallback-analysis.md` §1; measured values in §4 below).

### Explicitly NOT needed

- **No 2D acceleration.** No hooked `DrvBitBlt`/`DrvTextOut`/etc. — GDI's engine renders into the `EngCreateBitmap` surface itself. `Acceleration.Level` is irrelevant to us; VBox's 611-LOC `VBoxDispDrawCmd.cpp` exists only to feed VBVA/VRDP dirty-rects, which we don't have.
- **No DirectDraw HAL.** Phase 3's cheap path (WineD3D `ddraw/d3d8/d3d9` in system32 translating to GL, `README.md:95-99`) bypasses the DDraw HAL entirely; apps that insist on DDraw get Microsoft's software HEL. `displays/framebuf` ships DDraw entry points as empty stubs (`enable.c:44-72`) — keep them stubbed.
- **No D3D DDI.** Phase 3's "real path" only; explicitly deferred (`README.md:100-104`).
- **No hardware cursor.** std-vga has no cursor plane; leave `DrvSetPointerShape` returning `SPS_DECLINE` (or unhooked) and the GDI engine draws a software cursor. VBox's cursor path is HGSMI-routed and unportable anyway.
- **No VBVA/HGSMI/VMMDev, no VRDP, no DualView/multi-head, no dynamic host-driven resize.** All VirtualBox-protocol machinery with no QEMU transport (≈4,500+ LOC of the VBox tree evaporates on this decision alone).
- **No NT4/Win2k compat branches.** Target XP only: hardcode the XP `VIDEO_HW_INITIALIZATION_DATA` size, delete the NT4 `VideoPortGetProcAddress` fallbacks.

---

## 2. Adapter: `-vga std` (Bochs VBE), not `-vga cirrus`

Recommendation from the adapter mapper, adopted here: **target std-vga
(`PCI\VEN_1234&DEV_1111`) and change launch configurations from `-vga cirrus`
to `-vga std`.**

| | `-vga cirrus` (rig today) | `-vga std` — **recommended** |
|---|---|---|
| PCI ID | `1013:00B8` (real CL-GD5446) | `1234:1111` (verified: qemu-libretro `include/hw/pci/pci.h:55-57`) |
| In-box XP driver | **Yes** — running at boot; ours must dethrone a working incumbent (Have Disk, driver-ranking) | **No** — falls to "Standard VGA"; our exact-ID INF wins a clean first install (the VirtualBox private-ID pattern, `VBoxVideo.inf:47`) |
| Mode-set surface | Full Cirrus register model: banked FB, extended CRTC/SR/hidden-DAC quirks | ~10 port pokes, flat LFB (`vxd_vbe.c:220-284`) |
| Working reference in `refs/` | None for XPDM | ReactOS `miniport/bochs` (XPDM, same register family) + vmdisp9x VBE code |
| VRAM | **4 MiB** on the rig (verified: `vgamem_mb` device default 4, `hw/display/cirrus_vga.c:2986-2987`; accelerated `real_vram_size` hardcoded 4 MiB for CLGD5446, `:2921`). The `"vgamem_mb","8"` at `hw/core/machine.c:240` is a `hw_compat_2_8[]` entry — applies only to machine types pc-2.8 and older (`pc_piix.c:728`); the rig sets no machine type, so it runs the default `pc-i440fx-9.1` (`pc_piix.c:482-486`) where it never fires. 4 MiB can't even hold 1280×1024×32 | 16 MiB default (`hw/display/vga-pci.c:334`), uncapped |
| Build/core support | Both compiled into the libretro core; `-vga` is a per-VM text-file flag, not code | same |
| ICD escape work | identical | identical |

**Consequences for the rig:** all three launch configs
(`qemu/winxp/{Windows XP,Windows XP (3dfx),Need For Speed Underground 2}.qemu_cmd_line`
— verified, each contains `-vga cirrus`) need the one-token edit to `-vga std`.
Host-side analysis found
no coupling between the 3dfx/mesa passthrough and the VGA device type (hw/mesa and
hw/3dfx register no display PCI IDs; `ui/libretro-3dfx.c` only sizes its hidden window
from the generic VGA surface), so no passthrough regression is expected — but verify
empirically on first boot after the switch. Note XP will re-run PnP ("New Hardware
Found") on the changed adapter; on the disposable image copy that's a feature, not a
risk. If the switch is ever vetoed, the fallback is targeting cirrus's Bochs-VBE
extension — but whether QEMU's cirrus answers the full DISPI protocol at 0x1CE/0x1CF
is **unverified in this fork** and would need checking against
`hw/display/cirrus_vga.c` first; the std-vga route avoids the question entirely.

---

## 3. Crib plan — file by file

### Base decision: ReactOS `framebuf` + `bochs`, with VBox and the MS DDK sample as reading references

`README.md:85-90` ranks the VBox pair first "in order of usefulness." The mapper
evidence reverses that:

- **Size:** VBox's XPDM-relevant subtree is ≈11,670 LOC (disp/xpdm 7,066 + mp/xpdm
  2,443 + mp/common 2,190), of which HGSMI/VBVA/VMMDev/VRDP/DualView — none portable —
  is the majority. ReactOS `displays/framebuf` (1,929 LOC) + `miniport/bochs` (835
  LOC) = **2,764 LOC** (LOC re-verified this session), both built by ReactOS CI today,
  both GPL, and `bochs` already speaks the exact DISPI registers std-vga exposes.
- **The escape:** neither base contains it (§4), so "VBox has the escape code to crib"
  — the implicit reason for ranking it first — is false.
- VBox remains valuable as the *XP-era production details* reference: the
  `VBoxDispMini.cpp` IOCTL wrappers, `DrvEnableSurface` sequencing, the DrvEscape
  switch *shape*, INF registry keys, `HwResetHw` semantics, and the
  `HwInitDataSize`-per-OS gotcha.
- ReactOS `displays/vga_new` + `miniport/vga_new` (MS NT4 DDK sample source, 1,824 +
  4,415 LOC) is orphaned from the ReactOS build (`miniport/CMakeLists.txt:5` commented
  out) — use as a second-window cross-check of DDI usage, not as the fork base.
- ReactOS `videoprt/` (9,524 LOC) is a **reading aid only** for the
  `VideoPortXxx`/`IOCTL_VIDEO_*` contract — on real XP the driver loads against
  Microsoft's `videoprt.sys`, and only the documented public contract may be assumed.

### Miniport `qxpmp.sys` — from `refs/reactos/win32ss/drivers/miniport/bochs` (835 LOC)

| Piece | Action | Notes |
|---|---|---|
| `bochsmp.c` DISPI probe + FindAdapter/Initialize/StartIO/SetCurrentMode/QueryAvailModes (`:50-64,126-173,444-643`) | **Keep** | The whole file is on-target |
| Mode table | **Extend** | Widen the resolution×bpp matrix, filter by detected VRAM; crib the *structure* (not the VMMDev calls) of `VBoxMPVidModes.cpp` |
| `SHARE/UNSHARE_VIDEO_MEMORY`, `SET_COLOR_REGISTERS`, power stubs, `HwResetHw` | **Add** | Small, shapes verbatim from `VBoxMPDriver.cpp:605-694` / `VBoxMPIOCTL.cpp` |
| Registry helpers | **Add if needed** | `VBoxMPRegistry.cpp` (71 LOC) is a zero-dependency `VideoPortGetRegistryParameters` wrapper — crib verbatim |

Estimated result: **~900-1,200 LOC.**

### Display DLL `qxpdisp.dll` — from `refs/reactos/win32ss/drivers/displays/framebuf` (1,929 LOC)

| Piece | Action | Notes |
|---|---|---|
| `enable.c` DDI table, PDEV/surface lifecycle, `DrvGetModes`, `DrvAssertMode` | **Keep** | Cross-check sequencing against `VBoxDispDriver.cpp:593-750` and `vga_new` |
| Palette code | **Keep** | Needed for 8bpp modes |
| DirectDraw stubs (`enable.c:44-72`) | **Keep as stubs** | Harmless; placeholder if Phase 3 ever wants a HAL |
| Pointer functions | **Simplify** | `SPS_DECLINE` / engine software cursor |
| **`DrvEscape`** | **Write new** (~100-150 LOC) | §4. Not present in framebuf at all (no `INDEX_DrvEscape` in its table — add the table entry too) |

Estimated result: **~2,000-2,200 LOC.**

### Packaging

| Piece | Action |
|---|---|
| `qxp.inf` | **Write from `VBoxVideo.inf` template**: HW ID → `PCI\VEN_1234&DEV_1111`, service + `InstalledDisplayDrivers` → qxpdisp, `Acceleration.Level` optional, plus the `OpenGLDrivers\qxpicd` AddReg (§4) |
| Deploy | Mount an ISO containing the driver package, then use Device Manager → Update Driver → Have Disk |

**Total estimated driver: ~3,000-3,500 LOC**, of which the escape handler + ICD
registry glue (~150-250 LOC) is the only code without a working open-source model to
crib from.

---

## 4. The ICD escape answer (the actual point of Phase 2)

### Negative result, reconciled and re-verified

**Neither crib source implements the escape.** Verified this session by grep over the
full trees: `OPENGL_GETINFO`/`QUERYESCSUPPORT`/`Drv_Opengl_Info` appear **nowhere**
under `refs/virtualbox/.../Video` (miniport *or* display), and
`DrvEscape`/`OPENGL_GETINFO`/`QUERYESCSUPPORT` appear **nowhere** under
`refs/reactos/win32ss/drivers`. VBox's XPDM `DrvEscape`
(`disp/xpdm/VBoxDispDriver.cpp:864-978`) handles exactly three cases —
`VBOXESC_ISVRDPACTIVE` (`:871`), `VBOXESC_SETVISIBLEREGION` (`:881`), and
`VBOXESC_ISANYX` (`:951`, an `IOCTL_VIDEO_VBOX_ISANYX` passthrough) — none OpenGL-related. VirtualBox's
actual Windows OpenGL delivery (`Additions/3D/win/VBoxICD`) is a **WDDM-only wholesale
`opengl32.dll` replacement** (picks its backend via `D3DKMTQueryAdapterInfo`, a
Vista+ API) — i.e., the shipping vendor answer to XP-era GL was the same
"swap opengl32.dll" technique `README.md:76-80` lists as our interim hack. The escape
handler is therefore **greenfield code built from the loader-side contract**, using
VBox's switch shape only as a structural template. This corrects the README's implicit
premise that the VBox pair contains this logic to crib.

### The exact contract (loader side, `refs/reactos/dll/opengl/opengl32/icdload.c` — re-verified by direct read)

Stage 1 — support probe (`icdload.c:142-147`):

```c
DWORD dwInput = OPENGL_GETINFO;                    /* 0x1101 */
ret = ExtEscape(hdc, QUERYESCSUPPORT /* 8 */, sizeof(DWORD), &dwInput, 0, NULL);
/* ret <= 0  =>  loader gives up silently => software GL (today's std-vga state) */
```

Our `DrvEscape` must: on `iEsc == QUERYESCSUPPORT`, read the `ULONG` in `pvIn` and
return nonzero **iff** it equals `OPENGL_GETINFO` (or another escape we support), else
0. Returning nonzero indiscriminately, or 0 for `OPENGL_GETINFO`, are the two failure
modes that leave the system permanently in "no ICD" state.

Stage 2 — info query (`icdload.c:149-157`; struct at `icdload.c:15-20`, mirrored by
Mesa's `gldrv.h:562-587`):

```c
/* in:  DWORD ulSubEsc = 0 (OPENGL_GETINFO_DRVNAME)      */
/* out: 0x214-byte buffer, filled with:                  */
typedef struct {
    ULONG Version;                  /* interface version   */
    ULONG DriverVersion;            /* driver version      */
    WCHAR DriverName[MAX_PATH + 1]; /* ICD registry name   */
} Drv_Opengl_Info;                  /* -> {2, 1, L"qxpicd"} */
```

Return >0. Precedent for the values: vmdisp9x answers `{0x2, 0x1, "QEMUFX"}` on the
9x side (verified: `refs/vmdisp9x/control.c:84-119` — note the 9x struct is the ANSI
`char DLL[262]` variant; the NT-side struct is WCHAR per ReactOS/Mesa).

Stage 3 — registry pairing (`icdload.c:36,180-257`; `docs/icd-fallback-analysis.md:40-41,58-65`):

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd
    Dll           REG_SZ     qxpicd.dll
    Version       REG_DWORD  2      ; must equal escape's Version
    DriverVersion REG_DWORD  1      ; must equal escape's DriverVersion
    Flags         REG_DWORD  1      ; required by stock XP; compatibility bit
```

The loader cross-checks registry values against the escape's; **mismatch = silent
rejection** (`icdload.c:222-242`). ReactOS reads `Flags` but treats it as optional;
stock XP required the value to be present in the real-XP battery, so the INF freezes
it to 1. Then
`LoadLibrary(qxpicd.dll)` → optional `DrvValidateVersion(DriverVersion)` → 16
mandatory `Drv*` exports resolved → `DrvDescribePixelFormat` count cached. From there
Step 4's refusal semantics take over (`docs/icd-fallback-analysis.md` §2-3).

Everything above must be internally consistent across three places we control: the
`DrvEscape` answer, the INF's AddReg, and `qxpicd.dll`'s export surface (all 19 `Drv*`
per `mesa9x/mesa3d.def`, per `icd-fallback-analysis.md:112-114`).

### Real-XP closure and remaining driver-specific uncertainty

The user-mode T16 results below resolve the loader tuple and buffer contract.
Driver-specific routing remains T17:

1. **`Version` value — resolved:** matched `{1,1}` loads/resolves but does not enter
   the ICD on stock XP; matched `{2,1}` does. Use interface `Version=2`,
   `DriverVersion=1`, and registry `Flags=1`. `DrvValidateVersion` receives 1. (**T16 passed**)
2. **Exact out-buffer size — user-mode side resolved:** stock XP supplied at least
   the 0x214-byte structure accepted by the narrow ExtEscape substitute. The real
   driver should still accept `cjOut >= sizeof(struct)` and avoid over-copying.
   (**T16 passed; kernel/display routing remains T17**)
3. **Registry pairing — resolved:** mismatched versions and a missing `Flags` value
   were silently rejected by stock XP. Keep all four values exactly as above.
4. **Which DCs get the escape:** ReactOS sends it on first WGL touch per window/memDC;
   real XP's win32k may route memDC escapes differently. Observe with DebugView
   logging in `DrvEscape`. (**T17**)
5. **`QUERYESCSUPPORT` input width:** loader passes `sizeof(DWORD)`; be tolerant of
   `cjIn >= 4`.
6. T3/T11 are closed: XP retries failed validation and re-queries formats for a new
   window in the same process. The actual driver must still pass the same test once
   it replaces the in-process ExtEscape substitute.

---

## 5. Toolchain & debug

### Build

- **WDK 7.1.0 (7600.16385.1) or Windows Server 2003 SP1 DDK (3790.1830)** — both free
  ISOs, both build XPDM (`README.md:91-92`). WDK 7.1 is the
  pragmatic pick (installs cleanly on modern Windows; `build.exe` env: *Windows XP x86
  Free Build Environment*).
- Miniport: `TARGETTYPE=DRIVER`, links `videoprt.lib` (+ `ntoskrnl`/`hal` as needed);
  entry `DriverEntry` → `VideoPortInitialize`.
- Display DLL: `TARGETTYPE=GDI_DRIVER`, links `win32k.lib`; **linker entry point is
  `DrvEnableDriver@12`, not DllMain**, and no EXPORTS needed beyond it — the DDI table
  is returned at runtime (confirmed by VBox's `disp/Makefile.kmk:55-56` and
  `VBoxDisp.def`).
- Sources are C; the VBox files are `.cpp` but C-style — cribbing into plain C avoids
  DDK C++ friction.
- No signing requirement exists for XP 32-bit kernel drivers. The only gate is the
  unsigned-driver *warning* dialog: set Driver Signing policy to "Ignore" (System
  Properties → Hardware → Driver Signing, or
  `HKLM\SOFTWARE\Microsoft\Driver Signing\Policy` = 0) in the disposable image, then
  install via Device Manager → Update Driver → Have Disk with the deploy-ISO mounted.
- **Display-driver bricking safety net:** boot menu F8 → "Enable VGA Mode" / Last
  Known Good. Use a disposable image copy and snapshot the qcow2 before first install.

### Kernel debug — WinDbg over QEMU serial named pipe

The rig has **no serial device today** — verified: all three
`qemu/winxp/*.qemu_cmd_line` files contain no `-serial` (they do carry
`-qmp tcp:127.0.0.1:44553` on the 3dfx/NFSU2 configs, which
stays for the deploy loop). **Add to the cmd_line:**

```
-serial pipe:qxpdbg
```

(QEMU on Windows creates `\\.\pipe\qxpdbg`.) Host side:

```
windbg -k com:pipe,port=\\.\pipe\qxpdbg,resets=0,reconnect
```

Guest side, `C:\boot.ini`, cloned entry:
`/debug /debugport=COM1 /baudrate=115200`. Keep the non-debug entry as default so
normal EmuVR boots are unaffected. Caveats: WHPX + kernel-debug stubs is the rig's
real config — if breakpoints misbehave under WHPX, retest under pure TCG (drop the
`-accel whpx` token) before blaming the driver. `Ctrl+Alt+D` in the guest /
`.dump`/`!analyze` flows all work over pipe serial; this is the standard XP-era recipe.
Display drivers additionally honor the `Ex`/`Eng` debug print path — `DbgPrint` from
the miniport and `EngDebugPrint` from the display DLL both land on the kernel debugger.

---

## 6. Risks (ranked) and effort

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Actual XPDM escape routing differs by DC** (the values, registry contract, and stock loader are now measured; §4 item 4 remains) | Low | Blocks Phase 2 until corrected, but cheap to detect | DebugView logging in `DrvEscape`; T17 across window and memory DCs |
| 2 | **Broken display driver bricks boot** during bring-up | High (at least once) | Low (rig is disposable) | qcow2 snapshot before installs; F8 VGA Mode; Last Known Good |
| 3 | **`-vga std` switch regresses the rig** (3dfx passthrough, EmuVR behavior) | Low | Medium | Host-side audit found no VGA-type coupling; verify pass with existing kit v12 flow before driver work starts |
| 4 | **Mode-set gaps** (odd pitches, 24bpp, DDraw-HEL-only games slower) | Medium | Low | Advertise the conservative mode set first (16/32bpp, standard resolutions); framebuf/bochs both field-proven under ReactOS |
| 5 | **DDK build friction** on Win11 (path/env quirks in 2003-era `build.exe`) | Medium | Low | WDK 7.1 known-good on modern hosts; worst case build in a throwaway VM |
| 6 | **MS videoprt behavioral edge cases** vs ReactOS's reimplementation (undocumented IOCTL ordering) | Low | Medium | Stick to the documented VRP set VBox/bochs/framebuf already exercise on real Windows |
| 7 | **Cirrus-VBE assumption** — only if the std-vga switch is vetoed | Unverified | High for that path | Avoided entirely by the §2 recommendation |

**Effort estimate:** ~3-5 calendar weeks part-time, consistent with the README's
"weeks" (`README.md:82`):

- Wk 1: toolchain up; framebuf+bochs forked, renamed, building; boots to desktop on
  `-vga std` at one hardcoded mode.
- Wk 2: full mode table, 8/16/32bpp, `DrvAssertMode`/mode-switch stable, INF install
  loop smooth.
- Wk 3: `DrvEscape` + registry pairing; MS `opengl32.dll` observed loading `qxpicd.dll`
  (a Mode-B stub from the T-battery is sufficient — Step 4's real ICD not required).
- Wk 4-5: T17 + regression against EmuVR flow; buffer for driver/toolchain surprises.

**LOC:** ~3,000-3,500 total; ~150-250 genuinely new (escape + registration).

### What Phase 1's interim opengl32-swap results would change

Render through the interim install before implementing the display driver.
The swap results gate this plan three ways:

1. **If the interim install renders GLQuake accelerated**, Phase 2's residual scope is
   *only* the loader handshake — the ICD/fifo plumbing is proven, and any post-driver
   failure is immediately attributable to discovery, not GL. That's the ideal
   sequencing and the reason to keep Step 5 parked.
2. **If Phase 1 stalls** (wrapper can't be re-expressed as an ICD, host refusal
   semantics fail the T-battery), the escape answer is pointless for now — but a
   reduced Phase 2 (mode-set/LFB only) may *still* be worth doing to get std-vga
   higher resolutions and to de-risk the driver half independently.
3. **If the swap proves "good enough" in EmuVR practice**, Phase 2's priority drops
   but its justification sharpens: the swap **deletes the MS software renderer**
   (T10, `icd-fallback-analysis.md:291-295`), so the exclusive-owner design's "second
   client silently gets software GL" degrades to "second client gets Mesa softpipe" —
   acceptable perf-wise perhaps, but it also means SFC/WFP fights and a nonstandard
   system file forever. The driver restores the stock loader + genuine GDI-Generic
   fallback, which is the design the whole README rests on (`README.md:26-42`).
   Notably VirtualBox itself never built the XP-genuine path (§4) — shipping the swap
   permanently has vendor precedent, so this is a real decision point, not a foregone
   conclusion.

---

## 7. Verdict

**Feasible, and cheaper than the README's framing implies — with the crib plan
corrected.** The mode-set + linear-framebuffer body of the driver is an assembly job
from live, GPL, purpose-matched skeletons (ReactOS `framebuf` + `bochs`, 2,764 LOC
combined, already speaking std-vga's exact DISPI registers), cross-checked against a
shipping XP-era production pair (VBox XPDM) and Microsoft's own DDK sample — no
research risk remains there. The single piece of genuinely new code, the
`DrvEscape(QUERYESCSUPPORT/OPENGL_GETINFO)` responder plus its registry pairing, is
~150-250 LOC against a contract that is small, precisely documented from the loader
side (`icdload.c:142-157,15-20,36`), and precedented on 9x (vmdisp9x's `{2,1,"QEMUFX"}`
answer) — but it has **no worked NT-side open-source example anywhere in our refs**,
which is the finding that reshapes the plan: the VBox pair the README ranks first as
a crib contains zero escape code and routes around XP ICD discovery entirely, so its
value is reference-only. Prerequisites before code: flip the rig to `-vga std` (one
token in three cmd_line files, no expected passthrough coupling), add `-serial
pipe:qxpdbg` for WinDbg, snapshot the qcow2. Keep Step 5 parked until Step 4's interim
opengl32-swap milestone renders, then execute this plan in ~3-5 part-time weeks; the
remaining escape-routing risk is detectable in the first hour of on-rig testing with
a logging DrvEscape and has the specified T17 probes.

---

## 8. Verification (2026-07-10)

Nine load-bearing claims (C1-C9) were independently re-verified against the cited
sources by a separate adversarial pass. Result: **8 confirmed, 1 refuted and
corrected**; the verdict, crib base, and adapter recommendation all survive.

- **Confirmed as written:** the escape-is-greenfield negative (C1: zero grep hits in
  both crib trees); the loader contract, constants, and struct (C2:
  `icdload.c:15-20,142-157`, `QUERYESCSUPPORT=8`, `OPENGL_GETINFO=0x1101`); the
  `OpenGLDrivers` registry cross-check and silent-mismatch rejection (C3:
  `icdload.c:36,186-241`); vmdisp9x as the only worked escape answerer in refs/, 9x
  ANSI-struct variant (C4: `control.c:84-119,185`); rig ground truth — `-vga cirrus`
  ×3, QMP on 44553 (3dfx/NFSU2), no `-serial` (C5); framebuf=1,929 / bochs=835 LOC,
  GPL, in the ReactOS build, DISPI at 0x1CE/0x1CF (C7); VBoxVideo.inf as INF template
  (C8: `:47,83,85`); VBox's shipping GL = WDDM-only opengl32 replacement via
  `D3DKMTQueryAdapterInfo`, no XPDM escape ever (C9).
- **Refuted → fixed (C6):** the earlier draft's "cirrus VRAM machine-capped at
  8 MiB". `machine.c:240` is a `hw_compat_2_8[]` compat entry that only applies to
  pc-≤2.8 machine types; the rig runs the default `pc-i440fx-9.1`, so cirrus is
  actually **4 MiB** (device default + hardcoded `real_vram_size`). §2 corrected.
  Dependent conclusions unchanged — the correction *strengthens* the `-vga std`
  recommendation (4 MiB rules out 32bpp above ~1152×864).
- **Evidence-level corrections (claims stand):** VBox's XPDM `DrvEscape` has *three*
  cases, not two (adds `VBOXESC_ISANYX` at `:951`, not OpenGL-related) — §4 wording
  updated; C7's "~11,670 LOC" VBox figure is approximate — an independent measure of
  the three xpdm dirs gives ≥9,745, still ~4× framebuf+bochs, conclusion intact.
