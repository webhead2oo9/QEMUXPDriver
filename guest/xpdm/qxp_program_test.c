#include <stdio.h>
#include <string.h>
#include <windef.h>

#include "qxpmp/qxp_program.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "qxp_program_test: %s\n", message);
    return 0;
}

int main(void)
{
    static const struct {
        ULONG format;
        ULONG components;
        ULONG storage;
        ULONG normalized;
        ULONG bytes;
    } expected_formats[] = {
        { QXP_D3D_VERTEX_FORMAT_FLOAT1, 1, QXP_PROGRAM_STORAGE_FLOAT, 0, 4 },
        { QXP_D3D_VERTEX_FORMAT_FLOAT2, 2, QXP_PROGRAM_STORAGE_FLOAT, 0, 8 },
        { QXP_D3D_VERTEX_FORMAT_FLOAT3, 3, QXP_PROGRAM_STORAGE_FLOAT, 0, 12 },
        { QXP_D3D_VERTEX_FORMAT_FLOAT4, 4, QXP_PROGRAM_STORAGE_FLOAT, 0, 16 },
        { QXP_D3D_VERTEX_FORMAT_D3DCOLOR, 4, QXP_PROGRAM_STORAGE_UBYTE, 1, 4 },
        { QXP_D3D_VERTEX_FORMAT_UBYTE4, 4, QXP_PROGRAM_STORAGE_UBYTE, 0, 4 },
        { QXP_D3D_VERTEX_FORMAT_SHORT2, 2, QXP_PROGRAM_STORAGE_SHORT, 0, 4 },
        { QXP_D3D_VERTEX_FORMAT_SHORT4, 4, QXP_PROGRAM_STORAGE_SHORT, 0, 8 }
    };
    static const UCHAR expected_order[16] = {
        3, 2, 8, 9, 10, 11, 12, 13, 14, 15,
        0, 4, 5, 1, 6, 7
    };
    QXP_D3D_PROGRAM_VERTEX_PAYLOAD program;
    QXP_PROGRAM_ATTRIBUTE_FORMAT format;
    UCHAR vertices[56];
    UCHAR output[24];
    ULONG attribute;
    ULONG index;

    memset(&program, 0, sizeof(program));
    memset(vertices, 0xee, sizeof(vertices));
    program.input_mask = (1UL << 0) | (1UL << 3) | (1UL << 7);
    program.vertex_stride = 28;
    program.elements[0].format = QXP_D3D_VERTEX_FORMAT_FLOAT3;
    program.elements[0].byte_offset = 0;
    program.elements[3].format = QXP_D3D_VERTEX_FORMAT_D3DCOLOR;
    program.elements[3].byte_offset = 16;
    program.elements[7].format = QXP_D3D_VERTEX_FORMAT_SHORT4;
    program.elements[7].byte_offset = 20;
    for (index = 0; index < 12; ++index) {
        vertices[index] = (UCHAR)(0x10 + index);
        vertices[28 + index] = (UCHAR)(0x40 + index);
    }
    vertices[16] = 0x11; vertices[17] = 0x22;
    vertices[18] = 0x33; vertices[19] = 0x44;
    vertices[44] = 0xaa; vertices[45] = 0xbb;
    vertices[46] = 0xcc; vertices[47] = 0xdd;
    for (index = 0; index < 8; ++index) {
        vertices[20 + index] = (UCHAR)(0x70 + index);
        vertices[48 + index] = (UCHAR)(0x90 + index);
    }

    for (index = 0;
         index < sizeof(expected_formats) / sizeof(expected_formats[0]);
         ++index) {
        if (!require(qxp_program_attribute_format(
                         expected_formats[index].format, &format) &&
                     format.components == expected_formats[index].components &&
                     format.storage == expected_formats[index].storage &&
                     format.normalized == expected_formats[index].normalized &&
                     format.bytes == expected_formats[index].bytes,
                     "attribute format mismatch")) return 1;
    }
    if (!require(!qxp_program_attribute_format(8, &format),
                 "accepted unknown attribute format")) return 1;
    for (index = 0; index < 16; ++index) {
        if (!require(qxp_program_attribute_at_order(index, &attribute) &&
                     attribute == expected_order[index],
                     "Mesa push order mismatch")) return 1;
    }
    if (!require(!qxp_program_attribute_at_order(16, &attribute),
                 "accepted out-of-range push order")) return 1;

    memset(output, 0, sizeof(output));
    if (!require(qxp_program_copy_attribute(
                     &program, vertices, 2, 0, output, 24),
                 "failed FLOAT3 copy") ||
        !require(!memcmp(output, vertices, 12) &&
                 !memcmp(output + 12, vertices + 28, 12),
                 "FLOAT3 deinterleave mismatch")) return 1;
    memset(output, 0, sizeof(output));
    if (!require(qxp_program_copy_attribute(
                     &program, vertices, 2, 3, output, 8),
                 "failed D3DCOLOR copy") ||
        !require(!memcmp(output, vertices + 16, 4) &&
                 !memcmp(output + 4, vertices + 44, 4),
                 "D3DCOLOR deinterleave mismatch")) return 1;
    memset(output, 0, sizeof(output));
    if (!require(qxp_program_copy_attribute(
                     &program, vertices, 2, 7, output, 16),
                 "failed SHORT4 copy") ||
        !require(!memcmp(output, vertices + 20, 8) &&
                 !memcmp(output + 8, vertices + 48, 8),
                 "SHORT4 deinterleave mismatch")) return 1;
    if (!require(!qxp_program_copy_attribute(
                     &program, vertices, 2, 7, output, 15),
                 "accepted undersized output") ||
        !require(!qxp_program_copy_attribute(
                     &program, vertices, 2, 6, output, 8),
                 "accepted disabled attribute")) return 1;
    program.elements[7].byte_offset = 24;
    if (!require(!qxp_program_copy_attribute(
                     &program, vertices, 2, 7, output, 16),
                 "accepted stride-overrun element")) return 1;

    puts("qxp_program_test: ok");
    return 0;
}
