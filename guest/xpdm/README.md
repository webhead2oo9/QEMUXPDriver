# XPDM discovery component

This directory holds the first shared code for the future `qxpdisp.dll` XPDM
display driver. `qxp_escape.c` implements the shipping `QUERYESCSUPPORT` and
`OPENGL_GETINFO` contract without CRT or Win32 dependencies. Its output is the
measured stock-XP tuple `{Version=2, DriverVersion=1, DriverName=L"qxpicd"}` and
the exact 0x214-byte NT structure.

`qxpdisp_escape.c` is the thin `winddi.h` wrapper compiled into the display
DLL. The `DrvEnableDriver` table includes:

```c
{ INDEX_DrvEscape, (PFN)DrvEscape }
```

Run `make test` in WSL to validate probe filtering, input/output bounds,
subescape handling, structure layout, tuple, UTF-16 name, and zero-filled tail.
The build also cross-compiles the shared implementation to an i686 Windows
object. The legacy WDK is kept as an ignored, locally extracted toolchain under
`.tools/`; it is never installed into Windows or included in project output.

`qxp.inf` binds the paired NVIDIA PCI identity and legacy `PCI\VEN_1234&DEV_1111`,
installs `qxpmp.sys`, `qxpmap.sys`, `qxpdisp.dll`, `qxpicd.dll`, and
`qxpcoin2.dll` to their XP system destinations, and selects `qxpdisp` through
`InstalledDisplayDrivers`. `qxpmp` remains the INF's only service: the associated
Plug-and-Play video service (demand start). After successful device installation,
the signed co-installer creates the shared `qxpmap` kernel service with system
start, normal error control, image `\SystemRoot\system32\drivers\qxpmap.sys`, and
display name "XPDriver guest memory-window driver". The INF only copies this
image because authentic ChkINF reports W22.1.2027 for both `StartType=1` and
`StartType=2` in the display INF. The service serves both the OpenGL ICD and QD3D.
The INF uses
[`COPYFLG_NO_VERSION_DIALOG`](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive)
for that shared image: an installed higher VERSIONINFO `FILEVERSION` is kept,
not downgraded by an older package.

Clean XP display-class installs filter the INF's global `OpenGLDrivers`
entries, so the signed co-installer writes the same measured tuple during
`DIF_INSTALLDEVICE`; the declarative entries remain for compatible installers.
The co-installer requests post-processing only after that registration succeeds.
Its service setup accepts only the shared kernel-service identity and promotes
only DEMAND_START or DISABLED services to SYSTEM_START. Existing BOOT_START,
SYSTEM_START, and AUTO_START settings remain untouched, so a pending QD3D image
replacement can retain AUTO_START until SMSS completes the rename before SCM
loads the driver. Error control is normalized to NORMAL; foreign or
marked-for-deletion services remain untouched. Service creation/start is best effort
and never fails the display install; the ICD can fall back until the next boot.
Device removal never deletes this shared service. Host unit tests replace every
registry/SCM operation with fakes rather than touching host installation state.
The `OpenGLDrivers\qxpicd` key also marks QD3D's shared-driver dependency check.
`validate_inf.py` keeps the PCI binding, filenames, destinations, service
contracts, copy-version policy, co-installer registration, and header/registry
versions synchronized. `validate_coinstaller.py` additionally requires an i386
PE32 XP 5.1 DLL, the exact `QxpCoInstaller` export, and the minimal system-DLL
import set.

Change a previously shipped co-installer DLL's filename whenever its behavior
changes; its `QxpCoInstaller` export may stay unchanged. Unreleased revisions can
retain their filename. XP can keep the installed DLL loaded
during an upgrade, queue its replacement until reboot, and execute that older
co-installer for `DIF_INSTALLDEVICE`. The mapping-service implementation therefore
ships as `qxpcoin2.dll`, not the old `qxpcoinst.dll`, so upgrades load the new
code immediately. Staging removes the obsolete DLL from the mutable package;
historical frozen packages are unchanged.

The framebuffer display driver creates the default `PAL_BITFIELDS` palette and
reports the standard XP graphics/dither capabilities before associating its
engine-managed framebuffer surface. This palette is required by win32k; a null
`DEVINFO.hpalDefault` bugchecked XP immediately after `DrvEnableSurface`.

The INF names `qxp.cat` so Microsoft ChkINF can validate a structurally complete
third-party XPDM package. `stage-package.cmd <WDK-root>` builds all five binaries,
including `qxpmap.sys` through its warning-rejecting authentic WDK/native-driver
gate, and runs ICD dispatch and co-installer coverage. Build or copy failures
stop staging. The exact eight-member package under the ignored `build/package`
directory contains those five binaries, `qxp.inf`, `qxp.cat`, and
`xpdriver-test.cer`; Microsoft Inf2Cat covers the six INF/binary payloads.
By default the script creates a throwaway SHA-1 development certificate,
test-signs the catalog and includes the public certificate for the installer
to trust: development signing for automatic-rollback VM testing. With
`XPD_SIGN_PFX` set, `sign-package.ps1` signs with that persistent project
certificate instead (created once by `tools\new-signing-cert.ps1`, kept
outside the repository, password in `XPD_SIGN_PFX_PASSWORD` or prompted)
and bundles its public part, so releases keep one signer. Neither is a
production trust chain.

Versions come from the repository's `VERSION` file and `tools/version.py`:
MAJOR.MINOR.PATCH plus the number of commits reachable from HEAD. Every staged
binary carries that numeric FILEVERSION/PRODUCTVERSION; its string
ProductVersion also identifies HEAD's short commit hash and any tracked dirty
work. WDK build wrappers generate `build/version/xpdriver_version.h` before
building, and the co-installer's MinGW resource uses the shared
`tools/version.mk` rule. Staging replaces the copied INF's DriverVer with
`tools/version.py --driverver` (HEAD's commit date and the same numeric version)
before Inf2Cat. The checked-in INF retains only a valid ChkINF template value;
it is not the shipped version authority.

The same package is suitable for Windows XP's GUI-mode Plug-and-Play setup.
`tools\slipstream-xp.ps1` stages its seven required files beneath an extracted
XP x86 source's `I386\$OEM$\$1\Drivers\XPDriver` directory and safely updates
the source's `WINNT.SIF` with `OemPreinstall` and `OemPnPDriversPath`. It keeps
the original answer file, preserves unrelated sections and existing driver
paths, and is idempotent. Because the repository build is signed by a local
development certificate, the default integration also selects XP's Ignore
driver-signing policy; `-KeepDriverSigningPolicy` is available for a package
with an already trusted production catalog.

The real-XP persistent-overlay acceptance path installs the package and verifies
the installed files and exact ICD registry tuple. Before reporting
`QXPSETUP stage=files-verified`, `qxpsetup` also requires
`system32\drivers\qxpmap.sys` and the `Services\qxpmap` DWORD values `Type=1` and
`Start=1`. It also accepts `Start=2` only when the Session Manager's
`PendingFileRenameOperations` REG_MULTI_SZ contains a complete entry naming
`qxpmap.sys` (case-insensitively); absent or malformed data does not qualify.
The existing `FILES_OK=1` and `QXPMAP_SERVICE_OK=1` log markers are unchanged.
The later cold-boot stage loads the XPDM miniport and display DLL, routes stock
`opengl32.dll` through the native `OPENGL_GETINFO` escape into QXPICD, renders
through the host, verifies a concurrent GDI Generic fallback client, and requires
`QXPICD_RESULT rc=0`.

The native Direct3D path keeps Microsoft's stock XP DirectX runtimes. The
display driver sends bounded private video IOCTLs to a kernel miniport transport
that maps the existing Mesa MMIO/FIFO ranges, reserves a `QXPD3D` session,
maintains its heartbeat, and tears it down with the D3D context. The display DLL
tracks both video- and system-memory `CreateSurfaceEx` associations until their
owning DirectDraw object is destroyed, translates checked DP2 commands including
clipped triangle fans, and bounds-checks all vertex/index fetches. XP D3D8's
system-memory execute-buffer uploads are treated as circular only for that
specific surface class, including vertices that straddle the end of the buffer;
ordinary out-of-range sources remain errors. The miniport transport keeps
projective texture coordinates finite at the host boundary so malformed unused
coordinates cannot reject an otherwise valid game draw.

Translated draws use packed Mesa vertex arrays. Commands and their aligned vector
and vertex payloads are queued together until Present, a synchronous resource
operation, or the FIFO capacity boundary; miniport teardown reports draw, present,
clear, upload, batch, and maximum-batch counters for acceptance measurements.
The compact indices introduced in transport ABI 47 remain in the current ABI
53: byte indices are stored first, zero-padded to eight bytes, then followed by
transformed vertex records. The miniport validates that every declared
vertex is referenced and publishes that host-ready span with one bounded copy.
QXPD3D/3 identifies the paired attachment while the host retains `/1` and `/2`
compatibility. The
draw state transports D3D point, wireframe, and solid fill modes to host polygon
mode, including the corresponding polygon-offset target for integer depth bias.
Hardware Flip records both the presented and next owner-scoped surface handles in a
bounded presentation-chain set before a following SETRENDERTARGET. Every render
surface, including each swap-chain member, retains an independent host texture.
Presentation members use display-space orientation, while ordinary offscreen render
targets retain the deliberately opposite texture-space projection. Hardware Flip
copies the selected member to framebuffer zero, publishes it, and exchanges the host
buffers. Windowed Unlock/Blt presentation performs the same copy but publishes
without exchanging, preserving the contents required by `D3DSWAPEFFECT_COPY`.
Successful Lock calls record their access flags; presentation, read-only, and
duplicate Unlock callbacks cannot invalidate the host texture or upload stale guest
VRAM over accelerated rendering. Clear requests carry the
target dimensions and clipped D3D rectangles to the miniport. Zero-rectangle clears
use the active viewport (or the full target), and lists larger than 32 rectangles
are sent in bounded chunks. Host scissoring applies the same rectangle semantics to
color, depth, and stencil targets, including the required top-left-to-bottom-left
conversion for display-space presentation textures. Clear invalidates cached draw
state after temporarily overriding D3D color, depth, or stencil write masks.
Result-bearing Direct3D calls, including framebuffer
completeness, run inside a high-priority `VideoPortSynchronizeExecution` callback and
consume the globally completed `0xFA4` reply. They therefore do not depend on the
vCPU that executes a later MMIO load; the ordinary `0xFC0` per-vCPU reply remains for
legacy clients outside this D3D path.

Object-space table fog follows D3D's post-world/view/projection device Z, including
the homogeneous divide, rather than the original object or clip Z. The miniport
computes the required Z/W coordinate with bounded integer IEEE-754 helpers because
an XP video miniport cannot borrow the interrupted thread's floating-point state.
The explicit fog-coordinate array feeds linear, EXP, and EXP2 fixed fog and can be
selected by both bounded internal ARB vertex programs, preserving positional
lighting and literal four-component texture-q semantics. Transport version 38 is
unchanged.

Transport version 39 carries the remaining D3D8 scaled-point state:
`POINTSIZE_MIN/MAX` and the nonnegative A/B/C distance coefficients. For point
lists, the miniport applies D3D's viewport-height factor before OpenGL's matching
distance attenuation and min/max clamp. Adjacent equal `D3DFVF_PSIZE` values are
still submitted as bounded independent runs. ARB vertex programs bypass fixed
point attenuation, so both internal programs explicitly derive and clamp
`result.pointsize`; the point-size output mode is enabled only for scaled point
lists. Packet validation accepts positive subunit world-space sizes only while
scaling is enabled and rejects nonfinite, negative, inverted, or out-of-cap state.

Transport version 40 adds the Boolean D3D8 `POINTSPRITEENABLE` state. The display
driver records it through ordinary render-state and state-block paths, copies it
into both draw packet forms, and both packet boundaries reject values outside zero
or one. The miniport enables point-sprite rasterization only for point lists and
sets coordinate replacement on all four texture units, so later fixed-function or
pixel-program stages receive the generated `(s,t,0,1)` coordinates independently.
Core coordinate-origin state maps the top-left D3D convention onto presentation
targets and the inverse framebuffer orientation used by ordinary render textures.
An effective sprite-state cache limits replay to enable/disable and relevant target
orientation transitions rather than every unrelated fixed-state change.

The signed `.185` package keeps transport version 40 and adds a second bounded
internal ARB vertex program for exactly two active point lights. Fixed OpenGL lacks
D3D's per-light hard `Range` cutoff, so the 76-instruction variant computes both
eye-space distances and attenuation factors independently, masks each contribution
at its own Range, accumulates material-sourced diffuse/ambient/emissive lighting,
and retains all four texture-matrix/projected-q rules. Its conservative selector
requires normalized normals, material sources, Gouraud shading, no fog or specular,
and a non-point primitive; combinations whose complete equation is not implemented
remain on the existing fixed path. Program definition and local Range parameters
are cached, reset with the transport session, and restored through the same bounded
FIFO rollback rules as the existing internal programs.

The signed `.186` package expands the dual-point variant to 93 instructions and
accepts the complete MATERIAL/COLOR1/COLOR2 choice for diffuse, ambient, and
emissive roles. Program-local source masks retain D3D's material fallback whenever
the chosen vertex color is absent, while disabling OpenGL color material prevents
it from corrupting the explicit program state. Another local mask selects either
the inverse-transpose normal as supplied or its normalized value, matching
`NORMALIZENORMALS` without duplicating the program. The cache key includes all
source and normalization controls; specular, fog, point primitives, and non-point
positional-light combinations remain conservatively excluded.

The signed `.187` package renames that internal path to the multi-point Range
program and expands it to 112 instructions. It accepts either two or three active
point lights; slot three has independent Range, attenuation, diffuse, and ambient
terms and is branchlessly disabled by a program-local mask for two-light draws.
The local-parameter cache now keys the active count and third Range in addition to
the existing source and normalization controls. Four-or-more-point, spotlight,
specular, fog, and point-primitive combinations remain on their conservative
paths until a complete bounded variant is validated.

The signed `.188` package expands the multi-point program to 119 instructions
and accepts both vertex and table fog. It calculates D3D's absolute-Z or radial
eye-space vertex-fog coordinate from the same modelview position used for
lighting, then selects the bounded draw transport's supplied post-divide depth
when table fog is active. The existing fog-mode setup applies linear, EXP, or
EXP2 equations after that coordinate selection. Multi-point specular, four-or-
more-point, spotlight, and point-primitive combinations remain conservatively
excluded.

The signed `.189` package adds a separate exactly-128-instruction dual-point
specular program. It keeps each point light's Range/attenuation mask attached to
that light's diffuse and half-vector specular factors, then accumulates the two
secondary-color terms without relying on fixed OpenGL's unbounded positional
lighting. The selector requires exactly two point lights, specular enabled, no
fog, Gouraud shading, normals, and supported material sources; it preserves
MATERIAL/COLOR1/COLOR2 fallback, local/infinite viewers, optional normal
normalization, and four texture transforms. Definition state and local controls
are cached and reset with the transport session. Three-or-more-point specular,
fog-plus-multi-point-specular, spotlight combinations, and point primitives stay
on conservative paths until complete variants are validated.

The signed `.190` package adds an exactly-128-instruction four-point Range
program. It computes independent distance, Range, attenuation, diffuse, and
ambient factors for four point lights and preserves optional normal
normalization, MATERIAL/COLOR1/COLOR2 diffuse/ambient/emissive sources, and four
texture transforms. Its selector requires exactly four point lights, Gouraud
shading, normals, no fog/specular, and a non-point primitive; other combinations
remain conservative. Program definition, four Range components, material-source
masks, and normalization state participate in the same session/reset cache rules.

The signed `.191` package adds a scalable vertex-only GLSL 1.10 backend without
advertising D3D programmable-vertex-shader caps or changing the guest/host ABI.
It uses the existing FIFO calls to create, upload, compile, link, query, and delete
one bounded program per transport session. The initial selector accepts five
through eight point lights with independent Range and attenuation,
MATERIAL/COLOR1/COLOR2 diffuse, ambient, and emissive sources, optional normal
normalization, and four texture transforms; fog, specular, spotlights, point
primitives, and user clip planes remain conservative. Compile/link status and all
eight uniform locations are mandatory, failures print a bounded info log and
delete partial objects, ARB/GLSL switches explicitly unbind the previous program,
and session teardown selects program zero before deleting the linked object. The
production shader source is deterministically extracted and tested by
`mingw32-make -C guest/xpdm glsl-compat-smoke` against the actual host renderer.

The signed `.196` package extends that selector to three through eight point
lights when specular is enabled. The program evaluates Direct3D's local/infinite
half-vector equation, material Power, independent per-light Range/attenuation,
and MATERIAL/COLOR1/COLOR2 specular sources, then emits a separate secondary
color through explicitly enabled `GL_COLOR_SUM`. Power zero bypasses GLSL's
undefined `pow(0,0)` while retaining the Direct3D unit exponent result. A ninth
uniform carries material specular RGB and Power without depending on fixed
material visibility. Fog, spotlights, point primitives, and user clip planes
remain conservative for this scalable path. The production source is 8,038
bytes (8,048 FIFO bytes including NUL, alignment, and the host-pointer slot), and
the host smoke includes production geometry for MATERIAL Power=0 and the
seven-light local-viewer profile.

The signed `.197` package accepts mixed point/spot configurations from two
through eight active positional lights. The shader evaluates Direct3D's full
inner cone, zeroed outer boundary, and normalized nonlinear Falloff interval,
then combines that factor with the existing independent Range and attenuation
before ambient, diffuse, and specular accumulation. The driver preserves each
light's RGB and uses ignored light-color alpha internally to carry Falloff and
the inner half-angle only while this scalable backend is selected. The public
8 KiB shader-source limit and transport ABI remain unchanged; the audited
internal GLSL ceiling is 16 KiB. The production source is 9,309 bytes (9,320
FIFO bytes including NUL, alignment, and the host-pointer slot), and the host
smoke adds simultaneous linear and squared spotlight Falloff pixels.

The signed `.198` package carries planar/radial vertex-fog and table-fog
selection through a tenth scalable-program uniform. The vertex shader emits
absolute eye Z, radial eye distance, or the existing table-fog input as required;
the fixed fragment path still applies Direct3D's fog mode and color. The selector
therefore accepts fog combined with the same two-through-eight positional-light,
spotlight, material-source, and specular state without changing caps or transport
ABI. The production source is 9,546 bytes (9,560 FIFO bytes including NUL,
alignment, and the host-pointer slot), and the host smoke includes production
spotlight geometry under vertex fog.

The signed `.199` package accepts the six already-advertised D3D user clip planes
on the scalable path. The existing object-state transport defines world-space
plane equations under the view matrix, then restores view times world; the GLSL
program's eye-space `gl_ClipVertex` supplies the matching evaluation coordinate.
No source, uniform, caps, or ABI change is required. The host smoke applies a
translated world matrix after defining `x >= 0` and requires one spotlight
primitive to be rejected while the other retains its exact nonlinear Falloff.

The miniport exposes each supported resolution as both X8R8G8B8 and R5G6B5. Mode
stride, framebuffer bitmap type, halftone format, DirectDraw display masks, and D3D
display/backbuffer format operations all follow the selected depth. D16, D24X8,
and D24S8 are advertised for arbitrary color depth. D24X8 uses a true 24-bit
depth-only host renderbuffer and does not acquire a stencil attachment. The full
D3D8 visual/Reset/wireframe gate runs in both 32-bit and quantization-tolerant
RGB565 modes.

Dynamic textures are advertised through both the Direct3D and DirectDraw caps.
Stock D3D8 and legacy D3D9 can therefore create lockable
`D3DUSAGE_DYNAMIC`, `D3DPOOL_DEFAULT` textures. Writable unlocks reuse the
existing owner-scoped generation tracking, so `D3DLOCK_DISCARD` invalidates the
uploaded copy and the next sampled draw uploads the replacement contents. The
driver keeps a direct linear guest mapping rather than promising storage
renaming or an unmeasured performance gain. Both runtimes have an exact
before/after-discard pixel oracle, and D3D9 releases the default-pool resource
before Reset. The complete contract is in `docs/d3d-dynamic-textures.md`.

Stock XP `d3d9.dll` is also supported for the driver's fixed-function feature
set and pixel-shader 1.3 path. `GetDriverInfo2` returns an
explicit 304-byte D3D9 caps structure, one
adapter group, no extended modes, no hardware query types, and explicit no-AGP
lifecycle handling. The driver intentionally declines DX9 DDI version 4 because
that contract requires the not-yet-implemented programmable shader command
surface; the runtime then uses its supported DX8-compatible XPDM path. The
acceptance gate proves managed texture upload, an untextured draw, a textured
triangle strip, a stock-runtime `ps_1_1` texture-sampling arithmetic draw,
fullscreen Present, cooperative-level polling, Reset, exact post-reset output,
and clean transport teardown through System32 `d3d9.dll`. The shader translator
accepts the ps_1_1 arithmetic family, preserves shader-local `def`
constants, and supports standard projected texture lookup, coordinate reads,
texture-coordinate kill, and two-stage `texbem` lookup on the first two
texture stages. `texbeml` also transports the destination stage's luminance
scale/offset state. `D3DFMT_V8U8` and `D3DFMT_L6V5U5` are exposed as legacy
bump-map formats and converted to a portable host upload representation before
shader-side signed Du/Dv and luminance reconstruction. A third advertised
texture stage and coordinate set support the ps_1_1
`texm3x2pad`/`texm3x2tex` matrix lookup. The DDI and transport expose four
texture stages and one- through four-component FVF coordinate sets for both
fixed-function combining and pixel-shader sampling. Mipmapped cube maps are
advertised and transported as six checked face chains; sampling is rejected
unless every face contains the same complete level set. The shader translator
accepts the complete ps_1_1
`texm3x3pad`/`texm3x3tex`/`texm3x3spec`/`texm3x3vspec` family, with exact real-XP
acceptance covering a `texm3x3tex` positive-X cube lookup. The
translator also supports `texreg2ar` and
`texreg2gb` dependent reads using the source texture's alpha/red or green/blue
channels as the next stage's coordinates. Pixel-shader 1.2 adds `dp4`, `cmp`,
`texdp3`, `texdp3tex`, `texreg2rgb`, and the non-sampling `texm3x3` form;
pixel-shader 1.3 adds `texm3x2depth`. Version-specific instruction counts and
the ps_1_2/1_3 `dp4`/`cmp` limits are enforced. Generic texture reads are
compiled into the bounded combinations of 2D and cube targets used by the
shader, then resolved against the actual bound surfaces for each draw. Exact
real-XP acceptance covers a 2D-to-cube dependent read with `dp4`/`cmp` and a
depth-overriding `texm3x2depth` draw. Offscreen 16- and 32-bit color targets are
attached to a reusable host framebuffer, with persistent D16/D24X8/D24S8
renderbuffers keyed to the XPDM depth surfaces. A successful framebuffer-completeness result is
cached for each color-texture/depth-surface pairing; deleting a depth renderbuffer
invalidates every matching entry so a reused guest surface name is checked again.
The target-specific projection stores D3D's
top row in texture row zero, so the result can be sampled without an application-
visible vertical flip. Exact acceptance samples both the depth-tested triangle
and the untouched clear region before releasing every default-pool reference
and exercising Reset.

The first Shader Model 2 foundation is deliberately dormant. The production
`qxpdisp` build includes `qxp_shader_decode.c`, which decodes exact `ps_2_0` and
`vs_2_0` streams into a caller-owned, bounded, non-wire typed IR. It resolves
structured control and labels, records a summarized intraprocedural CFG,
validates register-component/subroutine/iteration dataflow, and records future
PS temporary/capability/instruction-slot requirements. It has no DDI caller and
does not alter caps, transport ABI 53, or the legacy SM1 paths. The standalone
`qxp_shader_decode_test` provides specification vectors, every-prefix
truncation, capability metadata, call/loop/dataflow, canary, determinism, and
bounded mutation gates before any runtime integration.

The adjacent dormant `qxp_vdecl9.c` foundation parses the counted
`D3DVERTEXELEMENT9` records carried by private XPDM opcode 71. Unlike the
application API array, that DDI payload has no `D3DDECL_END`. The parser
validates exact element layout, stream and semantic rules, POSITIONT,
share-offset requirements, and all tessellator/displacement methods, while
recording future declaration-type, device-capability, stride, programmable-
pipeline, and VS-version requirements.

`qxp_d3d9_shader_objects.c` adds the dormant owner-scoped lifecycle around that
parser and the B0 decoder for private DP2 opcodes 71-76. Separate 256-entry
declaration/function tables deep-copy parsed declarations and exact `vs_2_0`
bytecode, retain compact decoder summaries, and bind by nonwrapping generation.
Preparation/allocation is outside `qxp_d3d_table_lock`; publication, lookup,
binding, and detach are serialized under it; rollback and frees happen after
unlock. Declaration bit zero distinguishes private objects from preserved even
legacy FVF values. Zero clears, failed lookup preserves prior state, generation
exhaustion is permanent, and exact-owner teardown frees each object once. The
stock runtime cannot reach these private handlers while DDI v4 is declined, and
draws do not consume their bindings.

`qxp_d3d9_shader_link.c` is the adjacent dormant semantic boundary. It maps
exact `vs_2_0` DCL usage/index pairs to declaration elements and destination
input registers without retaining either input. Extra elements and their
streams are ignored, missing VS2 semantics are recorded as undefined, and
POSITIONT0 produces an explicit vertex-processing bypass. The fixed result
retains all stream/type/offset/method data needed by a later safe gather. Before
linking, retained declarations are reconstructed and passed through the exact
parser again, and typed DCL semantics must match their raw tokens. This remains
an uncalled draw-preparation result rather than state stored in either
independently bound object. Safe undefined-input values, stream gather/frequency
state, transport/host execution, caps, and public DDI exposure remain deferred.

`qxp_pshader_glsl.c` adds the first direct dormant GLSL lowering stage. It
accepts only exact B0 `ps_2_0` typed IR and emits bounded GLSL 1.20 into a
caller buffer with fixed DEF, constant, input, sampler, output, discard, and
capability metadata. It allocates nothing and retains no pointers. A separate
preflight reconstructs exact opcode framing, controls, slots, read ports,
declarations, matrix rows, component initialization, texture dependencies, and
future PS caps before emission. The portable oracle covers every admitted
pixel opcode and hostile typed-IR mutations; `ps20-glsl-smoke` compiles, links,
and executes the generated all-form shader on the real compatibility host.
This code is compiled but uncalled; it changes no DDI, caps, transport, or
runtime behavior.

`qxp_vshader20_glsl.c` completes the dormant direct SM2 lowering pair. It
accepts exact B0 `vs_2_0` typed IR and emits bounded GLSL 1.20 with fixed
usage/index, output, raw DEF/DEFI/DEFB, resource, and flow metadata. Its
independent preflight reconstructs opcode storage, DCL integrity, read ports,
relative matrix rows, control/call/`aL` scope, CFG, definite assignment,
iteration stability, outputs, and slots. Pair-provided centroid bits generate
interfaces matching the pixel lowerer. The epilogue preserves original D3D
clip position, target orientation and depth, programmable/table/fallback fog,
and point size. Portable mutation tests plus real-host flow, special-value,
arithmetic/geometric/matrix, clip/depth/fog/point, and generated-pair oracles
pass. The source is compiled but has no caller and changes no DDI, caps,
transport, or runtime behavior.

`qxp_d3d9_draw_state.c` adds the dormant B4 state foundation: fixed generation-
tagged declaration/vertex-function bindings, exact private declaration versus
legacy FVF transitions, sixteen streams with source/offset/stride/user-memory
and raw frequency, and all SM2 VS/PS float, integer, and Boolean constant banks.
Its DP2 oracle frames exact WDK operations 48/49/50/57/77/80/83/93/94/95,
publishes fixed batches only after complete candidate decoding, preserves only
complete earlier variable records, reports outer command offsets, and retains
no input pointer. Portable tests cover every bank boundary, native D3D9
frequency acceptance, all truncation prefixes, rollback, input alias/lifetime,
unbind, masks, canaries, and determinism. It is compiled but uncalled.

The B5 extension to `qxp_d3d9_shader_objects.c` adds a separate exact-owner
`ps_2_0` function namespace sharing the permanent declaration/VS generation
clock. Prepare exact-decodes B0 and runs B2 count-only outside the table lock,
then retains compact metadata and deep bytecode in caller-allocated trailing
storage. Publication/binding/detachment, exact-generation snapshots, batches,
and teardown preserve the existing lock and post-unlock-free contract. Tests
cover owner isolation, cross-stage handles, ABA/exhaustion, all frame prefixes,
allocation rollback, 29 metadata corruptions, batch offsets, and canaries.
At the B5 boundary standard pixel opcodes 54/55/56 remained uncalled because
they collide with D3D8 PS1.x.

B6A makes those shared opcodes live through
`QxpD3d9DispatchPixelFunctionBatch`. CREATE is framed per record and selects B5
only for exact `ps_2_0`; every other profile retains the existing PS1.x
translator. Final publication, SET, and DELETE resolve one exact-owner handle
across both tables under `qxp_d3d_table_lock`. One combined context state
switches the raw PS1 handle and generation-tagged PS2 binding atomically.
Legacy cache/refcount deletion remains intact, PS2 storage and owner teardown
free after unlock, and bound PS2 draws fail before fixed/PS1 resolution until
execution exists. The exact production dispatcher and combined state have
portable mixed-profile, truncation, prefix, resource, outer-offset, collision,
owner, stale-state, and set/clear/delete coverage. A signed seven-mode XP T2
passes the retained PS1 path and handback. DDI v4, caps, transport, ABI 53, and
DriverVer `.305` remain unchanged.

B6B embeds one initialized B4 ledger in each production context and removes the
loose D3D9 declaration/FVF/VS2 fields. Private lifecycle batches update the
ledger and synchronize mode after success or prefix failure under existing DP2
serialization. Active or malformed FVF/declaration/VS2/PS2 state rejects before
legacy host submission. The exact bridge has portable lifecycle, prefix, mode,
corrupt-state, and canary coverage plus a fresh signed seven-mode XP T2.
Constants, streams/frequency, linked snapshots, execution, DDI/caps, transport,
`.305`, and ABI 53 remain unchanged.

B6C routes only the non-colliding D3D9 opcodes 77/80/83/93/94/95 into that
ledger: integer/Boolean constants, SOURCE2, and stream frequency. The exact
portable production dispatcher snapshots and allocates outside the table lock,
parses outside it, and publishes affected fields under it. It preserves
constant prefixes, fixed atomicity, outer offsets, lock balance, and rejects
D3D8 state-block recording. SOURCE2 provenance and nonunit frequency fail
closed; a saturating source generation rejects both bound and first-time bind
races against same-owner surface teardown. R2 frozen review and fresh signed
seven-mode T2 pass. Shared float constants/streams, indices, linking, execution,
DDI/caps, transport, `.305`, and ABI 53 remain unchanged.

B6D1 moves shared float opcodes 48/57 to an exact portable production parser.
It atomically publishes B4 with D3D8 VS c0-c95 and PS c0-c7 while retaining
B4-only D3D9 VS c0-c255 and PS c0-c31. Prefix, truncation, outer-offset,
state-block, callback-OOM, declaration-reload, execute ordering, aliasing, lock,
and canary contracts are covered. Frozen review and fresh signed seven-mode T2
pass. Streams/indices, linked execution, DDI/caps, transport, `.305`, and ABI 53
remain unchanged.

B6D2 moves shared stream/source-UM/index opcodes 49/50/51 to one exact portable
production dispatcher. It validates fixed batches into bounded candidates,
then atomically publishes B4 and D3D8 mirrors under a common saturating
resource generation shared with SOURCE2 and teardown. Classic sources clear
SOURCE2 provenance but preserve frequency; UM is stream zero; bound indices use
2/4-byte stride and unbind normalizes to zero. Atomic rollback, aliasing,
recording mode, first-bind teardown races, exhaustion, lock balance, outer
errors, and canaries are covered. Frozen review and fresh signed T2 pass.
Linked execution, DDI/caps, transport, `.305`, and ABI 53 remain unchanged.

B6E adds allocator-owned pointer-free `qxp_d3d9_draw_prepare.[ch]`. It snapshots
exact-owner declaration/VS2/PS2 objects and raw ledger state under lock, then
decodes, semantically links, and count-only lowers outside it. The immutable
result carries safe zero undefined inputs, linked/missing VS-to-PS interface
data, stream/UM requirements, POSITIONT bypass, PS sampler metadata, and raw
plus definition-overlaid effective constants. Public validation reconstructs a
canonical snapshot and rejects any stale generation or hostile field edit.
Every large allocation/free remains outside locks. This module is linked but
has no directdraw caller, resource backing lookup, execution, DDI/cap,
transport/host, DriverVer, or ABI change.

B6F adds `qxp_d3d9_draw_gate.[ch]` and calls its exact portable command seam
before all six live fixed draw handlers. Complete private SM2 state is prepared
and freed, then still rejected before transport. Truncated/semantic state maps
to the outer command plus invalid parameters; allocation failure maps to
currently unavailable. Zero-count and legacy/PS1.x paths do not allocate or
lock. Portable tests cover all six opcodes, WDK-asserted record sizes, nondraw,
short/no-op, namespace/race/rollback, and transport/frame/batch exclusion. No
resource-backing resolution, execution, caps, DDI, transport/host, DriverVer,
or ABI change is added.

B6G1 adds pointer-free `qxp_d3d9_draw_backing.[ch]` inspection at that gate.
Required streams/index state must resolve to the exact shader+DirectDraw owner,
permanent nonwrapping surface incarnation, content generation, compatible
buffer role, and a hard finite system/video extent covering the declaration
base span. SOURCE2 and observed stream-0 UM are overflow checked without
retaining pointers. A production-equivalent DestroyDDLocal seam clears exact-
owner surface entries and legacy+B4 bindings under one lock, while deduplicated
host deletion occurs after unlock; owner/handle reuse gets a fresh incarnation.
This is base-span authority only: draw records, actual index accesses,
pinning/copy, wrapping, execution, caps/DDI, transport/host, DriverVer, and ABI
remain unchanged.

B6G2 adds pointer-free `qxp_d3d9_draw_envelope.[ch]`. It WDK-asserts and
independently frames all six fixed draw layouts, checks every primitive source
count, normalizes logical/byte starts, bounds SOURCE2+stride+span for each
required stream, and conservatively validates declared indexed windows plus
index byte ranges. Nonunit frequency rejects. Production record pointers exist
only during the synchronous inspector and are cleared before return. This does
not scan actual indices, pin/copy resources, support wrapping/instancing,
execute, expose caps/DDI, or change transport/host/DriverVer/ABI.

Dormant B6G3A adds `qxp_d3d9_draw_access.[ch]`. Given an exact caller-owned
immutable copy of B6G2's index interval, it byte-decodes every 16/32-bit index,
enforces each record's broad MinIndex/NumVertices promise and signed base, and
derives tight aggregate SOURCE2+stride+span stream ranges. Nonindexed ranges
carry through unchanged. Canonical reconstruction covers all pointer-free
output. It has no live caller and establishes no copy provenance, pinning,
generation revalidation, execution, cap/DDI, transport/host, or ABI support.

B6G3B1 adds `qxp_d3d9_draw_capture.[ch]`, the first live caller. Exported
`QxpD3d9DrawCaptureIsEligible` admits only nonindexed draws whose prepared and
backing required, required-user-memory, and populated stream masks are exactly
stream 0 over a `USER_MEMORY_CALL` backing; indexed and surface-backed draws
stop at the unchanged B6G2 boundary. Eligible draws are copied into one
allocator-owned pointer-free self-describing blob (≤ 1 MiB, 16-byte aligned
payload, FNV-style hash) holding the exact records and stream-0 vertex bytes.
Both ranges are arithmetic-checked before anything is allocated, then secured
with `EngSecureMem`, copied via `qxp_dd_copy`, and unsecured in reverse; B6G3A
validates the blob-owned records and the draw is still rejected. Appended
statuses 20/21/22 do not renumber existing codes, and only allocation failure
(21) maps to `QXP_DDERR_CURRENTLYNOTAVAIL` — a NULL `EngSecureMem` result is a
semantic rejection, because XP cannot distinguish an inaccessible range from
resource exhaustion. A wrapping record range is rejected as appended
`RECORD_RANGE` before any record byte is read, while zero/unrepresentable counts
keep `RECORD_COUNT`. The envelope arithmetic and activation rule are shared
helpers called by `directdraw.c`, so tests exercise one implementation. This
establishes no surface provenance, pinning, generation revalidation, execution,
cap/DDI, transport/host, DriverVer, or ABI support, and the stock runtime cannot
reach it while DDI v4 is declined.

Dormant `qxp_d3d9_draw_lifetime.[ch]` adds the gate that future surface-backed
capture requires, with no live caller and no `directdraw.c` change. It is a
pointer-free, allocation-free scalar state machine over caller-injected lock and
yield callbacks. Capture readers are admitted or refused under one lock
acquisition and never wait, so the DP2 path keeps its budget. Mutations publish
pending state first, then spin-drain outstanding readers with the lock released
around every yield, giving teardown priority over a stream of captures. This
deliberately extends the existing bounded `draw_batch_lock` spin discipline
rather than introducing `EngWaitForSingleObject`, because PASSIVE_LEVEL
waitability is not documented for every DirectDraw callback. Epochs are
nonwrapping and nonzero; exhaustion closes the gate permanently so reader
revalidation can never be fooled by a reused epoch.

`make test` also cross-builds and runs fast host-side integer-float, hardware-T&L,
packet, vertex, and shader translator gates. A source-level ARBvp check rejects
unsupported opcodes and programs above the portable 128-instruction limit before
XP is booted; the table-fog positional-light program deliberately occupies all 128
instructions. The shader gate covers the core arithmetic family, `def`, ordinary
sampling through stage three, `texbem`, `texbeml`, all three register-dependent
reads, the `texdp3` pair, `texm3x2`, the completed `texm3x3` forms, 2D/cube
variants, ps_1_2
arithmetic, ps_1_3 depth output, and malformed or incomplete bytecode rejection
without booting XP.

`mingw32-make glsl-compat-smoke` is a separate Windows-host architectural gate
for the scalable fixed-function HWTL backend. It creates a real legacy OpenGL
context and uses deterministic pixel readback to prove that a vertex-only GLSL
1.10 program feeds both fixed-function color/texture processing and the existing
ARB fragment-program stage, then checks production Range and scalable specular
geometry. It is opt-in because it requires a live host OpenGL display context;
the protocol and integration decision is recorded in
`docs/hwtl-glsl-backend.md`.

`ddk_probe/` is deliberately not an installable display driver. It is a minimal
Microsoft-WDK `GDI_DRIVER` target whose `DrvEnableDriver` table contains only
`INDEX_DrvEscape`. Building it in the XP x86 environment proves the shared
handler and DDI wrapper compile/link with the authentic `winddi.h` and
`win32k.lib` ABI before that entry is added to the full framebuffer driver.
Invoke `build-ddk.cmd <WDK-root>` from a normal command prompt. It selects the
official free Windows XP x86 environment, performs a clean build of the exact
shipping escape wrapper, checks that the result is an import-free i386 PE32
native driver targeting subsystem 5.1, and runs Microsoft ChkINF. ChkINF's own
exit status does not reflect reported diagnostics, so `validate_chkinf.py`
parses its summary and makes any error or warning fail the command.

`DrvEscape` additionally serves escape `0x1102`, the Shader Model 2 draw-source
self-test, implemented by `QxpD3d9SourceSelfTest` in `directdraw.c`. It exists
because the SM2 draw path is unreachable while DDI v4 and SM2 capabilities are
declined, which left that code unexecuted in every acceptance run. Given a byte
count and an expected FNV-1a checksum, it runs the real guarded transaction over
the calling process's registered system-memory vertex buffers and reports how
many matched, along with the gate reader count.

It is a strict observer: no draw, no host submission, no transport traffic, no
allocation, no state mutation, and no capability change. It collects candidate
handles under `qxp_d3d_table_lock`, releases the lock, and only then runs each
transaction, because that lock is not recursive and the source module acquires
it internally through injected callbacks.
