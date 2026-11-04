#ifndef QXP_D3D9_DRAW_GATE_H
#define QXP_D3D9_DRAW_GATE_H

#include "qxp_d3d9_draw_prepare.h"

#define QXP_D3D9_DP2OP_POINTS 1U
#define QXP_D3D9_DP2OP_DRAWPRIMITIVE 52U
#define QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE 53U
#define QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN 58U
#define QXP_D3D9_DP2OP_DRAWPRIMITIVE2 59U
#define QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2 60U

#define QXP_D3D9_DRAW_RECORD_BYTES_POINTS 4UL
#define QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE 12UL
#define QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE 24UL
#define QXP_D3D9_DRAW_RECORD_BYTES_CLIPPEDTRIANGLEFAN 12UL
#define QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2 12UL
#define QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2 24UL

typedef enum qxp_d3d9_draw_gate_result {
    QXP_D3D9_DRAW_GATE_LEGACY_ALLOWED = 0,
    QXP_D3D9_DRAW_GATE_UNSUPPORTED = 1,
    QXP_D3D9_DRAW_GATE_RESOURCE_FAILURE = 2
} QXP_D3D9_DRAW_GATE_RESULT;

typedef enum qxp_d3d9_draw_command_gate_result {
    QXP_D3D9_DRAW_COMMAND_NOT_DRAW = 0,
    QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED = 1,
    QXP_D3D9_DRAW_COMMAND_TRUNCATED = 2,
    QXP_D3D9_DRAW_COMMAND_UNSUPPORTED = 3,
    QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE = 4
} QXP_D3D9_DRAW_COMMAND_GATE_RESULT;

DWORD QxpD3d9DrawCommandRecordSize(BYTE opcode);

/* Optional production seam.  When supplied, the inspector is invoked exactly
 * once with the immutable prepared draw and the classified opcode after a
 * successful preparation and outside the table lock, before the prepared draw
 * is destroyed.  The prepared pointer must not escape the callback.  The gate
 * initializes failure_code to zero.  A declining callback may classify
 * resource backing (18), B6G2 envelope (19), B6G3B1 capture semantic (20),
 * capture resource (21), or live B6G3A access (22) failure.  Every other
 * value is normalized to resource backing.  Capture resource is returned as
 * a gate resource failure after prepared state is destroyed; accepting leaves
 * the preparation status zero. */
typedef BOOL (*QXP_D3D9_DRAW_PREPARED_INSPECTOR)(
    const QXP_D3D9_DRAW_PREPARED *prepared, BYTE opcode,
    DWORD *failure_code, void *opaque);

QXP_D3D9_DRAW_GATE_RESULT QxpD3d9GateDrawExecution(
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    BYTE opcode,
    QXP_D3D9_DRAW_PREPARED_INSPECTOR inspector,
    void *inspector_opaque,
    QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    QXP_D3D9_DRAW_PREPARE_STATUS *status);

/* Exact production pre-switch seam: classify the six fixed draw opcodes,
 * frame the complete outer record array, skip zero-count no-ops, and gate
 * before the caller can invoke any transport or frame-mark callback. */
QXP_D3D9_DRAW_COMMAND_GATE_RESULT QxpD3d9GateDrawCommand(
    BYTE opcode,
    DWORD record_count,
    ULONG available_bytes,
    DWORD outer_error_offset,
    DWORD *error_offset,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARED_INSPECTOR inspector,
    void *inspector_opaque,
    QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    QXP_D3D9_DRAW_PREPARE_STATUS *status);

#endif
