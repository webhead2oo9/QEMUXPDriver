#include <stdio.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxp_shader_ir.h"

#define QXP_ARRAY_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_ANY_OFFSET     0xffffffffUL
#define QXP_CANARY_A       0x13579bdfUL
#define QXP_CANARY_B       0x2468ace0UL
#define QXP_PARAMETER      0x80000000UL

#define QXP_INST(op, length) \
    ((DWORD)(op) | ((DWORD)(length) << D3DSI_INSTLENGTH_SHIFT))
#define QXP_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_DST(type, number, mask, modifier) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     ((DWORD)(mask) << 16) | (DWORD)(modifier))
#define QXP_SRC_SWIZZLE(type, number, swizzle, modifier) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     (DWORD)(swizzle) | (DWORD)(modifier))
#define QXP_SRC(type, number) \
    QXP_SRC_SWIZZLE(type, number, D3DSP_NOSWIZZLE, D3DSPSM_NONE)
#define QXP_SRC_NEG(type, number) \
    QXP_SRC_SWIZZLE(type, number, D3DSP_NOSWIZZLE, D3DSPSM_NEG)
#define QXP_REPLICATE(component) \
    (((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 0)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 2)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 4)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 6)))
#define QXP_SCALAR(type, number, component) \
    QXP_SRC_SWIZZLE(type, number, QXP_REPLICATE(component), D3DSPSM_NONE)
#define QXP_RELATIVE(type, number, swizzle) \
    (QXP_SRC_SWIZZLE(type, number, swizzle, D3DSPSM_NONE) | \
     D3DSHADER_ADDRMODE_RELATIVE)
#define QXP_DCL_USAGE(usage, index) \
    (QXP_PARAMETER | ((DWORD)(usage) << D3DSP_DCL_USAGE_SHIFT) | \
     ((DWORD)(index) << D3DSP_DCL_USAGEINDEX_SHIFT))
#define QXP_DCL_SAMPLER(type) (QXP_PARAMETER | (DWORD)(type))

typedef struct qxp_guarded_ir {
    DWORD before[8];
    QXP_SHADER_IR value;
    DWORD after[8];
} QXP_GUARDED_IR;

typedef struct qxp_guarded_error {
    DWORD before[8];
    QXP_SHADER_DECODE_ERROR value;
    DWORD after[8];
} QXP_GUARDED_ERROR;

static QXP_GUARDED_IR qxp_ir_a;
static QXP_GUARDED_IR qxp_ir_b;
static QXP_GUARDED_ERROR qxp_error_a;
static QXP_GUARDED_ERROR qxp_error_b;
static QXP_SHADER_IR qxp_zero_ir;
static DWORD qxp_work_tokens[QXP_SHADER_MAX_TOKENS + 1];
static DWORD qxp_mutation_tokens[512];
static int qxp_failures;

static const DWORD qxp_minimal_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_MOV, 2),
    QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
    QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_minimal_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
    QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
    QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
    QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
    QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_texture_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_PARTIALPRECISION),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_SAMPLER(D3DSTT_2D),
        QXP_DST(D3DSPR_SAMPLER, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_TEX, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_SAMPLER, 0),
    QXP_INST(D3DSIO_TEX, 3) | D3DSI_TEXLD_PROJECT,
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_PARTIALPRECISION),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_SAMPLER, 0),
    QXP_INST(D3DSIO_TEX, 3) | D3DSI_TEXLD_BIAS,
        QXP_DST(D3DSPR_TEMP, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_SAMPLER, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 2),
    D3DSIO_END
};

static const DWORD qxp_arithmetic_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 0, 15, D3DSPDM_NONE),
        0x3f800000UL, 0x80000000UL, 0x7fc12345UL, 0x00000001UL,
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 1, 15, D3DSPDM_NONE),
        0xbf800000UL, 0x40000000UL, 0x40400000UL, 0x40800000UL,
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE), QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 1, 15,
                D3DSPDM_SATURATE | D3DSPDM_PARTIALPRECISION),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC_NEG(D3DSPR_CONST, 1),
    QXP_INST(D3DSIO_MAD, 4),
        QXP_DST(D3DSPR_TEMP, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_TEMP, 1),
        QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_POW, 3),
        QXP_DST(D3DSPR_TEMP, 3, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0), QXP_SCALAR(D3DSPR_CONST, 1, 1),
    QXP_INST(D3DSIO_SINCOS, 4),
        QXP_DST(D3DSPR_TEMP, 4, 3, D3DSPDM_PARTIALPRECISION),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 2),
    D3DSIO_END
};

static const DWORD qxp_setup_relative_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 1),
        QXP_DST(D3DSPR_INPUT, 1, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 4, 15, D3DSPDM_NONE),
        0x3f000000UL, 0xbf000000UL, 0x7f800000UL, 0xff800000UL,
    QXP_INST(D3DSIO_DEFI, 5),
        QXP_DST(D3DSPR_CONSTINT, 0, 15, D3DSPDM_NONE),
        3UL, 1UL, 0xffffffffUL, 0UL,
    QXP_INST(D3DSIO_DEFB, 2),
        QXP_DST(D3DSPR_CONSTBOOL, 0, 15, D3DSPDM_NONE), 0x12345678UL,
    QXP_INST(D3DSIO_MOVA, 2),
        QXP_DST(D3DSPR_ADDR, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 0),
    QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 2, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_control_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DEFI, 5),
        QXP_DST(D3DSPR_CONSTINT, 0, 15, D3DSPDM_NONE), 2UL, 0UL, 1UL, 0UL,
    QXP_INST(D3DSIO_DEFB, 2),
        QXP_DST(D3DSPR_CONSTBOOL, 0, 15, D3DSPDM_NONE), 1UL,
    QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_SRC_SWIZZLE(D3DSPR_CONSTBOOL, 0, D3DSP_NOSWIZZLE, D3DSPSM_NOT),
    QXP_INST(D3DSIO_IF, 1), QXP_SRC(D3DSPR_CONSTBOOL, 0),
    QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
    QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_CONST, 0),
    QXP_INST(D3DSIO_ENDREP, 0),
    QXP_INST(D3DSIO_ELSE, 0),
    QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
        QXP_SRC(D3DSPR_CONSTINT, 0),
    QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 3, D3DSP_NOSWIZZLE),
        QXP_SRC(D3DSPR_LOOP, 0),
    QXP_INST(D3DSIO_ENDLOOP, 0),
    QXP_INST(D3DSIO_ENDIF, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_RET, 0),
    QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_RET, 0),
    D3DSIO_END
};

static const DWORD qxp_partial_outputs_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 3, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 12, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEXCRDOUT, 0, 2, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEXCRDOUT, 0, 5, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_ATTROUT, 0, 9, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_FOG, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 0, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE, 15, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 0, 1),
    D3DSIO_END
};

static const DWORD qxp_comment_shader[] = {
    D3DPS_VERSION(2, 0),
    D3DSHADER_COMMENT(2), D3DSIO_END, 0x80000000UL,
    QXP_INST(D3DSIO_NOP, 0),
    D3DSHADER_COMMENT(0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_shader_decode: failed: %s\n", name);
}

static void qxp_prepare_ir(QXP_GUARDED_IR *guard)
{
    DWORD i;

    memset(guard, 0xcd, sizeof(*guard));
    for (i = 0; i < QXP_ARRAY_COUNT(guard->before); ++i) {
        guard->before[i] = QXP_CANARY_A + i;
        guard->after[i] = QXP_CANARY_B - i;
    }
}

static void qxp_prepare_error(QXP_GUARDED_ERROR *guard)
{
    DWORD i;

    memset(guard, 0xab, sizeof(*guard));
    for (i = 0; i < QXP_ARRAY_COUNT(guard->before); ++i) {
        guard->before[i] = QXP_CANARY_B + i;
        guard->after[i] = QXP_CANARY_A - i;
    }
}

static int qxp_ir_canaries_ok(const QXP_GUARDED_IR *guard)
{
    DWORD i;

    for (i = 0; i < QXP_ARRAY_COUNT(guard->before); ++i) {
        if (guard->before[i] != QXP_CANARY_A + i ||
            guard->after[i] != QXP_CANARY_B - i) return 0;
    }
    return 1;
}

static int qxp_error_canaries_ok(const QXP_GUARDED_ERROR *guard)
{
    DWORD i;

    for (i = 0; i < QXP_ARRAY_COUNT(guard->before); ++i) {
        if (guard->before[i] != QXP_CANARY_B + i ||
            guard->after[i] != QXP_CANARY_A - i) return 0;
    }
    return 1;
}

static DWORD qxp_find_block(const QXP_SHADER_IR *ir, DWORD instruction)
{
    DWORD i;

    for (i = 0; i < ir->block_count; ++i) {
        DWORD first;
        DWORD end;

        first = ir->blocks[i].first_instruction;
        end = first + ir->blocks[i].instruction_count;
        if (instruction >= first && instruction < end) return i;
    }
    return QXP_SHADER_INDEX_NONE;
}

static int qxp_block_has_target(const QXP_SHADER_IR *ir, DWORD block,
                                DWORD target_instruction)
{
    DWORD target_block;
    DWORD i;

    if (block >= ir->block_count) return 0;
    target_block = qxp_find_block(ir, target_instruction);
    if (target_block == QXP_SHADER_INDEX_NONE) return 0;
    for (i = 0; i < ir->blocks[block].successor_count; ++i) {
        if (ir->blocks[block].successors[i] == target_block) return 1;
    }
    return 0;
}

static int qxp_test_swizzle_is_ps20_baseline(
    const QXP_SHADER_OPERAND *operand)
{
    DWORD value;

    value = (DWORD)operand->swizzle[0] |
            ((DWORD)operand->swizzle[1] << 2) |
            ((DWORD)operand->swizzle[2] << 4) |
            ((DWORD)operand->swizzle[3] << 6);
    if (value == 0xe4UL || value == 0xc9UL || value == 0xd2UL ||
        value == 0x1bUL) return 1;
    return operand->swizzle[0] == operand->swizzle[1] &&
           operand->swizzle[0] == operand->swizzle[2] &&
           operand->swizzle[0] == operand->swizzle[3];
}

static DWORD qxp_test_matrix_rows(DWORD opcode)
{
    if (opcode == QXP_SHADER_OP_M4X4 ||
        opcode == QXP_SHADER_OP_M3X4) return 4;
    if (opcode == QXP_SHADER_OP_M4X3 ||
        opcode == QXP_SHADER_OP_M3X3) return 3;
    return 2;
}

static int qxp_test_is_matrix_opcode(DWORD opcode)
{
    return opcode == QXP_SHADER_OP_M4X4 ||
           opcode == QXP_SHADER_OP_M4X3 ||
           opcode == QXP_SHADER_OP_M3X4 ||
           opcode == QXP_SHADER_OP_M3X3 ||
           opcode == QXP_SHADER_OP_M3X2;
}

static DWORD qxp_test_extended_texture_slots(
    const QXP_SHADER_INSTRUCTION *instruction,
    const BYTE sampler_types[16])
{
    DWORD sampler_type;

    if (instruction->opcode == QXP_SHADER_OP_TEXKILL) return 2;
    if (instruction->control == QXP_SHADER_CONTROL_TEX_BIAS) return 6;
    sampler_type = sampler_types[instruction->sources[1].register_number];
    if (instruction->control == QXP_SHADER_CONTROL_TEX_PROJECT)
        return sampler_type == QXP_SHADER_SAMPLER_CUBE ? 4 : 3;
    return sampler_type == QXP_SHADER_SAMPLER_CUBE ? 4 : 1;
}

static int qxp_validate_ir_independently(const QXP_SHADER_IR *ir,
                                         DWORD expected_token_count)
{
    DWORD i;
    DWORD previous_end;
    DWORD arithmetic;
    DWORD texture;
    DWORD flow;
    DWORD total;
    DWORD covered;
    DWORD required_ps_temps;
    DWORD required_ps_slots;
    BYTE sampler_types[16];
    BOOL arbitrary_swizzle;
    BOOL no_texture_instruction_limit;

    if (!ir->valid || ir->token_count != expected_token_count ||
        (ir->stage != QXP_SHADER_STAGE_VERTEX &&
         ir->stage != QXP_SHADER_STAGE_PIXEL) ||
        ir->instruction_count > QXP_SHADER_MAX_INSTRUCTIONS ||
        ir->block_count > QXP_SHADER_MAX_BLOCKS) return 0;

    previous_end = 1;
    arithmetic = 0;
    texture = 0;
    flow = 0;
    total = 0;
    required_ps_temps = 0;
    required_ps_slots = 0;
    memset(sampler_types, 0, sizeof(sampler_types));
    arbitrary_swizzle = FALSE;
    no_texture_instruction_limit = FALSE;
    for (i = 0; i < ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD j;

        instruction = &ir->instructions[i];
        if (instruction->token_offset < previous_end ||
            !instruction->token_count ||
            instruction->token_offset + instruction->token_count >=
                ir->token_count ||
            instruction->destination_count > QXP_SHADER_MAX_DESTINATIONS ||
            instruction->source_count > QXP_SHADER_MAX_SOURCES ||
            instruction->immediate_count > 4 ||
            (instruction->control_match != QXP_SHADER_INDEX_NONE &&
             instruction->control_match >= ir->instruction_count) ||
            (instruction->control_else != QXP_SHADER_INDEX_NONE &&
             instruction->control_else >= ir->instruction_count) ||
            (instruction->label_target != QXP_SHADER_INDEX_NONE &&
             instruction->label_target >= ir->instruction_count))
            return 0;
        previous_end = instruction->token_offset + instruction->token_count;
        total += instruction->slot_count;
        if (instruction->slot_class == QXP_SHADER_SLOT_ARITHMETIC)
            arithmetic += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            texture += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_FLOW)
            flow += instruction->slot_count;
        if ((instruction->opcode == QXP_SHADER_OP_IF ||
             instruction->opcode == QXP_SHADER_OP_LOOP ||
             instruction->opcode == QXP_SHADER_OP_REP) &&
            (instruction->control_match == QXP_SHADER_INDEX_NONE ||
             ir->instructions[instruction->control_match].control_match != i))
            return 0;
        if (ir->stage == QXP_SHADER_STAGE_PIXEL) {
            if (instruction->declaration.present &&
                instruction->declaration.kind ==
                    QXP_SHADER_DECLARATION_SAMPLER &&
                instruction->destinations[0].register_number < 16)
                sampler_types[instruction->destinations[0].register_number] =
                    (BYTE)instruction->declaration.sampler_type;
            for (j = 0; j < instruction->destination_count; ++j) {
                const QXP_SHADER_OPERAND *operand;
                DWORD required;

                operand = &instruction->destinations[j];
                if (operand->register_file != QXP_SHADER_REG_TEMP) continue;
                required = (DWORD)operand->register_number + 1;
                if (required > required_ps_temps)
                    required_ps_temps = required;
            }
            for (j = 0; j < instruction->source_count; ++j) {
                const QXP_SHADER_OPERAND *operand;
                DWORD required;

                operand = &instruction->sources[j];
                if (!qxp_test_swizzle_is_ps20_baseline(operand))
                    arbitrary_swizzle = TRUE;
                if (operand->register_file != QXP_SHADER_REG_TEMP) continue;
                required = (DWORD)operand->register_number + 1;
                if (j == 1 && qxp_test_is_matrix_opcode(
                                  instruction->opcode))
                    required = (DWORD)operand->register_number +
                               qxp_test_matrix_rows(instruction->opcode);
                if (required > required_ps_temps)
                    required_ps_temps = required;
            }
        }
    }
    if (total != ir->slot_count || arithmetic != ir->arithmetic_slot_count ||
        texture != ir->texture_slot_count || flow != ir->flow_slot_count)
        return 0;
    no_texture_instruction_limit =
        ir->stage == QXP_SHADER_STAGE_PIXEL &&
        (arithmetic > 64 || texture > 32);
    for (i = 0; i < ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[i];
        if (no_texture_instruction_limit &&
            instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            required_ps_slots += qxp_test_extended_texture_slots(
                instruction, sampler_types);
        else
            required_ps_slots += instruction->slot_count;
    }
    if (ir->stage == QXP_SHADER_STAGE_VERTEX) {
        if (total > 256 || ir->required_ps_temp_count != 0 ||
            ir->required_ps20_caps != 0 ||
            ir->required_ps_instruction_slots != 0) return 0;
    } else {
        DWORD allowed_caps;
        BOOL has_arbitrary;
        BOOL has_unlimited_textures;

        allowed_caps = QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE |
                       QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT |
                       QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT;
        has_arbitrary = (ir->required_ps20_caps &
                         QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE) != 0;
        has_unlimited_textures = (ir->required_ps20_caps &
                          QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT) != 0;
        if (required_ps_temps > 32 ||
            ir->required_ps_temp_count != required_ps_temps ||
            ir->required_ps_instruction_slots != required_ps_slots ||
            (ir->required_ps20_caps & ~allowed_caps) != 0 ||
            has_arbitrary != arbitrary_swizzle ||
            has_unlimited_textures != no_texture_instruction_limit) return 0;
    }

    covered = 0;
    for (i = 0; i < ir->block_count; ++i) {
        const QXP_SHADER_BASIC_BLOCK *block;
        DWORD j;

        block = &ir->blocks[i];
        if (!block->instruction_count ||
            block->first_instruction != covered ||
            block->successor_count > QXP_SHADER_MAX_SUCCESSORS)
            return 0;
        covered += block->instruction_count;
        for (j = 0; j < block->successor_count; ++j) {
            if (block->successors[j] >= ir->block_count) return 0;
            if (j && block->successors[j] == block->successors[0]) return 0;
        }
    }
    if (covered != ir->instruction_count) return 0;
    if (!ir->instruction_count && ir->block_count) return 0;
    if (ir->instruction_count && !ir->block_count) return 0;
    return 1;
}

static int qxp_decode_ok(const DWORD *tokens, DWORD count, const char *name)
{
    BOOL result;
    int ok;

    qxp_prepare_ir(&qxp_ir_a);
    qxp_prepare_error(&qxp_error_a);
    result = QxpDecodeShader20(tokens, count, &qxp_ir_a.value,
                               &qxp_error_a.value);
    ok = result && qxp_error_a.value.code == QXP_SHADER_DECODE_OK &&
         qxp_error_a.value.token_offset == 0 &&
         qxp_ir_canaries_ok(&qxp_ir_a) &&
         qxp_error_canaries_ok(&qxp_error_a) &&
         qxp_validate_ir_independently(&qxp_ir_a.value, count);
    qxp_check(ok, name);
    return ok;
}

static int qxp_expect_reject(const DWORD *tokens, DWORD count,
                             DWORD expected_code, DWORD expected_offset,
                             const char *name)
{
    BOOL result_a;
    BOOL result_b;
    int ok;

    qxp_prepare_ir(&qxp_ir_a);
    qxp_prepare_ir(&qxp_ir_b);
    qxp_prepare_error(&qxp_error_a);
    qxp_prepare_error(&qxp_error_b);
    result_a = QxpDecodeShader20(tokens, count, &qxp_ir_a.value,
                                 &qxp_error_a.value);
    result_b = QxpDecodeShader20(tokens, count, &qxp_ir_b.value,
                                 &qxp_error_b.value);
    ok = !result_a && !result_b &&
         (expected_code == QXP_ANY_OFFSET ||
          qxp_error_a.value.code == expected_code) &&
         (expected_offset == QXP_ANY_OFFSET ||
          qxp_error_a.value.token_offset == expected_offset) &&
         memcmp(&qxp_error_a.value, &qxp_error_b.value,
                sizeof(qxp_error_a.value)) == 0 &&
         memcmp(&qxp_ir_a.value, &qxp_zero_ir,
                sizeof(qxp_ir_a.value)) == 0 &&
         memcmp(&qxp_ir_b.value, &qxp_zero_ir,
                sizeof(qxp_ir_b.value)) == 0 &&
         qxp_ir_canaries_ok(&qxp_ir_a) && qxp_ir_canaries_ok(&qxp_ir_b) &&
         qxp_error_canaries_ok(&qxp_error_a) &&
         qxp_error_canaries_ok(&qxp_error_b);
    qxp_check(ok, name);
    return ok;
}

static void qxp_test_golden_minimal(void)
{
    const QXP_SHADER_INSTRUCTION *instruction;

    if (qxp_decode_ok(qxp_minimal_ps, QXP_ARRAY_COUNT(qxp_minimal_ps),
                      "minimal ps_2_0")) {
        instruction = &qxp_ir_a.value.instructions[0];
        qxp_check(qxp_ir_a.value.stage == QXP_SHADER_STAGE_PIXEL &&
                  qxp_ir_a.value.instruction_count == 1 &&
                  qxp_ir_a.value.arithmetic_slot_count == 1 &&
                  instruction->opcode == QXP_SHADER_OP_MOV &&
                  instruction->destination_count == 1 &&
                  instruction->source_count == 1 &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_COLOROUT &&
                  instruction->sources[0].register_file ==
                      QXP_SHADER_REG_CONST &&
                  instruction->sources[0].swizzle[0] == 0 &&
                  instruction->sources[0].swizzle[3] == 3,
                  "minimal pixel typed fields");
    }

    if (qxp_decode_ok(qxp_minimal_vs, QXP_ARRAY_COUNT(qxp_minimal_vs),
                      "minimal vs_2_0")) {
        qxp_check(qxp_ir_a.value.stage == QXP_SHADER_STAGE_VERTEX &&
                  qxp_ir_a.value.instruction_count == 2 &&
                  qxp_ir_a.value.declaration_count == 1 &&
                  qxp_ir_a.value.instructions[0].declaration.present &&
                  qxp_ir_a.value.instructions[0].declaration.usage ==
                      D3DDECLUSAGE_POSITION &&
                  qxp_ir_a.value.instructions[1].destinations[0].
                      register_file == QXP_SHADER_REG_RASTOUT,
                  "minimal vertex typed fields");
    }

    if (qxp_decode_ok(qxp_partial_outputs_vs,
                      QXP_ARRAY_COUNT(qxp_partial_outputs_vs),
                      "vs partial and combined output masks")) {
        qxp_check(qxp_ir_a.value.instructions[1].destinations[0].write_mask ==
                      3 &&
                  qxp_ir_a.value.instructions[2].destinations[0].write_mask ==
                      12 &&
                  qxp_ir_a.value.instructions[3].destinations[0].write_mask ==
                      2 &&
                  qxp_ir_a.value.instructions[4].destinations[0].write_mask ==
                      5 &&
                  qxp_ir_a.value.instructions[5].destinations[0].write_mask ==
                      9,
                  "vertex output masks preserved");
    }
}

static void qxp_test_texture_and_immediates(void)
{
    if (qxp_decode_ok(qxp_texture_ps, QXP_ARRAY_COUNT(qxp_texture_ps),
                      "ps declarations and texture controls")) {
        qxp_check(qxp_ir_a.value.declaration_count == 2 &&
                  qxp_ir_a.value.texture_slot_count == 3 &&
                  qxp_ir_a.value.instructions[1].declaration.kind ==
                      QXP_SHADER_DECLARATION_SAMPLER &&
                  qxp_ir_a.value.instructions[1].declaration.sampler_type ==
                      QXP_SHADER_SAMPLER_2D &&
                  qxp_ir_a.value.instructions[2].control ==
                      QXP_SHADER_CONTROL_NONE &&
                  qxp_ir_a.value.instructions[3].control ==
                      QXP_SHADER_CONTROL_TEX_PROJECT &&
                  qxp_ir_a.value.instructions[4].control ==
                      QXP_SHADER_CONTROL_TEX_BIAS &&
                  qxp_ir_a.value.instructions[2].sources[1].register_file ==
                      QXP_SHADER_REG_SAMPLER,
                  "texture IR metadata");
    }

    if (qxp_decode_ok(qxp_arithmetic_ps,
                      QXP_ARRAY_COUNT(qxp_arithmetic_ps),
                      "ps arithmetic modifiers and immediates")) {
        qxp_check(qxp_ir_a.value.definition_count == 2 &&
                  qxp_ir_a.value.instructions[0].immediate_count == 4 &&
                  qxp_ir_a.value.instructions[0].immediate[0] ==
                      0x3f800000UL &&
                  qxp_ir_a.value.instructions[0].immediate[1] ==
                      0x80000000UL &&
                  qxp_ir_a.value.instructions[0].immediate[2] ==
                      0x7fc12345UL &&
                  qxp_ir_a.value.instructions[3].destinations[0].
                      destination_modifier ==
                      (QXP_SHADER_DSTMOD_SATURATE |
                       QXP_SHADER_DSTMOD_PARTIAL_PRECISION) &&
                  qxp_ir_a.value.instructions[3].sources[1].source_modifier ==
                      QXP_SHADER_SRCMOD_NEGATE,
                  "immediate bits and modifiers preserved");
    }
}

static void qxp_test_vertex_setup_and_relative(void)
{
    const QXP_SHADER_OPERAND *source;

    if (!qxp_decode_ok(qxp_setup_relative_vs,
                       QXP_ARRAY_COUNT(qxp_setup_relative_vs),
                       "vs declarations definitions and relative source"))
        return;
    source = &qxp_ir_a.value.instructions[6].sources[0];
    qxp_check(qxp_ir_a.value.declaration_count == 2 &&
              qxp_ir_a.value.definition_count == 3 &&
              qxp_ir_a.value.instructions[2].immediate[2] == 0x7f800000UL &&
              qxp_ir_a.value.instructions[3].immediate[2] == 0xffffffffUL &&
              qxp_ir_a.value.instructions[4].immediate[0] == 0x12345678UL &&
              qxp_ir_a.value.instructions[5].destinations[0].register_file ==
                  QXP_SHADER_REG_ADDRESS &&
              source->register_file == QXP_SHADER_REG_CONST &&
              source->register_number == 2 && source->relative.present &&
              source->relative.role == QXP_SHADER_OPERAND_RELATIVE_ADDRESS &&
              source->relative.register_file == QXP_SHADER_REG_ADDRESS &&
              source->relative.swizzle[0] == 0 &&
              source->relative.swizzle[3] == 0,
              "vertex setup and relative typed fields");
}

static void qxp_test_control_and_cfg(void)
{
    const QXP_SHADER_IR *ir;
    DWORD call_block;
    DWORD if_block;
    DWORD rep_end_block;

    if (!qxp_decode_ok(qxp_control_vs, QXP_ARRAY_COUNT(qxp_control_vs),
                       "nested exact vs_2_0 static control")) return;
    ir = &qxp_ir_a.value;
    qxp_check(ir->instruction_count == 18 &&
              ir->instructions[3].label_target == 15 &&
              ir->instructions[4].control_match == 12 &&
              ir->instructions[4].control_else == 8 &&
              ir->instructions[8].control_match == 12 &&
              ir->instructions[12].control_match == 4 &&
              ir->instructions[5].control_match == 7 &&
              ir->instructions[7].control_match == 5 &&
              ir->instructions[9].control_match == 11 &&
              ir->instructions[11].control_match == 9 &&
              ir->instructions[15].label_target == 15,
              "structured control and label resolution");
    call_block = qxp_find_block(ir, 3);
    if_block = qxp_find_block(ir, 4);
    rep_end_block = qxp_find_block(ir, 7);
    qxp_check(ir->blocks[call_block].successor_count == 1 &&
              !qxp_block_has_target(ir, call_block, 15) &&
              qxp_block_has_target(ir, call_block, 4) &&
              ir->blocks[if_block].successor_count == 2 &&
              qxp_block_has_target(ir, if_block, 5) &&
              qxp_block_has_target(ir, if_block, 9) &&
              ir->blocks[rep_end_block].successor_count == 2 &&
              qxp_block_has_target(ir, rep_end_block, 6) &&
              qxp_block_has_target(ir, rep_end_block, 8),
              "intraprocedural CFG branch loop and call continuation");
}

static void qxp_test_comments(void)
{
    if (!qxp_decode_ok(qxp_comment_shader,
                       QXP_ARRAY_COUNT(qxp_comment_shader),
                       "comments and exact terminal END")) return;
    qxp_check(qxp_ir_a.value.comment_count == 2 &&
              qxp_ir_a.value.instruction_count == 2 &&
              qxp_ir_a.value.instructions[0].opcode == QXP_SHADER_OP_NOP &&
              qxp_ir_a.value.instructions[0].token_offset == 4 &&
              qxp_ir_a.value.instructions[0].token_count == 1,
              "comments are framed but not instructions");
}

static DWORD qxp_build_arithmetic_shader(DWORD stage, DWORD opcode)
{
    DWORD cursor;
    DWORD source_count;
    DWORD destination_mask;
    DWORD i;

    cursor = 0;
    qxp_work_tokens[cursor++] = stage == QXP_SHADER_STAGE_VERTEX ?
                                D3DVS_VERSION(2, 0) :
                                D3DPS_VERSION(2, 0);
    source_count = 0;
    if (opcode == D3DSIO_MOV || opcode == D3DSIO_RCP ||
        opcode == D3DSIO_RSQ || opcode == D3DSIO_EXP ||
        opcode == D3DSIO_LOG || opcode == D3DSIO_LIT ||
        opcode == D3DSIO_FRC || opcode == D3DSIO_ABS ||
        opcode == D3DSIO_NRM || opcode == D3DSIO_MOVA ||
        opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP)
        source_count = 1;
    else if (opcode == D3DSIO_ADD || opcode == D3DSIO_SUB ||
             opcode == D3DSIO_MUL || opcode == D3DSIO_DP3 ||
             opcode == D3DSIO_DP4 || opcode == D3DSIO_MIN ||
             opcode == D3DSIO_MAX || opcode == D3DSIO_SLT ||
             opcode == D3DSIO_SGE || opcode == D3DSIO_DST ||
             opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 ||
             opcode == D3DSIO_M3x4 || opcode == D3DSIO_M3x3 ||
             opcode == D3DSIO_M3x2 || opcode == D3DSIO_POW ||
             opcode == D3DSIO_CRS)
        source_count = 2;
    else if (opcode == D3DSIO_MAD || opcode == D3DSIO_LRP ||
             opcode == D3DSIO_SGN || opcode == D3DSIO_SINCOS ||
             opcode == D3DSIO_CMP || opcode == D3DSIO_DP2ADD)
        source_count = 3;

    if (opcode == D3DSIO_NOP) {
        qxp_work_tokens[cursor++] = QXP_INST(opcode, 0);
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
        qxp_work_tokens[cursor++] = stage == QXP_SHADER_STAGE_VERTEX ?
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE) :
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
        qxp_work_tokens[cursor++] = D3DSIO_END;
        return cursor;
    }

    for (i = 1; i <= 3; ++i) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, i, 15, D3DSPDM_NONE);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    }

    destination_mask = 15;
    if (opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x3 ||
        opcode == D3DSIO_CRS)
        destination_mask = 7;
    else if (opcode == D3DSIO_M3x2 || opcode == D3DSIO_SINCOS)
        destination_mask = 3;
    else if (opcode == D3DSIO_MOVA ||
             (stage == QXP_SHADER_STAGE_VERTEX &&
              (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
               opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
               opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP ||
               opcode == D3DSIO_POW)))
        destination_mask = 1;
    qxp_work_tokens[cursor++] = QXP_INST(opcode, source_count + 1);
    if (opcode == D3DSIO_MOVA)
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_ADDR, 0, destination_mask, D3DSPDM_NONE);
    else
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, 0, destination_mask, D3DSPDM_NONE);

    if (opcode == D3DSIO_SINCOS) {
        qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 1);
    } else if (opcode == D3DSIO_SGN) {
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 3);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 1);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 2);
    } else if (opcode == D3DSIO_POW) {
        qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
        qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 2, 1);
    } else if (opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 ||
               opcode == D3DSIO_M3x4 || opcode == D3DSIO_M3x3 ||
               opcode == D3DSIO_M3x2) {
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 1);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 4);
    } else if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
               opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
               opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP) {
        qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 1, 0);
    } else if (opcode == D3DSIO_DP2ADD) {
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 1);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, 2);
        qxp_work_tokens[cursor++] = QXP_SCALAR(D3DSPR_TEMP, 3, 0);
    } else {
        for (i = 0; i < source_count; ++i)
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, i + 1);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] = stage == QXP_SHADER_STAGE_VERTEX ?
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE) :
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_test_exact_opcode_coverage(void)
{
    static const DWORD pixel_opcodes[] = {
        D3DSIO_NOP, D3DSIO_MOV, D3DSIO_ADD, D3DSIO_SUB, D3DSIO_MAD,
        D3DSIO_MUL, D3DSIO_RCP, D3DSIO_RSQ, D3DSIO_DP3, D3DSIO_DP4,
        D3DSIO_MIN, D3DSIO_MAX, D3DSIO_EXP, D3DSIO_LOG, D3DSIO_LRP,
        D3DSIO_FRC, D3DSIO_M4x4, D3DSIO_M4x3, D3DSIO_M3x4,
        D3DSIO_M3x3, D3DSIO_M3x2, D3DSIO_POW, D3DSIO_CRS,
        D3DSIO_ABS, D3DSIO_NRM, D3DSIO_SINCOS, D3DSIO_CMP,
        D3DSIO_DP2ADD
    };
    static const DWORD vertex_opcodes[] = {
        D3DSIO_NOP, D3DSIO_MOV, D3DSIO_ADD, D3DSIO_SUB, D3DSIO_MAD,
        D3DSIO_MUL, D3DSIO_RCP, D3DSIO_RSQ, D3DSIO_DP3, D3DSIO_DP4,
        D3DSIO_MIN, D3DSIO_MAX, D3DSIO_SLT, D3DSIO_SGE, D3DSIO_EXP,
        D3DSIO_LOG, D3DSIO_LIT, D3DSIO_DST, D3DSIO_LRP, D3DSIO_FRC,
        D3DSIO_M4x4, D3DSIO_M4x3, D3DSIO_M3x4, D3DSIO_M3x3,
        D3DSIO_M3x2, D3DSIO_POW, D3DSIO_CRS, D3DSIO_SGN, D3DSIO_ABS,
        D3DSIO_NRM, D3DSIO_SINCOS, D3DSIO_MOVA, D3DSIO_EXPP,
        D3DSIO_LOGP
    };
    DWORD i;
    DWORD count;

    for (i = 0; i < QXP_ARRAY_COUNT(pixel_opcodes); ++i) {
        count = qxp_build_arithmetic_shader(QXP_SHADER_STAGE_PIXEL,
                                            pixel_opcodes[i]);
        if (!qxp_decode_ok(qxp_work_tokens, count,
                           "complete exact ps_2_0 arithmetic opcode")) {
            fprintf(stderr, "qxp_shader_decode: pixel opcode %lu\n",
                    (unsigned long)pixel_opcodes[i]);
            break;
        }
    }
    for (i = 0; i < QXP_ARRAY_COUNT(vertex_opcodes); ++i) {
        count = qxp_build_arithmetic_shader(QXP_SHADER_STAGE_VERTEX,
                                            vertex_opcodes[i]);
        if (!qxp_decode_ok(qxp_work_tokens, count,
                           "complete exact vs_2_0 arithmetic opcode")) {
            fprintf(stderr, "qxp_shader_decode: vertex opcode %lu\n",
                    (unsigned long)vertex_opcodes[i]);
            break;
        }
    }
}

static void qxp_test_truncation_prefixes(void)
{
    DWORD i;

    for (i = 0; i < QXP_ARRAY_COUNT(qxp_texture_ps); ++i) {
        if (!qxp_expect_reject(qxp_texture_ps, i, QXP_ANY_OFFSET,
                               QXP_ANY_OFFSET,
                               "every ps truncation prefix rejects")) break;
    }
    for (i = 0; i < QXP_ARRAY_COUNT(qxp_control_vs); ++i) {
        if (!qxp_expect_reject(qxp_control_vs, i, QXP_ANY_OFFSET,
                               QXP_ANY_OFFSET,
                               "every vs truncation prefix rejects")) break;
    }
}

static void qxp_test_versions_and_end(void)
{
    static const DWORD ps21[] = {D3DPS_VERSION(2, 1), D3DSIO_END};
    static const DWORD vs21[] = {D3DVS_VERSION(2, 1), D3DSIO_END};
    static const DWORD ps30[] = {D3DPS_VERSION(3, 0), D3DSIO_END};
    static const DWORD junk[] = {0UL, D3DSIO_END};
    static const DWORD missing[] = {D3DPS_VERSION(2, 0)};
    static const DWORD trailing[] = {
        D3DPS_VERSION(2, 0), D3DSIO_END, 0UL
    };
    static const DWORD bad_end[] = {
        D3DPS_VERSION(2, 0), D3DSIO_END | 0x00010000UL
    };

    qxp_expect_reject(ps21, QXP_ARRAY_COUNT(ps21),
                      QXP_SHADER_DECODE_VERSION, 0, "reject ps_2_x version");
    qxp_expect_reject(vs21, QXP_ARRAY_COUNT(vs21),
                      QXP_SHADER_DECODE_VERSION, 0, "reject vs_2_x version");
    qxp_expect_reject(ps30, QXP_ARRAY_COUNT(ps30),
                      QXP_SHADER_DECODE_VERSION, 0, "reject SM3 version");
    qxp_expect_reject(junk, QXP_ARRAY_COUNT(junk),
                      QXP_SHADER_DECODE_VERSION, 0, "reject unknown version");
    qxp_expect_reject(missing, QXP_ARRAY_COUNT(missing),
                      QXP_SHADER_DECODE_MISSING_END, 1, "missing END");
    qxp_expect_reject(trailing, QXP_ARRAY_COUNT(trailing),
                      QXP_SHADER_DECODE_TRAILING_TOKENS, 2,
                      "nonterminal END");
    qxp_expect_reject(bad_end, QXP_ARRAY_COUNT(bad_end),
                      QXP_SHADER_DECODE_INSTRUCTION_TOKEN, 1,
                      "END must be exact");
}

static void qxp_test_framing_and_markers(void)
{
    static const DWORD short_length[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 1),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD long_length[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1), D3DSIO_END
    };
    static const DWORD zero_length[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 0), D3DSIO_END
    };
    static const DWORD overrun[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MAD, 4),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD comment_overrun[] = {
        D3DPS_VERSION(2, 0), D3DSHADER_COMMENT(3), 1UL, D3DSIO_END
    };
    static const DWORD missing_dest_marker[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE) & ~QXP_PARAMETER,
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD missing_source_marker[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0) & ~QXP_PARAMETER, D3DSIO_END
    };
    static const DWORD missing_dcl_marker[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0) & ~QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD missing_relative_marker[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0) & ~QXP_PARAMETER, D3DSIO_END
    };
    static const DWORD reserved_destination[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE) | 0x00004000UL,
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD reserved_source[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0) | 0x00008000UL, D3DSIO_END
    };
    static const DWORD reserved_relative[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0) | 0x00004000UL, D3DSIO_END
    };
    static const DWORD bad_comment_marker[] = {
        D3DPS_VERSION(2, 0), D3DSHADER_COMMENT(0) | QXP_PARAMETER,
        D3DSIO_END
    };

    qxp_expect_reject(short_length, QXP_ARRAY_COUNT(short_length),
                      QXP_SHADER_DECODE_INSTRUCTION_LENGTH, QXP_ANY_OFFSET,
                      "short instruction length");
    qxp_expect_reject(long_length, QXP_ARRAY_COUNT(long_length),
                      QXP_SHADER_DECODE_INSTRUCTION_LENGTH, 1,
                      "long instruction length");
    qxp_expect_reject(zero_length, QXP_ARRAY_COUNT(zero_length),
                      QXP_SHADER_DECODE_INSTRUCTION_LENGTH, QXP_ANY_OFFSET,
                      "zero instruction length");
    qxp_expect_reject(overrun, QXP_ARRAY_COUNT(overrun),
                      QXP_SHADER_DECODE_INSTRUCTION_LENGTH, 1,
                      "parameter sequence cannot consume END");
    qxp_expect_reject(comment_overrun, QXP_ARRAY_COUNT(comment_overrun),
                      QXP_SHADER_DECODE_COMMENT, 1, "comment overrun");
    qxp_expect_reject(missing_dest_marker,
                      QXP_ARRAY_COUNT(missing_dest_marker),
                      QXP_SHADER_DECODE_PARAMETER_MARKER, 2,
                      "destination marker bit");
    qxp_expect_reject(missing_source_marker,
                      QXP_ARRAY_COUNT(missing_source_marker),
                      QXP_SHADER_DECODE_PARAMETER_MARKER, 3,
                      "source marker bit");
    qxp_expect_reject(missing_dcl_marker,
                      QXP_ARRAY_COUNT(missing_dcl_marker),
                      QXP_SHADER_DECODE_PARAMETER_MARKER, 2,
                      "DCL info marker bit");
    qxp_expect_reject(missing_relative_marker,
                      QXP_ARRAY_COUNT(missing_relative_marker),
                      QXP_SHADER_DECODE_PARAMETER_MARKER, 4,
                      "relative marker bit");
    qxp_expect_reject(reserved_destination,
                      QXP_ARRAY_COUNT(reserved_destination),
                      QXP_SHADER_DECODE_OPERAND_BITS, 2,
                      "reserved destination operand bits");
    qxp_expect_reject(reserved_source, QXP_ARRAY_COUNT(reserved_source),
                      QXP_SHADER_DECODE_OPERAND_BITS, 3,
                      "reserved source operand bits");
    qxp_expect_reject(reserved_relative,
                      QXP_ARRAY_COUNT(reserved_relative),
                      QXP_SHADER_DECODE_OPERAND_BITS, 4,
                      "reserved relative operand bits");
    qxp_expect_reject(bad_comment_marker,
                      QXP_ARRAY_COUNT(bad_comment_marker),
                      QXP_SHADER_DECODE_COMMENT, 1,
                      "comment token marker bit");
}

static void qxp_copy_tokens(DWORD *destination, const DWORD *source,
                            DWORD count)
{
    DWORD i;

    for (i = 0; i < count; ++i) destination[i] = source[i];
}

static void qxp_test_registers_modifiers_and_opcodes(void)
{
    static const DWORD ps_temp_range[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 32, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD ps_temp_r12[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_TEMP, 12, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_TEMP, 12),
        D3DSIO_END
    };
    static const DWORD ps_temp_r31[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_TEMP, 31, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_TEMP, 31),
        D3DSIO_END
    };
    static const DWORD ps_const_range[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 32), D3DSIO_END
    };
    static const DWORD bad_role[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_CONST, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1), D3DSIO_END
    };
    static const DWORD sampler_arithmetic[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_SAMPLER, 0), D3DSIO_END
    };
    static const DWORD bad_split_type[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(31, 0), D3DSIO_END
    };
    static const DWORD bad_srcmod[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC_SWIZZLE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE,
                        D3DSPSM_ABS), D3DSIO_END
    };
    static const DWORD arbitrary_swizzle[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC_SWIZZLE(D3DSPR_CONST, 0,
                        (0x39UL << D3DSP_SWIZZLE_SHIFT), D3DSPSM_NONE),
        QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), D3DSIO_END
    };
    static const DWORD bad_shift[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE) |
            (1UL << D3DSP_DSTSHIFT_SHIFT),
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD bad_centroid[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_ADD, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_MSAMPCENTROID),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1), D3DSIO_END
    };
    static const DWORD bad_add_control[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_ADD, 3) | 0x00010000UL,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1), D3DSIO_END
    };
    static const DWORD bad_tex_control[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_TEX, 3) |
            D3DSI_TEXLD_PROJECT | D3DSI_TEXLD_BIAS,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_SAMPLER, 0), D3DSIO_END
    };
    static const DWORD unsupported_ps[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_TEXLDD, 0), D3DSIO_END
    };
    static const DWORD unsupported_vs[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_BREAK, 0), D3DSIO_END
    };
    DWORD i;
    static const DWORD unsupported_opcodes[] = {
        D3DSIO_IFC, D3DSIO_BREAKC, D3DSIO_SETP, D3DSIO_BREAKP,
        D3DSIO_TEXLDL, D3DSIO_DSX, D3DSIO_DSY
    };

    qxp_expect_reject(ps_temp_range, QXP_ARRAY_COUNT(ps_temp_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, QXP_ANY_OFFSET,
                      "ps temporary structural range r32");
    if (qxp_decode_ok(ps_temp_r12, QXP_ARRAY_COUNT(ps_temp_r12),
                      "ps_2_0 structurally permits r12"))
        qxp_check(qxp_ir_a.value.required_ps_temp_count == 13 &&
                  qxp_ir_a.value.required_ps20_caps == 0,
                  "r12 exact future temp requirement");
    if (qxp_decode_ok(ps_temp_r31, QXP_ARRAY_COUNT(ps_temp_r31),
                      "ps_2_0 structurally permits r31"))
        qxp_check(qxp_ir_a.value.required_ps_temp_count == 32 &&
                  qxp_ir_a.value.required_ps20_caps == 0,
                  "r31 exact future temp requirement");
    qxp_expect_reject(ps_const_range, QXP_ARRAY_COUNT(ps_const_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, QXP_ANY_OFFSET,
                      "ps constant range");
    qxp_expect_reject(bad_role, QXP_ARRAY_COUNT(bad_role),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "destination register role");
    qxp_expect_reject(sampler_arithmetic,
                      QXP_ARRAY_COUNT(sampler_arithmetic),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "sampler arithmetic role");
    qxp_expect_reject(bad_split_type, QXP_ARRAY_COUNT(bad_split_type),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "five-bit split register type");
    qxp_expect_reject(bad_srcmod, QXP_ARRAY_COUNT(bad_srcmod),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "unsupported exact-profile source modifier");
    if (qxp_decode_ok(arbitrary_swizzle,
                      QXP_ARRAY_COUNT(arbitrary_swizzle),
                      "ps arbitrary swizzle is capability-dependent"))
        qxp_check(qxp_ir_a.value.required_ps_temp_count == 1 &&
                  qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE,
                  "arbitrary swizzle exact future requirement");
    qxp_expect_reject(bad_shift, QXP_ARRAY_COUNT(bad_shift),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "SM2 destination shift");
    qxp_expect_reject(bad_centroid, QXP_ARRAY_COUNT(bad_centroid),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "centroid only on declarations");
    qxp_expect_reject(bad_add_control, QXP_ARRAY_COUNT(bad_add_control),
                      QXP_SHADER_DECODE_CONTROL, 1,
                      "opcode control on arithmetic");
    qxp_expect_reject(bad_tex_control, QXP_ARRAY_COUNT(bad_tex_control),
                      QXP_SHADER_DECODE_CONTROL, 1,
                      "combined texldp texldb controls");
    qxp_expect_reject(unsupported_ps, QXP_ARRAY_COUNT(unsupported_ps),
                      QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                      "reject ps_2_x TEXLDD");
    qxp_expect_reject(unsupported_vs, QXP_ARRAY_COUNT(unsupported_vs),
                      QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                      "reject vs_2_x BREAK");

    for (i = 0; i < QXP_ARRAY_COUNT(unsupported_opcodes); ++i) {
        DWORD shader[3];

        shader[0] = i < 4 ? D3DVS_VERSION(2, 0) : D3DPS_VERSION(2, 0);
        shader[1] = QXP_INST(unsupported_opcodes[i], 0);
        shader[2] = D3DSIO_END;
        qxp_expect_reject(shader, QXP_ARRAY_COUNT(shader),
                          QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                          "reject 2_x or SM3-only opcode");
    }

    qxp_copy_tokens(qxp_work_tokens, qxp_minimal_ps,
                    QXP_ARRAY_COUNT(qxp_minimal_ps));
    qxp_work_tokens[1] |= D3DSHADER_INSTRUCTION_PREDICATED;
    qxp_expect_reject(qxp_work_tokens, QXP_ARRAY_COUNT(qxp_minimal_ps),
                      QXP_SHADER_DECODE_CONTROL, 1, "reject predication");
    qxp_copy_tokens(qxp_work_tokens, qxp_minimal_ps,
                    QXP_ARRAY_COUNT(qxp_minimal_ps));
    qxp_work_tokens[1] |= D3DSI_COISSUE;
    qxp_expect_reject(qxp_work_tokens, QXP_ARRAY_COUNT(qxp_minimal_ps),
                      QXP_SHADER_DECODE_CONTROL, 1, "reject coissue");
    qxp_copy_tokens(qxp_work_tokens, qxp_minimal_ps,
                    QXP_ARRAY_COUNT(qxp_minimal_ps));
    qxp_work_tokens[1] |= 0x20000000UL;
    qxp_expect_reject(qxp_work_tokens, QXP_ARRAY_COUNT(qxp_minimal_ps),
                      QXP_SHADER_DECODE_INSTRUCTION_TOKEN, 1,
                      "reject reserved instruction bit 29");
    qxp_copy_tokens(qxp_work_tokens, qxp_minimal_ps,
                    QXP_ARRAY_COUNT(qxp_minimal_ps));
    qxp_work_tokens[1] |= 0x80000000UL;
    qxp_expect_reject(qxp_work_tokens, QXP_ARRAY_COUNT(qxp_minimal_ps),
                      QXP_SHADER_DECODE_INSTRUCTION_TOKEN, 1,
                      "reject reserved instruction bit 31");
}

static void qxp_test_read_ports(void)
{
    static const DWORD ps_same_constant[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD ps_one_texture_read[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD ps_same_texture_twice[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD ps_sincos_constants[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD vs_same_input[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2),
            QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
            QXP_DST(D3DSPR_INPUT, 0, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_INPUT, 0),
            QXP_SRC_SWIZZLE(D3DSPR_INPUT, 0,
                            (0x39UL << D3DSP_SWIZZLE_SHIFT),
                            D3DSPSM_NONE),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD vs_same_constant_twice[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD vs_same_constant_three_times[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MAD, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD vs_sincos_constants[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD vs_sincos_relative_constants[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 0),
        QXP_INST(D3DSIO_SINCOS, 6), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
            QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
            QXP_RELATIVE(D3DSPR_CONST, 1, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD vs_sincos_mixed_relative_constants[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 0),
        QXP_INST(D3DSIO_SINCOS, 6), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
            QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
            QXP_RELATIVE(D3DSPR_CONST, 1, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD ps_sincos_destination_angle_alias[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD vs_sincos_destination_angle_alias[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD ps_three_constant_reads[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MAD, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD ps_distinct_constants[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    static const DWORD ps_distinct_files[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 0, 15, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    static const DWORD ps_distinct_inputs[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 0, 15, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 1, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_INPUT, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD ps_distinct_textures[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 1, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_TEXTURE, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD vs_distinct_constants[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    static const DWORD vs_distinct_inputs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2),
            QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
            QXP_DST(D3DSPR_INPUT, 0, 15, 0),
        QXP_INST(D3DSIO_DCL, 2),
            QXP_DCL_USAGE(D3DDECLUSAGE_NORMAL, 0),
            QXP_DST(D3DSPR_INPUT, 1, 15, 0),
        QXP_INST(D3DSIO_ADD, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_INPUT, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD vs_matrix_constants[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_M4x4, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 8), QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 12),
        D3DSIO_END
    };

    qxp_decode_ok(ps_same_constant, QXP_ARRAY_COUNT(ps_same_constant),
                  "ps two reads from one constant register");
    qxp_decode_ok(ps_one_texture_read, QXP_ARRAY_COUNT(ps_one_texture_read),
                  "ps one texture-coordinate read per instruction");
    qxp_expect_reject(ps_same_texture_twice,
                      QXP_ARRAY_COUNT(ps_same_texture_twice),
                      QXP_SHADER_DECODE_READ_PORT, 4,
                      "ps repeated t0 still exceeds one read per instruction");
    qxp_decode_ok(ps_sincos_constants,
                  QXP_ARRAY_COUNT(ps_sincos_constants),
                  "ps sincos constant read-port exception");
    qxp_decode_ok(vs_same_input, QXP_ARRAY_COUNT(vs_same_input),
                  "vs repeated access through one input read port");
    qxp_decode_ok(vs_same_constant_twice,
                  QXP_ARRAY_COUNT(vs_same_constant_twice),
                  "vs permits two reads from the same constant register");
    qxp_expect_reject(vs_same_constant_three_times,
                      QXP_ARRAY_COUNT(vs_same_constant_three_times),
                      QXP_SHADER_DECODE_READ_PORT, 1,
                      "vs repeated c0 exceeds two reads per instruction");
    qxp_decode_ok(vs_sincos_constants,
                  QXP_ARRAY_COUNT(vs_sincos_constants),
                  "vs sincos constant read-port exception");
    qxp_decode_ok(vs_sincos_relative_constants,
                  QXP_ARRAY_COUNT(vs_sincos_relative_constants),
                  "vs sincos relative constants share one a0 component");
    qxp_expect_reject(vs_sincos_mixed_relative_constants,
                      QXP_ARRAY_COUNT(vs_sincos_mixed_relative_constants),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, 7,
                      "vs sincos rejects mixed a0 components");
    qxp_expect_reject(ps_sincos_destination_angle_alias,
                      QXP_ARRAY_COUNT(ps_sincos_destination_angle_alias),
                      QXP_SHADER_DECODE_REGISTER_ROLE, 4,
                      "ps sincos destination cannot alias its angle");
    qxp_expect_reject(vs_sincos_destination_angle_alias,
                      QXP_ARRAY_COUNT(vs_sincos_destination_angle_alias),
                      QXP_SHADER_DECODE_REGISTER_ROLE, 4,
                      "vs sincos destination cannot alias its angle");
    qxp_expect_reject(ps_three_constant_reads,
                      QXP_ARRAY_COUNT(ps_three_constant_reads),
                      QXP_SHADER_DECODE_READ_PORT, QXP_ANY_OFFSET,
                      "ps two total constant reads per instruction");
    qxp_expect_reject(ps_distinct_constants,
                      QXP_ARRAY_COUNT(ps_distinct_constants),
                      QXP_SHADER_DECODE_READ_PORT, QXP_ANY_OFFSET,
                      "ps one distinct constant register per instruction");
    qxp_decode_ok(ps_distinct_files, QXP_ARRAY_COUNT(ps_distinct_files),
                  "ps v0 and t0 use distinct read ports");
    qxp_expect_reject(ps_distinct_inputs,
                      QXP_ARRAY_COUNT(ps_distinct_inputs),
                      QXP_SHADER_DECODE_READ_PORT, 7,
                      "ps distinct v registers overuse the v read port");
    qxp_expect_reject(ps_distinct_textures,
                      QXP_ARRAY_COUNT(ps_distinct_textures),
                      QXP_SHADER_DECODE_READ_PORT, 7,
                      "ps distinct t registers overuse the t read port");
    qxp_expect_reject(vs_distinct_constants,
                      QXP_ARRAY_COUNT(vs_distinct_constants),
                      QXP_SHADER_DECODE_READ_PORT, QXP_ANY_OFFSET,
                      "vs one distinct constant register per instruction");
    qxp_expect_reject(vs_distinct_inputs,
                      QXP_ARRAY_COUNT(vs_distinct_inputs),
                      QXP_SHADER_DECODE_READ_PORT, QXP_ANY_OFFSET,
                      "vs one distinct input register per instruction");
    qxp_expect_reject(vs_matrix_constants,
                      QXP_ARRAY_COUNT(vs_matrix_constants),
                      QXP_SHADER_DECODE_READ_PORT, QXP_ANY_OFFSET,
                      "matrix vector and rows cannot share a one-port file");
}

static void qxp_test_component_initialization(void)
{
    static const DWORD partial_temp_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD partial_v_defaults_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 0, 1, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_INPUT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    static const DWORD partial_texture_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 3, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD matrix_inputs_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 0, 7, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 1, 7, 0),
        QXP_INST(D3DSIO_M3x2, 3), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_INPUT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD uninitialized_temp_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD uninitialized_output_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    static const DWORD undeclared_texture_component_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 3, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 4, 0),
            QXP_SCALAR(D3DSPR_TEXTURE, 0, 2),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD missing_matrix_row_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_INPUT, 0, 7, 0),
        QXP_INST(D3DSIO_M3x2, 3), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_INPUT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD sincos_preserves_w_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 8, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD sincos_clobbers_y_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD incomplete_texld_coords_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_DCL_SAMPLER(D3DSTT_2D),
            QXP_DST(D3DSPR_SAMPLER, 0, 15, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_TEX, 3), QXP_DST(D3DSPR_TEMP, 1, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_SAMPLER, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };

    qxp_decode_ok(partial_temp_ps, QXP_ARRAY_COUNT(partial_temp_ps),
                  "ps initialized temporary component read");
    if (qxp_decode_ok(partial_v_defaults_ps,
                      QXP_ARRAY_COUNT(partial_v_defaults_ps),
                      "ps partial v declaration supplies default channels"))
        qxp_check(qxp_ir_a.value.instructions[0].destinations[0].write_mask ==
                      1 &&
                  qxp_ir_a.value.instructions[1].sources[0].register_file ==
                      QXP_SHADER_REG_INPUT &&
                  qxp_ir_a.value.required_ps_temp_count == 1,
                  "partial v linkage mask and defaulted read metadata");
    qxp_decode_ok(partial_texture_ps, QXP_ARRAY_COUNT(partial_texture_ps),
                  "ps declared texture components read");
    qxp_decode_ok(matrix_inputs_ps, QXP_ARRAY_COUNT(matrix_inputs_ps),
                  "ps matrix implicit input rows declared");
    qxp_decode_ok(sincos_preserves_w_ps,
                  QXP_ARRAY_COUNT(sincos_preserves_w_ps),
                  "ps sincos leaves destination w untouched");
    qxp_expect_reject(uninitialized_temp_ps,
                      QXP_ARRAY_COUNT(uninitialized_temp_ps),
                      QXP_SHADER_DECODE_UNINITIALIZED, QXP_ANY_OFFSET,
                      "ps temporary component initialized before read");
    qxp_expect_reject(uninitialized_output_ps,
                      QXP_ARRAY_COUNT(uninitialized_output_ps),
                      QXP_SHADER_DECODE_UNINITIALIZED, QXP_ANY_OFFSET,
                      "ps output cannot read an uninitialized temporary");
    qxp_expect_reject(undeclared_texture_component_ps,
                      QXP_ARRAY_COUNT(undeclared_texture_component_ps),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "ps texture declaration is component-exact");
    qxp_expect_reject(missing_matrix_row_ps,
                      QXP_ARRAY_COUNT(missing_matrix_row_ps),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "ps matrix implicit rows require declarations");
    qxp_expect_reject(sincos_clobbers_y_ps,
                      QXP_ARRAY_COUNT(sincos_clobbers_y_ps),
                      QXP_SHADER_DECODE_UNINITIALIZED, QXP_ANY_OFFSET,
                      "ps sincos clobbers unwritten xyz components");
    qxp_expect_reject(incomplete_texld_coords_ps,
                      QXP_ARRAY_COUNT(incomplete_texld_coords_ps),
                      QXP_SHADER_DECODE_UNINITIALIZED, QXP_ANY_OFFSET,
                      "texld temporary coordinates are initialized");
}

static void qxp_test_vertex_definite_assignment(void)
{
    static const DWORD branch_intersection_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_IF, 1), QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_ELSE, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 1, 0),
        QXP_INST(D3DSIO_ENDIF, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    static const DWORD branch_intersection_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_IF, 1), QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_ENDIF, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD loop_zero_iteration_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 1, 0),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    static const DWORD loop_zero_iteration_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD loop_zero_iteration_write_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD rep_sincos_post_clobber_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD loop_sgn_post_clobber_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_SGN, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEMP, 1),
            QXP_SRC(D3DSPR_TEMP, 2),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 3, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD rep_sincos_next_iteration_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_IF, 1), QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 1),
        QXP_INST(D3DSIO_ENDIF, 0),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD loop_sgn_next_iteration_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 3, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
        QXP_INST(D3DSIO_SGN, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEMP, 1),
            QXP_SRC(D3DSPR_TEMP, 2),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD rep_call_post_clobber_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD loop_callnz_next_iteration_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
            QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD rep_clobber_restore_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 1),
        QXP_INST(D3DSIO_ENDREP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 4, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD loop_clobber_restore_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 3, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
        QXP_INST(D3DSIO_SGN, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEMP, 1),
            QXP_SRC(D3DSPR_TEMP, 2),
        QXP_INST(D3DSIO_IF, 1), QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
        QXP_INST(D3DSIO_ELSE, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 0),
        QXP_INST(D3DSIO_ENDIF, 0),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 4, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 1, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD partial_component_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD partial_component_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD address_component_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_MOV, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_RELATIVE(D3DSPR_CONST, 4, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    static const DWORD address_component_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_RELATIVE(D3DSPR_CONST, 4, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD multiple_calls[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD conditional_call_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
            QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD conditional_call_required_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
            QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD conditional_call_clobber_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 3, 1),
        QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
            QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 4),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD subroutine_address_required_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_RELATIVE(D3DSPR_CONST, 4, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD subroutine_address_guaranteed_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_RELATIVE(D3DSPR_CONST, 4, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD subroutine_address_conditional_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALLNZ, 2), QXP_SRC(D3DSPR_LABEL, 0),
            QXP_SRC(D3DSPR_CONSTBOOL, 0),
        QXP_INST(D3DSIO_MOV, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_RELATIVE(D3DSPR_CONST, 4, D3DSP_NOSWIZZLE),
            QXP_SCALAR(D3DSPR_ADDR, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOVA, 2), QXP_DST(D3DSPR_ADDR, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD subroutine_required_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 1),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 15, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD subroutine_required_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 1),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 15, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 0),
        QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD sincos_preserve_w[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 8, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 3),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 1, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 3),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD sincos_kills_y[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 0, 2, 0),
            QXP_SCALAR(D3DSPR_CONST, 0, 1),
        QXP_INST(D3DSIO_SINCOS, 4), QXP_DST(D3DSPR_TEMP, 0, 1, 0),
            QXP_SCALAR(D3DSPR_CONST, 2, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 2, 0),
            QXP_SCALAR(D3DSPR_TEMP, 0, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    static const DWORD sgn_scratch_ok[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_SGN, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEMP, 1),
            QXP_SRC(D3DSPR_TEMP, 2),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    static const DWORD sgn_scratch_bad[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_SGN, 4), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEMP, 1),
            QXP_SRC(D3DSPR_TEMP, 2),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 3, 15, 0),
            QXP_SRC(D3DSPR_TEMP, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };

    qxp_decode_ok(branch_intersection_ok,
                  QXP_ARRAY_COUNT(branch_intersection_ok),
                  "VS branch intersection defines a component on both arms");
    qxp_expect_reject(branch_intersection_bad,
                      QXP_ARRAY_COUNT(branch_intersection_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 7,
                      "VS one-arm write is not definitely assigned");
    qxp_decode_ok(loop_zero_iteration_ok,
                  QXP_ARRAY_COUNT(loop_zero_iteration_ok),
                  "VS value initialized before a zero-iteration loop");
    qxp_expect_reject(loop_zero_iteration_bad,
                      QXP_ARRAY_COUNT(loop_zero_iteration_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 7,
                      "VS REP body may execute zero times");
    qxp_expect_reject(loop_zero_iteration_write_bad,
                      QXP_ARRAY_COUNT(loop_zero_iteration_write_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 8,
                      "VS LOOP body may execute zero times");
    qxp_expect_reject(rep_sincos_post_clobber_bad,
                      QXP_ARRAY_COUNT(rep_sincos_post_clobber_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 12,
                      "VS REP executed-path SINCOS clobber reaches exit");
    qxp_expect_reject(loop_sgn_post_clobber_bad,
                      QXP_ARRAY_COUNT(loop_sgn_post_clobber_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 13,
                      "VS LOOP executed-path SGN clobber reaches exit");
    qxp_expect_reject(rep_sincos_next_iteration_bad,
                      QXP_ARRAY_COUNT(rep_sincos_next_iteration_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 6,
                      "VS REP preserves inputs required by later iterations");
    qxp_expect_reject(loop_sgn_next_iteration_bad,
                      QXP_ARRAY_COUNT(loop_sgn_next_iteration_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 7,
                      "VS LOOP preserves inputs required by later iterations");
    qxp_expect_reject(rep_call_post_clobber_bad,
                      QXP_ARRAY_COUNT(rep_call_post_clobber_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 9,
                      "VS REP propagates unconditional call clobbers");
    qxp_expect_reject(loop_callnz_next_iteration_bad,
                      QXP_ARRAY_COUNT(loop_callnz_next_iteration_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 7,
                      "VS LOOP propagates conditional call clobbers");
    qxp_decode_ok(rep_clobber_restore_ok,
                  QXP_ARRAY_COUNT(rep_clobber_restore_ok),
                  "VS REP definitely restores a clobbered iteration input");
    qxp_decode_ok(loop_clobber_restore_ok,
                  QXP_ARRAY_COUNT(loop_clobber_restore_ok),
                  "VS LOOP restores an iteration input on both IF arms");
    qxp_decode_ok(partial_component_ok,
                  QXP_ARRAY_COUNT(partial_component_ok),
                  "VS swizzled initialized component read");
    qxp_expect_reject(partial_component_bad,
                      QXP_ARRAY_COUNT(partial_component_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 4,
                      "VS unwritten temp component read");
    qxp_decode_ok(address_component_ok,
                  QXP_ARRAY_COUNT(address_component_ok),
                  "VS relative c read uses written a0 component");
    qxp_expect_reject(address_component_bad,
                      QXP_ARRAY_COUNT(address_component_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 4,
                      "VS relative c read requires selected a0 component");

    if (qxp_decode_ok(multiple_calls, QXP_ARRAY_COUNT(multiple_calls),
                      "VS subroutine writes feed multiple call sites")) {
        const QXP_SHADER_IR *ir;
        DWORD call0_block;
        DWORD call1_block;
        DWORD main_ret_block;
        DWORD sub_ret_block;

        ir = &qxp_ir_a.value;
        call0_block = qxp_find_block(ir, 0);
        call1_block = qxp_find_block(ir, 2);
        main_ret_block = qxp_find_block(ir, 4);
        sub_ret_block = qxp_find_block(ir, 7);
        qxp_check(ir->instructions[0].label_target == 5 &&
                  ir->instructions[2].label_target == 5 &&
                  ir->blocks[call0_block].successor_count == 1 &&
                  qxp_block_has_target(ir, call0_block, 1) &&
                  !qxp_block_has_target(ir, call0_block, 5) &&
                  ir->blocks[call1_block].successor_count == 1 &&
                  qxp_block_has_target(ir, call1_block, 3) &&
                  !qxp_block_has_target(ir, call1_block, 5) &&
                  ir->blocks[main_ret_block].successor_count == 0 &&
                  ir->blocks[sub_ret_block].successor_count == 0,
                  "summarized call CFG has continuation-only edges");
    }
    qxp_expect_reject(conditional_call_bad,
                      QXP_ARRAY_COUNT(conditional_call_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 4,
                      "CALLNZ does not guarantee subroutine writes");
    qxp_expect_reject(conditional_call_required_bad,
                      QXP_ARRAY_COUNT(conditional_call_required_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 1,
                      "CALLNZ still checks required subroutine inputs");
    qxp_expect_reject(conditional_call_clobber_bad,
                      QXP_ARRAY_COUNT(conditional_call_clobber_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 7,
                      "CALLNZ applies possible subroutine clobbers");
    qxp_expect_reject(subroutine_address_required_bad,
                      QXP_ARRAY_COUNT(subroutine_address_required_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 1,
                      "subroutine a0 requirement crosses CALL");
    if (qxp_decode_ok(subroutine_address_guaranteed_ok,
                      QXP_ARRAY_COUNT(subroutine_address_guaranteed_ok),
                      "subroutine a0 guarantee crosses CALL"))
        qxp_check(qxp_ir_a.value.instructions[1].sources[0].relative.present &&
                  qxp_ir_a.value.instructions[1].sources[0].relative.
                      register_file == QXP_SHADER_REG_ADDRESS &&
                  qxp_ir_a.value.instructions[1].sources[0].relative.
                      swizzle[0] == 0,
                  "CALL-guaranteed a0 enables downstream relative read");
    qxp_expect_reject(subroutine_address_conditional_bad,
                      QXP_ARRAY_COUNT(subroutine_address_conditional_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 4,
                      "CALLNZ does not guarantee subroutine a0 writes");
    qxp_decode_ok(subroutine_required_ok,
                  QXP_ARRAY_COUNT(subroutine_required_ok),
                  "subroutine required input and guaranteed write summary");
    qxp_expect_reject(subroutine_required_bad,
                      QXP_ARRAY_COUNT(subroutine_required_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 1,
                      "subroutine required input enforced at call site");
    qxp_decode_ok(sincos_preserve_w, QXP_ARRAY_COUNT(sincos_preserve_w),
                  "VS SINCOS preserves destination w");
    qxp_expect_reject(sincos_kills_y, QXP_ARRAY_COUNT(sincos_kills_y),
                      QXP_SHADER_DECODE_UNINITIALIZED, 9,
                      "VS SINCOS makes unwritten xyz undefined");
    qxp_decode_ok(sgn_scratch_ok, QXP_ARRAY_COUNT(sgn_scratch_ok),
                  "VS SGN scratch temps are outputs not required inputs");
    qxp_expect_reject(sgn_scratch_bad, QXP_ARRAY_COUNT(sgn_scratch_bad),
                      QXP_SHADER_DECODE_UNINITIALIZED, 6,
                      "VS SGN scratch temps are undefined afterward");
}

static void qxp_test_exact_profile_rules(void)
{
    static const DWORD depth_and_mrt_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 1, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_DEPTHOUT, 0, 15, D3DSPDM_NONE),
            QXP_SCALAR(D3DSPR_CONST, 2, 2),
        D3DSIO_END
    };
    static const DWORD scalar_full_mask_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_RCP, 2),
            QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD bad_matrix_mask_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_M4x4, 3),
            QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_CONST, 4),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD missing_ps_output[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_NOP, 0), D3DSIO_END
    };
    static const DWORD duplicate_ps_output[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD sparse_ps_output[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_COLOROUT, 2, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD missing_vs_position[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD bad_texture_mask_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_TEXCRDOUT, 0, 2, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD skipped_texture_output_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_TEXCRDOUT, 1, 1, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD bad_scalar_output_mask_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_FOG, 1, D3DSPDM_NONE),
            QXP_SCALAR(D3DSPR_CONST, 0, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD bad_scalar_output_source_vs[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE, 15,
                    D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD nrm_xyz_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 7, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_NRM, 2), QXP_DST(D3DSPR_TEMP, 0, 7, 0),
            QXP_SRC(D3DSPR_TEMP, 1),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15, 0),
            QXP_SRC(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD bad_nrm_mask_ps[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_TEMP, 1, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_NRM, 2), QXP_DST(D3DSPR_TEMP, 0, 3, 0),
            QXP_SRC(D3DSPR_TEMP, 1),
        D3DSIO_END
    };
    static const DWORD bad_cross_mask_vs[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CRS, 3),
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 2), D3DSIO_END
    };
    static const DWORD bad_cross_swizzle_vs[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CRS, 3),
        QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
        QXP_SRC_SWIZZLE(D3DSPR_TEMP, 1,
                        (0x39UL << D3DSP_SWIZZLE_SHIFT), D3DSPSM_NONE),
        QXP_SRC(D3DSPR_TEMP, 2), D3DSIO_END
    };
    static const DWORD bad_cross_alias_vs[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CRS, 3),
        QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0), QXP_SRC(D3DSPR_TEMP, 1), D3DSIO_END
    };
    static const DWORD bad_dp2add_ps[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DP2ADD, 4),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 2),
        QXP_SRC(D3DSPR_TEMP, 3), D3DSIO_END
    };
    static const DWORD unavailable_ps_opcodes[] = {
        D3DSIO_SLT, D3DSIO_SGE, D3DSIO_LIT, D3DSIO_DST,
        D3DSIO_EXPP, D3DSIO_LOGP
    };
    DWORD i;

    qxp_decode_ok(depth_and_mrt_ps, QXP_ARRAY_COUNT(depth_and_mrt_ps),
                  "ps contiguous MRT and scalar depth output");
    qxp_expect_reject(
        scalar_full_mask_vs, QXP_ARRAY_COUNT(scalar_full_mask_vs),
        QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
        "vs scalar arithmetic requires one destination component");
    qxp_decode_ok(nrm_xyz_ps, QXP_ARRAY_COUNT(nrm_xyz_ps),
                  "nrm permits xyz destination mask");
    qxp_expect_reject(bad_matrix_mask_vs,
                      QXP_ARRAY_COUNT(bad_matrix_mask_vs),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "four-row matrix requires full destination mask");
    qxp_expect_reject(missing_ps_output, QXP_ARRAY_COUNT(missing_ps_output),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "ps_2_0 requires oC0 output");
    qxp_expect_reject(duplicate_ps_output,
                      QXP_ARRAY_COUNT(duplicate_ps_output),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "ps_2_0 color output written once");
    qxp_expect_reject(sparse_ps_output, QXP_ARRAY_COUNT(sparse_ps_output),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "ps_2_0 outputs are contiguous");
    qxp_expect_reject(missing_vs_position,
                      QXP_ARRAY_COUNT(missing_vs_position),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "vs_2_0 requires complete position output");
    qxp_expect_reject(bad_texture_mask_vs,
                      QXP_ARRAY_COUNT(bad_texture_mask_vs),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "vs texture combined mask");
    qxp_expect_reject(skipped_texture_output_vs,
                      QXP_ARRAY_COUNT(skipped_texture_output_vs),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "vs texture output register sequence");
    qxp_expect_reject(bad_scalar_output_mask_vs,
                      QXP_ARRAY_COUNT(bad_scalar_output_mask_vs),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "scalar output carries default destination mask");
    qxp_expect_reject(bad_scalar_output_source_vs,
                      QXP_ARRAY_COUNT(bad_scalar_output_source_vs),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "scalar output requires replicate sources");
    qxp_expect_reject(bad_nrm_mask_ps,
                      QXP_ARRAY_COUNT(bad_nrm_mask_ps),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "nrm destination mask is xyz or xyzw");
    qxp_expect_reject(bad_cross_mask_vs,
                      QXP_ARRAY_COUNT(bad_cross_mask_vs),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "cross-product xyz destination mask");
    qxp_expect_reject(bad_cross_swizzle_vs,
                      QXP_ARRAY_COUNT(bad_cross_swizzle_vs),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "cross-product default source swizzles");
    qxp_expect_reject(bad_cross_alias_vs,
                      QXP_ARRAY_COUNT(bad_cross_alias_vs),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "cross-product destination source alias");
    qxp_expect_reject(bad_dp2add_ps, QXP_ARRAY_COUNT(bad_dp2add_ps),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "dp2add scalar third source");
    for (i = 0; i < QXP_ARRAY_COUNT(unavailable_ps_opcodes); ++i) {
        DWORD shader[3];

        shader[0] = D3DPS_VERSION(2, 0);
        shader[1] = QXP_INST(unavailable_ps_opcodes[i], 0);
        shader[2] = D3DSIO_END;
        qxp_expect_reject(shader, QXP_ARRAY_COUNT(shader),
                          QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                          "reject vertex-only opcode in ps_2_0");
    }
}

static void qxp_test_malformed_setup_and_relative(void)
{
    static const DWORD unknown_sampler[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DCL, 2),
        QXP_PARAMETER | (1UL << D3DSP_TEXTURETYPE_SHIFT),
        QXP_DST(D3DSPR_SAMPLER, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD dcl_wrong_file[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD late_dcl[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_NOP, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD duplicate_dcl[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE), D3DSIO_END
    };
    static const DWORD bad_def_file[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE), 0UL, 0UL, 0UL, 0UL,
        D3DSIO_END
    };
    static const DWORD bad_def_mask[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 0, 7, D3DSPDM_NONE), 0UL, 0UL, 0UL, 0UL,
        D3DSIO_END
    };
    static const DWORD repeated_definitions[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_DEF, 5), QXP_DST(D3DSPR_CONST, 0, 15, 0),
            1UL, 2UL, 3UL, 4UL,
        QXP_INST(D3DSIO_DEF, 5), QXP_DST(D3DSPR_CONST, 0, 15, 0),
            5UL, 6UL, 7UL, 8UL,
        QXP_INST(D3DSIO_DEFI, 5), QXP_DST(D3DSPR_CONSTINT, 0, 15, 0),
            9UL, 10UL, 11UL, 12UL,
        QXP_INST(D3DSIO_DEFI, 5), QXP_DST(D3DSPR_CONSTINT, 0, 15, 0),
            13UL, 14UL, 15UL, 16UL,
        QXP_INST(D3DSIO_DEFB, 2), QXP_DST(D3DSPR_CONSTBOOL, 0, 15, 0),
            0UL,
        QXP_INST(D3DSIO_DEFB, 2), QXP_DST(D3DSPR_CONSTBOOL, 0, 15, 0),
            1UL,
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD ps_defi[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_DEFI, 5),
        QXP_DST(D3DSPR_CONSTINT, 0, 15, 0), 0UL, 0UL, 0UL, 0UL,
        D3DSIO_END
    };
    static const DWORD ps_relative[] = {
        D3DPS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, 0),
        QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0), D3DSIO_END
    };
    static const DWORD wrong_relative_base[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, 0),
        QXP_RELATIVE(D3DSPR_TEMP, 0, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_ADDR, 0, 0), D3DSIO_END
    };
    static const DWORD wrong_relative_file[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, 0),
        QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0), D3DSIO_END
    };
    static const DWORD wrong_relative_swizzle[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 3),
        QXP_DST(D3DSPR_TEMP, 0, 15, 0),
        QXP_RELATIVE(D3DSPR_CONST, 0, D3DSP_NOSWIZZLE),
        QXP_SRC_SWIZZLE(D3DSPR_ADDR, 0,
                        (0x39UL << D3DSP_SWIZZLE_SHIFT), D3DSPSM_NONE),
        D3DSIO_END
    };
    static const DWORD destination_relative[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, 0) | D3DSHADER_ADDRMODE_RELATIVE,
        QXP_SRC(D3DSPR_CONST, 0), D3DSIO_END
    };
    static const DWORD undeclared_input[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
        QXP_SRC(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD undeclared_sampler[] = {
        D3DPS_VERSION(2, 0),
        QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
            QXP_DST(D3DSPR_TEXTURE, 0, 15, 0),
        QXP_INST(D3DSIO_TEX, 3), QXP_DST(D3DSPR_TEMP, 0, 15, 0),
            QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_SAMPLER, 0),
        D3DSIO_END
    };

    qxp_expect_reject(unknown_sampler, QXP_ARRAY_COUNT(unknown_sampler),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "unknown sampler DCL type");
    qxp_expect_reject(dcl_wrong_file, QXP_ARRAY_COUNT(dcl_wrong_file),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "DCL register role");
    qxp_expect_reject(late_dcl, QXP_ARRAY_COUNT(late_dcl),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "DCL after executable instruction");
    qxp_expect_reject(duplicate_dcl, QXP_ARRAY_COUNT(duplicate_dcl),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "duplicate DCL");
    qxp_expect_reject(bad_def_file, QXP_ARRAY_COUNT(bad_def_file),
                      QXP_SHADER_DECODE_REGISTER_ROLE, QXP_ANY_OFFSET,
                      "DEF register role");
    qxp_expect_reject(bad_def_mask, QXP_ARRAY_COUNT(bad_def_mask),
                      QXP_SHADER_DECODE_MODIFIER, QXP_ANY_OFFSET,
                      "DEF full mask form");
    if (qxp_decode_ok(repeated_definitions,
                      QXP_ARRAY_COUNT(repeated_definitions),
                      "repeated DEF DEFI DEFB are ordered last-wins"))
        qxp_check(qxp_ir_a.value.definition_count == 6 &&
                  qxp_ir_a.value.instructions[0].immediate[0] == 1UL &&
                  qxp_ir_a.value.instructions[1].immediate[0] == 5UL &&
                  qxp_ir_a.value.instructions[2].immediate[3] == 12UL &&
                  qxp_ir_a.value.instructions[3].immediate[3] == 16UL &&
                  qxp_ir_a.value.instructions[4].immediate[0] == 0UL &&
                  qxp_ir_a.value.instructions[5].immediate[0] == 1UL,
                  "repeated definition immediates preserve stream order");
    qxp_expect_reject(ps_defi, QXP_ARRAY_COUNT(ps_defi),
                      QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                      "DEFI is not exact ps_2_0");
    qxp_expect_reject(ps_relative, QXP_ARRAY_COUNT(ps_relative),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, QXP_ANY_OFFSET,
                      "pixel relative addressing");
    qxp_expect_reject(wrong_relative_base,
                      QXP_ARRAY_COUNT(wrong_relative_base),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, QXP_ANY_OFFSET,
                      "relative address base register file");
    qxp_expect_reject(wrong_relative_file,
                      QXP_ARRAY_COUNT(wrong_relative_file),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, QXP_ANY_OFFSET,
                      "relative address operand file");
    qxp_expect_reject(wrong_relative_swizzle,
                      QXP_ARRAY_COUNT(wrong_relative_swizzle),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, QXP_ANY_OFFSET,
                      "relative address replicate swizzle");
    qxp_expect_reject(destination_relative,
                      QXP_ARRAY_COUNT(destination_relative),
                      QXP_SHADER_DECODE_RELATIVE_ADDRESS, QXP_ANY_OFFSET,
                      "relative destination in SM2");
    qxp_expect_reject(undeclared_input, QXP_ARRAY_COUNT(undeclared_input),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "VS input requires DCL");
    qxp_expect_reject(undeclared_sampler,
                      QXP_ARRAY_COUNT(undeclared_sampler),
                      QXP_SHADER_DECODE_DECLARATION, QXP_ANY_OFFSET,
                      "texture sampler requires DCL");
}

static void qxp_test_control_rejections(void)
{
    static const DWORD unmatched_else[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_ELSE, 0), D3DSIO_END
    };
    static const DWORD unmatched_end[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_ENDIF, 0), D3DSIO_END
    };
    static const DWORD missing_endif[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_IF, 1),
        QXP_SRC(D3DSPR_CONSTBOOL, 0), D3DSIO_END
    };
    static const DWORD mismatched_loop[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_REP, 1),
        QXP_SRC(D3DSPR_CONSTINT, 0), QXP_INST(D3DSIO_ENDLOOP, 0),
        D3DSIO_END
    };
    static const DWORD nested_loop[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_REP, 1),
        QXP_SRC(D3DSPR_CONSTINT, 0), QXP_INST(D3DSIO_LOOP, 2),
        QXP_SRC(D3DSPR_LOOP, 0), QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_ENDLOOP, 0), QXP_INST(D3DSIO_ENDREP, 0),
        D3DSIO_END
    };
    static const DWORD control_ret[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_IF, 1),
        QXP_SRC(D3DSPR_CONSTBOOL, 0), QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_ENDIF, 0), D3DSIO_END
    };
    static const DWORD duplicate_label[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_RET, 0), QXP_INST(D3DSIO_LABEL, 1),
        QXP_SRC(D3DSPR_LABEL, 0), QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD unresolved_call[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CALL, 1),
        QXP_SRC(D3DSPR_LABEL, 0), QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD label_without_ret[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_LABEL, 1),
        QXP_SRC(D3DSPR_LABEL, 0), QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD label_no_sub_ret[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CALL, 1),
        QXP_SRC(D3DSPR_LABEL, 0), QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_NOP, 0), D3DSIO_END
    };
    static const DWORD label_range[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CALL, 1),
        QXP_SRC(D3DSPR_LABEL, 16), QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD nested_call[] = {
        D3DVS_VERSION(2, 0), QXP_INST(D3DSIO_CALL, 1),
        QXP_SRC(D3DSPR_LABEL, 0), QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 1),
        QXP_INST(D3DSIO_RET, 0), QXP_INST(D3DSIO_LABEL, 1),
        QXP_SRC(D3DSPR_LABEL, 1), QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD unreferenced_label[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_RET, 0), D3DSIO_END
    };
    static const DWORD nested_iteration_call[] = {
        D3DVS_VERSION(2, 0),
        QXP_INST(D3DSIO_LOOP, 2), QXP_SRC(D3DSPR_LOOP, 0),
            QXP_SRC(D3DSPR_CONSTINT, 0),
        QXP_INST(D3DSIO_CALL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_ENDLOOP, 0),
        QXP_INST(D3DSIO_MOV, 2),
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, 0),
            QXP_SRC(D3DSPR_CONST, 0),
        QXP_INST(D3DSIO_RET, 0),
        QXP_INST(D3DSIO_LABEL, 1), QXP_SRC(D3DSPR_LABEL, 0),
        QXP_INST(D3DSIO_REP, 1), QXP_SRC(D3DSPR_CONSTINT, 1),
        QXP_INST(D3DSIO_ENDREP, 0), QXP_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };

    qxp_expect_reject(unmatched_else, QXP_ARRAY_COUNT(unmatched_else),
                      QXP_SHADER_DECODE_UNMATCHED_CONTROL, 1,
                      "unmatched ELSE");
    qxp_expect_reject(unmatched_end, QXP_ARRAY_COUNT(unmatched_end),
                      QXP_SHADER_DECODE_UNMATCHED_CONTROL, 1,
                      "unmatched ENDIF");
    qxp_expect_reject(missing_endif, QXP_ARRAY_COUNT(missing_endif),
                      QXP_SHADER_DECODE_UNMATCHED_CONTROL, 1,
                      "missing ENDIF");
    qxp_expect_reject(mismatched_loop, QXP_ARRAY_COUNT(mismatched_loop),
                      QXP_SHADER_DECODE_UNMATCHED_CONTROL, QXP_ANY_OFFSET,
                      "mismatched loop closer");
    qxp_expect_reject(nested_loop, QXP_ARRAY_COUNT(nested_loop),
                      QXP_SHADER_DECODE_CONTROL, QXP_ANY_OFFSET,
                      "exact vs_2_0 loop nesting depth");
    qxp_expect_reject(control_ret, QXP_ARRAY_COUNT(control_ret),
                      QXP_SHADER_DECODE_UNMATCHED_CONTROL, QXP_ANY_OFFSET,
                      "control cannot cross RET");
    qxp_expect_reject(duplicate_label, QXP_ARRAY_COUNT(duplicate_label),
                      QXP_SHADER_DECODE_DUPLICATE_LABEL, QXP_ANY_OFFSET,
                      "duplicate label");
    qxp_expect_reject(unresolved_call, QXP_ARRAY_COUNT(unresolved_call),
                      QXP_SHADER_DECODE_MISSING_LABEL, 1,
                      "unresolved call label");
    qxp_expect_reject(label_without_ret, QXP_ARRAY_COUNT(label_without_ret),
                      QXP_SHADER_DECODE_CALL_TARGET, 1,
                      "label must directly follow RET");
    qxp_expect_reject(label_no_sub_ret, QXP_ARRAY_COUNT(label_no_sub_ret),
                      QXP_SHADER_DECODE_CALL_TARGET, QXP_ANY_OFFSET,
                      "subroutine requires terminal RET");
    qxp_expect_reject(label_range, QXP_ARRAY_COUNT(label_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, QXP_ANY_OFFSET,
                      "exact vs_2_0 label range");
    qxp_expect_reject(nested_call, QXP_ARRAY_COUNT(nested_call),
                      QXP_SHADER_DECODE_CALL_TARGET, QXP_ANY_OFFSET,
                      "exact vs_2_0 call nesting and recursion boundary");
    qxp_expect_reject(unreferenced_label,
                      QXP_ARRAY_COUNT(unreferenced_label),
                      QXP_SHADER_DECODE_CALL_TARGET, QXP_ANY_OFFSET,
                      "every subroutine label is referenced");
    qxp_expect_reject(nested_iteration_call,
                      QXP_ARRAY_COUNT(nested_iteration_call),
                      QXP_SHADER_DECODE_CONTROL, QXP_ANY_OFFSET,
                      "loop nesting across a subroutine call");

    {
        DWORD cursor;
        DWORD i;

        cursor = 0;
        qxp_work_tokens[cursor++] = D3DVS_VERSION(2, 0);
        for (i = 0; i < 17; ++i) {
            qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_IF, 1);
            qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONSTBOOL, 0);
        }
        for (i = 0; i < 17; ++i)
            qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_ENDIF, 0);
        qxp_work_tokens[cursor++] = D3DSIO_END;
        qxp_expect_reject(qxp_work_tokens, cursor,
                          QXP_SHADER_DECODE_CONTROL, QXP_ANY_OFFSET,
                          "exact vs_2_0 static flow count");
    }
}

static DWORD qxp_build_nops(DWORD version, DWORD instruction_count)
{
    DWORD i;
    DWORD cursor;
    BOOL vertex;

    cursor = 0;
    vertex = version == D3DVS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = version;
    for (i = 1; i < instruction_count; ++i)
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_NOP, 0);
    if (instruction_count) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
        qxp_work_tokens[cursor++] = vertex ?
            QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE) :
            QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    }
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static DWORD qxp_build_texture_limit(DWORD texture_count,
                                     DWORD sampler_type)
{
    DWORD i;
    DWORD cursor;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEXTURE, 0, 15, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_DCL_SAMPLER(sampler_type);
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_SAMPLER, 0, 15, 0);
    for (i = 0; i < texture_count; ++i) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEX, 3);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, i % QXP_SHADER_MAX_PS_TEMPS, 15, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_SAMPLER, 0);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] =
        QXP_SRC(D3DSPR_TEMP,
                (texture_count - 1) % QXP_SHADER_MAX_PS_TEMPS);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static DWORD qxp_build_texture_dependency(DWORD dependency_order,
                                           BOOL append_texkill)
{
    DWORD cursor;
    DWORD i;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEXTURE, 0, 15, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_DCL_SAMPLER(D3DSTT_2D);
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_SAMPLER, 0, 15, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEMP, 0, 15, 0);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
    for (i = 0; i < dependency_order; ++i) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
        qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEMP, i + 1, 15, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, i);
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEX, 3);
        qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEMP, i + 1, 15, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEMP, i + 1);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_SAMPLER, 0);
    }
    if (append_texkill) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEXKILL, 1);
        qxp_work_tokens[cursor++] =
            QXP_DST(D3DSPR_TEMP, dependency_order, 15, 0);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static DWORD qxp_build_ps_texture_slot_case(
    DWORD arithmetic_slots, DWORD sampler_type, DWORD texture_control,
    BOOL texkill, BOOL arbitrary_swizzle)
{
    DWORD cursor;
    DWORD i;

    cursor = 0;
    qxp_work_tokens[cursor++] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_PARAMETER;
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEXTURE, 0, 15, 0);
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_DCL, 2);
    qxp_work_tokens[cursor++] = QXP_DCL_SAMPLER(sampler_type);
    qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_SAMPLER, 0, 15, 0);
    for (i = 1; i < arithmetic_slots; ++i) {
        if (arbitrary_swizzle && i == 1) {
            qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
            qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEMP, 1, 15, 0);
            qxp_work_tokens[cursor++] =
                QXP_SRC_SWIZZLE(D3DSPR_CONST, 0,
                                (0x39UL << D3DSP_SWIZZLE_SHIFT),
                                D3DSPSM_NONE);
        } else {
            qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_NOP, 0);
        }
    }
    if (texkill) {
        qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_TEXKILL, 1);
        qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEXTURE, 0, 15, 0);
    } else {
        qxp_work_tokens[cursor++] =
            QXP_INST(D3DSIO_TEX, 3) | texture_control;
        qxp_work_tokens[cursor++] = QXP_DST(D3DSPR_TEMP, 0, 15, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_TEXTURE, 0);
        qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_SAMPLER, 0);
    }
    qxp_work_tokens[cursor++] = QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[cursor++] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[cursor++] = QXP_SRC(D3DSPR_CONST, 0);
    qxp_work_tokens[cursor++] = D3DSIO_END;
    return cursor;
}

static void qxp_check_ps_texture_slot_case(
    DWORD arithmetic_slots, DWORD sampler_type, DWORD texture_control,
    BOOL texkill, BOOL arbitrary_swizzle, DWORD expected_caps,
    DWORD expected_required_slots, const char *name)
{
    DWORD count;

    count = qxp_build_ps_texture_slot_case(
        arithmetic_slots, sampler_type, texture_control, texkill,
        arbitrary_swizzle);
    if (qxp_decode_ok(qxp_work_tokens, count, name))
        qxp_check(qxp_ir_a.value.arithmetic_slot_count == arithmetic_slots &&
                  qxp_ir_a.value.texture_slot_count == 1 &&
                  qxp_ir_a.value.slot_count == arithmetic_slots + 1 &&
                  qxp_ir_a.value.required_ps20_caps == expected_caps &&
                  qxp_ir_a.value.required_ps_instruction_slots ==
                      expected_required_slots,
                  name);
}

static void qxp_test_limits(void)
{
    DWORD count;
    DWORD dependency_caps;
    DWORD i;

    count = qxp_build_nops(D3DPS_VERSION(2, 0), 64);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps baseline arithmetic slot boundary"))
        qxp_check(qxp_ir_a.value.required_ps20_caps == 0 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 64,
                  "ps baseline arithmetic slot metadata");
    count = qxp_build_nops(D3DPS_VERSION(2, 0), 65);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps arithmetic above 64 requests NOTEX"))
        qxp_check(qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT &&
                  qxp_ir_a.value.required_ps_instruction_slots == 65,
                  "ps arithmetic NOTEX exact requirement metadata");

    qxp_check_ps_texture_slot_case(
        64, D3DSTT_CUBE, 0, FALSE, FALSE, 0, 65,
        "base cube texld remains one slot without NOTEX");
    qxp_check_ps_texture_slot_case(
        64, D3DSTT_CUBE, 0, FALSE, TRUE,
        QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE, 65,
        "arbitrary-swizzle cap does not switch cube texld cost");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_2D, 0, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 66,
        "NOTEX 2D texld costs one slot");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_CUBE, 0, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 69,
        "NOTEX cube texld costs four slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_VOLUME, 0, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 66,
        "NOTEX volume texld costs one slot");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_2D, D3DSI_TEXLD_PROJECT, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 68,
        "NOTEX 2D texldp costs three slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_CUBE, D3DSI_TEXLD_PROJECT, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 69,
        "NOTEX cube texldp costs four slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_VOLUME, D3DSI_TEXLD_PROJECT, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 68,
        "NOTEX volume texldp costs three slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_2D, D3DSI_TEXLD_BIAS, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 71,
        "NOTEX 2D texldb costs six slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_CUBE, D3DSI_TEXLD_BIAS, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 71,
        "NOTEX cube texldb costs six slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_VOLUME, D3DSI_TEXLD_BIAS, FALSE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 71,
        "NOTEX volume texldb costs six slots");
    qxp_check_ps_texture_slot_case(
        65, D3DSTT_2D, 0, TRUE, FALSE,
        QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT, 67,
        "NOTEX texkill costs two slots");

    count = qxp_build_texture_limit(32, D3DSTT_2D);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps baseline texture slot boundary"))
        qxp_check(qxp_ir_a.value.required_ps20_caps == 0 &&
                  qxp_ir_a.value.required_ps_temp_count == 32 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 33,
                  "baseline texture requirement metadata");
    count = qxp_build_texture_limit(33, D3DSTT_2D);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps texture count above baseline needs a cap"))
        qxp_check(qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT &&
                  qxp_ir_a.value.required_ps_temp_count == 32 &&
                  qxp_ir_a.value.texture_slot_count == 33 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 34,
                  "unlimited texture exact requirement metadata");
    count = qxp_build_texture_limit(33, D3DSTT_CUBE);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps cube texture boundary recomputes all slots"))
        qxp_check(qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT &&
                  qxp_ir_a.value.texture_slot_count == 33 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 133,
                  "NOTEX retroactively costs every cube texld");
    count = qxp_build_texture_limit(95, D3DSTT_2D);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps exact 96 required-slot total"))
        qxp_check(qxp_ir_a.value.required_ps_instruction_slots == 96,
                  "ps exact 96 required-slot metadata");
    count = qxp_build_texture_limit(96, D3DSTT_2D);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps required slots above 96 are capability metadata"))
        qxp_check(qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT &&
                  qxp_ir_a.value.required_ps_instruction_slots == 97,
                  "ps required slots replace hard 96 rejection");
    dependency_caps = 0;
    count = qxp_build_texture_dependency(3, FALSE);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps third-order dependent texture boundary")) {
        dependency_caps = qxp_ir_a.value.required_ps20_caps;
        qxp_check(dependency_caps == 0 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 8,
                  "baseline dependent-read requirement metadata");
    }
    count = qxp_build_texture_dependency(3, TRUE);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps third-order dependency followed by texkill"))
        qxp_check(qxp_ir_a.value.required_ps20_caps == dependency_caps &&
                  (qxp_ir_a.value.required_ps20_caps &
                   QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT) == 0 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 9,
                  "texkill does not increase dependent-read order");
    count = qxp_build_texture_dependency(4, FALSE);
    if (qxp_decode_ok(qxp_work_tokens, count,
                      "ps deeper dependent reads need a cap"))
        qxp_check(qxp_ir_a.value.required_ps20_caps ==
                      QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT &&
                  qxp_ir_a.value.required_ps_temp_count == 5 &&
                  qxp_ir_a.value.required_ps_instruction_slots == 10,
                  "dependent-read cap does not switch texture slot costs");
    count = qxp_build_nops(D3DVS_VERSION(2, 0), 256);
    qxp_decode_ok(qxp_work_tokens, count, "vs instruction slot boundary");
    count = qxp_build_nops(D3DVS_VERSION(2, 0), 257);
    qxp_expect_reject(qxp_work_tokens, count, QXP_SHADER_DECODE_SLOT_LIMIT,
                      QXP_ANY_OFFSET, "vs instruction slot overflow");
    count = qxp_build_nops(D3DVS_VERSION(2, 0), 513);
    qxp_expect_reject(qxp_work_tokens, count,
                      QXP_SHADER_DECODE_INSTRUCTION_CAPACITY,
                      QXP_ANY_OFFSET,
                      "fixed instruction-storage resource exhaustion");

    qxp_expect_reject(qxp_minimal_ps, QXP_SHADER_MAX_TOKENS + 1,
                      QXP_SHADER_DECODE_TOKEN_COUNT,
                      QXP_SHADER_MAX_TOKENS,
                      "fixed token-storage resource exhaustion");

    qxp_work_tokens[0] = D3DPS_VERSION(2, 0);
    qxp_work_tokens[1] = D3DSHADER_COMMENT(QXP_SHADER_MAX_TOKENS - 6);
    for (i = 2; i < QXP_SHADER_MAX_TOKENS - 4; ++i)
        qxp_work_tokens[i] = i * 2654435761UL;
    qxp_work_tokens[QXP_SHADER_MAX_TOKENS - 4] =
        QXP_INST(D3DSIO_MOV, 2);
    qxp_work_tokens[QXP_SHADER_MAX_TOKENS - 3] =
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE);
    qxp_work_tokens[QXP_SHADER_MAX_TOKENS - 2] =
        QXP_SRC(D3DSPR_CONST, 0);
    qxp_work_tokens[QXP_SHADER_MAX_TOKENS - 1] = D3DSIO_END;
    qxp_decode_ok(qxp_work_tokens, QXP_SHADER_MAX_TOKENS,
                  "exact 64 KiB input boundary");
}

static DWORD qxp_random(DWORD *state)
{
    *state = *state * 1664525UL + 1013904223UL;
    return *state;
}

static void qxp_test_mutation_properties(void)
{
    DWORD seed;
    DWORD iteration;

    seed = 0x6d2b79f5UL;
    for (iteration = 0; iteration < 1200; ++iteration) {
        const DWORD *base;
        DWORD base_count;
        DWORD count;
        DWORD changes;
        DWORD i;
        BOOL result_a;
        BOOL result_b;
        int ok;

        if (qxp_random(&seed) & 1UL) {
            base = qxp_texture_ps;
            base_count = QXP_ARRAY_COUNT(qxp_texture_ps);
        } else {
            base = qxp_control_vs;
            base_count = QXP_ARRAY_COUNT(qxp_control_vs);
        }
        qxp_copy_tokens(qxp_mutation_tokens, base, base_count);
        count = base_count;
        changes = 1 + (qxp_random(&seed) % 3);
        for (i = 0; i < changes; ++i) {
            DWORD index;
            DWORD bit;

            index = qxp_random(&seed) % count;
            bit = qxp_random(&seed) & 31UL;
            qxp_mutation_tokens[index] ^= 1UL << bit;
        }
        if ((qxp_random(&seed) & 7UL) == 0)
            count = qxp_random(&seed) % (base_count + 1);
        else if ((qxp_random(&seed) & 15UL) == 0 && count < 511) {
            qxp_mutation_tokens[count++] = qxp_random(&seed);
        }

        qxp_prepare_ir(&qxp_ir_a);
        qxp_prepare_ir(&qxp_ir_b);
        qxp_prepare_error(&qxp_error_a);
        qxp_prepare_error(&qxp_error_b);
        result_a = QxpDecodeShader20(qxp_mutation_tokens, count,
                                     &qxp_ir_a.value, &qxp_error_a.value);
        result_b = QxpDecodeShader20(qxp_mutation_tokens, count,
                                     &qxp_ir_b.value, &qxp_error_b.value);
        ok = result_a == result_b &&
             memcmp(&qxp_ir_a.value, &qxp_ir_b.value,
                    sizeof(qxp_ir_a.value)) == 0 &&
             memcmp(&qxp_error_a.value, &qxp_error_b.value,
                    sizeof(qxp_error_a.value)) == 0 &&
             qxp_ir_canaries_ok(&qxp_ir_a) &&
             qxp_ir_canaries_ok(&qxp_ir_b) &&
             qxp_error_canaries_ok(&qxp_error_a) &&
             qxp_error_canaries_ok(&qxp_error_b);
        if (result_a)
            ok = ok && qxp_error_a.value.code == QXP_SHADER_DECODE_OK &&
                 qxp_validate_ir_independently(&qxp_ir_a.value, count);
        else
            ok = ok && qxp_error_a.value.code != QXP_SHADER_DECODE_OK &&
                 memcmp(&qxp_ir_a.value, &qxp_zero_ir,
                        sizeof(qxp_ir_a.value)) == 0;
        if (!ok) {
            qxp_check(0, "fixed-seed mutation determinism and invariants");
            break;
        }
    }
}

static void qxp_test_argument_contract(void)
{
    QXP_SHADER_DECODE_ERROR error;
    BOOL result;

    memset(&error, 0xff, sizeof(error));
    result = QxpDecodeShader20(qxp_minimal_ps,
                               QXP_ARRAY_COUNT(qxp_minimal_ps),
                               NULL, &error);
    qxp_check(!result && error.code == QXP_SHADER_DECODE_NULL_ARGUMENT &&
              error.token_offset == 0, "NULL IR argument");

    qxp_prepare_ir(&qxp_ir_a);
    result = QxpDecodeShader20(qxp_minimal_ps,
                               QXP_ARRAY_COUNT(qxp_minimal_ps),
                               &qxp_ir_a.value, NULL);
    qxp_check(!result && qxp_ir_canaries_ok(&qxp_ir_a) &&
              memcmp(&qxp_ir_a.value, &qxp_zero_ir,
                     sizeof(qxp_ir_a.value)) == 0,
              "NULL error argument still clears IR");

    qxp_prepare_ir(&qxp_ir_a);
    qxp_prepare_error(&qxp_error_a);
    result = QxpDecodeShader20(NULL, 1, &qxp_ir_a.value,
                               &qxp_error_a.value);
    qxp_check(!result &&
              qxp_error_a.value.code == QXP_SHADER_DECODE_NULL_ARGUMENT &&
              qxp_ir_canaries_ok(&qxp_ir_a) &&
              qxp_error_canaries_ok(&qxp_error_a) &&
              memcmp(&qxp_ir_a.value, &qxp_zero_ir,
                     sizeof(qxp_ir_a.value)) == 0,
              "NULL token argument");
}

/*
 * Shader Model 1 streams.  Every instruction token below deliberately has a
 * zero length field, which is what distinguishes real vs_1_1 bytecode from
 * vs_2_0 and what the decoder must reconstruct from each opcode's schema.
 * The first shader is the exact shape Prince of Persia: The Sands of Time
 * compiles for its stream setup: four usage declarations, then arithmetic.
 */
static const DWORD qxp_pop_vs11[] = {
    QXP_SHADER_VERSION_VS_1_1,
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_NORMAL, 0),
        QXP_DST(D3DSPR_INPUT, 2, 15, D3DSPDM_NONE),
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_COLOR, 0),
        QXP_DST(D3DSPR_INPUT, 3, 15, D3DSPDM_NONE),
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 0),
        QXP_DST(D3DSPR_INPUT, 9, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 2),
    D3DSIO_DP3, QXP_DST(D3DSPR_TEMP, 2, 8, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1), QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_MOV, QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MOV, QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 3),
    D3DSIO_END
};

/* vs_1_1 relative addressing: the bit alone means a0.x, and no address
 * token follows it.  The same source in vs_2_0 spells out a second token. */
static const DWORD qxp_relative_vs11[] = {
    QXP_SHADER_VERSION_VS_1_1,
    D3DSIO_DCL, QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MOV, QXP_DST(D3DSPR_ADDR, 0, 1, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEMP, 0, 0),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_RELATIVE(D3DSPR_CONST, 3, D3DSP_NOSWIZZLE),
    D3DSIO_MOV, QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_END
};

/*
 * The exact first pixel shader Prince of Persia: The Sands of Time binds,
 * captured from the guest: twelve tokens, a `tex t0` whose sampler is
 * implicit in the destination register, a `_x2`-shifted vector multiply,
 * and its co-issued scalar half.  Every instruction token carries the SM1
 * zero length field, exactly like real ps_1_1 bytecode.
 */
static const DWORD qxp_pop_ps11[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 7,
                        1UL << D3DSP_DSTSHIFT_SHIFT),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_MUL | D3DSI_COISSUE, QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* All four ps_1_1 texture stages sampled, then combined. */
static const DWORD qxp_ps11_multi_sampler[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 2, 15, D3DSPDM_NONE),
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 3, 15, D3DSPDM_NONE),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0), QXP_SRC(D3DSPR_TEXTURE, 1),
    D3DSIO_MAD, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 2), QXP_SRC(D3DSPR_TEXTURE, 3),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

/* A co-issue marker on the first instruction has nothing to pair with. */
static const DWORD qxp_ps11_first_coissue[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_MUL | D3DSI_COISSUE, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_INPUT, 1),
    D3DSIO_END
};

/* ps_1_x source modifiers beyond negate are not lowered; reject _bx2. */
static const DWORD qxp_ps11_bx2_source[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC_SWIZZLE(D3DSPR_INPUT, 0, D3DSP_NOSWIZZLE, D3DSPSM_BIASNEG),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* The ps_1_x texture-addressing family stays unsupported. */
static const DWORD qxp_ps11_texbem[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEXBEM, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* Sands of Time uses texcoord to feed a stage coordinate in as colour.  It
 * keeps its own IR opcode so the emitter never mistakes it for a sample.
 * texkill stays rejected: no title here needs it and nothing qualifies it. */
static const DWORD qxp_ps11_texcoord[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEXCOORD, QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 1),
    D3DSIO_END
};

/* r1 alone is not a result; ps_1_1 returns its colour in r0. */
static const DWORD qxp_ps11_no_r0_write[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* ps_1_1 register bounds: r0-r1, c0-c7, t0-t3. */
static const DWORD qxp_ps11_temp_range[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_ps11_const_range[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 8),
    D3DSIO_END
};

static const DWORD qxp_ps11_texture_range[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 4, 15, D3DSPDM_NONE),
    D3DSIO_END
};

/* Need for Speed: Underground 2 binds `mad` destinations carrying the
 * _x4 result shift together with _sat (r542 guest capture).  ps_1_1
 * admits exactly _x2 and _x4; _x8 is ps_1_4, and the divide result
 * modifiers stay deliberately unsupported fail-closed. */
static const DWORD qxp_ps11_nfsu2_mad_x4_sat[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MAD, QXP_DST(D3DSPR_TEMP, 0, 7,
                        D3DSPDM_SATURATE |
                        ((DWORD)2 << D3DSP_DSTSHIFT_SHIFT)),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD qxp_ps11_unknown_shift[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 7,
                        (DWORD)4 << D3DSP_DSTSHIFT_SHIFT),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

static const DWORD qxp_ps11_x8_shift[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 7,
                        D3DSPDM_SATURATE |
                        ((DWORD)3 << D3DSP_DSTSHIFT_SHIFT)),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

/* Exact second Need for Speed: Underground 2 pixel shader from the
 * r543 guest capture: texreg2ar/texreg2gb dependent reads feed an
 * add/mul/lrp chain, and the final mad carries the _comp source
 * modifier.  Every form here previously refused CREATEPIXELSHADER. */
static const DWORD qxp_ps11_nfsu2_texreg[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_DEF, QXP_DST(D3DSPR_CONST, 0, 15, D3DSPDM_NONE),
        0x3f800000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEXREG2AR, QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_TEXREG2GB, QXP_DST(D3DSPR_TEXTURE, 2, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_ADD, QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEXTURE, 1, 3),
        QXP_SCALAR(D3DSPR_CONST, 0, 3),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 1, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 2), QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEXTURE, 2, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 1), QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_LRP, QXP_DST(D3DSPR_TEMP, 1, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_CONST, 0), QXP_SRC(D3DSPR_TEXTURE, 2),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_MUL | D3DSI_COISSUE,
        QXP_DST(D3DSPR_TEMP, 1, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_TEXTURE, 1, 3),
        QXP_SCALAR(D3DSPR_TEMP, 0, 3),
    D3DSIO_MAD, QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
        QXP_SRC_SWIZZLE(D3DSPR_TEMP, 0, QXP_REPLICATE(3),
                        D3DSPSM_COMP),
        QXP_SRC(D3DSPR_TEMP, 1),
    D3DSIO_END
};

/* The texreg2* source stage must hold a texture-address result from a
 * previous tex/texcoord/texreg2*; stage 3 here was never addressed. */
static const DWORD qxp_ps11_texreg_unaddressed[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_TEXREG2AR, QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 3),
    D3DSIO_END
};

/* Texture-address forms never pair in a co-issue slot: the dependent-read
 * tracker is sequential, so a parallel pair could read its partner's
 * write. */
static const DWORD qxp_ps11_texreg_coissue[] = {
    QXP_SHADER_VERSION_PS_1_1,
    D3DSIO_TEX, QXP_DST(D3DSPR_TEXTURE, 0, 15, D3DSPDM_NONE),
    D3DSIO_MUL, QXP_DST(D3DSPR_TEMP, 0, 7, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0), QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_MUL | D3DSI_COISSUE,
        QXP_DST(D3DSPR_TEMP, 0, 8, D3DSPDM_NONE),
        QXP_SCALAR(D3DSPR_INPUT, 1, 3), QXP_SCALAR(D3DSPR_CONST, 0, 3),
    D3DSIO_TEXREG2AR | D3DSI_COISSUE,
        QXP_DST(D3DSPR_TEXTURE, 1, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEXTURE, 0),
    D3DSIO_END
};

static void qxp_test_ps11_streams(void)
{
    const QXP_SHADER_INSTRUCTION *instruction;

    if (qxp_decode_ok(qxp_pop_ps11, QXP_ARRAY_COUNT(qxp_pop_ps11),
                      "ps_1_1 first Prince of Persia shader")) {
        qxp_check(qxp_ir_a.value.stage == QXP_SHADER_STAGE_PIXEL &&
                  qxp_ir_a.value.version_token ==
                      QXP_SHADER_VERSION_PS_1_1 &&
                  qxp_ir_a.value.token_count == 12 &&
                  qxp_ir_a.value.declaration_count == 0 &&
                  qxp_ir_a.value.instruction_count == 3,
                  "ps_1_1 stream shape");
        instruction = &qxp_ir_a.value.instructions[0];
        qxp_check(instruction->opcode == QXP_SHADER_OP_TEX &&
                  instruction->destination_count == 1 &&
                  instruction->source_count == 0 &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_TEXTURE &&
                  instruction->destinations[0].register_number == 0 &&
                  instruction->destinations[0].write_mask == 15 &&
                  instruction->destinations[0].destination_shift == 0 &&
                  instruction->slot_class == QXP_SHADER_SLOT_TEXTURE,
                  "ps_1_1 tex names its sampler by destination register");
        instruction = &qxp_ir_a.value.instructions[1];
        qxp_check(instruction->opcode == QXP_SHADER_OP_MUL &&
                  instruction->destinations[0].destination_shift == 1 &&
                  instruction->destinations[0].write_mask == 7 &&
                  instruction->token_count == 4,
                  "ps_1_1 _x2 destination shift preserved");
        instruction = &qxp_ir_a.value.instructions[2];
        qxp_check(instruction->opcode == QXP_SHADER_OP_MUL &&
                  instruction->token_offset == 7 &&
                  instruction->destinations[0].write_mask == 8,
                  "ps_1_1 co-issued scalar half decodes sequentially");
    }

    if (qxp_decode_ok(qxp_ps11_multi_sampler,
                      QXP_ARRAY_COUNT(qxp_ps11_multi_sampler),
                      "ps_1_1 four-stage multi-sampler shader"))
        qxp_check(qxp_ir_a.value.instruction_count == 6 &&
                  qxp_ir_a.value.texture_slot_count == 4 &&
                  qxp_ir_a.value.arithmetic_slot_count == 2 &&
                  qxp_ir_a.value.required_ps_temp_count == 1 &&
                  qxp_ir_a.value.required_ps20_caps == 0,
                  "ps_1_1 multi-sampler stream shape");

    if (qxp_decode_ok(qxp_ps11_texcoord,
                      QXP_ARRAY_COUNT(qxp_ps11_texcoord),
                      "ps_1_1 texcoord coordinate passthrough")) {
        instruction = &qxp_ir_a.value.instructions[0];
        qxp_check(instruction->opcode == QXP_SHADER_OP_TEXCOORD &&
                  instruction->opcode != QXP_SHADER_OP_TEX &&
                  instruction->destination_count == 1 &&
                  instruction->source_count == 0 &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_TEXTURE &&
                  instruction->destinations[0].register_number == 1,
                  "ps_1_1 texcoord keeps its own IR opcode");
    }

    qxp_expect_reject(qxp_ps11_first_coissue,
                      QXP_ARRAY_COUNT(qxp_ps11_first_coissue),
                      QXP_SHADER_DECODE_CONTROL, 1,
                      "ps_1_1 co-issue needs a preceding instruction");
    qxp_expect_reject(qxp_ps11_bx2_source,
                      QXP_ARRAY_COUNT(qxp_ps11_bx2_source),
                      QXP_SHADER_DECODE_MODIFIER, 3,
                      "ps_1_1 _bx2 source modifier rejected");
    qxp_expect_reject(qxp_ps11_texbem, QXP_ARRAY_COUNT(qxp_ps11_texbem),
                      QXP_SHADER_DECODE_UNSUPPORTED_OPCODE, 1,
                      "ps_1_1 texbem texture addressing rejected");
    qxp_expect_reject(qxp_ps11_no_r0_write,
                      QXP_ARRAY_COUNT(qxp_ps11_no_r0_write),
                      QXP_SHADER_DECODE_REGISTER_ROLE, 7,
                      "ps_1_1 requires r0 output");
    qxp_expect_reject(qxp_ps11_temp_range,
                      QXP_ARRAY_COUNT(qxp_ps11_temp_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, 1,
                      "ps_1_1 temporary range is r0-r1");
    qxp_expect_reject(qxp_ps11_const_range,
                      QXP_ARRAY_COUNT(qxp_ps11_const_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, 1,
                      "ps_1_1 constant range is c0-c7");
    qxp_expect_reject(qxp_ps11_texture_range,
                      QXP_ARRAY_COUNT(qxp_ps11_texture_range),
                      QXP_SHADER_DECODE_REGISTER_RANGE, 1,
                      "ps_1_1 texture range is t0-t3");

    if (qxp_decode_ok(qxp_ps11_nfsu2_mad_x4_sat,
                      QXP_ARRAY_COUNT(qxp_ps11_nfsu2_mad_x4_sat),
                      "ps_1_1 NFSU2 mad_sat _x4 shader")) {
        instruction = &qxp_ir_a.value.instructions[2];
        qxp_check(instruction->opcode == QXP_SHADER_OP_MAD &&
                  instruction->source_count == 3 &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_TEMP &&
                  instruction->destinations[0].write_mask == 7 &&
                  instruction->destinations[0].destination_modifier ==
                      QXP_SHADER_DSTMOD_SATURATE &&
                  instruction->destinations[0].destination_shift == 2,
                  "ps_1_1 _sat _x4 destination modifiers preserved");
        qxp_check(qxp_ir_a.value.texture_slot_count == 1 &&
                  qxp_ir_a.value.arithmetic_slot_count == 2,
                  "ps_1_1 NFSU2 stream slot shape");
    }

    qxp_expect_reject(qxp_ps11_unknown_shift,
                      QXP_ARRAY_COUNT(qxp_ps11_unknown_shift),
                      QXP_SHADER_DECODE_MODIFIER, 6,
                      "ps_1_1 divide/reserved shift encoding rejected");
    qxp_expect_reject(qxp_ps11_x8_shift,
                      QXP_ARRAY_COUNT(qxp_ps11_x8_shift),
                      QXP_SHADER_DECODE_MODIFIER, 6,
                      "ps_1_1 rejects the ps_1_4-only _x8 shift");

    if (qxp_decode_ok(qxp_ps11_nfsu2_texreg,
                      QXP_ARRAY_COUNT(qxp_ps11_nfsu2_texreg),
                      "ps_1_1 NFSU2 texreg dependent-read shader")) {
        instruction = &qxp_ir_a.value.instructions[2];
        qxp_check(instruction->opcode == QXP_SHADER_OP_TEXREG2AR &&
                  instruction->source_count == 1 &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_TEXTURE &&
                  instruction->destinations[0].register_number == 1 &&
                  instruction->sources[0].register_file ==
                      QXP_SHADER_REG_TEXTURE,
                  "ps_1_1 texreg2ar decodes as a one-source texture form");
        instruction = &qxp_ir_a.value.instructions[3];
        qxp_check(instruction->opcode == QXP_SHADER_OP_TEXREG2GB &&
                  instruction->slot_class == QXP_SHADER_SLOT_TEXTURE,
                  "ps_1_1 texreg2gb keeps its own opcode and texture slot");
        qxp_check(qxp_ir_a.value.instruction_count == 10 &&
                  qxp_ir_a.value.texture_slot_count == 3 &&
                  qxp_ir_a.value.arithmetic_slot_count == 6 &&
                  qxp_ir_a.value.required_ps_temp_count == 2 &&
                  qxp_ir_a.value.required_ps20_caps == 0,
                  "ps_1_1 NFSU2 texreg stream shape");
        instruction = &qxp_ir_a.value.instructions[9];
        qxp_check(instruction->opcode == QXP_SHADER_OP_MAD &&
                  instruction->sources[1].source_modifier ==
                      QXP_SHADER_SRCMOD_COMP,
                  "ps_1_1 _comp source modifier preserved");
    }

    qxp_expect_reject(qxp_ps11_texreg_unaddressed,
                      QXP_ARRAY_COUNT(qxp_ps11_texreg_unaddressed),
                      QXP_SHADER_DECODE_UNINITIALIZED, 3,
                      "ps_1_1 texreg source must be texture-addressed");
    qxp_expect_reject(qxp_ps11_texreg_coissue,
                      QXP_ARRAY_COUNT(qxp_ps11_texreg_coissue),
                      QXP_SHADER_DECODE_CONTROL, 11,
                      "ps_1_1 texreg never co-issues");

    /* Regression: ps_2_0 texld keeps its explicit coordinate and sampler
     * sources; only the ps_1_1 stream gets the zero-source tex form. */
    if (qxp_decode_ok(qxp_texture_ps, QXP_ARRAY_COUNT(qxp_texture_ps),
                      "ps_2_0 texld form unchanged")) {
        instruction = &qxp_ir_a.value.instructions[2];
        qxp_check(instruction->opcode == QXP_SHADER_OP_TEX &&
                  instruction->destination_count == 1 &&
                  instruction->source_count == 2 &&
                  instruction->sources[1].register_file ==
                      QXP_SHADER_REG_SAMPLER,
                  "ps_2_0 texld still carries two sources");
    }
}

static void qxp_test_sm1_streams(void)
{
    const QXP_SHADER_INSTRUCTION *instruction;
    DWORD tokens[QXP_ARRAY_COUNT(qxp_pop_vs11)];

    if (qxp_decode_ok(qxp_pop_vs11, QXP_ARRAY_COUNT(qxp_pop_vs11),
                      "vs_1_1 with usage declarations")) {
        qxp_check(qxp_ir_a.value.stage == QXP_SHADER_STAGE_VERTEX &&
                  qxp_ir_a.value.version_token ==
                      QXP_SHADER_VERSION_VS_1_1 &&
                  qxp_ir_a.value.declaration_count == 4 &&
                  qxp_ir_a.value.instruction_count == 8,
                  "vs_1_1 stream shape");
        instruction = &qxp_ir_a.value.instructions[3];
        qxp_check(instruction->declaration.present &&
                  instruction->declaration.usage == D3DDECLUSAGE_TEXCOORD &&
                  instruction->destinations[0].register_file ==
                      QXP_SHADER_REG_INPUT &&
                  instruction->destinations[0].register_number == 9,
                  "vs_1_1 declaration binds usage to its input register");
        instruction = &qxp_ir_a.value.instructions[5];
        qxp_check(instruction->opcode == QXP_SHADER_OP_DP3 &&
                  instruction->source_count == 2 &&
                  instruction->token_count == 4,
                  "vs_1_1 instruction length derived from the schema");
    }

    if (qxp_decode_ok(qxp_relative_vs11,
                      QXP_ARRAY_COUNT(qxp_relative_vs11),
                      "vs_1_1 relative addressing")) {
        instruction = &qxp_ir_a.value.instructions[3];
        qxp_check(instruction->sources[0].relative.present &&
                  instruction->sources[0].relative.register_file ==
                      QXP_SHADER_REG_ADDRESS &&
                  instruction->sources[0].relative.register_number == 0 &&
                  instruction->sources[0].relative.swizzle[0] == 0 &&
                  instruction->token_count == 3,
                  "vs_1_1 relative source consumes no address token");
    }

    /* The same bytecode declared as vs_2_0 must fail: vs_2_0 requires the
     * length field the SM1 encoding leaves zero, so nothing silently
     * reinterprets one profile's stream as the other's. */
    memcpy(tokens, qxp_pop_vs11, sizeof(qxp_pop_vs11));
    tokens[0] = QXP_SHADER_VERSION_VS_2_0;
    qxp_expect_reject(tokens, QXP_ARRAY_COUNT(qxp_pop_vs11),
                    QXP_SHADER_DECODE_INSTRUCTION_LENGTH, QXP_ANY_OFFSET,
                    "SM1 encoding rejected when declared vs_2_0");

    /* ps_1_x remains undecodable; only vs_1_1 joined the accepted set. */
    memcpy(tokens, qxp_pop_vs11, sizeof(qxp_pop_vs11));
    tokens[0] = 0xffff0103UL;
    qxp_expect_reject(tokens, QXP_ARRAY_COUNT(qxp_pop_vs11),
                    QXP_SHADER_DECODE_VERSION, 0,
                    "ps_1_3 version still rejected");
}

int main(void)
{
    qxp_test_golden_minimal();
    qxp_test_texture_and_immediates();
    qxp_test_vertex_setup_and_relative();
    qxp_test_control_and_cfg();
    qxp_test_comments();
    qxp_test_exact_opcode_coverage();
    qxp_test_truncation_prefixes();
    qxp_test_versions_and_end();
    qxp_test_framing_and_markers();
    qxp_test_registers_modifiers_and_opcodes();
    qxp_test_read_ports();
    qxp_test_component_initialization();
    qxp_test_vertex_definite_assignment();
    qxp_test_exact_profile_rules();
    qxp_test_malformed_setup_and_relative();
    qxp_test_control_rejections();
    qxp_test_limits();
    qxp_test_mutation_properties();
    qxp_test_argument_contract();
    qxp_test_sm1_streams();
    qxp_test_ps11_streams();

    if (qxp_failures) {
        fprintf(stderr, "qxp_shader_decode: %d failure(s)\n", qxp_failures);
        return 1;
    }
    printf("qxp_shader_decode: all tests passed\n");
    return 0;
}
