#ifndef QXP_D3D9_SAMPLER_H
#define QXP_D3D9_SAMPLER_H

#include <windef.h>

#include "../qxp_d3d_transport.h"

#define QXP_D3D9_SAMPLER_TEXTURE_MAP 0UL
#define QXP_D3D9_SAMPLER_ADDRESS_U 13UL
#define QXP_D3D9_SAMPLER_ADDRESS_V 14UL
#define QXP_D3D9_SAMPLER_ADDRESS_W 25UL
#define QXP_D3D9_SAMPLER_BORDER_COLOR 15UL
#define QXP_D3D9_SAMPLER_MAG_FILTER 16UL
#define QXP_D3D9_SAMPLER_MIN_FILTER 17UL
#define QXP_D3D9_SAMPLER_MIP_FILTER 18UL
#define QXP_D3D9_SAMPLER_MIP_LOD_BIAS 19UL
#define QXP_D3D9_SAMPLER_MAX_MIP_LEVEL 20UL
#define QXP_D3D9_SAMPLER_MAX_ANISOTROPY 21UL
#define QXP_D3D9_SAMPLER_SRGB_TEXTURE 29UL
#define QXP_D3D9_SAMPLER_ELEMENT_INDEX 30UL
#define QXP_D3D9_SAMPLER_DMAP_OFFSET 31UL

#define QXP_D3D9_TEXTURE_ADDRESS_WRAP 1UL
#define QXP_D3D9_TEXTURE_ADDRESS_MIRROR 2UL
#define QXP_D3D9_TEXTURE_ADDRESS_CLAMP 3UL

#define QXP_D3D9_TEXTURE_FILTER_NONE 0UL
#define QXP_D3D9_TEXTURE_FILTER_POINT 1UL
#define QXP_D3D9_TEXTURE_FILTER_LINEAR 2UL

typedef struct qxp_d3d9_sampler_binding {
    DWORD handle;
    ULONG_PTR owner_identity;
    ULONG_PTR resource_identity;
    ULONGLONG incarnation;
} QXP_D3D9_SAMPLER_BINDING;

typedef struct qxp_d3d9_sampler_state {
    QXP_D3D9_SAMPLER_BINDING binding;
    DWORD address_u;
    DWORD address_v;
    DWORD mag_filter;
    DWORD min_filter;
    DWORD mip_filter;
    DWORD mip_lod_bias_bits;
    DWORD max_mip_level;
    DWORD unsupported_mask;
    DWORD unsupported_state;
    DWORD unsupported_value;
} QXP_D3D9_SAMPLER_STATE;

typedef enum qxp_d3d9_sampler_error {
    QXP_D3D9_SAMPLER_ERROR_NONE = 0,
    QXP_D3D9_SAMPLER_ERROR_ARGUMENT,
    QXP_D3D9_SAMPLER_ERROR_MASK,
    QXP_D3D9_SAMPLER_ERROR_STATE,
    QXP_D3D9_SAMPLER_ERROR_BINDING,
    QXP_D3D9_SAMPLER_ERROR_RESOURCE
} QXP_D3D9_SAMPLER_ERROR;

typedef struct qxp_d3d9_sampler_status {
    DWORD error;
    DWORD sampler;
    DWORD state;
    DWORD value;
} QXP_D3D9_SAMPLER_STATUS;

typedef BOOL (*QXP_D3D9_SAMPLER_RESOLVE)(
    void *context, DWORD sampler, DWORD texture_type,
    const QXP_D3D9_SAMPLER_BINDING *binding, DWORD *texture_name);

void QxpD3d9SamplerInitialize(QXP_D3D9_SAMPLER_STATE *state);
BOOL QxpD3d9SamplerSetState(
    QXP_D3D9_SAMPLER_STATE *state, DWORD sampler_state, DWORD value,
    QXP_D3D9_SAMPLER_STATUS *status);
BOOL QxpD3d9SamplerSetBinding(
    QXP_D3D9_SAMPLER_STATE *state, DWORD handle,
    ULONG_PTR owner_identity, ULONG_PTR resource_identity,
    ULONGLONG incarnation, QXP_D3D9_SAMPLER_STATUS *status);
BOOL QxpD3d9SamplerBuildWireState(
    const QXP_D3D9_SAMPLER_STATE states[QXP_D3D_SM2_SAMPLER_COUNT],
    DWORD sampled_mask, DWORD cube_mask, DWORD volume_mask,
    ULONG_PTR expected_owner_identity, QXP_D3D9_SAMPLER_RESOLVE resolve,
    void *resolve_context,
    QXP_D3D_SM2_SAMPLER_STATE output[QXP_D3D_SM2_SAMPLER_COUNT],
    QXP_D3D9_SAMPLER_STATUS *status);

#endif
