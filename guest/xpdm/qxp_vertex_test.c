#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_vertex.h"

#define FVF_XYZ 0x002UL
#define FVF_XYZRHW 0x004UL
#define FVF_NORMAL 0x010UL
#define FVF_PSIZE 0x020UL
#define FVF_DIFFUSE 0x040UL
#define FVF_SPECULAR 0x080UL
#define FVF_FOG 0x2000UL
#define FVF_TEX1 0x100UL
#define FVF_TEX2 0x200UL
#define FVF_TEXCOORDSIZE1(index) (3UL << (16 + (index) * 2))
#define FVF_TEXCOORDSIZE3(index) (1UL << (16 + (index) * 2))

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "qxp_vertex_test: %s\n", message);
    return 0;
}

static void put_word(UCHAR *bytes, ULONG offset, ULONG value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

int main(void)
{
    QXP_D3D_VERTEX_LAYOUT transformed;
    QXP_D3D_VERTEX_LAYOUT object;
    QXP_D3D_VERTEX vertex;
    QXP_D3D_OBJECT_VERTEX_DATA object_data;
    UCHAR source[64];
    UCHAR dense_indices[6] = { 2, 3, 4, 2, 4, 3 };
    UCHAR dense_vertices[20];
    UCHAR dense_written[5] = { 1, 1, 1, 1, 1 };
    UCHAR expected_indices[6] = { 0, 1, 2, 0, 2, 1 };
    UCHAR expected_vertices[12];
    ULONG map_vertices[QXP_VERTEX_COMPACT_MAP_SLOTS];
    UCHAR map_indices[QXP_VERTEX_COMPACT_MAP_SLOTS];
    UCHAR mapped_index;
    ULONG unique_count;
    ULONG map_test_index;
    ULONG object_offset;
    ULONG state_offset;
    ULONG request_bytes;
    ULONG preflight_request_bytes;
    ULONG index_count;
    ULONG expected_bytes;
    ULONG dense_vertex_count;
    ULONG header_bytes = sizeof(QXP_D3D_DRAW_REQUEST) -
                         sizeof(QXP_D3D_VERTEX);

    if (!require(qxp_vertex_decode_layout(
                     FVF_XYZRHW | FVF_PSIZE | FVF_DIFFUSE |
                     FVF_SPECULAR | FVF_TEX1 | FVF_FOG, &transformed),
                 "decode transformed FVF") ||
        !require(transformed.transformed && !transformed.has_normal &&
                 transformed.has_fog && transformed.vertex_bytes == 40 &&
                 transformed.diffuse_offset == 20 &&
                 transformed.specular_offset == 24 &&
                 transformed.texcoord_offset[0] == 28 &&
                 transformed.texcoord_dimensions[0] == 2 &&
                 transformed.fog_offset == 36,
                 "transformed offsets") ||
        !require(qxp_vertex_decode_layout(
                     FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | FVF_SPECULAR |
                     FVF_TEX2 | FVF_TEXCOORDSIZE3(0) |
                     FVF_TEXCOORDSIZE1(1) | FVF_FOG, &object),
                 "decode object FVF") ||
        !require(!object.transformed && object.has_normal &&
                 object.has_fog && object.normal_offset == 12 &&
                 object.diffuse_offset == 24 &&
                 object.specular_offset == 28 &&
                 object.texcoord_offset[0] == 32 &&
                 object.texcoord_dimensions[0] == 3 &&
                 object.texcoord_offset[1] == 44 &&
                 object.texcoord_dimensions[1] == 1 &&
                 object.fog_offset == 48 &&
                 object.vertex_bytes == 52,
                 "object offsets") ||
        !require(!qxp_vertex_decode_layout(
                     FVF_XYZRHW | FVF_NORMAL, &object),
                 "accepted transformed normal") ||
        !require(!qxp_vertex_decode_layout(0x006, &object),
                 "accepted blended position") ||
        !require(!qxp_vertex_decode_layout(
                     FVF_XYZ | FVF_TEX1 | FVF_TEXCOORDSIZE3(1), &object),
                 "accepted unused coordinate format")) return 1;

    qxp_vertex_decode_layout(
        FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1 | FVF_FOG, &transformed);
    memset(source, 0, sizeof(source));
    put_word(source, 0, 0x3f000000UL);
    put_word(source, 4, 0x3f400000UL);
    put_word(source, 8, 0x3f800000UL);
    put_word(source, 12, 0x40000000UL);
    put_word(source, 16, 0xffabcdefUL);
    put_word(source, 20, 0x3e800000UL);
    put_word(source, 24, 0x3f200000UL);
    put_word(source, 28, 0x3f600000UL);
    qxp_vertex_copy(&vertex, NULL, source, &transformed);
    if (!require(vertex.x_bits == 0x3f000000UL &&
                 vertex.y_bits == 0x3f400000UL &&
                 vertex.z_bits == 0x3f800000UL &&
                 vertex.rhw_bits == 0x40000000UL &&
                 vertex.color == 0xffabcdefUL &&
                 vertex.specular == 0 &&
                 vertex.texcoord[0][0] == 0x3e800000UL &&
                 vertex.texcoord[0][1] == 0x3f200000UL &&
                 vertex.fog_bits == 0x3f600000UL,
                 "transformed vertex copy")) return 1;

    qxp_vertex_decode_layout(
        FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | FVF_SPECULAR |
        FVF_TEX2 | FVF_TEXCOORDSIZE3(0) | FVF_TEXCOORDSIZE1(1) |
        FVF_FOG, &object);
    memset(source, 0, sizeof(source));
    put_word(source, 0, 0x3f800000UL);
    put_word(source, 4, 0x40000000UL);
    put_word(source, 8, 0x40400000UL);
    put_word(source, 12, 0x3dcccccdUL);
    put_word(source, 16, 0x3e4ccccdUL);
    put_word(source, 20, 0x3e99999aUL);
    put_word(source, 24, 0xff123456UL);
    put_word(source, 28, 0x80102030UL);
    put_word(source, 32, 0x3f000000UL);
    put_word(source, 36, 0x3f400000UL);
    put_word(source, 40, 0x3f800000UL);
    put_word(source, 44, 0x3e800000UL);
    put_word(source, 48, 0x3f400000UL);
    qxp_vertex_copy(&vertex, &object_data, source, &object);
    if (!require(vertex.x_bits == 0x3f800000UL &&
                 vertex.y_bits == 0x40000000UL &&
                 vertex.z_bits == 0x40400000UL &&
                 vertex.rhw_bits == 0x3f800000UL,
                 "object position copy") ||
        !require(object_data.normal[0] == 0x3dcccccdUL &&
                 object_data.normal[1] == 0x3e4ccccdUL &&
                 object_data.normal[2] == 0x3e99999aUL &&
                 object_data.point_size_bits == 0x3f800000UL,
                 "object normal copy") ||
        !require(vertex.color == 0xff123456UL &&
                 vertex.specular == 0x80102030UL &&
                 vertex.fog_bits == 0x3f400000UL,
                 "object color copy") ||
        !require(vertex.texcoord[0][0] == 0x3f000000UL &&
                 vertex.texcoord[0][2] == 0x3f800000UL &&
                 vertex.texcoord[1][0] == 0x3e800000UL &&
                 vertex.texcoord[1][1] == 0,
                 "object coordinate copy")) return 1;

    put_word(source, 32, 0x7f800000UL);
    put_word(source, 36, 0xffc00001UL);
    put_word(source, 40, 0x7f7fffffUL);
    qxp_vertex_copy(&vertex, &object_data, source, &object);
    if (!require(vertex.texcoord[0][0] == 0 &&
                 vertex.texcoord[0][1] == 0 &&
                 vertex.texcoord[0][2] == 0x7f7fffffUL &&
                 vertex.texcoord[1][0] == 0x3e800000UL,
                 "non-finite coordinate canonicalization")) return 1;

    qxp_vertex_decode_layout(FVF_XYZ | FVF_PSIZE, &object);
    memset(source, 0, sizeof(source));
    put_word(source, 12, 0x40800000UL);
    qxp_vertex_copy(&vertex, &object_data, source, &object);
    if (!require(object_data.point_size_bits == 0x40800000UL,
                 "object point-size copy")) return 1;

    if (!require(qxp_vertex_packet_layout(
                     QXP_D3D_VERTEX_TRANSFORMED, 3, &object_offset,
                     &state_offset, &request_bytes) &&
                 object_offset == 0 && state_offset == 0 &&
                 request_bytes == header_bytes +
                                  3 * sizeof(QXP_D3D_VERTEX),
                 "transformed packet layout") ||
        !require(qxp_vertex_packet_layout(
                     QXP_D3D_VERTEX_OBJECT, 3, &object_offset,
                     &state_offset, &request_bytes) &&
                 object_offset == header_bytes +
                                  3 * sizeof(QXP_D3D_VERTEX) &&
                 state_offset == object_offset +
                                 3 * sizeof(QXP_D3D_OBJECT_VERTEX_DATA) &&
                 request_bytes == state_offset +
                                  sizeof(QXP_D3D_HWTL_STATE),
                 "object packet layout") ||
        !require(!qxp_vertex_packet_layout(
                     QXP_D3D_VERTEX_OBJECT, 0, &object_offset,
                     &state_offset, &request_bytes),
                 "accepted empty packet") ||
        !require(!qxp_vertex_packet_layout(
                     99, 1, &object_offset, &state_offset, &request_bytes),
                  "accepted unknown vertex mode")) return 1;

    expected_bytes = header_bytes +
        sizeof(QXP_D3D_INDEXED_VERTEX_PAYLOAD) -
        sizeof(((QXP_D3D_INDEXED_VERTEX_PAYLOAD *)0)->data) +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT +
        3 * sizeof(QXP_D3D_VERTEX);
    if (!require(qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_INDEXED_TRANSFORMED, 3, 6,
                     &object_offset, &state_offset, &request_bytes) &&
                 !object_offset && !state_offset &&
                 request_bytes == expected_bytes,
                 "transformed indexed packet layout") ||
        !require(qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_INDEXED_OBJECT, 3, 6,
                     &object_offset, &state_offset, &request_bytes) &&
                 object_offset == expected_bytes &&
                 state_offset == object_offset +
                     3 * sizeof(QXP_D3D_OBJECT_VERTEX_DATA) &&
                 request_bytes == state_offset +
                     sizeof(QXP_D3D_HWTL_STATE),
                 "object indexed packet layout") ||
        !require(!qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_INDEXED_TRANSFORMED, 4, 3,
                     &object_offset, &state_offset, &request_bytes),
                 "accepted unused indexed vertex") ||
        !require(!qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_INDEXED_TRANSFORMED, 0, 3,
                     &object_offset, &state_offset, &request_bytes),
                 "accepted empty indexed vertex stream") ||
        !require(!qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_INDEXED_TRANSFORMED, 3, 0,
                     &object_offset, &state_offset, &request_bytes),
                 "accepted empty canonical index stream") ||
        !require(!qxp_vertex_indexed_packet_layout(
                     QXP_D3D_VERTEX_OBJECT, 3, 6,
                     &object_offset, &state_offset, &request_bytes),
                 "accepted nonindexed canonical mode"))
        return 1;

    qxp_vertex_decode_layout(
        FVF_XYZ | FVF_DIFFUSE | FVF_TEX1, &object);
    memset(source, 0, sizeof(source));
    expected_bytes = header_bytes +
        sizeof(QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD) -
        sizeof(((QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *)0)->data) +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT +
        2 * object.vertex_bytes;
    if (!require(qxp_vertex_raw_indexed_object_supported(&object),
                 "rejected raw prelit object layout") ||
        !require(qxp_vertex_raw_indexed_object_source_valid(
                     source, &object),
                 "rejected finite raw prelit source") ||
        !require(qxp_vertex_raw_indexed_object_packet_layout(
                     object.vertex_bytes, 2, 3,
                     &state_offset, &request_bytes),
                 "rejected raw indexed object packet layout") ||
        !require(state_offset == expected_bytes &&
                 request_bytes == state_offset +
                     sizeof(QXP_D3D_HWTL_STATE),
                 "wrong raw indexed object packet bytes") ||
        !require(!qxp_vertex_raw_indexed_object_packet_layout(
                     object.vertex_bytes, 4, 3,
                     &state_offset, &request_bytes),
                 "accepted unused raw object vertex") ||
        !require(!qxp_vertex_raw_indexed_object_packet_layout(
                     0, 2, 3, &state_offset, &request_bytes),
                 "accepted zero raw object stride"))
        return 1;
    put_word(source, object.texcoord_offset[0], 0x7f800000UL);
    if (!require(!qxp_vertex_raw_indexed_object_source_valid(
                     source, &object),
                 "accepted nonfinite raw texture coordinate"))
        return 1;
    put_word(source, object.texcoord_offset[0], 0);
    qxp_vertex_decode_layout(
        FVF_XYZ | FVF_NORMAL | FVF_DIFFUSE | FVF_TEX1, &object);
    put_word(source, object.normal_offset, 0x7f800000UL);
    if (!require(qxp_vertex_raw_indexed_object_supported(&object),
                 "rejected raw normal prelit object layout") ||
        !require(!qxp_vertex_raw_indexed_object_source_valid(
                     source, &object),
                 "accepted nonfinite raw normal"))
        return 1;
    qxp_vertex_decode_layout(
        FVF_XYZ | FVF_NORMAL | FVF_TEX1, &object);
    memset(source, 0, sizeof(source));
    if (!require(qxp_vertex_raw_indexed_object_supported(&object),
                 "rejected raw uncolored normal prelit object layout") ||
        !require(qxp_vertex_raw_indexed_object_source_valid(
                     source, &object),
                 "rejected raw uncolored normal prelit source"))
        return 1;
    qxp_vertex_decode_layout(
        FVF_XYZ | FVF_PSIZE | FVF_DIFFUSE | FVF_TEX1, &object);
    if (!require(!qxp_vertex_raw_indexed_object_supported(&object),
                 "accepted raw point-size layout"))
        return 1;

    qxp_vertex_decode_layout(FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1,
                             &transformed);
    memset(source, 0, sizeof(source));
    put_word(source, 12, 0x3f800000UL);
    if (!require(qxp_vertex_compact_supported(&transformed),
                 "rejected compact transformed layout") ||
        !require(qxp_vertex_compact_source_valid(source, &transformed),
                 "rejected finite compact source") ||
        !require(qxp_vertex_compact_packet_layout(
                     transformed.vertex_bytes, 3, &request_bytes),
                 "rejected compact packet layout") ||
        !require(request_bytes == header_bytes +
                     sizeof(QXP_D3D_COMPACT_VERTEX_PAYLOAD) -
                         sizeof(((QXP_D3D_COMPACT_VERTEX_PAYLOAD *)0)
                                    ->vertex_data) +
                     transformed.vertex_bytes * 3,
                 "wrong compact packet bytes") ||
        !require(qxp_vertex_compact_indexed_packet_layout(
                     transformed.vertex_bytes, 2, 3, &request_bytes),
                 "rejected compact indexed packet layout") ||
        !require(request_bytes == header_bytes +
                     sizeof(QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD) -
                         sizeof(((QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)0)
                                    ->data) +
                     transformed.vertex_bytes * 2 +
                         QXP_D3D_COMPACT_INDEX_ALIGNMENT,
                 "wrong compact indexed packet bytes") ||
        !require(!qxp_vertex_compact_indexed_packet_layout(
                     transformed.vertex_bytes, 4, 3, &request_bytes),
                 "accepted more compact vertices than indices") ||
        !require(!qxp_vertex_compact_indexed_packet_layout(
                     transformed.vertex_bytes, 2, 0, &request_bytes),
                 "accepted empty compact index stream")) return 1;
    for (index_count = 1; index_count <= 8; ++index_count) {
        expected_bytes = header_bytes +
            sizeof(QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD) -
            sizeof(((QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)0)->data) +
            ((index_count + QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
             ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL)) + 32;
        if (!require(qxp_vertex_compact_indexed_packet_layout(
                         32, 1, index_count, &request_bytes) &&
                     request_bytes == expected_bytes,
                     "wrong compact indexed alignment")) return 1;
    }
    qxp_vertex_compact_index_map_reset(map_vertices);
    unique_count = 0;
    if (!require(qxp_vertex_compact_index_map(
                     7, &unique_count, TRUE, &mapped_index,
                     map_vertices, map_indices) &&
                 mapped_index == 0 && unique_count == 1,
                 "compact map rejected first vertex") ||
        !require(qxp_vertex_compact_index_map(
                     1000, &unique_count, TRUE, &mapped_index,
                     map_vertices, map_indices) &&
                 mapped_index == 1 && unique_count == 2,
                 "compact map rejected second vertex") ||
        !require(qxp_vertex_compact_index_map(
                     7, &unique_count, FALSE, &mapped_index,
                     map_vertices, map_indices) &&
                 mapped_index == 0 && unique_count == 2,
                 "compact map missed first duplicate") ||
        !require(qxp_vertex_compact_index_map(
                     65536, &unique_count, TRUE, &mapped_index,
                     map_vertices, map_indices) &&
                 mapped_index == 2 && unique_count == 3,
                 "compact map rejected large vertex") ||
        !require(qxp_vertex_compact_index_map(
                     1000, &unique_count, FALSE, &mapped_index,
                     map_vertices, map_indices) &&
                 mapped_index == 1 && unique_count == 3,
                 "compact map missed large duplicate") ||
        !require(!qxp_vertex_compact_index_map(
                      42, &unique_count, FALSE, &mapped_index,
                      map_vertices, map_indices),
                 "compact map added lookup-only vertex") ||
        !require(!qxp_vertex_compact_index_map(
                      QXP_VERTEX_COMPACT_MAP_EMPTY, &unique_count, TRUE,
                      &mapped_index, map_vertices, map_indices),
                 "compact map accepted sentinel vertex")) return 1;
    qxp_vertex_compact_index_map_reset(map_vertices);
    unique_count = 0;
    for (map_test_index = 0;
         map_test_index < QXP_D3D_MAX_DRAW_VERTICES;
         ++map_test_index) {
        if (!require(qxp_vertex_compact_index_map(
                         17 + map_test_index * 7919, &unique_count, TRUE,
                         &mapped_index, map_vertices, map_indices) &&
                     mapped_index == map_test_index,
                     "compact map rejected bounded unique vertex"))
            return 1;
    }
    if (!require(unique_count == QXP_D3D_MAX_DRAW_VERTICES &&
                 !qxp_vertex_compact_index_map(
                     2000000, &unique_count, TRUE, &mapped_index,
                     map_vertices, map_indices),
                 "compact map exceeded unique vertex bound")) return 1;
    for (map_test_index = 0;
         map_test_index < QXP_D3D_MAX_DRAW_VERTICES;
         ++map_test_index) {
        if (!require(qxp_vertex_compact_index_map(
                         17 + map_test_index * 7919, &unique_count, FALSE,
                         &mapped_index, map_vertices, map_indices) &&
                     mapped_index == map_test_index,
                     "compact map missed bounded unique vertex"))
            return 1;
    }
    /* A nine-index preflight reserves 16 index bytes. Simulate its first
     * primitive being skipped after writing vertices 0 and 1: the six
     * surviving indices reference old slots 2-4 and shrink alignment to 8. */
    if (!require(qxp_vertex_compact_indexed_packet_layout(
                     4, 5, 9, &preflight_request_bytes) &&
                 preflight_request_bytes == header_bytes +
                     sizeof(QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD) -
                     sizeof(((QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)0)
                                ->data) + 16 + 20,
                 "wrong skipped-primitive preflight alignment")) return 1;
    put_word(dense_vertices, 0, 0x11111111UL);
    put_word(dense_vertices, 4, 0x22222222UL);
    put_word(dense_vertices, 8, 0x33333333UL);
    put_word(dense_vertices, 12, 0x44444444UL);
    put_word(dense_vertices, 16, 0x55555555UL);
    put_word(expected_vertices, 0, 0x33333333UL);
    put_word(expected_vertices, 4, 0x44444444UL);
    put_word(expected_vertices, 8, 0x55555555UL);
    if (!require(qxp_vertex_compact_indexed_densify(
                     dense_indices, 6, dense_vertices, 4, 5,
                     dense_written, &dense_vertex_count),
                 "rejected skipped-primitive compact densify") ||
        !require(dense_vertex_count == 3 &&
                 memcmp(dense_indices, expected_indices,
                        sizeof(dense_indices)) == 0 &&
                 memcmp(dense_vertices, expected_vertices,
                        sizeof(expected_vertices)) == 0,
                 "wrong skipped-primitive dense packet") ||
        !require(qxp_vertex_compact_indexed_packet_layout(
                     4, dense_vertex_count, 6, &request_bytes) &&
                 request_bytes == header_bytes +
                     sizeof(QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD) -
                     sizeof(((QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)0)
                                ->data) +
                     QXP_D3D_COMPACT_INDEX_ALIGNMENT + 12,
                 "wrong alignment-shrunk dense packet")) return 1;
    put_word(source, 12, 0x7f800000UL);
    if (!require(!qxp_vertex_compact_source_valid(source, &transformed),
                 "accepted nonfinite compact RHW")) return 1;
    qxp_vertex_decode_layout(FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1 |
                             FVF_TEXCOORDSIZE3(0), &transformed);
    if (!require(!qxp_vertex_compact_supported(&transformed),
                 "accepted compact 3D texture coordinate")) return 1;

    if (!require(qxp_vertex_ring_wrap_required(
                     192, 32, 6, 32, 320),
                 "rejected runtime user-vertex ring wrap") ||
        !require(!qxp_vertex_ring_wrap_required(
                     128, 32, 6, 32, 320),
                 "reported wrap for an exact in-range span") ||
        !require(qxp_vertex_ring_wrap_required(
                     320, 32, 1, 32, 320),
                 "rejected a rolling ring offset at the buffer end") ||
        !require(!qxp_vertex_ring_wrap_required(
                     0, 16, 2, 32, 320),
                 "accepted an element wider than its stride") ||
        !require(!qxp_vertex_ring_wrap_required(
                     0xfffffff0UL, 32, 2, 16, 320),
                 "accepted a wrapping 32-bit source span")) return 1;

    puts("qxp_vertex_test: ok");
    return 0;
}
