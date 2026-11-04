#ifndef QXP_D3D9_DRAW_CAPTURE_H
#define QXP_D3D9_DRAW_CAPTURE_H

#include <windef.h>

#include "qxp_d3d9_draw_access.h"

/*
 * Private immutable draw capture.  Version 3 captures the exact pointer-free
 * foundation and access plan, the exact indexed byte window when present, and
 * exact source bytes for as many as sixteen required vertex streams.  Indexed
 * captures are expanded to the existing nonindexed transport at execution;
 * no transport ABI change is made here.
 */

typedef enum qxp_d3d9_draw_capture_error_code {
    QXP_D3D9_DRAW_CAPTURE_OK = 0,
    QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE = 2,
    QXP_D3D9_DRAW_CAPTURE_ERROR_OPCODE = 3,
    QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_COUNT = 4,
    QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED = 5,
    QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING = 6,
    QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0 = 7,
    QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY = 8,
    QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY = 9,
    QXP_D3D9_DRAW_CAPTURE_ERROR_ALLOCATION = 10,
    QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_RECORDS = 11,
    QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY = 12,
    QXP_D3D9_DRAW_CAPTURE_ERROR_COPY = 13,
    QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS = 14,
    QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT = 15,
    QXP_D3D9_DRAW_CAPTURE_ERROR_HEADER = 16,
    QXP_D3D9_DRAW_CAPTURE_ERROR_HASH = 17,
    QXP_D3D9_DRAW_CAPTURE_ERROR_VALIDATION = 18,
    QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE = 19,
    QXP_D3D9_DRAW_CAPTURE_ERROR_INDEX_SOURCE = 20,
    /* Native execution reached a transient device/host transport refusal
     * after immutable capture validation completed. */
    QXP_D3D9_DRAW_CAPTURE_ERROR_TRANSPORT = 21
} QXP_D3D9_DRAW_CAPTURE_ERROR_CODE;

typedef enum qxp_d3d9_draw_capture_field {
    QXP_D3D9_DRAW_CAPTURE_FIELD_NONE = 0,
    QXP_D3D9_DRAW_CAPTURE_FIELD_ARGUMENT = 1,
    QXP_D3D9_DRAW_CAPTURE_FIELD_OPCODE = 2,
    QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_COUNT = 3,
    QXP_D3D9_DRAW_CAPTURE_FIELD_PREPARED = 4,
    QXP_D3D9_DRAW_CAPTURE_FIELD_BACKING = 5,
    QXP_D3D9_DRAW_CAPTURE_FIELD_ENVELOPE = 6,
    QXP_D3D9_DRAW_CAPTURE_FIELD_STREAM0 = 7,
    QXP_D3D9_DRAW_CAPTURE_FIELD_USER_MEMORY = 8,
    QXP_D3D9_DRAW_CAPTURE_FIELD_BLOB = 9,
    QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_PAYLOAD = 10,
    QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD = 11,
    QXP_D3D9_DRAW_CAPTURE_FIELD_HEADER = 12,
    QXP_D3D9_DRAW_CAPTURE_FIELD_HASH = 13,
    QXP_D3D9_DRAW_CAPTURE_FIELD_ACCESS_PLAN = 14,
    QXP_D3D9_DRAW_CAPTURE_FIELD_OUTPUT = 15,
    QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE = 16,
    QXP_D3D9_DRAW_CAPTURE_FIELD_INDEX_PAYLOAD = 17
} QXP_D3D9_DRAW_CAPTURE_FIELD;

typedef struct qxp_d3d9_draw_capture_status {
    DWORD code;
    DWORD field;
    DWORD stream;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_error;
    QXP_D3D9_DRAW_ACCESS_STATUS access_error;
} QXP_D3D9_DRAW_CAPTURE_STATUS;

/* One fixed pointer-free stream descriptor.  byte_first/last_exclusive are
 * absolute within the finite backing; payload_offset/bytes locate the exact
 * copied subrange in the blob.  Nonrequired descriptors are wholly zero. */
typedef struct qxp_d3d9_draw_capture_stream_descriptor {
    DWORD valid;
    DWORD source_kind;
    DWORD logical_first;
    DWORD logical_last_exclusive;
    DWORD byte_first;
    DWORD byte_last_exclusive;
    DWORD payload_offset;
    DWORD payload_bytes;
} QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR;

typedef struct qxp_d3d9_draw_capture_index_descriptor {
    DWORD valid;
    DWORD source_kind;
    DWORD stride;
    DWORD byte_first;
    DWORD byte_last_exclusive;
    DWORD payload_offset;
    DWORD payload_bytes;
    DWORD reserved;
} QXP_D3D9_DRAW_CAPTURE_INDEX_DESCRIPTOR;

/* Transient source view consumed only while capture is active.  The fixed
 * sixteen-entry array is never copied into the blob. */
typedef struct qxp_d3d9_draw_capture_source_view {
    const void *base;
    DWORD byte_count;
    DWORD kind;
} QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW;

/* Transient, validated execution view over one immutable captured blob.  The
 * pointers remain blob-owned and are legal only until that blob is destroyed;
 * no member may be published into context or surface state. */
typedef struct qxp_d3d9_captured_execution_stream {
    const void *payload;
    DWORD payload_bytes;
    DWORD stride;
    DWORD required_span;
    DWORD logical_first;
    DWORD logical_last_exclusive;
} QXP_D3D9_CAPTURED_EXECUTION_STREAM;

typedef struct qxp_d3d9_captured_execution_plan {
    const void *records;
    DWORD records_bytes;
    DWORD opcode;
    DWORD record_count;
    BOOL indexed;
    DWORD stream_mask;
    DWORD logical_first;
    DWORD logical_last_exclusive;
    const QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_CAPTURE_INDEX_DESCRIPTOR index;
    const void *index_payload;
    QXP_D3D9_CAPTURED_EXECUTION_STREAM
        streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
} QXP_D3D9_CAPTURED_EXECUTION_PLAN;

/* Common transport descriptors.  Byte offsets match the DP2 ...2 records. */
typedef struct qxp_d3d9_captured_draw2 {
    DWORD primitive_type;
    DWORD first_vertex_offset;
    DWORD primitive_count;
} QXP_D3D9_CAPTURED_DRAW2;

typedef struct qxp_d3d9_captured_indexed_draw2 {
    DWORD primitive_type;
    LONG base_vertex_offset;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index_offset;
    DWORD primitive_count;
} QXP_D3D9_CAPTURED_INDEXED_DRAW2;

/* Immutable, pointer-free, self-describing version-3 header. */
typedef struct qxp_d3d9_draw_capture_header {
    DWORD magic;
    DWORD version;
    DWORD valid;
    /* FNV-1a over the fixed header with this field treated as zero, rounded
     * header padding, exact records, and exact index payload.  Opaque vertex
     * stream payloads are excluded. */
    DWORD hash;

    DWORD opcode;
    DWORD record_count;
    DWORD record_size;
    DWORD header_size;
    DWORD payload_offset;
    DWORD records_offset;
    DWORD records_bytes;
    BOOL indexed;
    DWORD stream_mask;
    DWORD stream_count;
    DWORD stream_payload_bytes;
    DWORD index_payload_bytes;
    DWORD total_bytes;
    DWORD reserved;

    QXP_D3D9_DRAW_CAPTURE_INDEX_DESCRIPTOR index;
    QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR
        streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_DRAW_BACKING_SNAPSHOT backings;
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT envelope;
    QXP_D3D9_DRAW_ACCESS_PLAN access_plan;
} QXP_D3D9_DRAW_CAPTURE_HEADER;

#define QXP_D3D9_DRAW_CAPTURE_MAGIC 0x42364742UL
#define QXP_D3D9_DRAW_CAPTURE_VERSION 3UL
#define QXP_D3D9_DRAW_CAPTURE_VALID 1UL
/* Retained only for explicit stale-format rejection tests. */
#define QXP_D3D9_DRAW_CAPTURE_VERSION_NONINDEXED_ONLY 2UL
#define QXP_D3D9_DRAW_CAPTURE_STREAM_MASK 0x0000ffffUL
#define QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES (1024UL * 1024UL)
#define QXP_D3D9_DRAW_CAPTURE_PAYLOAD_ALIGNMENT 16UL
#define QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE \
    ((DWORD)sizeof(QXP_D3D9_DRAW_CAPTURE_HEADER))
#define QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET \
    ((QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE + \
      (QXP_D3D9_DRAW_CAPTURE_PAYLOAD_ALIGNMENT - 1UL)) & \
     ~(QXP_D3D9_DRAW_CAPTURE_PAYLOAD_ALIGNMENT - 1UL))

typedef void *(*QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK)(
    const void *address, ULONG byte_count, void *opaque);
typedef void (*QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK)(
    void *handle, void *opaque);
typedef BOOL (*QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK)(
    void *destination, const void *source, ULONG byte_count, void *opaque);

/* General entry point. index_source must be NULL for nonindexed captures and
 * must describe the complete system- or video-linear index backing for
 * indexed ones. Records, USER_MEMORY_CALL sources, and SYSTEM_LINEAR sources
 * are secured around the copy. VIDEO_LINEAR sources are driver-owned kernel
 * mappings: the caller must hold their immutable backing tokens and the
 * framebuffer lifetime mutation gate until this function returns, so they are
 * copied directly instead of being passed to EngSecureMem. */
BOOL QxpD3d9CaptureDrawStreams(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT],
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW *index_source,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

/* Nonindexed compatibility entry point. */
BOOL QxpD3d9CaptureNonindexedDrawStreams(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT],
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

/* Stream-0 compatibility wrappers retained for production callers. */
BOOL QxpD3d9CaptureNonindexedDraw(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *user_memory_base, DWORD user_memory_bytes,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

BOOL QxpD3d9CaptureNonindexedSurfaceDraw(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *surface_base, DWORD surface_bytes,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_DRAW_CAPTURE_SECURE_CALLBACK secure,
    QXP_D3D9_DRAW_CAPTURE_UNSECURE_CALLBACK unsecure,
    QXP_D3D9_DRAW_CAPTURE_COPY_CALLBACK copy,
    void *callback_opaque,
    void **out_blob, DWORD *out_blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

BOOL QxpD3d9ValidateCapturedDraw(
    const void *blob, DWORD blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

const void *QxpD3d9CapturedDrawRecords(const void *blob, DWORD *byte_count);
const void *QxpD3d9CapturedDrawIndexPayload(
    const void *blob, DWORD *byte_count);
const void *QxpD3d9CapturedDrawStreamPayload(
    const void *blob, DWORD stream, DWORD *byte_count);
const void *QxpD3d9CapturedDrawVertexPayload(
    const void *blob, DWORD *byte_count);

/* Builds the minimal pointer-free prepared authority for a legacy combined
 * vertex-shader declaration.  This is the producer path for stock DDI-fallback
 * VS1.1 multistream draws; native D3D9 object/SM2 state is rejected. */
BOOL QxpD3d9BuildLegacyMultistreamPreparedCapture(
    const QXP_D3D9_DRAW_STATE *draw_state,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel_binding,
    const QXP_D3D9_DRAW_OWNER_IDENTITY *owner,
    DWORD declaration_stream_mask,
    const DWORD declaration_stream_spans[QXP_D3D9_DRAW_STREAM_COUNT],
    QXP_D3D9_DRAW_PREPARED *prepared,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);


/* Builds an execution-only pointer view after complete blob revalidation.
 * Every required stream shares one logical range. */
BOOL QxpD3d9PrepareCapturedDrawExecution(
    const void *blob, DWORD blob_bytes,
    QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

BOOL QxpD3d9CapturedDrawRecordToDraw2(
    const QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan, DWORD record_index,
    QXP_D3D9_CAPTURED_DRAW2 *draw,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);
BOOL QxpD3d9CapturedDrawRecordToIndexedDraw2(
    const QXP_D3D9_CAPTURED_EXECUTION_PLAN *plan, DWORD record_index,
    QXP_D3D9_CAPTURED_INDEXED_DRAW2 *draw,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status);

BOOL QxpD3d9CaptureStatusIsResourceFailure(
    const QXP_D3D9_DRAW_CAPTURE_STATUS *status);

BOOL QxpD3d9BuildDrawUserMemoryEnvelope(
    BOOL user_memory_active, const void *vertices, DWORD vertex_offset,
    DWORD vertex_size, DWORD vertex_length,
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE *envelope,
    const void **out_base, DWORD *out_bytes);

/* General source-shape eligibility for the production inspector.  Indexed
 * capture requires system-linear vertex streams and a system-linear index
 * backing.  Admission only creates an immutable copy; execution validates it
 * again and never changes caps. */
BOOL QxpD3d9DrawCaptureStreamsAreEligible(
    BYTE opcode, BOOL indexed, BOOL user_memory_active,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings);

/* Existing exact stream-0 eligibility helpers remain compatibility seams. */
BOOL QxpD3d9DrawCaptureIsEligible(
    BYTE opcode, BOOL indexed, BOOL user_memory_active,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings);
BOOL QxpD3d9DrawCaptureIsSurfaceEligible(
    BYTE opcode, BOOL indexed,
    const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings);

void QxpD3d9DestroyCapturedDraw(
    void *blob, const QXP_D3D9_SHADER_ALLOCATOR *allocator);

#endif
