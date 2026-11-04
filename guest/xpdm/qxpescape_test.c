#include "qxp_escape.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static qxp_u32 source_selftest_calls;
static qxp_u32 sm2_link_selftest_calls;
static qxp_u32 sm2_draw_selftest_calls;
static qxp_u32 sm2_texture_selftest_calls;

static qxp_u32 qxp_test_source_selftest(
    const QXP_D3D9_SOURCE_SELFTEST_REQUEST *request,
    QXP_D3D9_SOURCE_SELFTEST_RESULT *result)
{
    ++source_selftest_calls;
    (void)request;
    memset(result, 0, sizeof(*result));
    result->version = QXP_D3D9_SOURCE_SELFTEST_VERSION;
    result->surfaces_examined = 0x10203040U;
    result->last_status = 0x50607080U;
    return 1;
}

static qxp_u32 qxp_test_sm2_link_selftest(
    const QXP_D3D9_SM2_LINK_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT *result)
{
    ++sm2_link_selftest_calls;
    (void)request;
    memset(result, 0, sizeof(*result));
    result->version = QXP_D3D9_SM2_LINK_SELFTEST_VERSION;
    result->stage = 0x11223344U;
    result->vertex_glsl_bytes = 0x55667788U;
    result->overall = 1;
    return 1;
}

static qxp_u32 qxp_test_sm2_draw_selftest(
    const QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT *result)
{
    ++sm2_draw_selftest_calls;
    (void)request;
    memset(result, 0, sizeof(*result));
    result->version = QXP_D3D9_SM2_DRAW_SELFTEST_VERSION;
    result->stage = 0x12345678U;
    result->inside_x = 10;
    result->inside_y = 4;
    result->expected_inside = 0xff2060dfU;
    result->observed_inside = 0xff2060dfU;
    result->overall = 1;
    return 1;
}

static qxp_u32 qxp_test_sm2_texture_selftest(
    const QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST *request,
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *result)
{
    ++sm2_texture_selftest_calls;
    (void)request;
    memset(result, 0, sizeof(*result));
    result->version = QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION;
    result->stage = QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE;
    result->sample0_x = 4;
    result->sample0_y = 10;
    result->expected_sample0 = 0xff459709U;
    result->observed_sample0 = 0xff459709U;
    result->sample1_x = 10;
    result->sample1_y = 4;
    result->expected_sample1 = 0xff8f3d11U;
    result->observed_sample1 = 0xff8f3d11U;
    result->overall = 1;
    result->multi_expected_sample0 = 0xff55c749U;
    result->multi_observed_sample0 = 0xff55c749U;
    result->multi_expected_sample1 = 0xffbf4d51U;
    result->multi_observed_sample1 = 0xffbf4d51U;
    result->multi_overall = 1;
    result->proj_expected_sample0 = 0xff459709U;
    result->proj_observed_sample0 = 0xff459709U;
    result->proj_expected_sample1 = 0xff8f3d11U;
    result->proj_observed_sample1 = 0xff8f3d11U;
    result->proj_overall = 1;
    result->cube_expected_sample0 = 0xff2060a0U;
    result->cube_observed_sample0 = 0xff2060a0U;
    result->cube_expected_sample1 = 0xff2060a0U;
    result->cube_observed_sample1 = 0xff2060a0U;
    result->cube_overall = 1;
    result->depth_overlap_x = 5;
    result->depth_overlap_y = 5;
    result->depth_far_only_x = 12;
    result->depth_far_only_y = 12;
    result->depth_expected_pass_a_overlap = 0xffdf9f20U;
    result->depth_observed_pass_a_overlap = 0xffdf9f20U;
    result->depth_expected_pass_a_far = 0xff2060dfU;
    result->depth_observed_pass_a_far = 0xff2060dfU;
    result->depth_expected_pass_b_overlap = 0xffdf9f20U;
    result->depth_observed_pass_b_overlap = 0xffdf9f20U;
    result->depth_expected_pass_b_far = 0xff2060dfU;
    result->depth_observed_pass_b_far = 0xff2060dfU;
    result->depth_overall = 1;
    result->scissor_excluded_x = 4;
    result->scissor_excluded_y = 10;
    result->scissor_included_x = 10;
    result->scissor_included_y = 4;
    result->scissor_expected_pass_a_excluded = 0xff07131fU;
    result->scissor_observed_pass_a_excluded = 0xff07131fU;
    result->scissor_expected_pass_a_included = 0xffdf9f20U;
    result->scissor_observed_pass_a_included = 0xffdf9f20U;
    result->scissor_expected_pass_b_excluded = 0xffdf9f20U;
    result->scissor_observed_pass_b_excluded = 0xffdf9f20U;
    result->scissor_expected_pass_b_included = 0xffdf9f20U;
    result->scissor_observed_pass_b_included = 0xffdf9f20U;
    result->scissor_overall = 1;
    result->stream_offset_stride = 16;
    result->stream_offset_buffer_bytes = 96;
    result->stream_offset_pass_a_offset = 0;
    result->stream_offset_pass_b_offset = 48;
    result->stream_offset_left_x = 2;
    result->stream_offset_left_y = 7;
    result->stream_offset_right_x = 12;
    result->stream_offset_right_y = 7;
    result->stream_offset_expected_pass_a_left = 0xffdf9f20U;
    result->stream_offset_observed_pass_a_left = 0xffdf9f20U;
    result->stream_offset_expected_pass_a_right = 0xff07131fU;
    result->stream_offset_observed_pass_a_right = 0xff07131fU;
    result->stream_offset_expected_pass_b_left = 0xff07131fU;
    result->stream_offset_observed_pass_b_left = 0xff07131fU;
    result->stream_offset_expected_pass_b_right = 0xffdf9f20U;
    result->stream_offset_observed_pass_b_right = 0xffdf9f20U;
    result->stream_offset_overall = 1;
    result->texldb_sample_x = 7;
    result->texldb_sample_y = 7;
    result->texldb_expected_control = 0xffe04020U;
    result->texldb_observed_control = 0xffe04020U;
    result->texldb_expected_biased = 0xff40a0e0U;
    result->texldb_observed_biased = 0xff40a0e0U;
    result->texldb_overall = 1;
    result->four_texture2_defined = 1;
    result->four_texture2_uploaded = 1;
    result->four_texture3_defined = 1;
    result->four_texture3_uploaded = 1;
    result->four_vertex_decode_status = 0;
    result->four_vertex_glsl_bytes = 0x123U;
    result->four_pixel_decode_status = 0;
    result->four_pixel_glsl_bytes = 0x234U;
    result->four_define_succeeded = 1;
    result->four_target_bound = 1;
    result->four_clear_succeeded = 1;
    result->four_draw_succeeded = 1;
    result->four_readback_succeeded = 1;
    result->four_expected_sample0 = 0xff6ddb57U;
    result->four_observed_sample0 = 0xff6ddb57U;
    result->four_expected_sample1 = 0xffd7615fU;
    result->four_observed_sample1 = 0xffd7615fU;
    result->four_program_deleted = 1;
    result->four_texture2_deleted = 1;
    result->four_texture3_deleted = 1;
    result->four_overall = 1;
    result->volume_texture_defined = 1;
    result->volume_texture_uploaded = 1;
    result->volume_vertex_decode_status = 0;
    result->volume_vertex_glsl_bytes = 0x345U;
    result->volume_pixel_decode_status = 0;
    result->volume_pixel_glsl_bytes = 0x456U;
    result->volume_define_succeeded = 1;
    result->volume_target_bound = 1;
    result->volume_clear_succeeded = 1;
    result->volume_draw_succeeded = 1;
    result->volume_readback_succeeded = 1;
    result->volume_sample0_x = 4;
    result->volume_sample0_y = 7;
    result->volume_sample1_x = 10;
    result->volume_sample1_y = 7;
    result->volume_expected_sample0 = 0xff23500aU;
    result->volume_observed_sample0 = 0xff23500aU;
    result->volume_expected_sample1 = 0xff835a2aU;
    result->volume_observed_sample1 = 0xff835a2aU;
    result->volume_program_deleted = 1;
    result->volume_texture_deleted = 1;
    result->volume_overall = 1;
    return 1;
}

#define QXP_ESCAPE_PORTABLE_TEST 1
#define QxpD3d9SourceSelfTest qxp_test_source_selftest
#define QxpD3d9Sm2LinkSelfTest qxp_test_sm2_link_selftest
#define QxpD3d9Sm2DrawSelfTest qxp_test_sm2_draw_selftest
#define QxpD3d9Sm2TextureSelfTest qxp_test_sm2_texture_selftest
#include "qxpdisp_escape.c"
#undef QxpD3d9Sm2TextureSelfTest
#undef QxpD3d9Sm2DrawSelfTest
#undef QxpD3d9Sm2LinkSelfTest
#undef QxpD3d9SourceSelfTest
#undef QXP_ESCAPE_PORTABLE_TEST

static int failed;

static void expect(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failed = 1;
    }
}

int main(void)
{
    uint32_t query = QXP_OPENGL_GETINFO;
    uint32_t unknown = 0x1234;
    uint32_t subescape = QXP_OPENGL_GETINFO_DRVNAME;
    uint32_t bad_subescape = 1;
    QXP_OPENGL_INFO info;
    QXP_D3D9_SOURCE_SELFTEST_REQUEST source_request;
    QXP_D3D9_SOURCE_SELFTEST_RESULT source_result;
    QXP_D3D9_SOURCE_SELFTEST_RESULT source_before;
    struct {
        QXP_D3D9_SOURCE_SELFTEST_RESULT result;
        qxp_u8 extra;
    } oversized, oversized_before;
    uint32_t source_query = QXP_D3D9_SOURCE_SELFTEST;
    uint32_t sm2_query = QXP_D3D9_SM2_LINK_SELFTEST;
    QXP_D3D9_SM2_LINK_SELFTEST_REQUEST sm2_request;
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT sm2_result;
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT sm2_before;
    struct {
        QXP_D3D9_SM2_LINK_SELFTEST_RESULT result;
        qxp_u8 extra;
    } sm2_oversized_output, sm2_oversized_output_before;
    struct {
        QXP_D3D9_SM2_LINK_SELFTEST_REQUEST request;
        qxp_u8 extra;
    } sm2_oversized_input;
    uint32_t sm2_draw_query = QXP_D3D9_SM2_DRAW_SELFTEST;
    QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST sm2_draw_request;
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT sm2_draw_result;
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT sm2_draw_before;
    struct {
        QXP_D3D9_SM2_DRAW_SELFTEST_RESULT result;
        qxp_u8 extra;
    } sm2_draw_oversized_output, sm2_draw_oversized_output_before;
    struct {
        QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST request;
        qxp_u8 extra;
    } sm2_draw_oversized_input;
    uint32_t sm2_texture_query = QXP_D3D9_SM2_TEXTURE_SELFTEST;
    QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST sm2_texture_request;
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT sm2_texture_result;
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT sm2_texture_before;
    struct {
        QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT result;
        qxp_u8 extra;
    } sm2_texture_oversized_output,
      sm2_texture_oversized_output_before;
    struct {
        QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST request;
        qxp_u8 extra;
    } sm2_texture_oversized_input;
    unsigned index;
    int tail_zero = 1;
    static const uint16_t expected_name[] = {
        'q', 'x', 'p', 'i', 'c', 'd', 0
    };

    expect(sizeof(info) == 0x214, "OPENGL_INFO ABI size");
    expect(qxp_escape(QXP_QUERYESCSUPPORT, sizeof(query), &query, 0, NULL) == 1,
           "support probe");
    expect(qxp_escape(QXP_QUERYESCSUPPORT, 3, &query, 0, NULL) == 0,
           "short support input");
    expect(qxp_escape(QXP_QUERYESCSUPPORT, sizeof(unknown), &unknown, 0,
                      NULL) == 0, "unknown support query");
    expect(qxp_escape(QXP_QUERYESCSUPPORT, sizeof(query), NULL, 0, NULL) == 0,
           "null support input");

    memset(&info, 0xa5, sizeof(info));
    expect(qxp_escape(QXP_OPENGL_GETINFO, sizeof(subescape), &subescape,
                      sizeof(info), &info) == 1, "info query");
    expect(info.version == 2 && info.driver_version == 1, "version tuple");
    expect(memcmp(info.driver_name, expected_name, sizeof(expected_name)) == 0,
           "driver name");
    for (index = sizeof(expected_name) / sizeof(expected_name[0]);
         index < QXP_MAX_PATH + 1U; ++index) {
        if (info.driver_name[index] != 0) tail_zero = 0;
    }
    expect(tail_zero, "zeroed name tail");

    memset(&info, 0xa5, sizeof(info));
    expect(qxp_escape(QXP_OPENGL_GETINFO, 0, NULL, sizeof(info), &info) == 1,
           "zero-length legacy info input");
    expect(info.version == 2 && info.driver_version == 1,
           "legacy version tuple");
    expect(qxp_escape(QXP_OPENGL_GETINFO, sizeof(bad_subescape),
                      &bad_subescape, sizeof(info), &info) == 0,
           "unknown info subescape");
    expect(qxp_escape(QXP_OPENGL_GETINFO, sizeof(subescape), &subescape,
                      sizeof(info) - 1U, &info) == 0,
           "short output buffer");
    expect(qxp_escape(0x9999, 0, NULL, sizeof(info), &info) == 0,
           "unknown escape");

    /* 0x2c plus the appended matches/match_handle pair. */
    expect(sizeof(source_result) == 0x34,
           "D3D9 source self-test ABI size");
    expect(sizeof(source_request) == 0x0c,
           "D3D9 source self-test request ABI size");
    expect(DrvEscape(NULL, QXP_QUERYESCSUPPORT, sizeof(source_query),
                     &source_query, 0, NULL) == 1,
           "source self-test support probe");
    expect(DrvEscape(NULL, QXP_QUERYESCSUPPORT, sizeof(query), &query,
                     0, NULL) == 1,
           "existing support probe still delegates");

    memset(&source_request, 0, sizeof(source_request));
    source_request.version = QXP_D3D9_SOURCE_SELFTEST_VERSION;
    source_request.byte_count = QXP_D3D9_SOURCE_SELFTEST_MAX_BYTES;
    memset(&source_result, 0xa5, sizeof(source_result));
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST,
                     sizeof(source_request), &source_request,
                     sizeof(source_result), &source_result) == 1,
           "exact source self-test output size");
    expect(source_selftest_calls == 1 &&
           source_result.version == QXP_D3D9_SOURCE_SELFTEST_VERSION &&
           source_result.surfaces_examined == 0x10203040U &&
           source_result.last_status == 0x50607080U,
           "source self-test dispatch result");

    memset(&source_result, 0xa5, sizeof(source_result));
    memcpy(&source_before, &source_result, sizeof(source_before));
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST,
                     sizeof(source_request), &source_request,
                     sizeof(source_result) - 1U, &source_result) == 0 &&
           source_selftest_calls == 1 &&
           memcmp(&source_result, &source_before,
                  sizeof(source_result)) == 0,
           "undersized source output rejected without write");

    memset(&oversized, 0x5a, sizeof(oversized));
    memcpy(&oversized_before, &oversized, sizeof(oversized_before));
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST,
                     sizeof(source_request), &source_request,
                     sizeof(source_result) + 1U, &oversized) == 0 &&
           source_selftest_calls == 1 &&
           memcmp(&oversized, &oversized_before, sizeof(oversized)) == 0,
           "oversized source output rejected without write");
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST,
                     sizeof(source_request), &source_request,
                     sizeof(source_result), NULL) == 0 &&
           source_selftest_calls == 1,
           "null exact source output rejected");

    memset(&source_result, 0xa5, sizeof(source_result));
    memcpy(&source_before, &source_result, sizeof(source_before));
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST, 0, NULL,
                     sizeof(source_result), &source_result) == 0 &&
           source_selftest_calls == 1 &&
           memcmp(&source_result, &source_before,
                  sizeof(source_result)) == 0,
           "missing source request rejected without write");

    memset(&source_result, 0xa5, sizeof(source_result));
    memcpy(&source_before, &source_result, sizeof(source_before));
    expect(DrvEscape(NULL, QXP_D3D9_SOURCE_SELFTEST,
                     sizeof(source_request) - 1U, &source_request,
                     sizeof(source_result), &source_result) == 0 &&
           source_selftest_calls == 1 &&
           memcmp(&source_result, &source_before,
                  sizeof(source_result)) == 0,
           "undersized source request rejected without write");

    expect(sizeof(sm2_request) == 0x04,
           "D3D9 SM2 link self-test request ABI size");
    expect(sizeof(sm2_result) == 0x28,
           "D3D9 SM2 link self-test result ABI size");
    expect(DrvEscape(NULL, QXP_QUERYESCSUPPORT, sizeof(sm2_query),
                     &sm2_query, 0, NULL) == 1,
           "SM2 link self-test support probe");

    memset(&sm2_request, 0, sizeof(sm2_request));
    sm2_request.version = QXP_D3D9_SM2_LINK_SELFTEST_VERSION;
    memset(&sm2_result, 0xa5, sizeof(sm2_result));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_request), &sm2_request,
                     sizeof(sm2_result), &sm2_result) == 1,
           "exact SM2 link self-test sizes");
    expect(sm2_link_selftest_calls == 1 &&
           sm2_result.version == QXP_D3D9_SM2_LINK_SELFTEST_VERSION &&
           sm2_result.stage == 0x11223344U &&
           sm2_result.vertex_glsl_bytes == 0x55667788U &&
           sm2_result.overall == 1,
           "SM2 link self-test dispatch result");

    memset(&sm2_result, 0xa5, sizeof(sm2_result));
    memcpy(&sm2_before, &sm2_result, sizeof(sm2_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_request), &sm2_request,
                     sizeof(sm2_result) - 1U, &sm2_result) == 0 &&
           sm2_link_selftest_calls == 1 &&
           memcmp(&sm2_result, &sm2_before, sizeof(sm2_result)) == 0,
           "undersized SM2 output rejected without write");

    memset(&sm2_oversized_output, 0x5a,
           sizeof(sm2_oversized_output));
    memcpy(&sm2_oversized_output_before, &sm2_oversized_output,
           sizeof(sm2_oversized_output_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_request), &sm2_request,
                     sizeof(sm2_result) + 1U,
                     &sm2_oversized_output) == 0 &&
           sm2_link_selftest_calls == 1 &&
           memcmp(&sm2_oversized_output, &sm2_oversized_output_before,
                  sizeof(sm2_oversized_output)) == 0,
           "oversized SM2 output rejected without write");
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_request), &sm2_request,
                     sizeof(sm2_result), NULL) == 0 &&
           sm2_link_selftest_calls == 1,
           "null exact SM2 output rejected");

    memset(&sm2_result, 0xa5, sizeof(sm2_result));
    memcpy(&sm2_before, &sm2_result, sizeof(sm2_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST, 0, NULL,
                     sizeof(sm2_result), &sm2_result) == 0 &&
           sm2_link_selftest_calls == 1 &&
           memcmp(&sm2_result, &sm2_before, sizeof(sm2_result)) == 0,
           "missing SM2 request rejected without write");

    memset(&sm2_result, 0xa5, sizeof(sm2_result));
    memcpy(&sm2_before, &sm2_result, sizeof(sm2_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_request) - 1U, &sm2_request,
                     sizeof(sm2_result), &sm2_result) == 0 &&
           sm2_link_selftest_calls == 1 &&
           memcmp(&sm2_result, &sm2_before, sizeof(sm2_result)) == 0,
           "undersized SM2 request rejected without write");

    memset(&sm2_oversized_input, 0,
           sizeof(sm2_oversized_input));
    sm2_oversized_input.request.version =
        QXP_D3D9_SM2_LINK_SELFTEST_VERSION;
    sm2_oversized_input.extra = 0x7b;
    memset(&sm2_result, 0xa5, sizeof(sm2_result));
    memcpy(&sm2_before, &sm2_result, sizeof(sm2_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_LINK_SELFTEST,
                     sizeof(sm2_oversized_input), &sm2_oversized_input,
                     sizeof(sm2_result), &sm2_result) == 0 &&
           sm2_link_selftest_calls == 1 &&
           memcmp(&sm2_result, &sm2_before, sizeof(sm2_result)) == 0,
           "oversized SM2 request rejected without write");

    expect(sizeof(sm2_draw_request) == 0x04,
           "D3D9 SM2 draw self-test request ABI size");
    expect(sizeof(sm2_draw_result) == 0x6c,
           "D3D9 SM2 draw self-test result ABI size");
    expect(DrvEscape(NULL, QXP_QUERYESCSUPPORT,
                     sizeof(sm2_draw_query), &sm2_draw_query,
                     0, NULL) == 1,
           "SM2 draw self-test support probe");

    memset(&sm2_draw_request, 0, sizeof(sm2_draw_request));
    sm2_draw_request.version = QXP_D3D9_SM2_DRAW_SELFTEST_VERSION;
    memset(&sm2_draw_result, 0xa5, sizeof(sm2_draw_result));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_request), &sm2_draw_request,
                     sizeof(sm2_draw_result), &sm2_draw_result) == 1,
           "exact SM2 draw self-test sizes");
    expect(sm2_draw_selftest_calls == 1 &&
           sm2_draw_result.version ==
               QXP_D3D9_SM2_DRAW_SELFTEST_VERSION &&
           sm2_draw_result.stage == 0x12345678U &&
           sm2_draw_result.inside_x == 10 &&
           sm2_draw_result.inside_y == 4 &&
           sm2_draw_result.expected_inside == 0xff2060dfU &&
           sm2_draw_result.observed_inside == 0xff2060dfU &&
           sm2_draw_result.overall == 1,
           "SM2 draw self-test dispatch result");

    memset(&sm2_draw_result, 0xa5, sizeof(sm2_draw_result));
    memcpy(&sm2_draw_before, &sm2_draw_result,
           sizeof(sm2_draw_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_request), &sm2_draw_request,
                     sizeof(sm2_draw_result) - 1U,
                     &sm2_draw_result) == 0 &&
           sm2_draw_selftest_calls == 1 &&
           memcmp(&sm2_draw_result, &sm2_draw_before,
                  sizeof(sm2_draw_result)) == 0,
           "undersized SM2 draw output rejected without write");

    memset(&sm2_draw_oversized_output, 0x5a,
           sizeof(sm2_draw_oversized_output));
    memcpy(&sm2_draw_oversized_output_before,
           &sm2_draw_oversized_output,
           sizeof(sm2_draw_oversized_output_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_request), &sm2_draw_request,
                     sizeof(sm2_draw_result) + 1U,
                     &sm2_draw_oversized_output) == 0 &&
           sm2_draw_selftest_calls == 1 &&
           memcmp(&sm2_draw_oversized_output,
                  &sm2_draw_oversized_output_before,
                  sizeof(sm2_draw_oversized_output)) == 0,
           "oversized SM2 draw output rejected without write");
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_request), &sm2_draw_request,
                     sizeof(sm2_draw_result), NULL) == 0 &&
           sm2_draw_selftest_calls == 1,
           "null exact SM2 draw output rejected");

    memset(&sm2_draw_result, 0xa5, sizeof(sm2_draw_result));
    memcpy(&sm2_draw_before, &sm2_draw_result,
           sizeof(sm2_draw_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     0, NULL, sizeof(sm2_draw_result),
                     &sm2_draw_result) == 0 &&
           sm2_draw_selftest_calls == 1 &&
           memcmp(&sm2_draw_result, &sm2_draw_before,
                  sizeof(sm2_draw_result)) == 0,
           "missing SM2 draw request rejected without write");

    memset(&sm2_draw_result, 0xa5, sizeof(sm2_draw_result));
    memcpy(&sm2_draw_before, &sm2_draw_result,
           sizeof(sm2_draw_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_request) - 1U,
                     &sm2_draw_request, sizeof(sm2_draw_result),
                     &sm2_draw_result) == 0 &&
           sm2_draw_selftest_calls == 1 &&
           memcmp(&sm2_draw_result, &sm2_draw_before,
                  sizeof(sm2_draw_result)) == 0,
           "undersized SM2 draw request rejected without write");

    memset(&sm2_draw_oversized_input, 0,
           sizeof(sm2_draw_oversized_input));
    sm2_draw_oversized_input.request.version =
        QXP_D3D9_SM2_DRAW_SELFTEST_VERSION;
    sm2_draw_oversized_input.extra = 0x7b;
    memset(&sm2_draw_result, 0xa5, sizeof(sm2_draw_result));
    memcpy(&sm2_draw_before, &sm2_draw_result,
           sizeof(sm2_draw_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_DRAW_SELFTEST,
                     sizeof(sm2_draw_oversized_input),
                     &sm2_draw_oversized_input,
                     sizeof(sm2_draw_result), &sm2_draw_result) == 0 &&
           sm2_draw_selftest_calls == 1 &&
           memcmp(&sm2_draw_result, &sm2_draw_before,
                  sizeof(sm2_draw_result)) == 0,
           "oversized SM2 draw request rejected without write");

    expect(sizeof(sm2_texture_request) == 0x04,
           "D3D9 SM2 texture self-test request ABI size");
    expect(sizeof(sm2_texture_result) == 0x360,
           "D3D9 SM2 texture self-test result ABI size");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_texture_defined) == 0x74,
           "D3D9 SM2 texture multi_texture_defined ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_texture_uploaded) == 0x78,
           "D3D9 SM2 texture multi_texture_uploaded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_vertex_decode_status) == 0x7c,
           "D3D9 SM2 texture multi_vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_vertex_glsl_bytes) == 0x80,
           "D3D9 SM2 texture multi_vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_pixel_decode_status) == 0x84,
           "D3D9 SM2 texture multi_pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_pixel_glsl_bytes) == 0x88,
           "D3D9 SM2 texture multi_pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_define_succeeded) == 0x8c,
           "D3D9 SM2 texture multi_define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_draw_succeeded) == 0x90,
           "D3D9 SM2 texture multi_draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_readback_succeeded) == 0x94,
           "D3D9 SM2 texture multi_readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_expected_sample0) == 0x98,
           "D3D9 SM2 texture multi_expected_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_observed_sample0) == 0x9c,
           "D3D9 SM2 texture multi_observed_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_expected_sample1) == 0xa0,
           "D3D9 SM2 texture multi_expected_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_observed_sample1) == 0xa4,
           "D3D9 SM2 texture multi_observed_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_program_deleted) == 0xa8,
           "D3D9 SM2 texture multi_program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_texture_deleted) == 0xac,
           "D3D9 SM2 texture multi_texture_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    multi_overall) == 0xb0,
           "D3D9 SM2 texture multi_overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_vertex_decode_status) == 0xb4,
           "D3D9 SM2 texture proj_vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_vertex_glsl_bytes) == 0xb8,
           "D3D9 SM2 texture proj_vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_pixel_decode_status) == 0xbc,
           "D3D9 SM2 texture proj_pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_pixel_glsl_bytes) == 0xc0,
           "D3D9 SM2 texture proj_pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_define_succeeded) == 0xc4,
           "D3D9 SM2 texture proj_define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_constants_succeeded) == 0xc8,
           "D3D9 SM2 texture proj_constants_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_draw_succeeded) == 0xcc,
           "D3D9 SM2 texture proj_draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_readback_succeeded) == 0xd0,
           "D3D9 SM2 texture proj_readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_expected_sample0) == 0xd4,
           "D3D9 SM2 texture proj_expected_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_observed_sample0) == 0xd8,
           "D3D9 SM2 texture proj_observed_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_expected_sample1) == 0xdc,
           "D3D9 SM2 texture proj_expected_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_observed_sample1) == 0xe0,
           "D3D9 SM2 texture proj_observed_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_program_deleted) == 0xe4,
           "D3D9 SM2 texture proj_program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    proj_overall) == 0xe8,
           "D3D9 SM2 texture proj_overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_vertex_decode_status) == 0xec,
           "D3D9 SM2 texture cube_vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_vertex_glsl_bytes) == 0xf0,
           "D3D9 SM2 texture cube_vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_pixel_decode_status) == 0xf4,
           "D3D9 SM2 texture cube_pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_pixel_glsl_bytes) == 0xf8,
           "D3D9 SM2 texture cube_pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_define_succeeded) == 0xfc,
           "D3D9 SM2 texture cube_define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_constants_succeeded) == 0x100,
           "D3D9 SM2 texture cube_constants_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_draw_succeeded) == 0x104,
           "D3D9 SM2 texture cube_draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_readback_succeeded) == 0x108,
           "D3D9 SM2 texture cube_readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_expected_sample0) == 0x10c,
           "D3D9 SM2 texture cube_expected_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_observed_sample0) == 0x110,
           "D3D9 SM2 texture cube_observed_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_expected_sample1) == 0x114,
           "D3D9 SM2 texture cube_expected_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_observed_sample1) == 0x118,
           "D3D9 SM2 texture cube_observed_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_program_deleted) == 0x11c,
           "D3D9 SM2 texture cube_program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    cube_overall) == 0x120,
           "D3D9 SM2 texture cube_overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_vertex_decode_status) == 0x124,
           "D3D9 SM2 texture depth_vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_vertex_glsl_bytes) == 0x128,
           "D3D9 SM2 texture depth_vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_pixel_decode_status) == 0x12c,
           "D3D9 SM2 texture depth_pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_pixel_glsl_bytes) == 0x130,
           "D3D9 SM2 texture depth_pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_define_succeeded) == 0x134,
           "D3D9 SM2 texture depth_define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_constants_succeeded) == 0x138,
           "D3D9 SM2 texture depth_constants_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_target_bound) == 0x13c,
           "D3D9 SM2 texture depth_target_bound ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_clear_succeeded) == 0x140,
           "D3D9 SM2 texture depth_clear_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_draw_succeeded) == 0x144,
           "D3D9 SM2 texture depth_draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_readback_succeeded) == 0x148,
           "D3D9 SM2 texture depth_readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_overlap_x) == 0x14c,
           "D3D9 SM2 texture depth_overlap_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_overlap_y) == 0x150,
           "D3D9 SM2 texture depth_overlap_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_far_only_x) == 0x154,
           "D3D9 SM2 texture depth_far_only_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_far_only_y) == 0x158,
           "D3D9 SM2 texture depth_far_only_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_expected_pass_a_overlap) == 0x15c,
           "D3D9 SM2 texture depth_expected_pass_a_overlap ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_observed_pass_a_overlap) == 0x160,
           "D3D9 SM2 texture depth_observed_pass_a_overlap ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_expected_pass_a_far) == 0x164,
           "D3D9 SM2 texture depth_expected_pass_a_far ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_observed_pass_a_far) == 0x168,
           "D3D9 SM2 texture depth_observed_pass_a_far ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_expected_pass_b_overlap) == 0x16c,
           "D3D9 SM2 texture depth_expected_pass_b_overlap ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_observed_pass_b_overlap) == 0x170,
           "D3D9 SM2 texture depth_observed_pass_b_overlap ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_expected_pass_b_far) == 0x174,
           "D3D9 SM2 texture depth_expected_pass_b_far ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_observed_pass_b_far) == 0x178,
           "D3D9 SM2 texture depth_observed_pass_b_far ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_program_deleted) == 0x17c,
           "D3D9 SM2 texture depth_program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_buffer_deleted) == 0x180,
           "D3D9 SM2 texture depth_buffer_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    depth_overall) == 0x184,
           "D3D9 SM2 texture depth_overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_vertex_decode_status) == 0x188,
           "D3D9 SM2 texture scissor_vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_vertex_glsl_bytes) == 0x18c,
           "D3D9 SM2 texture scissor_vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_pixel_decode_status) == 0x190,
           "D3D9 SM2 texture scissor_pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_pixel_glsl_bytes) == 0x194,
           "D3D9 SM2 texture scissor_pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_define_succeeded) == 0x198,
           "D3D9 SM2 texture scissor_define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_constants_succeeded) == 0x19c,
           "D3D9 SM2 texture scissor_constants_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_target_bound) == 0x1a0,
           "D3D9 SM2 texture scissor_target_bound ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_clear_succeeded) == 0x1a4,
           "D3D9 SM2 texture scissor_clear_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_draw_succeeded) == 0x1a8,
           "D3D9 SM2 texture scissor_draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_readback_succeeded) == 0x1ac,
           "D3D9 SM2 texture scissor_readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_excluded_x) == 0x1b0,
           "D3D9 SM2 texture scissor_excluded_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_excluded_y) == 0x1b4,
           "D3D9 SM2 texture scissor_excluded_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_included_x) == 0x1b8,
           "D3D9 SM2 texture scissor_included_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_included_y) == 0x1bc,
           "D3D9 SM2 texture scissor_included_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_expected_pass_a_excluded) == 0x1c0,
           "D3D9 SM2 texture scissor_expected_pass_a_excluded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_observed_pass_a_excluded) == 0x1c4,
           "D3D9 SM2 texture scissor_observed_pass_a_excluded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_expected_pass_a_included) == 0x1c8,
           "D3D9 SM2 texture scissor_expected_pass_a_included ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_observed_pass_a_included) == 0x1cc,
           "D3D9 SM2 texture scissor_observed_pass_a_included ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_expected_pass_b_excluded) == 0x1d0,
           "D3D9 SM2 texture scissor_expected_pass_b_excluded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_observed_pass_b_excluded) == 0x1d4,
           "D3D9 SM2 texture scissor_observed_pass_b_excluded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_expected_pass_b_included) == 0x1d8,
           "D3D9 SM2 texture scissor_expected_pass_b_included ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_observed_pass_b_included) == 0x1dc,
           "D3D9 SM2 texture scissor_observed_pass_b_included ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_program_deleted) == 0x1e0,
           "D3D9 SM2 texture scissor_program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    scissor_overall) == 0x1e4,
           "D3D9 SM2 texture scissor_overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_vertex_decode_status) == 0x1e8,
           "D3D9 SM2 texture stream offset VS decode ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_vertex_glsl_bytes) == 0x1ec,
           "D3D9 SM2 texture stream offset VS bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_pixel_decode_status) == 0x1f0,
           "D3D9 SM2 texture stream offset PS decode ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_pixel_glsl_bytes) == 0x1f4,
           "D3D9 SM2 texture stream offset PS bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_define_succeeded) == 0x1f8,
           "D3D9 SM2 texture stream offset define ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_constants_succeeded) == 0x1fc,
           "D3D9 SM2 texture stream offset constants ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_target_bound) == 0x200,
           "D3D9 SM2 texture stream offset target ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_clear_succeeded) == 0x204,
           "D3D9 SM2 texture stream offset clear ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_draw_succeeded) == 0x208,
           "D3D9 SM2 texture stream offset draw ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_readback_succeeded) == 0x20c,
           "D3D9 SM2 texture stream offset readback ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_stride) == 0x210,
           "D3D9 SM2 texture stream offset stride ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_buffer_bytes) == 0x214,
           "D3D9 SM2 texture stream offset buffer ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_pass_a_offset) == 0x218,
           "D3D9 SM2 texture stream offset pass A ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_pass_b_offset) == 0x21c,
           "D3D9 SM2 texture stream offset pass B ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_left_x) == 0x220,
           "D3D9 SM2 texture stream offset left X ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_left_y) == 0x224,
           "D3D9 SM2 texture stream offset left Y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_right_x) == 0x228,
           "D3D9 SM2 texture stream offset right X ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_right_y) == 0x22c,
           "D3D9 SM2 texture stream offset right Y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_expected_pass_a_left) == 0x230,
           "D3D9 SM2 texture stream offset expected A left ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_observed_pass_a_left) == 0x234,
           "D3D9 SM2 texture stream offset observed A left ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_expected_pass_a_right) == 0x238,
           "D3D9 SM2 texture stream offset expected A right ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_observed_pass_a_right) == 0x23c,
           "D3D9 SM2 texture stream offset observed A right ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_expected_pass_b_left) == 0x240,
           "D3D9 SM2 texture stream offset expected B left ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_observed_pass_b_left) == 0x244,
           "D3D9 SM2 texture stream offset observed B left ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_expected_pass_b_right) == 0x248,
           "D3D9 SM2 texture stream offset expected B right ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_observed_pass_b_right) == 0x24c,
           "D3D9 SM2 texture stream offset observed B right ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_program_deleted) == 0x250,
           "D3D9 SM2 texture stream offset delete ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    stream_offset_overall) == 0x254,
           "D3D9 SM2 texture stream offset overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_texture_defined) == 0x258,
           "D3D9 SM2 texture texldb texture_defined ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_texture_uploaded) == 0x25c,
           "D3D9 SM2 texture texldb texture_uploaded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_vertex_decode_status) == 0x260,
           "D3D9 SM2 texture texldb vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_vertex_glsl_bytes) == 0x264,
           "D3D9 SM2 texture texldb vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_pixel_decode_status) == 0x268,
           "D3D9 SM2 texture texldb pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_pixel_glsl_bytes) == 0x26c,
           "D3D9 SM2 texture texldb pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_define_succeeded) == 0x270,
           "D3D9 SM2 texture texldb define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_target_bound) == 0x274,
           "D3D9 SM2 texture texldb target_bound ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_sample_x) == 0x278,
           "D3D9 SM2 texture texldb sample_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_sample_y) == 0x27c,
           "D3D9 SM2 texture texldb sample_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_control_clear_succeeded) == 0x280,
           "D3D9 SM2 texture texldb control_clear ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_control_draw_succeeded) == 0x284,
           "D3D9 SM2 texture texldb control_draw ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_control_readback_succeeded) == 0x288,
           "D3D9 SM2 texture texldb control_readback ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_expected_control) == 0x28c,
           "D3D9 SM2 texture texldb expected_control ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_observed_control) == 0x290,
           "D3D9 SM2 texture texldb observed_control ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_biased_clear_succeeded) == 0x294,
           "D3D9 SM2 texture texldb biased_clear ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_biased_draw_succeeded) == 0x298,
           "D3D9 SM2 texture texldb biased_draw ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_biased_readback_succeeded) == 0x29c,
           "D3D9 SM2 texture texldb biased_readback ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_expected_biased) == 0x2a0,
           "D3D9 SM2 texture texldb expected_biased ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_observed_biased) == 0x2a4,
           "D3D9 SM2 texture texldb observed_biased ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_program_deleted) == 0x2a8,
           "D3D9 SM2 texture texldb program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_texture_deleted) == 0x2ac,
           "D3D9 SM2 texture texldb texture_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    texldb_overall) == 0x2b0,
           "D3D9 SM2 texture texldb overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture2_defined) == 0x2b4,
           "D3D9 SM2 texture four texture2 defined ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture2_uploaded) == 0x2b8,
           "D3D9 SM2 texture four texture2 uploaded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture3_defined) == 0x2bc,
           "D3D9 SM2 texture four texture3 defined ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture3_uploaded) == 0x2c0,
           "D3D9 SM2 texture four texture3 uploaded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_vertex_decode_status) == 0x2c4,
           "D3D9 SM2 texture four VS status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_vertex_glsl_bytes) == 0x2c8,
           "D3D9 SM2 texture four VS bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_pixel_decode_status) == 0x2cc,
           "D3D9 SM2 texture four PS status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_pixel_glsl_bytes) == 0x2d0,
           "D3D9 SM2 texture four PS bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_define_succeeded) == 0x2d4,
           "D3D9 SM2 texture four define ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_target_bound) == 0x2d8,
           "D3D9 SM2 texture four target ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_clear_succeeded) == 0x2dc,
           "D3D9 SM2 texture four clear ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_draw_succeeded) == 0x2e0,
           "D3D9 SM2 texture four draw ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_readback_succeeded) == 0x2e4,
           "D3D9 SM2 texture four readback ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_expected_sample0) == 0x2e8,
           "D3D9 SM2 texture four expected sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_observed_sample0) == 0x2ec,
           "D3D9 SM2 texture four observed sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_expected_sample1) == 0x2f0,
           "D3D9 SM2 texture four expected sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_observed_sample1) == 0x2f4,
           "D3D9 SM2 texture four observed sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_program_deleted) == 0x2f8,
           "D3D9 SM2 texture four program delete ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture2_deleted) == 0x2fc,
           "D3D9 SM2 texture four texture2 delete ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_texture3_deleted) == 0x300,
           "D3D9 SM2 texture four texture3 delete ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    four_overall) == 0x304,
           "D3D9 SM2 texture four overall ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_texture_defined) == 0x308,
           "D3D9 SM2 texture volume texture_defined ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_texture_uploaded) == 0x30c,
           "D3D9 SM2 texture volume texture_uploaded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_vertex_decode_status) == 0x310,
           "D3D9 SM2 texture volume vertex_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_vertex_glsl_bytes) == 0x314,
           "D3D9 SM2 texture volume vertex_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_pixel_decode_status) == 0x318,
           "D3D9 SM2 texture volume pixel_decode_status ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_pixel_glsl_bytes) == 0x31c,
           "D3D9 SM2 texture volume pixel_glsl_bytes ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_define_succeeded) == 0x320,
           "D3D9 SM2 texture volume define_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_target_bound) == 0x324,
           "D3D9 SM2 texture volume target_bound ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_clear_succeeded) == 0x328,
           "D3D9 SM2 texture volume clear_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_draw_succeeded) == 0x32c,
           "D3D9 SM2 texture volume draw_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_readback_succeeded) == 0x330,
           "D3D9 SM2 texture volume readback_succeeded ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_sample0_x) == 0x334,
           "D3D9 SM2 texture volume sample0_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_sample0_y) == 0x338,
           "D3D9 SM2 texture volume sample0_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_sample1_x) == 0x33c,
           "D3D9 SM2 texture volume sample1_x ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_sample1_y) == 0x340,
           "D3D9 SM2 texture volume sample1_y ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_expected_sample0) == 0x344,
           "D3D9 SM2 texture volume expected_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_observed_sample0) == 0x348,
           "D3D9 SM2 texture volume observed_sample0 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_expected_sample1) == 0x34c,
           "D3D9 SM2 texture volume expected_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_observed_sample1) == 0x350,
           "D3D9 SM2 texture volume observed_sample1 ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_program_deleted) == 0x354,
           "D3D9 SM2 texture volume program_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_texture_deleted) == 0x358,
           "D3D9 SM2 texture volume texture_deleted ABI offset");
    expect(offsetof(QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT,
                    volume_overall) == 0x35c,
           "D3D9 SM2 texture volume overall ABI offset");
    expect(DrvEscape(NULL, QXP_QUERYESCSUPPORT,
                     sizeof(sm2_texture_query), &sm2_texture_query,
                     0, NULL) == 1,
           "SM2 texture self-test support probe");

    memset(&sm2_texture_request, 0, sizeof(sm2_texture_request));
    sm2_texture_request.version = QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION;
    memset(&sm2_texture_result, 0xa5, sizeof(sm2_texture_result));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_request), &sm2_texture_request,
                     sizeof(sm2_texture_result),
                     &sm2_texture_result) == 1,
           "exact SM2 texture self-test sizes");
    expect(sm2_texture_selftest_calls == 1 &&
           sm2_texture_result.version ==
               QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION &&
           sm2_texture_result.stage ==
               QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE &&
           sm2_texture_result.sample0_x == 4 &&
           sm2_texture_result.sample0_y == 10 &&
           sm2_texture_result.expected_sample0 == 0xff459709U &&
           sm2_texture_result.observed_sample0 == 0xff459709U &&
           sm2_texture_result.sample1_x == 10 &&
           sm2_texture_result.sample1_y == 4 &&
           sm2_texture_result.expected_sample1 == 0xff8f3d11U &&
           sm2_texture_result.observed_sample1 == 0xff8f3d11U &&
           sm2_texture_result.overall == 1 &&
           sm2_texture_result.multi_expected_sample0 == 0xff55c749U &&
           sm2_texture_result.multi_observed_sample0 == 0xff55c749U &&
           sm2_texture_result.multi_expected_sample1 == 0xffbf4d51U &&
           sm2_texture_result.multi_observed_sample1 == 0xffbf4d51U &&
           sm2_texture_result.multi_overall == 1 &&
           sm2_texture_result.proj_expected_sample0 == 0xff459709U &&
           sm2_texture_result.proj_observed_sample0 == 0xff459709U &&
           sm2_texture_result.proj_expected_sample1 == 0xff8f3d11U &&
           sm2_texture_result.proj_observed_sample1 == 0xff8f3d11U &&
           sm2_texture_result.proj_overall == 1 &&
           sm2_texture_result.cube_expected_sample0 == 0xff2060a0U &&
           sm2_texture_result.cube_observed_sample0 == 0xff2060a0U &&
           sm2_texture_result.cube_expected_sample1 == 0xff2060a0U &&
           sm2_texture_result.cube_observed_sample1 == 0xff2060a0U &&
           sm2_texture_result.cube_overall == 1 &&
           sm2_texture_result.depth_overlap_x == 5 &&
           sm2_texture_result.depth_overlap_y == 5 &&
           sm2_texture_result.depth_far_only_x == 12 &&
           sm2_texture_result.depth_far_only_y == 12 &&
           sm2_texture_result.depth_expected_pass_a_overlap ==
               0xffdf9f20U &&
           sm2_texture_result.depth_observed_pass_a_overlap ==
               0xffdf9f20U &&
           sm2_texture_result.depth_expected_pass_a_far == 0xff2060dfU &&
           sm2_texture_result.depth_observed_pass_a_far == 0xff2060dfU &&
           sm2_texture_result.depth_expected_pass_b_overlap ==
               0xffdf9f20U &&
           sm2_texture_result.depth_observed_pass_b_overlap ==
               0xffdf9f20U &&
           sm2_texture_result.depth_expected_pass_b_far == 0xff2060dfU &&
           sm2_texture_result.depth_observed_pass_b_far == 0xff2060dfU &&
           sm2_texture_result.depth_overall == 1 &&
           sm2_texture_result.scissor_excluded_x == 4 &&
           sm2_texture_result.scissor_excluded_y == 10 &&
           sm2_texture_result.scissor_included_x == 10 &&
           sm2_texture_result.scissor_included_y == 4 &&
           sm2_texture_result.scissor_expected_pass_a_excluded ==
               0xff07131fU &&
           sm2_texture_result.scissor_observed_pass_a_excluded ==
               0xff07131fU &&
           sm2_texture_result.scissor_expected_pass_a_included ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_observed_pass_a_included ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_expected_pass_b_excluded ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_observed_pass_b_excluded ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_expected_pass_b_included ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_observed_pass_b_included ==
               0xffdf9f20U &&
           sm2_texture_result.scissor_overall == 1 &&
           sm2_texture_result.stream_offset_stride == 16 &&
           sm2_texture_result.stream_offset_buffer_bytes == 96 &&
           sm2_texture_result.stream_offset_pass_a_offset == 0 &&
           sm2_texture_result.stream_offset_pass_b_offset == 48 &&
           sm2_texture_result.stream_offset_left_x == 2 &&
           sm2_texture_result.stream_offset_left_y == 7 &&
           sm2_texture_result.stream_offset_right_x == 12 &&
           sm2_texture_result.stream_offset_right_y == 7 &&
           sm2_texture_result.stream_offset_expected_pass_a_left ==
               0xffdf9f20U &&
           sm2_texture_result.stream_offset_observed_pass_a_left ==
               0xffdf9f20U &&
           sm2_texture_result.stream_offset_expected_pass_a_right ==
               0xff07131fU &&
           sm2_texture_result.stream_offset_observed_pass_a_right ==
               0xff07131fU &&
           sm2_texture_result.stream_offset_expected_pass_b_left ==
               0xff07131fU &&
           sm2_texture_result.stream_offset_observed_pass_b_left ==
               0xff07131fU &&
           sm2_texture_result.stream_offset_expected_pass_b_right ==
               0xffdf9f20U &&
           sm2_texture_result.stream_offset_observed_pass_b_right ==
               0xffdf9f20U &&
           sm2_texture_result.stream_offset_overall == 1 &&
           sm2_texture_result.texldb_sample_x == 7 &&
           sm2_texture_result.texldb_sample_y == 7 &&
           sm2_texture_result.texldb_expected_control == 0xffe04020U &&
           sm2_texture_result.texldb_observed_control == 0xffe04020U &&
           sm2_texture_result.texldb_expected_biased == 0xff40a0e0U &&
           sm2_texture_result.texldb_observed_biased == 0xff40a0e0U &&
           sm2_texture_result.texldb_expected_control !=
               sm2_texture_result.texldb_expected_biased &&
           sm2_texture_result.texldb_overall == 1 &&
           sm2_texture_result.four_texture2_defined == 1 &&
           sm2_texture_result.four_texture2_uploaded == 1 &&
           sm2_texture_result.four_texture3_defined == 1 &&
           sm2_texture_result.four_texture3_uploaded == 1 &&
           sm2_texture_result.four_vertex_decode_status == 0 &&
           sm2_texture_result.four_vertex_glsl_bytes == 0x123U &&
           sm2_texture_result.four_pixel_decode_status == 0 &&
           sm2_texture_result.four_pixel_glsl_bytes == 0x234U &&
           sm2_texture_result.four_define_succeeded == 1 &&
           sm2_texture_result.four_target_bound == 1 &&
           sm2_texture_result.four_clear_succeeded == 1 &&
           sm2_texture_result.four_draw_succeeded == 1 &&
           sm2_texture_result.four_readback_succeeded == 1 &&
           sm2_texture_result.four_expected_sample0 == 0xff6ddb57U &&
           sm2_texture_result.four_observed_sample0 == 0xff6ddb57U &&
           sm2_texture_result.four_expected_sample1 == 0xffd7615fU &&
           sm2_texture_result.four_observed_sample1 == 0xffd7615fU &&
           sm2_texture_result.four_expected_sample0 !=
               sm2_texture_result.multi_expected_sample0 &&
           sm2_texture_result.four_expected_sample1 !=
               sm2_texture_result.multi_expected_sample1 &&
           sm2_texture_result.four_program_deleted == 1 &&
           sm2_texture_result.four_texture2_deleted == 1 &&
           sm2_texture_result.four_texture3_deleted == 1 &&
           sm2_texture_result.four_overall == 1 &&
           sm2_texture_result.volume_texture_defined == 1 &&
           sm2_texture_result.volume_texture_uploaded == 1 &&
           sm2_texture_result.volume_vertex_decode_status == 0 &&
           sm2_texture_result.volume_vertex_glsl_bytes == 0x345U &&
           sm2_texture_result.volume_pixel_decode_status == 0 &&
           sm2_texture_result.volume_pixel_glsl_bytes == 0x456U &&
           sm2_texture_result.volume_define_succeeded == 1 &&
           sm2_texture_result.volume_target_bound == 1 &&
           sm2_texture_result.volume_clear_succeeded == 1 &&
           sm2_texture_result.volume_draw_succeeded == 1 &&
           sm2_texture_result.volume_readback_succeeded == 1 &&
           sm2_texture_result.volume_sample0_x == 4 &&
           sm2_texture_result.volume_sample0_y == 7 &&
           sm2_texture_result.volume_sample1_x == 10 &&
           sm2_texture_result.volume_sample1_y == 7 &&
           sm2_texture_result.volume_expected_sample0 == 0xff23500aU &&
           sm2_texture_result.volume_observed_sample0 == 0xff23500aU &&
           sm2_texture_result.volume_expected_sample1 == 0xff835a2aU &&
           sm2_texture_result.volume_observed_sample1 == 0xff835a2aU &&
           sm2_texture_result.volume_expected_sample0 !=
               sm2_texture_result.volume_expected_sample1 &&
           sm2_texture_result.volume_program_deleted == 1 &&
           sm2_texture_result.volume_texture_deleted == 1 &&
           sm2_texture_result.volume_overall == 1,
           "SM2 texture self-test dispatch result");
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 1,
           "SM2 texture two-sample oracle accepts exact pair");
    sm2_texture_result.observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texture second sample oracle discriminates");
    sm2_texture_result.observed_sample1 ^= 1U;
    sm2_texture_result.version ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texture version oracle discriminates");
    sm2_texture_result.version ^= 1U;
    sm2_texture_result.multi_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 multi-texture overall oracle discriminates");
    sm2_texture_result.multi_overall = 1;
    sm2_texture_result.multi_observed_sample0 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 multi-texture first sample oracle discriminates");
    sm2_texture_result.multi_observed_sample0 ^= 1U;
    sm2_texture_result.multi_observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 multi-texture second sample oracle discriminates");
    sm2_texture_result.multi_observed_sample1 ^= 1U;
    sm2_texture_result.proj_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 projected texture overall oracle discriminates");
    sm2_texture_result.proj_overall = 1;
    sm2_texture_result.proj_observed_sample0 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 projected texture first sample oracle discriminates");
    sm2_texture_result.proj_observed_sample0 ^= 1U;
    sm2_texture_result.proj_observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 projected texture second sample oracle discriminates");
    sm2_texture_result.proj_observed_sample1 ^= 1U;
    sm2_texture_result.cube_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 cube texture overall oracle discriminates");
    sm2_texture_result.cube_overall = 1;
    sm2_texture_result.cube_observed_sample0 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 cube texture first sample oracle discriminates");
    sm2_texture_result.cube_observed_sample0 ^= 1U;
    sm2_texture_result.cube_observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 cube texture second sample oracle discriminates");
    sm2_texture_result.cube_observed_sample1 ^= 1U;
    sm2_texture_result.depth_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 depth overall oracle discriminates");
    sm2_texture_result.depth_overall = 1;
    sm2_texture_result.depth_observed_pass_a_overlap ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 depth pass A overlap oracle discriminates");
    sm2_texture_result.depth_observed_pass_a_overlap ^= 1U;
    sm2_texture_result.depth_observed_pass_a_far ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 depth pass A far oracle discriminates");
    sm2_texture_result.depth_observed_pass_a_far ^= 1U;
    sm2_texture_result.depth_observed_pass_b_overlap ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 depth pass B overlap oracle discriminates");
    sm2_texture_result.depth_observed_pass_b_overlap ^= 1U;
    sm2_texture_result.depth_observed_pass_b_far ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 depth pass B far oracle discriminates");
    sm2_texture_result.depth_observed_pass_b_far ^= 1U;
    sm2_texture_result.scissor_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 scissor overall oracle discriminates");
    sm2_texture_result.scissor_overall = 1;
    sm2_texture_result.scissor_observed_pass_a_excluded ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 scissor pass A excluded oracle discriminates");
    sm2_texture_result.scissor_observed_pass_a_excluded ^= 1U;
    sm2_texture_result.scissor_observed_pass_a_included ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 scissor pass A included oracle discriminates");
    sm2_texture_result.scissor_observed_pass_a_included ^= 1U;
    sm2_texture_result.scissor_observed_pass_b_excluded ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 scissor pass B excluded oracle discriminates");
    sm2_texture_result.scissor_observed_pass_b_excluded ^= 1U;
    sm2_texture_result.scissor_observed_pass_b_included ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 scissor pass B included oracle discriminates");
    sm2_texture_result.scissor_observed_pass_b_included ^= 1U;
    sm2_texture_result.stream_offset_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset overall oracle discriminates");
    sm2_texture_result.stream_offset_overall = 1;
    sm2_texture_result.stream_offset_observed_pass_a_left ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset pass A left oracle discriminates");
    sm2_texture_result.stream_offset_observed_pass_a_left ^= 1U;
    sm2_texture_result.stream_offset_observed_pass_a_right ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset pass A right oracle discriminates");
    sm2_texture_result.stream_offset_observed_pass_a_right ^= 1U;
    sm2_texture_result.stream_offset_observed_pass_b_left ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset pass B left oracle discriminates");
    sm2_texture_result.stream_offset_observed_pass_b_left ^= 1U;
    sm2_texture_result.stream_offset_observed_pass_b_right ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset pass B right oracle discriminates");
    sm2_texture_result.stream_offset_observed_pass_b_right ^= 1U;
    sm2_texture_result.stream_offset_expected_pass_b_left =
        sm2_texture_result.stream_offset_expected_pass_a_left;
    sm2_texture_result.stream_offset_observed_pass_b_left =
        sm2_texture_result.stream_offset_expected_pass_b_left;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 stream offset unequal-left oracle discriminates");
    sm2_texture_result.stream_offset_expected_pass_b_left = 0xff07131fU;
    sm2_texture_result.stream_offset_observed_pass_b_left = 0xff07131fU;

    sm2_texture_result.texldb_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texldb overall oracle discriminates");
    sm2_texture_result.texldb_overall = 1;
    sm2_texture_result.texldb_observed_control ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texldb control observed oracle discriminates");
    sm2_texture_result.texldb_observed_control ^= 1U;
    sm2_texture_result.texldb_observed_biased ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texldb biased observed oracle discriminates");
    sm2_texture_result.texldb_observed_biased ^= 1U;
    sm2_texture_result.texldb_expected_biased =
        sm2_texture_result.texldb_expected_control;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texldb equal-expected oracle discriminates");
    sm2_texture_result.texldb_expected_biased = 0xff40a0e0U;
    /* The strongest mutant: the biased pass observed the control's expected
     * level-0 red (bias ignored).  Even forging every broad overall bit must
     * not let the two-sided oracle pass, because observed biased no longer
     * equals expected biased. */
    sm2_texture_result.texldb_observed_biased =
        sm2_texture_result.texldb_expected_control;
    sm2_texture_result.texldb_overall = 1;
    sm2_texture_result.multi_overall = 1;
    sm2_texture_result.proj_overall = 1;
    sm2_texture_result.cube_overall = 1;
    sm2_texture_result.depth_overall = 1;
    sm2_texture_result.scissor_overall = 1;
    sm2_texture_result.stream_offset_overall = 1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 texldb biased-equals-control-expected oracle rejects");
    sm2_texture_result.texldb_observed_biased =
        sm2_texture_result.texldb_expected_biased;

    sm2_texture_result.four_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler overall oracle discriminates");
    sm2_texture_result.four_overall = 1;
    sm2_texture_result.four_observed_sample0 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler sample0 oracle discriminates");
    sm2_texture_result.four_observed_sample0 ^= 1U;
    sm2_texture_result.four_observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler sample1 oracle discriminates");
    sm2_texture_result.four_observed_sample1 ^= 1U;
    sm2_texture_result.four_expected_sample0 =
        sm2_texture_result.multi_expected_sample0;
    sm2_texture_result.four_observed_sample0 =
        sm2_texture_result.four_expected_sample0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler sample0 contribution discriminates");
    sm2_texture_result.four_expected_sample0 = 0xff6ddb57U;
    sm2_texture_result.four_observed_sample0 = 0xff6ddb57U;
    sm2_texture_result.four_expected_sample1 =
        sm2_texture_result.multi_expected_sample1;
    sm2_texture_result.four_observed_sample1 =
        sm2_texture_result.four_expected_sample1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler sample1 contribution discriminates");
    sm2_texture_result.four_expected_sample1 = 0xffd7615fU;
    sm2_texture_result.four_observed_sample1 = 0xffd7615fU;
    /* A self-consistent but wrong nonbaseline pair must not redefine the
     * oracle merely by forging both telemetry sides together. */
    sm2_texture_result.four_expected_sample0 = 0xff6ddb58U;
    sm2_texture_result.four_observed_sample0 = 0xff6ddb58U;
    sm2_texture_result.four_expected_sample1 = 0xffd76160U;
    sm2_texture_result.four_observed_sample1 = 0xffd76160U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler forged exact pair oracle rejects");
    sm2_texture_result.four_expected_sample0 = 0xff6ddb57U;
    sm2_texture_result.four_observed_sample0 = 0xff6ddb57U;
    sm2_texture_result.four_expected_sample1 = 0xffd7615fU;
    sm2_texture_result.four_observed_sample1 = 0xffd7615fU;
    /* Strongest mutant: both observed pixels are the exact two-sampler
     * baselines, as if stages 2/3 were ignored.  Forged broad overall bits do
     * not satisfy the explicit contribution and exact-pixel conditions. */
    sm2_texture_result.four_observed_sample0 =
        sm2_texture_result.multi_expected_sample0;
    sm2_texture_result.four_observed_sample1 =
        sm2_texture_result.multi_expected_sample1;
    sm2_texture_result.four_overall = 1;
    sm2_texture_result.texldb_overall = 1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 four-sampler two-stage fallback oracle rejects");
    sm2_texture_result.four_observed_sample0 =
        sm2_texture_result.four_expected_sample0;
    sm2_texture_result.four_observed_sample1 =
        sm2_texture_result.four_expected_sample1;

    /* Diagnostic volume (3D) phase oracle sensitivity.  The result is fully
     * valid again here, so each single mutation isolates one condition. */
    sm2_texture_result.stage ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume terminal stage oracle discriminates");
    sm2_texture_result.stage ^= 1U;
    sm2_texture_result.volume_overall = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume overall oracle discriminates");
    sm2_texture_result.volume_overall = 1;
    sm2_texture_result.volume_texture_defined = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume texture-defined oracle discriminates");
    sm2_texture_result.volume_texture_defined = 1;
    sm2_texture_result.volume_texture_uploaded = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume texture-uploaded oracle discriminates");
    sm2_texture_result.volume_texture_uploaded = 1;
    sm2_texture_result.volume_vertex_decode_status = 1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume vertex decode-status oracle discriminates");
    sm2_texture_result.volume_vertex_decode_status = 0;
    sm2_texture_result.volume_pixel_decode_status = 1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume pixel decode-status oracle discriminates");
    sm2_texture_result.volume_pixel_decode_status = 0;
    sm2_texture_result.volume_vertex_glsl_bytes = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume vertex GLSL bytes oracle discriminates");
    sm2_texture_result.volume_vertex_glsl_bytes = 0x345U;
    sm2_texture_result.volume_pixel_glsl_bytes = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume pixel GLSL bytes oracle discriminates");
    sm2_texture_result.volume_pixel_glsl_bytes = 0x456U;
    sm2_texture_result.volume_define_succeeded = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume define oracle discriminates");
    sm2_texture_result.volume_define_succeeded = 1;
    sm2_texture_result.volume_target_bound = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume target-bound oracle discriminates");
    sm2_texture_result.volume_target_bound = 1;
    sm2_texture_result.volume_clear_succeeded = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume clear oracle discriminates");
    sm2_texture_result.volume_clear_succeeded = 1;
    sm2_texture_result.volume_draw_succeeded = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume draw oracle discriminates");
    sm2_texture_result.volume_draw_succeeded = 1;
    sm2_texture_result.volume_readback_succeeded = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume readback oracle discriminates");
    sm2_texture_result.volume_readback_succeeded = 1;
    sm2_texture_result.volume_program_deleted = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume program-deleted oracle discriminates");
    sm2_texture_result.volume_program_deleted = 1;
    sm2_texture_result.volume_texture_deleted = 0;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume texture-deleted oracle discriminates");
    sm2_texture_result.volume_texture_deleted = 1;
    sm2_texture_result.volume_sample1_y ^= 2U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume sample coordinate oracle discriminates");
    sm2_texture_result.volume_sample1_y ^= 2U;
    sm2_texture_result.volume_observed_sample0 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume observed sample0 oracle discriminates");
    sm2_texture_result.volume_observed_sample0 ^= 1U;
    sm2_texture_result.volume_observed_sample1 ^= 1U;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume observed sample1 oracle discriminates");
    sm2_texture_result.volume_observed_sample1 ^= 1U;
    /* Forged expected values: move expected and observed together to a
     * self-consistent but nonbaseline pair.  The hardcoded oracle constants
     * reject it. */
    sm2_texture_result.volume_expected_sample0 = 0xff23500bU;
    sm2_texture_result.volume_observed_sample0 = 0xff23500bU;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume forged expected sample0 oracle rejects");
    sm2_texture_result.volume_expected_sample0 = 0xff23500aU;
    sm2_texture_result.volume_observed_sample0 = 0xff23500aU;
    sm2_texture_result.volume_expected_sample1 = 0xff835a2bU;
    sm2_texture_result.volume_observed_sample1 = 0xff835a2bU;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume forged expected sample1 oracle rejects");
    sm2_texture_result.volume_expected_sample1 = 0xff835a2aU;
    sm2_texture_result.volume_observed_sample1 = 0xff835a2aU;
    /* Strongest 2D/slice-zero fallback mutant: the second sample observes the
     * first sample's expected value, as if the interpolated depth were ignored
     * and both points read Z slice 0.  Forging every broad overall/deletion
     * bit must not rescue it, because observed sample1 no longer equals its
     * distinct expected value and the two expected depths are unequal. */
    sm2_texture_result.volume_observed_sample1 =
        sm2_texture_result.volume_expected_sample0;
    sm2_texture_result.volume_overall = 1;
    sm2_texture_result.volume_program_deleted = 1;
    sm2_texture_result.volume_texture_deleted = 1;
    sm2_texture_result.four_overall = 1;
    sm2_texture_result.texldb_overall = 1;
    expect(QxpD3d9Sm2TextureOraclePass(&sm2_texture_result) == 0,
           "SM2 volume slice-zero fallback oracle rejects");
    sm2_texture_result.volume_observed_sample1 =
        sm2_texture_result.volume_expected_sample1;

    memset(&sm2_texture_result, 0xa5, sizeof(sm2_texture_result));
    memcpy(&sm2_texture_before, &sm2_texture_result,
           sizeof(sm2_texture_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_request), &sm2_texture_request,
                     sizeof(sm2_texture_result) - 1U,
                     &sm2_texture_result) == 0 &&
           sm2_texture_selftest_calls == 1 &&
           memcmp(&sm2_texture_result, &sm2_texture_before,
                  sizeof(sm2_texture_result)) == 0,
           "undersized SM2 texture output rejected without write");

    memset(&sm2_texture_oversized_output, 0x5a,
           sizeof(sm2_texture_oversized_output));
    memcpy(&sm2_texture_oversized_output_before,
           &sm2_texture_oversized_output,
           sizeof(sm2_texture_oversized_output_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_request), &sm2_texture_request,
                     sizeof(sm2_texture_result) + 1U,
                     &sm2_texture_oversized_output) == 0 &&
           sm2_texture_selftest_calls == 1 &&
           memcmp(&sm2_texture_oversized_output,
                  &sm2_texture_oversized_output_before,
                  sizeof(sm2_texture_oversized_output)) == 0,
           "oversized SM2 texture output rejected without write");
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_request), &sm2_texture_request,
                     sizeof(sm2_texture_result), NULL) == 0 &&
           sm2_texture_selftest_calls == 1,
           "null exact SM2 texture output rejected");

    memset(&sm2_texture_result, 0xa5, sizeof(sm2_texture_result));
    memcpy(&sm2_texture_before, &sm2_texture_result,
           sizeof(sm2_texture_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     0, NULL, sizeof(sm2_texture_result),
                     &sm2_texture_result) == 0 &&
           sm2_texture_selftest_calls == 1 &&
           memcmp(&sm2_texture_result, &sm2_texture_before,
                  sizeof(sm2_texture_result)) == 0,
           "missing SM2 texture request rejected without write");

    memset(&sm2_texture_result, 0xa5, sizeof(sm2_texture_result));
    memcpy(&sm2_texture_before, &sm2_texture_result,
           sizeof(sm2_texture_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_request) - 1U,
                     &sm2_texture_request, sizeof(sm2_texture_result),
                     &sm2_texture_result) == 0 &&
           sm2_texture_selftest_calls == 1 &&
           memcmp(&sm2_texture_result, &sm2_texture_before,
                  sizeof(sm2_texture_result)) == 0,
           "undersized SM2 texture request rejected without write");

    memset(&sm2_texture_oversized_input, 0,
           sizeof(sm2_texture_oversized_input));
    sm2_texture_oversized_input.request.version =
        QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION;
    sm2_texture_oversized_input.extra = 0x7b;
    memset(&sm2_texture_result, 0xa5, sizeof(sm2_texture_result));
    memcpy(&sm2_texture_before, &sm2_texture_result,
           sizeof(sm2_texture_before));
    expect(DrvEscape(NULL, QXP_D3D9_SM2_TEXTURE_SELFTEST,
                     sizeof(sm2_texture_oversized_input),
                     &sm2_texture_oversized_input,
                     sizeof(sm2_texture_result),
                     &sm2_texture_result) == 0 &&
           sm2_texture_selftest_calls == 1 &&
           memcmp(&sm2_texture_result, &sm2_texture_before,
                  sizeof(sm2_texture_result)) == 0,
           "oversized SM2 texture request rejected without write");

    memset(&info, 0x3c, sizeof(info));
    expect(DrvEscape(NULL, QXP_OPENGL_GETINFO, 0, NULL,
                     sizeof(info), &info) == 1 &&
           info.version == QXP_ICD_INTERFACE_VERSION &&
           info.driver_version == QXP_ICD_DRIVER_VERSION,
           "existing info escape still delegates");
    expect(DrvEscape(NULL, 0x9999, 0, NULL, sizeof(info), &info) == 0,
           "unknown driver escape still delegates");

    if (!failed) printf("qxpescape: all contract checks passed\n");
    return failed;
}
