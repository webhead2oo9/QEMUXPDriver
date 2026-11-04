#include <windef.h>

#include "qxp_d3d9_draw_backing.h"

static void qxp_backing_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_backing_copy(
    void *destination, const void *source, ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static BOOL qxp_backing_equal(
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

static void qxp_backing_reset_status(QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    if (status) qxp_backing_zero(status, (ULONG)sizeof(*status));
}

static void qxp_backing_set_failure(
    QXP_D3D9_DRAW_BACKING_STATUS *status, DWORD code, DWORD stream,
    DWORD handle, DWORD role)
{
    if (!status) return;
    status->code = code;
    status->stream = stream;
    status->handle = handle;
    status->role = role;
}

/* Finite base extents are strictly bounded: a zero extent covers nothing and
 * the all-ones sentinel marks an unbounded (non-finite) backing. */
static BOOL qxp_backing_extent_is_finite(DWORD byte_length)
{
    return byte_length != 0 &&
           byte_length != QXP_D3D9_DRAW_BACKING_EXTENT_INFINITE;
}

static DWORD qxp_backing_role_bit(DWORD role)
{
    if (role == QXP_D3D9_DRAW_BACKING_ROLE_VERTEX)
        return QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX;
    if (role == QXP_D3D9_DRAW_BACKING_ROLE_INDEX)
        return QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
    return 0;
}

BOOL QxpD3d9ClassifyDrawBackingRoles(
    BOOL execute_buffer, BOOL vertex_buffer, BOOL index_buffer,
    DWORD *role_mask, DWORD *flags)
{
    if (!role_mask || !flags) return FALSE;
    *role_mask = 0;
    *flags = 0;
    if (vertex_buffer)
        *role_mask |= QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX;
    if (index_buffer)
        *role_mask |= QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
    if (!*role_mask && execute_buffer) {
        *role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
                     QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
        *flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    }
    return *role_mask != 0;
}

BOOL QxpD3d9ResolveLinearBufferExtent(
    LONG pitch, DWORD available_bytes, DWORD *byte_length)
{
    if (!byte_length) return FALSE;
    *byte_length = 0;
    if (pitch <= 0 ||
        (available_bytes && (DWORD)pitch > available_bytes)) return FALSE;
    *byte_length = (DWORD)pitch;
    return TRUE;
}

static BOOL qxp_backing_owner_equal(
    const QXP_D3D9_DRAW_OWNER_IDENTITY *left,
    const QXP_D3D9_DRAW_OWNER_IDENTITY *right)
{
    return left->context_handle == right->context_handle &&
           left->process_id == right->process_id &&
           left->device_identity == right->device_identity;
}

static BOOL qxp_backing_frequency_valid(DWORD stream, DWORD divider)
{
    DWORD flags;
    DWORD divisor;

    flags = divider & QXP_D3D9_STREAM_SOURCE_FLAG_MASK;
    divisor = divider & QXP_D3D9_STREAM_SOURCE_DIVISOR_MASK;
    if (!divider || flags == QXP_D3D9_STREAM_SOURCE_FLAG_MASK)
        return FALSE;
    if (flags == QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA)
        return stream != 0;
    if (flags == QXP_D3D9_STREAM_SOURCE_INDEXED_DATA)
        return TRUE;
    return flags == 0 && divisor != 0;
}

BOOL QxpD3d9AssignSurfaceIncarnation(
    ULONGLONG *clock, ULONGLONG existing, ULONGLONG *result,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    qxp_backing_reset_status(status);
    if (!clock || !result) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT, 0, 0, 0);
        return FALSE;
    }
    if (existing) {
        *result = existing;
        return TRUE;
    }
    if (*clock == ~(ULONGLONG)0) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_INCARNATION_EXHAUSTED,
            0, 0, 0);
        return FALSE;
    }
    *clock += 1;
    *result = *clock;
    return TRUE;
}

/* Validates one resolved backing's scalar metadata against the requested role
 * and the offset/span footprint.  Populates a candidate record on success. */
static BOOL qxp_backing_accept_metadata(
    const QXP_D3D9_DRAW_BACKING_METADATA *metadata, DWORD role,
    DWORD handle, DWORD offset, DWORD required_span, DWORD stream,
    QXP_D3D9_DRAW_BACKING_RECORD *record,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    if (metadata->valid != TRUE || metadata->handle != handle ||
        (metadata->role_mask & qxp_backing_role_bit(role)) == 0 ||
        (metadata->role_mask &
         ~(QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
           QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX)) != 0 ||
        (metadata->kind != QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
         metadata->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR) ||
        metadata->incarnation == 0 || metadata->content_generation == 0 ||
        !qxp_backing_extent_is_finite(metadata->byte_length) ||
        (metadata->flags & ~QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR) ||
        (metadata->flags &&
         (metadata->kind != QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR ||
          role != QXP_D3D9_DRAW_BACKING_ROLE_VERTEX))) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_METADATA, stream, handle,
            role);
        return FALSE;
    }
    /* A video-memory offset is meaningful only for a video-linear backing. */
    if ((metadata->kind != QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
         metadata->video_memory_offset != 0) ||
        (metadata->kind == QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
         metadata->video_memory_offset >
             0xffffffffUL - metadata->byte_length)) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_METADATA, stream, handle,
            role);
        return FALSE;
    }
    /* Overflow-safe base footprint: offset + span must stay within extent. */
    if (offset > metadata->byte_length ||
        required_span > metadata->byte_length - offset) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_OVERFLOW, stream, handle,
            role);
        return FALSE;
    }
    record->kind = metadata->kind;
    record->role = role;
    record->handle = handle;
    record->incarnation = metadata->incarnation;
    record->content_generation = metadata->content_generation;
    record->byte_length = metadata->byte_length;
    record->video_memory_offset =
        (metadata->kind == QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR) ?
            metadata->video_memory_offset : 0;
    record->flags = metadata->flags;
    return TRUE;
}

static BOOL qxp_backing_snapshot_locked(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *current_state,
    const QXP_D3D9_DRAW_BACKING_OWNER *resource_owner,
    BOOL indexed,
    const QXP_D3D9_DRAW_BACKING_UM_ENVELOPE *um_envelope,
    QXP_D3D9_DRAW_BACKING_LOOKUP_CALLBACK lookup, void *lookup_opaque,
    QXP_D3D9_DRAW_BACKING_SNAPSHOT *candidate,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    QXP_D3D9_DRAW_BACKING_METADATA metadata;
    DWORD required_mask;
    DWORD user_memory_mask;
    DWORD stream;

    /* Freshness: the live state must still byte-equal the prepared snapshot. */
    if (!qxp_backing_equal(
            current_state, &prepared->raw_state,
            (ULONG)sizeof(prepared->raw_state))) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED, 0, 0, 0);
        return FALSE;
    }

    candidate->owner.shader_owner.context_handle =
        resource_owner->shader_owner.context_handle;
    candidate->owner.shader_owner.process_id =
        resource_owner->shader_owner.process_id;
    candidate->owner.shader_owner.device_identity =
        resource_owner->shader_owner.device_identity;
    candidate->owner.dd_local_identity = resource_owner->dd_local_identity;
    candidate->stream_source_generation =
        current_state->stream_source_generation;
    required_mask = prepared->required_stream_mask;
    user_memory_mask = prepared->required_user_memory_stream_mask;
    candidate->required_stream_mask = required_mask;
    candidate->required_user_memory_stream_mask = user_memory_mask;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        const QXP_D3D9_DRAW_STREAM_STATE *source;
        QXP_D3D9_DRAW_BACKING_RECORD *record;
        DWORD required_span;
        BOOL want_user_memory;

        if (!(required_mask & (1UL << stream))) continue;
        source = &current_state->streams[stream];
        record = &candidate->streams[stream];
        required_span = prepared->required_stream_spans[stream];
        want_user_memory = (user_memory_mask & (1UL << stream)) ? TRUE : FALSE;

        record->stream = stream;
        record->role = QXP_D3D9_DRAW_BACKING_ROLE_VERTEX;
        record->stride = source->stride;
        record->source2_offset = source->offset;
        record->divider = source->divider;
        record->required_span = required_span;

        /* Recheck the execution-bearing canonical fields rather than trusting
         * the prepared valid bit.  Unrelated nonrequired streams remain
         * outside this bounded backing snapshot. */
        if ((source->user_memory != FALSE && source->user_memory != TRUE) ||
            (source->source2 != FALSE && source->source2 != TRUE) ||
            !required_span || !source->stride ||
            source->stride < required_span ||
            !qxp_backing_frequency_valid(stream, source->divider) ||
            (!source->source2 && source->offset)) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE, stream,
                source->handle, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
            return FALSE;
        }

        /* The prepared UM mask and the live user_memory flag must agree. */
        if (source->user_memory != want_user_memory) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE, stream,
                source->handle, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
            return FALSE;
        }

        if (want_user_memory) {
            DWORD bytes;

            /* User-memory streams bind only to stream 0 with fully canonical
             * state and are resolved through the observed input envelope. */
            if (stream != 0 || source->handle || source->offset ||
                source->source2) {
                qxp_backing_set_failure(
                    status, QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE, stream,
                    source->handle, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
                return FALSE;
            }
            if (um_envelope->active != TRUE ||
                um_envelope->nonnull_pointer_observed != TRUE ||
                um_envelope->address_range_valid != TRUE ||
                !um_envelope->vertex_size || !um_envelope->vertex_length ||
                um_envelope->vertex_length >
                    QXP_D3D9_DRAW_BACKING_EXTENT_INFINITE /
                        um_envelope->vertex_size) {
                qxp_backing_set_failure(
                    status, QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY, stream,
                    0, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
                return FALSE;
            }
            bytes = um_envelope->vertex_size * um_envelope->vertex_length;
            if (!qxp_backing_extent_is_finite(bytes) ||
                bytes < required_span) {
                qxp_backing_set_failure(
                    status, QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY, stream,
                    0, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
                return FALSE;
            }
            record->kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
            record->handle = 0;
            record->incarnation = 0;
            record->content_generation = 0;
            record->byte_length = bytes;
            record->video_memory_offset = 0;
            record->flags = 0;
            candidate->populated_stream_mask |= 1UL << stream;
            continue;
        }

        /* Canonical non-UM streams carry a nonzero surface handle. */
        if (!source->handle) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE, stream, 0,
                QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
            return FALSE;
        }
        qxp_backing_zero(&metadata, (ULONG)sizeof(metadata));
        if (!lookup(resource_owner, source->handle,
                    QXP_D3D9_DRAW_BACKING_ROLE_VERTEX, &metadata,
                    lookup_opaque)) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP, stream,
                source->handle, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
            return FALSE;
        }
        if (!qxp_backing_accept_metadata(
                &metadata, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX, source->handle,
                source->offset, required_span, stream, record, status)) {
            return FALSE;
        }
        /* A circular ring stays a stream-0, single-stream, non-SOURCE2
         * source.  An indexed draw may consume it: the XP D3D8 software-VP
         * runtime submits transformed user-memory geometry as
         * DRAWINDEXEDPRIMITIVE2 records over its internal circular execute
         * buffer (r532 evidence), and the declared index window unrolls
         * across the wrap through the same envelope machinery.  The index
         * buffer itself must remain linear. */
        if (record->flags &&
            (required_mask != 1UL || stream != 0 || source->source2)) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_METADATA, stream,
                source->handle, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
            return FALSE;
        }
        candidate->populated_stream_mask |= 1UL << stream;
    }

    if (indexed) {
        QXP_D3D9_DRAW_BACKING_RECORD *record;
        const QXP_D3D9_DRAW_INDEX_STATE *index_state;

        index_state = &current_state->indices;
        record = &candidate->index;
        if (!index_state->handle ||
            (index_state->stride != 2 && index_state->stride != 4)) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_INDEX_STATE, 0,
                index_state->handle, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
            return FALSE;
        }
        record->stream = 0;
        record->role = QXP_D3D9_DRAW_BACKING_ROLE_INDEX;
        record->stride = index_state->stride;
        record->required_span = index_state->stride;
        qxp_backing_zero(&metadata, (ULONG)sizeof(metadata));
        if (!lookup(resource_owner, index_state->handle,
                    QXP_D3D9_DRAW_BACKING_ROLE_INDEX, &metadata,
                    lookup_opaque)) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP, 0,
                index_state->handle, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
            return FALSE;
        }
        if (!qxp_backing_accept_metadata(
                &metadata, QXP_D3D9_DRAW_BACKING_ROLE_INDEX,
                index_state->handle, 0, 0, 0, record, status)) {
            return FALSE;
        }
        if (record->flags) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_METADATA, 0,
                index_state->handle, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
            return FALSE;
        }
        if (record->byte_length < index_state->stride) {
            qxp_backing_set_failure(
                status, QXP_D3D9_DRAW_BACKING_ERROR_EXTENT, 0,
                index_state->handle, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
            return FALSE;
        }
        candidate->indexed = TRUE;
    }

    candidate->valid = TRUE;
    return TRUE;
}

BOOL QxpD3d9SnapshotDrawBackings(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *current_state,
    const QXP_D3D9_DRAW_BACKING_OWNER *resource_owner,
    BOOL indexed,
    const QXP_D3D9_DRAW_BACKING_UM_ENVELOPE *um_envelope,
    QXP_D3D9_DRAW_BACKING_LOOKUP_CALLBACK lookup, void *lookup_opaque,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock, void *lock_opaque,
    QXP_D3D9_DRAW_BACKING_SNAPSHOT *out,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    QXP_D3D9_DRAW_BACKING_SNAPSHOT candidate;
    BOOL locked_result;

    qxp_backing_reset_status(status);
    if (out) qxp_backing_zero(out, (ULONG)sizeof(*out));
    if (!prepared || !current_state || !resource_owner || !um_envelope ||
        !lookup || !lock || !unlock || !out) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT, 0, 0, 0);
        return FALSE;
    }
    if (prepared->valid != TRUE ||
        (prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
         prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS) ||
        (indexed != FALSE && indexed != TRUE) ||
        (prepared->required_stream_mask & ~0xffffUL) ||
        (prepared->required_user_memory_stream_mask &
         ~prepared->required_stream_mask) ||
        (prepared->required_user_memory_stream_mask & ~1UL)) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE, 0, 0, 0);
        return FALSE;
    }
    if (!resource_owner->dd_local_identity ||
        !qxp_backing_owner_equal(
            &resource_owner->shader_owner, &prepared->owner)) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_OWNER, 0, 0, 0);
        return FALSE;
    }

    qxp_backing_zero(&candidate, (ULONG)sizeof(candidate));
    lock(lock_opaque);
    locked_result = qxp_backing_snapshot_locked(
        prepared, current_state, resource_owner, indexed, um_envelope,
        lookup, lookup_opaque, &candidate, status);
    unlock(lock_opaque);
    if (!locked_result) {
        qxp_backing_zero(out, (ULONG)sizeof(*out));
        return FALSE;
    }
    qxp_backing_copy(out, &candidate, (ULONG)sizeof(*out));
    return TRUE;
}

BOOL QxpD3d9ValidateDrawBackings(
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *snapshot,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *current_state,
    const QXP_D3D9_DRAW_BACKING_OWNER *resource_owner,
    BOOL indexed,
    const QXP_D3D9_DRAW_BACKING_UM_ENVELOPE *um_envelope,
    QXP_D3D9_DRAW_BACKING_LOOKUP_CALLBACK lookup, void *lookup_opaque,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock, void *lock_opaque,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    QXP_D3D9_DRAW_BACKING_SNAPSHOT reconstructed;

    qxp_backing_reset_status(status);
    if (!snapshot) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT, 0, 0, 0);
        return FALSE;
    }
    if (!QxpD3d9SnapshotDrawBackings(
            prepared, current_state, resource_owner, indexed, um_envelope,
            lookup, lookup_opaque, lock, unlock, lock_opaque,
            &reconstructed, status)) {
        return FALSE;
    }
    if (!qxp_backing_equal(
            snapshot, &reconstructed, (ULONG)sizeof(*snapshot))) {
        qxp_backing_reset_status(status);
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION, 0, 0, 0);
        return FALSE;
    }
    return TRUE;
}

BOOL QxpD3d9TeardownDrawBackingOwner(
    ULONG_PTR dd_local_identity,
    const QXP_D3D9_DRAW_BACKING_OWNER_TEARDOWN_CALLBACKS *callbacks,
    DWORD *texture_names, DWORD texture_capacity, DWORD *texture_count,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock, void *lock_opaque,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    QXP_D3D9_DRAW_BACKING_OWNER_SURFACE surface;
    DWORD surface_index;
    DWORD context_index;
    DWORD collected;
    BOOL callback_failed;
    BOOL capacity_failed;

    qxp_backing_reset_status(status);
    if (texture_count) *texture_count = 0;
    if (!dd_local_identity || !callbacks || !callbacks->read_surface ||
        !callbacks->detach_context || !callbacks->clear_surface ||
        !texture_count || !lock || !unlock ||
        (texture_capacity && !texture_names)) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT, 0, 0, 0);
        return FALSE;
    }

    collected = 0;
    callback_failed = FALSE;
    capacity_failed = FALSE;
    lock(lock_opaque);
    for (surface_index = 0;
         surface_index < callbacks->surface_count; ++surface_index) {
        qxp_backing_zero(&surface, (ULONG)sizeof(surface));
        if (!callbacks->read_surface(
                surface_index, &surface, callbacks->opaque)) {
            callback_failed = TRUE;
            break;
        }
        if ((surface.valid != FALSE && surface.valid != TRUE) ||
            (surface.valid && (!surface.dd_local_identity ||
             !surface.surface_identity || !surface.incarnation))) {
            callback_failed = TRUE;
            break;
        }
        if (!surface.valid ||
            surface.dd_local_identity != dd_local_identity) continue;

        for (context_index = 0;
             context_index < callbacks->context_count; ++context_index) {
            callbacks->detach_context(
                context_index, &surface, callbacks->opaque);
        }
        if (surface.texture_name) {
            if (texture_capacity) {
                if (collected < texture_capacity) {
                    texture_names[collected++] = surface.texture_name;
                } else {
                    capacity_failed = TRUE;
                }
            }
        }
        callbacks->clear_surface(surface_index, callbacks->opaque);
    }
    unlock(lock_opaque);
    *texture_count = collected;

    if (callback_failed) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN, 0, 0, 0);
        return FALSE;
    }
    if (capacity_failed) {
        qxp_backing_set_failure(
            status, QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN_CAPACITY,
            0, 0, 0);
        return FALSE;
    }
    return TRUE;
}

BOOL QxpD3d9UnbindOwnerContextResource(
    ULONG_PTR teardown_dd_local_identity,
    ULONG_PTR context_dd_local_identity,
    DWORD handle,
    const QXP_D3D9_LEGACY_RESOURCE_VIEW *legacy,
    QXP_D3D9_DRAW_STATE *draw_state)
{
    DWORD stream;

    if (!teardown_dd_local_identity ||
        teardown_dd_local_identity != context_dd_local_identity)
        return TRUE;
    if (!handle) return TRUE;
    if (!legacy || !draw_state || !legacy->stream_handles ||
        !legacy->stream_strides || !legacy->stream0_user_memory ||
        !legacy->index_handle || !legacy->index_stride ||
        legacy->stream_count > QXP_D3D9_DRAW_BACKING_STREAM_COUNT)
        return FALSE;

    for (stream = 0; stream < legacy->stream_count; ++stream) {
        if (legacy->stream_handles[stream] != handle) continue;
        legacy->stream_handles[stream] = 0;
        legacy->stream_strides[stream] = 0;
        if (stream == 0) *legacy->stream0_user_memory = FALSE;
    }
    if (*legacy->index_handle == handle) {
        *legacy->index_handle = 0;
        *legacy->index_stride = 0;
    }
    QxpD3d9DrawStateUnbindSurface(draw_state, handle);
    return TRUE;
}

BOOL QxpD3d9TextureNameFirstOccurrence(
    const DWORD *texture_names, DWORD texture_count, DWORD index)
{
    DWORD earlier;

    if (!texture_names || index >= texture_count ||
        !texture_names[index]) return FALSE;
    for (earlier = 0; earlier < index; ++earlier) {
        if (texture_names[earlier] == texture_names[index]) return FALSE;
    }
    return TRUE;
}

BOOL QxpD3d9DrawBackingStatusIsResourceFailure(
    const QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    return status &&
           status->code == QXP_D3D9_DRAW_BACKING_ERROR_INCARNATION_EXHAUSTED;
}
