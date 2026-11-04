# External web evidence: does an ICD "refuse at runtime" fall back to software GL on Windows XP?

Scope: independent **web-evidence** lens on the load-bearing assumption in `README.md`:

> "if the ICD refuses (returns failure from its pixel-format/context entry points),
> Microsoft's `opengl32.dll` falls back to its software renderer."

A parallel effort analyzes ReactOS/mesa9x **source** (`docs/icd-fallback-analysis.md`, not
touched here). This file gathers documented external evidence and weighs it.

Gathered 2026-07-10.

## The distinction that decides everything

The assumption bundles two very different failure modes. External evidence treats them
completely differently, so keep them apart:

- **(A) ICD not loaded** — DLL missing, wrong registry entry, version-validation fails,
  or the display driver never answers the ICD-name query. opengl32 never gets a working
  ICD, so the device context only ever offers *generic* pixel formats and the app renders
  in software (GDI Generic). **Fallback here is near-certain and well documented.**
- **(B) ICD loaded, then refuses a `Drv*` call at runtime** — `DrvSetPixelFormat`,
  `DrvCreateContext`/`DrvCreateLayerContext`, or `DrvSetContext` returns FALSE/NULL *after*
  the ICD is up. **This is the case XPDriver actually depends on**, and it is *not*
  supported by external evidence the way the README implies.

Evidence for (A) does **not** prove (B). Most of the "OpenGL falls back to software"
folklore online is really about (A).

---

## Angle 1 — Microsoft's own documentation

**Loading an OpenGL ICD** (Microsoft Learn, WDK / display drivers)
<https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver>

Describes the modern (Vista/WDDM) load path: the runtime calls `D3DKMTQueryAdapterInfo`
(`KMTQAITYPE_UMOPENGLINFO`) to get the ICD name/version/flags, validates the version, then
loads by name. Registry entries per adapter: `OpenGLDriverName`, `OpenGLVersion`,
`OpenGLFlags`. This documents *loading and version-validation* (case A). It says **nothing**
about runtime `Drv*` failures or a runtime fallback. On XP the discovery path is different
(ExtEscape, see Angle 5) but the "validate version or drop the ICD" logic is the same.

**wglCreateContext** (Microsoft Learn, wingdi.h)
<https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglcreatecontext>

> "If the function succeeds, the return value is a valid handle to an OpenGL rendering
> context. **If the function fails, the return value is NULL.** To get extended error
> information, call GetLastError."

The public contract for context creation is **success → HGLRC, failure → NULL**. There is
**no documented "on failure, substitute a software context"** behavior. Since `wglCreateContext`
is a thin wrapper that calls the ICD's `DrvCreateContext`/`DrvCreateLayerContext`, an ICD that
returns NULL there produces `wglCreateContext == NULL` in the app — i.e. an error, not a
silent software context. This is the single most relevant primary doc and it points *against*
the runtime-refusal assumption.

**Pixel Formats** (Microsoft Learn)
<https://learn.microsoft.com/en-us/windows/win32/opengl/pixel-formats>

> "A given device context can support several pixel formats. Windows identifies the pixel
> formats that a device context supports with consecutive one-based index values… A device
> context can have just one current pixel format… the rendering context inherits the device
> context's pixel format."

Establishes that whether you get hardware or software is a property of the **selected pixel
format**, fixed at `SetPixelFormat` time and inherited by the context — not something
renegotiated when a later call fails.

KB angle: searches for MS KB articles specifically about "broken/missing OpenGL driver →
automatic software fallback" turned up nothing. Microsoft docs consistently frame software
GL as "what you get when there is no ICD," never as a runtime rescue for a failing one.

## Angle 2 — OpenGL.org / Khronos: when do you land on "GDI Generic"?

**OpenGL FAQ / 5 — Microsoft Windows Specifics** (opengl.org archive)
<https://www.opengl.org/archives/resources/faq/technical/mswindows.htm>

- 5.130: `opengl32.dll` "contains the ICD kit, sample drivers, and **software OpenGL
  implementation**" — the generic renderer ships inside opengl32 itself.
- 5.100 / 5.040: which implementation you use is read from **pixel-format flags**:
  "You want both [`GENERIC_FORMAT` and `GENERIC_ACCELERATED`] on if you're using a 3D-DDI or
  an MCD and **neither on if you are using an ICD**." "If `PFD_GENERIC_ACCELERATED` is clear
  and `PFD_GENERIC_FORMAT` is set, then the pixel format is only supported by the generic
  implementation."
- 5.030: "`ChoosePixelFormat()` … **always selects a hardware accelerated pixel format when
  one is available**." To force software you must *not* use ChoosePixelFormat and instead
  iterate `DescribePixelFormat()` and deliberately pick a generic format.
- 5.040 (key for case A vs B): "Another way to check for hardware acceleration is to
  **temporarily remove or rename the ICD, so it can't be loaded**." — the canonical
  "how to get software" recipe is *unload the ICD* (case A), not "make the ICD fail a call."

**"Renderer Detection, GDI Generic & hardware accel." / "GDI Generic renderer"** (Khronos forums)
<https://community.khronos.org/t/renderer-detection-gdi-generic-hardware-accel/58722>

Community consensus on *why* apps unexpectedly get GDI Generic: either the ICD is missing/not
installed (case A), or **ChoosePixelFormat picked a `PFD_GENERIC_FORMAT` format** ("all of them
are marked as software modes"). Recommended fix: "Use DescribePixelFormat … and choose one with
your OWN CODE." Nobody describes a *loaded* ICD failing a runtime call and Windows quietly
swapping in software.

Takeaway: **the documented software-fallback mechanism is pixel-format selection, evaluated at
`ChoosePixelFormat`/`SetPixelFormat` time by flags — not a runtime rescue when a `Drv*` call
returns failure.**

## Angle 3 — VirtualBox (XP guest, 3D on then broken/disabled)

**Manual / GA behavior** (virtualbox.org manual ch.04; Oracle docs 4.5)
<https://www.virtualbox.org/manual/ch04.html> ·
<https://documentation.help/VirtualBox/ch04s05.html>

- XP guests need GAs installed in **Safe Mode**; enabling D3D means VirtualBox "replace[s]
  Windows system files in the virtual machine" — the same `system32` DLL-swap technique the
  README cites for the interim opengl32 replacement.
- When 3D is off/unavailable the guest reports **OpenGL 1.1** — "the software renderer built
  into Windows since forever (early NT days)… you ain't got no hardware accelerated OpenGL."
  This is a **case-A** fallback: no accelerated ICD present → generic software.

**Runtime failure of the passthrough (not a clean fallback):** "OpenGL broken with guest additions"
<https://forums.virtualbox.org/viewtopic.php?t=50124&p=228799>

When the accelerated GL path was present but the *passthrough layer* failed at runtime, the app
did **not** degrade to software — it produced hard errors ("No pincher, please call
crStateSetCurrentPointers()… X Error: BadDrawable") and broken output. This is the closest public
analog to case B (a loaded accelerated GL path failing mid-stream): the result was **failure, not
silent software substitution.** (Caveat: this is VirtualBox's Chromium/host-passthrough stack on
an X host, not the Windows ICD entry points themselves — treat as directional, not decisive.)

**Installer opengl32 swap postmortems:** old GA installers (pre-4.1.8) replaced protected system
files (incl. graphics DLLs) with no checks and relied on tricks to get past Windows File
Protection; community reports of the swapped/unsigned `opengl32.dll` then failing to load. Confirms
the SFC-exception technique is real and was fragile, but these are load-time breakages (case A),
not evidence about runtime `Drv*` refusal.
<https://forums.virtualbox.org/viewtopic.php?t=80622> ·
<https://learn.microsoft.com/en-us/answers/questions/3221154/virtualbox-wont-start-due-to-unsigned-opengl32-dll>

## Angle 4 — JHRobotics (mesa9x / softgpu / vmdisp9x): the recent real ICD

**mesa9x README** <https://github.com/JHRobotics/mesa9x>

> "In Microsoft Windows OS is OpenGL driver implement different way than DirectX API… in
> OpenGL case the video driver reports only name of OpenGL driver system and `opengl32.dll`
> only forward OpenGL calls to library associated with this driver (name of DLL is in
> registry)."

Two supported install modes are documented: (1) app-local `opengl32.dll` drop-in, (2) a real
**OpenGL ICD** registered so the stock MS `opengl32.dll` forwards to it. Confirms the ICD model
and registry-name indirection the README relies on, and that a software-only ICD (LLVMpipe/
softpipe) is a first-class configuration.

**softgpu / vmdisp9x READMEs** <https://github.com/JHRobotics/softgpu> ·
<https://github.com/JHRobotics/vmdisp9x>

softgpu's thin display driver exists precisely to make ICD discovery work (the vmdisp9x /
Necasek minidriver answers the OpenGL-name query — the 9x analog of the XP ExtEscape caveat in
`README.md` Phase 2). **Dead end for our specific question:** none of the JHRobotics READMEs (or
the VOGONS SoftGPU thread, <https://www.vogons.org/viewtopic.php?t=93671>) contain an explicit
statement that a *loaded* ICD returning failure from a `Drv*` call yields silent MS-software GL.
Their fallback story is "select the software Mesa driver," i.e. choosing a software *pipe inside
their own ICD*, not MS opengl32 rescuing a failing ICD. This maintainer — who shipped a real
XP/9x ICD — does not document case (B), which is mild evidence that it isn't a relied-upon
behavior.

## Angle 5 — Driver-developer lore / ICD architecture

**MS OpenGL ICD architecture** (DDK-era doc; original `read.pudn.com/.../opengl-icd.doc` now
dead, read via mirror)
<https://topic.alibabacloud.com/a/font-classtopic-s-color00c1deopenglfont-font-classtopic-s-color00c1dedriverfont-architecture_8_8_32257105.html>

> "When the application calls describepixelformat / getpixelformat / setpixelformat /
> choosepixelformat… opengl32 first displays the OpenGL pixel formats supported by the card
> driver… **If the driver does not have a corresponding callback function, opengl32.dll calls
> the corresponding function in the ICD driver** to complete the operation."

Documents the pixel-format handshake and the `wglCreateContext → DrvCreateContext →
DrvCreateLayerContext` call chain — but, like every other source, describes only the **success
path**. No mirror of this doc describes an opengl32 fallback when a `Drv*` call *fails*.

**XP ICD discovery (corroborates README caveat):** the ICD name is obtained by `ExtEscape` on the
display driver — sub-function `OPENGL_GETINFO_DRVNAME` returns the display-driver name, which
opengl32 uses to look up the ICD DLL in the registry. Documented as "only supported for display
drivers on Windows NT 4.0 and (in some cases) Windows NT 5.0." This is exactly the escape the
stock std-vga driver won't answer — matching the README's Phase-2 rationale. (Surfaced via the
MS ICD doc + ReactOS references below.)

**ReactOS `opengl32` ICD loader** (clean-room reimplementation; overlaps the source agent's remit,
cited here as an external reference, weighted as reimplementation not MS)
<https://doxygen.reactos.org/d3/dea/icdload_8c.html>

- Version-validation failure drops the ICD entirely: `if(!DrvValidateVersion(...)) { ERR(...);
  goto fail; }` → frees the library, returns NULL → app runs on generic (case A).
- The only GDI/software substitution is set up **at ICD init**, driven by pixel-format support,
  not by runtime failure: "Let's see if GDI should handle this instead of the ICD DLL… `if
  (GdiDescribePixelFormat(hdc,0,0,NULL) != 0) { /* GDI knows what to do with that. Override */
  data->DrvDescribePixelFormat = GdiDescribePixelFormat; }`" — it overrides only
  DescribePixelFormat/SetPixelFormat/SwapBuffers with GDI equivalents when GDI reports generic
  support. There is **no code path that, on a later `DrvCreateContext`/`DrvSetContext` failure,
  substitutes software** — the WGL call just fails. (Independent read; the source agent's file
  is authoritative on ReactOS specifics.)

## Angle 6 — Counter-evidence: broken ICD that did NOT fall back

- **Missing/broken ICD → app dies, not software.** Classic XP-era reports: Quake-engine
  `GLW_StartOpenGL() - could not load OpenGL subsystem`
  (<https://www.pcreview.co.uk/threads/glw_startopengl-could-not-load-opengl-subsystem.508822/>);
  games declaring "OpenGL not supported" on a GeForce4/XP box
  (<https://community.khronos.org/t/opengl-problems-with-windows-xp/8699>). Aggregated summary of
  the era: "Games requiring hardware acceleration **will fail with errors when the ICD is missing
  or broken**." Note the conflation of *missing* (A) and *broken* (B) — but the observed outcome
  is app failure, not a seamless software substitution the user never notices.
- **Modern Intel ICD crash** (<https://community.intel.com/t5/Graphics/OpenGL-crashing-on-Windows-Intel-integrated-graphics-driver/td-p/1722567>,
  fetch 403'd — read via search index): a bad Intel ICD build (15.65.3.4944) made OpenGL apps
  **crash**, not drop to GDI Generic. When a loaded ICD misbehaves, the app takes the fault.
- **`DrvSetContext` failure** (Microsoft Q&A / NVIDIA): reported as "OpenGL driver failed to
  switch rendering contexts" surfaced to the app — an error condition, again with no automatic
  software takeover.
- **`SetPixelFormat` returning FALSE** on specific GPUs/drivers (Khronos/GameDev threads): treated
  as a hard failure the app must handle; the documented cure is per-format enumeration, not "the
  OS will fall back for you."

No source in this sweep documents a case where a *loaded* ICD returned failure from a context or
pixel-format entry point and the app **silently and successfully** continued on Microsoft software
GL.

---

## Evidence-quality table

| # | Source | Tier | Speaks to | What it supports |
|---|--------|------|-----------|------------------|
| MS `wglCreateContext` doc | Primary (MS) | B (context) | failure → **NULL to app**, no fallback documented | **Against** runtime fallback |
| MS "Loading an OpenGL ICD" | Primary (MS) | A (load) | registry load + version-validate or drop | For (A) only |
| MS "Pixel Formats" | Primary (MS) | mechanism | hw/sw fixed by selected pixel format, inherited by context | Reframes: it's a *selection*, not a runtime decision |
| OpenGL.org FAQ §5 | Primary-ish (Khronos archive) | A + mechanism | generic vs ICD chosen by PFD flags; "remove/rename ICD" to get software; ChoosePixelFormat prefers hardware | Strong for (A); reframes (B) |
| MS ICD architecture doc (mirror) | Primary (MS, mirrored) | mechanism | pixel-format handshake + Drv call chain, **success path only** | Neutral; no fallback on failure |
| ReactOS `opengl32` loader (doxygen) | Reimplementation | A + B | drop-on-version-fail; GDI override only at init by pixel-format; **no runtime-failure fallback** | Against runtime fallback (corroborating, non-MS) |
| JHRobotics mesa9x/softgpu READMEs | Maintainer (shipping ICD) | model | confirms ICD/registry model; software = own softpipe, not MS rescue | Neutral / mild against (B) |
| VirtualBox manual + GA behavior | Shipping vendor | A | 3D off → OpenGL 1.1 software; system-DLL swap technique real | For (A); confirms SFC-swap approach |
| VirtualBox "OpenGL broken with GAs" | Vendor forum | B-analog | runtime passthrough failure → hard errors, **not** software | Against (directional) |
| XP-era "GL subsystem could not load" / Intel crash / DrvSetContext fail | Forum anecdote | A/B | broken/loaded-then-failing ICD → app error or crash | Against runtime fallback |

Tier order applied: primary MS/Khronos docs and shipping-vendor behavior > clean-room
reimplementation > maintainer READMEs > forum anecdotes.

---

## Bottom line

External evidence **splits the assumption in two and supports only half of it.**

**Well supported (case A — ICD absent / fails to load / fails version validation):** the app
lands on Microsoft's software renderer (GDI Generic). This is stated or implied by MS docs, the
OpenGL.org FAQ ("remove or rename the ICD, so it can't be loaded"), VirtualBox's "OpenGL 1.1 =
software" behavior, and the ReactOS loader dropping the ICD on `DrvValidateVersion` failure. If
XPDriver's refusal takes the form of *the ICD not offering itself / not being loadable for a
client*, silent software GL is near-certain.

**Not supported (case B — the one XPDriver actually depends on: a *loaded* ICD returns failure
from `DrvCreateContext`/`DrvCreateLayerContext`/`DrvSetContext` at runtime):** no source describes
opengl32 quietly substituting a working software context. The documented contract is the opposite
— `wglCreateContext` failure returns **NULL to the application**, and real-world broken/failing
ICDs make apps **error out or crash**, not degrade seamlessly. The genuine software path is chosen
**earlier, at pixel-format selection**, by the `PFD_GENERIC_FORMAT` flag on the format the app
picked — and `ChoosePixelFormat` prefers hardware, so an app that got an accelerated format and is
then refused at context creation will fail rather than fall back.

**Actionable implication for the design:** to make a *second* client silently get software GL, the
ICD should refuse at the **pixel-format enumeration / `DrvSetPixelFormat`** stage — i.e. present
that client's device context with **only generic (GDI) pixel formats** (return zero accelerated
formats from `DrvDescribePixelFormat` for it), so `ChoosePixelFormat` necessarily selects a generic
format and rendering goes through GDI software *without the ICD ever being asked to create a
context*. Refusing later, at `DrvCreateContext`/`DrvSetContext`, is **not** a documented soft-fail
and external evidence predicts it will surface as `wglCreateContext == NULL` and an app error.
(Even the pixel-format route depends on the app using `ChoosePixelFormat`/re-querying rather than a
hardcoded accelerated format index — well-behaved apps do, some games don't.) This should be
validated empirically on a real XP + ICD test rig; the design should not treat "refuse at any
`Drv*` entry point → silent software fallback" as a given.

**Strongest single piece FOR the assumption:** OpenGL.org FAQ §5.040 — the canonical way to force
software rendering is to "temporarily remove or rename the ICD, so it can't be loaded" (proves
robust fallback, but only for case A).

**Strongest single piece AGAINST (for the runtime case):** MS `wglCreateContext` docs — "If the
function fails, the return value is **NULL**" with no fallback documented, corroborated by the
ReactOS loader having no runtime-failure→software path and by real broken-ICD reports where apps
crashed/failed instead of degrading.
