#ifdef QXP_TEXTURE_OP_STANDALONE
#include <windef.h>
#else
#include "qxpmp.h"
#endif

#include "qxp_texture_op.h"

static BOOLEAN qxp_texture_argument_requires_texture(ULONG argument)
{
    if (argument & ~(QXP_D3DTA_SELECTMASK | QXP_D3DTA_COMPLEMENT |
                     QXP_D3DTA_ALPHAREPLICATE)) return TRUE;
    return (argument & QXP_D3DTA_SELECTMASK) == QXP_D3DTA_TEXTURE;
}

static BOOLEAN qxp_texture_operation_requires_texture(
    ULONG operation, ULONG argument1, ULONG argument2, ULONG argument0)
{
    switch (operation) {
    case QXP_D3DTOP_DISABLE:
        return FALSE;
    case QXP_D3DTOP_SELECTARG1:
        return qxp_texture_argument_requires_texture(argument1);
    case QXP_D3DTOP_SELECTARG2:
        return qxp_texture_argument_requires_texture(argument2);
    case QXP_D3DTOP_BLENDTEXTUREALPHA:
    case QXP_D3DTOP_BLENDTEXTUREALPHAPM:
    case QXP_D3DTOP_PREMODULATE:
    case QXP_D3DTOP_BUMPENVMAP:
    case QXP_D3DTOP_BUMPENVMAPLUMINANCE:
        return TRUE;
    case QXP_D3DTOP_MULTIPLYADD:
    case QXP_D3DTOP_LERP:
        return qxp_texture_argument_requires_texture(argument0) ||
               qxp_texture_argument_requires_texture(argument1) ||
               qxp_texture_argument_requires_texture(argument2);
    case QXP_D3DTOP_MODULATE:
    case QXP_D3DTOP_MODULATE2X:
    case QXP_D3DTOP_MODULATE4X:
    case QXP_D3DTOP_ADD:
    case QXP_D3DTOP_ADDSIGNED:
    case QXP_D3DTOP_ADDSIGNED2X:
    case QXP_D3DTOP_SUBTRACT:
    case QXP_D3DTOP_ADDSMOOTH:
    case QXP_D3DTOP_BLENDDIFFUSEALPHA:
    case QXP_D3DTOP_BLENDFACTORALPHA:
    case QXP_D3DTOP_BLENDCURRENTALPHA:
    case QXP_D3DTOP_MODULATEALPHA_ADDCOLOR:
    case QXP_D3DTOP_MODULATECOLOR_ADDALPHA:
    case QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR:
    case QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA:
    case QXP_D3DTOP_DOTPRODUCT3:
        return qxp_texture_argument_requires_texture(argument1) ||
               qxp_texture_argument_requires_texture(argument2);
    default:
        /* Unknown operations are conservatively sampling until their full
         * equation has a translator and an explicit argument contract. */
        return TRUE;
    }
}

BOOLEAN qxp_texture_stage_requires_texture(
    ULONG color_operation, ULONG color_argument1, ULONG color_argument2,
    ULONG color_argument0, ULONG alpha_operation, ULONG alpha_argument1,
    ULONG alpha_argument2, ULONG alpha_argument0)
{
    if (color_operation == QXP_D3DTOP_DISABLE) return FALSE;
    return qxp_texture_operation_requires_texture(
               color_operation, color_argument1, color_argument2,
               color_argument0) ||
           qxp_texture_operation_requires_texture(
               alpha_operation, alpha_argument1, alpha_argument2,
               alpha_argument0);
}

BOOLEAN qxp_texture_special_plan(
    ULONG operation, ULONG argument1, ULONG argument2,
    QXP_TEXTURE_SPECIAL_PLAN *plan)
{
    if (!plan) return FALSE;
    plan->combine = QXP_GL_MODULATE_ADD_ATI;
    switch (operation) {
    case QXP_D3DTOP_MODULATEALPHA_ADDCOLOR:
        /* Arg1.rgb + Arg1.a * Arg2.rgb. */
        plan->source[0] = argument2;
        plan->source[1] = argument1;
        plan->source[2] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        return TRUE;
    case QXP_D3DTOP_MODULATECOLOR_ADDALPHA:
        /* Arg1.rgb * Arg2.rgb + Arg1.a. */
        plan->source[0] = argument1;
        plan->source[1] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        plan->source[2] = argument2;
        return TRUE;
    case QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR:
        /* Arg1.rgb + (1 - Arg1.a) * Arg2.rgb. */
        plan->source[0] = argument2;
        plan->source[1] = argument1;
        plan->source[2] =
            (argument1 | QXP_D3DTA_ALPHAREPLICATE) ^
            QXP_D3DTA_COMPLEMENT;
        return TRUE;
    case QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA:
        /* (1 - Arg1.rgb) * Arg2.rgb + Arg1.a. */
        plan->source[0] = argument2;
        plan->source[1] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        plan->source[2] = argument1 ^ QXP_D3DTA_COMPLEMENT;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN qxp_texture_combine4_special_plan(
    ULONG operation, ULONG argument1, ULONG argument2,
    QXP_TEXTURE_COMBINE4_PLAN *plan)
{
    if (!plan) return FALSE;
    plan->combine = QXP_GL_ADD;
    plan->scale = 1;
    switch (operation) {
    case QXP_D3DTOP_MODULATEALPHA_ADDCOLOR:
        plan->source[0] = argument1;
        plan->source[1] = QXP_TEXTURE_SOURCE_ONE;
        plan->source[2] = argument2;
        plan->source[3] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        return TRUE;
    case QXP_D3DTOP_MODULATECOLOR_ADDALPHA:
        plan->source[0] = argument1;
        plan->source[1] = argument2;
        plan->source[2] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        plan->source[3] = QXP_TEXTURE_SOURCE_ONE;
        return TRUE;
    case QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR:
        plan->source[0] = argument2;
        plan->source[1] =
            (argument1 | QXP_D3DTA_ALPHAREPLICATE) ^
            QXP_D3DTA_COMPLEMENT;
        plan->source[2] = argument1;
        plan->source[3] = QXP_TEXTURE_SOURCE_ONE;
        return TRUE;
    case QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA:
        plan->source[0] = argument2;
        plan->source[1] = argument1 ^ QXP_D3DTA_COMPLEMENT;
        plan->source[2] = argument1 | QXP_D3DTA_ALPHAREPLICATE;
        plan->source[3] = QXP_TEXTURE_SOURCE_ONE;
        return TRUE;
    }
    return FALSE;
}

BOOLEAN qxp_texture_combine4_alpha_plan(
    ULONG operation, ULONG argument1, ULONG argument2, ULONG argument0,
    QXP_TEXTURE_COMBINE4_PLAN *plan)
{
    ULONG factor;

    if (!plan) return FALSE;
    plan->combine = QXP_GL_ADD;
    plan->scale = 1;
    plan->source[2] = QXP_TEXTURE_SOURCE_ZERO;
    plan->source[3] = QXP_TEXTURE_SOURCE_ZERO;
    if (operation == QXP_D3DTOP_SELECTARG1 ||
        operation == QXP_D3DTOP_SELECTARG2) {
        plan->source[0] = operation == QXP_D3DTOP_SELECTARG1 ?
                          argument1 : argument2;
        plan->source[1] = QXP_TEXTURE_SOURCE_ONE;
        return TRUE;
    }
    if (operation == QXP_D3DTOP_MODULATE ||
        operation == QXP_D3DTOP_MODULATE2X ||
        operation == QXP_D3DTOP_MODULATE4X) {
        plan->source[0] = argument1;
        plan->source[1] = argument2;
        if (operation == QXP_D3DTOP_MODULATE2X) plan->scale = 2;
        if (operation == QXP_D3DTOP_MODULATE4X) plan->scale = 4;
        return TRUE;
    }
    if (operation == QXP_D3DTOP_ADD ||
        operation == QXP_D3DTOP_ADDSIGNED ||
        operation == QXP_D3DTOP_ADDSIGNED2X) {
        plan->source[0] = argument1;
        plan->source[1] = QXP_TEXTURE_SOURCE_ONE;
        plan->source[2] = argument2;
        plan->source[3] = QXP_TEXTURE_SOURCE_ONE;
        if (operation != QXP_D3DTOP_ADD)
            plan->combine = QXP_GL_ADD_SIGNED;
        if (operation == QXP_D3DTOP_ADDSIGNED2X) plan->scale = 2;
        return TRUE;
    }
    if (operation == QXP_D3DTOP_BLENDDIFFUSEALPHA) {
        factor = QXP_D3DTA_DIFFUSE;
    } else if (operation == QXP_D3DTOP_BLENDTEXTUREALPHA) {
        factor = QXP_D3DTA_TEXTURE;
    } else if (operation == QXP_D3DTOP_BLENDFACTORALPHA) {
        factor = QXP_D3DTA_TFACTOR;
    } else if (operation == QXP_D3DTOP_BLENDCURRENTALPHA) {
        factor = QXP_D3DTA_CURRENT;
    } else if (operation == QXP_D3DTOP_LERP) {
        factor = argument0;
    } else {
        return FALSE;
    }
    plan->source[0] = argument1;
    plan->source[1] = factor;
    plan->source[2] = argument2;
    plan->source[3] = factor ^ QXP_D3DTA_COMPLEMENT;
    return TRUE;
}
