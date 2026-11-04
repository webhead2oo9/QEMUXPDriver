#include <windef.h>

#include "qxp_vshader.h"
#include "qxp_vshader_glsl.h"

#define QXP_AG_TEMPORARY_NAME_BYTES 16

typedef struct qxp_ag_text {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_AG_TEXT;

typedef struct qxp_ag_temporaries {
    char name[QXP_VERTEX_SHADER_ARB_TEMPORARIES]
             [QXP_AG_TEMPORARY_NAME_BYTES];
    DWORD count;
} QXP_AG_TEMPORARIES;

static const char qxp_ag_header[] =
    "#version 110\n"
    "uniform vec4 qxpConstants[97];\n"
    "uniform vec4 qxpControl;\n"
    "uniform vec4 qxpProjected;\n"
    "attribute vec4 qxpV0; attribute vec4 qxpV1;\n"
    "attribute vec4 qxpV2; attribute vec4 qxpV3;\n"
    "attribute vec4 qxpV4; attribute vec4 qxpV5;\n"
    "attribute vec4 qxpV6; attribute vec4 qxpV7;\n"
    "attribute vec4 qxpV8; attribute vec4 qxpV9;\n"
    "attribute vec4 qxpV10; attribute vec4 qxpV11;\n"
    "attribute vec4 qxpV12; attribute vec4 qxpV13;\n"
    "attribute vec4 qxpV14; attribute vec4 qxpV15;\n"
    "vec4 qxpABS(vec4 a) { return abs(a); }\n"
    "vec4 qxpADD(vec4 a, vec4 b) { return a + b; }\n"
    "vec4 qxpDP3(vec4 a, vec4 b) { return vec4(dot(a.xyz, b.xyz)); }\n"
    "vec4 qxpDP4(vec4 a, vec4 b) { return vec4(dot(a, b)); }\n"
    "vec4 qxpDST(vec4 a, vec4 b) { return vec4(1.0, a.y*b.y, a.z, b.w); }\n"
    "vec4 qxpEX2(vec4 a) { return vec4(exp2(a.x)); }\n"
    "vec4 qxpEXP(vec4 a) {\n"
    "  float f = floor(a.x);\n"
    "  return vec4(exp2(f), a.x-f, exp2(a.x), 1.0);\n"
    "}\n"
    "vec4 qxpFLR(vec4 a) { return floor(a); }\n"
    "vec4 qxpFRC(vec4 a) { return fract(a); }\n"
    "vec4 qxpLG2(vec4 a) { return vec4(log2(a.x)); }\n"
    "vec4 qxpLIT(vec4 a) {\n"
    "  float x = max(a.x, 0.0);\n"
    "  float y = max(a.y, 0.0);\n"
    "  float w = clamp(a.w, -127.99609375, 127.99609375);\n"
    "  float z = 0.0;\n"
    "  if (x > 0.0) z = (y == 0.0 && w == 0.0) ? 1.0 : pow(y, w);\n"
    "  return vec4(1.0, x, z, 1.0);\n"
    "}\n"
    "vec4 qxpLOG(vec4 a) {\n"
    "  float t = abs(a.x);\n"
    "  float l = log2(t);\n"
    "  float f = floor(l);\n"
    "  return vec4(f, t/exp2(f), l, 1.0);\n"
    "}\n"
    "vec4 qxpMAD(vec4 a, vec4 b, vec4 c) { return a*b+c; }\n"
    "vec4 qxpMAX(vec4 a, vec4 b) { return max(a, b); }\n"
    "vec4 qxpMIN(vec4 a, vec4 b) { return min(a, b); }\n"
    "vec4 qxpMOV(vec4 a) { return a; }\n"
    "vec4 qxpMUL(vec4 a, vec4 b) { return a*b; }\n"
    "vec4 qxpRCP(vec4 a) { return vec4(1.0/a.x); }\n"
    "vec4 qxpRSQ(vec4 a) { return vec4(inversesqrt(abs(a.x))); }\n"
    "vec4 qxpSGE(vec4 a, vec4 b) { return step(b, a); }\n"
    "vec4 qxpSLT(vec4 a, vec4 b) { return vec4(1.0)-step(b, a); }\n"
    "vec4 qxpSUB(vec4 a, vec4 b) { return a-b; }\n"
    "void main(void) {\n"
    "  vec4 qxpFog; vec4 qxpPoint;\n"
    "  float qxpA0 = 0.0;\n";

static void qxp_ag_append_char(QXP_AG_TEXT *output, char value)
{
    if (!output || !output->valid || output->length + 1 >= output->capacity) {
        if (output) output->valid = FALSE;
        return;
    }
    output->text[output->length++] = value;
    output->text[output->length] = 0;
}

static void qxp_ag_append(QXP_AG_TEXT *output, const char *value)
{
    if (!output || !value) return;
    while (*value) qxp_ag_append_char(output, *value++);
}

static void qxp_ag_append_range(QXP_AG_TEXT *output, const char *value,
                                DWORD length)
{
    while (value && length--) qxp_ag_append_char(output, *value++);
}

static void qxp_ag_append_uint(QXP_AG_TEXT *output, DWORD value)
{
    char digits[11];
    DWORD count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < (DWORD)sizeof(digits));
    while (count) qxp_ag_append_char(output, digits[--count]);
}

static BOOL qxp_ag_prefix_in_place(QXP_AG_TEXT *output, const char *prefix)
{
    volatile char *destination;
    DWORD prefix_length = 0;
    DWORD index;

    if (!output || !output->valid || !prefix) return FALSE;
    while (prefix[prefix_length]) ++prefix_length;
    if (output->length + prefix_length + 1 > output->capacity) {
        output->valid = FALSE;
        return FALSE;
    }
    destination = (volatile char *)output->text;
    index = output->length + 1;
    while (index) {
        --index;
        destination[index + prefix_length] = destination[index];
    }
    for (index = 0; index < prefix_length; ++index)
        destination[index] = prefix[index];
    output->length += prefix_length;
    return TRUE;
}

static DWORD qxp_ag_length(const char *value)
{
    DWORD length = 0;

    if (!value) return 0;
    while (value[length]) ++length;
    return length;
}

static BOOL qxp_ag_equal(const char *left, const char *right)
{
    if (!left || !right) return FALSE;
    while (*left && *right) {
        if (*left++ != *right++) return FALSE;
    }
    return *left == *right;
}

static BOOL qxp_ag_prefix(const char *value, const char *prefix)
{
    if (!value || !prefix) return FALSE;
    while (*prefix) {
        if (*value++ != *prefix++) return FALSE;
    }
    return TRUE;
}

static char *qxp_ag_trim(char *value)
{
    char *end;

    if (!value) return NULL;
    while (*value == ' ' || *value == '\t') ++value;
    end = value + qxp_ag_length(value);
    while (end > value &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = 0;
    return value;
}

static BOOL qxp_ag_uint(const char *value, DWORD length, DWORD *result)
{
    DWORD index;
    DWORD number = 0;

    if (!value || !length || !result) return FALSE;
    for (index = 0; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9' ||
            number > (0xffffffffUL - (DWORD)(value[index] - '0')) / 10UL)
            return FALSE;
        number = number * 10UL + (DWORD)(value[index] - '0');
    }
    *result = number;
    return TRUE;
}

static BOOL qxp_ag_identifier(const char *value, DWORD length)
{
    DWORD index;

    if (!value || !length ||
        (value[0] != 'r' && value[0] != 'q')) return FALSE;
    for (index = 1; index < length; ++index)
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'z'))) return FALSE;
    return TRUE;
}

static BOOL qxp_ag_temporary_declared(
    const QXP_AG_TEMPORARIES *temporaries,
    const char *value, DWORD length)
{
    DWORD temporary;
    DWORD index;

    if (!temporaries || !value || !length) return FALSE;
    for (temporary = 0; temporary < temporaries->count; ++temporary) {
        if (qxp_ag_length(temporaries->name[temporary]) != length) continue;
        for (index = 0; index < length; ++index) {
            if (temporaries->name[temporary][index] != value[index]) break;
        }
        if (index == length) return TRUE;
    }
    return FALSE;
}

static BOOL qxp_ag_add_temporary(QXP_AG_TEMPORARIES *temporaries,
                                 const char *value, DWORD length)
{
    DWORD index;

    if (!temporaries || !qxp_ag_identifier(value, length) ||
        length >= QXP_AG_TEMPORARY_NAME_BYTES ||
        temporaries->count >= QXP_VERTEX_SHADER_ARB_TEMPORARIES ||
        qxp_ag_temporary_declared(temporaries, value, length)) return FALSE;
    for (index = 0; index < length; ++index)
        temporaries->name[temporaries->count][index] = value[index];
    temporaries->name[temporaries->count][length] = 0;
    ++temporaries->count;
    return TRUE;
}

/* Keep GLSL scratch declarations derived from the translated ARB source.
 * Duplicating the translator's TEMP list here caused valid new temporaries to
 * compile as undeclared identifiers in every clip shader. */
static BOOL qxp_ag_emit_temporary_declaration(
    QXP_AG_TEXT *output, QXP_AG_TEMPORARIES *temporaries, const char *line)
{
    const char *cursor;
    const char *identifier;
    DWORD length;
    BOOL first = TRUE;

    if (!output || !temporaries || !line ||
        !qxp_ag_prefix(line, "TEMP ")) return FALSE;
    cursor = line + sizeof("TEMP ") - 1;
    qxp_ag_append(output, "  vec4 ");
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        identifier = cursor;
        while ((*cursor >= '0' && *cursor <= '9') ||
               (*cursor >= 'a' && *cursor <= 'z')) ++cursor;
        length = (DWORD)(cursor - identifier);
        if (!qxp_ag_add_temporary(temporaries, identifier, length))
            return FALSE;
        if (!first) qxp_ag_append(output, ", ");
        qxp_ag_append_range(output, identifier, length);
        first = FALSE;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor != ';' || cursor[1]) return FALSE;
        qxp_ag_append(output, ";\n");
        return output->valid;
    }
}

static BOOL qxp_ag_source(const char *input, char *text, DWORD capacity,
                          const QXP_AG_TEMPORARIES *temporaries)
{
    QXP_AG_TEXT output;
    const char *base_end;
    const char *swizzle = NULL;
    const char *cursor;
    DWORD base_length;
    DWORD index;
    DWORD number;
    DWORD swizzle_length;
    BOOL negate = FALSE;

    if (!input || !text || !capacity) return FALSE;
    output.text = text;
    output.capacity = capacity;
    output.length = 0;
    output.valid = TRUE;
    text[0] = 0;
    while (*input == ' ' || *input == '\t') ++input;
    if (*input == '-') {
        negate = TRUE;
        ++input;
    }
    if (*input == '{') {
        base_end = input + 1;
        while (*base_end && *base_end != '}') ++base_end;
        if (*base_end != '}') return FALSE;
        swizzle = base_end + 1;
        qxp_ag_append(&output, "vec4(");
        qxp_ag_append_range(&output, input + 1,
                            (DWORD)(base_end - input - 1));
        qxp_ag_append_char(&output, ')');
    } else {
        base_end = input;
        while (*base_end && *base_end != '.') ++base_end;
        if (qxp_ag_prefix(input, "vertex.attrib[")) {
            cursor = input + 14;
            base_end = cursor;
            while (*base_end && *base_end != ']') ++base_end;
            if (*base_end != ']' ||
                !qxp_ag_uint(cursor, (DWORD)(base_end - cursor), &number) ||
                number >= 16) return FALSE;
            qxp_ag_append(&output, "qxpV");
            qxp_ag_append_uint(&output, number);
            ++base_end;
        } else if (qxp_ag_prefix(input, "c[")) {
            cursor = input + 2;
            base_end = cursor;
            while (*base_end && *base_end != ']') ++base_end;
            if (*base_end != ']') return FALSE;
            if ((DWORD)(base_end - cursor) == 4 &&
                cursor[0] == 'a' && cursor[1] == '0' &&
                cursor[2] == '.' && cursor[3] == 'x') {
                qxp_ag_append(&output, "qxpConstants[int(qxpA0)]");
            } else {
                if (!qxp_ag_uint(cursor, (DWORD)(base_end - cursor),
                                 &number) || number > 96) return FALSE;
                qxp_ag_append(&output, "qxpConstants[");
                qxp_ag_append_uint(&output, number);
                qxp_ag_append_char(&output, ']');
            }
            ++base_end;
        } else if (qxp_ag_prefix(input, "program.local[0]")) {
            base_end = input + 16;
            qxp_ag_append(&output, "qxpControl");
        } else if (qxp_ag_prefix(input, "program.local[1]")) {
            base_end = input + 16;
            qxp_ag_append(&output, "qxpProjected");
        } else {
            base_length = (DWORD)(base_end - input);
            if (!qxp_ag_identifier(input, base_length) ||
                !qxp_ag_temporary_declared(
                    temporaries, input, base_length)) return FALSE;
            qxp_ag_append_range(&output, input, base_length);
        }
        swizzle = base_end;
    }
    if (swizzle && *swizzle) {
        if (*swizzle != '.') return FALSE;
        ++swizzle;
        swizzle_length = qxp_ag_length(swizzle);
        if (swizzle_length != 1 && swizzle_length != 4) return FALSE;
        for (index = 0; index < swizzle_length; ++index) {
            if (swizzle[index] != 'x' && swizzle[index] != 'y' &&
                swizzle[index] != 'z' && swizzle[index] != 'w') return FALSE;
        }
        if (swizzle_length == 1) {
            if (!qxp_ag_prefix_in_place(&output, "vec4((")) return FALSE;
            qxp_ag_append(&output, ").");
            qxp_ag_append_char(&output, swizzle[0]);
            qxp_ag_append_char(&output, ')');
        } else {
            qxp_ag_append_char(&output, '.');
            qxp_ag_append(&output, swizzle);
        }
    }
    if (negate) {
        if (!qxp_ag_prefix_in_place(&output, "-(")) return FALSE;
        qxp_ag_append_char(&output, ')');
    }
    return output.valid;
}

static BOOL qxp_ag_destination(
    const char *input, char *name, DWORD name_capacity,
    char *mask, DWORD mask_capacity,
    const QXP_AG_TEMPORARIES *temporaries)
{
    const char *suffix = NULL;
    const char *end;
    QXP_AG_TEXT name_output;
    DWORD index;
    DWORD number;

    if (!input || !name || !name_capacity || !mask || !mask_capacity)
        return FALSE;
    name_output.text = name;
    name_output.capacity = name_capacity;
    name_output.length = 0;
    name_output.valid = TRUE;
    name[0] = 0;
    mask[0] = 0;
    if (qxp_ag_prefix(input, "result.position")) {
        qxp_ag_append(&name_output, "gl_Position");
        suffix = input + 15;
    } else if (qxp_ag_prefix(input, "result.color.primary")) {
        qxp_ag_append(&name_output, "gl_FrontColor");
        suffix = input + 20;
    } else if (qxp_ag_prefix(input, "result.color.secondary")) {
        qxp_ag_append(&name_output, "gl_FrontSecondaryColor");
        suffix = input + 22;
    } else if (qxp_ag_prefix(input, "result.fogcoord")) {
        qxp_ag_append(&name_output, "qxpFog");
        suffix = input + 15;
    } else if (qxp_ag_prefix(input, "result.pointsize")) {
        qxp_ag_append(&name_output, "qxpPoint");
        suffix = input + 16;
    } else if (qxp_ag_prefix(input, "result.texcoord[")) {
        end = input + 16;
        suffix = end;
        while (*suffix && *suffix != ']') ++suffix;
        if (*suffix != ']' ||
            !qxp_ag_uint(end, (DWORD)(suffix - end), &number) || number >= 8)
            return FALSE;
        qxp_ag_append(&name_output, "gl_TexCoord[");
        qxp_ag_append_uint(&name_output, number);
        qxp_ag_append_char(&name_output, ']');
        ++suffix;
    } else {
        end = input;
        while (*end && *end != '.') ++end;
        if (!qxp_ag_identifier(input, (DWORD)(end - input)) ||
            !qxp_ag_temporary_declared(
                temporaries, input, (DWORD)(end - input))) return FALSE;
        qxp_ag_append_range(&name_output, input, (DWORD)(end - input));
        suffix = end;
    }
    if (!name_output.valid || !suffix) return FALSE;
    if (!*suffix) return TRUE;
    if (*suffix++ != '.') return FALSE;
    index = 0;
    while (*suffix) {
        if (index + 1 >= mask_capacity ||
            (*suffix != 'x' && *suffix != 'y' &&
             *suffix != 'z' && *suffix != 'w')) return FALSE;
        mask[index++] = *suffix++;
    }
    mask[index] = 0;
    return index != 0;
}

static DWORD qxp_ag_operands(char *text, char **operands, DWORD capacity)
{
    DWORD count = 0;
    DWORD depth = 0;
    char *cursor;

    if (!text || !operands || !capacity) return 0;
    operands[count++] = qxp_ag_trim(text);
    for (cursor = text; *cursor; ++cursor) {
        if (*cursor == '{' || *cursor == '[') ++depth;
        else if (*cursor == '}' || *cursor == ']') {
            if (!depth) return 0;
            --depth;
        } else if (*cursor == ',' && !depth) {
            if (count >= capacity) return 0;
            *cursor = 0;
            operands[count - 1] = qxp_ag_trim(operands[count - 1]);
            operands[count++] = qxp_ag_trim(cursor + 1);
        }
    }
    operands[count - 1] = qxp_ag_trim(operands[count - 1]);
    return depth ? 0 : count;
}

static DWORD qxp_ag_source_count(const char *opcode)
{
    if (qxp_ag_equal(opcode, "MOV") || qxp_ag_equal(opcode, "ABS") ||
        qxp_ag_equal(opcode, "RCP") || qxp_ag_equal(opcode, "RSQ") ||
        qxp_ag_equal(opcode, "EX2") || qxp_ag_equal(opcode, "EXP") ||
        qxp_ag_equal(opcode, "FLR") || qxp_ag_equal(opcode, "FRC") ||
        qxp_ag_equal(opcode, "LG2") || qxp_ag_equal(opcode, "LIT") ||
        qxp_ag_equal(opcode, "LOG")) return 1;
    if (qxp_ag_equal(opcode, "ADD") || qxp_ag_equal(opcode, "DP3") ||
        qxp_ag_equal(opcode, "DP4") || qxp_ag_equal(opcode, "DST") ||
        qxp_ag_equal(opcode, "MAX") || qxp_ag_equal(opcode, "MIN") ||
        qxp_ag_equal(opcode, "MUL") || qxp_ag_equal(opcode, "SGE") ||
        qxp_ag_equal(opcode, "SLT") || qxp_ag_equal(opcode, "SUB"))
        return 2;
    if (qxp_ag_equal(opcode, "MAD")) return 3;
    return 0;
}

static BOOL qxp_ag_emit_instruction(
    QXP_AG_TEXT *output, char *line, BOOL *clip_position,
    const QXP_AG_TEMPORARIES *temporaries)
{
    char *operands[4];
    char *arguments;
    char *opcode;
    char destination[64];
    char mask[8];
    char sources[3][512];
    DWORD count;
    DWORD expected;
    DWORD index;
    BOOL capture_clip;

    opcode = qxp_ag_trim(line);
    arguments = opcode;
    while (*arguments && *arguments != ' ' && *arguments != '\t')
        ++arguments;
    if (!*arguments) return FALSE;
    *arguments++ = 0;
    arguments = qxp_ag_trim(arguments);
    count = qxp_ag_operands(arguments, operands, 4);
    if (qxp_ag_equal(opcode, "ARL")) {
        if (count != 2 || !qxp_ag_equal(operands[0], "a0.x") ||
            !qxp_ag_source(operands[1], sources[0], sizeof(sources[0]),
                           temporaries)) return FALSE;
        qxp_ag_append(output, "  qxpA0 = floor((");
        qxp_ag_append(output, sources[0]);
        qxp_ag_append(output, ").x);\n");
        return output->valid;
    }
    expected = qxp_ag_source_count(opcode);
    if (!expected || count != expected + 1 ||
        !qxp_ag_destination(operands[0], destination,
                            sizeof(destination), mask, sizeof(mask),
                            temporaries)) return FALSE;
    for (index = 0; index < expected; ++index) {
        if (!qxp_ag_source(operands[index + 1], sources[index],
                           sizeof(sources[index]), temporaries)) return FALSE;
    }
    capture_clip = qxp_ag_equal(operands[0], "result.position.w");
    if (capture_clip &&
        !qxp_ag_temporary_declared(temporaries, "qp", 2)) return FALSE;
    qxp_ag_append(output, "  ");
    qxp_ag_append(output, destination);
    if (*mask) {
        qxp_ag_append_char(output, '.');
        qxp_ag_append(output, mask);
    }
    qxp_ag_append(output, " = qxp");
    qxp_ag_append(output, opcode);
    qxp_ag_append_char(output, '(');
    for (index = 0; index < expected; ++index) {
        if (index) qxp_ag_append(output, ", ");
        qxp_ag_append(output, sources[index]);
    }
    qxp_ag_append_char(output, ')');
    if (*mask) {
        qxp_ag_append_char(output, '.');
        qxp_ag_append(output, mask);
    }
    qxp_ag_append(output, ";\n");
    if (capture_clip) {
        qxp_ag_append(output, "  gl_ClipVertex = qp;\n");
        *clip_position = TRUE;
    }
    return output->valid;
}

BOOL QxpConvertVertexShaderArbToGlsl(
    const char *arb_source, DWORD arb_source_bytes,
    char *glsl_source, DWORD glsl_capacity, DWORD *glsl_source_bytes)
{
    QXP_AG_TEXT output;
    QXP_AG_TEMPORARIES temporaries;
    char line[512];
    DWORD cursor = 0;
    DWORD length;
    DWORD line_number = 0;
    char *trimmed;
    BOOL ended = FALSE;
    BOOL clip_position = FALSE;

    if (!arb_source || !arb_source_bytes || !glsl_source ||
        !glsl_capacity || !glsl_source_bytes) return FALSE;
    *glsl_source_bytes = 0;
    output.text = glsl_source;
    output.capacity = glsl_capacity;
    output.length = 0;
    output.valid = TRUE;
    temporaries.count = 0;
    glsl_source[0] = 0;
    qxp_ag_append(&output, qxp_ag_header);
    while (cursor < arb_source_bytes) {
        length = 0;
        while (cursor + length < arb_source_bytes &&
               arb_source[cursor + length] != '\n') {
            if (length + 1 >= (DWORD)sizeof(line)) return FALSE;
            line[length] = arb_source[cursor + length];
            ++length;
        }
        line[length] = 0;
        cursor += length;
        if (cursor < arb_source_bytes && arb_source[cursor] == '\n') ++cursor;
        ++line_number;
        trimmed = qxp_ag_trim(line);
        if (!*trimmed) continue;
        if (line_number == 1) {
            if (!qxp_ag_equal(trimmed, "!!ARBvp1.0")) return FALSE;
            continue;
        }
        if (qxp_ag_prefix(trimmed, "PARAM c[97] = ") ||
            qxp_ag_equal(trimmed, "ADDRESS a0;")) continue;
        if (qxp_ag_prefix(trimmed, "TEMP ")) {
            if (!qxp_ag_emit_temporary_declaration(
                    &output, &temporaries, trimmed)) return FALSE;
            continue;
        }
        if (qxp_ag_equal(trimmed, "END")) {
            if (!clip_position) return FALSE;
            qxp_ag_append(&output,
                "  gl_FogFragCoord = qxpFog.x;\n"
                "  gl_PointSize = qxpPoint.x;\n"
                "}\n");
            ended = TRUE;
            break;
        }
        length = qxp_ag_length(trimmed);
        if (!length || trimmed[length - 1] != ';') return FALSE;
        trimmed[length - 1] = 0;
        if (!qxp_ag_emit_instruction(
                &output, trimmed, &clip_position, &temporaries)) return FALSE;
    }
    if (!ended || cursor != arb_source_bytes || !output.valid) return FALSE;
    *glsl_source_bytes = output.length;
    return TRUE;
}
