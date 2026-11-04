/* Compile this file into the XPDM display DLL and add
 * { INDEX_DrvEscape, (PFN)DrvEscape } to its DRVFN table. */
#include <stddef.h>

#ifndef QXP_ESCAPE_PORTABLE_TEST
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)
#endif
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

#include "qxp_escape.h"

#ifdef QXP_ESCAPE_PORTABLE_TEST
typedef qxp_u32 ULONG;
typedef void *PVOID;
typedef void SURFOBJ;
#define APIENTRY
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#else
ULONG APIENTRY QxpD3d9SourceSelfTest(
    const QXP_D3D9_SOURCE_SELFTEST_REQUEST *request,
    QXP_D3D9_SOURCE_SELFTEST_RESULT *result);
ULONG APIENTRY QxpD3d9Sm2LinkSelfTest(
    const QXP_D3D9_SM2_LINK_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT *result);
ULONG APIENTRY QxpD3d9Sm2DrawSelfTest(
    const QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT *result);
ULONG APIENTRY QxpD3d9Sm2TextureSelfTest(
    const QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *result);

/* The standalone WDK escape probe deliberately has no DirectDraw subsystem.
 * Let that link use a no-op fallback while the real display DLL resolves the
 * strong implementation in directdraw.c. */
ULONG APIENTRY QxpD3d9SourceSelfTestUnavailable(
    const QXP_D3D9_SOURCE_SELFTEST_REQUEST *request,
    QXP_D3D9_SOURCE_SELFTEST_RESULT *result)
{
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(result);
    return 0;
}

ULONG APIENTRY QxpD3d9Sm2LinkSelfTestUnavailable(
    const QXP_D3D9_SM2_LINK_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT *result)
{
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(result);
    return 0;
}

ULONG APIENTRY QxpD3d9Sm2DrawSelfTestUnavailable(
    const QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT *result)
{
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(result);
    return 0;
}

ULONG APIENTRY QxpD3d9Sm2TextureSelfTestUnavailable(
    const QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *result)
{
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(result);
    return 0;
}

#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_QxpD3d9SourceSelfTest@8=_QxpD3d9SourceSelfTestUnavailable@8")
#pragma comment(linker, "/alternatename:_QxpD3d9Sm2LinkSelfTest@8=_QxpD3d9Sm2LinkSelfTestUnavailable@8")
#pragma comment(linker, "/alternatename:_QxpD3d9Sm2DrawSelfTest@8=_QxpD3d9Sm2DrawSelfTestUnavailable@8")
#pragma comment(linker, "/alternatename:_QxpD3d9Sm2TextureSelfTest@8=_QxpD3d9Sm2TextureSelfTestUnavailable@8")
#endif
#endif

qxp_u32 QxpD3d9Sm2TextureOraclePass(
    const QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *result)
{
    if (!result) return 0;
    return result->version == QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION &&
           result->observed_sample0 == result->expected_sample0 &&
           result->observed_sample1 == result->expected_sample1 &&
           result->multi_overall == 1 &&
           result->multi_observed_sample0 ==
               result->multi_expected_sample0 &&
           result->multi_observed_sample1 ==
               result->multi_expected_sample1 &&
           result->proj_overall == 1 &&
           result->proj_observed_sample0 ==
               result->proj_expected_sample0 &&
           result->proj_observed_sample1 ==
               result->proj_expected_sample1 &&
           result->cube_overall == 1 &&
           result->cube_observed_sample0 ==
               result->cube_expected_sample0 &&
           result->cube_observed_sample1 ==
               result->cube_expected_sample1 &&
           result->depth_overall == 1 &&
           result->depth_observed_pass_a_overlap ==
               result->depth_expected_pass_a_overlap &&
           result->depth_observed_pass_a_far ==
               result->depth_expected_pass_a_far &&
           result->depth_observed_pass_b_overlap ==
               result->depth_expected_pass_b_overlap &&
           result->depth_observed_pass_b_far ==
               result->depth_expected_pass_b_far &&
           result->scissor_overall == 1 &&
           result->scissor_observed_pass_a_excluded ==
               result->scissor_expected_pass_a_excluded &&
           result->scissor_observed_pass_a_included ==
               result->scissor_expected_pass_a_included &&
           result->scissor_observed_pass_b_excluded ==
               result->scissor_expected_pass_b_excluded &&
           result->scissor_observed_pass_b_included ==
               result->scissor_expected_pass_b_included &&
           result->stream_offset_overall == 1 &&
           result->stream_offset_expected_pass_a_left !=
               result->stream_offset_expected_pass_b_left &&
           result->stream_offset_expected_pass_a_right !=
               result->stream_offset_expected_pass_b_right &&
           result->stream_offset_observed_pass_a_left ==
               result->stream_offset_expected_pass_a_left &&
           result->stream_offset_observed_pass_a_right ==
               result->stream_offset_expected_pass_a_right &&
           result->stream_offset_observed_pass_b_left ==
               result->stream_offset_expected_pass_b_left &&
           result->stream_offset_observed_pass_b_right ==
               result->stream_offset_expected_pass_b_right &&
           result->texldb_overall == 1 &&
           result->texldb_expected_control !=
               result->texldb_expected_biased &&
           result->texldb_observed_control ==
               result->texldb_expected_control &&
           result->texldb_observed_biased ==
               result->texldb_expected_biased &&
           result->four_overall == 1 &&
           result->four_expected_sample0 == 0xff6ddb57U &&
           result->four_expected_sample1 == 0xffd7615fU &&
           result->four_expected_sample0 !=
               result->multi_expected_sample0 &&
           result->four_expected_sample1 !=
               result->multi_expected_sample1 &&
           result->four_observed_sample0 ==
               result->four_expected_sample0 &&
           result->four_observed_sample1 ==
               result->four_expected_sample1 &&
           /* Diagnostic volume (3D) fetch: terminal stage 175, all volume
            * execution/deletion telemetry, the hardcoded fixed sample
            * coordinates, the two hardcoded exact expected pixels, unequal
            * expected depths (so a slice-zero fallback that reports the same
            * value at both points fails), and both exact observations. */
           result->stage == QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE &&
           result->volume_texture_defined == 1 &&
           result->volume_texture_uploaded == 1 &&
           result->volume_vertex_decode_status == 0 &&
           result->volume_vertex_glsl_bytes != 0 &&
           result->volume_pixel_decode_status == 0 &&
           result->volume_pixel_glsl_bytes != 0 &&
           result->volume_define_succeeded == 1 &&
           result->volume_target_bound == 1 &&
           result->volume_clear_succeeded == 1 &&
           result->volume_draw_succeeded == 1 &&
           result->volume_readback_succeeded == 1 &&
           result->volume_sample0_x == 4 &&
           result->volume_sample0_y == 7 &&
           result->volume_sample1_x == 10 &&
           result->volume_sample1_y == 7 &&
           result->volume_expected_sample0 == 0xff23500aU &&
           result->volume_expected_sample1 == 0xff835a2aU &&
           result->volume_expected_sample0 !=
               result->volume_expected_sample1 &&
           result->volume_observed_sample0 ==
               result->volume_expected_sample0 &&
           result->volume_observed_sample1 ==
               result->volume_expected_sample1 &&
           result->volume_program_deleted == 1 &&
           result->volume_texture_deleted == 1 &&
           result->volume_overall == 1;
}

ULONG APIENTRY DrvEscape(SURFOBJ *surface, ULONG escape, ULONG input_bytes,
                         PVOID input, ULONG output_bytes, PVOID output)
{
    const qxp_u32 *queried_escape;

    UNREFERENCED_PARAMETER(surface);
    queried_escape = (const qxp_u32 *)input;
    if (escape == QXP_QUERYESCSUPPORT && queried_escape &&
        input_bytes >= sizeof(*queried_escape) &&
        (*queried_escape == QXP_D3D9_SOURCE_SELFTEST ||
         *queried_escape == QXP_D3D9_SM2_LINK_SELFTEST ||
         *queried_escape == QXP_D3D9_SM2_DRAW_SELFTEST ||
         *queried_escape == QXP_D3D9_SM2_TEXTURE_SELFTEST)) {
        return 1;
    }
    if (escape == QXP_D3D9_SOURCE_SELFTEST) {
        if (!output || output_bytes !=
                sizeof(QXP_D3D9_SOURCE_SELFTEST_RESULT)) return 0;
        if (!input || input_bytes !=
                sizeof(QXP_D3D9_SOURCE_SELFTEST_REQUEST)) return 0;
        return QxpD3d9SourceSelfTest(
            (const QXP_D3D9_SOURCE_SELFTEST_REQUEST *)input,
            (QXP_D3D9_SOURCE_SELFTEST_RESULT *)output);
    }
    if (escape == QXP_D3D9_SM2_LINK_SELFTEST) {
        if (!output || output_bytes !=
                sizeof(QXP_D3D9_SM2_LINK_SELFTEST_RESULT)) return 0;
        if (!input || input_bytes !=
                sizeof(QXP_D3D9_SM2_LINK_SELFTEST_REQUEST)) return 0;
        return QxpD3d9Sm2LinkSelfTest(
            (const QXP_D3D9_SM2_LINK_SELFTEST_REQUEST *)input,
            (QXP_D3D9_SM2_LINK_SELFTEST_RESULT *)output);
    }
    if (escape == QXP_D3D9_SM2_DRAW_SELFTEST) {
        if (!output || output_bytes !=
                sizeof(QXP_D3D9_SM2_DRAW_SELFTEST_RESULT)) return 0;
        if (!input || input_bytes !=
                sizeof(QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST)) return 0;
        return QxpD3d9Sm2DrawSelfTest(
            (const QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST *)input,
            (QXP_D3D9_SM2_DRAW_SELFTEST_RESULT *)output);
    }
    if (escape == QXP_D3D9_SM2_TEXTURE_SELFTEST) {
        if (!output || output_bytes !=
                sizeof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT)) return 0;
        if (!input || input_bytes !=
                sizeof(QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST)) return 0;
        return QxpD3d9Sm2TextureSelfTest(
            (const QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST *)input,
            (QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *)output);
    }
    return qxp_escape(escape, input_bytes, input, output_bytes, output);
}
