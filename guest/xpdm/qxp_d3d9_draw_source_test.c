#include <stdio.h>
#include <string.h>

#include <windef.h>

#include "qxpdisp/qxp_d3d9_draw_source.h"
#include "qxpdisp/qxp_d3d_texblt.h"

#define QXP_TEST_HANDLE 0x31415926UL
#define QXP_TEST_SURFACE_IDENTITY ((ULONG_PTR)0x2468ace0UL)
#define QXP_TEST_BYTE_LENGTH 64UL
#define QXP_TEST_READONLY 0x00000010UL

#define QXP_UNLOCK_EVENT_RESERVE 1UL
#define QXP_UNLOCK_EVENT_BEGIN 2UL
#define QXP_UNLOCK_EVENT_CLEAR 3UL
#define QXP_UNLOCK_EVENT_PRESENT 4UL
#define QXP_UNLOCK_EVENT_PUBLISH 5UL
#define QXP_UNLOCK_EVENT_FINISH 6UL
#define QXP_UNLOCK_EVENT_END 7UL

#define QXP_IDENTITY_HANDLE 0UL
#define QXP_IDENTITY_INCARNATION 1UL
#define QXP_IDENTITY_CONTENT_GENERATION 2UL
#define QXP_IDENTITY_KIND 3UL
#define QXP_IDENTITY_BYTE_LENGTH 4UL
#define QXP_IDENTITY_VIDEO_MEMORY_OFFSET 5UL
#define QXP_IDENTITY_FLAGS 6UL
#define QXP_IDENTITY_COUNT 7UL

typedef struct qxp_test_state {
    QXP_D3D9_DRAW_LIFETIME_GATE gate;
    QXP_D3D9_DRAW_BACKING_RECORD record;
    QXP_D3D9_DRAW_SOURCE_OBSERVED table_observed;
    BYTE storage[96];
    ULONG lock_acquires;
    ULONG lock_releases;
    ULONG lock_depth;
    ULONG lock_violations;
    ULONG resolve_calls;
    ULONG resolve_while_locked;
    ULONG resolve_fail_call;
    ULONG mutate_content_on_resolve_call;
    ULONG set_pending_after_resolve_call;
    ULONG zero_reader_count_after_resolve_call;
    DWORD last_resolve_handle;
    BOOL resolve_result;
} QXP_TEST_STATE;

typedef struct qxp_unlock_test_state {
    DWORD events[8];
    DWORD event_count;
    DWORD order_violations;
    DWORD reservation;
    DWORD lock_flags;
    DWORD generation;
    BOOL lock_active;
    BOOL reserve_result;
    BOOL mutation_end_required;
    BOOL mutation_active;
    BOOL permanently_closed;
    BOOL present_result;
    BOOL matched;
    BOOL publish_result;
    BOOL blocked_new_lock;
    BOOL blocked_destroy;
    BOOL blocked_registration;
    BOOL blocked_owner_teardown;
    BOOL destroy_admitted_after_finish;
    BOOL registration_admitted_after_finish;
    BOOL owner_teardown_admitted_after_finish;
    BOOL owner_teardown_pending;
    BOOL owner_operation_active;
    BOOL owner_entries_cleared;
    BOOL newer_lock_reserved;
} QXP_UNLOCK_TEST_STATE;

static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_source: failed: %s\n", name);
}

static void qxp_lock(void *opaque)
{
    QXP_TEST_STATE *state;

    state = (QXP_TEST_STATE *)opaque;
    ++state->lock_acquires;
    if (state->lock_depth) ++state->lock_violations;
    ++state->lock_depth;
}

static void qxp_unlock(void *opaque)
{
    QXP_TEST_STATE *state;

    state = (QXP_TEST_STATE *)opaque;
    ++state->lock_releases;
    if (state->lock_depth != 1UL) {
        ++state->lock_violations;
        if (!state->lock_depth) return;
    }
    --state->lock_depth;
}

static BOOL qxp_resolve(
    DWORD handle, QXP_D3D9_DRAW_SOURCE_OBSERVED *out, void *opaque)
{
    QXP_TEST_STATE *state;

    state = (QXP_TEST_STATE *)opaque;
    ++state->resolve_calls;
    state->last_resolve_handle = handle;
    if (state->lock_depth == 1UL)
        ++state->resolve_while_locked;
    else
        ++state->lock_violations;
    if (!state->resolve_result ||
        state->resolve_fail_call == state->resolve_calls) return FALSE;
    if (state->set_pending_after_resolve_call == state->resolve_calls)
        state->gate.mutation_pending = 1UL;
    if (state->zero_reader_count_after_resolve_call == state->resolve_calls)
        state->gate.reader_count = 0;
    *out = state->table_observed;
    if (state->mutate_content_on_resolve_call == state->resolve_calls)
        ++out->content_generation;
    return TRUE;
}

static void qxp_reset(QXP_TEST_STATE *state)
{
    memset(state, 0, sizeof(*state));
    state->resolve_result = TRUE;
    state->record.kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    state->record.role = QXP_D3D9_DRAW_BACKING_ROLE_VERTEX;
    state->record.stream = 3UL;
    state->record.handle = QXP_TEST_HANDLE;
    state->record.incarnation =
        ((ULONGLONG)0x12345678UL << 32) | (ULONGLONG)0x9abcdef0UL;
    state->record.content_generation = 17UL;
    state->record.byte_length = QXP_TEST_BYTE_LENGTH;
    state->record.video_memory_offset = 0;
    state->record.flags = 0;
    state->record.stride = 16UL;
    state->record.source2_offset = 8UL;
    state->record.divider = 1UL;
    state->record.required_span = 24UL;

    state->table_observed.kind = state->record.kind;
    state->table_observed.handle = state->record.handle;
    state->table_observed.incarnation = state->record.incarnation;
    state->table_observed.content_generation =
        state->record.content_generation;
    state->table_observed.byte_length = state->record.byte_length;
    state->table_observed.video_memory_offset =
        state->record.video_memory_offset;
    state->table_observed.flags = state->record.flags;
    state->table_observed.surface_identity = QXP_TEST_SURFACE_IDENTITY;
    state->table_observed.host_authoritative = FALSE;
    state->table_observed.lock_active = FALSE;
    state->table_observed.base_address = state->storage;
    state->table_observed.byte_count = QXP_TEST_BYTE_LENGTH;
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_acquire(
    QXP_TEST_STATE *state, QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    return QxpD3d9AcquireDrawSource(
        &state->record, &state->gate, qxp_resolve, state,
        qxp_lock, qxp_unlock, state, token);
}

static void qxp_check_balanced(
    const QXP_TEST_STATE *state, const char *name)
{
    qxp_check(state->lock_depth == 0UL &&
                  state->lock_acquires == state->lock_releases &&
                  state->lock_violations == 0UL,
              name);
}

static void qxp_check_failed_transaction(
    const QXP_TEST_STATE *state, const char *name)
{
    qxp_check(state->gate.reader_count == 0UL, name);
    qxp_check_balanced(state, name);
}

static int qxp_token_zero(const QXP_D3D9_DRAW_SOURCE_TOKEN *token)
{
    QXP_D3D9_DRAW_SOURCE_TOKEN zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(token, &zero, sizeof(zero)) == 0;
}

static int qxp_source_set_tokens_zero(
    const QXP_D3D9_DRAW_SOURCE_SET *set)
{
    DWORD stream;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (!qxp_token_zero(&set->tokens[stream])) return 0;
    }
    return 1;
}

static void qxp_source_set_expected(
    const QXP_TEST_STATE *state,
    QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT])
{
    DWORD stream;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        expected[stream] = state->record;
        expected[stream].stream = stream;
    }
}

static QXP_D3D9_DRAW_SOURCE_RESULT qxp_identity_status(ULONG field)
{
    if (field == QXP_IDENTITY_HANDLE)
        return QXP_D3D9_DRAW_SOURCE_ERROR_HANDLE_MISMATCH;
    if (field == QXP_IDENTITY_INCARNATION)
        return QXP_D3D9_DRAW_SOURCE_ERROR_INCARNATION_MISMATCH;
    if (field == QXP_IDENTITY_CONTENT_GENERATION)
        return QXP_D3D9_DRAW_SOURCE_ERROR_CONTENT_GENERATION_MISMATCH;
    if (field == QXP_IDENTITY_KIND)
        return QXP_D3D9_DRAW_SOURCE_ERROR_KIND_MISMATCH;
    if (field == QXP_IDENTITY_BYTE_LENGTH)
        return QXP_D3D9_DRAW_SOURCE_ERROR_BYTE_LENGTH_MISMATCH;
    if (field == QXP_IDENTITY_VIDEO_MEMORY_OFFSET)
        return QXP_D3D9_DRAW_SOURCE_ERROR_VIDEO_MEMORY_OFFSET_MISMATCH;
    return QXP_D3D9_DRAW_SOURCE_ERROR_FLAGS_MISMATCH;
}

static void qxp_mutate_observed_identity(
    QXP_D3D9_DRAW_SOURCE_OBSERVED *observed, ULONG field)
{
    if (field == QXP_IDENTITY_HANDLE) {
        ++observed->handle;
    } else if (field == QXP_IDENTITY_INCARNATION) {
        observed->incarnation ^= ((ULONGLONG)1UL << 32);
    } else if (field == QXP_IDENTITY_CONTENT_GENERATION) {
        ++observed->content_generation;
    } else if (field == QXP_IDENTITY_KIND) {
        observed->kind = QXP_D3D9_DRAW_BACKING_KIND_INVALID;
    } else if (field == QXP_IDENTITY_BYTE_LENGTH) {
        --observed->byte_length;
    } else if (field == QXP_IDENTITY_VIDEO_MEMORY_OFFSET) {
        observed->video_memory_offset = 1UL;
    } else {
        observed->flags = 1UL;
    }
}

static void qxp_test_happy_path(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_OBSERVED acquired_observation;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0xa5, sizeof(token));
    state.table_observed.byte_count = sizeof(state.storage);
    acquired_observation = state.table_observed;

    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK,
              "happy acquire succeeds");
    qxp_check(state.gate.reader_count == 1UL && token.reader.active == 1UL,
              "happy acquire owns one lifetime reader");
    qxp_check(token.base_address == state.storage &&
                  token.byte_count == QXP_TEST_BYTE_LENGTH &&
                  token.gate_epoch == 1UL && token.reader.epoch == 1UL,
              "happy token carries base usable bytes and epoch");
    qxp_check(memcmp(&token.observed, &acquired_observation,
                     sizeof(acquired_observation)) == 0,
              "happy token retains full observed identity");
    qxp_check(state.resolve_calls == 1UL &&
                  state.resolve_while_locked == 1UL &&
                  state.last_resolve_handle == QXP_TEST_HANDLE,
              "happy acquire resolves expected handle under lock");
    qxp_check_balanced(&state, "happy acquire returns unlocked");

    result = QxpD3d9RevalidateDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK &&
                  state.gate.reader_count == 1UL &&
                  state.resolve_calls == 2UL &&
                  state.resolve_while_locked == 2UL,
              "unchanged source and epoch revalidate while reader remains");
    qxp_check_balanced(&state, "happy revalidate returns unlocked");

    result = QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK &&
                  state.gate.reader_count == 0UL && qxp_token_zero(&token),
              "happy release drops exactly one reader and zeroes token");
    qxp_check(QxpD3d9ReleaseDrawSource(&token) ==
                  QXP_D3D9_DRAW_SOURCE_ERROR_NOT_ACTIVE &&
                  QxpD3d9RevalidateDrawSource(&token) ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NOT_ACTIVE &&
                  state.gate.reader_count == 0UL,
              "released token cannot release or revalidate twice");
    qxp_check_balanced(&state, "happy release returns unlocked");
}

static void qxp_test_admission_refused(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0xa5, sizeof(token));
    state.gate.mutation_pending = 1UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_PENDING,
              "pending mutation has distinct immediate refusal");
    qxp_check(state.resolve_calls == 0UL && qxp_token_zero(&token),
              "pending refusal does not resolve or publish a token");
    qxp_check_failed_transaction(
        &state, "pending refusal leaks no reader and returns unlocked");

    qxp_reset(&state);
    memset(&token, 0xa5, sizeof(token));
    state.gate.mutation_active = 1UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_ACTIVE,
              "active mutation has distinct immediate refusal");
    qxp_check(state.resolve_calls == 0UL && qxp_token_zero(&token),
              "active refusal does not resolve or publish a token");
    qxp_check_failed_transaction(
        &state, "active refusal leaks no reader and returns unlocked");

    qxp_reset(&state);
    memset(&token, 0xa5, sizeof(token));
    state.gate.closed = 1UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_CLOSED &&
                  state.resolve_calls == 0UL && qxp_token_zero(&token),
              "closed gate maps to distinct refusal without resolving");
    qxp_check_failed_transaction(
        &state, "closed refusal leaks no reader and returns unlocked");

    qxp_reset(&state);
    memset(&token, 0xa5, sizeof(token));
    state.gate.epoch_exhausted = 1UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result ==
                  QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_EPOCH_EXHAUSTED &&
                  state.resolve_calls == 0UL && qxp_token_zero(&token),
              "exhausted epoch maps to distinct refusal without resolving");
    qxp_check_failed_transaction(
        &state, "epoch refusal leaks no reader and returns unlocked");
}

/* Acquire must compare the immutable backing record itself, not merely trust
 * that a later revalidation will compare two equally wrong observations. */
static void qxp_test_acquire_identity_mismatches(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    ULONG field;

    for (field = 0; field < QXP_IDENTITY_COUNT; ++field) {
        qxp_reset(&state);
        memset(&token, 0, sizeof(token));
        qxp_mutate_observed_identity(&state.table_observed, field);
        result = qxp_acquire(&state, &token);
        qxp_check(result == qxp_identity_status(field),
                  "acquire detects one expected identity field mismatch");
        qxp_check(qxp_token_zero(&token) && state.resolve_calls == 1UL,
                  "acquire identity failure clears transaction token");
        qxp_check_failed_transaction(
            &state, "acquire identity failure releases reader and lock");
    }
}

/* Each field is changed only after a successful acquisition.  Thus every case
 * fails if its corresponding production comparison is removed or inverted. */
static void qxp_test_revalidate_identity_mismatches(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    ULONG field;

    for (field = 0; field < QXP_IDENTITY_COUNT; ++field) {
        qxp_reset(&state);
        memset(&token, 0, sizeof(token));
        qxp_check(qxp_acquire(&state, &token) ==
                      QXP_D3D9_DRAW_SOURCE_OK,
                  "identity revalidation setup acquires source");
        qxp_mutate_observed_identity(&state.table_observed, field);
        result = QxpD3d9RevalidateDrawSource(&token);
        if (result == QXP_D3D9_DRAW_SOURCE_OK)
            QxpD3d9ReleaseDrawSource(&token);
        qxp_check(result == qxp_identity_status(field),
                  "revalidation detects one observed identity field change");
        qxp_check(qxp_token_zero(&token) && state.resolve_calls == 2UL,
                  "identity revalidation failure clears transaction token");
        qxp_check_failed_transaction(
            &state, "identity revalidation failure releases reader and lock");
    }
}

static void qxp_test_wrapper_swap(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "wrapper swap setup acquires source");
    state.table_observed.surface_identity += 4UL;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result ==
                  QXP_D3D9_DRAW_SOURCE_ERROR_SURFACE_IDENTITY_MISMATCH,
              "CreateSurfaceEx-style wrapper swap is detected");
    qxp_check(state.table_observed.incarnation == state.record.incarnation &&
                  state.table_observed.content_generation ==
                      state.record.content_generation,
              "wrapper swap preserves incarnation and generation");
    qxp_check_failed_transaction(
        &state, "wrapper swap failure releases reader and lock");
}

static void qxp_test_epoch_cycle(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;
    ULONG admitted_epoch;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "epoch cycle setup acquires source");
    admitted_epoch = token.gate_epoch;
    qxp_check(QxpD3d9DrawLifetimeClose(
                  &state.gate, qxp_lock, qxp_unlock, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK &&
                  QxpD3d9DrawLifetimeReopen(
                  &state.gate, qxp_lock, qxp_unlock, &state) ==
                  QXP_D3D9_DRAW_LIFETIME_OK,
              "test cycles gate close and reopen");
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result ==
                  QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_REVALIDATION &&
                  state.gate.epoch_minus_one + 1UL > admitted_epoch,
              "revalidation detects close-reopen reader epoch change");
    qxp_check_failed_transaction(
        &state, "epoch failure releases old reader and returns unlocked");
}

static void qxp_test_acquire_policy_rejections(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.record.kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    state.record.video_memory_offset = 32UL;
    state.table_observed.kind = state.record.kind;
    state.table_observed.video_memory_offset = 32UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK &&
                  token.observed.kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
                  token.observed.video_memory_offset == 32UL &&
                  token.base_address == state.storage,
              "video-linear backing acquires its exact mapped range");
    if (result == QXP_D3D9_DRAW_SOURCE_OK) {
        result = QxpD3d9RevalidateDrawSource(&token);
        qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK,
                  "unchanged video-linear backing revalidates");
        if (result == QXP_D3D9_DRAW_SOURCE_OK)
            result = QxpD3d9ReleaseDrawSource(&token);
        qxp_check(result == QXP_D3D9_DRAW_SOURCE_OK &&
                      state.gate.reader_count == 0UL &&
                      qxp_token_zero(&token),
                  "video-linear release drops its reader exactly once");
    }
    qxp_check_balanced(&state, "video-linear transaction returns unlocked");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.record.kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    state.table_observed.kind = state.record.kind;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_BACKING_KIND,
              "matched non-surface backing kind is rejected");
    qxp_check_failed_transaction(
        &state, "invalid backing-kind rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.host_authoritative = TRUE;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_HOST_AUTHORITATIVE,
              "host-authoritative source is rejected");
    qxp_check_failed_transaction(
        &state, "host-authoritative rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.lock_active = TRUE;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_LOCK_ACTIVE,
              "actively locked source is rejected");
    qxp_check_failed_transaction(
        &state, "active-lock rejection releases reader and lock");
}

static void qxp_unlock_record_event(
    QXP_UNLOCK_TEST_STATE *state, DWORD event)
{
    if (state->event_count < 8UL)
        state->events[state->event_count++] = event;
    else
        ++state->order_violations;
}

static BOOL qxp_unlock_reserve(
    BOOL *lock_active, DWORD *lock_flags, void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_RESERVE);
    if (!state->reserve_result || state->reservation) return FALSE;
    state->reservation = 99UL;
    *lock_active = state->lock_active;
    *lock_flags = state->lock_flags;
    return TRUE;
}

static BOOL qxp_unlock_begin(void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_BEGIN);
    if (state->reservation != 99UL) ++state->order_violations;
    if (state->mutation_end_required)
        state->mutation_active = TRUE;
    else
        state->permanently_closed = TRUE;
    return state->mutation_end_required;
}

static void qxp_unlock_clear(void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_CLEAR);
    if (state->reservation != 99UL ||
        (!state->mutation_active && !state->permanently_closed))
        ++state->order_violations;
    state->lock_active = FALSE;
}

static BOOL qxp_unlock_present(BOOL *matched, void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_PRESENT);
    if (state->reservation != 99UL ||
        (!state->mutation_active && !state->permanently_closed))
        ++state->order_violations;
    state->blocked_new_lock =
        !QxpD3d9DrawSourceLockCanReserve(
            1UL, state->lock_active ? 1UL : 0,
            state->reservation ? 1UL : 0, 0,
            state->owner_teardown_pending ? 1UL : 0);
    state->blocked_destroy =
        !QxpD3d9DrawSourceDestroyCanReserve(
            1UL, state->lock_active ? 1UL : 0,
            state->reservation ? 1UL : 0, 0,
            state->owner_teardown_pending ? 1UL : 0);
    state->blocked_registration =
        !QxpD3d9DrawSourceRegistrationCanProceed(
            state->lock_active ? 1UL : 0,
            state->reservation ? 1UL : 0, 0,
            state->owner_teardown_pending ? 1UL : 0);
    state->blocked_owner_teardown =
        !QxpD3d9DrawSourceOwnerTeardownIsDrained(
            1UL, state->owner_operation_active ? 1UL : 0,
            0, state->reservation ? 1UL : 0, 0);
    *matched = state->matched;
    return state->present_result;
}

static BOOL qxp_unlock_publish(void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_PUBLISH);
    if (state->reservation != 99UL ||
        (!state->mutation_active && !state->permanently_closed))
        ++state->order_violations;
    ++state->generation;
    return state->publish_result;
}

static void qxp_unlock_finish(void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_FINISH);
    if (state->reservation != 99UL ||
        (!state->mutation_active && !state->permanently_closed))
        ++state->order_violations;
    state->reservation = 0;
    state->destroy_admitted_after_finish =
        QxpD3d9DrawSourceDestroyCanReserve(
            1UL, state->lock_active ? 1UL : 0, 0, 0,
            state->owner_teardown_pending ? 1UL : 0);
    state->registration_admitted_after_finish =
        QxpD3d9DrawSourceRegistrationCanProceed(
            0, 0, 0, state->owner_teardown_pending ? 1UL : 0);
    state->owner_teardown_admitted_after_finish =
        QxpD3d9DrawSourceOwnerTeardownIsDrained(
            1UL, state->owner_operation_active ? 1UL : 0,
            0, 0, 0);
    if (QxpD3d9DrawSourceLockCanReserve(
            1UL, state->lock_active ? 1UL : 0, 0, 0,
            state->owner_teardown_pending ? 1UL : 0)) {
        state->lock_active = TRUE;
        state->lock_flags = QXP_TEST_READONLY;
        state->newer_lock_reserved = TRUE;
    }
}

static void qxp_unlock_end(void *opaque)
{
    QXP_UNLOCK_TEST_STATE *state;

    state = (QXP_UNLOCK_TEST_STATE *)opaque;
    qxp_unlock_record_event(state, QXP_UNLOCK_EVENT_END);
    if (!state->mutation_active ||
        (state->owner_teardown_pending &&
         (state->owner_entries_cleared ||
          state->newer_lock_reserved || state->lock_active)) ||
        (!state->owner_teardown_pending &&
         (!state->newer_lock_reserved || !state->lock_active))) {
        ++state->order_violations;
    }
    state->mutation_active = FALSE;
}

static void qxp_unlock_initialize(
    QXP_UNLOCK_TEST_STATE *state,
    QXP_D3D9_DRAW_UNLOCK_CALLBACKS *callbacks)
{
    memset(state, 0, sizeof(*state));
    memset(callbacks, 0, sizeof(*callbacks));
    state->lock_active = TRUE;
    state->generation = 17UL;
    state->reserve_result = TRUE;
    state->mutation_end_required = TRUE;
    state->present_result = TRUE;
    state->publish_result = TRUE;
    state->owner_operation_active = TRUE;
    callbacks->reserve_lock = qxp_unlock_reserve;
    callbacks->begin_mutation = qxp_unlock_begin;
    callbacks->clear_old_lock = qxp_unlock_clear;
    callbacks->present = qxp_unlock_present;
    callbacks->publish_generation = qxp_unlock_publish;
    callbacks->finish_reservation = qxp_unlock_finish;
    callbacks->end_mutation = qxp_unlock_end;
    callbacks->opaque = state;
}

static int qxp_unlock_events_match(
    const QXP_UNLOCK_TEST_STATE *state,
    const DWORD *events, DWORD count)
{
    return state->event_count == count &&
           memcmp(state->events, events,
                  count * sizeof(events[0])) == 0;
}

static void qxp_test_unlock_publication_interleaving(void)
{
    static const DWORD writable_events[] = {
        QXP_UNLOCK_EVENT_RESERVE,
        QXP_UNLOCK_EVENT_BEGIN,
        QXP_UNLOCK_EVENT_CLEAR,
        QXP_UNLOCK_EVENT_PRESENT,
        QXP_UNLOCK_EVENT_PUBLISH,
        QXP_UNLOCK_EVENT_FINISH,
        QXP_UNLOCK_EVENT_END
    };
    static const DWORD no_publish_events[] = {
        QXP_UNLOCK_EVENT_RESERVE,
        QXP_UNLOCK_EVENT_BEGIN,
        QXP_UNLOCK_EVENT_CLEAR,
        QXP_UNLOCK_EVENT_PRESENT,
        QXP_UNLOCK_EVENT_FINISH,
        QXP_UNLOCK_EVENT_END
    };
    QXP_UNLOCK_TEST_STATE state;
    QXP_D3D9_DRAW_UNLOCK_CALLBACKS callbacks;
    QXP_D3D9_DRAW_UNLOCK_RESULT result;
    QXP_D3D9_DRAW_UNLOCK_RESULT zero_result;
    DWORD next;
    BOOL completed;

    memset(&zero_result, 0, sizeof(zero_result));
    qxp_unlock_initialize(&state, &callbacks);
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  result.reserved && result.mutation_end_required &&
                  result.lock_active && result.lock_flags == 0 &&
                  result.present_result && !result.matched &&
                  result.generation_available &&
                  state.generation == 18UL &&
                  state.blocked_new_lock && state.blocked_destroy &&
                  state.blocked_registration &&
                  state.blocked_owner_teardown &&
                  state.destroy_admitted_after_finish &&
                  state.registration_admitted_after_finish &&
                  !state.owner_teardown_admitted_after_finish &&
                  state.newer_lock_reserved && state.lock_active &&
                  state.lock_flags == QXP_TEST_READONLY &&
                  !state.mutation_active && !state.reservation &&
                  state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, writable_events,
                      (DWORD)(sizeof(writable_events) /
                              sizeof(writable_events[0]))),
              "production unlock seam orders and protects writable publish");

    qxp_unlock_initialize(&state, &callbacks);
    state.lock_flags = QXP_TEST_READONLY;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  result.generation_available &&
                  state.generation == 17UL &&
                  state.blocked_new_lock && state.newer_lock_reserved &&
                  state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, no_publish_events,
                      (DWORD)(sizeof(no_publish_events) /
                              sizeof(no_publish_events[0]))),
              "read-only unlock keeps its generation and exact ordering");

    qxp_unlock_initialize(&state, &callbacks);
    state.matched = TRUE;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  result.matched && state.generation == 17UL &&
                  state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, no_publish_events,
                      (DWORD)(sizeof(no_publish_events) /
                              sizeof(no_publish_events[0]))),
              "matched accelerated present suppresses CPU publication");

    qxp_unlock_initialize(&state, &callbacks);
    state.owner_teardown_pending = TRUE;
    memset(&result, 0xa5, sizeof(result));
    completed = QxpD3d9CompleteDrawSourceUnlock(
        QXP_TEST_READONLY, &callbacks, &result);
    qxp_check(completed == TRUE && result.reserved &&
                  result.generation_available && state.generation == 18UL &&
                  state.blocked_new_lock && state.blocked_destroy &&
                  state.blocked_registration &&
                  state.blocked_owner_teardown &&
                  !state.destroy_admitted_after_finish &&
                  !state.registration_admitted_after_finish &&
                  !state.owner_teardown_admitted_after_finish &&
                  !state.owner_entries_cleared &&
                  !state.newer_lock_reserved && !state.lock_active &&
                  !state.reservation && !state.mutation_active &&
                  state.owner_operation_active &&
                  state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, writable_events,
                      (DWORD)(sizeof(writable_events) /
                              sizeof(writable_events[0]))),
              "forced teardown overlap cannot erase outer Unlock transaction");
    state.owner_operation_active = FALSE;
    if (QxpD3d9DrawSourceOwnerTeardownIsDrained(
            1, 0, 0, 0, 0)) state.owner_entries_cleared = TRUE;
    qxp_check(state.owner_entries_cleared &&
                  !QxpD3d9DrawSourceLockCanReserve(
                      0, 0, 0, 0, 1) &&
                  !QxpD3d9DrawSourceUnlockCanReserve(
                      0, 0, 0, 0, 1) &&
                  !QxpD3d9DrawSourceRegistrationCanProceed(
                      0, 0, 0, 1),
              "terminal owner clears entries but keeps handoff tombstone");

    qxp_unlock_initialize(&state, &callbacks);
    state.present_result = FALSE;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  !result.present_result && !result.matched &&
                  result.generation_available &&
                  state.generation == 18UL &&
                  state.blocked_new_lock && state.blocked_destroy &&
                  state.blocked_registration &&
                  state.blocked_owner_teardown &&
                  state.destroy_admitted_after_finish &&
                  state.registration_admitted_after_finish &&
                  !state.owner_teardown_admitted_after_finish &&
                  state.newer_lock_reserved &&
                  state.lock_active && !state.mutation_active &&
                  !state.reservation && state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, writable_events,
                      (DWORD)(sizeof(writable_events) /
                              sizeof(writable_events[0]))),
              "present failure still publishes and closes the transaction");

    qxp_unlock_initialize(&state, &callbacks);
    state.publish_result = FALSE;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  result.present_result && !result.matched &&
                  !result.generation_available &&
                  state.generation == 18UL &&
                  state.blocked_new_lock && state.blocked_destroy &&
                  state.blocked_registration &&
                  state.blocked_owner_teardown &&
                  state.destroy_admitted_after_finish &&
                  state.registration_admitted_after_finish &&
                  !state.owner_teardown_admitted_after_finish &&
                  state.newer_lock_reserved &&
                  state.lock_active && !state.mutation_active &&
                  !state.reservation && state.order_violations == 0 &&
                  qxp_unlock_events_match(
                      &state, writable_events,
                      (DWORD)(sizeof(writable_events) /
                              sizeof(writable_events[0]))),
              "generation failure still finishes reservation and mutation");

    qxp_unlock_initialize(&state, &callbacks);
    state.mutation_end_required = FALSE;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == TRUE &&
                  !result.mutation_end_required &&
                  state.permanently_closed &&
                  state.event_count == 6UL &&
                  state.events[5] == QXP_UNLOCK_EVENT_FINISH &&
                  state.order_violations == 0,
              "permanently closed gate completes publish without an end");

    qxp_unlock_initialize(&state, &callbacks);
    state.reservation = 7UL;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == FALSE &&
                  state.event_count == 1UL &&
                  state.events[0] == QXP_UNLOCK_EVENT_RESERVE &&
                  state.reservation == 7UL &&
                  memcmp(&result, &zero_result, sizeof(result)) == 0,
              "conflicting exact unlock reservation mutates nothing");

    next = 99UL;
    qxp_check(QxpD3d9DrawSourceNextUnlockReservation(0, &next) == TRUE &&
                  next == 1UL &&
                  QxpD3d9DrawSourceNextUnlockReservation(
                      ~0UL, &next) == FALSE && next == 1UL,
              "unlock reservation clock starts nonzero and never wraps");
    qxp_check(QxpD3d9DrawSourceLockCanReserve(
                      0, 0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      1, 0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      1, 1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      1, 0, 1, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      1, 0, 0, 1, 0) == FALSE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      1, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      0, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceLockCanReserve(
                      0, 1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      1, 0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      1, 1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      1, 0, 1, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      1, 0, 0, 1, 0) == FALSE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      1, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceUnlockCanReserve(
                      0, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      1, 0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      1, 1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      1, 0, 1, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      1, 0, 0, 1, 0) == FALSE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      1, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceDestroyCanReserve(
                      0, 0, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceRegistrationCanProceed(
                      0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceRegistrationCanProceed(
                      1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceRegistrationCanProceed(
                      0, 1, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceRegistrationCanProceed(
                      0, 0, 1, 0) == FALSE &&
                  QxpD3d9DrawSourceRegistrationCanProceed(
                      0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      0, 0, 0, 0, 0) == TRUE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      0, 1, 0, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      1, 0, 1, 0, 0) == FALSE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      1, 0, 0, 1, 0) == FALSE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      1, 0, 0, 0, 1) == FALSE &&
                  QxpD3d9DrawSourceOwnerTeardownIsDrained(
                      1, 0, 0, 0, 0) == TRUE,
              "shared arbitration blocks entrants and drains terminal owner");

    qxp_unlock_initialize(&state, &callbacks);
    callbacks.finish_reservation = NULL;
    memset(&result, 0xa5, sizeof(result));
    qxp_check(QxpD3d9CompleteDrawSourceUnlock(
                  QXP_TEST_READONLY, &callbacks, &result) == FALSE &&
                  state.event_count == 0 &&
                  memcmp(&result, &zero_result, sizeof(result)) == 0 &&
                  QxpD3d9CompleteDrawSourceUnlock(
                      0, &callbacks, &result) == FALSE &&
                  QxpD3d9CompleteDrawSourceUnlock(
                      QXP_TEST_READONLY, NULL, &result) == FALSE &&
                  QxpD3d9CompleteDrawSourceUnlock(
                      QXP_TEST_READONLY, &callbacks, NULL) == FALSE &&
                  QxpD3d9DrawSourceNextUnlockReservation(0, NULL) == FALSE,
              "unlock seam rejects incomplete and null contracts pre-reserve");
}

static void qxp_test_extent_rejections(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.record.source2_offset = 0xfffffff0UL;
    state.record.required_span = 0x40UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT_OVERFLOW,
              "wrapping required extent is rejected before truncation");
    qxp_check_failed_transaction(
        &state, "extent overflow rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.record.source2_offset = 48UL;
    state.record.required_span = 17UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT,
              "required span beyond byte length is rejected");
    qxp_check_failed_transaction(
        &state, "overlong span rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.byte_count = 16UL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT,
              "addressable byte count must cover required extent");
    qxp_check_failed_transaction(
        &state, "short address extent releases reader and lock");
}

static void qxp_test_malformed_observations(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.resolve_result = FALSE;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE,
              "failed resolve callback is rejected");
    qxp_check_failed_transaction(
        &state, "resolve failure releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.surface_identity = 0;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_NULL_SURFACE_IDENTITY,
              "null wrapper identity is rejected");
    qxp_check_failed_transaction(
        &state, "null wrapper rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.base_address = NULL;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_NULL_BASE,
              "null source base is rejected");
    qxp_check_failed_transaction(
        &state, "null base rejection releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    state.table_observed.byte_count = 0;
    result = qxp_acquire(&state, &token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_ZERO_BYTE_COUNT,
              "zero source byte count is rejected");
    qxp_check_failed_transaction(
        &state, "zero byte rejection releases reader and lock");
}

static void qxp_test_revalidation_full_observation(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_SOURCE_RESULT result;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "kind mutation setup acquires source");
    state.table_observed.kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_KIND_MISMATCH,
              "revalidation detects a system-to-video kind change");
    qxp_check_failed_transaction(
        &state, "kind change releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "resolve failure mutation setup acquires source");
    state.resolve_result = FALSE;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE,
              "revalidation detects failed second resolve");
    qxp_check_failed_transaction(
        &state, "second resolve failure releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "base-address mutation setup acquires source");
    state.table_observed.base_address = state.storage + 1;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_BASE_ADDRESS_MISMATCH,
              "revalidation compares transient base address");
    qxp_check_failed_transaction(
        &state, "base-address change releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "base-count mutation setup acquires source");
    --state.table_observed.byte_count;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result ==
                  QXP_D3D9_DRAW_SOURCE_ERROR_BASE_BYTE_COUNT_MISMATCH,
              "revalidation compares transient base byte count");
    qxp_check_failed_transaction(
        &state, "base-count change releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "authority mutation setup acquires source");
    state.table_observed.host_authoritative = TRUE;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_HOST_AUTHORITATIVE,
              "revalidation rejects new host authority");
    qxp_check_failed_transaction(
        &state, "authority change releases reader and lock");

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    qxp_check(qxp_acquire(&state, &token) ==
                  QXP_D3D9_DRAW_SOURCE_OK,
              "lock mutation setup acquires source");
    state.table_observed.lock_active = TRUE;
    result = QxpD3d9RevalidateDrawSource(&token);
    if (result == QXP_D3D9_DRAW_SOURCE_OK)
        QxpD3d9ReleaseDrawSource(&token);
    qxp_check(result == QXP_D3D9_DRAW_SOURCE_ERROR_LOCK_ACTIVE,
              "revalidation rejects newly active lock");
    qxp_check_failed_transaction(
        &state, "lock change releases reader and lock");
}

static void qxp_test_source_set_happy_path(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_SOURCE_SET set;
    QXP_D3D9_DRAW_SOURCE_SET_STATUS status;
    DWORD stream;
    int tokens_exact;

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0xa5, sizeof(set));
    memset(&status, 0xa5, sizeof(status));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == TRUE,
              "complete source set acquires all sixteen streams");
    tokens_exact = set.active_stream_mask ==
                       QXP_D3D9_DRAW_SOURCE_STREAM_MASK &&
                   state.gate.reader_count ==
                       QXP_D3D9_DRAW_BACKING_STREAM_COUNT &&
                   state.resolve_calls ==
                       QXP_D3D9_DRAW_BACKING_STREAM_COUNT &&
                   status.result == QXP_D3D9_DRAW_SOURCE_OK &&
                   status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (!set.tokens[stream].reader.active ||
            set.tokens[stream].observed.handle != QXP_TEST_HANDLE ||
            set.tokens[stream].base_address != state.storage ||
            set.tokens[stream].byte_count != QXP_TEST_BYTE_LENGTH) {
            tokens_exact = 0;
        }
    }
    qxp_check(tokens_exact,
              "complete source set retains every exact duplicate backing token");
    qxp_check_balanced(&state, "complete source-set acquire returns unlocked");

    qxp_check(QxpD3d9RevalidateDrawSourceSet(&set, &status) == TRUE &&
                  set.active_stream_mask ==
                      QXP_D3D9_DRAW_SOURCE_STREAM_MASK &&
                  state.gate.reader_count ==
                      QXP_D3D9_DRAW_BACKING_STREAM_COUNT &&
                  state.resolve_calls ==
                      QXP_D3D9_DRAW_BACKING_STREAM_COUNT * 2UL &&
                  status.result == QXP_D3D9_DRAW_SOURCE_OK,
              "complete source set revalidates while retaining all readers");
    qxp_check(QxpD3d9ReleaseDrawSourceSet(&set, &status) == TRUE &&
                  set.active_stream_mask == 0 &&
                  state.gate.reader_count == 0 &&
                  qxp_source_set_tokens_zero(&set) &&
                  status.result == QXP_D3D9_DRAW_SOURCE_OK,
              "complete source set reverse-releases every reader");
    qxp_check(QxpD3d9ReleaseDrawSourceSet(&set, &status) == TRUE &&
                  state.gate.reader_count == 0,
              "an empty source set releases idempotently");
    qxp_check_balanced(&state, "complete source-set lifecycle balances locks");
}

static void qxp_test_source_set_failure_cleanup(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_SOURCE_SET set;
    QXP_D3D9_DRAW_SOURCE_SET_STATUS status;

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    state.resolve_fail_call = 8UL;
    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == FALSE &&
                  status.result == QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE &&
                  status.stream == 7UL &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  !QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls == 8UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "stream-seven resolve failure releases its reader and prefix");
    qxp_check_balanced(&state,
                       "partial source-set resolve failure balances locks");

    /* The failing scalar token can itself encounter an impossible empty gate
     * while releasing after semantic validation.  That release error becomes
     * the primary set result and is fatal through the shared classifier. */
    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    state.table_observed.kind = QXP_D3D9_DRAW_BACKING_KIND_INVALID;
    state.zero_reader_count_after_resolve_call = 1UL;
    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE &&
                  status.stream == 0 &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls == 1UL &&
                  state.lock_acquires == 3UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "failing token propagates its own release error as fatal");
    qxp_check_balanced(&state,
                       "primary release failure balances source-set locks");

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    state.set_pending_after_resolve_call = 1UL;
    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_PENDING &&
                  status.stream == 1UL &&
                  !QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls == 1UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "mutation pending after stream zero releases admitted prefix");
    qxp_check_balanced(&state,
                       "partial source-set admission refusal balances locks");

    /* Corrupt the test gate's reader count after eight successful admissions,
     * then announce a mutation.  Stream eight is refused and every reverse
     * cleanup attempt reports the impossible empty count; the first cleanup
     * error is preserved without stopping the remaining attempts. */
    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    state.set_pending_after_resolve_call = 8UL;
    state.zero_reader_count_after_resolve_call = 8UL;
    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_PENDING &&
                  status.stream == 8UL &&
                  status.cleanup_result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE &&
                  status.cleanup_stream == 7UL &&
                  QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls == 8UL &&
                  state.lock_acquires == 25UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "acquisition rollback preserves release failure and visits all");
    qxp_check_balanced(&state,
                       "release-failing source-set rollback balances locks");

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == TRUE,
              "source-set revalidation failure setup acquires all streams");
    state.resolve_fail_call =
        QXP_D3D9_DRAW_BACKING_STREAM_COUNT + 8UL;
    qxp_check(QxpD3d9RevalidateDrawSourceSet(&set, &status) == FALSE &&
                  status.result == QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE &&
                  status.stream == 7UL &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  !QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls ==
                      QXP_D3D9_DRAW_BACKING_STREAM_COUNT + 8UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "one failed revalidation releases the complete source set");
    qxp_check_balanced(&state,
                       "source-set revalidation failure balances locks");

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, QXP_D3D9_DRAW_SOURCE_STREAM_MASK,
                  &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == TRUE,
              "source-set ABA revalidation setup acquires all streams");
    state.mutate_content_on_resolve_call =
        QXP_D3D9_DRAW_BACKING_STREAM_COUNT + 8UL;
    qxp_check(QxpD3d9RevalidateDrawSourceSet(&set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_CONTENT_GENERATION_MISMATCH &&
                  status.stream == 7UL &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  !QxpD3d9DrawSourceSetStatusHasReleaseFailure(&status) &&
                  state.resolve_calls ==
                      QXP_D3D9_DRAW_BACKING_STREAM_COUNT + 8UL &&
                  state.gate.reader_count == 0 &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "stream-seven post-copy ABA releases the complete source set");
    qxp_check_balanced(&state,
                       "source-set ABA revalidation balances locks");
}

static void qxp_test_source_set_empty_and_invalid(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_SOURCE_SET set;
    QXP_D3D9_DRAW_SOURCE_SET_STATUS status;

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, 0, &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &status) == TRUE &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set) &&
                  state.resolve_calls == 0 && state.gate.reader_count == 0 &&
                  QxpD3d9RevalidateDrawSourceSet(&set, &status) == TRUE &&
                  QxpD3d9ReleaseDrawSourceSet(&set, &status) == TRUE,
              "zero-mask source set is a successful empty transaction");

    memset(&set, 0xa5, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, 0x00010000UL, &state.gate,
                  qxp_resolve, &state, qxp_lock, qxp_unlock, &state,
                  &set, &status) == FALSE &&
                  status.result == QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  set.active_stream_mask == 0 &&
                  qxp_source_set_tokens_zero(&set) &&
                  state.gate.reader_count == 0,
              "out-of-range source-set mask fails before admission");
    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, (1UL << 0) | (1UL << 7), &state.gate,
                  qxp_resolve, &state, qxp_lock, qxp_unlock, &state,
                  &set, &status) == TRUE &&
                  state.gate.reader_count == 2UL,
              "malformed revalidation cleanup setup acquires two readers");
    set.active_stream_mask |= 0x00010000UL;
    qxp_check(QxpD3d9RevalidateDrawSourceSet(&set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  set.active_stream_mask == 0 &&
                  state.gate.reader_count == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "malformed revalidation mask drains every valid reader");
    qxp_check_balanced(
        &state, "malformed revalidation cleanup balances locks");

    qxp_reset(&state);
    qxp_source_set_expected(&state, expected);
    memset(&set, 0, sizeof(set));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, (1UL << 0) | (1UL << 7), &state.gate,
                  qxp_resolve, &state, qxp_lock, qxp_unlock, &state,
                  &set, &status) == TRUE &&
                  state.gate.reader_count == 2UL,
              "malformed release cleanup setup acquires two readers");
    set.active_stream_mask |= 0x00010000UL;
    qxp_check(QxpD3d9ReleaseDrawSourceSet(&set, &status) == FALSE &&
                  status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  status.cleanup_result == QXP_D3D9_DRAW_SOURCE_OK &&
                  set.active_stream_mask == 0 &&
                  state.gate.reader_count == 0 &&
                  qxp_source_set_tokens_zero(&set),
              "malformed release mask drains every valid reader");
    qxp_check_balanced(&state,
                       "empty and malformed source sets balance locks");
}

static void qxp_test_owner_gate_state_machine(void)
{
    QXP_D3D9_DRAW_OWNER_GATE gate;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN token;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN second;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN third;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN bad;
    QXP_D3D9_DRAW_OWNER_GATE_RESULT result;
    DWORD old_generation;
    DWORD same_thread;
    DWORD other_threads;

    memset(&gate, 0, sizeof(gate));
    memset(&token, 0xa5, sizeof(token));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0x10UL, 0x20UL, 0x100UL, 3UL, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.global_identity == 0x20UL && gate.generation &&
                  gate.in_flight == 1UL && gate.operations == &token &&
                  token.owner_identity == 0x10UL &&
                  token.thread_identity == 0x100UL &&
                  token.generation == gate.generation &&
                  token.slot == 3UL && !token.revoked,
              "first local owner enters linked shared-global gate");
    old_generation = gate.generation;

    memset(&second, 0xa5, sizeof(second));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0x11UL, 0x20UL, 0x200UL, 3UL, &second);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.in_flight == 2UL && gate.operations == &second &&
                  second.next == &token &&
                  QxpD3d9DrawOwnerGateValidateOperation(
                      &gate, &token) == QXP_D3D9_DRAW_OWNER_GATE_OK,
              "distinct threads publish exact linked callback tokens");
    memset(&third, 0xa5, sizeof(third));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0x12UL, 0x20UL, 0x100UL, 3UL, &third);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.in_flight == 3UL && gate.operations == &third &&
                  third.next == &second &&
                  third.owner_identity == 0x12UL,
              "same thread may hold callbacks for distinct local owners");

    result = QxpD3d9DrawOwnerGateBeginTeardown(
        &gate, 0x10UL, 0x20UL, 0x100UL);
    same_thread = 99UL;
    other_threads = 99UL;
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.owner_identity == 0x10UL &&
                  gate.teardown_thread_identity == 0x100UL &&
                  QxpD3d9DrawOwnerGateTeardownOperationCounts(
                      &gate, 0x100UL, &same_thread,
                      &other_threads) == TRUE &&
                  same_thread == 2UL && other_threads == 1UL,
              "terminal close separates same-thread cross-owner tokens");
    memset(&bad, 0xa5, sizeof(bad));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0x12UL, 0x20UL, 0x300UL, 3UL, &bad);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_REFUSED && !bad.slot,
              "terminal admission closes new callbacks");
    qxp_check(QxpD3d9DrawOwnerGateBeginTeardown(
                  &gate, 0x12UL, 0x20UL, 0x100UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_FATAL &&
                  QxpD3d9DrawOwnerGateBeginTeardown(
                  &gate, 0x12UL, 0x20UL, 0x300UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REFUSED,
              "nested and concurrent terminal admission never self-waits");
    qxp_check(QxpD3d9DrawOwnerGateRevokeThreadOperations(
                  &gate, 0x100UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REFUSED &&
                  !token.revoked && !second.revoked && !third.revoked,
              "same-thread revocation cannot outrun another thread");

    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &second);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.in_flight == 2UL && gate.operations == &third &&
                  third.next == &token,
              "other-thread callback drains first");
    qxp_check(QxpD3d9DrawOwnerGateTeardownOperationCounts(
                  &gate, 0x100UL, &same_thread,
                  &other_threads) == TRUE &&
                  same_thread == 2UL && other_threads == 0,
              "drain leaves both same-thread local-owner tokens");
    result = QxpD3d9DrawOwnerGateRevokeThreadOperations(
        &gate, 0x100UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  token.revoked && !token.owner_identity &&
                  third.revoked && !third.owner_identity &&
                  QxpD3d9DrawOwnerGateValidateOperation(
                      &gate, &token) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  QxpD3d9DrawOwnerGateValidateOperation(
                      &gate, &third) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED,
              "all same-thread local-owner tokens lose owner authority");
    result = QxpD3d9DrawOwnerGateCompleteTeardown(
        &gate, 0x10UL, 0x20UL, 0x100UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  !gate.owner_identity &&
                  !gate.teardown_thread_identity &&
                  !gate.teardown_users && gate.revoking == 1UL &&
                  gate.in_flight == 2UL,
              "logical completion retains only owner-free revocation state");

    result = QxpD3d9DrawOwnerGateBeginTeardown(
        &gate, 0x11UL, 0x20UL, 0x300UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK,
              "second local teardown joins owner-free shared-global drain");
    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  gate.global_identity == 0x20UL &&
                  gate.revoking == 1UL && gate.in_flight == 1UL &&
                  gate.operations == &third &&
                  gate.owner_identity == 0x11UL,
              "first revoked owner callback drains inside successor teardown");
    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &third);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  gate.global_identity == 0x20UL &&
                  gate.revoking == 1UL && !gate.in_flight &&
                  gate.owner_identity == 0x11UL &&
                  QxpD3d9DrawOwnerGateTeardownOperationCounts(
                      &gate, 0x300UL, &same_thread,
                      &other_threads) == TRUE &&
                  same_thread == 0 && other_threads == 0,
              "last cross-owner callback drains inside successor teardown");
    result = QxpD3d9DrawOwnerGateCompleteTeardown(
        &gate, 0x11UL, 0x20UL, 0x300UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  !gate.global_identity && !gate.generation &&
                  !gate.revoking && !gate.in_flight &&
                  !gate.owner_identity,
              "successor terminal commit reclaims the drained gate");

    memset(&token, 0xa5, sizeof(token));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0, 0x20UL, 0x400UL, 9UL, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.generation != old_generation,
              "reopened slot receives a non-ABA generation");
    old_generation = gate.generation;
    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.global_identity == 0x20UL &&
                  gate.generation == old_generation &&
                  !gate.in_flight && !gate.owner_identity,
              "idle callback retains only hot global key and generation");
    result = QxpD3d9DrawOwnerGateBeginTeardown(
        &gate, 0x30UL, 0x20UL, 0x500UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3d9DrawOwnerGateCompleteTeardown(
                      &gate, 0x30UL, 0x20UL, 0x500UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  !gate.global_identity,
              "drained terminal close reclaims idle gate");

    memset(&gate, 0, sizeof(gate));
    memset(&token, 0, sizeof(token));
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0x40UL, 0x50UL, 0x600UL, 7UL, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK,
              "force-completion setup enters operation");
    bad = token;
    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &bad);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_FATAL &&
                  gate.in_flight == 1UL && gate.operations == &token,
              "copied token cannot unlink a live stack token");
    result = QxpD3d9DrawOwnerGateBeginTeardown(
        &gate, 0x40UL, 0x50UL, 0x700UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3d9DrawOwnerGateForceCompleteTeardown(
                      &gate, 0x40UL, 0x50UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  token.revoked && !token.owner_identity &&
                  gate.revoking == 1UL && !gate.owner_identity,
              "fatal fallback removes every token owner field");
    result = QxpD3d9DrawOwnerGateEndOperation(&gate, &token);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  !gate.global_identity,
              "force-revoked token performs final reclamation");

    memset(&gate, 0, sizeof(gate));
    memset(&token, 0, sizeof(token));
    memset(&second, 0, sizeof(second));
    qxp_check(QxpD3d9DrawOwnerGateBeginOperation(
                  &gate, 0x70UL, 0x80UL, 0x900UL, 4UL,
                  &token) == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3d9DrawOwnerGateBeginOperation(
                  &gate, 0, 0x80UL, 0xa00UL, 4UL,
                  &second) == QXP_D3D9_DRAW_OWNER_GATE_OK,
              "fatal global close setup links explicit and ownerless tokens");
    result = QxpD3d9DrawOwnerGateFatalClose(&gate);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  gate.revoking == 1UL && gate.in_flight == 2UL &&
                  token.revoked && !token.owner_identity &&
                  second.revoked && !second.owner_identity &&
                  QxpD3d9DrawOwnerGateValidateOperation(
                      &gate, &token) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  QxpD3d9DrawOwnerGateValidateOperation(
                      &gate, &second) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED,
              "token-independent fatal close removes all owner authority");
    qxp_check(QxpD3d9DrawOwnerGateEndOperation(
                  &gate, &second) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  QxpD3d9DrawOwnerGateEndOperation(
                  &gate, &token) ==
                      QXP_D3D9_DRAW_OWNER_GATE_REVOKED &&
                  !gate.global_identity && !gate.in_flight,
              "fatal-close callbacks drain and reclaim normally");
}

static void qxp_test_exclusive_callback_gate(void)
{
    QXP_D3D9_DRAW_OWNER_GATE gate;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN callback;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN refused;
    QXP_D3D9_DRAW_OWNER_GATE_RESULT result;
    unsigned long exclusive_owner_identity;
    int gdi_selected;

    memset(&gate, 0, sizeof(gate));
    memset(&callback, 0, sizeof(callback));
    memset(&refused, 0, sizeof(refused));
    exclusive_owner_identity = 0;
    gdi_selected = 1;
    result = QxpD3d9DrawOwnerGateBeginOperation(
        &gate, 0, 0x20UL, 0x100UL, 3UL, &callback);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3dTexBltSetExclusiveDisplayState(
                      0x20UL, 1, &exclusive_owner_identity,
                      &gdi_selected) &&
                  exclusive_owner_identity == 0x20UL && !gdi_selected,
              "admitted NT callback publishes exact exclusive owner");

    result = QxpD3d9DrawOwnerGateBeginTeardown(
        &gate, 0x10UL, 0x20UL, 0x200UL);
    qxp_check(result == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3d9DrawOwnerGateBeginOperation(
                      &gate, 0, 0x20UL, 0x300UL, 3UL,
                      &refused) ==
                          QXP_D3D9_DRAW_OWNER_GATE_REFUSED &&
                  !refused.slot &&
                  exclusive_owner_identity == 0x20UL && !gdi_selected,
              "local teardown refuses concurrent NT callback mutation");
    qxp_check(QxpD3d9DrawOwnerGateEndOperation(
                  &gate, &callback) ==
                      QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3d9DrawOwnerGateCompleteTeardown(
                      &gate, 0x10UL, 0x20UL, 0x200UL) ==
                      QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  exclusive_owner_identity == 0x20UL && !gdi_selected,
              "one local teardown preserves shared-global exclusive state");

    memset(&callback, 0, sizeof(callback));
    qxp_check(QxpD3d9DrawOwnerGateBeginOperation(
                  &gate, 0, 0x20UL, 0x400UL, 3UL,
                  &callback) == QXP_D3D9_DRAW_OWNER_GATE_OK &&
                  QxpD3dTexBltSetExclusiveDisplayState(
                      0x20UL, 0, &exclusive_owner_identity,
                      &gdi_selected) &&
                  !exclusive_owner_identity && gdi_selected &&
                  QxpD3d9DrawOwnerGateEndOperation(
                      &gate, &callback) ==
                      QXP_D3D9_DRAW_OWNER_GATE_OK,
              "explicit exclusive exit clears state after local teardown");
}

static void qxp_test_null_arguments(void)
{
    QXP_TEST_STATE state;
    QXP_D3D9_DRAW_SOURCE_TOKEN token;
    QXP_D3D9_DRAW_BACKING_RECORD
        expected[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_SOURCE_SET set;
    QXP_D3D9_DRAW_SOURCE_SET_STATUS set_status;
    QXP_D3D9_DRAW_SOURCE_RESULT error;

    qxp_reset(&state);
    memset(&token, 0, sizeof(token));
    error = QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT;
    qxp_check(QxpD3d9AcquireDrawSource(
                  NULL, &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &token) == error &&
                  QxpD3d9AcquireDrawSource(
                  &state.record, NULL, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &token) == error &&
                  QxpD3d9AcquireDrawSource(
                  &state.record, &state.gate, NULL, &state,
                  qxp_lock, qxp_unlock, &state, &token) == error &&
                  QxpD3d9AcquireDrawSource(
                  &state.record, &state.gate, qxp_resolve, &state,
                  NULL, qxp_unlock, &state, &token) == error &&
                  QxpD3d9AcquireDrawSource(
                  &state.record, &state.gate, qxp_resolve, &state,
                  qxp_lock, NULL, &state, &token) == error &&
                  QxpD3d9AcquireDrawSource(
                  &state.record, &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, NULL) == error,
              "acquire rejects every null required argument");
    qxp_check(QxpD3d9RevalidateDrawSource(NULL) == error &&
                  QxpD3d9ReleaseDrawSource(NULL) == error,
              "revalidate and release reject null token");

    qxp_source_set_expected(&state, expected);
    memset(&set, 0, sizeof(set));
    memset(&set_status, 0, sizeof(set_status));
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  NULL, 1UL, &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, &set, &set_status) == FALSE &&
                  set_status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  set.active_stream_mask == 0,
              "source-set acquire rejects a null backing array");
    qxp_check(QxpD3d9AcquireDrawSourceSet(
                  expected, 1UL, &state.gate, qxp_resolve, &state,
                  qxp_lock, qxp_unlock, &state, NULL, &set_status) == FALSE &&
                  set_status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  QxpD3d9RevalidateDrawSourceSet(NULL, &set_status) == FALSE &&
                  set_status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT &&
                  QxpD3d9ReleaseDrawSourceSet(NULL, &set_status) == FALSE &&
                  set_status.result ==
                      QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT,
              "source-set lifecycle rejects null set storage");
    qxp_check(state.resolve_calls == 0UL &&
                  state.gate.reader_count == 0UL,
              "null arguments admit no reader and invoke no resolve");
    qxp_check_balanced(&state, "null arguments never leave lock held");
}

int main(void)
{
    qxp_test_happy_path();
    qxp_test_admission_refused();
    qxp_test_acquire_identity_mismatches();
    qxp_test_revalidate_identity_mismatches();
    qxp_test_wrapper_swap();
    qxp_test_epoch_cycle();
    qxp_test_acquire_policy_rejections();
    qxp_test_unlock_publication_interleaving();
    qxp_test_extent_rejections();
    qxp_test_malformed_observations();
    qxp_test_revalidation_full_observation();
    qxp_test_source_set_happy_path();
    qxp_test_source_set_failure_cleanup();
    qxp_test_source_set_empty_and_invalid();
    qxp_test_owner_gate_state_machine();
    qxp_test_exclusive_callback_gate();
    qxp_test_null_arguments();

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw source tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw source tests: PASS\n");
    return 0;
}
