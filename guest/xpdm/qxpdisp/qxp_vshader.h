#ifndef QXP_VSHADER_H
#define QXP_VSHADER_H

#include <windef.h>

#define QXP_VERTEX_SHADER_CONSTANT_COUNT 96UL
#define QXP_VERTEX_SHADER_MAX_INSTRUCTIONS 128UL
#define QXP_VERTEX_SHADER_SOURCE_MAX (64UL * 1024UL)
#define QXP_VERTEX_SHADER_ARB_TEMPORARIES 24UL

#define QXP_VS_OUTPUT_POSITION 0x00000001UL
#define QXP_VS_OUTPUT_FOG 0x00000002UL
#define QXP_VS_OUTPUT_POINT_SIZE 0x00000004UL
#define QXP_VS_OUTPUT_COLOR0 0x00000008UL
#define QXP_VS_OUTPUT_COLOR1 0x00000010UL
#define QXP_VS_OUTPUT_TEXCOORD0 0x00000100UL

BOOL QxpTranslateVertexShader11(
    const DWORD *bytecode, DWORD bytecode_bytes,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    DWORD *used_inputs, DWORD *written_outputs,
    BOOL *uses_relative_addressing);

#endif
