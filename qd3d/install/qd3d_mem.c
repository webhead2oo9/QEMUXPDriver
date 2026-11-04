/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The four functions GCC may call by itself in freestanding code (struct
 * copies, zeroing). Built with -fno-builtin -fno-tree-loop-distribute-patterns
 * so these loops are not turned back into calls to themselves.
 */
#include <stddef.h>

void *memset(void *d, int c, size_t n);
void *memcpy(void *restrict d, const void *restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;

    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;

    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;

    if (p < q) {
        while (n--)
            *p++ = *q++;
    } else {
        while (n--)
            p[n] = q[n];
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;

    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}
