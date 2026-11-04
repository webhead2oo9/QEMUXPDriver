#ifndef QXP_VDECL9_H
#define QXP_VDECL9_H

#include <windef.h>

/*
 * Private, guest-only Direct3D 9 declaration representation.  The DDI gives
 * CREATEVERTEXSHADERDECL a counted array and does not append D3DDECL_END.
 * Callers own this bounded storage; the parser retains no input pointers.
 */
#define QXP_VERTEX_DECLARATION9_MAX_ELEMENTS       64UL
#define QXP_VERTEX_DECLARATION9_STREAM_COUNT       16UL
#define QXP_VERTEX_DECLARATION9_USAGE_COUNT        14UL
#define QXP_VERTEX_DECLARATION9_USAGE_INDEX_COUNT  16UL
#define QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX    15UL
#define QXP_VERTEX_ELEMENT9_BYTES                  8UL

/* Private values matching D3DDECLTYPE. */
typedef enum qxp_vertex_decl_type9 {
    QXP_VERTEX_DECLTYPE9_FLOAT1 = 0,
    QXP_VERTEX_DECLTYPE9_FLOAT2 = 1,
    QXP_VERTEX_DECLTYPE9_FLOAT3 = 2,
    QXP_VERTEX_DECLTYPE9_FLOAT4 = 3,
    QXP_VERTEX_DECLTYPE9_D3DCOLOR = 4,
    QXP_VERTEX_DECLTYPE9_UBYTE4 = 5,
    QXP_VERTEX_DECLTYPE9_SHORT2 = 6,
    QXP_VERTEX_DECLTYPE9_SHORT4 = 7,
    QXP_VERTEX_DECLTYPE9_UBYTE4N = 8,
    QXP_VERTEX_DECLTYPE9_SHORT2N = 9,
    QXP_VERTEX_DECLTYPE9_SHORT4N = 10,
    QXP_VERTEX_DECLTYPE9_USHORT2N = 11,
    QXP_VERTEX_DECLTYPE9_USHORT4N = 12,
    QXP_VERTEX_DECLTYPE9_UDEC3 = 13,
    QXP_VERTEX_DECLTYPE9_DEC3N = 14,
    QXP_VERTEX_DECLTYPE9_FLOAT16_2 = 15,
    QXP_VERTEX_DECLTYPE9_FLOAT16_4 = 16,
    QXP_VERTEX_DECLTYPE9_UNUSED = 17
} QXP_VERTEX_DECLTYPE9;

/* Private values matching D3DDECLMETHOD. */
typedef enum qxp_vertex_decl_method9 {
    QXP_VERTEX_DECLMETHOD9_DEFAULT = 0,
    QXP_VERTEX_DECLMETHOD9_PARTIALU = 1,
    QXP_VERTEX_DECLMETHOD9_PARTIALV = 2,
    QXP_VERTEX_DECLMETHOD9_CROSSUV = 3,
    QXP_VERTEX_DECLMETHOD9_UV = 4,
    QXP_VERTEX_DECLMETHOD9_LOOKUP = 5,
    QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED = 6
} QXP_VERTEX_DECLMETHOD9;

/* Private values matching D3DDECLUSAGE. */
typedef enum qxp_vertex_decl_usage9 {
    QXP_VERTEX_DECLUSAGE9_POSITION = 0,
    QXP_VERTEX_DECLUSAGE9_BLENDWEIGHT = 1,
    QXP_VERTEX_DECLUSAGE9_BLENDINDICES = 2,
    QXP_VERTEX_DECLUSAGE9_NORMAL = 3,
    QXP_VERTEX_DECLUSAGE9_PSIZE = 4,
    QXP_VERTEX_DECLUSAGE9_TEXCOORD = 5,
    QXP_VERTEX_DECLUSAGE9_TANGENT = 6,
    QXP_VERTEX_DECLUSAGE9_BINORMAL = 7,
    QXP_VERTEX_DECLUSAGE9_TESSFACTOR = 8,
    QXP_VERTEX_DECLUSAGE9_POSITIONT = 9,
    QXP_VERTEX_DECLUSAGE9_COLOR = 10,
    QXP_VERTEX_DECLUSAGE9_FOG = 11,
    QXP_VERTEX_DECLUSAGE9_DEPTH = 12,
    QXP_VERTEX_DECLUSAGE9_SAMPLE = 13
} QXP_VERTEX_DECLUSAGE9;

/* Private values matching D3DCAPS9.DeclTypes (D3DDTCAPS_*). */
#define QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4     0x00000001UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4N    0x00000002UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_SHORT2N    0x00000004UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_SHORT4N    0x00000008UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_USHORT2N   0x00000010UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_USHORT4N   0x00000020UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_UDEC3      0x00000040UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_DEC3N      0x00000080UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_2  0x00000100UL
#define QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_4  0x00000200UL

/* Private values matching D3DCAPS9.DevCaps and D3DCAPS9.DevCaps2. */
#define QXP_VERTEX_DECLARATION9_DEVCAPS_RTPATCHES   0x00400000UL
#define QXP_VERTEX_DECLARATION9_DEVCAPS_NPATCHES    0x01000000UL
#define QXP_VERTEX_DECLARATION9_DEVCAPS2_DMAPNPATCH 0x00000002UL
#define QXP_VERTEX_DECLARATION9_DEVCAPS2_PRESAMPLEDDMAPNPATCH \
    0x00000020UL
#define QXP_VERTEX_DECLARATION9_DEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET \
    0x00000040UL

/* Private numeric minimum, deliberately without the D3D shader token tag. */
#define QXP_VERTEX_DECLARATION9_VERTEX_SHADER_VERSION_2_0 0x00000200UL

/* Byte-layout-compatible with D3DVERTEXELEMENT9. */
typedef struct qxp_vertex_element9 {
    WORD stream;
    WORD offset;
    BYTE type;
    BYTE method;
    BYTE usage;
    BYTE usage_index;
} QXP_VERTEX_ELEMENT9;

typedef struct qxp_vertex_declaration9_element {
    WORD stream;
    WORD offset;
    BYTE type;
    BYTE method;
    BYTE usage;
    BYTE usage_index;
    DWORD byte_size;
} QXP_VERTEX_DECLARATION9_ELEMENT;

typedef struct qxp_vertex_declaration9 {
    BOOL valid;
    DWORD element_count;
    DWORD stream_mask;
    DWORD stream_spans[QXP_VERTEX_DECLARATION9_STREAM_COUNT];
    /* Minimum D3DCAPS9.MaxStreamStride needed by any referenced stream. */
    DWORD required_max_stream_stride;
    /* semantic_masks[u] has bit UsageIndex set for every declared semantic. */
    WORD semantic_masks[QXP_VERTEX_DECLARATION9_USAGE_COUNT];
    DWORD required_decl_types;
    DWORD required_dev_caps;
    DWORD required_dev_caps2;
    DWORD required_vertex_shader_version;
    BOOL has_positiont;
    /* Set by a tessellator method or the TESSFACTOR semantic.  TESSFACTOR
     * alone cannot select an exact RT-patch or N-patch DevCaps bit. */
    BOOL requires_tessellation;
    BOOL requires_displacement_mapping;
    BOOL requires_programmable_vertex_pipeline;
    QXP_VERTEX_DECLARATION9_ELEMENT
        elements[QXP_VERTEX_DECLARATION9_MAX_ELEMENTS];
} QXP_VERTEX_DECLARATION9;

typedef enum qxp_vertex_declaration9_error_code {
    QXP_VERTEX_DECLARATION9_ERROR_OK = 0,
    QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT = 1,
    QXP_VERTEX_DECLARATION9_ERROR_ELEMENT_COUNT = 2,
    QXP_VERTEX_DECLARATION9_ERROR_SENTINEL = 3,
    QXP_VERTEX_DECLARATION9_ERROR_STREAM_RANGE = 4,
    QXP_VERTEX_DECLARATION9_ERROR_TYPE = 5,
    QXP_VERTEX_DECLARATION9_ERROR_METHOD = 6,
    QXP_VERTEX_DECLARATION9_ERROR_USAGE_RANGE = 7,
    QXP_VERTEX_DECLARATION9_ERROR_USAGE_INDEX_RANGE = 8,
    QXP_VERTEX_DECLARATION9_ERROR_OFFSET_ALIGNMENT = 9,
    QXP_VERTEX_DECLARATION9_ERROR_DUPLICATE_SEMANTIC = 10,
    QXP_VERTEX_DECLARATION9_ERROR_NONCONSECUTIVE_STREAM = 11,
    QXP_VERTEX_DECLARATION9_ERROR_UNUSED_TYPE = 12,
    QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE = 13,
    QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE = 14,
    QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET = 15,
    QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE = 16,
    QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_COUNT = 17,
    QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_MIXED_METHOD = 18,
    QXP_VERTEX_DECLARATION9_ERROR_TESSFACTOR = 19,
    QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_STREAM = 20,
    QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_ORDER = 21,
    QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_METHOD = 22,
    QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_POSITION = 23,
    QXP_VERTEX_DECLARATION9_ERROR_TESSELLATION_TYPE = 24,
    QXP_VERTEX_DECLARATION9_ERROR_OFFSET_OVERFLOW = 25,
    /* Parsed object does not exactly reproduce from its retained elements. */
    QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE = 26
} QXP_VERTEX_DECLARATION9_ERROR_CODE;

typedef struct qxp_vertex_declaration9_error {
    DWORD code;
    DWORD element_index;
    DWORD byte_offset;
} QXP_VERTEX_DECLARATION9_ERROR;

/*
 * Parses exactly element_count DDI elements (there is no D3DDECL_END entry).
 * On failure, a non-NULL declaration is entirely zeroed and error identifies
 * the deterministic offending element.  On success, declaration->valid is
 * TRUE and error is all zero.  No allocation or input retention occurs.
 */
BOOL QxpParseVertexDeclaration9(
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    QXP_VERTEX_DECLARATION9 *declaration,
    QXP_VERTEX_DECLARATION9_ERROR *error);

/*
 * Reconstructs and reparses an existing parser result, then requires an exact
 * byte-for-byte match.  This shares every parser cross-field and derived-cap
 * invariant with later consumers without duplicating the parser.  No input
 * pointer is retained and error is entirely zero on success.
 */
BOOL QxpValidateParsedVertexDeclaration9(
    const QXP_VERTEX_DECLARATION9 *declaration,
    QXP_VERTEX_DECLARATION9_ERROR *error);

/*
 * Converts only the exactly packed, stream-zero fixed-function subset to a
 * legacy D3DFVF value.  Both outputs are zero on every failure.  The returned
 * stride is the packed byte extent represented by the FVF; no padding, gap,
 * overlap, reordered slot, or unsupported semantic is accepted.
 */
BOOL QxpTranslateVertexDeclaration9ToFvf(
    const QXP_VERTEX_DECLARATION9 *declaration,
    DWORD *fvf, DWORD *vertex_stride);

/*
 * Expands the exact fixed-function FVF subset accepted by the legacy vertex
 * decoder into the declaration model used by native D3D9 preparation.  The
 * result is reparsed through the ordinary declaration validator.  Unsupported
 * bits, malformed texture-coordinate sizes, and any layout beyond the shared
 * stream-zero contract fail closed; declaration is entirely zero on failure.
 */
BOOL QxpBuildVertexDeclaration9FromFvf(
    DWORD fvf, QXP_VERTEX_DECLARATION9 *declaration);

#endif
