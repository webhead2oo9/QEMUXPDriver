#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>

#include "qxpdisp/qxp_d3d9_draw_lifetime.h"

#define QXP_RELEASE_SLOTS 3UL
#define QXP_OBSERVATION_SLOTS 8UL

typedef struct qxp_test_lock_state {
    ULONG acquires;
    ULONG releases;
    ULONG depth;
    ULONG maximum_depth;
    ULONG violations;
    ULONG yields;
    ULONG yields_while_locked;
    ULONG pending_observations;
    ULONG active_with_readers;
    ULONG observed_reader_counts[QXP_OBSERVATION_SLOTS];
    QXP_D3D9_DRAW_LIFETIME_GATE *gate;
    QXP_D3D9_DRAW_LIFETIME_READER *release_readers[QXP_RELEASE_SLOTS];
    ULONG release_yields[QXP_RELEASE_SLOTS];
    ULONG release_failures;
    ULONG attempt_nested_begin;
    ULONG nested_begin_attempted;
    ULONG max_pending_observed;
    QXP_D3D9_DRAW_LIFETIME_RESULT nested_begin_result;
    ULONG attempt_pending_reader;
    ULONG pending_reader_attempted;
    ULONG pending_reader_yield_delta;
    QXP_D3D9_DRAW_LIFETIME_RESULT pending_reader_result;
    QXP_D3D9_DRAW_LIFETIME_READER pending_reader;
} QXP_TEST_LOCK_STATE;

static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_lifetime: failed: %s\n", name);
}

static void qxp_lock(void *opaque)
{
    QXP_TEST_LOCK_STATE *state;

    state = (QXP_TEST_LOCK_STATE *)opaque;
    ++state->acquires;
    if (state->depth) ++state->violations;
    ++state->depth;
    if (state->depth > state->maximum_depth)
        state->maximum_depth = state->depth;
}

static void qxp_unlock(void *opaque)
{
    QXP_TEST_LOCK_STATE *state;

    state = (QXP_TEST_LOCK_STATE *)opaque;
    ++state->releases;
    if (state->depth != 1UL) {
        ++state->violations;
        if (!state->depth) return;
    }
    --state->depth;
}

static void qxp_yield(void *opaque)
{
    QXP_TEST_LOCK_STATE *state;
    QXP_D3D9_DRAW_LIFETIME_RESULT result;
    ULONG yields_before;
    ULONG index;

    state = (QXP_TEST_LOCK_STATE *)opaque;
    if (state->depth) ++state->yields_while_locked;
    ++state->yields;
    if (state->yields > 32UL) {
        fprintf(stderr, "qxp_d3d9_draw_lifetime: drain did not converge\n");
        exit(2);
    }
    if (state->gate) {
        if (state->gate->mutation_pending &&
            !state->gate->mutation_active)
            ++state->pending_observations;
        if (state->gate->mutation_active && state->gate->reader_count)
            ++state->active_with_readers;
        if (state->gate->mutation_pending > state->max_pending_observed)
            state->max_pending_observed = state->gate->mutation_pending;
        if (state->yields <= QXP_OBSERVATION_SLOTS) {
            state->observed_reader_counts[state->yields - 1UL] =
                state->gate->reader_count;
        }
    }

    /* One-shot nested begin.  Mutations are counted, so this second teardown
     * reserves and drains behind the very same outstanding reader as the outer
     * one, which is the concurrency case counting exists for.  The one-shot
     * guard bounds re-entry to depth two and keeps the interleaving
     * deterministic without threads. */
    if (state->attempt_nested_begin && !state->nested_begin_attempted) {
        state->nested_begin_attempted = 1;
        state->nested_begin_result =
            QxpD3d9DrawLifetimeBeginMutation(
                state->gate, qxp_lock, qxp_unlock, qxp_yield, state);
    }

    if (state->attempt_pending_reader &&
        !state->pending_reader_attempted) {
        state->pending_reader_attempted = 1;
        yields_before = state->yields;
        state->pending_reader_result =
            QxpD3d9DrawLifetimeAcquireReader(
                state->gate, qxp_lock, qxp_unlock, state,
                &state->pending_reader);
        state->pending_reader_yield_delta = state->yields - yields_before;
        if (state->pending_reader_result ==
                QXP_D3D9_DRAW_LIFETIME_OK) {
            result = QxpD3d9DrawLifetimeReleaseReader(
                state->gate, &state->pending_reader,
                qxp_lock, qxp_unlock, state);
            if (result != QXP_D3D9_DRAW_LIFETIME_OK)
                ++state->release_failures;
        }
    }
    /* Consume each scheduled release exactly once.  A nested drain re-enters
     * this callback and returns after state->yields has advanced, so matching
     * on the counter alone would release the same reader twice. */
    for (index = 0; index < QXP_RELEASE_SLOTS; ++index) {
        if (state->release_readers[index] &&
            state->release_yields[index] == state->yields) {
            QXP_D3D9_DRAW_LIFETIME_READER *releasing;

            releasing = state->release_readers[index];
            state->release_readers[index] = NULL;
            result = QxpD3d9DrawLifetimeReleaseReader(
                state->gate, releasing, qxp_lock, qxp_unlock, state);
            if (result != QXP_D3D9_DRAW_LIFETIME_OK)
                ++state->release_failures;
        }
    }
}

static void qxp_reset_lock_state(
    QXP_TEST_LOCK_STATE *state,
    QXP_D3D9_DRAW_LIFETIME_GATE *gate)
{
    memset(state, 0, sizeof(*state));
    state->gate = gate;
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_acquire(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeAcquireReader(
        gate, qxp_lock, qxp_unlock, state, reader);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_release(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeReleaseReader(
        gate, reader, qxp_lock, qxp_unlock, state);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_revalidate(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    const QXP_D3D9_DRAW_LIFETIME_READER *reader,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeRevalidateReader(
        gate, reader, qxp_lock, qxp_unlock, state);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_begin(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeBeginMutation(
        gate, qxp_lock, qxp_unlock, qxp_yield, state);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_end(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeEndMutation(
        gate, qxp_lock, qxp_unlock, state);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_close(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeClose(
        gate, qxp_lock, qxp_unlock, state);
}

static QXP_D3D9_DRAW_LIFETIME_RESULT qxp_reopen(
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_TEST_LOCK_STATE *state)
{
    return QxpD3d9DrawLifetimeReopen(
        gate, qxp_lock, qxp_unlock, state);
}

static int qxp_reader_zero(
    const QXP_D3D9_DRAW_LIFETIME_READER *reader)
{
    QXP_D3D9_DRAW_LIFETIME_READER zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(reader, &zero, sizeof(zero)) == 0;
}

static void qxp_test_single_reader_drain_and_priority(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE initial;
    QXP_D3D9_DRAW_LIFETIME_READER reader;
    QXP_D3D9_DRAW_LIFETIME_READER refused;
    QXP_TEST_LOCK_STATE state;
    QXP_D3D9_DRAW_LIFETIME_RESULT result;
    ULONG yields_before;

    memset(&gate, 0, sizeof(gate));
    memset(&initial, 0, sizeof(initial));
    memset(&reader, 0, sizeof(reader));
    memset(&refused, 0xa5, sizeof(refused));
    qxp_reset_lock_state(&state, &gate);

    result = qxp_acquire(&gate, &reader, &state);
    qxp_check(result == QXP_D3D9_DRAW_LIFETIME_OK &&
                  reader.active == 1UL && reader.epoch == 1UL &&
                  gate.reader_count == 1UL,
              "zero gate admits one reader at nonzero epoch one");

    state.attempt_pending_reader = 1;
    state.release_readers[0] = &reader;
    state.release_yields[0] = 3UL;
    result = qxp_begin(&gate, &state);
    qxp_check(result == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_pending == 0UL &&
                  gate.mutation_active == 1UL &&
                  gate.reader_count == 0UL,
              "mutation activates only after outstanding reader releases");
    qxp_check(state.yields == 3UL &&
                  state.pending_observations == 3UL &&
                  state.observed_reader_counts[0] == 1UL &&
                  state.observed_reader_counts[1] == 1UL &&
                  state.observed_reader_counts[2] == 1UL,
              "single-reader drain spins until callback releases reader");
    qxp_check(state.active_with_readers == 0UL,
              "mutation never becomes active while reader is outstanding");
    qxp_check(state.pending_reader_attempted == 1UL &&
                  state.pending_reader_result ==
                      QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_PENDING &&
                  qxp_reader_zero(&state.pending_reader) &&
                  state.pending_reader_yield_delta == 0UL,
              "pending mutation refuses reader immediately without yield");
    qxp_check(state.yields_while_locked == 0UL &&
                  state.release_failures == 0UL,
              "drain invokes every yield outside injected lock");

    yields_before = state.yields;
    result = qxp_acquire(&gate, &refused, &state);
    qxp_check(result ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE &&
                  qxp_reader_zero(&refused) &&
                  state.yields == yields_before,
              "active mutation refuses reader immediately without yield");

    qxp_check(qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK,
              "active mutation ends successfully");
    qxp_check(memcmp(&gate, &initial, sizeof(gate)) == 0,
              "balanced reader and mutation restore whole zero gate");
    qxp_check(qxp_reader_zero(&reader),
              "balanced reader release restores whole zero token");
    qxp_check(state.depth == 0UL && state.violations == 0UL &&
                  state.acquires == state.releases,
              "single-reader injected lock calls balance");
}

static void qxp_test_multiple_readers_drain_last(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE initial;
    QXP_D3D9_DRAW_LIFETIME_READER readers[3];
    QXP_TEST_LOCK_STATE state;
    ULONG index;

    memset(&gate, 0, sizeof(gate));
    memset(&initial, 0, sizeof(initial));
    memset(readers, 0, sizeof(readers));
    qxp_reset_lock_state(&state, &gate);

    for (index = 0; index < 3UL; ++index) {
        qxp_check(qxp_acquire(&gate, &readers[index], &state) ==
                      QXP_D3D9_DRAW_LIFETIME_OK,
                  "concurrent reader admission succeeds");
        state.release_readers[index] = &readers[index];
    }
    state.release_yields[0] = 1UL;
    state.release_yields[1] = 2UL;
    state.release_yields[2] = 4UL;
    qxp_check(gate.reader_count == 3UL,
              "three concurrent readers are counted");
    qxp_check(qxp_begin(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK,
              "mutation drains multiple readers");
    qxp_check(state.yields == 4UL &&
                  state.observed_reader_counts[0] == 3UL &&
                  state.observed_reader_counts[1] == 2UL &&
                  state.observed_reader_counts[2] == 1UL &&
                  state.observed_reader_counts[3] == 1UL &&
                  gate.reader_count == 0UL &&
                  gate.mutation_active == 1UL,
              "mutation waits for final of multiple readers");
    qxp_check(state.active_with_readers == 0UL &&
                  state.yields_while_locked == 0UL &&
                  state.release_failures == 0UL,
              "multi-reader drain stays pending and unlocked");
    qxp_check(qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  memcmp(&gate, &initial, sizeof(gate)) == 0,
              "multi-reader drain and mutation restore initial gate");
}

static void qxp_test_close_reopen_and_revalidation(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_READER old_reader;
    QXP_D3D9_DRAW_LIFETIME_READER new_reader;
    QXP_D3D9_DRAW_LIFETIME_READER refused;
    QXP_TEST_LOCK_STATE state;
    ULONG old_epoch;
    ULONG yields_before;

    memset(&gate, 0, sizeof(gate));
    memset(&old_reader, 0, sizeof(old_reader));
    memset(&new_reader, 0, sizeof(new_reader));
    memset(&refused, 0xa5, sizeof(refused));
    qxp_reset_lock_state(&state, &gate);

    qxp_check(qxp_acquire(&gate, &old_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK,
              "old-epoch reader admitted");
    old_epoch = old_reader.epoch;
    qxp_check(qxp_close(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK && gate.closed == 1UL,
              "close marks gate closed");
    yields_before = state.yields;
    qxp_check(qxp_acquire(&gate, &refused, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED &&
                  qxp_reader_zero(&refused) &&
                  state.yields == yields_before,
              "closed gate refuses reader immediately");
    qxp_check(qxp_revalidate(&gate, &old_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED,
              "closed gate fails reader revalidation");

    qxp_check(qxp_reopen(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK && !gate.closed,
              "reopen succeeds from closed state");
    qxp_check(qxp_acquire(&gate, &new_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  new_reader.epoch > old_epoch && new_reader.epoch != 0UL,
              "reopen issues strictly greater nonzero epoch");
    qxp_check(qxp_revalidate(&gate, &old_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_EPOCH_MISMATCH,
              "old reader detects close and reopen epoch cycle");
    qxp_check(qxp_revalidate(&gate, &new_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK,
              "current-epoch reader revalidates");
    qxp_check(qxp_reopen(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_GATE_ALREADY_OPEN &&
                  gate.epoch_minus_one + 1UL == new_reader.epoch,
              "reopen cannot cycle an already open gate");
    qxp_check(qxp_release(&gate, &old_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  qxp_release(&gate, &new_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.reader_count == 0UL,
              "old and current epoch readers both release");
}

static void qxp_test_epoch_exhaustion_is_permanent(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE exhausted;
    QXP_D3D9_DRAW_LIFETIME_READER last_reader;
    QXP_D3D9_DRAW_LIFETIME_READER refused;
    QXP_TEST_LOCK_STATE state;
    ULONG yields_before;

    memset(&gate, 0, sizeof(gate));
    memset(&last_reader, 0, sizeof(last_reader));
    memset(&refused, 0xa5, sizeof(refused));
    gate.epoch_minus_one = ~0UL - 1UL;
    qxp_reset_lock_state(&state, &gate);

    qxp_check(qxp_acquire(&gate, &last_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  last_reader.epoch == ~0UL,
              "last representable nonzero epoch remains usable");
    qxp_check(qxp_release(&gate, &last_reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  qxp_close(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK,
              "last epoch closes after its reader releases");
    qxp_check(qxp_reopen(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED &&
                  gate.closed == 1UL && gate.epoch_exhausted == 1UL &&
                  gate.epoch_minus_one == ~0UL - 1UL,
              "epoch overflow permanently closes without wrapping");
    exhausted = gate;

    yields_before = state.yields;
    qxp_check(qxp_acquire(&gate, &refused, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED &&
                  qxp_reader_zero(&refused) &&
                  state.yields == yields_before,
              "exhausted gate refuses admission immediately");
    qxp_check(qxp_reopen(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED &&
                  memcmp(&gate, &exhausted, sizeof(gate)) == 0,
              "repeated reopen cannot reuse exhausted epoch");
    qxp_check(qxp_close(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_EPOCH_EXHAUSTED &&
                  memcmp(&gate, &exhausted, sizeof(gate)) == 0,
              "exhaustion remains permanent across close");
}

static void qxp_test_invalid_arguments_preserve_state(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE initial;
    QXP_D3D9_DRAW_LIFETIME_READER reader;
    QXP_TEST_LOCK_STATE state;
    QXP_D3D9_DRAW_LIFETIME_RESULT error;

    memset(&gate, 0, sizeof(gate));
    memset(&initial, 0, sizeof(initial));
    memset(&reader, 0, sizeof(reader));
    qxp_reset_lock_state(&state, &gate);
    error = QXP_D3D9_DRAW_LIFETIME_ERROR_NULL_ARGUMENT;

    qxp_check(QxpD3d9DrawLifetimeAcquireReader(
                  NULL, qxp_lock, qxp_unlock, &state, &reader) == error &&
                  QxpD3d9DrawLifetimeAcquireReader(
                  &gate, NULL, qxp_unlock, &state, &reader) == error &&
                  QxpD3d9DrawLifetimeAcquireReader(
                  &gate, qxp_lock, NULL, &state, &reader) == error &&
                  QxpD3d9DrawLifetimeAcquireReader(
                  &gate, qxp_lock, qxp_unlock, &state, NULL) == error,
              "reader acquire rejects every null required argument");
    qxp_check(QxpD3d9DrawLifetimeReleaseReader(
                  NULL, &reader, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeReleaseReader(
                  &gate, NULL, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeReleaseReader(
                  &gate, &reader, NULL, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeReleaseReader(
                  &gate, &reader, qxp_lock, NULL, &state) == error,
              "reader release rejects every null required argument");
    qxp_check(QxpD3d9DrawLifetimeRevalidateReader(
                  NULL, &reader, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeRevalidateReader(
                  &gate, NULL, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeRevalidateReader(
                  &gate, &reader, NULL, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeRevalidateReader(
                  &gate, &reader, qxp_lock, NULL, &state) == error,
              "reader revalidate rejects every null required argument");
    qxp_check(QxpD3d9DrawLifetimeBeginMutation(
                  NULL, qxp_lock, qxp_unlock, qxp_yield, &state) == error &&
                  QxpD3d9DrawLifetimeBeginMutation(
                  &gate, NULL, qxp_unlock, qxp_yield, &state) == error &&
                  QxpD3d9DrawLifetimeBeginMutation(
                  &gate, qxp_lock, NULL, qxp_yield, &state) == error &&
                  QxpD3d9DrawLifetimeBeginMutation(
                  &gate, qxp_lock, qxp_unlock, NULL, &state) == error,
              "mutation begin rejects every null required argument");
    qxp_check(QxpD3d9DrawLifetimeEndMutation(
                  NULL, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeEndMutation(
                  &gate, NULL, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeEndMutation(
                  &gate, qxp_lock, NULL, &state) == error,
              "mutation end rejects every null required argument");
    qxp_check(QxpD3d9DrawLifetimeClose(
                  NULL, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeClose(
                  &gate, NULL, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeClose(
                  &gate, qxp_lock, NULL, &state) == error &&
                  QxpD3d9DrawLifetimeReopen(
                  NULL, qxp_lock, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeReopen(
                  &gate, NULL, qxp_unlock, &state) == error &&
                  QxpD3d9DrawLifetimeReopen(
                  &gate, qxp_lock, NULL, &state) == error,
              "close and reopen reject every null required argument");
    qxp_check(memcmp(&gate, &initial, sizeof(gate)) == 0 &&
                  state.acquires == 0UL && state.releases == 0UL &&
                  state.yields == 0UL,
              "null arguments never lock, yield, or touch gate state");

    reader.epoch = 1UL;
    reader.active = 1UL;
    qxp_check(qxp_release(&gate, &reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_READER_COUNT_EMPTY &&
                  gate.reader_count == 0UL && reader.active == 1UL,
              "inconsistent reader release fails closed without underflow");
    memset(&reader, 0, sizeof(reader));
    qxp_check(qxp_release(&gate, &reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_READER_NOT_ACTIVE,
              "inactive reader release is deterministic");
    qxp_check(qxp_revalidate(&gate, &reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_READER_NOT_ACTIVE,
              "inactive reader revalidation is deterministic");
    qxp_check(qxp_end(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_MUTATION_NOT_ACTIVE,
              "unbalanced mutation end is deterministic");
    gate.reader_count = ~0UL;
    qxp_check(qxp_acquire(&gate, &reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_READER_COUNT_EXHAUSTED &&
                  gate.reader_count == ~0UL && qxp_reader_zero(&reader),
              "reader count overflow fails closed without state change");
}

/* Two owners can tear surfaces down at the same time, so mutations must be
 * counted rather than exclusive.  Were a second begin refused, that teardown
 * would proceed without ever draining outstanding capture readers. */
static void qxp_test_counted_mutations(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE initial;
    QXP_D3D9_DRAW_LIFETIME_READER refused;
    QXP_TEST_LOCK_STATE state;
    ULONG yields_before;

    memset(&gate, 0, sizeof(gate));
    memset(&initial, 0, sizeof(initial));
    memset(&refused, 0, sizeof(refused));
    qxp_reset_lock_state(&state, &gate);

    qxp_check(qxp_begin(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_active == 1UL,
              "first mutation activates with no readers outstanding");
    yields_before = state.yields;
    qxp_check(qxp_begin(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_active == 2UL &&
                  gate.mutation_pending == 0UL &&
                  state.yields == yields_before,
              "concurrent second mutation is admitted and counted");

    qxp_check(qxp_acquire(&gate, &refused, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE &&
                  qxp_reader_zero(&refused),
              "readers stay refused while two mutations are active");

    qxp_check(qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_active == 1UL,
              "ending one mutation leaves the other active");
    qxp_check(qxp_acquire(&gate, &refused, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_MUTATION_ACTIVE &&
                  qxp_reader_zero(&refused),
              "readers stay refused until the last mutation ends");

    qxp_check(qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_active == 0UL,
              "ending the last mutation clears the count");
    qxp_check(qxp_end(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_ERROR_MUTATION_NOT_ACTIVE,
              "an unmatched end is rejected without underflow");
    qxp_check(memcmp(&gate, &initial, sizeof(gate)) == 0,
              "balanced counted mutations restore whole zero gate");
    qxp_check(state.depth == 0UL && state.violations == 0UL,
              "counted mutation injected lock calls balance");
}

/* The case counting exists for: two teardowns pending behind one outstanding
 * reader.  Both must drain, and neither may activate while the reader lives. */
static void qxp_test_two_mutations_pend_behind_one_reader(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_GATE initial;
    QXP_D3D9_DRAW_LIFETIME_READER reader;
    QXP_TEST_LOCK_STATE state;

    memset(&gate, 0, sizeof(gate));
    memset(&initial, 0, sizeof(initial));
    memset(&reader, 0, sizeof(reader));
    qxp_reset_lock_state(&state, &gate);

    qxp_check(qxp_acquire(&gate, &reader, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.reader_count == 1UL,
              "reader is admitted before either teardown begins");

    state.attempt_nested_begin = 1;
    state.release_readers[0] = &reader;
    state.release_yields[0] = 3UL;

    qxp_check(qxp_begin(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK,
              "outer teardown drains and activates");
    qxp_check(state.nested_begin_attempted == 1UL &&
                  state.nested_begin_result == QXP_D3D9_DRAW_LIFETIME_OK,
              "second teardown is admitted while the first is pending");
    qxp_check(state.max_pending_observed == 2UL,
              "both teardowns are pending behind the reader at once");
    qxp_check(gate.mutation_active == 2UL && gate.mutation_pending == 0UL,
              "both teardowns end up counted active");
    qxp_check(state.active_with_readers == 0UL,
              "no teardown activates while the reader is outstanding");
    qxp_check(state.yields_while_locked == 0UL &&
                  state.release_failures == 0UL,
              "nested drain still yields outside the injected lock");

    qxp_check(qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  qxp_end(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK,
              "both teardowns end successfully");
    qxp_check(memcmp(&gate, &initial, sizeof(gate)) == 0,
              "two pending teardowns restore whole zero gate");
    qxp_check(state.depth == 0UL && state.violations == 0UL,
              "nested teardown injected lock calls balance");
}

/* A reservation that would wrap must never let a teardown proceed unprotected.
 * The gate fails closed instead: readers are refused permanently. */
static void qxp_test_reservation_exhaustion_fails_closed(void)
{
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_LIFETIME_READER refused;
    QXP_TEST_LOCK_STATE state;

    memset(&gate, 0, sizeof(gate));
    memset(&refused, 0, sizeof(refused));
    qxp_reset_lock_state(&state, &gate);

    /* One reservation below the ceiling: this must still succeed and promote
     * without wrapping the active count. */
    gate.mutation_active = ~0UL - 1UL;
    qxp_check(qxp_begin(&gate, &state) == QXP_D3D9_DRAW_LIFETIME_OK &&
                  gate.mutation_active == ~0UL &&
                  gate.mutation_pending == 0UL,
              "final reservation promotes without wrapping");

    /* At the ceiling the reservation would wrap. */
    qxp_check(qxp_begin(&gate, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK_GATE_PERMANENTLY_CLOSED &&
                  gate.mutation_active == ~0UL &&
                  gate.mutation_pending == 0UL,
              "exhausted reservation neither wraps nor fails open");
    qxp_check(gate.closed_permanent == 1UL && gate.closed == 1UL,
              "exhausted reservation closes the gate permanently");
    qxp_check(qxp_acquire(&gate, &refused, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_REFUSED_CLOSED &&
                  qxp_reader_zero(&refused),
              "permanently closed gate refuses every later reader");
    qxp_check(QxpD3d9DrawLifetimeReopen(
                  &gate, qxp_lock, qxp_unlock, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK_GATE_PERMANENTLY_CLOSED &&
                  gate.closed == 1UL,
              "a permanently closed gate is never reopened");
    qxp_check(state.depth == 0UL && state.violations == 0UL,
              "exhaustion path injected lock calls balance");
}

int main(void)
{
    qxp_test_single_reader_drain_and_priority();
    qxp_test_counted_mutations();
    qxp_test_two_mutations_pend_behind_one_reader();
    qxp_test_reservation_exhaustion_fails_closed();
    qxp_test_multiple_readers_drain_last();
    qxp_test_close_reopen_and_revalidation();
    qxp_test_epoch_exhaustion_is_permanent();
    qxp_test_invalid_arguments_preserve_state();

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw lifetime tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw lifetime tests: PASS\n");
    return 0;
}
