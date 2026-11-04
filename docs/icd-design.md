# qxpicd.dll — Step 4 guest ICD, architecture and bring-up (2026-07-10, v0.3)

**What this is:** the module/entry-point/attach/install/build design for the Phase-1
guest OpenGL ICD. The first attach/arbitration slice now lives in `guest/qxpicd`:
all 20 loader exports, MAPMEM mapping, v1 probe/reservation, synthesized PFD,
fixed-signature activation, tokened bind/release, and a 336-entry table. The first
FIFO-backed rendering slice is also implemented: real context create/make-current,
host strings, swap, state/query calls, textures, immediate geometry, and separate,
indexed, and interleaved client arrays. All 336 OpenGL 1.1 table slots now have
non-no-op dispatch bodies. The static audit reports 336/336; the real-XP suite
exercises representative generated calls and every custom pointer/bulk protocol,
including PixelMap and polygon-stipple round trips.
The concise implemented wire contract is `docs/mesa-qxpicd-protocol.md`.
Inputs: `docs/guest-wrapper-notes.md`,
`docs/icd-fallback-analysis.md`, `docs/refusal-semantics.md` (**present, v0.3, NOT
FROZEN** — R26 open items O1–O6 gate the freeze; this design binds to clauses
R10–R25 and flags every place a floating clause is load-bearing),
`docs/toolchain-notes.md`, plus mapper slices over `refs/mesa9x`, `refs/softgpu`,
`refs/qemu-3dfx`, `refs/vmdisp9x`, and the qemu-libretro host tree
(`<core checkout>` @ `73a050b026`+wt). Wrapper citations are
`refs/qemu-3dfx/wrappers/mesa/src/wrapgl32.c` @ `b47f30a396` unless pathed.
Key facts spot-checked in code 2026-07-10 (activation handshake, host memcmp,
mesa9x refusal gate, proc-table size, loader export requirements, MAPMEM service,
0xFE4 window-init path, softgpu CopyFileA).

QXPICD now starts a one-second token heartbeat after a successful bind. Active
transport calls also renew the lease at most twice per second, preventing the
heartbeat worker from being starved by a game continuously reacquiring the
transport lock. Command completion on the host renews an already-live lease as
well, because cold
texture upload or host-driver initialization can occupy one synchronous MMIO
dispatch longer than the five-second TTL. Register `0xFB0` renews a five-second
host lease only when the low-16-bit token matches the
current tokened owner. The host clock is virtual, so VM pause cannot expire a live
guest. Lease expiry performs host-driven context/window/timer cleanup and eagerly
resets the FIFO claim and ring refcount before returning the session to FREE.
Real-XP kill/reacquire and 12-second idle-owner tests both pass.

---

## 1. Module layout

`qxpicd.dll` = the wrapper's transport + GL body with a new ~1.7k-line `Drv*` front
replacing the ~1k-line WGL/DllMain front (`wrapgl32.c:16632-17601`).

### Implemented bring-up slice

`guest/qxpicd` cross-builds cleanly with MinGW as a 32-bit PE DLL, XP 5.1 GUI
subsystem, exactly 20 undecorated exports, and only `KERNEL32.dll`/`msvcrt.dll`
imports. The matching host now accepts `"QXPICD\0" + ABI 1` and initializes the
ring claim/cursors/refcount only after a reservation token wins. A boot fixture
using the fixed signature and no guest-written claim passed `LCSOBFAR` and
`TEARDOWN_OK`.

The same DLL then passed twice consecutively on real XP SP3 under WHPX with
`-smp 2`: validate, describe/reserve, fixed-signature activation, Choose-less
tokened bind, local context/proc-table return, delete/release, UDP `rc=0`, and
host `TEARDOWN_OK`. An intervening run exposed XP thread migration between an
MMIO write and its readback; QXPICD now temporarily pins only that pair to one
available vCPU and immediately restores the prior thread affinity.

The next vertical slice added real `0xFFC` context creation, `0xFF8` make-current,
FIFO batching, synchronous `glGetString`, and `0xFF0` swap. On real XP the ICD
reported `NVIDIA GeForce RTX 4090/PCIe/SSE2`, sustained 32.0 FPS, and produced a
captured interpolated RGB triangle (SHA-256
`A711B7357BD41AC9CC5435D118C3FE899FE8BAD3C7B39B3B85E2ACF5D272EA71`) before
clean level-0 release and DLL unload.

### Carried over verbatim (≈16.5k lines, mostly generated)

| Piece | Source | Notes |
|---|---|---|
| fifo client core | `wrapgl32.c:58-119` (`InitMesaPTMMBase`/`fifoAddData`/`FIFO_FLUSH`), submit macros `:540-544` | one added branch: inert gate (§3.4) |
| GL stub army (~2900 `gl*`) | `wrapgl32.c:549-16630` | become internal (not exported); reached only via the proc table and `DrvGetProcAddress` |
| `fifoOutData` readback + `fbtm` staging | `:105-114`, `:1152-1200`, `:4301-4307` | stale-readback sites get the inert gate (§3.4) |
| WGL_FUNCP marshalling (`0xFDC` named funcs) | `:16632-16640`, ARB create `:16749-16779` | ARB-create is the honest-ack template for `DrvCreateContext` (§2) |
| MAPMEM plumbing | `fxlib/fxlibnt.c:19-92` (open `\\.\MAPMEM`, map/unmap IOCTLs) | NT path only — drop `fxlib9x.c`/VXD selection (`wrapgl32.c:17528-17533`); XP-only DLL |
| pbuffer plumbing | `:16925-16940`, `currPB[16]` | as-is; feeds Phase-3 WineD3D question |

### New (estimated ≈1.6–1.9k LOC)

| Piece | est. LOC | Notes |
|---|---|---|
| `Drv*` bodies (20 exports) | ~450 | §2 table |
| Attach/probe state machine | ~200 | `0xFB4` probe client, soft-reserve token, lazy mapping, per-state transitions (§3) |
| Static `GLCLTPROCTABLE` (336 entries) | ~350 | field order = MS `gldrv.h`; entries point at the existing `gl*` stubs; pattern: one `static const` table returned by `DrvSetContext` (mesa9x `stw_context.c:944-953`, `stw_icd.h:46` `OPENGL_VERSION_110_ENTRIES`=336) |
| DHGLRC bookkeeping | ~100 | small handle table, integer handles, 0 = invalid (mesa9x convention, `stw_icd.h:44`; internally may keep the `MESAGL_MAGIC - level` scheme, `wrapgl32.c:16749`) |
| Inert-mode gating + cached-string store | ~120 | §3.4 |
| PFD synthesis | ~80 | one 32-bit RGBA double-buffered format, guest-side, host not consulted (R23) |
| `qxpicd.def` (hand-written) | ~21 | mesa3d.def shape (`refs/mesa9x/mesa3d.def:1-21`); no gendef (`docs/toolchain-notes.md:41-44`) |
| Makefile | ~70 | borrow flags from `wrappers/mesa/src/Makefile.in` (§6) |

### Deleted

- **DllMain app-fatal handshake** (`wrapgl32.c:17540-17573`): the map-ranges /
  signature-write / `0xFBC=0xA0320` poke / readback-or-die sequence leaves DllMain
  entirely (`docs/guest-wrapper-notes.md` §2.1: app-fatal load failure is
  unacceptable system-wide). New DllMain: zero statics, `DisableThreadLibraryCalls`,
  return TRUE always.
- **Guest-side arbitration**: the ring-collision sniff (`:72-73`) and the
  refcount/share path (`:17542-17547`) — R21: the ICD never ring-sniff-shares and
  never claims `mfifo[1]` itself (the claim word is written host-side as part of the
  R12 grant).
- **`wglSetPixelFormat` stack-walk + IAT hook patchers** (`:17448-17456`,
  `HookDeviceGammaRamp :17046-17073`, fxhook/fxtime/fxhpat): per-game shims; also
  removes the `COMPACT_FRAME` no-sse2 codegen constraint
  (`docs/toolchain-notes.md:56-57`). Opt-in later at most.
- **gendef/sed export machinery + 2977-export surface** (`Makefile.in:29-40`):
  qxpicd exports ~20 symbols; apps link MS `opengl32.dll`, which dispatches through
  the proc table.
- **`0xFB8` spin** (`:17405-17406`): the ICD never enters an unbounded poll; any
  wait is bounded and sentinel-aware (R3 `MESA_BUSY`), and the default flow avoids
  `0xFEC` altogether (§3.2, risk #4).

---

## 2. Entry-point map

Export set = `mesa3d.def:1-21` exactly: ReactOS's 16 mandatory
(`refs/reactos/dll/opengl/opengl32/icdload.c:311-326` — any missing ⇒ ICD load
fails ⇒ silent fallback) + `DrvValidateVersion`/`DrvSetCallbackProcs`
(`icdload.c:281-300`, optional) + `DrvPresentBuffers` + `DllCanUnloadNow`.

| Export | Strategy | Source / notes |
|---|---|---|
| `DrvValidateVersion(ULONG)` | **new** — device-present/ABI gate only, never the busy gate (R19; O2/T3 pending) | §3.1. mesa9x's rubber-stamp (`stw_device.c:261-274`) shows a lazy-init pattern is loader-legal; we use it as the permanent-failure gate instead |
| `DrvDescribePixelFormat` | **new** — probe + synthesize; THE refusal point | §3.2. Shape from mesa9x `stw_pixelformat.c:348-376`: `ppfd==NULL` ⇒ count; strict `cjpfd` check; success returns total count. Refusal = return 0 (`:357-358` pattern). Do NOT reuse the wrapper's `wglDescribePixelFormat` (`:17414-17428` returns 1/0, not count, and pokes the host) |
| `DrvSetPixelFormat(HDC,int)` | wrap `wglSetPixelFormat` `:17448-17473` minus stack-walk/hooks | adds activation-if-needed + token in scratch word 12, binds ownership on grant (R12, R23); FALSE on refusal — the recoverable backstop (`icd-fallback-analysis.md` §2 row 3, T5) |
| `DrvCreateContext(HDC)` | wrap `mglCreateContext` `:17191-17212` **made honest** | replace fire-and-forget `0xFFC` with the `0xFDC` ARB-create path, which returns a real status the guest checks (`:16749-16779`; host `mesapt_mm.c:2418-2432`); return DHGLRC from the handle table, 0 on host refusal — but only reachable pre-grant (§3.3 inert rule) |
| `DrvCreateLayerContext(HDC,int)` | plane==0 → `DrvCreateContext`, else 0 | mesa9x forwards the other way (`stw_context.c:119-122`); either shape fine |
| `DrvSetContext(HDC,DHGLRC,PFN_SETPROCTABLE)` | **new** + wrap `mglMakeCurrent` `:17214-17246` | return `&cpt` (static 336-entry table) on success, NULL on failure (`stw_context.c:944-953`); ignore `pfnSetProcTable` (mesa9x never uses it; wgltest passes NULL, `wgltest.c:299-307`); when inert: still succeed and return the table — stubs are self-gating (§3.4) |
| `DrvReleaseContext(DHGLRC)` | **new**, trivial | flush + make-current(0,0) analog (`docs/guest-wrapper-notes.md` §4 row); return TRUE always (loader ignores it, ReactOS `wgl.c:689-697`) |
| `DrvDeleteContext(DHGLRC)` | wrap `mglDeleteContext` `:17248-17265` | frees handle-table slot |
| `DrvCopyContext` | **intentional stub** → FALSE | host warns unhandled today; wrapper fabricates TRUE (`:17279-17284`) — be honest instead |
| `DrvShareLists` | wrap `mglShareLists` `:17293-17302` | real, via `0xFDC` |
| `DrvGetProcAddress` | wrap `mglGetProcAddress` `:17114-17186` | WGL-ext names answered locally (`:17122-17170`) — MUST NOT be treated as grant (`guest-wrapper-notes.md` §2.9); GL-ext query (`0xFE0`) only when live; when inert/refused: NULL |
| `DrvSwapBuffers(HDC)` | wrap `wglSwapBuffers` `:17336-17386` | FALSE when inert (R20/T8); keep FPS-pace readback when live |
| `DrvSwapLayerBuffers` | plane 0 → `DrvSwapBuffers`, else FALSE | wrapper stub `:17390` |
| `DrvDescribeLayerPlane` | **intentional stub** FALSE | wrapper `:17083-17089` |
| `DrvGetLayerPaletteEntries` | **intentional stub** 0 | `:17091-17097` |
| `DrvRealizeLayerPalette` | **intentional stub** FALSE | `:17099-17104` |
| `DrvSetLayerPaletteEntries` | **intentional stub** 0 | `:17106-17112` |
| `DrvSetCallbackProcs(INT,PROC*)` | **new**, trivial | bounded positional memcpy of ≤5 PROCs (mesa9x `stw_device.c:246-258`); XP loader passes 3 (`icdload.c:292-300`); we never invoke any of them (present-buffers callback is Vista+ composition — mesa9x slice §4) |
| `DrvPresentBuffers` | **intentional stub** FALSE | exported for mesa3d.def parity; never called on XP |
| `DllCanUnloadNow` | `return S_FALSE;` | COM leftover, not part of the WGL contract (`libgl_gdi_icd.c:4-7`); kept for parity, droppable |

Per-DHGLRC bookkeeping is new relative to the wrapper (it tracks one implicit
current context, `guest-wrapper-notes.md` §4 tail); a 6-slot table mirroring the
host's `hRC[MAX_LVLCNTX]` stack suffices for v1.

---

## 3. Attach flow re-homed

The wrapper's attach lives in DllMain and dies with the process
(`wrapgl32.c:17540-17573`). qxpicd splits it across two loader-driven entry points
and adds one process-global state machine:

`UNKNOWN → (DrvValidateVersion) → DEVICE_OK | DEAD`
`DEVICE_OK → (first DrvDescribePixelFormat) → ADVERTISING | REFUSED(per-probe)`
`ADVERTISING → (DrvSetPixelFormat grant) → GRANTED`
`GRANTED → (host death detected) → INERT` — one-way; `DEAD` and `INERT` are terminal.

### 3.1 `DrvValidateVersion` — the permanent-condition gate

Called by the loader before the mandatory-export resolution (`icdload.c:281-289`);
FALSE ⇒ `FreeLibrary` + silent software fallback. qxpicd does here, lazily, exactly
the *fatal-if-absent* checks:
1. Open `\\.\MAPMEM` (`fxlibnt.c:19-29`). Fail → **FALSE** (device driver not
   installed — permanent for this boot).
2. Map the four ranges (`InitMesaPTMMBase`, `wrapgl32.c:58-79`, minus the ring
   claim/reset — R21: no `mfifo[1]`/cursor writes here). Fail → **FALSE**.
3. Read the `0xFB4` probe (R10). No `0x3D` tag (old host or dead device) →
   **FALSE**. Tag present but `version` unsupported → **FALSE** (ABI mismatch is
   permanent per-boot).
4. Tag + version OK → **TRUE**, regardless of busy state. Busy is never signalled
   here (R19: real XP may cache a ValidateVersion FALSE per-process — open item
   O2/T3 — which would deny a client forever instead of per-window).

All four failure modes are permanent conditions, so even if XP caches the FALSE
per-process (O2) nothing is lost. mesa9x proves the loader tolerates real init
before/outside this call (`libgl_gdi.c:206-226` inits in DllMain,
`stw_device.c:261-274` rubber-stamps) — we keep init *out* of DllMain anyway
because failure there is app-fatal (`guest-wrapper-notes.md` §2.1).

### 3.2 First `DrvDescribePixelFormat` — the busy gate

The loader calls `DrvDescribePixelFormat(hdc,0,0,NULL)` once per window at first
GL touch and caches the count (ReactOS `wgl.c:76-82`). qxpicd, per call:

- **Probe** `0xFB4` (read-only, side-effect-free, R10).
- **FREE, or RESERVED/OWNED with my token** → write token T (soft-reserve /
  re-arm, 2000 ms lazy TTL, R11; T = PID&0xFFFF, 0→1) and return **1** — one
  synthesized 32-bit RGBA double-buffered format
  (`PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER`, **no**
  `PFD_GENERIC_FORMAT`), built guest-side. The host is not poked: enumeration
  allocates nothing host-side and cannot seize the owner's window — this kills the
  wrapper-era hazard where describing formats runs
  `MGLDescribePixelFormat → GLWINDOW_INIT()` (`mglcntx_mingw.c:450-454`, macro def
  `:269-274`; `guest-wrapper-notes.md` §4 Describe row).
- **Host busy** (OWNED/RESERVED by another) → return **0**. Loader: zero ICD
  formats ⇒ all indices generic ⇒ app silently gets MS software GL
  (`icd-fallback-analysis.md` §2 row 1; the mesa9x-shipping pattern,
  `stw_pixelformat.c:357-358`). No error, no host state touched (R19).
- **Host absent / no tag** (shouldn't normally reach here past 3.1, but re-checked
  cheaply) → **0** + latch `DEAD` for the process (R24) — inert discipline, never
  the legacy attach.
- **Version mismatch** → same as absent: 0 + latch (belt to 3.1's suspenders).

Format-index semantics per mesa9x: 1-based index, return value always the total
count, `ppfd==NULL` = count probe, wrong `cjpfd` = 0 (`stw_pixelformat.c:348-376`).

### 3.3 `DrvSetPixelFormat` — activation + binding

R23 flow: if the process hasn't activated, poke `0xFBC=0xA0320` now (allowed while
RESERVED — R13: activation is a refcounted device-open, not binding) with the
signature per §4; then `0xFE4` with `{format, ptm, PFD}` + token in scratch word 12
(R12). Readback `MESAGL_MAGIC` → **GRANTED** (ownership bound host-side); anything
else → **FALSE** to the app — app-visible but non-poisoning and recoverable
(`icd-fallback-analysis.md` §2 row 3 / T5); this is the accepted outcome of the
describe→set race (R11 rationale).

Note the host creates its render window inside the `0xFE4` handler:
`mesapt_mm.c:2397` → `MGLSetPixelFormat` (`mglcntx_mingw.c:418`) →
`GLWINDOW_INIT()` macro (`:422`, def `:269-274`; there is no `MGLWindowInit`
symbol). Since qxpicd skips `0xFEC` (and thus the
`0xFB8` spin that today synchronizes window readiness, `wrapgl32.c:17405-17406`),
window-ready synchronization must be resolved inside the host's `0xFE4` handling or
by a bounded ICD retry — flagged as risk #4 / test T15.

After a granted SetPixelFormat, `DrvCreateContext`/`DrvSetContext` proceed on the
fifo client with honest acks (§2). Failures *before* grant return honestly (loader
handles them per the T5/T6 matrix — only reachable by index-hardcoding apps);
failures *after* grant convert to inert, never to error (next).

### 3.4 Mid-session death → INERT (never fail after formats are handed out)

Once a window carries the ICD's format, no software context is ever reachable on it
(set-once, ReactOS `wgl.c:836-840`; `icd-fallback-analysis.md` §3: "Mid-session
refusal … can never fall back"). So on host death after grant — activation readback
lost, probe tag gone, MMIO reads 0/garbage — qxpicd latches `INERT` (R20):

- `DrvCreateContext`/`DrvSetContext` keep **succeeding** (local handle; the static
  proc table) — a failing DrvSetContext strands the thread contextless
  (`wgl.c:700-711`), a failing create poisons harder; inert success is the safe UX
  (T8: black-but-alive).
- All `gl*` submission stops at the source: one `live` check added inside
  `FIFO_GLFUNC`/sync-submit macros (`wrapgl32.c:540-544`) — compiled into every
  generated stub by the existing macro expansion; cursors never advance
  (`guest-wrapper-notes.md` §2.6 walk-off hazard; R9 host clamp is the second belt).
- Queries return zeros; `glGetString` returns strings cached at grant time; the two
  live-context stale-readback hazards are explicitly closed: `glMapBuffer` must
  return NULL instead of a pointer built from stale `FRet` (`:6961-6962`), and
  `glGetCompressedTexImage` must not memcpy stale `fbtm` (`:4301-4307`).
- `DrvSwapBuffers` → FALSE; DLL never unloads itself, never raises.
- Detection points (cheap, no new traffic): swap readback, sync-call readback
  anomaly, periodic probe re-read on swap.

---

## 4. Handshake / versioning

**Today** (verified): guest writes the 8-byte git stamp `rev_` to the top of `fbtm`
(`wrapgl32.c:17567`; produced by `stamp.o`'s `git rev-parse --short HEAD`,
`Makefile.in:48-51`); host memcmps against its own hand-vendored literal
`rev_[] = "b47f30a-"` (`hw/mesa/mglfuncs.h:49-50`, declared via `COMMIT_SIGN` at
`mesapt_mm.c:2215`) at `mesapt_mm.c:2222`, with `MesaVer` zeroed first (`:2221`,
fail-closed) and the readback (`:381-383`) reconstructing `0x320a0` purely by
bit-shuffling the write value (`:2224`) — the "version" is decorative; the entire
gate is one memcmp (host-handshake slice §4). The literal is duplicated untied in
`hw/3dfx/g2xfuncs.h:257-258` and `system/vl.c:885` (only the two device headers are
diffed at publish, `libretro-gitlab-build.sh:193-204` — `vl.c` can drift).

**Recommendation: versioned ABI (R25), staged.**
- **Stage 0 (bring-up only): keep-coupled.** qxpicd writes the same git `rev_` as
  the host build (a `-DQXP_SIGN=...` build flag replacing the `stamp.o` rule).
  Cost: every host rebuild orphans the installed ICD → guest reinstall per commit —
  untenable for a *system-wide* driver (the whole point is nothing per-game and
  servicing independence), acceptable for the first weeks while Step 2a lands.
  Benefit: zero host changes required; works against today's `mesapt_mm.c:2222`.
- **Stage 1 (ship): R25 versioned signature.** Host accepts EITHER exact `rev_`
  (legacy wrapper, kit v12 untouched — 2d-acc) OR `"QXPICD\0"+abi` where the abi
  byte must equal the version field the host advertises in the `0xFB4` probe tag
  (R10/R25). Host cost: the memcmp at `mesapt_mm.c:2218-2229` grows one alternative
  compare + the probe register — localized, no transport change (host-handshake
  slice §4 option 1/2). Guest cost: fixed string, no git dependency in the build.
- **Explicit caveat:** versioning the attach does NOT decouple the FIFO call ABI —
  the `FEnum_*` ordinals and marshalling tables (`mglfunci.h`/`szgldata.c`,
  `guest-wrapper-notes.md` §0) remain a separate compatibility surface. Rule: the
  abi byte covers the *whole* guest↔host ABI; any ordinal/layout/register change
  bumps it, and a host advertising version N accepts only `abi == N`.

Rejected: full capability negotiation (host-handshake slice §4 option 3) — real
protocol-design work with no current consumer; revisit if/when a second ABI
consumer (D3D DDI) appears.

---

## 5. Install story

### Interim (before the Phase-2 XPDM driver)

Two distinct problems; keep them separate:

1. **Driving the `Drv*` surface without the MS loader.** QEMU std-vga's driver
   never answers `QUERYESCSUPPORT/OPENGL_GETINFO`, so the real loader can't be
   pointed at qxpicd on the R1 rig (`icd-fallback-analysis.md` §1 row 1, §5 R1;
   registry-only registration is Win7+, `llvmpipe.rst:139-157` as cited there).
   Cheapest harness: **mesa9x `wgltest.c` "driver" mode** — `LoadLibraryA` the ICD
   and resolve `Drv*` by name, bypassing opengl32 entirely
   (`refs/mesa9x/wgltest_api.h:35-41`, `wgltest.c:192-229`). Gap to close: it never
   calls through the returned `GLCLTPROCTABLE` (passes NULL `pfnSetProcTable`, only
   null-checks the return, `wgltest.c:299-307`) — extend it to render via the table
   (test T13, §7).
2. **Whole-system smoke: the opengl32 swap.** Replace `system32\opengl32.dll` (and
   its `dllcache` mirror) with a full-replacement build — the `opengl32.mingw.def`
   shape (18 `Drv*` + ~250 bare `gl*/wgl*`, `refs/mesa9x/opengl32.mingw.def:1-390`),
   i.e. qxpicd's internals behind a wgl front, or simply today's wrapper renamed.
   **Mechanics are an open gap:** the "VirtualBox old installer SFC exception"
   claim (README.md:78-79) has no *bypass* mechanics sourced anywhere in `refs/` —
   grep for `dllcache|SfcFileException|SFC_OS` across `refs/` returns zero hits;
   the only SFC surface at all is a Wine protection-*query* header
   (`refs/virtualbox/.../shaderlib/wine/include/sfc.h:46-48`,
   `SfcIsFileProtected` et al. — declarations only, no exception/install code);
   softgpu's system32 copy is a plain `CopyFileA` that only works because 9x has
   no WFP (`refs/softgpu/actions.c:1359-1391`, `filecopy.c:99-163`). Plan: manual swap on
   the disposable image (copy over both `system32\` and `system32\dllcache\`
   copies, dismiss the WFP dialog), scripted `SfcFileException` later from external
   sources (VBox Additions installer) — test T14.
   **Known limits:** the swap removes the MS loader, so it can exercise Phase-1
   plumbing but **not** the software fallback (`icd-fallback-analysis.md` §5 R1/T10
   caveat), and it leaves no MS generic renderer at all (T10). Loader-vs-qxpicd
   refusal behavior is only testable on the R2 rig (VBox/VMware guest, hijacked
   `OpenGLDrivers` registration — `icd-fallback-analysis.md` §5 R2) where qxpicd
   runs deviceless, i.e. exactly its DEAD/inert paths.

Note `qxpicd.dll` itself is a *new* file in system32 — not WFP-protected; only the
opengl32 swap fights WFP. The registry write is one key.

### Final (Phase 2)

- XPDM display driver answers `QUERYESCSUPPORT(0x1101)` + `OPENGL_GETINFO`
  (escape shape: `refs/mesa9x/icdtest.c:14-57` from the caller side;
  `refs/vmdisp9x/control.c:151-245` from the driver side, 9x edition), returning
  `{ulVersion, ulDriverVersion, DriverName}`.
- Registry: `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\<Name>`
  with `Dll=qxpicd.dll`, `Version=2`, `DriverVersion=1`, and `Flags=1`; the first
  two numeric values **equal the escape's values**
  (loader cross-checks and silently rejects on mismatch, `icdload.c:222-242`;
  path per `icdload.c:36` — the NT form, not the 9x flat value
  `vmdisp9x/control.c:161-165`). The loader-side tuple is described in
  `docs/icd-fallback-analysis.md`; actual XPDM routing remains T17.
- opengl32.dll goes back to stock; fallback machinery restored.

### MAPMEM at-boot guarantee

Historical: the ICD now maps through XPDriver's `qxpmap.sys` (system-start
service, shipped in the signed package and by the QD3D installer;
`docs/design/qxpmap.md`). MAPMEM, `fxptl.sys` and `INSTDRV.EXE` below are no
longer XPDriver dependencies.

The transport hard-depends on `\\.\MAPMEM` (`fxlibnt.c:19-29`;
`guest-wrapper-notes.md` §0). The existing kit already solves boot persistence:
`INSTDRV.EXE` registers `fxptl.sys` as a `SERVICE_AUTO_START` kernel-driver service
via `CreateService` (`refs/qemu-3dfx/wrappers/3dfx/drv/instdrv.c:58-65`) +
`StartService` (`:76-95`) — one elevated run, then the device exists at every boot
before login. Re-running is idempotent (`main()` removes/reinstalls/self-tests,
`instdrv.c:97-138`). Two caveats: (a) `fxptl.sys` is a vendored opaque blob — built
from a checked-in `xxd` dump (`wrappers/3dfx/drv/Makefile:19-21`, sha-pinned
`shasum.txt`), no source in the repo — a supply-chain/audit flag; (b) qxpicd must
treat MAPMEM-absent as `DrvValidateVersion` FALSE (§3.1), never an app-fatal
DllMain like the wrapper (`wrapgl32.c:17552-17555`). The Phase-2 miniport should
eventually absorb the mapping duty (`guest-wrapper-notes.md` build notes),
retiring the blob.

---

## 6. Build plan

Per `docs/toolchain-notes.md` (verified by test build):

- **WSL cross:** `i686-w64-mingw32-gcc` 13-win32 + `-static-libgcc`; imports stay
  KERNEL32/msvcrt/USER32; subsystem 4.0 by default — add
  `-Wl,--subsystem,windows:5.01` for explicitness (`toolchain-notes.md:19-26`).
- **ISA floor:** drop upstream's `-march=x86-64-v2` (SSE4.2-era floor,
  `Makefile.in` CFLAGS) for `-march=pentium4` or lower (`toolchain-notes.md:52-57`);
  the stack-walk (`COMPACT_FRAME`) is deleted so its codegen constraints go too.
- **Exports:** hand-written `qxpicd.def` (~21 lines, §2 set) — no gendef/sed
  machinery (`toolchain-notes.md:41-44`).
- **Headers:** compile against **qemu-libretro's** `hw/mesa` (`mglfuncs.h`,
  `mglfunci.h`, …) so guest and host agree on the ABI the core actually implements
  (`toolchain-notes.md:36-39`) — not qemu-3dfx's `qemu-1/hw/mesa`.
- **Signature:** replace the `stamp.o` git-rev rule (`Makefile.in:48-51`;
  `toolchain-notes.md:58-59`) with the staged scheme of §4.
- **Housekeeping:** CRLF/bash caveats for any reused scripts
  (`toolchain-notes.md:49-51`); optional VERSIONINFO via
  `i686-w64-mingw32-windres` (`:75-76`). License: derived work is **GPLv2**
  (`toolchain-notes.md:61-69`) — ship sources with the guest pack.

---

## 7. Risks & unknowns, ranked

| # | Risk | Settled by |
|---|---|---|
| 1 | **Window poisoning / refusal-point model** | **Settled on stock XP:** T3–T6 passed; zero formats silently fall back, failed SetPixelFormat is recoverable, post-commit context failure poisons the window. |
| 2 | **Host hardening** — probe, reservation/ownership, main-thread GL dispatch, SMP reply latches, fixed ICD signature, host-created ICD ring claim, and token heartbeat/reclaim are implemented and pass synthetic + real-XP acceptance. | Legacy-client reclaim and EmuVR regression remain; QXPICD kill-owner/reclaim + idle-owner guard pass. |
| 3 | **SMP acknowledgement migration** — per-vCPU latches prevent cross-vCPU overwrites, but XP can migrate one guest thread between its MMIO write and read instructions. This occurred once on real `-smp 2` XP after the host granted `0xFE4`. | **Mitigated and twice re-tested:** QXPICD pins only each acknowledgement pair to the lowest allowed vCPU, restores affinity immediately, and passed two consecutive WHPX runs. Every later sync-readback port must use the same helper. |
| 4 | **Window-ready sync at tokened `0xFE4`** | **Settled by T15:** both the no-Choose boot fixture and real QXPICD XP runs passed. Main-loop dispatch completes `MGLWindowInit`/`SetPixelFormat` synchronously inside the trap; no `0xFB8` spin or retry is needed. |
| 5 | **Proc-table order correctness** — 336 entries are opengl32's compiled-in offset table; one swapped pair = silent wrong-function dispatch | **new T13**: extend wgltest driver mode to render through the returned `GLCLTPROCTABLE` (gap noted in mesa9x slice §7); build table from a verbatim MS-`gldrv.h`-derived header (mesa-21.3.x `gldrv.h:373-388`) |
| 6 | **`DrvValidateVersion` FALSE caching per-process** | **Settled by T3:** stock XP retries; no negative process cache observed. Busy still must not use this gate. |
| 7 | **Describe-count granularity** | **Settled by T4/T11:** a new window in the same process re-queries and can become ICD-backed. |
| 8 | **SetPixelFormat-FALSE recoverability** | **Settled by T5:** generic format succeeds on the same window after ICD SetPixelFormat returns FALSE. |
| 9 | **Interim install mechanics unsourced** — no SFC/WFP *bypass* mechanics anywhere in `refs/` (zero grep hits for `dllcache\|SfcFileException\|SFC_OS`; only SFC surface is Wine's query-API header, `refs/virtualbox/.../wine/include/sfc.h:46-48`; softgpu is 9x `CopyFileA`, `actions.c:1359-1391`); dllcache/WFP-dialog behavior on the real image unproven | **new T14**: manual swap incl. `dllcache` on the disposable image; then source `SfcFileException` externally (VBox Additions) |
| 10 | **MinGW-built DLL load + MAPMEM map on real XP** — DLL loading/export/callback ABI is proven; MAPMEM remains | Stub battery + **T10** extended to map ranges |
| 11 | **`fxptl.sys` opaque blob** — unauditable vendored binary as a kernel dependency (`drv/Makefile:19-21`) | accepted for Phase 1 (sha-pinned); retired by Phase 2 miniport absorbing the mapping |
| 12 | **refusal-semantics v0.3 not frozen** — R10/R11 register shape (`0xFB4`, token word 12) may shift with O1/O6 | R26 closure; qxpicd's probe client is isolated in one ~200-line module so a register/format change is contained |

New tests defined here: **T13** (proc-table dispatch validation via extended
wgltest driver mode), **T14** (opengl32-swap WFP/dllcache mechanics on real XP),
**T15** (window-ready synchronization for the Choose-less `0xFE4` bind).

---

## 8. Feasibility verdict

Feasible and well-bounded. The ICD is ~1.6–1.9k lines of genuinely new code on top
of ~16.5k lines that carry over untouched (fifo client, generated GL stubs, MAPMEM
plumbing), and every structurally hard sub-problem already has a verified reference
implementation: the export surface and refusal gate come from mesa9x
(`mesa3d.def:1-21`, `stw_pixelformat.c:357-358`), the dispatch table is one static
336-entry const (`stw_icd.h:46`, `stw_context.c:944-953`), the honest
context-create path already exists in the wrapper (`wrapgl32.c:16749-16779`), and
boot-persistent MAPMEM is a solved `SERVICE_AUTO_START` service
(`instdrv.c:58-65`). The former external blockers—host probe/ownership and SMP
main-thread dispatch—are implemented, and the ICD has now rendered through them
on real `-smp 2` XP. Zero-format fallback, pixel-format poisoning behavior,
versioned signature, T15 window readiness, the complete 336-slot OpenGL 1.1 table,
special pointer/bulk paths, tokened owner-death reclaim, idle retention, and MAPMEM
transport are empirically closed. Remaining work is chiefly real-application
compatibility breadth, extensions, true simultaneous contexts, complete inert
death handling, legacy-client reclaim, T14 system installation mechanics, and
Phase 2 registration.

The ordinary-window context contract is now explicit rather than fabricated:
QXPICD tracks one live `DHGLRC`, rejects a second creation and invalid handles,
actually unbinds WGL during `DrvReleaseContext`, supports rebind, and can delete
then reacquire a fresh host session on the same pixel-formatted DC. A real-XP
stage-27 lifecycle probe and the subsequent full render pass both succeed. True
simultaneous contexts still require extending the host's ordinary `hRC[]` path.

---

## 9. Verification (adversarial claim check, 2026-07-10)

Ten load-bearing claims independently re-verified against primary sources.

| Claim | Subject | Verdict | Correction applied |
|---|---|---|---|
| C1 | ReactOS loader: 16 mandatory `Drv*` (`icdload.c:311-326`), optional `DrvValidateVersion` FALSE ⇒ FreeLibrary + silent fallback | confirmed | none |
| C2 | mesa9x refusal = `DrvDescribePixelFormat` 0 via `if (!stw_dev)` (`stw_pixelformat.c:357-358`); count/probe/cjpfd semantics | confirmed | none |
| C3 | `DrvSetContext` returns one static const 336-entry `GLCLTPROCTABLE` or NULL; `pfnSetProcTable` never used | confirmed | none |
| C4 | Wrapper attach handshake app-fatal in DllMain: rev_ stamp → `0xFBC=0xA0320` → readback ≠ `0x320a0` ⇒ FALSE (`wrapgl32.c:17567-17573`) | confirmed | none |
| C5 | Host gate = single 8-byte memcmp vs `rev_[]="b47f30a-"`, fail-closed (`MesaVer=0` first), readback bit-shuffled from write — no real version negotiation | confirmed | none |
| C6 | `wglChoosePixelFormat` unbounded `0xFB8` spin (`:17405-17406`) hangs forever without host window | confirmed | none |
| C7 | `0xFE4` handler window-init + 3 s crashRC warp re-points `fifoptr[1]`, decrements refcount, no ownership check | confirmed (substance) | citations fixed: no `MGLWindowInit` symbol exists; window gate is the `GLWINDOW_INIT()` macro (`mglcntx_mingw.c:422` in `MGLSetPixelFormat :418`, def `:269-274`); `mglcntx_mingw.c:434-439` is the `iattr[]` array, not a gate. §3.2, §3.3, risk #4 corrected. File is `refs/qemu-3dfx/qemu-1/hw/mesa/mesapt_mm.c` (lines match qemu-libretro tree) |
| C8 | MAPMEM boot persistence solved: INSTDRV `CreateService` `SERVICE_AUTO_START` kernel driver "MAPMEM" (`instdrv.c:58-65`) + `StartService`; transport hard-opens `\\.\MAPMEM` (`fxlibnt.c:19-29`) | confirmed | none (nuance: INSTDRV registers but does not copy `fxptl.sys` to `drivers\` — deployment must place it) |
| C9 | Export surface = mesa3d.def exactly: 19 `Drv*` (16 mandatory + Validate/SetCallbackProcs/PresentBuffers) + `DllCanUnloadNow` | confirmed | none |
| C10 | "No SFC/WFP-exception code anywhere in `refs/`" | **refuted** (scope overreach) | grep was only over `refs/softgpu`; whole-`refs/` grep for `Sfc` hits Wine's `sfc.h:46-48` (protection-query declarations) in `refs/virtualbox`. Reworded §5 and risk #9: zero hits for `dllcache\|SfcFileException\|SFC_OS` anywhere in `refs/` — *bypass* mechanics remain unsourced; T14 gap and the design decision stand unchanged |
