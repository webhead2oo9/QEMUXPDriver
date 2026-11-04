#ifndef QXP_D3D9_DRAW_LIFETIME_H
#define QXP_D3D9_DRAW_LIFETIME_H

#include <windef.h>

/*
 * Lifetime gate for surface-backed draw capture.  It owns no allocation or
 * pointer.  The production inspector admits source readers, while teardown
 * and writable Unlock publication in directdraw.c are live mutation callers.
 * A zero-initialized gate is open at epoch 1; epoch_minus_one keeps that
 * initial state byte-zero while every epoch exposed to a reader remains
 * nonzero.
 */

typedef enum qxp_d3d9_draw_lifetime_result {
    QXP_D3D9_DRAW_LIFETIME_OK = 0,
    QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED = 2,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED = 3,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_PENDING = 4,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE = 5,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_READER_COUNT_EXHAUSTED = 6,
    QXP_D3D9_DRAW_LIFETIME_ERROR_READER_NOT_ACTIVE = 7,
    QXP_D3D9_DRAW_LIFETIME_ERROR_READER_COUNT_EMPTY = 8,
    QXP_D3D9_DRAW_LIFETIME_ERROR_EPOCH_MISMATCH = 9,
    QXP_D3D9_DRAW_LIFETIME_ERROR_MUTATION_NOT_ACTIVE = 10,
    QXP_D3D9_DRAW_LIFETIME_ERROR_GATE_ALREADY_OPEN = 11,
    QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_COUNT_EXHAUSTED = 12,
    QXP_D3D9_DRAW_LIFETIME_OK_GATE_PERMANENTLY_CLOSED = 13
} QXP_D3D9_DRAW_LIFETIME_RESULT;

/* Pointer-free scalar state.  All members are changed only while the caller's
 * injected lock is held.  epoch_exhausted is permanent once set.
 * mutation_pending and mutation_active are counts, not flags: independent
 * teardowns run concurrently, so excluding a second mutation would let it
 * proceed without draining.  closed_permanent is the fail-closed valve taken
 * when a reservation would wrap; it is never cleared, so readers stay refused
 * forever and no later teardown can run unprotected. */
typedef struct qxp_d3d9_draw_lifetime_gate {
    ULONG epoch_minus_one;
    ULONG reader_count;
    ULONG mutation_pending;
    ULONG mutation_active;
    ULONG closed;
    ULONG closed_permanent;
    ULONG epoch_exhausted;
} QXP_D3D9_DRAW_LIFETIME_GATE;

/* One caller-owned reader admission.  The recorded epoch is stable until the
 * admission is released and can be revalidated after a copy. */
typedef struct qxp_d3d9_draw_lifetime_reader {
    ULONG epoch;
    ULONG active;
} QXP_D3D9_DRAW_LIFETIME_READER;

typedef void (*QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK)(void *opaque);
typedef void (*QXP_D3D9_DRAW_LIFETIME_YIELD_CALLBACK)(void *opaque);

/* Attempts admission exactly once under the injected lock.  It never invokes
 * a yield callback and never waits.  A failed admission leaves reader zero. */
QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeAcquireReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque,
    QXP_D3D9_DRAW_LIFETIME_READER *reader);

/* Releases one successful admission.  Release remains valid after a
 * close/reopen cycle so an old reader can always drain the global count. */
QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeReleaseReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque);

/* Proves that an admitted reader still belongs to the current open epoch. */
QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeRevalidateReader(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    const QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque);

/* Gives mutations priority by publishing pending before draining readers.
 * Each nonzero observation drops the injected lock before one yield callback.
 * The callback must be a bounded processor-spin pause; this module never uses
 * a scheduler wait.  Mutations may overlap and are counted, because two owners
 * can tear surfaces down at the same time and every one of them must drain.
 * Counting keeps EndMutation unambiguous without an owner pointer or an
 * allocated token.  Readers stay refused until the last mutation ends.
 *
 * Begin never fails open.  A caller that reaches storage mutation must be
 * protected, so on every successful return the reader count is zero and
 * readers are refused, either because this mutation is counted active (OK, and
 * the caller must call EndMutation exactly once) or because a reservation
 * would have wrapped and the gate was permanently closed instead
 * (OK_GATE_PERMANENTLY_CLOSED, and the caller must not call EndMutation).
 * Only a null argument is rejected, and that rejects before any state change. */
QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeBeginMutation(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    QXP_D3D9_DRAW_LIFETIME_YIELD_CALLBACK yield_callback,
    void *callback_opaque);

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeEndMutation(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque);

/* Close only refuses future admission; storage changes still require a
 * draining mutation.  Reopen advances to a strictly greater epoch.  After
 * epoch ULONG_MAX has been used, reopen permanently exhausts and closes the
 * gate instead of wrapping to zero or reusing an epoch. */
QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeClose(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque);

QXP_D3D9_DRAW_LIFETIME_RESULT QxpD3d9DrawLifetimeReopen(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *callback_opaque);

#endif
