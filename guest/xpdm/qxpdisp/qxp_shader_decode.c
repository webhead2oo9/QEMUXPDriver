#include <windef.h>
#include <d3d9types.h>

#include "qxp_shader_ir.h"

#define QXP_PARAMETER_MARKER       0x80000000UL
#define QXP_PARAMETER_RESERVED     0x0000c000UL
#define QXP_PARAMETER_RELATIVE     0x00002000UL
#define QXP_SWIZZLE_IDENTITY       0xe4UL
#define QXP_SWIZZLE_YZXW           0xc9UL
#define QXP_SWIZZLE_ZXYW           0xd2UL
#define QXP_SWIZZLE_WZYX           0x1bUL
#define QXP_INSTRUCTION_RESERVED   0xa0000000UL
#define QXP_INSTRUCTION_PREDICATED 0x10000000UL
#define QXP_INSTRUCTION_COISSUE    0x40000000UL

#define QXP_FORM_REGULAR 0UL
#define QXP_FORM_DCL     1UL
#define QXP_FORM_DEF     2UL

#define QXP_CONTROL_IF   1UL
#define QXP_CONTROL_LOOP 2UL
#define QXP_CONTROL_REP  3UL

#define QXP_VS_TEMP_COUNT              12UL
#define QXP_VS_DATAFLOW_COMPONENTS     (QXP_VS_TEMP_COUNT * 4UL + 4UL)
#define QXP_VS_DATAFLOW_MAX_PASSES     \
    (QXP_VS_DATAFLOW_COMPONENTS + 1UL)
#define QXP_VS_DATAFLOW_MAX_DEPTH      16UL

typedef struct qxp_vs_component_state {
    BYTE temp_masks[QXP_VS_TEMP_COUNT];
    BYTE address_mask;
} QXP_VS_COMPONENT_STATE;

typedef struct qxp_vs_subroutine_summary {
    BOOL present;
    DWORD label_index;
    QXP_VS_COMPONENT_STATE required;
    QXP_VS_COMPONENT_STATE guaranteed;
    QXP_VS_COMPONENT_STATE available_if_all;
} QXP_VS_SUBROUTINE_SUMMARY;

typedef struct qxp_vs_flow_frame {
    DWORD opcode;
    BOOL saw_else;
    QXP_VS_COMPONENT_STATE entry;
    QXP_VS_COMPONENT_STATE branch;
} QXP_VS_FLOW_FRAME;

typedef struct qxp_opcode_info {
    DWORD destination_count;
    DWORD source_count;
    DWORD immediate_count;
    DWORD form;
    DWORD slot_class;
    DWORD slot_count;
} QXP_OPCODE_INFO;

typedef struct qxp_decode_context {
    const DWORD *tokens;
    DWORD token_count;
    QXP_SHADER_IR *ir;
    QXP_SHADER_DECODE_ERROR *error;

    DWORD control_stack[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD control_depth;
    DWORD loop_depth;
    DWORD static_flow_count;

    DWORD labels[QXP_SHADER_MAX_LABELS];
    DWORD vs_input_declarations;
    DWORD vs_semantics[14];
    DWORD vs_position_write_mask;
    DWORD vs_texture_write_masks[8];
    DWORD ps_input_declarations;
    DWORD ps_texture_declarations;
    DWORD ps_sampler_declarations;
    BYTE ps_input_masks[2];
    BYTE ps_texture_masks[8];
    /* ps_1_1 only: stages whose register holds a texture-address result
     * (tex/texcoord/texreg2*) rather than just the interpolated
     * coordinate; texreg2* source stages must appear here. */
    BYTE ps_texture_addressed[8];
    BYTE ps_sampler_types[16];
    BYTE ps_temp_masks[QXP_SHADER_MAX_PS_TEMPS];
    DWORD ps_temp_dependencies[QXP_SHADER_MAX_PS_TEMPS][4];
    DWORD ps_color_writes;
    BOOL ps_depth_written;
    QXP_VS_SUBROUTINE_SUMMARY vs_subroutines[QXP_SHADER_MAX_LABELS];
    BOOL executable_seen;
} QXP_DECODE_CONTEXT;

static void qxp_zero_memory(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD i;

    /* Volatile prevents the WDK optimizer from introducing a CRT memset. */
    bytes = (volatile BYTE *)memory;
    for (i = 0; i < byte_count; ++i) bytes[i] = 0;
}

static BOOL qxp_fail(QXP_DECODE_CONTEXT *context, DWORD code,
                     DWORD token_offset)
{
    context->error->code = code;
    context->error->token_offset = token_offset;
    return FALSE;
}

/*
 * Shader Model 1 streams differ from SM2 in exactly three decodable ways:
 * the instruction token carries no length field, a relative source addresses
 * the implicit a0.x with no following address token, and the address register
 * is written by MOV rather than MOVA.  Everything else -- register files,
 * output semantics, swizzles, and write masks -- is shared, so one decoder
 * serves both and only these three places branch.
 */
static BOOL qxp_stream_is_sm1(const QXP_DECODE_CONTEXT *context)
{
    return (context->ir->version_token & 0x0000ffffUL) < 0x0200UL;
}

/* ps_1_1 is the one SM1 pixel profile the decoder admits.  Its register
 * files, output register, and texture instruction forms differ enough
 * from ps_2_0 that each validation rule below branches on this test
 * instead of generalizing the ps_2_0 rule. */
static BOOL qxp_stream_is_ps11(const QXP_DECODE_CONTEXT *context)
{
    return context->ir->version_token == QXP_SHADER_VERSION_PS_1_1;
}

static void qxp_set_info(QXP_OPCODE_INFO *info, DWORD destinations,
                         DWORD sources, DWORD immediates, DWORD form,
                         DWORD slot_class, DWORD slots)
{
    info->destination_count = destinations;
    info->source_count = sources;
    info->immediate_count = immediates;
    info->form = form;
    info->slot_class = slot_class;
    info->slot_count = slots;
}

static BOOL qxp_get_vertex_opcode(DWORD opcode, QXP_OPCODE_INFO *info)
{
    switch (opcode) {
    case D3DSIO_NOP:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MOV:
    case D3DSIO_RCP:
    case D3DSIO_RSQ:
    case D3DSIO_EXP:
    case D3DSIO_LOG:
    case D3DSIO_LIT:
    case D3DSIO_FRC:
    case D3DSIO_ABS:
    case D3DSIO_NRM:
    case D3DSIO_MOVA:
    case D3DSIO_EXPP:
    case D3DSIO_LOGP:
        qxp_set_info(info, 1, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC,
                     opcode == D3DSIO_LIT || opcode == D3DSIO_NRM ? 3 : 1);
        return TRUE;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
    case D3DSIO_SLT:
    case D3DSIO_SGE:
    case D3DSIO_DST:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MAD:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_LRP:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_M4x4:
    case D3DSIO_M3x4:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 4);
        return TRUE;
    case D3DSIO_M4x3:
    case D3DSIO_M3x3:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_M3x2:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_POW:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_CRS:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_SGN:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_SINCOS:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 8);
        return TRUE;
    case D3DSIO_DCL:
        qxp_set_info(info, 1, 0, 0, QXP_FORM_DCL,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_DEF:
        qxp_set_info(info, 1, 0, 4, QXP_FORM_DEF,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_DEFI:
        qxp_set_info(info, 1, 0, 4, QXP_FORM_DEF,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_DEFB:
        qxp_set_info(info, 1, 0, 1, QXP_FORM_DEF,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_CALL:
        qxp_set_info(info, 0, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 2);
        return TRUE;
    case D3DSIO_CALLNZ:
        qxp_set_info(info, 0, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 3);
        return TRUE;
    case D3DSIO_LOOP:
        qxp_set_info(info, 0, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 3);
        return TRUE;
    case D3DSIO_RET:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 1);
        return TRUE;
    case D3DSIO_ENDLOOP:
    case D3DSIO_ENDREP:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 2);
        return TRUE;
    case D3DSIO_LABEL:
        qxp_set_info(info, 0, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_REP:
        qxp_set_info(info, 0, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 3);
        return TRUE;
    case D3DSIO_IF:
        qxp_set_info(info, 0, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 3);
        return TRUE;
    case D3DSIO_ELSE:
    case D3DSIO_ENDIF:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_FLOW, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL qxp_get_pixel_opcode(DWORD opcode, QXP_OPCODE_INFO *info)
{
    switch (opcode) {
    case D3DSIO_NOP:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MOV:
    case D3DSIO_RCP:
    case D3DSIO_RSQ:
    case D3DSIO_EXP:
    case D3DSIO_LOG:
    case D3DSIO_FRC:
    case D3DSIO_ABS:
        qxp_set_info(info, 1, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_NRM:
        qxp_set_info(info, 1, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MAD:
    case D3DSIO_CMP:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_LRP:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_M4x4:
    case D3DSIO_M3x4:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 4);
        return TRUE;
    case D3DSIO_M4x3:
    case D3DSIO_M3x3:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_M3x2:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_POW:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 3);
        return TRUE;
    case D3DSIO_CRS:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_SINCOS:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 8);
        return TRUE;
    case D3DSIO_DP2ADD:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 2);
        return TRUE;
    case D3DSIO_DCL:
        qxp_set_info(info, 1, 0, 0, QXP_FORM_DCL,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_DEF:
        qxp_set_info(info, 1, 0, 4, QXP_FORM_DEF,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    case D3DSIO_TEXKILL:
        qxp_set_info(info, 1, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_TEXTURE, 1);
        return TRUE;
    case D3DSIO_TEX:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_TEXTURE, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * ps_1_1 admits only the fixed-function-style arithmetic set plus its own
 * texture forms.  `tex tN`, `texcoord tN`, and `texkill tN` name their
 * stage by destination register and take no source operands, unlike the
 * ps_2_0 texld form whose coordinate and sampler are explicit sources.
 * The remaining ps_1_x texture-addressing family (texbem, texm3x*,
 * texdp3*) is deliberately absent so it falls through to
 * QXP_SHADER_DECODE_UNSUPPORTED_OPCODE.
 */
static BOOL qxp_get_ps11_opcode(DWORD opcode, QXP_OPCODE_INFO *info)
{
    switch (opcode) {
    case D3DSIO_NOP:
        qxp_set_info(info, 0, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MOV:
        qxp_set_info(info, 1, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
        qxp_set_info(info, 1, 2, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_MAD:
    case D3DSIO_LRP:
    case D3DSIO_CND:
    case D3DSIO_CMP:
        qxp_set_info(info, 1, 3, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_ARITHMETIC, 1);
        return TRUE;
    case D3DSIO_DEF:
        qxp_set_info(info, 1, 0, 4, QXP_FORM_DEF,
                     QXP_SHADER_SLOT_SETUP, 0);
        return TRUE;
    /* `tex` samples stage N; `texcoord` delivers stage N's interpolated
     * coordinate as colour without sampling.  Both name their stage by
     * destination register number.  `texkill` stays rejected: nothing
     * qualifies its lowering and no title here needs it. */
    case D3DSIO_TEX:
    case D3DSIO_TEXCOORD:
        qxp_set_info(info, 1, 0, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_TEXTURE, 1);
        return TRUE;
    case D3DSIO_TEXREG2AR:
    case D3DSIO_TEXREG2GB:
        /* Dependent read: stage N is sampled at (tM.a, tM.r) or
         * (tM.g, tM.b); NFSU2 depends on both forms. */
        qxp_set_info(info, 1, 1, 0, QXP_FORM_REGULAR,
                     QXP_SHADER_SLOT_TEXTURE, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL qxp_get_opcode(const QXP_DECODE_CONTEXT *context, DWORD opcode,
                           QXP_OPCODE_INFO *info)
{
    qxp_zero_memory(info, (DWORD)sizeof(*info));
    if (context->ir->stage == QXP_SHADER_STAGE_VERTEX)
        return qxp_get_vertex_opcode(opcode, info);
    if (qxp_stream_is_ps11(context))
        return qxp_get_ps11_opcode(opcode, info);
    return qxp_get_pixel_opcode(opcode, info);
}

static DWORD qxp_register_file(DWORD token)
{
    return ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
           ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
}

static DWORD qxp_swizzle_value(const QXP_SHADER_OPERAND *operand)
{
    return (DWORD)operand->swizzle[0] |
           ((DWORD)operand->swizzle[1] << 2) |
           ((DWORD)operand->swizzle[2] << 4) |
           ((DWORD)operand->swizzle[3] << 6);
}

static BOOL qxp_swizzle_is_identity(const QXP_SHADER_OPERAND *operand)
{
    return qxp_swizzle_value(operand) == QXP_SWIZZLE_IDENTITY;
}

static BOOL qxp_swizzle_is_replicate(const QXP_SHADER_OPERAND *operand)
{
    return operand->swizzle[0] == operand->swizzle[1] &&
           operand->swizzle[0] == operand->swizzle[2] &&
           operand->swizzle[0] == operand->swizzle[3];
}

static BOOL qxp_pixel_swizzle_is_baseline(
    const QXP_SHADER_OPERAND *operand)
{
    DWORD swizzle;

    swizzle = qxp_swizzle_value(operand);
    if (swizzle == QXP_SWIZZLE_IDENTITY ||
        swizzle == QXP_SWIZZLE_YZXW ||
        swizzle == QXP_SWIZZLE_ZXYW ||
        swizzle == QXP_SWIZZLE_WZYX) return TRUE;
    return qxp_swizzle_is_replicate(operand);
}

static void qxp_record_pixel_temp_requirement(
    QXP_DECODE_CONTEXT *context, DWORD register_number, DWORD count)
{
    DWORD required;

    required = register_number + count;
    if (required > context->ir->required_ps_temp_count)
        context->ir->required_ps_temp_count = required;
}

static void qxp_decode_swizzle(DWORD token, BYTE swizzle[4])
{
    DWORD value;
    DWORD i;

    value = (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
    for (i = 0; i < 4; ++i)
        swizzle[i] = (BYTE)((value >> (i * 2)) & 3UL);
}

static BOOL qxp_parse_destination(QXP_DECODE_CONTEXT *context, DWORD token,
                                  DWORD token_offset, BYTE role,
                                  QXP_SHADER_OPERAND *operand)
{
    if ((token & QXP_PARAMETER_MARKER) == 0)
        return qxp_fail(context, QXP_SHADER_DECODE_PARAMETER_MARKER,
                        token_offset);
    if (token & QXP_PARAMETER_RESERVED)
        return qxp_fail(context, QXP_SHADER_DECODE_OPERAND_BITS,
                        token_offset);
    if (token & QXP_PARAMETER_RELATIVE)
        return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                        token_offset);

    operand->role = role;
    operand->register_file = (BYTE)qxp_register_file(token);
    operand->register_number = (WORD)(token & D3DSP_REGNUM_MASK);
    operand->write_mask = (BYTE)((token & D3DSP_WRITEMASK_ALL) >> 16);
    operand->destination_modifier =
        (BYTE)((token & D3DSP_DSTMOD_MASK) >> D3DSP_DSTMOD_SHIFT);
    operand->destination_shift =
        (BYTE)((token & D3DSP_DSTSHIFT_MASK) >> D3DSP_DSTSHIFT_SHIFT);
    return TRUE;
}

static BOOL qxp_parse_source(QXP_DECODE_CONTEXT *context, DWORD *cursor,
                             DWORD instruction_end, BYTE role,
                             QXP_SHADER_OPERAND *operand)
{
    DWORD token;
    DWORD token_offset;
    DWORD relative_token;
    DWORD relative_file;
    DWORD relative_swizzle;

    if (*cursor >= instruction_end)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                        instruction_end);

    token_offset = *cursor;
    token = context->tokens[*cursor];
    ++(*cursor);
    if ((token & QXP_PARAMETER_MARKER) == 0)
        return qxp_fail(context, QXP_SHADER_DECODE_PARAMETER_MARKER,
                        token_offset);
    if (token & QXP_PARAMETER_RESERVED)
        return qxp_fail(context, QXP_SHADER_DECODE_OPERAND_BITS,
                        token_offset);

    operand->role = role;
    operand->register_file = (BYTE)qxp_register_file(token);
    operand->register_number = (WORD)(token & D3DSP_REGNUM_MASK);
    operand->source_modifier =
        (BYTE)((token & D3DSP_SRCMOD_MASK) >> D3DSP_SRCMOD_SHIFT);
    qxp_decode_swizzle(token, operand->swizzle);

    if ((token & QXP_PARAMETER_RELATIVE) == 0) return TRUE;
    if (context->ir->stage != QXP_SHADER_STAGE_VERTEX ||
        operand->register_file != QXP_SHADER_REG_CONST)
        return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                        token_offset);
    /* SM1 encodes relative addressing as a bare bit: the index is always
     * a0.x and no address token follows.  Synthesize the operand the SM2
     * form spells out so every later consumer sees one representation. */
    if (qxp_stream_is_sm1(context)) {
        operand->relative.present = TRUE;
        operand->relative.role = QXP_SHADER_OPERAND_RELATIVE_ADDRESS;
        operand->relative.register_file = QXP_SHADER_REG_ADDRESS;
        operand->relative.register_number = 0;
        operand->relative.source_modifier = QXP_SHADER_SRCMOD_NONE;
        operand->relative.swizzle[0] = 0;
        operand->relative.swizzle[1] = 0;
        operand->relative.swizzle[2] = 0;
        operand->relative.swizzle[3] = 0;
        return TRUE;
    }
    if (*cursor >= instruction_end)
        return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                        token_offset);

    token_offset = *cursor;
    relative_token = context->tokens[*cursor];
    ++(*cursor);
    if ((relative_token & QXP_PARAMETER_MARKER) == 0)
        return qxp_fail(context, QXP_SHADER_DECODE_PARAMETER_MARKER,
                        token_offset);
    if ((relative_token & QXP_PARAMETER_RESERVED) ||
        (relative_token & QXP_PARAMETER_RELATIVE))
        return qxp_fail(context, QXP_SHADER_DECODE_OPERAND_BITS,
                        token_offset);
    if ((relative_token & D3DSP_SRCMOD_MASK) != D3DSPSM_NONE)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        token_offset);

    relative_file = qxp_register_file(relative_token);
    if ((relative_file != QXP_SHADER_REG_ADDRESS &&
         relative_file != QXP_SHADER_REG_LOOP) ||
        (relative_token & D3DSP_REGNUM_MASK) != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                        token_offset);

    relative_swizzle =
        (relative_token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
    if (relative_file == QXP_SHADER_REG_ADDRESS) {
        DWORD component;

        component = relative_swizzle & 3UL;
        if (relative_swizzle !=
            (component | (component << 2) | (component << 4) |
             (component << 6)))
            return qxp_fail(context,
                            QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                            token_offset);
    } else if (relative_swizzle != QXP_SWIZZLE_IDENTITY) {
        return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                        token_offset);
    }

    operand->relative.present = TRUE;
    operand->relative.role = QXP_SHADER_OPERAND_RELATIVE_ADDRESS;
    operand->relative.register_file = (BYTE)relative_file;
    operand->relative.register_number =
        (WORD)(relative_token & D3DSP_REGNUM_MASK);
    operand->relative.source_modifier = QXP_SHADER_SRCMOD_NONE;
    qxp_decode_swizzle(relative_token, operand->relative.swizzle);
    return TRUE;
}

static BOOL qxp_operand_aliases(const QXP_SHADER_OPERAND *left,
                                const QXP_SHADER_OPERAND *right)
{
    return left->register_file == right->register_file &&
           left->register_number == right->register_number;
}

static BOOL qxp_validate_float_source(QXP_DECODE_CONTEXT *context,
                                      const QXP_SHADER_INSTRUCTION *instruction,
                                      const QXP_SHADER_OPERAND *operand)
{
    DWORD limit;

    limit = 0;
    if (context->ir->stage == QXP_SHADER_STAGE_VERTEX) {
        if (operand->register_file == QXP_SHADER_REG_TEMP) limit = 12;
        else if (operand->register_file == QXP_SHADER_REG_INPUT) limit = 16;
        else if (operand->register_file == QXP_SHADER_REG_CONST) limit = 256;
    } else if (qxp_stream_is_ps11(context)) {
        /* ps_1_1 register bounds: r0-r1, v0-v1, c0-c7, t0-t3. */
        if (operand->register_file == QXP_SHADER_REG_TEMP) limit = 2;
        else if (operand->register_file == QXP_SHADER_REG_INPUT) limit = 2;
        else if (operand->register_file == QXP_SHADER_REG_CONST) limit = 8;
        else if (operand->register_file == QXP_SHADER_REG_TEXTURE) limit = 4;
    } else {
        if (operand->register_file == QXP_SHADER_REG_TEMP)
            limit = QXP_SHADER_MAX_PS_TEMPS;
        else if (operand->register_file == QXP_SHADER_REG_INPUT) limit = 2;
        else if (operand->register_file == QXP_SHADER_REG_CONST) limit = 32;
        else if (operand->register_file == QXP_SHADER_REG_TEXTURE) limit = 8;
    }

    if (!limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if ((DWORD)operand->register_number >= limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    /* ps_1_1 admits the _neg and _comp source modifiers; NFSU2's
     * dependent-read shader uses _comp on a mad source (r543 capture).
     * Every other modifier stays profile-illegal here. */
    if (operand->source_modifier != QXP_SHADER_SRCMOD_NONE &&
        operand->source_modifier != QXP_SHADER_SRCMOD_NEGATE &&
        !(qxp_stream_is_ps11(context) &&
          operand->source_modifier == QXP_SHADER_SRCMOD_COMP))
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (context->ir->stage == QXP_SHADER_STAGE_PIXEL) {
        if (operand->register_file == QXP_SHADER_REG_TEMP)
            qxp_record_pixel_temp_requirement(
                context, operand->register_number, 1);
        if (!qxp_pixel_swizzle_is_baseline(operand))
            context->ir->required_ps20_caps |=
                QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE;
    }
    return TRUE;
}

static BOOL qxp_validate_exact_source(QXP_DECODE_CONTEXT *context,
                                      const QXP_SHADER_INSTRUCTION *instruction,
                                      QXP_SHADER_OPERAND *operand,
                                      DWORD register_file, DWORD limit,
                                      DWORD allowed_modifiers, BYTE role)
{
    operand->role = role;
    if (operand->register_file != register_file)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if ((DWORD)operand->register_number >= limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    if (((1UL << operand->source_modifier) & allowed_modifiers) == 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (!qxp_swizzle_is_identity(operand) || operand->relative.present)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    return TRUE;
}

static BOOL qxp_validate_vertex_destination(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_SHADER_OPERAND *operand)
{
    DWORD mask;

    if (operand->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        operand->destination_shift != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    mask = operand->write_mask;
    if (!mask)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);

    if (operand->register_file == QXP_SHADER_REG_TEMP) {
        if (operand->register_number >= 12)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_RASTOUT) {
        if (operand->register_number >= 3)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        /* Scalar outputs carry the default destination mask in bytecode. */
        if (operand->register_number != D3DSRO_POSITION && mask != 15)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        if (operand->register_number == D3DSRO_POSITION)
            context->vs_position_write_mask |= mask;
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_ATTROUT) {
        if (operand->register_number >= 2)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        return TRUE;
    }
    if (operand->register_file == QXP_SHADER_REG_TEXCRDOUT) {
        if (operand->register_number >= 8)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        /* Individual writes may be arbitrary; validation below checks the
         * profile's combined-mask and contiguous-register requirements. */
        context->vs_texture_write_masks[operand->register_number] |= mask;
        return TRUE;
    }
    return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                    instruction->token_offset);
}

static BOOL qxp_validate_pixel_temp_destination(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_SHADER_OPERAND *operand,
    DWORD allowed_modifiers)
{
    if (operand->register_file != QXP_SHADER_REG_TEMP)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (operand->register_number >= QXP_SHADER_MAX_PS_TEMPS)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    qxp_record_pixel_temp_requirement(context, operand->register_number, 1);
    if (!operand->write_mask)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (operand->destination_shift != 0 ||
        (operand->destination_modifier & ~allowed_modifiers) != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    return TRUE;
}

static DWORD qxp_matrix_rows(DWORD opcode)
{
    if (opcode == D3DSIO_M4x4 || opcode == D3DSIO_M3x4) return 4;
    if (opcode == D3DSIO_M4x3 || opcode == D3DSIO_M3x3) return 3;
    return 2;
}

static DWORD qxp_matrix_columns(DWORD opcode)
{
    if (opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3) return 4;
    return 3;
}

static BOOL qxp_validate_matrix(QXP_DECODE_CONTEXT *context,
                                QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    QXP_SHADER_OPERAND *source0;
    QXP_SHADER_OPERAND *source1;
    DWORD rows;
    DWORD required_mask;
    DWORD limit;

    destination = &instruction->destinations[0];
    source0 = &instruction->sources[0];
    source1 = &instruction->sources[1];
    rows = qxp_matrix_rows(instruction->opcode);
    required_mask = rows == 4 ? 15 : (rows == 3 ? 7 : 3);

    if (destination->write_mask != required_mask)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (!qxp_swizzle_is_identity(source1) ||
        source1->source_modifier != QXP_SHADER_SRCMOD_NONE)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (qxp_operand_aliases(destination, source0))
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);

    limit = 0;
    if (context->ir->stage == QXP_SHADER_STAGE_VERTEX) {
        if (source1->register_file == QXP_SHADER_REG_TEMP) limit = 12;
        else if (source1->register_file == QXP_SHADER_REG_INPUT) limit = 16;
        else if (source1->register_file == QXP_SHADER_REG_CONST) limit = 256;
    } else {
        if (source1->register_file == QXP_SHADER_REG_TEMP)
            limit = QXP_SHADER_MAX_PS_TEMPS;
        else if (source1->register_file == QXP_SHADER_REG_INPUT) limit = 2;
        else if (source1->register_file == QXP_SHADER_REG_CONST) limit = 32;
        else if (source1->register_file == QXP_SHADER_REG_TEXTURE) limit = 8;
    }
    if (!limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (!source1->relative.present &&
        (DWORD)source1->register_number + rows > limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
        source1->register_file == QXP_SHADER_REG_TEMP)
        qxp_record_pixel_temp_requirement(
            context, source1->register_number, rows);
    if (destination->register_file == source1->register_file &&
        (DWORD)destination->register_number >=
            (DWORD)source1->register_number &&
        (DWORD)destination->register_number <
            (DWORD)source1->register_number + rows)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    return TRUE;
}

static BOOL qxp_is_scalar_opcode(DWORD opcode)
{
    return opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
           opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
           opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP;
}

static BOOL qxp_is_matrix_opcode(DWORD opcode)
{
    return opcode == D3DSIO_M4x4 || opcode == D3DSIO_M4x3 ||
           opcode == D3DSIO_M3x4 || opcode == D3DSIO_M3x3 ||
           opcode == D3DSIO_M3x2;
}

static BOOL qxp_source_registers_are_identical(
    const QXP_SHADER_OPERAND *left, const QXP_SHADER_OPERAND *right)
{
    DWORD i;

    if (!qxp_operand_aliases(left, right) ||
        left->relative.present != right->relative.present)
        return FALSE;
    if (!left->relative.present) return TRUE;
    if (left->relative.register_file != right->relative.register_file ||
        left->relative.register_number != right->relative.register_number)
        return FALSE;
    for (i = 0; i < 4; ++i) {
        if (left->relative.swizzle[i] != right->relative.swizzle[i])
            return FALSE;
    }
    return TRUE;
}

static BOOL qxp_validate_read_ports(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    const QXP_SHADER_OPERAND *first_constant;
    const QXP_SHADER_OPERAND *first_input;
    const QXP_SHADER_OPERAND *first_texture;
    const QXP_SHADER_OPERAND *source0;
    const QXP_SHADER_OPERAND *source1;
    DWORD constant_reads;
    DWORD texture_reads;
    DWORD i;
    BOOL sincos;
    BOOL matrix;

    first_constant = (const QXP_SHADER_OPERAND *)0;
    first_input = (const QXP_SHADER_OPERAND *)0;
    first_texture = (const QXP_SHADER_OPERAND *)0;
    constant_reads = 0;
    texture_reads = 0;
    sincos = instruction->opcode == D3DSIO_SINCOS;
    matrix = qxp_is_matrix_opcode(instruction->opcode);

    /* A matrix macro expands into several dot products.  Its matrix source
     * may span a one-port register file, but the vector and matrix cannot
     * both come from that same one-port file.  Pixel v# and t# are distinct
     * register files with distinct read ports. */
    if (matrix) {
        source0 = &instruction->sources[0];
        source1 = &instruction->sources[1];
        if (context->ir->stage == QXP_SHADER_STAGE_VERTEX) {
            if ((source0->register_file == QXP_SHADER_REG_INPUT &&
                 source1->register_file == QXP_SHADER_REG_INPUT) ||
                (source0->register_file == QXP_SHADER_REG_CONST &&
                 source1->register_file == QXP_SHADER_REG_CONST))
                return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                                instruction->token_offset);
        } else if ((source0->register_file == QXP_SHADER_REG_INPUT &&
                    source1->register_file == QXP_SHADER_REG_INPUT) ||
                   (source0->register_file == QXP_SHADER_REG_TEXTURE &&
                    source1->register_file == QXP_SHADER_REG_TEXTURE) ||
                   (source0->register_file == QXP_SHADER_REG_CONST &&
                    source1->register_file == QXP_SHADER_REG_CONST)) {
            return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                            instruction->token_offset);
        }
    }

    for (i = 0; i < instruction->source_count; ++i) {
        const QXP_SHADER_OPERAND *source;
        const QXP_SHADER_OPERAND **first_port;

        source = &instruction->sources[i];
        /* The per-instruction read count is independent of register-port
         * identity.  SINCOS's two documented coefficient operands retain
         * their special constant-source handling. */
        if (source->register_file == QXP_SHADER_REG_CONST && !sincos) {
            if (++constant_reads > 2)
                return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                                instruction->token_offset);
            if (!first_constant) {
                first_constant = source;
            } else if (!qxp_source_registers_are_identical(first_constant,
                                                            source)) {
                return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                                instruction->token_offset);
            }
        }

        first_port = (const QXP_SHADER_OPERAND **)0;
        if (source->register_file == QXP_SHADER_REG_INPUT) {
            first_port = &first_input;
        } else if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
                   source->register_file == QXP_SHADER_REG_TEXTURE) {
            if (++texture_reads > 1)
                return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                                instruction->token_offset);
            first_port = &first_texture;
        }
        if (!first_port) continue;
        if (!*first_port) {
            *first_port = source;
        } else if (!qxp_source_registers_are_identical(*first_port, source)) {
            return qxp_fail(context, QXP_SHADER_DECODE_READ_PORT,
                            instruction->token_offset);
        }
    }
    return TRUE;
}

static DWORD qxp_pixel_source_logical_mask(
    const QXP_SHADER_INSTRUCTION *instruction, DWORD source_index)
{
    DWORD destination_mask;
    DWORD opcode;

    destination_mask = instruction->destination_count ?
        instruction->destinations[0].write_mask : 15;
    opcode = instruction->opcode;
    if (opcode == D3DSIO_DP3) return 7;
    if (opcode == D3DSIO_DP4) return 15;
    if (qxp_is_matrix_opcode(opcode))
        return qxp_matrix_columns(opcode) == 4 ? 15 : 7;
    if (opcode == D3DSIO_DP2ADD) return source_index < 2 ? 3 : 1;
    if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
        opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
        opcode == D3DSIO_POW)
        return 1;
    if (opcode == D3DSIO_SINCOS) return source_index ? 15 : 1;
    if (opcode == D3DSIO_NRM)
        return 7 | (destination_mask & 8);
    if (opcode == D3DSIO_CRS) return 7;
    return destination_mask;
}

static DWORD qxp_swizzled_component_mask(
    const QXP_SHADER_OPERAND *operand, DWORD logical_mask)
{
    DWORD component_mask;
    DWORD i;

    component_mask = 0;
    for (i = 0; i < 4; ++i) {
        if (logical_mask & (1UL << i))
            component_mask |= 1UL << operand->swizzle[i];
    }
    return component_mask;
}

static BOOL qxp_validate_pixel_register_components(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    DWORD register_file, DWORD register_number, DWORD required_mask)
{
    DWORD available_mask;

    if (register_file == QXP_SHADER_REG_TEMP) {
        available_mask = context->ps_temp_masks[register_number];
        if ((available_mask & required_mask) != required_mask)
            return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                            instruction->token_offset);
    } else if (register_file == QXP_SHADER_REG_INPUT) {
        /* ps_1_1 has no declarations; v0-v1 arrive fully defined.  The
         * v0-v1 range bound already ran in qxp_validate_float_source. */
        if (qxp_stream_is_ps11(context)) return TRUE;
        /* Channels omitted by a v# DCL have the ps_2_0 (0,0,0,1)
         * defaults, but the register itself must still be declared. */
        if (!(context->ps_input_declarations & (1UL << register_number)))
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
        available_mask = 15;
        if ((available_mask & required_mask) != required_mask)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
    } else if (register_file == QXP_SHADER_REG_TEXTURE) {
        available_mask = context->ps_texture_masks[register_number];
        if ((available_mask & required_mask) != required_mask)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
    }
    return TRUE;
}

static BOOL qxp_validate_pixel_source_components(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD i;

    for (i = 0; i < instruction->source_count; ++i) {
        const QXP_SHADER_OPERAND *source;
        DWORD logical_mask;
        DWORD required_mask;

        source = &instruction->sources[i];
        logical_mask = qxp_pixel_source_logical_mask(instruction, i);
        required_mask = qxp_swizzled_component_mask(source, logical_mask);
        if (!qxp_validate_pixel_register_components(
                context, instruction, source->register_file,
                source->register_number, required_mask)) return FALSE;
    }

    if (qxp_is_matrix_opcode(instruction->opcode)) {
        const QXP_SHADER_OPERAND *matrix;
        DWORD columns;
        DWORD rows;

        matrix = &instruction->sources[1];
        columns = qxp_matrix_columns(instruction->opcode);
        rows = qxp_matrix_rows(instruction->opcode);
        for (i = 1; i < rows; ++i) {
            if (!qxp_validate_pixel_register_components(
                    context, instruction, matrix->register_file,
                    (DWORD)matrix->register_number + i,
                    columns == 4 ? 15 : 7)) return FALSE;
        }
    }
    return TRUE;
}

static DWORD qxp_max_value(DWORD left, DWORD right)
{
    return left > right ? left : right;
}

static DWORD qxp_pixel_register_dependency(
    const QXP_DECODE_CONTEXT *context, DWORD register_file,
    DWORD register_number, DWORD component_mask)
{
    DWORD dependency;
    DWORD i;

    if (register_file != QXP_SHADER_REG_TEMP) return 0;
    dependency = 0;
    for (i = 0; i < 4; ++i) {
        if (component_mask & (1UL << i))
            dependency = qxp_max_value(
                dependency,
                context->ps_temp_dependencies[register_number][i]);
    }
    return dependency;
}

static DWORD qxp_pixel_operand_dependency(
    const QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_OPERAND *operand, DWORD logical_mask)
{
    return qxp_pixel_register_dependency(
        context, operand->register_file, operand->register_number,
        qxp_swizzled_component_mask(operand, logical_mask));
}

static DWORD qxp_pixel_result_dependency(
    const QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction, DWORD component)
{
    const QXP_SHADER_OPERAND *matrix;
    DWORD dependency;
    DWORD logical_mask;
    DWORD opcode;
    DWORD i;

    dependency = 0;
    opcode = instruction->opcode;
    if (qxp_is_matrix_opcode(opcode)) {
        logical_mask = qxp_matrix_columns(opcode) == 4 ? 15 : 7;
        dependency = qxp_pixel_operand_dependency(
            context, &instruction->sources[0], logical_mask);
        matrix = &instruction->sources[1];
        return qxp_max_value(
            dependency,
            qxp_pixel_register_dependency(
                context, matrix->register_file,
                (DWORD)matrix->register_number + component,
                logical_mask));
    }

    if (opcode == D3DSIO_DP3) logical_mask = 7;
    else if (opcode == D3DSIO_DP4) logical_mask = 15;
    else if (opcode == D3DSIO_DP2ADD) {
        dependency = qxp_pixel_operand_dependency(
            context, &instruction->sources[0], 3);
        dependency = qxp_max_value(
            dependency, qxp_pixel_operand_dependency(
                context, &instruction->sources[1], 3));
        return qxp_max_value(
            dependency, qxp_pixel_operand_dependency(
                context, &instruction->sources[2], 1));
    } else if (opcode == D3DSIO_CRS) {
        logical_mask = component == 0 ? 6 : (component == 1 ? 5 : 3);
    } else if (opcode == D3DSIO_NRM) {
        logical_mask = 7 | (1UL << component);
    } else if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
               opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
               opcode == D3DSIO_POW || opcode == D3DSIO_SINCOS) {
        logical_mask = 1;
    } else {
        logical_mask = 1UL << component;
    }

    for (i = 0; i < instruction->source_count; ++i) {
        if (opcode == D3DSIO_SINCOS && i != 0) continue;
        dependency = qxp_max_value(
            dependency, qxp_pixel_operand_dependency(
                context, &instruction->sources[i], logical_mask));
    }
    return dependency;
}

static void qxp_record_pixel_temp_write(
    QXP_DECODE_CONTEXT *context, const QXP_SHADER_INSTRUCTION *instruction)
{
    const QXP_SHADER_OPERAND *destination;
    DWORD dependencies[4];
    DWORD mask;
    DWORD i;

    if (!instruction->destination_count) return;
    destination = &instruction->destinations[0];
    if (destination->register_file != QXP_SHADER_REG_TEMP) return;
    qxp_zero_memory(dependencies, (DWORD)sizeof(dependencies));
    for (i = 0; i < 4; ++i) {
        if (destination->write_mask & (1UL << i))
            dependencies[i] = qxp_pixel_result_dependency(
                context, instruction, i);
    }

    mask = context->ps_temp_masks[destination->register_number];
    if (instruction->opcode == D3DSIO_SINCOS) {
        mask &= 8;
        for (i = 0; i < 3; ++i)
            context->ps_temp_dependencies[
                destination->register_number][i] = 0;
    }
    for (i = 0; i < 4; ++i) {
        if (destination->write_mask & (1UL << i))
            context->ps_temp_dependencies[
                destination->register_number][i] = dependencies[i];
    }
    context->ps_temp_masks[destination->register_number] =
        (BYTE)(mask | destination->write_mask);
}

static BOOL qxp_validate_pixel_texture_dependency(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction, DWORD required_mask,
    DWORD *texture_dependency)
{
    const QXP_SHADER_OPERAND *destination;
    const QXP_SHADER_OPERAND *source;
    DWORD dependency;
    DWORD order;

    destination = &instruction->destinations[0];
    source = &instruction->sources[0];
    dependency = qxp_pixel_register_dependency(
        context, source->register_file, source->register_number,
        required_mask);
    dependency = qxp_max_value(
        dependency, qxp_pixel_register_dependency(
            context, destination->register_file,
            destination->register_number,
            context->ps_temp_masks[destination->register_number]));
    order = (source->register_file == QXP_SHADER_REG_TEMP ||
             context->ps_temp_masks[destination->register_number]) ? 1 : 0;
    if (dependency) order = qxp_max_value(order, dependency + 1);

    if (order > 3)
        context->ir->required_ps20_caps |=
            QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT;
    *texture_dependency = order;
    return TRUE;
}

static void qxp_record_pixel_texture_write(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction, DWORD texture_dependency)
{
    const QXP_SHADER_OPERAND *destination;
    DWORD i;

    destination = &instruction->destinations[0];
    context->ps_temp_masks[destination->register_number] = 15;
    for (i = 0; i < 4; ++i)
        context->ps_temp_dependencies[destination->register_number][i] =
            texture_dependency;
}

static BOOL qxp_validate_cross_product(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;

    destination = &instruction->destinations[0];
    if (destination->register_file != QXP_SHADER_REG_TEMP ||
        (destination->write_mask & 8) != 0 ||
        qxp_operand_aliases(destination, &instruction->sources[0]) ||
        qxp_operand_aliases(destination, &instruction->sources[1]))
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (!qxp_swizzle_is_identity(&instruction->sources[0]) ||
        !qxp_swizzle_is_identity(&instruction->sources[1]))
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    return TRUE;
}

static BOOL qxp_validate_vertex_arithmetic(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD i;
    QXP_SHADER_OPERAND *destination;

    destination = instruction->destination_count ?
        &instruction->destinations[0] : (QXP_SHADER_OPERAND *)0;

    /* vs_1_1 loads the address register with MOV; vs_2_0 introduced MOVA
     * for the same effect.  Validate both against the MOVA rules so the
     * two spellings produce one IR shape. */
    if (instruction->opcode == D3DSIO_MOVA ||
        (qxp_stream_is_sm1(context) && instruction->opcode == D3DSIO_MOV &&
         destination &&
         destination->register_file == QXP_SHADER_REG_ADDRESS)) {
        if (destination->register_file != QXP_SHADER_REG_ADDRESS)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        if (destination->register_number != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        if (!destination->write_mask ||
            destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
    } else if (destination &&
               !qxp_validate_vertex_destination(context, instruction,
                                                destination)) {
        return FALSE;
    }

    for (i = 0; i < instruction->source_count; ++i) {
        if (!qxp_validate_float_source(context, instruction,
                                       &instruction->sources[i]))
            return FALSE;
    }
    if (!qxp_validate_read_ports(context, instruction)) return FALSE;

    if (destination &&
        destination->register_file == QXP_SHADER_REG_RASTOUT &&
        destination->register_number != D3DSRO_POSITION) {
        for (i = 0; i < instruction->source_count; ++i) {
            if (!qxp_swizzle_is_replicate(&instruction->sources[i]))
                return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                                instruction->token_offset);
        }
    }

    if (qxp_is_scalar_opcode(instruction->opcode)) {
        DWORD mask;

        mask = destination->write_mask;
        if (!qxp_swizzle_is_replicate(&instruction->sources[0]) ||
            (mask & (mask - 1)) != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
    }

    if (instruction->opcode == D3DSIO_POW) {
        DWORD mask;

        mask = destination->write_mask;
        if ((mask & (mask - 1)) != 0 ||
            destination->register_file != QXP_SHADER_REG_TEMP ||
            !qxp_swizzle_is_replicate(&instruction->sources[0]) ||
            !qxp_swizzle_is_replicate(&instruction->sources[1]))
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        if (qxp_operand_aliases(destination, &instruction->sources[1]))
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_NRM) {
        if (destination->register_file != QXP_SHADER_REG_TEMP ||
            qxp_operand_aliases(destination, &instruction->sources[0]))
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        if (destination->write_mask != 7 && destination->write_mask != 15)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_SGN) {
        QXP_SHADER_OPERAND *source1;
        QXP_SHADER_OPERAND *source2;

        source1 = &instruction->sources[1];
        source2 = &instruction->sources[2];
        if (source1->register_file != QXP_SHADER_REG_TEMP ||
            source1->register_number >= 12 ||
            source2->register_file != QXP_SHADER_REG_TEMP ||
            source2->register_number >= 12 ||
            qxp_operand_aliases(source1, source2) ||
            !qxp_swizzle_is_identity(source1) ||
            !qxp_swizzle_is_identity(source2) ||
            source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source2->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source1->relative.present || source2->relative.present)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_SINCOS) {
        QXP_SHADER_OPERAND *source1;
        QXP_SHADER_OPERAND *source2;

        source1 = &instruction->sources[1];
        source2 = &instruction->sources[2];
        if (destination->register_file != QXP_SHADER_REG_TEMP ||
            (destination->write_mask != 1 &&
             destination->write_mask != 2 &&
             destination->write_mask != 3) ||
            qxp_operand_aliases(destination, &instruction->sources[0]) ||
            !qxp_swizzle_is_replicate(&instruction->sources[0]) ||
            source1->register_file != QXP_SHADER_REG_CONST ||
            source2->register_file != QXP_SHADER_REG_CONST ||
            source1->register_number >= 256 ||
            source2->register_number >= 256 ||
            source1->register_number == source2->register_number ||
            !qxp_swizzle_is_identity(source1) ||
            !qxp_swizzle_is_identity(source2) ||
            source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source2->source_modifier != QXP_SHADER_SRCMOD_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_CRS)
        return qxp_validate_cross_product(context, instruction);
    if (qxp_is_matrix_opcode(instruction->opcode))
        return qxp_validate_matrix(context, instruction);
    return TRUE;
}

static BOOL qxp_validate_pixel_output(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    QXP_SHADER_OPERAND *source;

    destination = &instruction->destinations[0];
    source = &instruction->sources[0];
    if (instruction->opcode != D3DSIO_MOV)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0 ||
        destination->write_mask != 15)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (destination->register_file == QXP_SHADER_REG_COLOROUT) {
        DWORD bit;

        if (destination->register_number >= 4)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        if (!qxp_swizzle_is_identity(source) ||
            source->source_modifier != QXP_SHADER_SRCMOD_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        bit = 1UL << destination->register_number;
        if (context->ps_color_writes & bit)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        context->ps_color_writes |= bit;
    } else if (destination->register_file == QXP_SHADER_REG_DEPTHOUT) {
        if (destination->register_number != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        if (!qxp_swizzle_is_replicate(source) ||
            source->source_modifier != QXP_SHADER_SRCMOD_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        if (context->ps_depth_written)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        context->ps_depth_written = TRUE;
    } else {
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    }
    return TRUE;
}

/*
 * ps_1_1 arithmetic destinations are r0-r1 or t0-t3 with an optional
 * saturate and the _x2/_x4 multiply shifts (see the shift rule below);
 * t# stays a legal destination because ps_1_1 treats it as a general
 * register that happens to arrive holding the stage's interpolated
 * coordinate.
 */
static BOOL qxp_validate_ps11_destination(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_SHADER_OPERAND *operand)
{
    if (operand->register_file == QXP_SHADER_REG_TEMP) {
        if (operand->register_number >= 2)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        qxp_record_pixel_temp_requirement(
            context, operand->register_number, 1);
    } else if (operand->register_file == QXP_SHADER_REG_TEXTURE) {
        if (operand->register_number >= 4)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
    } else {
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    }
    if (!operand->write_mask)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    /* NFSU2 binds `mad` destinations carrying the _x4 result shift
     * together with _sat (r542 guest capture).  ps_1_1 admits exactly the
     * multiply shifts _x2 and _x4; _x8 arrived with ps_1_4.  Of the divide
     * result modifiers only _d2 is legal ps_1_1, and none are supported
     * here: every other shift encoding stays refused fail-closed with
     * attribution until a title evidences it, like the rest of the
     * unlowered ps_1_x surface. */
    if (operand->destination_shift > 2)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if ((operand->destination_modifier & ~QXP_SHADER_DSTMOD_SATURATE) != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    return TRUE;
}

/*
 * `tex tN` is stage-implicit in ps_1_1: it samples the 2D sampler N at
 * the coordinate tN arrived with and overwrites tN with the sample.  No
 * sampler declaration exists in the profile, and tN is fully defined
 * afterwards.
 */
static BOOL qxp_validate_ps11_texture(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;

    destination = &instruction->destinations[0];
    if (destination->register_file != QXP_SHADER_REG_TEXTURE)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (destination->register_number >= 4)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    if (destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    context->ps_texture_masks[destination->register_number] = 15;
    context->ps_texture_addressed[destination->register_number] = 1;
    return TRUE;
}

/*
 * `texreg2ar tN, tM` samples 2D stage N at the coordinate (tM.a, tM.r);
 * `texreg2gb tN, tM` uses (tM.g, tM.b).  NFSU2 depends on both forms for
 * its dependent palette-style reads (r543 guest capture).  The source
 * stage must already hold a texture-address result from a previous
 * tex/texcoord/texreg2*; reading a stage that only carries its
 * interpolated coordinate is undefined in the profile and stays refused.
 */
static BOOL qxp_validate_ps11_texreg(
    QXP_DECODE_CONTEXT *context, const QXP_SHADER_INSTRUCTION *instruction)
{
    const QXP_SHADER_OPERAND *destination;
    const QXP_SHADER_OPERAND *source;
    DWORD i;

    destination = &instruction->destinations[0];
    if (destination->register_file != QXP_SHADER_REG_TEXTURE)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (destination->register_number >= 4)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    if (destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);

    source = &instruction->sources[0];
    if (source->register_file != QXP_SHADER_REG_TEXTURE)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (source->register_number >= 4)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    if (source->source_modifier != QXP_SHADER_SRCMOD_NONE)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    for (i = 0; i < 4; ++i) {
        if (source->swizzle[i] != i)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
    }
    if (!context->ps_texture_addressed[source->register_number])
        return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                        instruction->token_offset);

    context->ps_texture_masks[destination->register_number] = 15;
    context->ps_texture_addressed[destination->register_number] = 1;
    return TRUE;
}

/*
 * ps_1_1 arithmetic has no colour or depth output registers (r0 is the
 * result, enforced by qxp_validate_outputs), no ps_2_0 read-port rules,
 * and none of the ps_2_0 scalar or matrix forms.  r# reads still require
 * a prior write; t# reads are always defined.
 */
static BOOL qxp_validate_ps11_arithmetic(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    DWORD i;

    destination = instruction->destination_count ?
        &instruction->destinations[0] : (QXP_SHADER_OPERAND *)0;
    if (destination &&
        !qxp_validate_ps11_destination(context, instruction, destination))
        return FALSE;
    for (i = 0; i < instruction->source_count; ++i) {
        if (!qxp_validate_float_source(context, instruction,
                                       &instruction->sources[i]))
            return FALSE;
    }
    if (!qxp_validate_pixel_source_components(context, instruction))
        return FALSE;
    qxp_record_pixel_temp_write(context, instruction);
    return TRUE;
}

static BOOL qxp_validate_pixel_texture(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    DWORD sampler_type;
    DWORD required_mask;
    DWORD texture_dependency;

    if (qxp_stream_is_ps11(context))
        return qxp_validate_ps11_texture(context, instruction);

    destination = &instruction->destinations[0];
    if (instruction->opcode == D3DSIO_TEXKILL) {
        if (destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0 ||
            destination->write_mask != 15)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        if (destination->register_file == QXP_SHADER_REG_TEMP) {
            if (destination->register_number >= QXP_SHADER_MAX_PS_TEMPS)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                                instruction->token_offset);
            qxp_record_pixel_temp_requirement(
                context, destination->register_number, 1);
        } else if (destination->register_file == QXP_SHADER_REG_TEXTURE) {
            if (destination->register_number >= 8)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                                instruction->token_offset);
        } else {
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        }
        if (!qxp_validate_pixel_register_components(
                context, instruction, destination->register_file,
                destination->register_number, 7)) return FALSE;
        return TRUE;
    }

    if (!qxp_validate_pixel_temp_destination(
            context, instruction, destination,
            QXP_SHADER_DSTMOD_PARTIAL_PRECISION)) return FALSE;
    if (destination->write_mask != 15)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if ((instruction->control != QXP_SHADER_CONTROL_NONE &&
         instruction->control != QXP_SHADER_CONTROL_TEX_PROJECT &&
         instruction->control != QXP_SHADER_CONTROL_TEX_BIAS))
        return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                        instruction->token_offset);

    if (!qxp_validate_float_source(context, instruction,
                                   &instruction->sources[0])) return FALSE;
    if ((instruction->sources[0].register_file != QXP_SHADER_REG_TEMP &&
         instruction->sources[0].register_file != QXP_SHADER_REG_TEXTURE) ||
        instruction->sources[0].source_modifier != QXP_SHADER_SRCMOD_NONE ||
        !qxp_swizzle_is_identity(&instruction->sources[0]) ||
        instruction->sources[0].relative.present)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if (!qxp_validate_exact_source(
            context, instruction, &instruction->sources[1],
            QXP_SHADER_REG_SAMPLER, 16, 1UL << QXP_SHADER_SRCMOD_NONE,
            QXP_SHADER_OPERAND_SOURCE)) return FALSE;
    if (!(context->ps_sampler_declarations &
          (1UL << instruction->sources[1].register_number)))
        return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                        instruction->token_offset);

    sampler_type = context->ps_sampler_types[
        instruction->sources[1].register_number];
    required_mask = sampler_type == QXP_SHADER_SAMPLER_2D ? 3 : 7;
    if (instruction->control == QXP_SHADER_CONTROL_TEX_PROJECT ||
        instruction->control == QXP_SHADER_CONTROL_TEX_BIAS)
        required_mask |= 8;
    if (!qxp_validate_pixel_register_components(
            context, instruction, instruction->sources[0].register_file,
            instruction->sources[0].register_number, required_mask))
        return FALSE;
    if (!qxp_validate_pixel_texture_dependency(
            context, instruction, required_mask, &texture_dependency))
        return FALSE;
    qxp_record_pixel_texture_write(context, instruction,
                                   texture_dependency);
    return TRUE;
}

static BOOL qxp_validate_pixel_arithmetic(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    DWORD allowed_modifiers;
    DWORD i;

    if (qxp_stream_is_ps11(context))
        return qxp_validate_ps11_arithmetic(context, instruction);

    destination = instruction->destination_count ?
        &instruction->destinations[0] : (QXP_SHADER_OPERAND *)0;
    if (destination &&
        (destination->register_file == QXP_SHADER_REG_COLOROUT ||
         destination->register_file == QXP_SHADER_REG_DEPTHOUT)) {
        if (!qxp_validate_float_source(context, instruction,
                                       &instruction->sources[0]))
            return FALSE;
        if (!qxp_validate_pixel_output(context, instruction)) return FALSE;
        return qxp_validate_pixel_source_components(context, instruction);
    }

    if (destination) {
        allowed_modifiers = QXP_SHADER_DSTMOD_SATURATE |
                            QXP_SHADER_DSTMOD_PARTIAL_PRECISION;
        if (instruction->opcode == D3DSIO_FRC ||
            instruction->opcode == D3DSIO_SINCOS)
            allowed_modifiers = QXP_SHADER_DSTMOD_PARTIAL_PRECISION;
        if (!qxp_validate_pixel_temp_destination(
                context, instruction, destination, allowed_modifiers))
            return FALSE;
    }

    for (i = 0; i < instruction->source_count; ++i) {
        if (!qxp_validate_float_source(context, instruction,
                                       &instruction->sources[i]))
            return FALSE;
    }
    if (!qxp_validate_read_ports(context, instruction)) return FALSE;
    if (qxp_is_scalar_opcode(instruction->opcode) &&
        !qxp_swizzle_is_replicate(&instruction->sources[0]))
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (instruction->opcode == D3DSIO_POW) {
        if (!qxp_swizzle_is_replicate(&instruction->sources[0]) ||
            !qxp_swizzle_is_replicate(&instruction->sources[1]) ||
            qxp_operand_aliases(destination, &instruction->sources[1]))
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_NRM) {
        if (qxp_operand_aliases(destination, &instruction->sources[0]))
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        if (destination->write_mask != 7 && destination->write_mask != 15)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_DP2ADD &&
        !qxp_swizzle_is_replicate(&instruction->sources[2]))
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);
    if (instruction->opcode == D3DSIO_SINCOS) {
        QXP_SHADER_OPERAND *source1;
        QXP_SHADER_OPERAND *source2;

        source1 = &instruction->sources[1];
        source2 = &instruction->sources[2];
        if ((destination->write_mask != 1 &&
             destination->write_mask != 2 &&
             destination->write_mask != 3) ||
            qxp_operand_aliases(destination, &instruction->sources[0]) ||
            !qxp_swizzle_is_replicate(&instruction->sources[0]) ||
            source1->register_file != QXP_SHADER_REG_CONST ||
            source2->register_file != QXP_SHADER_REG_CONST ||
            source1->register_number >= 32 ||
            source2->register_number >= 32 ||
            source1->register_number == source2->register_number ||
            !qxp_swizzle_is_identity(source1) ||
            !qxp_swizzle_is_identity(source2) ||
            source1->source_modifier != QXP_SHADER_SRCMOD_NONE ||
            source2->source_modifier != QXP_SHADER_SRCMOD_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
    }
    if (instruction->opcode == D3DSIO_CRS &&
        !qxp_validate_cross_product(context, instruction))
        return FALSE;
    if (qxp_is_matrix_opcode(instruction->opcode) &&
        !qxp_validate_matrix(context, instruction))
        return FALSE;
    if (!qxp_validate_pixel_source_components(context, instruction))
        return FALSE;
    qxp_record_pixel_temp_write(context, instruction);
    return TRUE;
}

static BOOL qxp_validate_declaration(QXP_DECODE_CONTEXT *context,
                                     QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    DWORD raw;
    DWORD usage;
    DWORD usage_index;
    DWORD sampler_type;
    DWORD bit;

    if (context->executable_seen)
        return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                        instruction->token_offset);
    destination = &instruction->destinations[0];
    raw = instruction->declaration.raw_token;
    if (context->ir->stage == QXP_SHADER_STAGE_VERTEX) {
        if ((raw & ~(QXP_PARAMETER_MARKER | D3DSP_DCL_USAGE_MASK |
                     D3DSP_DCL_USAGEINDEX_MASK)) != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset + 1);
        usage = (raw & D3DSP_DCL_USAGE_MASK) >> D3DSP_DCL_USAGE_SHIFT;
        usage_index = (raw & D3DSP_DCL_USAGEINDEX_MASK) >>
                      D3DSP_DCL_USAGEINDEX_SHIFT;
        if (usage > 13)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset + 1);
        if (destination->register_file != QXP_SHADER_REG_INPUT)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        if (destination->register_number >= 16)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        if (destination->write_mask != 15 ||
            destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        bit = 1UL << destination->register_number;
        if (context->vs_input_declarations & bit)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
        if (context->vs_semantics[usage] & (1UL << usage_index))
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
        context->vs_input_declarations |= bit;
        context->vs_semantics[usage] |= 1UL << usage_index;
        instruction->declaration.kind = QXP_SHADER_DECLARATION_INPUT;
        instruction->declaration.usage = usage;
        instruction->declaration.usage_index = usage_index;
    } else if (destination->register_file == QXP_SHADER_REG_SAMPLER) {
        if ((raw & ~(QXP_PARAMETER_MARKER | D3DSP_TEXTURETYPE_MASK)) != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset + 1);
        sampler_type = (raw & D3DSP_TEXTURETYPE_MASK) >>
                       D3DSP_TEXTURETYPE_SHIFT;
        if (sampler_type != QXP_SHADER_SAMPLER_2D &&
            sampler_type != QXP_SHADER_SAMPLER_CUBE &&
            sampler_type != QXP_SHADER_SAMPLER_VOLUME)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset + 1);
        if (destination->register_number >= 16)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            instruction->token_offset);
        if (destination->write_mask != 15 ||
            destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
            destination->destination_shift != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        bit = 1UL << destination->register_number;
        if (context->ps_sampler_declarations & bit)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset);
        context->ps_sampler_declarations |= bit;
        context->ps_sampler_types[destination->register_number] =
            (BYTE)sampler_type;
        instruction->declaration.kind = QXP_SHADER_DECLARATION_SAMPLER;
        instruction->declaration.sampler_type = sampler_type;
    } else {
        if (raw != QXP_PARAMETER_MARKER)
            return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                            instruction->token_offset + 1);
        if (!destination->write_mask ||
            destination->destination_shift != 0 ||
            (destination->destination_modifier &
             ~(QXP_SHADER_DSTMOD_PARTIAL_PRECISION |
               QXP_SHADER_DSTMOD_CENTROID)) != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                            instruction->token_offset);
        if (destination->register_file == QXP_SHADER_REG_INPUT) {
            if (destination->register_number >= 2)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                                instruction->token_offset);
            bit = 1UL << destination->register_number;
            if (context->ps_input_declarations & bit)
                return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                                instruction->token_offset);
            context->ps_input_declarations |= bit;
            context->ps_input_masks[destination->register_number] =
                destination->write_mask;
        } else if (destination->register_file == QXP_SHADER_REG_TEXTURE) {
            if (destination->register_number >= 8)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                                instruction->token_offset);
            bit = 1UL << destination->register_number;
            if (context->ps_texture_declarations & bit)
                return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                                instruction->token_offset);
            context->ps_texture_declarations |= bit;
            context->ps_texture_masks[destination->register_number] =
                destination->write_mask;
        } else {
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            instruction->token_offset);
        }
        instruction->declaration.kind = QXP_SHADER_DECLARATION_INPUT;
    }

    instruction->declaration.present = TRUE;
    ++context->ir->declaration_count;
    return TRUE;
}

static BOOL qxp_validate_definition(QXP_DECODE_CONTEXT *context,
                                    QXP_SHADER_INSTRUCTION *instruction)
{
    QXP_SHADER_OPERAND *destination;
    DWORD limit;
    DWORD expected_file;

    if (context->executable_seen)
        return qxp_fail(context, QXP_SHADER_DECODE_DEFINITION,
                        instruction->token_offset);
    destination = &instruction->destinations[0];
    if (destination->write_mask != 15 ||
        destination->destination_modifier != QXP_SHADER_DSTMOD_NONE ||
        destination->destination_shift != 0)
        return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                        instruction->token_offset);

    expected_file = QXP_SHADER_REG_CONST;
    limit = context->ir->stage == QXP_SHADER_STAGE_VERTEX ? 256 :
            qxp_stream_is_ps11(context) ? 8 : 32;
    if (instruction->opcode == D3DSIO_DEFI) {
        if (context->ir->stage != QXP_SHADER_STAGE_VERTEX)
            return qxp_fail(context, QXP_SHADER_DECODE_UNSUPPORTED_OPCODE,
                            instruction->token_offset);
        expected_file = QXP_SHADER_REG_CONSTINT;
        limit = 16;
    } else if (instruction->opcode == D3DSIO_DEFB) {
        if (context->ir->stage != QXP_SHADER_STAGE_VERTEX)
            return qxp_fail(context, QXP_SHADER_DECODE_UNSUPPORTED_OPCODE,
                            instruction->token_offset);
        expected_file = QXP_SHADER_REG_CONSTBOOL;
        limit = 16;
    }

    if (destination->register_file != expected_file)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        instruction->token_offset);
    if ((DWORD)destination->register_number >= limit)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                        instruction->token_offset);
    /* Repeated definitions are legal; ordered IR preserves last-wins. */
    ++context->ir->definition_count;
    return TRUE;
}

static BOOL qxp_validate_vertex_flow(QXP_DECODE_CONTEXT *context,
                                     QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD opcode;

    opcode = instruction->opcode;
    if (opcode == D3DSIO_CALL || opcode == D3DSIO_LABEL) {
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[0],
                QXP_SHADER_REG_LABEL, QXP_SHADER_MAX_LABELS,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_LABEL)) return FALSE;
    } else if (opcode == D3DSIO_CALLNZ) {
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[0],
                QXP_SHADER_REG_LABEL, QXP_SHADER_MAX_LABELS,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_LABEL)) return FALSE;
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[1],
                QXP_SHADER_REG_CONSTBOOL, 16,
                (1UL << QXP_SHADER_SRCMOD_NONE) |
                (1UL << QXP_SHADER_SRCMOD_NOT),
                QXP_SHADER_OPERAND_CONDITION)) return FALSE;
    } else if (opcode == D3DSIO_LOOP) {
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[0],
                QXP_SHADER_REG_LOOP, 1,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_LOOP_CONTROL)) return FALSE;
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[1],
                QXP_SHADER_REG_CONSTINT, 16,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_LOOP_CONTROL)) return FALSE;
    } else if (opcode == D3DSIO_REP) {
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[0],
                QXP_SHADER_REG_CONSTINT, 16,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_LOOP_CONTROL)) return FALSE;
    } else if (opcode == D3DSIO_IF) {
        if (!qxp_validate_exact_source(
                context, instruction, &instruction->sources[0],
                QXP_SHADER_REG_CONSTBOOL, 16,
                1UL << QXP_SHADER_SRCMOD_NONE,
                QXP_SHADER_OPERAND_CONDITION)) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_validate_relative_sources(
    QXP_DECODE_CONTEXT *context, QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD i;
    DWORD relative_file;
    DWORD relative_component;

    relative_file = QXP_SHADER_INDEX_NONE;
    relative_component = QXP_SHADER_INDEX_NONE;
    for (i = 0; i < instruction->source_count; ++i) {
        const QXP_SHADER_RELATIVE_OPERAND *relative;
        DWORD component;

        relative = &instruction->sources[i].relative;
        if (!relative->present) continue;
        component = relative->register_file == QXP_SHADER_REG_LOOP ?
                    0 : relative->swizzle[0];
        if (relative_file == QXP_SHADER_INDEX_NONE) {
            relative_file = relative->register_file;
            relative_component = component;
        } else if (relative_file != relative->register_file ||
                   relative_component != component) {
            return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                            instruction->token_offset);
        }
    }
    return TRUE;
}

static BOOL qxp_validate_instruction(QXP_DECODE_CONTEXT *context,
                                     QXP_SHADER_INSTRUCTION *instruction,
                                     const QXP_OPCODE_INFO *info)
{
    DWORD opcode;

    if (!qxp_validate_relative_sources(context, instruction)) return FALSE;
    opcode = instruction->opcode;
    if (info->form == QXP_FORM_DCL)
        return qxp_validate_declaration(context, instruction);
    if (info->form == QXP_FORM_DEF)
        return qxp_validate_definition(context, instruction);

    context->executable_seen = TRUE;
    if (context->ir->stage == QXP_SHADER_STAGE_PIXEL) {
        if (opcode == D3DSIO_TEX || opcode == D3DSIO_TEXKILL ||
            opcode == D3DSIO_TEXCOORD)
            return qxp_validate_pixel_texture(context, instruction);
        if ((opcode == D3DSIO_TEXREG2AR || opcode == D3DSIO_TEXREG2GB) &&
            qxp_stream_is_ps11(context))
            return qxp_validate_ps11_texreg(context, instruction);
        return qxp_validate_pixel_arithmetic(context, instruction);
    }

    if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ ||
        opcode == D3DSIO_LOOP || opcode == D3DSIO_RET ||
        opcode == D3DSIO_ENDLOOP || opcode == D3DSIO_LABEL ||
        opcode == D3DSIO_REP || opcode == D3DSIO_ENDREP ||
        opcode == D3DSIO_IF || opcode == D3DSIO_ELSE ||
        opcode == D3DSIO_ENDIF)
        return qxp_validate_vertex_flow(context, instruction);
    return qxp_validate_vertex_arithmetic(context, instruction);
}

static BOOL qxp_push_control(QXP_DECODE_CONTEXT *context, DWORD index)
{
    if (context->control_depth >= QXP_SHADER_MAX_INSTRUCTIONS)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_CAPACITY,
                        context->ir->instructions[index].token_offset);
    context->control_stack[context->control_depth++] = index;
    return TRUE;
}

static DWORD qxp_control_kind(DWORD opcode)
{
    if (opcode == D3DSIO_IF) return QXP_CONTROL_IF;
    if (opcode == D3DSIO_LOOP) return QXP_CONTROL_LOOP;
    if (opcode == D3DSIO_REP) return QXP_CONTROL_REP;
    return 0;
}

static BOOL qxp_add_static_flow(QXP_DECODE_CONTEXT *context,
                                DWORD token_offset)
{
    ++context->static_flow_count;
    if (context->static_flow_count > 16)
        return qxp_fail(context, QXP_SHADER_DECODE_CONTROL, token_offset);
    return TRUE;
}

static BOOL qxp_apply_control(QXP_DECODE_CONTEXT *context, DWORD index)
{
    QXP_SHADER_INSTRUCTION *instruction;
    QXP_SHADER_INSTRUCTION *opener;
    DWORD opener_index;
    DWORD opcode;
    DWORD expected_kind;

    instruction = &context->ir->instructions[index];
    opcode = instruction->opcode;
    if (opcode == D3DSIO_IF) {
        if (!qxp_add_static_flow(context, instruction->token_offset))
            return FALSE;
        return qxp_push_control(context, index);
    }
    if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
        if (context->loop_depth != 0)
            return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                            instruction->token_offset);
        if (!qxp_add_static_flow(context, instruction->token_offset))
            return FALSE;
        ++context->loop_depth;
        return qxp_push_control(context, index);
    }
    if (opcode == D3DSIO_ELSE) {
        if (!qxp_add_static_flow(context, instruction->token_offset))
            return FALSE;
        if (!context->control_depth)
            return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                            instruction->token_offset);
        opener_index = context->control_stack[context->control_depth - 1];
        opener = &context->ir->instructions[opener_index];
        if (opener->opcode != D3DSIO_IF ||
            opener->control_else != QXP_SHADER_INDEX_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                            instruction->token_offset);
        opener->control_else = index;
        instruction->control_else = opener_index;
        return TRUE;
    }
    if (opcode == D3DSIO_ENDIF || opcode == D3DSIO_ENDLOOP ||
        opcode == D3DSIO_ENDREP) {
        if (!context->control_depth)
            return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                            instruction->token_offset);
        opener_index = context->control_stack[context->control_depth - 1];
        opener = &context->ir->instructions[opener_index];
        expected_kind = opcode == D3DSIO_ENDIF ? QXP_CONTROL_IF :
                        (opcode == D3DSIO_ENDLOOP ? QXP_CONTROL_LOOP :
                                                   QXP_CONTROL_REP);
        if (qxp_control_kind(opener->opcode) != expected_kind)
            return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                            instruction->token_offset);
        --context->control_depth;
        if (opcode != D3DSIO_ENDIF) --context->loop_depth;
        opener->control_match = index;
        instruction->control_match = opener_index;
        if (opener->control_else != QXP_SHADER_INDEX_NONE) {
            QXP_SHADER_INSTRUCTION *else_instruction;

            else_instruction =
                &context->ir->instructions[opener->control_else];
            else_instruction->control_match = index;
            instruction->control_else = opener->control_else;
        }
        return TRUE;
    }
    if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ)
        return qxp_add_static_flow(context, instruction->token_offset);
    if ((opcode == D3DSIO_RET || opcode == D3DSIO_LABEL) &&
        context->control_depth)
        return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                        instruction->token_offset);
    return TRUE;
}

static BOOL qxp_record_label(QXP_DECODE_CONTEXT *context, DWORD index)
{
    QXP_SHADER_INSTRUCTION *instruction;
    DWORD label;

    instruction = &context->ir->instructions[index];
    if (instruction->opcode != D3DSIO_LABEL) return TRUE;
    label = instruction->sources[0].register_number;
    if (context->labels[label] != QXP_SHADER_INDEX_NONE)
        return qxp_fail(context, QXP_SHADER_DECODE_DUPLICATE_LABEL,
                        instruction->token_offset);
    context->labels[label] = index;
    instruction->label_target = index;
    return TRUE;
}

static BOOL qxp_validate_label_layout(QXP_DECODE_CONTEXT *context)
{
    BYTE label_referenced[QXP_SHADER_MAX_LABELS];
    DWORD i;
    BOOL in_subroutine;
    BOOL after_return;
    BOOL saw_label;

    qxp_zero_memory(label_referenced, (DWORD)sizeof(label_referenced));
    in_subroutine = FALSE;
    after_return = FALSE;
    saw_label = FALSE;
    for (i = 0; i < context->ir->instruction_count; ++i) {
        QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &context->ir->instructions[i];
        opcode = instruction->opcode;
        if (opcode == D3DSIO_LABEL) {
            if (i == 0 ||
                context->ir->instructions[i - 1].opcode != D3DSIO_RET)
                return qxp_fail(context, QXP_SHADER_DECODE_CALL_TARGET,
                                instruction->token_offset);
            in_subroutine = TRUE;
            after_return = FALSE;
            saw_label = TRUE;
        } else if (after_return) {
            return qxp_fail(context, QXP_SHADER_DECODE_CALL_TARGET,
                            instruction->token_offset);
        }

        if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ) {
            DWORD label;
            DWORD target;

            label = instruction->sources[0].register_number;
            target = context->labels[label];
            if (target == QXP_SHADER_INDEX_NONE)
                return qxp_fail(context, QXP_SHADER_DECODE_MISSING_LABEL,
                                instruction->token_offset);
            if (target <= i || in_subroutine)
                return qxp_fail(context, QXP_SHADER_DECODE_CALL_TARGET,
                                instruction->token_offset);
            instruction->label_target = target;
            label_referenced[label] = 1;
        }
        if (opcode == D3DSIO_RET) after_return = TRUE;
    }

    if (saw_label && context->ir->instruction_count &&
        context->ir->instructions[context->ir->instruction_count - 1].opcode !=
            D3DSIO_RET)
        return qxp_fail(
            context, QXP_SHADER_DECODE_CALL_TARGET,
            context->ir->instructions[context->ir->instruction_count - 1].
                token_offset);
    for (i = 0; i < context->ir->instruction_count; ++i) {
        QXP_SHADER_INSTRUCTION *instruction;
        DWORD label;

        instruction = &context->ir->instructions[i];
        if (instruction->opcode != D3DSIO_LABEL) continue;
        label = instruction->sources[0].register_number;
        if (!label_referenced[label])
            return qxp_fail(context, QXP_SHADER_DECODE_CALL_TARGET,
                            instruction->token_offset);
    }
    return TRUE;
}

static BOOL qxp_instruction_is_in_loop(const QXP_SHADER_IR *ir,
                                       DWORD instruction_index)
{
    DWORD i;

    for (i = 0; i < ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[i];
        if (instruction->opcode == D3DSIO_LOOP &&
            instruction->control_match != QXP_SHADER_INDEX_NONE &&
            i < instruction_index &&
            instruction_index < instruction->control_match)
            return TRUE;
    }
    return FALSE;
}

static BOOL qxp_instruction_is_in_iteration(const QXP_SHADER_IR *ir,
                                            DWORD instruction_index)
{
    DWORD i;

    for (i = 0; i < ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &ir->instructions[i];
        if ((instruction->opcode == D3DSIO_LOOP ||
             instruction->opcode == D3DSIO_REP) &&
            instruction->control_match != QXP_SHADER_INDEX_NONE &&
            i < instruction_index &&
            instruction_index < instruction->control_match)
            return TRUE;
    }
    return FALSE;
}

static BOOL qxp_subroutine_has_iteration(const QXP_SHADER_IR *ir,
                                         DWORD label_index)
{
    DWORD i;

    for (i = label_index + 1; i < ir->instruction_count; ++i) {
        DWORD opcode;

        opcode = ir->instructions[i].opcode;
        if (opcode == D3DSIO_RET || opcode == D3DSIO_LABEL) break;
        if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) return TRUE;
    }
    return FALSE;
}

static BOOL qxp_validate_call_iteration_nesting(QXP_DECODE_CONTEXT *context)
{
    DWORD i;

    for (i = 0; i < context->ir->instruction_count; ++i) {
        QXP_SHADER_INSTRUCTION *instruction;

        instruction = &context->ir->instructions[i];
        if (instruction->opcode != D3DSIO_CALL &&
            instruction->opcode != D3DSIO_CALLNZ)
            continue;
        if (qxp_instruction_is_in_iteration(context->ir, i) &&
            qxp_subroutine_has_iteration(context->ir,
                                         instruction->label_target))
            return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                            instruction->token_offset);
    }
    return TRUE;
}

static BOOL qxp_validate_loop_relative_scope(QXP_DECODE_CONTEXT *context)
{
    DWORD i;

    if (context->ir->stage != QXP_SHADER_STAGE_VERTEX) return TRUE;
    for (i = 0; i < context->ir->instruction_count; ++i) {
        QXP_SHADER_INSTRUCTION *instruction;
        DWORD j;
        BOOL uses_loop;
        DWORD subroutine_label;

        instruction = &context->ir->instructions[i];
        uses_loop = FALSE;
        for (j = 0; j < instruction->source_count; ++j) {
            if (instruction->sources[j].relative.present &&
                instruction->sources[j].relative.register_file ==
                    QXP_SHADER_REG_LOOP) {
                uses_loop = TRUE;
                break;
            }
        }
        if (!uses_loop || qxp_instruction_is_in_loop(context->ir, i))
            continue;

        subroutine_label = QXP_SHADER_INDEX_NONE;
        for (j = 0; j < i; ++j) {
            if (context->ir->instructions[j].opcode == D3DSIO_LABEL)
                subroutine_label = j;
        }
        if (subroutine_label == QXP_SHADER_INDEX_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                            instruction->token_offset);

        for (j = 0; j < context->ir->instruction_count; ++j) {
            QXP_SHADER_INSTRUCTION *call;

            call = &context->ir->instructions[j];
            if ((call->opcode == D3DSIO_CALL ||
                 call->opcode == D3DSIO_CALLNZ) &&
                call->label_target == subroutine_label &&
                !qxp_instruction_is_in_loop(context->ir, j))
                return qxp_fail(context,
                                QXP_SHADER_DECODE_RELATIVE_ADDRESS,
                                instruction->token_offset);
        }
    }
    return TRUE;
}

static BOOL qxp_validate_declared_inputs(QXP_DECODE_CONTEXT *context)
{
    DWORD i;
    DWORD j;

    /* ps_1_1 has no dcl instructions; v# and t# bind implicitly. */
    if (qxp_stream_is_ps11(context)) return TRUE;

    for (i = 0; i < context->ir->instruction_count; ++i) {
        QXP_SHADER_INSTRUCTION *instruction;

        instruction = &context->ir->instructions[i];
        for (j = 0; j < instruction->source_count; ++j) {
            QXP_SHADER_OPERAND *source;
            DWORD bit;

            source = &instruction->sources[j];
            if (context->ir->stage == QXP_SHADER_STAGE_VERTEX &&
                source->register_file == QXP_SHADER_REG_INPUT) {
                bit = 1UL << source->register_number;
                if (!(context->vs_input_declarations & bit))
                    return qxp_fail(context,
                                    QXP_SHADER_DECODE_DECLARATION,
                                    instruction->token_offset);
            }
            if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
                source->register_file == QXP_SHADER_REG_INPUT) {
                bit = 1UL << source->register_number;
                if (!(context->ps_input_declarations & bit))
                    return qxp_fail(context,
                                    QXP_SHADER_DECODE_DECLARATION,
                                    instruction->token_offset);
            }
            if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
                source->register_file == QXP_SHADER_REG_TEXTURE) {
                bit = 1UL << source->register_number;
                if (!(context->ps_texture_declarations & bit))
                    return qxp_fail(context,
                                    QXP_SHADER_DECODE_DECLARATION,
                                    instruction->token_offset);
            }
            if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
                source->register_file == QXP_SHADER_REG_SAMPLER) {
                bit = 1UL << source->register_number;
                if (!(context->ps_sampler_declarations & bit))
                    return qxp_fail(context,
                                    QXP_SHADER_DECODE_DECLARATION,
                                    instruction->token_offset);
            }
        }
        if (context->ir->stage == QXP_SHADER_STAGE_PIXEL &&
            instruction->opcode == D3DSIO_TEXKILL &&
            instruction->destinations[0].register_file ==
                QXP_SHADER_REG_TEXTURE) {
            DWORD bit;

            bit = 1UL << instruction->destinations[0].register_number;
            if (!(context->ps_texture_declarations & bit))
                return qxp_fail(context, QXP_SHADER_DECODE_DECLARATION,
                                instruction->token_offset);
        }
    }
    return TRUE;
}

static void qxp_vs_state_zero(QXP_VS_COMPONENT_STATE *state)
{
    qxp_zero_memory(state, (DWORD)sizeof(*state));
}

static void qxp_vs_state_all(QXP_VS_COMPONENT_STATE *state)
{
    DWORD i;

    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i) state->temp_masks[i] = 15;
    state->address_mask = 15;
}

static void qxp_vs_state_intersect(
    QXP_VS_COMPONENT_STATE *destination,
    const QXP_VS_COMPONENT_STATE *left,
    const QXP_VS_COMPONENT_STATE *right)
{
    DWORD i;

    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i)
        destination->temp_masks[i] =
            (BYTE)(left->temp_masks[i] & right->temp_masks[i]);
    destination->address_mask =
        (BYTE)(left->address_mask & right->address_mask);
}

static BOOL qxp_vs_state_has_bits(const QXP_VS_COMPONENT_STATE *state)
{
    DWORD i;

    if (state->address_mask) return TRUE;
    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i) {
        if (state->temp_masks[i]) return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_state_add(QXP_VS_COMPONENT_STATE *destination,
                             const QXP_VS_COMPONENT_STATE *source)
{
    DWORD i;
    BOOL changed;

    changed = FALSE;
    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i) {
        BYTE combined;

        combined = (BYTE)(destination->temp_masks[i] |
                          source->temp_masks[i]);
        if (combined != destination->temp_masks[i]) changed = TRUE;
        destination->temp_masks[i] = combined;
    }
    if ((BYTE)(destination->address_mask | source->address_mask) !=
        destination->address_mask) changed = TRUE;
    destination->address_mask =
        (BYTE)(destination->address_mask | source->address_mask);
    return changed;
}

static BOOL qxp_vs_state_includes(
    const QXP_VS_COMPONENT_STATE *available,
    const QXP_VS_COMPONENT_STATE *required)
{
    DWORD i;

    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i) {
        if ((DWORD)required->temp_masks[i] &
            ~(DWORD)available->temp_masks[i]) return FALSE;
    }
    if ((DWORD)required->address_mask &
        ~(DWORD)available->address_mask) return FALSE;
    return TRUE;
}

static void qxp_vs_require_temp(
    const QXP_VS_COMPONENT_STATE *state,
    QXP_VS_COMPONENT_STATE *missing, DWORD register_number, DWORD mask,
    DWORD token_offset, DWORD *first_missing_offset)
{
    DWORD absent;

    /* Register validation has already established this invariant. */
    if (register_number >= QXP_VS_TEMP_COUNT) return;
    absent = mask & ~(DWORD)state->temp_masks[register_number] & 15UL;
    if (!absent) return;
    missing->temp_masks[register_number] =
        (BYTE)(missing->temp_masks[register_number] | absent);
    if (*first_missing_offset == QXP_SHADER_INDEX_NONE)
        *first_missing_offset = token_offset;
}

static void qxp_vs_require_address(
    const QXP_VS_COMPONENT_STATE *state,
    QXP_VS_COMPONENT_STATE *missing, DWORD component,
    DWORD token_offset, DWORD *first_missing_offset)
{
    DWORD bit;

    component &= 3UL;
    bit = 1UL << component;
    if (state->address_mask & bit) return;
    missing->address_mask = (BYTE)(missing->address_mask | bit);
    if (*first_missing_offset == QXP_SHADER_INDEX_NONE)
        *first_missing_offset = token_offset;
}

static void qxp_vs_require_state(
    const QXP_VS_COMPONENT_STATE *state,
    QXP_VS_COMPONENT_STATE *missing,
    const QXP_VS_COMPONENT_STATE *required,
    DWORD token_offset, DWORD *first_missing_offset)
{
    DWORD i;

    for (i = 0; i < QXP_VS_TEMP_COUNT; ++i)
        qxp_vs_require_temp(state, missing, i, required->temp_masks[i],
                            token_offset, first_missing_offset);
    for (i = 0; i < 4; ++i) {
        if (required->address_mask & (1UL << i))
            qxp_vs_require_address(state, missing, i, token_offset,
                                   first_missing_offset);
    }
}

static DWORD qxp_vertex_source_logical_mask(
    const QXP_SHADER_INSTRUCTION *instruction, DWORD source_index)
{
    DWORD destination_mask;
    DWORD opcode;
    DWORD mask;

    destination_mask = instruction->destination_count ?
        instruction->destinations[0].write_mask : 15;
    opcode = instruction->opcode;
    if (opcode == D3DSIO_DP3) return 7;
    if (opcode == D3DSIO_DP4) return 15;
    if (qxp_is_matrix_opcode(opcode))
        return source_index == 0 ?
            (qxp_matrix_columns(opcode) == 4 ? 15 : 7) : 0;
    if (opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
        opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
        opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP ||
        opcode == D3DSIO_POW)
        return 1;
    if (opcode == D3DSIO_SINCOS) return source_index ? 15 : 1;
    if (opcode == D3DSIO_SGN)
        return source_index == 0 ? destination_mask : 0;
    if (opcode == D3DSIO_NRM)
        return 7 | (destination_mask & 8);
    if (opcode == D3DSIO_CRS) {
        mask = 0;
        if (destination_mask & 1) mask |= 6;
        if (destination_mask & 2) mask |= 5;
        if (destination_mask & 4) mask |= 3;
        return mask;
    }
    if (opcode == D3DSIO_DST) {
        mask = 0;
        if (destination_mask & 2) mask |= 2;
        if (source_index == 0 && (destination_mask & 4)) mask |= 4;
        if (source_index == 1 && (destination_mask & 8)) mask |= 8;
        return mask;
    }
    if (opcode == D3DSIO_LIT) {
        mask = 0;
        if (destination_mask & 2) mask |= 1;
        if (destination_mask & 4) mask |= 11;
        return mask;
    }
    return destination_mask;
}

static void qxp_vs_collect_instruction_reads(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    const QXP_VS_COMPONENT_STATE *state,
    QXP_VS_COMPONENT_STATE *missing, DWORD *first_missing_offset)
{
    DWORD i;

    for (i = 0; i < instruction->source_count; ++i) {
        const QXP_SHADER_OPERAND *source;

        source = &instruction->sources[i];
        if (source->relative.present &&
            source->relative.register_file == QXP_SHADER_REG_ADDRESS)
            qxp_vs_require_address(
                state, missing, source->relative.swizzle[0],
                instruction->token_offset, first_missing_offset);
    }

    if (qxp_is_matrix_opcode(instruction->opcode)) {
        const QXP_SHADER_OPERAND *vector;
        const QXP_SHADER_OPERAND *matrix;
        DWORD columns_mask;
        DWORD destination_mask;

        vector = &instruction->sources[0];
        matrix = &instruction->sources[1];
        columns_mask = qxp_matrix_columns(instruction->opcode) == 4 ?
                       15 : 7;
        destination_mask = instruction->destinations[0].write_mask;
        if (vector->register_file == QXP_SHADER_REG_TEMP)
            qxp_vs_require_temp(
                state, missing, vector->register_number,
                qxp_swizzled_component_mask(vector, columns_mask),
                instruction->token_offset, first_missing_offset);
        if (matrix->register_file == QXP_SHADER_REG_TEMP) {
            for (i = 0; i < qxp_matrix_rows(instruction->opcode); ++i) {
                if (destination_mask & (1UL << i))
                    qxp_vs_require_temp(
                        state, missing,
                        (DWORD)matrix->register_number + i,
                        qxp_swizzled_component_mask(matrix, columns_mask),
                        instruction->token_offset, first_missing_offset);
            }
        }
        return;
    }

    for (i = 0; i < instruction->source_count; ++i) {
        const QXP_SHADER_OPERAND *source;
        DWORD logical_mask;

        source = &instruction->sources[i];
        if (source->register_file != QXP_SHADER_REG_TEMP) continue;
        logical_mask = qxp_vertex_source_logical_mask(instruction, i);
        if (!logical_mask) continue;
        qxp_vs_require_temp(
            state, missing, source->register_number,
            qxp_swizzled_component_mask(source, logical_mask),
            instruction->token_offset, first_missing_offset);
    }

    if (instruction->opcode == D3DSIO_CALL ||
        instruction->opcode == D3DSIO_CALLNZ) {
        DWORD label;
        const QXP_VS_SUBROUTINE_SUMMARY *summary;

        label = instruction->sources[0].register_number;
        if (label >= QXP_SHADER_MAX_LABELS) return;
        summary = &context->vs_subroutines[label];
        if (summary->present)
            qxp_vs_require_state(
                state, missing, &summary->required,
                instruction->token_offset, first_missing_offset);
    }
}

static void qxp_vs_apply_call(
    QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction,
    QXP_VS_COMPONENT_STATE *state)
{
    const QXP_VS_SUBROUTINE_SUMMARY *summary;
    DWORD label;
    DWORD i;

    label = instruction->sources[0].register_number;
    if (label >= QXP_SHADER_MAX_LABELS) return;
    summary = &context->vs_subroutines[label];
    if (!summary->present) return;
    if (instruction->opcode == D3DSIO_CALL) {
        for (i = 0; i < QXP_VS_TEMP_COUNT; ++i)
            state->temp_masks[i] = (BYTE)(
                summary->guaranteed.temp_masks[i] |
                (state->temp_masks[i] &
                 summary->available_if_all.temp_masks[i]));
        state->address_mask = (BYTE)(
            summary->guaranteed.address_mask |
            (state->address_mask &
             summary->available_if_all.address_mask));
    } else {
        for (i = 0; i < QXP_VS_TEMP_COUNT; ++i)
            state->temp_masks[i] = (BYTE)(
                state->temp_masks[i] &
                summary->available_if_all.temp_masks[i]);
        state->address_mask = (BYTE)(
            state->address_mask & summary->available_if_all.address_mask);
    }
}

static void qxp_vs_apply_instruction_write(
    const QXP_SHADER_INSTRUCTION *instruction,
    QXP_VS_COMPONENT_STATE *state)
{
    const QXP_SHADER_OPERAND *destination;

    if (instruction->opcode == D3DSIO_SGN) {
        DWORD i;

        for (i = 1; i < 3; ++i) {
            const QXP_SHADER_OPERAND *scratch;

            scratch = &instruction->sources[i];
            if (scratch->register_file == QXP_SHADER_REG_TEMP &&
                scratch->register_number < QXP_VS_TEMP_COUNT)
                state->temp_masks[scratch->register_number] = 0;
        }
    }
    if (!instruction->destination_count) return;
    destination = &instruction->destinations[0];
    if (destination->register_file == QXP_SHADER_REG_TEMP &&
        destination->register_number < QXP_VS_TEMP_COUNT) {
        if (instruction->opcode == D3DSIO_SINCOS)
            state->temp_masks[destination->register_number] =
                (BYTE)((state->temp_masks[destination->register_number] & 8) |
                       destination->write_mask);
        else
            state->temp_masks[destination->register_number] =
                (BYTE)(state->temp_masks[destination->register_number] |
                       destination->write_mask);
    } else if (destination->register_file == QXP_SHADER_REG_ADDRESS &&
               destination->register_number == 0) {
        state->address_mask =
            (BYTE)(state->address_mask | destination->write_mask);
    }
}

static BOOL qxp_vs_analyze_range(
    QXP_DECODE_CONTEXT *context, DWORD start, DWORD end,
    const QXP_VS_COMPONENT_STATE *initial,
    QXP_VS_COMPONENT_STATE *result,
    QXP_VS_COMPONENT_STATE *missing, DWORD *first_missing_offset)
{
    QXP_VS_FLOW_FRAME flow_stack[QXP_VS_DATAFLOW_MAX_DEPTH];
    QXP_VS_COMPONENT_STATE state;
    DWORD flow_depth;
    DWORD i;

    state = *initial;
    qxp_vs_state_zero(missing);
    *first_missing_offset = QXP_SHADER_INDEX_NONE;
    flow_depth = 0;
    for (i = start; i < end; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &context->ir->instructions[i];
        opcode = instruction->opcode;
        qxp_vs_collect_instruction_reads(
            context, instruction, &state, missing, first_missing_offset);
        if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ)
            qxp_vs_apply_call(context, instruction, &state);
        else
            qxp_vs_apply_instruction_write(instruction, &state);

        if (opcode == D3DSIO_IF || opcode == D3DSIO_LOOP ||
            opcode == D3DSIO_REP) {
            QXP_VS_FLOW_FRAME *frame;

            if (flow_depth >= QXP_VS_DATAFLOW_MAX_DEPTH)
                return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                                instruction->token_offset);
            frame = &flow_stack[flow_depth++];
            frame->opcode = opcode;
            frame->saw_else = FALSE;
            frame->entry = state;
            qxp_vs_state_zero(&frame->branch);
        } else if (opcode == D3DSIO_ELSE) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                                instruction->token_offset);
            frame = &flow_stack[flow_depth - 1];
            frame->branch = state;
            frame->saw_else = TRUE;
            state = frame->entry;
        } else if (opcode == D3DSIO_ENDIF) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                                instruction->token_offset);
            frame = &flow_stack[--flow_depth];
            if (frame->saw_else)
                qxp_vs_state_intersect(
                    &state, &frame->branch, &state);
            else
                qxp_vs_state_intersect(
                    &state, &frame->entry, &state);
        } else if (opcode == D3DSIO_ENDLOOP ||
                   opcode == D3DSIO_ENDREP) {
            QXP_VS_FLOW_FRAME *frame;

            if (!flow_depth)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                                instruction->token_offset);
            frame = &flow_stack[--flow_depth];
            qxp_vs_state_intersect(&state, &frame->entry, &state);
        }
    }
    if (flow_depth)
        return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                        context->ir->instructions[start].token_offset);
    *result = state;
    return TRUE;
}

static BOOL qxp_vs_find_range_requirements(
    QXP_DECODE_CONTEXT *context, DWORD start, DWORD end,
    DWORD fallback_offset, QXP_VS_COMPONENT_STATE *required)
{
    QXP_VS_COMPONENT_STATE result;
    QXP_VS_COMPONENT_STATE missing;
    DWORD first_missing_offset;
    DWORD pass;

    qxp_vs_state_zero(required);
    for (pass = 0; pass < QXP_VS_DATAFLOW_MAX_PASSES; ++pass) {
        if (!qxp_vs_analyze_range(
                context, start, end, required, &result, &missing,
                &first_missing_offset)) return FALSE;
        if (!qxp_vs_state_has_bits(&missing)) return TRUE;
        if (!qxp_vs_state_add(required, &missing))
            return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                            first_missing_offset);
    }
    return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                    fallback_offset);
}

static BOOL qxp_build_vs_subroutine_summaries(
    QXP_DECODE_CONTEXT *context)
{
    DWORD i;

    for (i = 0; i < context->ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *label_instruction;
        QXP_VS_SUBROUTINE_SUMMARY *summary;
        QXP_VS_COMPONENT_STATE missing;
        QXP_VS_COMPONENT_STATE initial;
        DWORD first_missing_offset;
        DWORD label;
        DWORD end;

        label_instruction = &context->ir->instructions[i];
        if (label_instruction->opcode != D3DSIO_LABEL) continue;
        label = label_instruction->sources[0].register_number;
        if (label >= QXP_SHADER_MAX_LABELS)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_RANGE,
                            label_instruction->token_offset);
        summary = &context->vs_subroutines[label];
        summary->present = TRUE;
        summary->label_index = i;
        end = i + 1;
        while (end < context->ir->instruction_count &&
               context->ir->instructions[end].opcode != D3DSIO_RET)
            ++end;
        if (end < context->ir->instruction_count) ++end;

        if (!qxp_vs_find_range_requirements(
                context, i, end, label_instruction->token_offset,
                &summary->required)) return FALSE;

        qxp_vs_state_zero(&initial);
        if (!qxp_vs_analyze_range(
                context, i, end, &initial, &summary->guaranteed,
                &missing, &first_missing_offset)) return FALSE;
        qxp_vs_state_all(&initial);
        if (!qxp_vs_analyze_range(
                context, i, end, &initial, &summary->available_if_all,
                &missing, &first_missing_offset)) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_validate_vs_iteration_stability(
    QXP_DECODE_CONTEXT *context)
{
    DWORD i;

    for (i = 0; i < context->ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *opener;
        QXP_VS_COMPONENT_STATE required;
        QXP_VS_COMPONENT_STATE available_if_all;
        QXP_VS_COMPONENT_STATE initial;
        QXP_VS_COMPONENT_STATE result;
        QXP_VS_COMPONENT_STATE missing;
        DWORD first_missing_offset;
        DWORD end;

        opener = &context->ir->instructions[i];
        if (opener->opcode != D3DSIO_LOOP &&
            opener->opcode != D3DSIO_REP) continue;
        end = opener->control_match;
        if (end == QXP_SHADER_INDEX_NONE || end <= i ||
            end >= context->ir->instruction_count)
            return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                            opener->token_offset);

        if (!qxp_vs_find_range_requirements(
                context, i + 1, end, opener->token_offset, &required))
            return FALSE;
        qxp_vs_state_all(&initial);
        if (!qxp_vs_analyze_range(
                context, i + 1, end, &initial, &available_if_all,
                &missing, &first_missing_offset)) return FALSE;
        if (qxp_vs_state_includes(&available_if_all, &required)) continue;

        if (!qxp_vs_analyze_range(
                context, i + 1, end, &available_if_all, &result,
                &missing, &first_missing_offset)) return FALSE;
        if (qxp_vs_state_has_bits(&missing) &&
            first_missing_offset != QXP_SHADER_INDEX_NONE)
            return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                            first_missing_offset);
        return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                        opener->token_offset);
    }
    return TRUE;
}

static BOOL qxp_validate_vertex_definite_assignment(
    QXP_DECODE_CONTEXT *context)
{
    QXP_VS_COMPONENT_STATE initial;
    QXP_VS_COMPONENT_STATE result;
    QXP_VS_COMPONENT_STATE missing;
    DWORD first_missing_offset;
    DWORD main_end;
    DWORD i;

    if (context->ir->stage != QXP_SHADER_STAGE_VERTEX) return TRUE;
    if (!qxp_build_vs_subroutine_summaries(context)) return FALSE;
    if (!qxp_validate_vs_iteration_stability(context)) return FALSE;
    main_end = context->ir->instruction_count;
    for (i = 0; i < context->ir->instruction_count; ++i) {
        if (context->ir->instructions[i].opcode == D3DSIO_LABEL) {
            main_end = i;
            break;
        }
    }
    qxp_vs_state_zero(&initial);
    if (!qxp_vs_analyze_range(
            context, 0, main_end, &initial, &result, &missing,
            &first_missing_offset)) return FALSE;
    if (qxp_vs_state_has_bits(&missing))
        return qxp_fail(context, QXP_SHADER_DECODE_UNINITIALIZED,
                        first_missing_offset);
    return TRUE;
}

static BOOL qxp_validate_outputs(QXP_DECODE_CONTEXT *context)
{
    DWORD i;
    BOOL saw_texture_gap;
    DWORD end_offset;

    end_offset = context->token_count - 1;
    if (context->ir->stage == QXP_SHADER_STAGE_PIXEL) {
        if (qxp_stream_is_ps11(context)) {
            /* ps_1_1 has no oC registers; r0 carries the final colour,
             * so all four of its components must be defined. */
            if (context->ps_temp_masks[0] != 15)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                                end_offset);
            return TRUE;
        }
        if (context->ps_color_writes != 1 &&
            context->ps_color_writes != 3 &&
            context->ps_color_writes != 7 &&
            context->ps_color_writes != 15)
            return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                            end_offset);
        return TRUE;
    }

    if (context->vs_position_write_mask != 15)
        return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                        end_offset);

    saw_texture_gap = FALSE;
    for (i = 0; i < 8; ++i) {
        DWORD mask;

        mask = context->vs_texture_write_masks[i];
        if (!mask) {
            saw_texture_gap = TRUE;
        } else {
            if (saw_texture_gap)
                return qxp_fail(context, QXP_SHADER_DECODE_REGISTER_ROLE,
                                end_offset);
            if (mask != 1 && mask != 3 && mask != 7 && mask != 15)
                return qxp_fail(context, QXP_SHADER_DECODE_MODIFIER,
                                end_offset);
        }
    }
    return TRUE;
}

static DWORD qxp_pixel_extended_texture_slots(
    const QXP_DECODE_CONTEXT *context,
    const QXP_SHADER_INSTRUCTION *instruction)
{
    DWORD sampler_type;

    if (instruction->opcode == D3DSIO_TEXKILL) return 2;
    if (instruction->control == QXP_SHADER_CONTROL_TEX_BIAS) return 6;
    /* Only the ps_2_0 texld form names its sampler through sources[1];
     * the SM1 dependent-read forms carry one source or none. */
    if (instruction->source_count < 2)
        return instruction->slot_count;
    sampler_type = context->ps_sampler_types[
        instruction->sources[1].register_number];
    if (instruction->control == QXP_SHADER_CONTROL_TEX_PROJECT)
        return sampler_type == QXP_SHADER_SAMPLER_CUBE ? 4 : 3;
    return sampler_type == QXP_SHADER_SAMPLER_CUBE ? 4 : 1;
}

static BOOL qxp_validate_slot_limits(QXP_DECODE_CONTEXT *context)
{
    DWORD arithmetic;
    DWORD texture;
    DWORD total;
    DWORD required;
    DWORD i;
    BOOL no_texture_instruction_limit;

    arithmetic = 0;
    texture = 0;
    total = 0;
    for (i = 0; i < context->ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &context->ir->instructions[i];
        total += instruction->slot_count;
        if (instruction->slot_class == QXP_SHADER_SLOT_ARITHMETIC)
            arithmetic += instruction->slot_count;
        else if (instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            texture += instruction->slot_count;

        if (context->ir->stage == QXP_SHADER_STAGE_VERTEX && total > 256)
            return qxp_fail(context, QXP_SHADER_DECODE_SLOT_LIMIT,
                            instruction->token_offset);
    }
    if (context->ir->stage != QXP_SHADER_STAGE_PIXEL) return TRUE;

    no_texture_instruction_limit = arithmetic > 64 || texture > 32;
    if (no_texture_instruction_limit)
        context->ir->required_ps20_caps |=
            QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT;
    required = 0;
    for (i = 0; i < context->ir->instruction_count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;

        instruction = &context->ir->instructions[i];
        if (no_texture_instruction_limit &&
            instruction->slot_class == QXP_SHADER_SLOT_TEXTURE)
            required += qxp_pixel_extended_texture_slots(
                context, instruction);
        else
            required += instruction->slot_count;
    }
    context->ir->required_ps_instruction_slots = required;
    return TRUE;
}

static BOOL qxp_is_cfg_terminator(DWORD opcode)
{
    return opcode == D3DSIO_IF || opcode == D3DSIO_ELSE ||
           opcode == D3DSIO_LOOP || opcode == D3DSIO_REP ||
           opcode == D3DSIO_ENDLOOP || opcode == D3DSIO_ENDREP ||
           opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ ||
           opcode == D3DSIO_RET;
}

static void qxp_mark_leader(BYTE leaders[QXP_SHADER_MAX_INSTRUCTIONS],
                            DWORD instruction_count, DWORD index)
{
    if (index < instruction_count) leaders[index] = 1;
}

static BOOL qxp_add_successor(QXP_DECODE_CONTEXT *context,
                              QXP_SHADER_BASIC_BLOCK *block,
                              const DWORD instruction_to_block[
                                  QXP_SHADER_MAX_INSTRUCTIONS],
                              DWORD target_instruction)
{
    DWORD target_block;
    DWORD i;

    if (target_instruction >= context->ir->instruction_count) return TRUE;
    target_block = instruction_to_block[target_instruction];
    if (target_block >= context->ir->block_count)
        return qxp_fail(context, QXP_SHADER_DECODE_CFG_CAPACITY,
                        context->ir->instructions[target_instruction].
                            token_offset);
    for (i = 0; i < block->successor_count; ++i) {
        if (block->successors[i] == target_block) return TRUE;
    }
    if (block->successor_count >= QXP_SHADER_MAX_SUCCESSORS)
        return qxp_fail(context, QXP_SHADER_DECODE_CFG_CAPACITY,
                        context->ir->instructions[block->first_instruction].
                            token_offset);
    block->successors[block->successor_count++] = target_block;
    return TRUE;
}

static BOOL qxp_build_cfg(QXP_DECODE_CONTEXT *context)
{
    BYTE leaders[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD instruction_to_block[QXP_SHADER_MAX_INSTRUCTIONS];
    DWORD count;
    DWORD i;
    DWORD j;

    count = context->ir->instruction_count;
    qxp_zero_memory(leaders, (DWORD)sizeof(leaders));
    qxp_zero_memory(instruction_to_block,
                    (DWORD)sizeof(instruction_to_block));
    if (!count) return TRUE;
    leaders[0] = 1;

    for (i = 0; i < count; ++i) {
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD opcode;

        instruction = &context->ir->instructions[i];
        opcode = instruction->opcode;
        if (opcode == D3DSIO_LABEL) qxp_mark_leader(leaders, count, i);
        if (qxp_is_cfg_terminator(opcode))
            qxp_mark_leader(leaders, count, i + 1);
        if (opcode == D3DSIO_IF) {
            qxp_mark_leader(leaders, count, i + 1);
            if (instruction->control_else != QXP_SHADER_INDEX_NONE)
                qxp_mark_leader(leaders, count,
                                instruction->control_else + 1);
            else
                qxp_mark_leader(leaders, count,
                                instruction->control_match + 1);
        } else if (opcode == D3DSIO_ELSE) {
            qxp_mark_leader(leaders, count,
                            instruction->control_match + 1);
        } else if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            qxp_mark_leader(leaders, count, i + 1);
            qxp_mark_leader(leaders, count,
                            instruction->control_match + 1);
        } else if (opcode == D3DSIO_ENDLOOP ||
                   opcode == D3DSIO_ENDREP) {
            qxp_mark_leader(leaders, count,
                            instruction->control_match + 1);
            qxp_mark_leader(leaders, count, i + 1);
        }
    }

    i = 0;
    while (i < count) {
        QXP_SHADER_BASIC_BLOCK *block;
        DWORD start;
        DWORD block_index;

        if (context->ir->block_count >= QXP_SHADER_MAX_BLOCKS)
            return qxp_fail(context, QXP_SHADER_DECODE_CFG_CAPACITY,
                            context->ir->instructions[i].token_offset);
        block_index = context->ir->block_count++;
        block = &context->ir->blocks[block_index];
        start = i;
        ++i;
        while (i < count && !leaders[i]) ++i;
        block->first_instruction = start;
        block->instruction_count = i - start;
        block->successors[0] = QXP_SHADER_INDEX_NONE;
        block->successors[1] = QXP_SHADER_INDEX_NONE;
        for (j = start; j < i; ++j)
            instruction_to_block[j] = block_index;
    }

    for (i = 0; i < context->ir->block_count; ++i) {
        QXP_SHADER_BASIC_BLOCK *block;
        const QXP_SHADER_INSTRUCTION *instruction;
        DWORD last;
        DWORD opcode;

        block = &context->ir->blocks[i];
        last = block->first_instruction + block->instruction_count - 1;
        instruction = &context->ir->instructions[last];
        opcode = instruction->opcode;
        if (opcode == D3DSIO_IF) {
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   last + 1)) return FALSE;
            if (!qxp_add_successor(
                    context, block, instruction_to_block,
                    instruction->control_else != QXP_SHADER_INDEX_NONE ?
                        instruction->control_else + 1 :
                        instruction->control_match + 1)) return FALSE;
        } else if (opcode == D3DSIO_ELSE) {
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   instruction->control_match + 1))
                return FALSE;
        } else if (opcode == D3DSIO_LOOP || opcode == D3DSIO_REP) {
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   last + 1)) return FALSE;
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   instruction->control_match + 1))
                return FALSE;
        } else if (opcode == D3DSIO_ENDLOOP ||
                   opcode == D3DSIO_ENDREP) {
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   instruction->control_match + 1))
                return FALSE;
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   last + 1)) return FALSE;
        } else if (opcode == D3DSIO_CALL || opcode == D3DSIO_CALLNZ) {
            /* Intraprocedural CFG: label_target separately names the callee. */
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   last + 1)) return FALSE;
        } else if (opcode != D3DSIO_RET) {
            if (!qxp_add_successor(context, block, instruction_to_block,
                                   last + 1)) return FALSE;
        }
    }
    return TRUE;
}

static BOOL qxp_decode_instruction(QXP_DECODE_CONTEXT *context,
                                   DWORD instruction_offset,
                                   DWORD *next_offset)
{
    QXP_OPCODE_INFO info;
    QXP_SHADER_INSTRUCTION *instruction;
    DWORD instruction_token;
    DWORD opcode;
    DWORD control;
    DWORD length;
    DWORD cursor;
    DWORD instruction_end;
    DWORD index;
    DWORD i;

    instruction_token = context->tokens[instruction_offset];
    if (instruction_token & QXP_INSTRUCTION_RESERVED)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_TOKEN,
                        instruction_offset);
    if (instruction_token & QXP_INSTRUCTION_PREDICATED)
        return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                        instruction_offset);
    if (instruction_token & QXP_INSTRUCTION_COISSUE) {
        /* ps_1_1 may co-issue a scalar instruction beside the preceding
         * vector one.  The pair writes disjoint masks, so sequential IR
         * is exact and nothing extra is recorded; a marker on the very
         * first instruction has nothing to pair with. */
        if (!qxp_stream_is_ps11(context) ||
            !context->ir->instruction_count)
            return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                            instruction_offset);
    }

    opcode = instruction_token & D3DSI_OPCODE_MASK;
    if (!qxp_get_opcode(context, opcode, &info))
        return qxp_fail(context, QXP_SHADER_DECODE_UNSUPPORTED_OPCODE,
                        instruction_offset);
    /* Texture-address forms never pair in a co-issue slot: the tracker
     * that orders texreg2* dependent reads is sequential, and a parallel
     * pair could otherwise read a stage its co-issue partner writes. */
    if ((instruction_token & QXP_INSTRUCTION_COISSUE) &&
        (opcode == D3DSIO_TEXREG2AR || opcode == D3DSIO_TEXREG2GB))
        return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                        instruction_offset);
    control = (instruction_token & D3DSP_OPCODESPECIFICCONTROL_MASK) >>
              D3DSP_OPCODESPECIFICCONTROL_SHIFT;
    if (opcode == D3DSIO_TEX && !qxp_stream_is_ps11(context)) {
        if (control != QXP_SHADER_CONTROL_NONE &&
            control != QXP_SHADER_CONTROL_TEX_PROJECT &&
            control != QXP_SHADER_CONTROL_TEX_BIAS)
            return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                            instruction_offset);
    } else if (control != 0) {
        return qxp_fail(context, QXP_SHADER_DECODE_CONTROL,
                        instruction_offset);
    }

    /* SM2 states the operand-token count in the instruction token.  SM1
     * leaves that field zero, so derive the same count from the opcode's
     * own schema: DCL carries a usage token plus its destination, DEF a
     * destination plus its immediates, and every other form exactly its
     * destinations and sources.  SM1 relative addressing adds no token. */
    if (qxp_stream_is_sm1(context)) {
        if (info.form == QXP_FORM_DCL)
            length = 1 + info.destination_count;
        else
            length = info.destination_count + info.source_count +
                     info.immediate_count;
    } else {
        length = (instruction_token & D3DSI_INSTLENGTH_MASK) >>
                 D3DSI_INSTLENGTH_SHIFT;
    }
    if (length > context->token_count - instruction_offset - 1)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                        instruction_offset);
    if (context->ir->instruction_count >= QXP_SHADER_MAX_INSTRUCTIONS)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_CAPACITY,
                        instruction_offset);

    index = context->ir->instruction_count;
    instruction = &context->ir->instructions[index];
    instruction->opcode = opcode;
    instruction->control = control;
    instruction->destination_count = info.destination_count;
    instruction->source_count = info.source_count;
    instruction->immediate_count = info.immediate_count;
    instruction->token_offset = instruction_offset;
    instruction->token_count = length + 1;
    instruction->slot_class = info.slot_class;
    instruction->slot_count = info.slot_count;
    instruction->control_match = QXP_SHADER_INDEX_NONE;
    instruction->control_else = QXP_SHADER_INDEX_NONE;
    instruction->label_target = QXP_SHADER_INDEX_NONE;

    cursor = instruction_offset + 1;
    instruction_end = cursor + length;
    if (info.form == QXP_FORM_DCL) {
        DWORD declaration_token;

        if (cursor >= instruction_end)
            return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                            instruction_offset);
        declaration_token = context->tokens[cursor++];
        if ((declaration_token & QXP_PARAMETER_MARKER) == 0)
            return qxp_fail(context, QXP_SHADER_DECODE_PARAMETER_MARKER,
                            cursor - 1);
        instruction->declaration.raw_token = declaration_token;
        if (cursor >= instruction_end)
            return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                            instruction_offset);
        if (!qxp_parse_destination(
                context, context->tokens[cursor], cursor,
                QXP_SHADER_OPERAND_DECLARATION,
                &instruction->destinations[0])) return FALSE;
        ++cursor;
    } else if (info.form == QXP_FORM_DEF) {
        if (cursor >= instruction_end)
            return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                            instruction_offset);
        if (!qxp_parse_destination(
                context, context->tokens[cursor], cursor,
                QXP_SHADER_OPERAND_DEFINITION,
                &instruction->destinations[0])) return FALSE;
        ++cursor;
        if (info.immediate_count > instruction_end - cursor)
            return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                            instruction_offset);
        for (i = 0; i < info.immediate_count; ++i)
            instruction->immediate[i] = context->tokens[cursor++];
    } else {
        for (i = 0; i < info.destination_count; ++i) {
            if (cursor >= instruction_end)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                                instruction_offset);
            if (!qxp_parse_destination(
                    context, context->tokens[cursor], cursor,
                    QXP_SHADER_OPERAND_DESTINATION,
                    &instruction->destinations[i])) return FALSE;
            ++cursor;
        }
        for (i = 0; i < info.source_count; ++i) {
            if (!qxp_parse_source(
                    context, &cursor, instruction_end,
                    QXP_SHADER_OPERAND_SOURCE,
                    &instruction->sources[i])) return FALSE;
        }
    }

    if (cursor != instruction_end)
        return qxp_fail(context, QXP_SHADER_DECODE_INSTRUCTION_LENGTH,
                        instruction_offset);
    if (!qxp_validate_instruction(context, instruction, &info)) return FALSE;
    if (!qxp_apply_control(context, index)) return FALSE;
    if (!qxp_record_label(context, index)) return FALSE;

    context->ir->slot_count += info.slot_count;
    if (info.slot_class == QXP_SHADER_SLOT_ARITHMETIC)
        context->ir->arithmetic_slot_count += info.slot_count;
    else if (info.slot_class == QXP_SHADER_SLOT_TEXTURE)
        context->ir->texture_slot_count += info.slot_count;
    else if (info.slot_class == QXP_SHADER_SLOT_FLOW)
        context->ir->flow_slot_count += info.slot_count;
    ++context->ir->instruction_count;
    *next_offset = instruction_end;
    return TRUE;
}

static BOOL qxp_decode_stream(QXP_DECODE_CONTEXT *context)
{
    DWORD offset;
    DWORD i;
    BOOL found_end;

    /* ps_1_1 t0-t3 arrive holding their stage's interpolated coordinate,
     * so every component is defined before the first instruction runs. */
    if (qxp_stream_is_ps11(context)) {
        for (i = 0; i < 4; ++i)
            context->ps_texture_masks[i] = 15;
    }

    offset = 1;
    found_end = FALSE;
    while (offset < context->token_count) {
        DWORD token;
        DWORD opcode;

        token = context->tokens[offset];
        opcode = token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) {
            if (token != D3DSIO_END)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_INSTRUCTION_TOKEN,
                                offset);
            if (offset + 1 != context->token_count)
                return qxp_fail(context,
                                QXP_SHADER_DECODE_TRAILING_TOKENS,
                                offset + 1);
            found_end = TRUE;
            break;
        }
        if (opcode == D3DSIO_COMMENT) {
            DWORD comment_size;

            if (token & QXP_PARAMETER_MARKER)
                return qxp_fail(context, QXP_SHADER_DECODE_COMMENT,
                                offset);
            comment_size = (token & D3DSI_COMMENTSIZE_MASK) >>
                           D3DSI_COMMENTSIZE_SHIFT;
            if (comment_size > context->token_count - offset - 1)
                return qxp_fail(context, QXP_SHADER_DECODE_COMMENT,
                                offset);
            ++context->ir->comment_count;
            offset += comment_size + 1;
        } else {
            if (!qxp_decode_instruction(context, offset, &offset))
                return FALSE;
        }
    }

    if (!found_end)
        return qxp_fail(context, QXP_SHADER_DECODE_MISSING_END,
                        context->token_count);
    if (context->control_depth) {
        DWORD opener;

        opener = context->control_stack[context->control_depth - 1];
        return qxp_fail(context, QXP_SHADER_DECODE_UNMATCHED_CONTROL,
                        context->ir->instructions[opener].token_offset);
    }
    if (!qxp_validate_label_layout(context)) return FALSE;
    if (!qxp_validate_call_iteration_nesting(context)) return FALSE;
    if (!qxp_validate_loop_relative_scope(context)) return FALSE;
    if (!qxp_validate_declared_inputs(context)) return FALSE;
    if (!qxp_validate_vertex_definite_assignment(context)) return FALSE;
    if (!qxp_validate_outputs(context)) return FALSE;
    if (!qxp_validate_slot_limits(context)) return FALSE;
    if (!qxp_build_cfg(context)) return FALSE;
    return TRUE;
}

BOOL QxpDecodeShader20(const DWORD *tokens, DWORD token_count,
                       QXP_SHADER_IR *ir,
                       QXP_SHADER_DECODE_ERROR *error)
{
    QXP_DECODE_CONTEXT context;
    DWORD i;

    if (error) qxp_zero_memory(error, (DWORD)sizeof(*error));
    if (!ir) {
        if (error) {
            error->code = QXP_SHADER_DECODE_NULL_ARGUMENT;
            error->token_offset = 0;
        }
        return FALSE;
    }
    qxp_zero_memory(ir, (DWORD)sizeof(*ir));
    if (!error) return FALSE;
    if (!tokens) {
        error->code = QXP_SHADER_DECODE_NULL_ARGUMENT;
        error->token_offset = 0;
        return FALSE;
    }
    if (!token_count || token_count > QXP_SHADER_MAX_TOKENS) {
        error->code = QXP_SHADER_DECODE_TOKEN_COUNT;
        error->token_offset = token_count > QXP_SHADER_MAX_TOKENS ?
                              QXP_SHADER_MAX_TOKENS : 0;
        return FALSE;
    }
    if (!QXP_SHADER_VERSION_IS_PIXEL(tokens[0]) &&
        !QXP_SHADER_VERSION_IS_VERTEX(tokens[0])) {
        error->code = QXP_SHADER_DECODE_VERSION;
        error->token_offset = 0;
        return FALSE;
    }

    qxp_zero_memory(&context, (DWORD)sizeof(context));
    context.tokens = tokens;
    context.token_count = token_count;
    context.ir = ir;
    context.error = error;
    for (i = 0; i < QXP_SHADER_MAX_LABELS; ++i)
        context.labels[i] = QXP_SHADER_INDEX_NONE;

    ir->stage = QXP_SHADER_VERSION_IS_PIXEL(tokens[0]) ?
                QXP_SHADER_STAGE_PIXEL : QXP_SHADER_STAGE_VERTEX;
    ir->version_token = tokens[0];
    ir->token_count = token_count;
    if (!qxp_decode_stream(&context)) {
        qxp_zero_memory(ir, (DWORD)sizeof(*ir));
        return FALSE;
    }

    ir->valid = TRUE;
    return TRUE;
}
