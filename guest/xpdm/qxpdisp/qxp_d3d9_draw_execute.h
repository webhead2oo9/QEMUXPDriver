#ifndef QXP_D3D9_DRAW_EXECUTE_H
#define QXP_D3D9_DRAW_EXECUTE_H

#include <windef.h>

#include "../qxp_d3d_transport.h"
#include "qxp_d3d9_draw_prepare.h"
#include "qxp_vdecl.h"

#define QXP_D3D9_PROGRAM_VERTEX_INPUT_COUNT 16UL

typedef struct qxp_d3d9_program_vertex_layout {
    BOOL valid;
    DWORD input_mask;
    DWORD used_inputs;
    DWORD written_outputs;
    DWORD vertex_stride;
    QXP_D3D_PROGRAM_VERTEX_ELEMENT
        elements[QXP_D3D9_PROGRAM_VERTEX_INPUT_COUNT];
} QXP_D3D9_PROGRAM_VERTEX_LAYOUT;

/* Builds pointer-free transport metadata for one validated programmable draw.
 * Only streams used by matched shader inputs are inspected. */
BOOL QxpD3d9BuildProgramVertexLayout(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW
        views[QXP_D3D9_PROGRAM_VERTEX_INPUT_COUNT],
    QXP_D3D9_PROGRAM_VERTEX_LAYOUT *layout);

/* Gathers one logical vertex.  The supplied layout is revalidated against the
 * immutable prepared snapshot before any destination byte is changed. */
BOOL QxpD3d9GatherProgramVertex(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_VERTEX_STREAM_VIEW
        views[QXP_D3D9_PROGRAM_VERTEX_INPUT_COUNT],
    const QXP_D3D9_PROGRAM_VERTEX_LAYOUT *layout,
    DWORD vertex_index, BYTE *destination, DWORD destination_bytes);

#endif
