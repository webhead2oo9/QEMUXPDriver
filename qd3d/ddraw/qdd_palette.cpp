/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: IDirectDrawPalette.  An 8-bit palette without DDPCAPS_ALLOW256
 * keeps entry 0 black and entry 255 white (native, dsurface.c PaletteTest).
 * SetEntries changes the version of every surface using the palette (P8
 * textures re-upload) and presents a primary that uses it.
 */
#include "qdd_priv.h"

static UINT qdd_palette_size(DWORD flags)
{
    switch (flags & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT)) {
    case DDPCAPS_1BIT: return 2;
    case DDPCAPS_2BIT: return 4;
    case DDPCAPS_4BIT: return 16;
    case DDPCAPS_8BIT: return 256;
    default: return 0;
    }
}

static void qdd_palette_fix(QddPalette *p)
{
    if (p->count == 256 && !(p->caps & DDPCAPS_ALLOW256)) {
        p->entries[0].peRed = p->entries[0].peGreen = p->entries[0].peBlue = 0;
        p->entries[0].peFlags = 0;
        p->entries[255].peRed = p->entries[255].peGreen = p->entries[255].peBlue = 0xff;
        p->entries[255].peFlags = 0;
    }
}

/* Entries in the caller's layout: PALETTEENTRY, or one byte per entry
 * (DDPCAPS_8BITENTRIES on palettes below 8 bits). */
static void qdd_palette_store(QddPalette *p, UINT start, UINT count, const void *in)
{
    if ((p->caps & DDPCAPS_8BITENTRIES) && p->count < 256) {
        const BYTE *b = (const BYTE *)in;
        for (UINT i = 0; i < count; ++i) {
            p->entries[start + i].peRed = b[i];
            p->entries[start + i].peGreen = p->entries[start + i].peBlue = 0;
            p->entries[start + i].peFlags = 0;
        }
    } else {
        memcpy(&p->entries[start], in, count * sizeof(PALETTEENTRY));
    }
}

QddPalette *qdd_palette_from(const void *iface)
{
    return iface ? (QddPalette *)qdd_reg_find(iface, QDD_T_PALETTE, NULL) : NULL;
}

HRESULT qdd_palette_create(QddDraw *dd, int v, DWORD flags, const PALETTEENTRY *entries, QddPalette **out)
{
    UINT n = qdd_palette_size(flags);
    if (!n)
        return DDERR_INVALIDPARAMS;
    if (flags & ~(DWORD)(DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT | DDPCAPS_8BITENTRIES |
                         DDPCAPS_ALLOW256 | DDPCAPS_ALPHA | DDPCAPS_PRIMARYSURFACE | DDPCAPS_PRIMARYSURFACELEFT |
                         DDPCAPS_INITIALIZE | DDPCAPS_VSYNC))
        return DDERR_INVALIDPARAMS;
    if (!entries)
        return DDERR_INVALIDPARAMS;
    QddPalette *p = new QddPalette;
    if (!p)
        return DDERR_OUTOFMEMORY;
    p->iface.p = p;
    qdd_trace_vtbl(&p->iface, QDD_TK_PAL, 1);
    p->ref = 1;
    p->dd = dd;
    p->caps = flags;
    p->count = n;
    qdd_palette_store(p, 0, n, entries);
    qdd_palette_fix(p);
    p->version = 1;
    p->inv = NULL;
    if (!qdd_reg_add(&p->iface, p, QDD_T_PALETTE, 0)) {
        delete p;
        return DDERR_OUTOFMEMORY;
    }
    if (v == 4 || v == 7) {
        p->dd_ref = (IUnknown *)qdd_draw_iface(dd, v);
        qdd_draw_addref(dd, v);
    }
    *out = p;
    return DD_OK;
}

HRESULT QddPaletteIface::QueryInterface(REFIID riid, void **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, IID_IDirectDrawPalette) || IsEqualGUID(riid, IID_IUnknown)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

ULONG QddPaletteIface::AddRef()
{
    QddPalette *pal = qdd_palette_from(this);
    return pal ? (ULONG)InterlockedIncrement(&pal->ref) : 0;
}

ULONG QddPaletteIface::Release()
{
    QddLock lock;
    QddPalette *pal = qdd_palette_from(this);
    if (!pal || pal->ref <= 0)
        return 0;
    LONG r = InterlockedDecrement(&pal->ref);
    if (!r) {
        IUnknown *dd_ref = pal->dd_ref;
        qdd_reg_remove(this);
        free(pal->inv);
        delete pal;
        if (dd_ref)
            dd_ref->Release();
    }
    return (ULONG)r;
}

HRESULT QddPaletteIface::GetCaps(DWORD *caps)
{
    if (!caps)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    DWORD c = p->caps;
    if (p->dd && p->dd->primary && p->dd->primary->palette == p)
        c |= DDPCAPS_PRIMARYSURFACE;
    *caps = c;
    return DD_OK;
}

HRESULT QddPaletteIface::GetEntries(DWORD flags, DWORD base, DWORD count, PALETTEENTRY *entries)
{
    if (flags || !entries)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (base > p->count || count > p->count - base)
        return DDERR_INVALIDPARAMS;
    if ((p->caps & DDPCAPS_8BITENTRIES) && p->count < 256) {
        BYTE *b = (BYTE *)entries;
        for (DWORD i = 0; i < count; ++i)
            b[i] = p->entries[base + i].peRed;
    } else {
        memcpy(entries, &p->entries[base], count * sizeof(PALETTEENTRY));
    }
    return DD_OK;
}

HRESULT QddPaletteIface::Initialize(IDirectDraw *ddraw, DWORD flags, PALETTEENTRY *entries)
{
    return DDERR_ALREADYINITIALIZED;
}

HRESULT QddPaletteIface::SetEntries(DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries)
{
    if (!entries)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (start > p->count || count > p->count - start)
        return DDERR_INVALIDPARAMS;
    qdd_palette_store(p, start, count, entries);
    qdd_palette_fix(p);
    if (!++p->version)
        p->version = 1;
    if (p->dd) {
        for (QddSurface *s = p->dd->surfaces; s; s = s->dd_next) {
            if (s->palette != p)
                continue;
            qdd_surf_touch(s);
            if (qdd_surf_is_primary(s))
                qdd_primary_written(s, NULL);
        }
    }
    return DD_OK;
}

struct QddInvPal {
    DWORD version;
    DWORD rgb[32768]; /* per 15-bit color: the last color looked up there, + 1 */
    BYTE idx[32768];
};

BYTE qdd_palette_nearest(QddPalette *p, DWORD rgb)
{
    rgb &= 0xffffff;
    QddInvPal *v = p->inv;
    if (!v && (v = (QddInvPal *)calloc(1, sizeof(*v))) != NULL) {
        p->inv = v;
        v->version = p->version;
    }
    DWORD k = ((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x03e0) | ((rgb >> 3) & 0x001f);
    if (v) {
        if (v->version != p->version) {
            memset(v->rgb, 0, sizeof(v->rgb));
            v->version = p->version;
        }
        if (v->rgb[k] == rgb + 1)
            return v->idx[k];
    }
    int r = (int)(rgb >> 16), g = (int)((rgb >> 8) & 0xff), b = (int)(rgb & 0xff);
    UINT best = 0;
    LONG best_d = 0x7fffffff;
    for (UINT i = 0; i < p->count; ++i) {
        int dr = p->entries[i].peRed - r, dg = p->entries[i].peGreen - g, db = p->entries[i].peBlue - b;
        LONG d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
            if (!d)
                break;
        }
    }
    if (v) {
        v->rgb[k] = rgb + 1;
        v->idx[k] = (BYTE)best;
    }
    return (BYTE)best;
}
