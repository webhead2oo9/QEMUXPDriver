#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_vshader.h"

#ifndef D3DSP_REGNUM_MASK
#define D3DSP_REGNUM_MASK 0x00001fffUL
#endif

/* D3D8 sets bit 31 on every source and destination parameter token.  The
 * register-type mask deliberately excludes it, so account for the marker
 * explicitly instead of treating standard runtime bytecode as reserved. */
#define QXP_VS_PARAMETER_TOKEN 0x80000000UL

#define QXP_VS_DEST_ALLOWED \
    (D3DSP_REGNUM_MASK | D3DSP_WRITEMASK_ALL | D3DSP_DSTMOD_MASK | \
     D3DSP_DSTSHIFT_MASK | D3DSP_REGTYPE_MASK | QXP_VS_PARAMETER_TOKEN)
#define QXP_VS_SOURCE_ALLOWED \
    (D3DSP_REGNUM_MASK | D3DVS_ADDRESSMODE_MASK | D3DSP_SWIZZLE_MASK | \
     D3DSP_SRCMOD_MASK | D3DSP_REGTYPE_MASK | QXP_VS_PARAMETER_TOKEN)


typedef struct qxp_vs_text {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_VS_TEXT;

typedef struct qxp_vs_state {
    DWORD used_inputs;
    DWORD written_outputs;
    DWORD position_mask;
    DWORD fog_mask;
    DWORD point_mask;
    DWORD color_mask[2];
    DWORD texcoord_mask[8];
    DWORD instruction_slots;
    BOOL address_initialized;
    BOOL uses_relative;
} QXP_VS_STATE;

static void qxp_vs_append_char(QXP_VS_TEXT *output, char value)
{
    if (!output || !output->valid || output->length + 1 >= output->capacity) {
        if (output) output->valid = FALSE;
        return;
    }
    output->text[output->length++] = value;
    output->text[output->length] = '\0';
}

static void qxp_vs_append(QXP_VS_TEXT *output, const char *value)
{
    if (!output || !value) return;
    while (*value) qxp_vs_append_char(output, *value++);
}

static void qxp_vs_append_uint(QXP_VS_TEXT *output, DWORD value)
{
    char digits[11];
    DWORD count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < (DWORD)sizeof(digits));
    while (count) qxp_vs_append_char(output, digits[--count]);
}

static void qxp_vs_append_mask(QXP_VS_TEXT *output, DWORD mask)
{
    if (mask == D3DSP_WRITEMASK_ALL) return;
    qxp_vs_append_char(output, '.');
    if (mask & D3DSP_WRITEMASK_0) qxp_vs_append_char(output, 'x');
    if (mask & D3DSP_WRITEMASK_1) qxp_vs_append_char(output, 'y');
    if (mask & D3DSP_WRITEMASK_2) qxp_vs_append_char(output, 'z');
    if (mask & D3DSP_WRITEMASK_3) qxp_vs_append_char(output, 'w');
}

static void qxp_vs_append_swizzle(QXP_VS_TEXT *output, DWORD token)
{
    static const char component[4] = { 'x', 'y', 'z', 'w' };
    DWORD swizzle;
    DWORD index;

    if ((token & D3DSP_SWIZZLE_MASK) == D3DSP_NOSWIZZLE) return;
    swizzle = (token & D3DSP_SWIZZLE_MASK) >> D3DSP_SWIZZLE_SHIFT;
    qxp_vs_append_char(output, '.');
    for (index = 0; index < 4; ++index)
        qxp_vs_append_char(output,
                           component[(swizzle >> (index * 2)) & 3]);
}

static BOOL qxp_vs_replicated_source(DWORD token)
{
    DWORD swizzle = (token & D3DSP_SWIZZLE_MASK) >>
                    D3DSP_SWIZZLE_SHIFT;
    DWORD first = swizzle & 3;

    return ((swizzle >> 2) & 3) == first &&
           ((swizzle >> 4) & 3) == first &&
           ((swizzle >> 6) & 3) == first;
}

static BOOL qxp_vs_validate_source(QXP_VS_STATE *state, DWORD token)
{
    DWORD type;
    DWORD number;
    DWORD modifier;
    BOOL relative;

    if (!state || !(token & QXP_VS_PARAMETER_TOKEN) ||
        (token & ~QXP_VS_SOURCE_ALLOWED)) return FALSE;
    type = token & D3DSP_REGTYPE_MASK;
    number = token & D3DSP_REGNUM_MASK;
    modifier = token & D3DSP_SRCMOD_MASK;
    relative = (token & D3DVS_ADDRESSMODE_MASK) != 0;
    if (modifier != D3DSPSM_NONE && modifier != D3DSPSM_NEG)
        return FALSE;
    if (type == D3DSPR_TEMP) {
        if (number >= 12 || relative) return FALSE;
    } else if (type == D3DSPR_INPUT) {
        if (number >= 16 || relative) return FALSE;
        state->used_inputs |= 1UL << number;
    } else if (type == D3DSPR_CONST) {
        if (number >= QXP_VERTEX_SHADER_CONSTANT_COUNT) return FALSE;
        if (relative) {
            if (!state->address_initialized) return FALSE;
            state->uses_relative = TRUE;
        }
    } else {
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_validate_destination(QXP_VS_STATE *state, DWORD token)
{
    DWORD type;
    DWORD number;
    DWORD mask;

    if (!state || !(token & QXP_VS_PARAMETER_TOKEN) ||
        (token & ~QXP_VS_DEST_ALLOWED) ||
        (token & (D3DSP_DSTMOD_MASK | D3DSP_DSTSHIFT_MASK))) return FALSE;
    type = token & D3DSP_REGTYPE_MASK;
    number = token & D3DSP_REGNUM_MASK;
    mask = token & D3DSP_WRITEMASK_ALL;
    if (!mask) return FALSE;
    if (type == D3DSPR_TEMP) return number < 12;
    if (type == D3DSPR_ADDR)
        return number == 0 && mask == D3DSP_WRITEMASK_0;
    if (type == D3DSPR_RASTOUT) {
        /* Position may be written whole or per-component across several
         * instructions.  POP projects with four DP4s (oPos.x/y/z/w =
         * dot(r0, c0..c3)); accumulate the lanes and let
         * qxp_vs_finish_outputs require that all four are written. */
        if (number == D3DSRO_POSITION) {
            state->position_mask |= mask;
            state->written_outputs |= QXP_VS_OUTPUT_POSITION;
            return TRUE;
        }
        /* oFog and oPts are scalar outputs.  Accept any non-zero write mask
         * here (POP emits a full .xyzw "mul oFog"); qxp_vs_finish_outputs
         * enforces the single-lane-or-full-broadcast scalar rule on the
         * accumulated mask, and the emitter consumes the first written
         * lane. */
        if (number == D3DSRO_FOG) {
            state->fog_mask |= mask;
            state->written_outputs |= QXP_VS_OUTPUT_FOG;
            return TRUE;
        }
        if (number == D3DSRO_POINT_SIZE) {
            state->point_mask |= mask;
            state->written_outputs |= QXP_VS_OUTPUT_POINT_SIZE;
            return TRUE;
        }
        return FALSE;
    }
    if (type == D3DSPR_ATTROUT && number < 2) {
        state->color_mask[number] |= mask;
        state->written_outputs |= number ? QXP_VS_OUTPUT_COLOR1 :
                                           QXP_VS_OUTPUT_COLOR0;
        return TRUE;
    }
    if (type == D3DSPR_TEXCRDOUT && number < 8) {
        state->texcoord_mask[number] |= mask;
        state->written_outputs |= QXP_VS_OUTPUT_TEXCOORD0 << number;
        return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_same_register(DWORD first, DWORD second)
{
    DWORD identity = D3DSP_REGTYPE_MASK | D3DSP_REGNUM_MASK;
    return (first & identity) == (second & identity);
}

static BOOL qxp_vs_validate_read_ports(const DWORD *sources,
                                       DWORD source_count, BOOL matrix)
{
    DWORD input = 0xffffffffUL;
    DWORD constant = 0xffffffffUL;
    DWORD index;

    if (!sources || !source_count || source_count > 3) return FALSE;
    for (index = 0; index < source_count; ++index) {
        DWORD type = sources[index] & D3DSP_REGTYPE_MASK;
        DWORD identity = sources[index] &
            (D3DSP_REGNUM_MASK | D3DVS_ADDRESSMODE_MASK);

        if (type == D3DSPR_INPUT) {
            if (input != 0xffffffffUL && input != identity) return FALSE;
            input = identity;
        } else if (type == D3DSPR_CONST) {
            if (matrix && index == 0) return FALSE;
            if (!matrix && constant != 0xffffffffUL &&
                constant != identity) return FALSE;
            constant = identity;
        }
    }
    return TRUE;
}

static BOOL qxp_vs_emit_destination(QXP_VS_TEXT *output, DWORD token)
{
    DWORD type = token & D3DSP_REGTYPE_MASK;
    DWORD number = token & D3DSP_REGNUM_MASK;

    if (type == D3DSPR_TEMP) {
        qxp_vs_append_char(output, 'r');
        qxp_vs_append_uint(output, number);
    } else if (type == D3DSPR_RASTOUT) {
        if (number == D3DSRO_POSITION) qxp_vs_append(output, "qp");
        else if (number == D3DSRO_FOG)
            qxp_vs_append(output, "qf");
        else if (number == D3DSRO_POINT_SIZE)
            qxp_vs_append(output, "qs");
        else return FALSE;
    } else if (type == D3DSPR_ATTROUT) {
        qxp_vs_append(output, number ? "result.color.secondary" :
                                       "result.color.primary");
    } else if (type == D3DSPR_TEXCRDOUT) {
        /* Texcoords 0-3 route through qt mirrors so the END epilogue can
         * apply D3D's per-stage projected-q selection; result.* is write-only
         * in ARBvp, so the fixup must read an initialized temp. 4-7 emit
         * directly to result.texcoord. */
        if (number < 4) {
            qxp_vs_append(output, "qt");
            qxp_vs_append_uint(output, number);
        } else {
            qxp_vs_append(output, "result.texcoord[");
            qxp_vs_append_uint(output, number);
            qxp_vs_append_char(output, ']');
        }
    } else {
        return FALSE;
    }
    qxp_vs_append_mask(output, token & D3DSP_WRITEMASK_ALL);
    return output->valid;
}

static void qxp_vs_emit_source_register(QXP_VS_TEXT *output, DWORD token,
                                        DWORD constant_delta)
{
    DWORD type = token & D3DSP_REGTYPE_MASK;
    DWORD number = token & D3DSP_REGNUM_MASK;

    if (token & D3DSPSM_NEG) qxp_vs_append_char(output, '-');
    if (type == D3DSPR_TEMP) {
        qxp_vs_append_char(output, 'r');
        qxp_vs_append_uint(output, number);
    } else if (type == D3DSPR_INPUT) {
        qxp_vs_append(output, "vertex.attrib[");
        qxp_vs_append_uint(output, number);
        qxp_vs_append_char(output, ']');
    } else {
        qxp_vs_append(output, "c[");
        qxp_vs_append_uint(output, number + constant_delta);
        qxp_vs_append_char(output, ']');
    }
    qxp_vs_append_swizzle(output, token);
}

static BOOL qxp_vs_prepare_source(QXP_VS_TEXT *output, DWORD token,
                                  DWORD scratch, DWORD constant_delta)
{
    DWORD offset;

    if (!(token & D3DVS_ADDRESSMODE_MASK)) return TRUE;
    if (scratch > 2 ||
        (token & D3DSP_REGTYPE_MASK) != D3DSPR_CONST) return FALSE;
    offset = (token & D3DSP_REGNUM_MASK) + constant_delta;
    qxp_vs_append(output, "ADD q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".x, qa.x, {");
    qxp_vs_append_uint(output, offset);
    qxp_vs_append(output, ".0};\nMAX q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".x, {0.0};\nMIN q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, {95.0};\nSGE q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".z, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".x, {0.0};\nSLT q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".w, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".x, {96.0};\nMUL q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".z, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".z, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".w;\nMAD q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".z, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, {96.0};\nMAD q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".z, {-96.0}, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y;\nARL a0.x, q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ".y;\nMOV q");
    qxp_vs_append_uint(output, scratch);
    qxp_vs_append(output, ", c[a0.x];\n");
    return output->valid;
}

static BOOL qxp_vs_emit_source(QXP_VS_TEXT *output, DWORD token,
                               DWORD scratch, DWORD constant_delta)
{
    if (token & D3DVS_ADDRESSMODE_MASK) {
        if (token & D3DSPSM_NEG) qxp_vs_append_char(output, '-');
        qxp_vs_append_char(output, 'q');
        qxp_vs_append_uint(output, scratch);
        qxp_vs_append_swizzle(output, token);
    } else {
        qxp_vs_emit_source_register(output, token, constant_delta);
    }
    return output->valid;
}

static BOOL qxp_vs_emit_scalar_source(QXP_VS_TEXT *output, DWORD token,
                                      DWORD scratch, DWORD constant_delta)
{
    static const char component[4] = { 'x', 'y', 'z', 'w' };
    DWORD selected = ((token & D3DSP_SWIZZLE_MASK) >>
                      D3DSP_SWIZZLE_SHIFT) & 3;
    DWORD type = token & D3DSP_REGTYPE_MASK;
    DWORD number = token & D3DSP_REGNUM_MASK;

    if (token & D3DSPSM_NEG) qxp_vs_append_char(output, '-');
    if (token & D3DVS_ADDRESSMODE_MASK) {
        qxp_vs_append_char(output, 'q');
        qxp_vs_append_uint(output, scratch);
    } else if (type == D3DSPR_TEMP) {
        qxp_vs_append_char(output, 'r');
        qxp_vs_append_uint(output, number);
    } else if (type == D3DSPR_INPUT) {
        qxp_vs_append(output, "vertex.attrib[");
        qxp_vs_append_uint(output, number);
        qxp_vs_append_char(output, ']');
    } else {
        qxp_vs_append(output, "c[");
        qxp_vs_append_uint(output, number + constant_delta);
        qxp_vs_append_char(output, ']');
    }
    qxp_vs_append_char(output, '.');
    qxp_vs_append_char(output, component[selected]);
    return output->valid;
}

static BOOL qxp_vs_emit_normal(QXP_VS_TEXT *output, const char *operation,
                               DWORD destination, const DWORD *sources,
                               const DWORD *deltas, DWORD source_count)
{
    DWORD scratch[3] = { 0, 1, 2 };
    DWORD index;

    for (index = 0; index < source_count; ++index) {
        DWORD previous;
        DWORD delta = deltas ? deltas[index] : 0;

        if (sources[index] & D3DVS_ADDRESSMODE_MASK) {
            for (previous = 0; previous < index; ++previous) {
                DWORD previous_delta = deltas ? deltas[previous] : 0;
                DWORD operand_mask = (DWORD)~(D3DSP_SWIZZLE_MASK |
                                              D3DSP_SRCMOD_MASK);
                if ((sources[previous] & D3DVS_ADDRESSMODE_MASK) &&
                    (sources[index] & operand_mask) ==
                        (sources[previous] & operand_mask) &&
                    delta == previous_delta) {
                    scratch[index] = scratch[previous];
                    break;
                }
            }
        }
        if (scratch[index] == index &&
            !qxp_vs_prepare_source(output, sources[index], index, delta))
            return FALSE;
    }
    qxp_vs_append(output, operation);
    qxp_vs_append_char(output, ' ');
    if (!qxp_vs_emit_destination(output, destination)) return FALSE;
    for (index = 0; index < source_count; ++index) {
        qxp_vs_append(output, ", ");
        if (!qxp_vs_emit_source(output, sources[index], scratch[index],
                                deltas ? deltas[index] : 0)) return FALSE;
    }
    qxp_vs_append(output, ";\n");
    return output->valid;
}

static BOOL qxp_vs_emit_scalar_normal(QXP_VS_TEXT *output,
                                      const char *operation,
                                      DWORD destination, DWORD source)
{
    if (!qxp_vs_prepare_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, operation);
    qxp_vs_append_char(output, ' ');
    if (!qxp_vs_emit_destination(output, destination)) return FALSE;
    qxp_vs_append(output, ", ");
    if (!qxp_vs_emit_scalar_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, ";\n");
    return output->valid;
}

static BOOL qxp_vs_emit_address_move(QXP_VS_TEXT *output, DWORD source)
{
    if (!qxp_vs_prepare_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, "ADD qa.x, ");
    if (!qxp_vs_emit_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output,
        ", {0.5};\nFLR qa.x, qa.x;\nARL a0.x, qa.x;\n");
    return output->valid;
}

static BOOL qxp_vs_emit_rsq(QXP_VS_TEXT *output, DWORD destination,
                            DWORD source)
{
    if (!qxp_vs_prepare_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, "ABS qd.x, ");
    if (!qxp_vs_emit_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, ";\nRSQ ");
    if (!qxp_vs_emit_destination(output, destination)) return FALSE;
    qxp_vs_append(output, ", qd.x;\n");
    return output->valid;
}

static BOOL qxp_vs_emit_log(QXP_VS_TEXT *output, DWORD destination,
                            DWORD source)
{
    if (!qxp_vs_prepare_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, "ABS qd.x, ");
    if (!qxp_vs_emit_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output,
        ";\nSLT qd.w, -qd.x, {0.0};\n"
        "SUB qd.y, {1.0}, qd.w;\n"
        "ADD qd.x, qd.x, qd.y;\n"
        "LG2 qd.z, qd.x;\n"
        "MUL qd.z, qd.z, qd.w;\n"
        "MAD qd.z, qd.y, {-3.402823466e+38}, qd.z;\nMOV ");
    if (!qxp_vs_emit_destination(output, destination)) return FALSE;
    qxp_vs_append(output, ", qd.z;\n");
    return output->valid;
}

static BOOL qxp_vs_emit_lit(QXP_VS_TEXT *output, DWORD destination,
                            DWORD source)
{
    if (!qxp_vs_prepare_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output, "MOV qd, ");
    if (!qxp_vs_emit_source(output, source, 0, 0)) return FALSE;
    qxp_vs_append(output,
        ";\nMIN qd.w, qd.w, {127.99609375};\n"
        "MAX qd.w, qd.w, {-127.99609375};\nLIT ");
    if (!qxp_vs_emit_destination(output, destination)) return FALSE;
    qxp_vs_append(output, ", qd;\n");
    return output->valid;
}

static BOOL qxp_vs_matrix_info(DWORD opcode, DWORD *rows, DWORD *columns,
                               DWORD *required_mask, BOOL *allow_xyz)
{
    if (!rows || !columns || !required_mask || !allow_xyz) return FALSE;
    *allow_xyz = FALSE;
    switch (opcode) {
    case D3DSIO_M4x4:
        *rows = 4; *columns = 4;
        *required_mask = D3DSP_WRITEMASK_ALL; *allow_xyz = TRUE;
        return TRUE;
    case D3DSIO_M4x3:
        *rows = 3; *columns = 4;
        *required_mask = D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                         D3DSP_WRITEMASK_2;
        return TRUE;
    case D3DSIO_M3x4:
        *rows = 4; *columns = 3;
        *required_mask = D3DSP_WRITEMASK_ALL; *allow_xyz = TRUE;
        return TRUE;
    case D3DSIO_M3x3:
        *rows = 3; *columns = 3;
        *required_mask = D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                         D3DSP_WRITEMASK_2;
        return TRUE;
    case D3DSIO_M3x2:
        *rows = 2; *columns = 3;
        *required_mask = D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1;
        return TRUE;
    }
    return FALSE;
}

static BOOL qxp_vs_emit_matrix(QXP_VS_TEXT *output, DWORD opcode,
                               DWORD destination, const DWORD *sources)
{
    static const DWORD masks[4] = {
        D3DSP_WRITEMASK_0, D3DSP_WRITEMASK_1,
        D3DSP_WRITEMASK_2, D3DSP_WRITEMASK_3
    };
    DWORD rows;
    DWORD columns;
    DWORD required_mask;
    DWORD destination_mask;
    DWORD deltas[2] = { 0, 0 };
    DWORD row;
    BOOL allow_xyz;

    if (!qxp_vs_matrix_info(opcode, &rows, &columns, &required_mask,
                            &allow_xyz)) return FALSE;
    destination_mask = destination & D3DSP_WRITEMASK_ALL;
    if (destination_mask != required_mask &&
        !(allow_xyz && destination_mask ==
          (D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
           D3DSP_WRITEMASK_2))) return FALSE;
    if ((sources[1] & D3DSP_REGTYPE_MASK) != D3DSPR_CONST ||
        (sources[1] & (D3DSP_SWIZZLE_MASK | D3DSP_SRCMOD_MASK)) !=
            D3DSP_NOSWIZZLE ||
        qxp_vs_same_register(destination, sources[0])) return FALSE;
    if (!(sources[1] & D3DVS_ADDRESSMODE_MASK) &&
        (sources[1] & D3DSP_REGNUM_MASK) + rows >
            QXP_VERTEX_SHADER_CONSTANT_COUNT) return FALSE;
    for (row = 0; row < rows; ++row) {
        DWORD row_destination;

        if (!(destination_mask & masks[row])) continue;
        row_destination = (destination & ~D3DSP_WRITEMASK_ALL) | masks[row];
        deltas[1] = row;
        if (!qxp_vs_emit_normal(output, columns == 4 ? "DP4" : "DP3",
                                row_destination, sources, deltas, 2))
            return FALSE;
    }
    return output->valid;
}

static BOOL qxp_vs_opcode(DWORD opcode, const char **operation,
                          DWORD *source_count, DWORD *slots)
{
    if (!operation || !source_count || !slots) return FALSE;
    *operation = NULL;
    switch (opcode) {
    case D3DSIO_MOV: *operation = "MOV"; *source_count = 1; *slots = 1; break;
    case D3DSIO_ADD: *operation = "ADD"; *source_count = 2; *slots = 1; break;
    case D3DSIO_SUB: *operation = "SUB"; *source_count = 2; *slots = 1; break;
    case D3DSIO_MAD: *operation = "MAD"; *source_count = 3; *slots = 1; break;
    case D3DSIO_MUL: *operation = "MUL"; *source_count = 2; *slots = 1; break;
    case D3DSIO_RCP: *operation = "RCP"; *source_count = 1; *slots = 1; break;
    case D3DSIO_RSQ: *source_count = 1; *slots = 1; break;
    case D3DSIO_DP3: *operation = "DP3"; *source_count = 2; *slots = 1; break;
    case D3DSIO_DP4: *operation = "DP4"; *source_count = 2; *slots = 1; break;
    case D3DSIO_MIN: *operation = "MIN"; *source_count = 2; *slots = 1; break;
    case D3DSIO_MAX: *operation = "MAX"; *source_count = 2; *slots = 1; break;
    case D3DSIO_SLT: *operation = "SLT"; *source_count = 2; *slots = 1; break;
    case D3DSIO_SGE: *operation = "SGE"; *source_count = 2; *slots = 1; break;
    case D3DSIO_EXP: *operation = "EX2"; *source_count = 1; *slots = 10; break;
    case D3DSIO_LOG: *source_count = 1; *slots = 10; break;
    case D3DSIO_LIT: *operation = "LIT"; *source_count = 1; *slots = 1; break;
    case D3DSIO_DST: *operation = "DST"; *source_count = 2; *slots = 1; break;
    case D3DSIO_FRC: *operation = "FRC"; *source_count = 1; *slots = 3; break;
    case D3DSIO_M4x4: *source_count = 2; *slots = 4; break;
    case D3DSIO_M4x3: *source_count = 2; *slots = 3; break;
    case D3DSIO_M3x4: *source_count = 2; *slots = 4; break;
    case D3DSIO_M3x3: *source_count = 2; *slots = 3; break;
    case D3DSIO_M3x2: *source_count = 2; *slots = 2; break;
    case D3DSIO_EXPP: *operation = "EXP"; *source_count = 1; *slots = 1; break;
    case D3DSIO_LOGP: *source_count = 1; *slots = 1; break;
    default: return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vs_finish_outputs(QXP_VS_STATE *state)
{
    DWORD highest = 0;
    DWORD index;
    BOOL any = FALSE;

    /* Position must be fully written.  oFog/oPts are scalar: a single lane
     * or a full .xyzw broadcast (the form POP emits, "mul oFog, r6.w,
     * c20.z") is valid; scattered multi-lane writes have no scalar meaning
     * and are rejected. */
    if (!state || state->position_mask != D3DSP_WRITEMASK_ALL) return FALSE;
    if (state->fog_mask != D3DSP_WRITEMASK_ALL &&
        (state->fog_mask & (state->fog_mask - 1))) return FALSE;
    if (state->point_mask != D3DSP_WRITEMASK_ALL &&
        (state->point_mask & (state->point_mask - 1))) return FALSE;
    for (index = 0; index < 8; ++index) {
        DWORD mask = state->texcoord_mask[index];
        if (!mask) continue;
        if (mask != D3DSP_WRITEMASK_0 &&
            mask != (D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1) &&
            mask != (D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                     D3DSP_WRITEMASK_2) &&
            mask != D3DSP_WRITEMASK_ALL) return FALSE;
        highest = index;
        any = TRUE;
    }
    if (any) {
        for (index = 0; index < highest; ++index)
            if (!state->texcoord_mask[index]) return FALSE;
    }
    return TRUE;
}

static void qxp_vs_emit_scalar_output(QXP_VS_TEXT *output,
                                      const char *destination,
                                      const char *temporary, DWORD mask)
{
    static const char component[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;

    if (!mask) return;
    qxp_vs_append(output, "MOV ");
    qxp_vs_append(output, destination);
    qxp_vs_append(output, ", ");
    qxp_vs_append(output, temporary);
    qxp_vs_append_char(output, '.');
    for (index = 0; index < 4; ++index) {
        if (mask & (D3DSP_WRITEMASK_0 << index)) {
            qxp_vs_append_char(output, component[index]);
            break;
        }
    }
    qxp_vs_append(output, ";\n");
}

static void qxp_vs_emit_fog_output(QXP_VS_TEXT *output,
                                   const char *temporary, DWORD mask)
{
    static const char component[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;

    if (!mask) {
        qxp_vs_append(output, "MOV result.fogcoord.x, qp.x;\n");
        return;
    }
    qxp_vs_append(output, "MAX qf.x, ");
    qxp_vs_append(output, temporary);
    qxp_vs_append_char(output, '.');
    for (index = 0; index < 4; ++index) {
        if (mask & (D3DSP_WRITEMASK_0 << index)) {
            qxp_vs_append_char(output, component[index]);
            break;
        }
    }
    qxp_vs_append(output,
        ", {0.0}.x;\n"
        "MIN qf.x, qf.x, {1.0}.x;\n"
        "SUB qf.x, {1.0}.x, qf.x;\n"
        "SUB qf.y, {1.0}.x, program.local[0].y;\n"
        "MUL qp.x, qp.x, program.local[0].y;\n"
        "MAD result.fogcoord.x, qf.x, qf.y, qp.x;\n");
}

/* Emit D3D's per-stage projected-q selection for each written texcoord 0-3.
 * The ps samples every texture projectively (TXP), so D3D's nonprojected
 * stages need q=1 while projected stages keep the shader's q. program.local[1]
 * carries the per-stage projected mask {P0,P1,P2,P3}; each
 * result.texcoord[N].w = qtN.w*P + (1-P). result.* is write-only, so the
 * shader wrote texcoords into qt mirrors (initialized {0,0,0,1} so a partial
 * oTn.xy write defaults q to 1). */
static void qxp_vs_emit_texcoord_output(QXP_VS_TEXT *output,
                                        const QXP_VS_STATE *state)
{
    static const char component[4] = { 'x', 'y', 'z', 'w' };
    DWORD index;
    BOOL any = FALSE;

    for (index = 0; index < 4; ++index) {
        if (state->texcoord_mask[index]) { any = TRUE; break; }
    }
    if (!any) return;
    qxp_vs_append(output, "SUB qd, {1.0, 1.0, 1.0, 1.0}, program.local[1];\n");
    for (index = 0; index < 4; ++index) {
        if (!state->texcoord_mask[index]) continue;
        qxp_vs_append(output, "MOV result.texcoord[");
        qxp_vs_append_uint(output, index);
        qxp_vs_append(output, "].xyz, qt");
        qxp_vs_append_uint(output, index);
        qxp_vs_append(output, ";\n");
        qxp_vs_append(output, "MAD result.texcoord[");
        qxp_vs_append_uint(output, index);
        qxp_vs_append(output, "].w, qt");
        qxp_vs_append_uint(output, index);
        qxp_vs_append(output, ".w, program.local[1].");
        qxp_vs_append_char(output, component[index]);
        qxp_vs_append(output, ", qd.");
        qxp_vs_append_char(output, component[index]);
        qxp_vs_append(output, ";\n");
    }
}

static BOOL qxp_translate_vertex_shader11(
    const DWORD *bytecode, DWORD bytecode_bytes,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    DWORD *used_inputs, DWORD *written_outputs,
    BOOL *uses_relative_addressing)
{
    static const char header[] =
        "!!ARBvp1.0\n"
        "PARAM c[97] = { program.env[0..95], {0.0, 0.0, 0.0, 0.0} };\n"
        "TEMP r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11;\n"
        "TEMP qp, qf, qs, qa, q0, q1, q2, qd;\n"
        "TEMP qt0, qt1, qt2, qt3;\n"
        "ADDRESS a0;\n"
        /* qt mirrors start at {0,0,0,1} so a partial oTn.xy write leaves q=1
         * (D3D nonprojected q), which the END epilogue then selects against
         * the per-stage projected mask. */
        "MOV qt0, {0.0, 0.0, 0.0, 1.0};\n"
        "MOV qt1, {0.0, 0.0, 0.0, 1.0};\n"
        "MOV qt2, {0.0, 0.0, 0.0, 1.0};\n"
        "MOV qt3, {0.0, 0.0, 0.0, 1.0};\n";
    QXP_VS_TEXT output;
    QXP_VS_STATE state;
    const DWORD *cursor;
    const DWORD *end;

    if (!bytecode || !source || !source_bytes || !used_inputs ||
        !written_outputs || !uses_relative_addressing ||
        bytecode_bytes < 2 * sizeof(DWORD) || (bytecode_bytes & 3) ||
        source_capacity < 2 || bytecode[0] != D3DVS_VERSION(1, 1))
        return FALSE;
    output.text = source;
    output.capacity = source_capacity;
    output.length = 0;
    output.valid = TRUE;
    source[0] = '\0';
    state.used_inputs = 0;
    state.written_outputs = 0;
    state.position_mask = 0;
    state.fog_mask = 0;
    state.point_mask = 0;
    state.color_mask[0] = state.color_mask[1] = 0;
    state.instruction_slots = 0;
    state.address_initialized = FALSE;
    state.uses_relative = FALSE;
    {
        DWORD index;
        for (index = 0; index < 8; ++index) state.texcoord_mask[index] = 0;
    }
    qxp_vs_append(&output, header);
    cursor = bytecode + 1;
    end = bytecode + bytecode_bytes / sizeof(DWORD);
    while (cursor < end) {
        DWORD instruction = *cursor++;
        DWORD opcode = instruction & D3DSI_OPCODE_MASK;
        const char *operation;
        DWORD source_count;
        DWORD slots;
        DWORD destination;
        DWORD sources[3];
        DWORD index;
        DWORD destination_type;
        DWORD destination_mask;
        BOOL scalar;
        BOOL matrix;

        if (opcode == D3DSIO_END) {
            if (instruction != D3DSIO_END || cursor != end ||
                !qxp_vs_finish_outputs(&state)) return FALSE;
            qxp_vs_append(&output,
                "MOV result.position.x, qp.x;\n"
                "MUL result.position.y, qp.y, program.local[0].x;\n"
                "MAD result.position.z, qp.z, {2.0}, -qp.w;\n"
                "MOV result.position.w, qp.w;\n"
                "RCP qp.x, qp.w;\n"
                "MUL qp.x, qp.z, qp.x;\n");
            /* D3D oFog is the final near=1/far=0 blend factor. OpenGL's
             * linear fog state consumes a coordinate, so start=0/end=1
             * needs coordinate 1-oFog to reproduce that factor. Local 0.y
             * selects D3D table fog's post-divide z instead. */
            qxp_vs_emit_fog_output(&output, "qf", state.fog_mask);
            qxp_vs_emit_scalar_output(&output, "result.pointsize.x",
                                      "qs", state.point_mask);
            qxp_vs_emit_texcoord_output(&output, &state);
            qxp_vs_append(&output, "END\n");
            if (!output.valid) return FALSE;
            *source_bytes = output.length;
            *used_inputs = state.used_inputs;
            *written_outputs = state.written_outputs;
            *uses_relative_addressing = state.uses_relative;
            return TRUE;
        }
        if (opcode == D3DSIO_COMMENT) {
            DWORD words;
            if (instruction & ~(D3DSI_OPCODE_MASK |
                                D3DSI_COMMENTSIZE_MASK)) return FALSE;
            words = (instruction & D3DSI_COMMENTSIZE_MASK) >>
                    D3DSI_COMMENTSIZE_SHIFT;
            if (words > (DWORD)(end - cursor)) return FALSE;
            cursor += words;
            continue;
        }
        if (opcode == D3DSIO_NOP) {
            if (instruction != D3DSIO_NOP ||
                ++state.instruction_slots >
                    QXP_VERTEX_SHADER_MAX_INSTRUCTIONS) return FALSE;
            continue;
        }
        if (instruction != opcode ||
            !qxp_vs_opcode(opcode, &operation, &source_count, &slots) ||
            (DWORD)(end - cursor) < source_count + 1 ||
            state.instruction_slots + slots >
                QXP_VERTEX_SHADER_MAX_INSTRUCTIONS) return FALSE;
        state.instruction_slots += slots;
        destination = *cursor++;
        for (index = 0; index < source_count; ++index)
            sources[index] = *cursor++;
        for (index = 0; index < source_count; ++index)
            if (!qxp_vs_validate_source(&state, sources[index])) return FALSE;
        if (!qxp_vs_validate_destination(&state, destination)) return FALSE;
        destination_type = destination & D3DSP_REGTYPE_MASK;
        destination_mask = destination & D3DSP_WRITEMASK_ALL;
        matrix = opcode >= D3DSIO_M4x4 && opcode <= D3DSIO_M3x2;
        if (!qxp_vs_validate_read_ports(sources, source_count, matrix))
            return FALSE;
        scalar = opcode == D3DSIO_RCP || opcode == D3DSIO_RSQ ||
                 opcode == D3DSIO_EXP || opcode == D3DSIO_LOG ||
                 opcode == D3DSIO_EXPP || opcode == D3DSIO_LOGP;
        if (scalar &&
            (destination_mask != D3DSP_WRITEMASK_0 &&
             destination_mask != D3DSP_WRITEMASK_1 &&
             destination_mask != D3DSP_WRITEMASK_2 &&
             destination_mask != D3DSP_WRITEMASK_3)) return FALSE;
        if (scalar && !qxp_vs_replicated_source(sources[0])) return FALSE;
        if (opcode == D3DSIO_FRC &&
            destination_mask != D3DSP_WRITEMASK_1 &&
            destination_mask !=
                (D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1)) return FALSE;
        if (destination_type == D3DSPR_ADDR) {
            if (opcode != D3DSIO_MOV || source_count != 1 || matrix)
                return FALSE;
            if (!qxp_vs_replicated_source(sources[0])) return FALSE;
            if (!qxp_vs_emit_address_move(&output, sources[0])) return FALSE;
            state.address_initialized = TRUE;
        } else if (matrix) {
            if (!qxp_vs_emit_matrix(&output, opcode, destination, sources))
                return FALSE;
        } else if (opcode == D3DSIO_RSQ) {
            if (!qxp_vs_emit_rsq(&output, destination, sources[0]))
                return FALSE;
        } else if (opcode == D3DSIO_LOG || opcode == D3DSIO_LOGP) {
            if (!qxp_vs_emit_log(&output, destination, sources[0]))
                return FALSE;
        } else if (opcode == D3DSIO_LIT) {
            if (!qxp_vs_emit_lit(&output, destination, sources[0]))
                return FALSE;
        } else if (opcode == D3DSIO_RCP || opcode == D3DSIO_EXP ||
                   opcode == D3DSIO_EXPP) {
            if (!operation || !qxp_vs_emit_scalar_normal(
                    &output, operation, destination, sources[0]))
                return FALSE;
        } else {
            if (!operation || !qxp_vs_emit_normal(
                    &output, operation, destination, sources, NULL,
                    source_count)) return FALSE;
        }
    }
    return FALSE;
}

BOOL QxpTranslateVertexShader11(
    const DWORD *bytecode, DWORD bytecode_bytes,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    DWORD *used_inputs, DWORD *written_outputs,
    BOOL *uses_relative_addressing)
{
    return qxp_translate_vertex_shader11(
        bytecode, bytecode_bytes, source, source_capacity, source_bytes,
        used_inputs, written_outputs, uses_relative_addressing);
}
