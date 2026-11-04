#ifndef QXP_D3D9_SHADER_LINK_H
#define QXP_D3D9_SHADER_LINK_H

#include <windef.h>

#include "qxp_shader_ir.h"
#include "qxp_vdecl9.h"
#include "qxp_vshader20_glsl.h"

/*
 * Private, guest-only Direct3D 9 vertex-input semantic link.  This is not a
 * transport structure.  The result contains only fixed scalar data and never
 * retains pointers into the declaration or shader IR.
 */
#define QXP_D3D9_VERTEX_LINK_INPUT_COUNT 16UL
#define QXP_D3D9_VERTEX_LINK_INDEX_NONE  0xffffffffUL

typedef enum qxp_d3d9_vertex_link_mode {
    QXP_D3D9_VERTEX_LINK_MODE_INVALID = 0,
    QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE = 1,
    QXP_D3D9_VERTEX_LINK_MODE_POSITIONT_BYPASS = 2
} QXP_D3D9_VERTEX_LINK_MODE;

typedef struct qxp_d3d9_vertex_input_link {
    BOOL declared;
    BOOL matched;
    DWORD usage;
    DWORD usage_index;
    DWORD destination_register;
    DWORD declaration_element;
    DWORD source_stream;
    DWORD source_offset;
    DWORD source_type;
    DWORD source_method;
    DWORD source_byte_size;
} QXP_D3D9_VERTEX_INPUT_LINK;

typedef struct qxp_d3d9_vertex_shader_link {
    BOOL valid;
    DWORD mode;
    DWORD input_count;
    DWORD matched_input_count;
    DWORD undefined_input_count;
    DWORD input_mask;
    DWORD matched_input_mask;
    DWORD undefined_input_mask;
    DWORD matched_stream_mask;
    ULONGLONG matched_element_mask;
    DWORD required_stream_spans[QXP_VERTEX_DECLARATION9_STREAM_COUNT];
    /* Indexed by the shader destination input register v0 through v15. */
    QXP_D3D9_VERTEX_INPUT_LINK
        inputs[QXP_D3D9_VERTEX_LINK_INPUT_COUNT];
} QXP_D3D9_VERTEX_SHADER_LINK;

typedef enum qxp_d3d9_vertex_link_error_code {
    QXP_D3D9_VERTEX_LINK_OK = 0,
    QXP_D3D9_VERTEX_LINK_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_STATE = 2,
    QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_ELEMENT = 3,
    QXP_D3D9_VERTEX_LINK_ERROR_DECLARATION_SEMANTIC = 4,
    QXP_D3D9_VERTEX_LINK_ERROR_SHADER_STATE = 5,
    QXP_D3D9_VERTEX_LINK_ERROR_SHADER_PROFILE = 6,
    QXP_D3D9_VERTEX_LINK_ERROR_SHADER_DECLARATION = 7
} QXP_D3D9_VERTEX_LINK_ERROR_CODE;

typedef struct qxp_d3d9_vertex_link_status {
    DWORD code;
    DWORD declaration_element;
    DWORD instruction_index;
    DWORD token_offset;
} QXP_D3D9_VERTEX_LINK_STATUS;

/*
 * Links exact vs_1_1/vs_2_0 dcl usage/index inputs to an already parsed D3D9
 * vertex declaration.  Both vertex profiles carry the identical dcl_usage
 * declarations, so the same link logic applies to either; pixel profiles are
 * rejected.  Extra declaration elements are ignored.  Missing semantics are
 * legal for both vertex profiles and are recorded in undefined_input_mask;
 * later draw preparation must provide a safe undefined value rather than
 * reading absent memory.  POSITIONT0 succeeds in bypass mode with no
 * programmable mappings.
 *
 * On every failure, a non-NULL link result is entirely zeroed.  Success sets
 * link->valid and leaves status entirely zero.  No allocation is performed.
 */
BOOL QxpD3d9LinkVertexShader20(
    const QXP_VERTEX_DECLARATION9 *declaration,
    const QXP_SHADER_IR *shader,
    QXP_D3D9_VERTEX_SHADER_LINK *link,
    QXP_D3D9_VERTEX_LINK_STATUS *status);

/*
 * Reconstructs a link purely from a lowered stage's metadata and the
 * declaration, without the bytecode.  Two callers need exactly this: the draw
 * path re-derives the link to prove a prepared draw still describes its own
 * declaration, and a synthesized pre-transformed stage has no bytecode to
 * link from in the first place.  Sharing one construction is what makes those
 * two agree by definition rather than by inspection.
 *
 * mode is stored verbatim so a bypass link stays distinguishable from a
 * programmable one.  Returns FALSE, leaving the link zeroed, when the
 * declaration cannot be expressed.
 */
BOOL QxpD3d9RebuildVertexShaderLink(
    const QXP_VERTEX_DECLARATION9 *declaration,
    const QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    DWORD mode, QXP_D3D9_VERTEX_SHADER_LINK *link);

#endif
