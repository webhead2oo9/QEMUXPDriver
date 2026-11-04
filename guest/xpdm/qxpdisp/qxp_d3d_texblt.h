#ifndef QXP_D3D_TEXBLT_H
#define QXP_D3D_TEXBLT_H

#define QXP_D3D_TEXBLT_FORMAT_A8R8G8B8 21UL
#define QXP_D3D_TEXBLT_FORMAT_X8R8G8B8 22UL
#define QXP_D3D_TEXBLT_FORMAT_R5G6B5 23UL
#define QXP_D3D_TEXBLT_FORMAT_A8P8 40UL
#define QXP_D3D_TEXBLT_FORMAT_P8 41UL
#define QXP_D3D_TEXBLT_MAX_DIMENSION 2048UL

#define QXP_D3D_TEXBLT_SUCCESS 0UL
#define QXP_D3D_TEXBLT_REJECT_BATCH_SHORT 1UL
#define QXP_D3D_TEXBLT_REJECT_FLAGS 2UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_HANDLE 3UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_HANDLE 4UL
#define QXP_D3D_TEXBLT_REJECT_SELF_COPY 5UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_NOT_TEXTURE 6UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_NOT_TEXTURE 7UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_CUBE 8UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_CUBE 9UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_MIP 10UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_MIP 11UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_SYSTEM_MEMORY 12UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_GEOMETRY 13UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_GEOMETRY 14UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_FORMAT 15UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_FORMAT 16UL
#define QXP_D3D_TEXBLT_REJECT_FORMAT_MISMATCH 17UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_EMPTY 18UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_BOUNDS 19UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_POINT 20UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUNDS 21UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_IDENTITY 22UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_IDENTITY 23UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUND 24UL
#define QXP_D3D_TEXBLT_REJECT_SOURCE_PREPARE 25UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_PREPARE 26UL
#define QXP_D3D_TEXBLT_REJECT_STALE 27UL
#define QXP_D3D_TEXBLT_REJECT_TRANSPORT 28UL
#define QXP_D3D_TEXBLT_REJECT_DESTINATION_STALE 29UL
#define QXP_D3D_TEXBLT_REJECT_INTERNAL 30UL
#define QXP_D3D_TEXBLT_REJECT_PALETTE_MISMATCH 31UL
#define QXP_D3D_TEXBLT_REJECT_CUBE_BATCH 32UL

#define QXP_D3D_TEXBLT_CUBE_FACE_COUNT 6UL
#define QXP_D3D_TEXBLT_CUBE_FACE_MASK 0x3fUL

/* One DDI command covers two genuinely different operations.  A copy whose
 * source is a system-memory staging surface covering the whole destination is
 * an upload, and the driver already uploads texture levels in a format-
 * agnostic way, so it need not be restricted to the colour formats a host
 * framebuffer blit can attach.  A video-to-video copy is a real blit and keeps
 * that restriction.  Routing them apart is what removes a limitation that did
 * not need to exist, rather than relaxing a check. */
#define QXP_D3D_TEXBLT_ROUTE_BLIT 0UL
#define QXP_D3D_TEXBLT_ROUTE_UPLOAD 1UL

#define QXP_D3D_TEXBLT_TRANSACTION_SUCCESS 0UL
#define QXP_D3D_TEXBLT_TRANSACTION_UPLOAD_FAILED 1UL
#define QXP_D3D_TEXBLT_TRANSACTION_COMMIT_FAILED 2UL
#define QXP_D3D_TEXBLT_TRANSACTION_INVALID 3UL

#define QXP_D3D_TEXBLT_REGISTRATION_REFUSE 0UL
#define QXP_D3D_TEXBLT_REGISTRATION_PRESERVE 1UL
#define QXP_D3D_TEXBLT_REGISTRATION_RECREATE 2UL
#define QXP_D3D_TEXBLT_REGISTRATION_INSERT 3UL

typedef int (*QXP_D3D_TEXBLT_TRANSACTION_CALLBACK)(
    void *opaque, unsigned long scratch_name);
typedef void (*QXP_D3D_TEXBLT_TRANSACTION_CLEANUP_CALLBACK)(
    void *opaque, unsigned long scratch_name);

/* Byte-identical portable spelling of D3DNTHAL_DP2TEXBLT. */
typedef struct qxp_d3d_texblt_record {
    unsigned long destination_handle;
    unsigned long source_handle;
    signed long destination_x;
    signed long destination_y;
    signed long source_left;
    signed long source_top;
    signed long source_right;
    signed long source_bottom;
    unsigned long flags;
} QXP_D3D_TEXBLT_RECORD;

typedef struct qxp_d3d_texblt_surface {
    unsigned long resolved;
    unsigned long is_texture;
    unsigned long is_system_memory;
    unsigned long is_cube;
    unsigned long is_mipmapped;
    unsigned long is_bound_destination;
    unsigned long width;
    unsigned long height;
    unsigned long format;
    unsigned long format_valid;
    unsigned long palette_handle;
    unsigned long palette_flags;
} QXP_D3D_TEXBLT_SURFACE;

typedef struct qxp_d3d_texblt_plan {
    signed long source_left;
    signed long source_top;
    signed long source_right;
    signed long source_bottom;
    signed long destination_left;
    signed long destination_top;
    signed long destination_right;
    signed long destination_bottom;
    unsigned long route;
} QXP_D3D_TEXBLT_PLAN;

/* Portable, scalar view of one prevalidated cube-face record.  The driver
 * derives the resource identities from the canonical positive-X roots while
 * every surface wrapper is lifetime-pinned. */
typedef struct qxp_d3d_texblt_cube_member {
    unsigned long source_resource;
    unsigned long destination_resource;
    signed long source_face;
    signed long destination_face;
} QXP_D3D_TEXBLT_CUBE_MEMBER;

unsigned long QxpD3dTexBltRunTransaction(
    unsigned long scratch_name,
    QXP_D3D_TEXBLT_TRANSACTION_CALLBACK upload_callback,
    QXP_D3D_TEXBLT_TRANSACTION_CALLBACK commit_callback,
    QXP_D3D_TEXBLT_TRANSACTION_CLEANUP_CALLBACK cleanup_callback,
    void *opaque);
int QxpD3dTexBltStampRecreationEntry(
    unsigned long entry_owner_identity,
    unsigned long teardown_owner_identity, int has_surface,
    unsigned long epoch, unsigned long *entry_epoch,
    int *recreation_consumed);
unsigned long QxpD3dTexBltRegistrationAction(
    unsigned long primary_handle, unsigned long runtime_handle,
    unsigned long incoming_handle, int same_global_identity,
    unsigned long entry_recreation_epoch,
    unsigned long owner_recreation_epoch, int recreation_consumed);
int QxpD3dTexBltRegistrationEntryMatches(
    int primary_request, unsigned long incoming_handle,
    unsigned long incoming_surface_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity);
int QxpD3dTexBltRegistrationBatchCanCommit(
    const unsigned long *actions, unsigned long action_count,
    unsigned long free_slots, unsigned long *required_slots);
int QxpD3dTexBltCoherenceCandidateMatches(
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long surface_handle, unsigned long entry_surface_identity,
    unsigned long entry_global_identity, unsigned long entry_handle,
    unsigned long entry_runtime_handle, unsigned long generation,
    unsigned long texture_name, unsigned long chain_texture_name);
int QxpD3dTexBltExactIdentityValid(
    unsigned long owner_identity, unsigned long surface_identity,
    unsigned long incarnation_low, unsigned long incarnation_high);
int QxpD3dTexBltDestroyEntryMatches(
    unsigned long dd_global_identity, unsigned long surface_handle,
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long candidate_dd_global_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity,
    unsigned long candidate_global_identity);
int QxpD3dTexBltDestroyEntryPreferred(
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long candidate_surface_identity,
    unsigned long candidate_global_identity);
int QxpD3dTexBltCapturedMemberMatches(
    unsigned long runtime_handle, unsigned long surface_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity,
    unsigned long captured_incarnation_low,
    unsigned long captured_incarnation_high,
    unsigned long candidate_incarnation_low,
    unsigned long candidate_incarnation_high);
int QxpD3dTexBltCpuWriteDirectMatch(
    unsigned long surface_identity, unsigned long candidate_surface_identity,
    unsigned long surface_handle, unsigned long candidate_primary_handle);
int QxpD3dTexBltCpuWriteChainMatch(
    unsigned long owner_identity, unsigned long texture_name,
    unsigned long chain_texture_name, unsigned long direct_owner_identity,
    unsigned long direct_texture_name,
    unsigned long direct_chain_texture_name);
int QxpD3dTexBltPublicationUnchanged(
    unsigned long captured_texture_name,
    unsigned long captured_chain_texture_name,
    unsigned long current_texture_name,
    unsigned long current_chain_texture_name);
int QxpD3dTexBltMemberReady(
    unsigned long generation, unsigned long uploaded_generation,
    unsigned long chain_texture_name, unsigned long expected_texture_name,
    int failed_without_baseline);
int QxpD3dTexBltApplyPreparedMemberState(
    unsigned long expected_texture_name, unsigned long generation,
    int upload_complete, unsigned long *chain_texture_name,
    unsigned long *uploaded_generation, int *baseline_valid,
    int *failed_without_baseline, int *host_authoritative);
int QxpD3dTexBltNextTextureName(
    unsigned long current, unsigned long *next);
int QxpD3dTexBltNextGeneration(
    unsigned long current, unsigned long *next);
int QxpD3dTexBltApplyCpuWriteState(
    unsigned long *generation, unsigned long *uploaded_generation,
    int *baseline_valid, int *failed_without_baseline,
    int *host_authoritative, int *cpu_authoritative,
    int generation_available);
/* DirectDraw's NT callbacks provide the semantic display-ownership state
 * that surface caps and lock rectangles cannot.  Only the exact exclusive
 * owner while DirectDraw, rather than GDI, owns the scanout may preserve a
 * read-only readback's unchanged host texture. */
int QxpD3dTexBltSetExclusiveDisplayState(
    unsigned long owner_identity, int enter,
    unsigned long *exclusive_owner_identity, int *gdi_selected);
int QxpD3dTexBltSetGdiDisplayState(
    unsigned long owner_identity, int to_gdi,
    unsigned long exclusive_owner_identity, int *gdi_selected);
int QxpD3dTexBltExclusiveOwnerTeardownCanClear(
    unsigned long owner_identity,
    unsigned long exclusive_owner_identity,
    unsigned long surviving_same_global_owners);
int QxpD3dTexBltPreserveHostForReadOnlyLock(
    int read_only, unsigned long owner_identity,
    unsigned long exclusive_owner_identity, int gdi_selected);
/* Exclusive readback needs provisional Unlock classification only for a
 * surface proven to belong to a presentation chain.  Offscreen read-only
 * locks preserve the coherent host copy without requiring a display present. */
int QxpD3dTexBltReadbackDecisionEligible(
    int preserve_host, int lock_preparing, int present_target);
/* A preserved readback leaves host authority unchanged.  Every other
 * completed readback transfers authority to its coherent CPU copy. */
int QxpD3dTexBltApplyReadbackState(
    int preserve_host, unsigned long generation,
    int *host_authoritative, int *cpu_authoritative,
    unsigned long *cpu_presented_generation);
/* A provisional exclusive readback remains attributable only while every
 * publication identity still names the downloaded host generation. */
int QxpD3dTexBltOwnedSurfaceMatches(
    unsigned long surface_identity, unsigned long entry_surface_identity,
    unsigned long surface_handle, unsigned long entry_handle,
    unsigned long owner_global_identity,
    unsigned long entry_owner_global_identity);
int QxpD3dTexBltReadbackDecisionValid(
    int pending, unsigned long generation,
    unsigned long pending_generation, unsigned long uploaded_generation,
    unsigned long host_write_epoch, unsigned long pending_host_write_epoch,
    unsigned long texture_name, unsigned long pending_texture_name,
    int host_authoritative);
/* Matched accelerated Unlock keeps host authority. An exact unmatched Unlock
 * finalizes the coherent readback as CPU authority and leaves its display
 * credit pending until the CPU-source present succeeds. */
int QxpD3dTexBltReadbackFallbackEligible(
    int finalize_readback, int decision_pending, int cpu_authoritative,
    unsigned long generation, unsigned long cpu_presented_generation);
int QxpD3dTexBltReadbackFallbackUnavailable(
    int fallback_required, int matched, int context_acquired);
int QxpD3dTexBltFinalizeReadbackDecision(
    int decision_valid, int matched, unsigned long generation,
    int *host_authoritative, int *cpu_authoritative,
    unsigned long *cpu_presented_generation, int *decision_pending);
int QxpD3dTexBltNextRecreationEpoch(
    unsigned long *clock, unsigned long *epoch);
int QxpD3dTexBltMipTopologyCompatible(
    unsigned long source_levels, unsigned long destination_levels,
    int source_mip_proxy, int destination_mip_proxy);
int QxpD3dTexBltRetiredRangeAppend(
    unsigned long *first_name, unsigned long *last_name,
    unsigned long texture_name);
int QxpD3dTexBltRetiredRangesTouch(
    unsigned long first_name, unsigned long last_name,
    unsigned long other_first_name, unsigned long other_last_name);
int QxpD3dTexBltRetiredRangeMerge(
    unsigned long *first_name, unsigned long *last_name,
    unsigned long other_first_name, unsigned long other_last_name);
int QxpD3dTexBltRetirementCanAppend(
    unsigned long occupied, unsigned long capacity,
    const unsigned long *candidate_names, unsigned long candidate_count,
    unsigned long *required_slots);
int QxpD3dTexBltBatchBytes(unsigned long record_count,
                           unsigned long available_bytes,
                           unsigned long *consumed_bytes);
unsigned long QxpD3dTexBltValidateCubeBatch(
    const QXP_D3D_TEXBLT_CUBE_MEMBER *members,
    unsigned long available_count, unsigned long *error_index);
unsigned long QxpD3dTexBltValidate(
    const QXP_D3D_TEXBLT_RECORD *record,
    const QXP_D3D_TEXBLT_SURFACE *source,
    const QXP_D3D_TEXBLT_SURFACE *destination,
    QXP_D3D_TEXBLT_PLAN *plan);

#endif
