#ifndef QXP_FLOAT_H
#define QXP_FLOAT_H

BOOLEAN qxp_float_bits_finite(ULONG value);
BOOLEAN qxp_float_from_uint(ULONG value, PULONG result);
BOOLEAN qxp_float_multiply(ULONG left, ULONG right, PULONG result);
BOOLEAN qxp_float_add(ULONG left, ULONG right, PULONG result);
BOOLEAN qxp_float_divide(ULONG numerator, ULONG denominator, PULONG result);
BOOLEAN qxp_float_transform_depth_columns(
    const ULONG world[16], const ULONG view[16],
    const ULONG projection[16], ULONG columns[8]);
BOOLEAN qxp_float_project_depth(const ULONG position[3],
                                const ULONG columns[8], PULONG depth);

#endif
