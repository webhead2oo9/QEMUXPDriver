# Direct3D 8 fixed-function texture-stage contract

This document defines XPDriver's fixed-function texture-stage behavior through
the stock Windows XP Direct3D runtimes. It covers stage activation, null texture
bindings, the host placeholder object, and extension-dependent translation. It
does not change the public pixel-shader or guest/host packet ABI.

## Fixed-stage prefix

The active fixed-function pipeline is a contiguous prefix of stages zero through
three. The first stage whose `D3DTSS_COLOROP` is `D3DTOP_DISABLE` terminates that
stage and every higher stage. Higher-stage values retained by a runtime or state
block are stale state and must not make a terminated stage active.

The display driver applies the same prefix when preparing texture handles and
texture coordinates. The miniport independently enforces it before translating
the request, so malformed or stale higher-stage state cannot bypass the contract.

## Null-binding behavior

`SetTexture(stage, NULL)` is legal. The result depends on whether the active
fixed-function equation actually samples `D3DTA_TEXTURE`:

| Pipeline state | XPDriver behavior |
| --- | --- |
| `COLOROP=DISABLE`, or a lower stage already terminated the prefix | Disable and unbind both host texture targets for the stage. |
| Active fixed stage, null binding, equation samples the texture explicitly or implicitly | Disable/unbind the host unit so the stage is a pass-through. |
| Active fixed stage, null binding, equation uses only `CURRENT`, `DIFFUSE`, or `TFACTOR` | Bind the complete driver placeholder texture so the OpenGL texture unit runs the non-sampling equation. |
| Programmable pixel stage samples a null binding | Reject the draw request; no placeholder may satisfy a shader sample. |

Implicit sampling includes texture-alpha blend factors, premodulate, bump-map
operations, and any unknown operation. Unknown operations fail conservatively
rather than assuming that an unbound texture is unused.

The placeholder uses reserved host texture name `0xfffffffe`. The guest surface
allocator skips that value, guest texture-definition requests cannot claim it,
and the miniport deletes it when the D3D transport closes. It is a complete 1x1
2D object whose texel is never observed by the permitted equations; it is not a
presentation member, render target, or guest-visible texture.

## Texture-operation translation

The pure planner in `guest/xpdm/qxpmp/qxp_texture_op.c` determines argument
sampling and maps exact equations independently of MMIO/FIFO submission. The
miniport translates ordinary fixed operations through `GL_COMBINE`. Operations
18 through 21 (`MODULATEALPHA_ADDCOLOR`, `MODULATECOLOR_ADDALPHA`,
`MODULATEINVALPHA_ADDCOLOR`, and `MODULATEINVCOLOR_ADDALPHA`) use
`GL_NV_texture_env_combine4` when present and the ATI three-source form when it
can represent the equation.

Host extension queries clear the optional legacy extension-year payload before
each `glGetString(GL_EXTENSIONS)` request. Capability results therefore do not
inherit draw payload from a previous process or session.

This checkpoint does not advertise new `D3DTEXOPCAPS` bits. The four special
translations remain host-extension-dependent and accept incoming legal state;
advertising them requires a future capability path that can prove the paired
host support before the stock runtime sees the caps.

## Invariants and failure policy

- Transport version remains 43; no request layout or host ABI changes.
- The reserved placeholder name is never allocated to a guest surface.
- A fixed stage with a missing sampled texture never aliases another stage,
  render target, or presentation member.
- A programmable sample never receives the placeholder.
- Texture-state and vertex-array submission failures rewind their FIFO/data
  reservations and emit a bounded one-shot diagnostic.
- Reset, context destruction, process succession, and transport close retain
  their existing ownership and cleanup rules.

## Deterministic coverage

The portable `qxp_texture_op_test` covers all special operation plans,
complements, alpha plans, scaling, null output rejection, implicit/explicit
sampling, unknown operations, and disabled-stage termination.

The stock-D3D8 exact probe binds no texture at stage zero and selects
`TFACTOR=0xff102030`, then binds a 16x16 `0xff808080` texture at stage one and
adds `TEXTURE + CURRENT`. Exact output `0x90A0B0` proves that the null stage ran
its non-sampling equation and fed the higher textured stage. Broad D3D8, legacy
D3D9, Reset, VS1.1, simultaneous-context, process-succession, native-OpenGL,
and game gates remain mandatory for a versioned checkpoint because this state
crosses draw preparation, host translation, and cleanup.
