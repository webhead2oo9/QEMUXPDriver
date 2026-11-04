#include <stdio.h>
#include <stdlib.h>

#include "qxpmp/qxp_texture_op.h"

static void require(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "qxp_texture_op_test: %s\n", message);
    exit(1);
}

static void require_plan(ULONG operation, ULONG argument1, ULONG argument2,
                         ULONG source0, ULONG source1, ULONG source2)
{
    QXP_TEXTURE_SPECIAL_PLAN plan;

    require(qxp_texture_special_plan(
                operation, argument1, argument2, &plan),
            "supported operation rejected");
    require(plan.combine == QXP_GL_MODULATE_ADD_ATI,
            "wrong three-source combine mode");
    require(plan.source[0] == source0, "wrong source zero");
    require(plan.source[1] == source1, "wrong additive source");
    require(plan.source[2] == source2, "wrong multiplier source");
}

static void require_combine4_special(
    ULONG operation, ULONG argument1, ULONG argument2,
    ULONG source0, ULONG source1, ULONG source2, ULONG source3)
{
    QXP_TEXTURE_COMBINE4_PLAN plan;

    require(qxp_texture_combine4_special_plan(
                operation, argument1, argument2, &plan),
            "supported combine4 color operation rejected");
    require(plan.combine == QXP_GL_ADD && plan.scale == 1,
            "wrong combine4 color equation");
    require(plan.source[0] == source0 && plan.source[1] == source1 &&
            plan.source[2] == source2 && plan.source[3] == source3,
            "wrong combine4 color sources");
}

static void require_combine4_alpha(
    ULONG operation, ULONG argument1, ULONG argument2, ULONG argument0,
    ULONG combine, ULONG scale,
    ULONG source0, ULONG source1, ULONG source2, ULONG source3)
{
    QXP_TEXTURE_COMBINE4_PLAN plan;

    require(qxp_texture_combine4_alpha_plan(
                operation, argument1, argument2, argument0, &plan),
            "supported combine4 alpha operation rejected");
    require(plan.combine == combine && plan.scale == scale,
            "wrong combine4 alpha equation");
    require(plan.source[0] == source0 && plan.source[1] == source1 &&
            plan.source[2] == source2 && plan.source[3] == source3,
            "wrong combine4 alpha sources");
}

int main(void)
{
    ULONG texture = QXP_D3DTA_TEXTURE;
    ULONG current = QXP_D3DTA_CURRENT;
    ULONG complemented = texture | QXP_D3DTA_COMPLEMENT;
    QXP_TEXTURE_SPECIAL_PLAN plan;
    QXP_TEXTURE_COMBINE4_PLAN combine4_plan;

    require_plan(QXP_D3DTOP_MODULATEALPHA_ADDCOLOR, texture, current,
                 current, texture,
                 texture | QXP_D3DTA_ALPHAREPLICATE);
    require_plan(QXP_D3DTOP_MODULATECOLOR_ADDALPHA, texture, current,
                 texture, texture | QXP_D3DTA_ALPHAREPLICATE, current);
    require_plan(QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR, texture, current,
                 current, texture,
                 texture | QXP_D3DTA_ALPHAREPLICATE |
                     QXP_D3DTA_COMPLEMENT);
    require_plan(QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA, texture, current,
                 current, texture | QXP_D3DTA_ALPHAREPLICATE,
                 texture | QXP_D3DTA_COMPLEMENT);

    require_combine4_special(
        QXP_D3DTOP_MODULATEALPHA_ADDCOLOR, texture, current,
        texture, QXP_TEXTURE_SOURCE_ONE, current,
        texture | QXP_D3DTA_ALPHAREPLICATE);
    require_combine4_special(
        QXP_D3DTOP_MODULATECOLOR_ADDALPHA, texture, current,
        texture, current, texture | QXP_D3DTA_ALPHAREPLICATE,
        QXP_TEXTURE_SOURCE_ONE);
    require_combine4_special(
        QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR, texture, current,
        current, texture | QXP_D3DTA_ALPHAREPLICATE |
                     QXP_D3DTA_COMPLEMENT,
        texture, QXP_TEXTURE_SOURCE_ONE);
    require_combine4_special(
        QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA, texture, current,
        current, texture | QXP_D3DTA_COMPLEMENT,
        texture | QXP_D3DTA_ALPHAREPLICATE,
        QXP_TEXTURE_SOURCE_ONE);

    require_combine4_alpha(
        QXP_D3DTOP_SELECTARG1, texture, current, texture,
        QXP_GL_ADD, 1, texture, QXP_TEXTURE_SOURCE_ONE,
        QXP_TEXTURE_SOURCE_ZERO, QXP_TEXTURE_SOURCE_ZERO);
    require_combine4_alpha(
        QXP_D3DTOP_MODULATE4X, texture, current, texture,
        QXP_GL_ADD, 4, texture, current,
        QXP_TEXTURE_SOURCE_ZERO, QXP_TEXTURE_SOURCE_ZERO);
    require_combine4_alpha(
        QXP_D3DTOP_ADDSIGNED2X, texture, current, texture,
        QXP_GL_ADD_SIGNED, 2, texture, QXP_TEXTURE_SOURCE_ONE,
        current, QXP_TEXTURE_SOURCE_ONE);
    require_combine4_alpha(
        QXP_D3DTOP_BLENDCURRENTALPHA, texture, current, texture,
        QXP_GL_ADD, 1, texture, current, current,
        current | QXP_D3DTA_COMPLEMENT);
    require_combine4_alpha(
        QXP_D3DTOP_LERP, texture, current, QXP_D3DTA_TFACTOR,
        QXP_GL_ADD, 1, texture, QXP_D3DTA_TFACTOR, current,
        QXP_D3DTA_TFACTOR | QXP_D3DTA_COMPLEMENT);

    /* Inversion composes with D3DTA_COMPLEMENT; it does not blindly set it. */
    require_plan(QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR,
                 complemented, current, current, complemented,
                 texture | QXP_D3DTA_ALPHAREPLICATE);
    require_plan(QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA,
                 complemented, current, current,
                 complemented | QXP_D3DTA_ALPHAREPLICATE, texture);

    require(!qxp_texture_special_plan(
                QXP_D3DTOP_MODULATE, texture, current, &plan),
            "ordinary operation accepted as special");
    require(!qxp_texture_special_plan(
                QXP_D3DTOP_MODULATEALPHA_ADDCOLOR,
                texture, current, NULL),
            "null output accepted");
    require(!qxp_texture_combine4_special_plan(
                QXP_D3DTOP_MODULATE, texture, current, &combine4_plan),
            "ordinary operation accepted as combine4 special");
    require(!qxp_texture_combine4_alpha_plan(
                QXP_D3DTOP_SUBTRACT, texture, current, texture,
                &combine4_plan),
            "unrepresentable alpha subtract accepted");
    require(!qxp_texture_stage_requires_texture(
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_TFACTOR,
                QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT,
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_TFACTOR,
                QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT),
            "texture-factor-only stage requires a texture");
    require(qxp_texture_stage_requires_texture(
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_TEXTURE,
                QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT,
                QXP_D3DTOP_DISABLE, 0, 0, 0),
            "explicit color texture source was missed");
    require(qxp_texture_stage_requires_texture(
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_CURRENT,
                QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT,
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_TEXTURE,
                QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT),
            "explicit alpha texture source was missed");
    require(qxp_texture_stage_requires_texture(
                QXP_D3DTOP_BLENDTEXTUREALPHA, QXP_D3DTA_CURRENT,
                QXP_D3DTA_DIFFUSE, QXP_D3DTA_CURRENT,
                QXP_D3DTOP_DISABLE, 0, 0, 0),
            "implicit texture-alpha factor was missed");
    require(qxp_texture_stage_requires_texture(
                QXP_D3DTOP_LERP, QXP_D3DTA_CURRENT,
                QXP_D3DTA_DIFFUSE, QXP_D3DTA_TEXTURE,
                QXP_D3DTOP_DISABLE, 0, 0, 0),
            "third texture argument was missed");
    require(qxp_texture_stage_requires_texture(
                0xdeadUL, QXP_D3DTA_CURRENT, QXP_D3DTA_CURRENT,
                QXP_D3DTA_CURRENT, QXP_D3DTOP_DISABLE, 0, 0, 0),
            "unknown operation was not conservative");
    require(!qxp_texture_stage_requires_texture(
                QXP_D3DTOP_DISABLE, QXP_D3DTA_TEXTURE,
                QXP_D3DTA_TEXTURE, QXP_D3DTA_TEXTURE,
                QXP_D3DTOP_SELECTARG1, QXP_D3DTA_TEXTURE,
                QXP_D3DTA_TEXTURE, QXP_D3DTA_TEXTURE),
            "disabled color stage examined alpha arguments");
    puts("qxp_texture_op_test: ok");
    return 0;
}
