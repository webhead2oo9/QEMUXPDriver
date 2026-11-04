/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: the Direct3D 9 side: the shared device of a DirectDraw object, the
 * Direct3D 9 copies of surfaces with their transfers, and presentation.
 *
 * Presentation rules (docs/design/ddraw-remoting.md).  The VM shows QD3D
 * frames as the whole screen, so:
 *  - a process without a Direct3D device whose DirectDraw object is not in
 *    exclusive mode presents through GDI: writes to the primary go to the
 *    screen (clipped by the primary's clipper for Blt), reads of the
 *    primary come from the screen; no QD3D frames;
 *  - exclusive mode, or any Direct3D device in the process: the primary
 *    presents through QD3D: Flip presents the new front buffer at once;
 *    other writes to the primary (Blt, Unlock, ReleaseDC, palette changes)
 *    mark it dirty and a presenter thread presents it at the next emulated
 *    vertical blank.  The frame is the front buffer (P8 converted through
 *    its palette, 16/24-bit converted by StretchRect or on upload), the
 *    whole primary in exclusive mode, the cooperative/clipper window's
 *    client area of it when windowed.
 */
#include "qdd_priv.h"

/* ------------------------------------------------------------------------ */
/* Device                                                                    */

static const char g_hidden_class[] = "QDDrawHiddenWnd";

static HWND qdd_hidden_window(void)
{
    static bool registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = g_qdd_self;
        wc.lpszClassName = g_hidden_class;
        RegisterClassA(&wc);
        registered = true;
    }
    return CreateWindowExA(0, g_hidden_class, "qddraw", WS_POPUP, 0, 0, 16, 16, NULL, NULL, g_qdd_self, NULL);
}

/* The presentation rectangle of dd's primary (primary coordinates). */
static void qdd_present_rect(QddDraw *dd, RECT *r)
{
    QddSurface *p = dd->primary;
    LONG w = p ? (LONG)p->desc.dwWidth : 0, h = p ? (LONG)p->desc.dwHeight : 0;
    if (!p) {
        QddMode m;
        qdd_mode_current(&m);
        w = (LONG)m.w;
        h = (LONG)m.h;
    }
    SetRect(r, 0, 0, w, h);
    if (qdd_exclusive(dd))
        return;
    HWND win = (p && p->clipper && p->clipper->window) ? p->clipper->window : dd->window;
    RECT c;
    if (!win || !GetClientRect(win, &c) || c.right <= 0 || c.bottom <= 0)
        return;
    MapWindowPoints(win, NULL, (POINT *)&c, 2);
    RECT full = *r;
    if (IntersectRect(&c, &c, &full))
        *r = c;
}

static bool qdd_dev_create(QddDraw *dd, UINT w, UINT h)
{
    if (dd->dev9)
        return true;
    if (dd->dev9_failed || !dd->d3d9)
        return false;
    HWND win = dd->window ? dd->window : dd->focus_window;
    if (!win) {
        if (!dd->hidden_window)
            dd->hidden_window = qdd_hidden_window();
        win = dd->hidden_window;
    }
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = w ? w : 1;
    pp.BackBufferHeight = h ? h : 1;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = win;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    static const DWORD behaviors[] = {
        D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
    };
    HRESULT hr = E_FAIL;
    for (UINT i = 0; i < 3 && !dd->dev9; ++i) {
        D3DPRESENT_PARAMETERS p = pp;
        hr = dd->d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win, behaviors[i], &p, &dd->dev9);
        if (FAILED(hr))
            dd->dev9 = NULL;
    }
    if (!dd->dev9) {
        dd->dev9_failed = true;
        qdd_event("qddraw: Direct3D 9 device creation failed, hr 0x%08lx", (unsigned long)hr);
        return false;
    }
    dd->bb_w = pp.BackBufferWidth;
    dd->bb_h = pp.BackBufferHeight;
    return true;
}

static bool qdd_dev_ensure(QddDraw *dd, UINT w, UINT h)
{
    if (!dd->dev9)
        return qdd_dev_create(dd, w, h);
    if (dd->bb_w == w && dd->bb_h == h)
        return true;
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dd->window ? dd->window : (dd->focus_window ? dd->focus_window : dd->hidden_window);
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    qdd_d3d7_flush(dd);
    qdd_d3d7_before_reset(dd);
    HRESULT hr = dd->dev9->Reset(&pp);
    qdd_d3d7_after_reset(dd);
    if (FAILED(hr)) {
        static LONG budget = 4;
        if (InterlockedDecrement(&budget) >= 0)
            qdd_event("qddraw: Reset to %ux%u failed, hr 0x%08lx", w, h, (unsigned long)hr);
        return false;
    }
    dd->bb_w = w;
    dd->bb_h = h;
    return true;
}

IDirect3DDevice9 *qdd_draw_device9(QddDraw *dd)
{
    QddLock lock;
    RECT r;
    qdd_present_rect(dd, &r);
    if (!qdd_dev_create(dd, (UINT)(r.right - r.left), (UINT)(r.bottom - r.top)))
        return NULL;
    return dd->dev9;
}

/* ------------------------------------------------------------------------ */
/* Direct3D 9 copies                                                         */

static bool qdd_check(QddDraw *dd, DWORD usage, D3DRESOURCETYPE type, D3DFORMAT f)
{
    return SUCCEEDED(dd->d3d9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, type, f));
}

#define QDD_FMT_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))

static D3DFORMAT qdd_gpu_format(QddDraw *dd, const QddFormat *f)
{
    if (f->cls == QFC_DEPTH) {
        /* Copies the CPU can reach (qdd_pass.cpp): lockable without
         * stencil, an INTZ texture with it; else a plain depth stencil
         * whose contents never transfer. */
        bool stencil = f->mask_a != 0;
        if (!stencil && f->bpp == 16 && qdd_check(dd, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D16_LOCKABLE))
            return D3DFMT_D16_LOCKABLE;
        if (!stencil && qdd_check(dd, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D32F_LOCKABLE))
            return D3DFMT_D32F_LOCKABLE;
        if (qdd_check(dd, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, QDD_FMT_INTZ))
            return QDD_FMT_INTZ;
        if (f->d3d != D3DFMT_UNKNOWN && qdd_check(dd, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f->d3d))
            return f->d3d;
        if (f->bpp == 16 && !stencil && qdd_check(dd, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D16))
            return D3DFMT_D16;
        return stencil ? D3DFMT_D24S8 : D3DFMT_D24X8;
    }
    if (f->cls == QFC_RGB && f->d3d != D3DFMT_UNKNOWN && f->bpp >= 16 && f->bpp != 24 &&
        qdd_check(dd, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, f->d3d))
        return f->d3d;
    if (f->cls == QFC_RGB || f->cls == QFC_PAL || f->cls == QFC_LUM)
        return f->mask_a ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    return D3DFMT_UNKNOWN;
}

static bool qdd_gpu_create(QddSurface *s)
{
    QddDraw *dd = s->dd;
    QddBuffer *b = s->buf;
    D3DFORMAT f = qdd_gpu_format(dd, s->fmt);
    if (f == D3DFMT_UNKNOWN)
        return false;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    HRESULT hr;
    if (f == QDD_FMT_INTZ) {
        hr = dd->dev9->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, f, D3DPOOL_DEFAULT, &b->gpu_tex, NULL);
        if (SUCCEEDED(hr)) {
            hr = b->gpu_tex->GetSurfaceLevel(0, &b->gpu);
            if (FAILED(hr)) {
                b->gpu_tex->Release();
                b->gpu_tex = NULL;
            }
        }
    } else if (s->fmt->cls == QFC_DEPTH) {
        hr = dd->dev9->CreateDepthStencilSurface(w, h, f, D3DMULTISAMPLE_NONE, 0, FALSE, &b->gpu, NULL);
    } else if ((s->desc.ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) == (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) {
        hr = dd->dev9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &b->gpu_tex, NULL);
        if (SUCCEEDED(hr)) {
            hr = b->gpu_tex->GetSurfaceLevel(0, &b->gpu);
            if (FAILED(hr)) {
                b->gpu_tex->Release();
                b->gpu_tex = NULL;
            }
        }
    } else {
        hr = dd->dev9->CreateRenderTarget(w, h, f, D3DMULTISAMPLE_NONE, 0, FALSE, &b->gpu, NULL);
    }
    if (FAILED(hr)) {
        b->gpu = NULL;
        static LONG budget = 8;
        if (InterlockedDecrement(&budget) >= 0)
            qdd_event("qddraw: creating a %ux%u Direct3D 9 copy (format %u) failed, hr 0x%08lx", w, h, (unsigned)f,
                      (unsigned long)hr);
        return false;
    }
    b->gpu_fmt = f;
    b->gpu_valid = false;
    return true;
}

static bool qdd_stage(QddSurface *s)
{
    QddBuffer *b = s->buf;
    if (b->stage)
        return true;
    HRESULT hr = s->dd->dev9->CreateOffscreenPlainSurface(s->desc.dwWidth, s->desc.dwHeight, b->gpu_fmt,
                                                          D3DPOOL_SYSTEMMEM, &b->stage, NULL);
    if (FAILED(hr)) {
        b->stage = NULL;
        return false;
    }
    return true;
}

static bool qdd_gamma_identity(const QddDraw *dd)
{
    if (!dd->gamma_set)
        return true;
    for (UINT i = 0; i < 256; ++i)
        if ((dd->gamma.red[i] >> 8) != i || (dd->gamma.green[i] >> 8) != i || (dd->gamma.blue[i] >> 8) != i)
            return false;
    return true;
}

/* System memory -> Direct3D 9 copy. */
static void qdd_upload(QddSurface *s)
{
    QddBuffer *b = s->buf;
    QddXferTrace tr(s->fmt->cls == QFC_DEPTH ? QDD_TX_DEPTH_UP : QDD_TX_UPLOAD,
                    s->desc.dwWidth * s->desc.dwHeight * (s->fmt->bpp > 8 ? s->fmt->bpp / 8 : 1));
    if (s->fmt->cls == QFC_DEPTH) {
        qdd_depth_upload(s); /* without a transfer path the contents start undefined */
        b->gpu_valid = true;
        return;
    }
    if (s->fmt->cls == QFC_PAL && s->fmt->bpp == 8 && qdd_p8_upload(s, !b->gpu_valid)) {
        b->gpu_valid = true;
        b->gpu_pal_version = s->palette ? s->palette->version : 0;
        return;
    }
    if (!qdd_stage(s)) {
        b->gpu_valid = true;
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(b->stage->LockRect(&lr, NULL, 0))) {
        b->gpu_valid = true;
        return;
    }
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    const QddFormat *gf = qdd_format_d3d(b->gpu_fmt);
    const PALETTEENTRY *pal = qdd_format_is_pal(s->fmt) ? qdd_surf_pal_entries(s) : NULL;
    bool same = gf == s->fmt;
    for (UINT y = 0; y < h; ++y) {
        BYTE *d = (BYTE *)lr.pBits + (size_t)y * lr.Pitch;
        const BYTE *src = b->mem.ptr + (size_t)y * b->pitch;
        if (same)
            memcpy(d, src, (size_t)w * qdd_format_bytes(gf));
        else if (gf)
            qdd_convert_row(gf, d, s->fmt, src, w, pal);
    }
    b->stage->UnlockRect();
    s->dd->dev9->UpdateSurface(b->stage, NULL, b->gpu, NULL);
    b->gpu_valid = true;
    b->gpu_pal_version = s->palette ? s->palette->version : 0;
}

/* Direct3D 9 copy -> system memory. */
static void qdd_readback(QddSurface *s)
{
    QddBuffer *b = s->buf;
    QddXferTrace tr(s->fmt->cls == QFC_DEPTH ? QDD_TX_DEPTH_DOWN : QDD_TX_READBACK,
                    s->desc.dwWidth * s->desc.dwHeight * (s->fmt->bpp > 8 ? s->fmt->bpp / 8 : 1));
    if (s->fmt->cls == QFC_DEPTH) {
        qdd_depth_readback(s);
        return;
    }
    /* P8: a rendered copy (a legacy software device's target) maps back to
     * the nearest palette entries. */
    QddPalette *pal = NULL;
    if (s->fmt->cls == QFC_PAL) {
        pal = s->palette ? s->palette : (s->dd->primary ? s->dd->primary->palette : NULL);
        if (s->fmt->bpp != 8 || !pal)
            return;
    }
    if (!qdd_stage(s))
        return;
    if (FAILED(s->dd->dev9->GetRenderTargetData(b->gpu, b->stage)))
        return;
    D3DLOCKED_RECT lr;
    if (FAILED(b->stage->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    const QddFormat *gf = qdd_format_d3d(b->gpu_fmt);
    bool same = gf == s->fmt;
    for (UINT y = 0; y < h; ++y) {
        const BYTE *src = (const BYTE *)lr.pBits + (size_t)y * lr.Pitch;
        BYTE *d = b->mem.ptr + (size_t)y * b->pitch;
        if (pal) {
            for (UINT x = 0; x < w; ++x)
                d[x] = qdd_palette_nearest(pal, ((const DWORD *)src)[x]);
        } else if (same) {
            memcpy(d, src, (size_t)w * qdd_format_bytes(gf));
        } else if (gf) {
            qdd_convert_row(s->fmt, d, gf, src, w, NULL);
        }
    }
    b->stage->UnlockRect();
}

HRESULT qdd_buf_sys(QddSurface *s)
{
    QddBuffer *b = s->buf;
    if (b->sys_valid)
        return DD_OK;
    if (b->gpu && b->gpu_valid && s->dd && s->dd->dev9)
        qdd_readback(s);
    b->sys_valid = true;
    return DD_OK;
}

IDirect3DSurface9 *qdd_gpu_surface(QddSurface *s)
{
    QddDraw *dd = s->dd;
    if (!dd)
        return NULL;
    if (!dd->dev9) {
        RECT r;
        qdd_present_rect(dd, &r);
        if (!qdd_dev_create(dd, (UINT)(r.right - r.left), (UINT)(r.bottom - r.top)))
            return NULL;
    }
    QddBuffer *b = s->buf;
    if (!b->gpu && !qdd_gpu_create(s))
        return NULL;
    bool pal_stale = qdd_format_is_pal(s->fmt) && b->gpu_valid && s->palette &&
                     b->gpu_pal_version != s->palette->version;
    if (!b->gpu_valid || pal_stale) {
        if (b->sys_valid)
            qdd_upload(s);
        else
            b->gpu_valid = true;
    }
    return b->gpu;
}

void qdd_gpu_release(QddDraw *dd)
{
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next) {
        QddBuffer *b = &s->own_buf;
        QddBuffer *cur = s->buf;
        s->buf = b;
        if (b->gpu && b->gpu_valid && !b->sys_valid && dd->dev9)
            qdd_readback(s);
        b->sys_valid = true;
        s->buf = cur;
    }
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next) {
        QddBuffer *b = &s->own_buf;
        qdd_pass_buffer_free(b);
        if (b->gpu)
            b->gpu->Release();
        if (b->gpu_tex)
            b->gpu_tex->Release();
        if (b->stage)
            b->stage->Release();
        b->gpu = NULL;
        b->gpu_tex = NULL;
        b->stage = NULL;
        b->gpu_valid = false;
    }
    qdd_pass_free(dd);
    if (dd->dev9) {
        qdd_d3d7_flush(dd);
        dd->dev9->Release();
        dd->dev9 = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* qdd.h                                                                     */

IDirect3DSurface9 *qdd_surface_d3d9(QddSurface *s, bool write)
{
    QddLock lock;
    IDirect3DSurface9 *g = qdd_gpu_surface(s);
    if (g && write) {
        s->buf->gpu_valid = true;
        s->buf->sys_valid = false;
        qdd_surf_touch(s);
        if (s->dd && s->dd->primary == s) {
            s->dd->primary_dirty = true;
            qdd_presenter_kick();
        }
    }
    return g;
}

IDirect3DTexture9 *qdd_surface_rt_texture(QddSurface *s)
{
    QddLock lock;
    if ((s->desc.ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) != (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE))
        return NULL;
    if (!qdd_gpu_surface(s))
        return NULL;
    return s->buf->gpu_tex;
}

/* ------------------------------------------------------------------------ */
/* Presentation                                                              */

HRESULT qdd_present(QddDraw *dd)
{
    QddSurface *front = dd->primary;
    if (!front || !qdd_present_qd3d(dd))
        return DD_OK;
    RECT r;
    qdd_present_rect(dd, &r);
    UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
    if (!w || !h || !qdd_dev_ensure(dd, w, h))
        return DD_OK;
    LONGLONG t0 = g_qdd_trace ? qdd_trace_now() : 0;
    IDirect3DSurface9 *g = qdd_gpu_surface(front);
    if (!g)
        return DD_OK;
    IDirect3DSurface9 *bb = NULL;
    if (SUCCEEDED(dd->dev9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        /* The gamma ramp applies to every presented frame. */
        if (qdd_gamma_identity(dd) || !qdd_gamma_pass(dd, g, &r, bb))
            dd->dev9->StretchRect(g, &r, bb, NULL, D3DTEXF_POINT);
        bb->Release();
    }
    qdd_d3d7_flush(dd);
    dd->dev9->Present(NULL, NULL, NULL, NULL);
    dd->primary_dirty = false;
    dd->present_vblank = qdd_vblank_count();
    if (g_qdd_trace)
        qdd_trace_present(t0);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* GDI presentation of the primary                                           */

struct QddDibBmi {
    BITMAPINFOHEADER h;
    union {
        DWORD masks[3];
        RGBQUAD colors[256];
    };
};

static bool qdd_primary_bmi(QddSurface *s, LONG width, LONG height, QddDibBmi *bmi)
{
    const QddFormat *f = s->fmt;
    memset(bmi, 0, sizeof(*bmi));
    bmi->h.biSize = sizeof(bmi->h);
    bmi->h.biWidth = width;
    bmi->h.biHeight = -height;
    bmi->h.biPlanes = 1;
    bmi->h.biBitCount = f->bpp;
    if (f->cls == QFC_PAL) {
        bmi->h.biCompression = BI_RGB;
        bmi->h.biClrUsed = 1u << f->bpp;
        const PALETTEENTRY *pe = qdd_surf_pal_entries(s);
        for (UINT i = 0; pe && i < bmi->h.biClrUsed; ++i) {
            bmi->colors[i].rgbRed = pe[i].peRed;
            bmi->colors[i].rgbGreen = pe[i].peGreen;
            bmi->colors[i].rgbBlue = pe[i].peBlue;
        }
        return true;
    }
    if (f->cls != QFC_RGB || f->bpp < 16)
        return false;
    if (f->bpp == 16) {
        bmi->h.biCompression = BI_BITFIELDS;
        bmi->masks[0] = f->mask_r;
        bmi->masks[1] = f->mask_g;
        bmi->masks[2] = f->mask_b;
    }
    return true;
}

/* The primary's own DIB when its memory is one (large GDI-format surfaces
 * are allocated as DIB sections, qdd_mem.cpp). */
static HBITMAP qdd_primary_dib(QddSurface *s, QddDibBmi *bmi)
{
    (void)bmi;
    return s->buf->mem.kind == QDD_MEM_KIND_DIB ? s->buf->mem.dib : NULL;
}

static void qdd_gdi_transfer(QddSurface *s, const RECT *r_in, HRGN clip, bool to_screen)
{
    RECT r;
    if (r_in)
        r = *r_in;
    else
        SetRect(&r, 0, 0, (int)s->desc.dwWidth, (int)s->desc.dwHeight);
    RECT full = {0, 0, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight};
    if (!IntersectRect(&r, &r, &full))
        return;
    QddDibBmi bmi;
    if (!qdd_primary_bmi(s, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight, &bmi))
        return;
    HDC screen = GetDC(NULL);
    if (!screen)
        return;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = qdd_primary_dib(s, &bmi);
    HBITMAP tmp = NULL;
    LONG ox = 0, oy = 0;
    UINT bytes = (s->fmt->bpp + 7) / 8;
    if (!dib) {
        /* Pitch differs from GDI's: go through a temporary DIB of the rect. */
        QddDibBmi tb;
        qdd_primary_bmi(s, r.right - r.left, r.bottom - r.top, &tb);
        void *bits;
        tmp = CreateDIBSection(screen, (BITMAPINFO *)&tb, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!tmp) {
            DeleteDC(mem);
            ReleaseDC(NULL, screen);
            return;
        }
        DWORD stride = (((DWORD)(r.right - r.left) * s->fmt->bpp + 31) >> 3) & ~3u;
        if (to_screen)
            for (LONG y = r.top; y < r.bottom; ++y)
                memcpy((BYTE *)bits + (size_t)(y - r.top) * stride,
                       s->buf->mem.ptr + (size_t)y * s->buf->pitch + (size_t)r.left * bytes,
                       (size_t)(r.right - r.left) * bytes);
        dib = tmp;
        ox = r.left;
        oy = r.top;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    if (s->fmt->cls == QFC_PAL)
        SetDIBColorTable(mem, 0, bmi.h.biClrUsed, bmi.colors);
    if (to_screen) {
        if (clip)
            SelectClipRgn(screen, clip);
        BitBlt(screen, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, r.left - ox, r.top - oy, SRCCOPY);
    } else {
        BitBlt(mem, r.left - ox, r.top - oy, r.right - r.left, r.bottom - r.top, screen, r.left, r.top, SRCCOPY);
    }
    GdiFlush();
    if (tmp && !to_screen) {
        DIBSECTION ds;
        if (GetObjectA(tmp, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits)
            for (LONG y = r.top; y < r.bottom; ++y)
                memcpy(s->buf->mem.ptr + (size_t)y * s->buf->pitch + (size_t)r.left * bytes,
                       (BYTE *)ds.dsBm.bmBits + (size_t)(y - r.top) * ds.dsBm.bmWidthBytes,
                       (size_t)(r.right - r.left) * bytes);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    if (tmp)
        DeleteObject(tmp);
    ReleaseDC(NULL, screen);
}

void qdd_primary_written(QddSurface *s, const RECT *r)
{
    QddDraw *dd = s->dd;
    if (!dd)
        return;
    if (qdd_present_qd3d(dd)) {
        if (!dd->primary || s != dd->primary)
            return;
        /* A write covering the whole frame (a windowed 3D game's Blt of its
         * back buffer, a 2D game's full-screen Blt) is a frame: present it
         * now, unless a frame already went out in this refresh period
         * (palette changes, GDI drawing on the primary: many per frame).
         * Those, and smaller writes, go out with the presenter at the next
         * emulated vertical blank: at most one extra present per refresh. */
        RECT pr, both;
        qdd_present_rect(dd, &pr);
        if ((!r || (IntersectRect(&both, r, &pr) && EqualRect(&both, &pr))) &&
            dd->present_vblank != qdd_vblank_count()) {
            qdd_present(dd);
            return;
        }
        dd->primary_dirty = true;
        qdd_presenter_kick();
        return;
    }
    /* GDI: the screen is the primary's truth.  A blit through a clipper
     * only wrote its clip region, so only that goes to the screen. */
    HRGN clip = NULL;
    if (s->clipper) {
        bool none;
        clip = qdd_clipper_region(s->clipper, &none);
    }
    qdd_gdi_transfer(s, r, clip, true);
    if (clip)
        DeleteObject(clip);
}

void qdd_primary_read(QddSurface *s, const RECT *r)
{
    QddDraw *dd = s->dd;
    if (!dd || qdd_present_qd3d(dd))
        return;
    if (s->buf->gpu_valid && !s->buf->sys_valid)
        return;
    qdd_gdi_transfer(s, r, NULL, false);
    s->buf->sys_valid = true;
    s->buf->gpu_valid = false;
}

/* ------------------------------------------------------------------------ */
/* Presenter thread: presents primaries written outside Flip at the
 * emulated vertical blank.  It pins this module while it runs.            */

static HANDLE g_presenter;
static volatile LONG g_presenter_run;

static DWORD WINAPI qdd_presenter_main(void *module)
{
    qdd_trace_mark_presenter();
    for (;;) {
        qdd_vblank_wait();
        QddLock lock;
        if (!g_presenter_run || !g_qdd_draws)
            break;
        for (QddDraw *dd = g_qdd_draws; dd; dd = dd->next)
            /* At most one present per refresh period: changes made after
             * this period's Flip or frame write wait for the next one. */
            if (dd->primary_dirty && dd->present_vblank != qdd_vblank_count())
                qdd_present(dd);
    }
    {
        QddLock lock;
        CloseHandle(g_presenter);
        g_presenter = NULL;
        g_presenter_run = 0;
    }
    FreeLibraryAndExitThread((HMODULE)module, 0);
    return 0;
}

void qdd_presenter_kick(void)
{
    QddLock lock;
    if (g_presenter)
        return;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_qdd_self, path, MAX_PATH);
    HMODULE pin = n && n < MAX_PATH ? LoadLibraryA(path) : NULL;
    if (!pin)
        return;
    g_presenter_run = 1;
    DWORD tid;
    g_presenter = CreateThread(NULL, 0, qdd_presenter_main, pin, 0, &tid);
    if (!g_presenter) {
        g_presenter_run = 0;
        FreeLibrary(pin);
    }
}

void qdd_presenter_stop(void)
{
    g_presenter_run = 0;
}
