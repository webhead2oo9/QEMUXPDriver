#include <stdio.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxp_d3d9_shader_link.h"

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
#define QXP_SRC(type, number) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     D3DSP_NOSWIZZLE | D3DSPSM_NONE)
#define QXP_DCL_USAGE(usage, index) \
    (QXP_PARAMETER | ((DWORD)(usage) << D3DSP_DCL_USAGE_SHIFT) | \
     ((DWORD)(index) << D3DSP_DCL_USAGEINDEX_SHIFT))

typedef struct qxp_guarded_link {
    DWORD before[8];
    QXP_D3D9_VERTEX_SHADER_LINK value;
    DWORD after[8];
} QXP_GUARDED_LINK;

typedef struct qxp_guarded_status {
    DWORD before[8];
    QXP_D3D9_VERTEX_LINK_STATUS value;
    DWORD after[8];
} QXP_GUARDED_STATUS;

static QXP_SHADER_IR qxp_ir_a;
static QXP_SHADER_IR qxp_ir_b;
static QXP_VERTEX_DECLARATION9 qxp_decl_a;
static QXP_VERTEX_DECLARATION9 qxp_decl_b;
static QXP_GUARDED_LINK qxp_link_a;
static QXP_GUARDED_LINK qxp_link_b;
static QXP_GUARDED_STATUS qxp_status_a;
static QXP_GUARDED_STATUS qxp_status_b;
static QXP_D3D9_VERTEX_SHADER_LINK qxp_zero_link;
static QXP_D3D9_VERTEX_LINK_STATUS qxp_zero_status;
static int qxp_failures;

static const QXP_VERTEX_ELEMENT9 qxp_golden_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_TEXCOORD, 1 },
    { 0, 8, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_POSITION, 0 },
    { 1, 0, QXP_VERTEX_DECLTYPE9_D3DCOLOR,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_COLOR, 0 },
    { 2, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_NORMAL, 0 }
};

static const DWORD qxp_golden_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0),
        QXP_DST(D3DSPR_INPUT, 7, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 2, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 1),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 3),
        QXP_DST(D3DSPR_INPUT, 5, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 2),
    D3DSIO_END
};

static const DWORD qxp_no_input_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_texcoord_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_positiont_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITIONT, 0),
        QXP_DST(D3DSPR_INPUT, 3, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_minimal_pixel_shader[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

/* vs_1_1 mirror of qxp_golden_shader: identical dcl usage/index inputs and
 * the same oPos write, but real SM1 encoding -- every instruction token has
 * a zero length field, which the decoder derives from the opcode schema. */
static const DWORD qxp_golden_vs11_shader[] = {
    QXP_SHADER_VERSION_VS_1_1,
    D3DSIO_DCL,
        QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0),
        QXP_DST(D3DSPR_INPUT, 7, 15, D3DSPDM_NONE),
    D3DSIO_DCL,
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 2, 15, D3DSPDM_NONE),
    D3DSIO_DCL,
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 1),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_DCL,
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 3),
        QXP_DST(D3DSPR_INPUT, 5, 15, D3DSPDM_NONE),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 2),
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        ++qxp_failures;
    }
}

static void qxp_fill_words(DWORD *words, DWORD count, DWORD value)
{
    DWORD index;

    for (index = 0; index < count; ++index)
        words[index] = value;
}

static int qxp_words_equal(const DWORD *words, DWORD count, DWORD value)
{
    DWORD index;

    for (index = 0; index < count; ++index) {
        if (words[index] != value) return 0;
    }
    return 1;
}

static void qxp_prepare_guards(void)
{
    memset(&qxp_link_a, 0xa5, sizeof(qxp_link_a));
    memset(&qxp_link_b, 0x5a, sizeof(qxp_link_b));
    memset(&qxp_status_a, 0xa5, sizeof(qxp_status_a));
    memset(&qxp_status_b, 0x5a, sizeof(qxp_status_b));
    qxp_fill_words(qxp_link_a.before, 8, QXP_CANARY_A);
    qxp_fill_words(qxp_link_a.after, 8, QXP_CANARY_B);
    qxp_fill_words(qxp_status_a.before, 8, QXP_CANARY_A);
    qxp_fill_words(qxp_status_a.after, 8, QXP_CANARY_B);
    qxp_fill_words(qxp_link_b.before, 8, QXP_CANARY_B);
    qxp_fill_words(qxp_link_b.after, 8, QXP_CANARY_A);
    qxp_fill_words(qxp_status_b.before, 8, QXP_CANARY_B);
    qxp_fill_words(qxp_status_b.after, 8, QXP_CANARY_A);
}

static void qxp_check_guards(const char *name)
{
    qxp_check(qxp_words_equal(qxp_link_a.before, 8, QXP_CANARY_A), name);
    qxp_check(qxp_words_equal(qxp_link_a.after, 8, QXP_CANARY_B), name);
    qxp_check(qxp_words_equal(qxp_status_a.before, 8, QXP_CANARY_A), name);
    qxp_check(qxp_words_equal(qxp_status_a.after, 8, QXP_CANARY_B), name);
    qxp_check(qxp_words_equal(qxp_link_b.before, 8, QXP_CANARY_B), name);
    qxp_check(qxp_words_equal(qxp_link_b.after, 8, QXP_CANARY_A), name);
    qxp_check(qxp_words_equal(qxp_status_b.before, 8, QXP_CANARY_B), name);
    qxp_check(qxp_words_equal(qxp_status_b.after, 8, QXP_CANARY_A), name);
}

static int qxp_parse(const QXP_VERTEX_ELEMENT9 *elements, DWORD count,
                     QXP_VERTEX_DECLARATION9 *declaration)
{
    QXP_VERTEX_DECLARATION9_ERROR error;

    memset(&error, 0, sizeof(error));
    if (!QxpParseVertexDeclaration9(elements, count, declaration, &error)) {
        fprintf(stderr, "parse failed code=%lu element=%lu\n",
                (unsigned long)error.code,
                (unsigned long)error.element_index);
        ++qxp_failures;
        return 0;
    }
    return 1;
}

static int qxp_decode(const DWORD *tokens, DWORD count, QXP_SHADER_IR *ir)
{
    QXP_SHADER_DECODE_ERROR error;

    memset(&error, 0, sizeof(error));
    if (!QxpDecodeShader20(tokens, count, ir, &error)) {
        fprintf(stderr, "decode failed code=%lu token=%lu\n",
                (unsigned long)error.code,
                (unsigned long)error.token_offset);
        ++qxp_failures;
        return 0;
    }
    return 1;
}

static int qxp_link(const QXP_VERTEX_DECLARATION9 *declaration,
                    const QXP_SHADER_IR *shader,
                    QXP_D3D9_VERTEX_SHADER_LINK *link,
                    QXP_D3D9_VERTEX_LINK_STATUS *status)
{
    if (!QxpD3d9LinkVertexShader20(declaration, shader, link, status)) {
        fprintf(stderr,
                "link failed code=%lu decl=%lu instruction=%lu token=%lu\n",
                (unsigned long)status->code,
                (unsigned long)status->declaration_element,
                (unsigned long)status->instruction_index,
                (unsigned long)status->token_offset);
        ++qxp_failures;
        return 0;
    }
    return 1;
}

static void qxp_test_golden_link(void)
{
    QXP_D3D9_VERTEX_SHADER_LINK saved;

    if (!qxp_parse(qxp_golden_elements, QXP_ARRAY_COUNT(qxp_golden_elements),
                   &qxp_decl_a) ||
        !qxp_decode(qxp_golden_shader, QXP_ARRAY_COUNT(qxp_golden_shader),
                    &qxp_ir_a)) return;
    qxp_prepare_guards();
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;

    qxp_check(qxp_link_a.value.valid, "golden valid");
    qxp_check(qxp_link_a.value.mode ==
              QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE,
              "golden mode");
    qxp_check(qxp_link_a.value.input_count == 4,
              "golden input count");
    qxp_check(qxp_link_a.value.matched_input_count == 3,
              "golden matched count");
    qxp_check(qxp_link_a.value.undefined_input_count == 1,
              "golden undefined count");
    qxp_check(qxp_link_a.value.input_mask ==
              ((1UL << 0) | (1UL << 2) | (1UL << 5) | (1UL << 7)),
              "golden input mask");
    qxp_check(qxp_link_a.value.matched_input_mask ==
              ((1UL << 0) | (1UL << 2) | (1UL << 7)),
              "golden matched mask");
    qxp_check(qxp_link_a.value.undefined_input_mask == (1UL << 5),
              "golden undefined mask");
    qxp_check(qxp_link_a.value.matched_stream_mask == 3,
              "ignored declaration stream excluded");
    qxp_check(qxp_link_a.value.required_stream_spans[0] == 20 &&
              qxp_link_a.value.required_stream_spans[1] == 4 &&
              qxp_link_a.value.required_stream_spans[2] == 0,
              "matched stream spans");
    qxp_check(qxp_link_a.value.matched_element_mask ==
              (ULONGLONG)7,
              "matched element mask");

    qxp_check(qxp_link_a.value.inputs[7].matched &&
              qxp_link_a.value.inputs[7].usage ==
                  QXP_VERTEX_DECLUSAGE9_COLOR &&
              qxp_link_a.value.inputs[7].declaration_element == 2 &&
              qxp_link_a.value.inputs[7].source_stream == 1 &&
              qxp_link_a.value.inputs[7].source_type ==
                  QXP_VERTEX_DECLTYPE9_D3DCOLOR,
              "semantic maps color to v7");
    qxp_check(qxp_link_a.value.inputs[2].matched &&
              qxp_link_a.value.inputs[2].declaration_element == 1 &&
              qxp_link_a.value.inputs[2].source_offset == 8,
              "semantic maps position to v2");
    qxp_check(qxp_link_a.value.inputs[0].matched &&
              qxp_link_a.value.inputs[0].declaration_element == 0 &&
              qxp_link_a.value.inputs[0].source_byte_size == 8,
              "semantic maps texcoord1 to v0");
    qxp_check(qxp_link_a.value.inputs[5].declared &&
              !qxp_link_a.value.inputs[5].matched &&
              qxp_link_a.value.inputs[5].declaration_element ==
                  QXP_D3D9_VERTEX_LINK_INDEX_NONE,
              "missing vs2 semantic remains undefined");
    qxp_check(memcmp(&qxp_status_a.value, &qxp_zero_status,
                     sizeof(qxp_zero_status)) == 0,
              "success status zero");

    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_b.value,
                  &qxp_status_b.value)) return;
    qxp_check(memcmp(&qxp_link_a.value, &qxp_link_b.value,
                     sizeof(qxp_link_a.value)) == 0,
              "link twice deterministic");
    qxp_check(memcmp(&qxp_status_b.value, &qxp_zero_status,
                     sizeof(qxp_zero_status)) == 0,
              "second status zero");
    qxp_check_guards("golden canaries");

    memcpy(&saved, &qxp_link_a.value, sizeof(saved));
    memset(&qxp_decl_a, 0xcc, sizeof(qxp_decl_a));
    memset(&qxp_ir_a, 0xdd, sizeof(qxp_ir_a));
    qxp_check(memcmp(&saved, &qxp_link_a.value, sizeof(saved)) == 0,
              "link retains no input pointers");
}

static void qxp_test_no_input_shader(void)
{
    if (!qxp_parse(qxp_golden_elements, QXP_ARRAY_COUNT(qxp_golden_elements),
                   &qxp_decl_a) ||
        !qxp_decode(qxp_no_input_shader, QXP_ARRAY_COUNT(qxp_no_input_shader),
                    &qxp_ir_a)) return;
    memset(&qxp_link_a.value, 0xa5, sizeof(qxp_link_a.value));
    memset(&qxp_status_a.value, 0xa5, sizeof(qxp_status_a.value));
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;
    qxp_check(qxp_link_a.value.input_count == 0 &&
              qxp_link_a.value.input_mask == 0 &&
              qxp_link_a.value.matched_stream_mask == 0,
              "no-input shader ignores declaration");
}

static void qxp_test_positiont_rules(void)
{
    QXP_VERTEX_ELEMENT9 position;
    QXP_VERTEX_ELEMENT9 positiont;

    position.stream = 0;
    position.offset = 0;
    position.type = QXP_VERTEX_DECLTYPE9_FLOAT3;
    position.method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    position.usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    position.usage_index = 0;
    if (!qxp_parse(&position, 1, &qxp_decl_a) ||
        !qxp_decode(qxp_positiont_shader,
                    QXP_ARRAY_COUNT(qxp_positiont_shader), &qxp_ir_a))
        return;
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;
    qxp_check(qxp_link_a.value.mode ==
                  QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE &&
              qxp_link_a.value.undefined_input_mask == (1UL << 3),
              "position does not alias positiont");

    positiont = position;
    positiont.type = QXP_VERTEX_DECLTYPE9_FLOAT4;
    positiont.usage = QXP_VERTEX_DECLUSAGE9_POSITIONT;
    if (!qxp_parse(&positiont, 1, &qxp_decl_a) ||
        !qxp_decode(qxp_golden_shader, QXP_ARRAY_COUNT(qxp_golden_shader),
                    &qxp_ir_a)) return;
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;
    qxp_check(qxp_link_a.value.mode ==
                  QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS &&
              qxp_link_a.value.input_count == 4 &&
              qxp_link_a.value.matched_input_count == 0 &&
              qxp_link_a.value.undefined_input_count == 0 &&
              qxp_link_a.value.matched_stream_mask == 0,
              "positiont bypass skips programmable mapping");
}

static DWORD qxp_build_sixteen_input_shader(DWORD *tokens)
{
    DWORD cursor;
    DWORD index;

    cursor = 0;
    tokens[cursor++] = D3DVS_VERSION(2, 0);
    for (index = 0; index < 16; ++index) {
        tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
        tokens[cursor++] = QXP_DCL_USAGE(
            D3DDECLUSAGE_TEXCOORD, 15 - index);
        tokens[cursor++] = QXP_DST(
            D3DSPR_INPUT, index, 15, D3DSPDM_NONE);
    }
    tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    tokens[cursor++] = QXP_DST(
        D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE);
    tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, 0);
    tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_sixteen_inputs(void)
{
    QXP_VERTEX_ELEMENT9 elements[16];
    DWORD tokens[64];
    DWORD count;
    DWORD index;

    for (index = 0; index < 16; ++index) {
        elements[index].stream = 0;
        elements[index].offset = (WORD)(index * 4);
        elements[index].type = QXP_VERTEX_DECLTYPE9_FLOAT1;
        elements[index].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
        elements[index].usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
        elements[index].usage_index = (BYTE)index;
    }
    count = qxp_build_sixteen_input_shader(tokens);
    if (!qxp_parse(elements, 16, &qxp_decl_a) ||
        !qxp_decode(tokens, count, &qxp_ir_a)) return;
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;
    qxp_check(qxp_link_a.value.input_count == 16 &&
              qxp_link_a.value.matched_input_count == 16 &&
              qxp_link_a.value.input_mask == 0xffffUL &&
              qxp_link_a.value.matched_input_mask == 0xffffUL &&
              qxp_link_a.value.undefined_input_mask == 0,
              "all sixteen input registers");
    for (index = 0; index < 16; ++index) {
        qxp_check(qxp_link_a.value.inputs[index].declaration_element ==
                      15 - index,
                  "sixteen semantic register mapping");
    }
}

static DWORD qxp_build_one_input_shader(DWORD *tokens, DWORD usage,
                                        DWORD usage_index,
                                        DWORD destination_register)
{
    DWORD cursor;

    cursor = 0;
    tokens[cursor++] = D3DVS_VERSION(2, 0);
    tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    tokens[cursor++] = QXP_DCL_USAGE(usage, usage_index);
    tokens[cursor++] = QXP_DST(
        D3DSPR_INPUT, destination_register, 15, D3DSPDM_NONE);
    tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    tokens[cursor++] = QXP_DST(
        D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE);
    tokens[cursor++] = QXP_SRC(D3DSPR_INPUT, destination_register);
    tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_sixty_four_elements(void)
{
    QXP_VERTEX_ELEMENT9 elements[64];
    DWORD tokens[16];
    DWORD token_count;
    DWORD index;

    for (index = 0; index < 64; ++index) {
        elements[index].stream = (WORD)(index / 16);
        elements[index].offset = (WORD)((index % 16) * 4);
        elements[index].type = QXP_VERTEX_DECLTYPE9_FLOAT1;
        elements[index].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
        elements[index].usage = (BYTE)(index / 16);
        elements[index].usage_index = (BYTE)(index % 16);
    }
    token_count = qxp_build_one_input_shader(
        tokens, D3DDECLUSAGE_NORMAL, 15, 4);
    if (!qxp_parse(elements, 64, &qxp_decl_a) ||
        !qxp_decode(tokens, token_count, &qxp_ir_a)) return;
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;
    qxp_check(qxp_link_a.value.inputs[4].declaration_element == 63 &&
              qxp_link_a.value.matched_element_mask ==
                  (((ULONGLONG)1) << 63) &&
              qxp_link_a.value.matched_stream_mask == (1UL << 3) &&
              qxp_link_a.value.required_stream_spans[3] == 64,
              "sixty-four element boundary");
}

static void qxp_test_declaration_types(void)
{
    QXP_VERTEX_ELEMENT9 element;
    DWORD type;

    if (!qxp_decode(qxp_texcoord_shader,
                    QXP_ARRAY_COUNT(qxp_texcoord_shader), &qxp_ir_a))
        return;
    for (type = QXP_VERTEX_DECLTYPE9_FLOAT1;
         type <= QXP_VERTEX_DECLTYPE9_UNUSED; ++type) {
        element.stream = 0;
        element.offset = 0;
        element.type = (BYTE)type;
        element.method = type == QXP_VERTEX_DECLTYPE9_UNUSED ?
            QXP_VERTEX_DECLMETHOD9_UV :
            QXP_VERTEX_DECLMETHOD9_DEFAULT;
        element.usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
        element.usage_index = 0;
        if (!qxp_parse(&element, 1, &qxp_decl_a)) continue;
        if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                      &qxp_status_a.value)) continue;
        qxp_check(qxp_link_a.value.inputs[0].source_type == type &&
                  qxp_link_a.value.inputs[0].source_method ==
                      element.method &&
                  qxp_link_a.value.inputs[0].source_byte_size ==
                      qxp_decl_a.elements[0].byte_size,
                  "declaration type retained");
    }
}

static void qxp_test_generated_methods(void)
{
    QXP_VERTEX_ELEMENT9 partial[2];
    QXP_VERTEX_ELEMENT9 lookup[2];
    DWORD tokens[16];
    DWORD token_count;

    partial[0].stream = 0;
    partial[0].offset = 0;
    partial[0].type = QXP_VERTEX_DECLTYPE9_FLOAT3;
    partial[0].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    partial[0].usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    partial[0].usage_index = 0;
    partial[1] = partial[0];
    partial[1].method = QXP_VERTEX_DECLMETHOD9_PARTIALU;
    partial[1].usage = QXP_VERTEX_DECLUSAGE9_NORMAL;
    token_count = qxp_build_one_input_shader(
        tokens, D3DDECLUSAGE_NORMAL, 0, 0);
    if (qxp_parse(partial, 2, &qxp_decl_a) &&
        qxp_decode(tokens, token_count, &qxp_ir_a) &&
        qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                 &qxp_status_a.value)) {
        qxp_check(qxp_link_a.value.inputs[0].declaration_element == 1 &&
                  qxp_link_a.value.inputs[0].source_method ==
                      QXP_VERTEX_DECLMETHOD9_PARTIALU,
                  "partial method retained");
    }

    lookup[0].stream = 0;
    lookup[0].offset = 0;
    lookup[0].type = QXP_VERTEX_DECLTYPE9_FLOAT2;
    lookup[0].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    lookup[0].usage = QXP_VERTEX_DECLUSAGE9_TEXCOORD;
    lookup[0].usage_index = 0;
    lookup[1] = lookup[0];
    lookup[1].method = QXP_VERTEX_DECLMETHOD9_LOOKUP;
    lookup[1].usage = QXP_VERTEX_DECLUSAGE9_SAMPLE;
    token_count = qxp_build_one_input_shader(
        tokens, D3DDECLUSAGE_SAMPLE, 0, 1);
    if (qxp_parse(lookup, 2, &qxp_decl_a) &&
        qxp_decode(tokens, token_count, &qxp_ir_a) &&
        qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                 &qxp_status_a.value)) {
        qxp_check(qxp_link_a.value.inputs[1].declaration_element == 1 &&
                  qxp_link_a.value.inputs[1].source_method ==
                      QXP_VERTEX_DECLMETHOD9_LOOKUP,
                  "lookup method retained");
    }
}

static void qxp_expect_failure(
    const QXP_VERTEX_DECLARATION9 *declaration,
    const QXP_SHADER_IR *shader, DWORD expected_code,
    DWORD expected_element, DWORD expected_instruction,
    const char *name)
{
    BOOL result;

    memset(&qxp_link_a.value, 0xa5, sizeof(qxp_link_a.value));
    memset(&qxp_status_a.value, 0xa5, sizeof(qxp_status_a.value));
    result = QxpD3d9LinkVertexShader20(
        declaration, shader, &qxp_link_a.value, &qxp_status_a.value);
    qxp_check(!result, name);
    qxp_check(memcmp(&qxp_link_a.value, &qxp_zero_link,
                     sizeof(qxp_zero_link)) == 0,
              "failure zeroes link");
    if (qxp_status_a.value.code != expected_code) {
        fprintf(stderr, "FAIL: %s code expected=%lu actual=%lu\n",
                name, (unsigned long)expected_code,
                (unsigned long)qxp_status_a.value.code);
        ++qxp_failures;
    }
    if (qxp_status_a.value.declaration_element != expected_element) {
        fprintf(stderr, "FAIL: %s element expected=%lu actual=%lu\n",
                name, (unsigned long)expected_element,
                (unsigned long)qxp_status_a.value.declaration_element);
        ++qxp_failures;
    }
    if (qxp_status_a.value.instruction_index != expected_instruction) {
        fprintf(stderr, "FAIL: %s instruction expected=%lu actual=%lu\n",
                name, (unsigned long)expected_instruction,
                (unsigned long)qxp_status_a.value.instruction_index);
        ++qxp_failures;
    }
}

static void qxp_test_failures(void)
{
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_VERTEX_ELEMENT9 partial[2];

    if (!qxp_parse(qxp_golden_elements, QXP_ARRAY_COUNT(qxp_golden_elements),
                   &qxp_decl_a) ||
        !qxp_decode(qxp_golden_shader, QXP_ARRAY_COUNT(qxp_golden_shader),
                    &qxp_ir_a)) return;

    memset(&qxp_link_a.value, 0xa5, sizeof(qxp_link_a.value));
    memset(&qxp_status_a.value, 0xa5, sizeof(qxp_status_a.value));
    qxp_check(!QxpD3d9LinkVertexShader20(
                  NULL, &qxp_ir_a, &qxp_link_a.value, &qxp_status_a.value) &&
              qxp_status_a.value.code ==
                  QXP_D3D9_VERTEX_LINK_ERROR_NULL_ARGUMENT &&
              memcmp(&qxp_link_a.value, &qxp_zero_link,
                     sizeof(qxp_zero_link)) == 0,
              "null declaration");
    memset(&qxp_link_a.value, 0xa5, sizeof(qxp_link_a.value));
    qxp_check(!QxpD3d9LinkVertexShader20(
                  &qxp_decl_a, &qxp_ir_a, &qxp_link_a.value, NULL) &&
              memcmp(&qxp_link_a.value, &qxp_zero_link,
                     sizeof(qxp_zero_link)) == 0,
              "null status zeroes link");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.valid = FALSE;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_STATE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "invalid declaration state");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.elements[2].type = 99;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT, 2,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "corrupt declaration element");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.elements[1].usage = qxp_decl_b.elements[0].usage;
    qxp_decl_b.elements[1].usage_index =
        qxp_decl_b.elements[0].usage_index;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_SEMANTIC, 1,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "duplicate declaration semantic");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.semantic_masks[QXP_VERTEX_DECLUSAGE9_POSITION] ^= 1U;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_STATE,
        qxp_decl_b.element_count,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "corrupt semantic summary");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.elements[0].method = QXP_VERTEX_DECLMETHOD9_UV;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT, 0,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        "corrupt declaration method and type");

    qxp_decl_b = qxp_decl_a;
    qxp_decl_b.elements[0].usage = QXP_VERTEX_DECLUSAGE9_POSITIONT;
    qxp_decl_b.elements[0].usage_index = 0;
    qxp_expect_failure(
        &qxp_decl_b, &qxp_ir_a,
        QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT, 2,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        "corrupt positiont declaration");

    partial[0].stream = 0;
    partial[0].offset = 0;
    partial[0].type = QXP_VERTEX_DECLTYPE9_FLOAT3;
    partial[0].method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    partial[0].usage = QXP_VERTEX_DECLUSAGE9_POSITION;
    partial[0].usage_index = 0;
    partial[1] = partial[0];
    partial[1].method = QXP_VERTEX_DECLMETHOD9_PARTIALU;
    partial[1].usage = QXP_VERTEX_DECLUSAGE9_NORMAL;
    if (qxp_parse(partial, 2, &qxp_decl_b)) {
        qxp_decl_b.elements[1].offset = 4;
        qxp_expect_failure(
            &qxp_decl_b, &qxp_ir_a,
            QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT, 1,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            "corrupt generation reference");
    }

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.valid = FALSE;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_STATE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "invalid shader state");

    memset(&decode_error, 0, sizeof(decode_error));
    qxp_check(QxpDecodeShader20(
                  qxp_minimal_pixel_shader,
                  QXP_ARRAY_COUNT(qxp_minimal_pixel_shader),
                  &qxp_ir_b, &decode_error),
              "decode pixel for profile failure");
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_PROFILE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, "pixel profile rejected");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].destinations[0].register_number = 16;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, 0,
        "invalid shader input register");
    qxp_check(qxp_status_a.value.token_offset ==
              qxp_ir_b.instructions[0].token_offset,
              "shader error token offset");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.usage =
        QXP_VERTEX_DECLUSAGE9_NORMAL;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, 0,
        "nonduplicate raw semantic mismatch");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[0].declaration.raw_token |= 0x00000100UL;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, 0,
        "raw declaration reserved bits");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[1].declaration.usage =
        qxp_ir_b.instructions[0].declaration.usage;
    qxp_ir_b.instructions[1].declaration.usage_index =
        qxp_ir_b.instructions[0].declaration.usage_index;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, 1,
        "duplicate shader semantic");

    qxp_ir_b = qxp_ir_a;
    ++qxp_ir_b.declaration_count;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_STATE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        "shader declaration count mismatch");

    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.instructions[4].declaration.present = TRUE;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE, 4,
        "declaration metadata on executable instruction");
}

static void qxp_test_vs11_link(void)
{
    if (!qxp_parse(qxp_golden_elements, QXP_ARRAY_COUNT(qxp_golden_elements),
                   &qxp_decl_a) ||
        !qxp_decode(qxp_golden_vs11_shader,
                    QXP_ARRAY_COUNT(qxp_golden_vs11_shader), &qxp_ir_a))
        return;
    qxp_check(qxp_ir_a.version_token == QXP_SHADER_VERSION_VS_1_1,
              "vs_1_1 decoded version token");
    qxp_prepare_guards();
    if (!qxp_link(&qxp_decl_a, &qxp_ir_a, &qxp_link_a.value,
                  &qxp_status_a.value)) return;

    qxp_check(qxp_link_a.value.valid, "vs_1_1 link valid");
    qxp_check(qxp_link_a.value.mode ==
              QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE,
              "vs_1_1 link mode");
    qxp_check(qxp_link_a.value.input_count == 4 &&
              qxp_link_a.value.matched_input_count == 3 &&
              qxp_link_a.value.undefined_input_count == 1,
              "vs_1_1 link counts");
    qxp_check(qxp_link_a.value.input_mask ==
              ((1UL << 0) | (1UL << 2) | (1UL << 5) | (1UL << 7)) &&
              qxp_link_a.value.matched_input_mask ==
              ((1UL << 0) | (1UL << 2) | (1UL << 7)) &&
              qxp_link_a.value.undefined_input_mask == (1UL << 5),
              "vs_1_1 link masks");
    qxp_check(qxp_link_a.value.matched_stream_mask == 3 &&
              qxp_link_a.value.required_stream_spans[0] == 20 &&
              qxp_link_a.value.required_stream_spans[1] == 4 &&
              qxp_link_a.value.required_stream_spans[2] == 0 &&
              qxp_link_a.value.matched_element_mask == (ULONGLONG)7,
              "vs_1_1 stream spans and element mask");

    qxp_check(qxp_link_a.value.inputs[7].matched &&
              qxp_link_a.value.inputs[7].usage ==
                  QXP_VERTEX_DECLUSAGE9_COLOR &&
              qxp_link_a.value.inputs[7].usage_index == 0 &&
              qxp_link_a.value.inputs[7].destination_register == 7 &&
              qxp_link_a.value.inputs[7].declaration_element == 2 &&
              qxp_link_a.value.inputs[7].source_stream == 1 &&
              qxp_link_a.value.inputs[7].source_offset == 0 &&
              qxp_link_a.value.inputs[7].source_type ==
                  QXP_VERTEX_DECLTYPE9_D3DCOLOR,
              "vs_1_1 links color0 to v7 from stream 1");
    qxp_check(qxp_link_a.value.inputs[2].matched &&
              qxp_link_a.value.inputs[2].usage ==
                  QXP_VERTEX_DECLUSAGE9_POSITION &&
              qxp_link_a.value.inputs[2].usage_index == 0 &&
              qxp_link_a.value.inputs[2].destination_register == 2 &&
              qxp_link_a.value.inputs[2].declaration_element == 1 &&
              qxp_link_a.value.inputs[2].source_stream == 0 &&
              qxp_link_a.value.inputs[2].source_offset == 8,
              "vs_1_1 links position0 to v2 from stream 0 offset 8");
    qxp_check(qxp_link_a.value.inputs[0].matched &&
              qxp_link_a.value.inputs[0].usage ==
                  QXP_VERTEX_DECLUSAGE9_TEXCOORD &&
              qxp_link_a.value.inputs[0].usage_index == 1 &&
              qxp_link_a.value.inputs[0].destination_register == 0 &&
              qxp_link_a.value.inputs[0].declaration_element == 0 &&
              qxp_link_a.value.inputs[0].source_stream == 0 &&
              qxp_link_a.value.inputs[0].source_offset == 0 &&
              qxp_link_a.value.inputs[0].source_byte_size == 8,
              "vs_1_1 links texcoord1 to v0 from stream 0 offset 0");
    qxp_check(qxp_link_a.value.inputs[5].declared &&
              !qxp_link_a.value.inputs[5].matched &&
              qxp_link_a.value.inputs[5].usage ==
                  QXP_VERTEX_DECLUSAGE9_TEXCOORD &&
              qxp_link_a.value.inputs[5].usage_index == 3 &&
              qxp_link_a.value.inputs[5].declaration_element ==
                  QXP_D3D9_VERTEX_LINK_INDEX_NONE,
              "vs_1_1 missing texcoord3 remains undefined");
    qxp_check_guards("vs_1_1 canaries");

    /* A version token outside the accepted vertex set must still fail the
     * profile gate even on an otherwise valid decoded vertex IR. */
    qxp_ir_b = qxp_ir_a;
    qxp_ir_b.version_token = 0xffff0103UL;
    qxp_expect_failure(
        &qxp_decl_a, &qxp_ir_b,
        QXP_D3D9_VERTEX_LINK_ERROR_SHADER_PROFILE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        QXP_D3D9_VERTEX_LINK_INDEX_NONE,
        "ps_1_3 token still rejected");
}

static void qxp_test_bounded_mutations(void)
{
    DWORD index;

    if (!qxp_parse(qxp_golden_elements, QXP_ARRAY_COUNT(qxp_golden_elements),
                   &qxp_decl_a) ||
        !qxp_decode(qxp_golden_shader, QXP_ARRAY_COUNT(qxp_golden_shader),
                    &qxp_ir_a)) return;
    for (index = 0; index < qxp_decl_a.element_count; ++index) {
        qxp_decl_b = qxp_decl_a;
        qxp_decl_b.elements[index].usage =
            QXP_VERTEX_DECLARATION9_USAGE_COUNT;
        qxp_expect_failure(
            &qxp_decl_b, &qxp_ir_a,
            QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT,
            index, QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            "bounded declaration mutation");
    }
    for (index = 0; index < qxp_ir_a.declaration_count; ++index) {
        qxp_ir_b = qxp_ir_a;
        qxp_ir_b.instructions[index].destinations[0].write_mask = 7;
        qxp_expect_failure(
            &qxp_decl_a, &qxp_ir_b,
            QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE, index,
            "bounded shader declaration mutation");
    }
}

int main(void)
{
    qxp_test_golden_link();
    qxp_test_vs11_link();
    qxp_test_no_input_shader();
    qxp_test_positiont_rules();
    qxp_test_sixteen_inputs();
    qxp_test_sixty_four_elements();
    qxp_test_declaration_types();
    qxp_test_generated_methods();
    qxp_test_failures();
    qxp_test_bounded_mutations();

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 shader link tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 shader link tests: PASS\n");
    return 0;
}
