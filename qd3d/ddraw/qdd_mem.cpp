/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: surface memory.
 *
 * GetDC hands GDI a DIB section on the surface memory itself when that
 * memory is one: surfaces of GDI formats (P1/P4/P8, 16/24/32-bit RGB) of
 * QDD_MEM_DIB bytes or more are allocated by CreateDIBSection (no section
 * handle, so the DC's bitmap looks like native's: dshSection NULL,
 * dsOffset 0), as wide as the pitch, so GDI draws into the surface without
 * a copy.  Smaller surfaces live on the heap (a DIB section per sprite
 * would cost a 64 KiB address-space reservation each) and GetDC copies
 * them, a few kilobytes.  Large non-GDI surfaces use VirtualAlloc.
 */
#include "qdd_priv.h"

#define QDD_MEM_DIB (64u * 1024u)

/* The DIB view of a w x h surface of format f with row pitch `pitch`,
 * false when GDI has no such format. */
bool qdd_mem_bmi(const QddFormat *f, DWORD pitch, DWORD h, QddBmi *bmi, const PALETTEENTRY *pal)
{
    memset(bmi, 0, sizeof(*bmi));
    if (f->cls == QFC_PAL) {
        if (f->bpp == 2)
            return false;
    } else if (f->cls != QFC_RGB || f->bpp < 16) {
        return false;
    }
    bmi->h.biSize = sizeof(bmi->h);
    bmi->h.biWidth = (LONG)(pitch * 8 / f->bpp);
    bmi->h.biHeight = -(LONG)h;
    bmi->h.biPlanes = 1;
    bmi->h.biBitCount = f->bpp;
    if (f->cls == QFC_PAL) {
        bmi->h.biCompression = BI_RGB;
        UINT n = 1u << f->bpp;
        for (UINT i = 0; pal && i < n; ++i) {
            bmi->colors[i].rgbRed = pal[i].peRed;
            bmi->colors[i].rgbGreen = pal[i].peGreen;
            bmi->colors[i].rgbBlue = pal[i].peBlue;
        }
    } else if (f->bpp == 16) {
        bmi->h.biCompression = BI_BITFIELDS;
        bmi->masks[0] = f->mask_r;
        bmi->masks[1] = f->mask_g;
        bmi->masks[2] = f->mask_b;
    } else {
        bmi->h.biCompression = BI_RGB;
    }
    return true;
}

bool qdd_mem_alloc(QddMem *m, DWORD size, const QddFormat *f, DWORD pitch, DWORD h)
{
    memset(m, 0, sizeof(*m));
    if (!size)
        size = 1;
    QddBmi bmi;
    if (size >= QDD_MEM_DIB && f && qdd_mem_bmi(f, pitch, h, &bmi, NULL)) {
        void *bits = NULL;
        HBITMAP dib = CreateDIBSection(NULL, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dib && bits) {
            m->ptr = (BYTE *)bits;
            m->size = size;
            m->dib = dib;
            m->kind = QDD_MEM_KIND_DIB;
            return true;
        }
        if (dib)
            DeleteObject(dib);
    }
    if (size >= QDD_MEM_DIB) {
        m->ptr = (BYTE *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!m->ptr)
            return false;
        m->kind = QDD_MEM_KIND_VIRTUAL;
    } else {
        m->ptr = (BYTE *)calloc(1, size);
        if (!m->ptr)
            return false;
        m->kind = QDD_MEM_KIND_HEAP;
    }
    m->size = size;
    return true;
}

void qdd_mem_free(QddMem *m)
{
    switch (m->kind) {
    case QDD_MEM_KIND_DIB:
        DeleteObject(m->dib);
        break;
    case QDD_MEM_KIND_VIRTUAL:
        VirtualFree(m->ptr, 0, MEM_RELEASE);
        break;
    case QDD_MEM_KIND_HEAP:
        free(m->ptr);
        break;
    default: /* client memory */
        break;
    }
    memset(m, 0, sizeof(*m));
}
