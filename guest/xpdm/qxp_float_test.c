#include <stdio.h>
#include <string.h>
#include <windef.h>

#include "qxpmp/qxp_float.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "qxp_float_test: %s\n", message);
    return 0;
}

static void identity(ULONG matrix[16])
{
    ULONG index;

    memset(matrix, 0, 16 * sizeof(matrix[0]));
    for (index = 0; index < 4; ++index)
        matrix[index * 4 + index] = 0x3f800000UL;
}

int main(void)
{
    ULONG world[16];
    ULONG view[16];
    ULONG projection[16];
    ULONG columns[8];
    ULONG position[3];
    ULONG value;

    if (!require(qxp_float_from_uint(480, &value) &&
                 value == 0x43f00000UL, "viewport height to float") ||
        !require(qxp_float_from_uint(0xffffffffUL, &value) &&
                 value == 0x4f800000UL, "rounded maximum uint") ||
        !require(qxp_float_add(0x3e800000UL, 0x3e800000UL, &value) &&
                 value == 0x3f000000UL, "quarter plus quarter") ||
        !require(qxp_float_add(0x3f800000UL, 0xbf000000UL, &value) &&
                 value == 0x3f000000UL, "signed addition") ||
        !require(qxp_float_add(0x3f800000UL, 0xbf800000UL, &value) &&
                 value == 0, "exact cancellation") ||
        !require(qxp_float_divide(0x3f000000UL, 0x40000000UL, &value) &&
                 value == 0x3e800000UL, "half divided by two") ||
        !require(qxp_float_multiply(0x40400000UL, 0x3f000000UL, &value) &&
                 value == 0x3fc00000UL, "three times half")) return 1;

    identity(world);
    identity(view);
    identity(projection);
    position[0] = position[1] = 0;
    position[2] = 0x3e800000UL;
    if (!require(qxp_float_transform_depth_columns(
                     world, view, projection, columns),
                 "identity depth columns") ||
        !require(qxp_float_project_depth(position, columns, &value) &&
                 value == 0x3e800000UL, "identity depth")) return 1;

    projection[10] = 0x40000000UL;
    if (!require(qxp_float_transform_depth_columns(
                     world, view, projection, columns),
                 "scaled depth columns") ||
        !require(qxp_float_project_depth(position, columns, &value) &&
                 value == 0x3f000000UL, "postprojection depth")) return 1;

    identity(projection);
    projection[0] = projection[5] = projection[15] = 0x40000000UL;
    position[2] = 0x3f000000UL;
    if (!require(qxp_float_transform_depth_columns(
                     world, view, projection, columns),
                 "divide depth columns") ||
        !require(qxp_float_project_depth(position, columns, &value) &&
                 value == 0x3e800000UL, "homogeneous divide depth")) return 1;

    identity(projection);
    world[14] = 0x3e800000UL;
    view[14] = 0x3e800000UL;
    position[2] = 0x3e800000UL;
    if (!require(qxp_float_transform_depth_columns(
                     world, view, projection, columns),
                 "translated depth columns") ||
        !require(qxp_float_project_depth(position, columns, &value) &&
                 value == 0x3f400000UL, "world-view matrix order")) return 1;

    puts("qxp_float_test: ok");
    return 0;
}
