/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: Blt and BltFast.
 *
 * Validation order and results follow the native runtime (Wine's ddraw
 * tests, measured on the stock runtime).  Stretching samples
 *   src.left + floor((x - dst.left) * src_width / dst_width)
 * (and the same in y) from the original source image, overlapping
 * self-blits included.  Colour keys compare the raw pixel with the alpha
 * bits masked off; a destination-key blit through IDirectDrawSurface7
 * uses the destination's key, through the older interfaces the source
 * surface's destination key (measured; Wine documents it).
 *
 * Where the pixels are: work runs on the copy that is current.  A plain
 * copy or stretch whose source exists only as a Direct3D 9 render target
 * (3D rendering) runs as StretchRect, colour fills of GPU-current render
 * targets as ColorFill, depth fills of GPU-current z-buffers as Clear; the
 * rest runs on the system-memory copies (reading the GPU copy back first).
 */
#include "qdd_priv.h"

#define QDD_BLT_IGNORED                                                                            \
    (DDBLT_ALPHADEST | DDBLT_ALPHADESTCONSTOVERRIDE | DDBLT_ALPHADESTNEG |                          \
     DDBLT_ALPHADESTSURFACEOVERRIDE | DDBLT_ALPHAEDGEBLEND | DDBLT_ALPHASRC |                       \
     DDBLT_ALPHASRCCONSTOVERRIDE | DDBLT_ALPHASRCNEG | DDBLT_ALPHASRCSURFACEOVERRIDE |              \
     DDBLT_ZBUFFER | DDBLT_ZBUFFERDESTCONSTOVERRIDE | DDBLT_ZBUFFERDESTOVERRIDE |                   \
     DDBLT_ZBUFFERSRCCONSTOVERRIDE | DDBLT_ZBUFFERSRCOVERRIDE)

static bool qdd_rect_in(const RECT *r, DWORD w, DWORD h)
{
    return r->left >= 0 && r->top >= 0 && r->left < r->right && r->top < r->bottom && (DWORD)r->right <= w &&
           (DWORD)r->bottom <= h;
}

bool qdd_rect_valid(const RECT *r, DWORD w, DWORD h)
{
    return qdd_rect_in(r, w, h);
}

/* The rectangles a blit to dst writes: dr itself, or its intersection
 * with the clipper's region (and the surface). */
struct QddClip {
    RECT *rects;
    UINT count;
    RECT one;
    RGNDATA *data;
    HRGN rgn; /* the clip region for GDI pushes (primary), or NULL */
};

static void qdd_clip_free(QddClip *c)
{
    free(c->data);
    if (c->rgn)
        DeleteObject(c->rgn);
}

static HRESULT qdd_clip_build(QddSurface *dst, const RECT *dr, QddClip *c)
{
    memset(c, 0, sizeof(*c));
    RECT bounds = {0, 0, (LONG)dst->desc.dwWidth, (LONG)dst->desc.dwHeight};
    if (!dst->clipper) {
        c->one = *dr;
        c->rects = &c->one;
        c->count = 1;
        return DD_OK;
    }
    bool none = false;
    HRGN rgn = qdd_clipper_region(dst->clipper, &none);
    if (!rgn)
        return none ? DDERR_NOCLIPLIST : DDERR_GENERIC;
    HRGN box = CreateRectRgn(dr->left, dr->top, dr->right, dr->bottom);
    HRGN surf = CreateRectRgn(0, 0, bounds.right, bounds.bottom);
    if (!box || !surf) {
        if (box)
            DeleteObject(box);
        if (surf)
            DeleteObject(surf);
        DeleteObject(rgn);
        return DDERR_OUTOFMEMORY;
    }
    CombineRgn(rgn, rgn, box, RGN_AND);
    CombineRgn(rgn, rgn, surf, RGN_AND);
    DeleteObject(box);
    DeleteObject(surf);
    DWORD size = GetRegionData(rgn, 0, NULL);
    c->data = size ? (RGNDATA *)malloc(size) : NULL;
    if (!c->data || !GetRegionData(rgn, size, c->data)) {
        free(c->data);
        c->data = NULL;
        DeleteObject(rgn);
        return DDERR_OUTOFMEMORY;
    }
    c->rects = (RECT *)c->data->Buffer;
    c->count = c->data->rdh.nCount;
    c->rgn = rgn;
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Pixel access for sub-byte formats                                         */

static inline DWORD qdd_get_bits(const BYTE *row, DWORD x, UINT bpp)
{
    DWORD bit = x * bpp;
    UINT shift = 8 - bpp - (bit & 7);
    return (row[bit >> 3] >> shift) & ((1u << bpp) - 1);
}

static inline void qdd_set_bits(BYTE *row, DWORD x, UINT bpp, DWORD v)
{
    DWORD bit = x * bpp;
    UINT shift = 8 - bpp - (bit & 7);
    BYTE mask = (BYTE)(((1u << bpp) - 1) << shift);
    row[bit >> 3] = (BYTE)((row[bit >> 3] & ~mask) | ((v << shift) & mask));
}

/* ------------------------------------------------------------------------ */
/* CPU fills                                                                 */

static void qdd_cpu_fill(QddSurface *s, const RECT *r, DWORD value, DWORD keep_mask)
{
    QddBuffer *b = s->buf;
    UINT bytes = qdd_format_bytes(s->fmt);
    for (LONG y = r->top; y < r->bottom; ++y) {
        BYTE *row = b->mem.ptr + (size_t)y * b->pitch;
        if (!bytes) {
            for (LONG x = r->left; x < r->right; ++x)
                qdd_set_bits(row, (DWORD)x, s->fmt->bpp, value);
            continue;
        }
        BYTE *p = row + (size_t)r->left * bytes;
        LONG n = r->right - r->left;
        if (keep_mask) {
            for (LONG x = 0; x < n; ++x, p += bytes)
                qdd_write_px(p, bytes, (qdd_read_px(p, bytes) & keep_mask) | (value & ~keep_mask));
            continue;
        }
        switch (bytes) {
        case 1:
            memset(p, (int)(value & 0xff), (size_t)n);
            break;
        case 2:
            for (LONG x = 0; x < n; ++x)
                ((WORD *)p)[x] = (WORD)value;
            break;
        case 4:
            for (LONG x = 0; x < n; ++x)
                ((DWORD *)p)[x] = value;
            break;
        default:
            for (LONG x = 0; x < n; ++x, p += 3)
                qdd_write_px(p, 3, value);
            break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* CPU copies                                                                */

struct QddKeys {
    bool src, dst;
    DWORD src_lo, src_hi, src_mask;
    DWORD dst_lo, dst_hi, dst_mask;
};

static DWORD qdd_key_mask(const QddFormat *f)
{
    if (f->cls == QFC_PAL)
        return (1u << f->bpp) - 1;
    if (f->cls == QFC_RGB || f->cls == QFC_LUM)
        return f->mask_r | f->mask_g | f->mask_b;
    if (f->bpp >= 32)
        return 0xffffffffu;
    return (1u << f->bpp) - 1;
}

/* Copies srect of (sf, sbits, spitch) onto the part of drect inside clip
 * of (df, dbits, dpitch), scaled; mirror flags DDBLTFX_MIRROR*. */
static HRESULT qdd_copy_raw(const QddFormat *df, BYTE *dbits, LONG dpitch, const RECT *dr, const RECT *clip,
                            const QddFormat *sf, const BYTE *sbits, LONG spitch, const RECT *sr, const QddKeys *k,
                            DWORD mirror, const PALETTEENTRY *pal)
{
    LONG dw = dr->right - dr->left, dh = dr->bottom - dr->top;
    LONG sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    bool lr = (mirror & DDBLTFX_MIRRORLEFTRIGHT) != 0, ud = (mirror & DDBLTFX_MIRRORUPDOWN) != 0;
    UINT db = qdd_format_bytes(df), sb = qdd_format_bytes(sf);
    /* Formats that differ only in an alpha-or-unused channel copy raw: the
     * bits move unchanged (stock: X1R5G5B5 -> A1R5G5B5 keeps the top bit,
     * ddraw7.c test_surface_format_conversion_alpha). */
    bool same = df == sf || (df->cls == QFC_RGB && sf->cls == QFC_RGB && df->bpp == sf->bpp &&
                             df->mask_r == sf->mask_r && df->mask_g == sf->mask_g && df->mask_b == sf->mask_b);
    bool yuv = sf->cls == QFC_FOURCC_YUV;
    if (!same && !((df->cls == QFC_RGB || df->cls == QFC_LUM) &&
                   (sf->cls == QFC_RGB || sf->cls == QFC_PAL || sf->cls == QFC_LUM || yuv)))
        return DDERR_UNSUPPORTED;
    if (df->cls == QFC_FOURCC_YUV && same) {
        /* YUV to YUV: pairs of pixels, unscaled. */
        if (dw != sw || dh != sh || lr || ud || k->src || k->dst)
            return DDERR_UNSUPPORTED;
        LONG x0 = clip->left & ~1, x1 = (clip->right + 1) & ~1;
        for (LONG y = clip->top; y < clip->bottom; ++y)
            memmove(dbits + (size_t)y * dpitch + (size_t)x0 * 2,
                    sbits + (size_t)(sr->top + y - dr->top) * spitch + (size_t)((sr->left + x0 - dr->left) & ~1) * 2,
                    (size_t)(x1 - x0) * 2);
        return DD_OK;
    }

    bool plain = same && !k->src && !k->dst && !lr && !ud && dw == sw && dh == sh && db;
    if (plain) {
        size_t n = (size_t)(clip->right - clip->left) * db;
        for (LONG y = clip->top; y < clip->bottom; ++y) {
            const BYTE *s = sbits + (size_t)(sr->top + y - dr->top) * spitch + (size_t)(sr->left + clip->left - dr->left) * sb;
            memmove(dbits + (size_t)y * dpitch + (size_t)clip->left * db, s, n);
        }
        return DD_OK;
    }

    for (LONG y = clip->top; y < clip->bottom; ++y) {
        LONG v = y - dr->top;
        if (ud)
            v = dh - 1 - v;
        LONG sy = sr->top + (LONG)(((LONGLONG)v * sh) / dh);
        const BYTE *srow = sbits + (size_t)sy * spitch;
        BYTE *drow = dbits + (size_t)y * dpitch;
        for (LONG x = clip->left; x < clip->right; ++x) {
            LONG u = x - dr->left;
            if (lr)
                u = dw - 1 - u;
            LONG sx = sr->left + (LONG)(((LONGLONG)u * sw) / dw);
            DWORD spx;
            if (yuv) {
                spx = 0;
            } else if (sb) {
                spx = qdd_read_px(srow + (size_t)sx * sb, sb);
            } else {
                spx = qdd_get_bits(srow, (DWORD)sx, sf->bpp);
            }
            if (k->src && !yuv) {
                DWORD m = spx & k->src_mask;
                if (m >= k->src_lo && m <= k->src_hi)
                    continue;
            }
            if (k->dst) {
                DWORD dpx = db ? qdd_read_px(drow + (size_t)x * db, db) : qdd_get_bits(drow, (DWORD)x, df->bpp);
                DWORD m = dpx & k->dst_mask;
                if (m < k->dst_lo || m > k->dst_hi)
                    continue;
            }
            DWORD out;
            if (same) {
                out = spx;
            } else if (yuv) {
                BYTE tmp[8]; /* two pixels of up to 4 bytes */
                qdd_convert_row(df, tmp, sf, srow + (size_t)(sx & ~1) * 2, (sx & 1) + 1, NULL);
                out = qdd_read_px(tmp + (sx & 1) * db, db);
            } else {
                /* P8 -> RGB copies the index into every channel (stock,
                 * ddraw7.c test_p8_blit); others convert by value. */
                if (sf->cls == QFC_PAL)
                    out = qdd_argb_to_pixel(df, 0xff000000u | spx * 0x010101u);
                else
                    out = qdd_argb_to_pixel(df, qdd_pixel_to_argb(sf, spx, pal));
            }
            if (db)
                qdd_write_px(drow + (size_t)x * db, db, out);
            else
                qdd_set_bits(drow, (DWORD)x, df->bpp, out);
        }
    }
    return DD_OK;
}

/* Copies with a block (DXT) format on either side: unscaled block-aligned
 * copies of one format move blocks; everything else decodes the source
 * and the touched destination blocks to A8R8G8B8, copies, and encodes. */
static HRESULT qdd_copy_blocks(QddSurface *dst, const RECT *dr, const RECT *clip, const QddFormat *sf,
                               const BYTE *sbits, LONG spitch, const RECT *sr, const QddKeys *k, DWORD mirror,
                               const PALETTEENTRY *pal)
{
    const QddFormat *df = dst->fmt;
    QddBuffer *b = dst->buf;
    LONG dw = dr->right - dr->left, dh = dr->bottom - dr->top;
    LONG sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    if (df == sf && dw == sw && dh == sh && !mirror && !k->src && !k->dst && !(clip->left & 3) &&
        !(clip->top & 3) && !(sr->left & 3) && !(sr->top & 3) && !((sr->left + clip->left - dr->left) & 3) &&
        !((sr->top + clip->top - dr->top) & 3)) {
        LONG ox = clip->left - dr->left, oy = clip->top - dr->top;
        LONG bx = (sr->left + ox) / 4, by = (sr->top + oy) / 4;
        LONG nbx = (clip->right - clip->left + 3) / 4, nby = (clip->bottom - clip->top + 3) / 4;
        for (LONG y = 0; y < nby; ++y)
            memmove(b->mem.ptr + (size_t)(clip->top / 4 + y) * b->pitch + (size_t)(clip->left / 4) * df->block_bytes,
                    sbits + (size_t)(by + y) * spitch + (size_t)bx * sf->block_bytes, (size_t)nbx * df->block_bytes);
        return DD_OK;
    }
    if (k->src || k->dst)
        return DDERR_UNSUPPORTED; /* no color keys on compressed surfaces */
    const QddFormat *argb = qdd_format_d3d(D3DFMT_A8R8G8B8);
    const QddFormat *sf2 = sf;
    const BYTE *s2 = sbits;
    LONG sp2 = spitch;
    RECT sr2 = *sr;
    DWORD *stmp = NULL, *dtmp = NULL;
    HRESULT hr = DD_OK;
    if (sf->cls == QFC_FOURCC_BLOCK) {
        LONG x0 = sr->left & ~3L, y0 = sr->top & ~3L, x1 = (sr->right + 3) & ~3L, y1 = (sr->bottom + 3) & ~3L;
        stmp = (DWORD *)malloc((size_t)(x1 - x0) * (size_t)(y1 - y0) * 4);
        if (!stmp)
            return DDERR_OUTOFMEMORY;
        qdd_dxt_decode_rect(sf, sbits, spitch, x0, y0, x1, y1, stmp);
        sf2 = argb;
        s2 = (const BYTE *)stmp;
        sp2 = (x1 - x0) * 4;
        OffsetRect(&sr2, -x0, -y0);
    }
    if (df->cls != QFC_FOURCC_BLOCK) {
        hr = qdd_copy_raw(df, b->mem.ptr, b->pitch, dr, clip, sf2, s2, sp2, &sr2, k, mirror, pal);
    } else {
        LONG x0 = clip->left & ~3L, y0 = clip->top & ~3L, x1 = (clip->right + 3) & ~3L, y1 = (clip->bottom + 3) & ~3L;
        dtmp = (DWORD *)malloc((size_t)(x1 - x0) * (size_t)(y1 - y0) * 4);
        if (!dtmp) {
            free(stmp);
            return DDERR_OUTOFMEMORY;
        }
        qdd_dxt_decode_rect(df, b->mem.ptr, b->pitch, x0, y0, x1, y1, dtmp);
        RECT dr2 = *dr, clip2 = *clip;
        OffsetRect(&dr2, -x0, -y0);
        OffsetRect(&clip2, -x0, -y0);
        hr = qdd_copy_raw(argb, (BYTE *)dtmp, (x1 - x0) * 4, &dr2, &clip2, sf2, s2, sp2, &sr2, k, mirror, pal);
        /* A source without alpha is opaque (the raw copy between formats
         * that differ only in an unused channel kept its X bits). */
        if (SUCCEEDED(hr) && sf2->cls == QFC_RGB && !sf2->mask_a)
            for (LONG y = clip2.top; y < clip2.bottom; ++y)
                for (LONG x = clip2.left; x < clip2.right; ++x)
                    dtmp[(size_t)y * (x1 - x0) + x] |= 0xff000000u;
        if (SUCCEEDED(hr))
            qdd_dxt_encode_rect(df, b->mem.ptr, b->pitch, x0, y0, x1, y1, dtmp);
    }
    free(stmp);
    free(dtmp);
    return hr;
}

/* YV12 (planar) blits: unscaled copies between YV12 surfaces move the three
 * planes; a YV12 source converts to A8R8G8B8 first (BT.601) for blits to
 * RGB surfaces, stretched or not.  src_h: the source surface's height,
 * which places its chroma planes. */
static HRESULT qdd_copy_planar(QddSurface *dst, const RECT *dr, const RECT *clip, const QddFormat *sf,
                               const BYTE *sbits, LONG spitch, LONG src_h, const RECT *sr, const QddKeys *k,
                               DWORD mirror)
{
    const QddFormat *df = dst->fmt;
    QddBuffer *b = dst->buf;
    LONG dw = dr->right - dr->left, dh = dr->bottom - dr->top;
    LONG sw = sr->right - sr->left, sh = sr->bottom - sr->top;
    const BYTE *sv = sbits + (size_t)spitch * src_h;
    const BYTE *su = sv + (size_t)(spitch / 2) * ((src_h + 1) / 2);
    if (df->cls == QFC_FOURCC_PLANAR) {
        if (sf != df || dw != sw || dh != sh || mirror || k->src || k->dst)
            return DDERR_UNSUPPORTED;
        LONG dst_h = (LONG)dst->desc.dwHeight, dp = b->pitch;
        BYTE *dv = b->mem.ptr + (size_t)dp * dst_h, *du = dv + (size_t)(dp / 2) * ((dst_h + 1) / 2);
        LONG ox = sr->left - dr->left, oy = sr->top - dr->top;
        for (LONG y = clip->top; y < clip->bottom; ++y)
            memmove(b->mem.ptr + (size_t)y * dp + clip->left, sbits + (size_t)(y + oy) * spitch + clip->left + ox,
                    (size_t)(clip->right - clip->left));
        for (LONG y = clip->top / 2; y < (clip->bottom + 1) / 2; ++y) {
            size_t n = (size_t)((clip->right + 1) / 2 - clip->left / 2);
            size_t so = (size_t)(y + oy / 2) * (spitch / 2) + (clip->left + ox) / 2;
            memmove(dv + (size_t)y * (dp / 2) + clip->left / 2, sv + so, n);
            memmove(du + (size_t)y * (dp / 2) + clip->left / 2, su + so, n);
        }
        return DD_OK;
    }
    DWORD *tmp = (DWORD *)malloc((size_t)sw * (size_t)sh * 4);
    if (!tmp)
        return DDERR_OUTOFMEMORY;
    for (LONG y = 0; y < sh; ++y) {
        LONG sy = sr->top + y;
        for (LONG x = 0; x < sw; ++x) {
            LONG sx = sr->left + x;
            size_t c = (size_t)(sy / 2) * (spitch / 2) + sx / 2;
            tmp[(size_t)y * sw + x] = qdd_yuv_to_argb(sbits[(size_t)sy * spitch + sx], su[c], sv[c]);
        }
    }
    RECT sr2 = {0, 0, sw, sh};
    QddKeys nk = *k;
    nk.src = false; /* no source keys on YUV surfaces */
    HRESULT hr = qdd_copy_raw(df, b->mem.ptr, b->pitch, dr, clip, qdd_format_d3d(D3DFMT_A8R8G8B8),
                              (const BYTE *)tmp, sw * 4, &sr2, &nk, mirror, NULL);
    free(tmp);
    return hr;
}

static HRESULT qdd_cpu_copy(QddSurface *dst, const RECT *dr, const RECT *clip, const QddFormat *sf, const BYTE *sbits,
                            LONG spitch, LONG src_h, const RECT *sr, const QddKeys *k, DWORD mirror,
                            const PALETTEENTRY *pal)
{
    if (dst->fmt->cls == QFC_FOURCC_PLANAR || sf->cls == QFC_FOURCC_PLANAR)
        return qdd_copy_planar(dst, dr, clip, sf, sbits, spitch, src_h, sr, k, mirror);
    if (dst->fmt->cls == QFC_FOURCC_BLOCK || sf->cls == QFC_FOURCC_BLOCK)
        return qdd_copy_blocks(dst, dr, clip, sf, sbits, spitch, sr, k, mirror, pal);
    return qdd_copy_raw(dst->fmt, dst->buf->mem.ptr, dst->buf->pitch, dr, clip, sf, sbits, spitch, sr, k, mirror, pal);
}

/* ------------------------------------------------------------------------ */
/* GPU paths                                                                 */

static bool qdd_gpu_only(const QddSurface *s)
{
    return s->buf->gpu && s->buf->gpu_valid && !s->buf->sys_valid;
}

static bool qdd_gpu_rgb(const QddSurface *s)
{
    return s->fmt->cls == QFC_RGB && (s->desc.ddsCaps.dwCaps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE |
                                                                DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER));
}

static D3DCOLOR qdd_fill_argb(const QddSurface *s, DWORD value)
{
    return qdd_pixel_to_argb(s->fmt, value, NULL);
}

static HRESULT qdd_gpu_color_fill(QddSurface *dst, const QddClip *clip, DWORD value)
{
    IDirect3DDevice9 *dev = dst->dd->dev9;
    IDirect3DSurface9 *g = qdd_gpu_surface(dst);
    if (!g)
        return E_FAIL;
    D3DCOLOR c = qdd_fill_argb(dst, value);
    for (UINT i = 0; i < clip->count; ++i)
        dev->ColorFill(g, &clip->rects[i], c);
    dst->buf->gpu_valid = true;
    dst->buf->sys_valid = false;
    return DD_OK;
}

static HRESULT qdd_gpu_depth_fill(QddSurface *dst, const QddClip *clip, DWORD value)
{
    IDirect3DDevice9 *dev = dst->dd->dev9;
    IDirect3DSurface9 *z = qdd_gpu_surface(dst);
    if (!z)
        return E_FAIL;
    DWORD zmask = dst->fmt->mask_r;
    float depth = zmask ? (float)((double)(value & zmask) / (double)zmask) : 0.0f;
    IDirect3DSurface9 *old_ds = NULL;
    D3DVIEWPORT9 vp;
    dev->GetDepthStencilSurface(&old_ds);
    dev->GetViewport(&vp);
    dev->SetDepthStencilSurface(z);
    for (UINT i = 0; i < clip->count; ++i) {
        D3DRECT r = {clip->rects[i].left, clip->rects[i].top, clip->rects[i].right, clip->rects[i].bottom};
        D3DVIEWPORT9 full = {0, 0, dst->desc.dwWidth, dst->desc.dwHeight, 0.0f, 1.0f};
        dev->SetViewport(&full);
        dev->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, depth, 0);
    }
    dev->SetDepthStencilSurface(old_ds);
    dev->SetViewport(&vp);
    if (old_ds)
        old_ds->Release();
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Blt                                                                       */

static HRESULT qdd_fill(QddSurface *dst, const RECT *dr_in, DWORD value, bool depth)
{
    RECT dr;
    if (dr_in)
        dr = *dr_in;
    else
        SetRect(&dr, 0, 0, (int)dst->desc.dwWidth, (int)dst->desc.dwHeight);
    if (dr.left >= dr.right || dr.top >= dr.bottom)
        return DDERR_INVALIDRECT;
    if (!dst->clipper && !qdd_rect_in(&dr, dst->desc.dwWidth, dst->desc.dwHeight))
        return DDERR_INVALIDRECT;
    if (dst->lost)
        return DDERR_SURFACELOST;
    if (qdd_surf_busy(dst))
        return DDERR_SURFACEBUSY;
    if (dst->fmt->cls == QFC_FOURCC_BLOCK || dst->fmt->cls == QFC_FOURCC_PLANAR)
        return DDERR_UNSUPPORTED;
    bool yuv = dst->fmt->cls == QFC_FOURCC_YUV;
    QddClip clip;
    HRESULT hr = qdd_clip_build(dst, &dr, &clip);
    if (FAILED(hr))
        return hr;
    if (!yuv && qdd_gpu_only(dst) && dst->dd && dst->dd->dev9) {
        if (depth)
            hr = qdd_gpu_depth_fill(dst, &clip, value);
        else if (dst->fmt->cls == QFC_RGB)
            hr = qdd_gpu_color_fill(dst, &clip, value);
        else
            hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            qdd_surf_touch(dst);
            if (qdd_surf_is_primary(dst))
                qdd_primary_written(dst, &dr);
            qdd_clip_free(&clip);
            return DD_OK;
        }
    }
    hr = qdd_buf_sys(dst);
    if (FAILED(hr)) {
        qdd_clip_free(&clip);
        return hr;
    }
    DWORD keep = depth ? ~dst->fmt->mask_r : 0;
    if (depth && dst->fmt->bpp < 32)
        keep &= (1u << dst->fmt->bpp) - 1;
    for (UINT i = 0; i < clip.count; ++i) {
        if (yuv) {
            /* YUY2/UYVY: the fill value is a whole macropixel (two pixels,
             * Y0 U Y1 V in memory order), as native fills them. */
            const RECT *r = &clip.rects[i];
            LONG x0 = r->left & ~1L, x1 = (r->right + 1) & ~1L;
            for (LONG y = r->top; y < r->bottom; ++y) {
                DWORD *p = (DWORD *)(dst->buf->mem.ptr + (size_t)y * dst->buf->pitch) + x0 / 2;
                for (LONG x = x0; x < x1; x += 2)
                    *p++ = value;
            }
            continue;
        }
        qdd_cpu_fill(dst, &clip.rects[i], value, keep);
    }
    qdd_buf_sys_written(dst);
    if (qdd_surf_is_primary(dst))
        qdd_primary_written(dst, &dr);
    qdd_clip_free(&clip);
    return DD_OK;
}

HRESULT qdd_blt(QddSurface *dst, const RECT *dr_in, QddSurface *src, const RECT *sr_in, DWORD flags, const DDBLTFX *fx,
                int call_v)
{
    QddLock lock;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && (!fx || (flags & DDBLT_KEYSRC)))
        return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYDESTOVERRIDE) && (!fx || (flags & DDBLT_KEYDEST)))
        return DDERR_INVALIDPARAMS;
    flags &= ~(DWORD)QDD_BLT_IGNORED;
    if ((flags & (DDBLT_DDFX | DDBLT_COLORFILL | DDBLT_DEPTHFILL)) && !fx)
        return DDERR_INVALIDPARAMS;
    if (fx && fx->dwSize != sizeof(*fx))
        return DDERR_INVALIDPARAMS;
    bool zdst = dst->fmt->cls == QFC_DEPTH;
    if (flags & DDBLT_COLORFILL) {
        if ((flags & DDBLT_ROP) || src || zdst)
            return DDERR_INVALIDPARAMS;
        return qdd_fill(dst, dr_in, fx->dwFillColor, false);
    }
    if (flags & DDBLT_DEPTHFILL) {
        if (src || !zdst)
            return DDERR_INVALIDPARAMS;
        return qdd_fill(dst, dr_in, fx->dwFillDepth, true);
    }
    if (flags & DDBLT_ROP) {
        if (!fx)
            return DDERR_INVALIDPARAMS;
        switch (fx->dwROP) {
        case SRCCOPY:
            break;
        case BLACKNESS:
        case WHITENESS:
            /* No raster operations on FOURCC surfaces (stock; color fills
             * of YUY2/UYVY do work). */
            if (dst->fmt->cls == QFC_FOURCC_YUV || dst->fmt->cls == QFC_FOURCC_BLOCK)
                return DDERR_UNSUPPORTED;
            /* A source is allowed and ignored, but its rectangle is checked
             * (ddraw7.c test_color_fill). */
            if (src && sr_in &&
                (sr_in->left > sr_in->right || sr_in->top > sr_in->bottom ||
                 !qdd_rect_in(sr_in, src->desc.dwWidth, src->desc.dwHeight)))
                return DDERR_INVALIDRECT;
            /* Every bit, also those outside a z-buffer's depth mask
             * (ddraw7.c test_color_fill, stock). */
            return qdd_fill(dst, dr_in, fx->dwROP == WHITENESS ? 0xffffffffu : 0, false);
        default:
            return DDERR_NORASTEROPHW;
        }
    }
    if (!src)
        return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYSRC) && !(src->desc.dwFlags & DDSD_CKSRCBLT))
        return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYDEST) && !(dst->desc.dwFlags & DDSD_CKDESTBLT))
        return DDERR_INVALIDPARAMS;

    RECT dr, sr;
    if (dr_in)
        dr = *dr_in;
    else
        SetRect(&dr, 0, 0, (int)dst->desc.dwWidth, (int)dst->desc.dwHeight);
    if (sr_in)
        sr = *sr_in;
    else
        SetRect(&sr, 0, 0, (int)src->desc.dwWidth, (int)src->desc.dwHeight);
    if (dr.left >= dr.right || dr.top >= dr.bottom)
        return DDERR_INVALIDRECT;
    if (!dst->clipper && !qdd_rect_in(&dr, dst->desc.dwWidth, dst->desc.dwHeight))
        return DDERR_INVALIDRECT;
    if (!qdd_rect_in(&sr, src->desc.dwWidth, src->desc.dwHeight))
        return DDERR_INVALIDRECT;
    if (dst->lost || src->lost)
        return DDERR_SURFACELOST;
    if (qdd_surf_busy(dst) || qdd_surf_busy(src))
        return DDERR_SURFACEBUSY;

    QddKeys k;
    memset(&k, 0, sizeof(k));
    if (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) {
        const DDCOLORKEY *key = (flags & DDBLT_KEYSRC) ? &src->desc.ddckCKSrcBlt : &fx->ddckSrcColorkey;
        k.src = true;
        k.src_lo = key->dwColorSpaceLowValue;
        k.src_hi = key->dwColorSpaceHighValue;
        k.src_mask = qdd_key_mask(src->fmt);
    }
    if (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) {
        const DDCOLORKEY *key;
        if (flags & DDBLT_KEYDESTOVERRIDE)
            key = &fx->ddckDestColorkey;
        else
            key = call_v >= 4 ? &dst->desc.ddckCKDestBlt : &src->desc.ddckCKDestBlt;
        k.dst = true;
        k.dst_lo = key->dwColorSpaceLowValue;
        k.dst_hi = key->dwColorSpaceHighValue;
        k.dst_mask = qdd_key_mask(dst->fmt);
    }
    DWORD mirror = (flags & DDBLT_DDFX) ? fx->dwDDFX & (DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN) : 0;

    QddClip clip;
    HRESULT hr = qdd_clip_build(dst, &dr, &clip);
    if (FAILED(hr))
        return hr;
    if (!clip.count) {
        qdd_clip_free(&clip);
        return DD_OK;
    }

    /* GPU: a plain copy from a surface whose contents exist only on the GPU,
     * onto a surface that can hold a Direct3D 9 copy. */
    QddDraw *dd = dst->dd;
    if (dd && dd->dev9 && src != dst && !k.src && !k.dst && !mirror && clip.count == 1 &&
        EqualRect(&clip.rects[0], &dr) && qdd_gpu_only(src) && qdd_gpu_rgb(dst) && src->fmt->cls == QFC_RGB) {
        IDirect3DSurface9 *gs = qdd_gpu_surface(src), *gd = qdd_gpu_surface(dst);
        if (gs && gd && SUCCEEDED(dd->dev9->StretchRect(gs, &sr, gd, &dr, D3DTEXF_POINT))) {
            dst->buf->gpu_valid = true;
            dst->buf->sys_valid = false;
            qdd_surf_touch(dst);
            if (qdd_surf_is_primary(dst))
                qdd_primary_written(dst, &dr);
            qdd_clip_free(&clip);
            return DD_OK;
        }
    }

    if (qdd_surf_is_primary(src))
        qdd_primary_read(src, &sr);
    if (qdd_surf_is_primary(dst) && (k.src || k.dst))
        qdd_primary_read(dst, &dr);
    if (FAILED(hr = qdd_buf_sys(src)) || FAILED(hr = qdd_buf_sys(dst))) {
        qdd_clip_free(&clip);
        return hr;
    }
    /* Overlap with itself: work from a copy of the source rectangle. */
    const BYTE *sbits = src->buf->mem.ptr;
    LONG spitch = src->buf->pitch;
    BYTE *tmp = NULL;
    RECT srt = sr;
    if (src->buf == dst->buf) {
        LONG rows = (LONG)qdd_format_rows(src->fmt, (DWORD)(sr.bottom - sr.top));
        LONG first = src->fmt->cls == QFC_FOURCC_BLOCK ? sr.top / 4 : sr.top;
        tmp = (BYTE *)malloc((size_t)rows * spitch);
        if (!tmp) {
            qdd_clip_free(&clip);
            return DDERR_OUTOFMEMORY;
        }
        memcpy(tmp, sbits + (size_t)first * spitch, (size_t)rows * spitch);
        sbits = tmp - (size_t)first * spitch;
    }
    const PALETTEENTRY *pal = qdd_format_is_pal(src->fmt) ? qdd_surf_pal_entries(src) : NULL;
    for (UINT i = 0; i < clip.count && SUCCEEDED(hr); ++i)
        hr = qdd_cpu_copy(dst, &dr, &clip.rects[i], src->fmt, sbits, spitch, (LONG)src->desc.dwHeight, &srt, &k,
                          mirror, pal);
    free(tmp);
    if (SUCCEEDED(hr)) {
        qdd_buf_sys_written(dst);
        if (qdd_surf_is_primary(dst))
            qdd_primary_written(dst, &dr);
    }
    qdd_clip_free(&clip);
    return hr;
}

HRESULT qdd_bltfast(QddSurface *dst, DWORD x, DWORD y, QddSurface *src, const RECT *sr_in, DWORD trans, int call_v)
{
    QddLock lock;
    RECT sr;
    if (sr_in)
        sr = *sr_in;
    else
        SetRect(&sr, 0, 0, (int)src->desc.dwWidth, (int)src->desc.dwHeight);
    if (!qdd_rect_in(&sr, src->desc.dwWidth, src->desc.dwHeight))
        return DDERR_INVALIDRECT;
    DWORD sw = (DWORD)(sr.right - sr.left), sh = (DWORD)(sr.bottom - sr.top);
    DWORD dw = dst->desc.dwWidth, dh = dst->desc.dwHeight;
    if (sw > dw || x > dw - sw || sh > dh || y > dh - sh)
        return DDERR_INVALIDRECT;
    if (dst->clipper)
        return DDERR_BLTFASTCANTCLIP;
    DWORD flags = 0;
    DDBLTFX fx;
    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    if (trans & DDBLTFAST_SRCCOLORKEY) {
        if (!(src->desc.dwFlags & DDSD_CKSRCBLT))
            return DDERR_INVALIDPARAMS;
        flags |= DDBLT_KEYSRC;
    }
    if (trans & DDBLTFAST_DESTCOLORKEY) {
        if (!(dst->desc.dwFlags & DDSD_CKDESTBLT) && !(src->desc.dwFlags & DDSD_CKDESTBLT))
            return DDERR_INVALIDPARAMS;
        /* The key comes from the source surface through the older
         * interfaces, from the destination through IDirectDrawSurface4/7
         * (stock, ddraw4.c test_ck_operation). */
        fx.ddckDestColorkey = call_v >= 4 ? dst->desc.ddckCKDestBlt : src->desc.ddckCKDestBlt;
        flags |= DDBLT_KEYDESTOVERRIDE;
    }
    RECT dr = {(LONG)x, (LONG)y, (LONG)(x + sw), (LONG)(y + sh)};
    return qdd_blt(dst, &dr, src, &sr, flags, &fx, call_v);
}
