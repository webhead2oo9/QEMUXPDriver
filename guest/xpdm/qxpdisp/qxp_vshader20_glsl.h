#ifndef QXP_VSHADER20_GLSL_H
#define QXP_VSHADER20_GLSL_H

#include <windef.h>

#include "qxp_shader_ir.h"

/*
 * Private, guest-only direct vs_2_0 typed-IR to compatibility GLSL 1.20
 * representation.  This mirrors the architecture of qxp_pshader_glsl.[ch] but
 * is vertex-specific.  It consumes only a QXP_SHADER_IR already produced by
 * QxpDecodeShader20 and has no production caller.  No allocation is performed
 * and no input or output pointer is retained.
 *
 * The caller owns the deliberately large metadata storage (it embeds raw DEF
 * DWORDs for every float/int/bool constant slot).  Driver callers must not
 * place it on a small stack.
 */
#define QXP_VERTEX_SHADER_GLSL_MAX_SOURCE          (512UL * 1024UL)
#define QXP_VERTEX_SHADER_GLSL_TEMP_COUNT          12UL
#define QXP_VERTEX_SHADER_GLSL_INPUT_COUNT         16UL
#define QXP_VERTEX_SHADER_GLSL_FLOAT_CONSTANT_COUNT   256UL
#define QXP_VERTEX_SHADER_GLSL_INTEGER_CONSTANT_COUNT  16UL
#define QXP_VERTEX_SHADER_GLSL_BOOLEAN_CONSTANT_COUNT  16UL
#define QXP_VERTEX_SHADER_GLSL_TEXCOORD_OUTPUT_COUNT    8UL
#define QXP_VERTEX_SHADER_GLSL_ATTRIBUTE_OUTPUT_COUNT   2UL

/*
 * Fixed metadata.  No pointers.  All DEF/DEFI/DEFB immediates remain raw
 * DWORDs with exact last-wins masks and values so a future uniform upload can
 * overlay them; the emitted GLSL always sources constants from uniforms.
 */
typedef struct qxp_vertex_shader_glsl_metadata {
    /* Resource requirements and slot accounting. */
    DWORD required_temp_count;      /* highest r# used + 1, range 0..12 */
    DWORD required_float_constant_count; /* highest c# read + 1, 0..256 */
    DWORD instruction_count;
    DWORD slot_count;
    DWORD arithmetic_slot_count;
    DWORD flow_slot_count;
    DWORD declaration_count;
    DWORD definition_count;

    /* Inputs (declared attributes). */
    DWORD input_mask;               /* declared v# registers */
    DWORD input_usage[QXP_VERTEX_SHADER_GLSL_INPUT_COUNT];
    DWORD input_usage_index[QXP_VERTEX_SHADER_GLSL_INPUT_COUNT];
    BYTE  input_component_mask[QXP_VERTEX_SHADER_GLSL_INPUT_COUNT];

    /* Outputs, exact accumulated write masks. */
    DWORD attribute_output_mask;    /* oD0/oD1 written */
    BYTE  attribute_output_components[QXP_VERTEX_SHADER_GLSL_ATTRIBUTE_OUTPUT_COUNT];
    DWORD texcoord_output_mask;     /* oT0..7 written */
    BYTE  texcoord_output_components[QXP_VERTEX_SHADER_GLSL_TEXCOORD_OUTPUT_COUNT];
    /* Pair-specific bits: 0-1 are qxpPsInV#, 2-9 are qxpPsInT#. */
    DWORD output_centroid_mask;
    BOOL  writes_position;
    BYTE  position_component_mask;
    BOOL  writes_fog;
    BYTE  fog_component_mask;
    BOOL  writes_point_size;
    BYTE  point_size_component_mask;

    /* Constant definitions, raw last-wins DWORDs. */
    DWORD float_definition_mask[QXP_VERTEX_SHADER_GLSL_FLOAT_CONSTANT_COUNT / 32UL];
    DWORD float_definition_values[QXP_VERTEX_SHADER_GLSL_FLOAT_CONSTANT_COUNT][4];
    DWORD integer_definition_mask;  /* 16 bits */
    DWORD integer_definition_values[QXP_VERTEX_SHADER_GLSL_INTEGER_CONSTANT_COUNT][4];
    DWORD boolean_definition_mask;  /* 16 bits */
    DWORD boolean_definition_values[QXP_VERTEX_SHADER_GLSL_BOOLEAN_CONSTANT_COUNT];

    /* Feature summary. */
    BOOL uses_relative_constant;
    BOOL uses_address_register;
    BOOL uses_loop_register;
    BOOL uses_integer_constants;
    BOOL uses_boolean_constants;
    BOOL uses_static_flow;
    BOOL uses_subroutines;
} QXP_VERTEX_SHADER_GLSL_METADATA;

typedef enum qxp_vertex_shader_glsl_error_code {
    QXP_VERTEX_SHADER_GLSL_OK = 0,
    QXP_VERTEX_SHADER_GLSL_ERROR_NULL_ARGUMENT = 1,
    QXP_VERTEX_SHADER_GLSL_ERROR_IR_STATE = 2,
    QXP_VERTEX_SHADER_GLSL_ERROR_PROFILE = 3,
    QXP_VERTEX_SHADER_GLSL_ERROR_INSTRUCTION = 4,
    QXP_VERTEX_SHADER_GLSL_ERROR_REGISTER = 5,
    QXP_VERTEX_SHADER_GLSL_ERROR_DECLARATION = 6,
    QXP_VERTEX_SHADER_GLSL_ERROR_UNSUPPORTED_FORM = 7,
    QXP_VERTEX_SHADER_GLSL_ERROR_OUTPUT_CAPACITY = 8,
    QXP_VERTEX_SHADER_GLSL_ERROR_CONTROL_FLOW = 9,
    QXP_VERTEX_SHADER_GLSL_ERROR_UNINITIALIZED = 10
} QXP_VERTEX_SHADER_GLSL_ERROR_CODE;

typedef struct qxp_vertex_shader_glsl_error {
    DWORD code;
    DWORD instruction_index;
    DWORD token_offset;
    DWORD required_capacity;
} QXP_VERTEX_SHADER_GLSL_ERROR;

/*
 * Emits GLSL 1.20 directly from an exact decoded vs_2_0 IR.  source_capacity
 * includes the trailing NUL and source_bytes excludes it.  Passing NULL/zero
 * for source/capacity is a count-only query.
 *
 * The emitter independently reconstructs and validates every exact vs_2_0 form
 * admitted by QxpDecodeShader20 before any count-driven traversal.  It is
 * fail-closed and deterministic: on every failure the metadata and
 * source_bytes are zeroed and source[0] is set to 0 when possible.  No
 * allocation is performed and no pointer is retained.
 *
 * Emitted outputs pair with the pixel emitter: oD0/1 -> qxpPsInV0/1,
 * oT0..7 -> qxpPsInT0..7, oPos -> gl_Position, oFog -> gl_FogFragCoord,
 * oPts -> gl_PointSize.  output_centroid_mask is pair-specific: bits 0-1
 * qualify qxpPsInV0/1 and bits 2-9 qualify qxpPsInT0..7.  It must be a
 * subset of the outputs written by this shader and must match the paired
 * pixel emitter metadata.  Position commit uses uniform qxpVsClipControl.x
 * as the established D3D-to-host Y sign and converts D3D z to OpenGL clip z.
 */
BOOL QxpEmitVertexShader20Glsl(
    const QXP_SHADER_IR *ir, DWORD output_centroid_mask,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_VERTEX_SHADER_GLSL_ERROR *error);

#endif
