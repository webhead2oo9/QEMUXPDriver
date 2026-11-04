#include <windef.h>

#include "qxp_d3d9_draw_state.h"

static void qxp_d3d9_draw_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_d3d9_draw_copy(void *destination, const void *source,
                               ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static void qxp_d3d9_draw_reset_status(
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    if (status) qxp_d3d9_draw_zero(status, (ULONG)sizeof(*status));
}

static BOOL qxp_d3d9_draw_fail(
    QXP_D3D9_DRAW_STATE_STATUS *status, DWORD code,
    DWORD record_index, DWORD byte_offset, DWORD stage,
    DWORD constant_kind, DWORD register_index, DWORD stream)
{
    if (status) {
        status->code = code;
        status->record_index = record_index;
        status->byte_offset = byte_offset;
        status->stage = stage;
        status->constant_kind = constant_kind;
        status->register_index = register_index;
        status->stream = stream;
    }
    return FALSE;
}

static BOOL qxp_d3d9_draw_binding_valid(
    const QXP_D3D9_SHADER_BINDING *binding)
{
    if (!binding) return FALSE;
    if (!binding->handle) return binding->generation == 0;
    return binding->generation != 0;
}

static void qxp_d3d9_draw_store_binding(
    QXP_D3D9_SHADER_BINDING *destination,
    const QXP_D3D9_SHADER_BINDING *source)
{
    DWORD handle;
    ULONGLONG generation;

    handle = source->handle;
    generation = source->generation;
    qxp_d3d9_draw_zero(destination, (ULONG)sizeof(*destination));
    destination->handle = handle;
    destination->generation = generation;
}

static BOOL qxp_d3d9_draw_binding_equal(
    const QXP_D3D9_SHADER_BINDING *left,
    const QXP_D3D9_SHADER_BINDING *right)
{
    return left && right && left->handle == right->handle &&
           left->generation == right->generation;
}

static void qxp_d3d9_draw_clear_translated_declaration(
    QXP_D3D9_DRAW_STATE *state)
{
    qxp_d3d9_draw_zero(
        &state->translated_declaration_binding,
        (ULONG)sizeof(state->translated_declaration_binding));
    state->translated_declaration_fvf = 0;
    state->translated_vertex_stride = 0;
}

static BOOL qxp_d3d9_draw_constant_layout(
    DWORD stage, DWORD kind, DWORD *register_count, DWORD *element_bytes)
{
    if (!register_count || !element_bytes) return FALSE;
    *register_count = 0;
    *element_bytes = 0;
    if (stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX) {
        if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) {
            *register_count = QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT;
            *element_bytes = 4UL * sizeof(DWORD);
        } else if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) {
            *register_count = QXP_D3D9_DRAW_VS_INTEGER_CONSTANT_COUNT;
            *element_bytes = 4UL * sizeof(DWORD);
        } else if (kind == QXP_D3D9_DRAW_CONSTANT_BOOLEAN) {
            *register_count = QXP_D3D9_DRAW_VS_BOOLEAN_CONSTANT_COUNT;
            *element_bytes = sizeof(DWORD);
        }
    } else if (stage == QXP_D3D9_DRAW_SHADER_STAGE_PIXEL) {
        if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) {
            *register_count = QXP_D3D9_DRAW_PS_FLOAT_CONSTANT_COUNT;
            *element_bytes = 4UL * sizeof(DWORD);
        } else if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) {
            *register_count = QXP_D3D9_DRAW_PS_INTEGER_CONSTANT_COUNT;
            *element_bytes = 4UL * sizeof(DWORD);
        } else if (kind == QXP_D3D9_DRAW_CONSTANT_BOOLEAN) {
            *register_count = QXP_D3D9_DRAW_PS_BOOLEAN_CONSTANT_COUNT;
            *element_bytes = sizeof(DWORD);
        }
    }
    return *register_count != 0;
}

static BOOL qxp_d3d9_draw_index_valid(DWORD handle, DWORD stride)
{
    if (!handle) return stride == 0;
    return stride == 2 || stride == 4;
}

static BOOL qxp_d3d9_draw_frequency_valid(DWORD stream, DWORD divider)
{
    DWORD flags;
    DWORD divisor;

    if (stream >= QXP_D3D9_DRAW_STREAM_COUNT) return FALSE;
    flags = divider & QXP_D3D9_STREAM_SOURCE_FLAG_MASK;
    divisor = divider & QXP_D3D9_STREAM_SOURCE_DIVISOR_MASK;
    /* Native D3D9 accepts a high-bit flag with a zero low divisor and
     * accepts INDEXEDDATA on a nonzero stream.  The documented stream-zero
     * rule describes indexed-instancing usage, not API validation. */
    if (!divider || flags == QXP_D3D9_STREAM_SOURCE_FLAG_MASK)
        return FALSE;
    if (flags == QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA)
        return stream != 0;
    if (flags == QXP_D3D9_STREAM_SOURCE_INDEXED_DATA)
        return TRUE;
    return flags == 0 && divisor != 0;
}

static void qxp_d3d9_draw_mark_range(DWORD *mask, DWORD start, DWORD count)
{
    DWORD index;
    DWORD register_index;

    for (index = 0; index < count; ++index) {
        register_index = start + index;
        mask[register_index / 32UL] |=
            1UL << (register_index & 31UL);
    }
}

static void qxp_d3d9_draw_apply_constants(
    QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD kind,
    DWORD register_index, DWORD count, const void *values)
{
    ULONG byte_count;

    byte_count = count * (kind == QXP_D3D9_DRAW_CONSTANT_BOOLEAN ?
                          sizeof(DWORD) : 4UL * sizeof(DWORD));
    if (stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX) {
        if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) {
            qxp_d3d9_draw_copy(
                state->vs_float[register_index], values, byte_count);
            qxp_d3d9_draw_mark_range(
                state->vs_float_set_mask, register_index, count);
        } else if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) {
            qxp_d3d9_draw_copy(
                state->vs_integer[register_index], values, byte_count);
            qxp_d3d9_draw_mark_range(
                &state->vs_integer_set_mask, register_index, count);
        } else {
            qxp_d3d9_draw_copy(
                &state->vs_boolean[register_index], values, byte_count);
            qxp_d3d9_draw_mark_range(
                &state->vs_boolean_set_mask, register_index, count);
        }
    } else if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) {
        qxp_d3d9_draw_copy(
            state->ps_float[register_index], values, byte_count);
        qxp_d3d9_draw_mark_range(
            &state->ps_float_set_mask, register_index, count);
    } else if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) {
        qxp_d3d9_draw_copy(
            state->ps_integer[register_index], values, byte_count);
        qxp_d3d9_draw_mark_range(
            &state->ps_integer_set_mask, register_index, count);
    } else {
        qxp_d3d9_draw_copy(
            &state->ps_boolean[register_index], values, byte_count);
        qxp_d3d9_draw_mark_range(
            &state->ps_boolean_set_mask, register_index, count);
    }
}

static BOOL qxp_d3d9_draw_validate_constant(
    DWORD stage, DWORD kind, DWORD register_index, DWORD count,
    QXP_D3D9_DRAW_STATE_STATUS *status, DWORD record_index,
    DWORD byte_offset)
{
    DWORD register_count;
    DWORD element_bytes;

    if (stage != QXP_D3D9_DRAW_SHADER_STAGE_VERTEX &&
        stage != QXP_D3D9_DRAW_SHADER_STAGE_PIXEL) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STAGE,
            record_index, byte_offset, stage, kind, register_index, 0);
    }
    if (!qxp_d3d9_draw_constant_layout(
            stage, kind, &register_count, &element_bytes)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_KIND,
            record_index, byte_offset, stage, kind, register_index, 0);
    }
    if (!count) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_COUNT,
            record_index, byte_offset, stage, kind, register_index, 0);
    }
    if (register_index >= register_count ||
        count > register_count - register_index) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_RANGE,
            record_index, byte_offset, stage, kind, register_index, 0);
    }
    (void)element_bytes;
    return TRUE;
}

void QxpD3d9DrawStateInitialize(QXP_D3D9_DRAW_STATE *state)
{
    DWORD stream;

    if (!state) return;
    qxp_d3d9_draw_zero(state, (ULONG)sizeof(*state));
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream)
        state->streams[stream].divider = 1;
}

static BOOL qxp_d3d9_draw_classify_object_bindings(
    const QXP_D3D9_DRAW_STATE *state, DWORD *mode)
{
    if (!state || !mode ||
        !qxp_d3d9_draw_binding_valid(&state->declaration_binding) ||
        !qxp_d3d9_draw_binding_valid(
            &state->vertex_function_binding)) return FALSE;
    if (state->declaration_binding.handle) {
        if (state->legacy_fvf ||
            !(state->declaration_binding.handle &
              QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT)) return FALSE;
        *mode = QXP_D3D9_DRAW_DECLARATION_OBJECT;
    } else if (state->legacy_fvf) {
        if (state->legacy_fvf &
            QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) return FALSE;
        *mode = QXP_D3D9_DRAW_DECLARATION_FVF;
    } else {
        *mode = QXP_D3D9_DRAW_DECLARATION_NONE;
    }
    return TRUE;
}

BOOL QxpD3d9DrawStateSynchronizeObjectBindings(
    QXP_D3D9_DRAW_STATE *state, QXP_D3D9_DRAW_STATE_STATUS *status)
{
    DWORD mode;

    qxp_d3d9_draw_reset_status(status);
    if (!state) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_classify_object_bindings(state, &mode)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
            0, 0, 0, 0, 0, 0);
    }
    state->declaration_mode = mode;
    if (mode != QXP_D3D9_DRAW_DECLARATION_OBJECT ||
        !qxp_d3d9_draw_binding_equal(
            &state->translated_declaration_binding,
            &state->declaration_binding)) {
        qxp_d3d9_draw_clear_translated_declaration(state);
    }
    return TRUE;
}

BOOL QxpD3d9DrawStateValidate(
    const QXP_D3D9_DRAW_STATE *state,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    DWORD mode;
    DWORD stream;

    qxp_d3d9_draw_reset_status(status);
    if (!state) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_binding_valid(&state->declaration_binding)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_binding_valid(
            &state->vertex_function_binding)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_FUNCTION,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_classify_object_bindings(state, &mode) ||
        state->declaration_mode != mode) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
            0, 0, 0, 0, 0, 0);
    }
    if (state->translated_declaration_binding.handle ||
        state->translated_declaration_binding.generation ||
        state->translated_declaration_fvf ||
        state->translated_vertex_stride) {
        if (!qxp_d3d9_draw_binding_valid(
                &state->translated_declaration_binding) ||
            !state->translated_declaration_binding.handle ||
            state->declaration_mode !=
                QXP_D3D9_DRAW_DECLARATION_OBJECT ||
            !qxp_d3d9_draw_binding_equal(
                &state->translated_declaration_binding,
                &state->declaration_binding) ||
            !state->translated_declaration_fvf ||
            (state->translated_declaration_fvf &
             QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) ||
            !state->translated_vertex_stride) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
                0, 0, 0, 0, 0, 0);
        }
    }
    if (!qxp_d3d9_draw_index_valid(
            state->indices.handle, state->indices.stride)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE,
            0, 0, 0, 0, 0, 0);
    }
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        const QXP_D3D9_DRAW_STREAM_STATE *source;

        source = &state->streams[stream];
        if (source->user_memory != FALSE &&
            source->user_memory != TRUE) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                0, 0, 0, 0, 0, stream);
        }
        if (source->source2 != FALSE && source->source2 != TRUE) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                0, 0, 0, 0, 0, stream);
        }
        if (!qxp_d3d9_draw_frequency_valid(stream, source->divider)) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY,
                0, 0, 0, 0, 0, stream);
        }
        if (source->user_memory &&
            (stream != 0 || source->handle || source->offset ||
             source->source2)) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                0, 0, 0, 0, 0, stream);
        }
        if (!source->source2 && source->offset) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                0, 0, 0, 0, 0, stream);
        }
    }
    return TRUE;
}
DWORD QxpD3d9ResolveLegacyFvf(
    const QXP_D3D9_DRAW_STATE *state, DWORD persistent_fvf,
    DWORD call_fvf)
{
    if (!state) return 0;
    if (state->declaration_mode == QXP_D3D9_DRAW_DECLARATION_OBJECT) {
        if (state->translated_declaration_binding.handle !=
                state->declaration_binding.handle ||
            state->translated_declaration_binding.generation !=
                state->declaration_binding.generation ||
            !state->translated_vertex_stride) return 0;
        return state->translated_declaration_fvf;
    }
    if (call_fvf) return call_fvf;
    if (state->declaration_mode == QXP_D3D9_DRAW_DECLARATION_FVF)
        return state->legacy_fvf;
    return persistent_fvf;
}

BOOL QxpD3d9BuildCallDrawState(
    const QXP_D3D9_DRAW_STATE *source, DWORD vertex_fvf,
    DWORD vertex_stride, QXP_D3D9_DRAW_STATE *destination,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!source || !destination) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!QxpD3d9DrawStateValidate(source, status)) return FALSE;
    /* SETSTREAMSOURCEUM is the source authority.  The outer
     * USERMEMVERTICES flag says only that lpVertices is a valid union member;
     * it does not replace a persistent vertex-buffer binding. */
    if (source->streams[0].user_memory &&
        (!vertex_stride ||
         vertex_stride != source->streams[0].stride)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
            0, 0, 0, 0, 0, 0);
    }
    if (source->declaration_mode != QXP_D3D9_DRAW_DECLARATION_OBJECT &&
        vertex_fvf &
            QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
            0, 0, 0, 0, 0, 0);
    }

    qxp_d3d9_draw_copy(
        destination, source, (ULONG)sizeof(*destination));
    /* D3DNTHAL_DRAWPRIMITIVES2DATA.dwVertexType is the FVF for this call.
     * A DDI-v4 declaration object remains authoritative; otherwise the
     * call-scoped FVF supplies the declaration omitted from persistent DP2
     * state without leaking into the next call. */
    if (source->declaration_mode != QXP_D3D9_DRAW_DECLARATION_OBJECT &&
        vertex_fvf) {
        destination->declaration_mode =
            QXP_D3D9_DRAW_DECLARATION_FVF;
        qxp_d3d9_draw_zero(
            &destination->declaration_binding,
            (ULONG)sizeof(destination->declaration_binding));
        destination->legacy_fvf = vertex_fvf;
        qxp_d3d9_draw_clear_translated_declaration(destination);
    }
    return QxpD3d9DrawStateValidate(destination, status);
}

BOOL QxpD3d9DrawStateRequiresUnsupportedExecution(
    const QXP_D3D9_DRAW_STATE *state)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    if (!QxpD3d9DrawStateValidate(state, &status)) return TRUE;
    /* Legacy FVF execution, including translated declaration objects, can
     * consume only stream 0. State retained on any other stream is dormant
     * for that execution and must not force the native/capture path. */
    if (state->streams[0].divider != 1) return TRUE;
    /* Only the fixed-function stream-0 capture consumes SOURCE2 today.
     * Declaration-less execution can select older programmable paths, so it
     * remains fail-closed even for stream 0. */
    if (state->streams[0].source2 &&
        state->declaration_mode == QXP_D3D9_DRAW_DECLARATION_NONE)
        return TRUE;
    /* Direct3D bypasses the vertex shader outright for a pre-transformed
     * declaration, so a bound vertex-function handle is dormant for that
     * draw exactly as non-zero state on an unused stream is.  Treating it
     * as live forces a native path that has no pre-transformed route and
     * refuses the draw, when the legacy FVF path already draws XYZRHW
     * vertices.  Only a translated declaration qualifies: the translation
     * is what proves the position is pre-transformed and gives the stride
     * the legacy path consumes. */
    if (state->vertex_function_binding.handle != 0 &&
        (state->declaration_mode != QXP_D3D9_DRAW_DECLARATION_OBJECT ||
         !state->translated_declaration_fvf ||
         !state->translated_vertex_stride ||
         (state->translated_declaration_fvf &
          QXP_D3D9_DRAW_FVF_POSITION_MASK) !=
             QXP_D3D9_DRAW_FVF_XYZRHW))
        return TRUE;
    if (state->declaration_mode == QXP_D3D9_DRAW_DECLARATION_NONE ||
        state->declaration_mode == QXP_D3D9_DRAW_DECLARATION_FVF)
        return FALSE;
    return !state->translated_declaration_fvf ||
           !state->translated_vertex_stride;
}

BOOL QxpD3d9DrawStateStatusIsResourceFailure(
    const QXP_D3D9_DRAW_STATE_STATUS *status)
{
    return status && status->code == QXP_D3D9_DRAW_STATE_ERROR_RESOURCE;
}

BOOL QxpD3d9DrawStateSetDeclaration(
    QXP_D3D9_DRAW_STATE *state, DWORD mode,
    const QXP_D3D9_SHADER_BINDING *binding, DWORD legacy_fvf,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state || !binding) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_binding_valid(binding)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
            0, 0, 0, 0, 0, 0);
    }
    if (mode == QXP_D3D9_DRAW_DECLARATION_NONE) {
        if (binding->handle || legacy_fvf) goto invalid;
    } else if (mode == QXP_D3D9_DRAW_DECLARATION_FVF) {
        if (binding->handle || !legacy_fvf ||
            (legacy_fvf &
             QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT)) goto invalid;
    } else if (mode == QXP_D3D9_DRAW_DECLARATION_OBJECT) {
        if (!binding->handle ||
            !(binding->handle &
              QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) ||
            legacy_fvf) goto invalid;
    } else {
        goto invalid;
    }
    state->declaration_mode = mode;
    qxp_d3d9_draw_store_binding(
        &state->declaration_binding, binding);
    state->legacy_fvf = legacy_fvf;
    qxp_d3d9_draw_clear_translated_declaration(state);
    return TRUE;

invalid:
    return qxp_d3d9_draw_fail(
        status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
        0, 0, 0, 0, 0, 0);
}

BOOL QxpD3d9DrawStateSetTranslatedDeclaration(
    QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_SHADER_BINDING *binding,
    DWORD translated_fvf, DWORD translated_vertex_stride,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state || !binding) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!translated_fvf) {
        if (binding->handle || binding->generation ||
            translated_vertex_stride) goto invalid;
        qxp_d3d9_draw_clear_translated_declaration(state);
        return TRUE;
    }
    if (!qxp_d3d9_draw_binding_valid(binding) || !binding->handle ||
        !(binding->handle &
          QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) ||
        (translated_fvf &
         QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) ||
        !translated_vertex_stride || state->legacy_fvf ||
        !qxp_d3d9_draw_binding_equal(
            binding, &state->declaration_binding)) goto invalid;

    qxp_d3d9_draw_store_binding(
        &state->translated_declaration_binding, binding);
    state->translated_declaration_fvf = translated_fvf;
    state->translated_vertex_stride = translated_vertex_stride;
    return TRUE;

invalid:
    return qxp_d3d9_draw_fail(
        status, QXP_D3D9_DRAW_STATE_ERROR_DECLARATION,
        0, 0, 0, 0, 0, 0);
}

BOOL QxpD3d9DrawStateSetVertexFunction(
    QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state || !binding) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_binding_valid(binding)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_FUNCTION,
            0, 0, 0, 0, 0, 0);
    }
    qxp_d3d9_draw_store_binding(
        &state->vertex_function_binding, binding);
    return TRUE;
}

BOOL QxpD3d9DrawStateSetStreamSource(
    QXP_D3D9_DRAW_STATE *state, DWORD stream, DWORD handle,
    DWORD offset, DWORD stride, BOOL user_memory,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, stream);
    }
    if (stream >= QXP_D3D9_DRAW_STREAM_COUNT) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM,
            0, 0, 0, 0, 0, stream);
    }
    if (user_memory && (stream != 0 || handle || offset)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
            0, 0, 0, 0, 0, stream);
    }
    if (state->stream_source_generation == ~(ULONGLONG)0) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED,
            0, 0, 0, 0, 0, stream);
    }
    state->streams[stream].handle = handle;
    state->streams[stream].offset = offset;
    state->streams[stream].stride = stride;
    state->streams[stream].user_memory = user_memory ? TRUE : FALSE;
    state->streams[stream].source2 = offset != 0;
    ++state->stream_source_generation;
    return TRUE;
}

BOOL QxpD3d9DrawStateSetStreamFrequency(
    QXP_D3D9_DRAW_STATE *state, DWORD stream, DWORD divider,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, stream);
    }
    if (stream >= QXP_D3D9_DRAW_STREAM_COUNT) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM,
            0, 0, 0, 0, 0, stream);
    }
    if (!qxp_d3d9_draw_frequency_valid(stream, divider)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY,
            0, 0, 0, 0, 0, stream);
    }
    state->streams[stream].divider = divider;
    return TRUE;
}

BOOL QxpD3d9DrawStateSetIndices(
    QXP_D3D9_DRAW_STATE *state, DWORD handle, DWORD stride,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (handle && stride != 2 && stride != 4) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE,
            0, 0, 0, 0, 0, 0);
    }
    if (state->stream_source_generation == ~(ULONGLONG)0) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED,
            0, 0, 0, 0, 0, 0);
    }
    state->indices.handle = handle;
    state->indices.stride = handle ? stride : 0;
    ++state->stream_source_generation;
    return TRUE;
}

BOOL QxpD3d9DrawStateSetConstants(
    QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD constant_kind,
    DWORD register_index, DWORD count, const void *values,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    qxp_d3d9_draw_reset_status(status);
    if (!state || !values) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, stage, constant_kind, register_index, 0);
    }
    if (!qxp_d3d9_draw_validate_constant(
            stage, constant_kind, register_index, count,
            status, 0, 0)) return FALSE;
    qxp_d3d9_draw_apply_constants(
        state, stage, constant_kind, register_index, count, values);
    return TRUE;
}

BOOL QxpD3d9SharedFloatRangeFitsLegacy(
    DWORD stage, DWORD register_index, DWORD count, DWORD legacy_count)
{
    DWORD register_count;
    DWORD element_bytes;

    if (!qxp_d3d9_draw_constant_layout(
            stage, QXP_D3D9_DRAW_CONSTANT_FLOAT,
            &register_count, &element_bytes)) return FALSE;
    if (!count || register_index >= register_count ||
        count > register_count - register_index) return FALSE;
    if (register_index >= legacy_count ||
        count > legacy_count - register_index) return FALSE;
    (void)element_bytes;
    return TRUE;
}

BOOL QxpD3d9PublishSharedFloatConstants(
    QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD register_index,
    DWORD count, const void *values, DWORD (*legacy_values)[4],
    DWORD legacy_count, QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    DWORD legacy_copy_count;

    qxp_d3d9_draw_reset_status(status);
    if (!state || !values || !lock_callback || !unlock_callback ||
        (legacy_count && !legacy_values)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, stage, QXP_D3D9_DRAW_CONSTANT_FLOAT,
            register_index, 0);
    }
    if (!qxp_d3d9_draw_validate_constant(
            stage, QXP_D3D9_DRAW_CONSTANT_FLOAT, register_index, count,
            status, 0, 0)) return FALSE;
    legacy_copy_count = 0;
    if (register_index < legacy_count) {
        legacy_copy_count = count;
        if (legacy_copy_count > legacy_count - register_index)
            legacy_copy_count = legacy_count - register_index;
    }

    lock_callback(lock_opaque);
    qxp_d3d9_draw_apply_constants(
        state, stage, QXP_D3D9_DRAW_CONSTANT_FLOAT,
        register_index, count, values);
    if (legacy_copy_count) {
        qxp_d3d9_draw_copy(
            legacy_values[register_index], values,
            legacy_copy_count * 4UL * sizeof(DWORD));
    }
    unlock_callback(lock_opaque);
    return TRUE;
}

BOOL QxpD3d9DispatchSharedFloatBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    DWORD (*legacy_values)[4], DWORD legacy_count,
    BOOL state_block_recording,
    QXP_D3D9_SHARED_FLOAT_RECORD_CALLBACK record_callback,
    void *record_opaque,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_DP2_CONSTANT constant;
    const BYTE *bytes;
    DWORD stage;
    DWORD record_index;
    DWORD legacy_copy_count;
    DWORD constant_index;
    ULONG offset;
    ULONG record_offset;
    ULONG payload_bytes;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_draw_reset_status(status);
    if (!consumed_bytes || !error_offset) {
        if (error_offset) *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF) {
        stage = QXP_D3D9_DRAW_SHADER_STAGE_VERTEX;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF) {
        stage = QXP_D3D9_DRAW_SHADER_STAGE_PIXEL;
    } else {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_OPCODE,
            0, 0, 0, 0, 0, 0);
    }
    if (!state || !lock_callback || !unlock_callback ||
        (legacy_count && !legacy_values) ||
        (record_count && !records)) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, stage, QXP_D3D9_DRAW_CONSTANT_FLOAT, 0, 0);
    }
    if (!record_count) return TRUE;

    bytes = (const BYTE *)records;
    offset = 0;
    for (record_index = 0; record_index < record_count; ++record_index) {
        record_offset = offset;
        if (offset > available_bytes ||
            available_bytes - offset < sizeof(constant)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD,
                record_index, record_offset, stage,
                QXP_D3D9_DRAW_CONSTANT_FLOAT, 0, 0);
        }
        qxp_d3d9_draw_copy(&constant, bytes + offset, sizeof(constant));
        offset += sizeof(constant);
        if (!qxp_d3d9_draw_validate_constant(
                stage, QXP_D3D9_DRAW_CONSTANT_FLOAT,
                constant.register_index, constant.count,
                status, record_index, record_offset)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return FALSE;
        }
        if (state_block_recording &&
            !QxpD3d9SharedFloatRangeFitsLegacy(
                stage, constant.register_index, constant.count,
                legacy_count)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK,
                record_index, record_offset, stage,
                QXP_D3D9_DRAW_CONSTANT_FLOAT,
                constant.register_index, 0);
        }
        payload_bytes = constant.count * 4UL * sizeof(DWORD);
        if (offset > available_bytes ||
            available_bytes - offset < payload_bytes) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD,
                record_index, record_offset, stage,
                QXP_D3D9_DRAW_CONSTANT_FLOAT,
                constant.register_index, 0);
        }
        if (state_block_recording && !record_callback) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK,
                record_index, record_offset, stage,
                QXP_D3D9_DRAW_CONSTANT_FLOAT,
                constant.register_index, 0);
        }
        if (!state_block_recording &&
            !QxpD3d9PublishSharedFloatConstants(
                state, stage, constant.register_index, constant.count,
                bytes + offset, legacy_values, legacy_count,
                lock_callback, unlock_callback, lock_opaque, status)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return FALSE;
        }
        legacy_copy_count = 0;
        if (constant.register_index < legacy_count) {
            legacy_copy_count = constant.count;
            if (legacy_copy_count >
                    legacy_count - constant.register_index) {
                legacy_copy_count =
                    legacy_count - constant.register_index;
            }
        }
        for (constant_index = 0;
             state_block_recording && record_callback &&
             constant_index < legacy_copy_count;
             ++constant_index) {
            if (!record_callback(
                    constant.register_index + constant_index,
                    (const DWORD *)(bytes + offset) +
                        constant_index * 4UL,
                    record_opaque)) {
                *consumed_bytes = record_offset;
                *error_offset = command_offset;
                return qxp_d3d9_draw_fail(
                    status, QXP_D3D9_DRAW_STATE_ERROR_RESOURCE,
                    record_index, record_offset, stage,
                    QXP_D3D9_DRAW_CONSTANT_FLOAT,
                    constant.register_index + constant_index, 0);
            }
        }
        offset += payload_bytes;
        *consumed_bytes = offset;
    }
    return TRUE;
}

BOOL QxpD3d9DispatchSharedResourceBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_LEGACY_RESOURCE_VIEW *legacy,
    BOOL state_block_recording,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_STREAM_STATE
        stream_updates[QXP_D3D9_DRAW_STREAM_COUNT];
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE source;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM source_um;
    QXP_D3D9_DRAW_DP2_INDICES index_source;
    QXP_D3D9_DRAW_INDEX_STATE index_update;
    const BYTE *bytes;
    DWORD fixed_size;
    DWORD record_index;
    DWORD stream;
    DWORD stream_mask;
    DWORD index_touched;
    ULONGLONG snapshot_generation;
    ULONG total_bytes;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_draw_reset_status(status);
    if (!consumed_bytes || !error_offset) {
        if (error_offset) *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE) {
        fixed_size = sizeof(source);
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM) {
        fixed_size = sizeof(source_um);
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_INDICES) {
        fixed_size = sizeof(index_source);
    } else {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_OPCODE,
            0, 0, 0, 0, 0, 0);
    }
    if (!state || !legacy || !legacy->stream_handles ||
        !legacy->stream_strides ||
        legacy->stream_count < QXP_D3D9_DRAW_STREAM_COUNT ||
        !legacy->stream0_user_memory || !legacy->index_handle ||
        !legacy->index_stride || !lock_callback || !unlock_callback ||
        (record_count && !records)) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!record_count) return TRUE;
    if (record_count > available_bytes / fixed_size) {
        record_index = available_bytes / fixed_size;
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD,
            record_index, record_index * fixed_size, 0, 0, 0, 0);
    }
    total_bytes = record_count * fixed_size;

    lock_callback(lock_opaque);
    snapshot_generation = state->stream_source_generation;
    unlock_callback(lock_opaque);

    qxp_d3d9_draw_zero(stream_updates,
                        (ULONG)sizeof(stream_updates));
    qxp_d3d9_draw_zero(&index_update, (ULONG)sizeof(index_update));
    qxp_d3d9_draw_zero(&source, (ULONG)sizeof(source));
    qxp_d3d9_draw_zero(&source_um, (ULONG)sizeof(source_um));
    qxp_d3d9_draw_zero(&index_source, (ULONG)sizeof(index_source));
    bytes = (const BYTE *)records;
    stream_mask = 0;
    index_touched = FALSE;
    for (record_index = 0; record_index < record_count; ++record_index) {
        if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE) {
            qxp_d3d9_draw_copy(
                &source, bytes + record_index * fixed_size,
                sizeof(source));
            stream = source.stream;
            if (stream >= QXP_D3D9_DRAW_STREAM_COUNT) {
                *error_offset = command_offset;
                return qxp_d3d9_draw_fail(
                    status, QXP_D3D9_DRAW_STATE_ERROR_STREAM,
                    record_index, record_index * fixed_size,
                    0, 0, 0, stream);
            }
            stream_updates[stream].handle = source.handle;
            stream_updates[stream].offset = 0;
            stream_updates[stream].stride = source.stride;
            stream_updates[stream].user_memory = FALSE;
            stream_updates[stream].source2 = FALSE;
            stream_mask |= 1UL << stream;
        } else if (opcode ==
                   QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM) {
            qxp_d3d9_draw_copy(
                &source_um, bytes + record_index * fixed_size,
                sizeof(source_um));
            if (source_um.stream != 0) {
                *error_offset = command_offset;
                return qxp_d3d9_draw_fail(
                    status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                    record_index, record_index * fixed_size,
                    0, 0, 0, source_um.stream);
            }
            stream_updates[0].handle = 0;
            stream_updates[0].offset = 0;
            stream_updates[0].stride = source_um.stride;
            stream_updates[0].user_memory = TRUE;
            stream_updates[0].source2 = FALSE;
            stream_mask |= 1UL;
        } else {
            qxp_d3d9_draw_copy(
                &index_source, bytes + record_index * fixed_size,
                sizeof(index_source));
            if (index_source.handle && index_source.stride != 2 &&
                index_source.stride != 4) {
                *error_offset = command_offset;
                return qxp_d3d9_draw_fail(
                    status, QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE,
                    record_index, record_index * fixed_size,
                    0, 0, 0, 0);
            }
            index_update.handle = index_source.handle;
            index_update.stride =
                index_source.handle ? index_source.stride : 0;
            index_touched = TRUE;
        }
    }

    lock_callback(lock_opaque);
    if (state->stream_source_generation != snapshot_generation) {
        unlock_callback(lock_opaque);
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STATE_CHANGED,
            0, 0, 0, 0, 0, 0);
    }
    if (state->stream_source_generation == ~(ULONGLONG)0) {
        unlock_callback(lock_opaque);
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED,
            0, 0, 0, 0, 0, 0);
    }
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        if (!(stream_mask & (1UL << stream))) continue;
        state->streams[stream].handle = stream_updates[stream].handle;
        state->streams[stream].offset = 0;
        state->streams[stream].stride = stream_updates[stream].stride;
        state->streams[stream].user_memory =
            stream_updates[stream].user_memory;
        state->streams[stream].source2 = FALSE;
        legacy->stream_handles[stream] = stream_updates[stream].handle;
        legacy->stream_strides[stream] = stream_updates[stream].stride;
        if (stream == 0) {
            *legacy->stream0_user_memory =
                stream_updates[stream].user_memory;
        }
    }
    if (index_touched) {
        state->indices = index_update;
        *legacy->index_handle = index_update.handle;
        *legacy->index_stride = index_update.stride;
    }
    ++state->stream_source_generation;
    unlock_callback(lock_opaque);
    *consumed_bytes = total_bytes;
    (void)state_block_recording;
    return TRUE;
}

void QxpD3d9DrawStateUnbindSurface(
    QXP_D3D9_DRAW_STATE *state, DWORD surface_handle)
{
    DWORD stream;

    if (!state || !surface_handle) return;
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        if (state->streams[stream].handle != surface_handle) continue;
        state->streams[stream].handle = 0;
        state->streams[stream].offset = 0;
        state->streams[stream].stride = 0;
        state->streams[stream].user_memory = FALSE;
        state->streams[stream].source2 = FALSE;
    }
    if (state->indices.handle == surface_handle) {
        state->indices.handle = 0;
        state->indices.stride = 0;
    }
    /* This call is also the same-owner surface-destroy notification.  Advance
     * even when the handle was not previously bound so a first-time SOURCE2
     * candidate cannot publish that handle after its surface was removed. */
    if (state->stream_source_generation != ~(ULONGLONG)0)
        ++state->stream_source_generation;
}

static BOOL qxp_d3d9_draw_opcode_layout(
    BYTE opcode, DWORD *stage, DWORD *kind, DWORD *fixed_size)
{
    if (!stage || !kind || !fixed_size) return FALSE;
    *stage = 0;
    *kind = 0;
    *fixed_size = 0;
    switch (opcode) {
    case QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_VERTEX;
        *kind = QXP_D3D9_DRAW_CONSTANT_FLOAT;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_PIXEL;
        *kind = QXP_D3D9_DRAW_CONSTANT_FLOAT;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_VERTEX;
        *kind = QXP_D3D9_DRAW_CONSTANT_INTEGER;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_VERTEX;
        *kind = QXP_D3D9_DRAW_CONSTANT_BOOLEAN;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_PIXEL;
        *kind = QXP_D3D9_DRAW_CONSTANT_INTEGER;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB:
        *stage = QXP_D3D9_DRAW_SHADER_STAGE_PIXEL;
        *kind = QXP_D3D9_DRAW_CONSTANT_BOOLEAN;
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE:
        *fixed_size = sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE);
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM:
        *fixed_size = sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM);
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_INDICES:
        *fixed_size = sizeof(QXP_D3D9_DRAW_DP2_INDICES);
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2:
        *fixed_size = sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE2);
        break;
    case QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ:
        *fixed_size = sizeof(QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY);
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_d3d9_draw_decode_fixed_record(
    BYTE opcode, const BYTE *record, DWORD record_index,
    DWORD byte_offset, QXP_D3D9_DRAW_STREAM_STATE *streams,
    QXP_D3D9_DRAW_INDEX_STATE *indices,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE source;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM source_um;
    QXP_D3D9_DRAW_DP2_INDICES index_source;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE2 source2;
    QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY frequency;
    DWORD stream;
    DWORD divider;

    qxp_d3d9_draw_zero(&source, (ULONG)sizeof(source));
    qxp_d3d9_draw_zero(&source_um, (ULONG)sizeof(source_um));
    qxp_d3d9_draw_zero(&index_source, (ULONG)sizeof(index_source));
    qxp_d3d9_draw_zero(&source2, (ULONG)sizeof(source2));
    qxp_d3d9_draw_zero(&frequency, (ULONG)sizeof(frequency));
    stream = 0;
    divider = 1;
    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE) {
        qxp_d3d9_draw_copy(&source, record, sizeof(source));
        stream = source.stream;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM) {
        qxp_d3d9_draw_copy(&source_um, record, sizeof(source_um));
        stream = source_um.stream;
        if (stream != 0) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE,
                record_index, byte_offset, 0, 0, 0, stream);
        }
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_INDICES) {
        qxp_d3d9_draw_copy(&index_source, record, sizeof(index_source));
        if (index_source.handle && index_source.stride != 2 &&
            index_source.stride != 4) {
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE,
                record_index, byte_offset, 0, 0, 0, 0);
        }
        indices->handle = index_source.handle;
        indices->stride = index_source.handle ? index_source.stride : 0;
        return TRUE;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2) {
        qxp_d3d9_draw_copy(&source2, record, sizeof(source2));
        stream = source2.stream;
    } else {
        qxp_d3d9_draw_copy(&frequency, record, sizeof(frequency));
        stream = frequency.stream;
        divider = frequency.divider;
    }
    if (stream >= QXP_D3D9_DRAW_STREAM_COUNT) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM,
            record_index, byte_offset, 0, 0, 0, stream);
    }
    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ &&
        !qxp_d3d9_draw_frequency_valid(stream, divider)) {
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY,
            record_index, byte_offset, 0, 0, 0, stream);
    }
    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE) {
        streams[stream].handle = source.handle;
        streams[stream].offset = 0;
        streams[stream].stride = source.stride;
        streams[stream].user_memory = FALSE;
        streams[stream].source2 = FALSE;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM) {
        streams[0].handle = 0;
        streams[0].offset = 0;
        streams[0].stride = source_um.stride;
        streams[0].user_memory = TRUE;
        streams[0].source2 = FALSE;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2) {
        streams[stream].handle = source2.handle;
        streams[stream].offset = source2.offset;
        streams[stream].stride = source2.stride;
        streams[stream].user_memory = FALSE;
        streams[stream].source2 = TRUE;
    } else {
        streams[stream].divider = divider;
    }
    return TRUE;
}

BOOL QxpD3d9ProcessDrawStateBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_DP2_CONSTANT constant;
    const BYTE *bytes;
    DWORD stage;
    DWORD kind;
    DWORD fixed_size;
    DWORD register_count;
    DWORD element_bytes;
    DWORD record_index;
    QXP_D3D9_DRAW_STREAM_STATE
        candidate_streams[QXP_D3D9_DRAW_STREAM_COUNT];
    QXP_D3D9_DRAW_INDEX_STATE candidate_indices;
    ULONG offset;
    ULONG record_offset;
    ULONG payload_bytes;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_draw_reset_status(status);
    if (!state || !consumed_bytes || !error_offset ||
        (record_count && !records)) {
        if (error_offset) *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_opcode_layout(
            opcode, &stage, &kind, &fixed_size)) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_OPCODE,
            0, 0, 0, 0, 0, 0);
    }
    if (!record_count) return TRUE;
    bytes = (const BYTE *)records;
    offset = 0;

    if (fixed_size) {
        qxp_d3d9_draw_copy(
            candidate_streams, state->streams,
            (ULONG)sizeof(candidate_streams));
        candidate_indices = state->indices;
        for (record_index = 0; record_index < record_count;
             ++record_index) {
            if (offset > available_bytes ||
                available_bytes - offset < fixed_size) {
                *error_offset = command_offset;
                return qxp_d3d9_draw_fail(
                    status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD,
                    record_index, offset, 0, 0, 0, 0);
            }
            if (!qxp_d3d9_draw_decode_fixed_record(
                    opcode, bytes + offset, record_index, offset,
                    candidate_streams, &candidate_indices, status)) {
                *error_offset = command_offset;
                return FALSE;
            }
            offset += fixed_size;
        }
        if (opcode != QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ &&
            state->stream_source_generation == ~(ULONGLONG)0) {
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED,
                0, 0, 0, 0, 0, 0);
        }
        qxp_d3d9_draw_copy(
            state->streams, candidate_streams,
            (ULONG)sizeof(candidate_streams));
        state->indices = candidate_indices;
        if (opcode != QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ)
            ++state->stream_source_generation;
        *consumed_bytes = offset;
        return TRUE;
    }

    qxp_d3d9_draw_constant_layout(
        stage, kind, &register_count, &element_bytes);
    for (record_index = 0; record_index < record_count; ++record_index) {
        record_offset = offset;
        if (offset > available_bytes ||
            available_bytes - offset < sizeof(constant)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD,
                record_index, record_offset, stage, kind, 0, 0);
        }
        qxp_d3d9_draw_copy(
            &constant, bytes + offset, sizeof(constant));
        offset += sizeof(constant);
        if (!qxp_d3d9_draw_validate_constant(
                stage, kind, constant.register_index, constant.count,
                status, record_index, record_offset)) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return FALSE;
        }
        payload_bytes = constant.count * element_bytes;
        if (offset > available_bytes ||
            available_bytes - offset < payload_bytes) {
            *consumed_bytes = record_offset;
            *error_offset = command_offset;
            return qxp_d3d9_draw_fail(
                status, QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD,
                record_index, record_offset, stage, kind,
                constant.register_index, 0);
        }
        qxp_d3d9_draw_apply_constants(
            state, stage, kind, constant.register_index,
            constant.count, bytes + offset);
        offset += payload_bytes;
        *consumed_bytes = offset;
    }
    (void)register_count;
    return TRUE;
}

static BOOL qxp_d3d9_draw_dispatch_opcode_valid(BYTE opcode)
{
    return opcode == QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI ||
           opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2 ||
           opcode == QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB ||
           opcode == QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI ||
           opcode == QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB ||
           opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ;
}

static DWORD qxp_d3d9_draw_stream_mask(
    BYTE opcode, DWORD record_count, const void *records)
{
    const BYTE *bytes;
    DWORD fixed_size;
    DWORD index;
    DWORD stream;
    DWORD mask;

    bytes = (const BYTE *)records;
    fixed_size = opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2 ?
        sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE2) :
        sizeof(QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY);
    mask = 0;
    for (index = 0; index < record_count; ++index) {
        qxp_d3d9_draw_copy(&stream, bytes + index * fixed_size,
                           sizeof(stream));
        mask |= 1UL << stream;
    }
    return mask;
}

static BOOL qxp_d3d9_draw_publish_candidate(
    BYTE opcode, DWORD stream_mask, ULONGLONG snapshot_generation,
    QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_DRAW_STATE *candidate)
{
    DWORD stream;

    if (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2) {
        if (state->stream_source_generation != snapshot_generation ||
            state->stream_source_generation == ~(ULONGLONG)0)
            return FALSE;
        for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
            if (!(stream_mask & (1UL << stream))) continue;
            state->streams[stream].handle = candidate->streams[stream].handle;
            state->streams[stream].offset = candidate->streams[stream].offset;
            state->streams[stream].stride = candidate->streams[stream].stride;
            state->streams[stream].user_memory =
                candidate->streams[stream].user_memory;
            state->streams[stream].source2 =
                candidate->streams[stream].source2;
        }
        ++state->stream_source_generation;
    } else if (opcode ==
               QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ) {
        for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
            if (stream_mask & (1UL << stream))
                state->streams[stream].divider =
                    candidate->streams[stream].divider;
        }
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI) {
        qxp_d3d9_draw_copy(
            state->vs_integer, candidate->vs_integer,
            (ULONG)sizeof(state->vs_integer));
        state->vs_integer_set_mask = candidate->vs_integer_set_mask;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB) {
        qxp_d3d9_draw_copy(
            state->vs_boolean, candidate->vs_boolean,
            (ULONG)sizeof(state->vs_boolean));
        state->vs_boolean_set_mask = candidate->vs_boolean_set_mask;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI) {
        qxp_d3d9_draw_copy(
            state->ps_integer, candidate->ps_integer,
            (ULONG)sizeof(state->ps_integer));
        state->ps_integer_set_mask = candidate->ps_integer_set_mask;
    } else if (opcode == QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB) {
        qxp_d3d9_draw_copy(
            state->ps_boolean, candidate->ps_boolean,
            (ULONG)sizeof(state->ps_boolean));
        state->ps_boolean_set_mask = candidate->ps_boolean_set_mask;
    }
    return TRUE;
}

BOOL QxpD3d9DispatchDrawStateBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    BOOL state_block_recording,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_STATE *candidate;
    DWORD stream_mask;
    ULONGLONG snapshot_generation;
    BOOL result;
    BOOL publish;
    BOOL published;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_draw_reset_status(status);
    if (!consumed_bytes || !error_offset) {
        if (error_offset) *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (state_block_recording) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK,
            0, 0, 0, 0, 0, 0);
    }
    if (!state || !allocator || !allocator->allocate ||
        !allocator->release || !lock_callback || !unlock_callback ||
        (record_count && !records)) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT,
            0, 0, 0, 0, 0, 0);
    }
    if (!qxp_d3d9_draw_dispatch_opcode_valid(opcode)) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_OPCODE,
            0, 0, 0, 0, 0, 0);
    }
    candidate = (QXP_D3D9_DRAW_STATE *)allocator->allocate(
        (ULONG)sizeof(*candidate), allocator->opaque);
    if (!candidate) {
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_RESOURCE,
            0, 0, 0, 0, 0, 0);
    }

    lock_callback(lock_opaque);
    qxp_d3d9_draw_copy(candidate, state, (ULONG)sizeof(*candidate));
    snapshot_generation = state->stream_source_generation;
    unlock_callback(lock_opaque);

    result = QxpD3d9ProcessDrawStateBatch(
        opcode, record_count, records, available_bytes, candidate,
        command_offset, consumed_bytes, error_offset, status);
    publish = result ? record_count != 0 : *consumed_bytes != 0;
    stream_mask = 0;
    if (publish &&
        (opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2 ||
         opcode == QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ)) {
        stream_mask = qxp_d3d9_draw_stream_mask(
            opcode, record_count, records);
    }
    published = TRUE;
    if (publish) {
        lock_callback(lock_opaque);
        published = qxp_d3d9_draw_publish_candidate(
            opcode, stream_mask, snapshot_generation, state, candidate);
        unlock_callback(lock_opaque);
    }
    allocator->release(candidate, allocator->opaque);
    if (!published) {
        *consumed_bytes = 0;
        *error_offset = command_offset;
        return qxp_d3d9_draw_fail(
            status, QXP_D3D9_DRAW_STATE_ERROR_STATE_CHANGED,
            0, 0, 0, 0, 0, 0);
    }
    return result;
}
