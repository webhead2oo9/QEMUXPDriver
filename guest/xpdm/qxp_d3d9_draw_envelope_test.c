#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <windef.h>

#include "qxpdisp/qxp_d3d9_draw_envelope.h"

typedef struct qxp_test_points {
    WORD count;
    WORD start;
} QXP_TEST_POINTS;

typedef struct qxp_test_draw {
    DWORD primitive_type;
    DWORD vertex_start;
    DWORD primitive_count;
} QXP_TEST_DRAW;

typedef struct qxp_test_indexed {
    DWORD primitive_type;
    LONG base_vertex_index;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index;
    DWORD primitive_count;
} QXP_TEST_INDEXED;

typedef struct qxp_test_fan {
    DWORD first_vertex_offset;
    DWORD edge_flags;
    DWORD primitive_count;
} QXP_TEST_FAN;

typedef struct qxp_test_draw2 {
    DWORD primitive_type;
    DWORD first_vertex_offset;
    DWORD primitive_count;
} QXP_TEST_DRAW2;

typedef struct qxp_test_indexed2 {
    DWORD primitive_type;
    LONG base_vertex_offset;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index_offset;
    DWORD primitive_count;
} QXP_TEST_INDEXED2;

static QXP_D3D9_DRAW_PREPARED qxp_prepared;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_backings;
static QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT qxp_out;
static QXP_D3D9_DRAW_ENVELOPE_STATUS qxp_status;
static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_envelope: failed: %s\n", name);
}

static void qxp_set_linear_kind(
    QXP_D3D9_DRAW_BACKING_RECORD *record, DWORD kind, DWORD seed)
{
    record->kind = kind;
    record->incarnation = (ULONGLONG)(seed + 1UL);
    record->content_generation = seed + 3UL;
    record->byte_length = 0x100000UL;
    record->flags = 0;
    record->video_memory_offset =
        kind == QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR ?
            0x1000UL + seed * 0x100UL : 0;
}

static void qxp_reset_fixture(DWORD stream_mask, BOOL indexed)
{
    DWORD stream;

    memset(&qxp_prepared, 0, sizeof(qxp_prepared));
    memset(&qxp_backings, 0, sizeof(qxp_backings));
    memset(&qxp_out, 0, sizeof(qxp_out));
    memset(&qxp_status, 0, sizeof(qxp_status));

    qxp_prepared.valid = TRUE;
    qxp_prepared.mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    qxp_prepared.owner.context_handle = 0x41UL;
    qxp_prepared.owner.process_id = 0x51UL;
    qxp_prepared.owner.device_identity = 0x1234UL;
    qxp_prepared.required_stream_mask = stream_mask;
    qxp_prepared.raw_state.stream_source_generation = 9;

    /* Byte-offset records require canonical raw stream-0 stride even when
     * stream 0 is not shader-required. */
    qxp_prepared.raw_state.streams[0].stride = 16;
    qxp_prepared.raw_state.streams[0].divider = 1;
    qxp_prepared.raw_state.streams[0].source2 = TRUE;

    qxp_backings.valid = TRUE;
    qxp_backings.owner.shader_owner = qxp_prepared.owner;
    qxp_backings.owner.dd_local_identity = 0xabcUL;
    qxp_backings.stream_source_generation = 9;
    qxp_backings.required_stream_mask = stream_mask;
    qxp_backings.populated_stream_mask = stream_mask;
    qxp_backings.indexed = indexed;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
         ++stream) {
        QXP_D3D9_DRAW_STREAM_STATE *source;
        QXP_D3D9_DRAW_BACKING_RECORD *record;
        DWORD stride;
        DWORD span;
        DWORD offset;

        if (!(stream_mask & (1UL << stream))) continue;
        source = &qxp_prepared.raw_state.streams[stream];
        record = &qxp_backings.streams[stream];
        stride = 16UL + stream * 4UL;
        span = 12UL + (stream & 3UL) * 4UL;
        offset = stream * 7UL;
        source->handle = 0x100UL + stream;
        source->offset = offset;
        source->stride = stride;
        source->divider = 1;
        source->user_memory = FALSE;
        source->source2 = TRUE;
        qxp_prepared.required_stream_spans[stream] = span;

        qxp_set_linear_kind(
            record,
            (stream & 1UL) ?
                QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR :
                QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR,
            stream + 1UL);
        record->role = QXP_D3D9_DRAW_BACKING_ROLE_VERTEX;
        record->stream = stream;
        record->handle = source->handle;
        record->stride = stride;
        record->source2_offset = offset;
        record->divider = 1;
        record->required_span = span;
    }

    if (indexed) {
        QXP_D3D9_DRAW_BACKING_RECORD *record;

        qxp_prepared.raw_state.indices.handle = 0x700UL;
        qxp_prepared.raw_state.indices.stride = 2;
        record = &qxp_backings.index;
        qxp_set_linear_kind(
            record, QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR, 30UL);
        record->role = QXP_D3D9_DRAW_BACKING_ROLE_INDEX;
        record->stream = 0;
        record->handle = 0x700UL;
        record->stride = 2;
        record->required_span = 2;
    }
}

static void qxp_set_index_stride(DWORD stride)
{
    qxp_prepared.raw_state.indices.stride = stride;
    qxp_backings.index.stride = stride;
    qxp_backings.index.required_span = stride;
}

static void qxp_set_user_memory_stream0(DWORD byte_length)
{
    QXP_D3D9_DRAW_STREAM_STATE *source;
    QXP_D3D9_DRAW_BACKING_RECORD *record;

    qxp_reset_fixture(1UL, FALSE);
    source = &qxp_prepared.raw_state.streams[0];
    record = &qxp_backings.streams[0];
    qxp_prepared.required_user_memory_stream_mask = 1UL;
    qxp_backings.required_user_memory_stream_mask = 1UL;
    source->handle = 0;
    source->offset = 0;
    source->user_memory = TRUE;
    source->source2 = FALSE;
    record->kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    record->handle = 0;
    record->incarnation = 0;
    record->content_generation = 0;
    record->byte_length = byte_length;
    record->video_memory_offset = 0;
}

static void qxp_set_circular_stream0(
    DWORD byte_length, DWORD stride, DWORD span)
{
    QXP_D3D9_DRAW_STREAM_STATE *source;
    QXP_D3D9_DRAW_BACKING_RECORD *record;

    qxp_reset_fixture(1UL, FALSE);
    source = &qxp_prepared.raw_state.streams[0];
    record = &qxp_backings.streams[0];
    source->offset = 0;
    source->stride = stride;
    source->source2 = FALSE;
    qxp_prepared.required_stream_spans[0] = span;
    record->kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    record->byte_length = byte_length;
    record->video_memory_offset = 0;
    record->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    record->stride = stride;
    record->source2_offset = 0;
    record->required_span = span;
}

static BOOL qxp_build(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes)
{
    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes,
        &qxp_prepared, &qxp_backings, &qxp_out, &qxp_status);
}

static int qxp_out_zero(void)
{
    const BYTE *bytes;
    DWORD index;

    bytes = (const BYTE *)&qxp_out;
    for (index = 0; index < (DWORD)sizeof(qxp_out); ++index) {
        if (bytes[index]) return 0;
    }
    return 1;
}

static void qxp_expect_failure(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, DWORD code, const char *name)
{
    BOOL result;

    memset(&qxp_out, 0xcc, sizeof(qxp_out));
    memset(&qxp_status, 0xcc, sizeof(qxp_status));
    result = qxp_build(opcode, record_count, records, available_bytes);
    qxp_check(!result && qxp_status.code == code && qxp_out_zero(), name);
}

static void qxp_test_layouts(void)
{
    qxp_check(sizeof(QXP_TEST_POINTS) == 4 &&
                  offsetof(QXP_TEST_POINTS, count) == 0 &&
                  offsetof(QXP_TEST_POINTS, start) == 2 &&
                  QXP_D3D9_DRAW_RECORD_BYTES_POINTS == 4,
              "POINTS exact x86 layout");
    qxp_check(sizeof(QXP_TEST_DRAW) == 12 &&
                  offsetof(QXP_TEST_DRAW, primitive_type) == 0 &&
                  offsetof(QXP_TEST_DRAW, vertex_start) == 4 &&
                  offsetof(QXP_TEST_DRAW, primitive_count) == 8,
              "DRAWPRIMITIVE exact x86 layout");
    qxp_check(sizeof(QXP_TEST_INDEXED) == 24 &&
                  offsetof(QXP_TEST_INDEXED, base_vertex_index) == 4 &&
                  offsetof(QXP_TEST_INDEXED, min_index) == 8 &&
                  offsetof(QXP_TEST_INDEXED, num_vertices) == 12 &&
                  offsetof(QXP_TEST_INDEXED, start_index) == 16 &&
                  offsetof(QXP_TEST_INDEXED, primitive_count) == 20,
              "DRAWINDEXEDPRIMITIVE exact x86 layout");
    qxp_check(sizeof(QXP_TEST_FAN) == 12 &&
                  offsetof(QXP_TEST_FAN, first_vertex_offset) == 0 &&
                  offsetof(QXP_TEST_FAN, edge_flags) == 4 &&
                  offsetof(QXP_TEST_FAN, primitive_count) == 8,
              "CLIPPEDTRIANGLEFAN exact x86 layout");
    qxp_check(sizeof(QXP_TEST_DRAW2) == 12 &&
                  offsetof(QXP_TEST_DRAW2, primitive_type) == 0 &&
                  offsetof(QXP_TEST_DRAW2, first_vertex_offset) == 4 &&
                  offsetof(QXP_TEST_DRAW2, primitive_count) == 8,
              "DRAWPRIMITIVE2 exact x86 layout");
    qxp_check(sizeof(QXP_TEST_INDEXED2) == 24 &&
                  offsetof(QXP_TEST_INDEXED2, base_vertex_offset) == 4 &&
                  offsetof(QXP_TEST_INDEXED2, min_index) == 8 &&
                  offsetof(QXP_TEST_INDEXED2, num_vertices) == 12 &&
                  offsetof(QXP_TEST_INDEXED2, start_index_offset) == 16 &&
                  offsetof(QXP_TEST_INDEXED2, primitive_count) == 20,
              "DRAWINDEXEDPRIMITIVE2 exact x86 layout");
    qxp_check(QXP_D3D9_PRIMITIVE_POINTLIST == 1 &&
                  QXP_D3D9_PRIMITIVE_LINELIST == 2 &&
                  QXP_D3D9_PRIMITIVE_LINESTRIP == 3 &&
                  QXP_D3D9_PRIMITIVE_TRIANGLELIST == 4 &&
                  QXP_D3D9_PRIMITIVE_TRIANGLESTRIP == 5 &&
                  QXP_D3D9_PRIMITIVE_TRIANGLEFAN == 6,
              "primitive values one through six exact");
}

static void qxp_test_six_opcodes_and_primitive_types(void)
{
    static const DWORD types[6] = {1, 2, 3, 4, 5, 6};
    static const DWORD counts[6] = {2, 4, 3, 6, 4, 4};
    QXP_TEST_POINTS points;
    QXP_TEST_FAN fan;
    QXP_TEST_DRAW draw;
    QXP_TEST_INDEXED indexed;
    QXP_TEST_DRAW2 draw2;
    QXP_TEST_INDEXED2 indexed2;
    DWORD index;

    qxp_reset_fixture(1UL, FALSE);
    points.count = 2;
    points.start = 3;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points)) &&
                  qxp_out.nonempty_record_count == 1 &&
                  qxp_out.streams[0].logical_first == 3 &&
                  qxp_out.streams[0].logical_last_exclusive == 5 &&
                  qxp_out.streams[0].byte_first == 48 &&
                  qxp_out.streams[0].byte_last_exclusive == 76,
              "POINTS uses WORD count and direct vertex start");

    qxp_reset_fixture(1UL, FALSE);
    fan.first_vertex_offset = 32;
    fan.edge_flags = 0xa5a5UL;
    fan.primitive_count = 2;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1,
                  &fan, sizeof(fan)) &&
                  qxp_out.streams[0].logical_first == 2 &&
                  qxp_out.streams[0].logical_last_exclusive == 6,
              "clipped fan converts byte start through stream0 stride");

    for (index = 0; index < 6; ++index) {
        qxp_reset_fixture(1UL, FALSE);
        draw.primitive_type = types[index];
        draw.vertex_start = 4;
        draw.primitive_count = 2;
        qxp_check(qxp_build(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1,
                      &draw, sizeof(draw)) &&
                      qxp_out.streams[0].logical_first == 4 &&
                      qxp_out.streams[0].logical_last_exclusive ==
                          4UL + counts[index],
                  "legacy nonindexed primitive source count");

        qxp_reset_fixture(1UL, FALSE);
        draw2.primitive_type = types[index];
        draw2.first_vertex_offset = 64;
        draw2.primitive_count = 2;
        qxp_check(qxp_build(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
                      &draw2, sizeof(draw2)) &&
                      qxp_out.streams[0].logical_first == 4 &&
                      qxp_out.streams[0].logical_last_exclusive ==
                          4UL + counts[index],
                  "DRAWPRIMITIVE2 primitive source count and byte start");

        qxp_reset_fixture(1UL, TRUE);
        indexed.primitive_type = types[index];
        indexed.base_vertex_index = 1;
        indexed.min_index = 2;
        indexed.num_vertices = 10;
        indexed.start_index = 3;
        indexed.primitive_count = 2;
        qxp_check(qxp_build(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                      &indexed, sizeof(indexed)) &&
                      qxp_out.indexed &&
                      qxp_out.streams[0].logical_first == 3 &&
                      qxp_out.streams[0].logical_last_exclusive == 13 &&
                      qxp_out.index.byte_first == 6 &&
                      qxp_out.index.byte_last_exclusive ==
                          6UL + counts[index] * 2UL,
                  "legacy indexed declared window and index count");

        qxp_reset_fixture(1UL, TRUE);
        qxp_set_index_stride(4);
        indexed2.primitive_type = types[index];
        indexed2.base_vertex_offset = 16;
        indexed2.min_index = 2;
        indexed2.num_vertices = 10;
        indexed2.start_index_offset = 3;
        indexed2.primitive_count = 2;
        qxp_check(qxp_build(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                      &indexed2, sizeof(indexed2)) &&
                      qxp_out.streams[0].logical_first == 3 &&
                      qxp_out.streams[0].logical_last_exclusive == 13 &&
                      qxp_out.index.byte_first == 3 &&
                      qxp_out.index.byte_last_exclusive ==
                          3UL + counts[index] * 4UL,
                  "indexed2 byte offsets and all primitive source counts");
    }
}

static void qxp_test_zero_multiple_streams_and_kinds(void)
{
    QXP_TEST_POINTS points[2];
    QXP_TEST_DRAW draw;
    DWORD stream;

    qxp_reset_fixture(1UL, FALSE);
    points[0].count = 0;
    points[0].start = 0xffffU;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1,
                  points, sizeof(points[0])) &&
                  qxp_out.record_count == 1 &&
                  qxp_out.nonempty_record_count == 0 &&
                  !qxp_out.streams[0].valid && !qxp_out.index.valid,
              "zero POINTS record is framed no-access record");

    qxp_reset_fixture(1UL, FALSE);
    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.vertex_start = 0xffffffffUL;
    draw.primitive_count = 0;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1,
                  &draw, sizeof(draw)) &&
                  qxp_out.nonempty_record_count == 0,
              "zero PrimitiveCount ignores access scalars");

    qxp_reset_fixture(1UL, TRUE);
    {
        QXP_TEST_INDEXED empty_indexed;

        empty_indexed.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
        empty_indexed.base_vertex_index = (LONG)0x80000000UL;
        empty_indexed.min_index = 0xffffffffUL;
        empty_indexed.num_vertices = 0;
        empty_indexed.start_index = 0xffffffffUL;
        empty_indexed.primitive_count = 0;
        qxp_check(qxp_build(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                      &empty_indexed, sizeof(empty_indexed)) &&
                      qxp_out.record_count == 1 &&
                      qxp_out.nonempty_record_count == 0 &&
                      !qxp_out.streams[0].valid && !qxp_out.index.valid,
                  "zero indexed primitive is framed without access ranges");
    }

    qxp_reset_fixture(1UL, FALSE);
    points[0].count = 2;
    points[0].start = 5;
    points[1].count = 3;
    points[1].start = 1;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 2, points, sizeof(points)) &&
                  qxp_out.record_count == 2 &&
                  qxp_out.nonempty_record_count == 2 &&
                  qxp_out.streams[0].logical_first == 1 &&
                  qxp_out.streams[0].logical_last_exclusive == 7 &&
                  qxp_out.streams[0].byte_first == 16 &&
                  qxp_out.streams[0].byte_last_exclusive == 108,
              "multiple records aggregate min first and max exclusive");

    qxp_reset_fixture(0xffffUL, FALSE);
    points[0].count = 3;
    points[0].start = 2;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1,
                  points, sizeof(points[0])) &&
                  qxp_out.stream_mask == 0xffffUL,
              "all sixteen required streams accepted");
    for (stream = 0; stream < 16; ++stream) {
        DWORD stride;
        DWORD span;
        DWORD offset;

        stride = 16UL + stream * 4UL;
        span = 12UL + (stream & 3UL) * 4UL;
        offset = stream * 7UL;
        qxp_check(qxp_out.streams[stream].valid &&
                      qxp_out.streams[stream].stream == stream &&
                      qxp_out.streams[stream].logical_first == 2 &&
                      qxp_out.streams[stream].logical_last_exclusive == 5 &&
                      qxp_out.streams[stream].byte_first ==
                          offset + 2UL * stride &&
                      qxp_out.streams[stream].byte_last_exclusive ==
                          offset + 4UL * stride + span,
                  "per-stream stride span and SOURCE2 footprint exact");
    }

    qxp_set_user_memory_stream0(76);
    points[0].count = 2;
    points[0].start = 3;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1,
                  points, sizeof(points[0])) &&
                  qxp_out.streams[0].byte_last_exclusive == 76,
              "finite user-memory call extent exact end accepted");
    qxp_backings.streams[0].byte_length = 75;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, points, sizeof(points[0]),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
        "finite user-memory call short end rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    qxp_backings.streams[0].video_memory_offset = 0x2000UL;
    points[0].count = 1;
    points[0].start = 0;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1,
                  points, sizeof(points[0])) && qxp_out.valid,
              "finite video-linear extent accepted");
}

static void qxp_test_circular_execute_ring(void)
{
    QXP_TEST_DRAW draw;

    qxp_set_circular_stream0(0x140UL, 0x20UL, 0x20UL);
    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.vertex_start = 0x36UL;
    draw.primitive_count = 8UL;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw)) &&
                  qxp_out.streams[0].logical_first == 0x36UL &&
                  qxp_out.streams[0].logical_last_exclusive == 0x4eUL &&
                  qxp_out.streams[0].byte_first == 0x80UL &&
                  qxp_out.streams[0].byte_last_exclusive == 0x380UL,
              "A8 execute ring unrolls three wrapped source spans");

    qxp_backings.streams[0].flags = 0;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
        "A8 logical range remains invalid for a finite linear backing");

    qxp_set_circular_stream0(0x140UL, 0x20UL, 0x20UL);
    qxp_prepared.raw_state.streams[0].source2 = TRUE;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "SOURCE2 cannot claim legacy execute-ring semantics");
}

static void qxp_test_byte_start_alignment(void)
{
    QXP_TEST_DRAW2 draw2;
    QXP_TEST_DRAW2 phase_records[2];
    QXP_TEST_FAN fan;

    /* The stock D3D9 runtime suballocates DrawPrimitiveUP vertices inside a
     * shared stream buffer.  FirstVertexOffset is a byte address in that
     * buffer and its phase need not be zero modulo the declaration stride. */
    qxp_reset_fixture(1UL, FALSE);
    qxp_prepared.raw_state.streams[0].stride = 28;
    qxp_prepared.required_stream_spans[0] = 28;
    qxp_backings.streams[0].stride = 28;
    qxp_backings.streams[0].required_span = 28;
    draw2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw2.first_vertex_offset = 584;
    draw2.primitive_count = 1;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
                  &draw2, sizeof(draw2)) &&
                  qxp_out.streams[0].logical_first == 20 &&
                  qxp_out.streams[0].logical_last_exclusive == 23 &&
                  qxp_out.streams[0].byte_first == 584 &&
                  qxp_out.streams[0].byte_last_exclusive == 668,
              "single-stream draw2 preserves arbitrary byte phase");

    phase_records[0] = draw2;
    phase_records[1] = draw2;
    phase_records[1].first_vertex_offset = 668;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 2,
                  phase_records, sizeof(phase_records)) &&
                  qxp_out.streams[0].logical_first == 20 &&
                  qxp_out.streams[0].logical_last_exclusive == 26 &&
                  qxp_out.streams[0].byte_first == 584 &&
                  qxp_out.streams[0].byte_last_exclusive == 752,
              "single-stream draw2 batch preserves one byte phase");

    phase_records[1].first_vertex_offset = 669;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 2,
        phase_records, sizeof(phase_records),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT,
        "single-stream draw2 mixed byte phases rejected");

    qxp_reset_fixture(2UL, FALSE);
    qxp_prepared.raw_state.streams[0].stride = 8;
    draw2.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw2.first_vertex_offset = 16;
    draw2.primitive_count = 1;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
                  &draw2, sizeof(draw2)) &&
                  !qxp_out.streams[0].valid &&
                  qxp_out.streams[1].logical_first == 2,
              "stream0 converts byte offset while not shader-required");

    draw2.first_vertex_offset = 17;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw2, sizeof(draw2),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT,
        "multistream DRAWPRIMITIVE2 misaligned byte offset rejected");

    fan.first_vertex_offset = 18;
    fan.edge_flags = 0;
    fan.primitive_count = 1;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1, &fan, sizeof(fan),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT,
        "multistream clipped fan misaligned byte offset rejected");

    qxp_prepared.raw_state.streams[0].stride = 0;
    draw2.first_vertex_offset = 0;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw2, sizeof(draw2),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_BASE,
        "byte offset requires nonzero canonical raw stream0 stride");
}

static void qxp_test_indexed_windows(void)
{
    QXP_TEST_INDEXED records[2];
    QXP_TEST_INDEXED2 record2;

    qxp_reset_fixture(1UL, TRUE);
    records[0].primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    records[0].base_vertex_index = -4;
    records[0].min_index = 6;
    records[0].num_vertices = 5;
    records[0].start_index = 7;
    records[0].primitive_count = 1;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  records, sizeof(records[0])) &&
                  qxp_out.streams[0].logical_first == 2 &&
                  qxp_out.streams[0].logical_last_exclusive == 7 &&
                  qxp_out.index.byte_first == 14 &&
                  qxp_out.index.byte_last_exclusive == 20,
              "legacy signed negative base plus declared window accepted");

    records[0].base_vertex_index = -7;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
        records, sizeof(records[0]),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW,
        "legacy signed base yielding negative logical first rejected");

    qxp_reset_fixture(1UL, TRUE);
    qxp_set_index_stride(4);
    record2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLESTRIP;
    record2.base_vertex_offset = -16;
    record2.min_index = 2;
    record2.num_vertices = 4;
    record2.start_index_offset = 1;
    record2.primitive_count = 2;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record2, sizeof(record2)) &&
                  qxp_out.streams[0].logical_first == 1 &&
                  qxp_out.streams[0].logical_last_exclusive == 5 &&
                  qxp_out.index.byte_first == 1 &&
                  qxp_out.index.byte_last_exclusive == 17,
              "indexed2 negative divisible base and unaligned index byte start");

    record2.base_vertex_offset = 17;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record2, sizeof(record2)) &&
                  qxp_out.streams[0].logical_first == 3 &&
                  qxp_out.streams[0].logical_last_exclusive == 7 &&
                  qxp_out.streams[0].byte_first == 49 &&
                  qxp_out.streams[0].byte_last_exclusive == 109,
              "r533 indexed2 unaligned signed base floors with byte phase");

    qxp_reset_fixture(1UL, TRUE);
    records[0].primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    records[0].base_vertex_index = 10;
    records[0].min_index = 3;
    records[0].num_vertices = 4;
    records[0].start_index = 8;
    records[0].primitive_count = 2;
    records[1] = records[0];
    records[1].base_vertex_index = 1;
    records[1].min_index = 1;
    records[1].num_vertices = 3;
    records[1].start_index = 2;
    records[1].primitive_count = 3;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2,
                  records, sizeof(records)) &&
                  qxp_out.nonempty_record_count == 2 &&
                  qxp_out.streams[0].logical_first == 2 &&
                  qxp_out.streams[0].logical_last_exclusive == 17 &&
                  qxp_out.index.byte_first == 4 &&
                  qxp_out.index.byte_last_exclusive == 20,
              "multiple indexed records aggregate declared and index windows");
}

static void qxp_test_rejections_and_overflow(void)
{
    QXP_TEST_POINTS points;
    QXP_TEST_DRAW draw;
    QXP_TEST_INDEXED indexed;
    QXP_TEST_INDEXED2 indexed2;

    qxp_reset_fixture(1UL, FALSE);
    points.count = 1;
    points.start = 0;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 2, &points,
        QXP_D3D9_DRAW_RECORD_BYTES_POINTS * 2UL - 1UL,
        QXP_D3D9_DRAW_ENVELOPE_ERROR_TRUNCATION,
        "exact fixed framing truncation rejected");
    qxp_expect_failure(
        55, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_OPCODE,
        "unknown opcode rejected");

    draw.vertex_start = 0;
    draw.primitive_count = 1;
    draw.primitive_type = 7;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_TYPE,
        "unknown primitive type rejected");

    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw.primitive_count = 0x80000000UL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW,
        "line-list primitive multiplication overflow rejected");
    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINESTRIP;
    draw.primitive_count = 0xffffffffUL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW,
        "line-strip primitive addition overflow rejected");
    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.primitive_count = 0x55555556UL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW,
        "triangle-list primitive multiplication overflow rejected");
    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLEFAN;
    draw.primitive_count = 0xfffffffeUL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW,
        "strip/fan primitive addition overflow rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_prepared.raw_state.streams[0].stride = 0xffffffffUL;
    qxp_prepared.required_stream_spans[0] = 1;
    qxp_backings.streams[0].stride = 0xffffffffUL;
    qxp_backings.streams[0].required_span = 1;
    qxp_backings.streams[0].byte_length = 0xfffffffeUL;
    draw.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    draw.vertex_start = 0xffffffffUL;
    draw.primitive_count = 0xffffffffUL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, sizeof(draw),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
        "checked logical and 64-bit stream byte arithmetic overflow rejected");

    qxp_reset_fixture(1UL, FALSE);
    points.count = 2;
    points.start = 3;
    qxp_backings.streams[0].byte_length = 76;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points)),
              "stream exact exclusive end accepted");
    qxp_backings.streams[0].byte_length = 75;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
        "stream exact exclusive end short by one rejected");

    qxp_reset_fixture(1UL, TRUE);
    indexed.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    indexed.base_vertex_index = 0;
    indexed.min_index = 0;
    indexed.num_vertices = 0;
    indexed.start_index = 0;
    indexed.primitive_count = 1;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
        &indexed, sizeof(indexed),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW,
        "nonempty indexed draw requires nonzero NumVertices");
    indexed.num_vertices = 2;
    indexed.min_index = 0xffffffffUL;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
        &indexed, sizeof(indexed),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW,
        "MinIndex plus NumVertices overflow rejected");

    qxp_reset_fixture(1UL, TRUE);
    indexed.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    indexed.base_vertex_index = 0;
    indexed.min_index = 0;
    indexed.num_vertices = 3;
    indexed.start_index = 4;
    indexed.primitive_count = 1;
    qxp_backings.index.byte_length = 14;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &indexed, sizeof(indexed)) &&
                  qxp_out.index.byte_last_exclusive == 14,
              "legacy index exact exclusive end accepted");
    qxp_backings.index.byte_length = 13;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
        &indexed, sizeof(indexed),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_INDEX_RANGE,
        "legacy index exact end short by one rejected");

    qxp_reset_fixture(1UL, TRUE);
    qxp_set_index_stride(4);
    indexed2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    indexed2.base_vertex_offset = 0;
    indexed2.min_index = 0;
    indexed2.num_vertices = 3;
    indexed2.start_index_offset = 0xffffffffUL;
    indexed2.primitive_count = 1;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
        &indexed2, sizeof(indexed2),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_INDEX_RANGE,
        "indexed2 index byte range overflow finite extent rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_prepared.raw_state.streams[0].divider = 2;
    qxp_backings.streams[0].divider = 2;
    points.count = 1;
    points.start = 0;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_FREQUENCY_UNSUPPORTED,
        "nonunit required stream frequency remains deferred");
}

static void qxp_test_hostile_foundation_mismatches(void)
{
    QXP_TEST_POINTS points;

    points.count = 1;
    points.start = 0;

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.valid = 2;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile backing valid scalar rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.owner.shader_owner.process_id++;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile exact owner mismatch rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.stream_source_generation++;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile stream generation mismatch rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.populated_stream_mask = 0;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "populated mask must equal required mask");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.indexed = TRUE;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "indexed classification exact for nonindexed opcode");

    qxp_reset_fixture(1UL, FALSE);
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &points,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "opcode 53 independently requires indexed backing");
    qxp_reset_fixture(1UL, FALSE);
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1, &points,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2,
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "opcode 60 independently requires indexed backing");

    qxp_reset_fixture(1UL, FALSE);
    qxp_prepared.required_stream_spans[0]++;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile prepared span mismatch with backing rejected");

    qxp_reset_fixture(1UL, FALSE);
    qxp_backings.streams[0].stride++;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile stream scalar mismatch rejected");

    qxp_reset_fixture(1UL, TRUE);
    qxp_backings.index.handle++;
    qxp_expect_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &points,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
        "hostile index scalar mismatch rejected before record read");
}

static void qxp_expect_validator_reject(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *snapshot,
    const QXP_TEST_INDEXED *record, const char *name)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;

    memset(&status, 0xcc, sizeof(status));
    qxp_check(!QxpD3d9ValidateDrawEnvelope(
                  snapshot,
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  record, sizeof(*record), &qxp_prepared, &qxp_backings,
                  &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_VALIDATION,
              name);
}

static void qxp_test_canonical_validator(void)
{
    QXP_TEST_INDEXED record;
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT canonical;
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT mutated;
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;
    BYTE *bytes;
    size_t padding_offset;

    qxp_reset_fixture(1UL, TRUE);
    record.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    record.base_vertex_index = 1;
    record.min_index = 2;
    record.num_vertices = 5;
    record.start_index = 4;
    record.primitive_count = 1;
    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build canonical envelope for validator");
    canonical = qxp_out;
    qxp_check(QxpD3d9ValidateDrawEnvelope(
                  &canonical,
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), &qxp_prepared, &qxp_backings,
                  &status) && status.code == QXP_D3D9_DRAW_ENVELOPE_OK,
              "canonical reconstruction byte-compares exactly");

    mutated = canonical;
    mutated.valid = FALSE;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects valid mutation");
    mutated = canonical;
    mutated.opcode++;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects opcode mutation");
    mutated = canonical;
    mutated.record_count++;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects record-count mutation");
    mutated = canonical;
    mutated.nonempty_record_count++;
    qxp_expect_validator_reject(
        &mutated, &record, "validator rejects nonempty-count mutation");
    mutated = canonical;
    mutated.indexed = FALSE;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects indexed mutation");
    mutated = canonical;
    mutated.stream_mask = 2;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects stream-mask mutation");
    mutated = canonical;
    mutated.streams[0].valid = FALSE;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects stream-valid mutation");
    mutated = canonical;
    mutated.streams[0].stream = 1;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects stream-id mutation");
    mutated = canonical;
    mutated.streams[0].logical_first++;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects logical-range mutation");
    mutated = canonical;
    mutated.streams[0].byte_last_exclusive++;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects stream-byte mutation");
    mutated = canonical;
    mutated.index.valid = FALSE;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects index-valid mutation");
    mutated = canonical;
    mutated.index.byte_first++;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects index-byte mutation");

    padding_offset = offsetof(QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT, index) +
        offsetof(QXP_D3D9_DRAW_INDEX_ENVELOPE, byte_first) - 1;
    mutated = canonical;
    bytes = (BYTE *)&mutated;
    bytes[padding_offset] ^= 0x5aU;
    qxp_expect_validator_reject(&mutated, &record,
                                "validator rejects hostile output padding");

    record.start_index++;
    qxp_check(!QxpD3d9ValidateDrawEnvelope(
                  &canonical,
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), &qxp_prepared, &qxp_backings,
                  &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_VALIDATION,
              "validator rejects later execution-bearing record change");
    record.start_index--;

    qxp_backings.streams[0].byte_length = 40;
    qxp_check(!QxpD3d9ValidateDrawEnvelope(
                  &canonical,
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), &qxp_prepared, &qxp_backings,
                  &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
              "validator rejects later finite backing shrink");
    qxp_backings.streams[0].byte_length = 0x100000UL;

    qxp_prepared.raw_state.streams[0].stride++;
    qxp_check(!QxpD3d9ValidateDrawEnvelope(
                  &canonical,
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), &qxp_prepared, &qxp_backings,
                  &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
              "validator rejects later prepared/backing scalar mismatch");
}

static void qxp_test_null_and_pointer_free_contract(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;

    qxp_reset_fixture(1UL, FALSE);
    points.count = 1;
    points.start = 0;
    qxp_check(!QxpD3d9BuildDrawEnvelope(
                  QXP_D3D9_DP2OP_POINTS, 1, NULL, sizeof(points),
                  &qxp_prepared, &qxp_backings, &qxp_out, &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_NULL_ARGUMENT,
              "null transient records rejected");
    qxp_check(!QxpD3d9ValidateDrawEnvelope(
                  NULL, QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
                  &qxp_prepared, &qxp_backings, &status) &&
                  status.code == QXP_D3D9_DRAW_ENVELOPE_ERROR_NULL_ARGUMENT,
              "null canonical snapshot rejected");

    qxp_check(qxp_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points)) &&
                  sizeof(qxp_out.streams) / sizeof(qxp_out.streams[0]) ==
                      QXP_D3D9_DRAW_BACKING_STREAM_COUNT,
              "build is fixed-storage pointer-free with no callbacks");
}

static void qxp_test_indexed_circular_ring(void)
{
    QXP_TEST_INDEXED2 indexed2;

    /* r532 evidence: the XP D3D8 software-VP runtime submits transformed
     * DrawPrimitiveUP geometry as DRAWINDEXEDPRIMITIVE2 records over its
     * internal circular system-memory execute buffer.  The declared
     * min_index/num_vertices window must unroll across the ring wrap exactly
     * as a nonindexed window does, while the index bytes stay linear in
     * their own buffer. */
    qxp_reset_fixture(1UL, TRUE);
    qxp_prepared.raw_state.streams[0].source2 = FALSE;
    qxp_prepared.raw_state.streams[0].offset = 0;
    qxp_prepared.raw_state.streams[0].stride = 0x20UL;
    qxp_prepared.required_stream_spans[0] = 0x20UL;
    qxp_backings.streams[0].source2_offset = 0;
    qxp_backings.streams[0].flags =
        QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    qxp_backings.streams[0].byte_length = 0x140UL;
    qxp_backings.streams[0].stride = 0x20UL;
    qxp_backings.streams[0].required_span = 0x20UL;

    memset(&indexed2, 0, sizeof(indexed2));
    indexed2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    indexed2.primitive_count = 2;
    indexed2.base_vertex_offset = 0;
    indexed2.min_index = 7;
    indexed2.num_vertices = 5;
    indexed2.start_index_offset = 8;
    if (!qxp_build(QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1, &indexed2,
                   (ULONG)sizeof(indexed2))) {
        printf("DEBUG indexed-circular code=%lu field=%lu\n",
               (unsigned long)qxp_status.code,
               (unsigned long)qxp_status.field);
    }
    qxp_check(
        qxp_build(QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1, &indexed2,
                  (ULONG)sizeof(indexed2)) &&
        qxp_out.indexed &&
        qxp_out.nonempty_record_count == 1 &&
        qxp_out.streams[0].logical_first == 7UL &&
        qxp_out.streams[0].logical_last_exclusive == 12UL &&
        qxp_out.streams[0].byte_first == 0xe0UL &&
        qxp_out.streams[0].byte_last_exclusive == 0x180UL,
        "r532 indexed draw unrolls its declared window over the ring");

    /* r533 evidence: the ring suballocates mixed-size vertex blocks, so a
     * record's byte base need not be a multiple of this draw's pitch.  The
     * window floors to the containing vertex and carries the byte phase. */
    indexed2.base_vertex_offset = 48;
    indexed2.min_index = 2;
    indexed2.num_vertices = 5;
    indexed2.start_index_offset = 4;
    qxp_check(
        qxp_build(QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1, &indexed2,
                  (ULONG)sizeof(indexed2)) &&
        qxp_out.streams[0].logical_first == 3UL &&
        qxp_out.streams[0].logical_last_exclusive == 8UL &&
        qxp_out.streams[0].byte_first == 0x70UL &&
        qxp_out.streams[0].byte_last_exclusive == 0x110UL,
        "r533 unaligned indexed byte base floors with an exact phase");

    /* A negative byte base floors downward before the nonnegative window
     * check, so min_index still rescues the record exactly once. */
    indexed2.base_vertex_offset = -16;
    indexed2.min_index = 2;
    indexed2.num_vertices = 5;
    qxp_check(
        qxp_build(QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1, &indexed2,
                  (ULONG)sizeof(indexed2)) &&
        qxp_out.streams[0].logical_first == 1UL &&
        qxp_out.streams[0].logical_last_exclusive == 6UL &&
        qxp_out.streams[0].byte_first == 0x30UL,
        "r533 negative indexed byte base floors without sign loss");
}

int main(void)
{
    qxp_test_layouts();
    qxp_test_six_opcodes_and_primitive_types();
    qxp_test_zero_multiple_streams_and_kinds();
    qxp_test_circular_execute_ring();
    qxp_test_indexed_circular_ring();
    qxp_test_byte_start_alignment();
    qxp_test_indexed_windows();
    qxp_test_rejections_and_overflow();
    qxp_test_hostile_foundation_mismatches();
    qxp_test_canonical_validator();
    qxp_test_null_and_pointer_free_contract();

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw envelope tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw envelope tests: PASS\n");
    return 0;
}
