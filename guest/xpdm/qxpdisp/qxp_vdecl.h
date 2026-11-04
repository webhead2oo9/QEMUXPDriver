#ifndef QXP_VDECL_H
#define QXP_VDECL_H

#include <windef.h>

#define QXP_VERTEX_DECLARATION_INPUT_COUNT 16UL
#define QXP_VERTEX_DECLARATION_STREAM_COUNT 16UL
#define QXP_VERTEX_DECLARATION_CONSTANT_COUNT 96UL
/* Per-source-stream span cap (advertised MaxStreamStride). */
#define QXP_VERTEX_DECLARATION_MAX_STRIDE 255UL
/* Canonical packed record cap: 16 inputs * FLOAT4 (16 bytes) = 256. */
#define QXP_VERTEX_DECLARATION_MAX_PACKED_STRIDE 256UL

typedef struct qxp_vertex_declaration_element {
    DWORD input_register;
    DWORD data_type;
    DWORD byte_offset;         /* offset within the canonical packed record */
    DWORD byte_count;
    DWORD source_stream;       /* 0..15: bound stream supplying this element */
    DWORD source_byte_offset;  /* byte offset within the source stream vertex */
} QXP_VERTEX_DECLARATION_ELEMENT;

typedef struct qxp_vertex_declaration {
    DWORD byte_stride;         /* canonical packed record size (all elements) */
    DWORD input_mask;
    DWORD element_count;
    DWORD stream_mask;         /* bit s set if stream s supplies >=1 element */
    DWORD stream_spans[QXP_VERTEX_DECLARATION_STREAM_COUNT];
    QXP_VERTEX_DECLARATION_ELEMENT
        elements[QXP_VERTEX_DECLARATION_INPUT_COUNT];
    DWORD defined_constants[3];
    DWORD constant_values[QXP_VERTEX_DECLARATION_CONSTANT_COUNT][4];
} QXP_VERTEX_DECLARATION;

/* One bound source stream, resolved once per draw. */
typedef struct qxp_vertex_stream_view {
    const BYTE *base;   /* first byte of the stream allocation */
    DWORD bytes;        /* trustworthy allocation length in bytes */
    DWORD stride;       /* per-vertex stride in bytes */
    BOOL wraps;         /* wrap element reads modulo bytes (execute buffers) */
} QXP_VERTEX_STREAM_VIEW;

BOOL QxpParseVertexDeclaration8(
    const DWORD *tokens, DWORD token_bytes,
    QXP_VERTEX_DECLARATION *declaration);

/* Validates the declaration-only Direct3D 8 fixed-function subset supported
 * by the transport, derives its exact legacy FVF, and rewrites each retained
 * element's destination byte_offset into canonical FVF order. Source stream,
 * source offset, and span provenance remain unchanged. On failure fvf is zero
 * and declaration must not be consumed. */
BOOL QxpTranslateFixedVertexDeclaration8(
    QXP_VERTEX_DECLARATION *declaration, DWORD *fvf);

/* Gather one logical vertex from its declared source streams into a canonical
 * packed record. views[] has STREAM_COUNT entries; only the streams named by
 * declaration->stream_mask are read. vertex_index is the logical per-stream
 * index (BaseVertexIndex + element for indexed draws, VStart + n otherwise). */
BOOL QxpGatherVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const QXP_VERTEX_STREAM_VIEW *views,
    DWORD vertex_index,
    BYTE *destination, DWORD destination_bytes);

/* Fixed-function variant: retains D3DCOLOR in its source DWORD byte order for
 * qxp_vertex_copy. The programmable gather above canonicalizes D3DCOLOR to
 * RGBA vertex-attribute byte order. */
BOOL QxpGatherFixedVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const QXP_VERTEX_STREAM_VIEW *views,
    DWORD vertex_index,
    BYTE *destination, DWORD destination_bytes);

/* Single-stream (stream 0 only) convenience over the gather; source points at
 * the vertex already. Fails if the declaration references any stream but 0. */
BOOL QxpRepackVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const BYTE *source, DWORD source_stride,
    BYTE *destination, DWORD destination_bytes);

#endif
