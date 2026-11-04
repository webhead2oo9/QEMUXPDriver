#include <windef.h>

#include "qxp_vdecl9.h"

#define QXP_VDECL9_INDEX_NONE 0xffffffffUL

static void qxp_vdecl9_zero(void *memory, DWORD byte_count)
{
    volatile BYTE *bytes;
    DWORD index;

    /* Volatile prevents the WDK optimizer from introducing a CRT memset. */
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index)
        bytes[index] = 0;
}

static void qxp_vdecl9_set_error(QXP_VERTEX_DECLARATION9_ERROR *error,
                                 DWORD code, DWORD element_index)
{
    if (!error) return;
    error->code = code;
    error->element_index = element_index;
    if (element_index <= 0xffffffffUL / QXP_VERTEX_ELEMENT9_BYTES)
        error->byte_offset = element_index * QXP_VERTEX_ELEMENT9_BYTES;
    else
        error->byte_offset = 0xffffffffUL;
}

static BOOL qxp_vdecl9_fail(QXP_VERTEX_DECLARATION9 *declaration,
                            QXP_VERTEX_DECLARATION9_ERROR *error,
                            DWORD code, DWORD element_index)
{
    qxp_vdecl9_zero(declaration, (DWORD)sizeof(*declaration));
    qxp_vdecl9_set_error(error, code, element_index);
    return FALSE;
}

static DWORD qxp_vdecl9_type_size(DWORD type)
{
    switch (type) {
    case QXP_VERTEX_DECLTYPE9_FLOAT1: return 4;
    case QXP_VERTEX_DECLTYPE9_FLOAT2: return 8;
    case QXP_VERTEX_DECLTYPE9_FLOAT3: return 12;
    case QXP_VERTEX_DECLTYPE9_FLOAT4: return 16;
    case QXP_VERTEX_DECLTYPE9_D3DCOLOR: return 4;
    case QXP_VERTEX_DECLTYPE9_UBYTE4: return 4;
    case QXP_VERTEX_DECLTYPE9_SHORT2: return 4;
    case QXP_VERTEX_DECLTYPE9_SHORT4: return 8;
    case QXP_VERTEX_DECLTYPE9_UBYTE4N: return 4;
    case QXP_VERTEX_DECLTYPE9_SHORT2N: return 4;
    case QXP_VERTEX_DECLTYPE9_SHORT4N: return 8;
    case QXP_VERTEX_DECLTYPE9_USHORT2N: return 4;
    case QXP_VERTEX_DECLTYPE9_USHORT4N: return 8;
    case QXP_VERTEX_DECLTYPE9_UDEC3: return 4;
    case QXP_VERTEX_DECLTYPE9_DEC3N: return 4;
    case QXP_VERTEX_DECLTYPE9_FLOAT16_2: return 4;
    case QXP_VERTEX_DECLTYPE9_FLOAT16_4: return 8;
    case QXP_VERTEX_DECLTYPE9_UNUSED: return 0;
    }
    return 0;
}

static DWORD qxp_vdecl9_type_cap(DWORD type)
{
    switch (type) {
    case QXP_VERTEX_DECLTYPE9_UBYTE4:
        return QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4;
    case QXP_VERTEX_DECLTYPE9_UBYTE4N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_UBYTE4N;
    case QXP_VERTEX_DECLTYPE9_SHORT2N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_SHORT2N;
    case QXP_VERTEX_DECLTYPE9_SHORT4N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_SHORT4N;
    case QXP_VERTEX_DECLTYPE9_USHORT2N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_USHORT2N;
    case QXP_VERTEX_DECLTYPE9_USHORT4N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_USHORT4N;
    case QXP_VERTEX_DECLTYPE9_UDEC3:
        return QXP_VERTEX_DECLARATION9_DTCAPS_UDEC3;
    case QXP_VERTEX_DECLTYPE9_DEC3N:
        return QXP_VERTEX_DECLARATION9_DTCAPS_DEC3N;
    case QXP_VERTEX_DECLTYPE9_FLOAT16_2:
        return QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_2;
    case QXP_VERTEX_DECLTYPE9_FLOAT16_4:
        return QXP_VERTEX_DECLARATION9_DTCAPS_FLOAT16_4;
    }
    return 0;
}

static BOOL qxp_vdecl9_is_lookup(DWORD method)
{
    return method == QXP_VERTEX_DECLMETHOD9_LOOKUP ||
           method == QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED;
}

static BOOL qxp_vdecl9_is_partial(DWORD method)
{
    return method == QXP_VERTEX_DECLMETHOD9_PARTIALU ||
           method == QXP_VERTEX_DECLMETHOD9_PARTIALV ||
           method == QXP_VERTEX_DECLMETHOD9_CROSSUV;
}

static BOOL qxp_vdecl9_partial_type(DWORD type)
{
    return type == QXP_VERTEX_DECLTYPE9_D3DCOLOR ||
           type == QXP_VERTEX_DECLTYPE9_FLOAT3 ||
           type == QXP_VERTEX_DECLTYPE9_FLOAT4 ||
           type == QXP_VERTEX_DECLTYPE9_SHORT4 ||
           type == QXP_VERTEX_DECLTYPE9_UBYTE4;
}

static BOOL qxp_vdecl9_partial_usage(DWORD usage)
{
    return usage == QXP_VERTEX_DECLUSAGE9_POSITION ||
           usage == QXP_VERTEX_DECLUSAGE9_NORMAL ||
           usage == QXP_VERTEX_DECLUSAGE9_BLENDWEIGHT ||
           usage == QXP_VERTEX_DECLUSAGE9_TEXCOORD;
}

static BOOL qxp_vdecl9_has_default_reference(
    const QXP_VERTEX_DECLARATION9 *declaration, DWORD element_count,
    const QXP_VERTEX_ELEMENT9 *element)
{
    DWORD index;

    for (index = 0; index < element_count; ++index) {
        const QXP_VERTEX_DECLARATION9_ELEMENT *candidate;

        candidate = &declaration->elements[index];
        if (candidate->method == QXP_VERTEX_DECLMETHOD9_DEFAULT &&
            candidate->stream == element->stream &&
            candidate->offset == element->offset &&
            candidate->type == element->type)
            return TRUE;
    }
    return FALSE;
}

BOOL QxpParseVertexDeclaration9(
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    QXP_VERTEX_DECLARATION9 *declaration,
    QXP_VERTEX_DECLARATION9_ERROR *error)
{
    DWORD index;
    DWORD current_stream;
    DWORD seen_streams;
    DWORD lookup_index;
    DWORD lookup_count;
    DWORD first_nonzero_stream;
    DWORD first_nondefault;
    DWORD first_position;
    DWORD first_default_order;
    BOOL method_requires_tessellation;

    if (error) qxp_vdecl9_zero(error, (DWORD)sizeof(*error));
    if (!declaration) {
        qxp_vdecl9_set_error(error,
                             QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT, 0);
        return FALSE;
    }
    qxp_vdecl9_zero(declaration, (DWORD)sizeof(*declaration));
    if (!error) return FALSE;
    if (!elements)
        return qxp_vdecl9_fail(
            declaration, error,
            QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT, 0);
    if (!element_count ||
        element_count > QXP_VERTEX_DECLARATION9_MAX_ELEMENTS) {
        DWORD failure_index;

        failure_index = element_count >
                        QXP_VERTEX_DECLARATION9_MAX_ELEMENTS ?
                        QXP_VERTEX_DECLARATION9_MAX_ELEMENTS : 0;
        return qxp_vdecl9_fail(
            declaration, error,
            QXP_VERTEX_DECLARATION9_ERROR_ELEMENT_COUNT, failure_index);
    }

    current_stream = QXP_VDECL9_INDEX_NONE;
    seen_streams = 0;
    lookup_index = QXP_VDECL9_INDEX_NONE;
    lookup_count = 0;
    first_nonzero_stream = QXP_VDECL9_INDEX_NONE;
    first_nondefault = QXP_VDECL9_INDEX_NONE;
    first_position = QXP_VDECL9_INDEX_NONE;
    first_default_order = QXP_VDECL9_INDEX_NONE;
    method_requires_tessellation = FALSE;

    for (index = 0; index < element_count; ++index) {
        const QXP_VERTEX_ELEMENT9 *source;
        QXP_VERTEX_DECLARATION9_ELEMENT *destination;
        DWORD stream_bit;
        DWORD semantic_bit;
        DWORD byte_size;
        DWORD element_end;
        DWORD previous;
        BOOL lookup_method;

        source = &elements[index];
        if (source->stream == 0x00ffU)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_SENTINEL, index);
        if ((DWORD)source->stream >=
            QXP_VERTEX_DECLARATION9_STREAM_COUNT)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_STREAM_RANGE, index);
        if ((DWORD)source->type > QXP_VERTEX_DECLTYPE9_UNUSED)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_TYPE, index);
        if ((DWORD)source->method >
            QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_METHOD, index);
        if ((DWORD)source->usage >=
            QXP_VERTEX_DECLARATION9_USAGE_COUNT)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_USAGE_RANGE, index);
        if ((DWORD)source->usage_index >
            QXP_VERTEX_DECLARATION9_MAX_USAGE_INDEX)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_USAGE_INDEX_RANGE, index);
        if (source->offset & 3U)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_OFFSET_ALIGNMENT, index);

        stream_bit = 1UL << source->stream;
        if ((DWORD)source->stream != current_stream) {
            if (seen_streams & stream_bit)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_NONCONSECUTIVE_STREAM,
                    index);
            seen_streams |= stream_bit;
            current_stream = source->stream;
        }
        semantic_bit = 1UL << source->usage_index;
        if ((DWORD)declaration->semantic_masks[source->usage] &
            semantic_bit)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_DUPLICATE_SEMANTIC, index);

        lookup_method = qxp_vdecl9_is_lookup(source->method);
        if ((source->usage == QXP_VERTEX_DECLUSAGE9_SAMPLE) !=
            lookup_method)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, index);
        if (source->type == QXP_VERTEX_DECLTYPE9_UNUSED &&
            source->method != QXP_VERTEX_DECLMETHOD9_UV &&
            source->method !=
                QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_UNUSED_TYPE, index);
        if ((source->method == QXP_VERTEX_DECLMETHOD9_UV ||
             source->method ==
                QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED) &&
            source->type != QXP_VERTEX_DECLTYPE9_UNUSED)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, index);

        byte_size = qxp_vdecl9_type_size(source->type);
        if (byte_size > 0xffffffffUL - (DWORD)source->offset)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_OFFSET_OVERFLOW, index);
        element_end = (DWORD)source->offset + byte_size;

        if (source->method == QXP_VERTEX_DECLMETHOD9_LOOKUP) {
            if (source->type != QXP_VERTEX_DECLTYPE9_FLOAT2 &&
                source->type != QXP_VERTEX_DECLTYPE9_FLOAT3 &&
                source->type != QXP_VERTEX_DECLTYPE9_FLOAT4)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, index);
            if (!qxp_vdecl9_has_default_reference(
                    declaration, index, source))
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, index);
        } else if (source->method ==
                   QXP_VERTEX_DECLMETHOD9_LOOKUPPRESAMPLED) {
            if (source->stream != 0 || source->offset != 0)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET,
                    index);
        } else if (source->method == QXP_VERTEX_DECLMETHOD9_UV) {
            if (source->stream != 0 || source->offset != 0)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_STREAM_OFFSET,
                    index);
            if (source->usage != QXP_VERTEX_DECLUSAGE9_TEXCOORD &&
                source->usage != QXP_VERTEX_DECLUSAGE9_BLENDWEIGHT)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, index);
        } else if (qxp_vdecl9_is_partial(source->method)) {
            if (!qxp_vdecl9_partial_type(source->type))
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_TYPE, index);
            if (!qxp_vdecl9_partial_usage(source->usage))
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_USAGE, index);
            if (!qxp_vdecl9_has_default_reference(
                    declaration, index, source))
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_METHOD_REFERENCE, index);
        }

        if (source->usage == QXP_VERTEX_DECLUSAGE9_TESSFACTOR &&
            (source->type != QXP_VERTEX_DECLTYPE9_FLOAT1 ||
             source->usage_index != 0))
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_TESSFACTOR, index);

        destination = &declaration->elements[index];
        destination->stream = source->stream;
        destination->offset = source->offset;
        destination->type = source->type;
        destination->method = source->method;
        destination->usage = source->usage;
        destination->usage_index = source->usage_index;
        destination->byte_size = byte_size;
        declaration->semantic_masks[source->usage] =
            (WORD)((DWORD)declaration->semantic_masks[source->usage] |
                   semantic_bit);
        declaration->stream_mask |= stream_bit;
        if (element_end > declaration->stream_spans[source->stream])
            declaration->stream_spans[source->stream] = element_end;
        if (element_end > declaration->required_max_stream_stride)
            declaration->required_max_stream_stride = element_end;
        declaration->required_decl_types |=
            qxp_vdecl9_type_cap(source->type);
        if (source->type > QXP_VERTEX_DECLTYPE9_SHORT4 &&
            source->type < QXP_VERTEX_DECLTYPE9_UNUSED) {
            declaration->requires_programmable_vertex_pipeline = TRUE;
            declaration->required_vertex_shader_version =
                QXP_VERTEX_DECLARATION9_VERTEX_SHADER_VERSION_2_0;
        }

        if (source->stream != 0 &&
            first_nonzero_stream == QXP_VDECL9_INDEX_NONE)
            first_nonzero_stream = index;
        if (source->method != QXP_VERTEX_DECLMETHOD9_DEFAULT &&
            first_nondefault == QXP_VDECL9_INDEX_NONE)
            first_nondefault = index;
        if (source->usage == QXP_VERTEX_DECLUSAGE9_POSITION &&
            source->usage_index == 0 &&
            first_position == QXP_VDECL9_INDEX_NONE)
            first_position = index;
        if (source->usage == QXP_VERTEX_DECLUSAGE9_POSITIONT &&
            source->usage_index == 0)
            declaration->has_positiont = TRUE;

        if (source->method == QXP_VERTEX_DECLMETHOD9_DEFAULT) {
            for (previous = index; previous > 0; --previous) {
                const QXP_VERTEX_DECLARATION9_ELEMENT *candidate;
                DWORD candidate_end;

                candidate = &declaration->elements[previous - 1];
                if (candidate->stream != source->stream) break;
                if (candidate->method !=
                    QXP_VERTEX_DECLMETHOD9_DEFAULT) continue;
                candidate_end = (DWORD)candidate->offset +
                                candidate->byte_size;
                if ((DWORD)source->offset < candidate_end) {
                    declaration->required_dev_caps2 |=
                        QXP_VERTEX_DECLARATION9_DEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
                    if (first_default_order == QXP_VDECL9_INDEX_NONE)
                        first_default_order = index;
                }
                break;
            }
        }

        if (lookup_method) {
            ++lookup_count;
            if (lookup_count > 1)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_COUNT, index);
            lookup_index = index;
            declaration->requires_tessellation = TRUE;
            declaration->requires_displacement_mapping = TRUE;
            declaration->requires_programmable_vertex_pipeline = TRUE;
            declaration->required_dev_caps |=
                QXP_VERTEX_DECLARATION9_DEVCAPS_NPATCHES;
            if (source->method == QXP_VERTEX_DECLMETHOD9_LOOKUP)
                declaration->required_dev_caps2 |=
                    QXP_VERTEX_DECLARATION9_DEVCAPS2_DMAPNPATCH;
            else
                declaration->required_dev_caps2 |=
                    QXP_VERTEX_DECLARATION9_DEVCAPS2_PRESAMPLEDDMAPNPATCH;
            method_requires_tessellation = TRUE;
        } else if (source->method !=
                   QXP_VERTEX_DECLMETHOD9_DEFAULT) {
            declaration->requires_tessellation = TRUE;
            declaration->required_dev_caps |=
                QXP_VERTEX_DECLARATION9_DEVCAPS_RTPATCHES;
            method_requires_tessellation = TRUE;
        }
        if (source->usage == QXP_VERTEX_DECLUSAGE9_TESSFACTOR) {
            /* TESSFACTOR is shared by patch paths, so declaration creation
             * cannot attribute an exact RT-patch or N-patch DevCaps bit. */
            declaration->requires_tessellation = TRUE;
        }
    }

    if (lookup_index != QXP_VDECL9_INDEX_NONE) {
        for (index = 0; index < element_count; ++index) {
            if (index != lookup_index &&
                elements[index].method !=
                    QXP_VERTEX_DECLMETHOD9_DEFAULT)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_LOOKUP_MIXED_METHOD,
                    index);
        }
    }

    if (declaration->has_positiont) {
        if (first_nonzero_stream != QXP_VDECL9_INDEX_NONE)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_STREAM,
                first_nonzero_stream);
        if (first_default_order != QXP_VDECL9_INDEX_NONE)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_ORDER,
                first_default_order);
        if (first_nondefault != QXP_VDECL9_INDEX_NONE)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_METHOD,
                first_nondefault);
        if (first_position != QXP_VDECL9_INDEX_NONE)
            return qxp_vdecl9_fail(
                declaration, error,
                QXP_VERTEX_DECLARATION9_ERROR_POSITIONT_POSITION,
                first_position);
    }

    /* Nondefault methods commit the declaration to a patch or N-patch path;
     * D3D9 limits every data-bearing element on that path through SHORT4.
     * A DEFAULT TESSFACTOR semantic records a future tessellation requirement
     * but does not by itself make an ordinary draw structurally impossible. */
    if (method_requires_tessellation) {
        for (index = 0; index < element_count; ++index) {
            if (elements[index].type != QXP_VERTEX_DECLTYPE9_UNUSED &&
                elements[index].type > QXP_VERTEX_DECLTYPE9_SHORT4)
                return qxp_vdecl9_fail(
                    declaration, error,
                    QXP_VERTEX_DECLARATION9_ERROR_TESSELLATION_TYPE,
                    index);
        }
    }

    declaration->element_count = element_count;
    declaration->valid = TRUE;
    return TRUE;
}

static BOOL qxp_vdecl9_memory_equal(const void *left, const void *right,
                                    DWORD byte_count)
{
    const BYTE *left_bytes;
    const BYTE *right_bytes;
    DWORD index;

    left_bytes = (const BYTE *)left;
    right_bytes = (const BYTE *)right;
    for (index = 0; index < byte_count; ++index) {
        if (left_bytes[index] != right_bytes[index]) return FALSE;
    }
    return TRUE;
}

BOOL QxpValidateParsedVertexDeclaration9(
    const QXP_VERTEX_DECLARATION9 *declaration,
    QXP_VERTEX_DECLARATION9_ERROR *error)
{
    QXP_VERTEX_ELEMENT9 elements[QXP_VERTEX_DECLARATION9_MAX_ELEMENTS];
    QXP_VERTEX_DECLARATION9 reparsed;
    QXP_VERTEX_DECLARATION9_ERROR parse_error;
    DWORD index;

    if (error) qxp_vdecl9_zero(error, (DWORD)sizeof(*error));
    if (!error) return FALSE;
    if (!declaration) {
        qxp_vdecl9_set_error(
            error, QXP_VERTEX_DECLARATION9_ERROR_NULL_ARGUMENT, 0);
        return FALSE;
    }
    if (!declaration->valid || !declaration->element_count ||
        declaration->element_count >
            QXP_VERTEX_DECLARATION9_MAX_ELEMENTS) {
        qxp_vdecl9_set_error(
            error, QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE, 0);
        return FALSE;
    }

    qxp_vdecl9_zero(elements, (DWORD)sizeof(elements));
    for (index = 0; index < declaration->element_count; ++index) {
        elements[index].stream = declaration->elements[index].stream;
        elements[index].offset = declaration->elements[index].offset;
        elements[index].type = declaration->elements[index].type;
        elements[index].method = declaration->elements[index].method;
        elements[index].usage = declaration->elements[index].usage;
        elements[index].usage_index =
            declaration->elements[index].usage_index;
    }

    qxp_vdecl9_zero(&reparsed, (DWORD)sizeof(reparsed));
    qxp_vdecl9_zero(&parse_error, (DWORD)sizeof(parse_error));
    if (!QxpParseVertexDeclaration9(
            elements, declaration->element_count,
            &reparsed, &parse_error)) {
        *error = parse_error;
        return FALSE;
    }
    if (qxp_vdecl9_memory_equal(
            declaration, &reparsed, (DWORD)sizeof(reparsed)))
        return TRUE;

    for (index = 0; index < declaration->element_count; ++index) {
        if (!qxp_vdecl9_memory_equal(
                &declaration->elements[index],
                &reparsed.elements[index],
                (DWORD)sizeof(reparsed.elements[index]))) {
            qxp_vdecl9_set_error(
                error, QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE,
                index);
            return FALSE;
        }
    }
    qxp_vdecl9_set_error(
        error, QXP_VERTEX_DECLARATION9_ERROR_PARSED_STATE,
        declaration->element_count);
    return FALSE;
}

/* Exact D3DFVF values, kept private because this parser is also host-tested
 * without importing the mutually exclusive Direct3D public headers. */
#define QXP_VDECL9_FVF_XYZ              0x00000002UL
#define QXP_VDECL9_FVF_XYZRHW           0x00000004UL
#define QXP_VDECL9_FVF_NORMAL           0x00000010UL
#define QXP_VDECL9_FVF_PSIZE            0x00000020UL
#define QXP_VDECL9_FVF_DIFFUSE          0x00000040UL
#define QXP_VDECL9_FVF_SPECULAR         0x00000080UL
#define QXP_VDECL9_FVF_TEXCOUNT_SHIFT   8UL
#define QXP_VDECL9_FVF_TEXFORMAT_SHIFT  16UL
#define QXP_VDECL9_FVF_TEXFORMAT1       3UL
#define QXP_VDECL9_FVF_TEXFORMAT3       1UL
#define QXP_VDECL9_FVF_TEXFORMAT4       2UL
#define QXP_VDECL9_FVF_MAX_TEXCOORDS    8UL

static BOOL qxp_vdecl9_fvf_element_matches(
    const QXP_VERTEX_DECLARATION9 *declaration, DWORD element_index,
    DWORD offset, DWORD type, DWORD usage, DWORD usage_index,
    DWORD byte_size)
{
    const QXP_VERTEX_DECLARATION9_ELEMENT *element;

    if (element_index >= declaration->element_count) return FALSE;
    element = &declaration->elements[element_index];
    return element->stream == 0 && element->offset == offset &&
           element->type == type &&
           element->method == QXP_VERTEX_DECLMETHOD9_DEFAULT &&
           element->usage == usage &&
           element->usage_index == usage_index &&
           element->byte_size == byte_size;
}

BOOL QxpTranslateVertexDeclaration9ToFvf(
    const QXP_VERTEX_DECLARATION9 *declaration,
    DWORD *fvf, DWORD *vertex_stride)
{
    QXP_VERTEX_DECLARATION9_ERROR validation_error;
    DWORD texture_types[QXP_VDECL9_FVF_MAX_TEXCOORDS];
    DWORD texture_mask;
    DWORD texture_count;
    DWORD element_index;
    DWORD index;
    DWORD offset;
    DWORD translated_fvf;
    BOOL has_position;
    BOOL has_positiont;
    BOOL has_normal;
    BOOL has_psize;
    BOOL has_diffuse;
    BOOL has_specular;

    if (fvf) *fvf = 0;
    if (vertex_stride) *vertex_stride = 0;
    if (!declaration || !fvf || !vertex_stride) return FALSE;

    qxp_vdecl9_zero(&validation_error,
                    (DWORD)sizeof(validation_error));
    if (!QxpValidateParsedVertexDeclaration9(
            declaration, &validation_error)) return FALSE;

    for (index = 0; index < QXP_VDECL9_FVF_MAX_TEXCOORDS; ++index)
        texture_types[index] = QXP_VERTEX_DECLTYPE9_UNUSED;
    texture_mask = 0;
    has_position = FALSE;
    has_positiont = FALSE;
    has_normal = FALSE;
    has_psize = FALSE;
    has_diffuse = FALSE;
    has_specular = FALSE;

    for (index = 0; index < declaration->element_count; ++index) {
        const QXP_VERTEX_DECLARATION9_ELEMENT *element;
        DWORD texture_bit;

        element = &declaration->elements[index];
        if (element->stream != 0 ||
            element->method != QXP_VERTEX_DECLMETHOD9_DEFAULT)
            return FALSE;
        switch (element->usage) {
        case QXP_VERTEX_DECLUSAGE9_POSITION:
            if (element->usage_index != 0 ||
                element->type != QXP_VERTEX_DECLTYPE9_FLOAT3 ||
                has_position || has_positiont) return FALSE;
            has_position = TRUE;
            break;
        case QXP_VERTEX_DECLUSAGE9_POSITIONT:
            if (element->usage_index != 0 ||
                element->type != QXP_VERTEX_DECLTYPE9_FLOAT4 ||
                has_position || has_positiont) return FALSE;
            has_positiont = TRUE;
            break;
        case QXP_VERTEX_DECLUSAGE9_NORMAL:
            if (element->usage_index != 0 ||
                element->type != QXP_VERTEX_DECLTYPE9_FLOAT3 ||
                has_normal) return FALSE;
            has_normal = TRUE;
            break;
        case QXP_VERTEX_DECLUSAGE9_PSIZE:
            if (element->usage_index != 0 ||
                element->type != QXP_VERTEX_DECLTYPE9_FLOAT1 ||
                has_psize) return FALSE;
            has_psize = TRUE;
            break;
        case QXP_VERTEX_DECLUSAGE9_COLOR:
            if (element->type != QXP_VERTEX_DECLTYPE9_D3DCOLOR)
                return FALSE;
            if (element->usage_index == 0) {
                if (has_diffuse) return FALSE;
                has_diffuse = TRUE;
            } else if (element->usage_index == 1) {
                if (has_specular) return FALSE;
                has_specular = TRUE;
            } else {
                return FALSE;
            }
            break;
        case QXP_VERTEX_DECLUSAGE9_TEXCOORD:
            if ((DWORD)element->usage_index >=
                    QXP_VDECL9_FVF_MAX_TEXCOORDS ||
                element->type > QXP_VERTEX_DECLTYPE9_FLOAT4)
                return FALSE;
            texture_bit = 1UL << element->usage_index;
            if (texture_mask & texture_bit) return FALSE;
            texture_mask |= texture_bit;
            texture_types[element->usage_index] = element->type;
            break;
        default:
            return FALSE;
        }
    }

    if (has_position == has_positiont ||
        (has_positiont && has_normal)) return FALSE;
    texture_count = 0;
    while (texture_count < QXP_VDECL9_FVF_MAX_TEXCOORDS &&
           (texture_mask & (1UL << texture_count))) ++texture_count;
    if (texture_mask != ((1UL << texture_count) - 1UL)) return FALSE;

    element_index = 0;
    offset = 0;
    if (!qxp_vdecl9_fvf_element_matches(
            declaration, element_index, offset,
            has_position ? QXP_VERTEX_DECLTYPE9_FLOAT3 :
                           QXP_VERTEX_DECLTYPE9_FLOAT4,
            has_position ? QXP_VERTEX_DECLUSAGE9_POSITION :
                           QXP_VERTEX_DECLUSAGE9_POSITIONT,
            0, has_position ? 12UL : 16UL)) return FALSE;
    offset += has_position ? 12UL : 16UL;
    ++element_index;
    if (has_normal) {
        if (!qxp_vdecl9_fvf_element_matches(
                declaration, element_index, offset,
                QXP_VERTEX_DECLTYPE9_FLOAT3,
                QXP_VERTEX_DECLUSAGE9_NORMAL, 0, 12UL)) return FALSE;
        offset += 12UL;
        ++element_index;
    }
    if (has_psize) {
        if (!qxp_vdecl9_fvf_element_matches(
                declaration, element_index, offset,
                QXP_VERTEX_DECLTYPE9_FLOAT1,
                QXP_VERTEX_DECLUSAGE9_PSIZE, 0, 4UL)) return FALSE;
        offset += 4UL;
        ++element_index;
    }
    if (has_diffuse) {
        if (!qxp_vdecl9_fvf_element_matches(
                declaration, element_index, offset,
                QXP_VERTEX_DECLTYPE9_D3DCOLOR,
                QXP_VERTEX_DECLUSAGE9_COLOR, 0, 4UL)) return FALSE;
        offset += 4UL;
        ++element_index;
    }
    if (has_specular) {
        if (!qxp_vdecl9_fvf_element_matches(
                declaration, element_index, offset,
                QXP_VERTEX_DECLTYPE9_D3DCOLOR,
                QXP_VERTEX_DECLUSAGE9_COLOR, 1, 4UL)) return FALSE;
        offset += 4UL;
        ++element_index;
    }
    for (index = 0; index < texture_count; ++index) {
        DWORD byte_size;

        byte_size = (texture_types[index] + 1UL) * sizeof(DWORD);
        if (!qxp_vdecl9_fvf_element_matches(
                declaration, element_index, offset,
                texture_types[index], QXP_VERTEX_DECLUSAGE9_TEXCOORD,
                index, byte_size)) return FALSE;
        offset += byte_size;
        ++element_index;
    }
    if (element_index != declaration->element_count ||
        offset != declaration->stream_spans[0] ||
        offset != declaration->required_max_stream_stride) return FALSE;

    translated_fvf = has_position ? QXP_VDECL9_FVF_XYZ :
                                    QXP_VDECL9_FVF_XYZRHW;
    if (has_normal) translated_fvf |= QXP_VDECL9_FVF_NORMAL;
    if (has_psize) translated_fvf |= QXP_VDECL9_FVF_PSIZE;
    if (has_diffuse) translated_fvf |= QXP_VDECL9_FVF_DIFFUSE;
    if (has_specular) translated_fvf |= QXP_VDECL9_FVF_SPECULAR;
    translated_fvf |= texture_count << QXP_VDECL9_FVF_TEXCOUNT_SHIFT;
    for (index = 0; index < texture_count; ++index) {
        DWORD texture_format;

        texture_format = 0;
        if (texture_types[index] == QXP_VERTEX_DECLTYPE9_FLOAT1)
            texture_format = QXP_VDECL9_FVF_TEXFORMAT1;
        else if (texture_types[index] == QXP_VERTEX_DECLTYPE9_FLOAT3)
            texture_format = QXP_VDECL9_FVF_TEXFORMAT3;
        else if (texture_types[index] == QXP_VERTEX_DECLTYPE9_FLOAT4)
            texture_format = QXP_VDECL9_FVF_TEXFORMAT4;
        translated_fvf |= texture_format <<
            (QXP_VDECL9_FVF_TEXFORMAT_SHIFT + index * 2UL);
    }

    *fvf = translated_fvf;
    *vertex_stride = offset;
    return TRUE;
}

static void qxp_vdecl9_set_fvf_element(
    QXP_VERTEX_ELEMENT9 *element, DWORD offset, DWORD type,
    DWORD usage, DWORD usage_index)
{
    element->stream = 0;
    element->offset = (WORD)offset;
    element->type = (BYTE)type;
    element->method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
    element->usage = (BYTE)usage;
    element->usage_index = (BYTE)usage_index;
}

BOOL QxpBuildVertexDeclaration9FromFvf(
    DWORD fvf, QXP_VERTEX_DECLARATION9 *declaration)
{
    QXP_VERTEX_ELEMENT9 elements[1 + 4 + QXP_VDECL9_FVF_MAX_TEXCOORDS];
    QXP_VERTEX_DECLARATION9_ERROR error;
    DWORD allowed;
    DWORD position;
    DWORD texture_count;
    DWORD texture;
    DWORD texture_format;
    DWORD texture_type;
    DWORD texture_bytes;
    DWORD element_count;
    DWORD offset;
    DWORD translated_fvf;
    DWORD translated_stride;

    if (!declaration) return FALSE;
    qxp_vdecl9_zero(declaration, (DWORD)sizeof(*declaration));
    allowed = 0xffff0000UL | 0x00000f00UL |
              QXP_VDECL9_FVF_SPECULAR | QXP_VDECL9_FVF_DIFFUSE |
              QXP_VDECL9_FVF_PSIZE | QXP_VDECL9_FVF_NORMAL |
              0x0000000eUL;
    if (fvf & ~allowed) return FALSE;
    position = fvf & 0x0000000eUL;
    if ((position != QXP_VDECL9_FVF_XYZ &&
         position != QXP_VDECL9_FVF_XYZRHW) ||
        (position == QXP_VDECL9_FVF_XYZRHW &&
         (fvf & QXP_VDECL9_FVF_NORMAL))) return FALSE;
    texture_count = (fvf >> QXP_VDECL9_FVF_TEXCOUNT_SHIFT) & 0x0fUL;
    if (texture_count > QXP_VDECL9_FVF_MAX_TEXCOORDS) return FALSE;
    for (texture = texture_count;
         texture < QXP_VDECL9_FVF_MAX_TEXCOORDS; ++texture) {
        if ((fvf >> (QXP_VDECL9_FVF_TEXFORMAT_SHIFT +
                     texture * 2UL)) & 3UL) return FALSE;
    }

    qxp_vdecl9_zero(elements, (DWORD)sizeof(elements));
    qxp_vdecl9_zero(&error, (DWORD)sizeof(error));
    element_count = 0;
    offset = 0;
    qxp_vdecl9_set_fvf_element(
        &elements[element_count++], offset,
        position == QXP_VDECL9_FVF_XYZ ?
            QXP_VERTEX_DECLTYPE9_FLOAT3 : QXP_VERTEX_DECLTYPE9_FLOAT4,
        position == QXP_VDECL9_FVF_XYZ ?
            QXP_VERTEX_DECLUSAGE9_POSITION :
            QXP_VERTEX_DECLUSAGE9_POSITIONT,
        0);
    offset += position == QXP_VDECL9_FVF_XYZ ? 12UL : 16UL;
    if (fvf & QXP_VDECL9_FVF_NORMAL) {
        qxp_vdecl9_set_fvf_element(
            &elements[element_count++], offset,
            QXP_VERTEX_DECLTYPE9_FLOAT3,
            QXP_VERTEX_DECLUSAGE9_NORMAL, 0);
        offset += 12UL;
    }
    if (fvf & QXP_VDECL9_FVF_PSIZE) {
        qxp_vdecl9_set_fvf_element(
            &elements[element_count++], offset,
            QXP_VERTEX_DECLTYPE9_FLOAT1,
            QXP_VERTEX_DECLUSAGE9_PSIZE, 0);
        offset += 4UL;
    }
    if (fvf & QXP_VDECL9_FVF_DIFFUSE) {
        qxp_vdecl9_set_fvf_element(
            &elements[element_count++], offset,
            QXP_VERTEX_DECLTYPE9_D3DCOLOR,
            QXP_VERTEX_DECLUSAGE9_COLOR, 0);
        offset += 4UL;
    }
    if (fvf & QXP_VDECL9_FVF_SPECULAR) {
        qxp_vdecl9_set_fvf_element(
            &elements[element_count++], offset,
            QXP_VERTEX_DECLTYPE9_D3DCOLOR,
            QXP_VERTEX_DECLUSAGE9_COLOR, 1);
        offset += 4UL;
    }
    for (texture = 0; texture < texture_count; ++texture) {
        texture_format =
            (fvf >> (QXP_VDECL9_FVF_TEXFORMAT_SHIFT +
                     texture * 2UL)) & 3UL;
        texture_type = QXP_VERTEX_DECLTYPE9_FLOAT2;
        if (texture_format == QXP_VDECL9_FVF_TEXFORMAT1)
            texture_type = QXP_VERTEX_DECLTYPE9_FLOAT1;
        else if (texture_format == QXP_VDECL9_FVF_TEXFORMAT3)
            texture_type = QXP_VERTEX_DECLTYPE9_FLOAT3;
        else if (texture_format == QXP_VDECL9_FVF_TEXFORMAT4)
            texture_type = QXP_VERTEX_DECLTYPE9_FLOAT4;
        texture_bytes = (texture_type + 1UL) * sizeof(DWORD);
        qxp_vdecl9_set_fvf_element(
            &elements[element_count++], offset, texture_type,
            QXP_VERTEX_DECLUSAGE9_TEXCOORD, texture);
        offset += texture_bytes;
    }
    if (!QxpParseVertexDeclaration9(
            elements, element_count, declaration, &error)) return FALSE;
    translated_fvf = 0;
    translated_stride = 0;
    if (!QxpTranslateVertexDeclaration9ToFvf(
            declaration, &translated_fvf, &translated_stride) ||
        translated_fvf != fvf || translated_stride != offset) {
        qxp_vdecl9_zero(declaration, (DWORD)sizeof(*declaration));
        return FALSE;
    }
    return TRUE;
}
