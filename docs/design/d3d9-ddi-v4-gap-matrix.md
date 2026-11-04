# Direct3D 9 XPDM DDI-v4 gap matrix

## 2026-07-31 diagnostic DDI-v4 checkpoint (ABI 61)

The row-by-row matrix below is the original planning baseline and is historical
where this checkpoint says otherwise. DriverVer remains `1.0.0.305`.
Ordinary packages still decline DDI v4 and emit `QXPDISP GD9 FALLBACK`; their
native caps9 block remains `DevCaps2 = 0`, `MaxStreams = 1`, VS1.1, and PS1.3.
The stock DDI-v3 runtime derives 16 streams from caps8, but that is not native
DDI-v4 exposure. Exact ordinary evidence is
`artifacts/runs/20260731-abi61-d3d8-specular-fix-r140/D3d9/`, where PS2 and
the SM2 contract are explicitly skipped and the fixed-function/SM1.x
21-pixel frame, Reset, and teardown pass. The same frozen ordinary package
also passes the corrected 800x600 D3D8 visual and D3D8 interface matrices
without changing any advertised capability.

The coherent native profile is available only when a separately signed build
defines all four capability gates: DDI4, STREAMOFFSET, MAXSTREAMS8, and
SM2_CAPS. That diagnostic package returns DDI version 4 and advertises
`D3DDEVCAPS2_STREAMOFFSET`, eight native streams, VS2, and PS2. Plain-PS2
optional caps remain clear: `PS20Caps.Caps`, dynamic flow depth, and static
flow depth are all zero. Pixel static-flow opcodes remain fail-closed.
Volume textures, MRT, queries, multisampling, and other unimplemented optional
features remain unadvertised.

The former plain-PS2 sampler blocker is closed in ABI 61. A dedicated
sixteen-entry sampler state/binding array spans display capture, the
pointer-free packet, miniport validation, and host GL binding. Every declared
sampler is owner/generation/incarnation qualified; masks, dimensions, mips,
NPOT restrictions, addressing, and filters fail before side effects. The
decoder/translator keeps float-constant requirements separate from sampler
masks, and the packet contract admits every valid sixteen-bit sampler subset.
The host binds or explicitly unbinds all s0-s15 entries and restores active
texture zero. This implementation is present in ordinary builds but remains
unreachable from stock D3D9 while SM2 is unadvertised.

The exact diagnostic package at
`artifacts/runs/20260731-abi61-diagnostic-boundary-r143/` passes Install,
`D3d9Multistream7`, and handback as `T2-diagnostic`. It reports
`vs=fffe0200`, `ps=ffff0200`, `streams=8`, accepts advertised VS1.1, and
retains fixed-function/SM1.x plus Reset. Exact SM2 readbacks cover direct,
indexed managed, dynamic default-pool, and managed textured draws, including
isolated s4=`0xff2458a4` and s15=`0xffa4386c`. A single
stock-runtime-created `ps_2_0` shader then declares and samples sixteen
independently bound managed 2D textures; the mask is `0xffff` and the exact
center pixel is `0xffff2040`.

The same run passes the stream-0+7 oracle with independent offsets/strides,
exact pixels, resource release before Reset, Reset, teardown, and a 22/22
visual score. Its package and ISO remain unchanged throughout the cycle.


Against the old row IDs:

- G01 is diagnostic-enabled and stock-runtime proven; production enablement
  remains intentionally open.
- G02 SOURCE2 offsets, G03 split native shader objects, and G06 managed/default
  source identities execute and fail closed inside the diagnostic envelope.
- G07 Reset/lifetime is proven for native programs, buffers, textures, and
  stream-7 resources.
- G09's implementation blocker is closed by the ABI-61 s0-s15 path; its
  stock-runtime reachability remains diagnostic.
- G11 color-write and independent fog/specular execution are retained.
- G18's coherent plain-2.0 caps, G20's constant/sampler transport, G21 native
  submission, and G22 offset multistream execution are diagnostic-only proofs.

G05 copy/fill records remain trace-driven. G08's optional eight-stage
fixed-function policy is separate from plain SM2. Unadvertised
volume/MRT/query/multisample features remain optional and must not be inferred
from the diagnostic DDI-v4 switch.

## 2026-07-28 measured closure checkpoint (ABI 58)

The row-by-row source snapshot below is the original planning baseline; several
“missing” descriptions and line numbers are historical. The authoritative
current checkpoint is:

- shipping package `1.0.0.305` still declines DDI v4, SM2, and volume caps,
  reports `MaxStreams = 1`, and contains `QXPDISP GD9 FALLBACK`;
- diagnostic DDI v4 plus truthful `D3DDEVCAPS2_STREAMOFFSET` reaches stock XP
  SP3 System32 D3D9 and retains the 21-pixel fixed/SM1.x, managed mip/cube,
  render-target/depth, scissor, stream-offset, and Reset proof;
- ABI 58's diagnostic 3D define/upload/bind path executes true `sampler3D`
  fetches at exact pixels `0xff23500a` and `0xff835a2a`, without advertising a
  stock-runtime volume format or capability;
- transactional TEXBLT/resource identity, nonwrapping names and generations,
  recreation, teardown, and DLL-global display authority remain preserved; and
- title-agnostic generated texture coordinates, deterministic missing-normal
  semantics, and complete material-source GLSL restore UT2003 without changing
  any shipping capability.

The frozen package is DriverVer `07/22/2026,1.0.0.305`, signer
`1F590CA35B62264685FBCE150E252C9E2377A9D9`, `qxpdisp.dll`
`ECCC4E74889E6E0A63FCB772CAF4BB56CF361FA13DCAA11405CAEEF4B3E6FA98`, and
`qxpmp.sys`
`D7E08077AAF2D007E91383CC0BA9E1553AECA21F95B59E3B2BC9D90DFB7AF8A5`.
Its full nine-mode T2 matrix is
`artifacts/runs/20260729-volume-v10-frozen-t2-r2/` (17/17 self-test draws,
15 readbacks, Max Payne 24.506 fresh FPS, clean handback).
Exact-package game evidence is:

- UT2003 direct-map/pre-join diagnostic:
  `artifacts/runs/20260729-volume-v10-final-games-t3-r2/`
  (180157 ms, eight rich non-desktop samples, exact 675-frame/15-fps video),
  explicitly rejected as normal-menu or joined-gameplay proof by
  `artifacts/temp/volume-v10-final-games-t3-r2-ut-corrected-verification-r5.json`;
- Warcraft III D3D8:
  `artifacts/runs/20260729-volume-v10-final-games-t3-r2/`
  (System32 D3D8, 256 all-`V` cadence frames, exact 256-frame/30-fps video);
- Max Payne:
  `artifacts/runs/20260729-volume-v10-final-games-t3-r2/`
  (125 decodable frames, three tutorial samples at 7.796/22.965/38.049 s,
  242642 draws) plus hash-bound direct review
  `artifacts/temp/volume-v10-final-maxpayne-visual-review.json`
  (`1047E0F4B17506BD0F9924D19B0C52BB8B037EF1171A9E8C896ADA44ADED1D2B`).

The combined T3 cycle retains structurally valid direct-map UT artifacts,
valid Warcraft and Max Payne evidence, and clean handback. Oracle r2 rejected
direct-map video but used an impossible normal-menu predicate: the official
integrity-valid Demo 2206 archive configures `Map=Index.ut2` while intentionally
containing no physical `Index.ut2`. Its fresh T3 failure therefore invalidates
the oracle rather than localizing a disk or driver defect. The correction is
`artifacts/temp/volume-v10-ut-oracle-t3-verdict-correction-r1.json`. Minimal r5
(`xpdriver-acceptance-ut-oracle-r5.iso`, `EEDDB4A0...984D`) changes only the
observer among r2's 27 embedded files. Its fresh run at
`artifacts/runs/20260729-volume-v10-ut-oracle-r5-t3-r2/` remains entirely in the
NvidiaLogo scene; the title emits the return-to-Entry/controller sequence only
on forced teardown. The bound verdict is
`artifacts/temp/volume-v10-ut-oracle-r5-t3-r2-verdict.json` (`FAIL`). UT remains
a release blocker, but this does not localize a D3D/DDI primitive defect.

DDI-v4 evidence remains
`artifacts/runs/20260726-v4-quarantine-diag-r18/harness-result.json`; the
current exact package does not convert that diagnostic into a shipping claim.
Likewise, stream-7 execution is historical diagnostic ABI-57 evidence rather
than an ABI-58 runtime claim, while current source/tests preserve the gather
implementation and shipping stays at one stream.

This checkpoint does **not** justify shipping v4, SM2, volume, MRT, or
multistream caps. Remaining stock-runtime envelopes include indexed
programmable draws, volume creation/format exposure, SM2 Reset/video-memory
backing, MRT, and trace-derived copy/fill forms. Rows below should be read
against this checkpoint until individually rewritten.

## Purpose and conclusion

This document answers a narrower question than “how much Direct3D 9 does
XPDriver implement?”: **what must change before XPDriver can honestly return
`DX9_DDI_VERSION` (4), and what additional work is needed for stock D3D9
applications to reach the existing Shader Model 2 code?**

The first finding changes the shape of the problem. The supplied XP WDK headers
define the DDI-v4 handshake and the D3D9 capability query as separate
contracts:

- `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/ddk/d3dhal.h`
  defines `D3DGDI2_TYPE_GETDDIVERSION`, `DD_GETDDIVERSIONDATA`, and
  `DX9_DDI_VERSION` at lines 2159 and 2282-2290;
- the same header defines `D3DGDI2_TYPE_GETD3DCAPS9` separately at line 2149;
- no supplied WDK header ties DDI version 4 to an eight-stage or SM2 capability
  floor.

The current comment in `guest/xpdm/qxpdisp/directdraw.c:13932-13936` therefore
records a **project policy**, not a requirement substantiated by the supplied
XPDM DDI headers. Two independent implementations support that distinction:

- DXVK can expose a D3D9 adapter with shader model 0 or 1 and conditionally
  zeroes `VS20Caps`/`PS20Caps` when the configured shader model is below 2
  (`<DXVK checkout>/src/d3d9/d3d9_adapter.cpp:686-759`), while
  still using eight fixed-function stages
  (`<DXVK checkout>/src/d3d9/d3d9_caps.h:6-16`);
- ReactOS's XPDM-style runtime selects DX8 fallback solely from
  `dwDDIVersion`, then requests either `D3DCAPS8` or `D3DCAPS9`; it does not
  apply an SM2/eight-stage test in that decision
  ([`dll/directx/d3d9/d3d9_caps.c:453-527`](https://github.com/reactos/reactos/blob/9393fc320e3fcc89efa1c03c695d6f96885d88f6/dll/directx/d3d9/d3d9_caps.c#L453-L527)).

ReactOS is corroboration, not an authority for stock XP. The authoritative
provided evidence proves only that the WDK does not state the asserted caps
floor. A stock-XP experiment or an archived DirectX 9 DDK conformance document
is still needed to prove whether the Microsoft runtime applies an undocumented
floor.

Accordingly, the matrix uses three requirement scopes:

- **V4** — XPDM DDI-v4 framing, callbacks, and commands named by the supplied
  WDK. These are the version-flip contract surface and candidate blockers.
- **SM2** — requirements conditional on advertising `vs_2_0`/`ps_2_0`, which
  is the project's application-unblocking target.
- **9_1 calibration** — Microsoft's documented
  [“Minimum capabilities for Direct3D level 9_1”](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#minimum-capabilities-for-direct3d-level-9_1)
  table. That document is for the Vista+ user-mode/WDDM feature-level model,
  **not** the XPDM DDI-v4 handshake. It is useful as a conservative “DX9-class”
  target, but it must not be presented as proof of an XP requirement.

The analysis found **25 matrix gap rows**: seven v4 protocol/lifecycle items
(G01-G07), ten fixed-function/sampler/cap/format items (G08-G17), and eight
SM2 capability/execution items (G18-G25). A row can group closely coupled cap
fields, so 25 is a tracking count rather than a claim that all work items are
equal. Not all 25 must close to return version 4 with today's conservative caps;
all cap-dependent gaps must either be closed or remain honestly unadvertised.
For the narrow version-only decision, G02 and G03 are the substantiated code
blockers and G01 is the final switch. G04/G05 are runtime-reachability unknowns;
G06/G07 and G17 are proof/truthfulness gates. G08-G16 are policy/calibration
work, and G18-G25 become required only to the extent that SM2 or their cited
caps are advertised.

## Evidence conventions

- Every current-state claim cites XPDriver as `path:line` or `path:start-end`.
- WDK requirements cite the repository header path and symbol. The local WDK
  is authoritative for structure layout, command values, and explicit “must”
  comments.
- DXVK and Wine are calibration only. No implementation or text is copied from
  either project.
- **Implemented and proven** means exercised through a real stock-runtime or
  live-host oracle recorded in the repository. **Implemented but unproven**
  means code exists but DDI v4 cannot currently make the stock XP runtime
  exercise it. **Missing/absent** means no accepting production path was found;
  **partial** means only some required framing, state, execution, or proof
  exists. The SM2 GPU evidence is explicitly out-of-band.

## Gap matrix: XPDM DDI-v4 protocol and lifecycle

| ID | Requirement and evidence | Current state and evidence | Gap | Risk / effort |
|---|---|---|---|---|
| **G01** | **V4:** answer `D3DGDI2_TYPE_GETDDIVERSION` with a complete `DD_GETDDIVERSIONDATA` whose `dwDDIVersion` is `DX9_DDI_VERSION` (4). WDK: `DD_GETDDIVERSIONDATA` and `DX9_DDI_VERSION` (`inc/ddk/d3dhal.h:2282-2290`). | The handler leaves the default `DDERR_CURRENTLYNOTAVAIL`, writes no version, logs fallback, and returns handled (`guest/xpdm/qxpdisp/directdraw.c:13932-13939`). | **Missing.** The final one-DWORD handshake is deliberately absent. It must remain last, after reachable v4 stream behavior is correct. | **Cheap code, consequential exposure.** Isolated handler edit, but it changes every D3D9 application's command stream. Low code effort; maximum integration risk. |
| **G02** | **V4:** `D3DDEVCAPS2_STREAMOFFSET` “Must be set by DX9 drivers.” WDK: `D3DDEVCAPS2_STREAMOFFSET` (`inc/api/d3d9caps.h:536`). Its command is `D3DNTDP2OP_SETSTREAMSOURCE2`/`D3DNTHAL_DP2SETSTREAMSOURCE2` (`inc/api/d3dnthal.h:602,1338-1345`). | `qxp_d3d_caps9` is zeroed and `DevCaps2` is never assigned (`guest/xpdm/qxpdisp/directdraw.c:13769-13845`), so it reports zero. Opcode 80 is framed into the D3D9 ledger (`directdraw.c:11351-11368`), but any SOURCE2 state makes execution unsupported (`guest/xpdm/qxpdisp/qxp_d3d9_draw_state.c:344-360`). | **Partial.** Exact state parsing exists; honest cap reporting and draw consumption do not. | **Medium.** Ledger work is isolated and already present. Consuming offsets touches vertex-source capture and the draw path, but need not alter the legacy D3D8 fast path. |
| **G03** | **V4:** the D3D9 split declaration/function records are opcodes 71-76; WDK: `D3DDP2OP_CREATEVERTEXSHADERDECL` through `D3DDP2OP_SETVERTEXSHADERFUNC` and their create-record structures (`inc/ddk/d3dhal.h:947-952,1455-1470`). They replace the DX8 combined vertex-shader object shape on a v4 stream. | Opcodes 71-76 are routed (`guest/xpdm/qxpdisp/directdraw.c:11127-11163`) and declarations are parsed. Function preparation accepts **only** exact `vs_2_0` (`guest/xpdm/qxpdisp/qxp_d3d9_shader_objects.c:255-308`), so legal `vs_1_1` functions on an SM2-capable device are rejected. More fundamentally, any FVF/private declaration or vertex-function binding makes a draw “unsupported” (`qxp_d3d9_draw_state.c:344-360`), and the gate then rejects it (`qxp_d3d9_draw_gate.c:37-110`). | **Partial.** Framing/lifetime are implemented but neither v4 fixed-function FVF draws nor v4 `vs_1_1` compatibility are executable. | **Large, high regression risk.** This is the compatibility bridge for already-shipping SM1.x/fixed-function games. Route v4 FVF and `vs_1_1` without changing the proven DX8 object path; test both before SM2 exposure. |
| **G04** | **V4 / cap-gated:** opcode 79 is `D3DDP2OP_SETSCISSORRECT` and carries a `RECT`; WDK: `inc/ddk/d3dhal.h:954,1512-1513`. `D3DRS_SCISSORTESTENABLE` is state 174 (`inc/api/d3d9types.h:404`). | There is no opcode-79 case in the draw switch; unknown commands return `D3DNTERR_COMMAND_UNPARSED` (`guest/xpdm/qxpdisp/directdraw.c:11622-11628`). State 174 falls through the render-state default (`directdraw.c:3022-3180`). `RasterCaps` does not advertise scissor (`directdraw.c:13355-13362`). | **Missing.** Honest while the cap remains clear, but a v4 runtime can expose `SetScissorRect`; exact stock-XP emission behavior with the cap clear is **unknown**. | **Medium.** Add state plus transport/GL scissor handling. Hot draw-state path, but separable and cap-gated. |
| **G05** | **V4:** the WDK enum defines `BLT` 81, `COLORFILL` 82, and `SURFACEBLT` 96 (`inc/ddk/d3dhal.h:956-970`) with records at lines 1515-1548. A v4 implementation must either handle records the runtime can emit or substantiate why current caps make them unreachable. | None has a switch case; all reach the unknown-command return (`guest/xpdm/qxpdisp/directdraw.c:11622-11628`). DirectDraw Blt and a private point-filter GPU surface-copy path exist (`directdraw.c:12980-13145`), but those are different entry points and record shapes. | **Missing.** Runtime reachability under today's caps is not substantiated. Existing copy primitives are reusable evidence, not DDI handlers. | **Medium-large.** Surface ownership/coherency is sensitive. Avoid duplicating it: route DP2 operations through shared checked copy/fill primitives and prove prefix/error semantics. |
| **G06** | **V4 surface identity:** `DD_MISCELLANEOUS2CALLBACKS.CreateSurfaceEx` and `DDHAL_MISC2CB32_CREATESURFACEEX` are the XPDM surface-handle association contract (WDK `inc/api/ddrawint.h:400-420,1955-1964`). | The callback is advertised (`guest/xpdm/qxpdisp/directdraw.c:13854-13861`) and registers ordinary, cube-face, and mip-level surfaces (`directdraw.c:11725-11776`). It has extensive owner teardown, but DDI v4 never exercises it with D3D9 declaration/buffer/SM2 draws. | **Partial (implemented, unproven under v4).** No source defect was found for supported 2D/cube classes. Volume, multisample, and future format classes remain outside its proven surface table behavior. | **Medium.** Existing shipping paths share the surface table, so regression risk is real. Primarily test/oracle work until a concrete missing class is observed. |
| **G07** | **V4 Reset/lifetime:** XPDM has no dedicated `Reset` driver callback: the complete `D3DNTHAL_CALLBACKS` table contains context create/destroy, not Reset (WDK `inc/api/d3dnthal.h:382-442`). API Reset is realized through context/surface/target lifecycle. All production objects introduced for SM2 must therefore teardown and recreate through those paths. | Context create/destroy/all are installed (`guest/xpdm/qxpdisp/directdraw.c:13661-13665`) and implemented (`directdraw.c:10116-10398`). Production DDI SM2 never creates a host program. Miniport SM2 release exists for private programs (`guest/xpdm/qxpmp/qxpmp.c:1728-1770,1941-1951`) and is reached only by out-of-band/self-test plumbing. | **Partial.** No missing “Reset entry point”; the gap is integration and proof of v4 objects, surfaces, constants, and linked host-program teardown/recreation. | **Large integration test surface.** Lifetime code is mostly isolated, but failure affects Reset, process succession, and all current games. |

## Gap matrix: fixed-function and conservative 9_1 calibration

The rows in this section are **not claimed as XPDM-v4 minima**. They answer the
project's stronger policy question: what is missing if “honest Direct3D 9” is
calibrated against Microsoft's documented level-9_1 floor.

| ID | Requirement and evidence | Current state and evidence | Gap | Risk / effort |
|---|---|---|---|---|
| **G08** | **9_1 calibration:** `MaxTextureBlendStages >= 8` and `MaxSimultaneousTextures >= 8` ([Microsoft source lines 125-136](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L125-L136)). The XP WDK allocates eight cascade stages: `D3DNTHAL_TSS_MAXSTAGES == 8` (`inc/api/d3dnthal.h:963-994`). | Extended caps and caps8 report 4 (`guest/xpdm/qxpdisp/directdraw.c:13703-13704,13745-13746`), copied to caps9 (`directdraw.c:13812-13815`). Context state is `[4][64]` (`directdraw.c:320-354`), stage >3 is ignored (`directdraw.c:3197-3213`), and `ValidateTextureStageState` always says one successful pass (`directdraw.c:11684-11693`). | **Missing.** Four additional bindings, blend cascades, sampler states, transforms/coordinates, validation, transport, and host replay are absent. | **Very large; highest fixed-function regression risk.** This directly touches the hot path used by shipping D3D8/fallback games. Prefer a new array-based/v2 transport shape or isolated extension rather than mechanically enlarging every legacy packet. |
| **G09** | **SM2:** D3D9 has 16 pixel samplers and maps API `D3DSAMP_*` state to DDI `D3DTSS_*`; WDK `D3DHAL_SAMPLER_MAXSAMP == 16` and mapping comments (`inc/api/d3dnthal.h:1000-1054`). The local SM2 lowerer likewise defines 16 samplers (`guest/xpdm/qxpdisp/qxp_pshader_glsl.h:9-12`). | Metadata and host uniform locations are sized to 16, but draw state and wire bindings exist only for stages 0-3. The SM2 miniport explicitly rejects every active sampler `>= 4` (`guest/xpdm/qxpmp/qxpmp.c:8764-8772`); all sampler masks are validated with `& ~15UL` (`qxpmp.c:8704-8719,9143-9166`). | **Missing.** Raising fixed-function stages to eight would still leave samplers 8-15 absent. Blend-stage state (8) and shader-sampler state (16) must be separated. | **Very large but architecturally separable.** New SM2 sampler arrays/state packets can avoid perturbing the legacy fixed-function packet. Requires texture ownership, type, sampler-state, and dirty-update work. |
| **G10** | **9_1 calibration:** `Caps2` includes `D3DCAPS2_DYNAMICTEXTURES \| D3DCAPS2_FULLSCREENGAMMA` ([Microsoft source lines 29-33](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L29-L33)). | Caps8/caps9 include windowed rendering and dynamic textures, but not fullscreen gamma (`guest/xpdm/qxpdisp/directdraw.c:13709-13713,13772-13776`). No gamma implementation was found in the DirectDraw callbacks installed at `directdraw.c:14127-14159`. | **Missing.** The bit must remain clear unless a real gamma-ramp path is added and proven. | **Medium/unknown.** Mostly presentation/display state, not the draw hot path, but mode changes and Reset make regressions consequential. |
| **G11** | **9_1 calibration:** `PrimitiveMiscCaps` includes `D3DPMISCCAPS_COLORWRITEENABLE` ([Microsoft source lines 39-42](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L39-L42); flag in WDK `inc/api/d3d9caps.h`). | The cap word contains culling and blend-op only (`guest/xpdm/qxpdisp/directdraw.c:13355-13357`). Yet `D3DRS_COLORWRITEENABLE` is stored and transported (`directdraw.c:3113-3115,5341-5342`), and both legacy and SM2 miniport paths issue `glColorMask` (`guest/xpdm/qxpmp/qxpmp.c:8869-8879,9710-9720`). | **Partial.** Implementation exists; cap-level conformance proof is missing. Do not set the bit from code inspection alone. | **Cheap after an oracle.** Cap-only change if exact per-channel tests pass in fixed, PS1.x, and SM2 paths. Low code risk; modest game-path regression risk. |
| **G12** | **9_1 calibration:** `RasterCaps` includes `D3DPRASTERCAPS_DEPTHBIAS \| D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS` ([Microsoft source lines 108-112](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L108-L112)). D3D9 states are 195 and 175 (`inc/api/d3d9types.h:405,424`). | Caps advertise legacy `ZBIAS`, not either D3D9 bit (`guest/xpdm/qxpdisp/directdraw.c:13358-13363`). The context carries only integer `z_bias`; unknown D3D9 states are ignored (`directdraw.c:3022-3180`). Miniport sets polygon-offset factor 0 and derives units from that integer (`guest/xpdm/qxpmp/qxpmp.c:8945-8971`). | **Missing.** Float depth bias and slope-scale state, transport fields, validation, and GL mapping are absent. | **Medium-high.** Adds two hot render states and changes wire state. Existing Z-bias games require regression tests. |
| **G13** | **9_1 calibration:** `StencilCaps` includes `D3DSTENCILCAPS_TWOSIDED` ([Microsoft source lines 113-116](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L113-L116)). D3D9 defines enable and CCW operations at states 185-189 (`inc/api/d3d9types.h:414-418`). | Caps omit TWOSIDED (`guest/xpdm/qxpdisp/directdraw.c:13686-13690`). Only one face's fail/zfail/pass/function is stored and sent (`directdraw.c:3091-3112`; transport fields at `guest/xpdm/qxp_d3d_transport.h:405-413`). | **Missing.** Opposite-face state and separate host stencil calls are absent. | **Medium-high; hot raster path and ABI.** Isolated from texture blending but shared by every stencil game. |
| **G14** | **9_1 calibration:** four point/linear bits in `StretchRectFilterCaps` ([Microsoft source lines 96-103](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L96-L103)). WDK declares `D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES` and DP2 blit structures (`inc/api/d3d9caps.h`, `inc/ddk/d3dhal.h:1515-1548`). | `StretchRectFilterCaps` and `DevCaps2` remain zero (`guest/xpdm/qxpdisp/directdraw.c:13769-13845`). A private surface copy supports only point filtering and three RGB formats (`directdraw.c:12980-13145`); DP2 81/96 are unhandled (G05). | **Partial.** A useful primitive exists, but D3D9 API routing, linear filtering, broader valid format pairs, and cap reporting are absent. | **Medium-large.** Surface coherence/format conversion work; does not need to touch fixed-function draws. |
| **G15** | **9_1 calibration:** `MaxAnisotropy >= 2` ([Microsoft source lines 157-160](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L157-L160)), with matching anisotropic filter caps if advertised (WDK `inc/api/d3d9caps.h`, `D3DPTFILTERCAPS_*FANISOTROPIC`). | `MaxAnisotropy` is 1 (`guest/xpdm/qxpdisp/directdraw.c:13678,13736,13796`); anisotropic bits are absent (`directdraw.c:13374-13380`). Draw transport has no per-sampler max-anisotropy field (`guest/xpdm/qxp_d3d_transport.h:430-507`). | **Missing for 9_1; honestly unsupported today.** | **Medium-large and ABI-affecting.** Host anisotropy support is unverified; every affected sampler needs state, validation, and tests. Isolate it in new sampler state rather than grow legacy packets. |
| **G16** | **9_1 calibration:** `MaxVolumeExtent >= 256` ([Microsoft source lines 149-152](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L149-L152)). For an SM2 `sampler3D`, resource and sampler state must also exist; WDK format operation is `D3DFORMAT_OP_VOLUMETEXTURE` (`inc/api/ddrawint.h:108-113`). | Caps9 still leaves `MaxVolumeExtent`, volume filter/address caps, and volume texture caps zero; no `D3DFORMAT_OP_VOLUMETEXTURE` is advertised. ABI 58 now carries diagnostic `QXP_D3D_TEXTURE_VOLUME` define/upload/draw data to a validated `GL_TEXTURE_3D` path, and escape `0x1105` version 10 proves two exact Z-slice fetches. The stock runtime cannot create or reach that resource path. | **Missing for stock runtime/capability exposure; diagnostic backend proven.** Exact out-of-band execution closes the resource/backend uncertainty but not DDI creation, format operations, caps, limits, Reset, or application reachability. | **Medium-large remaining work, high honesty risk.** Add stock-runtime volume surface/subresource routing and lifetime/Reset coverage, then derive conservative caps and format operations from exact tests. Keep every bit clear until that full path passes. |
| **G17** | **V4 format truthfulness:** the runtime learns legal uses through `GETFORMATCOUNT/GETFORMAT`; `DDPIXELFORMAT.dwOperations` uses WDK `D3DFORMAT_OP_*` semantics (`inc/api/ddrawint.h:96-182`). Each advertised operation must work; absent formats may remain absent. | The driver returns 27 operation entries (`guest/xpdm/qxpdisp/directdraw.c:189-192,13976-13994`). Core RGB RTs and D16/D24S8/D24X8 are listed (`directdraw.c:13539-13658`), but no volume, float, sRGB, autogen, or vertex-texture operation is advertised. Miniport target binding accepts only X8R8G8B8/A8R8G8B8/R5G6B5 (`guest/xpdm/qxpmp/qxpmp.c:2569-2579`). No stock-v4 create/sample/RT oracle covers every advertised operation. | **Partial.** The table is conservatively broad for 2D/cube formats, but operation-by-operation v4 proof is missing. No authoritative XPDM mandatory `D3DFORMAT` set was found; unsupported entries should be omitted, not invented. | **Medium test effort; potentially large if failures appear.** Format work is mostly isolated, except RT/depth formats touch presentation and Reset. |

## Gap matrix: SM2 capability and production execution

| ID | Requirement and evidence | Current state and evidence | Gap | Risk / effort |
|---|---|---|---|---|
| **G18** | **SM2 capability surface / 9_1 calibration:** advertising SM2 requires `VertexShaderVersion`/`PixelShaderVersion` plus coherent `D3DVSHADERCAPS2_0`/`D3DPSHADERCAPS2_0`; the separate level-9_1 table specifically requires `PixelShaderVersion == D3DPS_VERSION(2,0)` ([Microsoft source lines 129-140](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L129-L140)). WDK minima are VS temps 12, dynamic flow 0, static flow 1; PS temps 12, dynamic/static flow 0, and 96 instruction slots (`inc/api/d3d9caps.h:16-63`). Wine's conservative SM2 branch reports those baseline shapes and zero optional cap bits (`<Wine checkout>/dlls/wined3d/directx.c:2626-2684`). | Caps8 reports VS1.1/96 constants and PS1.3 (`guest/xpdm/qxpdisp/directdraw.c:13752-13758`), copied into caps9 (`directdraw.c:13825-13835`). The whole caps9 structure is zeroed, so `VS20Caps`, `PS20Caps`, `Max*ShaderInstructionsExecuted`, and SM3-only fields remain zero (`directdraw.c:13769-13845`). | **Missing.** Do not merely change version tokens. Define a conservative, tested SM2 cap profile; optional bits may remain zero. | **Small cap code, high honesty risk.** Populate last, from proven production limits rather than lowerer aspirations. |
| **G19** | **SM2 language semantics:** every shader accepted under the reported version/cap profile must decode, lower, compile/link, and execute with D3D9 semantics. Optional `D3DPS20CAPS_*` bits only permit extra forms (WDK `inc/api/d3d9caps.h:41-63`). | Exact `ps_2_0`/`vs_2_0` typed IR and lowerers exist; VS has 12 temps/256 float constants (`guest/xpdm/qxpdisp/qxp_vshader20_glsl.h:20-25`), PS has 32 constants/16 samplers (`qxp_pshader_glsl.h:9-12`). Live GPU compile/link/draw/texture evidence exists only via escapes (`docs/design/d3d9-shader-model-2.md:1066-1123`). The decoder rejects uninitialized reads (`guest/xpdm/qxpdisp/qxp_shader_decode.c:908-918,2573-2582`); whether D3D9 instead requires zero-valued reads remains to be established. | **Partial: implemented/proven out-of-band, unproven through DDI.** A conformance/leniency audit remains before exposing the profile. | **Large semantic risk, mostly isolated from legacy.** Differential fixtures and Wine D3D9 tests can harden this without touching fixed-function code. |
| **G20** | **SM2 constants and limits:** WDK shader-profile limits include VS 16 integer/16 Boolean registers and PS 32 float constants (`inc/ddk/d3dhal.h:2368-2401,2457-2480`). `D3DCAPS9` also carries `MaxVertexShaderConst`, `MaxVShaderInstructionsExecuted`, `MaxPShaderInstructionsExecuted`, and PS instruction slots (`inc/api/d3d9caps.h:117-210`). | The dormant ledger stores VS 256F/16I/16B and PS 32F/16I/16B (`guest/xpdm/qxpdisp/qxp_d3d9_draw_state.h:22-29,115-126`); opcodes 48/57/77/83/93/94 route them (`directdraw.c:11267-11368`). ABI 58 retains the sticky 256/32 float uploads introduced at ABI 55 (`guest/xpdm/qxp_d3d_transport.h`). Caps still report VS 96 and zero SM2 execution limits. The decoder's fixed ceilings are 512 instructions/16,384 tokens (`qxp_shader_ir.h:11-18`), but no documented derivation maps those ceilings and loop execution to honest `Max*ShaderInstructionsExecuted` values. | **Partial.** Storage/transport primitives exist and self-tests use float constants; production binding and defensible cap values are missing. Integer/Boolean host upload through real draws is unproven. | **Medium-large, isolated new path.** Derive caps from admitted shaders, enforce them at creation, and test boundary programs before reporting. |
| **G21** | **SM2 production draw:** a stock-runtime draw must consume the WDK's command-buffer/context contract and return exact first-unhandled-command state (`D3DHAL_DRAWPRIMITIVES2DATA`, `inc/ddk/d3dhal.h:1623-1648`) while resolving object/resource ownership, streams/indices, program binding, constants/samplers, submission, and lifetime. | The gate always destroys a successfully prepared draw and returns unsupported (`guest/xpdm/qxpdisp/qxp_d3d9_draw_gate.c:76-110`). Live inspection captures only nonindexed stream-0 user memory or one system-memory surface; indexed, video-memory, and other shapes stop before capture (`guest/xpdm/qxpdisp/directdraw.c:9700-9784`). No production code sets `QXP_D3D_DRAW_REQUEST.sm2_program_name`; only self-tests do (`directdraw.c:8711,9195`). | **Missing.** This is the principal application-unblocking gap. The real host-GPU path is proven, but the DDI-to-host bridge is absent. | **Very large, but mostly isolated from the fixed path.** Complete in narrow source classes first, fail closed elsewhere, then broaden. Regression risk comes from shared surface/lifetime code, not the legacy wire packet. |
| **G22** | **Streams:** `D3DCAPS9.MaxStreams` must match executable stream sources (`inc/api/d3d9caps.h:180-183`); source offsets are mandatory for v4 (`inc/api/d3d9caps.h:536`, G02). Instancing/frequency must work only if exposed; Microsoft's level-9_3 note says `D3DVS_VERSION(3,0)` guarantees instancing ([source lines 313-322](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L313-L322)). | Caps9 deliberately reports one stream (`guest/xpdm/qxpdisp/directdraw.c:13820-13824`). The ledger can hold 16 and parses SOURCE2/frequency (`qxp_d3d9_draw_state.h:22-29,103-126`), but nonunit divider or SOURCE2 forces unsupported execution (`qxp_d3d9_draw_state.c:344-360`), and current capture is stream-0-only (`directdraw.c:9700-9784`). | **Partial.** `MaxStreams=1` is honest for an initial v4/SM2 device; multistream and instancing are compatibility work, not proven SM2 minima. Source offset is separately mandatory. | **Medium-large.** New SM2 gather/capture code; legacy D3D8 16-stream gather should remain untouched. |
| **G23** | **Draw limits:** calls up to reported `MaxPrimitiveCount` and indices up to `MaxVertexIndex` must work. The 9_1 calibration floor is 65,535/65,534 ([Microsoft source lines 141-148](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L141-L148)); fields are in WDK `D3DCAPS9` (`inc/api/d3d9caps.h:180-183`). | Caps9 inherits `0x100000` for both (`guest/xpdm/qxpdisp/directdraw.c:13748-13749,13818-13819`). Legacy/program-VS paths chunk requests at 255 vertices (`directdraw.c:5934-5941,6760-6767`). SM2 envelope/access arithmetic exists, but production SM2 never submits and capture is bounded/partial (G21). | **Partial/unproven for the v4 SM2 stream.** Existing numbers may be defensible for legacy draws; they are not yet proven for every D3D9 draw form. | **Medium test and batching work.** Keep caps conservative until indexed/nonindexed boundary oracles and real submission pass. Little fixed-path change should be needed. |
| **G24** | **Clip planes, points, and post-shader behavior:** reported `MaxUserClipPlanes`/`MaxPointSize` and relevant caps must hold for programmable draws; D3DCAPS9 fields are in `inc/api/d3d9caps.h:174-183`. | Caps report six clip planes and point size 64 (`guest/xpdm/qxpdisp/directdraw.c:13747,13765-13766,13816`). Fixed/HWTL state carries six planes and point state (`directdraw.c:3038-3071,3155-3169`). B3 emits clip/point epilogues and has out-of-band live evidence (`docs/design/d3d9-shader-model-2.md:405-428`). The SM2 miniport path rejects any fog-enabled draw (`guest/xpdm/qxpmp/qxpmp.c:8733-8737`), and no production SM2 draw proves clip, point-sprite coordinate replacement, fog, or clipping interaction. | **Partial.** Fixed path is implemented; programmable production behavior is not. Fog is concretely absent from the private SM2 draw path. | **Medium-high.** Mostly SM2 epilogue/post-raster state, but clip/point state is shared and game-visible. |
| **G25** | **D3D9 render/sampler state honesty:** the D3D9 enum adds scissor, slope/depth bias, two-sided stencil, blend factor, sRGB write, and separate-alpha states (`inc/api/d3d9types.h:404-436`); sampler states are mapped to DDI texture states including border, anisotropy, W address, and sRGB (`inc/api/d3dnthal.h:1028-1054`). Unsupported state must remain unadvertised or fail coherently, never be silently claimed. | `qxp_d3d_apply_render_state` implements the legacy subset and logs/default-ignores the rest (`guest/xpdm/qxpdisp/directdraw.c:3022-3180`). Texture state is limited to four stages and ignores border, max anisotropy, W for non-volume, and sRGB (`directdraw.c:3197-3450`). Cap words mostly keep those features clear, but there is no v4 state/cap consistency oracle. | **Partial.** Several baseline items are concrete gaps G04/G12/G13/G15; remaining optional states are honest only while their caps/format operations stay clear. Silent acceptance still needs stock-runtime error-semantics review. | **Large audit; implementation varies.** State-table audit is cheap. Each enabled feature affects the hot draw path; keep optional caps off until independently proven. |

## D3D9-only `D3dDrawPrimitives2` command inventory

The authoritative values are `D3DHAL_DP2OPERATION` in
`.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/ddk/d3dhal.h:947-972`. “Unhandled” means the command
reaches `directdraw.c:11622-11628`, which returns
`D3DNTERR_COMMAND_UNPARSED`; it does not mean the command is necessarily
reachable under current caps.

| Opcode(s) | WDK operation | XPDriver today | Gap / reachability |
|---|---|---|---|
| 71-76 | Create/delete/set vertex declaration and function | Routed to the private exact D3D9 object dispatcher (`guest/xpdm/qxpdisp/directdraw.c:11127-11163`). | **Partial (G03):** framing/lifetime exist; fixed FVF, `vs_1_1`, and draw execution do not form a working v4 path. |
| 77 | Set VS integer constants | Routed to D3D9 state (`directdraw.c:11351-11368`). | Implemented but production-draw unproven (G20/G21). |
| 78 | Reserved gap in WDK enum | No case. | None. |
| 79 | Set scissor rectangle | No case; default unparsed (`directdraw.c:11622-11628`). | Missing (G04). |
| 80 | Set stream source with offset | Routed to D3D9 state (`directdraw.c:11351-11368`). | Parsed, not executable (G02). |
| 81-82 | Blt; color fill | No cases; default unparsed. | Missing (G05); exact cap/runtime reachability needs a stock-XP trace. |
| 83 | Set VS Boolean constants | Routed to D3D9 state (`directdraw.c:11351-11368`). | Implemented but production-draw unproven (G20/G21). |
| 84 | Create query | No case. Query count is zero (`directdraw.c:13954-13958`). | **No current-cap gap; conditional missing.** Do not advertise queries. |
| 85 | Set render target 2 | Handled; only `RTIndex == 0` accepted (`directdraw.c:11546-11569`). | None for honest `NumSimultaneousRTs=1`; MRT remains optional. |
| 86 | Set depth-stencil | Handled (`directdraw.c:11571-11593`). | Implemented; v4/SM2 use unproven. |
| 87-88 | Response continue/query | WDK marks these driver-to-runtime only (`inc/ddk/d3dhal.h:962-963`). | Not inbound requirements. |
| 89 | Generate mip sublevels | No case. `CANAUTOGENMIPMAP` and `D3DFORMAT_OP_AUTOGENMIPMAP` are not advertised (`directdraw.c:13709-13713,13539-13658`). | No current-cap gap; conditional missing. |
| 90-91 | Delete/issue query | No cases; query count zero. | No current-cap gap; conditional missing. |
| 92 | Reserved gap in WDK enum | No case. | None. |
| 93-94 | Set PS integer/Boolean constants | Routed to D3D9 state (`directdraw.c:11351-11368`). | Implemented but production-draw unproven (G20/G21). |
| 95 | Set stream-source frequency | Routed to D3D9 state (`directdraw.c:11351-11368`). | Parsed; nonunit execution unsupported (G22). Not an SM2 baseline requirement. |
| 96 | Surface blt | No case. | Missing/unknown reachability (G05). |
| 97-98 | Convolution kernel; compose rects | No cases. No matching cap/format support is advertised. | No current-cap blocker found; leave unsupported. Stock XP (non-Ex) reachability was not substantiated. |

## Capability fields that are already coherent or honestly absent

These are deliberately not counted as gaps unless their cap is raised.

| Requirement / capability | Evidence on requirement side | Current state and evidence | Assessment |
|---|---|---|---|
| Core 9_1 compare, shade, filter, texture, address, texture-op, and blend bits | Microsoft level-9_1 table; flag definitions in WDK `inc/api/d3d9caps.h`. Its `TextureCaps` note permits both POW2 bits to be clear ([Microsoft source lines 43-107 and 167-172](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L43-L107)). | Current words are built at `guest/xpdm/qxpdisp/directdraw.c:13342-13391,13671-13746`. They include both required compare values, Gouraud shade bits, point/linear filters, alpha/cube/mipmap/perspective texture caps, wrap/mirror/clamp/independent UV, disable/select/modulate ops, and required source/destination blend factors; both POW2 alternatives are clear. | **No identified baseline gap** in these listed subsets. This is cap evidence, not exhaustive rendering proof. |
| Presentation intervals | Level 9_1 requires `IMMEDIATE \| ONE` ([Microsoft source lines 34-38](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L34-L38)). | Caps8 sets both and caps9 copies them (`guest/xpdm/qxpdisp/directdraw.c:13713-13714,13775-13776`). | **Numerically coherent.** Presentation correctness remains part of the stock-v4 oracle. |
| Texture dimensions, repeat, RT count, primitive/index scalar floors | Microsoft level-9_1 table. | 2048x2048, repeat 8192, one RT, and `0x100000` primitive/index values (`directdraw.c:13672-13680,13731-13749,13818-13819,13845`). | Numerically meets 9_1. SM2 draw proof remains G23. |
| `MaxVertexW` profile value | The WDDM table lists `0.f` for 9_1 but `1.0e10f` for cumulative level 9_2 ([9_1 source lines 161-164](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L161-L164); [9_2 source lines 237-240](https://github.com/MicrosoftDocs/windows-driver-docs/blob/81d06fd79dd36cdd74ec0b3aeed6c4e52c2e2fc3/windows-driver-docs-pr/display/required-direct3d-9-capabilities.md#L237-L240)). | Caps8 stores IEEE-754 `1.0e10f` (`0x501502f9`) and caps9 copies it (`guest/xpdm/qxpdisp/directdraw.c:13734,13797-13798`). | **Explicit unknown, not counted as an XPDM-v4 gap.** The feature-level table's profile/override semantics cannot be safely transplanted as an exact XPDM requirement; retain the proven legacy value until an authoritative XP rule or behavior test says otherwise. |
| MRT | `NumSimultaneousRTs` “will be at least 1” (WDK `inc/api/d3d9caps.h`, `D3DCAPS9`). | Reports one (`directdraw.c:13845`); opcode 85 rejects indices above zero (`directdraw.c:11556-11566`). | **Honest, no gap.** Do not set MRT misc caps or raise the count. Lowerer MRT output support does not justify the device cap. |
| Occlusion/event queries | WDK `GETD3DQUERYCOUNT/GETD3DQUERY` and query DP2 records (`inc/ddk/d3dhal.h:2157-2158,2267-2281,1560-1578`). | Returns a valid count of zero (`directdraw.c:13954-13958`); no query opcodes are handled. | **Honest optional absence.** Query support can improve games later, but is not substantiated as a v4 minimum. |
| Multisampling | WDK `D3DGDI2_TYPE_GETMULTISAMPLEQUALITYLEVELS` and `DD_MULTISAMPLEQUALITYLEVELSDATA` (`inc/ddk/d3dhal.h:2153,2258-2265`). | No multisample format/cap is advertised and no query handler is present (`directdraw.c:13884-14021`). | **Honest optional absence.** Missing handler becomes a gap only if multisample support is advertised. |
| Vertex blending | `MaxVertexBlendMatrices` and `MaxVertexBlendMatrixIndex` are capability values, not unconditional WDK minima (`inc/api/d3d9caps.h`, `D3DCAPS9`). | Both remain zero after caps9 initialization (`directdraw.c:13769-13845`); no blend weights/matrix palette execution is claimed. | **Honest optional absence, not a v4 blocker.** |
| Declaration extension types | `DeclTypes` bits enumerate optional nonbaseline declaration encodings (`inc/api/d3d9caps.h:545-557`). | Reports zero (`directdraw.c:13769-13845`). Parser records requirements for all extension types (`guest/xpdm/qxpdisp/qxp_vdecl9.c:40-87,344-356`), but production conversion/execution is absent. | **Honest optional absence.** Advertise types individually only after the actual gather/attribute path handles them. |
| Adapter group, empty extended modes, format queries | WDK GetDriverInfo2 structures (`inc/ddk/d3dhal.h:2204-2278`). | Adapter group ID 1, zero extended modes, and indexed format results are returned (`directdraw.c:13940-13994`). | Implemented. Exact stock-v4 initialization remains part of G01/G06 testing. |

## Eight-stage transport and ABI impact

Changing the two cap values from four to eight without all of the following
would be dishonest.

1. **Display context/state.** Replace the four-stage scalar fields and
   `[4][64]` state tables (`guest/xpdm/qxpdisp/directdraw.c:320-473`) with an
   eight-stage fixed-function cascade and a distinct 16-sampler table. Remove
   the stage >3 discard (`directdraw.c:3197-3213`). Preserve D3D's
   `D3DTOP_DISABLE` cascade termination.
2. **State blocks and validation.** Expand capture/replay of texture-stage
   state and make `QxpD3dValidateTextureStageState` validate actual operations,
   arguments, texture presence, and pass count instead of unconditional success
   (`directdraw.c:11684-11693`). Every existing state-block oracle needs 0-7
   coverage.
3. **Vertex coordinates/transforms.** Decide honestly how many FVF texture
   coordinate sets are supported. The wire vertex has only
   `texcoord[4][4]`, while HWTL has eight texture matrices but only four
   transform-flag entries (`guest/xpdm/qxp_d3d_transport.h:97-99,306-328,339-347`).
   Eight blend stages can reuse coordinates, but `FVFCaps=4` must not be
   silently treated as eight-coordinate support.
4. **Draw request ABI.** `QXP_D3D_DRAW_REQUEST` contains explicit stage-0,
   `texture1_*`, `texture2_*`, and `texture3_*` field groups only
   (`guest/xpdm/qxp_d3d_transport.h:430-507`). Adding groups before
   `pixel_program_name` and the variable `vertices[1]` tail changes the fixed
   header size and every later offset. Transport is currently ABI 58
   (`qxp_d3d_transport.h`). A version bump and paired display/miniport update
   are mandatory. A new request shape is safer than silently changing the
   legacy structure.
5. **Packet contracts.** Update all `~15UL` masks and four-stage loops in
   display preparation (`guest/xpdm/qxpdisp/directdraw.c:5019-5283`), packet
   validation (`guest/xpdm/qxpmp/qxp_packet.c:171-181,592-603`), miniport
   validation (`guest/xpdm/qxpmp/qxpmp.c:8704-8719,9143-9166`), and program
   texture contracts. Keep fixed-stage masks (8 bits) distinct from shader
   sampler masks (16 bits).
6. **Miniport replay/cache.** `qxp_d3d_apply_texture_state` rejects stage >3
   and explicitly selects four field groups (`guest/xpdm/qxpmp/qxpmp.c:7065-7180`);
   both SM2 and legacy draw paths call it in four-iteration loops
   (`qxpmp.c:8803-8806,9616-9620`). Extend binding, enable/disable, cube/2D,
   filters, combine equations, last-draw state-diff logic, and teardown for
   stages 4-7.
7. **Host capability and correctness proof.** Exercise every advertised
   texture op across stage positions, coordinate reuse/transform/projected
   flags, missing/stale bindings, state blocks, Reset, and mixed D3D8/D3D9
   contexts. Host GL unit count must be queried or bounded by the actual
   passthrough contract; it cannot be assumed from desktop hardware.
8. **Compatibility boundary.** Preserve the byte-identical ABI-58 path for
   current four-stage draws if possible. This is the single matrix item most
   likely to regress existing shipping games because it changes their hot
   fixed-function path even though they do not need DDI v4 or SM2.

A separate 16-sampler SM2 state/update mechanism is still required after this
work (G09). Eight fixed blend stages do not satisfy a shader that declares
`s4`-`s15`.

## Shader Model 2: defensible initial cap profile

The code can eventually expose a conservative **plain 2.0**, not 2_a/2_b,
profile. The following is a target to prove, not a recommendation to publish
now:

| Field | Defensible target after production proof | Why it is not publishable today |
|---|---:|---|
| `VertexShaderVersion` | `D3DVS_VERSION(2,0)` | Exact VS2 lowerer exists, but split VS1.1 compatibility and production draw are absent (G03/G21). |
| `MaxVertexShaderConst` | 256 | Ledger/lowerer/ABI have 256, but production upload/binding is unproven (G20). |
| `VS20Caps.Caps` | 0 | Predication is optional; claiming no extensions minimizes burden. |
| `VS20Caps.DynamicFlowControlDepth` | 0 | WDK minimum is 0. |
| `VS20Caps.NumTemps` | 12 | Matches WDK plain-VS2 minimum and lowerer storage. |
| `VS20Caps.StaticFlowControlDepth` | 1 | Matches WDK minimum; at least this depth needs a production oracle. |
| `PixelShaderVersion` | `D3DPS_VERSION(2,0)` | Exact PS2 lowerer and live escape exist, but only four samplers and no production draw (G09/G21). |
| `PS20Caps.Caps` | 0 initially | The decoder records arbitrary-swizzle/dependent-read/texture-limit requirements (`guest/xpdm/qxpdisp/qxp_shader_ir.h:247-253`), but optional global bits need whole-profile execution proof. |
| `PS20Caps.DynamicFlowControlDepth` | 0 | WDK plain-PS2 minimum. |
| `PS20Caps.NumTemps` | 12 initially | WDK plain-PS2 minimum. Although code can represent 32, claiming 32 changes the accepted shader set. |
| `PS20Caps.StaticFlowControlDepth` | 0 | WDK plain-PS2 minimum. |
| `PS20Caps.NumInstructionSlots` | 96 initially | WDK minimum. Creation must reject shaders whose computed requirement exceeds the published value. |
| `MaxVShaderInstructionsExecuted` / `MaxPShaderInstructionsExecuted` | **Unknown; derive and test** | WDK defines the fields but the current 512-instruction decoder ceiling plus bounded loops has not been mapped to truthful execution values. Wine uses 65,535/512 for plain VS2/PS2 (`<Wine checkout>/dlls/wined3d/directx.c:2626-2684`), but that is reference calibration, not proof for XPDriver's GLSL host. |
| SM3-only instruction-slot fields / vertex texture filter caps | 0 | Correct for a plain-SM2 device. |

This conservative profile intentionally does not claim optional PS2 extension
bits, 32 temporaries, 512 slots, MRT, vertex textures, or instancing merely
because individual code paths can represent them.

## Historical recommended sequencing

1. **Correct the contract decision first (cheap, no behavior change).** Treat
   DDI v4, SM2 caps, and the 9_1 calibration profile as three gates. Locate an
   archived XP DirectX 9 DDK conformance source or create a stock-XP runtime
   trace that records the exact v4 commands emitted with today's conservative
   caps. This resolves the only major requirement uncertainty before an
   expensive eight-stage ABI redesign.
2. **Make a v4 stream work with unchanged conservative caps (medium-large,
   highest compatibility value).** Do **not** return version 4 yet. Under a
   test-only gate, implement/verify FVF declarations, split `vs_1_1`, SOURCE2,
   opcode error semantics, and whichever of 79/81/82/96 the trace observes.
   This protects current fixed-function/SM1.x games and isolates DDI framing
   from shader work.
3. **Finish production SM2 draw submission (large, mostly isolated).** Extend
   guarded capture from the current nonindexed stream-0 cases to the source
   classes required by a first title; define/bind linked programs, upload
   constants, submit, and prove Reset/teardown. Keep unsupported shapes
   fail-closed. The out-of-band GPU path means this is integration, not a new
   shader compiler.
4. **Build the separate 16-sampler SM2 path (large, isolated new code).** Do
   this before advertising PS2. It is a harder blocker than the eight-stage
   fixed-function cascade for actual SM2 shaders because the miniport currently
   rejects `s4`-`s15`.
5. **Close the conservative SM2 cap profile (medium correctness work).** Add
   instruction/temp/constant boundary tests, D3D9 semantic/leniency cases, and
   stock-runtime VS1.1/PS1.x coexistence. Populate each caps field only after
   the corresponding production oracle passes.
6. **Close baseline raster/surface gaps selected as project policy
   (medium-large).** Color-write is likely cheap after proof. Depth bias,
   two-sided stencil, scissor, StretchRect, format operations, and fog are
   independent features. Keep every unimplemented cap clear.
7. **Expand fixed-function 4→8 only after its ABI design is frozen (very
   large, highest legacy regression risk).** Implement G08's full list with a
   new/versioned packet boundary and exhaustive D3D8 regression tests. This
   step is required by the project's current 9_1-like policy, but the supplied
   XPDM evidence does not establish it as a dependency of the v4 handshake or
   of the SM2 production bridge.
8. **Return `DX9_DDI_VERSION` last (cheap final switch).** For the project's
   SM2 target, gate on a stock XP v4 initialization/Reset suite, fixed-function
   and VS1.1 compatibility, actual DDI-created VS2/PS2 objects, textured and
   indexed draws, teardown, and at least one unmodified SM2 game. Queries, MRT,
   multisampling, anisotropy, volume textures, extension declaration types, and
   instancing may
   remain absent if their caps/counts remain honestly absent.

### Recommended next implementation step

Keep DDI v4, SM2, and native eight-stream caps diagnostic. Implement a separate
bounded SM2 sampler table and versioned wire/host binding for s0-s15 without
expanding the four-stage fixed-function cascade. Preserve exact ownership,
generation, format, mip, and target validation for every active sampler, and
fail before transport for unsupported resource classes. Then run boundary
shaders using s4 and s15 through stock D3D9, repeat Reset/teardown and the full
ordinary matrix, and audit every diagnostic cap field before considering the
version/cap switch.
