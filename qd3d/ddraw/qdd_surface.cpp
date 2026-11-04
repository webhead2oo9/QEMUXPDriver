/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: DirectDraw surfaces (IDirectDrawSurface, 2, 3, 4, 7 and
 * IDirectDrawGammaControl).
 *
 * Reference counting follows the native runtime (Wine's tests and the
 * stock runtime): one count per interface version, IUnknown shares the
 * IDirectDrawSurface count, the gamma control has its own; every surface
 * of a complex structure starts with one reference on the interface
 * version of its creator (so GetAttachedSurface returns a count of 2); the
 * structure is destroyed with its root when the root's last interface
 * reference goes away.  Surfaces created through IDirectDraw4/7 hold a
 * reference on that interface until then.
 *
 * Complex attachments use Wine's layout (complex_attached): a flip chain
 * front -> back 1 -> ... -> back N -> front; a mip level -> the next one;
 * a cube root (+X) -> -Z, +Z, -Y, +Y, -X level 0, then +X level 1.
 */
#include "qdd_priv.h"

bool g_qdd_d3d_loaded; /* a Direct3D interface was handed out (surface QI quirk) */

void qdd_vidmem_add(DWORD bytes);
void qdd_vidmem_sub(DWORD bytes);
bool qdd_vidmem_fits(DWORD bytes);

static const GUID QDD_IID_IDirectDrawGammaControl = {0x69c11c3e, 0xb46b, 0x11d1, {0xad, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x9b, 0x4e}};

/* ------------------------------------------------------------------------ */
/* Interface plumbing                                                        */

void *qdd_surf_iface(QddSurface *s, int v)
{
    switch (v) {
    case 1: return static_cast<IDirectDrawSurface *>(&s->if1);
    case 2: return static_cast<IDirectDrawSurface2 *>(&s->if2);
    case 3: return static_cast<IDirectDrawSurface3 *>(&s->if3);
    case 4: return static_cast<IDirectDrawSurface4 *>(&s->if4);
    default: return static_cast<IDirectDrawSurface7 *>(&s->if7);
    }
}

static int qdd_vidx_to_v(int vidx)
{
    static const int v[5] = {1, 2, 3, 4, 7};
    return v[vidx];
}

QddSurface *qdd_surf_from(const void *iface)
{
    if (!iface)
        return NULL;
    return (QddSurface *)qdd_reg_find(iface, QDD_T_SURFACE, NULL);
}

static void qdd_surface_destroy_tree(QddSurface *root);

ULONG qdd_surf_addref(QddSurface *s, int vidx)
{
    LONG r = InterlockedIncrement(&s->ref[vidx]);
    if (r == 1)
        InterlockedIncrement(&s->ifaces);
    return (ULONG)r;
}

ULONG qdd_surf_release(QddSurface *s, int vidx)
{
    if (s->ref[vidx] <= 0)
        return 0;
    LONG r = InterlockedDecrement(&s->ref[vidx]);
    if (!r && !InterlockedDecrement(&s->ifaces) && s->root == s)
        qdd_surface_destroy_tree(s);
    return (ULONG)r;
}

void qdd_surf_touch(QddSurface *s)
{
    if (!++s->version_counter)
        s->version_counter = 1;
}

/* ------------------------------------------------------------------------ */
/* Buffers and locations                                                     */

void qdd_buf_free(QddBuffer *b)
{
    qdd_pass_buffer_free(b);
    if (b->gpu)
        b->gpu->Release();
    if (b->gpu_tex)
        b->gpu_tex->Release();
    if (b->stage)
        b->stage->Release();
    qdd_mem_free(&b->mem);
    memset(b, 0, sizeof(*b));
}

void qdd_buf_sys_written(QddSurface *s)
{
    s->buf->sys_valid = true;
    s->buf->gpu_valid = false;
    qdd_surf_touch(s);
}

const PALETTEENTRY *qdd_surf_pal_entries(QddSurface *s)
{
    if (s->palette)
        return s->palette->entries;
    if (s->dd && s->dd->primary && s->dd->primary->palette)
        return s->dd->primary->palette->entries;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Creation                                                                  */

static void qdd_surface_link(QddDraw *dd, QddSurface *s)
{
    s->dd_prev = NULL;
    s->dd_next = dd->surfaces;
    if (dd->surfaces)
        dd->surfaces->dd_prev = s;
    dd->surfaces = s;
}

static void qdd_surface_unlink(QddSurface *s)
{
    if (!s->dd)
        return;
    if (s->dd_prev)
        s->dd_prev->dd_next = s->dd_next;
    else
        s->dd->surfaces = s->dd_next;
    if (s->dd_next)
        s->dd_next->dd_prev = s->dd_prev;
    s->dd_next = s->dd_prev = NULL;
}

static bool qdd_surface_register(QddSurface *s)
{
    for (int i = 0; i < 5; ++i)
        if (!qdd_reg_add(qdd_surf_iface(s, qdd_vidx_to_v(i)), s, QDD_T_SURFACE, i))
            return false;
    return qdd_reg_add(static_cast<IDirectDrawGammaControl *>(&s->ifg), s, QDD_T_GAMMA, 5);
}

static void qdd_surface_unregister(QddSurface *s)
{
    for (int i = 0; i < 5; ++i)
        qdd_reg_remove(qdd_surf_iface(s, qdd_vidx_to_v(i)));
    qdd_reg_remove(static_cast<IDirectDrawGammaControl *>(&s->ifg));
}

static bool qdd_is_vidmem(const QddSurface *s)
{
    return (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) != 0;
}

static DWORD qdd_surface_bytes(const QddSurface *s)
{
    return (DWORD)s->own_buf.pitch * qdd_format_rows(s->fmt, s->desc.dwHeight);
}

/* One surface of a structure with its memory (desc already final). */
static QddSurface *qdd_surface_new(QddDraw *dd, int version, int creator_v, const DDSURFACEDESC2 *desc,
                                   const QddFormat *fmt, void *client_mem)
{
    QddSurface *s = new QddSurface;
    if (!s)
        return NULL;
    s->if1.s = s->if2.s = s->if3.s = s->if4.s = s->if7.s = s;
    s->ifg.s = s;
    if (g_qdd_trace) {
        static const int vs[] = {1, 2, 3, 4, 7};
        for (int v : vs)
            qdd_trace_vtbl(qdd_surf_iface(s, v), QDD_TK_SURF, v);
        qdd_trace_vtbl(&s->ifg, QDD_TK_GAMMA, 1);
    }
    s->dd = dd;
    s->version = version;
    s->creator_v = creator_v;
    s->desc = *desc;
    s->fmt = fmt;
    s->root = s;
    s->buf = &s->own_buf;
    s->version_counter = 1;
    s->ref[qdd_vidx(version)] = 1;
    s->ifaces = 1;
    DWORD pitch = desc->dwFlags & DDSD_LINEARSIZE ? qdd_format_pitch(fmt, desc->dwWidth) : (DWORD)desc->lPitch;
    if (client_mem) {
        s->own_buf.mem.ptr = (BYTE *)client_mem;
        s->own_buf.pitch = (LONG)(desc->dwFlags & DDSD_LPSURFACE && desc->dwFlags & DDSD_PITCH && desc->lPitch
                                      ? (DWORD)desc->lPitch
                                      : pitch);
        s->own_buf.mem.size = (DWORD)s->own_buf.pitch * qdd_format_rows(fmt, desc->dwHeight);
    } else {
        s->own_buf.pitch = (LONG)pitch;
        ULONGLONG size = (ULONGLONG)pitch * qdd_format_rows(fmt, desc->dwHeight);
        if (size > 0x7fffffffu || !qdd_mem_alloc(&s->own_buf.mem, (DWORD)size, fmt, pitch, qdd_format_rows(fmt, desc->dwHeight))) {
            delete s;
            return NULL;
        }
    }
    s->own_buf.sys_valid = true;
    s->desc.lpSurface = NULL;
    s->desc.dwFlags &= ~DDSD_LPSURFACE;
    if (!qdd_surface_register(s)) {
        qdd_surface_unregister(s);
        qdd_buf_free(&s->own_buf);
        delete s;
        return NULL;
    }
    qdd_surface_link(dd, s);
    if (qdd_is_vidmem(s))
        qdd_vidmem_add(qdd_surface_bytes(s));
    return s;
}

/* Frees one surface (no tree walk). */
static void qdd_surface_free(QddSurface *s)
{
    qdd_d3d7_surface_destroy(s);
    if (s->dc) {
        SelectObject(s->dc, s->dc_old);
        DeleteDC(s->dc);
        if (s->dc_copy)
            DeleteObject(s->dc_dib);
        s->dc = NULL;
    }
    if (s->palette)
        s->palette->iface.Release();
    s->palette = NULL;
    if (s->clipper)
        qdd_clipper_release(s->clipper);
    s->clipper = NULL;
    while (s->priv) {
        QddPrivate *p = s->priv;
        s->priv = p->next;
        if (p->flags & DDSPD_IUNKNOWNPOINTER)
            ((IUnknown *)p->data)->Release();
        else
            free(p->data);
        free(p);
    }
    if (s->dd && qdd_is_vidmem(s))
        qdd_vidmem_sub(qdd_surface_bytes(s));
    if (s->dd && s->dd->primary == s)
        s->dd->primary = NULL;
    qdd_surface_unlink(s);
    qdd_surface_unregister(s);
    qdd_buf_free(&s->own_buf);
    free(s->attached);
    QddSurface *held = s->dup_of;
    delete s;
    /* A duplicate kept its source alive (DuplicateSurface). */
    if (held && !InterlockedDecrement(&held->ifaces) && held->root == held)
        qdd_surface_destroy_tree(held);
}

HRESULT qdd_surface_duplicate(QddSurface *src, int creator_v, int v, void **out)
{
    /* Native refuses primaries and surfaces of complex structures. */
    if (qdd_surf_is_primary(src) || src->root != src || src->complex_attached[0] ||
        (src->desc.ddsCaps.dwCaps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX)))
        return DDERR_CANTDUPLICATE;
    if (src->lost)
        return DDERR_SURFACELOST;
    DDSURFACEDESC2 desc = src->desc;
    desc.lPitch = src->buf->pitch;
    QddSurface *d = qdd_surface_new(src->dd, src->version, creator_v, &desc, src->fmt, src->buf->mem.ptr);
    if (!d)
        return DDERR_OUTOFMEMORY;
    /* The same memory and Direct3D 9 copy: both objects use src's buffer. */
    d->buf = src->buf;
    d->ref[qdd_vidx(src->version)] = 0;
    d->ref[qdd_vidx(v)] = 1;
    d->dup_of = src;
    InterlockedIncrement(&src->ifaces);
    *out = qdd_surf_iface(d, v);
    return DD_OK;
}

/* Explicit attachments: release what s holds and remove s from others. */
static void qdd_surface_detach_all(QddSurface *s)
{
    while (s->attached_count) {
        QddAttach a = s->attached[--s->attached_count];
        a.surf->attached_to--;
        qdd_surf_release(a.surf, a.vidx);
    }
}

static void qdd_surface_destroy_tree(QddSurface *root)
{
    QddLock lock;
    /* Collect the structure first: releases below may touch it. */
    QddSurface *list[1 + 6 * 16 + 64];
    UINT n = 0;
    for (QddSurface *s = root; s && n < sizeof(list) / sizeof(list[0]); s = s->complex_next)
        list[n++] = s;
    /* Flip chains rotate buffers: give every surface its own back so each
     * frees the memory it allocated. */
    for (UINT i = 0; i < n; ++i)
        list[i]->buf = &list[i]->own_buf;
    for (UINT i = 0; i < n; ++i)
        qdd_surface_detach_all(list[i]);
    IUnknown *dd_ref = root->dd_ref;
    for (UINT i = 0; i < n; ++i)
        qdd_surface_free(list[i]);
    if (dd_ref)
        dd_ref->Release();
}

static UINT qdd_log2(DWORD v)
{
    UINT n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
}

static bool qdd_ck_valid(const DDSURFACEDESC2 *d)
{
    if ((d->dwFlags & DDSD_CKDESTOVERLAY) &&
        d->ddckCKDestOverlay.dwColorSpaceLowValue != d->ddckCKDestOverlay.dwColorSpaceHighValue)
        return false;
    if ((d->dwFlags & DDSD_CKDESTBLT) &&
        d->ddckCKDestBlt.dwColorSpaceLowValue != d->ddckCKDestBlt.dwColorSpaceHighValue)
        return false;
    if ((d->dwFlags & DDSD_CKSRCOVERLAY) &&
        d->ddckCKSrcOverlay.dwColorSpaceLowValue != d->ddckCKSrcOverlay.dwColorSpaceHighValue)
        return false;
    if ((d->dwFlags & DDSD_CKSRCBLT) &&
        d->ddckCKSrcBlt.dwColorSpaceLowValue != d->ddckCKSrcBlt.dwColorSpaceHighValue)
        return false;
    return true;
}

bool qdd_draw_texture_format(QddDraw *dd, const DDPIXELFORMAT *pf)
{
    const QddFormat *f = qdd_format_from_ddpf(pf);
    if (!f || f->d3d == D3DFMT_UNKNOWN)
        return false;
    switch (f->cls) {
    case QFC_RGB:
    case QFC_LUM:
    case QFC_ALPHA:
    case QFC_BUMP:
    case QFC_FOURCC_BLOCK:
        break;
    default:
        return false;
    }
    if (!dd || !dd->d3d9)
        return true;
    return SUCCEEDED(dd->d3d9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                                 D3DRTYPE_TEXTURE, f->d3d));
}

HRESULT qdd_surface_create(QddDraw *dd, int v, const DDSURFACEDESC2 *in, QddSurface **out)
{
    DDSURFACEDESC2 desc = *in;
    *out = NULL;
    desc.dwFlags |= DDSD_CAPS;
    DWORD caps = desc.ddsCaps.dwCaps, caps2 = desc.ddsCaps.dwCaps2;
    int version = v == 2 || v == 3 ? 1 : v;

    if ((caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY)) == (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))
        return DDERR_INVALIDCAPS;
    if (v != 1 && (caps & DDSCAPS_BACKBUFFER))
        return DDERR_INVALIDCAPS;
    if (caps & DDSCAPS_FLIP) {
        if (!(desc.dwFlags & DDSD_BACKBUFFERCOUNT) || !desc.dwBackBufferCount)
            return DDERR_INVALIDCAPS;
        if (!(caps & DDSCAPS_COMPLEX))
            return DDERR_INVALIDCAPS;
        if ((caps & DDSCAPS_PRIMARYSURFACE) && !(dd->coop & DDSCL_EXCLUSIVE))
            return DDERR_NOEXCLUSIVEMODE;
        if (desc.dwBackBufferCount > 32)
            return DDERR_INVALIDPARAMS;
    } else if ((desc.dwFlags & DDSD_BACKBUFFERCOUNT) && (caps & DDSCAPS_COMPLEX)) {
        return DDERR_INVALIDCAPS;
    }
    if ((caps & (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE)) == (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE))
        return DDERR_INVALIDCAPS;
    /* Stock: WRITEONLY is accepted on managed textures only (ddraw7.c
     * test_lockrect_invalid). */
    if ((caps & DDSCAPS_WRITEONLY) && !(caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        return DDERR_INVALIDCAPS;
    if (caps & DDSCAPS_OVERLAY)
        return DDERR_NOOVERLAYHW;
    bool managed = (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) != 0;
    if (managed && (!(caps & DDSCAPS_TEXTURE) || (caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_ALLOCONLOAD) && !(caps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDCAPS;
    bool cube = (caps2 & DDSCAPS2_CUBEMAP) != 0;
    if (cube && !(caps2 & DDSCAPS2_CUBEMAP_ALLFACES))
        return DDERR_INVALIDPARAMS;
    if (!cube && (caps2 & DDSCAPS2_CUBEMAP_ALLFACES))
        return DDERR_INVALIDCAPS;
    if (cube && !(caps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_MIPMAP) && !(caps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_COMPLEX) && !(caps & (DDSCAPS_FLIP | DDSCAPS_MIPMAP)) && !cube)
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_ZBUFFER) && (caps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDCAPS;

    /* Pixel format. */
    const QddFormat *fmt;
    bool legacy_z = false;
    if (desc.dwFlags & DDSD_PIXELFORMAT) {
        fmt = qdd_format_from_ddpf(&desc.ddpfPixelFormat);
        if (!fmt)
            return DDERR_INVALIDPIXELFORMAT;
        if ((fmt->cls == QFC_DEPTH) != ((caps & DDSCAPS_ZBUFFER) != 0))
            return DDERR_INVALIDPIXELFORMAT;
    } else if (caps & DDSCAPS_ZBUFFER) {
        if (!(desc.dwFlags & DDSD_ZBUFFERBITDEPTH))
            return DDERR_INVALIDPIXELFORMAT;
        /* DDSURFACEDESC's dwZBufferBitDepth shares dwMipMapCount's slot. */
        fmt = qdd_format_for_depth_bits(desc.dwMipMapCount);
        if (!fmt)
            return DDERR_INVALIDPIXELFORMAT;
        legacy_z = true;
    } else {
        fmt = qdd_mode_format();
    }
    /* Stock: system-memory YUV surfaces only as textures. */
    if ((fmt->cls == QFC_FOURCC_YUV || fmt->cls == QFC_FOURCC_PLANAR) && (caps & DDSCAPS_SYSTEMMEMORY) &&
        !(caps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDPIXELFORMAT;

    /* Size. */
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        QddMode m;
        qdd_mode_current(&m);
        desc.dwWidth = m.w;
        desc.dwHeight = m.h;
        desc.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    } else if (!(desc.dwFlags & DDSD_WIDTH) || !(desc.dwFlags & DDSD_HEIGHT) || !desc.dwWidth || !desc.dwHeight) {
        return DDERR_INVALIDPARAMS;
    }
    if (desc.dwWidth > 16384 || desc.dwHeight > 16384)
        return DDERR_INVALIDPARAMS;
    if (fmt->cls == QFC_FOURCC_BLOCK && ((desc.dwWidth & 3) || (desc.dwHeight & 3)))
        return DDERR_INVALIDPARAMS;

    /* Mip levels. */
    UINT levels = 1;
    if (caps & DDSCAPS_MIPMAP) {
        if (caps & DDSCAPS_COMPLEX) {
            if (desc.dwFlags & DDSD_MIPMAPCOUNT) {
                if (!desc.dwMipMapCount)
                    return DDERR_INVALIDPARAMS;
            } else if (version == 7) {
                DWORD m = desc.dwWidth > desc.dwHeight ? desc.dwWidth : desc.dwHeight;
                desc.dwMipMapCount = qdd_log2(m) + 1;
            } else {
                DWORD m = desc.dwWidth < desc.dwHeight ? desc.dwWidth : desc.dwHeight;
                desc.dwMipMapCount = qdd_log2(m) + 1;
            }
        } else {
            if (desc.dwFlags & DDSD_MIPMAPCOUNT)
                return DDERR_INVALIDCAPS;
            desc.dwMipMapCount = 1;
        }
        DWORD maxdim = desc.dwWidth > desc.dwHeight ? desc.dwWidth : desc.dwHeight;
        if (desc.dwMipMapCount > qdd_log2(maxdim) + 1)
            return DDERR_INVALIDPARAMS;
        desc.dwFlags |= DDSD_MIPMAPCOUNT;
        levels = desc.dwMipMapCount;
    } else if ((desc.dwFlags & DDSD_MIPMAPCOUNT) && !(caps & DDSCAPS_ZBUFFER)) {
        return DDERR_INVALIDCAPS;
    }

    if (!qdd_ck_valid(&desc))
        return DDERR_NOCOLORKEYHW;

    /* Placement. */
    if (dd->hw == QddDraw::QDD_HEL_ONLY && !(caps & DDSCAPS_PRIMARYSURFACE)) {
        /* No hardware: system memory, and the emulation layer's 16/32-bit
         * z-buffers only (stock, ddraw7.c test_caps). */
        if (caps & DDSCAPS_VIDEOMEMORY)
            return DDERR_NODIRECTDRAWHW;
        if (fmt->cls == QFC_DEPTH && fmt->ddpf.dwZBufferBitDepth != 16 && fmt->ddpf.dwZBufferBitDepth != 32)
            return DDERR_INVALIDPIXELFORMAT;
        caps |= DDSCAPS_SYSTEMMEMORY;
    } else if (managed) {
        caps |= DDSCAPS_SYSTEMMEMORY;
    } else if (!(caps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) {
        if ((caps & DDSCAPS_TEXTURE) && !qdd_draw_texture_format(dd, &fmt->ddpf))
            caps |= DDSCAPS_SYSTEMMEMORY;
        else
            caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    } else if (caps & DDSCAPS_VIDEOMEMORY) {
        if ((caps & DDSCAPS_TEXTURE) && !qdd_draw_texture_format(dd, &fmt->ddpf))
            return DDERR_INVALIDPARAMS;
        caps |= DDSCAPS_LOCALVIDMEM;
    }

    /* Client memory. */
    void *client = NULL;
    if (desc.dwFlags & DDSD_LPSURFACE) {
        if (v < 4)
            return DDERR_INVALIDPARAMS; /* DDSURFACEDESC2 only (ddraw2.c test_set_surface_desc) */
        if (caps & DDSCAPS_VIDEOMEMORY)
            return DDERR_INVALIDCAPS;
        if ((caps & DDSCAPS_COMPLEX) || cube)
            return DDERR_INVALIDCAPS;
        if (!desc.lpSurface)
            return DDERR_INVALIDPARAMS;
        if (fmt->cls == QFC_FOURCC_BLOCK) {
            if (version == 7 && (desc.dwFlags & DDSD_PITCH))
                return DDERR_INVALIDPARAMS;
            if (!(desc.dwFlags & (DDSD_LINEARSIZE | DDSD_PITCH)))
                return DDERR_INVALIDPARAMS;
            if ((desc.dwFlags & DDSD_LINEARSIZE) &&
                desc.dwLinearSize < qdd_format_pitch(fmt, desc.dwWidth) * qdd_format_rows(fmt, desc.dwHeight))
                return DDERR_INVALIDPARAMS;
        } else {
            if (!(desc.dwFlags & DDSD_PITCH))
                return DDERR_INVALIDPARAMS;
            DWORD minpitch = (desc.dwWidth * fmt->bpp + 7) / 8;
            if (desc.lPitch <= 0 || (desc.lPitch & 3) || (DWORD)desc.lPitch < minpitch)
                return DDERR_INVALIDPARAMS;
        }
        if (levels > 1 || cube || (caps & DDSCAPS_FLIP))
            return DDERR_INVALIDPARAMS;
        client = desc.lpSurface;
    }

    caps |= (fmt->cls == QFC_PAL) ? 0 : 0;
    desc.ddsCaps.dwCaps = caps;
    desc.ddpfPixelFormat = fmt->ddpf;
    desc.dwFlags |= DDSD_PIXELFORMAT;
    desc.dwFlags &= ~DDSD_ZBUFFERBITDEPTH;
    if (fmt->cls == QFC_DEPTH) /* the legacy depth shares dwMipMapCount */
        desc.dwMipMapCount = 0;
    if (fmt->cls == QFC_FOURCC_BLOCK) {
        desc.dwFlags &= ~DDSD_PITCH;
        desc.dwFlags |= DDSD_LINEARSIZE;
        desc.dwLinearSize = client && (desc.dwFlags & DDSD_LPSURFACE) ? 0xffffffffu
                                                                       : qdd_format_pitch(fmt, desc.dwWidth) *
                                                                             qdd_format_rows(fmt, desc.dwHeight);
    } else {
        desc.dwFlags &= ~DDSD_LINEARSIZE;
        desc.dwFlags |= DDSD_PITCH;
        if (!client)
            desc.lPitch = (LONG)qdd_format_pitch(fmt, desc.dwWidth);
    }
    if (caps & DDSCAPS_VIDEOMEMORY) {
        ULONGLONG total = (ULONGLONG)qdd_format_pitch(fmt, desc.dwWidth) * qdd_format_rows(fmt, desc.dwHeight);
        if (caps & DDSCAPS_FLIP)
            total *= desc.dwBackBufferCount + 1;
        if (levels > 1)
            total = total * 4 / 3;
        if (cube)
            total *= 6;
        if (total > 0xffffffffu || !qdd_vidmem_fits((DWORD)total))
            return DDERR_OUTOFVIDEOMEMORY;
    }

    /* Root. */
    DDSURFACEDESC2 rd = desc;
    if (caps & DDSCAPS_FLIP) {
        rd.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
    } else {
        rd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
        rd.dwBackBufferCount = 0;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE)
        rd.ddsCaps.dwCaps |= DDSCAPS_VISIBLE;
    if (cube) {
        rd.ddsCaps.dwCaps2 &= ~DDSCAPS2_CUBEMAP_ALLFACES;
        rd.ddsCaps.dwCaps2 |= DDSCAPS2_CUBEMAP_POSITIVEX;
    }
    QddSurface *root = qdd_surface_new(dd, version, v, &rd, fmt, client);
    if (!root)
        return DDERR_OUTOFMEMORY;
    root->legacy_z = legacy_z;
    root->levels = levels;
    QddSurface *last = root;
    bool ok = true;

    if (caps & DDSCAPS_FLIP) {
        QddSurface *prev = root;
        for (DWORD i = 1; i <= desc.dwBackBufferCount && ok; ++i) {
            DDSURFACEDESC2 bd = desc;
            bd.ddsCaps.dwCaps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER);
            if (i == 1)
                bd.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.dwBackBufferCount = 0;
            QddSurface *b = qdd_surface_new(dd, version, v, &bd, fmt, NULL);
            if (!b) {
                ok = false;
                break;
            }
            b->root = root;
            b->legacy_z = legacy_z;
            b->levels = 1;
            last->complex_next = b;
            last = b;
            prev->complex_attached[0] = b;
            prev = b;
        }
        if (ok)
            prev->complex_attached[0] = root;
    }

    UINT faces = cube ? 6 : 1;
    for (UINT f = 0; f < faces && ok; ++f) {
        QddSurface *above = NULL;
        for (UINT l = 0; l < levels && ok; ++l) {
            if (f == 0 && l == 0) {
                above = root;
                continue;
            }
            DDSURFACEDESC2 md = desc;
            md.dwWidth = desc.dwWidth >> l ? desc.dwWidth >> l : 1;
            md.dwHeight = desc.dwHeight >> l ? desc.dwHeight >> l : 1;
            md.dwMipMapCount = levels - l;
            if (cube) {
                md.ddsCaps.dwCaps2 &= ~DDSCAPS2_CUBEMAP_ALLFACES;
                md.ddsCaps.dwCaps2 |= DDSCAPS2_CUBEMAP_POSITIVEX << f;
            }
            if (l && version == 7)
                md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            if (fmt->cls == QFC_FOURCC_BLOCK)
                md.dwLinearSize = qdd_format_pitch(fmt, md.dwWidth) * qdd_format_rows(fmt, md.dwHeight);
            else
                md.lPitch = (LONG)qdd_format_pitch(fmt, md.dwWidth);
            QddSurface *m = qdd_surface_new(dd, version, v, &md, fmt, NULL);
            if (!m) {
                ok = false;
                break;
            }
            m->root = root;
            m->face = f;
            m->level = l;
            m->levels = levels - l;
            last->complex_next = m;
            last = m;
            if (l == 0)
                root->complex_attached[5 - f] = m; /* faces 1..5 at 4..0 */
            else if (f == 0 && l == 1)
                root->complex_attached[cube ? 5 : 0] = m;
            else
                above->complex_attached[0] = m;
            above = m;
        }
    }

    if (!ok) {
        root->ref[qdd_vidx(version)] = 0;
        root->ifaces = 0;
        qdd_surface_destroy_tree(root);
        return DDERR_OUTOFMEMORY;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE)
        dd->primary = root;
    *out = root;
    return DD_OK;
}

void qdd_surface_mark_lost(QddDraw *dd, bool primary_only)
{
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next) {
        if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY))
            continue;
        if (primary_only && !(s->root->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
            continue;
        s->lost = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Descriptions                                                              */

/* The description as interface version v reports it (desc_size bytes). */
static void qdd_surface_report(const QddSurface *s, int v, void *out, DWORD out_size)
{
    DDSURFACEDESC2 d = s->desc;
    d.lpSurface = NULL;
    /* Cleared keys keep their value internally (qdd_surface_set_ck) but
     * report zero (ddraw1.c test_ck_operation). */
    if (!(d.dwFlags & DDSD_CKSRCBLT))
        memset(&d.ddckCKSrcBlt, 0, sizeof(d.ddckCKSrcBlt));
    if (!(d.dwFlags & DDSD_CKDESTBLT))
        memset(&d.ddckCKDestBlt, 0, sizeof(d.ddckCKDestBlt));
    if (!(d.dwFlags & DDSD_CKSRCOVERLAY))
        memset(&d.ddckCKSrcOverlay, 0, sizeof(d.ddckCKSrcOverlay));
    if (!(d.dwFlags & DDSD_CKDESTOVERLAY))
        memset(&d.ddckCKDestOverlay, 0, sizeof(d.ddckCKDestOverlay));
    DWORD size = out_size < sizeof(d) ? out_size : sizeof(d);
    DWORD keep = ((DDSURFACEDESC2 *)out)->dwSize;
    memcpy(out, &d, size);
    ((DDSURFACEDESC2 *)out)->dwSize = keep;
    /* Through DDSURFACEDESC a z-buffer reports DDSD_ZBUFFERBITDEPTH, not
     * DDSD_PIXELFORMAT, though the format stays filled in (dsurface.c
     * zbufferbitdepth_test, stock). */
    if (v < 4 && s->fmt->cls == QFC_DEPTH) {
        DDSURFACEDESC *o = (DDSURFACEDESC *)out;
        o->dwFlags &= ~DDSD_PIXELFORMAT;
        o->dwFlags |= DDSD_ZBUFFERBITDEPTH;
        o->dwZBufferBitDepth = s->fmt->ddpf.dwZBufferBitDepth;
    }
}

/* ------------------------------------------------------------------------ */
/* Lock                                                                      */

static bool qdd_lock_desc_size(const QddSurface *s, const DDSURFACEDESC2 *d, DWORD *size)
{
    if (!d)
        return false;
    if (d->dwSize == sizeof(DDSURFACEDESC) || d->dwSize == sizeof(DDSURFACEDESC2)) {
        *size = d->dwSize;
        return true;
    }
    if (s->version == 7 && (s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE) &&
        !(s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)) {
        *size = d->dwSize >= sizeof(DDSURFACEDESC2) ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
        return true;
    }
    return false;
}

static BYTE *qdd_surface_ptr(QddSurface *s, const RECT *r)
{
    BYTE *p = s->buf->mem.ptr;
    if (!r)
        return p;
    const QddFormat *f = s->fmt;
    LONG pitch = s->buf->pitch;
    if (f->cls == QFC_FOURCC_BLOCK)
        return p + (r->top / 4) * pitch + (r->left / 4) * f->block_bytes;
    if (f->cls == QFC_FOURCC_YUV)
        return p + r->top * pitch + (r->left & ~1) * 2;
    return p + r->top * pitch + (r->left * f->bpp) / 8;
}

static HRESULT qdd_surface_lock(QddSurface *s, const RECT *r, DDSURFACEDESC2 *d, DWORD size, DWORD flags)
{
    QddLock lock;
    d->lpSurface = NULL;
    DWORD w = s->desc.dwWidth, h = s->desc.dwHeight;
    /* IDirectDraw7 system-memory and managed textures do not validate the
     * rectangle (stock, ddraw7.c test_lockrect_invalid). */
    bool check = !(s->version == 7 && (s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE) &&
                   !(s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY));
    if (r && check) {
        if (r->left < 0 || r->top < 0 || r->left > r->right || r->top > r->bottom || (DWORD)r->right > w ||
            (DWORD)r->bottom > h)
            return DDERR_INVALIDPARAMS;
        if ((s->fmt->cls == QFC_FOURCC_BLOCK || s->fmt->cls == QFC_FOURCC_YUV) && (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)) {
            UINT bw = s->fmt->cls == QFC_FOURCC_BLOCK ? 4 : 2, bh = s->fmt->cls == QFC_FOURCC_BLOCK ? 4 : 1;
            if ((r->left % bw) || (r->top % bh) || ((r->right % bw) && (DWORD)r->right != w) ||
                ((r->bottom % bh) && (DWORD)r->bottom != h))
                return DDERR_INVALIDPARAMS;
        }
    }
    if (s->lost)
        return DDERR_SURFACELOST;
    if (s->locked || s->dc)
        return DDERR_SURFACEBUSY;
    if (qdd_surf_is_primary(s))
        qdd_primary_read(s, r);
    if (!(flags & DDLOCK_DISCARDCONTENTS) || !(s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)) {
        HRESULT hr = qdd_buf_sys(s);
        if (FAILED(hr))
            return hr;
    } else {
        s->buf->sys_valid = true;
    }
    s->locked = true;
    s->lock_write = !(flags & DDLOCK_READONLY);
    if (r)
        s->lock_rect = *r;
    else
        SetRect(&s->lock_rect, 0, 0, (int)w, (int)h);
    DDSURFACEDESC2 out = s->desc;
    if (s->buf->pitch != s->desc.lPitch && !(s->desc.dwFlags & DDSD_LINEARSIZE))
        out.lPitch = s->buf->pitch;
    out.lpSurface = qdd_surface_ptr(s, r);
    DWORD keep = d->dwSize;
    memcpy(d, &out, size);
    d->dwSize = keep;
    /* A z-buffer locked with DDSURFACEDESC reports neither flag (NVIDIA's
     * behaviour the tests accept). */
    if (size == sizeof(DDSURFACEDESC) && s->fmt->cls == QFC_DEPTH) {
        DDSURFACEDESC *o = (DDSURFACEDESC *)(void *)d;
        o->dwFlags &= ~(DDSD_PIXELFORMAT | DDSD_ZBUFFERBITDEPTH);
        o->dwZBufferBitDepth = 0;
    }
    return DD_OK;
}

static HRESULT qdd_surface_unlock(QddSurface *s)
{
    QddLock lock;
    if (!s->locked)
        return DDERR_NOTLOCKED;
    s->locked = false;
    if (s->lock_write) {
        qdd_buf_sys_written(s);
        if (qdd_surf_is_primary(s))
            qdd_primary_written(s, &s->lock_rect);
    }
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* GetDC / ReleaseDC                                                         */

static DWORD qdd_dib_stride(LONG width, UINT bpp)
{
    return (((DWORD)width * bpp + 31) >> 3) & ~3u;
}

static HRESULT qdd_surface_getdc(QddSurface *s, HDC *out)
{
    QddLock lock;
    if (!out)
        return DDERR_INVALIDPARAMS;
    if (s->dc)
        return DDERR_DCALREADYCREATED;
    if (s->lost)
        return DDERR_SURFACELOST;
    QddBmi bmi;
    QddBuffer *b = s->buf;
    const PALETTEENTRY *pal = qdd_format_is_pal(s->fmt) ? qdd_surf_pal_entries(s) : NULL;
    /* The DC's bitmap: the surface's own DIB when its memory is one, else a
     * surface-sized DIB written back at ReleaseDC. */
    bool direct = b->mem.kind == QDD_MEM_KIND_DIB;
    DWORD stride = qdd_dib_stride((LONG)s->desc.dwWidth, s->fmt->bpp);
    if (!qdd_mem_bmi(s->fmt, direct ? (DWORD)b->pitch : stride, s->desc.dwHeight, &bmi, pal)) {
        *out = NULL;
        return DDERR_CANTCREATEDC;
    }
    if (qdd_surf_is_primary(s))
        qdd_primary_read(s, NULL);
    HRESULT hr = qdd_buf_sys(s);
    if (FAILED(hr))
        return hr;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        *out = NULL;
        return DDERR_CANTCREATEDC;
    }
    HBITMAP dib = NULL;
    void *bits = NULL;
    if (direct) {
        dib = b->mem.dib;
    } else {
        dib = CreateDIBSection(dc, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dib) {
            DWORD row = (s->desc.dwWidth * s->fmt->bpp + 7) / 8;
            for (DWORD y = 0; y < s->desc.dwHeight; ++y)
                memcpy((BYTE *)bits + y * stride, b->mem.ptr + y * b->pitch, row);
        }
    }
    if (!dib) {
        DeleteDC(dc);
        *out = NULL;
        return DDERR_CANTCREATEDC;
    }
    s->dc_old = SelectObject(dc, dib);
    if (s->fmt->cls == QFC_PAL)
        SetDIBColorTable(dc, 0, 1u << s->fmt->bpp, bmi.colors);
    s->dc = dc;
    s->dc_dib = dib;
    s->dc_copy = !direct;
    *out = dc;
    return DD_OK;
}

static HRESULT qdd_surface_releasedc(QddSurface *s, HDC dc)
{
    QddLock lock;
    if (!s->dc)
        return DDERR_NODC;
    if (dc != s->dc)
        return DDERR_INVALIDPARAMS;
    GdiFlush();
    if (s->dc_copy) {
        DIBSECTION ds;
        if (GetObjectA(s->dc_dib, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits) {
            DWORD stride = (DWORD)ds.dsBm.bmWidthBytes;
            DWORD row = (s->desc.dwWidth * s->fmt->bpp + 7) / 8;
            for (DWORD y = 0; y < s->desc.dwHeight; ++y)
                memcpy(s->buf->mem.ptr + y * s->buf->pitch, (BYTE *)ds.dsBm.bmBits + y * stride, row);
        }
    }
    SelectObject(dc, s->dc_old);
    DeleteDC(dc);
    if (s->dc_copy)
        DeleteObject(s->dc_dib);
    s->dc = NULL;
    s->dc_dib = NULL;
    qdd_buf_sys_written(s);
    if (qdd_surf_is_primary(s))
        qdd_primary_written(s, NULL);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Flip                                                                      */

/* Validates a flip of front s (to target, or along the chain) and collects
 * the chain. */
static HRESULT qdd_flip_check(QddSurface *s, QddSurface *target, QddSurface **chain, UINT *count)
{
    if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) || !(s->desc.ddsCaps.dwCaps & DDSCAPS_FLIP))
        return DDERR_NOTFLIPPABLE;
    if (qdd_surf_is_primary(s) && (!s->dd || !qdd_exclusive(s->dd)))
        return DDERR_NOEXCLUSIVEMODE;
    UINT n = 0;
    for (QddSurface *c = s; c && n < 34; c = c->complex_attached[0]) {
        chain[n++] = c;
        if (c->complex_attached[0] == s)
            break;
    }
    if (n < 2)
        return DDERR_NOTFLIPPABLE;
    if (target) {
        bool found = false;
        for (UINT i = 1; i < n && !found; ++i)
            found = chain[i] == target;
        if (!found)
            return DDERR_NOTFLIPPABLE;
    }
    for (UINT i = 0; i < n; ++i) {
        if (chain[i]->lost)
            return DDERR_SURFACELOST;
        if (qdd_surf_busy(chain[i]))
            return DDERR_SURFACEBUSY;
    }
    *count = n;
    return DD_OK;
}

/* A flip of the primary takes effect at the emulated vertical blank: the
 * next flip waits for interval blanks after the previous one (or returns
 * DDERR_WASSTILLDRAWING when the caller does not wait: IDirectDrawSurface7
 * waits unless DDFLIP_DONOTWAIT, the older interfaces only with
 * DDFLIP_WAIT). */
static HRESULT qdd_surface_flip(QddSurface *s, QddSurface *target, DWORD flags, int call_v)
{
    QddSurface *chain[34];
    UINT n = 0;
    qdd_lock();
    HRESULT hr = qdd_flip_check(s, target, chain, &n);
    if (SUCCEEDED(hr) && !(flags & DDFLIP_NOVSYNC) && qdd_surf_is_primary(s)) {
        LONGLONG interval = 1;
        if ((flags & DDFLIP_INTERVAL4) == DDFLIP_INTERVAL4)
            interval = 4;
        else if ((flags & DDFLIP_INTERVAL3) == DDFLIP_INTERVAL3)
            interval = 3;
        else if (flags & DDFLIP_INTERVAL2)
            interval = 2;
        LONGLONG due = s->last_flip + interval;
        bool wait = call_v == 7 ? !(flags & DDFLIP_DONOTWAIT) : (flags & DDFLIP_WAIT) != 0;
        if (qdd_vblank_count() < due) {
            if (!wait) {
                hr = DDERR_WASSTILLDRAWING;
            } else {
                qdd_unlock();
                while (qdd_vblank_count() < due)
                    qdd_vblank_wait();
                qdd_lock();
                hr = qdd_flip_check(s, target, chain, &n);
            }
        }
    }
    if (SUCCEEDED(hr)) {
        if (target) {
            QddBuffer *b = s->buf;
            s->buf = target->buf;
            target->buf = b;
            qdd_surf_touch(target);
        } else {
            QddBuffer *front = chain[0]->buf;
            for (UINT i = 0; i + 1 < n; ++i)
                chain[i]->buf = chain[i + 1]->buf;
            chain[n - 1]->buf = front;
            for (UINT i = 1; i < n; ++i)
                qdd_surf_touch(chain[i]);
        }
        qdd_surf_touch(s);
        s->last_flip = qdd_vblank_count();
        if (qdd_surf_is_primary(s) && s->dd)
            qdd_present(s->dd);
    }
    qdd_unlock();
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Attachments                                                               */

static HRESULT qdd_surface_attach(QddSurface *s, QddSurface *a, int vidx, int call_v)
{
    QddLock lock;
    if (!a || a == s)
        return DDERR_CANNOTATTACHSURFACE;
    DWORD sc = s->desc.ddsCaps.dwCaps, ac = a->desc.ddsCaps.dwCaps;
    if (call_v >= 4) {
        if (!(ac & DDSCAPS_ZBUFFER))
            return DDERR_CANNOTATTACHSURFACE;
        if (!(sc & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE)) && !(sc & DDSCAPS_OFFSCREENPLAIN) &&
            !(sc & DDSCAPS_TEXTURE))
            return DDERR_CANNOTATTACHSURFACE;
        if (a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight)
            return DDERR_CANNOTATTACHSURFACE;
    } else {
        if ((sc & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN)) &&
            (ac & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN))) {
            if (a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight)
                return DDERR_CANNOTATTACHSURFACE;
        } else if (!(sc & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE)) || !(ac & DDSCAPS_ZBUFFER)) {
            return DDERR_CANNOTATTACHSURFACE;
        } else if (a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight) {
            return DDERR_CANNOTATTACHSURFACE;
        }
    }
    for (UINT i = 0; i < s->attached_count; ++i)
        if (s->attached[i].surf == a)
            return DDERR_SURFACEALREADYATTACHED;
    for (UINT i = 0; i < 8; ++i)
        if (s->complex_attached[i] == a)
            return DDERR_SURFACEALREADYATTACHED;
    if (s->attached_count == s->attached_cap) {
        UINT cap = s->attached_cap ? s->attached_cap * 2 : 4;
        QddAttach *n = (QddAttach *)realloc(s->attached, cap * sizeof(QddAttach));
        if (!n)
            return DDERR_OUTOFMEMORY;
        s->attached = n;
        s->attached_cap = cap;
    }
    s->attached[s->attached_count].surf = a;
    s->attached[s->attached_count].vidx = vidx;
    s->attached_count++;
    a->attached_to++;
    qdd_surf_addref(a, vidx);
    return DD_OK;
}

static HRESULT qdd_surface_detach(QddSurface *s, QddSurface *a, int vidx)
{
    QddLock lock;
    if (!a)
        return DDERR_SURFACENOTATTACHED;
    for (UINT i = 0; i < 8; ++i)
        if (s->complex_attached[i] == a)
            return DDERR_CANNOTDETACHSURFACE;
    for (UINT i = 0; i < s->attached_count; ++i) {
        if (s->attached[i].surf != a || s->attached[i].vidx != vidx)
            continue;
        memmove(&s->attached[i], &s->attached[i + 1], (s->attached_count - i - 1) * sizeof(QddAttach));
        s->attached_count--;
        a->attached_to--;
        qdd_surf_release(a, vidx);
        return DD_OK;
    }
    return DDERR_SURFACENOTATTACHED;
}

static QddSurface *qdd_surface_find_attached(QddSurface *s, DWORD caps, DWORD caps2)
{
    for (UINT i = 0; i < 8; ++i) {
        QddSurface *c = s->complex_attached[i];
        if (!c)
            continue;
        if ((c->desc.ddsCaps.dwCaps & caps) == caps && (c->desc.ddsCaps.dwCaps2 & caps2) == caps2)
            return c;
    }
    for (UINT i = 0; i < s->attached_count; ++i) {
        QddSurface *c = s->attached[i].surf;
        if ((c->desc.ddsCaps.dwCaps & caps) == caps && (c->desc.ddsCaps.dwCaps2 & caps2) == caps2)
            return c;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Color keys                                                                */

static HRESULT qdd_surface_set_ck(QddSurface *s, DWORD flags, const DDCOLORKEY *key)
{
    QddLock lock;
    DWORD type = flags & ~(DWORD)DDCKEY_COLORSPACE;
    if (s->version == 7 && (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
        return DDERR_NOTONMIPMAPSUBLEVEL;
    DDCOLORKEY k;
    if (key) {
        k = *key;
        if (flags & DDCKEY_COLORSPACE) {
            if (k.dwColorSpaceLowValue != k.dwColorSpaceHighValue)
                return DDERR_NOCOLORKEYHW;
        } else {
            k.dwColorSpaceHighValue = k.dwColorSpaceLowValue;
        }
    } else {
        k.dwColorSpaceLowValue = k.dwColorSpaceHighValue = 0;
    }
    DDCOLORKEY *slot;
    DWORD flag;
    switch (type) {
    case DDCKEY_DESTOVERLAY: slot = &s->desc.ddckCKDestOverlay; flag = DDSD_CKDESTOVERLAY; break;
    case DDCKEY_DESTBLT: slot = &s->desc.ddckCKDestBlt; flag = DDSD_CKDESTBLT; break;
    case DDCKEY_SRCOVERLAY: slot = &s->desc.ddckCKSrcOverlay; flag = DDSD_CKSRCOVERLAY; break;
    case DDCKEY_SRCBLT: slot = &s->desc.ddckCKSrcBlt; flag = DDSD_CKSRCBLT; break;
    default: return DDERR_INVALIDPARAMS;
    }
    /* Clearing a key keeps its value: DDBLT_KEYDEST through the older
     * interfaces still uses a cleared source key (stock, ddraw1.c
     * test_ck_operation). */
    if (key) {
        *slot = k;
        s->desc.dwFlags |= flag;
    } else {
        s->desc.dwFlags &= ~flag;
    }
    qdd_surf_touch(s);
    return DD_OK;
}

static HRESULT qdd_surface_get_ck(QddSurface *s, DWORD flags, DDCOLORKEY *key)
{
    QddLock lock;
    if (!key)
        return DDERR_INVALIDPARAMS;
    const DDCOLORKEY *slot;
    DWORD flag;
    switch (flags) {
    case DDCKEY_DESTOVERLAY: slot = &s->desc.ddckCKDestOverlay; flag = DDSD_CKDESTOVERLAY; break;
    case DDCKEY_DESTBLT: slot = &s->desc.ddckCKDestBlt; flag = DDSD_CKDESTBLT; break;
    case DDCKEY_SRCOVERLAY: slot = &s->desc.ddckCKSrcOverlay; flag = DDSD_CKSRCOVERLAY; break;
    case DDCKEY_SRCBLT: slot = &s->desc.ddckCKSrcBlt; flag = DDSD_CKSRCBLT; break;
    default: return DDERR_INVALIDPARAMS;
    }
    if (!(s->desc.dwFlags & flag))
        return DDERR_NOCOLORKEY;
    *key = *slot;
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* SetSurfaceDesc                                                            */

static HRESULT qdd_surface_set_desc(QddSurface *s, const DDSURFACEDESC2 *d, DWORD flags)
{
    QddLock lock;
    if (!d)
        return DDERR_INVALIDPARAMS;
    DWORD caps = s->desc.ddsCaps.dwCaps;
    if (!(caps & DDSCAPS_SYSTEMMEMORY) || (caps & DDSCAPS_PRIMARYSURFACE) ||
        (s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        return DDERR_INVALIDSURFACETYPE;
    if (flags)
        return DDERR_INVALIDPARAMS;
    DWORD f = d->dwFlags;
    if (f & ~(DWORD)(DDSD_LPSURFACE | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS))
        return DDERR_INVALIDPARAMS;
    if (!(f & DDSD_LPSURFACE) || !d->lpSurface)
        return DDERR_INVALIDPARAMS;
    if ((f & DDSD_CAPS) && d->ddsCaps.dwCaps)
        return DDERR_INVALIDCAPS;
    if (((f & DDSD_WIDTH) != 0) != ((f & DDSD_PITCH) != 0))
        return DDERR_INVALIDPARAMS;
    if ((f & DDSD_WIDTH) && (!d->dwWidth || d->lPitch <= 0 || (d->lPitch & 3)))
        return DDERR_INVALIDPARAMS;
    if ((f & DDSD_HEIGHT) && !d->dwHeight)
        return DDERR_INVALIDPARAMS;
    const QddFormat *fmt = s->fmt;
    if (f & DDSD_PIXELFORMAT) {
        fmt = qdd_format_from_ddpf(&d->ddpfPixelFormat);
        if (!fmt || fmt->cls == QFC_DEPTH)
            return DDERR_INVALIDPIXELFORMAT;
    }
    if (s->locked || s->dc)
        return DDERR_SURFACEBUSY;
    QddBuffer *b = s->buf;
    qdd_pass_buffer_free(b);
    if (b->gpu || b->gpu_tex || b->stage) {
        if (b->gpu)
            b->gpu->Release();
        if (b->gpu_tex)
            b->gpu_tex->Release();
        if (b->stage)
            b->stage->Release();
        b->gpu = NULL;
        b->gpu_tex = NULL;
        b->stage = NULL;
    }
    qdd_mem_free(&b->mem);
    b->mem.ptr = (BYTE *)d->lpSurface;
    if (f & DDSD_WIDTH) {
        s->desc.dwWidth = d->dwWidth;
        b->pitch = d->lPitch;
        s->desc.lPitch = d->lPitch;
    }
    if (f & DDSD_HEIGHT)
        s->desc.dwHeight = d->dwHeight;
    if (f & DDSD_PIXELFORMAT) {
        s->fmt = fmt;
        s->desc.ddpfPixelFormat = fmt->ddpf;
        if (!(f & DDSD_WIDTH)) {
            b->pitch = (LONG)qdd_format_pitch(fmt, s->desc.dwWidth);
            s->desc.lPitch = b->pitch;
        }
    }
    b->mem.size = (DWORD)b->pitch * qdd_format_rows(s->fmt, s->desc.dwHeight);
    b->sys_valid = true;
    b->gpu_valid = false;
    qdd_surf_touch(s);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Private data                                                              */

static QddPrivate *qdd_priv_find(QddSurface *s, REFGUID tag)
{
    for (QddPrivate *p = s->priv; p; p = p->next)
        if (IsEqualGUID(p->tag, tag))
            return p;
    return NULL;
}

static void qdd_priv_free(QddSurface *s, QddPrivate *p)
{
    QddPrivate **pp = &s->priv;
    while (*pp != p)
        pp = &(*pp)->next;
    *pp = p->next;
    if (p->flags & DDSPD_IUNKNOWNPOINTER)
        ((IUnknown *)p->data)->Release();
    else
        free(p->data);
    free(p);
}

static HRESULT qdd_priv_set(QddSurface *s, REFGUID tag, void *data, DWORD size, DWORD flags)
{
    QddLock lock;
    if (!data)
        return DDERR_INVALIDPARAMS;
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        if (!data || size != sizeof(IUnknown *))
            return DDERR_INVALIDPARAMS;
    }
    QddPrivate *p = (QddPrivate *)calloc(1, sizeof(QddPrivate));
    if (!p)
        return DDERR_OUTOFMEMORY;
    p->tag = tag;
    p->flags = flags;
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        p->data = data;
        p->size = sizeof(IUnknown *);
        ((IUnknown *)data)->AddRef();
    } else {
        p->data = malloc(size ? size : 1);
        if (!p->data) {
            free(p);
            return DDERR_OUTOFMEMORY;
        }
        memcpy(p->data, data, size);
        p->size = size;
    }
    QddPrivate *old = qdd_priv_find(s, tag);
    if (old)
        qdd_priv_free(s, old);
    p->next = s->priv;
    s->priv = p;
    p->version_set = s->version_counter;
    return DD_OK;
}

static HRESULT qdd_priv_get(QddSurface *s, REFGUID tag, void *data, DWORD *size)
{
    QddLock lock;
    QddPrivate *p = qdd_priv_find(s, tag);
    if (!p)
        return DDERR_NOTFOUND;
    if (!size)
        return DDERR_INVALIDPARAMS;
    if ((p->flags & DDSPD_VOLATILE) && p->version_set != s->version_counter)
        return DDERR_EXPIRED;
    if (*size < p->size) {
        *size = p->size;
        return DDERR_MOREDATA;
    }
    if (!data)
        return DDERR_INVALIDPARAMS;
    *size = p->size;
    if (p->flags & DDSPD_IUNKNOWNPOINTER)
        *(void **)data = p->data;
    else
        memcpy(data, p->data, p->size);
    return DD_OK;
}

static HRESULT qdd_priv_del(QddSurface *s, REFGUID tag)
{
    QddLock lock;
    QddPrivate *p = qdd_priv_find(s, tag);
    if (!p)
        return DDERR_NOTFOUND;
    qdd_priv_free(s, p);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Interface methods                                                         */

template <int V> HRESULT QddSurfIface<V>::QueryInterface(REFIID riid, void **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (qdd_iid_null(&riid))
        return DDERR_INVALIDPARAMS;
    int v = 0;
    if (IsEqualGUID(riid, IID_IDirectDrawSurface7))
        v = 7;
    else if (IsEqualGUID(riid, IID_IDirectDrawSurface4))
        v = 4;
    else if (IsEqualGUID(riid, IID_IDirectDrawSurface3))
        v = 3;
    else if (IsEqualGUID(riid, IID_IDirectDrawSurface2))
        v = 2;
    else if (IsEqualGUID(riid, IID_IDirectDrawSurface) || IsEqualGUID(riid, IID_IUnknown))
        v = 1;
    if (v) {
        qdd_surf_addref(s, qdd_vidx(v));
        *out = qdd_surf_iface(s, v);
        return S_OK;
    }
    if (IsEqualGUID(riid, QDD_IID_IDirectDrawGammaControl)) {
        qdd_surf_addref(s, 5);
        *out = static_cast<IDirectDrawGammaControl *>(&s->ifg);
        return S_OK;
    }
    if (s->version != 7 && qdd_d3d_surface_query) {
        /* The Direct3D hook answers E_NOINTERFACE for interfaces it does not
         * implement; any other failure (a device GUID on a surface that
         * cannot hold that device, ddraw1.c test_rt_caps) is the answer. */
        QddLock lock;
        HRESULT hr = qdd_d3d_surface_query(s, riid, out);
        if (SUCCEEDED(hr))
            return hr;
        *out = NULL;
        if (hr != E_NOINTERFACE)
            return hr;
    }
    /* No color controls (overlays only): E_NOINTERFACE on every version. */
    if (IsEqualGUID(riid, IID_IDirectDrawColorControl))
        return E_NOINTERFACE;
    /* Native: once Direct3D is loaded, surfaces of IDirectDraw/2/4 answer
     * E_INVALIDARG for other interfaces (Wine's surface QI tests); before,
     * and for IDirectDraw7 surfaces, E_NOINTERFACE (the stock runtime). */
    if (s->version != 7 && g_qdd_d3d_loaded)
        return E_INVALIDARG;
    return E_NOINTERFACE;
}

template <int V> ULONG QddSurfIface<V>::AddRef()
{
    return qdd_surf_addref(s, qdd_vidx(V));
}

template <int V> ULONG QddSurfIface<V>::Release()
{
    return qdd_surf_release(s, qdd_vidx(V));
}

template <int V> HRESULT QddSurfIface<V>::AddAttachedSurface(I *att)
{
    int vidx;
    QddSurface *a = (QddSurface *)qdd_reg_find(att, QDD_T_SURFACE, &vidx);
    if (!a)
        return DDERR_INVALIDPARAMS;
    return qdd_surface_attach(s, a, vidx, V);
}

template <int V> HRESULT QddSurfIface<V>::AddOverlayDirtyRect(RECT *r)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::Blt(RECT *dr, I *src, RECT *sr, DWORD flags, DDBLTFX *fx)
{
    QddSurface *ss = NULL;
    if (src && !(ss = qdd_surf_from(src)))
        return DDERR_INVALIDPARAMS;
    return qdd_blt(s, dr, ss, sr, flags, fx, V);
}

template <int V> HRESULT QddSurfIface<V>::BltBatch(DDBLTBATCH *batch, DWORD count, DWORD flags)
{
    return DDERR_UNSUPPORTED;
}

template <int V> HRESULT QddSurfIface<V>::BltFast(DWORD x, DWORD y, I *src, RECT *sr, DWORD trans)
{
    QddSurface *ss = qdd_surf_from(src);
    if (!ss)
        return DDERR_INVALIDPARAMS;
    return qdd_bltfast(s, x, y, ss, sr, trans, V);
}

template <int V> HRESULT QddSurfIface<V>::DeleteAttachedSurface(DWORD flags, I *att)
{
    int vidx = 0;
    QddSurface *a = att ? (QddSurface *)qdd_reg_find(att, QDD_T_SURFACE, &vidx) : NULL;
    return qdd_surface_detach(s, a, vidx);
}

template <int V> HRESULT QddSurfIface<V>::EnumAttachedSurfaces(void *ctx, typename T::EnumCb cb)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    QddSurface *list[8 + 64];
    UINT n = 0;
    for (UINT i = 0; i < 8; ++i)
        if (s->complex_attached[i])
            list[n++] = s->complex_attached[i];
    for (UINT i = 0; i < s->attached_count && n < 72; ++i)
        list[n++] = s->attached[i].surf;
    for (UINT i = 0; i < n; ++i) {
        QddSurface *c = list[i];
        qdd_surf_addref(c, qdd_vidx(V));
        typename T::Desc d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        qdd_surface_report(c, V, &d, sizeof(d));
        /* The older callbacks are declared with IDirectDrawSurface; native
         * passes the interface the method was called through. */
        if (cb((typename T::CbSurf *)qdd_surf_iface(c, V), &d, ctx) == DDENUMRET_CANCEL)
            break;
    }
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::EnumOverlayZOrders(DWORD flags, void *ctx, typename T::EnumCb cb)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::Flip(I *target, DWORD flags)
{
    QddSurface *t = NULL;
    if (target && !(t = qdd_surf_from(target)))
        return DDERR_INVALIDPARAMS;
    return qdd_surface_flip(s, t, flags, V);
}

template <int V> HRESULT QddSurfIface<V>::GetAttachedSurface(typename T::Caps *caps, I **out)
{
    if (!caps || !out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    DWORD c2 = 0;
    if (V >= 4 && s->version == 7)
        c2 = ((const DDSCAPS2 *)(const void *)caps)->dwCaps2;
    QddSurface *a = qdd_surface_find_attached(s, caps->dwCaps, c2);
    if (!a) {
        *out = NULL;
        return DDERR_NOTFOUND;
    }
    qdd_surf_addref(a, qdd_vidx(V));
    *out = (I *)qdd_surf_iface(a, V);
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetBltStatus(DWORD flags)
{
    if (flags != DDGBS_CANBLT && flags != DDGBS_ISBLTDONE)
        return DDERR_INVALIDPARAMS;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetCaps(typename T::Caps *caps)
{
    if (!caps)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    memcpy(caps, &s->desc.ddsCaps, sizeof(*caps));
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetClipper(IDirectDrawClipper **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!s->clipper) {
        *out = NULL;
        return DDERR_NOCLIPPERATTACHED;
    }
    qdd_clipper_addref(s->clipper);
    *out = &s->clipper->iface;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetColorKey(DWORD flags, DDCOLORKEY *key)
{
    return qdd_surface_get_ck(s, flags, key);
}

template <int V> HRESULT QddSurfIface<V>::GetDC(HDC *dc)
{
    return qdd_surface_getdc(s, dc);
}

template <int V> HRESULT QddSurfIface<V>::GetFlipStatus(DWORD flags)
{
    if (flags != DDGFS_CANFLIP && flags != DDGFS_ISFLIPDONE)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_FLIP))
        return DDERR_NOTFLIPPABLE;
    if (flags == DDGFS_ISFLIPDONE && s->root->last_flip && qdd_vblank_count() <= s->root->last_flip)
        return DDERR_WASSTILLDRAWING;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetOverlayPosition(LONG *x, LONG *y)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::GetPalette(IDirectDrawPalette **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!s->palette) {
        *out = NULL;
        return DDERR_NOPALETTEATTACHED;
    }
    s->palette->iface.AddRef();
    *out = &s->palette->iface;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetPixelFormat(DDPIXELFORMAT *pf)
{
    if (!pf)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    *pf = s->desc.ddpfPixelFormat;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetSurfaceDesc(typename T::Desc *desc)
{
    if (!desc || desc->dwSize != sizeof(typename T::Desc))
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    qdd_surface_report(s, V, desc, sizeof(typename T::Desc));
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::Initialize(IDirectDraw *ddraw, typename T::Desc *desc)
{
    return DDERR_ALREADYINITIALIZED;
}

template <int V> HRESULT QddSurfIface<V>::IsLost()
{
    QddLock lock;
    return s->lost ? DDERR_SURFACELOST : DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::Lock(RECT *r, typename T::Desc *desc, DWORD flags, HANDLE event)
{
    DWORD size;
    if (!qdd_lock_desc_size(s, (DDSURFACEDESC2 *)(void *)desc, &size))
        return DDERR_INVALIDPARAMS;
    return qdd_surface_lock(s, r, (DDSURFACEDESC2 *)(void *)desc, size, flags);
}

template <int V> HRESULT QddSurfIface<V>::ReleaseDC(HDC dc)
{
    return qdd_surface_releasedc(s, dc);
}

template <int V> HRESULT QddSurfIface<V>::Restore()
{
    QddLock lock;
    if (s->dd) {
        QddDraw *ex = qdd_exclusive_owner();
        if (ex && ex != s->dd && qdd_surf_is_primary(s))
            return DDERR_WRONGMODE;
    }
    s->lost = false;
    for (UINT i = 0; i < 8; ++i)
        if (s->complex_attached[i])
            s->complex_attached[i]->lost = false;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::SetClipper(IDirectDrawClipper *clipper)
{
    QddLock lock;
    QddClipper *c = NULL;
    if (clipper && !(c = qdd_clipper_from(clipper)))
        return DDERR_INVALIDPARAMS;
    if (c == s->clipper)
        return DD_OK;
    if (c)
        qdd_clipper_addref(c);
    if (s->clipper)
        qdd_clipper_release(s->clipper);
    s->clipper = c;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::SetColorKey(DWORD flags, DDCOLORKEY *key)
{
    return qdd_surface_set_ck(s, flags, key);
}

template <int V> HRESULT QddSurfIface<V>::SetOverlayPosition(LONG x, LONG y)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::SetPalette(IDirectDrawPalette *pal)
{
    QddLock lock;
    if (s->version == 7 && (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL))
        return DDERR_NOTONMIPMAPSUBLEVEL;
    QddPalette *p = NULL;
    if (pal) {
        if (!(p = qdd_palette_from(pal)))
            return DDERR_INVALIDPARAMS;
        if (s->fmt->cls != QFC_PAL)
            return DDERR_INVALIDPIXELFORMAT;
    }
    if (p == s->palette)
        return DD_OK;
    if (p)
        p->iface.AddRef();
    if (s->palette)
        s->palette->iface.Release();
    s->palette = p;
    qdd_surf_touch(s);
    if (qdd_surf_is_primary(s))
        qdd_primary_written(s, NULL);
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::Unlock(typename T::UnlockArg r)
{
    return qdd_surface_unlock(s);
}

template <int V>
HRESULT QddSurfIface<V>::UpdateOverlay(RECT *sr, I *dst, RECT *dr, DWORD flags, DDOVERLAYFX *fx)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::UpdateOverlayDisplay(DWORD flags)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::UpdateOverlayZOrder(DWORD flags, I *ref)
{
    return DDERR_NOTAOVERLAYSURFACE;
}

template <int V> HRESULT QddSurfIface<V>::GetDDInterface(void **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!s->dd) {
        *out = NULL;
        return DDERR_INVALIDOBJECT;
    }
    *out = qdd_draw_iface(s->dd, s->creator_v);
    qdd_draw_addref(s->dd, s->creator_v);
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::PageLock(DWORD flags)
{
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::PageUnlock(DWORD flags)
{
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::SetSurfaceDesc(typename T::Desc *desc, DWORD flags)
{
    if (!desc)
        return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 d2;
    if (V >= 4) {
        memcpy(&d2, desc, sizeof(d2));
    } else {
        memset(&d2, 0, sizeof(d2));
        memcpy(&d2, desc, sizeof(DDSURFACEDESC));
    }
    return qdd_surface_set_desc(s, &d2, flags);
}

template <int V> HRESULT QddSurfIface<V>::SetPrivateData(REFGUID tag, void *data, DWORD size, DWORD flags)
{
    return qdd_priv_set(s, tag, data, size, flags);
}

template <int V> HRESULT QddSurfIface<V>::GetPrivateData(REFGUID tag, void *data, DWORD *size)
{
    return qdd_priv_get(s, tag, data, size);
}

template <int V> HRESULT QddSurfIface<V>::FreePrivateData(REFGUID tag)
{
    return qdd_priv_del(s, tag);
}

template <int V> HRESULT QddSurfIface<V>::GetUniquenessValue(DWORD *value)
{
    if (!value)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    *value = s->version_counter;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::ChangeUniquenessValue()
{
    QddLock lock;
    qdd_surf_touch(s);
    return DD_OK;
}

static HRESULT qdd_priority_check(QddSurface *s)
{
    if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE))
        return DDERR_INVALIDOBJECT;
    if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) || s->root != s)
        return DDERR_INVALIDPARAMS;
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::SetPriority(DWORD prio)
{
    QddLock lock;
    HRESULT hr = qdd_priority_check(s);
    if (SUCCEEDED(hr))
        s->priority = prio;
    return hr;
}

template <int V> HRESULT QddSurfIface<V>::GetPriority(DWORD *prio)
{
    QddLock lock;
    HRESULT hr = qdd_priority_check(s);
    if (SUCCEEDED(hr)) {
        if (!prio)
            return DDERR_INVALIDPARAMS;
        *prio = s->priority;
    }
    return hr;
}

template <int V> HRESULT QddSurfIface<V>::SetLOD(DWORD lod)
{
    QddLock lock;
    if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        return DDERR_INVALIDOBJECT;
    if (lod >= s->levels)
        return DDERR_INVALIDPARAMS;
    s->lod = lod;
    qdd_surf_touch(s);
    return DD_OK;
}

template <int V> HRESULT QddSurfIface<V>::GetLOD(DWORD *lod)
{
    QddLock lock;
    if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        return DDERR_INVALIDOBJECT;
    if (!lod)
        return DDERR_INVALIDPARAMS;
    *lod = s->lod;
    return DD_OK;
}

template class QddSurfIface<1>;
template class QddSurfIface<2>;
template class QddSurfIface<3>;
template class QddSurfIface<4>;
template class QddSurfIface<7>;

/* ------------------------------------------------------------------------ */
/* Gamma control                                                             */

HRESULT QddGammaIface::QueryInterface(REFIID riid, void **out)
{
    return s->if7.QueryInterface(riid, out);
}

ULONG QddGammaIface::AddRef()
{
    return qdd_surf_addref(s, 5);
}

ULONG QddGammaIface::Release()
{
    return qdd_surf_release(s, 5);
}

HRESULT QddGammaIface::GetGammaRamp(DWORD flags, DDGAMMARAMP *ramp)
{
    if (!ramp)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (s->dd && s->dd->gamma_set) {
        *ramp = s->dd->gamma;
    } else {
        for (UINT i = 0; i < 256; ++i)
            ramp->red[i] = ramp->green[i] = ramp->blue[i] = (WORD)(i * 0x101);
    }
    return DD_OK;
}

HRESULT QddGammaIface::SetGammaRamp(DWORD flags, DDGAMMARAMP *ramp)
{
    if (!ramp)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!qdd_surf_is_primary(s) || !s->dd)
        return DD_OK;
    s->dd->gamma = *ramp;
    s->dd->gamma_set = true;
    s->dd->gamma_version++;
    qdd_primary_written(s, NULL);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* qdd.h accessors                                                           */

QddSurfaceQueryHook qdd_d3d_surface_query;

QddSurface *qdd_surface_lookup(const void *p) { return qdd_surf_from(p); }
void *qdd_surface_iface(QddSurface *s, int version) { return qdd_surf_iface(s, version); }
int qdd_surface_create_version(QddSurface *s) { return s->creator_v; }
IDirectDrawSurface7 *qdd_surface_iface7(QddSurface *s) { return &s->if7; }
QddDraw *qdd_surface_draw(QddSurface *s) { return s->dd; }
const DDSURFACEDESC2 *qdd_surface_desc(QddSurface *s) { return &s->desc; }
bool qdd_surface_lost(QddSurface *s) { return s->lost; }
void **qdd_surface_d3d7_slot(QddSurface *s) { return &s->d3d7; }
DWORD qdd_surface_version(QddSurface *s) { return s->version_counter; }

QddSurface *qdd_surface_attached(QddSurface *s, DWORD caps, DWORD caps2)
{
    return qdd_surface_find_attached(s, caps, caps2);
}

UINT qdd_surface_levels(QddSurface *root)
{
    return root->levels ? root->levels : 1;
}

QddSurface *qdd_surface_sub(QddSurface *root, UINT face, UINT level)
{
    QddSurface *s = root;
    if (face) {
        if (face > 5 || !(root->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) || root->root != root)
            return NULL;
        s = root->complex_attached[5 - face];
        if (!s)
            return NULL;
    }
    for (UINT l = 0; l < level && s; ++l) {
        QddSurface *next = NULL;
        if (s == s->root && (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP))
            next = s->complex_attached[5];
        else if (s->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP)
            next = s->complex_attached[0];
        s = next;
    }
    return s;
}

bool qdd_surface_palette(QddSurface *s, PALETTEENTRY out[256])
{
    if (!s->palette)
        return false;
    memset(out, 0, 256 * sizeof(PALETTEENTRY));
    bool alpha = (s->palette->caps & DDPCAPS_ALPHA) != 0;
    for (UINT i = 0; i < s->palette->count; ++i) {
        out[i] = s->palette->entries[i];
        if (!alpha)
            out[i].peFlags = 0xff;
    }
    return true;
}

HRESULT qdd_surface_map(QddSurface *s, bool write, BYTE **bits, LONG *pitch)
{
    HRESULT hr = qdd_buf_sys(s);
    if (FAILED(hr))
        return hr;
    *bits = s->buf->mem.ptr;
    *pitch = s->buf->pitch;
    s->map_write = write;
    return DD_OK;
}

void qdd_surface_unmap(QddSurface *s)
{
    if (s->map_write) {
        s->map_write = false;
        qdd_buf_sys_written(s);
    }
}
