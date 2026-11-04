#ifndef QXP_D3D9_DRAW_ACCESS_H
#define QXP_D3D9_DRAW_ACCESS_H

#include <windef.h>

#include "qxp_d3d9_draw_envelope.h"

/*
 * B6G3A is a dormant, production-linked exact aggregate access planner.  It
 * allocates and locks nothing, retains no input pointer, and reads only the
 * caller-owned immutable index copy while a call is active.  It establishes
 * no pointer provenance, backing pin/copy, generation revalidation, execution,
 * cap, or DDI exposure.  There is no production live call and ABI stays 53.
 */

typedef enum qxp_d3d9_draw_access_error_code {
    QXP_D3D9_DRAW_ACCESS_OK = 0,
    QXP_D3D9_DRAW_ACCESS_ERROR_NULL_ARGUMENT = 1,
    QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE = 2,
    QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY = 3,
    QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_VALUE = 4,
    QXP_D3D9_DRAW_ACCESS_ERROR_LOGICAL_INDEX = 5,
    QXP_D3D9_DRAW_ACCESS_ERROR_COUNT_OVERFLOW = 6,
    QXP_D3D9_DRAW_ACCESS_ERROR_STREAM_RANGE = 7,
    QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE_SUBSET = 8,
    QXP_D3D9_DRAW_ACCESS_ERROR_VALIDATION = 9
} QXP_D3D9_DRAW_ACCESS_ERROR_CODE;

typedef enum qxp_d3d9_draw_access_field {
    QXP_D3D9_DRAW_ACCESS_FIELD_NONE = 0,
    QXP_D3D9_DRAW_ACCESS_FIELD_RECORDS = 1,
    QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE = 2,
    QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_COPY = 3,
    QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_VALUE = 4,
    QXP_D3D9_DRAW_ACCESS_FIELD_BASE_VERTEX = 5,
    QXP_D3D9_DRAW_ACCESS_FIELD_SCANNED_COUNT = 6,
    QXP_D3D9_DRAW_ACCESS_FIELD_STREAM_BYTES = 7,
    QXP_D3D9_DRAW_ACCESS_FIELD_OUTPUT = 8
} QXP_D3D9_DRAW_ACCESS_FIELD;

/* record_index and index_ordinal identify the exact source index failure;
 * stream identifies an exact-range failure.  The nested status preserves the
 * canonical B6G2 failure and otherwise remains wholly zero. */
typedef struct qxp_d3d9_draw_access_status {
    DWORD code;
    DWORD record_index;
    DWORD index_ordinal;
    DWORD stream;
    DWORD field;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_error;
} QXP_D3D9_DRAW_ACCESS_STATUS;

/* The scanned count is an explicit little/high DWORD pair so the immutable
 * byte image has no compiler-dependent 64-bit member alignment. */
typedef struct qxp_d3d9_draw_access_count64 {
    DWORD low;
    DWORD high;
} QXP_D3D9_DRAW_ACCESS_COUNT64;

/* Pointer-free, zero-canonical aggregate plan.  For nonindexed packets the
 * B6G2 stream and index ranges are copied unchanged.  Indexed stream ranges
 * are tightened to the extrema actually observed in the immutable index copy;
 * the B6G2 index range is retained because that is the exact copied extent. */
typedef struct qxp_d3d9_draw_access_plan {
    BOOL valid;
    DWORD opcode;
    DWORD record_count;
    DWORD nonempty_record_count;
    BOOL indexed;
    DWORD stream_mask;
    QXP_D3D9_DRAW_ACCESS_COUNT64 scanned_index_count;
    QXP_D3D9_DRAW_STREAM_ENVELOPE
        streams[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_INDEX_ENVELOPE index;
} QXP_D3D9_DRAW_ACCESS_PLAN;

/* envelope must be the canonical B6G2 result for these exact records,
 * prepared snapshot, and backings.  It is validated before any index byte is
 * read.  Index byte zero maps to envelope->index.byte_first and byte_count
 * must exactly equal its half-open length.  No index access requires
 * index_bytes == NULL and index_byte_count == 0. */
BOOL QxpD3d9BuildDrawAccess(
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, ULONG index_byte_count,
    QXP_D3D9_DRAW_ACCESS_PLAN *out,
    QXP_D3D9_DRAW_ACCESS_STATUS *status);

/* Canonically rebuilds and byte-compares the complete zero-initialized plan,
 * including padding.  The caller must keep the index copy immutable during a
 * call.  Different copies with the same observed extrema intentionally
 * validate to the same aggregate plan. */
BOOL QxpD3d9ValidateDrawAccess(
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan,
    const QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT *envelope,
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_BACKING_SNAPSHOT *backings,
    const void *index_bytes, ULONG index_byte_count,
    QXP_D3D9_DRAW_ACCESS_STATUS *status);

#endif
