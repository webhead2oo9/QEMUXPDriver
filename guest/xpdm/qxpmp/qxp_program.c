#ifdef QXP_PROGRAM_STANDALONE
#include <windef.h>
#else
#include "qxpmp.h"
#endif

#include "qxp_program.h"

BOOLEAN qxp_program_attribute_format(
    ULONG format, QXP_PROGRAM_ATTRIBUTE_FORMAT *result)
{
    if (!result) return FALSE;
    result->normalized = FALSE;
    switch (format) {
    case QXP_D3D_VERTEX_FORMAT_FLOAT1:
        result->components = 1; result->storage = QXP_PROGRAM_STORAGE_FLOAT;
        result->bytes = 4; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_FLOAT2:
        result->components = 2; result->storage = QXP_PROGRAM_STORAGE_FLOAT;
        result->bytes = 8; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_FLOAT3:
        result->components = 3; result->storage = QXP_PROGRAM_STORAGE_FLOAT;
        result->bytes = 12; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_FLOAT4:
        result->components = 4; result->storage = QXP_PROGRAM_STORAGE_FLOAT;
        result->bytes = 16; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_D3DCOLOR:
        result->components = 4; result->storage = QXP_PROGRAM_STORAGE_UBYTE;
        result->normalized = TRUE; result->bytes = 4; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_UBYTE4:
        result->components = 4; result->storage = QXP_PROGRAM_STORAGE_UBYTE;
        result->bytes = 4; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_SHORT2:
        result->components = 2; result->storage = QXP_PROGRAM_STORAGE_SHORT;
        result->bytes = 4; return TRUE;
    case QXP_D3D_VERTEX_FORMAT_SHORT4:
        result->components = 4; result->storage = QXP_PROGRAM_STORAGE_SHORT;
        result->bytes = 8; return TRUE;
    }
    return FALSE;
}

BOOLEAN qxp_program_attribute_at_order(ULONG order, PULONG attribute)
{
    static const UCHAR push_order[16] = {
        3, 2, 8, 9, 10, 11, 12, 13, 14, 15,
        0, 4, 5, 1, 6, 7
    };

    if (!attribute || order >= 16) return FALSE;
    *attribute = push_order[order];
    return TRUE;
}

BOOLEAN qxp_program_copy_attribute(
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    const UCHAR *vertex_data, ULONG vertex_count, ULONG attribute,
    UCHAR *destination, ULONG destination_bytes)
{
    QXP_PROGRAM_ATTRIBUTE_FORMAT format;
    const QXP_D3D_PROGRAM_VERTEX_ELEMENT *element;
    ULONG required;
    ULONG vertex;
    ULONG byte;

    if (!program || !vertex_data || !destination || attribute >= 16 ||
        !(program->input_mask & (1UL << attribute)) ||
        !program->vertex_stride ||
        !qxp_program_attribute_format(
            program->elements[attribute].format, &format)) return FALSE;
    element = &program->elements[attribute];
    if (element->byte_offset > program->vertex_stride ||
        format.bytes > program->vertex_stride - element->byte_offset ||
        vertex_count > 0xffffffffUL / format.bytes) return FALSE;
    required = vertex_count * format.bytes;
    if (destination_bytes != required) return FALSE;
    for (vertex = 0; vertex < vertex_count; ++vertex) {
        for (byte = 0; byte < format.bytes; ++byte) {
            destination[vertex * format.bytes + byte] =
                vertex_data[vertex * program->vertex_stride +
                            element->byte_offset + byte];
        }
    }
    return TRUE;
}
