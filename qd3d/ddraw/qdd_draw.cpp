/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: the DirectDraw object (IDirectDraw, 2, 4, 7), cooperative levels,
 * display modes with emulated depth, caps and the emulated vertical blank.
 *
 * Display modes.  The application sees the mode it set: 8, 16, 24 or 32
 * bits per pixel at any resolution the guest's GDI offers at 32 bits.  The
 * guest desktop changes to that resolution at 32 bits (input coordinates
 * then match); the depth is emulated: the primary has the requested format
 * and presentation converts.  QDDRAW_VIRTUAL_MODES=1 keeps the real display
 * untouched (host tests).
 */
#include "qdd_priv.h"

QddDraw *g_qdd_draws;

static QddMode g_mode;          /* the application's mode once set */
static bool g_mode_set;
static bool g_mode_changed_real; /* ChangeDisplaySettings changed the display */
static QddDraw *g_mode_owner;    /* restores the mode when it goes away */
static QddDraw *g_exclusive;     /* holds exclusive mode */
static LONG g_3d_devices;

/* Video memory accounting for GetAvailableVidMem. */
#define QDD_VIDMEM_TOTAL (256u * 1024u * 1024u)
static DWORD g_vidmem_used;

/* ------------------------------------------------------------------------ */
/* Display modes                                                             */

struct QddGdiMode {
    DWORD w, h, refresh;
};

/* The guest's 32-bit (or desktop-depth) modes, in driver order, unique. */
static UINT qdd_gdi_modes(QddGdiMode *out, UINT cap)
{
    DEVMODEA dm, cur;
    UINT n = 0;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    DWORD desk_bpp = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur) ? cur.dmBitsPerPel : 32;
    for (DWORD i = 0;; ++i) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            break;
        if (dm.dmBitsPerPel != 32 && dm.dmBitsPerPel != desk_bpp)
            continue;
        DWORD r = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
        bool dup = false;
        for (UINT j = 0; j < n && !dup; ++j)
            dup = out[j].w == dm.dmPelsWidth && out[j].h == dm.dmPelsHeight && out[j].refresh == r;
        if (dup)
            continue;
        if (n == cap)
            break;
        out[n].w = dm.dmPelsWidth;
        out[n].h = dm.dmPelsHeight;
        out[n].refresh = r;
        ++n;
    }
    if (!n && cur.dmPelsWidth) {
        out[0].w = cur.dmPelsWidth;
        out[0].h = cur.dmPelsHeight;
        out[0].refresh = cur.dmDisplayFrequency > 1 ? cur.dmDisplayFrequency : 0;
        n = 1;
    }
    return n;
}

void qdd_mode_current(QddMode *m)
{
    QddLock lock;
    if (g_mode_set) {
        *m = g_mode;
        return;
    }
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        m->w = dm.dmPelsWidth;
        m->h = dm.dmPelsHeight;
        m->bpp = dm.dmBitsPerPel;
        m->refresh = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
    } else {
        m->w = (DWORD)GetSystemMetrics(SM_CXSCREEN);
        m->h = (DWORD)GetSystemMetrics(SM_CYSCREEN);
        m->bpp = 32;
        m->refresh = 0;
    }
    if (m->bpp != 8 && m->bpp != 16 && m->bpp != 24 && m->bpp != 32)
        m->bpp = 32;
}

const QddFormat *qdd_mode_format(void)
{
    QddMode m;
    qdd_mode_current(&m);
    return qdd_format_for_bpp(m.bpp);
}

bool qdd_exclusive(QddDraw *dd)
{
    return g_exclusive == dd && (dd->coop & DDSCL_EXCLUSIVE);
}

QddDraw *qdd_exclusive_owner(void)
{
    return g_exclusive;
}

static const int g_draw_versions[4] = {1, 2, 4, 7};

bool qdd_present_qd3d(QddDraw *dd)
{
    return qdd_exclusive(dd) || g_3d_devices > 0;
}

static void qdd_fill_mode_desc(DDSURFACEDESC2 *d, DWORD w, DWORD h, DWORD bpp, DWORD refresh)
{
    const QddFormat *f = qdd_format_for_bpp(bpp);
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH | DDSD_REFRESHRATE;
    d->dwWidth = w;
    d->dwHeight = h;
    d->lPitch = (LONG)(((w * bpp / 8) + 3) & ~3u);
    d->dwRefreshRate = refresh;
    d->ddpfPixelFormat = f->ddpf;
}

/* Changes the real display to w x h (32 bits) unless virtual. */
static HRESULT qdd_change_display(DWORD w, DWORD h, DWORD refresh)
{
    if (g_qdd_env.virtual_modes)
        return DD_OK;
    DEVMODEA cur;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur) && cur.dmPelsWidth == w && cur.dmPelsHeight == h &&
        cur.dmBitsPerPel == 32 && (!refresh || cur.dmDisplayFrequency == refresh))
        return DD_OK;
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmBitsPerPel = 32;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (refresh) {
        dm.dmDisplayFrequency = refresh;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    LONG r = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        qdd_event("qddraw: ChangeDisplaySettingsEx(%lux%lux32@%lu) failed %ld", w, h, refresh, r);
        return DDERR_UNSUPPORTED;
    }
    g_mode_changed_real = true;
    return DD_OK;
}

static void qdd_restore_display(void)
{
    if (g_mode_changed_real && !g_qdd_env.virtual_modes)
        ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
    g_mode_changed_real = false;
}

/* Moves the exclusive window over the screen (native: the window rect
 * becomes the mode rect; style unchanged). */
static void qdd_setup_fullscreen_window(QddDraw *dd)
{
    HWND w = dd->window;
    if (!w || (dd->coop & DDSCL_NOWINDOWCHANGES))
        return;
    QddMode m;
    qdd_mode_current(&m);
    HWND after = GetForegroundWindow() == w ? HWND_TOPMOST : NULL;
    UINT flags = SWP_NOACTIVATE | (after ? 0 : SWP_NOZORDER);
    SetWindowPos(w, after, 0, 0, (int)m.w, (int)m.h, flags);
    dd->window_setup = true;
}

static HRESULT qdd_set_display_mode(QddDraw *dd, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    QddLock lock;
    if (g_exclusive && g_exclusive != dd)
        return DDERR_NOEXCLUSIVEMODE;
    if (!w || !h)
        return DD_OK; /* Need for Speed: Porsche Unleashed expects DD_OK (Wine) */
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return DDERR_INVALIDMODE;
    QddGdiMode modes[256];
    UINT n = qdd_gdi_modes(modes, 256);
    bool found = false;
    for (UINT i = 0; i < n && !found; ++i)
        found = modes[i].w == w && modes[i].h == h && (!refresh || modes[i].refresh == refresh || !modes[i].refresh);
    if (!found)
        return DDERR_INVALIDMODE;
    QddMode cur;
    qdd_mode_current(&cur);
    bool same = cur.w == w && cur.h == h && cur.bpp == bpp;
    HRESULT hr = qdd_change_display(w, h, refresh);
    if (FAILED(hr))
        return hr;
    g_mode.w = w;
    g_mode.h = h;
    g_mode.bpp = bpp;
    g_mode.refresh = refresh;
    if (!refresh) {
        DEVMODEA dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!g_qdd_env.virtual_modes && EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1)
            g_mode.refresh = dm.dmDisplayFrequency;
        else
            g_mode.refresh = 60;
    }
    g_mode_set = true;
    g_mode_owner = dd;
    if (!same)
        qdd_surface_mark_lost(dd, false);
    if (dd->coop & DDSCL_EXCLUSIVE) {
        qdd_setup_fullscreen_window(dd);
        RECT clip = {0, 0, (LONG)w, (LONG)h};
        ClipCursor(&clip);
    }
    return DD_OK;
}

static HRESULT qdd_restore_display_mode(QddDraw *dd)
{
    QddLock lock;
    if (g_exclusive && g_exclusive != dd)
        return DDERR_NOEXCLUSIVEMODE;
    if (!g_mode_set)
        return DD_OK;
    QddMode before = g_mode;
    g_mode_set = false;
    g_mode_owner = NULL;
    qdd_restore_display();
    QddMode now;
    qdd_mode_current(&now);
    if (now.w != before.w || now.h != before.h || now.bpp != before.bpp)
        qdd_surface_mark_lost(dd, false);
    if (dd->coop & DDSCL_EXCLUSIVE)
        ClipCursor(NULL);
    return DD_OK;
}

/* ------------------------------------------------------------------------ */
/* Emulated vertical blank                                                   */

static LARGE_INTEGER g_qpc_freq;

static DWORD qdd_refresh(void)
{
    QddMode m;
    qdd_mode_current(&m);
    return m.refresh >= 30 && m.refresh <= 240 ? m.refresh : 60;
}

static LONGLONG qdd_now(void)
{
    LARGE_INTEGER t;
    if (!g_qpc_freq.QuadPart)
        QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

LONGLONG qdd_vblank_count(void)
{
    LONGLONG now = qdd_now();
    return now * (LONGLONG)qdd_refresh() / g_qpc_freq.QuadPart;
}

/* Position in the refresh period [0, 1) as a fraction of 65536. */
static DWORD qdd_vblank_phase(void)
{
    LONGLONG now = qdd_now(), r = qdd_refresh();
    LONGLONG ticks = now * r * 65536 / g_qpc_freq.QuadPart;
    return (DWORD)(ticks & 0xffff);
}

bool qdd_vblank_in(DWORD *scanline)
{
    QddMode m;
    qdd_mode_current(&m);
    DWORD total = m.h + m.h / 20 + 1; /* ~5% blanking */
    DWORD line = (DWORD)(((ULONGLONG)qdd_vblank_phase() * total) >> 16);
    if (scanline)
        *scanline = line;
    return line >= m.h;
}

void qdd_vblank_wait(void)
{
    LONGLONG t0 = g_qdd_trace ? qdd_trace_now() : 0;
    LONGLONG start = qdd_vblank_count();
    LONGLONG target_tick = (start + 1) * g_qpc_freq.QuadPart / qdd_refresh();
    for (;;) {
        LONGLONG now = qdd_now();
        if (now >= target_tick)
            break;
        LONGLONG ms = (target_tick - now) * 1000 / g_qpc_freq.QuadPart;
        Sleep(ms > 2 ? (DWORD)(ms - 2) : 0);
    }
    if (g_qdd_trace)
        qdd_trace_vblank(t0);
}

/* ------------------------------------------------------------------------ */
/* Object lifetime                                                           */

void *qdd_draw_iface(QddDraw *dd, int v)
{
    switch (v) {
    case 1: return static_cast<IDirectDraw *>(&dd->if1);
    case 2: return static_cast<IDirectDraw2 *>(&dd->if2);
    case 4: return static_cast<IDirectDraw4 *>(&dd->if4);
    default: return static_cast<IDirectDraw7 *>(&dd->if7);
    }
}

static int qdd_draw_ridx(int v)
{
    return v == 1 ? 0 : v == 2 ? 1 : v == 4 ? 2 : 3;
}

HRESULT qdd_draw_create(IDirect3D9 *d3d9, bool ex, bool initialized, QddDraw **out)
{
    QddDraw *dd = new QddDraw;
    *out = NULL;
    if (!dd) {
        d3d9->Release();
        return DDERR_OUTOFMEMORY;
    }
    dd->if1.dd = dd;
    dd->if2.dd = dd;
    dd->if4.dd = dd;
    dd->if7.dd = dd;
    if (g_qdd_trace) {
        static const int vs[] = {1, 2, 4, 7};
        for (int v : vs)
            qdd_trace_vtbl(qdd_draw_iface(dd, v), QDD_TK_DRAW, v);
    }
    dd->ex = ex;
    dd->initialized = initialized;
    dd->d3d9 = d3d9;
    dd->ref[3] = 1;
    dd->ifaces = 1;
    if (!qdd_reg_add(qdd_draw_iface(dd, 1), dd, QDD_T_DRAW, 1) ||
        !qdd_reg_add(qdd_draw_iface(dd, 2), dd, QDD_T_DRAW, 2) ||
        !qdd_reg_add(qdd_draw_iface(dd, 4), dd, QDD_T_DRAW, 4) ||
        !qdd_reg_add(qdd_draw_iface(dd, 7), dd, QDD_T_DRAW, 7)) {
        for (int i = 0; i < 4; ++i)
            qdd_reg_remove(qdd_draw_iface(dd, g_draw_versions[i]));
        d3d9->Release();
        delete dd;
        return DDERR_OUTOFMEMORY;
    }
    QddLock lock;
    dd->next = g_qdd_draws;
    g_qdd_draws = dd;
    *out = dd;
    return DD_OK;
}

static void qdd_draw_destroy(QddDraw *dd)
{
    {
        QddLock lock;
        qdd_d3d7_draw_destroy(dd);
        /* Surfaces outliving their DirectDraw object keep working on their
         * system-memory copies; they lose their Direct3D 9 copies. */
        qdd_gpu_release(dd);
        for (QddSurface *s = dd->surfaces; s; s = s->dd_next)
            s->dd = NULL;
        if (g_exclusive == dd) {
            g_exclusive = NULL;
            ClipCursor(NULL);
        }
        if (g_mode_owner == dd) {
            g_mode_set = false;
            g_mode_owner = NULL;
            qdd_restore_display();
        }
        if (dd->device_window)
            DestroyWindow(dd->device_window);
        if (dd->hidden_window)
            DestroyWindow(dd->hidden_window);
        QddDraw **pp = &g_qdd_draws;
        while (*pp && *pp != dd)
            pp = &(*pp)->next;
        if (*pp)
            *pp = dd->next;
        for (int i = 0; i < 4; ++i)
            qdd_reg_remove(qdd_draw_iface(dd, g_draw_versions[i]));
    }
    if (dd->d3d9)
        dd->d3d9->Release();
    delete dd;
}

ULONG qdd_draw_addref(QddDraw *dd, int v)
{
    LONG r = InterlockedIncrement(&dd->ref[qdd_draw_ridx(v)]);
    if (r == 1)
        InterlockedIncrement(&dd->ifaces);
    return (ULONG)r;
}

ULONG qdd_draw_release(QddDraw *dd, int v)
{
    LONG r = InterlockedDecrement(&dd->ref[qdd_draw_ridx(v)]);
    if (!r && !InterlockedDecrement(&dd->ifaces))
        qdd_draw_destroy(dd);
    return (ULONG)r;
}

HRESULT qdd_draw_qi(QddDraw *dd, REFIID riid, void **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (qdd_iid_null(&riid))
        return DDERR_INVALIDPARAMS;
    int v = 0;
    /* IUnknown is the IDirectDraw interface, sharing its count (stock). */
    if (IsEqualGUID(riid, IID_IDirectDraw7))
        v = 7;
    else if (IsEqualGUID(riid, IID_IDirectDraw4))
        v = 4;
    else if (IsEqualGUID(riid, IID_IDirectDraw2))
        v = 2;
    else if (IsEqualGUID(riid, IID_IDirectDraw) || IsEqualGUID(riid, IID_IUnknown))
        v = 1;
    if (v) {
        qdd_draw_addref(dd, v);
        *out = qdd_draw_iface(dd, v);
        return S_OK;
    }
    bool d3d7 = IsEqualGUID(riid, QDD_IID_IDirect3D7);
    bool d3dold = IsEqualGUID(riid, QDD_IID_IDirect3D) || IsEqualGUID(riid, QDD_IID_IDirect3D2) ||
                  IsEqualGUID(riid, QDD_IID_IDirect3D3);
    /* Native: IDirect3D7 only from DirectDraw7 objects, the older ones only
     * from the others (Wine's refcount tests). */
    if ((d3d7 && dd->ex) || (d3dold && !dd->ex)) {
        QddLock lock;
        HRESULT hr = qdd_d3d7_query(dd, riid, out);
        if (SUCCEEDED(hr))
            g_qdd_d3d_loaded = true;
        return hr;
    }
    return E_NOINTERFACE;
}

/* ------------------------------------------------------------------------ */
/* Cooperative level                                                         */

static const char g_devwnd_class[] = "QDDrawDeviceWnd";

static HWND qdd_create_device_window(void)
{
    static bool registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = g_qdd_self;
        wc.lpszClassName = g_devwnd_class;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassA(&wc);
        registered = true;
    }
    return CreateWindowExA(0, g_devwnd_class, "DirectDrawDeviceWnd", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                           GetSystemMetrics(SM_CYSCREEN), NULL, NULL, g_qdd_self, NULL);
}

static HRESULT qdd_set_focus_window(QddDraw *dd, HWND window)
{
    if (dd->focus_window && dd->focus_window != window)
        return DDERR_HWNDALREADYSET;
    if (dd->window && dd->window != window && (dd->coop & DDSCL_EXCLUSIVE))
        return DDERR_HWNDALREADYSET;
    dd->focus_window = window;
    return DD_OK;
}

static HRESULT qdd_set_coop(QddDraw *dd, HWND window, DWORD flags, bool restore_mode_on_normal)
{
    QddLock lock;
    if (dd->scl_recursive)
        return DD_OK;
    if (!(flags & (DDSCL_SETFOCUSWINDOW | DDSCL_NORMAL | DDSCL_EXCLUSIVE)))
        return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_CREATEDEVICEWINDOW) && !(flags & DDSCL_EXCLUSIVE))
        return DDERR_INVALIDPARAMS;
    if (window && !IsWindow(window))
        return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_SETFOCUSWINDOW) && !(flags & DDSCL_CREATEDEVICEWINDOW)) {
        if (flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE | DDSCL_ALLOWREBOOT |
                     DDSCL_ALLOWMODEX | DDSCL_SETDEVICEWINDOW | DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))
            return DDERR_INVALIDPARAMS;
        return qdd_set_focus_window(dd, window);
    }
    if ((flags & DDSCL_NORMAL) && (flags & DDSCL_EXCLUSIVE))
        return DDERR_INVALIDPARAMS;
    dd->scl_recursive = true;
    HRESULT hr = DD_OK;
    if (flags & DDSCL_EXCLUSIVE) {
        if (!(flags & DDSCL_FULLSCREEN) || !(window || (flags & DDSCL_CREATEDEVICEWINDOW))) {
            hr = DDERR_INVALIDPARAMS;
            goto done;
        }
        if (g_exclusive && g_exclusive != dd) {
            hr = DDERR_EXCLUSIVEMODEALREADYSET;
            goto done;
        }
        if (flags & DDSCL_CREATEDEVICEWINDOW) {
            if (!dd->focus_window && !(flags & DDSCL_SETFOCUSWINDOW)) {
                hr = DDERR_NOFOCUSWINDOW;
                goto done;
            }
            HWND dw = qdd_create_device_window();
            if (!dw) {
                hr = E_FAIL;
                goto done;
            }
            ShowWindow(dw, SW_SHOW);
            if (dd->device_window)
                DestroyWindow(dd->device_window);
            dd->device_window = dw;
            if (flags & DDSCL_SETFOCUSWINDOW) {
                if (!window) {
                    hr = DDERR_NOHWND;
                    goto done;
                }
                if (FAILED(hr = qdd_set_focus_window(dd, window)))
                    goto done;
            }
            window = dw;
        } else if (!dd->focus_window) {
            dd->focus_window = window;
        }
    } else {
        if (dd->device_window) {
            DestroyWindow(dd->device_window);
            dd->device_window = NULL;
        }
        dd->focus_window = NULL;
    }
    {
        bool was_exclusive = qdd_exclusive(dd);
        bool now_exclusive = (flags & DDSCL_EXCLUSIVE) != 0;
        if (now_exclusive)
            g_exclusive = dd;
        else if (g_exclusive == dd)
            g_exclusive = NULL;
        if (was_exclusive && !now_exclusive) {
            ClipCursor(NULL);
            if (restore_mode_on_normal && g_mode_owner == dd) {
                g_mode_set = false;
                g_mode_owner = NULL;
                qdd_restore_display();
            }
        }
        if (was_exclusive != now_exclusive)
            qdd_surface_mark_lost(dd, true);
        /* Leaving exclusive mode with no 3D device: give the Direct3D 9
         * device (and the display it shows) back now; the desktop path
         * needs none, and a new exclusive session creates another. */
        if (was_exclusive && !now_exclusive && g_3d_devices <= 0 && dd->dev9)
            qdd_gpu_release(dd);
        dd->coop = flags;
        dd->window = window;
        if (now_exclusive)
            qdd_setup_fullscreen_window(dd);
        else
            dd->window_setup = false;
    }
done:
    dd->scl_recursive = false;
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Caps                                                                      */

static const DWORD g_fourcc[] = {
    MAKEFOURCC('D', 'X', 'T', '1'), MAKEFOURCC('D', 'X', 'T', '2'), MAKEFOURCC('D', 'X', 'T', '3'),
    MAKEFOURCC('D', 'X', 'T', '4'), MAKEFOURCC('D', 'X', 'T', '5'), MAKEFOURCC('Y', 'U', 'Y', '2'),
    MAKEFOURCC('U', 'Y', 'V', 'Y'), MAKEFOURCC('Y', 'V', '1', '2'),
};

static void qdd_fill_caps(DDCAPS_DX7 *c)
{
    const DWORD blt = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                      DDCAPS_BLTFOURCC | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP |
                      DDCAPS_CANCLIPSTRETCHED;
    memset(c, 0, sizeof(*c));
    c->dwSize = sizeof(*c);
    c->dwCaps = DDCAPS_3D | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_ZBLTS | blt;
    c->dwCaps2 = DDCAPS2_CERTIFIED | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_WIDESURFACES | DDCAPS2_PRIMARYGAMMA |
                 DDCAPS2_CANRENDERWINDOWED | DDCAPS2_FLIPINTERVAL | DDCAPS2_FLIPNOVSYNC |
                 DDCAPS2_CANMANAGETEXTURE;
    c->dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT | DDCKEYCAPS_SRCBLTCLRSPACE |
                    DDCKEYCAPS_DESTBLTCLRSPACE;
    c->dwFXCaps = DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN | DDFXCAPS_BLTSHRINKX |
                  DDFXCAPS_BLTSHRINKY | DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY;
    c->dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
    c->dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    c->dwVidMemTotal = QDD_VIDMEM_TOTAL;
    c->dwVidMemFree = QDD_VIDMEM_TOTAL - g_vidmem_used;
    c->dwNumFourCCCodes = sizeof(g_fourcc) / sizeof(g_fourcc[0]);
    c->dwRops[SRCCOPY >> 21] |= 1u << ((SRCCOPY >> 16) & 0x1f);
    c->dwRops[BLACKNESS >> 21] |= 1u << ((BLACKNESS >> 16) & 0x1f);
    c->dwRops[WHITENESS >> 21] |= 1u << ((WHITENESS >> 16) & 0x1f);
    c->dwSVBCaps = c->dwVSBCaps = c->dwSSBCaps = blt;
    c->dwSVBCKeyCaps = c->dwVSBCKeyCaps = c->dwSSBCKeyCaps = c->dwCKeyCaps;
    c->dwSVBFXCaps = c->dwVSBFXCaps = c->dwSSBFXCaps = c->dwFXCaps;
    memcpy(c->dwSVBRops, c->dwRops, sizeof(c->dwRops));
    memcpy(c->dwVSBRops, c->dwRops, sizeof(c->dwRops));
    memcpy(c->dwSSBRops, c->dwRops, sizeof(c->dwRops));
    c->dwMinOverlayStretch = c->dwMaxOverlayStretch = 1000;
    c->ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                        DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                        DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_VISIBLE |
                        DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP | DDSCAPS_LOCALVIDMEM;
    c->ddsCaps.dwCaps2 = DDSCAPS2_CUBEMAP;
    c->ddsOldCaps.dwCaps = c->ddsCaps.dwCaps;
}

/* ------------------------------------------------------------------------ */
/* qdd.h accessors                                                           */

IDirectDraw7 *qdd_draw_iface7(QddDraw *dd) { return &dd->if7; }
QddDraw *qdd_draw_lookup(const void *p) { return (QddDraw *)qdd_reg_find(p, QDD_T_DRAW, NULL); }
IDirect3D9 *qdd_draw_d3d9(QddDraw *dd) { return dd->d3d9; }
DWORD qdd_draw_coop(QddDraw *dd) { return dd->coop; }
HWND qdd_draw_window(QddDraw *dd) { return dd->window; }
void **qdd_draw_d3d7_slot(QddDraw *dd) { return &dd->d3d7; }
bool qdd_draw_created_ex(QddDraw *dd) { return dd->ex; }

void qdd_draw_3d_devices(QddDraw *dd, int delta)
{
    QddLock lock;
    g_3d_devices += delta;
    if (g_3d_devices < 0)
        g_3d_devices = 0;
    (void)dd;
}

/* ------------------------------------------------------------------------ */
/* Interface methods                                                         */

#define QDD_INIT_CHECK()                 \
    do {                                 \
        if (!dd->initialized)            \
            return DDERR_NOTINITIALIZED; \
    } while (0)

template <int V> HRESULT QddDrawIface<V>::QueryInterface(REFIID riid, void **out)
{
    return qdd_draw_qi(dd, riid, out);
}

template <int V> ULONG QddDrawIface<V>::AddRef()
{
    return qdd_draw_addref(dd, V);
}

template <int V> ULONG QddDrawIface<V>::Release()
{
    return qdd_draw_release(dd, V);
}

template <int V> HRESULT QddDrawIface<V>::Compact()
{
    QDD_INIT_CHECK();
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::CreateClipper(DWORD flags, IDirectDrawClipper **out, IUnknown *outer)
{
    QDD_INIT_CHECK();
    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddClipper *c;
    HRESULT hr = qdd_clipper_create(flags, &c, true);
    if (FAILED(hr))
        return hr;
    if (V == 4 || V == 7) {
        c->dd_ref = (IUnknown *)qdd_draw_iface(dd, V);
        qdd_draw_addref(dd, V);
    }
    *out = &c->iface;
    return DD_OK;
}

template <int V>
HRESULT QddDrawIface<V>::CreatePalette(DWORD flags, PALETTEENTRY *entries, IDirectDrawPalette **out, IUnknown *outer)
{
    QDD_INIT_CHECK();
    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (!dd->coop)
        return DDERR_NOCOOPERATIVELEVELSET;
    QddPalette *p;
    HRESULT hr = qdd_palette_create(dd, V, flags, entries, &p);
    if (FAILED(hr)) {
        *out = NULL;
        return hr;
    }
    *out = &p->iface;
    return DD_OK;
}

/* DDSURFACEDESC -> DDSURFACEDESC2 (the fields after ddsCaps differ). */
static void qdd_desc1_to_2(const DDSURFACEDESC *in, DDSURFACEDESC2 *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out, in, sizeof(*in));
    out->dwSize = sizeof(*out);
}

template <int V>
HRESULT QddDrawIface<V>::CreateSurface(typename T::Desc *desc, typename T::Surf **out, IUnknown *outer)
{
    QDD_INIT_CHECK();
    QddLock lock;
    if (!(dd->coop & (DDSCL_NORMAL | DDSCL_EXCLUSIVE)))
        return DDERR_NOCOOPERATIVELEVELSET;
    if (!desc || desc->dwSize != sizeof(typename T::Desc))
        return DDERR_INVALIDPARAMS;
    if (!out || IsBadWritePtr(out, sizeof(*out)))
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    DDSURFACEDESC2 d2;
    if (V >= 4) {
        memcpy(&d2, desc, sizeof(d2));
        if (desc->ddsCaps.dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER))
            return DDERR_INVALIDCAPS;
    } else {
        qdd_desc1_to_2((const DDSURFACEDESC *)(const void *)desc, &d2);
        DWORD c = desc->ddsCaps.dwCaps;
        if ((c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) == (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER) ||
            (c & (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER)) == (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER))
            return DDERR_INVALIDCAPS;
    }
    QddSurface *s;
    HRESULT hr = qdd_surface_create(dd, V, &d2, &s);
    if (FAILED(hr))
        return hr;
    *out = (typename T::Surf *)qdd_surf_iface(s, SV);
    if (V == 4 || V == 7) {
        s->dd_ref = (IUnknown *)qdd_draw_iface(dd, V);
        qdd_draw_addref(dd, V);
    }
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::DuplicateSurface(typename T::Surf *src, typename T::Surf **out)
{
    QDD_INIT_CHECK();
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    QddLock lock;
    QddSurface *s = qdd_surf_from(src);
    if (!s)
        return DDERR_INVALIDPARAMS;
    return qdd_surface_duplicate(s, V, SV, (void **)out);
}

template <int V>
HRESULT QddDrawIface<V>::EnumDisplayModes(DWORD flags, typename T::Desc *desc, void *ctx, typename T::ModeCb cb)
{
    QDD_INIT_CHECK();
    if (!cb)
        return DDERR_INVALIDPARAMS;
    static const DWORD depths[] = {32, 24, 16, 8};
    QddGdiMode *modes = (QddGdiMode *)malloc(256 * sizeof(QddGdiMode));
    struct Seen {
        DWORD w, h, bpp, refresh;
    } *seen = (Seen *)malloc(1024 * sizeof(Seen));
    if (!modes || !seen) {
        free(modes);
        free(seen);
        return DDERR_OUTOFMEMORY;
    }
    UINT n = qdd_gdi_modes(modes, 256), nseen = 0;
    for (UINT d = 0; d < 4; ++d) {
        for (UINT i = 0; i < n; ++i) {
            DWORD refresh = (flags & DDEDM_REFRESHRATES) ? modes[i].refresh : 0;
            if (desc) {
                if ((desc->dwFlags & DDSD_WIDTH) && modes[i].w != desc->dwWidth)
                    continue;
                if ((desc->dwFlags & DDSD_HEIGHT) && modes[i].h != desc->dwHeight)
                    continue;
                if ((desc->dwFlags & DDSD_REFRESHRATE) && modes[i].refresh != desc->dwRefreshRate)
                    continue;
                if ((desc->dwFlags & DDSD_PIXELFORMAT) && depths[d] != desc->ddpfPixelFormat.dwRGBBitCount)
                    continue;
            }
            bool dup = false;
            for (UINT j = 0; j < nseen && !dup; ++j)
                dup = seen[j].w == modes[i].w && seen[j].h == modes[i].h && seen[j].bpp == depths[d] &&
                      seen[j].refresh == refresh;
            if (dup)
                continue;
            if (nseen < 1024) {
                seen[nseen].w = modes[i].w;
                seen[nseen].h = modes[i].h;
                seen[nseen].bpp = depths[d];
                seen[nseen].refresh = refresh;
                ++nseen;
            }
            DDSURFACEDESC2 sd;
            qdd_fill_mode_desc(&sd, modes[i].w, modes[i].h, depths[d], refresh);
            HRESULT r;
            if (V >= 4) {
                r = cb((typename T::Desc *)(void *)&sd, ctx);
            } else {
                DDSURFACEDESC sd1;
                memcpy(&sd1, &sd, sizeof(sd1));
                sd1.dwSize = sizeof(sd1);
                r = cb((typename T::Desc *)(void *)&sd1, ctx);
            }
            if (r == DDENUMRET_CANCEL) {
                free(modes);
                free(seen);
                return DD_OK;
            }
        }
    }
    free(modes);
    free(seen);
    return DD_OK;
}

template <int V>
HRESULT QddDrawIface<V>::EnumSurfaces(DWORD flags, typename T::Desc *desc, void *ctx, typename T::SurfCb cb)
{
    QDD_INIT_CHECK();
    if (!cb)
        return DDERR_INVALIDPARAMS;
    if (flags & DDENUMSURFACES_CANBECREATED) {
        if (!(flags & DDENUMSURFACES_MATCH) || (flags & DDENUMSURFACES_ALL))
            return DDERR_INVALIDPARAMS;
        /* Potential surfaces: the given size, else every display resolution. */
        const QddFormat *f = qdd_mode_format();
        if (desc && (desc->dwFlags & DDSD_PIXELFORMAT) && !(f = qdd_format_from_ddpf(&desc->ddpfPixelFormat)))
            return DD_OK;
        QddGdiMode modes[256];
        UINT n;
        if (desc && (desc->dwFlags & DDSD_WIDTH) && (desc->dwFlags & DDSD_HEIGHT)) {
            modes[0].w = desc->dwWidth;
            modes[0].h = desc->dwHeight;
            n = 1;
        } else {
            QddGdiMode all[256];
            UINT m = qdd_gdi_modes(all, 256);
            n = 0;
            for (UINT i = 0; i < m; ++i) {
                bool dup = false;
                for (UINT j = 0; j < n && !dup; ++j)
                    dup = modes[j].w == all[i].w && modes[j].h == all[i].h;
                if (!dup)
                    modes[n++] = all[i];
            }
        }
        for (UINT i = 0; i < n; ++i) {
            DDSURFACEDESC2 sd;
            memset(&sd, 0, sizeof(sd));
            sd.dwSize = sizeof(typename T::Desc);
            sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
            sd.ddsCaps.dwCaps = desc ? desc->ddsCaps.dwCaps : 0;
            sd.dwWidth = modes[i].w;
            sd.dwHeight = modes[i].h;
            sd.lPitch = (LONG)qdd_format_pitch(f, modes[i].w);
            sd.ddpfPixelFormat = f->ddpf;
            if (cb(NULL, (typename T::Desc *)(void *)&sd, ctx) == DDENUMRET_CANCEL)
                break;
        }
        return DD_OK;
    }
    if (!(flags & DDENUMSURFACES_DOESEXIST))
        return DDERR_INVALIDPARAMS;
    bool all = (flags & DDENUMSURFACES_ALL) != 0, nomatch = (flags & DDENUMSURFACES_NOMATCH) != 0;
    if (!all && !desc)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    /* Snapshot: the callback may release surfaces. */
    UINT count = 0;
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next)
        ++count;
    QddSurface **list = (QddSurface **)malloc((count ? count : 1) * sizeof(QddSurface *));
    if (!list)
        return DDERR_OUTOFMEMORY;
    count = 0;
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next) {
        bool match = true;
        if (!all) {
            const DDSURFACEDESC2 *d = &s->desc;
            DWORD f = desc->dwFlags;
            if ((f & DDSD_WIDTH) && d->dwWidth != desc->dwWidth)
                match = false;
            if ((f & DDSD_HEIGHT) && d->dwHeight != desc->dwHeight)
                match = false;
            if ((f & DDSD_CAPS) && (d->ddsCaps.dwCaps & desc->ddsCaps.dwCaps) != desc->ddsCaps.dwCaps)
                match = false;
            if ((f & DDSD_PIXELFORMAT) &&
                memcmp(&d->ddpfPixelFormat, &desc->ddpfPixelFormat, sizeof(DDPIXELFORMAT)))
                match = false;
            if (nomatch)
                match = !match;
        }
        if (match) {
            list[count++] = s;
            qdd_surf_addref(s, qdd_vidx(SV));
        }
    }
    for (UINT i = 0; i < count; ++i) {
        QddSurface *s = list[i];
        DDSURFACEDESC2 sd = s->desc;
        HRESULT r;
        if (V >= 4) {
            r = cb((typename T::Surf *)qdd_surf_iface(s, SV), (typename T::Desc *)(void *)&sd, ctx);
        } else {
            DDSURFACEDESC sd1;
            memcpy(&sd1, &sd, sizeof(sd1));
            sd1.dwSize = sizeof(sd1);
            r = cb((typename T::Surf *)qdd_surf_iface(s, SV), (typename T::Desc *)(void *)&sd1, ctx);
        }
        if (r == DDENUMRET_CANCEL) {
            for (UINT j = i + 1; j < count; ++j)
                qdd_surf_release(list[j], qdd_vidx(SV));
            break;
        }
    }
    free(list);
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::FlipToGDISurface()
{
    QDD_INIT_CHECK();
    QddLock lock;
    QddSurface *p = dd->primary;
    if (!p)
        return DDERR_NOTFOUND;
    if ((qdd_surf_caps(p) & DDSCAPS_FLIP) && p->complex_attached[0]) {
        /* Restore the original front buffer: rotate until it is in front. */
        QddSurface *s = p;
        UINT guard = 0;
        while (s->buf != &p->own_buf && guard++ < 16) {
            HRESULT hr = p->if7.Flip(NULL, DDFLIP_NOVSYNC);
            if (FAILED(hr))
                break;
            s = p;
        }
    }
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetCaps(DDCAPS *hal, DDCAPS *hel)
{
    QDD_INIT_CHECK();
    if (!hal && !hel)
        return DDERR_INVALIDPARAMS;
    DDCAPS_DX7 caps[2];
    {
        QddLock lock;
        qdd_fill_caps(&caps[0]);
    }
    /* The emulation layer's caps (stock values, ddraw7.c test_caps). */
    caps[1] = caps[0];
    caps[1].ddsCaps.dwCaps = DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE |
                             DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
    caps[1].ddsOldCaps.dwCaps = caps[1].ddsCaps.dwCaps;
    caps[1].dwZBufferBitDepths = DDBD_16;
    /* Objects created for one layer report nothing for the other. */
    if (dd->hw == QddDraw::QDD_HEL_ONLY) {
        memset(&caps[0], 0, sizeof(caps[0]));
        caps[0].dwSize = sizeof(caps[0]);
    } else if (dd->hw == QddDraw::QDD_HW_ONLY) {
        memset(&caps[1], 0, sizeof(caps[1]));
        caps[1].dwSize = sizeof(caps[1]);
    }
    DDCAPS *outs[2] = {hal, hel};
    for (DDCAPS *o : outs) {
        if (!o)
            continue;
        DWORD size = o->dwSize;
        if (size != sizeof(DDCAPS_DX7) && size != sizeof(DDCAPS_DX6) && size != sizeof(DDCAPS_DX5) &&
            size != sizeof(DDCAPS_DX3))
            return DDERR_INVALIDPARAMS;
    }
    for (int i = 0; i < 2; ++i) {
        if (!outs[i])
            continue;
        DWORD size = outs[i]->dwSize;
        memcpy(outs[i], &caps[i], size);
        outs[i]->dwSize = size;
    }
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetDisplayMode(typename T::Desc *desc)
{
    QDD_INIT_CHECK();
    if (!desc || (desc->dwSize != sizeof(DDSURFACEDESC) && desc->dwSize != sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    QddMode m;
    qdd_mode_current(&m);
    DDSURFACEDESC2 sd;
    qdd_fill_mode_desc(&sd, m.w, m.h, m.bpp, m.refresh);
    sd.lPitch = (LONG)(m.w * m.bpp / 8);
    memcpy(desc, &sd, desc->dwSize);
    desc->dwSize = sizeof(typename T::Desc);
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetFourCCCodes(DWORD *count, DWORD *codes)
{
    QDD_INIT_CHECK();
    const DWORD n = sizeof(g_fourcc) / sizeof(g_fourcc[0]);
    if (!count)
        return DDERR_INVALIDPARAMS;
    if (codes) {
        DWORD k = *count < n ? *count : n;
        memcpy(codes, g_fourcc, k * sizeof(DWORD));
    }
    *count = n;
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetGDISurface(typename T::Surf **out)
{
    QDD_INIT_CHECK();
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    *out = NULL;
    QddSurface *p = dd->primary;
    if (!p)
        return DDERR_NOTFOUND;
    /* The surface whose buffer is the original front buffer. */
    QddSurface *s = p;
    for (UINT i = 0; i < 16 && s; ++i) {
        if (s->buf == &p->own_buf)
            break;
        s = s->complex_attached[0];
        if (s == p) {
            s = p;
            break;
        }
    }
    if (!s)
        s = p;
    qdd_surf_addref(s, qdd_vidx(SV));
    *out = (typename T::Surf *)qdd_surf_iface(s, SV);
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetMonitorFrequency(DWORD *freq)
{
    QDD_INIT_CHECK();
    if (!freq)
        return DDERR_INVALIDPARAMS;
    *freq = qdd_refresh();
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetScanLine(DWORD *line)
{
    QDD_INIT_CHECK();
    if (!line)
        return DDERR_INVALIDPARAMS;
    DWORD l;
    bool in = qdd_vblank_in(&l);
    *line = l;
    return in ? DDERR_VERTICALBLANKINPROGRESS : DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetVerticalBlankStatus(WINBOOL *in_vb)
{
    QDD_INIT_CHECK();
    if (!in_vb)
        return DDERR_INVALIDPARAMS;
    *in_vb = qdd_vblank_in(NULL);
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::Initialize(GUID *guid)
{
    QddLock lock;
    if (dd->initialized)
        return DDERR_ALREADYINITIALIZED;
    dd->initialized = true;
    qdd_draw_set_guid(dd, guid);
    return DD_OK;
}

void qdd_draw_set_guid(QddDraw *dd, const GUID *guid)
{
    if (guid == (const GUID *)DDCREATE_EMULATIONONLY)
        dd->hw = QddDraw::QDD_HEL_ONLY;
    else if (guid == (const GUID *)DDCREATE_HARDWAREONLY)
        dd->hw = QddDraw::QDD_HW_ONLY;
    else
        dd->hw = QddDraw::QDD_HW_BOTH;
}

template <int V> HRESULT QddDrawIface<V>::RestoreDisplayMode()
{
    QDD_INIT_CHECK();
    return qdd_restore_display_mode(dd);
}

template <int V> HRESULT QddDrawIface<V>::SetCooperativeLevel(HWND window, DWORD flags)
{
    QDD_INIT_CHECK();
    HRESULT hr = qdd_set_coop(dd, window, flags, V != 1 && !dd->scl_ddraw1);
    if (SUCCEEDED(hr))
        dd->scl_ddraw1 = V == 1;
    return hr;
}

template <int V> HRESULT QddDrawIface<V>::SetDisplayMode(DWORD w, DWORD h, DWORD bpp)
{
    QDD_INIT_CHECK();
    return qdd_set_display_mode(dd, w, h, bpp, 0, 0);
}

template <int V> HRESULT QddDrawIface<V>::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    QDD_INIT_CHECK();
    return qdd_set_display_mode(dd, w, h, bpp, refresh, flags);
}

template <int V> HRESULT QddDrawIface<V>::WaitForVerticalBlank(DWORD flags, HANDLE event)
{
    QDD_INIT_CHECK();
    if (flags & ~(DWORD)(DDWAITVB_BLOCKBEGIN | DDWAITVB_BLOCKBEGINEVENT | DDWAITVB_BLOCKEND))
        return DDERR_INVALIDPARAMS;
    if (flags & DDWAITVB_BLOCKBEGINEVENT)
        return DDERR_UNSUPPORTED;
    if (flags & DDWAITVB_BLOCKEND) {
        /* Wait until the blank in progress (or the next one) ends. */
        while (!qdd_vblank_in(NULL))
            Sleep(0);
        while (qdd_vblank_in(NULL))
            Sleep(0);
        return DD_OK;
    }
    qdd_vblank_wait();
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetAvailableVidMem(typename T::Caps *caps, DWORD *total, DWORD *free_mem)
{
    QDD_INIT_CHECK();
    if (!total && !free_mem)
        return DDERR_INVALIDPARAMS;
    QddLock lock;
    if (total)
        *total = QDD_VIDMEM_TOTAL;
    if (free_mem)
        *free_mem = QDD_VIDMEM_TOTAL - g_vidmem_used;
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetSurfaceFromDC(HDC dc, typename T::Surf **out)
{
    QDD_INIT_CHECK();
    if (!out)
        return E_INVALIDARG;
    QddLock lock;
    if (dc) {
        for (QddSurface *s = dd->surfaces; s; s = s->dd_next) {
            if (s->dc == dc) {
                /* Through IDirectDraw4 native hands out the surface's
                 * IDirectDrawSurface interface (ddraw4.c
                 * test_get_surface_from_dc). */
                const int rv = V == 4 ? 1 : SV;
                qdd_surf_addref(s, qdd_vidx(rv));
                *out = (typename T::Surf *)qdd_surf_iface(s, rv);
                return DD_OK;
            }
        }
    }
    *out = NULL;
    return DDERR_NOTFOUND;
}

template <int V> HRESULT QddDrawIface<V>::RestoreAllSurfaces()
{
    QDD_INIT_CHECK();
    QddLock lock;
    for (QddSurface *s = dd->surfaces; s; s = s->dd_next)
        s->lost = false;
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::TestCooperativeLevel()
{
    QDD_INIT_CHECK();
    QddLock lock;
    if (g_exclusive && g_exclusive != dd)
        return DDERR_EXCLUSIVEMODEALREADYSET;
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::GetDeviceIdentifier(typename T::DevId *id, DWORD flags)
{
    QDD_INIT_CHECK();
    if (!id)
        return DDERR_INVALIDPARAMS;
    D3DADAPTER_IDENTIFIER9 ai;
    memset(&ai, 0, sizeof(ai));
    if (!dd->d3d9 || FAILED(dd->d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ai))) {
        strcpy(ai.Driver, "qddraw.dll");
        strcpy(ai.Description, "QD3D DirectDraw");
    }
    DDDEVICEIDENTIFIER2 out;
    memset(&out, 0, sizeof(out));
    memcpy(out.szDriver, ai.Driver, sizeof(out.szDriver) - 1);
    memcpy(out.szDescription, ai.Description, sizeof(out.szDescription) - 1);
    out.liDriverVersion = ai.DriverVersion;
    out.dwVendorId = ai.VendorId;
    out.dwDeviceId = ai.DeviceId;
    out.dwSubSysId = ai.SubSysId;
    out.dwRevision = ai.Revision;
    out.guidDeviceIdentifier = ai.DeviceIdentifier;
    out.dwWHQLLevel = (flags & DDGDI_GETHOSTIDENTIFIER) ? 0 : ai.WHQLLevel;
    memcpy(id, &out, sizeof(*id));
    return DD_OK;
}

template <int V> HRESULT QddDrawIface<V>::StartModeTest(SIZE *modes, DWORD count, DWORD flags)
{
    QDD_INIT_CHECK();
    return DDERR_TESTFINISHED;
}

template <int V> HRESULT QddDrawIface<V>::EvaluateMode(DWORD flags, DWORD *timeout)
{
    QDD_INIT_CHECK();
    return DDERR_INVALIDOBJECT;
}

template class QddDrawIface<1>;
template class QddDrawIface<2>;
template class QddDrawIface<4>;
template class QddDrawIface<7>;

/* ------------------------------------------------------------------------ */
/* Video memory accounting (surfaces call these)                             */

void qdd_vidmem_add(DWORD bytes)
{
    g_vidmem_used = bytes > QDD_VIDMEM_TOTAL - g_vidmem_used ? QDD_VIDMEM_TOTAL : g_vidmem_used + bytes;
}

void qdd_vidmem_sub(DWORD bytes)
{
    g_vidmem_used = bytes > g_vidmem_used ? 0 : g_vidmem_used - bytes;
}

bool qdd_vidmem_fits(DWORD bytes)
{
    return bytes <= QDD_VIDMEM_TOTAL - g_vidmem_used;
}
