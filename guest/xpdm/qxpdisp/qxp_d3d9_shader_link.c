#include <windef.h>

#include "qxp_d3d9_shader_link.h"

#define QXP_D3D9_DCL_PARAMETER_MARKER   0x80000000UL
#define QXP_D3D9_DCL_USAGE_MASK         0x0000000fUL
#define QXP_D3D9_DCL_USAGE_INDEX_MASK   0x000f0000UL
#define QXP_D3D9_DCL_USAGE_INDEX_SHIFT  16UL

static void qxp_d3d9_link_zero(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD index;

    /* Volatile prevents the WDK optimizer from introducing a CRT memset. */
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index)
        bytes[index] = 0;
}

static BOOL qxp_d3d9_link_fail(
    QXP_D3D9_VERTEX_SHADER_LINK *link,
    QXP_D3D9_VERTEX_LINK_STATUS *status,
    DWORD code, DWORD declaration_element,
    DWORD instruction_index, DWORD token_offset)
{
    if (link)
        qxp_d3d9_link_zero(link, (DWORD)sizeof(*link));
    if (status) {
        qxp_d3d9_link_zero(status, (DWORD)sizeof(*status));
        status->code = code;
        status->declaration_element = declaration_element;
        status->instruction_index = instruction_index;
        status->token_offset = token_offset;
    }
    return FALSE;
}

static BOOL qxp_d3d9_link_validate_declaration(
    const QXP_VERTEX_DECLARATION9 *declaration,
    BOOL *positiont_bypass,
    QXP_D3D9_VERTEX_SHADER_LINK *link,
    QXP_D3D9_VERTEX_LINK_STATUS *status)
{
    QXP_VERTEX_DECLARATION9_ERROR validation_error;
    DWORD code;
    DWORD index;

    if (!declaration->valid || !declaration->element_count ||
        declaration->element_count >
            QXP_VERTEX_DECLARATION9_MAX_ELEMENTS)
        return qxp_d3d9_link_fail(
            link, status, QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_STATE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE);

    qxp_d3d9_link_zero(
        &validation_error, (DWORD)sizeof(validation_error));
    if (!QxpValidateParsedVertexDeclaration9(
            declaration, &validation_error)) {
        code = QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT;
        if (validation_error.code ==
            QXP_VERTEX_DECLARATION9_ERROR_DUPLICATE_SEMANTIC)
            code = QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_SEMANTIC;
        else if (validation_error.code ==
                 QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE)
            code = QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_STATE;
        return qxp_d3d9_link_fail(
            link, status, code, validation_error.element_index,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE);
    }

    *positiont_bypass = FALSE;
    for (index = 0; index < declaration->element_count; ++index) {
        if (declaration->elements[index].usage ==
                QXP_VERTEX_DECLUSAGE9_POSITIONT &&
            declaration->elements[index].usage_index == 0) {
            *positiont_bypass = TRUE;
            break;
        }
    }
    return TRUE;
}

static BOOL qxp_d3d9_link_validate_shader(
    const QXP_SHADER_IR *shader,
    QXP_D3D9_VERTEX_SHADER_LINK *link,
    QXP_D3D9_VERTEX_LINK_STATUS *status)
{
    DWORD semantic_masks[QXP_VERTEX_DECLARATION9_USAGE_COUNT];
    DWORD input_mask;
    DWORD declaration_count;
    DWORD index;

    if (!shader->valid || shader->token_count < 2 ||
        shader->token_count > QXP_SHADER_MAX_TOKENS ||
        shader->instruction_count > QXP_SHADER_MAX_INSTRUCTIONS ||
        shader->declaration_count > QXP_D3D9_VERTEX_LINK_INPUT_COUNT)
        return qxp_d3d9_link_fail(
            link, status, QXP_D3D9_VERTEX_LINK_ERROR_SHADER_STATE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE);
    if (shader->stage != QXP_SHADER_STAGE_VERTEX ||
        !QXP_SHADER_VERSION_IS_VERTEX(shader->version_token))
        return qxp_d3d9_link_fail(
            link, status, QXP_D3D9_VERTEX_LINK_ERROR_SHADER_PROFILE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE);

    qxp_d3d9_link_zero(semantic_masks, (DWORD)sizeof(semantic_masks));
    input_mask = 0;
    declaration_count = 0;
    for (index = 0; index < shader->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        const QXP_SHADER_OPERAND *destination;
        QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;
        DWORD semantic_bit;
        DWORD raw_declaration;

        instruction = &shader->instructions[index];
        if (instruction->opcode != QXP_SHADER_OP_DCL) {
            if (instruction->declaration.present)
                return qxp_d3d9_link_fail(
                    link, status,
                    QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
                    QXP_D3D9_VERTEX_LINK_INDEX_NONE, index,
                    instruction->token_offset);
            continue;
        }
        destination = &instruction->destinations[0];
        raw_declaration = instruction->declaration.raw_token;
        if (!instruction->declaration.present ||
            instruction->declaration.kind !=
                QXP_SHADER_DECLARATION_INPUT ||
            instruction->declaration.usage >=
                QXP_VERTEX_DECLARATION9_USAGE_COUNT ||
            instruction->declaration.usage_index >
                QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX ||
            (raw_declaration & QXP_D3D9_DCL_PARAMETER_MARKER) == 0 ||
            (raw_declaration &
             ~(QXP_D3D9_DCL_PARAMETER_MARKER |
               QXP_D3D9_DCL_USAGE_MASK |
               QXP_D3D9_DCL_USAGE_INDEX_MASK)) != 0 ||
            (raw_declaration & QXP_D3D9_DCL_USAGE_MASK) !=
                instruction->declaration.usage ||
            ((raw_declaration & QXP_D3D9_DCL_USAGE_INDEX_MASK) >>
             QXP_D3D9_DCL_USAGE_INDEX_SHIFT) !=
                instruction->declaration.usage_index ||
            instruction->destination_count != 1 ||
            instruction->source_count != 0 ||
            instruction->immediate_count != 0 ||
            instruction->token_count != 3 ||
            instruction->token_offset >= shader->token_count ||
            instruction->token_count >
                shader->token_count - instruction->token_offset ||
            destination->role != QXP_SHADER_OPERAND_DECLARATION ||
            destination->register_file != QXP_SHADER_REG_INPUT ||
            destination->register_number >=
                QXP_D3D9_VERTEX_LINK_INPUT_COUNT ||
            destination->write_mask != 15 ||
            destination->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            destination->destination_modifier !=
                QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0 ||
            destination->relative.present)
            return qxp_d3d9_link_fail(
                link, status,
                QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
                QXP_D3D9_VERTEX_LINK_INDEX_NONE, index,
                instruction->token_offset);

        input_bit = 1UL << destination->register_number;
        semantic_bit = 1UL << instruction->declaration.usage_index;
        if ((input_mask & input_bit) ||
            (semantic_masks[instruction->declaration.usage] & semantic_bit))
            return qxp_d3d9_link_fail(
                link, status,
                QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION,
                QXP_D3D9_VERTEX_LINK_INDEX_NONE, index,
                instruction->token_offset);
        input_mask |= input_bit;
        semantic_masks[instruction->declaration.usage] |= semantic_bit;

        input = &link->inputs[destination->register_number];
        input->declared = TRUE;
        input->usage = instruction->declaration.usage;
        input->usage_index = instruction->declaration.usage_index;
        input->destination_register = destination->register_number;
        input->declaration_element = QXP_D3D9_VERTEX_LINK_INDEX_NONE;
        ++declaration_count;
    }

    if (declaration_count != shader->declaration_count)
        return qxp_d3d9_link_fail(
            link, status,
            QXP_D3D9_VERTEX_LINK_ERROR_SHADER_STATE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE,
            QXP_D3D9_VERTEX_LINK_INDEX_NONE);
    link->input_count = declaration_count;
    link->input_mask = input_mask;
    return TRUE;
}

BOOL QxpD3d9LinkVertexShader20(
    const QXP_VERTEX_DECLARATION9 *declaration,
    const QXP_SHADER_IR *shader,
    QXP_D3D9_VERTEX_SHADER_LINK *link,
    QXP_D3D9_VERTEX_LINK_STATUS *status)
{
    DWORD input_index;
    BOOL positiont_bypass;

    if (link)
        qxp_d3d9_link_zero(link, (DWORD)sizeof(*link));
    if (status)
        qxp_d3d9_link_zero(status, (DWORD)sizeof(*status));
    if (!declaration || !shader || !link || !status)
        return qxp_d3d9_link_fail(
            link, status, QXP_D3D9_VERTEX_LINK_ERROR_NULL_ARGUMENT,
            0, 0, 0);

    positiont_bypass = FALSE;
    if (!qxp_d3d9_link_validate_declaration(
            declaration, &positiont_bypass, link, status))
        return FALSE;

    for (input_index = 0;
         input_index < QXP_D3D9_VERTEX_LINK_INPUT_COUNT; ++input_index)
        link->inputs[input_index].declaration_element =
            QXP_D3D9_VERTEX_LINK_INDEX_NONE;
    if (!qxp_d3d9_link_validate_shader(shader, link, status))
        return FALSE;

    if (positiont_bypass) {
        link->mode = QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS;
        link->valid = TRUE;
        return TRUE;
    }

    link->mode = QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE;
    for (input_index = 0;
         input_index < QXP_D3D9_VERTEX_LINK_INPUT_COUNT; ++input_index) {
        QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD element_index;
        DWORD input_bit;
        BOOL found;

        input = &link->inputs[input_index];
        if (!input->declared) continue;
        input_bit = 1UL << input_index;
        found = FALSE;
        for (element_index = 0;
             element_index < declaration->element_count; ++element_index) {
            const QXP_VERTEX_DECLARATION9_ELEMENT *element;
            DWORD element_end;

            element = &declaration->elements[element_index];
            if ((DWORD)element->usage != input->usage ||
                (DWORD)element->usage_index != input->usage_index)
                continue;
            input->matched = TRUE;
            input->declaration_element = element_index;
            input->source_stream = element->stream;
            input->source_offset = element->offset;
            input->source_type = element->type;
            input->source_method = element->method;
            input->source_byte_size = element->byte_size;
            link->matched_input_mask |= input_bit;
            link->matched_stream_mask |= 1UL << element->stream;
            link->matched_element_mask |=
                ((ULONGLONG)1) << element_index;
            element_end = (DWORD)element->offset + element->byte_size;
            if (element_end >
                link->required_stream_spans[element->stream])
                link->required_stream_spans[element->stream] = element_end;
            ++link->matched_input_count;
            found = TRUE;
            break;
        }
        if (!found) {
            link->undefined_input_mask |= input_bit;
            ++link->undefined_input_count;
        }
    }

    link->valid = TRUE;
    return TRUE;
}

static DWORD qxp_d3d9_link_type_bytes(DWORD type)
{
    static const BYTE type_bytes[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };

    if (type >= 8) return 0;
    return type_bytes[type];
}

static DWORD qxp_d3d9_link_popcount16(DWORD value)
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

BOOL QxpD3d9RebuildVertexShaderLink(
    const QXP_VERTEX_DECLARATION9 *declaration,
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    DWORD mode, QXP_D3D9_VERTEX_SHADER_LINK *link)
{
    DWORD input_index;

    if (link) qxp_d3d9_link_zero(link, (DWORD)sizeof(*link));
    if (!declaration || !metadata || !link) return FALSE;
    link->mode = mode;
    link->input_mask = metadata->input_mask;
    link->input_count = qxp_d3d9_link_popcount16(metadata->input_mask);
    for (input_index = 0;
         input_index < QXP_D3D9_VERTEX_LINK_INPUT_COUNT; ++input_index)
        link->inputs[input_index].declaration_element =
            QXP_D3D9_VERTEX_LINK_INDEX_NONE;

    for (input_index = 0;
         input_index < QXP_D3D9_VERTEX_LINK_INPUT_COUNT; ++input_index) {
        QXP_D3D9_VERTEX_INPUT_LINK *input;
        DWORD input_bit;
        DWORD element_index;
        BOOL found;

        input_bit = 1UL << input_index;
        if (!(metadata->input_mask & input_bit)) continue;
        input = &link->inputs[input_index];
        input->declared = TRUE;
        input->usage = metadata->input_usage[input_index];
        input->usage_index = metadata->input_usage_index[input_index];
        input->destination_register = input_index;
        found = FALSE;
        for (element_index = 0;
             element_index < declaration->element_count; ++element_index) {
            const QXP_VERTEX_DECLARATION9_ELEMENT *element;
            DWORD element_end;

            element = &declaration->elements[element_index];
            if ((DWORD)element->usage != input->usage ||
                (DWORD)element->usage_index != input->usage_index)
                continue;
            if (element->stream >= QXP_VERTEX_DECLARATION9_STREAM_COUNT ||
                element->type >= 8 ||
                element->byte_size !=
                    qxp_d3d9_link_type_bytes(element->type) ||
                (DWORD)element->offset > 0xffffffffUL - element->byte_size)
                return FALSE;
            element_end = (DWORD)element->offset + element->byte_size;
            input->matched = TRUE;
            input->declaration_element = element_index;
            input->source_stream = element->stream;
            input->source_offset = element->offset;
            input->source_type = element->type;
            input->source_method = element->method;
            input->source_byte_size = element->byte_size;
            link->matched_input_mask |= input_bit;
            link->matched_stream_mask |= 1UL << element->stream;
            link->matched_element_mask |= ((ULONGLONG)1) << element_index;
            if (element_end > link->required_stream_spans[element->stream])
                link->required_stream_spans[element->stream] = element_end;
            ++link->matched_input_count;
            found = TRUE;
            break;
        }
        if (!found) {
            link->undefined_input_mask |= input_bit;
            ++link->undefined_input_count;
        }
    }
    link->valid = TRUE;
    return TRUE;
}
