/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: textures.  A DirectDraw texture surface bound to a stage gets a
 * Q7Tex with its own managed D3D9 texture (plain or cube, the surface's mip
 * levels), refreshed from the surfaces' system-memory bits when
 * qdd_surface_version says they changed.  Formats D3D9 cannot sample are
 * converted (P8/P4 through the palette, 24-bit, R3G3B2, A8R3G3B2,
 * X4R4G4B4, BGR orders); a keyed variant (colour key -> alpha 0) is kept
 * alongside for D3DRENDERSTATE_COLORKEYENABLE.  Render-target textures
 * (DDSCAPS_TEXTURE|DDSCAPS_3DDEVICE) sample the core's render-target
 * texture directly.  Also IDirect3DDevice7::PreLoad and ::Load.
 *
 * Colour-key comparison uses the texel's native bits including alpha
 * (Wine's test_ck_rgba: key 0xff00ff00 does not match 0x7f00ff00); X bits
 * are ignored.  Keyed texels keep their colour and get alpha 0.
 */
#include "qdd_d3d7.h"

bool q7_surface_ckey(QddSurface *s, DWORD *key)
{
    const DDSURFACEDESC2 *d = qdd_surface_desc(s);
    if (!(d->dwFlags & DDSD_CKSRCBLT))
        return false;
    *key = d->ddckCKSrcBlt.dwColorSpaceLowValue;
    return true;
}

void q7_tex_free(Q7Tex *t)
{
    if (!t)
        return;
    for (UINT v = 0; v < 2; ++v)
        if (t->var[v].tex9)
            t->var[v].tex9->Release();
    free(t);
}

static void q7_tex_reset(Q7Tex *t)
{
    for (UINT v = 0; v < 2; ++v) {
        if (t->var[v].tex9)
            t->var[v].tex9->Release();
        memset(&t->var[v], 0, sizeof(t->var[v]));
    }
}

static bool q7_tex_describe(Q7Tex *t, QddSurface *s)
{
    const DDSURFACEDESC2 *d = qdd_surface_desc(s);
    if (!q7_texture_format(&d->ddpfPixelFormat, &t->fmt))
        return false;
    t->w = d->dwWidth;
    t->h = d->dwHeight;
    t->cube = (d->ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0;
    t->levels = qdd_surface_levels(s);
    if (!t->levels)
        t->levels = 1;
    if (t->levels > Q7_MAX_LEVELS)
        t->levels = Q7_MAX_LEVELS;
    t->desc_caps = d->ddsCaps.dwCaps;
    t->desc_caps2 = d->ddsCaps.dwCaps2;
    t->pf_flags = d->ddpfPixelFormat.dwFlags;
    t->pf_bpp = d->ddpfPixelFormat.dwRGBBitCount;
    return t->w && t->h;
}

Q7Surf *q7_surf(QddSurface *s)
{
    void **slot = qdd_surface_d3d7_slot(s);
    if (!*slot)
        *slot = new Q7Surf(s);
    return (Q7Surf *)*slot;
}

Q7Tex *q7_tex(QddSurface *s)
{
    Q7Surf *x = q7_surf(s);
    if (!x)
        return NULL;
    if (x->tex)
        return x->tex;
    Q7Tex *t = (Q7Tex *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->root = s;
    if (!q7_tex_describe(t, s)) {
        free(t);
        return NULL;
    }
    x->tex = t;
    return t;
}

/* ------------------------------------------------------------------------ */
/* Row conversions                                                           */

static inline DWORD q7_rep5(DWORD v)
{
    return (v << 3) | (v >> 2);
}

static inline DWORD q7_rep6(DWORD v)
{
    return (v << 2) | (v >> 4);
}

static inline DWORD q7_rep3(DWORD v)
{
    return (v << 5) | (v << 2) | (v >> 1);
}

static inline DWORD q7_rep2(DWORD v)
{
    return v * 0x55u;
}

struct Q7Row {
    Q7Conv conv;
    UINT bpp;          /* source bits per pixel */
    bool keyed;
    DWORD key, key_mask;
    DWORD keyed_and;   /* Q7C_COPY: what keyed texels keep (clears their alpha) */
    const DWORD *pal;  /* 256 A8R8G8B8 entries for P8/P4 */
};

/* Bytes per destination texel of a D3D9 format we create (non-block). */
static UINT q7_d9_bpp(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_X8L8V8U8: case D3DFMT_V16U16:
        return 4;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_A8L8:
        return 2;
    case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_A4L4:
        return 1;
    default:
        return 0;
    }
}

static void q7_convert_row(const Q7Row *r, const BYTE *src, BYTE *dst, UINT w)
{
    const bool k = r->keyed;
    const DWORD km = r->key_mask, kv = r->key & r->key_mask;
    switch (r->conv) {
    case Q7C_COPY: {
        UINT bytes = r->bpp / 8;
        memcpy(dst, src, (size_t)w * bytes);
        if (!k)
            return;
        if (bytes == 4) {
            DWORD *d = (DWORD *)dst;
            for (UINT x = 0; x < w; ++x)
                if ((d[x] & km) == kv)
                    d[x] &= 0x00ffffffu;
        } else if (bytes == 2) {
            /* A1R5G5B5 or A4R4G4B4 with alpha of their own. */
            WORD *d = (WORD *)dst;
            for (UINT x = 0; x < w; ++x)
                if ((d[x] & km) == kv)
                    d[x] &= (WORD)r->keyed_and;
        }
        return;
    }
    case Q7C_X8_TO_A8: {
        const DWORD *s = (const DWORD *)src;
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = s[x] & 0x00ffffffu;
            d[x] = (k && v == kv) ? v : v | 0xff000000u;
        }
        return;
    }
    case Q7C_565_TO_8888: {
        const WORD *s = (const WORD *)src;
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = s[x];
            DWORD c = (q7_rep5(v >> 11) << 16) | (q7_rep6((v >> 5) & 0x3f) << 8) | q7_rep5(v & 0x1f);
            d[x] = (k && v == kv) ? c : c | 0xff000000u;
        }
        return;
    }
    case Q7C_555_TO_1555: {
        const WORD *s = (const WORD *)src;
        WORD *d = (WORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            WORD v = (WORD)(s[x] & 0x7fff);
            d[x] = (k && v == kv) ? v : (WORD)(v | 0x8000);
        }
        return;
    }
    case Q7C_444_TO_4444: {
        const WORD *s = (const WORD *)src;
        WORD *d = (WORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            WORD v = (WORD)(s[x] & 0x0fff);
            d[x] = (k && v == kv) ? v : (WORD)(v | 0xf000);
        }
        return;
    }
    case Q7C_888_TO_8888: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = (DWORD)src[3 * x] | ((DWORD)src[3 * x + 1] << 8) | ((DWORD)src[3 * x + 2] << 16);
            d[x] = (k && v == kv) ? v : v | 0xff000000u;
        }
        return;
    }
    case Q7C_332_TO_8888: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = src[x];
            DWORD c = (q7_rep3(v >> 5) << 16) | (q7_rep3((v >> 2) & 7) << 8) | q7_rep2(v & 3);
            d[x] = (k && v == kv) ? c : c | 0xff000000u;
        }
        return;
    }
    case Q7C_8332_TO_8888: {
        const WORD *s = (const WORD *)src;
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = s[x], l = v & 0xff;
            DWORD c = (q7_rep3(l >> 5) << 16) | (q7_rep3((l >> 2) & 7) << 8) | q7_rep2(l & 3);
            d[x] = (k && v == kv) ? c : c | ((v >> 8) << 24);
        }
        return;
    }
    case Q7C_PAL8: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD i = src[x];
            d[x] = (k && i == kv) ? (r->pal[i] & 0x00ffffffu) : r->pal[i];
        }
        return;
    }
    case Q7C_PAL4: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD i = (x & 1) ? (src[x / 2] & 0x0f) : (src[x / 2] >> 4);
            d[x] = (k && i == kv) ? (r->pal[i] & 0x00ffffffu) : r->pal[i];
        }
        return;
    }
    case Q7C_L8_TO_8888: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD l = src[x], c = (l << 16) | (l << 8) | l;
            d[x] = (k && l == kv) ? c : c | 0xff000000u;
        }
        return;
    }
    case Q7C_A8L8_TO_8888: {
        const WORD *s = (const WORD *)src;
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = s[x], l = v & 0xff, c = (l << 16) | (l << 8) | l;
            d[x] = (k && v == kv) ? c : c | ((v >> 8) << 24);
        }
        return;
    }
    case Q7C_A4L4_TO_8888: {
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = src[x], l = (v & 0x0f) * 0x11u, c = (l << 16) | (l << 8) | l;
            d[x] = (k && v == kv) ? c : c | (((v >> 4) * 0x11u) << 24);
        }
        return;
    }
    case Q7C_BGRX_TO_8888: {
        const DWORD *s = (const DWORD *)src;
        DWORD *d = (DWORD *)dst;
        bool alpha = km == 0xffffffffu;
        for (UINT x = 0; x < w; ++x) {
            DWORD v = s[x];
            DWORD c = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
            DWORD a = alpha ? (v & 0xff000000u) : 0xff000000u;
            d[x] = (k && (v & km) == kv) ? c : c | a;
        }
        return;
    }
    default:
        memset(dst, 0, (size_t)w * 4);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Upload                                                                    */

/* The palette a level maps through as A8R8G8B8 (its own, else the
 * root's); alpha from an alpha palette's peFlags. */
static bool q7_palette(QddSurface *level, QddSurface *root, DWORD out[256])
{
    PALETTEENTRY pe[256];
    if (!qdd_surface_palette(level, pe) && !qdd_surface_palette(root, pe))
        return false;
    for (UINT i = 0; i < 256; ++i)
        out[i] = ((DWORD)pe[i].peFlags << 24) | ((DWORD)pe[i].peRed << 16) | ((DWORD)pe[i].peGreen << 8) |
                 pe[i].peBlue;
    return true;
}

static HRESULT q7_upload_level(Q7Tex *t, Q7TexVariant *v, QddSurface *root, QddSurface *ls, UINT face, UINT level,
                               bool keyed, DWORD key)
{
    UINT lw = t->w >> level, lh = t->h >> level;
    if (!lw)
        lw = 1;
    if (!lh)
        lh = 1;

    D3DLOCKED_RECT lr;
    HRESULT hr;
    if (t->cube)
        hr = ((IDirect3DCubeTexture9 *)v->tex9)->LockRect((D3DCUBEMAP_FACES)face, level, &lr, NULL, 0);
    else
        hr = ((IDirect3DTexture9 *)v->tex9)->LockRect(level, &lr, NULL, 0);
    if (FAILED(hr))
        return hr;

    BYTE *bits = NULL;
    LONG pitch = 0;
    hr = qdd_surface_map(ls, false, &bits, &pitch);
    if (SUCCEEDED(hr)) {
        if (t->fmt.block) {
            UINT bsize = t->fmt.d9 == D3DFMT_DXT1 ? 8 : 16;
            UINT rows = (lh + 3) / 4, rowbytes = ((lw + 3) / 4) * bsize;
            for (UINT y = 0; y < rows; ++y)
                memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, bits + (size_t)y * pitch, rowbytes);
        } else {
            Q7Row r;
            DWORD pal[256];
            r.conv = v->conv;
            r.bpp = t->fmt.bpp;
            r.keyed = keyed;
            r.key = key;
            r.key_mask = t->fmt.key_mask;
            r.keyed_and = v->fmt9 == D3DFMT_A4R4G4B4 ? 0x0fffu : v->fmt9 == D3DFMT_A1R5G5B5 ? 0x7fffu : 0x00ffffffu;
            r.pal = NULL;
            if (v->conv == Q7C_PAL8 || v->conv == Q7C_PAL4) {
                if (!q7_palette(ls, root, pal))
                    memset(pal, 0, sizeof(pal)); /* no palette: black, as an empty one */
                r.pal = pal;
            }
            for (UINT y = 0; y < lh; ++y)
                q7_convert_row(&r, bits + (size_t)y * pitch, (BYTE *)lr.pBits + (size_t)y * lr.Pitch, lw);
        }
        qdd_surface_unmap(ls);
    }
    if (t->cube)
        ((IDirect3DCubeTexture9 *)v->tex9)->UnlockRect((D3DCUBEMAP_FACES)face, level);
    else
        ((IDirect3DTexture9 *)v->tex9)->UnlockRect(level);
    return hr;
}

/* Conversion of a format D3D9 lacks to A8R8G8B8 (luminance and the
 * 16-bit RGB formats some hardware drops), Q7C_NONE when there is none. */
static Q7Conv q7_fallback(const Q7Format *f)
{
    switch ((DWORD)f->d9) {
    case D3DFMT_L8: return Q7C_L8_TO_8888;
    case D3DFMT_A8L8: return Q7C_A8L8_TO_8888;
    case D3DFMT_A4L4: return Q7C_A4L4_TO_8888;
    case D3DFMT_R5G6B5: return Q7C_565_TO_8888;
    default: return Q7C_NONE;
    }
}

static bool q7_tex_create(Q7D3D *d3d, Q7Tex *t, Q7TexVariant *v, bool keyed)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    IDirect3D9 *d3d9 = qdd_draw_d3d9(d3d->dd);
    if (!d9 || !d3d9 || !d3d->get_caps())
        return false;
    D3DFORMAT f = keyed ? t->fmt.d9_key : t->fmt.d9;
    Q7Conv conv = keyed ? t->fmt.conv_key : t->fmt.conv;
    D3DRESOURCETYPE rtype = t->cube ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE;
    if (FAILED(d3d9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3d->display_format, 0, rtype, f))) {
        Q7Conv fb = keyed ? Q7C_NONE : q7_fallback(&t->fmt);
        if (fb == Q7C_NONE)
            return false;
        f = D3DFMT_A8R8G8B8;
        conv = fb;
    }
    HRESULT hr;
    if (t->cube) {
        IDirect3DCubeTexture9 *c = NULL;
        hr = d9->CreateCubeTexture(t->w, t->levels, 0, f, D3DPOOL_MANAGED, &c, NULL);
        v->tex9 = c;
    } else {
        IDirect3DTexture9 *p = NULL;
        hr = d9->CreateTexture(t->w, t->h, t->levels, 0, f, D3DPOOL_MANAGED, &p, NULL);
        v->tex9 = p;
    }
    if (FAILED(hr) || !v->tex9) {
        v->tex9 = NULL;
        return false;
    }
    if (!t->fmt.block && !q7_d9_bpp(f)) {
        v->tex9->Release();
        v->tex9 = NULL;
        return false;
    }
    v->fmt9 = f;
    v->conv = conv;
    memset(v->versions, 0, sizeof(v->versions));
    v->root_version = 0;
    return true;
}

IDirect3DBaseTexture9 *q7_tex_d3d9(Q7D3D *d3d, QddSurface *s, bool ck, bool *source_alpha)
{
    const DDSURFACEDESC2 *d = qdd_surface_desc(s);
    *source_alpha = false;
    if (d->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) {
        /* Render-target texture: the core keeps it current. */
        IDirect3DTexture9 *rtt = qdd_surface_rt_texture(s);
        if (rtt) {
            *source_alpha = (d->ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) != 0;
            return rtt;
        }
    }
    Q7Tex *t = q7_tex(s);
    if (!t)
        return NULL;
    /* SetSurfaceDesc may replace size or format. */
    if (d->dwWidth != t->w || d->dwHeight != t->h || d->ddpfPixelFormat.dwFlags != t->pf_flags ||
        d->ddpfPixelFormat.dwRGBBitCount != t->pf_bpp || qdd_surface_levels(s) != t->levels) {
        q7_tex_reset(t);
        if (!q7_tex_describe(t, s))
            return NULL;
    }
    *source_alpha = t->fmt.has_alpha;

    DWORD key = 0;
    bool keyed = ck && t->fmt.d9_key != D3DFMT_UNKNOWN && q7_surface_ckey(s, &key);
    Q7TexVariant *v = &t->var[keyed ? 1 : 0];
    if (!v->tex9 && !q7_tex_create(d3d, t, v, keyed))
        return NULL;

    /* Palettes and keys come from the root (cube faces: each face root);
     * any change of the root re-uploads every level that depends on it. */
    bool root_dep = keyed || v->conv == Q7C_PAL8 || v->conv == Q7C_PAL4;
    UINT faces = t->cube ? 6 : 1;
    for (UINT face = 0; face < faces; ++face) {
        QddSurface *froot = t->cube ? qdd_surface_sub(s, face, 0) : s;
        if (!froot)
            continue;
        DWORD fkey = key;
        bool fkeyed = keyed && (!t->cube || q7_surface_ckey(froot, &fkey));
        DWORD rver = root_dep ? qdd_surface_version(froot) : 0;
        bool all = root_dep && (v->root_version != rver || (fkeyed && v->key != fkey));
        for (UINT level = 0; level < t->levels; ++level) {
            QddSurface *ls = level ? qdd_surface_sub(s, face, level) : froot;
            if (!ls)
                continue;
            DWORD ver = qdd_surface_version(ls);
            if (!all && v->versions[face][level] == ver)
                continue;
            if (SUCCEEDED(q7_upload_level(t, v, froot, ls, face, level, fkeyed, fkey)))
                v->versions[face][level] = ver;
        }
        if (root_dep) {
            v->root_version = rver;
            v->key = fkey;
        }
    }
    return v->tex9;
}

/* ------------------------------------------------------------------------ */
/* PreLoad, Load                                                             */

HRESULT STDMETHODCALLTYPE Q7Device::PreLoad(IDirectDrawSurface7 *surface)
{
    if (!surface)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!d3d)
        return DDERR_INVALIDOBJECT;
    QddSurface *s = qdd_surface_lookup(surface);
    if (!s || qdd_surface_draw(s) != dd || !(qdd_surface_desc(s)->ddsCaps.dwCaps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDPARAMS;
    bool alpha;
    IDirect3DBaseTexture9 *t9 = q7_tex_d3d9(d3d, s, st.rs[d7::D3DRENDERSTATE_COLORKEYENABLE] != 0, &alpha);
    if (t9)
        t9->PreLoad();
    return D3D_OK;
}

/* Native rules (Wine's d3d_device7_Load and test_device_load): top-level
 * surfaces only; the rectangle must lie in the source and fit the
 * destination; destination levels must be a subset of the source's; cube
 * maps load the faces in flags; the palette entries (when both have one)
 * and the colour keys are copied. */
static bool q7_levels_subset(QddSurface *dst, QddSurface *src)
{
    UINT dl = qdd_surface_levels(dst), sl = qdd_surface_levels(src);
    const DDSURFACEDESC2 *dd0 = qdd_surface_desc(dst);
    const DDSURFACEDESC2 *sd0 = qdd_surface_desc(src);
    DWORD dw = dd0->dwWidth, dh = dd0->dwHeight;
    for (UINT i = 0; i < sl; ++i) {
        DWORD sw = sd0->dwWidth >> i, sh = sd0->dwHeight >> i;
        if (!sw)
            sw = 1;
        if (!sh)
            sh = 1;
        if (sw == dw && sh == dh)
            return sl - i >= dl;
    }
    return false;
}

static void q7_copy_props(QddSurface *dst, QddSurface *src)
{
    IDirectDrawSurface7 *d = qdd_surface_iface7(dst), *s = qdd_surface_iface7(src);
    IDirectDrawPalette *dp = NULL, *sp = NULL;
    if (SUCCEEDED(s->GetPalette(&sp)) && sp && SUCCEEDED(d->GetPalette(&dp)) && dp) {
        PALETTEENTRY pe[256];
        if (SUCCEEDED(sp->GetEntries(0, 0, 256, pe)))
            dp->SetEntries(0, 0, 256, pe);
    }
    if (dp)
        dp->Release();
    if (sp)
        sp->Release();
    /* The destination's keys become the source's: a key only the
     * destination has is deleted (native, Wine's test_texture_load_ckey). */
    for (DWORD flag = DDCKEY_DESTBLT; flag <= DDCKEY_SRCOVERLAY; flag <<= 1) {
        DDCOLORKEY key;
        if (SUCCEEDED(s->GetColorKey(flag, &key)))
            d->SetColorKey(flag, &key);
        else if (SUCCEEDED(d->GetColorKey(flag, &key)))
            d->SetColorKey(flag, NULL);
    }
}

static void q7_copy_chain(QddSurface *dst_root, QddSurface *src_root, UINT dface, UINT sface, POINT pt, RECT r)
{
    QddSurface *dst0 = qdd_surface_sub(dst_root, dface, 0), *src0 = qdd_surface_sub(src_root, sface, 0);
    if (!dst0 || !src0)
        return;
    q7_copy_props(dst0, src0);
    UINT sl = qdd_surface_levels(src_root), dl = qdd_surface_levels(dst_root);
    UINT di = 0;
    for (UINT si = 0; si < sl && di < dl; ++si) {
        QddSurface *s = qdd_surface_sub(src_root, sface, si);
        QddSurface *d = qdd_surface_sub(dst_root, dface, di);
        if (!s || !d)
            break;
        const DDSURFACEDESC2 *sd = qdd_surface_desc(s), *ddsc = qdd_surface_desc(d);
        if (sd->dwWidth == ddsc->dwWidth && sd->dwHeight == ddsc->dwHeight) {
            RECT dr = {pt.x, pt.y, pt.x + (r.right - r.left), pt.y + (r.bottom - r.top)};
            qdd_surface_iface7(d)->Blt(&dr, qdd_surface_iface7(s), &r, DDBLT_WAIT, NULL);
            ++di;
        }
        pt.x /= 2;
        pt.y /= 2;
        r.left /= 2;
        r.top /= 2;
        r.right = (r.right + 1) / 2;
        r.bottom = (r.bottom + 1) / 2;
    }
}

HRESULT STDMETHODCALLTYPE Q7Device::Load(IDirectDrawSurface7 *dst_iface, POINT *dst_point,
                                         IDirectDrawSurface7 *src_iface, RECT *src_rect, DWORD flags)
{
    Q7Lock lock;
    if (!d3d)
        return DDERR_INVALIDOBJECT;
    QddSurface *dst = dst_iface ? qdd_surface_lookup(dst_iface) : NULL;
    QddSurface *src = src_iface ? qdd_surface_lookup(src_iface) : NULL;
    if (!dst || !src)
        return DDERR_INVALIDPARAMS;
    const DDSURFACEDESC2 *sd = qdd_surface_desc(src);
    const DDSURFACEDESC2 *ddsc = qdd_surface_desc(dst);
    RECT r;
    if (src_rect)
        r = *src_rect;
    else
        SetRect(&r, 0, 0, (int)sd->dwWidth, (int)sd->dwHeight);
    POINT pt = {0, 0};
    if (dst_point)
        pt = *dst_point;
    if (r.left >= r.right || r.top >= r.bottom || r.left < 0 || r.top < 0 || (DWORD)r.right > sd->dwWidth ||
        (DWORD)r.bottom > sd->dwHeight || pt.x < 0 || pt.y < 0 ||
        (DWORD)(pt.x + r.right - r.left) > sd->dwWidth || (DWORD)(pt.y + r.bottom - r.top) > sd->dwHeight ||
        ddsc->dwWidth > sd->dwWidth || ddsc->dwHeight > sd->dwHeight)
        return DDERR_INVALIDPARAMS;
    if ((sd->ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL) || (ddsc->ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
        return DDERR_INVALIDPARAMS;
    bool scube = (sd->ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0, dcube = (ddsc->ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0;
    if (scube != dcube)
        return DDERR_INVALIDPARAMS;
    if (!scube) {
        if (!q7_levels_subset(dst, src))
            return DDERR_INVALIDPARAMS;
        q7_copy_chain(dst, src, 0, 0, pt, r);
        return D3D_OK;
    }
    /* Every source face needs a destination face; destination faces are
     * loaded where flags names them. */
    for (UINT f = 0; f < 6; ++f) {
        QddSurface *sf = qdd_surface_sub(src, f, 0), *df = qdd_surface_sub(dst, f, 0);
        if (sf && !df)
            return DDERR_INVALIDPARAMS;
        if (sf && df && !q7_levels_subset(df, sf))
            return DDERR_INVALIDPARAMS;
    }
    for (UINT f = 0; f < 6; ++f) {
        if (!(flags & (DDSCAPS2_CUBEMAP_POSITIVEX << f)))
            continue;
        if (qdd_surface_sub(src, f, 0) && qdd_surface_sub(dst, f, 0))
            q7_copy_chain(dst, src, f, f, pt, r);
    }
    return D3D_OK;
}
