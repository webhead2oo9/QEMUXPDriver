#include "qxpmp.h"
#include "qxp_float.h"
#include "qxp_packet.h"
#include "qxp_program.h"
#include "qxp_texture_op.h"

static const QXP_RESOLUTION qxp_resolutions[QXP_MODE_CAPACITY] = {
    { 640, 480, 32 },
    { 800, 600, 32 },
    { 1024, 768, 32 },
    { 1280, 720, 32 },
    { 1280, 1024, 32 },
    { 1600, 900, 32 },
    { 1920, 1080, 32 },
    { 2048, 1536, 32 },
    { 640, 480, 16 },
    { 800, 600, 16 },
    { 1024, 768, 16 },
    { 1280, 720, 16 },
    { 1280, 1024, 16 },
    { 1600, 900, 16 },
    { 1920, 1080, 16 },
    { 2048, 1536, 16 },
    { 640, 480, 8 },
    { 800, 600, 8 },
    { 1024, 768, 8 },
    { 1280, 720, 8 },
    { 1280, 1024, 8 },
    { 1600, 900, 8 },
    { 1920, 1080, 8 },
    { 2048, 1536, 8 },
};

#define QXP_MESAGL_MAGIC 0x5b5eb5e5UL
#define QXP_MESA_PROBE_TAG 0x3dUL
#define QXP_MESA_PROBE_VERSION 1UL
#define QXP_D3D_SIGNATURE_VERSION 3UL
#define QXP_MESA_PROBE_FREE 0UL
#define QXP_MESA_PROBE_RESERVED 1UL
#define QXP_MESA_PROBE_OWNED 2UL

static BOOLEAN qxp_double_words_from_float(ULONG value, PULONG words);
#define QXP_MESA_FIRST_FIFO 24UL
#define QXP_MESA_MAX_FIFO 0x0c0000UL
#define QXP_D3D_DRAW_FIFO_RESERVE 0x00010000UL
#define QXP_D3D_DRAW_DATA_RESERVE 0x00006000UL
/* Asynchronous draw submission: ring 0xfb8 instead of 0xfc0 to hand the queued
 * pure-draw batch to the host without waiting on GL. Gated on the host
 * advertising QXP_MESA_ASYNC_CAP_V4 at the read-only capability register
 * 0xfac; older hosts return 0 there, so the guest keeps using 0xfc0. */
#define QXP_MESA_ASYNC_DRAW_DOORBELL 0xfb8UL
#define QXP_MESA_ASYNC_CAP_ADDR 0xfacUL
#define QXP_MESA_ASYNC_CAP_V4 0x41535904UL
#define QXP_MESA_MAX_DATA \
    ((QXP_MESA_FIFO_SIZE - 4UL * QXP_MESA_MAX_FIFO - 4UL * 4096UL) / 4UL)

/* Mesa FIFO ABI 1 ordinals from qemu-libretro's mglfunci.h. A host ordinal
 * change requires a protocol-version and QXPD3D signature-version bump. */
#define QXP_FENUM_GL_ACTIVE_TEXTURE 0x006UL
#define QXP_FENUM_GL_ATTACH_SHADER 0x019UL
#define QXP_FENUM_GL_BLIT_FRAMEBUFFER 0x083UL
#define QXP_FENUM_GL_CLEAR 0x09aUL
#define QXP_FENUM_GL_CLEAR_COLOR 0x0a3UL
#define QXP_FENUM_GL_CLEAR_DEPTH 0x0a7UL
#define QXP_FENUM_GL_CLEAR_STENCIL 0x0b5UL
#define QXP_FENUM_GL_ALPHA_FUNC 0x00cUL
#define QXP_FENUM_GL_BIND_FRAMEBUFFER 0x03cUL
#define QXP_FENUM_GL_BIND_ATTRIB_LOCATION 0x02aUL
#define QXP_FENUM_GL_BIND_PROGRAM_ARB 0x045UL
#define QXP_FENUM_GL_BIND_RENDERBUFFER 0x048UL
#define QXP_FENUM_GL_BIND_TEXTURE 0x04eUL
#define QXP_FENUM_GL_BLEND_EQUATION 0x06eUL
#define QXP_FENUM_GL_BLEND_FUNC 0x078UL
#define QXP_FENUM_GL_CLIENT_ACTIVE_TEXTURE 0x0b8UL
#define QXP_FENUM_GL_COMPILE_SHADER 0x113UL
#define QXP_FENUM_GL_COLOR_MASK 0x0f5UL
#define QXP_FENUM_GL_COLOR_MATERIAL 0x0f8UL
#define QXP_FENUM_GL_COLOR_POINTER 0x0fdUL
#define QXP_FENUM_GL_CHECK_FRAMEBUFFER_STATUS 0x094UL
#define QXP_FENUM_GL_CLIP_PLANE 0x0beUL
#define QXP_FENUM_GL_CULL_FACE 0x180UL
#define QXP_FENUM_GL_CREATE_PROGRAM 0x171UL
#define QXP_FENUM_GL_CREATE_SHADER 0x177UL
#define QXP_FENUM_GL_DEPTH_FUNC 0x1b9UL
#define QXP_FENUM_GL_DEPTH_MASK 0x1baUL
#define QXP_FENUM_GL_DEPTH_RANGE 0x1bbUL
#define QXP_FENUM_GL_DELETE_TEXTURES 0x1b0UL
#define QXP_FENUM_GL_DELETE_PROGRAM 0x1a2UL
#define QXP_FENUM_GL_DELETE_PROGRAMS_ARB 0x1a4UL
#define QXP_FENUM_GL_DELETE_RENDERBUFFERS 0x1a9UL
#define QXP_FENUM_GL_DELETE_SHADER 0x1adUL
#define QXP_FENUM_GL_DETACH_SHADER 0x1c3UL
#define QXP_FENUM_GL_DISABLE 0x1c5UL
#define QXP_FENUM_GL_DISABLE_CLIENT_STATE 0x1c6UL
#define QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB 0x1d1UL
#define QXP_FENUM_GL_DRAW_ARRAYS 0x1d5UL
#define QXP_FENUM_GL_DRAW_ELEMENTS 0x1e6UL
#define QXP_FENUM_GL_ENABLE 0x209UL
#define QXP_FENUM_GL_ENABLE_CLIENT_STATE 0x20aUL
#define QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB 0x215UL
#define QXP_FENUM_GL_FLUSH 0x245UL
#define QXP_FENUM_GL_FOG_COORD_POINTER 0x25aUL
#define QXP_FENUM_GL_FOG_F 0x25dUL
#define QXP_FENUM_GL_FOG_FV 0x25fUL
#define QXP_FENUM_GL_FOG_I 0x260UL
#define QXP_FENUM_GL_FRONT_FACE 0x28eUL
#define QXP_FENUM_GL_GET_STRING 0x405UL
#define QXP_FENUM_GL_FRAMEBUFFER_RENDERBUFFER 0x277UL
#define QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D 0x27fUL
#define QXP_FENUM_GL_GEN_FRAMEBUFFERS 0x29dUL
#define QXP_FENUM_GL_GEN_RENDERBUFFERS 0x2aaUL
#define QXP_FENUM_GL_GET_PROGRAM_INFO_LOG 0x3cbUL
#define QXP_FENUM_GL_GET_PROGRAM_IV 0x3cdUL
#define QXP_FENUM_GL_GET_SHADER_INFO_LOG 0x3fcUL
#define QXP_FENUM_GL_GET_SHADER_IV 0x3fdUL
#define QXP_FENUM_GL_GET_TEX_LEVEL_PARAMETER_IV 0x416UL
#define QXP_FENUM_GL_GET_UNIFORM_LOCATION 0x444UL
#define QXP_FENUM_GL_LOAD_IDENTITY 0x500UL
#define QXP_FENUM_GL_LOAD_MATRIX_F 0x503UL
#define QXP_FENUM_GL_LIGHT_F 0x4e9UL
#define QXP_FENUM_GL_LIGHT_FV 0x4eaUL
#define QXP_FENUM_GL_LIGHT_MODEL_FV 0x4eeUL
#define QXP_FENUM_GL_LIGHT_MODEL_I 0x4efUL
#define QXP_FENUM_GL_LINK_PROGRAM 0x4f8UL
#define QXP_FENUM_GL_MATRIX_MODE 0x549UL
#define QXP_FENUM_GL_MATERIAL_FV 0x537UL
#define QXP_FENUM_GL_MULT_MATRIX_F 0x600UL
#define QXP_FENUM_GL_NORMAL_POINTER 0x64dUL
#define QXP_FENUM_GL_ORTHO 0x65fUL
#define QXP_FENUM_GL_PIXEL_STORE_I 0x684UL
#define QXP_FENUM_GL_READ_PIXELS 0x7a4UL
#define QXP_FENUM_GL_GET_TEX_IMAGE 0x414UL
/* glGetTexImage packs the byte count it computed at the FBT base and starts
 * the image at the next eight-byte boundary; glReadPixels instead packs from
 * the base with no header. */
#define QXP_D3D_GET_TEX_IMAGE_OFFSET 8UL
#define QXP_FENUM_GL_BIND_BUFFER 0x02cUL
#define QXP_FENUM_GL_POINT_PARAMETER_F 0x697UL
#define QXP_FENUM_GL_POINT_PARAMETER_FV 0x69bUL
#define QXP_FENUM_GL_POINT_PARAMETER_I 0x69fUL
#define QXP_FENUM_GL_POINT_SIZE 0x6a4UL
#define QXP_FENUM_GL_POLYGON_MODE 0x6a8UL
#define QXP_FENUM_GL_POLYGON_OFFSET 0x6a9UL
#define QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB 0x6d0UL
#define QXP_FENUM_GL_PROGRAM_ENV_PARAMETER_4FV_ARB 0x6c5UL
#define QXP_FENUM_GL_PROGRAM_STRING_ARB 0x6e6UL
#define QXP_FENUM_GL_PROVOKING_VERTEX 0x773UL
#define QXP_FENUM_GL_RENDERBUFFER_STORAGE 0x7b2UL
#define QXP_FENUM_GL_SCISSOR 0x7f0UL
#define QXP_FENUM_GL_SECONDARY_COLOR_POINTER 0x81bUL
#define QXP_FENUM_GL_SHADE_MODEL 0x829UL
#define QXP_FENUM_GL_SHADER_SOURCE 0x82eUL
#define QXP_FENUM_GL_STENCIL_FUNC 0x844UL
#define QXP_FENUM_GL_STENCIL_MASK 0x847UL
#define QXP_FENUM_GL_STENCIL_OP 0x849UL
#define QXP_FENUM_GL_TEX_ENV_F 0x8c4UL
#define QXP_FENUM_GL_TEX_ENV_FV 0x8c5UL
#define QXP_FENUM_GL_TEX_ENV_I 0x8c6UL
#define QXP_FENUM_GL_TEX_GEN_FV 0x8ceUL
#define QXP_FENUM_GL_TEX_GEN_I 0x8cfUL
#define QXP_FENUM_GL_TEX_COORD_POINTER 0x8c0UL
#define QXP_FENUM_GL_TEX_IMAGE_2D 0x8d4UL
#define QXP_FENUM_GL_TEX_IMAGE_3D 0x8d7UL
#define QXP_FENUM_GL_TEX_PARAMETER_F 0x8ddUL
#define QXP_FENUM_GL_TEX_PARAMETER_I 0x8dfUL
#define QXP_FENUM_GL_TEX_SUB_IMAGE_2D 0x8f5UL
#define QXP_FENUM_GL_TEX_SUB_IMAGE_3D 0x8f7UL
#define QXP_FENUM_GL_UNIFORM_1I 0x944UL
#define QXP_FENUM_GL_UNIFORM_1IV 0x94aUL
#define QXP_FENUM_GL_UNIFORM_4FV 0x984UL
#define QXP_FENUM_GL_UNIFORM_4IV 0x98cUL
#define QXP_FENUM_GL_USE_PROGRAM 0x9bcUL
#define QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB 0xa65UL
#define QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB 0xaecUL
#define QXP_FENUM_GL_VERTEX_POINTER 0xb0aUL
#define QXP_FENUM_GL_VIEWPORT 0xb37UL

#define QXP_GL_COLOR_BUFFER_BIT 0x00004000UL
#define QXP_GL_DEPTH_BUFFER_BIT 0x00000100UL
#define QXP_GL_STENCIL_BUFFER_BIT 0x00000400UL
#define QXP_GL_SCISSOR_TEST 0x00000c11UL
#define QXP_GL_POINTS 0x00000000UL
#define QXP_GL_LINES 0x00000001UL
#define QXP_GL_TRIANGLES 0x00000004UL
#define QXP_GL_FRONT_AND_BACK 0x00000408UL
#define QXP_GL_BACK 0x00000405UL
#define QXP_GL_CW 0x00000900UL
#define QXP_GL_CCW 0x00000901UL
#define QXP_GL_FLAT 0x00001d00UL
#define QXP_GL_SMOOTH 0x00001d01UL
#define QXP_GL_POINT 0x00001b00UL
#define QXP_GL_POINT_SIZE_MIN 0x00008126UL
#define QXP_GL_POINT_SIZE_MAX 0x00008127UL
#define QXP_GL_POINT_FADE_THRESHOLD_SIZE 0x00008128UL
#define QXP_GL_POINT_DISTANCE_ATTENUATION 0x00008129UL
#define QXP_GL_POINT_SPRITE 0x00008861UL
#define QXP_GL_COORD_REPLACE 0x00008862UL
#define QXP_GL_POINT_SPRITE_COORD_ORIGIN 0x00008ca0UL
#define QXP_GL_LOWER_LEFT 0x00008ca1UL
#define QXP_GL_UPPER_LEFT 0x00008ca2UL
#define QXP_GL_LINE 0x00001b01UL
#define QXP_GL_FILL 0x00001b02UL
#define QXP_GL_NEVER 0x00000200UL
#define QXP_GL_ZERO 0x00000000UL
#define QXP_GL_ONE 0x00000001UL
#define QXP_GL_EXTENSIONS 0x00001f03UL
#define QXP_GL_ONE_MINUS_SRC_COLOR 0x00000301UL
#define QXP_GL_ONE_MINUS_SRC_ALPHA 0x00000303UL
#define QXP_GL_DST_ALPHA 0x00000304UL
#define QXP_GL_ONE_MINUS_DST_ALPHA 0x00000305UL
#define QXP_GL_DST_COLOR 0x00000306UL
#define QXP_GL_ONE_MINUS_DST_COLOR 0x00000307UL
#define QXP_GL_SRC_ALPHA_SATURATE 0x00000308UL
#define QXP_GL_BLEND 0x00000be2UL
#define QXP_GL_DITHER 0x00000bd0UL
#define QXP_GL_FOG 0x00000b60UL
#define QXP_GL_FOG_DENSITY 0x00000b62UL
#define QXP_GL_FOG_START 0x00000b63UL
#define QXP_GL_FOG_END 0x00000b64UL
#define QXP_GL_FOG_MODE 0x00000b65UL
#define QXP_GL_FOG_COLOR 0x00000b66UL
#define QXP_GL_EXP 0x00000800UL
#define QXP_GL_EXP2 0x00000801UL
#define QXP_GL_FOG_COORDINATE_SOURCE 0x00008450UL
#define QXP_GL_FOG_COORDINATE 0x00008451UL
#define QXP_GL_FRAGMENT_DEPTH 0x00008452UL
#define QXP_GL_FOG_DISTANCE_MODE_NV 0x0000855aUL
#define QXP_GL_EYE_RADIAL_NV 0x0000855bUL
#define QXP_GL_EYE_PLANE_ABSOLUTE_NV 0x0000855cUL
#define QXP_GL_FUNC_ADD 0x00008006UL
#define QXP_GL_MIN 0x00008007UL
#define QXP_GL_MAX 0x00008008UL
#define QXP_GL_FUNC_SUBTRACT 0x0000800aUL
#define QXP_GL_FUNC_REVERSE_SUBTRACT 0x0000800bUL
#define QXP_GL_COLOR_SUM 0x00008458UL
#define QXP_GL_ALPHA_TEST 0x00000bc0UL
#define QXP_GL_STENCIL_TEST 0x00000b90UL
#define QXP_GL_POLYGON_OFFSET_POINT 0x00002a01UL
#define QXP_GL_POLYGON_OFFSET_LINE 0x00002a02UL
#define QXP_GL_POLYGON_OFFSET_FILL 0x00008037UL
#define QXP_GL_CULL_FACE 0x00000b44UL
#define QXP_GL_DEPTH_TEST 0x00000b71UL
#define QXP_GL_LIGHTING 0x00000b50UL
#define QXP_GL_LIGHT_MODEL_LOCAL_VIEWER 0x00000b51UL
#define QXP_GL_LIGHT_MODEL_AMBIENT 0x00000b53UL
#define QXP_GL_COLOR_MATERIAL 0x00000b57UL
#define QXP_GL_NORMALIZE 0x00000ba1UL
#define QXP_GL_LIGHT0 0x00004000UL
#define QXP_GL_AMBIENT 0x00001200UL
#define QXP_GL_DIFFUSE 0x00001201UL
#define QXP_GL_SPECULAR 0x00001202UL
#define QXP_GL_POSITION 0x00001203UL
#define QXP_GL_SPOT_DIRECTION 0x00001204UL
#define QXP_GL_SPOT_EXPONENT 0x00001205UL
#define QXP_GL_SPOT_CUTOFF 0x00001206UL
#define QXP_GL_CONSTANT_ATTENUATION 0x00001207UL
#define QXP_GL_LINEAR_ATTENUATION 0x00001208UL
#define QXP_GL_QUADRATIC_ATTENUATION 0x00001209UL
#define QXP_GL_EMISSION 0x00001600UL
#define QXP_GL_SHININESS 0x00001601UL
#define QXP_GL_AMBIENT_AND_DIFFUSE 0x00001602UL
#define QXP_GL_LIGHT_MODEL_COLOR_CONTROL 0x000081f8UL
#define QXP_GL_SINGLE_COLOR 0x000081f9UL
#define QXP_GL_SEPARATE_SPECULAR_COLOR 0x000081faUL
#define QXP_GL_FRAGMENT_SHADER 0x00008b30UL
#define QXP_GL_VERTEX_SHADER 0x00008b31UL
#define QXP_GL_COMPILE_STATUS 0x00008b81UL
#define QXP_GL_LINK_STATUS 0x00008b82UL
#define QXP_D3DLIGHT_POINT 1UL
#define QXP_D3DLIGHT_SPOT 2UL
#define QXP_D3DLIGHT_DIRECTIONAL 3UL
#define QXP_D3DMCS_MATERIAL 0UL
#define QXP_D3DMCS_COLOR1 1UL
#define QXP_D3DMCS_COLOR2 2UL
#define QXP_GL_TEXTURE_2D 0x00000de1UL
#define QXP_GL_TEXTURE_3D 0x0000806fUL
#define QXP_GL_DEPTH_COMPONENT16 0x000081a5UL
#define QXP_GL_DEPTH_COMPONENT24 0x000081a6UL
#define QXP_GL_DEPTH24_STENCIL8 0x000088f0UL
#define QXP_GL_FRAMEBUFFER 0x00008d40UL
#define QXP_GL_READ_FRAMEBUFFER 0x00008ca8UL
#define QXP_GL_DRAW_FRAMEBUFFER 0x00008ca9UL
#define QXP_GL_RENDERBUFFER 0x00008d41UL
#define QXP_GL_COLOR_ATTACHMENT0 0x00008ce0UL
#define QXP_GL_DEPTH_ATTACHMENT 0x00008d00UL
#define QXP_GL_STENCIL_ATTACHMENT 0x00008d20UL
#define QXP_GL_FRAMEBUFFER_COMPLETE 0x00008cd5UL
#define QXP_GL_COLOR_BUFFER_BIT 0x00004000UL
#define QXP_GL_NEAREST 0x00002600UL
#define QXP_GL_TEXTURE_CUBE_MAP 0x00008513UL
#define QXP_GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x00008515UL
#define QXP_GL_TEXTURE0 0x000084c0UL
#define QXP_GL_S 0x00002000UL
#define QXP_GL_T 0x00002001UL
#define QXP_GL_R 0x00002002UL
#define QXP_GL_Q 0x00002003UL
#define QXP_GL_EYE_LINEAR 0x00002400UL
#define QXP_GL_EYE_PLANE 0x00002502UL
#define QXP_GL_TEXTURE_GEN_MODE 0x00002500UL
#define QXP_GL_TEXTURE_GEN_S 0x00000c60UL
#define QXP_GL_TEXTURE_GEN_T 0x00000c61UL
#define QXP_GL_TEXTURE_GEN_R 0x00000c62UL
#define QXP_GL_TEXTURE_GEN_Q 0x00000c63UL
#define QXP_GL_NORMAL_MAP 0x00008511UL
#define QXP_GL_REFLECTION_MAP 0x00008512UL
#define QXP_GL_FLOAT 0x00001406UL
#define QXP_GL_SHORT 0x00001402UL
#define QXP_GL_UNSIGNED_BYTE 0x00001401UL
#define QXP_GL_VERTEX_ARRAY 0x00008074UL
#define QXP_GL_COLOR_ARRAY 0x00008076UL
#define QXP_GL_TEXTURE_COORD_ARRAY 0x00008078UL
#define QXP_GL_NORMAL_ARRAY 0x00008075UL
#define QXP_GL_FOG_COORDINATE_ARRAY 0x00008457UL
#define QXP_GL_SECONDARY_COLOR_ARRAY 0x0000845eUL
#define QXP_GL_SECONDARY_COLOR_ATTRIBUTE 4UL
#define QXP_D3D_ARRAY_HANDLE_BASE 0x71000000UL
#define QXP_D3D_ARRAY_HANDLE_STRIDE 0x00010000UL
#define QXP_D3D_COLOR_ARRAY_HANDLE \
    (QXP_D3D_ARRAY_HANDLE_BASE + 0UL * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_TEXCOORD_ARRAY_HANDLE(stage) \
    (QXP_D3D_ARRAY_HANDLE_BASE + \
     (1UL + (stage)) * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_VERTEX_ARRAY_HANDLE \
    (QXP_D3D_ARRAY_HANDLE_BASE + 6UL * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_SECONDARY_ARRAY_HANDLE \
    (QXP_D3D_ARRAY_HANDLE_BASE + 7UL * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_FOG_ARRAY_HANDLE \
    (QXP_D3D_ARRAY_HANDLE_BASE + 8UL * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_NORMAL_ARRAY_HANDLE \
    (QXP_D3D_ARRAY_HANDLE_BASE + 9UL * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_PROGRAM_ARRAY_HANDLE_BASE 0x72000000UL
#define QXP_D3D_PROGRAM_ARRAY_HANDLE(attribute) \
    (QXP_D3D_PROGRAM_ARRAY_HANDLE_BASE + \
     (attribute) * QXP_D3D_ARRAY_HANDLE_STRIDE)
#define QXP_D3D_INTERLEAVED_ARRAY_HANDLE_BASE 0x73000000UL
#define QXP_D3D_INTERLEAVED_ARRAY_HANDLE(offset) \
    (QXP_D3D_INTERLEAVED_ARRAY_HANDLE_BASE + (offset))
#define QXP_D3D_TEXTURE_Q_PROGRAM 0x02000001UL
#define QXP_D3D_POSITIONAL_RANGE_PROGRAM 0x02000002UL
#define QXP_D3D_MULTI_POINT_RANGE_PROGRAM 0x02000003UL
#define QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM 0x02000004UL
#define QXP_D3D_QUAD_POINT_RANGE_PROGRAM 0x02000005UL
#define QXP_D3D_SCALABLE_POINT_PROGRAM 0x02000006UL
#define QXP_D3D_COMPACT_TRANSFORMED_PROGRAM 0x02000007UL
#define QXP_D3D_SPECULAR_ALPHA_PROGRAM 0x02000008UL
#define QXP_D3D_GLSL_UNIFORM_LIGHT_COUNT 0UL
#define QXP_D3D_GLSL_UNIFORM_RANGE0 1UL
#define QXP_D3D_GLSL_UNIFORM_RANGE1 2UL
#define QXP_D3D_GLSL_UNIFORM_PROJECTED 3UL
#define QXP_D3D_GLSL_UNIFORM_LITERAL 4UL
#define QXP_D3D_GLSL_UNIFORM_SOURCE 5UL
#define QXP_D3D_GLSL_UNIFORM_SECONDARY 6UL
#define QXP_D3D_GLSL_UNIFORM_NORMALIZE 7UL
#define QXP_D3D_GLSL_UNIFORM_MATERIAL_SPECULAR 8UL
#define QXP_D3D_GLSL_UNIFORM_FOG 9UL
#define QXP_D3D_GLSL_UNIFORM_TEXGEN 10UL
#define QXP_D3D_GLSL_UNIFORM_COUNT QXP_D3D_GLSL_UNIFORM_CAPACITY
/* Holds every live program across all devices.  The display driver caches a
 * whole frame's working set per context (128 entries) so it stops recompiling
 * shaders every frame, so this has to fit two such devices at once; each entry
 * is 120 bytes, so the table costs 61 KiB. */
#define QXP_D3D_SM2_PROGRAM_CAPACITY 512UL
/* Internal GLSL is submitted directly through the existing Mesa data ring.
 * Keep the public D3D program-string bound at 8 KiB while allowing this
 * audited fixed-function implementation its own still-small source ceiling. */
#define QXP_D3D_MAX_INTERNAL_GLSL_SOURCE_BYTES 16384UL
#define QXP_GL_TEXTURE_ENV 0x00002300UL
#define QXP_GL_TEXTURE_ENV_COLOR 0x00002201UL
#define QXP_GL_TEXTURE_ENV_MODE 0x00002200UL
#define QXP_GL_ALPHA_SCALE 0x00000d1cUL
#define QXP_GL_RGB_SCALE 0x00008573UL
#define QXP_GL_INTERPOLATE 0x00008575UL
#define QXP_GL_COMBINE 0x00008570UL
#define QXP_GL_COMBINE4_NV 0x00008503UL
#define QXP_GL_CONSTANT 0x00008576UL
#define QXP_GL_COMBINE_RGB 0x00008571UL
#define QXP_GL_DOT3_RGB 0x000086aeUL
#define QXP_GL_SUBTRACT 0x000084e7UL
#define QXP_GL_COMBINE_ALPHA 0x00008572UL
#define QXP_GL_SOURCE0_RGB 0x00008580UL
#define QXP_GL_SOURCE1_RGB 0x00008581UL
#define QXP_GL_SOURCE2_RGB 0x00008582UL
#define QXP_GL_SOURCE0_ALPHA 0x00008588UL
#define QXP_GL_SOURCE1_ALPHA 0x00008589UL
#define QXP_GL_SOURCE2_ALPHA 0x0000858aUL
#define QXP_GL_SOURCE3_RGB 0x00008583UL
#define QXP_GL_SOURCE3_ALPHA 0x0000858bUL
#define QXP_GL_OPERAND0_RGB 0x00008590UL
#define QXP_GL_OPERAND1_RGB 0x00008591UL
#define QXP_GL_OPERAND2_RGB 0x00008592UL
#define QXP_GL_OPERAND0_ALPHA 0x00008598UL
#define QXP_GL_OPERAND1_ALPHA 0x00008599UL
#define QXP_GL_OPERAND2_ALPHA 0x0000859aUL
#define QXP_GL_OPERAND3_RGB 0x00008593UL
#define QXP_GL_OPERAND3_ALPHA 0x0000859bUL
#define QXP_GL_TEXTURE 0x00001702UL
#define QXP_GL_PRIMARY_COLOR 0x00008577UL
#define QXP_GL_PREVIOUS 0x00008578UL
#define QXP_GL_SRC_COLOR 0x00000300UL
#define QXP_GL_SRC_ALPHA 0x00000302UL
#define QXP_GL_REPLACE 0x00001e01UL
#define QXP_GL_MODULATE 0x00002100UL
#define QXP_GL_KEEP 0x00001e00UL
#define QXP_GL_INCR 0x00001e02UL
#define QXP_GL_DECR 0x00001e03UL
#define QXP_GL_INVERT 0x0000150aUL
#define QXP_GL_NEAREST 0x00002600UL
#define QXP_GL_LINEAR 0x00002601UL
#define QXP_GL_NEAREST_MIPMAP_NEAREST 0x00002700UL
#define QXP_GL_LINEAR_MIPMAP_NEAREST 0x00002701UL
#define QXP_GL_NEAREST_MIPMAP_LINEAR 0x00002702UL
#define QXP_GL_LINEAR_MIPMAP_LINEAR 0x00002703UL
#define QXP_GL_REPEAT 0x00002901UL
#define QXP_GL_MIRRORED_REPEAT 0x00008370UL
#define QXP_GL_CLAMP_TO_EDGE 0x0000812fUL
#define QXP_GL_TEXTURE_MAG_FILTER 0x00002800UL
#define QXP_GL_TEXTURE_MIN_FILTER 0x00002801UL
#define QXP_GL_TEXTURE_WRAP_S 0x00002802UL
#define QXP_GL_TEXTURE_WRAP_T 0x00002803UL
#define QXP_GL_TEXTURE_WRAP_R 0x00008072UL
#define QXP_GL_TEXTURE_BASE_LEVEL 0x0000813cUL
#define QXP_GL_TEXTURE_MAX_LEVEL 0x0000813dUL
#define QXP_GL_TEXTURE_FILTER_CONTROL 0x00008500UL
#define QXP_GL_TEXTURE_LOD_BIAS 0x00008501UL
#define QXP_GL_UNPACK_ALIGNMENT 0x00000cf5UL
#define QXP_GL_UNPACK_ROW_LENGTH 0x00000cf2UL
#define QXP_GL_UNPACK_SKIP_ROWS 0x00000cf3UL
#define QXP_GL_UNPACK_SKIP_PIXELS 0x00000cf4UL
#define QXP_GL_UNPACK_IMAGE_HEIGHT 0x0000806eUL
#define QXP_GL_UNPACK_SKIP_IMAGES 0x0000806dUL
#define QXP_GL_UNSIGNED_BYTE 0x00001401UL
#define QXP_GL_UNSIGNED_BYTE_3_3_2 0x00008032UL
#define QXP_GL_UNSIGNED_SHORT_5_6_5 0x00008363UL
#define QXP_GL_UNSIGNED_SHORT_4_4_4_4_REV 0x00008365UL
#define QXP_GL_UNSIGNED_SHORT_1_5_5_5_REV 0x00008366UL
#define QXP_GL_ALPHA 0x00001906UL
#define QXP_GL_RGB 0x00001907UL
#define QXP_GL_RGBA 0x00001908UL
#define QXP_GL_LUMINANCE 0x00001909UL
#define QXP_GL_LUMINANCE_ALPHA 0x0000190aUL
#define QXP_GL_BGR 0x000080e0UL
#define QXP_GL_BGRA 0x000080e1UL
#define QXP_GL_PACK_SWAP_BYTES 0x00000d00UL
#define QXP_GL_PACK_ROW_LENGTH 0x00000d02UL
#define QXP_GL_PACK_SKIP_ROWS 0x00000d03UL
#define QXP_GL_PACK_SKIP_PIXELS 0x00000d04UL
#define QXP_GL_PACK_ALIGNMENT 0x00000d05UL
#define QXP_GL_PIXEL_PACK_BUFFER 0x000088ebUL
#define QXP_GL_RGB8 0x00008051UL
#define QXP_GL_RGBA8 0x00008058UL
#define QXP_GL_ALPHA8 0x0000803cUL
#define QXP_GL_TEXTURE_RED_SIZE 0x0000805cUL
#define QXP_GL_TEXTURE_GREEN_SIZE 0x0000805dUL
#define QXP_GL_TEXTURE_BLUE_SIZE 0x0000805eUL
#define QXP_GL_TEXTURE_ALPHA_SIZE 0x0000805fUL
#define QXP_GL_TEXTURE_LUMINANCE_SIZE 0x00008060UL
#define QXP_GL_TEXTURE_INTERNAL_FORMAT 0x00001003UL
#define QXP_GL_LUMINANCE8 0x00008040UL
#define QXP_GL_LUMINANCE8_ALPHA8 0x00008045UL
#define QXP_GL_MODELVIEW 0x00001700UL
#define QXP_GL_PROJECTION 0x00001701UL
#define QXP_GL_CLIP_PLANE0 0x00003000UL
#define QXP_GL_FRAGMENT_PROGRAM_ARB 0x00008804UL
#define QXP_GL_VERTEX_PROGRAM_ARB 0x00008620UL
#define QXP_GL_VERTEX_PROGRAM_POINT_SIZE_ARB 0x00008642UL
#define QXP_GL_PROGRAM_FORMAT_ASCII_ARB 0x00008875UL
#define QXP_GL_FIRST_VERTEX_CONVENTION 0x00008e4dUL

#define QXP_D3DFVF_NORMAL 0x00000010UL
#define QXP_D3DFVF_PSIZE 0x00000020UL
#define QXP_D3DFVF_DIFFUSE 0x00000040UL
#define QXP_D3DFVF_SPECULAR 0x00000080UL
#define QXP_D3DFVF_FOG 0x00002000UL
#define QXP_D3DFMT_R8G8B8 20UL
#define QXP_D3DFMT_A8R8G8B8 21UL
#define QXP_D3DFMT_X8R8G8B8 22UL
#define QXP_D3DFMT_R5G6B5 23UL
#define QXP_D3DFMT_X1R5G5B5 24UL
#define QXP_D3DFMT_A1R5G5B5 25UL
#define QXP_D3DFMT_A4R4G4B4 26UL
#define QXP_D3DFMT_R3G3B2 27UL
#define QXP_D3DFMT_A8 28UL
#define QXP_D3DFMT_X4R4G4B4 30UL
#define QXP_D3DFMT_L8 50UL
#define QXP_D3DFMT_A8L8 51UL
#define QXP_D3DFMT_D24S8 75UL
#define QXP_D3DFMT_D24X8 77UL
#define QXP_D3DFMT_D16 80UL

#define QXP_PFD_DOUBLEBUFFER 0x00000001UL
#define QXP_PFD_DRAW_TO_WINDOW 0x00000004UL
#define QXP_PFD_SUPPORT_OPENGL 0x00000020UL

typedef struct qxp_pixel_format_descriptor {
    USHORT nSize;
    USHORT nVersion;
    ULONG dwFlags;
    UCHAR iPixelType;
    UCHAR cColorBits;
    UCHAR cRedBits;
    UCHAR cRedShift;
    UCHAR cGreenBits;
    UCHAR cGreenShift;
    UCHAR cBlueBits;
    UCHAR cBlueShift;
    UCHAR cAlphaBits;
    UCHAR cAlphaShift;
    UCHAR cAccumBits;
    UCHAR cAccumRedBits;
    UCHAR cAccumGreenBits;
    UCHAR cAccumBlueBits;
    UCHAR cAccumAlphaBits;
    UCHAR cDepthBits;
    UCHAR cStencilBits;
    UCHAR cAuxBuffers;
    UCHAR iLayerType;
    UCHAR bReserved;
    ULONG dwLayerMask;
    ULONG dwVisibleMask;
    ULONG dwDamageMask;
} QXP_PIXEL_FORMAT_DESCRIPTOR;

typedef char QXP_PFD_SIZE_MUST_BE_40[
    sizeof(QXP_PIXEL_FORMAT_DESCRIPTOR) == 40 ? 1 : -1];

typedef struct qxp_d3d_sm2_program_state {
    QXP_DEVICE_EXTENSION *device;
    ULONG process_id;
    ULONG program_name;
    ULONG vertex_shader;
    ULONG pixel_shader;
    ULONG gl_program;
    LONG vertex_constants_location;
    LONG vertex_clip_control_location;
    /* Negative unless the program is a synthesized pre-transformed stage,
     * which needs the viewport to turn screen pixels into clip space. */
    LONG vertex_positiont_location;
    LONG vertex_integer_constants_location;
    LONG vertex_boolean_constants_location;
    LONG pixel_constants_location;
    LONG pixel_fog_location;
    /* Negative unless the synthesized combiner reads D3DRS_TEXTUREFACTOR.
     * Feeding it per draw keeps an animated factor from redefining the
     * program on every colour change. */
    LONG pixel_texture_factor_location;
    ULONG sampler_mask;
    ULONG cube_sampler_mask;
    ULONG volume_sampler_mask;
    LONG sampler_locations[QXP_D3D_SM2_SAMPLER_COUNT];
} QXP_D3D_SM2_PROGRAM_STATE;

static QXP_D3D_SM2_PROGRAM_STATE
    qxp_d3d_sm2_programs[QXP_D3D_SM2_PROGRAM_CAPACITY];

static VIDEO_ACCESS_RANGE qxp_legacy_resources[] = {
    { { { 0x000003B0, 0x00000000 } }, 0x0000000C, 1, 1, 1, 0 },
    { { { 0x000003C0, 0x00000000 } }, 0x00000020, 1, 1, 1, 0 },
    { { { 0x000A0000, 0x00000000 } }, 0x00020000, 0, 0, 1, 0 },
};

/* QEMU's debugcon answers a read with 0xE9; an unassigned port floats to
 * 0xFF.  Probing once lets an ordinary run drop -debugcon and pay nothing:
 * every character below is an I/O port write, and a port write is a VM exit
 * that costs far more than the work it describes.  Racing probes compute the
 * same answer, so the unsynchronised store is deliberate. */
static LONG qxp_debug_state;

static BOOLEAN qxp_debug_present(void)
{
    LONG state = qxp_debug_state;

    if (!state) {
        state = (VideoPortReadPortUchar((PUCHAR)(ULONG_PTR)0xE9) == 0xE9)
                    ? 1 : -1;
        qxp_debug_state = state;
    }
    return (BOOLEAN)(state > 0);
}

static void qxp_debug(const char *message)
{
    if (!qxp_debug_present()) return;
    while (*message) {
        VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9, (UCHAR)*message++);
    }
    VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9, '\n');
}

static void qxp_debug_status(ULONG status)
{
    static const char digits[] = "0123456789ABCDEF";
    LONG shift;

    if (!qxp_debug_present()) return;
    qxp_debug("QXPMP STATUS");
    VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9, '0');
    VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9, 'x');
    for (shift = 28; shift >= 0; shift -= 4) {
        VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9,
                                digits[(status >> shift) & 0xF]);
    }
    VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xE9, '\n');
}

#ifdef QXP_D3D_PROFILE
static void qxp_d3d_profile_add(ULONGLONG *total, LONGLONG ticks)
{
    ULONGLONG maximum = ~(ULONGLONG)0;
    ULONGLONG value;

    if (!total || ticks <= 0) return;
    value = (ULONGLONG)ticks;
    if (*total > maximum - value)
        *total = maximum;
    else
        *total += value;
}

static void qxp_d3d_profile_status64(ULONGLONG value)
{
    qxp_debug_status((ULONG)(value >> 32));
    qxp_debug_status((ULONG)value);
}

static void qxp_d3d_profile_finish_draw(
    QXP_DEVICE_EXTENSION *device, LONGLONG start_ticks)
{
    LONGLONG finish_ticks;

    finish_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    qxp_d3d_profile_add(&device->d3d_profile_draw_ticks,
                        finish_ticks - start_ticks);
}

/* Separates a slow guest CPU from a slow trap.  Under a hypervisor an I/O
 * port write leaves the guest entirely, so it can cost thousands of times a
 * plain instruction; every debug byte pays that price once.  Measuring both
 * against the same clock tells whether emitted diagnostics or the emulated
 * processor dominates a frame. */
#define QXP_D3D_PROFILE_CALIBRATION_COUNT 1000UL

static void qxp_d3d_profile_calibrate(QXP_DEVICE_EXTENSION *device)
{
    LONGLONG start_ticks;
    LONGLONG finish_ticks;
    ULONG index;

    start_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    for (index = 0; index < QXP_D3D_PROFILE_CALIBRATION_COUNT; ++index) {
        /* 0xEB carries no device, so this measures the trap alone. */
        VideoPortWritePortUchar((PUCHAR)(ULONG_PTR)0xEB, (UCHAR)index);
    }
    finish_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    device->d3d_profile_port_write_ticks =
        (ULONGLONG)(finish_ticks - start_ticks);

    start_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    for (index = 0; index < QXP_D3D_PROFILE_CALIBRATION_COUNT; ++index) {
        device->d3d_profile_calibration_sink += index;
    }
    finish_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    device->d3d_profile_spin_ticks =
        (ULONGLONG)(finish_ticks - start_ticks);
}

static void qxp_d3d_profile_move_interleaved(
    QXP_DEVICE_EXTENSION *device, PVOID destination,
    const VOID *source, ULONG bytes)
{
    LONGLONG start_ticks;
    LONGLONG finish_ticks;

    start_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    VideoPortMoveMemory(destination, (PVOID)source, bytes);
    finish_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    qxp_d3d_profile_add(
        &device->d3d_profile_interleaved_copy_ticks,
        finish_ticks - start_ticks);
    qxp_d3d_profile_add(
        &device->d3d_profile_interleaved_copy_bytes,
        (LONGLONG)bytes);
    if (device->d3d_profile_interleaved_copy_calls != 0xffffffffUL)
        ++device->d3d_profile_interleaved_copy_calls;
}
#define QXP_D3D_MOVE_INTERLEAVED(device, destination, source, bytes) \
    qxp_d3d_profile_move_interleaved(                              \
        (device), (destination), (source), (bytes))
#else
#define QXP_D3D_MOVE_INTERLEAVED(device, destination, source, bytes) \
    VideoPortMoveMemory((destination), (PVOID)(source), (bytes))
#endif

static void qxp_write(QXP_DEVICE_EXTENSION *device, USHORT index,
                      USHORT value)
{
    if (device->use_mmio) {
        VideoPortWriteRegisterUshort(
            (PUSHORT)(device->control.mapped + QXP_MMIO_DISPI_OFFSET +
                      index * sizeof(USHORT)), value);
    } else {
        VideoPortWritePortUshort((PUSHORT)device->control.mapped, index);
        VideoPortWritePortUshort((PUSHORT)(device->control.mapped + 1), value);
    }
}

static USHORT qxp_read(QXP_DEVICE_EXTENSION *device, USHORT index)
{
    if (device->use_mmio) {
        return VideoPortReadRegisterUshort(
            (PUSHORT)(device->control.mapped + QXP_MMIO_DISPI_OFFSET +
                      index * sizeof(USHORT)));
    }
    VideoPortWritePortUshort((PUSHORT)device->control.mapped, index);
    return VideoPortReadPortUshort((PUSHORT)(device->control.mapped + 1));
}

static ULONG qxp_mesa_mmio_read(QXP_DEVICE_EXTENSION *device, ULONG offset)
{
    if (!device->mesa_registers.mapped ||
        offset > device->mesa_registers.length - sizeof(ULONG)) {
        return 0;
    }
    return VideoPortReadRegisterUlong(
        (PULONG)(device->mesa_registers.mapped + offset));
}

static void qxp_mesa_mmio_write(QXP_DEVICE_EXTENSION *device, ULONG offset,
                                ULONG value)
{
    if (device->mesa_registers.mapped &&
        offset <= device->mesa_registers.length - sizeof(ULONG)) {
        VideoPortWriteRegisterUlong(
            (PULONG)(device->mesa_registers.mapped + offset), value);
    }
}

static BOOLEAN qxp_ascii_extension_present(
    const volatile UCHAR *text, ULONG capacity, const char *extension)
{
    ULONG offset;
    ULONG length = 0;
    ULONG index;

    if (!text || !capacity || !extension || !extension[0]) return FALSE;
    while (extension[length]) ++length;
    if (length >= capacity) return FALSE;
    for (offset = 0; offset + length < capacity && text[offset]; ++offset) {
        if (offset && text[offset - 1] != ' ') continue;
        for (index = 0; index < length; ++index) {
            if (text[offset + index] != (UCHAR)extension[index]) break;
        }
        if (index == length &&
            (!text[offset + length] || text[offset + length] == ' '))
            return TRUE;
    }
    return FALSE;
}

static BOOLEAN qxp_d3d_host_extension(
    QXP_DEVICE_EXTENSION *device, USHORT token, const char *extension)
{
    volatile ULONG *fifo;
    volatile ULONG *data;
    volatile UCHAR *output;
    ULONG output_capacity = 3UL * 4096UL;

    if (!device || !device->mesa_fifo.mapped || !token) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    data = fifo + QXP_MESA_MAX_FIFO;
    output = device->mesa_fifo.mapped +
             QXP_MESA_FIFO_SIZE - output_capacity;
    /* glGetString(GL_EXTENSIONS) accepts an optional legacy year limit in
     * the first aligned payload word. Never inherit one from an old draw. */
    data[2] = 0;
    output[0] = 0;
    fifo[2] = QXP_GL_EXTENSIONS;
    qxp_mesa_mmio_write(device, 0xfb0, token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_GET_STRING);
    return qxp_ascii_extension_present(
        output, output_capacity, extension);
}

static BOOLEAN qxp_map_transport_range(QXP_DEVICE_EXTENSION *device,
                                       QXP_RANGE *range, ULONG physical_base,
                                       ULONG length, BOOLEAN cached)
{
    PHYSICAL_ADDRESS physical;
    PVOID mapped = NULL;
    ULONG address_space = VIDEO_MEMORY_SPACE_MEMORY;
    VP_STATUS status;

    /* The FIFO command ring and the FBTM aperture are host RAM shared with the
     * device model, not real device MMIO. Mapping them uncached forces every
     * vertex, command, texture, and readback byte through uncached memory,
     * which is ~100x slower under hardware virtualization (WHPX) than a cached
     * mapping and is invisible under TCG (which ignores guest cache attrs).
     * Request a write-back cached mapping (P6CACHE) for those RAM ranges.
     * Coherence with the host is guaranteed on x86: the doorbell MMIO write
     * that hands a batch to the host is a serializing VM exit, and cache
     * coherency (MESI) makes the host's reads of the same physical page see
     * the guest's stores. The register range (the doorbell itself) must stay
     * uncached so writes trap. */
    if (cached) {
        address_space |= VIDEO_MEMORY_SPACE_P6CACHE;
    }

    physical.LowPart = physical_base;
    physical.HighPart = 0;
    status = VideoPortMapMemory(device, physical, &length, &address_space,
                                &mapped);
    if (status != NO_ERROR || !mapped) return FALSE;
    range->physical = physical;
    range->length = length;
    range->io_space = 0;
    range->mapped = mapped;
    return TRUE;
}

static void qxp_unmap_transport_range(QXP_DEVICE_EXTENSION *device,
                                      QXP_RANGE *range)
{
    if (range->mapped) {
        VideoPortUnmapMemory(device, range->mapped, NULL);
        range->mapped = NULL;
        range->length = 0;
    }
}

static void qxp_d3d_unmap_transport(QXP_DEVICE_EXTENSION *device)
{
    qxp_unmap_transport_range(device, &device->mesa_fbtm);
    qxp_unmap_transport_range(device, &device->mesa_fifo);
    qxp_unmap_transport_range(device, &device->mesa_registers);
}

static BOOLEAN qxp_d3d_map_transport(QXP_DEVICE_EXTENSION *device)
{
    if (device->mesa_registers.mapped && device->mesa_fifo.mapped &&
        device->mesa_fbtm.mapped) {
        return TRUE;
    }
    qxp_d3d_unmap_transport(device);
    if (!qxp_map_transport_range(device, &device->mesa_registers,
                                 QXP_MESA_REGS_BASE,
                                 QXP_MESA_REGS_SIZE, FALSE) ||
        !qxp_map_transport_range(device, &device->mesa_fifo,
                                 QXP_MESA_FIFO_BASE,
                                 QXP_MESA_FIFO_SIZE, TRUE) ||
        !qxp_map_transport_range(device, &device->mesa_fbtm,
                                 QXP_MESA_FBTM_BASE,
                                 QXP_MESA_FBTM_SIZE, TRUE)) {
        qxp_d3d_unmap_transport(device);
        return FALSE;
    }
    return TRUE;
}

static void qxp_fill_mode(QXP_DEVICE_EXTENSION *device, ULONG mode_number,
                          PVIDEO_MODE_INFORMATION output)
{
    const QXP_RESOLUTION *resolution =
        &qxp_resolutions[device->supported[mode_number]];

    VideoPortZeroMemory(output, sizeof(*output));
    output->Length = sizeof(*output);
    output->ModeIndex = mode_number;
    output->VisScreenWidth = resolution->width;
    output->VisScreenHeight = resolution->height;
    output->ScreenStride = resolution->width *
                           (resolution->bits_per_pixel / 8);
    output->NumberOfPlanes = 1;
    output->BitsPerPlane = resolution->bits_per_pixel;
    output->Frequency = 60;
    output->XMillimeter = resolution->width * 254 / 960;
    output->YMillimeter = resolution->height * 254 / 960;
    if (resolution->bits_per_pixel == 8) {
        output->NumberRedBits = 8;
        output->NumberGreenBits = 8;
        output->NumberBlueBits = 8;
    } else if (resolution->bits_per_pixel == 16) {
        output->NumberRedBits = 5;
        output->NumberGreenBits = 6;
        output->NumberBlueBits = 5;
        output->RedMask = 0x0000F800;
        output->GreenMask = 0x000007E0;
        output->BlueMask = 0x0000001F;
    } else {
        output->NumberRedBits = 8;
        output->NumberGreenBits = 8;
        output->NumberBlueBits = 8;
        output->RedMask = 0x00FF0000;
        output->GreenMask = 0x0000FF00;
        output->BlueMask = 0x000000FF;
    }
    output->AttributeFlags = VIDEO_MODE_GRAPHICS | VIDEO_MODE_COLOR |
                             VIDEO_MODE_NO_OFF_SCREEN;
    output->VideoMemoryBitmapWidth = resolution->width;
    output->VideoMemoryBitmapHeight = resolution->height;
}

static BOOLEAN qxp_set_mode(QXP_DEVICE_EXTENSION *device, ULONG mode_number)
{
    const QXP_RESOLUTION *resolution;
    BOOLEAN valid;

    if (mode_number >= device->mode_count) return FALSE;
    resolution = &qxp_resolutions[device->supported[mode_number]];
    qxp_write(device, QXP_DISPI_INDEX_ENABLE, QXP_DISPI_DISABLED);
    qxp_write(device, QXP_DISPI_INDEX_XRES, resolution->width);
    qxp_write(device, QXP_DISPI_INDEX_YRES, resolution->height);
    qxp_write(device, QXP_DISPI_INDEX_BPP,
              resolution->bits_per_pixel);
    qxp_write(device, QXP_DISPI_INDEX_VIRT_WIDTH, resolution->width);
    qxp_write(device, QXP_DISPI_INDEX_X_OFFSET, 0);
    qxp_write(device, QXP_DISPI_INDEX_Y_OFFSET, 0);
    valid = qxp_read(device, QXP_DISPI_INDEX_XRES) == resolution->width &&
            qxp_read(device, QXP_DISPI_INDEX_YRES) == resolution->height &&
            qxp_read(device, QXP_DISPI_INDEX_BPP) ==
                resolution->bits_per_pixel;
    qxp_write(device, QXP_DISPI_INDEX_ENABLE,
              QXP_DISPI_ENABLED | QXP_DISPI_8BIT_DAC |
                  QXP_DISPI_LFB_ENABLED);
    if (valid) device->current_mode = mode_number;
    return valid;
}

static BOOLEAN qxp_set_color_registers(PVIDEO_REQUEST_PACKET request)
{
    PVIDEO_CLUT clut;
    ULONG required;
    ULONG index;

    if (!request || !request->InputBuffer || request->InputBufferLength <
            FIELD_OFFSET(VIDEO_CLUT, LookupTable)) return FALSE;
    clut = (PVIDEO_CLUT)request->InputBuffer;
    if (!clut->NumEntries || clut->FirstEntry >= 256 ||
        clut->NumEntries > 256 - clut->FirstEntry ||
        clut->NumEntries >
            (0xffffffffUL - FIELD_OFFSET(VIDEO_CLUT, LookupTable)) /
                sizeof(clut->LookupTable[0])) return FALSE;
    required = FIELD_OFFSET(VIDEO_CLUT, LookupTable) +
               clut->NumEntries * sizeof(clut->LookupTable[0]);
    if (request->InputBufferLength < required) return FALSE;
    VideoPortWritePortUchar(
        (PUCHAR)(ULONG_PTR)QXP_VGA_DAC_WRITE_INDEX,
        (UCHAR)clut->FirstEntry);
    for (index = 0; index < clut->NumEntries; ++index) {
        VideoPortWritePortUchar(
            (PUCHAR)(ULONG_PTR)QXP_VGA_DAC_DATA,
            clut->LookupTable[index].RgbArray.Red);
        VideoPortWritePortUchar(
            (PUCHAR)(ULONG_PTR)QXP_VGA_DAC_DATA,
            clut->LookupTable[index].RgbArray.Green);
        VideoPortWritePortUchar(
            (PUCHAR)(ULONG_PTR)QXP_VGA_DAC_DATA,
            clut->LookupTable[index].RgbArray.Blue);
    }
    return TRUE;
}

VP_STATUS QxpFindAdapter(PVOID extension, PVOID context, PWSTR arguments,
                         PVIDEO_PORT_CONFIG_INFO config, PUCHAR again)
{
    QXP_DEVICE_EXTENSION *device = extension;
    VIDEO_ACCESS_RANGE ranges[2];
    VIDEO_ACCESS_RANGE legacy;
    VIDEO_ACCESS_RANGE dac;
    VP_STATUS status;

    qxp_debug("QXPMP F");
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(arguments);
    if (!config || config->Length < sizeof(*config)) {
        qxp_debug("QXPMP F1");
        return ERROR_INVALID_PARAMETER;
    }
    *again = 0;
    VideoPortZeroMemory(ranges, sizeof(ranges));
    status = VideoPortGetAccessRanges(device, 0, NULL, 2, ranges,
                                      NULL, NULL, NULL);
    if (status != NO_ERROR || ranges[0].RangeLength == 0 ||
        ranges[0].RangeInIoSpace) {
        qxp_debug("QXPMP F2");
        return ERROR_DEV_NOT_EXIST;
    }
    device->framebuffer.physical = ranges[0].RangeStart;
    device->framebuffer.length = ranges[0].RangeLength;
    device->framebuffer.io_space = ranges[0].RangeInIoSpace;

    if (ranges[1].RangeLength >= QXP_MMIO_LENGTH &&
        !ranges[1].RangeInIoSpace) {
        device->control.physical = ranges[1].RangeStart;
        device->control.length = ranges[1].RangeLength;
        device->control.io_space = ranges[1].RangeInIoSpace;
        device->use_mmio = TRUE;
    } else {
        VideoPortZeroMemory(&legacy, sizeof(legacy));
        legacy.RangeStart.LowPart = 0x01CE;
        legacy.RangeLength = 2;
        legacy.RangeInIoSpace = TRUE;
        legacy.RangeVisible = TRUE;
        legacy.RangeShareable = TRUE;
        if (VideoPortVerifyAccessRanges(device, 1, &legacy) != NO_ERROR) {
            qxp_debug("QXPMP F3");
            return ERROR_DEV_NOT_EXIST;
        }
        device->control.physical = legacy.RangeStart;
        device->control.length = legacy.RangeLength;
        device->control.io_space = legacy.RangeInIoSpace;
        device->use_mmio = FALSE;
    }
    VideoPortZeroMemory(&dac, sizeof(dac));
    dac.RangeStart.LowPart = QXP_VGA_DAC_WRITE_INDEX;
    dac.RangeLength = 2;
    dac.RangeInIoSpace = TRUE;
    dac.RangeVisible = TRUE;
    dac.RangeShareable = TRUE;
    if (VideoPortVerifyAccessRanges(device, 1, &dac) != NO_ERROR) {
        qxp_debug("QXPMP F DAC-");
        return ERROR_DEV_NOT_EXIST;
    }
    device->control.mapped = VideoPortGetDeviceBase(
        device, device->control.physical, device->control.length,
        device->control.io_space ? VIDEO_MEMORY_SPACE_IO :
                                   VIDEO_MEMORY_SPACE_MEMORY);
    if (!device->control.mapped) {
        qxp_debug("QXPMP F4");
        return ERROR_DEV_NOT_EXIST;
    }

    config->VdmPhysicalVideoMemoryAddress.LowPart = 0x000A0000;
    config->VdmPhysicalVideoMemoryAddress.HighPart = 0;
    config->VdmPhysicalVideoMemoryLength = 0x00020000;
    qxp_debug(device->use_mmio ? "QXPMP F+M" : "QXPMP F+P");
    return NO_ERROR;
}

/*
 * Publish the adapter identity and an honest video-memory size to the
 * registry so the Display control panel, dxdiag, and D3D adapter queries
 * show a populated "NVIDIA GeForce4 Ti 4600" instead of blank Chip / DAC /
 * Memory / adapter-name fields.
 *
 * MemorySize is taken live from device->vram_bytes, which itself is read from
 * the emulated adapter (QEMU's VGA vgamem_mb -> DISPI VRAM_64K register).
 * This keeps QEMU's vgamem_mb the single VRAM tuning knob: change it and the
 * reported memory tracks automatically -- always honest, with no driver edit.
 * VideoPortSetRegistryParameters stores each value as REG_BINARY; the control
 * panel reads the string values as UTF-16 and MemorySize as a byte count.
 */
static void qxp_register_hardware_info(QXP_DEVICE_EXTENSION *device)
{
    ULONG memory_bytes = device->vram_bytes;

    VideoPortSetRegistryParameters(device,
        L"HardwareInformation.AdapterString",
        L"NVIDIA GeForce4 Ti 4600", sizeof(L"NVIDIA GeForce4 Ti 4600"));
    VideoPortSetRegistryParameters(device,
        L"HardwareInformation.ChipType",
        L"GeForce4 Ti 4600", sizeof(L"GeForce4 Ti 4600"));
    VideoPortSetRegistryParameters(device,
        L"HardwareInformation.DacType",
        L"Integrated RAMDAC", sizeof(L"Integrated RAMDAC"));
    VideoPortSetRegistryParameters(device,
        L"HardwareInformation.BiosString",
        L"NVIDIA GeForce4 Ti 4600 VGA BIOS",
        sizeof(L"NVIDIA GeForce4 Ti 4600 VGA BIOS"));
    VideoPortSetRegistryParameters(device,
        L"HardwareInformation.MemorySize",
        &memory_bytes, sizeof(memory_bytes));
}

BOOLEAN QxpInitialize(PVOID extension)
{
    QXP_DEVICE_EXTENSION *device = extension;
    ULONG index;
    USHORT identifier;
    ULONG reported_vram;

    qxp_debug("QXPMP I");
    identifier = qxp_read(device, QXP_DISPI_INDEX_ID);
    if (identifier < QXP_DISPI_ID2 || identifier > QXP_DISPI_ID5) {
        qxp_debug("QXPMP I1");
        return FALSE;
    }
    qxp_write(device, QXP_DISPI_INDEX_ENABLE, QXP_DISPI_GETCAPS);
    device->max_width = qxp_read(device, QXP_DISPI_INDEX_XRES);
    device->max_height = qxp_read(device, QXP_DISPI_INDEX_YRES);
    reported_vram = (ULONG)qxp_read(device, QXP_DISPI_INDEX_VRAM_64K) * 65536;
    qxp_write(device, QXP_DISPI_INDEX_ENABLE, QXP_DISPI_DISABLED);
    device->vram_bytes = device->framebuffer.length;
    if (reported_vram && reported_vram < device->vram_bytes) {
        device->vram_bytes = reported_vram;
    }
    if (!device->max_width || !device->max_height || !device->vram_bytes) {
        qxp_debug("QXPMP I2");
        return FALSE;
    }
    for (index = 0; index < QXP_MODE_CAPACITY; ++index) {
        ULONG required = (ULONG)qxp_resolutions[index].width *
                         qxp_resolutions[index].height *
                         (qxp_resolutions[index].bits_per_pixel / 8);
        if (qxp_resolutions[index].width <= device->max_width &&
            qxp_resolutions[index].height <= device->max_height &&
            required <= device->vram_bytes) {
            device->supported[device->mode_count++] = (UCHAR)index;
        }
    }
    if (!device->mode_count) {
        qxp_debug("QXPMP I3");
        return FALSE;
    }
    qxp_register_hardware_info(device);
    qxp_debug("QXPMP I+");
    return TRUE;
}

static ULONG qxp_float_from_unorm8(UCHAR value)
{
    ULONG shift;
    ULONG exponent;
    ULONG numerator;
    ULONG remainder;
    ULONG mantissa = 0;
    ULONG index;

    if (!value) return 0;
    if (value == 255) return 0x3f800000UL;
    for (shift = 1; shift < 8; ++shift) {
        if (((ULONG)value << shift) >= 255) break;
    }
    exponent = 127 - shift;
    numerator = (ULONG)value << shift;
    remainder = numerator - 255;
    for (index = 0; index < 23; ++index) {
        remainder <<= 1;
        mantissa <<= 1;
        if (remainder >= 255) {
            remainder -= 255;
            mantissa |= 1;
        }
    }
    remainder <<= 1;
    if (remainder > 255 || (remainder == 255 && (mantissa & 1))) {
        ++mantissa;
        if (mantissa == 0x00800000UL) {
            mantissa = 0;
            ++exponent;
        }
    }
    return (exponent << 23) | mantissa;
}

static BOOLEAN qxp_d3d_fog_mode(ULONG value, PULONG mode)
{
    if (!mode) return FALSE;
    switch (value) {
    case 1:
        *mode = QXP_GL_EXP;
        return TRUE;
    case 2:
        *mode = QXP_GL_EXP2;
        return TRUE;
    case 3:
        *mode = QXP_GL_LINEAR;
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN qxp_d3d_point_state_valid(
    const QXP_D3D_DRAW_REQUEST *request)
{
    /* A device clamps point size to its advertised maximum; it does not
     * reject the draw when a game asks for a larger D3DRS_POINTSIZE or
     * D3DRS_POINTSIZE_MAX (D3D default POINTSIZE_MAX is 64.0f, but titles
     * legitimately set larger values, e.g. POP:SOT uses 100.0f). The upper
     * bound is therefore enforced by clamping at apply time, not by failing
     * validation here. Sign, finiteness, nonzero, and min<=max remain real
     * validity constraints. */
    if (!request || request->point_scale_enable > 1 ||
        request->point_sprite_enable > 1 ||
        !qxp_float_bits_finite(request->point_size_bits) ||
        !qxp_float_bits_finite(request->point_size_min_bits) ||
        !qxp_float_bits_finite(request->point_size_max_bits) ||
        !qxp_float_bits_finite(request->point_scale_a_bits) ||
        !qxp_float_bits_finite(request->point_scale_b_bits) ||
        !qxp_float_bits_finite(request->point_scale_c_bits) ||
        !(request->point_size_bits & 0x7fffffffUL) ||
        (request->point_size_bits & 0x80000000UL) ||
        (!request->point_scale_enable &&
         request->point_size_bits < 0x3f800000UL) ||
        (request->point_size_min_bits & 0x80000000UL) ||
        (request->point_size_max_bits & 0x80000000UL) ||
        !(request->point_size_max_bits & 0x7fffffffUL) ||
        request->point_size_min_bits > request->point_size_max_bits ||
        (request->point_scale_a_bits & 0x80000000UL) ||
        (request->point_scale_b_bits & 0x80000000UL) ||
        (request->point_scale_c_bits & 0x80000000UL)) return FALSE;
    return TRUE;
}

static ULONG qxp_float_from_negative_uint(ULONG value)
{
    ULONG exponent = 0;
    ULONG base;
    ULONG mantissa;

    if (!value) return 0;
    while (exponent < 31 && (1UL << exponent) <= value / 2) ++exponent;
    base = 1UL << exponent;
    mantissa = (value - base) << (23 - exponent);
    return 0x80000000UL | ((127UL + exponent) << 23) | mantissa;
}

static BOOLEAN qxp_d3d_fifo_submit(QXP_DEVICE_EXTENSION *device,
                                   ULONG function, const ULONG *arguments,
                                   ULONG count)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG index;
    ULONG argument;

    if (!fifo || count > 16) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    index = fifo[0];
    if (index < QXP_MESA_FIRST_FIFO ||
        index + count + 1 >= QXP_MESA_MAX_FIFO ||
        data[0] >= QXP_MESA_MAX_DATA) {
        return FALSE;
    }
    fifo[index++] = function;
    for (argument = 0; argument < count; ++argument) {
        fifo[index++] = arguments[argument];
    }
    fifo[0] = index;
    return TRUE;
}

static BOOLEAN qxp_d3d_bytes_equal(const void *left, const void *right,
                                   ULONG bytes)
{
    const ULONG *left_words = left;
    const ULONG *right_words = right;
    const UCHAR *left_bytes;
    const UCHAR *right_bytes;

    if (!left || !right) return FALSE;
    if (!(((ULONG_PTR)left | (ULONG_PTR)right | bytes) &
          (sizeof(ULONG) - 1UL))) {
        while (bytes >= sizeof(ULONG)) {
            if (*left_words++ != *right_words++) return FALSE;
            bytes -= sizeof(ULONG);
        }
        return TRUE;
    }
    left_bytes = left;
    right_bytes = right;
    while (bytes--) {
        if (*left_bytes++ != *right_bytes++) return FALSE;
    }
    return TRUE;
}

static void qxp_d3d_invalidate_draw_state(QXP_DEVICE_EXTENSION *device)
{
    if (device) device->d3d_draw_state_valid = FALSE;
}

static ULONG qxp_d3d_canonical_vertex_mode(ULONG vertex_mode)
{
    if (vertex_mode == QXP_D3D_VERTEX_INDEXED_TRANSFORMED)
        return QXP_D3D_VERTEX_TRANSFORMED;
    if (vertex_mode == QXP_D3D_VERTEX_INDEXED_OBJECT)
        return QXP_D3D_VERTEX_OBJECT;
    return vertex_mode;
}

static BOOLEAN qxp_d3d_fixed_state_same(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *hwtl_state)
{
    ULONG bytes;

    if (!device || !request || !device->d3d_draw_state_valid ||
        device->d3d_last_draw.primitive_type != request->primitive_type ||
        device->d3d_last_draw.fvf != request->fvf ||
        qxp_d3d_canonical_vertex_mode(
            device->d3d_last_draw.vertex_mode) !=
        qxp_d3d_canonical_vertex_mode(request->vertex_mode) ||
        device->d3d_last_draw.texture_coord_index !=
            request->texture_coord_index ||
        device->d3d_last_draw.texture1_coord_index !=
            request->texture1_coord_index ||
        device->d3d_last_draw.texture2_coord_index !=
            request->texture2_coord_index ||
        device->d3d_last_draw.texture3_coord_index !=
            request->texture3_coord_index ||
        device->d3d_last_target_texture !=
            (device->d3d_target_texture != 0) ||
        device->d3d_last_target_present !=
            device->d3d_target_present) return FALSE;
    bytes = (ULONG)((const UCHAR *)&request->texture_name -
                    (const UCHAR *)&request->target_width);
    if (!qxp_d3d_bytes_equal(&device->d3d_last_draw.target_width,
                             &request->target_width, bytes)) return FALSE;
    if (!hwtl_state) return !device->d3d_last_hwtl_valid;
    return device->d3d_last_hwtl_valid &&
           qxp_d3d_bytes_equal(&device->d3d_last_hwtl_state, hwtl_state,
                               sizeof(*hwtl_state));
}

static BOOLEAN qxp_d3d_specular_alpha_program_supported(
    const QXP_D3D_DRAW_REQUEST *request)
{
    return (BOOLEAN)(request &&
        !request->pixel_program_name && !request->sm2_program_name &&
        qxp_d3d_canonical_vertex_mode(request->vertex_mode) ==
            QXP_D3D_VERTEX_TRANSFORMED &&
        (request->fvf & (QXP_D3DFVF_SPECULAR | QXP_D3DFVF_FOG)) ==
            (QXP_D3DFVF_SPECULAR | QXP_D3DFVF_FOG) &&
        !request->fog_table_mode &&
        !request->point_scale_enable && request->specular_enable &&
        request->texture_color_op == QXP_D3DTOP_SELECTARG1 &&
        request->texture_color_arg1 == QXP_D3DTA_DIFFUSE &&
        request->texture_alpha_op == QXP_D3DTOP_SELECTARG1 &&
        request->texture_alpha_arg1 == QXP_D3DTA_SPECULAR &&
        request->texture1_color_op == QXP_D3DTOP_DISABLE);
}

static BOOLEAN qxp_d3d_texture_state_same(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    ULONG stage)
{
    const UCHAR *old_state;
    const UCHAR *new_state;
    ULONG bytes;
    ULONG bit;

    if (!device || !request || !device->d3d_draw_state_valid || stage >= 4 ||
        qxp_d3d_specular_alpha_program_supported(request) ||
        qxp_d3d_specular_alpha_program_supported(&device->d3d_last_draw) ||
        device->d3d_last_draw.pixel_program_name !=
            request->pixel_program_name ||
        device->d3d_last_draw.texture_factor !=
            request->texture_factor) return FALSE;
    bit = 1UL << stage;
    if (((device->d3d_last_draw.pixel_sampled_stages ^
          request->pixel_sampled_stages) & bit) ||
        ((device->d3d_last_draw.pixel_cube_stages ^
          request->pixel_cube_stages) & bit) ||
        ((device->d3d_last_draw.pixel_volume_stages ^
          request->pixel_volume_stages) & bit)) return FALSE;
    if (!stage) {
        old_state = (const UCHAR *)&device->d3d_last_draw.texture_name;
        new_state = (const UCHAR *)&request->texture_name;
        bytes = (ULONG)((const UCHAR *)&request->texture1_name - new_state);
    } else if (stage == 1) {
        old_state = (const UCHAR *)&device->d3d_last_draw.texture1_name;
        new_state = (const UCHAR *)&request->texture1_name;
        bytes = (ULONG)((const UCHAR *)&request->texture2_name - new_state);
    } else if (stage == 2) {
        old_state = (const UCHAR *)&device->d3d_last_draw.texture2_name;
        new_state = (const UCHAR *)&request->texture2_name;
        bytes = (ULONG)((const UCHAR *)&request->texture3_name - new_state);
    } else {
        old_state = (const UCHAR *)&device->d3d_last_draw.texture3_name;
        new_state = (const UCHAR *)&request->texture3_name;
        bytes = (ULONG)((const UCHAR *)&request->pixel_program_name - new_state);
    }
    return qxp_d3d_bytes_equal(old_state, new_state, bytes);
}

static BOOLEAN qxp_d3d_point_sprite_state_same(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request)
{
    BOOLEAN old_enable;
    BOOLEAN new_enable;

    if (!device || !request || !device->d3d_draw_state_valid) return FALSE;
    old_enable = device->d3d_last_draw.point_sprite_enable &&
                 device->d3d_last_draw.primitive_type == 1;
    new_enable = request->point_sprite_enable && request->primitive_type == 1;
    if (old_enable != new_enable) return FALSE;
    if (!new_enable) return TRUE;
    return device->d3d_last_target_texture ==
               (device->d3d_target_texture != 0) &&
           device->d3d_last_target_present == device->d3d_target_present;
}

static BOOLEAN qxp_d3d_array_state_same(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    ULONG coordinate_stages, BOOLEAN fog_active,
    BOOLEAN generic_secondary)
{
    return device && request &&
           !qxp_d3d_specular_alpha_program_supported(request) &&
           !qxp_d3d_specular_alpha_program_supported(
               &device->d3d_last_draw) &&
           device->d3d_draw_state_valid &&
           device->d3d_last_draw.fvf == request->fvf &&
           qxp_d3d_canonical_vertex_mode(
               device->d3d_last_draw.vertex_mode) ==
               qxp_d3d_canonical_vertex_mode(request->vertex_mode) &&
           device->d3d_last_coordinate_stages == coordinate_stages &&
           device->d3d_last_draw.texture_coord_index ==
               request->texture_coord_index &&
           device->d3d_last_draw.texture1_coord_index ==
               request->texture1_coord_index &&
           device->d3d_last_draw.texture2_coord_index ==
               request->texture2_coord_index &&
           device->d3d_last_draw.texture3_coord_index ==
               request->texture3_coord_index &&
           device->d3d_last_fog_active == fog_active &&
           device->d3d_last_generic_secondary == generic_secondary;
}

static void qxp_d3d_commit_draw_state(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *hwtl_state, ULONG coordinate_stages,
    BOOLEAN fog_active, BOOLEAN generic_secondary)
{
    ULONG bytes;

    if (!device || !request) return;
    bytes = sizeof(device->d3d_last_draw) -
            sizeof(device->d3d_last_draw.vertices);
    VideoPortMoveMemory(&device->d3d_last_draw, (PVOID)request, bytes);
    device->d3d_last_hwtl_valid = hwtl_state != NULL;
    if (hwtl_state) {
        VideoPortMoveMemory(&device->d3d_last_hwtl_state,
                            (PVOID)hwtl_state, sizeof(*hwtl_state));
    }
    device->d3d_last_coordinate_stages = coordinate_stages;
    device->d3d_last_fog_active = fog_active;
    device->d3d_last_generic_secondary = generic_secondary;
    device->d3d_last_target_texture = device->d3d_target_texture != 0;
    device->d3d_last_target_present = device->d3d_target_present;
    device->d3d_draw_state_valid = TRUE;
}

static void qxp_d3d_finish_draw_batch(QXP_DEVICE_EXTENSION *device)
{
    if (!device || !device->d3d_pending_draws) return;
    if (device->d3d_batch_count != 0xffffffffUL)
        ++device->d3d_batch_count;
    if (device->d3d_pending_draws > device->d3d_batch_max_draws)
        device->d3d_batch_max_draws = device->d3d_pending_draws;
    device->d3d_pending_draws = 0;
}

static BOOLEAN qxp_d3d_reserve_draw_fifo(QXP_DEVICE_EXTENSION *device)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;

    if (!fifo || fifo[0] < QXP_MESA_FIRST_FIFO ||
        fifo[0] >= QXP_MESA_MAX_FIFO) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    if (data[0] >= 2UL && data[0] < QXP_MESA_MAX_DATA &&
        fifo[0] <= QXP_MESA_MAX_FIFO - QXP_D3D_DRAW_FIFO_RESERVE &&
        data[0] <= QXP_MESA_MAX_DATA - QXP_D3D_DRAW_DATA_RESERVE)
        return TRUE;

    /* Flush only between draws. A doorbell processes both command and data
     * queues before invoking glFlush, leaving room for one bounded draw. */
    qxp_d3d_finish_draw_batch(device);
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    /* Pure-draw flush: use the async doorbell when the host supports it, else
     * the synchronous one. Both leave the FIFO reset for the next draw. */
    qxp_mesa_mmio_write(device,
        device->d3d_async_supported ? QXP_MESA_ASYNC_DRAW_DOORBELL : 0xfc0,
        QXP_FENUM_GL_FLUSH);
    return fifo[0] == QXP_MESA_FIRST_FIFO && data[0] == 2UL;
}

static PUCHAR qxp_d3d_fifo_append_data(QXP_DEVICE_EXTENSION *device,
                                       ULONG bytes)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG data_index;
    ULONG aligned;
    ULONG next;
    PUCHAR destination;

    if (!fifo || !bytes || bytes > 0xfffffff8UL) return NULL;
    data = fifo + QXP_MESA_MAX_FIFO;
    data_index = data[0];
    aligned = (bytes + 7UL) & ~7UL;
    if (data_index < 2UL || data_index >= QXP_MESA_MAX_DATA ||
        aligned / sizeof(ULONG) > QXP_MESA_MAX_DATA - data_index)
        return NULL;
    next = data_index + aligned / sizeof(ULONG);
    if (next >= QXP_MESA_MAX_DATA) return NULL;
    destination = (PUCHAR)&data[data_index];
    /* Every caller writes all requested payload bytes before publishing the
     * corresponding FIFO command. Clear only the alignment tail: clearing
     * the complete range here doubled writes for the compact/interleaved draw
     * path and dominated its measured miniport submission time. */
    if (aligned > bytes)
        VideoPortZeroMemory(destination + bytes, aligned - bytes);
    data[0] = next;
    return destination;
}

typedef struct qxp_d3d_sync_result {
    QXP_DEVICE_EXTENSION *device;
    ULONG function;
    ULONG result;
} QXP_D3D_SYNC_RESULT;

static BOOLEAN qxp_d3d_call_fbtm_sync(PVOID context)
{
    QXP_D3D_SYNC_RESULT *call = context;

    if (!call || !call->device) return FALSE;
    qxp_mesa_mmio_write(call->device, 0xfb0, call->device->d3d_token);
    qxp_mesa_mmio_write(call->device, 0xfc0, call->function);
    /* XP can resume the caller on another vCPU after the synchronous MMIO
     * store.  The XPDM transport is globally serialized here, so consume the
     * host's global Direct3D result rather than a per-vCPU 0xFC0 latch. */
    call->result = qxp_mesa_mmio_read(call->device, 0xfa4);
    return TRUE;
}

static BOOLEAN qxp_d3d_call_fbtm_result(QXP_DEVICE_EXTENSION *device,
                                        ULONG function,
                                        const ULONG *arguments,
                                        ULONG count, const void *input,
                                        ULONG input_bytes, PULONG result)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    ULONG aligned;
    ULONG index;
    PUCHAR destination;
    QXP_D3D_SYNC_RESULT call;

    if (!fifo || !device->mesa_fbtm.mapped || count > 16 ||
        (input_bytes && !input) || input_bytes > QXP_MESA_FBTM_SIZE ||
        input_bytes > 0xfffffff8UL) return FALSE;
    aligned = (input_bytes + 7UL) & ~7UL;
    if (aligned) {
        destination = device->mesa_fbtm.mapped +
                      QXP_MESA_FBTM_SIZE - aligned;
        VideoPortZeroMemory(destination, aligned);
        VideoPortMoveMemory(destination, (PVOID)input, input_bytes);
    }
    for (index = 0; index < count; ++index) fifo[2 + index] = arguments[index];
    qxp_d3d_finish_draw_batch(device);
    if (result) {
        call.device = device;
        call.function = function;
        call.result = 0;
        if (!VideoPortSynchronizeExecution(
                device, VpHighPriority, qxp_d3d_call_fbtm_sync, &call))
            return FALSE;
        *result = call.result;
    } else {
        qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
        qxp_mesa_mmio_write(device, 0xfc0, function);
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_call_fbtm(QXP_DEVICE_EXTENSION *device,
                                 ULONG function, const ULONG *arguments,
                                 ULONG count, const void *input,
                                 ULONG input_bytes)
{
    return qxp_d3d_call_fbtm_result(device, function, arguments, count,
                                    input, input_bytes, NULL);
}

static BOOLEAN qxp_d3d_call_output(QXP_DEVICE_EXTENSION *device,
                                   ULONG function, const ULONG *arguments,
                                   ULONG count, void *output,
                                   ULONG output_bytes)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    PUCHAR shared_output;
    ULONG index;

    if (!fifo || !output || !output_bytes || count > 16 ||
        output_bytes > 3UL * 4096UL) return FALSE;
    shared_output = device->mesa_fifo.mapped + QXP_MESA_FIFO_SIZE -
                    3UL * 4096UL;
    VideoPortZeroMemory(shared_output, output_bytes);
    for (index = 0; index < count; ++index) fifo[2 + index] = arguments[index];
    qxp_d3d_finish_draw_batch(device);
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, function);
    VideoPortMoveMemory(output, shared_output, output_bytes);
    return TRUE;
}

static BOOLEAN qxp_d3d_call_input_result(
    QXP_DEVICE_EXTENSION *device, ULONG function,
    const ULONG *arguments, ULONG count, const void *input,
    ULONG input_bytes, PULONG result)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG data_index;
    ULONG words;
    ULONG next;
    ULONG index;
    QXP_D3D_SYNC_RESULT call;

    if (!fifo || !input || !input_bytes || count > 16 ||
        input_bytes > 0xfffffffCUL) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    data_index = data[0];
    words = (input_bytes + 3UL) >> 2;
    if (data_index < 2 || words > QXP_MESA_MAX_DATA - data_index) return FALSE;
    next = data_index + words;
    if (next & 1) ++next;
    if (next >= QXP_MESA_MAX_DATA) return FALSE;
    VideoPortZeroMemory((PVOID)&data[data_index],
                        (next - data_index) * sizeof(ULONG));
    VideoPortMoveMemory((PVOID)&data[data_index], (PVOID)input, input_bytes);
    data[0] = next;
    for (index = 0; index < count; ++index) fifo[2 + index] = arguments[index];
    qxp_d3d_finish_draw_batch(device);
    if (result) {
        call.device = device;
        call.function = function;
        call.result = 0;
        if (!VideoPortSynchronizeExecution(
                device, VpHighPriority, qxp_d3d_call_fbtm_sync, &call))
            return FALSE;
        *result = call.result;
    } else {
        qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
        qxp_mesa_mmio_write(device, 0xfc0, function);
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_call_input(QXP_DEVICE_EXTENSION *device,
                                  ULONG function, const ULONG *arguments,
                                  ULONG count, const void *input,
                                  ULONG input_bytes)
{
    return qxp_d3d_call_input_result(
        device, function, arguments, count, input, input_bytes, NULL);
}

static BOOLEAN qxp_d3d_fifo_submit_input(QXP_DEVICE_EXTENSION *device,
                                         ULONG function,
                                         const ULONG *arguments, ULONG count,
                                         const void *input, ULONG input_bytes)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG fifo_start;
    ULONG data_start;
    PUCHAR destination;

    if (!fifo || !input || !input_bytes || count > 16) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    destination = qxp_d3d_fifo_append_data(device, input_bytes);
    if (!destination) return FALSE;
    VideoPortMoveMemory(destination, (PVOID)input, input_bytes);
    if (!qxp_d3d_fifo_submit(device, function, arguments, count)) {
        fifo[0] = fifo_start;
        data[0] = data_start;
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_call_program_string(
    QXP_DEVICE_EXTENSION *device, ULONG target,
    const UCHAR *source, ULONG source_bytes)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG arguments[4];
    ULONG aligned;
    ULONG total;
    ULONG data_index;
    ULONG next;
    ULONG index;

    if (!fifo || !source || !source_bytes ||
        source_bytes > QXP_D3D_MAX_SHADER_SOURCE_BYTES ||
        source_bytes > 0xfffffff0UL) return FALSE;
    aligned = (source_bytes + 7UL) & ~7UL;
    total = aligned + 8UL;
    data = fifo + QXP_MESA_MAX_FIFO;
    data_index = data[0];
    if (data_index < 2 || total / sizeof(ULONG) >
            QXP_MESA_MAX_DATA - data_index) return FALSE;
    next = data_index + total / sizeof(ULONG);
    if (next >= QXP_MESA_MAX_DATA) return FALSE;
    VideoPortZeroMemory((PVOID)&data[data_index], total);
    VideoPortMoveMemory((PVOID)&data[data_index], (PVOID)source,
                        source_bytes);
    data[0] = next;
    arguments[0] = target;
    arguments[1] = QXP_GL_PROGRAM_FORMAT_ASCII_ARB;
    arguments[2] = source_bytes;
    arguments[3] = 0;
    for (index = 0; index < 4; ++index) fifo[2 + index] = arguments[index];
    qxp_d3d_finish_draw_batch(device);
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_PROGRAM_STRING_ARB);
    return TRUE;
}

static BOOLEAN qxp_d3d_call_shader_source(
    QXP_DEVICE_EXTENSION *device, ULONG shader,
    const UCHAR *source, ULONG source_bytes)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG arguments[4];
    ULONG aligned;
    ULONG total;
    ULONG data_index;
    ULONG next;
    ULONG index;

    if (!fifo || !shader || !source || source_bytes < 2 ||
        source[source_bytes - 1] != 0 ||
        source_bytes > QXP_D3D_MAX_SHADER_SOURCE_BYTES ||
        source_bytes > 0xfffffff0UL) return FALSE;
    aligned = (source_bytes + 7UL) & ~7UL;
    /* The host reconstructs a native char* array in the data ring. Count one
     * needs the aligned NUL-terminated source plus one 8-byte pointer slot. */
    total = aligned + 8UL;
    data = fifo + QXP_MESA_MAX_FIFO;
    data_index = data[0];
    if (data_index < 2 || total / sizeof(ULONG) >
            QXP_MESA_MAX_DATA - data_index) return FALSE;
    next = data_index + total / sizeof(ULONG);
    if (next >= QXP_MESA_MAX_DATA) return FALSE;
    VideoPortZeroMemory((PVOID)&data[data_index], total);
    VideoPortMoveMemory((PVOID)&data[data_index], (PVOID)source,
                        source_bytes);
    data[0] = next;
    arguments[0] = shader;
    arguments[1] = 1;
    arguments[2] = 0;
    arguments[3] = 0;
    for (index = 0; index < 4; ++index) fifo[2 + index] = arguments[index];
    qxp_d3d_finish_draw_batch(device);
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_SHADER_SOURCE);
    return TRUE;
}

typedef struct qxp_d3d_gl_scalar_output {
    ULONG count;
    ULONG padding;
    LONG value;
    ULONG trailing;
} QXP_D3D_GL_SCALAR_OUTPUT;

static BOOLEAN qxp_d3d_gl_query_int(
    QXP_DEVICE_EXTENSION *device, ULONG function, ULONG object,
    ULONG parameter, PLONG value)
{
    QXP_D3D_GL_SCALAR_OUTPUT output;
    ULONG arguments[3];

    if (!device || !object || !value) return FALSE;
    VideoPortZeroMemory(&output, sizeof(output));
    arguments[0] = object;
    arguments[1] = parameter;
    arguments[2] = 0;
    if (!qxp_d3d_call_output(device, function, arguments, 3,
                             &output, sizeof(output)) || output.count != 1)
        return FALSE;
    *value = output.value;
    return TRUE;
}


static void qxp_d3d_gl_debug_log(QXP_DEVICE_EXTENSION *device,
                                 ULONG function, ULONG object)
{
    UCHAR output[520];
    ULONG arguments[4];

    if (!device || !object) return;
    VideoPortZeroMemory(output, sizeof(output));
    arguments[0] = object;
    arguments[1] = sizeof(output) - 8;
    arguments[2] = 0;
    arguments[3] = 0;
    if (qxp_d3d_call_output(device, function, arguments, 4,
                            output, sizeof(output))) {
        output[sizeof(output) - 1] = 0;
        if (*(PULONG)output) qxp_debug((const char *)&output[8]);
    }
}

static BOOLEAN qxp_d3d_gl_uniform_location(
    QXP_DEVICE_EXTENSION *device, ULONG program, const char *name,
    ULONG name_bytes, PLONG location)
{
    ULONG arguments[2];
    ULONG result;

    if (!device || !program || !name || name_bytes < 2 ||
        name[name_bytes - 1] != 0 || !location) return FALSE;
    arguments[0] = program;
    arguments[1] = 0;
    if (!qxp_d3d_call_input_result(
            device, QXP_FENUM_GL_GET_UNIFORM_LOCATION,
            arguments, 2, name, name_bytes, &result)) return FALSE;
    *location = (LONG)result;
    return *location >= 0;
}

static BOOLEAN qxp_d3d_gl_uniform_location_optional(
    QXP_DEVICE_EXTENSION *device, ULONG program, const char *name,
    ULONG name_bytes, PLONG location)
{
    ULONG arguments[2];
    ULONG result;

    if (!device || !program || !name || name_bytes < 2 ||
        name[name_bytes - 1] != 0 || !location) return FALSE;
    arguments[0] = program;
    arguments[1] = 0;
    if (!qxp_d3d_call_input_result(
            device, QXP_FENUM_GL_GET_UNIFORM_LOCATION,
            arguments, 2, name, name_bytes, &result)) return FALSE;
    *location = (LONG)result;
    return TRUE;
}

static QXP_D3D_SM2_PROGRAM_STATE *qxp_d3d_find_sm2_program(
    QXP_DEVICE_EXTENSION *device, ULONG process_id, ULONG program_name)
{
    ULONG index;

    if (!device || !process_id || !program_name) return NULL;
    for (index = 0; index < QXP_D3D_SM2_PROGRAM_CAPACITY; ++index) {
        if (qxp_d3d_sm2_programs[index].device == device &&
            qxp_d3d_sm2_programs[index].process_id == process_id &&
            qxp_d3d_sm2_programs[index].program_name == program_name)
            return &qxp_d3d_sm2_programs[index];
    }
    return NULL;
}

static QXP_D3D_SM2_PROGRAM_STATE *qxp_d3d_free_sm2_program(void)
{
    ULONG index;

    for (index = 0; index < QXP_D3D_SM2_PROGRAM_CAPACITY; ++index) {
        if (!qxp_d3d_sm2_programs[index].program_name)
            return &qxp_d3d_sm2_programs[index];
    }
    return NULL;
}

static BOOLEAN qxp_d3d_destroy_sm2_objects(
    QXP_DEVICE_EXTENSION *device, ULONG gl_program,
    ULONG vertex_shader, BOOLEAN vertex_attached,
    ULONG pixel_shader, BOOLEAN pixel_attached)
{
    ULONG arguments[2];
    BOOLEAN result = TRUE;

    if (!device) return FALSE;
    if (gl_program && vertex_shader && vertex_attached) {
        arguments[0] = gl_program;
        arguments[1] = vertex_shader;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_DETACH_SHADER,
                arguments, 2, NULL, 0)) result = FALSE;
    }
    if (gl_program && pixel_shader && pixel_attached) {
        arguments[0] = gl_program;
        arguments[1] = pixel_shader;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_DETACH_SHADER,
                arguments, 2, NULL, 0)) result = FALSE;
    }
    if (vertex_shader) {
        arguments[0] = vertex_shader;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_DELETE_SHADER,
                arguments, 1, NULL, 0)) result = FALSE;
    }
    if (pixel_shader) {
        arguments[0] = pixel_shader;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_DELETE_SHADER,
                arguments, 1, NULL, 0)) result = FALSE;
    }
    if (gl_program) {
        arguments[0] = gl_program;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_DELETE_PROGRAM,
                arguments, 1, NULL, 0)) result = FALSE;
    }
    return result;
}

static BOOLEAN qxp_d3d_release_sm2_program(
    QXP_DEVICE_EXTENSION *device, QXP_D3D_SM2_PROGRAM_STATE *program)
{
    if (!device || !program || program->device != device ||
        !program->process_id || !program->program_name ||
        !program->vertex_shader || !program->pixel_shader ||
        !program->gl_program) return FALSE;
    if (!qxp_d3d_destroy_sm2_objects(
            device, program->gl_program,
            program->vertex_shader, TRUE,
            program->pixel_shader, TRUE)) return FALSE;
    VideoPortZeroMemory(program, sizeof(*program));
    return TRUE;
}

static void qxp_d3d_release_sm2_programs(
    QXP_DEVICE_EXTENSION *device, ULONG process_id)
{
    ULONG index;

    if (!device || !process_id) return;
    for (index = 0; index < QXP_D3D_SM2_PROGRAM_CAPACITY; ++index) {
        if (qxp_d3d_sm2_programs[index].device != device ||
            qxp_d3d_sm2_programs[index].process_id != process_id)
            continue;
        if (!qxp_d3d_release_sm2_program(
                device, &qxp_d3d_sm2_programs[index])) {
            qxp_debug("QXPMP D3D SM2 GLSL RELEASE-");
            VideoPortZeroMemory(&qxp_d3d_sm2_programs[index],
                                sizeof(qxp_d3d_sm2_programs[index]));
        }
    }
}

static void qxp_d3d_forget_sm2_programs(
    QXP_DEVICE_EXTENSION *device, ULONG process_id)
{
    ULONG index;

    if (!device || !process_id) return;
    for (index = 0; index < QXP_D3D_SM2_PROGRAM_CAPACITY; ++index) {
        if (qxp_d3d_sm2_programs[index].device == device &&
            qxp_d3d_sm2_programs[index].process_id == process_id) {
            VideoPortZeroMemory(&qxp_d3d_sm2_programs[index],
                                sizeof(qxp_d3d_sm2_programs[index]));
        }
    }
}

static QXP_D3D_VERTEX_CLIP_STATE *qxp_d3d_find_vertex_clip(
    QXP_DEVICE_EXTENSION *device, ULONG program_name)
{
    ULONG index;

    if (!device || !program_name) return NULL;
    for (index = 0; index < QXP_D3D_VERTEX_CLIP_CAPACITY; ++index) {
        if (device->d3d_vertex_clips[index].program_name == program_name)
            return &device->d3d_vertex_clips[index];
    }
    return NULL;
}

static ULONG qxp_d3d_current_glsl_program(
    QXP_DEVICE_EXTENSION *device)
{
    QXP_D3D_VERTEX_CLIP_STATE *clip;

    if (!device) return 0;
    if (device->d3d_vertex_program_clip) {
        clip = qxp_d3d_find_vertex_clip(
            device, device->d3d_vertex_program);
        return clip ? clip->gl_program : 0;
    }
    if (device->d3d_vertex_program == QXP_D3D_SCALABLE_POINT_PROGRAM)
        return device->d3d_glsl_program;
    if (device->d3d_vertex_program == QXP_D3D_SPECULAR_ALPHA_PROGRAM)
        return device->d3d_specular_alpha_program;
    return 0;
}

static QXP_D3D_VERTEX_CLIP_STATE *qxp_d3d_free_vertex_clip(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG index;

    if (!device) return NULL;
    for (index = 0; index < QXP_D3D_VERTEX_CLIP_CAPACITY; ++index) {
        if (!device->d3d_vertex_clips[index].program_name)
            return &device->d3d_vertex_clips[index];
    }
    return NULL;
}

static QXP_D3D_TEXTURE_STATE *qxp_d3d_find_texture(
    QXP_DEVICE_EXTENSION *device, ULONG process_id, ULONG name)
{
    QXP_D3D_TEXTURE_STATE *entry;
    ULONG probe;
    ULONG slot;

    if (!process_id || !name) return NULL;
    slot = name & (QXP_D3D_TEXTURE_CAPACITY - 1);
    for (probe = 0; probe < QXP_D3D_TEXTURE_CAPACITY; ++probe) {
        entry = &device->d3d_textures[slot];
        if (!entry->name) return NULL;
        if (entry->process_id == process_id && entry->name == name)
            return entry;
        slot = (slot + 1) & (QXP_D3D_TEXTURE_CAPACITY - 1);
    }
    return NULL;
}

/* Reserves the probe slot a later qxp_d3d_find_texture will land on. */
static QXP_D3D_TEXTURE_STATE *qxp_d3d_reserve_texture(
    QXP_DEVICE_EXTENSION *device, ULONG name)
{
    QXP_D3D_TEXTURE_STATE *entry;
    ULONG probe;
    ULONG slot;

    if (!name) return NULL;
    slot = name & (QXP_D3D_TEXTURE_CAPACITY - 1);
    for (probe = 0; probe < QXP_D3D_TEXTURE_CAPACITY; ++probe) {
        entry = &device->d3d_textures[slot];
        if (!entry->name || entry->name == QXP_D3D_TEXTURE_TOMBSTONE)
            return entry;
        slot = (slot + 1) & (QXP_D3D_TEXTURE_CAPACITY - 1);
    }
    return NULL;
}

static BOOLEAN qxp_d3d_texture_format(ULONG format, PULONG internal_format,
                                      PULONG pixel_format, PULONG pixel_type,
                                      PULONG bytes_per_pixel)
{
    switch (format) {
    case QXP_D3DFMT_R8G8B8:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_BGR;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 3;
        return TRUE;
    case QXP_D3DFMT_A8R8G8B8:
        *internal_format = QXP_GL_RGBA8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 4;
        return TRUE;
    case QXP_D3DFMT_X8R8G8B8:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 4;
        return TRUE;
    case QXP_D3DFMT_R5G6B5:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_RGB;
        *pixel_type = QXP_GL_UNSIGNED_SHORT_5_6_5;
        *bytes_per_pixel = 2;
        return TRUE;
    case QXP_D3DFMT_X1R5G5B5:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_SHORT_1_5_5_5_REV;
        *bytes_per_pixel = 2;
        return TRUE;
    case QXP_D3DFMT_A1R5G5B5:
        *internal_format = QXP_GL_RGBA8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_SHORT_1_5_5_5_REV;
        *bytes_per_pixel = 2;
        return TRUE;
    case QXP_D3DFMT_A4R4G4B4:
        *internal_format = QXP_GL_RGBA8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_SHORT_4_4_4_4_REV;
        *bytes_per_pixel = 2;
        return TRUE;
    case QXP_D3DFMT_X4R4G4B4:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_BGRA;
        *pixel_type = QXP_GL_UNSIGNED_SHORT_4_4_4_4_REV;
        *bytes_per_pixel = 2;
        return TRUE;
    case QXP_D3DFMT_R3G3B2:
        *internal_format = QXP_GL_RGB8;
        *pixel_format = QXP_GL_RGB;
        *pixel_type = QXP_GL_UNSIGNED_BYTE_3_3_2;
        *bytes_per_pixel = 1;
        return TRUE;
    case QXP_D3DFMT_A8:
        *internal_format = QXP_GL_ALPHA8;
        *pixel_format = QXP_GL_ALPHA;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 1;
        return TRUE;
    case QXP_D3DFMT_L8:
        *internal_format = QXP_GL_LUMINANCE8;
        *pixel_format = QXP_GL_LUMINANCE;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 1;
        return TRUE;
    case QXP_D3DFMT_A8L8:
        *internal_format = QXP_GL_LUMINANCE8_ALPHA8;
        *pixel_format = QXP_GL_LUMINANCE_ALPHA;
        *pixel_type = QXP_GL_UNSIGNED_BYTE;
        *bytes_per_pixel = 2;
        return TRUE;
    }
    return FALSE;
}


static ULONG qxp_d3d_mip_dimension(ULONG base, ULONG level)
{
    while (level-- && base > 1) base >>= 1;
    return base;
}

static void qxp_d3d_release_transport(QXP_DEVICE_EXTENSION *device)
{
    if (device->d3d_active && device->mesa_registers.mapped) {
        ULONG arguments[2];
        ULONG index;
#ifdef QXP_D3D_PROFILE
        LONGLONG profile_frequency;
#endif

        qxp_d3d_finish_draw_batch(device);
        if (device->d3d_glsl_program ||
            device->d3d_specular_alpha_program ||
            device->d3d_vertex_program_clip) {
            arguments[0] = 0;
            qxp_d3d_call_fbtm(device, QXP_FENUM_GL_USE_PROGRAM,
                              arguments, 1, NULL, 0);
        }
        qxp_d3d_release_sm2_programs(device, device->d3d_process_id);
        for (index = 0; index < QXP_D3D_VERTEX_CLIP_CAPACITY; ++index) {
            if (device->d3d_vertex_clips[index].gl_program) {
                arguments[0] = device->d3d_vertex_clips[index].gl_program;
                qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                                  arguments, 1, NULL, 0);
            }
        }
        if (device->d3d_glsl_program) {
            arguments[0] = device->d3d_glsl_program;
            qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                              arguments, 1, NULL, 0);
        }
        if (device->d3d_specular_alpha_program) {
            arguments[0] = device->d3d_specular_alpha_program;
            qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                              arguments, 1, NULL, 0);
        }
        if (device->d3d_neutral_texture_defined) {
            ULONG neutral_textures[3];

            neutral_textures[0] = QXP_D3D_NEUTRAL_TEXTURE_NAME;
            neutral_textures[1] =
                QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME;
            neutral_textures[2] =
                QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME;
            arguments[0] = 3;
            arguments[1] = 1;
            qxp_d3d_call_input(
                device, QXP_FENUM_GL_DELETE_TEXTURES, arguments, 2,
                neutral_textures, sizeof(neutral_textures));
        }
        qxp_debug("QXPMP D3D DRAWS");
        qxp_debug_status(device->d3d_draw_count);
        qxp_debug("QXPMP D3D DRAW IOCTLS");
        qxp_debug_status(device->d3d_draw_ioctl_count);
        qxp_debug("QXPMP D3D DRAW BATCH IOCTLS");
        qxp_debug_status(device->d3d_draw_batch_ioctl_count);
        qxp_debug("QXPMP D3D DRAW BATCH MAX");
        qxp_debug_status(device->d3d_draw_batch_max_requests);
        qxp_debug("QXPMP D3D PRESENTS");
        qxp_debug_status(device->d3d_present_count);
        qxp_debug("QXPMP D3D PRESENTS CPU");
        qxp_debug_status(device->d3d_present_cpu_count);
        qxp_debug("QXPMP D3D PRESENTS SURFTEX");
        qxp_debug_status(device->d3d_present_texture_count);
        qxp_debug("QXPMP D3D PRESENTS EMPTY");
        qxp_debug_status(device->d3d_present_empty_count);
        qxp_debug("QXPMP D3D SURFACE COPIES");
        qxp_debug_status(device->d3d_surface_copy_count);
        qxp_debug("QXPMP D3D SURFACE READBACKS");
        qxp_debug_status(device->d3d_surface_readback_count);
        qxp_debug("QXPMP D3D CLEARS");
        qxp_debug_status(device->d3d_clear_count);
        qxp_debug("QXPMP D3D UPLOADS");
        qxp_debug_status(device->d3d_texture_upload_count);
        qxp_debug("QXPMP D3D TEXTURE DEFINES");
        qxp_debug_status(device->d3d_texture_define_count);
        qxp_debug("QXPMP D3D TEXTURE DELETES");
        qxp_debug_status(device->d3d_texture_delete_count);
        qxp_debug("QXPMP D3D TEXTURE LIVE MAX");
        qxp_debug_status(device->d3d_texture_live_max);
        qxp_debug("QXPMP D3D BATCHES");
        qxp_debug_status(device->d3d_batch_count);
        qxp_debug("QXPMP D3D BATCHMAX");
        qxp_debug_status(device->d3d_batch_max_draws);
        qxp_debug("QXPMP D3D FIXEDREUSE");
        qxp_debug_status(device->d3d_fixed_state_reuse_count);
        qxp_debug("QXPMP D3D TEXREUSE");
        qxp_debug_status(device->d3d_texture_state_reuse_count);
        qxp_debug("QXPMP D3D ARRAYREUSE");
        qxp_debug_status(device->d3d_array_state_reuse_count);
        qxp_debug("QXPMP D3D INTERLEAVED");
        qxp_debug_status(device->d3d_interleaved_draw_count);
        qxp_debug("QXPMP D3D COMPACT");
        qxp_debug_status(device->d3d_compact_draw_count);
        qxp_debug("QXPMP D3D COMPACT INDEXED");
        qxp_debug_status(device->d3d_compact_indexed_draw_count);
        qxp_debug("QXPMP D3D INDEXED CANONICAL");
        qxp_debug_status(device->d3d_indexed_canonical_draw_count);
        qxp_debug("QXPMP D3D INDEXED ELEMENTS");
        qxp_debug_status(device->d3d_indexed_element_draw_count);
        qxp_debug("QXPMP D3D RAW OBJECT");
        qxp_debug_status(device->d3d_raw_indexed_object_draw_count);
#ifdef QXP_D3D_PROFILE
        qxp_debug("QXPMP D3D PERF DRAW TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_draw_ticks);
        qxp_debug("QXPMP D3D PERF PACKET TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_packet_ticks);
        qxp_debug("QXPMP D3D PERF PREPARE TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_prepare_ticks);
        qxp_debug("QXPMP D3D PERF STATE TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_state_ticks);
        qxp_debug("QXPMP D3D PERF SUBMIT TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_submit_ticks);
        qxp_debug("QXPMP D3D PERF INTERLEAVED COPY TICKS");
        qxp_d3d_profile_status64(
            device->d3d_profile_interleaved_copy_ticks);
        qxp_debug("QXPMP D3D PERF INTERLEAVED COPY BYTES");
        qxp_d3d_profile_status64(
            device->d3d_profile_interleaved_copy_bytes);
        qxp_debug("QXPMP D3D PERF INTERLEAVED COPY CALLS");
        qxp_debug_status(device->d3d_profile_interleaved_copy_calls);
        profile_frequency = 0;
        (void)VideoPortQueryPerformanceCounter(device, &profile_frequency);
        qxp_debug("QXPMP D3D PERF PORT WRITE TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_port_write_ticks);
        qxp_debug("QXPMP D3D PERF SPIN TICKS");
        qxp_d3d_profile_status64(device->d3d_profile_spin_ticks);
        qxp_debug("QXPMP D3D PERF CALIBRATION COUNT");
        qxp_debug_status(QXP_D3D_PROFILE_CALIBRATION_COUNT);
        qxp_debug("QXPMP D3D PERF FREQUENCY");
        qxp_d3d_profile_status64(profile_frequency > 0 ?
                                 (ULONGLONG)profile_frequency : 0);
#endif
        VideoPortStopTimer(device);
        qxp_mesa_mmio_write(device, 0xff4, QXP_MESAGL_MAGIC);
        qxp_mesa_mmio_write(device, 0xfbc, 0xd0320UL);
    }
    qxp_d3d_forget_sm2_programs(device, device->d3d_process_id);
    device->d3d_active = FALSE;
    device->d3d_async_supported = FALSE;
    device->d3d_rhw_clamp_logged = FALSE;
    device->d3d_array_failure_logged = FALSE;
    device->d3d_texture_failure_logged = FALSE;
    device->d3d_sm2_draw_failure_logged = FALSE;
    device->d3d_target_failure_logged = FALSE;
    device->d3d_batch_failure_logged = FALSE;
    device->d3d_texq_glsl_logged = FALSE;
    device->d3d_texture_combine3 = FALSE;
    device->d3d_texture_combine4 = FALSE;
    device->d3d_neutral_texture_defined = FALSE;
    device->d3d_table_fog_logged[0] = FALSE;
    device->d3d_table_fog_logged[1] = FALSE;
    device->d3d_token = 0;
    device->d3d_process_id = 0;
    device->d3d_references = 0;
    device->d3d_pixel_program = 0;
    device->d3d_vertex_program = 0;
    device->d3d_vertex_program_point_size = FALSE;
    device->d3d_vertex_program_clip = FALSE;
    device->d3d_texture_q_program_defined = FALSE;
    device->d3d_positional_range_program_defined = FALSE;
    device->d3d_dual_point_specular_program_defined = FALSE;
    device->d3d_quad_point_range_program_defined = FALSE;
    device->d3d_multi_point_range_program_defined = FALSE;
    device->d3d_compact_transformed_program_defined = FALSE;
    device->d3d_glsl_program = 0;
    device->d3d_glsl_program_failed = FALSE;
    VideoPortZeroMemory(device->d3d_glsl_uniforms,
                        sizeof(device->d3d_glsl_uniforms));
    device->d3d_specular_alpha_program = 0;
    device->d3d_specular_alpha_program_failed = FALSE;
    device->d3d_framebuffer = 0;
    device->d3d_copy_framebuffer = 0;
    device->d3d_present_cpu_count = 0;
    device->d3d_present_texture_count = 0;
    device->d3d_present_empty_count = 0;
    device->d3d_target_texture = 0;
    device->d3d_target_depth = 0;
    device->d3d_target_present = FALSE;
    device->d3d_draw_count = 0;
    device->d3d_draw_ioctl_count = 0;
    device->d3d_draw_batch_ioctl_count = 0;
    device->d3d_draw_batch_max_requests = 0;
    device->d3d_present_count = 0;
    device->d3d_surface_copy_count = 0;
    device->d3d_surface_readback_count = 0;
    device->d3d_clear_count = 0;
    device->d3d_texture_upload_count = 0;
    device->d3d_texture_define_count = 0;
    device->d3d_texture_delete_count = 0;
    device->d3d_texture_live_max = 0;
    device->d3d_pending_draws = 0;
    device->d3d_batch_count = 0;
    device->d3d_batch_max_draws = 0;
    device->d3d_fixed_state_reuse_count = 0;
    device->d3d_texture_state_reuse_count = 0;
    device->d3d_array_state_reuse_count = 0;
    device->d3d_interleaved_draw_count = 0;
    device->d3d_compact_draw_count = 0;
    device->d3d_compact_indexed_draw_count = 0;
    device->d3d_indexed_canonical_draw_count = 0;
#ifdef QXP_D3D_PROFILE
    device->d3d_profile_draw_ticks = 0;
    device->d3d_profile_packet_ticks = 0;
    device->d3d_profile_prepare_ticks = 0;
    device->d3d_profile_state_ticks = 0;
    device->d3d_profile_submit_ticks = 0;
    device->d3d_profile_interleaved_copy_ticks = 0;
    device->d3d_profile_interleaved_copy_bytes = 0;
    device->d3d_profile_interleaved_copy_calls = 0;
#endif
    device->d3d_draw_state_valid = FALSE;
    device->d3d_last_hwtl_valid = FALSE;
    device->d3d_last_target_present = FALSE;
    VideoPortZeroMemory(device->d3d_pixel_constants,
                        sizeof(device->d3d_pixel_constants));
    VideoPortZeroMemory(device->d3d_textures,
                        sizeof(device->d3d_textures));
    VideoPortZeroMemory(device->d3d_depths,
                        sizeof(device->d3d_depths));
    VideoPortZeroMemory(device->d3d_vertex_clips,
                        sizeof(device->d3d_vertex_clips));
    qxp_d3d_unmap_transport(device);
}

static BOOLEAN qxp_d3d_open_transport(QXP_DEVICE_EXTENSION *device,
                                      ULONG process_id)
{
    static const UCHAR signature[8] = {
        'Q', 'X', 'P', 'D', '3', 'D', 0, QXP_D3D_SIGNATURE_VERSION
    };
    QXP_PIXEL_FORMAT_DESCRIPTOR pfd;
    volatile ULONG *scratch;
    volatile UCHAR *signature_target;
    ULONG probe;
    ULONG result;
    ULONG index;
    USHORT token;

    if (device->d3d_active) {
        if (device->d3d_process_id != process_id) return FALSE;
        ++device->d3d_references;
        return TRUE;
    }
    if (!qxp_d3d_map_transport(device)) {
        qxp_debug("QXPMP D3D MAP-");
        return FALSE;
    }
    probe = qxp_mesa_mmio_read(device, 0xfb4);
    if ((probe >> 24) != QXP_MESA_PROBE_TAG ||
        ((probe >> 20) & 0x0f) != QXP_MESA_PROBE_VERSION ||
        ((probe >> 16) & 0x0f) != QXP_MESA_PROBE_FREE) {
        qxp_debug("QXPMP D3D BUSY");
        qxp_d3d_unmap_transport(device);
        return FALSE;
    }
    token = (USHORT)(process_id ^ (process_id >> 16));
    if (!token) token = 1;
    qxp_mesa_mmio_write(device, 0xfb4, token);
    result = qxp_mesa_mmio_read(device, 0xfb4);
    if (((result >> 16) & 0x0f) != QXP_MESA_PROBE_RESERVED ||
        (USHORT)result != token) {
        qxp_debug("QXPMP D3D RESERVE-");
        qxp_d3d_unmap_transport(device);
        return FALSE;
    }

    signature_target = device->mesa_fbtm.mapped +
                       QXP_MESA_FBTM_SIZE - 16;
    for (index = 0; index < 16; ++index) signature_target[index] = 0;
    for (index = 0; index < sizeof(signature); ++index) {
        signature_target[index] = signature[index];
    }
    qxp_mesa_mmio_write(device, 0xfbc, 0xa0320UL);
    result = qxp_mesa_mmio_read(device, 0xfa8);
    if (result != 0x000320a0UL) {
        qxp_debug("QXPMP D3D ACTIVATE-");
        qxp_mesa_mmio_write(device, 0xfbc, 0xd0320UL);
        qxp_d3d_unmap_transport(device);
        return FALSE;
    }

    scratch = (volatile ULONG *)(device->mesa_fifo.mapped +
                                 QXP_MESA_FIFO_SIZE - 4096);
    for (index = 0; index < 16; ++index) scratch[index] = 0;
    VideoPortZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = QXP_PFD_DRAW_TO_WINDOW | QXP_PFD_SUPPORT_OPENGL |
                  QXP_PFD_DOUBLEBUFFER;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    scratch[0] = 1;
    scratch[1] = (ULONG)(ULONG_PTR)device->mesa_registers.mapped;
    VideoPortMoveMemory((PVOID)&scratch[2], &pfd, sizeof(pfd));
    scratch[12] = token;
    qxp_mesa_mmio_write(device, 0xfe4, QXP_MESAGL_MAGIC);
    result = qxp_mesa_mmio_read(device, 0xfb4);
    if (((result >> 16) & 0x0f) != QXP_MESA_PROBE_OWNED ||
        (USHORT)result != token) {
        qxp_debug("QXPMP D3D BIND-");
        qxp_mesa_mmio_write(device, 0xfbc, 0xd0320UL);
        qxp_d3d_unmap_transport(device);
        return FALSE;
    }

    scratch[0] = 0;
    qxp_mesa_mmio_write(device, 0xffc, QXP_MESAGL_MAGIC);
    for (index = 0; index < 16; ++index) scratch[index] = 0;
    scratch[0] = QXP_MESAGL_MAGIC;
    VideoPortMoveMemory((PVOID)&scratch[1], "QXPD3D/3 XPDM", 14);
    qxp_mesa_mmio_write(device, 0xff8, QXP_MESAGL_MAGIC);

    /* Probe the static host capability register: async draw submission is
     * available only when the host returns the expected value. */
    device->d3d_async_supported =
        (qxp_mesa_mmio_read(device, QXP_MESA_ASYNC_CAP_ADDR) ==
         QXP_MESA_ASYNC_CAP_V4);
    qxp_debug(device->d3d_async_supported ?
              "QXPMP D3D ASYNC+" : "QXPMP D3D ASYNC-");

    device->d3d_texture_combine3 = qxp_d3d_host_extension(
        device, token, "GL_ATI_texture_env_combine3");
    device->d3d_texture_combine4 = qxp_d3d_host_extension(
        device, token, "GL_NV_texture_env_combine4");
    qxp_debug(device->d3d_texture_combine3 ?
              "QXPMP D3D COMBINE3+" : "QXPMP D3D COMBINE3-");
    qxp_debug(device->d3d_texture_combine4 ?
              "QXPMP D3D COMBINE4+" : "QXPMP D3D COMBINE4-");
    device->d3d_active = TRUE;
    device->d3d_rhw_clamp_logged = FALSE;
    device->d3d_array_failure_logged = FALSE;
    device->d3d_texture_failure_logged = FALSE;
    device->d3d_sm2_draw_failure_logged = FALSE;
    device->d3d_target_failure_logged = FALSE;
    device->d3d_batch_failure_logged = FALSE;
    device->d3d_texq_glsl_logged = FALSE;
    device->d3d_texgen_camera_position_logged = FALSE;
    device->d3d_neutral_texture_defined = FALSE;
    device->d3d_table_fog_logged[0] = FALSE;
    device->d3d_table_fog_logged[1] = FALSE;
    device->d3d_token = token;
    device->d3d_process_id = process_id;
    device->d3d_references = 1;
    device->d3d_vertex_program = 0;
    device->d3d_vertex_program_point_size = FALSE;
    device->d3d_vertex_program_clip = FALSE;
    device->d3d_texture_q_program_defined = FALSE;
    device->d3d_positional_range_program_defined = FALSE;
    device->d3d_dual_point_specular_program_defined = FALSE;
    device->d3d_quad_point_range_program_defined = FALSE;
    device->d3d_multi_point_range_program_defined = FALSE;
    device->d3d_compact_transformed_program_defined = FALSE;
    device->d3d_glsl_program = 0;
    device->d3d_glsl_program_failed = FALSE;
    VideoPortZeroMemory(device->d3d_glsl_uniforms,
                        sizeof(device->d3d_glsl_uniforms));
    device->d3d_specular_alpha_program = 0;
    device->d3d_specular_alpha_program_failed = FALSE;
    VideoPortZeroMemory(device->d3d_vertex_clips,
                        sizeof(device->d3d_vertex_clips));
    device->d3d_draw_count = 0;
    device->d3d_draw_ioctl_count = 0;
    device->d3d_draw_batch_ioctl_count = 0;
    device->d3d_draw_batch_max_requests = 0;
    device->d3d_present_count = 0;
    device->d3d_surface_copy_count = 0;
    device->d3d_surface_readback_count = 0;
    device->d3d_clear_count = 0;
    device->d3d_texture_upload_count = 0;
    device->d3d_texture_define_count = 0;
    device->d3d_texture_delete_count = 0;
    device->d3d_texture_live_max = 0;
    device->d3d_pending_draws = 0;
    device->d3d_batch_count = 0;
    device->d3d_batch_max_draws = 0;
    device->d3d_fixed_state_reuse_count = 0;
    device->d3d_texture_state_reuse_count = 0;
    device->d3d_array_state_reuse_count = 0;
    device->d3d_interleaved_draw_count = 0;
    device->d3d_compact_draw_count = 0;
    device->d3d_compact_indexed_draw_count = 0;
    device->d3d_indexed_canonical_draw_count = 0;
    device->d3d_indexed_element_draw_count = 0;
#ifdef QXP_D3D_PROFILE
    device->d3d_profile_draw_ticks = 0;
    device->d3d_profile_packet_ticks = 0;
    device->d3d_profile_prepare_ticks = 0;
    device->d3d_profile_state_ticks = 0;
    device->d3d_profile_submit_ticks = 0;
    device->d3d_profile_interleaved_copy_ticks = 0;
    device->d3d_profile_interleaved_copy_bytes = 0;
    device->d3d_profile_interleaved_copy_calls = 0;
    device->d3d_profile_calibration_sink = 0;
    qxp_d3d_profile_calibrate(device);
#endif
    device->d3d_draw_state_valid = FALSE;
    device->d3d_last_hwtl_valid = FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, token);
    VideoPortStartTimer(device);
    qxp_debug("QXPMP D3D OPEN+");
    return TRUE;
}

static BOOLEAN qxp_d3d_close_transport(QXP_DEVICE_EXTENSION *device,
                                       ULONG process_id)
{
    if (!device->d3d_active || device->d3d_process_id != process_id ||
        !device->d3d_references) {
        return FALSE;
    }
    if (--device->d3d_references) return TRUE;
    qxp_d3d_release_transport(device);
    qxp_debug("QXPMP D3D CLOSE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_texture_define_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_TEXTURE_DEFINE_REQUEST *request)
{
    QXP_D3D_TEXTURE_STATE *texture;
    QXP_D3D_TEXTURE_STATE *empty = NULL;
    ULONG bind_arguments[2];
    ULONG store_arguments[2];
    ULONG image_arguments[10];
    ULONG internal_format;
    ULONG pixel_format;
    ULONG pixel_type;
    ULONG bytes_per_pixel;
    ULONG bind_target;
    ULONG image_target;
    ULONG face;
    BOOLEAN volume;
    volatile ULONG *fifo;
    ULONG fifo_start;

    /* Portable structural checks (version, identity, type, face, depth,
     * dimensions, levels, volume constraints) are enforced by the shared
     * contract; the stateful identity and mip matching below run before any
     * FIFO/GL work. */
    if (!device->d3d_active || !request ||
        request->process_id != device->d3d_process_id ||
        !qxp_packet_texture_define_contract(request) ||
        !qxp_d3d_texture_format(request->format, &internal_format,
                                &pixel_format, &pixel_type,
                                &bytes_per_pixel) ||
        !qxp_packet_texture_allocation_contract(
            request, bytes_per_pixel)) {
        qxp_debug("QXPMP D3D DEFINE REJECT");
        qxp_debug_status(1);
        return FALSE;
    }
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    if (!texture) {
        if (request->level ||
            (request->texture_type == QXP_D3D_TEXTURE_CUBE &&
             request->face)) {
            qxp_debug("QXPMP D3D DEFINE REJECT");
            qxp_debug_status(2);
            return FALSE;
        }
        empty = qxp_d3d_reserve_texture(device, request->texture_name);
        if (!empty) {
            qxp_debug("QXPMP D3D DEFINE REJECT");
            qxp_debug_status(3);
            qxp_debug_status(device->d3d_texture_define_count);
            qxp_debug_status(device->d3d_texture_delete_count);
            return FALSE;
        }
        texture = empty;
        ++device->d3d_texture_define_count;
        if (device->d3d_texture_define_count -
                device->d3d_texture_delete_count >
            device->d3d_texture_live_max) {
            device->d3d_texture_live_max =
                device->d3d_texture_define_count -
                device->d3d_texture_delete_count;
        }
    } else if (texture->texture_type != request->texture_type ||
               texture->format != request->format ||
               texture->levels != request->total_levels ||
               request->width !=
                   qxp_d3d_mip_dimension(texture->width, request->level) ||
               request->height !=
                   qxp_d3d_mip_dimension(texture->height, request->level) ||
               request->depth !=
                   qxp_d3d_mip_dimension(texture->depth, request->level)) {
        qxp_debug("QXPMP D3D DEFINE REJECT");
        qxp_debug_status(4);
        qxp_debug_status(texture->texture_type);
        qxp_debug_status(texture->format);
        qxp_debug_status(texture->levels);
        qxp_debug_status(texture->width);
        qxp_debug_status(texture->height);
        qxp_debug_status(request->total_levels);
        return FALSE;
    }
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) {
        qxp_debug("QXPMP D3D DEFINE REJECT");
        qxp_debug_status(5);
        return FALSE;
    }
    fifo_start = fifo[0];

    volume = request->texture_type == QXP_D3D_TEXTURE_VOLUME;
    face = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
           request->face : 0;
    if (volume) {
        bind_target = QXP_GL_TEXTURE_3D;
        image_target = QXP_GL_TEXTURE_3D;
    } else {
        bind_target = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
                      QXP_GL_TEXTURE_CUBE_MAP : QXP_GL_TEXTURE_2D;
        image_target = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
                       QXP_GL_TEXTURE_CUBE_MAP_POSITIVE_X + face :
                       QXP_GL_TEXTURE_2D;
    }
    bind_arguments[0] = bind_target;
    bind_arguments[1] = request->texture_name;
    store_arguments[0] = QXP_GL_UNPACK_ALIGNMENT;
    store_arguments[1] = 1;
    image_arguments[0] = image_target;
    image_arguments[1] = request->level;
    image_arguments[2] = internal_format;
    image_arguments[3] = request->width;
    image_arguments[4] = request->height;
    if (volume) {
        /* glTexImage3D(target, level, internalformat, width, height, depth,
         * border, format, type, NULL): exactly ten arguments. */
        image_arguments[5] = request->depth;
        image_arguments[6] = 0;
        image_arguments[7] = pixel_format;
        image_arguments[8] = pixel_type;
        image_arguments[9] = 0;
    } else {
        image_arguments[5] = 0;
        image_arguments[6] = pixel_format;
        image_arguments[7] = pixel_type;
        image_arguments[8] = 0;
    }
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PIXEL_STORE_I,
                             store_arguments, 2) ||
        (volume ?
             !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_IMAGE_3D,
                                image_arguments, 10, NULL, 0) :
             !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_IMAGE_2D,
                                image_arguments, 9, NULL, 0))) {
        qxp_debug("QXPMP D3D DEFINE REJECT");
        qxp_debug_status(6);
        fifo[0] = fifo_start;
        return FALSE;
    }

    if (!request->level) {
        texture->process_id = request->process_id;
        texture->name = request->texture_name;
        texture->width = request->width;
        texture->height = request->height;
        texture->depth = request->depth;
        texture->format = request->format;
        texture->texture_type = request->texture_type;
        texture->levels = request->total_levels;
        texture->framebuffer_complete = FALSE;
        texture->copy_complete = FALSE;
    }
    texture->defined_levels[face] |= 1UL << request->level;
    return TRUE;
}

static BOOLEAN qxp_d3d_texture_upload_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_TEXTURE_UPLOAD_REQUEST *request, ULONG request_bytes)
{
    /* Tight unpack state for x-fastest, then y, then z volume payloads.
     * Alignment one plus zeroed row length, image height, and skip counts so
     * the packed sub-box maps one-to-one onto the requested region. */
    static const ULONG unpack_defaults[6][2] = {
        { QXP_GL_UNPACK_ALIGNMENT, 1 },
        { QXP_GL_UNPACK_ROW_LENGTH, 0 },
        { QXP_GL_UNPACK_IMAGE_HEIGHT, 0 },
        { QXP_GL_UNPACK_SKIP_PIXELS, 0 },
        { QXP_GL_UNPACK_SKIP_ROWS, 0 },
        { QXP_GL_UNPACK_SKIP_IMAGES, 0 }
    };
    QXP_D3D_TEXTURE_STATE *texture;
    ULONG bind_arguments[2];
    ULONG store_arguments[2];
    ULONG image_arguments[11];
    ULONG internal_format;
    ULONG pixel_format;
    ULONG pixel_type;
    ULONG bytes_per_pixel;
    ULONG bind_target;
    ULONG image_target;
    ULONG face;
    ULONG header_bytes;
    ULONG store_index;
    BOOLEAN volume;
    volatile ULONG *fifo;
    ULONG fifo_start;

    header_bytes = sizeof(*request) - sizeof(request->data);
    if (!device->d3d_active || !request || request_bytes < header_bytes ||
        request->process_id != device->d3d_process_id ||
        !qxp_d3d_texture_format(request->format, &internal_format,
                                &pixel_format, &pixel_type,
                                &bytes_per_pixel)) return FALSE;
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    face = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
           request->face : 0;
    /* Stateful identity, format/type, defined-level, and mip matching stay
     * here and complete before any FIFO/GL call.  The face bound guards the
     * defined_levels index the portable contract validates afterwards. */
    if (!texture || texture->texture_type != request->texture_type ||
        texture->format != request->format ||
        request->level >= texture->levels || face >= 6 ||
        !(texture->defined_levels[face] & (1UL << request->level)))
        return FALSE;
    /* Portable structural and arithmetic validation: version, type, face,
     * z/depth rules, checked XYZ sub-box against the requested mip level,
     * overflow-checked width*height*depth*bpp, and exact packet length. */
    if (!qxp_packet_texture_upload_contract(
            request, request_bytes, texture->width, texture->height,
            texture->depth, texture->levels, bytes_per_pixel)) return FALSE;

    volume = request->texture_type == QXP_D3D_TEXTURE_VOLUME;
    if (volume) {
        bind_target = QXP_GL_TEXTURE_3D;
        image_target = QXP_GL_TEXTURE_3D;
    } else {
        bind_target = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
                      QXP_GL_TEXTURE_CUBE_MAP : QXP_GL_TEXTURE_2D;
        image_target = request->texture_type == QXP_D3D_TEXTURE_CUBE ?
                       QXP_GL_TEXTURE_CUBE_MAP_POSITIVE_X + face :
                       QXP_GL_TEXTURE_2D;
    }
    bind_arguments[0] = bind_target;
    bind_arguments[1] = request->texture_name;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    if (volume) {
        for (store_index = 0; store_index < 6; ++store_index) {
            store_arguments[0] = unpack_defaults[store_index][0];
            store_arguments[1] = unpack_defaults[store_index][1];
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PIXEL_STORE_I,
                                     store_arguments, 2)) {
                fifo[0] = fifo_start;
                return FALSE;
            }
        }
        /* glTexSubImage3D(target, level, x, y, z, width, height, depth,
         * format, type, data): exactly eleven arguments. */
        image_arguments[0] = image_target;
        image_arguments[1] = request->level;
        image_arguments[2] = request->x;
        image_arguments[3] = request->y;
        image_arguments[4] = request->z;
        image_arguments[5] = request->width;
        image_arguments[6] = request->height;
        image_arguments[7] = request->depth;
        image_arguments[8] = pixel_format;
        image_arguments[9] = pixel_type;
        image_arguments[10] = 1;
        if (!qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_SUB_IMAGE_3D,
                               image_arguments, 11, request->data,
                               request->data_bytes)) {
            fifo[0] = fifo_start;
            return FALSE;
        }
    } else {
        store_arguments[0] = QXP_GL_UNPACK_ALIGNMENT;
        store_arguments[1] = 1;
        image_arguments[0] = image_target;
        image_arguments[1] = request->level;
        image_arguments[2] = request->x;
        image_arguments[3] = request->y;
        image_arguments[4] = request->width;
        image_arguments[5] = request->height;
        image_arguments[6] = pixel_format;
        image_arguments[7] = pixel_type;
        image_arguments[8] = 1;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PIXEL_STORE_I,
                                 store_arguments, 2) ||
            !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_SUB_IMAGE_2D,
                               image_arguments, 9, request->data,
                               request->data_bytes)) {
            fifo[0] = fifo_start;
            return FALSE;
        }
    }
    return TRUE;
}

static QXP_D3D_DEPTH_STATE *qxp_d3d_find_depth(
    QXP_DEVICE_EXTENSION *device, ULONG process_id, ULONG surface_name)
{
    ULONG index;

    for (index = 0; index < QXP_D3D_DEPTH_CAPACITY; ++index) {
        if (device->d3d_depths[index].process_id == process_id &&
            device->d3d_depths[index].surface_name == surface_name) {
            return &device->d3d_depths[index];
        }
    }
    return NULL;
}

static BOOLEAN qxp_d3d_generate_name(QXP_DEVICE_EXTENSION *device,
                                     ULONG function, PULONG name)
{
    ULONG arguments[2];

    if (!name) return FALSE;
    *name = 0;
    arguments[0] = 1;
    arguments[1] = 0;
    return qxp_d3d_call_output(device, function, arguments, 2,
                               name, sizeof(*name)) && *name != 0;
}

static BOOLEAN qxp_d3d_bind_default_target(QXP_DEVICE_EXTENSION *device)
{
    ULONG arguments[2];

    if (!device->d3d_target_texture && !device->d3d_target_depth)
        return TRUE;
    arguments[0] = QXP_GL_FRAMEBUFFER;
    arguments[1] = 0;
    if (!qxp_d3d_call_fbtm(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                           arguments, 2, NULL, 0)) return FALSE;
    device->d3d_target_texture = 0;
    device->d3d_target_depth = 0;
    device->d3d_target_present = FALSE;
    qxp_d3d_invalidate_draw_state(device);
    return TRUE;
}

static BOOLEAN qxp_d3d_target_transport(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_TARGET_REQUEST *request)
{
    QXP_D3D_TEXTURE_STATE *texture;
    QXP_D3D_DEPTH_STATE *depth = NULL;
    QXP_D3D_DEPTH_STATE *empty = NULL;
    ULONG framebuffer_arguments[2];
    ULONG texture_arguments[5];
    ULONG renderbuffer_arguments[2];
    ULONG storage_arguments[4];
    ULONG depth_arguments[4];
    ULONG stencil_arguments[4];
    ULONG status_arguments[1];
    ULONG delete_arguments[2];
    ULONG renderbuffer_name = 0;
    ULONG old_renderbuffer_name = 0;
    ULONG internal_depth_format = 0;
    ULONG status = 0;
    ULONG index;
    ULONG depth_plan;
    volatile ULONG *fifo;
    ULONG fifo_start;
    BOOLEAN new_depth = FALSE;
    BOOLEAN recreated_depth = FALSE;
    ULONG failure_reason = 0;
    ULONG existing_depth_width = 0;
    ULONG existing_depth_height = 0;
    ULONG existing_depth_format = 0;
#ifdef QXP_VERBOSE_TRACE
    ULONG old_target_read_arguments[7];
#endif

    /* The validation below was one short-circuiting expression, which told a
     * caller only that the bind failed and never which of a dozen distinct
     * conditions rejected it.  The ordered checks preserve the original
     * evaluation order and outcomes exactly and add a reason code, so a
     * refusal names its own cause instead of requiring another cycle to
     * guess.  The same instrument on SM2 draws paid for itself immediately. */
    if (!device->d3d_active) { failure_reason = 1; goto rejected; }
    if (!request) { failure_reason = 2; goto rejected; }
    if (request->version != QXP_D3D_TRANSPORT_VERSION) {
        failure_reason = 3;
        goto rejected;
    }
    if (request->process_id != device->d3d_process_id) {
        failure_reason = 4;
        goto rejected;
    }
    if (request->flags & ~QXP_D3D_TARGET_PRESENT) {
        failure_reason = 5;
        goto rejected;
    }
    if (!request->texture_name) {
        if (request->width || request->height || request->depth_name ||
            request->depth_width || request->depth_height ||
            request->depth_format || request->flags) {
            failure_reason = 6;
            goto rejected;
        }
        return qxp_d3d_bind_default_target(device);
    }
    if (!request->width || !request->height) {
        failure_reason = 7;
        goto rejected;
    }
    if (request->width > 2048 || request->height > 2048) {
        failure_reason = 8;
        goto rejected;
    }
    if (!!request->depth_name != !!request->depth_width ||
        !!request->depth_name != !!request->depth_height ||
        !!request->depth_name != !!request->depth_format) {
        failure_reason = 9;
        goto rejected;
    }
    if (request->depth_format && request->depth_format != QXP_D3DFMT_D16 &&
        request->depth_format != QXP_D3DFMT_D24S8 &&
        request->depth_format != QXP_D3DFMT_D24X8) {
        failure_reason = 10;
        goto rejected;
    }
    if (request->depth_width > 2048 || request->depth_height > 2048) {
        failure_reason = 17;
        goto rejected;
    }
    if (request->depth_name &&
        (request->depth_width < request->width ||
         request->depth_height < request->height)) {
        failure_reason = 18;
        goto rejected;
    }
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    if (!texture) { failure_reason = 11; goto rejected; }
    if (texture->texture_type != QXP_D3D_TEXTURE_2D) {
        failure_reason = 12;
        goto rejected;
    }
    if (texture->width != request->width ||
        texture->height != request->height) {
        failure_reason = 13;
        goto rejected;
    }
    if (!(texture->defined_levels[0] & 1UL)) {
        failure_reason = 14;
        goto rejected;
    }
    if (texture->format != QXP_D3DFMT_X8R8G8B8 &&
        texture->format != QXP_D3DFMT_A8R8G8B8 &&
        texture->format != QXP_D3DFMT_R5G6B5) {
        failure_reason = 15;
        goto rejected;
    }
    if (request->depth_name) {
        depth = qxp_d3d_find_depth(device, request->process_id,
                                   request->depth_name);
        if (depth) {
            /* Direct3D 9 requires the depth-stencil surface to be at least
             * as large as the render target, not exactly its size, and the
             * runtime does bind a small render target against the larger
             * swap-chain depth buffer.  A change from the recorded true
             * dimensions identifies a recreated guest surface generation;
             * only an unchanged generation must match the recorded format. */
            depth_plan = qxp_packet_target_depth_plan(
                request, depth->width, depth->height, depth->format);
            if (depth_plan == QXP_PACKET_TARGET_DEPTH_REJECT) {
                /* Report what the existing surface actually is, so a
                 * mismatch identifies itself rather than needing a further
                 * cycle to distinguish a size difference from a format one. */
                existing_depth_width = depth->width;
                existing_depth_height = depth->height;
                existing_depth_format = depth->format;
                failure_reason = 16;
                goto rejected;
            }
            if (depth_plan == QXP_PACKET_TARGET_DEPTH_RECREATE) {
                empty = depth;
                old_renderbuffer_name = depth->renderbuffer_name;
                new_depth = TRUE;
                recreated_depth = TRUE;
            } else {
                renderbuffer_name = depth->renderbuffer_name;
            }
        } else {
            for (index = 0; index < QXP_D3D_DEPTH_CAPACITY; ++index) {
                if (!device->d3d_depths[index].surface_name) {
                    empty = &device->d3d_depths[index];
                    break;
                }
            }
            if (!empty) return FALSE;
            new_depth = TRUE;
        }
        internal_depth_format = request->depth_format == QXP_D3DFMT_D16 ?
                                QXP_GL_DEPTH_COMPONENT16 :
                                (request->depth_format == QXP_D3DFMT_D24X8 ?
                                 QXP_GL_DEPTH_COMPONENT24 :
                                 QXP_GL_DEPTH24_STENCIL8);
    }
    if (!new_depth &&
        device->d3d_target_texture == request->texture_name &&
        device->d3d_target_depth == request->depth_name &&
        device->d3d_target_present ==
            !!(request->flags & QXP_D3D_TARGET_PRESENT)) return TRUE;

#ifdef QXP_VERBOSE_TRACE
    if (device->d3d_target_texture && device->d3d_framebuffer &&
        device->mesa_fbtm.mapped) {
        old_target_read_arguments[0] = 8;
        old_target_read_arguments[1] = 56;
        old_target_read_arguments[2] = 1;
        old_target_read_arguments[3] = 1;
        old_target_read_arguments[4] = QXP_GL_BGRA;
        old_target_read_arguments[5] = QXP_GL_UNSIGNED_BYTE;
        old_target_read_arguments[6] = 0;
        VideoPortZeroMemory(device->mesa_fbtm.mapped, sizeof(ULONG));
        if (qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_READ_PIXELS,
                old_target_read_arguments, 7, NULL, 0)) {
            qxp_debug("QXPMP D3D TARGET OLD PIXEL");
            qxp_debug_status(device->d3d_target_texture);
            qxp_debug_status(request->texture_name);
            qxp_debug_status(
                *(volatile ULONG *)device->mesa_fbtm.mapped);
        } else {
            qxp_debug("QXPMP D3D TARGET OLD PIXEL-");
        }
    }
#endif

    if (!device->d3d_framebuffer &&
        !qxp_d3d_generate_name(device, QXP_FENUM_GL_GEN_FRAMEBUFFERS,
                               &device->d3d_framebuffer)) return FALSE;
    if (new_depth && !qxp_d3d_generate_name(
            device, QXP_FENUM_GL_GEN_RENDERBUFFERS,
            &renderbuffer_name)) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    framebuffer_arguments[0] = QXP_GL_FRAMEBUFFER;
    framebuffer_arguments[1] = device->d3d_framebuffer;
    texture_arguments[0] = QXP_GL_FRAMEBUFFER;
    texture_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    texture_arguments[2] = QXP_GL_TEXTURE_2D;
    texture_arguments[3] = request->texture_name;
    texture_arguments[4] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             framebuffer_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             texture_arguments, 5)) goto fail;
    if (request->depth_name) {
        renderbuffer_arguments[0] = QXP_GL_RENDERBUFFER;
        renderbuffer_arguments[1] = renderbuffer_name;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_RENDERBUFFER,
                                 renderbuffer_arguments, 2)) goto fail;
        if (new_depth) {
            storage_arguments[0] = QXP_GL_RENDERBUFFER;
            storage_arguments[1] = internal_depth_format;
            storage_arguments[2] = request->depth_width;
            storage_arguments[3] = request->depth_height;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_RENDERBUFFER_STORAGE,
                    storage_arguments, 4)) goto fail;
        }
    }
    depth_arguments[0] = QXP_GL_FRAMEBUFFER;
    depth_arguments[1] = QXP_GL_DEPTH_ATTACHMENT;
    depth_arguments[2] = QXP_GL_RENDERBUFFER;
    depth_arguments[3] = renderbuffer_name;
    stencil_arguments[0] = QXP_GL_FRAMEBUFFER;
    stencil_arguments[1] = QXP_GL_STENCIL_ATTACHMENT;
    stencil_arguments[2] = QXP_GL_RENDERBUFFER;
    stencil_arguments[3] = request->depth_format == QXP_D3DFMT_D24S8 ?
                           renderbuffer_name : 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_RENDERBUFFER,
                             depth_arguments, 4) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_RENDERBUFFER,
                             stencil_arguments, 4)) goto fail;
    if (recreated_depth) {
        for (index = 0; index < QXP_D3D_TEXTURE_CAPACITY; ++index) {
            if (device->d3d_textures[index].framebuffer_complete &&
                device->d3d_textures[index].framebuffer_depth_name ==
                    request->depth_name) {
                device->d3d_textures[index].framebuffer_complete = FALSE;
            }
        }
    }
    if (new_depth || !texture->framebuffer_complete ||
        texture->framebuffer_depth_name != request->depth_name ||
        texture->framebuffer_depth_format != request->depth_format) {
        status_arguments[0] = QXP_GL_FRAMEBUFFER;
        if (!qxp_d3d_call_fbtm_result(
                device, QXP_FENUM_GL_CHECK_FRAMEBUFFER_STATUS,
                status_arguments, 1, NULL, 0, &status)) goto fail;
        if (status != QXP_GL_FRAMEBUFFER_COMPLETE) goto fail_bound;
        texture->framebuffer_depth_name = request->depth_name;
        texture->framebuffer_depth_format = request->depth_format;
        texture->framebuffer_complete = TRUE;
    }

    if (new_depth) {
        empty->process_id = request->process_id;
        empty->surface_name = request->depth_name;
        empty->renderbuffer_name = renderbuffer_name;
        empty->width = request->depth_width;
        empty->height = request->depth_height;
        empty->format = request->depth_format;
    }
    if (recreated_depth && old_renderbuffer_name) {
        delete_arguments[0] = 1;
        delete_arguments[1] = old_renderbuffer_name;
        if (!qxp_d3d_call_input(
                device, QXP_FENUM_GL_DELETE_RENDERBUFFERS,
                delete_arguments, 2, &old_renderbuffer_name,
                sizeof(old_renderbuffer_name))) {
            qxp_debug("QXPMP D3D DEPTH OLD DELETE-");
        }
    }
    device->d3d_target_texture = request->texture_name;
    device->d3d_target_depth = request->depth_name;
    device->d3d_target_present =
        !!(request->flags & QXP_D3D_TARGET_PRESENT);
    qxp_d3d_invalidate_draw_state(device);
#ifdef QXP_VERBOSE_TRACE
    qxp_debug("QXPMP D3D TARGET+");
    qxp_debug_status(request->texture_name);
    qxp_debug_status(request->width);
    qxp_debug_status(request->height);
    qxp_debug_status(request->depth_name);
    qxp_debug_status(request->flags);
#endif
    return TRUE;

rejected:
    /* Report once per device so a repeating bind cannot flood the log, which
     * is what the SM2 draw instrument does for the same reason. */
    if (device && !device->d3d_target_failure_logged) {
        device->d3d_target_failure_logged = TRUE;
        qxp_debug("QXPMP D3D TARGET REJECT");
        qxp_debug_status(failure_reason);
        if (request) {
            qxp_debug_status(request->texture_name);
            qxp_debug_status(request->width);
            qxp_debug_status(request->height);
            qxp_debug_status(request->depth_name);
            qxp_debug_status(request->depth_width);
            qxp_debug_status(request->depth_height);
            qxp_debug_status(request->depth_format);
            qxp_debug_status(request->flags);
            qxp_debug_status(existing_depth_width);
            qxp_debug_status(existing_depth_height);
            qxp_debug_status(existing_depth_format);
        }
    }
    return FALSE;

fail:
    fifo[0] = fifo_start;
fail_bound:
    qxp_debug("QXPMP D3D TARGET-");
    qxp_debug_status(request->texture_name);
    qxp_debug_status(request->depth_name);
    qxp_debug_status(request->depth_format);
    qxp_debug_status(status);
    qxp_d3d_bind_default_target(device);
    if (new_depth && renderbuffer_name) {
        delete_arguments[0] = 1;
        delete_arguments[1] = renderbuffer_name;
        qxp_d3d_call_input(device, QXP_FENUM_GL_DELETE_RENDERBUFFERS,
                           delete_arguments, 2, &renderbuffer_name,
                           sizeof(renderbuffer_name));
    }
    return FALSE;
}

static BOOLEAN qxp_d3d_restore_target_framebuffer(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG attach_arguments[5];

    if (!device || !device->d3d_framebuffer) return FALSE;
    bind_arguments[0] = QXP_GL_FRAMEBUFFER;
    bind_arguments[1] = device->d3d_framebuffer;
    if (!qxp_d3d_call_fbtm(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                           bind_arguments, 2, NULL, 0)) goto failed;
    if (device->d3d_target_texture) {
        attach_arguments[0] = QXP_GL_FRAMEBUFFER;
        attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
        attach_arguments[2] = QXP_GL_TEXTURE_2D;
        attach_arguments[3] = device->d3d_target_texture;
        attach_arguments[4] = 0;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                attach_arguments, 5, NULL, 0)) goto failed;
    }
    qxp_d3d_invalidate_draw_state(device);
    return TRUE;

failed:
    device->d3d_target_texture = 0;
    device->d3d_target_depth = 0;
    device->d3d_target_present = FALSE;
    qxp_d3d_invalidate_draw_state(device);
    return FALSE;
}

static BOOLEAN qxp_d3d_surface_copy_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SURFACE_COPY_REQUEST *request)
{
    QXP_D3D_TEXTURE_STATE *source;
    QXP_D3D_TEXTURE_STATE *destination;
    ULONG bind_read_arguments[2];
    ULONG bind_draw_arguments[2];
    ULONG read_attach_arguments[5];
    ULONG draw_attach_arguments[5];
    ULONG read_status_arguments[1];
    ULONG draw_status_arguments[1];
    ULONG blit_arguments[10];
    ULONG restore_bind_arguments[2];
    ULONG restore_attach_arguments[5];
    ULONG status = 0;
    ULONG fifo_start;
    BOOLEAN source_is_target;
    BOOLEAN checks_cached;
    volatile ULONG *fifo;

    if (!device || !device->d3d_active ||
        !qxp_packet_surface_copy_contract(request) ||
        request->process_id != device->d3d_process_id ||
        !device->d3d_framebuffer ||
        request->destination_texture_name == device->d3d_target_texture ||
        request->destination_texture_name == device->d3d_target_depth)
        return FALSE;
    source = qxp_d3d_find_texture(
        device, request->process_id, request->source_texture_name);
    destination = qxp_d3d_find_texture(
        device, request->process_id, request->destination_texture_name);
    if (!source || !destination ||
        source->texture_type != QXP_D3D_TEXTURE_2D ||
        destination->texture_type != QXP_D3D_TEXTURE_2D ||
        source->width != request->source_width ||
        source->height != request->source_height ||
        source->format != request->source_format ||
        destination->width != request->destination_width ||
        destination->height != request->destination_height ||
        destination->format != request->destination_format ||
        !(source->defined_levels[0] & 1UL) ||
        !(destination->defined_levels[0] & 1UL)) return FALSE;
    checks_cached = source->copy_complete && destination->copy_complete;
    if (!device->d3d_copy_framebuffer &&
        !qxp_d3d_generate_name(
            device, QXP_FENUM_GL_GEN_FRAMEBUFFERS,
            &device->d3d_copy_framebuffer)) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    qxp_d3d_finish_draw_batch(device);

    fifo_start = fifo[0];
    source_is_target = request->source_texture_name ==
                       device->d3d_target_texture;
    bind_read_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    bind_read_arguments[1] = device->d3d_framebuffer;
    bind_draw_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
    bind_draw_arguments[1] = device->d3d_copy_framebuffer;
    read_attach_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    read_attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    read_attach_arguments[2] = QXP_GL_TEXTURE_2D;
    read_attach_arguments[3] = request->source_texture_name;
    read_attach_arguments[4] = 0;
    draw_attach_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
    draw_attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    draw_attach_arguments[2] = QXP_GL_TEXTURE_2D;
    draw_attach_arguments[3] = request->destination_texture_name;
    draw_attach_arguments[4] = 0;
    read_status_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    draw_status_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_read_arguments, 2) ||
        (!source_is_target &&
         !qxp_d3d_fifo_submit(
             device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
             read_attach_arguments, 5)) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_draw_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             draw_attach_arguments, 5)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    /* FBO completeness is intrinsic to a texture's format and defined
     * levels, so a pair already verified complete is not re-checked: the
     * binds, blit and restore all ride the one ordered FIFO and processFifo
     * drains it whole, so the copy then needs no doorbell of its own.  Only
     * a first-seen texture pays the synchronous validation. */
    if (!checks_cached) {
        if (!qxp_d3d_call_fbtm_result(
                device, QXP_FENUM_GL_CHECK_FRAMEBUFFER_STATUS,
                read_status_arguments, 1, NULL, 0, &status) ||
            status != QXP_GL_FRAMEBUFFER_COMPLETE ||
            !qxp_d3d_call_fbtm_result(
                device, QXP_FENUM_GL_CHECK_FRAMEBUFFER_STATUS,
                draw_status_arguments, 1, NULL, 0, &status) ||
            status != QXP_GL_FRAMEBUFFER_COMPLETE) {
            qxp_debug("QXPMP D3D SURFACE COPY FBO-");
            qxp_debug_status(status);
            (void)qxp_d3d_restore_target_framebuffer(device);
            return FALSE;
        }
        source->copy_complete = TRUE;
        destination->copy_complete = TRUE;
    }

    blit_arguments[0] = (ULONG)request->source_rect.left;
    blit_arguments[2] = (ULONG)request->source_rect.right;
    if (source_is_target && device->d3d_target_present) {
        blit_arguments[1] = request->source_height -
                            (ULONG)request->source_rect.top;
        blit_arguments[3] = request->source_height -
                            (ULONG)request->source_rect.bottom;
    } else {
        blit_arguments[1] = (ULONG)request->source_rect.top;
        blit_arguments[3] = (ULONG)request->source_rect.bottom;
    }
    blit_arguments[4] = (ULONG)request->destination_rect.left;
    blit_arguments[5] = (ULONG)request->destination_rect.top;
    blit_arguments[6] = (ULONG)request->destination_rect.right;
    blit_arguments[7] = (ULONG)request->destination_rect.bottom;
    blit_arguments[8] = QXP_GL_COLOR_BUFFER_BIT;
    blit_arguments[9] = QXP_GL_NEAREST;
    fifo_start = fifo[0];
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLIT_FRAMEBUFFER,
                             blit_arguments, 10)) {
        fifo[0] = fifo_start;
        (void)qxp_d3d_restore_target_framebuffer(device);
        return FALSE;
    }
    /* Restore the render target through the same FIFO rather than two more
     * synchronous calls.  It is queued after the blit and drained with it,
     * so d3d_framebuffer is reattached in order before any later command. */
    restore_bind_arguments[0] = QXP_GL_FRAMEBUFFER;
    restore_bind_arguments[1] = device->d3d_framebuffer;
    fifo_start = fifo[0];
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             restore_bind_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    if (device->d3d_target_texture) {
        restore_attach_arguments[0] = QXP_GL_FRAMEBUFFER;
        restore_attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
        restore_attach_arguments[2] = QXP_GL_TEXTURE_2D;
        restore_attach_arguments[3] = device->d3d_target_texture;
        restore_attach_arguments[4] = 0;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                restore_attach_arguments, 5)) {
            fifo[0] = fifo_start;
            return FALSE;
        }
    }
    qxp_d3d_invalidate_draw_state(device);
    if (device->d3d_surface_copy_count != 0xffffffffUL)
        ++device->d3d_surface_copy_count;
    if (device->d3d_surface_copy_count == 1)
        qxp_debug("QXPMP D3D SURFACE COPY+");
    return TRUE;
}

/* Downloads one host-authoritative texture image into guest video memory so
 * CPU access observes what the GPU produced. The mip level named by the
 * caller's extent is downloaded through the FBT bounce region in canonical
 * D3D row order (row zero is the top row, exactly the layout the upload path
 * consumes), and written into the framebuffer aperture with the caller's
 * pitch. Runs at PASSIVE_LEVEL from the display driver's
 * EngDeviceIoControl. */
static BOOLEAN qxp_d3d_surface_readback_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SURFACE_READBACK_REQUEST *request)
{
    static const ULONG pack_defaults[5][2] = {
        { QXP_GL_PACK_SWAP_BYTES, 0 },
        { QXP_GL_PACK_ROW_LENGTH, 0 },
        { QXP_GL_PACK_SKIP_ROWS, 0 },
        { QXP_GL_PACK_SKIP_PIXELS, 0 },
        { QXP_GL_PACK_ALIGNMENT, 1 }
    };
    QXP_D3D_TEXTURE_STATE *texture;
    PHYSICAL_ADDRESS physical;
    ULONG bind_texture_arguments[2];
    ULONG pack_buffer_arguments[2];
    ULONG pack_arguments[2];
    ULONG read_arguments[5];
    ULONG pack_index;
    ULONG internal_format = 0;
    ULONG pixel_format = 0;
    ULONG pixel_type = 0;
    ULONG bytes_per_pixel = 0;
    ULONG row_bytes;
    ULONG total_bytes;
    ULONG span_bytes;
    ULONG page_offset;
    ULONG map_length;
    ULONG address_space =
        VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_P6CACHE;
    ULONG fifo_start;
    ULONG level;
    ULONG row;
    PVOID mapped = NULL;
    PUCHAR destination;
    const UCHAR *source;
    volatile ULONG *fifo;

    if (!device || !device->d3d_active ||
        !qxp_packet_surface_readback_contract(request) ||
        request->process_id != device->d3d_process_id) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(1);
        if (request) {
            qxp_debug_status(request->format);
            qxp_debug_status(request->width);
            qxp_debug_status(request->height);
            qxp_debug_status(request->pitch);
            qxp_debug_status(request->version);
        }
        return FALSE;
    }
#ifdef QXP_VERBOSE_TRACE
    qxp_debug("QXPMP D3D READBACK TRACE");
    qxp_debug_status(request->texture_name);
    qxp_debug_status(request->width);
    qxp_debug_status(request->height);
    qxp_debug_status(request->format);
    qxp_debug_status(device->d3d_target_texture);
    qxp_debug_status(device->d3d_target_present);
#endif
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    if (!texture || texture->texture_type != QXP_D3D_TEXTURE_2D ||
        texture->format != request->format) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(2);
        qxp_debug_status(request->texture_name);
        qxp_debug_status(request->format);
        qxp_debug_status(texture ? texture->format : 0);
        qxp_debug_status(texture ? texture->texture_type : 0);
        return FALSE;
    }
    /* Every level of a mip chain is its own DirectDraw surface, and the
     * display driver reads one back with that level's extent while the whole
     * chain shares a single GL texture whose registered extent is level 0.
     * A D3D mip chain halves at least one dimension per level until both
     * reach 1, so the requested pair names exactly one level. */
    for (level = 0; level < texture->levels; ++level) {
        if (qxp_packet_mip_dimension(texture->width, level) ==
                request->width &&
            qxp_packet_mip_dimension(texture->height, level) ==
                request->height) break;
    }
    if (level >= texture->levels ||
        !(texture->defined_levels[0] & (1UL << level))) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(3);
        qxp_debug_status(request->width);
        qxp_debug_status(request->height);
        qxp_debug_status(texture->width);
        qxp_debug_status(texture->height);
        qxp_debug_status(texture->levels);
        qxp_debug_status(texture->defined_levels[0]);
        return FALSE;
    }
    if (!qxp_d3d_texture_format(request->format, &internal_format,
                                &pixel_format, &pixel_type,
                                &bytes_per_pixel)) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(4);
        qxp_debug_status(request->format);
        return FALSE;
    }
    row_bytes = request->width * bytes_per_pixel;
    total_bytes = request->height * row_bytes;
    /* The contract bounds width/height at 2048 and pitch at 1 MB, so these
     * products cannot overflow; span still needs the aperture bound. */
    span_bytes = (request->height - 1UL) * request->pitch + row_bytes;
    if (total_bytes > QXP_MESA_FBTM_SIZE - QXP_D3D_GET_TEX_IMAGE_OFFSET ||
        request->vidmem_offset >= device->framebuffer.length ||
        span_bytes > device->framebuffer.length - request->vidmem_offset) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(5);
        qxp_debug_status(total_bytes);
        qxp_debug_status(request->vidmem_offset);
        qxp_debug_status(span_bytes);
        qxp_debug_status(request->pitch);
        return FALSE;
    }
    if (!device->mesa_fbtm.mapped) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(6);
        return FALSE;
    }
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(7);
        return FALSE;
    }
    qxp_d3d_finish_draw_batch(device);

#ifdef QXP_VERBOSE_TRACE
    if (request->width > 8 && request->height > 56) {
        read_arguments[0] = 8;
        read_arguments[1] = 56;
        read_arguments[2] = 1;
        read_arguments[3] = 1;
        read_arguments[4] = pixel_format;
        read_arguments[5] = pixel_type;
        read_arguments[6] = 0;
        VideoPortZeroMemory(device->mesa_fbtm.mapped, sizeof(ULONG));
        if (qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_READ_PIXELS,
                read_arguments, 7, NULL, 0)) {
            qxp_debug("QXPMP D3D READBACK PREBIND PIXEL");
            qxp_debug_status(
                *(volatile ULONG *)device->mesa_fbtm.mapped);
        } else {
            qxp_debug("QXPMP D3D READBACK PREBIND PIXEL-");
        }
    }
#endif

    /* Download the level with the texture-download call rather than an FBO
     * colour read.  Attaching a non-base mip level to the copy framebuffer
     * reports GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT on this host even when
     * every level is defined, and an FBO read can only ever address a
     * colour-renderable level; glGetTexImage names the level directly. */
    fifo_start = fifo[0];
    bind_texture_arguments[0] = QXP_GL_TEXTURE_2D;
    bind_texture_arguments[1] = request->texture_name;
    pack_buffer_arguments[0] = QXP_GL_PIXEL_PACK_BUFFER;
    pack_buffer_arguments[1] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_texture_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_BUFFER,
                             pack_buffer_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    /* Every pixel-pack parameter that can change the destination layout is
     * forced to its canonical value; the host also redirects the output to
     * the FBT bounce only when no pixel-pack buffer is bound. */
    for (pack_index = 0; pack_index < 5; ++pack_index) {
        pack_arguments[0] = pack_defaults[pack_index][0];
        pack_arguments[1] = pack_defaults[pack_index][1];
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PIXEL_STORE_I,
                                 pack_arguments, 2)) {
            fifo[0] = fifo_start;
            return FALSE;
        }
    }
#ifdef QXP_VERBOSE_TRACE
    if (request->width > 8 && request->height > 56) {
        read_arguments[0] = 8;
        read_arguments[1] = 56;
        read_arguments[2] = 1;
        read_arguments[3] = 1;
        read_arguments[4] = pixel_format;
        read_arguments[5] = pixel_type;
        read_arguments[6] = 0;
        VideoPortZeroMemory(device->mesa_fbtm.mapped, sizeof(ULONG));
        if (qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_READ_PIXELS,
                read_arguments, 7, NULL, 0)) {
            qxp_debug("QXPMP D3D READBACK COPY PIXEL");
            qxp_debug_status(
                *(volatile ULONG *)device->mesa_fbtm.mapped);
        } else {
            qxp_debug("QXPMP D3D READBACK COPY PIXEL-");
        }
    }
#endif
    /* The host executes glGetTexImage synchronously during the 0xfc0 doorbell
     * write.  It writes the byte count it computed from the live GL level at
     * the FBT base and packs the image immediately after it, eight-byte
     * aligned (alignment one, no row padding: width * bytes_per_pixel per
     * row, row zero of the level first, which is the row order the upload
     * path wrote). */
    read_arguments[0] = QXP_GL_TEXTURE_2D;
    read_arguments[1] = level;
    read_arguments[2] = pixel_format;
    read_arguments[3] = pixel_type;
    read_arguments[4] = 1;
    if (!qxp_d3d_call_fbtm(device, QXP_FENUM_GL_GET_TEX_IMAGE,
                           read_arguments, 5, NULL, 0)) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(8);
        return FALSE;
    }
    /* The host refuses to pack a level whose size it cannot honour, leaving
     * the count it computed behind; a short level would otherwise be copied
     * out of a partly written bounce. */
    if (*(volatile ULONG *)device->mesa_fbtm.mapped < total_bytes) {
        qxp_debug("QXPMP D3D READBACK REJECT");
        qxp_debug_status(9);
        qxp_debug_status(*(volatile ULONG *)device->mesa_fbtm.mapped);
        qxp_debug_status(total_bytes);
        qxp_debug_status(request->width);
        qxp_debug_status(request->height);
        qxp_debug_status(level);
        return FALSE;
    }

    page_offset = request->vidmem_offset & 0xfffUL;
    map_length = page_offset + span_bytes;
    physical.QuadPart = device->framebuffer.physical.QuadPart +
                        (request->vidmem_offset - page_offset);
    if (VideoPortMapMemory(device, physical, &map_length, &address_space,
                           &mapped) != NO_ERROR || !mapped) {
        qxp_debug("QXPMP D3D READBACK MAP-");
        return FALSE;
    }
    source = device->mesa_fbtm.mapped + QXP_D3D_GET_TEX_IMAGE_OFFSET;
    destination = (PUCHAR)mapped + page_offset;
    for (row = 0; row < request->height; ++row) {
        VideoPortMoveMemory(destination, (PVOID)source, row_bytes);
        source += row_bytes;
        destination += request->pitch;
    }
#ifdef QXP_VERBOSE_TRACE
    if (request->width > 8 && request->height > 56 &&
        bytes_per_pixel == sizeof(ULONG)) {
        qxp_debug("QXPMP D3D READBACK GUEST PIXEL");
        qxp_debug_status(*(volatile ULONG *)(
            (PUCHAR)mapped + page_offset + 56UL * request->pitch +
            8UL * bytes_per_pixel));
    }
#endif
    VideoPortUnmapMemory(device, mapped, NULL);
    if (device->d3d_surface_readback_count != 0xffffffffUL)
        ++device->d3d_surface_readback_count;
    if (device->d3d_surface_readback_count == 1)
        qxp_debug("QXPMP D3D SURFACE READBACK+");
    return TRUE;
}

static BOOLEAN qxp_d3d_texture_delete_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_TEXTURE_DELETE_REQUEST *request)
{
    QXP_D3D_TEXTURE_STATE *texture;
    QXP_D3D_DEPTH_STATE *depth;
    ULONG arguments[2];
    ULONG index;
    BOOLEAN result = TRUE;

    if (!device->d3d_active || !request ||
        request->process_id != device->d3d_process_id ||
        !request->texture_name) return FALSE;
    /* Deletes now arrive mid-frame from DdDestroySurface, so any batched draw
     * still naming this texture must reach the host before it disappears. */
    qxp_d3d_finish_draw_batch(device);
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    depth = qxp_d3d_find_depth(device, request->process_id,
                               request->texture_name);
    if ((device->d3d_target_texture == request->texture_name ||
         device->d3d_target_depth == request->texture_name) &&
        !qxp_d3d_bind_default_target(device)) return FALSE;
    arguments[0] = 1;
    if (texture) {
        arguments[1] = request->texture_name;
        result = qxp_d3d_call_input(device, QXP_FENUM_GL_DELETE_TEXTURES,
                                    arguments, 2, &request->texture_name,
                                    sizeof(request->texture_name));
        if (result) {
            VideoPortZeroMemory(texture, sizeof(*texture));
            /* Keep probe runs that collided past this slot intact. */
            texture->name = QXP_D3D_TEXTURE_TOMBSTONE;
            ++device->d3d_texture_delete_count;
        }
    }
    if (result && depth) {
        arguments[1] = depth->renderbuffer_name;
        result = qxp_d3d_call_input(
            device, QXP_FENUM_GL_DELETE_RENDERBUFFERS, arguments, 2,
            &depth->renderbuffer_name, sizeof(depth->renderbuffer_name));
        if (result) {
            /* A guest surface name can later be reused for a different depth
             * renderbuffer. Invalidate every attachment pairing that trusted
             * this generation before releasing the depth record. */
            for (index = 0; index < QXP_D3D_TEXTURE_CAPACITY; ++index) {
                if (device->d3d_textures[index].framebuffer_complete &&
                    device->d3d_textures[index].framebuffer_depth_name ==
                        request->texture_name) {
                    device->d3d_textures[index].framebuffer_complete = FALSE;
                }
            }
            VideoPortZeroMemory(depth, sizeof(*depth));
        }
    }
    return result;
}

static BOOLEAN qxp_d3d_sm2_program_define_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *request,
    ULONG request_bytes)
{
    QXP_D3D_SM2_PROGRAM_STATE *slot;
    ULONG arguments[3];
    ULONG result = 0;
    ULONG vertex_shader = 0;
    ULONG pixel_shader = 0;
    ULONG gl_program = 0;
    ULONG attribute;
    ULONG sampler;
    LONG status = 0;
    LONG vertex_constants_location = -1;
    LONG vertex_clip_control_location = -1;
    LONG vertex_positiont_location = -1;
    LONG vertex_integer_constants_location = -1;
    LONG vertex_boolean_constants_location = -1;
    LONG pixel_constants_location = -1;
    LONG pixel_fog_location = -1;
    LONG pixel_texture_factor_location = -1;
    LONG sampler_locations[QXP_D3D_SM2_SAMPLER_COUNT];
    char attribute_name[7];
    char sm2_attribute_name[13];
    char sampler_name[9];
    ULONG attribute_name_bytes;
    ULONG sm2_attribute_name_bytes;
    ULONG sampler_name_bytes;
    BOOLEAN vertex_attached = FALSE;
    BOOLEAN pixel_attached = FALSE;

    if (!device || !device->d3d_active ||
        !qxp_packet_sm2_program_define_contract(
            request, request_bytes) ||
        request->process_id != device->d3d_process_id ||
        qxp_d3d_find_sm2_program(
            device, request->process_id, request->program_name))
        return FALSE;
    slot = qxp_d3d_free_sm2_program();
    if (!slot) return FALSE;

    arguments[0] = QXP_GL_VERTEX_SHADER;
    result = 0;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    vertex_shader = result;
    if (!qxp_d3d_call_shader_source(
            device, vertex_shader, request->source,
            request->vertex_source_bytes)) goto fail;
    arguments[0] = vertex_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, vertex_shader,
            QXP_GL_COMPILE_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D SM2 VERTEX GLSL COMPILE-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, vertex_shader);
        goto fail;
    }

    arguments[0] = QXP_GL_FRAGMENT_SHADER;
    result = 0;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    pixel_shader = result;
    if (!qxp_d3d_call_shader_source(
            device, pixel_shader,
            request->source + request->vertex_source_bytes,
            request->pixel_source_bytes)) goto fail;
    arguments[0] = pixel_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, pixel_shader,
            QXP_GL_COMPILE_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D SM2 PIXEL GLSL COMPILE-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, pixel_shader);
        goto fail;
    }

    result = 0;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_PROGRAM,
            NULL, 0, NULL, 0, &result) || !result) goto fail;
    gl_program = result;
    arguments[0] = gl_program;
    arguments[1] = vertex_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    vertex_attached = TRUE;
    arguments[1] = pixel_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    pixel_attached = TRUE;

    for (attribute = 0; attribute < 16; ++attribute) {
        attribute_name[0] = 'q';
        attribute_name[1] = 'x';
        attribute_name[2] = 'p';
        attribute_name[3] = 'V';
        if (attribute < 10) {
            attribute_name[4] = (char)('0' + attribute);
            attribute_name[5] = 0;
            attribute_name_bytes = 6;
        } else {
            attribute_name[4] = '1';
            attribute_name[5] = (char)('0' + attribute - 10);
            attribute_name[6] = 0;
            attribute_name_bytes = 7;
        }
        arguments[0] = gl_program;
        arguments[1] = attribute;
        arguments[2] = 0;
        if (!qxp_d3d_call_input(
                device, QXP_FENUM_GL_BIND_ATTRIB_LOCATION,
                arguments, 3, attribute_name,
                attribute_name_bytes)) goto fail;
        /* SM2 input declarations use qxpVsInputN. Keep the required qxpVN
         * clip-path binding and alias the active SM2 name to the same index. */
        sm2_attribute_name[0] = 'q';
        sm2_attribute_name[1] = 'x';
        sm2_attribute_name[2] = 'p';
        sm2_attribute_name[3] = 'V';
        sm2_attribute_name[4] = 's';
        sm2_attribute_name[5] = 'I';
        sm2_attribute_name[6] = 'n';
        sm2_attribute_name[7] = 'p';
        sm2_attribute_name[8] = 'u';
        sm2_attribute_name[9] = 't';
        if (attribute < 10) {
            sm2_attribute_name[10] = (char)('0' + attribute);
            sm2_attribute_name[11] = 0;
            sm2_attribute_name_bytes = 12;
        } else {
            sm2_attribute_name[10] = '1';
            sm2_attribute_name[11] =
                (char)('0' + attribute - 10);
            sm2_attribute_name[12] = 0;
            sm2_attribute_name_bytes = 13;
        }
        if (!qxp_d3d_call_input(
                device, QXP_FENUM_GL_BIND_ATTRIB_LOCATION,
                arguments, 3, sm2_attribute_name,
                sm2_attribute_name_bytes)) goto fail;
    }

    arguments[0] = gl_program;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_LINK_PROGRAM,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_PROGRAM_IV, gl_program,
            QXP_GL_LINK_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D SM2 GLSL LINK-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_PROGRAM_INFO_LOG, gl_program);
        goto fail;
    }

    if (!qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpVsFloatConst[0]",
            sizeof("qxpVsFloatConst[0]"),
            &vertex_constants_location) ||
        !qxp_d3d_gl_uniform_location(
            device, gl_program, "qxpVsClipControl",
            sizeof("qxpVsClipControl"),
            &vertex_clip_control_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpVsPositiont",
            sizeof("qxpVsPositiont"),
            &vertex_positiont_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpVsIntConst[0]",
            sizeof("qxpVsIntConst[0]"),
            &vertex_integer_constants_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpVsBoolConst[0]",
            sizeof("qxpVsBoolConst[0]"),
            &vertex_boolean_constants_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpPsConstants[0]",
            sizeof("qxpPsConstants[0]"),
            &pixel_constants_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpPsFog", sizeof("qxpPsFog"),
            &pixel_fog_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpTFactor", sizeof("qxpTFactor"),
            &pixel_texture_factor_location)) {
        qxp_debug("QXPMP D3D SM2 GLSL UNIFORM-");
        goto fail;
    }
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT;
         ++sampler) {
        sampler_name[0] = 'q';
        sampler_name[1] = 'x';
        sampler_name[2] = 'p';
        sampler_name[3] = 'P';
        sampler_name[4] = 's';
        sampler_name[5] = 'S';
        if (sampler < 10) {
            sampler_name[6] = (char)('0' + sampler);
            sampler_name[7] = 0;
            sampler_name_bytes = 8;
        } else {
            sampler_name[6] = '1';
            sampler_name[7] = (char)('0' + sampler - 10);
            sampler_name[8] = 0;
            sampler_name_bytes = 9;
        }
        if (!qxp_d3d_gl_uniform_location_optional(
                device, gl_program, sampler_name,
                sampler_name_bytes, &sampler_locations[sampler])) {
            qxp_debug("QXPMP D3D SM2 GLSL SAMPLER-");
            goto fail;
        }
        if (((request->sampler_mask & (1UL << sampler)) != 0) !=
            (sampler_locations[sampler] >= 0)) {
            qxp_debug("QXPMP D3D SM2 SAMPLER CONTRACT-");
            goto fail;
        }
    }

    VideoPortZeroMemory(slot, sizeof(*slot));
    slot->device = device;
    slot->process_id = request->process_id;
    slot->program_name = request->program_name;
    slot->vertex_shader = vertex_shader;
    slot->pixel_shader = pixel_shader;
    slot->gl_program = gl_program;
    slot->vertex_constants_location = vertex_constants_location;
    slot->vertex_clip_control_location = vertex_clip_control_location;
    slot->vertex_positiont_location = vertex_positiont_location;
    slot->vertex_integer_constants_location =
        vertex_integer_constants_location;
    slot->vertex_boolean_constants_location =
        vertex_boolean_constants_location;
    slot->pixel_constants_location = pixel_constants_location;
    slot->pixel_fog_location = pixel_fog_location;
    slot->pixel_texture_factor_location = pixel_texture_factor_location;
    slot->sampler_mask = request->sampler_mask;
    slot->cube_sampler_mask = request->cube_sampler_mask;
    slot->volume_sampler_mask = request->volume_sampler_mask;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT;
         ++sampler) {
        slot->sampler_locations[sampler] = sampler_locations[sampler];
    }
    if (request->sampler_mask & ~15UL) {
        qxp_debug("QXPMP D3D SM2 HIGH SAMPLER DEFINE");
        qxp_debug_status(request->sampler_mask);
        qxp_debug_status(request->cube_sampler_mask);
        qxp_debug_status(request->volume_sampler_mask);
    }
    qxp_debug("QXPMP D3D SM2 GLSL DEFINE+");
    return TRUE;

fail:
    qxp_d3d_destroy_sm2_objects(
        device, gl_program, vertex_shader, vertex_attached,
        pixel_shader, pixel_attached);
    qxp_debug("QXPMP D3D SM2 GLSL DEFINE-");
    return FALSE;
}

static BOOLEAN qxp_d3d_sm2_program_delete_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SM2_PROGRAM_DELETE_REQUEST *request,
    ULONG request_bytes)
{
    QXP_D3D_SM2_PROGRAM_STATE *program;

    if (!device || !device->d3d_active ||
        !qxp_packet_sm2_program_delete_contract(
            request, request_bytes) ||
        request->process_id != device->d3d_process_id) return FALSE;
    program = qxp_d3d_find_sm2_program(
        device, request->process_id, request->program_name);
    if (!program) return FALSE;
    if (!qxp_d3d_release_sm2_program(device, program)) return FALSE;
    qxp_debug("QXPMP D3D SM2 GLSL DELETE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_sm2_constants_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SM2_CONSTANTS_REQUEST *request,
    ULONG request_bytes)
{
    QXP_D3D_SM2_PROGRAM_STATE *program;
    const ULONG *pixel_data;
    ULONG use_arguments[1];
    ULONG uniform_arguments[3];
    ULONG previous_program;
    volatile ULONG *fifo;
    volatile ULONG *data;
    ULONG fifo_start;
    ULONG data_start;

    if (!device || !request || !device->d3d_active) return FALSE;
    program = qxp_d3d_find_sm2_program(
        device, request->process_id, request->program_name);
    if (!qxp_packet_sm2_constants_contract(
            request, request_bytes, device->d3d_process_id,
            program ? program->program_name : 0)) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo || !qxp_d3d_reserve_draw_fifo(device)) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    previous_program = qxp_d3d_current_glsl_program(device);
    use_arguments[0] = program->gl_program;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_USE_PROGRAM,
            use_arguments, 1)) goto fail;
    if (request->vertex_register_count &&
        program->vertex_constants_location >= 0) {
        uniform_arguments[0] =
            (ULONG)(program->vertex_constants_location +
                    (LONG)request->vertex_start_register);
        uniform_arguments[1] = request->vertex_register_count;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                uniform_arguments, 3, request->data,
                request->vertex_register_count * 4UL *
                    sizeof(ULONG))) goto fail;
    }
    pixel_data = request->data +
                 request->vertex_register_count * 4UL;
    if (request->pixel_register_count &&
        program->pixel_constants_location >= 0) {
        uniform_arguments[0] =
            (ULONG)(program->pixel_constants_location +
                    (LONG)request->pixel_start_register);
        uniform_arguments[1] = request->pixel_register_count;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                uniform_arguments, 3, pixel_data,
                request->pixel_register_count * 4UL *
                    sizeof(ULONG))) goto fail;
    }
    use_arguments[0] = previous_program;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_USE_PROGRAM,
            use_arguments, 1)) goto fail;
    return TRUE;

fail:
    fifo[0] = fifo_start;
    data[0] = data_start;
    return FALSE;
}

static BOOLEAN qxp_d3d_vertex_clip_define_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SHADER_DEFINE_REQUEST *request)
{
    QXP_D3D_VERTEX_CLIP_STATE *slot;
    ULONG arguments[3];
    ULONG result = 0;
    ULONG shader = 0;
    ULONG gl_program = 0;
    ULONG attribute;
    LONG status = 0;
    LONG constants_location = -1;
    LONG control_location = -1;
    LONG projected_location = -1;
    char attribute_name[7];
    ULONG attribute_name_bytes;

    if (!device || !request ||
        request->shader_type != QXP_D3D_SHADER_VERTEX_CLIP ||
        (request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE ||
        request->source_bytes < 2 ||
        request->source_bytes > QXP_D3D_MAX_VERTEX_CLIP_SOURCE_BYTES ||
        request->source[request->source_bytes - 1] != 0 ||
        qxp_d3d_find_vertex_clip(device, request->program_name))
        return FALSE;
    slot = qxp_d3d_free_vertex_clip(device);
    if (!slot) return FALSE;

    arguments[0] = QXP_GL_VERTEX_SHADER;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    shader = result;
    if (!qxp_d3d_call_shader_source(
            device, shader, request->source,
            request->source_bytes)) goto fail;
    arguments[0] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, shader,
            QXP_GL_COMPILE_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D CLIP GLSL COMPILE-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, shader);
        goto fail;
    }

    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_PROGRAM,
            NULL, 0, NULL, 0, &result) || !result) goto fail;
    gl_program = result;
    arguments[0] = gl_program;
    arguments[1] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    for (attribute = 0; attribute < 16; ++attribute) {
        attribute_name[0] = 'q';
        attribute_name[1] = 'x';
        attribute_name[2] = 'p';
        attribute_name[3] = 'V';
        if (attribute < 10) {
            attribute_name[4] = (char)('0' + attribute);
            attribute_name[5] = 0;
            attribute_name_bytes = 6;
        } else {
            attribute_name[4] = '1';
            attribute_name[5] = (char)('0' + attribute - 10);
            attribute_name[6] = 0;
            attribute_name_bytes = 7;
        }
        arguments[0] = gl_program;
        arguments[1] = attribute;
        arguments[2] = 0;
        if (!qxp_d3d_call_input(
                device, QXP_FENUM_GL_BIND_ATTRIB_LOCATION,
                arguments, 3, attribute_name,
                attribute_name_bytes)) goto fail;
    }
    arguments[0] = gl_program;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_LINK_PROGRAM,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_PROGRAM_IV, gl_program,
            QXP_GL_LINK_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D CLIP GLSL LINK-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_PROGRAM_INFO_LOG, gl_program);
        goto fail;
    }
    if (!qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpConstants[0]",
            sizeof("qxpConstants[0]"), &constants_location) ||
        !qxp_d3d_gl_uniform_location(
            device, gl_program, "qxpControl", sizeof("qxpControl"),
            &control_location) ||
        !qxp_d3d_gl_uniform_location_optional(
            device, gl_program, "qxpProjected", sizeof("qxpProjected"),
            &projected_location)) goto fail;

    arguments[0] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_DELETE_SHADER,
            arguments, 1, NULL, 0)) goto fail;
    shader = 0;
    slot->program_name = request->program_name;
    slot->gl_program = gl_program;
    slot->constants_location = constants_location;
    slot->control_location = control_location;
    slot->projected_location = projected_location;
    qxp_debug("QXPMP D3D CLIP GLSL DEFINE+");
    return TRUE;

fail:
    if (gl_program) {
        arguments[0] = gl_program;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                          arguments, 1, NULL, 0);
    }
    if (shader) {
        arguments[0] = shader;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_SHADER,
                          arguments, 1, NULL, 0);
    }
    qxp_debug("QXPMP D3D CLIP GLSL DEFINE-");
    return FALSE;
}

static BOOLEAN qxp_d3d_vertex_clip_delete_transport(
    QXP_DEVICE_EXTENSION *device, ULONG program_name)
{
    QXP_D3D_VERTEX_CLIP_STATE *clip = NULL;
    ULONG arguments[1];
    ULONG point_size_arguments[1];

    if (!device || !program_name) return FALSE;
    clip = qxp_d3d_find_vertex_clip(device, program_name);
    if (!clip) return TRUE;
    if (device->d3d_vertex_program_clip &&
        device->d3d_vertex_program == program_name) {
        if (device->d3d_vertex_program_point_size) {
            point_size_arguments[0] =
                QXP_GL_VERTEX_PROGRAM_POINT_SIZE_ARB;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE,
                    point_size_arguments, 1)) return FALSE;
        }
        arguments[0] = 0;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_USE_PROGRAM,
                arguments, 1)) return FALSE;
        device->d3d_vertex_program = 0;
        device->d3d_vertex_program_point_size = FALSE;
        device->d3d_vertex_program_clip = FALSE;
        qxp_d3d_invalidate_draw_state(device);
    }
    arguments[0] = clip->gl_program;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_DELETE_PROGRAM,
            arguments, 1, NULL, 0)) return FALSE;
    VideoPortZeroMemory(clip, sizeof(*clip));
    qxp_debug("QXPMP D3D CLIP GLSL DELETE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_shader_define_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SHADER_DEFINE_REQUEST *request, ULONG request_bytes)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    ULONG target;
    ULONG maximum_source_bytes;
    ULONG minimum;
    volatile ULONG *fifo;
    ULONG fifo_start;

    minimum = sizeof(*request) - sizeof(request->source);
    if (!device->d3d_active || !request ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        request->process_id != device->d3d_process_id ||
        !request->program_name || !request->source_bytes ||
        request_bytes < minimum ||
        request->source_bytes > request_bytes - minimum) return FALSE;
    if (request->shader_type == QXP_D3D_SHADER_VERTEX_CLIP)
        return qxp_d3d_vertex_clip_define_transport(device, request);
    if (request->shader_type == QXP_D3D_SHADER_PIXEL) {
        if ((request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_PIXEL_PROGRAM_NAMESPACE) return FALSE;
        target = QXP_GL_FRAGMENT_PROGRAM_ARB;
        maximum_source_bytes = QXP_D3D_MAX_PIXEL_SHADER_SOURCE_BYTES;
        restore_arguments[1] = device->d3d_pixel_program;
    } else if (request->shader_type == QXP_D3D_SHADER_VERTEX) {
        if ((request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE) return FALSE;
        target = QXP_GL_VERTEX_PROGRAM_ARB;
        maximum_source_bytes = QXP_D3D_MAX_VERTEX_SHADER_SOURCE_BYTES;
        restore_arguments[1] = device->d3d_vertex_program ==
                QXP_D3D_SCALABLE_POINT_PROGRAM ?
            0 : device->d3d_vertex_program;
    } else {
        return FALSE;
    }
    if (request->source_bytes > maximum_source_bytes) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = target;
    bind_arguments[1] = request->program_name;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                              bind_arguments, 2) ||
        !qxp_d3d_call_program_string(device, target, request->source,
                                      request->source_bytes)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = target;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                              restore_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    qxp_debug("QXPMP D3D SHADER DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_shader_delete_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_SHADER_DELETE_REQUEST *request)
{
    ULONG arguments[2];
    ULONG state_arguments[2];
    ULONG target;
    PULONG bound_program;

    if (!device->d3d_active || !request ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        request->process_id != device->d3d_process_id ||
        !request->program_name) return FALSE;
    if (request->shader_type == QXP_D3D_SHADER_VERTEX_CLIP) {
        if ((request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE) return FALSE;
        return qxp_d3d_vertex_clip_delete_transport(
            device, request->program_name);
    }
    if (request->shader_type == QXP_D3D_SHADER_PIXEL) {
        if ((request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_PIXEL_PROGRAM_NAMESPACE) return FALSE;
        target = QXP_GL_FRAGMENT_PROGRAM_ARB;
        bound_program = &device->d3d_pixel_program;
    } else if (request->shader_type == QXP_D3D_SHADER_VERTEX) {
        if ((request->program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE) return FALSE;
        target = QXP_GL_VERTEX_PROGRAM_ARB;
        bound_program = &device->d3d_vertex_program;
    } else {
        return FALSE;
    }
    if (*bound_program == request->program_name &&
        !(request->shader_type == QXP_D3D_SHADER_VERTEX &&
          device->d3d_vertex_program_clip)) {
        state_arguments[0] = target;
        arguments[0] = target;
        arguments[1] = 0;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                  state_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                                  arguments, 2)) return FALSE;
        *bound_program = 0;
    }
    arguments[0] = 1;
    arguments[1] = request->program_name;
    if (!qxp_d3d_call_input(device, QXP_FENUM_GL_DELETE_PROGRAMS_ARB,
                            arguments, 2, &request->program_name,
                            sizeof(request->program_name))) return FALSE;
    qxp_debug("QXPMP D3D SHADER DELETE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_apply_pixel_program(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request)
{
    ULONG bind_arguments[2];
    ULONG constant_arguments[3];
    ULONG luminance_values[4];
    ULONG state_arguments[1];
    ULONG constant;
    BOOLEAN reuse_constants;

    if (!device || !request) return FALSE;
    state_arguments[0] = QXP_GL_FRAGMENT_PROGRAM_ARB;
    if (!request->pixel_program_name) {
        if (!device->d3d_pixel_program) return TRUE;
        bind_arguments[0] = QXP_GL_FRAGMENT_PROGRAM_ARB;
        bind_arguments[1] = 0;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                                 bind_arguments, 2)) return FALSE;
        device->d3d_pixel_program = 0;
        return TRUE;
    }
    reuse_constants = device->d3d_draw_state_valid &&
        device->d3d_pixel_program == request->pixel_program_name &&
        device->d3d_last_draw.pixel_program_name ==
            request->pixel_program_name;
    if (device->d3d_pixel_program != request->pixel_program_name) {
        reuse_constants = FALSE;
        bind_arguments[0] = QXP_GL_FRAGMENT_PROGRAM_ARB;
        bind_arguments[1] = request->pixel_program_name;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                                 bind_arguments, 2) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                 state_arguments, 1)) return FALSE;
        device->d3d_pixel_program = request->pixel_program_name;
    }
    for (constant = 0; constant < QXP_D3D_PIXEL_CONSTANT_COUNT;
         ++constant) {
        if (reuse_constants && qxp_d3d_bytes_equal(
                device->d3d_last_draw.pixel_constants[constant],
                request->pixel_constants[constant],
                sizeof(request->pixel_constants[constant]))) continue;
        constant_arguments[0] = QXP_GL_FRAGMENT_PROGRAM_ARB;
        constant_arguments[1] = constant;
        constant_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
                constant_arguments, 3, request->pixel_constants[constant],
                sizeof(request->pixel_constants[constant]))) return FALSE;
        VideoPortMoveMemory(device->d3d_pixel_constants[constant],
                            (PVOID)request->pixel_constants[constant],
                            sizeof(request->pixel_constants[constant]));
    }
    constant_arguments[0] = QXP_GL_FRAGMENT_PROGRAM_ARB;
    constant_arguments[1] = QXP_D3D_PIXEL_CONSTANT_COUNT;
    constant_arguments[2] = 0;
    if ((!reuse_constants || !qxp_d3d_bytes_equal(
             &device->d3d_last_draw.texture1_bumpenv_mat00_bits,
             &request->texture1_bumpenv_mat00_bits,
             4 * sizeof(request->texture1_bumpenv_mat00_bits))) &&
        !qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            constant_arguments, 3, &request->texture1_bumpenv_mat00_bits,
            4 * sizeof(request->texture1_bumpenv_mat00_bits))) return FALSE;
    luminance_values[0] = request->texture1_bumpenv_lscale_bits;
    luminance_values[1] = request->texture1_bumpenv_loffset_bits;
    luminance_values[2] = 0;
    luminance_values[3] = 0;
    constant_arguments[1] = QXP_D3D_PIXEL_CONSTANT_COUNT + 1;
    if ((!reuse_constants ||
         device->d3d_last_draw.texture1_bumpenv_lscale_bits !=
             request->texture1_bumpenv_lscale_bits ||
         device->d3d_last_draw.texture1_bumpenv_loffset_bits !=
             request->texture1_bumpenv_loffset_bits) &&
        !qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            constant_arguments, 3, luminance_values,
            sizeof(luminance_values))) return FALSE;
    return TRUE;
}

/* Fixed OpenGL always interprets the fourth texture coordinate as homogeneous
 * q. Direct3D does that only for a stage marked PROJECTED. This bounded vertex
 * program preserves the existing fixed transform and colors, applies all four
 * fixed texture matrices, and selects transformed q or literal one per stage.
 * Position-invariant programs retain fixed user clipping. This variant also
 * emits D3D's absolute-Z or radial vertex-fog coordinate, selecting a supplied
 * table-fog coordinate when that array is active, and derives the complete D3D
 * scaled-point size because ARB vertex programs bypass fixed attenuation. */
static const UCHAR qxp_d3d_texture_q_program[] =
    "!!ARBvp1.0\n"
    "OPTION ARB_position_invariant;\n"
    "PARAM projected = program.local[0];\n"
    "PARAM literal = program.local[1];\n"
    "PARAM rangeFog = program.local[2];\n"
    "PARAM tableFog = program.local[3];\n"
    "PARAM epsilon = { 1.0e-20, 0.0, 0.0, 0.0 };\n"
    "PARAM pointSize = state.point.size;\n"
    "PARAM pointAttenuation = state.point.attenuation;\n"
    "PARAM mv0 = state.matrix.modelview.row[0];\n"
    "PARAM mv1 = state.matrix.modelview.row[1];\n"
    "PARAM mv2 = state.matrix.modelview.row[2];\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP value, eye, fog, denominator;\n"
    "MOV result.color, vertex.color;\n"
    "MOV result.color.secondary, vertex.color.secondary;\n"
    "DP4 eye.x, mv0, vertex.position;\n"
    "DP4 eye.y, mv1, vertex.position;\n"
    "DP4 eye.z, mv2, vertex.position;\n"
    "ABS fog.x, eye.z;\n"
    "DP3 fog.y, eye, eye;\n"
    "MAX fog.z, fog.y, epsilon.x;\n"
    "RSQ fog.z, fog.z;\n"
    "MUL fog.w, fog.y, fog.z;\n"
    "MAD denominator.x, pointAttenuation.z, fog.y, pointAttenuation.x;\n"
    "MAD denominator.x, pointAttenuation.y, fog.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RSQ denominator.x, denominator.x;\n"
    "MUL denominator.y, pointSize.x, denominator.x;\n"
    "MAX denominator.y, denominator.y, pointSize.y;\n"
    "MIN result.pointsize.x, denominator.y, pointSize.z;\n"
    "SUB fog.y, fog.w, fog.x;\n"
    "MAD fog.x, fog.y, rangeFog.x, fog.x;\n"
    "MAD result.fogcoord.x, fog.x, tableFog.x, vertex.fogcoord.x;\n"
    "DP4 value.x, t00, vertex.texcoord[0];\n"
    "DP4 value.y, t01, vertex.texcoord[0];\n"
    "DP4 value.z, t02, vertex.texcoord[0];\n"
    "DP4 value.w, t03, vertex.texcoord[0];\n"
    "MOV result.texcoord[0].xyz, value;\n"
    "MAD result.texcoord[0].w, value.w, projected.x, literal.x;\n"
    "DP4 value.x, t10, vertex.texcoord[1];\n"
    "DP4 value.y, t11, vertex.texcoord[1];\n"
    "DP4 value.z, t12, vertex.texcoord[1];\n"
    "DP4 value.w, t13, vertex.texcoord[1];\n"
    "MOV result.texcoord[1].xyz, value;\n"
    "MAD result.texcoord[1].w, value.w, projected.y, literal.y;\n"
    "DP4 value.x, t20, vertex.texcoord[2];\n"
    "DP4 value.y, t21, vertex.texcoord[2];\n"
    "DP4 value.z, t22, vertex.texcoord[2];\n"
    "DP4 value.w, t23, vertex.texcoord[2];\n"
    "MOV result.texcoord[2].xyz, value;\n"
    "MAD result.texcoord[2].w, value.w, projected.z, literal.z;\n"
    "DP4 value.x, t30, vertex.texcoord[3];\n"
    "DP4 value.y, t31, vertex.texcoord[3];\n"
    "DP4 value.z, t32, vertex.texcoord[3];\n"
    "DP4 value.w, t33, vertex.texcoord[3];\n"
    "MOV result.texcoord[3].xyz, value;\n"
    "MAD result.texcoord[3].w, value.w, projected.w, literal.w;\n"
    "END\n";

/* ABI 44's compact transformed path preserves the guest's tight XYZRHW FVF
 * record.  Attribute zero carries x/y/z/rhw, attributes three and four carry
 * the raw little-endian D3DCOLOR words, and attributes eight through eleven
 * are bound to the coordinate set selected by each texture stage.  Recreate
 * the canonical fixed path in one bounded ARBvp: use w=1 for the fixed ortho
 * position, swap D3DCOLOR's in-memory BGRA bytes, multiply ordinary texture
 * coordinates by rhw, and emit the same explicit transformed fog coordinate. */
static const UCHAR qxp_d3d_compact_transformed_program[] =
    "!!ARBvp1.0\n"
    "ATTRIB rawPosition = vertex.attrib[0];\n"
    "ATTRIB diffuse = vertex.attrib[3];\n"
    "ATTRIB secondary = vertex.attrib[4];\n"
    "ATTRIB rawTex0 = vertex.attrib[8];\n"
    "ATTRIB rawTex1 = vertex.attrib[9];\n"
    "ATTRIB rawTex2 = vertex.attrib[10];\n"
    "ATTRIB rawTex3 = vertex.attrib[11];\n"
    "PARAM fogControl = program.local[0];\n"
    "PARAM one = { 1.0, 0.0, 0.0, 0.0 };\n"
    "PARAM mvp0 = state.matrix.mvp.row[0];\n"
    "PARAM mvp1 = state.matrix.mvp.row[1];\n"
    "PARAM mvp2 = state.matrix.mvp.row[2];\n"
    "PARAM mvp3 = state.matrix.mvp.row[3];\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP position, tex, value, fog;\n"
    "MOV position, rawPosition;\n"
    "MOV position.w, one.x;\n"
    "DP4 result.position.x, mvp0, position;\n"
    "DP4 result.position.y, mvp1, position;\n"
    "DP4 result.position.z, mvp2, position;\n"
    "DP4 result.position.w, mvp3, position;\n"
    "MOV result.color, diffuse.zyxw;\n"
    "MOV result.color.secondary, secondary.zyxw;\n"
    "SUB fog.x, one.x, secondary.w;\n"
    "SUB fog.y, rawPosition.z, fog.x;\n"
    "MAD result.fogcoord.x, fog.y, fogControl.x, fog.x;\n"
    "MOV tex, one.y;\n"
    "MOV tex.w, rawPosition.w;\n"
    "MUL tex.xy, rawTex0, rawPosition.w;\n"
    "DP4 value.x, t00, tex;\n"
    "DP4 value.y, t01, tex;\n"
    "DP4 value.z, t02, tex;\n"
    "DP4 value.w, t03, tex;\n"
    "MOV result.texcoord[0], value;\n"
    "MOV tex, one.y;\n"
    "MOV tex.w, rawPosition.w;\n"
    "MUL tex.xy, rawTex1, rawPosition.w;\n"
    "DP4 value.x, t10, tex;\n"
    "DP4 value.y, t11, tex;\n"
    "DP4 value.z, t12, tex;\n"
    "DP4 value.w, t13, tex;\n"
    "MOV result.texcoord[1], value;\n"
    "MOV tex, one.y;\n"
    "MOV tex.w, rawPosition.w;\n"
    "MUL tex.xy, rawTex2, rawPosition.w;\n"
    "DP4 value.x, t20, tex;\n"
    "DP4 value.y, t21, tex;\n"
    "DP4 value.z, t22, tex;\n"
    "DP4 value.w, t23, tex;\n"
    "MOV result.texcoord[2], value;\n"
    "MOV tex, one.y;\n"
    "MOV tex.w, rawPosition.w;\n"
    "MUL tex.xy, rawTex3, rawPosition.w;\n"
    "DP4 value.x, t30, tex;\n"
    "DP4 value.y, t31, tex;\n"
    "DP4 value.z, t32, tex;\n"
    "DP4 value.w, t33, tex;\n"
    "MOV result.texcoord[3], value;\n"
    "MOV result.texcoord[4], one.y;\n"
    "MOV result.texcoord[4].w, rawPosition.w;\n"
    "END\n";

/* Fixed OpenGL has neither Direct3D's hard positional-light Range cutoff nor
 * its distinct spotlight inner/outer cones and Falloff exponent. This bounded
 * program covers one point or spot light with MATERIAL/COLOR1/COLOR2 material
 * sources, local or infinite specular viewing, normalized normals, optional
 * vertex fog, and the same four-stage texture-q rules as the lean program
 * above. Other lighting combinations stay on the established fixed path until
 * their complete equations have their own validated variant. */
static const UCHAR qxp_d3d_positional_range_program[] =
    "!!ARBvp1.0\n"
    "OPTION ARB_position_invariant;\n"
    "PARAM projected = program.local[0];\n"
    "PARAM literal = program.local[1];\n"
    "PARAM rangeFog = program.local[2];\n"
    "PARAM lightRange = program.local[3];\n"
    "PARAM spotControl = program.local[4];\n"
    "PARAM sourceControl = program.local[5];\n"
    "PARAM secondarySourceControl = program.local[6];\n"
    "PARAM viewerControl = program.local[7];\n"
    "PARAM epsilon = { 1.0e-20, 0.0, 0.0, 0.0 };\n"
    "PARAM infiniteViewer = { 0.0, 0.0, -1.0, 0.0 };\n"
    "PARAM pointSize = state.point.size;\n"
    "PARAM pointAttenuation = state.point.attenuation;\n"
    "PARAM mv[4] = { state.matrix.modelview };\n"
    "PARAM mvit[4] = { state.matrix.modelview.invtrans };\n"
    "PARAM lightPosition = state.light[0].position;\n"
    "PARAM attenuation = state.light[0].attenuation;\n"
    "PARAM spotDirection = state.light[0].spot.direction;\n"
    "PARAM innerDirection = state.light[1].spot.direction;\n"
    "PARAM lightDiffuse = state.light[0].diffuse;\n"
    "PARAM lightSpecular = state.light[0].specular;\n"
    "PARAM materialDiffuse = state.material.diffuse;\n"
    "PARAM materialSpecular = state.material.specular;\n"
    "PARAM materialPower = state.material.shininess;\n"
    "PARAM lightAmbient = state.light[0].ambient;\n"
    "PARAM materialAmbient = state.material.ambient;\n"
    "PARAM materialEmission = state.material.emission;\n"
    "PARAM globalAmbient = state.lightmodel.ambient;\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP value, eye, fog, normal, lightVector;\n"
    "TEMP distance, factor, denominator, color, diffuse;\n"
    "TEMP spot, cone;\n"
    "DP4 eye.x, mv[0], vertex.position;\n"
    "DP4 eye.y, mv[1], vertex.position;\n"
    "DP4 eye.z, mv[2], vertex.position;\n"
    "DP3 normal.x, mvit[0], vertex.normal;\n"
    "DP3 normal.y, mvit[1], vertex.normal;\n"
    "DP3 normal.z, mvit[2], vertex.normal;\n"
    "DP3 normal.w, normal, normal;\n"
    "MAX normal.w, normal.w, epsilon.x;\n"
    "RSQ normal.w, normal.w;\n"
    "MUL normal.xyz, normal, normal.w;\n"
    "SUB lightVector.xyz, lightPosition, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation.z, distance.x, attenuation.x;\n"
    "MAD denominator.x, attenuation.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.x, denominator.x;\n"
    "SGE factor.y, lightRange.x, distance.w;\n"
    "MUL factor.x, factor.x, factor.y;\n"
    "DP3 spot.w, spotDirection, spotDirection;\n"
    "MAX spot.w, spot.w, epsilon.x;\n"
    "RSQ spot.w, spot.w;\n"
    "MUL spot.xyz, -spotDirection, spot.w;\n"
    "DP3 spot.x, spot, lightVector;\n"
    "SUB cone.x, spot.x, spotDirection.w;\n"
    "SUB cone.y, innerDirection.w, spotDirection.w;\n"
    "MAX cone.y, cone.y, epsilon.x;\n"
    "RCP cone.y, cone.y;\n"
    "MUL cone.x, cone.x, cone.y;\n"
    "MAX cone.x, cone.x, 0;\n"
    "MIN cone.x, cone.x, 1;\n"
    "SLT cone.z, spotDirection.w, spot.x;\n"
    "MAX cone.x, cone.x, epsilon.x;\n"
    "POW cone.x, cone.x, spotControl.y;\n"
    "MUL cone.x, cone.x, cone.z;\n"
    "SUB cone.x, cone.x, 1;\n"
    "MAD cone.x, cone.x, spotControl.x, 1;\n"
    "MUL factor.x, factor.x, cone.x;\n"
    "DP3 factor.z, normal, lightVector;\n"
    "MAX factor.z, factor.z, 0;\n"
    "MUL factor.z, factor.z, factor.x;\n"
    "MOV diffuse, materialDiffuse;\n"
    "SUB color, vertex.color, materialDiffuse;\n"
    "MAD diffuse, color, sourceControl.x, diffuse;\n"
    "SUB color, vertex.color.secondary, materialDiffuse;\n"
    "MAD diffuse, color, secondarySourceControl.x, diffuse;\n"
    "MUL diffuse.xyz, diffuse, lightDiffuse;\n"
    "MAD cone, factor.x, lightAmbient, globalAmbient;\n"
    "MOV color, materialAmbient;\n"
    "SUB spot, vertex.color, materialAmbient;\n"
    "MAD color, spot, sourceControl.y, color;\n"
    "SUB spot, vertex.color.secondary, materialAmbient;\n"
    "MAD color, spot, secondarySourceControl.y, color;\n"
    "MOV spot, materialEmission;\n"
    "SUB denominator, vertex.color, materialEmission;\n"
    "MAD spot, denominator, sourceControl.z, spot;\n"
    "SUB denominator, vertex.color.secondary, materialEmission;\n"
    "MAD spot, denominator, secondarySourceControl.z, spot;\n"
    "MAD color, color, cone, spot;\n"
    "MOV result.color, diffuse;\n"
    "MAD result.color.xyz, factor.z, diffuse, color;\n"
    "MOV diffuse, materialSpecular;\n"
    "SUB color, vertex.color, materialSpecular;\n"
    "MAD diffuse, color, sourceControl.w, diffuse;\n"
    "SUB color, vertex.color.secondary, materialSpecular;\n"
    "MAD diffuse, color, secondarySourceControl.w, diffuse;\n"
    "DP3 distance.x, eye, eye;\n"
    "MAX distance.x, distance.x, epsilon.x;\n"
    "RSQ distance.x, distance.x;\n"
    "MUL distance.xyz, -eye, distance.x;\n"
    "SUB denominator.xyz, distance, infiniteViewer;\n"
    "MAD distance.xyz, denominator, viewerControl.x, infiniteViewer;\n"
    "ADD denominator.xyz, lightVector, distance;\n"
    "DP3 distance.x, denominator, denominator;\n"
    "MAX distance.x, distance.x, epsilon.x;\n"
    "RSQ distance.x, distance.x;\n"
    "MUL denominator.xyz, denominator, distance.x;\n"
    "DP3 distance.x, normal, denominator;\n"
    "MAX distance.x, distance.x, 0;\n"
    "POW distance.x, distance.x, materialPower.x;\n"
    "SLT distance.y, 0, factor.z;\n"
    "MUL distance.x, distance.x, distance.y;\n"
    "MUL distance.x, distance.x, factor.x;\n"
    "MUL distance.x, distance.x, viewerControl.y;\n"
    "MUL diffuse, diffuse, lightSpecular;\n"
    "MUL result.color.secondary, diffuse, distance.x;\n"
    "ABS fog.x, eye.z;\n"
    "DP3 fog.y, eye, eye;\n"
    "MAX fog.z, fog.y, epsilon.x;\n"
    "RSQ fog.z, fog.z;\n"
    "MUL fog.w, fog.y, fog.z;\n"
    "MAD denominator.x, pointAttenuation.z, fog.y, pointAttenuation.x;\n"
    "MAD denominator.x, pointAttenuation.y, fog.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RSQ denominator.x, denominator.x;\n"
    "MUL denominator.y, pointSize.x, denominator.x;\n"
    "MAX denominator.y, denominator.y, pointSize.y;\n"
    "MIN result.pointsize.x, denominator.y, pointSize.z;\n"
    "SUB fog.y, fog.w, fog.x;\n"
    "MAD fog.x, fog.y, rangeFog.x, fog.x;\n"
    "MAD result.fogcoord.x, fog.x, viewerControl.z, vertex.fogcoord.x;\n"
    "DP4 value.x, t00, vertex.texcoord[0];\n"
    "DP4 value.y, t01, vertex.texcoord[0];\n"
    "DP4 value.z, t02, vertex.texcoord[0];\n"
    "DP4 value.w, t03, vertex.texcoord[0];\n"
    "MOV result.texcoord[0].xyz, value;\n"
    "MAD result.texcoord[0].w, value.w, projected.x, literal.x;\n"
    "DP4 value.x, t10, vertex.texcoord[1];\n"
    "DP4 value.y, t11, vertex.texcoord[1];\n"
    "DP4 value.z, t12, vertex.texcoord[1];\n"
    "DP4 value.w, t13, vertex.texcoord[1];\n"
    "MOV result.texcoord[1].xyz, value;\n"
    "MAD result.texcoord[1].w, value.w, projected.y, literal.y;\n"
    "DP4 value.x, t20, vertex.texcoord[2];\n"
    "DP4 value.y, t21, vertex.texcoord[2];\n"
    "DP4 value.z, t22, vertex.texcoord[2];\n"
    "DP4 value.w, t23, vertex.texcoord[2];\n"
    "MOV result.texcoord[2].xyz, value;\n"
    "MAD result.texcoord[2].w, value.w, projected.z, literal.z;\n"
    "DP4 value.x, t30, vertex.texcoord[3];\n"
    "DP4 value.y, t31, vertex.texcoord[3];\n"
    "DP4 value.z, t32, vertex.texcoord[3];\n"
    "DP4 value.w, t33, vertex.texcoord[3];\n"
    "MOV result.texcoord[3].xyz, value;\n"
    "MAD result.texcoord[3].w, value.w, projected.w, literal.w;\n"
    "END\n";

/* Two simultaneous point lights with specular enabled need both independent
 * hard Range cutoffs and a separate specular term per light. Fixed OpenGL's
 * positional-light Range omission makes its color sum cross-contaminate the
 * lights, so this bounded program keeps the diffuse and specular factors
 * paired. It preserves MATERIAL/COLOR1/COLOR2 sources, local or infinite
 * viewing, optional normal normalization, and all four texture-q rules. Fog
 * and a third light remain on their complete, separately validated paths. */
static const UCHAR qxp_d3d_dual_point_specular_program[] =
    "!!ARBvp1.0\n"
    "OPTION ARB_position_invariant;\n"
    "PARAM projected = program.local[0];\n"
    "PARAM literal = program.local[1];\n"
    "PARAM lightRange = program.local[3];\n"
    "PARAM sourceControl = program.local[4];\n"
    "PARAM secondarySourceControl = program.local[5];\n"
    "PARAM normalizeControl = program.local[6];\n"
    "PARAM viewerControl = program.local[7];\n"
    "PARAM epsilon = { 1.0e-20, 0.0, 0.0, 0.0 };\n"
    "PARAM zero = { 0.0, 0.0, 0.0, 0.0 };\n"
    "PARAM infiniteViewer = { 0.0, 0.0, -1.0, 0.0 };\n"
    "PARAM mv[4] = { state.matrix.modelview };\n"
    "PARAM mvit[4] = { state.matrix.modelview.invtrans };\n"
    "PARAM lightPosition0 = state.light[0].position;\n"
    "PARAM attenuation0 = state.light[0].attenuation;\n"
    "PARAM lightDiffuse0 = state.light[0].diffuse;\n"
    "PARAM lightSpecular0 = state.light[0].specular;\n"
    "PARAM lightAmbient0 = state.light[0].ambient;\n"
    "PARAM lightPosition1 = state.light[1].position;\n"
    "PARAM attenuation1 = state.light[1].attenuation;\n"
    "PARAM lightDiffuse1 = state.light[1].diffuse;\n"
    "PARAM lightSpecular1 = state.light[1].specular;\n"
    "PARAM lightAmbient1 = state.light[1].ambient;\n"
    "PARAM materialDiffuse = state.material.diffuse;\n"
    "PARAM materialSpecular = state.material.specular;\n"
    "PARAM materialPower = state.material.shininess;\n"
    "PARAM materialAmbient = state.material.ambient;\n"
    "PARAM materialEmission = state.material.emission;\n"
    "PARAM globalAmbient = state.lightmodel.ambient;\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP value, eye, rawNormal, normal, lightVector, distance;\n"
    "TEMP denominator, factor, ambient, color, diffuse;\n"
    "TEMP source, emission, viewer, specularFactor, specular;\n"
    "DP4 eye.x, mv[0], vertex.position;\n"
    "DP4 eye.y, mv[1], vertex.position;\n"
    "DP4 eye.z, mv[2], vertex.position;\n"
    "DP3 rawNormal.x, mvit[0], vertex.normal;\n"
    "DP3 rawNormal.y, mvit[1], vertex.normal;\n"
    "DP3 rawNormal.z, mvit[2], vertex.normal;\n"
    "DP3 normal.w, rawNormal, rawNormal;\n"
    "MAX normal.w, normal.w, epsilon.x;\n"
    "RSQ normal.w, normal.w;\n"
    "MUL normal.xyz, rawNormal, normal.w;\n"
    "SUB normal.xyz, normal, rawNormal;\n"
    "MAD normal.xyz, normal, normalizeControl.x, rawNormal;\n"
    "DP3 viewer.w, eye, eye;\n"
    "MAX viewer.w, viewer.w, epsilon.x;\n"
    "RSQ viewer.w, viewer.w;\n"
    "MUL viewer.xyz, -eye, viewer.w;\n"
    "SUB denominator.xyz, viewer, infiniteViewer;\n"
    "MAD viewer.xyz, denominator, viewerControl.x, infiniteViewer;\n"
    "SUB lightVector.xyz, lightPosition0, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation0.z, distance.x, attenuation0.x;\n"
    "MAD denominator.x, attenuation0.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.x, denominator.x;\n"
    "SGE denominator.y, lightRange.x, distance.w;\n"
    "MUL factor.x, factor.x, denominator.y;\n"
    "DP3 factor.y, normal, lightVector;\n"
    "MAX factor.y, factor.y, 0;\n"
    "MUL factor.y, factor.y, factor.x;\n"
    "ADD denominator.xyz, lightVector, viewer;\n"
    "DP3 distance.y, denominator, denominator;\n"
    "MAX distance.y, distance.y, epsilon.x;\n"
    "RSQ distance.y, distance.y;\n"
    "MUL denominator.xyz, denominator, distance.y;\n"
    "DP3 denominator.y, normal, denominator;\n"
    "MOV denominator.x, factor.y;\n"
    "MOV denominator.w, materialPower.x;\n"
    "LIT distance, denominator;\n"
    "MUL specularFactor.x, distance.z, factor.x;\n"
    "SUB lightVector.xyz, lightPosition1, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation1.z, distance.x, attenuation1.x;\n"
    "MAD denominator.x, attenuation1.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.z, denominator.x;\n"
    "SGE denominator.y, lightRange.y, distance.w;\n"
    "MUL factor.z, factor.z, denominator.y;\n"
    "DP3 factor.w, normal, lightVector;\n"
    "MAX factor.w, factor.w, 0;\n"
    "MUL factor.w, factor.w, factor.z;\n"
    "ADD denominator.xyz, lightVector, viewer;\n"
    "DP3 distance.y, denominator, denominator;\n"
    "MAX distance.y, distance.y, epsilon.x;\n"
    "RSQ distance.y, distance.y;\n"
    "MUL denominator.xyz, denominator, distance.y;\n"
    "DP3 denominator.y, normal, denominator;\n"
    "MOV denominator.x, factor.w;\n"
    "MOV denominator.w, materialPower.x;\n"
    "LIT distance, denominator;\n"
    "MUL specularFactor.y, distance.z, factor.z;\n"
    "MOV diffuse, materialDiffuse;\n"
    "SUB source, vertex.color, materialDiffuse;\n"
    "MAD diffuse, source, sourceControl.x, diffuse;\n"
    "SUB source, vertex.color.secondary, materialDiffuse;\n"
    "MAD diffuse, source, secondarySourceControl.x, diffuse;\n"
    "MOV result.color.w, diffuse.w;\n"
    "MOV color, materialAmbient;\n"
    "SUB source, vertex.color, materialAmbient;\n"
    "MAD color, source, sourceControl.y, color;\n"
    "SUB source, vertex.color.secondary, materialAmbient;\n"
    "MAD color, source, secondarySourceControl.y, color;\n"
    "MAD ambient, factor.x, lightAmbient0, globalAmbient;\n"
    "MAD ambient, factor.z, lightAmbient1, ambient;\n"
    "MUL color, color, ambient;\n"
    "MOV emission, materialEmission;\n"
    "SUB source, vertex.color, materialEmission;\n"
    "MAD emission, source, sourceControl.z, emission;\n"
    "SUB source, vertex.color.secondary, materialEmission;\n"
    "MAD emission, source, secondarySourceControl.z, emission;\n"
    "ADD color, color, emission;\n"
    "MUL source, diffuse, lightDiffuse0;\n"
    "MAD color.xyz, factor.y, source, color;\n"
    "MUL source, diffuse, lightDiffuse1;\n"
    "MAD color.xyz, factor.w, source, color;\n"
    "MOV result.color.xyz, color;\n"
    "MOV specular, materialSpecular;\n"
    "SUB source, vertex.color, materialSpecular;\n"
    "MAD specular, source, sourceControl.w, specular;\n"
    "SUB source, vertex.color.secondary, materialSpecular;\n"
    "MAD specular, source, secondarySourceControl.w, specular;\n"
    "MUL source, specular, lightSpecular0;\n"
    "MUL emission, source, specularFactor.x;\n"
    "MUL source, specular, lightSpecular1;\n"
    "MAD emission, specularFactor.y, source, emission;\n"
    "MOV result.color.secondary, emission;\n"
    "MOV result.fogcoord.x, zero.x;\n"
    "DP4 value.x, t00, vertex.texcoord[0];\n"
    "DP4 value.y, t01, vertex.texcoord[0];\n"
    "DP4 value.z, t02, vertex.texcoord[0];\n"
    "DP4 value.w, t03, vertex.texcoord[0];\n"
    "MOV result.texcoord[0].xyz, value;\n"
    "MAD result.texcoord[0].w, value.w, projected.x, literal.x;\n"
    "DP4 value.x, t10, vertex.texcoord[1];\n"
    "DP4 value.y, t11, vertex.texcoord[1];\n"
    "DP4 value.z, t12, vertex.texcoord[1];\n"
    "DP4 value.w, t13, vertex.texcoord[1];\n"
    "MOV result.texcoord[1].xyz, value;\n"
    "MAD result.texcoord[1].w, value.w, projected.y, literal.y;\n"
    "DP4 value.x, t20, vertex.texcoord[2];\n"
    "DP4 value.y, t21, vertex.texcoord[2];\n"
    "DP4 value.z, t22, vertex.texcoord[2];\n"
    "DP4 value.w, t23, vertex.texcoord[2];\n"
    "MOV result.texcoord[2].xyz, value;\n"
    "MAD result.texcoord[2].w, value.w, projected.z, literal.z;\n"
    "DP4 value.x, t30, vertex.texcoord[3];\n"
    "DP4 value.y, t31, vertex.texcoord[3];\n"
    "DP4 value.z, t32, vertex.texcoord[3];\n"
    "DP4 value.w, t33, vertex.texcoord[3];\n"
    "MOV result.texcoord[3].xyz, value;\n"
    "MAD result.texcoord[3].w, value.w, projected.w, literal.w;\n"
    "END\n";

/* Four simultaneous point lights need four independent hard Range cutoffs.
 * This portable 128-instruction variant leaves fog, specular, spotlight, and
 * point-primitive cases on their conservative paths. Within that complete
 * subset it retains optional normal normalization, independent attenuation,
 * MATERIAL/COLOR1/COLOR2 sources, and all four texture-q rules. */
static const UCHAR qxp_d3d_quad_point_range_program[] =
    "!!ARBvp1.0\n"
    "OPTION ARB_position_invariant;\n"
    "PARAM projected = program.local[0];\n"
    "PARAM literal = program.local[1];\n"
    "PARAM lightRange = program.local[3];\n"
    "PARAM sourceControl = program.local[4];\n"
    "PARAM secondarySourceControl = program.local[5];\n"
    "PARAM normalizeControl = program.local[6];\n"
    "PARAM epsilon = { 1.0e-20, 0.0, 0.0, 0.0 };\n"
    "PARAM zero = { 0.0, 0.0, 0.0, 0.0 };\n"
    "PARAM mv[4] = { state.matrix.modelview };\n"
    "PARAM mvit[4] = { state.matrix.modelview.invtrans };\n"
    "PARAM lightPosition0 = state.light[0].position;\n"
    "PARAM attenuation0 = state.light[0].attenuation;\n"
    "PARAM lightDiffuse0 = state.light[0].diffuse;\n"
    "PARAM lightAmbient0 = state.light[0].ambient;\n"
    "PARAM lightPosition1 = state.light[1].position;\n"
    "PARAM attenuation1 = state.light[1].attenuation;\n"
    "PARAM lightDiffuse1 = state.light[1].diffuse;\n"
    "PARAM lightAmbient1 = state.light[1].ambient;\n"
    "PARAM lightPosition2 = state.light[2].position;\n"
    "PARAM attenuation2 = state.light[2].attenuation;\n"
    "PARAM lightDiffuse2 = state.light[2].diffuse;\n"
    "PARAM lightAmbient2 = state.light[2].ambient;\n"
    "PARAM lightPosition3 = state.light[3].position;\n"
    "PARAM attenuation3 = state.light[3].attenuation;\n"
    "PARAM lightDiffuse3 = state.light[3].diffuse;\n"
    "PARAM lightAmbient3 = state.light[3].ambient;\n"
    "PARAM materialDiffuse = state.material.diffuse;\n"
    "PARAM materialAmbient = state.material.ambient;\n"
    "PARAM materialEmission = state.material.emission;\n"
    "PARAM globalAmbient = state.lightmodel.ambient;\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP value, eye, rawNormal, normal, lightVector, distance;\n"
    "TEMP denominator, factor, ambient, color, diffuse;\n"
    "TEMP source, emission, third, fourth;\n"
    "DP4 eye.x, mv[0], vertex.position;\n"
    "DP4 eye.y, mv[1], vertex.position;\n"
    "DP4 eye.z, mv[2], vertex.position;\n"
    "DP3 rawNormal.x, mvit[0], vertex.normal;\n"
    "DP3 rawNormal.y, mvit[1], vertex.normal;\n"
    "DP3 rawNormal.z, mvit[2], vertex.normal;\n"
    "DP3 normal.w, rawNormal, rawNormal;\n"
    "MAX normal.w, normal.w, epsilon.x;\n"
    "RSQ normal.w, normal.w;\n"
    "MUL normal.xyz, rawNormal, normal.w;\n"
    "SUB normal.xyz, normal, rawNormal;\n"
    "MAD normal.xyz, normal, normalizeControl.x, rawNormal;\n"
    "SUB lightVector.xyz, lightPosition0, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation0.z, distance.x, attenuation0.x;\n"
    "MAD denominator.x, attenuation0.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.x, denominator.x;\n"
    "SGE denominator.y, lightRange.x, distance.w;\n"
    "MUL factor.x, factor.x, denominator.y;\n"
    "DP3 factor.y, normal, lightVector;\n"
    "MAX factor.y, factor.y, 0;\n"
    "MUL factor.y, factor.y, factor.x;\n"
    "SUB lightVector.xyz, lightPosition1, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation1.z, distance.x, attenuation1.x;\n"
    "MAD denominator.x, attenuation1.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.z, denominator.x;\n"
    "SGE denominator.y, lightRange.y, distance.w;\n"
    "MUL factor.z, factor.z, denominator.y;\n"
    "DP3 factor.w, normal, lightVector;\n"
    "MAX factor.w, factor.w, 0;\n"
    "MUL factor.w, factor.w, factor.z;\n"
    "SUB lightVector.xyz, lightPosition2, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation2.z, distance.x, attenuation2.x;\n"
    "MAD denominator.x, attenuation2.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP third.x, denominator.x;\n"
    "SGE denominator.y, lightRange.z, distance.w;\n"
    "MUL third.x, third.x, denominator.y;\n"
    "DP3 third.y, normal, lightVector;\n"
    "MAX third.y, third.y, 0;\n"
    "MUL third.y, third.y, third.x;\n"
    "SUB lightVector.xyz, lightPosition3, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation3.z, distance.x, attenuation3.x;\n"
    "MAD denominator.x, attenuation3.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP fourth.x, denominator.x;\n"
    "SGE denominator.y, lightRange.w, distance.w;\n"
    "MUL fourth.x, fourth.x, denominator.y;\n"
    "DP3 fourth.y, normal, lightVector;\n"
    "MAX fourth.y, fourth.y, 0;\n"
    "MUL fourth.y, fourth.y, fourth.x;\n"
    "MOV diffuse, materialDiffuse;\n"
    "SUB source, vertex.color, materialDiffuse;\n"
    "MAD diffuse, source, sourceControl.x, diffuse;\n"
    "SUB source, vertex.color.secondary, materialDiffuse;\n"
    "MAD diffuse, source, secondarySourceControl.x, diffuse;\n"
    "MOV result.color.w, diffuse.w;\n"
    "MOV color, materialAmbient;\n"
    "SUB source, vertex.color, materialAmbient;\n"
    "MAD color, source, sourceControl.y, color;\n"
    "SUB source, vertex.color.secondary, materialAmbient;\n"
    "MAD color, source, secondarySourceControl.y, color;\n"
    "MAD ambient, factor.x, lightAmbient0, globalAmbient;\n"
    "MAD ambient, factor.z, lightAmbient1, ambient;\n"
    "MAD ambient, third.x, lightAmbient2, ambient;\n"
    "MAD ambient, fourth.x, lightAmbient3, ambient;\n"
    "MUL color, color, ambient;\n"
    "MOV emission, materialEmission;\n"
    "SUB source, vertex.color, materialEmission;\n"
    "MAD emission, source, sourceControl.z, emission;\n"
    "SUB source, vertex.color.secondary, materialEmission;\n"
    "MAD emission, source, secondarySourceControl.z, emission;\n"
    "ADD color, color, emission;\n"
    "MUL source, diffuse, lightDiffuse0;\n"
    "MAD color.xyz, factor.y, source, color;\n"
    "MUL source, diffuse, lightDiffuse1;\n"
    "MAD color.xyz, factor.w, source, color;\n"
    "MUL source, diffuse, lightDiffuse2;\n"
    "MAD color.xyz, third.y, source, color;\n"
    "MUL source, diffuse, lightDiffuse3;\n"
    "MAD color.xyz, fourth.y, source, color;\n"
    "MOV result.color.xyz, color;\n"
    "MOV result.color.secondary, zero;\n"
    "DP4 value.x, t00, vertex.texcoord[0];\n"
    "DP4 value.y, t01, vertex.texcoord[0];\n"
    "DP4 value.z, t02, vertex.texcoord[0];\n"
    "DP4 value.w, t03, vertex.texcoord[0];\n"
    "MOV result.texcoord[0].xyz, value;\n"
    "MAD result.texcoord[0].w, value.w, projected.x, literal.x;\n"
    "DP4 value.x, t10, vertex.texcoord[1];\n"
    "DP4 value.y, t11, vertex.texcoord[1];\n"
    "DP4 value.z, t12, vertex.texcoord[1];\n"
    "DP4 value.w, t13, vertex.texcoord[1];\n"
    "MOV result.texcoord[1].xyz, value;\n"
    "MAD result.texcoord[1].w, value.w, projected.y, literal.y;\n"
    "DP4 value.x, t20, vertex.texcoord[2];\n"
    "DP4 value.y, t21, vertex.texcoord[2];\n"
    "DP4 value.z, t22, vertex.texcoord[2];\n"
    "DP4 value.w, t23, vertex.texcoord[2];\n"
    "MOV result.texcoord[2].xyz, value;\n"
    "MAD result.texcoord[2].w, value.w, projected.z, literal.z;\n"
    "DP4 value.x, t30, vertex.texcoord[3];\n"
    "DP4 value.y, t31, vertex.texcoord[3];\n"
    "DP4 value.z, t32, vertex.texcoord[3];\n"
    "DP4 value.w, t33, vertex.texcoord[3];\n"
    "MOV result.texcoord[3].xyz, value;\n"
    "MAD result.texcoord[3].w, value.w, projected.w, literal.w;\n"
    "END\n";

/* Two or three simultaneous point lights need independent hard Range cutoffs,
 * which fixed OpenGL cannot express. Keep this variant deliberately bounded:
 * diffuse/ambient/emissive material sources without specular. It emits D3D's
 * eye-space vertex-fog coordinate or selects supplied post-divide table fog.
 * It still implements all four D3D texture transforms and projected-q rules,
 * so selecting it does not trade lighting correctness for texture semantics. */
static const UCHAR qxp_d3d_multi_point_range_program[] =
    "!!ARBvp1.0\n"
    "OPTION ARB_position_invariant;\n"
    "PARAM projected = program.local[0];\n"
    "PARAM literal = program.local[1];\n"
    "PARAM rangeFog = program.local[2];\n"
    "PARAM lightRange = program.local[3];\n"
    "PARAM sourceControl = program.local[4];\n"
    "PARAM secondarySourceControl = program.local[5];\n"
    "PARAM normalizeControl = program.local[6];\n"
    "PARAM lightControl = program.local[7];\n"
    "PARAM epsilon = { 1.0e-20, 0.0, 0.0, 0.0 };\n"
    "PARAM zero = { 0.0, 0.0, 0.0, 0.0 };\n"
    "PARAM mv[4] = { state.matrix.modelview };\n"
    "PARAM mvit[4] = { state.matrix.modelview.invtrans };\n"
    "PARAM lightPosition0 = state.light[0].position;\n"
    "PARAM attenuation0 = state.light[0].attenuation;\n"
    "PARAM lightDiffuse0 = state.light[0].diffuse;\n"
    "PARAM lightAmbient0 = state.light[0].ambient;\n"
    "PARAM lightPosition1 = state.light[1].position;\n"
    "PARAM attenuation1 = state.light[1].attenuation;\n"
    "PARAM lightDiffuse1 = state.light[1].diffuse;\n"
    "PARAM lightAmbient1 = state.light[1].ambient;\n"
    "PARAM lightPosition2 = state.light[2].position;\n"
    "PARAM attenuation2 = state.light[2].attenuation;\n"
    "PARAM lightDiffuse2 = state.light[2].diffuse;\n"
    "PARAM lightAmbient2 = state.light[2].ambient;\n"
    "PARAM materialDiffuse = state.material.diffuse;\n"
    "PARAM materialAmbient = state.material.ambient;\n"
    "PARAM materialEmission = state.material.emission;\n"
    "PARAM globalAmbient = state.lightmodel.ambient;\n"
    "PARAM t00 = state.matrix.texture[0].row[0];\n"
    "PARAM t01 = state.matrix.texture[0].row[1];\n"
    "PARAM t02 = state.matrix.texture[0].row[2];\n"
    "PARAM t03 = state.matrix.texture[0].row[3];\n"
    "PARAM t10 = state.matrix.texture[1].row[0];\n"
    "PARAM t11 = state.matrix.texture[1].row[1];\n"
    "PARAM t12 = state.matrix.texture[1].row[2];\n"
    "PARAM t13 = state.matrix.texture[1].row[3];\n"
    "PARAM t20 = state.matrix.texture[2].row[0];\n"
    "PARAM t21 = state.matrix.texture[2].row[1];\n"
    "PARAM t22 = state.matrix.texture[2].row[2];\n"
    "PARAM t23 = state.matrix.texture[2].row[3];\n"
    "PARAM t30 = state.matrix.texture[3].row[0];\n"
    "PARAM t31 = state.matrix.texture[3].row[1];\n"
    "PARAM t32 = state.matrix.texture[3].row[2];\n"
    "PARAM t33 = state.matrix.texture[3].row[3];\n"
    "TEMP value, eye, rawNormal, normal, lightVector, distance;\n"
    "TEMP denominator, factor, ambient, color, diffuse;\n"
    "TEMP source, emission, third, fog;\n"
    "DP4 eye.x, mv[0], vertex.position;\n"
    "DP4 eye.y, mv[1], vertex.position;\n"
    "DP4 eye.z, mv[2], vertex.position;\n"
    "DP3 rawNormal.x, mvit[0], vertex.normal;\n"
    "DP3 rawNormal.y, mvit[1], vertex.normal;\n"
    "DP3 rawNormal.z, mvit[2], vertex.normal;\n"
    "DP3 normal.w, rawNormal, rawNormal;\n"
    "MAX normal.w, normal.w, epsilon.x;\n"
    "RSQ normal.w, normal.w;\n"
    "MUL normal.xyz, rawNormal, normal.w;\n"
    "SUB normal.xyz, normal, rawNormal;\n"
    "MAD normal.xyz, normal, normalizeControl.x, rawNormal;\n"
    "SUB lightVector.xyz, lightPosition0, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation0.z, distance.x, attenuation0.x;\n"
    "MAD denominator.x, attenuation0.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.x, denominator.x;\n"
    "SGE denominator.y, lightRange.x, distance.w;\n"
    "MUL factor.x, factor.x, denominator.y;\n"
    "DP3 factor.y, normal, lightVector;\n"
    "MAX factor.y, factor.y, 0;\n"
    "MUL factor.y, factor.y, factor.x;\n"
    "SUB lightVector.xyz, lightPosition1, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation1.z, distance.x, attenuation1.x;\n"
    "MAD denominator.x, attenuation1.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP factor.z, denominator.x;\n"
    "SGE denominator.y, lightRange.y, distance.w;\n"
    "MUL factor.z, factor.z, denominator.y;\n"
    "DP3 factor.w, normal, lightVector;\n"
    "MAX factor.w, factor.w, 0;\n"
    "MUL factor.w, factor.w, factor.z;\n"
    "SUB lightVector.xyz, lightPosition2, eye;\n"
    "DP3 distance.x, lightVector, lightVector;\n"
    "MAX distance.y, distance.x, epsilon.x;\n"
    "RSQ distance.z, distance.y;\n"
    "MUL lightVector.xyz, lightVector, distance.z;\n"
    "MUL distance.w, distance.x, distance.z;\n"
    "MAD denominator.x, attenuation2.z, distance.x, attenuation2.x;\n"
    "MAD denominator.x, attenuation2.y, distance.w, denominator.x;\n"
    "MAX denominator.x, denominator.x, epsilon.x;\n"
    "RCP third.x, denominator.x;\n"
    "SGE denominator.y, lightRange.z, distance.w;\n"
    "MUL third.x, third.x, denominator.y;\n"
    "MUL third.x, third.x, lightControl.x;\n"
    "DP3 third.y, normal, lightVector;\n"
    "MAX third.y, third.y, 0;\n"
    "MUL third.y, third.y, third.x;\n"
    "MOV diffuse, materialDiffuse;\n"
    "SUB source, vertex.color, materialDiffuse;\n"
    "MAD diffuse, source, sourceControl.x, diffuse;\n"
    "SUB source, vertex.color.secondary, materialDiffuse;\n"
    "MAD diffuse, source, secondarySourceControl.x, diffuse;\n"
    "MOV result.color.w, diffuse.w;\n"
    "MOV color, materialAmbient;\n"
    "SUB source, vertex.color, materialAmbient;\n"
    "MAD color, source, sourceControl.y, color;\n"
    "SUB source, vertex.color.secondary, materialAmbient;\n"
    "MAD color, source, secondarySourceControl.y, color;\n"
    "MAD ambient, factor.x, lightAmbient0, globalAmbient;\n"
    "MAD ambient, factor.z, lightAmbient1, ambient;\n"
    "MAD ambient, third.x, lightAmbient2, ambient;\n"
    "MUL color, color, ambient;\n"
    "MOV emission, materialEmission;\n"
    "SUB source, vertex.color, materialEmission;\n"
    "MAD emission, source, sourceControl.z, emission;\n"
    "SUB source, vertex.color.secondary, materialEmission;\n"
    "MAD emission, source, secondarySourceControl.z, emission;\n"
    "ADD color, color, emission;\n"
    "MUL source, diffuse, lightDiffuse0;\n"
    "MAD color.xyz, factor.y, source, color;\n"
    "MUL source, diffuse, lightDiffuse1;\n"
    "MAD color.xyz, factor.w, source, color;\n"
    "MUL source, diffuse, lightDiffuse2;\n"
    "MAD color.xyz, third.y, source, color;\n"
    "MOV result.color.xyz, color;\n"
    "MOV result.color.secondary, zero;\n"
    "ABS fog.x, eye.z;\n"
    "DP3 fog.y, eye, eye;\n"
    "MAX fog.z, fog.y, epsilon.x;\n"
    "RSQ fog.z, fog.z;\n"
    "MUL fog.w, fog.y, fog.z;\n"
    "SUB fog.y, fog.w, fog.x;\n"
    "MAD fog.x, fog.y, rangeFog.x, fog.x;\n"
    "MAD result.fogcoord.x, fog.x, lightControl.y, vertex.fogcoord.x;\n"
    "DP4 value.x, t00, vertex.texcoord[0];\n"
    "DP4 value.y, t01, vertex.texcoord[0];\n"
    "DP4 value.z, t02, vertex.texcoord[0];\n"
    "DP4 value.w, t03, vertex.texcoord[0];\n"
    "MOV result.texcoord[0].xyz, value;\n"
    "MAD result.texcoord[0].w, value.w, projected.x, literal.x;\n"
    "DP4 value.x, t10, vertex.texcoord[1];\n"
    "DP4 value.y, t11, vertex.texcoord[1];\n"
    "DP4 value.z, t12, vertex.texcoord[1];\n"
    "DP4 value.w, t13, vertex.texcoord[1];\n"
    "MOV result.texcoord[1].xyz, value;\n"
    "MAD result.texcoord[1].w, value.w, projected.y, literal.y;\n"
    "DP4 value.x, t20, vertex.texcoord[2];\n"
    "DP4 value.y, t21, vertex.texcoord[2];\n"
    "DP4 value.z, t22, vertex.texcoord[2];\n"
    "DP4 value.w, t23, vertex.texcoord[2];\n"
    "MOV result.texcoord[2].xyz, value;\n"
    "MAD result.texcoord[2].w, value.w, projected.z, literal.z;\n"
    "DP4 value.x, t30, vertex.texcoord[3];\n"
    "DP4 value.y, t31, vertex.texcoord[3];\n"
    "DP4 value.z, t32, vertex.texcoord[3];\n"
    "DP4 value.w, t33, vertex.texcoord[3];\n"
    "MOV result.texcoord[3].xyz, value;\n"
    "MAD result.texcoord[3].w, value.w, projected.w, literal.w;\n"
    "END\n";

/* GLSL removes ARBvp1.0's portable 128-instruction ceiling without changing
 * the guest ABI or advertising programmable D3D vertex shaders. The selector
 * handles broad fixed-lighting subsets and any object draw that combines an
 * internal vertex program with camera-generated texture coordinates. It keeps
 * independent Range/attenuation, D3D inner/outer-cone Falloff, local or
 * infinite viewing, all material vertex-color sources, all documented TCI
 * modes and four texture transforms, vertex/table fog, user clipping, and D3D
 * point scaling exact. */
static const UCHAR qxp_d3d_scalable_point_program[] =
    "#version 110\n"
    "uniform int qxpLightCount;\n"
    "uniform vec4 qxpRange0;\n"
    "uniform vec4 qxpRange1;\n"
    "uniform vec4 qxpProjected;\n"
    "uniform vec4 qxpLiteral;\n"
    "uniform vec4 qxpSource;\n"
    "uniform vec4 qxpSecondary;\n"
    "uniform vec4 qxpNormalize;\n"
    "uniform vec4 qxpMaterialSpecular;\n"
    "uniform vec4 qxpFog;\n"
    "uniform vec4 qxpTexgen;\n"
    "void qxpAddLight(vec4 position, float constantAttenuation,\n"
    "                 float linearAttenuation,\n"
    "                 float quadraticAttenuation, vec4 lightAmbient,\n"
    "                 vec4 lightDiffuse, vec4 lightSpecular,\n"
    "                 vec3 spotDirection, float spotOuter,\n"
    "                 float lightRange, vec3 eye, vec3 normal,\n"
    "                 vec3 viewer, vec4 materialDiffuse,\n"
    "                 vec4 materialSpecular, float materialPower,\n"
    "                 inout vec4 ambient, inout vec3 color,\n"
    "                 inout vec3 specular)\n"
    "{\n"
    "    vec3 vector = position.xyz - eye * position.w;\n"
    "    float distanceSquared = dot(vector, vector);\n"
    "    float inverseDistance =\n"
    "        inversesqrt(max(distanceSquared, 1.0e-20));\n"
    "    float distanceValue = distanceSquared * inverseDistance;\n"
    "    vec3 direction = vector * inverseDistance;\n"
    "    float attenuation = 1.0;\n"
    "    if (position.w != 0.0) {\n"
    "        float denominator = max(constantAttenuation +\n"
    "            linearAttenuation * distanceValue +\n"
    "            quadraticAttenuation * distanceSquared, 1.0e-20);\n"
    "        attenuation = step(distanceValue, lightRange) / denominator;\n"
    "    }\n"
    "    if (spotOuter >= 0.0) {\n"
    "        vec3 unitSpot = spotDirection *\n"
    "            inversesqrt(max(dot(spotDirection, spotDirection),\n"
    "                            1.0e-20));\n"
    "        float rho = dot(-direction, unitSpot);\n"
    "        float spotFactor = 0.0;\n"
    "        if (rho > spotOuter) {\n"
    "            float spotInner = cos(radians(lightDiffuse.a));\n"
    "            spotFactor = 1.0;\n"
    "            if (rho < spotInner && lightAmbient.a > 0.0)\n"
    "                spotFactor = pow((rho - spotOuter) /\n"
    "                    max(spotInner - spotOuter, 1.0e-20),\n"
    "                    lightAmbient.a);\n"
    "        }\n"
    "        attenuation *= spotFactor;\n"
    "    }\n"
    "    float frontFactor = max(dot(normal, direction), 0.0);\n"
    "    float diffuseFactor = frontFactor * attenuation;\n"
    "    ambient += attenuation * lightAmbient;\n"
    "    color += diffuseFactor * materialDiffuse.rgb * lightDiffuse.rgb;\n"
    "    if (qxpNormalize.z > 0.5 && frontFactor > 0.0) {\n"
    "        vec3 halfVector = direction + viewer;\n"
    "        halfVector *= inversesqrt(max(dot(halfVector, halfVector),\n"
    "                                     1.0e-20));\n"
    "        float specularFactor = 1.0;\n"
    "        if (materialPower > 0.0)\n"
    "            specularFactor = pow(max(dot(normal, halfVector), 0.0),\n"
    "                                 materialPower);\n"
    "        specularFactor *= attenuation;\n"
    "        specular += specularFactor * materialSpecular.rgb *\n"
    "                    lightSpecular.rgb;\n"
    "    }\n"
    "}\n"
    "vec4 qxpTexcoord(vec4 supplied, float mode, vec3 eye, vec3 normal)\n"
    "{\n"
    "    if (mode < 0.5) return supplied;\n"
    "    if (mode < 1.5) return vec4(normal, 1.0);\n"
    "    if (mode < 2.5) return vec4(eye, 1.0);\n"
    "    vec3 incident = eye *\n"
    "        inversesqrt(max(dot(eye, eye), 1.0e-20));\n"
    "    return vec4(reflect(incident, normal), 1.0);\n"
    "}\n"
    "void main(void)\n"
    "{\n"
    "    vec4 eye4 = gl_ModelViewMatrix * gl_Vertex;\n"
    "    vec3 eye = eye4.xyz;\n"
    "    vec3 rawNormal = vec3(0.0);\n"
    "    if (qxpFog.z > 0.5)\n"
    "        rawNormal = gl_NormalMatrix * gl_Normal;\n"
    "    vec3 unitNormal = rawNormal *\n"
    "        inversesqrt(max(dot(rawNormal, rawNormal), 1.0e-20));\n"
    "    vec3 normal = mix(rawNormal, unitNormal, qxpNormalize.x);\n"
    "    vec3 localViewer = -eye *\n"
    "        inversesqrt(max(dot(eye, eye), 1.0e-20));\n"
    "    vec3 viewer = mix(vec3(0.0, 0.0, -1.0), localViewer,\n"
    "                      qxpNormalize.y);\n"
    "    vec4 materialDiffuse = gl_FrontMaterial.diffuse;\n"
    "    materialDiffuse +=\n"
    "        (gl_Color - materialDiffuse) * qxpSource.x;\n"
    "    materialDiffuse +=\n"
    "        (gl_SecondaryColor - materialDiffuse) * qxpSecondary.x;\n"
    "    vec4 materialAmbient = gl_FrontMaterial.ambient;\n"
    "    materialAmbient +=\n"
    "        (gl_Color - materialAmbient) * qxpSource.y;\n"
    "    materialAmbient +=\n"
    "        (gl_SecondaryColor - materialAmbient) * qxpSecondary.y;\n"
    "    vec4 materialEmission = gl_FrontMaterial.emission;\n"
    "    materialEmission +=\n"
    "        (gl_Color - materialEmission) * qxpSource.z;\n"
    "    materialEmission +=\n"
    "        (gl_SecondaryColor - materialEmission) * qxpSecondary.z;\n"
    "    vec4 materialSpecular = vec4(qxpMaterialSpecular.rgb, 1.0);\n"
    "    materialSpecular +=\n"
    "        (gl_Color - materialSpecular) * qxpSource.w;\n"
    "    materialSpecular +=\n"
    "        (gl_SecondaryColor - materialSpecular) * qxpSecondary.w;\n"
    "    vec4 ambient = gl_LightModel.ambient;\n"
    "    vec3 color = materialEmission.rgb;\n"
    "    vec3 specular = vec3(0.0);\n"
    "    if (qxpLightCount > 0) qxpAddLight(gl_LightSource[0].position,\n"
    "        gl_LightSource[0].constantAttenuation,\n"
    "        gl_LightSource[0].linearAttenuation,\n"
    "        gl_LightSource[0].quadraticAttenuation,\n"
    "        gl_LightSource[0].ambient, gl_LightSource[0].diffuse,\n"
    "        gl_LightSource[0].specular, gl_LightSource[0].spotDirection,\n"
    "        gl_LightSource[0].spotCosCutoff, qxpRange0.x, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 1) qxpAddLight(gl_LightSource[1].position,\n"
    "        gl_LightSource[1].constantAttenuation,\n"
    "        gl_LightSource[1].linearAttenuation,\n"
    "        gl_LightSource[1].quadraticAttenuation,\n"
    "        gl_LightSource[1].ambient, gl_LightSource[1].diffuse,\n"
    "        gl_LightSource[1].specular, gl_LightSource[1].spotDirection,\n"
    "        gl_LightSource[1].spotCosCutoff, qxpRange0.y, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 2) qxpAddLight(gl_LightSource[2].position,\n"
    "        gl_LightSource[2].constantAttenuation,\n"
    "        gl_LightSource[2].linearAttenuation,\n"
    "        gl_LightSource[2].quadraticAttenuation,\n"
    "        gl_LightSource[2].ambient, gl_LightSource[2].diffuse,\n"
    "        gl_LightSource[2].specular, gl_LightSource[2].spotDirection,\n"
    "        gl_LightSource[2].spotCosCutoff, qxpRange0.z, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 3) qxpAddLight(gl_LightSource[3].position,\n"
    "        gl_LightSource[3].constantAttenuation,\n"
    "        gl_LightSource[3].linearAttenuation,\n"
    "        gl_LightSource[3].quadraticAttenuation,\n"
    "        gl_LightSource[3].ambient, gl_LightSource[3].diffuse,\n"
    "        gl_LightSource[3].specular, gl_LightSource[3].spotDirection,\n"
    "        gl_LightSource[3].spotCosCutoff, qxpRange0.w, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 4) qxpAddLight(gl_LightSource[4].position,\n"
    "        gl_LightSource[4].constantAttenuation,\n"
    "        gl_LightSource[4].linearAttenuation,\n"
    "        gl_LightSource[4].quadraticAttenuation,\n"
    "        gl_LightSource[4].ambient, gl_LightSource[4].diffuse,\n"
    "        gl_LightSource[4].specular, gl_LightSource[4].spotDirection,\n"
    "        gl_LightSource[4].spotCosCutoff, qxpRange1.x, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 5) qxpAddLight(gl_LightSource[5].position,\n"
    "        gl_LightSource[5].constantAttenuation,\n"
    "        gl_LightSource[5].linearAttenuation,\n"
    "        gl_LightSource[5].quadraticAttenuation,\n"
    "        gl_LightSource[5].ambient, gl_LightSource[5].diffuse,\n"
    "        gl_LightSource[5].specular, gl_LightSource[5].spotDirection,\n"
    "        gl_LightSource[5].spotCosCutoff, qxpRange1.y, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 6) qxpAddLight(gl_LightSource[6].position,\n"
    "        gl_LightSource[6].constantAttenuation,\n"
    "        gl_LightSource[6].linearAttenuation,\n"
    "        gl_LightSource[6].quadraticAttenuation,\n"
    "        gl_LightSource[6].ambient, gl_LightSource[6].diffuse,\n"
    "        gl_LightSource[6].specular, gl_LightSource[6].spotDirection,\n"
    "        gl_LightSource[6].spotCosCutoff, qxpRange1.z, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    if (qxpLightCount > 7) qxpAddLight(gl_LightSource[7].position,\n"
    "        gl_LightSource[7].constantAttenuation,\n"
    "        gl_LightSource[7].linearAttenuation,\n"
    "        gl_LightSource[7].quadraticAttenuation,\n"
    "        gl_LightSource[7].ambient, gl_LightSource[7].diffuse,\n"
    "        gl_LightSource[7].specular, gl_LightSource[7].spotDirection,\n"
    "        gl_LightSource[7].spotCosCutoff, qxpRange1.w, eye, normal, viewer,\n"
    "        materialDiffuse, materialSpecular, qxpMaterialSpecular.w,\n"
    "        ambient, color, specular);\n"
    "    color += materialAmbient.rgb * ambient.rgb;\n"
    "    vec4 litPrimary = vec4(color, materialDiffuse.a);\n"
    "    vec4 primary = mix(gl_Color, litPrimary, qxpNormalize.w);\n"
    "    gl_FrontColor = primary;\n"
    "    gl_BackColor = primary;\n"
    "    vec4 litSecondary = vec4(specular, 0.0);\n"
    "    vec4 secondary = mix(gl_SecondaryColor, litSecondary,\n"
    "                         qxpNormalize.w);\n"
    "    gl_FrontSecondaryColor = secondary;\n"
    "    gl_BackSecondaryColor = secondary;\n"
    "    vec4 textureSource = qxpTexcoord(\n"
    "        gl_MultiTexCoord0, qxpTexgen.x, eye, normal);\n"
    "    vec4 textureValue = gl_TextureMatrix[0] * textureSource;\n"
    "    gl_TexCoord[0] = vec4(textureValue.xyz,\n"
    "        textureValue.w * qxpProjected.x + qxpLiteral.x);\n"
    "    textureSource = qxpTexcoord(\n"
    "        gl_MultiTexCoord1, qxpTexgen.y, eye, normal);\n"
    "    textureValue = gl_TextureMatrix[1] * textureSource;\n"
    "    gl_TexCoord[1] = vec4(textureValue.xyz,\n"
    "        textureValue.w * qxpProjected.y + qxpLiteral.y);\n"
    "    textureSource = qxpTexcoord(\n"
    "        gl_MultiTexCoord2, qxpTexgen.z, eye, normal);\n"
    "    textureValue = gl_TextureMatrix[2] * textureSource;\n"
    "    gl_TexCoord[2] = vec4(textureValue.xyz,\n"
    "        textureValue.w * qxpProjected.z + qxpLiteral.z);\n"
    "    textureSource = qxpTexcoord(\n"
    "        gl_MultiTexCoord3, qxpTexgen.w, eye, normal);\n"
    "    textureValue = gl_TextureMatrix[3] * textureSource;\n"
    "    gl_TexCoord[3] = vec4(textureValue.xyz,\n"
    "        textureValue.w * qxpProjected.w + qxpLiteral.w);\n"
    "    float radialSquared = dot(eye, eye);\n"
    "    float radialFog = radialSquared *\n"
    "        inversesqrt(max(radialSquared, 1.0e-20));\n"
    "    float vertexFog = mix(abs(eye.z), radialFog, qxpFog.x);\n"
    "    gl_FogFragCoord = mix(vertexFog, gl_FogCoord, qxpFog.y);\n"
    "    gl_ClipVertex = eye4;\n"
    "    float pointDenominator = max(\n"
    "        gl_Point.distanceConstantAttenuation +\n"
    "        gl_Point.distanceLinearAttenuation * radialFog +\n"
    "        gl_Point.distanceQuadraticAttenuation * radialSquared,\n"
    "        1.0e-20);\n"
    "    gl_PointSize = clamp(gl_Point.size *\n"
    "        inversesqrt(pointDenominator), gl_Point.sizeMin,\n"
    "        gl_Point.sizeMax);\n"
    "    gl_Position = ftransform();\n"
    "}\n";

/* A fixed-function texture combiner cannot address secondary-color alpha.
 * This complete GLSL pair handles the stock SWVP form emitted after
 * FOGINFVF: stage zero selects diffuse RGB and independent specular alpha,
 * color sum adds specular RGB, and fog clamps only after interpolation. */
static const UCHAR qxp_d3d_specular_alpha_vertex_program[] =
    "#version 110\n"
    "void main(void)\n"
    "{\n"
    "    vec4 position = gl_Vertex;\n"
    "    position.w = 1.0;\n"
    "    gl_FrontColor = gl_Color;\n"
    "    gl_BackColor = gl_Color;\n"
    "    gl_FrontSecondaryColor = gl_SecondaryColor;\n"
    "    gl_BackSecondaryColor = gl_SecondaryColor;\n"
    "    gl_FogFragCoord = gl_FogCoord;\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * position;\n"
    "}\n";

static const UCHAR qxp_d3d_specular_alpha_fragment_program[] =
    "#version 110\n"
    "void main(void)\n"
    "{\n"
    "    vec4 color = vec4(gl_Color.rgb + gl_SecondaryColor.rgb,\n"
    "                      gl_SecondaryColor.a);\n"
    "    float fog = clamp((gl_Fog.end - gl_FogFragCoord) *\n"
    "                      gl_Fog.scale, 0.0, 1.0);\n"
    "    color.rgb = mix(gl_Fog.color.rgb, color.rgb, fog);\n"
    "    gl_FragColor = color;\n"
    "}\n";

static BOOLEAN qxp_d3d_ensure_texture_q_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_texture_q_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_TEXTURE_Q_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_texture_q_program,
            sizeof(qxp_d3d_texture_q_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_texture_q_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP TEXQ DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_compact_transformed_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG provoking_arguments[1];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_compact_transformed_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_COMPACT_TRANSFORMED_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_compact_transformed_program,
            sizeof(qxp_d3d_compact_transformed_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    provoking_arguments[0] = QXP_GL_FIRST_VERTEX_CONVENTION;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PROVOKING_VERTEX,
                             provoking_arguments, 1)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_compact_transformed_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP COMPACT DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_positional_range_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_positional_range_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_POSITIONAL_RANGE_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_positional_range_program,
            sizeof(qxp_d3d_positional_range_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_positional_range_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP POSITIONAL DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_multi_point_range_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_multi_point_range_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_MULTI_POINT_RANGE_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_multi_point_range_program,
            sizeof(qxp_d3d_multi_point_range_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_multi_point_range_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP MULTI POINT DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_dual_point_specular_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_dual_point_specular_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_dual_point_specular_program,
            sizeof(qxp_d3d_dual_point_specular_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_dual_point_specular_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP DUAL POINT SPECULAR DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_quad_point_range_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG restore_arguments[2];
    volatile ULONG *fifo;
    ULONG fifo_start;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_quad_point_range_program_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    bind_arguments[1] = QXP_D3D_QUAD_POINT_RANGE_PROGRAM;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             bind_arguments, 2) ||
        !qxp_d3d_call_program_string(
            device, QXP_GL_VERTEX_PROGRAM_ARB,
            qxp_d3d_quad_point_range_program,
            sizeof(qxp_d3d_quad_point_range_program) - 1)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    restore_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    restore_arguments[1] = device->d3d_vertex_program;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                             restore_arguments, 2)) return FALSE;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, 0xfc0, QXP_FENUM_GL_FLUSH);
    device->d3d_quad_point_range_program_defined = TRUE;
    qxp_debug("QXPMP D3D VP QUAD POINT DEFINE+");
    return TRUE;
}

static BOOLEAN qxp_d3d_ensure_specular_alpha_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG arguments[2];
    ULONG vertex_shader = 0;
    ULONG fragment_shader = 0;
    ULONG program = 0;
    ULONG result = 0;
    LONG status = 0;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_specular_alpha_program) return TRUE;
    if (device->d3d_specular_alpha_program_failed) return FALSE;
    device->d3d_specular_alpha_program_failed = TRUE;

    arguments[0] = QXP_GL_VERTEX_SHADER;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    vertex_shader = result;
    if (!qxp_d3d_call_shader_source(
            device, vertex_shader,
            qxp_d3d_specular_alpha_vertex_program,
            sizeof(qxp_d3d_specular_alpha_vertex_program))) goto fail;
    arguments[0] = vertex_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, vertex_shader,
            QXP_GL_COMPILE_STATUS, &status) || !status) {
        qxp_debug("QXPMP D3D SPECALPHA VS-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, vertex_shader);
        goto fail;
    }

    arguments[0] = QXP_GL_FRAGMENT_SHADER;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    fragment_shader = result;
    if (!qxp_d3d_call_shader_source(
            device, fragment_shader,
            qxp_d3d_specular_alpha_fragment_program,
            sizeof(qxp_d3d_specular_alpha_fragment_program))) goto fail;
    arguments[0] = fragment_shader;
    status = 0;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, fragment_shader,
            QXP_GL_COMPILE_STATUS, &status) || !status) {
        qxp_debug("QXPMP D3D SPECALPHA PS-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, fragment_shader);
        goto fail;
    }

    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_PROGRAM,
            NULL, 0, NULL, 0, &result) || !result) goto fail;
    program = result;
    arguments[0] = program;
    arguments[1] = vertex_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    arguments[1] = fragment_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    arguments[0] = program;
    status = 0;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_LINK_PROGRAM,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_PROGRAM_IV, program,
            QXP_GL_LINK_STATUS, &status) || !status) {
        qxp_debug("QXPMP D3D SPECALPHA LINK-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_PROGRAM_INFO_LOG, program);
        goto fail;
    }

    arguments[0] = vertex_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_DELETE_SHADER,
            arguments, 1, NULL, 0)) goto fail;
    vertex_shader = 0;
    arguments[0] = fragment_shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_DELETE_SHADER,
            arguments, 1, NULL, 0)) goto fail;
    fragment_shader = 0;
    device->d3d_specular_alpha_program = program;
    device->d3d_specular_alpha_program_failed = FALSE;
    qxp_debug("QXPMP D3D SPECALPHA DEFINE+");
    return TRUE;

fail:
    if (program) {
        arguments[0] = program;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                          arguments, 1, NULL, 0);
    }
    if (fragment_shader) {
        arguments[0] = fragment_shader;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_SHADER,
                          arguments, 1, NULL, 0);
    }
    if (vertex_shader) {
        arguments[0] = vertex_shader;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_SHADER,
                          arguments, 1, NULL, 0);
    }
    qxp_debug("QXPMP D3D SPECALPHA DEFINE-");
    return FALSE;
}

static BOOLEAN qxp_d3d_ensure_scalable_point_program(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG arguments[2];
    ULONG shader = 0;
    ULONG program = 0;
    ULONG result = 0;
    LONG status = 0;

    if (!device || !device->d3d_active) return FALSE;
    if (device->d3d_glsl_program) return TRUE;
    if (device->d3d_glsl_program_failed) return FALSE;
    device->d3d_glsl_program_failed = TRUE;

    arguments[0] = QXP_GL_VERTEX_SHADER;
    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_SHADER,
            arguments, 1, NULL, 0, &result) || !result) goto fail;
    shader = result;
    if (!qxp_d3d_call_shader_source(
            device, shader, qxp_d3d_scalable_point_program,
            sizeof(qxp_d3d_scalable_point_program))) goto fail;
    arguments[0] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_COMPILE_SHADER,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_SHADER_IV, shader,
            QXP_GL_COMPILE_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D GLSL COMPILE-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_SHADER_INFO_LOG, shader);
        goto fail;
    }

    if (!qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CREATE_PROGRAM,
            NULL, 0, NULL, 0, &result) || !result) goto fail;
    program = result;
    arguments[0] = program;
    arguments[1] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_ATTACH_SHADER,
            arguments, 2, NULL, 0)) goto fail;
    arguments[0] = program;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_LINK_PROGRAM,
            arguments, 1, NULL, 0) ||
        !qxp_d3d_gl_query_int(
            device, QXP_FENUM_GL_GET_PROGRAM_IV, program,
            QXP_GL_LINK_STATUS, &status)) goto fail;
    if (!status) {
        qxp_debug("QXPMP D3D GLSL LINK-");
        qxp_d3d_gl_debug_log(
            device, QXP_FENUM_GL_GET_PROGRAM_INFO_LOG, program);
        goto fail;
    }

    if (!qxp_d3d_gl_uniform_location(
            device, program, "qxpLightCount", sizeof("qxpLightCount"),
            &device->d3d_glsl_uniforms[
                QXP_D3D_GLSL_UNIFORM_LIGHT_COUNT]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpRange0", sizeof("qxpRange0"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_RANGE0]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpRange1", sizeof("qxpRange1"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_RANGE1]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpProjected", sizeof("qxpProjected"),
            &device->d3d_glsl_uniforms[
                QXP_D3D_GLSL_UNIFORM_PROJECTED]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpLiteral", sizeof("qxpLiteral"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_LITERAL]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpSource", sizeof("qxpSource"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_SOURCE]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpSecondary", sizeof("qxpSecondary"),
            &device->d3d_glsl_uniforms[
                QXP_D3D_GLSL_UNIFORM_SECONDARY]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpNormalize", sizeof("qxpNormalize"),
            &device->d3d_glsl_uniforms[
                QXP_D3D_GLSL_UNIFORM_NORMALIZE]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpMaterialSpecular",
            sizeof("qxpMaterialSpecular"),
            &device->d3d_glsl_uniforms[
                QXP_D3D_GLSL_UNIFORM_MATERIAL_SPECULAR]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpFog", sizeof("qxpFog"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_FOG]) ||
        !qxp_d3d_gl_uniform_location(
            device, program, "qxpTexgen", sizeof("qxpTexgen"),
            &device->d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_TEXGEN])) {
        qxp_debug("QXPMP D3D GLSL UNIFORM-");
        goto fail;
    }

    arguments[0] = shader;
    if (!qxp_d3d_call_fbtm(
            device, QXP_FENUM_GL_DELETE_SHADER,
            arguments, 1, NULL, 0)) goto fail;
    shader = 0;
    device->d3d_glsl_program = program;
    device->d3d_glsl_program_failed = FALSE;
    qxp_debug("QXPMP D3D VP SCALABLE DEFINE+");
    return TRUE;

fail:
    if (program) {
        arguments[0] = program;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_PROGRAM,
                          arguments, 1, NULL, 0);
    }
    if (shader) {
        arguments[0] = shader;
        qxp_d3d_call_fbtm(device, QXP_FENUM_GL_DELETE_SHADER,
                          arguments, 1, NULL, 0);
    }
    qxp_debug("QXPMP D3D VP SCALABLE DEFINE-");
    return FALSE;
}

static BOOLEAN qxp_d3d_submit_glsl_uniform4(
    QXP_DEVICE_EXTENSION *device, ULONG uniform_index,
    const ULONG values[4])
{
    ULONG arguments[3];
    LONG location;

    if (!device || !values || uniform_index >= QXP_D3D_GLSL_UNIFORM_COUNT)
        return FALSE;
    location = device->d3d_glsl_uniforms[uniform_index];
    if (location < 0) return FALSE;
    arguments[0] = (ULONG)location;
    arguments[1] = 1;
    arguments[2] = 0;
    return qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_UNIFORM_4FV,
        arguments, 3, values, 4 * sizeof(ULONG));
}

static BOOLEAN qxp_d3d_apply_scalable_point_uniforms(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state,
    const QXP_D3D_TEXCOORD_PLAN *texcoord_plan,
    const ULONG projected[4], const ULONG literal[4])
{
    ULONG count_arguments[2];
    ULONG range0[4] = {0, 0, 0, 0};
    ULONG range1[4] = {0, 0, 0, 0};
    ULONG source[4] = {0, 0, 0, 0};
    ULONG secondary[4] = {0, 0, 0, 0};
    ULONG normalize[4] = {0, 0, 0, 0};
    ULONG fog[4] = {0, 0, 0, 0};
    ULONG texgen[4] = {0, 0, 0, 0};
    ULONG material_specular[4];
    ULONG index;
    BOOLEAN color1_available;
    BOOLEAN color2_available;

    if (!device || !request || !state || !texcoord_plan ||
        !projected || !literal || !device->d3d_glsl_program ||
        device->d3d_glsl_uniforms[
            QXP_D3D_GLSL_UNIFORM_LIGHT_COUNT] < 0 ||
        state->active_light_count > QXP_D3D_MAX_ACTIVE_LIGHTS)
        return FALSE;
    for (index = 0; index < 4; ++index) {
        if (!(texcoord_plan->generated_stages & (1UL << index)))
            continue;
        if (texcoord_plan->modes[index] ==
                QXP_D3D_TCI_CAMERASPACENORMAL) {
            texgen[index] = 0x3f800000UL;
        } else if (texcoord_plan->modes[index] ==
                       QXP_D3D_TCI_CAMERASPACEPOSITION) {
            texgen[index] = 0x40000000UL;
        } else if (texcoord_plan->modes[index] ==
                       QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR) {
            texgen[index] = 0x40400000UL;
        } else {
            return FALSE;
        }
    }
    for (index = 0; index < state->active_light_count; ++index) {
        if (index < 4)
            range0[index] = state->active_lights[index].light.range_bits;
        else
            range1[index - 4] =
                state->active_lights[index].light.range_bits;
    }
    color1_available = state->color_vertex_enable &&
        (request->fvf & QXP_D3DFVF_DIFFUSE) != 0;
    source[0] = color1_available &&
        state->diffuse_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source[1] = color1_available &&
        state->ambient_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source[2] = color1_available &&
        state->emissive_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source[3] = color1_available && request->specular_enable &&
        state->specular_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    color2_available = state->color_vertex_enable &&
        (request->fvf & QXP_D3DFVF_SPECULAR) != 0;
    secondary[0] = color2_available &&
        state->diffuse_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary[1] = color2_available &&
        state->ambient_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary[2] = color2_available &&
        state->emissive_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary[3] = color2_available && request->specular_enable &&
        state->specular_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    normalize[0] = state->normalize_normals ? 0x3f800000UL : 0;
    normalize[1] = state->local_viewer ? 0x3f800000UL : 0;
    normalize[2] = request->specular_enable ? 0x3f800000UL : 0;
    normalize[3] = state->lighting_enable ? 0x3f800000UL : 0;
    fog[0] = state->range_fog_enable ? 0x3f800000UL : 0;
    fog[1] = request->fog_enable &&
        (request->fog_table_mode ||
         (request->fvf & QXP_D3DFVF_FOG)) ?
        0x3f800000UL : 0;
    /* A fixed declaration without NORMAL is legal. D3D supplies a zero
     * normal, producing no diffuse/specular light instead of rejecting the
     * draw. Compatibility GL has a nonzero current normal, so the internal
     * shader must explicitly mask it. */
    fog[2] = (request->fvf & QXP_D3DFVF_NORMAL) ?
        0x3f800000UL : 0;
    fog[3] = 0;
    material_specular[0] = state->material.specular.component[0];
    material_specular[1] = state->material.specular.component[1];
    material_specular[2] = state->material.specular.component[2];
    material_specular[3] = state->material.power_bits;
    count_arguments[0] = (ULONG)device->d3d_glsl_uniforms[
        QXP_D3D_GLSL_UNIFORM_LIGHT_COUNT];
    count_arguments[1] = state->lighting_enable ?
        state->active_light_count : 0;
    return qxp_d3d_fifo_submit(
               device, QXP_FENUM_GL_UNIFORM_1I,
               count_arguments, 2) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_RANGE0, range0) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_RANGE1, range1) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_PROJECTED, projected) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_LITERAL, literal) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_SOURCE, source) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_SECONDARY, secondary) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_NORMALIZE, normalize) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_MATERIAL_SPECULAR,
               material_specular) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_FOG, fog) &&
           qxp_d3d_submit_glsl_uniform4(
               device, QXP_D3D_GLSL_UNIFORM_TEXGEN, texgen);
}

static BOOLEAN qxp_d3d_disable_clip_planes(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG arguments[1];
    ULONG plane;

    if (!device) return FALSE;
    for (plane = 0; plane < QXP_D3D_MAX_USER_CLIP_PLANES; ++plane) {
        arguments[0] = QXP_GL_CLIP_PLANE0 + plane;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_DISABLE,
                arguments, 1)) return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_apply_program_clip_planes(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program)
{
    ULONG matrix_arguments[1];
    ULONG clip_arguments[2];
    ULONG clip_equation[8];
    ULONG enable_arguments[1];
    ULONG plane;
    ULONG component;

    if (!device || !program) return FALSE;
    matrix_arguments[0] = QXP_GL_MODELVIEW;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_MATRIX_MODE,
            matrix_arguments, 1) ||
        !qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_LOAD_IDENTITY,
            NULL, 0)) return FALSE;
    for (plane = 0; plane < QXP_D3D_MAX_USER_CLIP_PLANES; ++plane) {
        for (component = 0; component < 4; ++component) {
            if (!qxp_double_words_from_float(
                    program->clip_planes[plane][component],
                    &clip_equation[component * 2])) return FALSE;
        }
        clip_arguments[0] = QXP_GL_CLIP_PLANE0 + plane;
        clip_arguments[1] = 0;
        enable_arguments[0] = clip_arguments[0];
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_CLIP_PLANE,
                clip_arguments, 2, clip_equation,
                sizeof(clip_equation)) ||
            !qxp_d3d_fifo_submit(
                device,
                program->clip_plane_enable & (1UL << plane) ?
                    QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
                enable_arguments, 1)) return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_apply_program_vertex_program(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program)
{
    ULONG bind_arguments[2];
    ULONG parameter_arguments[3];
    ULONG state_arguments[1];
    ULONG point_size_arguments[1];
    ULONG orientation[4];
    ULONG projected[4];
    QXP_D3D_VERTEX_CLIP_STATE *clip = NULL;
    ULONG constant;
    BOOLEAN clip_active;
    BOOLEAN point_size_mode;

    if (!device || !request || !program || !program->program_name)
        return FALSE;
    state_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    point_size_arguments[0] = QXP_GL_VERTEX_PROGRAM_POINT_SIZE_ARB;
    clip_active = program->clipping_enable && program->clip_plane_enable;
    point_size_mode = request->primitive_type == 1 &&
        (program->written_outputs & QXP_D3D_VS_OUTPUT_POINT_SIZE) != 0;
    if (clip_active) {
        clip = qxp_d3d_find_vertex_clip(device, program->program_name);
        if (!clip) return FALSE;
        if (!device->d3d_vertex_program_clip &&
            device->d3d_vertex_program &&
            device->d3d_vertex_program != QXP_D3D_SCALABLE_POINT_PROGRAM) {
            bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
            bind_arguments[1] = 0;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE,
                    state_arguments, 1) ||
                !qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                    bind_arguments, 2)) return FALSE;
        }
        if (!device->d3d_vertex_program_clip ||
            device->d3d_vertex_program != program->program_name) {
            bind_arguments[0] = clip->gl_program;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_USE_PROGRAM,
                    bind_arguments, 1)) return FALSE;
        }
        device->d3d_vertex_program = program->program_name;
        device->d3d_vertex_program_clip = TRUE;
    } else {
        if (device->d3d_vertex_program_clip) {
            bind_arguments[0] = 0;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_USE_PROGRAM,
                    bind_arguments, 1) ||
                !qxp_d3d_disable_clip_planes(device)) return FALSE;
            device->d3d_vertex_program = 0;
            device->d3d_vertex_program_clip = FALSE;
        } else if (device->d3d_vertex_program ==
                       QXP_D3D_SCALABLE_POINT_PROGRAM) {
            bind_arguments[0] = 0;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_USE_PROGRAM,
                                     bind_arguments, 1)) return FALSE;
            device->d3d_vertex_program = 0;
        }
    }
    if (!clip_active &&
        device->d3d_vertex_program != program->program_name) {
        bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
        bind_arguments[1] = program->program_name;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                                 bind_arguments, 2) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                 state_arguments, 1)) return FALSE;
        device->d3d_vertex_program = program->program_name;
        device->d3d_vertex_program_clip = FALSE;
    }
    if (device->d3d_vertex_program_point_size != point_size_mode) {
        if (!qxp_d3d_fifo_submit(
                device, point_size_mode ? QXP_FENUM_GL_ENABLE :
                                          QXP_FENUM_GL_DISABLE,
                point_size_arguments, 1)) return FALSE;
        device->d3d_vertex_program_point_size = point_size_mode;
    }
    orientation[0] = device->d3d_target_texture &&
                     !device->d3d_target_present ?
                     0xbf800000UL : 0x3f800000UL;
    orientation[1] = request->fog_enable && request->fog_table_mode ?
                     0x3f800000UL : 0;
    orientation[2] = 0;
    orientation[3] = 0;
    /* The programmable vs epilogue selects D3D projected q per stage from
     * program.local[1] (ARB) / qxpProjected (clip GLSL). */
    projected[0] = (request->texture_projected_stages & 1UL) ?
                   0x3f800000UL : 0;
    projected[1] = (request->texture_projected_stages & 2UL) ?
                   0x3f800000UL : 0;
    projected[2] = (request->texture_projected_stages & 4UL) ?
                   0x3f800000UL : 0;
    projected[3] = (request->texture_projected_stages & 8UL) ?
                   0x3f800000UL : 0;
    if (clip_active) {
        if (clip->constants_location >= 0) {
            parameter_arguments[0] = (ULONG)clip->constants_location;
            parameter_arguments[1] = QXP_D3D_VERTEX_CONSTANT_COUNT;
            parameter_arguments[2] = 0;
            if (!qxp_d3d_fifo_submit_input(
                    device, QXP_FENUM_GL_UNIFORM_4FV,
                    parameter_arguments, 3, program->constants,
                    sizeof(program->constants))) return FALSE;
        }
        parameter_arguments[0] = (ULONG)clip->control_location;
        parameter_arguments[1] = 1;
        parameter_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                parameter_arguments, 3, orientation,
                sizeof(orientation))) return FALSE;
        if (clip->projected_location >= 0) {
            parameter_arguments[0] = (ULONG)clip->projected_location;
            parameter_arguments[1] = 1;
            parameter_arguments[2] = 0;
            if (!qxp_d3d_fifo_submit_input(
                    device, QXP_FENUM_GL_UNIFORM_4FV,
                    parameter_arguments, 3, projected,
                    sizeof(projected))) return FALSE;
        }
        return qxp_d3d_apply_program_clip_planes(device, program);
    }
    parameter_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    parameter_arguments[2] = 0;
    for (constant = 0; constant < QXP_D3D_VERTEX_CONSTANT_COUNT;
         ++constant) {
        parameter_arguments[1] = constant;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_PROGRAM_ENV_PARAMETER_4FV_ARB,
                parameter_arguments, 3, program->constants[constant],
                sizeof(program->constants[constant]))) return FALSE;
    }
    parameter_arguments[1] = 0;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, orientation, sizeof(orientation)))
        return FALSE;
    parameter_arguments[1] = 1;
    return qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
        parameter_arguments, 3, projected, sizeof(projected));
}

static BOOLEAN qxp_d3d_apply_vertex_program(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state,
    const QXP_D3D_TEXCOORD_PLAN *texcoord_plan, ULONG program)
{
    ULONG bind_arguments[2];
    ULONG parameter_arguments[3];
    ULONG projected[4];
    ULONG literal[4];
    ULONG range_fog[4];
    ULONG table_fog[4];
    ULONG light_range[4];
    ULONG spot_control[4];
    ULONG source_control[4];
    ULONG secondary_source_control[4];
    ULONG viewer_control[4];
    ULONG normalize_control[4];
    ULONG light_control[4];
    ULONG state_arguments[1];
    ULONG point_size_arguments[1];
    ULONG stage;
    BOOLEAN changed;
    BOOLEAN color1_available;
    BOOLEAN color2_available;
    BOOLEAN point_size_mode;

    if (!device || !request || !texcoord_plan ||
        (program && !state &&
         program != QXP_D3D_COMPACT_TRANSFORMED_PROGRAM &&
         program != QXP_D3D_SPECULAR_ALPHA_PROGRAM) ||
        (program != 0 && program != QXP_D3D_TEXTURE_Q_PROGRAM &&
         program != QXP_D3D_POSITIONAL_RANGE_PROGRAM &&
         program != QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM &&
         program != QXP_D3D_QUAD_POINT_RANGE_PROGRAM &&
         program != QXP_D3D_MULTI_POINT_RANGE_PROGRAM &&
         program != QXP_D3D_SCALABLE_POINT_PROGRAM &&
         program != QXP_D3D_COMPACT_TRANSFORMED_PROGRAM &&
         program != QXP_D3D_SPECULAR_ALPHA_PROGRAM)) return FALSE;
    state_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    point_size_arguments[0] = QXP_GL_VERTEX_PROGRAM_POINT_SIZE_ARB;
    point_size_mode = program &&
                      program != QXP_D3D_COMPACT_TRANSFORMED_PROGRAM &&
                      request->point_scale_enable &&
                      request->primitive_type == 1;
    if (!program) {
        if (!device->d3d_vertex_program &&
            !device->d3d_vertex_program_point_size &&
            !device->d3d_vertex_program_clip) return TRUE;
        if (device->d3d_vertex_program_point_size &&
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 point_size_arguments, 1)) return FALSE;
        if (device->d3d_vertex_program_clip) {
            bind_arguments[0] = 0;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_USE_PROGRAM,
                                     bind_arguments, 1) ||
                !qxp_d3d_disable_clip_planes(device)) return FALSE;
        } else if (device->d3d_vertex_program ==
                       QXP_D3D_SCALABLE_POINT_PROGRAM ||
                   device->d3d_vertex_program ==
                       QXP_D3D_SPECULAR_ALPHA_PROGRAM) {
            bind_arguments[0] = 0;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_USE_PROGRAM,
                                     bind_arguments, 1)) return FALSE;
        } else if (device->d3d_vertex_program) {
            bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
            bind_arguments[1] = 0;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                     state_arguments, 1) ||
                !qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                    bind_arguments, 2)) return FALSE;
        }
        device->d3d_vertex_program = 0;
        device->d3d_vertex_program_point_size = FALSE;
        device->d3d_vertex_program_clip = FALSE;
        return TRUE;
    }
    if ((program == QXP_D3D_TEXTURE_Q_PROGRAM &&
         !device->d3d_texture_q_program_defined) ||
        (program == QXP_D3D_POSITIONAL_RANGE_PROGRAM &&
         (!device->d3d_positional_range_program_defined ||
          !state->active_light_count)) ||
        (program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM &&
         (!device->d3d_dual_point_specular_program_defined ||
          state->active_light_count != 2)) ||
        (program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM &&
         (!device->d3d_quad_point_range_program_defined ||
          state->active_light_count != 4)) ||
        (program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM &&
         (!device->d3d_multi_point_range_program_defined ||
          state->active_light_count < 2 ||
          state->active_light_count > 3)) ||
        (program == QXP_D3D_SCALABLE_POINT_PROGRAM &&
         (!device->d3d_glsl_program ||
          state->active_light_count > QXP_D3D_MAX_ACTIVE_LIGHTS)) ||
        (program == QXP_D3D_SPECULAR_ALPHA_PROGRAM &&
         !device->d3d_specular_alpha_program) ||
        (program == QXP_D3D_COMPACT_TRANSFORMED_PROGRAM &&
         !device->d3d_compact_transformed_program_defined))
        return FALSE;
    changed = device->d3d_vertex_program != program ||
              device->d3d_vertex_program_clip;
    if (changed) {
        if (program == QXP_D3D_SCALABLE_POINT_PROGRAM ||
            program == QXP_D3D_SPECULAR_ALPHA_PROGRAM) {
            if (device->d3d_vertex_program_clip) {
                if (!qxp_d3d_disable_clip_planes(device)) return FALSE;
            } else if (device->d3d_vertex_program &&
                       device->d3d_vertex_program !=
                           QXP_D3D_SCALABLE_POINT_PROGRAM &&
                       device->d3d_vertex_program !=
                           QXP_D3D_SPECULAR_ALPHA_PROGRAM) {
                bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
                bind_arguments[1] = 0;
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_DISABLE,
                        state_arguments, 1) ||
                    !qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                        bind_arguments, 2)) return FALSE;
            }
            bind_arguments[0] =
                program == QXP_D3D_SCALABLE_POINT_PROGRAM ?
                device->d3d_glsl_program :
                device->d3d_specular_alpha_program;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_USE_PROGRAM,
                    bind_arguments, 1)) return FALSE;
        } else {
            if (device->d3d_vertex_program_clip) {
                bind_arguments[0] = 0;
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_USE_PROGRAM,
                        bind_arguments, 1) ||
                    !qxp_d3d_disable_clip_planes(device)) return FALSE;
            } else if (device->d3d_vertex_program ==
                           QXP_D3D_SCALABLE_POINT_PROGRAM ||
                       device->d3d_vertex_program ==
                           QXP_D3D_SPECULAR_ALPHA_PROGRAM) {
                bind_arguments[0] = 0;
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_USE_PROGRAM,
                        bind_arguments, 1)) return FALSE;
            }
            bind_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
            bind_arguments[1] = program;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_BIND_PROGRAM_ARB,
                    bind_arguments, 2) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                     state_arguments, 1)) return FALSE;
        }
        device->d3d_vertex_program = program;
        device->d3d_vertex_program_clip = FALSE;
    }
    if (device->d3d_vertex_program_point_size != point_size_mode) {
        if (!qxp_d3d_fifo_submit(
                device, point_size_mode ? QXP_FENUM_GL_ENABLE :
                                          QXP_FENUM_GL_DISABLE,
                point_size_arguments, 1)) return FALSE;
        device->d3d_vertex_program_point_size = point_size_mode;
    }
    if (program == QXP_D3D_SPECULAR_ALPHA_PROGRAM) return TRUE;
    if (program == QXP_D3D_COMPACT_TRANSFORMED_PROGRAM) {
        ULONG compact_fog[4];

        parameter_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
        parameter_arguments[1] = 0;
        parameter_arguments[2] = 0;
        compact_fog[0] = request->fog_enable && request->fog_table_mode ?
                         0x3f800000UL : 0;
        compact_fog[1] = 0;
        compact_fog[2] = 0;
        compact_fog[3] = 0;
        return qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, compact_fog, sizeof(compact_fog));
    }
    if (program != QXP_D3D_SCALABLE_POINT_PROGRAM &&
        !changed && device->d3d_draw_state_valid &&
        device->d3d_last_hwtl_valid &&
        device->d3d_last_draw.fvf == request->fvf &&
        device->d3d_last_draw.texture_projected_stages ==
            request->texture_projected_stages &&
        device->d3d_last_draw.fog_enable == request->fog_enable &&
        device->d3d_last_draw.fog_table_mode == request->fog_table_mode &&
        device->d3d_last_hwtl_state.range_fog_enable ==
            state->range_fog_enable &&
        (program != QXP_D3D_POSITIONAL_RANGE_PROGRAM ||
         (device->d3d_last_hwtl_state.active_light_count &&
          device->d3d_last_hwtl_state.active_lights[0].light.type ==
              state->active_lights[0].light.type &&
          device->d3d_last_hwtl_state.active_lights[0].light.range_bits ==
              state->active_lights[0].light.range_bits &&
           device->d3d_last_hwtl_state.active_lights[0].light.falloff_bits ==
               state->active_lights[0].light.falloff_bits &&
           device->d3d_last_hwtl_state.color_vertex_enable ==
               state->color_vertex_enable &&
           device->d3d_last_draw.specular_enable ==
               request->specular_enable &&
           device->d3d_last_hwtl_state.local_viewer ==
               state->local_viewer &&
           device->d3d_last_hwtl_state.diffuse_material_source ==
               state->diffuse_material_source &&
           device->d3d_last_hwtl_state.specular_material_source ==
               state->specular_material_source &&
           device->d3d_last_hwtl_state.ambient_material_source ==
               state->ambient_material_source &&
           device->d3d_last_hwtl_state.emissive_material_source ==
               state->emissive_material_source)) &&
        (program != QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM ||
         (device->d3d_last_hwtl_state.active_light_count == 2 &&
          device->d3d_last_hwtl_state.active_lights[0].light.range_bits ==
              state->active_lights[0].light.range_bits &&
          device->d3d_last_hwtl_state.active_lights[1].light.range_bits ==
              state->active_lights[1].light.range_bits &&
          device->d3d_last_hwtl_state.color_vertex_enable ==
              state->color_vertex_enable &&
          device->d3d_last_hwtl_state.normalize_normals ==
              state->normalize_normals &&
          device->d3d_last_hwtl_state.local_viewer ==
              state->local_viewer &&
          device->d3d_last_hwtl_state.diffuse_material_source ==
              state->diffuse_material_source &&
          device->d3d_last_hwtl_state.specular_material_source ==
              state->specular_material_source &&
          device->d3d_last_hwtl_state.ambient_material_source ==
              state->ambient_material_source &&
          device->d3d_last_hwtl_state.emissive_material_source ==
              state->emissive_material_source)) &&
        (program != QXP_D3D_QUAD_POINT_RANGE_PROGRAM ||
         (device->d3d_last_hwtl_state.active_light_count == 4 &&
          device->d3d_last_hwtl_state.active_lights[0].light.range_bits ==
              state->active_lights[0].light.range_bits &&
          device->d3d_last_hwtl_state.active_lights[1].light.range_bits ==
              state->active_lights[1].light.range_bits &&
          device->d3d_last_hwtl_state.active_lights[2].light.range_bits ==
              state->active_lights[2].light.range_bits &&
          device->d3d_last_hwtl_state.active_lights[3].light.range_bits ==
              state->active_lights[3].light.range_bits &&
          device->d3d_last_hwtl_state.color_vertex_enable ==
              state->color_vertex_enable &&
          device->d3d_last_hwtl_state.normalize_normals ==
              state->normalize_normals &&
          device->d3d_last_hwtl_state.diffuse_material_source ==
              state->diffuse_material_source &&
          device->d3d_last_hwtl_state.ambient_material_source ==
              state->ambient_material_source &&
          device->d3d_last_hwtl_state.emissive_material_source ==
              state->emissive_material_source)) &&
        (program != QXP_D3D_MULTI_POINT_RANGE_PROGRAM ||
         (device->d3d_last_hwtl_state.active_light_count ==
              state->active_light_count &&
          device->d3d_last_hwtl_state.active_lights[0].light.range_bits ==
              state->active_lights[0].light.range_bits &&
          device->d3d_last_hwtl_state.active_lights[1].light.range_bits ==
              state->active_lights[1].light.range_bits &&
          (state->active_light_count == 2 ||
           device->d3d_last_hwtl_state.active_lights[2].light.range_bits ==
               state->active_lights[2].light.range_bits) &&
          device->d3d_last_hwtl_state.color_vertex_enable ==
              state->color_vertex_enable &&
          device->d3d_last_hwtl_state.normalize_normals ==
              state->normalize_normals &&
          device->d3d_last_hwtl_state.diffuse_material_source ==
              state->diffuse_material_source &&
          device->d3d_last_hwtl_state.ambient_material_source ==
              state->ambient_material_source &&
          device->d3d_last_hwtl_state.emissive_material_source ==
              state->emissive_material_source))) return TRUE;
    for (stage = 0; stage < 4; ++stage) {
        projected[stage] =
            request->texture_projected_stages & (1UL << stage) ?
            0x3f800000UL : 0;
        literal[stage] = projected[stage] ? 0 : 0x3f800000UL;
    }
    if (program == QXP_D3D_SCALABLE_POINT_PROGRAM)
        return qxp_d3d_apply_scalable_point_uniforms(
            device, request, state, texcoord_plan, projected, literal);
    parameter_arguments[0] = QXP_GL_VERTEX_PROGRAM_ARB;
    parameter_arguments[1] = 0;
    parameter_arguments[2] = 0;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, projected, sizeof(projected)))
        return FALSE;
    parameter_arguments[1] = 1;
    if (!qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
        parameter_arguments, 3, literal, sizeof(literal))) return FALSE;
    range_fog[0] = state->range_fog_enable ? 0x3f800000UL : 0;
    range_fog[1] = range_fog[2] = range_fog[3] = 0;
    parameter_arguments[1] = 2;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, range_fog, sizeof(range_fog)))
        return FALSE;
    if (program == QXP_D3D_TEXTURE_Q_PROGRAM) {
        /* With the fog array disabled, OpenGL's current fog coordinate is
         * zero.  Multiplying the calculated vertex-fog coordinate by one
         * therefore selects it; multiplying by zero selects the enabled
         * table-fog array without needing a fragment-only CMP opcode. */
        table_fog[0] = request->fog_enable && request->fog_table_mode ?
            0 : 0x3f800000UL;
        table_fog[1] = table_fog[2] = table_fog[3] = 0;
        parameter_arguments[1] = 3;
        return qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, table_fog, sizeof(table_fog));
    }
    light_range[0] = state->active_lights[0].light.range_bits;
    light_range[1] =
        (program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM ||
         program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM ||
         program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM) ?
        state->active_lights[1].light.range_bits : 0;
    light_range[2] =
        ((program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM &&
          state->active_light_count == 3) ||
         program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM) ?
        state->active_lights[2].light.range_bits : 0;
    light_range[3] = program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM ?
        state->active_lights[3].light.range_bits : 0;
    parameter_arguments[1] = 3;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, light_range, sizeof(light_range)))
        return FALSE;
    if (program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM ||
        program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM ||
        program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM) {
        color1_available = state->color_vertex_enable &&
            (request->fvf & QXP_D3DFVF_DIFFUSE) != 0;
        source_control[0] = color1_available &&
            state->diffuse_material_source == QXP_D3DMCS_COLOR1 ?
            0x3f800000UL : 0;
        source_control[1] = color1_available &&
            state->ambient_material_source == QXP_D3DMCS_COLOR1 ?
            0x3f800000UL : 0;
        source_control[2] = color1_available &&
            state->emissive_material_source == QXP_D3DMCS_COLOR1 ?
            0x3f800000UL : 0;
        source_control[3] =
            program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM &&
            color1_available &&
            state->specular_material_source == QXP_D3DMCS_COLOR1 ?
            0x3f800000UL : 0;
        parameter_arguments[1] = 4;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
                parameter_arguments, 3, source_control,
                sizeof(source_control))) return FALSE;
        color2_available = state->color_vertex_enable &&
            (request->fvf & QXP_D3DFVF_SPECULAR) != 0;
        secondary_source_control[0] = color2_available &&
            state->diffuse_material_source == QXP_D3DMCS_COLOR2 ?
            0x3f800000UL : 0;
        secondary_source_control[1] = color2_available &&
            state->ambient_material_source == QXP_D3DMCS_COLOR2 ?
            0x3f800000UL : 0;
        secondary_source_control[2] = color2_available &&
            state->emissive_material_source == QXP_D3DMCS_COLOR2 ?
            0x3f800000UL : 0;
        secondary_source_control[3] =
            program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM &&
            color2_available &&
            state->specular_material_source == QXP_D3DMCS_COLOR2 ?
            0x3f800000UL : 0;
        parameter_arguments[1] = 5;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
                parameter_arguments, 3, secondary_source_control,
                sizeof(secondary_source_control))) return FALSE;
        normalize_control[0] = state->normalize_normals ?
            0x3f800000UL : 0;
        normalize_control[1] = normalize_control[2] =
            normalize_control[3] = 0;
        parameter_arguments[1] = 6;
        if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, normalize_control,
            sizeof(normalize_control))) return FALSE;
        if (program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM) return TRUE;
        if (program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM) {
            viewer_control[0] = state->local_viewer ? 0x3f800000UL : 0;
            viewer_control[1] = viewer_control[2] = viewer_control[3] = 0;
            parameter_arguments[1] = 7;
            return qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
                parameter_arguments, 3, viewer_control,
                sizeof(viewer_control));
        }
        light_control[0] = state->active_light_count == 3 ?
            0x3f800000UL : 0;
        light_control[1] = request->fog_enable && request->fog_table_mode ?
            0 : 0x3f800000UL;
        light_control[2] = light_control[3] = 0;
        parameter_arguments[1] = 7;
        return qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, light_control,
            sizeof(light_control));
    }
    spot_control[0] = state->active_lights[0].light.type ==
        QXP_D3DLIGHT_SPOT ? 0x3f800000UL : 0;
    spot_control[1] = state->active_lights[0].light.falloff_bits;
    spot_control[2] = spot_control[3] = 0;
    parameter_arguments[1] = 4;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, spot_control, sizeof(spot_control)))
        return FALSE;
    color1_available = state->color_vertex_enable &&
        (request->fvf & QXP_D3DFVF_DIFFUSE) != 0;
    source_control[0] = color1_available &&
        state->diffuse_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source_control[1] = color1_available &&
        state->ambient_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source_control[2] = color1_available &&
        state->emissive_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    source_control[3] = color1_available && request->specular_enable &&
        state->specular_material_source == QXP_D3DMCS_COLOR1 ?
        0x3f800000UL : 0;
    parameter_arguments[1] = 5;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, source_control,
            sizeof(source_control))) return FALSE;
    color2_available = state->color_vertex_enable &&
        (request->fvf & QXP_D3DFVF_SPECULAR) != 0;
    secondary_source_control[0] = color2_available &&
        state->diffuse_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary_source_control[1] = color2_available &&
        state->ambient_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary_source_control[2] = color2_available &&
        state->emissive_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    secondary_source_control[3] = color2_available &&
        request->specular_enable &&
        state->specular_material_source == QXP_D3DMCS_COLOR2 ?
        0x3f800000UL : 0;
    parameter_arguments[1] = 6;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
            parameter_arguments, 3, secondary_source_control,
            sizeof(secondary_source_control))) return FALSE;
    viewer_control[0] = state->local_viewer ? 0x3f800000UL : 0;
    viewer_control[1] = request->specular_enable ? 0x3f800000UL : 0;
    viewer_control[2] = request->fog_enable && request->fog_table_mode ?
        0 : 0x3f800000UL;
    viewer_control[3] = 0;
    parameter_arguments[1] = 7;
    return qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_PROGRAM_LOCAL_PARAMETER_4FV_ARB,
        parameter_arguments, 3, viewer_control, sizeof(viewer_control));
}

static BOOLEAN qxp_d3d_positional_range_program_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    const QXP_D3D_LIGHT *light;

    if (!request || !state || !state->lighting_enable ||
        request->shade_mode != 2 ||
        !(request->fvf & QXP_D3DFVF_NORMAL) ||
        !state->normalize_normals ||
        state->diffuse_material_source > QXP_D3DMCS_COLOR2 ||
        state->specular_material_source > QXP_D3DMCS_COLOR2 ||
        state->ambient_material_source > QXP_D3DMCS_COLOR2 ||
        state->emissive_material_source > QXP_D3DMCS_COLOR2 ||
        (state->material.power_bits & 0x80000000UL) ||
        state->material.power_bits > 0x43000000UL ||
        state->active_light_count != 1) return FALSE;
    light = &state->active_lights[0].light;
    if (light->type == QXP_D3DLIGHT_POINT) return TRUE;
    return light->type == QXP_D3DLIGHT_SPOT &&
           light->theta_bits <= light->phi_bits &&
           light->phi_bits <= 0x40490fdbUL;
}

static BOOLEAN qxp_d3d_multi_point_range_program_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    if (!request || !state || !state->lighting_enable ||
        request->shade_mode != 2 || request->primitive_type == 1 ||
        !(request->fvf & QXP_D3DFVF_NORMAL) ||
        request->specular_enable ||
        state->diffuse_material_source > QXP_D3DMCS_COLOR2 ||
        state->ambient_material_source > QXP_D3DMCS_COLOR2 ||
        state->emissive_material_source > QXP_D3DMCS_COLOR2 ||
        state->active_light_count < 2 ||
        state->active_light_count > 3) return FALSE;
    if (state->active_lights[0].light.type != QXP_D3DLIGHT_POINT ||
        state->active_lights[1].light.type != QXP_D3DLIGHT_POINT) return FALSE;
    return state->active_light_count == 2 ||
           state->active_lights[2].light.type == QXP_D3DLIGHT_POINT;
}

static BOOLEAN qxp_d3d_dual_point_specular_program_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    if (!request || !state || !state->lighting_enable ||
        request->shade_mode != 2 || request->primitive_type == 1 ||
        !(request->fvf & QXP_D3DFVF_NORMAL) ||
        !request->specular_enable || request->fog_enable ||
        state->diffuse_material_source > QXP_D3DMCS_COLOR2 ||
        state->specular_material_source > QXP_D3DMCS_COLOR2 ||
        state->ambient_material_source > QXP_D3DMCS_COLOR2 ||
        state->emissive_material_source > QXP_D3DMCS_COLOR2 ||
        (state->material.power_bits & 0x80000000UL) ||
        state->material.power_bits > 0x43000000UL ||
        state->active_light_count != 2) return FALSE;
    return state->active_lights[0].light.type == QXP_D3DLIGHT_POINT &&
           state->active_lights[1].light.type == QXP_D3DLIGHT_POINT;
}

static BOOLEAN qxp_d3d_quad_point_range_program_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    ULONG index;

    if (!request || !state || !state->lighting_enable ||
        request->shade_mode != 2 || request->primitive_type == 1 ||
        !(request->fvf & QXP_D3DFVF_NORMAL) ||
        request->specular_enable || request->fog_enable ||
        state->diffuse_material_source > QXP_D3DMCS_COLOR2 ||
        state->ambient_material_source > QXP_D3DMCS_COLOR2 ||
        state->emissive_material_source > QXP_D3DMCS_COLOR2 ||
        state->active_light_count != 4) return FALSE;
    for (index = 0; index < 4; ++index) {
        if (state->active_lights[index].light.type != QXP_D3DLIGHT_POINT)
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_scalable_point_program_supported(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state)
{
    ULONG index;
    BOOLEAN has_spot = FALSE;
    BOOLEAN has_directional = FALSE;
    BOOLEAN has_positional = FALSE;

    if (!request || !state || !state->lighting_enable ||
        request->shade_mode != 2 ||
        !(request->fvf & QXP_D3DFVF_NORMAL) ||
        state->diffuse_material_source > QXP_D3DMCS_COLOR2 ||
        state->specular_material_source > QXP_D3DMCS_COLOR2 ||
        state->ambient_material_source > QXP_D3DMCS_COLOR2 ||
        state->emissive_material_source > QXP_D3DMCS_COLOR2 ||
        (request->specular_enable &&
         ((state->material.power_bits & 0x80000000UL) ||
          state->material.power_bits > 0x43000000UL)) ||
        state->active_light_count < 2 ||
        state->active_light_count > QXP_D3D_MAX_ACTIVE_LIGHTS)
        return FALSE;
    for (index = 0; index < state->active_light_count; ++index) {
        const QXP_D3D_LIGHT *light = &state->active_lights[index].light;

        if (light->type == QXP_D3DLIGHT_POINT) {
            has_positional = TRUE;
            continue;
        }
        if (light->type == QXP_D3DLIGHT_DIRECTIONAL) {
            has_directional = TRUE;
            continue;
        }
        if (light->type != QXP_D3DLIGHT_SPOT ||
            light->theta_bits > light->phi_bits ||
            light->phi_bits > 0x40490fdbUL) return FALSE;
        has_positional = TRUE;
        has_spot = TRUE;
    }
    return has_spot || (has_directional && has_positional) ||
        (!has_directional && state->active_light_count >=
         (request->specular_enable ? 3UL : 5UL));
}

#ifdef QXP_VERBOSE_TRACE
static BOOLEAN qxp_d3d_trace_ordinal(ULONG ordinal);
#endif

static BOOLEAN qxp_d3d_clear_transport(QXP_DEVICE_EXTENSION *device,
                                       const QXP_D3D_CLEAR_REQUEST *request)
{
    ULONG color_arguments[4];
    ULONG color_mask_arguments[4];
    ULONG depth_arguments[2];
    ULONG stencil_arguments[1];
    ULONG state_arguments[1];
    ULONG clear_arguments[1];
    ULONG scissor_arguments[4];
    ULONG mask = 0;
    ULONG index;
    volatile ULONG *fifo;
    ULONG fifo_start;
    BOOLEAN result = FALSE;

    if (!device->d3d_active ||
        request->process_id != device->d3d_process_id) {
        return FALSE;
    }
    if (!request->flags || (request->flags & ~0x7UL) ||
        !request->target_width || !request->target_height ||
        request->target_width > 0xffffUL ||
        request->target_height > 0xffffUL ||
        !request->rectangle_count ||
        request->rectangle_count > QXP_D3D_MAX_CLEAR_RECTS) return FALSE;
    for (index = 0; index < request->rectangle_count; ++index) {
        const QXP_D3D_RECT *rectangle = &request->rectangles[index];

        if (rectangle->left < 0 || rectangle->top < 0 ||
            rectangle->right <= rectangle->left ||
            rectangle->bottom <= rectangle->top ||
            (ULONG)rectangle->right > request->target_width ||
            (ULONG)rectangle->bottom > request->target_height) return FALSE;
    }
#ifdef QXP_VERBOSE_TRACE
    if (qxp_d3d_trace_ordinal(device->d3d_clear_count + 1)) {
        qxp_debug("QXPMP D3D CLEAR TRACE");
        qxp_debug_status(device->d3d_clear_count + 1);
        qxp_debug_status(request->flags);
        qxp_debug_status(request->color);
        qxp_debug_status(request->depth_bits);
        qxp_debug_status(request->stencil);
        qxp_debug_status(request->rectangle_count);
        if (request->rectangle_count) {
            qxp_debug_status(request->rectangles[0].left);
            qxp_debug_status(request->rectangles[0].top);
            qxp_debug_status(request->rectangles[0].right);
            qxp_debug_status(request->rectangles[0].bottom);
        } else {
            qxp_debug_status(0);
            qxp_debug_status(0);
            qxp_debug_status(0);
            qxp_debug_status(0);
        }
    }
#endif
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];
    if (request->flags & 0x1UL) {
        color_arguments[0] = qxp_float_from_unorm8(
            (UCHAR)(request->color >> 16));
        color_arguments[1] = qxp_float_from_unorm8(
            (UCHAR)(request->color >> 8));
        color_arguments[2] = qxp_float_from_unorm8((UCHAR)request->color);
        color_arguments[3] = qxp_float_from_unorm8(
            (UCHAR)(request->color >> 24));
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_CLEAR_COLOR,
                                 color_arguments, 4)) goto done;
        /* D3D Clear ignores D3DRS_COLORWRITEENABLE. Each draw restores the
         * application's channel mask before rendering. */
        color_mask_arguments[0] = TRUE;
        color_mask_arguments[1] = TRUE;
        color_mask_arguments[2] = TRUE;
        color_mask_arguments[3] = TRUE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_COLOR_MASK,
                                 color_mask_arguments, 4)) goto done;
        mask |= QXP_GL_COLOR_BUFFER_BIT;
    }
    if (request->flags & 0x2UL) {
        ULONG exponent;
        ULONG mantissa;
        ULONGLONG double_bits;

        /* D3D supplies a positive float in [0,1], while glClearDepth takes
         * a double. Convert the IEEE-754 payload without using kernel FP.
         * Float subnormals are far below the precision of any advertised
         * depth buffer and are intentionally rounded to zero. */
        if (request->depth_bits & 0x80000000UL ||
            request->depth_bits > 0x3f800000UL) goto done;
        exponent = (request->depth_bits >> 23) & 0xffUL;
        mantissa = request->depth_bits & 0x007fffffUL;
        if (!exponent) {
            depth_arguments[0] = 0;
            depth_arguments[1] = 0;
        } else {
            double_bits = ((ULONGLONG)(exponent + 896UL) << 52) |
                          ((ULONGLONG)mantissa << 29);
            depth_arguments[0] = (ULONG)double_bits;
            depth_arguments[1] = (ULONG)(double_bits >> 32);
        }
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_CLEAR_DEPTH,
                                 depth_arguments, 2)) goto done;
        /* OpenGL masks depth clears when writes are disabled; D3D Clear does
         * not. Each draw restores the application's depth-write state. */
        state_arguments[0] = TRUE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_MASK,
                                 state_arguments, 1)) goto done;
        mask |= QXP_GL_DEPTH_BUFFER_BIT;
    }
    if (request->flags & 0x4UL) {
        stencil_arguments[0] = request->stencil;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_CLEAR_STENCIL,
                                 stencil_arguments, 1)) goto done;
        /* Like depth writes, the OpenGL stencil write mask affects Clear.
         * D3D Clear ignores the current render-state write mask. */
        state_arguments[0] = 0xffffffffUL;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_STENCIL_MASK,
                                 state_arguments, 1)) goto done;
        mask |= QXP_GL_STENCIL_BUFFER_BIT;
    }
    if (mask) {
        clear_arguments[0] = mask;
        state_arguments[0] = QXP_GL_SCISSOR_TEST;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                 state_arguments, 1)) goto done;
        for (index = 0; index < request->rectangle_count; ++index) {
            const QXP_D3D_RECT *rectangle = &request->rectangles[index];

            scissor_arguments[0] = (ULONG)rectangle->left;
            scissor_arguments[1] = device->d3d_target_texture &&
                                    !device->d3d_target_present ?
                (ULONG)rectangle->top : request->target_height -
                                        (ULONG)rectangle->bottom;
            scissor_arguments[2] = (ULONG)(rectangle->right -
                                            rectangle->left);
            scissor_arguments[3] = (ULONG)(rectangle->bottom -
                                            rectangle->top);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_SCISSOR,
                                     scissor_arguments, 4) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_CLEAR,
                                     clear_arguments, 1)) goto done;
        }
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) goto done;
    }
#ifdef QXP_VERBOSE_TRACE
    qxp_debug("QXPMP D3D CLEAR+");
#endif
    /* Clear temporarily overrides color, depth, and stencil write masks.
     * The next draw must restore the application's complete fixed state even
     * when its request matches the last draw submitted before this clear. */
    qxp_d3d_invalidate_draw_state(device);
    result = TRUE;

done:
    if (!result) fifo[0] = fifo_start;
    return result;
}

static void qxp_double_words_from_uint(ULONG value, PULONG words)
{
    ULONG leading;
    ULONG base;

    words[0] = 0;
    if (!value) {
        words[1] = 0;
        return;
    }
    leading = 0;
    while (leading < 20 && (1UL << (leading + 1)) <= value) ++leading;
    base = 1UL << leading;
    words[1] = ((1023UL + leading) << 20) |
               ((value - base) << (20 - leading));
}

static BOOLEAN qxp_double_words_from_float(ULONG value, PULONG words)
{
    ULONG sign;
    ULONG exponent;
    ULONG fraction;
    LONG unbiased;

    if (!words) return FALSE;
    sign = value & 0x80000000UL;
    exponent = (value >> 23) & 0xffUL;
    fraction = value & 0x007fffffUL;
    if (exponent == 0xffUL) return FALSE;
    if (!exponent && !fraction) {
        words[0] = 0;
        words[1] = sign;
        return TRUE;
    }
    if (!exponent) {
        unbiased = -126;
        while (!(fraction & 0x00800000UL)) {
            fraction <<= 1;
            --unbiased;
        }
        fraction &= 0x007fffffUL;
    } else {
        unbiased = (LONG)exponent - 127;
    }
    words[0] = fraction << 29;
    words[1] = sign | ((ULONG)(unbiased + 1023) << 20) |
               (fraction >> 3);
    return TRUE;
}

static BOOLEAN qxp_d3d_submit_matrix_f(
    QXP_DEVICE_EXTENSION *device, ULONG function,
    const QXP_D3D_MATRIX *matrix)
{
    ULONG arguments[1];

    if (!device || !matrix) return FALSE;
    arguments[0] = 0;
    return qxp_d3d_fifo_submit_input(
        device, function, arguments, 1, matrix, sizeof(*matrix));
}

static BOOLEAN qxp_d3d_apply_texcoord_generation(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_TEXCOORD_PLAN *plan)
{
    static const ULONG eye_planes[3][4] = {
        { 0x3f800000UL, 0, 0, 0 },
        { 0, 0x3f800000UL, 0, 0 },
        { 0, 0, 0x3f800000UL, 0 }
    };
    ULONG active_arguments[1];
    ULONG matrix_arguments[1];
    ULONG texgen_arguments[3];
    ULONG plane_arguments[3];
    ULONG state_arguments[1];
    ULONG coordinate;
    ULONG generation_mode;
    ULONG gl_mode;
    ULONG stage;
    BOOLEAN camera_position = FALSE;
    BOOLEAN result;

    if (!device || !plan) return FALSE;
    for (stage = 0; stage < 4; ++stage) {
        if ((plan->generated_stages & (1UL << stage)) &&
            plan->modes[stage] ==
                QXP_D3D_TCI_CAMERASPACEPOSITION) {
            camera_position = TRUE;
            break;
        }
    }
    /* GL captures EYE_PLANE through the inverse model-view current at the
     * TexGen call. Identity makes the three canonical planes reproduce D3D's
     * camera-space x/y/z exactly; object matrices are reloaded immediately
     * afterward by the fixed-state path. */
    if (camera_position) {
        matrix_arguments[0] = QXP_GL_MODELVIEW;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                                 matrix_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LOAD_IDENTITY,
                                 NULL, 0)) return FALSE;
    }
    for (stage = 0; stage < 4; ++stage) {
        active_arguments[0] = QXP_GL_TEXTURE0 + stage;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                                 active_arguments, 1)) return FALSE;
        generation_mode =
            (plan->generated_stages & (1UL << stage)) ?
                plan->modes[stage] : QXP_D3D_TCI_PASSTHRU;
        if (generation_mode == QXP_D3D_TCI_CAMERASPACENORMAL)
            gl_mode = QXP_GL_NORMAL_MAP;
        else if (generation_mode ==
                 QXP_D3D_TCI_CAMERASPACEPOSITION)
            gl_mode = QXP_GL_EYE_LINEAR;
        else if (generation_mode ==
                 QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR)
            gl_mode = QXP_GL_REFLECTION_MAP;
        else if (generation_mode == QXP_D3D_TCI_PASSTHRU)
            gl_mode = 0;
        else return FALSE;
        for (coordinate = 0; coordinate < 3; ++coordinate) {
            state_arguments[0] = QXP_GL_TEXTURE_GEN_S + coordinate;
            if (!gl_mode) {
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_DISABLE,
                        state_arguments, 1)) return FALSE;
                continue;
            }
            texgen_arguments[0] = QXP_GL_S + coordinate;
            texgen_arguments[1] = QXP_GL_TEXTURE_GEN_MODE;
            texgen_arguments[2] = gl_mode;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_TEX_GEN_I,
                    texgen_arguments, 3)) return FALSE;
            if (generation_mode ==
                    QXP_D3D_TCI_CAMERASPACEPOSITION) {
                plane_arguments[0] = QXP_GL_S + coordinate;
                plane_arguments[1] = QXP_GL_EYE_PLANE;
                plane_arguments[2] = 0;
                if (!qxp_d3d_fifo_submit_input(
                        device, QXP_FENUM_GL_TEX_GEN_FV,
                        plane_arguments, 3, eye_planes[coordinate],
                        sizeof(eye_planes[coordinate]))) return FALSE;
            }
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_ENABLE,
                    state_arguments, 1)) return FALSE;
        }
        /* Generated S/T/R override a neutral array whose q is one. NORMAL_MAP
         * and REFLECTION_MAP are invalid modes for Q, so keep Q generation
         * disabled for every path and retain that literal homogeneous input. */
        state_arguments[0] = QXP_GL_TEXTURE_GEN_Q;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) return FALSE;
    }
    active_arguments[0] = QXP_GL_TEXTURE0;
    result = qxp_d3d_fifo_submit(
        device, QXP_FENUM_GL_ACTIVE_TEXTURE, active_arguments, 1);
    if (result && camera_position &&
        !device->d3d_texgen_camera_position_logged) {
        device->d3d_texgen_camera_position_logged = TRUE;
        /* This success marker is emitted only after all glTexGeni mode and
         * pointer-bearing glTexGenfv eye-plane submissions completed. */
        qxp_debug("QXPMP D3D TEXGEN CAMERA POSITION+");
    }
    return result;
}

static BOOLEAN qxp_d3d_apply_object_matrices(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_HWTL_STATE *state)
{
    ULONG active_arguments[1];
    ULONG matrix_arguments[1];
    ULONG ortho_arguments[12];
    ULONG depth_arguments[4];
    ULONG clip_arguments[2];
    ULONG clip_equation[8];
    ULONG enable_arguments[1];
    ULONG texture_flags;
    ULONG texture_count;
    ULONG stage;
    ULONG plane;
    ULONG component;
    QXP_D3D_MATRIX texture_matrix;
    const QXP_D3D_MATRIX *texture_source;

    if (!device || !state) return FALSE;
    matrix_arguments[0] = QXP_GL_PROJECTION;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                             matrix_arguments, 1) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LOAD_IDENTITY,
                             NULL, 0)) return FALSE;
    ortho_arguments[0] = 0;
    ortho_arguments[1] = 0xbff00000UL;
    ortho_arguments[2] = 0;
    ortho_arguments[3] = 0x3ff00000UL;
    if (device->d3d_target_texture && !device->d3d_target_present) {
        ortho_arguments[4] = 0;
        ortho_arguments[5] = 0x3ff00000UL;
        ortho_arguments[6] = 0;
        ortho_arguments[7] = 0xbff00000UL;
    } else {
        ortho_arguments[4] = 0;
        ortho_arguments[5] = 0xbff00000UL;
        ortho_arguments[6] = 0;
        ortho_arguments[7] = 0x3ff00000UL;
    }
    ortho_arguments[8] = 0;
    ortho_arguments[9] = 0;
    ortho_arguments[10] = 0;
    ortho_arguments[11] = 0xbff00000UL;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ORTHO,
                             ortho_arguments, 12) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_MULT_MATRIX_F,
                                 &state->projection) ||
        !qxp_double_words_from_float(state->z_min_bits,
                                     &depth_arguments[0]) ||
        !qxp_double_words_from_float(state->z_max_bits,
                                     &depth_arguments[2]) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_RANGE,
                             depth_arguments, 4)) return FALSE;

    matrix_arguments[0] = QXP_GL_MODELVIEW;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                             matrix_arguments, 1) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_LOAD_MATRIX_F,
                                 &state->view) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_MULT_MATRIX_F,
                                 &state->world[0])) return FALSE;
    /* Direct3D clip planes are expressed in world space. OpenGL captures a
     * plane through the inverse-transpose of the current model-view matrix,
     * so temporarily remove the world transform while defining them. */
    if (!qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_LOAD_MATRIX_F,
                                 &state->view)) return FALSE;
    for (plane = 0; plane < QXP_D3D_MAX_USER_CLIP_PLANES; ++plane) {
        for (component = 0; component < 4; ++component) {
            if (!qxp_double_words_from_float(
                    state->clip_planes[plane][component],
                    &clip_equation[component * 2])) return FALSE;
        }
        clip_arguments[0] = QXP_GL_CLIP_PLANE0 + plane;
        clip_arguments[1] = 0;
        enable_arguments[0] = clip_arguments[0];
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_CLIP_PLANE,
                clip_arguments, 2, clip_equation,
                sizeof(clip_equation)) ||
            !qxp_d3d_fifo_submit(
                device,
                state->clipping_enable &&
                    (state->clip_plane_enable & (1UL << plane)) ?
                    QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
                enable_arguments, 1)) return FALSE;
    }
    if (!qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_LOAD_MATRIX_F,
                                 &state->view) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_MULT_MATRIX_F,
                                 &state->world[0])) return FALSE;

    for (stage = 0; stage < 4; ++stage) {
        active_arguments[0] = QXP_GL_TEXTURE0 + stage;
        matrix_arguments[0] = QXP_GL_TEXTURE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                                 active_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                                 matrix_arguments, 1)) return FALSE;
        texture_flags = state->texture_transform_flags[stage];
        texture_count = texture_flags & 0x0fUL;
        if (texture_count) {
            texture_source = &state->texture[stage];
            if (!((texture_flags & 0x00000100UL) &&
                  texture_count == 4)) {
                VideoPortZeroMemory(&texture_matrix,
                                    sizeof(texture_matrix));
                if (texture_flags & 0x00000100UL) {
                    /* COUNT3|PROJECTED divides s/t by D3D output 2. GL
                     * performs projective division with output q instead, so
                     * move D3D's third matrix column into GL's fourth. */
                    for (component = 0; component < 4; ++component) {
                        texture_matrix.element[component * 4] =
                            texture_source->element[component * 4];
                        texture_matrix.element[component * 4 + 1] =
                            texture_source->element[component * 4 + 1];
                        texture_matrix.element[component * 4 + 3] =
                            texture_source->element[component * 4 + 2];
                    }
                } else {
                    /* D3D discards outputs beyond COUNTn and performs no
                     * homogeneous divide. Preserve up to s/t/r and force GL
                     * q to the implicit 1 supplied by 1D..3D inputs. */
                    for (component = 0; component < 4; ++component) {
                        texture_matrix.element[component * 4] =
                            texture_source->element[component * 4];
                        if (texture_count >= 2)
                            texture_matrix.element[component * 4 + 1] =
                                texture_source->element[component * 4 + 1];
                        if (texture_count >= 3)
                            texture_matrix.element[component * 4 + 2] =
                                texture_source->element[component * 4 + 2];
                    }
                    texture_matrix.element[15] = 0x3f800000UL;
                }
                texture_source = &texture_matrix;
            }
            if (!qxp_d3d_submit_matrix_f(
                    device, QXP_FENUM_GL_LOAD_MATRIX_F,
                    texture_source)) return FALSE;
        } else if (!qxp_d3d_fifo_submit(
                       device, QXP_FENUM_GL_LOAD_IDENTITY,
                       NULL, 0)) return FALSE;
    }
    active_arguments[0] = QXP_GL_TEXTURE0;
    matrix_arguments[0] = QXP_GL_MODELVIEW;
    return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                               active_arguments, 1) &&
           qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                               matrix_arguments, 1);
}

static BOOLEAN qxp_d3d_object_color_material_mode(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state, BOOLEAN program_material_sources,
    PULONG mode, PBOOLEAN enabled)
{
    ULONG mask = 0;
    ULONG sources[4];
    ULONG index;

    if (!request || !state || !mode || !enabled) return FALSE;
    *enabled = FALSE;
    *mode = QXP_GL_DIFFUSE;
    if (!state->color_vertex_enable) return TRUE;
    sources[0] = state->diffuse_material_source;
    sources[1] = request->specular_enable ?
                 state->specular_material_source : 0;
    sources[2] = state->ambient_material_source;
    sources[3] = state->emissive_material_source;
    if (program_material_sources) {
        /* The bounded positional program selects MATERIAL/COLOR1/COLOR2 for
         * every role itself. Fixed OpenGL color material cannot represent
         * arbitrary combinations (for example COLOR1 diffuse + specular),
         * so leave it disabled while still submitting ordinary material and
         * light state for the program parameters below. */
        return TRUE;
    }
    for (index = 0; index < 4; ++index) {
        /* Fixed OpenGL color material cannot select the secondary color. */
        if (sources[index] == QXP_D3DMCS_COLOR2) {
            return FALSE;
        }
        if (sources[index] == QXP_D3DMCS_COLOR1) mask |= 1UL << index;
    }
    if (!mask) return TRUE;
    if (mask == 1) *mode = QXP_GL_DIFFUSE;
    else if (mask == 2) *mode = QXP_GL_SPECULAR;
    else if (mask == 4) *mode = QXP_GL_AMBIENT;
    else if (mask == 8) *mode = QXP_GL_EMISSION;
    else if (mask == 5) *mode = QXP_GL_AMBIENT_AND_DIFFUSE;
    else return FALSE;
    *enabled = TRUE;
    return TRUE;
}

static BOOLEAN qxp_d3d_submit_light_fv(
    QXP_DEVICE_EXTENSION *device, ULONG light, ULONG parameter,
    const ULONG *values, ULONG bytes)
{
    ULONG arguments[3];

    arguments[0] = light;
    arguments[1] = parameter;
    arguments[2] = 0;
    return qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_LIGHT_FV, arguments, 3, values, bytes);
}

static BOOLEAN qxp_d3d_submit_material_fv(
    QXP_DEVICE_EXTENSION *device, ULONG parameter,
    const ULONG *values, ULONG bytes)
{
    ULONG arguments[3];

    arguments[0] = QXP_GL_FRONT_AND_BACK;
    arguments[1] = parameter;
    arguments[2] = 0;
    return qxp_d3d_fifo_submit_input(
        device, QXP_FENUM_GL_MATERIAL_FV, arguments, 3, values, bytes);
}

static BOOLEAN qxp_d3d_apply_object_lighting(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_HWTL_STATE *state, BOOLEAN program_material_sources,
    BOOLEAN scalable_lighting)
{
    ULONG state_arguments[2];
    ULONG model_arguments[2];
    ULONG material_arguments[2];
    ULONG light_arguments[3];
    ULONG position[4];
    ULONG ambient[4];
    ULONG cutoff;
    ULONG inner_cutoff;
    ULONG exponent;
    ULONG slot;
    ULONG component;
    BOOLEAN color_material;

    if (!device || !request || !state) return FALSE;
    state_arguments[0] = QXP_GL_LIGHTING;
    if (!state->lighting_enable) {
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) return FALSE;
        state_arguments[0] = QXP_GL_COLOR_MATERIAL;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) return FALSE;
        state_arguments[0] = QXP_GL_NORMALIZE;
        return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                   state_arguments, 1);
    }
    if ((!(request->fvf & QXP_D3DFVF_NORMAL) &&
         !program_material_sources) ||
        !qxp_d3d_object_color_material_mode(
            request, state, program_material_sources,
            &material_arguments[1], &color_material))
        return FALSE;

    material_arguments[0] = QXP_GL_FRONT_AND_BACK;
    if (color_material &&
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_COLOR_MATERIAL,
                             material_arguments, 2)) return FALSE;
    state_arguments[0] = QXP_GL_COLOR_MATERIAL;
    if (!qxp_d3d_fifo_submit(
            device, color_material ? QXP_FENUM_GL_ENABLE :
                                     QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) return FALSE;

    if (!qxp_d3d_submit_material_fv(
            device, QXP_GL_DIFFUSE, state->material.diffuse.component,
            sizeof(state->material.diffuse)) ||
        !qxp_d3d_submit_material_fv(
            device, QXP_GL_AMBIENT, state->material.ambient.component,
            sizeof(state->material.ambient)) ||
        !qxp_d3d_submit_material_fv(
            device, QXP_GL_SPECULAR, state->material.specular.component,
            sizeof(state->material.specular)) ||
        !qxp_d3d_submit_material_fv(
            device, QXP_GL_EMISSION, state->material.emissive.component,
            sizeof(state->material.emissive)) ||
        !qxp_d3d_submit_material_fv(
            device, QXP_GL_SHININESS, &state->material.power_bits,
            sizeof(state->material.power_bits))) return FALSE;

    ambient[0] = qxp_float_from_unorm8((UCHAR)(state->ambient >> 16));
    ambient[1] = qxp_float_from_unorm8((UCHAR)(state->ambient >> 8));
    ambient[2] = qxp_float_from_unorm8((UCHAR)state->ambient);
    ambient[3] = 0x3f800000UL;
    model_arguments[0] = QXP_GL_LIGHT_MODEL_AMBIENT;
    model_arguments[1] = 0;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_LIGHT_MODEL_FV,
            model_arguments, 2, ambient, sizeof(ambient))) return FALSE;
    model_arguments[0] = QXP_GL_LIGHT_MODEL_LOCAL_VIEWER;
    model_arguments[1] = state->local_viewer;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_MODEL_I,
                             model_arguments, 2)) return FALSE;
    model_arguments[0] = QXP_GL_LIGHT_MODEL_COLOR_CONTROL;
    model_arguments[1] = request->specular_enable ?
        QXP_GL_SEPARATE_SPECULAR_COLOR : QXP_GL_SINGLE_COLOR;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_MODEL_I,
                             model_arguments, 2)) return FALSE;

    state_arguments[0] = QXP_GL_NORMALIZE;
    if (!qxp_d3d_fifo_submit(
            device, state->normalize_normals ? QXP_FENUM_GL_ENABLE :
                                              QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) return FALSE;

    state_arguments[0] = QXP_GL_MODELVIEW;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                             state_arguments, 1) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_LOAD_MATRIX_F,
                                 &state->view)) return FALSE;
    for (slot = 0; slot < QXP_D3D_MAX_ACTIVE_LIGHTS; ++slot) {
        state_arguments[0] = QXP_GL_LIGHT0 + slot;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) return FALSE;
    }
    for (slot = 0; slot < state->active_light_count; ++slot) {
        const QXP_D3D_LIGHT *light =
            &state->active_lights[slot].light;
        ULONG gl_light = QXP_GL_LIGHT0 + slot;
        ULONG light_ambient[4];
        ULONG light_diffuse[4];

        cutoff = 0x43340000UL;
        inner_cutoff = 0;
        exponent = 0;
        if (light->type == QXP_D3DLIGHT_SPOT &&
            (!qxp_float_multiply(light->phi_bits, 0x41e52ee1UL,
                                 &cutoff) ||
             !qxp_float_multiply(light->theta_bits, 0x41e52ee1UL,
                                 &inner_cutoff))) return FALSE;
        for (component = 0; component < 4; ++component) {
            light_ambient[component] = light->ambient.component[component];
            light_diffuse[component] = light->diffuse.component[component];
        }
        if (scalable_lighting) {
            /* D3D light-color alpha does not enter vertex lighting. Preserve
             * all RGB terms while carrying the inner half-angle in degrees
             * and Falloff through existing GL light state for the internal
             * shader. Point lights carry zero and remain distinguishable by
             * GL's -1 spotCosCutoff. */
            light_ambient[3] = light->type == QXP_D3DLIGHT_SPOT ?
                light->falloff_bits : 0;
            light_diffuse[3] = light->type == QXP_D3DLIGHT_SPOT ?
                inner_cutoff : 0;
        }

        if (!qxp_d3d_submit_light_fv(
                device, gl_light, QXP_GL_AMBIENT,
                light_ambient, sizeof(light_ambient)) ||
            !qxp_d3d_submit_light_fv(
                device, gl_light, QXP_GL_DIFFUSE,
                light_diffuse, sizeof(light_diffuse)) ||
            !qxp_d3d_submit_light_fv(
                device, gl_light, QXP_GL_SPECULAR,
                light->specular.component, sizeof(light->specular)))
            return FALSE;
        if (light->type == QXP_D3DLIGHT_DIRECTIONAL) {
            for (component = 0; component < 3; ++component)
                position[component] =
                    light->direction[component] ^ 0x80000000UL;
            position[3] = 0;
        } else {
            for (component = 0; component < 3; ++component)
                position[component] = light->position[component];
            position[3] = 0x3f800000UL;
        }
        if (!qxp_d3d_submit_light_fv(
                device, gl_light, QXP_GL_POSITION,
                position, sizeof(position))) return FALSE;

        light_arguments[0] = gl_light;
        light_arguments[1] = QXP_GL_CONSTANT_ATTENUATION;
        light_arguments[2] = light->attenuation0_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                 light_arguments, 3)) return FALSE;
        light_arguments[1] = QXP_GL_LINEAR_ATTENUATION;
        light_arguments[2] = light->attenuation1_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                 light_arguments, 3)) return FALSE;
        light_arguments[1] = QXP_GL_QUADRATIC_ATTENUATION;
        light_arguments[2] = light->attenuation2_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                 light_arguments, 3)) return FALSE;

        if (light->type == QXP_D3DLIGHT_SPOT) {
            /* GL has one exponent but D3D has distinct theta/phi cones.
             * Exponent zero preserves the exact D3D full-intensity inner
             * cone; internal programs cover the outer falloff. */
            if (!qxp_d3d_submit_light_fv(
                    device, gl_light, QXP_GL_SPOT_DIRECTION,
                    light->direction, sizeof(light->direction)))
                return FALSE;
        }
        light_arguments[1] = QXP_GL_SPOT_CUTOFF;
        light_arguments[2] = cutoff;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                 light_arguments, 3)) return FALSE;
        light_arguments[1] = QXP_GL_SPOT_EXPONENT;
        light_arguments[2] = exponent;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                 light_arguments, 3)) return FALSE;
        if (light->type == QXP_D3DLIGHT_SPOT &&
            state->active_light_count == 1 && !slot) {
            /* ARBvp exposes GL's precomputed cosine in spot.direction.w.
             * Light zero carries D3D's outer Phi cone; disabled light one
             * carries the inner Theta cone without consuming transport state
             * or affecting conventional fixed lighting. */
            light_arguments[0] = QXP_GL_LIGHT0 + 1;
            light_arguments[1] = QXP_GL_SPOT_CUTOFF;
            light_arguments[2] = inner_cutoff;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LIGHT_F,
                                     light_arguments, 3)) return FALSE;
            light_arguments[0] = gl_light;
        }
        state_arguments[0] = gl_light;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                 state_arguments, 1)) return FALSE;
    }

    state_arguments[0] = QXP_GL_MODELVIEW;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                             state_arguments, 1) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_LOAD_MATRIX_F,
                                 &state->view) ||
        !qxp_d3d_submit_matrix_f(device, QXP_FENUM_GL_MULT_MATRIX_F,
                                 &state->world[0])) return FALSE;
    state_arguments[0] = QXP_GL_LIGHTING;
    return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                               state_arguments, 1);
}

static BOOLEAN qxp_d3d_texture_source(ULONG argument, BOOLEAN alpha,
                                      ULONG stage, PULONG source,
                                      PULONG operand)
{
    ULONG selected;
    BOOLEAN complement;
    BOOLEAN alpha_replicate;

    if (!source || !operand) return FALSE;
    if (argument == QXP_TEXTURE_SOURCE_ZERO ||
        argument == QXP_TEXTURE_SOURCE_ONE) {
        *source = QXP_GL_ZERO;
        if (alpha) {
            *operand = argument == QXP_TEXTURE_SOURCE_ONE ?
                       QXP_GL_ONE_MINUS_SRC_ALPHA : QXP_GL_SRC_ALPHA;
        } else {
            *operand = argument == QXP_TEXTURE_SOURCE_ONE ?
                       QXP_GL_ONE_MINUS_SRC_COLOR : QXP_GL_SRC_COLOR;
        }
        return TRUE;
    }
    if (
        (argument & ~(QXP_D3DTA_SELECTMASK | QXP_D3DTA_COMPLEMENT |
                      QXP_D3DTA_ALPHAREPLICATE))) return FALSE;
    selected = argument & QXP_D3DTA_SELECTMASK;
    complement = (argument & QXP_D3DTA_COMPLEMENT) != 0;
    alpha_replicate = (argument & QXP_D3DTA_ALPHAREPLICATE) != 0;
    if (selected == QXP_D3DTA_TEXTURE) {
        *source = QXP_GL_TEXTURE;
    } else if (selected == QXP_D3DTA_DIFFUSE) {
        *source = QXP_GL_PRIMARY_COLOR;
    } else if (selected == QXP_D3DTA_CURRENT) {
        *source = stage ? QXP_GL_PREVIOUS : QXP_GL_PRIMARY_COLOR;
    } else if (selected == QXP_D3DTA_TFACTOR) {
        *source = QXP_GL_CONSTANT;
    } else {
        return FALSE;
    }
    if (alpha || alpha_replicate) {
        *operand = complement ? QXP_GL_ONE_MINUS_SRC_ALPHA :
                                QXP_GL_SRC_ALPHA;
    } else {
        *operand = complement ? QXP_GL_ONE_MINUS_SRC_COLOR :
                                QXP_GL_SRC_COLOR;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_texture_combine(
    QXP_DEVICE_EXTENSION *device, ULONG operation, ULONG argument1,
    ULONG argument2, ULONG argument0, BOOLEAN alpha, ULONG stage)
{
    QXP_TEXTURE_SPECIAL_PLAN special;
    ULONG arguments[3];
    ULONG source0;
    ULONG source1 = 0;
    ULONG source2 = 0;
    ULONG operand0;
    ULONG operand1 = 0;
    ULONG operand2 = QXP_GL_SRC_ALPHA;
    ULONG combine;
    ULONG scale = 1;
    BOOLEAN binary = FALSE;
    BOOLEAN interpolate = FALSE;

    if (operation == QXP_D3DTOP_SELECTARG1) {
        if (!qxp_d3d_texture_source(argument1, alpha, stage, &source0,
                                    &operand0)) return FALSE;
        combine = QXP_GL_REPLACE;
    } else if (operation == QXP_D3DTOP_SELECTARG2) {
        if (!qxp_d3d_texture_source(argument2, alpha, stage, &source0,
                                    &operand0)) return FALSE;
        combine = QXP_GL_REPLACE;
    /* GL DOT3_RGB preserves the independently configured alpha combine,
     * matching D3D's color-stage DOTPRODUCT3 behavior. */
    } else if (operation == QXP_D3DTOP_MODULATE ||
               operation == QXP_D3DTOP_MODULATE2X ||
               operation == QXP_D3DTOP_MODULATE4X ||
               operation == QXP_D3DTOP_ADD ||
               operation == QXP_D3DTOP_ADDSIGNED ||
               operation == QXP_D3DTOP_ADDSIGNED2X ||
               operation == QXP_D3DTOP_SUBTRACT ||
               (!alpha && operation == QXP_D3DTOP_DOTPRODUCT3)) {
        if (!qxp_d3d_texture_source(argument1, alpha, stage, &source0,
                                    &operand0) ||
            !qxp_d3d_texture_source(argument2, alpha, stage, &source1,
                                    &operand1)) return FALSE;
        combine = operation == QXP_D3DTOP_ADD ? QXP_GL_ADD :
                  ((operation == QXP_D3DTOP_ADDSIGNED ||
                    operation == QXP_D3DTOP_ADDSIGNED2X) ?
                       QXP_GL_ADD_SIGNED :
                  (operation == QXP_D3DTOP_SUBTRACT ? QXP_GL_SUBTRACT :
                  (operation == QXP_D3DTOP_DOTPRODUCT3 ? QXP_GL_DOT3_RGB :
                                                        QXP_GL_MODULATE)));
        binary = TRUE;
        if (operation == QXP_D3DTOP_MODULATE2X ||
            operation == QXP_D3DTOP_ADDSIGNED2X) scale = 2;
        if (operation == QXP_D3DTOP_MODULATE4X) scale = 4;
    } else if (!alpha && device->d3d_texture_combine3 &&
               qxp_texture_special_plan(
                   operation, argument1, argument2, &special)) {
        /* GL_ATI_texture_env_combine3 supplies the exact D3D equation
         * Arg0 * Arg2 + Arg1.  The pure planner maps each D3D operation's
         * color/alpha replication and inversion onto those three inputs. */
        if (!qxp_d3d_texture_source(
                special.source[0], FALSE, stage, &source0, &operand0) ||
            !qxp_d3d_texture_source(
                special.source[1], FALSE, stage, &source1, &operand1) ||
            !qxp_d3d_texture_source(
                special.source[2], FALSE, stage, &source2, &operand2))
            return FALSE;
        combine = special.combine;
        binary = TRUE;
        interpolate = TRUE;
    } else if (operation == QXP_D3DTOP_LERP) {
        /* GL_INTERPOLATE is source0*source2 + source1*(1-source2),
         * so D3D's Arg0*Arg1 + (1-Arg0)*Arg2 maps as 1, 2, 0. */
        if (!qxp_d3d_texture_source(argument1, alpha, stage, &source0,
                                    &operand0) ||
            !qxp_d3d_texture_source(argument2, alpha, stage, &source1,
                                    &operand1) ||
            !qxp_d3d_texture_source(argument0, alpha, stage, &source2,
                                    &operand2)) return FALSE;
        combine = QXP_GL_INTERPOLATE;
        binary = TRUE;
        interpolate = TRUE;
    } else if (operation == QXP_D3DTOP_BLENDDIFFUSEALPHA ||
               operation == QXP_D3DTOP_BLENDTEXTUREALPHA ||
               operation == QXP_D3DTOP_BLENDFACTORALPHA ||
               operation == QXP_D3DTOP_BLENDCURRENTALPHA) {
        if (!qxp_d3d_texture_source(argument1, alpha, stage, &source0,
                                    &operand0) ||
            !qxp_d3d_texture_source(argument2, alpha, stage, &source1,
                                    &operand1)) return FALSE;
        if (operation == QXP_D3DTOP_BLENDTEXTUREALPHA)
            source2 = QXP_GL_TEXTURE;
        else if (operation == QXP_D3DTOP_BLENDFACTORALPHA)
            source2 = QXP_GL_CONSTANT;
        else if (operation == QXP_D3DTOP_BLENDCURRENTALPHA)
            source2 = stage ? QXP_GL_PREVIOUS : QXP_GL_PRIMARY_COLOR;
        else
            source2 = QXP_GL_PRIMARY_COLOR;
        combine = QXP_GL_INTERPOLATE;
        binary = TRUE;
        interpolate = TRUE;
    } else {
        return FALSE;
    }

    arguments[0] = QXP_GL_TEXTURE_ENV;
    arguments[1] = alpha ? QXP_GL_COMBINE_ALPHA : QXP_GL_COMBINE_RGB;
    arguments[2] = combine;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                             arguments, 3)) return FALSE;
    arguments[1] = alpha ? QXP_GL_SOURCE0_ALPHA : QXP_GL_SOURCE0_RGB;
    arguments[2] = source0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                             arguments, 3)) return FALSE;
    arguments[1] = alpha ? QXP_GL_OPERAND0_ALPHA : QXP_GL_OPERAND0_RGB;
    arguments[2] = operand0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                             arguments, 3)) return FALSE;
    if (binary) {
        arguments[1] = alpha ? QXP_GL_SOURCE1_ALPHA : QXP_GL_SOURCE1_RGB;
        arguments[2] = source1;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
        arguments[1] = alpha ? QXP_GL_OPERAND1_ALPHA : QXP_GL_OPERAND1_RGB;
        arguments[2] = operand1;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
    }
    if (interpolate) {
        arguments[1] = alpha ? QXP_GL_SOURCE2_ALPHA : QXP_GL_SOURCE2_RGB;
        arguments[2] = source2;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
        arguments[1] = alpha ? QXP_GL_OPERAND2_ALPHA : QXP_GL_OPERAND2_RGB;
        arguments[2] = operand2;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
    }
    arguments[1] = alpha ? QXP_GL_ALPHA_SCALE : QXP_GL_RGB_SCALE;
    arguments[2] = scale;
    return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                               arguments, 3);
}

static BOOLEAN qxp_d3d_texture_combine4_channel(
    QXP_DEVICE_EXTENSION *device, const QXP_TEXTURE_COMBINE4_PLAN *plan,
    BOOLEAN alpha, ULONG stage)
{
    static const ULONG rgb_sources[4] = {
        QXP_GL_SOURCE0_RGB, QXP_GL_SOURCE1_RGB,
        QXP_GL_SOURCE2_RGB, QXP_GL_SOURCE3_RGB
    };
    static const ULONG alpha_sources[4] = {
        QXP_GL_SOURCE0_ALPHA, QXP_GL_SOURCE1_ALPHA,
        QXP_GL_SOURCE2_ALPHA, QXP_GL_SOURCE3_ALPHA
    };
    static const ULONG rgb_operands[4] = {
        QXP_GL_OPERAND0_RGB, QXP_GL_OPERAND1_RGB,
        QXP_GL_OPERAND2_RGB, QXP_GL_OPERAND3_RGB
    };
    static const ULONG alpha_operands[4] = {
        QXP_GL_OPERAND0_ALPHA, QXP_GL_OPERAND1_ALPHA,
        QXP_GL_OPERAND2_ALPHA, QXP_GL_OPERAND3_ALPHA
    };
    ULONG arguments[3];
    ULONG source;
    ULONG operand;
    ULONG slot;

    if (!device || !plan) return FALSE;
    arguments[0] = QXP_GL_TEXTURE_ENV;
    arguments[1] = alpha ? QXP_GL_COMBINE_ALPHA : QXP_GL_COMBINE_RGB;
    arguments[2] = plan->combine;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                             arguments, 3)) return FALSE;
    for (slot = 0; slot < 4; ++slot) {
        if (!qxp_d3d_texture_source(
                plan->source[slot], alpha, stage, &source, &operand))
            return FALSE;
        arguments[1] = alpha ? alpha_sources[slot] : rgb_sources[slot];
        arguments[2] = source;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
        arguments[1] = alpha ? alpha_operands[slot] : rgb_operands[slot];
        arguments[2] = operand;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 arguments, 3)) return FALSE;
    }
    arguments[1] = alpha ? QXP_GL_ALPHA_SCALE : QXP_GL_RGB_SCALE;
    arguments[2] = plan->scale;
    return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                               arguments, 3);
}

static BOOLEAN qxp_d3d_texture_combine4(
    QXP_DEVICE_EXTENSION *device,
    ULONG color_op, ULONG color_arg1, ULONG color_arg2,
    ULONG alpha_op, ULONG alpha_arg1, ULONG alpha_arg2, ULONG alpha_arg0,
    ULONG stage)
{
    QXP_TEXTURE_COMBINE4_PLAN color_plan;
    QXP_TEXTURE_COMBINE4_PLAN alpha_plan;
    ULONG arguments[3];

    if (!device || !device->d3d_texture_combine4 ||
        !qxp_texture_combine4_special_plan(
            color_op, color_arg1, color_arg2, &color_plan) ||
        !qxp_texture_combine4_alpha_plan(
            alpha_op, alpha_arg1, alpha_arg2, alpha_arg0, &alpha_plan))
        return FALSE;
    arguments[0] = QXP_GL_TEXTURE_ENV;
    arguments[1] = QXP_GL_TEXTURE_ENV_MODE;
    arguments[2] = QXP_GL_COMBINE4_NV;
    return qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                               arguments, 3) &&
           qxp_d3d_texture_combine4_channel(
               device, &color_plan, FALSE, stage) &&
           qxp_d3d_texture_combine4_channel(
               device, &alpha_plan, TRUE, stage);
}

static BOOLEAN qxp_d3d_ensure_neutral_texture(
    QXP_DEVICE_EXTENSION *device)
{
    ULONG bind_arguments[2];
    ULONG parameter_arguments[3];
    ULONG image_arguments[10];
    ULONG opaque_black = 0xff000000UL;
    volatile ULONG *fifo;
    ULONG fifo_start;
    ULONG face;

    if (!device) return FALSE;
    if (device->d3d_neutral_texture_defined) return TRUE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo) return FALSE;
    fifo_start = fifo[0];

    bind_arguments[0] = QXP_GL_TEXTURE_2D;
    bind_arguments[1] = QXP_D3D_NEUTRAL_TEXTURE_NAME;
    parameter_arguments[0] = QXP_GL_TEXTURE_2D;
    parameter_arguments[1] = QXP_GL_TEXTURE_MIN_FILTER;
    parameter_arguments[2] = QXP_GL_NEAREST;
    image_arguments[0] = QXP_GL_TEXTURE_2D;
    image_arguments[1] = 0;
    image_arguments[2] = QXP_GL_RGBA8;
    image_arguments[3] = 1;
    image_arguments[4] = 1;
    image_arguments[5] = 0;
    image_arguments[6] = QXP_GL_RGBA;
    image_arguments[7] = QXP_GL_UNSIGNED_BYTE;
    image_arguments[8] = 1;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             parameter_arguments, 3) ||
        !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_IMAGE_2D,
                           image_arguments, 9, &opaque_black,
                           sizeof(opaque_black))) goto failed;

    bind_arguments[0] = QXP_GL_TEXTURE_CUBE_MAP;
    bind_arguments[1] = QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME;
    parameter_arguments[0] = QXP_GL_TEXTURE_CUBE_MAP;
    image_arguments[6] = QXP_GL_RGBA;
    image_arguments[7] = QXP_GL_UNSIGNED_BYTE;
    image_arguments[8] = 1;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             parameter_arguments, 3)) goto failed;
    for (face = 0; face < 6; ++face) {
        image_arguments[0] = QXP_GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
        if (!qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_TEX_IMAGE_2D,
                image_arguments, 9, &opaque_black,
                sizeof(opaque_black))) goto failed;
    }

    bind_arguments[0] = QXP_GL_TEXTURE_3D;
    bind_arguments[1] = QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME;
    parameter_arguments[0] = QXP_GL_TEXTURE_3D;
    image_arguments[0] = QXP_GL_TEXTURE_3D;
    image_arguments[1] = 0;
    image_arguments[2] = QXP_GL_RGBA8;
    image_arguments[3] = 1;
    image_arguments[4] = 1;
    image_arguments[5] = 1;
    image_arguments[6] = 0;
    image_arguments[7] = QXP_GL_RGBA;
    image_arguments[8] = QXP_GL_UNSIGNED_BYTE;
    image_arguments[9] = 1;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             parameter_arguments, 3) ||
        !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_IMAGE_3D,
                           image_arguments, 10, &opaque_black,
                           sizeof(opaque_black))) goto failed;
    device->d3d_neutral_texture_defined = TRUE;
    return TRUE;

failed:
    fifo[0] = fifo_start;
    return FALSE;
}

static BOOLEAN qxp_d3d_apply_texture_state(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    ULONG stage)
{
    QXP_D3D_TEXTURE_STATE *texture;
    ULONG arguments[3];
    ULONG active_arguments[1];
    ULONG texture_name;
    ULONG color_op;
    ULONG color_arg1;
    ULONG color_arg2;
    ULONG color_arg0;
    ULONG alpha_state_op;
    ULONG alpha_state_arg1;
    ULONG alpha_state_arg2;
    ULONG alpha_state_arg0;
    ULONG address_u_state;
    ULONG address_v_state;
    ULONG mag_filter;
    ULONG min_filter_state;
    ULONG mip_filter;
    ULONG mip_lod_bias_bits;
    ULONG max_mip_level_state;
    ULONG address_u = QXP_GL_CLAMP_TO_EDGE;
    ULONG address_v = QXP_GL_CLAMP_TO_EDGE;
    ULONG min_filter = QXP_GL_NEAREST;
    ULONG max_mip_level = 0;
    ULONG alpha_op;
    ULONG alpha_arg1;
    ULONG alpha_arg2;
    ULONG alpha_arg0;
    ULONG factor_arguments[3];
    ULONG factor_color[4];
    ULONG texture_target;
    ULONG other_target;
    ULONG other_target2;
    ULONG required_levels;
    ULONG face;
    ULONG failure_phase = 0;
    BOOLEAN programmable_sample = FALSE;
    BOOLEAN programmable_cube = FALSE;
    BOOLEAN programmable_volume = FALSE;
    BOOLEAN neutral_texture = FALSE;
    BOOLEAN missing_fixed_texture = FALSE;

    if (!device || !request || stage > 3) return FALSE;
    texture = NULL;
    if (!stage) {
        texture_name = request->texture_name;
        color_op = request->texture_color_op;
        color_arg1 = request->texture_color_arg1;
        color_arg2 = request->texture_color_arg2;
        color_arg0 = request->texture_color_arg0;
        alpha_state_op = request->texture_alpha_op;
        alpha_state_arg1 = request->texture_alpha_arg1;
        alpha_state_arg2 = request->texture_alpha_arg2;
        alpha_state_arg0 = request->texture_alpha_arg0;
        address_u_state = request->texture_address_u;
        address_v_state = request->texture_address_v;
        mag_filter = request->texture_mag_filter;
        min_filter_state = request->texture_min_filter;
        mip_filter = request->texture_mip_filter;
        mip_lod_bias_bits = request->texture_mip_lod_bias_bits;
        max_mip_level_state = request->texture_max_mip_level;
    } else if (stage == 1) {
        texture_name = request->texture1_name;
        color_op = request->texture1_color_op;
        color_arg1 = request->texture1_color_arg1;
        color_arg2 = request->texture1_color_arg2;
        color_arg0 = request->texture1_color_arg0;
        alpha_state_op = request->texture1_alpha_op;
        alpha_state_arg1 = request->texture1_alpha_arg1;
        alpha_state_arg2 = request->texture1_alpha_arg2;
        alpha_state_arg0 = request->texture1_alpha_arg0;
        address_u_state = request->texture1_address_u;
        address_v_state = request->texture1_address_v;
        mag_filter = request->texture1_mag_filter;
        min_filter_state = request->texture1_min_filter;
        mip_filter = request->texture1_mip_filter;
        mip_lod_bias_bits = request->texture1_mip_lod_bias_bits;
        max_mip_level_state = request->texture1_max_mip_level;
    } else if (stage == 2) {
        texture_name = request->texture2_name;
        color_op = request->texture2_color_op;
        color_arg1 = request->texture2_color_arg1;
        color_arg2 = request->texture2_color_arg2;
        color_arg0 = request->texture2_color_arg0;
        alpha_state_op = request->texture2_alpha_op;
        alpha_state_arg1 = request->texture2_alpha_arg1;
        alpha_state_arg2 = request->texture2_alpha_arg2;
        alpha_state_arg0 = request->texture2_alpha_arg0;
        address_u_state = request->texture2_address_u;
        address_v_state = request->texture2_address_v;
        mag_filter = request->texture2_mag_filter;
        min_filter_state = request->texture2_min_filter;
        mip_filter = request->texture2_mip_filter;
        mip_lod_bias_bits = request->texture2_mip_lod_bias_bits;
        max_mip_level_state = request->texture2_max_mip_level;
    } else {
        texture_name = request->texture3_name;
        color_op = request->texture3_color_op;
        color_arg1 = request->texture3_color_arg1;
        color_arg2 = request->texture3_color_arg2;
        color_arg0 = request->texture3_color_arg0;
        alpha_state_op = request->texture3_alpha_op;
        alpha_state_arg1 = request->texture3_alpha_arg1;
        alpha_state_arg2 = request->texture3_alpha_arg2;
        alpha_state_arg0 = request->texture3_alpha_arg0;
        address_u_state = request->texture3_address_u;
        address_v_state = request->texture3_address_v;
        mag_filter = request->texture3_mag_filter;
        min_filter_state = request->texture3_min_filter;
        mip_filter = request->texture3_mip_filter;
        mip_lod_bias_bits = request->texture3_mip_lod_bias_bits;
        max_mip_level_state = request->texture3_max_mip_level;
    }

    /* D3DTOP_DISABLE terminates this and every higher fixed-function stage.
     * Runtimes may retain stale higher-stage state, but it must be ignored. */
    if (!request->pixel_program_name && !request->sm2_program_name &&
        ((stage >= 1 &&
          request->texture_color_op == QXP_D3DTOP_DISABLE) ||
         (stage >= 2 &&
          request->texture1_color_op == QXP_D3DTOP_DISABLE) ||
         (stage >= 3 &&
          request->texture2_color_op == QXP_D3DTOP_DISABLE))) {
        color_op = QXP_D3DTOP_DISABLE;
    }

    active_arguments[0] = QXP_GL_TEXTURE0 + stage;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                             active_arguments, 1)) {
        failure_phase = 1;
        goto failed;
    }
    programmable_sample =
        (request->pixel_program_name || request->sm2_program_name) &&
        (request->pixel_sampled_stages & (1UL << stage));
    programmable_cube =
        (request->pixel_program_name || request->sm2_program_name) &&
        (request->pixel_cube_stages & (1UL << stage));
    programmable_volume =
        (request->pixel_program_name || request->sm2_program_name) &&
        (request->pixel_volume_stages & (1UL << stage));
    missing_fixed_texture = !programmable_sample && !texture_name &&
        qxp_texture_stage_requires_texture(
            color_op, color_arg1, color_arg2, color_arg0,
            alpha_state_op, alpha_state_arg1, alpha_state_arg2,
            alpha_state_arg0);
    /* A NULL fixed-function binding is legal, including the default stage-0
     * MODULATE state.  A stage that would sample that absent texture becomes
     * a pass-through GL unit.  A stage whose equation does not sample still
     * needs the neutral object below so TFACTOR/CURRENT operations execute. */
    if ((color_op == QXP_D3DTOP_DISABLE || missing_fixed_texture) &&
        !programmable_sample) {
        arguments[0] = QXP_GL_TEXTURE_2D;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 arguments, 1)) {
            failure_phase = 2;
            goto failed;
        }
        arguments[0] = QXP_GL_TEXTURE_CUBE_MAP;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 arguments, 1)) {
            failure_phase = 3;
            goto failed;
        }
        arguments[0] = QXP_GL_TEXTURE_3D;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 arguments, 1)) {
            failure_phase = 0x60UL;
            goto failed;
        }
        /* Do not leave an unused stage's texture bound.  Render targets are
         * ordinary host textures, and sampling from an attached target is an
         * invalid feedback loop even when fixed-function texturing is off.
         * All three targets (2D, cube, 3D) are unbound on this dead stage. */
        arguments[0] = QXP_GL_TEXTURE_2D;
        arguments[1] = 0;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                                 arguments, 2)) {
            failure_phase = 4;
            goto failed;
        }
        arguments[0] = QXP_GL_TEXTURE_CUBE_MAP;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                                 arguments, 2)) {
            failure_phase = 5;
            goto failed;
        }
        arguments[0] = QXP_GL_TEXTURE_3D;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                                 arguments, 2)) {
            failure_phase = 0x61UL;
            goto failed;
        }
        return TRUE;
    }
    if (!texture_name) {
        if (programmable_sample &&
            !qxp_packet_texture_binding_contract(
                0, programmable_sample, programmable_cube,
                programmable_volume)) {
            failure_phase = 6;
            goto failed;
        }
        if (!qxp_d3d_ensure_neutral_texture(device)) {
            failure_phase = 7;
            goto failed;
        }
        if (programmable_cube) {
            texture_name = QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME;
            texture_target = QXP_GL_TEXTURE_CUBE_MAP;
            other_target = QXP_GL_TEXTURE_2D;
            other_target2 = QXP_GL_TEXTURE_3D;
        } else if (programmable_volume) {
            texture_name = QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME;
            texture_target = QXP_GL_TEXTURE_3D;
            other_target = QXP_GL_TEXTURE_2D;
            other_target2 = QXP_GL_TEXTURE_CUBE_MAP;
        } else {
            texture_name = QXP_D3D_NEUTRAL_TEXTURE_NAME;
            texture_target = QXP_GL_TEXTURE_2D;
            other_target = QXP_GL_TEXTURE_CUBE_MAP;
            other_target2 = QXP_GL_TEXTURE_3D;
        }
        neutral_texture = TRUE;
        goto texture_ready;
    }
    texture = qxp_d3d_find_texture(device, request->process_id, texture_name);
    if (!texture) {
        failure_phase = 0x10UL;
        goto failed;
    }
    if (address_u_state < 1 || address_u_state > 3 ||
        address_v_state < 1 || address_v_state > 3 || mag_filter > 2 ||
        min_filter_state > 2 || mip_filter > 2 ||
        (mip_lod_bias_bits & 0x7f800000UL) == 0x7f800000UL) {
        failure_phase = 0x11UL;
        goto failed;
    }
    if (!texture->levels) {
        failure_phase = 0x12UL;
        goto failed;
    }
    /* Volume resources are diagnostic-only: unlike existing 2D/cube fixed
     * stages, they require an SM2 sampled stage with the explicit volume bit.
     * Programmable stages must otherwise match their selected resource type. */
    if (!qxp_packet_texture_binding_contract(
            texture->texture_type, programmable_sample,
            programmable_cube, programmable_volume)) {
        failure_phase = 0x13UL;
        goto failed;
    }
    required_levels = (1UL << texture->levels) - 1;
    if (texture->texture_type == QXP_D3D_TEXTURE_CUBE) {
        for (face = 0; face < 6; ++face) {
            if ((texture->defined_levels[face] & required_levels) !=
                    required_levels) {
                failure_phase = 0x20UL + face;
                goto failed;
            }
        }
        texture_target = QXP_GL_TEXTURE_CUBE_MAP;
        other_target = QXP_GL_TEXTURE_2D;
        other_target2 = QXP_GL_TEXTURE_3D;
    } else if (texture->texture_type == QXP_D3D_TEXTURE_VOLUME &&
               (texture->defined_levels[0] & required_levels) ==
                   required_levels) {
        texture_target = QXP_GL_TEXTURE_3D;
        other_target = QXP_GL_TEXTURE_2D;
        other_target2 = QXP_GL_TEXTURE_CUBE_MAP;
    } else if (texture->texture_type == QXP_D3D_TEXTURE_2D &&
               (texture->defined_levels[0] & required_levels) ==
                   required_levels) {
        texture_target = QXP_GL_TEXTURE_2D;
        other_target = QXP_GL_TEXTURE_CUBE_MAP;
        other_target2 = QXP_GL_TEXTURE_3D;
    } else {
        failure_phase = 0x30UL;
        goto failed;
    }

texture_ready:
    if (programmable_sample) {
        color_op = QXP_D3DTOP_SELECTARG1;
        color_arg1 = QXP_D3DTA_TEXTURE;
        color_arg2 = QXP_D3DTA_CURRENT;
        color_arg0 = QXP_D3DTA_CURRENT;
        alpha_state_op = QXP_D3DTOP_SELECTARG1;
        alpha_state_arg1 = QXP_D3DTA_TEXTURE;
        alpha_state_arg2 = QXP_D3DTA_CURRENT;
        alpha_state_arg0 = QXP_D3DTA_CURRENT;
    }
    if (address_u_state == 1) address_u = QXP_GL_REPEAT;
    else if (address_u_state == 2)
        address_u = QXP_GL_MIRRORED_REPEAT;
    else address_u = QXP_GL_CLAMP_TO_EDGE;
    if (address_v_state == 1) address_v = QXP_GL_REPEAT;
    else if (address_v_state == 2)
        address_v = QXP_GL_MIRRORED_REPEAT;
    else address_v = QXP_GL_CLAMP_TO_EDGE;
    if (mip_filter == 1) {
        min_filter = min_filter_state == 2 ?
            QXP_GL_LINEAR_MIPMAP_NEAREST : QXP_GL_NEAREST_MIPMAP_NEAREST;
    } else if (mip_filter == 2) {
        min_filter = min_filter_state == 2 ?
            QXP_GL_LINEAR_MIPMAP_LINEAR : QXP_GL_NEAREST_MIPMAP_LINEAR;
    } else {
        min_filter = min_filter_state == 2 ? QXP_GL_LINEAR : QXP_GL_NEAREST;
    }
    max_mip_level = max_mip_level_state;
    if (neutral_texture) {
        max_mip_level = 0;
    } else if (max_mip_level >= texture->levels) {
        max_mip_level = texture->levels - 1;
    }
    alpha_op = alpha_state_op;
    alpha_arg1 = alpha_state_arg1;
    alpha_arg2 = alpha_state_arg2;
    alpha_arg0 = alpha_state_arg0;
    if (device->d3d_vertex_program == QXP_D3D_SPECULAR_ALPHA_PROGRAM)
        alpha_arg1 = QXP_D3DTA_DIFFUSE;
    if (alpha_op == QXP_D3DTOP_DISABLE) {
        alpha_op = QXP_D3DTOP_SELECTARG1;
        alpha_arg1 = QXP_D3DTA_DIFFUSE;
        alpha_arg0 = QXP_D3DTA_CURRENT;
    }
    if ((color_arg1 & QXP_D3DTA_SELECTMASK) ==
            QXP_D3DTA_TFACTOR ||
        (color_arg2 & QXP_D3DTA_SELECTMASK) ==
            QXP_D3DTA_TFACTOR ||
        (color_arg0 & QXP_D3DTA_SELECTMASK) ==
            QXP_D3DTA_TFACTOR ||
        (alpha_arg1 & QXP_D3DTA_SELECTMASK) == QXP_D3DTA_TFACTOR ||
        (alpha_arg2 & QXP_D3DTA_SELECTMASK) == QXP_D3DTA_TFACTOR ||
        (alpha_arg0 & QXP_D3DTA_SELECTMASK) == QXP_D3DTA_TFACTOR ||
        color_op == QXP_D3DTOP_BLENDFACTORALPHA ||
        alpha_op == QXP_D3DTOP_BLENDFACTORALPHA) {
        factor_arguments[0] = QXP_GL_TEXTURE_ENV;
        factor_arguments[1] = QXP_GL_TEXTURE_ENV_COLOR;
        factor_arguments[2] = 0;
        factor_color[0] = qxp_float_from_unorm8(
            (UCHAR)((request->texture_factor >> 16) & 0xff));
        factor_color[1] = qxp_float_from_unorm8(
            (UCHAR)((request->texture_factor >> 8) & 0xff));
        factor_color[2] = qxp_float_from_unorm8(
            (UCHAR)(request->texture_factor & 0xff));
        factor_color[3] = qxp_float_from_unorm8(
            (UCHAR)((request->texture_factor >> 24) & 0xff));
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_TEX_ENV_FV, factor_arguments, 3,
                factor_color, sizeof(factor_color))) {
            failure_phase = 0x40UL;
            goto failed;
        }
    }

    arguments[0] = other_target;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE, arguments, 1)) {
        failure_phase = 0x41UL;
        goto failed;
    }
    /* Both unselected targets are disabled and unbound on the active stage so
     * only the selected 2D/cube/3D target can be sampled. */
    arguments[0] = other_target;
    arguments[1] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             arguments, 2)) {
        failure_phase = 0x54UL;
        goto failed;
    }
    arguments[0] = other_target2;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE, arguments, 1)) {
        failure_phase = 0x55UL;
        goto failed;
    }
    arguments[0] = other_target2;
    arguments[1] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             arguments, 2)) {
        failure_phase = 0x56UL;
        goto failed;
    }
    arguments[0] = texture_target;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE, arguments, 1)) {
        failure_phase = 0x42UL;
        goto failed;
    }
    arguments[0] = texture_target;
    arguments[1] = texture_name;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             arguments, 2)) {
        failure_phase = 0x43UL;
        goto failed;
    }
    if (!neutral_texture) {
    arguments[0] = QXP_GL_TEXTURE_FILTER_CONTROL;
    arguments[1] = QXP_GL_TEXTURE_LOD_BIAS;
    arguments[2] = mip_lod_bias_bits;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_F,
                             arguments, 3)) {
        failure_phase = 0x44UL;
        goto failed;
    }
    arguments[0] = texture_target;
    arguments[1] = QXP_GL_TEXTURE_MAG_FILTER;
    arguments[2] = mag_filter == 2 ? QXP_GL_LINEAR : QXP_GL_NEAREST;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x45UL;
        goto failed;
    }
    arguments[1] = QXP_GL_TEXTURE_MIN_FILTER;
    arguments[2] = min_filter;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x46UL;
        goto failed;
    }
    arguments[1] = QXP_GL_TEXTURE_BASE_LEVEL;
    arguments[2] = max_mip_level;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x47UL;
        goto failed;
    }
    arguments[1] = QXP_GL_TEXTURE_MAX_LEVEL;
    arguments[2] = texture->levels - 1;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x48UL;
        goto failed;
    }
    arguments[1] = QXP_GL_TEXTURE_WRAP_S;
    arguments[2] = address_u;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x49UL;
        goto failed;
    }
    arguments[1] = QXP_GL_TEXTURE_WRAP_T;
    arguments[2] = texture->texture_type == QXP_D3D_TEXTURE_CUBE ?
                   QXP_GL_CLAMP_TO_EDGE : address_v;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                             arguments, 3)) {
        failure_phase = 0x4aUL;
        goto failed;
    }
    if (texture->texture_type == QXP_D3D_TEXTURE_CUBE) {
        arguments[1] = QXP_GL_TEXTURE_WRAP_S;
        arguments[2] = QXP_GL_CLAMP_TO_EDGE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                                 arguments, 3)) {
            failure_phase = 0x4bUL;
            goto failed;
        }
        arguments[1] = QXP_GL_TEXTURE_WRAP_R;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                                 arguments, 3)) {
            failure_phase = 0x4cUL;
            goto failed;
        }
    }
    if (texture->texture_type == QXP_D3D_TEXTURE_VOLUME) {
        /* Volume textures clamp the third coordinate; S and T retain the
         * requested addressing modes. */
        arguments[1] = QXP_GL_TEXTURE_WRAP_R;
        arguments[2] = QXP_GL_CLAMP_TO_EDGE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_PARAMETER_I,
                                 arguments, 3)) {
            failure_phase = 0x4dUL;
            goto failed;
        }
    }
    }
    if (device->d3d_texture_combine4 &&
        color_op >= QXP_D3DTOP_MODULATEALPHA_ADDCOLOR &&
        color_op <= QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA) {
        if (!qxp_d3d_texture_combine4(
                device, color_op, color_arg1, color_arg2,
                alpha_op, alpha_arg1, alpha_arg2, alpha_arg0, stage)) {
            failure_phase = 0x53UL;
            goto failed;
        }
        return TRUE;
    }
    arguments[0] = QXP_GL_TEXTURE_ENV;
    arguments[1] = QXP_GL_TEXTURE_ENV_MODE;
    arguments[2] = QXP_GL_COMBINE;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                             arguments, 3)) {
        failure_phase = 0x50UL;
        goto failed;
    }
    if (!qxp_d3d_texture_combine(
            device, color_op, color_arg1, color_arg2, color_arg0,
            FALSE, stage)) {
        failure_phase = 0x51UL;
        goto failed;
    }
    if (!qxp_d3d_texture_combine(
            device, alpha_op, alpha_arg1, alpha_arg2, alpha_arg0,
            TRUE, stage)) {
        failure_phase = 0x52UL;
        goto failed;
    }
    return TRUE;

failed:
    if (!device->d3d_texture_failure_logged) {
        volatile ULONG *fifo =
            (volatile ULONG *)device->mesa_fifo.mapped;
        volatile ULONG *data = fifo ? fifo + QXP_MESA_MAX_FIFO : NULL;

        device->d3d_texture_failure_logged = TRUE;
        qxp_debug("QXPMP D3D TEXTURE STATE FAILURE");
        qxp_debug_status(failure_phase);
        qxp_debug_status(stage);
        qxp_debug_status(texture_name);
        qxp_debug_status(color_op);
        qxp_debug_status(color_arg1);
        qxp_debug_status(color_arg2);
        qxp_debug_status(color_arg0);
        qxp_debug_status(alpha_state_op);
        qxp_debug_status(alpha_state_arg1);
        qxp_debug_status(alpha_state_arg2);
        qxp_debug_status(alpha_state_arg0);
        qxp_debug_status(address_u_state);
        qxp_debug_status(address_v_state);
        qxp_debug_status(mag_filter);
        qxp_debug_status(min_filter_state);
        qxp_debug_status(mip_filter);
        qxp_debug_status(mip_lod_bias_bits);
        qxp_debug_status(max_mip_level_state);
        qxp_debug_status(programmable_sample);
        qxp_debug_status(programmable_cube);
        qxp_debug_status(programmable_volume);
        qxp_debug_status(texture != NULL);
        qxp_debug_status(texture ? texture->texture_type : 0);
        qxp_debug_status(texture ? texture->levels : 0);
        qxp_debug_status(request->fvf);
        qxp_debug_status(request->vertex_mode);
        qxp_debug_status(request->fog_enable);
        qxp_debug_status(request->fog_table_mode);
        qxp_debug_status(request->specular_enable);
        for (face = 0; face < 6; ++face)
            qxp_debug_status(texture ? texture->defined_levels[face] : 0);
        qxp_debug_status(fifo ? fifo[0] : 0);
        qxp_debug_status(data ? data[0] : 0);
    }
    return FALSE;
}

static BOOLEAN qxp_d3d_blend_factor(ULONG value, BOOLEAN source,
                                    PULONG factor)
{
    if (!factor) return FALSE;
    switch (value) {
    case 1:
        *factor = QXP_GL_ZERO;
        return TRUE;
    case 2:
        *factor = QXP_GL_ONE;
        return TRUE;
    case 3:
        *factor = QXP_GL_SRC_COLOR;
        return TRUE;
    case 4:
        *factor = QXP_GL_ONE_MINUS_SRC_COLOR;
        return TRUE;
    case 5:
        *factor = QXP_GL_SRC_ALPHA;
        return TRUE;
    case 6:
        *factor = QXP_GL_ONE_MINUS_SRC_ALPHA;
        return TRUE;
    case 7:
        *factor = QXP_GL_DST_ALPHA;
        return TRUE;
    case 8:
        *factor = QXP_GL_ONE_MINUS_DST_ALPHA;
        return TRUE;
    case 9:
        *factor = QXP_GL_DST_COLOR;
        return TRUE;
    case 10:
        *factor = QXP_GL_ONE_MINUS_DST_COLOR;
        return TRUE;
    case 11:
        if (!source) return FALSE;
        *factor = QXP_GL_SRC_ALPHA_SATURATE;
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN qxp_d3d_blend_operation(ULONG value, PULONG operation)
{
    if (!operation) return FALSE;
    switch (value) {
    case 1:
        *operation = QXP_GL_FUNC_ADD;
        return TRUE;
    case 2:
        *operation = QXP_GL_FUNC_SUBTRACT;
        return TRUE;
    case 3:
        *operation = QXP_GL_FUNC_REVERSE_SUBTRACT;
        return TRUE;
    case 4:
        *operation = QXP_GL_MIN;
        return TRUE;
    case 5:
        *operation = QXP_GL_MAX;
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN qxp_d3d_stencil_operation(ULONG value, PULONG operation)
{
    if (!operation) return FALSE;
    switch (value) {
    case 1:
        *operation = QXP_GL_KEEP;
        return TRUE;
    case 2:
        *operation = QXP_GL_ZERO;
        return TRUE;
    case 3:
        *operation = QXP_GL_REPLACE;
        return TRUE;
    case 4:
        *operation = QXP_GL_INCR;
        return TRUE;
    case 5:
        *operation = QXP_GL_DECR;
        return TRUE;
    case 6:
        *operation = QXP_GL_INVERT;
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN qxp_d3d_primitive_parameters(ULONG primitive_type,
                                             PULONG gl_primitive,
                                             PULONG primitive_size)
{
    if (!gl_primitive || !primitive_size) return FALSE;
    if (primitive_type == 1) {
        *gl_primitive = QXP_GL_POINTS;
        *primitive_size = 1;
        return TRUE;
    }
    if (primitive_type == 2) {
        *gl_primitive = QXP_GL_LINES;
        *primitive_size = 2;
        return TRUE;
    }
    if (primitive_type == 4) {
        *gl_primitive = QXP_GL_TRIANGLES;
        *primitive_size = 3;
        return TRUE;
    }
    return FALSE;
}

#ifdef QXP_VERBOSE_TRACE
static BOOLEAN qxp_d3d_trace_ordinal(ULONG ordinal)
{
    if (ordinal <= 16) return TRUE;
    return ordinal && !(ordinal & (ordinal - 1));
}

typedef struct qxp_d3d_gl_trace_output {
    ULONG count;
    ULONG padding;
    ULONG values[32];
} QXP_D3D_GL_TRACE_OUTPUT;

static void qxp_d3d_trace_gl_query(
    QXP_DEVICE_EXTENSION *device, ULONG function, ULONG parameter,
    ULONG words_per_value, ULONG maximum_words)
{
    QXP_D3D_GL_TRACE_OUTPUT output;
    ULONG arguments[2];
    ULONG words;
    ULONG index;

    if (!device || !words_per_value || !maximum_words ||
        maximum_words > 32) return;
    VideoPortZeroMemory(&output, sizeof(output));
    arguments[0] = parameter;
    arguments[1] = 0;
    if (!qxp_d3d_call_output(
            device, function, arguments, 2, &output, sizeof(output))) {
        qxp_debug("QXPMP D3D GL QUERY-");
        qxp_debug_status(function);
        qxp_debug_status(parameter);
        return;
    }
    words = output.count;
    if (words > maximum_words / words_per_value)
        words = maximum_words / words_per_value;
    words *= words_per_value;
    qxp_debug("QXPMP D3D GL QUERY");
    qxp_debug_status(function);
    qxp_debug_status(parameter);
    qxp_debug_status(output.count);
    for (index = 0; index < words; ++index)
        qxp_debug_status(output.values[index]);
}

static void qxp_d3d_trace_gl_state(QXP_DEVICE_EXTENSION *device)
{
    /* MESAGL's frozen function ordinals from mglfunci.h. */
    const ULONG get_boolean_v = 0x000002cbUL;
    const ULONG get_double_v = 0x000002fbUL;
    const ULONG get_float_v = 0x00000305UL;
    const ULONG get_integer_v = 0x00000329UL;
    QXP_D3D_GL_SCALAR_OUTPUT attachment;
    ULONG attachment_arguments[4];
    ULONG read_arguments[7];
    ULONG status_arguments[1];
    ULONG status = 0;

    qxp_debug("QXPMP D3D GL STATE TRACE");
    status_arguments[0] = QXP_GL_FRAMEBUFFER;
    if (qxp_d3d_call_fbtm_result(
            device, QXP_FENUM_GL_CHECK_FRAMEBUFFER_STATUS,
            status_arguments, 1, NULL, 0, &status)) {
        qxp_debug("QXPMP D3D GL FBO STATUS");
        qxp_debug_status(status);
    } else {
        qxp_debug("QXPMP D3D GL FBO STATUS-");
    }
    VideoPortZeroMemory(&attachment, sizeof(attachment));
    attachment_arguments[0] = QXP_GL_FRAMEBUFFER;
    attachment_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    attachment_arguments[2] = 0x00008cd1UL;
    attachment_arguments[3] = 0;
    if (qxp_d3d_call_output(
            device, 0x0000030eUL, attachment_arguments, 4,
            &attachment, sizeof(attachment)) && attachment.count == 1) {
        qxp_debug("QXPMP D3D GL FBO COLOR NAME");
        qxp_debug_status((ULONG)attachment.value);
    } else {
        qxp_debug("QXPMP D3D GL FBO COLOR NAME-");
    }
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000ba2UL, 1, 4);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000c23UL, 1, 4);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000ba0UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000b40UL, 1, 2);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00008ca6UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00008b8dUL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008620UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008804UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000be2UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000b44UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000c11UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000b71UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000bc0UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000b90UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008c89UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x0000809eUL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x000080a0UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00000bf2UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003000UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003001UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003002UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003003UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003004UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00003005UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000c01UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00000c02UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_double_v, 0x00000b70UL, 2, 4);
    qxp_d3d_trace_gl_query(device, get_float_v, 0x00000b00UL, 1, 4);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008074UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008076UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_boolean_v, 0x00008078UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x0000807aUL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x0000807cUL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00008081UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_integer_v, 0x00008083UL, 1, 1);
    qxp_d3d_trace_gl_query(device, get_double_v, 0x00000ba7UL, 2, 32);
    qxp_d3d_trace_gl_query(device, get_double_v, 0x00000ba6UL, 2, 32);
    if (device->d3d_draw_count == 1UL && device->mesa_fbtm.mapped) {
        read_arguments[0] = 8;
        read_arguments[1] = 56;
        read_arguments[2] = 1;
        read_arguments[3] = 1;
        read_arguments[4] = QXP_GL_BGRA;
        read_arguments[5] = QXP_GL_UNSIGNED_BYTE;
        read_arguments[6] = 0;
        VideoPortZeroMemory(device->mesa_fbtm.mapped, sizeof(ULONG));
        if (qxp_d3d_call_fbtm(
                device, QXP_FENUM_GL_READ_PIXELS,
                read_arguments, 7, NULL, 0)) {
            qxp_debug("QXPMP D3D GL DRAW PIXEL");
            qxp_debug_status(
                *(volatile ULONG *)device->mesa_fbtm.mapped);
        } else {
            qxp_debug("QXPMP D3D GL DRAW PIXEL-");
        }
    }
}

static void qxp_d3d_trace_draw(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_VERTEX *vertices, const UCHAR *vertex_indices,
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data)
{
    ULONG ordinal;
    ULONG vertex;
    ULONG trace_vertices;
    ULONG source_vertex;

    if (!device || !request || !vertices) return;
    ordinal = device->d3d_draw_count + 1;
    if (!qxp_d3d_trace_ordinal(ordinal) && request->fill_mode == 3 &&
        request->primitive_type == 4 &&
        !(request->fvf & QXP_D3DFVF_PSIZE)) return;
    qxp_debug("QXPMP D3D DRAW TRACE");
    qxp_debug_status(ordinal);
    qxp_debug_status(request->primitive_type);
    qxp_debug_status(request->vertex_count);
    qxp_debug_status(request->fvf);
    qxp_debug_status(request->target_width);
    qxp_debug_status(request->target_height);
    qxp_debug_status(request->viewport_x);
    qxp_debug_status(request->viewport_y);
    qxp_debug_status(request->viewport_width);
    qxp_debug_status(request->viewport_height);
    qxp_debug_status(request->z_enable);
    qxp_debug_status(request->z_write_enable);
    qxp_debug_status(request->z_func);
    qxp_debug_status(request->cull_mode);
    qxp_debug_status(request->fill_mode);
    qxp_debug_status(device->d3d_target_texture);
    qxp_debug_status(request->color_write_enable);
    qxp_debug_status(request->alpha_test_enable);
    qxp_debug_status(request->alpha_reference);
    qxp_debug_status(request->alpha_function);
    qxp_debug_status(request->alpha_blend_enable);
    qxp_debug_status(request->source_blend);
    qxp_debug_status(request->destination_blend);
    qxp_debug_status(request->pixel_program_name);
    qxp_debug_status(request->texture_name);
    qxp_debug_status(request->texture1_name);
    trace_vertices =
        request->vertex_count < 3 ? request->vertex_count : 3;
    for (vertex = 0; vertex < trace_vertices; ++vertex) {
        source_vertex = vertex_indices ? vertex_indices[vertex] : vertex;
        qxp_debug_status(vertices[source_vertex].x_bits);
        qxp_debug_status(vertices[source_vertex].y_bits);
        qxp_debug_status(vertices[source_vertex].z_bits);
        qxp_debug_status(vertices[source_vertex].rhw_bits);
        qxp_debug_status(vertices[source_vertex].color);
    }
    qxp_debug_status(request->vertex_mode);
    qxp_debug_status(request->point_size_bits);
    qxp_debug_status(request->point_scale_enable);
    qxp_debug_status(request->point_sprite_enable);
    qxp_debug_status(request->point_size_min_bits);
    qxp_debug_status(request->point_size_max_bits);
    qxp_debug_status(request->point_scale_a_bits);
    qxp_debug_status(request->point_scale_b_bits);
    qxp_debug_status(request->point_scale_c_bits);
    for (vertex = 0; vertex < trace_vertices; ++vertex) {
        source_vertex = vertex_indices ? vertex_indices[vertex] : vertex;
        qxp_debug_status(object_data ?
            object_data[source_vertex].point_size_bits :
            request->point_size_bits);
    }
}
#endif

static BOOLEAN qxp_d3d_set_client_state(QXP_DEVICE_EXTENSION *device,
                                         ULONG state, BOOLEAN enable)
{
    ULONG arguments[1];

    arguments[0] = state;
    return qxp_d3d_fifo_submit(
        device, enable ? QXP_FENUM_GL_ENABLE_CLIENT_STATE :
                         QXP_FENUM_GL_DISABLE_CLIENT_STATE,
        arguments, 1);
}

static BOOLEAN qxp_d3d_program_attribute_format(
    ULONG format, PULONG size, PULONG type, PULONG normalized,
    PULONG bytes)
{
    QXP_PROGRAM_ATTRIBUTE_FORMAT result;

    if (!size || !type || !normalized || !bytes) return FALSE;
    if (!qxp_program_attribute_format(format, &result)) return FALSE;
    *size = result.components;
    *type = result.storage == QXP_PROGRAM_STORAGE_FLOAT ? QXP_GL_FLOAT :
            (result.storage == QXP_PROGRAM_STORAGE_UBYTE ?
             QXP_GL_UNSIGNED_BYTE : QXP_GL_SHORT);
    *normalized = result.normalized;
    *bytes = result.bytes;
    return TRUE;
}

static BOOLEAN qxp_d3d_submit_program_vertex_arrays(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    const UCHAR *vertex_data, ULONG gl_primitive)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    ULONG fifo_start;
    ULONG data_start;
    ULONG pointer_arguments[6];
    ULONG attribute_arguments[5];
    ULONG draw_arguments[3];
    ULONG attribute;
    ULONG size;
    ULONG type;
    ULONG normalized;
    ULONG bytes;
    ULONG failure_phase = 0;
    BOOLEAN active;
    BOOLEAN submitted = FALSE;
    PUCHAR interleaved_data;

    if (!fifo || !request || !program ||
        (program->vertex_data_bytes && !vertex_data)) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    for (attribute = 0; attribute < 16; ++attribute) {
        active = (program->input_mask & (1UL << attribute)) != 0;
        attribute_arguments[0] = attribute;
        if (active) {
            failure_phase = 0x100UL + attribute;
            if (!qxp_d3d_program_attribute_format(
                    program->elements[attribute].format,
                    &size, &type, &normalized, &bytes)) goto done;
            pointer_arguments[0] = attribute;
            pointer_arguments[1] = size;
            pointer_arguments[2] = type;
            pointer_arguments[3] = normalized;
            pointer_arguments[4] = program->vertex_stride;
            pointer_arguments[5] =
                QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                    program->elements[attribute].byte_offset);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB,
                    pointer_arguments, 6)) goto done;
        }
        failure_phase = 0x200UL + attribute;
        if (!qxp_d3d_fifo_submit(
                device, active ?
                    QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB :
                    QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                attribute_arguments, 1)) goto done;
        if (!active &&
            (program->used_inputs & (1UL << attribute))) {
            attribute_arguments[1] = 0;
            attribute_arguments[2] = 0;
            attribute_arguments[3] = 0;
            attribute_arguments[4] = 0;
            failure_phase = 0x280UL + attribute;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB,
                    attribute_arguments, 5)) goto done;
        }
    }
    failure_phase = 0x300UL;
    if (program->vertex_data_bytes) {
        interleaved_data = qxp_d3d_fifo_append_data(
            device, program->vertex_data_bytes);
        if (!interleaved_data) goto done;
        QXP_D3D_MOVE_INTERLEAVED(
            device, interleaved_data, vertex_data,
            program->vertex_data_bytes);
    }
    failure_phase = 0x500UL;
    draw_arguments[0] = gl_primitive;
    draw_arguments[1] = 0;
    draw_arguments[2] = request->vertex_count;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DRAW_ARRAYS,
                             draw_arguments, 3)) goto done;
    if (device->d3d_pending_draws != 0xffffffffUL)
        ++device->d3d_pending_draws;
    if (program->vertex_data_bytes &&
        device->d3d_interleaved_draw_count != 0xffffffffUL)
        ++device->d3d_interleaved_draw_count;
    submitted = TRUE;

done:
    if (!submitted) {
        if (!device->d3d_array_failure_logged) {
            device->d3d_array_failure_logged = TRUE;
            qxp_debug("QXPMP D3D PROGRAM ARRAY FAILURE");
            qxp_debug_status(failure_phase);
            qxp_debug_status(program->program_name);
            qxp_debug_status(program->input_mask);
            qxp_debug_status(program->used_inputs);
            qxp_debug_status(program->written_outputs);
            qxp_debug_status(program->vertex_stride);
            qxp_debug_status(program->vertex_data_bytes);
            qxp_debug_status(request->vertex_count);
            qxp_debug_status(fifo[0] - fifo_start);
            qxp_debug_status(data[0] - data_start);
        }
        fifo[0] = fifo_start;
        data[0] = data_start;
    }
    return submitted;
}

static BOOLEAN qxp_d3d_submit_compact_vertex_arrays(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_COMPACT_VERTEX_PAYLOAD *compact,
    const QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *compact_indexed,
    const UCHAR *vertex_data, const UCHAR *index_data,
    ULONG coordinate_stages, ULONG gl_primitive,
    BOOLEAN configure_arrays)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    QXP_D3D_COMPACT_FVF_LAYOUT layout;
    ULONG fifo_start;
    ULONG data_start;
    ULONG pointer_arguments[6];
    ULONG attribute_arguments[5];
    ULONG draw_arguments[4];
    ULONG attribute;
    ULONG stage;
    ULONG coordinate_index;
    ULONG offset;
    ULONG vertex_count;
    ULONG vertex_data_bytes;
    ULONG index_count = 0;
    ULONG index_data_bytes = 0;
    ULONG aligned_index_bytes = 0;
    ULONG combined_data_bytes = 0;
    ULONG failure_phase = 0;
    PUCHAR interleaved_data;
    BOOLEAN indexed;
    BOOLEAN active;
    BOOLEAN submitted = FALSE;

    if (!fifo || !request || !vertex_data ||
        (!compact && !compact_indexed) || (compact && compact_indexed) ||
        !qxp_packet_compact_fvf_layout(request->fvf, &layout))
        return FALSE;
    indexed = compact_indexed != NULL;
    if (indexed) {
        if (!index_data ||
            compact_indexed->vertex_stride != layout.vertex_stride ||
            compact_indexed->vertex_count >
                0xffffffffUL / layout.vertex_stride ||
            compact_indexed->vertex_data_bytes !=
                compact_indexed->vertex_count * layout.vertex_stride ||
            compact_indexed->index_count != request->vertex_count ||
            compact_indexed->index_data_bytes !=
                compact_indexed->index_count)
            return FALSE;
        vertex_count = compact_indexed->vertex_count;
        vertex_data_bytes = compact_indexed->vertex_data_bytes;
        index_count = compact_indexed->index_count;
        index_data_bytes = compact_indexed->index_data_bytes;
        aligned_index_bytes =
            (index_data_bytes + QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
            ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
        if (aligned_index_bytes > 0xffffffffUL - vertex_data_bytes ||
            vertex_data != index_data + aligned_index_bytes)
            return FALSE;
        combined_data_bytes = aligned_index_bytes + vertex_data_bytes;
    } else {
        if (compact->vertex_stride != layout.vertex_stride ||
            request->vertex_count > 0xffffffffUL / layout.vertex_stride ||
            compact->vertex_data_bytes !=
                request->vertex_count * layout.vertex_stride)
            return FALSE;
        vertex_count = request->vertex_count;
        vertex_data_bytes = compact->vertex_data_bytes;
    }
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];

    if (configure_arrays) {
        for (attribute = 0; attribute < 16; ++attribute) {
            attribute_arguments[0] = attribute;
            failure_phase = 0x100UL + attribute;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    attribute_arguments, 1)) goto done;
        }

        pointer_arguments[0] = 0;
        pointer_arguments[1] = 4;
        pointer_arguments[2] = QXP_GL_FLOAT;
        pointer_arguments[3] = FALSE;
        pointer_arguments[4] = layout.vertex_stride;
        pointer_arguments[5] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(0);
        attribute_arguments[0] = 0;
        failure_phase = 0x200UL;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB,
                pointer_arguments, 6) ||
            !qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB,
                attribute_arguments, 1)) goto done;

        for (attribute = 3; attribute <= 4; ++attribute) {
            active = attribute == 3 ?
                (request->fvf & QXP_D3DFVF_DIFFUSE) != 0 :
                (request->fvf & QXP_D3DFVF_SPECULAR) != 0;
            offset = attribute == 3 ? layout.diffuse_offset :
                                      layout.specular_offset;
            attribute_arguments[0] = attribute;
            failure_phase = 0x210UL + attribute;
            if (active) {
                pointer_arguments[0] = attribute;
                pointer_arguments[1] = 4;
                pointer_arguments[2] = QXP_GL_UNSIGNED_BYTE;
                pointer_arguments[3] = TRUE;
                pointer_arguments[4] = layout.vertex_stride;
                pointer_arguments[5] =
                    QXP_D3D_INTERLEAVED_ARRAY_HANDLE(offset);
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB,
                        pointer_arguments, 6) ||
                    !qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB,
                        attribute_arguments, 1)) goto done;
            } else {
                attribute_arguments[1] = attribute == 3 ?
                                         0x3f800000UL : 0;
                attribute_arguments[2] = attribute_arguments[1];
                attribute_arguments[3] = attribute_arguments[1];
                attribute_arguments[4] = attribute_arguments[1];
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB,
                        attribute_arguments, 5)) goto done;
            }
        }


        for (stage = 0; stage < 4; ++stage) {
            active = (coordinate_stages & (1UL << stage)) != 0;
            attribute_arguments[0] = 8 + stage;
            failure_phase = 0x220UL + stage;
            if (!active) continue;
            coordinate_index = (!stage ? request->texture_coord_index :
                (stage == 1 ? request->texture1_coord_index :
                 (stage == 2 ? request->texture2_coord_index :
                               request->texture3_coord_index))) &
                QXP_D3D_TCI_INDEX_MASK;
            if (coordinate_index >= layout.texture_count) goto done;
            pointer_arguments[0] = 8 + stage;
            pointer_arguments[1] = 2;
            pointer_arguments[2] = QXP_GL_FLOAT;
            pointer_arguments[3] = FALSE;
            pointer_arguments[4] = layout.vertex_stride;
            pointer_arguments[5] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.texture_offsets[coordinate_index]);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB,
                    pointer_arguments, 6) ||
                !qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    attribute_arguments, 1)) goto done;
        }
    }

    failure_phase = indexed ? 0x2f0UL : 0x300UL;
    interleaved_data = qxp_d3d_fifo_append_data(
        device, indexed ? combined_data_bytes : vertex_data_bytes);
    if (!interleaved_data) goto done;
    QXP_D3D_MOVE_INTERLEAVED(
        device, interleaved_data, indexed ? index_data : vertex_data,
        indexed ? combined_data_bytes : vertex_data_bytes);
    draw_arguments[0] = gl_primitive;
    failure_phase = 0x400UL;
    if (indexed) {
        draw_arguments[1] = index_count;
        draw_arguments[2] = QXP_GL_UNSIGNED_BYTE;
        draw_arguments[3] = 0;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DRAW_ELEMENTS,
                                 draw_arguments, 4)) goto done;
    } else {
        draw_arguments[1] = 0;
        draw_arguments[2] = request->vertex_count;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DRAW_ARRAYS,
                                 draw_arguments, 3)) goto done;
    }
    if (device->d3d_pending_draws != 0xffffffffUL)
        ++device->d3d_pending_draws;
    if (device->d3d_interleaved_draw_count != 0xffffffffUL)
        ++device->d3d_interleaved_draw_count;
    if (device->d3d_compact_draw_count != 0xffffffffUL)
        ++device->d3d_compact_draw_count;
    if (indexed &&
        device->d3d_compact_indexed_draw_count != 0xffffffffUL)
        ++device->d3d_compact_indexed_draw_count;
    submitted = TRUE;

done:
    if (!submitted) {
        if (!device->d3d_array_failure_logged) {
            device->d3d_array_failure_logged = TRUE;
            qxp_debug("QXPMP D3D COMPACT ARRAY FAILURE");
            qxp_debug_status(failure_phase);
            qxp_debug_status(request->fvf);
            qxp_debug_status(layout.vertex_stride);
            qxp_debug_status(vertex_data_bytes);
            qxp_debug_status(vertex_count);
            qxp_debug_status(index_count);
            qxp_debug_status(request->vertex_count);
            qxp_debug_status(fifo[0] - fifo_start);
            qxp_debug_status(data[0] - data_start);
        }
        fifo[0] = fifo_start;
        data[0] = data_start;
    }
    return submitted;
}

static BOOLEAN qxp_d3d_submit_raw_indexed_object_arrays(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *raw,
    const UCHAR *vertex_data, const UCHAR *index_data,
    ULONG coordinate_stages, ULONG gl_primitive,
    BOOLEAN configure_arrays)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    QXP_D3D_RAW_OBJECT_FVF_LAYOUT layout;
    ULONG pointer_arguments[4];
    ULONG attribute_arguments[5];
    ULONG client_arguments[1];
    ULONG draw_arguments[4];
    ULONG aligned_index_bytes;
    ULONG combined_data_bytes;
    ULONG fifo_start;
    ULONG data_start;
    ULONG stage;
    ULONG failure_phase = 0;
    PUCHAR interleaved_data;
    BOOLEAN active;
    BOOLEAN submitted = FALSE;

    if (!fifo || !request || !raw || !vertex_data || !index_data ||
        coordinate_stages != 1 ||
        !qxp_packet_raw_indexed_object_fvf_layout(
            request->fvf, &layout) ||
        raw->vertex_stride != layout.vertex_stride ||
        raw->vertex_data_bytes !=
            raw->vertex_count * layout.vertex_stride ||
        raw->index_count != request->vertex_count ||
        raw->index_data_bytes != raw->index_count)
        return FALSE;
    aligned_index_bytes =
        (raw->index_data_bytes +
         QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    if (aligned_index_bytes >
        0xffffffffUL - raw->vertex_data_bytes)
        return FALSE;
    combined_data_bytes = aligned_index_bytes + raw->vertex_data_bytes;
    if (vertex_data != index_data + aligned_index_bytes) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];

    if (configure_arrays) {
        for (stage = 0; stage < 16; ++stage) {
            client_arguments[0] = stage;
            failure_phase = 0x100UL + stage;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    client_arguments, 1)) goto done;
        }

        failure_phase = 0x200UL;
        client_arguments[0] = QXP_GL_COLOR_ARRAY;
        if (request->fvf & QXP_D3DFVF_DIFFUSE) {
            pointer_arguments[0] = QXP_GL_BGRA;
            pointer_arguments[1] = QXP_GL_UNSIGNED_BYTE;
            pointer_arguments[2] = layout.vertex_stride;
            pointer_arguments[3] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.diffuse_offset);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_COLOR_POINTER,
                    pointer_arguments, 4) ||
                !qxp_d3d_set_client_state(
                    device, QXP_GL_COLOR_ARRAY, TRUE)) goto done;
        } else {
            attribute_arguments[0] = 3;
            attribute_arguments[1] = 0x3f800000UL;
            attribute_arguments[2] = 0x3f800000UL;
            attribute_arguments[3] = 0x3f800000UL;
            attribute_arguments[4] = 0x3f800000UL;
            if (!qxp_d3d_set_client_state(
                    device, QXP_GL_COLOR_ARRAY, FALSE) ||
                !qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB,
                    attribute_arguments, 5)) goto done;
        }

        failure_phase = 0x210UL;
        if (request->fvf & QXP_D3DFVF_NORMAL) {
            pointer_arguments[0] = QXP_GL_FLOAT;
            pointer_arguments[1] = layout.vertex_stride;
            pointer_arguments[2] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.normal_offset);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_NORMAL_POINTER,
                    pointer_arguments, 3) ||
                !qxp_d3d_set_client_state(
                    device, QXP_GL_NORMAL_ARRAY, TRUE)) goto done;
        } else if (!qxp_d3d_set_client_state(
                       device, QXP_GL_NORMAL_ARRAY, FALSE)) {
            goto done;
        }

        for (stage = 0; stage < 5; ++stage) {
            active = stage < 4 &&
                     (coordinate_stages & (1UL << stage)) != 0;
            client_arguments[0] = QXP_GL_TEXTURE0 + stage;
            failure_phase = 0x220UL + stage;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_CLIENT_ACTIVE_TEXTURE,
                    client_arguments, 1)) goto done;
            if (active) {
                pointer_arguments[0] = 2;
                pointer_arguments[1] = QXP_GL_FLOAT;
                pointer_arguments[2] = layout.vertex_stride;
                pointer_arguments[3] =
                    QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                        layout.texture_offset);
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_TEX_COORD_POINTER,
                        pointer_arguments, 4)) goto done;
            }
            if (!qxp_d3d_set_client_state(
                    device, QXP_GL_TEXTURE_COORD_ARRAY, active)) goto done;
            if (!active) {
                attribute_arguments[0] = 8 + stage;
                attribute_arguments[1] = 0;
                attribute_arguments[2] = 0;
                attribute_arguments[3] = 0;
                attribute_arguments[4] = 0x3f800000UL;
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB,
                        attribute_arguments, 5)) goto done;
            }
        }

        failure_phase = 0x230UL;
        pointer_arguments[0] = 3;
        pointer_arguments[1] = QXP_GL_FLOAT;
        pointer_arguments[2] = layout.vertex_stride;
        pointer_arguments[3] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(0);
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_VERTEX_POINTER,
                pointer_arguments, 4) ||
            !qxp_d3d_set_client_state(
                device, QXP_GL_VERTEX_ARRAY, TRUE)) goto done;

        failure_phase = 0x240UL;
        attribute_arguments[0] = QXP_GL_SECONDARY_COLOR_ATTRIBUTE;
        attribute_arguments[1] = 0;
        attribute_arguments[2] = 0;
        attribute_arguments[3] = 0;
        attribute_arguments[4] = 0;
        if (!qxp_d3d_set_client_state(
                device, QXP_GL_SECONDARY_COLOR_ARRAY, FALSE) ||
            !qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_VERTEX_ATTRIB_4F_ARB,
                attribute_arguments, 5) ||
            !qxp_d3d_set_client_state(
                device, QXP_GL_FOG_COORDINATE_ARRAY, FALSE)) goto done;
    }

    failure_phase = 0x300UL;
    interleaved_data = qxp_d3d_fifo_append_data(
        device, combined_data_bytes);
    if (!interleaved_data) goto done;
    QXP_D3D_MOVE_INTERLEAVED(
        device, interleaved_data, index_data, combined_data_bytes);
    draw_arguments[0] = gl_primitive;
    draw_arguments[1] = raw->index_count;
    draw_arguments[2] = QXP_GL_UNSIGNED_BYTE;
    draw_arguments[3] = 0;
    failure_phase = 0x400UL;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DRAW_ELEMENTS,
            draw_arguments, 4)) goto done;
    if (device->d3d_pending_draws != 0xffffffffUL)
        ++device->d3d_pending_draws;
    if (device->d3d_interleaved_draw_count != 0xffffffffUL)
        ++device->d3d_interleaved_draw_count;
    if (device->d3d_indexed_element_draw_count != 0xffffffffUL)
        ++device->d3d_indexed_element_draw_count;
    if (device->d3d_raw_indexed_object_draw_count != 0xffffffffUL)
        ++device->d3d_raw_indexed_object_draw_count;
    submitted = TRUE;

done:
    if (!submitted) {
        if (!device->d3d_array_failure_logged) {
            device->d3d_array_failure_logged = TRUE;
            qxp_debug("QXPMP D3D RAW OBJECT ARRAY FAILURE");
            qxp_debug_status(failure_phase);
            qxp_debug_status(request->fvf);
            qxp_debug_status(layout.vertex_stride);
            qxp_debug_status(raw->vertex_count);
            qxp_debug_status(raw->index_count);
            qxp_debug_status(fifo[0] - fifo_start);
            qxp_debug_status(data[0] - data_start);
        }
        fifo[0] = fifo_start;
        data[0] = data_start;
    }
    return submitted;
}

#define QXP_D3D_ARRAY_OFFSET_DISABLED 0xffffffffUL

typedef struct qxp_d3d_interleaved_vertex_layout {
    ULONG stride;
    ULONG color_offset;
    ULONG normal_offset;
    ULONG texcoord_offset[5];
    ULONG vertex_offset;
    ULONG secondary_offset;
    ULONG fog_offset;
} QXP_D3D_INTERLEAVED_VERTEX_LAYOUT;

static BOOLEAN qxp_d3d_interleaved_vertex_layout(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data,
    ULONG coordinate_stages, BOOLEAN fog_array_active,
    QXP_D3D_INTERLEAVED_VERTEX_LAYOUT *layout)
{
    ULONG stage;
    ULONG offset;

    if (!request || !layout) return FALSE;
    offset = 0;
    layout->color_offset = offset;
    offset += 4UL;
    layout->normal_offset = QXP_D3D_ARRAY_OFFSET_DISABLED;
    if (object_data && (request->fvf & QXP_D3DFVF_NORMAL)) {
        layout->normal_offset = offset;
        offset += 3UL * sizeof(ULONG);
    }
    for (stage = 0; stage < 5; ++stage) {
        BOOLEAN active = stage < 4 ?
            (coordinate_stages & (1UL << stage)) != 0 :
            coordinate_stages != 0;

        layout->texcoord_offset[stage] = QXP_D3D_ARRAY_OFFSET_DISABLED;
        if (!active) continue;
        layout->texcoord_offset[stage] = offset;
        offset += 4UL * sizeof(ULONG);
    }
    layout->vertex_offset = offset;
    offset += 3UL * sizeof(ULONG);
    layout->secondary_offset = QXP_D3D_ARRAY_OFFSET_DISABLED;
    if (request->fvf & QXP_D3DFVF_SPECULAR) {
        layout->secondary_offset = offset;
        offset += 4UL;
    }
    layout->fog_offset = QXP_D3D_ARRAY_OFFSET_DISABLED;
    if (fog_array_active) {
        layout->fog_offset = offset;
        offset += sizeof(ULONG);
    }
    layout->stride = offset;
    return offset != 0 && offset < QXP_D3D_ARRAY_HANDLE_STRIDE;
}

static BOOLEAN qxp_d3d_append_vertex_array_data(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_VERTEX *vertices, const UCHAR *vertex_indices,
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data,
    const QXP_D3D_INTERLEAVED_VERTEX_LAYOUT *layout,
    const ULONG *table_fog_columns,
    BOOLEAN table_fog, ULONG primitive_size, ULONG first, ULONG count,
    PULONG failure_reason)
{
    ULONG stage;
    ULONG vertex;
    ULONG source_vertex;
    ULONG flat_vertex;
    ULONG coordinate_index;
    ULONG coordinate_mode;
    ULONG dimension;
    ULONG rhw;
    ULONG color;
    ULONG specular;
    ULONG bytes;
    PULONG words;
    PUCHAR output;
    PUCHAR record;
    BOOLEAN projected;

    if (failure_reason) *failure_reason = 0;
    if (!device || !request || !vertices || !layout || !layout->stride ||
        !count || count > 0xffffffffUL / layout->stride ||
        (object_data && table_fog && !table_fog_columns) ||
        first > request->vertex_count ||
        count > request->vertex_count - first) {
        if (failure_reason) *failure_reason = 1;
        return FALSE;
    }
    bytes = count * layout->stride;
    output = qxp_d3d_fifo_append_data(device, bytes);
    if (!output) {
        if (failure_reason) *failure_reason = 2;
        return FALSE;
    }

    for (vertex = first; vertex < first + count; ++vertex) {
        source_vertex = vertex_indices ? vertex_indices[vertex] : vertex;
        flat_vertex = request->shade_mode == 1 ?
                      vertex - vertex % primitive_size : vertex;
        if (vertex_indices) flat_vertex = vertex_indices[flat_vertex];
        record = output + (vertex - first) * layout->stride;

        color = vertices[flat_vertex].color;
        record[layout->color_offset] = (UCHAR)(color >> 16);
        record[layout->color_offset + 1UL] = (UCHAR)(color >> 8);
        record[layout->color_offset + 2UL] = (UCHAR)color;
        record[layout->color_offset + 3UL] = (UCHAR)(color >> 24);

        if (layout->normal_offset != QXP_D3D_ARRAY_OFFSET_DISABLED) {
            words = (PULONG)(record + layout->normal_offset);
            words[0] = object_data[source_vertex].normal[0];
            words[1] = object_data[source_vertex].normal[1];
            words[2] = object_data[source_vertex].normal[2];
        }

        for (stage = 0; stage < 5; ++stage) {
            if (layout->texcoord_offset[stage] ==
                QXP_D3D_ARRAY_OFFSET_DISABLED) continue;
            words = (PULONG)(record + layout->texcoord_offset[stage]);
            words[0] = 0;
            words[1] = 0;
            words[2] = 0;
            words[3] = object_data ? 0x3f800000UL :
                                     vertices[source_vertex].rhw_bits;
            if (stage == 4) continue;
            coordinate_index = !stage ? request->texture_coord_index :
                (stage == 1 ? request->texture1_coord_index :
                 (stage == 2 ? request->texture2_coord_index :
                               request->texture3_coord_index));
            if (object_data) {
                coordinate_mode =
                    coordinate_index & QXP_D3D_TCI_MODE_MASK;
                if (coordinate_mode != QXP_D3D_TCI_PASSTHRU) continue;
                coordinate_index &= QXP_D3D_TCI_INDEX_MASK;
                dimension =
                    request->texture_coord_dimensions[coordinate_index];
                words[0] =
                    vertices[source_vertex].texcoord[coordinate_index][0];
                if (dimension >= 2)
                    words[1] =
                        vertices[source_vertex]
                            .texcoord[coordinate_index][1];
                if (dimension >= 3)
                    words[2] =
                        vertices[source_vertex]
                            .texcoord[coordinate_index][2];
                if (dimension == 4)
                    words[3] =
                        vertices[source_vertex]
                            .texcoord[coordinate_index][3];
                continue;
            }
            coordinate_index &= QXP_D3D_TCI_INDEX_MASK;
            dimension = request->texture_coord_dimensions[coordinate_index];
            rhw = vertices[source_vertex].rhw_bits;
            if (!qxp_float_bits_finite(rhw)) {
                if (!device->d3d_rhw_clamp_logged) {
                    device->d3d_rhw_clamp_logged = TRUE;
                    qxp_debug("QXPMP D3D RHW CLAMP");
                    qxp_debug_status(vertex);
                    qxp_debug_status(rhw);
                }
                rhw = 0x3f800000UL;
            }
            words[3] = rhw;
            if (!qxp_float_multiply(
                    vertices[source_vertex].texcoord[coordinate_index][0],
                    rhw, &words[0])) {
                if (failure_reason) *failure_reason = 0x20UL + stage;
                return FALSE;
            }
            if (dimension >= 2 &&
                !qxp_float_multiply(
                    vertices[source_vertex].texcoord[coordinate_index][1],
                    rhw, &words[1])) {
                if (failure_reason) *failure_reason = 0x30UL + stage;
                return FALSE;
            }
            if (dimension >= 3 &&
                !qxp_float_multiply(
                    vertices[source_vertex].texcoord[coordinate_index][2],
                    rhw, &words[2])) {
                if (failure_reason) *failure_reason = 0x40UL + stage;
                return FALSE;
            }
            projected = (request->texture_projected_stages &
                         (1UL << stage)) != 0;
            if (projected &&
                !qxp_float_multiply(
                    vertices[source_vertex]
                        .texcoord[coordinate_index][dimension - 1],
                    rhw, &words[3])) {
                if (failure_reason) *failure_reason = 0x50UL + stage;
                return FALSE;
            }
        }

        words = (PULONG)(record + layout->vertex_offset);
        words[0] = vertices[source_vertex].x_bits;
        words[1] = vertices[source_vertex].y_bits;
        words[2] = vertices[source_vertex].z_bits;

        if (layout->secondary_offset != QXP_D3D_ARRAY_OFFSET_DISABLED) {
            specular = vertices[flat_vertex].specular;
            record[layout->secondary_offset] =
                (UCHAR)(specular >> 16);
            record[layout->secondary_offset + 1UL] =
                (UCHAR)(specular >> 8);
            record[layout->secondary_offset + 2UL] = (UCHAR)specular;
            record[layout->secondary_offset + 3UL] =
                (UCHAR)(specular >> 24);
        }

        if (layout->fog_offset != QXP_D3D_ARRAY_OFFSET_DISABLED) {
            words = (PULONG)(record + layout->fog_offset);
            if (table_fog && object_data) {
                if (!qxp_float_project_depth(
                        &vertices[source_vertex].x_bits,
                        table_fog_columns, &words[0])) {
                    if (failure_reason) *failure_reason = 0x90UL;
                    return FALSE;
                }
            } else if (table_fog) {
                words[0] = vertices[source_vertex].z_bits;
            } else if (request->fvf & QXP_D3DFVF_FOG) {
                if (!qxp_float_add(
                        0x3f800000UL,
                        vertices[source_vertex].fog_bits ^ 0x80000000UL,
                        &words[0])) {
                    if (failure_reason) *failure_reason = 0x91UL;
                    return FALSE;
                }
            } else {
                words[0] = qxp_float_from_unorm8(
                    (UCHAR)(0xffUL -
                        (vertices[source_vertex].specular >> 24)));
            }
            if (table_fog && request->fog_table_mode >= 1 &&
                request->fog_table_mode <= 2 && vertex == first &&
                !device->d3d_table_fog_logged[
                    request->fog_table_mode - 1]) {
                device->d3d_table_fog_logged[
                    request->fog_table_mode - 1] = TRUE;
                qxp_debug("QXPMP D3D TABLE FOG INPUT");
                qxp_debug_status(request->fog_table_mode);
                qxp_debug_status(request->fog_density_bits);
                qxp_debug_status(request->fog_start_bits);
                qxp_debug_status(request->fog_end_bits);
                qxp_debug_status(vertices[source_vertex].z_bits);
                qxp_debug_status(words[0]);
            }
        }
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_point_size_value(
    const QXP_D3D_DRAW_REQUEST *request, ULONG source, PULONG result)
{
    ULONG viewport_height;

    if (!request || !result) return FALSE;
    if (!request->point_scale_enable) {
        *result = source;
        return TRUE;
    }
    return qxp_float_from_uint(request->viewport_height, &viewport_height) &&
           qxp_float_multiply(source, viewport_height, result);
}

/* Clamp a positive float-bits point size to the advertised MaxPointSize cap
 * (64.0f = 0x42800000). The device clamps to its cap rather than rejecting
 * large D3DRS_POINTSIZE / D3DRS_POINTSIZE_MAX values (see
 * qxp_d3d_point_state_valid); same-sign positive floats compare as
 * integers, and validation already rejected negative/NaN inputs. */
static ULONG qxp_d3d_clamp_point_size_bits(ULONG bits)
{
    return bits > 0x42800000UL ? 0x42800000UL : bits;
}

static BOOLEAN qxp_d3d_apply_point_state(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request)
{
    ULONG arguments[2];
    ULONG attenuation[3];
    ULONG point_size;

    if (!device || !request ||
        !qxp_d3d_point_size_value(request, request->point_size_bits,
                                  &point_size)) return FALSE;
    arguments[0] = QXP_GL_POINT_SIZE_MIN;
    arguments[1] = request->point_scale_enable ?
        qxp_d3d_clamp_point_size_bits(request->point_size_min_bits) :
        0x3f800000UL;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_PARAMETER_F,
                             arguments, 2)) return FALSE;
    arguments[0] = QXP_GL_POINT_SIZE_MAX;
    arguments[1] = request->point_scale_enable ?
        qxp_d3d_clamp_point_size_bits(request->point_size_max_bits) :
        0x42800000UL;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_PARAMETER_F,
                             arguments, 2)) return FALSE;
    /* D3D has no small-point alpha fade state. A zero threshold keeps the
     * standard size clamp without allowing OpenGL to attenuate color alpha. */
    arguments[0] = QXP_GL_POINT_FADE_THRESHOLD_SIZE;
    arguments[1] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_PARAMETER_F,
                             arguments, 2)) return FALSE;
    arguments[0] = QXP_GL_POINT_DISTANCE_ATTENUATION;
    arguments[1] = 0;
    attenuation[0] = request->point_scale_enable ?
        request->point_scale_a_bits : 0x3f800000UL;
    attenuation[1] = request->point_scale_enable ?
        request->point_scale_b_bits : 0;
    attenuation[2] = request->point_scale_enable ?
        request->point_scale_c_bits : 0;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_POINT_PARAMETER_FV, arguments, 2,
            attenuation, sizeof(attenuation))) return FALSE;
    arguments[0] = point_size;
    return qxp_d3d_fifo_submit(
        device, QXP_FENUM_GL_POINT_SIZE, arguments, 1);
}

static BOOLEAN qxp_d3d_apply_point_sprite_state(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request)
{
    ULONG active_arguments[1];
    ULONG origin_arguments[2];
    ULONG sprite_arguments[1];
    ULONG texture_arguments[3];
    ULONG stage;
    BOOLEAN enable;

    if (!device || !request) return FALSE;
    enable = request->point_sprite_enable && request->primitive_type == 1;
    sprite_arguments[0] = QXP_GL_POINT_SPRITE;
    if (!qxp_d3d_fifo_submit(
            device, enable ? QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            sprite_arguments, 1)) return FALSE;

    /* ARB point sprites use top-left texture coordinates. Presentation
     * targets share that D3D orientation, while ordinary render textures map
     * D3D's top row to the lower OpenGL framebuffer row so the completed
     * texture can later be sampled without a flip. Core GL's origin control
     * keeps generated coordinates standards-correct in both targets. */
    origin_arguments[0] = QXP_GL_POINT_SPRITE_COORD_ORIGIN;
    origin_arguments[1] = device->d3d_target_texture &&
                          !device->d3d_target_present ?
                          QXP_GL_LOWER_LEFT : QXP_GL_UPPER_LEFT;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_PARAMETER_I,
                             origin_arguments, 2)) return FALSE;

    texture_arguments[0] = QXP_GL_POINT_SPRITE;
    texture_arguments[1] = QXP_GL_COORD_REPLACE;
    texture_arguments[2] = enable ? TRUE : FALSE;
    for (stage = 0; stage < 4; ++stage) {
        active_arguments[0] = QXP_GL_TEXTURE0 + stage;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                                 active_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_TEX_ENV_I,
                                 texture_arguments, 3)) return FALSE;
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_submit_vertex_arrays(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_VERTEX *vertices, const UCHAR *vertex_indices,
    ULONG source_vertex_count,
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data,
    const ULONG *table_fog_columns,
    ULONG coordinate_stages, BOOLEAN table_fog, BOOLEAN fog_array_active,
    ULONG primitive_size, ULONG gl_primitive, BOOLEAN generic_secondary,
    BOOLEAN configure_arrays)
{
    volatile ULONG *fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    volatile ULONG *data;
    QXP_D3D_INTERLEAVED_VERTEX_LAYOUT layout;
    ULONG fifo_start;
    ULONG data_start;
    ULONG pointer_arguments[6];
    ULONG client_arguments[1];
    ULONG draw_arguments[4];
    ULONG point_arguments[1];
    ULONG stage;
    ULONG run_first;
    ULONG run_end;
    ULONG run_first_source;
    ULONG run_end_source;
    ULONG submitted_draws = 0;
    ULONG failure_phase = 1;
    ULONG failure_reason = 0;
    PUCHAR index_output;
    BOOLEAN active;
    BOOLEAN secondary_active;
    BOOLEAN normal_active;
    BOOLEAN submitted = FALSE;
    BOOLEAN direct_indexed;

    if (!fifo || !request || !vertices || !source_vertex_count ||
        source_vertex_count > request->vertex_count) return FALSE;
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    direct_indexed = vertex_indices && request->shade_mode != 1 &&
        !(object_data && gl_primitive == QXP_GL_POINTS &&
          (request->fvf & QXP_D3DFVF_PSIZE));

    secondary_active = (request->fvf & QXP_D3DFVF_SPECULAR) != 0;
    normal_active = object_data &&
                    (request->fvf & QXP_D3DFVF_NORMAL) != 0;
    if (!qxp_d3d_interleaved_vertex_layout(
            request, object_data, coordinate_stages, fog_array_active,
            &layout)) return FALSE;
    if (configure_arrays) {
        /* Generic attributes alias the compatibility arrays in Mesa's FIFO
         * state. Clear all aliases first so a preceding programmable draw
         * cannot leave Weight, GenAttrib, or texture units 5..7 enabled. */
        for (stage = 0; stage < 16; ++stage) {
            client_arguments[0] = stage;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    client_arguments, 1)) goto done;
        }
        /* Every enabled array names one field in the single packed payload.
         * The host validates the common stride, then publishes it with one
         * cache copy instead of walking one data block per semantic. */
        pointer_arguments[0] = 4;
        pointer_arguments[1] = QXP_GL_UNSIGNED_BYTE;
        pointer_arguments[2] = layout.stride;
        pointer_arguments[3] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
            layout.color_offset);
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_COLOR_POINTER,
                                 pointer_arguments, 4) ||
            !qxp_d3d_set_client_state(device, QXP_GL_COLOR_ARRAY, TRUE))
            goto done;

        if (normal_active) {
            pointer_arguments[0] = QXP_GL_FLOAT;
            pointer_arguments[1] = layout.stride;
            pointer_arguments[2] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.normal_offset);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_NORMAL_POINTER,
                                     pointer_arguments, 3)) goto done;
        }
        if (!qxp_d3d_set_client_state(
                device, QXP_GL_NORMAL_ARRAY, normal_active)) goto done;

        for (stage = 0; stage < 5; ++stage) {
            active = stage < 4 ?
                (coordinate_stages & (1UL << stage)) != 0 :
                coordinate_stages != 0;
            client_arguments[0] = QXP_GL_TEXTURE0 + stage;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_CLIENT_ACTIVE_TEXTURE,
                    client_arguments, 1)) goto done;
            if (active) {
                pointer_arguments[0] = 4;
                pointer_arguments[1] = QXP_GL_FLOAT;
                pointer_arguments[2] = layout.stride;
                pointer_arguments[3] =
                    QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                        layout.texcoord_offset[stage]);
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_TEX_COORD_POINTER,
                        pointer_arguments, 4)) goto done;
            }
            if (!qxp_d3d_set_client_state(
                    device, QXP_GL_TEXTURE_COORD_ARRAY, active)) goto done;
        }

        pointer_arguments[0] = 3;
        pointer_arguments[1] = QXP_GL_FLOAT;
        pointer_arguments[2] = layout.stride;
        pointer_arguments[3] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
            layout.vertex_offset);
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_VERTEX_POINTER,
                                 pointer_arguments, 4) ||
            !qxp_d3d_set_client_state(device, QXP_GL_VERTEX_ARRAY, TRUE))
            goto done;

        client_arguments[0] = QXP_GL_SECONDARY_COLOR_ATTRIBUTE;
        if (generic_secondary) {
            if (!qxp_d3d_set_client_state(
                    device, QXP_GL_SECONDARY_COLOR_ARRAY, FALSE)) goto done;
            pointer_arguments[0] = QXP_GL_SECONDARY_COLOR_ATTRIBUTE;
            pointer_arguments[1] = 4;
            pointer_arguments[2] = QXP_GL_UNSIGNED_BYTE;
            pointer_arguments[3] = TRUE;
            pointer_arguments[4] = layout.stride;
            pointer_arguments[5] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.secondary_offset);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_VERTEX_ATTRIB_POINTER_ARB,
                    pointer_arguments, 6) ||
                !qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_ENABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    client_arguments, 1)) goto done;
        } else {
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                    client_arguments, 1)) goto done;
            if (secondary_active) {
                pointer_arguments[0] = 3;
                pointer_arguments[1] = QXP_GL_UNSIGNED_BYTE;
                pointer_arguments[2] = layout.stride;
                pointer_arguments[3] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                    layout.secondary_offset);
                if (!qxp_d3d_fifo_submit(
                        device, QXP_FENUM_GL_SECONDARY_COLOR_POINTER,
                        pointer_arguments, 4)) goto done;
            }
            if (!qxp_d3d_set_client_state(
                    device, QXP_GL_SECONDARY_COLOR_ARRAY,
                    secondary_active)) goto done;
        }

        if (fog_array_active) {
            pointer_arguments[0] = QXP_GL_FLOAT;
            pointer_arguments[1] = layout.stride;
            pointer_arguments[2] = QXP_D3D_INTERLEAVED_ARRAY_HANDLE(
                layout.fog_offset);
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_FOG_COORD_POINTER,
                    pointer_arguments, 3)) goto done;
        }
        if (!qxp_d3d_set_client_state(
                device, QXP_GL_FOG_COORDINATE_ARRAY,
                fog_array_active)) goto done;
    }

    if (direct_indexed) {
        index_output = qxp_d3d_fifo_append_data(
            device, request->vertex_count);
        if (!index_output) goto done;
        VideoPortMoveMemory(index_output, (PVOID)vertex_indices,
                            request->vertex_count);
    }

    if (object_data && gl_primitive == QXP_GL_POINTS &&
        (request->fvf & QXP_D3DFVF_PSIZE)) {
        /* D3DFVF_PSIZE overrides D3DRS_POINTSIZE. Fixed OpenGL has no point-
         * size array, but point-list vertices are independent, so preserve
         * order while coalescing adjacent equal sizes into bounded DrawArrays
         * runs. Scaled sizes first acquire D3D's viewport-height factor;
         * OpenGL distance attenuation and min/max state then finish the same
         * standard equation. Restore the global fallback afterward. */
        run_first = 0;
        while (run_first < request->vertex_count) {
            run_end = run_first + 1;
            run_first_source =
                vertex_indices ? vertex_indices[run_first] : run_first;
            while (run_end < request->vertex_count) {
                run_end_source =
                    vertex_indices ? vertex_indices[run_end] : run_end;
                if (object_data[run_end_source].point_size_bits !=
                    object_data[run_first_source].point_size_bits) break;
                ++run_end;
            }
            if (!qxp_d3d_point_size_value(
                    request, object_data[run_first_source].point_size_bits,
                    &point_arguments[0])) goto done;
            draw_arguments[0] = QXP_GL_POINTS;
            draw_arguments[1] = run_first;
            draw_arguments[2] = run_end - run_first;
            if (!qxp_d3d_append_vertex_array_data(
                    device, request, vertices, vertex_indices, object_data,
                    &layout, table_fog_columns, table_fog, primitive_size,
                    run_first, run_end - run_first, &failure_reason) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_SIZE,
                                     point_arguments, 1) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DRAW_ARRAYS,
                                     draw_arguments, 3)) goto done;
            ++submitted_draws;
            run_first = run_end;
        }
        if (!qxp_d3d_point_size_value(
                request, request->point_size_bits,
                &point_arguments[0])) goto done;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POINT_SIZE,
                                 point_arguments, 1)) goto done;
    } else {
        failure_phase = 2;
        if (!qxp_d3d_append_vertex_array_data(
                device, request, vertices,
                direct_indexed ? NULL : vertex_indices, object_data,
                &layout, table_fog_columns, table_fog,
                direct_indexed ? 1UL : primitive_size, 0,
                direct_indexed ? source_vertex_count :
                                 request->vertex_count,
                &failure_reason)) goto done;
        failure_phase = 3;
        draw_arguments[0] = gl_primitive;
        if (direct_indexed) {
            draw_arguments[1] = request->vertex_count;
            draw_arguments[2] = QXP_GL_UNSIGNED_BYTE;
            draw_arguments[3] = 0;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DRAW_ELEMENTS,
                    draw_arguments, 4)) goto done;
        } else {
            draw_arguments[1] = 0;
            draw_arguments[2] = request->vertex_count;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DRAW_ARRAYS,
                    draw_arguments, 3)) goto done;
        }
        submitted_draws = 1;
        if (direct_indexed &&
            device->d3d_indexed_element_draw_count != 0xffffffffUL)
            ++device->d3d_indexed_element_draw_count;
    }
    if (device->d3d_pending_draws != 0xffffffffUL) {
        if (submitted_draws >
            0xffffffffUL - device->d3d_pending_draws) {
            device->d3d_pending_draws = 0xffffffffUL;
        } else {
            device->d3d_pending_draws += submitted_draws;
        }
    }
    if (device->d3d_interleaved_draw_count != 0xffffffffUL)
        ++device->d3d_interleaved_draw_count;
    submitted = TRUE;

done:
    if (!submitted) {
        if (!device->d3d_array_failure_logged) {
            device->d3d_array_failure_logged = TRUE;
            qxp_debug("QXPMP D3D ARRAY FAILURE");
            qxp_debug_status(failure_phase);
            qxp_debug_status(failure_reason);
            qxp_debug_status(request->vertex_count);
            qxp_debug_status(request->fvf);
            qxp_debug_status(coordinate_stages);
            qxp_debug_status(object_data != NULL);
            qxp_debug_status(table_fog);
            qxp_debug_status(fog_array_active);
            qxp_debug_status(fifo[0] - fifo_start);
            qxp_debug_status(data[0] - data_start);
        }
        fifo[0] = fifo_start;
        data[0] = data_start;
    }
    return submitted;
}

static BOOLEAN qxp_d3d_sm2_sampler_zero(
    const QXP_D3D_SM2_SAMPLER_STATE *state)
{
    return state && !state->texture_name && !state->texture_type &&
           !state->address_u && !state->address_v &&
           !state->mag_filter && !state->min_filter &&
           !state->mip_filter && !state->mip_lod_bias_bits &&
           !state->max_mip_level;
}

static BOOLEAN qxp_d3d_sm2_sampler_equal(
    const QXP_D3D_SM2_SAMPLER_STATE *left,
    const QXP_D3D_SM2_SAMPLER_STATE *right)
{
    return left && right &&
           left->texture_name == right->texture_name &&
           left->texture_type == right->texture_type &&
           left->address_u == right->address_u &&
           left->address_v == right->address_v &&
           left->mag_filter == right->mag_filter &&
           left->min_filter == right->min_filter &&
           left->mip_filter == right->mip_filter &&
           left->mip_lod_bias_bits == right->mip_lod_bias_bits &&
           left->max_mip_level == right->max_mip_level;
}

static BOOLEAN qxp_d3d_sm2_sampler_contract(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_SM2_PROGRAM_STATE *program,
    const QXP_D3D_SM2_DRAW_STATE *draw_state)
{
    const QXP_D3D_SM2_SAMPLER_STATE *state;
    const QXP_D3D_SM2_SAMPLER_STATE *prior;
    QXP_D3D_TEXTURE_STATE *texture;
    ULONG sampler;
    ULONG prior_sampler;
    ULONG bit;
    ULONG expected_type;
    ULONG required_levels;
    ULONG face;

    if (!device || !request || !program || !draw_state ||
        request->pixel_sampled_stages != program->sampler_mask ||
        request->pixel_cube_stages != program->cube_sampler_mask ||
        request->pixel_volume_stages != program->volume_sampler_mask)
        return FALSE;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT; ++sampler) {
        state = &draw_state->samplers[sampler];
        bit = 1UL << sampler;
        if (!(program->sampler_mask & bit)) {
            if (!qxp_d3d_sm2_sampler_zero(state)) return FALSE;
            continue;
        }
        /* DirectX permits a programmable sample from an unbound resource.
         * The unbound form is the fully zeroed record; the draw binds the
         * opaque-black dummy of the mask-selected target below. */
        if (!state->texture_name) {
            if (!qxp_d3d_sm2_sampler_zero(state)) return FALSE;
            continue;
        }
        expected_type =
            (program->cube_sampler_mask & bit) ? QXP_D3D_TEXTURE_CUBE :
            (program->volume_sampler_mask & bit) ?
                QXP_D3D_TEXTURE_VOLUME : QXP_D3D_TEXTURE_2D;
        if (state->texture_type != expected_type ||
            state->address_u < 1 || state->address_u > 3 ||
            state->address_v < 1 || state->address_v > 3 ||
            (state->mag_filter != 1 && state->mag_filter != 2) ||
            (state->min_filter != 1 && state->min_filter != 2) ||
            state->mip_filter > 2 ||
            !qxp_float_bits_finite(state->mip_lod_bias_bits))
            return FALSE;
        texture = qxp_d3d_find_texture(
            device, request->process_id, state->texture_name);
        if (!texture || texture->texture_type != expected_type ||
            !texture->width || !texture->height || !texture->depth ||
            !texture->levels ||
            texture->levels > QXP_D3D_MAX_TEXTURE_LEVELS)
            return FALSE;
        required_levels = (1UL << texture->levels) - 1UL;
        if (expected_type == QXP_D3D_TEXTURE_CUBE) {
            for (face = 0; face < 6; ++face) {
                if ((texture->defined_levels[face] & required_levels) !=
                    required_levels) return FALSE;
            }
        } else if ((texture->defined_levels[0] & required_levels) !=
                   required_levels) {
            return FALSE;
        }
        /* Compatibility GL stores filtering and addressing on the texture
         * object, not on the unit.  One object cannot truthfully carry two
         * different sampler states within the same draw. */
        for (prior_sampler = 0; prior_sampler < sampler; ++prior_sampler) {
            if (!(program->sampler_mask & (1UL << prior_sampler))) continue;
            prior = &draw_state->samplers[prior_sampler];
            if (prior->texture_name == state->texture_name &&
                !qxp_d3d_sm2_sampler_equal(prior, state))
                return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_apply_sm2_sampler(
    QXP_DEVICE_EXTENSION *device, ULONG sampler,
    const QXP_D3D_SM2_SAMPLER_STATE *state, ULONG expected_type)
{
    QXP_D3D_TEXTURE_STATE *texture;
    ULONG active_arguments[1];
    ULONG arguments[3];
    ULONG targets[3];
    ULONG texture_target;
    ULONG texture_name;
    ULONG address_u;
    ULONG address_v;
    ULONG min_filter;
    ULONG base_level;
    ULONG index;

    if (!device || !state || sampler >= QXP_D3D_SM2_SAMPLER_COUNT)
        return FALSE;
    targets[0] = QXP_GL_TEXTURE_2D;
    targets[1] = QXP_GL_TEXTURE_CUBE_MAP;
    targets[2] = QXP_GL_TEXTURE_3D;
    active_arguments[0] = QXP_GL_TEXTURE0 + sampler;
    if (!state->texture_name) {
        /* Unbound programmable sample: bind the complete opaque-black
         * dummy of the mask-selected target. Its creation-time NEAREST
         * filter and clamp addressing already make it complete, so no
         * per-object parameter state is needed here. */
        if (!qxp_d3d_ensure_neutral_texture(device)) return FALSE;
        texture_target = expected_type == QXP_D3D_TEXTURE_CUBE ?
            QXP_GL_TEXTURE_CUBE_MAP :
            (expected_type == QXP_D3D_TEXTURE_VOLUME ?
                QXP_GL_TEXTURE_3D : QXP_GL_TEXTURE_2D);
        texture_name = expected_type == QXP_D3D_TEXTURE_CUBE ?
            QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME :
            (expected_type == QXP_D3D_TEXTURE_VOLUME ?
                QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME :
                QXP_D3D_NEUTRAL_TEXTURE_NAME);
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                active_arguments, 1)) return FALSE;
        for (index = 0; index < 3; ++index) {
            if (targets[index] == texture_target) continue;
            arguments[0] = targets[index];
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_DISABLE, arguments, 1))
                return FALSE;
            arguments[1] = 0;
            if (!qxp_d3d_fifo_submit(
                    device, QXP_FENUM_GL_BIND_TEXTURE, arguments, 2))
                return FALSE;
        }
        arguments[0] = texture_target;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ENABLE, arguments, 1))
            return FALSE;
        arguments[1] = texture_name;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_BIND_TEXTURE, arguments, 2))
            return FALSE;
        return TRUE;
    }
    texture = qxp_d3d_find_texture(
        device, device->d3d_process_id, state->texture_name);
    if (!texture || texture->texture_type != state->texture_type ||
        !texture->levels) return FALSE;
    texture_target = state->texture_type == QXP_D3D_TEXTURE_CUBE ?
        QXP_GL_TEXTURE_CUBE_MAP :
        (state->texture_type == QXP_D3D_TEXTURE_VOLUME ?
            QXP_GL_TEXTURE_3D : QXP_GL_TEXTURE_2D);
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_ACTIVE_TEXTURE,
            active_arguments, 1)) return FALSE;
    for (index = 0; index < 3; ++index) {
        if (targets[index] == texture_target) continue;
        arguments[0] = targets[index];
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_DISABLE, arguments, 1))
            return FALSE;
        arguments[1] = 0;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_BIND_TEXTURE, arguments, 2))
            return FALSE;
    }
    arguments[0] = texture_target;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_ENABLE, arguments, 1))
        return FALSE;
    arguments[1] = state->texture_name;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_BIND_TEXTURE, arguments, 2))
        return FALSE;

    address_u = state->address_u == 1 ? QXP_GL_REPEAT :
                (state->address_u == 2 ?
                    QXP_GL_MIRRORED_REPEAT : QXP_GL_CLAMP_TO_EDGE);
    address_v = state->address_v == 1 ? QXP_GL_REPEAT :
                (state->address_v == 2 ?
                    QXP_GL_MIRRORED_REPEAT : QXP_GL_CLAMP_TO_EDGE);
    if (state->mip_filter == 1) {
        min_filter = state->min_filter == 2 ?
            QXP_GL_LINEAR_MIPMAP_NEAREST :
            QXP_GL_NEAREST_MIPMAP_NEAREST;
    } else if (state->mip_filter == 2) {
        min_filter = state->min_filter == 2 ?
            QXP_GL_LINEAR_MIPMAP_LINEAR :
            QXP_GL_NEAREST_MIPMAP_LINEAR;
    } else {
        min_filter = state->min_filter == 2 ?
            QXP_GL_LINEAR : QXP_GL_NEAREST;
    }
    base_level = state->max_mip_level;
    if (base_level >= texture->levels) base_level = texture->levels - 1;

    arguments[0] = texture_target;
    arguments[1] = QXP_GL_TEXTURE_LOD_BIAS;
    arguments[2] = state->mip_lod_bias_bits;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_F, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_MAG_FILTER;
    arguments[2] =
        state->mag_filter == 2 ? QXP_GL_LINEAR : QXP_GL_NEAREST;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_MIN_FILTER;
    arguments[2] = min_filter;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_BASE_LEVEL;
    arguments[2] = base_level;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_MAX_LEVEL;
    arguments[2] = texture->levels - 1;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_WRAP_S;
    arguments[2] = state->texture_type == QXP_D3D_TEXTURE_CUBE ?
        QXP_GL_CLAMP_TO_EDGE : address_u;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    arguments[1] = QXP_GL_TEXTURE_WRAP_T;
    arguments[2] = state->texture_type == QXP_D3D_TEXTURE_CUBE ?
        QXP_GL_CLAMP_TO_EDGE : address_v;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
        return FALSE;
    if (state->texture_type != QXP_D3D_TEXTURE_2D) {
        arguments[1] = QXP_GL_TEXTURE_WRAP_R;
        arguments[2] = QXP_GL_CLAMP_TO_EDGE;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_TEX_PARAMETER_I, arguments, 3))
            return FALSE;
    }
    if (sampler >= 4) {
        qxp_debug("QXPMP D3D SM2 HIGH SAMPLER BIND");
        qxp_debug_status(sampler);
        qxp_debug_status(state->texture_name);
        qxp_debug_status(state->texture_type);
    }
    return TRUE;
}

static BOOLEAN qxp_d3d_scissor_state_valid(
    const QXP_D3D_DRAW_REQUEST *request)
{
    const QXP_D3D_RECT *rectangle;

    if (!request || request->scissor_enable > 1) return FALSE;
    if (!request->scissor_enable) return TRUE;
    rectangle = &request->scissor_rectangle;
    if (rectangle->left < 0 || rectangle->top < 0 ||
        rectangle->right < rectangle->left ||
        rectangle->bottom < rectangle->top ||
        (ULONG)rectangle->right > request->target_width ||
        (ULONG)rectangle->bottom > request->target_height) return FALSE;
    return TRUE;
}

static BOOLEAN qxp_d3d_sm2_draw_transport(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    ULONG request_bytes)
{
    QXP_D3D_SM2_PROGRAM_STATE *program;
    QXP_D3D_DRAW_PACKET_VIEW packet;
    const QXP_D3D_SM2_DRAW_STATE *sm2_state;
    const QXP_D3D_SM2_VERTEX_DRAW_STATE *draw_state;
    ULONG viewport_arguments[4];
    ULONG depth_range_arguments[4];
    ULONG scissor_arguments[4];
    ULONG state_arguments[2];
    ULONG blend_arguments[2];
    ULONG blend_equation_arguments[1];
    ULONG alpha_arguments[2];
    ULONG stencil_func_arguments[3];
    ULONG stencil_op_arguments[3];
    ULONG polygon_offset_arguments[2];
    ULONG color_mask_arguments[4];
    ULONG use_arguments[1];
    ULONG uniform_arguments[3];
    ULONG orientation[4];
    ULONG viewport_width_bits;
    ULONG viewport_height_bits;
    ULONG active_arguments[1];
    ULONG attribute_arguments[1];
    ULONG previous_program;
    ULONG previous_pending_draws;
    ULONG previous_interleaved_draw_count;
    BOOLEAN previous_vertex_program_point_size;
    ULONG gl_primitive;
    ULONG primitive_size;
    ULONG sampler;
    ULONG failure_sampler = 0xffffffffUL;
    ULONG stage;
    volatile ULONG *fifo;
    volatile ULONG *data;
    ULONG fifo_start;
    ULONG data_start;
    ULONG failure_phase = 0;
    ULONG sm2_fog_uniform[4];
    ULONG sm2_texture_factor_uniform[4];
    ULONG sm2_fog_arguments[2];
    ULONG sm2_fog_color[4];
    BOOLEAN previous_neutral_texture_defined = FALSE;
    BOOLEAN result = FALSE;

    if (!device || !request || !request->sm2_program_name ||
        !device->d3d_active ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        request->process_id != device->d3d_process_id ||
        (request->sm2_program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
            QXP_D3D_VERTEX_PROGRAM_NAMESPACE ||
        request->pixel_program_name) {
        failure_phase = 1;
        goto failed;
    }
    if (!qxp_d3d_primitive_parameters(
            request->primitive_type, &gl_primitive, &primitive_size)) {
        failure_phase = 2;
        goto failed;
    }
    if (!request->vertex_count ||
        request->vertex_count % primitive_size) {
        failure_phase = 3;
        goto failed;
    }
    if (!request->target_width || !request->target_height ||
        !request->viewport_width || !request->viewport_height ||
        request->viewport_x > request->target_width ||
        request->viewport_y > request->target_height ||
        request->viewport_width >
            request->target_width - request->viewport_x ||
        request->viewport_height >
            request->target_height - request->viewport_y ||
        request->target_width > 0xffffUL ||
        request->target_height > 0xffffUL ||
        !qxp_d3d_scissor_state_valid(request)) {
        failure_phase = 4;
        goto failed;
    }
    /* SM2 sampler units and pixel-input t# varyings are independent.
     * Programmable samplers span s0-s15 while pixel inputs remain t0-t7;
     * cube and volume targets are disjoint sampled-unit subsets. */
    if ((request->pixel_sampled_stages & ~0xffffUL) ||
        (request->pixel_coordinate_stages & ~0xffUL) ||
        (request->pixel_cube_stages & ~0xffffUL) ||
        (request->pixel_volume_stages & ~0xffffUL) ||
        (request->texture_projected_stages & ~0xffffUL) ||
        (request->pixel_cube_stages &
         ~request->pixel_sampled_stages) ||
        (request->pixel_volume_stages &
         ~request->pixel_sampled_stages) ||
        (request->pixel_cube_stages &
         request->pixel_volume_stages)) {
        failure_phase = 5;
        goto failed;
    }
    if (request->z_enable > 1 || request->z_write_enable > 1 ||
        request->z_func < 1 || request->z_func > 8 ||
        request->z_bias > 16) {
        failure_phase = 7;
        goto failed;
    }
    if (request->cull_mode < 1 || request->cull_mode > 3 ||
        request->fill_mode < 1 || request->fill_mode > 3 ||
        request->shade_mode < 1 || request->shade_mode > 2) {
        failure_phase = 8;
        goto failed;
    }
    if (!qxp_d3d_point_state_valid(request)) {
        failure_phase = 9;
        goto failed;
    }
    if (request->alpha_blend_enable > 1 ||
        request->alpha_test_enable > 1 ||
        request->alpha_reference > 255 ||
        (request->alpha_test_enable &&
         (request->alpha_function < 1 ||
          request->alpha_function > 8))) {
        failure_phase = 10;
        goto failed;
    }
    /* Fog is honoured by the generated fragment shader's epilogue, which
     * reads gl_Fog and the qxpPsFog mode uniform programmed below, so an
     * enabled fog state is no longer a refusal. */
    if (request->stencil_enable > 1 ||
        request->specular_enable > 1 ||
        request->dither_enable > 1 ||
        request->fog_table_mode > 3UL ||
        (request->color_write_enable & ~0x0fUL)) {
        failure_phase = 11;
        goto failed;
    }
    if (!qxp_packet_view_draw(request, request_bytes, &packet)) {
        failure_phase = 12;
        goto failed;
    }
    if (!packet.program ||
        packet.program->program_name != request->sm2_program_name) {
        failure_phase = 13;
        goto failed;
    }
    sm2_state = packet.sm2_draw_state;
    draw_state = &sm2_state->vertex;
    if (!qxp_packet_program_texture_contract(
            packet.program, request->pixel_coordinate_stages,
            request->texture_projected_stages)) {
        failure_phase = 14;
        goto failed;
    }
    if (packet.program->clipping_enable != TRUE ||
        (packet.program->clip_plane_enable &
         ~((1UL << QXP_D3D_MAX_USER_CLIP_PLANES) - 1UL)) ||
        !qxp_float_bits_finite(draw_state->z_min_bits) ||
        !qxp_float_bits_finite(draw_state->z_max_bits) ||
        ((draw_state->z_min_bits & 0x80000000UL) &&
         (draw_state->z_min_bits & 0x7fffffffUL)) ||
        ((draw_state->z_max_bits & 0x80000000UL) &&
         (draw_state->z_max_bits & 0x7fffffffUL)) ||
        (draw_state->z_min_bits & 0x7fffffffUL) > 0x3f800000UL ||
        (draw_state->z_max_bits & 0x7fffffffUL) > 0x3f800000UL ||
        (draw_state->z_min_bits & 0x7fffffffUL) >
            (draw_state->z_max_bits & 0x7fffffffUL)) {
        failure_phase = 0x80UL;
        goto failed;
    }
    for (stage = 0;
         stage < QXP_D3D_SM2_VERTEX_BOOLEAN_CONSTANT_COUNT; ++stage) {
        if (draw_state->boolean_constants[stage] > 1UL) {
            failure_phase = 0x81UL;
            goto failed;
        }
    }
    if (request->alpha_blend_enable) {
        if (!qxp_d3d_blend_factor(
                request->source_blend, TRUE, &blend_arguments[0])) {
            failure_phase = 15;
            goto failed;
        }
        if (!qxp_d3d_blend_factor(
                request->destination_blend, FALSE,
                &blend_arguments[1])) {
            failure_phase = 16;
            goto failed;
        }
        if (!qxp_d3d_blend_operation(
                request->blend_operation,
                &blend_equation_arguments[0])) {
            failure_phase = 17;
            goto failed;
        }
    }
    if (request->stencil_enable) {
        if (request->stencil_function < 1 ||
            request->stencil_function > 8) {
            failure_phase = 18;
            goto failed;
        }
        if (!qxp_d3d_stencil_operation(
                request->stencil_fail, &stencil_op_arguments[0])) {
            failure_phase = 19;
            goto failed;
        }
        if (!qxp_d3d_stencil_operation(
                request->stencil_z_fail, &stencil_op_arguments[1])) {
            failure_phase = 20;
            goto failed;
        }
        if (!qxp_d3d_stencil_operation(
                request->stencil_pass, &stencil_op_arguments[2])) {
            failure_phase = 21;
            goto failed;
        }
    }
    program = qxp_d3d_find_sm2_program(
        device, request->process_id, request->sm2_program_name);
    if (!program || program->vertex_clip_control_location < 0) {
        failure_phase = 22;
        goto failed;
    }
    if (!qxp_d3d_sm2_sampler_contract(
            device, request, program, sm2_state)) {
        failure_phase = 23;
        goto failed;
    }

    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo || !qxp_d3d_reserve_draw_fifo(device)) {
        failure_phase = 24;
        goto failed;
    }
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    previous_program = qxp_d3d_current_glsl_program(device);
    previous_pending_draws = device->d3d_pending_draws;
    previous_interleaved_draw_count =
        device->d3d_interleaved_draw_count;
    previous_vertex_program_point_size =
        device->d3d_vertex_program_point_size;
    previous_neutral_texture_defined =
        device->d3d_neutral_texture_defined;

    viewport_arguments[0] = request->viewport_x;
    viewport_arguments[1] = device->d3d_target_texture &&
                            !device->d3d_target_present ?
        request->viewport_y : request->target_height -
        request->viewport_y - request->viewport_height;
    viewport_arguments[2] = request->viewport_width;
    viewport_arguments[3] = request->viewport_height;
    if (!qxp_double_words_from_float(
            draw_state->z_min_bits, &depth_range_arguments[0]) ||
        !qxp_double_words_from_float(
            draw_state->z_max_bits, &depth_range_arguments[2]))
        goto done;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_VIEWPORT,
            viewport_arguments, 4) ||
        !qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DEPTH_RANGE,
            depth_range_arguments, 4)) goto done;

    state_arguments[0] = QXP_GL_SCISSOR_TEST;
    if (!qxp_d3d_fifo_submit(
            device, request->scissor_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    if (request->scissor_enable) {
        scissor_arguments[0] = (ULONG)request->scissor_rectangle.left;
        /* Render textures store D3D's top row as texture row zero, while a
         * presented target retains OpenGL's bottom-left window origin. */
        scissor_arguments[1] = device->d3d_target_texture &&
                               !device->d3d_target_present ?
            (ULONG)request->scissor_rectangle.top :
            request->target_height -
                (ULONG)request->scissor_rectangle.bottom;
        scissor_arguments[2] =
            (ULONG)(request->scissor_rectangle.right -
                    request->scissor_rectangle.left);
        scissor_arguments[3] =
            (ULONG)(request->scissor_rectangle.bottom -
                    request->scissor_rectangle.top);
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_SCISSOR,
                                 scissor_arguments, 4)) goto done;
    }

    failure_phase = 25;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT;
         ++sampler) {
        if (!(program->sampler_mask & (1UL << sampler))) continue;
        failure_sampler = sampler;
        if (!qxp_d3d_apply_sm2_sampler(
                device, sampler, &sm2_state->samplers[sampler],
                (program->cube_sampler_mask & (1UL << sampler)) ?
                    QXP_D3D_TEXTURE_CUBE :
                (program->volume_sampler_mask & (1UL << sampler)) ?
                    QXP_D3D_TEXTURE_VOLUME : QXP_D3D_TEXTURE_2D))
            goto done;
    }
    failure_sampler = 0xffffffffUL;
    failure_phase = 26;
    active_arguments[0] = QXP_GL_TEXTURE0;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_ACTIVE_TEXTURE,
            active_arguments, 1)) goto done;

    use_arguments[0] = program->gl_program;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_USE_PROGRAM,
            use_arguments, 1)) goto done;
    orientation[0] = device->d3d_target_texture &&
                     !device->d3d_target_present ?
                     0xbf800000UL : 0x3f800000UL;
    orientation[1] = request->fog_enable &&
                     request->fog_table_mode ?
                     0x3f800000UL : 0;
    if (!qxp_float_from_uint(request->viewport_width,
                             &viewport_width_bits) ||
        !qxp_float_from_uint(request->viewport_height,
                             &viewport_height_bits) ||
        !qxp_float_divide(0x3f800000UL, viewport_width_bits,
                          &orientation[2]) ||
        !qxp_float_divide(0x3f800000UL, viewport_height_bits,
                          &orientation[3])) goto done;
    if (!(device->d3d_target_texture && !device->d3d_target_present))
        orientation[3] ^= 0x80000000UL;
    uniform_arguments[0] =
        (ULONG)program->vertex_clip_control_location;
    uniform_arguments[1] = 1;
    uniform_arguments[2] = 0;
    if (!qxp_d3d_fifo_submit_input(
            device, QXP_FENUM_GL_UNIFORM_4FV,
            uniform_arguments, 3, orientation,
            sizeof(orientation))) goto done;
    if (program->vertex_positiont_location >= 0) {
        ULONG positiont[4];

        /* A pre-transformed vertex arrives in viewport pixels with y running
         * down, so the stage needs the scale and origin that map it into D3D
         * clip space.  The y sign is D3D's own convention, not the render
         * target's: the shared clip epilogue above already reorients. */
        if (!qxp_float_divide(0x40000000UL, viewport_width_bits,
                              &positiont[0]) ||
            !qxp_float_divide(0x40000000UL, viewport_height_bits,
                              &positiont[1]) ||
            !qxp_float_from_uint(request->viewport_x, &positiont[2]) ||
            !qxp_float_from_uint(request->viewport_y, &positiont[3]))
            goto done;
        positiont[1] ^= 0x80000000UL;
        uniform_arguments[0] =
            (ULONG)program->vertex_positiont_location;
        uniform_arguments[1] = 1;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                uniform_arguments, 3, positiont,
                sizeof(positiont))) goto done;
    }
    if (program->vertex_integer_constants_location >= 0) {
        uniform_arguments[0] =
            (ULONG)program->vertex_integer_constants_location;
        uniform_arguments[1] =
            QXP_D3D_SM2_VERTEX_INTEGER_CONSTANT_COUNT;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4IV,
                uniform_arguments, 3,
                draw_state->integer_constants,
                sizeof(draw_state->integer_constants))) goto done;
    }
    if (program->vertex_boolean_constants_location >= 0) {
        uniform_arguments[0] =
            (ULONG)program->vertex_boolean_constants_location;
        uniform_arguments[1] =
            QXP_D3D_SM2_VERTEX_BOOLEAN_CONSTANT_COUNT;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_1IV,
                uniform_arguments, 3,
                draw_state->boolean_constants,
                sizeof(draw_state->boolean_constants))) goto done;
    }
    if (!qxp_d3d_apply_program_clip_planes(
            device, packet.program)) goto done;
    state_arguments[0] = QXP_GL_VERTEX_PROGRAM_POINT_SIZE_ARB;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    device->d3d_vertex_program_point_size = FALSE;
    for (sampler = 0; sampler < QXP_D3D_SM2_SAMPLER_COUNT;
         ++sampler) {
        if (program->sampler_locations[sampler] < 0) continue;
        uniform_arguments[0] =
            (ULONG)program->sampler_locations[sampler];
        uniform_arguments[1] = sampler;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_UNIFORM_1I,
                uniform_arguments, 2)) goto done;
    }
    /* A bound GLSL program bypasses the fixed-function fog stage, so the
     * generated fragment shader performs the blend itself.  Publish the mode
     * it switches on plus the gl_Fog range/colour it reads. */
    if (program->pixel_fog_location >= 0) {
        /* IEEE bit patterns for 0.0f..3.0f keep this integer-only. */
        static const ULONG sm2_fog_mode_bits[4] = {
            0x00000000UL, 0x3f800000UL, 0x40000000UL, 0x40400000UL
        };
        sm2_fog_uniform[0] =
            (request->fog_enable && request->fog_table_mode < 4UL) ?
                sm2_fog_mode_bits[request->fog_table_mode] : 0UL;
        sm2_fog_uniform[1] = 0UL;
        sm2_fog_uniform[2] = 0UL;
        sm2_fog_uniform[3] = 0UL;
        uniform_arguments[0] = (ULONG)program->pixel_fog_location;
        uniform_arguments[1] = 1;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                uniform_arguments, 3, sm2_fog_uniform,
                sizeof(sm2_fog_uniform))) goto done;
    }
    /* The synthesized combiner reads the texture factor from a uniform, so a
     * title that animates it costs one uniform update per draw instead of a
     * fresh shader compile. */
    if (program->pixel_texture_factor_location >= 0) {
        static const ULONG channel_shift[4] = { 16UL, 8UL, 0UL, 24UL };
        ULONG channel;
        ULONG denominator = 0;

        if (!qxp_float_from_uint(255UL, &denominator)) goto done;
        for (channel = 0; channel < 4UL; ++channel) {
            ULONG numerator = 0;

            if (!qxp_float_from_uint(
                    (request->texture_factor >>
                     channel_shift[channel]) & 0xffUL,
                    &numerator) ||
                !qxp_float_divide(numerator, denominator,
                                  &sm2_texture_factor_uniform[channel]))
                goto done;
        }
        uniform_arguments[0] =
            (ULONG)program->pixel_texture_factor_location;
        uniform_arguments[1] = 1;
        uniform_arguments[2] = 0;
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_UNIFORM_4FV,
                uniform_arguments, 3, sm2_texture_factor_uniform,
                sizeof(sm2_texture_factor_uniform))) goto done;
    }
    if (request->fog_enable) {
        sm2_fog_arguments[0] = QXP_GL_FOG_START;
        sm2_fog_arguments[1] = request->fog_start_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                 sm2_fog_arguments, 2)) goto done;
        sm2_fog_arguments[0] = QXP_GL_FOG_END;
        sm2_fog_arguments[1] = request->fog_end_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                 sm2_fog_arguments, 2)) goto done;
        sm2_fog_arguments[0] = QXP_GL_FOG_DENSITY;
        sm2_fog_arguments[1] = request->fog_density_bits;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                 sm2_fog_arguments, 2)) goto done;
        sm2_fog_arguments[0] = QXP_GL_FOG_COLOR;
        sm2_fog_arguments[1] = 0;
        sm2_fog_color[0] = qxp_float_from_unorm8(
            (UCHAR)(request->fog_color >> 16));
        sm2_fog_color[1] = qxp_float_from_unorm8(
            (UCHAR)(request->fog_color >> 8));
        sm2_fog_color[2] = qxp_float_from_unorm8((UCHAR)request->fog_color);
        sm2_fog_color[3] = qxp_float_from_unorm8(
            (UCHAR)(request->fog_color >> 24));
        if (!qxp_d3d_fifo_submit_input(
                device, QXP_FENUM_GL_FOG_FV, sm2_fog_arguments, 2,
                sm2_fog_color, sizeof(sm2_fog_color))) goto done;
    }

    state_arguments[0] = QXP_GL_LIGHTING;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_COLOR_MATERIAL;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_NORMALIZE;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_DITHER;
    if (!qxp_d3d_fifo_submit(
            device, request->dither_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_FOG;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_COLOR_SUM;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;

    color_mask_arguments[0] =
        (request->color_write_enable & 1UL) != 0;
    color_mask_arguments[1] =
        (request->color_write_enable & 2UL) != 0;
    color_mask_arguments[2] =
        (request->color_write_enable & 4UL) != 0;
    color_mask_arguments[3] =
        (request->color_write_enable & 8UL) != 0;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_COLOR_MASK,
            color_mask_arguments, 4)) goto done;

    state_arguments[0] = QXP_GL_BLEND;
    if (!qxp_d3d_fifo_submit(
            device, request->alpha_blend_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    if (request->alpha_blend_enable &&
        (!qxp_d3d_fifo_submit(
             device, QXP_FENUM_GL_BLEND_EQUATION,
             blend_equation_arguments, 1) ||
         !qxp_d3d_fifo_submit(
             device, QXP_FENUM_GL_BLEND_FUNC,
             blend_arguments, 2))) goto done;

    state_arguments[0] = QXP_GL_ALPHA_TEST;
    if (!qxp_d3d_fifo_submit(
            device, request->alpha_test_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    if (request->alpha_test_enable) {
        alpha_arguments[0] =
            QXP_GL_NEVER + request->alpha_function - 1;
        alpha_arguments[1] = qxp_float_from_unorm8(
            (UCHAR)request->alpha_reference);
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ALPHA_FUNC,
                alpha_arguments, 2)) goto done;
    }

    state_arguments[0] = QXP_GL_STENCIL_TEST;
    if (!qxp_d3d_fifo_submit(
            device, request->stencil_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    if (request->stencil_enable) {
        stencil_func_arguments[0] =
            QXP_GL_NEVER + request->stencil_function - 1;
        stencil_func_arguments[1] = request->stencil_reference;
        stencil_func_arguments[2] = request->stencil_mask;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_STENCIL_FUNC,
                stencil_func_arguments, 3) ||
            !qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_STENCIL_OP,
                stencil_op_arguments, 3)) goto done;
        state_arguments[0] = request->stencil_write_mask;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_STENCIL_MASK,
                state_arguments, 1)) goto done;
    }

    state_arguments[0] = QXP_GL_DEPTH_TEST;
    if (!qxp_d3d_fifo_submit(
            device, request->z_enable ?
                QXP_FENUM_GL_ENABLE : QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_NEVER + request->z_func - 1;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DEPTH_FUNC,
            state_arguments, 1)) goto done;
    state_arguments[0] = request->z_write_enable ? TRUE : FALSE;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DEPTH_MASK,
            state_arguments, 1)) goto done;

    state_arguments[0] = QXP_GL_POLYGON_OFFSET_POINT;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_POLYGON_OFFSET_LINE;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_POLYGON_OFFSET_FILL;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_DISABLE,
            state_arguments, 1)) goto done;
    if (request->z_bias) {
        state_arguments[0] =
            request->primitive_type == 1 || request->fill_mode == 1 ?
            QXP_GL_POLYGON_OFFSET_POINT :
            (request->primitive_type == 2 || request->fill_mode == 2 ?
             QXP_GL_POLYGON_OFFSET_LINE : QXP_GL_POLYGON_OFFSET_FILL);
        polygon_offset_arguments[0] = 0;
        polygon_offset_arguments[1] =
            qxp_float_from_negative_uint(request->z_bias);
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ENABLE,
                state_arguments, 1) ||
            !qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_POLYGON_OFFSET,
                polygon_offset_arguments, 2)) goto done;
    }

    state_arguments[0] = QXP_GL_CULL_FACE;
    if (!qxp_d3d_fifo_submit(
            device, request->cull_mode == 1 ?
                QXP_FENUM_GL_DISABLE : QXP_FENUM_GL_ENABLE,
            state_arguments, 1)) goto done;
    if (request->cull_mode != 1) {
        state_arguments[0] = QXP_GL_BACK;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_CULL_FACE,
                state_arguments, 1)) goto done;
        state_arguments[0] = request->cull_mode == 3 ?
                             QXP_GL_CW : QXP_GL_CCW;
        if (device->d3d_target_texture &&
            !device->d3d_target_present) {
            state_arguments[0] = state_arguments[0] == QXP_GL_CCW ?
                                 QXP_GL_CW : QXP_GL_CCW;
        }
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_FRONT_FACE,
                state_arguments, 1)) goto done;
    }
    state_arguments[0] = QXP_GL_FRONT_AND_BACK;
    state_arguments[1] = request->fill_mode == 1 ? QXP_GL_POINT :
                         (request->fill_mode == 2 ?
                          QXP_GL_LINE : QXP_GL_FILL);
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_POLYGON_MODE,
            state_arguments, 2) ||
        !qxp_d3d_apply_point_state(device, request) ||
        !qxp_d3d_apply_point_sprite_state(device, request)) goto done;
    state_arguments[0] = request->shade_mode == 1 ?
                         QXP_GL_FLAT : QXP_GL_SMOOTH;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_SHADE_MODEL,
            state_arguments, 1)) goto done;
    state_arguments[0] = QXP_GL_FIRST_VERTEX_CONVENTION;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_PROVOKING_VERTEX,
            state_arguments, 1)) goto done;

    if (!qxp_d3d_submit_program_vertex_arrays(
            device, request, packet.program,
            packet.program_vertex_data, gl_primitive)) goto done;
    for (sampler = 0; sampler < 16; ++sampler) {
        attribute_arguments[0] = sampler;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_DISABLE_VERTEX_ATTRIB_ARRAY_ARB,
                attribute_arguments, 1)) goto done;
    }
    /* Compatibility fixed-function texturing can observe enabled units above
     * stage three.  Retire every high programmable unit after its draw so the
     * legacy four-stage cascade remains unchanged. */
    failure_phase = 27;
    for (sampler = 4; sampler < QXP_D3D_SM2_SAMPLER_COUNT; ++sampler) {
        if (!(program->sampler_mask & (1UL << sampler))) continue;
        failure_sampler = sampler;
        active_arguments[0] = QXP_GL_TEXTURE0 + sampler;
        state_arguments[0] =
            (program->cube_sampler_mask & (1UL << sampler)) ?
                QXP_GL_TEXTURE_CUBE_MAP :
            (program->volume_sampler_mask & (1UL << sampler)) ?
                QXP_GL_TEXTURE_3D : QXP_GL_TEXTURE_2D;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_ACTIVE_TEXTURE,
                active_arguments, 1) ||
            !qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_DISABLE,
                state_arguments, 1))
            goto done;
        state_arguments[1] = 0;
        if (!qxp_d3d_fifo_submit(
                device, QXP_FENUM_GL_BIND_TEXTURE,
                state_arguments, 2))
            goto done;
        qxp_debug("QXPMP D3D SM2 HIGH SAMPLER CLEAN");
        qxp_debug_status(sampler);
    }
    failure_sampler = 0xffffffffUL;
    failure_phase = 28;
    active_arguments[0] = QXP_GL_TEXTURE0;
    use_arguments[0] = previous_program;
    if (!qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_ACTIVE_TEXTURE,
            active_arguments, 1) ||
        !qxp_d3d_fifo_submit(
            device, QXP_FENUM_GL_USE_PROGRAM,
            use_arguments, 1)) goto done;
    qxp_d3d_invalidate_draw_state(device);
    result = TRUE;

done:
    if (!result) {
        if (!device->d3d_sm2_draw_failure_logged) {
            device->d3d_sm2_draw_failure_logged = TRUE;
            qxp_debug("QXPMP D3D SM2 EXECUTE REJECT");
            qxp_debug_status(failure_phase);
            qxp_debug_status(failure_sampler);
        }
        fifo[0] = fifo_start;
        data[0] = data_start;
        device->d3d_pending_draws = previous_pending_draws;
        device->d3d_interleaved_draw_count =
            previous_interleaved_draw_count;
        device->d3d_neutral_texture_defined =
            previous_neutral_texture_defined;
        device->d3d_vertex_program_point_size =
            previous_vertex_program_point_size;
        qxp_d3d_invalidate_draw_state(device);
    }
    return result;

failed:
    if (device && !device->d3d_sm2_draw_failure_logged) {
        device->d3d_sm2_draw_failure_logged = TRUE;
        qxp_debug("QXPMP D3D SM2 DRAW REJECT");
        qxp_debug_status(failure_phase);
        if (request) {
            qxp_debug_status(request->sm2_program_name);
            qxp_debug_status(request->pixel_sampled_stages);
            qxp_debug_status(request->pixel_coordinate_stages);
            qxp_debug_status(request->pixel_cube_stages);
            qxp_debug_status(request->pixel_volume_stages);
            qxp_debug_status(request->fog_enable);
            qxp_debug_status(request->fog_table_mode);
            qxp_debug_status(request->fog_color);
            qxp_debug_status(request->fog_start_bits);
            qxp_debug_status(request->fog_end_bits);
            qxp_debug_status(request->fog_density_bits);
            qxp_debug_status(request->stencil_enable);
            qxp_debug_status(request->color_write_enable);
        }
    }
    return FALSE;
}

static BOOLEAN qxp_d3d_draw_transport(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_DRAW_REQUEST *request,
    ULONG request_bytes)
{
    ULONG viewport_arguments[4];
    ULONG matrix_arguments[1];
    ULONG ortho_arguments[12];
    ULONG depth_range_arguments[4];
    ULONG scissor_arguments[4];
    ULONG disable_arguments[1];
    ULONG state_arguments[2];
    ULONG blend_arguments[2];
    ULONG blend_equation_arguments[1];
    ULONG alpha_arguments[2];
    ULONG stencil_func_arguments[3];
    ULONG stencil_op_arguments[3];
    ULONG polygon_offset_arguments[2];
    ULONG color_mask_arguments[4];
    ULONG fog_arguments[2];
    ULONG fog_color[4];
    ULONG table_fog_columns[8];
    const ULONG *active_table_fog_columns = NULL;
    volatile ULONG *fifo;
    volatile ULONG *data;
    ULONG fifo_start;
    ULONG data_start;
    ULONG previous_pixel_program;
    ULONG previous_vertex_program;
    BOOLEAN previous_vertex_program_point_size;
    BOOLEAN previous_vertex_program_clip;
    BOOLEAN previous_neutral_texture_defined;
    ULONG index;
    ULONG gl_primitive;
    ULONG primitive_size;
    ULONG texture_count;
    ULONG coordinate_stages;
    ULONG expected_projected_stages = 0;
    ULONG fog_mode = 0;
    ULONG vertex_program = 0;
    ULONG stage = 0;
    BOOLEAN table_fog;
    BOOLEAN vertex_fog;
    BOOLEAN object_vertex_fog;
    BOOLEAN fog_active;
    BOOLEAN fog_array_active;
    BOOLEAN texture_q_program = FALSE;
    BOOLEAN positional_range_program;
    BOOLEAN dual_point_specular_program;
    BOOLEAN specular_alpha_program;
    BOOLEAN quad_point_range_program;
    BOOLEAN multi_point_range_program;
    BOOLEAN scalable_point_program;
    BOOLEAN object_program_material_sources;
    BOOLEAN object_color_material;
    ULONG object_material_mode;
    BOOLEAN generic_secondary;
    BOOLEAN fixed_state_cached;
    BOOLEAN point_sprite_state_cached;
    BOOLEAN texture_state_cached[4];
    BOOLEAN array_state_cached;
    BOOLEAN compact_draw;
    BOOLEAN object_draw;
    BOOLEAN raw_object_draw;
    BOOLEAN result = FALSE;
    QXP_D3D_DRAW_PACKET_VIEW packet;
    QXP_D3D_TEXCOORD_PLAN texcoord_plan;
#ifdef QXP_D3D_PROFILE
    LONGLONG profile_start_ticks;
    LONGLONG profile_packet_ticks;
    LONGLONG profile_prepare_ticks;
    LONGLONG profile_state_ticks;
    LONGLONG profile_submit_ticks;

    profile_start_ticks = VideoPortQueryPerformanceCounter(device, NULL);
    profile_packet_ticks = profile_start_ticks;
    profile_prepare_ticks = profile_start_ticks;
    profile_state_ticks = profile_start_ticks;
    profile_submit_ticks = profile_start_ticks;
#endif

    if (!request) {
        qxp_debug("QXPMP D3D DRAW NULL-");
        return FALSE;
    }
    if (request->sm2_program_name)
        return qxp_d3d_sm2_draw_transport(
            device, request, request_bytes);
    if (!qxp_d3d_primitive_parameters(request->primitive_type,
                                       &gl_primitive,
                                       &primitive_size) ||
        !device->d3d_active ||
        request->version != QXP_D3D_TRANSPORT_VERSION ||
        request->process_id != device->d3d_process_id ||
        !request->vertex_count ||
        request->vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        request->vertex_count % primitive_size ||
        !request->target_width || !request->target_height ||
        !request->viewport_width || !request->viewport_height ||
        request->viewport_x > request->target_width ||
        request->viewport_y > request->target_height ||
        request->viewport_width >
            request->target_width - request->viewport_x ||
        request->viewport_height >
            request->target_height - request->viewport_y ||
        request->target_width > 0xffffUL ||
        request->target_height > 0xffffUL ||
        !qxp_d3d_scissor_state_valid(request) ||
        (request->pixel_sampled_stages & ~15UL) ||
        (request->pixel_coordinate_stages & ~15UL) ||
        (request->pixel_cube_stages & ~15UL) ||
        (request->texture_projected_stages & ~15UL) ||
        (request->pixel_sampled_stages &
         ~request->pixel_coordinate_stages) ||
        (request->pixel_cube_stages &
         ~request->pixel_sampled_stages) ||
        /* The ordinary fixed/ARB draw path never consumes a volume mask;
         * only the diagnostic SM2 transport above may. */
        request->pixel_volume_stages ||
        (!request->pixel_program_name &&
         (request->pixel_sampled_stages ||
          request->pixel_coordinate_stages ||
          request->pixel_cube_stages)) ||
        (request->pixel_program_name &&
         (request->pixel_program_name & QXP_D3D_PROGRAM_NAMESPACE_MASK) !=
             QXP_D3D_PIXEL_PROGRAM_NAMESPACE)) {
        qxp_debug("QXPMP D3D DRAW HEADER-");
        qxp_debug_status(request->version);
        qxp_debug_status(request->primitive_type);
        qxp_debug_status(request->fvf);
        qxp_debug_status(request->vertex_count);
        qxp_debug("QXPMP D3D DRAW HEADER ID");
        qxp_debug_status(device->d3d_active);
        qxp_debug_status(device->d3d_process_id);
        qxp_debug_status(request->process_id);
        qxp_debug("QXPMP D3D DRAW HEADER TARGET");
        qxp_debug_status(request->target_width);
        qxp_debug_status(request->target_height);
        qxp_debug_status(request->viewport_x);
        qxp_debug_status(request->viewport_y);
        qxp_debug_status(request->viewport_width);
        qxp_debug_status(request->viewport_height);
        qxp_debug("QXPMP D3D DRAW HEADER PIXEL");
        qxp_debug_status(request->pixel_program_name);
        qxp_debug_status(request->pixel_sampled_stages);
        qxp_debug_status(request->pixel_coordinate_stages);
        qxp_debug_status(request->pixel_cube_stages);
        qxp_debug_status(request->pixel_volume_stages);
        qxp_debug_status(request->texture_projected_stages);
        qxp_debug("QXPMP D3D DRAW HEADER FIXED");
        qxp_debug_status(request->texture_color_op);
        qxp_debug_status(request->texture1_color_op);
        qxp_debug_status(request->texture2_color_op);
        qxp_debug_status(request->texture3_color_op);
        qxp_debug_status(request->texture_name);
        qxp_debug_status(request->texture1_name);
        qxp_debug_status(request->texture2_name);
        qxp_debug_status(request->texture3_name);
        return FALSE;
    }
    if (!qxp_packet_view_draw(request, request_bytes, &packet)) {
        qxp_debug("QXPMP D3D DRAW PACKET-");
        return FALSE;
    }
    compact_draw = packet.compact != NULL ||
                   packet.compact_indexed != NULL;
    raw_object_draw = packet.raw_indexed_object != NULL;
    object_draw = packet.object_data != NULL || raw_object_draw;
#ifdef QXP_D3D_PROFILE
    profile_packet_ticks = VideoPortQueryPerformanceCounter(device, NULL);
#endif
    texture_count = (request->fvf >> 8) & 0x0fUL;
    /* The FVF coordinate-dimension check applies to the fixed paths; the
     * programmable path carries texcoords in the vs payload instead, so skip
     * it when a program payload is present. */
    if (!packet.program) {
        for (index = 0; index < 4; ++index) {
            if ((index < texture_count &&
                 (request->texture_coord_dimensions[index] < 1 ||
                  request->texture_coord_dimensions[index] > 4)) ||
                (index >= texture_count &&
                 request->texture_coord_dimensions[index])) {
                qxp_debug("QXPMP D3D DRAW TEXDIM-");
                qxp_debug_status(index);
                qxp_debug_status(request->texture_coord_dimensions[index]);
                return FALSE;
            }
        }
    }
    coordinate_stages = request->pixel_coordinate_stages;
    if (!request->pixel_program_name && request->texture_name &&
        request->texture_color_op != QXP_D3DTOP_DISABLE)
        coordinate_stages |= 1UL;
    if (!request->pixel_program_name && request->texture1_name &&
        request->texture1_color_op != QXP_D3DTOP_DISABLE)
        coordinate_stages |= 2UL;
    if (!request->pixel_program_name && request->texture2_name &&
        request->texture2_color_op != QXP_D3DTOP_DISABLE)
        coordinate_stages |= 4UL;
    if (!request->pixel_program_name && request->texture3_name &&
        request->texture3_color_op != QXP_D3DTOP_DISABLE)
        coordinate_stages |= 8UL;
    if (!qxp_packet_texcoord_plan(
            request, &packet, coordinate_stages, &texcoord_plan)) {
        qxp_debug("QXPMP D3D DRAW TEXCOORD-");
        qxp_debug_status(request->texture_coord_index);
        qxp_debug_status(request->texture1_coord_index);
        qxp_debug_status(request->texture2_coord_index);
        qxp_debug_status(request->texture3_coord_index);
        qxp_debug_status(texture_count);
        return FALSE;
    }
    if (packet.program &&
        !qxp_packet_program_texture_contract(
            packet.program, coordinate_stages,
            request->texture_projected_stages)) {
        qxp_debug("QXPMP D3D DRAW PROGRAM TEXCOORD-");
        qxp_debug_status(coordinate_stages);
        qxp_debug_status(request->texture_projected_stages);
        qxp_debug_status(packet.program->written_outputs);
        return FALSE;
    }
    for (stage = 0; stage < 4; ++stage) {
        ULONG coordinate_index;
        ULONG flags;

        /* The program contract above validates stage n against oTn and the
         * projected-mask subset without consulting fixed-FVF dimensions. */
        if (packet.program) continue;
        if (object_draw) {
            flags = packet.hwtl_state->texture_transform_flags[stage];
            if (coordinate_stages & (1UL << stage)) {
                coordinate_index = texcoord_plan.selectors[stage];
                /* Generated sources are intrinsically 3D and retain neutral
                 * array q=1. Only a pass-through 4D FVF input needs the
                 * bounded q-correction vertex program. */
                if ((texcoord_plan.passthrough_stages &
                     (1UL << stage)) &&
                    !(flags & 0x00000100UL) &&
                    request->texture_coord_dimensions[coordinate_index] == 4)
                    texture_q_program = TRUE;
                if (flags & 0x00000100UL) {
                    if ((flags & 0x0fUL) < 3) {
                        qxp_debug("QXPMP D3D DRAW PROJECT COUNT-");
                        qxp_debug_status(stage);
                        qxp_debug_status(flags);
                        return FALSE;
                    }
                    expected_projected_stages |= 1UL << stage;
                }
            }
            continue;
        }
        if (!(request->texture_projected_stages & (1UL << stage))) continue;
        if (!(coordinate_stages & (1UL << stage))) {
            qxp_debug("QXPMP D3D DRAW PROJECT MASK-");
            qxp_debug_status(request->texture_projected_stages);
            qxp_debug_status(coordinate_stages);
            return FALSE;
        }
        coordinate_index = texcoord_plan.selectors[stage];
        if (request->texture_coord_dimensions[coordinate_index] < 3) {
            qxp_debug("QXPMP D3D DRAW PROJECT DIM-");
            qxp_debug_status(stage);
            qxp_debug_status(
                request->texture_coord_dimensions[coordinate_index]);
            return FALSE;
        }
    }
    if (object_draw && request->texture_projected_stages !=
                              expected_projected_stages) {
        qxp_debug("QXPMP D3D DRAW PROJECT OBJECT-");
        qxp_debug_status(request->texture_projected_stages);
        qxp_debug_status(expected_projected_stages);
        return FALSE;
    }
    if (request->alpha_test_enable > 1 || request->alpha_reference > 255 ||
        (request->alpha_test_enable &&
         (request->alpha_function < 1 || request->alpha_function > 8))) {
        qxp_debug("QXPMP D3D DRAW ALPHA-");
        qxp_debug_status(request->alpha_test_enable);
        qxp_debug_status(request->alpha_reference);
        qxp_debug_status(request->alpha_function);
        return FALSE;
    }
    if (request->stencil_enable > 1 ||
        (request->stencil_enable &&
         (request->stencil_function < 1 ||
          request->stencil_function > 8 ||
          !qxp_d3d_stencil_operation(request->stencil_fail,
                                     &stencil_op_arguments[0]) ||
          !qxp_d3d_stencil_operation(request->stencil_z_fail,
                                     &stencil_op_arguments[1]) ||
          !qxp_d3d_stencil_operation(request->stencil_pass,
                                     &stencil_op_arguments[2])))) {
        qxp_debug("QXPMP D3D DRAW STENCIL-");
        qxp_debug_status(request->stencil_enable);
        qxp_debug_status(request->stencil_function);
        qxp_debug_status(request->stencil_pass);
        return FALSE;
    }
    if (request->z_enable > 1 || request->z_write_enable > 1 ||
        !qxp_d3d_point_state_valid(request) ||
        request->z_bias > 16 ||
        (request->color_write_enable & ~0x0fUL) ||
        request->alpha_blend_enable > 1 ||
        request->specular_enable > 1 || request->dither_enable > 1 ||
        request->fog_enable > 1 || request->fog_table_mode > 3 ||
        !qxp_float_bits_finite(request->fog_start_bits) ||
        !qxp_float_bits_finite(request->fog_end_bits) ||
        !qxp_float_bits_finite(request->fog_density_bits) ||
        (request->fog_density_bits & 0x80000000UL) ||
        request->z_func < 1 || request->z_func > 8 ||
        request->cull_mode < 1 || request->cull_mode > 3 ||
        request->fill_mode < 1 || request->fill_mode > 3 ||
        request->shade_mode < 1 || request->shade_mode > 2) {
        qxp_debug("QXPMP D3D DRAW STATE-");
        qxp_debug_status(request->z_enable);
        qxp_debug_status(request->z_write_enable);
        qxp_debug_status(request->z_func);
        qxp_debug_status(request->z_bias);
        qxp_debug_status(request->color_write_enable);
        qxp_debug_status(request->alpha_blend_enable);
        qxp_debug_status(request->cull_mode);
        qxp_debug_status(request->fill_mode);
        qxp_debug_status(request->shade_mode);
        qxp_debug_status(request->point_size_bits);
        qxp_debug_status(request->point_scale_enable);
        qxp_debug_status(request->point_sprite_enable);
        qxp_debug_status(request->point_size_min_bits);
        qxp_debug_status(request->point_size_max_bits);
        qxp_debug_status(request->point_scale_a_bits);
        qxp_debug_status(request->point_scale_b_bits);
        qxp_debug_status(request->point_scale_c_bits);
        qxp_debug_status(request->fog_enable);
        qxp_debug_status(request->fog_table_mode);
        return FALSE;
    }
    table_fog = request->fog_enable && request->fog_table_mode;
    if (table_fog && object_draw) {
        if (!qxp_float_transform_depth_columns(
                packet.hwtl_state->world[0].element,
                packet.hwtl_state->view.element,
                packet.hwtl_state->projection.element,
                table_fog_columns)) {
            qxp_debug("QXPMP D3D DRAW TABLE FOG MATRIX-");
            return FALSE;
        }
        active_table_fog_columns = table_fog_columns;
    }
    specular_alpha_program =
        qxp_d3d_specular_alpha_program_supported(request);
    object_vertex_fog = object_draw && request->fog_enable &&
        !request->fog_table_mode && packet.hwtl_state->fog_vertex_mode &&
        !(request->fvf & QXP_D3DFVF_FOG);
    vertex_fog = !request->fog_table_mode &&
        (packet.program ?
         (request->fog_enable &&
          (packet.program->written_outputs & QXP_D3D_VS_OUTPUT_FOG) != 0) :
         ((request->fvf & QXP_D3DFVF_FOG) != 0 ||
          (request->fog_enable &&
           (object_draw ? object_vertex_fog :
            (request->fvf & QXP_D3DFVF_SPECULAR) != 0))));
    fog_active = table_fog || vertex_fog;
    fog_array_active = !packet.program && !compact_draw &&
                       fog_active && !object_vertex_fog;
    positional_range_program = object_draw &&
        qxp_d3d_positional_range_program_supported(
            request, packet.hwtl_state);
    dual_point_specular_program = object_draw &&
        qxp_d3d_dual_point_specular_program_supported(
            request, packet.hwtl_state);
    quad_point_range_program = object_draw &&
        qxp_d3d_quad_point_range_program_supported(
            request, packet.hwtl_state);
    multi_point_range_program = object_draw &&
        qxp_d3d_multi_point_range_program_supported(
            request, packet.hwtl_state);
    scalable_point_program = object_draw &&
        qxp_d3d_scalable_point_program_supported(
            request, packet.hwtl_state);
    if (positional_range_program) {
        vertex_program = QXP_D3D_POSITIONAL_RANGE_PROGRAM;
    } else if (dual_point_specular_program) {
        vertex_program = QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM;
    } else if (quad_point_range_program) {
        vertex_program = QXP_D3D_QUAD_POINT_RANGE_PROGRAM;
    } else if (multi_point_range_program) {
        vertex_program = QXP_D3D_MULTI_POINT_RANGE_PROGRAM;
    } else if (scalable_point_program) {
        vertex_program = QXP_D3D_SCALABLE_POINT_PROGRAM;
    } else if (texture_q_program) {
        vertex_program = QXP_D3D_TEXTURE_Q_PROGRAM;
    }
    if (specular_alpha_program)
        vertex_program = QXP_D3D_SPECULAR_ALPHA_PROGRAM;
    else if (compact_draw)
        vertex_program = QXP_D3D_COMPACT_TRANSFORMED_PROGRAM;
    else if (packet.program)
        vertex_program = packet.program->program_name;
    if (qxp_packet_object_glsl_required(
            request, &packet, &texcoord_plan,
            (BOOLEAN)(vertex_program != 0))) {
        if (!device->d3d_texq_glsl_logged && texture_q_program &&
            vertex_program == QXP_D3D_TEXTURE_Q_PROGRAM &&
            !texcoord_plan.generated_stages &&
            packet.hwtl_state->lighting_enable &&
            !(request->fvf & QXP_D3DFVF_NORMAL)) {
            device->d3d_texq_glsl_logged = TRUE;
            qxp_debug("QXPMP D3D TEXQ GLSL+");
            qxp_debug_status(request->fvf);
            qxp_debug_status(texcoord_plan.passthrough_stages);
            qxp_debug_status(request->texture_coord_dimensions[0]);
            qxp_debug_status(request->texture_coord_dimensions[1]);
            qxp_debug_status(request->texture_coord_dimensions[2]);
            qxp_debug_status(request->texture_coord_dimensions[3]);
        }
        /* ARB vertex programs bypass fixed GL texgen. Complete GLSL also
         * preserves D3D's missing-normal value and independent material
         * sources where fixed OpenGL cannot. The validated plan is uploaded
         * below. */
        vertex_program = QXP_D3D_SCALABLE_POINT_PROGRAM;
    } else if (texcoord_plan.generated_stages && vertex_program) {
        qxp_debug("QXPMP D3D DRAW TEXGEN PROGRAM-");
        return FALSE;
    }
    /* Defer the TEXQ conflict until complete GLSL containment has had a
     * chance to replace it. This keeps valid lit/no-normal object draws
     * reachable while retaining the conservative residual rejection. */
    if (texture_q_program &&
        vertex_program == QXP_D3D_TEXTURE_Q_PROGRAM &&
        packet.hwtl_state->lighting_enable &&
        !texcoord_plan.generated_stages &&
        !positional_range_program && !dual_point_specular_program &&
        !quad_point_range_program && !multi_point_range_program &&
        !scalable_point_program) {
        qxp_debug("QXPMP D3D DRAW TEXQ VP STATE-");
        qxp_debug_status(request->fog_table_mode);
        qxp_debug_status(packet.hwtl_state->lighting_enable);
        qxp_debug_status(gl_primitive);
        qxp_debug_status(packet.hwtl_state->clip_plane_enable);
        return FALSE;
    }
    generic_secondary = packet.program ?
        (packet.program->written_outputs & QXP_D3D_VS_OUTPUT_COLOR1) != 0 :
        (vertex_program != 0 &&
         (request->fvf & QXP_D3DFVF_SPECULAR) != 0);
    if (table_fog &&
        !qxp_d3d_fog_mode(request->fog_table_mode, &fog_mode)) {
        qxp_debug("QXPMP D3D DRAW FOG-");
        return FALSE;
    }
    if (object_vertex_fog) {
        if (!qxp_d3d_fog_mode(packet.hwtl_state->fog_vertex_mode,
                              &fog_mode)) {
            qxp_debug("QXPMP D3D DRAW OBJECT FOG-");
            return FALSE;
        }
    } else if (vertex_fog) {
        /* Transformed vertices carry fog in specular alpha; programmable
         * draws emit the equivalent 1-oFog coordinate from the ARB epilogue. */
        fog_mode = QXP_GL_LINEAR;
    }
    if (request->alpha_blend_enable &&
        (!qxp_d3d_blend_factor(request->source_blend, TRUE,
                               &blend_arguments[0]) ||
         !qxp_d3d_blend_factor(request->destination_blend, FALSE,
                               &blend_arguments[1]) ||
         !qxp_d3d_blend_operation(request->blend_operation,
                                  &blend_equation_arguments[0]))) {
        qxp_debug("QXPMP D3D DRAW BLEND-");
        qxp_debug_status(request->source_blend);
        qxp_debug_status(request->destination_blend);
        qxp_debug_status(request->blend_operation);
        return FALSE;
    }
    if ((vertex_program == QXP_D3D_TEXTURE_Q_PROGRAM &&
         !qxp_d3d_ensure_texture_q_program(device)) ||
        (vertex_program == QXP_D3D_POSITIONAL_RANGE_PROGRAM &&
         !qxp_d3d_ensure_positional_range_program(device)) ||
        (vertex_program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM &&
         !qxp_d3d_ensure_dual_point_specular_program(device)) ||
        (vertex_program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM &&
         !qxp_d3d_ensure_quad_point_range_program(device)) ||
        (vertex_program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM &&
         !qxp_d3d_ensure_multi_point_range_program(device)) ||
        (vertex_program == QXP_D3D_SCALABLE_POINT_PROGRAM &&
         !qxp_d3d_ensure_scalable_point_program(device)) ||
        (vertex_program == QXP_D3D_SPECULAR_ALPHA_PROGRAM &&
         !qxp_d3d_ensure_specular_alpha_program(device)) ||
        (vertex_program == QXP_D3D_COMPACT_TRANSFORMED_PROGRAM &&
         !qxp_d3d_ensure_compact_transformed_program(device))) {
        qxp_debug("QXPMP D3D DRAW INTERNAL VP DEFINE-");
        return FALSE;
    }
#ifdef QXP_D3D_PROFILE
    profile_prepare_ticks = VideoPortQueryPerformanceCounter(device, NULL);
#endif
#ifdef QXP_VERBOSE_TRACE
    if (!packet.program && !compact_draw && !raw_object_draw)
        qxp_d3d_trace_draw(
            device, request, packet.vertices, packet.vertex_indices,
            packet.object_data);
#endif
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    if (!fifo || !qxp_d3d_reserve_draw_fifo(device)) {
        qxp_debug("QXPMP D3D DRAW FIFO-");
        return FALSE;
    }
    data = fifo + QXP_MESA_MAX_FIFO;
    fifo_start = fifo[0];
    data_start = data[0];
    previous_pixel_program = device->d3d_pixel_program;
    previous_vertex_program = device->d3d_vertex_program;
    previous_vertex_program_point_size =
        device->d3d_vertex_program_point_size;
    previous_vertex_program_clip = device->d3d_vertex_program_clip;
    previous_neutral_texture_defined =
        device->d3d_neutral_texture_defined;
    fixed_state_cached = !packet.program &&
        device->d3d_last_coordinate_stages == coordinate_stages &&
        qxp_d3d_fixed_state_same(
            device, request, packet.hwtl_state);
    point_sprite_state_cached = qxp_d3d_point_sprite_state_same(
        device, request);
    for (index = 0; index < 4; ++index) {
        texture_state_cached[index] =
            qxp_d3d_texture_state_same(device, request, index);
    }
    array_state_cached = !packet.program && qxp_d3d_array_state_same(
        device, request, coordinate_stages, fog_array_active,
        generic_secondary);
    if (!(packet.program ?
          qxp_d3d_apply_program_vertex_program(
              device, request, packet.program) :
          qxp_d3d_apply_vertex_program(
              device, request, packet.hwtl_state,
              &texcoord_plan, vertex_program))) {
        qxp_debug("QXPMP D3D DRAW VP-");
        goto done;
    }
    if (!qxp_d3d_apply_pixel_program(device, request)) {
        qxp_debug("QXPMP D3D DRAW PS-");
        goto done;
    }

    /* The fixed-state key includes scissor enable, rectangle, target type,
     * and target dimensions. Re-submit only on an exact transition; a cache
     * hit proves that the preceding successful draw installed the same GL
     * scissor state. */
    if (!fixed_state_cached) {
        state_arguments[0] = QXP_GL_SCISSOR_TEST;
        if (!qxp_d3d_fifo_submit(
                device, request->scissor_enable ? QXP_FENUM_GL_ENABLE :
                                                   QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        if (request->scissor_enable) {
            scissor_arguments[0] =
                (ULONG)request->scissor_rectangle.left;
            /* Render textures store D3D's top row as texture row zero, while
             * a presented target retains OpenGL's bottom-left origin. */
            scissor_arguments[1] = device->d3d_target_texture &&
                                   !device->d3d_target_present ?
                (ULONG)request->scissor_rectangle.top :
                request->target_height -
                    (ULONG)request->scissor_rectangle.bottom;
            scissor_arguments[2] =
                (ULONG)(request->scissor_rectangle.right -
                        request->scissor_rectangle.left);
            scissor_arguments[3] =
                (ULONG)(request->scissor_rectangle.bottom -
                        request->scissor_rectangle.top);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_SCISSOR,
                                     scissor_arguments, 4)) goto done;
        }
    }

    if (!fixed_state_cached) {
        if (!qxp_d3d_apply_texcoord_generation(
                device, &texcoord_plan)) goto done;
        viewport_arguments[0] = request->viewport_x;
        viewport_arguments[1] = device->d3d_target_texture &&
                                !device->d3d_target_present ?
            request->viewport_y : request->target_height -
            request->viewport_y - request->viewport_height;
        viewport_arguments[2] = request->viewport_width;
        viewport_arguments[3] = request->viewport_height;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_VIEWPORT,
                                 viewport_arguments, 4)) goto done;
        if (packet.program) {
            /* The translated program emits OpenGL clip coordinates directly;
             * its epilogue already converts D3D depth and target Y policy. */
            depth_range_arguments[0] = 0;
            depth_range_arguments[1] = 0;
            depth_range_arguments[2] = 0;
            depth_range_arguments[3] = 0x3ff00000UL;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_RANGE,
                                     depth_range_arguments, 4)) goto done;
        } else if (object_draw) {
            if (!qxp_d3d_apply_object_matrices(
                    device, packet.hwtl_state)) goto done;
        } else {
        matrix_arguments[0] = QXP_GL_PROJECTION;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                                 matrix_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LOAD_IDENTITY,
                                 NULL, 0)) goto done;
        qxp_double_words_from_uint(request->viewport_x, &ortho_arguments[0]);
        qxp_double_words_from_uint(request->viewport_x +
                                   request->viewport_width,
                                   &ortho_arguments[2]);
        if (device->d3d_target_texture && !device->d3d_target_present) {
            /* Store the D3D top row in texture row zero. Sampling the completed
             * render target then uses ordinary D3D texture coordinates without
             * a per-stage vertical-flip flag. */
            qxp_double_words_from_uint(request->viewport_y,
                                       &ortho_arguments[4]);
            qxp_double_words_from_uint(request->viewport_y +
                                       request->viewport_height,
                                       &ortho_arguments[6]);
        } else {
            qxp_double_words_from_uint(request->viewport_y +
                                       request->viewport_height,
                                       &ortho_arguments[4]);
            qxp_double_words_from_uint(request->viewport_y,
                                       &ortho_arguments[6]);
        }
        qxp_double_words_from_uint(0, &ortho_arguments[8]);
        /* glOrtho(near=0, far=-1) maps D3D's post-transform z range [0,1]
         * onto OpenGL's clip-space range [-1,1].  A positive far plane would
         * map z=0.5 to -2 and clip otherwise valid pre-transformed geometry. */
        ortho_arguments[10] = 0;
        ortho_arguments[11] = 0xbff00000UL;
        depth_range_arguments[0] = 0;
        depth_range_arguments[1] = 0;
        depth_range_arguments[2] = 0;
        depth_range_arguments[3] = 0x3ff00000UL;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ORTHO,
                                 ortho_arguments, 12) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_RANGE,
                                 depth_range_arguments, 4)) goto done;
        matrix_arguments[0] = QXP_GL_MODELVIEW;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_MATRIX_MODE,
                                 matrix_arguments, 1) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_LOAD_IDENTITY,
                                 NULL, 0)) goto done;
        }
    }

    stage = 1;
    for (index = 0; index < 4; ++index) {
        if (!texture_state_cached[index] &&
            !qxp_d3d_apply_texture_state(device, request, index)) goto done;
    }
    stage = 2;
    if (!point_sprite_state_cached &&
        !qxp_d3d_apply_point_sprite_state(device, request)) goto done;
    if (!fixed_state_cached) {
        if (object_draw) {
            object_program_material_sources =
                vertex_program == QXP_D3D_POSITIONAL_RANGE_PROGRAM ||
                vertex_program == QXP_D3D_DUAL_POINT_SPECULAR_PROGRAM ||
                vertex_program == QXP_D3D_QUAD_POINT_RANGE_PROGRAM ||
                vertex_program == QXP_D3D_MULTI_POINT_RANGE_PROGRAM ||
                vertex_program == QXP_D3D_SCALABLE_POINT_PROGRAM;
            stage = 20;
            if (packet.hwtl_state->lighting_enable &&
                !(request->fvf & QXP_D3DFVF_NORMAL) &&
                !object_program_material_sources) {
                stage = 22;
            } else if (packet.hwtl_state->lighting_enable &&
                       !qxp_d3d_object_color_material_mode(
                           request, packet.hwtl_state,
                           object_program_material_sources,
                           &object_material_mode,
                           &object_color_material)) {
                stage = 23;
            }
            if (!qxp_d3d_apply_object_lighting(
                    device, request, packet.hwtl_state,
                    object_program_material_sources,
                    vertex_program == QXP_D3D_SCALABLE_POINT_PROGRAM))
                goto done;
        } else {
            stage = 21;
            disable_arguments[0] = QXP_GL_LIGHTING;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                     disable_arguments, 1)) goto done;
            disable_arguments[0] = QXP_GL_COLOR_MATERIAL;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                     disable_arguments, 1)) goto done;
            disable_arguments[0] = QXP_GL_NORMALIZE;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                     disable_arguments, 1)) goto done;
        }
        stage = 3;
        state_arguments[0] = QXP_GL_DITHER;
        if (!qxp_d3d_fifo_submit(
                device, request->dither_enable ? QXP_FENUM_GL_ENABLE :
                                                 QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        state_arguments[0] = QXP_GL_FOG;
        if (!qxp_d3d_fifo_submit(
                device, fog_active ? QXP_FENUM_GL_ENABLE :
                                     QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        if (fog_active) {
            fog_arguments[0] = QXP_GL_FOG_MODE;
            fog_arguments[1] = fog_mode;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_I,
                                     fog_arguments, 2)) goto done;
            fog_arguments[0] = QXP_GL_FOG_COORDINATE_SOURCE;
            fog_arguments[1] = (packet.program || compact_draw) ?
                QXP_GL_FOG_COORDINATE :
                (object_vertex_fog && !vertex_program ?
                 QXP_GL_FRAGMENT_DEPTH : QXP_GL_FOG_COORDINATE);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_I,
                                     fog_arguments, 2)) goto done;
            if (object_vertex_fog && !vertex_program) {
                /* D3D fixed-function vertex fog uses absolute eye-space Z,
                 * or Euclidean eye distance when RANGEFOGENABLE is set. The
                 * 2001 NV_fog_distance modes express those definitions
                 * directly while leaving transformed/table fog on explicit
                 * coordinates. */
                fog_arguments[0] = QXP_GL_FOG_DISTANCE_MODE_NV;
                fog_arguments[1] =
                    packet.hwtl_state->range_fog_enable ?
                    QXP_GL_EYE_RADIAL_NV :
                    QXP_GL_EYE_PLANE_ABSOLUTE_NV;
                if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_I,
                                         fog_arguments, 2)) goto done;
            }
            fog_arguments[0] = QXP_GL_FOG_START;
            fog_arguments[1] = (table_fog || object_vertex_fog) ?
                request->fog_start_bits : 0;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                     fog_arguments, 2)) goto done;
            fog_arguments[0] = QXP_GL_FOG_END;
            fog_arguments[1] = (table_fog || object_vertex_fog) ?
                request->fog_end_bits : 0x3f800000UL;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                     fog_arguments, 2)) goto done;
            fog_arguments[0] = QXP_GL_FOG_DENSITY;
            fog_arguments[1] = request->fog_density_bits;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FOG_F,
                                     fog_arguments, 2)) goto done;
            fog_arguments[0] = QXP_GL_FOG_COLOR;
            fog_arguments[1] = 0;
            fog_color[0] = qxp_float_from_unorm8(
                (UCHAR)(request->fog_color >> 16));
            fog_color[1] = qxp_float_from_unorm8(
                (UCHAR)(request->fog_color >> 8));
            fog_color[2] = qxp_float_from_unorm8((UCHAR)request->fog_color);
            fog_color[3] = qxp_float_from_unorm8(
                (UCHAR)(request->fog_color >> 24));
            if (!qxp_d3d_fifo_submit_input(
                    device, QXP_FENUM_GL_FOG_FV, fog_arguments, 2,
                    fog_color, sizeof(fog_color))) goto done;
        }
        stage = 4;
        color_mask_arguments[0] =
            (request->color_write_enable & 0x1UL) != 0;
        color_mask_arguments[1] =
            (request->color_write_enable & 0x2UL) != 0;
        color_mask_arguments[2] =
            (request->color_write_enable & 0x4UL) != 0;
        color_mask_arguments[3] =
            (request->color_write_enable & 0x8UL) != 0;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_COLOR_MASK,
                                 color_mask_arguments, 4)) goto done;
        stage = 5;
        state_arguments[0] = QXP_GL_COLOR_SUM;
        if (!qxp_d3d_fifo_submit(
                device, (request->specular_enable &&
                         ((packet.program &&
                           (packet.program->written_outputs &
                            QXP_D3D_VS_OUTPUT_COLOR1)) ||
                          positional_range_program ||
                          dual_point_specular_program ||
                          vertex_program ==
                              QXP_D3D_SCALABLE_POINT_PROGRAM ||
                          (request->fvf & QXP_D3DFVF_SPECULAR))) ?
                         QXP_FENUM_GL_ENABLE :
                         QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        stage = 6;
        state_arguments[0] = QXP_GL_BLEND;
        if (!qxp_d3d_fifo_submit(
                device, request->alpha_blend_enable ? QXP_FENUM_GL_ENABLE :
                                                      QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        if (request->alpha_blend_enable &&
            (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLEND_EQUATION,
                                  blend_equation_arguments, 1) ||
             !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLEND_FUNC,
                                  blend_arguments, 2))) goto done;
        stage = 6;
        state_arguments[0] = QXP_GL_ALPHA_TEST;
        if (!qxp_d3d_fifo_submit(
                device, request->alpha_test_enable ? QXP_FENUM_GL_ENABLE :
                                                     QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) {
            qxp_debug("QXPMP D3D DRAW ALPHA ENABLE-");
            goto done;
        }
        if (request->alpha_test_enable) {
            alpha_arguments[0] = QXP_GL_NEVER + request->alpha_function - 1;
            alpha_arguments[1] = qxp_float_from_unorm8(
                (UCHAR)request->alpha_reference);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ALPHA_FUNC,
                                     alpha_arguments, 2)) {
                qxp_debug("QXPMP D3D DRAW ALPHA FUNC-");
                goto done;
            }
        }
        stage = 7;
        state_arguments[0] = QXP_GL_STENCIL_TEST;
        if (!qxp_d3d_fifo_submit(
                device, request->stencil_enable ? QXP_FENUM_GL_ENABLE :
                                                  QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        if (request->stencil_enable) {
            stencil_func_arguments[0] = QXP_GL_NEVER +
                                        request->stencil_function - 1;
            stencil_func_arguments[1] = request->stencil_reference;
            stencil_func_arguments[2] = request->stencil_mask;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_STENCIL_FUNC,
                                     stencil_func_arguments, 3) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_STENCIL_OP,
                                     stencil_op_arguments, 3)) goto done;
            state_arguments[0] = request->stencil_write_mask;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_STENCIL_MASK,
                                     state_arguments, 1)) goto done;
        }

        stage = 8;
        state_arguments[0] = QXP_GL_DEPTH_TEST;
        if (!qxp_d3d_fifo_submit(
                device, request->z_enable ? QXP_FENUM_GL_ENABLE :
                                            QXP_FENUM_GL_DISABLE,
                state_arguments, 1)) goto done;
        state_arguments[0] = QXP_GL_NEVER + request->z_func - 1;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_FUNC,
                                 state_arguments, 1)) goto done;
        state_arguments[0] = request->z_write_enable ? TRUE : FALSE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DEPTH_MASK,
                                 state_arguments, 1)) goto done;

        stage = 9;
        state_arguments[0] = QXP_GL_POLYGON_OFFSET_POINT;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) goto done;
        state_arguments[0] = QXP_GL_POLYGON_OFFSET_LINE;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) goto done;
        state_arguments[0] = QXP_GL_POLYGON_OFFSET_FILL;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_DISABLE,
                                 state_arguments, 1)) goto done;
        if (request->z_bias) {
            state_arguments[0] =
                request->primitive_type == 1 || request->fill_mode == 1 ?
                QXP_GL_POLYGON_OFFSET_POINT :
                (request->primitive_type == 2 || request->fill_mode == 2 ?
                 QXP_GL_POLYGON_OFFSET_LINE : QXP_GL_POLYGON_OFFSET_FILL);
            polygon_offset_arguments[0] = 0;
            polygon_offset_arguments[1] =
                qxp_float_from_negative_uint(request->z_bias);
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_ENABLE,
                                     state_arguments, 1) ||
                !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POLYGON_OFFSET,
                                     polygon_offset_arguments, 2)) goto done;
        }

        stage = 10;
        state_arguments[0] = QXP_GL_CULL_FACE;
        if (!qxp_d3d_fifo_submit(
                device, request->cull_mode == 1 ? QXP_FENUM_GL_DISABLE :
                                                 QXP_FENUM_GL_ENABLE,
                state_arguments, 1)) goto done;
        if (request->cull_mode != 1) {
            state_arguments[0] = QXP_GL_BACK;
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_CULL_FACE,
                                     state_arguments, 1)) goto done;
            /* D3DCULL_CCW discards counter-clockwise faces and therefore keeps
             * clockwise screen-space triangles. Display-space presentation
             * targets retain the default y-down mapping; only ordinary render
             * textures use the opposite vertical projection. */
            state_arguments[0] = request->cull_mode == 3 ? QXP_GL_CW : QXP_GL_CCW;
            if (device->d3d_target_texture &&
                !device->d3d_target_present) {
                state_arguments[0] = state_arguments[0] == QXP_GL_CCW ?
                                     QXP_GL_CW : QXP_GL_CCW;
            }
            if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRONT_FACE,
                                     state_arguments, 1)) goto done;
        }
        stage = 11;
        state_arguments[0] = QXP_GL_FRONT_AND_BACK;
        state_arguments[1] = request->fill_mode == 1 ? QXP_GL_POINT :
                             (request->fill_mode == 2 ? QXP_GL_LINE :
                                                        QXP_GL_FILL);
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_POLYGON_MODE,
                                  state_arguments, 2)) goto done;
        stage = 12;
        if (!qxp_d3d_apply_point_state(device, request)) goto done;
        stage = 130;
        state_arguments[0] = request->shade_mode == 1 ? QXP_GL_FLAT :
                                                        QXP_GL_SMOOTH;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_SHADE_MODEL,
                                 state_arguments, 1)) goto done;
        if (packet.program || compact_draw) {
            stage = 131;
            state_arguments[0] = QXP_GL_FIRST_VERTEX_CONVENTION;
            if (!qxp_d3d_fifo_submit(device,
                                     QXP_FENUM_GL_PROVOKING_VERTEX,
                                     state_arguments, 1)) goto done;
        }
    }

    stage = 132;
#ifdef QXP_VERBOSE_TRACE
    if (device->d3d_draw_count < 3UL) qxp_d3d_trace_gl_state(device);
#endif
#ifdef QXP_D3D_PROFILE
    profile_state_ticks = VideoPortQueryPerformanceCounter(device, NULL);
#endif
    if (!(packet.program ?
          qxp_d3d_submit_program_vertex_arrays(
              device, request, packet.program,
              packet.program_vertex_data, gl_primitive) :
          (raw_object_draw ?
           qxp_d3d_submit_raw_indexed_object_arrays(
               device, request, packet.raw_indexed_object,
               packet.raw_indexed_object_vertex_data,
               packet.raw_indexed_object_index_data,
               coordinate_stages, gl_primitive,
               !array_state_cached) :
           (compact_draw ?
            qxp_d3d_submit_compact_vertex_arrays(
                device, request, packet.compact,
                packet.compact_indexed,
                packet.compact ? packet.compact_vertex_data :
                                 packet.compact_indexed_vertex_data,
                packet.compact_indexed_index_data, coordinate_stages,
                gl_primitive, !array_state_cached) :
            qxp_d3d_submit_vertex_arrays(
                device, request, packet.vertices, packet.vertex_indices,
                packet.source_vertex_count, packet.object_data,
                active_table_fog_columns,
                coordinate_stages, table_fog, fog_array_active,
                primitive_size, gl_primitive, generic_secondary,
                !array_state_cached))))) goto done;
    if (packet.vertex_indices &&
        device->d3d_indexed_canonical_draw_count != 0xffffffffUL)
        ++device->d3d_indexed_canonical_draw_count;
    if (fixed_state_cached &&
        device->d3d_fixed_state_reuse_count != 0xffffffffUL)
        ++device->d3d_fixed_state_reuse_count;
    for (index = 0; index < 4; ++index) {
        if (texture_state_cached[index] &&
            device->d3d_texture_state_reuse_count != 0xffffffffUL)
            ++device->d3d_texture_state_reuse_count;
    }
    if (array_state_cached &&
        device->d3d_array_state_reuse_count != 0xffffffffUL)
        ++device->d3d_array_state_reuse_count;
    qxp_d3d_commit_draw_state(device, request, packet.hwtl_state,
                              coordinate_stages, fog_array_active,
                              generic_secondary);
#ifdef QXP_D3D_PROFILE
    profile_submit_ticks = VideoPortQueryPerformanceCounter(device, NULL);
#endif
    result = TRUE;

done:
#ifdef QXP_D3D_PROFILE
    if (result) {
        qxp_d3d_profile_add(
            &device->d3d_profile_packet_ticks,
            profile_packet_ticks - profile_start_ticks);
        qxp_d3d_profile_add(
            &device->d3d_profile_prepare_ticks,
            profile_prepare_ticks - profile_packet_ticks);
        qxp_d3d_profile_add(
            &device->d3d_profile_state_ticks,
            profile_state_ticks - profile_prepare_ticks);
        qxp_d3d_profile_add(
            &device->d3d_profile_submit_ticks,
            profile_submit_ticks - profile_state_ticks);
    }
#endif
    if (!result) {
        qxp_debug("QXPMP D3D DRAW STAGE-");
        qxp_debug_status(stage);
        fifo[0] = fifo_start;
        data[0] = data_start;
        /* Program binding commands were part of the discarded FIFO suffix.
         * Keep the software shadow aligned with the still-live GL bindings so
         * a later draw cannot incorrectly skip the required rebind. */
        device->d3d_pixel_program = previous_pixel_program;
        device->d3d_vertex_program = previous_vertex_program;
        device->d3d_vertex_program_point_size =
            previous_vertex_program_point_size;
        device->d3d_vertex_program_clip = previous_vertex_program_clip;
        device->d3d_neutral_texture_defined =
            previous_neutral_texture_defined;
        qxp_d3d_invalidate_draw_state(device);
    }
    return result;
}

static BOOLEAN qxp_d3d_draw_batch_transport(
    QXP_DEVICE_EXTENSION *device,
    const QXP_D3D_DRAW_BATCH_REQUEST *request, ULONG request_bytes)
{
    QXP_D3D_DRAW_BATCH_VIEW view;
    const QXP_D3D_DRAW_REQUEST *draw;
    ULONG draw_bytes;

    if (!device) return FALSE;
    if (!qxp_packet_view_draw_batch(request, request_bytes, &view)) {
        if (!device->d3d_batch_failure_logged) {
            device->d3d_batch_failure_logged = TRUE;
            qxp_debug("QXPMP D3D DRAW BATCH VIEW-");
            qxp_debug_status(view.failure_reason);
            qxp_debug_status(view.failure_entry);
            qxp_debug_status(request_bytes);
            qxp_debug_status(view.failure_draw_bytes);
            if (view.failure_draw) {
                qxp_debug_status(view.failure_draw->version);
                qxp_debug_status(view.failure_draw->vertex_count);
                qxp_debug_status(view.failure_draw->vertex_mode);
                qxp_debug_status(view.failure_draw->object_data_offset);
                qxp_debug_status(view.failure_draw->hwtl_state_offset);
            }
        }
        return FALSE;
    }
    if (!device->d3d_active ||
        request->process_id != device->d3d_process_id) {
        if (!device->d3d_batch_failure_logged) {
            device->d3d_batch_failure_logged = TRUE;
            qxp_debug("QXPMP D3D DRAW BATCH OWNER-");
            qxp_debug_status(device->d3d_active);
            qxp_debug_status(device->d3d_process_id);
            qxp_debug_status(request->process_id);
        }
        return FALSE;
    }
    if (request->draw_count > device->d3d_draw_batch_max_requests)
        device->d3d_draw_batch_max_requests = request->draw_count;
    while (view.remaining) {
        if (!qxp_packet_next_batch_draw(&view, &draw, &draw_bytes) ||
            !qxp_d3d_draw_transport(device, draw, draw_bytes)) return FALSE;
        if (device->d3d_draw_count != 0xffffffffUL)
            ++device->d3d_draw_count;
        if (device->d3d_draw_count == 1) qxp_debug("QXPMP D3D DRAW+");
    }
    return TRUE;
}

/* Presents guest video memory that the CPU authored (a composited primary,
 * e.g. POP:SoT's menu): uploads the described span into a scratch texture
 * and blits it to output framebuffer zero before publishing. The legacy
 * path below presents the session's current render-target texture and
 * cannot show CPU-written pixels; POP flips such primaries in a window
 * where no render target has been set at all. */
static BOOLEAN qxp_d3d_present_cpu_transport(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_PRESENT_REQUEST *request,
    BOOLEAN exchange_buffers)
{
    PHYSICAL_ADDRESS physical;
    volatile ULONG *fifo;
    volatile ULONG *swap_control;
    ULONG address_space =
        VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_P6CACHE;
    ULONG bind_texture_arguments[2];
    ULONG store_arguments[2];
    ULONG image_arguments[9];
    ULONG bind_read_arguments[2];
    ULONG attach_arguments[5];
    ULONG bind_draw_arguments[2];
    ULONG blit_arguments[10];
    ULONG restore_arguments[2];
    ULONG internal_format = 0;
    ULONG pixel_format = 0;
    ULONG pixel_type = 0;
    ULONG bytes_per_pixel = 0;
    ULONG row_bytes;
    ULONG total_bytes;
    ULONG span_bytes;
    ULONG page_offset;
    ULONG map_length;
    ULONG fifo_start;
    ULONG row;
    PVOID mapped = NULL;
    PUCHAR source;
    PUCHAR destination;

    if (!device->d3d_active ||
        request->process_id != device->d3d_process_id) return FALSE;
    if (!device->mesa_fifo.mapped || !device->mesa_fbtm.mapped)
        return FALSE;
    if (!qxp_d3d_texture_format(request->format, &internal_format,
                                &pixel_format, &pixel_type,
                                &bytes_per_pixel)) return FALSE;
    row_bytes = request->width * bytes_per_pixel;
    total_bytes = request->height * row_bytes;
    span_bytes = (request->height - 1UL) * request->pitch + row_bytes;
    if (total_bytes > QXP_MESA_FBTM_SIZE ||
        request->vidmem_offset >= device->framebuffer.length ||
        span_bytes > device->framebuffer.length - request->vidmem_offset)
        return FALSE;

    /* Stage the guest image at the FBT base, top row first, no padding. */
    page_offset = request->vidmem_offset & 0xfffUL;
    map_length = page_offset + span_bytes;
    physical.QuadPart = device->framebuffer.physical.QuadPart +
                        (request->vidmem_offset - page_offset);
    if (VideoPortMapMemory(device, physical, &map_length, &address_space,
                           &mapped) != NO_ERROR || !mapped) {
        qxp_debug("QXPMP D3D PRESENT MAP-");
        return FALSE;
    }
    source = (PUCHAR)mapped + page_offset;
    destination = device->mesa_fbtm.mapped;
    for (row = 0; row < request->height; ++row) {
        VideoPortMoveMemory((PVOID)destination, source, row_bytes);
        source += request->pitch;
        destination += row_bytes;
    }
    VideoPortUnmapMemory(device, mapped, NULL);

    if (!device->d3d_copy_framebuffer &&
        !qxp_d3d_generate_name(
            device, QXP_FENUM_GL_GEN_FRAMEBUFFERS,
            &device->d3d_copy_framebuffer)) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    qxp_d3d_finish_draw_batch(device);

    /* The scratch name is reserved transport-wide (never allocated by the
     * display, rejected in guest texture requests), so binding it here
     * creates or reuses the miniport's private object without any risk of
     * aliasing a game texture; TexImage2D respecifies it every present. */
    fifo_start = fifo[0];
    bind_texture_arguments[0] = QXP_GL_TEXTURE_2D;
    bind_texture_arguments[1] = QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME;
    store_arguments[0] = QXP_GL_UNPACK_ALIGNMENT;
    store_arguments[1] = 1;
    image_arguments[0] = QXP_GL_TEXTURE_2D;
    image_arguments[1] = 0;
    image_arguments[2] = internal_format;
    image_arguments[3] = request->width;
    image_arguments[4] = request->height;
    image_arguments[5] = 0;
    image_arguments[6] = pixel_format;
    image_arguments[7] = pixel_type;
    image_arguments[8] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_TEXTURE,
                             bind_texture_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_PIXEL_STORE_I,
                             store_arguments, 2) ||
        !qxp_d3d_call_fbtm(device, QXP_FENUM_GL_TEX_IMAGE_2D,
                           image_arguments, 9, NULL, 0)) {
        fifo[0] = fifo_start;
        return FALSE;
    }

    fifo_start = fifo[0];
    bind_read_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    bind_read_arguments[1] = device->d3d_copy_framebuffer;
    attach_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    attach_arguments[2] = QXP_GL_TEXTURE_2D;
    attach_arguments[3] = QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME;
    attach_arguments[4] = 0;
    bind_draw_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
    bind_draw_arguments[1] = 0;
    blit_arguments[0] = 0;
    blit_arguments[1] = 0;
    blit_arguments[2] = request->width;
    blit_arguments[3] = request->height;
    blit_arguments[4] = 0;
    blit_arguments[5] = 0;
    blit_arguments[6] = request->width;
    blit_arguments[7] = request->height;
    blit_arguments[8] = QXP_GL_COLOR_BUFFER_BIT;
    blit_arguments[9] = QXP_GL_NEAREST;
    restore_arguments[0] = QXP_GL_FRAMEBUFFER;
    restore_arguments[1] = device->d3d_framebuffer;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_read_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             attach_arguments, 5) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_draw_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLIT_FRAMEBUFFER,
                             blit_arguments, 10)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    /* Detach the scratch texture and restore the session framebuffer
     * binding (zero binds the default framebuffer when no render target
     * has been created in this session, which is exactly the state POP's
     * menu-era flips arrive in).  The detach and restore belong to the
     * same transaction as the blit: on capacity failure the rollback
     * unwinds the whole group, so the host never sees a present that
     * leaves the copy framebuffer or binding dirty. */
    attach_arguments[3] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             attach_arguments, 5) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             restore_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    qxp_d3d_invalidate_draw_state(device);

    swap_control = (volatile ULONG *)(device->mesa_fifo.mapped +
                                      QXP_MESA_FIFO_SIZE - 8UL);
    swap_control[0] = 0;
    swap_control[1] = 0;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, exchange_buffers ? 0xff0 : 0xfd4,
                        QXP_MESAGL_MAGIC);
    if (!device->d3d_present_cpu_count)
        qxp_debug("QXPMP D3D PRESENT CPU");
    if (device->d3d_present_cpu_count != 0xffffffffUL)
        ++device->d3d_present_cpu_count;
    return TRUE;
}

/* Presents a named host texture (surface-level GPU authority: an
 * accelerated copy or draw produced the flipped surface's newest image and
 * no readback has consumed it).  Same output path as the CPU-source
 * present, minus the upload. */
static BOOLEAN qxp_d3d_present_texture_transport(
    QXP_DEVICE_EXTENSION *device, const QXP_D3D_PRESENT_REQUEST *request,
    BOOLEAN exchange_buffers)
{
    QXP_D3D_TEXTURE_STATE *texture;
    volatile ULONG *fifo;
    volatile ULONG *swap_control;
    ULONG bind_read_arguments[2];
    ULONG attach_arguments[5];
    ULONG bind_draw_arguments[2];
    ULONG blit_arguments[10];
    ULONG restore_arguments[2];
    ULONG fifo_start;

    if (!device->d3d_active ||
        request->process_id != device->d3d_process_id) return FALSE;
    if (!device->mesa_fifo.mapped) return FALSE;
    texture = qxp_d3d_find_texture(device, request->process_id,
                                   request->texture_name);
    if (!texture || texture->texture_type != QXP_D3D_TEXTURE_2D ||
        !texture->width || !texture->height ||
        !(texture->defined_levels[0] & 1UL)) return FALSE;
    if (!device->d3d_copy_framebuffer &&
        !qxp_d3d_generate_name(
            device, QXP_FENUM_GL_GEN_FRAMEBUFFERS,
            &device->d3d_copy_framebuffer)) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    qxp_d3d_finish_draw_batch(device);

    fifo_start = fifo[0];
    bind_read_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    bind_read_arguments[1] = device->d3d_copy_framebuffer;
    attach_arguments[0] = QXP_GL_READ_FRAMEBUFFER;
    attach_arguments[1] = QXP_GL_COLOR_ATTACHMENT0;
    attach_arguments[2] = QXP_GL_TEXTURE_2D;
    attach_arguments[3] = request->texture_name;
    attach_arguments[4] = 0;
    bind_draw_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
    bind_draw_arguments[1] = 0;
    blit_arguments[0] = 0;
    blit_arguments[1] = 0;
    blit_arguments[2] = texture->width;
    blit_arguments[3] = texture->height;
    blit_arguments[4] = 0;
    blit_arguments[5] = 0;
    blit_arguments[6] = texture->width;
    blit_arguments[7] = texture->height;
    blit_arguments[8] = QXP_GL_COLOR_BUFFER_BIT;
    blit_arguments[9] = QXP_GL_NEAREST;
    restore_arguments[0] = QXP_GL_FRAMEBUFFER;
    restore_arguments[1] = device->d3d_framebuffer;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_read_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             attach_arguments, 5) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             bind_draw_arguments, 2) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLIT_FRAMEBUFFER,
                             blit_arguments, 10)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    /* Detach so the copy framebuffer never holds a texture the guest may
     * later delete, and restore the session framebuffer binding.  Checked
     * as part of the present transaction: a capacity failure rolls the
     * whole group back so no partially-restored present ever publishes. */
    attach_arguments[3] = 0;
    if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_FRAMEBUFFER_TEXTURE_2D,
                             attach_arguments, 5) ||
        !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                             restore_arguments, 2)) {
        fifo[0] = fifo_start;
        return FALSE;
    }
    qxp_d3d_invalidate_draw_state(device);

    swap_control = (volatile ULONG *)(device->mesa_fifo.mapped +
                                      QXP_MESA_FIFO_SIZE - 8UL);
    swap_control[0] = 0;
    swap_control[1] = 0;
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    qxp_mesa_mmio_write(device, exchange_buffers ? 0xff0 : 0xfd4,
                        QXP_MESAGL_MAGIC);
    if (!device->d3d_present_texture_count)
        qxp_debug("QXPMP D3D PRESENT SURFACE TEXTURE");
    if (device->d3d_present_texture_count != 0xffffffffUL)
        ++device->d3d_present_texture_count;
    return TRUE;
}

static BOOLEAN qxp_d3d_present_transport(QXP_DEVICE_EXTENSION *device,
                                          ULONG process_id,
                                          BOOLEAN exchange_buffers)
{
    QXP_D3D_TEXTURE_STATE *texture;
    volatile ULONG *fifo;
    volatile ULONG *swap_control;
    ULONG fifo_start;
    ULONG bind_draw_arguments[2];
    ULONG blit_arguments[10];
    ULONG restore_arguments[2];

    if (!device->d3d_active || process_id != device->d3d_process_id) {
        return FALSE;
    }
    if (!device->mesa_fifo.mapped) return FALSE;
    fifo = (volatile ULONG *)device->mesa_fifo.mapped;
    /* COPY-style swap chains preserve every member independently. Rendering
     * stays in the selected surface texture, then Present copies that image
     * to framebuffer zero for the frontend. Restore the render FBO before
     * ringing the publish/swap doorbell so later draws remain on the selected
     * surface without another target transition. */
    if (device->d3d_target_texture) {
        texture = qxp_d3d_find_texture(device, process_id,
                                       device->d3d_target_texture);
        if (!texture || !device->d3d_framebuffer || !texture->width ||
            !texture->height) return FALSE;
        if (!device->d3d_present_count)
            qxp_debug("QXPMP D3D PRESENT TEXTURE");
        fifo_start = fifo[0];
        bind_draw_arguments[0] = QXP_GL_DRAW_FRAMEBUFFER;
        bind_draw_arguments[1] = 0;
        blit_arguments[0] = 0;
        blit_arguments[1] = 0;
        blit_arguments[2] = texture->width;
        blit_arguments[3] = texture->height;
        blit_arguments[4] = 0;
        blit_arguments[5] = 0;
        blit_arguments[6] = texture->width;
        blit_arguments[7] = texture->height;
        blit_arguments[8] = QXP_GL_COLOR_BUFFER_BIT;
        blit_arguments[9] = QXP_GL_NEAREST;
        restore_arguments[0] = QXP_GL_FRAMEBUFFER;
        restore_arguments[1] = device->d3d_framebuffer;
        if (!qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                                 bind_draw_arguments, 2) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BLIT_FRAMEBUFFER,
                                 blit_arguments, 10) ||
            !qxp_d3d_fifo_submit(device, QXP_FENUM_GL_BIND_FRAMEBUFFER,
                                 restore_arguments, 2)) {
            fifo[0] = fifo_start;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
    }
    /* Publishing with no presentable target republishes the previous host
     * frame.  Legitimate for windowed COPY publishes before any target
     * exists, but historically this silently swallowed every menu present
     * of a CPU-composited chain, so it stays loudly observable. */
    if (!device->d3d_target_texture) {
        if (!device->d3d_present_empty_count)
            qxp_debug("QXPMP D3D PRESENT EMPTY");
        if (device->d3d_present_empty_count != 0xffffffffUL)
            ++device->d3d_present_empty_count;
    }
    /* 0xff0 uses the final aligned pair as an in/out swap-control block.
     * Always clear the request before ringing the doorbell.  Reusing the
     * host's odd success value feeds it back as an exponentially increasing
     * FPS limit (1, 3, 7, ...), eventually throttling every presentation. */
    swap_control = (volatile ULONG *)(device->mesa_fifo.mapped +
                                      QXP_MESA_FIFO_SIZE - 8UL);
    swap_control[0] = 0;
    swap_control[1] = 0;
    qxp_d3d_finish_draw_batch(device);
    qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    /* A DirectDraw Flip exchanges the host double buffer. Windowed D3D COPY
     * presentation must publish the completed backbuffer without exchanging
     * it, because D3D8 guarantees that Present preserves its contents. */
    qxp_mesa_mmio_write(device, exchange_buffers ? 0xff0 : 0xfd4,
                        QXP_MESAGL_MAGIC);
    return TRUE;
}

static BOOLEAN qxp_d3d_session_request_valid(PVIDEO_REQUEST_PACKET packet,
                                             QXP_D3D_SESSION_REQUEST **request)
{
    if (packet->InputBufferLength < sizeof(**request)) return FALSE;
    *request = packet->InputBuffer;
    return (*request)->version == QXP_D3D_TRANSPORT_VERSION &&
           (*request)->process_id != 0;
}

/* The linear framebuffer is host RAM (the guest video memory the DirectDraw
 * surfaces live in), not real device MMIO. XP's stock windowed present runs a
 * software Blt that READS the whole framebuffer every frame, and the game
 * locks its surfaces (Lock/Unlock) to read and write pixels each frame. An
 * uncached mapping forces all of that through uncached memory (~37 MB/s
 * measured), which dominates frame time under WHPX and is invisible under TCG.
 * Map the framebuffer write-back cached (P6CACHE) for both the kernel
 * (present/GDI) and the user-mode (surface Lock) views below. Scanout
 * coherence holds on x86: QEMU's display reads the framebuffer RAMBlock with
 * ordinary host CPU loads, so MESI snooping returns the guest's latest cached
 * stores, and QEMU's dirty-page tracking is enforced at the EPT/host-page
 * level, independent of the guest cache attribute, so display refresh still
 * observes every modified page. */
static BOOLEAN qxp_map_memory(QXP_DEVICE_EXTENSION *device,
                              PVIDEO_REQUEST_PACKET request)
{
    PVIDEO_MEMORY input = request->InputBuffer;
    PVIDEO_MEMORY_INFORMATION output = request->OutputBuffer;
    ULONG address_space =
        VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_P6CACHE;
    ULONG length = device->framebuffer.length;
    VP_STATUS status;

    qxp_debug("QXPMP M");
    if (request->InputBufferLength < sizeof(*input) ||
        request->OutputBufferLength < sizeof(*output)) {
        request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
        qxp_debug("QXPMP M1");
        return FALSE;
    }
    VideoPortZeroMemory(output, sizeof(*output));
    output->VideoRamBase = input->RequestedVirtualAddress;
    status = VideoPortMapMemory(device, device->framebuffer.physical,
                                &length, &address_space,
                                &output->VideoRamBase);
    if (status != NO_ERROR) {
        request->StatusBlock->Status = status;
        qxp_debug("QXPMP M2");
        return FALSE;
    }
    output->VideoRamLength = length;
    output->FrameBufferBase = output->VideoRamBase;
    output->FrameBufferLength = length;
    request->StatusBlock->Information = sizeof(*output);
    request->StatusBlock->Status = NO_ERROR;
    qxp_debug("QXPMP M+");
    return TRUE;
}

static BOOLEAN qxp_share_memory(QXP_DEVICE_EXTENSION *device,
                                PVIDEO_REQUEST_PACKET request)
{
    PVIDEO_SHARE_MEMORY input = request->InputBuffer;
    PVIDEO_SHARE_MEMORY_INFORMATION output = request->OutputBuffer;
    PHYSICAL_ADDRESS physical;
    PVOID virtual_address;
    ULONG address_space;
    ULONG length;
    VP_STATUS status;

    qxp_debug("QXPMP SH");
    if (request->InputBufferLength < sizeof(*input) ||
        request->OutputBufferLength < sizeof(*output) ||
        !input->ProcessHandle ||
        input->ViewOffset > device->framebuffer.length) {
        request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
        qxp_debug("QXPMP SH1");
        return FALSE;
    }
    length = input->ViewSize;
    if (!length) length = device->framebuffer.length - input->ViewOffset;
    if (length > device->framebuffer.length - input->ViewOffset) {
        request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
        qxp_debug("QXPMP SH2");
        return FALSE;
    }

    physical = device->framebuffer.physical;
    physical.QuadPart += input->ViewOffset;
    /* Write-back cached user-mode view of the framebuffer; see qxp_map_memory
     * for the scanout-coherence rationale. */
    address_space = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_USER_MODE |
                    VIDEO_MEMORY_SPACE_P6CACHE;
    virtual_address = input->ProcessHandle;
    status = VideoPortMapMemory(device, physical, &length, &address_space,
                                &virtual_address);
    if (status != NO_ERROR) {
        request->StatusBlock->Status = status;
        qxp_debug("QXPMP SH3");
        return FALSE;
    }

    VideoPortZeroMemory(output, sizeof(*output));
    output->SharedViewOffset = input->ViewOffset;
    output->SharedViewSize = length;
    output->VirtualAddress = virtual_address;
    request->StatusBlock->Information = sizeof(*output);
    request->StatusBlock->Status = NO_ERROR;
    qxp_debug("QXPMP SH+");
    return TRUE;
}

static BOOLEAN qxp_unshare_memory(QXP_DEVICE_EXTENSION *device,
                                  PVIDEO_REQUEST_PACKET request)
{
    PVIDEO_SHARE_MEMORY input = request->InputBuffer;
    VP_STATUS status;

    qxp_debug("QXPMP USH");
    if (request->InputBufferLength < sizeof(*input) ||
        !input->ProcessHandle || !input->RequestedVirtualAddress) {
        request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
        qxp_debug("QXPMP USH1");
        return FALSE;
    }
    status = VideoPortUnmapMemory(device, input->RequestedVirtualAddress,
                                  input->ProcessHandle);
    request->StatusBlock->Status = status;
    if (status != NO_ERROR) {
        qxp_debug("QXPMP USH2");
        return FALSE;
    }
    qxp_debug("QXPMP USH+");
    return TRUE;
}

BOOLEAN QxpStartIO(PVOID extension, PVIDEO_REQUEST_PACKET request)
{
    QXP_DEVICE_EXTENSION *device = extension;
    QXP_D3D_SESSION_REQUEST *session_request;
    QXP_D3D_PRESENT_REQUEST *present_request;
    QXP_D3D_CLEAR_REQUEST *clear_request;
    QXP_D3D_TARGET_REQUEST *target_request;
    QXP_D3D_SURFACE_COPY_REQUEST *surface_copy_request;
    QXP_D3D_SURFACE_READBACK_REQUEST *surface_readback_request;
    QXP_D3D_DRAW_REQUEST *draw_request;
    QXP_D3D_DRAW_BATCH_REQUEST *draw_batch_request;
    QXP_D3D_TEXTURE_DEFINE_REQUEST *texture_define_request;
    QXP_D3D_TEXTURE_UPLOAD_REQUEST *texture_upload_request;
    QXP_D3D_TEXTURE_DELETE_REQUEST *texture_delete_request;
    QXP_D3D_SHADER_DEFINE_REQUEST *shader_define_request;
    QXP_D3D_SHADER_DELETE_REQUEST *shader_delete_request;
    QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *sm2_program_define_request;
    QXP_D3D_SM2_PROGRAM_DELETE_REQUEST *sm2_program_delete_request;
    QXP_D3D_SM2_CONSTANTS_REQUEST *sm2_constants_request;
    ULONG index;
#ifdef QXP_D3D_PROFILE
    LONGLONG profile_start_ticks;
#endif

#ifdef QXP_VERBOSE_TRACE
    qxp_debug("QXPMP IO");
    qxp_debug_status(request->IoControlCode);
#endif
    request->StatusBlock->Information = 0;
    request->StatusBlock->Status = ERROR_INVALID_FUNCTION;
    switch (request->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        if (request->OutputBufferLength < sizeof(VIDEO_NUM_MODES)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        ((PVIDEO_NUM_MODES)request->OutputBuffer)->NumModes = device->mode_count;
        ((PVIDEO_NUM_MODES)request->OutputBuffer)->ModeInformationLength =
            sizeof(VIDEO_MODE_INFORMATION);
        request->StatusBlock->Information = sizeof(VIDEO_NUM_MODES);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        if (request->OutputBufferLength <
            device->mode_count * sizeof(VIDEO_MODE_INFORMATION)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        for (index = 0; index < device->mode_count; ++index) {
            qxp_fill_mode(device, index,
                          &((PVIDEO_MODE_INFORMATION)request->OutputBuffer)[index]);
        }
        request->StatusBlock->Information =
            device->mode_count * sizeof(VIDEO_MODE_INFORMATION);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        if (request->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION) ||
            device->current_mode >= device->mode_count) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        qxp_fill_mode(device, device->current_mode,
                      (PVIDEO_MODE_INFORMATION)request->OutputBuffer);
        request->StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_SET_CURRENT_MODE:
        qxp_debug("QXPMP S");
        if (request->InputBufferLength < sizeof(VIDEO_MODE)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            qxp_debug("QXPMP S1");
            return FALSE;
        }
        index = ((PVIDEO_MODE)request->InputBuffer)->RequestedMode & 0x3FFFFFFF;
        if (!qxp_set_mode(device, index)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            qxp_debug("QXPMP S2");
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        qxp_debug("QXPMP S+");
        return TRUE;

    case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        return qxp_map_memory(device, request);

    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        if (request->InputBufferLength < sizeof(VIDEO_MEMORY)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        request->StatusBlock->Status = VideoPortUnmapMemory(
            device,
            ((PVIDEO_MEMORY)request->InputBuffer)->RequestedVirtualAddress,
            NULL);
        return request->StatusBlock->Status == NO_ERROR;

    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:
        return qxp_share_memory(device, request);

    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY:
        return qxp_unshare_memory(device, request);

    case IOCTL_VIDEO_QXP_D3D_OPEN:
        if (!qxp_d3d_session_request_valid(request, &session_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        if (!qxp_d3d_open_transport(device,
                                    session_request->process_id)) {
            request->StatusBlock->Status = ERROR_BUSY;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_CLOSE:
        if (!qxp_d3d_session_request_valid(request, &session_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        if (!qxp_d3d_close_transport(device,
                                     session_request->process_id)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_CLEAR:
        if (request->InputBufferLength < sizeof(*clear_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        clear_request = request->InputBuffer;
        if (clear_request->version != QXP_D3D_TRANSPORT_VERSION ||
            !clear_request->process_id ||
            !qxp_d3d_clear_transport(device, clear_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        if (device->d3d_clear_count != 0xffffffffUL)
            ++device->d3d_clear_count;
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_DRAW:
        if (request->InputBufferLength <
            sizeof(*draw_request) - sizeof(draw_request->vertices)) {
            qxp_debug("QXPMP D3D DRAW SHORT-");
            qxp_debug_status(request->InputBufferLength);
            qxp_debug_status(sizeof(*draw_request) -
                             sizeof(draw_request->vertices));
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        draw_request = request->InputBuffer;
        if (device->d3d_draw_ioctl_count != 0xffffffffUL)
            ++device->d3d_draw_ioctl_count;
#ifdef QXP_D3D_PROFILE
        profile_start_ticks =
            VideoPortQueryPerformanceCounter(device, NULL);
#endif
        if (!qxp_d3d_draw_transport(device, draw_request,
                                    request->InputBufferLength)) {
#ifdef QXP_D3D_PROFILE
            qxp_d3d_profile_finish_draw(device, profile_start_ticks);
#endif
            qxp_debug("QXPMP D3D DRAW TRANSPORT-");
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
#ifdef QXP_D3D_PROFILE
        qxp_d3d_profile_finish_draw(device, profile_start_ticks);
#endif
        if (device->d3d_draw_count != 0xffffffffUL)
            ++device->d3d_draw_count;
        if (device->d3d_draw_count == 1) qxp_debug("QXPMP D3D DRAW+");
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_DRAW_BATCH:
        if (request->InputBufferLength <
                QXP_D3D_DRAW_BATCH_HEADER_BYTES ||
            request->InputBufferLength > QXP_D3D_MAX_DRAW_BATCH_BYTES) {
            qxp_debug("QXPMP D3D DRAW BATCH SIZE-");
            qxp_debug_status(request->InputBufferLength);
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        draw_batch_request = request->InputBuffer;
        if (device->d3d_draw_batch_ioctl_count != 0xffffffffUL)
            ++device->d3d_draw_batch_ioctl_count;
#ifdef QXP_D3D_PROFILE
        profile_start_ticks =
            VideoPortQueryPerformanceCounter(device, NULL);
#endif
        if (!qxp_d3d_draw_batch_transport(
                device, draw_batch_request,
                request->InputBufferLength)) {
#ifdef QXP_D3D_PROFILE
            qxp_d3d_profile_finish_draw(device, profile_start_ticks);
#endif
            qxp_debug("QXPMP D3D DRAW BATCH TRANSPORT-");
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
#ifdef QXP_D3D_PROFILE
        qxp_d3d_profile_finish_draw(device, profile_start_ticks);
#endif
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_PRESENT:
    case IOCTL_VIDEO_QXP_D3D_PUBLISH:
        if (request->InputBufferLength < sizeof(*present_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        if (request->InputBufferLength != sizeof(*present_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        present_request = request->InputBuffer;
        if (!qxp_packet_present_contract(present_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        if (!device->d3d_active || !device->d3d_references ||
            present_request->process_id != device->d3d_process_id) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        if (present_request->flags & QXP_D3D_PRESENT_CPU_SOURCE) {
            if (!qxp_d3d_present_cpu_transport(
                    device, present_request,
                    request->IoControlCode ==
                        IOCTL_VIDEO_QXP_D3D_PRESENT)) {
                request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
                return FALSE;
            }
        } else if (present_request->flags & QXP_D3D_PRESENT_GPU_TEXTURE) {
            if (!qxp_d3d_present_texture_transport(
                    device, present_request,
                    request->IoControlCode ==
                        IOCTL_VIDEO_QXP_D3D_PRESENT)) {
                request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
                return FALSE;
            }
        } else if (!qxp_d3d_present_transport(
                device, present_request->process_id,
                request->IoControlCode == IOCTL_VIDEO_QXP_D3D_PRESENT)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        if (device->d3d_present_count != 0xffffffffUL)
            ++device->d3d_present_count;
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SET_TARGET:
        if (request->InputBufferLength < sizeof(*target_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        target_request = request->InputBuffer;
        if (!qxp_d3d_target_transport(device, target_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SURFACE_COPY:
        if (request->InputBufferLength < sizeof(*surface_copy_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        if (request->InputBufferLength != sizeof(*surface_copy_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        surface_copy_request = request->InputBuffer;
        if (!qxp_d3d_surface_copy_transport(
                device, surface_copy_request)) {
            qxp_debug("QXPMP D3D SURFACE COPY-");
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SURFACE_READBACK:
        if (request->InputBufferLength < sizeof(*surface_readback_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        if (request->InputBufferLength !=
            sizeof(*surface_readback_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        surface_readback_request = request->InputBuffer;
        if (!qxp_d3d_surface_readback_transport(
                device, surface_readback_request)) {
            qxp_debug("QXPMP D3D SURFACE READBACK-");
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_TEXTURE_DEFINE:
        if (request->InputBufferLength < sizeof(*texture_define_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        texture_define_request = request->InputBuffer;
        if (texture_define_request->version != QXP_D3D_TRANSPORT_VERSION ||
            !qxp_d3d_texture_define_transport(device,
                                               texture_define_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_TEXTURE_UPLOAD:
        if (request->InputBufferLength <
            sizeof(*texture_upload_request) -
                sizeof(texture_upload_request->data)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        texture_upload_request = request->InputBuffer;
        if (texture_upload_request->version != QXP_D3D_TRANSPORT_VERSION ||
            !qxp_d3d_texture_upload_transport(
                device, texture_upload_request,
                request->InputBufferLength)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        if (device->d3d_texture_upload_count != 0xffffffffUL)
            ++device->d3d_texture_upload_count;
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_TEXTURE_DELETE:
        if (request->InputBufferLength < sizeof(*texture_delete_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        texture_delete_request = request->InputBuffer;
        if (texture_delete_request->version != QXP_D3D_TRANSPORT_VERSION ||
            !qxp_d3d_texture_delete_transport(device,
                                               texture_delete_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SM2_PROGRAM_DEFINE:
        if (request->InputBufferLength < 5UL * sizeof(ULONG)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        sm2_program_define_request = request->InputBuffer;
        qxp_debug("QXPMP D3D SM2 DEFINE");
        qxp_debug_status(request->InputBufferLength);
        qxp_debug_status(sm2_program_define_request->version);
        qxp_debug_status(sm2_program_define_request->process_id);
        qxp_debug_status(sm2_program_define_request->program_name);
        qxp_debug_status(sm2_program_define_request->vertex_source_bytes);
        qxp_debug_status(sm2_program_define_request->pixel_source_bytes);
        if (!qxp_d3d_sm2_program_define_transport(
                device, sm2_program_define_request,
                request->InputBufferLength)) {
            qxp_debug("QXPMP D3D SM2 DEFINE-");
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SM2_PROGRAM_DELETE:
        if (request->InputBufferLength <
            sizeof(*sm2_program_delete_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        sm2_program_delete_request = request->InputBuffer;
        if (!qxp_d3d_sm2_program_delete_transport(
                device, sm2_program_delete_request,
                request->InputBufferLength)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SM2_CONSTANTS:
        if (request->InputBufferLength <
            7UL * sizeof(ULONG)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        sm2_constants_request = request->InputBuffer;
        if (!qxp_d3d_sm2_constants_transport(
                device, sm2_constants_request,
                request->InputBufferLength)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SHADER_DEFINE:
        if (request->InputBufferLength <
            sizeof(*shader_define_request) -
                sizeof(shader_define_request->source)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        shader_define_request = request->InputBuffer;
        if (!qxp_d3d_shader_define_transport(
                device, shader_define_request,
                request->InputBufferLength)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_QXP_D3D_SHADER_DELETE:
        if (request->InputBufferLength < sizeof(*shader_delete_request)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        shader_delete_request = request->InputBuffer;
        if (!qxp_d3d_shader_delete_transport(device,
                                              shader_delete_request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        qxp_d3d_invalidate_draw_state(device);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_RESET_DEVICE:
        qxp_write(device, QXP_DISPI_INDEX_ENABLE, QXP_DISPI_DISABLED);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        if (!qxp_set_color_registers(request)) {
            request->StatusBlock->Status = ERROR_INVALID_PARAMETER;
            return FALSE;
        }
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;

    case IOCTL_VIDEO_GET_CHILD_STATE:
        if (request->OutputBufferLength < sizeof(ULONG)) {
            request->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
            return FALSE;
        }
        *(PULONG)request->OutputBuffer = VIDEO_CHILD_ACTIVE;
        request->StatusBlock->Information = sizeof(ULONG);
        request->StatusBlock->Status = NO_ERROR;
        return TRUE;
    }
    return FALSE;
}

static VOID QxpTimer(PVOID extension)
{
    QXP_DEVICE_EXTENSION *device = extension;

    if (device->d3d_active && device->d3d_token) {
        qxp_mesa_mmio_write(device, 0xfb0, device->d3d_token);
    }
}

BOOLEAN QxpResetHw(PVOID extension, ULONG columns, ULONG rows)
{
    QXP_DEVICE_EXTENSION *device = extension;

    qxp_debug("QXPMP R");
    UNREFERENCED_PARAMETER(columns);
    UNREFERENCED_PARAMETER(rows);
    qxp_write(device, QXP_DISPI_INDEX_ENABLE, QXP_DISPI_DISABLED);
    /* We have left VBE graphics mode, but the HAL must run int10 to restore
     * a real VGA text mode before it can paint a bugcheck screen. */
    return FALSE;
}

static VP_STATUS QxpSetPowerState(PVOID extension, ULONG hardware_id,
                                  PVIDEO_POWER_MANAGEMENT power)
{
    qxp_debug("QXPMP PS");
    UNREFERENCED_PARAMETER(extension);
    UNREFERENCED_PARAMETER(hardware_id);
    UNREFERENCED_PARAMETER(power);
    return NO_ERROR;
}

static VP_STATUS QxpGetPowerState(PVOID extension, ULONG hardware_id,
                                  PVIDEO_POWER_MANAGEMENT power)
{
    qxp_debug("QXPMP PG");
    UNREFERENCED_PARAMETER(extension);
    UNREFERENCED_PARAMETER(hardware_id);
    UNREFERENCED_PARAMETER(power);
    return NO_ERROR;
}

static VP_STATUS QxpGetChildDescriptor(PVOID extension,
                                       PVIDEO_CHILD_ENUM_INFO information,
                                       PVIDEO_CHILD_TYPE child_type,
                                       PVOID descriptor, PULONG child_id,
                                       PULONG unused)
{
    UNREFERENCED_PARAMETER(extension);
    UNREFERENCED_PARAMETER(descriptor);
    qxp_debug("QXPMP D");
    if (!information || !child_type || !child_id || !unused) {
        return ERROR_INVALID_PARAMETER;
    }
    if (information->Size < sizeof(*information)) {
        return VIDEO_ENUM_NO_MORE_DEVICES;
    }
    qxp_debug_status(information->ChildIndex);
    *unused = 0;
    if (information->ChildIndex == 0) return VIDEO_ENUM_INVALID_DEVICE;
    if (information->ChildIndex == DISPLAY_ADAPTER_HW_ID) {
        *child_type = VideoChip;
        return VIDEO_ENUM_MORE_DEVICES;
    }
    if (information->ChildIndex == 1) {
        *child_id = information->ChildIndex;
        *child_type = Monitor;
        return VIDEO_ENUM_MORE_DEVICES;
    }
    return VIDEO_ENUM_NO_MORE_DEVICES;
}

ULONG DriverEntry(PVOID context1, PVOID context2)
{
    VIDEO_HW_INITIALIZATION_DATA initialization;
    ULONG status;

    qxp_debug("QXPMP E");
    qxp_debug("QXPMP VERSION 227");
    qxp_debug(QXP_D3D_TRANSPORT_ABI_MARKER);

    VideoPortZeroMemory(&initialization, sizeof(initialization));
    initialization.HwInitDataSize = sizeof(initialization);
    initialization.HwFindAdapter = QxpFindAdapter;
    initialization.HwInitialize = QxpInitialize;
    initialization.HwStartIO = QxpStartIO;
    initialization.HwResetHw = QxpResetHw;
    initialization.HwTimer = QxpTimer;
    initialization.HwSetPowerState = QxpSetPowerState;
    initialization.HwGetPowerState = QxpGetPowerState;
    initialization.HwGetVideoChildDescriptor = QxpGetChildDescriptor;
    initialization.HwDeviceExtensionSize = sizeof(QXP_DEVICE_EXTENSION);
    initialization.HwLegacyResourceList = qxp_legacy_resources;
    initialization.HwLegacyResourceCount =
        sizeof(qxp_legacy_resources) / sizeof(qxp_legacy_resources[0]);
    initialization.AdapterInterfaceType = PCIBus;
    status = VideoPortInitialize(context1, context2, &initialization, NULL);
    qxp_debug_status(status);
    qxp_debug(status == NO_ERROR ? "QXPMP E+" : "QXPMP E-");
    return status;
}
