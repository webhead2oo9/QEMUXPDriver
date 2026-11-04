#ifndef QXP_SHADER_IR_H
#define QXP_SHADER_IR_H

#include <windef.h>

/*
 * Private, guest-only Shader Model 2 decoder representation.  This is not a
 * transport structure.  The caller owns the deliberately large QXP_SHADER_IR
 * storage; driver callers should not place it on a small stack.
 */
#define QXP_SHADER_MAX_TOKENS        16384UL
#define QXP_SHADER_MAX_INSTRUCTIONS  512UL
#define QXP_SHADER_MAX_BLOCKS        512UL
#define QXP_SHADER_MAX_SOURCES       3UL
#define QXP_SHADER_MAX_DESTINATIONS  1UL
#define QXP_SHADER_MAX_SUCCESSORS    2UL
#define QXP_SHADER_MAX_LABELS        16UL
#define QXP_SHADER_MAX_PS_TEMPS      32UL
#define QXP_SHADER_INDEX_NONE        ((DWORD)0xffffffffUL)

/* Private values matching the optional D3DPS20CAPS capability bits. */
#define QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE       0x00000001UL
#define QXP_SHADER_PS20CAPS_GRADIENTINSTRUCTIONS   0x00000002UL
#define QXP_SHADER_PS20CAPS_PREDICATION            0x00000004UL
#define QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT   0x00000008UL
#define QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT  0x00000010UL

#define QXP_SHADER_VERSION_VS_1_1    0xfffe0101UL
#define QXP_SHADER_VERSION_PS_1_1    0xffff0101UL
#define QXP_SHADER_VERSION_PS_2_0    0xffff0200UL
#define QXP_SHADER_VERSION_VS_2_0    0xfffe0200UL

/*
 * vs_1_1 and vs_2_0 share every register file, output semantic, swizzle rule,
 * and write mask this driver consumes.  They differ only in token encoding --
 * SM1 omits the instruction length field, addresses constants through an
 * implicit a0.x, and writes that address register with MOV instead of MOVA --
 * and QxpDecodeShader20 normalizes all three into one IR shape.  Every
 * consumer downstream of the decoder therefore accepts both profiles.
 */
#define QXP_SHADER_VERSION_IS_VERTEX(version) \
    ((version) == QXP_SHADER_VERSION_VS_2_0 || \
     (version) == QXP_SHADER_VERSION_VS_1_1)

/*
 * ps_1_1 differs from ps_2_0 more than the vertex profiles differ from each
 * other: its t# registers are writable sample results rather than read-only
 * texcoord inputs, `tex tN` names its sampler implicitly by register number
 * instead of declaring it, the final colour lands in r0 rather than oC0, and
 * it may co-issue a scalar beside a vector.  The decoder normalizes the
 * encoding and records those forms; the emitter lowers them.
 */
#define QXP_SHADER_VERSION_IS_PIXEL(version) \
    ((version) == QXP_SHADER_VERSION_PS_2_0 || \
     (version) == QXP_SHADER_VERSION_PS_1_1)

typedef enum qxp_shader_stage {
    QXP_SHADER_STAGE_INVALID = 0,
    QXP_SHADER_STAGE_VERTEX = 1,
    QXP_SHADER_STAGE_PIXEL = 2
} QXP_SHADER_STAGE;

/* Values deliberately match the five-bit D3D9 register-file encoding. */
typedef enum qxp_shader_register_file {
    QXP_SHADER_REG_TEMP = 0,
    QXP_SHADER_REG_INPUT = 1,
    QXP_SHADER_REG_CONST = 2,
    QXP_SHADER_REG_ADDRESS = 3,
    QXP_SHADER_REG_TEXTURE = 3,
    QXP_SHADER_REG_RASTOUT = 4,
    QXP_SHADER_REG_ATTROUT = 5,
    QXP_SHADER_REG_TEXCRDOUT = 6,
    QXP_SHADER_REG_CONSTINT = 7,
    QXP_SHADER_REG_COLOROUT = 8,
    QXP_SHADER_REG_DEPTHOUT = 9,
    QXP_SHADER_REG_SAMPLER = 10,
    QXP_SHADER_REG_CONST2 = 11,
    QXP_SHADER_REG_CONST3 = 12,
    QXP_SHADER_REG_CONST4 = 13,
    QXP_SHADER_REG_CONSTBOOL = 14,
    QXP_SHADER_REG_LOOP = 15,
    QXP_SHADER_REG_TEMPFLOAT16 = 16,
    QXP_SHADER_REG_MISCTYPE = 17,
    QXP_SHADER_REG_LABEL = 18,
    QXP_SHADER_REG_PREDICATE = 19
} QXP_SHADER_REGISTER_FILE;

typedef enum qxp_shader_operand_role {
    QXP_SHADER_OPERAND_NONE = 0,
    QXP_SHADER_OPERAND_DESTINATION = 1,
    QXP_SHADER_OPERAND_SOURCE = 2,
    QXP_SHADER_OPERAND_DECLARATION = 3,
    QXP_SHADER_OPERAND_DEFINITION = 4,
    QXP_SHADER_OPERAND_RELATIVE_ADDRESS = 5,
    QXP_SHADER_OPERAND_LABEL = 6,
    QXP_SHADER_OPERAND_CONDITION = 7,
    QXP_SHADER_OPERAND_LOOP_CONTROL = 8
} QXP_SHADER_OPERAND_ROLE;

typedef enum qxp_shader_source_modifier {
    QXP_SHADER_SRCMOD_NONE = 0,
    QXP_SHADER_SRCMOD_NEGATE = 1,
    /* ps_1_1 complement (1 - x); used by NFSU2 dependent-read
     * shaders (r543 capture). */
    QXP_SHADER_SRCMOD_COMP = 6,
    QXP_SHADER_SRCMOD_NOT = 13
} QXP_SHADER_SOURCE_MODIFIER;

typedef enum qxp_shader_destination_modifier {
    QXP_SHADER_DSTMOD_NONE = 0,
    QXP_SHADER_DSTMOD_SATURATE = 1,
    QXP_SHADER_DSTMOD_PARTIAL_PRECISION = 2,
    QXP_SHADER_DSTMOD_CENTROID = 4
} QXP_SHADER_DESTINATION_MODIFIER;

typedef enum qxp_shader_opcode {
    QXP_SHADER_OP_NOP = 0,
    QXP_SHADER_OP_MOV = 1,
    QXP_SHADER_OP_ADD = 2,
    QXP_SHADER_OP_SUB = 3,
    QXP_SHADER_OP_MAD = 4,
    QXP_SHADER_OP_MUL = 5,
    QXP_SHADER_OP_RCP = 6,
    QXP_SHADER_OP_RSQ = 7,
    QXP_SHADER_OP_DP3 = 8,
    QXP_SHADER_OP_DP4 = 9,
    QXP_SHADER_OP_MIN = 10,
    QXP_SHADER_OP_MAX = 11,
    QXP_SHADER_OP_SLT = 12,
    QXP_SHADER_OP_SGE = 13,
    QXP_SHADER_OP_EXP = 14,
    QXP_SHADER_OP_LOG = 15,
    QXP_SHADER_OP_LIT = 16,
    QXP_SHADER_OP_DST = 17,
    QXP_SHADER_OP_LRP = 18,
    QXP_SHADER_OP_FRC = 19,
    QXP_SHADER_OP_M4X4 = 20,
    QXP_SHADER_OP_M4X3 = 21,
    QXP_SHADER_OP_M3X4 = 22,
    QXP_SHADER_OP_M3X3 = 23,
    QXP_SHADER_OP_M3X2 = 24,
    QXP_SHADER_OP_CALL = 25,
    QXP_SHADER_OP_CALLNZ = 26,
    QXP_SHADER_OP_LOOP = 27,
    QXP_SHADER_OP_RET = 28,
    QXP_SHADER_OP_ENDLOOP = 29,
    QXP_SHADER_OP_LABEL = 30,
    QXP_SHADER_OP_DCL = 31,
    QXP_SHADER_OP_POW = 32,
    QXP_SHADER_OP_CRS = 33,
    QXP_SHADER_OP_SGN = 34,
    QXP_SHADER_OP_ABS = 35,
    QXP_SHADER_OP_NRM = 36,
    QXP_SHADER_OP_SINCOS = 37,
    QXP_SHADER_OP_REP = 38,
    QXP_SHADER_OP_ENDREP = 39,
    QXP_SHADER_OP_IF = 40,
    QXP_SHADER_OP_ELSE = 42,
    QXP_SHADER_OP_ENDIF = 43,
    QXP_SHADER_OP_MOVA = 46,
    QXP_SHADER_OP_DEFB = 47,
    QXP_SHADER_OP_DEFI = 48,
    QXP_SHADER_OP_TEXCOORD = 64,
    QXP_SHADER_OP_TEXKILL = 65,
    QXP_SHADER_OP_TEX = 66,
    QXP_SHADER_OP_TEXREG2AR = 69,
    QXP_SHADER_OP_TEXREG2GB = 70,
    QXP_SHADER_OP_EXPP = 78,
    QXP_SHADER_OP_LOGP = 79,
    QXP_SHADER_OP_DEF = 81,
    QXP_SHADER_OP_CMP = 88,
    QXP_SHADER_OP_DP2ADD = 90
} QXP_SHADER_OPCODE;

typedef enum qxp_shader_instruction_control {
    QXP_SHADER_CONTROL_NONE = 0,
    QXP_SHADER_CONTROL_TEX_PROJECT = 1,
    QXP_SHADER_CONTROL_TEX_BIAS = 2
} QXP_SHADER_INSTRUCTION_CONTROL;

typedef enum qxp_shader_slot_class {
    QXP_SHADER_SLOT_SETUP = 0,
    QXP_SHADER_SLOT_ARITHMETIC = 1,
    QXP_SHADER_SLOT_TEXTURE = 2,
    QXP_SHADER_SLOT_FLOW = 3
} QXP_SHADER_SLOT_CLASS;

typedef enum qxp_shader_declaration_kind {
    QXP_SHADER_DECLARATION_NONE = 0,
    QXP_SHADER_DECLARATION_INPUT = 1,
    QXP_SHADER_DECLARATION_SAMPLER = 2
} QXP_SHADER_DECLARATION_KIND;

typedef enum qxp_shader_sampler_type {
    QXP_SHADER_SAMPLER_NONE = 0,
    QXP_SHADER_SAMPLER_2D = 2,
    QXP_SHADER_SAMPLER_CUBE = 3,
    QXP_SHADER_SAMPLER_VOLUME = 4
} QXP_SHADER_SAMPLER_TYPE;

typedef struct qxp_shader_relative_operand {
    BOOL present;
    BYTE role;
    BYTE register_file;
    BYTE source_modifier;
    BYTE reserved;
    WORD register_number;
    BYTE swizzle[4];
} QXP_SHADER_RELATIVE_OPERAND;

typedef struct qxp_shader_operand {
    BYTE role;
    BYTE register_file;
    BYTE write_mask;
    BYTE source_modifier;
    BYTE destination_modifier;
    BYTE destination_shift;
    WORD register_number;
    BYTE swizzle[4];
    QXP_SHADER_RELATIVE_OPERAND relative;
} QXP_SHADER_OPERAND;

typedef struct qxp_shader_declaration {
    BOOL present;
    DWORD kind;
    DWORD usage;
    DWORD usage_index;
    DWORD sampler_type;
    DWORD raw_token;
} QXP_SHADER_DECLARATION;

typedef struct qxp_shader_instruction {
    DWORD opcode;
    DWORD control;
    DWORD destination_count;
    DWORD source_count;
    DWORD immediate_count;
    DWORD token_offset;
    DWORD token_count;
    DWORD slot_class;
    DWORD slot_count;
    QXP_SHADER_OPERAND destinations[QXP_SHADER_MAX_DESTINATIONS];
    QXP_SHADER_OPERAND sources[QXP_SHADER_MAX_SOURCES];
    DWORD immediate[4];
    QXP_SHADER_DECLARATION declaration;

    /* Reciprocal opener/closer pair; QXP_SHADER_INDEX_NONE if not paired. */
    DWORD control_match;
    /* IF <-> ELSE association; QXP_SHADER_INDEX_NONE when there is no ELSE. */
    DWORD control_else;
    /* CALL/CALLNZ resolves to its LABEL instruction; LABEL points to itself. */
    DWORD label_target;
} QXP_SHADER_INSTRUCTION;

/*
 * Bounded intraprocedural CFG.  CALL/CALLNZ have only their ordinary
 * continuation successor here; QXP_SHADER_INSTRUCTION.label_target records
 * the separate callee relation.  RET terminates its subroutine graph.
 * Interprocedural effects are represented by decoder-internal summaries, not
 * by pretending that a call has both callee and continuation CFG edges.
 */
typedef struct qxp_shader_basic_block {
    DWORD first_instruction;
    DWORD instruction_count;
    DWORD successor_count;
    DWORD successors[QXP_SHADER_MAX_SUCCESSORS];
} QXP_SHADER_BASIC_BLOCK;

typedef struct qxp_shader_ir {
    BOOL valid;
    DWORD stage;
    DWORD version_token;
    DWORD token_count;
    DWORD instruction_count;

    /*
     * Requirements for a future pixel-capability advertisement check.
     * Decoding records, but deliberately does not enforce, these values.
     * required_ps_instruction_slots is the minimum NumInstructionSlots after
     * applying the texture costs selected by required_ps20_caps.  All three
     * fields are zero for vertex shaders.
     */
    DWORD required_ps_temp_count;
    DWORD required_ps20_caps;
    DWORD required_ps_instruction_slots;
    DWORD declaration_count;
    DWORD definition_count;
    DWORD comment_count;
    DWORD slot_count;
    DWORD arithmetic_slot_count;
    DWORD texture_slot_count;
    DWORD flow_slot_count;
    DWORD block_count;
    QXP_SHADER_INSTRUCTION instructions[QXP_SHADER_MAX_INSTRUCTIONS];
    QXP_SHADER_BASIC_BLOCK blocks[QXP_SHADER_MAX_BLOCKS];
} QXP_SHADER_IR;

typedef enum qxp_shader_decode_error_code {
    QXP_SHADER_DECODE_OK = 0,
    QXP_SHADER_DECODE_NULL_ARGUMENT = 1,
    QXP_SHADER_DECODE_TOKEN_COUNT = 2,
    QXP_SHADER_DECODE_VERSION = 3,
    QXP_SHADER_DECODE_MISSING_END = 4,
    QXP_SHADER_DECODE_TRAILING_TOKENS = 5,
    QXP_SHADER_DECODE_COMMENT = 6,
    QXP_SHADER_DECODE_INSTRUCTION_TOKEN = 7,
    QXP_SHADER_DECODE_UNSUPPORTED_OPCODE = 8,
    QXP_SHADER_DECODE_INSTRUCTION_LENGTH = 9,
    QXP_SHADER_DECODE_PARAMETER_MARKER = 10,
    QXP_SHADER_DECODE_OPERAND_BITS = 11,
    QXP_SHADER_DECODE_REGISTER_ROLE = 12,
    QXP_SHADER_DECODE_REGISTER_RANGE = 13,
    QXP_SHADER_DECODE_MODIFIER = 14,
    QXP_SHADER_DECODE_CONTROL = 15,
    QXP_SHADER_DECODE_DECLARATION = 16,
    QXP_SHADER_DECODE_DEFINITION = 17,
    QXP_SHADER_DECODE_RELATIVE_ADDRESS = 18,
    /* Fixed-storage resource exhaustion, not shader-profile illegality. */
    QXP_SHADER_DECODE_INSTRUCTION_CAPACITY = 19,
    QXP_SHADER_DECODE_SLOT_LIMIT = 20,
    QXP_SHADER_DECODE_UNMATCHED_CONTROL = 21,
    QXP_SHADER_DECODE_DUPLICATE_LABEL = 22,
    QXP_SHADER_DECODE_MISSING_LABEL = 23,
    QXP_SHADER_DECODE_CALL_TARGET = 24,
    QXP_SHADER_DECODE_CFG_CAPACITY = 25,
    QXP_SHADER_DECODE_READ_PORT = 26,
    QXP_SHADER_DECODE_UNINITIALIZED = 27,
    /* Reserved: dependency depth now records a required PS20 cap. */
    QXP_SHADER_DECODE_DEPENDENCY_LIMIT = 28
} QXP_SHADER_DECODE_ERROR_CODE;

typedef struct qxp_shader_decode_error {
    DWORD code;
    DWORD token_offset;
} QXP_SHADER_DECODE_ERROR;

/*
 * Decodes exact ps_2_0, vs_2_0, and vs_1_1 streams.  No input pointer is
 * retained.
 * On every failure, a non-NULL IR is zeroed and the error is deterministic.
 * QXP_SHADER_DECODE_TOKEN_COUNT and ..._INSTRUCTION_CAPACITY report the
 * decoder's fixed-storage resource bounds; they do not classify an otherwise
 * well-formed shader as unsupported or illegal.  No allocation is performed.
 */
BOOL QxpDecodeShader20(const DWORD *tokens, DWORD token_count,
                       QXP_SHADER_IR *ir,
                       QXP_SHADER_DECODE_ERROR *error);

#endif
