#include <stddef.h>
#include <windef.h>

#include "qxp_d3d9_draw_capture.h"

#define QXP_D3D9_DRAW_CAPTURE_HASH_SEED 2166136261UL
#define QXP_D3D9_DRAW_CAPTURE_HASH_PRIME 16777619UL

typedef char qxp_capture_stream_count_is_sixteen[
    QXP_D3D9_DRAW_BACKING_STREAM_COUNT == 16UL ? 1 : -1];
typedef char qxp_capture_payload_offset_aligned[
    (QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET %
        QXP_D3D9_DRAW_CAPTURE_PAYLOAD_ALIGNMENT) == 0 ? 1 : -1];
typedef char qxp_capture_payload_offset_covers_header[
    QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET >=
        QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE ? 1 : -1];
typedef char qxp_capture_header_within_cap[
    QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET <
        QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES ? 1 : -1];

typedef struct qxp_capture_layout {
    DWORD record_size;
    DWORD records_bytes;
    BOOL indexed;
    DWORD stream_mask;
    DWORD stream_count;
    DWORD stream_payload_bytes;
    DWORD index_payload_bytes;
    DWORD total_bytes;
    QXP_D3D9_DRAW_CAPTURE_INDEX_DESCRIPTOR index;
    QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR
        streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
} QXP_CAPTURE_LAYOUT;

static void qxp_capture_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static BOOL qxp_capture_equal(
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

static BOOL qxp_capture_is_zero(const void *memory, ULONG byte_count)
{
    const BYTE *bytes;
    ULONG index;

    bytes = (const BYTE *)memory;
    for (index = 0; index < byte_count; ++index) {
        if (bytes[index]) return FALSE;
    }
    return TRUE;
}

static void qxp_capture_copy_bytes(
    void *destination, const void *source, ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static void qxp_capture_reset_status(QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    if (status) qxp_capture_zero(status, (ULONG)sizeof(*status));
}

static BOOL qxp_capture_fail(
    QXP_D3D9_DRAW_CAPTURE_STATUS *status, DWORD code, DWORD field,
    DWORD stream)
{
    if (status) {
        status->code = code;
        status->field = field;
        status->stream = stream;
    }
    return FALSE;
}

static DWORD qxp_capture_hash_region(
    const BYTE *base, DWORD length, DWORD skip_offset, DWORD skip_length)
{
    DWORD hash;
    DWORD index;

    hash = QXP_D3D9_DRAW_CAPTURE_HASH_SEED;
    for (index = 0; index < length; ++index) {
        BYTE value;

        value = base[index];
        if (index >= skip_offset && index - skip_offset < skip_length)
            value = 0;
        hash ^= (DWORD)value;
        hash *= QXP_D3D9_DRAW_CAPTURE_HASH_PRIME;
    }
    return hash;
}

static DWORD qxp_capture_blob_hash(const BYTE *blob, DWORD hash_length)
{
    return qxp_capture_hash_region(
        blob, hash_length,
        (DWORD)offsetof(QXP_D3D9_DRAW_CAPTURE_HEADER, hash),
        (DWORD)sizeof(((QXP_D3D9_DRAW_CAPTURE_HEADER *)0)->hash));
}

static DWORD qxp_capture_record_size(BYTE opcode)
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

static BOOL qxp_capture_exact_record_bytes(
    BYTE opcode, DWORD record_count, DWORD *out_bytes)
{
    DWORD record_size;

    *out_bytes = 0;
    record_size = qxp_capture_record_size(opcode);
    if (!record_size || !record_count ||
        record_count > 0xffffffffUL / record_size) return FALSE;
    *out_bytes = record_count * record_size;
    return TRUE;
}

static BOOL qxp_capture_required(DWORD mask, DWORD stream)
{
    return (mask & (1UL << stream)) != 0;
}

static BOOL qxp_capture_linear_kind(DWORD kind)
{
    return kind == QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR ||
           kind == QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
}

static BOOL qxp_capture_check_stream_semantics(
    DWORD stream, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_D3D9_DRAW_STREAM_ENVELOPE *range,
    QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const QXP_D3D9_DRAW_BACKING_RECORD *backing;
    const QXP_D3D9_DRAW_STREAM_STATE *state;
    DWORD expected_kind;
    DWORD logical_first;
    DWORD logical_last;
    DWORD first_vertex_bytes;
    DWORD trailing_vertex_bytes;
    DWORD last_vertex_delta;
    DWORD byte_first;
    DWORD byte_remainder;
    DWORD byte_last;
    DWORD remaining;
    DWORD payload_bytes;

    backing = &backings->streams[stream];
    state = &prepared->raw_state.streams[stream];
    if (prepared->required_user_memory_stream_mask & (1UL << stream)) {
        expected_kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    } else {
        expected_kind = qxp_capture_linear_kind(backing->kind) ?
            backing->kind : 0;
    }

    if (stream != 0 &&
        expected_kind == QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, stream);
    }
    if (!expected_kind || backing->kind != expected_kind ||
        backing->role != QXP_D3D9_DRAW_BACKING_ROLE_VERTEX ||
        backing->stream != stream ||
        (backing->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
         backing->video_memory_offset) ||
        (backing->flags &&
         (stream != 0 || state->source2 ||
          expected_kind != QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR)) ||
        !backing->stride || !backing->required_span ||
        backing->divider != 1UL ||
        (state->source2 != FALSE && state->source2 != TRUE) ||
        (!state->source2 && state->offset) ||
        backing->source2_offset != state->offset ||
        state->divider != 1UL || backing->stride != state->stride ||
        backing->required_span != prepared->required_stream_spans[stream]) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
    }
    if (expected_kind == QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL) {
        if (stream != 0 || backing->handle || backing->incarnation ||
            backing->content_generation || state->handle || state->offset ||
            state->source2 || state->user_memory != TRUE) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
        }
    } else {
        if (!backing->handle || !backing->incarnation ||
            !backing->content_generation || !state->handle ||
            state->handle != backing->handle || state->user_memory != FALSE) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
        }
    }

    if (range->valid != TRUE || range->stream != stream ||
        range->logical_first >= range->logical_last_exclusive ||
        range->byte_first >= range->byte_last_exclusive ||
        range->logical_first > (ULONGLONG)0xffffffffUL ||
        range->logical_last_exclusive > (ULONGLONG)0xffffffffUL ||
        range->byte_first > (ULONGLONG)0xffffffffUL ||
        range->byte_last_exclusive > (ULONGLONG)0xffffffffUL) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }

    logical_first = (DWORD)range->logical_first;
    logical_last = (DWORD)range->logical_last_exclusive;
    last_vertex_delta = logical_last - logical_first - 1UL;
    if (backing->flags) {
        if (range->byte_first >= (ULONGLONG)backing->byte_length ||
            last_vertex_delta >
                (0xffffffffUL - backing->required_span) / backing->stride) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
                QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
        }
        payload_bytes =
            last_vertex_delta * backing->stride + backing->required_span;
        if (range->byte_last_exclusive - range->byte_first !=
                (ULONGLONG)payload_bytes) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
                QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
        }
        descriptor->valid = TRUE;
        descriptor->source_kind = expected_kind;
        descriptor->logical_first = logical_first;
        descriptor->logical_last_exclusive = logical_last;
        descriptor->byte_first = (DWORD)range->byte_first;
        descriptor->byte_last_exclusive =
            (DWORD)range->byte_last_exclusive;
        descriptor->payload_bytes = payload_bytes;
        return TRUE;
    }
    remaining = backing->byte_length;
    if (state->offset > remaining) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    remaining -= state->offset;
    if (logical_first > remaining / backing->stride) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    first_vertex_bytes = logical_first * backing->stride;
    remaining -= first_vertex_bytes;
    byte_first = state->offset + first_vertex_bytes;
    if (range->byte_first < (ULONGLONG)byte_first ||
        range->byte_first - (ULONGLONG)byte_first >=
            (ULONGLONG)backing->stride ||
        (stream != 0 && range->byte_first != (ULONGLONG)byte_first)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    byte_remainder =
        (DWORD)(range->byte_first - (ULONGLONG)byte_first);
    if (byte_remainder > remaining) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    remaining -= byte_remainder;
    byte_first += byte_remainder;
    if (last_vertex_delta > remaining / backing->stride) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    trailing_vertex_bytes = last_vertex_delta * backing->stride;
    remaining -= trailing_vertex_bytes;
    if (backing->required_span > remaining) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }
    byte_last = byte_first + trailing_vertex_bytes;
    byte_last += backing->required_span;
    if (range->byte_first != (ULONGLONG)byte_first ||
        range->byte_last_exclusive != (ULONGLONG)byte_last) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
            QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0, stream);
    }

    descriptor->valid = TRUE;
    descriptor->source_kind = expected_kind;
    descriptor->logical_first = logical_first;
    descriptor->logical_last_exclusive = logical_last;
    descriptor->byte_first = byte_first;
    descriptor->byte_last_exclusive = byte_last;
    descriptor->payload_bytes = byte_last - byte_first;
    return TRUE;
}

static BOOL qxp_capture_check_semantics(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_CAPTURE_LAYOUT *layout, QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    DWORD record_size;
    DWORD records_bytes;
    DWORD stream_mask;
    DWORD user_memory_mask;
    DWORD stream;

    qxp_capture_zero(layout, (ULONG)sizeof(*layout));
    qxp_capture_zero(&envelope_status, (ULONG)sizeof(envelope_status));
    if (!QxpD3d9ValidateDrawEnvelope(
            envelope, opcode, record_count, records, available_bytes,
            prepared, backings, &envelope_status)) {
        qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ENVELOPE, envelope_status.stream);
        if (status) {
            qxp_capture_copy_bytes(
                &status->envelope_error, &envelope_status,
                (ULONG)sizeof(status->envelope_error));
        }
        return FALSE;
    }

    record_size = qxp_capture_record_size(opcode);
    if (!record_size) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_OPCODE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_OPCODE, 0);
    }
    if (!record_count || record_count > (DWORD)(available_bytes / record_size) ||
        record_count > 0xffffffffUL / record_size) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_COUNT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_COUNT, 0);
    }
    records_bytes = record_count * record_size;
    stream_mask = prepared->required_stream_mask;
    user_memory_mask = prepared->required_user_memory_stream_mask;

    if (!stream_mask ||
        (stream_mask & ~QXP_D3D9_DRAW_CAPTURE_STREAM_MASK) ||
        (user_memory_mask & ~1UL) || (user_memory_mask & ~stream_mask)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, 0);
    }
    if (backings->valid != TRUE || backings->indexed != FALSE ||
        backings->required_stream_mask != stream_mask ||
        backings->required_user_memory_stream_mask != user_memory_mask ||
        backings->populated_stream_mask != stream_mask) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, 0);
    }
    if (envelope->valid != TRUE || envelope->indexed != FALSE ||
        envelope->stream_mask != stream_mask ||
        !qxp_capture_is_zero(
            &backings->index, (ULONG)sizeof(backings->index)) ||
        !qxp_capture_is_zero(
            &envelope->index, (ULONG)sizeof(envelope->index))) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, 0);
    }

    layout->record_size = record_size;
    layout->records_bytes = records_bytes;
    layout->stream_mask = stream_mask;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(stream_mask, stream)) {
            if (!qxp_capture_check_stream_semantics(
                    stream, prepared, backings, &envelope->streams[stream],
                    &layout->streams[stream], status)) return FALSE;
            if (layout->streams[stream].payload_bytes >
                    0xffffffffUL - layout->stream_payload_bytes) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, stream);
            }
            layout->stream_payload_bytes +=
                layout->streams[stream].payload_bytes;
            ++layout->stream_count;
        } else {
            /* Unused raw bindings are ordinary D3D state and need not be
             * zero.  Only capture-authoritative nonrequired records must be
             * zero-canonical. */
            if (!qxp_capture_is_zero(
                    &backings->streams[stream],
                    (ULONG)sizeof(backings->streams[stream])) ||
                !qxp_capture_is_zero(
                    &envelope->streams[stream],
                    (ULONG)sizeof(envelope->streams[stream]))) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
            }
        }
    }
    return TRUE;
}

static BOOL qxp_capture_check_indexed_foundation(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_CAPTURE_LAYOUT *layout, QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    const QXP_D3D9_DRAW_BACKING_RECORD *index;
    DWORD record_size;
    DWORD stream_mask;
    DWORD stream;

    qxp_capture_zero(layout, (ULONG)sizeof(*layout));
    qxp_capture_zero(&envelope_status, (ULONG)sizeof(envelope_status));
    if (opcode != QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE &&
        opcode != QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_OPCODE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_OPCODE, 0);
    }
    if (!QxpD3d9ValidateDrawEnvelope(
            envelope, opcode, record_count, records, available_bytes,
            prepared, backings, &envelope_status)) {
        qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ENVELOPE, envelope_status.stream);
        qxp_capture_copy_bytes(
            &status->envelope_error, &envelope_status,
            (ULONG)sizeof(status->envelope_error));
        return FALSE;
    }
    record_size = qxp_capture_record_size(opcode);
    if (!record_size || !record_count ||
        record_count > (DWORD)(available_bytes / record_size) ||
        record_count > 0xffffffffUL / record_size) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_COUNT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_COUNT, 0);
    }
    stream_mask = prepared->required_stream_mask;
    if (!stream_mask ||
        (stream_mask & ~QXP_D3D9_DRAW_CAPTURE_STREAM_MASK) ||
        prepared->required_user_memory_stream_mask ||
        backings->valid != TRUE || backings->indexed != TRUE ||
        backings->required_stream_mask != stream_mask ||
        backings->required_user_memory_stream_mask ||
        backings->populated_stream_mask != stream_mask ||
        envelope->valid != TRUE || envelope->indexed != TRUE ||
        envelope->stream_mask != stream_mask || !envelope->index.valid) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, 0);
    }
    index = &backings->index;
    if (!qxp_capture_linear_kind(index->kind) ||
        index->role != QXP_D3D9_DRAW_BACKING_ROLE_INDEX ||
        index->stream || !index->handle || !index->incarnation ||
        !index->content_generation || !index->byte_length ||
        (index->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
         index->video_memory_offset) ||
        index->flags ||
        index->source2_offset || index->divider ||
        index->required_span != index->stride ||
        envelope->index.byte_first >=
            envelope->index.byte_last_exclusive ||
        envelope->index.byte_last_exclusive >
            (ULONGLONG)index->byte_length ||
        envelope->index.byte_first > (ULONGLONG)0xffffffffUL ||
        envelope->index.byte_last_exclusive >
            (ULONGLONG)0xffffffffUL) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_INDEX_SOURCE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    layout->record_size = record_size;
    layout->records_bytes = record_count * record_size;
    layout->indexed = TRUE;
    layout->stream_mask = stream_mask;
    layout->index.valid = TRUE;
    layout->index.source_kind = index->kind;
    layout->index.stride = index->stride;
    layout->index.byte_first = (DWORD)envelope->index.byte_first;
    layout->index.byte_last_exclusive =
        (DWORD)envelope->index.byte_last_exclusive;
    layout->index.payload_bytes =
        layout->index.byte_last_exclusive - layout->index.byte_first;
    layout->index_payload_bytes = layout->index.payload_bytes;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(stream_mask, stream)) continue;
        if (!qxp_capture_is_zero(
                &backings->streams[stream],
                (ULONG)sizeof(backings->streams[stream])) ||
            !qxp_capture_is_zero(
                &envelope->streams[stream],
                (ULONG)sizeof(envelope->streams[stream]))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
        }
    }
    return TRUE;
}

static BOOL qxp_capture_apply_indexed_access_layout(
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_CAPTURE_LAYOUT *layout, QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    DWORD stream;

    if (!plan || plan->valid != TRUE || plan->indexed != TRUE ||
        plan->stream_mask != layout->stream_mask) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
    }
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (!qxp_capture_required(layout->stream_mask, stream)) continue;
        if (!qxp_capture_check_stream_semantics(
                stream, prepared, backings, &plan->streams[stream],
                &layout->streams[stream], status)) return FALSE;
        if (layout->streams[stream].payload_bytes >
                0xffffffffUL - layout->stream_payload_bytes) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, stream);
        }
        layout->stream_payload_bytes +=
            layout->streams[stream].payload_bytes;
        ++layout->stream_count;
    }
    return TRUE;
}

static BOOL qxp_capture_finish_layout(
    QXP_CAPTURE_LAYOUT *layout, QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    DWORD cursor;
    DWORD stream;

    cursor = QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET;
    if (layout->records_bytes >
            QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES - cursor) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, 0);
    }
    cursor += layout->records_bytes;
    if (layout->indexed) {
        layout->index.payload_offset = cursor;
        if (!layout->index.valid ||
            layout->index.payload_bytes != layout->index_payload_bytes ||
            layout->index.payload_bytes >
                QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES - cursor) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
        }
        cursor += layout->index.payload_bytes;
    }
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(layout->stream_mask, stream)) {
            layout->streams[stream].payload_offset = cursor;
            if (layout->streams[stream].payload_bytes >
                    QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES - cursor) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, stream);
            }
            cursor += layout->streams[stream].payload_bytes;
        }
    }
    layout->total_bytes = cursor;
    return TRUE;
}

static BOOL qxp_capture_check_access_plan(
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan,
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    const QXP_CAPTURE_LAYOUT *layout,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    DWORD stream;

    if (plan->valid != TRUE || plan->indexed != layout->indexed ||
        plan->stream_mask != layout->stream_mask) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
    }
    if (layout->indexed) {
        if (!plan->index.valid ||
            !qxp_capture_equal(
                &plan->index, &envelope->index,
                (ULONG)sizeof(plan->index))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
                QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
        }
    } else if (plan->scanned_index_count.low ||
               plan->scanned_index_count.high ||
               !qxp_capture_is_zero(
                   &plan->index, (ULONG)sizeof(plan->index))) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
    }
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(layout->stream_mask, stream)) {
            if (plan->streams[stream].valid != TRUE ||
                plan->streams[stream].stream != stream ||
                plan->streams[stream].logical_first !=
                    (ULONGLONG)layout->streams[stream].logical_first ||
                plan->streams[stream].logical_last_exclusive !=
                    (ULONGLONG)layout->streams[stream].logical_last_exclusive ||
                plan->streams[stream].byte_first !=
                    (ULONGLONG)layout->streams[stream].byte_first ||
                plan->streams[stream].byte_last_exclusive !=
                    (ULONGLONG)layout->streams[stream].byte_last_exclusive ||
                (!layout->indexed &&
                 !qxp_capture_equal(
                     &plan->streams[stream], &envelope->streams[stream],
                     (ULONG)sizeof(plan->streams[stream])))) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, stream);
            }
        } else if (!qxp_capture_is_zero(
                       &plan->streams[stream],
                       (ULONG)sizeof(plan->streams[stream]))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
                QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, stream);
        }
    }
    return TRUE;
}

static BOOL qxp_capture_build_access(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    DWORD records_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, DWORD index_byte_count,
    QXP_D3D9_DRAW_ACCESS_PLAN *plan,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_ACCESS_STATUS access_status;

    qxp_capture_zero(&access_status, (ULONG)sizeof(access_status));
    if (!QxpD3d9BuildDrawAccess(
            envelope, opcode, record_count, records, (ULONG)records_bytes,
            prepared, backings, index_bytes, (ULONG)index_byte_count,
            plan, &access_status)) {
        qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, access_status.stream);
        if (status) {
            qxp_capture_copy_bytes(
                &status->access_error, &access_status,
                (ULONG)sizeof(status->access_error));
        }
        return FALSE;
    }
    qxp_capture_zero(&access_status, (ULONG)sizeof(access_status));
    if (!QxpD3d9ValidateDrawAccess(
            plan, envelope, opcode, record_count, records,
            (ULONG)records_bytes, prepared, backings,
            index_bytes, (ULONG)index_byte_count, &access_status)) {
        qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, access_status.stream);
        if (status) {
            qxp_capture_copy_bytes(
                &status->access_error, &access_status,
                (ULONG)sizeof(status->access_error));
        }
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_capture_build_and_prove_access(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    DWORD records_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, DWORD index_byte_count,
    QXP_D3D9_DRAW_ACCESS_PLAN *plan, const QXP_CAPTURE_LAYOUT *layout,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    if (!qxp_capture_build_access(
            envelope, opcode, record_count, records, records_bytes,
            prepared, backings, index_bytes, index_byte_count, plan,
            status)) return FALSE;
    return qxp_capture_check_access_plan(plan, envelope, layout, status);
}

static BOOL qxp_capture_check_sources(
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW *sources,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_CAPTURE_LAYOUT *layout,
    const BYTE **source_addresses,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW *source;
    const QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor;
    DWORD stream;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        source_addresses[stream] = NULL;
        source = &sources[stream];
        descriptor = &layout->streams[stream];
        if (!qxp_capture_required(layout->stream_mask, stream)) {
            if (source->base || source->byte_count || source->kind) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
            }
            continue;
        }
        if (source->kind != descriptor->source_kind ||
            source->byte_count != backings->streams[stream].byte_length) {
            if (stream == 0 &&
                source->kind ==
                    QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL &&
                prepared->required_user_memory_stream_mask != 1UL) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, stream);
            }
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
                QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING, stream);
        }
        if (backings->streams[stream].flags) {
            if (!source->base || !source->byte_count ||
                descriptor->byte_first >= source->byte_count ||
                !descriptor->payload_bytes ||
                (ULONG_PTR)source->base >
                    ~(ULONG_PTR)0 - (ULONG_PTR)source->byte_count) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_USER_MEMORY, stream);
            }
            source_addresses[stream] = (const BYTE *)source->base;
            continue;
        }
        if (!source->base || descriptor->byte_first > source->byte_count ||
            descriptor->payload_bytes >
                source->byte_count - descriptor->byte_first ||
            (ULONG_PTR)source->base >
                ~(ULONG_PTR)0 - (ULONG_PTR)source->byte_count ||
            (ULONG_PTR)source->base >
                ~(ULONG_PTR)0 - (ULONG_PTR)descriptor->byte_first) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_USER_MEMORY, stream);
        }
        source_addresses[stream] =
            (const BYTE *)source->base + descriptor->byte_first;
        if ((ULONG_PTR)source_addresses[stream] >
                ~(ULONG_PTR)0 - (ULONG_PTR)descriptor->payload_bytes) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_USER_MEMORY, stream);
        }
    }
    return TRUE;
}

static BOOL qxp_capture_check_index_source(
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW *source,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_CAPTURE_LAYOUT *layout, const BYTE **source_address,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    *source_address = NULL;
    if (!layout->indexed) {
        if (source) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_INDEX_SOURCE,
                QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
        }
        return TRUE;
    }
    if (!source || !source->base ||
        !qxp_capture_linear_kind(source->kind) ||
        source->kind != layout->index.source_kind ||
        source->byte_count != backings->index.byte_length ||
        layout->index.byte_first > source->byte_count ||
        layout->index.payload_bytes >
            source->byte_count - layout->index.byte_first ||
        (ULONG_PTR)source->base >
            ~(ULONG_PTR)0 - (ULONG_PTR)source->byte_count ||
        (ULONG_PTR)source->base >
            ~(ULONG_PTR)0 - (ULONG_PTR)layout->index.byte_first) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_INDEX_SOURCE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    *source_address =
        (const BYTE *)source->base + layout->index.byte_first;
    if ((ULONG_PTR)*source_address >
            ~(ULONG_PTR)0 - (ULONG_PTR)layout->index.payload_bytes) {
        *source_address = NULL;
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_INDEX_SOURCE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    return TRUE;
}

static BOOL qxp_capture_copy_stream_payload(
    BYTE *destination, const BYTE *source, DWORD source_bytes,
    const QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor,
    BOOL circular, QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy, void *opaque)
{
    DWORD copied_bytes;
    DWORD remaining;
    DWORD source_offset;

    if (!circular) {
        return copy(
            destination, source, (ULONG)descriptor->payload_bytes, opaque);
    }
    if (!destination || !source || !source_bytes || !descriptor ||
        !descriptor->payload_bytes ||
        descriptor->byte_first >= source_bytes) return FALSE;
    copied_bytes = 0;
    remaining = descriptor->payload_bytes;
    source_offset = descriptor->byte_first;
    while (remaining) {
        DWORD chunk;

        chunk = source_bytes - source_offset;
        if (chunk > remaining) chunk = remaining;
        if (!copy(
                destination + copied_bytes, source + source_offset,
                (ULONG)chunk, opaque)) return FALSE;
        copied_bytes += chunk;
        remaining -= chunk;
        source_offset = 0;
    }
    return TRUE;
}

static void qxp_capture_unsecure_all(
    void **stream_handles, DWORD secured_stream_mask, void *index_handle,
    void *record_handle, QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    void *opaque)
{
    DWORD stream;

    stream = QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
    while (stream) {
        --stream;
        if (qxp_capture_required(secured_stream_mask, stream))
            unsecure(stream_handles[stream], opaque);
    }
    if (index_handle) unsecure(index_handle, opaque);
    if (record_handle) unsecure(record_handle, opaque);
}

BOOL QxpD3d9CaptureDrawStreams(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT],
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW *index_source,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_CAPTURE_LAYOUT layout;
    QXP_D3D9_DRAW_ACCESS_PLAN transient_plan;
    QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    BYTE *blob;
    const BYTE *source_addresses[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    const BYTE *index_address;
    const BYTE *copied_index_address;
    void *stream_handles[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    void *record_handle;
    void *index_handle;
    DWORD record_range_bytes;
    DWORD secured_stream_mask;
    DWORD stream;
    DWORD copy_field;
    DWORD copy_stream;
    DWORD hash_length;
    DWORD secure_bytes;
    BOOL indexed;
    BOOL copied;

    qxp_capture_reset_status(status);
    if (out_blob) *out_blob = NULL;
    if (out_blob_bytes) *out_blob_bytes = 0;
    if (!envelope || !records || !prepared || !backings || !sources ||
        !out_blob || !out_blob_bytes || !status) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT, 0);
    }
    if (qxp_capture_exact_record_bytes(
            opcode, record_count, &record_range_bytes) &&
        (!record_range_bytes ||
         (ULONG_PTR)records >
             ~(ULONG_PTR)0 - (ULONG_PTR)record_range_bytes)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    indexed = (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
               opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) ?
        TRUE : FALSE;
    if (indexed) {
        if (!qxp_capture_check_indexed_foundation(
                envelope, opcode, record_count, records, available_bytes,
                prepared, backings, &layout, status)) return FALSE;
    } else if (!qxp_capture_check_semantics(
                   envelope, opcode, record_count, records, available_bytes,
                   prepared, backings, &layout, status)) {
        return FALSE;
    }
    if (!allocator || !allocator->allocate || !allocator->release ||
        !secure || !unsecure || !copy) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT, 0);
    }
    index_address = NULL;
    if (!qxp_capture_check_index_source(
            index_source, backings, &layout, &index_address, status)) {
        return FALSE;
    }
    if (indexed) {
        if (!qxp_capture_build_access(
                envelope, opcode, record_count, records,
                layout.records_bytes, prepared, backings, index_address,
                layout.index.payload_bytes, &transient_plan, status) ||
            !qxp_capture_apply_indexed_access_layout(
                &transient_plan, prepared, backings, &layout, status) ||
            !qxp_capture_check_access_plan(
                &transient_plan, envelope, &layout, status)) return FALSE;
    } else if (!qxp_capture_build_and_prove_access(
                   envelope, opcode, record_count, records,
                   layout.records_bytes, prepared, backings, NULL, 0,
                   &transient_plan, &layout, status)) {
        return FALSE;
    }
    if (!qxp_capture_check_sources(
            sources, prepared, backings, &layout, source_addresses, status))
        return FALSE;
    if (!qxp_capture_finish_layout(&layout, status)) return FALSE;

    blob = (BYTE *)allocator->allocate(
        (ULONG)layout.total_bytes, allocator->opaque);
    if (!blob) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ALLOCATION,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, 0);
    }
    qxp_capture_zero(blob, (ULONG)layout.total_bytes);
    qxp_capture_zero(stream_handles, (ULONG)sizeof(stream_handles));
    header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    header->magic = QXP_D3D9_DRAW_CAPTURE_MAGIC;
    header->version = QXP_D3D9_DRAW_CAPTURE_VERSION;
    header->opcode = (DWORD)opcode;
    header->record_count = record_count;
    header->record_size = layout.record_size;
    header->header_size = QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE;
    header->payload_offset = QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET;
    header->records_offset = QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET;
    header->records_bytes = layout.records_bytes;
    header->indexed = layout.indexed;
    header->stream_mask = layout.stream_mask;
    header->stream_count = layout.stream_count;
    header->stream_payload_bytes = layout.stream_payload_bytes;
    header->index_payload_bytes = layout.index_payload_bytes;
    header->total_bytes = layout.total_bytes;
    qxp_capture_copy_bytes(
        &header->index, &layout.index, (ULONG)sizeof(header->index));
    qxp_capture_copy_bytes(
        header->streams, layout.streams, (ULONG)sizeof(header->streams));
    qxp_capture_copy_bytes(
        &header->prepared, prepared, (ULONG)sizeof(header->prepared));
    qxp_capture_copy_bytes(
        &header->backings, backings, (ULONG)sizeof(header->backings));
    qxp_capture_copy_bytes(
        &header->envelope, envelope, (ULONG)sizeof(header->envelope));

    record_handle = secure(
        records, (ULONG)layout.records_bytes, callback_opaque);
    if (!record_handle) {
        allocator->release(blob, allocator->opaque);
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_RECORDS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_PAYLOAD, 0);
    }
    index_handle = NULL;
    if (indexed &&
        index_source->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR) {
        index_handle = secure(
            index_address, (ULONG)layout.index.payload_bytes,
            callback_opaque);
        if (!index_handle) {
            qxp_capture_unsecure_all(
                stream_handles, 0, NULL, record_handle, unsecure,
                callback_opaque);
            allocator->release(blob, allocator->opaque);
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
        }
    }
    secured_stream_mask = 0;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(layout.stream_mask, stream) &&
            sources[stream].kind !=
                QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR) {
            secure_bytes = backings->streams[stream].flags ?
                sources[stream].byte_count :
                layout.streams[stream].payload_bytes;
            stream_handles[stream] = secure(
                source_addresses[stream], (ULONG)secure_bytes,
                callback_opaque);
            if (!stream_handles[stream]) {
                qxp_capture_unsecure_all(
                    stream_handles, secured_stream_mask, index_handle,
                    record_handle, unsecure, callback_opaque);
                allocator->release(blob, allocator->opaque);
                return qxp_capture_fail(
                    status,
                    QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD, stream);
            }
            secured_stream_mask |= 1UL << stream;
        }
    }

    copy_field = QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_PAYLOAD;
    copy_stream = 0;
    copied = copy(
        blob + QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET, records,
        (ULONG)layout.records_bytes, callback_opaque);
    if (copied && indexed) {
        copy_field = QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD;
        copied = copy(
            blob + layout.index.payload_offset, index_address,
            (ULONG)layout.index.payload_bytes, callback_opaque);
    }
    for (stream = 0; copied &&
             stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (qxp_capture_required(layout.stream_mask, stream)) {
            copy_field = QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD;
            copy_stream = stream;
            copied = qxp_capture_copy_stream_payload(
                blob + layout.streams[stream].payload_offset,
                source_addresses[stream], sources[stream].byte_count,
                &layout.streams[stream],
                backings->streams[stream].flags ? TRUE : FALSE,
                copy, callback_opaque);
        }
    }
    qxp_capture_unsecure_all(
        stream_handles, secured_stream_mask, index_handle, record_handle,
        unsecure, callback_opaque);
    if (!copied) {
        allocator->release(blob, allocator->opaque);
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_COPY, copy_field,
            copy_stream);
    }

    copied_index_address = indexed ?
        blob + header->index.payload_offset : NULL;
    if (!qxp_capture_build_and_prove_access(
            &header->envelope, opcode, record_count,
            blob + header->records_offset, layout.records_bytes,
            &header->prepared, &header->backings, copied_index_address,
            header->index_payload_bytes, &header->access_plan,
            &layout, status)) {
        allocator->release(blob, allocator->opaque);
        return FALSE;
    }
    if (!qxp_capture_equal(
            &header->access_plan, &transient_plan,
            (ULONG)sizeof(header->access_plan))) {
        allocator->release(blob, allocator->opaque);
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
    }

    header->valid = QXP_D3D9_DRAW_CAPTURE_VALID;
    hash_length = indexed ?
        header->index.payload_offset + header->index.payload_bytes :
        header->records_offset + header->records_bytes;
    header->hash = qxp_capture_blob_hash(blob, hash_length);
    *out_blob = blob;
    *out_blob_bytes = layout.total_bytes;
    return TRUE;
}

BOOL QxpD3d9CaptureNonindexedDrawStreams(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT],
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    return QxpD3d9CaptureDrawStreams(
        envelope, opcode, record_count, records, available_bytes, prepared,
        backings, sources, NULL, allocator, secure, unsecure, copy,
        callback_opaque, out_blob, out_blob_bytes, status);
}

BOOL QxpD3d9CaptureNonindexedDraw(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *user_memory_base, DWORD user_memory_bytes,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];

    qxp_capture_zero(sources, (ULONG)sizeof(sources));
    sources[0].base = user_memory_base;
    sources[0].byte_count = user_memory_bytes;
    sources[0].kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    return QxpD3d9CaptureNonindexedDrawStreams(
        envelope, opcode, record_count, records, available_bytes, prepared,
        backings, sources, allocator, secure, unsecure, copy, callback_opaque,
        out_blob, out_blob_bytes, status);
}

BOOL QxpD3d9CaptureNonindexedSurfaceDraw(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *surface_base, DWORD surface_bytes,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];

    qxp_capture_zero(sources, (ULONG)sizeof(sources));
    sources[0].base = surface_base;
    sources[0].byte_count = surface_bytes;
    sources[0].kind = backings ?
        backings->streams[0].kind : QXP_D3D9_DRAW_BACKING_KIND_INVALID;
    return QxpD3d9CaptureNonindexedDrawStreams(
        envelope, opcode, record_count, records, available_bytes, prepared,
        backings, sources, allocator, secure, unsecure, copy, callback_opaque,
        out_blob, out_blob_bytes, status);
}

BOOL QxpD3d9ValidateCapturedDraw(
    const void *blob, DWORD blob_byte_count,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const BYTE *blob_bytes;
    const BYTE *records_in_blob;
    const BYTE *index_in_blob;
    QXP_CAPTURE_LAYOUT layout;
    QXP_D3D9_DRAW_ACCESS_PLAN rebuilt_plan;
    DWORD record_size;
    DWORD records_bytes;
    DWORD cursor;
    DWORD aggregate;
    DWORD count;
    DWORD stream;
    DWORD computed_hash;
    DWORD hash_length;
    BYTE opcode;
    BOOL opcode_indexed;

    qxp_capture_reset_status(status);
    if (!blob || !status) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT, 0);
    }
    if (blob_byte_count < QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET ||
        blob_byte_count > QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB, 0);
    }
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    blob_bytes = (const BYTE *)blob;
    if (header->magic != QXP_D3D9_DRAW_CAPTURE_MAGIC ||
        header->version != QXP_D3D9_DRAW_CAPTURE_VERSION ||
        header->valid != QXP_D3D9_DRAW_CAPTURE_VALID ||
        header->opcode > 0xffUL ||
        (header->indexed != FALSE && header->indexed != TRUE)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_HEADER,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }
    opcode = (BYTE)header->opcode;
    opcode_indexed =
        (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
         opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) ? TRUE : FALSE;
    record_size = qxp_capture_record_size(opcode);
    if (!record_size || header->record_size != record_size ||
        header->header_size != QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE ||
        header->payload_offset != QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET ||
        header->records_offset != QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET ||
        header->indexed != opcode_indexed ||
        header->reserved || !header->record_count ||
        header->record_count > 0xffffffffUL / record_size ||
        !header->stream_mask ||
        (header->stream_mask & ~QXP_D3D9_DRAW_CAPTURE_STREAM_MASK)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }
    records_bytes = header->record_count * record_size;
    if (header->records_bytes != records_bytes ||
        records_bytes > QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES -
                            QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }

    cursor = QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET + records_bytes;
    if (header->indexed) {
        if (header->index.valid != TRUE ||
            !qxp_capture_linear_kind(header->index.source_kind) ||
            (header->index.stride != 2UL &&
             header->index.stride != 4UL) ||
            header->index.byte_first >=
                header->index.byte_last_exclusive ||
            header->index.payload_bytes !=
                header->index.byte_last_exclusive -
                    header->index.byte_first ||
            header->index.payload_bytes != header->index_payload_bytes ||
            header->index.payload_offset != cursor ||
            header->index.reserved ||
            header->index.payload_bytes >
                QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES - cursor) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
                QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
        }
        cursor += header->index.payload_bytes;
    } else if (header->index_payload_bytes ||
               !qxp_capture_is_zero(
                   &header->index, (ULONG)sizeof(header->index))) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }

    aggregate = 0;
    count = 0;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        const QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor;

        descriptor = &header->streams[stream];
        if (qxp_capture_required(header->stream_mask, stream)) {
            if (descriptor->valid != TRUE ||
                descriptor->logical_first >=
                    descriptor->logical_last_exclusive ||
                descriptor->byte_first >= descriptor->byte_last_exclusive ||
                descriptor->payload_bytes !=
                    descriptor->byte_last_exclusive - descriptor->byte_first ||
                descriptor->payload_offset != cursor ||
                descriptor->payload_bytes >
                    QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES - cursor ||
                descriptor->payload_bytes > 0xffffffffUL - aggregate) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, stream);
            }
            cursor += descriptor->payload_bytes;
            aggregate += descriptor->payload_bytes;
            ++count;
        } else if (!qxp_capture_is_zero(
                       descriptor, (ULONG)sizeof(*descriptor))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
                QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, stream);
        }
    }
    if (header->stream_count != count ||
        header->stream_payload_bytes != aggregate ||
        header->total_bytes != cursor || cursor != blob_byte_count) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }
    if (!qxp_capture_is_zero(
            blob_bytes + QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE,
            QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET -
                QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }

    hash_length = header->indexed ?
        header->index.payload_offset + header->index.payload_bytes :
        header->records_offset + records_bytes;
    computed_hash = qxp_capture_blob_hash(blob_bytes, hash_length);
    if (computed_hash != header->hash) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_HASH,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HASH, 0);
    }
    records_in_blob = blob_bytes + header->records_offset;
    index_in_blob = header->indexed ?
        blob_bytes + header->index.payload_offset : NULL;
    if (header->indexed) {
        if (!qxp_capture_check_indexed_foundation(
                &header->envelope, opcode, header->record_count,
                records_in_blob, (ULONG)records_bytes, &header->prepared,
                &header->backings, &layout, status) ||
            !qxp_capture_build_access(
                &header->envelope, opcode, header->record_count,
                records_in_blob, records_bytes, &header->prepared,
                &header->backings, index_in_blob,
                header->index_payload_bytes, &rebuilt_plan, status) ||
            !qxp_capture_equal(
                &rebuilt_plan, &header->access_plan,
                (ULONG)sizeof(rebuilt_plan)) ||
            !qxp_capture_apply_indexed_access_layout(
                &rebuilt_plan, &header->prepared, &header->backings,
                &layout, status) ||
            !qxp_capture_check_access_plan(
                &rebuilt_plan, &header->envelope, &layout, status)) {
            if (!status->code) {
                qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
            }
            return FALSE;
        }
    } else {
        if (!qxp_capture_check_semantics(
                &header->envelope, opcode, header->record_count,
                records_in_blob, (ULONG)records_bytes, &header->prepared,
                &header->backings, &layout, status) ||
            !qxp_capture_build_and_prove_access(
                &header->envelope, opcode, header->record_count,
                records_in_blob, records_bytes, &header->prepared,
                &header->backings, NULL, 0, &rebuilt_plan, &layout,
                status)) return FALSE;
        if (!qxp_capture_equal(
                &rebuilt_plan, &header->access_plan,
                (ULONG)sizeof(rebuilt_plan))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
                QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN, 0);
        }
    }
    if (!qxp_capture_finish_layout(&layout, status)) return FALSE;
    if (layout.record_size != header->record_size ||
        layout.records_bytes != header->records_bytes ||
        layout.indexed != header->indexed ||
        layout.stream_mask != header->stream_mask ||
        layout.stream_count != header->stream_count ||
        layout.stream_payload_bytes != header->stream_payload_bytes ||
        layout.index_payload_bytes != header->index_payload_bytes ||
        layout.total_bytes != header->total_bytes ||
        !qxp_capture_equal(
            &layout.index, &header->index,
            (ULONG)sizeof(header->index)) ||
        !qxp_capture_equal(
            layout.streams, header->streams,
            (ULONG)sizeof(header->streams))) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
            QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER, 0);
    }
    return TRUE;
}

const void *QxpD3d9CapturedDrawRecords(const void *blob, DWORD *byte_count)
{
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;

    if (byte_count) *byte_count = 0;
    if (!blob) return NULL;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    if (byte_count) *byte_count = header->records_bytes;
    return (const BYTE *)blob + header->records_offset;
}

const void *QxpD3d9CapturedDrawIndexPayload(
    const void *blob, DWORD *byte_count)
{
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;

    if (byte_count) *byte_count = 0;
    if (!blob) return NULL;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    if (header->index.valid != TRUE || !header->index.payload_bytes)
        return NULL;
    if (byte_count) *byte_count = header->index.payload_bytes;
    return (const BYTE *)blob + header->index.payload_offset;
}

const void *QxpD3d9CapturedDrawStreamPayload(
    const void *blob, DWORD stream, DWORD *byte_count)
{
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor;

    if (byte_count) *byte_count = 0;
    if (!blob || stream >= QXP_D3D9_DRAW_BACKING_STREAM_COUNT) return NULL;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    descriptor = &header->streams[stream];
    if (descriptor->valid != TRUE || !descriptor->payload_bytes) return NULL;
    if (byte_count) *byte_count = descriptor->payload_bytes;
    return (const BYTE *)blob + descriptor->payload_offset;
}

const void *QxpD3d9CapturedDrawVertexPayload(
    const void *blob, DWORD *byte_count)
{
    return QxpD3d9CapturedDrawStreamPayload(blob, 0, byte_count);
}

BOOL QxpD3d9BuildLegacyMultistreamPreparedCapture(
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_D3D9_DRAW_OWNER_IDENTITY *owner,
    DWORD declaration_stream_mask,
    const DWORD declaration_stream_spans[QXP_D3D9_DRAW_STREAM_COUNT],
    QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    DWORD user_memory_mask;
    DWORD stream;

    if (prepared) qxp_capture_zero(prepared, (ULONG)sizeof(*prepared));
    qxp_capture_reset_status(status);
    if (!draw_state || !pixel_binding || !owner ||
        !declaration_stream_spans || !prepared || !status) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT, 0);
    }
    if (!owner->context_handle || !owner->process_id ||
        !owner->device_identity ||
        !(declaration_stream_mask & 1UL) ||
        !(declaration_stream_mask & ~1UL) ||
        (declaration_stream_mask & ~QXP_D3D9_DRAW_CAPTURE_STREAM_MASK) ||
        draw_state->declaration_binding.handle ||
        draw_state->declaration_binding.generation ||
        draw_state->vertex_function_binding.handle ||
        draw_state->vertex_function_binding.generation ||
        draw_state->declaration_mode ==
            QXP_D3D9_DRAW_DECLARATION_OBJECT ||
        pixel_binding->sm2.handle || pixel_binding->sm2.generation ||
        QxpD3d9PixelBindingRequiresSm2Execution(pixel_binding)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, 0);
    }
    qxp_capture_zero(&draw_status, (ULONG)sizeof(draw_status));
    if (!QxpD3d9DrawStateValidate(draw_state, &draw_status)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, draw_status.stream);
    }

    user_memory_mask = 0;
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        const QXP_D3D9_DRAW_STREAM_STATE *source;
        DWORD span;

        source = &draw_state->streams[stream];
        span = declaration_stream_spans[stream];
        if (!(declaration_stream_mask & (1UL << stream))) {
            if (span) {
                return qxp_capture_fail(
                    status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
                    QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, stream);
            }
            continue;
        }
        if (!span || !source->stride || source->stride < span ||
            source->divider != 1UL ||
            (source->user_memory != FALSE && source->user_memory != TRUE) ||
            (source->source2 != FALSE && source->source2 != TRUE) ||
            (!source->source2 && source->offset) ||
            (stream && source->user_memory) ||
            (!source->user_memory && !source->handle) ||
            (source->user_memory &&
             (stream || source->handle || source->offset ||
              source->source2))) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
                QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, stream);
        }
        if (source->user_memory) user_memory_mask |= 1UL << stream;
    }

    prepared->mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    qxp_capture_copy_bytes(
        &prepared->owner, owner, (ULONG)sizeof(prepared->owner));
    qxp_capture_copy_bytes(
        &prepared->raw_state, draw_state,
        (ULONG)sizeof(prepared->raw_state));
    qxp_capture_copy_bytes(
        &prepared->raw_pixel_binding, pixel_binding,
        (ULONG)sizeof(prepared->raw_pixel_binding));
    prepared->required_stream_mask = declaration_stream_mask;
    prepared->required_user_memory_stream_mask = user_memory_mask;
    qxp_capture_copy_bytes(
        prepared->required_stream_spans, declaration_stream_spans,
        (ULONG)sizeof(prepared->required_stream_spans));
    prepared->valid = TRUE;
    return TRUE;
}

BOOL QxpD3d9PrepareCapturedDrawExecution(
    const void *blob, DWORD blob_bytes,
    QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    DWORD records_bytes;
    DWORD logical_first;
    DWORD logical_last;
    DWORD stream;

    if (plan) qxp_capture_zero(plan, (ULONG)sizeof(*plan));
    qxp_capture_reset_status(status);
    if (!blob || !plan || !status) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT, 0);
    }
    if (!QxpD3d9ValidateCapturedDraw(blob, blob_bytes, status)) return FALSE;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    if (!header->stream_count) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, 0);
    }

    for (stream = 0;
         stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT &&
         !(header->stream_mask & (1UL << stream));
         ++stream) {
    }
    if (stream == QXP_D3D9_DRAW_BACKING_STREAM_COUNT) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
            QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED, 0);
    }
    logical_first = header->streams[stream].logical_first;
    logical_last = header->streams[stream].logical_last_exclusive;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
         ++stream) {
        const QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR *descriptor;
        const QXP_D3D9_DRAW_STREAM_STATE *state;
        const void *payload;
        DWORD payload_bytes;
        DWORD vertex_delta;
        DWORD required_bytes;
        DWORD required_span;

        if (!(header->stream_mask & (1UL << stream))) continue;
        descriptor = &header->streams[stream];
        state = &header->prepared.raw_state.streams[stream];
        required_span = header->prepared.required_stream_spans[stream];
        payload = QxpD3d9CapturedDrawStreamPayload(
            blob, stream, &payload_bytes);
        if (!payload || !payload_bytes || !state->stride ||
            !required_span || state->stride < required_span ||
            descriptor->logical_first != logical_first ||
            descriptor->logical_last_exclusive != logical_last ||
            logical_first >= logical_last) {
            qxp_capture_zero(plan, (ULONG)sizeof(*plan));
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
                QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD, stream);
        }
        vertex_delta = logical_last - logical_first - 1UL;
        if (vertex_delta > 0xffffffffUL / state->stride) {
            qxp_capture_zero(plan, (ULONG)sizeof(*plan));
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
                QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD, stream);
        }
        required_bytes = vertex_delta * state->stride;
        if (required_span > 0xffffffffUL - required_bytes ||
            required_bytes + required_span != payload_bytes) {
            qxp_capture_zero(plan, (ULONG)sizeof(*plan));
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
                QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD, stream);
        }
        plan->streams[stream].payload = payload;
        plan->streams[stream].payload_bytes = payload_bytes;
        plan->streams[stream].stride = state->stride;
        plan->streams[stream].required_span = required_span;
        plan->streams[stream].logical_first = logical_first;
        plan->streams[stream].logical_last_exclusive = logical_last;
    }
    plan->records = QxpD3d9CapturedDrawRecords(blob, &records_bytes);
    if (!plan->records || records_bytes != header->records_bytes) {
        qxp_capture_zero(plan, (ULONG)sizeof(*plan));
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    plan->records_bytes = records_bytes;
    plan->opcode = header->opcode;
    plan->record_count = header->record_count;
    plan->indexed = header->indexed;
    if (header->indexed) {
        plan->index_payload =
            QxpD3d9CapturedDrawIndexPayload(blob, NULL);
        if (!plan->index_payload) {
            qxp_capture_zero(plan, (ULONG)sizeof(*plan));
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION,
                QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
        }
        qxp_capture_copy_bytes(
            &plan->index, &header->index,
            (ULONG)sizeof(plan->index));
    }
    plan->stream_mask = header->stream_mask;
    plan->logical_first = logical_first;
    plan->logical_last_exclusive = logical_last;
    plan->prepared = &header->prepared;
    return TRUE;
}

static WORD qxp_capture_read_word(const BYTE *bytes)
{
    WORD value;

    value = 0;
    qxp_capture_copy_bytes(&value, bytes, (ULONG)sizeof(value));
    return value;
}

static DWORD qxp_capture_read_dword(const BYTE *bytes)
{
    DWORD value;

    value = 0;
    qxp_capture_copy_bytes(&value, bytes, (ULONG)sizeof(value));
    return value;
}

static BOOL qxp_capture_primitive_source_count(
    DWORD primitive_type, DWORD primitive_count, DWORD *source_count)
{
    if (!source_count) return FALSE;
    *source_count = 0;
    switch (primitive_type) {
    case QXP_D3D9_PRIMITIVE_POINTLIST:
        *source_count = primitive_count;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_LINELIST:
        if (primitive_count > 0xffffffffUL / 2UL) return FALSE;
        *source_count = primitive_count * 2UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_LINESTRIP:
        if (!primitive_count) return TRUE;
        if (primitive_count == 0xffffffffUL) return FALSE;
        *source_count = primitive_count + 1UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_TRIANGLELIST:
        if (primitive_count > 0xffffffffUL / 3UL) return FALSE;
        *source_count = primitive_count * 3UL;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_TRIANGLESTRIP:
    case QXP_D3D9_PRIMITIVE_TRIANGLEFAN:
        if (!primitive_count) return TRUE;
        if (primitive_count > 0xffffffffUL - 2UL) return FALSE;
        *source_count = primitive_count + 2UL;
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL QxpD3d9CapturedDrawRecordToDraw2(
    const QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan, DWORD record_index,
    QXP_D3D9_CAPTURED_DRAW2 *draw,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const BYTE *record;
    DWORD record_size;
    DWORD stream0_stride;
    DWORD vertex_start;
    DWORD source_count;
    DWORD logical_first;
    DWORD edge_flags;
    QXP_D3D9_CAPTURED_DRAW2 candidate;

    if (draw) qxp_capture_zero(draw, (ULONG)sizeof(*draw));
    qxp_capture_reset_status(status);
    if (!plan || !draw || !status || !plan->prepared || !plan->records ||
        !plan->record_count || record_index >= plan->record_count ||
        !plan->prepared->raw_state.streams[0].stride) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    record_size = qxp_capture_record_size((BYTE)plan->opcode);
    if (!record_size || plan->record_count > 0xffffffffUL / record_size ||
        plan->records_bytes != plan->record_count * record_size) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    record = (const BYTE *)plan->records + record_index * record_size;
    stream0_stride = plan->prepared->raw_state.streams[0].stride;
    vertex_start = 0;
    edge_flags = 0;
    qxp_capture_zero(&candidate, (ULONG)sizeof(candidate));
    switch (plan->opcode) {
    case QXP_D3D9_DP2OP_POINTS:
        candidate.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
        candidate.primitive_count = (DWORD)qxp_capture_read_word(
            record + QXP_D3D9_POINTS_OFFSET_COUNT);
        vertex_start = (DWORD)qxp_capture_read_word(
            record + QXP_D3D9_POINTS_OFFSET_START);
        if (vertex_start > 0xffffffffUL / stream0_stride) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
                QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
        }
        candidate.first_vertex_offset = vertex_start * stream0_stride;
        break;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE:
        candidate.primitive_type = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_TYPE);
        vertex_start = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE_OFFSET_VERTEX_START);
        candidate.primitive_count = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_COUNT);
        if (vertex_start > 0xffffffffUL / stream0_stride) {
            return qxp_capture_fail(
                status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
                QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
        }
        candidate.first_vertex_offset = vertex_start * stream0_stride;
        break;
    case QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN:
        candidate.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLEFAN;
        candidate.first_vertex_offset = qxp_capture_read_dword(
            record + QXP_D3D9_CLIPPEDFAN_OFFSET_FIRST_VERTEX);
        edge_flags = qxp_capture_read_dword(
            record + QXP_D3D9_CLIPPEDFAN_OFFSET_EDGE_FLAGS);
        candidate.primitive_count = qxp_capture_read_dword(
            record + QXP_D3D9_CLIPPEDFAN_OFFSET_PRIMITIVE_COUNT);
        /* Edge flags only affect non-solid edge rendering.  The advertised
         * ordinary path is solid fill, and the legacy fan conversion
         * already ignores them, so a clipped fan from the stock runtime's
         * software-VP clipping (r535 evidence) captures with the same
         * meaning instead of refusing the draw. */
        (void)edge_flags;
        break;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE2:
        candidate.primitive_type = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE2_OFFSET_PRIMITIVE_TYPE);
        candidate.first_vertex_offset = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE2_OFFSET_FIRST_VERTEX);
        candidate.primitive_count = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWPRIMITIVE2_OFFSET_PRIMITIVE_COUNT);
        break;
    default:
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_OPCODE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_OPCODE, 0);
    }
    if ((candidate.first_vertex_offset % stream0_stride &&
         plan->stream_mask != 1UL) ||
        !qxp_capture_primitive_source_count(
            candidate.primitive_type, candidate.primitive_count,
            &source_count)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    logical_first = candidate.first_vertex_offset / stream0_stride;
    if (source_count &&
        (logical_first < plan->logical_first ||
         logical_first >= plan->logical_last_exclusive ||
         source_count > plan->logical_last_exclusive - logical_first)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    qxp_capture_copy_bytes(draw, &candidate, (ULONG)sizeof(candidate));
    return TRUE;
}

BOOL QxpD3d9CapturedDrawRecordToIndexedDraw2(
    const QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan, DWORD record_index,
    QXP_D3D9_CAPTURED_INDEXED_DRAW2 *draw,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    const BYTE *record;
    DWORD record_size;
    DWORD source_count;
    DWORD raw_start;
    ULONGLONG start_offset;
    ULONGLONG end_offset;
    LONGLONG base_offset;
    LONG raw_base;
    QXP_D3D9_CAPTURED_INDEXED_DRAW2 candidate;

    if (draw) qxp_capture_zero(draw, (ULONG)sizeof(*draw));
    qxp_capture_reset_status(status);
    if (!plan || !draw || !status || !plan->prepared ||
        !plan->indexed || !plan->records ||
        !plan->index_payload || !plan->record_count ||
        record_index >= plan->record_count ||
        (plan->index.stride != 2UL && plan->index.stride != 4UL) ||
        !plan->prepared->raw_state.streams[0].stride) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    record_size = qxp_capture_record_size((BYTE)plan->opcode);
    if (!record_size || plan->record_count > 0xffffffffUL / record_size ||
        plan->records_bytes != plan->record_count * record_size) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE, 0);
    }
    record = (const BYTE *)plan->records + record_index * record_size;
    qxp_capture_zero(&candidate, (ULONG)sizeof(candidate));
    raw_base = 0;
    raw_start = 0;
    if (plan->opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE) {
        candidate.primitive_type = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_TYPE);
        raw_base = (LONG)qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_BASE_VERTEX);
        candidate.min_index = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_MIN_INDEX);
        candidate.num_vertices = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_NUM_VERTICES);
        raw_start = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_START_INDEX);
        candidate.primitive_count = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_COUNT);
    } else if (plan->opcode ==
                   QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) {
        candidate.primitive_type = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_TYPE);
        raw_base = (LONG)qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_BASE_VERTEX);
        candidate.min_index = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_MIN_INDEX);
        candidate.num_vertices = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_NUM_VERTICES);
        raw_start = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_START_INDEX);
        candidate.primitive_count = qxp_capture_read_dword(
            record + QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_COUNT);
    } else {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_OPCODE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_OPCODE, 0);
    }
    if (!qxp_capture_primitive_source_count(
            candidate.primitive_type, candidate.primitive_count,
            &source_count)) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    if (!source_count) {
        candidate.base_vertex_offset = 0;
        candidate.start_index_offset = 0;
        qxp_capture_copy_bytes(
            draw, &candidate, (ULONG)sizeof(candidate));
        return TRUE;
    }
    if (plan->opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE) {
        base_offset = (LONGLONG)raw_base *
                      (LONGLONG)
                          plan->prepared->raw_state.streams[0].stride;
        start_offset = (ULONGLONG)raw_start *
                       (ULONGLONG)plan->index.stride;
    } else {
        base_offset = (LONGLONG)raw_base;
        start_offset = (ULONGLONG)raw_start;
    }
    if (base_offset < (-2147483647L - 1L) ||
        base_offset > 0x7fffffffL ||
        start_offset < (ULONGLONG)plan->index.byte_first ||
        start_offset >
            (ULONGLONG)plan->index.byte_last_exclusive) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    end_offset = start_offset +
                 (ULONGLONG)source_count *
                     (ULONGLONG)plan->index.stride;
    if (end_offset < start_offset ||
        end_offset > (ULONGLONG)plan->index.byte_last_exclusive ||
        start_offset - (ULONGLONG)plan->index.byte_first >
            0xffffffffUL) {
        return qxp_capture_fail(
            status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE,
            QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD, 0);
    }
    candidate.base_vertex_offset = (LONG)base_offset;
    candidate.start_index_offset =
        (DWORD)(start_offset - (ULONGLONG)plan->index.byte_first);
    qxp_capture_copy_bytes(draw, &candidate, (ULONG)sizeof(candidate));
    return TRUE;
}

BOOL QxpD3d9CaptureStatusIsResourceFailure(
    const QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    return status &&
           (status->code == QXP_D3D9_DRAW_CAPTURE_ERROR_ALLOCATION ||
            status->code == QXP_D3D9_DRAW_CAPTURE_ERROR_TRANSPORT);
}

BOOL QxpD3d9BuildDrawUserMemoryEnvelope(
    BOOL user_memory_active, const void *vertices, DWORD vertex_offset,
    DWORD vertex_size, DWORD vertex_length,
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE *envelope,
    const void **out_base, DWORD *out_bytes)
{
    DWORD address;
    DWORD observed;
    DWORD bytes;

    if (out_base) *out_base = NULL;
    if (out_bytes) *out_bytes = 0;
    if (!envelope) return FALSE;
    qxp_capture_zero(envelope, (ULONG)sizeof(*envelope));
    envelope->active = user_memory_active ? TRUE : FALSE;
    envelope->nonnull_pointer_observed = vertices ? TRUE : FALSE;
    envelope->vertex_size = vertex_size;
    envelope->vertex_length = vertex_length;
    envelope->vertex_offset = vertex_offset;
    address = (DWORD)(ULONG_PTR)vertices;
    if (!envelope->active || !envelope->nonnull_pointer_observed ||
        !vertex_size || !vertex_length ||
        vertex_length > 0xffffffffUL / vertex_size) return FALSE;
    bytes = vertex_length * vertex_size;
    if (!bytes || vertex_offset > 0xffffffffUL - address) return FALSE;
    observed = address + vertex_offset;
    if (bytes > 0xffffffffUL - observed) return FALSE;
    envelope->address_range_valid = TRUE;
    if (out_base) {
        *out_base = (const void *)((const BYTE *)vertices + vertex_offset);
    }
    if (out_bytes) *out_bytes = bytes;
    return TRUE;
}

BOOL QxpD3d9DrawCaptureStreamsAreEligible(
    BYTE opcode, BOOL indexed, BOOL user_memory_active,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings)
{
    DWORD stream_mask;
    DWORD user_memory_mask;
    DWORD stream;

    if (!prepared || !backings || !qxp_capture_record_size(opcode))
        return FALSE;
    if (indexed !=
        (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
         opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2)) return FALSE;
    if (indexed && user_memory_active) return FALSE;
    stream_mask = prepared->required_stream_mask;
    user_memory_mask = prepared->required_user_memory_stream_mask;
    if (!stream_mask ||
        (stream_mask & ~QXP_D3D9_DRAW_CAPTURE_STREAM_MASK) ||
        (user_memory_mask & ~1UL) ||
        (user_memory_mask & ~stream_mask) ||
        (user_memory_mask && !user_memory_active) ||
        (indexed && user_memory_mask) ||
        backings->valid != TRUE || backings->indexed != indexed ||
        backings->required_stream_mask != stream_mask ||
        backings->required_user_memory_stream_mask != user_memory_mask ||
        backings->populated_stream_mask != stream_mask) return FALSE;
    if (indexed &&
        (!qxp_capture_linear_kind(backings->index.kind) ||
         (backings->index.stride != 2UL &&
          backings->index.stride != 4UL))) return FALSE;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        DWORD kind;

        if (!(stream_mask & (1UL << stream))) continue;
        kind = backings->streams[stream].kind;
        if (user_memory_mask & (1UL << stream)) {
            if (kind != QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL)
                return FALSE;
        } else if (!qxp_capture_linear_kind(kind)) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL QxpD3d9DrawCaptureIsEligible(
    BYTE opcode, BOOL indexed, BOOL user_memory_active,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings)
{
    if (!prepared || !backings) return FALSE;
    if (!user_memory_active || indexed) return FALSE;
    if (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
        opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) return FALSE;
    if (prepared->required_stream_mask != 1UL ||
        prepared->required_user_memory_stream_mask != 1UL ||
        backings->required_stream_mask != 1UL ||
        backings->required_user_memory_stream_mask != 1UL ||
        backings->populated_stream_mask != 1UL ||
        backings->streams[0].kind !=
            QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL) return FALSE;
    return TRUE;
}

BOOL QxpD3d9DrawCaptureIsSurfaceEligible(
    BYTE opcode, BOOL indexed,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings)
{
    if (!prepared || !backings) return FALSE;
    if (indexed) return FALSE;
    if (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
        opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) return FALSE;
    if (prepared->required_stream_mask != 1UL ||
        prepared->required_user_memory_stream_mask != 0 ||
        backings->required_stream_mask != 1UL ||
        backings->required_user_memory_stream_mask != 0 ||
        backings->populated_stream_mask != 1UL ||
        !qxp_capture_linear_kind(backings->streams[0].kind)) return FALSE;
    return TRUE;
}

void QxpD3d9DestroyCapturedDraw(
    void *blob, const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    if (!blob || !allocator || !allocator->release) return;
    allocator->release(blob, allocator->opaque);
}
