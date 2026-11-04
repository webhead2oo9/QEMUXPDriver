# Real-XP OpenGL ownership acceptance tool

`glowner.exe <tag>` creates a 640×480 double-buffered OpenGL window, logs each
WGL milestone to `C:\XPDRIVER-GL-<tag>.LOG`, and continuously swaps a changing
clear color. Put it beside the matching kit-v12 `opengl32.dll`.

Acceptance sequence:

1. Start `glowner.exe first`; require `RENDER_BEGIN` and changing captured
   frames. After five seconds it launches `glowner.exe second` itself, avoiding
   any dependency on desktop focus or test-harness keystrokes.
2. While it renders, start `glowner.exe second` from the same directory.
3. Require `second` to log `SET_PIXEL_FORMAT_FAIL` and exit without creating a
   context; require `first` to continue producing changing frames.
4. Reverse launch order: start `glowner.exe revfirst`. It loads the wrapper
   first (its DllMain claims the shared ring at process start), immediately
   spawns `glowner.exe second` — which ring-sniff-shares, binds first, and
   must own the session — then attempts its own bind eight seconds later.
   Require `revfirst` to log `REVERSE_REFUSED_OK` and exit 0; a logged
   `REVERSE_MISGRANT` (exit 99) is the reverse-order arbitration bug. The
   host trace must show exactly one bound session, a
   `refused SetPixelFormat ... refcnt 2` entry, and the owner's FPS trace
   continuing after the refusal. Closed on real XP 2026-07-14 against
   qemu-libretro `9188331372`: owner at 32 FPS on the host renderer,
   refusal at the shared `0097e000` VA, clean teardown.

This specifically exercises the legacy wrapper's shared-ring DllMain path, where
both XP processes commonly map `ptm` at the same virtual address.

The same ISO contains `qxpwgltest.exe`. The host harness's `-Wgl` mode runs it
through stock XP `opengl32.dll`; the test creates a temporary QXPICD registry entry
and copies the ICD into system32 inside the `-snapshot` overlay. The established
IAT shim supplies only `OPENGL_GETINFO`, standing in for the future XPDM display
escape. Microsoft handles every later loader/WGL/GL operation. The accelerated
parent must report the host renderer, a concurrent child must select a generic PFD
and report `GDI Generic`, and the parent must continue rendering afterward.

`qxpgametest.exe` is the first real-game compatibility probe. It launches the
Quake II demo from `C:\WINDOWS\desktop\Quake2 Demo\Install\Data`, where no
private `opengl32.dll` can shadow the installed system ICD. It enables Quake's
console log, waits for `GL_VENDOR`, `GL_RENDERER`, and `GL_VERSION`, rejects
`GDI Generic`, loads the `demo1` 3D map, keeps it alive for ten seconds, reports
the renderer over the acceptance result channel, and then terminates the game.
XPDM profiles can route that channel through QEMU COM1 with no emulated network
adapter; older profiles retain UDP as a fallback. Run
it through the host harness with `-Xpdm -BootOnly
-Game` against an already-installed persistent XPDM overlay.

`qxpd3dprobe.exe` validates the native, install-once Direct3D path. It rejects a
non-System32 `d3d8.dll`, queries the stock XP Direct3D 8 runtime for the HAL
adapter/caps and supported formats, creates a HAL device, clears and presents a
frame, and reports the exact failing stage/HRESULT. Its packed-format scene includes
exact P8/A8P8 palette behavior and A8R3G3B2/A4L4 alpha/color expansion in both
X8R8G8B8 and RGB565 display modes. A separate object-space frame is the HWTL
oracle: while the capability remains disabled, XP's software vertex processor
produces 86 exact samples spanning matrices, lighting/material/fog, point sizing,
clip planes, texture transforms, and point sprites. The point-sprite subset covers
fixed stages zero and one, disabled explicit coordinates, a stock-runtime `ps_1_1`
lookup, and a 64x64 offscreen render-target replay with asymmetric orientation.
Two additional samples exercise all eight advertised directional-light slots with
sparse logical indices, additive RGB accumulation, disable, and slot compaction.
Once HWTL is advertised, that unchanged frame becomes the accelerated-path parity
gate. It launches the companion
`qxpddquery.exe` to report the low-level DirectDraw HAL tables and relevant
registry overrides before using the public Direct3D API. Run it with `-Xpdm
-BootOnly -D3d` against the persistent XPDM overlay.

`qxpd3dvsprobe.exe` is the focused stock-System32 D3D8 vertex-shader 1.1 gate.
It requires the advertised 1.1/96-constant caps, exact clipped and no-clip host
frames, declaration-constant Apply/Capture state blocks, Reset close/reopen, and
malformed-token rejection. Its lifetime phase applies a state block only while
the referenced shader is live, deletes the block before the shader, then creates,
binds, verifies, draws with, and deletes a distinct shader. D3D8 state blocks are
not treated as owners that make deleted public shader handles valid. Run it with
`-Xpdm -BootOnly -D3dVs11`; the host gate requires six vertex and six clip
definitions/deletions, six pre-Reset plus one post-Reset draw/present, exact
pixels, and clean teardown.

`qxpd3dvsbench.exe` is the capture-free stock-System32 D3D8 programmable-path
measurement fixture. It creates four fresh hardware-VP devices in
fixed/shader/shader/fixed (ABBA) order, using the same triangle and raster state
for a pretransformed `XYZRHW` control and a public `vs_1_1` pass-through shader.
Each phase submits one warm-up frame and sixteen measured frames with sixteen
`DrawPrimitiveUP` calls per frame. It reports device creation, shader creation,
API submission nanoseconds per draw, and CPU-visible `Present` microseconds per
frame. Run it only after the exact `qxpd3dvsprobe.exe` correctness gate with
`-Xpdm -BootOnly -D3dVsBenchmark`. The host gate requires four clean context and
transport lifecycles, two paired vertex/clip program lifecycles, four exact
profile blocks, and the stock runtime's observed coalescing of 272 API calls
into seventeen transport draws per phase (one warm-up plus sixteen measured
frames). It does not dump frames, prove GPU completion, or turn its timings into
game FPS.

`qxpd3dvssuccession.exe` extends that gate across real process teardown. It
launches `qxpd3dvsprobe.exe` twice in sequence with distinct child-result
markers, waiting for the first stock-D3D8 process to exit before starting the
second. The wrapper deliberately leaves COM1 to the active child and reports
only the final aggregate result after both child handles close. Run it with
`-Xpdm -BootOnly -D3dVsSuccession`; the host requires both child results, four
context and host-session opens across the two Reset cycles, doubled shader and
clip define/delete counts, exactly four `6,1,6,1` draw/present sessions, exact
second-child clipped and fast-path pixels, and clean teardown.

`qxpd3dvsmultictx.exe` is the simultaneous owner-isolation gate. One process
keeps two windowed hardware-VP D3D8 devices alive together; stock XP assigns
both devices the same public vertex-shader handle. The probe alternates exact
pink/blue/orange frames between the two contexts, destroys device A, then
requires device B to retain its shader, opposite constant, and exact green
survivor frame. Run it with `-Xpdm -BootOnly -D3dVsMultiContext`. The host gate
requires exactly two contexts sharing one reference-counted transport, two
vertex/clip program pairs, four draws/presents, ordered exact frames, paired
host deletion, no DP2 failure, and clean teardown. Stock D3D8 may defer each
final-per-device public delete to context destruction, so host deletion plus the
survivor frame—not an optional explicit DP2 delete marker—is authoritative.

`qxpd3dgame.exe` is the stock-runtime Direct3D real-game probe. It inventories
the installed Max Payne demo, rejects game-local Direct3D/WineD3D/OpenGL
wrappers, launches `MaxPayneDemo.exe`, proves the process loaded only
`C:\WINDOWS\system32\d3d8.dll`, activates the launcher, and leaves the game
running for a bounded observation interval. Run it with `-Xpdm -BootOnly
-D3dGame`; the host additionally requires an XPDM D3D context, a host session,
draw traffic without transport failures, accelerated presents, and a captured
non-desktop/non-black game frame. The retained proof is
`qemu\winxp\xpdm-maxpayne.bmp`; a visual failure instead retains
`xpdm-maxpayne-failed.bmp`.

`qxpgameobserve.exe` is the installed-game diagnostic companion. It
recursively locates the selected demo, reports visible windows, selected
configuration/log lines, and loaded `d3d8`/`d3d9`/`ddraw`/`opengl32` modules,
and follows a matching child process when a launcher hands off. The
`ut2003visual` mode is the fail-closed normal-menu oracle: on the disposable
child it removes only the prior `UT2003.log`, launches `UT2003.exe` with no map
argument, default audio, and no renderer command-line override, and requires a
foreground window owned by the launched process after canonical System32
`d3d8.dll` is loaded. After a 30-second warmup the guest emits exactly one
`foreground-d3d8-window-stable` boundary, then requires a further 180 seconds
independently of launch elapsed time. Startup allows 300 seconds plus one
bounded polling-slack second. The normal-menu contract deliberately does not
depend on `Index.ut2`: the official Demo 2206 uses it as a placeholder, and a
successful launch need not emit a live-log transition for that nonexistent
map.

The host uses synchronous producer readback and quarantines two callbacks at
the visual-ready marker—one possibly inside the inline BMP writer and one in
the frontend pending slot—then accepts only later frames. Eight manifest-bound
samples must span at least 150 seconds with no gap above 45 seconds, at least
four canonical decoded-RGB hashes, and at least three changes of 0.05% or more
under the RGB-distance gate. The independent menu classifier decodes each
sample and requires the 640x480 title-logo, highlight, and four menu-row
signatures in at least six of eight frames. It rejects desktop, logo-only,
opaque/blank, wrong-geometry, frozen, hash-only, and process-lifetime evidence
before the final verifier can return `PASS`.
The automated cycle pass remains separate from the release-review gate. A
schema-1 direct review must bind all sample/canonical hashes and the video at
the post-visual-ready boundary, then confirm every sample and the video show
the intact, legible menu without logo-only or opaque-glyph frames.

`ut2003gameplay` is the separate joined-match qualification path. It removes
the prior title log, launches
`DM-Antalus?game=XGame.xDeathMatch` with audio disabled, and requires the fresh
ordered `Browse DM-Antalus` -> `LoadMap DM-Antalus` -> `xDeathMatch` game class
-> `DM-Antalus.myLevel` -> `Bringing Level ... up for play` sequence. It also
requires canonical System32 `d3d8.dll` and three accepted foreground
`SendInput` fire attempts; a missing map stage, failed input injection, wrapper
DLL, or short observation fails in the guest. The cycle-level
`UT2003Gameplay` mode requires a same-cycle exact-package install, anchors
capture at the first fire attempt, retains eight post-input samples and a
35-second review video, and revalidates their manifest hashes, decoded-pixel
diversity, visible transitions, input records, and terminal duration.
`ut2003gameplayswvp` applies the same guest semantic/input checks after the
software-VP INI override but remains a diagnostic comparison. Joined gameplay
does not substitute for the independent normal-menu oracle.
`ut2003gameplayperformance` uses the same direct-map, fresh-log, System32
D3D8, semantic, and input gates without capture. After the live-level semantic
and first accepted foreground fire input, it brackets a 45-second guest sample
with measurement markers for the host publication and CPU profilers.

`warcraft3` retains the established forced-OpenGL diagnostic.
`warcraft3d3d8` uses the game's ordinary default launch with no renderer
switch, requires canonical System32 `d3d8.dll`, warms for 15 seconds, and
brackets a further 25-second black-cadence oracle while keeping the game alive
through the host's frozen capture boundary. A handoff before qualification
restarts that warmup; a handoff after oracle start fails rather than inheriting
the old process's module/window/timer proof. `warcraft3d3d8performance` keeps
the same default/System32-D3D8 path but replaces the cadence capture with a
45-second measurement window. Both forms close the game normally
so teardown counters are attributable before the guest result. The visual host
combines oracle messages with its bounded frame sequence; the performance host
profiles only the measurement-marker interval.

The current development checkpoint also transports immediate and buffered
primitives, fixed-function texture and raster state, depth/stencil, fog, and
accelerated fullscreen presentation. The synthetic probe verifies exact pixels.
Its patterned-A8 contract covers fixed-function decode and blending, PS 1.1
alpha sampling, and a `BeginStateBlock`/`EndStateBlock`/`CaptureStateBlock`/
`ApplyStateBlock` isolation pass: recording `ONE`/`ZERO` blending and a null
pixel shader must not alter the live `SRCALPHA`/`INVSRCALPHA` blend or PS 1.1
binding that Capture observes. The broader state-block contract covers
record/apply/capture/delete for render, texture-stage, transform, material,
light, light-enable, clip-plane, vertex/pixel shader, and float-constant state,
Reset recovery, plus runtime-owned PIXEL/VERTEX/ALL predefined membership and
excluded-state behavior. Destructive state-block membership checks run after
the exact visual and Reset gates so restored bindings cannot contaminate
unrelated image oracles. The capacity phase keeps 257 uniquely handled blocks
live together and applies the first, 65th, and 257th after all allocations, so
neither the retired 64-object quota nor a disguised 256-object ceiling can
regress.
The probe also verifies resource release and VM
teardown. Its `--visual` option holds each
distinct presented reference for 2.5 seconds so low-rate synchronous host capture
cannot skip between adjacent frames; the bounded worker watchdog accounts for
those intentional holds, while ordinary smoke tests retain the short hold.
The Max Payne gate adds real stock-D3D8 discovery and rendering evidence. The
probe's `--clean-maxpayne`
mode removes old per-game WineD3D/qemu-3dfx DLLs from the disposable demo
directory so the game can only exercise the installed display driver and stock
XP DirectX.

`qxpd3d9probe.exe` serves two stock-System32 contracts. Under an ordinary
DDI-v3 package it verifies VS1.1/PS1.3 fallback behavior and explicitly skips
PS2/SM2 because they are not advertised. Under the separately signed
DDI-v4 diagnostic profile it requires coherent VS2/PS2 caps and eight streams,
verifies VS1.1 backward compatibility, then executes exact-pixel direct,
indexed managed, dynamic default-pool, and managed textured VS2/PS2 draws. Its
texture phase samples s4 and s15 independently, then creates one `ps_2_0`
shader that declares all sixteen sampler registers, binds sixteen independent
managed 2D textures, samples every s0-s15 source in one draw, and requires
exact `mask=ffff` / center=`ffff2040` output.

`qxpd3d9selftest.exe` remains the out-of-band draw-source authority test. It
creates a `D3DPOOL_SYSTEMMEM` vertex buffer, fills it with a deterministic
pattern, and calls `ExtEscape` with escape `0x1102`, supplying the byte count and
FNV-1a checksum it expects. The driver admits a lifetime-gate reader, resolves
the base under the table lock, secures and copies the exact range, hashes it,
revalidates, and releases it over every registered system-memory vertex buffer
owned by the process. The probe requires `matches >= 1` and `readers == 0`; it
executes no draw and submits nothing to the host.

The probe's SM2 texture escape contract now ends at result version 10, terminal
stage 175 (`QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE`). Beyond the established
single/multi/projective/cube/depth/scissor/stream-offset, texldb, and
four-sampler phases, version 10 appends a diagnostic-only genuine 4x4x4
`sampler3D` fetch: a `dcl_volume s0` / `texld` program samples an A8R8G8B8
volume whose texels encode (x,y,z), so two draw pixels at the same (u,v) but
different interpolated depths must read two different Z slices. The guest gate
requires exact pixels (4,7)=`0xff23500a` and (10,7)=`0xff835a2a`, unequal
expected values, complete program/texture deletion and render-target restore
gating, and a bounded `sm2_volume` detail record naming the result size and all
22 appended `volume_*` fields. This remains a driver-internal volume diagnostic.
Plain VS2/PS2 are advertised only by the diagnostic profile, and no volume
format or volume-texture capability is advertised by either profile; this
escape does not imply stock-runtime volume support.
The historical exact-package matrix
at `artifacts/runs/20260728-volume-v10-texgen-material-final-t2-r1/` passes the
version-10 phase and complete T2 set. The same package hashes pass Max Payne at
`20260728-volume-v10-material-source-maxpayne-t3-r1`, historical UT2003
direct-map/pre-join visual evidence at
`20260728-volume-v10-canonical-visual-summary-ut-t3-r1`, and the
256-frame all-visible Warcraft III D3D8 oracle at
`20260728-volume-v10-canonical-visual-summary-warcraft-t3-r1`.
