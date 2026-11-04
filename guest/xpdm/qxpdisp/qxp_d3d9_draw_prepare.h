#ifndef QXP_D3D9_DRAW_PREPARE_H
#define QXP_D3D9_DRAW_PREPARE_H

#include <windef.h>

#include "qxp_d3d9_draw_state.h"
#include "qxp_ffpshader_glsl.h"
#include "qxp_d3d9_shader_link.h"
#include "qxp_vshader20_glsl.h"
#include "qxp_positiont_vshader_glsl.h"

#define QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT 10UL
#define QXP_D3D9_DRAW_UNDEFINED_INPUT_COUNT 16UL
#define QXP_D3D9_DRAW_UNDEFINED_INPUT_COMPONENT_COUNT 4UL

typedef enum qxp_d3d9_draw_prepared_mode {
    QXP_D3D9_DRAW_PREPARED_MODE_INVALID = 0,
    QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE = 1,
    QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS = 2
} QXP_D3D9_DRAW_PREPARED_MODE;

typedef enum qxp_d3d9_draw_interface_missing_policy {
    QXP_D3D9_DRAW_INTERFACE_MISSING_NONE = 0,
    QXP_D3D9_DRAW_INTERFACE_MISSING_ZERO = 1
} QXP_D3D9_DRAW_INTERFACE_MISSING_POLICY;

typedef enum qxp_d3d9_draw_prepare_stage {
    QXP_D3D9_DRAW_PREPARE_STAGE_NONE = 0,
    QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION = 1,
    QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX = 2,
    QXP_D3D9_DRAW_PREPARE_STAGE_PIXEL = 3
} QXP_D3D9_DRAW_PREPARE_STAGE;

typedef enum qxp_d3d9_draw_prepare_error_code {
    QXP_D3D9_DRAW_PREPARE_OK = 0,
    QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION = 2,
    QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE = 3,
    QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING = 4,
    QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BINDING = 5,
    QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING = 6,
    QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT = 7,
    QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_BYTECODE = 8,
    QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_DECODE = 9,
    QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_SUMMARY = 10,
    QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK = 11,
    QXP_D3D9_DRAW_PREPARE_ERROR_VERTEX_LOWERING = 12,
    QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_SOURCE = 13,
    QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_STRIDE = 14,
    QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_OVERFLOW = 15,
    QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE = 16,
    QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED = 17,
    /* Appended for B6G1: a prepared draw whose optional resource-backing
     * inspector declined it.  Old values are preserved. */
    QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING = 18,
    /* Appended for B6G2 declared/conservative record-envelope rejection. */
    QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ENVELOPE = 19,
    /* Appended for B6G3B1 live immutable-capture semantic rejection. */
    QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE = 20,
    /* Appended for B6G3B1 transient capture/execution resource failures.
     * Capture allocation and post-validation native transport refusal are
     * resource failures; secure/copy rejection remains semantic. */
    QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE = 21,
    /* Appended for live B6G3A exact-access build or validation rejection. */
    QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ACCESS = 22
} QXP_D3D9_DRAW_PREPARE_ERROR_CODE;

/* Fixed, deterministic status.  Only the nested member for the primary
 * failure is populated; every unrelated field remains zero. */
typedef struct qxp_d3d9_draw_prepare_status {
    DWORD code;
    DWORD stage;
    DWORD stream;
    DWORD input;
    DWORD interface_index;
    QXP_D3D9_DRAW_STATE_STATUS draw_state_error;
    QXP_D3D9_SHADER_OBJECT_STATUS shader_object_error;
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_D3D9_VERTEX_LINK_STATUS semantic_link_error;
    QXP_VERTEX_SHADER_GLSL_ERROR vertex_lowering_error;
} QXP_D3D9_DRAW_PREPARE_STATUS;

typedef struct qxp_d3d9_draw_stage_interface {
    DWORD pixel_required_mask;
    DWORD vertex_available_mask;
    DWORD matched_mask;
    DWORD missing_mask;
    DWORD pixel_centroid_mask;
    DWORD matched_centroid_mask;
    BYTE pixel_required_components[QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT];
    BYTE vertex_available_components[QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT];
    BYTE matched_components[QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT];
    BYTE missing_components[QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT];
    BYTE missing_policy[QXP_D3D9_DRAW_INTERFACE_SLOT_COUNT];
} QXP_D3D9_DRAW_STAGE_INTERFACE;

typedef struct qxp_d3d9_draw_owner_identity {
    unsigned long context_handle;
    unsigned long process_id;
    ULONG_PTR device_identity;
} QXP_D3D9_DRAW_OWNER_IDENTITY;

typedef struct qxp_d3d9_draw_effective_constants {
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
} QXP_D3D9_DRAW_EFFECTIVE_CONSTANTS;

/* One allocator-owned immutable draw description.  It contains no pointer
 * fields and retains no pointer to draw state, object-table storage, shader
 * bytecode, or caller scratch.  device_identity is the exact opaque numeric
 * value from QXP_SHADER_OWNER::device and is never dereferenced here. */
typedef struct qxp_d3d9_draw_prepared {
    BOOL valid;
    DWORD mode;
    QXP_D3D9_DRAW_OWNER_IDENTITY owner;

    QXP_D3D9_DRAW_STATE raw_state;
    QXP_D3D9_PIXEL_BINDING_STATE raw_pixel_binding;

    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_VERTEX_DECLARATION9 declaration;
    QXP_D3D9_SHADER_BINDING vertex_shader_binding;
    DWORD vertex_shader_bytecode_size;
    QXP_D3D9_SHADER_DECODE_SUMMARY vertex_shader_summary;
    QXP_D3D9_SHADER_BINDING pixel_shader_binding;
    DWORD pixel_shader_bytecode_size;
    DWORD pixel_glsl_source_bytes;
    QXP_D3D9_SHADER_DECODE_SUMMARY pixel_shader_summary;
    QXP_PIXEL_SHADER_GLSL_METADATA pixel_metadata;
    /* Nonzero when no pixel object is bound and the pixel stage was
     * synthesized from the fixed-function combiner instead.  The signature
     * separates one combiner configuration from another in the program
     * cache, which is otherwise keyed only by shader bindings. */
    DWORD fixed_function_pixel_signature;
    QXP_FF_PIXEL_STATE fixed_function_pixel;
    /* Nonzero when the declaration carries POSITIONT and the vertex stage was
     * synthesized from it instead of from the bound shader, which the runtime
     * ignores for pre-transformed geometry.  The signature separates one
     * pre-transformed layout from another in the program cache, which is
     * otherwise keyed only by shader bindings. */
    DWORD positiont_vertex_signature;
    QXP_POSITIONT_VERTEX_STATE positiont_vertex;

    QXP_D3D9_VERTEX_SHADER_LINK vertex_link;
    QXP_VERTEX_SHADER_GLSL_METADATA vertex_metadata;
    DWORD vertex_glsl_source_bytes;
    QXP_D3D9_DRAW_STAGE_INTERFACE stage_interface;

    DWORD required_stream_mask;
    DWORD required_user_memory_stream_mask;
    DWORD required_stream_spans[QXP_D3D9_DRAW_STREAM_COUNT];

    DWORD undefined_vertex_input_mask;
    DWORD undefined_vertex_input_defaults
        [QXP_D3D9_DRAW_UNDEFINED_INPUT_COUNT]
        [QXP_D3D9_DRAW_UNDEFINED_INPUT_COMPONENT_COUNT];

    QXP_D3D9_DRAW_EFFECTIVE_CONSTANTS effective_constants;
} QXP_D3D9_DRAW_PREPARED;

/* Decoding, linking, lowering, and emitting a stage depend only on the bound
 * declaration, vertex function, pixel binding, and combiner state - never on
 * per-draw constants or stream offsets.  A title that redraws with the same
 * program therefore rebuilds an identical result every draw, so a cache lets
 * the derived half be reused while the per-draw half is always recomputed.
 * The key is the identity those derived parts are a pure function of;
 * anything outside it changing forces a full rebuild.
 *
 * Titles interleave programs within a frame - Prince of Persia alternates UI,
 * scene, and fade draws - so a single entry thrashes.  Four ways with a
 * round-robin victim hold a frame's working set.  The key carries the owner
 * identity, so one cache is safely shared by every context: two devices
 * cannot collide, and two contexts on one device should share.  The caller
 * serializes access with the same callbacks it uses for the object table. */
#define QXP_D3D9_DRAW_PREPARE_CACHE_WAYS 4

typedef struct qxp_d3d9_draw_prepare_cache_entry {
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_DRAW_OWNER_IDENTITY owner;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING vertex_function_binding;
    QXP_D3D9_PIXEL_BINDING_STATE pixel_binding;
    QXP_FF_PIXEL_STATE fixed_function_pixel;
    ULONGLONG table_generation;
    DWORD declaration_mode;
    DWORD legacy_fvf;
    BOOL has_fixed_function_pixel;
    BOOL valid;
} QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY;

typedef struct qxp_d3d9_draw_prepare_cache {
    QXP_D3D9_DRAW_PREPARE_CACHE_ENTRY ways[QXP_D3D9_DRAW_PREPARE_CACHE_WAYS];
    DWORD next_victim;
#ifdef QXP_D3D_PROFILE
    DWORD profile_hits;
    DWORD profile_misses;
#endif
} QXP_D3D9_DRAW_PREPARE_CACHE;

/* The caller serializes draw state, pixel binding, fixed-function combiner
 * state, and object table with the supplied callbacks.  Exactly three
 * temporary allocations are attempted in order: prepared result, maximum VS
 * bytecode, and one shader IR.  fixed_function_pixel describes the pixel
 * stage to synthesize when no pixel object is bound; passing NULL keeps an
 * absent pixel binding a refusal. */
QXP_D3D9_DRAW_PREPARED *QxpD3d9PrepareDrawSnapshot(
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARE_CACHE *cache,
    QXP_D3D9_DRAW_PREPARE_STATUS *status);

/* Reconstructs one canonical snapshot from current state and exact-generation
 * objects, then byte-compares every pointer-free execution-bearing field.
 * Canonical reconstruction uses the same three outside-lock allocations as
 * preparation and rejects both stale current state and hostile payload edits. */
BOOL QxpD3d9ValidatePreparedDraw(
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_FF_PIXEL_STATE *fixed_function_pixel,
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback,
    void *lock_opaque,
    QXP_D3D9_DRAW_PREPARE_STATUS *status);

void QxpD3d9DestroyPreparedDraw(
    QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator);

BOOL QxpD3d9DrawPrepareStatusIsResourceFailure(
    const QXP_D3D9_DRAW_PREPARE_STATUS *status);

#endif
