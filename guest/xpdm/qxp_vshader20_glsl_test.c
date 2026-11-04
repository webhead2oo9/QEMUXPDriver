#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxp_vshader20_glsl.h"

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
#define QXP_RELATIVE(type, number) \
    (QXP_SRC(type, number) | D3DSHADER_ADDRMODE_RELATIVE)
#define QXP_DCL_USAGE(usage, index) \
    (QXP_PARAMETER | (DWORD)(usage) | ((DWORD)(index) << 16))

typedef struct qxp_guarded_metadata {
    DWORD before[8];
    QXP_VERTEX_SHADER_GLSL_METADATA value;
    DWORD after[8];
} QXP_GUARDED_METADATA;

typedef struct qxp_guarded_error {
    DWORD before[8];
    QXP_VERTEX_SHADER_GLSL_ERROR value;
    DWORD after[8];
} QXP_GUARDED_ERROR;

static QXP_SHADER_IR qxp_ir_a;
static QXP_SHADER_IR qxp_ir_b;
static QXP_SHADER_DECODE_ERROR qxp_decode_error;
static QXP_GUARDED_METADATA qxp_metadata_a;
static QXP_GUARDED_METADATA qxp_metadata_b;
static QXP_GUARDED_ERROR qxp_error_a;
static QXP_GUARDED_ERROR qxp_error_b;
static char qxp_source_a[QXP_VERTEX_SHADER_GLSL_MAX_SOURCE + 1];
static char qxp_source_b[QXP_VERTEX_SHADER_GLSL_MAX_SOURCE + 1];
static DWORD qxp_tokens[4096];
static int qxp_failures;

static const DWORD qxp_minimal_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_illegal_scalar_mask_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_RCP, 2),
        QXP_DST(D3DSPR_TEMP, 0, 3, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 0, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_matrix_relative_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOVA, 2),
        QXP_DST(D3DSPR_ADDR, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 0, 0),
    QXP_INST(D3DSIO_M4x4, 4),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
        QXP_RELATIVE(D3DSPR_CONST, 255),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

/* vs_1_1 stream: SM1 instruction tokens carry a zero length field, the
 * address-register write is spelled MOV (not MOVA), and the relative
 * constant source sets only the relative bit with no address token. */
static const DWORD qxp_vs11_relative_vs[] = {
    D3DVS_VERSION(1, 1),
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0),
        QXP_DST(D3DSPR_INPUT, 3, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_ADDR, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 0, 0),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 3),
    D3DSIO_MOV, QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MOV, QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 3),
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_vshader20_glsl: failed: %s\n", name);
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
    return QxpDecodeShader20(tokens, count, &qxp_ir_a,
                             &qxp_decode_error) &&
           qxp_decode_error.code == QXP_SHADER_DECODE_OK;
}

static int qxp_emit_current(const char *name, DWORD centroid_mask)
{
    DWORD queried;
    DWORD bytes_a;
    DWORD bytes_b;
    BOOL result_a;
    BOOL result_b;

    qxp_prepare_guards();
    memset(qxp_source_a, 0x6b, sizeof(qxp_source_a));
    memset(qxp_source_b, 0x7c, sizeof(qxp_source_b));
    queried = 0;
    result_a = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, centroid_mask, NULL, 0, &queried,
        &qxp_metadata_a.value,
        &qxp_error_a.value);
    if (!result_a || !queried ||
        qxp_error_a.value.code != QXP_VERTEX_SHADER_GLSL_OK) {
        qxp_check(0, name);
        return 0;
    }
    bytes_a = 0;
    result_a = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, centroid_mask,
        qxp_source_a, (DWORD)sizeof(qxp_source_a),
        &bytes_a, &qxp_metadata_a.value, &qxp_error_a.value);
    bytes_b = 0;
    result_b = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, centroid_mask,
        qxp_source_b, (DWORD)sizeof(qxp_source_b),
        &bytes_b, &qxp_metadata_b.value, &qxp_error_b.value);
    if (!result_a || !result_b || bytes_a != queried ||
        bytes_a != bytes_b || qxp_source_a[bytes_a] != 0 ||
        qxp_source_b[bytes_b] != 0 ||
        memcmp(qxp_source_a, qxp_source_b, bytes_a + 1) != 0 ||
        memcmp(&qxp_metadata_a.value, &qxp_metadata_b.value,
               sizeof(qxp_metadata_a.value)) != 0 ||
        qxp_error_a.value.code != QXP_VERTEX_SHADER_GLSL_OK ||
        qxp_error_b.value.code != QXP_VERTEX_SHADER_GLSL_OK ||
        !qxp_guards_ok()) {
        qxp_check(0, name);
        return 0;
    }
    return 1;
}

static void qxp_push(DWORD *cursor, DWORD token)
{
    if (*cursor >= QXP_ARRAY_COUNT(qxp_tokens)) {
        qxp_check(0, "coverage token capacity");
        return;
    }
    qxp_tokens[(*cursor)++] = token;
}

static void qxp_emit_unary_tokens(DWORD *cursor, DWORD opcode,
                                  DWORD destination, DWORD source)
{
    qxp_push(cursor, QXP_INST(opcode, 2));
    qxp_push(cursor, destination);
    qxp_push(cursor, source);
}

static void qxp_emit_binary_tokens(DWORD *cursor, DWORD opcode,
                                   DWORD destination, DWORD source0,
                                   DWORD source1)
{
    qxp_push(cursor, QXP_INST(opcode, 3));
    qxp_push(cursor, destination);
    qxp_push(cursor, source0);
    qxp_push(cursor, source1);
}

static void qxp_emit_ternary_tokens(DWORD *cursor, DWORD opcode,
                                    DWORD destination, DWORD source0,
                                    DWORD source1, DWORD source2)
{
    qxp_push(cursor, QXP_INST(opcode, 4));
    qxp_push(cursor, destination);
    qxp_push(cursor, source0);
    qxp_push(cursor, source1);
    qxp_push(cursor, source2);
}

static void qxp_emit_matrix_tokens(DWORD *cursor, DWORD opcode, DWORD mask)
{
    qxp_emit_binary_tokens(cursor, opcode,
        QXP_DST(D3DSPR_TEMP, 0, mask, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_CONST, 4));
}

static void qxp_emit_matrix_custom_tokens(
    DWORD *cursor, DWORD opcode, DWORD mask, DWORD destination,
    DWORD vector, DWORD matrix_base)
{
    qxp_emit_binary_tokens(cursor, opcode,
        QXP_DST(D3DSPR_TEMP, destination, mask, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, vector),
        QXP_SRC(D3DSPR_CONST, matrix_base));
}

static DWORD qxp_build_coverage_shader(void)
{
    static const DWORD unary_opcodes[] = {
        D3DSIO_MOV, D3DSIO_RCP, D3DSIO_RSQ, D3DSIO_EXP, D3DSIO_LOG,
        D3DSIO_LIT, D3DSIO_FRC, D3DSIO_ABS, D3DSIO_NRM,
        D3DSIO_EXPP, D3DSIO_LOGP
    };
    static const DWORD binary_opcodes[] = {
        D3DSIO_ADD, D3DSIO_SUB, D3DSIO_MUL, D3DSIO_DP3, D3DSIO_DP4,
        D3DSIO_MIN, D3DSIO_MAX, D3DSIO_SLT, D3DSIO_SGE, D3DSIO_DST,
        D3DSIO_POW, D3DSIO_CRS
    };
    DWORD cursor;
    DWORD index;
    DWORD destination;
    DWORD source0;
    DWORD source1;

    memset(qxp_tokens, 0, sizeof(qxp_tokens));
    cursor = 0;
    qxp_push(&cursor, D3DVS_VERSION(2, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE));

    qxp_push(&cursor, QXP_INST(D3DSIO_DEF, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONST, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 0x3f800000UL);
    qxp_push(&cursor, 0x40000000UL);
    qxp_push(&cursor, 0x40400000UL);
    qxp_push(&cursor, 0x3f800000UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEF, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONST, 1, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 0x3f000000UL);
    qxp_push(&cursor, 0xbf000000UL);
    qxp_push(&cursor, 0x3e800000UL);
    qxp_push(&cursor, 0x3f800000UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEF, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONST, 1, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 0x3e4ccccdUL);
    qxp_push(&cursor, 0x3ecccccdUL);
    qxp_push(&cursor, 0x3f19999aUL);
    qxp_push(&cursor, 0x3f800000UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEF, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONST, 255, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 0x3dcccccdUL);
    qxp_push(&cursor, 0x3e4ccccdUL);
    qxp_push(&cursor, 0x3e99999aUL);
    qxp_push(&cursor, 0x3f800000UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEFI, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONSTINT, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 2UL);
    qxp_push(&cursor, 0UL);
    qxp_push(&cursor, 1UL);
    qxp_push(&cursor, 0UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEFI, 5));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONSTINT, 1, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 2UL);
    qxp_push(&cursor, 0UL);
    qxp_push(&cursor, 1UL);
    qxp_push(&cursor, 0UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEFB, 2));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONSTBOOL, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 1UL);
    qxp_push(&cursor, QXP_INST(D3DSIO_DEFB, 2));
    qxp_push(&cursor, QXP_DST(D3DSPR_CONSTBOOL, 1, 15, D3DSPDM_NONE));
    qxp_push(&cursor, 0UL);

    qxp_push(&cursor, QXP_INST(D3DSIO_NOP, 0));
    for (index = 0; index < 12; ++index) {
        qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
            QXP_DST(D3DSPR_TEMP, index, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0));
    }
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOVA,
        QXP_DST(D3DSPR_ADDR, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_MOV, 3));
    qxp_push(&cursor, QXP_DST(D3DSPR_TEMP, 11, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_RELATIVE(D3DSPR_CONST, 1));
    qxp_push(&cursor, QXP_SCALAR(D3DSPR_ADDR, 0, 0));

    for (index = 0; index < QXP_ARRAY_COUNT(unary_opcodes); ++index) {
        destination = QXP_DST(D3DSPR_TEMP, 0,
            (unary_opcodes[index] == D3DSIO_RCP ||
             unary_opcodes[index] == D3DSIO_RSQ ||
             unary_opcodes[index] == D3DSIO_EXP ||
             unary_opcodes[index] == D3DSIO_LOG ||
             unary_opcodes[index] == D3DSIO_EXPP ||
             unary_opcodes[index] == D3DSIO_LOGP) ? 1 : 15,
            D3DSPDM_NONE);
        source0 = (unary_opcodes[index] == D3DSIO_RCP ||
                   unary_opcodes[index] == D3DSIO_RSQ ||
                   unary_opcodes[index] == D3DSIO_EXP ||
                   unary_opcodes[index] == D3DSIO_LOG ||
                   unary_opcodes[index] == D3DSIO_EXPP ||
                   unary_opcodes[index] == D3DSIO_LOGP) ?
                  QXP_SCALAR(D3DSPR_TEMP, 1, 0) :
                  QXP_SRC(D3DSPR_TEMP, 1);
        qxp_emit_unary_tokens(&cursor, unary_opcodes[index],
                              destination, source0);
    }
    for (index = 0; index < QXP_ARRAY_COUNT(binary_opcodes); ++index) {
        destination = QXP_DST(D3DSPR_TEMP, 0,
            binary_opcodes[index] == D3DSIO_CRS ? 7 :
            (binary_opcodes[index] == D3DSIO_POW ? 1 : 15),
            D3DSPDM_NONE);
        source0 = (binary_opcodes[index] == D3DSIO_POW) ?
                  QXP_SCALAR(D3DSPR_TEMP, 1, 0) :
                  QXP_SRC(D3DSPR_TEMP, 1);
        source1 = (binary_opcodes[index] == D3DSIO_POW) ?
                  QXP_SCALAR(D3DSPR_TEMP, 2, 0) :
                  QXP_SRC(D3DSPR_TEMP, 2);
        qxp_emit_binary_tokens(&cursor, binary_opcodes[index],
                               destination, source0, source1);
    }
    qxp_emit_ternary_tokens(&cursor, D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 2),
        QXP_SRC(D3DSPR_TEMP, 3));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_LRP,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 2),
        QXP_SRC(D3DSPR_TEMP, 3));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_SGN,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 10),
        QXP_SRC(D3DSPR_TEMP, 11));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 10, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 11, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_SINCOS,
        QXP_DST(D3DSPR_TEMP, 0, 3, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 0),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1));

    qxp_emit_matrix_tokens(&cursor, D3DSIO_M4x4, 15);
    qxp_emit_matrix_tokens(&cursor, D3DSIO_M4x3, 7);
    qxp_emit_matrix_tokens(&cursor, D3DSIO_M3x4, 15);
    qxp_emit_matrix_tokens(&cursor, D3DSIO_M3x3, 7);
    qxp_emit_matrix_tokens(&cursor, D3DSIO_M3x2, 3);

    qxp_push(&cursor, QXP_INST(D3DSIO_IF, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTBOOL, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_ELSE, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_SUB,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDIF, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_REP, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTINT, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDREP, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_LOOP, 2));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LOOP, 0));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTINT, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_MOV, 3));
    qxp_push(&cursor, QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_RELATIVE(D3DSPR_CONST, 0));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LOOP, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDLOOP, 0));

    qxp_emit_unary_tokens(&cursor, D3DSIO_MOVA,
        QXP_DST(D3DSPR_ADDR, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 10, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_MOV, 3));
    qxp_push(&cursor, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_RELATIVE(D3DSPR_CONST, 255));
    qxp_push(&cursor, QXP_SCALAR(D3DSPR_ADDR, 0, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_REP, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTINT, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 1));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDREP, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_LOOP, 2));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LOOP, 0));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTINT, 1));
    qxp_push(&cursor, QXP_INST(D3DSIO_ADD, 4));
    qxp_push(&cursor, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_SRC(D3DSPR_TEMP, 0));
    qxp_push(&cursor, QXP_RELATIVE(D3DSPR_CONST, 2));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LOOP, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDLOOP, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_IF, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_CONSTBOOL, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 4));
    qxp_push(&cursor, QXP_INST(D3DSIO_ELSE, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 6));
    qxp_push(&cursor, QXP_INST(D3DSIO_ENDIF, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_CALL, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LABEL, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_CALLNZ, 2));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LABEL, 1));
    qxp_push(&cursor,
        QXP_SRC_SWIZZLE(D3DSPR_CONSTBOOL, 0, D3DSP_NOSWIZZLE,
                        D3DSPSM_NOT));

    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEXCRDOUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEXCRDOUT, 1, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_FOG, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 7, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_RET, 0));

    qxp_push(&cursor, QXP_INST(D3DSIO_LABEL, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LABEL, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_CONST, 5));
    qxp_push(&cursor, QXP_INST(D3DSIO_RET, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_LABEL, 1));
    qxp_push(&cursor, QXP_SRC(D3DSPR_LABEL, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 5, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1));
    qxp_push(&cursor, QXP_INST(D3DSIO_RET, 0));
    qxp_push(&cursor, D3DSIO_END);
    return cursor;
}

static DWORD qxp_build_special_shader(void)
{
    DWORD cursor;

    memset(qxp_tokens, 0, sizeof(qxp_tokens));
    cursor = 0;
    qxp_push(&cursor, D3DVS_VERSION(2, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE));
    qxp_emit_unary_tokens(&cursor, D3DSIO_RCP,
        QXP_DST(D3DSPR_TEMP, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_RSQ,
        QXP_DST(D3DSPR_TEMP, 0, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_LOG,
        QXP_DST(D3DSPR_TEMP, 0, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_SGE,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 3),
        QXP_SCALAR(D3DSPR_CONST, 0, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_EXPP,
        QXP_DST(D3DSPR_TEMP, 1, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 0, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_LOGP,
        QXP_DST(D3DSPR_TEMP, 1, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 0, 2));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 1, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 1, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_CONST, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1));
    qxp_push(&cursor, D3DSIO_END);
    return cursor;
}

static DWORD qxp_build_fog_shader(BOOL writes_fog)
{
    DWORD cursor;

    memset(qxp_tokens, 0, sizeof(qxp_tokens));
    cursor = 0;
    qxp_push(&cursor, D3DVS_VERSION(2, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE));
    if (writes_fog) {
        qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
        qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0));
        qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE));
    }
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0));
    if (writes_fog) {
        qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_FOG, 15, D3DSPDM_NONE),
            QXP_SCALAR(D3DSPR_INPUT, 1, 0));
    }
    qxp_push(&cursor, D3DSIO_END);
    return cursor;
}

static DWORD qxp_build_arithmetic_shader(void)
{
    DWORD cursor;

    memset(qxp_tokens, 0, sizeof(qxp_tokens));
    cursor = 0;
    qxp_push(&cursor, D3DVS_VERSION(2, 0));
    qxp_push(&cursor, QXP_INST(D3DSIO_DCL, 2));
    qxp_push(&cursor, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0));
    qxp_push(&cursor, QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE));

    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_MAD,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_SCALAR(D3DSPR_CONST, 0, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_FRC,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_EXP,
        QXP_DST(D3DSPR_TEMP, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 0, 1, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SCALAR(D3DSPR_CONST, 20, 0));

    qxp_emit_binary_tokens(&cursor, D3DSIO_DP3,
        QXP_DST(D3DSPR_TEMP, 1, 1, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 3), QXP_SRC(D3DSPR_CONST, 3));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MIN,
        QXP_DST(D3DSPR_TEMP, 1, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 0), QXP_SCALAR(D3DSPR_CONST, 5, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MAX,
        QXP_DST(D3DSPR_TEMP, 1, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 1), QXP_SCALAR(D3DSPR_CONST, 6, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_SLT,
        QXP_DST(D3DSPR_TEMP, 1, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 1), QXP_SCALAR(D3DSPR_CONST, 7, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_SGE,
        QXP_DST(D3DSPR_TEMP, 1, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 1), QXP_SCALAR(D3DSPR_CONST, 6, 0));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 1, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 1), QXP_SCALAR(D3DSPR_CONST, 20, 1));

    qxp_emit_binary_tokens(&cursor, D3DSIO_DP4,
        QXP_DST(D3DSPR_TEMP, 2, 1, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 8), QXP_SRC(D3DSPR_CONST, 8));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_LRP,
        QXP_DST(D3DSPR_TEMP, 2, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 0), QXP_SRC(D3DSPR_CONST, 11),
        QXP_SRC(D3DSPR_CONST, 11));
    /* The same constant register carries distinct y/z values for LRP. */
    qxp_tokens[cursor - 2] = QXP_SCALAR(D3DSPR_CONST, 11, 1);
    qxp_tokens[cursor - 1] = QXP_SCALAR(D3DSPR_CONST, 11, 2);

    qxp_emit_binary_tokens(&cursor, D3DSIO_DST,
        QXP_DST(D3DSPR_TEMP, 3, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 13), QXP_SRC(D3DSPR_CONST, 13));
    qxp_emit_unary_tokens(&cursor, D3DSIO_LIT,
        QXP_DST(D3DSPR_TEMP, 4, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 3));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 1, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 2));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 4, 2));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0));

    qxp_emit_unary_tokens(&cursor, D3DSIO_NRM,
        QXP_DST(D3DSPR_TEMP, 5, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 15));
    qxp_emit_binary_tokens(&cursor, D3DSIO_POW,
        QXP_DST(D3DSPR_TEMP, 6, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 5, 0),
        QXP_SCALAR(D3DSPR_CONST, 16, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 5, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 17));
    qxp_emit_binary_tokens(&cursor, D3DSIO_CRS,
        QXP_DST(D3DSPR_TEMP, 7, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 5), QXP_SRC(D3DSPR_CONST, 18));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_SGN,
        QXP_DST(D3DSPR_TEMP, 8, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 7), QXP_SRC(D3DSPR_TEMP, 10),
        QXP_SRC(D3DSPR_TEMP, 11));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 8, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 8, 1),
        QXP_SCALAR(D3DSPR_CONST, 20, 1));
    qxp_emit_ternary_tokens(&cursor, D3DSIO_SINCOS,
        QXP_DST(D3DSPR_TEMP, 9, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_CONST, 19, 0),
        QXP_SRC(D3DSPR_CONST, 21), QXP_SRC(D3DSPR_CONST, 22));

    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 3, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 23));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 14));
    qxp_emit_matrix_custom_tokens(
        &cursor, D3DSIO_M4x4, 15, 0, 3, 24);
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3), QXP_SCALAR(D3DSPR_TEMP, 0, 0));
    qxp_emit_matrix_custom_tokens(
        &cursor, D3DSIO_M4x3, 7, 0, 3, 24);
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3), QXP_SCALAR(D3DSPR_TEMP, 0, 1));
    qxp_emit_matrix_custom_tokens(
        &cursor, D3DSIO_M3x4, 15, 0, 3, 24);
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3), QXP_SCALAR(D3DSPR_TEMP, 0, 2));
    qxp_emit_matrix_custom_tokens(
        &cursor, D3DSIO_M3x3, 7, 0, 3, 24);
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3), QXP_SCALAR(D3DSPR_TEMP, 0, 0));
    qxp_emit_matrix_custom_tokens(
        &cursor, D3DSIO_M3x2, 3, 0, 3, 24);
    qxp_emit_binary_tokens(&cursor, D3DSIO_ADD,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3), QXP_SCALAR(D3DSPR_TEMP, 0, 1));
    qxp_emit_binary_tokens(&cursor, D3DSIO_MUL,
        QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3),
        QXP_SCALAR(D3DSPR_CONST, 20, 3));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 6, 2, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 8, 1));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 6, 4, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 9, 0));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_TEMP, 6, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 2, 3));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 6));
    qxp_emit_unary_tokens(&cursor, D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0));
    qxp_push(&cursor, D3DSIO_END);
    return cursor;
}

static int qxp_metadata_is_zero(const QXP_VERTEX_SHADER_GLSL_METADATA *value)
{
    static const QXP_VERTEX_SHADER_GLSL_METADATA zero_value = { 0 };

    return memcmp(value, &zero_value, sizeof(*value)) == 0;
}

static void qxp_expect_ir_reject(const QXP_SHADER_IR *ir, DWORD expected,
                                 const char *name)
{
    DWORD bytes;
    BOOL result;

    qxp_prepare_guards();
    memset(qxp_source_a, 0x31, sizeof(qxp_source_a));
    bytes = 0xfeedfaceUL;
    result = QxpEmitVertexShader20Glsl(
        ir, 0, qxp_source_a, (DWORD)sizeof(qxp_source_a), &bytes,
        &qxp_metadata_a.value, &qxp_error_a.value);
    qxp_check(!result && bytes == 0 && qxp_source_a[0] == 0 &&
              qxp_metadata_is_zero(&qxp_metadata_a.value) &&
              qxp_error_a.value.code == expected && qxp_guards_ok(), name);
}

static void qxp_expect_centroid_reject(DWORD centroid_mask,
                                       const char *name)
{
    DWORD bytes;
    BOOL result;

    qxp_prepare_guards();
    memset(qxp_source_a, 0x31, sizeof(qxp_source_a));
    bytes = 0xfeedfaceUL;
    result = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, centroid_mask, qxp_source_a,
        (DWORD)sizeof(qxp_source_a), &bytes,
        &qxp_metadata_a.value, &qxp_error_a.value);
    qxp_check(!result && bytes == 0 && qxp_source_a[0] == 0 &&
              qxp_metadata_is_zero(&qxp_metadata_a.value) &&
              qxp_error_a.value.code ==
                  QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION &&
              qxp_guards_ok(), name);
}

static void qxp_test_minimal_and_relative(void)
{
    qxp_check(!qxp_decode(
                  qxp_illegal_scalar_mask_vs,
                  QXP_ARRAY_COUNT(qxp_illegal_scalar_mask_vs)) &&
              qxp_decode_error.code == QXP_SHADER_DECODE_MODIFIER,
              "decoder rejects multi-lane scalar destination");
    if (qxp_decode(qxp_minimal_vs, QXP_ARRAY_COUNT(qxp_minimal_vs)) &&
        qxp_emit_current("minimal deterministic emit", 0)) {
        qxp_check(qxp_metadata_a.value.input_mask == 1 &&
                  qxp_metadata_a.value.required_float_constant_count == 0 &&
                  qxp_metadata_a.value.writes_position &&
                  qxp_metadata_a.value.position_component_mask == 15 &&
                  strstr(qxp_source_a, "attribute vec4 qxpVsInput0;") != NULL &&
                  strstr(qxp_source_a,
                         "gl_Position.x = qxpVsOutPosition.x + qxpVsOutPosition.w*qxpVsClipControl.z;") != NULL &&
                  strstr(qxp_source_a,
                         "gl_Position.y = qxpVsOutPosition.y*qxpVsClipControl.x + qxpVsOutPosition.w*qxpVsClipControl.w;") != NULL &&
                  strstr(qxp_source_a,
                         "gl_Position.z = 2.0*qxpVsOutPosition.z - qxpVsOutPosition.w;") != NULL,
                  "minimal metadata and D3D9 clip-space interface");
    } else {
        qxp_check(0, "minimal decode");
    }
    if (!qxp_decode(qxp_matrix_relative_vs,
                    QXP_ARRAY_COUNT(qxp_matrix_relative_vs)) ||
        !qxp_emit_current("relative matrix deterministic emit", 0)) {
        qxp_check(0, "relative matrix decode and emit");
    } else {
        qxp_check(qxp_metadata_a.value.uses_relative_constant &&
                  qxp_metadata_a.value.uses_address_register &&
                  qxp_metadata_a.value.required_float_constant_count == 256 &&
              strstr(qxp_source_a,
                     "qxpVsRelFloatConst(255 + 3 + qxpVsA0.x)") != NULL,
                  "high-base relative matrix rows lower safely");
    }
}

static void qxp_test_vs11_lowering(void)
{
    if (!qxp_decode(qxp_vs11_relative_vs,
                    QXP_ARRAY_COUNT(qxp_vs11_relative_vs)) ||
        !qxp_emit_current("vs_1_1 deterministic emit", 0)) {
        qxp_check(0, "vs_1_1 decode and emit");
        return;
    }
    qxp_check(qxp_metadata_a.value.uses_address_register &&
              strstr(qxp_source_a, "ivec4 qxpVsA0 = ivec4(0);") != NULL &&
              strstr(qxp_source_a,
                     "qxpVsA0.x = qxpVsRound(qxpValue).x;") != NULL,
              "vs_1_1 mov-to-address lowers like mova");
    qxp_check(qxp_metadata_a.value.uses_relative_constant &&
              strstr(qxp_source_a,
                     "qxpVsRelFloatConst(3 + qxpVsA0.x)") != NULL,
              "vs_1_1 tokenless relative constant indexes through a0.x");
    qxp_check(qxp_metadata_a.value.input_mask == 9 &&
              qxp_metadata_a.value.input_usage[0] == D3DDECLUSAGE_POSITION &&
              qxp_metadata_a.value.input_usage[3] == D3DDECLUSAGE_COLOR &&
              qxp_metadata_a.value.writes_position &&
              qxp_metadata_a.value.position_component_mask == 15 &&
              qxp_metadata_a.value.attribute_output_mask == 1,
              "vs_1_1 interface metadata matches vs_2_0 collection");
}

static void qxp_test_coverage_and_metadata(void)
{
    DWORD count;
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata;

    count = qxp_build_coverage_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("all-form deterministic emit", 5)) {
        qxp_check(0, "all-form decode and emit");
        return;
    }
    metadata = &qxp_metadata_a.value;
    qxp_check(metadata->required_temp_count == 12 &&
              metadata->input_mask == 3 &&
              metadata->input_usage[0] == D3DDECLUSAGE_POSITION &&
              metadata->input_usage[1] == D3DDECLUSAGE_COLOR &&
              metadata->attribute_output_mask == 3 &&
              metadata->texcoord_output_mask == 3 &&
              metadata->texcoord_output_components[1] == 7 &&
              metadata->writes_position && metadata->writes_fog &&
              metadata->writes_point_size,
              "all-form interface metadata");
    qxp_check((metadata->float_definition_mask[0] & 3) == 3 &&
              (metadata->float_definition_mask[7] & 0x80000000UL) != 0 &&
              metadata->float_definition_values[1][0] == 0x3e4ccccdUL &&
              metadata->integer_definition_mask == 3 &&
              metadata->integer_definition_values[0][2] == 1 &&
              metadata->boolean_definition_mask == 3 &&
              metadata->boolean_definition_values[0] == 1 &&
              metadata->boolean_definition_values[1] == 0,
              "exact last-wins definition metadata");
    qxp_check(metadata->uses_relative_constant &&
              metadata->uses_address_register &&
              metadata->uses_loop_register &&
              metadata->uses_integer_constants &&
              metadata->uses_boolean_constants &&
              metadata->uses_static_flow && metadata->uses_subroutines &&
              metadata->output_centroid_mask == 5,
              "all-form feature metadata");
    qxp_check(strstr(qxp_source_a,
                     "vec4 qxpVsExpp(float v) { return vec4(exp2(v)); }") != NULL &&
              strstr(qxp_source_a,
                     "vec4 qxpVsLogp(float v) { return vec4(qxpVsLog(v)); }") != NULL &&
              strstr(qxp_source_a, "qxpVsSign(qxpArg0)") != NULL &&
              strstr(qxp_source_a, "qxpVsSge(qxpArg0, qxpArg1)") != NULL &&
              strstr(qxp_source_a, "qxpVsSubroutineL0();") != NULL &&
              strstr(qxp_source_a, "qxpVsAL += qxpVsLoopStep") != NULL,
              "exact arithmetic and flow lowering text");
}

static void qxp_test_special_semantics(void)
{
    DWORD count;

    count = qxp_build_special_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("special semantic deterministic emit", 3)) {
        qxp_check(0, "special semantic decode and emit");
        return;
    }
    qxp_check(qxp_metadata_a.value.attribute_output_mask == 3 &&
              qxp_metadata_a.value.output_centroid_mask == 3 &&
              strstr(qxp_source_a, "qxpVsRcp(qxpArg0.x)") != NULL &&
              strstr(qxp_source_a, "qxpVsRsq(qxpArg0.x)") != NULL &&
              strstr(qxp_source_a, "qxpVsLog(qxpArg0.x)") != NULL &&
              strstr(qxp_source_a, "qxpVsSge(qxpArg0, qxpArg1)") != NULL &&
              strstr(qxp_source_a, "qxpVsExpp(qxpArg0.x)") != NULL &&
              strstr(qxp_source_a, "qxpVsLogp(qxpArg0.x)") != NULL,
              "special semantic lowering families present");
}

static void qxp_test_arithmetic_semantics(void)
{
    DWORD count;

    count = qxp_build_arithmetic_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("arithmetic semantic deterministic emit", 3)) {
        fprintf(stderr,
                "arithmetic seed decode error=%lu offset=%lu token=%08lx\n",
                (unsigned long)qxp_decode_error.code,
                (unsigned long)qxp_decode_error.token_offset,
                (unsigned long)qxp_tokens[qxp_decode_error.token_offset]);
        qxp_check(0, "arithmetic semantic decode and emit");
        return;
    }
    qxp_check(qxp_metadata_a.value.attribute_output_mask == 3 &&
              qxp_metadata_a.value.output_centroid_mask == 3 &&
              qxp_metadata_a.value.required_temp_count == 12,
              "arithmetic semantic metadata");
}

static void qxp_test_failure_contract(void)
{
    DWORD count;
    DWORD bytes;
    DWORD required;
    BOOL result;
    char exact[4];

    if (!qxp_decode(qxp_minimal_vs, QXP_ARRAY_COUNT(qxp_minimal_vs))) {
        qxp_check(0, "failure-contract seed decode");
        return;
    }
    qxp_prepare_guards();
    bytes = 7;
    result = QxpEmitVertexShader20Glsl(
        NULL, 0, NULL, 0, &bytes, &qxp_metadata_a.value,
        &qxp_error_a.value);
    qxp_check(!result && bytes == 0 &&
              qxp_error_a.value.code ==
                  QXP_VERTEX_SHADER_GLSL_ERROR_NULL_ARGUMENT &&
              qxp_metadata_is_zero(&qxp_metadata_a.value) && qxp_guards_ok(),
              "null IR failure zeroes outputs");

    required = 0;
    result = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, 0, NULL, 0, &required,
        &qxp_metadata_a.value,
        &qxp_error_a.value);
    qxp_check(result && required > 3, "count-only query succeeds");
    memset(exact, 0x44, sizeof(exact));
    bytes = 9;
    result = QxpEmitVertexShader20Glsl(
        &qxp_ir_a, 0, exact, (DWORD)sizeof(exact), &bytes,
        &qxp_metadata_a.value, &qxp_error_a.value);
    qxp_check(!result && bytes == 0 && exact[0] == 0 && exact[3] == 0x44 &&
              qxp_error_a.value.code ==
                  QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY &&
              qxp_error_a.value.required_capacity == required + 1 &&
              qxp_metadata_is_zero(&qxp_metadata_a.value),
              "capacity failure is bounded and exact");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.stage = QXP_SHADER_STAGE_PIXEL;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_PROFILE,
                         "wrong profile rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.block_count = 0;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                         "missing CFG rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.blocks[0].successors[1] = 0;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                         "corrupt CFG sentinel rejected");
    qxp_expect_centroid_reject(1UL,
                               "unwritten centroid output rejected");
    qxp_expect_centroid_reject(0x400UL,
                               "out-of-range centroid bit rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.raw_token ^= 1UL;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                         "raw declaration mismatch rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.sampler_type =
        QXP_SHADER_SAMPLER_2D;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                         "vertex DCL sampler metadata rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].sources[2].role =
        QXP_SHADER_OPERAND_SOURCE;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                         "unused operand corruption rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[1].sources[0].register_file =
        QXP_SHADER_REG_TEMP;
    qxp_ir_b.instructions[1].sources[0].register_number = 0;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
                         "uninitialized temp rejected");

    count = qxp_build_coverage_shader();
    if (!qxp_decode(qxp_tokens, count)) {
        qxp_check(0, "mutation coverage seed decode");
        return;
    }
    qxp_ir_b = qxp_ir_a;
    for (count = 0; count < qxp_ir_b.instruction_count; ++count) {
        if (qxp_ir_b.instructions[count].opcode == QXP_SHADER_OP_RCP) {
            qxp_ir_b.instructions[count].destinations[0].write_mask = 3;
            break;
        }
    }
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                         "lowerer rejects multi-lane scalar destination");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.slot_count++;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                         "slot summary mutation rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.usage++;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                         "typed declaration mutation rejected");
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[1].declaration.usage =
        qxp_ir_b.instructions[0].declaration.usage;
    qxp_ir_b.instructions[1].declaration.usage_index =
        qxp_ir_b.instructions[0].declaration.usage_index;
    qxp_ir_b.instructions[1].declaration.raw_token =
        qxp_ir_b.instructions[0].declaration.raw_token;
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                         "duplicate semantic mutation rejected");
    qxp_ir_b = qxp_ir_a;
    for (count = 0; count < qxp_ir_b.instruction_count; ++count) {
        if (qxp_ir_b.instructions[count].opcode == QXP_SHADER_OP_LABEL) {
            qxp_ir_b.instructions[count].label_target =
                QXP_SHADER_INDEX_NONE;
            break;
        }
    }
    qxp_expect_ir_reject(&qxp_ir_b,
                         QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                         "label target mutation rejected");
}

static int qxp_write_arithmetic_shader(const char *path)
{
    DWORD count;
    FILE *file;
    size_t bytes;

    count = qxp_build_arithmetic_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("arithmetic shader deterministic emit", 3))
        return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    bytes = strlen(qxp_source_a);
    if (fwrite(qxp_source_a, 1, bytes, file) != bytes) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) return 0;
    printf("emitted arithmetic vs_2_0 GLSL %lu bytes instructions=%lu\n",
           (unsigned long)bytes,
           (unsigned long)qxp_ir_a.instruction_count);
    return 1;
}

static int qxp_write_fog_shader(const char *path, BOOL writes_fog)
{
    DWORD count;
    FILE *file;
    size_t bytes;

    count = qxp_build_fog_shader(writes_fog);
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current(
            writes_fog ? "fog shader deterministic emit" :
                         "no-fog shader deterministic emit", 0))
        return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    bytes = strlen(qxp_source_a);
    if (fwrite(qxp_source_a, 1, bytes, file) != bytes) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) return 0;
    printf("emitted %s vs_2_0 GLSL %lu bytes\n",
           writes_fog ? "fog" : "no-fog", (unsigned long)bytes);
    return 1;
}

static int qxp_write_special_shader(const char *path)
{
    DWORD count;
    FILE *file;
    size_t bytes;

    count = qxp_build_special_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("special shader deterministic emit", 3))
        return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    bytes = strlen(qxp_source_a);
    if (fwrite(qxp_source_a, 1, bytes, file) != bytes) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) return 0;
    printf("emitted special vs_2_0 GLSL %lu bytes instructions=%lu\n",
           (unsigned long)bytes,
           (unsigned long)qxp_ir_a.instruction_count);
    return 1;
}

static int qxp_write_coverage_shader(const char *path,
                                     DWORD centroid_mask)
{
    DWORD count;
    FILE *file;
    size_t bytes;

    count = qxp_build_coverage_shader();
    if (!qxp_decode(qxp_tokens, count) ||
        !qxp_emit_current("coverage shader deterministic emit",
                          centroid_mask))
        return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    bytes = strlen(qxp_source_a);
    if (fwrite(qxp_source_a, 1, bytes, file) != bytes) {
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) return 0;
    printf("emitted vs_2_0 GLSL %lu bytes instructions=%lu temps=%lu\n",
           (unsigned long)bytes,
           (unsigned long)qxp_ir_a.instruction_count,
           (unsigned long)qxp_metadata_a.value.required_temp_count);
    return 1;
}

int main(int argc, char **argv)
{
    qxp_test_minimal_and_relative();
    qxp_test_vs11_lowering();
    qxp_test_coverage_and_metadata();
    qxp_test_special_semantics();
    qxp_test_arithmetic_semantics();
    qxp_test_failure_contract();
    if (!qxp_failures && argc == 3 &&
        (strcmp(argv[1], "--emit-glsl") == 0 ||
         strcmp(argv[1], "--emit-noncentroid-glsl") == 0)) {
        DWORD centroid_mask;

        centroid_mask = strcmp(argv[1], "--emit-glsl") == 0 ? 5 : 1;
        if (!qxp_write_coverage_shader(argv[2], centroid_mask))
            qxp_check(0, "write coverage GLSL");
    } else if (!qxp_failures && argc == 3 &&
               strcmp(argv[1], "--emit-special-glsl") == 0) {
        if (!qxp_write_special_shader(argv[2]))
            qxp_check(0, "write special semantic GLSL");
    } else if (!qxp_failures && argc == 3 &&
               strcmp(argv[1], "--emit-arithmetic-glsl") == 0) {
        if (!qxp_write_arithmetic_shader(argv[2]))
            qxp_check(0, "write arithmetic semantic GLSL");
    } else if (!qxp_failures && argc == 3 &&
               (strcmp(argv[1], "--emit-fog-glsl") == 0 ||
                strcmp(argv[1], "--emit-nofog-glsl") == 0)) {
        if (!qxp_write_fog_shader(
                argv[2], strcmp(argv[1], "--emit-fog-glsl") == 0))
            qxp_check(0, "write fog semantic GLSL");
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: qxp_vshader20_glsl_test "
                "[--emit-glsl path | --emit-noncentroid-glsl path | "
                "--emit-special-glsl path | "
                "--emit-arithmetic-glsl path | --emit-fog-glsl path | "
                "--emit-nofog-glsl path]\n");
        return 2;
    }
    if (qxp_failures) {
        fprintf(stderr, "qxp_vshader20_glsl: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp_vshader20_glsl: PASS\n");
    return 0;
}
