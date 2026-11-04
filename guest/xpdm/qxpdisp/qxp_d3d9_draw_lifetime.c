#include <windef.h>

#include "qxp_d3d9_draw_lifetime.h"

/* Lifetime gate: the production inspector admits source readers, while
 * teardown and writable Unlock publication in directdraw.c are live mutation
 * callers.  It performs no allocation, capture, storage mutation, or
 * scheduler wait. */

static void qxp_lifetime_zero_reader(
    QXP_D3D9_DRAW_LIFETIME_READER *reader)
{
    if (!reader) return;
    reader->epoch = 0;
    reader->active = 0;
}

static ULONG qxp_lifetime_current_epoch(
    const QXP_D3D9_DRAW_LIFETIME_GATE *gate)
{
    return gate->epoch_minus_one + 1UL;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeAcquireReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque,
    QXP_D3D9_DRAW_LIFETIME_READER *reader)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    qxp_lifetime_zero_reader(reader);
    if (!gate || !lock_callback || !unlock_callback || !reader)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    if (gate->epoch_exhausted || gate->epoch_minus_one == ~0UL) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED;
    } else if (gate->closed) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED;
    } else if (gate->mutation_pending) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_PENDING;
    } else if (gate->mutation_active) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE;
    } else if (gate->reader_count == ~0UL) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_READER_COUNT_EXHAUSTED;
    } else {
        ++gate->reader_count;
        reader->epoch = qxp_lifetime_current_epoch(gate);
        reader->active = 1;
    }
    unlock_callback(callback_opaque);
    return result;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeReleaseReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    if (!gate || !reader || !lock_callback || !unlock_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    if (!reader->active || !reader->epoch) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_READER_NOT_ACTIVE;
    } else if (!gate->reader_count) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_READER_COUNT_EMPTY;
    } else {
        --gate->reader_count;
        qxp_lifetime_zero_reader(reader);
    }
    unlock_callback(callback_opaque);
    return result;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeRevalidateReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    const QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    if (!gate || !reader || !lock_callback || !unlock_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    if (!reader->active || !reader->epoch) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_READER_NOT_ACTIVE;
    } else if (gate->epoch_exhausted || gate->epoch_minus_one == ~0UL) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED;
    } else if (gate->closed) {
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED;
    } else if (reader->epoch != qxp_lifetime_current_epoch(gate)) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_EPOCH_MISMATCH;
    }
    unlock_callback(callback_opaque);
    return result;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeBeginMutation(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    QXP_D3D9_DRAW_LIFETIME_YIELD_CALLBACK yield_callback,
    void *callback_opaque)
{
    ULONG reserved;

    if (!gate || !lock_callback || !unlock_callback || !yield_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    /* Reserve first, then drain.  Bounding the pending+active sum here is what
     * makes the later promotion overflow-safe, because promotion only moves
     * one unit from pending to active and never changes that sum. */
    reserved = 0;
    lock_callback(callback_opaque);
    if (gate->closed_permanent) {
        /* Readers are already refused forever; no reservation is needed. */
    } else if (gate->mutation_pending >= ~0UL - gate->mutation_active) {
        /* A reservation would wrap.  Refusing here would let this teardown
         * clear storage without draining, so fail closed instead: refuse
         * readers permanently and still drain below. */
        gate->closed = 1;
        gate->closed_permanent = 1;
    } else {
        ++gate->mutation_pending;
        reserved = 1;
    }
    unlock_callback(callback_opaque);

    for (;;) {
        lock_callback(callback_opaque);
        if (!gate->reader_count) {
            if (reserved) {
                --gate->mutation_pending;
                ++gate->mutation_active;
            }
            unlock_callback(callback_opaque);
            return reserved ?
                QXP_D3D9_DRAW_LIFETIME_OK :
                QXP_D3D9_DRAW_LIFETIME_OK_GATE_PERMANENTLY_CLOSED;
        }
        unlock_callback(callback_opaque);
        yield_callback(callback_opaque);
    }
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeEndMutation(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    if (!gate || !lock_callback || !unlock_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    if (!gate->mutation_active) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_MUTATION_NOT_ACTIVE;
    } else {
        --gate->mutation_active;
    }
    unlock_callback(callback_opaque);
    return result;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeClose(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    if (!gate || !lock_callback || !unlock_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    gate->closed = 1;
    if (gate->epoch_exhausted)
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED;
    unlock_callback(callback_opaque);
    return result;
}

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeReopen(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque)
{
    QXP_D3D9_DRAW_LIFETIME_RESULT result;

    if (!gate || !lock_callback || !unlock_callback)
        return QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    result = QXP_D3D9_DRAW_LIFETIME_OK;
    lock_callback(callback_opaque);
    if (gate->closed_permanent) {
        /* The fail-closed valve is never reopened. */
        gate->closed = 1;
        result = QXP_D3D9_DRAW_LIFETIME_OK_GATE_PERMANENTLY_CLOSED;
    } else if (gate->epoch_exhausted) {
        gate->closed = 1;
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED;
    } else if (!gate->closed) {
        result = QXP_D3D9_DRAW_LIFETIME_ERROR_GATE_ALREADY_OPEN;
    } else if (gate->epoch_minus_one >= ~0UL - 1UL) {
        gate->closed = 1;
        gate->epoch_exhausted = 1;
        result = QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED;
    } else {
        ++gate->epoch_minus_one;
        gate->closed = 0;
    }
    unlock_callback(callback_opaque);
    return result;
}
