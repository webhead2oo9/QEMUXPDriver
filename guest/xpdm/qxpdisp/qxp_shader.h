#ifndef QXP_SHADER_H
#define QXP_SHADER_H

#include <windef.h>

#define QXP_PIXEL_SHADER_CONSTANT_COUNT 8UL
#define QXP_PIXEL_SHADER_SOURCE_MAX 8192UL

BOOL QxpTranslatePixelShader1x(
    const DWORD *bytecode, DWORD bytecode_bytes, DWORD cube_overrides,
    char *source, DWORD source_capacity, DWORD *source_bytes,
    DWORD *sampled_texture_stages, DWORD *coordinate_texture_stages,
    DWORD *cube_texture_stages, DWORD *dynamic_texture_stages,
    DWORD *defined_constants,
    DWORD constant_values[QXP_PIXEL_SHADER_CONSTANT_COUNT][4]);

#endif
