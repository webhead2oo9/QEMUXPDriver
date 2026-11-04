#ifndef QXP_D3D9_DRAW_ENVELOPE_H
#define QXP_D3D9_DRAW_ENVELOPE_H

#include <windef.h>

#include "qxp_d3d9_draw_backing.h"
#include "qxp_d3d9_draw_gate.h"

/* D3DPRIMITIVETYPE values carried by the six fixed DP2 draw records. */
#define QXP_D3D9_PRIMITIVE_POINTLIST     1UL
#define QXP_D3D9_PRIMITIVE_LINELIST      2UL
#define QXP_D3D9_PRIMITIVE_LINESTRIP     3UL
#define QXP_D3D9_PRIMITIVE_TRIANGLELIST  4UL
#define QXP_D3D9_PRIMITIVE_TRIANGLESTRIP 5UL
#define QXP_D3D9_PRIMITIVE_TRIANGLEFAN   6UL

/* Exact x86 field offsets for the private fixed-record layouts.  Production
 * compile-time assertions compare these constants and the sizes from
 * qxp_d3d9_draw_gate.h against the WDK D3DNTHAL definitions. */
#define QXP_D3D9_POINTS_OFFSET_COUNT 0UL
#define QXP_D3D9_POINTS_OFFSET_START 2UL

#define QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_TYPE 0UL
#define QXP_D3D9_DRAWPRIMITIVE_OFFSET_VERTEX_START 4UL
#define QXP_D3D9_DRAWPRIMITIVE_OFFSET_PRIMITIVE_COUNT 8UL

#define QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_TYPE 0UL
#define QXP_D3D9_DRAWINDEXED_OFFSET_BASE_VERTEX 4UL
#define QXP_D3D9_DRAWINDEXED_OFFSET_MIN_INDEX 8UL
#define QXP_D3D9_DRAWINDEXED_OFFSET_NUM_VERTICES 12UL
#define QXP_D3D9_DRAWINDEXED_OFFSET_START_INDEX 16UL
#define QXP_D3D9_DRAWINDEXED_OFFSET_PRIMITIVE_COUNT 20UL

#define QXP_D3D9_CLIPPEDFAN_OFFSET_FIRST_VERTEX 0UL
#define QXP_D3D9_CLIPPEDFAN_OFFSET_EDGE_FLAGS 4UL
#define QXP_D3D9_CLIPPEDFAN_OFFSET_PRIMITIVE_COUNT 8UL

#define QXP_D3D9_DRAWPRIMITIVE2_OFFSET_PRIMITIVE_TYPE 0UL
#define QXP_D3D9_DRAWPRIMITIVE2_OFFSET_FIRST_VERTEX 4UL
#define QXP_D3D9_DRAWPRIMITIVE2_OFFSET_PRIMITIVE_COUNT 8UL

#define QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_TYPE 0UL
#define QXP_D3D9_DRAWINDEXED2_OFFSET_BASE_VERTEX 4UL
#define QXP_D3D9_DRAWINDEXED2_OFFSET_MIN_INDEX 8UL
#define QXP_D3D9_DRAWINDEXED2_OFFSET_NUM_VERTICES 12UL
#define QXP_D3D9_DRAWINDEXED2_OFFSET_START_INDEX 16UL
#define QXP_D3D9_DRAWINDEXED2_OFFSET_PRIMITIVE_COUNT 20UL

typedef enum qxp_d3d9_draw_envelope_error_code {
    QXP_D3D9_DRAW_ENVELOPE_OK = 0,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_BACKING_STATE = 2,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_OPCODE = 3,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_TRUNCATION = 4,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_TYPE = 5,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_PRIMITIVE_COUNT_OVERFLOW = 6,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_BASE = 7,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM0_ALIGNMENT = 8,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_DECLARED_INDEXED_WINDOW = 9,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_FREQUENCY_UNSUPPORTED = 10,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_STREAM_RANGE = 11,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_INDEX_RANGE = 12,
    QXP_D3D9_DRAW_ENVELOPE_ERROR_VALIDATION = 13
} QXP_D3D9_DRAW_ENVELOPE_ERROR_CODE;

typedef enum qxp_d3d9_draw_envelope_field {
    QXP_D3D9_DRAW_ENVELOPE_FIELD_NONE = 0,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_RECORDS = 1,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_OPCODE = 2,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_RECORD_COUNT = 3,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_PREPARED = 4,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_BACKING = 5,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_OWNER = 6,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_GENERATION = 7,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_MASK = 8,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_RECORD = 9,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_INDEX_RECORD = 10,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_PRIMITIVE_TYPE = 11,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_PRIMITIVE_COUNT = 12,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_FIRST_VERTEX = 13,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_BASE_VERTEX = 14,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_MIN_INDEX = 15,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_NUM_VERTICES = 16,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_START_INDEX = 17,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_DIVIDER = 18,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_STREAM_BYTES = 19,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_INDEX_BYTES = 20,
    QXP_D3D9_DRAW_ENVELOPE_FIELD_OUTPUT = 21
} QXP_D3D9_DRAW_ENVELOPE_FIELD;

typedef struct qxp_d3d9_draw_envelope_status {
    DWORD code;
    DWORD record_index;
    DWORD stream;
    DWORD field;
} QXP_D3D9_DRAW_ENVELOPE_STATUS;

/* For a finite linear backing, byte_first/last_exclusive are absolute backing
 * offsets.  For the admitted stream-0 execute ring, byte_first is the
 * normalized ring start and byte_last_exclusive is the unrolled end; their
 * difference is the exact linear payload capture must materialize. */
typedef struct qxp_d3d9_draw_stream_envelope {
    BOOL valid;
    DWORD stream;
    ULONGLONG logical_first;
    ULONGLONG logical_last_exclusive;
    ULONGLONG byte_first;
    ULONGLONG byte_last_exclusive;
} QXP_D3D9_DRAW_STREAM_ENVELOPE;

typedef struct qxp_d3d9_draw_index_envelope {
    BOOL valid;
    ULONGLONG byte_first;
    ULONGLONG byte_last_exclusive;
} QXP_D3D9_DRAW_INDEX_ENVELOPE;

/* Immutable, bounded and pointer-free result.  This is declared/conservative
 * record-envelope validation.  It is not exact indexed access validation: no
 * index value is read and MinIndex/NumVertices remains the runtime-declared
 * conservative authority for indexed vertex extents. */
typedef struct qxp_d3d9_draw_envelope_snapshot {
    BOOL valid;
    DWORD opcode;
    DWORD record_count;
    DWORD nonempty_record_count;
    BOOL indexed;
    DWORD stream_mask;
    QXP_D3D9_DRAW_STREAM_ENVELOPE
        streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_INDEX_ENVELOPE index;
} QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT;

/* Builds declared/conservative record envelopes after a canonical B6G1
 * backing snapshot has succeeded.  No allocation or lock is taken, and no
 * caller pointer is retained. */
BOOL QxpD3d9BuildDrawEnvelope(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *out,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status);

/* Canonically reconstructs from the same transient inputs and byte-compares
 * every pointer-free output byte, including padding. */
BOOL QxpD3d9ValidateDrawEnvelope(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *snapshot,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    QXP_D3D9_DRAW_ENVELOPE_STATUS *status);

#endif
