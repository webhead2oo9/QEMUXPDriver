#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_d3d_texblt.h"

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

typedef struct qxp_transaction_test_state {
    int upload_result;
    int commit_result;
    unsigned long current_name;
    unsigned long cleaned_name;
    unsigned long scratch_write_count;
    unsigned long upload_count;
    unsigned long commit_count;
    unsigned long cleanup_count;
} QXP_TRANSACTION_TEST_STATE;

static int transaction_test_upload(void *opaque, unsigned long scratch_name)
{
    QXP_TRANSACTION_TEST_STATE *state;

    state = (QXP_TRANSACTION_TEST_STATE *)opaque;
    ++state->upload_count;
    ++state->scratch_write_count;
    (void)scratch_name;
    return state->upload_result;
}

static int transaction_test_commit(void *opaque, unsigned long scratch_name)
{
    QXP_TRANSACTION_TEST_STATE *state;

    state = (QXP_TRANSACTION_TEST_STATE *)opaque;
    ++state->commit_count;
    if (state->commit_result) state->current_name = scratch_name;
    return state->commit_result;
}

static void transaction_test_cleanup(void *opaque,
                                     unsigned long scratch_name)
{
    QXP_TRANSACTION_TEST_STATE *state;

    state = (QXP_TRANSACTION_TEST_STATE *)opaque;
    ++state->cleanup_count;
    state->cleaned_name = scratch_name;
}

static void valid_fixture(QXP_D3D_TEXBLT_RECORD *record,
                          QXP_D3D_TEXBLT_SURFACE *source,
                          QXP_D3D_TEXBLT_SURFACE *destination)
{
    memset(record, 0, sizeof(*record));
    memset(source, 0, sizeof(*source));
    memset(destination, 0, sizeof(*destination));
    record->destination_handle = 10;
    record->source_handle = 20;
    record->destination_x = 11;
    record->destination_y = 13;
    record->source_left = 17;
    record->source_top = 19;
    record->source_right = 117;
    record->source_bottom = 219;
    source->resolved = 1;
    source->is_texture = 1;
    source->width = 256;
    source->height = 256;
    source->format = QXP_D3D_TEXBLT_FORMAT_A8R8G8B8;
    source->format_valid = 1;
    destination->resolved = 1;
    destination->is_texture = 1;
    destination->width = 256;
    destination->height = 256;
    destination->format = QXP_D3D_TEXBLT_FORMAT_A8R8G8B8;
    destination->format_valid = 1;
}

static int expect_reason(unsigned long expected,
                         QXP_D3D_TEXBLT_RECORD *record,
                         QXP_D3D_TEXBLT_SURFACE *source,
                         QXP_D3D_TEXBLT_SURFACE *destination,
                         const char *message)
{
    QXP_D3D_TEXBLT_PLAN plan;
    unsigned long actual;

    memset(&plan, 0, sizeof(plan));
    actual = QxpD3dTexBltValidate(record, source, destination, &plan);
    return require(actual == expected, message);
}

int main(void)
{
    QXP_D3D_TEXBLT_RECORD record;
    QXP_D3D_TEXBLT_SURFACE source;
    QXP_D3D_TEXBLT_SURFACE destination;
    QXP_D3D_TEXBLT_PLAN plan;
    QXP_D3D_TEXBLT_CUBE_MEMBER cube_members[7];
    QXP_TRANSACTION_TEST_STATE transaction;
    int baseline_valid;
    int cpu_authoritative;
    int decision_pending;
    int failed_without_baseline;
    int gdi_selected;
    int host_authoritative;
    int preserve_host;
    int recreation_consumed;
    unsigned long generation;
    unsigned long registration_actions[2];
    unsigned long retirement_names[4];
    unsigned long action;
    unsigned long chain_texture_name;
    unsigned long clock;
    unsigned long consumed;
    unsigned long cpu_presented_generation;
    unsigned long epoch;
    unsigned long error_index;
    unsigned long exclusive_owner_identity;
    unsigned long index;
    unsigned long next_generation;
    unsigned long required_slots;
    unsigned long retired_first;
    unsigned long retired_last;
    unsigned long uploaded_generation;
    unsigned long record_bytes = sizeof(QXP_D3D_TEXBLT_RECORD);

    if (!require(record_bytes == 36UL, "TEXBLT record wire size") ||
        !require(QxpD3dTexBltBatchBytes(2, record_bytes * 2, &consumed) &&
                 consumed == record_bytes * 2,
                 "two-record batch parsing") ||
        !require(!QxpD3dTexBltBatchBytes(
                     2, record_bytes * 2 - 1, &consumed) && consumed == 0,
                 "truncated second record accepted") ||
        !require(QxpD3dTexBltBatchBytes(0, 0, &consumed) && consumed == 0,
                 "zero-record batch parsing")) return 1;

    memset(cube_members, 0, sizeof(cube_members));
    for (index = 0; index < 7; ++index) {
        cube_members[index].source_resource = 100;
        cube_members[index].destination_resource = 200;
        cube_members[index].source_face =
            (signed long)(5 - (index % 6));
        cube_members[index].destination_face =
            cube_members[index].source_face;
    }
    error_index = 99;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_SUCCESS && error_index == 0,
                 "complete unordered cube batch rejected") ||
        !require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 7, &error_index) ==
                     QXP_D3D_TEXBLT_SUCCESS,
                 "complete cube prefix with a following record rejected") ||
        !require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 5, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_CUBE_BATCH &&
                     error_index == 0,
                 "short cube batch accepted")) return 1;

    cube_members[4].source_face = cube_members[0].source_face;
    cube_members[4].destination_face = cube_members[0].destination_face;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_CUBE_BATCH &&
                     error_index == 4,
                 "duplicate cube face accepted at the wrong offset"))
        return 1;
    cube_members[4].source_face = 1;
    cube_members[4].destination_face = 1;
    cube_members[2].destination_face = 0;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_CUBE_BATCH &&
                     error_index == 2,
                 "mismatched source and destination faces accepted"))
        return 1;
    cube_members[2].destination_face = cube_members[2].source_face;
    cube_members[3].source_resource = 101;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_CUBE_BATCH &&
                     error_index == 3,
                 "mixed source resources accepted")) return 1;
    cube_members[3].source_resource = 100;
    cube_members[4].destination_resource = 201;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_CUBE_BATCH &&
                     error_index == 4,
                 "mixed destination resources accepted")) return 1;
    cube_members[4].destination_resource = 200;
    for (index = 0; index < 6; ++index)
        cube_members[index].destination_resource = 100;
    if (!require(QxpD3dTexBltValidateCubeBatch(
                     cube_members, 6, &error_index) ==
                     QXP_D3D_TEXBLT_REJECT_SELF_COPY &&
                     error_index == 0,
                 "same-resource cube copy accepted")) return 1;

    if (!require(QxpD3dTexBltRegistrationEntryMatches(
                     1, 10, 100, 10, 10, 200),
                 "exact registration handle did not match") ||
        !require(!QxpD3dTexBltRegistrationEntryMatches(
                     1, 10, 100, 11, 11, 100),
                 "historical wrapper pointer overrode scalar handles") ||
        !require(QxpD3dTexBltRegistrationEntryMatches(
                     0, 0, 100, 0, 0, 100),
                 "handleless live reference did not match") ||
        !require(!QxpD3dTexBltRegistrationEntryMatches(
                     1, 0, 100, 0, 0, 100),
                 "handleless primary registration matched") ||
        !require(QxpD3dTexBltCapturedMemberMatches(
                     4, 100, 4, 4, 100, 44, 1, 44, 1),
                 "exact captured resource member did not match") ||
        !require(!QxpD3dTexBltCapturedMemberMatches(
                     4, 100, 14, 14, 100, 44, 1, 44, 1),
                 "historical same-pointer handle alias matched capture") ||
        !require(!QxpD3dTexBltCapturedMemberMatches(
                     4, 100, 4, 4, 100, 44, 1, 45, 1),
                 "reincarnated resource member matched stale capture") ||
        !require(!QxpD3dTexBltCapturedMemberMatches(
                     4, 100, 4, 4, 100, 44, 1, 44, 2),
                 "high-half incarnation mismatch was ignored") ||
        !require(QxpD3dTexBltCapturedMemberMatches(
                     0, 100, 0, 0, 100, 44, 1, 44, 1),
                 "handleless exact resource member did not match") ||
        !require(QxpD3dTexBltCoherenceCandidateMatches(
                     100, 200, 4, 100, 200, 4, 4,
                     7, 41, 41),
                 "exact coherence candidate was rejected") ||
        !require(!QxpD3dTexBltCoherenceCandidateMatches(
                     100, 200, 4, 100, 201, 4, 4,
                     7, 41, 41),
                 "stale-global coherence candidate was accepted") ||
        !require(!QxpD3dTexBltCoherenceCandidateMatches(
                     100, 200, 4, 100, 200, 5, 5,
                     7, 41, 41),
                 "wrong-handle coherence candidate was accepted") ||
        !require(QxpD3dTexBltCoherenceCandidateMatches(
                     100, 200, 0, 100, 200, 0, 0,
                     7, 41, 41),
                 "handleless coherence candidate was rejected") ||
        !require(QxpD3dTexBltExactIdentityValid(2, 100, 0, 1) &&
                 QxpD3dTexBltExactIdentityValid(2, 100, 44, 0),
                 "handleless exact readback identity was rejected") ||
        !require(!QxpD3dTexBltExactIdentityValid(0, 100, 44, 0) &&
                 !QxpD3dTexBltExactIdentityValid(2, 0, 44, 0) &&
                 !QxpD3dTexBltExactIdentityValid(2, 100, 0, 0),
                 "incomplete exact readback identity was accepted") ||
        !require(QxpD3dTexBltDestroyEntryMatches(
                     2, 4, 100, 200, 2, 4, 4, 100, 200) &&
                 !QxpD3dTexBltDestroyEntryMatches(
                     2, 4, 100, 200, 1, 4, 4, 100, 200),
                 "destroy selection crossed owner-scoped handles") ||
        !require(QxpD3dTexBltDestroyEntryMatches(
                     2, 0, 100, 200, 2, 0, 0, 100, 200) &&
                 !QxpD3dTexBltDestroyEntryMatches(
                     2, 0, 100, 200, 1, 0, 0, 100, 200),
                 "handleless destroy selection crossed owners") ||
        !require(!QxpD3dTexBltDestroyEntryMatches(
                     2, 4, 100, 200, 2, 4, 4, 101, 200),
                 "delayed old-wrapper destroy selected replacement") ||
        !require(!QxpD3dTexBltDestroyEntryMatches(
                     2, 4, 100, 200, 2, 4, 4, 100, 201),
                 "delayed old-global destroy selected replacement") ||
        !require(QxpD3dTexBltDestroyEntryPreferred(
                     100, 200, 100, 200) &&
                 !QxpD3dTexBltDestroyEntryPreferred(
                     100, 200, 100, 201),
                 "destroy preference ignored backing identity"))
        return 1;

    epoch = 0;
    recreation_consumed = 1;
    if (!require(QxpD3dTexBltStampRecreationEntry(
                     2, 2, 1, 7, &epoch, &recreation_consumed) &&
                 epoch == 7 && !recreation_consumed,
                 "owner recreation stamp did not reset consumption"))
        return 1;
    epoch = 9;
    recreation_consumed = 1;
    if (!require(!QxpD3dTexBltStampRecreationEntry(
                     2, 3, 1, 8, &epoch, &recreation_consumed) &&
                 epoch == 9 && recreation_consumed,
                 "unrelated owner teardown changed recreation latch") ||
        !require(!QxpD3dTexBltStampRecreationEntry(
                     2, 2, 0, 8, &epoch, &recreation_consumed) &&
                 epoch == 9 && recreation_consumed,
                 "dead entry received a recreation stamp")) return 1;

    action = QxpD3dTexBltRegistrationAction(
        10, 10, 10, 1, 0, 0, 0);
    if (!require(action == QXP_D3D_TEXBLT_REGISTRATION_PRESERVE,
                 "same-global primary wrapper was not preserved") ||
        !require(QxpD3dTexBltRegistrationAction(
                     0, 10, 10, 1, 0, 0, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_PRESERVE,
                 "same-global reference wrapper was not preserved") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 10, 0, 7, 7, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_RECREATE,
                 "exact-epoch primary recreation refused") ||
        !require(QxpD3dTexBltRegistrationAction(
                     0, 10, 10, 0, 7, 7, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_RECREATE,
                 "exact-epoch reference recreation refused") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 10, 0, 7, 7, 1) ==
                     QXP_D3D_TEXBLT_REGISTRATION_REFUSE,
                 "consumed Reset recreation accepted a delayed wrapper") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 10, 0, 0, 0, 1) ==
                     QXP_D3D_TEXBLT_REGISTRATION_RECREATE,
                 "post-window authoritative handle reuse was refused") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 10, 0, 0, 0, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_RECREATE,
                 "ordinary primary handle reuse did not get fresh identity") ||
        !require(QxpD3dTexBltRegistrationAction(
                     0, 10, 10, 0, 0, 0, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_RECREATE,
                 "ordinary reference handle reuse did not get fresh identity") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 10, 0, 7, 8, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_REFUSE,
                 "stale recreation epoch accepted") ||
        !require(QxpD3dTexBltRegistrationAction(
                     10, 10, 11, 1, 7, 7, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_REFUSE,
                 "conflicting incoming handle accepted") ||
        !require(QxpD3dTexBltRegistrationAction(
                     0, 0, 0, 1, 7, 7, 0) ==
                     QXP_D3D_TEXBLT_REGISTRATION_REFUSE,
                 "zero incoming handle accepted")) return 1;

    if (!require(QxpD3dTexBltPublicationUnchanged(41, 39, 41, 39),
                 "unchanged publication state was rejected") ||
        !require(!QxpD3dTexBltPublicationUnchanged(41, 39, 42, 42),
                 "concurrent texture-name replacement was ignored") ||
        !require(!QxpD3dTexBltPublicationUnchanged(41, 39, 41, 42),
                 "concurrent chain-name replacement was ignored")) return 1;

    if (!require(QxpD3dTexBltMemberReady(7, 7, 41, 41, 0),
                 "complete texture member was not ready") ||
        !require(!QxpD3dTexBltMemberReady(7, 6, 41, 41, 0),
                 "stale mip child was reported ready") ||
        !require(!QxpD3dTexBltMemberReady(7, 7, 42, 41, 0),
                 "wrong-chain mip child was reported ready") ||
        !require(!QxpD3dTexBltMemberReady(7, 7, 41, 41, 1),
                 "failed-baseline mip child was reported ready")) return 1;

    generation = 7;
    uploaded_generation = 7;
    chain_texture_name = 42;
    baseline_valid = 1;
    failed_without_baseline = 0;
    host_authoritative = 1;
    if (!require(!QxpD3dTexBltApplyPreparedMemberState(
                     41, generation, 0, &chain_texture_name,
                     &uploaded_generation, &baseline_valid,
                     &failed_without_baseline, &host_authoritative) &&
                 chain_texture_name == 42 && uploaded_generation == 7 &&
                 baseline_valid && !failed_without_baseline &&
                 host_authoritative &&
                 !QxpD3dTexBltMemberReady(
                     generation, uploaded_generation,
                     chain_texture_name, 41, failed_without_baseline),
                 "failed upload repaired wrong-chain readiness") ||
        !require(QxpD3dTexBltApplyPreparedMemberState(
                     41, generation, 1, &chain_texture_name,
                     &uploaded_generation, &baseline_valid,
                     &failed_without_baseline, &host_authoritative) &&
                 QxpD3dTexBltMemberReady(
                     generation, uploaded_generation,
                     chain_texture_name, 41, failed_without_baseline) &&
                 baseline_valid && !host_authoritative,
                 "complete upload did not publish member readiness"))
        return 1;

    next_generation = 99;
    if (!require(QxpD3dTexBltNextTextureName(0, &next_generation) &&
                 next_generation == 1,
                 "first dynamic texture name was not one") ||
        !require(QxpD3dTexBltNextTextureName(
                     0xfffffff8UL, &next_generation) &&
                 next_generation == 0xfffffff9UL,
                 "last dynamic texture name was skipped")) return 1;
    next_generation = 77;
    if (!require(!QxpD3dTexBltNextTextureName(
                     0xfffffff9UL, &next_generation) &&
                 next_generation == 77,
                 "texture allocator entered reserved volume range") ||
        !require(!QxpD3dTexBltNextTextureName(
                     0xfffffffbUL, &next_generation) &&
                 next_generation == 77,
                 "texture-name exhaustion wrapped into reserved names") ||
        !require(!QxpD3dTexBltNextTextureName(
                     0xffffffffUL, &next_generation) &&
                 next_generation == 77,
                 "exhausted texture name wrapped to a live name")) return 1;

    next_generation = 99;
    if (!require(QxpD3dTexBltNextGeneration(
                     0xfffffffeUL, &next_generation) &&
                 next_generation == 0xffffffffUL,
                 "penultimate surface generation did not advance") ||
        !require(!QxpD3dTexBltNextGeneration(
                     0xffffffffUL, &next_generation) &&
                 next_generation == 0xffffffffUL,
                 "exhausted surface generation wrapped") ||
        !require(!QxpD3dTexBltNextGeneration(0, &next_generation) &&
                 next_generation == 0xffffffffUL,
                 "zero surface generation was revived")) return 1;

    generation = 0xfffffffeUL;
    uploaded_generation = 17;
    baseline_valid = 1;
    failed_without_baseline = 1;
    host_authoritative = 1;
    cpu_authoritative = 0;
    if (!require(QxpD3dTexBltApplyCpuWriteState(
                     &generation, &uploaded_generation,
                     &baseline_valid, &failed_without_baseline,
                     &host_authoritative, &cpu_authoritative, 1) &&
                 generation == 0xffffffffUL &&
                 uploaded_generation == 0 && !baseline_valid &&
                 !failed_without_baseline && !host_authoritative &&
                 cpu_authoritative,
                 "CPU BLT transition did not advance and recover state"))
        return 1;
    uploaded_generation = 23;
    baseline_valid = 1;
    failed_without_baseline = 1;
    host_authoritative = 1;
    cpu_authoritative = 0;
    if (!require(!QxpD3dTexBltApplyCpuWriteState(
                     &generation, &uploaded_generation,
                     &baseline_valid, &failed_without_baseline,
                     &host_authoritative, &cpu_authoritative, 0) &&
                 generation == 0 && uploaded_generation == 0 &&
                 !baseline_valid && !failed_without_baseline &&
                 !host_authoritative && cpu_authoritative,
                 "exhausted CPU BLT transition retained stale authority"))
        return 1;

    generation = 41;
    exclusive_owner_identity = 0;
    gdi_selected = 1;
    if (!require(QxpD3dTexBltSetExclusiveDisplayState(
                     101, 1, &exclusive_owner_identity, &gdi_selected) &&
                 exclusive_owner_identity == 101 && !gdi_selected,
                 "exclusive display entry did not publish exact owner") ||
        !require(QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 101, exclusive_owner_identity, gdi_selected),
                 "exact exclusive DirectDraw owner lost host authority") ||
        !require(!QxpD3dTexBltPreserveHostForReadOnlyLock(
                     0, 101, exclusive_owner_identity, gdi_selected) &&
                 !QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 202, exclusive_owner_identity, gdi_selected),
                 "writable or foreign-owner map retained host authority") ||
        !require(!QxpD3dTexBltSetGdiDisplayState(
                     202, 1, exclusive_owner_identity, &gdi_selected) &&
                 !gdi_selected && exclusive_owner_identity == 101,
                 "foreign GDI notification changed exact display owner") ||
        !require(QxpD3dTexBltSetGdiDisplayState(
                     101, 1, exclusive_owner_identity, &gdi_selected) &&
                 gdi_selected &&
                 !QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 101, exclusive_owner_identity, gdi_selected),
                 "GDI-selected display retained host authority") ||
        !require(QxpD3dTexBltSetGdiDisplayState(
                     101, 0, exclusive_owner_identity, &gdi_selected) &&
                 !gdi_selected,
                 "return from GDI did not restore direct-display state") ||
        !require(!QxpD3dTexBltSetExclusiveDisplayState(
                     202, 0, &exclusive_owner_identity, &gdi_selected) &&
                 exclusive_owner_identity == 101 && !gdi_selected,
                 "foreign exclusive exit cleared the live owner") ||
        !require(QxpD3dTexBltSetExclusiveDisplayState(
                     202, 1, &exclusive_owner_identity, &gdi_selected) &&
                 exclusive_owner_identity == 202 && !gdi_selected &&
                 !QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 101, exclusive_owner_identity, gdi_selected) &&
                 QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 202, exclusive_owner_identity, gdi_selected),
                 "exclusive owner handoff retained prior-owner authority") ||
        !require(QxpD3dTexBltSetExclusiveDisplayState(
                     202, 0, &exclusive_owner_identity, &gdi_selected) &&
                 !exclusive_owner_identity && gdi_selected,
                 "exclusive display exit retained direct-display authority"))
        return 1;
    if (!require(QxpD3dTexBltSetExclusiveDisplayState(
                     303, 1, &exclusive_owner_identity, &gdi_selected) &&
                 !QxpD3dTexBltExclusiveOwnerTeardownCanClear(
                     303, exclusive_owner_identity, 1) &&
                 QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 303, exclusive_owner_identity, gdi_selected),
                 "shared-global survivor lost exclusive authority") ||
        !require(QxpD3dTexBltExclusiveOwnerTeardownCanClear(
                     303, exclusive_owner_identity, 0) &&
                 QxpD3dTexBltSetExclusiveDisplayState(
                     303, 0, &exclusive_owner_identity, &gdi_selected) &&
                 !QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 303, exclusive_owner_identity, gdi_selected),
                 "terminal owner teardown retained recycled identity") ||
        !require(QxpD3dTexBltSetExclusiveDisplayState(
                     303, 1, &exclusive_owner_identity, &gdi_selected) &&
                 QxpD3dTexBltPreserveHostForReadOnlyLock(
                     1, 303, exclusive_owner_identity, gdi_selected) &&
                 QxpD3dTexBltSetExclusiveDisplayState(
                     303, 0, &exclusive_owner_identity, &gdi_selected),
                 "recycled owner could not establish a fresh lifetime"))
        return 1;

    host_authoritative = 1;
    cpu_authoritative = 0;
    cpu_presented_generation = 0;
    preserve_host = QxpD3dTexBltPreserveHostForReadOnlyLock(
        1, 101, 101, 0);
    if (!require(preserve_host &&
                 QxpD3dTexBltApplyReadbackState(
                     preserve_host, generation, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation) &&
                 host_authoritative && !cpu_authoritative &&
                 cpu_presented_generation == 0,
                 "direct-display readback discarded host authority"))
        return 1;
    preserve_host = QxpD3dTexBltPreserveHostForReadOnlyLock(
        1, 101, 0, 1);
    host_authoritative = 1;
    cpu_authoritative = 0;
    cpu_presented_generation = 0;
    if (!require(!preserve_host &&
                 QxpD3dTexBltApplyReadbackState(
                     preserve_host, generation, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation) &&
                 !host_authoritative && cpu_authoritative &&
                 cpu_presented_generation == generation,
                 "window-composited readback did not select CPU authority") ||
        !require(!QxpD3dTexBltApplyReadbackState(
                     1, 0, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation),
                 "zero-generation readback state was accepted")) return 1;

    if (!require(QxpD3dTexBltReadbackDecisionEligible(1, 1, 1),
                 "present-target readback decision was rejected") ||
        !require(!QxpD3dTexBltReadbackDecisionEligible(1, 1, 0),
                 "offscreen read-only lock required display fallback") ||
        !require(!QxpD3dTexBltReadbackDecisionEligible(1, 0, 1) &&
                 !QxpD3dTexBltReadbackDecisionEligible(0, 1, 1),
                 "nonpreparing or nonpreserved readback became provisional"))
        return 1;

    if (!require(QxpD3dTexBltOwnedSurfaceMatches(
                     11, 11, 7, 7, 101, 101) &&
                 QxpD3dTexBltOwnedSurfaceMatches(
                     11, 11, 7, 0, 101, 101),
                 "exact owner-scoped surface identity was rejected") ||
        !require(!QxpD3dTexBltOwnedSurfaceMatches(
                     11, 11, 7, 7, 101, 202) &&
                 !QxpD3dTexBltOwnedSurfaceMatches(
                     11, 12, 7, 7, 101, 101) &&
                 !QxpD3dTexBltOwnedSurfaceMatches(
                     11, 11, 7, 8, 101, 101),
                 "foreign-owner or conflicting surface identity matched") ||
        !require(QxpD3dTexBltReadbackDecisionValid(
                     1, 41, 41, 41, 7, 7, 9, 9, 1),
                 "exact provisional readback identity was rejected") ||
        !require(!QxpD3dTexBltReadbackDecisionValid(
                     1, 42, 41, 42, 7, 7, 9, 9, 1) &&
                 !QxpD3dTexBltReadbackDecisionValid(
                     1, 41, 41, 41, 8, 7, 9, 9, 1) &&
                 !QxpD3dTexBltReadbackDecisionValid(
                     1, 41, 41, 41, 7, 7, 10, 9, 1) &&
                 !QxpD3dTexBltReadbackDecisionValid(
                     1, 41, 41, 40, 7, 7, 9, 9, 1) &&
                 !QxpD3dTexBltReadbackDecisionValid(
                     1, 41, 41, 41, 7, 7, 9, 9, 0),
                 "stale provisional readback identity was retained"))
        return 1;
    if (!require(QxpD3dTexBltReadbackFallbackEligible(
                     1, 1, 0, 41, 0) &&
                 QxpD3dTexBltReadbackFallbackEligible(
                     1, 0, 1, 41, 0),
                 "initial and failed CPU fallback were not retryable") ||
        !require(!QxpD3dTexBltReadbackFallbackEligible(
                     1, 0, 1, 41, 41) &&
                 !QxpD3dTexBltReadbackFallbackEligible(
                     0, 1, 0, 41, 0),
                 "credited or non-Unlock fallback remained eligible") ||
        !require(QxpD3dTexBltReadbackFallbackUnavailable(
                     QxpD3dTexBltReadbackFallbackEligible(
                         1, 1, 0, 41, 0), 0, 0) &&
                 QxpD3dTexBltReadbackFallbackUnavailable(
                     QxpD3dTexBltReadbackFallbackEligible(
                         1, 0, 1, 41, 0), 0, 0),
                 "initial or consumed fallback hid context unavailability") ||
        !require(!QxpD3dTexBltReadbackFallbackUnavailable(1, 1, 0) &&
                 !QxpD3dTexBltReadbackFallbackUnavailable(1, 0, 1) &&
                 !QxpD3dTexBltReadbackFallbackUnavailable(0, 0, 0),
                 "matched, acquired, or ineligible fallback failed closed"))
        return 1;
    decision_pending = 1;
    host_authoritative = 1;
    cpu_authoritative = 0;
    cpu_presented_generation = 0;
    if (!require(QxpD3dTexBltFinalizeReadbackDecision(
                     1, 1, 41, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation,
                     &decision_pending) &&
                 !decision_pending && host_authoritative &&
                 !cpu_authoritative && !cpu_presented_generation,
                 "matched Unlock did not preserve host authority")) return 1;
    decision_pending = 1;
    host_authoritative = 1;
    cpu_authoritative = 0;
    cpu_presented_generation = 41;
    if (!require(QxpD3dTexBltFinalizeReadbackDecision(
                     1, 0, 41, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation,
                     &decision_pending) &&
                 !decision_pending && !host_authoritative &&
                 cpu_authoritative && !cpu_presented_generation,
                 "unmatched Unlock did not select pending CPU authority"))
        return 1;
    decision_pending = 1;
    host_authoritative = 1;
    cpu_authoritative = 0;
    cpu_presented_generation = 0;
    if (!require(!QxpD3dTexBltFinalizeReadbackDecision(
                     0, 0, 41, &host_authoritative,
                     &cpu_authoritative, &cpu_presented_generation,
                     &decision_pending) &&
                 !decision_pending && host_authoritative &&
                 !cpu_authoritative,
                 "stale readback decision changed authority")) return 1;

    registration_actions[0] = QXP_D3D_TEXBLT_REGISTRATION_INSERT;
    registration_actions[1] = QXP_D3D_TEXBLT_REGISTRATION_INSERT;
    required_slots = 99;
    if (!require(!QxpD3dTexBltRegistrationBatchCanCommit(
                     registration_actions, 2, 1, &required_slots) &&
                 required_slots == 2,
                 "prefix-mutating registration batch was admitted") ||
        !require(QxpD3dTexBltRegistrationBatchCanCommit(
                     registration_actions, 2, 2, &required_slots) &&
                 required_slots == 2,
                 "fully reserved registration batch was refused")) return 1;
    registration_actions[0] = QXP_D3D_TEXBLT_REGISTRATION_PRESERVE;
    registration_actions[1] = QXP_D3D_TEXBLT_REGISTRATION_RECREATE;
    if (!require(QxpD3dTexBltRegistrationBatchCanCommit(
                     registration_actions, 2, 0, &required_slots) &&
                 required_slots == 0,
                 "recreation incorrectly consumed a table slot")) return 1;
    registration_actions[1] = QXP_D3D_TEXBLT_REGISTRATION_REFUSE;
    if (!require(!QxpD3dTexBltRegistrationBatchCanCommit(
                     registration_actions, 2, 2, &required_slots),
                 "injected reference refusal was admitted") ||
        !require(QxpD3dTexBltCpuWriteDirectMatch(55, 55, 7, 0) &&
                 QxpD3dTexBltCpuWriteDirectMatch(55, 55, 7, 7),
                 "ambiguous retained/live CPU-write matches not modeled") ||
        !require(!QxpD3dTexBltCpuWriteDirectMatch(55, 55, 7, 8) &&
                 !QxpD3dTexBltCpuWriteDirectMatch(0, 0, 7, 7) &&
                 !QxpD3dTexBltCpuWriteDirectMatch(55, 56, 7, 7),
                 "conflicting CPU-write identity was matched") ||
        !require(QxpD3dTexBltCpuWriteChainMatch(
                     11, 101, 0, 11, 101, 0) &&
                 !QxpD3dTexBltCpuWriteChainMatch(
                     11, 101, 0, 22, 202, 0) &&
                 QxpD3dTexBltCpuWriteChainMatch(
                     22, 202, 0, 22, 202, 0) &&
                 !QxpD3dTexBltCpuWriteChainMatch(
                     22, 202, 0, 11, 101, 0),
                 "ambiguous owner chains were cross-selected") ||
        !require(QxpD3dTexBltCpuWriteChainMatch(
                     11, 999, 101, 11, 888, 101) &&
                 !QxpD3dTexBltCpuWriteChainMatch(
                     11, 101, 0, 11, 202, 202) &&
                 !QxpD3dTexBltCpuWriteChainMatch(
                     11, 0, 0, 11, 0, 0),
                 "CPU-write chain-name precedence was not enforced"))
        return 1;

    clock = 0;
    epoch = 0;
    if (!require(QxpD3dTexBltNextRecreationEpoch(&clock, &epoch) &&
                 clock == 1 && epoch == 1,
                 "first recreation epoch was not nonzero") ||
        !require(QxpD3dTexBltNextRecreationEpoch(&clock, &epoch) &&
                 clock == 2 && epoch == 2,
                 "recreation epoch did not advance")) return 1;
    clock = ~0UL;
    epoch = 99;
    if (!require(!QxpD3dTexBltNextRecreationEpoch(&clock, &epoch) &&
                 clock == ~0UL && epoch == 99,
                 "exhausted recreation epoch wrapped")) return 1;

    if (!require(QxpD3dTexBltMipTopologyCompatible(1, 1, 0, 0),
                 "single-level topology rejected") ||
        !require(QxpD3dTexBltMipTopologyCompatible(3, 3, 1, 1),
                 "equal mip topology rejected") ||
        !require(QxpD3dTexBltMipTopologyCompatible(3, 1, 1, 1),
                 "hidden destination mip proxy rejected") ||
        !require(!QxpD3dTexBltMipTopologyCompatible(3, 1, 0, 1),
                 "source without mip proxy accepted") ||
        !require(QxpD3dTexBltMipTopologyCompatible(3, 1, 1, 0),
                 "stock hidden destination mip proxy rejected") ||
        !require(!QxpD3dTexBltMipTopologyCompatible(1, 3, 1, 1),
                 "short source mip chain accepted") ||
        !require(!QxpD3dTexBltMipTopologyCompatible(3, 2, 1, 1),
                 "arbitrary unequal mip chains accepted") ||
        !require(!QxpD3dTexBltMipTopologyCompatible(0, 1, 1, 1),
                 "zero source levels accepted")) return 1;

    retired_first = 0;
    retired_last = 0;
    for (index = 1; index <= 5000; ++index) {
        if (!require(QxpD3dTexBltRetiredRangeAppend(
                         &retired_first, &retired_last, index),
                     "sustained retirement range exhausted")) return 1;
    }
    if (!require(retired_first == 1 && retired_last == 5000,
                 "sustained retirement range split unexpectedly") ||
        !require(QxpD3dTexBltRetiredRangeMerge(
                     &retired_first, &retired_last, 5001, 5100) &&
                 retired_first == 1 && retired_last == 5100,
                 "adjacent retirement ranges did not coalesce") ||
        !require(!QxpD3dTexBltRetiredRangeAppend(
                     &retired_first, &retired_last, 6000),
                 "disjoint retirement name corrupted a range") ||
        !require(retired_first == 1 && retired_last == 5100,
                 "failed range append changed quarantine state")) return 1;
    retired_first = 0xfffffffeUL;
    retired_last = 0xfffffffeUL;
    if (!require(QxpD3dTexBltRetiredRangeAppend(
                     &retired_first, &retired_last, 0xffffffffUL) &&
                 retired_last == 0xffffffffUL,
                 "maximum retirement name did not extend safely") ||
        !require(!QxpD3dTexBltRetiredRangesTouch(
                     0xfffffffeUL, 0xffffffffUL, 1, 1),
                 "retirement range wrapped across zero")) return 1;

    retirement_names[0] = 41;
    retirement_names[1] = 41;
    retirement_names[2] = 42;
    retirement_names[3] = 0;
    required_slots = 99;
    if (!require(QxpD3dTexBltRetirementCanAppend(
                     2, 4, retirement_names, 4, &required_slots) &&
                 required_slots == 2,
                 "exact-capacity retirement was refused") ||
        !require(!QxpD3dTexBltRetirementCanAppend(
                     3, 4, retirement_names, 4, &required_slots) &&
                 required_slots == 2,
                 "retirement overflow was accepted") ||
        !require(!QxpD3dTexBltRetirementCanAppend(
                     5, 4, retirement_names, 4, &required_slots),
                 "overfull quarantine state accepted")) return 1;

    memset(&transaction, 0, sizeof(transaction));
    transaction.current_name = 77;
    transaction.commit_result = 1;
    if (!require(QxpD3dTexBltRunTransaction(
                     88, transaction_test_upload,
                     transaction_test_commit, transaction_test_cleanup,
                     &transaction) ==
                     QXP_D3D_TEXBLT_TRANSACTION_UPLOAD_FAILED,
                 "partial scratch upload was not rejected") ||
        !require(transaction.upload_count == 1 &&
                 transaction.scratch_write_count == 1 &&
                 transaction.commit_count == 0 &&
                 transaction.cleanup_count == 1 &&
                 transaction.cleaned_name == 88 &&
                 transaction.current_name == 77,
                 "upload failure changed the committed name")) return 1;
    memset(&transaction, 0, sizeof(transaction));
    transaction.current_name = 77;
    transaction.upload_result = 1;
    if (!require(QxpD3dTexBltRunTransaction(
                     88, transaction_test_upload,
                     transaction_test_commit, transaction_test_cleanup,
                     &transaction) ==
                     QXP_D3D_TEXBLT_TRANSACTION_COMMIT_FAILED,
                 "failed commit was not rejected") ||
        !require(transaction.upload_count == 1 &&
                 transaction.commit_count == 1 &&
                 transaction.cleanup_count == 1 &&
                 transaction.cleaned_name == 88 &&
                 transaction.current_name == 77,
                 "commit failure changed the committed name")) return 1;
    memset(&transaction, 0, sizeof(transaction));
    transaction.current_name = 77;
    transaction.upload_result = 1;
    transaction.commit_result = 1;
    if (!require(QxpD3dTexBltRunTransaction(
                     88, transaction_test_upload,
                     transaction_test_commit, transaction_test_cleanup,
                     &transaction) ==
                     QXP_D3D_TEXBLT_TRANSACTION_SUCCESS,
                 "successful transaction was rejected") ||
        !require(transaction.current_name == 88 &&
                 transaction.commit_count == 1 &&
                 transaction.cleanup_count == 0,
                 "successful transaction was not committed atomically"))
        return 1;

    valid_fixture(&record, &source, &destination);
    memset(&plan, 0, sizeof(plan));
    if (!require(QxpD3dTexBltValidate(
                     &record, &source, &destination, &plan) ==
                     QXP_D3D_TEXBLT_SUCCESS,
                 "valid TEXBLT rejected") ||
        !require(plan.destination_left == 11 &&
                 plan.destination_top == 13 &&
                 plan.destination_right == 111 &&
                 plan.destination_bottom == 213,
                 "destination rectangle was not derived without scaling"))
        return 1;

    valid_fixture(&record, &source, &destination);
    record.flags = 1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_FLAGS, &record, &source,
                       &destination, "unknown flags accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    source.resolved = 0;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_HANDLE, &record,
                       &source, &destination,
                       "unresolved source accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.resolved = 0;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_HANDLE, &record,
                       &source, &destination,
                       "unresolved destination accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    record.destination_handle = record.source_handle;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SELF_COPY, &record, &source,
                       &destination, "self copy accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    source.is_texture = 0;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_NOT_TEXTURE, &record,
                       &source, &destination,
                       "non-texture source accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.is_texture = 0;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_NOT_TEXTURE,
                       &record, &source, &destination,
                       "non-texture destination accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    source.is_cube = 1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_CUBE, &record, &source,
                       &destination, "cube source accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.is_mipmapped = 1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_MIP, &record,
                       &source, &destination,
                       "mip destination accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.is_system_memory = 1;
    if (!expect_reason(
            QXP_D3D_TEXBLT_REJECT_DESTINATION_SYSTEM_MEMORY, &record,
            &source, &destination,
            "system-memory destination accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.is_bound_destination = 1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUND, &record,
                       &source, &destination,
                       "bound destination accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    destination.format = QXP_D3D_TEXBLT_FORMAT_X8R8G8B8;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_FORMAT_MISMATCH, &record,
                       &source, &destination,
                       "mismatched formats accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    source.format = QXP_D3D_TEXBLT_FORMAT_P8;
    destination.format = QXP_D3D_TEXBLT_FORMAT_P8;
    source.palette_handle = 3;
    destination.palette_handle = 4;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_PALETTE_MISMATCH, &record,
                       &source, &destination,
                       "palettized copy with different palettes accepted"))
        return 1;
    /* Both formats move together: an unsupported format on only one side is a
     * mismatch, which is a different and equally correct refusal.  The case
     * this names is an unsupported format the two surfaces agree on, which the
     * blit route must still refuse because a host framebuffer may not be able
     * to attach it. */
    valid_fixture(&record, &source, &destination);
    source.format = 99;
    destination.format = 99;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_FORMAT, &record,
                       &source, &destination,
                       "unsupported source format accepted")) return 1;

    /* The upload route carries no colour-format restriction, because it goes
     * through the format-agnostic level upload rather than a host blit.  A
     * full-surface copy from a system-memory staging surface in a format the
     * blit route refuses must therefore be admitted, and routed as an upload.
     * This is the case the stock runtime issues for a bump-map texture. */
    valid_fixture(&record, &source, &destination);
    source.is_system_memory = 1;
    source.width = 4;
    source.height = 4;
    destination.width = 4;
    destination.height = 4;
    source.format = 60;
    destination.format = 60;
    record.source_left = 0;
    record.source_top = 0;
    record.source_right = 4;
    record.source_bottom = 4;
    record.destination_x = 0;
    record.destination_y = 0;
    memset(&plan, 0, sizeof(plan));
    if (!require(QxpD3dTexBltValidate(&record, &source, &destination,
                                      &plan) == QXP_D3D_TEXBLT_SUCCESS,
                 "full system-memory copy of an unsupported blit format "
                 "was refused") ||
        !require(plan.route == QXP_D3D_TEXBLT_ROUTE_UPLOAD,
                 "full system-memory copy was not routed as an upload"))
        return 1;

    /* TEXBLT names a cube face's top-level surface and carries all common mip
     * levels of that face.  A full staging upload is therefore valid for a
     * cube mip chain when both resources have the same topology. */
    source.is_cube = 1;
    destination.is_cube = 1;
    source.is_mipmapped = 1;
    destination.is_mipmapped = 1;
    memset(&plan, 0, sizeof(plan));
    if (!require(QxpD3dTexBltValidate(&record, &source, &destination,
                                      &plan) == QXP_D3D_TEXBLT_SUCCESS,
                 "cube mip-chain staging upload was refused") ||
        !require(plan.route == QXP_D3D_TEXBLT_ROUTE_UPLOAD,
                 "cube mip-chain copy was not routed as an upload"))
        return 1;
    destination.is_cube = 0;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_CUBE, &record,
                       &source, &destination,
                       "cube-to-2D topology mismatch accepted")) return 1;

    /* A partial copy cannot be an upload, so it stays a blit and keeps the
     * restriction: the route must follow the shape of the copy, not merely
     * the fact that the source happens to live in system memory. */
    source.is_cube = 0;
    source.is_mipmapped = 0;
    destination.is_mipmapped = 0;
    record.source_right = 3;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_FORMAT, &record,
                       &source, &destination,
                       "partial system-memory copy escaped the blit "
                       "format restriction")) return 1;
    valid_fixture(&record, &source, &destination);
    record.source_right = record.source_left;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_EMPTY, &record,
                       &source, &destination,
                       "empty source rectangle accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    record.source_left = -1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_BOUNDS, &record,
                       &source, &destination,
                       "negative source rectangle accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    record.source_right = 257;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_RECT_BOUNDS, &record,
                       &source, &destination,
                       "source rectangle overflow accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    record.destination_x = -1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_POINT, &record,
                       &source, &destination,
                       "negative destination point accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    record.destination_x = 157;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_DESTINATION_BOUNDS, &record,
                       &source, &destination,
                       "destination overflow accepted")) return 1;
    valid_fixture(&record, &source, &destination);
    source.width = QXP_D3D_TEXBLT_MAX_DIMENSION + 1;
    if (!expect_reason(QXP_D3D_TEXBLT_REJECT_SOURCE_GEOMETRY, &record,
                       &source, &destination,
                       "oversized source accepted")) return 1;

    puts("PASS TEXBLT record parsing, bounds, and refusal validation");
    return 0;
}
