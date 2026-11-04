#include <stdio.h>
#include <string.h>

#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_vshader.h"
#include "qxp_vshader_glsl.h"

#ifndef D3DSP_REGNUM_MASK
#define D3DSP_REGNUM_MASK 0x00001fffUL
#endif

#define QXP_PARAMETER_TOKEN 0x80000000UL
#define QXP_DEST(type, number, mask) \
    (QXP_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | (DWORD)(mask))
#define QXP_SOURCE(type, number) \
    (QXP_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     D3DSP_NOSWIZZLE)
#define QXP_REPLICATE(component) \
    (((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 0)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 2)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 4)) | \
     ((DWORD)(component) << (D3DSP_SWIZZLE_SHIFT + 6)))
#define QXP_SCALAR_SOURCE(type, number, component) \
    (QXP_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     QXP_REPLICATE(component))

typedef struct qxp_vshader_result {
    char source[QXP_VERTEX_SHADER_SOURCE_MAX];
    DWORD source_bytes;
    DWORD used_inputs;
    DWORD written_outputs;
    BOOL uses_relative;
} QXP_VSHADER_RESULT;

static int qxp_failures;
static char qxp_glsl_source[QXP_VERTEX_SHADER_GLSL_MAX];

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_vshader: failed: %s\n", name);
}

static BOOL qxp_translate_capacity(const DWORD *tokens, DWORD count,
                                   QXP_VSHADER_RESULT *result,
                                   DWORD capacity)
{
    memset(result, 0, sizeof(*result));
    return QxpTranslateVertexShader11(
        tokens, count * sizeof(tokens[0]), result->source, capacity,
        &result->source_bytes, &result->used_inputs,
        &result->written_outputs, &result->uses_relative);
}

static BOOL qxp_translate(const DWORD *tokens, DWORD count,
                          QXP_VSHADER_RESULT *result)
{
    return qxp_translate_capacity(tokens, count, result,
                                  sizeof(result->source));
}

static void qxp_test_glsl_conversion(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;
    DWORD glsl_bytes = 0;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result) &&
              QxpConvertVertexShaderArbToGlsl(
                  result.source, result.source_bytes, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes),
              "clip GLSL conversion");
    qxp_check(glsl_bytes &&
              strstr(qxp_glsl_source,
                     "uniform vec4 qxpConstants[97];") != NULL &&
              strstr(qxp_glsl_source,
                     "attribute vec4 qxpV15;") != NULL &&
              strstr(qxp_glsl_source,
                     "vec4 qt0, qt1, qt2, qt3;") != NULL &&
              strstr(qxp_glsl_source,
                     "gl_ClipVertex = qp;") != NULL &&
              strstr(qxp_glsl_source,
                     "gl_Position.z = qxpMAD(") != NULL,
              "clip GLSL contract emitted");
    qxp_check(!QxpConvertVertexShaderArbToGlsl(
                  "!!ARBvp1.0\nMOV r0, r1;\nEND\n", 32,
                  qxp_glsl_source, sizeof(qxp_glsl_source), &glsl_bytes),
              "clip GLSL rejects missing position epilogue");
}

static void qxp_test_glsl_temp_contract(void)
{
    static const char declared[] =
        "!!ARBvp1.0\n"
        "TEMP r0, qt0, qp;\n"
        "MOV qt0, {1.0, 1.0, 1.0, 1.0};\n"
        "MOV r0, qt0;\n"
        "MOV qp, r0;\n"
        "MOV result.position.w, qp;\n"
        "END\n";
    static const char undeclared[] =
        "!!ARBvp1.0\n"
        "TEMP r0, qp;\n"
        "MOV r0, qt0;\n"
        "MOV result.position.w, r0;\n"
        "END\n";
    static const char missing_qp[] =
        "!!ARBvp1.0\n"
        "TEMP r0;\n"
        "MOV r0, {1.0, 1.0, 1.0, 1.0};\n"
        "MOV result.position.w, r0;\n"
        "END\n";
    static const char duplicate[] =
        "!!ARBvp1.0\n"
        "TEMP r0, qp;\n"
        "TEMP r0;\n"
        "MOV result.position.w, r0;\n"
        "END\n";
    static const char overflow[] =
        "!!ARBvp1.0\n"
        "TEMP r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, "
        "q0, q1, q2, q3, q4, q5, q6, q7, q8, q9, q10, q11, q12;\n"
        "MOV result.position.w, r0;\n"
        "END\n";
    DWORD glsl_bytes = 0;

    qxp_check(QxpConvertVertexShaderArbToGlsl(
                  declared, sizeof(declared) - 1, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes) &&
              strstr(qxp_glsl_source, "vec4 r0, qt0, qp;") != NULL &&
              strstr(qxp_glsl_source, "r0 = qxpMOV(qt0);") != NULL &&
              strstr(qxp_glsl_source, "qp = qxpMOV(r0);") != NULL,
              "declared GLSL temporaries resolve");
    qxp_check(!QxpConvertVertexShaderArbToGlsl(
                  undeclared, sizeof(undeclared) - 1, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes),
              "undeclared GLSL temporary rejected");
    qxp_check(!QxpConvertVertexShaderArbToGlsl(
                  missing_qp, sizeof(missing_qp) - 1, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes),
              "injected clip temporary must be declared");
    qxp_check(!QxpConvertVertexShaderArbToGlsl(
                  duplicate, sizeof(duplicate) - 1, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes),
              "duplicate GLSL temporary rejected");
    qxp_check(!QxpConvertVertexShaderArbToGlsl(
                  overflow, sizeof(overflow) - 1, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes),
              "GLSL temporary budget enforced");
}

static void qxp_test_arithmetic(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_ADD, QXP_DEST(D3DSPR_TEMP, 1, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_SUB, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_CONST, 1) | D3DSPSM_NEG,
        D3DSIO_MAD, QXP_DEST(D3DSPR_TEMP, 3, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 2),
        D3DSIO_MUL, QXP_DEST(D3DSPR_TEMP, 4, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_CONST, 2),
        D3DSIO_DP3, QXP_DEST(D3DSPR_TEMP, 5, D3DSP_WRITEMASK_0),
            QXP_SOURCE(D3DSPR_TEMP, 3), QXP_SOURCE(D3DSPR_TEMP, 4),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 5, D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_TEMP, 3), QXP_SOURCE(D3DSPR_TEMP, 4),
        D3DSIO_MIN, QXP_DEST(D3DSPR_TEMP, 6, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 3), QXP_SOURCE(D3DSPR_TEMP, 4),
        D3DSIO_MAX, QXP_DEST(D3DSPR_TEMP, 7, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 3), QXP_SOURCE(D3DSPR_TEMP, 4),
        D3DSIO_SLT, QXP_DEST(D3DSPR_TEMP, 8, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 6), QXP_SOURCE(D3DSPR_TEMP, 7),
        D3DSIO_SGE, QXP_DEST(D3DSPR_TEMP, 9, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 7), QXP_SOURCE(D3DSPR_TEMP, 6),
        D3DSIO_DST, QXP_DEST(D3DSPR_TEMP, 10, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 6), QXP_SOURCE(D3DSPR_TEMP, 7),
        D3DSIO_LIT, QXP_DEST(D3DSPR_TEMP, 11, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 10),
        D3DSIO_FRC, QXP_DEST(D3DSPR_TEMP, 11,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_TEMP, 10),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "core arithmetic translates");
    qxp_check(result.used_inputs == 1 && !result.uses_relative,
              "core input metadata");
    qxp_check(result.written_outputs == QXP_VS_OUTPUT_POSITION,
              "core output metadata");
    qxp_check(strstr(result.source, "MOV r0, vertex.attrib[0];") != NULL,
              "input move emitted");
    qxp_check(strstr(result.source, "SUB r2, r1, -c[1];") != NULL,
              "negated source emitted");
    qxp_check(strstr(result.source, "FRC r11.xy, r10;") != NULL,
              "frc emitted");
    qxp_check(strstr(result.source,
                     "MIN qd.w, qd.w, {127.99609375};") != NULL &&
              strstr(result.source,
                     "MAX qd.w, qd.w, {-127.99609375};") != NULL,
              "D3D lit exponent clamp emitted");
    qxp_check(strstr(result.source,
                     "MUL result.position.y, qp.y, program.local[0].x;") !=
                  NULL &&
              strstr(result.source,
                     "MAD result.position.z, qp.z, {2.0}, -qp.w;") != NULL &&
              strstr(result.source, "RCP qp.x, qp.w;") != NULL &&
              strstr(result.source, "MUL qp.x, qp.z, qp.x;") != NULL,
              "target orientation and D3D clip/fog depth conversion emitted");
}

static void qxp_test_scalar_math(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_RCP, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 0, 1),
        D3DSIO_RSQ, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_1),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 0, 2) | D3DSPSM_NEG,
        D3DSIO_EXP, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_2),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 0, 0),
        D3DSIO_EXPP, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_3),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 0, 3),
        D3DSIO_LOG, QXP_DEST(D3DSPR_TEMP, 1, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 1, 0),
        D3DSIO_LOGP, QXP_DEST(D3DSPR_TEMP, 1, D3DSP_WRITEMASK_1),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "scalar math translates");
    qxp_check(result.used_inputs == 7, "scalar input metadata");
    qxp_check(strstr(result.source, "RCP r0.x, vertex.attrib[0].y;") !=
                  NULL,
              "rcp replicate emitted");
    qxp_check(strstr(result.source,
                     "ABS qd.x, -vertex.attrib[0].zzzz;") != NULL &&
              strstr(result.source, "RSQ r0.y, qd.x;") != NULL,
              "absolute rsq emitted");
    qxp_check(strstr(result.source, "EX2 r0.z") != NULL &&
              strstr(result.source, "EXP r0.w") != NULL,
              "full and partial exponentials emitted");
    qxp_check(strstr(result.source,
                     "MAD qd.z, qd.y, {-3.402823466e+38}, qd.z;") != NULL,
              "zero-safe logarithm emitted");
}

static void qxp_test_matrices(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_M4x4, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_M4x3, QXP_DEST(D3DSPR_TEMP, 1,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 0) | D3DSPSM_NEG,
            QXP_SOURCE(D3DSPR_CONST, 4),
        D3DSIO_M3x4, QXP_DEST(D3DSPR_TEMP, 2,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 1), QXP_SOURCE(D3DSPR_CONST, 7),
        D3DSIO_M3x3, QXP_DEST(D3DSPR_TEMP, 3,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 1), QXP_SOURCE(D3DSPR_CONST, 11),
        D3DSIO_M3x2, QXP_DEST(D3DSPR_TEMP, 4,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 1), QXP_SOURCE(D3DSPR_CONST, 14),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "matrix family translates");
    qxp_check(strstr(result.source,
                     "DP4 r0.w, vertex.attrib[0], c[3];") != NULL,
              "m4x4 fourth row emitted");
    qxp_check(strstr(result.source,
                     "DP4 r1.z, -vertex.attrib[0], c[6];") != NULL,
              "m4x3 source modifier emitted");
    qxp_check(strstr(result.source,
                     "DP3 r2.z, vertex.attrib[1], c[9];") != NULL &&
              strstr(result.source, "DP3 r2.w") == NULL,
              "m3x4 xyz mask honored");
    qxp_check(strstr(result.source,
                     "DP3 r4.y, vertex.attrib[1], c[15];") != NULL,
              "m3x2 emitted");
}

static void qxp_test_relative_addressing(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ADDR, 0, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 1, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_CONST, 2) | D3DVS_ADDRMODE_RELATIVE,
        D3DSIO_M4x4, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
            QXP_SOURCE(D3DSPR_CONST, 90) | D3DVS_ADDRMODE_RELATIVE,
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "relative addressing translates");
    qxp_check(result.uses_relative && result.used_inputs == 3,
              "relative metadata");
    qxp_check(strstr(result.source,
                     "ADD qa.x, vertex.attrib[1].xxxx, {0.5};") != NULL &&
              strstr(result.source, "FLR qa.x, qa.x;") != NULL,
              "round-to-nearest address emitted");
    qxp_check(strstr(result.source, "ADD q0.x, qa.x, {2.0};") != NULL &&
              strstr(result.source, "MIN q0.y, q0.y, {95.0};") != NULL &&
              strstr(result.source, "SLT q0.w, q0.x, {96.0};") != NULL &&
              strstr(result.source,
                     "MAD q0.y, q0.z, {-96.0}, q0.y;") != NULL &&
              strstr(result.source, "MOV q0, c[a0.x];") != NULL,
              "guarded relative read emitted");
    qxp_check(strstr(result.source, "ADD q1.x, qa.x, {93.0};") != NULL,
              "relative matrix row offset emitted");
}

static void qxp_test_outputs(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 1,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 0,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 0, D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 2),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ATTROUT, 1, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 3),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_FOG,
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 4),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE,
                             D3DSP_WRITEMASK_3),
            QXP_SOURCE(D3DSPR_INPUT, 5),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 6),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;
    DWORD expected = QXP_VS_OUTPUT_POSITION | QXP_VS_OUTPUT_FOG |
                     QXP_VS_OUTPUT_POINT_SIZE | QXP_VS_OUTPUT_COLOR0 |
                     QXP_VS_OUTPUT_COLOR1 | QXP_VS_OUTPUT_TEXCOORD0 |
                     (QXP_VS_OUTPUT_TEXCOORD0 << 1);

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "output contract translates");
    qxp_check(result.written_outputs == expected, "output metadata");
    qxp_check(strstr(result.source, "MOV qf.z, vertex.attrib[4];") != NULL &&
              strstr(result.source,
                     "MAX qf.x, qf.z, {0.0}.x;") != NULL &&
              strstr(result.source,
                     "MIN qf.x, qf.x, {1.0}.x;") != NULL &&
              strstr(result.source, "SUB qf.x, {1.0}.x, qf.x;") != NULL &&
              strstr(result.source,
                     "SUB qf.y, {1.0}.x, program.local[0].y;") != NULL &&
              strstr(result.source,
                     "MAD result.fogcoord.x, qf.x, qf.y, qp.x;") != NULL,
              "saturated vertex/table fog selector staged");
    qxp_check(strstr(result.source, "MOV qs.w, vertex.attrib[5];") != NULL &&
              strstr(result.source, "MOV result.pointsize.x, qs.w;") != NULL,
              "point-size component staged");
}

/* Prince of Persia writes the scalar fog/point-size outputs with a full
 * .xyzw mask (e.g. "mul oFog, r6.w, c20.z").  This is legal vs_1_1 that the
 * D3DX assembler emits; the translator must accept the broadcast and consume
 * the first lane, rather than demanding a single write component. */
static void qxp_test_full_mask_scalar_outputs(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MUL, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_FOG,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_CONST, 20),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "full-mask oFog/oPts translate (POP pattern)");
    qxp_check((result.written_outputs & QXP_VS_OUTPUT_FOG) &&
              (result.written_outputs & QXP_VS_OUTPUT_POINT_SIZE),
              "full-mask scalar outputs recorded");
    qxp_check(strstr(result.source,
                     "MAX qf.x, qf.x, {0.0}.x;") != NULL &&
              strstr(result.source,
                     "MIN qf.x, qf.x, {1.0}.x;") != NULL &&
              strstr(result.source, "SUB qf.x, {1.0}.x, qf.x;") != NULL,
              "full-mask fog consumes and saturates first lane");
    qxp_check(strstr(result.source,
                     "MOV result.pointsize.x, qs.x;") != NULL,
              "full-mask point size consumes first lane");
}

/* Prince of Persia writes clip-space position with four per-component DP4s
 * (oPos.x/y/z/w = dot(r0, c0..c3)) rather than one full-mask instruction.
 * This is standard vs_1_1; the translator must accumulate the lanes and
 * accept the shader once every component is written. */
static void qxp_test_split_position(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_DP4, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_0),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_DP4, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 1),
        D3DSIO_DP4, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 2),
        D3DSIO_DP4, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_3),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 3),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "per-component DP4 position translates (POP projection)");
    qxp_check(result.written_outputs == QXP_VS_OUTPUT_POSITION,
              "split position output metadata");
    qxp_check(strstr(result.source,
                     "DP4 qp.x, vertex.attrib[0], c[0];") != NULL &&
              strstr(result.source,
                     "DP4 qp.w, vertex.attrib[0], c[3];") != NULL,
              "split position lanes emitted");
}

static void qxp_test_comments_and_budget(void)
{
    DWORD exact[132];
    DWORD overflow[133];
    DWORD index;
    static const DWORD comment_shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSHADER_COMMENT(2), 0x12345678UL, 0x9abcdef0UL,
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

    qxp_check(qxp_translate(comment_shader,
                            sizeof(comment_shader) /
                                sizeof(comment_shader[0]), &result),
              "bounded comment translates");
    exact[0] = D3DVS_VERSION(1, 1);
    for (index = 0; index < 127; ++index) exact[1 + index] = D3DSIO_NOP;
    exact[128] = D3DSIO_MOV;
    exact[129] = QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                          D3DSP_WRITEMASK_ALL);
    exact[130] = QXP_SOURCE(D3DSPR_INPUT, 0);
    exact[131] = D3DSIO_END;
    qxp_check(qxp_translate(exact, 132, &result),
              "128 weighted slots accepted");
    overflow[0] = D3DVS_VERSION(1, 1);
    for (index = 0; index < 128; ++index)
        overflow[1 + index] = D3DSIO_NOP;
    overflow[129] = D3DSIO_MOV;
    overflow[130] = QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL);
    overflow[131] = QXP_SOURCE(D3DSPR_INPUT, 0);
    overflow[132] = D3DSIO_END;
    qxp_check(!qxp_translate(overflow, 133, &result),
              "129 weighted slots rejected");
}

static void qxp_test_worst_valid_expansion(void)
{
    DWORD shader[646];
    DWORD cursor = 0;
    DWORD index;
    DWORD relative = QXP_SOURCE(D3DSPR_CONST, 0) |
                     D3DVS_ADDRMODE_RELATIVE;
    QXP_VSHADER_RESULT result;
    DWORD glsl_bytes = 0;

    shader[cursor++] = D3DVS_VERSION(1, 1);
    shader[cursor++] = D3DSIO_MOV;
    shader[cursor++] = QXP_DEST(D3DSPR_ADDR, 0, D3DSP_WRITEMASK_0);
    shader[cursor++] = QXP_SCALAR_SOURCE(D3DSPR_INPUT, 1, 0);
    for (index = 0; index < 126; ++index) {
        shader[cursor++] = D3DSIO_MAD;
        shader[cursor++] = QXP_DEST(D3DSPR_TEMP, 0,
                                    D3DSP_WRITEMASK_ALL);
        shader[cursor++] = relative;
        shader[cursor++] = relative;
        shader[cursor++] = relative;
    }
    shader[cursor++] = D3DSIO_MAD;
    shader[cursor++] = QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                                D3DSP_WRITEMASK_ALL);
    shader[cursor++] = relative;
    shader[cursor++] = relative;
    shader[cursor++] = relative;
    shader[cursor++] = D3DSIO_END;
    qxp_check(cursor <= sizeof(shader) / sizeof(shader[0]),
              "worst expansion token buffer bounded");
    qxp_check(qxp_translate(shader, cursor, &result),
              "worst valid relative expansion translates");
    qxp_check(result.source_bytes < 60UL * 1024UL,
              "duplicate relative reads keep worst expansion bounded");
    qxp_check(QxpConvertVertexShaderArbToGlsl(
                  result.source, result.source_bytes, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes) &&
              glsl_bytes < QXP_VERTEX_SHADER_GLSL_MAX,
              "worst valid clip GLSL expansion bounded");
}

static void qxp_test_rejections(void)
{
    static const DWORD missing_end[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0)
    };
    static const DWORD wrong_version[] = {
        D3DVS_VERSION(2, 0), D3DSIO_END
    };
    static const DWORD truncated[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MAD,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0)
    };
    static const DWORD unmarked_destination[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                 D3DSP_WRITEMASK_ALL) & ~QXP_PARAMETER_TOKEN,
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD unmarked_source[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                 D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0) & ~QXP_PARAMETER_TOKEN,
        D3DSIO_END
    };
    static const DWORD relative_before_address[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_CONST, 0) | D3DVS_ADDRMODE_RELATIVE,
        D3DSIO_END
    };
    static const DWORD partial_position[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                 D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 |
                 D3DSP_WRITEMASK_2),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD missing_texcoord[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_TEXCRDOUT, 1, D3DSP_WRITEMASK_0),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD bad_texcoord_mask[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_TEXCRDOUT, 0,
                 D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_2),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD multiple_fog_components[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_FOG, D3DSP_WRITEMASK_0),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_FOG, D3DSP_WRITEMASK_1),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD bad_scalar_swizzle[] = {
        D3DVS_VERSION(1, 1), D3DSIO_RCP,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD bad_frc_mask[] = {
        D3DVS_VERSION(1, 1), D3DSIO_FRC,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD aliased_matrix[] = {
        D3DVS_VERSION(1, 1), D3DSIO_M4x4,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD matrix_constant_overflow[] = {
        D3DVS_VERSION(1, 1), D3DSIO_M4x4,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 94),
        D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD bad_modifier[] = {
        D3DVS_VERSION(1, 1), D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0) | D3DSPSM_BIAS, D3DSIO_END
    };
    static const DWORD multiple_input_ports[] = {
        D3DVS_VERSION(1, 1), D3DSIO_ADD,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD multiple_constant_ports[] = {
        D3DVS_VERSION(1, 1), D3DSIO_ADD,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_CONST, 0), QXP_SOURCE(D3DSPR_CONST, 1),
        D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    static const DWORD matrix_constant_vector[] = {
        D3DVS_VERSION(1, 1), D3DSIO_M4x4,
        QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_CONST, 4), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_MOV,
        QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION, D3DSP_WRITEMASK_ALL),
        QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
    };
    QXP_VSHADER_RESULT result;

#define QXP_REJECT(shader, name) \
    qxp_check(!qxp_translate((shader), \
        sizeof(shader) / sizeof((shader)[0]), &result), (name))
    QXP_REJECT(missing_end, "missing end rejected");
    QXP_REJECT(wrong_version, "wrong version rejected");
    QXP_REJECT(truncated, "truncated instruction rejected");
    QXP_REJECT(unmarked_destination,
               "unmarked destination parameter rejected");
    QXP_REJECT(unmarked_source, "unmarked source parameter rejected");
    QXP_REJECT(relative_before_address,
               "relative source before address rejected");
    QXP_REJECT(partial_position, "partial position rejected");
    QXP_REJECT(missing_texcoord, "noncontiguous texcoord rejected");
    QXP_REJECT(bad_texcoord_mask, "bad combined texcoord mask rejected");
    QXP_REJECT(multiple_fog_components,
               "multiple fog components rejected");
    QXP_REJECT(bad_scalar_swizzle, "nonreplicated scalar rejected");
    QXP_REJECT(bad_frc_mask, "bad frc mask rejected");
    QXP_REJECT(aliased_matrix, "aliased matrix destination rejected");
    QXP_REJECT(matrix_constant_overflow,
               "absolute matrix constant overflow rejected");
    QXP_REJECT(bad_modifier, "unsupported source modifier rejected");
    QXP_REJECT(multiple_input_ports,
               "multiple input read ports rejected");
    QXP_REJECT(multiple_constant_ports,
               "multiple constant read ports rejected");
    QXP_REJECT(matrix_constant_vector,
               "matrix constant vector source rejected");
#undef QXP_REJECT

    qxp_check(!qxp_translate_capacity(
                  (const DWORD[]){
                      D3DVS_VERSION(1, 1), D3DSIO_MOV,
                      QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                               D3DSP_WRITEMASK_ALL),
                      QXP_SOURCE(D3DSPR_INPUT, 0), D3DSIO_END
                  }, 5, &result, 32),
              "small output buffer rejected");
}

static int qxp_emit_vshader_smoke(const char *path, int glsl)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ADDR, 0, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 1, 0),
        D3DSIO_M4x4, QXP_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
            QXP_SOURCE(D3DSPR_CONST, 0) | D3DVS_ADDRMODE_RELATIVE,
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 2),
        D3DSIO_ADD, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_CONST, 10),
        D3DSIO_SUB, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_CONST, 10),
        D3DSIO_MAD, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 2),
            QXP_SOURCE(D3DSPR_CONST, 11),
        D3DSIO_MUL, QXP_DEST(D3DSPR_TEMP, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_CONST, 12),
        D3DSIO_RCP, QXP_DEST(D3DSPR_TEMP, 3, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_TEMP, 2, 0),
        D3DSIO_RSQ, QXP_DEST(D3DSPR_TEMP, 3, D3DSP_WRITEMASK_1),
            QXP_SCALAR_SOURCE(D3DSPR_TEMP, 2, 1),
        D3DSIO_EXP, QXP_DEST(D3DSPR_TEMP, 3, D3DSP_WRITEMASK_2),
            QXP_SCALAR_SOURCE(D3DSPR_TEMP, 2, 2),
        D3DSIO_EXPP, QXP_DEST(D3DSPR_TEMP, 3, D3DSP_WRITEMASK_3),
            QXP_SCALAR_SOURCE(D3DSPR_TEMP, 2, 3),
        D3DSIO_LOG, QXP_DEST(D3DSPR_TEMP, 1, D3DSP_WRITEMASK_0),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 2, 0),
        D3DSIO_LOGP, QXP_DEST(D3DSPR_TEMP, 1, D3DSP_WRITEMASK_1),
            QXP_SCALAR_SOURCE(D3DSPR_INPUT, 2, 1),
        D3DSIO_DP3, QXP_DEST(D3DSPR_TEMP, 4, D3DSP_WRITEMASK_0),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 3),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 4, D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 3),
        D3DSIO_MIN, QXP_DEST(D3DSPR_TEMP, 5, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 3),
        D3DSIO_MAX, QXP_DEST(D3DSPR_TEMP, 6, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 3),
        D3DSIO_SLT, QXP_DEST(D3DSPR_TEMP, 7, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 5), QXP_SOURCE(D3DSPR_TEMP, 6),
        D3DSIO_SGE, QXP_DEST(D3DSPR_TEMP, 7, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 6), QXP_SOURCE(D3DSPR_TEMP, 5),
        D3DSIO_DST, QXP_DEST(D3DSPR_TEMP, 8, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_TEMP, 3),
        D3DSIO_LIT, QXP_DEST(D3DSPR_TEMP, 9, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 8),
        D3DSIO_FRC, QXP_DEST(D3DSPR_TEMP, 10,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_TEMP, 9),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_FOG,
                             D3DSP_WRITEMASK_2),
            QXP_SOURCE(D3DSPR_INPUT, 2),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POINT_SIZE,
                             D3DSP_WRITEMASK_3),
            QXP_SOURCE(D3DSPR_INPUT, 3),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 0,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 4),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_TEMP, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;
    const char *output_source;
    DWORD output_bytes;
    DWORD glsl_bytes = 0;
    FILE *file;

    if (!path || !qxp_translate(shader,
            sizeof(shader) / sizeof(shader[0]), &result)) return 0;
    output_source = result.source;
    output_bytes = result.source_bytes;
    if (glsl) {
        if (!QxpConvertVertexShaderArbToGlsl(
                result.source, result.source_bytes, qxp_glsl_source,
                sizeof(qxp_glsl_source), &glsl_bytes)) return 0;
        output_source = qxp_glsl_source;
        output_bytes = glsl_bytes;
    }
    file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(output_source, 1, output_bytes, file) != output_bytes ||
        fclose(file) != 0) return 0;
    printf("qxp_vshader: emitted %lu-byte %s smoke source\n",
           (unsigned long)output_bytes, glsl ? "clip GLSL" : "ARBvp");
    return 1;
}

static int qxp_emit_clip_visual(const char *path)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;
    DWORD glsl_bytes = 0;
    FILE *file;

    if (!path || !qxp_translate(shader,
            sizeof(shader) / sizeof(shader[0]), &result) ||
        !QxpConvertVertexShaderArbToGlsl(
            result.source, result.source_bytes, qxp_glsl_source,
            sizeof(qxp_glsl_source), &glsl_bytes)) return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(qxp_glsl_source, 1, glsl_bytes, file) != glsl_bytes ||
        fclose(file) != 0) return 0;
    printf("qxp_vshader: emitted %lu-byte clip visual GLSL source\n",
           (unsigned long)glsl_bytes);
    return 1;
}

static void qxp_test_texcoord_projection(void)
{
    static const DWORD shader[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 0, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 3),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 1,
                             D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1),
            QXP_SOURCE(D3DSPR_INPUT, 4),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 2, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 5),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 3, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 6),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 4, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 7),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 5, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 8),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 6, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 9),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEXCRDOUT, 7, D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 10),
        D3DSIO_MOV, QXP_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                             D3DSP_WRITEMASK_ALL),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_VSHADER_RESULT result;
    DWORD glsl_bytes = 0;
    DWORD expected = QXP_VS_OUTPUT_POSITION;
    DWORD index;

    for (index = 0; index < 8; ++index)
        expected |= QXP_VS_OUTPUT_TEXCOORD0 << index;
    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "texcoord projection translates");
    qxp_check(result.written_outputs == expected,
              "texcoord projection output metadata");
    /* qt mirrors are initialized so a partial oTn.xy write leaves q=1 */
    qxp_check(strstr(result.source, "MOV qt0, {0.0, 0.0, 0.0, 1.0};") != NULL,
              "qt mirrors initialized");
    /* oT0 full write routes to qt0, oT1 partial (.xy) routes to qt1.xy */
    qxp_check(strstr(result.source, "MOV qt0, vertex.attrib[3];") != NULL,
              "oT0 routed through qt0");
    qxp_check(strstr(result.source, "MOV qt1.xy, vertex.attrib[4]") != NULL,
              "oT1 partial routed through qt1.xy");
    /* END epilogue selects D3D projected q from program.local[1] */
    qxp_check(strstr(result.source,
                     "SUB qd, {1.0, 1.0, 1.0, 1.0}, program.local[1];") != NULL,
              "projected complement emitted");
    qxp_check(strstr(result.source,
                     "MOV result.texcoord[0].xyz, qt0;") != NULL &&
              strstr(result.source,
                     "MAD result.texcoord[0].w, qt0.w, program.local[1].x, qd.x;") != NULL,
              "oT0 projected-q epilogue emitted");
    qxp_check(strstr(result.source,
                     "MAD result.texcoord[1].w, qt1.w, program.local[1].y, qd.y;") != NULL,
              "oT1 projected-q epilogue emitted");
    qxp_check(strstr(result.source,
                     "MAD result.texcoord[2].w, qt2.w, program.local[1].z, qd.z;") != NULL,
              "oT2 projected-q epilogue emitted");
    qxp_check(strstr(result.source,
                     "MAD result.texcoord[3].w, qt3.w, program.local[1].w, qd.w;") != NULL,
              "oT3 projected-q epilogue emitted");
    qxp_check(strstr(result.source,
                     "MOV result.texcoord[4], vertex.attrib[7];") != NULL &&
              strstr(result.source,
                     "MOV result.texcoord[7], vertex.attrib[10];") != NULL &&
              strstr(result.source, "qt4") == NULL,
              "oT4 through oT7 remain unchanged");
    /* the clip GLSL path must convert the projected-q epilogue too */
    qxp_check(QxpConvertVertexShaderArbToGlsl(
                  result.source, result.source_bytes, qxp_glsl_source,
                  sizeof(qxp_glsl_source), &glsl_bytes) &&
              strstr(qxp_glsl_source, "uniform vec4 qxpProjected;") != NULL &&
              strstr(qxp_glsl_source,
                     "vec4 qt0, qt1, qt2, qt3;") != NULL &&
              strstr(qxp_glsl_source, "qt0 = qxpMOV(qxpV3);") != NULL &&
              strstr(qxp_glsl_source,
                     "gl_TexCoord[4] = qxpMOV(qxpV7);") != NULL &&
              strstr(qxp_glsl_source,
                     "gl_TexCoord[7] = qxpMOV(qxpV10);") != NULL,
              "texcoord projection clip GLSL conversion");
}

int main(int argc, char **argv)
{
    qxp_test_arithmetic();
    qxp_test_scalar_math();
    qxp_test_matrices();
    qxp_test_relative_addressing();
    qxp_test_outputs();
    qxp_test_full_mask_scalar_outputs();
    qxp_test_split_position();
    qxp_test_texcoord_projection();
    qxp_test_comments_and_budget();
    qxp_test_worst_valid_expansion();
    qxp_test_rejections();
    qxp_test_glsl_conversion();
    qxp_test_glsl_temp_contract();
    if (qxp_failures) {
        fprintf(stderr, "qxp_vshader: %d contract checks failed\n",
                qxp_failures);
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--emit-arbvp") == 0) {
        if (!qxp_emit_vshader_smoke(argv[2], 0)) {
            fputs("qxp_vshader: could not emit ARBvp smoke source\n",
                  stderr);
            return 1;
        }
    } else if (argc == 3 && strcmp(argv[1], "--emit-clip-glsl") == 0) {
        if (!qxp_emit_vshader_smoke(argv[2], 1)) {
            fputs("qxp_vshader: could not emit clip GLSL smoke source\n",
                  stderr);
            return 1;
        }
    } else if (argc == 3 &&
               strcmp(argv[1], "--emit-clip-visual-glsl") == 0) {
        if (!qxp_emit_clip_visual(argv[2])) {
            fputs("qxp_vshader: could not emit clip visual GLSL source\n",
                  stderr);
            return 1;
        }
    } else if (argc != 1) {
        fputs("usage: qxp_vshader_test [--emit-arbvp path | "
              "--emit-clip-glsl path | --emit-clip-visual-glsl path]\n",
              stderr);
        return 1;
    }
    puts("qxp_vshader: all translator checks passed");
    return 0;
}
