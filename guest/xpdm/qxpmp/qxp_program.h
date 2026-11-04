#ifndef QXP_PROGRAM_H
#define QXP_PROGRAM_H

#include "../qxp_d3d_transport.h"

#define QXP_PROGRAM_STORAGE_FLOAT 1UL
#define QXP_PROGRAM_STORAGE_UBYTE 2UL
#define QXP_PROGRAM_STORAGE_SHORT 3UL

typedef struct qxp_program_attribute_format {
    ULONG components;
    ULONG storage;
    ULONG normalized;
    ULONG bytes;
} QXP_PROGRAM_ATTRIBUTE_FORMAT;

BOOLEAN qxp_program_attribute_format(
    ULONG format, QXP_PROGRAM_ATTRIBUTE_FORMAT *result);
BOOLEAN qxp_program_attribute_at_order(ULONG order, PULONG attribute);
BOOLEAN qxp_program_copy_attribute(
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    const UCHAR *vertex_data, ULONG vertex_count, ULONG attribute,
    UCHAR *destination, ULONG destination_bytes);

#endif
