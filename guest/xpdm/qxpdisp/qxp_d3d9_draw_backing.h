#ifndef QXP_D3D9_DRAW_BACKING_H
#define QXP_D3D9_DRAW_BACKING_H

#include <windef.h>

#include "qxp_d3d9_draw_prepare.h"

/*
 * B6G1 resource-backing snapshot.  This portable module owns no allocation and
 * retains no caller pointer.  It validates, for one already prepared SM2 draw,
 * that every required vertex stream and (when indexed) the bound index buffer
 * resolves to a live backing of the exact draw owner with a nonzero permanent
 * incarnation, a nonzero content generation, and a finite base byte extent that
 * covers the shader-declared per-vertex span.  B6G1 deliberately validates
 * identity plus the finite base span only.  A circular execute-buffer tag is
 * retained only for an ordinary non-SOURCE2 vertex stream; later envelope and
 * capture validation must prove and materialize the exact wrapped access.
 * B6G1 does not validate per-record ranges, index values, pin/copy residency,
 * or execute any draw.
 */

#define QXP_D3D9_DRAW_BACKING_STREAM_COUNT QXP_D3D9_DRAW_STREAM_COUNT
#define QXP_D3D9_DRAW_BACKING_EXTENT_INFINITE 0xffffffffUL

typedef enum qxp_d3d9_draw_backing_kind {
    QXP_D3D9_DRAW_BACKING_KIND_INVALID = 0,
    QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR = 1,
    QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR = 2,
    QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL = 3
} QXP_D3D9_DRAW_BACKING_KIND;

/* Role identifiers double as single-bit role-mask values so a backing that
 * advertises several capabilities is checked with one mask test. */
typedef enum qxp_d3d9_draw_backing_role {
    QXP_D3D9_DRAW_BACKING_ROLE_VERTEX = 1,
    QXP_D3D9_DRAW_BACKING_ROLE_INDEX = 2
} QXP_D3D9_DRAW_BACKING_ROLE;

#define QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX 0x00000001UL
#define QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX  0x00000002UL

/* CIRCULAR marks an execute-buffer style system-memory ring.  It is admitted
 * only for an ordinary non-SOURCE2 vertex stream; capture materializes its
 * exact wrapped access into one immutable linear payload. */
#define QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR 0x00000001UL

/* Classifies the runtime's overlapping legacy execute-buffer and explicit
 * vertex/index-buffer tags.  An explicit role describes a finite standalone
 * linear buffer even when the compatibility execute-buffer bit is also set;
 * only an execute-only allocation retains circular ring authority. */
BOOL QxpD3d9ClassifyDrawBackingRoles(
    BOOL execute_buffer, BOOL vertex_buffer, BOOL index_buffer,
    DWORD *role_mask, DWORD *flags);

/* Windows 2000 and later report the exact byte length of a linear
 * execute/vertex/index buffer in lPitch.  A zero available_bytes means the
 * system-memory pointer has no aperture bound; a nonzero value bounds a video
 * allocation to the remaining mapped aperture.  Failure always zeroes the
 * output. */
BOOL QxpD3d9ResolveLinearBufferExtent(
    LONG pitch, DWORD available_bytes, DWORD *byte_length);

typedef enum qxp_d3d9_draw_backing_error_code {
    QXP_D3D9_DRAW_BACKING_OK = 0,
    QXP_D3D9_DRAW_BACKING_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_BACKING_ERROR_PREPARED_STATE = 2,
    QXP_D3D9_DRAW_BACKING_ERROR_OWNER = 3,
    QXP_D3D9_DRAW_BACKING_ERROR_STATE_CHANGED = 4,
    QXP_D3D9_DRAW_BACKING_ERROR_STREAM_STATE = 5,
    QXP_D3D9_DRAW_BACKING_ERROR_USER_MEMORY = 6,
    QXP_D3D9_DRAW_BACKING_ERROR_LOOKUP = 7,
    QXP_D3D9_DRAW_BACKING_ERROR_METADATA = 8,
    QXP_D3D9_DRAW_BACKING_ERROR_EXTENT = 9,
    QXP_D3D9_DRAW_BACKING_ERROR_OVERFLOW = 10,
    QXP_D3D9_DRAW_BACKING_ERROR_INDEX_STATE = 11,
    QXP_D3D9_DRAW_BACKING_ERROR_VALIDATION = 12,
    QXP_D3D9_DRAW_BACKING_ERROR_INCARNATION_EXHAUSTED = 13,
    QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN = 14,
    QXP_D3D9_DRAW_BACKING_ERROR_TEARDOWN_CAPACITY = 15
} QXP_D3D9_DRAW_BACKING_ERROR_CODE;

/* Deterministic status.  Only the fields relevant to the primary failure are
 * populated; every unrelated field stays zero. */
typedef struct qxp_d3d9_draw_backing_status {
    DWORD code;
    DWORD stream;
    DWORD handle;
    DWORD role;
} QXP_D3D9_DRAW_BACKING_STATUS;

/* Exact draw owner.  shader_owner is the copied numeric shader-object owner;
 * dd_local_identity is the opaque nonzero DirectDraw local identity.  Neither
 * value is ever dereferenced by this module. */
typedef struct qxp_d3d9_draw_backing_owner {
    QXP_D3D9_DRAW_OWNER_IDENTITY shader_owner;
    ULONG_PTR dd_local_identity;
} QXP_D3D9_DRAW_BACKING_OWNER;

/* Scalar metadata a lookup fills for one backing.  The module copies only the
 * scalar values it accepts; it never retains this structure. */
typedef struct qxp_d3d9_draw_backing_metadata {
    BOOL valid;
    DWORD handle;
    ULONGLONG incarnation;
    DWORD content_generation;
    DWORD byte_length;
    DWORD kind;
    DWORD role_mask;
    DWORD video_memory_offset;
    DWORD flags;
} QXP_D3D9_DRAW_BACKING_METADATA;

/* Scalar observation of a user-memory (system-supplied) stream-0 vertex
 * block.  The production caller reduces the transient address to these two
 * booleans after checking the complete 32-bit address range.  No pointer or
 * address enters this portable interface or the immutable snapshot. */
typedef struct qxp_d3d9_draw_backing_um_envelope {
    BOOL active;
    BOOL nonnull_pointer_observed;
    BOOL address_range_valid;
    DWORD vertex_size;
    DWORD vertex_length;
    DWORD vertex_offset;
} QXP_D3D9_DRAW_BACKING_UM_ENVELOPE;

/* One immutable, pointer-free backing record. */
typedef struct qxp_d3d9_draw_backing_record {
    DWORD kind;
    DWORD role;
    DWORD stream;
    DWORD handle;
    ULONGLONG incarnation;
    DWORD content_generation;
    DWORD byte_length;
    DWORD video_memory_offset;
    DWORD flags;
    DWORD stride;
    DWORD source2_offset;
    DWORD divider;
    DWORD required_span;
} QXP_D3D9_DRAW_BACKING_RECORD;

/* Immutable pointer-free draw-backing snapshot. */
typedef struct qxp_d3d9_draw_backing_snapshot {
    BOOL valid;
    QXP_D3D9_DRAW_BACKING_OWNER owner;
    ULONGLONG stream_source_generation;
    DWORD required_stream_mask;
    DWORD required_user_memory_stream_mask;
    DWORD populated_stream_mask;
    QXP_D3D9_DRAW_BACKING_RECORD streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    BOOL indexed;
    QXP_D3D9_DRAW_BACKING_RECORD index;
} QXP_D3D9_DRAW_BACKING_SNAPSHOT;

/* A lookup resolves one exact-owner handle in a given role while the caller's
 * table lock is held by this module.  It must fill metadata and return TRUE
 * only when a matching live backing exists.  The module independently
 * re-validates every scalar it returns and never trusts the boolean alone. */
typedef BOOL (*QXP_D3D9_DRAW_BACKING_LOOKUP_CALLBACK)(
    const QXP_D3D9_DRAW_BACKING_OWNER *owner, DWORD handle, DWORD role,
    QXP_D3D9_DRAW_BACKING_METADATA *metadata, void *opaque);

/* Assigns a permanent, nonwrapping, never-zero surface incarnation.  A nonzero
 * existing incarnation is preserved without advancing the clock.  A fresh
 * assignment pre-increments the module-lifetime clock and issues that value.
 * At the maximum clock a fresh assignment fails permanently and modifies
 * neither the clock nor the result.  Null clock/result is a deterministic
 * failure. */
BOOL QxpD3d9AssignSurfaceIncarnation(
    ULONGLONG *clock, ULONGLONG existing, ULONGLONG *result,
    QXP_D3D9_DRAW_BACKING_STATUS *status);

/* Builds one draw-backing snapshot.  Performs no allocation.  Acquires the
 * caller lock exactly once, byte-compares current_state against the prepared
 * raw state, resolves every required stream and (when indexed) the bound index
 * buffer through lookup, then publishes into out only after fully succeeding
 * and unlocking.  Every failure unlocks, zeroes out, and reports status. */
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
    QXP_D3D9_DRAW_BACKING_STATUS *status);

/* Reconstructs one canonical snapshot from the same inputs and byte-compares
 * every pointer-free byte against the supplied snapshot.  No allocation. */
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
    QXP_D3D9_DRAW_BACKING_STATUS *status);

BOOL QxpD3d9DrawBackingStatusIsResourceFailure(
    const QXP_D3D9_DRAW_BACKING_STATUS *status);

/* Portable production-equivalent owner teardown seam.  The caller supplies
 * scalar table accessors; this module owns exact-owner selection, one-lock
 * iteration, context detachment ordering, optional texture-name collection,
 * and repeated-teardown behavior. */
typedef struct qxp_d3d9_draw_backing_owner_surface {
    BOOL valid;
    ULONG_PTR dd_local_identity;
    ULONG_PTR surface_identity;
    DWORD handle;
    ULONGLONG incarnation;
    DWORD texture_name;
} QXP_D3D9_DRAW_BACKING_OWNER_SURFACE;

typedef BOOL (*QXP_D3D9_DRAW_BACKING_READ_SURFACE_CALLBACK)(
    DWORD index, QXP_D3D9_DRAW_BACKING_OWNER_SURFACE *surface,
    void *opaque);
typedef void (*QXP_D3D9_DRAW_BACKING_DETACH_CONTEXT_CALLBACK)(
    DWORD index, const QXP_D3D9_DRAW_BACKING_OWNER_SURFACE *surface,
    void *opaque);
typedef void (*QXP_D3D9_DRAW_BACKING_CLEAR_SURFACE_CALLBACK)(
    DWORD index, void *opaque);

typedef struct qxp_d3d9_draw_backing_owner_teardown_callbacks {
    DWORD surface_count;
    DWORD context_count;
    QXP_D3D9_DRAW_BACKING_READ_SURFACE_CALLBACK read_surface;
    QXP_D3D9_DRAW_BACKING_DETACH_CONTEXT_CALLBACK detach_context;
    QXP_D3D9_DRAW_BACKING_CLEAR_SURFACE_CALLBACK clear_surface;
    void *opaque;
} QXP_D3D9_DRAW_BACKING_OWNER_TEARDOWN_CALLBACKS;

BOOL QxpD3d9TeardownDrawBackingOwner(
    ULONG_PTR dd_local_identity,
    const QXP_D3D9_DRAW_BACKING_OWNER_TEARDOWN_CALLBACKS *callbacks,
    DWORD *texture_names, DWORD texture_capacity, DWORD *texture_count,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock, void *lock_opaque,
    QXP_D3D9_DRAW_BACKING_STATUS *status);

/* Shared exact legacy+B4 unbind used by production and its portable oracle.
 * Same-owner nonzero teardown always advances B4's saturating sequence, even
 * when the handle was not previously bound. */
BOOL QxpD3d9UnbindOwnerContextResource(
    ULONG_PTR teardown_dd_local_identity,
    ULONG_PTR context_dd_local_identity,
    DWORD handle,
    const QXP_D3D9_LEGACY_RESOURCE_VIEW *legacy,
    QXP_D3D9_DRAW_STATE *draw_state);

/* TRUE only for the first occurrence at index; used outside the table lock to
 * deduplicate collected host texture deletion. */
BOOL QxpD3d9TextureNameFirstOccurrence(
    const DWORD *texture_names, DWORD texture_count, DWORD index);

#endif
