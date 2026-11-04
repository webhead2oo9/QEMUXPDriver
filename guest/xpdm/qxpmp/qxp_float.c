#ifdef QXP_FLOAT_STANDALONE
#include <windef.h>
#else
#include "qxpmp.h"
#endif

#include "qxp_float.h"

#define QXP_FLOAT_ABSOLUTE_MASK 0x7fffffffUL
#define QXP_FLOAT_EXPONENT_MASK 0x7f800000UL
#define QXP_FLOAT_FRACTION_MASK 0x007fffffUL
#define QXP_FLOAT_HIDDEN_BIT 0x00800000UL
#define QXP_FLOAT_MAX_FINITE 0x7f7fffffUL

BOOLEAN qxp_float_bits_finite(ULONG value)
{
    return (value & QXP_FLOAT_EXPONENT_MASK) != QXP_FLOAT_EXPONENT_MASK;
}

BOOLEAN qxp_float_from_uint(ULONG value, PULONG result)
{
    ULONG exponent;
    ULONG base;
    ULONG mantissa;
    ULONG shift;
    ULONG remainder;
    ULONG halfway;

    if (!result) return FALSE;
    if (!value) {
        *result = 0;
        return TRUE;
    }
    exponent = 0;
    while (exponent < 31 && (1UL << exponent) <= value / 2) ++exponent;
    base = 1UL << exponent;
    if (exponent <= 23) {
        mantissa = (value - base) << (23 - exponent);
    } else {
        shift = exponent - 23;
        mantissa = (value - base) >> shift;
        remainder = value & ((1UL << shift) - 1);
        halfway = 1UL << (shift - 1);
        if (remainder > halfway ||
            (remainder == halfway && (mantissa & 1))) ++mantissa;
        if (mantissa == QXP_FLOAT_HIDDEN_BIT) {
            mantissa = 0;
            ++exponent;
        }
    }
    *result = ((127UL + exponent) << 23) |
              (mantissa & QXP_FLOAT_FRACTION_MASK);
    return TRUE;
}

static ULONGLONG qxp_float_shift_right_jam(ULONGLONG value, ULONG shift)
{
    ULONGLONG mask;

    if (!shift) return value;
    if (shift >= 64) return value ? 1 : 0;
    mask = ((ULONGLONG)1 << shift) - 1;
    return (value >> shift) | ((value & mask) != 0);
}

static ULONG qxp_float_round_extended(ULONGLONG value)
{
    ULONG mantissa;
    ULONG remainder;

    mantissa = (ULONG)(value >> 3);
    remainder = (ULONG)(value & 7);
    if (remainder > 4 || (remainder == 4 && (mantissa & 1))) ++mantissa;
    return mantissa;
}

/* XP video miniports cannot borrow the interrupted thread's floating-point
 * state. Keep all transform helpers on raw IEEE-754 bit patterns. */
BOOLEAN qxp_float_multiply(ULONG left, ULONG right, PULONG result)
{
    ULONG sign;
    ULONG left_mantissa;
    ULONG right_mantissa;
    LONG left_exponent;
    LONG right_exponent;
    LONG exponent;
    ULONGLONG product;
    ULONGLONG remainder;
    ULONGLONG halfway;
    ULONG mantissa;
    ULONG shift;

    if (!result || !qxp_float_bits_finite(right)) return FALSE;
    if (!qxp_float_bits_finite(left)) {
        *result = 0;
        return TRUE;
    }
    sign = (left ^ right) & 0x80000000UL;
    if (!(left & QXP_FLOAT_ABSOLUTE_MASK) ||
        !(right & QXP_FLOAT_ABSOLUTE_MASK)) {
        *result = sign;
        return TRUE;
    }

    left_exponent = (LONG)((left >> 23) & 0xffUL);
    right_exponent = (LONG)((right >> 23) & 0xffUL);
    left_mantissa = left & QXP_FLOAT_FRACTION_MASK;
    right_mantissa = right & QXP_FLOAT_FRACTION_MASK;
    if (left_exponent) {
        left_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    } else {
        left_exponent = 1;
        while (!(left_mantissa & QXP_FLOAT_HIDDEN_BIT)) {
            left_mantissa <<= 1;
            --left_exponent;
        }
    }
    if (right_exponent) {
        right_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    } else {
        right_exponent = 1;
        while (!(right_mantissa & QXP_FLOAT_HIDDEN_BIT)) {
            right_mantissa <<= 1;
            --right_exponent;
        }
    }

    product = (ULONGLONG)left_mantissa * right_mantissa;
    exponent = left_exponent + right_exponent - 127;
    if (product & ((ULONGLONG)1 << 47)) {
        shift = 24;
        ++exponent;
    } else {
        shift = 23;
    }
    mantissa = (ULONG)(product >> shift);
    remainder = product & (((ULONGLONG)1 << shift) - 1);
    halfway = (ULONGLONG)1 << (shift - 1);
    if (remainder > halfway ||
        (remainder == halfway && (mantissa & 1))) ++mantissa;
    if (mantissa == 0x01000000UL) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        *result = sign | QXP_FLOAT_MAX_FINITE;
        return TRUE;
    }
    if (exponent <= 0) {
        shift = (ULONG)(1 - exponent);
        if (shift >= 25) {
            *result = sign;
            return TRUE;
        }
        remainder = mantissa & (((ULONGLONG)1 << shift) - 1);
        halfway = (ULONGLONG)1 << (shift - 1);
        mantissa >>= shift;
        if (remainder > halfway ||
            (remainder == halfway && (mantissa & 1))) ++mantissa;
        *result = sign | mantissa;
        return TRUE;
    }
    *result = sign | ((ULONG)exponent << 23) |
              (mantissa & QXP_FLOAT_FRACTION_MASK);
    return TRUE;
}

BOOLEAN qxp_float_add(ULONG left, ULONG right, PULONG result)
{
    ULONG left_sign;
    ULONG right_sign;
    ULONG left_exponent;
    ULONG right_exponent;
    ULONG left_mantissa;
    ULONG right_mantissa;
    ULONG temporary;
    ULONG exponent;
    ULONG sign;
    ULONG mantissa;
    ULONG exponent_field;
    ULONGLONG left_extended;
    ULONGLONG right_extended;
    ULONGLONG value;
    ULONG shift;

    if (!result || !qxp_float_bits_finite(left) ||
        !qxp_float_bits_finite(right)) return FALSE;
    if (!(left & QXP_FLOAT_ABSOLUTE_MASK)) {
        *result = right;
        return TRUE;
    }
    if (!(right & QXP_FLOAT_ABSOLUTE_MASK)) {
        *result = left;
        return TRUE;
    }
    left_sign = left & 0x80000000UL;
    right_sign = right & 0x80000000UL;
    left_exponent = (left >> 23) & 0xffUL;
    right_exponent = (right >> 23) & 0xffUL;
    left_mantissa = left & QXP_FLOAT_FRACTION_MASK;
    right_mantissa = right & QXP_FLOAT_FRACTION_MASK;
    if (left_exponent) left_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    else left_exponent = 1;
    if (right_exponent) right_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    else right_exponent = 1;

    if (left_exponent < right_exponent ||
        (left_exponent == right_exponent &&
         left_mantissa < right_mantissa)) {
        temporary = left_sign;
        left_sign = right_sign;
        right_sign = temporary;
        temporary = left_exponent;
        left_exponent = right_exponent;
        right_exponent = temporary;
        temporary = left_mantissa;
        left_mantissa = right_mantissa;
        right_mantissa = temporary;
    }
    exponent = left_exponent;
    sign = left_sign;
    left_extended = (ULONGLONG)left_mantissa << 3;
    shift = left_exponent - right_exponent;
    right_extended = qxp_float_shift_right_jam(
        (ULONGLONG)right_mantissa << 3, shift);
    if (left_sign == right_sign) {
        value = left_extended + right_extended;
        if (value & ((ULONGLONG)1 << 27)) {
            value = qxp_float_shift_right_jam(value, 1);
            ++exponent;
        }
    } else {
        value = left_extended - right_extended;
        if (!value) {
            *result = 0;
            return TRUE;
        }
        while (exponent > 1 && !(value & ((ULONGLONG)1 << 26))) {
            value <<= 1;
            --exponent;
        }
    }
    mantissa = qxp_float_round_extended(value);
    if (mantissa & 0x01000000UL) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        *result = sign | QXP_FLOAT_MAX_FINITE;
        return TRUE;
    }
    exponent_field = exponent == 1 &&
        mantissa < QXP_FLOAT_HIDDEN_BIT ? 0 : exponent;
    *result = sign | (exponent_field << 23) |
              (mantissa & QXP_FLOAT_FRACTION_MASK);
    return TRUE;
}

BOOLEAN qxp_float_divide(ULONG numerator, ULONG denominator, PULONG result)
{
    ULONG sign;
    ULONG numerator_mantissa;
    ULONG denominator_mantissa;
    LONG numerator_exponent;
    LONG denominator_exponent;
    LONG exponent;
    ULONGLONG dividend;
    ULONGLONG extended;
    ULONGLONG remainder;
    ULONG mantissa;
    ULONG exponent_field;
    ULONG shift;

    if (!result || !qxp_float_bits_finite(numerator) ||
        !qxp_float_bits_finite(denominator)) return FALSE;
    sign = (numerator ^ denominator) & 0x80000000UL;
    if (!(numerator & QXP_FLOAT_ABSOLUTE_MASK)) {
        *result = sign;
        return TRUE;
    }
    if (!(denominator & QXP_FLOAT_ABSOLUTE_MASK)) {
        *result = sign | QXP_FLOAT_MAX_FINITE;
        return TRUE;
    }
    numerator_exponent = (LONG)((numerator >> 23) & 0xffUL);
    denominator_exponent = (LONG)((denominator >> 23) & 0xffUL);
    numerator_mantissa = numerator & QXP_FLOAT_FRACTION_MASK;
    denominator_mantissa = denominator & QXP_FLOAT_FRACTION_MASK;
    if (numerator_exponent) {
        numerator_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    } else {
        numerator_exponent = 1;
        while (!(numerator_mantissa & QXP_FLOAT_HIDDEN_BIT)) {
            numerator_mantissa <<= 1;
            --numerator_exponent;
        }
    }
    if (denominator_exponent) {
        denominator_mantissa |= QXP_FLOAT_HIDDEN_BIT;
    } else {
        denominator_exponent = 1;
        while (!(denominator_mantissa & QXP_FLOAT_HIDDEN_BIT)) {
            denominator_mantissa <<= 1;
            --denominator_exponent;
        }
    }
    exponent = numerator_exponent - denominator_exponent + 127;
    if (numerator_mantissa < denominator_mantissa) {
        numerator_mantissa <<= 1;
        --exponent;
    }
    dividend = (ULONGLONG)numerator_mantissa << 26;
    extended = dividend / denominator_mantissa;
    remainder = dividend % denominator_mantissa;
    if (remainder) extended |= 1;
    if (exponent <= 0) {
        shift = (ULONG)(1 - exponent);
        extended = qxp_float_shift_right_jam(extended, shift);
        exponent = 1;
    }
    mantissa = qxp_float_round_extended(extended);
    if (mantissa & 0x01000000UL) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        *result = sign | QXP_FLOAT_MAX_FINITE;
        return TRUE;
    }
    exponent_field = exponent == 1 &&
        mantissa < QXP_FLOAT_HIDDEN_BIT ? 0 : (ULONG)exponent;
    *result = sign | (exponent_field << 23) |
              (mantissa & QXP_FLOAT_FRACTION_MASK);
    return TRUE;
}

static BOOLEAN qxp_float_dot4(const ULONG left[4], const ULONG right[4],
                              PULONG result)
{
    ULONG product;
    ULONG sum;
    ULONG index;

    if (!left || !right || !result ||
        !qxp_float_multiply(left[0], right[0], &sum)) return FALSE;
    for (index = 1; index < 4; ++index) {
        if (!qxp_float_multiply(left[index], right[index], &product) ||
            !qxp_float_add(sum, product, &sum)) return FALSE;
    }
    *result = sum;
    return TRUE;
}

BOOLEAN qxp_float_transform_depth_columns(
    const ULONG world[16], const ULONG view[16],
    const ULONG projection[16], ULONG columns[8])
{
    ULONG view_projection[8];
    ULONG left[4];
    ULONG right[4];
    ULONG row;
    ULONG component;
    ULONG column;

    if (!world || !view || !projection || !columns) return FALSE;
    for (column = 0; column < 2; ++column) {
        for (row = 0; row < 4; ++row) {
            for (component = 0; component < 4; ++component) {
                left[component] = view[row * 4 + component];
                right[component] =
                    projection[component * 4 + column + 2];
            }
            if (!qxp_float_dot4(left, right,
                                &view_projection[row * 2 + column]))
                return FALSE;
        }
    }
    for (column = 0; column < 2; ++column) {
        for (row = 0; row < 4; ++row) {
            for (component = 0; component < 4; ++component) {
                left[component] = world[row * 4 + component];
                right[component] =
                    view_projection[component * 2 + column];
            }
            if (!qxp_float_dot4(left, right,
                                &columns[row * 2 + column])) return FALSE;
        }
    }
    return TRUE;
}

BOOLEAN qxp_float_project_depth(const ULONG position[3],
                                const ULONG columns[8], PULONG depth)
{
    ULONG left[4];
    ULONG right[4];
    ULONG clip_z;
    ULONG clip_w;
    ULONG component;

    if (!position || !columns || !depth) return FALSE;
    left[0] = position[0];
    left[1] = position[1];
    left[2] = position[2];
    left[3] = 0x3f800000UL;
    for (component = 0; component < 4; ++component)
        right[component] = columns[component * 2];
    if (!qxp_float_dot4(left, right, &clip_z)) return FALSE;
    for (component = 0; component < 4; ++component)
        right[component] = columns[component * 2 + 1];
    return qxp_float_dot4(left, right, &clip_w) &&
           qxp_float_divide(clip_z, clip_w, depth);
}
