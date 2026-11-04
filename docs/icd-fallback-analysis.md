# ICD refusal → software fallback: source analysis + XP test plan (2026-07-10)

**Question (Step 1a, reading half):** when an installed OpenGL ICD refuses, does the
XP-era `opengl32.dll` loader silently fall back to its built-in software renderer — and
at exactly which entry points is refusal "silent fallback" vs. "app-visible error"?

**Sources:**
- `refs/reactos` @ `c296dcfa29e7f32b623d8ed3e70604a06472fda2` (2026-07-10) —
  `dll/opengl/opengl32/{icdload.c,wgl.c,icd.h,opengl32.h,swimpl.c,apistubs.c,glapi_x86.s,dllmain.c}`
- `refs/mesa9x` @ `29b9adb44bc5ea54dc53c02b5e4b49292c6cc04f` (2026-07-06) — Gallium WGL
  frontend (`mesa-17.3.9/src/gallium/state_trackers/wgl`, newer trees under
  `mesa-2x.x/src/gallium/frontends/wgl`), `icdtest.c`, `wgltest.c`, `mesa3d.def`
- Cross-refs: `refs/vmdisp9x` @ `718b3d51a153` (`control.c` — the 9x display-driver side
  of the escape), `refs/softgpu` @ `be7a642106` (9x-only; no XP SFC logic found)

**Bottom line:** the fallback claim is **true but only for refusals made at load/
enumeration time**. Every failure from ICD discovery through "DrvDescribePixelFormat
returns 0 formats" degrades silently to the generic software renderer. Every failure
**after** the app has been handed an ICD pixel-format index (`DrvSetPixelFormat`,
`DrvCreateContext`, `DrvSetContext`, `DrvSwapBuffers`) is returned to the app as a WGL
error — no automatic re-route to software, and once `SetPixelFormat` has *succeeded* on
an ICD format the window is locked to the ICD for its lifetime. **The ICD must refuse at
`DrvDescribePixelFormat` (advertise zero formats), not at context-creation time.** This
changes the host protocol: the "am I allowed in" probe must be answerable at first GL
touch, before any format is handed out.

**Execution update:** T3–T8 and T11 were run against stock XP SP3 and confirmed this
matrix. Two loader details differed from the ReactOS proxy: stock XP requires a
`Flags` registry value, and the working interface
tuple is `{Version=2, DriverVersion=1, Flags=1}`. Matched version 1 loads/resolves the
DLL but does not enter its `Drv*` surface. XP calls `DrvValidateVersion(1)` and supplies
four callbacks for version 2.

---

## 1. ICD discovery & load sequence on XP

All logic in `IntGetIcdData()` (`icdload.c:62-360`), called from `get_dc_data_ex()` at
`wgl.c:76` the first time a process touches a given window/memDC with any WGL call.
Sequence, with the outcome of a failure at each stage:

| # | Stage | Code | On failure |
|---|-------|------|-----------|
| 1 | `ExtEscape(hdc, QUERYESCSUPPORT, &dw=OPENGL_GETINFO)` — ask display driver if it supports the GL escape | `icdload.c:142-147` | ret ≤ 0 → `return NULL` → **silent software fallback** (this is the default state on QEMU std-vga: stock VGA driver answers nothing) |
| 2 | `ExtEscape(hdc, OPENGL_GETINFO, in={ulSubEsc=0}, out=Drv_Opengl_Info{Version, DriverVersion, DriverName})` | `icdload.c:149-157` | ret ≤ 0 → `return NULL` → **silent fallback** (loader logs "claims to support … but doesn't") |
| 3 | Per-process ICD cache lookup by `DriverName` | `icdload.c:166-177` | n/a (hit → reuse) |
| 4 | Open `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers` (`icdload.c:36`); prefer subkey `<DriverName>` with values `Dll` (REG_SZ), `Version`, `DriverVersion`, `Flags` (REG_DWORD); legacy fallback: a plain value `<DriverName>=<dll>` directly under `OpenGLDrivers` ("like the Matrox G400", `icdload.c:186-201`) | `icdload.c:180-257` | key/value missing → `NULL` → **silent fallback**. Stock XP testing found `Flags` mandatory even though ReactOS defaults it. |
| 5 | Registry `Version`/`DriverVersion` cross-checked against the escape's values; mismatch → refuse | `icdload.c:222-228, 236-242` | **silent fallback**. Measured XP tuple: `Version=2`, `DriverVersion=1`, `Flags=1`. |
| 6 | `LoadLibraryW(DllName)` | `icdload.c:268-275` | fail → `NULL` → **silent fallback** |
| 7 | `DrvValidateVersion(DriverVersion)` — **optional export**; only called if present ("Some drivers (at least VBOX), initialize stuff upon this call") | `icdload.c:281-289` | returns FALSE → `goto fail` → `FreeLibrary` + `NULL` → **silent fallback**. Not cached: retried per new window/DC first-touch |
| 8 | `DrvSetCallbackProcs(3, {wglSetCurrentValue, wglGetCurrentValue, wglGetDHGLRC})` — optional | `icdload.c:292-300` | skipped if absent, no failure path |
| 9 | `GetProcAddress` of **16 mandatory exports**: DrvCopyContext, DrvCreateContext, DrvCreateLayerContext, DrvDeleteContext, DrvDescribeLayerPlane, DrvDescribePixelFormat, DrvGetLayerPaletteEntries, DrvGetProcAddress, DrvReleaseContext, DrvRealizeLayerPalette, DrvSetContext, DrvSetLayerPaletteEntries, DrvSetPixelFormat, DrvShareLists, DrvSwapBuffers, DrvSwapLayerBuffers | `icdload.c:303-327` | any missing → `goto fail` → **silent fallback** |
| 10 | If `GdiDescribePixelFormat(hdc,0,0,NULL) != 0`, win32k overrides `DrvDescribePixelFormat/DrvSetPixelFormat/DrvSwapBuffers` ("FIXME: maybe there is a better way") | `icdload.c:329-338` | ReactOS-specific plumbing; see §4 |
| 11 | Immediately after load, `get_dc_data_ex` caches `nb_icd_formats = DrvDescribePixelFormat(hdc, format, size, descr)` — **once per window** ("Get the number of available formats for this DC once and for all") | `wgl.c:76-82` | returns 0 → window has zero ICD formats → all indices map to software → **silent fallback** with the DLL still loaded |

**Escape constants, cross-checked:** `QUERYESCSUPPORT` = 8, `OPENGL_GETINFO` = 0x1101,
output buffer 0x214 bytes (`mesa9x/icdtest.c:17-36`); output struct
`{ULONG ulVersion /* "must be 1 for this version" */, ULONG ulDriverVersion,
WCHAR awch[MAX_PATH+1]}` (`mesa9x/mesa-25.1.x/.../wgl/gldrv.h:562-587` — this header is
a near-verbatim copy of Microsoft's own `gldrv.h`, the best documentation of the real
interface). vmdisp9x answers it with `{Version=2, DriverVersion=1, "QEMUFX"/"VMWSVGA"/…}`
(`vmdisp9x/control.c:84-119, 219-240`). ReactOS's `Drv_Opengl_Info` (`icdload.c:15-20`)
matches field-for-field.

**Registry path correction:** every source agrees the NT key is
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers` (`icdload.c:36`;
`mesa9x/mesa-23.1.x/docs/drivers/llvmpipe.rst:154`), *not* anything under
`Session Manager`. The 9x equivalent is
`HKLM\Software\Microsoft\Windows\CurrentVersion\OpenGLDrivers` (`vmdisp9x/control.c:163`).
Registry-only registration (no escape) is documented as working on **Windows 7+ only**
(`llvmpipe.rst:139-157`); on XP the display driver must answer the escape — confirming
README.md's Phase-2 caveat.

**Summary:** *every* failure in stages 1–9 and a zero return in stage 11 yields
`icd_data == NULL` or `nb_icd_formats == 0` in the DC data, which is indistinguishable
from "no ICD installed" — the state every stock XP box is in. Fallback at this level is
as safe as it gets.

---

## 2. Per-entry-point failure matrix (SetPixelFormat → CreateContext → MakeCurrent → SwapBuffers)

Software formats are appended *after* ICD formats in one index space:
`wglDescribePixelFormat` returns `nb_icd_formats + nb_sw_formats` (`wgl.c:134`); indices
`1..nb_icd_formats` route to the ICD, higher indices to the built-in renderer
(`wgl.c:149-168`, `wgl.c:842-867`). Software formats carry `PFD_GENERIC_FORMAT`
(`swimpl.c:20-24`), ICD formats don't; `wglChoosePixelFormat` prefers non-generic
(`wgl.c:344-351`), so an app that enumerates via ChoosePixelFormat will always be steered
to the ICD range while it advertises anything.

| ICD call | Refused by returning | Loader reaction | App sees | Fallback? |
|---|---|---|---|---|
| `DrvDescribePixelFormat(hdc,0,0,NULL)` (count query at first touch) | 0 | `nb_icd_formats = 0`; all format indices become software formats; a later successful software `SetPixelFormat` also NULLs `icd_data` for the window (`wgl.c:78-82`, `wgl.c:854-866`) | nothing — enumeration shows only generic formats | **YES — silent, total** |
| `DrvDescribePixelFormat(hdc,i,…)` per-format (after a nonzero count was cached) | 0 | `wglDescribePixelFormat` returns 0 for that index (`wgl.c:149-159`); `wglChoosePixelFormat` skips it (`wgl.c:199`) and lands on a generic format | ChoosePixelFormat hands back a software index | **YES for ChoosePixelFormat users; NO for apps that hardcode/compute an index in the ICD range** (their SetPixelFormat then goes to the ICD) |
| `DrvSetPixelFormat(hdc, i)` | FALSE | `wglSetPixelFormat` returns FALSE; `dc_data->pixelformat` stays 0 (`wgl.c:842-851`) | `SetPixelFormat` fails → typical app aborts with "could not set pixel format". Window is **not** poisoned — a retry with a generic-range index succeeds and flips the window software-only (`wgl.c:854-866`) — but stock apps don't retry | **NO (app-visible error; recoverable only if the app itself retries)** |
| `DrvCreateContext` / `DrvCreateLayerContext` | NULL/0 | `wglCreateContext` returns NULL, `ERROR_INVALID_PIXEL_FORMAT` (`wgl.c:405-422`, `wgl.c:479-489`). Because `SetPixelFormat` already succeeded on an ICD index, `dc_data->pixelformat` is set and `icd_data` non-NULL: any retry of SetPixelFormat returns `format == pixelformat` (`wgl.c:836-840`) — **window permanently bound to the refusing ICD** | wglCreateContext fails; no way to get a software context on that window | **NO — window poisoned** |
| `DrvSetContext` | NULL | `wglMakeCurrent` reverts thread binding, clears the dispatch table, returns FALSE with `ERROR_INVALID_PARAMETER` (`wgl.c:700-711`). Note the *old* context was already released at `wgl.c:689-697` → after failure the thread has **no** current context (mixed state) | wglMakeCurrent fails; subsequent `gl*` calls are silent no-ops (NULL dispatch check: `apistubs.c:14-30`; asm path `glapi_x86.s:29-35`) — no crash, but nothing renders | **NO** |
| `DrvSwapBuffers` | FALSE | returned verbatim (`wgl.c:915-916`) | SwapBuffers FALSE; most apps ignore it → frozen/black window | **NO** |
| `DrvReleaseContext`, `DrvDeleteContext` | any | return value largely ignored (`wgl.c:543-546`, `689-697`) | nothing | n/a |

Related once-per-window mechanics: DC data is keyed by **HWND** for window DCs
(`WindowFromDC`, `wgl.c:36-43`), by HDC for memDCs — so pixel-format state and the
ICD/software decision are per-window, matching the documented "a window's pixel format
can be set only once" rule (`wgl.c:836-840` implements set-once: repeat calls just
compare indices, TRUE iff identical).

Mesa9x cross-check — the shipping ICD behaves exactly as this matrix assumes:
- Its global refusal switch is `stw_dev == NULL` (init failed): `DrvDescribePixelFormat`
  → 0 (`stw_pixelformat.c:357-358`), `DrvSetPixelFormat` → FALSE
  (`stw_framebuffer.c:434-435` in 17.3.9), context creation → 0
  (`stw_context.c:175-176`). I.e. VMware/Mesa's own "driver can't run here" path refuses
  at **DescribePixelFormat time**, which is precisely the path known to produce GDI
  Generic on real Windows.
- `DrvSetContext` returns NULL on failure, proc table on success
  (`stw_context.c:944-953`); `DrvCreateContext` → `DrvCreateLayerContext(hdc, 0)`
  (`stw_context.c:119-132`).
- `DrvValidateVersion` ignores its argument and returns TRUE, with a comment confirming
  the argument is the XPDM ExtEscape's DriverVersion (`stw_device.c:261-274`).
- Its export list (`mesa9x/mesa3d.def:2-21`) is exactly ReactOS's 16 mandatory entries
  plus `DrvPresentBuffers`, `DrvSetCallbackProcs`, `DrvValidateVersion`,
  `DllCanUnloadNow` — so the qxpicd export surface should be all 19 `Drv*`.
- One divergence worth knowing: mesa's `DrvSetPixelFormat` itself calls GDI
  `SetPixelFormat` when unset (`stw_framebuffer.c:463-474`), and returns FALSE if called
  a second time on a window (17.3.9 `stw_framebuffer.c:442-453`) — the MS loader
  evidently tolerates both.

---

## 3. The pixel-format subtlety (why "refuse at context-creation" fails)

The three candidate refusal points, in call order for a typical app
(`ChoosePixelFormat → SetPixelFormat → wglCreateContext → wglMakeCurrent`):

1. **Refuse at `DrvDescribePixelFormat` (advertise 0 formats).** The app never sees an
   ICD format. ChoosePixelFormat returns a generic index; SetPixelFormat flips the
   window software-only (`wgl.c:862-864`); the app gets a GDI-generic context with no
   error anywhere. **This is the only fully silent path.** Decision granularity: once
   per window at first GL touch (count cached at `wgl.c:79`), ICD DLL cached per process
   (`icdload.c:166-177`) — a "busy" verdict sticks for that window even if the owner
   later exits, which is acceptable (new windows / new processes re-query).
2. **Refuse at `DrvSetPixelFormat`.** App-visible failure of a GDI call that virtually
   no 1998–2005 app retries. The window is *recoverable* in principle
   (`dc_data->pixelformat` still 0), but only by app-side logic we don't control. Not
   acceptable as the primary mechanism; fine as a belt-and-suspenders backstop for the
   race in §5/T5.
3. **Refuse at `DrvCreateContext`/`DrvSetContext`.** Worst case: SetPixelFormat already
   bound the window to an ICD index; set-once (`wgl.c:836-840`) makes that permanent;
   every subsequent wglCreateContext re-enters the ICD (`wgl.c:405-414`) and fails.
   **The window is poisoned — no software context is reachable on it, ever.** The app
   gets NULL from wglCreateContext and typically dies with an error box.

**Design consequence for the host protocol:** the busy/free decision must be answerable
when the ICD's `DrvDescribePixelFormat` is first called — i.e. a cheap, read-only
"is the passthrough claimable by me?" probe that does **not** allocate host state
(DescribePixelFormat is called by any process touching GDI pixel-format APIs, including
non-rendering ones). There is then an unavoidable race — host may become busy between
"advertised N formats" and the owner's `DrvCreateContext` — so either (a) the host
soft-reserves ownership at the describe-probe with a short TTL, or (b) we accept that
the losing app of a near-simultaneous start gets an error box instead of fallback
(matrix rows 3–5). Mid-session refusal (owner alive, host dies) can never fall back:
the window keeps its ICD format; the ICD must instead keep answering GL calls inertly
(no-op dispatch, FALSE swaps) to avoid crashes — the NULL-dispatch behavior of
`apistubs.c:14-30` only covers the no-context case, not a live context whose host died.

---

## 4. ReactOS-as-proxy caveats (what might not be MS behavior)

Ranked: **H** = high confidence it matches XP, **M** = plausible/unverified, **L** = likely ReactOS-ism.

1. **H** — Load-stage failures (escape, registry, LoadLibrary, missing exports) → generic
   software. This is identical to the universal "no ICD installed" state; also matches
   Mesa/VMware's shipping refusal path (§2). Tests T2–T4 confirm.
2. **H** — Format index spaces: ICD first, generic appended, `PFD_GENERIC_FORMAT`
   marking, ChoosePixelFormat preferring hardware (`wgl.c:134,344-351` — the chooser is
   lifted from Wine, which reverse-engineered Windows).
3. **H** — Set-once pixel format per window; failed SetPixelFormat leaves it unset
   (`wgl.c:836-851`). Matches MSDN semantics.
4. **Resolved on XP (T11)** — formats are re-queried for a new window/DC in the same
   process. A mode-2 first window used GDI Generic; after flipping to mode 6, a second
   window selected ICD format 1 and reported StubICD.
5. **M** — All 16 `Drv*` exports hard-mandatory (`icdload.c:303-327`). Mesa exports all
   of them plus extras, so real-world ICDs are consistent with this, but MS's actual
   mandatory subset is unverified (e.g. XP might tolerate a missing
   `DrvSwapLayerBuffers`). Irrelevant if the stub/real ICD exports all 19. Test T12.
6. **Resolved on XP (T3)** — validation FALSE is retried repeatedly within the same
   process; there is no observed negative process cache.
7. **Resolved on XP (T16)** — registry/escape mismatch is rejected, matched version 1
   does not enter the ICD, and matched `{2,1}` does. Registry `Flags=1` is also
   required. Freeze escape and registry at `{Version=2, DriverVersion=1}`, with
   `Flags=1`; `DrvValidateVersion` receives 1.
8. **M** — After failed `DrvSetContext`, thread left with *no* current context, old
   context force-released (`wgl.c:689-711`). MS might restore the previous binding.
   Cosmetic for our design.
9. **L** — `HKCU\Software\ReactOS\OpenGL` custom-driver override
   (`icdload.c:75-128`) — pure ReactOS debug feature; **cannot be used to register the
   stub on real XP**.
10. **L** — `GdiDescribePixelFormat != 0` → win32k overrides describe/set/swap
   (`icdload.c:329-338`, FIXME'd). Approximation of XP's win32k-managed
   device-format path; details surely differ. Doesn't affect a userland DLL ICD.
11. **L** — `wglSwapLayerBuffers` hardcoded FALSE, `wglSwapMultipleBuffers` 0
   (`wgl.c:921-929`); real XP forwards to the ICD. Only matters for layered-plane apps.
12. **L** — ReactOS's generic renderer is its own Mesa port (`swimpl.c`), so
   GL_RENDERER differs from XP's "GDI Generic" string; fallback *logic* unaffected.

---

## 5. Stub-ICD test battery for real XP

**Stub DLL (`stubicd.dll`):** exports all 19 `Drv*` from `mesa9x/mesa3d.def:2-21`.
Every call logs entry-point name, args, and returned value via `OutputDebugString`
(capture with DbgView) or an append-only `C:\stubicd.log`. Behavior driven by
`HKLM\SOFTWARE\XPDriver\StubICD\FailMode` (REG_DWORD, read at each call so it can be
flipped without reboot):

- Mode A "validate-fail": `DrvValidateVersion` → FALSE, everything else never reached.
- Mode B "zero-formats": `DrvValidateVersion` → TRUE; `DrvDescribePixelFormat` → 0
  always; all context calls → fail if ever reached (they shouldn't be).
- Mode C "advertise-then-refuse-SPF": `DrvDescribePixelFormat` → 1 format (32-bit RGBA
  double-buffered, `PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER`, **no**
  `PFD_GENERIC_FORMAT`); `DrvSetPixelFormat` → FALSE.
- Mode D "accept-SPF-refuse-create": as C but `DrvSetPixelFormat` → TRUE,
  `DrvCreateContext`/`DrvCreateLayerContext` → 0.
- Mode E "accept-create-refuse-SetContext": as D but create returns handle 1,
  `DrvSetContext` → NULL.
- Mode F "inert-success": full success path — `DrvSetContext` returns a
  `GLCLTPROCTABLE` (cEntries = 336) of no-op stubs whose `GetString` returns
  "StubICD"; `DrvSwapBuffers` → FALSE.

**Probe app (`glprobe.exe`)** — extend `mesa9x/wgltest.c` (it already drives
system/wrapper/ICD modes): logs, in order: DescribePixelFormat total count + per-format
`PFD_GENERIC_FORMAT` bit; ChoosePixelFormat result index; SetPixelFormat return +
GetLastError; wglCreateContext return + GetLastError; wglMakeCurrent return;
`glGetString(GL_VENDOR/GL_RENDERER/GL_VERSION)`; draws one colored triangle + SwapBuffers;
**then** (for the poisoning question) retries SetPixelFormat with the first
generic-flagged index on the *same window*, retries wglCreateContext, logs both; then
repeats the whole sequence on a *fresh window*. Record per test: (1) probe log,
(2) stub log (which entry points the real loader called, in what order), (3) crash Y/N,
(4) window contents (triangle / white / black), (5) GL_RENDERER string.

**Rig.** Two environments:
- **R1 — QEMU std-vga XP**: stock VGA driver never answers
  `QUERYESCSUPPORT(OPENGL_GETINFO)` → the MS loader can't be pointed at the stub via
  registry alone on XP (§1, Win7+ only). Use R1 only for T0 and for the *interim
  opengl32-swap* guest-side tests (README.md:66-71: replace `system32\opengl32.dll`
  outright under an SFC/WFP exception — copy over both `system32\opengl32.dll` and
  `system32\dllcache\opengl32.dll`, dismiss/suppress the WFP dialog; the old VirtualBox
  installer technique). Note the swap **removes the MS loader**, so it tests Phase-1
  plumbing, not fallback.
- **R2 — VirtualBox (or VMware) XP guest with guest additions video driver**: the
  display driver answers the escape and names a registered ICD. Hijack it: under
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\<Name>` repoint
  `Dll` at `stubicd.dll` (keep `Version`/`DriverVersion` values identical to what the
  escape reports — dump them first with a port of `mesa9x/icdtest.c`, escape 8/0x1101,
  0x214-byte buffer). This exercises the **genuine XP `opengl32.dll` loader** against a
  refusing ICD; the display driver underneath is irrelevant to loader logic.

**Tests.** (Run probe + a real game-shaped app — GLQuake or `scrsave 3D Pipes` — per
test; record against test number.)

- **T0 (R1)** Baseline, untouched XP on std-vga. Expect: only generic formats,
  GL_RENDERER "GDI Generic", triangle renders. Confirms probe correctness + stage-1
  fallback (`icdload.c:142-147`).
- **T1 (R2)** Baseline with the real additions ICD. Expect accelerated renderer string.
  Confirms rig actually loads an ICD; stub log later comparable.
- **T2 (R2)** Registry `Dll` points at a nonexistent file. Expect: silent GDI Generic,
  no error UI. Confirms `icdload.c:268-275` behavior on real XP.
- **T3 (R2)** Mode A. Expect: silent GDI Generic. Also run probe twice in one process
  (two windows) and check stub log: is `DrvValidateVersion` re-called (ReactOS predicts
  yes, §4.6)? Confirms stage-7 fallback + refusal granularity.
- **T4 (R2)** Mode B. Expect: silent GDI Generic; stub log shows only
  describe-count calls; **no** DrvSetPixelFormat/DrvCreateContext ever. **This is the
  design-critical test** — it validates the §3 point-1 refusal path end-to-end.
- **T5 (R2)** Mode C. Expect: ChoosePixelFormat returns the ICD index (1);
  SetPixelFormat FALSE; app-visible failure. Probe's retry with a generic index on the
  same window expected to SUCCEED and yield GDI Generic (window not poisoned,
  `wgl.c:842-851` prediction). Refutes/confirms "SetPixelFormat-refusal is survivable
  only with app cooperation".
- **T6 (R2)** Mode D. Expect: SetPixelFormat TRUE, wglCreateContext NULL
  (GetLastError = 2000 `ERROR_INVALID_PIXEL_FORMAT` if ReactOS matches XP); retry of
  SetPixelFormat with generic index on same window expected to FAIL (set-once) and
  fresh-window run expected to work → **window poisoned, process survivable**. Confirms
  §3 point-3. Also note from stub log whether XP calls `DrvCreateContext` or
  `DrvCreateLayerContext(hdc,0)` for plain wglCreateContext.
- **T7 (R2)** Mode E. Expect: wglMakeCurrent FALSE; subsequent `gl*` calls no-op (no
  crash); window poisoned as T6. Checks NULL-dispatch safety (`apistubs.c:14-30`
  equivalent in real opengl32).
- **T8 (R2)** Mode F. Expect: context current, GL_RENDERER "StubICD", swap fails,
  black-but-alive window, clean exit. Documents mid-session-failure UX (the "owner's
  host died" scenario) — confirms it's crash-free but *not* a fallback.
- **T9 (R2)** Mode B in process 1 (running), flip FailMode to real-ICD passthrough n/a —
  instead: start probe #1 under Mode F (holds "acceleration"), flip to Mode B, start
  probe #2. Expect: #2 silently GDI Generic while #1 unaffected. Simulates
  owner-plus-latecomer with per-process independence.
- **T10 (R1)** opengl32-swap route sanity: mesa9x `opengl32.dll` (softpipe) dropped in
  `system32` + dllcache with WFP exception; run probe + 3D screensaver. Expect both get
  Mesa. Documents the interim install path's side effect (no MS software renderer left —
  the README.md:66-71 caveat; softgpu/README notes 3D Maze-class apps break, mesa9x
  README.md:22).
- **T11 (R2)** Mode B→(real ICD) flip between two windows of ONE process: does window 2
  get ICD formats after window 1 got zero? Stub log shows whether XP re-queries
  `DrvDescribePixelFormat` per window/call or caches per process (§4.4). Determines the
  granularity the host probe must assume.
- **T12 (R2, optional)** Remove one export (`DrvSwapLayerBuffers`) from an otherwise
  Mode-F stub. Silent fallback (ReactOS predicts yes per §1 stage 9) or load anyway?
  Pins down the real mandatory-export set (§4.5).

Success criterion for the project design: T2, T3, T4 all show GDI Generic with zero user-
visible errors, and T4's stub log shows the refusal is consulted before any format is
granted. T6 confirming poisoning is *expected* and equally load-bearing: it proves the
refusal must not be deferred to context creation.

---

## 6. Verdict

The source supports the README design **with one amendment**. "If the ICD refuses, MS
opengl32 falls back to software" is true as written only when the refusal happens at
load/enumeration time — any failure from the display-driver escape through
`DrvDescribePixelFormat` returning zero formats leaves the process in the exact
"no ICD installed" state, and the app silently gets generic GL
(`icdload.c:142-327`, `wgl.c:76-82,854-866`). But README.md's phrasing "returns failure
from its pixel-format/context entry points" is too loose: refusing at
`DrvSetPixelFormat` is an app-visible error (survivable only if the app retries with a
generic format, which real games don't), and refusing at
`DrvCreateContext`/`DrvSetContext` permanently poisons the window (set-once ICD format,
`wgl.c:836-840,405-422`) — no software context is ever reachable on it. **The refusal
point must move to `DrvDescribePixelFormat`: host-busy ⇒ advertise zero pixel formats at
the client's first GL touch** — precisely the refusal path Mesa/VMware's shipping ICD
uses when its backend can't initialize (`stw_pixelformat.c:357-358`). No
deny-at-attestation pivot is needed; the host protocol just needs a cheap read-only
claimable-probe answerable at describe time, plus (optionally) a soft-reservation to
close the describe→create race. Confidence is high but rests on ReactOS+Mesa as proxies;
tests T2–T6 on real XP are the cheap decisive check before any host code.
