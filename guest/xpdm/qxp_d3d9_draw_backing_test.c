#include <stdio.h>
#include <string.h>

#include <windef.h>

#include "qxpdisp/qxp_d3d9_draw_backing.h"

#define QXP_FAKE_MAX 32
#define QXP_MUTATE_NONE 0UL
#define QXP_MUTATE_STATE_GENERATION 1UL
#define QXP_MUTATE_STATE_UNBIND 2UL
#define QXP_MUTATE_STATE_REBIND 3UL
#define QXP_MUTATE_TABLE_TEARDOWN 4UL
#define QXP_MUTATE_TABLE_INCARNATION 5UL
#define QXP_MUTATE_TABLE_GENERATION 6UL

typedef struct qxp_fake_backing {
    DWORD handle;
    DWORD role;
    BOOL present;
    QXP_D3D9_DRAW_BACKING_METADATA metadata;
} QXP_FAKE_BACKING;

static QXP_FAKE_BACKING qxp_fakes[QXP_FAKE_MAX];
static DWORD qxp_fake_count;
static DWORD qxp_lookup_calls;
static DWORD qxp_lookup_calls_while_unlocked;
static DWORD qxp_last_dd_identity;
static DWORD qxp_fake_dd_identity;

static DWORD qxp_lock_acquires;
static DWORD qxp_lock_releases;
static DWORD qxp_lock_depth;
static DWORD qxp_lock_max_depth;
static DWORD qxp_lock_violations;
static DWORD qxp_mutate_at_acquire;
static DWORD qxp_mutation;
static DWORD qxp_mutation_value;

static QXP_D3D9_DRAW_PREPARED qxp_prepared;
static QXP_D3D9_DRAW_STATE qxp_current;
static QXP_D3D9_DRAW_BACKING_OWNER qxp_owner;
static QXP_D3D9_DRAW_BACKING_UM_ENVELOPE qxp_um;
static BOOL qxp_indexed;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_out;
static QXP_D3D9_DRAW_BACKING_STATUS qxp_status;
static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_backing: failed: %s\n", name);
}

static void qxp_lock(void *opaque)
{
    (void)opaque;
    ++qxp_lock_acquires;
    if (qxp_lock_depth) ++qxp_lock_violations;
    ++qxp_lock_depth;
    if (qxp_lock_depth > qxp_lock_max_depth)
        qxp_lock_max_depth = qxp_lock_depth;
    if (qxp_mutate_at_acquire == qxp_lock_acquires) {
        if (qxp_mutation == QXP_MUTATE_STATE_GENERATION) {
            qxp_current.stream_source_generation = qxp_mutation_value;
        } else if (qxp_mutation == QXP_MUTATE_STATE_UNBIND) {
            qxp_current.streams[0].handle = 0;
        } else if (qxp_mutation == QXP_MUTATE_STATE_REBIND) {
            qxp_current.streams[0].handle = qxp_mutation_value;
        } else if (qxp_mutation == QXP_MUTATE_TABLE_TEARDOWN) {
            qxp_fakes[0].present = FALSE;
        } else if (qxp_mutation == QXP_MUTATE_TABLE_INCARNATION) {
            qxp_fakes[0].metadata.incarnation = qxp_mutation_value;
        } else if (qxp_mutation == QXP_MUTATE_TABLE_GENERATION) {
            qxp_fakes[0].metadata.content_generation = qxp_mutation_value;
        }
        qxp_mutation = QXP_MUTATE_NONE;
    }
}

static void qxp_unlock(void *opaque)
{
    (void)opaque;
    ++qxp_lock_releases;
    if (qxp_lock_depth != 1) {
        ++qxp_lock_violations;
        if (!qxp_lock_depth) return;
    }
    --qxp_lock_depth;
}

static BOOL qxp_backing_lookup(
    const QXP_D3D9_DRAW_BACKING_OWNER *owner, DWORD handle, DWORD role,
    QXP_D3D9_DRAW_BACKING_METADATA *metadata, void *opaque)
{
    DWORD index;

    (void)opaque;
    ++qxp_lookup_calls;
    if (!qxp_lock_depth) ++qxp_lookup_calls_while_unlocked;
    if (owner) qxp_last_dd_identity = (DWORD)owner->dd_local_identity;
    if (!owner || owner->dd_local_identity != qxp_fake_dd_identity)
        return FALSE;
    for (index = 0; index < qxp_fake_count; ++index) {
        if (qxp_fakes[index].handle == handle &&
            qxp_fakes[index].role == role) {
            if (!qxp_fakes[index].present) return FALSE;
            *metadata = qxp_fakes[index].metadata;
            return TRUE;
        }
    }
    return FALSE;
}

static void qxp_clear_fakes(void)
{
    qxp_fake_count = 0;
}

static QXP_D3D9_DRAW_BACKING_METADATA *qxp_add_fake(DWORD handle, DWORD role)
{
    QXP_FAKE_BACKING *fake;

    fake = &qxp_fakes[qxp_fake_count++];
    memset(fake, 0, sizeof(*fake));
    fake->handle = handle;
    fake->role = role;
    fake->present = TRUE;
    fake->metadata.valid = TRUE;
    fake->metadata.handle = handle;
    fake->metadata.incarnation = 5;
    fake->metadata.content_generation = 3;
    fake->metadata.byte_length = 4096;
    fake->metadata.kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    fake->metadata.role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
                               QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
    fake->metadata.video_memory_offset = 0;
    fake->metadata.flags = 0;
    return &fake->metadata;
}

static void qxp_reset_baseline(void)
{
    memset(&qxp_prepared, 0, sizeof(qxp_prepared));
    memset(&qxp_current, 0, sizeof(qxp_current));
    memset(&qxp_owner, 0, sizeof(qxp_owner));
    memset(&qxp_um, 0, sizeof(qxp_um));
    qxp_clear_fakes();
    qxp_indexed = FALSE;
    qxp_fake_dd_identity = 0xABCUL;
    qxp_mutate_at_acquire = 0;
    qxp_mutation = QXP_MUTATE_NONE;
    qxp_mutation_value = 0;

    qxp_prepared.valid = TRUE;
    qxp_prepared.mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    qxp_prepared.owner.context_handle = 0x41UL;
    qxp_prepared.owner.process_id = 0x51UL;
    qxp_prepared.owner.device_identity = 0x1234UL;
    qxp_prepared.required_stream_mask = 1UL;
    qxp_prepared.required_user_memory_stream_mask = 0UL;
    qxp_prepared.required_stream_spans[0] = 12UL;

    qxp_current.streams[0].handle = 0x900UL;
    qxp_current.streams[0].offset = 0UL;
    qxp_current.streams[0].stride = 20UL;
    qxp_current.streams[0].divider = 1UL;
    qxp_current.streams[0].user_memory = FALSE;
    qxp_current.streams[0].source2 = FALSE;
    qxp_current.stream_source_generation = 7;

    memcpy(&qxp_prepared.raw_state, &qxp_current, sizeof(qxp_current));

    qxp_owner.shader_owner = qxp_prepared.owner;
    qxp_owner.dd_local_identity = 0xABCUL;

    qxp_add_fake(0x900UL, QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
}

static void qxp_publish_raw(void)
{
    memcpy(&qxp_prepared.raw_state, &qxp_current, sizeof(qxp_current));
}

static void qxp_mutate_next_lock(DWORD mutation, DWORD value)
{
    qxp_mutate_at_acquire = qxp_lock_acquires + 1;
    qxp_mutation = mutation;
    qxp_mutation_value = value;
}

static BOOL qxp_snapshot(void)
{
    return QxpD3d9SnapshotDrawBackings(
        &qxp_prepared, &qxp_current, &qxp_owner, qxp_indexed, &qxp_um,
        qxp_backing_lookup, NULL, qxp_lock, qxp_unlock, NULL, &qxp_out,
        &qxp_status);
}

static int qxp_out_is_zero(void)
{
    const BYTE *bytes;
    DWORD index;

    bytes = (const BYTE *)&qxp_out;
    for (index = 0; index < (DWORD)sizeof(qxp_out); ++index) {
        if (bytes[index]) return 0;
    }
    return 1;
}

static void qxp_expect_fail(DWORD code, const char *name)
{
    BOOL result;

    memset(&qxp_out, 0xcc, sizeof(qxp_out));
    memset(&qxp_status, 0xcc, sizeof(qxp_status));
    result = qxp_snapshot();
    qxp_check(!result && qxp_status.code == code && qxp_out_is_zero(),
              name);
}

static void qxp_test_role_classification(void)
{
    DWORD role_mask;
    DWORD flags;

    role_mask = 0xaaaaaaaaUL;
    flags = 0xbbbbbbbbUL;
    qxp_check(!QxpD3d9ClassifyDrawBackingRoles(
                  FALSE, FALSE, FALSE, &role_mask, &flags) &&
                  role_mask == 0 && flags == 0,
              "untyped backing has no draw role");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  FALSE, TRUE, FALSE, &role_mask, &flags) &&
                  role_mask == QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX &&
                  flags == 0,
              "explicit-only vertex backing is finite and noncircular");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  FALSE, FALSE, TRUE, &role_mask, &flags) &&
                  role_mask == QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX &&
                  flags == 0,
              "explicit-only index backing is finite and noncircular");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  FALSE, TRUE, TRUE, &role_mask, &flags) &&
                  role_mask ==
                      (QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
                       QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX) &&
                  flags == 0,
              "explicit-only dual backing is finite and noncircular");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  TRUE, FALSE, FALSE, &role_mask, &flags) &&
                  role_mask ==
                      (QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
                       QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX) &&
                  flags == QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR,
              "execute-only backing retains circular dual role");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  TRUE, TRUE, FALSE, &role_mask, &flags) &&
                  role_mask == QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX &&
                  flags == 0,
              "explicit vertex role overrides compatibility execute cap");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  TRUE, FALSE, TRUE, &role_mask, &flags) &&
                  role_mask == QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX &&
                  flags == 0,
              "explicit index role overrides compatibility execute cap");
    qxp_check(QxpD3d9ClassifyDrawBackingRoles(
                  TRUE, TRUE, TRUE, &role_mask, &flags) &&
                  role_mask ==
                      (QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX |
                       QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX) &&
                  flags == 0,
              "explicit dual roles remain finite and noncircular");
    qxp_check(!QxpD3d9ClassifyDrawBackingRoles(
                  TRUE, TRUE, FALSE, NULL, &flags) &&
                  !QxpD3d9ClassifyDrawBackingRoles(
                      TRUE, TRUE, FALSE, &role_mask, NULL),
              "role classification rejects null outputs");
}

static void qxp_test_linear_buffer_extent(void)
{
    DWORD byte_length;

    byte_length = 0xaaaaaaaaUL;
    qxp_check(QxpD3d9ResolveLinearBufferExtent(
                  48, 0, &byte_length) &&
                  byte_length == 48,
              "system linear buffer uses exact positive pitch");
    qxp_check(QxpD3d9ResolveLinearBufferExtent(
                  48, 48, &byte_length) &&
                  byte_length == 48,
              "video linear buffer fits exact remaining aperture");
    byte_length = 0xaaaaaaaaUL;
    qxp_check(!QxpD3d9ResolveLinearBufferExtent(
                  48, 47, &byte_length) &&
                  byte_length == 0,
              "video linear buffer cannot exceed remaining aperture");
    byte_length = 0xaaaaaaaaUL;
    qxp_check(!QxpD3d9ResolveLinearBufferExtent(
                  0, 0, &byte_length) &&
                  byte_length == 0 &&
                  !QxpD3d9ResolveLinearBufferExtent(
                      -1, 0, &byte_length) &&
                  byte_length == 0,
              "nonpositive linear buffer pitch fails closed");
    qxp_check(!QxpD3d9ResolveLinearBufferExtent(48, 48, NULL),
              "linear buffer extent rejects null output");
}

static void qxp_test_incarnation(void)
{
    ULONGLONG clock;
    ULONGLONG result;
    QXP_D3D9_DRAW_BACKING_STATUS status;

    clock = 0;
    result = 0xdeadUL;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(&clock, 9, &result, &status) &&
                  result == 9 && clock == 0 &&
                  status.code == QXP_D3D9_DRAW_BACKING_OK,
              "existing incarnation preserved without advancing clock");

    clock = 0;
    result = 0;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(&clock, 0, &result, &status) &&
                  result == 1 && clock == 1,
              "fresh incarnation increments clock and is never zero");
    qxp_check(QxpD3d9AssignSurfaceIncarnation(&clock, 0, &result, &status) &&
                  result == 2 && clock == 2,
              "second fresh incarnation is strictly monotonic");

    /* Production owner lifecycle model: a live wrapper replacement preserves
     * identity, but DestroyDDLocal clears the slot so raw owner/handle reuse
     * consumes a distinct permanent incarnation. */
    clock = 0;
    result = 0;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(
                  &clock, 0, &result, &status) &&
                  result == 1 && clock == 1,
              "live owner resource receives first incarnation");
    qxp_check(QxpD3d9AssignSurfaceIncarnation(
                  &clock, result, &result, &status) &&
                  result == 1 && clock == 1,
              "same live owner handle wrapper preserves incarnation");
    result = 0;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(
                  &clock, 0, &result, &status) &&
                  result == 2 && clock == 2,
              "owner teardown and raw owner handle reuse gets new identity");

    clock = ~(ULONGLONG)0;
    result = 0x77UL;
    qxp_check(!QxpD3d9AssignSurfaceIncarnation(&clock, 0, &result, &status) &&
                  clock == ~(ULONGLONG)0 && result == 0x77UL &&
                  status.code ==
                      QXP_D3D9_DRAW_BACKING_ERROR_INCARNATION_EXHAUSTED &&
                  QxpD3d9DrawBackingStatusIsResourceFailure(&status),
              "exhausted clock fails permanently and modifies nothing");

    clock = ~(ULONGLONG)0;
    result = 0;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(&clock, 42, &result, &status) &&
                  result == 42 && clock == ~(ULONGLONG)0,
              "existing incarnation survives an exhausted clock");

    result = 0x99UL;
    qxp_check(!QxpD3d9AssignSurfaceIncarnation(NULL, 0, &result, &status) &&
                  result == 0x99UL &&
                  status.code == QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT,
              "null clock is a deterministic failure");
    clock = 5;
    qxp_check(!QxpD3d9AssignSurfaceIncarnation(&clock, 0, NULL, &status) &&
                  clock == 5 &&
                  status.code == QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT,
              "null result leaves the clock intact");
}

static void qxp_test_baseline_and_scalars(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;

    qxp_reset_baseline();
    qxp_current.streams[0].divider = 0x30UL;
    qxp_publish_raw();
    qxp_last_dd_identity = 0;
    qxp_check(qxp_snapshot() && qxp_status.code == QXP_D3D9_DRAW_BACKING_OK,
              "baseline single-stream snapshot succeeds");
    qxp_check(qxp_out.valid && qxp_out.required_stream_mask == 1UL &&
                  qxp_out.required_user_memory_stream_mask == 0UL &&
                  qxp_out.populated_stream_mask == 1UL &&
                  qxp_out.stream_source_generation == 7 && !qxp_out.indexed,
              "baseline snapshot masks and generation exact");
    qxp_check(qxp_out.owner.dd_local_identity == 0xABCUL &&
                  qxp_out.owner.shader_owner.context_handle == 0x41UL &&
                  qxp_out.owner.shader_owner.process_id == 0x51UL &&
                  qxp_last_dd_identity == 0xABCUL,
              "snapshot copies exact resource owner into lookup");
    qxp_check(qxp_out.streams[0].kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                  qxp_out.streams[0].role ==
                      QXP_D3D9_DRAW_BACKING_ROLE_VERTEX &&
                  qxp_out.streams[0].stream == 0 &&
                  qxp_out.streams[0].handle == 0x900UL &&
                  qxp_out.streams[0].incarnation == 5 &&
                  qxp_out.streams[0].content_generation == 3 &&
                  qxp_out.streams[0].byte_length == 4096 &&
                  qxp_out.streams[0].video_memory_offset == 0 &&
                  qxp_out.streams[0].flags == 0 &&
                  qxp_out.streams[0].stride == 20 &&
                  qxp_out.streams[0].source2_offset == 0 &&
                  qxp_out.streams[0].divider == 0x30UL &&
                  qxp_out.streams[0].required_span == 12,
              "baseline stream record scalar fields exact");

    qxp_reset_baseline();
    qxp_current.streams[0].source2 = TRUE;
    qxp_publish_raw();
    qxp_check(qxp_snapshot() &&
                  qxp_out.streams[0].source2_offset == 0,
              "SOURCE2 zero offset provenance accepted and copied");

    qxp_reset_baseline();
    qxp_prepared.mode = QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS;
    qxp_check(qxp_snapshot() && qxp_out.valid,
              "POSITIONT bypass prepared mode snapshots backings");

    qxp_reset_baseline();
    qxp_current.streams[0].offset = 100UL;
    qxp_current.streams[0].source2 = TRUE;
    qxp_publish_raw();
    metadata = &qxp_fakes[0].metadata;
    metadata->byte_length = 200UL;
    qxp_check(qxp_snapshot() && qxp_out.streams[0].source2_offset == 100UL &&
                  qxp_out.streams[0].byte_length == 200UL,
              "SOURCE2 offset copied and fits extent");

    qxp_current.streams[0].offset = 189UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_OVERFLOW,
                    "SOURCE2 short remaining span rejected");

    qxp_current.streams[0].offset = 201UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_OVERFLOW,
                    "SOURCE2 offset beyond extent rejected");

    qxp_current.streams[0].offset = 0xfffffff8UL;
    qxp_publish_raw();
    metadata->byte_length = 0xfffffffeUL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_OVERFLOW,
                    "SOURCE2 offset plus span arithmetic overflow rejected");
}

static void qxp_test_sixteen_streams(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;
    DWORD stream;

    qxp_reset_baseline();
    qxp_clear_fakes();
    memset(&qxp_current, 0, sizeof(qxp_current));
    qxp_current.stream_source_generation = 7;
    qxp_prepared.required_stream_mask = 0xffffUL;
    qxp_prepared.required_user_memory_stream_mask = 0UL;
    for (stream = 0; stream < 16; ++stream) {
        qxp_current.streams[stream].handle = 0x900UL + stream;
        qxp_current.streams[stream].stride = 32UL;
        qxp_current.streams[stream].divider = 1UL;
        qxp_prepared.required_stream_spans[stream] = 8UL + stream;
        metadata = qxp_add_fake(0x900UL + stream,
                                QXP_D3D9_DRAW_BACKING_ROLE_VERTEX);
        metadata->incarnation = 100UL + stream;
        metadata->content_generation = 200UL + stream;
        metadata->byte_length = 1000UL + stream;
    }
    qxp_publish_raw();
    qxp_check(qxp_snapshot() && qxp_out.populated_stream_mask == 0xffffUL,
              "all sixteen required streams populate");
    for (stream = 0; stream < 16; ++stream) {
        qxp_check(qxp_out.streams[stream].handle == 0x900UL + stream &&
                      qxp_out.streams[stream].incarnation ==
                          100UL + stream &&
                      qxp_out.streams[stream].content_generation ==
                          200UL + stream &&
                      qxp_out.streams[stream].byte_length ==
                          1000UL + stream &&
                      qxp_out.streams[stream].required_span ==
                          8UL + stream &&
                      qxp_out.streams[stream].stream == stream,
                  "sixteen-stream record scalars exact per stream");
    }
}

static void qxp_test_kinds_video_extent(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    metadata->video_memory_offset = 0x40000UL;
    qxp_check(qxp_snapshot() &&
                  qxp_out.streams[0].kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
                  qxp_out.streams[0].video_memory_offset == 0x40000UL,
              "video-linear backing keeps its video-memory offset");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->video_memory_offset = 4UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "system-linear backing rejects a video-memory offset");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    metadata->video_memory_offset = 0xfffffff0UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "video-memory offset plus extent overflow rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->byte_length = 0UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "zero extent rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->byte_length = QXP_D3D9_DRAW_BACKING_EXTENT_INFINITE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "non-finite extent rejected");
}

static void qxp_test_user_memory(void)
{
    qxp_reset_baseline();
    qxp_clear_fakes();
    qxp_current.streams[0].handle = 0UL;
    qxp_current.streams[0].offset = 0UL;
    qxp_current.streams[0].source2 = FALSE;
    qxp_current.streams[0].user_memory = TRUE;
    qxp_prepared.required_user_memory_stream_mask = 1UL;
    qxp_publish_raw();
    qxp_um.active = TRUE;
    qxp_um.nonnull_pointer_observed = TRUE;
    qxp_um.address_range_valid = TRUE;
    qxp_um.vertex_size = 20UL;
    qxp_um.vertex_length = 4UL;
    qxp_um.vertex_offset = 64UL;
    qxp_check(qxp_snapshot() &&
                  qxp_out.required_user_memory_stream_mask == 1UL &&
                  qxp_out.populated_stream_mask == 1UL &&
                  qxp_out.streams[0].kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL &&
                  qxp_out.streams[0].handle == 0 &&
                  qxp_out.streams[0].incarnation == 0 &&
                  qxp_out.streams[0].content_generation == 0 &&
                  qxp_out.streams[0].byte_length == 80,
              "user-memory stream resolves through observed envelope");

    qxp_um.active = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "inactive user-memory envelope rejected");
    qxp_um.active = TRUE;
    qxp_um.nonnull_pointer_observed = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "null user-memory pointer rejected");
    qxp_um.nonnull_pointer_observed = TRUE;
    qxp_um.address_range_valid = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "unvalidated user-memory address range rejected");
    qxp_um.address_range_valid = TRUE;
    qxp_um.vertex_length = 0UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "zero-length user memory rejected");
    qxp_um.vertex_length = 4UL;
    qxp_um.vertex_size = 2UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "user-memory bytes below span rejected");
    qxp_um.vertex_size = 0x10000000UL;
    qxp_um.vertex_length = 0x100UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "user-memory size times length overflow rejected");
    qxp_um.vertex_size = 0xffffffffUL;
    qxp_um.vertex_length = 1UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "unknown all-ones user-memory extent rejected");
    qxp_um.vertex_size = 20UL;
    qxp_um.vertex_length = 4UL;
    qxp_um.active = (BOOL)2;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "noncanonical user-memory active flag rejected");
    qxp_um.active = TRUE;
    qxp_um.nonnull_pointer_observed = (BOOL)2;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "noncanonical pointer-observed flag rejected");
    qxp_um.nonnull_pointer_observed = TRUE;
    qxp_um.address_range_valid = (BOOL)2;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY,
                    "noncanonical address-valid flag rejected");

    qxp_reset_baseline();
    qxp_prepared.required_user_memory_stream_mask = 1UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "user-memory mask without user-memory raw rejected");

    qxp_reset_baseline();
    qxp_clear_fakes();
    qxp_current.streams[0].handle = 0UL;
    qxp_current.streams[0].user_memory = TRUE;
    qxp_prepared.required_user_memory_stream_mask = 1UL;
    qxp_um.active = TRUE;
    qxp_um.nonnull_pointer_observed = TRUE;
    qxp_um.address_range_valid = TRUE;
    qxp_um.vertex_size = 20UL;
    qxp_um.vertex_length = 4UL;
    qxp_current.streams[0].handle = 0x900UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "user-memory stream with retained handle rejected");
    qxp_current.streams[0].handle = 0;
    qxp_current.streams[0].offset = 4UL;
    qxp_current.streams[0].source2 = TRUE;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "user-memory stream with SOURCE2 offset rejected");
}

static void qxp_test_indexed(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;

    qxp_reset_baseline();
    qxp_indexed = TRUE;
    qxp_current.indices.handle = 0x700UL;
    qxp_current.indices.stride = 2UL;
    qxp_publish_raw();
    metadata = qxp_add_fake(0x700UL, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
    metadata->byte_length = 64UL;
    qxp_check(qxp_snapshot() && qxp_out.indexed &&
                  qxp_out.index.kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                  qxp_out.index.role == QXP_D3D9_DRAW_BACKING_ROLE_INDEX &&
                  qxp_out.index.handle == 0x700UL &&
                  qxp_out.index.stride == 2 &&
                  qxp_out.index.byte_length == 64 &&
                  qxp_out.index.required_span == 2,
              "indexed draw resolves bound index buffer");

    qxp_current.indices.stride = 4UL;
    qxp_publish_raw();
    qxp_check(qxp_snapshot() && qxp_out.index.stride == 4,
              "index stride four accepted");

    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    metadata->video_memory_offset = 0x22000UL;
    qxp_check(qxp_snapshot() &&
                  qxp_out.index.kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
                  qxp_out.index.video_memory_offset == 0x22000UL,
              "video-linear index backing accepted with exact offset");
    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    metadata->video_memory_offset = 0;

    qxp_current.indices.stride = 3UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_INDEX_STATE,
                    "index stride other than 2 or 4 rejected");

    qxp_current.indices.stride = 2UL;
    qxp_current.indices.handle = 0UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_INDEX_STATE,
                    "indexed draw without a bound index rejected");

    qxp_current.indices.handle = 0x700UL;
    qxp_current.indices.stride = 4UL;
    qxp_publish_raw();
    metadata = &qxp_fakes[1].metadata;
    metadata->byte_length = 2UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_EXTENT,
                    "index extent below stride rejected");
    metadata->byte_length = 64UL;
    metadata->role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "index backing with wrong role rejected");
    metadata->role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
    metadata->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "circular index backing rejected");
    metadata->flags = 0;
    metadata->incarnation = 0;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "zero index incarnation rejected");

    qxp_reset_baseline();
    qxp_indexed = FALSE;
    qxp_current.indices.handle = 0x700UL;
    qxp_current.indices.stride = 2UL;
    qxp_publish_raw();
    qxp_check(qxp_snapshot() && !qxp_out.indexed &&
                  qxp_out.index.handle == 0,
              "nonindexed draw ignores an unrelated bound index");
}

static void qxp_test_bad_metadata(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;

    qxp_reset_baseline();
    qxp_fakes[0].present = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP,
                    "absent backing reported as a lookup failure");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->valid = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "invalid metadata flag rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->valid = (BOOL)2;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "noncanonical metadata valid flag rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->handle = 0x901UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "backing handle mismatch rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "backing missing the requested role bit rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_INVALID;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "invalid backing kind rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->incarnation = 0;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "zero incarnation rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->content_generation = 0;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "zero content generation rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_check(qxp_snapshot() &&
                  qxp_out.streams[0].flags ==
                      QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR,
              "ordinary stream retains circular backing authority");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_current.streams[0].source2 = TRUE;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "SOURCE2 stream cannot consume circular backing");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    metadata->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "video-linear backing cannot claim circular authority");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->flags = 0x80000000UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "unknown backing flags rejected");

    qxp_reset_baseline();
    metadata = &qxp_fakes[0].metadata;
    metadata->role_mask |= 0x80000000UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_METADATA,
                    "unknown backing role bits rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].handle = 0UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "non-user-memory stream requires a handle");
}

static void qxp_test_owner_and_prepared(void)
{
    qxp_reset_baseline();
    qxp_owner.dd_local_identity = 0UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_OWNER,
                    "zero DirectDraw local identity rejected");

    qxp_reset_baseline();
    qxp_owner.dd_local_identity = 0xDEFUL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP,
                    "foreign DirectDraw owner cannot resolve exact handle");

    qxp_reset_baseline();
    qxp_owner.shader_owner.process_id = 0x99UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_OWNER,
                    "resource owner must equal prepared shader owner");

    qxp_reset_baseline();
    qxp_prepared.valid = FALSE;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE,
                    "invalid prepared draw rejected");

    qxp_reset_baseline();
    qxp_prepared.mode = 0UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE,
                    "prepared draw with invalid mode rejected");

    qxp_reset_baseline();
    qxp_prepared.required_user_memory_stream_mask = 2UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE,
                    "user-memory mask outside stream zero rejected");

    qxp_reset_baseline();
    qxp_prepared.required_stream_mask = 0x10000UL;
    qxp_prepared.required_stream_spans[0] = 12UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE,
                    "out-of-range required stream mask rejected");

    qxp_reset_baseline();
    qxp_indexed = (BOOL)2;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE,
                    "noncanonical indexed classification rejected");

    qxp_reset_baseline();
    qxp_prepared.required_stream_spans[0] = 0;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "zero required stream span rejected independently");

    qxp_reset_baseline();
    memset(&qxp_out, 0xcc, sizeof(qxp_out));
    qxp_check(!QxpD3d9SnapshotDrawBackings(
                  NULL, &qxp_current, &qxp_owner, FALSE, &qxp_um,
                  qxp_backing_lookup, NULL, qxp_lock, qxp_unlock, NULL,
                  &qxp_out, &qxp_status) &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT &&
                  qxp_out_is_zero(),
              "null prepared argument rejected with zeroed output");
}

static void qxp_test_canonical_stream_state(void)
{
    qxp_reset_baseline();
    qxp_current.streams[0].stride = 0;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "zero required stream stride rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].stride = 8UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "required stream stride below span rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].user_memory = (BOOL)2;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "noncanonical stream user-memory flag rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].source2 = (BOOL)2;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "noncanonical SOURCE2 flag rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].divider = 0;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "zero stream divider rejected independently");

    qxp_reset_baseline();
    qxp_current.streams[0].divider =
        QXP_D3D9_STREAM_SOURCE_FLAG_MASK | 1UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "conflicting stream-frequency flags rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].offset = 4UL;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "legacy stream with SOURCE2 offset rejected");

    qxp_reset_baseline();
    qxp_current.streams[0].handle = 0;
    qxp_current.streams[0].user_memory = TRUE;
    qxp_publish_raw();
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE,
                    "raw user memory missing from prepared mask rejected");
}

static void qxp_test_state_changed(void)
{
    qxp_reset_baseline();
    qxp_current.streams[0].stride = 24UL;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
                    "live stream state change rejected");

    qxp_reset_baseline();
    qxp_current.stream_source_generation = 8;
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
                    "advanced stream-source generation (ABA) rejected");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_STATE_GENERATION, 8UL);
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
                    "generation mutation at lock acquisition rejected");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_STATE_UNBIND, 0);
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
                    "unbind at lock acquisition rejected");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_STATE_REBIND, 0x901UL);
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
                    "rebind at lock acquisition rejected");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_TABLE_TEARDOWN, 0);
    qxp_expect_fail(QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP,
                    "backing teardown at lock acquisition rejected");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_TABLE_INCARNATION, 0x55UL);
    qxp_check(qxp_snapshot() &&
                  qxp_out.streams[0].incarnation == 0x55UL,
              "incarnation mutation at acquisition is snapshotted exactly");

    qxp_reset_baseline();
    qxp_mutate_next_lock(QXP_MUTATE_TABLE_GENERATION, 0x66UL);
    qxp_check(qxp_snapshot() &&
                  qxp_out.streams[0].content_generation == 0x66UL,
              "content mutation at acquisition is snapshotted exactly");
}

static void qxp_test_validator(void)
{
    QXP_D3D9_DRAW_BACKING_SNAPSHOT saved;

    qxp_reset_baseline();
    qxp_check(qxp_snapshot() && qxp_out.valid,
              "prepare snapshot for validation");
    saved = qxp_out;
    qxp_check(QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  qxp_indexed, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_BACKING_OK,
              "validation of a fresh snapshot succeeds");

    saved.streams[0].incarnation ^= 1;
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  qxp_indexed, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION,
              "mutated snapshot rejected by validator");
    saved.streams[0].incarnation ^= 1;

    qxp_current.streams[0].stride = 99UL;
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  qxp_indexed, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED,
              "validator rejects stale live state via reconstruction");

    qxp_reset_baseline();
    qxp_check(qxp_snapshot(), "snapshot before same-handle ABA validation");
    saved = qxp_out;
    qxp_fakes[0].metadata.incarnation = 6UL;
    qxp_fakes[0].metadata.content_generation = 1UL;
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  FALSE, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION,
              "destroy and recreate same handle ABA rejected");

    qxp_reset_baseline();
    qxp_check(qxp_snapshot(), "snapshot before content validation");
    saved = qxp_out;
    qxp_fakes[0].metadata.content_generation = 4UL;
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  FALSE, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION,
              "same-incarnation content generation change rejected");

    qxp_reset_baseline();
    qxp_check(qxp_snapshot(), "snapshot before locked metadata mutation");
    saved = qxp_out;
    qxp_mutate_next_lock(QXP_MUTATE_TABLE_INCARNATION, 0x77UL);
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  FALSE, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION,
              "incarnation change at validation lock rejected");

    qxp_reset_baseline();
    qxp_check(qxp_snapshot(), "snapshot before locked teardown");
    saved = qxp_out;
    qxp_mutate_next_lock(QXP_MUTATE_TABLE_TEARDOWN, 0);
    qxp_check(!QxpD3d9ValidateDrawBackings(
                  &saved, &qxp_prepared, &qxp_current, &qxp_owner,
                  FALSE, &qxp_um, qxp_backing_lookup, NULL,
                  qxp_lock, qxp_unlock, NULL, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP,
              "teardown at validation lock rejected");
}

#define QXP_TEARDOWN_SURFACES 4UL
#define QXP_TEARDOWN_CONTEXTS 2UL

typedef struct qxp_fake_owner_context {
    ULONG_PTR dd_local_identity;
    DWORD stream_handles[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    DWORD stream_strides[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    BOOL stream0_user_memory;
    DWORD index_handle;
    DWORD index_stride;
    QXP_D3D9_DRAW_STATE draw_state;
    DWORD detach_calls;
} QXP_FAKE_OWNER_CONTEXT;

static QXP_D3D9_DRAW_BACKING_OWNER_SURFACE
    qxp_owner_surfaces[QXP_TEARDOWN_SURFACES];
static QXP_FAKE_OWNER_CONTEXT
    qxp_owner_contexts[QXP_TEARDOWN_CONTEXTS];
static DWORD qxp_owner_clear_calls;
static DWORD qxp_owner_callback_unlocked;
static DWORD qxp_owner_read_fail_index;

static BOOL qxp_owner_read_surface(
    DWORD index, QXP_D3D9_DRAW_BACKING_OWNER_SURFACE *surface,
    void *opaque)
{
    (void)opaque;
    if (!qxp_lock_depth) ++qxp_owner_callback_unlocked;
    if (index == qxp_owner_read_fail_index) return FALSE;
    if (index >= QXP_TEARDOWN_SURFACES || !surface) return FALSE;
    *surface = qxp_owner_surfaces[index];
    return TRUE;
}

static void qxp_owner_detach_context(
    DWORD index, const QXP_D3D9_DRAW_BACKING_OWNER_SURFACE *surface,
    void *opaque)
{
    QXP_D3D9_LEGACY_RESOURCE_VIEW legacy;
    QXP_FAKE_OWNER_CONTEXT *context;

    (void)opaque;
    if (!qxp_lock_depth) ++qxp_owner_callback_unlocked;
    if (index >= QXP_TEARDOWN_CONTEXTS || !surface) return;
    context = &qxp_owner_contexts[index];
    ++context->detach_calls;
    legacy.stream_handles = context->stream_handles;
    legacy.stream_strides = context->stream_strides;
    legacy.stream_count = QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
    legacy.stream0_user_memory = &context->stream0_user_memory;
    legacy.index_handle = &context->index_handle;
    legacy.index_stride = &context->index_stride;
    qxp_check(QxpD3d9UnbindOwnerContextResource(
                  surface->dd_local_identity,
                  context->dd_local_identity, surface->handle,
                  &legacy, &context->draw_state),
              "production teardown context view is valid");
}

static void qxp_owner_clear_surface(DWORD index, void *opaque)
{
    (void)opaque;
    if (!qxp_lock_depth) ++qxp_owner_callback_unlocked;
    if (index >= QXP_TEARDOWN_SURFACES) return;
    memset(&qxp_owner_surfaces[index], 0,
           sizeof(qxp_owner_surfaces[index]));
    ++qxp_owner_clear_calls;
}

static void qxp_reset_owner_teardown(void)
{
    QXP_FAKE_OWNER_CONTEXT *context;
    DWORD index;

    memset(qxp_owner_surfaces, 0, sizeof(qxp_owner_surfaces));
    memset(qxp_owner_contexts, 0, sizeof(qxp_owner_contexts));
    qxp_owner_clear_calls = 0;
    qxp_owner_callback_unlocked = 0;
    qxp_owner_read_fail_index = ~(DWORD)0;
    qxp_mutate_at_acquire = 0;
    qxp_mutation = QXP_MUTATE_NONE;

    qxp_owner_surfaces[0].valid = TRUE;
    qxp_owner_surfaces[0].dd_local_identity = 0xAAUL;
    qxp_owner_surfaces[0].surface_identity = 0x1001UL;
    qxp_owner_surfaces[0].handle = 0x900UL;
    qxp_owner_surfaces[0].incarnation = 1;
    qxp_owner_surfaces[0].texture_name = 10UL;
    qxp_owner_surfaces[1].valid = TRUE;
    qxp_owner_surfaces[1].dd_local_identity = 0xBBUL;
    qxp_owner_surfaces[1].surface_identity = 0x2001UL;
    qxp_owner_surfaces[1].handle = 0x900UL;
    qxp_owner_surfaces[1].incarnation = 2;
    qxp_owner_surfaces[1].texture_name = 20UL;
    qxp_owner_surfaces[2].valid = TRUE;
    qxp_owner_surfaces[2].dd_local_identity = 0xAAUL;
    qxp_owner_surfaces[2].surface_identity = 0x1002UL;
    qxp_owner_surfaces[2].handle = 0x901UL;
    qxp_owner_surfaces[2].incarnation = 3;
    qxp_owner_surfaces[2].texture_name = 10UL;
    qxp_owner_surfaces[3].valid = TRUE;
    qxp_owner_surfaces[3].dd_local_identity = 0xAAUL;
    qxp_owner_surfaces[3].surface_identity = 0x1003UL;
    qxp_owner_surfaces[3].handle = 0;
    qxp_owner_surfaces[3].incarnation = 4;
    qxp_owner_surfaces[3].texture_name = 30UL;

    for (index = 0; index < QXP_TEARDOWN_CONTEXTS; ++index) {
        context = &qxp_owner_contexts[index];
        context->dd_local_identity = index ? 0xBBUL : 0xAAUL;
        context->stream_handles[0] = 0x900UL;
        context->stream_strides[0] = 20UL;
        context->stream0_user_memory = FALSE;
        context->index_handle = 0x901UL;
        context->index_stride = 2UL;
        QxpD3d9DrawStateInitialize(&context->draw_state);
        context->draw_state.streams[0].handle = 0x900UL;
        context->draw_state.streams[0].stride = 20UL;
        context->draw_state.indices.handle = 0x901UL;
        context->draw_state.indices.stride = 2UL;
        context->draw_state.stream_source_generation = 5;
    }
}

static BOOL qxp_run_owner_teardown(
    DWORD *texture_names, DWORD texture_capacity, DWORD *texture_count,
    QXP_D3D9_DRAW_BACKING_STATUS *status)
{
    QXP_D3D9_DRAW_BACKING_OWNER_TEARDOWN_CALLBACKS callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.surface_count = QXP_TEARDOWN_SURFACES;
    callbacks.context_count = QXP_TEARDOWN_CONTEXTS;
    callbacks.read_surface = qxp_owner_read_surface;
    callbacks.detach_context = qxp_owner_detach_context;
    callbacks.clear_surface = qxp_owner_clear_surface;
    return QxpD3d9TeardownDrawBackingOwner(
        0xAAUL, &callbacks, texture_names, texture_capacity,
        texture_count, qxp_lock, qxp_unlock, NULL, status);
}

static void qxp_test_owner_teardown_seam(void)
{
    QXP_D3D9_DRAW_BACKING_STATUS status;
    QXP_FAKE_OWNER_CONTEXT *owned;
    QXP_FAKE_OWNER_CONTEXT *foreign;
    DWORD texture_names[QXP_TEARDOWN_SURFACES];
    DWORD texture_count;
    DWORD unique_count;
    DWORD index;
    DWORD locks_before;
    DWORD unlocks_before;
    ULONGLONG clock;
    ULONGLONG old_incarnation;
    ULONGLONG new_incarnation;

    qxp_reset_owner_teardown();
    memset(texture_names, 0, sizeof(texture_names));
    locks_before = qxp_lock_acquires;
    unlocks_before = qxp_lock_releases;
    qxp_check(qxp_run_owner_teardown(
                  texture_names, QXP_TEARDOWN_SURFACES,
                  &texture_count, &status) &&
                  status.code == QXP_D3D9_DRAW_BACKING_OK,
              "production owner teardown seam succeeds");
    qxp_check(qxp_lock_acquires == locks_before + 1 &&
                  qxp_lock_releases == unlocks_before + 1 &&
                  qxp_owner_callback_unlocked == 0,
              "owner teardown uses one lock and callbacks only inside it");
    qxp_check(qxp_owner_clear_calls == 3 && texture_count == 3 &&
                  !qxp_owner_surfaces[0].valid &&
                  qxp_owner_surfaces[1].valid &&
                  qxp_owner_surfaces[1].dd_local_identity == 0xBBUL &&
                  !qxp_owner_surfaces[2].valid &&
                  !qxp_owner_surfaces[3].valid,
              "teardown clears exact-owner including handle-zero reference");

    owned = &qxp_owner_contexts[0];
    foreign = &qxp_owner_contexts[1];
    qxp_check(owned->stream_handles[0] == 0 &&
                  owned->stream_strides[0] == 0 &&
                  owned->index_handle == 0 && owned->index_stride == 0 &&
                  owned->draw_state.streams[0].handle == 0 &&
                  owned->draw_state.indices.handle == 0 &&
                  owned->draw_state.stream_source_generation == 7,
              "same-owner legacy and B4 bindings detach with generations");
    qxp_check(foreign->stream_handles[0] == 0x900UL &&
                  foreign->index_handle == 0x901UL &&
                  foreign->draw_state.streams[0].handle == 0x900UL &&
                  foreign->draw_state.indices.handle == 0x901UL &&
                  foreign->draw_state.stream_source_generation == 5,
              "foreign-owner context remains byte-for-byte bound");
    qxp_check(owned->detach_calls == 3 && foreign->detach_calls == 3,
              "each exact surface visits every context under seam");

    unique_count = 0;
    for (index = 0; index < texture_count; ++index) {
        if (QxpD3d9TextureNameFirstOccurrence(
                texture_names, texture_count, index)) ++unique_count;
    }
    qxp_check(texture_names[0] == 10UL && texture_names[1] == 10UL &&
                  texture_names[2] == 30UL && unique_count == 2 &&
                  QxpD3d9TextureNameFirstOccurrence(
                      texture_names, texture_count, 0) &&
                  !QxpD3d9TextureNameFirstOccurrence(
                      texture_names, texture_count, 1),
              "host texture deletion collection deduplicates after unlock");

    locks_before = qxp_lock_acquires;
    texture_count = 0xdeadUL;
    qxp_check(qxp_run_owner_teardown(
                  texture_names, QXP_TEARDOWN_SURFACES,
                  &texture_count, &status) && texture_count == 0 &&
                  qxp_owner_clear_calls == 3 &&
                  qxp_lock_acquires == locks_before + 1,
              "repeated owner teardown is a locked no-op");

    qxp_reset_owner_teardown();
    memset(texture_names, 0, sizeof(texture_names));
    qxp_owner_read_fail_index = 2;
    locks_before = qxp_lock_acquires;
    unlocks_before = qxp_lock_releases;
    texture_count = 0;
    qxp_check(!qxp_run_owner_teardown(
                  texture_names, QXP_TEARDOWN_SURFACES,
                  &texture_count, &status) &&
                  status.code == QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN &&
                  texture_count == 1 && texture_names[0] == 10UL &&
                  qxp_owner_clear_calls == 1 &&
                  !qxp_owner_surfaces[0].valid &&
                  qxp_owner_surfaces[1].valid &&
                  qxp_owner_surfaces[2].valid &&
                  qxp_owner_surfaces[3].valid &&
                  qxp_owner_contexts[0].stream_handles[0] == 0 &&
                  qxp_owner_contexts[0].index_handle == 0x901UL &&
                  qxp_owner_contexts[0].draw_state.stream_source_generation ==
                      6 &&
                  qxp_lock_acquires == locks_before + 1 &&
                  qxp_lock_releases == unlocks_before + 1,
              "read failure reports exact cleaned prefix under one lock");
    qxp_owner_read_fail_index = ~(DWORD)0;
    memset(texture_names, 0, sizeof(texture_names));
    texture_count = 0;
    qxp_check(qxp_run_owner_teardown(
                  texture_names, QXP_TEARDOWN_SURFACES,
                  &texture_count, &status) &&
                  status.code == QXP_D3D9_DRAW_BACKING_OK &&
                  texture_count == 2 && texture_names[0] == 10UL &&
                  texture_names[1] == 30UL &&
                  qxp_owner_clear_calls == 3 &&
                  !qxp_owner_surfaces[2].valid &&
                  !qxp_owner_surfaces[3].valid &&
                  qxp_owner_contexts[0].index_handle == 0 &&
                  qxp_owner_contexts[0].draw_state.stream_source_generation ==
                      7,
              "retry after callback failure completes exact suffix");

    qxp_reset_owner_teardown();
    texture_count = 0xdeadUL;
    qxp_check(qxp_run_owner_teardown(
                  NULL, 0, &texture_count, &status) &&
                  texture_count == 0 && qxp_owner_clear_calls == 3 &&
                  !qxp_owner_surfaces[0].valid &&
                  qxp_owner_contexts[0].stream_handles[0] == 0 &&
                  qxp_owner_contexts[0].draw_state.streams[0].handle == 0,
              "texture scratch allocation failure cannot block teardown");

    qxp_reset_owner_teardown();
    texture_count = 0;
    qxp_check(!qxp_run_owner_teardown(
                  texture_names, 1, &texture_count, &status) &&
                  status.code ==
                      QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN_CAPACITY &&
                  texture_count == 1 && qxp_owner_clear_calls == 3,
              "short texture collection reports failure after full cleanup");

    clock = 0;
    old_incarnation = 0;
    new_incarnation = 0;
    qxp_check(QxpD3d9AssignSurfaceIncarnation(
                  &clock, 0, &old_incarnation, &status),
              "assign pre-teardown owner incarnation");
    qxp_reset_owner_teardown();
    qxp_owner_surfaces[0].incarnation = old_incarnation;
    qxp_check(qxp_run_owner_teardown(
                  NULL, 0, &texture_count, &status) &&
                  qxp_owner_surfaces[0].incarnation == 0 &&
                  QxpD3d9AssignSurfaceIncarnation(
                      &clock, qxp_owner_surfaces[0].incarnation,
                      &new_incarnation, &status) &&
                  old_incarnation != new_incarnation,
              "raw owner pointer and handle reuse receives fresh incarnation");
}

static void qxp_test_indexed_circular_stream(void)
{
    QXP_D3D9_DRAW_BACKING_METADATA *metadata;

    /* r532 evidence: the XP D3D8 software-VP runtime feeds transformed
     * DrawPrimitiveUP geometry to the driver as DRAWINDEXEDPRIMITIVE2
     * records over its internal circular system-memory execute buffer.  A
     * single-stream, non-SOURCE2 circular vertex backing must therefore be
     * admitted for an indexed draw exactly as for a nonindexed one; only
     * the index buffer itself stays linear. */
    qxp_reset_baseline();
    qxp_indexed = TRUE;
    qxp_current.indices.handle = 0x700UL;
    qxp_current.indices.stride = 2UL;
    qxp_publish_raw();
    qxp_add_fake(0x700UL, QXP_D3D9_DRAW_BACKING_ROLE_INDEX);
    metadata = &qxp_fakes[0].metadata;
    metadata->flags = QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
    qxp_check(qxp_snapshot() &&
                  qxp_out.indexed &&
                  qxp_out.streams[0].flags ==
                      QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR &&
                  qxp_out.index.handle == 0x700UL &&
                  !qxp_out.index.flags,
              "r532 indexed draw admits the circular stream-0 vertex ring");
}

int main(void)
{
    qxp_test_role_classification();
    qxp_test_linear_buffer_extent();
    qxp_test_incarnation();
    qxp_test_baseline_and_scalars();
    qxp_test_sixteen_streams();
    qxp_test_kinds_video_extent();
    qxp_test_user_memory();
    qxp_test_indexed();
    qxp_test_bad_metadata();
    qxp_test_indexed_circular_stream();
    qxp_test_owner_and_prepared();
    qxp_test_canonical_stream_state();
    qxp_test_state_changed();
    qxp_test_validator();
    qxp_test_owner_teardown_seam();

    qxp_check(qxp_lock_depth == 0 &&
                  qxp_lock_acquires == qxp_lock_releases &&
                  qxp_lock_max_depth <= 1 && qxp_lock_violations == 0,
              "exact lock balance and no nesting");
    qxp_check(qxp_lookup_calls > 0 &&
                  qxp_lookup_calls_while_unlocked == 0,
              "backing lookup runs only while the caller lock is held");

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw backing tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw backing tests: PASS\n");
    return 0;
}
