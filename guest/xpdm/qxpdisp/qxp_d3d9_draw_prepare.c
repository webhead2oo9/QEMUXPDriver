#include <windef.h>

#include "qxp_d3d9_draw_prepare.h"

typedef union qxp_d3d9_draw_validation_scratch {
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT declaration;
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT vertex;
    QXP_D3D9_PIXEL_FUNCTION_SNAPSHOT pixel;
    /* A synthesized pixel stage is emitted here to measure it; the draw
     * path re-emits it from the recorded combiner state at define time. */
    char fixed_function_source[QXP_FF_PIXEL_MAX_SOURCE];
} QXP_D3D9_DRAW_VALIDATION_SCRATCH;

typedef char qxp_d3d9_draw_validation_scratch_fits_bytecode[
    sizeof(QXP_D3D9_DRAW_VALIDATION_SCRATCH) <=
        QXP_SHADER_MAX_TOKENS * sizeof(DWORD) ? 1 : -1];

/* The scratch also aliases the shader-IR allocation, so it must fit there
 * too - the synthesized-source slot is the largest member. */
typedef char qxp_d3d9_draw_validation_scratch_fits_ir[
    sizeof(QXP_D3D9_DRAW_VALIDATION_SCRATCH) <=
        sizeof(QXP_SHADER_IR) ? 1 : -1];

static void qxp_d3d9_prepare_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_d3d9_prepare_copy(
    void *destination, const void *source, ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static BOOL qxp_d3d9_prepare_equal(
    const void *left, const void *right, ULONG byte_count)
{
    const BYTE *left_bytes;
    const BYTE *right_bytes;
    ULONG index;

    left_bytes = (const BYTE *)left;
    right_bytes = (const BYTE *)right;
    for (index = 0; index < byte_count; ++index) {
        if (left_bytes[index] != right_bytes[index]) return FALSE;
    }
    return TRUE;
}

static void qxp_d3d9_prepare_reset_status(
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    if (status)
        qxp_d3d9_prepare_zero(status, (ULONG)sizeof(*status));
}

static void qxp_d3d9_prepare_set_failure(
    QXP_D3D9_DRAW_PREPARE_STATUS *status, DWORD code,
    DWORD stage, DWORD stream, DWORD input, DWORD interface_index)
{
    if (!status) return;
    status->code = code;
    status->stage = stage;
    status->stream = stream;
    status->input = input;
    status->interface_index = interface_index;
}

static void qxp_d3d9_prepare_store_owner(
    QXP_D3D9_DRAW_OWNER_IDENTITY *destination,
    const QXP_SHADER_OWNER *source)
{
    destination->context_handle = source->context_handle;
    destination->process_id = source->process_id;
    destination->device_identity = (ULONG_PTR)source->device;
}

static void qxp_d3d9_prepare_load_owner(
    QXP_SHADER_OWNER *destination,
    const QXP_D3D9_DRAW_OWNER_IDENTITY *source)
{
    qxp_shader_owner_initialize(
        destination, source->context_handle, source->process_id,
        (const void *)source->device_identity);
}

static BOOL qxp_d3d9_prepare_owner_matches(
    const QXP_D3D9_DRAW_OWNER_IDENTITY *identity,
    const QXP_SHADER_OWNER *owner)
{
    if (!identity || !owner) return FALSE;
    return identity->context_handle == owner->context_handle &&
           identity->process_id == owner->process_id &&
           identity->device_identity == (ULONG_PTR)owner->device;
}

static BOOL qxp_d3d9_prepare_binding_equal(
    const QXP_D3D9_SHADER_BINDING *left,
    const QXP_D3D9_SHADER_BINDING *right)
{
    return left && right && left->handle == right->handle &&
           left->generation == right->generation;
}

static BOOL qxp_d3d9_prepare_summary_matches_ir(
    const QXP_D3D9_SHADER_DECODE_SUMMARY *summary,
    const QXP_SHADER_IR *ir)
{
    if (!summary || !ir) return FALSE;
    return summary->stage == ir->stage &&
           summary->version_token == ir->version_token &&
           summary->token_count == ir->token_count &&
           summary->instruction_count == ir->instruction_count &&
           summary->declaration_count == ir->declaration_count &&
           summary->definition_count == ir->definition_count &&
           summary->comment_count == ir->comment_count &&
           summary->slot_count == ir->slot_count &&
           summary->arithmetic_slot_count == ir->arithmetic_slot_count &&
           summary->texture_slot_count == ir->texture_slot_count &&
           summary->flow_slot_count == ir->flow_slot_count &&
           summary->block_count == ir->block_count;
}

static BOOL qxp_d3d9_prepare_exact_bindings(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    const QXP_D3D9_DRAW_STATE *state;
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel;

    state = &prepared->raw_state;
    pixel = &prepared->raw_pixel_binding;
    if (state->declaration_mode ==
            QXP_D3D9_DRAW_DECLARATION_OBJECT) {
        if (!state->declaration_binding.handle || state->legacy_fvf ||
            !(state->declaration_binding.handle &
              QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) ||
            !qxp_d3d9_prepare_binding_equal(
                &state->declaration_binding,
                &prepared->declaration_binding)) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
            return FALSE;
        }
    } else if (state->declaration_mode ==
                   QXP_D3D9_DRAW_DECLARATION_FVF) {
        if (!state->legacy_fvf ||
            state->declaration_binding.handle ||
            state->declaration_binding.generation ||
            prepared->declaration_binding.handle ||
            prepared->declaration_binding.generation) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
            return FALSE;
        }
    } else {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
        return FALSE;
    }
    /* A synthesized stage stands in for the vertex object exactly as one
     * stands in for the pixel object below.  Its binding must be absent
     * outright: a stale handle means the caller thinks a program is bound,
     * which is a different draw from this one. */
    if (!state->vertex_function_binding.handle) {
        if (prepared->vertex_shader_binding.handle ||
            prepared->vertex_shader_binding.generation) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
            return FALSE;
        }
    } else if (!qxp_d3d9_prepare_binding_equal(
                   &state->vertex_function_binding,
                   &prepared->vertex_shader_binding)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    /* A synthesized stage stands in for the pixel object.  Its binding must
     * be absent outright: a stale or legacy handle means the caller thinks a
     * program is bound, which is a different draw from this one. */
    if (prepared->fixed_function_pixel_signature) {
        if (pixel->legacy_handle || pixel->sm2.handle ||
            pixel->sm2.generation) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
            return FALSE;
        }
    } else if (pixel->legacy_handle || !pixel->sm2.handle ||
               !pixel->sm2.generation ||
               !qxp_d3d9_prepare_binding_equal(
                   &pixel->sm2, &prepared->pixel_shader_binding)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
        return FALSE;
    }
    /* A synthesized vertex stage decodes no bytecode, so it carries no
     * summary, exactly as a synthesized pixel stage carries none. */
    if (state->vertex_function_binding.handle &&
        (prepared->vertex_shader_summary.stage !=
             QXP_SHADER_STAGE_VERTEX ||
         !QXP_SHADER_VERSION_IS_VERTEX(
             prepared->vertex_shader_summary.version_token))) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    /* A synthesized stage decodes no bytecode, so it carries no summary. */
    if (!prepared->fixed_function_pixel_signature &&
        (prepared->pixel_shader_summary.stage !=
             QXP_SHADER_STAGE_PIXEL ||
         !QXP_SHADER_VERSION_IS_PIXEL(
             prepared->pixel_shader_summary.version_token))) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_d3d9_prepare_snapshot_locked(
    QXP_D3D9_DRAW_PREPARED *prepared, DWORD *vertex_bytecode,
    QXP_D3D9_DRAW_VALIDATION_SCRATCH *scratch,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    qxp_d3d9_prepare_store_owner(&prepared->owner, owner);
    qxp_d3d9_prepare_copy(
        &prepared->raw_state, draw_state,
        (ULONG)sizeof(prepared->raw_state));
    qxp_d3d9_prepare_copy(
        &prepared->raw_pixel_binding, pixel_binding,
        (ULONG)sizeof(prepared->raw_pixel_binding));

    if (prepared->raw_state.declaration_mode ==
            QXP_D3D9_DRAW_DECLARATION_FVF) {
        if (!QxpBuildVertexDeclaration9FromFvf(
                prepared->raw_state.legacy_fvf,
                &prepared->declaration)) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
            return FALSE;
        }
    } else {
        if (!QxpD3d9SnapshotVertexDeclaration(
                table, owner, &prepared->raw_state.declaration_binding,
                &scratch->declaration,
                &status->shader_object_error)) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT,
                QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
            return FALSE;
        }
        prepared->declaration_binding = scratch->declaration.binding;
        qxp_d3d9_prepare_copy(
            &prepared->declaration, &scratch->declaration.declaration,
            (ULONG)sizeof(prepared->declaration));
    }

    if (!QxpD3d9SnapshotVertexFunction(
            table, owner, &prepared->raw_state.vertex_function_binding,
            &scratch->vertex, vertex_bytecode,
            QXP_SHADER_MAX_TOKENS * (ULONG)sizeof(DWORD),
            &status->shader_object_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    prepared->vertex_shader_binding = scratch->vertex.binding;
    prepared->vertex_shader_bytecode_size = scratch->vertex.bytecode_size;
    qxp_d3d9_prepare_copy(
        &prepared->vertex_shader_summary, &scratch->vertex.summary,
        (ULONG)sizeof(prepared->vertex_shader_summary));

    /* No pixel object bound: the pixel stage is the fixed-function combiner.
     * Synthesizing it here - rather than refusing - is what lets a title that
     * pairs a programmable vertex shader with fixed-function pixel
     * processing execute on the native path at all. */
    if (fixed_function_pixel && !prepared->raw_pixel_binding.legacy_handle &&
        !prepared->raw_pixel_binding.sm2.handle) {
        QXP_FF_PIXEL_ERROR fixed_function_error;
        DWORD source_bytes = 0;

        qxp_d3d9_prepare_copy(
            &prepared->fixed_function_pixel, fixed_function_pixel,
            (ULONG)sizeof(prepared->fixed_function_pixel));
        if (!QxpEmitFixedFunctionPixelGlsl(
                &prepared->fixed_function_pixel,
                scratch->fixed_function_source,
                (DWORD)sizeof(scratch->fixed_function_source),
                &source_bytes, &prepared->pixel_metadata,
                &fixed_function_error)) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
                QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL,
                fixed_function_error.code, fixed_function_error.stage,
                fixed_function_error.value);
            return FALSE;
        }
        prepared->pixel_glsl_source_bytes = source_bytes;
        prepared->fixed_function_pixel_signature =
            QxpFixedFunctionPixelSignature(&prepared->fixed_function_pixel);
        return TRUE;
    }

    if (!QxpD3d9SnapshotPixelFunction(
            table, owner, &prepared->raw_pixel_binding.sm2,
            &scratch->pixel, NULL, 0,
            &status->shader_object_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT,
            QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
        return FALSE;
    }
    prepared->pixel_shader_binding = scratch->pixel.binding;
    prepared->pixel_shader_bytecode_size = scratch->pixel.bytecode_size;
    prepared->pixel_glsl_source_bytes = scratch->pixel.glsl_source_bytes;
    qxp_d3d9_prepare_copy(
        &prepared->pixel_shader_summary, &scratch->pixel.summary,
        (ULONG)sizeof(prepared->pixel_shader_summary));
    qxp_d3d9_prepare_copy(
        &prepared->pixel_metadata, &scratch->pixel.metadata,
        (ULONG)sizeof(prepared->pixel_metadata));
    return TRUE;
}

static DWORD qxp_d3d9_prepare_vertex_output_mask(
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata)
{
    return metadata->attribute_output_mask |
           (metadata->texcoord_output_mask << 2);
}

static void qxp_d3d9_prepare_derive_interface(
    QXP_D3D9_DRAW_PREPARED *prepared)
{
    QXP_D3D9_DRAW_STAGE_INTERFACE *interface_state;
    const QXP_VERTEX_SHADER_GLSL_METADATA *vertex;
    const QXP_PIXEL_SHADER_GLSL_METADATA *pixel;
    DWORD index;
    DWORD required;
    DWORD available;

    interface_state = &prepared->stage_interface;
    vertex = &prepared->vertex_metadata;
    pixel = &prepared->pixel_metadata;
    qxp_d3d9_prepare_zero(
        interface_state, (ULONG)sizeof(*interface_state));

    interface_state->pixel_required_mask =
        pixel->input_color_mask | (pixel->input_texture_mask << 2);
    interface_state->vertex_available_mask =
        qxp_d3d9_prepare_vertex_output_mask(vertex);
    interface_state->matched_mask =
        interface_state->pixel_required_mask &
        interface_state->vertex_available_mask;
    interface_state->missing_mask =
        interface_state->pixel_required_mask &
        ~interface_state->vertex_available_mask;
    interface_state->pixel_centroid_mask =
        pixel->input_centroid_mask & 0x3ffUL;
    interface_state->matched_centroid_mask =
        interface_state->pixel_centroid_mask &
        interface_state->matched_mask;

    for (index = 0; index < QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT;
         ++index) {
        if (index < 2) {
            required = pixel->input_color_components[index] & 15U;
            available = vertex->attribute_output_components[index] & 15U;
        } else {
            required = pixel->input_texture_components[index - 2] & 15U;
            available =
                vertex->texcoord_output_components[index - 2] & 15U;
        }
        interface_state->pixel_required_components[index] =
            (BYTE)required;
        interface_state->vertex_available_components[index] =
            (BYTE)available;
        interface_state->matched_components[index] =
            (BYTE)(required & available);
        interface_state->missing_components[index] =
            (BYTE)(required & ~available);
        if (interface_state->missing_components[index]) {
            interface_state->missing_policy[index] =
                QXP_D3D9_DRAW_INTERFACE_MISSING_ZERO;
        }
    }
}

static BOOL qxp_d3d9_prepare_lower_vertex(
    QXP_D3D9_DRAW_PREPARED *prepared, const QXP_SHADER_IR *ir,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    DWORD source_bytes;
    DWORD centroid_mask;

    source_bytes = 0;
    if (!QxpEmitVertexShader20Glsl(
            ir, 0, NULL, 0, &source_bytes,
            &prepared->vertex_metadata,
            &status->vertex_lowering_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_LOWERING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }

    centroid_mask = 0;
    if (prepared->vertex_link.mode ==
            QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE) {
        qxp_d3d9_prepare_derive_interface(prepared);
        centroid_mask = prepared->stage_interface.matched_centroid_mask;
    }
    source_bytes = 0;
    if (!QxpEmitVertexShader20Glsl(
            ir, centroid_mask, NULL, 0, &source_bytes,
            &prepared->vertex_metadata,
            &status->vertex_lowering_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_LOWERING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    prepared->vertex_glsl_source_bytes = source_bytes;
    if (prepared->vertex_link.mode ==
            QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE) {
        qxp_d3d9_prepare_derive_interface(prepared);
    } else {
        qxp_d3d9_prepare_zero(
            &prepared->stage_interface,
            (ULONG)sizeof(prepared->stage_interface));
    }
    return TRUE;
}

/* Component count a declaration type presents to the vertex stage.  Zero for
 * a type the pre-transformed stage cannot forward, which keeps an
 * inexpressible declaration a refused draw.  The admitted set is exactly the
 * one the program vertex layout can pack, so prepare can never accept a
 * declaration the layout then rejects. */
static DWORD qxp_d3d9_prepare_decl_components(BYTE type)
{
    switch (type) {
    case QXP_VERTEX_DECLTYPE9_FLOAT1:
        return 1;
    case QXP_VERTEX_DECLTYPE9_FLOAT2:
    case QXP_VERTEX_DECLTYPE9_SHORT2:
        return 2;
    case QXP_VERTEX_DECLTYPE9_FLOAT3:
        return 3;
    case QXP_VERTEX_DECLTYPE9_FLOAT4:
    case QXP_VERTEX_DECLTYPE9_D3DCOLOR:
    case QXP_VERTEX_DECLTYPE9_UBYTE4:
    case QXP_VERTEX_DECLTYPE9_SHORT4:
        return 4;
    default:
        return 0;
    }
}

/* Reduces a pre-transformed declaration to the semantics the synthesized
 * vertex stage forwards.  The runtime ignores the bound shader for such a
 * draw, so the declaration - not the shader - is the whole input contract.
 * A semantic beyond POSITIONT, COLOR0/1 and TEXCOORD0..7 has no place to go
 * in a pre-transformed stage and refuses the draw rather than vanishing. */
static BOOL qxp_d3d9_prepare_positiont_state(
    const QXP_VERTEX_DECLARATION9 *declaration,
    QXP_POSITIONT_VERTEX_STATE *state, DWORD *reason)
{
    DWORD index;
    DWORD components;
    BOOL seen_position = FALSE;

    qxp_d3d9_prepare_zero(state, (ULONG)sizeof(*state));
    *reason = 0;
    for (index = 0; index < declaration->element_count; ++index) {
        const QXP_VERTEX_DECLARATION9_ELEMENT *element;

        element = &declaration->elements[index];
        components = qxp_d3d9_prepare_decl_components(element->type);
        if (!components) {
            *reason = element->type;
            return FALSE;
        }
        if (element->usage == QXP_VERTEX_DECLUSAGE9_POSITIONT) {
            /* Exactly one pre-transformed position, and it must carry the
             * screen x/y, the depth and the reciprocal w the stage divides
             * by, so a narrower type cannot stand in for it. */
            if (element->usage_index || seen_position ||
                element->type != QXP_VERTEX_DECLTYPE9_FLOAT4) {
                *reason = element->usage;
                return FALSE;
            }
            seen_position = TRUE;
        } else if (element->usage == QXP_VERTEX_DECLUSAGE9_COLOR) {
            if (element->usage_index >= QXP_POSITIONT_COLOR_COUNT) {
                *reason = element->usage;
                return FALSE;
            }
            state->color_mask |= 1UL << element->usage_index;
        } else if (element->usage == QXP_VERTEX_DECLUSAGE9_TEXCOORD) {
            if (element->usage_index >= QXP_POSITIONT_TEXCOORD_COUNT) {
                *reason = element->usage;
                return FALSE;
            }
            state->texcoord_mask |= 1UL << element->usage_index;
            state->texcoord_components[element->usage_index] =
                (BYTE)components;
        } else if (element->usage != QXP_VERTEX_DECLUSAGE9_PSIZE) {
            /* A per-vertex point size reaches no execution path in this
             * driver: D3DRS_POINTSIZE is the only point size the miniport
             * is ever given, and the legacy fixed-function route the
             * synthesized stage stands in for ignores the attribute too.
             * Refusing it here would make the synthesized stage strictly
             * weaker than the route it replaces, so it is accepted and left
             * ungathered - identical observable behaviour, not a dropped
             * output. */
            *reason = element->usage;
            return FALSE;
        }
    }
    return seen_position;
}

/* Replaces the lowered shader stage with one synthesized from the
 * pre-transformed declaration.  The bound shader is decoded and lowered
 * first because the object snapshot and its summary still have to agree with
 * the program cache key; only the emitted stage and its metadata differ. */
static BOOL qxp_d3d9_prepare_synthesize_positiont(
    QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_POSITIONT_VERTEX_ERROR positiont_error;
    DWORD source_bytes = 0;
    DWORD reason = 0;

    if (!qxp_d3d9_prepare_positiont_state(
            &prepared->declaration, &prepared->positiont_vertex, &reason)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, reason, 0, 0);
        return FALSE;
    }
    if (!QxpEmitPositiontVertexGlsl(
            &prepared->positiont_vertex, NULL, 0, &source_bytes,
            &prepared->vertex_metadata, &positiont_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_LOWERING,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, positiont_error.code,
            positiont_error.index, positiont_error.value);
        return FALSE;
    }
    prepared->vertex_glsl_source_bytes = source_bytes;
    prepared->positiont_vertex_signature =
        QxpPositiontVertexSignature(&prepared->positiont_vertex);
    /* The bypass link carries no input mappings, because the ignored shader
     * declared none that matter.  Rebuild it from the synthesized metadata so
     * the packed vertex the draw path gathers is described by the same
     * construction the draw path re-derives to validate it. */
    if (!QxpD3d9RebuildVertexShaderLink(
            &prepared->declaration, &prepared->vertex_metadata,
            QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS,
            &prepared->vertex_link)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    /* Every declared input is matched by construction, so an undefined one
     * would mean the state and the declaration disagree. */
    if (prepared->vertex_link.undefined_input_mask) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX,
            prepared->vertex_link.undefined_input_mask, 0, 0);
        return FALSE;
    }
    /* The pixel stage links against the synthesized outputs, not the ignored
     * shader's, so the interface has to be derived again from them. */
    qxp_d3d9_prepare_derive_interface(prepared);
    return TRUE;
}

static void qxp_d3d9_prepare_effective_constants(
    QXP_D3D9_DRAW_PREPARED *prepared)
{
    QXP_D3D9_DRAW_EFFECTIVE_CONSTANTS *effective;
    const QXP_D3D9_DRAW_STATE *raw;
    const QXP_VERTEX_SHADER_GLSL_METADATA *vertex;
    const QXP_PIXEL_SHADER_GLSL_METADATA *pixel;
    DWORD index;

    effective = &prepared->effective_constants;
    raw = &prepared->raw_state;
    vertex = &prepared->vertex_metadata;
    pixel = &prepared->pixel_metadata;

    qxp_d3d9_prepare_copy(
        effective->vs_float, raw->vs_float,
        (ULONG)sizeof(effective->vs_float));
    qxp_d3d9_prepare_copy(
        effective->vs_integer, raw->vs_integer,
        (ULONG)sizeof(effective->vs_integer));
    qxp_d3d9_prepare_copy(
        effective->vs_boolean, raw->vs_boolean,
        (ULONG)sizeof(effective->vs_boolean));
    qxp_d3d9_prepare_copy(
        effective->ps_float, raw->ps_float,
        (ULONG)sizeof(effective->ps_float));
    qxp_d3d9_prepare_copy(
        effective->ps_integer, raw->ps_integer,
        (ULONG)sizeof(effective->ps_integer));
    qxp_d3d9_prepare_copy(
        effective->ps_boolean, raw->ps_boolean,
        (ULONG)sizeof(effective->ps_boolean));
    qxp_d3d9_prepare_copy(
        effective->vs_float_set_mask, raw->vs_float_set_mask,
        (ULONG)sizeof(effective->vs_float_set_mask));
    effective->vs_integer_set_mask = raw->vs_integer_set_mask;
    effective->vs_boolean_set_mask = raw->vs_boolean_set_mask;
    effective->ps_float_set_mask = raw->ps_float_set_mask;
    effective->ps_integer_set_mask = raw->ps_integer_set_mask;
    effective->ps_boolean_set_mask = raw->ps_boolean_set_mask;

    for (index = 0; index < QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT;
         ++index) {
        if (!(vertex->float_definition_mask[index / 32UL] &
              (1UL << (index & 31UL)))) continue;
        qxp_d3d9_prepare_copy(
            effective->vs_float[index],
            vertex->float_definition_values[index],
            (ULONG)sizeof(effective->vs_float[index]));
        effective->vs_float_set_mask[index / 32UL] |=
            1UL << (index & 31UL);
    }
    for (index = 0; index < QXP_D3D9_DRAW_VS_INTEGER_CONSTANT_COUNT;
         ++index) {
        if (!(vertex->integer_definition_mask & (1UL << index)))
            continue;
        qxp_d3d9_prepare_copy(
            effective->vs_integer[index],
            vertex->integer_definition_values[index],
            (ULONG)sizeof(effective->vs_integer[index]));
        effective->vs_integer_set_mask |= 1UL << index;
    }
    for (index = 0; index < QXP_D3D9_DRAW_VS_BOOLEAN_CONSTANT_COUNT;
         ++index) {
        if (!(vertex->boolean_definition_mask & (1UL << index)))
            continue;
        effective->vs_boolean[index] =
            vertex->boolean_definition_values[index];
        effective->vs_boolean_set_mask |= 1UL << index;
    }
    for (index = 0; index < QXP_D3D9_DRAW_PS_FLOAT_CONSTANT_COUNT;
         ++index) {
        if (!(pixel->definition_mask & (1UL << index))) continue;
        qxp_d3d9_prepare_copy(
            effective->ps_float[index], pixel->definition_values[index],
            (ULONG)sizeof(effective->ps_float[index]));
        effective->ps_float_set_mask |= 1UL << index;
    }
}

static BOOL qxp_d3d9_prepare_streams(
    QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    const QXP_VERTEX_DECLARATION9 *declaration;
    DWORD stream;
    DWORD required_span;
    DWORD required_mask;

    declaration = &prepared->declaration;
    if (prepared->vertex_link.mode ==
            QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS) {
        required_mask = declaration->stream_mask;
        qxp_d3d9_prepare_copy(
            prepared->required_stream_spans,
            declaration->stream_spans,
            (ULONG)sizeof(prepared->required_stream_spans));
    } else {
        required_mask = prepared->vertex_link.matched_stream_mask;
        qxp_d3d9_prepare_copy(
            prepared->required_stream_spans,
            prepared->vertex_link.required_stream_spans,
            (ULONG)sizeof(prepared->required_stream_spans));
    }
    prepared->required_stream_mask = required_mask;

    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        const QXP_D3D9_DRAW_STREAM_STATE *source;

        if (!(required_mask & (1UL << stream))) continue;
        source = &prepared->raw_state.streams[stream];
        required_span = prepared->required_stream_spans[stream];
        if (source->user_memory) {
            if (stream != 0 || source->handle || source->offset ||
                source->source2) {
                qxp_d3d9_prepare_set_failure(
                    status, QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_SOURCE,
                    QXP_D3D9_DRAW_PREPARE_STAGE_NONE, stream, 0, 0);
                return FALSE;
            }
            prepared->required_user_memory_stream_mask |=
                1UL << stream;
        } else if (!source->handle) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_SOURCE,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE, stream, 0, 0);
            return FALSE;
        }
        if (!required_span || !source->stride ||
            source->stride < required_span) {
            /* Three different faults share this code - a matched stream with
             * no span, an unbound stride, and a stride too short for the
             * span - so carry the pair that separates them in the generic
             * detail slots rather than leaving the caller to guess. */
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_STRIDE,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE, stream, required_span,
                source->stride);
            return FALSE;
        }
        if (source->offset > 0xffffffffUL - required_span) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_OVERFLOW,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE, stream, 0, 0);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL qxp_d3d9_prepare_fresh_locked(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_DRAW_VALIDATION_SCRATCH *scratch,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_SHADER_OWNER owner;

    qxp_d3d9_prepare_load_owner(&owner, &prepared->owner);
    if (!qxp_d3d9_prepare_equal(
            draw_state, &prepared->raw_state,
            (ULONG)sizeof(*draw_state)) ||
        !qxp_d3d9_prepare_equal(
            pixel_binding, &prepared->raw_pixel_binding,
            (ULONG)sizeof(*pixel_binding))) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        return FALSE;
    }
    if (prepared->raw_state.declaration_mode ==
            QXP_D3D9_DRAW_DECLARATION_FVF) {
        if (!QxpBuildVertexDeclaration9FromFvf(
                prepared->raw_state.legacy_fvf,
                &scratch->declaration.declaration) ||
            !qxp_d3d9_prepare_equal(
                &scratch->declaration.declaration,
                &prepared->declaration,
                (ULONG)sizeof(prepared->declaration))) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
                QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
            return FALSE;
        }
    } else if (!QxpD3d9SnapshotVertexDeclaration(
                   table, &owner, &prepared->declaration_binding,
                   &scratch->declaration,
                   &status->shader_object_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
            QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION, 0, 0, 0);
        return FALSE;
    }
    if (!QxpD3d9SnapshotVertexFunction(
            table, &owner, &prepared->vertex_shader_binding,
            &scratch->vertex, NULL, 0,
            &status->shader_object_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    /* A synthesized stage has no pixel object to re-snapshot; its freshness
     * is the combiner state it was emitted from.  Comparing that state is
     * what stops a texture-stage change from executing a stale program. */
    if (prepared->fixed_function_pixel_signature) {
        if (!fixed_function_pixel ||
            !qxp_d3d9_prepare_equal(
                fixed_function_pixel, &prepared->fixed_function_pixel,
                (ULONG)sizeof(*fixed_function_pixel))) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
                QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
            return FALSE;
        }
    } else if (!QxpD3d9SnapshotPixelFunction(
                   table, &owner, &prepared->pixel_shader_binding,
                   &scratch->pixel, NULL, 0,
                   &status->shader_object_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
            QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_d3d9_prepare_fresh(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_DRAW_VALIDATION_SCRATCH *scratch,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    BOOL result;

    qxp_d3d9_prepare_reset_status(status);
    lock_callback(lock_opaque);
    result = qxp_d3d9_prepare_fresh_locked(
        prepared, draw_state, pixel_binding, fixed_function_pixel, table,
        scratch, status);
    unlock_callback(lock_opaque);
    return result;
}

static void qxp_d3d9_prepare_release_temporary(
    QXP_D3D9_DRAW_PREPARED *prepared, DWORD *vertex_bytecode,
    QXP_SHADER_IR *ir, const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    BOOL release_prepared)
{
    if (ir) allocator->release(ir, allocator->opaque);
    if (vertex_bytecode)
        allocator->release(vertex_bytecode, allocator->opaque);
    if (release_prepared && prepared)
        allocator->release(prepared, allocator->opaque);
}

/* True when every input the derived half is a pure function of is unchanged,
 * so the cached decode, link, lowering, and emitted stages still describe
 * this draw.  Deliberately excludes constants, stream offsets, and render
 * state: those feed the per-draw half, which is always recomputed. */
static BOOL qxp_d3d9_prepare_entry_matches(
    const QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY *entry,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    if (!entry->valid) return FALSE;
    if (entry->table_generation != table->generation_clock) return FALSE;
    if (!qxp_d3d9_prepare_owner_matches(&entry->owner, owner)) return FALSE;
    if (entry->declaration_mode != draw_state->declaration_mode ||
        entry->legacy_fvf != draw_state->legacy_fvf)
        return FALSE;
    if (!qxp_d3d9_prepare_equal(
            &entry->declaration_binding, &draw_state->declaration_binding,
            (ULONG)sizeof(entry->declaration_binding)) ||
        !qxp_d3d9_prepare_equal(
            &entry->vertex_function_binding,
            &draw_state->vertex_function_binding,
            (ULONG)sizeof(entry->vertex_function_binding)) ||
        !qxp_d3d9_prepare_equal(
            &entry->pixel_binding, pixel_binding,
            (ULONG)sizeof(entry->pixel_binding)))
        return FALSE;
    if (entry->has_fixed_function_pixel != (fixed_function_pixel != NULL))
        return FALSE;
    if (fixed_function_pixel &&
        !qxp_d3d9_prepare_equal(
            &entry->fixed_function_pixel, fixed_function_pixel,
            (ULONG)sizeof(entry->fixed_function_pixel)))
        return FALSE;
    return TRUE;
}

static const QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY *qxp_d3d9_prepare_cache_lookup(
    const QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    DWORD way;

    for (way = 0; way < QXP_D3D9_DRAW_PREPARE_CACHE_WAYS; ++way) {
        if (qxp_d3d9_prepare_entry_matches(
                &cache->ways[way], owner, draw_state, pixel_binding,
                fixed_function_pixel, table))
            return &cache->ways[way];
    }
    return NULL;
}

static void qxp_d3d9_prepare_cache_store(
    QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY *entry;

    /* Round-robin rather than LRU: the working set either fits, in which case
     * every policy hits, or it does not, in which case recency buys nothing
     * for an interleaved draw order. */
    entry = &cache->ways[cache->next_victim];
    cache->next_victim =
        (cache->next_victim + 1) % QXP_D3D9_DRAW_PREPARE_CACHE_WAYS;

    qxp_d3d9_prepare_copy(
        &entry->prepared, prepared, (ULONG)sizeof(entry->prepared));
    qxp_d3d9_prepare_store_owner(&entry->owner, owner);
    qxp_d3d9_prepare_copy(
        &entry->declaration_binding, &draw_state->declaration_binding,
        (ULONG)sizeof(entry->declaration_binding));
    qxp_d3d9_prepare_copy(
        &entry->vertex_function_binding,
        &draw_state->vertex_function_binding,
        (ULONG)sizeof(entry->vertex_function_binding));
    qxp_d3d9_prepare_copy(
        &entry->pixel_binding, pixel_binding,
        (ULONG)sizeof(entry->pixel_binding));
    entry->has_fixed_function_pixel = fixed_function_pixel != NULL;
    if (fixed_function_pixel) {
        qxp_d3d9_prepare_copy(
            &entry->fixed_function_pixel, fixed_function_pixel,
            (ULONG)sizeof(entry->fixed_function_pixel));
    } else {
        qxp_d3d9_prepare_zero(
            &entry->fixed_function_pixel,
            (ULONG)sizeof(entry->fixed_function_pixel));
    }
    entry->declaration_mode = draw_state->declaration_mode;
    entry->legacy_fvf = draw_state->legacy_fvf;
    entry->table_generation = table->generation_clock;
    entry->valid = TRUE;
}

/* Decodes, links and lowers the bound vertex object into the prepared
 * stage.  Split out from the snapshot builder so the fixed-function route -
 * which has no object to decode - reads as the alternative it is rather
 * than as a special case threaded through the programmable path.  Cleanup
 * stays with the caller, which owns the scratch. */
static BOOL qxp_d3d9_prepare_vertex_object_stage(
    QXP_D3D9_DRAW_PREPARED *prepared, DWORD *vertex_bytecode,
    QXP_SHADER_IR *ir, QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    DWORD token_count;

    if (!prepared->vertex_shader_bytecode_size ||
        prepared->vertex_shader_bytecode_size % sizeof(DWORD) != 0 ||
        prepared->vertex_shader_bytecode_size >
            QXP_SHADER_MAX_TOKENS * sizeof(DWORD)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BYTECODE,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    token_count = prepared->vertex_shader_bytecode_size /
                  (DWORD)sizeof(DWORD);
    if (!QxpDecodeShader20(
            vertex_bytecode, token_count, ir, &status->decode_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_DECODE,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    if (!qxp_d3d9_prepare_summary_matches_ir(
            &prepared->vertex_shader_summary, ir)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_SUMMARY,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    if (!QxpD3d9LinkVertexShader20(
            &prepared->declaration, ir,
            &prepared->vertex_link, &status->semantic_link_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        return FALSE;
    }
    if (!qxp_d3d9_prepare_lower_vertex(prepared, ir, status)) return FALSE;
    if (prepared->vertex_link.mode ==
            QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE) {
        prepared->mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
        prepared->undefined_vertex_input_mask =
            prepared->vertex_link.undefined_input_mask;
        return TRUE;
    }
    if (prepared->vertex_link.mode ==
            QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS) {
        prepared->mode = QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS;
        return qxp_d3d9_prepare_synthesize_positiont(prepared, status);
    }
    qxp_d3d9_prepare_set_failure(
        status, QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK,
        QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
    return FALSE;
}

/* No vertex object is bound, so the vertex stage is fixed-function
 * processing.  Synthesizing it - rather than refusing - is the exact mirror
 * of the synthesized pixel stage, and is what lets a title that pairs
 * fixed-function vertex processing with a ps_1_x or ps_2_0 pixel shader
 * execute on the native path at all: the bound pixel object forces the
 * native route, and the native route otherwise demands a vertex object that
 * such a draw never had.  A pre-transformed declaration is the one
 * fixed-function stage this driver can express, so anything else still
 * refuses inside the synthesis rather than drawing wrongly. */
static BOOL qxp_d3d9_prepare_fixed_function_vertex_stage(
    QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    prepared->mode = QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS;
    return qxp_d3d9_prepare_synthesize_positiont(prepared, status);
}

QXP_D3D9_DRAW_PREPARED *QxpD3d9PrepareDrawSnapshot(
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    DWORD *vertex_bytecode;
    QXP_SHADER_IR *ir;
    BOOL snapshot_result;
    BOOL vertex_stage_result;
    const QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY *hit;

    qxp_d3d9_prepare_reset_status(status);
    if (!status || !owner || !qxp_shader_owner_is_set(owner) ||
        !draw_state || !pixel_binding || !table || !allocator ||
        !allocator->allocate || !allocator->release || !lock_callback ||
        !unlock_callback) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        return NULL;
    }

    prepared = (QXP_D3D9_DRAW_PREPARED *)allocator->allocate(
        (ULONG)sizeof(*prepared), allocator->opaque);
    if (!prepared) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        return NULL;
    }
    qxp_d3d9_prepare_zero(prepared, (ULONG)sizeof(*prepared));

    hit = cache ? qxp_d3d9_prepare_cache_lookup(
                      cache, owner, draw_state, pixel_binding,
                      fixed_function_pixel, table)
                : NULL;
    if (hit) {
        QXP_D3D9_DRAW_VALIDATION_SCRATCH *scratch;
#ifdef QXP_D3D_PROFILE
        ++cache->profile_hits;
#endif

        /* Reuse the derived half, then recompute the per-draw half over it.
         * Freshness is still proved the same way the full build proves it,
         * so a reused snapshot carries no weaker guarantee. */
        qxp_d3d9_prepare_copy(
            prepared, &hit->prepared, (ULONG)sizeof(*prepared));
        qxp_d3d9_prepare_copy(
            &prepared->raw_state, draw_state,
            (ULONG)sizeof(prepared->raw_state));
        qxp_d3d9_prepare_copy(
            &prepared->raw_pixel_binding, pixel_binding,
            (ULONG)sizeof(prepared->raw_pixel_binding));
        if (!QxpD3d9DrawStateValidate(
                &prepared->raw_state, &status->draw_state_error)) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE,
                status->draw_state_error.stream, 0, 0);
            qxp_d3d9_prepare_release_temporary(
                prepared, NULL, NULL, allocator, TRUE);
            return NULL;
        }
        if (!qxp_d3d9_prepare_exact_bindings(prepared, status) ||
            !qxp_d3d9_prepare_streams(prepared, status)) {
            qxp_d3d9_prepare_release_temporary(
                prepared, NULL, NULL, allocator, TRUE);
            return NULL;
        }
        qxp_d3d9_prepare_effective_constants(prepared);
        scratch = (QXP_D3D9_DRAW_VALIDATION_SCRATCH *)allocator->allocate(
            (ULONG)sizeof(*scratch), allocator->opaque);
        if (!scratch) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
            qxp_d3d9_prepare_release_temporary(
                prepared, NULL, NULL, allocator, TRUE);
            return NULL;
        }
        if (!qxp_d3d9_prepare_fresh(
                prepared, draw_state, pixel_binding, fixed_function_pixel,
                table, scratch, lock_callback, unlock_callback, lock_opaque,
                status)) {
            allocator->release(scratch, allocator->opaque);
            qxp_d3d9_prepare_release_temporary(
                prepared, NULL, NULL, allocator, TRUE);
            return NULL;
        }
        allocator->release(scratch, allocator->opaque);
        prepared->valid = TRUE;
        return prepared;
    }
#ifdef QXP_D3D_PROFILE
    if (cache) ++cache->profile_misses;
#endif

    vertex_bytecode = (DWORD *)allocator->allocate(
        QXP_SHADER_MAX_TOKENS * (ULONG)sizeof(DWORD), allocator->opaque);
    if (!vertex_bytecode) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        qxp_d3d9_prepare_release_temporary(
            prepared, NULL, NULL, allocator, TRUE);
        return NULL;
    }
    ir = (QXP_SHADER_IR *)allocator->allocate(
        (ULONG)sizeof(*ir), allocator->opaque);
    if (!ir) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION,
            QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX, 0, 0, 0);
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, NULL, allocator, TRUE);
        return NULL;
    }

    lock_callback(lock_opaque);
    snapshot_result = qxp_d3d9_prepare_snapshot_locked(
        prepared, vertex_bytecode,
        (QXP_D3D9_DRAW_VALIDATION_SCRATCH *)ir,
        owner, draw_state, pixel_binding, fixed_function_pixel, table,
        status);
    unlock_callback(lock_opaque);
    if (!snapshot_result) {
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }

    if (!QxpD3d9DrawStateValidate(
            &prepared->raw_state, &status->draw_state_error)) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE,
            status->draw_state_error.stream, 0, 0);
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }
    if (!qxp_d3d9_prepare_exact_bindings(prepared, status)) {
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }

    /* Mesa Nine establishes its missing-attribute dummy VBO as four raw
     * zero DWORDs (refs/mesa9x/mesa-25.1.x/src/gallium/frontends/nine/
     * device9.c).  Preserve that safe policy explicitly for every v#; the
     * mask says which entries are consumed as defaults. */
    qxp_d3d9_prepare_zero(
        prepared->undefined_vertex_input_defaults,
        (ULONG)sizeof(prepared->undefined_vertex_input_defaults));
    if (prepared->raw_state.vertex_function_binding.handle) {
        vertex_stage_result = qxp_d3d9_prepare_vertex_object_stage(
            prepared, vertex_bytecode, ir, status);
    } else {
        vertex_stage_result = qxp_d3d9_prepare_fixed_function_vertex_stage(
            prepared, status);
    }
    if (!vertex_stage_result) {
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }

    /* A synthesized pixel stage only reaches the host as half of a linked
     * native program, so it is admissible exactly for the modes that build
     * one.  A pre-transformed draw now synthesizes its vertex stage too, so
     * both halves may be synthesized at once. */
    if (prepared->fixed_function_pixel_signature &&
        prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
        prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
            QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL, 0, 0, 0);
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }

    if (!qxp_d3d9_prepare_streams(prepared, status)) {
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }
    qxp_d3d9_prepare_effective_constants(prepared);

    if (!qxp_d3d9_prepare_fresh(
            prepared, draw_state, pixel_binding, fixed_function_pixel, table,
            (QXP_D3D9_DRAW_VALIDATION_SCRATCH *)vertex_bytecode,
            lock_callback, unlock_callback, lock_opaque, status)) {
        qxp_d3d9_prepare_release_temporary(
            prepared, vertex_bytecode, ir, allocator, TRUE);
        return NULL;
    }
    prepared->valid = TRUE;
    if (cache) {
        qxp_d3d9_prepare_cache_store(
            cache, prepared, owner, draw_state, pixel_binding,
            fixed_function_pixel, table);
    }
    qxp_d3d9_prepare_release_temporary(
        prepared, vertex_bytecode, ir, allocator, FALSE);
    return prepared;
}

BOOL QxpD3d9ValidatePreparedDraw(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_D3D9_DRAW_PREPARED *canonical;
    BOOL raw_changed;
    BOOL pixel_changed;

    qxp_d3d9_prepare_reset_status(status);
    if (!status || !prepared || !owner || !draw_state || !pixel_binding ||
        !table || !allocator || !allocator->allocate ||
        !allocator->release || !lock_callback || !unlock_callback) {
        qxp_d3d9_prepare_set_failure(
            status, QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        return FALSE;
    }
    if (!prepared->valid ||
        (prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
         prepared->mode !=
             QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS) ||
        !qxp_shader_owner_is_set(owner) ||
        !qxp_d3d9_prepare_owner_matches(&prepared->owner, owner) ||
        !qxp_d3d9_prepare_exact_bindings(prepared, status)) {
        if (!status->code) {
            qxp_d3d9_prepare_set_failure(
                status, QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
                QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        }
        return FALSE;
    }

    /* Validation exists to catch a snapshot that no longer matches state, so
     * it must rebuild from scratch; reusing a cache here would compare a
     * value against itself. */
    canonical = QxpD3d9PrepareDrawSnapshot(
        owner, draw_state, pixel_binding, fixed_function_pixel, table,
        allocator,
        lock_callback, unlock_callback, lock_opaque, NULL, status);
    if (!canonical) {
        if (!QxpD3d9DrawPrepareStatusIsResourceFailure(status)) {
            status->code = QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED;
        }
        return FALSE;
    }
    raw_changed = !qxp_d3d9_prepare_equal(
        &prepared->raw_state, &canonical->raw_state,
        (ULONG)sizeof(prepared->raw_state));
    pixel_changed = !qxp_d3d9_prepare_equal(
        &prepared->raw_pixel_binding, &canonical->raw_pixel_binding,
        (ULONG)sizeof(prepared->raw_pixel_binding));
    if (!qxp_d3d9_prepare_equal(
            prepared, canonical, (ULONG)sizeof(*prepared))) {
        qxp_d3d9_prepare_reset_status(status);
        qxp_d3d9_prepare_set_failure(
            status, (raw_changed || pixel_changed) ?
                QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED :
                QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            QXP_D3D9_DRAW_PREPARE_STAGE_NONE, 0, 0, 0);
        QxpD3d9DestroyPreparedDraw(canonical, allocator);
        return FALSE;
    }
    QxpD3d9DestroyPreparedDraw(canonical, allocator);
    return TRUE;
}

void QxpD3d9DestroyPreparedDraw(
    QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    if (!prepared || !allocator || !allocator->release) return;
    allocator->release(prepared, allocator->opaque);
}

BOOL QxpD3d9DrawPrepareStatusIsResourceFailure(
    const QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    return status && status->code ==
        QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION;
}
