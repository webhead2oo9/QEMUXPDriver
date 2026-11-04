#include <windef.h>

#include "qxp_d3d9_draw_source.h"

typedef char qxp_draw_source_stream_count_is_sixteen[
    QXP_D3D9_DRAW_BACKING_STREAM_COUNT == 16UL ? 1 : -1];

static void qxp_draw_source_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static BOOL qxp_draw_source_linear_kind(DWORD kind)
{
    return kind == QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR ||
           kind == QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_draw_source_map_admission(
    QXP_D3D9_DRAW_LIFETIME_RESULT result)
{
    if (result == QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED)
        return QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_CLOSED;
    if (result == QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED)
        return QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_EPOCH_EXHAUSTED;
    if (result == QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_PENDING)
        return QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_PENDING;
    if (result == QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE)
        return QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_ACTIVE;
    if (result ==
            QXP_D3D9_DRAW_LIFETIME_REFUSED_READER_COUNT_EXHAUSTED)
        return QXP_D3D9_DRAW_SOURCE_REFUSED_READER_COUNT_EXHAUSTED;
    return QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_ADMISSION;
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_draw_source_release_active(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT lifetime_result;

    lifetime_result = QxpD3d9DrawLifetimeReleaseReader(
        token->gate, &token->reader, token->lock_callback,
        token->unlock_callback, token->lock_opaque);
    qxp_draw_source_zero(token, (ULONG)sizeof(*token));
    if (lifetime_result != QXP_D3D9_DRAW_LIFETIME_OK)
        return QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE;
    return QXP_D3D9_DRAW_SOURCE_OK;
}

/* All post-admission failures converge here.  This is the only internal path
 * that drops a failed transaction's reader, so every such path releases once
 * and token zeroing makes a later public release a harmless NOT_ACTIVE. */
static QXP_D3D9_DRAW_SOURCE_RESULT qxp_draw_source_fail_active(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token,
    QXP_D3D9_DRAW_SOURCE_RESULT failure)
{
    QXP_D3D9_DRAW_SOURCE_RESULT release_result;

    release_result = qxp_draw_source_release_active(token);
    if (release_result != QXP_D3D9_DRAW_SOURCE_OK) return release_result;
    return failure;
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_draw_source_validate_acquire(
    const QXP_D3D9_DRAW_BACKING_RECORD *expected,
    const QXP_D3D9_DRAW_SOURCE_OBSERVED *observed,
    DWORD *usable_byte_count)
{
    DWORD required_end;

    if (observed->kind != expected->kind)
        return QXP_D3D9_DRAW_SOURCE_ERROR_KIND_MISMATCH;
    if (!qxp_draw_source_linear_kind(observed->kind))
        return QXP_D3D9_DRAW_SOURCE_ERROR_BACKING_KIND;
    if (observed->host_authoritative)
        return QXP_D3D9_DRAW_SOURCE_ERROR_HOST_AUTHORITATIVE;
    if (observed->lock_active)
        return QXP_D3D9_DRAW_SOURCE_ERROR_LOCK_ACTIVE;
    if (observed->handle != expected->handle)
        return QXP_D3D9_DRAW_SOURCE_ERROR_HANDLE_MISMATCH;
    if (observed->incarnation != expected->incarnation)
        return QXP_D3D9_DRAW_SOURCE_ERROR_INCARNATION_MISMATCH;
    if (observed->content_generation != expected->content_generation)
        return QXP_D3D9_DRAW_SOURCE_ERROR_CONTENT_GENERATION_MISMATCH;
    if (observed->byte_length != expected->byte_length)
        return QXP_D3D9_DRAW_SOURCE_ERROR_BYTE_LENGTH_MISMATCH;
    if (observed->video_memory_offset != expected->video_memory_offset)
        return QXP_D3D9_DRAW_SOURCE_ERROR_VIDEO_MEMORY_OFFSET_MISMATCH;
    if (observed->flags != expected->flags)
        return QXP_D3D9_DRAW_SOURCE_ERROR_FLAGS_MISMATCH;
    if (!observed->surface_identity)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_SURFACE_IDENTITY;
    if (!observed->base_address)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_BASE;
    if (!observed->byte_count)
        return QXP_D3D9_DRAW_SOURCE_ERROR_ZERO_BYTE_COUNT;
    if (expected->source2_offset >
            0xffffffffUL - expected->required_span)
        return QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT_OVERFLOW;
    required_end = expected->source2_offset + expected->required_span;
    *usable_byte_count = observed->byte_count;
    if (*usable_byte_count > observed->byte_length)
        *usable_byte_count = observed->byte_length;
    if (required_end > observed->byte_length ||
        required_end > *usable_byte_count)
        return QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT;
    return QXP_D3D9_DRAW_SOURCE_OK;
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_draw_source_compare_observation(
    const QXP_D3D9_DRAW_SOURCE_OBSERVED *expected,
    const QXP_D3D9_DRAW_SOURCE_OBSERVED *observed)
{
    if (observed->kind != expected->kind)
        return QXP_D3D9_DRAW_SOURCE_ERROR_KIND_MISMATCH;
    if (!qxp_draw_source_linear_kind(observed->kind))
        return QXP_D3D9_DRAW_SOURCE_ERROR_BACKING_KIND;
    if (observed->host_authoritative)
        return QXP_D3D9_DRAW_SOURCE_ERROR_HOST_AUTHORITATIVE;
    if (observed->lock_active)
        return QXP_D3D9_DRAW_SOURCE_ERROR_LOCK_ACTIVE;
    if (observed->handle != expected->handle)
        return QXP_D3D9_DRAW_SOURCE_ERROR_HANDLE_MISMATCH;
    if (observed->incarnation != expected->incarnation)
        return QXP_D3D9_DRAW_SOURCE_ERROR_INCARNATION_MISMATCH;
    if (observed->content_generation != expected->content_generation)
        return QXP_D3D9_DRAW_SOURCE_ERROR_CONTENT_GENERATION_MISMATCH;
    if (observed->byte_length != expected->byte_length)
        return QXP_D3D9_DRAW_SOURCE_ERROR_BYTE_LENGTH_MISMATCH;
    if (observed->video_memory_offset != expected->video_memory_offset)
        return QXP_D3D9_DRAW_SOURCE_ERROR_VIDEO_MEMORY_OFFSET_MISMATCH;
    if (observed->flags != expected->flags)
        return QXP_D3D9_DRAW_SOURCE_ERROR_FLAGS_MISMATCH;
    if (!observed->surface_identity)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_SURFACE_IDENTITY;
    if (observed->surface_identity != expected->surface_identity)
        return QXP_D3D9_DRAW_SOURCE_ERROR_SURFACE_IDENTITY_MISMATCH;
    if (!observed->base_address)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_BASE;
    if (observed->base_address != expected->base_address)
        return QXP_D3D9_DRAW_SOURCE_ERROR_BASE_ADDRESS_MISMATCH;
    if (!observed->byte_count)
        return QXP_D3D9_DRAW_SOURCE_ERROR_ZERO_BYTE_COUNT;
    if (observed->byte_count != expected->byte_count)
        return QXP_D3D9_DRAW_SOURCE_ERROR_BASE_BYTE_COUNT_MISMATCH;
    return QXP_D3D9_DRAW_SOURCE_OK;
}

QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9AcquireDrawSource(
    const QXP_D3D9_DRAW_BACKING_RECORD *expected,
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_SOURCE_RESOLVE_CALLBACK resolve,
    void *resolve_opaque,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT lifetime_result;
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    BOOL resolved;
    DWORD usable_byte_count;

    if (token) qxp_draw_source_zero(token, (ULONG)sizeof(*token));
    if (!expected || !gate || !resolve || !lock_callback ||
        !unlock_callback || !token)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT;

    token->gate = gate;
    token->resolve = resolve;
    token->resolve_opaque = resolve_opaque;
    token->lock_callback = lock_callback;
    token->unlock_callback = unlock_callback;
    token->lock_opaque = lock_opaque;
    lifetime_result = QxpD3d9DrawLifetimeAcquireReader(
        gate, lock_callback, unlock_callback, lock_opaque, &token->reader);
    if (lifetime_result != QXP_D3D9_DRAW_LIFETIME_OK) {
        result = qxp_draw_source_map_admission(lifetime_result);
        qxp_draw_source_zero(token, (ULONG)sizeof(*token));
        return result;
    }

    lock_callback(lock_opaque);
    resolved = resolve(expected->handle, &token->observed, resolve_opaque);
    unlock_callback(lock_opaque);
    if (!resolved)
        return qxp_draw_source_fail_active(
            token, QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE);

    usable_byte_count = 0;
    result = qxp_draw_source_validate_acquire(
        expected, &token->observed, &usable_byte_count);
    if (result != QXP_D3D9_DRAW_SOURCE_OK)
        return qxp_draw_source_fail_active(token, result);

    token->base_address = token->observed.base_address;
    token->byte_count = usable_byte_count;
    token->gate_epoch = token->reader.epoch;
    return QXP_D3D9_DRAW_SOURCE_OK;
}

QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9RevalidateDrawSource(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    QXP_D3D9_DRAW_SOURCE_OBSERVED observed;
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    QXP_D3D9_DRAW_LIFETIME_RESULT lifetime_result;
    BOOL resolved;

    if (!token) return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT;
    if (!token->reader.active || !token->reader.epoch || !token->gate ||
        !token->resolve || !token->lock_callback || !token->unlock_callback)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NOT_ACTIVE;

    qxp_draw_source_zero(&observed, (ULONG)sizeof(observed));
    token->lock_callback(token->lock_opaque);
    resolved = token->resolve(
        token->observed.handle, &observed, token->resolve_opaque);
    token->unlock_callback(token->lock_opaque);
    if (!resolved)
        return qxp_draw_source_fail_active(
            token, QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE);

    result = qxp_draw_source_compare_observation(
        &token->observed, &observed);
    if (result != QXP_D3D9_DRAW_SOURCE_OK)
        return qxp_draw_source_fail_active(token, result);

    lifetime_result = QxpD3d9DrawLifetimeRevalidateReader(
        token->gate, &token->reader, token->lock_callback,
        token->unlock_callback, token->lock_opaque);
    if (lifetime_result != QXP_D3D9_DRAW_LIFETIME_OK)
        return qxp_draw_source_fail_active(
            token, QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_REVALIDATION);
    return QXP_D3D9_DRAW_SOURCE_OK;
}

QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9ReleaseDrawSource(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    if (!token) return QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT;
    if (!token->reader.active || !token->reader.epoch || !token->gate ||
        !token->lock_callback || !token->unlock_callback)
        return QXP_D3D9_DRAW_SOURCE_ERROR_NOT_ACTIVE;
    return qxp_draw_source_release_active(token);
}

static void qxp_draw_source_set_reset_status(
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status)
{
    if (status) qxp_draw_source_zero(status, (ULONG)sizeof(*status));
}

static void qxp_draw_source_set_primary_failure(
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status,
    QXP_D3D9_DRAW_SOURCE_RESULT result, DWORD stream)
{
    if (!status || status->result != QXP_D3D9_DRAW_SOURCE_OK) return;
    status->result = (DWORD)result;
    status->stream = stream;
}

static void qxp_draw_source_set_cleanup_failure(
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status,
    QXP_D3D9_DRAW_SOURCE_RESULT result, DWORD stream)
{
    if (!status || status->cleanup_result != QXP_D3D9_DRAW_SOURCE_OK)
        return;
    status->cleanup_result = (DWORD)result;
    status->cleanup_stream = stream;
}

/* Always visits the selected active tokens in reverse order.  cleanup selects
 * the secondary status fields used while aborting a different primary
 * operation; explicit release reports its first failure as primary. */
static BOOL qxp_draw_source_set_release_active(
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status, BOOL cleanup)
{
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    DWORD stream;
    BOOL success;

    success = TRUE;
    stream = QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
    while (stream) {
        --stream;
        if (!(set->active_stream_mask & (1UL << stream))) continue;
        result = QxpD3d9ReleaseDrawSource(&set->tokens[stream]);
        set->active_stream_mask &= ~(1UL << stream);
        if (result == QXP_D3D9_DRAW_SOURCE_OK) continue;
        success = FALSE;
        if (cleanup) {
            qxp_draw_source_set_cleanup_failure(status, result, stream);
        } else {
            qxp_draw_source_set_primary_failure(status, result, stream);
        }
    }
    return success;
}

BOOL QxpD3d9AcquireDrawSourceSet(
    const QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT],
    DWORD stream_mask,
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_SOURCE_RESOLVE_CALLBACK resolve,
    void *resolve_opaque,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status)
{
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    DWORD stream;

    qxp_draw_source_set_reset_status(status);
    if (set) qxp_draw_source_zero(set, (ULONG)sizeof(*set));
    if (!expected || !gate || !resolve || !lock_callback ||
        !unlock_callback || !set || !status ||
        (stream_mask & ~QXP_D3D9_DRAW_SOURCE_STREAM_MASK)) {
        qxp_draw_source_set_primary_failure(
            status, QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT, 0);
        return FALSE;
    }

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (!(stream_mask & (1UL << stream))) continue;
        result = QxpD3d9AcquireDrawSource(
            &expected[stream], gate, resolve, resolve_opaque,
            lock_callback, unlock_callback, lock_opaque,
            &set->tokens[stream]);
        if (result != QXP_D3D9_DRAW_SOURCE_OK) {
            qxp_draw_source_set_primary_failure(status, result, stream);
            (void)qxp_draw_source_set_release_active(set, status, TRUE);
            return FALSE;
        }
        set->active_stream_mask |= 1UL << stream;
    }
    return TRUE;
}

BOOL QxpD3d9RevalidateDrawSourceSet(
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status)
{
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    DWORD stream;

    qxp_draw_source_set_reset_status(status);
    if (!set || !status) {
        qxp_draw_source_set_primary_failure(
            status, QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT, 0);
        return FALSE;
    }
    if (set->active_stream_mask &
            ~QXP_D3D9_DRAW_SOURCE_STREAM_MASK) {
        qxp_draw_source_set_primary_failure(
            status, QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT, 0);
        set->active_stream_mask &= QXP_D3D9_DRAW_SOURCE_STREAM_MASK;
        (void)qxp_draw_source_set_release_active(set, status, TRUE);
        return FALSE;
    }

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (!(set->active_stream_mask & (1UL << stream))) continue;
        result = QxpD3d9RevalidateDrawSource(&set->tokens[stream]);
        if (result != QXP_D3D9_DRAW_SOURCE_OK) {
            /* Every scalar revalidation failure that follows admission
             * releases and zeroes its own token.  Clear its membership before
             * releasing the complete remainder. */
            set->active_stream_mask &= ~(1UL << stream);
            qxp_draw_source_set_primary_failure(status, result, stream);
            (void)qxp_draw_source_set_release_active(set, status, TRUE);
            return FALSE;
        }
    }
    return TRUE;
}

BOOL QxpD3d9ReleaseDrawSourceSet(
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status)
{
    qxp_draw_source_set_reset_status(status);
    if (!set || !status) {
        qxp_draw_source_set_primary_failure(
            status, QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT, 0);
        return FALSE;
    }
    if (set->active_stream_mask &
            ~QXP_D3D9_DRAW_SOURCE_STREAM_MASK) {
        qxp_draw_source_set_primary_failure(
            status, QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT, 0);
        set->active_stream_mask &= QXP_D3D9_DRAW_SOURCE_STREAM_MASK;
        (void)qxp_draw_source_set_release_active(set, status, TRUE);
        return FALSE;
    }
    return qxp_draw_source_set_release_active(set, status, FALSE);
}

BOOL QxpD3d9DrawSourceSetStatusHasReleaseFailure(
    const QXP_D3D9_DRAW_SOURCE_SET_STATUS *status)
{
    return status &&
           (status->result ==
                QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE ||
            status->cleanup_result ==
                QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE);
}

static DWORD qxp_draw_source_owner_gate_generation;

static BOOL qxp_draw_source_owner_gate_next_generation(DWORD *generation)
{
    if (!generation || qxp_draw_source_owner_gate_generation == ~0UL)
        return FALSE;
    ++qxp_draw_source_owner_gate_generation;
    if (!qxp_draw_source_owner_gate_generation) return FALSE;
    *generation = qxp_draw_source_owner_gate_generation;
    return TRUE;
}

static BOOL qxp_draw_source_owner_gate_valid(
    const QXP_D3D9_DRAW_OWNER_GATE *gate)
{
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;
    DWORD count;

    if (!gate) return FALSE;
    if (!gate->global_identity) {
        return !gate->owner_identity &&
               !gate->teardown_thread_identity && !gate->generation &&
               !gate->teardown_users && !gate->revoking &&
               !gate->in_flight && !gate->operations;
    }
    if (!gate->generation || gate->teardown_users > 1UL ||
        gate->revoking > 1UL ||
        ((!gate->teardown_users) != (!gate->owner_identity)) ||
        ((!gate->teardown_users) !=
         (!gate->teardown_thread_identity)) ||
        ((!gate->in_flight) != (!gate->operations)) ||
        (gate->revoking && !gate->in_flight &&
         !gate->teardown_users)) return FALSE;
    count = 0;
    token = gate->operations;
    while (token) {
        if (count >= gate->in_flight || !token->global_identity ||
            token->global_identity != gate->global_identity ||
            !token->thread_identity ||
            token->generation != gate->generation || !token->slot ||
            (token->revoked &&
             !gate->teardown_users && !gate->revoking) ||
            (gate->revoking && !token->revoked)) return FALSE;
        ++count;
        token = token->next;
    }
    return count == gate->in_flight;
}

static BOOL qxp_draw_source_owner_gate_contains(
    const QXP_D3D9_DRAW_OWNER_GATE *gate,
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token)
{
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *current;

    if (!gate || !token) return FALSE;
    current = gate->operations;
    while (current) {
        if (current == token) return TRUE;
        current = current->next;
    }
    return FALSE;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateBeginOperation(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity, DWORD slot,
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token)
{
    DWORD generation;

    if (!token) return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    if (!gate || !global_identity || !thread_identity || !slot ||
        !qxp_draw_source_owner_gate_valid(gate)) {
        qxp_draw_source_zero(token, (ULONG)sizeof(*token));
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    if (qxp_draw_source_owner_gate_contains(gate, token))
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    qxp_draw_source_zero(token, (ULONG)sizeof(*token));
    if (gate->global_identity &&
        gate->global_identity != global_identity)
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    if (gate->teardown_users || gate->revoking)
        return QXP_D3D9_DRAW_OWNER_GATE_REFUSED;
    if (gate->in_flight == ~0UL)
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    if (!gate->global_identity) {
        generation = 0;
        if (!qxp_draw_source_owner_gate_next_generation(&generation))
            return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
        gate->global_identity = global_identity;
        gate->generation = generation;
    }
    token->owner_identity = owner_identity;
    token->global_identity = gate->global_identity;
    token->thread_identity = thread_identity;
    token->generation = gate->generation;
    token->slot = slot;
    token->next = gate->operations;
    gate->operations = token;
    ++gate->in_flight;
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateValidateOperation(
    const QXP_D3D9_DRAW_OWNER_GATE *gate,
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token)
{
    if (!qxp_draw_source_owner_gate_valid(gate) || !token ||
        !qxp_draw_source_owner_gate_contains(gate, token) ||
        token->global_identity != gate->global_identity ||
        token->generation != gate->generation) {
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    return token->revoked ? QXP_D3D9_DRAW_OWNER_GATE_REVOKED :
                            QXP_D3D9_DRAW_OWNER_GATE_OK;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateEndOperation(
    QXP_D3D9_DRAW_OWNER_GATE *gate,
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token)
{
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN **link;
    QXP_D3D9_DRAW_OWNER_GATE_RESULT result;

    if (!qxp_draw_source_owner_gate_valid(gate) || !token ||
        !token->slot || !token->global_identity ||
        token->global_identity != gate->global_identity ||
        token->generation != gate->generation) {
        qxp_draw_source_zero(token, token ? (ULONG)sizeof(*token) : 0);
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    link = &gate->operations;
    while (*link && *link != token) link = &(*link)->next;
    if (!*link) {
        qxp_draw_source_zero(token, (ULONG)sizeof(*token));
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    *link = token->next;
    --gate->in_flight;
    result = token->revoked ? QXP_D3D9_DRAW_OWNER_GATE_REVOKED :
                              QXP_D3D9_DRAW_OWNER_GATE_OK;
    qxp_draw_source_zero(token, (ULONG)sizeof(*token));
    if (!gate->in_flight && gate->revoking &&
        !gate->teardown_users) {
        qxp_draw_source_zero(gate, (ULONG)sizeof(*gate));
    }
    return result;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateBeginTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity)
{
    DWORD generation;

    if (!gate || !owner_identity || !global_identity || !thread_identity ||
        !qxp_draw_source_owner_gate_valid(gate)) {
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    if (gate->global_identity &&
        gate->global_identity != global_identity)
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    if (gate->teardown_users) {
        return gate->teardown_thread_identity == thread_identity ?
            QXP_D3D9_DRAW_OWNER_GATE_FATAL :
            QXP_D3D9_DRAW_OWNER_GATE_REFUSED;
    }
    if (!gate->global_identity) {
        generation = 0;
        if (!qxp_draw_source_owner_gate_next_generation(&generation))
            return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
        gate->global_identity = global_identity;
        gate->generation = generation;
    }
    gate->owner_identity = owner_identity;
    gate->teardown_thread_identity = thread_identity;
    gate->teardown_users = 1;
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

BOOL QxpD3d9DrawOwnerGateTeardownOperationCounts(
    const QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR thread_identity,
    DWORD *same_thread, DWORD *other_threads)
{
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;

    if (same_thread) *same_thread = 0;
    if (other_threads) *other_threads = 0;
    if (!same_thread || !other_threads || !thread_identity ||
        !qxp_draw_source_owner_gate_valid(gate) ||
        gate->teardown_users != 1 ||
        gate->teardown_thread_identity != thread_identity) return FALSE;
    token = gate->operations;
    while (token) {
        if (!token->revoked) {
            if (token->thread_identity == thread_identity)
                ++*same_thread;
            else
                ++*other_threads;
        }
        token = token->next;
    }
    return TRUE;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT
QxpD3d9DrawOwnerGateRevokeThreadOperations(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR thread_identity)
{
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;

    if (!thread_identity || !qxp_draw_source_owner_gate_valid(gate) ||
        gate->teardown_users != 1 ||
        gate->teardown_thread_identity != thread_identity) {
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    token = gate->operations;
    while (token) {
        if (!token->revoked &&
            token->thread_identity != thread_identity)
            return QXP_D3D9_DRAW_OWNER_GATE_REFUSED;
        token = token->next;
    }
    token = gate->operations;
    while (token) {
        if (token->thread_identity == thread_identity) {
            token->owner_identity = 0;
            token->revoked = TRUE;
        }
        token = token->next;
    }
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateCompleteTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity)
{
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;

    if (!owner_identity || !global_identity || !thread_identity ||
        !qxp_draw_source_owner_gate_valid(gate) ||
        gate->owner_identity != owner_identity ||
        gate->global_identity != global_identity ||
        gate->teardown_thread_identity != thread_identity ||
        gate->teardown_users != 1) {
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    token = gate->operations;
    while (token) {
        if (!token->revoked)
            return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
        token = token->next;
    }
    gate->owner_identity = 0;
    gate->teardown_thread_identity = 0;
    gate->teardown_users = 0;
    if (gate->in_flight) {
        gate->revoking = 1;
    } else {
        qxp_draw_source_zero(gate, (ULONG)sizeof(*gate));
    }
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT
QxpD3d9DrawOwnerGateForceCompleteTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity)
{
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;

    if (!gate || !owner_identity || !global_identity ||
        gate->owner_identity != owner_identity ||
        gate->global_identity != global_identity ||
        gate->teardown_users != 1) {
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    }
    token = gate->operations;
    while (token) {
        token->owner_identity = 0;
        token->revoked = TRUE;
        token = token->next;
    }
    gate->owner_identity = 0;
    gate->teardown_thread_identity = 0;
    gate->teardown_users = 0;
    if (gate->in_flight) {
        gate->revoking = 1;
    } else {
        qxp_draw_source_zero(gate, (ULONG)sizeof(*gate));
    }
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateFatalClose(
    QXP_D3D9_DRAW_OWNER_GATE *gate)
{
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token;

    if (!qxp_draw_source_owner_gate_valid(gate))
        return QXP_D3D9_DRAW_OWNER_GATE_FATAL;
    token = gate->operations;
    while (token) {
        token->owner_identity = 0;
        token->revoked = TRUE;
        token = token->next;
    }
    gate->owner_identity = 0;
    gate->teardown_thread_identity = 0;
    gate->teardown_users = 0;
    if (gate->in_flight) {
        gate->revoking = 1;
    } else {
        qxp_draw_source_zero(gate, (ULONG)sizeof(*gate));
    }
    return QXP_D3D9_DRAW_OWNER_GATE_OK;
}

BOOL QxpD3d9DrawOwnerGateBlocksIdentity(
    const QXP_D3D9_DRAW_OWNER_GATE *gate,
    ULONG_PTR owner_identity, ULONG_PTR global_identity)
{
    return gate && owner_identity && global_identity &&
           gate->teardown_users == 1 &&
           gate->owner_identity == owner_identity &&
           gate->global_identity == global_identity;
}

BOOL QxpD3d9DrawSourceLockCanReserve(
    DWORD matching_entries, DWORD active_entries,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns)
{
    if (active_entries > matching_entries ||
        unlock_reservations > matching_entries ||
        destroy_reservations > matching_entries ||
        owner_teardowns > matching_entries) return FALSE;
    return !matching_entries ||
           (!active_entries && !unlock_reservations &&
            !destroy_reservations && !owner_teardowns);
}

BOOL QxpD3d9DrawSourceUnlockCanReserve(
    DWORD matching_entries, DWORD lock_preparations,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns)
{
    if (lock_preparations > matching_entries ||
        unlock_reservations > matching_entries ||
        destroy_reservations > matching_entries ||
        owner_teardowns > matching_entries) return FALSE;
    return !matching_entries ||
           (!lock_preparations && !unlock_reservations &&
            !destroy_reservations && !owner_teardowns);
}

BOOL QxpD3d9DrawSourceDestroyCanReserve(
    DWORD matching_entries, DWORD active_entries,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns)
{
    if (matching_entries != 1 || active_entries > matching_entries ||
        unlock_reservations > matching_entries ||
        destroy_reservations > matching_entries ||
        owner_teardowns > matching_entries) return FALSE;
    return !active_entries && !unlock_reservations &&
           !destroy_reservations && !owner_teardowns;
}

BOOL QxpD3d9DrawSourceRegistrationCanProceed(
    DWORD active_entries, DWORD unlock_reservations,
    DWORD destroy_reservations, DWORD owner_teardowns)
{
    return !active_entries && !unlock_reservations &&
           !destroy_reservations && !owner_teardowns;
}

BOOL QxpD3d9DrawSourceOwnerTeardownIsDrained(
    DWORD owner_entries, DWORD owner_operations,
    DWORD lock_preparations, DWORD unlock_reservations,
    DWORD destroy_reservations)
{
    if (lock_preparations > owner_entries ||
        unlock_reservations > owner_entries ||
        destroy_reservations > owner_entries) return FALSE;
    return !owner_operations && !lock_preparations &&
           !unlock_reservations && !destroy_reservations;
}

BOOL QxpD3d9DrawSourceNextUnlockReservation(
    DWORD current, DWORD *next)
{
    if (!next || current == ~0UL) return FALSE;
    *next = current + 1UL;
    return *next != 0;
}

BOOL QxpD3d9CompleteDrawSourceUnlock(
    DWORD read_only_flag,
    const QXP_D3D9_DRAW_UNLOCK_CALLBACKS *callbacks,
    QXP_D3D9_DRAW_UNLOCK_RESULT *result)
{
    qxp_draw_source_zero(result, result ? (ULONG)sizeof(*result) : 0);
    if (!read_only_flag || !callbacks || !result ||
        !callbacks->reserve_lock || !callbacks->begin_mutation ||
        !callbacks->clear_old_lock || !callbacks->present ||
        !callbacks->publish_generation ||
        !callbacks->finish_reservation || !callbacks->end_mutation) {
        return FALSE;
    }
    if (!callbacks->reserve_lock(
            &result->lock_active, &result->lock_flags,
            callbacks->opaque)) {
        qxp_draw_source_zero(result, (ULONG)sizeof(*result));
        return FALSE;
    }
    result->reserved = TRUE;
    result->mutation_end_required =
        callbacks->begin_mutation(callbacks->opaque);
    callbacks->clear_old_lock(callbacks->opaque);
    result->present_result = callbacks->present(
        &result->matched, callbacks->opaque);
    result->generation_available = TRUE;
    if (result->lock_active && !result->matched &&
        !(result->lock_flags & read_only_flag)) {
        result->generation_available =
            callbacks->publish_generation(callbacks->opaque);
    }
    callbacks->finish_reservation(callbacks->opaque);
    if (result->mutation_end_required)
        callbacks->end_mutation(callbacks->opaque);
    return TRUE;
}
