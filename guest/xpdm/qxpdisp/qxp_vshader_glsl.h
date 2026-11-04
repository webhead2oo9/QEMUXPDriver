#ifndef QXP_VSHADER_GLSL_H
#define QXP_VSHADER_GLSL_H

#include <windef.h>

#define QXP_VERTEX_SHADER_GLSL_MAX (128UL * 1024UL)

BOOL QxpConvertVertexShaderArbToGlsl(
    const char *arb_source, DWORD arb_source_bytes,
    char *glsl_source, DWORD glsl_capacity, DWORD *glsl_source_bytes);

#endif
