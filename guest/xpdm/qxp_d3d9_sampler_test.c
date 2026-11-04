#include <stdio.h>
#include <string.h>
#include <windef.h>

#include "qxpdisp/qxp_d3d9_sampler.h"

static int qxp_failures;

static void qxp_check(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "qxp_d3d9_sampler_test: %s\n", message);
    ++qxp_failures;
}

typedef struct qxp_sampler_resolver {
    ULONG_PTR owner;
    ULONG_PTR resource_identity[QXP_D3D_SM2_SAMPLER_COUNT];
    ULONGLONG incarnation[QXP_D3D_SM2_SAMPLER_COUNT];
    DWORD texture_name[QXP_D3D_SM2_SAMPLER_COUNT];
} QXP_SAMPLER_RESOLVER;

static BOOL qxp_resolve_sampler(
    void *opaque, DWORD sampler, DWORD texture_type,
    const QXP_D3D9_SAMPLER_BINDING *binding, DWORD *texture_name)
{
    QXP_SAMPLER_RESOLVER *resolver = (QXP_SAMPLER_RESOLVER *)opaque;

    if (!resolver || !binding || !texture_name ||
        sampler >= QXP_D3D_SM2_SAMPLER_COUNT ||
        (texture_type != QXP_D3D_TEXTURE_2D &&
         texture_type != QXP_D3D_TEXTURE_CUBE &&
         texture_type != QXP_D3D_TEXTURE_VOLUME) ||
        binding->owner_identity != resolver->owner ||
        binding->resource_identity !=
            resolver->resource_identity[sampler] ||
        binding->incarnation != resolver->incarnation[sampler])
        return FALSE;
    *texture_name = resolver->texture_name[sampler];
    return *texture_name != 0;
}

static void qxp_test_state_contract(void)
{
    QXP_D3D9_SAMPLER_STATE state;
    QXP_D3D9_SAMPLER_STATE saved;
    QXP_D3D9_SAMPLER_STATUS status;

    qxp_check(QXP_D3D9_SAMPLER_ADDRESS_U == 13 &&
              QXP_D3D9_SAMPLER_ADDRESS_V == 14 &&
              QXP_D3D9_SAMPLER_BORDER_COLOR == 15 &&
              QXP_D3D9_SAMPLER_MAG_FILTER == 16 &&
              QXP_D3D9_SAMPLER_MAX_ANISOTROPY == 21 &&
              QXP_D3D9_SAMPLER_ADDRESS_W == 25 &&
              QXP_D3D9_SAMPLER_SRGB_TEXTURE == 29 &&
              QXP_D3D9_SAMPLER_ELEMENT_INDEX == 30 &&
              QXP_D3D9_SAMPLER_DMAP_OFFSET == 31,
              "sampler state IDs do not match the XP DDI contract");
    QxpD3d9SamplerInitialize(&state);
    qxp_check(!state.binding.handle && !state.binding.resource_identity &&
              !state.binding.incarnation &&
              state.address_u == QXP_D3D9_TEXTURE_ADDRESS_WRAP &&
              state.address_v == QXP_D3D9_TEXTURE_ADDRESS_WRAP &&
              state.mag_filter == QXP_D3D9_TEXTURE_FILTER_POINT &&
              state.min_filter == QXP_D3D9_TEXTURE_FILTER_POINT &&
              state.mip_filter == QXP_D3D9_TEXTURE_FILTER_NONE,
              "initial state is not the deterministic D3D default");

    qxp_check(!QxpD3d9SamplerSetBinding(
                  &state, 19, 0, 0x1234, 7, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_BINDING,
              "accepted a binding without owner identity");
    qxp_check(QxpD3d9SamplerSetBinding(
                  &state, 19, 0x55, 0x1234, 7, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_NONE &&
              state.binding.handle == 19 &&
              state.binding.owner_identity == 0x55 &&
              state.binding.resource_identity == 0x1234 &&
              state.binding.incarnation == 7,
              "did not publish an owner-qualified sampler binding");

    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_U,
                  QXP_D3D9_TEXTURE_ADDRESS_CLAMP, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_V,
                  QXP_D3D9_TEXTURE_ADDRESS_MIRROR, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MAG_FILTER,
                  QXP_D3D9_TEXTURE_FILTER_LINEAR, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MIN_FILTER,
                  QXP_D3D9_TEXTURE_FILTER_LINEAR, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MIP_FILTER,
                  QXP_D3D9_TEXTURE_FILTER_LINEAR, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MIP_LOD_BIAS,
                  0xbf000000UL, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MAX_MIP_LEVEL,
                  9, &status),
              "rejected supported sampler state");
    qxp_check(state.address_u == QXP_D3D9_TEXTURE_ADDRESS_CLAMP &&
              state.address_v == QXP_D3D9_TEXTURE_ADDRESS_MIRROR &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_W,
                  QXP_D3D9_TEXTURE_ADDRESS_WRAP, &status) &&
              QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_SRGB_TEXTURE, 0, &status) &&
              state.mag_filter == QXP_D3D9_TEXTURE_FILTER_LINEAR &&
              state.min_filter == QXP_D3D9_TEXTURE_FILTER_LINEAR &&
              state.mip_filter == QXP_D3D9_TEXTURE_FILTER_LINEAR &&
              state.mip_lod_bias_bits == 0xbf000000UL &&
              state.max_mip_level == 9,
              "supported sampler state did not retain exact values");

    saved = state;
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_U, 0, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_NONE &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_ADDRESS_U) &&
              state.unsupported_state == QXP_D3D9_SAMPLER_ADDRESS_U &&
              state.unsupported_value == 0 &&
              state.address_u == saved.address_u,
              "unsupported address mode was not deferred");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_U,
                  saved.address_u, &status) &&
              !memcmp(&state, &saved, sizeof(state)),
              "supported address mode did not repair deferred state");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MAG_FILTER,
                  QXP_D3D9_TEXTURE_FILTER_NONE, &status) &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_MAG_FILTER) &&
              state.mag_filter == saved.mag_filter,
              "unsupported magnitude filter was not deferred");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MAG_FILTER,
                  saved.mag_filter, &status) &&
              !memcmp(&state, &saved, sizeof(state)),
              "supported magnitude filter did not repair deferred state");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MIP_LOD_BIAS,
                  0x7f800000UL, &status) &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_MIP_LOD_BIAS) &&
              state.mip_lod_bias_bits == saved.mip_lod_bias_bits,
              "non-finite LOD bias was not deferred");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_MIP_LOD_BIAS,
                  saved.mip_lod_bias_bits, &status) &&
              !memcmp(&state, &saved, sizeof(state)),
              "finite LOD bias did not repair deferred state");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_BORDER_COLOR,
                  0x01020304UL, &status) &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_BORDER_COLOR),
              "nonzero border color aborted instead of deferring");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_BORDER_COLOR, 0, &status) &&
              !memcmp(&state, &saved, sizeof(state)),
              "zero border color did not repair deferred state");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_W,
                  QXP_D3D9_TEXTURE_ADDRESS_CLAMP, &status) &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_ADDRESS_W),
              "unsupported W address mode was not deferred");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_SRGB_TEXTURE, 1, &status) &&
              state.unsupported_mask ==
                  ((1UL << QXP_D3D9_SAMPLER_ADDRESS_W) |
                   (1UL << QXP_D3D9_SAMPLER_SRGB_TEXTURE)) &&
              state.unsupported_state == QXP_D3D9_SAMPLER_SRGB_TEXTURE &&
              state.unsupported_value == 1,
              "multiple unsupported sampler states were not retained");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_SRGB_TEXTURE, 0, &status) &&
              state.unsupported_mask ==
                  (1UL << QXP_D3D9_SAMPLER_ADDRESS_W) &&
              state.unsupported_state == QXP_D3D9_SAMPLER_ADDRESS_W,
              "repairing one sampler state lost another refusal");
    qxp_check(QxpD3d9SamplerSetState(
                  &state, QXP_D3D9_SAMPLER_ADDRESS_W,
                  QXP_D3D9_TEXTURE_ADDRESS_WRAP, &status) &&
              !memcmp(&state, &saved, sizeof(state)),
              "supported W address mode did not repair deferred state");
    qxp_check(!QxpD3d9SamplerSetState(
                  &state, 0xffffffffUL, 1, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_STATE &&
              !memcmp(&state, &saved, sizeof(state)),
              "unknown sampler state mutated the binding");

    qxp_check(QxpD3d9SamplerSetBinding(
                  &state, 0, 0, 0, 0, &status) &&
              !state.binding.handle && !state.binding.owner_identity &&
              !state.binding.resource_identity &&
              !state.binding.incarnation,
              "explicit sampler unbind retained resource authority");
    qxp_check(state.address_u == saved.address_u &&
              state.max_mip_level == saved.max_mip_level,
              "texture unbind reset independent sampler state");
}

static void qxp_test_wire_state(void)
{
    QXP_D3D9_SAMPLER_STATE states[QXP_D3D_SM2_SAMPLER_COUNT];
    QXP_D3D_SM2_SAMPLER_STATE wire[QXP_D3D_SM2_SAMPLER_COUNT];
    QXP_D3D9_SAMPLER_STATUS status;
    QXP_SAMPLER_RESOLVER resolver;
    DWORD sampler;
    DWORD all = QXP_D3D_SM2_SAMPLER_MASK;

    memset(&resolver, 0, sizeof(resolver));
    resolver.owner = 0x77;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT; ++sampler) {
        QxpD3d9SamplerInitialize(&states[sampler]);
        resolver.resource_identity[sampler] = 0x1000UL + sampler;
        resolver.incarnation[sampler] = 0x2000ULL + sampler;
        resolver.texture_name[sampler] = 0x3000UL + sampler;
        qxp_check(QxpD3d9SamplerSetBinding(
                      &states[sampler], 0x4000UL + sampler,
                      resolver.owner, resolver.resource_identity[sampler],
                      resolver.incarnation[sampler], &status),
                  "failed to bind one of sixteen sampler registers");
        qxp_check(QxpD3d9SamplerSetState(
                      &states[sampler],
                      QXP_D3D9_SAMPLER_MAX_MIP_LEVEL,
                      sampler, &status),
                  "failed to set one of sixteen sampler states");
    }

    qxp_check(QxpD3d9SamplerBuildWireState(
                  states, all, 1UL << 14, 1UL << 15,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status),
              "rejected a complete sixteen-sampler wire snapshot");
    qxp_check(wire[0].texture_name == 0x3000UL &&
              wire[4].texture_name == 0x3004UL &&
              wire[15].texture_name == 0x300fUL &&
              wire[4].texture_type == QXP_D3D_TEXTURE_2D &&
              wire[14].texture_type == QXP_D3D_TEXTURE_CUBE &&
              wire[15].texture_type == QXP_D3D_TEXTURE_VOLUME &&
              wire[15].max_mip_level == 15,
              "wire snapshot lost high-register binding or state");
    qxp_check(QxpD3d9SamplerSetState(
                  &states[15], QXP_D3D9_SAMPLER_SRGB_TEXTURE,
                  1, &status) &&
              !QxpD3d9SamplerBuildWireState(
                  states, all, 1UL << 14, 1UL << 15,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_STATE &&
              status.sampler == 15 &&
              status.state == QXP_D3D9_SAMPLER_SRGB_TEXTURE &&
              status.value == 1,
              "unsupported state reached the sixteen-sampler wire");
    qxp_check(QxpD3d9SamplerSetState(
                  &states[15], QXP_D3D9_SAMPLER_SRGB_TEXTURE,
                  0, &status),
              "failed to repair high-register sampler state");

    resolver.incarnation[15]++;
    qxp_check(!QxpD3d9SamplerBuildWireState(
                  states, all, 1UL << 14, 1UL << 15,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_RESOURCE &&
              status.sampler == 15,
              "accepted stale high-register resource incarnation");
    resolver.incarnation[15]--;

    qxp_check(!QxpD3d9SamplerBuildWireState(
                  states, all, 1UL << 14, 1UL << 15,
                  0x78, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_BINDING &&
              status.sampler == 0,
              "accepted sampler bindings owned by another context");

    qxp_check(!QxpD3d9SamplerBuildWireState(
                  states, 1UL, 2UL, 0,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_MASK,
              "accepted cube mask outside the sampled set");

    qxp_check(QxpD3d9SamplerBuildWireState(
                  states, 1UL << 15, 0, 0,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              !wire[0].texture_name && !wire[14].texture_name &&
              wire[15].texture_name == 0x300fUL,
              "inactive samplers were not zeroed deterministically");

    /* Direct3D does not fail a draw that samples a stage with nothing bound.
     * The unbound stage must reach the wire as the fully zeroed record the
     * packet contract admits and the miniport resolves to its opaque-black
     * dummy - not as a refusal, and not as a partially filled record that
     * would fail the contract's all-or-nothing zero check. */
    QxpD3d9SamplerInitialize(&states[2]);
    qxp_check(QxpD3d9SamplerBuildWireState(
                  states, (1UL << 15) | (1UL << 2), 0, 0,
                  resolver.owner, qxp_resolve_sampler, &resolver,
                  wire, &status) &&
              status.error == QXP_D3D9_SAMPLER_ERROR_NONE &&
              wire[15].texture_name == 0x300fUL &&
              !wire[2].texture_name && !wire[2].texture_type &&
              !wire[2].address_u && !wire[2].address_v &&
              !wire[2].mag_filter && !wire[2].min_filter &&
              !wire[2].mip_filter && !wire[2].mip_lod_bias_bits &&
              !wire[2].max_mip_level,
              "sampled stage with no binding must wire as the zeroed form");
}

int main(void)
{
    qxp_test_state_contract();
    qxp_test_wire_state();
    if (qxp_failures) return 1;
    puts("qxp d3d9 sampler tests passed");
    return 0;
}
