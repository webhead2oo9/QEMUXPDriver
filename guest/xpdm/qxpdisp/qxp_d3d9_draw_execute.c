#include <windef.h>

#include "qxp_d3d9_draw_execute.h"

#define QXP_D3D9_DWORD_MAX 0xffffffffUL
#define QXP_D3D9_LINK_NONE 0xffffffffUL

static void qxp_d3d9_execute_zero(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD index;

    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_d3d9_execute_copy(
    void *destination, const void *source, DWORD byte_count)
{
    volatile BYTE *to;
    const volatile BYTE *from;
    DWORD index;

    to = (volatile BYTE *)destination;
    from = (const volatile BYTE *)source;
    for (index = 0; index < byte_count; ++index) to[index] = from[index];
}

static BOOL qxp_d3d9_execute_equal(
    const void *left, const void *right, DWORD byte_count)
{
    const BYTE *left_bytes;
    const BYTE *right_bytes;
    DWORD index;

    left_bytes = (const BYTE *)left;
    right_bytes = (const BYTE *)right;
    for (index = 0; index < byte_count; ++index) {
        if (left_bytes[index] != right_bytes[index]) return FALSE;
    }
    return TRUE;
}

static DWORD qxp_d3d9_execute_type_bytes(DWORD type)
{
    static const BYTE type_bytes[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };

    if (type >= 8) return 0;
    return type_bytes[type];
}

static DWORD qxp_d3d9_execute_popcount16(DWORD value)
{
    DWORD count;

    count = 0;
    value &= 0xffffUL;
    while (value) {
        count += value & 1UL;
        value >>= 1;
    }
    return count;
}

static BOOL qxp_d3d9_execute_valid_bool(BOOL value)
{
    return value == FALSE || value == TRUE;
}

static BOOL qxp_d3d9_execute_valid_component_mask(DWORD mask)
{
    return mask == 1 || mask == 3 || mask == 7 || mask == 15;
}

static BOOL qxp_d3d9_execute_validate_metadata(
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    DWORD *written_outputs)
{
    DWORD index;
    DWORD output;

    if ((metadata->input_mask & ~0xffffUL) ||
        metadata->declaration_count !=
            qxp_d3d9_execute_popcount16(metadata->input_mask) ||
        (metadata->attribute_output_mask & ~3UL) ||
        (metadata->texcoord_output_mask & ~0xffUL) ||
        !qxp_d3d9_execute_valid_bool(metadata->writes_position) ||
        !qxp_d3d9_execute_valid_bool(metadata->writes_fog) ||
        !qxp_d3d9_execute_valid_bool(metadata->writes_point_size) ||
        metadata->writes_position != TRUE ||
        metadata->position_component_mask != 15)
        return FALSE;

    for (index = 0; index < 16; ++index) {
        DWORD bit;

        bit = 1UL << index;
        if (metadata->input_mask & bit) {
            if (metadata->input_usage[index] >=
                    QXP_VERTEX_DECLARATION9_USAGE_COUNT ||
                metadata->input_usage_index[index] >
                    QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX ||
                metadata->input_component_mask[index] != 15)
                return FALSE;
        } else if (metadata->input_usage[index] ||
                   metadata->input_usage_index[index] ||
                   metadata->input_component_mask[index]) {
            return FALSE;
        }
    }
    for (index = 0; index < 2; ++index) {
        DWORD bit;
        DWORD components;

        bit = 1UL << index;
        components = metadata->attribute_output_components[index];
        if (metadata->attribute_output_mask & bit) {
            if (!components || (components & ~15UL)) return FALSE;
        } else if (components) {
            return FALSE;
        }
    }
    for (index = 0; index < 8; ++index) {
        DWORD bit;
        DWORD components;

        bit = 1UL << index;
        components = metadata->texcoord_output_components[index];
        if (metadata->texcoord_output_mask & bit) {
            if (!qxp_d3d9_execute_valid_component_mask(components))
                return FALSE;
        } else if (components) {
            return FALSE;
        }
    }
    if (metadata->writes_fog) {
        if (!metadata->fog_component_mask ||
            (metadata->fog_component_mask & ~15U)) return FALSE;
    } else if (metadata->fog_component_mask) {
        return FALSE;
    }
    if (metadata->writes_point_size) {
        if (!metadata->point_size_component_mask ||
            (metadata->point_size_component_mask & ~15U)) return FALSE;
    } else if (metadata->point_size_component_mask) {
        return FALSE;
    }

    output = QXP_D3D_VS_OUTPUT_POSITION;
    if (metadata->writes_fog) output |= QXP_D3D_VS_OUTPUT_FOG;
    if (metadata->writes_point_size) output |= QXP_D3D_VS_OUTPUT_POINT_SIZE;
    if (metadata->attribute_output_mask & 1UL)
        output |= QXP_D3D_VS_OUTPUT_COLOR0;
    if (metadata->attribute_output_mask & 2UL)
        output |= QXP_D3D_VS_OUTPUT_COLOR1;
    output |= metadata->texcoord_output_mask << 8;
    if (output & ~QXP_D3D_VS_OUTPUT_MASK) return FALSE;
    *written_outputs = output;
    return TRUE;
}

static BOOL qxp_d3d9_execute_rebuild_link(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_VERTEX_SHADER_LINK *expected)
{
    return QxpD3d9RebuildVertexShaderLink(
        &prepared->declaration, &prepared->vertex_metadata,
        prepared->vertex_link.mode, expected);
}

static BOOL qxp_d3d9_execute_validate_prepared(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW *views,
    QXP_D3D9_VERTEX_SHADER_LINK *expected_link,
    DWORD *written_outputs)
{
    QXP_VERTEX_DECLARATION9_ERROR declaration_error;
    DWORD stream;
    DWORD element_index;
    DWORD expected_user_memory_mask;

    if (!prepared || !views || prepared->valid != TRUE ||
        (prepared->mode != QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
         prepared->mode !=
             QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS) ||
        prepared->declaration.valid != TRUE ||
        !prepared->declaration.element_count ||
        prepared->declaration.element_count >
            QXP_VERTEX_DECLARATION9_MAX_ELEMENTS)
        return FALSE;
    qxp_d3d9_execute_zero(
        &declaration_error, (DWORD)sizeof(declaration_error));
    if (!QxpValidateParsedVertexDeclaration9(
            &prepared->declaration, &declaration_error))
        return FALSE;
    for (element_index = 0;
         element_index < prepared->declaration.element_count;
         ++element_index) {
        const QXP_VERTEX_DECLARATION9_ELEMENT *element;

        element = &prepared->declaration.elements[element_index];
        if (element->type >= 8 ||
            element->method != QXP_VERTEX_DECLMETHOD9_DEFAULT ||
            element->byte_size !=
                qxp_d3d9_execute_type_bytes(element->type))
            return FALSE;
    }
    if (!qxp_d3d9_execute_validate_metadata(
            &prepared->vertex_metadata, written_outputs) ||
        !qxp_d3d9_execute_rebuild_link(prepared, expected_link) ||
        !qxp_d3d9_execute_equal(
            &prepared->vertex_link, expected_link,
            (DWORD)sizeof(*expected_link)) ||
        prepared->undefined_vertex_input_mask !=
            expected_link->undefined_input_mask ||
        prepared->required_stream_mask !=
            expected_link->matched_stream_mask ||
        (prepared->required_user_memory_stream_mask &
         ~prepared->required_stream_mask))
        return FALSE;

    for (element_index = 0; element_index < 16; ++element_index) {
        DWORD component;

        for (component = 0; component < 4; ++component) {
            if (prepared->undefined_vertex_input_defaults[element_index]
                    [component] != 0)
                return FALSE;
        }
    }

    expected_user_memory_mask = 0;
    for (stream = 0; stream < 16; ++stream) {
        DWORD bit;
        DWORD span;
        const QXP_VERTEX_STREAM_VIEW *view;
        const QXP_D3D9_DRAW_STREAM_STATE *source;

        bit = 1UL << stream;
        span = expected_link->required_stream_spans[stream];
        if (prepared->required_stream_spans[stream] != span) return FALSE;
        if (!(expected_link->matched_stream_mask & bit)) {
            if (span) return FALSE;
            continue;
        }
        source = &prepared->raw_state.streams[stream];
        if (!qxp_d3d9_execute_valid_bool(source->user_memory) ||
            !qxp_d3d9_execute_valid_bool(source->source2) ||
            !span || !source->stride || source->stride < span ||
            source->divider != 1UL ||
            (!source->source2 && source->offset) ||
            source->offset > QXP_D3D9_DWORD_MAX - span)
            return FALSE;
        if (source->user_memory) {
            if (stream != 0 || source->handle || source->offset ||
                source->source2) return FALSE;
            expected_user_memory_mask |= bit;
        } else if (!source->handle) {
            return FALSE;
        }
        view = &views[stream];
        if (!view->base || !view->bytes || !view->stride ||
            !qxp_d3d9_execute_valid_bool(view->wraps) ||
            view->stride != source->stride || view->stride < span)
            return FALSE;
        if (view->wraps == FALSE &&
            source->offset + span > view->bytes)
            return FALSE;
    }
    for (element_index = 0; element_index < 16; ++element_index) {
        const QXP_D3D9_VERTEX_INPUT_LINK *input;

        if (!(expected_link->matched_input_mask &
              (1UL << element_index))) continue;
        input = &expected_link->inputs[element_index];
        if (views[input->source_stream].wraps == TRUE &&
            input->source_byte_size >
                views[input->source_stream].bytes)
            return FALSE;
    }
    if (prepared->required_user_memory_stream_mask !=
        expected_user_memory_mask) return FALSE;
    return TRUE;
}

static BOOL qxp_d3d9_execute_build(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW *views,
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT *layout)
{
    QXP_D3D9_VERTEX_SHADER_LINK expected_link;
    DWORD written_outputs;
    DWORD input_index;
    DWORD packed_offset;

    qxp_d3d9_execute_zero(layout, (DWORD)sizeof(*layout));
    written_outputs = 0;
    if (!qxp_d3d9_execute_validate_prepared(
            prepared, views, &expected_link, &written_outputs))
        return FALSE;

    packed_offset = 0;
    for (input_index = 0; input_index < 16; ++input_index) {
        const QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;
        DWORD byte_size;
        DWORD format;

        input_bit = 1UL << input_index;
        input = &expected_link.inputs[input_index];
        if (expected_link.matched_input_mask & input_bit) {
            byte_size = input->source_byte_size;
            format = input->source_type;
        } else if (expected_link.undefined_input_mask & input_bit) {
            if (input_index != 0) continue;
            byte_size = 16;
            format = QXP_D3D_VERTEX_FORMAT_FLOAT4;
        } else {
            continue;
        }
        if (!byte_size ||
            packed_offset > QXP_D3D_MAX_PROGRAM_VERTEX_STRIDE - byte_size)
            return FALSE;
        layout->elements[input_index].format = format;
        layout->elements[input_index].byte_offset = packed_offset;
        layout->input_mask |= input_bit;
        packed_offset += byte_size;
    }
    if (packed_offset > QXP_D3D_MAX_PROGRAM_VERTEX_STRIDE) return FALSE;
    layout->used_inputs = expected_link.input_mask;
    layout->written_outputs = written_outputs;
    layout->vertex_stride = packed_offset;
    layout->valid = TRUE;
    return TRUE;
}

BOOL QxpD3d9BuildProgramVertexLayout(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW views[16],
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT *layout)
{
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT candidate;

    if (layout)
        qxp_d3d9_execute_zero(layout, (DWORD)sizeof(*layout));
    if (!prepared || !views || !layout) return FALSE;
    if (!qxp_d3d9_execute_build(prepared, views, &candidate)) return FALSE;
    qxp_d3d9_execute_copy(layout, &candidate, (DWORD)sizeof(candidate));
    return TRUE;
}

static BOOL qxp_d3d9_execute_source_start(
    const QXP_VERTEX_STREAM_VIEW *view, DWORD vertex_index,
    DWORD stream_offset, DWORD element_offset, DWORD byte_size,
    ULONGLONG *source_start)
{
    ULONGLONG product;
    ULONGLONG start;
    ULONGLONG maximum;

    maximum = ~(ULONGLONG)0;
    product = (ULONGLONG)vertex_index * (ULONGLONG)view->stride;
    if (stream_offset > maximum - product) return FALSE;
    start = product + stream_offset;
    if (element_offset > maximum - start) return FALSE;
    start += element_offset;
    if (byte_size > maximum - start) return FALSE;
    if (view->wraps == FALSE &&
        start + byte_size > (ULONGLONG)view->bytes)
        return FALSE;
    if (view->wraps == TRUE && byte_size > view->bytes) return FALSE;
    *source_start = start;
    return TRUE;
}

static void qxp_d3d9_execute_copy_source(
    BYTE *destination, const QXP_VERTEX_STREAM_VIEW *view,
    ULONGLONG source_start, DWORD byte_size)
{
    DWORD offset;
    DWORD first;

    if (view->wraps == FALSE) {
        qxp_d3d9_execute_copy(
            destination, view->base + (DWORD)source_start, byte_size);
        return;
    }
    offset = (DWORD)(source_start % view->bytes);
    first = view->bytes - offset;
    if (first >= byte_size) {
        qxp_d3d9_execute_copy(destination, view->base + offset, byte_size);
    } else {
        qxp_d3d9_execute_copy(destination, view->base + offset, first);
        qxp_d3d9_execute_copy(
            destination + first, view->base, byte_size - first);
    }
}

BOOL QxpD3d9GatherProgramVertex(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW views[16],
    const QXP_D3D9_PROGRAM_VERTEX_LAYOUT *layout,
    DWORD vertex_index, BYTE *destination, DWORD destination_bytes)
{
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT expected_layout;
    ULONGLONG source_starts[16];
    DWORD input_index;

    if (!prepared || !views || !layout ||
        !qxp_d3d9_execute_build(prepared, views, &expected_layout) ||
        !qxp_d3d9_execute_equal(
            layout, &expected_layout, (DWORD)sizeof(expected_layout)) ||
        destination_bytes < expected_layout.vertex_stride ||
        (!destination && expected_layout.vertex_stride))
        return FALSE;

    for (input_index = 0; input_index < 16; ++input_index) {
        const QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;

        input_bit = 1UL << input_index;
        if (!(prepared->vertex_link.matched_input_mask & input_bit)) {
            source_starts[input_index] = 0;
            continue;
        }
        input = &prepared->vertex_link.inputs[input_index];
        if (!qxp_d3d9_execute_source_start(
                &views[input->source_stream], vertex_index,
                prepared->raw_state.streams[input->source_stream].offset,
                input->source_offset, input->source_byte_size,
                &source_starts[input_index]))
            return FALSE;
    }

    for (input_index = 0; input_index < 16; ++input_index) {
        const QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;
        BYTE *output;

        input_bit = 1UL << input_index;
        if (!(expected_layout.input_mask & input_bit)) continue;
        output = destination +
                 expected_layout.elements[input_index].byte_offset;
        if (!(prepared->vertex_link.matched_input_mask & input_bit)) {
            qxp_d3d9_execute_zero(output, 16);
            continue;
        }
        input = &prepared->vertex_link.inputs[input_index];
        qxp_d3d9_execute_copy_source(
            output, &views[input->source_stream],
            source_starts[input_index], input->source_byte_size);
        if (input->source_type == QXP_VERTEX_DECLTYPE9_D3DCOLOR) {
            BYTE value;

            value = output[0];
            output[0] = output[2];
            output[2] = value;
        }
    }
    return TRUE;
}
