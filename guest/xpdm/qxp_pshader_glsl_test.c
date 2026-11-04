#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxp_pshader_glsl.h"

#define QXP_ARRAY_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_PARAMETER      0x80000000UL
#define QXP_CANARY_A       0x13579bdfUL
#define QXP_CANARY_B       0x2468ace0UL

#define QXP_INST(op, length) \
    ((DWORD)(op) | ((DWORD)(length) << D3DSI_INSTLENGTH_SHIFT))
#define QXP_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_DST(type, number, mask, modifier) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     ((DWORD)(mask) << 16) | (DWORD)(modifier))
#define QXP_SRC_SWIZZLE(type, number, swizzle, modifier) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     (DWORD)(swizzle) | (DWORD)(modifier))
#define QXP_SRC(type, number) \
    QXP_SRC_SWIZZLE(type, number, D3DSP_NOSWIZZLE, D3DSPSM_NONE)
#define QXP_SRC_NEG(type, number) \
    QXP_SRC_SWIZZLE(type, number, D3DSP_NOSWIZZLE, D3DSPSM_NEG)
#define QXP_REPLICATE(component) \
    (((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 0)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 2)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 4)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 6)))
#define QXP_SCALAR(type, number, component) \
    QXP_SRC_SWIZZLE(type, number, QXP_REPLICATE(component), D3DSPSM_NONE)
#define QXP_DCL_SAMPLER(type) (QXP_PARAMETER | (DWORD)(type))
/* ps_1_1 _x2 destination result shift: raw D3DSP_DSTSHIFT field value 1. */
#define QXP_DST_X2 ((DWORD)(1UL << D3DSP_DSTSHIFT_SHIFT))

typedef struct qxp_guarded_metadata {
    DWORD before[8];
    QXP_PIXEL_SHADER_GLSL_METADATA value;
    DWORD after[8];
} QXP_GUARDED_METADATA;

typedef struct qxp_guarded_error {
    DWORD before[8];
    QXP_PIXEL_SHADER_GLSL_ERROR value;
    DWORD after[8];
} QXP_GUARDED_ERROR;

static QXP_SHADER_IR qxp_ir_a;
static QXP_SHADER_IR qxp_ir_b;
static QXP_SHADER_DECODE_ERROR qxp_decode_error;
static QXP_GUARDED_METADATA qxp_metadata_a;
static QXP_GUARDED_METADATA qxp_metadata_b;
static QXP_GUARDED_ERROR qxp_error_a;
static QXP_GUARDED_ERROR qxp_error_b;
static char qxp_source_a[QXP_PIXEL_SHADER_GLSL_MAX_SOURCE + 1];
static char qxp_source_b[QXP_PIXEL_SHADER_GLSL_MAX_SOURCE + 1];
static DWORD qxp_work_tokens[4096];
static int qxp_failures;

static const DWORD qxp_minimal_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_noncentroid_link_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

static const DWORD qxp_constant_port_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD qxp_input_port_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD qxp_texture_port_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD qxp_matrix_port_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 7, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 1, 7, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_M3x2, 3),
        QXP_DST(D3DSPR_TEMP, 0, 3, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEXTURE, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_input_matrix_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_M3x2, 3),
        QXP_DST(D3DSPR_TEMP, 0, 3, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_texkill_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 7, D3DSPDM_NONE),
    QXP_INST(D3DSIO_TEXKILL, 1),
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_interface_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 0, 1,
                D3DSPDM_PARTIALPRECISION | D3DSPDM_MSAMPCENTROID),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 3, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 7, 15, D3DSPDM_NONE),
        0x3e800000UL, 0x3f000000UL, 0x3f400000UL, 0x3f800000UL,
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 3, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

/* Ground-truth first Sands of Time pixel shader: ps_1_1 SM1 tokens
 * carry a zero length field, `tex t0` names its sampler implicitly, the
 * first MUL scales by _x2, and the second MUL co-issues beside it. */
static const DWORD qxp_ps11_first_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 0, 7, QXP_DST_X2),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_MUL | D3DSI_COISSUE,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* Four stage-implicit samplers feeding a mul/mad chain. */
static const DWORD qxp_ps11_four_sampler_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 2, 15, D3DSPDM_NONE),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 3, 15, D3DSPDM_NONE),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_TEXTURE, 1),
    D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 2), QXP_SRC(D3DSPR_TEXTURE, 3),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_END
};

/* t# reads with no preceding tex see the interpolated stage coordinate,
 * including its .w channel; _sat clamps the stored component. */
static const DWORD qxp_ps11_coordinate_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_SATURATE),
        QXP_SCALAR(D3DSPR_TEXTURE, 0, 3),
    D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 1), QXP_SRC(D3DSPR_TEXTURE, 1),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

/* Ground-truth Sands of Time shader that blends two sampled stages.  Its lrp
 * is what exposed the slot-cost disagreement between the decoder and the
 * emitter schema: ps_1_1 charges lrp one arithmetic slot, ps_2_0 charges two,
 * and the cross-check rejected the whole shader while they disagreed. */
static const DWORD qxp_ps11_blend_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_SATURATE),
        QXP_SCALAR(D3DSPR_TEXTURE, 0, 3),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 1),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 0, 7, QXP_DST_X2),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 1, 7, QXP_DST_X2),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_LRP,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 3), QXP_SRC(D3DSPR_TEMP, 1),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

/* Need for Speed: Underground 2 shape (r542 guest capture): a `mad`
 * destination carries the _x4 result shift together with _sat.  D3D
 * applies the multiply before the saturate and the write mask. */
static const DWORD qxp_ps11_nfsu2_mad_x4_sat_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 7,
                D3DSPDM_SATURATE |
                ((DWORD)2 << D3DSP_DSTSHIFT_SHIFT)),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

/* Exact second Need for Speed: Underground 2 pixel shader from the r543
 * guest capture: texreg2ar/texreg2gb dependent reads feed an add/mul/lrp
 * chain, and the final mad carries the _comp source modifier. */
static const DWORD qxp_ps11_nfsu2_texreg_ps[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_DEF,
        QXP_DST(D3DSPR_CONST, 0, 15, D3DSPDM_NONE),
        0x3f800000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    D3DSIO_TEX,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEXREG2AR,
        QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_TEXREG2GB,
        QXP_DST(D3DSPR_TEXTURE, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEXTURE, 1, 3),
        QXP_SCALAR(D3DSPR_CONST, 0, 3),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 1, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 2), QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_MUL,
        QXP_DST(D3DSPR_TEXTURE, 2, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 1), QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_LRP,
        QXP_DST(D3DSPR_TEMP, 1, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 2),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_MUL | D3DSI_COISSUE,
        QXP_DST(D3DSPR_TEMP, 1, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEXTURE, 1, 3),
        QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_SRC_SWIZZLE(D3DSPR_TEMP, 0, QXP_REPLICATE(3),
                        D3DSPSM_COMP),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_pshader_glsl: failed: %s\n", name);
}

static void qxp_prepare_guards(void)
{
    DWORD index;

    memset(&qxp_metadata_a, 0xcd, sizeof(qxp_metadata_a));
    memset(&qxp_metadata_b, 0xab, sizeof(qxp_metadata_b));
    memset(&qxp_error_a, 0x5a, sizeof(qxp_error_a));
    memset(&qxp_error_b, 0xa5, sizeof(qxp_error_b));
    for (index = 0; index < 8; ++index) {
        qxp_metadata_a.before[index] = QXP_CANARY_A + index;
        qxp_metadata_a.after[index] = QXP_CANARY_B - index;
        qxp_metadata_b.before[index] = QXP_CANARY_B + index;
        qxp_metadata_b.after[index] = QXP_CANARY_A - index;
        qxp_error_a.before[index] = QXP_CANARY_B + index;
        qxp_error_a.after[index] = QXP_CANARY_A - index;
        qxp_error_b.before[index] = QXP_CANARY_A + index;
        qxp_error_b.after[index] = QXP_CANARY_B - index;
    }
}

static int qxp_guards_ok(void)
{
    DWORD index;

    for (index = 0; index < 8; ++index) {
        if (qxp_metadata_a.before[index] != QXP_CANARY_A + index ||
            qxp_metadata_a.after[index] != QXP_CANARY_B - index ||
            qxp_metadata_b.before[index] != QXP_CANARY_B + index ||
            qxp_metadata_b.after[index] != QXP_CANARY_A - index ||
            qxp_error_a.before[index] != QXP_CANARY_B + index ||
            qxp_error_a.after[index] != QXP_CANARY_A - index ||
            qxp_error_b.before[index] != QXP_CANARY_A + index ||
            qxp_error_b.after[index] != QXP_CANARY_B - index)
            return 0;
    }
    return 1;
}

static int qxp_decode(const DWORD *tokens, DWORD count)
{
    memset(&qxp_ir_a, 0, sizeof(qxp_ir_a));
    memset(&qxp_decode_error, 0, sizeof(qxp_decode_error));
    return QxpDecodeShader20(
        tokens, count, &qxp_ir_a, &qxp_decode_error) &&
        qxp_decode_error.code == QXP_SHADER_DECODE_OK;
}

static int qxp_emit_current(const char *name)
{
    DWORD bytes_a;
    DWORD bytes_b;
    DWORD queried;
    BOOL result_a;
    BOOL result_b;

    qxp_prepare_guards();
    memset(qxp_source_a, 0x6b, sizeof(qxp_source_a));
    memset(qxp_source_b, 0x7c, sizeof(qxp_source_b));
    queried = 0;
    result_a = QxpEmitPixelShader20Glsl(
        &qxp_ir_a, NULL, 0, &queried, &qxp_metadata_a.value,
        &qxp_error_a.value);
    if (!result_a || !queried ||
        qxp_error_a.value.code != QXP_PIXEL_SHADER_GLSL_OK) {
        qxp_check(0, name);
        return 0;
    }
    bytes_a = 0;
    result_a = QxpEmitPixelShader20Glsl(
        &qxp_ir_a, qxp_source_a, (DWORD)sizeof(qxp_source_a),
        &bytes_a, &qxp_metadata_a.value, &qxp_error_a.value);
    bytes_b = 0;
    result_b = QxpEmitPixelShader20Glsl(
        &qxp_ir_a, qxp_source_b, (DWORD)sizeof(qxp_source_b),
        &bytes_b, &qxp_metadata_b.value, &qxp_error_b.value);
    if (!result_a || !result_b || bytes_a != queried ||
        bytes_a != bytes_b || qxp_source_a[bytes_a] != 0 ||
        memcmp(qxp_source_a, qxp_source_b, bytes_a + 1) != 0 ||
        memcmp(&qxp_metadata_a.value, &qxp_metadata_b.value,
               sizeof(qxp_metadata_a.value)) != 0 ||
        qxp_error_a.value.code != QXP_PIXEL_SHADER_GLSL_OK ||
        qxp_error_b.value.code != QXP_PIXEL_SHADER_GLSL_OK ||
        !qxp_guards_ok()) {
        qxp_check(0, name);
        return 0;
    }
    return 1;
}

static DWORD qxp_build_arithmetic_shader(DWORD opcode)
{
    DWORD cursor;
    DWORD source_count;
    DWORD destination_mask;
    DWORD index;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    source_count = 0;
    if (opcode == D3DSIO_MOV || opcode == D3DSIO_RCP ||
        opcode == D3DSIO_RSQ || opcode == D3DSIO_EXP ||
        opcode == D3DSIO_LOG || opcode == D3DSIO_FRC ||
        opcode == D3DSIO_ABS || opcode == D3DSIO_NRM)
        source_count = 1;
    else if (opcode == D3DSIO_ADD || opcode == D3DSIO_SUB ||
             opcode == D3DSIO_MUL || opcode == D3DSIO_DP3 ||
             opcode == D3DSIO_DP4 || opcode == D3DSIO_MIN ||
             opcode == D3DSIO_MAX || opcode == D3DSIO_M4x4 ||
             opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x4 ||
             opcode == D3DSIO_M3x3 || opcode == D3DSIO_M3x2 ||
             opcode == D3DSIO_POW || opcode == D3DSIO_CRS)
        source_count = 2;
    else if (opcode == D3DSIO_MAD || opcode == D3DSIO_LRP ||
             opcode == D3DSIO_SINCOS || opcode == D3DSIO_CMP ||
             opcode == D3DSIO_DP2ADD)
        source_count = 3;
    if (opcode == D3DSIO_NOP) {
        qxp_work_tokens[cursor++] = QXP_INST(opcode, 0);
    } else {
        for (index = 1; index <= 3; ++index) {
            qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
            qxp_work_tokens[cursor++] =
                QXP_DST(D3DSPR_TEMP, index, 15, D3DSPDM_NONE);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
        }
        destination_mask = 15;
        if (opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x3 ||
            opcode == D3DSIO_CRS || opcode == D3DSIO_NRM)
            destination_mask = 7;
        else if (opcode == D3DSIO_M3x2 || opcode == D3DSIO_SINCOS)
            destination_mask = 3;
        qxp_work_tokens[cursor++] = QXP_INST(opcode, source_count + 1);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, 0, destination_mask, D3DSPDM_NONE);
        if (opcode == D3DSIO_SINCOS) {
            qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 1);
        } else if (opcode == D3DSIO_POW) {
            qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
            qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 2, 1);
        } else if (opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 ||
                   opcode == D3DSIO_M3x4 || opcode == D3DSIO_M3x3 ||
                   opcode == D3DSIO_M3x2) {
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 1);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 4);
        } else if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
                   opcode == D3DSIO_EXP || opcode == D3DSIO_LOG) {
            qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
        } else if (opcode == D3DSIO_DP2ADD) {
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 1);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 2);
            qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
        } else {
            for (index = 0; index < source_count; ++index)
                qxp_work_tokens[cursor++] =
                    QXP_SRC(D3DSPR_TEMP, index + 1);
        }
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_arithmetic_coverage(void)
{
    static const DWORD opcodes[] = {
        D3DSIO_NOP, D3DSIO_MOV, D3DSIO_ADD, D3DSIO_SUB, D3DSIO_MAD,
        D3DSIO_MUL, D3DSIO_RCP, D3DSIO_RSQ, D3DSIO_DP3, D3DSIO_DP4,
        D3DSIO_MIN, D3DSIO_MAX, D3DSIO_EXP, D3DSIO_LOG, D3DSIO_LRP,
        D3DSIO_FRC, D3DSIO_M4x4, D3DSIO_M4x3, D3DSIO_M3x4,
        D3DSIO_M3x3, D3DSIO_M3x2, D3DSIO_POW, D3DSIO_CRS,
        D3DSIO_ABS, D3DSIO_NRM, D3DSIO_SINCOS, D3DSIO_CMP,
        D3DSIO_DP2ADD
    };
    static const char *snippets[] = {
        "/* nop */", "qxpValue = qxpArg0;",
        "qxpArg0 + qxpArg1", "qxpArg0 - qxpArg1",
        "qxpArg0*qxpArg1 + qxpArg2", "qxpArg0 * qxpArg1",
        "1.0/qxpArg0.x", "inversesqrt(abs(qxpArg0.x))",
        "dot(qxpArg0.xyz, qxpArg1.xyz)",
        "dot(qxpArg0, qxpArg1)", "qxpPsMin", "qxpPsMax",
        "exp2(qxpArg0.x)", "log2(abs(qxpArg0.x))",
        "(vec4(1.0)-qxpArg0)*qxpArg2", "fract(qxpArg0)",
        "qxpValue.w = dot(qxpArg0, qxpArg1);",
        "qxpValue.z = dot(qxpArg0, qxpArg1);",
        "qxpValue.w = dot(qxpArg0.xyz, qxpArg1.xyz);",
        "qxpValue.z = dot(qxpArg0.xyz, qxpArg1.xyz);",
        "qxpValue.y = dot(qxpArg0.xyz, qxpArg1.xyz);",
        "pow(abs(qxpArg0.x), qxpArg1.x)", "cross(qxpArg0.xyz",
        "abs(qxpArg0)", "3.402823466e+38",
        "cos(qxpArg0.x), sin(qxpArg0.x)", "qxpPsCmp",
        "dot(qxpArg0.xy, qxpArg1.xy) + qxpArg2.x"
    };
    DWORD index;
    DWORD count;

    qxp_check(QXP_ARRAY_COUNT(opcodes) == QXP_ARRAY_COUNT(snippets),
              "opcode/snippet table shape");
    for (index = 0; index < QXP_ARRAY_COUNT(opcodes); ++index) {
        count = qxp_build_arithmetic_shader(opcodes[index]);
        if (!qxp_decode(qxp_work_tokens, count)) {
            qxp_check(0, "arithmetic coverage decoder input");
            break;
        }
        if (!qxp_emit_current("arithmetic coverage deterministic emit"))
            break;
        if (!strstr(qxp_source_a, snippets[index])) {
            fprintf(stderr, "qxp_pshader_glsl: opcode %lu missing %s\n",
                    (unsigned long)opcodes[index], snippets[index]);
            qxp_check(0, "arithmetic semantic snippet");
            break;
        }
    }
}

static DWORD qxp_build_texture_shader(
    DWORD sampler_type, DWORD control, DWORD sampler_register)
{
    DWORD cursor;
    DWORD required_mask;

    cursor = 0;
    required_mask = sampler_type == D3DSTT_2D ? 3 : 7;
    if (control == D3DSI_TEXLD_PROJECT ||
        control == D3DSI_TEXLD_BIAS) required_mask |= 8;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEXTURE, 0, required_mask, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_DCL_SAMPLER(sampler_type);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_SAMPLER, sampler_register, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEX, 3) | control;
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_PARTIALPRECISION);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
    qxp_work_tokens[cursor++] =
        QXP_SRC(D3DSPR_SAMPLER, sampler_register);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_texture_coverage(void)
{
    static const DWORD types[] = {
        D3DSTT_2D, D3DSTT_CUBE, D3DSTT_VOLUME
    };
    static const DWORD controls[] = {
        0, D3DSI_TEXLD_PROJECT, D3DSI_TEXLD_BIAS
    };
    static const char *dimensions[] = {
        "texture2D", "textureCube", "texture3D"
    };
    static const DWORD sampler_registers[] = { 3, 15 };
    DWORD type_index;
    DWORD control_index;
    DWORD sampler_index;
    DWORD sampler_register;
    char function_name[64];
    DWORD count;

    for (type_index = 0; type_index < QXP_ARRAY_COUNT(types);
         ++type_index) {
        for (control_index = 0;
             control_index < QXP_ARRAY_COUNT(controls);
             ++control_index) {
            for (sampler_index = 0;
                 sampler_index < QXP_ARRAY_COUNT(sampler_registers);
                 ++sampler_index) {
                sampler_register = sampler_registers[sampler_index];
                count = qxp_build_texture_shader(
                    types[type_index], controls[control_index],
                    sampler_register);
                qxp_check(qxp_decode(qxp_work_tokens, count),
                          "texture coverage decoder input");
                if (!qxp_emit_current(
                        "texture coverage deterministic emit"))
                    return;
                _snprintf(
                    function_name, sizeof(function_name), "%s(qxpPsS%lu",
                    dimensions[type_index],
                    (unsigned long)sampler_register);
                function_name[sizeof(function_name) - 1] = '\0';
                qxp_check(strstr(qxp_source_a, function_name) != NULL,
                          "texture dimension and register lowering");
                if (controls[control_index] == D3DSI_TEXLD_PROJECT)
                    qxp_check(strstr(qxp_source_a, "/ qxpArg0.w") != NULL,
                              "projective texture lowering");
                if (controls[control_index] == D3DSI_TEXLD_BIAS)
                    qxp_check(strstr(qxp_source_a, ", qxpArg0.w);") != NULL,
                              "biased texture lowering");
                qxp_check(
                    qxp_metadata_a.value.sampler_mask ==
                        (1UL << sampler_register) &&
                    qxp_metadata_a.value.sampler_types[sampler_register] ==
                        type_index + QXP_SHADER_SAMPLER_2D,
                    "texture sampler metadata");
            }
        }
    }
}

static DWORD qxp_build_dependent_texture_shader(void)
{
    DWORD cursor;
    DWORD index;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEXTURE, 0, 3, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_DCL_SAMPLER(D3DSTT_2D);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_SAMPLER, 0, 15, D3DSPDM_NONE);
    for (index = 0; index < 5; ++index) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEX, 3);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, index, 15, D3DSPDM_NONE);
        if (index)
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, index - 1);
        else
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_SAMPLER, 0);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 4);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_dependent_read_metadata(void)
{
    DWORD count;

    count = qxp_build_dependent_texture_shader();
    qxp_check(qxp_decode(qxp_work_tokens, count),
              "dependent texture decoder input");
    qxp_check((qxp_ir_a.required_ps20_caps &
               QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT) != 0,
              "decoder records dependent-read requirement");
    if (!qxp_emit_current("dependent texture deterministic emit")) return;
    qxp_check((qxp_metadata_a.value.required_ps20_caps &
               QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT) != 0,
              "lowerer preserves recomputed dependent-read requirement");
}

static void qxp_test_interface_and_definitions(void)
{
    qxp_check(qxp_decode(qxp_minimal_ps,
                         QXP_ARRAY_COUNT(qxp_minimal_ps)),
              "minimal constant decoder input");
    if (qxp_emit_current("minimal constant deterministic emit")) {
        qxp_check(qxp_metadata_a.value.required_constant_count == 1,
                  "minimal constant upload bound");
    }

    qxp_check(qxp_decode(qxp_interface_ps,
                         QXP_ARRAY_COUNT(qxp_interface_ps)),
              "interface decoder input");
    if (!qxp_emit_current("interface deterministic emit")) return;
    qxp_check(qxp_metadata_a.value.input_color_mask == 1 &&
              qxp_metadata_a.value.input_texture_mask == 1 &&
              qxp_metadata_a.value.input_centroid_mask == 1 &&
              qxp_metadata_a.value.input_color_components[0] == 1 &&
              qxp_metadata_a.value.input_texture_components[0] == 3 &&
              qxp_metadata_a.value.definition_mask == (1UL << 7) &&
              qxp_metadata_a.value.definition_values[7][0] ==
                  0x3e800000UL &&
              qxp_metadata_a.value.definition_values[7][3] ==
                  0x3f800000UL &&
              qxp_metadata_a.value.color_output_mask == 1,
              "interface and exact DEF metadata");
    qxp_check(strstr(qxp_source_a,
                     "centroid varying vec4 qxpPsInV0;") != NULL &&
              strstr(qxp_source_a,
                     "varying vec4 qxpPsInT0;") != NULL &&
              strstr(qxp_source_a,
                     "qxpT0.xy = qxpPsInT0.xy;") != NULL &&
              strstr(qxp_source_a,
                     "qxpV0.x = clamp(qxpPsInV0.x, 0.0, 1.0)") != NULL &&
              strstr(qxp_source_a,
                     "vec4 qxpV0 = vec4(0.0, 0.0, 0.0, 1.0)") != NULL,
              "input interpolation and color clamp lowering");

    qxp_check(qxp_decode(qxp_input_port_ps,
                         QXP_ARRAY_COUNT(qxp_input_port_ps)),
              "plain color DCL decoder input");
    if (!qxp_emit_current("plain color DCL deterministic emit")) return;
    qxp_check(qxp_metadata_a.value.input_centroid_mask == 3 &&
              strstr(qxp_source_a,
                     "centroid varying vec4 qxpPsInV0;") != NULL &&
              strstr(qxp_source_a,
                     "centroid varying vec4 qxpPsInV1;") != NULL,
              "ps_2_0 color registers apply effective centroid");

    qxp_check(qxp_decode(qxp_input_matrix_ps,
                         QXP_ARRAY_COUNT(qxp_input_matrix_ps)),
              "two-row input matrix decoder input");
    if (!qxp_emit_current("two-row input matrix deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a,
                     "centroid varying vec4 qxpPsInV1;") != NULL &&
              strstr(qxp_source_a, "qxpArg1 = qxpV1;") != NULL,
              "matrix input rows are all declared and emitted");
}

static void qxp_test_ps11_lowering(void)
{
    qxp_check(qxp_decode(qxp_ps11_first_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_first_ps)),
              "ps_1_1 first shader decoder input");
    if (!qxp_emit_current("ps_1_1 first shader deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a,
                     "qxpT0 = texture2D(qxpPsS0, qxpPsInT0.xy);") != NULL,
              "ps_1_1 tex samples implicit sampler at stage coordinate");
    qxp_check(strstr(qxp_source_a,
                     "uniform sampler2D qxpPsS0;") != NULL &&
              strstr(qxp_source_a,
                     "varying vec4 qxpPsInT0;") != NULL &&
              strstr(qxp_source_a, "qxpT0 = qxpPsInT0;") != NULL,
              "ps_1_1 mutable t# starts as the stage coordinate");
    qxp_check(strstr(qxp_source_a,
                     "qxpValue = qxpValue * 2.0;") != NULL,
              "ps_1_1 _x2 destination shift lowering");
    qxp_check(strstr(qxp_source_a,
                     "qxpR0.xyz = qxpValue.xyz;") != NULL &&
              strstr(qxp_source_a,
                     "qxpR0.w = qxpValue.w;") != NULL,
              "ps_1_1 co-issued pair lowers sequentially");
    qxp_check(strstr(qxp_source_a,
                     "gl_FragColor = qxpR0;") != NULL,
              "ps_1_1 r0 is the colour output");
    qxp_check(qxp_metadata_a.value.sampler_mask == 1 &&
              qxp_metadata_a.value.sampler_types[0] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.input_texture_mask == 1 &&
              qxp_metadata_a.value.input_texture_components[0] == 15 &&
              qxp_metadata_a.value.input_color_mask == 1 &&
              qxp_metadata_a.value.input_color_components[0] == 15 &&
              qxp_metadata_a.value.color_output_mask == 1 &&
              qxp_metadata_a.value.required_constant_count == 0 &&
              qxp_metadata_a.value.required_ps_temp_count == 1 &&
              qxp_metadata_a.value.required_ps20_caps == 0 &&
              qxp_metadata_a.value.required_ps_instruction_slots == 3 &&
              !qxp_metadata_a.value.writes_depth &&
              !qxp_metadata_a.value.uses_discard &&
              qxp_metadata_a.value.input_centroid_mask == 1,
              "ps_1_1 first shader metadata parity");

    qxp_check(qxp_decode(qxp_ps11_four_sampler_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_four_sampler_ps)),
              "ps_1_1 four-sampler decoder input");
    if (!qxp_emit_current("ps_1_1 four-sampler deterministic emit"))
        return;
    qxp_check(qxp_metadata_a.value.sampler_mask == 15 &&
              qxp_metadata_a.value.sampler_types[0] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.sampler_types[1] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.sampler_types[2] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.sampler_types[3] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.input_texture_mask == 15 &&
              qxp_metadata_a.value.required_ps_temp_count == 2 &&
              qxp_metadata_a.value.color_output_mask == 1,
              "ps_1_1 four-sampler metadata parity");
    qxp_check(strstr(qxp_source_a,
                     "qxpT3 = texture2D(qxpPsS3, qxpPsInT3.xy);") != NULL &&
              strstr(qxp_source_a,
                     "uniform sampler2D qxpPsS3;") != NULL,
              "ps_1_1 stage three sampler lowering");
    qxp_check(strstr(qxp_source_a,
                     "gl_FragColor = qxpR0;") != NULL,
              "ps_1_1 four-sampler r0 colour output");

    qxp_check(qxp_decode(qxp_ps11_coordinate_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_coordinate_ps)),
              "ps_1_1 coordinate decoder input");
    if (!qxp_emit_current("ps_1_1 coordinate deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a, "qxpT0 = qxpPsInT0;") != NULL &&
              strstr(qxp_source_a, "qxpT1 = qxpPsInT1;") != NULL,
              "ps_1_1 t# locals hold stage coordinates before any tex");
    qxp_check(strstr(qxp_source_a,
                     "qxpValue = qxpPsSaturate(qxpValue);") != NULL,
              "ps_1_1 saturate destination lowering");
    qxp_check(qxp_metadata_a.value.input_texture_mask == 3 &&
              qxp_metadata_a.value.sampler_mask == 0 &&
              qxp_metadata_a.value.required_constant_count == 2 &&
              qxp_metadata_a.value.input_color_mask == 1 &&
              qxp_metadata_a.value.input_centroid_mask == 1 &&
              qxp_metadata_a.value.color_output_mask == 1,
              "ps_1_1 coordinate metadata parity");

    qxp_check(qxp_decode(qxp_ps11_blend_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_blend_ps)),
              "ps_1_1 blend decoder input");
    if (!qxp_emit_current("ps_1_1 blend deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a,
                     "qxpArg0*qxpArg1 + (vec4(1.0)-qxpArg0)*qxpArg2")
                  != NULL,
              "ps_1_1 lrp lowering");
    qxp_check(qxp_metadata_a.value.sampler_mask == 3 &&
              qxp_metadata_a.value.required_constant_count == 1 &&
              qxp_metadata_a.value.input_color_mask == 1 &&
              qxp_metadata_a.value.color_output_mask == 1,
              "ps_1_1 blend metadata parity");

    qxp_check(qxp_decode(qxp_ps11_nfsu2_mad_x4_sat_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_nfsu2_mad_x4_sat_ps)),
              "ps_1_1 NFSU2 mad_sat _x4 decoder input");
    if (!qxp_emit_current("ps_1_1 NFSU2 mad_sat _x4 deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a,
                     "qxpValue = qxpValue * 4.0;") != NULL,
              "ps_1_1 _x4 destination shift lowering");
    qxp_check(strstr(qxp_source_a,
                     "qxpValue = qxpPsSaturate(qxpValue);") != NULL,
              "ps_1_1 _sat destination modifier lowering");
    qxp_check(strstr(qxp_source_a, "* 4.0;") != NULL &&
              strstr(qxp_source_a, "* 4.0;") <
                  strstr(qxp_source_a, "qxpPsSaturate(qxpValue);"),
              "ps_1_1 result shift applies before the saturate");
    qxp_check(qxp_metadata_a.value.sampler_mask == 1 &&
              qxp_metadata_a.value.required_constant_count == 1 &&
              qxp_metadata_a.value.input_texture_mask == 1 &&
              qxp_metadata_a.value.color_output_mask == 1 &&
              qxp_metadata_a.value.required_ps20_caps == 0,
              "ps_1_1 NFSU2 metadata parity");

    qxp_check(qxp_decode(qxp_ps11_nfsu2_texreg_ps,
                         QXP_ARRAY_COUNT(qxp_ps11_nfsu2_texreg_ps)),
              "ps_1_1 NFSU2 texreg decoder input");
    if (!qxp_emit_current("ps_1_1 NFSU2 texreg deterministic emit"))
        return;
    qxp_check(strstr(qxp_source_a,
                     "qxpT1 = texture2D(qxpPsS1, vec2(qxpT0.a,"
                     " qxpT0.r));") != NULL,
              "ps_1_1 texreg2ar samples stage 1 at (a, r)");
    qxp_check(strstr(qxp_source_a,
                     "qxpT2 = texture2D(qxpPsS2, vec2(qxpT0.g,"
                     " qxpT0.b));") != NULL,
              "ps_1_1 texreg2gb samples stage 2 at (g, b)");
    qxp_check(strstr(qxp_source_a, "vec4(1.0) - (") != NULL,
              "ps_1_1 _comp source modifier lowering");
    qxp_check(qxp_metadata_a.value.sampler_mask == 7 &&
              qxp_metadata_a.value.input_texture_mask == 7 &&
              qxp_metadata_a.value.sampler_types[0] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.sampler_types[1] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.sampler_types[2] ==
                  QXP_SHADER_SAMPLER_2D &&
              qxp_metadata_a.value.required_constant_count == 1 &&
              qxp_metadata_a.value.required_ps_temp_count == 2 &&
              qxp_metadata_a.value.color_output_mask == 1 &&
              qxp_metadata_a.value.required_ps20_caps == 0,
              "ps_1_1 NFSU2 texreg metadata parity");
}

static int qxp_zero_metadata(const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    QXP_PIXEL_SHADER_GLSL_METADATA zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(metadata, &zero, sizeof(zero)) == 0;
}

static void qxp_expect_ir_failure(QXP_SHADER_IR *ir, DWORD expected_code,
                                  const char *name)
{
    DWORD bytes;
    BOOL result;

    qxp_prepare_guards();
    qxp_source_a[0] = 'X';
    bytes = 123;
    result = QxpEmitPixelShader20Glsl(
        ir, qxp_source_a, (DWORD)sizeof(qxp_source_a), &bytes,
        &qxp_metadata_a.value, &qxp_error_a.value);
    qxp_check(!result && bytes == 0 && qxp_source_a[0] == 0 &&
              qxp_zero_metadata(&qxp_metadata_a.value) &&
              qxp_error_a.value.code == expected_code &&
              qxp_guards_ok(), name);
}

static void qxp_test_failure_contract(void)
{
    DWORD bytes;
    DWORD required;
    BOOL result;

    qxp_check(qxp_decode(qxp_minimal_ps, QXP_ARRAY_COUNT(qxp_minimal_ps)),
              "failure-contract decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.stage = QXP_SHADER_STAGE_VERTEX;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_PROFILE,
        "wrong profile rejects and zeros outputs");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.required_ps_temp_count = 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "mutated temp summary rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.required_ps_instruction_slots++;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "mutated slot summary rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].sources[0].register_file =
        QXP_SHADER_REG_SAMPLER;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
        "mutated source register rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].opcode = QXP_SHADER_OP_IF;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM,
        "unsupported typed opcode rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].destination_count =
        QXP_SHADER_MAX_DESTINATIONS + 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "oversized destination count rejects before traversal");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].source_count = QXP_SHADER_MAX_SOURCES + 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "oversized source count rejects before traversal");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].destinations[0].register_number = 31;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
        "oversized color output index rejects before shift");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].sources[0].register_number = 32;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
        "oversized constant index rejects before access");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].control = QXP_SHADER_CONTROL_TEX_PROJECT;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
        "nontexture instruction control rejects");
    qxp_ir_b = qxp_ir_a;
    ++qxp_ir_b.instructions[0].token_count;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "mutated instruction token span rejects");
    qxp_ir_b = qxp_ir_a;
    ++qxp_ir_b.instructions[0].slot_count;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "mutated opcode slot attribution rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].control_match = 0;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "mutated control sentinel rejects");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.present = TRUE;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
        "mutated declaration state rejects");

    qxp_prepare_guards();
    required = 0;
    result = QxpEmitPixelShader20Glsl(
        &qxp_ir_a, NULL, 0, &required, &qxp_metadata_a.value,
        &qxp_error_a.value);
    qxp_check(result && required > 1, "capacity query succeeds");
    bytes = 999;
    qxp_source_a[0] = 'X';
    result = QxpEmitPixelShader20Glsl(
        &qxp_ir_a, qxp_source_a, required, &bytes,
        &qxp_metadata_a.value, &qxp_error_a.value);
    qxp_check(!result && bytes == 0 && qxp_source_a[0] == 0 &&
              qxp_zero_metadata(&qxp_metadata_a.value) &&
              qxp_error_a.value.code ==
                  QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY &&
              qxp_error_a.value.required_capacity == required + 1,
              "exact short capacity reports required bytes");

    bytes = 123;
    result = QxpEmitPixelShader20Glsl(
        NULL, NULL, 0, &bytes, &qxp_metadata_a.value,
        &qxp_error_a.value);
    qxp_check(!result && bytes == 0 &&
              qxp_error_a.value.code ==
                  QXP_PIXEL_SHADER_GLSL_ERROR_NULL_ARGUMENT,
              "null IR rejects deterministically");

    qxp_check(qxp_decode(
                  qxp_work_tokens,
                  qxp_build_dependent_texture_shader()),
              "dependent capability mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.required_ps20_caps &=
        ~QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "removed dependent-read capability rejects");
    qxp_check(qxp_decode(qxp_minimal_ps,
                         QXP_ARRAY_COUNT(qxp_minimal_ps)),
              "injected capability mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.required_ps20_caps |=
        QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "injected dependent-read capability rejects");

    qxp_check(qxp_decode(
                  qxp_work_tokens,
                  qxp_build_texture_shader(D3DSTT_2D, 0, 3)),
              "sampler index mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[2].sources[1].register_number = 0xffffU;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
        "oversized sampler index rejects before access");

    qxp_check(qxp_decode(qxp_constant_port_ps,
                         QXP_ARRAY_COUNT(qxp_constant_port_ps)),
              "constant read-port mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].sources[1].register_number = 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "distinct constant read-port mutation rejects");
    qxp_check(qxp_decode(qxp_input_port_ps,
                         QXP_ARRAY_COUNT(qxp_input_port_ps)),
              "input read-port mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[2].sources[1].register_number = 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "distinct input read-port mutation rejects");
    qxp_check(qxp_decode(qxp_texture_port_ps,
                         QXP_ARRAY_COUNT(qxp_texture_port_ps)),
              "texture read-port mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[2].sources[1].register_file =
        QXP_SHADER_REG_TEXTURE;
    qxp_ir_b.instructions[2].sources[1].register_number = 1;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "second texture read-port mutation rejects");
    qxp_check(qxp_decode(qxp_matrix_port_ps,
                         QXP_ARRAY_COUNT(qxp_matrix_port_ps)),
              "matrix read-port mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[3].sources[0].register_file =
        QXP_SHADER_REG_TEXTURE;
    qxp_ir_b.instructions[3].sources[0].register_number = 0;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
        "matrix same-file read-port mutation rejects");
    qxp_check(qxp_decode(qxp_texkill_ps,
                         QXP_ARRAY_COUNT(qxp_texkill_ps)),
              "TEXKILL relative mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[1].destinations[0].relative.present = TRUE;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
        "relative TEXKILL destination rejects");

    qxp_check(qxp_decode(qxp_input_matrix_ps,
                         QXP_ARRAY_COUNT(qxp_input_matrix_ps)),
              "matrix input-row declaration mutation decoder input");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[1].destinations[0].register_file =
        QXP_SHADER_REG_TEXTURE;
    qxp_expect_ir_failure(
        &qxp_ir_b, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
        "missing second matrix input-row declaration rejects");
}

static void qxp_append_arithmetic(DWORD *cursor, DWORD opcode)
{
    DWORD source_count;
    DWORD destination_mask;
    DWORD index;

    source_count = 0;
    if (opcode == D3DSIO_MOV || opcode == D3DSIO_RCP ||
        opcode == D3DSIO_RSQ || opcode == D3DSIO_EXP ||
        opcode == D3DSIO_LOG || opcode == D3DSIO_FRC ||
        opcode == D3DSIO_ABS || opcode == D3DSIO_NRM)
        source_count = 1;
    else if (opcode == D3DSIO_ADD || opcode == D3DSIO_SUB ||
             opcode == D3DSIO_MUL || opcode == D3DSIO_DP3 ||
             opcode == D3DSIO_DP4 || opcode == D3DSIO_MIN ||
             opcode == D3DSIO_MAX || opcode == D3DSIO_M4x4 ||
             opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x4 ||
             opcode == D3DSIO_M3x3 || opcode == D3DSIO_M3x2 ||
             opcode == D3DSIO_POW || opcode == D3DSIO_CRS)
        source_count = 2;
    else
        source_count = 3;
    if (opcode == D3DSIO_NOP) {
        qxp_work_tokens[(*cursor)++] = QXP_INST(opcode, 0);
        return;
    }
    destination_mask = 15;
    if (opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x3 ||
        opcode == D3DSIO_CRS || opcode == D3DSIO_NRM)
        destination_mask = 7;
    else if (opcode == D3DSIO_M3x2 || opcode == D3DSIO_SINCOS)
        destination_mask = 3;
    qxp_work_tokens[(*cursor)++] = QXP_INST(opcode, source_count + 1);
    qxp_work_tokens[(*cursor)++] =
        QXP_DST(D3DSPR_TEMP, 0, destination_mask, D3DSPDM_NONE);
    if (opcode == D3DSIO_SINCOS) {
        qxp_work_tokens[(*cursor)++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_CONST, 0);
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_CONST, 1);
    } else if (opcode == D3DSIO_POW) {
        qxp_work_tokens[(*cursor)++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
        qxp_work_tokens[(*cursor)++] = QXP_SCALAR(D3DSPR_TEMP, 2, 1);
    } else if (opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 ||
               opcode == D3DSIO_M3x4 || opcode == D3DSIO_M3x3 ||
               opcode == D3DSIO_M3x2) {
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_TEMP, 1);
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_CONST, 4);
    } else if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
               opcode == D3DSIO_EXP || opcode == D3DSIO_LOG) {
        qxp_work_tokens[(*cursor)++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
    } else if (opcode == D3DSIO_DP2ADD) {
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_TEMP, 1);
        qxp_work_tokens[(*cursor)++] = QXP_SRC(D3DSPR_TEMP, 2);
        qxp_work_tokens[(*cursor)++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
    } else {
        for (index = 0; index < source_count; ++index)
            qxp_work_tokens[(*cursor)++] =
                QXP_SRC(D3DSPR_TEMP, index + 1);
    }
}

static DWORD qxp_build_coverage_shader(void)
{
    static const DWORD opcodes[] = {
        D3DSIO_NOP, D3DSIO_MOV, D3DSIO_ADD, D3DSIO_SUB, D3DSIO_MAD,
        D3DSIO_MUL, D3DSIO_RCP, D3DSIO_RSQ, D3DSIO_DP3, D3DSIO_DP4,
        D3DSIO_MIN, D3DSIO_MAX, D3DSIO_EXP, D3DSIO_LOG, D3DSIO_LRP,
        D3DSIO_FRC, D3DSIO_M4x4, D3DSIO_M4x3, D3DSIO_M3x4,
        D3DSIO_M3x3, D3DSIO_M3x2, D3DSIO_POW, D3DSIO_CRS,
        D3DSIO_ABS, D3DSIO_NRM, D3DSIO_SINCOS, D3DSIO_CMP,
        D3DSIO_DP2ADD
    };
    static const DWORD sampler_types[] = {
        D3DSTT_2D, D3DSTT_CUBE, D3DSTT_VOLUME
    };
    static const DWORD controls[] = {
        0, D3DSI_TEXLD_PROJECT, D3DSI_TEXLD_BIAS
    };
    DWORD cursor;
    DWORD index;
    DWORD type_index;
    DWORD control_index;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_INPUT, 0, 1, D3DSPDM_MSAMPCENTROID);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_MSAMPCENTROID);
    for (type_index = 0; type_index < 3; ++type_index) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
        qxp_work_tokens[cursor++] =
            QXP_DCL_SAMPLER(sampler_types[type_index]);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_SAMPLER, type_index, 15, D3DSPDM_NONE);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DEF, 5);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_CONST, 31, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = 0x3e800000UL;
    qxp_work_tokens[cursor++] = 0x3f000000UL;
    qxp_work_tokens[cursor++] = 0x3f400000UL;
    qxp_work_tokens[cursor++] = 0x3f800000UL;
    for (index = 1; index <= 3; ++index) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, index, 15, D3DSPDM_NONE);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 31);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 31, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 31);
    for (index = 0; index < QXP_ARRAY_COUNT(opcodes); ++index)
        qxp_append_arithmetic(&cursor, opcodes[index]);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 5, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    for (type_index = 0; type_index < 3; ++type_index) {
        for (control_index = 0; control_index < 3; ++control_index) {
            qxp_work_tokens[cursor++] =
                QXP_INST(D3DSIO_TEX, 3) | controls[control_index];
            qxp_work_tokens[cursor++] =
                QXP_DST(D3DSPR_TEMP, 10 + type_index * 3 +
                        control_index, 15, D3DSPDM_NONE);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
            qxp_work_tokens[cursor++] =
                QXP_SRC(D3DSPR_SAMPLER, type_index);
        }
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEXKILL, 1);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 31, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 31);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_ADD, 3);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 1, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_ADD, 3);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 2, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MUL, 3);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 4, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MUL, 3);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    for (index = 10; index <= 12; ++index) {
        qxp_work_tokens[cursor++] = QXP_INST(
            index == 12 ? D3DSIO_ADD : D3DSIO_MUL, 3);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, 0, 1UL << (index - 10),
                    D3DSPDM_NONE);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, index);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 1, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_DEPTHOUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_CONST, 31, 3);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static int qxp_write_shader_file(const char *path,
                                 const DWORD *tokens, DWORD count,
                                 const char *name)
{
    FILE *file;
    size_t bytes;

    if (!qxp_decode(tokens, count) || !qxp_emit_current(name)) return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    bytes = strlen(qxp_source_a);
    if (fwrite(qxp_source_a, 1, bytes, file) != bytes) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) return 0;
    return 1;
}

static int qxp_write_coverage_shader(const char *path)
{
    DWORD count;
    count = qxp_build_coverage_shader();
    if (!qxp_write_shader_file(
            path, qxp_work_tokens, count,
            "coverage shader deterministic emit"))
        return 0;
    printf("emitted ps_2_0 GLSL %lu bytes instructions=%lu temps=%lu\n",
           (unsigned long)strlen(qxp_source_a),
           (unsigned long)qxp_ir_a.instruction_count,
           (unsigned long)qxp_ir_a.required_ps_temp_count);
    return 1;
}

int main(int argc, char **argv)
{
    qxp_test_arithmetic_coverage();
    qxp_test_texture_coverage();
    qxp_test_dependent_read_metadata();
    qxp_test_interface_and_definitions();
    qxp_test_ps11_lowering();
    qxp_test_failure_contract();
    if (!qxp_failures && argc == 3 &&
        strcmp(argv[1], "--emit-glsl") == 0) {
        if (!qxp_write_coverage_shader(argv[2]))
            qxp_check(0, "write coverage GLSL");
    } else if (!qxp_failures && argc == 3 &&
               strcmp(argv[1], "--emit-noncentroid-glsl") == 0) {
        if (!qxp_write_shader_file(
                argv[2], qxp_noncentroid_link_ps,
                QXP_ARRAY_COUNT(qxp_noncentroid_link_ps),
                "non-centroid link shader deterministic emit"))
            qxp_check(0, "write non-centroid link GLSL");
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: qxp_pshader_glsl_test "
                "[--emit-glsl path | --emit-noncentroid-glsl path]\n");
        return 2;
    }
    if (qxp_failures) {
        fprintf(stderr, "qxp_pshader_glsl: %d failure(s)\n", qxp_failures);
        return 1;
    }
    printf("qxp_pshader_glsl: PASS\n");
    return 0;
}
