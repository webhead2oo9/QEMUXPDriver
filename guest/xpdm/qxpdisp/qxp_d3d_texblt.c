#include "qxp_d3d_texblt.h"

static int qxp_d3d_texblt_format_supported(unsigned long format)
{
    return format == QXP_D3D_TEXBLT_FORMAT_R5G6B5 ||
           format == QXP_D3D_TEXBLT_FORMAT_A8R8G8B8 ||
           format == QXP_D3D_TEXBLT_FORMAT_X8R8G8B8;
}

unsigned long QxpD3dTexBltRunTransaction(
    unsigned long scratch_name,
    QXP_D3D_TEXBLT_TRANSACTION_CALLBACK upload_callback,
    QXP_D3D_TEXBLT_TRANSACTION_CALLBACK commit_callback,
    QXP_D3D_TEXBLT_TRANSACTION_CLEANUP_CALLBACK cleanup_callback,
    void *opaque)
{
    if (!scratch_name || !upload_callback || !commit_callback ||
        !cleanup_callback)
        return QXP_D3D_TEXBLT_TRANSACTION_INVALID;
    if (!upload_callback(opaque, scratch_name)) {
        cleanup_callback(opaque, scratch_name);
        return QXP_D3D_TEXBLT_TRANSACTION_UPLOAD_FAILED;
    }
    if (!commit_callback(opaque, scratch_name)) {
        cleanup_callback(opaque, scratch_name);
        return QXP_D3D_TEXBLT_TRANSACTION_COMMIT_FAILED;
    }
    return QXP_D3D_TEXBLT_TRANSACTION_SUCCESS;
}

int QxpD3dTexBltStampRecreationEntry(
    unsigned long entry_owner_identity,
    unsigned long teardown_owner_identity, int has_surface,
    unsigned long epoch, unsigned long *entry_epoch,
    int *recreation_consumed)
{
    if (!entry_owner_identity ||
        entry_owner_identity != teardown_owner_identity ||
        !has_surface || !epoch || !entry_epoch ||
        !recreation_consumed) return 0;
    *entry_epoch = epoch;
    *recreation_consumed = 0;
    return 1;
}

unsigned long QxpD3dTexBltRegistrationAction(
    unsigned long primary_handle, unsigned long runtime_handle,
    unsigned long incoming_handle, int same_global_identity,
    unsigned long entry_recreation_epoch,
    unsigned long owner_recreation_epoch, int recreation_consumed)
{
    if (!incoming_handle)
        return QXP_D3D_TEXBLT_REGISTRATION_REFUSE;
    if (primary_handle && primary_handle != incoming_handle)
        return QXP_D3D_TEXBLT_REGISTRATION_REFUSE;
    if (runtime_handle && runtime_handle != incoming_handle)
        return QXP_D3D_TEXBLT_REGISTRATION_REFUSE;
    if (same_global_identity)
        return QXP_D3D_TEXBLT_REGISTRATION_PRESERVE;
    /* CreateSurfaceEx is the runtime's authoritative current handle mapping.
     * Ordinary handle reuse (both epochs zero) must receive fresh identity,
     * never stale authority.  A teardown-stamped entry is stricter: only its
     * exact still-open owner epoch may replace it, so delayed Reset callbacks
     * cannot cross a later session boundary. */
    if (entry_recreation_epoch &&
        (recreation_consumed ||
         entry_recreation_epoch != owner_recreation_epoch))
        return QXP_D3D_TEXBLT_REGISTRATION_REFUSE;
    return QXP_D3D_TEXBLT_REGISTRATION_RECREATE;
}

int QxpD3dTexBltRegistrationEntryMatches(
    int primary_request, unsigned long incoming_handle,
    unsigned long incoming_surface_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity)
{
    if (incoming_handle) {
        return candidate_primary_handle == incoming_handle ||
               candidate_runtime_handle == incoming_handle;
    }
    return !primary_request && incoming_surface_identity &&
           candidate_surface_identity == incoming_surface_identity;
}

int QxpD3dTexBltCoherenceCandidateMatches(
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long surface_handle, unsigned long entry_surface_identity,
    unsigned long entry_global_identity, unsigned long entry_handle,
    unsigned long entry_runtime_handle, unsigned long generation,
    unsigned long texture_name, unsigned long chain_texture_name)
{
    /* Host authority and uploaded generation are post-transfer state.  This
     * predicate selects the exact identity that may first need pending render
     * target output transferred into its surface entry. */
    return surface_identity && global_identity && generation &&
           surface_identity == entry_surface_identity &&
           global_identity == entry_global_identity &&
           (texture_name || chain_texture_name) &&
           (!surface_handle || entry_handle == surface_handle ||
            entry_runtime_handle == surface_handle);
}

int QxpD3dTexBltExactIdentityValid(
    unsigned long owner_identity, unsigned long surface_identity,
    unsigned long incarnation_low, unsigned long incarnation_high)
{
    return owner_identity && surface_identity &&
           (incarnation_low || incarnation_high);
}

int QxpD3dTexBltDestroyEntryMatches(
    unsigned long dd_global_identity, unsigned long surface_handle,
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long candidate_dd_global_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity,
    unsigned long candidate_global_identity)
{
    if (!dd_global_identity ||
        dd_global_identity != candidate_dd_global_identity ||
        !surface_identity || surface_identity != candidate_surface_identity ||
        !global_identity || global_identity != candidate_global_identity)
        return 0;
    return !surface_handle || candidate_primary_handle == surface_handle ||
           candidate_runtime_handle == surface_handle;
}

int QxpD3dTexBltDestroyEntryPreferred(
    unsigned long surface_identity, unsigned long global_identity,
    unsigned long candidate_surface_identity,
    unsigned long candidate_global_identity)
{
    return surface_identity && surface_identity == candidate_surface_identity &&
           global_identity && global_identity == candidate_global_identity;
}

int QxpD3dTexBltCapturedMemberMatches(
    unsigned long runtime_handle, unsigned long surface_identity,
    unsigned long candidate_primary_handle,
    unsigned long candidate_runtime_handle,
    unsigned long candidate_surface_identity,
    unsigned long captured_incarnation_low,
    unsigned long captured_incarnation_high,
    unsigned long candidate_incarnation_low,
    unsigned long candidate_incarnation_high)
{
    return QxpD3dTexBltRegistrationEntryMatches(
               runtime_handle != 0, runtime_handle, surface_identity,
               candidate_primary_handle, candidate_runtime_handle,
               candidate_surface_identity) &&
           surface_identity == candidate_surface_identity &&
           captured_incarnation_low == candidate_incarnation_low &&
           captured_incarnation_high == candidate_incarnation_high &&
           (captured_incarnation_low || captured_incarnation_high);
}

int QxpD3dTexBltRegistrationBatchCanCommit(
    const unsigned long *actions, unsigned long action_count,
    unsigned long free_slots, unsigned long *required_slots)
{
    unsigned long index;
    unsigned long required;

    if (!required_slots || (action_count && !actions)) return 0;
    required = 0;
    for (index = 0; index < action_count; ++index) {
        if (actions[index] == QXP_D3D_TEXBLT_REGISTRATION_INSERT) {
            if (required == ~0UL) return 0;
            ++required;
        } else if (actions[index] !=
                       QXP_D3D_TEXBLT_REGISTRATION_PRESERVE &&
                   actions[index] !=
                       QXP_D3D_TEXBLT_REGISTRATION_RECREATE) {
            *required_slots = required;
            return 0;
        }
    }
    *required_slots = required;
    return required <= free_slots;
}

int QxpD3dTexBltCpuWriteDirectMatch(
    unsigned long surface_identity, unsigned long candidate_surface_identity,
    unsigned long surface_handle, unsigned long candidate_primary_handle)
{
    if (!surface_identity ||
        surface_identity != candidate_surface_identity) return 0;
    return !surface_handle || !candidate_primary_handle ||
           candidate_primary_handle == surface_handle;
}

int QxpD3dTexBltCpuWriteChainMatch(
    unsigned long owner_identity, unsigned long texture_name,
    unsigned long chain_texture_name, unsigned long direct_owner_identity,
    unsigned long direct_texture_name,
    unsigned long direct_chain_texture_name)
{
    unsigned long candidate_name;
    unsigned long direct_name;

    candidate_name = chain_texture_name ? chain_texture_name : texture_name;
    direct_name = direct_chain_texture_name ?
                  direct_chain_texture_name : direct_texture_name;
    return owner_identity && owner_identity == direct_owner_identity &&
           candidate_name && candidate_name == direct_name;
}

int QxpD3dTexBltPublicationUnchanged(
    unsigned long captured_texture_name,
    unsigned long captured_chain_texture_name,
    unsigned long current_texture_name,
    unsigned long current_chain_texture_name)
{
    return captured_texture_name == current_texture_name &&
           captured_chain_texture_name == current_chain_texture_name;
}

int QxpD3dTexBltMemberReady(
    unsigned long generation, unsigned long uploaded_generation,
    unsigned long chain_texture_name, unsigned long expected_texture_name,
    int failed_without_baseline)
{
    return generation && uploaded_generation == generation &&
           chain_texture_name == expected_texture_name &&
           expected_texture_name && !failed_without_baseline;
}

int QxpD3dTexBltApplyPreparedMemberState(
    unsigned long expected_texture_name, unsigned long generation,
    int upload_complete, unsigned long *chain_texture_name,
    unsigned long *uploaded_generation, int *baseline_valid,
    int *failed_without_baseline, int *host_authoritative)
{
    if (!upload_complete || !expected_texture_name || !generation ||
        !chain_texture_name || !uploaded_generation || !baseline_valid ||
        !failed_without_baseline || !host_authoritative) return 0;
    *chain_texture_name = expected_texture_name;
    *uploaded_generation = generation;
    *baseline_valid = 1;
    *failed_without_baseline = 0;
    *host_authoritative = 0;
    return 1;
}

int QxpD3dTexBltNextTextureName(
    unsigned long current, unsigned long *next)
{
    if (!next || current >= 0xfffffff9UL) return 0;
    *next = current + 1;
    return *next != 0;
}

int QxpD3dTexBltNextGeneration(
    unsigned long current, unsigned long *next)
{
    if (!next || !current || current == ~0UL) return 0;
    *next = current + 1UL;
    return *next != 0;
}

int QxpD3dTexBltApplyCpuWriteState(
    unsigned long *generation, unsigned long *uploaded_generation,
    int *baseline_valid, int *failed_without_baseline,
    int *host_authoritative, int *cpu_authoritative,
    int generation_available)
{
    unsigned long next_generation;

    if (generation) {
        if (!generation_available ||
            !QxpD3dTexBltNextGeneration(*generation, &next_generation)) {
            *generation = 0;
            generation_available = 0;
        } else {
            *generation = next_generation;
        }
    }
    if (uploaded_generation) *uploaded_generation = 0;
    if (baseline_valid) *baseline_valid = 0;
    if (failed_without_baseline) *failed_without_baseline = 0;
    if (host_authoritative) *host_authoritative = 0;
    if (cpu_authoritative) *cpu_authoritative = 1;
    return generation_available;
}

int QxpD3dTexBltSetExclusiveDisplayState(
    unsigned long owner_identity, int enter,
    unsigned long *exclusive_owner_identity, int *gdi_selected)
{
    if (!owner_identity || !exclusive_owner_identity || !gdi_selected)
        return 0;
    if (enter) {
        *exclusive_owner_identity = owner_identity;
        *gdi_selected = 0;
        return 1;
    }
    if (*exclusive_owner_identity != owner_identity) return 0;
    *exclusive_owner_identity = 0;
    *gdi_selected = 1;
    return 1;
}

int QxpD3dTexBltSetGdiDisplayState(
    unsigned long owner_identity, int to_gdi,
    unsigned long exclusive_owner_identity, int *gdi_selected)
{
    if (!owner_identity || !exclusive_owner_identity || !gdi_selected ||
        owner_identity != exclusive_owner_identity) return 0;
    *gdi_selected = to_gdi != 0;
    return 1;
}

int QxpD3dTexBltExclusiveOwnerTeardownCanClear(
    unsigned long owner_identity,
    unsigned long exclusive_owner_identity,
    unsigned long surviving_same_global_owners)
{
    return owner_identity && owner_identity == exclusive_owner_identity &&
           !surviving_same_global_owners;
}

int QxpD3dTexBltPreserveHostForReadOnlyLock(
    int read_only, unsigned long owner_identity,
    unsigned long exclusive_owner_identity, int gdi_selected)
{
    return read_only && owner_identity &&
           owner_identity == exclusive_owner_identity && !gdi_selected;
}

int QxpD3dTexBltReadbackDecisionEligible(
    int preserve_host, int lock_preparing, int present_target)
{
    return preserve_host && lock_preparing && present_target;
}

int QxpD3dTexBltApplyReadbackState(
    int preserve_host, unsigned long generation,
    int *host_authoritative, int *cpu_authoritative,
    unsigned long *cpu_presented_generation)
{
    if (!generation || !host_authoritative || !cpu_authoritative ||
        !cpu_presented_generation) return 0;
    if (preserve_host) return 1;
    *host_authoritative = 0;
    *cpu_authoritative = 1;
    *cpu_presented_generation = generation;
    return 1;
}

int QxpD3dTexBltOwnedSurfaceMatches(
    unsigned long surface_identity, unsigned long entry_surface_identity,
    unsigned long surface_handle, unsigned long entry_handle,
    unsigned long owner_global_identity,
    unsigned long entry_owner_global_identity)
{
    return surface_identity &&
           surface_identity == entry_surface_identity &&
           (!surface_handle || !entry_handle ||
            surface_handle == entry_handle) &&
           owner_global_identity &&
           owner_global_identity == entry_owner_global_identity;
}

int QxpD3dTexBltReadbackDecisionValid(
    int pending, unsigned long generation,
    unsigned long pending_generation, unsigned long uploaded_generation,
    unsigned long host_write_epoch, unsigned long pending_host_write_epoch,
    unsigned long texture_name, unsigned long pending_texture_name,
    int host_authoritative)
{
    return pending && generation && generation == pending_generation &&
           uploaded_generation == generation && host_write_epoch &&
           host_write_epoch == pending_host_write_epoch && texture_name &&
           texture_name == pending_texture_name && host_authoritative;
}

int QxpD3dTexBltReadbackFallbackEligible(
    int finalize_readback, int decision_pending, int cpu_authoritative,
    unsigned long generation, unsigned long cpu_presented_generation)
{
    return finalize_readback &&
           (decision_pending ||
            (cpu_authoritative && generation &&
             cpu_presented_generation != generation));
}

int QxpD3dTexBltReadbackFallbackUnavailable(
    int fallback_required, int matched, int context_acquired)
{
    return fallback_required && !matched && !context_acquired;
}

int QxpD3dTexBltFinalizeReadbackDecision(
    int decision_valid, int matched, unsigned long generation,
    int *host_authoritative, int *cpu_authoritative,
    unsigned long *cpu_presented_generation, int *decision_pending)
{
    if (!host_authoritative || !cpu_authoritative ||
        !cpu_presented_generation || !decision_pending) return 0;
    if (!*decision_pending) return 1;
    *decision_pending = 0;
    if (!decision_valid) return 0;
    if (matched) return 1;
    if (!QxpD3dTexBltApplyReadbackState(
            0, generation, host_authoritative, cpu_authoritative,
            cpu_presented_generation)) return 0;
    *cpu_presented_generation = 0;
    return 1;
}

int QxpD3dTexBltNextRecreationEpoch(
    unsigned long *clock, unsigned long *epoch)
{
    if (!clock || !epoch || *clock == ~0UL) return 0;
    ++*clock;
    if (!*clock) return 0;
    *epoch = *clock;
    return 1;
}

int QxpD3dTexBltMipTopologyCompatible(
    unsigned long source_levels, unsigned long destination_levels,
    int source_mip_proxy, int destination_mip_proxy)
{
    if (!source_levels || !destination_levels) return 0;
    if (source_levels == destination_levels) return 1;
    /* XP's managed-resource destination wrappers omit both attached children
     * and, in observed stock-D3D9 traffic, the mip caps themselves.  The
     * authoritative signal is the staging source's real attached mip chain;
     * TEXBLT copies its common levels into the named destination resource. */
    (void)destination_mip_proxy;
    return source_levels > 1 && destination_levels == 1 &&
           source_mip_proxy;
}

int QxpD3dTexBltRetiredRangeAppend(
    unsigned long *first_name, unsigned long *last_name,
    unsigned long texture_name)
{
    if (!first_name || !last_name || !texture_name) return 0;
    if (!*first_name && !*last_name) {
        *first_name = texture_name;
        *last_name = texture_name;
        return 1;
    }
    if (!*first_name || *last_name < *first_name) return 0;
    if (texture_name >= *first_name && texture_name <= *last_name)
        return 1;
    if (texture_name != 0xffffffffUL &&
        texture_name + 1 == *first_name) {
        *first_name = texture_name;
        return 1;
    }
    if (*last_name != 0xffffffffUL &&
        texture_name == *last_name + 1) {
        *last_name = texture_name;
        return 1;
    }
    return 0;
}

int QxpD3dTexBltRetiredRangesTouch(
    unsigned long first_name, unsigned long last_name,
    unsigned long other_first_name, unsigned long other_last_name)
{
    if (!first_name || !other_first_name || last_name < first_name ||
        other_last_name < other_first_name) return 0;
    if (first_name <= other_last_name &&
        other_first_name <= last_name) return 1;
    if (last_name != 0xffffffffUL &&
        last_name + 1 == other_first_name) return 1;
    return other_last_name != 0xffffffffUL &&
           other_last_name + 1 == first_name;
}

int QxpD3dTexBltRetiredRangeMerge(
    unsigned long *first_name, unsigned long *last_name,
    unsigned long other_first_name, unsigned long other_last_name)
{
    if (!first_name || !last_name ||
        !QxpD3dTexBltRetiredRangesTouch(
            *first_name, *last_name,
            other_first_name, other_last_name)) return 0;
    if (other_first_name < *first_name) *first_name = other_first_name;
    if (other_last_name > *last_name) *last_name = other_last_name;
    return 1;
}

int QxpD3dTexBltRetirementCanAppend(
    unsigned long occupied, unsigned long capacity,
    const unsigned long *candidate_names, unsigned long candidate_count,
    unsigned long *required_slots)
{
    unsigned long candidate;
    unsigned long index;
    unsigned long prior;
    unsigned long required;

    if (!required_slots) return 0;
    *required_slots = 0;
    if (occupied > capacity ||
        (candidate_count && !candidate_names)) return 0;
    required = 0;
    for (index = 0; index < candidate_count; ++index) {
        candidate = candidate_names[index];
        if (!candidate) continue;
        for (prior = 0; prior < index; ++prior) {
            if (candidate_names[prior] == candidate) break;
        }
        if (prior != index) continue;
        if (required == ~0UL) return 0;
        ++required;
    }
    *required_slots = required;
    return required <= capacity - occupied;
}

int QxpD3dTexBltBatchBytes(unsigned long record_count,
                           unsigned long available_bytes,
                           unsigned long *consumed_bytes)
{
    if (!consumed_bytes) return 0;
    *consumed_bytes = 0;
    if (record_count > available_bytes / sizeof(QXP_D3D_TEXBLT_RECORD))
        return 0;
    *consumed_bytes = record_count * sizeof(QXP_D3D_TEXBLT_RECORD);
    return 1;
}

unsigned long QxpD3dTexBltValidateCubeBatch(
    const QXP_D3D_TEXBLT_CUBE_MEMBER *members,
    unsigned long available_count, unsigned long *error_index)
{
    unsigned long face_mask;
    unsigned long face_bit;
    unsigned long index;

    if (!error_index) return QXP_D3D_TEXBLT_REJECT_INTERNAL;
    *error_index = 0;
    if (!members) return QXP_D3D_TEXBLT_REJECT_INTERNAL;
    if (available_count < QXP_D3D_TEXBLT_CUBE_FACE_COUNT)
        return QXP_D3D_TEXBLT_REJECT_CUBE_BATCH;
    if (!members[0].source_resource ||
        !members[0].destination_resource)
        return QXP_D3D_TEXBLT_REJECT_CUBE_BATCH;
    if (members[0].source_resource == members[0].destination_resource)
        return QXP_D3D_TEXBLT_REJECT_SELF_COPY;

    face_mask = 0;
    for (index = 0; index < QXP_D3D_TEXBLT_CUBE_FACE_COUNT; ++index) {
        if (members[index].source_resource !=
                members[0].source_resource ||
            members[index].destination_resource !=
                members[0].destination_resource ||
            members[index].source_face < 0 ||
            members[index].destination_face < 0 ||
            (unsigned long)members[index].source_face >=
                QXP_D3D_TEXBLT_CUBE_FACE_COUNT ||
            (unsigned long)members[index].destination_face >=
                QXP_D3D_TEXBLT_CUBE_FACE_COUNT ||
            members[index].source_face != members[index].destination_face) {
            *error_index = index;
            return QXP_D3D_TEXBLT_REJECT_CUBE_BATCH;
        }
        face_bit = 1UL << (unsigned long)members[index].source_face;
        if (face_mask & face_bit) {
            *error_index = index;
            return QXP_D3D_TEXBLT_REJECT_CUBE_BATCH;
        }
        face_mask |= face_bit;
    }
    if (face_mask != QXP_D3D_TEXBLT_CUBE_FACE_MASK)
        return QXP_D3D_TEXBLT_REJECT_CUBE_BATCH;
    return QXP_D3D_TEXBLT_SUCCESS;
}

unsigned long QxpD3dTexBltValidate(
    const QXP_D3D_TEXBLT_RECORD *record,
    const QXP_D3D_TEXBLT_SURFACE *source,
    const QXP_D3D_TEXBLT_SURFACE *destination,
    QXP_D3D_TEXBLT_PLAN *plan)
{
    unsigned long copy_width;
    unsigned long copy_height;
    unsigned long destination_x;
    unsigned long destination_y;

    if (!record || !source || !destination || !plan)
        return QXP_D3D_TEXBLT_REJECT_INTERNAL;
    if (record->flags) return QXP_D3D_TEXBLT_REJECT_FLAGS;
    if (!record->source_handle || !source->resolved)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_HANDLE;
    if (!record->destination_handle || !destination->resolved)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_HANDLE;
    if (record->source_handle == record->destination_handle)
        return QXP_D3D_TEXBLT_REJECT_SELF_COPY;
    if (!source->is_texture)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_NOT_TEXTURE;
    if (!destination->is_texture)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_NOT_TEXTURE;
    if (destination->is_system_memory)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_SYSTEM_MEMORY;
    if (destination->is_bound_destination)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUND;
    if (!source->width || !source->height ||
        source->width > QXP_D3D_TEXBLT_MAX_DIMENSION ||
        source->height > QXP_D3D_TEXBLT_MAX_DIMENSION)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_GEOMETRY;
    if (!destination->width || !destination->height ||
        destination->width > QXP_D3D_TEXBLT_MAX_DIMENSION ||
        destination->height > QXP_D3D_TEXBLT_MAX_DIMENSION)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_GEOMETRY;
    if (!source->format_valid)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_FORMAT;
    if (!destination->format_valid)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_FORMAT;
    if (source->format != destination->format)
        return QXP_D3D_TEXBLT_REJECT_FORMAT_MISMATCH;
    if ((source->format == QXP_D3D_TEXBLT_FORMAT_P8 ||
         source->format == QXP_D3D_TEXBLT_FORMAT_A8P8) &&
        (source->palette_handle != destination->palette_handle ||
         source->palette_flags != destination->palette_flags))
        return QXP_D3D_TEXBLT_REJECT_PALETTE_MISMATCH;
    if (record->source_right <= record->source_left ||
        record->source_bottom <= record->source_top)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_EMPTY;
    if (record->source_left < 0 || record->source_top < 0 ||
        (unsigned long)record->source_right > source->width ||
        (unsigned long)record->source_bottom > source->height)
        return QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_BOUNDS;
    if (record->destination_x < 0 || record->destination_y < 0)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_POINT;

    copy_width = (unsigned long)(record->source_right -
                                 record->source_left);
    copy_height = (unsigned long)(record->source_bottom -
                                  record->source_top);
    destination_x = (unsigned long)record->destination_x;
    destination_y = (unsigned long)record->destination_y;
    if (destination_x > destination->width ||
        destination_y > destination->height ||
        copy_width > destination->width - destination_x ||
        copy_height > destination->height - destination_y)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUNDS;

    plan->source_left = record->source_left;
    plan->source_top = record->source_top;
    plan->source_right = record->source_right;
    plan->source_bottom = record->source_bottom;
    plan->destination_left = record->destination_x;
    plan->destination_top = record->destination_y;
    plan->destination_right = record->destination_x + (signed long)copy_width;
    plan->destination_bottom = record->destination_y +
                               (signed long)copy_height;

    /* An upload replaces the whole destination level from a system-memory
     * staging surface, so it is only that when the copy covers both surfaces
     * completely.  Anything partial would need a sub-rectangle upload, which
     * the level upload path does not express, so it stays a blit and keeps the
     * blit's format restriction. */
    if (source->is_system_memory &&
        record->source_left == 0 && record->source_top == 0 &&
        copy_width == source->width && copy_height == source->height &&
        record->destination_x == 0 && record->destination_y == 0 &&
        copy_width == destination->width &&
        copy_height == destination->height) {
        if (source->is_cube != destination->is_cube) {
            return source->is_cube ?
                QXP_D3D_TEXBLT_REJECT_SOURCE_CUBE :
                QXP_D3D_TEXBLT_REJECT_DESTINATION_CUBE;
        }
        plan->route = QXP_D3D_TEXBLT_ROUTE_UPLOAD;
        return QXP_D3D_TEXBLT_SUCCESS;
    }
    plan->route = QXP_D3D_TEXBLT_ROUTE_BLIT;
    if (source->is_cube) return QXP_D3D_TEXBLT_REJECT_SOURCE_CUBE;
    if (destination->is_cube)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_CUBE;
    if (source->is_mipmapped) return QXP_D3D_TEXBLT_REJECT_SOURCE_MIP;
    if (destination->is_mipmapped)
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_MIP;
    if (!qxp_d3d_texblt_format_supported(source->format))
        return QXP_D3D_TEXBLT_REJECT_SOURCE_FORMAT;
    if (!qxp_d3d_texblt_format_supported(destination->format))
        return QXP_D3D_TEXBLT_REJECT_DESTINATION_FORMAT;
    return QXP_D3D_TEXBLT_SUCCESS;
}
