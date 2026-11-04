# D3D9 native-DDI capability and format audit

## Scope and result

**2026-07-31 boundary correction:** this audit defines the native DDI-v4
diagnostic profile, not the ordinary package contract. Ordinary ABI-61 builds
decline DDI v4, emit `QXPDISP GD9 FALLBACK`, and retain the low native caps9
block documented in the tables below: `DevCaps2 = 0`, `MaxStreams = 1`,
VS1.1, and PS1.3. Exact evidence is
`artifacts/runs/20260731-abi61-d3d8-specular-fix-r140/D3d9/`; stock DDI-v3
derives 16 streams from caps8, skips PS2/SM2, and passes the 21/21 legacy
visual plus Reset. The same exact ordinary package passes the 800x600 D3D8
visual gate and the D3D8 interface regression matrix; no capability bit was
raised by that probe-only viewport correction.

The separately signed diagnostic package enables DDI v4,
`D3DDEVCAPS2_STREAMOFFSET`, eight streams, VS2, and PS2. Its
`T2-diagnostic` exact-package run at
`artifacts/runs/20260731-abi61-diagnostic-boundary-r143/D3d9Multistream7/`
passes fixed-function/SM1.x plus direct, indexed, default-pool, and
managed-texture SM2 draws. One plain-`ps_2_0` program declares, binds, and
samples all s0-s15 registers in the same draw (`mask=ffff`, center
`ffff2040`), followed by the stream-0+7 lifetime/Reset oracle. Volume
textures, MRT, queries, multisampling, and other unimplemented optional caps
remain clear in both variants.


This is the value-level companion to
`docs/design/d3d9-ddi-v4-gap-matrix.md`; it does not repeat that document's
ABI, callback, or DP2 coverage. The earlier measured native-v4 failure was
adapter-initialization rejection: `GetDeviceCaps(HAL)` returned `0x8876086a`
without touching the caller's `D3DCAPS9`, creating a device, or sending DP2,
after all 127 `GetDriverInfo2` requests were handled [GAP]. That failure
analysis remains provenance for the diagnostic contract, not a reason to
promote the contract to ordinary builds.

The strongest audit finding remains that the hand-built `QXP_D3DCAPS9` must
not copy legacy DX7/DX8-only bits into D3D9 masks. The 27-record
operation-format list is structurally complete for QXP's implemented display,
render-target, and depth formats [QXP-FMT; HOST-FMT].

### Named references

* **GAP** — `docs/design/d3d9-ddi-v4-gap-matrix.md:5-18,52-77,119-139,206-232`.
* **QXP-ABI** — `guest/xpdm/qxpdisp/directdraw.c:62-137` (304-byte local `D3DCAPS9` layout).
* **CAPS-ABI** — authoritative layout, `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/api/d3d9caps.h:107-193`.
* **QXP-CAPS** — `guest/xpdm/qxpdisp/directdraw.c:13335-13384,13647-13845`; experimental HWTL and VS 1.1 gates are both `1` at `directdraw.c:35-36`.
* **CAPS-BITS** — `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/api/d3d9caps.h:215-230,278-519`; legacy-only definitions are `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/api/d3dcaps.h:89-111,144-167,242-247,456-475` and `inc/api/d3d8caps.h:108-119`.
* **DXVK-CAPS** — DXVK `1a5919b7`, `<DXVK checkout>/src/d3d9/d3d9_adapter.cpp:390-787` and `d3d9_caps.h:5-38`.  This is a real, broad D3D9 implementation, not a minimum profile and not a list to copy.
* **WINE-CAPS** — Wine `1012f3d9`, `<Wine checkout>/dlls/wined3d/directx.c:1623-1744`.  Wine is corroboration that capability values are assembled from implemented backend limits, not a required XP profile.
* **QXP-GDI2** — `guest/xpdm/qxpdisp/directdraw.c:13900-13988` (default status, caps/DDI/AGP/format query handling).
* **QXP-FMT** — `guest/xpdm/qxpdisp/directdraw.c:13422-13444,13539-13656,13965-13988`; count is `44 - 17 = 27` (`directdraw.c:188-192`).
* **FORMAT-ABI** — `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/api/ddrawint.h:100-178`; numeric `D3DFORMAT` values are `inc/api/d3d9types.h:1298-1410`.
* **HOST-FMT** — `guest/xpdm/qxpmp/qxpmp.c:2559-2580,2608-2612` (accepted color and depth targets).
* **QXP-ZPF** — `guest/xpdm/qxpdisp/directdraw.c:660-673,13863-13880,14022-14026`.
* **DXVK-FMT** — `<DXVK checkout>/src/d3d9/d3d9_monitor.cpp:28-78` and `d3d9_adapter.cpp:110-225,322-355`.
* **WINE-FMT** — `<Wine checkout>/dlls/wined3d/directx.c:1970-2170,2260-2335` and `utils.c:1411-1465,1710-1765`.
* **ROS-RUNTIME** — ReactOS `e3c4b217`, `dll/directx/d3d9/d3d9_caps.c:509-604,784-806,860-879` and `format.c:206-299,326-520` (named source reference; corroborative, not stock-XP authority).
* **DDI-AGP** — `.tools/wdk71/WinDDK/7600.16385.win7_wdk.100208-1538/inc/ddk/d3dhal.h:2148-2159,2199-2213`.
* **MS-9_1** — Microsoft, “Direct3D feature levels,” section *9_1 feature level* (`https://learn.microsoft.com/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro`).  This later minimum is a comparison point only; **unknown:** stock XP's private HAL-acceptance predicate is not published.

## Complete `D3DCAPS9` audit

Risk is the plausibility that this field alone or as part of a consistency check rejects native v4: **H** high-value probe, **M** plausible aggregate contributor, **L** legal/conservative difference, **N** no concern.  Every QXP number below is resolved from [QXP-CAPS] with [CAPS-BITS]; every field and nested field from [CAPS-ABI] is present.  DXVK comparisons use [DXVK-CAPS].

### Header through fixed-function limits

| Field | QXP value (decoded meaning) | Real implementation comparison | Risk |
|---|---|---|---|
| `DeviceType` | `1` (`HAL`) | DXVK reports requested HAL type | N |
| `AdapterOrdinal` | `0` | DXVK reports its actual ordinal | N |
| `Caps` | `0` | DXVK: `0x00020000` (`READ_SCANLINE`); ReactOS overwrites this member from Draw caps [ROS-RUNTIME] | L |
| `Caps2` | `0x20080000`: dynamic textures plus **legacy `CANRENDERWINDOWED=0x00080000`** | DXVK uses only D3D9-defined gamma/dynamic/autogen bits | **H** |
| `Caps3` | `0` | DXVK advertises alpha-fullscreen, linear-to-sRGB, and copy directions; all optional here | L |
| `PresentationIntervals` | `0x80000001`: one-vblank and immediate | DXVK additionally supports default/two/three/four | L |
| `CursorCaps` | `0` | DXVK: color cursor | L |
| `DevCaps` | `0x0009af51`: execute/TL/texture system memory, texture video memory, TL draw, render-after-flip, DP2/DP2Ex, HWTL, HW rasterization, plus **legacy `FLOATTLVERTEX=0x1`** | DXVK has no bit `0x1`; D3D9's valid flags begin at `0x10` [CAPS-BITS] | **H** |
| `PrimitiveMiscCaps` | `0x00000870`: all three cull modes and `BLENDOP` | DXVK also has mask-Z, color-write, clipping, temp and later features | M |
| `RasterCaps` | `0x002161b1`: dither, Z-test, vertex/table/range/Z fog, mip LOD bias, plus **legacy `SUBPIXEL=0x20` and `ZBIAS=0x4000`** | DXVK uses D3D9 `SLOPESCALEDEPTHBIAS=0x02000000` and `DEPTHBIAS=0x04000000`, never the legacy bits | **H** |
| `ZCmpCaps` | `0x000000ff`: all eight comparisons | DXVK: same | N |
| `SrcBlendCaps` | `0x000007ff`: zero/one, source/destination color and inverse, source/destination alpha and inverse, source-alpha-saturate | DXVK is a superset | L |
| `DestBlendCaps` | `0x000003ff`: same except source-alpha-saturate | DXVK is a superset | L |
| `AlphaCmpCaps` | `0x000000ff`: all comparisons | DXVK: same | N |
| `ShadeCaps` | `0x000c520a`: valid Gouraud color/specular/alpha/fog plus **legacy flat-color `0x2`, flat-alpha `0x1000`, flat-fog `0x40000`** | DXVK reports exactly the four D3D9 Gouraud flags (`0x00084208`) | **H** |
| `TextureCaps` | `0x00014c85`: perspective, alpha, alpha palette, projected, cube, mip, mip-cube | DXVK additionally supports repeat-not-scaled and volume maps | L |
| `TextureFilterCaps` | `0x03030303`: valid point/linear min, mag, mip plus **legacy `NEAREST=1`, `LINEAR=2`** | DXVK uses only D3D9 min/mag/mip flags | **H** |
| `CubeTextureFilterCaps` | `0x03030303`, same decode and invalid low bits | DXVK mirrors its valid texture-filter mask | **H** |
| `VolumeTextureFilterCaps` | `0` | DXVK mirrors its texture mask; QXP advertises no volumes | L |
| `TextureAddressCaps` | `0x17`: wrap, mirror, clamp, independent U/V | DXVK adds border and mirror-once | L |
| `VolumeTextureAddressCaps` | `0` | DXVK mirrors address caps; coherent with no volumes | L |
| `LineCaps` | `0x0f`: texture, Z-test, blend, alpha-test | DXVK adds fog and AA | L |
| `MaxTextureWidth` | `2048` | DXVK: `16384` | L |
| `MaxTextureHeight` | `2048` | DXVK: `16384` | L |
| `MaxVolumeExtent` | `0` | DXVK: `8192`; coherent with no volume caps/formats | L |
| `MaxTextureRepeat` | `8192` | DXVK: `8192` | N |
| `MaxTextureAspectRatio` | `2048` | DXVK: `8192` | L |
| `MaxAnisotropy` | `1` | DXVK: `16`; QXP advertises no anisotropic-filter flags | L |
| `MaxVertexW` | `1.0e10` (bits `0x501502f9`) | DXVK: `1.0e10` | N |
| `GuardBandLeft` | `-8192.0` (`0xc6000000`) | DXVK: `-32768.0` | L |
| `GuardBandTop` | `-8192.0` (`0xc6000000`) | DXVK: `-32768.0` | L |
| `GuardBandRight` | `8192.0` (`0x46000000`) | DXVK: `32768.0` | L |
| `GuardBandBottom` | `8192.0` (`0x46000000`) | DXVK: `32768.0` | L |
| `ExtentsAdjust` | `0.0` | DXVK: `0.0` | N |
| `StencilCaps` | `0x3f`: keep, zero, replace, saturating increment/decrement, invert | DXVK adds wrapping increment/decrement and two-sided | L |
| `FVFCaps` | `0x4`: four texture-coordinate sets | DXVK: eight plus point-size | L |
| `TextureOpCaps` | `0x0280bbff`: disable/select 1/2, modulate 1x/2x/4x, add, signed add 1x/2x, subtract, four alpha blends, dot3, lerp | DXVK is a superset assembled from implemented ops | L |
| `MaxTextureBlendStages` | `4` | DXVK: `8`; MS-9_1 also uses eight, but that does not prove an XP rejection rule | M |
| `MaxSimultaneousTextures` | `4` | DXVK: `8`; four matches QXP's four implemented stages (`directdraw.c:392-479`) | L |
| `VertexProcessingCaps` | `0x3a`: material-source selection, directional/positional lights, local viewer | DXVK adds texgen, tweening and sphere-map | L |
| `MaxActiveLights` | `8` | DXVK: `8` | N |
| `MaxUserClipPlanes` | `6` | DXVK: `6` | N |
| `MaxVertexBlendMatrices` | `0` | DXVK: `4`; QXP makes no vertex-blend claim | M |
| `MaxVertexBlendMatrixIndex` | `0` | DXVK: `0` | N |
| `MaxPointSize` | `64.0` (`0x42800000`) | DXVK uses backend limit | L |
| `MaxPrimitiveCount` | `1,048,576` (`0x00100000`) | DXVK: `0x00555555` | L |
| `MaxVertexIndex` | `1,048,576` (`0x00100000`) | DXVK: `0x00ffffff` | L |
| `MaxStreams` | `1` | DXVK: `16`. Honest, but not for the reason once recorded: the declaration DDI has landed and binding/validation are multistream (`qxp_d3d9_draw_prepare.c:471`, streams 1/2/3/15 tested in `qxp_d3d9_draw_state_test.c`). **Capture** is the real limit -- it fail-closes on any nonzero backing for streams >= 1 (`qxp_d3d9_draw_capture.c:321,353,425`), so 16 would be a lie no current test would catch. Raise only with multistream capture plus a test that renders from a nonzero stream. | L |
| `MaxStreamStride` | `255` | DXVK: `508` | L |
| `VertexShaderVersion` | `0xfffe0101` (VS 1.1) | DXVK can expose VS 1.1 for its low shader profile (`d3d9_adapter.cpp:694-708`) | N |
| `MaxVertexShaderConst` | `96` | DXVK: `256`; 96 matches QXP's implemented VS 1.1 constant file (`guest/xpdm/qxp_d3d_transport.h:61`) | N |
| `PixelShaderVersion` | `0xffff0103` (PS 1.3) | DXVK supports selectable profiles; a non-SM2 D3D9 device is representable | N |
| `PixelShader1xMaxValue` | `1.0` (`0x3f800000`) | DXVK uses max float when shaders are enabled; value range differs, not structure | L |

### D3D9-specific tail and nested fields

| Field | QXP value | Real implementation comparison | Risk |
|---|---:|---|---|
| `DevCaps2` | `0` | DXVK advertises stream offset/stretch-from-texture/shared-offset; prior QXP probe filled this with no outcome change [GAP] | L |
| `MaxNpatchTessellationLevel` | `0.0` | DXVK: `0.0` | N |
| `Reserved5` | `0` | DXVK: `0` | N |
| `MasterAdapterOrdinal` | `0` | DXVK: `0` | N |
| `AdapterOrdinalInGroup` | `0` | DXVK: `0` | N |
| `NumberOfAdaptersInGroup` | `1` | DXVK: `1` | N |
| `DeclTypes` | `0` | DXVK has packed/half types; prior QXP probe changed this with no outcome change [GAP] | L |
| `NumSimultaneousRTs` | `1` | DXVK: `4`; the ABI explicitly says “at least 1” [CAPS-ABI] | N |
| `StretchRectFilterCaps` | `0` | DXVK point+linear; prior QXP probe changed it with no outcome change [GAP] | L |
| `VS20Caps` (aggregate) | all zero | DXVK zeros all children when shader model `< 2` | N |
| `VS20Caps.Caps` | `0` | same low-profile behavior | N |
| `VS20Caps.DynamicFlowControlDepth` | `0` | same low-profile behavior | N |
| `VS20Caps.NumTemps` | `0` | same low-profile behavior | N |
| `VS20Caps.StaticFlowControlDepth` | `0` | same low-profile behavior | N |
| `PS20Caps` (aggregate) | all zero | DXVK zeros all children when shader model `< 2` | N |
| `PS20Caps.Caps` | `0` | same low-profile behavior | N |
| `PS20Caps.DynamicFlowControlDepth` | `0` | same low-profile behavior | N |
| `PS20Caps.NumTemps` | `0` | same low-profile behavior | N |
| `PS20Caps.StaticFlowControlDepth` | `0` | same low-profile behavior | N |
| `PS20Caps.NumInstructionSlots` | `0` | same low-profile behavior | N |
| `VertexTextureFilterCaps` | `0` | DXVK has vertex-texture filters only for later shader profiles | N |
| `MaxVShaderInstructionsExecuted` | `0` | DXVK uses `0xffff`; prior QXP probe changed this with no outcome change [GAP] | L |
| `MaxPShaderInstructionsExecuted` | `0` | DXVK uses `0xffff`; prior QXP probe changed this with no outcome change [GAP] | L |
| `MaxVertexShader30InstructionSlots` | `0` | DXVK uses `32768` for SM3; zero is coherent with VS 1.1 | N |
| `MaxPixelShader30InstructionSlots` | `0` | DXVK uses `32768` for SM3; zero is coherent with PS 1.3 | N |

### Exact no-overclaim caps correction

The native structure should remove, not translate or replace, the legacy-only bits: `Caps2 &= ~0x00080000` → `0x20000000`; `DevCaps &= ~0x1` → `0x0009af50`; `RasterCaps &= ~0x00004020` → `0x00212191`; `ShadeCaps &= ~0x00041002` → `0x00084208`; and both 2D/cube filter masks `&= ~0x3` → `0x03030300` [QXP-CAPS; CAPS-BITS].  This is truthful cleanup.  Adding D3D9 depth-bias, more blend stages, volume support, or richer shader caps instead requires the corresponding implementation and is **not** recommended as a probe [QXP-CAPS; DXVK-CAPS].

`D3DPMISCCAPS_COLORWRITEENABLE=0x80` is a separate truthful omission: QXP consumes `D3DRS_COLORWRITEENABLE` at `directdraw.c:3113-3115`, and the miniport sends per-channel color-mask state at `guest/xpdm/qxpmp/qxpmp.c:8869-8882`.  Adding only that bit changes `PrimitiveMiscCaps` from `0x870` to `0x8f0`; all other missing DXVK bits need their own implementation audit [CAPS-BITS; QXP-CAPS].

## All 27 `GETFORMAT` records

Operation abbreviations and exact values are: `T` texture `0x1`; `C` cube texture `0x4`; `ORT` offscreen render target `0x8`; `SRT` same-format render target `0x10`; `Z` depth/stencil `0x40`; `ZA` depth usable with arbitrary color depth `0x80`; `ART` same format up to alpha render target `0x100`; `D` display mode `0x400`; `3D` 3D acceleration `0x800`; `P` offscreen plain `0x4000`; `B` bump-map instructions `0x10000` [FORMAT-ABI].  Format numbers and FourCC values come from [FORMAT-ABI] and `d3d9types.h:1338-1410`.

| Query index (array) | Format = numeric value | Bits | `dwOperations` = decoded operations | Assessment |
|---:|---|---:|---|---|
| 0 (17) | `X8R8G8B8 = 22` | 32 | `0x4c1d = T|C|ORT|SRT|D|3D|P` | Required 32-bit display/RT class present |
| 1 (18) | `A8R8G8B8 = 21` | 32 | `0x410d = T|C|ORT|ART|P` | Alpha-compatible 32-bit RT present |
| 2 (19) | `R5G6B5 = 23` | 16 | `0x4c1d = T|C|ORT|SRT|D|3D|P` | Required 16-bit display/RT class present |
| 3 (20) | `A1R5G5B5 = 25` | 16 | `0x4005 = T|C|P` | Texture only; conservative |
| 4 (21) | `A4R4G4B4 = 26` | 16 | `0x4005 = T|C|P` | Texture only |
| 5 (22) | `L8 = 50` | 8 | `0x4005 = T|C|P` | Texture only |
| 6 (23) | `A8L8 = 51` | 16 | `0x4005 = T|C|P` | Texture only |
| 7 (24) | `A8 = 28` | 8 | `0x4005 = T|C|P` | Texture only |
| 8 (25) | `DXT1 = 0x31545844` (`'DXT1'`) | 0 | `0x5 = T|C` | Compressed texture; no bogus pixel size |
| 9 (26) | `DXT3 = 0x33545844` (`'DXT3'`) | 0 | `0x5 = T|C` | Compressed texture |
| 10 (27) | `DXT5 = 0x35545844` (`'DXT5'`) | 0 | `0x5 = T|C` | Compressed texture |
| 11 (28) | `X1R5G5B5 = 24` | 16 | `0x4005 = T|C|P` | Not claimed as display/RT |
| 12 (29) | `X4R4G4B4 = 30` | 16 | `0x4005 = T|C|P` | Texture only |
| 13 (30) | `R8G8B8 = 20` | 24 | `0x4005 = T|C|P` | Texture only |
| 14 (31) | `R3G3B2 = 27` | 8 | `0x4005 = T|C|P` | Texture only |
| 15 (32) | `V8U8 = 60` | 16 | `0x10001 = T|B` | Bump texture |
| 16 (33) | `L6V5U5 = 61` | 16 | `0x10001 = T|B` | Bump+luminance texture |
| 17 (34) | `D16 = 80` | 16 | `0xc0 = Z|ZA` | Required depth class present and backend-backed |
| 18 (35) | `D24S8 = 75` | 32 | `0xc0 = Z|ZA` | Required depth+stencil class present and backend-backed |
| 19 (36) | `VERTEXDATA = 100` | 0 | `0` | Presence denotes buffer-format support |
| 20 (37) | `INDEX16 = 101` | 16 | `0` | 16-bit index format present |
| 21 (38) | `INDEX32 = 102` | 32 | `0` | 32-bit index format present |
| 22 (39) | `P8 = 41` | 8 | `0x4005 = T|C|P` | Palettized texture |
| 23 (40) | `A8P8 = 40` | 16 | `0x4005 = T|C|P` | Alpha-palettized texture |
| 24 (41) | `A8R3G3B2 = 29` | 16 | `0x4005 = T|C|P` | Texture only |
| 25 (42) | `A4L4 = 52` | 8 | `0x4005 = T|C|P` | Texture only |
| 26 (43) | `D24X8 = 77` | 32 | `0xc0 = Z|ZA` | Required 24-bit depth class present and backend-backed |

Each returned `DDPIXELFORMAT` is zero-initialized, has `dwSize=32`, `DDPF_D3DFORMAT=0x00200000`, its numeric format in `dwFourCC`, and the listed private bit count/operations [QXP-FMT; FORMAT-ABI].  The handler bounds-checks `0..26` and copies exactly one record [QXP-FMT].  DXVK and Wine expose richer sets but make the same semantic separation between adapter/display, render-target, and depth/stencil usability [DXVK-FMT; WINE-FMT].

### Mandatory-class check and the separate Z path

For the conservative QXP device, the operation list has both implemented display formats (`X8R8G8B8`, `R5G6B5`), same-format RT support for each, alpha-compatible `A8R8G8B8` RT support, and all three depth formats accepted by the miniport (`D16`, `D24S8`, `D24X8`) [QXP-FMT; HOST-FMT].  Thus there is no currently missing core display/RT/Z record.  MS-9_1 and modern implementations are useful corroboration for these classes, but **unknown:** no inspected source proves that stock XP requires precisely that later profile during native-v4 initialization [MS-9_1; DXVK-FMT; WINE-FMT].

`GUID_ZPixelFormats` is separate: QXP returns a fixed 32-entry DX7 table containing D16, D24S8, D24X8 followed by 29 zero records [QXP-ZPF].  `GETFORMAT` independently returns those same three formats with `Z|ZA` [QXP-FMT].  ReactOS caches `GETFORMAT` records as `pSupportedFormatOps`, uses those operations for `CheckDeviceFormat`/depth matching, and frees its separately fetched Z-pixel list after loading [ROS-RUNTIME].  Therefore the legacy Z list cannot substitute for a missing D3D9 operation record.  **Unknown:** the exact stock-XP internal use of the Z list is closed-source, but the measured 27 `GETFORMAT` queries and the documented operation contract make `GETFORMAT` the defensible D3D9 source [GAP; FORMAT-ABI].

Do not add D32: QXP's host target validator accepts only X8R8G8B8, R5G6B5, A8R8G8B8, D16, D24S8, and D24X8 [HOST-FMT].  Advertising D32 or additional RT/depth usages would be dishonest until implemented.

## Ranked causes and cheapest VM tests

1. **`DEFERRED_AGP_AWARE` returns failure (high).**  Type `0x18` tells the driver the runtime will later notify it after the last AGP lock; it has no output payload [DDI-AGP].  QXP leaves `dwActualSize=0`, `ddRVal=0x88760028`, then returns success for the `0x20` “start deferring” notification [QXP-GDI2].  This is the only unsuccessful measured native query and is semantically inconsistent even with no advertised AGP heap [GAP].  **Test:** for `0x18` only, set `dwActualSize=0`, `ddRVal=DD_OK`; keep `0x19/0x20` successful no-ops.  No capability is invented.
2. **Legacy/undefined bits in native `D3DCAPS9` (high).**  Native v4 consumes the hand-built structure directly, unlike a legacy path that can translate/mask old caps [QXP-CAPS; ROS-RUNTIME].  **Test:** apply only the five sanitizing masks in “Exact no-overclaim caps correction”; log the returned 304 bytes.  This only removes claims.
3. **A small native-capability floor/consistency check (medium).**  The best honest addition is `COLORWRITEENABLE`, already implemented end-to-end; `MaxTextureBlendStages=4` and zero blend matrices differ from DXVK/MS-9_1 but cannot be raised without implementation [QXP-CAPS; DXVK-CAPS; MS-9_1].  **Test:** after tests 1–2, add only `0x80` to `PrimitiveMiscCaps`.  Do not repeat the already-negative `DevCaps2`/`DeclTypes`/stretch/instruction-limit bundle [GAP].
4. **Operation-list content (low after this audit).**  All core classes and backend-backed depth formats are present, and every record is well formed [QXP-FMT; HOST-FMT].  **Test:** if 1–3 fail, temporarily return a truthful nine-record subset: X8R8G8B8, A8R8G8B8, R5G6B5, D16, D24S8, D24X8, VERTEXDATA, INDEX16, INDEX32.  A success implicates an optional record; restore classes incrementally.  Removing support is conservative; adding unsupported ops is forbidden.
5. **Unknown stock-XP private predicate (residual).**  ReactOS validates sizes/status and one deprecated vertex-declaration condition, but it is not stock XP and does not implement the measured AGP tail [ROS-RUNTIME].  If all probes fail, capture every query's type, expected size, actual size, `ddRVal`, and returned bytes, then compare with a known native-v4 XPDM driver.  Do not guess new caps.

### Safe grouping

For the cheapest first reboot, group: (a) successful no-op `0x18`, (b) all legacy-bit removals, and (c) `COLORWRITEENABLE` [DDI-AGP; CAPS-BITS; QXP-CAPS].  The group is safe because it acknowledges a notification, deletes undefined claims, and exposes already implemented color masking; it adds no unsupported rendering feature.  If HAL appears, split those three changes in the next build to identify the trigger.  If it does not, keep (a) and (b) as correctness fixes, remove (c) if desired, and use the nine-format subset as a separate test.  Never group a format overclaim, eight-stage report, new depth format, shader-model increase, or MRT increase without implementing and testing that feature [HOST-FMT; QXP-CAPS].
