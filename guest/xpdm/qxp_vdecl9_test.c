#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include <windef.h>
#include <d3d9types.h>
#include <d3d9caps.h>

#include "qxp_vdecl9.h"

#define QXP_ARRAY_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_ANY_VALUE      0xffffffffUL
#define QXP_CANARY_A       0x13579bdfUL
#define QXP_CANARY_B       0x2468ace0UL
#define QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0 0x00000200UL
#define QXP_E(s, o, t, m, u, i) \
    { (WORD)(s), (WORD)(o), (BYTE)(t), (BYTE)(m), (BYTE)(u), (BYTE)(i) }

typedef char qxp_element9_size_is_8[
    sizeof(QXP_VERTEX_ELEMENT9) == 8 ? 1 : -1];
typedef char qxp_element9_stream_is_first[
    offsetof(QXP_VERTEX_ELEMENT9, stream) == 0 ? 1 : -1];
typedef char qxp_element9_offset_is_second[
    offsetof(QXP_VERTEX_ELEMENT9, offset) == 2 ? 1 : -1];
typedef char qxp_element9_type_is_fourth_byte[
    offsetof(QXP_VERTEX_ELEMENT9, type) == 4 ? 1 : -1];
typedef char qxp_element9_usage_index_is_last[
    offsetof(QXP_VERTEX_ELEMENT9, usage_index) == 7 ? 1 : -1];

typedef struct qxp_guarded_declaration9 {
    DWORD before[8];
    QXP_VERTEX_DECLARATION9 value;
    DWORD after[8];
} QXP_GUARDED_DECLARATION9;

typedef struct qxp_guarded_error9 {
    DWORD before[8];
    QXP_VERTEX_DECLARATION9_ERROR value;
    DWORD after[8];
} QXP_GUARDED_ERROR9;

typedef struct qxp_guarded_elements9 {
    DWORD before[8];
    QXP_VERTEX_ELEMENT9 values[4];
    DWORD after[8];
} QXP_GUARDED_ELEMENTS9;

static QXP_GUARDED_DECLARATION9 qxp_declaration_a;
static QXP_GUARDED_DECLARATION9 qxp_declaration_b;
static QXP_GUARDED_ERROR9 qxp_error_a;
static QXP_GUARDED_ERROR9 qxp_error_b;
static QXP_VERTEX_DECLARATION9 qxp_zero_declaration;
static QXP_VERTEX_DECLARATION9_ELEMENT qxp_zero_element;
static QXP_VERTEX_DECLARATION9_ERROR qxp_zero_error;
static QXP_VERTEX_ELEMENT9 qxp_work_elements[65];
static QXP_VERTEX_ELEMENT9 qxp_mutation_elements[16];
static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_vdecl9: failed: %s\n", name);
}

static void qxp_prepare_declaration(QXP_GUARDED_DECLARATION9 *guard)
{
    DWORD index;

    memset(guard, 0xcd, sizeof(*guard));
    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        guard->before[index] = QXP_CANARY_A + index;
        guard->after[index] = QXP_CANARY_B - index;
    }
}

static void qxp_prepare_error(QXP_GUARDED_ERROR9 *guard)
{
    DWORD index;

    memset(guard, 0xab, sizeof(*guard));
    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        guard->before[index] = QXP_CANARY_B + index;
        guard->after[index] = QXP_CANARY_A - index;
    }
}

static int qxp_declaration_canaries_ok(
    const QXP_GUARDED_DECLARATION9 *guard)
{
    DWORD index;

    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        if (guard->before[index] != QXP_CANARY_A + index ||
            guard->after[index] != QXP_CANARY_B - index) return 0;
    }
    return 1;
}

static int qxp_error_canaries_ok(const QXP_GUARDED_ERROR9 *guard)
{
    DWORD index;

    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        if (guard->before[index] != QXP_CANARY_B + index ||
            guard->after[index] != QXP_CANARY_A - index) return 0;
    }
    return 1;
}

static void qxp_set_element(QXP_VERTEX_ELEMENT9 *element,
                            DWORD stream, DWORD offset, DWORD type,
                            DWORD method, DWORD usage, DWORD usage_index)
{
    element->stream = (WORD)stream;
    element->offset = (WORD)offset;
    element->type = (BYTE)type;
    element->method = (BYTE)method;
    element->usage = (BYTE)usage;
    element->usage_index = (BYTE)usage_index;
}

static DWORD qxp_independent_type_size(DWORD type)
{
    switch (type) {
    case D3DDECLTYPE_FLOAT1: return 4;
    case D3DDECLTYPE_FLOAT2: return 8;
    case D3DDECLTYPE_FLOAT3: return 12;
    case D3DDECLTYPE_FLOAT4: return 16;
    case D3DDECLTYPE_D3DCOLOR: return 4;
    case D3DDECLTYPE_UBYTE4: return 4;
    case D3DDECLTYPE_SHORT2: return 4;
    case D3DDECLTYPE_SHORT4: return 8;
    case D3DDECLTYPE_UBYTE4N: return 4;
    case D3DDECLTYPE_SHORT2N: return 4;
    case D3DDECLTYPE_SHORT4N: return 8;
    case D3DDECLTYPE_USHORT2N: return 4;
    case D3DDECLTYPE_USHORT4N: return 8;
    case D3DDECLTYPE_UDEC3: return 4;
    case D3DDECLTYPE_DEC3N: return 4;
    case D3DDECLTYPE_FLOAT16_2: return 4;
    case D3DDECLTYPE_FLOAT16_4: return 8;
    case D3DDECLTYPE_UNUSED: return 0;
    }
    return QXP_ANY_VALUE;
}

static DWORD qxp_independent_type_cap(DWORD type)
{
    switch (type) {
    case D3DDECLTYPE_UBYTE4: return D3DDTCAPS_UBYTE4;
    case D3DDECLTYPE_UBYTE4N: return D3DDTCAPS_UBYTE4N;
    case D3DDECLTYPE_SHORT2N: return D3DDTCAPS_SHORT2N;
    case D3DDECLTYPE_SHORT4N: return D3DDTCAPS_SHORT4N;
    case D3DDECLTYPE_USHORT2N: return D3DDTCAPS_USHORT2N;
    case D3DDECLTYPE_USHORT4N: return D3DDTCAPS_USHORT4N;
    case D3DDECLTYPE_UDEC3: return D3DDTCAPS_UDEC3;
    case D3DDECLTYPE_DEC3N: return D3DDTCAPS_DEC3N;
    case D3DDECLTYPE_FLOAT16_2: return D3DDTCAPS_FLOAT16_2;
    case D3DDECLTYPE_FLOAT16_4: return D3DDTCAPS_FLOAT16_4;
    }
    return 0;
}

static int qxp_validate_success_independently(
    const QXP_VERTEX_ELEMENT9 *input, DWORD count,
    const QXP_VERTEX_DECLARATION9 *declaration)
{
    DWORD spans[QXP_VERTEX_DECLARATION9_STREAM_COUNT];
    WORD semantics[QXP_VERTEX_DECLARATION9_USAGE_COUNT];
    DWORD index;
    DWORD previous;
    DWORD expected_stream_mask;
    DWORD expected_decl_types;
    DWORD expected_dev_caps;
    DWORD expected_dev_caps2;
    DWORD expected_vertex_shader_version;
    DWORD expected_max_stride;
    DWORD seen_streams;
    DWORD current_stream;
    BOOL expected_positiont;
    BOOL expected_tessellation;
    BOOL expected_displacement;
    BOOL expected_programmable;

    if (!declaration->valid || declaration->element_count != count ||
        !count || count > QXP_VERTEX_DECLARATION9_MAX_ELEMENTS) return 0;
    memset(spans, 0, sizeof(spans));
    memset(semantics, 0, sizeof(semantics));
    expected_stream_mask = 0;
    expected_decl_types = 0;
    expected_dev_caps = 0;
    expected_dev_caps2 = 0;
    expected_vertex_shader_version = 0;
    expected_max_stride = 0;
    seen_streams = 0;
    current_stream = QXP_ANY_VALUE;
    expected_positiont = FALSE;
    expected_tessellation = FALSE;
    expected_displacement = FALSE;
    expected_programmable = FALSE;

    for (index = 0; index < count; ++index) {
        const QXP_VERTEX_ELEMENT9 *source;
        const QXP_VERTEX_DECLARATION9_ELEMENT *parsed;
        DWORD size;
        DWORD end;
        DWORD stream_bit;
        WORD semantic_bit;

        source = &input[index];
        parsed = &declaration->elements[index];
        size = qxp_independent_type_size(source->type);
        if (source->stream >= QXP_VERTEX_DECLARATION9_STREAM_COUNT ||
            source->usage >= QXP_VERTEX_DECLARATION9_USAGE_COUNT ||
            source->usage_index > QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX ||
            (source->offset & 3U) || size == QXP_ANY_VALUE ||
            size > 0xffffffffUL - (DWORD)source->offset) return 0;
        if (parsed->stream != source->stream ||
            parsed->offset != source->offset ||
            parsed->type != source->type ||
            parsed->method != source->method ||
            parsed->usage != source->usage ||
            parsed->usage_index != source->usage_index ||
            parsed->byte_size != size) return 0;

        stream_bit = 1UL << source->stream;
        if ((DWORD)source->stream != current_stream) {
            if (seen_streams & stream_bit) return 0;
            seen_streams |= stream_bit;
            current_stream = source->stream;
        }
        semantic_bit = (WORD)(1UL << source->usage_index);
        if (semantics[source->usage] & semantic_bit) return 0;
        semantics[source->usage] =
            (WORD)(semantics[source->usage] | semantic_bit);
        end = (DWORD)source->offset + size;
        if (end > spans[source->stream]) spans[source->stream] = end;
        if (end > expected_max_stride) expected_max_stride = end;
        expected_stream_mask |= stream_bit;
        expected_decl_types |= qxp_independent_type_cap(source->type);
        if (source->type > D3DDECLTYPE_SHORT4 &&
            source->type < D3DDECLTYPE_UNUSED) {
            expected_programmable = TRUE;
            expected_vertex_shader_version =
                QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0;
        }

        if (source->method == D3DDECLMETHOD_DEFAULT) {
            for (previous = 0; previous < index; ++previous) {
                const QXP_VERTEX_ELEMENT9 *candidate;
                DWORD candidate_size;
                DWORD candidate_end;

                candidate = &input[previous];
                if (candidate->stream != source->stream ||
                    candidate->method != D3DDECLMETHOD_DEFAULT) continue;
                candidate_size = qxp_independent_type_size(candidate->type);
                candidate_end = (DWORD)candidate->offset + candidate_size;
                if ((DWORD)source->offset < (DWORD)candidate->offset ||
                    ((DWORD)source->offset < candidate_end &&
                     (DWORD)candidate->offset < end)) {
                    expected_dev_caps2 |=
                        D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
                    break;
                }
            }
        }
        if (source->usage == D3DDECLUSAGE_POSITIONT &&
            source->usage_index == 0) expected_positiont = TRUE;
        if (source->method != D3DDECLMETHOD_DEFAULT ||
            source->usage == D3DDECLUSAGE_TESSFACTOR)
            expected_tessellation = TRUE;
        switch (source->method) {
        case D3DDECLMETHOD_PARTIALU:
        case D3DDECLMETHOD_PARTIALV:
        case D3DDECLMETHOD_CROSSUV:
        case D3DDECLMETHOD_UV:
            expected_dev_caps |= D3DDEVCAPS_RTPATCHES;
            break;
        case D3DDECLMETHOD_LOOKUP:
            expected_dev_caps |= D3DDEVCAPS_NPATCHES;
            expected_dev_caps2 |= D3DDEVCAPS2_DMAPNPATCH;
            expected_displacement = TRUE;
            expected_programmable = TRUE;
            break;
        case D3DDECLMETHOD_LOOKUPPRESAMPLED:
            expected_dev_caps |= D3DDEVCAPS_NPATCHES;
            expected_dev_caps2 |= D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH;
            expected_displacement = TRUE;
            expected_programmable = TRUE;
            break;
        }
    }

    if (declaration->stream_mask != expected_stream_mask ||
        declaration->required_decl_types != expected_decl_types ||
        declaration->required_dev_caps != expected_dev_caps ||
        declaration->required_dev_caps2 != expected_dev_caps2 ||
        declaration->required_vertex_shader_version !=
            expected_vertex_shader_version ||
        declaration->required_max_stream_stride != expected_max_stride ||
        declaration->has_positiont != expected_positiont ||
        declaration->requires_tessellation != expected_tessellation ||
        declaration->requires_displacement_mapping != expected_displacement ||
        declaration->requires_programmable_vertex_pipeline !=
            expected_programmable ||
        memcmp(declaration->stream_spans, spans, sizeof(spans)) != 0 ||
        memcmp(declaration->semantic_masks, semantics,
               sizeof(semantics)) != 0) return 0;
    for (index = count;
         index < QXP_VERTEX_DECLARATION9_MAX_ELEMENTS; ++index) {
        if (memcmp(&declaration->elements[index], &qxp_zero_element,
                   sizeof(qxp_zero_element)) != 0) return 0;
    }
    return 1;
}

static int qxp_audit_parse(const QXP_VERTEX_ELEMENT9 *elements, DWORD count,
                           BOOL expected_success, DWORD expected_code,
                           DWORD expected_index)
{
    BOOL result_a;
    BOOL result_b;
    int ok;

    qxp_prepare_declaration(&qxp_declaration_a);
    qxp_prepare_declaration(&qxp_declaration_b);
    qxp_prepare_error(&qxp_error_a);
    qxp_prepare_error(&qxp_error_b);
    result_a = QxpParseVertexDeclaration9(
        elements, count, &qxp_declaration_a.value, &qxp_error_a.value);
    result_b = QxpParseVertexDeclaration9(
        elements, count, &qxp_declaration_b.value, &qxp_error_b.value);
    ok = result_a == expected_success && result_b == expected_success &&
         qxp_declaration_canaries_ok(&qxp_declaration_a) &&
         qxp_declaration_canaries_ok(&qxp_declaration_b) &&
         qxp_error_canaries_ok(&qxp_error_a) &&
         qxp_error_canaries_ok(&qxp_error_b) &&
         memcmp(&qxp_declaration_a.value, &qxp_declaration_b.value,
                sizeof(qxp_declaration_a.value)) == 0 &&
         memcmp(&qxp_error_a.value, &qxp_error_b.value,
                sizeof(qxp_error_a.value)) == 0;
    if (!ok) return 0;
    if (expected_success) {
        return !memcmp(&qxp_error_a.value, &qxp_zero_error,
                       sizeof(qxp_error_a.value)) &&
               qxp_validate_success_independently(
                   elements, count, &qxp_declaration_a.value);
    }
    return !memcmp(&qxp_declaration_a.value, &qxp_zero_declaration,
                   sizeof(qxp_zero_declaration)) &&
           qxp_error_a.value.code != QXP_VERTEX_DECLARATION9_ERROR_OK &&
           (expected_code == QXP_ANY_VALUE ||
            qxp_error_a.value.code == expected_code) &&
           (expected_index == QXP_ANY_VALUE ||
            qxp_error_a.value.element_index == expected_index) &&
           qxp_error_a.value.byte_offset ==
               qxp_error_a.value.element_index * QXP_VERTEX_ELEMENT9_BYTES;
}

static int qxp_expect_success(const QXP_VERTEX_ELEMENT9 *elements,
                              DWORD count, const char *name)
{
    int ok;

    ok = qxp_audit_parse(elements, count, TRUE, 0, 0);
    qxp_check(ok, name);
    return ok;
}

static int qxp_expect_failure(const QXP_VERTEX_ELEMENT9 *elements,
                              DWORD count, DWORD code, DWORD index,
                              const char *name)
{
    int ok;

    ok = qxp_audit_parse(elements, count, FALSE, code, index);
    qxp_check(ok, name);
    return ok;
}

static int qxp_expect_fvf_success(
    const QXP_VERTEX_ELEMENT9 *elements, DWORD count,
    DWORD expected_fvf, DWORD expected_stride, const char *name)
{
    QXP_VERTEX_DECLARATION9 declaration;
    QXP_VERTEX_DECLARATION9_ERROR error;
    DWORD fvf;
    DWORD stride;
    BOOL parsed;
    BOOL translated;
    int ok;

    memset(&declaration, 0, sizeof(declaration));
    memset(&error, 0, sizeof(error));
    fvf = QXP_CANARY_A;
    stride = QXP_CANARY_B;
    parsed = QxpParseVertexDeclaration9(
        elements, count, &declaration, &error);
    translated = parsed && QxpTranslateVertexDeclaration9ToFvf(
        &declaration, &fvf, &stride);
    ok = translated && fvf == expected_fvf &&
         stride == expected_stride;
    qxp_check(ok, name);
    return ok;
}

static int qxp_expect_fvf_refusal(
    const QXP_VERTEX_ELEMENT9 *elements, DWORD count, const char *name)
{
    QXP_VERTEX_DECLARATION9 declaration;
    QXP_VERTEX_DECLARATION9_ERROR error;
    DWORD fvf;
    DWORD stride;
    BOOL parsed;
    BOOL translated;
    int ok;

    memset(&declaration, 0, sizeof(declaration));
    memset(&error, 0, sizeof(error));
    parsed = QxpParseVertexDeclaration9(
        elements, count, &declaration, &error);
    fvf = QXP_CANARY_A;
    stride = QXP_CANARY_B;
    translated = parsed && QxpTranslateVertexDeclaration9ToFvf(
        &declaration, &fvf, &stride);
    ok = parsed && !translated && !fvf && !stride;
    qxp_check(ok, name);
    return ok;
}

static void qxp_test_layout_and_private_values(void)
{
    qxp_check(sizeof(QXP_VERTEX_ELEMENT9) == 8 &&
              sizeof(QXP_VERTEX_ELEMENT9) == sizeof(D3DVERTEXELEMENT9) &&
              offsetof(QXP_VERTEX_ELEMENT9, stream) ==
                  offsetof(D3DVERTEXELEMENT9, Stream) &&
              offsetof(QXP_VERTEX_ELEMENT9, offset) ==
                  offsetof(D3DVERTEXELEMENT9, Offset) &&
              offsetof(QXP_VERTEX_ELEMENT9, type) ==
                  offsetof(D3DVERTEXELEMENT9, Type) &&
              offsetof(QXP_VERTEX_ELEMENT9, method) ==
                  offsetof(D3DVERTEXELEMENT9, Method) &&
              offsetof(QXP_VERTEX_ELEMENT9, usage) ==
                  offsetof(D3DVERTEXELEMENT9, Usage) &&
              offsetof(QXP_VERTEX_ELEMENT9, usage_index) ==
                  offsetof(D3DVERTEXELEMENT9, UsageIndex),
              "exact D3DVERTEXELEMENT9 byte layout");
    qxp_check(QXP_VERTEX_DECLARATION9_MAX_ELEMENTS == 64 &&
              QXP_VERTEX_DECLARATION9_STREAM_COUNT == 16 &&
              QXP_VERTEX_DECLARATION9_USAGE_COUNT == 14 &&
              QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX == 15,
              "fixed parser limits");
    qxp_check(QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4 ==
                  D3DDTCAPS_UBYTE4 &&
              QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4N ==
                  D3DDTCAPS_UBYTE4N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_SHORT2N ==
                  D3DDTCAPS_SHORT2N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_SHORT4N ==
                  D3DDTCAPS_SHORT4N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_USHORT2N ==
                  D3DDTCAPS_USHORT2N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_USHORT4N ==
                  D3DDTCAPS_USHORT4N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_UDEC3 ==
                  D3DDTCAPS_UDEC3 &&
              QXP_VERTEX_DECLARATION9_DTCAPS_DEC3N ==
                  D3DDTCAPS_DEC3N &&
              QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_2 ==
                  D3DDTCAPS_FLOAT16_2 &&
              QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_4 ==
                  D3DDTCAPS_FLOAT16_4 &&
              QXP_VERTEX_DECLARATION9_DEVCAPS_RTPATCHES ==
                  D3DDEVCAPS_RTPATCHES &&
              QXP_VERTEX_DECLARATION9_DEVCAPS_RTPATCHES ==
                  0x00400000UL &&
              QXP_VERTEX_DECLARATION9_DEVCAPS_NPATCHES ==
                  D3DDEVCAPS_NPATCHES &&
              QXP_VERTEX_DECLARATION9_DEVCAPS_NPATCHES ==
                  0x01000000UL &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_DMAPNPATCH ==
                  D3DDEVCAPS2_DMAPNPATCH &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_DMAPNPATCH ==
                  0x00000002UL &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_PRESAMPLEDDMAPNPATCH ==
                  D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_PRESAMPLEDDMAPNPATCH ==
                  0x00000020UL &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET ==
                  D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET &&
              QXP_VERTEX_DECLARATION9_DEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET ==
                  0x00000040UL &&
              QXP_VERTEX_DECLARATION9_VERTEX_SHADER_VERSION_2_0 ==
                  QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
              "private capability bits and VS requirement match D3D9");
}

static void qxp_test_ordinary_golden_and_no_end(void)
{
    static const QXP_VERTEX_ELEMENT9 single[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 multi[] = {
        QXP_E(2, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 1),
        QXP_E(2, 8, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0),
        QXP_E(7, 4, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 1)
    };
    static const QXP_VERTEX_ELEMENT9 ddi_one[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 with_end[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0xff, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT, 0, 0)
    };
    static const QXP_VERTEX_ELEMENT9 malformed_end[] = {
        QXP_E(0xff, 4, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_NORMAL, 2)
    };

    if (qxp_expect_success(single, QXP_ARRAY_COUNT(single),
                           "ordinary single-stream declaration")) {
        qxp_check(qxp_declaration_a.value.stream_mask == 1 &&
                  qxp_declaration_a.value.stream_spans[0] == 16 &&
                  qxp_declaration_a.value.required_max_stream_stride == 16 &&
                  qxp_declaration_a.value.semantic_masks[
                      D3DDECLUSAGE_POSITION] == 1 &&
                  qxp_declaration_a.value.semantic_masks[
                      D3DDECLUSAGE_COLOR] == 1 &&
                  qxp_declaration_a.value.required_decl_types == 0 &&
                  qxp_declaration_a.value.elements[0].byte_size == 12 &&
                  qxp_declaration_a.value.elements[1].byte_size == 4,
                  "ordinary single-stream golden metadata");
    }
    if (qxp_expect_success(multi, QXP_ARRAY_COUNT(multi),
                           "ordinary multi-stream declaration")) {
        qxp_check(qxp_declaration_a.value.stream_mask ==
                      ((1UL << 2) | (1UL << 7)) &&
                  qxp_declaration_a.value.stream_spans[2] == 12 &&
                  qxp_declaration_a.value.stream_spans[7] == 20 &&
                  qxp_declaration_a.value.required_max_stream_stride == 20 &&
                  qxp_declaration_a.value.required_decl_types ==
                      D3DDTCAPS_UBYTE4 &&
                  qxp_declaration_a.value.elements[2].stream == 7 &&
                  qxp_declaration_a.value.elements[2].offset == 4,
                  "ordinary multi-stream golden metadata");
    }
    qxp_expect_success(ddi_one, QXP_ARRAY_COUNT(ddi_one),
                       "counted DDI declaration needs no END");
    qxp_expect_failure(with_end, QXP_ARRAY_COUNT(with_end),
                       QXP_VERTEX_DECLARATION9_ERROR_SENTINEL, 1,
                       "application API END is invalid in counted DDI input");
    qxp_expect_failure(malformed_end, QXP_ARRAY_COUNT(malformed_end),
                       QXP_VERTEX_DECLARATION9_ERROR_SENTINEL, 0,
                       "malformed sentinel stream fails closed");
}

static void qxp_test_all_types_and_caps(void)
{
    static const DWORD expected_sizes[] = {
        4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8
    };
    static const DWORD expected_type_caps[] = {
        0, 0, 0, 0, 0, D3DDTCAPS_UBYTE4, 0, 0,
        D3DDTCAPS_UBYTE4N, D3DDTCAPS_SHORT2N,
        D3DDTCAPS_SHORT4N, D3DDTCAPS_USHORT2N,
        D3DDTCAPS_USHORT4N, D3DDTCAPS_UDEC3, D3DDTCAPS_DEC3N,
        D3DDTCAPS_FLOAT16_2, D3DDTCAPS_FLOAT16_4
    };
    static const DWORD expected_shader_versions[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0,
        QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0
    };
    DWORD index;
    DWORD offset;
    DWORD expected_caps;

    for (index = 0; index < QXP_ARRAY_COUNT(expected_sizes); ++index) {
        qxp_set_element(&qxp_work_elements[0], 0, 0, index,
                        D3DDECLMETHOD_DEFAULT,
                        D3DDECLUSAGE_TEXCOORD, 0);
        if (!qxp_expect_success(qxp_work_elements, 1,
                                "each declaration type in isolation") ||
            qxp_declaration_a.value.elements[0].byte_size !=
                expected_sizes[index] ||
            qxp_declaration_a.value.required_decl_types !=
                expected_type_caps[index] ||
            qxp_declaration_a.value.required_vertex_shader_version !=
                expected_shader_versions[index] ||
            qxp_declaration_a.value.
                requires_programmable_vertex_pipeline !=
                (expected_shader_versions[index] != 0)) {
            qxp_check(0, "per-type size, cap, and exact VS requirement");
            break;
        }
    }

    offset = 0;
    for (index = 0; index < QXP_ARRAY_COUNT(expected_sizes); ++index) {
        qxp_set_element(&qxp_work_elements[index], 0, offset, index,
                        D3DDECLMETHOD_DEFAULT,
                        index < 16 ? D3DDECLUSAGE_TEXCOORD :
                                     D3DDECLUSAGE_COLOR,
                        index < 16 ? index : 0);
        offset += expected_sizes[index];
    }
    if (!qxp_expect_success(qxp_work_elements,
                            QXP_ARRAY_COUNT(expected_sizes),
                            "all FLOAT1 through FLOAT16_4 types")) return;
    expected_caps = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N |
                    D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
                    D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N |
                    D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N |
                    D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    qxp_check(qxp_declaration_a.value.required_decl_types == expected_caps &&
              expected_caps == 0x000003ffUL &&
              qxp_declaration_a.value.required_vertex_shader_version ==
                  QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0 &&
              qxp_declaration_a.value.
                  requires_programmable_vertex_pipeline &&
              qxp_declaration_a.value.required_max_stream_stride == offset,
              "exact aggregate DeclTypes and VS 2.0 requirements");
    for (index = 0; index < QXP_ARRAY_COUNT(expected_sizes); ++index) {
        if (qxp_declaration_a.value.elements[index].byte_size !=
            expected_sizes[index]) {
            qxp_check(0, "exact byte size for every declaration type");
            break;
        }
    }
}

static void qxp_test_count_and_span_boundaries(void)
{
    DWORD index;

    for (index = 0; index < 65; ++index) {
        qxp_set_element(&qxp_work_elements[index], 0, index * 4,
                        D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
                        index / 16, index & 15);
    }
    qxp_expect_failure(qxp_work_elements, 0,
                       QXP_VERTEX_DECLARATION9_ERROR_ELEMENT_COUNT, 0,
                       "empty declaration rejected");
    for (index = 1; index <= 64; ++index) {
        if (!qxp_expect_success(qxp_work_elements, index,
                                "every declaration prefix from 1 through 64"))
            break;
    }
    qxp_check(index == 65 &&
              qxp_declaration_a.value.element_count == 64 &&
              qxp_declaration_a.value.stream_spans[0] == 256,
              "64-element declaration boundary accepted");
    qxp_expect_failure(qxp_work_elements, 65,
                       QXP_VERTEX_DECLARATION9_ERROR_ELEMENT_COUNT, 64,
                       "65-element declaration rejected before decoding");

    qxp_set_element(&qxp_work_elements[0], 15, 65532,
                    D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_TEXCOORD, 15);
    if (qxp_expect_success(qxp_work_elements, 1,
                           "largest aligned WORD offset is widened safely")) {
        qxp_check(qxp_declaration_a.value.stream_spans[15] == 65548UL &&
                  qxp_declaration_a.value.required_max_stream_stride ==
                      65548UL,
                  "span uses overflow-safe DWORD arithmetic");
    }
}

static void qxp_test_structural_failures(void)
{
    static const QXP_VERTEX_ELEMENT9 unaligned[] = {
        QXP_E(0, 2, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 stream_range[] = {
        QXP_E(16, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 type_range[] = {
        QXP_E(0, 0, 18, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 method_range[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, 7,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 usage_range[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, 14, 0)
    };
    static const QXP_VERTEX_ELEMENT9 usage_index_range[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 16)
    };
    static const QXP_VERTEX_ELEMENT9 duplicate[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 nonconsecutive[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(1, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 4, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };

    qxp_expect_failure(unaligned, QXP_ARRAY_COUNT(unaligned),
                       QXP_VERTEX_DECLARATION9_ERROR_OFFSET_ALIGNMENT, 0,
                       "DWORD offset alignment enforced");
    qxp_expect_failure(stream_range, QXP_ARRAY_COUNT(stream_range),
                       QXP_VERTEX_DECLARATION9_ERROR_STREAM_RANGE, 0,
                       "stream range enforced");
    qxp_expect_failure(type_range, QXP_ARRAY_COUNT(type_range),
                       QXP_VERTEX_DECLARATION9_ERROR_TYPE, 0,
                       "type range enforced");
    qxp_expect_failure(method_range, QXP_ARRAY_COUNT(method_range),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD, 0,
                       "method range enforced");
    qxp_expect_failure(usage_range, QXP_ARRAY_COUNT(usage_range),
                       QXP_VERTEX_DECLARATION9_ERROR_USAGE_RANGE, 0,
                       "usage range enforced");
    qxp_expect_failure(usage_index_range,
                       QXP_ARRAY_COUNT(usage_index_range),
                       QXP_VERTEX_DECLARATION9_ERROR_USAGE_INDEX_RANGE, 0,
                       "usage-index range enforced");
    qxp_expect_failure(duplicate, QXP_ARRAY_COUNT(duplicate),
                       QXP_VERTEX_DECLARATION9_ERROR_DUPLICATE_SEMANTIC, 1,
                       "duplicate semantic rejected across streams");
    qxp_expect_failure(nonconsecutive, QXP_ARRAY_COUNT(nonconsecutive),
                       QXP_VERTEX_DECLARATION9_ERROR_NONCONSECUTIVE_STREAM, 2,
                       "nonconsecutive stream block rejected");
}

static void qxp_test_share_offset_requirement(void)
{
    static const QXP_VERTEX_ELEMENT9 ordered[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 overlap[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 8, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 equal[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 reversed[] = {
        QXP_E(0, 16, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 1)
    };
    static const QXP_VERTEX_ELEMENT9 nested[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 8, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };

    if (qxp_expect_success(ordered, QXP_ARRAY_COUNT(ordered),
                           "ordered nonoverlapping defaults"))
        qxp_check(qxp_declaration_a.value.required_dev_caps2 == 0,
                  "ordinary offsets need no share capability");
    if (qxp_expect_success(overlap, QXP_ARRAY_COUNT(overlap),
                           "overlap accepted structurally"))
        qxp_check(qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET &&
                  qxp_declaration_a.value.stream_spans[0] == 20,
                  "overlap records share-offset requirement and max span");
    if (qxp_expect_success(equal, QXP_ARRAY_COUNT(equal),
                           "equal offsets accepted structurally"))
        qxp_check(qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET,
                  "equal offset records share-offset requirement");
    if (qxp_expect_success(reversed, QXP_ARRAY_COUNT(reversed),
                           "out-of-order offsets accepted structurally"))
        qxp_check(qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET &&
                  qxp_declaration_a.value.stream_spans[0] == 20,
                  "out-of-order offsets retain maximum end span");
    if (qxp_expect_success(nested, QXP_ARRAY_COUNT(nested),
                           "nested overlap accepted structurally"))
        qxp_check(qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET &&
                  qxp_declaration_a.value.stream_spans[0] == 16,
                  "nested overlap keeps widest earlier span");
}

static void qxp_test_positiont(void)
{
    static const QXP_VERTEX_ELEMENT9 valid[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_stream[] = {
        QXP_E(1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_alignment[] = {
        QXP_E(0, 2, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_duplicate[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_order[] = {
        QXP_E(0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_FOG, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_overlap[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_method[] = {
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 4, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_position[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };

    if (qxp_expect_success(valid, QXP_ARRAY_COUNT(valid),
                           "valid transformed declaration"))
        qxp_check(qxp_declaration_a.value.has_positiont &&
                  !qxp_declaration_a.value.requires_tessellation &&
                  qxp_declaration_a.value.required_dev_caps2 == 0,
                  "POSITIONT metadata is explicit");
    qxp_expect_failure(bad_stream, QXP_ARRAY_COUNT(bad_stream),
                       QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_STREAM, 0,
                       "POSITIONT requires stream zero");
    qxp_expect_failure(bad_alignment, QXP_ARRAY_COUNT(bad_alignment),
                       QXP_VERTEX_DECLARATION9_ERROR_OFFSET_ALIGNMENT, 0,
                       "POSITIONT retains DWORD alignment restriction");
    qxp_expect_failure(bad_duplicate, QXP_ARRAY_COUNT(bad_duplicate),
                       QXP_VERTEX_DECLARATION9_ERROR_DUPLICATE_SEMANTIC, 1,
                       "POSITIONT retains unique semantic restriction");
    qxp_expect_failure(bad_order, QXP_ARRAY_COUNT(bad_order),
                       QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_ORDER, 1,
                       "POSITIONT rejects out-of-order offsets");
    qxp_expect_failure(bad_overlap, QXP_ARRAY_COUNT(bad_overlap),
                       QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_ORDER, 1,
                       "POSITIONT rejects overlap instead of recording cap");
    qxp_expect_failure(bad_method, QXP_ARRAY_COUNT(bad_method),
                       QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_METHOD, 0,
                       "POSITIONT requires DEFAULT methods");
    qxp_expect_failure(bad_position, QXP_ARRAY_COUNT(bad_position),
                       QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_POSITION, 1,
                       "POSITIONT conflicts with POSITION zero");
}

static void qxp_test_lookup_methods(void)
{
    static const QXP_VERTEX_ELEMENT9 lookup[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 3),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_with_share[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0),
        QXP_E(0, 8, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_bad_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_missing[] = {
        QXP_E(0, 4, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_mismatch[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_forward[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 lookup_wrong_usage[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 sample_default[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 two_lookup[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 1)
    };
    static const QXP_VERTEX_ELEMENT9 two_lookup_kinds[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0),
        QXP_E(0, 0, D3DDECLTYPE_UNUSED,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 1)
    };
    static const QXP_VERTEX_ELEMENT9 mixed_lookup[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 presampled[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_UNUSED,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 presampled_with_share[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 8, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 0, D3DDECLTYPE_UNUSED,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 presampled_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 presampled_stream[] = {
        QXP_E(1, 0, D3DDECLTYPE_UNUSED,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 presampled_offset[] = {
        QXP_E(0, 4, D3DDECLTYPE_UNUSED,
              D3DDECLMETHOD_LOOKUPPRESAMPLED,
              D3DDECLUSAGE_SAMPLE, 0)
    };

    if (qxp_expect_success(lookup, QXP_ARRAY_COUNT(lookup),
                           "valid displacement LOOKUP"))
        qxp_check(qxp_declaration_a.value.requires_tessellation &&
                  qxp_declaration_a.value.requires_displacement_mapping &&
                  qxp_declaration_a.value.
                      requires_programmable_vertex_pipeline &&
                  qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_NPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_DMAPNPATCH &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      0 &&
                  qxp_declaration_a.value.stream_spans[0] == 24,
                  "LOOKUP records exact N-patch displacement requirements");
    if (qxp_expect_success(lookup_with_share,
                           QXP_ARRAY_COUNT(lookup_with_share),
                           "LOOKUP plus ordinary overlapping defaults"))
        qxp_check(qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_NPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 ==
                      (D3DDEVCAPS2_DMAPNPATCH |
                       D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET),
                  "LOOKUP and share-offset masks remain independent");
    qxp_expect_failure(lookup_bad_type, QXP_ARRAY_COUNT(lookup_bad_type),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, 1,
                       "LOOKUP accepts only FLOAT2 FLOAT3 FLOAT4");
    qxp_expect_failure(lookup_missing, QXP_ARRAY_COUNT(lookup_missing),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, 0,
                       "LOOKUP requires an existing same-locus element");
    qxp_expect_failure(lookup_mismatch, QXP_ARRAY_COUNT(lookup_mismatch),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, 1,
                       "LOOKUP reference type must match");
    qxp_expect_failure(lookup_forward, QXP_ARRAY_COUNT(lookup_forward),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, 0,
                       "LOOKUP cannot reference a later element");
    qxp_expect_failure(lookup_wrong_usage,
                       QXP_ARRAY_COUNT(lookup_wrong_usage),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, 1,
                       "LOOKUP requires SAMPLE semantic");
    qxp_expect_failure(sample_default, QXP_ARRAY_COUNT(sample_default),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, 0,
                       "SAMPLE requires a lookup-kind method");
    qxp_expect_failure(two_lookup, QXP_ARRAY_COUNT(two_lookup),
                       QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_COUNT, 2,
                       "only one LOOKUP element is allowed");
    qxp_expect_failure(two_lookup_kinds,
                       QXP_ARRAY_COUNT(two_lookup_kinds),
                       QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_COUNT, 2,
                       "LOOKUP and LOOKUPPRESAMPLED share one-element limit");
    qxp_expect_failure(mixed_lookup, QXP_ARRAY_COUNT(mixed_lookup),
                       QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_MIXED_METHOD, 1,
                       "LOOKUP declaration permits only other DEFAULT methods");
    if (qxp_expect_success(presampled, QXP_ARRAY_COUNT(presampled),
                           "valid LOOKUPPRESAMPLED"))
        qxp_check(qxp_declaration_a.value.elements[1].byte_size == 0 &&
                  qxp_declaration_a.value.stream_spans[0] == 12 &&
                  qxp_declaration_a.value.requires_displacement_mapping &&
                  qxp_declaration_a.value.
                      requires_programmable_vertex_pipeline &&
                  qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_NPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      0,
                  "LOOKUPPRESAMPLED records exact presampled requirements");
    if (qxp_expect_success(presampled_with_share,
                           QXP_ARRAY_COUNT(presampled_with_share),
                           "LOOKUPPRESAMPLED plus overlapping defaults"))
        qxp_check(qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_NPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 ==
                      (D3DDEVCAPS2_PRESAMPLEDDMAPNPATCH |
                       D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET),
                  "presampled and share-offset masks remain independent");
    qxp_expect_failure(presampled_type,
                       QXP_ARRAY_COUNT(presampled_type),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, 0,
                       "LOOKUPPRESAMPLED requires UNUSED");
    qxp_expect_failure(presampled_stream,
                       QXP_ARRAY_COUNT(presampled_stream),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET, 0,
                       "LOOKUPPRESAMPLED requires stream zero");
    qxp_expect_failure(presampled_offset,
                       QXP_ARRAY_COUNT(presampled_offset),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET, 0,
                       "LOOKUPPRESAMPLED requires offset zero");
}

static void qxp_test_tessellation_methods(void)
{
    static const DWORD methods[] = {
        D3DDECLMETHOD_PARTIALU,
        D3DDECLMETHOD_PARTIALV,
        D3DDECLMETHOD_CROSSUV
    };
    static const DWORD allowed_partial_types[] = {
        D3DDECLTYPE_D3DCOLOR,
        D3DDECLTYPE_FLOAT3,
        D3DDECLTYPE_FLOAT4,
        D3DDECLTYPE_SHORT4,
        D3DDECLTYPE_UBYTE4
    };
    static const DWORD disallowed_partial_types[] = {
        D3DDECLTYPE_FLOAT1,
        D3DDECLTYPE_FLOAT2,
        D3DDECLTYPE_SHORT2,
        D3DDECLTYPE_UBYTE4N,
        D3DDECLTYPE_SHORT2N,
        D3DDECLTYPE_SHORT4N,
        D3DDECLTYPE_USHORT2N,
        D3DDECLTYPE_USHORT4N,
        D3DDECLTYPE_UDEC3,
        D3DDECLTYPE_DEC3N,
        D3DDECLTYPE_FLOAT16_2,
        D3DDECLTYPE_FLOAT16_4
    };
    static const DWORD usages[] = {
        D3DDECLUSAGE_POSITION,
        D3DDECLUSAGE_NORMAL,
        D3DDECLUSAGE_BLENDWEIGHT,
        D3DDECLUSAGE_TEXCOORD
    };
    static const QXP_VERTEX_ELEMENT9 uv_texcoord[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 uv_weight[] = {
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_BLENDWEIGHT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 uv_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 uv_stream[] = {
        QXP_E(1, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 uv_offset[] = {
        QXP_E(0, 4, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 uv_usage[] = {
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 patch_with_share[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 8, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 partial_usage[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALV,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 cross_missing[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_CROSSUV,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 cross_mismatch[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_CROSSUV,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 combined[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_POSITION, 1),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALV,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_CROSSUV,
              D3DDECLUSAGE_BLENDWEIGHT, 0),
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_UV,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 modern_tessellation[] = {
        QXP_E(0, 0, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0),
        QXP_E(0, 4, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 4, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_NORMAL, 0)
    };
    DWORD method_index;
    DWORD type_index;
    DWORD usage_index;
    DWORD prefix;

    if (qxp_expect_success(uv_texcoord, QXP_ARRAY_COUNT(uv_texcoord),
                           "valid UV TEXCOORD method"))
        qxp_check(qxp_declaration_a.value.requires_tessellation &&
                  !qxp_declaration_a.value.
                      requires_displacement_mapping &&
                  !qxp_declaration_a.value.
                      requires_programmable_vertex_pipeline &&
                  qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_RTPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 == 0 &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      0 &&
                  qxp_declaration_a.value.elements[1].byte_size == 0,
                  "UV records exact RT-patch and no VS 2.0 requirement");
    qxp_expect_success(uv_weight, QXP_ARRAY_COUNT(uv_weight),
                       "valid UV BLENDWEIGHT method");
    qxp_expect_failure(uv_type, QXP_ARRAY_COUNT(uv_type),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, 0,
                       "UV requires UNUSED");
    qxp_expect_failure(uv_stream, QXP_ARRAY_COUNT(uv_stream),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET, 0,
                       "UV requires stream zero");
    qxp_expect_failure(uv_offset, QXP_ARRAY_COUNT(uv_offset),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET, 0,
                       "UV requires offset zero");
    qxp_expect_failure(uv_usage, QXP_ARRAY_COUNT(uv_usage),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, 0,
                       "UV semantic is TEXCOORD or BLENDWEIGHT");

    for (method_index = 0; method_index < QXP_ARRAY_COUNT(methods);
         ++method_index) {
        for (type_index = 0;
             type_index < QXP_ARRAY_COUNT(allowed_partial_types);
             ++type_index) {
            for (usage_index = 0; usage_index < QXP_ARRAY_COUNT(usages);
                 ++usage_index) {
                qxp_set_element(&qxp_work_elements[0], 0, 0,
                                allowed_partial_types[type_index],
                                D3DDECLMETHOD_DEFAULT,
                                D3DDECLUSAGE_POSITION, 0);
                qxp_set_element(&qxp_work_elements[1], 0, 0,
                                allowed_partial_types[type_index],
                                methods[method_index],
                                usages[usage_index],
                                usages[usage_index] ==
                                    D3DDECLUSAGE_POSITION ? 1 : 0);
                if (qxp_expect_success(
                        qxp_work_elements, 2,
                        "each partial method, type, and semantic"))
                    qxp_check(
                        qxp_declaration_a.value.required_dev_caps ==
                            D3DDEVCAPS_RTPATCHES &&
                        qxp_declaration_a.value.required_dev_caps2 == 0 &&
                        qxp_declaration_a.value.
                            required_vertex_shader_version == 0 &&
                        !qxp_declaration_a.value.
                            requires_programmable_vertex_pipeline,
                        "each partial method has exact RT-patch mask");
            }
        }
        for (type_index = 0;
             type_index < QXP_ARRAY_COUNT(disallowed_partial_types);
             ++type_index) {
            qxp_set_element(&qxp_work_elements[0], 0, 0,
                            disallowed_partial_types[type_index],
                            D3DDECLMETHOD_DEFAULT,
                            D3DDECLUSAGE_POSITION, 0);
            qxp_set_element(&qxp_work_elements[1], 0, 0,
                            disallowed_partial_types[type_index],
                            methods[method_index],
                            D3DDECLUSAGE_NORMAL, 0);
            qxp_expect_failure(
                qxp_work_elements, 2,
                QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, 1,
                "partial methods reject every non-Microsoft input type");
        }
    }
    qxp_expect_failure(partial_usage, QXP_ARRAY_COUNT(partial_usage),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, 1,
                       "PARTIALV restricts semantic");
    qxp_expect_failure(cross_missing, QXP_ARRAY_COUNT(cross_missing),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, 0,
                       "CROSSUV requires existing compatible element");
    qxp_expect_failure(cross_mismatch, QXP_ARRAY_COUNT(cross_mismatch),
                       QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, 1,
                       "CROSSUV reference must match type at same locus");
    if (qxp_expect_success(patch_with_share,
                           QXP_ARRAY_COUNT(patch_with_share),
                           "RT-patch method plus overlapping defaults"))
        qxp_check(qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_RTPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 ==
                      D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET,
                  "RT-patch and share-offset masks remain independent");
    for (prefix = 1; prefix <= QXP_ARRAY_COUNT(combined); ++prefix) {
        if (!qxp_expect_success(combined, prefix,
                                "every valid tessellation prefix")) break;
    }
    if (prefix == QXP_ARRAY_COUNT(combined) + 1)
        qxp_check(qxp_declaration_a.value.requires_tessellation &&
                  qxp_declaration_a.value.required_dev_caps ==
                      D3DDEVCAPS_RTPATCHES &&
                  qxp_declaration_a.value.required_dev_caps2 == 0 &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      0 &&
                  qxp_declaration_a.value.stream_spans[0] == 12,
                  "combined patch methods retain one exact RT-patch mask");
    qxp_expect_failure(modern_tessellation,
                       QXP_ARRAY_COUNT(modern_tessellation),
                       QXP_VERTEX_DECLARATION9_ERROR_TESSELLATION_TYPE, 0,
                       "tessellation declarations reject post-SHORT4 types");
}

static void qxp_test_tessfactor_and_unused(void)
{
    static const QXP_VERTEX_ELEMENT9 valid[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TESSFACTOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TESSFACTOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 bad_index[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TESSFACTOR, 1)
    };
    static const QXP_VERTEX_ELEMENT9 tessfactor_with_modern_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TESSFACTOR, 0),
        QXP_E(0, 4, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 unused_default[] = {
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 unused_lookup[] = {
        QXP_E(0, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_LOOKUP,
              D3DDECLUSAGE_SAMPLE, 0)
    };

    if (qxp_expect_success(valid, QXP_ARRAY_COUNT(valid),
                           "valid TESSFACTOR declaration"))
        qxp_check(qxp_declaration_a.value.requires_tessellation &&
                  qxp_declaration_a.value.required_dev_caps == 0 &&
                  qxp_declaration_a.value.required_dev_caps2 == 0 &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      0 &&
                  !qxp_declaration_a.value.
                      requires_programmable_vertex_pipeline &&
                  qxp_declaration_a.value.elements[0].byte_size == 4,
                  "TESSFACTOR alone has no exact device-cap requirement");
    if (qxp_expect_success(tessfactor_with_modern_type,
                           QXP_ARRAY_COUNT(tessfactor_with_modern_type),
                           "TESSFACTOR permits ordinary modern inputs"))
        qxp_check(qxp_declaration_a.value.required_decl_types ==
                      D3DDTCAPS_FLOAT16_2 &&
                  qxp_declaration_a.value.required_dev_caps == 0 &&
                  qxp_declaration_a.value.required_vertex_shader_version ==
                      QXP_EXPECTED_VERTEX_SHADER_VERSION_2_0 &&
                  qxp_declaration_a.value.
                      requires_programmable_vertex_pipeline &&
                  qxp_declaration_a.value.requires_tessellation,
                  "TESSFACTOR keeps independent DeclTypes and VS metadata");
    qxp_expect_failure(bad_type, QXP_ARRAY_COUNT(bad_type),
                       QXP_VERTEX_DECLARATION9_ERROR_TESSFACTOR, 0,
                       "TESSFACTOR requires FLOAT1");
    qxp_expect_failure(bad_index, QXP_ARRAY_COUNT(bad_index),
                       QXP_VERTEX_DECLARATION9_ERROR_TESSFACTOR, 0,
                       "TESSFACTOR requires usage index zero");
    qxp_expect_failure(unused_default, QXP_ARRAY_COUNT(unused_default),
                       QXP_VERTEX_DECLARATION9_ERROR_UNUSED_TYPE, 0,
                       "UNUSED is not an ordinary element type");
    qxp_expect_failure(unused_lookup, QXP_ARRAY_COUNT(unused_lookup),
                       QXP_VERTEX_DECLARATION9_ERROR_UNUSED_TYPE, 0,
                       "UNUSED is limited to explicit pseudo methods");
}

static void qxp_test_nulls_zeroing_and_canaries(void)
{
    static const QXP_VERTEX_ELEMENT9 valid[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    QXP_GUARDED_ELEMENTS9 guarded;
    QXP_VERTEX_ELEMENT9 copy[4];
    DWORD index;
    BOOL result;

    qxp_expect_failure(NULL, 1,
                       QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT, 0,
                       "NULL element pointer fails deterministically");

    qxp_prepare_error(&qxp_error_a);
    result = QxpParseVertexDeclaration9(
        valid, QXP_ARRAY_COUNT(valid), NULL, &qxp_error_a.value);
    qxp_check(!result && qxp_error_canaries_ok(&qxp_error_a) &&
              qxp_error_a.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT &&
              qxp_error_a.value.element_index == 0 &&
              qxp_error_a.value.byte_offset == 0,
              "NULL declaration reports deterministic error");

    memset(&qxp_declaration_a.value, 0xa5,
           sizeof(qxp_declaration_a.value));
    result = QxpParseVertexDeclaration9(
        valid, QXP_ARRAY_COUNT(valid), &qxp_declaration_a.value, NULL);
    qxp_check(!result &&
              !memcmp(&qxp_declaration_a.value, &qxp_zero_declaration,
                      sizeof(qxp_zero_declaration)),
              "NULL error pointer still zeroes declaration");

    memset(&guarded, 0, sizeof(guarded));
    for (index = 0; index < QXP_ARRAY_COUNT(guarded.before); ++index) {
        guarded.before[index] = QXP_CANARY_A + index;
        guarded.after[index] = QXP_CANARY_B - index;
    }
    memcpy(guarded.values, valid, sizeof(valid));
    qxp_set_element(&guarded.values[1], 0, 4, D3DDECLTYPE_FLOAT1,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0);
    qxp_set_element(&guarded.values[2], 0, 8, D3DDECLTYPE_FLOAT1,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0);
    qxp_set_element(&guarded.values[3], 0, 12, D3DDECLTYPE_FLOAT1,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0);
    memcpy(copy, guarded.values, sizeof(copy));
    qxp_expect_success(guarded.values, QXP_ARRAY_COUNT(guarded.values),
                       "guarded input declaration");
    qxp_check(!memcmp(copy, guarded.values, sizeof(copy)),
              "parser never modifies input elements");
    for (index = 0; index < QXP_ARRAY_COUNT(guarded.before); ++index) {
        if (guarded.before[index] != QXP_CANARY_A + index ||
            guarded.after[index] != QXP_CANARY_B - index) {
            qxp_check(0, "input canaries remain intact");
            break;
        }
    }
}

static void qxp_test_parsed_integrity(void)
{
    QXP_VERTEX_ELEMENT9 ordinary[2];
    QXP_VERTEX_ELEMENT9 partial[2];

    qxp_set_element(&ordinary[0], 0, 0, D3DDECLTYPE_FLOAT2,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0);
    qxp_set_element(&ordinary[1], 0, 8, D3DDECLTYPE_FLOAT3,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0);
    qxp_prepare_declaration(&qxp_declaration_a);
    qxp_prepare_error(&qxp_error_a);
    qxp_check(QxpParseVertexDeclaration9(
                  ordinary, QXP_ARRAY_COUNT(ordinary),
                  &qxp_declaration_a.value, &qxp_error_a.value),
              "integrity baseline parses");
    qxp_prepare_error(&qxp_error_a);
    qxp_check(QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_a.value, &qxp_error_a.value) &&
              memcmp(&qxp_error_a.value, &qxp_zero_error,
                     sizeof(qxp_zero_error)) == 0,
              "exact parser result validates");
    qxp_check(qxp_error_canaries_ok(&qxp_error_a),
              "integrity success error canaries");

    qxp_declaration_b.value = qxp_declaration_a.value;
    qxp_declaration_b.value.elements[0].method = D3DDECLMETHOD_UV;
    qxp_prepare_error(&qxp_error_b);
    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_b.value, &qxp_error_b.value) &&
              qxp_error_b.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE &&
              qxp_error_b.value.element_index == 0,
              "integrity rejects corrupt method and type");

    qxp_declaration_b.value = qxp_declaration_a.value;
    qxp_declaration_b.value.elements[0].usage =
        D3DDECLUSAGE_POSITIONT;
    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_b.value, &qxp_error_b.value) &&
              qxp_error_b.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_POSITION &&
              qxp_error_b.value.element_index == 1,
              "integrity rejects corrupt positiont combination");

    partial[0] = ordinary[1];
    partial[0].offset = 0;
    partial[1] = partial[0];
    partial[1].method = D3DDECLMETHOD_PARTIALU;
    partial[1].usage = D3DDECLUSAGE_NORMAL;
    qxp_check(QxpParseVertexDeclaration9(
                  partial, QXP_ARRAY_COUNT(partial),
                  &qxp_declaration_a.value, &qxp_error_a.value),
              "integrity partial baseline parses");
    qxp_declaration_b.value = qxp_declaration_a.value;
    qxp_declaration_b.value.elements[1].offset = 4;
    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_b.value, &qxp_error_b.value) &&
              qxp_error_b.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE &&
              qxp_error_b.value.element_index == 1,
              "integrity rejects corrupt generation reference");

    qxp_declaration_b.value = qxp_declaration_a.value;
    qxp_declaration_b.value.stream_mask ^= 2;
    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_b.value, &qxp_error_b.value) &&
              qxp_error_b.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE &&
              qxp_error_b.value.element_index ==
                  qxp_declaration_b.value.element_count,
              "integrity rejects corrupt derived state");

    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  NULL, &qxp_error_b.value) &&
              qxp_error_b.value.code ==
                  QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT,
              "integrity null declaration");
    qxp_check(!QxpValidateParsedVertexDeclaration9(
                  &qxp_declaration_a.value, NULL),
              "integrity null error");
}

static void qxp_test_fvf_supported_slots(void)
{
    static const QXP_VERTEX_ELEMENT9 position[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 positiont[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 normal[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 psize[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_PSIZE, 0)
    };
    static const QXP_VERTEX_ELEMENT9 diffuse[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 specular[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 1)
    };
    static const QXP_VERTEX_ELEMENT9 tex1[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 tex2[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 tex3[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 tex4[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };

    qxp_expect_fvf_success(
        position, QXP_ARRAY_COUNT(position), D3DFVF_XYZ, 12,
        "FVF POSITION FLOAT3 maps to XYZ");
    qxp_expect_fvf_success(
        positiont, QXP_ARRAY_COUNT(positiont), D3DFVF_XYZRHW, 16,
        "FVF POSITIONT FLOAT4 maps to XYZRHW only");
    qxp_expect_fvf_success(
        normal, QXP_ARRAY_COUNT(normal), D3DFVF_XYZ | D3DFVF_NORMAL, 24,
        "FVF NORMAL FLOAT3 slot");
    qxp_expect_fvf_success(
        psize, QXP_ARRAY_COUNT(psize), D3DFVF_XYZ | D3DFVF_PSIZE, 16,
        "FVF PSIZE FLOAT1 slot");
    qxp_expect_fvf_success(
        diffuse, QXP_ARRAY_COUNT(diffuse),
        D3DFVF_XYZ | D3DFVF_DIFFUSE, 16,
        "FVF COLOR zero maps to diffuse");
    qxp_expect_fvf_success(
        specular, QXP_ARRAY_COUNT(specular),
        D3DFVF_XYZ | D3DFVF_SPECULAR, 16,
        "FVF COLOR one maps to specular");
    qxp_expect_fvf_success(
        tex1, QXP_ARRAY_COUNT(tex1),
        D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE1(0), 16,
        "FVF TEXCOORD FLOAT1 size bits");
    qxp_expect_fvf_success(
        tex2, QXP_ARRAY_COUNT(tex2), D3DFVF_XYZ | D3DFVF_TEX1, 20,
        "FVF TEXCOORD FLOAT2 default size bits");
    qxp_expect_fvf_success(
        tex3, QXP_ARRAY_COUNT(tex3),
        D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0), 24,
        "FVF TEXCOORD FLOAT3 size bits");
    qxp_expect_fvf_success(
        tex4, QXP_ARRAY_COUNT(tex4),
        D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0), 28,
        "FVF TEXCOORD FLOAT4 size bits");
}

static void qxp_test_fvf_realistic_and_texture_sets(void)
{
    static const QXP_VERTEX_ELEMENT9 realistic[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0),
        QXP_E(0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0),
        QXP_E(0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0)
    };
    static const QXP_VERTEX_ELEMENT9 textures[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 0),
        QXP_E(0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 1),
        QXP_E(0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 2),
        QXP_E(0, 36, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 3)
    };
    DWORD expected_fvf;
    DWORD offset;
    DWORD index;

    qxp_expect_fvf_success(
        realistic, QXP_ARRAY_COUNT(realistic),
        D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1,
        36, "realistic fixed-function declaration exact FVF");
    expected_fvf = D3DFVF_XYZ | D3DFVF_TEX4 |
                   D3DFVF_TEXCOORDSIZE1(0) |
                   D3DFVF_TEXCOORDSIZE3(2) |
                   D3DFVF_TEXCOORDSIZE4(3);
    qxp_expect_fvf_success(
        textures, QXP_ARRAY_COUNT(textures), expected_fvf, 52,
        "multiple texture sets carry count and size bits");

    qxp_set_element(&qxp_work_elements[0], 0, 0, D3DDECLTYPE_FLOAT3,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0);
    expected_fvf = D3DFVF_XYZ | D3DFVF_TEX8;
    offset = 12;
    for (index = 0; index < 8; ++index) {
        DWORD type;

        type = index & 3UL;
        qxp_set_element(&qxp_work_elements[index + 1], 0, offset, type,
                        D3DDECLMETHOD_DEFAULT,
                        D3DDECLUSAGE_TEXCOORD, index);
        offset += (type + 1UL) * sizeof(DWORD);
        if (type == D3DDECLTYPE_FLOAT1)
            expected_fvf |= D3DFVF_TEXCOORDSIZE1(index);
        else if (type == D3DDECLTYPE_FLOAT3)
            expected_fvf |= D3DFVF_TEXCOORDSIZE3(index);
        else if (type == D3DDECLTYPE_FLOAT4)
            expected_fvf |= D3DFVF_TEXCOORDSIZE4(index);
    }
    qxp_expect_fvf_success(
        qxp_work_elements, 9, expected_fvf, offset,
        "all eight sequential texture coordinate sets map exactly");
}

static void qxp_test_fvf_inverse(void)
{
    QXP_VERTEX_DECLARATION9 declaration;
    DWORD fvf;
    DWORD stride;

    memset(&declaration, 0xcd, sizeof(declaration));
    qxp_check(QxpBuildVertexDeclaration9FromFvf(
                  D3DFVF_XYZRHW | D3DFVF_PSIZE |
                      D3DFVF_DIFFUSE | D3DFVF_TEX1,
                  &declaration) &&
                  declaration.valid &&
                  declaration.element_count == 4 &&
                  declaration.stream_mask == 1UL &&
                  declaration.stream_spans[0] == 32UL &&
                  declaration.has_positiont &&
                  declaration.elements[0].usage ==
                      D3DDECLUSAGE_POSITIONT &&
                  declaration.elements[1].usage == D3DDECLUSAGE_PSIZE &&
                  declaration.elements[2].usage == D3DDECLUSAGE_COLOR &&
                  declaration.elements[3].usage == D3DDECLUSAGE_TEXCOORD,
              "FVF inverse builds exact A8 probe declaration");
    fvf = 0;
    stride = 0;
    qxp_check(QxpTranslateVertexDeclaration9ToFvf(
                  &declaration, &fvf, &stride) &&
                  fvf == (D3DFVF_XYZRHW | D3DFVF_PSIZE |
                          D3DFVF_DIFFUSE | D3DFVF_TEX1) &&
                  stride == 32UL,
              "FVF inverse round trips exact value and stride");

    memset(&declaration, 0xcd, sizeof(declaration));
    qxp_check(!QxpBuildVertexDeclaration9FromFvf(
                  D3DFVF_XYZRHW | D3DFVF_NORMAL, &declaration) &&
                  !declaration.valid && !declaration.element_count,
              "FVF inverse rejects transformed normal");
    memset(&declaration, 0xcd, sizeof(declaration));
    qxp_check(!QxpBuildVertexDeclaration9FromFvf(
                  D3DFVF_XYZ | D3DFVF_TEX1 |
                      D3DFVF_TEXCOORDSIZE3(1),
                  &declaration) &&
                  !declaration.valid && !declaration.element_count,
              "FVF inverse rejects texture size outside declared set");
    qxp_check(!QxpBuildVertexDeclaration9FromFvf(
                  D3DFVF_XYZ, NULL),
              "FVF inverse rejects null destination");
}

static void qxp_test_fvf_refusals(void)
{
    static const QXP_VERTEX_ELEMENT9 nonzero_stream[] = {
        QXP_E(1, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 nondefault_method[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_PARTIALU,
              D3DDECLUSAGE_POSITION, 1)
    };
    static const QXP_VERTEX_ELEMENT9 unsupported_usage[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_BLENDWEIGHT, 0)
    };
    static const QXP_VERTEX_ELEMENT9 wrong_type[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 out_of_order[] = {
        QXP_E(0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0),
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0)
    };
    static const QXP_VERTEX_ELEMENT9 gap[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 overlap[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_COLOR, 0)
    };
    static const QXP_VERTEX_ELEMENT9 nonsequential_texture[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_TEXCOORD, 1)
    };
    static const QXP_VERTEX_ELEMENT9 transformed_normal[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITIONT, 0),
        QXP_E(0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0)
    };
    static const QXP_VERTEX_ELEMENT9 duplicate_baseline[] = {
        QXP_E(0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_POSITION, 0),
        QXP_E(0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
              D3DDECLUSAGE_NORMAL, 0)
    };
    QXP_VERTEX_DECLARATION9 declaration;
    QXP_VERTEX_DECLARATION9_ERROR error;
    DWORD fvf;
    DWORD stride;
    DWORD offset;
    DWORD index;
    BOOL parsed;

    qxp_expect_fvf_refusal(
        nonzero_stream, QXP_ARRAY_COUNT(nonzero_stream),
        "FVF refuses nonzero stream");
    qxp_expect_fvf_refusal(
        nondefault_method, QXP_ARRAY_COUNT(nondefault_method),
        "FVF refuses nondefault method");
    qxp_expect_fvf_refusal(
        unsupported_usage, QXP_ARRAY_COUNT(unsupported_usage),
        "FVF refuses unsupported usage");
    qxp_expect_fvf_refusal(
        wrong_type, QXP_ARRAY_COUNT(wrong_type),
        "FVF refuses wrong slot type");
    qxp_expect_fvf_refusal(
        out_of_order, QXP_ARRAY_COUNT(out_of_order),
        "FVF refuses out-of-order elements and offsets");
    qxp_expect_fvf_refusal(
        gap, QXP_ARRAY_COUNT(gap), "FVF refuses offset gap");
    qxp_expect_fvf_refusal(
        overlap, QXP_ARRAY_COUNT(overlap), "FVF refuses offset overlap");
    qxp_expect_fvf_refusal(
        nonsequential_texture, QXP_ARRAY_COUNT(nonsequential_texture),
        "FVF refuses nonsequential texture usage index");
    qxp_expect_fvf_refusal(
        transformed_normal, QXP_ARRAY_COUNT(transformed_normal),
        "FVF refuses NORMAL with XYZRHW");

    memset(&declaration, 0, sizeof(declaration));
    memset(&error, 0, sizeof(error));
    parsed = QxpParseVertexDeclaration9(
        duplicate_baseline, QXP_ARRAY_COUNT(duplicate_baseline),
        &declaration, &error);
    declaration.elements[1].usage = D3DDECLUSAGE_POSITION;
    declaration.elements[1].usage_index = 0;
    fvf = QXP_CANARY_A;
    stride = QXP_CANARY_B;
    qxp_check(parsed && !QxpTranslateVertexDeclaration9ToFvf(
                  &declaration, &fvf, &stride) && !fvf && !stride,
              "FVF refuses duplicate semantic in corrupt parsed input");

    qxp_set_element(&qxp_work_elements[0], 0, 0, D3DDECLTYPE_FLOAT3,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0);
    offset = 12;
    for (index = 0; index < 9; ++index) {
        qxp_set_element(&qxp_work_elements[index + 1], 0, offset,
                        D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
                        D3DDECLUSAGE_TEXCOORD, index);
        offset += 8;
    }
    qxp_expect_fvf_refusal(
        qxp_work_elements, 10,
        "FVF refuses more than eight texture coordinate sets");
}

static DWORD qxp_mutation_next(DWORD *state)
{
    *state = *state * 1664525UL + 1013904223UL;
    return *state;
}

static void qxp_test_bounded_mutations(void)
{
    QXP_VERTEX_ELEMENT9 baseline[12];
    DWORD seed;
    DWORD iteration;
    DWORD index;
    int ok;

    for (index = 0; index < QXP_ARRAY_COUNT(baseline); ++index) {
        qxp_set_element(&baseline[index], 0, index * 4,
                        D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
                        index < 8 ? D3DDECLUSAGE_TEXCOORD :
                                    D3DDECLUSAGE_COLOR,
                        index < 8 ? index : index - 8);
    }
    seed = 0x6d2b79f5UL;
    ok = 1;
    for (iteration = 0; iteration < 512; ++iteration) {
        DWORD selected;
        DWORD byte_index;
        DWORD random_value;
        BYTE *bytes;

        memcpy(qxp_mutation_elements, baseline, sizeof(baseline));
        selected = qxp_mutation_next(&seed) % QXP_ARRAY_COUNT(baseline);
        byte_index = qxp_mutation_next(&seed) % QXP_VERTEX_ELEMENT9_BYTES;
        random_value = qxp_mutation_next(&seed);
        bytes = (BYTE *)&qxp_mutation_elements[selected];
        bytes[byte_index] = (BYTE)(bytes[byte_index] ^
                                   (BYTE)((random_value >> 24) | 1U));
        if (!qxp_audit_parse(qxp_mutation_elements,
                             QXP_ARRAY_COUNT(baseline),
                             QxpParseVertexDeclaration9(
                                 qxp_mutation_elements,
                                 QXP_ARRAY_COUNT(baseline),
                                 &qxp_declaration_a.value,
                                 &qxp_error_a.value),
                             QXP_ANY_VALUE, QXP_ANY_VALUE)) {
            ok = 0;
            break;
        }
    }
    qxp_check(ok && iteration == 512,
              "fixed-seed bounded mutations satisfy independent invariants");
}

int main(void)
{
    qxp_test_layout_and_private_values();
    qxp_test_ordinary_golden_and_no_end();
    qxp_test_all_types_and_caps();
    qxp_test_count_and_span_boundaries();
    qxp_test_structural_failures();
    qxp_test_share_offset_requirement();
    qxp_test_positiont();
    qxp_test_lookup_methods();
    qxp_test_tessellation_methods();
    qxp_test_tessfactor_and_unused();
    qxp_test_nulls_zeroing_and_canaries();
    qxp_test_parsed_integrity();
    qxp_test_fvf_supported_slots();
    qxp_test_fvf_realistic_and_texture_sets();
    qxp_test_fvf_refusals();
    qxp_test_fvf_inverse();
    qxp_test_bounded_mutations();
    if (qxp_failures) {
        fprintf(stderr, "qxp_vdecl9: %d contract checks failed\n",
                qxp_failures);
        return 1;
    }
    puts("qxp_vdecl9: all declaration checks passed");
    return 0;
}
