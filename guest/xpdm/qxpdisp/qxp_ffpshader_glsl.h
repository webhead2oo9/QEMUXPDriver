#ifndef QXP_FFPSHADER_GLSL_H
#define QXP_FFPSHADER_GLSL_H

#include <windef.h>

#include "qxp_pshader_glsl.h"

/*
 * Fixed-function pixel processing expressed as compatibility GLSL.
 *
 * A Direct3D 9 title may bind a programmable vertex shader while leaving
 * pixel processing to the fixed-function texture-stage combiner.  The native
 * transport has no such mode: QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST always
 * carries a vertex source and a pixel source, and the miniport compiles,
 * attaches and links both.  Synthesizing the combiner into a pixel stage is
 * therefore what lets those draws execute natively at all.
 *
 * The emitted stage obeys the same interface contract as a compiled ps_2_0
 * stage - it reads qxpPsInV#/qxpPsInT# varyings, samples qxpPsS# and writes
 * gl_FragColor - and reports the identical metadata, so the draw path treats
 * a synthesized stage exactly like a compiled one.
 */

#define QXP_FF_PIXEL_STAGE_COUNT 4UL

/* Ceiling for one synthesized stage.  Four stages of the widest expressible
 * operation stay far below this, so it bounds the scratch the draw path
 * reserves rather than constraining any real combiner. */
#define QXP_FF_PIXEL_MAX_SOURCE 8192UL

/* Mirrors the D3DTSS state a stage contributes.  sampler_type is
 * QXP_SHADER_SAMPLER_2D/_CUBE/_VOLUME, or zero when no texture is bound. */
typedef struct qxp_ff_pixel_stage {
    ULONG color_op;
    ULONG color_arg1;
    ULONG color_arg2;
    ULONG color_arg0;
    ULONG alpha_op;
    ULONG alpha_arg1;
    ULONG alpha_arg2;
    ULONG alpha_arg0;
    ULONG coordinate_index;
    ULONG sampler_type;
} QXP_FF_PIXEL_STAGE;

/* Only combiner structure lives here, because this struct is hashed into the
 * program cache key.  The texture factor is deliberately absent: it is a
 * per-draw colour that titles animate (Prince of Persia fades its menus with
 * it), and baking it in made every frame's colour a brand new cache key,
 * recompiling the whole shader working set every frame.  It now travels as
 * the qxpTFactor uniform, fed from the draw request the miniport already
 * receives. */
typedef struct qxp_ff_pixel_state {
    QXP_FF_PIXEL_STAGE stages[QXP_FF_PIXEL_STAGE_COUNT];
} QXP_FF_PIXEL_STATE;

typedef enum qxp_ff_pixel_error_code {
    QXP_FF_PIXEL_OK = 0,
    QXP_FF_PIXEL_ERROR_NULL_ARGUMENT = 1,
    QXP_FF_PIXEL_ERROR_OPERATION = 2,
    QXP_FF_PIXEL_ERROR_ARGUMENT = 3,
    QXP_FF_PIXEL_ERROR_TEXTURE = 4,
    QXP_FF_PIXEL_ERROR_COORDINATE = 5,
    QXP_FF_PIXEL_ERROR_OUTPUT_CAPACITY = 6
} QXP_FF_PIXEL_ERROR_CODE;

typedef struct qxp_ff_pixel_error {
    DWORD code;
    DWORD stage;
    DWORD value;
} QXP_FF_PIXEL_ERROR;

/*
 * Emits the combiner chain as GLSL 1.20 and reports the interface it reads.
 * Returns FALSE without writing output when the configuration is not
 * expressible, which keeps an unsupported combiner a refused draw rather
 * than a wrongly shaded one.
 */
BOOL QxpEmitFixedFunctionPixelGlsl(
    const QXP_FF_PIXEL_STATE *state,
    char *output, DWORD output_bytes, DWORD *source_bytes,
    QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_FF_PIXEL_ERROR *error);

/*
 * Order-sensitive digest of the combiner state.  The native program cache is
 * keyed by shader bindings, which are absent for a synthesized stage, so this
 * distinguishes one combiner configuration from another.  Never zero, so a
 * synthesized entry can never collide with a compiled pixel binding.
 */
DWORD QxpFixedFunctionPixelSignature(const QXP_FF_PIXEL_STATE *state);

#endif
