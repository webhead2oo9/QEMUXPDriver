#include "qxp_d3d9_sampler.h"

static void qxp_sampler_zero(void *destination, DWORD bytes)
{
    volatile BYTE *output = (volatile BYTE *)destination;

    while (bytes--) *output++ = 0;
}

static BOOL qxp_sampler_finite(DWORD bits)
{
    return (bits & 0x7f800000UL) != 0x7f800000UL;
}

static BOOL qxp_sampler_fail(
    QXP_D3D9_SAMPLER_STATUS *status, DWORD error, DWORD sampler,
    DWORD state, DWORD value)
{
    if (status) {
        status->error = error;
        status->sampler = sampler;
        status->state = state;
        status->value = value;
    }
    return FALSE;
}

static void qxp_sampler_reset_status(QXP_D3D9_SAMPLER_STATUS *status)
{
    if (status) qxp_sampler_zero(status, sizeof(*status));
}
static void qxp_sampler_record_support(
    QXP_D3D9_SAMPLER_STATE *state, DWORD sampler_state, DWORD value,
    BOOL supported)
{
    DWORD bit = 1UL << sampler_state;
    DWORD pending;

    if (!supported) {
        state->unsupported_mask |= bit;
        state->unsupported_state = sampler_state;
        state->unsupported_value = value;
        return;
    }
    state->unsupported_mask &= ~bit;
    if (state->unsupported_state != sampler_state) return;
    state->unsupported_state = 0;
    state->unsupported_value = 0;
    pending = state->unsupported_mask;
    for (sampler_state = 0; pending; ++sampler_state, pending >>= 1) {
        if (pending & 1UL) {
            state->unsupported_state = sampler_state;
            break;
        }
    }
}


void QxpD3d9SamplerInitialize(QXP_D3D9_SAMPLER_STATE *state)
{
    if (!state) return;
    qxp_sampler_zero(state, sizeof(*state));
    state->address_u = QXP_D3D9_TEXTURE_ADDRESS_WRAP;
    state->address_v = QXP_D3D9_TEXTURE_ADDRESS_WRAP;
    state->mag_filter = QXP_D3D9_TEXTURE_FILTER_POINT;
    state->min_filter = QXP_D3D9_TEXTURE_FILTER_POINT;
    state->mip_filter = QXP_D3D9_TEXTURE_FILTER_NONE;
}

BOOL QxpD3d9SamplerSetState(
    QXP_D3D9_SAMPLER_STATE *state, DWORD sampler_state, DWORD value,
    QXP_D3D9_SAMPLER_STATUS *status)
{
    BOOL supported;

    qxp_sampler_reset_status(status);
    if (!state)
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_ARGUMENT, 0,
            sampler_state, value);

    switch (sampler_state) {
    case QXP_D3D9_SAMPLER_ADDRESS_U:
    case QXP_D3D9_SAMPLER_ADDRESS_V:
        supported = value >= QXP_D3D9_TEXTURE_ADDRESS_WRAP &&
                    value <= QXP_D3D9_TEXTURE_ADDRESS_CLAMP;
        if (supported) {
            if (sampler_state == QXP_D3D9_SAMPLER_ADDRESS_U)
                state->address_u = value;
            else
                state->address_v = value;
        }
        break;
    case QXP_D3D9_SAMPLER_MAG_FILTER:
    case QXP_D3D9_SAMPLER_MIN_FILTER:
        supported = value == QXP_D3D9_TEXTURE_FILTER_POINT ||
                    value == QXP_D3D9_TEXTURE_FILTER_LINEAR;
        if (supported) {
            if (sampler_state == QXP_D3D9_SAMPLER_MAG_FILTER)
                state->mag_filter = value;
            else
                state->min_filter = value;
        }
        break;
    case QXP_D3D9_SAMPLER_MIP_FILTER:
        supported = value <= QXP_D3D9_TEXTURE_FILTER_LINEAR;
        if (supported) state->mip_filter = value;
        break;
    case QXP_D3D9_SAMPLER_MIP_LOD_BIAS:
        supported = qxp_sampler_finite(value);
        if (supported) state->mip_lod_bias_bits = value;
        break;
    case QXP_D3D9_SAMPLER_MAX_MIP_LEVEL:
        supported = TRUE;
        state->max_mip_level = value;
        break;
    case QXP_D3D9_SAMPLER_ADDRESS_W:
        supported = value == QXP_D3D9_TEXTURE_ADDRESS_WRAP;
        break;
    case QXP_D3D9_SAMPLER_BORDER_COLOR:
    case QXP_D3D9_SAMPLER_SRGB_TEXTURE:
    case QXP_D3D9_SAMPLER_ELEMENT_INDEX:
    case QXP_D3D9_SAMPLER_DMAP_OFFSET:
        supported = value == 0;
        break;
    case QXP_D3D9_SAMPLER_MAX_ANISOTROPY:
        supported = value == 1;
        break;
    default:
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_STATE, 0,
            sampler_state, value);
    }
    qxp_sampler_record_support(state, sampler_state, value, supported);
    return TRUE;
}

BOOL QxpD3d9SamplerSetBinding(
    QXP_D3D9_SAMPLER_STATE *state, DWORD handle,
    ULONG_PTR owner_identity, ULONG_PTR resource_identity,
    ULONGLONG incarnation, QXP_D3D9_SAMPLER_STATUS *status)
{
    qxp_sampler_reset_status(status);
    if (!state)
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_ARGUMENT, 0, 0, handle);
    if (!handle) {
        qxp_sampler_zero(&state->binding, sizeof(state->binding));
        return TRUE;
    }
    if (!owner_identity || !resource_identity || !incarnation)
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_BINDING, 0, 0, handle);
    state->binding.handle = handle;
    state->binding.owner_identity = owner_identity;
    state->binding.resource_identity = resource_identity;
    state->binding.incarnation = incarnation;
    return TRUE;
}

BOOL QxpD3d9SamplerBuildWireState(
    const QXP_D3D9_SAMPLER_STATE states[QXP_D3D_SM2_SAMPLER_COUNT],
    DWORD sampled_mask, DWORD cube_mask, DWORD volume_mask,
    ULONG_PTR expected_owner_identity, QXP_D3D9_SAMPLER_RESOLVE resolve,
    void *resolve_context,
    QXP_D3D_SM2_SAMPLER_STATE output[QXP_D3D_SM2_SAMPLER_COUNT],
    QXP_D3D9_SAMPLER_STATUS *status)
{
    DWORD sampler;
    DWORD bit;
    DWORD texture_type;
    DWORD texture_name;
    const QXP_D3D9_SAMPLER_STATE *state;

    qxp_sampler_reset_status(status);
    if (output)
        qxp_sampler_zero(
            output, sizeof(*output) * QXP_D3D_SM2_SAMPLER_COUNT);
    if (!states || !expected_owner_identity || !resolve || !output)
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_ARGUMENT, 0, 0, 0);
    if ((sampled_mask & ~0xffffUL) ||
        (cube_mask & ~sampled_mask) ||
        (volume_mask & ~sampled_mask) ||
        (cube_mask & volume_mask))
        return qxp_sampler_fail(
            status, QXP_D3D9_SAMPLER_ERROR_MASK, 0, 0,
            sampled_mask | cube_mask | volume_mask);

    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT; ++sampler) {
        bit = 1UL << sampler;
        if (!(sampled_mask & bit)) continue;
        state = &states[sampler];
        if (state->unsupported_mask)
            return qxp_sampler_fail(
                status, QXP_D3D9_SAMPLER_ERROR_STATE, sampler,
                state->unsupported_state, state->unsupported_value);
        texture_type = (cube_mask & bit) ? QXP_D3D_TEXTURE_CUBE :
                       (volume_mask & bit) ? QXP_D3D_TEXTURE_VOLUME :
                       QXP_D3D_TEXTURE_2D;
        /* A shader may sample a stage with nothing bound to it.  Direct3D does
         * not fail such a draw - the fetch simply returns the device's default
         * - so leave the wire record fully zeroed, which is the transport's
         * established unbound form: the packet contract admits it and the
         * miniport resolves it to the opaque-black dummy of the mask-selected
         * target, exactly as it does for a missing fixed-function stage.
         * A binding that is present but disagrees with its owner or resource
         * identity is a different matter and still refuses: that is stale
         * state, not an empty stage. */
        if (!state->binding.handle) continue;
        if (state->binding.owner_identity != expected_owner_identity ||
            !state->binding.resource_identity ||
            !state->binding.incarnation)
            return qxp_sampler_fail(
                status, QXP_D3D9_SAMPLER_ERROR_BINDING, sampler, 0,
                state->binding.handle);
        if (state->address_u < QXP_D3D9_TEXTURE_ADDRESS_WRAP ||
            state->address_u > QXP_D3D9_TEXTURE_ADDRESS_CLAMP ||
            state->address_v < QXP_D3D9_TEXTURE_ADDRESS_WRAP ||
            state->address_v > QXP_D3D9_TEXTURE_ADDRESS_CLAMP ||
            (state->mag_filter != QXP_D3D9_TEXTURE_FILTER_POINT &&
             state->mag_filter != QXP_D3D9_TEXTURE_FILTER_LINEAR) ||
            (state->min_filter != QXP_D3D9_TEXTURE_FILTER_POINT &&
             state->min_filter != QXP_D3D9_TEXTURE_FILTER_LINEAR) ||
            state->mip_filter > QXP_D3D9_TEXTURE_FILTER_LINEAR ||
            !qxp_sampler_finite(state->mip_lod_bias_bits))
            return qxp_sampler_fail(
                status, QXP_D3D9_SAMPLER_ERROR_STATE, sampler, 0, 0);

        texture_name = 0;
        if (!resolve(
                resolve_context, sampler, texture_type,
                &state->binding, &texture_name) || !texture_name)
            return qxp_sampler_fail(
                status, QXP_D3D9_SAMPLER_ERROR_RESOURCE, sampler, 0,
                state->binding.handle);
        output[sampler].texture_name = texture_name;
        output[sampler].texture_type = texture_type;
        output[sampler].address_u = state->address_u;
        output[sampler].address_v = state->address_v;
        output[sampler].mag_filter = state->mag_filter;
        output[sampler].min_filter = state->min_filter;
        output[sampler].mip_filter = state->mip_filter;
        output[sampler].mip_lod_bias_bits = state->mip_lod_bias_bits;
        output[sampler].max_mip_level = state->max_mip_level;
    }
    return TRUE;
}
