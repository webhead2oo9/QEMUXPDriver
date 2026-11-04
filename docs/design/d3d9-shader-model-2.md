# Direct3D 9 / Shader Model 2.0 roadmap

## Status

### 2026-07-31 diagnostic closure checkpoint (ABI 61)

DriverVer remains `1.0.0.305`; transport ABI 61 carries the complete
title-agnostic SM2 implementation, but ordinary packages do not advertise it.
With all diagnostic gates unset, the driver declines DDI v4, emits
`QXPDISP GD9 FALLBACK`, and keeps its native caps9 block at VS1.1/PS1.3,
`DevCaps2 = 0`, and `MaxStreams = 1`. Stock DDI-v3 derives 16 streams from
caps8; native DDI-v4, VS2/PS2, and the eight-stream profile remain
diagnostic-only.

ABI 61 replaces the fixed four-entry programmable sampler prefix with a
dedicated sixteen-entry state/binding array. Display capture classifies all
`D3DSAMP_*` states independently of fixed-function texture-stage state,
snapshots exact owner/resource/generation/incarnation identity for every
shader-declared sampler, and emits a fixed-size pointer-free request. The
miniport validates the complete mask, neutral reserved fields, resource type,
identity, dimensions, mip completeness, and NPOT/address/filter restrictions
before any host side effect. The host binds or explicitly unbinds s0-s15, sets
each admitted parameter, and restores GL active texture zero.

The closure also removes two hidden count-vs-mask assumptions. Pixel-shader
translation derives constant capacity from constant declarations rather than
the sampler bit mask, while failed sampler declarations roll back decoder
state. Program-definition validation accepts any valid subset of the sixteen
sampler bits instead of imposing a four-set-bit ceiling. Focused decoder,
translator, packet, shader-object, sampler, and draw-preparation tests defend
those boundaries.

The ordinary frozen package at
`artifacts/runs/20260731-abi61-d3d8-specular-fix-r140/D3d9/` passes the
fallback contract with `diagnostic=false`, `ddiFallback=True`, and
`ddi4=False`. It reports VS1.1/PS1.3, skips PS2 and the SM2 contract, and
retains the fixed-function/SM1.x 21/21 visual, Reset, and teardown. The same
frozen package passes the corrected 800x600 D3D8 visual gate and the complete
D3D8 interface matrix without changing any advertised capability.

The separately signed package at
`artifacts/runs/20260731-abi61-diagnostic-boundary-r143/` enables exactly the
DDI4, STREAMOFFSET, MAXSTREAMS8, and SM2_CAPS gates. Its
`D3d9Multistream7` run is `T2-diagnostic`, reports
`vs=fffe0200`, `ps=ffff0200`, and `streams=8`, and accepts advertised VS1.1.
Exact native readbacks include:

- direct nonindexed VS2/PS2: `0xff2060df`;
- indexed managed VB/IB: `0xffbf4020`;
- dynamic default-pool VB/IB: `0xff20bf60`;
- managed 2D textured VS2/PS2: `0xff3090d0`;
- isolated sampler s4: `0xff2458a4`;
- isolated sampler s15: `0xffa4386c`; and
- one shader sampling sixteen independently bound managed textures:
  `mask=0xffff`, `0xffff2040`.

The same run retains the fixed-function/SM1.x visual and Reset contract, then
proves stream 0+7 with independent offsets/strides, release-before-Reset,
Reset, teardown, and a 22/22 visual score. The complete XPDM unit target,
ordinary and diagnostic WDK 7.1 free-x86 builds, ChkINF, Inf2Cat, signing,
and frozen-input provenance pass.

This is implementation closure plus a diagnostic capability proof, not a
production promotion. The ordinary r142 title matrix now supersedes the
earlier diagnostic-package title observations: Max Payne passes, while
Warcraft III remains below its strict cadence-frame minimum and UT2003 still
does not reach the required main-menu semantic transition. No optional
capability should rise without its own stock-runtime implementation and
ordinary exact-package evidence.


### 2026-07-29 automated closure checkpoint

Transport remains ABI 58 and shipping remains package `1.0.0.305` with
`MaxStreams = 1`; ordinary builds still decline DDI v4, SM2, and volume caps.
The diagnostic DDI-v4 build continues to reach stock System32 D3D9 and retain
the representative fixed/SM1.x 21-pixel, managed mip/cube TEXBLT, presentation,
and post-Reset proof.

Volume fetch, biased 2D fetch, and four simultaneous 2D samplers are exact-pixel
Tier 2. Escape `0x1105` version 10 preserves stages 0-159 and appends a genuine
`dcl_volume s0` / `sampler3D` phase at stages 160-175. The coordinate-coded
4x4x4 volume produces `(4,7)=0xff23500a` and `(10,7)=0xff835a2a`, then restores
the target and deletes its program and texture. Canonical signed evidence is
`artifacts/runs/20260729-volume-v10-frozen-t2-r2/`: all nine modes pass,
D3d9SelfTest records 17 logical/single draws and 15 readbacks, Max Payne
records 24.506 fresh FPS, and handback is clean.

The same exact package—`qxpdisp.dll`
`ECCC4E74889E6E0A63FCB772CAF4BB56CF361FA13DCAA11405CAEEF4B3E6FA98` and
`qxpmp.sys`
`D7E08077AAF2D007E91383CC0BA9E1553AECA21F95B59E3B2BC9D90DFB7AF8A5`—passes
the final Warcraft III D3D8 and Max Payne visual gates:

- Warcraft III D3D8:
  `artifacts/runs/20260729-volume-v10-final-games-t3-r2/`
  (System32 D3D8, 256 all-`V` cadence frames, exact 256-frame/30-fps MP4);
- Max Payne:
  `artifacts/runs/20260729-volume-v10-final-games-t3-r2/` plus
  `artifacts/temp/volume-v10-final-maxpayne-visual-review.json`
  (125 decodable frames, samples at 7.796/22.965/38.049 s; direct review finds
  intact scene, character, HUD, lighting, textures, and snow).

UT2003 is not a final visual pass. Its bundle in the same T3 run used a direct
`DM-Antalus` launch and stopped at pre-join; the corrected read-only verdict is
`artifacts/temp/volume-v10-final-games-t3-r2-ut-corrected-verification-r5.json`
(`FAIL`). The normal executable reproduction is also non-qualifying. Oracle r2
(`xpdriver-acceptance-ut-oracle-r2.iso`, `7AAC989A...B906F0`) was then invalidated
by its first fresh run: it required a physical `Index.ut2` load, but the
integrity-valid official Demo 2206 archive intentionally configures
`Map=Index.ut2` without containing that map. The real log path is `Browse:
Index.ut2` -> `Failed; returning to Entry` -> viewport actor spawn -> player
controller attachment. The superseding correction is
`artifacts/temp/volume-v10-ut-oracle-t3-verdict-correction-r1.json`. The selected
corrected input is minimal r5 (`xpdriver-acceptance-ut-oracle-r5.iso`,
`EEDDB4A0...984D`), which changes only the observer among r2's 27 files. Its
host gate uses synchronous readback, two guard callbacks, canonical decoded-RGB
change tests, and a separate hash-bound direct menu review. Its fresh run at
`artifacts/runs/20260729-volume-v10-ut-oracle-r5-t3-r2/` fails: all retained
samples/video remain in NvidiaLogo and the return-to-Entry sequence appears only
on forced teardown. The hash-bound verdict is
`artifacts/temp/volume-v10-ut-oracle-r5-t3-r2-verdict.json`. No UT driver defect
is localized; the normal-startup blocker remains unresolved.

The passing runs and the direct-map diagnostic still exercise the
title-agnostic generated-TCI/internal-vertex path,
D3D missing-normal semantics, complete material-source GLSL containment, and
global display authority needed by the measured games. They do not expose SM2
or volume to stock applications. The historical stream-7 run is ABI-57
diagnostic evidence, not current-package runtime proof; shipping truthfully
remains one stream. A corrected normal-launch oracle with post-return-to-Entry
visual evidence must pass before this checkpoint is considered closed.

Remaining production SM2 envelopes include indexed draws, SM2 Reset and
video-memory backing, stock-runtime volume creation/routing, MRT, and
trace-derived copy/fill forms. Indexed work must begin with an actual runtime
opcode trace, then use bounded capture-v3 integrity/lifetime handling, exact
guest-side index expansion, one ABI-58 packet, and portable plus XP/GPU proof.
No cap should rise before its corresponding production evidence.

The incremental phase list below is historical in places; later sections and
the checkpoint above supersede statements that surface-backed capture or host
execution is still wholly absent.

- Phase A platform work is complete: point-size clamping, Blt coherence,
  VS caps, generic multi-stream support, and the Ti 4600 identity are landed.
- Phase B0 is complete: a dormant, guest-only exact `ps_2_0` / `vs_2_0`
  decoder, typed IR, structured control/CFG, dataflow validation, capability
  requirements, and deterministic parser oracle compile in the production
  display driver. No DDI, cap, transport, or runtime behavior changed.
- Phase B1 declaration parsing, object lifecycle, and semantic linking are
  complete and dormant: the counted XPDM `D3DVERTEXELEMENT9` payload has a
  bounded parser and requirement oracle, private opcodes 71-76 manage
  owner-scoped declaration and `vs_2_0` function objects, and a pure linker
  resolves exact DCL semantics without exposing DDI v4.
- Phase B2 pixel lowering is complete and dormant: every exact `ps_2_0`
  form admitted by B0 lowers directly from typed IR to bounded compatibility
  GLSL with fixed metadata and no retained pointers. The DDI decline remains.
- Phase B3 vertex lowering is complete and dormant: every exact `vs_2_0`
  form admitted by B0 lowers directly to pair-qualified compatibility GLSL,
  with D3D clip/fog/point epilogues and fixed metadata. The DDI decline
  remains.
- Phase B4 draw-state foundation is complete and dormant: exact D3D9 constant
  banks, declaration/FVF and vertex-function binding shapes, sixteen stream
  sources, offsets, user-memory state, and native-compatible frequency state
  have bounded DP2 framing. The DDI decline remains.
- Phase B5 pixel-function lifecycle is complete: exact `ps_2_0` objects share
  the permanent owner-scoped generation clock, retain count-only B2 metadata
  and bytecode, and support bounded snapshots, batches, and teardown.
- Phase B6A unified pixel routing is complete: standard opcodes 54-56 profile-
  route through one exact-owner PS1.x/PS2 handle namespace and fail closed.
- Phase B6B production object state is complete: each context owns an
  initialized B4 ledger and object lifecycle is canonical there.
- Phase B6C routes D3D9-only integer/Boolean constants, SOURCE2, and frequency.
- Phase B6D1 routes shared VS/PS float constants and mirrors D3D8 prefixes.
- Phase B6D2 routes shared streams/indices through one teardown-safe generation.
- Phase B6E produces a pointer-free immutable linked draw snapshot and
  canonically validates every execution-bearing field and generation.
- Phase B6F prepares exact SM2 state at the live draw boundary and rejects
  before transport while capabilities remain declined.
- Phase B6G1 resolves permanent backing identity and finite stream/index base
  spans without retaining pointers.
- Phase B6G2 validates bounded conservative envelopes for all fixed draw forms.
- Dormant B6G3A derives tight access ranges from immutable copied index bytes.
- Phase B6G3B1 captures live nonindexed stream-0 user-memory draws into one
  pointer-free blob, plans exact access over it, and still rejects at that
  historical boundary. Later source/transport work is summarized by the
  closure checkpoint above; the next new slice is indexed trace and capture,
  not the stale B6G3B1 follow-up text.
- Author: XPDriver driver development
- Supersedes: the ad-hoc `GD9 FALLBACK` DDI-decline as a "solution" for D3D9
  games. It is a stopgap, not support.

## Problem

D3D9 titles initialize and play audio but render nothing (black screen).
Confirmed on the F.E.A.R. demo (2005, D3D9/SM2.0). The .257 cycle proved the
failing run was *not* a parser bug — it is a capability ceiling:

- Driver `.161` (what the FEAR VM ran) also lacked `D3DNTDP2OP_STATESET`
  (added in `.169`, commit `b5b7043`), producing a DP2 `UNKNOWN 0x27`
  desync and a black screen. `.257` fixes that: a `.257` FEAR run shows
  **zero** DP2 UNKNOWN and 11 cleanly-handled STATESETs.
- But `.257` FEAR still renders nothing: the run does 3 total draws, 60
  texture-stage sets, 38 render-state sets, 3 presents, then stops driving
  the driver. The runtime is rejecting the device's rendering path.

## Root cause (grounded)

The driver deliberately caps itself at the **DX8-level DDI** because it does
not implement the full D3D9 minimum contract. Evidence in
`guest/xpdm/qxpdisp/directdraw.c`:

- `QxpDdGetDriverInfo` on `QXP_D3DGDI2_TYPE_GETDDIVERSION` returns
  `DDHAL_DRIVER_HANDLED` with `ddRVal = DDERR_CURRENTLYNOTAVAIL` and logs
  `QXPDISP GD9 FALLBACK` (9761-9768). The inline comment states the intent:
  reporting `DX9_DDI_VERSION` "would require the full Direct3D 9 minimum
  capability contract (including eight blend stages and shader model 2.0)."
- `qxp_d3d_caps9` is built by **copying** the DX8 caps (9634-9697). It
  inherits `PixelShaderVersion = D3DPS_VERSION(1,3)` (9621 -> 9686),
  `MaxTextureBlendStages = 4` (9609 -> 9680),
  `MaxSimultaneousTextures = 4` (9610 -> 9682), and sets
  `NumSimultaneousRTs = 1` (9697). It does **not** set
  `VertexShaderVersion` beyond the SM1.1 inherited value,
  `MaxVertexBlendMatrices`/`MaxVertexBlendMatrixIndex` (left **0**),
  or any `PS20Caps`/`VS20Caps`.
- The pixel/vertex shader path is miniport-side translation to host
  GLSL/ARB fragment program (`guest/xpdm/qxpmp/qxpmp.c`
  `QXP_D3D_GLSL_UNIFORM_*`, `QXP_GL_FRAGMENT_PROGRAM_ARB`, the
  `COMBINE3`/`COMBINE4` combine path) — a fixed-function/SM1.x-era model.

Net: an SM2.0 title (FEAR) cannot map its `ps_2_0`/`vs_2_0` shaders onto a
device that advertises `ps_1.3`, and the D3D9 DDI stream is declined, so the
game initializes and silently produces no picture.

This also explains the current support spread: DirectDraw 2D (AoE2), D3D8
SM1.x (Max Payne), and D3D8/9-runtime SM1.x (UT2003) work; D3D9 SM2.0 does
not.

## Why the fallback must not be treated as support

`GD9 FALLBACK` keeps the runtime on a "DX8-compatible stream." For an SM1.x
game that is honest and works. For an SM2.0 game it yields a device the game
creates but cannot render through — the black screen is a *consequence* of
the fallback, not an independent bug. We therefore do **not** invest in
hardening or extending the fallback; we build the real contract and lift the
cap only when the contract is honestly met.

## The D3D9 minimum contract to close

Ordered by dependency. Each item names its anchor.

1. **Honest capability surface (per-feature, not a blanket DDI flip).**
   Raise individual caps only as the matching feature is implemented and
   validated; report `DX9_DDI_VERSION` last, once the set is coherent.
   - `MaxVertexBlendMatrices` / `MaxVertexBlendMatrixIndex` (currently 0;
     left unassigned, `directdraw.c` caps init). Drives software-vs-hardware
     VP for blended meshes (KOTOR, many 2003-2004 titles).
   - `MaxTextureBlendStages` / `MaxSimultaneousTextures` 4 -> 8.
   - `NumSimultaneousRTs` 1 -> 4 (only if MRT is implemented).
   - `VertexShaderVersion` / `PixelShaderVersion` -> SM2.0 (only when the
     translator supports them).
2. **D3D9 DP2 records.** Add cases for the D3D9-only opcodes the runtime
   emits on a DX9 DDI stream — chiefly `SETVERTEXSHADERDECL` (73) and
   `SETVERTEXSHADERFUNC` (76), plus `SETSTREAMSOURCEFREQ` (95) and the
   integer/bool shader-const records. (`SETRENDERTARGET2`=85 and
   `SETDEPTHSTENCIL`=86 already exist, `directdraw.c:150`.)
3. **SM2.0 shader translation.** Extend the miniport translator from the
   SM1.x / fixed-function-combine model to `ps_2_0` and `vs_2_0` (host GLSL
   is already the sink). This is the core, highest-risk work item and gates
   the `*_ShaderVersion` cap raise.
4. **D3D9 query support.** `GETD3DQUERYCOUNT` is currently answered with an
   empty table (`QC9`, 9786-9792); implement real occlusion/event query
   handling if targeted titles require it.

## Adapter identity dial

Many XP-era titles gate on the *adapter's PCI identity*, not just its caps:
they carry a whitelist of known-good GPUs and refuse "unknown" adapters.
Prince of Persia: The Sands of Time errors "unsupported video card"; the
community workaround is 3D-Analyze, which spoofs a GeForce4 Ti 4600 *and*
emulates its caps. Our virtual adapter reports VendorId `0x1234` /
DeviceId `0x1111` ("QEMU Standard VGA"), which is off every such whitelist.

So the driver presents the identity of a real, recognized NVIDIA GPU. This
is a GLOBAL adapter property, **not a per-game shim**, and it advances in
lockstep with our real capability so we never claim features we cannot back:

| We honestly back | Identity presented | VendorId:DeviceId |
|---|---|---|
| SM1.x (now) | GeForce4 Ti 4600 | `0x10DE:0x0250` |
| SM2.0 (Phase B) | Radeon 9800 Pro / GeForce FX 5950 | `0x1002:0x4E48` / `0x10DE:0x0333` |
| SM3.0 (Phase C) | GeForce 6800 Ultra | `0x10DE:0x0040` |

**Terminal target: GeForce 6800 Ultra / full Shader Model 3.0** — the XP /
DX9.0c ceiling. Once we honestly back SM3.0 there is nothing an XP-era D3D
game can request that we cannot answer. The identity dial is the
compatibility handshake; honest caps are the contract; the shader-token →
typed IR → GLSL compiler (Phase B) is the keystone that carries us there
(SM3.0 *extends* the SM2.0 compiler with dynamic flow control, vertex-texture
fetch, and more registers — not a rewrite).

Discipline: report the highest-end real card whose known caps do **not**
exceed what we implement. A Radeon 9800 (SM2.0) or 6800 Ultra (SM3.0)
identity before those shaders exist would make games use paths we cannot run.

## Phases and gates

Correctness is the hard gate at every phase (full T2). A cap is only raised
when its feature passes correctness on a representative title; FPS is a
separate, later concern (see the open Max Payne ~5 FPS investigation).

### Phase A — honest D3D9 caps, SM1.x ceiling (no DDI flip)

Read-only scoping (2026-07-19, SOL max) confirms Phase A is **viable and
small**: the `GD9 FALLBACK` DDI decline cleanly falls back to the legacy
DX8-compatible DDI (runtime trace `G9 → GD9 FALLBACK → G8`), and the stock
D3D9 acceptance suite already renders fixed-function and ps_1.1–1.3 through
it. So a ps_1.x D3D9 title should render on the *unchanged* driver; FEAR
fails only on SM2.0 shader translation, not a poisoned device.

- Behaviorally almost no change is needed: copy VS1.1 + 96 constants into
  `caps9`'s raw VS fields; optionally `D3DFVFCAPS_PSIZE` after validation.
- **Keep `MaxVertexBlendMatrices`=0** — a nonzero value would be a lie
  (the miniport multiplies only `world[0]`; XYZB layouts are rejected).
  Vertex blending is a separate feature-before-cap step, not part of A.
- Keep `PixelShaderVersion`/`VertexShaderVersion` at SM1.x.
- Re-sequence per scoping: verify the unchanged driver against a real ps_1.x
  D3D9 title and implement ONLY the observed gap; do NOT merge with B. Do
  the full D3D9 DP2 opcode audit (v9 71–96) just before the Phase C flip.
- Gate: T2 correctness + one D3D9-with-SM1.x-fallback title renders.
- Target: **Prince of Persia: The Sands of Time** (2003, D3D9, designed for
  Shader Model 1.1, no SM2.0 requirement) — installing on
  `xpdm-games-pop-257-20260719`. (Earlier candidate KOTOR is OpenGL, not
  D3D9; it would test the OpenGL path, not D3D9.)

#### Phase A findings — Prince of Persia: The Sands of Time (2026-07-19)

The "renders on the unchanged driver" prediction was too optimistic. On the
stock driver POP froze immediately; with fixes it renders internally but is
still black and quits via its **own** error handler ("unsupported video
card" — a whitelist gate, not a crash). Gaps found, all honest, no shims:

- **Point-size clamp** (`.258`): POP sets `D3DRS_POINTSIZE_MAX=100.0`; the
  draw validator *rejected* >64.0 instead of clamping. Fixed — the device
  clamps to the advertised `MaxPointSize`. Cleared 346 draw-state rejects;
  POP no longer freezes.
- **caps9 `VertexShaderVersion=0`**: caps9 copied the PS fields but not the
  raw VS version / constant count. Fixed — copies VS1.1 + constants.
- **Blt→primary coherence**: accelerated frames live in host GL, but
  `QxpDdBlt` CPU-copied stale guest VRAM (6 `BLT COPY-` failures). Fixed —
  Blt now presents accelerated sources via host GL first, mirroring Flip.
- **`MIN_STREAMS_2`** → generic multi-stream (`.259`, landing): POP requires
  `MaxStreams >= 2`; the driver supported one and rejected `SETSTREAMSOURCE
  dwStream != 0`. Built as real D3D8 multi-stream, not a cap lie. Design: the
  guest **gathers** all declared streams into one canonical packed record and
  reuses the existing display→miniport→host interleaved path — no host
  multi-buffer protocol, only a packed-stride bump 255→256 (transport ABI
  47→48). `caps8.MaxStreams=16` is what POP sees: it reads caps through the
  DX8-fallback DDI (the trace shows VS1.1/PS1.3 = caps8 values), so 16 clears
  its gate; `caps9.MaxStreams` stays 1 (real D3D9 multi-stream needs the
  separate D3D9 declaration/function DDI, opcodes 71-80). Fixed-function FVF
  stays on stream 0; only programmable (VS) draws gather. Done + unit-tested:
  the `qxp_vdecl` parser (per-element source_stream/offset + packed offset,
  `stream_mask`, `stream_spans[16]`) and `QxpGatherVertexDeclaration8`;
  context `stream_handle[16]`/`stream_stride[16]`, `SETSTREAMSOURCE`
  (dwStream<16), caps. In implementation: the two programmable assemblers
  (additive multi-stream branch, single-stream path untouched; logical-index
  recovery `byte_offset/stream_stride[0]`; byte-offset commands self-reject)
  + transport validator/chunk.
- **Ti4600 identity** (`.259`, implemented): POP's whitelist rejects
  `0x1234:0x1111`; first rung of the identity dial above. Host `vga-pci.c`
  now presents the concrete `"VGA"` adapter as NVIDIA GeForce4 Ti 4600
  `10DE:0250` with a QEMU/virtio subsystem `1AF4:1100` (honest "VM"; and the
  guest INF matches `VEN_10DE&DEV_0250&SUBSYS_11001AF4` more specifically
  than any inbox NVIDIA INF, so XP binds our driver). Guest INF is dual-ID
  (NVIDIA + legacy `1234:1111` for smooth migration); `qxpsetup` installs
  against whichever device is present.

Instrumentation showed POP is **not** failing at init: it issues 18,643
draws / 18,647 presents across 7 device sessions, but the picture is black.
Whether that residual black is the Blt path (now fixed) or shader-produced
black is deferred until the `.259` run: with multi-stream + Ti4600 clearing
the two hard gates, the draw/multistream debug counters and the visible
result will localize it before any further pixel-probe diagnosis. The black
plus the `MIN_STREAMS_2` give-up were the remaining Phase-A gates.

### Phase B — SM2.0 shader translation

This is the large, highest-risk item (read-only scope, 2026-07-19: roughly
13–18 engineer-weeks). The legacy translators still emit `vs_1_1` directly as
`!!ARBvp1.0` (`qxp_vshader.c`) and `ps_1.1–1.3` as `!!ARBfp1.0`
(`qxp_shader.c`), with fixed-function via COMBINE3/4. SM2.0 uses a separate
**D3D9 token decoder → typed CFG/IR → direct compatibility-GLSL emitter**;
do not grow the hard-coded ARB emitters or the ARB→GLSL reparser. **Both
`ps_2_0` and `vs_2_0`, plus the D3D9 DDI flip, are required for FEAR—neither
stage alone unblocks it.**

#### Phase B0 — decoder and typed IR (complete, dormant)

`qxp_shader_decode.c` now accepts exact `ps_2_0` and `vs_2_0` token streams
into the private, non-wire `qxp_shader_ir.h` representation. It bounds framing,
comments, operands, immediates, declarations, labels, calls, structured static
flow, instruction slots, and relative addressing. Its summarized
intraprocedural CFG keeps call targets explicit, while nonrecursive dataflow
validates component initialization, subroutine effects, and zero/executed/later
LOOP/REP paths. Capability-dependent PS forms record the required temporary
count, `D3DPS20CAPS` bits, and instruction slots rather than changing public
caps. Fixed 16,384-token and 512-instruction ceilings are explicit decoder
resource limits.

The deterministic oracle covers every exact-profile opcode admitted by this
slice, golden typed IR, all truncation prefixes, malformed declarations and
operands, read ports, repeated-definition last-wins, sampler-dependent slot
costs, CFG/call/loop behavior, canaries, decode-twice equality, and bounded
mutation fuzzing. It passes portable C89 `/Werror`, full `preinstall-test`, and
the authentic WDK 7.1 XP x86 `/W4 /WX` build. Frozen review
`SM2-B0-DECODER-306-R4` approved with no findings. Evidence is in
`artifacts/runs/20260723-sm2-b0-decoder/`.

B0 is intentionally uncalled: `GETDDIVERSION` still declines DX9 DDI version
4, SM2 caps remain zero, DriverVer remains `.305`, transport ABI remains 53,
and no FEAR/game support claim is made.

#### Phase B1 — D3D9 declarations and shader objects (semantic link complete, dormant)

`qxp_vdecl9.c` parses the 1–64 counted `D3DVERTEXELEMENT9` records following
XPDM opcode 71. The DDI count excludes the application-level `D3DDECL_END`,
which is explicitly rejected. The parser validates the exact 8-byte shape,
type sizes, stream grouping and alignment, semantic uniqueness, overlap/share
requirements, POSITIONT, tessellator methods, displacement lookup, and
TESSFACTOR interactions. It records the future `DeclTypes`, `DevCaps`,
`DevCaps2`, maximum-stride, programmable-pipeline, and VS-version requirements
without exposing them.

The focused oracle exhausts ordinary/modern types, share-offset boundaries,
all generation methods and compatible types, capability masks, POSITIONT,
count/no-END/error-zeroing contracts, canaries, and bounded deterministic
mutations. Strict C89, full `preinstall-test`, WDK `/W4 /WX`, and frozen review
`SM2-B1-VDECL9-R3` pass. Evidence is in
`artifacts/runs/20260723-sm2-b1-vdecl9/`.

The lifecycle slice wires private DP2 opcodes 71-76 while keeping the production
translation unit at `DIRECT3D_VERSION 0x0800`. It uses independent 256-entry
owner-scoped declaration and function namespaces. Create framing is bounded by
subtraction; declaration parse, function decode, deep copy, and allocation occur
outside `qxp_d3d_table_lock`; atomic publication/bind/detach occurs under the
lock; rollback and frees occur after unlock. Function objects retain exact
bytecode and compact scalar B0 summaries, never transient input or the large IR.
Bindings carry nonwrapping generations; exhaustion permanently refuses future
publication rather than allowing ABA. Declaration bit zero identifies a private
object handle, even nonzero values remain legacy FVF state, and zero clears.
Failed odd-handle lookup preserves prior state. Delete and exact-owner teardown
clear matching generation-tagged bindings and release each object exactly once.
Every DP2 failure reports the outer command offset in `lpDDCommands`.

Frozen review `SM2-B1-D3D9-OBJECTS-R2` approved the corrected slice with no
findings. Focused lifecycle/production-batch tests, strict C89, full
`preinstall-test`, WDK `/W4 /WX`, PE/ChkINF/Inf2Cat/signing, and the final fresh
T2 pass. Evidence is in
`artifacts/runs/20260723-sm2-b1-objects-final-t2/`. That T2 validates only no
regression to reachable stock D3D8/legacy D3D9 context, Reset, ownership, and
teardown paths; it does not claim VM execution of private opcodes 71-76.

`qxp_d3d9_shader_link.c` completes the dormant declaration/function semantic
boundary as a pure draw-preparation operation. Each exact `vs_2_0` DCL
usage/index selects the matching declaration element and destination `v#`;
declaration order is irrelevant. Extra elements are ignored and do not require
their streams. Missing VS2 semantics remain legal undefined inputs rather than
link errors, and POSITIONT0 returns an explicit successful bypass with no
programmable mapping. The fixed result retains matched stream, offset, type,
method, byte size, span, input/stream masks, and a 64-element attribution mask,
but no pointers. A future gather must supply a safe dummy value for undefined
inputs and must never read absent guest or host memory.

The link is deliberately not stored in either independently bound object. A
future draw snapshots the exact declaration/function generations, decodes and
links outside the table lock, and may cache a pair only under both generations.
`QxpValidateParsedVertexDeclaration9` reconstructs every retained element,
reuses the exact declaration parser, and requires byte-identical derived state;
the linker also requires typed DCL usage/index metadata to match its raw token.
This prevents corrupted parser or IR metadata from becoming a valid mapping.

The oracle covers declaration/shader order, missing and extra semantics, ignored
streams, all declaration types, generated methods, all 16 input registers, the
64-element boundary, POSITIONT bypass, raw/typed mismatches, exact parser-result
integrity, canaries, determinism, no input retention, and bounded mutations.
Strict C89/pedantic, full `preinstall-test`, authentic WDK 7.1 XP x86
`/W4 /WX`, ChkINF 0/0, no nonempty sidecars, and frozen review
`SM2-B1-SEMANTIC-LINK-R2` pass. Evidence is in
`artifacts/runs/20260723-sm2-b1-semantic-link/`.

#### Phase B2 — direct ps_2_0 compatibility-GLSL lowering (complete, dormant)

`qxp_pshader_glsl.c` consumes only B0's exact typed `ps_2_0` IR and emits GLSL
1.20 directly; it does not extend or reparse the legacy ARB translators. A
count-only pass and a caller-provided output buffer enforce the 512 KiB source
ceiling. The emitter allocates nothing, retains no pointers, and returns fixed
metadata for 32 float constants, exact last-wins DEF DWORDs, 16 typed samplers,
input masks/effective centroid interpolation, MRT/depth outputs, discard, and
the decoder's independently verified temp/capability/slot requirements.

The preflight independently reconstructs the exact pixel opcode schema, token
spans, slots, controls, sentinels, register/read-port rules, declaration rows,
component initialization, matrix spans, texture dependency order, and
`D3DPS20CAPS` requirements before any count-driven traversal or index. Corrupt
IR therefore fails closed. Lowering covers every B0-admitted arithmetic/matrix,
plain/projected/biased 2D/cube/volume texture, TEXKILL, partial-precision,
saturate, source-negate, MRT, and depth form. Omitted `v#` channels use the
required `(0,0,0,1)` defaults; color inputs are clamped and receive D3D9's
automatic centroid interpolation.

The deterministic oracle decodes and emits every admitted opcode, all texture
dimensions and controls, partial declarations/defaults, exact DEF metadata,
dependent-read capability boundaries, capacity/error zeroing, canaries,
determinism, and hostile typed-IR mutations. Its generated coverage shader
compiles and links on the real NVIDIA compatibility host and a live pixel
oracle discriminates color clamp/defaults, DP2ADD, plain/projective/bias texture
sampling with separate texels/mips, positive/negative TEXKILL, MRT, and depth
output. Strict C89/pedantic, full `preinstall-test`, authentic WDK 7.1 XP x86
`/W4 /WX`, PE/ChkINF/Inf2Cat/signing, and frozen review
`SM2-B2-PS20-GLSL-R4` pass. Evidence is in
`artifacts/runs/20260723-sm2-b2-ps20-glsl/`.

B2 is compiled into the display driver but has no production caller. It does
not compile on the paired host, bind to a draw, alter transport, or expose DDI
v4/caps. DriverVer remains `.305` and ABI remains 53.

#### Phase B3 — direct vs_2_0 compatibility-GLSL lowering (complete, dormant)

`qxp_vshader20_glsl.c` consumes only exact B0 `vs_2_0` typed IR. It emits
bounded GLSL 1.20 with 12 temporaries, 16 attributes, 256 float constants, 16
integer constants, 16 Boolean constants, all four `a0` components, `aL`, static
IF/REP/LOOP, and resolved forward subroutines. The fixed caller-owned metadata
retains usage/index inputs; exact output masks; raw last-wins DEF/DEFI/DEFB
DWORDs; and relative-address, flow, and resource summaries. There is no
allocation, large automatic metadata copy, or retained pointer.

The independent preflight reconstructs opcode/token framing, roles and unused
storage, raw/typed DCL integrity, read ports, scalar masks, relative rows,
control pairs, call/iteration and `aL` scope, interprocedural definite
assignment, iteration stability, outputs, slots, and the complete CFG. A B3
correction tightened both B0 and the lowerer to D3D9's one-component
RCP/RSQ/EXP/EXPP/LOG/LOGP/POW destination rule.

Emission preserves high-base relative matrix rows with safe out-of-range-zero
constant reads. Pair-specific centroid metadata produces matching generated
VS/PS interfaces. The epilogue preserves original D3D `oPos` in
`gl_ClipVertex`, applies caller-selected target Y orientation and `z=2z-w`,
implements vertex/table/no-`oFog` semantics, and writes programmable point
size.

The deterministic 99-instruction fixture covers every admitted form. Real
NVIDIA compatibility-host oracles observe high-base `c255[a0.x]`, negative
MOVA, REP, LOOP/`aL`, IF, CALL, constants, Y/depth conversion, user clipping,
point footprint, special zero/NaN behavior, replicated VS2 EXPP/LOGP, chained
arithmetic/geometric operations, all five matrix forms, all fog modes, and
centroid/non-centroid generated VS+PS links. Strict C89/pedantic, full
`preinstall-test`, authentic WDK 7.1 XP x86 `/W4 /WX`, package gates, and
frozen review `SM2-B3-VS20-GLSL-R4` pass. Evidence is in
`artifacts/runs/20260723-sm2-b3-vs20-glsl/`.

B3 is compiled but uncalled. Its centroid/clip controls are future linked-pair
inputs, not active runtime state. It changes no DDI, caps, transport, host
execution, DriverVer `.305`, or ABI 53.

#### Phase B4 — D3D9 draw-state foundation (complete, dormant)

`qxp_d3d9_draw_state.c` adds a fixed-storage, synchronization-free state
container for the next production integration slice. It holds exact generation-
tagged declaration and vertex-function binding shapes, mutually exclusive
private-declaration/FVF modes, sixteen streams with offset/stride/user-memory
and raw frequency state, and SM2's VS float 256/int 16/bool 16 and PS float
32/int 16/bool 16 constant banks plus deterministic set masks.

The bounded DP2 oracle frames opcodes 48, 49, 50, 57, 77, 80, 83, 93, 94,
and 95 using exact XP WDK record layouts. Fixed batches decode into a bounded
candidate before one publication, so truncation or a later invalid record
cannot partially mutate state and aliased input cannot corrupt a subsequent
record. Variable constant batches commit complete earlier records only.
Every failure reports the supplied outer-command offset; no input pointer or
allocation is retained.

Stream-frequency validation follows native D3D9 conformance rather than
narrower instancing-usage guidance: raw zero and both flags are rejected;
positive raw dividers, flag-only values, and nonzero-stream `INDEXEDDATA` are
accepted; `INSTANCEDATA` on stream zero is rejected. The deterministic oracle
covers every constant bank boundary, masks, all truncation prefixes, later-
record rollback, bindings/FVF transitions, all stream forms, native frequency
cases, unbind, canaries, input lifetime, alias safety, and repeat equality.

Strict C89/pedantic and full `preinstall-test` pass. Authentic WDK 7.1 XP x86
`/W4 /WX` initially exposed C4701 path-analysis warnings in the fixed-record
decoder; zero-initializing its bounded locals fixed the earliest gate. The
final WDK/package, ChkINF 0/0, Inf2Cat/signing, and acceptance-ISO builds pass.
Frozen review found no defect in the final staged blobs after the native-
frequency correction. Evidence is in
`artifacts/runs/20260723-sm2-b4-draw-state/`.

B4 is compiled but has no production caller. It does not bind state into
`QXP_D3D_CONTEXT`, execute a draw, change DDI/caps/transport/host behavior, or
alter DriverVer `.305` or ABI 53.

#### Phase B5 — owner-scoped ps_2_0 function lifecycle (complete, dormant)

`qxp_d3d9_shader_objects.c` now carries a separate 256-entry pixel-function
namespace beside declarations and VS2 functions. All three namespaces consume
one permanent nonwrapping generation clock and use the same exact owner tuple.
Pixel creation allocates the large B0 IR outside the table lock, exact-decodes
`ps_2_0`, calls the B2 emitter count-only, releases the IR, then allocates a
compact trailing-storage object containing deep-copied bytecode, source size,
compact decode summary, and fixed B2 definition/interface/sampler/output/
capability metadata. No transient pointer persists.

Publication, exact-generation binding, deletion, snapshots, and detachment are
synchronization-free primitives for a caller-held table lock. Duplicate,
full-table, malformed-prepared-object, and permanent-generation-exhaustion
failures consume no generation and mutate neither object nor table. Owner
teardown detaches under callbacks and releases after unlock. Variable CREATE
batches preserve complete earlier records; fixed SET/DELETE batches frame the
whole payload before sequential semantic mutation. Every failure retains the
outer DP2 command offset.

The oracle covers every CREATE prefix, exact PS2 versus VS/malformed profiles,
count-only B2 metadata and bytecode lifetime, every allocation failure,
cross-stage numeric handles, multiple owners, shared generation sequencing,
set/clear/foreign preservation, delete/recreate ABA, stale and short snapshots,
duplicate/full/exhaustion atomicity, 29 independent prepared-object metadata
corruptions, exact-owner teardown, batch prefix/framing/offset behavior,
canaries, and lock/allocation/free balance. Strict C89/pedantic, full
`preinstall-test`, authentic WDK 7.1 XP x86 `/W4 /WX`, package gates,
acceptance-ISO construction, and frozen review
`SM2-B5-PIXEL-LIFECYCLE-R2` pass. Evidence is in
`artifacts/runs/20260723-sm2-b5-pixel-lifecycle/`.

XP assigns standard pixel CREATE/DELETE/SET values 54/55/56 to both D3D8 and
D3D9. At the B5 boundary `directdraw.c` still routed them only through its
owner-scoped PS1.x translator/cache; B6A resolves that shared boundary below.

B5 changed no DDI, caps, transport, host execution, DriverVer `.305`, or ABI
53.

#### Phase B6A — unified standard pixel routing (complete, fail-closed)

`QxpD3d9DispatchPixelFunctionBatch` is now the checked production framing
surface for opcodes 54-56. Each variable CREATE record is bounded before its
profile token is copied; exact `ps_2_0` selects B5 preparation while all other
profiles retain the existing PS1.x translator's exact rejection/acceptance.
Fixed SET and DELETE batches frame every record before the first callback.
Earlier complete CREATE or semantic SET/DELETE records remain committed, every
failure retains the outer DP2 command offset, and B5 allocation/table/
generation failures preserve resource classification.

The existing and B5 pixel tables now form one exact-owner numeric namespace.
Legacy installation checks B5 under `qxp_d3d_table_lock`; B5 publication checks
the legacy table under the same lock after prepare. Thus either racing creator
wins one final atomic publication and the loser rolls back/frees after unlock.
A combined context binding contains the raw PS1 handle and generation-tagged
PS2 binding. Unified SET switches both fields atomically, zero clears both,
and impossible dual matches fail without mutation. Unified DELETE resolves
exactly one representation; the existing PS1 cache/refcount and post-unlock
host-delete behavior is retained, while detached PS2 storage is freed after
unlock. Context cleanup calls the exact-owner PS2 teardown after detachment.

PS2 execution deliberately remains absent. Draw-time resolution checks the
combined binding under the table lock and rejects a bound PS2 function before
fixed-function or PS1 selection, preventing a false render. The portable oracle
executes the exact production dispatcher for mixed PS1/PS2 CREATE, every
relevant prefix/fixed framing class, resource status, consumed bytes, semantic
failure, and outer offsets. It also covers both binding fields, both collision
directions, exact owners, stale/corrupt state, set/clear/delete, and the
fail-closed execution predicate. Existing B5 teardown tests prove detach under
lock and release after unlock.

Strict C89/pedantic, full `preinstall-test`, authentic WDK 7.1 XP x86 `/W4
/WX`, no nonempty sidecars, ChkINF/Inf2Cat/signing, acceptance ISO, and frozen
review `SM2-B6A-UNIFIED-PIXEL-R2` pass. A fresh signed XP T2 passes Install,
Boot, D3dVisual, D3d9, D3dVs11, simultaneous contexts, process succession, and
clean handback, exercising the retained live PS1 create/set/draw/delete/cache
path. Evidence is in
`artifacts/runs/20260723-sm2-b6a-unified-pixel-t2/`.

B6A remains ABI-neutral and changes no DDI, caps, transport, host execution,
DriverVer `.305`, or ABI 53.

#### Phase B6B — production object-state ledger (complete, fail-closed)

`QXP_D3D_CONTEXT` now embeds one initialized `QXP_D3D9_DRAW_STATE`; the loose
D3D9 declaration, FVF, and VS2 fields are removed. Context creation establishes
all sixteen divider defaults at one before publication. Private lifecycle
batches write the ledger binding members directly, then derive and validate
its declaration mode after success or prefix failure while the existing
`draw_batch_lock` serializes that context. Bound-object deletion, object/FVF/
clear transitions, and earlier successful records therefore remain coherent.

The shared draw resolver now rejects active FVF, private declaration, VS2, PS2,
or malformed D3D9 object state before unchanged host submission. The portable
oracle exercises the exact object-batch-to-ledger bridge, initialization,
set/delete/clear/FVF/prefix transitions, mode derivation, corrupt dual state,
fail-closed classification, and canaries. Frozen review
`SM2-B6B-OBJECT-STATE-R1` approved with no findings. Strict C89, full
`preinstall-test`, WDK 7.1 `/W4 /WX`, package/sidecar/ISO gates, and fresh
signed Install/Boot/D3dVisual/D3d9/D3dVs11/MultiContext/Succession T2 pass with
clean handback. Evidence is in
`artifacts/runs/20260723-sm2-b6b-object-state-t2/`.

B6B does not yet migrate constants, streams/frequency, or state blocks into
production B4 consumption and adds no linked snapshot, execution, DDI, cap,
transport, host, DriverVer, or ABI change.

#### Phase B6C — D3D9-only state routing (complete, fail-closed)

The six non-colliding D3D9-only DP2 opcodes 77/80/83/93/94/95 now enter the
embedded ledger: VS/PS integer and Boolean constants, SOURCE2, and stream
frequency. `QxpD3d9DispatchDrawStateBatch` is the exact portable production
bridge. It allocates and snapshots through callbacks, parses outside
`qxp_d3d_table_lock`, and publishes only the affected bank or stream field under
the lock. Complete constant-record prefixes publish on later failure; fixed
batches are atomic; resource and outer-command errors remain exact. D3D8 state-
block recording rejects these commands rather than creating unreplayable state.

Per-stream SOURCE2 provenance distinguishes offset-zero SOURCE2 from legacy
state. SOURCE2 and nonunit frequency reject every legacy draw in the shared
resolver. Same-owner surface destruction clears matching streams and always
advances a saturating nonwrapping source generation. Publication validates its
snapshot generation, so both previously-bound and first-time candidate handles
cannot cross teardown. R1 found the first-time gap; R2
`SM2-B6C-D3D9-STATE-R2` approved the corrected frozen snapshot with no findings.
Portable tests cover every routed opcode, truncation and constant prefixes,
fixed rollback, outer offsets, allocation failure, lock balance, state-block
rejection, zero-offset provenance, reset, malformed streams, exhaustion,
matched/unmatched teardown, the initially-unbound race, and canaries. Strict
C89, full preinstall, WDK/package/sidecar/ISO gates, and fresh signed seven-mode
XP T2 pass. Evidence is in
`artifacts/runs/20260723-sm2-b6c-d3d9-state-t2/`.

B6C deliberately leaves shared D3D8/D3D9 opcodes 48/49/50/57 on the legacy
path. It adds no indices, linked snapshot, execution, DDI, caps, transport,
host, DriverVer, or ABI change.

#### Phase B6D1 — shared float constants (complete)

`QxpD3d9DispatchSharedFloatBatch` is now the exact production parser for shared
DP2 opcodes 48 and 57. It validates 8-byte headers and bounded payloads outside
the table lock, then publishes B4 and the representable legacy prefix together
under it. D3D8 keeps VS c0-c95 and PS c0-c7; dormant D3D9 accepts VS c0-c255
and PS c0-c31. Earlier complete records survive a later failure and every error
reports the outer command. High registers are rejected while a legacy state
block records because replay cannot represent them.

VS state-block callbacks run after live publication, preserving the established
behavior where operation-allocation failure leaves the current constants and
any earlier recorded operations. Programmable D3D8 VS bind mirrors declaration-
defined constants into B4 while already holding the table lock. State-block
execute republishes recorded VS constants to both representations after the
shader bind; capture remains read-only. The portable oracle invokes the exact
production dispatcher and covers all boundaries, every truncation prefix,
high-only and crossing ranges, B4/legacy equality, callback resource failure,
state-block rejection, aliasing, locks, status, and canaries. Frozen review
`SM2-B6D1-SHARED-FLOAT-R1` approved with no findings; strict C89, full
preinstall, WDK/package/sidecar/ISO, and fresh signed seven-mode XP T2 pass.
Evidence: `artifacts/runs/20260724-sm2-b6d1-shared-float-t2/`.

B6D1 adds no stream/index bridge, linked snapshot, execution, DDI, caps,
transport, host, DriverVer, or ABI change.

#### Phase B6D2 — shared streams and indices (complete)

`QxpD3d9DispatchSharedResourceBatch` is now the exact production parser for
shared DP2 opcodes 49/50/51. It preflights fixed widths, snapshots the common
resource-binding generation, decodes all records into bounded candidates
outside `qxp_d3d_table_lock`, and publishes B4 plus D3D8 mirrors atomically
under the lock only if the generation still matches. Semantic failures and
truncation mutate nothing; every error reports the outer command.

Classic stream source clears SOURCE2 provenance and preserves its divider.
User-memory source remains stream-zero-only and also preserves frequency.
Index state is now canonical in B4: unbound handles normalize stride to zero;
bound indices accept only stride 2 or 4. Streams, indices, SOURCE2 publication,
and same-owner surface teardown share the saturating generation. Teardown
clears matching B4/legacy streams and indices and always advances, so neither a
previously-bound nor first-time candidate can cross destruction. D3D8 state-
block recording deliberately retains its prior live-only stream/index behavior;
this slice does not claim replay support.

The portable oracle invokes the exact production dispatcher and covers all
three opcodes, every truncation prefix, semantic all-or-none rollback, duplicate
last-wins state, classic/SOURCE2/frequency transitions, UM, index bind/unbind,
alias input, recording mode, first-bind teardown race, generation exhaustion,
matched teardown, B4/legacy equality, lock balance, outer offsets, malformed
canonical state, and canaries. Frozen review
`SM2-B6D2-SHARED-RESOURCE-R1` approved without findings; strict C89, full
preinstall, WDK/package/sidecar/ISO, and fresh signed seven-mode XP T2 pass.
Evidence: `artifacts/runs/20260724-sm2-b6d2-shared-resource-t2/`.

B6D2 adds no linked snapshot, execution, DDI, caps, transport, host, DriverVer,
or ABI change.

#### Phase B6E — immutable linked draw snapshot (complete, dormant)

`QxpD3d9PrepareDrawSnapshot` allocates a pointer-free immutable description plus
maximum VS-bytecode and typed-IR scratch outside the table lock. Under caller
callbacks it copies canonical raw state and exact-owner private declaration,
VS2 bytecode/summary, and generation-checked stored PS2 metadata. After unlock
it decodes VS2, checks the stored summary, performs semantic declaration link,
and runs two-pass count-only VS lowering against the matched PS centroid mask.
No input, object, bytecode, or scratch pointer is retained.

The snapshot retains raw state and separately derives DEF/DEFI/DEFB-overlaid
effective constants, declaration and shader generations, PS sampler metadata,
required matched streams/spans, SOURCE2 and stream-zero UM state, POSITIONT
bypass, and a ten-slot VS→PS interface with matched/missing component and
centroid masks. Missing vertex inputs use four raw zero DWORDs, matching the
Windows behavior recorded by Mesa Nine's dummy VBO. Missing VS outputs remain
explicit zero-policy metadata and are not falsely claimed linked. Structural
validation rejects malformed ledger/index/frequency state, missing required
handles, short stride, and offset/span overflow; actual surface backing and
draw-range validation remain downstream.

Final freshness validates state and all exact object generations. Public
validation canonically reconstructs the whole snapshot and byte-compares every
pointer-free field, rejecting both current-state changes and hostile edits.
Every large allocation and rollback/free occurs outside locks. R1
`SM2-B6E-LINKED-SNAPSHOT-R1` found that the first public validator checked only
raw state/generations; R2 `SM2-B6E-LINKED-SNAPSHOT-R2` approved canonical
reconstruction and hostile tests covering declaration, link, interface,
streams, defaults, stage metadata, effective constants, raw state, and pixel
binding. Strict C89, full preinstall, WDK/package/sidecar/ISO, and fresh signed
seven-mode XP T2 pass. Evidence:
`artifacts/runs/20260724-sm2-b6e-linked-snapshot-t2/`.

B6E adds no execution, resource backing lookup, DDI, caps, transport, host,
DriverVer, or ABI change.

#### Phase B6F — live prepare-but-reject draw gate (complete)

`QxpD3d9GateDrawExecution` preserves a zero-allocation/zero-lock fast path for
canonical legacy state. Any D3D9-only or malformed state instead enters the
exact B6E preparation path. A complete private declaration/VS2/PS2 set is fully
prepared, immediately freed outside the table lock, and still reported
unsupported; preparation/resource failures preserve their detailed status. A
zero SM2 handle with nonzero generation is not treated as legacy.

`QxpD3d9GateDrawCommand` is the production pre-switch seam. It owns the six
fixed draw opcodes, validates the complete outer record array before semantic
work, accepts zero count as a no-op, and returns the exact outer offset for
truncation, unsupported state, or resource failure. WDK compile-time assertions
bind its portable opcode numbers and x86 record sizes to the actual DDI enums
and structures. `directdraw.c` calls it before POINTS, CLIPPEDTRIANGLEFAN,
DRAWPRIMITIVE, DRAWPRIMITIVE2, DRAWINDEXEDPRIMITIVE, and
DRAWINDEXEDPRIMITIVE2 handlers. Unsupported/truncated state maps to
INVALIDPARAMS; allocator failure maps to CURRENTLYNOTAVAIL. Both return before
transport or frame marking. Earlier successful DP2 prefix work retains its
normal batching semantics. The original resolver gate remains defense in depth.

The production-seam oracle covers every opcode with complete SM2, legacy,
zero-count, and one-byte-short batches; a nondraw control; exact offsets; both
failure classes; and host/frame/batch counters. The underlying gate retains all
three allocation-failure rollbacks, final freshness race, exact namespace,
malformed state, and lock/allocation balance checks. R1
`SM2-B6F-LIVE-DRAW-GATE-R1` found the generation-only PS2 shortcut and missing
seam oracle; R2 found a recognized null-status outer-offset defect; R3
`SM2-B6F-LIVE-DRAW-GATE-R3` approved the fixes without findings. Strict C89,
full preinstall, WDK/package/sidecar/ISO, and fresh signed seven-mode XP T2
pass. Evidence: `artifacts/runs/20260724-sm2-b6f-live-draw-gate-t2/`.

B6F adds no SM2 draw execution, DDI, caps, transport, host, DriverVer, or ABI
change.

#### Phase B6G1 — permanent backing identity and finite base spans (complete)

`qxp_d3d9_draw_backing.[ch]` produces a pointer-free snapshot after B6E
preparation and before B6F rejection. It requires the exact numeric shader
owner plus opaque DirectDraw-local identity, byte-equal current stream state,
and a permanent surface incarnation. Every shader-required stream records its
owner-scoped handle, role, system/video/observed-UM class, content generation,
hard finite byte extent, video offset, stride, SOURCE2 offset, divider, and
declaration-required span. Indexed opcodes additionally require a finite exact
index backing with stride 2 or 4; nonindexed opcodes ignore unrelated index
state. Stream-zero UM stores only validated call-envelope scalars and never a
pointer or address. Circular execute buffers, fallback/remaining-VRAM extents,
unknown roles/flags, short/overflowing spans, stale state, foreign owners, and
malformed metadata fail closed. Canonical reconstruction rejects hostile or
stale snapshots without allocation.

Each fresh surface-table publication consumes a module-lifetime, never-zero,
nonwrapping 64-bit incarnation before slot mutation. Destroy clears the slot
without rewinding the clock; same live owner+handle wrapper refresh preserves
incarnation and content authority so `.284`/`.295` host-newer images are not
invalidated. A registered `DestroyDDLocal` handles system/reference surfaces
that may not receive DestroySurface: the portable production teardown seam
selects exact-owner entries and invokes shared legacy+B4 unbinding under one
table lock, including handle-zero references, then host texture names are
deduplicated/deleted after unlock. Raw owner-pointer+handle reuse must therefore
receive a different incarnation.

The backing oracle covers 1/16 streams, system/video/UM, SOURCE2 boundaries,
indexed/nonindexed, finite extents, wrong roles, exact/foreign owners, stale
state, teardown/rebind/content/incarnation changes, exhaustion, hostile
validation, and lock discipline. The same production teardown seam is tested
for nonowner entries, legacy+B4 generations, handle-zero references, repeated
teardown, no-scratch cleanup, dedup/capacity, partial callback failure, and
exact suffix retry. R1 found missing owner teardown; R2 found a nonproduction
oracle; R3 found untested partial callback failure; R4
`SM2-B6G1-BACKING-AUTHORITY-R4` approved the cumulative snapshot. Strict C89,
full preinstall, WDK/package/sidecar/ISO, and fresh signed seven-mode XP T2
pass. Evidence: `artifacts/runs/20260724-sm2-b6g1-backing-authority-t2/`.

B6G1 validates identity and finite base spans only.

#### Phase B6G2 — conservative fixed-record draw envelopes (complete)

`qxp_d3d9_draw_envelope.[ch]` independently frames the exact private x86
layouts for all six fixed draw opcodes and normalizes every record into a
pointer-free aggregate. Production compile-time assertions bind opcode values,
sizes, and execution-bearing field offsets to WDK D3DNTHAL structures. Point,
line-list/strip, and triangle-list/strip/fan source counts are overflow checked;
zero primitive records remain framed no-access operations.

For nonindexed forms, VStart/POINTS starts are logical indices while
FirstVertexOffset forms must divide exactly by canonical stream-0 stride. Each
required stream is checked through `SOURCE2 + logical*stride + required span`
against its B6G1 finite extent, then multiple records aggregate minimum first
and maximum exclusive bounds. Indexed forms separately bound StartIndex units
or unaligned StartIndexOffset bytes, normalize signed BaseVertexIndex or
divisible BaseVertexOffset, reject invalid MinIndex/NumVertices windows, and
apply the runtime-declared conservative vertex window to every stream. Divider
values other than one reject because instancing/frequency ranges remain
unimplemented.

The live inspector consumes the current transient command-record pointer only
inside its synchronous callback and clears it before return. It first builds
and validates B6G1, then builds and canonically byte-validates B6G2; failure
status 18 distinguishes backing and 19 distinguishes the declared envelope.
Both still reject before transport. The oracle covers every opcode and
primitive type, exact layouts, zero/multiple records, 16 streams,
system/video/UM, byte conversions and alignment, signed indexed bases, stride
2/4, unaligned indexed2 starts, arithmetic/exclusive-end boundaries,
truncation, hostile foundation/output/padding and later record/backing changes.
R1 found no source defect but produced an untracked NUL inspection artifact;
after removal, R2 `SM2-B6G2-DRAW-ENVELOPE-R2` approved the unchanged clean
snapshot. Strict C89, full preinstall, WDK/package/sidecar/ISO, and fresh signed
seven-mode XP T2 pass. Evidence:
`artifacts/runs/20260724-sm2-b6g2-draw-envelope-t2/`.

B6G2 is conservative, not exact indexed access: it does not read index values,
pin/copy memory, support circular or instanced accesses, execute SM2, expose
DDI/caps, or change transport/host/DriverVer/ABI.

#### Phase B6G3A — exact aggregate index-access planning (complete, dormant)

`qxp_d3d9_draw_access.[ch]` is production-linked but intentionally has no live
caller. It first reconstructs and validates the exact B6G2 foundation, then
requires a caller-owned immutable byte copy whose first byte and length exactly
match B6G2's aggregate index interval. The four nonindexed opcodes carry their
already exact B6G2 ranges through. For both indexed layouts it safely byte-reads
unaligned records and little-endian 16/32-bit values, preserves unit-versus-byte
start semantics, enforces every actual raw index inside the record's broad
MinIndex/NumVertices promise, applies signed BaseVertexIndex or divisible
BaseVertexOffset, and rejects negative effective indices. Checked arithmetic
then derives tight observed aggregate logical and SOURCE2+stride+span ranges for
all sixteen required streams and proves each is a subset of B6G2 and its finite
backing. The result retains no pointer; canonical validation rebuilds and
byte-compares all output including padding.

The deterministic oracle uses the real B6E/B6G1/B6G2 builders and covers all
primitive counts/forms, both indexed layouts and widths, all nonzero bytes in
full-width values, unaligned starts/copies, a four-byte copy near ULONG's finite
boundary, broad promises, signed bases, 16 streams, exact final extents,
gapped/overlapping records, null/short/long copies, malformed foundations,
complete nested status, output/padding mutation, and allocation/lock absence.
R1 identified full-width decode and status-oracle gaps; test-only commit
`55dc2a7` closed them, and R2 `SM2-B6G3A-EXACT-INDEX-ACCESS-R2` approved the
clean snapshot. Strict C89, full preinstall, WDK/package/sidecar/ISO, and fresh
signed seven-mode XP T2 pass. Evidence:
`artifacts/runs/20260724-sm2-b6g3a-exact-index-access-t2/`.

B6G3A's input-copy immutability and origin are caller contracts. It does not
copy live backing, pin lifetime, prove incarnation/generation after a copy,
support circular/instanced access, execute, expose DDI/caps, or change
transport/host/DriverVer/ABI.

#### Phase B6G3B1 — live nonindexed user-memory capture (complete)

`qxp_d3d9_draw_capture.[ch]` is the first slice with a live production caller.
After a successful B6G2 inspection, `qxp_d3d9_draw_backing_inspect` asks the
exported `QxpD3d9DrawCaptureIsEligible` whether the draw is a nonindexed,
stream-0-only user-memory draw whose prepared and backing required,
required-user-memory, and populated stream masks are all exactly stream 0 over
a `USER_MEMORY_CALL` vertex backing. Indexed user-memory draws and every
surface-backed draw stop at the unchanged B6G2 boundary, because only
call-scoped user memory is valid for the synchronous DDI call and therefore
needs no surface generation revalidation.

An eligible draw is captured into a single allocator-owned, pointer-free,
self-describing blob of at most 1 MiB holding the fixed header, the exact
transient draw records, and the exact stream-0 user-memory bytes at a 16-byte
aligned payload offset under an FNV-style content hash. Both source ranges are
arithmetic-validated before anything is allocated, secured, or copied; each is
then secured with `EngSecureMem`, copied through `qxp_dd_copy`, and unsecured in
reverse order. B6G3A Build+Validate then runs over the blob-owned records, and
the draw is still rejected before transport. Statuses 20/21/22
(`DRAW_CAPTURE`, `DRAW_CAPTURE_RESOURCE`, `DRAW_ACCESS`) were appended without
renumbering; only 21 maps to `QXP_DDERR_CURRENTLYNOTAVAIL`, and the gate
destroys prepared state before returning it.

Failure classification is deliberately conservative. XP documents only that
`EngSecureMem` returns NULL on failure, and it wraps `MmSecureVirtualMemory`,
which also returns NULL for a kernel or invalid range; after the checked range
arithmetic performed here, NULL most plausibly names an inaccessible caller
range rather than resource exhaustion. Allocation failure is therefore the only
status reported as a transient resource failure, and both secure statuses are
semantic rejections kept distinct only to diagnose which range failed. A
nonzero, representable record byte count that wraps the address space is
rejected as appended `RECORD_RANGE` (19) before a single record byte is read;
zero and unrepresentable counts keep their established `RECORD_COUNT`
classification and an ineligible opcode keeps `OPCODE`.

The user-memory envelope/base arithmetic and the capture activation rule are
exported helpers called by `directdraw.c`, so the oracle exercises exactly one
implementation rather than a restatement. Tests cover envelope guards
(null pointer, zero size/length, size×length overflow, offset overflow,
base+bytes overflow, exact valid base and count), the activation rule across
all four eligible opcodes, both indexed opcodes, surface-backed stream 0,
multi-stream and unpopulated masks, and null snapshots, plus allocation-only
resource classification and wrapping-range rejection with zero callback
invocations. The production dispatcher test pins statuses 20/21/22 to their
exact values and their outer `CURRENTLYNOTAVAIL` / `INVALIDPARAMS` mappings at
the exact outer command offset with zero transport, frame, and batch effects.

R1 `SM2-B6G3B1-LIVE-UM-CAPTURE-R1` rejected the first implementation for
reporting secure failure as a resource failure and for testing only synthetic
helpers; `37651a0`, `1841189`, and `64797f8` closed both, and R2
`SM2-B6G3B1-LIVE-UM-CAPTURE-R2` approved with a single non-blocking comment
correction, applied in `64797f8`. Strict C89, full preinstall, WDK/package/
sidecar/ChkINF/ISO, and a fresh signed seven-mode XP T2 pass with clean
handback. Evidence:
`artifacts/runs/20260724-sm2-b6g3b1-live-um-capture-t2/`.

B6G3B1 does not execute, transport, or render anything, and it captures no
surface-backed or indexed source. Its live path is also not exercised by the
acceptance cycle: `GETDDIVERSION` still declines DDI v4, so the stock runtime
never creates an SM2 shader and the D3D9-only draw gate never fires on the VM.
The seven-mode T2 pass therefore proves the slice is inert with respect to every
shipping path — unchanged counters, ownership, presentation, and teardown — not
that capture ran under XP. Only the portable oracle proves capture behavior
today.

#### Phase B6G3B2 — provenance-safe surface-backed capture (planned)

This phase is designed but not implemented. It is recorded here so the lifetime
reasoning is reviewable before any code is written.

**Correction to the earlier plan.** Indexed user-memory draws cannot ship as a
lifetime-free slice ahead of the surface work. `D3DNTHAL_DRAWPRIMITIVES2DATA`
contains exactly one user pointer, `lpVertices`, and no index pointer
(`d3dnthal.h:878-905`), while `D3DHAL_DP2DRAWINDEXEDPRIMITIVE2.StartIndexOffset`
is explicitly an offset *in the index buffer* (`d3dhal.h:1353-1364`). Both
indexed opcodes therefore always consume the surface-backed `SETINDICES`
binding, so indexed capture strictly depends on the lifetime mechanism.

**Why a mechanism is needed at all.** `qxp_d3d_resolve_surface_source` finds a
surface under `qxp_d3d_table_lock` but returns its address for use after the
lock is released. Post-copy revalidation detects a race only after the fact; it
cannot make freed storage safe to dereference. The paths that can free,
replace, or invalidate that storage are `QxpDdDestroySurface`,
`QxpDdDestroyDDLocal` via `QxpD3d9TeardownDrawBackingOwner`, context
destroy/destroy-all, `CreateSurfaceEx` wrapper replacement, `QxpDdLock` /
`QxpDdUnlock`, CPU and accelerated Blt writes, `DrvDisableDirectDraw`,
`DrvDisableSurface` (which unmaps `pdev->framebuffer`), `DrvAssertMode`, and
`DrvDisablePDEV`.

**Mechanism: a bounded spin drain, not a scheduler wait.** The driver already
serializes destruction against in-flight draws: `draw_batch_lock` is a per-
context `xchg` spin lock held across an entire DP2 call, and
`QxpDdDestroySurface` already spin-waits on it through
`qxp_d3d_ensure_cpu_coherent` — across a full host readback round-trip, which is
far longer than any capture. B6G3B2 therefore extends that proven discipline to
a driver-global capture/mutation drain rather than introducing
`EngCreateEvent` / `EngWaitForSingleObject`. This deliberately avoids claiming
PASSIVE_LEVEL waitability for every DirectDraw callback, which the WDK does not
document and which we have no checked-XP evidence for. A capture reader holds
the gate only for a bounded, wait-free, at most 1 MiB copy, so the spin is
bounded by construction.

Admission is nonblocking for the draw path: a pending or active mutation makes
capture admission fail immediately and the draw falls back to the unchanged
B6G2 reject, so DP2 never waits and the hard-won batch budget is unaffected.
Mutations get priority so captures cannot starve teardown. A nonwrapping
gate/aperture epoch closes capture permanently on exhaustion rather than
permitting ABA.

**Revalidation contract.** Generation and incarnation alone are insufficient:
`CreateSurfaceEx` replaces `entry->surface` in place while deliberately
preserving both (`directdraw.c:7923-7960`). After the copy and before sealing,
every required stream and the index source must re-prove full backing owner
(DD-local, context, process, device), owner-scoped handle, incarnation, content
generation, backing kind, byte extent, video-memory offset, role, stream,
stride/source offset/divider/span, `host_write_epoch`, CPU/host authority,
`lock_active`, pending target-written state, the gate epoch, and the transient
base address — which is compared and then discarded, never stored in the blob.
Capture declines host-authoritative or locked sources outright instead of
calling `qxp_d3d_ensure_cpu_coherent`, because DP2 already holds
`draw_batch_lock`.

**Lock discipline.** Validate, allocate, and stage outside all locks; acquire
the capture reader nonblockingly; take `qxp_d3d_table_lock` only for lookup,
validation, and publication; secure, copy, parse, hash, and free strictly
outside it; never spin on the gate while holding the table lock. Mutation paths
allocate scratch first, announce the mutation, drain readers with no table or
batch lock held, take the table lock for detach, release it, then perform host
deletion and frees, and finally leave the gate.

**Status codes.** Append after `RECORD_RANGE = 19` without renumbering:
`ERROR_LIFETIME`, `ERROR_SURFACE_SOURCE`, `ERROR_SURFACE_SECURE`, and
`ERROR_REVALIDATION`, with matching appended fields. Every one of them is a
semantic rejection; allocation failure remains the only transient-resource
claim, and the outer 20/21/22 values and mappings are unchanged.

**Oracle.** Extend the portable strict-C89 capture test with a scalar fake
surface table driving the same exported gate helpers `directdraw.c` calls, so
one implementation is under test. Concurrency becomes deterministic by
interleaving explicit transitions rather than threads: a reader is admitted, a
destroy request enters the production wait transition, and the test asserts no
detach, clear, unmap, or reuse occurred while the reader existed. Further cases
cover admission refusal under pending mutation, mutation between snapshot and
admission, injected metadata mutation producing `REVALIDATION` with no blob,
locked/host-authoritative/closed-aperture refusal, table-lock depth zero at
every callback, and index-versus-vertex byte provenance.

**Status.** The gate state machine itself is now implemented and production-
linked but dormant, in `qxp_d3d9_draw_lifetime.[ch]`, with no live caller and no
change to `directdraw.c`. A zero-initialized gate is open at epoch 1;
`epoch_minus_one` keeps that initial state byte-zero while every reader-visible
epoch stays nonzero. Admission takes the injected lock exactly once and never
yields, so the draw path cannot wait. `BeginMutation` publishes pending state,
releases the lock, and then drains, re-taking the lock only to test the reader
count and always releasing it before each yield, so the spin never holds the
lock. Mutations are **counted rather than exclusive**: exactly two sites clear
surface-table entries, `QxpDdDestroySurface` and the owner teardown reached from
`QxpDdDestroyDDLocal`, and two owners can reach them at the same time. Refusing
an overlapping mutation would let that second teardown proceed without ever
draining outstanding readers, so counting is a correctness requirement, not a
convenience. Readers stay refused until the last mutation ends, and counting
keeps `EndMutation` unambiguous without an owner pointer or an allocated token.
Reopen advances strictly; exhausting
`ULONG_MAX` closes the gate permanently rather than wrapping or reusing a value.
Release stays valid across a close/reopen cycle so an old reader can always
drain the count, while `RevalidateReader` reports the epoch mismatch.

**Mutation boundary inventory.** Exactly two sites clear surface-table entries,
and both are now wired: `QxpDdDestroySurface` and the owner teardown reached
from `QxpDdDestroyDDLocal`. Three paths the original audit listed are
deliberately *not* drain boundaries. `QxpDdLock` and `QxpDdUnlock` change
content and authority but never free storage, so they are covered by
revalidation instead, which keeps a hot path off the gate. `CreateSurfaceEx`
replaces the wrapper while preserving the live resource, so it likewise frees
nothing and is a revalidation concern. Context destroy clears context state, not
surface storage. The framebuffer unmap paths in `framebuffer.c`
(`DrvDisableSurface`, `DrvAssertMode`, `DrvDisablePDEV`) matter only for
video-memory backings, which the first reader slice excludes.

#### Execution plan: the host half already exists

An earlier assessment in this document assumed SM2 execution required a large
paired QEMU/host implementation of GLSL compile, link and cache. Inspecting the
code shows that is **wrong**, and the correction materially shrinks the
remaining work.

The miniport already drives a complete GLSL pipeline through the mesa FIFO. The
vertex-clip path in `qxpmp.c` calls `glCreateShader`, `glShaderSource`,
`glCompileShader`, checks `GL_COMPILE_STATUS` and dumps the info log on failure,
then `glCreateProgram`, `glAttachShader`, binds `qxpV0..qxpV15` attribute
locations, links, and caches uniform locations. The FIFO already defines
`CREATE_SHADER`, `ATTACH_SHADER`, `LINK_PROGRAM`, `USE_PROGRAM`,
`GET_UNIFORM_LOCATION`, `UNIFORM_1I` and `UNIFORM_4FV`.

On the QEMU side, `hw/mesa/mglfptbl.h` is a generic function-pointer table over
the whole GL API, including `glCreateShader` and `glLinkProgram`; the FIFO enum
is simply an index into it. Nothing whitelists a D3D-specific subset. The clip
path already proves this works end to end in a live guest.

Consequently SM2 execution is expected to need **no paired QEMU change at all**.
`GL_FRAGMENT_SHADER` (0x8b30) is merely a constant the miniport does not yet
define; it is passed as an argument, not as a dispatch selector. That claim will
be treated as provisional until the first SM2 program actually links in a live
guest.

What genuinely remains is guest-side, and does require an ABI bump:

- A shader type that carries a linked `vs_2_0` + `ps_2_0` GLSL pair, since the
  existing vertex and pixel defines target ARB assembly programs
  (`glProgramStringARB`), which cannot express Shader Model 2.
- A larger pixel source limit. `QXP_D3D_MAX_PIXEL_SHADER_SOURCE_BYTES` is 8192
  and the generated `ps_2_0` coverage shader is already 8191 bytes, so real
  shaders will not fit. The `vs_2_0` coverage shader is 12341 bytes against a
  64 KiB vertex limit, which is comfortable.
- Larger constant files: `vs_2_0` has 256 float4 constants and `ps_2_0` has 32,
  against the current `QXP_D3D_VERTEX_CONSTANT_COUNT` of 96 and
  `QXP_D3D_PIXEL_CONSTANT_COUNT` of 8.
- Miniport support to compile and link the pair, bind the same `qxpV*`
  attributes, cache uniform locations for the constant arrays and samplers, and
  own the program cache lifetime.
- Display-driver emission of that define from the linked SM2 snapshot, then the
  draw itself.

#### Confirmed: SM2 pixel shaders sample textures

Run `artifacts/runs/20260725-sm2-texture-selftest-t2-r2/` (escape `0x1105`)
sampled a 5x5 texture from a `ps_2_0` shader and matched two different texels
exactly:

```
expected_sample0=0xff459709 observed_sample0=0xff459709   /* texel (1,3) */
expected_sample1=0xff8f3d11 observed_sample1=0xff8f3d11   /* texel (3,1) */
```

Checking two target pixels that map to *different* texels is what makes this
oracle meaningful: a U/V transpose swaps the two colours, a V flip selects
(1,1) and (3,3), and an off-by-one lands on another of the 25 unique texels. A
single sample, or a uniformly coloured texture, would have proved nothing about
orientation. Sampling is point filtered with clamp addressing and no mipmaps,
and the coordinates land on exact texel centres, half a texel from any boundary.
The source is non-power-of-two, so NPOT sampling is covered too.

**Contract detail worth remembering.** The first attempt failed with
`draw_succeeded=0` and `QXPMP D3D DRAW TRANSPORT-`, because
`qxp_packet_view_draw` requires every `texture_coord_dimensions[]` entry to be
zero for a programmable draw. The FVF-era per-stage coordinate width and the
programmable vertex declaration are mutually exclusive descriptions of the same
property, and the packet validator enforces that. Setting both is rejected
before any GL work occurs.

#### Confirmed: SM2 shaders execute and render correct pixels

Run `artifacts/runs/20260725-sm2-draw-selftest-t2/` (ABI 55) is the first
Shader Model 2 execution in this project. Escape `0x1104` linked a pair,
uploaded float constants through the new constants IOCTL, bound the program with
`glUseProgram`, issued a real draw, and read the result back:

```
draw_succeeded=1 readback_succeeded=1
expected_inside=0xff2060df  observed_inside=0xff2060df
expected_outside=0xff102040 observed_outside=0xff102040
```

The oracle is two-sided. The inside sample can only take the value
`0xff2060df` by the pixel shader reading the uploaded constant
`(0.125, 0.375, 0.875, 1.0)`; the outside sample must stay at the clear colour.
A missing draw fails the inside sample, an overdraw fails the outside sample,
and an inverted Y sign fails both because the mirrored triangle swaps which
sample it covers. Both matched exactly, so float-to-unorm8 is round-to-nearest.

SM2 constants are uploaded out of band and are sticky in the GL program, which
matches GL uniform semantics and avoids inlining 256 vertex and 32 pixel float4
registers into every draw packet. `sm2_program_name` in the draw request selects
the linked pair; zero preserves the legacy ARB and fixed-function paths exactly
through a single early branch in the miniport draw entry.

`qxpVsClipControl` is now cached and set by the SM2 path, carrying the same
D3D-to-host Y sign and clip-z conversion the existing GLSL clip path uses.

This remains an out-of-band escape rather than the shipping path: DDI v4 and SM2
caps are still declined, so no application can reach it.

#### Confirmed: SM2 GLSL links on the real GPU

The provisional claim above is now settled by live evidence. Run
`artifacts/runs/20260724-sm2-link-selftest-t2/` reports

```
sm2_stage=8 sm2_vs_decode=0 sm2_ps_decode=0 sm2_vs_bytes=2332
sm2_ps_bytes=894 sm2_defined=1 sm2_deleted=1 sm2_ok=1
```

Escape `0x1103` drove the production chain inside the guest: decode of embedded
minimal `vs_2_0` and `ps_2_0` bytecode, GLSL emission by the production
emitters, the ABI 54 define IOCTL, and miniport compile, attribute binding and
link. `sm2_defined=1` is a GPU fact because the miniport reads `GL_LINK_STATUS`
after `glLinkProgram` and fails the IOCTL, after dumping the program info log,
when the link did not succeed. **No paired QEMU change was needed**, exactly as
predicted from the generic function-pointer table.

Uniform locations are looked up with an *optional* helper, since a GLSL compiler
legitimately eliminates constants and samplers that a given shader never reads;
a missing location is therefore not a link failure.

Still unproven: nothing binds these programs. There is no `glUseProgram`, no
constant or sampler upload, and no draw, so no SM2 shader has yet *run*.

#### Live execution evidence (the `D3d9SelfTest` mode)

Everything above is unreachable in a shipping configuration: with DDI-v4/SM2
capabilities declined the stock runtime never creates the shader objects a
capture-eligible draw requires, so the draw gate never opens. For a long stretch
that meant none of this code had ever executed, and every acceptance pass proved
only inertness.

The `D3d9SelfTest` acceptance mode closes that gap without exposing any
capability. The guest probe `qxpd3d9selftest.exe` creates a Direct3D 9 device
and a `D3DPOOL_SYSTEMMEM` vertex buffer, fills it with a deterministic pattern,
and calls `ExtEscape` with escape `0x1102`, passing the byte count and the
FNV-1a checksum it expects. `DrvEscape` routes that to `QxpD3d9SourceSelfTest`
in `directdraw.c`, which runs the **real production** guarded transaction over
the calling process's registered system-memory vertex buffers and reports how
many hashed to the caller's checksum.

The self-test is a strict observer: no draw, no host submission, no transport
traffic, no capability change, no state mutation. It collects candidate handles
under `qxp_d3d_table_lock`, releases the lock, and only then runs each
transaction, because that lock is not recursive and the source module takes it
internally through injected callbacks.

First passing evidence, from
`artifacts/runs/20260724-sm2-source-selftest-match-t2/`:

```text
examined=6 admitted=6 refused=0 revalidated=6 released=6 readers=0
matches=1 match_handle=0x8 bytes=256 checksum_match=ok
```

`readers=0` proves the gate drained, so no reader was leaked — the failure that
would hang surface teardown. `matches=1` proves the bytes read at the resolved
base hashed exactly to the pattern the probe wrote into its own buffer, which is
direct evidence that the resolver produces a correct base address and extent for
a real surface. That is the property whose failure mode is a kernel-mode
out-of-bounds read.

A first attempt reported `checksum_match=BAD`. The **oracle** was wrong, not the
driver: the probe had assumed the driver's first examined surface was its own
buffer, but the runtime creates several system-memory vertex buffers per
process. The comparison was inverted rather than weakened — the probe now
supplies the checksum to search for and the driver reports matches — which is
strictly stronger and cannot be satisfied by accident. The driver was not
changed to make the test pass.

This still does not prove SM2 draws render. It proves the lifetime gate and the
surface resolver behave correctly in a live guest.

**Two exact source contracts, one implementation.** The capture module now
accepts either a stream-0 `KIND_USER_MEMORY_CALL` block or a
`KIND_SYSTEM_LINEAR` surface, threading an explicit expected source kind through
one shared validation core rather than forking it.
`QxpD3d9CaptureNonindexedSurfaceDraw` and `QxpD3d9DrawCaptureIsSurfaceEligible`
sit beside the unchanged user-memory entry points, and
`QxpD3d9ValidateCapturedDraw` dispatches on the kind recorded inside the blob so
it still revalidates from its own bytes. The user-memory contract keeps its
exact zero identity, and the surface contract requires the opposite: nonzero
handle, incarnation, and content generation, with `user_memory` false and the
prepared handle equal to the backing handle. No status code was appended; the
maximum stays `RECORD_RANGE = 19`.

One honesty note recorded from mutation testing: those per-kind identity checks
cannot be pinned by any oracle through the public entry points, because
`QxpD3d9BuildDrawEnvelope` already refuses a prepared/backing pair whose
provenance and identity disagree, so the B6G2 envelope rejects such a draw
first. Deleting the checks changes no observable outcome. They are kept as
defence in depth and labelled as such in the source, and the tests that appear
to cover them in fact pin the envelope. Claiming otherwise would overstate the
evidence.

**Guarded source acquisition.** `qxp_d3d9_draw_source.[ch]` is the gate's first
reader and ties admission, address resolution, and identity revalidation into
one auditable transaction. `QxpD3d9AcquireDrawSource` admits a reader
nonblockingly, then resolves the current source under the injected lock through
a caller-supplied callback; `QxpD3d9RevalidateDrawSource` re-resolves after the
byte copy and requires an identical observation plus an unchanged reader epoch;
`QxpD3d9ReleaseDrawSource` releases exactly once. The observation deliberately
includes `surface_identity`, the opaque `PDD_SURFACE_LOCAL` wrapper, because
`CreateSurfaceEx` swaps that wrapper while preserving incarnation and content
generation, so those two alone cannot detect it. Only `KIND_SYSTEM_LINEAR` is
accepted; `KIND_VIDEO_LINEAR` has its own distinct rejection status because
video-memory backings additionally require the `framebuffer.c` unmap paths to
become drain boundaries. Host-authoritative and locked sources are refused
outright rather than made coherent, since the DP2 path already holds a draw
batch lock.

The module's single most important property is that the reader is released
exactly once on **every** post-admission failure path: a leaked reader would
make the drain spin forever and hang surface teardown, which is far worse than a
declined capture. That property, and the wrapper-swap detection, were each
confirmed to be genuinely covered by deliberately breaking the implementation
and observing the specific oracles fail.

**Slices.** B6G3B2a-1 landed the gate wiring with no reader admitted anywhere,
proving inertness and the absence of teardown deadlock before any capture
depends on it. B6G3B2a-2 admits readers for nonindexed, stream-0-only,
CPU-authoritative, unlocked, **system-memory** (`KIND_SYSTEM_LINEAR`) surface
backing, so no framebuffer unmap path is involved. B6G3B2b generalizes to
video-memory backings, multi-stream mixed user-memory/surface sources, and both
indexed opcodes with index-first B6G3A planning. None claims textures, samplers,
targets, transport, execution, or cap exposure, and all still reject before
transport.

Stock D3D9 still cannot execute this path because `GETDDIVERSION` declines DDI
v4. Draws do not consume linked D3D9 stages or lowering, public caps remain
unchanged, and no shader/game-support claim is made.

- Remaining order: (1) capture exact stream/index/UM bytes under safe backing
  lifetime and revalidate owner/incarnation/generation; (2) linked-stage
  transport/cache lifetime and execution;
  (3) samplers/textures, targets/depth, draws, Reset and teardown; (4) caps/DDI
  and stock-runtime tests.
- Transport: the current baseline is ABI 53. A future paired display/miniport
  bump must add GLSL stage/source kinds, PS 8→32 / VS 96→256 float4 constants,
  4→16 samplers, dirty masks, and both stage IDs without enlarging the legacy
  D3D8 fast path blindly.
- Host: GLSL stage compile + a linked VS/PS pair cache (D3D binds stages
  independently; classic GLSL programs are monolithic). The host GLSL
  compile path already exists (`mesapt_mm.c` reconstructs shader-source
  pointers and calls real host GL).
- Only then raise `*_ShaderVersion` to SM2.0.
- Gate: T2 correctness + an SM2.0 title renders correctly.
- Target: **F.E.A.R. demo** (already installed on the FEAR line; has a
  deterministic built-in Performance Test for repeatable correctness and
  later perf).

### Phase C — full DDI surface

- Report `DX9_DDI_VERSION`; enable 8 blend stages, MRT as implemented,
  queries as required.
- Gate: T2 correctness + a representative 2004-2008 D3D9 title renders.
- Target: a mainstream 2005-2006 D3D9 title from the user's library.

## Validation plan

- Every execution-bearing slice gets a fresh signed package with attributable
  DriverVer, signer, and file hashes plus the full T2 correctness cycle
  (Install, Boot, D3dVisual, D3d9, D3dVs11, D3dVsMultiContext,
  D3dVsSuccession, and D3d9SelfTest). Never reuse package bytes across source
  changes; bump DriverVer when the shipping package version advances.
- A stock-runtime, capability-exposure, presentation, or game-visible slice
  also requires a real-game render check on its phase target through the
  existing preflight (no per-game DLL wrappers).
- A `DrvEscape`-only diagnostic oracle extension that changes no normal runtime,
  capability, transport, presentation, or game-visible path does not repeat a
  manual game baseline. The four-sampler v9 slice is in this category and used
  automated T0 plus the full fresh T2 matrix instead.
- Use a matched host/core/workload; capture-free performance comes only after
  correctness and is never the gate.

## Representative milestone titles

| Title | Runtime | Shader | Phase | Note |
|---|---|---|---|---|
| Age of Empires II | DirectDraw 2D | none | (done) | runs today |
| Max Payne | D3D8 | SM1.x | (done) | renders; perf open |
| UT2003 | D3D8/9-rt | SM1.x | (done) | renders |
| Prince of Persia: Sands of Time | D3D9 | ps_1.1 | A | renders internally; platform/multi-stream/Ti4600 work complete, missing GUI remains pre-DDI |
| Knights of the Old Republic | OpenGL | n/a | — | OpenGL path, not D3D9 |
| F.E.A.R. demo | D3D9 | SM2.0 | B | installed; Perf Test wired |
| (2005-2006 mainstream) | D3D9 | SM2.0 | C | broad library unlock |

## Non-goals / constraints

- No per-game DLL shims, wrappers, or private runtimes; stock XP D3D9 only.
- No strengthening of the `GD9 FALLBACK` path as a substitute for support.
- Do not report `DX9_DDI_VERSION` before the matching features are honest —
  advertising ahead of implementation is how we got the black-screen
  half-state in the first place.
- Preserve stock XP runtimes, exact rendering, Reset, ownership/lifetime/
  teardown, and the single-writer boundaries on `directdraw.c` / `qxpmp.c`.

## Shader Model 2 evidence tiers (what is actually proven)

Earlier working notes listed cube, volume, projected and biased fetch and
multiple simultaneous samplers as "untested". That overstated the gap and
invited redundant work: all were compile-proven first, and cube, volume,
projected, biased 2D, and the full four-sampler 2D path have since advanced to
exact-pixel Tier 2. The distinction matters, so record the tier rather than a
single pass/fail.

**Tier 1 - lowered and compiled on a real GL driver.** The pixel coverage
shader is emitted from the production decoder and lowering path
(`qxp_pshader_glsl_test.exe --emit-glsl`) and then compiled and linked by the
host GL driver via `qxp_glsl_compat_test.exe --ps20-glsl` and `--sm20-pair`,
both wired into `make -C guest/xpdm preinstall-test`. That shader declares nine
samplers, covering all three sampler types (`D3DSTT_2D`, `D3DSTT_CUBE`,
`D3DSTT_VOLUME`) crossed with all three fetch controls (plain,
`D3DSI_TEXLD_PROJECT`, `D3DSI_TEXLD_BIAS`), alongside the arithmetic and
flow-control opcode set. Broader sampler/control combinations therefore retain
real-driver compile evidence even where no exact-pixel execution oracle exists.

**Tier 2 - executed on the real GPU with an exact-pixel oracle.** The
`DrvEscape` self-tests cover source match (`0x1102`), link (`0x1103`), draw
(`0x1104`), and ten texture phases in `0x1105`: single 2D sampling, two
simultaneous samplers, projected fetch, cube fetch, depth testing, scissor,
stream-source offset selection, biased 2D fetch, four simultaneous 2D samplers,
and volume fetch. They assert exact ARGB values and use controls that reject
missing draws, clipping, last-draw-wins, ignored offsets/bias/depth, slice-zero
fallback, and missing or swapped higher sampler bindings.

**Not proven at Tier 2.** Render targets beyond the single bound target; SM2
Reset; stock-runtime video-memory-backed and indexed SM2 draws.

> The per-phase sections further down are dated records of the state when each
> phase landed, so their "still not proven" lists are historical snapshots and
> go stale. The authoritative current position is the matrix at the end of this
> document.

Tier 1 is real evidence that the lowering is well-formed, and it is much cheaper
to extend than Tier 2, but it cannot catch a swizzle, coordinate-space or
texel-addressing error. Only Tier 2 can. Any claim that a fetch mode "works"
must state which tier it means.

**Reachability is separate from both tiers.** Every Tier 2 result is obtained
through `DrvEscape`, which bypasses the Direct3D 9 runtime entirely. No
unmodified application can reach any of this until the DDI v4 blocker in
`d3d9-ddi-v4-runtime-contract.md` is resolved, so none of the above is a claim
of shipping Shader Model 2 support.

## Multiple simultaneous samplers: proven (Tier 2)

Two simultaneous texture samplers now execute on the real GPU against an
exact-pixel oracle, so multitexturing moves from Tier 1 to Tier 2. Run
`20260725-sm2-multisampler-t2-r3`, full eight-mode cycle pass:

```
stage=sm2_texture ... stage=33 ... overall=1
  multi_vertex_glsl_bytes=2704 multi_pixel_glsl_bytes=1388
  multi_define_succeeded=1 multi_draw_succeeded=1 multi_readback_succeeded=1
  expected_multi0=0xff55c749 observed_multi0=0xff55c749
  expected_multi1=0xffbf4d51 observed_multi1=0xffbf4d51
  multi_overall=1 ok=ok
```

The phase-1 single-sampler assertions are byte-identical (`0xff459709`,
`0xff8f3d11`), so the existing proof did not move. `D3d9SelfTest` counters go
from `2/2/0/0/0` to `3/3/0/0/0`, the one added draw.

The test declares two samplers in `ps_2_0`, samples sampler 0 with `t0` and
sampler 1 with `t1`, adds the results and writes the sum. The second texture is
`red = x * 0x10`, `green = y * 0x10`, `blue = 0x40`, alpha 0, so the expected
output is the phase-1 texel plus the second texel with alpha saturating. Both
sums landed exactly, with no floating-point drift.

### The contract this pinned down

Getting here took two failed attempts, and the rule they established is worth
keeping visible because nothing in the DDK headers states it:

**sampler N is bound to coordinate stage N, which requires vertex output
TEXCOORD N.**

Two miniport rules combine to enforce it, and the driver is correct in both:

1. `qxpmp.c:8708` - `pixel_sampled_stages & ~pixel_coordinate_stages` must be
   zero, so a sampled stage must also carry coordinates.
2. `qxp_packet.c:168-183` (`qxp_packet_program_texture_contract`) - every set
   coordinate stage `N` requires `written_outputs` to contain
   `QXP_D3D_VS_OUTPUT_TEXCOORD0 << N`.

The first attempt had both samplers read `t0` and was refused; widening
`pixel_sampled_stages` alone was refused again by the second rule. The fix is a
vertex shader that writes `oT1`.

**TEXCOORD1 comes from the shader, not from the vertex buffer.** Because the
vertex shader writes `oT1` from the same input register as `oT0`, the vertex
layout is unchanged from phase 1: still six floats, two elements, one draw
size. An intermediate version added a third vertex element and a wider draw
request, which was both misleading (the shader never read that element) and
risky, since the draw size also sizes the workspace allocation. It was removed.

Pixel and vertex GLSL varyings are named `qxpPsInT<index>` on both sides and
link by register index rather than semantic, which is why `oT1` reaches `t1`.

### Still not proven at Tier 2

Exact-pixel correctness for cube, volume, projected and biased fetch; more than
two simultaneous samplers; render targets beyond the single bound target;
depth; `Reset`; video-memory backings; indexed draws.

Reachability is unchanged: this runs through `DrvEscape`, which bypasses the
Direct3D 9 runtime, so it is not a claim of shipping Shader Model 2 support
while the DDI v4 blocker stands.

## Projected fetch: proven (Tier 2)

Projected texture fetch (`texldp`, `D3DSI_TEXLD_PROJECT`) now executes on the
real GPU against an exact-pixel oracle. Run `20260725-sm2-projected-t2-r3`,
full eight-mode cycle pass:

```
stage=sm2_texture ... stage=45 ... overall=1 ... multi_overall=1
  proj_vertex_glsl_bytes=2574 proj_pixel_glsl_bytes=1102
  proj_define_succeeded=1 proj_constants_succeeded=1
  proj_draw_succeeded=1 proj_readback_succeeded=1
  expected_proj0=0xff459709 observed_proj0=0xff459709
  expected_proj1=0xff8f3d11 observed_proj1=0xff8f3d11
  proj_overall=1 ok=ok
```

The vertex shader computes `oT0 = v1 * c0` with `c0 = (2, 2, 0, 2)`, so the
coordinate arrives as `(2u, 2v, 0, 2)`. `texldp` divides xy by w, recovering
`(u, v)` — phase 1's coordinate. The expected pixels are therefore deliberately
identical to phase 1's, and that equality is the proof: without the projective
divide the sample lands on `(2u, 2v)`, a different texel with a different
colour. Phases 1 and 2 are unchanged. `D3d9SelfTest` counters move from
`3/3/0/0/0` to `4/4/0/0/0` for the one added draw.

### The trap: `def` constants are metadata, not code

The first attempt used a `def c0, 2, 2, 0, 2` instruction in the vertex shader
and failed with both sample points reading exactly texel `(0,0)`.

**In this codebase `def` does not emit an inline GLSL constant.**
`qxp_vshader20_glsl.c:1679` records the immediate values into
`metadata->float_definition_mask` and `metadata->float_definition_values` and
emits nothing; it is the caller's job to upload them. Nothing did, so `c0` was
zero, `oT0` was zero, and the projective divide was degenerate.

The fix is to drop `def` and supply the constant through the constants
transport, with `vertex_start_register = 0` and `vertex_register_count = 1`,
exactly as the `sm2_draw` self-test does at `directdraw.c:8708-8726`. Any future
shader that relies on a `def`-declared constant must upload it the same way.

The pixel lowering was never at fault: `qxp_pshader_glsl.c:585` emits
`/ qxpArg0.w`, which is the correct Direct3D semantic. GLSL's three-component
`textureProj` would divide by `.z`, and that is not what is generated.

### Why Tier 1 could not have caught this

Phase 1's pixel shader consumes only `.xy` of its coordinate, so nothing it
asserts depends on `.z` or `.w`, and the coverage shader only has to *compile*.
A degenerate w produces perfectly valid GLSL. Only an exact-pixel execution
oracle distinguishes a correct projective divide from a divide by zero, which
is the concrete case for keeping the two evidence tiers separate.

### Still not proven at Tier 2

Exact-pixel correctness for cube, volume and biased fetch; more than two
simultaneous samplers; render targets beyond the single bound target; depth;
`Reset`; video-memory backings; indexed draws.

Reachability is unchanged: all of this runs through `DrvEscape`, which bypasses
the Direct3D 9 runtime, so it is not a claim of shipping Shader Model 2 support
while the DDI v4 blocker stands.

## Cube fetch: proven on the GPU, at the cost of three constraints

Cube texture fetch is now **Tier 2 proven**: a `samplerCUBE` fetch executes on
the host GPU and both sampled pixels match their expected value exactly. It took
five cycles and cost three separate constraints, none of them documented
anywhere, so all three are recorded here.

Progress by internal stage: **47 → 48 → 55 → 60 (complete)**.

### Constraint 1: a cube texture is defined per face, not once

`stage=47` (upload). The miniport records `defined_levels` **per face**
(`qxpmp.c:2397`, set by the DEFINE handler) and the upload path refuses any
face/level pair that was never defined (`qxpmp.c:2440`). Defining the texture
once therefore lets face 0 upload and rejects the other five.

`QXP_D3D_TEXTURE_DEFINE_REQUEST` carries a `face` field for exactly this
reason. Issue the define six times, once per face, before uploading. No ABI
change is needed; the raw-upload request already carries `face` too.

### Constraint 2: vs_2_0 constant reads must name the same register

`stage=48` (vertex decode). The shader used `mad oT0, v1, c1, c0`, reading two
different constant registers in one instruction. The decoder enforces the
vs_2_0 read-port rule at `qxp_shader_decode.c:830-840`: every constant source
in an instruction must be the *identical* register, not merely the same file.

Rewriting as `mad oT0, v1, c0, c0` satisfies it and still produces the wanted
constant: with `c0 = (0,0,1,0)` and a `FLOAT2` texcoord expanding to
`(u,v,0,1)`, the product is `(0,0,0,0)` and the sum is exactly the +Z
direction. Only one constant register then needs uploading.

### Constraint 3: the oracle's terminal stage must advance with the phase

`stage=55` (cube draw) refused, then — after the draw validation was
instrumented — a run where **every cube value was already correct** still
reported `ok=BAD`.

The self-test oracle in `guest/acceptance/qxpd3d9selftest.c` requires the
reported stage to equal the terminal stage. Adding a phase moves the terminal
stage, and the check was still pinned to the previous phase's completion
(`..._STAGE_PROJ_COMPLETE`, 45) while the driver now correctly finished at
`..._STAGE_CUBE_COMPLETE` (60). Every other term passed, so the failure looked
like a driver fault and was purely a harness oversight.

When adding a phase, advance that terminal-stage comparison. Note this is a
tightening, not a relaxation: the oracle demands the test reach the new, later
stage.

### What made the difference

The draw refusal at `stage=55` was diagnosed only after the SM2 draw validation
was instrumented. It previously refused a draw through one ~60-term
short-circuiting condition while logging nothing, so a refusal was invisible
except as a draw IOCTL that never became a logical draw. It now logs
`QXPMP D3D SM2 DRAW REJECT` with the failing clause index.

That instrument is what turned an open-ended search into a decisive run, and it
is the same silent-failure shape already corrected for `DrvGetDirectDrawInfo`,
`DrvEnableDirectDraw`, declined GUIDs and the self-test reporter.

### Proven values

Run `artifacts/runs/20260725-sm2-cube-t2-r5`, 8/8 modes pass, `rc=0`:
`version=4`, `stage=60`, `cube_define_succeeded=1`,
`cube_constants_succeeded=1`, `cube_draw_succeeded=1`,
`observed_cube0=0xff2060a0`, `observed_cube1=0xff2060a0`, `cube_overall=1`,
with the three earlier phases still `overall=1`, `multi_overall=1` and
`proj_overall=1`. D3d9SelfTest counters rise from 4/4/0/0/0 to **5/5/0/0/0**.

Tier 2 SM2 texture coverage is now: single sampler, two simultaneous samplers,
projected fetch, and cube fetch.

## Biased 2D fetch: proven (Tier 2, eighth texture phase)

Run `artifacts/runs/20260727-sm2-texldb-v8-t2-r1/` passes all eight T2 modes.
The texture self-test reports `version=8`, terminal `stage=140`, and a separate
`sm2_texldb` record with every define/upload/decode/emit/target/draw/readback
and deletion flag set.

The phase defines a complete A8R8G8B8 chain with uniform but distinct levels:
4x4 `0xffe04020`, 2x2 `0xff20e040`, and 1x1 `0xff40a0e0`. Both passes use the
same 15x15 target, point min/mag/mip filtering, clamp addressing, triangle,
FLOAT4 coordinate, and sample `(7,7)`. The coordinate gradient yields implicit
LOD 0. Plain `texld` therefore must observe level-0 red. The second linked
program changes only the fetch control to `D3DSI_TEXLD_BIAS`; `t0.w=2` must
select level-2 blue. Ignoring bias returns red, and an off-by-one selection
returns green, so neither can pass:

```
texldb_expected_control=0xffe04020 texldb_observed_control=0xffe04020
texldb_expected_biased=0xff40a0e0 texldb_observed_biased=0xff40a0e0
texldb_program_deleted=1 texldb_texture_deleted=1 texldb_overall=1
```

The result contract is append-only within diagnostic escape `0x1105`; transport
ABI 57 is byte-identical. Frozen review `D3D-SM2-TEXLDB-V8-R1` found no code,
ownership, sizing, cleanup, or evidence defect. The same cycle attributes the
unchanged `1.0.0.305` package, records 15 logical/single draws and 13 readbacks,
and passes base/process/lease/QMP/repository handback. Reachability remains
unchanged: this is diagnostic execution, not a shipping SM2 capability claim.

## Four simultaneous 2D samplers: proven (Tier 2, ninth texture phase)

Run `artifacts/runs/20260727-sm2-four-sampler-v9-t2-r1/` passes all eight T2
modes. The texture self-test reports `version=9`, terminal `stage=159`, and a
standalone `sm2_four_sampler` record with every define, upload, decode/emit,
target, clear, draw, readback, and deletion field set.

The phase reuses the established s0/s1 textures and defines two uniform 5x5
A8R8G8B8 textures: s2=`0x00201810` and s3=`0x00080402`. The unchanged
FLOAT4+FLOAT2 packet feeds a VS that writes TEXCOORD0..3; sampled and coordinate
masks are both `0xF`. The PS samples matching s0..s3 and computes
`(s0+s1+s2)-s3`. The subtract makes stage order observable, while exact output
makes either missing stage observable:

```
four_expected_sample0=0xff6ddb57 four_observed_sample0=0xff6ddb57
four_expected_sample1=0xffd7615f four_observed_sample1=0xffd7615f
four_program_deleted=1 four_texture2_deleted=1
four_texture3_deleted=1 four_overall=1
```

Both outputs differ from their two-sampler baselines (`0xff55c749` and
`0xffbf4d51`). The portable oracle additionally rejects a forged wrong expected/
observed pair and an exact two-sampler fallback. The complete appended guest
record is: texture2/texture3 define+upload `1`; VS/PS decode status `0`; GLSL
bytes `3048/1984`; program define, target, clear, draw, and readback `1`; both
exact expected/observed pairs above; program/texture2/texture3 deletion `1`; and
`four_overall=1`.

Sol S1 found one forgeable-expected gap and S1-R2 approved its hard-constant
fix. Sol S2 found one hidden-failure-stage gap and S2-R2 approved the preserved-
stage fix. S3 approved the guest gate; final review
`D3D-SM2-FOUR-SAMPLER-V9-R1` approved the integrated slice with no findings.
The T2 bundle records 16 logical/single draws, 14 readbacks, and ABI 57. Package
`07/22/2026,1.0.0.305`, signer
`24712EBA0E5E8C059CCA792F1A6BA463E8B0FC20`, and every package file remained
unchanged during the cycle. Handback passes preflight, base image, processes,
QMP 51419, lease, both repositories, package-directory identity, and each named
bundle: Install, Boot, D3dVisual, D3d9, D3dVs11, D3dVsMultiContext,
D3dVsSuccession, and D3d9SelfTest. The accepted r18 was not accessed. No manual
game test was required because this is diagnostic execution and changes no
normal runtime or game-visible path.

## Volume fetch: proven (Tier 2, tenth texture phase)

Transport ABI 58 adds an explicit volume resource type, define depth, upload
Z/depth, and draw-stage volume mask while requiring every 2D/cube producer to
send depth 1, Z 0, and a zero volume mask. The miniport validates target
identity and complete mip dimensions before FIFO work, uses `GL_TEXTURE_3D`
with exact `glTexImage3D`/`glTexSubImage3D` framing and tight unpack state,
disables/unbinds the two unselected texture targets, and permits a nonzero
volume mask only on the diagnostic SM2 path. Render targets remain 2D-only.

Escape `0x1105` version 10 appends stages 160-175 and 22 DWORDs without moving
a v9 field. The phase defines and uploads a 4x4x4 A8R8G8B8 volume in x/y/z
order, emits a true `dcl_volume s0` program, and draws one oversized triangle
with FLOAT4 coordinates. Point/clamp sampling at equal `(u,v)` but different Z
must produce:

```
volume_expected_sample0=0xff23500a volume_observed_sample0=0xff23500a
volume_expected_sample1=0xff835a2a volume_observed_sample1=0xff835a2a
volume_program_deleted=1 volume_texture_deleted=1 volume_overall=1
```

The oracle hardcodes both unequal pixels and rejects a slice-zero fallback.
Cleanup revalidates owner authority before IOCTLs, restores the old target
before deleting the program and texture, and preserves the first volume
failure stage. Terminal stage 175 publishes only after every old phase, exact
volume oracle, restoration, and deletion succeeds.

Canonical evidence is
`artifacts/runs/20260729-volume-v10-frozen-t2-r2/`: result size
864, version 10, stage 175, all nine T2 modes, and clean handback. The same signed `.305` package hashes pass final Warcraft III D3D8 and Max
Payne evidence at
`artifacts/runs/20260729-volume-v10-final-games-t3-r2/`. That run's UT summary
binds eight rich non-desktop direct-map/pre-join samples and an exact 675-frame
video, but those artifacts no longer qualify as normal-menu or joined-gameplay
evidence; the corrected verifier records `FAIL` in
`artifacts/temp/volume-v10-final-games-t3-r2-ut-corrected-verification-r5.json`.
The Warcraft summary binds eight samples, an exact 256-frame/30-fps video, and
a 256-character all-`V` cadence. Shipping DDI-v4, SM2/volume caps, and
`MaxStreams=1` remain unchanged.

## Authoritative Shader Model 2 coverage matrix

This supersedes the per-phase "still not proven" lists above, which are dated
snapshots. Every Tier 2 row names a run whose evidence file can be inspected.

### Proven on the GPU with exact-pixel oracles (Tier 2)

| Capability | Oracle | Evidence |
|---|---|---|
| `vs_2_0`/`ps_2_0` source match | escape `0x1102` | `stage=observed examined=5 admitted=5 refused=0` |
| Program link | escape `0x1103` | `sm2_vs_bytes=2332 sm2_ps_bytes=894 sm2_ok=1` |
| Programmable draw | escape `0x1104` | `observed_inside=0xff2060df observed_outside=0xff102040` |
| Single 2D sampler fetch | `0x1105` phase 1 | `0xff459709`, `0xff8f3d11` |
| Two simultaneous samplers | `0x1105` phase 2 | `0xff55c749`, `0xffbf4d51` |
| Projected fetch | `0x1105` phase 3 | `0xff459709`, `0xff8f3d11` |
| Cube fetch | `0x1105` phase 4 | `0xff2060a0` twice |
| Depth testing | `0x1105` phase 5 | NEAR in both draw orders; FAR-only control also exact |
| Scissor | `0x1105` phase 6 | excluded clear pixel in pass A, same pixel drawn after widening in pass B |
| Stream source offset | `0x1105` phase 7 | offsets 0/48 swap exact left/right draw colours |
| Biased 2D fetch | `0x1105` phase 8 | plain level-0 `0xffe04020`; biased level-2 `0xff40a0e0` |
| **Four simultaneous 2D samplers** | `0x1105` phase 9 | `(s0+s1+s2)-s3`: `0xff6ddb57`, `0xffd7615f` |
| **Volume fetch** | `0x1105` phase 10 | equal `(u,v)`, distinct Z slices: `0xff23500a`, `0xff835a2a` |

Authoritative current evidence is
`artifacts/runs/20260729-volume-v10-frozen-t2-r2/`:
`version=10`, result size 864, terminal `stage=175`, 17 logical draws, 17
single-draw IOCTLs, 15 readbacks, and every phase/cleanup overall flag equals
one.

### Compile-proven only (Tier 1)

Nine sampler declarations, being three sampler types times three controls,
lowered and compiled on a real GL driver through `preinstall-test`. Compilation
proves the lowering is well-formed; it cannot catch a swizzle, coordinate-space
or texel-addressing error.

### Not proven at either tier

| Capability | Blocking prerequisite |
|---|---|
| Render targets beyond the single bound target / MRT | `NumSimultaneousRTs` remains honestly 1 |
| SM2 `Reset` | the fixed/SM1.x D3D9 Reset path is proven; linked SM2 object/resource recovery is not |
| Stock-runtime video-memory backings, indexed SM2 draws | bounded foundations exist, but no final exact-pixel stock-runtime proof |
| Shipping multistream SM2 draws | stream-source capture has separate diagnostic stream-7 evidence; shipping `MaxStreams` remains honestly 1 |

### Reachability: the claim this matrix does NOT support

Every SM2 Tier 2 result above is obtained through `DrvEscape`, which bypasses
the Direct3D 9 runtime. **No unmodified application reaches this SM2 path.** A
diagnostic DDI-v4 build now executes the representative fixed-function/SM1.x
workload, but SM2 caps remain clear; ordinary packages still decline DDI v4 and
SM2 entirely.

So this matrix is evidence that the Shader Model 2 implementation is correct
where it is exercised. It is **not** evidence of shipping Shader Model 2
support, and must not be cited as such.
