#ifndef QXP_POSITIONT_VSHADER_GLSL_H
#define QXP_POSITIONT_VSHADER_GLSL_H

#include <windef.h>

#include "qxp_vshader20_glsl.h"

/*
 * Pre-transformed vertex processing expressed as compatibility GLSL.
 *
 * A Direct3D 9 title draws screen-space geometry - menus, HUD, full-screen
 * overlays - through a declaration carrying D3DDECLUSAGE_POSITIONT.  The
 * runtime then ignores any bound vertex shader and processes the vertices as
 * already transformed.  When such a draw also binds a ps_2_0 pixel shader the
 * native transport has no route for it: QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST
 * always carries a vertex source and a pixel source, and the miniport
 * compiles, attaches and links both.  Synthesizing the pre-transformed stage
 * into a vertex stage is therefore what lets those draws execute at all.
 *
 * The emitted stage obeys the same interface contract as a compiled vs_2_0
 * stage - it reads qxpVsInput# attributes, writes qxpPsInV#/qxpPsInT#
 * varyings, applies the shared qxpVsClipControl epilogue and reports
 * QXP_VERTEX_SHADER_GLSL_METADATA - so the draw path treats a synthesized
 * stage exactly like a compiled one.
 *
 * Attribute locations are fixed by this contract rather than by a shader's
 * input declarations, because there is no shader to declare them:
 *
 *   qxpVsInput0        POSITIONT     (x, y in viewport pixels; z depth; w rhw)
 *   qxpVsInput1        COLOR0
 *   qxpVsInput2        COLOR1
 *   qxpVsInput3 + k    TEXCOORD k    (k < QXP_POSITIONT_TEXCOORD_COUNT)
 */

#define QXP_POSITIONT_TEXCOORD_COUNT 8UL
#define QXP_POSITIONT_COLOR_COUNT 2UL

#define QXP_POSITIONT_INPUT_POSITION 0UL
#define QXP_POSITIONT_INPUT_COLOR 1UL
#define QXP_POSITIONT_INPUT_TEXCOORD 3UL

/* Ceiling for one synthesized stage.  Both colours and eight four-component
 * texture coordinates stay far below this, so it bounds the scratch the draw
 * path reserves rather than constraining any real declaration. */
#define QXP_POSITIONT_MAX_SOURCE 8192UL

/*
 * What the declaration supplies, reduced to the semantics a pre-transformed
 * stage can forward.  A declaration element the stage cannot express is a
 * refused draw, never a silently dropped attribute.
 */
typedef struct qxp_positiont_vertex_state {
    /* Bit k set when COLOR<k> is declared. */
    DWORD color_mask;
    /* Bit k set when TEXCOORD<k> is declared. */
    DWORD texcoord_mask;
    /* Declared component count, 1 to 4, for every bit set in texcoord_mask.
     * Entries outside the mask are zero. */
    BYTE texcoord_components[QXP_POSITIONT_TEXCOORD_COUNT];
} QXP_POSITIONT_VERTEX_STATE;

typedef enum qxp_positiont_vertex_error_code {
    QXP_POSITIONT_VERTEX_OK = 0,
    QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT = 1,
    QXP_POSITIONT_VERTEX_ERROR_COLOR = 2,
    QXP_POSITIONT_VERTEX_ERROR_TEXCOORD = 3,
    QXP_POSITIONT_VERTEX_ERROR_OUTPUT_CAPACITY = 4
} QXP_POSITIONT_VERTEX_ERROR_CODE;

typedef struct qxp_positiont_vertex_error {
    DWORD code;
    DWORD index;
    DWORD value;
} QXP_POSITIONT_VERTEX_ERROR;

/*
 * Emits the pre-transformed vertex stage.  output_bytes includes the trailing
 * NUL and source_bytes excludes it.  Passing NULL/zero for output/output_bytes
 * is a count-only query, so the draw path can size its scratch exactly.
 *
 * Fail-closed and deterministic: on every failure the metadata and
 * source_bytes are zeroed and output[0] is set to 0 when possible.  No
 * allocation is performed and no pointer is retained.  Returns FALSE without
 * writing output when the declaration is not expressible, which keeps an
 * unsupported layout a refused draw rather than a wrongly placed primitive.
 */
BOOL QxpEmitPositiontVertexGlsl(
    const QXP_POSITIONT_VERTEX_STATE *state,
    char *output, DWORD output_bytes, DWORD *source_bytes,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_POSITIONT_VERTEX_ERROR *error);

/*
 * Order-sensitive digest of the declaration state.  The native program cache
 * is keyed by shader bindings, which do not describe a synthesized stage, so
 * this distinguishes one pre-transformed layout from another.  Never zero, so
 * a synthesized entry can never collide with a compiled vertex binding.
 */
DWORD QxpPositiontVertexSignature(const QXP_POSITIONT_VERTEX_STATE *state);

#endif
