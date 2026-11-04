#include <stdio.h>
#include <string.h>

#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_vdecl.h"

static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_vdecl: failed: %s\n", name);
}

static BOOL qxp_parse(const DWORD *tokens, DWORD count,
                      QXP_VERTEX_DECLARATION *declaration)
{
    return QxpParseVertexDeclaration8(
        tokens, count * sizeof(tokens[0]), declaration);
}

static void qxp_zero_views(QXP_VERTEX_STREAM_VIEW *views)
{
    DWORD index;

    for (index = 0; index < QXP_VERTEX_DECLARATION_STREAM_COUNT; ++index) {
        views[index].base = NULL;
        views[index].bytes = 0;
        views[index].stride = 0;
        views[index].wraps = FALSE;
    }
}

static void qxp_test_types_skips_and_constants(void)
{
    static const DWORD declaration_tokens[] = {
        D3DVSD_STREAM(0),
        D3DVSD_NOP(),
        D3DVSD_REG(0, D3DVSDT_FLOAT1),
        D3DVSD_REG(1, D3DVSDT_FLOAT2),
        D3DVSD_REG(2, D3DVSDT_FLOAT3),
        D3DVSD_REG(3, D3DVSDT_FLOAT4),
        D3DVSD_SKIP(2),
        D3DVSD_REG(4, D3DVSDT_D3DCOLOR),
        D3DVSD_REG(5, D3DVSDT_UBYTE4),
        D3DVSD_REG(6, D3DVSDT_SHORT2),
        D3DVSD_REG(7, D3DVSDT_SHORT4),
        D3DVSD_CONST(2, 2),
            0x3f800000UL, 0x40000000UL, 0x40400000UL, 0x40800000UL,
            0xbf800000UL, 0xc0000000UL, 0xc0400000UL, 0xc0800000UL,
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;

    qxp_check(qxp_parse(
                  declaration_tokens,
                  sizeof(declaration_tokens) /
                      sizeof(declaration_tokens[0]), &declaration),
              "all declaration data types parse");
    qxp_check(declaration.input_mask == 0xff &&
              declaration.element_count == 8 &&
              declaration.stream_mask == 0x1,
              "declaration input metadata");
    qxp_check(declaration.elements[0].byte_offset == 0 &&
              declaration.elements[0].byte_count == 4 &&
              declaration.elements[1].byte_offset == 4 &&
              declaration.elements[1].byte_count == 8 &&
              declaration.elements[2].byte_offset == 12 &&
              declaration.elements[2].byte_count == 12 &&
              declaration.elements[3].byte_offset == 24 &&
              declaration.elements[3].byte_count == 16,
              "float declaration offsets");
    /* Skips consume source bytes but emit no packed output, so packed offsets
     * after the skip are tighter than the source offsets. */
    qxp_check(declaration.elements[4].byte_offset == 40 &&
              declaration.elements[5].byte_offset == 44 &&
              declaration.elements[6].byte_offset == 48 &&
              declaration.elements[7].byte_offset == 52 &&
              declaration.byte_stride == 60,
              "packed declaration offsets and stride");
    qxp_check(declaration.elements[3].source_byte_offset == 24 &&
              declaration.elements[4].source_byte_offset == 48 &&
              declaration.elements[5].source_byte_offset == 52 &&
              declaration.elements[7].source_byte_offset == 60 &&
              declaration.stream_spans[0] == 68,
              "source offsets track skips");
    qxp_check(declaration.elements[0].source_stream == 0 &&
              declaration.elements[7].source_stream == 0,
              "single stream source assignment");
    qxp_check(declaration.defined_constants[0] == 12 &&
              declaration.defined_constants[1] == 0 &&
              declaration.defined_constants[2] == 0,
              "declaration constant mask");
    qxp_check(declaration.constant_values[2][0] == 0x3f800000UL &&
              declaration.constant_values[2][3] == 0x40800000UL &&
              declaration.constant_values[3][0] == 0xbf800000UL &&
              declaration.constant_values[3][3] == 0xc0800000UL,
              "declaration constant values");
}

static void qxp_test_last_definition_wins(void)
{
    static const DWORD declaration_tokens[] = {
        D3DVSD_CONST(95, 1), 1, 2, 3, 4,
        D3DVSD_NOP(),
        D3DVSD_CONST(95, 1), 5, 6, 7, 8,
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;

    qxp_check(qxp_parse(
                  declaration_tokens,
                  sizeof(declaration_tokens) /
                      sizeof(declaration_tokens[0]), &declaration),
              "constant-only declaration parses");
    qxp_check(declaration.input_mask == 0 &&
              declaration.byte_stride == 0 &&
              declaration.stream_mask == 0 &&
              declaration.defined_constants[2] == 0x80000000UL &&
              declaration.constant_values[95][0] == 5 &&
              declaration.constant_values[95][3] == 8,
              "last declaration constant wins");
}

static void qxp_test_stride_boundary(void)
{
    DWORD declaration_tokens[8];
    DWORD cursor = 0;
    DWORD index;
    QXP_VERTEX_DECLARATION declaration;

    declaration_tokens[cursor++] = D3DVSD_STREAM(0);
    for (index = 0; index < 4; ++index)
        declaration_tokens[cursor++] = D3DVSD_SKIP(15);
    declaration_tokens[cursor++] = D3DVSD_REG(0, D3DVSDT_FLOAT3);
    declaration_tokens[cursor++] = D3DVSD_END();
    qxp_check(qxp_parse(declaration_tokens, cursor, &declaration) &&
              declaration.stream_spans[0] == 252 &&
              declaration.byte_stride == 12,
              "largest source span below advertised limit accepted");

    declaration_tokens[cursor - 2] = D3DVSD_REG(0, D3DVSDT_FLOAT4);
    qxp_check(!qxp_parse(declaration_tokens, cursor, &declaration),
              "source span above advertised limit rejected");
}

static void qxp_test_multi_stream(void)
{
    static const DWORD declaration_tokens[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_STREAM(1), D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
                          D3DVSD_REG(2, D3DVSDT_FLOAT2),
        D3DVSD_STREAM(2), D3DVSD_REG(3, D3DVSDT_FLOAT1),
        D3DVSD_STREAM(15), D3DVSD_REG(5, D3DVSDT_FLOAT1),
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;
    QXP_VERTEX_STREAM_VIEW views[QXP_VERTEX_DECLARATION_STREAM_COUNT];
    BYTE stream0[36];
    BYTE stream1[36];
    BYTE stream2[12];
    BYTE stream15[8];
    BYTE destination[64];
    BYTE expected[32];
    DWORD index;

    qxp_check(qxp_parse(
                  declaration_tokens,
                  sizeof(declaration_tokens) /
                      sizeof(declaration_tokens[0]), &declaration),
              "multi-stream declaration parses");
    qxp_check(declaration.stream_mask == ((1UL << 0) | (1UL << 1) |
                                          (1UL << 2) | (1UL << 15)) &&
              declaration.element_count == 5 &&
              declaration.byte_stride == 32,
              "multi-stream mask and packed stride");
    qxp_check(declaration.elements[0].source_stream == 0 &&
              declaration.elements[0].source_byte_offset == 0 &&
              declaration.elements[0].byte_offset == 0 &&
              declaration.elements[1].source_stream == 1 &&
              declaration.elements[1].source_byte_offset == 0 &&
              declaration.elements[1].byte_offset == 12 &&
              declaration.elements[2].source_stream == 1 &&
              declaration.elements[2].source_byte_offset == 4 &&
              declaration.elements[2].byte_offset == 16 &&
              declaration.elements[3].source_stream == 2 &&
              declaration.elements[3].byte_offset == 24 &&
              declaration.elements[4].source_stream == 15 &&
              declaration.elements[4].byte_offset == 28,
              "per-element stream and offset mapping");
    qxp_check(declaration.stream_spans[0] == 12 &&
              declaration.stream_spans[1] == 12 &&
              declaration.stream_spans[2] == 4 &&
              declaration.stream_spans[15] == 4,
              "per-stream spans");

    for (index = 0; index < sizeof(stream0); ++index) stream0[index] =
        (BYTE)(0x10 + index);
    for (index = 0; index < sizeof(stream1); ++index) stream1[index] =
        (BYTE)(0x40 + index);
    for (index = 0; index < sizeof(stream2); ++index) stream2[index] =
        (BYTE)(0x80 + index);
    for (index = 0; index < sizeof(stream15); ++index) stream15[index] =
        (BYTE)(0xc0 + index);

    qxp_zero_views(views);
    views[0].base = stream0; views[0].bytes = sizeof(stream0);
    views[0].stride = 12;
    views[1].base = stream1; views[1].bytes = sizeof(stream1);
    views[1].stride = 12;
    views[2].base = stream2; views[2].bytes = sizeof(stream2);
    views[2].stride = 4;
    views[15].base = stream15; views[15].bytes = sizeof(stream15);
    views[15].stride = 4;

    /* Gather logical vertex 1: each stream advances by its own stride. */
    memset(destination, 0xa5, sizeof(destination));
    qxp_check(QxpGatherVertexDeclaration8(&declaration, views, 1,
                                          destination, sizeof(destination)),
              "multi-stream gather succeeds");
    memcpy(expected + 0, stream0 + 12, 12);          /* reg0 FLOAT3 */
    expected[12] = stream1[14];                        /* D3DCOLOR swap */
    expected[13] = stream1[13];
    expected[14] = stream1[12];
    expected[15] = stream1[15];
    memcpy(expected + 16, stream1 + 16, 8);          /* reg2 FLOAT2 */
    memcpy(expected + 24, stream2 + 4, 4);           /* reg3 FLOAT1 */
    memcpy(expected + 28, stream15 + 4, 4);          /* reg5 FLOAT1 */
    qxp_check(!memcmp(destination, expected, 32) &&
              destination[32] == 0xa5,
              "multi-stream gather packs from each stream");

    /* A missing binding for a referenced stream must fail closed. */
    views[1].base = NULL;
    qxp_check(!QxpGatherVertexDeclaration8(&declaration, views, 1,
                                           destination, sizeof(destination)),
              "gather rejects unbound stream");
    views[1].base = stream1;

    /* An out-of-range vertex index for the tightest stream fails. */
    qxp_check(!QxpGatherVertexDeclaration8(&declaration, views, 3,
                                           destination, sizeof(destination)),
              "gather rejects out-of-range vertex");

    /* A destination smaller than the packed stride fails. */
    qxp_check(!QxpGatherVertexDeclaration8(&declaration, views, 0,
                                           destination, 31),
              "gather rejects short destination");
}

static void qxp_test_stream_wrap(void)
{
    static const DWORD declaration_tokens[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT1),
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;
    QXP_VERTEX_STREAM_VIEW views[QXP_VERTEX_DECLARATION_STREAM_COUNT];
    BYTE stream0[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    BYTE destination[8];
    BYTE expected[4];

    qxp_check(qxp_parse(
                  declaration_tokens,
                  sizeof(declaration_tokens) /
                      sizeof(declaration_tokens[0]), &declaration),
              "wrap declaration parses");
    qxp_zero_views(views);
    views[0].base = stream0;
    views[0].bytes = sizeof(stream0);
    views[0].stride = 4;
    views[0].wraps = TRUE;
    /* Vertex 1 starts at byte 4; a FLOAT1 reads bytes 4,5 then wraps to 0,1. */
    qxp_check(QxpGatherVertexDeclaration8(&declaration, views, 1,
                                          destination, sizeof(destination)),
              "wrapped gather succeeds");
    expected[0] = stream0[4];
    expected[1] = stream0[5];
    expected[2] = stream0[0];
    expected[3] = stream0[1];
    qxp_check(!memcmp(destination, expected, 4),
              "wrapped element crosses the allocation end");
}

static void qxp_test_permitted(void)
{
    static const DWORD empty_stream[] = {
        D3DVSD_STREAM(0), D3DVSD_NOP(), D3DVSD_END()
    };
    static const DWORD stream_one[] = {
        D3DVSD_STREAM(1), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_END()
    };
    static const DWORD skip_only_stream[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT2),
        D3DVSD_STREAM(1), D3DVSD_SKIP(2),
        D3DVSD_END()
    };
    static const DWORD repeated_stream[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_STREAM(0), D3DVSD_REG(1, D3DVSDT_FLOAT3),
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;

    qxp_check(qxp_parse(empty_stream, sizeof(empty_stream) /
                        sizeof(empty_stream[0]), &declaration) &&
              declaration.element_count == 0 &&
              declaration.stream_mask == 0 &&
              declaration.byte_stride == 0,
              "empty stream permitted");
    qxp_check(qxp_parse(stream_one, sizeof(stream_one) /
                        sizeof(stream_one[0]), &declaration) &&
              declaration.stream_mask == 0x2 &&
              declaration.elements[0].source_stream == 1 &&
              declaration.elements[0].byte_offset == 0 &&
              declaration.stream_spans[1] == 12,
              "stream 1 only declaration permitted");
    qxp_check(qxp_parse(skip_only_stream, sizeof(skip_only_stream) /
                        sizeof(skip_only_stream[0]), &declaration) &&
              declaration.stream_mask == 0x1 &&
              declaration.stream_spans[1] == 8,
              "skip-only stream carries a span but no binding");
    /* A repeated selector restarts that stream's source cursor. */
    qxp_check(qxp_parse(repeated_stream, sizeof(repeated_stream) /
                        sizeof(repeated_stream[0]), &declaration) &&
              declaration.element_count == 2 &&
              declaration.elements[0].source_byte_offset == 0 &&
              declaration.elements[1].source_byte_offset == 0 &&
              declaration.elements[0].byte_offset == 0 &&
              declaration.elements[1].byte_offset == 12 &&
              declaration.stream_spans[0] == 12,
              "repeated stream selector resets source cursor");
}

static void qxp_test_rejections(void)
{
    static const DWORD missing_end[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3)
    };
    static const DWORD trailing_end[] = {
        D3DVSD_END(), D3DVSD_NOP()
    };
    static const DWORD tess_stream[] = {
        D3DVSD_STREAM_TESS(), D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_END()
    };
    static const DWORD orphan_data[] = {
        D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_END()
    };
    static const DWORD duplicate_register[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(0, D3DVSDT_FLOAT2), D3DVSD_END()
    };
    static const DWORD duplicate_register_cross_stream[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_STREAM(1), D3DVSD_REG(0, D3DVSDT_FLOAT2), D3DVSD_END()
    };
    static const DWORD invalid_type[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, 8), D3DVSD_END()
    };
    static const DWORD reserved_data_bit[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3) | 0x00000100UL,
        D3DVSD_END()
    };
    static const DWORD zero_constant_count[] = {
        D3DVSD_CONST(0, 0), D3DVSD_END()
    };
    static const DWORD constant_overflow[] = {
        D3DVSD_CONST(95, 2),
            1, 2, 3, 4, 5, 6, 7, 8,
        D3DVSD_END()
    };
    static const DWORD truncated_constant[] = {
        D3DVSD_CONST(0, 1), 1, 2, 3
    };
    static const DWORD tessellator[] = {
        D3DVSD_TESSUV(0), D3DVSD_END()
    };
    static const DWORD extension[] = {
        D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_EXT), D3DVSD_END()
    };
    static const DWORD noncanonical_end[] = {
        D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_END), D3DVSD_END()
    };
    static const DWORD noncanonical_nop[] = {
        1, D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;

#define QXP_REJECT(tokens, name) \
    qxp_check(!qxp_parse((tokens), \
        sizeof(tokens) / sizeof((tokens)[0]), &declaration), (name))
    QXP_REJECT(missing_end, "missing end rejected");
    QXP_REJECT(trailing_end, "trailing token after end rejected");
    QXP_REJECT(tess_stream, "tessellator stream rejected");
    QXP_REJECT(orphan_data, "stream data without selector rejected");
    QXP_REJECT(duplicate_register, "duplicate input register rejected");
    QXP_REJECT(duplicate_register_cross_stream,
               "duplicate input register across streams rejected");
    QXP_REJECT(invalid_type, "invalid data type rejected");
    QXP_REJECT(reserved_data_bit, "reserved stream-data bit rejected");
    QXP_REJECT(zero_constant_count, "zero constant count rejected");
    QXP_REJECT(constant_overflow, "constant range overflow rejected");
    QXP_REJECT(truncated_constant, "truncated constant payload rejected");
    QXP_REJECT(tessellator, "tessellator token rejected");
    QXP_REJECT(extension, "extension token rejected");
    QXP_REJECT(noncanonical_end, "noncanonical end rejected");
    QXP_REJECT(noncanonical_nop, "noncanonical nop rejected");
#undef QXP_REJECT

    qxp_check(!QxpParseVertexDeclaration8(
                  (const DWORD[]){ D3DVSD_END() }, 3, &declaration),
              "unaligned declaration byte count rejected");
}

static void qxp_test_raw_repack(void)
{
    static const DWORD declaration_tokens[] = {
        D3DVSD_STREAM(0),
        D3DVSD_SKIP(1),
        D3DVSD_REG(3, D3DVSDT_D3DCOLOR),
        D3DVSD_REG(7, D3DVSDT_UBYTE4),
        D3DVSD_SKIP(1),
        D3DVSD_END()
    };
    static const BYTE source[] = {
        0x10, 0x11, 0x12, 0x13,
        0x21, 0x22, 0x23, 0x24,
        0x30, 0x31, 0x32, 0x33,
        0x40, 0x41, 0x42, 0x43,
        0xee, 0xef
    };
    /* Packed record excludes the skipped source bytes: D3DCOLOR (swapped)
     * then UBYTE4. */
    static const BYTE expected[] = {
        0x23, 0x22, 0x21, 0x24,
        0x30, 0x31, 0x32, 0x33
    };
    static const DWORD multi_stream_tokens[] = {
        D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT2),
        D3DVSD_STREAM(1), D3DVSD_REG(1, D3DVSDT_FLOAT1),
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;
    QXP_VERTEX_DECLARATION multi_stream;
    QXP_VERTEX_DECLARATION constant_only;
    BYTE destination[sizeof(source)];

    qxp_check(qxp_parse(
                  declaration_tokens,
                  sizeof(declaration_tokens) /
                      sizeof(declaration_tokens[0]), &declaration) &&
              declaration.byte_stride == sizeof(expected) &&
              declaration.stream_spans[0] == 16,
              "repack declaration parses");
    memset(destination, 0xa5, sizeof(destination));
    qxp_check(QxpRepackVertexDeclaration8(
                  &declaration, source, declaration.stream_spans[0],
                  destination, sizeof(destination)),
              "raw declaration record repacks");
    qxp_check(!memcmp(destination, expected, sizeof(expected)) &&
              destination[sizeof(expected)] == 0xa5,
              "only D3DCOLOR red and blue bytes canonicalized");
    qxp_check(!QxpRepackVertexDeclaration8(
                  &declaration, source, declaration.stream_spans[0] - 1,
                  destination, sizeof(destination)),
              "short source stride rejected");
    qxp_check(!QxpRepackVertexDeclaration8(
                  &declaration, source, declaration.stream_spans[0],
                  destination, declaration.byte_stride - 1),
              "short destination rejected");

    /* Repack is a single-stream helper: a multi-stream declaration must be
     * rejected so callers use the gather with per-stream sources. */
    qxp_check(qxp_parse(multi_stream_tokens, sizeof(multi_stream_tokens) /
                        sizeof(multi_stream_tokens[0]), &multi_stream) &&
              !QxpRepackVertexDeclaration8(
                  &multi_stream, source, 8, destination,
                  sizeof(destination)),
              "multi-stream declaration rejected by single-stream repack");

    qxp_check(qxp_parse((const DWORD[]){ D3DVSD_CONST(0, 1),
                  1, 2, 3, 4, D3DVSD_END() }, 6, &constant_only) &&
              QxpRepackVertexDeclaration8(
                  &constant_only, NULL, 0, NULL, 0),
              "constant-only declaration needs no vertex bytes");
}

static void qxp_test_fixed_function_translation(void)
{
    static const DWORD tokens[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(3, D3DVSDT_FLOAT3),
        D3DVSD_STREAM(1),
        D3DVSD_REG(6, D3DVSDT_D3DCOLOR),
        D3DVSD_STREAM(2),
        D3DVSD_REG(5, D3DVSDT_D3DCOLOR),
        D3DVSD_STREAM(3),
        D3DVSD_REG(7, D3DVSDT_FLOAT2),
        D3DVSD_END()
    };
    static const DWORD gap_tokens[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(8, D3DVSDT_FLOAT2),
        D3DVSD_END()
    };
    static const DWORD constant_tokens[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_CONST(0, 1), 1, 2, 3, 4,
        D3DVSD_END()
    };
    QXP_VERTEX_DECLARATION declaration;
    QXP_VERTEX_STREAM_VIEW views[QXP_VERTEX_DECLARATION_STREAM_COUNT];
    BYTE position_normal[24];
    BYTE specular[4] = { 0x11, 0x22, 0x33, 0x44 };
    BYTE diffuse[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    BYTE texture[8];
    BYTE destination[48];
    DWORD fvf;
    DWORD index;

    for (index = 0; index < sizeof(position_normal); ++index)
        position_normal[index] = (BYTE)(0x40 + index);
    for (index = 0; index < sizeof(texture); ++index)
        texture[index] = (BYTE)(0x80 + index);
    fvf = 0;
    qxp_check(qxp_parse(tokens, sizeof(tokens) / sizeof(tokens[0]),
                        &declaration) &&
              QxpTranslateFixedVertexDeclaration8(&declaration, &fvf),
              "fixed declaration translates");
    qxp_check(fvf == 0x000001d2UL && declaration.byte_stride == 40 &&
              declaration.elements[0].byte_offset == 0 &&
              declaration.elements[1].byte_offset == 12 &&
              declaration.elements[2].byte_offset == 28 &&
              declaration.elements[3].byte_offset == 24 &&
              declaration.elements[4].byte_offset == 32,
              "fixed declaration derives canonical FVF order");
    qxp_zero_views(views);
    views[0].base = position_normal;
    views[0].bytes = sizeof(position_normal);
    views[0].stride = sizeof(position_normal);
    views[1].base = specular;
    views[1].bytes = sizeof(specular);
    views[1].stride = sizeof(specular);
    views[2].base = diffuse;
    views[2].bytes = sizeof(diffuse);
    views[2].stride = sizeof(diffuse);
    views[3].base = texture;
    views[3].bytes = sizeof(texture);
    views[3].stride = sizeof(texture);
    memset(destination, 0, sizeof(destination));
    qxp_check(QxpGatherFixedVertexDeclaration8(
                  &declaration, views, 0, destination,
                  sizeof(destination)),
              "fixed declaration gathers multiple streams");
    qxp_check(!memcmp(destination, position_normal, 24) &&
              !memcmp(destination + 24, diffuse, 4) &&
              !memcmp(destination + 28, specular, 4) &&
              !memcmp(destination + 32, texture, 8),
              "fixed gather preserves FVF and D3DCOLOR byte order");

    fvf = 1;
    qxp_check(qxp_parse(gap_tokens,
                        sizeof(gap_tokens) / sizeof(gap_tokens[0]),
                        &declaration) &&
              !QxpTranslateFixedVertexDeclaration8(&declaration, &fvf) &&
              fvf == 0,
              "fixed declaration rejects texture-coordinate gaps");
    fvf = 1;
    qxp_check(qxp_parse(constant_tokens,
                        sizeof(constant_tokens) /
                            sizeof(constant_tokens[0]), &declaration) &&
              !QxpTranslateFixedVertexDeclaration8(&declaration, &fvf) &&
              fvf == 0,
              "fixed declaration rejects embedded constants");
}

int main(void)
{
    qxp_test_types_skips_and_constants();
    qxp_test_last_definition_wins();
    qxp_test_stride_boundary();
    qxp_test_multi_stream();
    qxp_test_stream_wrap();
    qxp_test_permitted();
    qxp_test_rejections();
    qxp_test_raw_repack();
    qxp_test_fixed_function_translation();
    if (qxp_failures) {
        fprintf(stderr, "qxp_vdecl: %d contract checks failed\n",
                qxp_failures);
        return 1;
    }
    puts("qxp_vdecl: all declaration checks passed");
    return 0;
}
