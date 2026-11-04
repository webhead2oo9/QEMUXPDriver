#ifndef QXP_D3D9_DRAW_SOURCE_H
#define QXP_D3D9_DRAW_SOURCE_H

#include <windef.h>

#include "qxp_d3d9_draw_backing.h"
#include "qxp_d3d9_draw_lifetime.h"

/*
 * Transient, guarded address resolution for surface-backed draw sources.
 * The scalar primitive owns one token; the complete-set helpers coordinate up
 * to sixteen tokens without allocation or copying.  Their live caller is the
 * capture branch of qxp_d3d9_draw_backing_inspect in directdraw.c.  That
 * branch cannot run in a shipping configuration while
 * SM2/DDI-v4 capabilities remain declined, because the stock runtime then
 * never creates the shader objects a capture-eligible draw requires.
 */

typedef enum qxp_d3d9_draw_source_result {
    QXP_D3D9_DRAW_SOURCE_OK = 0,
    QXP_D3D9_DRAW_SOURCE_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_CLOSED = 2,
    QXP_D3D9_DRAW_SOURCE_REFUSED_LIFETIME_EPOCH_EXHAUSTED = 3,
    QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_PENDING = 4,
    QXP_D3D9_DRAW_SOURCE_REFUSED_MUTATION_ACTIVE = 5,
    QXP_D3D9_DRAW_SOURCE_REFUSED_READER_COUNT_EXHAUSTED = 6,
    QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_ADMISSION = 7,
    QXP_D3D9_DRAW_SOURCE_ERROR_RESOLVE = 8,
    /* Retained numeric slot in the diagnostic result ABI; no longer emitted. */
    QXP_D3D9_DRAW_SOURCE_ERROR_VIDEO_LINEAR = 9,
    QXP_D3D9_DRAW_SOURCE_ERROR_BACKING_KIND = 10,
    QXP_D3D9_DRAW_SOURCE_ERROR_HOST_AUTHORITATIVE = 11,
    QXP_D3D9_DRAW_SOURCE_ERROR_LOCK_ACTIVE = 12,
    QXP_D3D9_DRAW_SOURCE_ERROR_HANDLE_MISMATCH = 13,
    QXP_D3D9_DRAW_SOURCE_ERROR_INCARNATION_MISMATCH = 14,
    QXP_D3D9_DRAW_SOURCE_ERROR_CONTENT_GENERATION_MISMATCH = 15,
    QXP_D3D9_DRAW_SOURCE_ERROR_KIND_MISMATCH = 16,
    QXP_D3D9_DRAW_SOURCE_ERROR_BYTE_LENGTH_MISMATCH = 17,
    QXP_D3D9_DRAW_SOURCE_ERROR_VIDEO_MEMORY_OFFSET_MISMATCH = 18,
    QXP_D3D9_DRAW_SOURCE_ERROR_FLAGS_MISMATCH = 19,
    QXP_D3D9_DRAW_SOURCE_ERROR_NULL_SURFACE_IDENTITY = 20,
    QXP_D3D9_DRAW_SOURCE_ERROR_NULL_BASE = 21,
    QXP_D3D9_DRAW_SOURCE_ERROR_ZERO_BYTE_COUNT = 22,
    QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT_OVERFLOW = 23,
    QXP_D3D9_DRAW_SOURCE_ERROR_EXTENT = 24,
    QXP_D3D9_DRAW_SOURCE_ERROR_SURFACE_IDENTITY_MISMATCH = 25,
    QXP_D3D9_DRAW_SOURCE_ERROR_BASE_ADDRESS_MISMATCH = 26,
    QXP_D3D9_DRAW_SOURCE_ERROR_BASE_BYTE_COUNT_MISMATCH = 27,
    QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_REVALIDATION = 28,
    QXP_D3D9_DRAW_SOURCE_ERROR_NOT_ACTIVE = 29,
    QXP_D3D9_DRAW_SOURCE_ERROR_LIFETIME_RELEASE = 30
} QXP_D3D9_DRAW_SOURCE_RESULT;

/* One complete table observation.  surface_identity names the transient
 * PDD_SURFACE_LOCAL wrapper but is never dereferenced by this module. */
typedef struct qxp_d3d9_draw_source_observed {
    DWORD kind;
    DWORD handle;
    ULONGLONG incarnation;
    DWORD content_generation;
    DWORD byte_length;
    DWORD video_memory_offset;
    DWORD flags;
    ULONG_PTR surface_identity;
    BOOL host_authoritative;
    BOOL lock_active;
    const BYTE *base_address;
    DWORD byte_count;
} QXP_D3D9_DRAW_SOURCE_OBSERVED;

typedef BOOL (*QXP_D3D9_DRAW_SOURCE_RESOLVE_CALLBACK)(
    DWORD handle, QXP_D3D9_DRAW_SOURCE_OBSERVED *out, void *opaque);

/* Caller-owned, call-scoped token.  The address and callback pointers are
 * transient and must never enter an immutable backing snapshot or capture
 * blob.  observed is retained in full so post-copy validation includes the
 * PDD_SURFACE_LOCAL wrapper identity as well as backing generations. */
typedef struct qxp_d3d9_draw_source_token {
    const BYTE *base_address;
    DWORD byte_count;
    ULONG gate_epoch;
    QXP_D3D9_DRAW_SOURCE_OBSERVED observed;
    QXP_D3D9_DRAW_LIFETIME_GATE *gate;
    QXP_D3D9_DRAW_LIFETIME_READER reader;
    QXP_D3D9_DRAW_SOURCE_RESOLVE_CALLBACK resolve;
    void *resolve_opaque;
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback;
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback;
    void *lock_opaque;
} QXP_D3D9_DRAW_SOURCE_TOKEN;

/* One fixed, caller-owned complete-set transaction.  active_stream_mask is
 * the exact set of tokens still holding lifetime readers.  Tokens remain
 * transient and must never enter a capture blob or transport packet. */
typedef struct qxp_d3d9_draw_source_set {
    DWORD active_stream_mask;
    QXP_D3D9_DRAW_SOURCE_TOKEN
        tokens[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
} QXP_D3D9_DRAW_SOURCE_SET;

/* Primary identifies the acquisition or revalidation failure.  Cleanup is
 * populated only when releasing the already-admitted remainder also fails.
 * On an ordinary explicit release, a release failure is primary. */
#define QXP_D3D9_DRAW_SOURCE_STREAM_MASK 0x0000ffffUL

typedef struct qxp_d3d9_draw_source_set_status {
    DWORD result;
    DWORD stream;
    DWORD cleanup_result;
    DWORD cleanup_stream;
} QXP_D3D9_DRAW_SOURCE_SET_STATUS;

/* Production-shared writable-Unlock transition seam.  reserve_lock snapshots
 * the old lock and publishes an exact per-entry reservation without clearing
 * it.  begin_mutation then drains source readers; clear_old_lock runs only
 * afterward and must retain the reservation.  finish_reservation removes that
 * exact reservation after optional generation publication. */
typedef BOOL (*QXP_D3D9_DRAW_UNLOCK_RESERVE_CALLBACK)(
    BOOL *lock_active, DWORD *lock_flags, void *opaque);
typedef BOOL (*QXP_D3D9_DRAW_UNLOCK_BEGIN_CALLBACK)(void *opaque);
typedef void (*QXP_D3D9_DRAW_UNLOCK_CLEAR_CALLBACK)(void *opaque);
typedef BOOL (*QXP_D3D9_DRAW_UNLOCK_PRESENT_CALLBACK)(
    BOOL *matched, void *opaque);
typedef BOOL (*QXP_D3D9_DRAW_UNLOCK_PUBLISH_CALLBACK)(void *opaque);
typedef void (*QXP_D3D9_DRAW_UNLOCK_FINISH_CALLBACK)(void *opaque);
typedef void (*QXP_D3D9_DRAW_UNLOCK_END_CALLBACK)(void *opaque);

typedef struct qxp_d3d9_draw_unlock_callbacks {
    QXP_D3D9_DRAW_UNLOCK_RESERVE_CALLBACK reserve_lock;
    QXP_D3D9_DRAW_UNLOCK_BEGIN_CALLBACK begin_mutation;
    QXP_D3D9_DRAW_UNLOCK_CLEAR_CALLBACK clear_old_lock;
    QXP_D3D9_DRAW_UNLOCK_PRESENT_CALLBACK present;
    QXP_D3D9_DRAW_UNLOCK_PUBLISH_CALLBACK publish_generation;
    QXP_D3D9_DRAW_UNLOCK_FINISH_CALLBACK finish_reservation;
    QXP_D3D9_DRAW_UNLOCK_END_CALLBACK end_mutation;
    void *opaque;
} QXP_D3D9_DRAW_UNLOCK_CALLBACKS;

typedef struct qxp_d3d9_draw_unlock_result {
    BOOL reserved;
    BOOL mutation_end_required;
    BOOL lock_active;
    DWORD lock_flags;
    BOOL present_result;
    BOOL matched;
    BOOL generation_available;
} QXP_D3D9_DRAW_UNLOCK_RESULT;

/* Production-shared global callback gate.  XP may attach multiple local
 * DirectDraw owners to one device-global object, so owner_identity names only
 * a currently executing terminal teardown.  Ordinary operations are linked
 * global-scoped readers.  The links point only to call-scoped stack tokens and
 * are protected by the production table lock.  An idle gate retains only its
 * device-global key and generation for constant-time reuse. */
typedef enum qxp_d3d9_draw_owner_gate_result {
    QXP_D3D9_DRAW_OWNER_GATE_OK = 0,
    QXP_D3D9_DRAW_OWNER_GATE_REFUSED = 1,
    QXP_D3D9_DRAW_OWNER_GATE_REVOKED = 2,
    QXP_D3D9_DRAW_OWNER_GATE_FATAL = 3
} QXP_D3D9_DRAW_OWNER_GATE_RESULT;

typedef struct qxp_d3d9_draw_owner_gate_token
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN;

typedef struct qxp_d3d9_draw_owner_gate {
    ULONG_PTR owner_identity;
    ULONG_PTR global_identity;
    ULONG_PTR teardown_thread_identity;
    DWORD generation;
    DWORD teardown_users;
    DWORD revoking;
    DWORD in_flight;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *operations;
} QXP_D3D9_DRAW_OWNER_GATE;

struct qxp_d3d9_draw_owner_gate_token {
    ULONG_PTR owner_identity;
    ULONG_PTR global_identity;
    ULONG_PTR thread_identity;
    DWORD generation;
    DWORD slot;
    BOOL revoked;
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *next;
};

QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateBeginOperation(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity, DWORD slot,
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token);
QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateValidateOperation(
    const QXP_D3D9_DRAW_OWNER_GATE *gate,
    const QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token);
QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateEndOperation(
    QXP_D3D9_DRAW_OWNER_GATE *gate,
    QXP_D3D9_DRAW_OWNER_GATE_TOKEN *token);
/* Terminal admission closes normal callbacks but does not wait.  Production
 * drains other-thread operations, then revokes only impossible same-thread
 * recursion before it synchronously removes every local-owner pointer. */
QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateBeginTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity);
BOOL QxpD3d9DrawOwnerGateTeardownOperationCounts(
    const QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR thread_identity,
    DWORD *same_thread, DWORD *other_threads);
QXP_D3D9_DRAW_OWNER_GATE_RESULT
QxpD3d9DrawOwnerGateRevokeThreadOperations(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR thread_identity);
/* Completion removes terminal owner authority immediately.  If revoked stack
 * tokens remain, the owner-free gate stays closed until their benign End calls
 * reclaim it; otherwise completion reclaims the slot immediately. */
QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateCompleteTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity, ULONG_PTR thread_identity);
QXP_D3D9_DRAW_OWNER_GATE_RESULT
QxpD3d9DrawOwnerGateForceCompleteTeardown(
    QXP_D3D9_DRAW_OWNER_GATE *gate, ULONG_PTR owner_identity,
    ULONG_PTR global_identity);
QXP_D3D9_DRAW_OWNER_GATE_RESULT QxpD3d9DrawOwnerGateFatalClose(
    QXP_D3D9_DRAW_OWNER_GATE *gate);
BOOL QxpD3d9DrawOwnerGateBlocksIdentity(
    const QXP_D3D9_DRAW_OWNER_GATE *gate,
    ULONG_PTR owner_identity, ULONG_PTR global_identity);

/* Attempts lifetime admission once and never waits.  On success, resolves
 * under the injected lock and publishes a token.  Every failure after reader
 * admission releases that reader before returning and leaves token zero. */
QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9AcquireDrawSource(
    const QXP_D3D9_DRAW_BACKING_RECORD *expected,
    QXP_D3D9_DRAW_LIFETIME_GATE *gate,
    QXP_D3D9_DRAW_SOURCE_RESOLVE_CALLBACK resolve,
    void *resolve_opaque,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK lock_callback,
    QXP_D3D9_DRAW_LIFETIME_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_SOURCE_TOKEN *token);

/* Re-resolves under the original injected lock, requires a byte-for-byte
 * equivalent field observation, then revalidates the admitted gate epoch.
 * Failure releases the reader and zeroes token; success leaves it admitted. */
QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9RevalidateDrawSource(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token);

/* Releases one successful transaction.  A second release is rejected without
 * calling the lifetime gate again. */
QXP_D3D9_DRAW_SOURCE_RESULT QxpD3d9ReleaseDrawSource(
    QXP_D3D9_DRAW_SOURCE_TOKEN *token);

/* Acquires every selected surface-linear backing in ascending stream order.
 * Both system and video-memory linear resources are supported. A zero mask is
 * a successful empty transaction. Any failure releases the admitted prefix in
 * reverse order and leaves set wholly inactive. */
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
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status);

/* Revalidates all active tokens in ascending stream order.  A failure releases
 * that token through the scalar primitive and the complete remaining set in
 * reverse order, so FALSE always leaves set inactive.  TRUE preserves every
 * admitted reader until the explicit complete-set release. */
BOOL QxpD3d9RevalidateDrawSourceSet(
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status);

/* Releases every active token in reverse stream order and always attempts the
 * whole set.  TRUE leaves an empty set with no release failure. */
BOOL QxpD3d9ReleaseDrawSourceSet(
    QXP_D3D9_DRAW_SOURCE_SET *set,
    QXP_D3D9_DRAW_SOURCE_SET_STATUS *status);

/* Fatal classifier shared by the portable oracle and the display inspector.
 * Admission/refusal and ordinary identity failures remain observational while
 * an impossible reader-release failure must fail the draw closed. */
BOOL QxpD3d9DrawSourceSetStatusHasReleaseFailure(
    const QXP_D3D9_DRAW_SOURCE_SET_STATUS *status);

/* Shared table-arbitration policy.  Lock, Unlock, destruction, registration,
 * and terminal owner teardown all call these predicates before mutating an
 * entry, so exact reservations cannot be erased by an overlapping operation.
 * No-match Lock/Unlock surfaces retain legacy untracked behavior. */
BOOL QxpD3d9DrawSourceLockCanReserve(
    DWORD matching_entries, DWORD active_entries,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns);
BOOL QxpD3d9DrawSourceUnlockCanReserve(
    DWORD matching_entries, DWORD lock_preparations,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns);
BOOL QxpD3d9DrawSourceDestroyCanReserve(
    DWORD matching_entries, DWORD active_entries,
    DWORD unlock_reservations, DWORD destroy_reservations,
    DWORD owner_teardowns);
BOOL QxpD3d9DrawSourceRegistrationCanProceed(
    DWORD active_entries, DWORD unlock_reservations,
    DWORD destroy_reservations, DWORD owner_teardowns);
/* FALSE means terminal owner teardown has not drained yet.  Active mappings
 * need not drain, but ordinary callbacks still preparing a mapping and exact
 * Unlock/Destroy transactions must finish before synchronous logical clear.
 * Revoked same-thread transactions are cleared rather than waited upon. */
BOOL QxpD3d9DrawSourceOwnerTeardownIsDrained(
    DWORD owner_entries, DWORD owner_operations,
    DWORD lock_preparations, DWORD unlock_reservations,
    DWORD destroy_reservations);

/* Strictly monotonic, nonwrapping reservation identity. */
BOOL QxpD3d9DrawSourceNextUnlockReservation(
    DWORD current, DWORD *next);

/* Executes the exact reserve/begin/clear/present/publish/finish/end sequence.
 * FALSE means reservation failed or arguments were invalid and no mutation
 * callback ran.  Once reserved, every path finishes the reservation and ends
 * a counted mutation exactly once. */
BOOL QxpD3d9CompleteDrawSourceUnlock(
    DWORD read_only_flag,
    const QXP_D3D9_DRAW_UNLOCK_CALLBACKS *callbacks,
    QXP_D3D9_DRAW_UNLOCK_RESULT *result);

#endif
