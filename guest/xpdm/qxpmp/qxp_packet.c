#include <stddef.h>

#ifdef QXP_PACKET_STANDALONE
#include <windef.h>
#else
#include "qxpmp.h"
#endif

#include "qxp_packet.h"

#define QXP_PACKET_FVF_POSITION_MASK 0x0000000eUL
#define QXP_PACKET_FVF_XYZ 0x00000002UL
#define QXP_PACKET_FVF_XYZRHW 0x00000004UL
#define QXP_PACKET_FVF_NORMAL 0x00000010UL
#define QXP_PACKET_FVF_PSIZE 0x00000020UL
#define QXP_PACKET_FVF_DIFFUSE 0x00000040UL
#define QXP_PACKET_FVF_SPECULAR 0x00000080UL
#define QXP_PACKET_FVF_FOG 0x00002000UL
#define QXP_PACKET_FVF_TEXCOUNT_MASK 0x00000f00UL
#define QXP_PACKET_FVF_TEXTURE_FORMAT_MASK 0x00ff0000UL
#define QXP_PACKET_D3DMCS_COLOR1 1UL
#define QXP_PACKET_D3DMCS_COLOR2 2UL

static BOOLEAN qxp_packet_float_finite(ULONG value)
{
    return (value & 0x7f800000UL) != 0x7f800000UL;
}

static BOOLEAN qxp_packet_float_array(const ULONG *values, ULONG count)
{
    ULONG index;

    if (!values) return FALSE;
    for (index = 0; index < count; ++index) {
        if (!qxp_packet_float_finite(values[index])) return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_packet_hwtl_state(const QXP_D3D_HWTL_STATE *state)
{
    ULONG index;
    ULONG light;
    ULONG flags;

    if (!state || state->active_light_count > QXP_D3D_MAX_ACTIVE_LIGHTS ||
        state->lighting_enable > 1 || state->color_vertex_enable > 1 ||
        state->local_viewer > 1 || state->normalize_normals > 1 ||
        state->clipping_enable > 1 || state->range_fog_enable > 1 ||
        state->fog_vertex_mode > 3 ||
        state->diffuse_material_source > 2 ||
        state->specular_material_source > 2 ||
        state->ambient_material_source > 2 ||
        state->emissive_material_source > 2 ||
        (state->clip_plane_enable &
         ~((1UL << QXP_D3D_MAX_USER_CLIP_PLANES) - 1UL)) ||
        !qxp_packet_float_finite(state->z_min_bits) ||
        !qxp_packet_float_finite(state->z_max_bits) ||
        (state->z_min_bits & 0x80000000UL) ||
        (state->z_max_bits & 0x80000000UL) ||
        state->z_min_bits > state->z_max_bits ||
        state->z_max_bits > 0x3f800000UL ||
        !qxp_packet_float_array(
            (const ULONG *)state->world,
            QXP_D3D_MAX_WORLD_MATRICES * 16UL) ||
        !qxp_packet_float_array(state->view.element, 16) ||
        !qxp_packet_float_array(state->projection.element, 16) ||
        !qxp_packet_float_array(
            (const ULONG *)state->texture,
            QXP_D3D_MAX_TEXTURE_MATRICES * 16UL) ||
        !qxp_packet_float_array(
            (const ULONG *)&state->material,
            sizeof(state->material) / sizeof(ULONG)) ||
        !qxp_packet_float_array(
            (const ULONG *)state->clip_planes,
            QXP_D3D_MAX_USER_CLIP_PLANES * 4UL)) return FALSE;
    for (index = 0; index < 4; ++index) {
        flags = state->texture_transform_flags[index];
        if ((flags & ~0x0000010fUL) || (flags & 0x0fUL) > 4 ||
            ((flags & 0x00000100UL) && !(flags & 0x0fUL))) return FALSE;
    }
    for (light = 0; light < state->active_light_count; ++light) {
        const QXP_D3D_LIGHT *value = &state->active_lights[light].light;

        if (value->type < 1 || value->type > 3 ||
            !qxp_packet_float_array(
                (const ULONG *)&value->diffuse,
                (sizeof(*value) - sizeof(value->type)) / sizeof(ULONG)) ||
            (value->range_bits & 0x80000000UL) ||
            (value->falloff_bits & 0x80000000UL) ||
            (value->attenuation0_bits & 0x80000000UL) ||
            (value->attenuation1_bits & 0x80000000UL) ||
            (value->attenuation2_bits & 0x80000000UL) ||
            (value->theta_bits & 0x80000000UL) ||
            (value->phi_bits & 0x80000000UL)) return FALSE;
    }
    return TRUE;
}

static ULONG qxp_packet_program_format_bytes(ULONG format)
{
    switch (format) {
    case QXP_D3D_VERTEX_FORMAT_FLOAT1: return 4;
    case QXP_D3D_VERTEX_FORMAT_FLOAT2: return 8;
    case QXP_D3D_VERTEX_FORMAT_FLOAT3: return 12;
    case QXP_D3D_VERTEX_FORMAT_FLOAT4: return 16;
    case QXP_D3D_VERTEX_FORMAT_D3DCOLOR: return 4;
    case QXP_D3D_VERTEX_FORMAT_UBYTE4: return 4;
    case QXP_D3D_VERTEX_FORMAT_SHORT2: return 4;
    case QXP_D3D_VERTEX_FORMAT_SHORT4: return 8;
    }
    return 0;
}

static BOOLEAN qxp_packet_program_payload(
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    ULONG payload_bytes, ULONG vertex_count)
{
    ULONG header_bytes = sizeof(*program) - sizeof(program->vertex_data);
    ULONG expected_bytes;
    ULONG input;

    if (!program || payload_bytes < header_bytes ||
        !program->program_name ||
        (program->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE ||
        (program->input_mask & ~0xffffUL) ||
        (program->used_inputs & ~0xffffUL) ||
        (program->used_inputs & ~program->input_mask & 1UL) ||
        (program->written_outputs & ~QXP_D3D_VS_OUTPUT_MASK) ||
        !(program->written_outputs & QXP_D3D_VS_OUTPUT_POSITION) ||
        program->vertex_stride > QXP_D3D_MAX_PROGRAM_VERTEX_STRIDE ||
        program->clipping_enable > 1 ||
        (program->clip_plane_enable &
         ~((1UL << QXP_D3D_MAX_USER_CLIP_PLANES) - 1UL)) ||
        !qxp_packet_float_array(
            (const ULONG *)program->clip_planes,
            QXP_D3D_MAX_USER_CLIP_PLANES * 4UL)) return FALSE;
    if (program->input_mask) {
        if (!program->vertex_stride) return FALSE;
    } else if (program->vertex_stride || program->vertex_data_bytes) {
        return FALSE;
    }
    if (program->vertex_stride &&
        vertex_count > 0xffffffffUL / program->vertex_stride) return FALSE;
    expected_bytes = program->vertex_stride * vertex_count;
    if (program->vertex_data_bytes != expected_bytes ||
        expected_bytes > payload_bytes - header_bytes) return FALSE;

    for (input = 0; input < 16; ++input) {
        const QXP_D3D_PROGRAM_VERTEX_ELEMENT *element =
            &program->elements[input];
        ULONG bytes;

        if (!(program->input_mask & (1UL << input))) {
            if (element->format || element->byte_offset) return FALSE;
            continue;
        }
        bytes = qxp_packet_program_format_bytes(element->format);
        if (!bytes || element->byte_offset > program->vertex_stride ||
            bytes > program->vertex_stride - element->byte_offset)
            return FALSE;
    }
    return TRUE;
}

BOOLEAN qxp_packet_program_texture_contract(
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    ULONG coordinate_stages, ULONG projected_stages)
{
    ULONG stage;

    if (!program || (coordinate_stages & ~0xffUL) ||
        (projected_stages & ~15UL) ||
        (projected_stages & ~coordinate_stages)) return FALSE;
    for (stage = 0; stage < 8; ++stage) {
        if ((coordinate_stages & (1UL << stage)) &&
            !(program->written_outputs &
              (QXP_D3D_VS_OUTPUT_TEXCOORD0 << stage))) return FALSE;
    }
    return TRUE;
}

ULONG qxp_packet_mip_dimension(ULONG base, ULONG level)
{
    while (level-- && base > 1) base >>= 1;
    return base;
}

BOOLEAN qxp_packet_texture_define_contract(
    const QXP_D3D_TEXTURE_DEFINE_REQUEST *request)
{
    if (!request || request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->texture_name ||
        request->texture_name == QXP_D3D_NEUTRAL_TEXTURE_NAME ||
        request->texture_name == QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME ||
        request->texture_name == QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME ||
        request->texture_name == QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME ||
        !request->width || !request->height ||
        request->width > QXP_PACKET_TEXTURE_MAX_DIMENSION ||
        request->height > QXP_PACKET_TEXTURE_MAX_DIMENSION ||
        !request->total_levels ||
        request->total_levels > QXP_D3D_MAX_TEXTURE_LEVELS ||
        request->level >= request->total_levels) return FALSE;
    switch (request->texture_type) {
    case QXP_D3D_TEXTURE_2D:
        if (request->face || request->depth != 1) return FALSE;
        break;
    case QXP_D3D_TEXTURE_CUBE:
        if (request->face >= 6 || request->depth != 1 ||
            request->width != request->height) return FALSE;
        break;
    case QXP_D3D_TEXTURE_VOLUME:
        if (request->face || !request->depth ||
            request->depth > QXP_PACKET_TEXTURE_MAX_DIMENSION) return FALSE;
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

BOOLEAN qxp_packet_texture_allocation_contract(
    const QXP_D3D_TEXTURE_DEFINE_REQUEST *request,
    ULONG bytes_per_pixel)
{
    ULONG row_bytes;
    ULONG slice_bytes;

    if (!request || !bytes_per_pixel ||
        !qxp_packet_texture_define_contract(request) ||
        request->width >
            QXP_PACKET_TEXTURE_MAX_ALLOCATION_BYTES / bytes_per_pixel)
        return FALSE;
    row_bytes = request->width * bytes_per_pixel;
    if (request->height >
            QXP_PACKET_TEXTURE_MAX_ALLOCATION_BYTES / row_bytes)
        return FALSE;
    slice_bytes = row_bytes * request->height;
    if (request->depth >
            QXP_PACKET_TEXTURE_MAX_ALLOCATION_BYTES / slice_bytes)
        return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_texture_upload_contract(
    const QXP_D3D_TEXTURE_UPLOAD_REQUEST *request, ULONG request_bytes,
    ULONG base_width, ULONG base_height, ULONG base_depth,
    ULONG level_count, ULONG bytes_per_pixel)
{
    ULONG header_bytes =
        (ULONG)offsetof(QXP_D3D_TEXTURE_UPLOAD_REQUEST, data);
    ULONG level_width;
    ULONG level_height;
    ULONG level_depth;
    ULONG expected_bytes;

    if (!request || request_bytes < header_bytes ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->texture_name ||
        !bytes_per_pixel || !level_count ||
        request->level >= level_count) return FALSE;
    switch (request->texture_type) {
    case QXP_D3D_TEXTURE_2D:
        if (request->face || request->z || request->depth != 1) return FALSE;
        break;
    case QXP_D3D_TEXTURE_CUBE:
        if (request->face >= 6 || request->z || request->depth != 1)
            return FALSE;
        break;
    case QXP_D3D_TEXTURE_VOLUME:
        if (request->face) return FALSE;
        break;
    default:
        return FALSE;
    }
    if (!request->width || !request->height || !request->depth)
        return FALSE;
    level_width = qxp_packet_mip_dimension(base_width, request->level);
    level_height = qxp_packet_mip_dimension(base_height, request->level);
    level_depth = qxp_packet_mip_dimension(base_depth, request->level);
    if (request->x > level_width ||
        request->width > level_width - request->x ||
        request->y > level_height ||
        request->height > level_height - request->y ||
        request->z > level_depth ||
        request->depth > level_depth - request->z) return FALSE;
    if (request->width > 0xffffffffUL / bytes_per_pixel) return FALSE;
    expected_bytes = request->width * bytes_per_pixel;
    if (request->height > 0xffffffffUL / expected_bytes) return FALSE;
    expected_bytes *= request->height;
    if (request->depth > 0xffffffffUL / expected_bytes) return FALSE;
    expected_bytes *= request->depth;
    if (request->data_bytes != expected_bytes ||
        request->data_bytes > QXP_D3D_MAX_TEXTURE_UPLOAD_BYTES ||
        request->data_bytes != request_bytes - header_bytes) return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_draw_mask_contract(
    ULONG sampled_stages, ULONG coordinate_stages,
    ULONG cube_stages, ULONG volume_stages)
{
    if ((sampled_stages & ~15UL) || (coordinate_stages & ~15UL) ||
        (cube_stages & ~15UL) || (volume_stages & ~15UL) ||
        (cube_stages & volume_stages) ||
        (cube_stages & ~sampled_stages) ||
        (volume_stages & ~sampled_stages) ||
        (sampled_stages & ~coordinate_stages)) return FALSE;
    return TRUE;
}
static BOOLEAN qxp_packet_sm2_sampler_zero(
    const QXP_D3D_SM2_SAMPLER_STATE *state)
{
    return state && !state->texture_name && !state->texture_type &&
           !state->address_u && !state->address_v &&
           !state->mag_filter && !state->min_filter &&
           !state->mip_filter && !state->mip_lod_bias_bits &&
           !state->max_mip_level;
}

static BOOLEAN qxp_packet_sm2_sampler_contract(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_SM2_DRAW_STATE *draw_state)
{
    const QXP_D3D_SM2_SAMPLER_STATE *state;
    ULONG sampler;
    ULONG bit;
    ULONG expected_type;

    if (!request || !draw_state) return FALSE;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT; ++sampler) {
        state = &draw_state->samplers[sampler];
        bit = 1UL << sampler;
        if (!(request->pixel_sampled_stages & bit)) {
            if (!qxp_packet_sm2_sampler_zero(state)) return FALSE;
            continue;
        }
        /* DirectX permits a programmable sample from an unbound resource;
         * the fully zeroed record is the unbound form and resolves to the
         * miniport's opaque-black dummy of the mask-selected target. */
        if (!state->texture_name) {
            if (!qxp_packet_sm2_sampler_zero(state)) return FALSE;
            continue;
        }
        expected_type =
            (request->pixel_cube_stages & bit) ? QXP_D3D_TEXTURE_CUBE :
            (request->pixel_volume_stages & bit) ?
                QXP_D3D_TEXTURE_VOLUME : QXP_D3D_TEXTURE_2D;
        if (state->texture_type != expected_type ||
            state->address_u < 1 || state->address_u > 3 ||
            state->address_v < 1 || state->address_v > 3 ||
            (state->mag_filter != 1 && state->mag_filter != 2) ||
            (state->min_filter != 1 && state->min_filter != 2) ||
            state->mip_filter > 2 ||
            !qxp_packet_float_finite(state->mip_lod_bias_bits))
            return FALSE;
    }
    return TRUE;
}


BOOLEAN qxp_packet_texcoord_plan(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_DRAW_PACKET_VIEW *view,
    ULONG coordinate_stages, QXP_D3D_TEXCOORD_PLAN *plan)
{
    ULONG raw[4];
    ULONG texture_count;
    ULONG stage;
    ULONG mode;
    ULONG selector;

    if (!request || !view || !plan || (coordinate_stages & ~15UL))
        return FALSE;
    plan->passthrough_stages = 0;
    plan->generated_stages = 0;
    for (stage = 0; stage < 4; ++stage) {
        plan->modes[stage] = 0;
        plan->selectors[stage] = 0;
    }
    raw[0] = request->texture_coord_index;
    raw[1] = request->texture1_coord_index;
    raw[2] = request->texture2_coord_index;
    raw[3] = request->texture3_coord_index;
    if (view->program) {
        for (stage = 0; stage < 4; ++stage) {
            if (raw[stage] != stage) return FALSE;
            plan->selectors[stage] = stage;
        }
        plan->passthrough_stages = coordinate_stages;
        return TRUE;
    }
    texture_count =
        (request->fvf & QXP_PACKET_FVF_TEXCOUNT_MASK) >> 8;
    for (stage = 0; stage < 4; ++stage) {
        if (!(coordinate_stages & (1UL << stage))) continue;
        mode = raw[stage] & QXP_D3D_TCI_MODE_MASK;
        selector = raw[stage] & QXP_D3D_TCI_INDEX_MASK;
        plan->modes[stage] = mode;
        plan->selectors[stage] = selector;
        if (mode == QXP_D3D_TCI_PASSTHRU) {
            if (selector >= texture_count) return FALSE;
            plan->passthrough_stages |= 1UL << stage;
            continue;
        }
        if ((mode != QXP_D3D_TCI_CAMERASPACENORMAL &&
             mode != QXP_D3D_TCI_CAMERASPACEPOSITION &&
             mode != QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR) ||
            selector >= 8 || !view->object_data ||
            (request->vertex_mode != QXP_D3D_VERTEX_OBJECT &&
             request->vertex_mode != QXP_D3D_VERTEX_INDEXED_OBJECT) ||
            view->compact || view->compact_indexed ||
            ((mode == QXP_D3D_TCI_CAMERASPACENORMAL ||
              mode == QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR) &&
             !(request->fvf & QXP_PACKET_FVF_NORMAL))) return FALSE;
        plan->generated_stages |= 1UL << stage;
    }
    return TRUE;
}

static BOOLEAN qxp_packet_fixed_material_sources_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    ULONG sources[4];
    ULONG mask = 0;
    ULONG index;

    if (!state->color_vertex_enable) return TRUE;
    sources[0] = state->diffuse_material_source;
    sources[1] = request->specular_enable ?
                 state->specular_material_source : 0;
    sources[2] = state->ambient_material_source;
    sources[3] = state->emissive_material_source;
    for (index = 0; index < 4; ++index) {
        if (sources[index] == QXP_PACKET_D3DMCS_COLOR2) return FALSE;
        if (sources[index] == QXP_PACKET_D3DMCS_COLOR1)
            mask |= 1UL << index;
    }
    return (BOOLEAN)(!mask || mask == 1 || mask == 2 || mask == 4 ||
                     mask == 5 || mask == 8);
}

BOOLEAN qxp_packet_object_glsl_required(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_DRAW_PACKET_VIEW *view,
    const QXP_D3D_TEXCOORD_PLAN *plan, BOOLEAN internal_program)
{
    if (!request || !view || !plan ||
        (!view->object_data && !view->raw_indexed_object) ||
        !view->hwtl_state || view->program || view->compact ||
        view->compact_indexed) return FALSE;
    if (plan->generated_stages && internal_program) return TRUE;
    return (BOOLEAN)(view->hwtl_state->lighting_enable &&
                     (!(request->fvf & QXP_PACKET_FVF_NORMAL) ||
                      !qxp_packet_fixed_material_sources_supported(
                          request, view->hwtl_state)));
}

BOOLEAN qxp_packet_texture_binding_contract(
    ULONG texture_type, BOOLEAN programmable_sample,
    BOOLEAN programmable_cube, BOOLEAN programmable_volume)
{
    if (programmable_cube && programmable_volume) return FALSE;
    /* DirectX permits a shader sample from an unbound resource. The miniport
     * resolves type zero to its complete opaque-black dummy of the target
     * selected by the cube/volume masks. */
    if (programmable_sample && !texture_type) return TRUE;
    if (!programmable_sample) {
        return (BOOLEAN)(texture_type == QXP_D3D_TEXTURE_2D ||
                         texture_type == QXP_D3D_TEXTURE_CUBE);
    }
    if (programmable_volume)
        return (BOOLEAN)(texture_type == QXP_D3D_TEXTURE_VOLUME);
    if (programmable_cube)
        return (BOOLEAN)(texture_type == QXP_D3D_TEXTURE_CUBE);
    return (BOOLEAN)(texture_type == QXP_D3D_TEXTURE_2D);
}

ULONG qxp_packet_target_depth_plan(
    const QXP_D3D_TARGET_REQUEST *request, ULONG existing_width,
    ULONG existing_height, ULONG existing_format)
{
    if (!request || !request->depth_name || !request->depth_width ||
        !request->depth_height ||
        request->depth_width < request->width ||
        request->depth_height < request->height)
        return QXP_PACKET_TARGET_DEPTH_REJECT;
    if (existing_width != request->depth_width ||
        existing_height != request->depth_height)
        return QXP_PACKET_TARGET_DEPTH_RECREATE;
    if (existing_width < request->width ||
        existing_height < request->height ||
        existing_format != request->depth_format)
        return QXP_PACKET_TARGET_DEPTH_REJECT;
    return QXP_PACKET_TARGET_DEPTH_REUSE;
}

static BOOLEAN qxp_packet_surface_copy_format(ULONG format)
{
    return format == QXP_D3D_COPY_FORMAT_R5G6B5 ||
           format == QXP_D3D_COPY_FORMAT_A8R8G8B8 ||
           format == QXP_D3D_COPY_FORMAT_X8R8G8B8;
}

static BOOLEAN qxp_packet_surface_copy_rect(
    const QXP_D3D_RECT *rect, ULONG width, ULONG height)
{
    if (!rect || rect->left < 0 || rect->top < 0 ||
        rect->right <= rect->left || rect->bottom <= rect->top)
        return FALSE;
    return (ULONG)rect->right <= width &&
           (ULONG)rect->bottom <= height;
}

BOOLEAN qxp_packet_surface_copy_contract(
    const QXP_D3D_SURFACE_COPY_REQUEST *request)
{
    BOOLEAN formats_compatible;

    if (!request || request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->source_texture_name ||
        !request->destination_texture_name ||
        request->source_texture_name == request->destination_texture_name ||
        !request->source_width || !request->source_height ||
        !request->destination_width || !request->destination_height ||
        request->source_width > 2048 || request->source_height > 2048 ||
        request->destination_width > 2048 ||
        request->destination_height > 2048 ||
        request->filter != QXP_D3D_SURFACE_COPY_POINT ||
        !qxp_packet_surface_copy_format(request->source_format) ||
        !qxp_packet_surface_copy_format(request->destination_format) ||
        !qxp_packet_surface_copy_rect(
            &request->source_rect, request->source_width,
            request->source_height) ||
        !qxp_packet_surface_copy_rect(
            &request->destination_rect, request->destination_width,
            request->destination_height)) return FALSE;
    formats_compatible = request->source_format ==
                         request->destination_format;
    if ((request->source_format == QXP_D3D_COPY_FORMAT_X8R8G8B8 &&
         request->destination_format == QXP_D3D_COPY_FORMAT_A8R8G8B8) ||
        (request->source_format == QXP_D3D_COPY_FORMAT_A8R8G8B8 &&
         request->destination_format == QXP_D3D_COPY_FORMAT_X8R8G8B8)) {
        formats_compatible = TRUE;
    }
    return formats_compatible;
}

BOOLEAN qxp_packet_sm2_program_define_contract(
    const QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *request,
    ULONG request_bytes)
{
    ULONG header_bytes =
        (ULONG)offsetof(QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST, source);
    ULONG source_bytes;

    if (!request || request_bytes < header_bytes ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->program_name ||
        (request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE ||
        !request->vertex_source_bytes || !request->pixel_source_bytes ||
        (request->sampler_mask & ~0xffffUL) ||
        (request->cube_sampler_mask & ~request->sampler_mask) ||
        (request->volume_sampler_mask & ~request->sampler_mask) ||
        (request->cube_sampler_mask & request->volume_sampler_mask))
        return FALSE;
    if (request->vertex_source_bytes >
        0xffffffffUL - request->pixel_source_bytes) return FALSE;
    if (request->vertex_source_bytes >
            QXP_D3D_SM2_MAX_VERTEX_SOURCE_BYTES ||
        request->pixel_source_bytes >
            QXP_D3D_SM2_MAX_PIXEL_SOURCE_BYTES) return FALSE;
    source_bytes = request->vertex_source_bytes +
                   request->pixel_source_bytes;
    if (source_bytes != request_bytes - header_bytes ||
        request->source[request->vertex_source_bytes - 1] != 0 ||
        request->source[source_bytes - 1] != 0) return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_sm2_program_delete_contract(
    const QXP_D3D_SM2_PROGRAM_DELETE_REQUEST *request,
    ULONG request_bytes)
{
    if (!request || request_bytes != sizeof(*request) ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->program_name ||
        (request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE) return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_sm2_constants_contract(
    const QXP_D3D_SM2_CONSTANTS_REQUEST *request,
    ULONG request_bytes, ULONG owner_process_id,
    ULONG owner_program_name)
{
    ULONG header_bytes =
        (ULONG)offsetof(QXP_D3D_SM2_CONSTANTS_REQUEST, data);
    ULONG register_count;
    ULONG data_bytes;

    if (!request || request_bytes < header_bytes ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        !owner_process_id || !owner_program_name ||
        request->process_id != owner_process_id ||
        request->program_name != owner_program_name ||
        (request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE ||
        (!request->vertex_register_count &&
         !request->pixel_register_count) ||
        request->vertex_start_register >
            QXP_D3D_SM2_VERTEX_CONSTANT_COUNT ||
        request->vertex_register_count >
            QXP_D3D_SM2_VERTEX_CONSTANT_COUNT -
                request->vertex_start_register ||
        request->pixel_start_register >
            QXP_D3D_SM2_PIXEL_CONSTANT_COUNT ||
        request->pixel_register_count >
            QXP_D3D_SM2_PIXEL_CONSTANT_COUNT -
                request->pixel_start_register) return FALSE;
    register_count = request->vertex_register_count +
                     request->pixel_register_count;
    if (register_count > 0xffffffffUL / (4UL * sizeof(ULONG)))
        return FALSE;
    data_bytes = register_count * 4UL * sizeof(ULONG);
    return data_bytes == request_bytes - header_bytes;
}

BOOLEAN qxp_packet_surface_readback_contract(
    const QXP_D3D_SURFACE_READBACK_REQUEST *request)
{
    ULONG bytes_per_pixel;
    ULONG row_bytes;

    if (!request || request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->texture_name ||
        !request->width || !request->height ||
        request->width > 2048 || request->height > 2048 ||
        !qxp_packet_surface_copy_format(request->format)) return FALSE;
    bytes_per_pixel =
        request->format == QXP_D3D_COPY_FORMAT_R5G6B5 ? 2UL : 4UL;
    row_bytes = request->width * bytes_per_pixel;
    /* Pitch is bounded so height * pitch cannot overflow 32 bits: the
     * device-specific aperture bound is the miniport's responsibility. */
    if (request->pitch < row_bytes || request->pitch > 0x00100000UL)
        return FALSE;
    if (request->vidmem_offset >
        0xffffffffUL - request->height * request->pitch) return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_present_contract(
    const QXP_D3D_PRESENT_REQUEST *request)
{
    ULONG bytes_per_pixel;
    ULONG row_bytes;

    if (!request || request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id ||
        (request->flags & ~(QXP_D3D_PRESENT_CPU_SOURCE |
                            QXP_D3D_PRESENT_GPU_TEXTURE))) return FALSE;
    if ((request->flags & QXP_D3D_PRESENT_CPU_SOURCE) &&
        (request->flags & QXP_D3D_PRESENT_GPU_TEXTURE)) return FALSE;
    if (request->flags & QXP_D3D_PRESENT_GPU_TEXTURE) {
        /* A texture present carries only the texture name. */
        if (!request->texture_name ||
            request->texture_name == QXP_D3D_NEUTRAL_TEXTURE_NAME ||
            request->texture_name ==
                QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME ||
            request->texture_name ==
                QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME ||
            request->texture_name ==
                QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME ||
            request->width || request->height || request->format ||
            request->vidmem_offset || request->pitch) return FALSE;
        return TRUE;
    }
    if (!(request->flags & QXP_D3D_PRESENT_CPU_SOURCE)) {
        /* Legacy render-target present: strictly empty payload. */
        return (BOOLEAN)(!request->width && !request->height &&
                         !request->format && !request->vidmem_offset &&
                         !request->pitch && !request->texture_name);
    }
    if (request->texture_name) return FALSE;
    if (!request->width || !request->height ||
        request->width > 2048 || request->height > 2048 ||
        !qxp_packet_surface_copy_format(request->format)) return FALSE;
    bytes_per_pixel =
        request->format == QXP_D3D_COPY_FORMAT_R5G6B5 ? 2UL : 4UL;
    row_bytes = request->width * bytes_per_pixel;
    if (request->pitch < row_bytes || request->pitch > 0x00100000UL)
        return FALSE;
    if (request->vidmem_offset >
        0xffffffffUL - request->height * request->pitch) return FALSE;
    return TRUE;
}

BOOLEAN qxp_packet_compact_fvf_layout(
    ULONG fvf, QXP_D3D_COMPACT_FVF_LAYOUT *layout)
{
    ULONG allowed = QXP_PACKET_FVF_POSITION_MASK |
                    QXP_PACKET_FVF_DIFFUSE | QXP_PACKET_FVF_SPECULAR |
                    QXP_PACKET_FVF_TEXCOUNT_MASK |
                    QXP_PACKET_FVF_TEXTURE_FORMAT_MASK;
    ULONG offset = 16;
    ULONG texture;
    ULONG format;
    ULONG dimension;

    if (!layout || (fvf & ~allowed) ||
        (fvf & QXP_PACKET_FVF_POSITION_MASK) != QXP_PACKET_FVF_XYZRHW)
        return FALSE;
    layout->vertex_stride = 0;
    layout->texture_count = 0;
    layout->diffuse_offset = 0;
    layout->specular_offset = 0;
    for (texture = 0; texture < 4; ++texture)
        layout->texture_offsets[texture] = 0;
    layout->texture_count =
        (fvf & QXP_PACKET_FVF_TEXCOUNT_MASK) >> 8;
    if (layout->texture_count > 4) return FALSE;
    if (fvf & QXP_PACKET_FVF_DIFFUSE) {
        layout->diffuse_offset = offset;
        offset += 4;
    }
    if (fvf & QXP_PACKET_FVF_SPECULAR) {
        layout->specular_offset = offset;
        offset += 4;
    }
    for (texture = 0; texture < 4; ++texture) {
        format = (fvf >> (16 + texture * 2)) & 3UL;
        if (texture >= layout->texture_count) {
            if (format) return FALSE;
            continue;
        }
        dimension = format == 3 ? 1 : (format == 1 ? 3 :
                    (format == 2 ? 4 : 2));
        /* ABI 44's bounded shader is deliberately limited to ordinary 2D
         * transformed coordinates. Other legal FVFs remain on the canonical
         * path until their homogeneous/projected equations are validated. */
        if (dimension != 2) return FALSE;
        layout->texture_offsets[texture] = offset;
        offset += dimension * sizeof(ULONG);
    }
    if (offset > QXP_D3D_MAX_VERTEX_STRIDE) return FALSE;
    layout->vertex_stride = offset;
    return TRUE;
}

BOOLEAN qxp_packet_raw_indexed_object_fvf_layout(
    ULONG fvf, QXP_D3D_RAW_OBJECT_FVF_LAYOUT *layout)
{
    ULONG allowed = QXP_PACKET_FVF_POSITION_MASK |
                    QXP_PACKET_FVF_NORMAL |
                    QXP_PACKET_FVF_DIFFUSE |
                    QXP_PACKET_FVF_TEXCOUNT_MASK |
                    QXP_PACKET_FVF_TEXTURE_FORMAT_MASK;
    ULONG offset = 12;

    if (!layout || (fvf & ~allowed) ||
        (fvf & QXP_PACKET_FVF_POSITION_MASK) != QXP_PACKET_FVF_XYZ ||
        (fvf & QXP_PACKET_FVF_TEXCOUNT_MASK) != 0x100UL ||
        (fvf & QXP_PACKET_FVF_TEXTURE_FORMAT_MASK))
        return FALSE;
    layout->normal_offset = 0xffffffffUL;
    if (fvf & QXP_PACKET_FVF_NORMAL) {
        layout->normal_offset = offset;
        offset += 3UL * sizeof(ULONG);
    }
    layout->diffuse_offset = 0xffffffffUL;
    if (fvf & QXP_PACKET_FVF_DIFFUSE) {
        layout->diffuse_offset = offset;
        offset += sizeof(ULONG);
    }
    layout->texture_offset = offset;
    offset += 2UL * sizeof(ULONG);
    if (offset > QXP_D3D_MAX_VERTEX_STRIDE) return FALSE;
    layout->vertex_stride = offset;
    return TRUE;
}

static BOOLEAN qxp_packet_compact_payload(
    const QXP_D3D_DRAW_REQUEST *request, ULONG payload_bytes,
    QXP_D3D_DRAW_PACKET_VIEW *view)
{
    const QXP_D3D_COMPACT_VERTEX_PAYLOAD *compact =
        (const QXP_D3D_COMPACT_VERTEX_PAYLOAD *)request->vertices;
    ULONG header_bytes = sizeof(*compact) - sizeof(compact->vertex_data);
    QXP_D3D_COMPACT_FVF_LAYOUT layout;
    ULONG expected_bytes;
    ULONG vertex;
    ULONG texture;
    const ULONG *record;

    if (!request || !view || payload_bytes < header_bytes ||
        request->texture_projected_stages || request->object_data_offset ||
        request->hwtl_state_offset ||
        !qxp_packet_compact_fvf_layout(request->fvf, &layout) ||
        compact->vertex_stride != layout.vertex_stride ||
        request->vertex_count > 0xffffffffUL / layout.vertex_stride)
        return FALSE;
    expected_bytes = request->vertex_count * layout.vertex_stride;
    if (compact->vertex_data_bytes != expected_bytes ||
        expected_bytes > payload_bytes - header_bytes) return FALSE;
    for (texture = 0; texture < 4; ++texture) {
        if (request->texture_coord_dimensions[texture] !=
            (texture < layout.texture_count ? 2UL : 0UL)) return FALSE;
    }
    for (vertex = 0; vertex < request->vertex_count; ++vertex) {
        record = (const ULONG *)((const UCHAR *)compact->vertex_data +
                                 vertex * layout.vertex_stride);
        if (!qxp_packet_float_array(record, 4)) return FALSE;
        for (texture = 0; texture < layout.texture_count; ++texture) {
            if (!qxp_packet_float_array(
                    (const ULONG *)((const UCHAR *)record +
                                     layout.texture_offsets[texture]),
                    2)) return FALSE;
        }
    }
    view->compact = compact;
    view->compact_vertex_data = (const UCHAR *)compact->vertex_data;
    return TRUE;
}

static BOOLEAN qxp_packet_compact_indexed_payload(
    const QXP_D3D_DRAW_REQUEST *request, ULONG payload_bytes,
    QXP_D3D_DRAW_PACKET_VIEW *view)
{
    const QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *compact =
        (const QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)request->vertices;
    ULONG header_bytes = sizeof(*compact) - sizeof(compact->data);
    QXP_D3D_COMPACT_FVF_LAYOUT layout;
    const UCHAR *vertex_data;
    const UCHAR *index_data;
    const ULONG *record;
    ULONG expected_vertex_bytes;
    ULONG aligned_index_bytes;
    ULONG vertex;
    ULONG texture;
    ULONG index;
    UCHAR vertex_used[QXP_D3D_MAX_DRAW_VERTICES];

    if (!request || !view || payload_bytes < header_bytes ||
        request->texture_projected_stages || request->object_data_offset ||
        request->hwtl_state_offset ||
        !qxp_packet_compact_fvf_layout(request->fvf, &layout) ||
        compact->vertex_stride != layout.vertex_stride ||
        !compact->vertex_count ||
        compact->vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        compact->vertex_count > request->vertex_count ||
        compact->index_count != request->vertex_count ||
        compact->index_data_bytes != compact->index_count ||
        compact->vertex_count >
            0xffffffffUL / layout.vertex_stride)
        return FALSE;
    expected_vertex_bytes = compact->vertex_count * layout.vertex_stride;
    aligned_index_bytes =
        (compact->index_data_bytes +
            QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    if (compact->vertex_data_bytes != expected_vertex_bytes ||
        aligned_index_bytes > payload_bytes - header_bytes ||
        expected_vertex_bytes >
            payload_bytes - header_bytes - aligned_index_bytes)
        return FALSE;
    for (texture = 0; texture < 4; ++texture) {
        if (request->texture_coord_dimensions[texture] !=
            (texture < layout.texture_count ? 2UL : 0UL)) return FALSE;
    }
    index_data = (const UCHAR *)compact->data;
    vertex_data = index_data + aligned_index_bytes;
    for (index = 0; index < compact->vertex_count; ++index)
        vertex_used[index] = 0;
    for (index = 0; index < compact->index_count; ++index) {
        if (index_data[index] >= compact->vertex_count) return FALSE;
        vertex_used[index_data[index]] = 1;
    }
    for (; index < aligned_index_bytes; ++index) {
        if (index_data[index]) return FALSE;
    }
    for (index = 0; index < compact->vertex_count; ++index) {
        if (!vertex_used[index]) return FALSE;
    }
    for (vertex = 0; vertex < compact->vertex_count; ++vertex) {
        record = (const ULONG *)(vertex_data +
                                  vertex * layout.vertex_stride);
        if (!qxp_packet_float_array(record, 4)) return FALSE;
        for (texture = 0; texture < layout.texture_count; ++texture) {
            if (!qxp_packet_float_array(
                    (const ULONG *)((const UCHAR *)record +
                                     layout.texture_offsets[texture]),
                    2)) return FALSE;
        }
    }
    view->compact_indexed = compact;
    view->compact_indexed_vertex_data = vertex_data;
    view->compact_indexed_index_data = index_data;
    return TRUE;
}

static BOOLEAN qxp_packet_raw_indexed_object_payload(
    const QXP_D3D_DRAW_REQUEST *request, ULONG request_bytes,
    QXP_D3D_DRAW_PACKET_VIEW *view)
{
    const QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *raw;
    const UCHAR *index_data;
    const UCHAR *vertex_data;
    const ULONG *record;
    QXP_D3D_RAW_OBJECT_FVF_LAYOUT layout;
    ULONG draw_header =
        sizeof(QXP_D3D_DRAW_REQUEST) -
        sizeof(((QXP_D3D_DRAW_REQUEST *)0)->vertices);
    ULONG payload_header =
        sizeof(QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD) -
        sizeof(((QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *)0)->data);
    ULONG aligned_index_bytes;
    ULONG expected_vertex_bytes;
    ULONG required;
    ULONG index;
    ULONG vertex;
    UCHAR vertex_used[QXP_D3D_MAX_DRAW_VERTICES];

    if (!request || !view || request_bytes < draw_header + payload_header ||
        request->vertex_mode != QXP_D3D_VERTEX_RAW_INDEXED_OBJECT ||
        request->texture_projected_stages || request->object_data_offset ||
        request->texture_coord_dimensions[0] != 2 ||
        request->texture_coord_dimensions[1] ||
        request->texture_coord_dimensions[2] ||
        request->texture_coord_dimensions[3] ||
        request->texture_coord_index != 0 ||
        request->texture1_coord_index != 1 ||
        request->texture2_coord_index != 2 ||
        request->texture3_coord_index != 3 ||
        request->pixel_program_name ||
        request->pixel_coordinate_stages ||
        request->pixel_cube_stages || request->pixel_volume_stages ||
        request->fog_enable ||
        !qxp_packet_raw_indexed_object_fvf_layout(
            request->fvf, &layout))
        return FALSE;
    raw = (const QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *)request->vertices;
    if (raw->vertex_stride != layout.vertex_stride ||
        !raw->vertex_count ||
        raw->vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        raw->vertex_count > request->vertex_count ||
        raw->index_count != request->vertex_count ||
        raw->index_data_bytes != raw->index_count ||
        raw->vertex_count > 0xffffffffUL / layout.vertex_stride)
        return FALSE;
    expected_vertex_bytes = raw->vertex_count * layout.vertex_stride;
    aligned_index_bytes =
        (raw->index_data_bytes +
         QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    if (raw->vertex_data_bytes != expected_vertex_bytes ||
        draw_header > 0xffffffffUL - payload_header ||
        aligned_index_bytes >
            0xffffffffUL - draw_header - payload_header ||
        expected_vertex_bytes >
            0xffffffffUL - draw_header - payload_header -
                aligned_index_bytes)
        return FALSE;
    required = draw_header + payload_header + aligned_index_bytes +
               expected_vertex_bytes;
    if (request->hwtl_state_offset != required ||
        required > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE) ||
        request_bytes != required + sizeof(QXP_D3D_HWTL_STATE))
        return FALSE;
    view->hwtl_state = (const QXP_D3D_HWTL_STATE *)
        ((const UCHAR *)request + request->hwtl_state_offset);
    if (!qxp_packet_hwtl_state(view->hwtl_state) ||
        view->hwtl_state->lighting_enable ||
        view->hwtl_state->fog_vertex_mode ||
        view->hwtl_state->clip_plane_enable ||
        view->hwtl_state->range_fog_enable)
        return FALSE;
    index_data = (const UCHAR *)raw->data;
    vertex_data = index_data + aligned_index_bytes;
    for (index = 0; index < raw->vertex_count; ++index)
        vertex_used[index] = 0;
    for (index = 0; index < raw->index_count; ++index) {
        if (index_data[index] >= raw->vertex_count) return FALSE;
        vertex_used[index_data[index]] = 1;
    }
    for (; index < aligned_index_bytes; ++index) {
        if (index_data[index]) return FALSE;
    }
    for (index = 0; index < raw->vertex_count; ++index) {
        if (!vertex_used[index]) return FALSE;
    }
    for (vertex = 0; vertex < raw->vertex_count; ++vertex) {
        record = (const ULONG *)(vertex_data +
                                  vertex * layout.vertex_stride);
        if (!qxp_packet_float_array(record, 3) ||
            ((request->fvf & QXP_PACKET_FVF_NORMAL) &&
             !qxp_packet_float_array(
                 (const ULONG *)((const UCHAR *)record +
                                  layout.normal_offset), 3)) ||
            !qxp_packet_float_array(
                (const ULONG *)((const UCHAR *)record +
                                 layout.texture_offset), 2))
            return FALSE;
    }
    view->raw_indexed_object = raw;
    view->raw_indexed_object_index_data = index_data;
    view->raw_indexed_object_vertex_data = vertex_data;
    view->vertices = NULL;
    view->vertex_indices = NULL;
    view->source_vertex_count = raw->vertex_count;
    view->failure_index = 0xffffffffUL;
    view->failure_detail = 0;
    return TRUE;
}

static BOOLEAN qxp_packet_indexed_payload(
    const QXP_D3D_DRAW_REQUEST *request, ULONG request_bytes,
    QXP_D3D_DRAW_PACKET_VIEW *view)
{
    const QXP_D3D_INDEXED_VERTEX_PAYLOAD *indexed;
    const UCHAR *index_data;
    const UCHAR *vertex_data;
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data = NULL;
    const QXP_D3D_HWTL_STATE *hwtl_state = NULL;
    ULONG draw_header =
        sizeof(QXP_D3D_DRAW_REQUEST) -
        sizeof(((QXP_D3D_DRAW_REQUEST *)0)->vertices);
    ULONG payload_header =
        sizeof(QXP_D3D_INDEXED_VERTEX_PAYLOAD) -
        sizeof(((QXP_D3D_INDEXED_VERTEX_PAYLOAD *)0)->data);
    ULONG aligned_index_bytes;
    ULONG vertex_bytes;
    ULONG required;
    ULONG index;
    UCHAR vertex_used[QXP_D3D_MAX_DRAW_VERTICES];
    BOOLEAN object_mode;

    if (!request || request_bytes < draw_header + payload_header)
        return FALSE;
    indexed = (const QXP_D3D_INDEXED_VERTEX_PAYLOAD *)request->vertices;
    object_mode =
        request->vertex_mode == QXP_D3D_VERTEX_INDEXED_OBJECT;
    if ((!object_mode &&
         request->vertex_mode != QXP_D3D_VERTEX_INDEXED_TRANSFORMED) ||
        !indexed->vertex_count || indexed->vertex_count > 0xffUL ||
        indexed->vertex_count > request->vertex_count ||
        indexed->index_count != request->vertex_count ||
        indexed->vertex_count >
            0xffffffffUL / sizeof(QXP_D3D_VERTEX) ||
        indexed->index_count >
            0xffffffffUL - (QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL))
        return FALSE;
    vertex_bytes = indexed->vertex_count * sizeof(QXP_D3D_VERTEX);
    aligned_index_bytes =
        (indexed->index_count +
         QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    if (draw_header > 0xffffffffUL - payload_header ||
        aligned_index_bytes >
            0xffffffffUL - draw_header - payload_header ||
        vertex_bytes >
            0xffffffffUL - draw_header - payload_header -
            aligned_index_bytes)
        return FALSE;
    required = draw_header + payload_header +
               aligned_index_bytes + vertex_bytes;
    index_data = (const UCHAR *)indexed->data;
    vertex_data = index_data + aligned_index_bytes;
    if (object_mode) {
        if (request->object_data_offset != required ||
            indexed->vertex_count >
                (0xffffffffUL - required) /
                sizeof(QXP_D3D_OBJECT_VERTEX_DATA))
            return FALSE;
        object_data = (const QXP_D3D_OBJECT_VERTEX_DATA *)
            ((const UCHAR *)request + required);
        required += indexed->vertex_count *
                    sizeof(QXP_D3D_OBJECT_VERTEX_DATA);
        if (request->hwtl_state_offset != required ||
            required > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE))
            return FALSE;
        hwtl_state = (const QXP_D3D_HWTL_STATE *)
            ((const UCHAR *)request + required);
        required += sizeof(QXP_D3D_HWTL_STATE);
        if (!qxp_packet_hwtl_state(hwtl_state)) return FALSE;
    } else if (request->object_data_offset ||
               request->hwtl_state_offset) {
        return FALSE;
    }
    view->failure_detail = required;
    if (request_bytes != required) return FALSE;
    for (index = 0; index < indexed->vertex_count; ++index)
        vertex_used[index] = 0;
    for (index = 0; index < indexed->index_count; ++index) {
        if (index_data[index] >= indexed->vertex_count) {
            view->failure_index = index;
            view->failure_detail = index_data[index];
            return FALSE;
        }
        vertex_used[index_data[index]] = 1;
    }
    for (; index < aligned_index_bytes; ++index) {
        if (index_data[index]) {
            view->failure_index = index;
            view->failure_detail = index_data[index];
            return FALSE;
        }
    }
    for (index = 0; index < indexed->vertex_count; ++index) {
        if (!vertex_used[index]) {
            view->failure_index = index;
            return FALSE;
        }
    }
    view->vertices = (const QXP_D3D_VERTEX *)vertex_data;
    view->vertex_indices = index_data;
    view->source_vertex_count = indexed->vertex_count;
    view->object_data = object_data;
    view->hwtl_state = hwtl_state;
    view->failure_index = 0xffffffffUL;
    view->failure_detail = 0;
    return TRUE;
}

BOOLEAN qxp_packet_view_draw(const QXP_D3D_DRAW_REQUEST *request,
                             ULONG request_bytes,
                             QXP_D3D_DRAW_PACKET_VIEW *view)
{
    ULONG header_bytes = sizeof(*request) - sizeof(request->vertices);
    ULONG required;
    ULONG texture_count;
    ULONG texture;
    ULONG format;
    ULONG dimension;
    ULONG vertex;
    ULONG component;
    BOOLEAN indexed_mode;
    BOOLEAN object_mode;

    if (!view) return FALSE;
    view->failure_index = 0xffffffffUL;
    view->failure_detail = 0;
    if (!request || request_bytes < header_bytes ||
        request->point_scale_enable > 1 ||
        request->point_sprite_enable > 1 ||
        !request->vertex_count ||
        request->vertex_count > QXP_D3D_MAX_DRAW_VERTICES) return FALSE;
    if (request->sm2_program_name) {
        if ((request->pixel_sampled_stages &
             ~QXP_D3D_SM2_SAMPLER_MASK) ||
            (request->pixel_coordinate_stages & ~0xffUL) ||
            (request->pixel_cube_stages & ~QXP_D3D_SM2_SAMPLER_MASK) ||
            (request->pixel_volume_stages &
             ~QXP_D3D_SM2_SAMPLER_MASK) ||
            (request->pixel_cube_stages &
             request->pixel_volume_stages) ||
            (request->pixel_cube_stages &
             ~request->pixel_sampled_stages) ||
            (request->pixel_volume_stages &
             ~request->pixel_sampled_stages)) return FALSE;
    } else if (!qxp_packet_draw_mask_contract(
                   request->pixel_sampled_stages,
                   request->pixel_coordinate_stages,
                   request->pixel_cube_stages,
                   request->pixel_volume_stages)) {
        return FALSE;
    }
    view->object_data = NULL;
    view->hwtl_state = NULL;
    view->program = NULL;
    view->program_vertex_data = NULL;
    view->sm2_draw_state = NULL;
    view->compact = NULL;
    view->compact_vertex_data = NULL;
    view->compact_indexed = NULL;
    view->compact_indexed_vertex_data = NULL;
    view->compact_indexed_index_data = NULL;
    view->raw_indexed_object = NULL;
    view->raw_indexed_object_vertex_data = NULL;
    view->raw_indexed_object_index_data = NULL;
    view->vertices = request->vertices;
    view->vertex_indices = NULL;
    view->source_vertex_count = request->vertex_count;

    if (request->vertex_mode == QXP_D3D_VERTEX_COMPACT_TRANSFORMED) {
        return qxp_packet_compact_payload(
            request, request_bytes - header_bytes, view);
    }

    if (request->vertex_mode ==
            QXP_D3D_VERTEX_COMPACT_INDEXED_TRANSFORMED) {
        return qxp_packet_compact_indexed_payload(
            request, request_bytes - header_bytes, view);
    }

    if (request->vertex_mode == QXP_D3D_VERTEX_RAW_INDEXED_OBJECT)
        return qxp_packet_raw_indexed_object_payload(
            request, request_bytes, view);

    if (request->vertex_mode == QXP_D3D_VERTEX_PROGRAMMABLE) {
        const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program =
            (const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *)request->vertices;
        const QXP_D3D_SM2_DRAW_STATE *sm2_draw_state = NULL;

        /* The programmable vs epilogue consumes the per-stage projected mask,
         * so accept bits 0-3 (the four texture stages); reject higher bits. */
        if (request->fvf || (request->texture_projected_stages & ~15UL) ||
            request->texture_coord_index != 0 ||
            request->texture1_coord_index != 1 ||
            request->texture2_coord_index != 2 ||
            request->texture3_coord_index != 3 ||
            request->object_data_offset || request->hwtl_state_offset ||
            request->texture_coord_dimensions[0] ||
            request->texture_coord_dimensions[1] ||
            request->texture_coord_dimensions[2] ||
            request->texture_coord_dimensions[3] ||
            !qxp_packet_program_payload(
                program, request_bytes - header_bytes,
                request->vertex_count)) return FALSE;
        if (request->sm2_program_name) {
            sm2_draw_state =
                (const QXP_D3D_SM2_DRAW_STATE *)program->constants;
            if (!qxp_packet_sm2_sampler_contract(
                    request, sm2_draw_state)) return FALSE;
        }
        view->program = program;
        view->sm2_draw_state = sm2_draw_state;
        view->program_vertex_data =
            (const UCHAR *)program->vertex_data;
        return TRUE;
    }

    indexed_mode =
        request->vertex_mode == QXP_D3D_VERTEX_INDEXED_TRANSFORMED ||
        request->vertex_mode == QXP_D3D_VERTEX_INDEXED_OBJECT;
    object_mode =
        request->vertex_mode == QXP_D3D_VERTEX_OBJECT ||
        request->vertex_mode == QXP_D3D_VERTEX_INDEXED_OBJECT;
    texture_count =
        (request->fvf & QXP_PACKET_FVF_TEXCOUNT_MASK) >> 8;
    if (texture_count > 4) return FALSE;
    if (indexed_mode) {
        if (!qxp_packet_indexed_payload(request, request_bytes, view))
            return FALSE;
        required = request_bytes;
    } else {
        if (request->vertex_count > (0xffffffffUL - header_bytes) /
                                    sizeof(request->vertices[0]))
            return FALSE;
        required = header_bytes +
                   request->vertex_count * sizeof(request->vertices[0]);
    }

    if (!object_mode &&
        (request->vertex_mode == QXP_D3D_VERTEX_TRANSFORMED ||
         request->vertex_mode == QXP_D3D_VERTEX_INDEXED_TRANSFORMED)) {
        ULONG fvf_without_fog = request->fvf & ~QXP_PACKET_FVF_FOG;

        if ((!indexed_mode &&
             (request->object_data_offset || request->hwtl_state_offset)) ||
            (fvf_without_fog != 0x44 && fvf_without_fog != 0x0c4 &&
             fvf_without_fog != 0x144 && fvf_without_fog != 0x1c4 &&
             fvf_without_fog != 0x244 && fvf_without_fog != 0x2c4 &&
             fvf_without_fog != 0x344 && fvf_without_fog != 0x3c4 &&
             fvf_without_fog != 0x444 && fvf_without_fog != 0x4c4))
            return FALSE;
    } else if (object_mode) {
        if ((request->fvf & QXP_PACKET_FVF_POSITION_MASK) !=
                QXP_PACKET_FVF_XYZ ||
            (request->fvf & ~(QXP_PACKET_FVF_POSITION_MASK |
                              QXP_PACKET_FVF_NORMAL |
                              QXP_PACKET_FVF_PSIZE |
                              QXP_PACKET_FVF_DIFFUSE |
                              QXP_PACKET_FVF_SPECULAR |
                              QXP_PACKET_FVF_FOG |
                              QXP_PACKET_FVF_TEXCOUNT_MASK |
                              QXP_PACKET_FVF_TEXTURE_FORMAT_MASK)))
            return FALSE;
        if (!indexed_mode) {
            if (request->object_data_offset != required ||
                request->vertex_count >
                    (0xffffffffUL - required) /
                    sizeof(QXP_D3D_OBJECT_VERTEX_DATA))
                return FALSE;
            required += request->vertex_count *
                        sizeof(QXP_D3D_OBJECT_VERTEX_DATA);
            if (request->hwtl_state_offset != required ||
                required > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE))
                return FALSE;
            required += sizeof(QXP_D3D_HWTL_STATE);
            if (request_bytes < required) return FALSE;
            view->object_data = (const QXP_D3D_OBJECT_VERTEX_DATA *)
                ((const UCHAR *)request + request->object_data_offset);
            view->hwtl_state = (const QXP_D3D_HWTL_STATE *)
                ((const UCHAR *)request + request->hwtl_state_offset);
            if (!qxp_packet_hwtl_state(view->hwtl_state)) return FALSE;
        }
    } else {
        return FALSE;
    }
    if (request_bytes < required) return FALSE;
    if (request->fvf & QXP_PACKET_FVF_FOG) {
        for (vertex = 0; vertex < view->source_vertex_count; ++vertex) {
            if (!qxp_packet_float_finite(
                    view->vertices[vertex].fog_bits)) {
                view->failure_index = vertex;
                return FALSE;
            }
        }
    }

    if (view->object_data) {
        for (texture = 0; texture < texture_count; ++texture) {
            format = (request->fvf >> (16 + texture * 2)) & 3UL;
            dimension = format == 3 ? 1 : (format == 1 ? 3 :
                        (format == 2 ? 4 : 2));
            if (request->texture_coord_dimensions[texture] != dimension) {
                view->failure_index = texture;
                view->failure_detail = dimension;
                return FALSE;
            }
        }
        for (texture = texture_count; texture < 4; ++texture) {
            if ((request->fvf >> (16 + texture * 2)) & 3UL) {
                view->failure_index = texture;
                return FALSE;
            }
        }
        for (vertex = 0; vertex < view->source_vertex_count; ++vertex) {
            if (!qxp_packet_float_array(
                    &view->vertices[vertex].x_bits, 3) ||
                view->vertices[vertex].rhw_bits != 0x3f800000UL ||
                !qxp_packet_float_array(
                    view->object_data[vertex].normal, 3) ||
                !qxp_packet_float_finite(
                     view->object_data[vertex].point_size_bits) ||
                !(view->object_data[vertex].point_size_bits & 0x7fffffffUL) ||
                (!request->point_scale_enable &&
                 view->object_data[vertex].point_size_bits < 0x3f800000UL) ||
                view->object_data[vertex].point_size_bits > 0x42800000UL) {
                view->failure_index = vertex;
                view->failure_detail =
                    view->object_data[vertex].point_size_bits;
                return FALSE;
            }
            for (texture = 0; texture < texture_count; ++texture) {
                for (component = 0;
                     component < request->texture_coord_dimensions[texture];
                     ++component) {
                    if (!qxp_packet_float_finite(
                            view->vertices[vertex]
                                .texcoord[texture][component])) {
                        view->failure_index = vertex;
                        view->failure_detail =
                            (texture << 16) | component;
                        return FALSE;
                    }
                }
            }
        }
    }
    view->failure_index = 0xffffffffUL;
    view->failure_detail = 0;
    return TRUE;
}

BOOLEAN qxp_packet_view_draw_batch(
    const QXP_D3D_DRAW_BATCH_REQUEST *request, ULONG request_bytes,
    QXP_D3D_DRAW_BATCH_VIEW *view)
{
    const UCHAR *cursor;
    const UCHAR *end;
    const QXP_D3D_DRAW_BATCH_ENTRY *entry;
    const QXP_D3D_DRAW_REQUEST *draw;
    QXP_D3D_DRAW_PACKET_VIEW draw_view;
    ULONG draw_header_bytes = sizeof(*draw) - sizeof(draw->vertices);
    ULONG remaining;
    ULONG index;

    if (!view) return FALSE;
    view->cursor = NULL;
    view->remaining = 0;
    view->failure_reason = QXP_PACKET_BATCH_FAILURE_NONE;
    view->failure_entry = 0xffffffffUL;
    view->failure_draw = NULL;
    view->failure_draw_bytes = 0;
    view->draw_failure_index = 0xffffffffUL;
    view->draw_failure_detail = 0;
    if (!request ||
        request_bytes < QXP_D3D_DRAW_BATCH_HEADER_BYTES ||
        request_bytes > QXP_D3D_MAX_DRAW_BATCH_BYTES ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        !request->process_id || !request->draw_count ||
        request->draw_count > QXP_D3D_MAX_DRAW_BATCH_REQUESTS ||
        request->payload_bytes !=
            request_bytes - QXP_D3D_DRAW_BATCH_HEADER_BYTES ||
        request->payload_bytes <
            request->draw_count * QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES) {
        view->failure_reason = QXP_PACKET_BATCH_FAILURE_HEADER;
        return FALSE;
    }

    cursor = (const UCHAR *)request + QXP_D3D_DRAW_BATCH_HEADER_BYTES;
    end = cursor + request->payload_bytes;
    remaining = request->payload_bytes;
    for (index = 0; index < request->draw_count; ++index) {
        view->failure_entry = index;
        if (remaining < QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES) {
            view->failure_reason =
                QXP_PACKET_BATCH_FAILURE_ENTRY_HEADER;
            return FALSE;
        }
        entry = (const QXP_D3D_DRAW_BATCH_ENTRY *)cursor;
        if (entry->request_bytes < draw_header_bytes ||
            (entry->request_bytes & 3UL) ||
            entry->request_bytes >
                remaining - QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES) {
            view->failure_reason = QXP_PACKET_BATCH_FAILURE_ENTRY_SIZE;
            view->failure_draw_bytes = entry->request_bytes;
            return FALSE;
        }
        draw = (const QXP_D3D_DRAW_REQUEST *)entry->request;
        view->failure_draw = draw;
        view->failure_draw_bytes = entry->request_bytes;
        if (draw->version != request->version ||
            draw->process_id != request->process_id) {
            view->failure_reason =
                QXP_PACKET_BATCH_FAILURE_DRAW_IDENTITY;
            return FALSE;
        }
        if (!qxp_packet_view_draw(
                draw, entry->request_bytes, &draw_view)) {
            view->failure_reason = QXP_PACKET_BATCH_FAILURE_DRAW_PACKET;
            view->draw_failure_index = draw_view.failure_index;
            view->draw_failure_detail = draw_view.failure_detail;
            return FALSE;
        }
        cursor += QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                  entry->request_bytes;
        remaining -= QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                     entry->request_bytes;
    }
    if (cursor != end || remaining) {
        view->failure_reason = QXP_PACKET_BATCH_FAILURE_TRAILING;
        return FALSE;
    }
    view->cursor = (const UCHAR *)request +
                   QXP_D3D_DRAW_BATCH_HEADER_BYTES;
    view->remaining = request->draw_count;
    view->failure_reason = QXP_PACKET_BATCH_FAILURE_NONE;
    view->failure_entry = 0xffffffffUL;
    view->failure_draw = NULL;
    view->failure_draw_bytes = 0;
    view->draw_failure_index = 0xffffffffUL;
    view->draw_failure_detail = 0;
    return TRUE;
}

BOOLEAN qxp_packet_next_batch_draw(
    QXP_D3D_DRAW_BATCH_VIEW *view,
    const QXP_D3D_DRAW_REQUEST **request, PULONG request_bytes)
{
    const QXP_D3D_DRAW_BATCH_ENTRY *entry;

    if (!view || !request || !request_bytes || !view->cursor ||
        !view->remaining) return FALSE;
    entry = (const QXP_D3D_DRAW_BATCH_ENTRY *)view->cursor;
    *request = (const QXP_D3D_DRAW_REQUEST *)entry->request;
    *request_bytes = entry->request_bytes;
    view->cursor += QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                    entry->request_bytes;
    --view->remaining;
    return TRUE;
}
