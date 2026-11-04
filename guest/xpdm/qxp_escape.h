#ifndef QXP_ESCAPE_H
#define QXP_ESCAPE_H

#ifdef _MSC_VER
typedef unsigned char qxp_u8;
typedef unsigned short qxp_u16;
typedef unsigned long qxp_u32;
#else
#include <stdint.h>
typedef uint8_t qxp_u8;
typedef uint16_t qxp_u16;
typedef uint32_t qxp_u32;
#endif

#define QXP_QUERYESCSUPPORT 8U
#define QXP_OPENGL_GETINFO 0x1101U
#define QXP_D3D9_SOURCE_SELFTEST 0x1102U
#define QXP_D3D9_SM2_LINK_SELFTEST 0x1103U
#define QXP_D3D9_SM2_DRAW_SELFTEST 0x1104U
#define QXP_D3D9_SM2_TEXTURE_SELFTEST 0x1105U
#define QXP_OPENGL_GETINFO_DRVNAME 0U
#define QXP_ICD_INTERFACE_VERSION 2U
#define QXP_ICD_DRIVER_VERSION 1U
#define QXP_MAX_PATH 260U

typedef struct qxp_opengl_info {
    qxp_u32 version;
    qxp_u32 driver_version;
    qxp_u16 driver_name[QXP_MAX_PATH + 1U];
} QXP_OPENGL_INFO;

#define QXP_D3D9_SOURCE_SELFTEST_VERSION 2U

/* Largest range the self-test will read from any one surface. */
#define QXP_D3D9_SOURCE_SELFTEST_MAX_BYTES 256U

/* The caller supplies the byte count and the checksum it expects, because the
 * D3D runtime creates several system-memory vertex buffers per process and the
 * caller cannot know which surface handle is its own.  The driver hashes that
 * exact range on every eligible surface and reports how many matched, which
 * proves it resolved a base address naming the caller's real buffer contents. */
typedef struct qxp_d3d9_source_selftest_request {
    qxp_u32 version;
    qxp_u32 byte_count;
    qxp_u32 expected_checksum;
} QXP_D3D9_SOURCE_SELFTEST_REQUEST;

typedef struct qxp_d3d9_source_selftest_result {
    qxp_u32 version;
    qxp_u32 surfaces_examined;
    qxp_u32 admitted;
    qxp_u32 refused;
    qxp_u32 revalidated;
    qxp_u32 released;
    qxp_u32 gate_reader_count;
    qxp_u32 first_handle;
    qxp_u32 first_byte_count;
    qxp_u32 first_checksum;
    qxp_u32 last_status;
    /* Surfaces whose hashed range equalled the requested checksum, and the
     * handle of the first such surface. */
    qxp_u32 matches;
    qxp_u32 match_handle;
} QXP_D3D9_SOURCE_SELFTEST_RESULT;

#define QXP_D3D9_SM2_LINK_SELFTEST_VERSION 1U

#define QXP_D3D9_SM2_LINK_STAGE_NONE 0U
#define QXP_D3D9_SM2_LINK_STAGE_WORKSPACE 1U
#define QXP_D3D9_SM2_LINK_STAGE_VS_DECODE 2U
#define QXP_D3D9_SM2_LINK_STAGE_VS_EMIT 3U
#define QXP_D3D9_SM2_LINK_STAGE_PS_DECODE 4U
#define QXP_D3D9_SM2_LINK_STAGE_PS_EMIT 5U
#define QXP_D3D9_SM2_LINK_STAGE_DEFINE 6U
#define QXP_D3D9_SM2_LINK_STAGE_DELETE 7U
#define QXP_D3D9_SM2_LINK_STAGE_COMPLETE 8U

/* Version is the only request input.  The result is fixed and pointer-free so
 * the same contract can be consumed by both driver and user-mode probes. */
typedef struct qxp_d3d9_sm2_link_selftest_request {
    qxp_u32 version;
} QXP_D3D9_SM2_LINK_SELFTEST_REQUEST;

typedef struct qxp_d3d9_sm2_link_selftest_result {
    qxp_u32 version;
    qxp_u32 context_found;
    qxp_u32 stage;
    qxp_u32 vertex_decode_status;
    qxp_u32 pixel_decode_status;
    qxp_u32 vertex_glsl_bytes;
    qxp_u32 pixel_glsl_bytes;
    qxp_u32 define_succeeded;
    qxp_u32 delete_succeeded;
    qxp_u32 overall;
} QXP_D3D9_SM2_LINK_SELFTEST_RESULT;

#define QXP_D3D9_SM2_DRAW_SELFTEST_VERSION 1U

#define QXP_D3D9_SM2_DRAW_STAGE_NONE 0U
#define QXP_D3D9_SM2_DRAW_STAGE_WORKSPACE 1U
#define QXP_D3D9_SM2_DRAW_STAGE_VS_DECODE 2U
#define QXP_D3D9_SM2_DRAW_STAGE_VS_EMIT 3U
#define QXP_D3D9_SM2_DRAW_STAGE_PS_DECODE 4U
#define QXP_D3D9_SM2_DRAW_STAGE_PS_EMIT 5U
#define QXP_D3D9_SM2_DRAW_STAGE_DEFINE 6U
#define QXP_D3D9_SM2_DRAW_STAGE_CONSTANTS 7U
#define QXP_D3D9_SM2_DRAW_STAGE_TEXTURE 8U
#define QXP_D3D9_SM2_DRAW_STAGE_TARGET 9U
#define QXP_D3D9_SM2_DRAW_STAGE_CLEAR 10U
#define QXP_D3D9_SM2_DRAW_STAGE_DRAW 11U
#define QXP_D3D9_SM2_DRAW_STAGE_READBACK 12U
#define QXP_D3D9_SM2_DRAW_STAGE_ORACLE 13U
#define QXP_D3D9_SM2_DRAW_STAGE_RESTORE_TARGET 14U
#define QXP_D3D9_SM2_DRAW_STAGE_DELETE_PROGRAM 15U
#define QXP_D3D9_SM2_DRAW_STAGE_DELETE_TEXTURE 16U
#define QXP_D3D9_SM2_DRAW_STAGE_COMPLETE 17U

/* Pointer-free escape ABI shared by the XP display driver and MinGW probe. */
typedef struct qxp_d3d9_sm2_draw_selftest_request {
    qxp_u32 version;
} QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST;

typedef struct qxp_d3d9_sm2_draw_selftest_result {
    qxp_u32 version;
    qxp_u32 context_found;
    qxp_u32 stage;
    qxp_u32 vertex_decode_status;
    qxp_u32 pixel_decode_status;
    qxp_u32 vertex_glsl_bytes;
    qxp_u32 pixel_glsl_bytes;
    qxp_u32 define_succeeded;
    qxp_u32 constants_succeeded;
    qxp_u32 texture_defined;
    qxp_u32 target_bound;
    qxp_u32 clear_succeeded;
    qxp_u32 draw_succeeded;
    qxp_u32 readback_succeeded;
    qxp_u32 inside_x;
    qxp_u32 inside_y;
    qxp_u32 outside_x;
    qxp_u32 outside_y;
    qxp_u32 expected_inside;
    qxp_u32 observed_inside;
    qxp_u32 expected_outside;
    qxp_u32 observed_outside;
    qxp_u32 target_restored;
    qxp_u32 program_deleted;
    qxp_u32 texture_deleted;
    qxp_u32 memory_restored;
    qxp_u32 overall;
} QXP_D3D9_SM2_DRAW_SELFTEST_RESULT;

#define QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION 10U

#define QXP_D3D9_SM2_TEXTURE_STAGE_NONE 0U
#define QXP_D3D9_SM2_TEXTURE_STAGE_WORKSPACE 1U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VS_DECODE 2U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VS_EMIT 3U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PS_DECODE 4U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PS_EMIT 5U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEFINE 6U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SAMPLE_TEXTURE 7U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SAMPLE_UPLOAD 8U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TARGET_TEXTURE 9U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TARGET 10U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CLEAR 11U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DRAW 12U
#define QXP_D3D9_SM2_TEXTURE_STAGE_READBACK 13U
#define QXP_D3D9_SM2_TEXTURE_STAGE_ORACLE 14U
#define QXP_D3D9_SM2_TEXTURE_STAGE_RESTORE_TARGET 15U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DELETE_PROGRAM 16U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DELETE_SAMPLE_TEXTURE 17U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DELETE_TARGET_TEXTURE 18U
#define QXP_D3D9_SM2_TEXTURE_STAGE_COMPLETE 19U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_TEXTURE 20U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_UPLOAD 21U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_VS_DECODE 22U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_VS_EMIT 23U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_PS_DECODE 24U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_PS_EMIT 25U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_DEFINE 26U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_CLEAR 27U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_DRAW 28U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_READBACK 29U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_ORACLE 30U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_DELETE_PROGRAM 31U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_DELETE_TEXTURE 32U
#define QXP_D3D9_SM2_TEXTURE_STAGE_MULTI_COMPLETE 33U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_VS_DECODE 34U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_VS_EMIT 35U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_PS_DECODE 36U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_PS_EMIT 37U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_DEFINE 38U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_CONSTANTS 39U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_CLEAR 40U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_DRAW 41U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_READBACK 42U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_ORACLE 43U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_DELETE_PROGRAM 44U
#define QXP_D3D9_SM2_TEXTURE_STAGE_PROJ_COMPLETE 45U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_TEXTURE 46U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_UPLOAD 47U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_VS_DECODE 48U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_VS_EMIT 49U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_PS_DECODE 50U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_PS_EMIT 51U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_DEFINE 52U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_CONSTANTS 53U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_CLEAR 54U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_DRAW 55U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_READBACK 56U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_ORACLE 57U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_DELETE_PROGRAM 58U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_DELETE_TEXTURE 59U
#define QXP_D3D9_SM2_TEXTURE_STAGE_CUBE_COMPLETE 60U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_VS_DECODE 61U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_VS_EMIT 62U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PS_DECODE 63U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PS_EMIT 64U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_DEFINE 65U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_TARGET 66U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_CLEAR 67U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_FAR_CONSTANTS 68U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_FAR_DRAW 69U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_NEAR_CONSTANTS 70U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_NEAR_DRAW 71U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_READBACK 72U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_A_ORACLE 73U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_CLEAR 74U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_NEAR_CONSTANTS 75U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_NEAR_DRAW 76U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_FAR_CONSTANTS 77U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_FAR_DRAW 78U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_READBACK 79U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_PASS_B_ORACLE 80U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_RESTORE_TARGET 81U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_DELETE_PROGRAM 82U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_DELETE_BUFFER 83U
#define QXP_D3D9_SM2_TEXTURE_STAGE_DEPTH_COMPLETE 84U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_VS_DECODE 85U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_VS_EMIT 86U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PS_DECODE 87U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PS_EMIT 88U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_DEFINE 89U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_TARGET 90U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_A_CLEAR 91U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_CONSTANTS 92U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_A_DRAW 93U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_A_READBACK 94U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_A_ORACLE 95U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_B_CLEAR 96U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_B_DRAW 97U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_B_READBACK 98U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_PASS_B_ORACLE 99U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_RESTORE_TARGET 100U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_DELETE_PROGRAM 101U
#define QXP_D3D9_SM2_TEXTURE_STAGE_SCISSOR_COMPLETE 102U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_VS_DECODE 103U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_VS_EMIT 104U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PS_DECODE 105U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PS_EMIT 106U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_DEFINE 107U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_TARGET 108U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_A_CLEAR 109U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_CONSTANTS 110U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_A_DRAW 111U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_A_READBACK 112U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_A_ORACLE 113U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_B_CLEAR 114U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_B_DRAW 115U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_B_READBACK 116U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_PASS_B_ORACLE 117U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_RESTORE_TARGET 118U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_DELETE_PROGRAM 119U
#define QXP_D3D9_SM2_TEXTURE_STAGE_STREAM_OFFSET_COMPLETE 120U
/* ps_2_0 texldb (D3DSI_TEXLD_BIAS) exact-pixel phase: a plain-texld control
 * pass and a biased pass on one mipmapped 2D texture, appended after the
 * stream-offset phase without renumbering any prior stage. */
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_TEXTURE 121U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_UPLOAD 122U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_VS_DECODE 123U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_VS_EMIT 124U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_PS_DECODE 125U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_PS_EMIT 126U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_DEFINE 127U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_TARGET 128U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_CONTROL_CLEAR 129U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_CONTROL_DRAW 130U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_CONTROL_READBACK 131U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_CONTROL_ORACLE 132U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_BIASED_CLEAR 133U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_BIASED_DRAW 134U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_BIASED_READBACK 135U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_BIASED_ORACLE 136U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_RESTORE_TARGET 137U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_DELETE_PROGRAM 138U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_DELETE_TEXTURE 139U
#define QXP_D3D9_SM2_TEXTURE_STAGE_TEXLDB_COMPLETE 140U
/* Four simultaneous 2D samplers.  Stages 0/1 reuse the established source
 * textures; stages 2/3 use distinct uniform textures and noncommutative shader
 * arithmetic so missing or swapped higher-stage bindings fail exact pixels. */
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_TEXTURE2 141U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_TEXTURE2_UPLOAD 142U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_TEXTURE3 143U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_TEXTURE3_UPLOAD 144U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_VS_DECODE 145U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_VS_EMIT 146U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_PS_DECODE 147U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_PS_EMIT 148U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_DEFINE 149U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_TARGET 150U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_CLEAR 151U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_DRAW 152U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_READBACK 153U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_ORACLE 154U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_RESTORE_TARGET 155U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_DELETE_PROGRAM 156U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_DELETE_TEXTURE2 157U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_DELETE_TEXTURE3 158U
#define QXP_D3D9_SM2_TEXTURE_STAGE_FOUR_COMPLETE 159U
/* Diagnostic volume (3D) texture exact-pixel phase, appended for result
 * version 10.  A genuine dcl_volume s0 / sampler3D texld program samples a
 * 4x4x4 A8R8G8B8 volume whose texels encode (x,y,z), so two draw pixels at the
 * same (u,v) but different interpolated depths must read two different Z
 * slices.  No prior stage is renumbered. */
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_TEXTURE 160U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_UPLOAD 161U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_VS_DECODE 162U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_VS_EMIT 163U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_PS_DECODE 164U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_PS_EMIT 165U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_DEFINE 166U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_TARGET 167U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_CLEAR 168U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_DRAW 169U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_READBACK 170U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_ORACLE 171U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_RESTORE_TARGET 172U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_DELETE_PROGRAM 173U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_DELETE_TEXTURE 174U
#define QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE 175U

/* Pointer-free texture execution oracle shared by driver and user probe. */
typedef struct qxp_d3d9_sm2_texture_selftest_request {
    qxp_u32 version;
} QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST;

typedef struct qxp_d3d9_sm2_texture_selftest_result {
    qxp_u32 version;
    qxp_u32 context_found;
    qxp_u32 stage;
    qxp_u32 vertex_decode_status;
    qxp_u32 pixel_decode_status;
    qxp_u32 vertex_glsl_bytes;
    qxp_u32 pixel_glsl_bytes;
    qxp_u32 define_succeeded;
    qxp_u32 sample_texture_defined;
    qxp_u32 sample_texture_uploaded;
    qxp_u32 target_texture_defined;
    qxp_u32 target_bound;
    qxp_u32 clear_succeeded;
    qxp_u32 draw_succeeded;
    qxp_u32 readback_succeeded;
    qxp_u32 sample0_x;
    qxp_u32 sample0_y;
    qxp_u32 expected_sample0;
    qxp_u32 observed_sample0;
    qxp_u32 sample1_x;
    qxp_u32 sample1_y;
    qxp_u32 expected_sample1;
    qxp_u32 observed_sample1;
    qxp_u32 target_restored;
    qxp_u32 program_deleted;
    qxp_u32 sample_texture_deleted;
    qxp_u32 target_texture_deleted;
    qxp_u32 memory_restored;
    qxp_u32 overall;
    qxp_u32 multi_texture_defined;
    qxp_u32 multi_texture_uploaded;
    qxp_u32 multi_vertex_decode_status;
    qxp_u32 multi_vertex_glsl_bytes;
    qxp_u32 multi_pixel_decode_status;
    qxp_u32 multi_pixel_glsl_bytes;
    qxp_u32 multi_define_succeeded;
    qxp_u32 multi_draw_succeeded;
    qxp_u32 multi_readback_succeeded;
    qxp_u32 multi_expected_sample0;
    qxp_u32 multi_observed_sample0;
    qxp_u32 multi_expected_sample1;
    qxp_u32 multi_observed_sample1;
    qxp_u32 multi_program_deleted;
    qxp_u32 multi_texture_deleted;
    qxp_u32 multi_overall;
    qxp_u32 proj_vertex_decode_status;
    qxp_u32 proj_vertex_glsl_bytes;
    qxp_u32 proj_pixel_decode_status;
    qxp_u32 proj_pixel_glsl_bytes;
    qxp_u32 proj_define_succeeded;
    qxp_u32 proj_constants_succeeded;
    qxp_u32 proj_draw_succeeded;
    qxp_u32 proj_readback_succeeded;
    qxp_u32 proj_expected_sample0;
    qxp_u32 proj_observed_sample0;
    qxp_u32 proj_expected_sample1;
    qxp_u32 proj_observed_sample1;
    qxp_u32 proj_program_deleted;
    qxp_u32 proj_overall;
    qxp_u32 cube_vertex_decode_status;
    qxp_u32 cube_vertex_glsl_bytes;
    qxp_u32 cube_pixel_decode_status;
    qxp_u32 cube_pixel_glsl_bytes;
    qxp_u32 cube_define_succeeded;
    qxp_u32 cube_constants_succeeded;
    qxp_u32 cube_draw_succeeded;
    qxp_u32 cube_readback_succeeded;
    qxp_u32 cube_expected_sample0;
    qxp_u32 cube_observed_sample0;
    qxp_u32 cube_expected_sample1;
    qxp_u32 cube_observed_sample1;
    qxp_u32 cube_program_deleted;
    qxp_u32 cube_overall;
    qxp_u32 depth_vertex_decode_status;
    qxp_u32 depth_vertex_glsl_bytes;
    qxp_u32 depth_pixel_decode_status;
    qxp_u32 depth_pixel_glsl_bytes;
    qxp_u32 depth_define_succeeded;
    qxp_u32 depth_constants_succeeded;
    qxp_u32 depth_target_bound;
    qxp_u32 depth_clear_succeeded;
    qxp_u32 depth_draw_succeeded;
    qxp_u32 depth_readback_succeeded;
    qxp_u32 depth_overlap_x;
    qxp_u32 depth_overlap_y;
    qxp_u32 depth_far_only_x;
    qxp_u32 depth_far_only_y;
    qxp_u32 depth_expected_pass_a_overlap;
    qxp_u32 depth_observed_pass_a_overlap;
    qxp_u32 depth_expected_pass_a_far;
    qxp_u32 depth_observed_pass_a_far;
    qxp_u32 depth_expected_pass_b_overlap;
    qxp_u32 depth_observed_pass_b_overlap;
    qxp_u32 depth_expected_pass_b_far;
    qxp_u32 depth_observed_pass_b_far;
    qxp_u32 depth_program_deleted;
    qxp_u32 depth_buffer_deleted;
    qxp_u32 depth_overall;
    qxp_u32 scissor_vertex_decode_status;
    qxp_u32 scissor_vertex_glsl_bytes;
    qxp_u32 scissor_pixel_decode_status;
    qxp_u32 scissor_pixel_glsl_bytes;
    qxp_u32 scissor_define_succeeded;
    qxp_u32 scissor_constants_succeeded;
    qxp_u32 scissor_target_bound;
    qxp_u32 scissor_clear_succeeded;
    qxp_u32 scissor_draw_succeeded;
    qxp_u32 scissor_readback_succeeded;
    qxp_u32 scissor_excluded_x;
    qxp_u32 scissor_excluded_y;
    qxp_u32 scissor_included_x;
    qxp_u32 scissor_included_y;
    qxp_u32 scissor_expected_pass_a_excluded;
    qxp_u32 scissor_observed_pass_a_excluded;
    qxp_u32 scissor_expected_pass_a_included;
    qxp_u32 scissor_observed_pass_a_included;
    qxp_u32 scissor_expected_pass_b_excluded;
    qxp_u32 scissor_observed_pass_b_excluded;
    qxp_u32 scissor_expected_pass_b_included;
    qxp_u32 scissor_observed_pass_b_included;
    qxp_u32 scissor_program_deleted;
    qxp_u32 scissor_overall;
    qxp_u32 stream_offset_vertex_decode_status;
    qxp_u32 stream_offset_vertex_glsl_bytes;
    qxp_u32 stream_offset_pixel_decode_status;
    qxp_u32 stream_offset_pixel_glsl_bytes;
    qxp_u32 stream_offset_define_succeeded;
    qxp_u32 stream_offset_constants_succeeded;
    qxp_u32 stream_offset_target_bound;
    qxp_u32 stream_offset_clear_succeeded;
    qxp_u32 stream_offset_draw_succeeded;
    qxp_u32 stream_offset_readback_succeeded;
    qxp_u32 stream_offset_stride;
    qxp_u32 stream_offset_buffer_bytes;
    qxp_u32 stream_offset_pass_a_offset;
    qxp_u32 stream_offset_pass_b_offset;
    qxp_u32 stream_offset_left_x;
    qxp_u32 stream_offset_left_y;
    qxp_u32 stream_offset_right_x;
    qxp_u32 stream_offset_right_y;
    qxp_u32 stream_offset_expected_pass_a_left;
    qxp_u32 stream_offset_observed_pass_a_left;
    qxp_u32 stream_offset_expected_pass_a_right;
    qxp_u32 stream_offset_observed_pass_a_right;
    qxp_u32 stream_offset_expected_pass_b_left;
    qxp_u32 stream_offset_observed_pass_b_left;
    qxp_u32 stream_offset_expected_pass_b_right;
    qxp_u32 stream_offset_observed_pass_b_right;
    qxp_u32 stream_offset_program_deleted;
    qxp_u32 stream_offset_overall;
    /* ps_2_0 texldb (D3DSI_TEXLD_BIAS) two-sided oracle telemetry.  Appended
     * after stream_offset_overall; older fields keep their offsets.  A control
     * pass with plain texld must observe level-0 red and a biased pass with
     * texldb (texcoord w=2) must observe level-2 blue on the same mipmapped 2D
     * texture, so the phase cannot pass if the LOD bias is ignored. */
    qxp_u32 texldb_texture_defined;
    qxp_u32 texldb_texture_uploaded;
    qxp_u32 texldb_vertex_decode_status;
    qxp_u32 texldb_vertex_glsl_bytes;
    qxp_u32 texldb_pixel_decode_status;
    qxp_u32 texldb_pixel_glsl_bytes;
    qxp_u32 texldb_define_succeeded;
    qxp_u32 texldb_target_bound;
    qxp_u32 texldb_sample_x;
    qxp_u32 texldb_sample_y;
    qxp_u32 texldb_control_clear_succeeded;
    qxp_u32 texldb_control_draw_succeeded;
    qxp_u32 texldb_control_readback_succeeded;
    qxp_u32 texldb_expected_control;
    qxp_u32 texldb_observed_control;
    qxp_u32 texldb_biased_clear_succeeded;
    qxp_u32 texldb_biased_draw_succeeded;
    qxp_u32 texldb_biased_readback_succeeded;
    qxp_u32 texldb_expected_biased;
    qxp_u32 texldb_observed_biased;
    qxp_u32 texldb_program_deleted;
    qxp_u32 texldb_texture_deleted;
    qxp_u32 texldb_overall;
    /* Four-sampler exact-pixel telemetry, appended for result version 9.  The
     * shader computes (s0 + s1 + s2) - s3, making the two new stage bindings
     * individually necessary and their order observable. */
    qxp_u32 four_texture2_defined;
    qxp_u32 four_texture2_uploaded;
    qxp_u32 four_texture3_defined;
    qxp_u32 four_texture3_uploaded;
    qxp_u32 four_vertex_decode_status;
    qxp_u32 four_vertex_glsl_bytes;
    qxp_u32 four_pixel_decode_status;
    qxp_u32 four_pixel_glsl_bytes;
    qxp_u32 four_define_succeeded;
    qxp_u32 four_target_bound;
    qxp_u32 four_clear_succeeded;
    qxp_u32 four_draw_succeeded;
    qxp_u32 four_readback_succeeded;
    qxp_u32 four_expected_sample0;
    qxp_u32 four_observed_sample0;
    qxp_u32 four_expected_sample1;
    qxp_u32 four_observed_sample1;
    qxp_u32 four_program_deleted;
    qxp_u32 four_texture2_deleted;
    qxp_u32 four_texture3_deleted;
    qxp_u32 four_overall;
    /* Diagnostic volume (3D) texture exact-pixel telemetry, appended for
     * result version 10.  Fields 0x308..0x35c; older fields keep their
     * offsets.  A genuine sampler3D texld program samples a 4x4x4 A8R8G8B8
     * volume at a fixed (u,v) but two different interpolated depths, so the
     * two observed pixels read two different Z slices and cannot be equal. */
    qxp_u32 volume_texture_defined;
    qxp_u32 volume_texture_uploaded;
    qxp_u32 volume_vertex_decode_status;
    qxp_u32 volume_vertex_glsl_bytes;
    qxp_u32 volume_pixel_decode_status;
    qxp_u32 volume_pixel_glsl_bytes;
    qxp_u32 volume_define_succeeded;
    qxp_u32 volume_target_bound;
    qxp_u32 volume_clear_succeeded;
    qxp_u32 volume_draw_succeeded;
    qxp_u32 volume_readback_succeeded;
    qxp_u32 volume_sample0_x;
    qxp_u32 volume_sample0_y;
    qxp_u32 volume_sample1_x;
    qxp_u32 volume_sample1_y;
    qxp_u32 volume_expected_sample0;
    qxp_u32 volume_observed_sample0;
    qxp_u32 volume_expected_sample1;
    qxp_u32 volume_observed_sample1;
    qxp_u32 volume_program_deleted;
    qxp_u32 volume_texture_deleted;
    qxp_u32 volume_overall;
} QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT;

qxp_u32 QxpD3d9Sm2TextureOraclePass(
    const QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT *result);

qxp_u32 qxp_escape(qxp_u32 escape, qxp_u32 input_bytes,
                   const void *input, qxp_u32 output_bytes, void *output);

#endif
