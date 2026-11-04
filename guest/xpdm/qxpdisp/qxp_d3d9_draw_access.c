#include <windef.h>

#include "qxp_d3d9_draw_access.h"

/* Dormant B6G3A implementation: production-linked, but intentionally has no
 * live caller and performs no execution, pinning, copying, or revalidation. */

static void qxp_access_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_access_copy(
    void *destination, const void *source, ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static BOOL qxp_access_equal(
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

static void qxp_access_reset_status(QXP_D3D9_DRAW_ACCESS_STATUS *status)
{
    if (status) qxp_access_zero(status, (ULONG)sizeof(*status));
}

static BOOL qxp_access_fail(
    QXP_D3D9_DRAW_ACCESS_STATUS *status, DWORD code,
    DWORD record_index, DWORD index_ordinal, DWORD stream, DWORD field)
{
    if (status) {
        status->code = code;
        status->record_index = record_index;
        status->index_ordinal = index_ordinal;
        status->stream = stream;
        status->field = field;
    }
    return FALSE;
}

static BOOL qxp_access_add_u64(
    ULONGLONG left, ULONGLONG right, ULONGLONG *result)
{
    if (left > ~(ULONGLONG)0 - right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL qxp_access_mul_u64(
    ULONGLONG left, ULONGLONG right, ULONGLONG *result)
{
    if (left && right > ~(ULONGLONG)0 / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static WORD qxp_access_read_u16(const BYTE *bytes)
{
    DWORD value;

    value = (DWORD)bytes[0];
    value |= (DWORD)bytes[1] << 8;
    return (WORD)value;
}

static DWORD qxp_access_read_u32(const BYTE *bytes)
{
    DWORD value;

    value = (DWORD)bytes[0];
    value |= (DWORD)bytes[1] << 8;
    value |= (DWORD)bytes[2] << 16;
    value |= (DWORD)bytes[3] << 24;
    return value;
}

static LONGLONG qxp_access_signed_dword(DWORD value)
{
    DWORD magnitude;

    if (!(value & 0x80000000UL)) return (LONGLONG)value;
    magnitude = (~value) + 1UL;
    return -(LONGLONG)magnitude;
}

static BOOL qxp_access_source_count(
    DWORD primitive_type, DWORD primitive_count, DWORD *source_count)
{
    if (!primitive_count) {
        *source_count = 0;
        return primitive_type >= QXP_D3D9_PRIMITIVE_POINTLIST &&
               primitive_type <= QXP_D3D9_PRIMITIVE_TRIANGLEFAN;
    }
    switch (primitive_type) {
    case QXP_D3D9_PRIMITIVE_POINTLIST:
        *source_count = primitive_count;
        return TRUE;
    case QXP_D3D9_PRIMITIVE_LINELIST:
        if (primitive_count <= 0xffffffffUL / 2UL) {
            *source_count = primitive_count * 2UL;
            return TRUE;
        }
        break;
    case QXP_D3D9_PRIMITIVE_LINESTRIP:
        if (primitive_count <= 0xffffffffUL - 1UL) {
            *source_count = primitive_count + 1UL;
            return TRUE;
        }
        break;
    case QXP_D3D9_PRIMITIVE_TRIANGLELIST:
        if (primitive_count <= 0xffffffffUL / 3UL) {
            *source_count = primitive_count * 3UL;
            return TRUE;
        }
        break;
    case QXP_D3D9_PRIMITIVE_TRIANGLESTRIP:
    case QXP_D3D9_PRIMITIVE_TRIANGLEFAN:
        if (primitive_count <= 0xffffffffUL - 2UL) {
            *source_count = primitive_count + 2UL;
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static BOOL qxp_access_index_copy_valid(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    const void *index_bytes, ULONG index_byte_count,
    QXP_D3D9_DRAW_ACCESS_STATUS *status)
{
    ULONGLONG byte_count;

    if (!envelope->index.valid) {
        if (index_bytes || index_byte_count) {
            return qxp_access_fail(
                status, QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY,
                0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_COPY);
        }
        return TRUE;
    }
    if (envelope->index.byte_last_exclusive <
            envelope->index.byte_first) {
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
            0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE);
    }
    byte_count = envelope->index.byte_last_exclusive -
                 envelope->index.byte_first;
    if (!index_bytes || byte_count != (ULONGLONG)index_byte_count) {
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY,
            0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_COPY);
    }
    return TRUE;
}

static BOOL qxp_access_add_stream_window(
    QXP_D3D9_DRAW_ACCESS_PLAN *candidate,
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    DWORD record_index, ULONGLONG logical_first,
    ULONGLONG logical_last_inclusive,
    QXP_D3D9_DRAW_ACCESS_STATUS *status)
{
    ULONGLONG logical_last_exclusive;
    DWORD stream;

    if (!qxp_access_add_u64(
            logical_last_inclusive, (ULONGLONG)1,
            &logical_last_exclusive)) {
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_STREAM_RANGE,
            record_index, 0, 0,
            QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
    }

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
         ++stream) {
        const QXP_D3D9_DRAW_BACKING_RECORD *backing;
        const QXP_D3D9_DRAW_STREAM_ENVELOPE *broad;
        QXP_D3D9_DRAW_STREAM_ENVELOPE *exact;
        ULONGLONG byte_first;
        ULONGLONG byte_last;
        ULONGLONG product;
        ULONGLONG delta_first;
        ULONGLONG delta_last;

        if (!(candidate->stream_mask & (1UL << stream))) continue;
        backing = &backings->streams[stream];
        broad = &envelope->streams[stream];
        exact = &candidate->streams[stream];
        if (!broad->valid || broad->stream != stream ||
            logical_first < broad->logical_first ||
            logical_last_exclusive > broad->logical_last_exclusive) {
            return qxp_access_fail(
                status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                record_index, 0, stream,
                QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
        }
        if (backing->flags) {
            /* A circular ring is byte-phased: the envelope's broad window
             * already carries the physical start of its first vertex, so
             * anchor the exact window there in pitch units rather than
             * recomputing an absolute logical*stride address (r533/r534
             * evidence).  Both windows stay unrolled across the wrap. */
            if (!qxp_access_mul_u64(
                    logical_first - broad->logical_first,
                    (ULONGLONG)backing->stride, &delta_first) ||
                !qxp_access_mul_u64(
                    logical_last_inclusive - broad->logical_first,
                    (ULONGLONG)backing->stride, &delta_last)) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_STREAM_RANGE,
                    record_index, 0, stream,
                    QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
            }
            byte_first = broad->byte_first + delta_first;
            byte_last = broad->byte_first + delta_last +
                        (ULONGLONG)backing->required_span;
            if (byte_first < broad->byte_first ||
                byte_last > broad->byte_last_exclusive) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                    record_index, 0, stream,
                    QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
            }
        } else {
            if (!qxp_access_mul_u64(
                    logical_first, (ULONGLONG)backing->stride, &product) ||
                !qxp_access_add_u64(
                    (ULONGLONG)backing->source2_offset,
                    product, &byte_first) ||
                !qxp_access_mul_u64(
                    logical_last_inclusive,
                    (ULONGLONG)backing->stride, &product) ||
                !qxp_access_add_u64(
                    (ULONGLONG)backing->source2_offset,
                    product, &byte_last) ||
                !qxp_access_add_u64(
                    byte_last, (ULONGLONG)backing->required_span,
                    &byte_last) ||
                byte_last > (ULONGLONG)backing->byte_length) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_STREAM_RANGE,
                    record_index, 0, stream,
                    QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
            }
            if (byte_first < broad->byte_first ||
                byte_last > broad->byte_last_exclusive) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                    record_index, 0, stream,
                    QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES);
            }
        }
        if (!exact->valid) {
            exact->valid = TRUE;
            exact->stream = stream;
            exact->logical_first = logical_first;
            exact->logical_last_exclusive = logical_last_exclusive;
            exact->byte_first = byte_first;
            exact->byte_last_exclusive = byte_last;
        } else {
            if (logical_first < exact->logical_first)
                exact->logical_first = logical_first;
            if (logical_last_exclusive > exact->logical_last_exclusive)
                exact->logical_last_exclusive = logical_last_exclusive;
            if (byte_first < exact->byte_first)
                exact->byte_first = byte_first;
            if (byte_last > exact->byte_last_exclusive)
                exact->byte_last_exclusive = byte_last;
        }
    }
    return TRUE;
}

BOOL QxpD3d9BuildDrawAccess(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, ULONG index_byte_count,
    QXP_D3D9_DRAW_ACCESS_PLAN *out,
    QXP_D3D9_DRAW_ACCESS_STATUS *status)
{
    QXP_D3D9_DRAW_ACCESS_PLAN candidate;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    const BYTE *record_bytes;
    const BYTE *copy_bytes;
    ULONGLONG scanned_count;
    DWORD record_index;
    DWORD record_size;
    DWORD primitive_type_offset;
    DWORD base_vertex_offset;
    DWORD min_index_offset;
    DWORD num_vertices_offset;
    DWORD start_index_offset;
    DWORD primitive_count_offset;

    qxp_access_reset_status(status);
    if (out) qxp_access_zero(out, (ULONG)sizeof(*out));
    if (!envelope || !records || !prepared || !backings || !out || !status) {
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_NULL_ARGUMENT,
            0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_RECORDS);
    }

    qxp_access_zero(&envelope_status, (ULONG)sizeof(envelope_status));
    if (!QxpD3d9ValidateDrawEnvelope(
            envelope, opcode, record_count, records, available_bytes,
            prepared, backings, &envelope_status)) {
        qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE,
            envelope_status.record_index, 0, envelope_status.stream,
            QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE);
        qxp_access_copy(
            &status->envelope_error, &envelope_status,
            (ULONG)sizeof(status->envelope_error));
        return FALSE;
    }
    if (!qxp_access_index_copy_valid(
            envelope, index_bytes, index_byte_count, status)) return FALSE;

    qxp_access_zero(&candidate, (ULONG)sizeof(candidate));
    candidate.opcode = envelope->opcode;
    candidate.record_count = envelope->record_count;
    candidate.nonempty_record_count = envelope->nonempty_record_count;
    candidate.indexed = envelope->indexed;
    candidate.stream_mask = envelope->stream_mask;
    qxp_access_copy(
        &candidate.index, &envelope->index,
        (ULONG)sizeof(candidate.index));

    if (!envelope->indexed) {
        qxp_access_copy(
            candidate.streams, envelope->streams,
            (ULONG)sizeof(candidate.streams));
        candidate.valid = TRUE;
        qxp_access_copy(out, &candidate, (ULONG)sizeof(*out));
        return TRUE;
    }
    if (!envelope->index.valid) {
        candidate.valid = TRUE;
        qxp_access_copy(out, &candidate, (ULONG)sizeof(*out));
        return TRUE;
    }

    if (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) {
        record_size = QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2;
        primitive_type_offset =
            QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_TYPE;
        base_vertex_offset = QXP_D3D9_DRAWINDEXED2_OFFSET_BASE_VERTEX;
        min_index_offset = QXP_D3D9_DRAWINDEXED2_OFFSET_MIN_INDEX;
        num_vertices_offset = QXP_D3D9_DRAWINDEXED2_OFFSET_NUM_VERTICES;
        start_index_offset = QXP_D3D9_DRAWINDEXED2_OFFSET_START_INDEX;
        primitive_count_offset =
            QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_COUNT;
    } else {
        record_size = QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE;
        primitive_type_offset =
            QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_TYPE;
        base_vertex_offset = QXP_D3D9_DRAWINDEXED_OFFSET_BASE_VERTEX;
        min_index_offset = QXP_D3D9_DRAWINDEXED_OFFSET_MIN_INDEX;
        num_vertices_offset = QXP_D3D9_DRAWINDEXED_OFFSET_NUM_VERTICES;
        start_index_offset = QXP_D3D9_DRAWINDEXED_OFFSET_START_INDEX;
        primitive_count_offset =
            QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_COUNT;
    }
    record_bytes = (const BYTE *)records;
    copy_bytes = (const BYTE *)index_bytes;
    scanned_count = 0;
    for (record_index = 0; record_index < record_count; ++record_index) {
        const BYTE *record;
        DWORD primitive_type;
        DWORD primitive_count;
        DWORD source_count;
        DWORD base_value;
        DWORD min_index;
        DWORD num_vertices;
        DWORD start_index;
        DWORD ordinal;
        DWORD index_stride;
        LONGLONG signed_base;
        ULONGLONG index_first;
        ULONGLONG record_minimum;
        ULONGLONG record_maximum;
        BOOL saw_index;

        record = record_bytes + record_index * record_size;
        primitive_type = qxp_access_read_u32(
            record + primitive_type_offset);
        primitive_count = qxp_access_read_u32(
            record + primitive_count_offset);
        source_count = 0;
        if (!qxp_access_source_count(
                primitive_type, primitive_count, &source_count)) {
            return qxp_access_fail(
                status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                record_index, 0, 0,
                QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE);
        }
        if (!source_count) continue;

        base_value = qxp_access_read_u32(record + base_vertex_offset);
        min_index = qxp_access_read_u32(record + min_index_offset);
        num_vertices = qxp_access_read_u32(record + num_vertices_offset);
        start_index = qxp_access_read_u32(record + start_index_offset);
        index_stride = backings->index.stride;
        signed_base = qxp_access_signed_dword(base_value);
        if (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) {
            /* Floor toward negative infinity exactly like the envelope's
             * byte-phase normalization, so a negative unaligned base maps
             * scanned indices into the same logical window (r533). */
            LONGLONG phase;
            LONGLONG stride;

            stride = (LONGLONG)prepared->raw_state.streams[0].stride;
            phase = signed_base % stride;
            signed_base /= stride;
            if (phase < 0) {
                phase += stride;
                signed_base -= 1;
            }
            index_first = (ULONGLONG)start_index;
        } else {
            if (!qxp_access_mul_u64(
                    (ULONGLONG)start_index,
                    (ULONGLONG)index_stride, &index_first)) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                    record_index, 0, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE);
            }
        }

        record_minimum = 0;
        record_maximum = 0;
        saw_index = FALSE;
        for (ordinal = 0; ordinal < source_count; ++ordinal) {
            ULONGLONG ordinal_bytes;
            ULONGLONG absolute_byte;
            ULONGLONG byte_end;
            ULONGLONG copy_offset;
            ULONGLONG next_count;
            DWORD raw_index;
            LONGLONG logical_signed;
            ULONGLONG logical_index;

            if (!qxp_access_mul_u64(
                    (ULONGLONG)ordinal,
                    (ULONGLONG)index_stride, &ordinal_bytes) ||
                !qxp_access_add_u64(
                    index_first, ordinal_bytes, &absolute_byte) ||
                !qxp_access_add_u64(
                    absolute_byte, (ULONGLONG)index_stride,
                    &byte_end) ||
                absolute_byte < envelope->index.byte_first ||
                byte_end > envelope->index.byte_last_exclusive) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET,
                    record_index, ordinal, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE);
            }
            copy_offset = absolute_byte - envelope->index.byte_first;
            if (copy_offset > (ULONGLONG)index_byte_count ||
                (ULONGLONG)index_stride >
                    (ULONGLONG)index_byte_count - copy_offset) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY,
                    record_index, ordinal, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_COPY);
            }
            if (index_stride == 2) {
                raw_index = (DWORD)qxp_access_read_u16(
                    copy_bytes + (ULONG)copy_offset);
            } else {
                raw_index = qxp_access_read_u32(
                    copy_bytes + (ULONG)copy_offset);
            }
            if (raw_index < min_index ||
                raw_index - min_index >= num_vertices) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_VALUE,
                    record_index, ordinal, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_VALUE);
            }
            logical_signed = signed_base + (LONGLONG)raw_index;
            if (logical_signed < 0) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_LOGICAL_INDEX,
                    record_index, ordinal, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_BASE_VERTEX);
            }
            logical_index = (ULONGLONG)logical_signed;
            if (!saw_index) {
                record_minimum = logical_index;
                record_maximum = logical_index;
                saw_index = TRUE;
            } else {
                if (logical_index < record_minimum)
                    record_minimum = logical_index;
                if (logical_index > record_maximum)
                    record_maximum = logical_index;
            }
            if (!qxp_access_add_u64(
                    scanned_count, (ULONGLONG)1, &next_count)) {
                return qxp_access_fail(
                    status, QXP_D3D9_DRAW_ACCESS_ERROR_COUNT_OVERFLOW,
                    record_index, ordinal, 0,
                    QXP_D3D9_DRAW_ACCESS_FIELD_SCANNED_COUNT);
            }
            scanned_count = next_count;
        }
        if (saw_index && !qxp_access_add_stream_window(
                &candidate, envelope, backings, record_index,
                record_minimum, record_maximum, status)) return FALSE;
    }

    candidate.scanned_index_count.low = (DWORD)scanned_count;
    candidate.scanned_index_count.high = (DWORD)(scanned_count >> 32);
    candidate.valid = TRUE;
    qxp_access_copy(out, &candidate, (ULONG)sizeof(*out));
    return TRUE;
}

BOOL QxpD3d9ValidateDrawAccess(
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan,
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, ULONG index_byte_count,
    QXP_D3D9_DRAW_ACCESS_STATUS *status)
{
    QXP_D3D9_DRAW_ACCESS_PLAN reconstructed;

    qxp_access_reset_status(status);
    if (!plan) {
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_NULL_ARGUMENT,
            0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_OUTPUT);
    }
    if (!QxpD3d9BuildDrawAccess(
            envelope, opcode, record_count, records, available_bytes,
            prepared, backings, index_bytes, index_byte_count,
            &reconstructed, status)) return FALSE;
    if (!qxp_access_equal(
            plan, &reconstructed, (ULONG)sizeof(*plan))) {
        qxp_access_reset_status(status);
        return qxp_access_fail(
            status, QXP_D3D9_DRAW_ACCESS_ERROR_VALIDATION,
            0, 0, 0, QXP_D3D9_DRAW_ACCESS_FIELD_OUTPUT);
    }
    return TRUE;
}
