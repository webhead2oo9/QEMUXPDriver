#include <windef.h>

#include "qxp_vshader20_glsl.h"

/*
 * Direct exact vs_2_0/vs_1_1 typed-IR to compatibility GLSL 1.20 emitter.
 * This file mirrors the architecture of qxp_pshader_glsl.c but is
 * vertex-specific.  It
 * consumes only a QXP_SHADER_IR already produced by QxpDecodeShader20 and has
 * no production caller.
 *
 * The emitter is caller-owned, allocation-free, deterministic and fail-closed.
 * Before any count-driven traversal it independently reconstructs and
 * validates every exact vertex form the decoder admits: instruction header and
 * token-span schema (including relative-operand token accounting), operand
 * roles/ranges/modifiers/swizzles/relative operands, control pairing and
 * label targets, declaration/definition ordering and counts, exact output
 * requirements, per-instruction read ports, and a sound interprocedural
 * definite-assignment summary.  Corrupt typed IR is rejected rather than
 * trusted.
 */

/* Rasterizer output register indices (matches D3DSRO_*). */
#define QXP_VS_RASTOUT_POSITION    0UL
#define QXP_VS_RASTOUT_FOG         1UL
#define QXP_VS_RASTOUT_POINT_SIZE  2UL

#define QXP_VS_DCL_PARAMETER_MARKER  0x80000000UL
#define QXP_VS_DCL_USAGE_MASK        0x0000000fUL
#define QXP_VS_DCL_USAGE_INDEX_MASK  0x000f0000UL
#define QXP_VS_DCL_USAGE_INDEX_SHIFT 16UL

/* Bounds for the independent definite-assignment forward analysis. */
#define QXP_VS_DATAFLOW_COMPONENTS (QXP_VERTEX_SHADER_GLSL_TEMP_COUNT * 4UL + 4UL)
#define QXP_VS_DATAFLOW_MAX_PASSES (QXP_VS_DATAFLOW_COMPONENTS + 1UL)
#define QXP_VS_DATAFLOW_MAX_DEPTH  16UL

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static void qxp_vs_zero(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD index;

    /* Volatile prevents the WDK optimizer from introducing a CRT memset. */
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index)
        bytes[index] = 0;
}

static BOOL qxp_vs_memory_is_zero(const void *memory, DWORD byte_count)
{
    const BYTE *bytes;
    DWORD index;

    bytes = (const BYTE *)memory;
    for (index = 0; index < byte_count; ++index) {
        if (bytes[index] != 0) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_fail(QXP_VERTEX_SHADER_GLSL_ERROR *error, DWORD code,
                        DWORD instruction_index, DWORD token_offset,
                        DWORD required_capacity)
{
    if (error) {
        qxp_vs_zero(error, (DWORD)sizeof(*error));
        error->code = code;
        error->instruction_index = instruction_index;
        error->token_offset = token_offset;
        error->required_capacity = required_capacity;
    }
    return FALSE;
}

static BOOL qxp_vs_is_matrix_opcode(DWORD opcode)
{
    return opcode == QXP_SHADER_OP_M4X4 || opcode == QXP_SHADER_OP_M4X3 ||
           opcode == QXP_SHADER_OP_M3X4 || opcode == QXP_SHADER_OP_M3X3 ||
           opcode == QXP_SHADER_OP_M3X2;
}

static DWORD qxp_vs_matrix_rows(DWORD opcode)
{
    if (opcode == QXP_SHADER_OP_M4X4 || opcode == QXP_SHADER_OP_M3X4)
        return 4;
    if (opcode == QXP_SHADER_OP_M4X3 || opcode == QXP_SHADER_OP_M3X3)
        return 3;
    return 2;
}

static DWORD qxp_vs_matrix_columns(DWORD opcode)
{
    if (opcode == QXP_SHADER_OP_M4X4 || opcode == QXP_SHADER_OP_M4X3)
        return 4;
    return 3;
}

static BOOL qxp_vs_is_scalar_opcode(DWORD opcode)
{
    return opcode == QXP_SHADER_OP_RCP || opcode == QXP_SHADER_OP_RSQ ||
           opcode == QXP_SHADER_OP_EXP || opcode == QXP_SHADER_OP_LOG ||
           opcode == QXP_SHADER_OP_EXPP || opcode == QXP_SHADER_OP_LOGP;
}

/* vs_1_1 spells the address-register write as MOV where vs_2_0 uses MOVA;
 * the decoder preserves the source opcode, so both forms denote the same
 * operation and must validate and lower identically. */
static BOOL qxp_vs_is_address_write(const QXP_SHADER_INSTRUCTION *instruction)
{
    return instruction->opcode == QXP_SHADER_OP_MOVA ||
           (instruction->opcode == QXP_SHADER_OP_MOV &&
            instruction->destination_count &&
            instruction->destinations[0].register_file ==
                QXP_SHADER_REG_ADDRESS);
}

static BOOL qxp_vs_swizzle_is_identity(const QXP_SHADER_OPERAND *operand)
{
    return operand->swizzle[0] == 0 && operand->swizzle[1] == 1 &&
           operand->swizzle[2] == 2 && operand->swizzle[3] == 3;
}

static BOOL qxp_vs_swizzle_is_replicate(const QXP_SHADER_OPERAND *operand)
{
    return operand->swizzle[0] == operand->swizzle[1] &&
           operand->swizzle[0] == operand->swizzle[2] &&
           operand->swizzle[0] == operand->swizzle[3];
}

static BOOL qxp_vs_operands_alias(const QXP_SHADER_OPERAND *left,
                                  const QXP_SHADER_OPERAND *right)
{
    return left->register_file == right->register_file &&
           left->register_number == right->register_number;
}

static DWORD qxp_vs_swizzled_component_mask(
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

/* ------------------------------------------------------------------ */
/* Opcode schema (independent reconstruction)                          */
/* ------------------------------------------------------------------ */

typedef struct qxp_vs_opcode_schema {
    BYTE destinations;
    BYTE sources;
    BYTE immediates;
    BYTE extra_tokens;   /* DCL's declaration token */
    BYTE slot_class;
    BYTE slot_count;
    BYTE is_declaration;
    BYTE is_definition;
    BYTE is_flow;
} QXP_VS_OPCODE_SCHEMA;

static BOOL qxp_vs_get_opcode_schema(DWORD opcode,
                                     QXP_VS_OPCODE_SCHEMA *schema)
{
    qxp_vs_zero(schema, (DWORD)sizeof(*schema));
    schema->slot_class = (BYTE)QXP_SHADER_SLOT_ARITHMETIC;
    schema->slot_count = 1;
    if (opcode == QXP_SHADER_OP_NOP)
        return TRUE;
    if (opcode == QXP_SHADER_OP_MOV || opcode == QXP_SHADER_OP_RCP ||
        opcode == QXP_SHADER_OP_RSQ || opcode == QXP_SHADER_OP_EXP ||
        opcode == QXP_SHADER_OP_LOG || opcode == QXP_SHADER_OP_FRC ||
        opcode == QXP_SHADER_OP_ABS || opcode == QXP_SHADER_OP_MOVA ||
        opcode == QXP_SHADER_OP_EXPP || opcode == QXP_SHADER_OP_LOGP) {
        schema->destinations = 1;
        schema->sources = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LIT || opcode == QXP_SHADER_OP_NRM) {
        schema->destinations = 1;
        schema->sources = 1;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_ADD || opcode == QXP_SHADER_OP_SUB ||
        opcode == QXP_SHADER_OP_MUL || opcode == QXP_SHADER_OP_DP3 ||
        opcode == QXP_SHADER_OP_DP4 || opcode == QXP_SHADER_OP_MIN ||
        opcode == QXP_SHADER_OP_MAX || opcode == QXP_SHADER_OP_SLT ||
        opcode == QXP_SHADER_OP_SGE || opcode == QXP_SHADER_OP_DST) {
        schema->destinations = 1;
        schema->sources = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_MAD) {
        schema->destinations = 1;
        schema->sources = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LRP) {
        schema->destinations = 1;
        schema->sources = 3;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M4X4 || opcode == QXP_SHADER_OP_M3X4) {
        schema->destinations = 1;
        schema->sources = 2;
        schema->slot_count = 4;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M4X3 || opcode == QXP_SHADER_OP_M3X3) {
        schema->destinations = 1;
        schema->sources = 2;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_M3X2) {
        schema->destinations = 1;
        schema->sources = 2;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_POW) {
        schema->destinations = 1;
        schema->sources = 2;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_CRS) {
        schema->destinations = 1;
        schema->sources = 2;
        schema->slot_count = 2;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_SGN) {
        schema->destinations = 1;
        schema->sources = 3;
        schema->slot_count = 3;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_SINCOS) {
        schema->destinations = 1;
        schema->sources = 3;
        schema->slot_count = 8;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DCL) {
        schema->destinations = 1;
        schema->extra_tokens = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        schema->is_declaration = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DEF || opcode == QXP_SHADER_OP_DEFI) {
        schema->destinations = 1;
        schema->immediates = 4;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        schema->is_definition = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_DEFB) {
        schema->destinations = 1;
        schema->immediates = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        schema->is_definition = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_CALL) {
        schema->sources = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 2;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_CALLNZ) {
        schema->sources = 2;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 3;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LOOP) {
        schema->sources = 2;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 3;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_RET) {
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 1;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_ENDLOOP || opcode == QXP_SHADER_OP_ENDREP) {
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 2;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LABEL) {
        schema->sources = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_SETUP;
        schema->slot_count = 0;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_REP) {
        schema->sources = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 3;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_IF) {
        schema->sources = 1;
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 3;
        schema->is_flow = 1;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_ELSE || opcode == QXP_SHADER_OP_ENDIF) {
        schema->slot_class = (BYTE)QXP_SHADER_SLOT_FLOW;
        schema->slot_count = 1;
        schema->is_flow = 1;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Instruction header validation                                       */
/* ------------------------------------------------------------------ */

static BOOL qxp_vs_link_fields_valid(
    const QXP_SHADER_INSTRUCTION *instruction, DWORD index)
{
    DWORD opcode;

    opcode = instruction->opcode;
    if (opcode == QXP_SHADER_OP_IF)
        return instruction->control_match != QXP_SHADER_INDEX_NONE &&
               instruction->label_target == QXP_SHADER_INDEX_NONE;
    if (opcode == QXP_SHADER_OP_ELSE)
        return instruction->control_match != QXP_SHADER_INDEX_NONE &&
               instruction->control_else != QXP_SHADER_INDEX_NONE &&
               instruction->label_target == QXP_SHADER_INDEX_NONE;
    if (opcode == QXP_SHADER_OP_ENDIF)
        return instruction->control_match != QXP_SHADER_INDEX_NONE &&
               instruction->label_target == QXP_SHADER_INDEX_NONE;
    if (opcode == QXP_SHADER_OP_LOOP || opcode == QXP_SHADER_OP_REP ||
        opcode == QXP_SHADER_OP_ENDLOOP ||
        opcode == QXP_SHADER_OP_ENDREP)
        return instruction->control_match != QXP_SHADER_INDEX_NONE &&
               instruction->control_else == QXP_SHADER_INDEX_NONE &&
               instruction->label_target == QXP_SHADER_INDEX_NONE;
    if (opcode == QXP_SHADER_OP_CALL || opcode == QXP_SHADER_OP_CALLNZ)
        return instruction->control_match == QXP_SHADER_INDEX_NONE &&
               instruction->control_else == QXP_SHADER_INDEX_NONE &&
               instruction->label_target != QXP_SHADER_INDEX_NONE;
    if (opcode == QXP_SHADER_OP_LABEL)
        return instruction->control_match == QXP_SHADER_INDEX_NONE &&
               instruction->control_else == QXP_SHADER_INDEX_NONE &&
               instruction->label_target == index;
    return instruction->control_match == QXP_SHADER_INDEX_NONE &&
           instruction->control_else == QXP_SHADER_INDEX_NONE &&
           instruction->label_target == QXP_SHADER_INDEX_NONE;
}

static BOOL qxp_vs_validate_instruction_headers(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD previous_end;

    previous_end = 1;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        QXP_VS_OPCODE_SCHEMA schema;
        DWORD expected_tokens;
        DWORD source_index;

        instruction = &ir->instructions[index];
        if (instruction->destination_count > QXP_SHADER_MAX_DESTINATIONS ||
            instruction->source_count > QXP_SHADER_MAX_SOURCES ||
            instruction->immediate_count > 4)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);
        if (!qxp_vs_get_opcode_schema(instruction->opcode, &schema))
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_UNSUPPORTED_FORM,
                               index, instruction->token_offset, 0);
        expected_tokens = 1UL + schema.destinations + schema.immediates +
                          schema.extra_tokens;
        for (source_index = 0; source_index < schema.sources;
             ++source_index) {
            expected_tokens += 1UL;
            /* Only the vs_2_0 encoding spends a token on the relative
             * address register; the SM1 stream sets the relative bit alone
             * and the decoder synthesizes the a0.x operand, so a vs_1_1
             * relative source occupies a single token. */
            if (source_index < instruction->source_count &&
                instruction->sources[source_index].relative.present &&
                ir->version_token == QXP_SHADER_VERSION_VS_2_0)
                expected_tokens += 1UL;
        }
        if (instruction->destination_count != schema.destinations ||
            instruction->source_count != schema.sources ||
            instruction->immediate_count != schema.immediates ||
            instruction->slot_class != schema.slot_class ||
            instruction->slot_count != schema.slot_count ||
            instruction->token_count != expected_tokens ||
            instruction->control != QXP_SHADER_CONTROL_NONE ||
            !qxp_vs_link_fields_valid(instruction, index) ||
            instruction->token_offset < previous_end ||
            instruction->token_offset >= ir->token_count ||
            instruction->token_count >
                ir->token_count - instruction->token_offset ||
            instruction->token_offset + instruction->token_count >=
                ir->token_count)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);
        if ((instruction->opcode == QXP_SHADER_OP_DCL) !=
            (instruction->declaration.present != FALSE))
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
        if (instruction->opcode != QXP_SHADER_OP_DCL &&
            !qxp_vs_memory_is_zero(
                &instruction->declaration,
                (DWORD)sizeof(instruction->declaration)))
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);
        for (source_index = instruction->destination_count;
             source_index < QXP_SHADER_MAX_DESTINATIONS;
             ++source_index) {
            if (!qxp_vs_memory_is_zero(
                    &instruction->destinations[source_index],
                    (DWORD)sizeof(instruction->destinations[source_index])))
                return qxp_vs_fail(
                    error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                    index, instruction->token_offset, 0);
        }
        for (source_index = instruction->source_count;
             source_index < QXP_SHADER_MAX_SOURCES; ++source_index) {
            if (!qxp_vs_memory_is_zero(
                    &instruction->sources[source_index],
                    (DWORD)sizeof(instruction->sources[source_index])))
                return qxp_vs_fail(
                    error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                    index, instruction->token_offset, 0);
        }
        for (source_index = instruction->immediate_count;
             source_index < 4; ++source_index) {
            if (instruction->immediate[source_index] != 0)
                return qxp_vs_fail(
                    error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                    index, instruction->token_offset, 0);
        }
        previous_end = instruction->token_offset + instruction->token_count;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Control-structure reconstruction                                    */
/* ------------------------------------------------------------------ */

static DWORD qxp_vs_control_kind(DWORD opcode)
{
    if (opcode == QXP_SHADER_OP_IF) return 1;
    if (opcode == QXP_SHADER_OP_LOOP) return 2;
    if (opcode == QXP_SHADER_OP_REP) return 3;
    return 0;
}

static BOOL qxp_vs_validate_control_structure(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD control_stack[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD labels[QXP_SHADER_MAX_LABELS];
    BYTE label_referenced[QXP_SHADER_MAX_LABELS];
    DWORD control_depth;
    DWORD loop_depth;
    DWORD static_flow;
    DWORD index;
    BOOL in_subroutine;
    BOOL after_return;
    BOOL saw_label;

    control_depth = 0;
    loop_depth = 0;
    static_flow = 0;
    for (index = 0; index < QXP_SHADER_MAX_LABELS; ++index) {
        labels[index] = QXP_SHADER_INDEX_NONE;
        label_referenced[index] = 0;
    }

    /* First pass: record label definitions and verify uniqueness. */
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD label;

        instruction = &ir->instructions[index];
        if (instruction->opcode != QXP_SHADER_OP_LABEL) continue;
        if (instruction->sources[0].register_file != QXP_SHADER_REG_LABEL ||
            (DWORD)instruction->sources[0].register_number >=
                QXP_SHADER_MAX_LABELS)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        label = instruction->sources[0].register_number;
        if (labels[label] != QXP_SHADER_INDEX_NONE)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        labels[label] = index;
        if (instruction->label_target != index)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
    }

    /* Second pass: reconstruct the opener/closer stack and validate the
     * decoder-recorded control_match / control_else associations. */
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &ir->instructions[index];
        opcode = instruction->opcode;
        if (opcode == QXP_SHADER_OP_IF) {
            if (++static_flow > 16 ||
                control_depth >= QXP_SHADER_MAX_INSTRUCTIONS)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            control_stack[control_depth++] = index;
        } else if (opcode == QXP_SHADER_OP_LOOP ||
                   opcode == QXP_SHADER_OP_REP) {
            if (loop_depth != 0 || ++static_flow > 16 ||
                control_depth >= QXP_SHADER_MAX_INSTRUCTIONS)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            ++loop_depth;
            control_stack[control_depth++] = index;
        } else if (opcode == QXP_SHADER_OP_ELSE) {
            const QXP_SHADER_INSTRUCTION *opener;
            DWORD opener_index;

            if (++static_flow > 16 || !control_depth)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            opener_index = control_stack[control_depth - 1];
            opener = &ir->instructions[opener_index];
            if (opener->opcode != QXP_SHADER_OP_IF ||
                opener->control_else != index ||
                instruction->control_else != opener_index)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
        } else if (opcode == QXP_SHADER_OP_ENDIF ||
                   opcode == QXP_SHADER_OP_ENDLOOP ||
                   opcode == QXP_SHADER_OP_ENDREP) {
            const QXP_SHADER_INSTRUCTION *opener;
            DWORD opener_index;
            DWORD expected_kind;

            if (!control_depth)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            opener_index = control_stack[--control_depth];
            opener = &ir->instructions[opener_index];
            expected_kind = opcode == QXP_SHADER_OP_ENDIF ? 1 :
                            (opcode == QXP_SHADER_OP_ENDLOOP ? 2 : 3);
            if (qxp_vs_control_kind(opener->opcode) != expected_kind ||
                opener->control_match != index ||
                instruction->control_match != opener_index)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            if (opcode != QXP_SHADER_OP_ENDIF)
                --loop_depth;
            if (opcode == QXP_SHADER_OP_ENDIF &&
                opener->control_else != QXP_SHADER_INDEX_NONE) {
                const QXP_SHADER_INSTRUCTION *else_instruction;

                if (opener->control_else >= ir->instruction_count ||
                    instruction->control_else != opener->control_else)
                    return qxp_vs_fail(
                        error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                        index, instruction->token_offset, 0);
                else_instruction =
                    &ir->instructions[opener->control_else];
                if (else_instruction->opcode != QXP_SHADER_OP_ELSE ||
                    else_instruction->control_match != index)
                    return qxp_vs_fail(
                        error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                        index, instruction->token_offset, 0);
            }
        } else if (opcode == QXP_SHADER_OP_CALL ||
                   opcode == QXP_SHADER_OP_CALLNZ) {
            if (++static_flow > 16)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
        } else if ((opcode == QXP_SHADER_OP_RET ||
                    opcode == QXP_SHADER_OP_LABEL) && control_depth) {
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        }
    }
    if (control_depth)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);

    /* Third pass: subroutine layout and forward call-target resolution. */
    in_subroutine = FALSE;
    after_return = FALSE;
    saw_label = FALSE;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &ir->instructions[index];
        opcode = instruction->opcode;
        if (opcode == QXP_SHADER_OP_LABEL) {
            if (index == 0 ||
                ir->instructions[index - 1].opcode != QXP_SHADER_OP_RET)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            in_subroutine = TRUE;
            after_return = FALSE;
            saw_label = TRUE;
        } else if (after_return) {
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        }
        if (opcode == QXP_SHADER_OP_CALL ||
            opcode == QXP_SHADER_OP_CALLNZ) {
            DWORD label;
            DWORD target;

            if (instruction->sources[0].register_file !=
                    QXP_SHADER_REG_LABEL ||
                (DWORD)instruction->sources[0].register_number >=
                    QXP_SHADER_MAX_LABELS)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            label = instruction->sources[0].register_number;
            target = labels[label];
            if (target == QXP_SHADER_INDEX_NONE || target <= index ||
                in_subroutine || instruction->label_target != target)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            label_referenced[label] = 1;
        }
        if (opcode == QXP_SHADER_OP_RET) after_return = TRUE;
    }
    if (saw_label && ir->instruction_count &&
        ir->instructions[ir->instruction_count - 1].opcode !=
            QXP_SHADER_OP_RET)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                           ir->instruction_count - 1,
                           ir->instructions[ir->instruction_count - 1].
                               token_offset, 0);
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD label;

        instruction = &ir->instructions[index];
        if (instruction->opcode != QXP_SHADER_OP_LABEL) continue;
        label = instruction->sources[0].register_number;
        if (!label_referenced[label])
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
    }
    if (static_flow) metadata->uses_static_flow = TRUE;
    if (saw_label) metadata->uses_subroutines = TRUE;
    return TRUE;
}

static BOOL qxp_vs_instruction_is_in_iteration(
    const QXP_SHADER_IR *ir, DWORD instruction_index, BOOL loop_only)
{
    DWORD index;

    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[index];
        if ((instruction->opcode == QXP_SHADER_OP_LOOP ||
             (!loop_only && instruction->opcode == QXP_SHADER_OP_REP)) &&
            instruction->control_match != QXP_SHADER_INDEX_NONE &&
            index < instruction_index &&
            instruction_index < instruction->control_match)
            return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_subroutine_has_iteration(
    const QXP_SHADER_IR *ir, DWORD label_index)
{
    DWORD index;

    for (index = label_index + 1; index < ir->instruction_count; ++index) {
        DWORD opcode;

        opcode = ir->instructions[index].opcode;
        if (opcode == QXP_SHADER_OP_RET || opcode == QXP_SHADER_OP_LABEL)
            break;
        if (opcode == QXP_SHADER_OP_LOOP || opcode == QXP_SHADER_OP_REP)
            return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_validate_call_iteration_nesting(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;

    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[index];
        if (instruction->opcode != QXP_SHADER_OP_CALL &&
            instruction->opcode != QXP_SHADER_OP_CALLNZ)
            continue;
        if (qxp_vs_instruction_is_in_iteration(ir, index, FALSE) &&
            qxp_vs_subroutine_has_iteration(ir, instruction->label_target))
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
    }
    return TRUE;
}

static BOOL qxp_vs_validate_loop_relative_scope(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;

    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD source_index;
        BOOL uses_loop;
        DWORD subroutine_label;

        instruction = &ir->instructions[index];
        uses_loop = FALSE;
        for (source_index = 0; source_index < instruction->source_count;
             ++source_index) {
            if (instruction->sources[source_index].relative.present &&
                instruction->sources[source_index].relative.register_file ==
                    QXP_SHADER_REG_LOOP) {
                uses_loop = TRUE;
                break;
            }
        }
        if (!uses_loop ||
            qxp_vs_instruction_is_in_iteration(ir, index, TRUE))
            continue;

        subroutine_label = QXP_SHADER_INDEX_NONE;
        for (source_index = 0; source_index < index; ++source_index) {
            if (ir->instructions[source_index].opcode == QXP_SHADER_OP_LABEL)
                subroutine_label = source_index;
        }
        if (subroutine_label == QXP_SHADER_INDEX_NONE)
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        for (source_index = 0; source_index < ir->instruction_count;
             ++source_index) {
            const QXP_SHADER_INSTRUCTION *call;

            call = &ir->instructions[source_index];
            if ((call->opcode == QXP_SHADER_OP_CALL ||
                 call->opcode == QXP_SHADER_OP_CALLNZ) &&
                call->label_target == subroutine_label &&
                !qxp_vs_instruction_is_in_iteration(
                    ir, source_index, TRUE))
                return qxp_vs_fail(
                    error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                    index, instruction->token_offset, 0);
        }
    }
    return TRUE;
}

static BOOL qxp_vs_cfg_terminator(DWORD opcode)
{
    return opcode == QXP_SHADER_OP_IF || opcode == QXP_SHADER_OP_ELSE ||
           opcode == QXP_SHADER_OP_LOOP || opcode == QXP_SHADER_OP_REP ||
           opcode == QXP_SHADER_OP_ENDLOOP ||
           opcode == QXP_SHADER_OP_ENDREP || opcode == QXP_SHADER_OP_CALL ||
           opcode == QXP_SHADER_OP_CALLNZ || opcode == QXP_SHADER_OP_RET;
}

static void qxp_vs_mark_leader(BYTE *leaders, DWORD count, DWORD index)
{
    if (index < count) leaders[index] = 1;
}

static BOOL qxp_vs_expected_successor(
    const QXP_SHADER_IR *ir, DWORD block_index, DWORD *successor_count,
    DWORD successors[QXP_SHADER_MAX_SUCCESSORS],
    const DWORD *instruction_to_block, DWORD target_instruction,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD target_block;
    DWORD index;

    if (target_instruction >= ir->instruction_count) return TRUE;
    target_block = instruction_to_block[target_instruction];
    if (target_block >= ir->block_count)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           block_index, QXP_SHADER_INDEX_NONE, 0);
    for (index = 0; index < *successor_count; ++index) {
        if (successors[index] == target_block) return TRUE;
    }
    if (*successor_count >= QXP_SHADER_MAX_SUCCESSORS)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           block_index, QXP_SHADER_INDEX_NONE, 0);
    successors[(*successor_count)++] = target_block;
    return TRUE;
}

static BOOL qxp_vs_validate_cfg(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    BYTE leaders[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD instruction_to_block[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD count;
    DWORD expected_blocks;
    DWORD index;
    DWORD cursor;

    qxp_vs_zero(leaders, (DWORD)sizeof(leaders));
    qxp_vs_zero(instruction_to_block,
                (DWORD)sizeof(instruction_to_block));
    count = ir->instruction_count;
    if (!count) {
        if (ir->block_count != 0)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               QXP_SHADER_INDEX_NONE,
                               QXP_SHADER_INDEX_NONE, 0);
        return TRUE;
    }
    leaders[0] = 1;
    for (index = 0; index < count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &ir->instructions[index];
        opcode = instruction->opcode;
        if (opcode == QXP_SHADER_OP_LABEL)
            qxp_vs_mark_leader(leaders, count, index);
        if (qxp_vs_cfg_terminator(opcode))
            qxp_vs_mark_leader(leaders, count, index + 1);
        if (opcode == QXP_SHADER_OP_IF) {
            qxp_vs_mark_leader(leaders, count, index + 1);
            qxp_vs_mark_leader(
                leaders, count,
                instruction->control_else != QXP_SHADER_INDEX_NONE ?
                    instruction->control_else + 1 :
                    instruction->control_match + 1);
        } else if (opcode == QXP_SHADER_OP_ELSE) {
            qxp_vs_mark_leader(
                leaders, count, instruction->control_match + 1);
        } else if (opcode == QXP_SHADER_OP_LOOP ||
                   opcode == QXP_SHADER_OP_REP) {
            qxp_vs_mark_leader(leaders, count, index + 1);
            qxp_vs_mark_leader(
                leaders, count, instruction->control_match + 1);
        } else if (opcode == QXP_SHADER_OP_ENDLOOP ||
                   opcode == QXP_SHADER_OP_ENDREP) {
            qxp_vs_mark_leader(
                leaders, count, instruction->control_match + 1);
            qxp_vs_mark_leader(leaders, count, index + 1);
        }
    }

    expected_blocks = 0;
    cursor = 0;
    while (cursor < count) {
        DWORD start;
        DWORD scan;

        start = cursor++;
        while (cursor < count && !leaders[cursor]) ++cursor;
        if (expected_blocks >= ir->block_count ||
            ir->blocks[expected_blocks].first_instruction != start ||
            ir->blocks[expected_blocks].instruction_count != cursor - start)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               expected_blocks,
                               ir->instructions[start].token_offset, 0);
        for (scan = start; scan < cursor; ++scan)
            instruction_to_block[scan] = expected_blocks;
        ++expected_blocks;
    }
    if (expected_blocks != ir->block_count)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);

    for (index = 0; index < expected_blocks; ++index) {
        const QXP_SHADER_BASIC_BLOCK *block;
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD successors[QXP_SHADER_MAX_SUCCESSORS];
        DWORD successor_count;
        DWORD last;
        DWORD opcode;
        DWORD successor_index;

        block = &ir->blocks[index];
        last = block->first_instruction + block->instruction_count - 1;
        instruction = &ir->instructions[last];
        opcode = instruction->opcode;
        successor_count = 0;
        successors[0] = QXP_SHADER_INDEX_NONE;
        successors[1] = QXP_SHADER_INDEX_NONE;
        if (opcode == QXP_SHADER_OP_IF) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block, last + 1, error) ||
                !qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block,
                    instruction->control_else != QXP_SHADER_INDEX_NONE ?
                        instruction->control_else + 1 :
                        instruction->control_match + 1,
                    error)) return FALSE;
        } else if (opcode == QXP_SHADER_OP_ELSE) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block,
                    instruction->control_match + 1, error)) return FALSE;
        } else if (opcode == QXP_SHADER_OP_LOOP ||
                   opcode == QXP_SHADER_OP_REP) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block, last + 1, error) ||
                !qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block,
                    instruction->control_match + 1, error)) return FALSE;
        } else if (opcode == QXP_SHADER_OP_ENDLOOP ||
                   opcode == QXP_SHADER_OP_ENDREP) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block,
                    instruction->control_match + 1, error) ||
                !qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block, last + 1, error)) return FALSE;
        } else if (opcode == QXP_SHADER_OP_CALL ||
                   opcode == QXP_SHADER_OP_CALLNZ) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block, last + 1, error)) return FALSE;
        } else if (opcode != QXP_SHADER_OP_RET) {
            if (!qxp_vs_expected_successor(
                    ir, index, &successor_count, successors,
                    instruction_to_block, last + 1, error)) return FALSE;
        }
        if (block->successor_count != successor_count)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);
        for (successor_index = 0;
             successor_index < QXP_SHADER_MAX_SUCCESSORS;
             ++successor_index) {
            DWORD expected;

            expected = successor_index < successor_count ?
                       successors[successor_index] : QXP_SHADER_INDEX_NONE;
            if (block->successors[successor_index] != expected)
                return qxp_vs_fail(
                    error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                    index, instruction->token_offset, 0);
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Operand validation and metadata collection                          */
/* ------------------------------------------------------------------ */

static BOOL qxp_vs_validate_relative(const QXP_SHADER_OPERAND *operand)
{
    if (!operand->relative.present) return TRUE;
    if (operand->register_file != QXP_SHADER_REG_CONST ||
        operand->relative.role != QXP_SHADER_OPERAND_RELATIVE_ADDRESS ||
        operand->relative.reserved != 0)
        return FALSE;
    if (operand->relative.register_file != QXP_SHADER_REG_ADDRESS &&
        operand->relative.register_file != QXP_SHADER_REG_LOOP)
        return FALSE;
    if (operand->relative.register_number != 0) return FALSE;
    if (operand->relative.source_modifier != QXP_SHADER_SRCMOD_NONE)
        return FALSE;
    if (operand->relative.swizzle[0] >= 4 ||
        operand->relative.swizzle[1] >= 4 ||
        operand->relative.swizzle[2] >= 4 ||
        operand->relative.swizzle[3] >= 4)
        return FALSE;
    if (operand->relative.register_file == QXP_SHADER_REG_ADDRESS) {
        BYTE component;

        /* a0 relative replicates one selected component across the swizzle. */
        component = operand->relative.swizzle[0];
        if (operand->relative.swizzle[1] != component ||
            operand->relative.swizzle[2] != component ||
            operand->relative.swizzle[3] != component)
            return FALSE;
    } else {
        /* aL relative uses the loop counter as a scalar with the identity
         * swizzle, exactly as the decoder admits. */
        if (operand->relative.swizzle[0] != 0 ||
            operand->relative.swizzle[1] != 1 ||
            operand->relative.swizzle[2] != 2 ||
            operand->relative.swizzle[3] != 3)
            return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_source_aux_valid(const QXP_SHADER_OPERAND *operand)
{
    if (operand->write_mask != 0 ||
        operand->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        operand->destination_shift != 0)
        return FALSE;
    if (!operand->relative.present &&
        !qxp_vs_memory_is_zero(
            &operand->relative, (DWORD)sizeof(operand->relative)))
        return FALSE;
    return TRUE;
}

static BOOL qxp_vs_destination_aux_valid(const QXP_SHADER_OPERAND *operand)
{
    DWORD index;

    if (operand->source_modifier != QXP_SHADER_SRCMOD_NONE ||
        !qxp_vs_memory_is_zero(
            &operand->relative, (DWORD)sizeof(operand->relative)))
        return FALSE;
    for (index = 0; index < 4; ++index) {
        if (operand->swizzle[index] != 0) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_validate_float_source(
    const QXP_SHADER_IR *ir, DWORD index,
    const QXP_SHADER_OPERAND *operand,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    DWORD limit;
    DWORD swizzle_index;

    instruction = &ir->instructions[index];
    if (operand->role != QXP_SHADER_OPERAND_SOURCE ||
        !qxp_vs_source_aux_valid(operand))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    limit = 0;
    if (operand->register_file == QXP_SHADER_REG_TEMP) limit = 12;
    else if (operand->register_file == QXP_SHADER_REG_INPUT) limit = 16;
    else if (operand->register_file == QXP_SHADER_REG_CONST) limit = 256;
    if (!limit || (DWORD)operand->register_number >= limit)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    if (operand->source_modifier != QXP_SHADER_SRCMOD_NONE &&
        operand->source_modifier != QXP_SHADER_SRCMOD_NEGATE)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    for (swizzle_index = 0; swizzle_index < 4; ++swizzle_index) {
        if (operand->swizzle[swizzle_index] >= 4)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
    }
    if (!qxp_vs_validate_relative(operand))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    if (operand->register_file == QXP_SHADER_REG_TEMP ||
        operand->register_file == QXP_SHADER_REG_CONST) {
        DWORD required;

        required = (DWORD)operand->register_number + 1;
        if (operand->register_file == QXP_SHADER_REG_TEMP) {
            if (required > metadata->required_temp_count)
                metadata->required_temp_count = required;
        } else if (required > metadata->required_float_constant_count) {
            metadata->required_float_constant_count = required;
        }
    }
    if (operand->relative.present) {
        metadata->uses_relative_constant = TRUE;
        if (operand->relative.register_file == QXP_SHADER_REG_ADDRESS)
            metadata->uses_address_register = TRUE;
        else
            metadata->uses_loop_register = TRUE;
    }
    return TRUE;
}

static BOOL qxp_vs_validate_relative_sources(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    DWORD relative_file;
    DWORD relative_component;
    DWORD source_index;

    instruction = &ir->instructions[index];
    relative_file = QXP_SHADER_INDEX_NONE;
    relative_component = QXP_SHADER_INDEX_NONE;
    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        const QXP_SHADER_RELATIVE_OPERAND *relative;
        DWORD component;

        relative = &instruction->sources[source_index].relative;
        if (!relative->present) continue;
        component = relative->register_file == QXP_SHADER_REG_LOOP ?
                    0 : relative->swizzle[0];
        if (relative_file == QXP_SHADER_INDEX_NONE) {
            relative_file = relative->register_file;
            relative_component = component;
        } else if (relative_file != relative->register_file ||
                   relative_component != component) {
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        }
    }
    return TRUE;
}

static BOOL qxp_vs_validate_destination(
    const QXP_SHADER_IR *ir, DWORD index,
    const QXP_SHADER_OPERAND *operand,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    DWORD mask;

    instruction = &ir->instructions[index];
    if (operand->role != QXP_SHADER_OPERAND_DESTINATION ||
        !qxp_vs_destination_aux_valid(operand) ||
        operand->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        operand->destination_shift != 0 || operand->relative.present)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    mask = operand->write_mask;
    if (!mask || mask > 15)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);

    if (operand->register_file == QXP_SHADER_REG_TEMP) {
        DWORD required;

        if (operand->register_number >= 12)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        required = (DWORD)operand->register_number + 1;
        if (required > metadata->required_temp_count)
            metadata->required_temp_count = required;
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_RASTOUT) {
        if (operand->register_number >= 3)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        if (operand->register_number != QXP_VS_RASTOUT_POSITION && mask != 15)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        if (operand->register_number == QXP_VS_RASTOUT_POSITION) {
            metadata->writes_position = TRUE;
            metadata->position_component_mask =
                (BYTE)(metadata->position_component_mask | mask);
        } else if (operand->register_number == QXP_VS_RASTOUT_FOG) {
            metadata->writes_fog = TRUE;
            metadata->fog_component_mask =
                (BYTE)(metadata->fog_component_mask | mask);
        } else {
            metadata->writes_point_size = TRUE;
            metadata->point_size_component_mask =
                (BYTE)(metadata->point_size_component_mask | mask);
        }
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_ATTROUT) {
        if (operand->register_number >= 2)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->attribute_output_mask |= 1UL << operand->register_number;
        metadata->attribute_output_components[operand->register_number] =
            (BYTE)(metadata->attribute_output_components[
                       operand->register_number] | mask);
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_TEXCRDOUT) {
        if (operand->register_number >= 8)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->texcoord_output_mask |= 1UL << operand->register_number;
        metadata->texcoord_output_components[operand->register_number] =
            (BYTE)(metadata->texcoord_output_components[
                       operand->register_number] | mask);
        return TRUE;
    }
    return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                       index, instruction->token_offset, 0);
}

static BOOL qxp_vs_validate_read_ports(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    const QXP_SHADER_OPERAND *first_constant;
    const QXP_SHADER_OPERAND *first_input;
    DWORD constant_reads;
    DWORD source_index;
    BOOL sincos;

    instruction = &ir->instructions[index];
    if (qxp_vs_is_matrix_opcode(instruction->opcode)) {
        const QXP_SHADER_OPERAND *source0;
        const QXP_SHADER_OPERAND *source1;

        source0 = &instruction->sources[0];
        source1 = &instruction->sources[1];
        if ((source0->register_file == QXP_SHADER_REG_INPUT &&
             source1->register_file == QXP_SHADER_REG_INPUT) ||
            (source0->register_file == QXP_SHADER_REG_CONST &&
             source1->register_file == QXP_SHADER_REG_CONST))
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);
    }
    first_constant = (const QXP_SHADER_OPERAND *)0;
    first_input = (const QXP_SHADER_OPERAND *)0;
    constant_reads = 0;
    sincos = instruction->opcode == QXP_SHADER_OP_SINCOS;
    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        const QXP_SHADER_OPERAND *source;

        source = &instruction->sources[source_index];
        if (source->register_file == QXP_SHADER_REG_CONST && !sincos) {
            if (++constant_reads > 2)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                                   index, instruction->token_offset, 0);
            if (!first_constant) {
                first_constant = source;
            } else if (first_constant->register_number !=
                           source->register_number ||
                       first_constant->relative.present !=
                           source->relative.present) {
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                                   index, instruction->token_offset, 0);
            }
        }
        if (source->register_file == QXP_SHADER_REG_INPUT) {
            if (!first_input) {
                first_input = source;
            } else if (first_input->register_number !=
                       source->register_number) {
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                                   index, instruction->token_offset, 0);
            }
        }
    }
    return TRUE;
}

static BOOL qxp_vs_validate_matrix_form(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    const QXP_SHADER_OPERAND *destination;
    const QXP_SHADER_OPERAND *source0;
    const QXP_SHADER_OPERAND *source1;
    DWORD rows;
    DWORD required_mask;
    DWORD limit;

    instruction = &ir->instructions[index];
    destination = &instruction->destinations[0];
    source0 = &instruction->sources[0];
    source1 = &instruction->sources[1];
    rows = qxp_vs_matrix_rows(instruction->opcode);
    required_mask = rows == 4 ? 15 : (rows == 3 ? 7 : 3);
    if (destination->write_mask != required_mask ||
        !qxp_vs_swizzle_is_identity(source1) ||
        source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
        qxp_vs_operands_alias(destination, source0))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                           index, instruction->token_offset, 0);
    limit = 0;
    if (source1->register_file == QXP_SHADER_REG_TEMP) limit = 12;
    else if (source1->register_file == QXP_SHADER_REG_INPUT) limit = 16;
    else if (source1->register_file == QXP_SHADER_REG_CONST) limit = 256;
    if (!limit ||
        (!source1->relative.present &&
         (DWORD)source1->register_number + rows > limit))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    if (destination->register_file == source1->register_file &&
        (DWORD)destination->register_number >=
            (DWORD)source1->register_number &&
        (DWORD)destination->register_number <
            (DWORD)source1->register_number + rows)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    if (source1->register_file == QXP_SHADER_REG_TEMP) {
        DWORD required;

        required = (DWORD)source1->register_number + rows;
        if (required > metadata->required_temp_count)
            metadata->required_temp_count = required;
    }
    return TRUE;
}

static BOOL qxp_vs_validate_arithmetic(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    const QXP_SHADER_OPERAND *destination;
    DWORD opcode;
    DWORD source_index;

    instruction = &ir->instructions[index];
    opcode = instruction->opcode;
    destination = instruction->destination_count ?
        &instruction->destinations[0] : (const QXP_SHADER_OPERAND *)0;

    if (qxp_vs_is_address_write(instruction)) {
        if (!destination ||
            destination->role != QXP_SHADER_OPERAND_DESTINATION ||
            !qxp_vs_destination_aux_valid(destination) ||
            destination->register_file != QXP_SHADER_REG_ADDRESS ||
            destination->register_number != 0 ||
            !destination->write_mask ||
            destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0 ||
            destination->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->uses_address_register = TRUE;
    } else if (destination) {
        if (!qxp_vs_validate_destination(ir, index, destination, metadata,
                                         error))
            return FALSE;
    }

    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        if (!qxp_vs_validate_float_source(
                ir, index, &instruction->sources[source_index], metadata,
                error))
            return FALSE;
    }
    if (!qxp_vs_validate_read_ports(ir, index, error)) return FALSE;

    if (destination &&
        destination->register_file == QXP_SHADER_REG_RASTOUT &&
        destination->register_number != QXP_VS_RASTOUT_POSITION) {
        for (source_index = 0; source_index < instruction->source_count;
             ++source_index) {
            if (!qxp_vs_swizzle_is_replicate(
                    &instruction->sources[source_index]))
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                                   index, instruction->token_offset, 0);
        }
    }
    if (qxp_vs_is_scalar_opcode(opcode)) {
        DWORD mask;

        mask = destination->write_mask;
        if (!qxp_vs_swizzle_is_replicate(&instruction->sources[0]) ||
            (mask & (mask - 1)) != 0)
            return qxp_vs_fail(
                error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_POW) {
        DWORD mask;

        mask = destination->write_mask;
        if (!destination || (mask & (mask - 1)) != 0 ||
            destination->register_file != QXP_SHADER_REG_TEMP ||
            !qxp_vs_swizzle_is_replicate(&instruction->sources[0]) ||
            !qxp_vs_swizzle_is_replicate(&instruction->sources[1]) ||
            qxp_vs_operands_alias(destination, &instruction->sources[1]))
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_NRM) {
        if (!destination ||
            destination->register_file != QXP_SHADER_REG_TEMP ||
            qxp_vs_operands_alias(destination, &instruction->sources[0]) ||
            (destination->write_mask != 7 && destination->write_mask != 15))
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_SGN) {
        const QXP_SHADER_OPERAND *source1;
        const QXP_SHADER_OPERAND *source2;

        source1 = &instruction->sources[1];
        source2 = &instruction->sources[2];
        if (source1->register_file != QXP_SHADER_REG_TEMP ||
            source1->register_number >= 12 ||
            source2->register_file != QXP_SHADER_REG_TEMP ||
            source2->register_number >= 12 ||
            qxp_vs_operands_alias(source1, source2) ||
            !qxp_vs_swizzle_is_identity(source1) ||
            !qxp_vs_swizzle_is_identity(source2) ||
            source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source2->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source1->relative.present || source2->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_SINCOS) {
        const QXP_SHADER_OPERAND *source1;
        const QXP_SHADER_OPERAND *source2;

        source1 = &instruction->sources[1];
        source2 = &instruction->sources[2];
        if (!destination ||
            destination->register_file != QXP_SHADER_REG_TEMP ||
            (destination->write_mask != 1 && destination->write_mask != 2 &&
             destination->write_mask != 3) ||
            qxp_vs_operands_alias(destination, &instruction->sources[0]) ||
            !qxp_vs_swizzle_is_replicate(&instruction->sources[0]) ||
            source1->register_file != QXP_SHADER_REG_CONST ||
            source2->register_file != QXP_SHADER_REG_CONST ||
            source1->register_number >= 256 ||
            source2->register_number >= 256 ||
            source1->register_number == source2->register_number ||
            !qxp_vs_swizzle_is_identity(source1) ||
            !qxp_vs_swizzle_is_identity(source2) ||
            source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source2->source_modifier != QXP_SHADER_SRCMOD_NONE)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_CRS) {
        if (!destination ||
            destination->register_file != QXP_SHADER_REG_TEMP ||
            (destination->write_mask & 8) != 0 ||
            qxp_vs_operands_alias(destination, &instruction->sources[0]) ||
            qxp_vs_operands_alias(destination, &instruction->sources[1]) ||
            !qxp_vs_swizzle_is_identity(&instruction->sources[0]) ||
            !qxp_vs_swizzle_is_identity(&instruction->sources[1]))
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                               index, instruction->token_offset, 0);
    }
    if (qxp_vs_is_matrix_opcode(opcode)) {
        if (!qxp_vs_validate_matrix_form(ir, index, metadata, error))
            return FALSE;
    }
    return qxp_vs_validate_relative_sources(ir, index, error);
}

static BOOL qxp_vs_validate_flow_operands(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    DWORD opcode;
    DWORD source_index;

    instruction = &ir->instructions[index];
    opcode = instruction->opcode;
    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        if (!qxp_vs_source_aux_valid(&instruction->sources[source_index]))
            return qxp_vs_fail(
                error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                index, instruction->token_offset, 0);
    }
    if (opcode == QXP_SHADER_OP_RET || opcode == QXP_SHADER_OP_ENDLOOP ||
        opcode == QXP_SHADER_OP_ENDREP || opcode == QXP_SHADER_OP_ELSE ||
        opcode == QXP_SHADER_OP_ENDIF)
        return TRUE;
    if (opcode == QXP_SHADER_OP_CALL || opcode == QXP_SHADER_OP_LABEL) {
        const QXP_SHADER_OPERAND *source;

        source = &instruction->sources[0];
        if (source->role != QXP_SHADER_OPERAND_LABEL ||
            source->register_file != QXP_SHADER_REG_LABEL ||
            (DWORD)source->register_number >= QXP_SHADER_MAX_LABELS ||
            source->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(source) || source->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_CALLNZ) {
        const QXP_SHADER_OPERAND *label;
        const QXP_SHADER_OPERAND *condition;

        label = &instruction->sources[0];
        condition = &instruction->sources[1];
        if (label->role != QXP_SHADER_OPERAND_LABEL ||
            label->register_file != QXP_SHADER_REG_LABEL ||
            (DWORD)label->register_number >= QXP_SHADER_MAX_LABELS ||
            label->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(label) || label->relative.present ||
            condition->role != QXP_SHADER_OPERAND_CONDITION ||
            condition->register_file != QXP_SHADER_REG_CONSTBOOL ||
            (DWORD)condition->register_number >= 16 ||
            (condition->source_modifier != QXP_SHADER_SRCMOD_NONE &&
             condition->source_modifier != QXP_SHADER_SRCMOD_NOT) ||
            !qxp_vs_swizzle_is_identity(condition) ||
            condition->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        metadata->uses_boolean_constants = TRUE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_LOOP) {
        const QXP_SHADER_OPERAND *loop;
        const QXP_SHADER_OPERAND *counter;

        loop = &instruction->sources[0];
        counter = &instruction->sources[1];
        if (loop->role != QXP_SHADER_OPERAND_LOOP_CONTROL ||
            loop->register_file != QXP_SHADER_REG_LOOP ||
            loop->register_number != 0 ||
            loop->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(loop) || loop->relative.present ||
            counter->role != QXP_SHADER_OPERAND_LOOP_CONTROL ||
            counter->register_file != QXP_SHADER_REG_CONSTINT ||
            (DWORD)counter->register_number >= 16 ||
            counter->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(counter) ||
            counter->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        metadata->uses_integer_constants = TRUE;
        metadata->uses_loop_register = TRUE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_REP) {
        const QXP_SHADER_OPERAND *counter;

        counter = &instruction->sources[0];
        if (counter->role != QXP_SHADER_OPERAND_LOOP_CONTROL ||
            counter->register_file != QXP_SHADER_REG_CONSTINT ||
            (DWORD)counter->register_number >= 16 ||
            counter->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(counter) ||
            counter->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        metadata->uses_integer_constants = TRUE;
        return TRUE;
    }
    if (opcode == QXP_SHADER_OP_IF) {
        const QXP_SHADER_OPERAND *condition;

        condition = &instruction->sources[0];
        if (condition->role != QXP_SHADER_OPERAND_CONDITION ||
            condition->register_file != QXP_SHADER_REG_CONSTBOOL ||
            (DWORD)condition->register_number >= 16 ||
            condition->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(condition) ||
            condition->relative.present)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, instruction->token_offset, 0);
        metadata->uses_boolean_constants = TRUE;
        return TRUE;
    }
    return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_UNSUPPORTED_FORM,
                       index, instruction->token_offset, 0);
}

static BOOL qxp_vs_validate_declaration(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    const QXP_SHADER_OPERAND *destination;
    DWORD previous;
    DWORD raw;
    DWORD raw_usage;
    DWORD raw_usage_index;

    instruction = &ir->instructions[index];
    destination = &instruction->destinations[0];
    raw = instruction->declaration.raw_token;
    raw_usage = raw & QXP_VS_DCL_USAGE_MASK;
    raw_usage_index = (raw & QXP_VS_DCL_USAGE_INDEX_MASK) >>
                      QXP_VS_DCL_USAGE_INDEX_SHIFT;
    if (instruction->declaration.kind != QXP_SHADER_DECLARATION_INPUT ||
        instruction->declaration.sampler_type != QXP_SHADER_SAMPLER_NONE ||
        instruction->declaration.usage > 13 ||
        instruction->declaration.usage_index > 15 ||
        (raw & QXP_VS_DCL_PARAMETER_MARKER) == 0 ||
        (raw & ~(QXP_VS_DCL_PARAMETER_MARKER |
                 QXP_VS_DCL_USAGE_MASK |
                 QXP_VS_DCL_USAGE_INDEX_MASK)) != 0 ||
        raw_usage != instruction->declaration.usage ||
        raw_usage_index != instruction->declaration.usage_index ||
        destination->role != QXP_SHADER_OPERAND_DECLARATION ||
        !qxp_vs_destination_aux_valid(destination) ||
        destination->register_file != QXP_SHADER_REG_INPUT ||
        destination->register_number >= 16 ||
        destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0 ||
        destination->relative.present)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                           index, instruction->token_offset, 0);
    if (metadata->input_mask & (1UL << destination->register_number))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                           index, instruction->token_offset, 0);
    for (previous = 0; previous < index; ++previous) {
        const QXP_SHADER_INSTRUCTION *earlier;

        earlier = &ir->instructions[previous];
        if (earlier->opcode == QXP_SHADER_OP_DCL &&
            earlier->declaration.usage == instruction->declaration.usage &&
            earlier->declaration.usage_index ==
                instruction->declaration.usage_index)
            return qxp_vs_fail(
                error, QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                index, instruction->token_offset, 0);
    }
    metadata->input_mask |= 1UL << destination->register_number;
    metadata->input_usage[destination->register_number] =
        instruction->declaration.usage;
    metadata->input_usage_index[destination->register_number] =
        instruction->declaration.usage_index;
    metadata->input_component_mask[destination->register_number] =
        (BYTE)destination->write_mask;
    return TRUE;
}

static BOOL qxp_vs_validate_definition(
    const QXP_SHADER_IR *ir, DWORD index,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    const QXP_SHADER_OPERAND *destination;
    DWORD number;

    instruction = &ir->instructions[index];
    destination = &instruction->destinations[0];
    if (destination->role != QXP_SHADER_OPERAND_DEFINITION ||
        !qxp_vs_destination_aux_valid(destination) ||
        destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0 ||
        destination->relative.present)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           index, instruction->token_offset, 0);
    number = destination->register_number;
    if (instruction->opcode == QXP_SHADER_OP_DEF) {
        if (destination->register_file != QXP_SHADER_REG_CONST ||
            number >= 256)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->float_definition_mask[number >> 5] |=
            1UL << (number & 31);
        metadata->float_definition_values[number][0] =
            instruction->immediate[0];
        metadata->float_definition_values[number][1] =
            instruction->immediate[1];
        metadata->float_definition_values[number][2] =
            instruction->immediate[2];
        metadata->float_definition_values[number][3] =
            instruction->immediate[3];
    } else if (instruction->opcode == QXP_SHADER_OP_DEFI) {
        if (destination->register_file != QXP_SHADER_REG_CONSTINT ||
            number >= 16)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->integer_definition_mask |= 1UL << number;
        metadata->integer_definition_values[number][0] =
            instruction->immediate[0];
        metadata->integer_definition_values[number][1] =
            instruction->immediate[1];
        metadata->integer_definition_values[number][2] =
            instruction->immediate[2];
        metadata->integer_definition_values[number][3] =
            instruction->immediate[3];
        metadata->uses_integer_constants = TRUE;
    } else {
        if (destination->register_file != QXP_SHADER_REG_CONSTBOOL ||
            number >= 16)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               index, instruction->token_offset, 0);
        metadata->boolean_definition_mask |= 1UL << number;
        metadata->boolean_definition_values[number] =
            instruction->immediate[0];
        metadata->uses_boolean_constants = TRUE;
    }
    return TRUE;
}

static BOOL qxp_vs_collect_metadata(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    DWORD declaration_count;
    DWORD definition_count;
    DWORD arithmetic_slots;
    DWORD flow_slots;
    DWORD total_slots;
    BOOL executable_seen;

    declaration_count = 0;
    definition_count = 0;
    arithmetic_slots = 0;
    flow_slots = 0;
    total_slots = 0;
    executable_seen = FALSE;
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &ir->instructions[index];
        opcode = instruction->opcode;
        total_slots += instruction->slot_count;
        if (instruction->slot_class == QXP_SHADER_SLOT_ARITHMETIC)
            arithmetic_slots += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_FLOW)
            flow_slots += instruction->slot_count;
        else if (instruction->slot_class != QXP_SHADER_SLOT_SETUP)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               index, instruction->token_offset, 0);

        if (opcode == QXP_SHADER_OP_DCL) {
            if (executable_seen)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                                   index, instruction->token_offset, 0);
            ++declaration_count;
            if (!qxp_vs_validate_declaration(ir, index, metadata, error))
                return FALSE;
        } else if (opcode == QXP_SHADER_OP_DEF ||
                   opcode == QXP_SHADER_OP_DEFI ||
                   opcode == QXP_SHADER_OP_DEFB) {
            if (executable_seen)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                                   index, instruction->token_offset, 0);
            ++definition_count;
            if (!qxp_vs_validate_definition(ir, index, metadata, error))
                return FALSE;
        } else if (opcode == QXP_SHADER_OP_NOP) {
            executable_seen = TRUE;
        } else if (opcode == QXP_SHADER_OP_CALL ||
                   opcode == QXP_SHADER_OP_CALLNZ ||
                   opcode == QXP_SHADER_OP_LOOP ||
                   opcode == QXP_SHADER_OP_RET ||
                   opcode == QXP_SHADER_OP_ENDLOOP ||
                   opcode == QXP_SHADER_OP_LABEL ||
                   opcode == QXP_SHADER_OP_REP ||
                   opcode == QXP_SHADER_OP_ENDREP ||
                   opcode == QXP_SHADER_OP_IF ||
                   opcode == QXP_SHADER_OP_ELSE ||
                   opcode == QXP_SHADER_OP_ENDIF) {
            executable_seen = TRUE;
            if (instruction->declaration.present)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                                   index, instruction->token_offset, 0);
            if (!qxp_vs_validate_flow_operands(ir, index, metadata, error))
                return FALSE;
        } else {
            executable_seen = TRUE;
            if (instruction->declaration.present)
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION,
                                   index, instruction->token_offset, 0);
            if (!qxp_vs_validate_arithmetic(ir, index, metadata, error))
                return FALSE;
        }
    }

    if (metadata->required_temp_count > QXP_VERTEX_SHADER_GLSL_TEMP_COUNT)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (declaration_count != ir->declaration_count ||
        definition_count != ir->definition_count ||
        arithmetic_slots != ir->arithmetic_slot_count ||
        flow_slots != ir->flow_slot_count ||
        total_slots != ir->slot_count || total_slots > 256)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    metadata->instruction_count = ir->instruction_count;
    metadata->slot_count = ir->slot_count;
    metadata->arithmetic_slot_count = ir->arithmetic_slot_count;
    metadata->flow_slot_count = ir->flow_slot_count;
    metadata->declaration_count = ir->declaration_count;
    metadata->definition_count = ir->definition_count;
    return TRUE;
}

static BOOL qxp_vs_validate_outputs(
    const QXP_SHADER_IR *ir, const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;
    BOOL saw_gap;

    if (!metadata->writes_position ||
        metadata->position_component_mask != 15)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                           QXP_SHADER_INDEX_NONE,
                           ir->token_count ? ir->token_count - 1 : 0, 0);
    saw_gap = FALSE;
    for (index = 0; index < 8; ++index) {
        DWORD mask;

        if (!(metadata->texcoord_output_mask & (1UL << index))) {
            saw_gap = TRUE;
            continue;
        }
        if (saw_gap)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               QXP_SHADER_INDEX_NONE,
                               ir->token_count ? ir->token_count - 1 : 0, 0);
        mask = metadata->texcoord_output_components[index];
        if (mask != 1 && mask != 3 && mask != 7 && mask != 15)
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER,
                               QXP_SHADER_INDEX_NONE,
                               ir->token_count ? ir->token_count - 1 : 0, 0);
    }
    return TRUE;
}

static BOOL qxp_vs_validate_declared_inputs(
    const QXP_SHADER_IR *ir, const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD index;

    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD source_index;

        instruction = &ir->instructions[index];
        if (instruction->opcode == QXP_SHADER_OP_DCL) continue;
        for (source_index = 0; source_index < instruction->source_count;
             ++source_index) {
            const QXP_SHADER_OPERAND *source;

            source = &instruction->sources[source_index];
            if (source->register_file == QXP_SHADER_REG_INPUT &&
                !(metadata->input_mask &
                  (1UL << source->register_number)))
                return qxp_vs_fail(error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
                                   index, instruction->token_offset, 0);
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Independent definite-assignment forward analysis                    */
/* ------------------------------------------------------------------ */

typedef struct qxp_vs_state {
    BYTE temp_masks[QXP_VERTEX_SHADER_GLSL_TEMP_COUNT];
    BYTE address_mask;
} QXP_VS_STATE;

typedef struct qxp_vs_summary {
    BOOL present;
    QXP_VS_STATE required;
    QXP_VS_STATE guaranteed;
    QXP_VS_STATE available_if_all;
} QXP_VS_SUMMARY;

typedef struct qxp_vs_flow_frame {
    DWORD opcode;
    BOOL saw_else;
    QXP_VS_STATE entry;
    QXP_VS_STATE branch;
} QXP_VS_FLOW_FRAME;

typedef struct qxp_vs_dataflow {
    const QXP_SHADER_IR *ir;
    QXP_VERTEX_SHADER_GLSL_ERROR *error;
    QXP_VS_SUMMARY summaries[QXP_SHADER_MAX_LABELS];
} QXP_VS_DATAFLOW;

static void qxp_vs_state_zero(QXP_VS_STATE *state)
{
    qxp_vs_zero(state, (DWORD)sizeof(*state));
}

static void qxp_vs_state_all(QXP_VS_STATE *state)
{
    DWORD index;

    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index)
        state->temp_masks[index] = 15;
    state->address_mask = 15;
}

static void qxp_vs_state_intersect(QXP_VS_STATE *destination,
                                   const QXP_VS_STATE *left,
                                   const QXP_VS_STATE *right)
{
    DWORD index;

    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index)
        destination->temp_masks[index] =
            (BYTE)(left->temp_masks[index] & right->temp_masks[index]);
    destination->address_mask =
        (BYTE)(left->address_mask & right->address_mask);
}

static BOOL qxp_vs_state_has_bits(const QXP_VS_STATE *state)
{
    DWORD index;

    if (state->address_mask) return TRUE;
    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index) {
        if (state->temp_masks[index]) return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_state_add(QXP_VS_STATE *destination,
                             const QXP_VS_STATE *source)
{
    DWORD index;
    BOOL changed;

    changed = FALSE;
    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index) {
        BYTE combined;

        combined = (BYTE)(destination->temp_masks[index] |
                          source->temp_masks[index]);
        if (combined != destination->temp_masks[index]) changed = TRUE;
        destination->temp_masks[index] = combined;
    }
    if ((BYTE)(destination->address_mask | source->address_mask) !=
        destination->address_mask) changed = TRUE;
    destination->address_mask =
        (BYTE)(destination->address_mask | source->address_mask);
    return changed;
}

static BOOL qxp_vs_state_includes(
    const QXP_VS_STATE *available, const QXP_VS_STATE *required)
{
    DWORD index;

    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index) {
        if ((DWORD)required->temp_masks[index] &
            ~(DWORD)available->temp_masks[index])
            return FALSE;
    }
    if ((DWORD)required->address_mask &
        ~(DWORD)available->address_mask)
        return FALSE;
    return TRUE;
}

static void qxp_vs_require_temp(const QXP_VS_STATE *state,
                               QXP_VS_STATE *missing, DWORD register_number,
                               DWORD mask, DWORD token_offset,
                               DWORD *first_missing_offset)
{
    DWORD absent;

    if (register_number >= QXP_VERTEX_SHADER_GLSL_TEMP_COUNT) return;
    absent = mask & ~(DWORD)state->temp_masks[register_number] & 15UL;
    if (!absent) return;
    missing->temp_masks[register_number] =
        (BYTE)(missing->temp_masks[register_number] | absent);
    if (*first_missing_offset == QXP_SHADER_INDEX_NONE)
        *first_missing_offset = token_offset;
}

static void qxp_vs_require_address(const QXP_VS_STATE *state,
                                  QXP_VS_STATE *missing, DWORD component,
                                  DWORD token_offset,
                                  DWORD *first_missing_offset)
{
    DWORD bit;

    component &= 3UL;
    bit = 1UL << component;
    if (state->address_mask & bit) return;
    missing->address_mask = (BYTE)(missing->address_mask | bit);
    if (*first_missing_offset == QXP_SHADER_INDEX_NONE)
        *first_missing_offset = token_offset;
}

static void qxp_vs_require_state(const QXP_VS_STATE *state,
                                QXP_VS_STATE *missing,
                                const QXP_VS_STATE *required,
                                DWORD token_offset,
                                DWORD *first_missing_offset)
{
    DWORD index;

    for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index)
        qxp_vs_require_temp(state, missing, index,
                            required->temp_masks[index], token_offset,
                            first_missing_offset);
    for (index = 0; index < 4; ++index) {
        if (required->address_mask & (1UL << index))
            qxp_vs_require_address(state, missing, index, token_offset,
                                   first_missing_offset);
    }
}

static DWORD qxp_vs_source_logical_mask(
    const QXP_SHADER_INSTRUCTION *instruction, DWORD source_index)
{
    DWORD destination_mask;
    DWORD opcode;
    DWORD mask;

    destination_mask = instruction->destination_count ?
        instruction->destinations[0].write_mask : 15;
    opcode = instruction->opcode;
    if (opcode == QXP_SHADER_OP_DP3) return 7;
    if (opcode == QXP_SHADER_OP_DP4) return 15;
    if (qxp_vs_is_matrix_opcode(opcode))
        return source_index == 0 ?
            (qxp_vs_matrix_columns(opcode) == 4 ? 15 : 7) : 0;
    if (opcode == QXP_SHADER_OP_RCP || opcode == QXP_SHADER_OP_RSQ ||
        opcode == QXP_SHADER_OP_EXP || opcode == QXP_SHADER_OP_LOG ||
        opcode == QXP_SHADER_OP_EXPP || opcode == QXP_SHADER_OP_LOGP ||
        opcode == QXP_SHADER_OP_POW)
        return 1;
    if (opcode == QXP_SHADER_OP_SINCOS) return source_index ? 15 : 1;
    if (opcode == QXP_SHADER_OP_SGN)
        return source_index == 0 ? destination_mask : 0;
    if (opcode == QXP_SHADER_OP_NRM)
        return 7 | (destination_mask & 8);
    if (opcode == QXP_SHADER_OP_CRS) {
        mask = 0;
        if (destination_mask & 1) mask |= 6;
        if (destination_mask & 2) mask |= 5;
        if (destination_mask & 4) mask |= 3;
        return mask;
    }
    if (opcode == QXP_SHADER_OP_DST) {
        mask = 0;
        if (destination_mask & 2) mask |= 2;
        if (source_index == 0 && (destination_mask & 4)) mask |= 4;
        if (source_index == 1 && (destination_mask & 8)) mask |= 8;
        return mask;
    }
    if (opcode == QXP_SHADER_OP_LIT) {
        mask = 0;
        if (destination_mask & 2) mask |= 1;
        if (destination_mask & 4) mask |= 11;
        return mask;
    }
    return destination_mask;
}

static void qxp_vs_collect_reads(QXP_VS_DATAFLOW *df,
                                 const QXP_SHADER_INSTRUCTION *instruction,
                                 const QXP_VS_STATE *state,
                                 QXP_VS_STATE *missing,
                                 DWORD *first_missing_offset)
{
    DWORD source_index;

    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        const QXP_SHADER_OPERAND *source;

        source = &instruction->sources[source_index];
        if (source->relative.present &&
            source->relative.register_file == QXP_SHADER_REG_ADDRESS)
            qxp_vs_require_address(state, missing,
                                   source->relative.swizzle[0],
                                   instruction->token_offset,
                                   first_missing_offset);
    }

    if (qxp_vs_is_matrix_opcode(instruction->opcode)) {
        const QXP_SHADER_OPERAND *vector;
        const QXP_SHADER_OPERAND *matrix;
        DWORD columns_mask;
        DWORD destination_mask;

        vector = &instruction->sources[0];
        matrix = &instruction->sources[1];
        columns_mask = qxp_vs_matrix_columns(instruction->opcode) == 4 ?
                       15 : 7;
        destination_mask = instruction->destinations[0].write_mask;
        if (vector->register_file == QXP_SHADER_REG_TEMP)
            qxp_vs_require_temp(state, missing, vector->register_number,
                                qxp_vs_swizzled_component_mask(vector,
                                                               columns_mask),
                                instruction->token_offset,
                                first_missing_offset);
        if (matrix->register_file == QXP_SHADER_REG_TEMP) {
            DWORD row;

            for (row = 0; row < qxp_vs_matrix_rows(instruction->opcode);
                 ++row) {
                if (destination_mask & (1UL << row))
                    qxp_vs_require_temp(
                        state, missing,
                        (DWORD)matrix->register_number + row,
                        qxp_vs_swizzled_component_mask(matrix, columns_mask),
                        instruction->token_offset, first_missing_offset);
            }
        }
        return;
    }

    for (source_index = 0; source_index < instruction->source_count;
         ++source_index) {
        const QXP_SHADER_OPERAND *source;
        DWORD logical_mask;

        source = &instruction->sources[source_index];
        if (source->register_file != QXP_SHADER_REG_TEMP) continue;
        logical_mask = qxp_vs_source_logical_mask(instruction, source_index);
        if (!logical_mask) continue;
        qxp_vs_require_temp(state, missing, source->register_number,
                            qxp_vs_swizzled_component_mask(source,
                                                           logical_mask),
                            instruction->token_offset, first_missing_offset);
    }

    if (instruction->opcode == QXP_SHADER_OP_CALL ||
        instruction->opcode == QXP_SHADER_OP_CALLNZ) {
        DWORD label;
        const QXP_VS_SUMMARY *summary;

        label = instruction->sources[0].register_number;
        if (label >= QXP_SHADER_MAX_LABELS) return;
        summary = &df->summaries[label];
        if (summary->present)
            qxp_vs_require_state(state, missing, &summary->required,
                                 instruction->token_offset,
                                 first_missing_offset);
    }
}

static void qxp_vs_apply_call(QXP_VS_DATAFLOW *df,
                              const QXP_SHADER_INSTRUCTION *instruction,
                              QXP_VS_STATE *state)
{
    const QXP_VS_SUMMARY *summary;
    DWORD label;
    DWORD index;

    label = instruction->sources[0].register_number;
    if (label >= QXP_SHADER_MAX_LABELS) return;
    summary = &df->summaries[label];
    if (!summary->present) return;
    if (instruction->opcode == QXP_SHADER_OP_CALL) {
        for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index)
            state->temp_masks[index] = (BYTE)(
                summary->guaranteed.temp_masks[index] |
                (state->temp_masks[index] &
                 summary->available_if_all.temp_masks[index]));
        state->address_mask = (BYTE)(
            summary->guaranteed.address_mask |
            (state->address_mask &
             summary->available_if_all.address_mask));
    } else {
        for (index = 0; index < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT; ++index)
            state->temp_masks[index] = (BYTE)(
                state->temp_masks[index] &
                summary->available_if_all.temp_masks[index]);
        state->address_mask = (BYTE)(
            state->address_mask & summary->available_if_all.address_mask);
    }
}

static void qxp_vs_apply_write(const QXP_SHADER_INSTRUCTION *instruction,
                               QXP_VS_STATE *state)
{
    const QXP_SHADER_OPERAND *destination;

    if (instruction->opcode == QXP_SHADER_OP_SGN) {
        DWORD source_index;

        for (source_index = 1; source_index < 3; ++source_index) {
            const QXP_SHADER_OPERAND *scratch;

            scratch = &instruction->sources[source_index];
            if (scratch->register_file == QXP_SHADER_REG_TEMP &&
                scratch->register_number <
                    QXP_VERTEX_SHADER_GLSL_TEMP_COUNT)
                state->temp_masks[scratch->register_number] = 0;
        }
    }
    if (!instruction->destination_count) return;
    destination = &instruction->destinations[0];
    if (destination->register_file == QXP_SHADER_REG_TEMP &&
        destination->register_number < QXP_VERTEX_SHADER_GLSL_TEMP_COUNT) {
        if (instruction->opcode == QXP_SHADER_OP_SINCOS)
            state->temp_masks[destination->register_number] = (BYTE)(
                (state->temp_masks[destination->register_number] & 8) |
                destination->write_mask);
        else
            state->temp_masks[destination->register_number] = (BYTE)(
                state->temp_masks[destination->register_number] |
                destination->write_mask);
    } else if (destination->register_file == QXP_SHADER_REG_ADDRESS &&
               destination->register_number == 0) {
        state->address_mask =
            (BYTE)(state->address_mask | destination->write_mask);
    }
}

static BOOL qxp_vs_analyze_range(QXP_VS_DATAFLOW *df, DWORD start, DWORD end,
                                 const QXP_VS_STATE *initial,
                                 QXP_VS_STATE *result, QXP_VS_STATE *missing,
                                 DWORD *first_missing_offset)
{
    QXP_VS_FLOW_FRAME flow_stack[QXP_VS_DATAFLOW_MAX_DEPTH];
    QXP_VS_STATE state;
    DWORD flow_depth;
    DWORD index;

    state = *initial;
    qxp_vs_state_zero(missing);
    *first_missing_offset = QXP_SHADER_INDEX_NONE;
    flow_depth = 0;
    for (index = start; index < end; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &df->ir->instructions[index];
        opcode = instruction->opcode;
        qxp_vs_collect_reads(df, instruction, &state, missing,
                             first_missing_offset);
        if (opcode == QXP_SHADER_OP_CALL || opcode == QXP_SHADER_OP_CALLNZ)
            qxp_vs_apply_call(df, instruction, &state);
        else
            qxp_vs_apply_write(instruction, &state);

        if (opcode == QXP_SHADER_OP_IF || opcode == QXP_SHADER_OP_LOOP ||
            opcode == QXP_SHADER_OP_REP) {
            QXP_VS_FLOW_FRAME *frame;

            if (flow_depth >= QXP_VS_DATAFLOW_MAX_DEPTH)
                return qxp_vs_fail(df->error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            frame = &flow_stack[flow_depth++];
            frame->opcode = opcode;
            frame->saw_else = FALSE;
            frame->entry = state;
            qxp_vs_state_zero(&frame->branch);
        } else if (opcode == QXP_SHADER_OP_ELSE) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_vs_fail(df->error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            frame = &flow_stack[flow_depth - 1];
            frame->branch = state;
            frame->saw_else = TRUE;
            state = frame->entry;
        } else if (opcode == QXP_SHADER_OP_ENDIF) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_vs_fail(df->error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            frame = &flow_stack[--flow_depth];
            if (frame->saw_else)
                qxp_vs_state_intersect(&state, &frame->branch, &state);
            else
                qxp_vs_state_intersect(&state, &frame->entry, &state);
        } else if (opcode == QXP_SHADER_OP_ENDLOOP ||
                   opcode == QXP_SHADER_OP_ENDREP) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_vs_fail(df->error,
                                   QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                                   index, instruction->token_offset, 0);
            frame = &flow_stack[--flow_depth];
            qxp_vs_state_intersect(&state, &frame->entry, &state);
        }
    }
    if (flow_depth)
        return qxp_vs_fail(df->error,
                           QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                           start, df->ir->instructions[start].token_offset,
                           0);
    *result = state;
    return TRUE;
}

static BOOL qxp_vs_find_requirements(QXP_VS_DATAFLOW *df, DWORD start,
                                     DWORD end, DWORD fallback_offset,
                                     QXP_VS_STATE *required)
{
    QXP_VS_STATE result;
    QXP_VS_STATE missing;
    DWORD first_missing_offset;
    DWORD pass;

    qxp_vs_state_zero(required);
    for (pass = 0; pass < QXP_VS_DATAFLOW_MAX_PASSES; ++pass) {
        if (!qxp_vs_analyze_range(df, start, end, required, &result,
                                  &missing, &first_missing_offset))
            return FALSE;
        if (!qxp_vs_state_has_bits(&missing)) return TRUE;
        if (!qxp_vs_state_add(required, &missing))
            return qxp_vs_fail(df->error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
                               QXP_SHADER_INDEX_NONE, first_missing_offset, 0);
    }
    return qxp_vs_fail(df->error, QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
                       QXP_SHADER_INDEX_NONE, fallback_offset, 0);
}

static BOOL qxp_vs_build_summaries(QXP_VS_DATAFLOW *df)
{
    DWORD index;

    for (index = 0; index < df->ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *label_instruction;
        QXP_VS_SUMMARY *summary;
        QXP_VS_STATE initial;
        QXP_VS_STATE missing;
        DWORD first_missing_offset;
        DWORD label;
        DWORD end;

        label_instruction = &df->ir->instructions[index];
        if (label_instruction->opcode != QXP_SHADER_OP_LABEL) continue;
        label = label_instruction->sources[0].register_number;
        if (label >= QXP_SHADER_MAX_LABELS)
            return qxp_vs_fail(df->error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, label_instruction->token_offset, 0);
        summary = &df->summaries[label];
        summary->present = TRUE;
        end = index + 1;
        while (end < df->ir->instruction_count &&
               df->ir->instructions[end].opcode != QXP_SHADER_OP_RET)
            ++end;
        if (end < df->ir->instruction_count) ++end;

        if (!qxp_vs_find_requirements(df, index, end,
                                      label_instruction->token_offset,
                                      &summary->required))
            return FALSE;
        qxp_vs_state_zero(&initial);
        if (!qxp_vs_analyze_range(df, index, end, &initial,
                                  &summary->guaranteed, &missing,
                                  &first_missing_offset))
            return FALSE;
        qxp_vs_state_all(&initial);
        if (!qxp_vs_analyze_range(df, index, end, &initial,
                                  &summary->available_if_all, &missing,
                                  &first_missing_offset))
            return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_validate_iteration_stability(QXP_VS_DATAFLOW *df)
{
    DWORD index;

    for (index = 0; index < df->ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *opener;
        QXP_VS_STATE required;
        QXP_VS_STATE available_if_all;
        QXP_VS_STATE initial;
        QXP_VS_STATE result;
        QXP_VS_STATE missing;
        DWORD first_missing_offset;
        DWORD end;

        opener = &df->ir->instructions[index];
        if (opener->opcode != QXP_SHADER_OP_LOOP &&
            opener->opcode != QXP_SHADER_OP_REP)
            continue;
        end = opener->control_match;
        if (end == QXP_SHADER_INDEX_NONE || end <= index ||
            end >= df->ir->instruction_count)
            return qxp_vs_fail(
                df->error, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                index, opener->token_offset, 0);
        if (!qxp_vs_find_requirements(
                df, index + 1, end, opener->token_offset, &required))
            return FALSE;
        qxp_vs_state_all(&initial);
        if (!qxp_vs_analyze_range(
                df, index + 1, end, &initial, &available_if_all,
                &missing, &first_missing_offset))
            return FALSE;
        if (qxp_vs_state_includes(&available_if_all, &required))
            continue;
        if (!qxp_vs_analyze_range(
                df, index + 1, end, &available_if_all, &result,
                &missing, &first_missing_offset))
            return FALSE;
        if (qxp_vs_state_has_bits(&missing) &&
            first_missing_offset != QXP_SHADER_INDEX_NONE)
            return qxp_vs_fail(
                df->error, QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
                index, first_missing_offset, 0);
        return qxp_vs_fail(
            df->error, QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
            index, opener->token_offset, 0);
    }
    return TRUE;
}

static BOOL qxp_vs_validate_definite_assignment(
    const QXP_SHADER_IR *ir, QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    QXP_VS_DATAFLOW df;
    QXP_VS_STATE initial;
    QXP_VS_STATE result;
    QXP_VS_STATE missing;
    DWORD first_missing_offset;
    DWORD main_end;
    DWORD index;

    qxp_vs_zero(&df, (DWORD)sizeof(df));
    df.ir = ir;
    df.error = error;
    if (!qxp_vs_build_summaries(&df)) return FALSE;
    if (!qxp_vs_validate_iteration_stability(&df)) return FALSE;
    main_end = ir->instruction_count;
    for (index = 0; index < ir->instruction_count; ++index) {
        if (ir->instructions[index].opcode == QXP_SHADER_OP_LABEL) {
            main_end = index;
            break;
        }
    }
    qxp_vs_state_zero(&initial);
    if (!qxp_vs_analyze_range(&df, 0, main_end, &initial, &result, &missing,
                              &first_missing_offset))
        return FALSE;
    if (qxp_vs_state_has_bits(&missing))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED,
                           QXP_SHADER_INDEX_NONE, first_missing_offset, 0);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Top-level validation and metadata collection                        */
/* ------------------------------------------------------------------ */

static BOOL qxp_vs_prepare(const QXP_SHADER_IR *ir,
                           QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
                           QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    if (!ir->valid || ir->token_count < 2 ||
        ir->token_count > QXP_SHADER_MAX_TOKENS ||
        ir->instruction_count > QXP_SHADER_MAX_INSTRUCTIONS ||
        ir->block_count > QXP_SHADER_MAX_BLOCKS)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (ir->stage != QXP_SHADER_STAGE_VERTEX ||
        !QXP_SHADER_VERSION_IS_VERTEX(ir->version_token))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_PROFILE,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (ir->required_ps_temp_count || ir->required_ps20_caps ||
        ir->required_ps_instruction_slots || ir->texture_slot_count)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    if (!qxp_vs_validate_instruction_headers(ir, error)) return FALSE;
    if (!qxp_vs_validate_control_structure(ir, metadata, error)) return FALSE;
    if (!qxp_vs_validate_call_iteration_nesting(ir, error)) return FALSE;
    if (!qxp_vs_validate_loop_relative_scope(ir, error)) return FALSE;
    if (!qxp_vs_validate_cfg(ir, error)) return FALSE;
    if (!qxp_vs_collect_metadata(ir, metadata, error)) return FALSE;
    if (!qxp_vs_validate_outputs(ir, metadata, error)) return FALSE;
    if (!qxp_vs_validate_declared_inputs(ir, metadata, error)) return FALSE;
    if (!qxp_vs_validate_definite_assignment(ir, error)) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* GLSL text emission                                                  */
/* ------------------------------------------------------------------ */

typedef struct qxp_vs_text {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_VS_TEXT;

typedef struct qxp_vs_emit {
    QXP_VS_TEXT output;
    QXP_VERTEX_SHADER_GLSL_ERROR *error;
    DWORD instruction_index;
    DWORD token_offset;
} QXP_VS_EMIT;

static void qxp_vs_append_char(QXP_VS_TEXT *output, char value)
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

static void qxp_vs_append(QXP_VS_TEXT *output, const char *value)
{
    if (!output || !value) return;
    while (*value) qxp_vs_append_char(output, *value++);
}

static void qxp_vs_append_uint(QXP_VS_TEXT *output, DWORD value)
{
    char digits[11];
    DWORD count;

    count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < (DWORD)sizeof(digits));
    while (count) qxp_vs_append_char(output, digits[--count]);
}

static void qxp_vs_append_component_mask(QXP_VS_TEXT *output, DWORD mask)
{
    static const char components[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;

    if (mask == 15) return;
    qxp_vs_append_char(output, '.');
    for (index = 0; index < 4; ++index) {
        if (mask & (1UL << index))
            qxp_vs_append_char(output, components[index]);
    }
}

static BOOL qxp_vs_emit_fail(QXP_VS_EMIT *emit, DWORD code)
{
    return qxp_vs_fail(emit->error, code, emit->instruction_index,
                       emit->token_offset, 0);
}

static BOOL qxp_vs_source_valid(const QXP_SHADER_OPERAND *operand)
{
    DWORD limit;
    DWORD index;

    if (!operand || operand->role != QXP_SHADER_OPERAND_SOURCE ||
        (operand->source_modifier != QXP_SHADER_SRCMOD_NONE &&
         operand->source_modifier != QXP_SHADER_SRCMOD_NEGATE))
        return FALSE;
    limit = 0;
    if (operand->register_file == QXP_SHADER_REG_TEMP) limit = 12;
    else if (operand->register_file == QXP_SHADER_REG_INPUT) limit = 16;
    else if (operand->register_file == QXP_SHADER_REG_CONST) limit = 256;
    if (!limit || (DWORD)operand->register_number >= limit) return FALSE;
    for (index = 0; index < 4; ++index) {
        if (operand->swizzle[index] >= 4) return FALSE;
    }
    if (operand->relative.present) {
        if (operand->register_file != QXP_SHADER_REG_CONST) return FALSE;
        if (operand->relative.register_file != QXP_SHADER_REG_ADDRESS &&
            operand->relative.register_file != QXP_SHADER_REG_LOOP)
            return FALSE;
        if (operand->relative.register_number != 0) return FALSE;
        if (operand->relative.swizzle[0] >= 4) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_append_register(QXP_VS_EMIT *emit,
                                   const QXP_SHADER_OPERAND *operand)
{
    QXP_VS_TEXT *output;
    DWORD file;
    DWORD number;

    output = &emit->output;
    file = operand->register_file;
    number = operand->register_number;
    if (file == QXP_SHADER_REG_TEMP && number < 12) {
        qxp_vs_append(output, "qxpVsR");
        qxp_vs_append_uint(output, number);
    } else if (file == QXP_SHADER_REG_INPUT && number < 16) {
        qxp_vs_append(output, "qxpVsInput");
        qxp_vs_append_uint(output, number);
    } else if (file == QXP_SHADER_REG_CONST && number < 256) {
        if (operand->relative.present) {
            qxp_vs_append(output, "qxpVsRelFloatConst(");
            qxp_vs_append_uint(output, number);
            if (operand->relative.register_file == QXP_SHADER_REG_ADDRESS) {
                qxp_vs_append(output, " + qxpVsA0.");
                qxp_vs_append_char(output,
                    "xyzw"[operand->relative.swizzle[0] & 3]);
            } else {
                qxp_vs_append(output, " + qxpVsAL");
            }
            qxp_vs_append_char(output, ')');
        } else {
            qxp_vs_append(output, "qxpVsFloatConst[");
            qxp_vs_append_uint(output, number);
            qxp_vs_append_char(output, ']');
        }
    } else {
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    }
    return output->valid;
}

static BOOL qxp_vs_append_source(QXP_VS_EMIT *emit,
                                 const QXP_SHADER_OPERAND *operand)
{
    static const char components[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;
    BOOL identity;

    if (!qxp_vs_source_valid(operand))
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    if (operand->source_modifier == QXP_SHADER_SRCMOD_NEGATE)
        qxp_vs_append(&emit->output, "-(");
    if (!qxp_vs_append_register(emit, operand)) return FALSE;
    identity = TRUE;
    for (index = 0; index < 4; ++index) {
        if (operand->swizzle[index] != index) identity = FALSE;
    }
    if (!identity) {
        qxp_vs_append_char(&emit->output, '.');
        for (index = 0; index < 4; ++index)
            qxp_vs_append_char(&emit->output,
                               components[operand->swizzle[index]]);
    }
    if (operand->source_modifier == QXP_SHADER_SRCMOD_NEGATE)
        qxp_vs_append_char(&emit->output, ')');
    return emit->output.valid;
}

static BOOL qxp_vs_append_source_offset(QXP_VS_EMIT *emit,
                                        const QXP_SHADER_OPERAND *operand,
                                        DWORD register_offset)
{
    QXP_SHADER_OPERAND adjusted;

    if (!qxp_vs_source_valid(operand))
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    if (operand->relative.present) {
        if (operand->register_file != QXP_SHADER_REG_CONST ||
            operand->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            !qxp_vs_swizzle_is_identity(operand))
            return qxp_vs_emit_fail(
                emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
        qxp_vs_append(&emit->output, "qxpVsRelFloatConst(");
        qxp_vs_append_uint(&emit->output, operand->register_number);
        qxp_vs_append(&emit->output, " + ");
        qxp_vs_append_uint(&emit->output, register_offset);
        if (operand->relative.register_file == QXP_SHADER_REG_ADDRESS) {
            qxp_vs_append(&emit->output, " + qxpVsA0.");
            qxp_vs_append_char(
                &emit->output,
                "xyzw"[operand->relative.swizzle[0] & 3]);
        } else if (operand->relative.register_file ==
                   QXP_SHADER_REG_LOOP) {
            qxp_vs_append(&emit->output, " + qxpVsAL");
        } else {
            return qxp_vs_emit_fail(
                emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
        }
        qxp_vs_append_char(&emit->output, ')');
        return emit->output.valid;
    }
    if ((DWORD)operand->register_number + register_offset > 0xffffUL)
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    adjusted = *operand;
    adjusted.register_number =
        (WORD)((DWORD)adjusted.register_number + register_offset);
    return qxp_vs_append_source(emit, &adjusted);
}

static BOOL qxp_vs_append_destination_register(
    QXP_VS_EMIT *emit, const QXP_SHADER_OPERAND *destination)
{
    QXP_VS_TEXT *output;
    DWORD file;
    DWORD number;

    output = &emit->output;
    file = destination->register_file;
    number = destination->register_number;
    if (file == QXP_SHADER_REG_TEMP && number < 12) {
        qxp_vs_append(output, "qxpVsR");
        qxp_vs_append_uint(output, number);
    } else if (file == QXP_SHADER_REG_RASTOUT &&
               number == QXP_VS_RASTOUT_POSITION) {
        qxp_vs_append(output, "qxpVsOutPosition");
    } else if (file == QXP_SHADER_REG_RASTOUT &&
               number == QXP_VS_RASTOUT_FOG) {
        qxp_vs_append(output, "qxpVsOutFog");
    } else if (file == QXP_SHADER_REG_RASTOUT &&
               number == QXP_VS_RASTOUT_POINT_SIZE) {
        qxp_vs_append(output, "qxpVsOutPointSize");
    } else if (file == QXP_SHADER_REG_ATTROUT && number < 2) {
        qxp_vs_append(output, "qxpVsOutColor");
        qxp_vs_append_uint(output, number);
    } else if (file == QXP_SHADER_REG_TEXCRDOUT && number < 8) {
        qxp_vs_append(output, "qxpVsOutTexcoord");
        qxp_vs_append_uint(output, number);
    } else {
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    }
    return output->valid;
}

static BOOL qxp_vs_emit_store(QXP_VS_EMIT *emit,
                              const QXP_SHADER_OPERAND *destination)
{
    if (!destination ||
        destination->role != QXP_SHADER_OPERAND_DESTINATION ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0 ||
        destination->relative.present || !destination->write_mask ||
        destination->write_mask > 15)
        return qxp_vs_emit_fail(emit, QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER);
    qxp_vs_append(&emit->output, "  ");
    if (!qxp_vs_append_destination_register(emit, destination)) return FALSE;
    qxp_vs_append_component_mask(&emit->output, destination->write_mask);
    qxp_vs_append(&emit->output, " = qxpValue");
    qxp_vs_append_component_mask(&emit->output, destination->write_mask);
    qxp_vs_append(&emit->output, ";\n");
    return emit->output.valid;
}

static BOOL qxp_vs_emit_arguments(QXP_VS_EMIT *emit,
                                  const QXP_SHADER_INSTRUCTION *instruction,
                                  DWORD count)
{
    DWORD index;

    for (index = 0; index < count; ++index) {
        qxp_vs_append(&emit->output, "  qxpArg");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, " = ");
        if (!qxp_vs_append_source(emit, &instruction->sources[index]))
            return FALSE;
        qxp_vs_append(&emit->output, ";\n");
    }
    return emit->output.valid;
}

static BOOL qxp_vs_emit_matrix(QXP_VS_EMIT *emit,
                               const QXP_SHADER_INSTRUCTION *instruction,
                               DWORD rows, DWORD columns)
{
    DWORD row;

    qxp_vs_append(&emit->output, "  qxpArg0 = ");
    if (!qxp_vs_append_source(emit, &instruction->sources[0])) return FALSE;
    qxp_vs_append(&emit->output, ";\n  qxpValue = vec4(0.0);\n");
    for (row = 0; row < rows; ++row) {
        qxp_vs_append(&emit->output, "  qxpArg1 = ");
        if (!qxp_vs_append_source_offset(emit, &instruction->sources[1],
                                         row)) return FALSE;
        qxp_vs_append(&emit->output, ";\n  qxpValue.");
        qxp_vs_append_char(&emit->output, "xyzw"[row]);
        qxp_vs_append(&emit->output, columns == 4 ?
                      " = dot(qxpArg0, qxpArg1);\n" :
                      " = dot(qxpArg0.xyz, qxpArg1.xyz);\n");
    }
    return qxp_vs_emit_store(emit, &instruction->destinations[0]);
}

static BOOL qxp_vs_emit_operation(QXP_VS_EMIT *emit,
                                  const QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD opcode;

    opcode = instruction->opcode;
    if (opcode == QXP_SHADER_OP_NOP) {
        qxp_vs_append(&emit->output, "  /* nop */\n");
        return emit->output.valid;
    }
    if (qxp_vs_is_address_write(instruction)) {
        qxp_vs_append(&emit->output, "  qxpValue = ");
        if (!qxp_vs_append_source(emit, &instruction->sources[0]))
            return FALSE;
        qxp_vs_append(&emit->output, ";\n  qxpVsA0");
        qxp_vs_append_component_mask(&emit->output,
                                     instruction->destinations[0].write_mask);
        qxp_vs_append(&emit->output, " = qxpVsRound(qxpValue)");
        qxp_vs_append_component_mask(&emit->output,
                                     instruction->destinations[0].write_mask);
        qxp_vs_append(&emit->output, ";\n");
        return emit->output.valid;
    }
    if (opcode == QXP_SHADER_OP_M4X4)
        return qxp_vs_emit_matrix(emit, instruction, 4, 4);
    if (opcode == QXP_SHADER_OP_M4X3)
        return qxp_vs_emit_matrix(emit, instruction, 3, 4);
    if (opcode == QXP_SHADER_OP_M3X4)
        return qxp_vs_emit_matrix(emit, instruction, 4, 3);
    if (opcode == QXP_SHADER_OP_M3X3)
        return qxp_vs_emit_matrix(emit, instruction, 3, 3);
    if (opcode == QXP_SHADER_OP_M3X2)
        return qxp_vs_emit_matrix(emit, instruction, 2, 3);
    if (opcode == QXP_SHADER_OP_SGN) {
        qxp_vs_append(&emit->output, "  qxpArg0 = ");
        if (!qxp_vs_append_source(emit, &instruction->sources[0]))
            return FALSE;
        qxp_vs_append(&emit->output, ";\n  qxpValue = qxpVsSign(qxpArg0);\n");
        /* Scratch temporaries are destroyed; zero them deterministically
         * before the destination store so a destination that aliases a
         * scratch register still receives the result. */
        qxp_vs_append(&emit->output, "  qxpVsR");
        qxp_vs_append_uint(&emit->output,
                           instruction->sources[1].register_number);
        qxp_vs_append(&emit->output, " = vec4(0.0);\n  qxpVsR");
        qxp_vs_append_uint(&emit->output,
                           instruction->sources[2].register_number);
        qxp_vs_append(&emit->output, " = vec4(0.0);\n");
        return qxp_vs_emit_store(emit, &instruction->destinations[0]);
    }
    if (opcode == QXP_SHADER_OP_SINCOS) {
        qxp_vs_append(&emit->output, "  qxpArg0 = ");
        if (!qxp_vs_append_source(emit, &instruction->sources[0]))
            return FALSE;
        qxp_vs_append(&emit->output,
            ";\n  qxpValue = vec4(cos(qxpArg0.x), sin(qxpArg0.x), 0.0, 0.0);\n");
        return qxp_vs_emit_store(emit, &instruction->destinations[0]);
    }

    /* Remaining opcodes evaluate qxpArg0..N then produce qxpValue. */
    if (opcode == QXP_SHADER_OP_MAD || opcode == QXP_SHADER_OP_LRP) {
        if (!qxp_vs_emit_arguments(emit, instruction, 3)) return FALSE;
    } else if (opcode == QXP_SHADER_OP_ADD || opcode == QXP_SHADER_OP_SUB ||
               opcode == QXP_SHADER_OP_MUL || opcode == QXP_SHADER_OP_DP3 ||
               opcode == QXP_SHADER_OP_DP4 || opcode == QXP_SHADER_OP_MIN ||
               opcode == QXP_SHADER_OP_MAX || opcode == QXP_SHADER_OP_SLT ||
               opcode == QXP_SHADER_OP_SGE || opcode == QXP_SHADER_OP_DST ||
               opcode == QXP_SHADER_OP_POW || opcode == QXP_SHADER_OP_CRS) {
        if (!qxp_vs_emit_arguments(emit, instruction, 2)) return FALSE;
    } else {
        if (!qxp_vs_emit_arguments(emit, instruction, 1)) return FALSE;
    }

    if (opcode == QXP_SHADER_OP_MOV)
        qxp_vs_append(&emit->output, "  qxpValue = qxpArg0;\n");
    else if (opcode == QXP_SHADER_OP_ADD)
        qxp_vs_append(&emit->output, "  qxpValue = qxpArg0 + qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_SUB)
        qxp_vs_append(&emit->output, "  qxpValue = qxpArg0 - qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_MUL)
        qxp_vs_append(&emit->output, "  qxpValue = qxpArg0 * qxpArg1;\n");
    else if (opcode == QXP_SHADER_OP_MAD)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpArg0*qxpArg1 + qxpArg2;\n");
    else if (opcode == QXP_SHADER_OP_DP3)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(dot(qxpArg0.xyz, qxpArg1.xyz));\n");
    else if (opcode == QXP_SHADER_OP_DP4)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(dot(qxpArg0, qxpArg1));\n");
    else if (opcode == QXP_SHADER_OP_MIN)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsMin(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_MAX)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsMax(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_SLT)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsSlt(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_SGE)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsSge(qxpArg0, qxpArg1);\n");
    else if (opcode == QXP_SHADER_OP_DST)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(1.0, qxpArg0.y*qxpArg1.y, qxpArg0.z, qxpArg1.w);\n");
    else if (opcode == QXP_SHADER_OP_LRP)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpArg0*(qxpArg1 - qxpArg2) + qxpArg2;\n");
    else if (opcode == QXP_SHADER_OP_RCP)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(qxpVsRcp(qxpArg0.x));\n");
    else if (opcode == QXP_SHADER_OP_RSQ)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(qxpVsRsq(qxpArg0.x));\n");
    else if (opcode == QXP_SHADER_OP_EXP)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(exp2(qxpArg0.x));\n");
    else if (opcode == QXP_SHADER_OP_LOG)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(qxpVsLog(qxpArg0.x));\n");
    else if (opcode == QXP_SHADER_OP_EXPP)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsExpp(qxpArg0.x);\n");
    else if (opcode == QXP_SHADER_OP_LOGP)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsLogp(qxpArg0.x);\n");
    else if (opcode == QXP_SHADER_OP_LIT)
        qxp_vs_append(&emit->output,
                      "  qxpValue = qxpVsLit(qxpArg0);\n");
    else if (opcode == QXP_SHADER_OP_FRC)
        qxp_vs_append(&emit->output, "  qxpValue = fract(qxpArg0);\n");
    else if (opcode == QXP_SHADER_OP_ABS)
        qxp_vs_append(&emit->output, "  qxpValue = abs(qxpArg0);\n");
    else if (opcode == QXP_SHADER_OP_POW)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(pow(abs(qxpArg0.x), qxpArg1.x));\n");
    else if (opcode == QXP_SHADER_OP_CRS)
        qxp_vs_append(&emit->output,
                      "  qxpValue = vec4(cross(qxpArg0.xyz, qxpArg1.xyz), 0.0);\n");
    else if (opcode == QXP_SHADER_OP_NRM)
        qxp_vs_append(&emit->output,
                      "  qxpScalar = dot(qxpArg0.xyz, qxpArg0.xyz);\n"
                      "  qxpValue = qxpArg0 * ((qxpScalar != 0.0) ? inversesqrt(qxpScalar) : 3.402823466e+38);\n");
    else
        return qxp_vs_emit_fail(emit,
                                QXP_VERTEX_SHADER_GLSL_ERROR_UNSUPPORTED_FORM);
    return qxp_vs_emit_store(emit, &instruction->destinations[0]);
}

static BOOL qxp_vs_emit_condition(QXP_VS_EMIT *emit,
                                  const QXP_SHADER_OPERAND *condition)
{
    if (condition->register_file != QXP_SHADER_REG_CONSTBOOL ||
        (DWORD)condition->register_number >= 16 ||
        (condition->source_modifier != QXP_SHADER_SRCMOD_NONE &&
         condition->source_modifier != QXP_SHADER_SRCMOD_NOT))
        return qxp_vs_emit_fail(emit,
                                QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW);
    if (condition->source_modifier == QXP_SHADER_SRCMOD_NOT)
        qxp_vs_append(&emit->output, "!");
    qxp_vs_append(&emit->output, "qxpVsBoolConst[");
    qxp_vs_append_uint(&emit->output, condition->register_number);
    qxp_vs_append_char(&emit->output, ']');
    return emit->output.valid;
}

static BOOL qxp_vs_emit_int_count(QXP_VS_EMIT *emit,
                                  const QXP_SHADER_OPERAND *counter,
                                  const char *component, const char *low,
                                  const char *high)
{
    if (counter->register_file != QXP_SHADER_REG_CONSTINT ||
        (DWORD)counter->register_number >= 16)
        return qxp_vs_emit_fail(emit,
                                QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW);
    qxp_vs_append(&emit->output, "int(clamp(float(qxpVsIntConst[");
    qxp_vs_append_uint(&emit->output, counter->register_number);
    qxp_vs_append(&emit->output, "].");
    qxp_vs_append(&emit->output, component);
    qxp_vs_append(&emit->output, "), ");
    qxp_vs_append(&emit->output, low);
    qxp_vs_append(&emit->output, ", ");
    qxp_vs_append(&emit->output, high);
    qxp_vs_append(&emit->output, "))");
    return emit->output.valid;
}

static BOOL qxp_vs_emit_range(QXP_VS_EMIT *emit, const QXP_SHADER_IR *ir,
                              DWORD start, DWORD end, BOOL in_subroutine)
{
    DWORD index;

    for (index = start; index < end; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &ir->instructions[index];
        opcode = instruction->opcode;
        emit->instruction_index = index;
        emit->token_offset = instruction->token_offset;
        if (opcode == QXP_SHADER_OP_DCL || opcode == QXP_SHADER_OP_DEF ||
            opcode == QXP_SHADER_OP_DEFI || opcode == QXP_SHADER_OP_DEFB)
            continue;
        if (opcode == QXP_SHADER_OP_RET) {
            if (in_subroutine)
                qxp_vs_append(&emit->output, "  return;\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_LABEL)
            return qxp_vs_emit_fail(emit,
                                    QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW);
        if (opcode == QXP_SHADER_OP_IF) {
            qxp_vs_append(&emit->output, "  if (");
            if (!qxp_vs_emit_condition(emit, &instruction->sources[0]))
                return FALSE;
            qxp_vs_append(&emit->output, ") {\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_ELSE) {
            qxp_vs_append(&emit->output, "  } else {\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_ENDIF) {
            qxp_vs_append(&emit->output, "  }\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_REP) {
            qxp_vs_append(&emit->output, "  {\n    int qxpVsRepCount");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = ");
            if (!qxp_vs_emit_int_count(emit, &instruction->sources[0],
                                       "x", "0.0", "255.0")) return FALSE;
            qxp_vs_append(&emit->output, ";\n    for (int qxpVsRepIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = 0; qxpVsRepIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " < qxpVsRepCount");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, "; ++qxpVsRepIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, ") {\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_ENDREP) {
            qxp_vs_append(&emit->output, "    }\n  }\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_LOOP) {
            qxp_vs_append(&emit->output, "  {\n    int qxpVsLoopCount");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = ");
            if (!qxp_vs_emit_int_count(emit, &instruction->sources[1],
                                       "x", "0.0", "255.0")) return FALSE;
            qxp_vs_append(&emit->output, ";\n    int qxpVsLoopInit");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = ");
            if (!qxp_vs_emit_int_count(emit, &instruction->sources[1],
                                       "y", "0.0", "255.0")) return FALSE;
            qxp_vs_append(&emit->output, ";\n    int qxpVsLoopStep");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = ");
            if (!qxp_vs_emit_int_count(emit, &instruction->sources[1],
                                       "z", "-128.0", "127.0")) return FALSE;
            qxp_vs_append(&emit->output, ";\n    qxpVsAL = qxpVsLoopInit");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, ";\n    for (int qxpVsLoopIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " = 0; qxpVsLoopIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, " < qxpVsLoopCount");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, "; ++qxpVsLoopIter");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, ") {\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_ENDLOOP) {
            qxp_vs_append(&emit->output, "    qxpVsAL += qxpVsLoopStep");
            qxp_vs_append_uint(&emit->output, instruction->control_match);
            qxp_vs_append(&emit->output, ";\n    }\n  }\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_CALL) {
            if (instruction->sources[0].register_file !=
                    QXP_SHADER_REG_LABEL ||
                (DWORD)instruction->sources[0].register_number >=
                    QXP_SHADER_MAX_LABELS)
                return qxp_vs_emit_fail(
                    emit, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW);
            qxp_vs_append(&emit->output, "  qxpVsSubroutineL");
            qxp_vs_append_uint(&emit->output,
                               instruction->sources[0].register_number);
            qxp_vs_append(&emit->output, "();\n");
            continue;
        }
        if (opcode == QXP_SHADER_OP_CALLNZ) {
            if (instruction->sources[0].register_file !=
                    QXP_SHADER_REG_LABEL ||
                (DWORD)instruction->sources[0].register_number >=
                    QXP_SHADER_MAX_LABELS)
                return qxp_vs_emit_fail(
                    emit, QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW);
            qxp_vs_append(&emit->output, "  if (");
            if (!qxp_vs_emit_condition(emit, &instruction->sources[1]))
                return FALSE;
            qxp_vs_append(&emit->output, ") qxpVsSubroutineL");
            qxp_vs_append_uint(&emit->output,
                               instruction->sources[0].register_number);
            qxp_vs_append(&emit->output, "();\n");
            continue;
        }
        if (!qxp_vs_emit_operation(emit, instruction)) return FALSE;
        if (!emit->output.valid)
            return qxp_vs_emit_fail(
                emit, QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY);
    }
    return emit->output.valid;
}

static void qxp_vs_emit_scratch_declarations(QXP_VS_EMIT *emit)
{
    qxp_vs_append(&emit->output,
        "  vec4 qxpArg0 = vec4(0.0);\n"
        "  vec4 qxpArg1 = vec4(0.0);\n"
        "  vec4 qxpArg2 = vec4(0.0);\n"
        "  vec4 qxpValue = vec4(0.0);\n"
        "  float qxpScalar = 0.0;\n");
}

static void qxp_vs_emit_interface(
    QXP_VS_EMIT *emit, const QXP_SHADER_IR *ir,
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata)
{
    DWORD index;

    qxp_vs_append(&emit->output, "#version 120\n");
    qxp_vs_append(&emit->output, "uniform vec4 qxpVsFloatConst[256];\n");
    qxp_vs_append(&emit->output, "uniform vec4 qxpVsClipControl;\n");
    if (metadata->uses_integer_constants)
        qxp_vs_append(&emit->output, "uniform ivec4 qxpVsIntConst[16];\n");
    if (metadata->uses_boolean_constants)
        qxp_vs_append(&emit->output, "uniform bool qxpVsBoolConst[16];\n");
    for (index = 0; index < 16; ++index) {
        if (!(metadata->input_mask & (1UL << index))) continue;
        qxp_vs_append(&emit->output, "attribute vec4 qxpVsInput");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, ";\n");
    }
    /* D3D leaves a pixel-shader colour input undefined when the vertex shader
     * never writes it, and hardware supplies a neutral value rather than
     * failing the draw.  Publish both colour varyings unconditionally so the
     * pair always links with a defined value: white for diffuse (modulate is
     * a no-op) and black for specular (add is a no-op). */
    for (index = 0; index < 2; ++index) {
        if (metadata->output_centroid_mask & (1UL << index))
            qxp_vs_append(&emit->output, "centroid ");
        qxp_vs_append(&emit->output, "varying vec4 qxpPsInV");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, ";\n");
    }
    for (index = 0; index < 8; ++index) {
        if (!(metadata->texcoord_output_mask & (1UL << index))) continue;
        if (metadata->output_centroid_mask & (1UL << (index + 2)))
            qxp_vs_append(&emit->output, "centroid ");
        qxp_vs_append(&emit->output, "varying vec4 qxpPsInT");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, ";\n");
    }
    for (index = 0; index < metadata->required_temp_count; ++index) {
        qxp_vs_append(&emit->output, "vec4 qxpVsR");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, " = vec4(0.0);\n");
    }
    if (metadata->uses_address_register)
        qxp_vs_append(&emit->output, "ivec4 qxpVsA0 = ivec4(0);\n");
    if (metadata->uses_loop_register)
        qxp_vs_append(&emit->output, "int qxpVsAL = 0;\n");
    if (metadata->writes_position)
        qxp_vs_append(&emit->output,
                      "vec4 qxpVsOutPosition = vec4(0.0);\n");
    if (metadata->writes_fog)
        qxp_vs_append(&emit->output, "vec4 qxpVsOutFog = vec4(0.0);\n");
    if (metadata->writes_point_size)
        qxp_vs_append(&emit->output,
                      "vec4 qxpVsOutPointSize = vec4(0.0);\n");
    for (index = 0; index < 2; ++index) {
        if (!(metadata->attribute_output_mask & (1UL << index))) continue;
        qxp_vs_append(&emit->output, "vec4 qxpVsOutColor");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, " = vec4(0.0);\n");
    }
    for (index = 0; index < 8; ++index) {
        if (!(metadata->texcoord_output_mask & (1UL << index))) continue;
        qxp_vs_append(&emit->output, "vec4 qxpVsOutTexcoord");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, " = vec4(0.0);\n");
    }

    /* Each helper is emitted with a separate append so that no single string
     * literal exceeds the ISO C90 509-character guarantee. */
    qxp_vs_append(&emit->output,
        "vec4 qxpVsMin(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsMax(vec4 a, vec4 b) { return vec4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsSlt(vec4 a, vec4 b) { return vec4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsSge(vec4 a, vec4 b) { return vec4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }\n");
    qxp_vs_append(&emit->output,
        "float qxpVsSign1(float v) { if (v < 0.0) return -1.0; if (v == 0.0) return 0.0; return 1.0; }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsSign(vec4 v) { return vec4(qxpVsSign1(v.x), qxpVsSign1(v.y), qxpVsSign1(v.z), qxpVsSign1(v.w)); }\n");
    qxp_vs_append(&emit->output,
        "float qxpVsRcp(float v) { if (v == 0.0) return 3.402823466e+38; return 1.0/v; }\n");
    qxp_vs_append(&emit->output,
        "float qxpVsRsq(float v) { float t = abs(v); if (t == 0.0) return 3.402823466e+38; return inversesqrt(t); }\n");
    qxp_vs_append(&emit->output,
        "float qxpVsLog(float v) { float t = abs(v); if (t == 0.0) return -3.402823466e+38; return log2(t); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsExpp(float v) { return vec4(exp2(v)); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsLogp(float v) { return vec4(qxpVsLog(v)); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsLit(vec4 a) { float x = max(a.x, 0.0); float w = clamp(a.w, -127.99609375, 127.99609375); float z = 0.0; if (a.x > 0.0 && a.y > 0.0) z = pow(a.y, w); return vec4(1.0, x, z, 1.0); }\n");
    qxp_vs_append(&emit->output,
        "float qxpVsRound1(float v) { float t = clamp(v, -65536.0, 65536.0); return floor(t + 0.5); }\n");
    qxp_vs_append(&emit->output,
        "ivec4 qxpVsRound(vec4 v) { return ivec4(qxpVsRound1(v.x), qxpVsRound1(v.y), qxpVsRound1(v.z), qxpVsRound1(v.w)); }\n");
    qxp_vs_append(&emit->output,
        "vec4 qxpVsRelFloatConst(int i) { if (i < 0 || i > 255) return vec4(0.0); return qxpVsFloatConst[i]; }\n");

    /* Subroutine prototypes precede main so forward calls resolve. */
    for (index = 0; index < ir->instruction_count; ++index) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[index];
        if (instruction->opcode != QXP_SHADER_OP_LABEL) continue;
        qxp_vs_append(&emit->output, "void qxpVsSubroutineL");
        qxp_vs_append_uint(&emit->output,
                           instruction->sources[0].register_number);
        qxp_vs_append(&emit->output, "(void);\n");
    }
}

static void qxp_vs_emit_commit(
    QXP_VS_EMIT *emit, const QXP_VERTEX_SHADER_GLSL_METADATA *metadata)
{
    DWORD index;

    if (metadata->writes_position) {
        qxp_vs_append(&emit->output,
                      "  gl_ClipVertex = qxpVsOutPosition;\n");
        qxp_vs_append(&emit->output,
                      "  gl_Position.x = qxpVsOutPosition.x + qxpVsOutPosition.w*qxpVsClipControl.z;\n");
        qxp_vs_append(&emit->output,
                      "  gl_Position.y = qxpVsOutPosition.y*qxpVsClipControl.x + qxpVsOutPosition.w*qxpVsClipControl.w;\n");
        qxp_vs_append(&emit->output,
                      "  gl_Position.z = 2.0*qxpVsOutPosition.z - qxpVsOutPosition.w;\n");
        qxp_vs_append(&emit->output,
                      "  gl_Position.w = qxpVsOutPosition.w;\n");
    }
    for (index = 0; index < 2; ++index) {
        qxp_vs_append(&emit->output, "  qxpPsInV");
        qxp_vs_append_uint(&emit->output, index);
        if (metadata->attribute_output_mask & (1UL << index)) {
            qxp_vs_append(&emit->output, " = qxpVsOutColor");
            qxp_vs_append_uint(&emit->output, index);
            qxp_vs_append(&emit->output, ";\n");
        } else if (index) {
            qxp_vs_append(&emit->output, " = vec4(0.0, 0.0, 0.0, 0.0);\n");
        } else {
            qxp_vs_append(&emit->output, " = vec4(1.0, 1.0, 1.0, 1.0);\n");
        }
    }
    for (index = 0; index < 8; ++index) {
        if (!(metadata->texcoord_output_mask & (1UL << index))) continue;
        qxp_vs_append(&emit->output, "  qxpPsInT");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, " = qxpVsOutTexcoord");
        qxp_vs_append_uint(&emit->output, index);
        qxp_vs_append(&emit->output, ";\n");
    }
    if (metadata->writes_fog) {
        qxp_vs_append(&emit->output,
                      "  gl_FogFragCoord = (1.0 - qxpVsOutFog.x) * (1.0 - qxpVsClipControl.y) + (qxpVsOutPosition.z / qxpVsOutPosition.w) * qxpVsClipControl.y;\n");
    } else {
        qxp_vs_append(&emit->output,
                      "  gl_FogFragCoord = qxpVsOutPosition.z / qxpVsOutPosition.w;\n");
    }
    if (metadata->writes_point_size)
        qxp_vs_append(&emit->output,
                      "  gl_PointSize = qxpVsOutPointSize.x;\n");
}

static BOOL qxp_vs_emit_pass(const QXP_SHADER_IR *ir,
                             const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
                             char *source, DWORD source_capacity,
                             DWORD *source_length,
                             QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    QXP_VS_EMIT emit;
    DWORD main_end;
    DWORD index;

    qxp_vs_zero(&emit, (DWORD)sizeof(emit));
    emit.output.text = source;
    emit.output.capacity = source_capacity;
    emit.output.valid = TRUE;
    emit.error = error;
    if (source) source[0] = 0;

    qxp_vs_emit_interface(&emit, ir, metadata);

    main_end = ir->instruction_count;
    for (index = 0; index < ir->instruction_count; ++index) {
        if (ir->instructions[index].opcode == QXP_SHADER_OP_LABEL) {
            main_end = index;
            break;
        }
    }

    qxp_vs_append(&emit.output, "void main(void) {\n");
    qxp_vs_emit_scratch_declarations(&emit);
    if (!qxp_vs_emit_range(&emit, ir, 0, main_end, FALSE)) return FALSE;
    qxp_vs_emit_commit(&emit, metadata);
    qxp_vs_append(&emit.output, "}\n");

    index = main_end;
    while (index < ir->instruction_count) {
        const QXP_SHADER_INSTRUCTION *label_instruction;
        DWORD end;

        label_instruction = &ir->instructions[index];
        if (label_instruction->opcode != QXP_SHADER_OP_LABEL)
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, label_instruction->token_offset, 0);
        end = index + 1;
        while (end < ir->instruction_count &&
               ir->instructions[end].opcode != QXP_SHADER_OP_RET)
            ++end;
        if (end >= ir->instruction_count)
            return qxp_vs_fail(error,
                               QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW,
                               index, label_instruction->token_offset, 0);
        qxp_vs_append(&emit.output, "void qxpVsSubroutineL");
        qxp_vs_append_uint(&emit.output,
                           label_instruction->sources[0].register_number);
        qxp_vs_append(&emit.output, "(void) {\n");
        qxp_vs_emit_scratch_declarations(&emit);
        if (!qxp_vs_emit_range(&emit, ir, index + 1, end, TRUE))
            return FALSE;
        qxp_vs_append(&emit.output, "  return;\n}\n");
        index = end + 1;
    }

    if (!emit.output.valid)
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    *source_length = emit.output.length;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

BOOL QxpEmitVertexShader20Glsl(
    const QXP_SHADER_IR *ir, DWORD output_centroid_mask,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error)
{
    DWORD required_bytes;
    DWORD emitted_bytes;

    if (source_bytes) *source_bytes = 0;
    if (metadata) qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
    if (error) qxp_vs_zero(error, (DWORD)sizeof(*error));
    if (source && source_capacity) source[0] = 0;
    if (!ir || !source_bytes || !metadata || !error ||
        (!source && source_capacity) || (source && !source_capacity))
        return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_NULL_ARGUMENT,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);

    /* The metadata is deliberately large (it embeds raw DEF DWORDs for every
     * constant slot).  Collect directly into the caller-owned storage rather
     * than copying it through a large automatic, and zero it on any later
     * failure so a partial result never escapes. */
    if (!qxp_vs_prepare(ir, metadata, error)) {
        qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
        return FALSE;
    }
    if ((output_centroid_mask & ~0x3ffUL) != 0 ||
        (output_centroid_mask &
         ~(metadata->attribute_output_mask |
           (metadata->texcoord_output_mask << 2))) != 0) {
        qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
        return qxp_vs_fail(
            error, QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION,
            QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE, 0);
    }
    metadata->output_centroid_mask = output_centroid_mask;

    required_bytes = 0;
    if (!qxp_vs_emit_pass(ir, metadata, NULL,
                          QXP_VERTEX_SHADER_GLSL_MAX_SOURCE + 1UL,
                          &required_bytes, error)) {
        qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
        if (source && source_capacity) source[0] = 0;
        return FALSE;
    }
    if (required_bytes > QXP_VERTEX_SHADER_GLSL_MAX_SOURCE) {
        qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
        return qxp_vs_fail(error,
                           QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE,
                           required_bytes + 1UL);
    }
    if (source && source_capacity <= required_bytes) {
        qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
        return qxp_vs_fail(error,
                           QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY,
                           QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE,
                           required_bytes + 1UL);
    }
    if (source) {
        emitted_bytes = 0;
        if (!qxp_vs_emit_pass(ir, metadata, source, source_capacity,
                              &emitted_bytes, error)) {
            source[0] = 0;
            qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
            return FALSE;
        }
        if (emitted_bytes != required_bytes) {
            source[0] = 0;
            qxp_vs_zero(metadata, (DWORD)sizeof(*metadata));
            return qxp_vs_fail(error, QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE,
                               QXP_SHADER_INDEX_NONE, QXP_SHADER_INDEX_NONE,
                               0);
        }
    }
    *source_bytes = required_bytes;
    qxp_vs_zero(error, (DWORD)sizeof(*error));
    return TRUE;
}
