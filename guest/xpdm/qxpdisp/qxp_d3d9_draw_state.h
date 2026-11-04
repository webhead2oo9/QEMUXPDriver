#ifndef QXP_D3D9_DRAW_STATE_H
#define QXP_D3D9_DRAW_STATE_H

#include <windef.h>

#include "qxp_d3d9_shader_objects.h"

/* Private spellings keep directdraw.c at DIRECT3D_VERSION 0x0800 while the
 * dormant DDI-v4 state path frames the exact XP D3D9 DP2 values. */
#define QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF 48U
#define QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE 49U
#define QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM 50U
#define QXP_D3D9_DRAW_DP2OP_SET_INDICES 51U
#define QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF 57U
#define QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI 77U
#define QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2 80U
#define QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB 83U
#define QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI 93U
#define QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB 94U
#define QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ 95U

/* Position slots of a translated declaration's FVF.  qxp_vdecl9 emits
 * exactly one of XYZ or XYZRHW, and XYZRHW is what marks a pre-transformed
 * declaration whose bound vertex shader Direct3D never runs. */
#define QXP_D3D9_DRAW_FVF_POSITION_MASK 0x0000000eUL
#define QXP_D3D9_DRAW_FVF_XYZRHW 0x00000004UL

#define QXP_D3D9_DRAW_STREAM_COUNT 16UL
#define QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT 256UL
#define QXP_D3D9_DRAW_VS_INTEGER_CONSTANT_COUNT 16UL
#define QXP_D3D9_DRAW_VS_BOOLEAN_CONSTANT_COUNT 16UL
#define QXP_D3D9_DRAW_PS_FLOAT_CONSTANT_COUNT 32UL
#define QXP_D3D9_DRAW_PS_INTEGER_CONSTANT_COUNT 16UL
#define QXP_D3D9_DRAW_PS_BOOLEAN_CONSTANT_COUNT 16UL

#define QXP_D3D9_STREAM_SOURCE_INDEXED_DATA 0x40000000UL
#define QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA 0x80000000UL
#define QXP_D3D9_STREAM_SOURCE_FLAG_MASK 0xc0000000UL
#define QXP_D3D9_STREAM_SOURCE_DIVISOR_MASK 0x3fffffffUL

typedef enum qxp_d3d9_draw_declaration_mode {
    QXP_D3D9_DRAW_DECLARATION_NONE = 0,
    QXP_D3D9_DRAW_DECLARATION_FVF = 1,
    QXP_D3D9_DRAW_DECLARATION_OBJECT = 2
} QXP_D3D9_DRAW_DECLARATION_MODE;

typedef enum qxp_d3d9_draw_shader_stage {
    QXP_D3D9_DRAW_SHADER_STAGE_VERTEX = 1,
    QXP_D3D9_DRAW_SHADER_STAGE_PIXEL = 2
} QXP_D3D9_DRAW_SHADER_STAGE;

typedef enum qxp_d3d9_draw_constant_kind {
    QXP_D3D9_DRAW_CONSTANT_FLOAT = 1,
    QXP_D3D9_DRAW_CONSTANT_INTEGER = 2,
    QXP_D3D9_DRAW_CONSTANT_BOOLEAN = 3
} QXP_D3D9_DRAW_CONSTANT_KIND;

typedef enum qxp_d3d9_draw_state_error_code {
    QXP_D3D9_DRAW_STATE_OK = 0,
    QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_STATE_ERROR_OPCODE = 2,
    QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD = 3,
    QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD = 4,
    QXP_D3D9_DRAW_STATE_ERROR_STAGE = 5,
    QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_KIND = 6,
    QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_COUNT = 7,
    QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_RANGE = 8,
    QXP_D3D9_DRAW_STATE_ERROR_STREAM = 9,
    QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE = 10,
    QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY = 11,
    QXP_D3D9_DRAW_STATE_ERROR_DECLARATION = 12,
    QXP_D3D9_DRAW_STATE_ERROR_FUNCTION = 13,
    QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK = 14,
    QXP_D3D9_DRAW_STATE_ERROR_RESOURCE = 15,
    QXP_D3D9_DRAW_STATE_ERROR_STATE_CHANGED = 16,
    QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED = 17,
    QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE = 18
} QXP_D3D9_DRAW_STATE_ERROR_CODE;

typedef struct qxp_d3d9_draw_state_status {
    DWORD code;
    DWORD record_index;
    DWORD byte_offset;
    DWORD stage;
    DWORD constant_kind;
    DWORD register_index;
    DWORD stream;
} QXP_D3D9_DRAW_STATE_STATUS;

typedef struct qxp_d3d9_draw_stream_state {
    DWORD handle;
    DWORD offset;
    DWORD stride;
    /* Raw D3D9 divider plus INDEXEDDATA/INSTANCEDATA flag bits. */
    DWORD divider;
    BOOL user_memory;
    /* SOURCE2 provenance is required even when offset is zero.  The bounded
     * nonindexed stream-0 fixed-function capture consumes it; every other
     * stream or execution shape remains fail-closed. */
    BOOL source2;
} QXP_D3D9_DRAW_STREAM_STATE;

typedef struct qxp_d3d9_draw_index_state {
    DWORD handle;
    DWORD stride;
} QXP_D3D9_DRAW_INDEX_STATE;

typedef struct qxp_d3d9_legacy_resource_view {
    DWORD *stream_handles;
    DWORD *stream_strides;
    DWORD stream_count;
    BOOL *stream0_user_memory;
    DWORD *index_handle;
    DWORD *index_stride;
} QXP_D3D9_LEGACY_RESOURCE_VIEW;

typedef struct qxp_d3d9_draw_state {
    DWORD declaration_mode;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    DWORD legacy_fvf;
    /* Exact declaration-object translation used only by the legacy draw gate.
     * The source binding prevents a stale cache from surviving a rebind. */
    QXP_D3D9_SHADER_BINDING translated_declaration_binding;
    DWORD translated_declaration_fvf;
    DWORD translated_vertex_stride;
    QXP_D3D9_SHADER_BINDING vertex_function_binding;
    QXP_D3D9_DRAW_STREAM_STATE streams[QXP_D3D9_DRAW_STREAM_COUNT];
    QXP_D3D9_DRAW_INDEX_STATE indices;
    /* Saturating, nonwrapping resource-binding sequence. SOURCE2 and shared
     * stream/index publication reject teardown or competing binding changes
     * against their snapshots. */
    ULONGLONG stream_source_generation;

    DWORD vs_float[QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT][4];
    DWORD vs_integer[QXP_D3D9_DRAW_VS_INTEGER_CONSTANT_COUNT][4];
    DWORD vs_boolean[QXP_D3D9_DRAW_VS_BOOLEAN_CONSTANT_COUNT];
    DWORD ps_float[QXP_D3D9_DRAW_PS_FLOAT_CONSTANT_COUNT][4];
    DWORD ps_integer[QXP_D3D9_DRAW_PS_INTEGER_CONSTANT_COUNT][4];
    DWORD ps_boolean[QXP_D3D9_DRAW_PS_BOOLEAN_CONSTANT_COUNT];

    DWORD vs_float_set_mask[QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT / 32UL];
    DWORD vs_integer_set_mask;
    DWORD vs_boolean_set_mask;
    DWORD ps_float_set_mask;
    DWORD ps_integer_set_mask;
    DWORD ps_boolean_set_mask;
} QXP_D3D9_DRAW_STATE;

/* Exact private copies of the XP D3D9 DP2 records. */
typedef struct qxp_d3d9_draw_dp2_constant {
    DWORD register_index;
    DWORD count;
} QXP_D3D9_DRAW_DP2_CONSTANT;

typedef struct qxp_d3d9_draw_dp2_stream_source {
    DWORD stream;
    DWORD handle;
    DWORD stride;
} QXP_D3D9_DRAW_DP2_STREAM_SOURCE;

typedef struct qxp_d3d9_draw_dp2_stream_source_um {
    DWORD stream;
    DWORD stride;
} QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM;

typedef struct qxp_d3d9_draw_dp2_indices {
    DWORD handle;
    DWORD stride;
} QXP_D3D9_DRAW_DP2_INDICES;

typedef struct qxp_d3d9_draw_dp2_stream_source2 {
    DWORD stream;
    DWORD handle;
    DWORD offset;
    DWORD stride;
} QXP_D3D9_DRAW_DP2_STREAM_SOURCE2;

typedef struct qxp_d3d9_draw_dp2_stream_frequency {
    DWORD stream;
    DWORD divider;
} QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY;

typedef char QXP_D3D9_draw_dp2_constant_size_must_be_8[
    sizeof(QXP_D3D9_DRAW_DP2_CONSTANT) == 8 ? 1 : -1];
typedef char QXP_D3D9_draw_dp2_stream_source_size_must_be_12[
    sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE) == 12 ? 1 : -1];
typedef char QXP_D3D9_draw_dp2_stream_source_um_size_must_be_8[
    sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM) == 8 ? 1 : -1];
typedef char QXP_D3D9_draw_dp2_indices_size_must_be_8[
    sizeof(QXP_D3D9_DRAW_DP2_INDICES) == 8 ? 1 : -1];
typedef char QXP_D3D9_draw_dp2_stream_source2_size_must_be_16[
    sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE2) == 16 ? 1 : -1];
typedef char QXP_D3D9_draw_dp2_stream_frequency_size_must_be_8[
    sizeof(QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY) == 8 ? 1 : -1];

/* This module is synchronization-free and owns no allocations.  A future
 * production caller serializes state mutation with its context/table lock.
 * No input pointer is retained. */
void QxpD3d9DrawStateInitialize(QXP_D3D9_DRAW_STATE *state);
/* Object lifecycle writes the binding/FVF members under its table lock.  The
 * serialized production DP2 bridge calls this after each batch, including a
 * prefix failure, to derive and validate the declaration mode. */
BOOL QxpD3d9DrawStateSynchronizeObjectBindings(
    QXP_D3D9_DRAW_STATE *state, QXP_D3D9_DRAW_STATE_STATUS *status);
/* Side-effect-free canonical-state validation.  Valid active D3D9 state is
 * distinct from malformed state and is never rewritten by this helper. */
BOOL QxpD3d9DrawStateValidate(
    const QXP_D3D9_DRAW_STATE *state,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* TRUE includes malformed object state: unsupported or corrupt D3D9 binding
 * must fail closed before legacy draw submission. */
BOOL QxpD3d9DrawStateRequiresUnsupportedExecution(
    const QXP_D3D9_DRAW_STATE *state);
BOOL QxpD3d9DrawStateStatusIsResourceFailure(
    const QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetDeclaration(
    QXP_D3D9_DRAW_STATE *state, DWORD mode,
    const QXP_D3D9_SHADER_BINDING *binding, DWORD legacy_fvf,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Publishes an exact, immutable declaration-object translation.  Pass an
 * all-zero binding/FVF/stride to clear an untranslatable or unbound cache. */
BOOL QxpD3d9DrawStateSetTranslatedDeclaration(
    QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_SHADER_BINDING *binding,
    DWORD translated_fvf, DWORD translated_vertex_stride,
    QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetVertexFunction(
    QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetStreamSource(
    QXP_D3D9_DRAW_STATE *state, DWORD stream, DWORD handle,
    DWORD offset, DWORD stride, BOOL user_memory,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Selects the fixed-function FVF for one draw.  A declaration-object
 * translation remains authoritative.  Otherwise dwVertexType is call-scoped
 * source state and overrides a persistent DDI-v3 SetFVF binding without
 * mutating it. */
DWORD QxpD3d9ResolveLegacyFvf(
    const QXP_D3D9_DRAW_STATE *state, DWORD persistent_fvf,
    DWORD call_fvf);

/* D3DNTHAL_DRAWPRIMITIVES2DATA supplies the call's FVF in dwVertexType.
 * SETSTREAMSOURCEUM state remains the sole user-memory source authority; the
 * outer USERMEMVERTICES flag only identifies a valid union member and cannot
 * replace a vertex-buffer binding.  Build one immutable effective state.
 * vertex_stride is required and must match only for a bound UM stream. */
BOOL QxpD3d9BuildCallDrawState(
    const QXP_D3D9_DRAW_STATE *source, DWORD vertex_fvf,
    DWORD vertex_stride, QXP_D3D9_DRAW_STATE *destination,
    QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetStreamFrequency(
    QXP_D3D9_DRAW_STATE *state, DWORD stream, DWORD divider,
    QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetIndices(
    QXP_D3D9_DRAW_STATE *state, DWORD handle, DWORD stride,
    QXP_D3D9_DRAW_STATE_STATUS *status);
BOOL QxpD3d9DrawStateSetConstants(
    QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD constant_kind,
    DWORD register_index, DWORD count, const void *values,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Shared D3D8/D3D9 float publication. Validation happens before the callback
 * lock; B4 and the representable legacy prefix copy in one locked transaction.
 * High D3D9 registers remain B4-only. */
typedef BOOL (*QXP_D3D9_SHARED_FLOAT_RECORD_CALLBACK)(
    DWORD register_index, const DWORD *values, void *opaque);

BOOL QxpD3d9SharedFloatRangeFitsLegacy(
    DWORD stage, DWORD register_index, DWORD count, DWORD legacy_count);
BOOL QxpD3d9PublishSharedFloatConstants(
    QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD register_index,
    DWORD count, const void *values, DWORD (*legacy_values)[4],
    DWORD legacy_count, QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Exact shared float DP2 dispatcher for opcodes 48/57. Complete earlier
 * records survive a later failure. A callback records each representable VS
 * register after the live B4/legacy publication, preserving D3D8 OOM behavior. */
BOOL QxpD3d9DispatchSharedFloatBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    DWORD (*legacy_values)[4], DWORD legacy_count,
    BOOL state_block_recording,
    QXP_D3D9_SHARED_FLOAT_RECORD_CALLBACK record_callback,
    void *record_opaque,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Exact shared fixed-resource dispatcher for opcodes 49/50/51. It completely
 * frames and validates into bounded candidates outside the table lock, then
 * atomically publishes B4 and legacy state if the common generation matches.
 * Recording a legacy state block intentionally preserves existing live-only
 * stream/index behavior; these states are not added to replay in this slice. */
BOOL QxpD3d9DispatchSharedResourceBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    const QXP_D3D9_LEGACY_RESOURCE_VIEW *legacy,
    BOOL state_block_recording,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status);
/* Same-owner destroy notification: clears matching streams/index and always
 * advances the resource generation so a first-time bind cannot cross teardown. */
void QxpD3d9DrawStateUnbindSurface(
    QXP_D3D9_DRAW_STATE *state, DWORD surface_handle);

/* Frames one DP2 command payload.  Fixed-size batches are completely framed
 * before mutation.  Variable constant batches preserve earlier successful
 * records if a later record fails.  On failure error_offset is always the
 * supplied outer command offset; consumed_bytes ends after successful records. */
BOOL QxpD3d9ProcessDrawStateBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status);

/* Exact production bridge for D3D9-only draw state.  It snapshots under the
 * caller's table lock, parses/allocates outside the lock, and publishes only
 * the affected stream or constant bank under the lock.  Valid constant-record
 * prefixes are published on later failure; fixed batches remain atomic. */
BOOL QxpD3d9DispatchDrawStateBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, QXP_D3D9_DRAW_STATE *state,
    BOOL state_block_recording,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_DRAW_STATE_STATUS *status);

#endif
