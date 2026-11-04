#include <stddef.h>
#include <windef.h>

#include "qxp_d3d9_draw_envelope.h"

/* Private byte-exact x86 layouts for the six fixed DP2 draw records. */
typedef struct qxp_d3d9_points_record {
    WORD count;
    WORD start;
} QXP_D3D9_POINTS_RECORD;

typedef struct qxp_d3d9_draw_primitive_record {
    DWORD primitive_type;
    DWORD vertex_start;
    DWORD primitive_count;
} QXP_D3D9_DRAW_PRIMITIVE_RECORD;

typedef struct qxp_d3d9_draw_indexed_record {
    DWORD primitive_type;
    LONG base_vertex_index;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index;
    DWORD primitive_count;
} QXP_D3D9_DRAW_INDEXED_RECORD;

typedef struct qxp_d3d9_clipped_fan_record {
    DWORD first_vertex_offset;
    DWORD edge_flags;
    DWORD primitive_count;
} QXP_D3D9_CLIPPED_FAN_RECORD;

typedef struct qxp_d3d9_draw_primitive2_record {
    DWORD primitive_type;
    DWORD first_vertex_offset;
    DWORD primitive_count;
} QXP_D3D9_DRAW_PRIMITIVE2_RECORD;

typedef struct qxp_d3d9_draw_indexed2_record {
    DWORD primitive_type;
    LONG base_vertex_offset;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index_offset;
    DWORD primitive_count;
} QXP_D3D9_DRAW_INDEXED2_RECORD;

typedef char qxp_envelope_private_points_size[
    sizeof(QXP_D3D9_POINTS_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_POINTS ? 1 : -1];
typedef char qxp_envelope_private_draw_size[
    sizeof(QXP_D3D9_DRAW_PRIMITIVE_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE ? 1 : -1];
typedef char qxp_envelope_private_indexed_size[
    sizeof(QXP_D3D9_DRAW_INDEXED_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE ? 1 : -1];
typedef char qxp_envelope_private_fan_size[
    sizeof(QXP_D3D9_CLIPPED_FAN_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_CLIPPEDTRIANGLEFAN ? 1 : -1];
typedef char qxp_envelope_private_draw2_size[
    sizeof(QXP_D3D9_DRAW_PRIMITIVE2_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2 ? 1 : -1];
typedef char qxp_envelope_private_indexed2_size[
    sizeof(QXP_D3D9_DRAW_INDEXED2_RECORD) ==
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2 ? 1 : -1];
typedef char qxp_envelope_private_points_offsets[
    offsetof(QXP_D3D9_POINTS_RECORD, count) ==
        QXP_D3D9_POINTS_OFFSET_COUNT &&
    offsetof(QXP_D3D9_POINTS_RECORD, start) ==
        QXP_D3D9_POINTS_OFFSET_START ? 1 : -1];
typedef char qxp_envelope_private_draw_offsets[
    offsetof(QXP_D3D9_DRAW_PRIMITIVE_RECORD, primitive_type) ==
        QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_TYPE &&
    offsetof(QXP_D3D9_DRAW_PRIMITIVE_RECORD, vertex_start) ==
        QXP_D3D9_DRAWPRIMITIVE_OFFSET_VERTEX_START &&
    offsetof(QXP_D3D9_DRAW_PRIMITIVE_RECORD, primitive_count) ==
        QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_COUNT ? 1 : -1];
typedef char qxp_envelope_private_indexed_offsets[
    offsetof(QXP_D3D9_DRAW_INDEXED_RECORD, base_vertex_index) ==
        QXP_D3D9_DRAWINDEXED_OFFSET_BASE_VERTEX &&
    offsetof(QXP_D3D9_DRAW_INDEXED_RECORD, start_index) ==
        QXP_D3D9_DRAWINDEXED_OFFSET_START_INDEX &&
    offsetof(QXP_D3D9_DRAW_INDEXED_RECORD, primitive_count) ==
        QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_COUNT ? 1 : -1];
typedef char qxp_envelope_private_fan_offsets[
    offsetof(QXP_D3D9_CLIPPED_FAN_RECORD, first_vertex_offset) ==
        QXP_D3D9_CLIPPEDFAN_OFFSET_FIRST_VERTEX &&
    offsetof(QXP_D3D9_CLIPPED_FAN_RECORD, edge_flags) ==
        QXP_D3D9_CLIPPEDFAN_OFFSET_EDGE_FLAGS &&
    offsetof(QXP_D3D9_CLIPPED_FAN_RECORD, primitive_count) ==
        QXP_D3D9_CLIPPEDFAN_OFFSET_PRIMITIVE_COUNT ? 1 : -1];
typedef char qxp_envelope_private_draw2_offsets[
    offsetof(QXP_D3D9_DRAW_PRIMITIVE2_RECORD, first_vertex_offset) ==
        QXP_D3D9_DRAWPRIMITIVE2_OFFSET_FIRST_VERTEX &&
    offsetof(QXP_D3D9_DRAW_PRIMITIVE2_RECORD, primitive_count) ==
        QXP_D3D9_DRAWPRIMITIVE2_OFFSET_PRIMITIVE_COUNT ? 1 : -1];
typedef char qxp_envelope_private_indexed2_offsets[
    offsetof(QXP_D3D9_DRAW_INDEXED2_RECORD, base_vertex_offset) ==
        QXP_D3D9_DRAWINDEXED2_OFFSET_BASE_VERTEX &&
    offsetof(QXP_D3D9_DRAW_INDEXED2_RECORD, start_index_offset) ==
        QXP_D3D9_DRAWINDEXED2_OFFSET_START_INDEX &&
    offsetof(QXP_D3D9_DRAW_INDEXED2_RECORD, primitive_count) ==
        QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_COUNT ? 1 : -1];

static void qxp_envelope_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_envelope_copy(
    void *destination, const void *source, ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static BOOL qxp_envelope_equal(
    const void *left, const void *right, ULONG byte_count)
{
    const BYTE *left_bytes;
    const BYTE *right_bytes;
    ULONG index;

    left_bytes = (const BYTE *)left;
    right_bytes = (const BYTE *)right;
    for (index = 0; index < byte_count; ++index) {
        if (left_bytes[index] != right_bytes[index]) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_envelope_is_zero(const void *memory, ULONG byte_count)
{
    const BYTE *bytes;
    ULONG index;

    bytes = (const BYTE *)memory;
    for (index = 0; index < byte_count; ++index) {
        if (bytes[index]) return FALSE;
    }
    return TRUE;
}

static void qxp_envelope_reset_status(QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    if (status) qxp_envelope_zero(status, (ULONG)sizeof(*status));
}

static BOOL qxp_envelope_fail(
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status, DWORD code,
    DWORD record_index, DWORD stream, DWORD field)
{
    if (status) {
        status->code = code;
        status->record_index = record_index;
        status->stream = stream;
        status->field = field;
    }
    return FALSE;
}

static BOOL qxp_envelope_bool(BOOL value)
{
    return value == FALSE || value == TRUE;
}

static BOOL qxp_envelope_finite(DWORD byte_length)
{
    return byte_length != 0 &&
           byte_length != QXP_D3D9_DRAW_BACKING_EXTENT_INFINITE;
}

static BOOL qxp_envelope_owner_equal(
    const QXP_D3D9_DRAW_OWNER_IDENTITY *left,
    const QXP_D3D9_DRAW_OWNER_IDENTITY *right)
{
    return left->context_handle == right->context_handle &&
           left->process_id == right->process_id &&
           left->device_identity == right->device_identity;
}

static BOOL qxp_envelope_add_u64(
    ULONGLONG left, ULONGLONG right, ULONGLONG *result)
{
    if (left > ~(ULONGLONG)0 - right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL qxp_envelope_mul_u64(
    ULONGLONG left, ULONGLONG right, ULONGLONG *result)
{
    if (left && right > ~(ULONGLONG)0 / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static BOOL qxp_envelope_ring_offset(
    ULONGLONG logical_first, DWORD stride, DWORD byte_remainder,
    DWORD byte_length, ULONGLONG *offset)
{
    ULONGLONG product;

    if (!stride || !byte_length || byte_remainder >= stride || !offset)
        return FALSE;
    product = (logical_first % (ULONGLONG)byte_length) *
              ((ULONGLONG)stride % (ULONGLONG)byte_length);
    product %= (ULONGLONG)byte_length;
    product += (ULONGLONG)byte_remainder %
               (ULONGLONG)byte_length;
    *offset = product % (ULONGLONG)byte_length;
    return TRUE;
}

static BOOL qxp_envelope_linear_backing_valid(
    const QXP_D3D9_DRAW_BACKING_RECORD *record, BOOL allow_circular)
{
    if ((record->kind != QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
         record->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR) ||
        !record->incarnation || !record->content_generation ||
        !qxp_envelope_finite(record->byte_length) ||
        (record->flags & ~QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR) ||
        (record->flags &&
         (record->kind != QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR ||
          !allow_circular)))
        return FALSE;
    if (record->kind == QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR)
        return record->video_memory_offset == 0;
    return record->video_memory_offset <=
        0xffffffffUL - record->byte_length;
}

static DWORD qxp_envelope_record_size(BYTE opcode)
{
    switch (opcode) {
    case QXP_D3D9_DP2OP_POINTS:
        return QXP_D3D9_DRAW_RECORD_BYTES_POINTS;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE;
    case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE;
    case QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN:
        return QXP_D3D9_DRAW_RECORD_BYTES_CLIPPEDTRIANGLEFAN;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE2:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2;
    case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2;
    default:
        return 0;
    }
}

static BOOL qxp_envelope_validate_stream_record(
    DWORD stream, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    const QXP_D3D9_DRAW_STREAM_STATE *source;
    const QXP_D3D9_DRAW_BACKING_RECORD *record;
    DWORD span;
    BOOL user_memory;

    source = &prepared->raw_state.streams[stream];
    record = &backings->streams[stream];
    span = prepared->required_stream_spans[stream];
    user_memory = (prepared->required_user_memory_stream_mask &
                   (1UL << stream)) ? TRUE : FALSE;

    if (!qxp_envelope_bool(source->user_memory) ||
        !qxp_envelope_bool(source->source2) || !span || !source->stride ||
        source->stride < span || (!source->source2 && source->offset) ||
        source->user_memory != user_memory ||
        record->role != QXP_D3D9_DRAW_BACKING_ROLE_VERTEX ||
        record->stream != stream || record->stride != source->stride ||
        record->source2_offset != source->offset ||
        record->divider != source->divider ||
        record->required_span != span ||
        !qxp_envelope_finite(record->byte_length) ||
        (record->flags & ~QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR) ||
        (record->flags && source->source2) ||
        source->offset > record->byte_length ||
        span > record->byte_length - source->offset) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, stream, QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD);
    }

    if (user_memory) {
        if (stream != 0 || source->handle || source->offset ||
            source->source2 ||
            record->kind !=
                QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL ||
            record->handle || record->incarnation ||
            record->content_generation || record->video_memory_offset) {
            return qxp_envelope_fail(
                status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
                0, stream, QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD);
        }
    } else if (!source->handle || record->handle != source->handle ||
               !qxp_envelope_linear_backing_valid(
                   record, !source->source2)) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, stream, QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD);
    }

    if (source->divider != 1UL) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_FREQUENCY_UNSUPPORTED,
            0, stream, QXP_D3D9_DRAW_ENVELOPE_FIELD_DIVIDER);
    }
    return TRUE;
}

static BOOL qxp_envelope_validate_index_record(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    const QXP_D3D9_DRAW_INDEX_STATE *source;
    const QXP_D3D9_DRAW_BACKING_RECORD *record;

    source = &prepared->raw_state.indices;
    record = &backings->index;
    if (!source->handle || (source->stride != 2 && source->stride != 4) ||
        record->role != QXP_D3D9_DRAW_BACKING_ROLE_INDEX ||
        record->stream != 0 || record->handle != source->handle ||
        record->stride != source->stride || record->source2_offset != 0 ||
        record->divider != 0 || record->required_span != source->stride ||
        !qxp_envelope_linear_backing_valid(record, FALSE) ||
        record->byte_length < source->stride) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_INDEX_RECORD);
    }
    return TRUE;
}

static BOOL qxp_envelope_validate_foundation(
    BOOL indexed, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    DWORD stream;
    DWORD required_mask;

    if (prepared->valid != TRUE ||
        (prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
         prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS)) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_PREPARED);
    }
    required_mask = prepared->required_stream_mask;
    if ((required_mask & ~0xffffUL) ||
        (prepared->required_user_memory_stream_mask & ~required_mask) ||
        (prepared->required_user_memory_stream_mask & ~1UL) ||
        backings->valid != TRUE || !qxp_envelope_bool(backings->indexed) ||
        backings->indexed != indexed || !backings->owner.dd_local_identity ||
        !qxp_envelope_owner_equal(
            &backings->owner.shader_owner, &prepared->owner)) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_BACKING);
    }
    if (backings->stream_source_generation !=
            prepared->raw_state.stream_source_generation) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_GENERATION);
    }
    if (backings->required_stream_mask != required_mask ||
        backings->required_user_memory_stream_mask !=
            prepared->required_user_memory_stream_mask ||
        backings->populated_stream_mask != required_mask) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_MASK);
    }

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
         ++stream) {
        if (required_mask & (1UL << stream)) {
            if (!qxp_envelope_validate_stream_record(
                    stream, prepared, backings, status)) return FALSE;
            if (backings->streams[stream].flags &&
                (required_mask != 1UL || stream != 0)) {
                /* A circular ring stays a stream-0, single-stream source.
                 * An indexed draw may consume it: the XP D3D8 software-VP
                 * runtime draws transformed user-memory geometry through
                 * DRAWINDEXEDPRIMITIVE2 records over its internal circular
                 * execute buffer (r532 evidence). */
                return qxp_envelope_fail(
                    status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
                    0, stream,
                    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD);
            }
        } else {
            if (prepared->required_stream_spans[stream] ||
                !qxp_envelope_is_zero(
                    &backings->streams[stream],
                    (ULONG)sizeof(backings->streams[stream]))) {
                return qxp_envelope_fail(
                    status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
                    0, stream,
                    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD);
            }
        }
    }

    if (indexed) return qxp_envelope_validate_index_record(
        prepared, backings, status);
    if (!qxp_envelope_is_zero(
            &backings->index, (ULONG)sizeof(backings->index))) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_INDEX_RECORD);
    }
    return TRUE;
}

static BOOL qxp_envelope_source_count(
    DWORD primitive_type, DWORD primitive_count, DWORD record_index,
    DWORD *source_count, QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    if (primitive_type < QXP_D3D9_PRIMITIVE_POINTLIST ||
        primitive_type > QXP_D3D9_PRIMITIVE_TRIANGLEFAN) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_TYPE,
            record_index, 0,
            QXP_D3D9_DRAW_ENVELOPE_FIELD_PRIMITIVE_TYPE);
    }
    if (!primitive_count) {
        *source_count = 0;
        return TRUE;
    }
    switch (primitive_type) {
    case QXP_D3D9_PRIMITIVE_POINTLIST:
        *source_count = primitive_count;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_LINELIST:
        if (primitive_count > 0xffffffffUL / 2UL) break;
        *source_count = primitive_count * 2UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_LINESTRIP:
        if (primitive_count > 0xffffffffUL - 1UL) break;
        *source_count = primitive_count + 1UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_TRIANGLELIST:
        if (primitive_count > 0xffffffffUL / 3UL) break;
        *source_count = primitive_count * 3UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_TRIANGLESTRIP:
    case QXP_D3D9_PRIMITIVE_TRIANGLEFAN:
        if (primitive_count > 0xffffffffUL - 2UL) break;
        *source_count = primitive_count + 2UL;
        return TRUE;
    default:
        break;
    }
    return qxp_envelope_fail(
        status, QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW,
        record_index, 0,
        QXP_D3D9_DRAW_ENVELOPE_FIELD_PRIMITIVE_COUNT);
}

static BOOL qxp_envelope_stream0_stride(
    const QXP_D3D9_DRAW_PREPARED *prepared, DWORD record_index,
    DWORD field, DWORD *stride, QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    const QXP_D3D9_DRAW_STREAM_STATE *stream0;

    stream0 = &prepared->raw_state.streams[0];
    if (!stream0->stride || !qxp_envelope_bool(stream0->user_memory) ||
        !qxp_envelope_bool(stream0->source2) ||
        (!stream0->source2 && stream0->offset)) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_BASE,
            record_index, 0, field);
    }
    *stride = stream0->stride;
    return TRUE;
}

static BOOL qxp_envelope_add_stream_window(
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *candidate,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    DWORD record_index, ULONGLONG logical_first, ULONGLONG logical_count,
    DWORD stream0_byte_remainder, QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    ULONGLONG logical_last;
    ULONGLONG last_vertex;
    DWORD stream;

    if (!logical_count ||
        !qxp_envelope_add_u64(
            logical_first, logical_count, &logical_last)) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
            record_index, 0,
            QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_BYTES);
    }
    last_vertex = logical_last - 1;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
         ++stream) {
        const QXP_D3D9_DRAW_BACKING_RECORD *record;
        QXP_D3D9_DRAW_STREAM_ENVELOPE *aggregate;
        ULONGLONG byte_first;
        ULONGLONG byte_last;
        ULONGLONG product;
        ULONGLONG aggregate_first;
        ULONGLONG aggregate_last;
        ULONGLONG payload_bytes;
        ULONGLONG existing_start;

        if (!(candidate->stream_mask & (1UL << stream))) continue;
        record = &backings->streams[stream];
        aggregate = &candidate->streams[stream];
        if (record->flags) {
            aggregate_first = logical_first;
            aggregate_last = logical_last;
            if (aggregate->valid) {
                if (!qxp_envelope_ring_offset(
                        aggregate->logical_first, record->stride,
                        stream == 0 ? stream0_byte_remainder : 0,
                        record->byte_length, &existing_start) ||
                    aggregate->byte_first != existing_start) {
                    return qxp_envelope_fail(
                        status, QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
                        record_index, stream,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_BYTES);
                }
                if (aggregate->logical_first < aggregate_first)
                    aggregate_first = aggregate->logical_first;
                if (aggregate->logical_last_exclusive > aggregate_last)
                    aggregate_last = aggregate->logical_last_exclusive;
            }
            if (aggregate_first >= aggregate_last ||
                !qxp_envelope_mul_u64(
                    aggregate_last - aggregate_first - 1,
                    (ULONGLONG)record->stride, &payload_bytes) ||
                !qxp_envelope_add_u64(
                    payload_bytes, (ULONGLONG)record->required_span,
                    &payload_bytes) ||
                !qxp_envelope_ring_offset(
                    aggregate_first, record->stride,
                    stream == 0 ? stream0_byte_remainder : 0,
                    record->byte_length, &byte_first) ||
                !qxp_envelope_add_u64(
                    byte_first, payload_bytes, &byte_last)) {
                return qxp_envelope_fail(
                    status, QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
                    record_index, stream,
                    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_BYTES);
            }
            aggregate->valid = TRUE;
            aggregate->stream = stream;
            aggregate->logical_first = aggregate_first;
            aggregate->logical_last_exclusive = aggregate_last;
            aggregate->byte_first = byte_first;
            aggregate->byte_last_exclusive = byte_last;
            continue;
        }
        if (stream0_byte_remainder >= record->stride ||
            !qxp_envelope_mul_u64(
                logical_first, (ULONGLONG)record->stride, &product) ||
            !qxp_envelope_add_u64(
                (ULONGLONG)record->source2_offset, product, &byte_first) ||
            !qxp_envelope_add_u64(
                byte_first,
                stream == 0 ? (ULONGLONG)stream0_byte_remainder : 0,
                &byte_first) ||
            !qxp_envelope_mul_u64(
                last_vertex, (ULONGLONG)record->stride, &product) ||
            !qxp_envelope_add_u64(
                (ULONGLONG)record->source2_offset, product, &byte_last) ||
            !qxp_envelope_add_u64(
                byte_last,
                stream == 0 ? (ULONGLONG)stream0_byte_remainder : 0,
                &byte_last) ||
            !qxp_envelope_add_u64(
                byte_last, (ULONGLONG)record->required_span, &byte_last) ||
            byte_last > (ULONGLONG)record->byte_length) {
            return qxp_envelope_fail(
                status, QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE,
                record_index, stream,
                QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_BYTES);
        }
        if (!aggregate->valid) {
            aggregate->valid = TRUE;
            aggregate->stream = stream;
            aggregate->logical_first = logical_first;
            aggregate->logical_last_exclusive = logical_last;
            aggregate->byte_first = byte_first;
            aggregate->byte_last_exclusive = byte_last;
        } else {
            if (logical_first < aggregate->logical_first)
                aggregate->logical_first = logical_first;
            if (logical_last > aggregate->logical_last_exclusive)
                aggregate->logical_last_exclusive = logical_last;
            if (byte_first < aggregate->byte_first)
                aggregate->byte_first = byte_first;
            if (byte_last > aggregate->byte_last_exclusive)
                aggregate->byte_last_exclusive = byte_last;
        }
    }
    (void)prepared;
    return TRUE;
}

static BOOL qxp_envelope_add_index_window(
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *candidate,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    DWORD record_index, ULONGLONG byte_first, DWORD index_count,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    ULONGLONG byte_count;
    ULONGLONG byte_last;

    if (!qxp_envelope_mul_u64(
            (ULONGLONG)index_count,
            (ULONGLONG)backings->index.stride, &byte_count) ||
        !qxp_envelope_add_u64(byte_first, byte_count, &byte_last) ||
        byte_last > (ULONGLONG)backings->index.byte_length) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_INDEX_RANGE,
            record_index, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_INDEX_BYTES);
    }
    if (!candidate->index.valid) {
        candidate->index.valid = TRUE;
        candidate->index.byte_first = byte_first;
        candidate->index.byte_last_exclusive = byte_last;
    } else {
        if (byte_first < candidate->index.byte_first)
            candidate->index.byte_first = byte_first;
        if (byte_last > candidate->index.byte_last_exclusive)
            candidate->index.byte_last_exclusive = byte_last;
    }
    return TRUE;
}

BOOL QxpD3d9BuildDrawEnvelope(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *out,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT candidate;
    const BYTE *record_bytes;
    DWORD record_size;
    DWORD record_index;
    BOOL indexed;
    DWORD stream0_byte_remainder;
    BOOL stream0_byte_remainder_valid;

    qxp_envelope_reset_status(status);
    if (out) qxp_envelope_zero(out, (ULONG)sizeof(*out));
    if (!records || !prepared || !backings || !out || !status) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_NULL_ARGUMENT,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_RECORDS);
    }
    record_size = qxp_envelope_record_size(opcode);
    if (!record_size) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_OPCODE,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_OPCODE);
    }
    if (record_count > available_bytes / record_size) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_TRUNCATION,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_RECORD_COUNT);
    }
    indexed = (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
               opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) ?
                  TRUE : FALSE;
    if (!qxp_envelope_validate_foundation(
            indexed, prepared, backings, status)) return FALSE;

    qxp_envelope_zero(&candidate, (ULONG)sizeof(candidate));
    candidate.opcode = opcode;
    candidate.record_count = record_count;
    candidate.indexed = indexed;
    candidate.stream_mask = prepared->required_stream_mask;
    stream0_byte_remainder = 0;
    stream0_byte_remainder_valid = FALSE;
    record_bytes = (const BYTE *)records;

    for (record_index = 0; record_index < record_count; ++record_index) {
        QXP_D3D9_POINTS_RECORD points;
        QXP_D3D9_DRAW_PRIMITIVE_RECORD draw;
        QXP_D3D9_DRAW_INDEXED_RECORD draw_indexed;
        QXP_D3D9_CLIPPED_FAN_RECORD fan;
        QXP_D3D9_DRAW_PRIMITIVE2_RECORD draw2;
        QXP_D3D9_DRAW_INDEXED2_RECORD draw_indexed2;
        DWORD primitive_type;
        DWORD primitive_count;
        DWORD source_count;
        DWORD first_vertex;
        DWORD min_index;
        DWORD num_vertices;
        DWORD start_index;
        LONG base_vertex;
        BOOL byte_first_vertex;
        BOOL indexed2;
        DWORD stream0_stride;
        DWORD byte_first_remainder;
        DWORD indexed_remainder;
        ULONGLONG logical_first;
        ULONGLONG index_byte_first;
        LONGLONG signed_base;
        LONGLONG signed_first;

        primitive_type = 0;
        primitive_count = 0;
        source_count = 0;
        first_vertex = 0;
        min_index = 0;
        num_vertices = 0;
        start_index = 0;
        base_vertex = 0;
        byte_first_vertex = FALSE;
        indexed2 = FALSE;
        stream0_stride = 0;
        byte_first_remainder = 0;
        indexed_remainder = 0;
        logical_first = 0;
        index_byte_first = 0;
        signed_base = 0;
        signed_first = 0;

        switch (opcode) {
        case QXP_D3D9_DP2OP_POINTS:
            qxp_envelope_copy(
                &points, record_bytes + record_index * record_size,
                (ULONG)sizeof(points));
            primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
            primitive_count = points.count;
            first_vertex = points.start;
            break;
        case QXP_D3D9_DP2OP_DRAWPRIMITIVE:
            qxp_envelope_copy(
                &draw, record_bytes + record_index * record_size,
                (ULONG)sizeof(draw));
            primitive_type = draw.primitive_type;
            primitive_count = draw.primitive_count;
            first_vertex = draw.vertex_start;
            break;
        case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE:
            qxp_envelope_copy(
                &draw_indexed, record_bytes + record_index * record_size,
                (ULONG)sizeof(draw_indexed));
            primitive_type = draw_indexed.primitive_type;
            primitive_count = draw_indexed.primitive_count;
            base_vertex = draw_indexed.base_vertex_index;
            min_index = draw_indexed.min_index;
            num_vertices = draw_indexed.num_vertices;
            start_index = draw_indexed.start_index;
            break;
        case QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN:
            qxp_envelope_copy(
                &fan, record_bytes + record_index * record_size,
                (ULONG)sizeof(fan));
            primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLEFAN;
            primitive_count = fan.primitive_count;
            first_vertex = fan.first_vertex_offset;
            byte_first_vertex = TRUE;
            break;
        case QXP_D3D9_DP2OP_DRAWPRIMITIVE2:
            qxp_envelope_copy(
                &draw2, record_bytes + record_index * record_size,
                (ULONG)sizeof(draw2));
            primitive_type = draw2.primitive_type;
            primitive_count = draw2.primitive_count;
            first_vertex = draw2.first_vertex_offset;
            byte_first_vertex = TRUE;
            break;
        case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2:
            qxp_envelope_copy(
                &draw_indexed2,
                record_bytes + record_index * record_size,
                (ULONG)sizeof(draw_indexed2));
            primitive_type = draw_indexed2.primitive_type;
            primitive_count = draw_indexed2.primitive_count;
            base_vertex = draw_indexed2.base_vertex_offset;
            min_index = draw_indexed2.min_index;
            num_vertices = draw_indexed2.num_vertices;
            start_index = draw_indexed2.start_index_offset;
            indexed2 = TRUE;
            break;
        default:
            return qxp_envelope_fail(
                status, QXP_D3D9_DRAW_ENVELOPE_ERROR_OPCODE,
                record_index, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_OPCODE);
        }

        if (!qxp_envelope_source_count(
                primitive_type, primitive_count, record_index,
                &source_count, status)) return FALSE;
        if (!source_count) continue;
        ++candidate.nonempty_record_count;

        if (indexed) {
            if (!num_vertices ||
                min_index > 0xffffffffUL - num_vertices) {
                return qxp_envelope_fail(
                    status,
                    QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW,
                    record_index, 0,
                    !num_vertices ?
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_NUM_VERTICES :
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_MIN_INDEX);
            }
            signed_base = (LONGLONG)base_vertex;
            if (indexed2) {
                if (!qxp_envelope_stream0_stride(
                        prepared, record_index,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_BASE_VERTEX,
                        &stream0_stride, status)) return FALSE;
                /* The XP D3D8 software-VP transform ring suballocates
                 * mixed-size vertex blocks, so a record's byte base need
                 * not be a multiple of this draw's declaration pitch
                 * (r533 evidence).  Floor the base to its containing
                 * vertex and carry the byte phase exactly like the
                 * nonindexed byte-offset path; records inside one batch
                 * must agree on that phase. */
                indexed_remainder = (DWORD)
                    (signed_base % (LONGLONG)stream0_stride);
                signed_base /= (LONGLONG)stream0_stride;
                if ((LONG)indexed_remainder < 0) {
                    indexed_remainder += stream0_stride;
                    signed_base -= 1;
                }
                if ((stream0_byte_remainder_valid &&
                     indexed_remainder != stream0_byte_remainder)) {
                    return qxp_envelope_fail(
                        status,
                        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT,
                        record_index, 0,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_BASE_VERTEX);
                }
                stream0_byte_remainder = indexed_remainder;
                stream0_byte_remainder_valid = TRUE;
                index_byte_first = (ULONGLONG)start_index;
            } else {
                if (!qxp_envelope_mul_u64(
                        (ULONGLONG)start_index,
                        (ULONGLONG)backings->index.stride,
                        &index_byte_first)) {
                    return qxp_envelope_fail(
                        status, QXP_D3D9_DRAW_ENVELOPE_ERROR_INDEX_RANGE,
                        record_index, 0,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_START_INDEX);
                }
            }
            signed_first = signed_base + (LONGLONG)min_index;
            if (signed_first < 0) {
                return qxp_envelope_fail(
                    status,
                    QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW,
                    record_index, 0,
                    QXP_D3D9_DRAW_ENVELOPE_FIELD_BASE_VERTEX);
            }
            logical_first = (ULONGLONG)signed_first;
            if (!qxp_envelope_add_stream_window(
                    &candidate, prepared, backings, record_index,
                    logical_first, (ULONGLONG)num_vertices,
                    stream0_byte_remainder, status) ||
                !qxp_envelope_add_index_window(
                    &candidate, backings, record_index,
                    index_byte_first, source_count, status)) {
                return FALSE;
            }
        } else {
            if (byte_first_vertex) {
                if (!qxp_envelope_stream0_stride(
                        prepared, record_index,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_FIRST_VERTEX,
                        &stream0_stride, status)) return FALSE;
                byte_first_remainder = first_vertex % stream0_stride;
                if ((candidate.stream_mask != 1UL &&
                     byte_first_remainder) ||
                    (stream0_byte_remainder_valid &&
                     byte_first_remainder != stream0_byte_remainder)) {
                    return qxp_envelope_fail(
                        status,
                        QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT,
                        record_index, 0,
                        QXP_D3D9_DRAW_ENVELOPE_FIELD_FIRST_VERTEX);
                }
                stream0_byte_remainder = byte_first_remainder;
                stream0_byte_remainder_valid = TRUE;
                logical_first = first_vertex / stream0_stride;
            } else {
                logical_first = first_vertex;
            }
            if (!qxp_envelope_add_stream_window(
                    &candidate, prepared, backings, record_index,
                    logical_first, (ULONGLONG)source_count,
                    byte_first_remainder, status)) return FALSE;
        }
    }

    candidate.valid = TRUE;
    qxp_envelope_copy(out, &candidate, (ULONG)sizeof(*out));
    return TRUE;
}

BOOL QxpD3d9ValidateDrawEnvelope(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *snapshot,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status)
{
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT reconstructed;

    qxp_envelope_reset_status(status);
    if (!snapshot) {
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_NULL_ARGUMENT,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_OUTPUT);
    }
    if (!QxpD3d9BuildDrawEnvelope(
            opcode, record_count, records, available_bytes,
            prepared, backings, &reconstructed, status)) return FALSE;
    if (!qxp_envelope_equal(
            snapshot, &reconstructed, (ULONG)sizeof(*snapshot))) {
        qxp_envelope_reset_status(status);
        return qxp_envelope_fail(
            status, QXP_D3D9_DRAW_ENVELOPE_ERROR_VALIDATION,
            0, 0, QXP_D3D9_DRAW_ENVELOPE_FIELD_OUTPUT);
    }
    return TRUE;
}
