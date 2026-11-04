#include <windef.h>

#include "qxp_pshader_glsl.h"

#define QXP_PS_DCL_PARAMETER_MARKER  0x80000000UL
#define QXP_PS_DCL_TEXTURETYPE_MASK  0x78000000UL
#define QXP_PS_DCL_TEXTURETYPE_SHIFT 27UL

typedef struct qxp_ps_glsl_text {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_PS_GLSL_TEXT;

typedef struct qxp_ps_glsl_emit {
    QXP_PS_GLSL_TEXT output;
    QXP_PIXEL_SHADER_GLSL_ERROR *error;
    DWORD instruction_index;
    DWORD token_offset;
    /* ps_1_1 reuses this emitter: t# is a mutable register, arithmetic
     * destinations may scale by _x2, and r0 is the pixel colour. */
    BOOL ps11;
} QXP_PS_GLSL_EMIT;

static void qxp_ps_zero(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD index;

    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index)
        bytes[index] = 0;
}

static BOOL qxp_ps_fail(QXP_PIXEL_SHADER_GLSL_ERROR *error,
                        DWORD code, DWORD instruction_index,
                        DWORD token_offset, DWORD required_capacity)
{
    if (error) {
        qxp_ps_zero(error, (DWORD)sizeof(*error));
        error->code = code;
        error->instruction_index = instruction_index;
        error->token_offset = token_offset;
        error->required_capacity = required_capacity;
    }
    return FALSE;
}

static BOOL qxp_ps_emit_fail(QXP_PS_GLSL_EMIT *emit, DWORD code)
{
    return qxp_ps_fail(emit->error, code, emit->instruction_index,
                       emit->token_offset, 0);
}

static void qxp_ps_append_char(QXP_PS_GLSL_TEXT *output, char value)
{
    if (!output || !output->valid ||
        output->length + 1 >= output->capacity) {
        if (output) output->valid = FALSE;
        return;
    }
    if (output->text) {
        output->text[output->length] = value;
        output->text[output->length + 1] = 0;
    }
    ++output->length;
}

static void qxp_ps_append(QXP_PS_GLSL_TEXT *output, const char *value)
{
    if (!output || !value) return;
    while (*value) qxp_ps_append_char(output, *value++);
}

static void qxp_ps_append_uint(QXP_PS_GLSL_TEXT *output, DWORD value)
{
    char digits[11];
    DWORD count;

    count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < (DWORD)sizeof(digits));
    while (count) qxp_ps_append_char(output, digits[--count]);
}

static void qxp_ps_append_component_mask(QXP_PS_GLSL_TEXT *output,
                                         DWORD mask)
{
    static const char components[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;

    if (mask == 15) return;
    qxp_ps_append_char(output, '.');
    for (index = 0; index < 4; ++index) {
        if (mask & (1UL << index))
            qxp_ps_append_char(output, components[index]);
    }
}


static BOOL qxp_ps_source_valid(const QXP_SHADER_OPERAND *operand)
{
    DWORD limit;
    DWORD index;

    if (!operand || operand->role != QXP_SHADER_OPERAND_SOURCE ||
        operand->relative.present ||
        (operand->source_modifier != QXP_SHADER_SRCMOD_NONE &&
         operand->source_modifier != QXP_SHADER_SRCMOD_NEGATE &&
         operand->source_modifier != QXP_SHADER_SRCMOD_COMP))
        return FALSE;
    limit = 0;
    if (operand->register_file == QXP_SHADER_REG_TEMP)
        limit = QXP_SHADER_MAX_PS_TEMPS;
    else if (operand->register_file == QXP_SHADER_REG_INPUT)
        limit = 2;
    else if (operand->register_file == QXP_SHADER_REG_CONST)
        limit = QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT;
    else if (operand->register_file == QXP_SHADER_REG_TEXTURE)
        limit = 8;
    if (!limit || (DWORD)operand->register_number >= limit) return FALSE;
    for (index = 0; index < 4; ++index) {
        if (operand->swizzle[index] >= 4) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_ps_swizzle_identity(
    const QXP_SHADER_OPERAND *operand)
{
    return operand && operand->swizzle[0] == 0 &&
           operand->swizzle[1] == 1 && operand->swizzle[2] == 2 &&
           operand->swizzle[3] == 3;
}

static BOOL qxp_ps_swizzle_replicate(
    const QXP_SHADER_OPERAND *operand)
{
    return operand &&
           operand->swizzle[0] == operand->swizzle[1] &&
           operand->swizzle[0] == operand->swizzle[2] &&
           operand->swizzle[0] == operand->swizzle[3];
}

static BOOL qxp_ps_operands_alias(
    const QXP_SHADER_OPERAND *left,
    const QXP_SHADER_OPERAND *right)
{
    return left && right &&
           left->register_file == right->register_file &&
           left->register_number == right->register_number;
}

static BOOL qxp_ps_append_register(QXP_PS_GLSL_EMIT *emit,
                                   DWORD register_file,
                                   DWORD register_number)
{
    QXP_PS_GLSL_TEXT *output;

    output = &emit->output;
    if (register_file == QXP_SHADER_REG_TEMP &&
        register_number < QXP_SHADER_MAX_PS_TEMPS) {
        qxp_ps_append(output, "qxpR");
    } else if (register_file == QXP_SHADER_REG_INPUT &&
               register_number < 2) {
        qxp_ps_append(output, "qxpV");
    } else if (register_file == QXP_SHADER_REG_TEXTURE &&
               register_number < 8) {
        qxp_ps_append(output, "qxpT");
    } else if (register_file == QXP_SHADER_REG_CONST &&
               register_number < QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT) {
        qxp_ps_append(output, "qxpPsConstants[");
        qxp_ps_append_uint(output, register_number);
        qxp_ps_append_char(output, ']');
        return output->valid;
    } else {
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    }
    qxp_ps_append_uint(output, register_number);
    return output->valid;
}

static BOOL qxp_ps_append_source(QXP_PS_GLSL_EMIT *emit,
                                 const QXP_SHADER_OPERAND *operand)
{
    static const char components[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;
    BOOL identity;

    if (!qxp_ps_source_valid(operand))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    /* _comp is a ps_1_1-only admission in the decoder, so a hand-built
     * ps_2_0 IR must not reach the complement lowering either. */
    if (operand->source_modifier == QXP_SHADER_SRCMOD_COMP &&
        !emit->ps11)
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    if (operand->source_modifier == QXP_SHADER_SRCMOD_NEGATE)
        qxp_ps_append(&emit->output, "-(");
    else if (operand->source_modifier == QXP_SHADER_SRCMOD_COMP)
        qxp_ps_append(&emit->output, "vec4(1.0) - (");
    if (!qxp_ps_append_register(
            emit, operand->register_file, operand->register_number))
        return FALSE;
    identity = TRUE;
    for (index = 0; index < 4; ++index) {
        if (operand->swizzle[index] != index) identity = FALSE;
    }
    if (!identity) {
        qxp_ps_append_char(&emit->output, '.');
        for (index = 0; index < 4; ++index)
            qxp_ps_append_char(
                &emit->output, components[operand->swizzle[index]]);
    }
    if (operand->source_modifier == QXP_SHADER_SRCMOD_NEGATE ||
        operand->source_modifier == QXP_SHADER_SRCMOD_COMP)
        qxp_ps_append_char(&emit->output, ')');
    return emit->output.valid;
}

static BOOL qxp_ps_append_source_register_offset(
    QXP_PS_GLSL_EMIT *emit, const QXP_SHADER_OPERAND *operand,
    DWORD register_offset)
{
    QXP_SHADER_OPERAND adjusted;

    if (!qxp_ps_source_valid(operand) ||
        (DWORD)operand->register_number + register_offset > 0xffffUL)
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    adjusted = *operand;
    adjusted.register_number =
        (WORD)((DWORD)adjusted.register_number + register_offset);
    return qxp_ps_append_source(emit, &adjusted);
}

/*
 * ps_2_0 arithmetic destinations are always r#; ps_1_1 may also write its
 * mutable t# registers and scale the result by the _x2 or _x4 multiply
 * shifts (max_shift 2, the raw D3DSP_DSTSHIFT value; NFSU2 evidence r542
 * uses _x4).  Divide shifts stay decoder-refused.
 */
static BOOL qxp_ps_temp_destination_valid(
    const QXP_SHADER_OPERAND *destination, DWORD allowed_modifiers,
    DWORD max_shift, BOOL allow_texture)
{
    if (!destination ||
        destination->role != QXP_SHADER_OPERAND_DESTINATION)
        return FALSE;
    if (destination->register_file == QXP_SHADER_REG_TEXTURE) {
        if (!allow_texture || destination->register_number >= 8)
            return FALSE;
    } else if (destination->register_file != QXP_SHADER_REG_TEMP ||
               destination->register_number >= QXP_SHADER_MAX_PS_TEMPS) {
        return FALSE;
    }
    if (!destination->write_mask || destination->write_mask > 15 ||
        destination->destination_shift > max_shift ||
        (destination->destination_modifier & ~allowed_modifiers) != 0 ||
        destination->relative.present)
        return FALSE;
    return TRUE;
}

static BOOL qxp_ps_emit_arguments(QXP_PS_GLSL_EMIT *emit,
                                  const QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD index;

    for (index = 0; index < instruction->source_count; ++index) {
        qxp_ps_append(&emit->output, "  qxpArg");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, " = ");
        if (!qxp_ps_append_source(emit, &instruction->sources[index]))
            return FALSE;
        qxp_ps_append(&emit->output, ";\n");
    }
    return emit->output.valid;
}

static BOOL qxp_ps_emit_store(QXP_PS_GLSL_EMIT *emit,
                              const QXP_SHADER_OPERAND *destination,
                              BOOL allow_saturate)
{
    DWORD modifier;

    if (!qxp_ps_temp_destination_valid(
            destination,
            emit->ps11 ? QXP_SHADER_DSTMOD_SATURATE :
                         (QXP_SHADER_DSTMOD_SATURATE |
                          QXP_SHADER_DSTMOD_PARTIAL_PRECISION),
            emit->ps11 ? 2 : 0, emit->ps11))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    modifier = destination->destination_modifier;
    if (!allow_saturate && (modifier & QXP_SHADER_DSTMOD_SATURATE))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    /* D3D applies the ps_1_1 result shift (_x2, _x4) before the
     * saturate and the write mask, so scale the whole value here. */
    if (destination->destination_shift == 1)
        qxp_ps_append(&emit->output, "  qxpValue = qxpValue * 2.0;\n");
    else if (destination->destination_shift == 2)
        qxp_ps_append(&emit->output, "  qxpValue = qxpValue * 4.0;\n");
    if (modifier & QXP_SHADER_DSTMOD_SATURATE)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpPsSaturate(qxpValue);\n");
    qxp_ps_append(&emit->output,
                  destination->register_file == QXP_SHADER_REG_TEXTURE ?
                  "  qxpT" : "  qxpR");
    qxp_ps_append_uint(&emit->output, destination->register_number);
    qxp_ps_append_component_mask(
        &emit->output, destination->write_mask);
    qxp_ps_append(&emit->output, " = qxpValue");
    qxp_ps_append_component_mask(
        &emit->output, destination->write_mask);
    qxp_ps_append(&emit->output, ";\n");
    return emit->output.valid;
}

static BOOL qxp_ps_shape(const QXP_SHADER_INSTRUCTION *instruction,
                         DWORD destinations, DWORD sources,
                         DWORD immediates)
{
    return instruction->destination_count == destinations &&
           instruction->source_count == sources &&
           instruction->immediate_count == immediates;
}

typedef struct qxp_ps_opcode_schema {
    BYTE destinations;
    BYTE sources;
    BYTE immediates;
    BYTE extra_tokens;
    BYTE slot_class;
    BYTE slot_count;
} QXP_PS_OPCODE_SCHEMA;

static BOOL qxp_ps_get_opcode_schema(
    DWORD opcode, BOOL ps11, QXP_PS_OPCODE_SCHEMA *schema)
{
    qxp_ps_zero(schema, (DWORD)sizeof(*schema));
    schema->slot_class = QXP_SHADER_SLOT_ARITHMETIC;
    schema->slot_count = 1;
    if (opcode == QXP_SHADER_OP_NOP) return TRUE;
    schema->destinations = 1;
    if (opcode == QXP_SHADER_OP_MOV || opcode == QXP_SHADER_OP_RCP ||
        opcode == QXP_SHADER_OP_RSQ || opcode == QXP_SHADER_OP_EXP ||
        opcode == QXP_SHADER_OP_LOG || opcode == QXP_SHADER_OP_FRC ||
        opcode == QXP_SHADER_OP_ABS) {
        schema->sources = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_NRM) {
        schema->sources = 1;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_ADD || opcode == QXP_SHADER_OP_SUB ||
        opcode == QXP_SHADER_OP_MUL || opcode == QXP_SHADER_OP_DP3 ||
        opcode == QXP_SHADER_OP_DP4 || opcode == QXP_SHADER_OP_MIN ||
        opcode == QXP_SHADER_OP_MAX) {
        schema->sources = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_MAD || opcode == QXP_SHADER_OP_CMP) {
        schema->sources = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LRP) {
        schema->sources = 3;
        /* ps_2_0 charges lrp two instruction slots; ps_1_1 counts it as one
         * arithmetic instruction against its budget of eight.  The decoder
         * records the profile's own cost, so the schema must agree or the
         * cross-check rejects every ps_1_1 shader that blends. */
        schema->slot_count = ps11 ? 1 : 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M4X4 ||
        opcode == QXP_SHADER_OP_M3X4) {
        schema->sources = 2;
        schema->slot_count = 4;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M4X3 ||
        opcode == QXP_SHADER_OP_M3X3) {
        schema->sources = 2;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M3X2) {
        schema->sources = 2;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_POW) {
        schema->sources = 2;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_CRS) {
        schema->sources = 2;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_SINCOS) {
        schema->sources = 3;
        schema->slot_count = 8;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DP2ADD) {
        schema->sources = 3;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DCL) {
        schema->extra_tokens = 1;
        schema->slot_class = QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DEF) {
        schema->immediates = 4;
        schema->slot_class = QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_TEXKILL) {
        schema->slot_class = QXP_SHADER_SLOT_TEXTURE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_TEX) {
        /* ps_1_1 `tex tN` names its sampler and coordinate implicitly by
         * register number instead of spelling two source operands. */
        schema->sources = ps11 ? 0 : 2;
        schema->slot_class = QXP_SHADER_SLOT_TEXTURE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_TEXCOORD) {
        /* ps_1_1 only; it consumes no sampler and no source operands. */
        if (!ps11) return FALSE;
        schema->slot_class = QXP_SHADER_SLOT_TEXTURE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_TEXREG2AR ||
        opcode == QXP_SHADER_OP_TEXREG2GB) {
        /* ps_1_1 only: dependent read of stage N through the coordinate
         * carried by the one texture-file source operand. */
        if (!ps11) return FALSE;
        schema->sources = 1;
        schema->slot_class = QXP_SHADER_SLOT_TEXTURE;
        return TRUE;
    }
    return FALSE;
}


static BOOL qxp_ps_validate_instruction_headers(
    const QXP_SHADER_IR *ir, BOOL ps11,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD previous_end;

    previous_end = 1;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        QXP_PS_OPCODE_SCHEMA schema;
        DWORD expected_tokens;

        instruction = &ir->instructions[index];
        if (instruction->destination_count >
                QXP_SHADER_MAX_DESTINATIONS ||
            instruction->source_count > QXP_SHADER_MAX_SOURCES ||
            instruction->immediate_count > 4)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                index, instruction->token_offset, 0);
        if (!qxp_ps_get_opcode_schema(
                instruction->opcode, ps11, &schema))
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM,
                index, instruction->token_offset, 0);
        /* ps_1_1 has no declaration or texkill forms; only the plain
         * stage-implicit `tex tN` samples. */
        if (ps11 &&
            (instruction->opcode == QXP_SHADER_OP_DCL ||
             instruction->opcode == QXP_SHADER_OP_TEXKILL))
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM,
                index, instruction->token_offset, 0);
        expected_tokens = 1UL + schema.destinations + schema.sources +
                          schema.immediates + schema.extra_tokens;
        if (instruction->destination_count != schema.destinations ||
            instruction->source_count != schema.sources ||
            instruction->immediate_count != schema.immediates ||
            instruction->token_count != expected_tokens ||
            instruction->slot_class != schema.slot_class ||
            instruction->slot_count != schema.slot_count ||
            instruction->token_offset < previous_end ||
            instruction->token_offset >= ir->token_count ||
            instruction->token_count >
                ir->token_count - instruction->token_offset ||
            instruction->token_offset + instruction->token_count >=
                ir->token_count ||
            instruction->control_match != QXP_SHADER_INDEX_NONE ||
            instruction->control_else != QXP_SHADER_INDEX_NONE ||
            instruction->label_target != QXP_SHADER_INDEX_NONE)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                index, instruction->token_offset, 0);
        if (instruction->opcode == QXP_SHADER_OP_TEX) {
            if (instruction->control != QXP_SHADER_CONTROL_NONE &&
                (ps11 ||
                 (instruction->control !=
                      QXP_SHADER_CONTROL_TEX_PROJECT &&
                  instruction->control !=
                      QXP_SHADER_CONTROL_TEX_BIAS)))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                    index, instruction->token_offset, 0);
        } else if (instruction->control != QXP_SHADER_CONTROL_NONE) {
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                index, instruction->token_offset, 0);
        }
        if ((instruction->opcode == QXP_SHADER_OP_DCL) !=
            (instruction->declaration.present != FALSE))
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                index, instruction->token_offset, 0);
        previous_end = instruction->token_offset +
                       instruction->token_count;
    }
    if (ir->block_count != 1 ||
        ir->blocks[0].first_instruction != 0 ||
        ir->blocks[0].instruction_count != ir->instruction_count ||
        ir->blocks[0].successor_count != 0)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    return TRUE;
}

static BOOL qxp_ps_emit_matrix(QXP_PS_GLSL_EMIT *emit,
                               const QXP_SHADER_INSTRUCTION *instruction,
                               DWORD rows, DWORD columns)
{
    DWORD row;
    DWORD expected_mask;

    if (!qxp_ps_shape(instruction, 1, 2, 0) ||
        !qxp_ps_source_valid(&instruction->sources[0]) ||
        !qxp_ps_source_valid(&instruction->sources[1]) ||
        !qxp_ps_swizzle_identity(&instruction->sources[1]) ||
        instruction->sources[1].source_modifier !=
            QXP_SHADER_SRCMOD_NONE ||
        qxp_ps_operands_alias(&instruction->destinations[0],
                              &instruction->sources[0]))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    expected_mask = rows == 4 ? 15 : (rows == 3 ? 7 : 3);
    if (!qxp_ps_temp_destination_valid(
            &instruction->destinations[0],
            QXP_SHADER_DSTMOD_SATURATE |
            QXP_SHADER_DSTMOD_PARTIAL_PRECISION, 0, FALSE) ||
        instruction->destinations[0].write_mask != expected_mask ||
        (instruction->destinations[0].register_file ==
             instruction->sources[1].register_file &&
         (DWORD)instruction->destinations[0].register_number >=
             (DWORD)instruction->sources[1].register_number &&
         (DWORD)instruction->destinations[0].register_number <
             (DWORD)instruction->sources[1].register_number + rows))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    if (!qxp_ps_emit_arguments(emit, instruction)) return FALSE;
    qxp_ps_append(&emit->output, "  qxpValue = vec4(0.0);\n");
    for (row = 0; row < rows; ++row) {
        qxp_ps_append(&emit->output, "  qxpArg1 = ");
        if (!qxp_ps_append_source_register_offset(
                emit, &instruction->sources[1], row)) return FALSE;
        qxp_ps_append(&emit->output, ";\n  qxpValue.");
        qxp_ps_append_char(&emit->output, "xyzw"[row]);
        qxp_ps_append(&emit->output, columns == 4 ?
                      " = dot(qxpArg0, qxpArg1);\n" :
                      " = dot(qxpArg0.xyz, qxpArg1.xyz);\n");
    }
    return qxp_ps_emit_store(
        emit, &instruction->destinations[0], TRUE);
}

static BOOL qxp_ps_emit_texture(
    QXP_PS_GLSL_EMIT *emit, const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    const QXP_SHADER_OPERAND *destination;
    const QXP_SHADER_OPERAND *coordinate;
    const QXP_SHADER_OPERAND *sampler;
    DWORD sampler_type;
    DWORD sampler_number;
    const char *function_name;
    const char *coordinate_suffix;

    if (emit->ps11) {
        /* ps_1_1 stage-implicit forms.  `tex tN` samples 2D sampler N at the
         * stage-N coordinate; `texcoord tN` delivers that coordinate as a
         * colour instead, clamped to [0,1] with alpha 1.  Both overwrite the
         * mutable tN local and name their stage by destination number.
         * `texreg2ar tN, tM` / `texreg2gb tN, tM` are the profile's one
         * dependent read: they sample 2D sampler N at (tM.a, tM.r) or
         * (tM.g, tM.b) and overwrite tN with the result. */
        BOOL coordinate_only;
        BOOL reg2_ar;

        coordinate_only = instruction->opcode == QXP_SHADER_OP_TEXCOORD;
        reg2_ar = instruction->opcode == QXP_SHADER_OP_TEXREG2AR;
        if (!coordinate_only && !reg2_ar &&
            instruction->opcode != QXP_SHADER_OP_TEXREG2GB &&
            instruction->opcode != QXP_SHADER_OP_TEX)
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
        if (!qxp_ps_shape(instruction, 1,
                          coordinate_only || instruction->opcode ==
                          QXP_SHADER_OP_TEX ? 0 : 1, 0))
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
        destination = &instruction->destinations[0];
        if (destination->role != QXP_SHADER_OPERAND_DESTINATION ||
            destination->register_file != QXP_SHADER_REG_TEXTURE ||
            destination->register_number >= 8 ||
            destination->write_mask != 15 ||
            destination->destination_modifier !=
                QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0 ||
            destination->relative.present)
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
        sampler_number = destination->register_number;
        if (!coordinate_only &&
            (!(metadata->sampler_mask & (1UL << sampler_number)) ||
             metadata->sampler_types[sampler_number] !=
                 QXP_SHADER_SAMPLER_2D))
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION);
        if (instruction->opcode != QXP_SHADER_OP_TEX) {
            const QXP_SHADER_OPERAND *source = &instruction->sources[0];
            DWORD i;

            if (source->role != QXP_SHADER_OPERAND_SOURCE ||
                source->register_file != QXP_SHADER_REG_TEXTURE ||
                source->register_number >= 8 ||
                source->source_modifier != QXP_SHADER_SRCMOD_NONE ||
                source->relative.present)
                return qxp_ps_emit_fail(
                    emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
            for (i = 0; i < 4; ++i) {
                if (source->swizzle[i] != i)
                    return qxp_ps_emit_fail(
                        emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
            }
            qxp_ps_append(&emit->output, "  qxpT");
            qxp_ps_append_uint(&emit->output, sampler_number);
            qxp_ps_append(&emit->output, " = texture2D(qxpPsS");
            qxp_ps_append_uint(&emit->output, sampler_number);
            qxp_ps_append(&emit->output, ", vec2(qxpT");
            qxp_ps_append_uint(&emit->output, source->register_number);
            /* texreg2ar reads (alpha, red); texreg2gb reads
             * (green, blue). */
            if (reg2_ar)
                qxp_ps_append(&emit->output, ".a, qxpT");
            else
                qxp_ps_append(&emit->output, ".g, qxpT");
            qxp_ps_append_uint(&emit->output, source->register_number);
            if (reg2_ar)
                qxp_ps_append(&emit->output, ".r));\n");
            else
                qxp_ps_append(&emit->output, ".b));\n");
            return emit->output.valid;
        }
        if (coordinate_only) {
            qxp_ps_append(&emit->output, "  qxpT");
            qxp_ps_append_uint(&emit->output, sampler_number);
            qxp_ps_append(&emit->output, " = vec4(clamp(qxpPsInT");
            qxp_ps_append_uint(&emit->output, sampler_number);
            qxp_ps_append(&emit->output, ".xyz, 0.0, 1.0), 1.0);\n");
            return emit->output.valid;
        }
        qxp_ps_append(&emit->output, "  qxpT");
        qxp_ps_append_uint(&emit->output, sampler_number);
        qxp_ps_append(&emit->output, " = texture2D(qxpPsS");
        qxp_ps_append_uint(&emit->output, sampler_number);
        qxp_ps_append(&emit->output, ", qxpPsInT");
        qxp_ps_append_uint(&emit->output, sampler_number);
        qxp_ps_append(&emit->output, ".xy);\n");
        return emit->output.valid;
    }

    if (!qxp_ps_shape(instruction, 1, 2, 0))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    destination = &instruction->destinations[0];
    coordinate = &instruction->sources[0];
    sampler = &instruction->sources[1];
    if (!qxp_ps_temp_destination_valid(
            destination, QXP_SHADER_DSTMOD_PARTIAL_PRECISION, 0,
            FALSE) ||
        destination->write_mask != 15 ||
        !qxp_ps_source_valid(coordinate) ||
        !qxp_ps_swizzle_identity(coordinate) ||
        (coordinate->register_file != QXP_SHADER_REG_TEMP &&
         coordinate->register_file != QXP_SHADER_REG_TEXTURE) ||
        coordinate->source_modifier != QXP_SHADER_SRCMOD_NONE ||
        sampler->role != QXP_SHADER_OPERAND_SOURCE ||
        sampler->register_file != QXP_SHADER_REG_SAMPLER ||
        sampler->register_number >= QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT ||
        sampler->source_modifier != QXP_SHADER_SRCMOD_NONE ||
        !qxp_ps_swizzle_identity(sampler) || sampler->relative.present)
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    sampler_number = sampler->register_number;
    if (!(metadata->sampler_mask & (1UL << sampler_number)))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION);
    sampler_type = metadata->sampler_types[sampler_number];
    if (sampler_type == QXP_SHADER_SAMPLER_2D) {
        function_name = "texture2D";
        coordinate_suffix = ".xy";
    } else if (sampler_type == QXP_SHADER_SAMPLER_CUBE) {
        function_name = "textureCube";
        coordinate_suffix = ".xyz";
    } else if (sampler_type == QXP_SHADER_SAMPLER_VOLUME) {
        function_name = "texture3D";
        coordinate_suffix = ".xyz";
    } else {
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION);
    }
    qxp_ps_append(&emit->output, "  qxpArg0 = ");
    if (!qxp_ps_append_source(emit, coordinate)) return FALSE;
    qxp_ps_append(&emit->output, ";\n  qxpValue = ");
    qxp_ps_append(&emit->output, function_name);
    qxp_ps_append(&emit->output, "(qxpPsS");
    qxp_ps_append_uint(&emit->output, sampler_number);
    qxp_ps_append(&emit->output, ", qxpArg0");
    qxp_ps_append(&emit->output, coordinate_suffix);
    if (instruction->control == QXP_SHADER_CONTROL_TEX_PROJECT)
        qxp_ps_append(&emit->output, " / qxpArg0.w");
    else if (instruction->control != QXP_SHADER_CONTROL_NONE &&
             instruction->control != QXP_SHADER_CONTROL_TEX_BIAS)
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (instruction->control == QXP_SHADER_CONTROL_TEX_BIAS)
        qxp_ps_append(&emit->output, ", qxpArg0.w");
    qxp_ps_append(&emit->output, ");\n");
    return qxp_ps_emit_store(emit, destination, FALSE);
}

static BOOL qxp_ps_emit_output(
    QXP_PS_GLSL_EMIT *emit,
    const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    const QXP_SHADER_OPERAND *destination;

    if (!qxp_ps_shape(instruction, 1, 1, 0) ||
        instruction->opcode != QXP_SHADER_OP_MOV)
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    destination = &instruction->destinations[0];
    if (destination->role != QXP_SHADER_OPERAND_DESTINATION ||
        destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0 ||
        destination->relative.present ||
        !qxp_ps_source_valid(&instruction->sources[0]))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    if (destination->register_file == QXP_SHADER_REG_COLOROUT &&
        destination->register_number < 4) {
        if (!qxp_ps_swizzle_identity(&instruction->sources[0]) ||
            instruction->sources[0].source_modifier !=
                QXP_SHADER_SRCMOD_NONE)
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
        if (!qxp_ps_emit_arguments(emit, instruction)) return FALSE;
        if (metadata->color_output_mask == 1)
            qxp_ps_append(&emit->output,
                          "  gl_FragColor = qxpArg0;\n");
        else {
            qxp_ps_append(&emit->output, "  gl_FragData[");
            qxp_ps_append_uint(&emit->output,
                               destination->register_number);
            qxp_ps_append(&emit->output, "] = qxpArg0;\n");
        }
    } else if (destination->register_file == QXP_SHADER_REG_DEPTHOUT &&
               destination->register_number == 0) {
        if (!qxp_ps_swizzle_replicate(&instruction->sources[0]) ||
            instruction->sources[0].source_modifier !=
                QXP_SHADER_SRCMOD_NONE)
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
        if (!qxp_ps_emit_arguments(emit, instruction)) return FALSE;
        qxp_ps_append(&emit->output, "  gl_FragDepth = qxpArg0.x;\n");
    } else {
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    }
    return emit->output.valid;
}

static BOOL qxp_ps_emit_arithmetic(
    QXP_PS_GLSL_EMIT *emit, const QXP_SHADER_INSTRUCTION *instruction)
{
    const QXP_SHADER_OPERAND *destination;
    DWORD opcode;
    DWORD sources;
    BOOL allow_saturate;

    opcode = instruction->opcode;
    sources = 0;
    if (opcode == QXP_SHADER_OP_MOV || opcode == QXP_SHADER_OP_RCP ||
        opcode == QXP_SHADER_OP_RSQ || opcode == QXP_SHADER_OP_EXP ||
        opcode == QXP_SHADER_OP_LOG || opcode == QXP_SHADER_OP_FRC ||
        opcode == QXP_SHADER_OP_ABS || opcode == QXP_SHADER_OP_NRM)
        sources = 1;
    else if (opcode == QXP_SHADER_OP_ADD ||
             opcode == QXP_SHADER_OP_SUB ||
             opcode == QXP_SHADER_OP_MUL ||
             opcode == QXP_SHADER_OP_DP3 ||
             opcode == QXP_SHADER_OP_DP4 ||
             opcode == QXP_SHADER_OP_MIN ||
             opcode == QXP_SHADER_OP_MAX ||
             opcode == QXP_SHADER_OP_POW ||
             opcode == QXP_SHADER_OP_CRS)
        sources = 2;
    else if (opcode == QXP_SHADER_OP_MAD ||
             opcode == QXP_SHADER_OP_CMP ||
             opcode == QXP_SHADER_OP_LRP ||
             opcode == QXP_SHADER_OP_DP2ADD ||
             opcode == QXP_SHADER_OP_SINCOS)
        sources = 3;
    else
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM);
    if (!qxp_ps_shape(instruction, 1, sources, 0))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    destination = &instruction->destinations[0];
    allow_saturate = opcode != QXP_SHADER_OP_FRC &&
                     opcode != QXP_SHADER_OP_SINCOS;
    if (!qxp_ps_temp_destination_valid(
            destination,
            emit->ps11 ? QXP_SHADER_DSTMOD_SATURATE :
                         (QXP_SHADER_DSTMOD_SATURATE |
                          QXP_SHADER_DSTMOD_PARTIAL_PRECISION),
            emit->ps11 ? 2 : 0, emit->ps11) ||
        (!allow_saturate &&
         (destination->destination_modifier &
          QXP_SHADER_DSTMOD_SATURATE)))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
    if ((opcode == QXP_SHADER_OP_RCP ||
         opcode == QXP_SHADER_OP_RSQ ||
         opcode == QXP_SHADER_OP_EXP ||
         opcode == QXP_SHADER_OP_LOG) &&
        !qxp_ps_swizzle_replicate(&instruction->sources[0]))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (opcode == QXP_SHADER_OP_POW &&
        (!qxp_ps_swizzle_replicate(&instruction->sources[0]) ||
         !qxp_ps_swizzle_replicate(&instruction->sources[1]) ||
         qxp_ps_operands_alias(destination,
                               &instruction->sources[1])))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (opcode == QXP_SHADER_OP_NRM &&
        ((destination->write_mask != 7 &&
          destination->write_mask != 15) ||
         qxp_ps_operands_alias(destination,
                               &instruction->sources[0])))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (opcode == QXP_SHADER_OP_DP2ADD &&
        !qxp_ps_swizzle_replicate(&instruction->sources[2]))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (opcode == QXP_SHADER_OP_CRS &&
        ((destination->write_mask & 8) != 0 ||
         qxp_ps_operands_alias(destination,
                               &instruction->sources[0]) ||
         qxp_ps_operands_alias(destination,
                               &instruction->sources[1]) ||
         !qxp_ps_swizzle_identity(&instruction->sources[0]) ||
         !qxp_ps_swizzle_identity(&instruction->sources[1])))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (opcode == QXP_SHADER_OP_SINCOS &&
        ((destination->write_mask != 1 &&
          destination->write_mask != 2 &&
          destination->write_mask != 3) ||
         qxp_ps_operands_alias(destination,
                               &instruction->sources[0]) ||
         !qxp_ps_swizzle_replicate(&instruction->sources[0]) ||
         instruction->sources[1].register_file !=
             QXP_SHADER_REG_CONST ||
         instruction->sources[2].register_file !=
             QXP_SHADER_REG_CONST ||
         instruction->sources[1].register_number ==
             instruction->sources[2].register_number ||
         !qxp_ps_swizzle_identity(&instruction->sources[1]) ||
         !qxp_ps_swizzle_identity(&instruction->sources[2]) ||
         instruction->sources[1].source_modifier !=
             QXP_SHADER_SRCMOD_NONE ||
         instruction->sources[2].source_modifier !=
             QXP_SHADER_SRCMOD_NONE))
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
    if (!qxp_ps_emit_arguments(emit, instruction)) return FALSE;

    if (opcode == QXP_SHADER_OP_MOV)
        qxp_ps_append(&emit->output, "  qxpValue = qxpArg0;\n");
    else if (opcode == QXP_SHADER_OP_ADD)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpArg0 + qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_SUB)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpArg0 - qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_MUL)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpArg0 * qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_MAD)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpArg0*qxpArg1 + qxpArg2;\n");
    else if (opcode == QXP_SHADER_OP_DP3)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(dot(qxpArg0.xyz, qxpArg1.xyz));\n");
    else if (opcode == QXP_SHADER_OP_DP4)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(dot(qxpArg0, qxpArg1));\n");
    else if (opcode == QXP_SHADER_OP_MIN)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpPsMin(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_MAX)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpPsMax(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_RCP)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(1.0/qxpArg0.x);\n");
    else if (opcode == QXP_SHADER_OP_RSQ)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(inversesqrt(abs(qxpArg0.x)));\n");
    else if (opcode == QXP_SHADER_OP_EXP)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(exp2(qxpArg0.x));\n");
    else if (opcode == QXP_SHADER_OP_LOG)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(log2(abs(qxpArg0.x)));\n");
    else if (opcode == QXP_SHADER_OP_FRC)
        qxp_ps_append(&emit->output,
                      "  qxpValue = fract(qxpArg0);\n");
    else if (opcode == QXP_SHADER_OP_ABS)
        qxp_ps_append(&emit->output,
                      "  qxpValue = abs(qxpArg0);\n");
    else if (opcode == QXP_SHADER_OP_CMP)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpPsCmp(qxpArg0, qxpArg1, qxpArg2);\n");
    else if (opcode == QXP_SHADER_OP_LRP)
        qxp_ps_append(&emit->output,
                      "  qxpValue = qxpArg0*qxpArg1 + (vec4(1.0)-qxpArg0)*qxpArg2;\n");
    else if (opcode == QXP_SHADER_OP_POW)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(pow(abs(qxpArg0.x), qxpArg1.x));\n");
    else if (opcode == QXP_SHADER_OP_CRS)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(cross(qxpArg0.xyz, qxpArg1.xyz), 0.0);\n");
    else if (opcode == QXP_SHADER_OP_DP2ADD)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(dot(qxpArg0.xy, qxpArg1.xy) + qxpArg2.x);\n");
    else if (opcode == QXP_SHADER_OP_NRM)
        qxp_ps_append(&emit->output,
                      "  qxpScalar = dot(qxpArg0.xyz, qxpArg0.xyz);\n"
                      "  qxpValue = qxpArg0 * ((qxpScalar != 0.0) ? inversesqrt(qxpScalar) : 3.402823466e+38);\n");
    else if (opcode == QXP_SHADER_OP_SINCOS)
        qxp_ps_append(&emit->output,
                      "  qxpValue = vec4(cos(qxpArg0.x), sin(qxpArg0.x), 0.0, 0.0);\n");
    else
        return qxp_ps_emit_fail(
            emit, QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM);
    return qxp_ps_emit_store(emit, destination, allow_saturate);
}

static BOOL qxp_ps_emit_instruction(
    QXP_PS_GLSL_EMIT *emit, const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    const QXP_SHADER_OPERAND *destination;
    DWORD opcode;

    opcode = instruction->opcode;
    if (opcode == QXP_SHADER_OP_DCL || opcode == QXP_SHADER_OP_DEF)
        return TRUE;
    if (opcode == QXP_SHADER_OP_NOP) {
        if (!qxp_ps_shape(instruction, 0, 0, 0))
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
        qxp_ps_append(&emit->output, "  /* nop */\n");
        return emit->output.valid;
    }
    if (instruction->destination_count) {
        destination = &instruction->destinations[0];
        if (destination->register_file == QXP_SHADER_REG_COLOROUT ||
            destination->register_file == QXP_SHADER_REG_DEPTHOUT)
            return qxp_ps_emit_output(emit, instruction, metadata);
    }
    if (opcode == QXP_SHADER_OP_TEX ||
        opcode == QXP_SHADER_OP_TEXCOORD ||
        opcode == QXP_SHADER_OP_TEXREG2AR ||
        opcode == QXP_SHADER_OP_TEXREG2GB)
        return qxp_ps_emit_texture(emit, instruction, metadata);
    if (opcode == QXP_SHADER_OP_TEXKILL) {
        if (!qxp_ps_shape(instruction, 1, 0, 0))
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION);
        destination = &instruction->destinations[0];
        if (destination->role != QXP_SHADER_OPERAND_DESTINATION ||
            destination->write_mask != 15 ||
            destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0 ||
            destination->relative.present ||
            (destination->register_file != QXP_SHADER_REG_TEMP &&
             destination->register_file != QXP_SHADER_REG_TEXTURE))
            return qxp_ps_emit_fail(
                emit, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER);
        qxp_ps_append(&emit->output, "  qxpArg0 = ");
        if (!qxp_ps_append_register(
                emit, destination->register_file,
                destination->register_number)) return FALSE;
        qxp_ps_append(&emit->output,
                      ";\n  if (any(lessThan(qxpArg0.xyz, vec3(0.0)))) discard;\n");
        return emit->output.valid;
    }
    if (opcode == QXP_SHADER_OP_M4X4)
        return qxp_ps_emit_matrix(emit, instruction, 4, 4);
    if (opcode == QXP_SHADER_OP_M4X3)
        return qxp_ps_emit_matrix(emit, instruction, 3, 4);
    if (opcode == QXP_SHADER_OP_M3X4)
        return qxp_ps_emit_matrix(emit, instruction, 4, 3);
    if (opcode == QXP_SHADER_OP_M3X3)
        return qxp_ps_emit_matrix(emit, instruction, 3, 3);
    if (opcode == QXP_SHADER_OP_M3X2)
        return qxp_ps_emit_matrix(emit, instruction, 2, 3);
    return qxp_ps_emit_arithmetic(emit, instruction);
}

static BOOL qxp_ps_matrix_opcode(DWORD opcode)
{
    return opcode == QXP_SHADER_OP_M4X4 ||
           opcode == QXP_SHADER_OP_M4X3 ||
           opcode == QXP_SHADER_OP_M3X4 ||
           opcode == QXP_SHADER_OP_M3X3 ||
           opcode == QXP_SHADER_OP_M3X2;
}

static DWORD qxp_ps_matrix_rows(DWORD opcode)
{
    if (opcode == QXP_SHADER_OP_M4X4 ||
        opcode == QXP_SHADER_OP_M3X4) return 4;
    if (opcode == QXP_SHADER_OP_M4X3 ||
        opcode == QXP_SHADER_OP_M3X3) return 3;
    return 2;
}

static DWORD qxp_ps_source_logical_mask(
    const QXP_SHADER_INSTRUCTION *instruction, DWORD source_index)
{
    DWORD destination_mask;
    DWORD opcode;

    destination_mask = instruction->destination_count ?
        instruction->destinations[0].write_mask : 15;
    opcode = instruction->opcode;
    if (opcode == QXP_SHADER_OP_DP3) return 7;
    if (opcode == QXP_SHADER_OP_DP4) return 15;
    if (qxp_ps_matrix_opcode(opcode))
        return opcode == QXP_SHADER_OP_M4X4 ||
               opcode == QXP_SHADER_OP_M4X3 ? 15 : 7;
    if (opcode == QXP_SHADER_OP_DP2ADD)
        return source_index < 2 ? 3 : 1;
    if (opcode == QXP_SHADER_OP_RCP || opcode == QXP_SHADER_OP_RSQ ||
        opcode == QXP_SHADER_OP_EXP || opcode == QXP_SHADER_OP_LOG ||
        opcode == QXP_SHADER_OP_POW)
        return 1;
    if (opcode == QXP_SHADER_OP_SINCOS)
        return source_index ? 15 : 1;
    if (opcode == QXP_SHADER_OP_NRM)
        return 7 | (destination_mask & 8);
    if (opcode == QXP_SHADER_OP_CRS) return 7;
    if (opcode == QXP_SHADER_OP_TEX) return 15;
    return destination_mask;
}

static DWORD qxp_ps_swizzled_mask(
    const QXP_SHADER_OPERAND *operand, DWORD logical_mask)
{
    DWORD mask;
    DWORD index;

    mask = 0;
    for (index = 0; index < 4; ++index) {
        if (logical_mask & (1UL << index))
            mask |= 1UL << operand->swizzle[index];
    }
    return mask;
}

static BOOL qxp_ps_baseline_swizzle(
    const QXP_SHADER_OPERAND *operand)
{
    DWORD value;

    value = (DWORD)operand->swizzle[0] |
            ((DWORD)operand->swizzle[1] << 2) |
            ((DWORD)operand->swizzle[2] << 4) |
            ((DWORD)operand->swizzle[3] << 6);
    if (value == 0xe4UL || value == 0xc9UL || value == 0xd2UL ||
        value == 0x1bUL) return TRUE;
    return operand->swizzle[0] == operand->swizzle[1] &&
           operand->swizzle[0] == operand->swizzle[2] &&
           operand->swizzle[0] == operand->swizzle[3];
}

static BOOL qxp_ps_same_register(
    const QXP_SHADER_OPERAND *left,
    const QXP_SHADER_OPERAND *right)
{
    return left->register_file == right->register_file &&
           left->register_number == right->register_number &&
           left->relative.present == right->relative.present;
}


static BOOL qxp_ps_validate_read_ports(
    const QXP_SHADER_INSTRUCTION *instruction,
    DWORD instruction_index,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_OPERAND *first_constant;
    const QXP_SHADER_OPERAND *first_input;
    const QXP_SHADER_OPERAND *first_texture;
    DWORD constant_reads;
    DWORD texture_reads;
    DWORD source_index;
    BOOL sincos;

    if (instruction->opcode == QXP_SHADER_OP_DCL ||
        instruction->opcode == QXP_SHADER_OP_DEF ||
        instruction->opcode == QXP_SHADER_OP_TEX ||
        instruction->opcode == QXP_SHADER_OP_TEXCOORD ||
        instruction->opcode == QXP_SHADER_OP_TEXKILL ||
        instruction->opcode == QXP_SHADER_OP_NOP)
        return TRUE;
    sincos = instruction->opcode == QXP_SHADER_OP_SINCOS;
    if (qxp_ps_matrix_opcode(instruction->opcode)) {
        const QXP_SHADER_OPERAND *source0;
        const QXP_SHADER_OPERAND *source1;

        source0 = &instruction->sources[0];
        source1 = &instruction->sources[1];
        if ((source0->register_file == QXP_SHADER_REG_INPUT &&
             source1->register_file == QXP_SHADER_REG_INPUT) ||
            (source0->register_file == QXP_SHADER_REG_TEXTURE &&
             source1->register_file == QXP_SHADER_REG_TEXTURE) ||
            (source0->register_file == QXP_SHADER_REG_CONST &&
             source1->register_file == QXP_SHADER_REG_CONST))
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                instruction_index, instruction->token_offset, 0);
    }
    first_constant = (const QXP_SHADER_OPERAND *)0;
    first_input = (const QXP_SHADER_OPERAND *)0;
    first_texture = (const QXP_SHADER_OPERAND *)0;
    constant_reads = 0;
    texture_reads = 0;
    for (source_index = 0;
         source_index < instruction->source_count; ++source_index) {
        const QXP_SHADER_OPERAND *source;
        const QXP_SHADER_OPERAND **first_port;

        source = &instruction->sources[source_index];
        if (source->register_file == QXP_SHADER_REG_CONST && !sincos) {
            if (++constant_reads > 2)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                    instruction_index, instruction->token_offset, 0);
            if (!first_constant)
                first_constant = source;
            else if (!qxp_ps_same_register(first_constant, source))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                    instruction_index, instruction->token_offset, 0);
        }
        first_port = (const QXP_SHADER_OPERAND **)0;
        if (source->register_file == QXP_SHADER_REG_INPUT) {
            first_port = &first_input;
        } else if (source->register_file == QXP_SHADER_REG_TEXTURE) {
            if (++texture_reads > 1)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                    instruction_index, instruction->token_offset, 0);
            first_port = &first_texture;
        }
        if (!first_port) continue;
        if (!*first_port)
            *first_port = source;
        else if (!qxp_ps_same_register(*first_port, source))
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                instruction_index, instruction->token_offset, 0);
    }
    return TRUE;
}

static DWORD qxp_ps_max(DWORD left, DWORD right)
{
    return left > right ? left : right;
}

static DWORD qxp_ps_register_dependency(
    DWORD dependencies[QXP_SHADER_MAX_PS_TEMPS][4],
    DWORD register_file, DWORD register_number, DWORD component_mask)
{
    DWORD dependency;
    DWORD index;

    if (register_file != QXP_SHADER_REG_TEMP ||
        register_number >= QXP_SHADER_MAX_PS_TEMPS) return 0;
    dependency = 0;
    for (index = 0; index < 4; ++index) {
        if (component_mask & (1UL << index))
            dependency = qxp_ps_max(
                dependency, dependencies[register_number][index]);
    }
    return dependency;
}

static DWORD qxp_ps_operand_dependency(
    DWORD dependencies[QXP_SHADER_MAX_PS_TEMPS][4],
    const QXP_SHADER_OPERAND *operand, DWORD logical_mask)
{
    return qxp_ps_register_dependency(
        dependencies, operand->register_file, operand->register_number,
        qxp_ps_swizzled_mask(operand, logical_mask));
}

static DWORD qxp_ps_result_dependency(
    DWORD dependencies[QXP_SHADER_MAX_PS_TEMPS][4],
    const QXP_SHADER_INSTRUCTION *instruction, DWORD component)
{
    const QXP_SHADER_OPERAND *matrix;
    DWORD dependency;
    DWORD logical_mask;
    DWORD opcode;
    DWORD index;

    dependency = 0;
    opcode = instruction->opcode;
    if (qxp_ps_matrix_opcode(opcode)) {
        logical_mask = opcode == QXP_SHADER_OP_M4X4 ||
                       opcode == QXP_SHADER_OP_M4X3 ? 15 : 7;
        dependency = qxp_ps_operand_dependency(
            dependencies, &instruction->sources[0], logical_mask);
        matrix = &instruction->sources[1];
        return qxp_ps_max(
            dependency,
            qxp_ps_register_dependency(
                dependencies, matrix->register_file,
                (DWORD)matrix->register_number + component,
                logical_mask));
    }
    if (opcode == QXP_SHADER_OP_DP3) logical_mask = 7;
    else if (opcode == QXP_SHADER_OP_DP4) logical_mask = 15;
    else if (opcode == QXP_SHADER_OP_DP2ADD) {
        dependency = qxp_ps_operand_dependency(
            dependencies, &instruction->sources[0], 3);
        dependency = qxp_ps_max(
            dependency, qxp_ps_operand_dependency(
                dependencies, &instruction->sources[1], 3));
        return qxp_ps_max(
            dependency, qxp_ps_operand_dependency(
                dependencies, &instruction->sources[2], 1));
    } else if (opcode == QXP_SHADER_OP_CRS) {
        logical_mask = component == 0 ? 6 :
                       (component == 1 ? 5 : 3);
    } else if (opcode == QXP_SHADER_OP_NRM) {
        logical_mask = 7 | (1UL << component);
    } else if (opcode == QXP_SHADER_OP_RCP ||
               opcode == QXP_SHADER_OP_RSQ ||
               opcode == QXP_SHADER_OP_EXP ||
               opcode == QXP_SHADER_OP_LOG ||
               opcode == QXP_SHADER_OP_POW ||
               opcode == QXP_SHADER_OP_SINCOS) {
        logical_mask = 1;
    } else {
        logical_mask = 1UL << component;
    }
    for (index = 0; index < instruction->source_count; ++index) {
        if (opcode == QXP_SHADER_OP_SINCOS && index != 0) continue;
        dependency = qxp_ps_max(
            dependency, qxp_ps_operand_dependency(
                dependencies, &instruction->sources[index],
                logical_mask));
    }
    return dependency;
}

/*
 * ps_1_1 summary cross-check.  The ps_2_0 read-port, dependent-read, and
 * output-register rules do not apply: r0 is the colour, t# is always
 * defined, and the only recordable capability is the arbitrary-swizzle
 * bit the decoder already mirrors.  Recompute the temp and slot totals
 * from the operands so a hand-mutated summary still rejects.
 */
static BOOL qxp_ps_validate_ps11_summary(
    const QXP_SHADER_IR *ir,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD operand_index;
    DWORD required_temps;
    DWORD arithmetic_slots;
    DWORD texture_slots;
    DWORD total_slots;
    DWORD expected_caps;

    required_temps = 0;
    arithmetic_slots = 0;
    texture_slots = 0;
    total_slots = 0;
    expected_caps = 0;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[index];
        total_slots += instruction->slot_count;
        if (instruction->slot_class == QXP_SHADER_SLOT_ARITHMETIC)
            arithmetic_slots += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            texture_slots += instruction->slot_count;
        else if (instruction->slot_class != QXP_SHADER_SLOT_SETUP)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                index, instruction->token_offset, 0);
        if (instruction->opcode == QXP_SHADER_OP_DEF)
            continue;
        for (operand_index = 0;
             operand_index < instruction->destination_count;
             ++operand_index) {
            const QXP_SHADER_OPERAND *operand;
            DWORD required;

            operand = &instruction->destinations[operand_index];
            if (operand->register_file == QXP_SHADER_REG_TEMP) {
                if (operand->register_number >=
                    QXP_SHADER_MAX_PS_TEMPS)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                        index, instruction->token_offset, 0);
                required = (DWORD)operand->register_number + 1;
                if (required > required_temps)
                    required_temps = required;
            } else if (operand->register_file !=
                           QXP_SHADER_REG_TEXTURE ||
                       operand->register_number >= 8) {
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                    index, instruction->token_offset, 0);
            }
        }
        for (operand_index = 0;
             operand_index < instruction->source_count;
             ++operand_index) {
            const QXP_SHADER_OPERAND *operand;
            DWORD required;

            operand = &instruction->sources[operand_index];
            if (!qxp_ps_source_valid(operand))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                    index, instruction->token_offset, 0);
            if (!qxp_ps_baseline_swizzle(operand))
                expected_caps |= QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE;
            if (operand->register_file != QXP_SHADER_REG_TEMP)
                continue;
            required = (DWORD)operand->register_number + 1;
            if (required > required_temps) required_temps = required;
        }
    }
    if (required_temps != ir->required_ps_temp_count ||
        total_slots != ir->slot_count ||
        arithmetic_slots != ir->arithmetic_slot_count ||
        texture_slots != ir->texture_slot_count ||
        ir->flow_slot_count != 0 ||
        total_slots != ir->required_ps_instruction_slots ||
        expected_caps != ir->required_ps20_caps)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    return TRUE;
}

static BOOL qxp_ps_validate_ir_summary(
    const QXP_SHADER_IR *ir,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD required_temps;
    DWORD arithmetic_slots;
    DWORD texture_slots;
    DWORD flow_slots;
    DWORD total_slots;
    DWORD required_slots;
    DWORD expected_caps;
    DWORD output_mask;
    BOOL no_texture_limit;
    BOOL arbitrary_swizzle;
    BOOL depth_written;
    BOOL dependent_read_limit;
    BYTE temp_masks[QXP_SHADER_MAX_PS_TEMPS];
    DWORD temp_dependencies[QXP_SHADER_MAX_PS_TEMPS][4];

    if (ir->version_token == QXP_SHADER_VERSION_PS_1_1)
        return qxp_ps_validate_ps11_summary(ir, error);

    required_temps = 0;
    arithmetic_slots = 0;
    texture_slots = 0;
    flow_slots = 0;
    total_slots = 0;
    output_mask = 0;
    arbitrary_swizzle = FALSE;
    depth_written = FALSE;
    dependent_read_limit = FALSE;
    qxp_ps_zero(temp_masks, (DWORD)sizeof(temp_masks));
    qxp_ps_zero(temp_dependencies,
                (DWORD)sizeof(temp_dependencies));
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD operand_index;

        instruction = &ir->instructions[index];
        if (!qxp_ps_validate_read_ports(
                instruction, index, error)) return FALSE;
        total_slots += instruction->slot_count;
        if (instruction->slot_class == QXP_SHADER_SLOT_ARITHMETIC)
            arithmetic_slots += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            texture_slots += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_FLOW)
            flow_slots += instruction->slot_count;
        else if (instruction->slot_class != QXP_SHADER_SLOT_SETUP)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                index, instruction->token_offset, 0);
        for (operand_index = 0;
             operand_index < instruction->destination_count;
             ++operand_index) {
            const QXP_SHADER_OPERAND *operand;
            DWORD required;

            operand = &instruction->destinations[operand_index];
            if ((operand->register_file == QXP_SHADER_REG_TEMP &&
                 operand->register_number >= QXP_SHADER_MAX_PS_TEMPS) ||
                (operand->register_file == QXP_SHADER_REG_COLOROUT &&
                 operand->register_number >= 4) ||
                (operand->register_file == QXP_SHADER_REG_DEPTHOUT &&
                 operand->register_number != 0))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                    index, instruction->token_offset, 0);
            if (operand->register_file == QXP_SHADER_REG_TEMP) {
                required = (DWORD)operand->register_number + 1;
                if (required > required_temps) required_temps = required;
            }
            if (operand->register_file == QXP_SHADER_REG_COLOROUT) {
                DWORD bit;

                bit = 1UL << operand->register_number;
                if (output_mask & bit)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                        index, instruction->token_offset, 0);
                output_mask |= bit;
            } else if (operand->register_file ==
                       QXP_SHADER_REG_DEPTHOUT) {
                if (depth_written)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                        index, instruction->token_offset, 0);
                depth_written = TRUE;
            }
        }
        for (operand_index = 0;
             operand_index < instruction->source_count;
             ++operand_index) {
            const QXP_SHADER_OPERAND *operand;
            DWORD required;
            DWORD register_offset;
            DWORD component_mask;
            DWORD declared_mask;

            operand = &instruction->sources[operand_index];
            if (operand->register_file == QXP_SHADER_REG_SAMPLER) {
                if (operand->role != QXP_SHADER_OPERAND_SOURCE ||
                    operand->register_number >=
                        QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT ||
                    operand->source_modifier !=
                        QXP_SHADER_SRCMOD_NONE ||
                    !qxp_ps_swizzle_identity(operand) ||
                    operand->relative.present)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                        index, instruction->token_offset, 0);
                continue;
            }
            if (!qxp_ps_source_valid(operand))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                    index, instruction->token_offset, 0);
            if (!qxp_ps_baseline_swizzle(operand))
                arbitrary_swizzle = TRUE;
            register_offset = 0;
            if (operand_index == 1 &&
                qxp_ps_matrix_opcode(instruction->opcode))
                register_offset = qxp_ps_matrix_rows(
                    instruction->opcode) - 1;
            if (operand->register_file == QXP_SHADER_REG_TEMP) {
                required = (DWORD)operand->register_number +
                           register_offset + 1;
                if (required > required_temps) required_temps = required;
            }
            if (instruction->opcode == QXP_SHADER_OP_TEX &&
                operand_index == 0) {
                DWORD logical_mask;
                DWORD sampler_number;

                sampler_number =
                    instruction->sources[1].register_number;
                if (sampler_number >=
                    QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                        index, instruction->token_offset, 0);
                logical_mask = metadata->sampler_types[sampler_number] ==
                    QXP_SHADER_SAMPLER_2D ? 3 : 7;
                if (instruction->control ==
                        QXP_SHADER_CONTROL_TEX_PROJECT ||
                    instruction->control == QXP_SHADER_CONTROL_TEX_BIAS)
                    logical_mask |= 8;
                component_mask = qxp_ps_swizzled_mask(
                    operand, logical_mask);
            } else {
                component_mask = qxp_ps_swizzled_mask(
                    operand, qxp_ps_source_logical_mask(
                        instruction, operand_index));
            }
            if (operand->register_file == QXP_SHADER_REG_TEMP) {
                DWORD row;

                for (row = 0; row <= register_offset; ++row) {
                    DWORD number;

                    number = (DWORD)operand->register_number + row;
                    if (number >= QXP_SHADER_MAX_PS_TEMPS ||
                        (component_mask & ~temp_masks[number]) != 0)
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                            index, instruction->token_offset, 0);
                }
            } else if (operand->register_file == QXP_SHADER_REG_INPUT) {
                DWORD row;

                for (row = 0; row <= register_offset; ++row) {
                    DWORD number;

                    number = (DWORD)operand->register_number + row;
                    if (number >= 2 ||
                        !(metadata->input_color_mask &
                          (1UL << number)))
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                            index, instruction->token_offset, 0);
                }
                /* Omitted v# channels are the ps_2_0 (0,0,0,1)
                 * defaults and remain readable after any nonempty DCL. */
            } else if (operand->register_file ==
                       QXP_SHADER_REG_TEXTURE) {
                DWORD row;

                for (row = 0; row <= register_offset; ++row) {
                    DWORD number;

                    number = (DWORD)operand->register_number + row;
                    if (number >= 8 ||
                        !(metadata->input_texture_mask &
                          (1UL << number)))
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                            index, instruction->token_offset, 0);
                    declared_mask =
                        metadata->input_texture_components[number];
                    if ((component_mask & ~declared_mask) != 0)
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                            index, instruction->token_offset, 0);
                }
            }
        }
        if (instruction->opcode == QXP_SHADER_OP_TEXKILL) {
            const QXP_SHADER_OPERAND *operand;

            operand = &instruction->destinations[0];
            if (operand->register_file == QXP_SHADER_REG_TEXTURE) {
                if (operand->register_number >= 8)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                        index, instruction->token_offset, 0);
                if (!(metadata->input_texture_mask &
                      (1UL << operand->register_number)) ||
                    (7 & ~metadata->input_texture_components[
                         operand->register_number]) != 0)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                        index, instruction->token_offset, 0);
            } else if (operand->register_file == QXP_SHADER_REG_TEMP) {
                if ((7 & ~temp_masks[operand->register_number]) != 0)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                        index, instruction->token_offset, 0);
            }
        }
        if (instruction->opcode != QXP_SHADER_OP_DCL &&
            instruction->opcode != QXP_SHADER_OP_DEF &&
            instruction->destination_count &&
            instruction->destinations[0].register_file ==
                QXP_SHADER_REG_TEMP) {
            DWORD number;
            DWORD component;

            number = instruction->destinations[0].register_number;
            if (instruction->opcode == QXP_SHADER_OP_TEX) {
                DWORD dependency;
                DWORD order;
                DWORD required_mask;
                DWORD sampler_number;

                sampler_number = instruction->sources[1].register_number;
                required_mask = metadata->sampler_types[sampler_number] ==
                    QXP_SHADER_SAMPLER_2D ? 3 : 7;
                if (instruction->control ==
                        QXP_SHADER_CONTROL_TEX_PROJECT ||
                    instruction->control == QXP_SHADER_CONTROL_TEX_BIAS)
                    required_mask |= 8;
                dependency = qxp_ps_register_dependency(
                    temp_dependencies,
                    instruction->sources[0].register_file,
                    instruction->sources[0].register_number,
                    required_mask);
                dependency = qxp_ps_max(
                    dependency, qxp_ps_register_dependency(
                        temp_dependencies, QXP_SHADER_REG_TEMP, number,
                        temp_masks[number]));
                order = instruction->sources[0].register_file ==
                            QXP_SHADER_REG_TEMP || temp_masks[number] ?
                            1 : 0;
                if (dependency)
                    order = qxp_ps_max(order, dependency + 1);
                if (order > 3) dependent_read_limit = TRUE;
                temp_masks[number] = 15;
                for (component = 0; component < 4; ++component)
                    temp_dependencies[number][component] = order;
            } else {
                DWORD dependencies[4];

                qxp_ps_zero(dependencies, (DWORD)sizeof(dependencies));
                for (component = 0; component < 4; ++component) {
                    if (instruction->destinations[0].write_mask &
                        (1UL << component))
                        dependencies[component] =
                            qxp_ps_result_dependency(
                                temp_dependencies, instruction,
                                component);
                }
                if (instruction->opcode == QXP_SHADER_OP_SINCOS) {
                    temp_masks[number] &= 8;
                    for (component = 0; component < 3; ++component)
                        temp_dependencies[number][component] = 0;
                }
                for (component = 0; component < 4; ++component) {
                    if (instruction->destinations[0].write_mask &
                        (1UL << component))
                        temp_dependencies[number][component] =
                            dependencies[component];
                }
                temp_masks[number] |=
                    instruction->destinations[0].write_mask;
            }
        }
    }
    if (output_mask != 1 && output_mask != 3 &&
        output_mask != 7 && output_mask != 15)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (required_temps != ir->required_ps_temp_count ||
        total_slots != ir->slot_count ||
        arithmetic_slots != ir->arithmetic_slot_count ||
        texture_slots != ir->texture_slot_count ||
        flow_slots != ir->flow_slot_count)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    no_texture_limit = arithmetic_slots > 64 || texture_slots > 32;
    expected_caps = dependent_read_limit ?
        QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT : 0;
    if (arbitrary_swizzle)
        expected_caps |= QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE;
    if (no_texture_limit)
        expected_caps |= QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT;
    if (ir->required_ps20_caps != expected_caps)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    required_slots = 0;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[index];
        if (no_texture_limit &&
            instruction->slot_class == QXP_SHADER_SLOT_TEXTURE) {
            if (instruction->opcode == QXP_SHADER_OP_TEXKILL) {
                required_slots += 2;
            } else if (instruction->control ==
                       QXP_SHADER_CONTROL_TEX_BIAS) {
                required_slots += 6;
            } else if (instruction->control ==
                       QXP_SHADER_CONTROL_TEX_PROJECT) {
                DWORD sampler_type;

                sampler_type = metadata->sampler_types[
                    instruction->sources[1].register_number];
                required_slots += sampler_type ==
                    QXP_SHADER_SAMPLER_CUBE ? 4 : 3;
            } else {
                DWORD sampler_type;

                sampler_type = metadata->sampler_types[
                    instruction->sources[1].register_number];
                required_slots += sampler_type ==
                    QXP_SHADER_SAMPLER_CUBE ? 4 : 1;
            }
        } else {
            required_slots += instruction->slot_count;
        }
    }
    if (required_slots != ir->required_ps_instruction_slots)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    return TRUE;
}

static BOOL qxp_ps_collect_metadata(
    const QXP_SHADER_IR *ir, QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD declaration_count;
    DWORD definition_count;
    DWORD source_index;
    BOOL executable_seen;
    BOOL ps11;

    if (!ir->valid || ir->token_count < 2 ||
        ir->token_count > QXP_SHADER_MAX_TOKENS ||
        ir->instruction_count > QXP_SHADER_MAX_INSTRUCTIONS ||
        ir->required_ps_temp_count > QXP_SHADER_MAX_PS_TEMPS)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (ir->stage != QXP_SHADER_STAGE_PIXEL ||
        !QXP_SHADER_VERSION_IS_PIXEL(ir->version_token))
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_PROFILE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    ps11 = ir->version_token == QXP_SHADER_VERSION_PS_1_1;
    if (!qxp_ps_validate_instruction_headers(ir, ps11, error))
        return FALSE;

    metadata->required_ps_temp_count = ir->required_ps_temp_count;
    metadata->required_ps20_caps = ir->required_ps20_caps;
    metadata->required_ps_instruction_slots =
        ir->required_ps_instruction_slots;
    declaration_count = 0;
    definition_count = 0;
    executable_seen = FALSE;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        const QXP_SHADER_OPERAND *destination;
        DWORD raw;
        DWORD number;
        DWORD sampler_type;
        DWORD component_mask;

        instruction = &ir->instructions[index];
        if (!instruction->token_count ||
            instruction->token_offset >= ir->token_count ||
            instruction->token_count >
                ir->token_count - instruction->token_offset)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                index, instruction->token_offset, 0);
        if (instruction->opcode == QXP_SHADER_OP_DCL) {
            if (executable_seen)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                    index, instruction->token_offset, 0);
            ++declaration_count;
            if (!qxp_ps_shape(instruction, 1, 0, 0) ||
                !instruction->declaration.present)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                    index, instruction->token_offset, 0);
            destination = &instruction->destinations[0];
            raw = instruction->declaration.raw_token;
            number = destination->register_number;
            if (destination->role != QXP_SHADER_OPERAND_DECLARATION ||
                destination->destination_shift != 0 ||
                destination->relative.present)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                    index, instruction->token_offset, 0);
            if (destination->register_file == QXP_SHADER_REG_SAMPLER) {
                if (number >= QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT ||
                    destination->write_mask != 15 ||
                    destination->destination_modifier !=
                        QXP_SHADER_DSTMOD_NONE ||
                    instruction->declaration.kind !=
                        QXP_SHADER_DECLARATION_SAMPLER ||
                    (raw & QXP_PS_DCL_PARAMETER_MARKER) == 0 ||
                    (raw & ~(QXP_PS_DCL_PARAMETER_MARKER |
                             QXP_PS_DCL_TEXTURETYPE_MASK)) != 0)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                        index, instruction->token_offset, 0);
                sampler_type = (raw & QXP_PS_DCL_TEXTURETYPE_MASK) >>
                               QXP_PS_DCL_TEXTURETYPE_SHIFT;
                if (sampler_type !=
                    instruction->declaration.sampler_type ||
                    (sampler_type != QXP_SHADER_SAMPLER_2D &&
                     sampler_type != QXP_SHADER_SAMPLER_CUBE &&
                     sampler_type != QXP_SHADER_SAMPLER_VOLUME) ||
                    (metadata->sampler_mask & (1UL << number)))
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                        index, instruction->token_offset, 0);
                metadata->sampler_mask |= 1UL << number;
                metadata->sampler_types[number] = (BYTE)sampler_type;
            } else if (destination->register_file == QXP_SHADER_REG_INPUT ||
                       destination->register_file ==
                           QXP_SHADER_REG_TEXTURE) {
                if (instruction->declaration.kind !=
                        QXP_SHADER_DECLARATION_INPUT ||
                    raw != QXP_PS_DCL_PARAMETER_MARKER ||
                    !destination->write_mask ||
                    destination->write_mask > 15 ||
                    (destination->destination_modifier &
                     ~(QXP_SHADER_DSTMOD_PARTIAL_PRECISION |
                       QXP_SHADER_DSTMOD_CENTROID)) != 0)
                    return qxp_ps_fail(
                        error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                        index, instruction->token_offset, 0);
                component_mask = destination->write_mask;
                if (destination->register_file ==
                    QXP_SHADER_REG_INPUT) {
                    if (number >= 2 ||
                        (metadata->input_color_mask & (1UL << number)))
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                            index, instruction->token_offset, 0);
                    metadata->input_color_mask |= 1UL << number;
                    metadata->input_color_components[number] =
                        (BYTE)component_mask;
                    /* ps_2_0 v# are Input Color Registers.  D3D9 applies
                     * centroid interpolation to them automatically, even
                     * when the optional DCL centroid bit is absent. */
                    metadata->input_centroid_mask |= 1UL << number;
                } else {
                    if (number >= 8 ||
                        (metadata->input_texture_mask &
                         (1UL << number)))
                        return qxp_ps_fail(
                            error,
                            QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                            index, instruction->token_offset, 0);
                    metadata->input_texture_mask |= 1UL << number;
                    metadata->input_texture_components[number] =
                        (BYTE)component_mask;
                    if (destination->destination_modifier &
                        QXP_SHADER_DSTMOD_CENTROID)
                        metadata->input_centroid_mask |=
                            1UL << (number + 2);
                }
            } else {
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION,
                    index, instruction->token_offset, 0);
            }
        } else if (instruction->opcode == QXP_SHADER_OP_DEF) {
            if (executable_seen)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                    index, instruction->token_offset, 0);
            ++definition_count;
            if (!qxp_ps_shape(instruction, 1, 0, 4))
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                    index, instruction->token_offset, 0);
            destination = &instruction->destinations[0];
            number = destination->register_number;
            if (destination->role != QXP_SHADER_OPERAND_DEFINITION ||
                destination->register_file != QXP_SHADER_REG_CONST ||
                number >= QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT ||
                destination->write_mask != 15 ||
                destination->destination_modifier !=
                    QXP_SHADER_DSTMOD_NONE ||
                destination->destination_shift != 0 ||
                destination->relative.present)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER,
                    index, instruction->token_offset, 0);
            metadata->definition_mask |= 1UL << number;
            metadata->definition_values[number][0] =
                instruction->immediate[0];
            metadata->definition_values[number][1] =
                instruction->immediate[1];
            metadata->definition_values[number][2] =
                instruction->immediate[2];
            metadata->definition_values[number][3] =
                instruction->immediate[3];
        } else {
            executable_seen = TRUE;
            if (instruction->declaration.present)
                return qxp_ps_fail(
                    error, QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION,
                    index, instruction->token_offset, 0);
            if (instruction->destination_count) {
                destination = &instruction->destinations[0];
                if (destination->register_file ==
                        QXP_SHADER_REG_COLOROUT &&
                    destination->register_number < 4)
                    metadata->color_output_mask |=
                        1UL << destination->register_number;
                else if (destination->register_file ==
                             QXP_SHADER_REG_DEPTHOUT &&
                         destination->register_number == 0)
                    metadata->writes_depth = TRUE;
                if (ps11 &&
                    destination->register_file ==
                        QXP_SHADER_REG_TEXTURE &&
                    destination->register_number < 8) {
                    /* ps_1_1 declares nothing: a written t# needs its
                     * mutable local, and `tex tN` implicitly consumes
                     * 2D sampler N and the stage-N coordinate. */
                    metadata->input_texture_mask |=
                        1UL << destination->register_number;
                    metadata->input_texture_components[
                        destination->register_number] = 15;
                    if (instruction->opcode == QXP_SHADER_OP_TEX ||
                        instruction->opcode ==
                            QXP_SHADER_OP_TEXREG2AR ||
                        instruction->opcode ==
                            QXP_SHADER_OP_TEXREG2GB) {
                        /* texreg2* samples 2D stage N implicitly, exactly
                         * like `tex tN`. */
                        metadata->sampler_mask |=
                            1UL << destination->register_number;
                        metadata->sampler_types[
                            destination->register_number] =
                            (BYTE)QXP_SHADER_SAMPLER_2D;
                    }
                }
            }
            if (instruction->opcode == QXP_SHADER_OP_TEXKILL)
                metadata->uses_discard = TRUE;
        }
        for (source_index = 0;
             source_index < instruction->source_count; ++source_index) {
            const QXP_SHADER_OPERAND *source;
            DWORD required;

            source = &instruction->sources[source_index];
            if (source->register_file == QXP_SHADER_REG_CONST) {
                required = (DWORD)source->register_number + 1;
                if (required > metadata->required_constant_count)
                    metadata->required_constant_count = required;
            }
            if (ps11 &&
                source->register_file == QXP_SHADER_REG_TEXTURE &&
                source->register_number < 8) {
                /* A t# read before any `tex` sees the interpolated
                 * stage coordinate, so every consumed stage is an
                 * input, whole coordinate included. */
                metadata->input_texture_mask |=
                    1UL << source->register_number;
                metadata->input_texture_components[
                    source->register_number] = 15;
            } else if (ps11 &&
                       source->register_file == QXP_SHADER_REG_INPUT &&
                       source->register_number < 2) {
                metadata->input_color_mask |=
                    1UL << source->register_number;
                metadata->input_color_components[
                    source->register_number] = 15;
                /* ps_1_1 v# are the same Input Color Registers D3D9
                 * centroid-interpolates automatically for ps_2_0. */
                metadata->input_centroid_mask |=
                    1UL << source->register_number;
            }
        }
    }
    if (declaration_count != ir->declaration_count ||
        definition_count != ir->definition_count)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (!qxp_ps_validate_ir_summary(ir, metadata, error))
        return FALSE;
    if (ps11) {
        /* ps_1_1 has no oC# registers: r0 is the pixel colour and is
         * always emitted, so keep its local declared even in a
         * hand-built IR that never touches a temp. */
        metadata->color_output_mask = 1;
        if (!metadata->required_ps_temp_count)
            metadata->required_ps_temp_count = 1;
    }
    return TRUE;
}


static void qxp_ps_emit_interface(
    QXP_PS_GLSL_EMIT *emit,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    DWORD index;

    qxp_ps_append(&emit->output, "#version 120\n");
    if (metadata->color_output_mask != 1)
        qxp_ps_append(&emit->output,
                      "#extension GL_ARB_draw_buffers : require\n");
    qxp_ps_append(&emit->output,
        "uniform vec4 qxpPsConstants[32];\n");
    for (index = 0; index < 2; ++index) {
        if (!(metadata->input_color_mask & (1UL << index))) continue;
        qxp_ps_append(&emit->output, "centroid varying vec4 qxpPsInV");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, ";\n");
    }
    for (index = 0; index < 8; ++index) {
        if (!(metadata->input_texture_mask & (1UL << index))) continue;
        if (metadata->input_centroid_mask & (1UL << (index + 2)))
            qxp_ps_append(&emit->output, "centroid ");
        qxp_ps_append(&emit->output, "varying vec4 qxpPsInT");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, ";\n");
    }
    for (index = 0;
         index < QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT; ++index) {
        if (!(metadata->sampler_mask & (1UL << index))) continue;
        if (metadata->sampler_types[index] == QXP_SHADER_SAMPLER_2D)
            qxp_ps_append(&emit->output, "uniform sampler2D qxpPsS");
        else if (metadata->sampler_types[index] ==
                 QXP_SHADER_SAMPLER_CUBE)
            qxp_ps_append(&emit->output, "uniform samplerCube qxpPsS");
        else
            qxp_ps_append(&emit->output, "uniform sampler3D qxpPsS");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, ";\n");
    }
    /* D3D applies pixel fog after the shader.  A bound GLSL program disables
     * the fixed-function fog stage, so the blend has to happen here; the
     * vertex stage already publishes the fog distance in gl_FogFragCoord and
     * the miniport programs gl_Fog's colour and range.  x carries
     * D3DRS_FOGTABLEMODE, or zero when fog is off. */
    qxp_ps_append(&emit->output,
        "uniform vec4 qxpPsFog;\n");
    qxp_ps_append(&emit->output,
        "float qxpPsSat1(float a) { if (!(a > 0.0)) return 0.0; if (a > 1.0) return 1.0; return a; }\n"
        "vec4 qxpPsSaturate(vec4 a) { return vec4(qxpPsSat1(a.x), qxpPsSat1(a.y), qxpPsSat1(a.z), qxpPsSat1(a.w)); }\n");
    qxp_ps_append(&emit->output,
        "vec4 qxpPsMin(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n");
    qxp_ps_append(&emit->output,
        "vec4 qxpPsMax(vec4 a, vec4 b) { return vec4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n");
    qxp_ps_append(&emit->output,
        "vec4 qxpPsCmp(vec4 a, vec4 b, vec4 c) { return vec4(a.x >= 0.0 ? b.x : c.x, a.y >= 0.0 ? b.y : c.y, a.z >= 0.0 ? b.z : c.z, a.w >= 0.0 ? b.w : c.w); }\n");
    qxp_ps_append(&emit->output,
        "void main(void) {\n"
        "  vec4 qxpValue = vec4(0.0);\n"
        "  vec4 qxpArg0 = vec4(0.0);\n"
        "  vec4 qxpArg1 = vec4(0.0);\n"
        "  vec4 qxpArg2 = vec4(0.0);\n"
        "  float qxpScalar = 0.0;\n");
    for (index = 0; index < metadata->required_ps_temp_count; ++index) {
        qxp_ps_append(&emit->output, "  vec4 qxpR");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, " = vec4(0.0);\n");
    }
    for (index = 0; index < 2; ++index) {
        DWORD mask;

        if (!(metadata->input_color_mask & (1UL << index))) continue;
        mask = metadata->input_color_components[index];
        qxp_ps_append(&emit->output, "  vec4 qxpV");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, " = vec4(0.0, 0.0, 0.0, 1.0);\n");
        qxp_ps_append(&emit->output, "  qxpV");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append_component_mask(&emit->output, mask);
        qxp_ps_append(&emit->output, " = clamp(qxpPsInV");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append_component_mask(&emit->output, mask);
        qxp_ps_append(&emit->output, ", 0.0, 1.0);\n");
    }
    for (index = 0; index < 8; ++index) {
        DWORD mask;

        if (!(metadata->input_texture_mask & (1UL << index))) continue;
        mask = metadata->input_texture_components[index];
        qxp_ps_append(&emit->output, "  vec4 qxpT");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append(&emit->output, " = vec4(0.0);\n  qxpT");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append_component_mask(&emit->output, mask);
        qxp_ps_append(&emit->output, " = qxpPsInT");
        qxp_ps_append_uint(&emit->output, index);
        qxp_ps_append_component_mask(&emit->output, mask);
        qxp_ps_append(&emit->output, ";\n");
    }
}

static BOOL qxp_ps_emit_pass(
    const QXP_SHADER_IR *ir,
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    char *source, DWORD source_capacity, DWORD *source_length,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    QXP_PS_GLSL_EMIT emit;
    DWORD index;

    qxp_ps_zero(&emit, (DWORD)sizeof(emit));
    emit.output.text = source;
    emit.output.capacity = source_capacity;
    emit.output.valid = TRUE;
    emit.error = error;
    emit.ps11 = ir->version_token == QXP_SHADER_VERSION_PS_1_1;
    if (source) source[0] = 0;
    qxp_ps_emit_interface(&emit, metadata);
    for (index = 0; index < ir->instruction_count; ++index) {
        emit.instruction_index = index;
        emit.token_offset = ir->instructions[index].token_offset;
        if (!qxp_ps_emit_instruction(
                &emit, &ir->instructions[index], metadata))
            return FALSE;
        if (!emit.output.valid)
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
                index, emit.token_offset, 0);
    }
    /* ps_1_1 writes no oC#: whatever r0 holds after the last
     * instruction is the pixel colour. */
    if (emit.ps11)
        qxp_ps_append(&emit.output, "  gl_FragColor = qxpR0;\n");
    /* Fog epilogue.  qxpPsFog.x is D3DRS_FOGTABLEMODE (0 off, 1 EXP,
     * 2 EXP2, 3 LINEAR); gl_Fog and gl_FogFragCoord carry the range and the
     * distance the vertex stage already published. */
    qxp_ps_append(&emit.output,
        "  if (qxpPsFog.x > 0.5) {\n"
        "    float qxpFogF;\n"
        "    if (qxpPsFog.x > 2.5) qxpFogF = (gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale;\n"
        "    else if (qxpPsFog.x > 1.5) qxpFogF = exp(-(gl_Fog.density * gl_FogFragCoord) * (gl_Fog.density * gl_FogFragCoord));\n"
        "    else qxpFogF = exp(-gl_Fog.density * gl_FogFragCoord);\n");
    /* A shader that publishes more than one render target writes gl_FragData
     * and must not also touch gl_FragColor: binding both to COL0 fails to
     * link.  D3D fogs only the first target, so blend exactly that one. */
    if (metadata->color_output_mask != 1)
        qxp_ps_append(&emit.output,
            "    gl_FragData[0].xyz = mix(gl_Fog.color.xyz, gl_FragData[0].xyz, qxpPsSat1(qxpFogF));\n");
    else
        qxp_ps_append(&emit.output,
            "    gl_FragColor.xyz = mix(gl_Fog.color.xyz, gl_FragColor.xyz, qxpPsSat1(qxpFogF));\n");
    qxp_ps_append(&emit.output, "  }\n");
    qxp_ps_append(&emit.output, "}\n");
    if (!emit.output.valid)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    *source_length = emit.output.length;
    return TRUE;
}

BOOL QxpEmitPixelShader20Glsl(
    const QXP_SHADER_IR *ir,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_PIXEL_SHADER_GLSL_ERROR *error)
{
    QXP_PIXEL_SHADER_GLSL_METADATA prepared_metadata;
    DWORD required_bytes;
    DWORD emitted_bytes;

    if (source_bytes) *source_bytes = 0;
    if (metadata)
        qxp_ps_zero(metadata, (DWORD)sizeof(*metadata));
    if (error)
        qxp_ps_zero(error, (DWORD)sizeof(*error));
    if (source && source_capacity) source[0] = 0;
    if (!ir || !source_bytes || !metadata || !error ||
        (!source && source_capacity) || (source && !source_capacity))
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_NULL_ARGUMENT,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);

    qxp_ps_zero(&prepared_metadata, (DWORD)sizeof(prepared_metadata));
    if (!qxp_ps_collect_metadata(ir, &prepared_metadata, error))
        return FALSE;
    required_bytes = 0;
    if (!qxp_ps_emit_pass(
            ir, &prepared_metadata, NULL,
            QXP_PIXEL_SHADER_GLSL_MAX_SOURCE + 1UL,
            &required_bytes, error))
        return FALSE;
    if (required_bytes > QXP_PIXEL_SHADER_GLSL_MAX_SOURCE)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE,
            required_bytes + 1UL);
    if (source && source_capacity <= required_bytes)
        return qxp_ps_fail(
            error, QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE,
            required_bytes + 1UL);
    if (source) {
        emitted_bytes = 0;
        if (!qxp_ps_emit_pass(
                ir, &prepared_metadata, source, source_capacity,
                &emitted_bytes, error)) {
            source[0] = 0;
            return FALSE;
        }
        if (emitted_bytes != required_bytes) {
            source[0] = 0;
            return qxp_ps_fail(
                error, QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE,
                QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
        }
    }
    *source_bytes = required_bytes;
    *metadata = prepared_metadata;
    qxp_ps_zero(error, (DWORD)sizeof(*error));
    return TRUE;
}
