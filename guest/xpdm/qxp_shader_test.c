#include <stdio.h>
#include <string.h>

#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_shader.h"

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

#define QXP_DEST(type, number) \
    ((DWORD)(type) | (DWORD)(number) | D3DSP_WRITEMASK_ALL)
#define QXP_SOURCE(type, number) \
    ((DWORD)(type) | (DWORD)(number) | D3DSP_NOSWIZZLE)

typedef struct qxp_shader_result {
    char source[QXP_PIXEL_SHADER_SOURCE_MAX];
    DWORD source_bytes;
    DWORD sampled_stages;
    DWORD coordinate_stages;
    DWORD cube_stages;
    DWORD dynamic_stages;
    DWORD defined_constants;
    DWORD constants[QXP_PIXEL_SHADER_CONSTANT_COUNT][4];
} QXP_SHADER_RESULT;

static int qxp_failures;

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_shader: failed: %s\n", name);
}

static BOOL qxp_translate_cube(const DWORD *tokens, DWORD count,
                               DWORD cube_overrides,
                               QXP_SHADER_RESULT *result)
{
    memset(result, 0, sizeof(*result));
    return QxpTranslatePixelShader1x(
        tokens, count * sizeof(tokens[0]), cube_overrides, result->source,
        sizeof(result->source), &result->source_bytes,
        &result->sampled_stages, &result->coordinate_stages,
        &result->cube_stages, &result->dynamic_stages,
        &result->defined_constants,
        result->constants);
}

static BOOL qxp_translate(const DWORD *tokens, DWORD count,
                          QXP_SHADER_RESULT *result)
{
    return qxp_translate_cube(tokens, count, 0, result);
}

static void qxp_test_arithmetic(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_ADD, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_SUB, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 1),
        D3DSIO_MAD, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 2),
            QXP_SOURCE(D3DSPR_CONST, 3),
        D3DSIO_MUL, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 4),
        D3DSIO_DP3, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 5),
        D3DSIO_LRP, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_CONST, 6), QXP_SOURCE(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_CONST, 7),
        D3DSIO_CND, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEMP, 0) | D3DSP_REPLICATEALPHA,
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "core arithmetic translates");
    qxp_check(strstr(result.source, "MOV r0, v0;") != NULL,
              "mov emitted");
    qxp_check(strstr(result.source, "DP3 r1, r1, c5;") != NULL,
              "dp3 emitted");
    qxp_check(strstr(result.source, "CMP r0, -qd.x, r1, v0;") != NULL,
              "cnd emitted");
    qxp_check(strstr(result.source, "MOV result.color, r0;") != NULL,
              "result emitted");
}

static void qxp_test_coissued_arithmetic(void)
{
    /* Seven arithmetic slots: three RGB/alpha pairs carry ten instruction
     * tokens. This is the exact valid ps_1_1 shape that exposed slot
     * over-counting at a real level-load boundary. */
    static const DWORD shader[] = {
        0xffff0101UL,
        0x00000051UL, 0xa00f0001UL, 0x00000000UL, 0x00000000UL,
            0x00000000UL, 0x3f800000UL,
        0x00000042UL, 0xb00f0000UL,
        0x00000042UL, 0xb00f0001UL,
        0x00000040UL, 0xb00f0002UL,
        0x00000042UL, 0xb00f0003UL,
        0x00000008UL, 0x801f0000UL, 0xb4e40001UL, 0xb4e40003UL,
        0x00000005UL, 0x80080000UL, 0x80ff0000UL, 0x80ff0000UL,
        0x00000005UL, 0x80080000UL, 0x80ff0000UL, 0x80ff0000UL,
        0x00000005UL, 0x80080000UL, 0x80ff0000UL, 0x80ff0000UL,
        0x40000005UL, 0x80070001UL, 0xb0e40000UL, 0xa0e40000UL,
        0x00000008UL, 0xb01f0000UL, 0xb4e40002UL, 0xb4e40002UL,
        0x00000005UL, 0x80070000UL, 0x80ff0000UL, 0x80e40001UL,
        0x40000005UL, 0x80080001UL, 0xb6ff0000UL, 0xb6ff0000UL,
        0x00000005UL, 0x80070000UL, 0x80e40000UL, 0x80ff0001UL,
        0x40000001UL, 0x80080000UL, 0xa0ff0001UL,
        0x0000ffffUL
    };
    QXP_SHADER_RESULT result;
    const char *first;
    const char *second;
    const char *commit;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "coissued arithmetic shares instruction slots");
    first = strstr(result.source, "MUL qp.w, r0.wwww, r0.wwww;");
    second = strstr(result.source, "MUL r1.xyz, t0, c0;");
    commit = strstr(result.source, "MOV r0.w, qp;");
    qxp_check(first && second && commit &&
              first < second && second < commit,
              "coissued pair commits after both source reads");
}

static void qxp_test_texture_and_definition(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_DEF, QXP_DEST(D3DSPR_CONST, 2),
            0x3f800000UL, 0x3f000000UL, 0, 0xbf800000UL,
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_ADD, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 0), QXP_SOURCE(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "texture and def translate");
    qxp_check(result.sampled_stages == 1 &&
              result.coordinate_stages == 1 &&
              result.dynamic_stages == 1,
              "texture stage masks");
    qxp_check(result.defined_constants == (1UL << 2),
              "defined constant mask");
    qxp_check(result.constants[2][0] == 0x3f800000UL &&
              result.constants[2][3] == 0xbf800000UL,
              "defined constant values");
    qxp_check(strstr(result.source,
                     "TXP t0, i0, texture[0], 2D;") != NULL,
              "projected lookup emitted");
}

static void qxp_test_dynamic_texture_targets(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate_cube(
                  shader, sizeof(shader) / sizeof(shader[0]), 1, &result),
              "cube tex variant translates");
    qxp_check(result.sampled_stages == 1 &&
              result.dynamic_stages == 1 && result.cube_stages == 1,
              "cube tex variant masks");
    qxp_check(strstr(result.source,
                     "TXP t0, i0, texture[0], CUBE;") != NULL,
              "cube tex variant emitted");
}

static void qxp_test_fourth_texture_stage(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 3),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 3),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "fourth texture stage translates");
    qxp_check(result.sampled_stages == 8 &&
              result.coordinate_stages == 8,
              "fourth texture stage masks");
    qxp_check(strstr(result.source,
                     "TXP t3, i3, texture[3], 2D;") != NULL,
              "fourth projected lookup emitted");
}

static void qxp_test_luminance_bump(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXBEML, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "texbeml translates");
    qxp_check(result.sampled_stages == 3 &&
              result.coordinate_stages == 3,
              "texbeml stage masks");
    qxp_check(strstr(result.source, "PARAM l1 = program.local[9];") != NULL,
              "luminance constant declared");
    qxp_check(strstr(result.source, "MAD q1.x, t0.z, l1.x, l1.y;") != NULL,
              "luminance calculation emitted");
}

static void qxp_test_register_texture(void)
{
    static const DWORD ar_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXREG2AR, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
        D3DSIO_END
    };
    static const DWORD gb_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXREG2AR, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXREG2GB, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_ADD, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 2),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(ar_shader,
                            sizeof(ar_shader) / sizeof(ar_shader[0]),
                            &result),
              "texreg2ar translates");
    qxp_check(result.sampled_stages == 3 &&
              result.coordinate_stages == 3,
              "texreg2ar stage masks");
    qxp_check(strstr(result.source,
                     "TEX t1, t0.wxwx, texture[1], 2D;") != NULL,
              "texreg2ar lookup emitted");

    qxp_check(qxp_translate(gb_shader,
                            sizeof(gb_shader) / sizeof(gb_shader[0]),
                            &result),
              "texreg2gb translates");
    qxp_check(result.sampled_stages == 7 &&
              result.coordinate_stages == 7,
              "combined texreg2 stage masks");
    qxp_check(strstr(result.source,
                     "TEX t2, t0.yzyz, texture[2], 2D;") != NULL,
              "texreg2gb lookup emitted");
}

static void qxp_test_matrix_texture(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2TEX, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 2),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "texm3x2 translates");
    qxp_check(result.sampled_stages == 5 &&
              result.coordinate_stages == 7,
              "texm3x2 stage masks");
    qxp_check(strstr(result.source, "DP3 q0.x, q1, t0;") != NULL,
              "texm3x2 first row emitted");
    qxp_check(strstr(result.source,
                     "TEX t2, q0, texture[2], 2D;") != NULL,
              "texm3x2 lookup emitted");
}

static void qxp_test_matrix_cube_texture(void)
{
    static const DWORD texture_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3TEX, QXP_DEST(D3DSPR_TEXTURE, 3),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 3),
        D3DSIO_END
    };
    static const DWORD spec_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3SPEC, QXP_DEST(D3DSPR_TEXTURE, 3),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
            QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 3),
        D3DSIO_END
    };
    static const DWORD vspec_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3VSPEC, QXP_DEST(D3DSPR_TEXTURE, 3),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 3),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(
                  texture_shader,
                  sizeof(texture_shader) / sizeof(texture_shader[0]),
                  &result),
              "texm3x3tex translates");
    qxp_check(result.sampled_stages == 9 &&
              result.coordinate_stages == 15 &&
              result.cube_stages == 8,
              "texm3x3tex stage masks");
    qxp_check(strstr(result.source, "DP3 q0.z, q1, t0;") != NULL,
              "texm3x3 third row emitted");
    qxp_check(strstr(result.source,
                     "TEX t3, q0, texture[3], CUBE;") != NULL,
              "texm3x3 cube lookup emitted");

    qxp_check(qxp_translate(
                  spec_shader,
                  sizeof(spec_shader) / sizeof(spec_shader[0]), &result),
              "texm3x3spec translates");
    qxp_check(result.sampled_stages == 9 && result.cube_stages == 8,
              "texm3x3spec stage masks");
    qxp_check(strstr(result.source, "DP3 qd.x, q0, c0;") != NULL &&
              strstr(result.source, "SUB q0.xyz, q1, c0;") != NULL,
              "texm3x3spec reflection emitted");

    qxp_check(qxp_translate(
                  vspec_shader,
                  sizeof(vspec_shader) / sizeof(vspec_shader[0]),
                  &result),
              "texm3x3vspec translates");
    qxp_check(result.sampled_stages == 9 && result.cube_stages == 8,
              "texm3x3vspec stage masks");
    qxp_check(strstr(result.source, "DP3 qd.x, q0, q2;") != NULL &&
              strstr(result.source, "SUB q0.xyz, q1, q2;") != NULL,
              "texm3x3vspec reflection emitted");
}

static void qxp_test_pixel_shader_12(void)
{
    static const DWORD arithmetic_shader[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 2),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_CMP, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEMP, 2), QXP_SOURCE(D3DSPR_CONST, 1),
            QXP_SOURCE(D3DSPR_CONST, 2),
        D3DSIO_END
    };
    static const DWORD dot_shader[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXDP3, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0) | D3DSPSM_SIGN,
        D3DSIO_TEXDP3TEX, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 2),
        D3DSIO_END
    };
    static const DWORD register_rgb_shader[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXREG2RGB, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
        D3DSIO_END
    };
    static const DWORD matrix_shader[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3, QXP_DEST(D3DSPR_TEXTURE, 3),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 3),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(
                  arithmetic_shader,
                  sizeof(arithmetic_shader) / sizeof(arithmetic_shader[0]),
                  &result),
              "ps_1_2 arithmetic translates");
    qxp_check(strstr(result.source, "DP4 r2, r1, c0;") != NULL,
              "ps_1_2 dp4 emitted");
    qxp_check(strstr(result.source,
                     "SLT qd, r2, {0.0, 0.0, 0.0, 0.0};") != NULL &&
              strstr(result.source, "LRP qd, qd, c2, c1;") != NULL,
              "ps_1_2 zero-correct cmp emitted");

    qxp_check(qxp_translate(
                  dot_shader, sizeof(dot_shader) / sizeof(dot_shader[0]),
                  &result),
              "ps_1_2 dot texture instructions translate");
    qxp_check(result.sampled_stages == 5 &&
              result.coordinate_stages == 7,
              "ps_1_2 dot texture masks");
    qxp_check(strstr(result.source,
                     "MAD qd, t0, {2.0, 2.0, 2.0, 2.0}") != NULL &&
              strstr(result.source,
                     "TEX t2, q0, texture[2], 2D;") != NULL,
              "ps_1_2 dot texture operations emitted");

    qxp_check(qxp_translate_cube(
                  register_rgb_shader,
                  sizeof(register_rgb_shader) /
                      sizeof(register_rgb_shader[0]),
                  2, &result),
              "ps_1_2 texreg2rgb cube variant translates");
    qxp_check(result.dynamic_stages == 3 && result.cube_stages == 2,
              "ps_1_2 texreg2rgb variant masks");
    qxp_check(strstr(result.source,
                     "TEX t1, t0, texture[1], CUBE;") != NULL,
              "ps_1_2 texreg2rgb cube lookup emitted");

    qxp_check(qxp_translate(
                  matrix_shader,
                  sizeof(matrix_shader) / sizeof(matrix_shader[0]),
                  &result),
              "ps_1_2 texm3x3 translates");
    qxp_check(result.sampled_stages == 1 &&
              result.coordinate_stages == 15 &&
              result.cube_stages == 0,
              "ps_1_2 texm3x3 masks");
    qxp_check(strstr(result.source, "MOV t3.xyz, q0;") != NULL,
              "ps_1_2 texm3x3 result emitted");
}

static void qxp_test_pixel_shader_13_depth(void)
{
    static const DWORD shader[] = {
        D3DPS_VERSION(1, 3),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2DEPTH, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(qxp_translate(shader, sizeof(shader) / sizeof(shader[0]),
                            &result),
              "ps_1_3 texm3x2depth translates");
    qxp_check(result.sampled_stages == 1 &&
              result.coordinate_stages == 7,
              "ps_1_3 depth masks");
    qxp_check(strstr(result.source, "MOV result.depth, qd.x;") != NULL,
              "ps_1_3 depth output emitted");
}

static void qxp_test_rejections(void)
{
    static const DWORD missing_end[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0)
    };
    static const DWORD wrong_version[] = {
        D3DPS_VERSION(1, 4),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    static const DWORD truncated_texture[] = {
        D3DPS_VERSION(1, 1), D3DSIO_TEXBEML,
        QXP_DEST(D3DSPR_TEXTURE, 1)
    };
    static const DWORD incomplete_matrix[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_END
    };
    static const DWORD incomplete_matrix3x3[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x3PAD, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_END
    };
    static const DWORD undefined_register_source[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEXREG2AR, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 1),
        D3DSIO_END
    };
    static const DWORD early_depth[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_TEX, QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2PAD, QXP_DEST(D3DSPR_TEXTURE, 1),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXM3x2DEPTH, QXP_DEST(D3DSPR_TEXTURE, 2),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    static const DWORD aliased_cmp[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_CMP, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEMP, 0), QXP_SOURCE(D3DSPR_CONST, 0),
            QXP_SOURCE(D3DSPR_CONST, 1),
        D3DSIO_END
    };
    static const DWORD nontexture_shader[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    static const DWORD arithmetic_slot_overflow[] = {
        D3DPS_VERSION(1, 2),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_INPUT, 0), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_DP4, QXP_DEST(D3DSPR_TEMP, 1),
            QXP_SOURCE(D3DSPR_TEMP, 1), QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEMP, 1),
        D3DSIO_END
    };
    static const DWORD orphan_coissue[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_MOV | D3DSI_COISSUE, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    static const DWORD overlapping_coissue[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_MOV,
            QXP_DEST(D3DSPR_TEMP, 0) & ~D3DSP_WRITEMASK_3,
            QXP_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_MUL | D3DSI_COISSUE,
            QXP_DEST(D3DSPR_TEMP, 1) & ~D3DSP_WRITEMASK_3,
            QXP_SOURCE(D3DSPR_INPUT, 0),
            QXP_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_END
    };
    static const DWORD texture_coissue[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX | D3DSI_COISSUE,
            QXP_DEST(D3DSPR_TEXTURE, 0),
        D3DSIO_MOV, QXP_DEST(D3DSPR_TEMP, 0),
            QXP_SOURCE(D3DSPR_TEXTURE, 0),
        D3DSIO_END
    };
    QXP_SHADER_RESULT result;

    qxp_check(!qxp_translate(
                  missing_end,
                  sizeof(missing_end) / sizeof(missing_end[0]), &result),
              "missing end rejected");
    qxp_check(!qxp_translate(
                  wrong_version,
                  sizeof(wrong_version) / sizeof(wrong_version[0]), &result),
              "wrong version rejected");
    qxp_check(!qxp_translate(
                  truncated_texture,
                  sizeof(truncated_texture) / sizeof(truncated_texture[0]),
                  &result),
              "truncated texture instruction rejected");
    qxp_check(!qxp_translate(
                  incomplete_matrix,
                  sizeof(incomplete_matrix) / sizeof(incomplete_matrix[0]),
                  &result),
              "incomplete texm3x2 sequence rejected");
    qxp_check(!qxp_translate(
                  incomplete_matrix3x3,
                  sizeof(incomplete_matrix3x3) /
                      sizeof(incomplete_matrix3x3[0]),
                  &result),
              "incomplete texm3x3 sequence rejected");
    qxp_check(!qxp_translate(
                  undefined_register_source,
                  sizeof(undefined_register_source) /
                      sizeof(undefined_register_source[0]),
                  &result),
              "undefined texture-register source rejected");
    qxp_check(!qxp_translate(
                  early_depth,
                  sizeof(early_depth) / sizeof(early_depth[0]), &result),
              "ps_1_2 depth instruction rejected");
    qxp_check(!qxp_translate(
                  aliased_cmp,
                  sizeof(aliased_cmp) / sizeof(aliased_cmp[0]), &result),
              "ps_1_2 aliased cmp destination rejected");
    qxp_check(!qxp_translate_cube(
                  nontexture_shader,
                  sizeof(nontexture_shader) / sizeof(nontexture_shader[0]), 1,
                  &result),
              "unused cube override rejected");
    qxp_check(!qxp_translate(
                  arithmetic_slot_overflow,
                  sizeof(arithmetic_slot_overflow) /
                      sizeof(arithmetic_slot_overflow[0]),
                  &result),
              "ps_1_2 weighted arithmetic slot overflow rejected");
    qxp_check(!qxp_translate(
                  orphan_coissue,
                  sizeof(orphan_coissue) / sizeof(orphan_coissue[0]),
                  &result),
              "orphan coissue rejected");
    qxp_check(!qxp_translate(
                  overlapping_coissue,
                  sizeof(overlapping_coissue) /
                      sizeof(overlapping_coissue[0]),
                  &result),
              "overlapping coissue masks rejected");
    qxp_check(!qxp_translate(
                  texture_coissue,
                  sizeof(texture_coissue) / sizeof(texture_coissue[0]),
                  &result),
              "texture coissue rejected");
}

int main(void)
{
    qxp_test_arithmetic();
    qxp_test_coissued_arithmetic();
    qxp_test_texture_and_definition();
    qxp_test_dynamic_texture_targets();
    qxp_test_fourth_texture_stage();
    qxp_test_luminance_bump();
    qxp_test_register_texture();
    qxp_test_matrix_texture();
    qxp_test_matrix_cube_texture();
    qxp_test_pixel_shader_12();
    qxp_test_pixel_shader_13_depth();
    qxp_test_rejections();
    if (qxp_failures) {
        fprintf(stderr, "qxp_shader: %d contract checks failed\n",
                qxp_failures);
        return 1;
    }
    puts("qxp_shader: all translator checks passed");
    return 0;
}
