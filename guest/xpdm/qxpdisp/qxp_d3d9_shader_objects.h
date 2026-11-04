#ifndef QXP_D3D9_SHADER_OBJECTS_H
#define QXP_D3D9_SHADER_OBJECTS_H

#include <windef.h>

#include "qxp_shader_ir.h"
#include "qxp_shader_owner.h"
#include "qxp_pshader_glsl.h"
#include "qxp_vdecl9.h"

/* Private D3D9-only DP2 values.  Keep these names distinct from every WDK
 * D3DNTDP2OP_* spelling while directdraw.c remains a D3D8 translation unit. */
#define QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION 71U
#define QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION 72U
#define QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION    73U
#define QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION    74U
#define QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION    75U
#define QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION       76U

/* Standard D3D pixel-function opcode values.  The production DDI-v4 dispatcher
 * classifies each CREATE by exact profile, then enforces one exact-owner
 * handle namespace across legacy PS1.x and SM2 before profile-less SET or
 * DELETE.  Unsupported profiles and invalid object state fail closed. */
#define QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION 54U
#define QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION 55U
#define QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION    56U

#define QXP_D3D9_SHADER_OBJECT_CAPACITY 256UL
#define QXP_D3D9_CREATE_RECORD_BYTES    8UL
#define QXP_D3D9_HANDLE_RECORD_BYTES    4UL

/* Bit zero distinguishes private D3D9 declaration objects from the legacy
 * FVF values accepted by SETVERTEXSHADERDECL. */
#define QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT 0x00000001UL

typedef struct qxp_d3d9_private_create_declaration {
    DWORD handle;
    DWORD count;
} QXP_D3D9_PRIVATE_CREATE_DECLARATION;

typedef struct qxp_d3d9_private_create_function {
    DWORD handle;
    DWORD size;
} QXP_D3D9_PRIVATE_CREATE_FUNCTION;

typedef struct qxp_d3d9_private_handle {
    DWORD handle;
} QXP_D3D9_PRIVATE_HANDLE;

typedef char QXP_D3D9_private_create_declaration_size_must_be_8[
    sizeof(QXP_D3D9_PRIVATE_CREATE_DECLARATION) == 8 ? 1 : -1];
typedef char QXP_D3D9_private_create_function_size_must_be_8[
    sizeof(QXP_D3D9_PRIVATE_CREATE_FUNCTION) == 8 ? 1 : -1];
typedef char QXP_D3D9_private_handle_size_must_be_4[
    sizeof(QXP_D3D9_PRIVATE_HANDLE) == 4 ? 1 : -1];
typedef char QXP_D3D9_private_vertex_element_size_must_be_8[
    sizeof(QXP_VERTEX_ELEMENT9) == 8 ? 1 : -1];

typedef enum qxp_d3d9_shader_object_error_code {
    QXP_D3D9_SHADER_OBJECT_OK = 0,
    QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_SHADER_OBJECT_ERROR_OWNER = 2,
    QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER = 3,
    QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE = 4,
    QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT = 5,
    QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE = 6,
    QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD = 7,
    QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION = 8,
    QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_PARSE = 9,
    QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE = 10,
    QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE = 11,
    QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE = 12,
    QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL = 13,
    QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND = 14,
    QXP_D3D9_SHADER_OBJECT_ERROR_BINDING = 15,
    QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT = 16,
    QXP_D3D9_SHADER_OBJECT_ERROR_OUTPUT_BUFFER = 17,
    QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE = 18,
    QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED = 19,
    QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE = 20,
    QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_LOWERING = 21
} QXP_D3D9_SHADER_OBJECT_ERROR_CODE;

/* byte_offset is relative to the record accepted by a framing helper and to
 * the declaration/function payload accepted by a prepare helper.  Nested B1,
 * B0, and B2 errors are copied exactly; unrelated nested members stay zero. */
typedef struct qxp_d3d9_shader_object_status {
    DWORD code;
    DWORD byte_offset;
    QXP_VERTEX_DECLARATION9_ERROR declaration_error;
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_PIXEL_SHADER_GLSL_ERROR glsl_error;
} QXP_D3D9_SHADER_OBJECT_STATUS;

typedef void *(*QXP_D3D9_SHADER_ALLOCATE)(ULONG byte_count, void *opaque);
typedef void (*QXP_D3D9_SHADER_RELEASE)(void *allocation, void *opaque);

typedef struct qxp_d3d9_shader_allocator {
    QXP_D3D9_SHADER_ALLOCATE allocate;
    QXP_D3D9_SHADER_RELEASE release;
    void *opaque;
} QXP_D3D9_SHADER_ALLOCATOR;

typedef void (*QXP_D3D9_SHADER_LOCK_CALLBACK)(void *opaque);

typedef struct qxp_d3d9_shader_binding {
    DWORD handle;
    ULONGLONG generation;
} QXP_D3D9_SHADER_BINDING;

/* SET and DELETE carry no profile token.  The production dispatcher therefore
 * resolves each exact-owner numeric handle across the mutually exclusive
 * legacy PS1.x and native representations while holding its table lock. */
typedef enum qxp_d3d9_pixel_function_route {
    QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE = 0,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY = 1,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2 = 2
} QXP_D3D9_PIXEL_FUNCTION_ROUTE;

typedef struct qxp_d3d9_pixel_binding_state {
    DWORD legacy_handle;
    QXP_D3D9_SHADER_BINDING sm2;
} QXP_D3D9_PIXEL_BINDING_STATE;

/* `native` is true when the create record's profile is one the D3D9 object
 * table can hold -- ps_1_1 or ps_2_0, both of which lower to GLSL.  Every
 * other legacy pixel profile is false and stays on the ARB pipeline. */
typedef BOOL (*QXP_D3D9_PIXEL_CREATE_CALLBACK)(
    DWORD handle, const DWORD *tokens, DWORD bytecode_size, BOOL native,
    void *opaque, QXP_D3D9_SHADER_OBJECT_STATUS *status);
typedef BOOL (*QXP_D3D9_PIXEL_HANDLE_CALLBACK)(
    DWORD handle, void *opaque, QXP_D3D9_SHADER_OBJECT_STATUS *status);

typedef struct qxp_d3d9_pixel_dispatch_callbacks {
    QXP_D3D9_PIXEL_CREATE_CALLBACK create;
    QXP_D3D9_PIXEL_HANDLE_CALLBACK set;
    QXP_D3D9_PIXEL_HANDLE_CALLBACK delete_function;
    void *opaque;
} QXP_D3D9_PIXEL_DISPATCH_CALLBACKS;

typedef struct qxp_d3d9_vertex_declaration_object {
    QXP_SHADER_OWNER owner;
    DWORD handle;
    ULONGLONG generation;
    QXP_VERTEX_DECLARATION9 declaration;
    /* Immutable prepare-time cache.  Valid may describe all eight FVF texture
     * sets even though the current legacy transport consumes at most four. */
    BOOL legacy_fvf_valid;
    DWORD legacy_fvf;
    DWORD legacy_vertex_stride;
} QXP_D3D9_VERTEX_DECLARATION_OBJECT;

/* Only scalar B0 results persist.  The deliberately large QXP_SHADER_IR is a
 * temporary prepare-time allocation and is never embedded in an object. */
typedef struct qxp_d3d9_shader_decode_summary {
    DWORD stage;
    DWORD version_token;
    DWORD token_count;
    DWORD instruction_count;
    DWORD declaration_count;
    DWORD definition_count;
    DWORD comment_count;
    DWORD slot_count;
    DWORD arithmetic_slot_count;
    DWORD texture_slot_count;
    DWORD flow_slot_count;
    DWORD block_count;
} QXP_D3D9_SHADER_DECODE_SUMMARY;

typedef struct qxp_d3d9_vertex_function_object {
    QXP_SHADER_OWNER owner;
    DWORD handle;
    ULONGLONG generation;
    DWORD bytecode_size;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
    /* C89 trailing storage.  Allocate sizeof(object) + bytecode_size - 4. */
    DWORD bytecode[1];
} QXP_D3D9_VERTEX_FUNCTION_OBJECT;

typedef struct qxp_d3d9_pixel_function_object {
    QXP_SHADER_OWNER owner;
    DWORD handle;
    ULONGLONG generation;
    DWORD bytecode_size;
    DWORD glsl_source_bytes;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
    QXP_PIXEL_SHADER_GLSL_METADATA metadata;
    /* C89 trailing storage.  Allocate sizeof(object) + bytecode_size - 4. */
    DWORD bytecode[1];
} QXP_D3D9_PIXEL_FUNCTION_OBJECT;

typedef struct qxp_d3d9_shader_object_table {
    QXP_D3D9_VERTEX_DECLARATION_OBJECT
        *declarations[QXP_D3D9_SHADER_OBJECT_CAPACITY];
    QXP_D3D9_VERTEX_FUNCTION_OBJECT
        *functions[QXP_D3D9_SHADER_OBJECT_CAPACITY];
    QXP_D3D9_PIXEL_FUNCTION_OBJECT
        *pixel_functions[QXP_D3D9_SHADER_OBJECT_CAPACITY];
    ULONGLONG generation_clock;
} QXP_D3D9_SHADER_OBJECT_TABLE;

typedef struct qxp_d3d9_vertex_declaration_snapshot {
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING binding;
    QXP_VERTEX_DECLARATION9 declaration;
} QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT;

typedef struct qxp_d3d9_vertex_function_snapshot {
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING binding;
    DWORD bytecode_size;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
} QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT;

typedef struct qxp_d3d9_pixel_function_snapshot {
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING binding;
    DWORD bytecode_size;
    DWORD glsl_source_bytes;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
    QXP_PIXEL_SHADER_GLSL_METADATA metadata;
} QXP_D3D9_PIXEL_FUNCTION_SNAPSHOT;

/* These helpers frame exactly one variable record.  They validate bounded
 * sizes by subtraction before any payload pointer can be advanced.  Every
 * scalar output, including consumed_bytes, is zero on failure. */
BOOL QxpD3d9FrameCreateVertexDeclaration(
    const void *record, ULONG available_bytes, DWORD *handle, DWORD *count,
    ULONG *consumed_bytes, QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9FrameCreateVertexFunction(
    const void *record, ULONG available_bytes, DWORD *handle,
    DWORD *bytecode_size, ULONG *consumed_bytes,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9FrameCreatePixelFunction(
    const void *record, ULONG available_bytes, DWORD *handle,
    DWORD *bytecode_size, ULONG *consumed_bytes,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Prepare parses/decodes and allocates without retaining an input pointer.
 * It performs no table access and must be called outside the table lock. */
BOOL QxpD3d9PrepareVertexDeclaration(
    const QXP_SHADER_OWNER *owner, DWORD handle,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9PrepareVertexFunction(
    const QXP_SHADER_OWNER *owner, DWORD handle, const DWORD *tokens,
    DWORD bytecode_size, const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9PreparePixelFunction(
    const QXP_SHADER_OWNER *owner, DWORD handle, const DWORD *tokens,
    DWORD bytecode_size, const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
void QxpD3d9DestroyVertexDeclaration(
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator);
void QxpD3d9DestroyVertexFunction(
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator);
void QxpD3d9DestroyPixelFunction(
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator);

/* All operations below that mutate a table or binding are deliberately
 * synchronization-free.  The caller must serialize them with its table lock.
 * Publish transfers ownership only on success; on failure the caller retains
 * and destroys the prepared object after dropping the lock. */
void QxpD3d9ShaderObjectTableInitialize(
    QXP_D3D9_SHADER_OBJECT_TABLE *table);
BOOL QxpD3d9PublishVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9PublishVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9PublishPixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Unified pixel routing helpers are also synchronization-free.  The caller
 * computes legacy_handle_exists for the same exact owner and handle under the
 * same table lock.  This makes the final cross-representation collision check
 * atomic with publication, SET, or detachment. */
BOOL QxpD3d9PublishPixelFunctionUnified(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared,
    BOOL legacy_handle_exists,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9SetPixelFunctionUnified(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    BOOL legacy_handle_exists, QXP_D3D9_PIXEL_BINDING_STATE *binding,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE *route,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9DeletePixelFunctionUnified(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    BOOL legacy_handle_exists, QXP_D3D9_PIXEL_BINDING_STATE *binding,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE *route,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
void QxpD3d9ClearPixelBindingState(
    QXP_D3D9_PIXEL_BINDING_STATE *binding);
BOOL QxpD3d9PixelBindingRequiresSm2Execution(
    const QXP_D3D9_PIXEL_BINDING_STATE *binding);

const QXP_D3D9_VERTEX_DECLARATION_OBJECT *QxpD3d9FindVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle);
const QXP_D3D9_VERTEX_FUNCTION_OBJECT *QxpD3d9FindVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle);
const QXP_D3D9_PIXEL_FUNCTION_OBJECT *QxpD3d9FindPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle);

BOOL QxpD3d9SetVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding, DWORD *legacy_fvf,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9SetVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9SetPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9DeleteVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9DeleteVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9DeletePixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* A zero binding snapshots successfully as an all-zero result.  Function
 * bytecode is optional: NULL/zero requests metadata only; otherwise the
 * caller-provided buffer must hold bytecode_size exact bytes. */
BOOL QxpD3d9SnapshotVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT *snapshot,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9SnapshotVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT *snapshot, void *bytecode,
    ULONG bytecode_capacity, QXP_D3D9_SHADER_OBJECT_STATUS *status);
BOOL QxpD3d9SnapshotPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_PIXEL_FUNCTION_SNAPSHOT *snapshot, void *bytecode,
    ULONG bytecode_capacity, QXP_D3D9_SHADER_OBJECT_STATUS *status);

void QxpD3d9ClearShaderBinding(QXP_D3D9_SHADER_BINDING *binding);
QXP_D3D9_VERTEX_DECLARATION_OBJECT *QxpD3d9DetachOwnerVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner);
QXP_D3D9_VERTEX_FUNCTION_OBJECT *QxpD3d9DetachOwnerVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner);
QXP_D3D9_PIXEL_FUNCTION_OBJECT *QxpD3d9DetachOwnerPixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner);
DWORD QxpD3d9VertexDeclarationCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table);
DWORD QxpD3d9VertexFunctionCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table);
DWORD QxpD3d9PixelFunctionCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table);
BOOL QxpD3d9ShaderObjectStatusIsResourceFailure(
    const QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Production DP2 oracle for private opcodes 71 through 76.  Fixed-size
 * batches are completely framed before mutation.  Variable create batches
 * retain earlier successful records if a later record fails.  consumed_bytes
 * counts successful records; error_offset is zero on success and the supplied
 * outer DP2 command offset on every failure. */
BOOL QxpD3d9ProcessShaderObjectBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_SHADER_BINDING *declaration_binding, DWORD *legacy_fvf,
    QXP_D3D9_SHADER_BINDING *function_binding,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Detach exact-owner objects while locked and release every allocation after
 * unlocking.  The two counts are deterministic and zero on validation error. */
BOOL QxpD3d9DestroyOwnerShaderObjects(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD *declaration_count, DWORD *function_count,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Production framing and profile-routing oracle for standard opcodes 54-56.
 * CREATE classifies each completely framed record independently.  Fixed SET
 * and DELETE batches are completely framed before their callbacks run. */
BOOL QxpD3d9DispatchPixelFunctionBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_PIXEL_DISPATCH_CALLBACKS *callbacks,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

/* Direct table-only PS2 oracle retained for lifecycle tests. */
BOOL QxpD3d9ProcessPixelFunctionBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_SHADER_BINDING *pixel_function_binding,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

BOOL QxpD3d9DestroyOwnerPixelFunctions(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD *pixel_function_count,
    QXP_D3D9_SHADER_OBJECT_STATUS *status);

#endif
