#include <windef.h>

#include "qxp_d3d9_draw_gate.h"

static void qxp_d3d9_draw_gate_zero_status(
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!status) return;
    bytes = (volatile BYTE *)status;
    for (index = 0; index < (ULONG)sizeof(*status); ++index)
        bytes[index] = 0;
}

DWORD QxpD3d9DrawCommandRecordSize(BYTE opcode)
{
    switch (opcode) {
    case QXP_D3D9_DP2OP_POINTS:
        return QXP_D3D9_DRAW_RECORD_BYTES_POINTS;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE;
    case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE;
    case QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN:
        return QXP_D3D9_DRAW_RECORD_BYTES_CLIPPEDTRIANGLEFAN;
    case QXP_D3D9_DP2OP_DRAWPRIMITIVE2:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2;
    case QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2:
        return QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2;
    default:
        return 0;
    }
}

/*
 * Opcodes whose legacy execution honours a stream-0 byte offset.  Both
 * transport cores resolve a SETSTREAMSOURCE2 binding and apply its offset,
 * so the indexed forms belong here alongside the nonindexed ones; an opcode
 * left out is refused whenever stream 0 carries SOURCE2 provenance.
 */
static BOOL qxp_d3d9_draw_gate_stream_offset_opcode(BYTE opcode)
{
    return opcode == QXP_D3D9_DP2OP_POINTS ||
           opcode == QXP_D3D9_DP2OP_DRAWPRIMITIVE ||
           opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
           opcode == QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN ||
           opcode == QXP_D3D9_DP2OP_DRAWPRIMITIVE2 ||
           opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2;
}

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
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    DWORD inspector_failure_code;
    BOOL inspector_resource_failure;

    qxp_d3d9_draw_gate_zero_status(status);
    if (!status || !owner || !qxp_shader_owner_is_set(owner) ||
        !draw_state || !pixel_binding || !table || !allocator ||
        !allocator->allocate || !allocator->release || !lock_callback ||
        !unlock_callback) {
        if (status)
            status->code = QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT;
        return QXP_D3D9_DRAW_GATE_UNSUPPORTED;
    }

    if (!QxpD3d9DrawStateRequiresUnsupportedExecution(draw_state) &&
        !QxpD3d9PixelBindingRequiresSm2Execution(pixel_binding) &&
        !pixel_binding->sm2.generation &&
        (!draw_state->streams[0].source2 ||
         qxp_d3d9_draw_gate_stream_offset_opcode(opcode))) {
        return QXP_D3D9_DRAW_GATE_LEGACY_ALLOWED;
    }

    prepared = QxpD3d9PrepareDrawSnapshot(
        owner, draw_state, pixel_binding, fixed_function_pixel, table,
        allocator,
        lock_callback, unlock_callback, lock_opaque, cache, status);
    if (!prepared) {
        if (QxpD3d9DrawPrepareStatusIsResourceFailure(status))
            return QXP_D3D9_DRAW_GATE_RESOURCE_FAILURE;
        return QXP_D3D9_DRAW_GATE_UNSUPPORTED;
    }

    /* The prepared draw is complete but SM2 execution remains unsupported.
     * An optional inspector observes it outside the table lock before the
     * prepared draw is destroyed; declining only refines the status. */
    inspector_failure_code = 0;
    inspector_resource_failure = FALSE;
    if (inspector && !inspector(
            prepared, opcode, &inspector_failure_code,
            inspector_opaque)) {
        if (inspector_failure_code !=
                QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING &&
            inspector_failure_code !=
                QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ENVELOPE &&
            inspector_failure_code !=
                QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE &&
            inspector_failure_code !=
                QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE &&
            inspector_failure_code !=
                QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ACCESS) {
            inspector_failure_code =
                QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING;
        }
        status->code = inspector_failure_code;
        inspector_resource_failure = inspector_failure_code ==
            QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE;
    }
    QxpD3d9DestroyPreparedDraw(prepared, allocator);
    if (inspector_resource_failure)
        return QXP_D3D9_DRAW_GATE_RESOURCE_FAILURE;
    return QXP_D3D9_DRAW_GATE_UNSUPPORTED;
}

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
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    QXP_D3D9_DRAW_GATE_RESULT gate_result;
    DWORD record_size;

    qxp_d3d9_draw_gate_zero_status(status);
    if (error_offset) *error_offset = 0;
    record_size = QxpD3d9DrawCommandRecordSize(opcode);
    if (!record_size) return QXP_D3D9_DRAW_COMMAND_NOT_DRAW;
    if (!error_offset || !status) {
        if (error_offset) *error_offset = outer_error_offset;
        if (status)
            status->code = QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT;
        return QXP_D3D9_DRAW_COMMAND_UNSUPPORTED;
    }
    if (record_count > available_bytes / record_size) {
        *error_offset = outer_error_offset;
        return QXP_D3D9_DRAW_COMMAND_TRUNCATED;
    }
    if (!record_count) return QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED;

    gate_result = QxpD3d9GateDrawExecution(
        owner, draw_state, pixel_binding, fixed_function_pixel, table,
        allocator,
        lock_callback, unlock_callback, lock_opaque, opcode, inspector,
        inspector_opaque, cache, status);
    if (gate_result == QXP_D3D9_DRAW_GATE_LEGACY_ALLOWED)
        return QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED;
    *error_offset = outer_error_offset;
    if (gate_result == QXP_D3D9_DRAW_GATE_RESOURCE_FAILURE)
        return QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE;
    return QXP_D3D9_DRAW_COMMAND_UNSUPPORTED;
}
