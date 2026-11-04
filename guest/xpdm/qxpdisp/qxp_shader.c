#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_shader.h"

/* Some MinGW d3d8 headers omit these two legacy token helpers.  Keep the
 * translator buildable by the fast host-side contract test as well as WDK. */
#ifndef D3DSP_REGNUM_MASK
#define D3DSP_REGNUM_MASK 0x00001fffUL
#endif
#ifndef D3DSP_REPLICATEALPHA
#define D3DSP_REPLICATEALPHA \
    ((3UL << (D3DSP_SWIZZLE_SHIFT + 0)) | \
     (3UL << (D3DSP_SWIZZLE_SHIFT + 2)) | \
     (3UL << (D3DSP_SWIZZLE_SHIFT + 4)) | \
     (3UL << (D3DSP_SWIZZLE_SHIFT + 6)))
#endif

typedef struct qxp_shader_text {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_SHADER_TEXT;

static void qxp_shader_append_char(QXP_SHADER_TEXT *output, char value)
{
    if (!output || !output->valid || output->length + 1 >= output->capacity) {
        if (output) output->valid = FALSE;
        return;
    }
    output->text[output->length++] = value;
    output->text[output->length] = '\0';
}

static void qxp_shader_append(QXP_SHADER_TEXT *output, const char *value)
{
    if (!output || !value) return;
    while (*value) qxp_shader_append_char(output, *value++);
}

static void qxp_shader_append_uint(QXP_SHADER_TEXT *output, DWORD value)
{
    char digits[11];
    DWORD count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < (DWORD)sizeof(digits));
    while (count) qxp_shader_append_char(output, digits[--count]);
}

static BOOL qxp_shader_register(QXP_SHADER_TEXT *output, DWORD token,
                                BOOL destination)
{
    DWORD type = token & D3DSP_REGTYPE_MASK;
    DWORD number = token & D3DSP_REGNUM_MASK;

    if (type == D3DSPR_TEMP && number < 6) {
        qxp_shader_append_char(output, 'r');
    } else if (type == D3DSPR_TEXTURE && number < 4) {
        qxp_shader_append_char(output, 't');
    } else if (!destination && type == D3DSPR_INPUT && number < 2) {
        qxp_shader_append_char(output, 'v');
    } else if (!destination && type == D3DSPR_CONST && number < 8) {
        qxp_shader_append_char(output, 'c');
    } else {
        return FALSE;
    }
    qxp_shader_append_uint(output, number);
    return output->valid;
}

static BOOL qxp_shader_texture_destination(DWORD token, DWORD *stage)
{
    DWORD number = token & D3DSP_REGNUM_MASK;

    if (!stage || (token & D3DSP_REGTYPE_MASK) != D3DSPR_TEXTURE ||
        number >= 4 ||
        (token & D3DSP_WRITEMASK_ALL) != D3DSP_WRITEMASK_ALL ||
        (token & (D3DSP_DSTMOD_MASK | D3DSP_DSTSHIFT_MASK |
                  D3DVS_ADDRESSMODE_MASK))) return FALSE;
    *stage = number;
    return TRUE;
}

static BOOL qxp_shader_texture_source(DWORD token, DWORD *stage,
                                      BOOL allow_signed)
{
    DWORD number = token & D3DSP_REGNUM_MASK;
    DWORD modifier = token & D3DSP_SRCMOD_MASK;

    if (!stage || number >= 4 ||
        (token & (D3DSP_REGTYPE_MASK | D3DSP_SWIZZLE_MASK |
                  D3DVS_ADDRESSMODE_MASK)) !=
            (D3DSPR_TEXTURE | D3DSP_NOSWIZZLE) ||
        (modifier != D3DSPSM_NONE &&
         (!allow_signed || modifier != D3DSPSM_SIGN))) return FALSE;
    *stage = number;
    return TRUE;
}

static BOOL qxp_shader_prepare_texture_source(QXP_SHADER_TEXT *output,
                                               DWORD token)
{
    DWORD modifier = token & D3DSP_SRCMOD_MASK;

    if (modifier == D3DSPSM_NONE) return TRUE;
    if (modifier != D3DSPSM_SIGN) return FALSE;
    qxp_shader_append(output, "MAD qd, t");
    qxp_shader_append_uint(output, token & D3DSP_REGNUM_MASK);
    qxp_shader_append(output,
        ", {2.0, 2.0, 2.0, 2.0}, {-1.0, -1.0, -1.0, -1.0};\n");
    return output->valid;
}

static BOOL qxp_shader_texture_operand(QXP_SHADER_TEXT *output, DWORD token)
{
    if ((token & D3DSP_SRCMOD_MASK) == D3DSPSM_SIGN) {
        qxp_shader_append(output, "qd");
    } else {
        qxp_shader_append_char(output, 't');
        qxp_shader_append_uint(output, token & D3DSP_REGNUM_MASK);
    }
    return output->valid;
}

static BOOL qxp_shader_write_mask(QXP_SHADER_TEXT *output, DWORD token)
{
    DWORD mask = token & D3DSP_WRITEMASK_ALL;

    if (!mask) mask = D3DSP_WRITEMASK_ALL;
    if (mask == D3DSP_WRITEMASK_ALL) return TRUE;
    qxp_shader_append_char(output, '.');
    if (mask & D3DSP_WRITEMASK_0) qxp_shader_append_char(output, 'x');
    if (mask & D3DSP_WRITEMASK_1) qxp_shader_append_char(output, 'y');
    if (mask & D3DSP_WRITEMASK_2) qxp_shader_append_char(output, 'z');
    if (mask & D3DSP_WRITEMASK_3) qxp_shader_append_char(output, 'w');
    return output->valid;
}

static BOOL qxp_shader_swizzle(QXP_SHADER_TEXT *output, DWORD token)
{
    static const char components[4] = { 'x', 'y', 'z', 'w' };
    DWORD swizzle = (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
    DWORD component;

    if ((token & D3DSP_SWIZZLE_MASK) == D3DSP_NOSWIZZLE) return TRUE;
    qxp_shader_append_char(output, '.');
    for (component = 0; component < 4; ++component) {
        qxp_shader_append_char(
            output, components[(swizzle >> (component * 2)) & 3]);
    }
    return output->valid;
}

static BOOL qxp_shader_base_source(QXP_SHADER_TEXT *output, DWORD token,
                                   BOOL negate)
{
    if (token & D3DVS_ADDRESSMODE_MASK) return FALSE;
    if (negate) qxp_shader_append_char(output, '-');
    return qxp_shader_register(output, token, FALSE) &&
           qxp_shader_swizzle(output, token);
}

static BOOL qxp_shader_prepare_source(QXP_SHADER_TEXT *output, DWORD token,
                                      DWORD scratch)
{
    DWORD modifier = token & D3DSP_SRCMOD_MASK;

    if (modifier == D3DSPSM_NONE || modifier == D3DSPSM_NEG) return TRUE;
    if (scratch > 2) return FALSE;
    if (modifier == D3DSPSM_BIAS) {
        qxp_shader_append(output, "ADD q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, ", {-0.5, -0.5, -0.5, -0.5};\n");
    } else if (modifier == D3DSPSM_BIASNEG) {
        qxp_shader_append(output, "SUB q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", {0.5, 0.5, 0.5, 0.5}, ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, ";\n");
    } else if (modifier == D3DSPSM_SIGN ||
               modifier == D3DSPSM_SIGNNEG) {
        qxp_shader_append(output, "MAD q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, modifier == D3DSPSM_SIGN ?
            ", {2.0, 2.0, 2.0, 2.0}, {-1.0, -1.0, -1.0, -1.0};\n" :
            ", {-2.0, -2.0, -2.0, -2.0}, {1.0, 1.0, 1.0, 1.0};\n");
    } else if (modifier == D3DSPSM_COMP) {
        qxp_shader_append(output, "SUB q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", {1.0, 1.0, 1.0, 1.0}, ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, ";\n");
    } else if (modifier == D3DSPSM_X2 || modifier == D3DSPSM_X2NEG) {
        qxp_shader_append(output, "MUL q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, modifier == D3DSPSM_X2 ?
            ", {2.0, 2.0, 2.0, 2.0};\n" :
            ", {-2.0, -2.0, -2.0, -2.0};\n");
    } else if (modifier == D3DSPSM_DZ || modifier == D3DSPSM_DW) {
        qxp_shader_append(output, "MOV q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", ");
        if (!qxp_shader_base_source(output, token, FALSE)) return FALSE;
        qxp_shader_append(output, ";\nRCP qd.x, q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output,
            modifier == D3DSPSM_DZ ? ".z;\nMUL q" : ".w;\nMUL q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", q");
        qxp_shader_append_uint(output, scratch);
        qxp_shader_append(output, ", qd.x;\n");
    } else {
        return FALSE;
    }
    return output->valid;
}

static BOOL qxp_shader_source(QXP_SHADER_TEXT *output, DWORD token,
                              DWORD scratch)
{
    DWORD modifier = token & D3DSP_SRCMOD_MASK;

    if (modifier == D3DSPSM_NONE || modifier == D3DSPSM_NEG) {
        return qxp_shader_base_source(output, token,
                                      modifier == D3DSPSM_NEG);
    }
    qxp_shader_append_char(output, 'q');
    qxp_shader_append_uint(output, scratch);
    return output->valid;
}

static BOOL qxp_shader_scale(DWORD token, const char **scale)
{
    if (!scale) return FALSE;
    switch ((token & D3DSP_DSTSHIFT_MASK) >> D3DSP_DSTSHIFT_SHIFT) {
    case 0: *scale = NULL; break;
    case 1: *scale = "{2.0, 2.0, 2.0, 2.0}"; break;
    case 2: *scale = "{4.0, 4.0, 4.0, 4.0}"; break;
    case 3: *scale = "{8.0, 8.0, 8.0, 8.0}"; break;
    case 13: *scale = "{0.125, 0.125, 0.125, 0.125}"; break;
    case 14: *scale = "{0.25, 0.25, 0.25, 0.25}"; break;
    case 15: *scale = "{0.5, 0.5, 0.5, 0.5}"; break;
    default: return FALSE;
    }
    return TRUE;
}

static BOOL qxp_shader_destination(QXP_SHADER_TEXT *output,
                                   DWORD destination,
                                   const char *destination_override)
{
    if (destination_override) {
        qxp_shader_append(output, destination_override);
    } else if (!qxp_shader_register(output, destination, TRUE)) {
        return FALSE;
    }
    return qxp_shader_write_mask(output, destination);
}

static BOOL qxp_shader_instruction(
    QXP_SHADER_TEXT *output, const char *operation, DWORD destination,
    const DWORD *sources, DWORD source_count,
    const char *destination_override)
{
    const char *scale;
    DWORD source;
    BOOL saturate;

    if (!output || !operation || !sources || source_count > 3 ||
        !source_count || (destination & D3DSP_DSTMOD_MASK &
                          ~D3DSPDM_SATURATE)) return FALSE;
    if (!qxp_shader_scale(destination, &scale)) return FALSE;
    for (source = 0; source < source_count; ++source) {
        if (!qxp_shader_prepare_source(output, sources[source], source))
            return FALSE;
    }
    saturate = (destination & D3DSP_DSTMOD_MASK) == D3DSPDM_SATURATE;
    qxp_shader_append(output, operation);
    if (!scale && saturate) qxp_shader_append(output, "_SAT");
    qxp_shader_append_char(output, ' ');
    if (scale) {
        qxp_shader_append(output, "qd");
    } else if (!qxp_shader_destination(
                   output, destination, destination_override)) {
        return FALSE;
    }
    for (source = 0; source < source_count; ++source) {
        qxp_shader_append(output, ", ");
        if (!qxp_shader_source(output, sources[source], source)) return FALSE;
    }
    qxp_shader_append(output, ";\n");
    if (scale) {
        qxp_shader_append(output, saturate ? "MUL_SAT " : "MUL ");
        if (!qxp_shader_destination(
                output, destination, destination_override)) return FALSE;
        qxp_shader_append(output, ", qd, ");
        qxp_shader_append(output, scale);
        qxp_shader_append(output, ";\n");
    }
    return output->valid;
}

static BOOL qxp_shader_store_result(QXP_SHADER_TEXT *output,
                                    DWORD destination,
                                    const char *value,
                                    const char *destination_override)
{
    const char *scale;
    BOOL saturate;

    if (!output || !value ||
        (destination & D3DSP_DSTMOD_MASK & ~D3DSPDM_SATURATE) ||
        !qxp_shader_scale(destination, &scale)) return FALSE;
    saturate = (destination & D3DSP_DSTMOD_MASK) == D3DSPDM_SATURATE;
    qxp_shader_append(output, scale ? "MUL" : "MOV");
    if (saturate) qxp_shader_append(output, "_SAT");
    qxp_shader_append_char(output, ' ');
    if (!qxp_shader_destination(
            output, destination, destination_override)) return FALSE;
    qxp_shader_append(output, ", ");
    qxp_shader_append(output, value);
    if (scale) {
        qxp_shader_append(output, ", ");
        qxp_shader_append(output, scale);
    }
    qxp_shader_append(output, ";\n");
    return output->valid;
}

static DWORD qxp_shader_operand_count(DWORD opcode)
{
    switch (opcode) {
    case D3DSIO_MOV: return 1;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_DP3:
    case D3DSIO_DP4: return 2;
    case D3DSIO_MAD:
    case D3DSIO_LRP:
    case D3DSIO_CND:
    case D3DSIO_CMP: return 3;
    }
    return 0;
}

static DWORD qxp_shader_arithmetic_slots(DWORD opcode)
{
    return opcode == D3DSIO_DP4 || opcode == D3DSIO_CMP ? 2 : 1;
}

static DWORD qxp_shader_destination_mask(DWORD destination)
{
    DWORD mask = destination & D3DSP_WRITEMASK_ALL;

    return mask ? mask : D3DSP_WRITEMASK_ALL;
}

static BOOL qxp_shader_pair_masks(DWORD first_destination,
                                  DWORD second_destination)
{
    DWORD alpha_mask = D3DSP_WRITEMASK_3;
    DWORD rgb_mask = D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                     D3DSP_WRITEMASK_2;
    DWORD first_mask = qxp_shader_destination_mask(first_destination);
    DWORD second_mask = qxp_shader_destination_mask(second_destination);

    return (first_mask == rgb_mask && second_mask == alpha_mask) ||
           (first_mask == alpha_mask && second_mask == rgb_mask);
}

static BOOL qxp_shader_emit_arithmetic(
    QXP_SHADER_TEXT *output, DWORD opcode, DWORD destination,
    const DWORD *sources, DWORD source_count,
    const char *destination_override)
{
    const char *operation;
    DWORD destination_id;
    DWORD index;

    if (opcode == D3DSIO_MOV) operation = "MOV";
    else if (opcode == D3DSIO_ADD) operation = "ADD";
    else if (opcode == D3DSIO_SUB) operation = "SUB";
    else if (opcode == D3DSIO_MAD) operation = "MAD";
    else if (opcode == D3DSIO_MUL) operation = "MUL";
    else if (opcode == D3DSIO_DP3) operation = "DP3";
    else if (opcode == D3DSIO_DP4) operation = "DP4";
    else if (opcode == D3DSIO_LRP) operation = "LRP";
    else if (opcode == D3DSIO_CND) {
        if (source_count != 3 ||
            (destination & (D3DSP_DSTMOD_MASK |
                            D3DSP_DSTSHIFT_MASK)) ||
            (sources[0] & (D3DSP_REGTYPE_MASK | D3DSP_REGNUM_MASK |
                           D3DSP_SWIZZLE_MASK | D3DSP_SRCMOD_MASK |
                           D3DVS_ADDRESSMODE_MASK)) !=
                (D3DSPR_TEMP | D3DSP_REPLICATEALPHA) ||
            !qxp_shader_prepare_source(output, sources[1], 1) ||
            !qxp_shader_prepare_source(output, sources[2], 2)) {
            return FALSE;
        }
        qxp_shader_append(output,
            "ADD qd.x, r0.w, {-0.5, -0.5, -0.5, -0.5};\nCMP ");
        if (!qxp_shader_destination(
                output, destination, destination_override)) return FALSE;
        qxp_shader_append(output, ", -qd.x, ");
        if (!qxp_shader_source(output, sources[1], 1)) return FALSE;
        qxp_shader_append(output, ", ");
        if (!qxp_shader_source(output, sources[2], 2)) return FALSE;
        qxp_shader_append(output, ";\n");
        return output->valid;
    } else if (opcode == D3DSIO_CMP) {
        if (source_count != 3) return FALSE;
        destination_id = destination &
            (D3DSP_REGTYPE_MASK | D3DSP_REGNUM_MASK);
        for (index = 0; index < 3; ++index) {
            if ((sources[index] &
                 (D3DSP_REGTYPE_MASK | D3DSP_REGNUM_MASK)) ==
                    destination_id) return FALSE;
            if (!qxp_shader_prepare_source(
                    output, sources[index], index)) return FALSE;
        }
        qxp_shader_append(output, "SLT qd, ");
        if (!qxp_shader_source(output, sources[0], 0)) return FALSE;
        qxp_shader_append(output,
            ", {0.0, 0.0, 0.0, 0.0};\nLRP qd, qd, ");
        if (!qxp_shader_source(output, sources[2], 2)) return FALSE;
        qxp_shader_append(output, ", ");
        if (!qxp_shader_source(output, sources[1], 1)) return FALSE;
        qxp_shader_append(output, ";\n");
        return qxp_shader_store_result(
            output, destination, "qd", destination_override);
    } else {
        return FALSE;
    }
    return qxp_shader_instruction(
        output, operation, destination, sources, source_count,
        destination_override);
}

static BOOL qxp_shader_emit_pair(
    QXP_SHADER_TEXT *output,
    DWORD first_opcode, DWORD first_destination, const DWORD *first_sources,
    DWORD first_source_count, DWORD second_opcode,
    DWORD second_destination, const DWORD *second_sources,
    DWORD second_source_count)
{
    if (!qxp_shader_pair_masks(first_destination, second_destination) ||
        !qxp_shader_emit_arithmetic(
            output, first_opcode, first_destination, first_sources,
            first_source_count, "qp") ||
        !qxp_shader_emit_arithmetic(
            output, second_opcode, second_destination, second_sources,
            second_source_count, NULL)) {
        return FALSE;
    }
    qxp_shader_append(output, "MOV ");
    if (!qxp_shader_destination(output, first_destination, NULL)) return FALSE;
    qxp_shader_append(output, ", qp;\n");
    return output->valid;
}

BOOL QxpTranslatePixelShader1x(
    const DWORD *bytecode, DWORD bytecode_bytes, DWORD cube_overrides,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    DWORD *sampled_texture_stages, DWORD *coordinate_texture_stages,
    DWORD *cube_texture_stages, DWORD *dynamic_texture_stages,
    DWORD *defined_constants,
    DWORD constant_values[QXP_PIXEL_SHADER_CONSTANT_COUNT][4])
{
    static const char header[] =
        "!!ARBfp1.0\n"
        "ATTRIB v0 = fragment.color.primary;\n"
        "ATTRIB v1 = fragment.color.secondary;\n"
        "ATTRIB i0 = fragment.texcoord[0];\n"
        "ATTRIB i1 = fragment.texcoord[1];\n"
        "ATTRIB i2 = fragment.texcoord[2];\n"
        "ATTRIB i3 = fragment.texcoord[3];\n"
        "ATTRIB i4 = fragment.texcoord[4];\n"
        "PARAM c0 = program.local[0];\n"
        "PARAM c1 = program.local[1];\n"
        "PARAM c2 = program.local[2];\n"
        "PARAM c3 = program.local[3];\n"
        "PARAM c4 = program.local[4];\n"
        "PARAM c5 = program.local[5];\n"
        "PARAM c6 = program.local[6];\n"
        "PARAM c7 = program.local[7];\n"
        "PARAM b1 = program.local[8];\n"
        "PARAM l1 = program.local[9];\n"
        "TEMP r0, r1, r2, r3, r4, r5;\n"
        "TEMP t0, t1, t2, t3;\n"
        "TEMP q0, q1, q2, qd, qp;\n";
    QXP_SHADER_TEXT output;
    const DWORD *cursor;
    const DWORD *end;
    DWORD instruction_token;
    DWORD opcode;
    DWORD operands;
    DWORD second_instruction_token;
    DWORD second_opcode;
    DWORD second_operands;
    DWORD second_destination;
    DWORD arithmetic_slots;
    const DWORD *next_instruction;
    DWORD destination;
    DWORD arithmetic_count = 0;
    DWORD dp4_count = 0;
    DWORD cmp_count = 0;
    DWORD texture_count = 0;
    DWORD texture_stage;
    DWORD texture_source_stage;
    DWORD definition;
    DWORD index;
    DWORD shader_minor;
    DWORD initialized_texture_stages = 0;
    BOOL arithmetic_started = FALSE;
    BOOL matrix3x2_pending = FALSE;
    DWORD matrix3x3_pending = 0;

    if (!bytecode || !source || !source_bytes ||
        !sampled_texture_stages || !coordinate_texture_stages ||
        !cube_texture_stages || !dynamic_texture_stages ||
        !defined_constants || !constant_values || (cube_overrides & ~15UL) ||
        bytecode_bytes < 2 * sizeof(DWORD) ||
        (bytecode_bytes & 3) || source_capacity < 2 ||
        bytecode[0] < D3DPS_VERSION(1, 1) ||
        bytecode[0] > D3DPS_VERSION(1, 3)) return FALSE;
    shader_minor = bytecode[0] & 0xffUL;
    output.text = source;
    output.capacity = source_capacity;
    output.length = 0;
    output.valid = TRUE;
    source[0] = '\0';
    *sampled_texture_stages = 0;
    *coordinate_texture_stages = 0;
    *cube_texture_stages = 0;
    *dynamic_texture_stages = 0;
    *defined_constants = 0;
    for (definition = 0; definition < QXP_PIXEL_SHADER_CONSTANT_COUNT;
         ++definition) {
        for (index = 0; index < 4; ++index)
            constant_values[definition][index] = 0;
    }
    qxp_shader_append(&output, header);
    cursor = bytecode + 1;
    end = bytecode + bytecode_bytes / sizeof(DWORD);
    while (cursor < end) {
        instruction_token = *cursor++;
        opcode = instruction_token & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) {
            if (cursor != end || !arithmetic_count || matrix3x2_pending ||
                matrix3x3_pending ||
                (cube_overrides & ~*dynamic_texture_stages))
                return FALSE;
            qxp_shader_append(&output, "MOV result.color, r0;\nEND\n");
            if (!output.valid) return FALSE;
            *source_bytes = output.length;
            return TRUE;
        }
        if (opcode == D3DSIO_COMMENT) {
            DWORD words = (cursor[-1] & D3DSI_COMMENTSIZE_MASK) >>
                          D3DSI_COMMENTSIZE_SHIFT;
            if (words > (DWORD)(end - cursor)) return FALSE;
            cursor += words;
            continue;
        }
        if (instruction_token & D3DSI_COISSUE) return FALSE;
        if (opcode == D3DSIO_NOP) continue;
        if (opcode == D3DSIO_DEF) {
            if ((DWORD)(end - cursor) < 5 ||
                (cursor[0] & D3DSP_REGTYPE_MASK) != D3DSPR_CONST ||
                (cursor[0] & D3DSP_REGNUM_MASK) >=
                    QXP_PIXEL_SHADER_CONSTANT_COUNT) return FALSE;
            definition = cursor[0] & D3DSP_REGNUM_MASK;
            *defined_constants |= 1UL << definition;
            for (index = 0; index < 4; ++index)
                constant_values[definition][index] = cursor[1 + index];
            cursor += 5;
            continue;
        }
        if (opcode == D3DSIO_TEXCOORD || opcode == D3DSIO_TEXKILL ||
            opcode == D3DSIO_TEX) {
            if (arithmetic_started || matrix3x2_pending ||
                matrix3x3_pending || cursor >= end ||
                ++texture_count > 4 ||
                !qxp_shader_texture_destination(*cursor++,
                                                &texture_stage)) {
                return FALSE;
            }
            *coordinate_texture_stages |= 1UL << texture_stage;
            if (opcode == D3DSIO_TEX) {
                *sampled_texture_stages |= 1UL << texture_stage;
                *dynamic_texture_stages |= 1UL << texture_stage;
                initialized_texture_stages |= 1UL << texture_stage;
                if (cube_overrides & (1UL << texture_stage))
                    *cube_texture_stages |= 1UL << texture_stage;
                qxp_shader_append(&output, "TXP t");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", texture[");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output,
                    cube_overrides & (1UL << texture_stage) ?
                        "], CUBE;\n" : "], 2D;\n");
            } else if (opcode == D3DSIO_TEXCOORD) {
                initialized_texture_stages |= 1UL << texture_stage;
                qxp_shader_append(&output, "RCP qd.x, i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ".w;\nMUL_SAT t");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ".xyz, i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", qd.x;\nMOV t");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ".w, {1.0, 1.0, 1.0, 1.0};\n");
            } else {
                qxp_shader_append(&output, "RCP qd.w, i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ".w;\nMUL qd.xyz, i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output,
                    ", qd.w;\nMOV qd.w, {1.0, 1.0, 1.0, 1.0};\nKIL qd;\n");
            }
            if (!output.valid) return FALSE;
            continue;
        }
        if (opcode == D3DSIO_TEXBEM || opcode == D3DSIO_TEXBEML) {
            if (arithmetic_started || matrix3x2_pending ||
                matrix3x3_pending ||
                (DWORD)(end - cursor) < 2 ||
                ++texture_count > 4 ||
                !qxp_shader_texture_destination(cursor[0],
                                                &texture_stage) ||
                !qxp_shader_texture_source(cursor[1],
                                           &texture_source_stage, FALSE) ||
                texture_stage != 1 || texture_source_stage != 0 ||
                !(initialized_texture_stages & 1UL)) return FALSE;
            cursor += 2;
            *sampled_texture_stages |= 2UL;
            *coordinate_texture_stages |= 2UL;
            initialized_texture_stages |= 2UL;
            /* Bump maps are uploaded as biased RGBA8. Undo the bias here so
             * these instructions see the signed Du/Dv required by ps_1_1. */
            qxp_shader_append(&output,
                "MAD q0.xy, t0, {2.007874016, 2.007874016, 2.007874016, 2.007874016}, {-1.007874016, -1.007874016, -1.007874016, -1.007874016};\n"
                "MAX q0.xy, q0, {-1.0, -1.0, -1.0, -1.0};\n"
                "RCP qd.w, i1.w;\n"
                "MUL qd.xy, i1, qd.w;\n"
                "MAD qd.x, q0.x, b1.x, qd.x;\n"
                "MAD qd.x, q0.y, b1.z, qd.x;\n"
                "MAD qd.y, q0.x, b1.y, qd.y;\n"
                "MAD qd.y, q0.y, b1.w, qd.y;\n"
                "TEX t1, qd, texture[1], 2D;\n");
            if (opcode == D3DSIO_TEXBEML) {
                qxp_shader_append(&output,
                    "MAD q1.x, t0.z, l1.x, l1.y;\n"
                    "MUL t1, t1, q1.x;\n");
            }
            if (!output.valid) return FALSE;
            continue;
        }
        if (opcode == D3DSIO_TEXREG2AR ||
            opcode == D3DSIO_TEXREG2GB ||
            opcode == D3DSIO_TEXREG2RGB) {
            BOOL rgb = opcode == D3DSIO_TEXREG2RGB;

            if (arithmetic_started || matrix3x2_pending ||
                matrix3x3_pending ||
                (rgb && shader_minor < 2) ||
                (DWORD)(end - cursor) < 2 ||
                ++texture_count > 4 ||
                !qxp_shader_texture_destination(cursor[0],
                                                &texture_stage) ||
                !qxp_shader_texture_source(cursor[1],
                                           &texture_source_stage, rgb) ||
                texture_stage <= texture_source_stage ||
                !(initialized_texture_stages &
                  (1UL << texture_source_stage))) return FALSE;
            if (!qxp_shader_prepare_texture_source(&output, cursor[1]))
                return FALSE;
            cursor += 2;
            *sampled_texture_stages |= 1UL << texture_stage;
            initialized_texture_stages |= 1UL << texture_stage;
            if (rgb) {
                *dynamic_texture_stages |= 1UL << texture_stage;
                if (cube_overrides & (1UL << texture_stage))
                    *cube_texture_stages |= 1UL << texture_stage;
            }
            /* The coordinate comes from the source texture register in the
             * ARB program. Keep the sampled unit in the transport coordinate
             * mask as required by the current draw contract. */
            *coordinate_texture_stages |= 1UL << texture_stage;
            qxp_shader_append(&output, "TEX t");
            qxp_shader_append_uint(&output, texture_stage);
            qxp_shader_append(&output, ", ");
            if (!qxp_shader_texture_operand(&output,
                                             cursor[-1])) return FALSE;
            if (!rgb) {
                qxp_shader_append(&output,
                    opcode == D3DSIO_TEXREG2AR ? ".wxwx" : ".yzyz");
            }
            qxp_shader_append(&output, ", texture[");
            qxp_shader_append_uint(&output, texture_stage);
            qxp_shader_append(&output,
                rgb && (cube_overrides & (1UL << texture_stage)) ?
                    "], CUBE;\n" : "], 2D;\n");
            if (!output.valid) return FALSE;
            continue;
        }
        if (opcode == D3DSIO_TEXDP3 || opcode == D3DSIO_TEXDP3TEX) {
            BOOL lookup = opcode == D3DSIO_TEXDP3TEX;
            DWORD source_token;

            if (shader_minor < 2 || arithmetic_started ||
                matrix3x2_pending || matrix3x3_pending ||
                (DWORD)(end - cursor) < 2 || ++texture_count > 4 ||
                !qxp_shader_texture_destination(cursor[0],
                                                &texture_stage) ||
                !qxp_shader_texture_source(cursor[1],
                                           &texture_source_stage, TRUE) ||
                texture_stage <= texture_source_stage ||
                !(initialized_texture_stages &
                  (1UL << texture_source_stage))) return FALSE;
            source_token = cursor[1];
            cursor += 2;
            *coordinate_texture_stages |= 1UL << texture_stage;
            initialized_texture_stages |= 1UL << texture_stage;
            if (lookup) *sampled_texture_stages |= 1UL << texture_stage;
            qxp_shader_append(&output, "RCP qd.w, i");
            qxp_shader_append_uint(&output, texture_stage);
            qxp_shader_append(&output, ".w;\nMUL q1.xyz, i");
            qxp_shader_append_uint(&output, texture_stage);
            qxp_shader_append(&output, ", qd.w;\n");
            if (!qxp_shader_prepare_texture_source(&output, source_token))
                return FALSE;
            if (lookup) {
                qxp_shader_append(&output, "DP3 q0.x, q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output,
                    ";\nMOV q0.y, {0.0, 0.0, 0.0, 0.0};\nTEX t");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", q0, texture[");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, "], 2D;\n");
            } else {
                qxp_shader_append(&output, "DP3 t");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output, ";\n");
            }
            if (!output.valid) return FALSE;
            continue;
        }
        if (opcode == D3DSIO_TEXM3x2PAD ||
            opcode == D3DSIO_TEXM3x2TEX ||
            opcode == D3DSIO_TEXM3x2DEPTH) {
            BOOL depth = opcode == D3DSIO_TEXM3x2DEPTH;
            DWORD source_token;

            if (arithmetic_started || matrix3x3_pending ||
                (depth && shader_minor < 3) ||
                (DWORD)(end - cursor) < 2 ||
                ++texture_count > 4 ||
                !qxp_shader_texture_destination(cursor[0],
                                                &texture_stage) ||
                !qxp_shader_texture_source(cursor[1],
                                           &texture_source_stage, TRUE) ||
                texture_source_stage != 0 ||
                !(initialized_texture_stages & 1UL)) return FALSE;
            source_token = cursor[1];
            cursor += 2;
            if (opcode == D3DSIO_TEXM3x2PAD) {
                if (matrix3x2_pending || texture_stage != 1) return FALSE;
                matrix3x2_pending = TRUE;
                *coordinate_texture_stages |= 2UL;
                qxp_shader_append(&output,
                    "RCP qd.w, i1.w;\n"
                    "MUL q1.xyz, i1, qd.w;\n");
                if (!qxp_shader_prepare_texture_source(
                        &output, source_token)) return FALSE;
                qxp_shader_append(&output, "DP3 q0.x, q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output, ";\n");
            } else {
                if (!matrix3x2_pending || texture_stage != 2) return FALSE;
                matrix3x2_pending = FALSE;
                *coordinate_texture_stages |= 4UL;
                qxp_shader_append(&output,
                    "RCP qd.w, i2.w;\n"
                    "MUL q1.xyz, i2, qd.w;\n");
                if (!qxp_shader_prepare_texture_source(
                        &output, source_token)) return FALSE;
                qxp_shader_append(&output, "DP3 q0.y, q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output, ";\n");
                if (depth) {
                    qxp_shader_append(&output,
                        "RCP qd.x, q0.y;\n"
                        "MUL qd.x, q0.x, qd.x;\n"
                        "ABS qd.y, q0.y;\n"
                        "SLT qd.y, qd.y, {0.00000000000000000001, 0.00000000000000000001, 0.00000000000000000001, 0.00000000000000000001};\n"
                        "CMP qd.x, -qd.y, {1.0, 1.0, 1.0, 1.0}, qd.x;\n"
                        "MOV result.depth, qd.x;\n");
                } else {
                    *sampled_texture_stages |= 4UL;
                    initialized_texture_stages |= 4UL;
                    qxp_shader_append(&output,
                        "TEX t2, q0, texture[2], 2D;\n");
                }
            }
            if (!output.valid) return FALSE;
            continue;
        }
        if (opcode == D3DSIO_TEXM3x3PAD ||
            opcode == D3DSIO_TEXM3x3TEX ||
            opcode == D3DSIO_TEXM3x3SPEC ||
            opcode == D3DSIO_TEXM3x3VSPEC ||
            opcode == D3DSIO_TEXM3x3) {
            DWORD instruction_words =
                opcode == D3DSIO_TEXM3x3SPEC ? 3 : 2;
            DWORD reflection_source = 0;
            DWORD source_token;

            if (arithmetic_started || matrix3x2_pending ||
                (opcode == D3DSIO_TEXM3x3 && shader_minor < 2) ||
                (DWORD)(end - cursor) < instruction_words ||
                ++texture_count > 4 ||
                !qxp_shader_texture_destination(cursor[0],
                                                &texture_stage) ||
                !qxp_shader_texture_source(cursor[1],
                                           &texture_source_stage, TRUE) ||
                texture_source_stage != 0 ||
                !(initialized_texture_stages & 1UL)) return FALSE;
            source_token = cursor[1];
            if (opcode == D3DSIO_TEXM3x3SPEC) {
                reflection_source = cursor[2];
                if ((reflection_source & D3DSP_REGTYPE_MASK) !=
                        D3DSPR_CONST ||
                    (reflection_source & D3DSP_REGNUM_MASK) >=
                        QXP_PIXEL_SHADER_CONSTANT_COUNT ||
                    (reflection_source & D3DVS_ADDRESSMODE_MASK)) {
                    return FALSE;
                }
            }
            cursor += instruction_words;
            if (opcode == D3DSIO_TEXM3x3PAD) {
                if (matrix3x3_pending >= 2 ||
                    texture_stage != matrix3x3_pending + 1) return FALSE;
                ++matrix3x3_pending;
                *coordinate_texture_stages |= 1UL << texture_stage;
                qxp_shader_append(&output, "RCP qd.w, i4.w;\nMUL q1, i");
                qxp_shader_append_uint(&output, texture_stage);
                qxp_shader_append(&output, ", qd.w;\n");
                if (!qxp_shader_prepare_texture_source(
                        &output, source_token)) return FALSE;
                qxp_shader_append(&output, "DP3 q0.");
                qxp_shader_append_char(
                    &output, matrix3x3_pending == 1 ? 'x' : 'y');
                qxp_shader_append(&output, ", q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output, ";\nMOV q2.");
                qxp_shader_append_char(
                    &output, matrix3x3_pending == 1 ? 'x' : 'y');
                qxp_shader_append(&output, ", q1.w;\n");
            } else {
                if (matrix3x3_pending != 2 || texture_stage != 3)
                    return FALSE;
                matrix3x3_pending = 0;
                *coordinate_texture_stages |= 8UL;
                qxp_shader_append(&output,
                    "RCP qd.w, i4.w;\n"
                    "MUL q1, i3, qd.w;\n");
                if (!qxp_shader_prepare_texture_source(
                        &output, source_token)) return FALSE;
                qxp_shader_append(&output, "DP3 q0.z, q1, ");
                if (!qxp_shader_texture_operand(&output, source_token))
                    return FALSE;
                qxp_shader_append(&output, ";\nMOV q2.z, q1.w;\n");
                if (opcode == D3DSIO_TEXM3x3) {
                    initialized_texture_stages |= 8UL;
                    qxp_shader_append(&output,
                        "MOV t3.xyz, q0;\n"
                        "MOV t3.w, {1.0, 1.0, 1.0, 1.0};\n");
                } else if (opcode == D3DSIO_TEXM3x3SPEC) {
                    if (!qxp_shader_prepare_source(
                            &output, reflection_source, 2)) return FALSE;
                    qxp_shader_append(&output, "DP3 qd.x, q0, ");
                    if (!qxp_shader_source(
                            &output, reflection_source, 2)) return FALSE;
                    qxp_shader_append(&output,
                        ";\nDP3 qd.y, q0, q0;\n"
                        "RCP qd.y, qd.y;\n"
                        "MUL qd.x, qd.x, qd.y;\n"
                        "MUL qd.x, qd.x, {2.0, 2.0, 2.0, 2.0};\n"
                        "MUL q1.xyz, qd.x, q0;\nSUB q0.xyz, q1, ");
                    if (!qxp_shader_source(
                            &output, reflection_source, 2)) return FALSE;
                    qxp_shader_append(&output, ";\n");
                } else if (opcode == D3DSIO_TEXM3x3VSPEC) {
                    qxp_shader_append(&output,
                        "DP3 qd.x, q0, q2;\n"
                        "DP3 qd.y, q0, q0;\n"
                        "RCP qd.y, qd.y;\n"
                        "MUL qd.x, qd.x, qd.y;\n"
                        "MUL qd.x, qd.x, {2.0, 2.0, 2.0, 2.0};\n"
                        "MUL q1.xyz, qd.x, q0;\n"
                        "SUB q0.xyz, q1, q2;\n");
                }
                if (opcode != D3DSIO_TEXM3x3) {
                    *sampled_texture_stages |= 8UL;
                    *cube_texture_stages |= 8UL;
                    initialized_texture_stages |= 8UL;
                    qxp_shader_append(&output,
                        "TEX t3, q0, texture[3], CUBE;\n");
                }
            }
            if (!output.valid) return FALSE;
            continue;
        }
        if (matrix3x2_pending || matrix3x3_pending) return FALSE;
        operands = qxp_shader_operand_count(opcode);
        if (!operands || (DWORD)(end - cursor) < operands + 1) return FALSE;
        destination = cursor[0];
        next_instruction = cursor + operands + 1;
        if (next_instruction < end &&
            (*next_instruction & D3DSI_COISSUE)) {
            second_instruction_token = *next_instruction;
            second_opcode =
                second_instruction_token & D3DSI_OPCODE_MASK;
            second_operands = qxp_shader_operand_count(second_opcode);
            if (!second_operands ||
                (DWORD)(end - (next_instruction + 1)) <
                    second_operands + 1) return FALSE;
            second_destination = next_instruction[1];
            if (opcode == D3DSIO_DP4 && ++dp4_count > 4) return FALSE;
            if (opcode == D3DSIO_CMP && ++cmp_count > 3) return FALSE;
            if (second_opcode == D3DSIO_DP4 &&
                ++dp4_count > 4) return FALSE;
            if (second_opcode == D3DSIO_CMP &&
                ++cmp_count > 3) return FALSE;
            arithmetic_slots = qxp_shader_arithmetic_slots(opcode);
            if (qxp_shader_arithmetic_slots(second_opcode) >
                    arithmetic_slots) {
                arithmetic_slots =
                    qxp_shader_arithmetic_slots(second_opcode);
            }
            if (arithmetic_count > 8 - arithmetic_slots) return FALSE;
            arithmetic_count += arithmetic_slots;
            arithmetic_started = TRUE;
            if (!qxp_shader_emit_pair(
                    &output, opcode, destination, cursor + 1, operands,
                    second_opcode, second_destination,
                    next_instruction + 2, second_operands)) return FALSE;
            cursor = next_instruction + second_operands + 2;
            continue;
        }
        if (opcode == D3DSIO_DP4 && ++dp4_count > 4) return FALSE;
        if (opcode == D3DSIO_CMP && ++cmp_count > 3) return FALSE;
        arithmetic_slots = qxp_shader_arithmetic_slots(opcode);
        if (arithmetic_count > 8 - arithmetic_slots) return FALSE;
        arithmetic_count += arithmetic_slots;
        arithmetic_started = TRUE;
        if (!qxp_shader_emit_arithmetic(
                &output, opcode, destination, cursor + 1, operands,
                NULL)) return FALSE;
        cursor += operands + 1;
    }
    return FALSE;
}
