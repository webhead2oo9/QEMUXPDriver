#ifndef QXP_PSHADER_GLSL_H
#define QXP_PSHADER_GLSL_H

#include <windef.h>

#include "qxp_shader_ir.h"

/* Private, guest-only direct ps_2_0/ps_1_1 compatibility-GLSL
 * representation. */
#define QXP_PIXEL_SHADER_GLSL_MAX_SOURCE (512UL * 1024UL)
#define QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT 32UL
#define QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT 16UL

typedef struct qxp_pixel_shader_glsl_metadata {
    DWORD required_ps_temp_count;
    DWORD required_ps20_caps;
    DWORD required_ps_instruction_slots;
    DWORD required_constant_count; /* highest c# read + 1, range 0..32 */
    DWORD input_color_mask;
    DWORD input_texture_mask;
    /* Effective interpolation: bits 0-1 are always set for declared
     * ps_2_0 v# Input Color Registers and for ps_1_1 v# reads; bits 2-9
     * follow ps_2_0 t# _centroid. */
    DWORD input_centroid_mask;
    DWORD sampler_mask;
    DWORD color_output_mask;
    BOOL writes_depth;
    BOOL uses_discard;
    BYTE input_color_components[2];
    BYTE input_texture_components[8];
    BYTE sampler_types[QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT];
    DWORD definition_mask;
    DWORD definition_values[QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT][4];
} QXP_PIXEL_SHADER_GLSL_METADATA;

typedef enum qxp_pixel_shader_glsl_error_code {
    QXP_PIXEL_SHADER_GLSL_OK = 0,
    QXP_PIXEL_SHADER_GLSL_ERROR_NULL_ARGUMENT = 1,
    QXP_PIXEL_SHADER_GLSL_ERROR_IR_STATE = 2,
    QXP_PIXEL_SHADER_GLSL_ERROR_PROFILE = 3,
    QXP_PIXEL_SHADER_GLSL_ERROR_INSTRUCTION = 4,
    QXP_PIXEL_SHADER_GLSL_ERROR_REGISTER = 5,
    QXP_PIXEL_SHADER_GLSL_ERROR_DECLARATION = 6,
    QXP_PIXEL_SHADER_GLSL_ERROR_UNSUPPORTED_FORM = 7,
    QXP_PIXEL_SHADER_GLSL_ERROR_OUTPUT_CAPACITY = 8
} QXP_PIXEL_SHADER_GLSL_ERROR_CODE;

typedef struct qxp_pixel_shader_glsl_error {
    DWORD code;
    DWORD instruction_index;
    DWORD token_offset;
    DWORD required_capacity;
} QXP_PIXEL_SHADER_GLSL_ERROR;

/*
 * Emits GLSL 1.20 directly from an exact decoded ps_2_0 or ps_1_1 IR.
 * ps_1_1 declares nothing: its sampler, texcoord, and colour-input
 * metadata is derived from register use, mutable t# registers become
 * locals initialized from the stage texcoords, and the final r0 value is
 * emitted as the colour.  source_capacity
 * includes the trailing NUL and source_bytes excludes it.  Passing NULL/zero
 * for source/capacity is a count-only query.  DEF immediates remain raw DWORDs
 * in metadata so future uniform upload can overlay exact last-wins values.
 *
 * No allocation is performed and no input/output pointer is retained.  Every
 * failure zeros metadata and source_bytes and writes source[0]=0 when possible.
 */
BOOL QxpEmitPixelShader20Glsl(
    const QXP_SHADER_IR *ir,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_PIXEL_SHADER_GLSL_ERROR *error);

#endif
