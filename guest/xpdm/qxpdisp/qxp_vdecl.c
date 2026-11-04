#include <windef.h>

#define DIRECT3D_VERSION 0x0800
#include <d3d8types.h>

#include "qxp_vdecl.h"

static void qxp_vdecl_copy(BYTE *destination, const BYTE *source, DWORD bytes)
{
    DWORD index;

    for (index = 0; index < bytes; ++index)
        destination[index] = source[index];
}

static void qxp_vdecl_reset(QXP_VERTEX_DECLARATION *declaration)
{
    DWORD index;
    DWORD component;

    declaration->byte_stride = 0;
    declaration->input_mask = 0;
    declaration->element_count = 0;
    declaration->stream_mask = 0;
    for (index = 0; index < QXP_VERTEX_DECLARATION_STREAM_COUNT; ++index)
        declaration->stream_spans[index] = 0;
    for (index = 0; index < QXP_VERTEX_DECLARATION_INPUT_COUNT; ++index) {
        declaration->elements[index].input_register = 0;
        declaration->elements[index].data_type = 0;
        declaration->elements[index].byte_offset = 0;
        declaration->elements[index].byte_count = 0;
        declaration->elements[index].source_stream = 0;
        declaration->elements[index].source_byte_offset = 0;
    }
    for (index = 0; index < 3; ++index)
        declaration->defined_constants[index] = 0;
    for (index = 0; index < QXP_VERTEX_DECLARATION_CONSTANT_COUNT; ++index) {
        for (component = 0; component < 4; ++component)
            declaration->constant_values[index][component] = 0;
    }
}

static DWORD qxp_vdecl_type_bytes(DWORD data_type)
{
    switch (data_type) {
    case D3DVSDT_FLOAT1: return 4;
    case D3DVSDT_FLOAT2: return 8;
    case D3DVSDT_FLOAT3: return 12;
    case D3DVSDT_FLOAT4: return 16;
    case D3DVSDT_D3DCOLOR: return 4;
    case D3DVSDT_UBYTE4: return 4;
    case D3DVSDT_SHORT2: return 4;
    case D3DVSDT_SHORT4: return 8;
    }
    return 0;
}

BOOL QxpParseVertexDeclaration8(
    const DWORD *tokens, DWORD token_bytes,
    QXP_VERTEX_DECLARATION *declaration)
{
    const DWORD *cursor;
    const DWORD *end;
    BOOL stream_open = FALSE;
    DWORD current_stream = 0;
    DWORD source_offset = 0;

    if (!tokens || !declaration || token_bytes < sizeof(DWORD) ||
        (token_bytes & 3)) return FALSE;
    qxp_vdecl_reset(declaration);
    cursor = tokens;
    end = tokens + token_bytes / sizeof(DWORD);
    while (cursor < end) {
        DWORD token = *cursor++;
        DWORD token_type;

        if (token == 0xffffffffUL) {
            /* END is valid as the final token; a trailing empty or skip-only
             * stream selector is permitted. */
            return cursor == end;
        }
        token_type = (token & D3DVSD_TOKENTYPEMASK) >>
                     D3DVSD_TOKENTYPESHIFT;
        if (token_type == D3DVSD_TOKEN_NOP) {
            if (token != D3DVSD_NOP()) return FALSE;
            continue;
        }
        if (token_type == D3DVSD_TOKEN_STREAM) {
            DWORD allowed = (DWORD)(D3DVSD_TOKENTYPEMASK |
                                    D3DVSD_STREAMNUMBERMASK);
            DWORD stream = (token & D3DVSD_STREAMNUMBERMASK) >>
                           D3DVSD_STREAMNUMBERSHIFT;

            if ((token & ~allowed) ||
                stream >= QXP_VERTEX_DECLARATION_STREAM_COUNT) return FALSE;
            /* A new stream selector restarts that stream's source cursor.
             * Streams may be empty, repeated, or skip-only. */
            current_stream = stream;
            source_offset = 0;
            stream_open = TRUE;
            continue;
        }
        if (token_type == D3DVSD_TOKEN_STREAMDATA) {
            BOOL skip = (token & D3DVSD_DATALOADTYPEMASK) != 0;
            DWORD source_bytes;

            if (!stream_open) return FALSE;
            if (skip) {
                DWORD allowed = (DWORD)(D3DVSD_TOKENTYPEMASK |
                                        D3DVSD_DATALOADTYPEMASK |
                                        D3DVSD_SKIPCOUNTMASK);
                DWORD dwords;

                if (token & ~allowed) return FALSE;
                dwords = (token & D3DVSD_SKIPCOUNTMASK) >>
                         D3DVSD_SKIPCOUNTSHIFT;
                /* A skip consumes source bytes but emits no packed output. */
                source_bytes = dwords * sizeof(DWORD);
            } else {
                DWORD allowed = (DWORD)(D3DVSD_TOKENTYPEMASK |
                                        D3DVSD_DATATYPEMASK |
                                        D3DVSD_VERTEXREGMASK);
                DWORD input_register;
                DWORD data_type;
                DWORD byte_count;
                QXP_VERTEX_DECLARATION_ELEMENT *element;

                if (token & ~allowed) return FALSE;
                input_register = (token & D3DVSD_VERTEXREGMASK) >>
                                 D3DVSD_VERTEXREGSHIFT;
                data_type = (token & D3DVSD_DATATYPEMASK) >>
                            D3DVSD_DATATYPESHIFT;
                byte_count = qxp_vdecl_type_bytes(data_type);
                if (!byte_count ||
                    input_register >= QXP_VERTEX_DECLARATION_INPUT_COUNT ||
                    (declaration->input_mask &
                     (1UL << input_register)) ||
                    declaration->element_count >=
                        QXP_VERTEX_DECLARATION_INPUT_COUNT) return FALSE;
                if (byte_count > QXP_VERTEX_DECLARATION_MAX_PACKED_STRIDE -
                                 declaration->byte_stride) return FALSE;
                element = &declaration->elements[
                    declaration->element_count++];
                element->input_register = input_register;
                element->data_type = data_type;
                element->byte_count = byte_count;
                element->source_stream = current_stream;
                element->source_byte_offset = source_offset;
                element->byte_offset = declaration->byte_stride;
                declaration->input_mask |= 1UL << input_register;
                declaration->stream_mask |= 1UL << current_stream;
                declaration->byte_stride += byte_count;
                source_bytes = byte_count;
            }
            if (source_bytes > QXP_VERTEX_DECLARATION_MAX_STRIDE -
                               source_offset) return FALSE;
            source_offset += source_bytes;
            if (source_offset > declaration->stream_spans[current_stream])
                declaration->stream_spans[current_stream] = source_offset;
            continue;
        }
        if (token_type == D3DVSD_TOKEN_CONSTMEM) {
            DWORD allowed = (DWORD)(D3DVSD_TOKENTYPEMASK |
                                    D3DVSD_CONSTCOUNTMASK |
                                    D3DVSD_CONSTADDRESSMASK);
            DWORD count;
            DWORD address;
            DWORD constant;
            DWORD component;

            if (token & ~allowed) return FALSE;
            count = (token & D3DVSD_CONSTCOUNTMASK) >>
                    D3DVSD_CONSTCOUNTSHIFT;
            address = (token & D3DVSD_CONSTADDRESSMASK) >>
                      D3DVSD_CONSTADDRESSSHIFT;
            if (!count || address >= QXP_VERTEX_DECLARATION_CONSTANT_COUNT ||
                count > QXP_VERTEX_DECLARATION_CONSTANT_COUNT - address ||
                (DWORD)(end - cursor) < count * 4) return FALSE;
            for (constant = 0; constant < count; ++constant) {
                DWORD destination = address + constant;

                declaration->defined_constants[destination / 32] |=
                    1UL << (destination & 31);
                for (component = 0; component < 4; ++component)
                    declaration->constant_values[destination][component] =
                        *cursor++;
            }
            continue;
        }
        /* Tessellator and extension tokens remain invalid while their D3D8
         * capabilities are zero. A noncanonical END token is invalid too. */
        return FALSE;
    }
    return FALSE;
}

BOOL QxpTranslateFixedVertexDeclaration8(
    QXP_VERTEX_DECLARATION *declaration, DWORD *fvf)
{
    DWORD element_by_register[QXP_VERTEX_DECLARATION_INPUT_COUNT];
    DWORD allowed_mask;
    DWORD translated_fvf;
    DWORD texture_count;
    DWORD texture;
    DWORD dimension;
    DWORD format;
    DWORD offset;
    DWORD index;

    if (fvf) *fvf = 0;
    if (!declaration || !fvf || !declaration->element_count ||
        declaration->element_count > QXP_VERTEX_DECLARATION_INPUT_COUNT)
        return FALSE;
    for (index = 0; index < QXP_VERTEX_DECLARATION_INPUT_COUNT; ++index)
        element_by_register[index] = 0xffffffffUL;
    for (index = 0; index < declaration->element_count; ++index) {
        DWORD input = declaration->elements[index].input_register;

        if (input >= QXP_VERTEX_DECLARATION_INPUT_COUNT ||
            element_by_register[input] != 0xffffffffUL) return FALSE;
        element_by_register[input] = index;
    }
    for (index = 0; index < 3; ++index) {
        if (declaration->defined_constants[index]) return FALSE;
    }
    if (element_by_register[0] == 0xffffffffUL ||
        declaration->elements[element_by_register[0]].source_stream != 0 ||
        declaration->elements[element_by_register[0]].data_type !=
            D3DVSDT_FLOAT3 ||
        element_by_register[1] != 0xffffffffUL ||
        element_by_register[2] != 0xffffffffUL ||
        element_by_register[15] != 0xffffffffUL) return FALSE;
    if (element_by_register[3] != 0xffffffffUL &&
        declaration->elements[element_by_register[3]].data_type !=
            D3DVSDT_FLOAT3) return FALSE;
    if (element_by_register[4] != 0xffffffffUL &&
        declaration->elements[element_by_register[4]].data_type !=
            D3DVSDT_FLOAT1) return FALSE;
    if ((element_by_register[5] != 0xffffffffUL &&
         declaration->elements[element_by_register[5]].data_type !=
            D3DVSDT_D3DCOLOR) ||
        (element_by_register[6] != 0xffffffffUL &&
         declaration->elements[element_by_register[6]].data_type !=
            D3DVSDT_D3DCOLOR)) return FALSE;

    texture_count = 0;
    while (texture_count < 4 &&
           element_by_register[7 + texture_count] != 0xffffffffUL)
        ++texture_count;
    for (texture = texture_count; texture < 8; ++texture) {
        if (element_by_register[7 + texture] != 0xffffffffUL) return FALSE;
    }
    translated_fvf = 0x00000002UL;
    allowed_mask = 1UL;
    offset = 12;
    declaration->elements[element_by_register[0]].byte_offset = 0;
    if (element_by_register[3] != 0xffffffffUL) {
        translated_fvf |= 0x00000010UL;
        allowed_mask |= 1UL << 3;
        declaration->elements[element_by_register[3]].byte_offset = offset;
        offset += 12;
    }
    if (element_by_register[4] != 0xffffffffUL) {
        translated_fvf |= 0x00000020UL;
        allowed_mask |= 1UL << 4;
        declaration->elements[element_by_register[4]].byte_offset = offset;
        offset += 4;
    }
    if (element_by_register[5] != 0xffffffffUL) {
        translated_fvf |= 0x00000040UL;
        allowed_mask |= 1UL << 5;
        declaration->elements[element_by_register[5]].byte_offset = offset;
        offset += 4;
    }
    if (element_by_register[6] != 0xffffffffUL) {
        translated_fvf |= 0x00000080UL;
        allowed_mask |= 1UL << 6;
        declaration->elements[element_by_register[6]].byte_offset = offset;
        offset += 4;
    }
    translated_fvf |= texture_count << 8;
    for (texture = 0; texture < texture_count; ++texture) {
        index = element_by_register[7 + texture];
        format = declaration->elements[index].data_type;
        if (format > D3DVSDT_FLOAT4) return FALSE;
        dimension = format - D3DVSDT_FLOAT1 + 1;
        if (dimension == 1) format = 3;
        else if (dimension == 2) format = 0;
        else if (dimension == 3) format = 1;
        else format = 2;
        translated_fvf |= format << (16 + texture * 2);
        allowed_mask |= 1UL << (7 + texture);
        declaration->elements[index].byte_offset = offset;
        offset += dimension * sizeof(DWORD);
    }
    if (declaration->input_mask != allowed_mask ||
        offset > QXP_VERTEX_DECLARATION_MAX_PACKED_STRIDE) return FALSE;
    declaration->byte_stride = offset;
    *fvf = translated_fvf;
    return TRUE;
}

static BOOL qxp_vdecl_gather(
    const QXP_VERTEX_DECLARATION *declaration,
    const QXP_VERTEX_STREAM_VIEW *views,
    DWORD vertex_index,
    BYTE *destination, DWORD destination_bytes,
    BOOL canonicalize_color)
{
    DWORD index;

    if (!declaration || !views || !destination ||
        declaration->element_count > QXP_VERTEX_DECLARATION_INPUT_COUNT)
        return FALSE;
    if (!declaration->byte_stride) return TRUE;
    if (destination_bytes < declaration->byte_stride) return FALSE;

    for (index = 0; index < declaration->element_count; ++index) {
        const QXP_VERTEX_DECLARATION_ELEMENT *element =
            &declaration->elements[index];
        DWORD expected = qxp_vdecl_type_bytes(element->data_type);
        const QXP_VERTEX_STREAM_VIEW *view;
        ULONGLONG source_start;
        BYTE *dest;

        if (!expected || element->byte_count != expected ||
            element->source_stream >=
                QXP_VERTEX_DECLARATION_STREAM_COUNT ||
            element->byte_offset > declaration->byte_stride -
                element->byte_count) return FALSE;
        view = &views[element->source_stream];
        if (!view->base || !view->stride || !view->bytes) return FALSE;
        source_start = (ULONGLONG)vertex_index * view->stride +
                       element->source_byte_offset;
        dest = destination + element->byte_offset;
        if (view->wraps) {
            DWORD wrapped;
            DWORD first;

            if (element->byte_count > view->bytes) return FALSE;
            wrapped = (DWORD)(source_start % view->bytes);
            first = view->bytes - wrapped;
            if (first >= element->byte_count) {
                qxp_vdecl_copy(dest, view->base + wrapped,
                               element->byte_count);
            } else {
                qxp_vdecl_copy(dest, view->base + wrapped, first);
                qxp_vdecl_copy(dest + first, view->base,
                               element->byte_count - first);
            }
        } else {
            if (source_start + element->byte_count > view->bytes)
                return FALSE;
            qxp_vdecl_copy(dest, view->base + (DWORD)source_start,
                           element->byte_count);
        }
    }
    if (canonicalize_color) {
        for (index = 0; index < declaration->element_count; ++index) {
            const QXP_VERTEX_DECLARATION_ELEMENT *element =
                &declaration->elements[index];

            if (element->data_type == D3DVSDT_D3DCOLOR) {
                BYTE value = destination[element->byte_offset];

                destination[element->byte_offset] =
                    destination[element->byte_offset + 2];
                destination[element->byte_offset + 2] = value;
            }
        }
    }
    return TRUE;
}

BOOL QxpGatherVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const QXP_VERTEX_STREAM_VIEW *views,
    DWORD vertex_index,
    BYTE *destination, DWORD destination_bytes)
{
    return qxp_vdecl_gather(
        declaration, views, vertex_index, destination, destination_bytes,
        TRUE);
}

BOOL QxpGatherFixedVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const QXP_VERTEX_STREAM_VIEW *views,
    DWORD vertex_index,
    BYTE *destination, DWORD destination_bytes)
{
    return qxp_vdecl_gather(
        declaration, views, vertex_index, destination, destination_bytes,
        FALSE);
}

BOOL QxpRepackVertexDeclaration8(
    const QXP_VERTEX_DECLARATION *declaration,
    const BYTE *source, DWORD source_stride,
    BYTE *destination, DWORD destination_bytes)
{
    QXP_VERTEX_STREAM_VIEW views[QXP_VERTEX_DECLARATION_STREAM_COUNT];
    DWORD index;

    if (!declaration) return FALSE;
    /* Single-stream only: a declaration touching streams 1..15 needs the
     * multi-stream gather with per-stream sources. */
    if (declaration->stream_mask & ~1UL) return FALSE;
    if (!declaration->byte_stride) return TRUE;
    if (!source || !source_stride ||
        source_stride < declaration->stream_spans[0]) return FALSE;
    for (index = 0; index < QXP_VERTEX_DECLARATION_STREAM_COUNT; ++index) {
        views[index].base = NULL;
        views[index].bytes = 0;
        views[index].stride = 0;
        views[index].wraps = FALSE;
    }
    views[0].base = source;
    views[0].bytes = source_stride;
    views[0].stride = source_stride;
    views[0].wraps = FALSE;
    return QxpGatherVertexDeclaration8(declaration, views, 0,
                                       destination, destination_bytes);
}
