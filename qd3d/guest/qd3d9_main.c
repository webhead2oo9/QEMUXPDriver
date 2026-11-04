/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Direct3D 9 front end of the guest proxy: DLL entry points and exports,
 * IUnknown, IDirect3D9 adapter queries from guest GDI, device creation,
 * Reset and Present (extension operations), and the guest-local device,
 * swap chain, query, shader, declaration, and state block methods.
 */
#include <stdlib.h>
#include <string.h>

#include "qd3d9_priv.h"

/* ------------------------------------------------------------------------ */
/* Interface identifiers (values from the public Direct3D 9 headers)        */
/* ------------------------------------------------------------------------ */
static const GUID qd3d_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static const GUID qd3d_iids[QD3D_T_COUNT] = {
    [QD3D_T_IUnknown] = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}},
    [QD3D_T_IDirect3D9] = {0x81bdcbca, 0x64d4, 0x426d, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}},
    [QD3D_T_IDirect3DDevice9] = {0xd0223b96, 0xbf7a, 0x43fd, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}},
    [QD3D_T_IDirect3DStateBlock9] = {0xb07c4fe5, 0x310d, 0x4ba8, {0xa2, 0x3c, 0x4f, 0x0f, 0x20, 0x6f, 0x21, 0x8b}},
    [QD3D_T_IDirect3DSwapChain9] = {0x794950f2, 0xadfc, 0x458a, {0x90, 0x5e, 0x10, 0xa1, 0x0b, 0x0b, 0x50, 0x3b}},
    [QD3D_T_IDirect3DResource9] = {0x05eec05d, 0x8f7d, 0x4362, {0xb9, 0x99, 0xd1, 0xba, 0xf3, 0x57, 0xc7, 0x04}},
    [QD3D_T_IDirect3DVertexDeclaration9] = {0xdd13c59c, 0x36fa, 0x4098, {0xa8, 0xfb, 0xc7, 0xed, 0x39, 0xdc, 0x85, 0x46}},
    [QD3D_T_IDirect3DVertexShader9] = {0xefc5557e, 0x6265, 0x4613, {0x8a, 0x94, 0x43, 0x85, 0x78, 0x89, 0xeb, 0x36}},
    [QD3D_T_IDirect3DPixelShader9] = {0x6d3bdbdc, 0x5b02, 0x4415, {0xb8, 0x52, 0xce, 0x5e, 0x8b, 0xcc, 0xb2, 0x89}},
    [QD3D_T_IDirect3DBaseTexture9] = {0x580ca87e, 0x1d3c, 0x4d54, {0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce}},
    [QD3D_T_IDirect3DTexture9] = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}},
    [QD3D_T_IDirect3DVolumeTexture9] = {0x2518526c, 0xe789, 0x4111, {0xa7, 0xb9, 0x47, 0xef, 0x32, 0x8d, 0x13, 0xe6}},
    [QD3D_T_IDirect3DCubeTexture9] = {0xfff32f81, 0xd953, 0x473a, {0x92, 0x23, 0x93, 0xd6, 0x52, 0xab, 0xa9, 0x3f}},
    [QD3D_T_IDirect3DVertexBuffer9] = {0xb64bb1b5, 0xfd70, 0x4df6, {0xbf, 0x91, 0x19, 0xd0, 0xa1, 0x24, 0x55, 0xe3}},
    [QD3D_T_IDirect3DIndexBuffer9] = {0x7c9dd65e, 0xd3f7, 0x4529, {0xac, 0xee, 0x78, 0x58, 0x30, 0xac, 0xde, 0x35}},
    [QD3D_T_IDirect3DSurface9] = {0x0cfbaf3a, 0x9ff6, 0x429a, {0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b}},
    [QD3D_T_IDirect3DVolume9] = {0x24f416e6, 0x1f67, 0x4aa7, {0xb8, 0x8e, 0xd3, 0x3f, 0x6f, 0x31, 0x28, 0xa1}},
    [QD3D_T_IDirect3DQuery9] = {0xd9771460, 0xa695, 0x4f26, {0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc}},
};

int qd3d_iid_equal(REFIID a, REFIID b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

HRESULT qd3d_query_interface(struct qd3d_obj *o, REFIID riid, void **ppv)
{
    uint32_t t;

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (!riid)
        return E_POINTER;
    if (qd3d_iid_equal(riid, &qd3d_iid_unknown) ||
        (o->type == QD3D_T_IDirect3D9 && qd3d_iid_equal(riid, &IID_QD3D_REMOTE))) {
        qd3d_obj_addref(o);
        *ppv = o;
        return S_OK;
    }
    for (t = o->type; t != QD3D_T_NONE && t < (uint32_t)QD3D_T_COUNT; t = qd3d_type_base[t]) {
        if (qd3d_iid_equal(riid, &qd3d_iids[t])) {
            qd3d_obj_addref(o);
            *ppv = o;
            return S_OK;
        }
    }
    return E_NOINTERFACE;
}

/* ------------------------------------------------------------------------ */
/* Process state                                                            */
/* ------------------------------------------------------------------------ */

static HMODULE g_self;
static HMODULE g_stock;
static int g_stock_tried;        /* load_stock ran: refused or missing stays so */
static int g_session;            /* HELLO accepted, GOODBYE not yet sent */
static uint32_t g_d3d_id;        /* the session's IDirect3D9 on the wire */

static void ext_free_plain(struct qd3d_obj *o)
{
    free(o->ext);
    o->ext = NULL;
}

/* ------------------------------------------------------------------------ */
/* IUnknown                                                                 */
/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE qd3d9o_IUnknown_QueryInterface(IUnknown *This, REFIID riid, void **ppvObj)
{
    return qd3d_query_interface(qd3d_obj(This), riid, ppvObj);
}

ULONG STDMETHODCALLTYPE qd3d9o_IUnknown_AddRef(IUnknown *This)
{
    return qd3d_obj_addref(qd3d_obj(This));
}

ULONG STDMETHODCALLTYPE qd3d9o_IUnknown_Release(IUnknown *This)
{
    return qd3d_obj_release(qd3d_obj(This));
}

/* ------------------------------------------------------------------------ */
/* Display modes from guest GDI                                             */
/* ------------------------------------------------------------------------ */

static D3DFORMAT bpp_format(DWORD bpp)
{
    switch (bpp) {
    case 32: return D3DFMT_X8R8G8B8;
    case 24: return D3DFMT_R8G8B8;
    case 16: return D3DFMT_R5G6B5;
    case 8:  return D3DFMT_P8;
    default: return D3DFMT_UNKNOWN;
    }
}

static UINT format_bpp(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A2R10G10B10:
        return 32;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        return 16;
    default:
        return 0;
    }
}

/* Display format a back buffer format is shown in. */
static D3DFORMAT display_format(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: return D3DFMT_X8R8G8B8;
    case D3DFMT_A1R5G5B5: return D3DFMT_X1R5G5B5;
    default:              return f;
    }
}

HRESULT qd3d_current_mode(D3DDISPLAYMODE *m)
{
    DEVMODEA dm;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        return D3DERR_NOTAVAILABLE;
    m->Width = dm.dmPelsWidth;
    m->Height = dm.dmPelsHeight;
    m->RefreshRate = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
    m->Format = bpp_format(dm.dmBitsPerPel);
    return D3D_OK;
}

static D3DDISPLAYMODE *g_modes;
static UINT g_mode_count;
static int g_modes_built;

/* Adapter modes enumerated once (under the runtime lock). Only formats
 * Direct3D 9 enumerates as display formats are kept, deduplicated. */
static void build_modes(void)
{
    DEVMODEA dm;
    DWORD i;
    UINT cap = 0;

    if (g_modes_built)
        return;
    g_modes_built = 1;
    for (i = 0;; i++) {
        D3DDISPLAYMODE m;
        UINT j;
        int dup = 0;

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            break;
        if (dm.dmBitsPerPel != 32 && dm.dmBitsPerPel != 16)
            continue;
        m.Width = dm.dmPelsWidth;
        m.Height = dm.dmPelsHeight;
        m.RefreshRate = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
        m.Format = bpp_format(dm.dmBitsPerPel);
        for (j = 0; j < g_mode_count && !dup; j++)
            dup = g_modes[j].Width == m.Width && g_modes[j].Height == m.Height &&
                  g_modes[j].RefreshRate == m.RefreshRate && g_modes[j].Format == m.Format;
        if (dup)
            continue;
        if (g_mode_count == cap) {
            UINT ncap = cap ? cap * 2 : 64;
            D3DDISPLAYMODE *n = (D3DDISPLAYMODE *)realloc(g_modes, ncap * sizeof(*n));
            if (!n)
                break;
            g_modes = n;
            cap = ncap;
        }
        g_modes[g_mode_count++] = m;
    }
}

/* Index of the n-th mode with format f, or -1. */
static int find_mode(D3DFORMAT f, UINT n)
{
    UINT i;

    for (i = 0; i < g_mode_count; i++)
        if (g_modes[i].Format == f && n-- == 0)
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* IDirect3D9 (guest-local)                                                 */
/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3D9_RegisterSoftwareDevice(IDirect3D9 *This, void *pInitializeFunction)
{
    return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE qd3d9o_IDirect3D9_GetAdapterCount(IDirect3D9 *This)
{
    return 1;
}

UINT STDMETHODCALLTYPE qd3d9o_IDirect3D9_GetAdapterModeCount(IDirect3D9 *This, UINT Adapter, D3DFORMAT Format)
{
    UINT n = 0, i;

    if (Adapter != 0)
        return 0;
    qd3d_rt_lock();
    build_modes();
    for (i = 0; i < g_mode_count; i++)
        if (g_modes[i].Format == Format)
            n++;
    qd3d_rt_unlock();
    return n;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3D9_EnumAdapterModes(IDirect3D9 *This, UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode)
{
    int i;

    if (Adapter != 0 || !pMode)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    build_modes();
    i = find_mode(Format, Mode);
    if (i >= 0)
        *pMode = g_modes[i];
    qd3d_rt_unlock();
    return i >= 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3D9_GetAdapterDisplayMode(IDirect3D9 *This, UINT Adapter, D3DDISPLAYMODE *pMode)
{
    if (Adapter != 0 || !pMode)
        return D3DERR_INVALIDCALL;
    return qd3d_current_mode(pMode);
}

HMONITOR STDMETHODCALLTYPE qd3d9o_IDirect3D9_GetAdapterMonitor(IDirect3D9 *This, UINT Adapter)
{
    POINT pt = {0, 0};

    if (Adapter != 0)
        return NULL;
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

/* ------------------------------------------------------------------------ */
/* Devices                                                                  */
/* ------------------------------------------------------------------------ */

/* Native Direct3D 9 keeps bound objects alive after the application released
 * them (the public count reads 0) and hands the same object back from Get*;
 * games rely on both (Need for Speed: Underground 2 releases bound objects
 * twice during a mode change). These slots hold what the device and each
 * state block keep alive. */
#define QD3D_TEX_SLOTS    21u /* samplers 0-15, D3DDMAPSAMPLER, vertex 0-3 */
#define QD3D_STREAM_SLOTS 16u
#define QD3D_RT_SLOTS     4u
#define QD3D_OWNED_MAX    8u

struct qd3d_binds {
    struct qd3d_obj *tex[QD3D_TEX_SLOTS];
    struct qd3d_obj *stream[QD3D_STREAM_SLOTS];
    struct qd3d_obj *indices, *decl, *vs, *ps;
};

/* State block: the bindings it contains and the objects it recorded. */
struct qd3d_sb {
    uint32_t tex_mask, stream_mask;
    uint8_t indices, decl, vs, ps;
    struct qd3d_binds b;
};

struct qd3d_dev {
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DPRESENT_PARAMETERS pp;     /* as the application sees it (resolved) */
    D3DCAPS9 caps;                /* the device's caps, fetched once */
    int caps_ok;
    int mode_changed;             /* guest display mode changed (VM only) */
    HWND fs_window;               /* window restyled for fullscreen */
    LONG saved_style;
    LONG saved_exstyle;
    RECT saved_rect;
    HCURSOR cursor;               /* SetCursorProperties, a Windows cursor */
    BOOL cursor_visible;          /* ShowCursor */
    struct qd3d_binds b;                    /* bound now */
    struct qd3d_obj *rt[QD3D_RT_SLOTS];     /* set render targets; NULL: default */
    struct qd3d_obj *ds;                    /* set depth stencil when ds_set */
    int ds_set;
    struct qd3d_obj *owned[QD3D_OWNED_MAX]; /* implicit swap chain, back
                                               buffers, automatic depth */
    unsigned owned_count;
    struct qd3d_sb *recording;              /* BeginStateBlock..EndStateBlock */
    volatile LONG flush;                    /* a bound resource may have
                                               writes the host lacks */
    int in_scene;                           /* BeginScene..EndScene */
    int has_decl;                           /* a declaration or FVF may be
                                               set (0 only when certainly
                                               none: creation, Reset, NULL) */
    DWORD fvf;                              /* FVF set last, 0 once a
                                               declaration replaced it */
    int presented;                          /* presented once: counts in
                                               g_cursor.devices */
};

/* An additional swap chain (CreateAdditionalSwapChain): its parameters as
 * the application sees them (window resolved) and the back buffers it owns
 * (held: natively they live as long as the swap chain, each with a count
 * of its own and a device reference while the application holds it). The
 * implicit swap chain has none; it answers the device's parameters. */
#define QD3D_SC_BUFFERS 3u

struct qd3d_sc {
    D3DPRESENT_PARAMETERS pp;
    struct qd3d_obj *bb[QD3D_SC_BUFFERS];
};

static struct qd3d_dev *dev_ext(const void *iface)
{
    struct qd3d_obj *d = qd3d_obj_device(iface);
    return d ? (struct qd3d_dev *)d->ext : NULL;
}

int qd3d_dev_in_scene(struct qd3d_obj *dev)
{
    struct qd3d_dev *d = dev ? (struct qd3d_dev *)dev->ext : NULL;

    return d && d->in_scene;
}

const D3DCAPS9 *qd3d_dev_caps(struct qd3d_obj *dev)
{
    struct qd3d_dev *d = dev ? (struct qd3d_dev *)dev->ext : NULL;

    return d && d->caps_ok ? &d->caps : NULL;
}

void qd3d_dev_note_flush(struct qd3d_obj *dev)
{
    struct qd3d_dev *d = dev ? (struct qd3d_dev *)dev->ext : NULL;

    if (d)
        InterlockedExchange(&d->flush, 1);
}

/* Before a draw: the resources it can read upload what the host lacks
 * (managed resources upload at first use, as with the native runtime). */
static void dev_flush(struct qd3d_dev *d)
{
    unsigned i;

    if (!InterlockedExchange(&d->flush, 0))
        return;
    qd3d_rt_lock();
    for (i = 0; i < QD3D_TEX_SLOTS; i++)
        qd3d_res_flush(d->b.tex[i]);
    for (i = 0; i < QD3D_STREAM_SLOTS; i++)
        qd3d_res_flush(d->b.stream[i]);
    qd3d_res_flush(d->b.indices);
    qd3d_rt_unlock();
}

void qd3d_dev_flush(struct qd3d_obj *dev)
{
    struct qd3d_dev *d = dev ? (struct qd3d_dev *)dev->ext : NULL;

    if (d && d->flush)
        dev_flush(d);
}

/* A newly bound resource with pending writes: the next draw flushes. */
static void bind_note(struct qd3d_dev *d, const void *iface)
{
    if (!d->recording && qd3d_res_has_pending(iface))
        InterlockedExchange(&d->flush, 1);
}

/* A bound level surface keeps its container alive as well: native levels
 * share their texture's lifetime. */
static void bind_hold(struct qd3d_obj *o)
{
    qd3d_obj_hold(o);
    if (o->owned_level && o->parent)
        qd3d_obj_hold(o->parent);
}

static void bind_unhold(struct qd3d_obj *o)
{
    struct qd3d_obj *c = o->owned_level ? o->parent : NULL;

    qd3d_obj_unhold(o);
    if (c)
        qd3d_obj_unhold(c);
}

/* Points *slot at o: the slot holds o and lets go of what it held. */
static void slot_set(struct qd3d_obj **slot, struct qd3d_obj *o)
{
    struct qd3d_obj *old;

    qd3d_rt_lock();
    old = *slot;
    if (old != o) {
        if (o)
            bind_hold(o);
        *slot = o;
    }
    qd3d_rt_unlock();
    if (old && old != o)
        bind_unhold(old);
}

static void binds_clear(struct qd3d_binds *b)
{
    unsigned i;

    for (i = 0; i < QD3D_TEX_SLOTS; i++)
        slot_set(&b->tex[i], NULL);
    for (i = 0; i < QD3D_STREAM_SLOTS; i++)
        slot_set(&b->stream[i], NULL);
    slot_set(&b->indices, NULL);
    slot_set(&b->decl, NULL);
    slot_set(&b->vs, NULL);
    slot_set(&b->ps, NULL);
}

/* Copies the bindings sb contains (Capture: device to block, Apply: block to
 * device). */
static void binds_copy(const struct qd3d_sb *sb, struct qd3d_binds *to, const struct qd3d_binds *from)
{
    unsigned i;

    for (i = 0; i < QD3D_TEX_SLOTS; i++)
        if (sb->tex_mask & (1u << i))
            slot_set(&to->tex[i], from->tex[i]);
    for (i = 0; i < QD3D_STREAM_SLOTS; i++)
        if (sb->stream_mask & (1u << i))
            slot_set(&to->stream[i], from->stream[i]);
    if (sb->indices)
        slot_set(&to->indices, from->indices);
    if (sb->decl)
        slot_set(&to->decl, from->decl);
    if (sb->vs)
        slot_set(&to->vs, from->vs);
    if (sb->ps)
        slot_set(&to->ps, from->ps);
}

static void sb_free(struct qd3d_sb *sb)
{
    if (sb) {
        binds_clear(&sb->b);
        free(sb);
    }
}

static void sb_ext_free(struct qd3d_obj *o)
{
    sb_free((struct qd3d_sb *)o->ext);
    o->ext = NULL;
}

static void sb_attach(void *iface, struct qd3d_sb *sb)
{
    struct qd3d_obj *o = qd3d_obj(iface);

    qd3d_rt_lock();
    if (!o->ext) {
        o->ext = sb;
        o->ext_free = sb_ext_free;
        sb = NULL;
    }
    qd3d_rt_unlock();
    sb_free(sb);
}

/* The device keeps its implicit objects (swap chain, back buffers, automatic
 * depth stencil) alive until Reset or its end, as native Direct3D does. */
static void own_implicit(struct qd3d_dev *d, void *iface)
{
    struct qd3d_obj *o = qd3d_obj(iface);
    unsigned i;

    qd3d_rt_lock();
    for (i = 0; i < d->owned_count; i++)
        if (d->owned[i] == o)
            break;
    if (i == d->owned_count && i < QD3D_OWNED_MAX) {
        d->owned[d->owned_count++] = o;
        qd3d_obj_hold(o);
    }
    qd3d_rt_unlock();
}

/* Reset and the device's end: nothing stays bound or owned. */
static void dev_unbind_all(struct qd3d_dev *d)
{
    unsigned i, n;

    sb_free(d->recording);
    d->recording = NULL;
    binds_clear(&d->b);
    for (i = 0; i < QD3D_RT_SLOTS; i++)
        slot_set(&d->rt[i], NULL);
    slot_set(&d->ds, NULL);
    d->ds_set = 0;
    qd3d_rt_lock();
    n = d->owned_count;
    d->owned_count = 0;
    qd3d_rt_unlock();
    while (n--)
        qd3d_obj_unhold(d->owned[n]);
}

void qd3d_dev_teardown(struct qd3d_obj *dev)
{
    struct qd3d_dev *d = (struct qd3d_dev *)dev->ext;

    if (d)
        dev_unbind_all(d);
}

static void leave_fullscreen(struct qd3d_dev *d)
{
    if (d->mode_changed) {
        ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
        d->mode_changed = 0;
    }
    if (d->fs_window) {
        if (IsWindow(d->fs_window)) {
            SetWindowLongA(d->fs_window, GWL_STYLE, d->saved_style);
            SetWindowLongA(d->fs_window, GWL_EXSTYLE, d->saved_exstyle);
            SetWindowPos(d->fs_window, HWND_NOTOPMOST, d->saved_rect.left, d->saved_rect.top,
                         d->saved_rect.right - d->saved_rect.left,
                         d->saved_rect.bottom - d->saved_rect.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
        d->fs_window = NULL;
    }
}

/* Fullscreen: on the VM transport the guest display mode really changes so
 * that guest input coordinates match the published frame. In loopback the
 * display belongs to the developer's desktop and is never touched. */
static HRESULT enter_fullscreen(struct qd3d_dev *d, const D3DPRESENT_PARAMETERS *pp, HWND w)
{
    DEVMODEA dm;
    UINT bpp;
    LONG r;

    if (qd3d_rt_transport() != QD3D_TRANSPORT_VM)
        return D3D_OK;
    bpp = format_bpp(pp->BackBufferFormat);
    if (!bpp)
        return D3DERR_INVALIDCALL;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = pp->BackBufferWidth;
    dm.dmPelsHeight = pp->BackBufferHeight;
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (pp->FullScreen_RefreshRateInHz) {
        dm.dmDisplayFrequency = pp->FullScreen_RefreshRateInHz;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    r = ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        qd3d_rt_log("ChangeDisplaySettingsEx %ux%ux%u failed (%ld)", (unsigned)dm.dmPelsWidth,
                    (unsigned)dm.dmPelsHeight, (unsigned)bpp, (long)r);
        return D3DERR_NOTAVAILABLE;
    }
    d->mode_changed = 1;
    if (w && IsWindow(w) && !d->fs_window) {
        d->fs_window = w;
        d->saved_style = GetWindowLongA(w, GWL_STYLE);
        d->saved_exstyle = GetWindowLongA(w, GWL_EXSTYLE);
        GetWindowRect(w, &d->saved_rect);
        SetWindowLongA(w, GWL_STYLE, (d->saved_style & ~(LONG)(WS_CAPTION | WS_THICKFRAME)) | (LONG)WS_POPUP);
        SetWindowLongA(w, GWL_EXSTYLE, d->saved_exstyle & ~(LONG)(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
    }
    if (d->fs_window)
        SetWindowPos(d->fs_window, HWND_TOPMOST, 0, 0, (int)pp->BackBufferWidth,
                     (int)pp->BackBufferHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    return D3D_OK;
}

static void cursor_cache_forget(HCURSOR h);
static void cursor_poll_forget(void);
static void source_forget(uint32_t id);

static void dev_ext_free(struct qd3d_obj *o)
{
    struct qd3d_dev *d = (struct qd3d_dev *)o->ext;

    if (d) {
        dev_unbind_all(d);
        leave_fullscreen(d);
        if (d->cursor) {
            /* The device's cursor goes with it. */
            if (d->cursor_visible && GetCursor() == d->cursor)
                SetCursor(NULL);
            cursor_cache_forget(d->cursor);
            DestroyCursor(d->cursor);
        }
        if (d->presented)
            cursor_poll_forget();
        free(d);
    }
    source_forget(o->id);
    o->ext = NULL;
}

/* Resolves defaults the application left to the runtime. */
static HRESULT resolve_pp(HWND focus, D3DPRESENT_PARAMETERS *pp)
{
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;

    if (pp->BackBufferCount == 0)
        pp->BackBufferCount = 1;
    if (pp->Windowed) {
        if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
            RECT rc = {0, 0, 0, 0};
            if (w)
                GetClientRect(w, &rc);
            if (!pp->BackBufferWidth)
                pp->BackBufferWidth = rc.right > rc.left ? (UINT)(rc.right - rc.left) : 1u;
            if (!pp->BackBufferHeight)
                pp->BackBufferHeight = rc.bottom > rc.top ? (UINT)(rc.bottom - rc.top) : 1u;
        }
        if (pp->BackBufferFormat == D3DFMT_UNKNOWN) {
            D3DDISPLAYMODE m;
            if (FAILED(qd3d_current_mode(&m)))
                return D3DERR_NOTAVAILABLE;
            pp->BackBufferFormat = m.Format;
        }
    } else if (!w || !pp->BackBufferWidth || !pp->BackBufferHeight ||
               pp->BackBufferFormat == D3DFMT_UNKNOWN) {
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

static void pp_to_wire(const D3DPRESENT_PARAMETERS *pp, struct qd3d_wire_pp *w)
{
    w->BackBufferWidth = pp->BackBufferWidth;
    w->BackBufferHeight = pp->BackBufferHeight;
    w->BackBufferFormat = (uint32_t)pp->BackBufferFormat;
    w->BackBufferCount = pp->BackBufferCount;
    w->MultiSampleType = (uint32_t)pp->MultiSampleType;
    w->MultiSampleQuality = pp->MultiSampleQuality;
    w->SwapEffect = (uint32_t)pp->SwapEffect;
    w->hDeviceWindow = (uint32_t)(uintptr_t)pp->hDeviceWindow;
    w->Windowed = (uint32_t)pp->Windowed;
    w->EnableAutoDepthStencil = (uint32_t)pp->EnableAutoDepthStencil;
    w->AutoDepthStencilFormat = (uint32_t)pp->AutoDepthStencilFormat;
    w->Flags = pp->Flags;
    w->FullScreen_RefreshRateInHz = pp->FullScreen_RefreshRateInHz;
    w->PresentationInterval = pp->PresentationInterval;
}

/* Values the host accepted that the application can observe. */
static void pp_from_wire(D3DPRESENT_PARAMETERS *pp, const struct qd3d_wire_pp *w)
{
    pp->BackBufferWidth = w->BackBufferWidth;
    pp->BackBufferHeight = w->BackBufferHeight;
    pp->BackBufferFormat = (D3DFORMAT)w->BackBufferFormat;
    pp->BackBufferCount = w->BackBufferCount;
}

/* Without D3DCREATE_FPU_PRESERVE, Direct3D puts the creating thread's x87
 * unit into single precision, round-to-nearest, with every exception masked,
 * and games rely on it: Need for Speed: Underground 2 runs with invalid
 * operation unmasked and traps on its first NaN unless device creation
 * masks it. Same control word change as the native runtime and Wine. */
static void fpu_setup(void)
{
    unsigned short cw;

    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0x0f3fu) | 0x003fu);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

/* CreateDevice and Reset failures reach the persistent log, the first few
 * of each per process (a game may retry in a loop). */
static void note_device_failure(LONG *budget, const char *what, HRESULT hr, DWORD flags,
                                const D3DPRESENT_PARAMETERS *pp)
{
    if (InterlockedDecrement(budget) < 0)
        return;
    if (pp)
        qd3d_rt_event("%s failed (hr 0x%08lx): behavior %#lx, %ux%u format %u, %s", what,
                      (unsigned long)hr, (unsigned long)flags, pp->BackBufferWidth, pp->BackBufferHeight,
                      (unsigned)pp->BackBufferFormat, pp->Windowed ? "windowed" : "fullscreen");
    else
        qd3d_rt_event("%s failed (hr 0x%08lx)", what, (unsigned long)hr);
}

static HRESULT create_device(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    struct qd3d_obj *d3d = qd3d_obj(This);
    struct qd3d_create_device cd;
    struct qd3d_wire_pp acc;
    D3DPRESENT_PARAMETERS pp;
    struct qd3d_dev *ext;
    struct qd3d_obj *dev;
    struct qd3d_call k;
    HRESULT hr;
    int got;

    if (!ppReturnedDeviceInterface)
        return D3DERR_INVALIDCALL;
    *ppReturnedDeviceInterface = NULL;
    if (!pPresentationParameters || Adapter != 0)
        return D3DERR_INVALIDCALL;
    if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE))
        fpu_setup();
    pp = *pPresentationParameters;
    hr = resolve_pp(hFocusWindow, &pp);
    if (FAILED(hr))
        return hr;
    ext = (struct qd3d_dev *)calloc(1, sizeof(*ext));
    if (!ext)
        return E_OUTOFMEMORY;
    ext->cp.AdapterOrdinal = Adapter;
    ext->cp.DeviceType = DeviceType;
    ext->cp.hFocusWindow = hFocusWindow;
    ext->cp.BehaviorFlags = BehaviorFlags;
    ext->cursor_visible = FALSE;
    if (!pp.Windowed) {
        hr = enter_fullscreen(ext, &pp, pp.hDeviceWindow ? pp.hDeviceWindow : hFocusWindow);
        if (FAILED(hr)) {
            free(ext);
            return hr;
        }
    }

    dev = qd3d_obj_new(NULL, QD3D_T_IDirect3DDevice9);
    if (!dev) {
        leave_fullscreen(ext);
        free(ext);
        return E_OUTOFMEMORY;
    }
    memset(&cd, 0, sizeof(cd));
    cd.device_id = dev->id;
    cd.adapter = Adapter;
    cd.device_type = (uint32_t)DeviceType;
    cd.behavior_flags = BehaviorFlags;
    pp_to_wire(&pp, &cd.pp);
    qd3d_call_begin(&k, QD3D_OP_CREATE_DEVICE, d3d->id, 1, sizeof(cd));
    qd3d_put_raw(&k, &cd, sizeof(cd));
    hr = (HRESULT)qd3d_call_end(&k);
    got = SUCCEEDED(hr) && k.rleft == sizeof(acc) && qd3d_get_raw(&k, &acc, sizeof(acc));
    qd3d_call_done(&k);
    if (SUCCEEDED(hr) && !got) {
        qd3d_rt_log("CREATE_DEVICE reply malformed");
        hr = D3DERR_DRIVERINTERNALERROR;
    }
    if (FAILED(hr)) {
        qd3d_obj_discard(dev);
        leave_fullscreen(ext);
        free(ext);
        return hr;
    }
    pp_from_wire(&pp, &acc);
    ext->pp = pp;
    /* Validation answers locally from the device's caps (shader constant
     * limits and the like), so they are read once. */
    ext->caps_ok = SUCCEEDED(qd3d9g_IDirect3DDevice9_GetDeviceCaps((IDirect3DDevice9 *)dev, &ext->caps));
    dev->ext = ext;
    dev->ext_free = dev_ext_free;
    qd3d_obj_set_parent(dev, d3d);
    pp_from_wire(pPresentationParameters, &acc);
    *ppReturnedDeviceInterface = (IDirect3DDevice9 *)dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3D9_CreateDevice(IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    static LONG budget = 8;
    HRESULT hr = create_device(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                               ppReturnedDeviceInterface);

    if (FAILED(hr))
        note_device_failure(&budget, "CreateDevice", hr, BehaviorFlags, pPresentationParameters);
    return hr;
}

static HRESULT reset(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    struct qd3d_obj *dev = qd3d_obj(This);
    struct qd3d_dev *d = (struct qd3d_dev *)dev->ext;
    struct qd3d_wire_pp w, acc;
    D3DPRESENT_PARAMETERS pp;
    struct qd3d_call k;
    HRESULT hr;
    int got;

    if (!pPresentationParameters || !d)
        return D3DERR_INVALIDCALL;
    pp = *pPresentationParameters;
    hr = resolve_pp(d->cp.hFocusWindow, &pp);
    if (FAILED(hr))
        return hr;
    if (pp.Windowed) {
        leave_fullscreen(d);
    } else {
        hr = enter_fullscreen(d, &pp, pp.hDeviceWindow ? pp.hDeviceWindow : d->cp.hFocusWindow);
        if (FAILED(hr))
            return hr;
    }
    pp_to_wire(&pp, &w);
    qd3d_call_begin(&k, QD3D_OP_RESET, dev->id, 1, sizeof(w));
    qd3d_put_raw(&k, &w, sizeof(w));
    hr = (HRESULT)qd3d_call_end(&k);
    got = SUCCEEDED(hr) && k.rleft == sizeof(acc) && qd3d_get_raw(&k, &acc, sizeof(acc));
    qd3d_call_done(&k);
    if (SUCCEEDED(hr) && !got) {
        qd3d_rt_log("RESET reply malformed");
        hr = D3DERR_DRIVERINTERNALERROR;
    }
    if (FAILED(hr))
        return hr;
    pp_from_wire(&pp, &acc);
    d->pp = pp;
    /* Reset returns the device to its default state: nothing bound, new
     * implicit surfaces in place of the old ones. */
    dev_unbind_all(d);
    d->in_scene = 0;
    d->has_decl = 0;
    d->fvf = 0;
    pp_from_wire(pPresentationParameters, &acc);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_Reset(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    static LONG budget = 8;
    struct qd3d_dev *d = dev_ext(This);
    HRESULT hr = reset(This, pPresentationParameters);

    if (FAILED(hr))
        note_device_failure(&budget, "Reset", hr, d ? d->cp.BehaviorFlags : 0u, pPresentationParameters);
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Bindings, implicit objects and state blocks (lifetime bookkeeping)       */
/* ------------------------------------------------------------------------ */

static int tex_slot(DWORD stage)
{
    if (stage < 16u)
        return (int)stage;
    if (stage >= D3DDMAPSAMPLER && stage <= D3DVERTEXTEXTURESAMPLER3)
        return 16 + (int)(stage - D3DDMAPSAMPLER);
    return -1;
}

/* Bindings a Set* call changes: the state block being recorded, else the
 * device. */
static struct qd3d_binds *binds_for_set(struct qd3d_dev *d)
{
    return d->recording ? &d->recording->b : &d->b;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetTexture(IDirect3DDevice9 *This, DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetTexture(This, Stage, pTexture);
    struct qd3d_dev *d = dev_ext(This);
    int i = tex_slot(Stage);

    if (SUCCEEDED(hr) && d && i >= 0) {
        if (d->recording)
            d->recording->tex_mask |= 1u << i;
        slot_set(&binds_for_set(d)->tex[i], pTexture ? qd3d_obj(pTexture) : NULL);
        bind_note(d, pTexture);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetStreamSource(IDirect3DDevice9 *This, UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetStreamSource(This, StreamNumber, pStreamData, OffsetInBytes, Stride);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && StreamNumber < QD3D_STREAM_SLOTS) {
        if (d->recording)
            d->recording->stream_mask |= 1u << StreamNumber;
        slot_set(&binds_for_set(d)->stream[StreamNumber], pStreamData ? qd3d_obj(pStreamData) : NULL);
        bind_note(d, pStreamData);
    }
    return hr;
}

/* Draws fail natively without a vertex declaration (or FVF); checked here
 * so the asynchronous call can answer as the native runtime does. (A
 * missing index buffer is no error: Wine's test_draw_primitive passes on
 * the stock runtime.) */
static int draw_ok(const struct qd3d_dev *d)
{
    return !d || d->has_decl;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawPrimitive(IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    struct qd3d_dev *d = dev_ext(This);

    if (!draw_ok(d))
        return D3DERR_INVALIDCALL;
    if (d && d->flush)
        dev_flush(d);
    return qd3d9g_IDirect3DDevice9_DrawPrimitive(This, PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawIndexedPrimitive(IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
{
    struct qd3d_dev *d = dev_ext(This);

    if (!draw_ok(d))
        return D3DERR_INVALIDCALL;
    if (d && d->flush)
        dev_flush(d);
    return qd3d9g_IDirect3DDevice9_DrawIndexedPrimitive(This, PrimitiveType, BaseVertexIndex, MinVertexIndex,
                                                        NumVertices, startIndex, primCount);
}

/* The native runtime unbinds stream 0 after DrawPrimitiveUP, and the index
 * buffer too after DrawIndexedPrimitiveUP, even when the draw fails for
 * want of a declaration; so the host runs a failing draw too (it fails and
 * unbinds the same way), and what was bound is no longer kept alive. The
 * checks skip the lock in the common unbound case. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawPrimitiveUP(IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    struct qd3d_dev *d = dev_ext(This);
    int ok = draw_ok(d);
    HRESULT hr;

    if (ok && d && d->flush)
        dev_flush(d);
    hr = qd3d9g_IDirect3DDevice9_DrawPrimitiveUP(This, PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                                                 VertexStreamZeroStride);
    if (!ok)
        hr = D3DERR_INVALIDCALL;
    if ((SUCCEEDED(hr) || !ok) && d && d->b.stream[0])
        slot_set(&d->b.stream[0], NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawIndexedPrimitiveUP(IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    struct qd3d_dev *d = dev_ext(This);
    int ok = draw_ok(d);
    HRESULT hr;

    if (ok && d && d->flush)
        dev_flush(d);
    hr = qd3d9g_IDirect3DDevice9_DrawIndexedPrimitiveUP(This, PrimitiveType, MinVertexIndex, NumVertices,
                                                        PrimitiveCount, pIndexData, IndexDataFormat,
                                                        pVertexStreamZeroData, VertexStreamZeroStride);
    if (!ok)
        hr = D3DERR_INVALIDCALL;
    if ((SUCCEEDED(hr) || !ok) && d) {
        if (d->b.stream[0])
            slot_set(&d->b.stream[0], NULL);
        if (d->b.indices)
            slot_set(&d->b.indices, NULL);
    }
    return hr;
}

/* Scenes do not nest (measured: Wine's test_scene passes on the stock
 * runtime); Reset ends one. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_BeginScene(IDirect3DDevice9 *This)
{
    struct qd3d_dev *d = dev_ext(This);
    HRESULT hr;

    if (d && d->in_scene)
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DDevice9_BeginScene(This);
    if (SUCCEEDED(hr) && d)
        d->in_scene = 1;
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_EndScene(IDirect3DDevice9 *This)
{
    struct qd3d_dev *d = dev_ext(This);
    HRESULT hr;

    if (d && !d->in_scene)
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DDevice9_EndScene(This);
    if (SUCCEEDED(hr) && d)
        d->in_scene = 0;
    return hr;
}

/* Clearing depth or stencil needs a depth-stencil surface. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_Clear(IDirect3DDevice9 *This, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    struct qd3d_dev *d = dev_ext(This);

    if (d && (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) &&
        !(d->ds_set ? d->ds != NULL : d->pp.EnableAutoDepthStencil))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_Clear(This, Count, pRects, Flags, Color, Z, Stencil);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetFVF(IDirect3DDevice9 *This, DWORD FVF)
{
    struct qd3d_dev *d = dev_ext(This);
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetFVF(This, FVF);

    if (SUCCEEDED(hr) && d && !d->recording) {
        d->fvf = FVF;
        if (FVF)
            d->has_decl = 1;
    }
    return hr;
}

/* Native rules (Wine's stream_test passes on the stock runtime): a
 * setting needs a divider or a flag, not both flags, and stream 0 takes no
 * instance data. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetStreamSourceFreq(IDirect3DDevice9 *This, UINT StreamNumber, UINT Setting)
{
    const D3DCAPS9 *caps = qd3d_dev_caps(qd3d_obj_device(This));

    if (!Setting || ((Setting & D3DSTREAMSOURCE_INSTANCEDATA) && (Setting & D3DSTREAMSOURCE_INDEXEDDATA)) ||
        (!StreamNumber && (Setting & D3DSTREAMSOURCE_INSTANCEDATA)) ||
        (caps && StreamNumber >= caps->MaxStreams))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetStreamSourceFreq(This, StreamNumber, Setting);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetIndices(IDirect3DDevice9 *This, IDirect3DIndexBuffer9 *pIndexData)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetIndices(This, pIndexData);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d) {
        if (d->recording)
            d->recording->indices = 1;
        slot_set(&binds_for_set(d)->indices, pIndexData ? qd3d_obj(pIndexData) : NULL);
        bind_note(d, pIndexData);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetVertexDeclaration(IDirect3DDevice9 *This, IDirect3DVertexDeclaration9 *pDecl)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetVertexDeclaration(This, pDecl);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d) {
        if (d->recording) {
            d->recording->decl = 1;
        } else {
            d->has_decl = pDecl != NULL;
            d->fvf = 0;
        }
        slot_set(&binds_for_set(d)->decl, pDecl ? qd3d_obj(pDecl) : NULL);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetVertexShader(IDirect3DDevice9 *This, IDirect3DVertexShader9 *pShader)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetVertexShader(This, pShader);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d) {
        if (d->recording)
            d->recording->vs = 1;
        slot_set(&binds_for_set(d)->vs, pShader ? qd3d_obj(pShader) : NULL);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetPixelShader(IDirect3DDevice9 *This, IDirect3DPixelShader9 *pShader)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_SetPixelShader(This, pShader);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d) {
        if (d->recording)
            d->recording->ps = 1;
        slot_set(&binds_for_set(d)->ps, pShader ? qd3d_obj(pShader) : NULL);
    }
    return hr;
}

/* Shader constant registers the native runtime accepts (measured on the
 * stock 32-bit runtime): vertex float registers up to the device's
 * MaxVertexShaderConst, integer and boolean registers 16; devices created
 * for software or mixed vertex processing take 8192 float and 2048 integer
 * and boolean vertex registers whatever the current mode; pixel float
 * registers 224 with ps_3_0 and 32 below, pixel integer and boolean 16. A
 * range beyond the limit fails at once (games probe for the limit with a
 * loop that only ends on failure). */
enum { QD3D_CONST_F, QD3D_CONST_I, QD3D_CONST_B };

static int const_ok(const void *iface, int pixel, int kind, UINT start, UINT count)
{
    struct qd3d_dev *d = dev_ext(iface);
    UINT limit;

    if (!d)
        return 1;
    if (pixel) {
        limit = kind != QD3D_CONST_F ? 16u
                : (d->caps_ok && (d->caps.PixelShaderVersion & 0xffffu) >= 0x0300u) ? 224u : 32u;
    } else if (d->cp.BehaviorFlags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) {
        limit = kind == QD3D_CONST_F ? 8192u : 2048u;
    } else {
        limit = kind != QD3D_CONST_F ? 16u : d->caps_ok ? d->caps.MaxVertexShaderConst : 256u;
    }
    return start <= limit && count <= limit - start;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetVertexShaderConstantF(IDirect3DDevice9 *This, UINT StartRegister, const float *pConstantData, UINT Vector4fCount)
{
    if (!const_ok(This, 0, QD3D_CONST_F, StartRegister, Vector4fCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetVertexShaderConstantF(This, StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetVertexShaderConstantI(IDirect3DDevice9 *This, UINT StartRegister, const int *pConstantData, UINT Vector4iCount)
{
    if (!const_ok(This, 0, QD3D_CONST_I, StartRegister, Vector4iCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetVertexShaderConstantI(This, StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetVertexShaderConstantB(IDirect3DDevice9 *This, UINT StartRegister, const BOOL *pConstantData, UINT BoolCount)
{
    if (!const_ok(This, 0, QD3D_CONST_B, StartRegister, BoolCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetVertexShaderConstantB(This, StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetPixelShaderConstantF(IDirect3DDevice9 *This, UINT StartRegister, const float *pConstantData, UINT Vector4fCount)
{
    if (!const_ok(This, 1, QD3D_CONST_F, StartRegister, Vector4fCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetPixelShaderConstantF(This, StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetPixelShaderConstantI(IDirect3DDevice9 *This, UINT StartRegister, const int *pConstantData, UINT Vector4iCount)
{
    if (!const_ok(This, 1, QD3D_CONST_I, StartRegister, Vector4iCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetPixelShaderConstantI(This, StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetPixelShaderConstantB(IDirect3DDevice9 *This, UINT StartRegister, const BOOL *pConstantData, UINT BoolCount)
{
    if (!const_ok(This, 1, QD3D_CONST_B, StartRegister, BoolCount))
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DDevice9_SetPixelShaderConstantB(This, StartRegister, pConstantData, BoolCount);
}

/* Render targets and the depth stencil are not state-block state: they
 * always change the device. Natively render target 0 cannot be unset, the
 * index is bounded by NumSimultaneousRTs, and surfaces must be this
 * device's render targets (depth stencils for SetDepthStencilSurface). */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetRenderTarget(IDirect3DDevice9 *This, DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget)
{
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_surf_info si;
    HRESULT hr;

    if ((d && d->caps_ok && RenderTargetIndex >= d->caps.NumSimultaneousRTs) ||
        (!RenderTargetIndex && !pRenderTarget))
        return D3DERR_INVALIDCALL;
    if (pRenderTarget && (qd3d_obj_device(pRenderTarget) != qd3d_obj(This) ||
                          (qd3d_res_surf_info(pRenderTarget, &si) && !(si.usage & D3DUSAGE_RENDERTARGET))))
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DDevice9_SetRenderTarget(This, RenderTargetIndex, pRenderTarget);
    if (SUCCEEDED(hr) && d && RenderTargetIndex < QD3D_RT_SLOTS)
        slot_set(&d->rt[RenderTargetIndex], pRenderTarget ? qd3d_obj(pRenderTarget) : NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetDepthStencilSurface(IDirect3DDevice9 *This, IDirect3DSurface9 *pNewZStencil)
{
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_surf_info si;
    HRESULT hr;

    if (pNewZStencil && (qd3d_obj_device(pNewZStencil) != qd3d_obj(This) ||
                         (qd3d_res_surf_info(pNewZStencil, &si) && !(si.usage & D3DUSAGE_DEPTHSTENCIL))))
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DDevice9_SetDepthStencilSurface(This, pNewZStencil);
    if (SUCCEEDED(hr) && d) {
        d->ds_set = 1;
        slot_set(&d->ds, pNewZStencil ? qd3d_obj(pNewZStencil) : NULL);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetSwapChain(IDirect3DDevice9 *This, UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetSwapChain(This, iSwapChain, pSwapChain);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && pSwapChain && *pSwapChain)
        own_implicit(d, *pSwapChain);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetBackBuffer(IDirect3DDevice9 *This, UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetBackBuffer(This, iSwapChain, iBackBuffer, Type, ppBackBuffer);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && ppBackBuffer && *ppBackBuffer) {
        own_implicit(d, *ppBackBuffer);
        qd3d_res_set_swapchain(*ppBackBuffer, iSwapChain,
                               iSwapChain ? -1 : !!(d->pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER));
    }
    return hr;
}

/* An additional swap chain's back buffer belongs to it (held, the swap chain
 * its container); the implicit swap chain's are the device's. */
static void sc_own(struct qd3d_obj *sco, void *iface)
{
    struct qd3d_sc *sc = (struct qd3d_sc *)sco->ext;
    struct qd3d_obj *o = qd3d_obj(iface);
    unsigned i, slot = QD3D_SC_BUFFERS;

    qd3d_rt_lock();
    for (i = 0; i < QD3D_SC_BUFFERS && sc->bb[i] != o; i++)
        if (!sc->bb[i] && slot == QD3D_SC_BUFFERS)
            slot = i;
    if (i == QD3D_SC_BUFFERS && slot < QD3D_SC_BUFFERS) {
        sc->bb[slot] = o;
        qd3d_obj_hold(o);
        qd3d_res_set_sc_container(o, sco);
    }
    qd3d_rt_unlock();
}

/* Natively a failed swap chain GetBackBuffer leaves *ppBackBuffer alone
 * (Wine's test_swapchain on the stock runtime). */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetBackBuffer(IDirect3DSwapChain9 *This, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer)
{
    IDirect3DSurface9 *keep = ppBackBuffer ? *ppBackBuffer : NULL;
    HRESULT hr = qd3d9g_IDirect3DSwapChain9_GetBackBuffer(This, iBackBuffer, Type, ppBackBuffer);
    struct qd3d_dev *d = dev_ext(This);

    if (FAILED(hr)) {
        if (ppBackBuffer)
            *ppBackBuffer = keep;
        return hr;
    }
    if (d && ppBackBuffer && *ppBackBuffer) {
        if (qd3d_obj(This)->ext) {
            sc_own(qd3d_obj(This), *ppBackBuffer);
        } else {
            own_implicit(d, *ppBackBuffer);
            qd3d_res_set_swapchain(*ppBackBuffer, 0, -1);
        }
    }
    return hr;
}

/* With nothing set, these return the implicit back buffer and automatic
 * depth stencil, which the device owns. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetRenderTarget(IDirect3DDevice9 *This, DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetRenderTarget(This, RenderTargetIndex, ppRenderTarget);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && ppRenderTarget && *ppRenderTarget && RenderTargetIndex < QD3D_RT_SLOTS &&
        !d->rt[RenderTargetIndex]) {
        own_implicit(d, *ppRenderTarget);
        if (!RenderTargetIndex)
            qd3d_res_set_swapchain(*ppRenderTarget, 0, !!(d->pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetDepthStencilSurface(IDirect3DDevice9 *This, IDirect3DSurface9 **ppZStencilSurface)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetDepthStencilSurface(This, ppZStencilSurface);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && ppZStencilSurface && *ppZStencilSurface && !d->ds_set)
        own_implicit(d, *ppZStencilSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_BeginStateBlock(IDirect3DDevice9 *This)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_BeginStateBlock(This);
    struct qd3d_dev *d = dev_ext(This);

    if (SUCCEEDED(hr) && d && !d->recording)
        d->recording = (struct qd3d_sb *)calloc(1, sizeof(*d->recording));
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_EndStateBlock(IDirect3DDevice9 *This, IDirect3DStateBlock9 **ppSB)
{
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_sb *sb = d ? d->recording : NULL;
    HRESULT hr;

    if (d)
        d->recording = NULL;
    hr = qd3d9g_IDirect3DDevice9_EndStateBlock(This, ppSB);
    if (SUCCEEDED(hr) && ppSB && *ppSB && sb) {
        sb_attach(*ppSB, sb);
        sb = NULL;
    }
    sb_free(sb);
    return hr;
}

/* D3D9 state block types: textures, streams and indices belong to
 * D3DSBT_ALL only; the pixel and vertex types hold their shaders (and the
 * vertex declaration). */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateStateBlock(IDirect3DDevice9 *This, D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_CreateStateBlock(This, Type, ppSB);
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_sb *sb;

    if (FAILED(hr) || !d || !ppSB || !*ppSB)
        return hr;
    sb = (struct qd3d_sb *)calloc(1, sizeof(*sb));
    if (!sb)
        return hr;
    if (Type == D3DSBT_ALL) {
        sb->tex_mask = (1u << QD3D_TEX_SLOTS) - 1u;
        sb->stream_mask = (1u << QD3D_STREAM_SLOTS) - 1u;
        sb->indices = sb->decl = sb->vs = sb->ps = 1;
    } else if (Type == D3DSBT_PIXELSTATE) {
        sb->ps = 1;
    } else if (Type == D3DSBT_VERTEXSTATE) {
        sb->decl = sb->vs = 1;
    }
    binds_copy(sb, &sb->b, &d->b);
    sb_attach(*ppSB, sb);
    return hr;
}

/* A state block cannot be captured or applied while one is recorded. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DStateBlock9_Capture(IDirect3DStateBlock9 *This)
{
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_sb *sb = (struct qd3d_sb *)qd3d_obj(This)->ext;
    HRESULT hr;

    if (d && d->recording)
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DStateBlock9_Capture(This);
    if (SUCCEEDED(hr) && d && sb)
        binds_copy(sb, &sb->b, &d->b);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DStateBlock9_Apply(IDirect3DStateBlock9 *This)
{
    struct qd3d_dev *d = dev_ext(This);
    struct qd3d_sb *sb = (struct qd3d_sb *)qd3d_obj(This)->ext;
    HRESULT hr;

    if (d && d->recording)
        return D3DERR_INVALIDCALL;
    hr = qd3d9g_IDirect3DStateBlock9_Apply(This);
    if (SUCCEEDED(hr) && d) {
        if (sb)
            binds_copy(sb, &d->b, &sb->b);
        d->has_decl = 1; /* the block may set a declaration or FVF */
        d->fvf = 0;
        InterlockedExchange(&d->flush, 1);
    }
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Present                                                                  */
/* ------------------------------------------------------------------------ */

#define QD3D_MAX_FRAMES_AHEAD 2u

/*
 * The cursor over published frames. The VM shows Direct3D frames instead of
 * the guest desktop, where GDI draws the cursor, so the host draws the
 * cursor the guest shows (GetCursorInfo: the Windows cursor, which the
 * device cursor is, see SetCursorProperties) over the frames it publishes,
 * never into the back buffer or front buffer, like a hardware cursor. The
 * image travels once per cursor handle (CURSOR); every PRESENT says where
 * the cursor is and whether the presentation window is minimized or hidden
 * (then the desktop shows).
 *
 * Between presents a poller thread reports what changed: the cursor
 * (CURSOR_POS) and a frame source's window minimized, hidden or shown
 * again (SOURCE_SHOWN), so the cursor follows the mouse and the desktop
 * comes and goes as natively while an application presents only when its
 * picture changes; and it beats ctrl->heartbeat, which keeps such a static
 * screen up (a slot that falls silent lost its process). It runs while a
 * device that presented lives (no thread before the first present), wakes
 * every QD3D_CURSOR_POLL_MS (no finer than the system timer: 15.6 ms on
 * Windows XP unless the application raised it with timeBeginPeriod, as
 * games commonly do), never waits for the connection lock (a busy lock
 * skips a report to the next tick) and takes it only to report. Its thread
 * holds a reference on this DLL (FreeLibraryAndExitThread) and ends as the
 * last such device is released, so FreeLibrary unloads the DLL at once, as
 * natively.
 */
#define QD3D_CURSOR_POLL_MS 8u
#define QD3D_SOURCES_MAX    16u

static struct {
    HCURSOR volatile handle; /* the image the host has came from it; NULL: none */
    volatile int ok;         /* it converted (else the host has none) */
    /* What the host was told last, in ring order (written, lock held, with
     * the command that tells it): the cursor shows, and where. */
    volatile LONG on;
    volatile LONG pos[2];
} g_cursor;

/* The poller, under a lock of its own that nothing holds while it waits:
 * devices that presented and live, the thread started for them (NULL once
 * the last of them went; a thread ends at its first wake without one), and
 * the auto-reset event that wakes it at once then. */
static CRITICAL_SECTION g_poll_cs;
static struct {
    volatile LONG devices;
    HANDLE thread;
    HANDLE wake;
} g_poll;

/* Frame sources that presented, a device (for its implicit swap chain) or
 * an additional swap chain by wire id (0: free entry): the window of its
 * last present, whether that window showed since it became the source's (a
 * helper window that never shows never hides frames), and whether the host
 * was told it hides them. Lock held for writes. */
static struct {
    volatile uint32_t id;
    HWND volatile window;
    volatile LONG seen;
    volatile LONG hidden;
} g_src[QD3D_SOURCES_MAX];

static void cursor_cache_forget(HCURSOR h)
{
    qd3d_rt_lock();
    if (g_cursor.handle == h)
        g_cursor.handle = NULL;
    qd3d_rt_unlock();
}

/* A Windows cursor in the wire format (qd3d_proto.h, CURSOR): monochrome
 * AND/XOR pairs and color cursors with a mask become opaque, transparent
 * or inverting pixels, 32-bit cursors with alpha keep it. *px receives
 * malloc'd pixels. Returns 0 when the cursor cannot be read or is larger
 * than QD3D_CURSOR_MAX. */
static int cursor_convert(HCURSOR c, struct qd3d_cursor *hdr, DWORD **px)
{
    struct {
        BITMAPINFOHEADER h;
        RGBQUAD pal[2];
    } mi;
    BITMAPINFO bi;
    ICONINFO ii;
    BITMAP bm, cb;
    BYTE *mask = NULL;
    DWORD *color = NULL, *out = NULL;
    UINT w, mh, h, row, x, y;
    int ok = 0, alpha = 0, inv;
    HDC dc = NULL;

    *px = NULL;
    if (!GetIconInfo(c, &ii))
        return 0;
    if (!GetObjectA(ii.hbmMask, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        goto done;
    w = (UINT)bm.bmWidth;
    mh = (UINT)bm.bmHeight;
    h = ii.hbmColor ? mh : mh / 2u;
    if (!h || w > QD3D_CURSOR_MAX || h > QD3D_CURSOR_MAX || ii.xHotspot >= w || ii.yHotspot >= h)
        goto done;
    row = (w + 31u) / 32u * 4u;
    mask = (BYTE *)malloc((size_t)row * mh);
    out = (DWORD *)malloc((size_t)w * h * 4u);
    color = ii.hbmColor ? (DWORD *)malloc((size_t)w * h * 4u) : NULL;
    dc = CreateCompatibleDC(NULL);
    if (!mask || !out || (ii.hbmColor && !color) || !dc)
        goto done;
    memset(&mi, 0, sizeof(mi));
    mi.h.biSize = sizeof(mi.h);
    mi.h.biWidth = (LONG)w;
    mi.h.biHeight = -(LONG)mh;
    mi.h.biPlanes = 1;
    mi.h.biBitCount = 1;
    mi.h.biCompression = BI_RGB;
    if (GetDIBits(dc, ii.hbmMask, 0, mh, mask, (BITMAPINFO *)&mi, DIB_RGB_COLORS) != (int)mh)
        goto done;
    /* Bit value 1 is the palette's second color: white normally. */
    inv = mi.pal[0].rgbRed && mi.pal[0].rgbGreen && mi.pal[0].rgbBlue;
    if (color) {
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(dc, ii.hbmColor, 0, h, color, &bi, DIB_RGB_COLORS) != (int)h)
            goto done;
        if (GetObjectA(ii.hbmColor, sizeof(cb), &cb) && cb.bmBitsPixel == 32)
            for (y = 0; y < w * h && !alpha; y++)
                alpha = (color[y] >> 24) != 0;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            DWORD *o = &out[(size_t)y * w + x];
            int and_bit = ((mask[(size_t)y * row + x / 8u] >> (7u - x % 8u)) & 1) ^ inv;

            if (alpha) {
                DWORD p = color[(size_t)y * w + x];

                *o = (p >> 24) ? p : 0;
            } else if (color) {
                DWORD p = color[(size_t)y * w + x] & 0x00ffffffu;

                *o = and_bit ? p : (p | 0xff000000u);
            } else {
                int xor_bit = ((mask[(size_t)(y + h) * row + x / 8u] >> (7u - x % 8u)) & 1) ^ inv;

                *o = and_bit ? (xor_bit ? 0x00ffffffu : 0u) : (xor_bit ? 0xffffffffu : 0xff000000u);
            }
        }
    }
    hdr->width = w;
    hdr->height = h;
    hdr->hot_x = ii.xHotspot;
    hdr->hot_y = ii.yHotspot;
    *px = out;
    out = NULL;
    ok = 1;
done:
    if (dc)
        DeleteDC(dc);
    free(mask);
    free(color);
    free(out);
    DeleteObject(ii.hbmMask);
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    return ok;
}

/* Sends the image of cursor c (CURSOR) unless the host's came from it. Lock
 * held. */
static void cursor_image(HCURSOR c)
{
    struct qd3d_cursor hdr;
    struct qd3d_call k;
    DWORD *px = NULL;
    int ok;

    if (c == g_cursor.handle)
        return;
    memset(&hdr, 0, sizeof(hdr));
    ok = cursor_convert(c, &hdr, &px);
    if (!ok)
        memset(&hdr, 0, sizeof(hdr));
    qd3d_call_begin(&k, QD3D_OP_CURSOR, 0, 0, sizeof(hdr) + (uint64_t)hdr.width * hdr.height * 4u);
    qd3d_put_raw(&k, &hdr, sizeof(hdr));
    if (px)
        qd3d_put_raw(&k, px, (uint64_t)hdr.width * hdr.height * 4u);
    (void)qd3d_call_end(&k);
    qd3d_call_done(&k);
    free(px);
    g_cursor.handle = c;
    g_cursor.ok = ok;
}

/* The cursor fields of a PRESENT: the cursor the guest shows and where, its
 * image sent first when it changed. The host places it over the frame's
 * screen rectangle (nothing of it over an empty one). */
static void cursor_sample(struct qd3d_present *p)
{
    CURSORINFO ci;

    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
        return;
    qd3d_rt_lock();
    cursor_image(ci.hCursor);
    if (g_cursor.ok) {
        p->cursor = 1;
        p->cursor_pos[0] = ci.ptScreenPos.x;
        p->cursor_pos[1] = ci.ptScreenPos.y;
    }
    qd3d_rt_unlock();
}

/* The command being written tells the host this cursor. Lock held. */
static void cursor_told(uint32_t on, const int32_t *pos)
{
    g_cursor.on = (LONG)on;
    g_cursor.pos[0] = on ? pos[0] : 0;
    g_cursor.pos[1] = on ? pos[1] : 0;
}

/* Window w hides the frames presented to it: minimized, or hidden after it
 * showed (*seen, set once it shows). */
static LONG window_hidden(HWND w, LONG *seen)
{
    if (!w)
        return 0;
    if (IsIconic(w))
        return 1;
    if (IsWindowVisible(w)) {
        *seen = 1;
        return 0;
    }
    return *seen;
}

/* PRESENT hidden of source id presenting to w, which the host is told.
 * Lock held. */
static uint32_t source_present(uint32_t id, HWND w)
{
    unsigned i, slot = QD3D_SOURCES_MAX;
    LONG seen = 0, hidden;

    for (i = 0; i < QD3D_SOURCES_MAX && g_src[i].id != id; i++)
        if (!g_src[i].id && slot == QD3D_SOURCES_MAX)
            slot = i;
    if (i < QD3D_SOURCES_MAX) {
        slot = i;
    } else if (slot < QD3D_SOURCES_MAX) {
        g_src[slot].window = NULL;
        g_src[slot].id = id;
    } else {
        return (uint32_t)window_hidden(w, &seen); /* untracked */
    }
    if (g_src[slot].window != w) {
        g_src[slot].window = w;
        g_src[slot].seen = 0;
    }
    seen = g_src[slot].seen;
    hidden = window_hidden(w, &seen);
    g_src[slot].seen = seen;
    g_src[slot].hidden = hidden;
    return (uint32_t)hidden;
}

/* A device or additional swap chain goes away (before its RELEASE, under
 * the same lock: the poller never names a released id). */
static void source_forget(uint32_t id)
{
    unsigned i;

    qd3d_rt_lock();
    for (i = 0; i < QD3D_SOURCES_MAX; i++)
        if (g_src[i].id == id)
            g_src[i].id = 0;
    qd3d_rt_unlock();
}

/* One poll: the heartbeat, then what the guest shows now against what the
 * host was told last; changes go out as CURSOR (a new image), CURSOR_POS
 * and SOURCE_SHOWN. */
static void cursor_poll_once(void)
{
    struct qd3d_cursor_pos cp;
    struct qd3d_source_shown ss;
    struct qd3d_call k;
    CURSORINFO ci;
    int sampled, shows, change = 0, image = 0;
    unsigned i;
    LONG seen, hidden;
    uint32_t on;

    qd3d_rt_heartbeat();
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    sampled = GetCursorInfo(&ci) != 0;
    shows = sampled && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
    /* Changes as far as reads without the lock tell; mostly none. */
    if (sampled && ((shows && ci.hCursor != g_cursor.handle) || (LONG)(shows && g_cursor.ok) != g_cursor.on ||
                    (g_cursor.on && (ci.ptScreenPos.x != g_cursor.pos[0] || ci.ptScreenPos.y != g_cursor.pos[1]))))
        change = 1;
    for (i = 0; i < QD3D_SOURCES_MAX && !change; i++) {
        seen = g_src[i].seen;
        if (g_src[i].id && (window_hidden(g_src[i].window, &seen) != g_src[i].hidden || seen != g_src[i].seen))
            change = 1;
    }
    if (!change)
        return;
    if (!qd3d_rt_trylock())
        return; /* the next tick reports it */
    if (sampled) {
        if (shows && ci.hCursor != g_cursor.handle) {
            cursor_image(ci.hCursor);
            image = 1;
        }
        on = shows && g_cursor.ok;
        if ((LONG)on != g_cursor.on ||
            (on && (image || ci.ptScreenPos.x != g_cursor.pos[0] || ci.ptScreenPos.y != g_cursor.pos[1]))) {
            cp.visible = on;
            cp.pos[0] = on ? ci.ptScreenPos.x : 0;
            cp.pos[1] = on ? ci.ptScreenPos.y : 0;
            qd3d_call_begin(&k, QD3D_OP_CURSOR_POS, 0, 0, sizeof(cp));
            qd3d_put_raw(&k, &cp, sizeof(cp));
            cursor_told(cp.visible, cp.pos);
            (void)qd3d_call_end(&k);
            qd3d_call_done(&k);
        }
    }
    for (i = 0; i < QD3D_SOURCES_MAX; i++) {
        if (!g_src[i].id)
            continue;
        seen = g_src[i].seen;
        hidden = window_hidden(g_src[i].window, &seen);
        g_src[i].seen = seen;
        if (hidden == g_src[i].hidden)
            continue;
        g_src[i].hidden = hidden;
        ss.shown = !hidden;
        qd3d_call_begin(&k, QD3D_OP_SOURCE_SHOWN, g_src[i].id, 0, sizeof(ss));
        qd3d_put_raw(&k, &ss, sizeof(ss));
        (void)qd3d_call_end(&k);
        qd3d_call_done(&k);
    }
    qd3d_rt_unlock();
}

static DWORD WINAPI cursor_poll(LPVOID module)
{
    for (;;) {
        WaitForSingleObject(g_poll.wake, QD3D_CURSOR_POLL_MS);
        if (!g_poll.devices || !qd3d_rt_alive())
            break;
        cursor_poll_once();
    }
    FreeLibraryAndExitThread((HMODULE)module, 0);
    return 0;
}

/* A device presented for the first time: a poller runs while such a device
 * lives. Above normal priority: it runs at once when due, however busy the
 * application keeps the processor, and does next to nothing. */
static void cursor_poll_device(void)
{
    HMODULE m = NULL;
    HANDLE t;

    EnterCriticalSection(&g_poll_cs);
    g_poll.devices++;
    if (!g_poll.wake)
        g_poll.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_poll.thread && g_poll.wake &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&g_poll, &m)) {
        t = CreateThread(NULL, 0, cursor_poll, m, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
            g_poll.thread = t;
        } else {
            FreeLibrary(m);
        }
    }
    LeaveCriticalSection(&g_poll_cs);
}

/* A device that presented goes. With the last one the poller ends: waited
 * for, normally a few microseconds (it never waits for the connection lock,
 * which the caller may hold), so the application's FreeLibrary after its
 * last Release unloads this DLL at once; bounded, because a thread ending
 * needs the loader lock, which a Release inside a DllMain holds (the
 * thread then ends later; its reference keeps the DLL until it has). */
static void cursor_poll_forget(void)
{
    HANDLE t = NULL;

    EnterCriticalSection(&g_poll_cs);
    if (g_poll.devices && --g_poll.devices == 0) {
        t = g_poll.thread;
        g_poll.thread = NULL;
    }
    LeaveCriticalSection(&g_poll_cs);
    if (t) {
        SetEvent(g_poll.wake);
        WaitForSingleObject(t, 100);
        CloseHandle(t);
    }
}

/* Where the presented image lands on the guest screen: windowed, the
 * destination rectangle in the presentation window's client area (all of
 * it by default); fullscreen, the display mode, which is the back buffer
 * size. Empty when the window shows nothing. */
static void present_screen(struct qd3d_present *p, HWND w, const D3DPRESENT_PARAMETERS *pp, const RECT *dst)
{
    RECT r;

    if (!pp->Windowed) {
        r.left = r.top = 0;
        r.right = (LONG)pp->BackBufferWidth;
        r.bottom = (LONG)pp->BackBufferHeight;
    } else {
        if (!w || !IsWindowVisible(w) || IsIconic(w) || !GetClientRect(w, &r))
            return;
        if (dst)
            r = *dst;
        MapWindowPoints(w, NULL, (POINT *)&r, 2);
    }
    p->screen[0] = r.left;
    p->screen[1] = r.top;
    p->screen[2] = r.right;
    p->screen[3] = r.bottom;
}

static HRESULT present(struct qd3d_obj *target, const RECT *src, const RECT *dst, HWND over, DWORD flags)
{
    struct qd3d_dev *d = dev_ext(target);
    struct qd3d_obj *dev = qd3d_obj_device(target);
    const struct qd3d_sc *sc = target->type == QD3D_T_IDirect3DSwapChain9 ? (const struct qd3d_sc *)target->ext
                                                                             : NULL;
    const D3DPRESENT_PARAMETERS *pp = sc ? &sc->pp : d ? &d->pp : NULL;
    struct qd3d_present p;
    struct qd3d_call k;
    HWND w = NULL;
    int first = 0;

    if (!qd3d_rt_alive())
        return D3DERR_DRIVERINTERNALERROR;
    if ((flags & D3DPRESENT_DONOTWAIT) && qd3d_rt_frames_ahead() > QD3D_MAX_FRAMES_AHEAD)
        return D3DERR_WASSTILLDRAWING;
    memset(&p, 0, sizeof(p));
    p.flags = flags;
    if (src) {
        p.has_src = 1;
        p.src[0] = src->left;
        p.src[1] = src->top;
        p.src[2] = src->right;
        p.src[3] = src->bottom;
    }
    if (dst) {
        p.has_dst = 1;
        p.dst[0] = dst->left;
        p.dst[1] = dst->top;
        p.dst[2] = dst->right;
        p.dst[3] = dst->bottom;
    }
    p.dest_window = (uint32_t)(uintptr_t)over;
    if (pp && d) {
        w = over ? over : pp->hDeviceWindow ? pp->hDeviceWindow : d->cp.hFocusWindow;
        present_screen(&p, w, pp, dst);
    }
    cursor_sample(&p);
    qd3d_call_begin(&k, QD3D_OP_PRESENT, target->id, 0, sizeof(p));
    /* Lock held: the source's window and the cursor as the cursor poller
     * compares them, in ring order; the poller starts with the first
     * present of a device. */
    if (dev)
        p.hidden = source_present(sc ? target->id : dev->id, w);
    qd3d_put_raw(&k, &p, sizeof(p));
    if (!k.dead) {
        cursor_told(p.cursor, p.cursor_pos);
        if (d && !d->presented) {
            d->presented = 1;
            first = 1;
        }
    }
    qd3d_rt_frame_mark(&k);
    (void)qd3d_call_end(&k);
    qd3d_call_done(&k);
    if (first)
        cursor_poll_device();
    qd3d_rt_frame_pace(QD3D_MAX_FRAMES_AHEAD);
    return qd3d_rt_alive() ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_Present(IDirect3DDevice9 *This, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
    return present(qd3d_obj(This), pSourceRect, pDestRect, hDestWindowOverride, 0);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_Present(IDirect3DSwapChain9 *This, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
{
    return present(qd3d_obj(This), pSourceRect, pDestRect, hDestWindowOverride, dwFlags);
}

/* ------------------------------------------------------------------------ */
/* Device (guest-local)                                                     */
/* ------------------------------------------------------------------------ */

static HRESULT get_device(const void *iface, IDirect3DDevice9 **pp)
{
    struct qd3d_obj *d = qd3d_obj_device(iface);

    if (!pp)
        return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!d)
        return D3DERR_INVALIDCALL;
    qd3d_obj_addref(d);
    *pp = (IDirect3DDevice9 *)d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_TestCooperativeLevel(IDirect3DDevice9 *This)
{
    return qd3d_rt_alive() ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetDirect3D(IDirect3DDevice9 *This, IDirect3D9 **ppD3D9)
{
    struct qd3d_obj *d3d = qd3d_obj(This)->parent;

    if (!ppD3D9)
        return D3DERR_INVALIDCALL;
    *ppD3D9 = NULL;
    if (!d3d)
        return D3DERR_INVALIDCALL;
    qd3d_obj_addref(d3d);
    *ppD3D9 = (IDirect3D9 *)d3d;
    return D3D_OK;
}

static HRESULT device_display_mode(struct qd3d_dev *d, D3DDISPLAYMODE *m)
{
    HRESULT hr;

    if (!m || !d)
        return D3DERR_INVALIDCALL;
    hr = qd3d_current_mode(m);
    if (FAILED(hr))
        return hr;
    if (!d->pp.Windowed) {
        /* The mode the application asked for (in loopback the real display
         * mode is the host desktop's and stays unchanged). */
        m->Width = d->pp.BackBufferWidth;
        m->Height = d->pp.BackBufferHeight;
        m->Format = display_format(d->pp.BackBufferFormat);
        if (d->pp.FullScreen_RefreshRateInHz)
            m->RefreshRate = d->pp.FullScreen_RefreshRateInHz;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetDisplayMode(IDirect3DDevice9 *This, UINT iSwapChain, D3DDISPLAYMODE *pMode)
{
    if (iSwapChain != 0)
        return D3DERR_INVALIDCALL;
    return device_display_mode(dev_ext(This), pMode);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetCreationParameters(IDirect3DDevice9 *This, D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
    struct qd3d_dev *d = dev_ext(This);

    if (!pParameters || !d)
        return D3DERR_INVALIDCALL;
    *pParameters = d->cp;
    return D3D_OK;
}

/*
 * The device cursor is a Windows cursor, as with the native runtime
 * (measured on the stock 32-bit runtime, Windows 11, windowed): ShowCursor
 * shows nothing until SetCursorProperties gave a bitmap; ShowCursor(TRUE)
 * then makes the bitmap the Windows cursor at once (SetCursor; GetCursorInfo
 * answers its handle, hot spot and pixels) and ShowCursor(FALSE) leaves no
 * cursor (SetCursor(NULL)); SetCursorPosition moves the Windows cursor
 * (SetCursorPos) unless it is there already, and nothing before a bitmap
 * was given; pixels with alpha 255 are opaque, every other pixel is
 * transparent; sizes are powers of two no larger than the display mode,
 * hot spots inside. So the cursor follows the guest's mouse like the
 * Windows cursor it is, and the host composes whatever cursor the guest
 * shows (cursor_sample, cursor_poll_once). The bitmap keeps its size:
 * Windows 11's runtime scales it to the system cursor size, Wine does not,
 * and 32x32 (the common size) is the system size anyway.
 */
static int is_pow2(UINT v)
{
    return v && !(v & (v - 1u));
}

/* The bitmap's A8R8G8B8 rows (w x h, tightly packed) into px. */
static HRESULT cursor_bitmap_read(IDirect3DDevice9 *dev, IDirect3DSurface9 *s, UINT w, UINT h, DWORD *px)
{
    IDirect3DSurface9 *tmp = NULL, *src = s;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    UINT y;

    hr = s->lpVtbl->LockRect(s, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        /* A render target without a lockable copy: read it back. */
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &tmp, NULL);
        if (SUCCEEDED(hr))
            hr = dev->lpVtbl->GetRenderTargetData(dev, s, tmp);
        if (SUCCEEDED(hr))
            hr = tmp->lpVtbl->LockRect(tmp, &lr, NULL, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            if (tmp)
                tmp->lpVtbl->Release(tmp);
            return D3DERR_INVALIDCALL;
        }
        src = tmp;
    }
    for (y = 0; y < h; y++)
        memcpy(px + (size_t)y * w, (const BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch, (size_t)w * 4u);
    src->lpVtbl->UnlockRect(src);
    if (tmp)
        tmp->lpVtbl->Release(tmp);
    return D3D_OK;
}

/* A Windows cursor from w x h A8R8G8B8 pixels: alpha 255 opaque, the rest
 * transparent (mask bit set, color 0), as the native runtime builds it. */
static HCURSOR cursor_create(const DWORD *px, UINT w, UINT h, UINT hx, UINT hy)
{
    UINT mask_row = (w + 15u) / 16u * 2u, x, y;
    BITMAPINFO bi;
    ICONINFO ii;
    HCURSOR c = NULL;
    void *bits = NULL;
    BYTE *mask = (BYTE *)calloc(1, (size_t)mask_row * h);

    if (!mask)
        return NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = FALSE;
    ii.xHotspot = hx;
    ii.yHotspot = hy;
    ii.hbmColor = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (ii.hbmColor && bits) {
        DWORD *out = (DWORD *)bits;

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                DWORD p = px[(size_t)y * w + x];

                if ((p >> 24) == 0xffu) {
                    out[(size_t)y * w + x] = p;
                } else {
                    out[(size_t)y * w + x] = 0;
                    mask[(size_t)y * mask_row + x / 8u] |= (BYTE)(0x80u >> (x % 8u));
                }
            }
        }
        GdiFlush();
        ii.hbmMask = CreateBitmap((int)w, (int)h, 1, 1, mask);
        if (ii.hbmMask)
            c = (HCURSOR)CreateIconIndirect(&ii);
    }
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    free(mask);
    return c;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetCursorProperties(IDirect3DDevice9 *This, UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap)
{
    struct qd3d_dev *d = dev_ext(This);
    D3DSURFACE_DESC desc;
    D3DDISPLAYMODE mode;
    HCURSOR c, old;
    DWORD *px;
    HRESULT hr;

    if (!d || !pCursorBitmap)
        return D3DERR_INVALIDCALL;
    if (FAILED(pCursorBitmap->lpVtbl->GetDesc(pCursorBitmap, &desc)) || FAILED(qd3d_current_mode(&mode)) ||
        desc.Format != D3DFMT_A8R8G8B8 || !is_pow2(desc.Width) || !is_pow2(desc.Height) ||
        desc.Width > mode.Width || desc.Height > mode.Height || XHotSpot >= desc.Width || YHotSpot >= desc.Height)
        return D3DERR_INVALIDCALL;
    px = (DWORD *)malloc((size_t)desc.Width * desc.Height * 4u);
    if (!px)
        return E_OUTOFMEMORY;
    hr = cursor_bitmap_read(This, pCursorBitmap, desc.Width, desc.Height, px);
    c = SUCCEEDED(hr) ? cursor_create(px, desc.Width, desc.Height, XHotSpot, YHotSpot) : NULL;
    free(px);
    if (FAILED(hr))
        return hr;
    if (!c)
        return E_OUTOFMEMORY;
    qd3d_rt_lock();
    old = d->cursor;
    d->cursor = c;
    if (d->cursor_visible)
        SetCursor(c);
    qd3d_rt_unlock();
    if (old) {
        cursor_cache_forget(old);
        DestroyCursor(old);
    }
    return D3D_OK;
}

void STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_SetCursorPosition(IDirect3DDevice9 *This, int X, int Y, DWORD Flags)
{
    struct qd3d_dev *d = dev_ext(This);
    POINT p;

    if (!d || !d->cursor)
        return;
    if (!GetCursorPos(&p) || p.x != X || p.y != Y)
        SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_ShowCursor(IDirect3DDevice9 *This, BOOL bShow)
{
    struct qd3d_dev *d = dev_ext(This);
    BOOL prev;

    if (!d)
        return FALSE;
    qd3d_rt_lock();
    prev = d->cursor_visible;
    if (d->cursor) {
        d->cursor_visible = bShow ? TRUE : FALSE;
        SetCursor(bShow ? d->cursor : NULL);
    }
    qd3d_rt_unlock();
    return prev;
}

/* Its back buffers go with it unless the application still holds them. */
static void sc_ext_free(struct qd3d_obj *o)
{
    struct qd3d_sc *sc = (struct qd3d_sc *)o->ext;
    unsigned i;

    source_forget(o->id);
    o->ext = NULL;
    if (!sc)
        return;
    for (i = 0; i < QD3D_SC_BUFFERS; i++) {
        if (sc->bb[i]) {
            qd3d_res_set_sc_container(sc->bb[i], NULL);
            qd3d_obj_unhold(sc->bb[i]);
        }
    }
    free(sc);
}

/* Native rules (Wine's test_swapchain on the stock runtime): no fullscreen
 * swap chain and none on a fullscreen device; a missing window is the
 * focus window (GetPresentParameters reports it, the caller's structure
 * keeps NULL), a zero size its client area and a zero count one, written
 * back as for CreateDevice. The swap chain references the device. */
static HRESULT create_swap_chain(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pPresentationParameters,
                                 IDirect3DSwapChain9 **pSwapChain)
{
    struct qd3d_obj *dev = qd3d_obj(This);
    struct qd3d_dev *d = (struct qd3d_dev *)dev->ext;
    struct qd3d_create_swapchain cs;
    struct qd3d_wire_pp acc;
    D3DPRESENT_PARAMETERS pp;
    struct qd3d_sc *sc;
    struct qd3d_obj *o;
    struct qd3d_call k;
    HRESULT hr;
    int got;

    if (!pSwapChain)
        return D3DERR_INVALIDCALL;
    *pSwapChain = NULL;
    if (!pPresentationParameters || !d || !pPresentationParameters->Windowed || !d->pp.Windowed)
        return D3DERR_INVALIDCALL;
    pp = *pPresentationParameters;
    if (!pp.hDeviceWindow)
        pp.hDeviceWindow = d->cp.hFocusWindow;
    hr = resolve_pp(d->cp.hFocusWindow, &pp);
    if (FAILED(hr))
        return hr;
    sc = (struct qd3d_sc *)calloc(1, sizeof(*sc));
    if (!sc)
        return E_OUTOFMEMORY;
    o = qd3d_obj_new(dev, QD3D_T_IDirect3DSwapChain9);
    if (!o) {
        free(sc);
        return E_OUTOFMEMORY;
    }
    memset(&cs, 0, sizeof(cs));
    cs.swapchain_id = o->id;
    pp_to_wire(&pp, &cs.pp);
    qd3d_call_begin(&k, QD3D_OP_CREATE_SWAPCHAIN, dev->id, 1, sizeof(cs));
    qd3d_put_raw(&k, &cs, sizeof(cs));
    hr = (HRESULT)qd3d_call_end(&k);
    got = SUCCEEDED(hr) && k.rleft == sizeof(acc) && qd3d_get_raw(&k, &acc, sizeof(acc));
    qd3d_call_done(&k);
    if (SUCCEEDED(hr) && !got) {
        qd3d_rt_log("CREATE_SWAPCHAIN reply malformed");
        hr = D3DERR_DRIVERINTERNALERROR;
    }
    if (FAILED(hr)) {
        qd3d_obj_discard(o);
        free(sc);
        return hr;
    }
    pp_from_wire(&pp, &acc);
    sc->pp = pp;
    o->ext = sc;
    o->ext_free = sc_ext_free;
    pp_from_wire(pPresentationParameters, &acc);
    *pSwapChain = (IDirect3DSwapChain9 *)o;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateAdditionalSwapChain(IDirect3DDevice9 *This, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **pSwapChain)
{
    return create_swap_chain(This, pPresentationParameters, pSwapChain);
}

/* Raster position of a 60 Hz display derived from QueryPerformanceCounter;
 * the last twentieth of each refresh is vertical blank. */
static HRESULT raster_status(struct qd3d_dev *d, D3DRASTER_STATUS *rs)
{
    LARGE_INTEGER f, c;
    D3DDISPLAYMODE m;
    uint64_t period, phase, total, line;

    if (!rs || !d || FAILED(device_display_mode(d, &m)) || !m.Height)
        return D3DERR_INVALIDCALL;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c) || f.QuadPart < 60)
        return D3DERR_INVALIDCALL;
    period = (uint64_t)f.QuadPart / 60u;
    phase = (uint64_t)c.QuadPart % period;
    total = (uint64_t)m.Height + (m.Height / 20u ? m.Height / 20u : 1u);
    line = phase * total / period;
    if (line >= m.Height) {
        rs->InVBlank = TRUE;
        rs->ScanLine = 0;
    } else {
        rs->InVBlank = FALSE;
        rs->ScanLine = (UINT)line;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetRasterStatus(IDirect3DDevice9 *This, UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus)
{
    if (iSwapChain != 0)
        return D3DERR_INVALIDCALL;
    return raster_status(dev_ext(This), pRasterStatus);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawRectPatch(IDirect3DDevice9 *This, UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo)
{
    return D3DERR_INVALIDCALL; /* patches are not remoted */
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DrawTriPatch(IDirect3DDevice9 *This, UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo)
{
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_DeletePatch(IDirect3DDevice9 *This, UINT Handle)
{
    return D3DERR_INVALIDCALL;
}

/* ------------------------------------------------------------------------ */
/* Swap chain (guest-local)                                                 */
/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetRasterStatus(IDirect3DSwapChain9 *This, D3DRASTER_STATUS *pRasterStatus)
{
    return raster_status(dev_ext(This), pRasterStatus);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetDisplayMode(IDirect3DSwapChain9 *This, D3DDISPLAYMODE *pMode)
{
    return device_display_mode(dev_ext(This), pMode);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetDevice(IDirect3DSwapChain9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetPresentParameters(IDirect3DSwapChain9 *This, D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    const struct qd3d_sc *sc = (const struct qd3d_sc *)qd3d_obj(This)->ext;
    struct qd3d_dev *d = dev_ext(This);

    if (!pPresentationParameters || !d)
        return D3DERR_INVALIDCALL;
    *pPresentationParameters = sc ? sc->pp : d->pp;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* State blocks, shaders, declarations, queries                             */
/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DStateBlock9_GetDevice(IDirect3DStateBlock9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

struct qd3d_blob {
    uint32_t size;
    uint8_t data[];
};

/* Attaches a copy of data to a freshly created proxy, or releases the proxy
 * if that is impossible. */
static HRESULT attach_blob(void **pp, const void *data, uint32_t size)
{
    struct qd3d_obj *o = qd3d_obj(*pp);
    struct qd3d_blob *b = (struct qd3d_blob *)malloc(sizeof(*b) + size);

    if (!b) {
        qd3d_obj_release(o);
        *pp = NULL;
        return E_OUTOFMEMORY;
    }
    b->size = size;
    memcpy(b->data, data, size);
    o->ext = b;
    o->ext_free = ext_free_plain;
    return D3D_OK;
}

/* Natively GetDeclaration never checks the caller's element count; shader
 * functions need room (sized). */
static HRESULT get_blob(const void *iface, void *pData, UINT *pSize, uint32_t unit, int sized)
{
    const struct qd3d_blob *b = (const struct qd3d_blob *)qd3d_obj(iface)->ext;

    if (!pSize)
        return D3DERR_INVALIDCALL;
    if (!b) {
        /* Proxy for an object the host named first (its contents never
         * crossed the guest); nothing truthful to return. */
        return D3DERR_INVALIDCALL;
    }
    if (!pData) {
        *pSize = b->size / unit;
        return D3D_OK;
    }
    if (sized && *pSize < b->size / unit)
        return D3DERR_INVALIDCALL;
    memcpy(pData, b->data, b->size);
    *pSize = b->size / unit;
    return D3D_OK;
}

static void decl_add(D3DVERTEXELEMENT9 *e, unsigned *n, WORD *off, BYTE type, BYTE usage, BYTE index)
{
    static const BYTE sizes[] = {4, 8, 12, 16, 4, 4}; /* FLOAT1..FLOAT4, D3DCOLOR, UBYTE4 */

    e[*n].Stream = 0;
    e[*n].Offset = *off;
    e[*n].Type = type;
    e[*n].Method = D3DDECLMETHOD_DEFAULT;
    e[*n].Usage = usage;
    e[*n].UsageIndex = index;
    *off = (WORD)(*off + sizes[type]);
    (*n)++;
}

/* The declaration the native runtime builds for an FVF (its layout is what
 * GetDeclaration returns after SetFVF; Wine's test_fvf_decl_conversion
 * passes on the stock runtime with these rules). */
static unsigned fvf_decl(DWORD fvf, D3DVERTEXELEMENT9 *e)
{
    static const BYTE tc[] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
    static const D3DVERTEXELEMENT9 end = D3DDECL_END();
    DWORD pos = fvf & D3DFVF_POSITION_MASK;
    int blend = (fvf & D3DFVF_XYZB5) > D3DFVF_XYZRHW;
    int blend_idx = blend && ((fvf & D3DFVF_XYZB5) == D3DFVF_XYZB5 ||
                              (fvf & (D3DFVF_LASTBETA_D3DCOLOR | D3DFVF_LASTBETA_UBYTE4)));
    int blends = blend ? 1 + (int)(((fvf & D3DFVF_XYZB5) - D3DFVF_XYZB1) >> 1) - blend_idx : 0;
    unsigned i, n = 0, textures = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    WORD off = 0;

    if (pos) {
        if (!blend && (fvf & D3DFVF_XYZRHW))
            decl_add(e, &n, &off, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
        else if (!blend && (fvf & D3DFVF_XYZW) == D3DFVF_XYZW)
            decl_add(e, &n, &off, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
        else
            decl_add(e, &n, &off, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
    }
    if (blends > 0) {
        if ((fvf & D3DFVF_XYZB5) == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR))
            decl_add(e, &n, &off, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDWEIGHT, 0);
        else
            decl_add(e, &n, &off, (BYTE)(D3DDECLTYPE_FLOAT1 + blends - 1), D3DDECLUSAGE_BLENDWEIGHT, 0);
    }
    if (blend_idx) {
        if ((fvf & D3DFVF_LASTBETA_UBYTE4) ||
            ((fvf & D3DFVF_XYZB5) == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR)))
            decl_add(e, &n, &off, D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0);
        else if (fvf & D3DFVF_LASTBETA_D3DCOLOR)
            decl_add(e, &n, &off, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0);
        else
            decl_add(e, &n, &off, D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_BLENDINDICES, 0);
    }
    if (fvf & D3DFVF_NORMAL)
        decl_add(e, &n, &off, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
    if (fvf & D3DFVF_PSIZE)
        decl_add(e, &n, &off, D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
    if (fvf & D3DFVF_DIFFUSE)
        decl_add(e, &n, &off, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
    if (fvf & D3DFVF_SPECULAR)
        decl_add(e, &n, &off, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
    for (i = 0; i < textures && i < 8; i++)
        decl_add(e, &n, &off, tc[(fvf >> (16 + i * 2)) & 3], D3DDECLUSAGE_TEXCOORD, (BYTE)i);
    e[n++] = end;
    return n;
}

/* A declaration the host made for the FVF set last gets the native
 * contents GetDeclaration returns. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetVertexDeclaration(IDirect3DDevice9 *This, IDirect3DVertexDeclaration9 **ppDecl)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetVertexDeclaration(This, ppDecl);
    struct qd3d_dev *d = dev_ext(This);
    D3DVERTEXELEMENT9 e[16];

    if (SUCCEEDED(hr) && d && d->fvf && ppDecl && *ppDecl && !qd3d_obj(*ppDecl)->ext)
        hr = attach_blob((void **)ppDecl, e, fvf_decl(d->fvf, e) * (uint32_t)sizeof(*e));
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateVertexShader(IDirect3DDevice9 *This, const DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_CreateVertexShader(This, pFunction, ppShader);

    if (SUCCEEDED(hr) && ppShader && *ppShader)
        hr = attach_blob((void **)ppShader, pFunction, qd3d_shader_size(pFunction));
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreatePixelShader(IDirect3DDevice9 *This, const DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_CreatePixelShader(This, pFunction, ppShader);

    if (SUCCEEDED(hr) && ppShader && *ppShader)
        hr = attach_blob((void **)ppShader, pFunction, qd3d_shader_size(pFunction));
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateVertexDeclaration(IDirect3DDevice9 *This, const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_CreateVertexDeclaration(This, pVertexElements, ppDecl);

    if (SUCCEEDED(hr) && ppDecl && *ppDecl)
        hr = attach_blob((void **)ppDecl, pVertexElements,
                         qd3d_decl_count(pVertexElements) * (uint32_t)sizeof(D3DVERTEXELEMENT9));
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexShader9_GetDevice(IDirect3DVertexShader9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexShader9_GetFunction(IDirect3DVertexShader9 *This, void *pData, UINT *pSizeOfData)
{
    return get_blob(This, pData, pSizeOfData, 1, 1);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DPixelShader9_GetDevice(IDirect3DPixelShader9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DPixelShader9_GetFunction(IDirect3DPixelShader9 *This, void *pData, UINT *pSizeOfData)
{
    return get_blob(This, pData, pSizeOfData, 1, 1);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexDeclaration9_GetDevice(IDirect3DVertexDeclaration9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexDeclaration9_GetDeclaration(IDirect3DVertexDeclaration9 *This, D3DVERTEXELEMENT9 *pElement, UINT *pNumElements)
{
    return get_blob(This, pElement, pNumElements, (uint32_t)sizeof(D3DVERTEXELEMENT9), 0);
}

struct qd3d_query {
    D3DQUERYTYPE type;
    DWORD size;
};

static DWORD query_data_size(D3DQUERYTYPE t)
{
    switch (t) {
    case D3DQUERYTYPE_VCACHE:            return sizeof(D3DDEVINFO_VCACHE);
    case D3DQUERYTYPE_RESOURCEMANAGER:   return sizeof(D3DDEVINFO_RESOURCEMANAGER);
    case D3DQUERYTYPE_VERTEXSTATS:       return sizeof(D3DDEVINFO_D3DVERTEXSTATS);
    case D3DQUERYTYPE_EVENT:             return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:         return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:         return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ:     return sizeof(UINT64);
    case D3DQUERYTYPE_PIPELINETIMINGS:   return sizeof(D3DDEVINFO_D3D9PIPELINETIMINGS);
    case D3DQUERYTYPE_INTERFACETIMINGS:  return sizeof(D3DDEVINFO_D3D9INTERFACETIMINGS);
    case D3DQUERYTYPE_VERTEXTIMINGS:
    case D3DQUERYTYPE_PIXELTIMINGS:      return sizeof(D3DDEVINFO_D3D9STAGETIMINGS);
    case D3DQUERYTYPE_BANDWIDTHTIMINGS:  return sizeof(D3DDEVINFO_D3D9BANDWIDTHTIMINGS);
    case D3DQUERYTYPE_CACHEUTILIZATION:  return sizeof(D3DDEVINFO_D3D9CACHEUTILIZATION);
    default:                             return 0;
    }
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateQuery(IDirect3DDevice9 *This, D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_CreateQuery(This, Type, ppQuery);

    if (SUCCEEDED(hr) && ppQuery && *ppQuery) {
        struct qd3d_obj *o = qd3d_obj(*ppQuery);
        struct qd3d_query *q = (struct qd3d_query *)malloc(sizeof(*q));
        if (!q) {
            qd3d_obj_release(o);
            *ppQuery = NULL;
            return E_OUTOFMEMORY;
        }
        q->type = Type;
        q->size = query_data_size(Type);
        o->ext = q;
        o->ext_free = ext_free_plain;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DQuery9_GetDevice(IDirect3DQuery9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

D3DQUERYTYPE STDMETHODCALLTYPE qd3d9o_IDirect3DQuery9_GetType(IDirect3DQuery9 *This)
{
    const struct qd3d_query *q = (const struct qd3d_query *)qd3d_obj(This)->ext;
    return q ? q->type : (D3DQUERYTYPE)0;
}

DWORD STDMETHODCALLTYPE qd3d9o_IDirect3DQuery9_GetDataSize(IDirect3DQuery9 *This)
{
    const struct qd3d_query *q = (const struct qd3d_query *)qd3d_obj(This)->ext;
    return q ? q->size : 0;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DQuery9_GetData(IDirect3DQuery9 *This, void *pData, DWORD dwSize, DWORD dwGetDataFlags)
{
    struct qd3d_query_data q;
    struct qd3d_call k;
    HRESULT hr;

    if (dwSize > 256u || (dwSize && !pData))
        return D3DERR_INVALIDCALL;
    q.size = dwSize;
    q.flags = dwGetDataFlags;
    qd3d_call_begin(&k, QD3D_OP_QUERY_DATA, qd3d_obj(This)->id, 1, sizeof(q));
    qd3d_put_raw(&k, &q, sizeof(q));
    hr = (HRESULT)qd3d_call_end(&k);
    if (hr == S_OK && dwSize) {
        if (k.rleft != dwSize || !qd3d_get_raw(&k, pData, dwSize)) {
            qd3d_rt_log("QUERY_DATA reply has %u bytes, expected %u", (unsigned)k.rleft, (unsigned)dwSize);
            hr = D3DERR_DRIVERINTERNALERROR;
        }
    }
    qd3d_call_done(&k);
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Direct3DCreate9 and the stock-runtime fallback                           */
/* ------------------------------------------------------------------------ */

typedef IDirect3D9 *(WINAPI *qd3d_create9_fn)(UINT);
typedef UINT (WINAPI *qd3d_wow64dir_fn)(LPSTR, UINT);

/* Directory part of path (in place); returns its length. */
static size_t dir_part(char *path)
{
    size_t n = strlen(path);

    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    if (n)
        n--;
    path[n] = 0;
    return n;
}

static int same_dir(const char *a, const char *b)
{
    return a[0] && b[0] && lstrcmpiA(a, b) == 0;
}

/* The stock runtime. Installed (in System32, or SysWOW64), it is
 * qd3d9_orig.dll beside us. A per-game copy uses System32\d3d9.dll, unless a
 * system-wide install put the QD3D proxy there and kept the stock DLL as
 * qd3d9_orig.dll. Never another QD3D proxy: it would start a second session
 * on a second channel slot. QD3D9_TEST_SYSTEM_DIR names a directory that
 * stands for System32 in tests. */
static HMODULE load_stock(void)
{
    static const char orig[] = "\\qd3d9_orig.dll", stock[] = "\\d3d9.dll";
    char sys[MAX_PATH], wow[MAX_PATH], self[MAX_PATH], path[MAX_PATH + sizeof orig];
    qd3d_wow64dir_fn wow64dir;
    size_t len;
    UINT n;
    HMODULE m;
    int in_sys;

    wow[0] = 0;
    n = GetEnvironmentVariableA("QD3D9_TEST_SYSTEM_DIR", sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (sys[n - 1] == '\\')
            sys[n - 1] = 0;
    } else {
        n = GetSystemDirectoryA(sys, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return NULL;
        wow64dir = (qd3d_wow64dir_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                   "GetSystemWow64DirectoryA");
        if (wow64dir) {
            UINT w = wow64dir(wow, MAX_PATH);
            if (!w || w >= MAX_PATH)
                wow[0] = 0;
        }
    }
    n = GetModuleFileNameA(g_self, self, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return NULL;
    dir_part(self);
    in_sys = same_dir(self, sys) || same_dir(self, wow);
    len = strlen(sys);
    memcpy(path, sys, len);
    memcpy(path + len, orig, sizeof orig);
    if (!in_sys && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        memcpy(path + len, stock, sizeof stock);
    m = LoadLibraryA(path);
    if (m && (m == g_self || FindResourceA(m, "QD3D_MARKER", RT_RCDATA))) {
        FreeLibrary(m);
        qd3d_rt_log("stock runtime %s is a QD3D copy; refused", path);
        return NULL;
    }
    if (!m)
        qd3d_rt_log("stock runtime %s not loadable", path);
    return m;
}

/* An export of the stock runtime (loaded on first use, tried once), or NULL. */
static FARPROC stock_proc(const char *name)
{
    FARPROC fn = NULL;

    qd3d_rt_lock();
    if (!g_stock_tried) {
        g_stock_tried = 1;
        g_stock = load_stock();
    }
    if (g_stock)
        fn = GetProcAddress(g_stock, name);
    qd3d_rt_unlock();
    return fn;
}

static IDirect3D9 *forward_create(UINT sdk)
{
    qd3d_create9_fn fn = (qd3d_create9_fn)(void (*)(void))stock_proc("Direct3DCreate9");

    return fn ? fn(sdk) : NULL;
}

/* 1 the first time this process falls back to the stock runtime: that one
 * reaches the persistent log. */
static int first_fallback(void)
{
    static LONG noted;

    return InterlockedExchange(&noted, 1) == 0;
}

/* Starts the process's one session (lock held): the host binds its
 * IDirect3D9 to a fresh guest id that every IDirect3D9 proxy names. */
static int session_start(UINT sdk)
{
    struct qd3d_hello h;
    struct qd3d_hello_reply r;
    struct qd3d_call k;
    uint32_t id = qd3d_rt_new_id();
    HRESULT hr;
    int ok;

    if (!id)
        return 0;
    h.abi = QD3D_ABI_VERSION;
    h.pid = GetCurrentProcessId();
    h.sdk_version = sdk;
    h.d3d_id = id;
    qd3d_call_begin(&k, QD3D_OP_HELLO, 0, 1, sizeof(h));
    qd3d_rt_hello_mark(&k);
    qd3d_put_raw(&k, &h, sizeof(h));
    hr = (HRESULT)qd3d_call_end(&k);
    ok = SUCCEEDED(hr) && k.rleft == sizeof(r) && qd3d_get_raw(&k, &r, sizeof(r)) && r.abi == QD3D_ABI_VERSION;
    qd3d_call_done(&k);
    if (!ok) {
        if (first_fallback())
            qd3d_rt_event("HELLO failed (hr %#lx); using the stock runtime", (unsigned long)hr);
        else
            qd3d_rt_log("HELLO failed (hr %#lx); using the stock runtime", (unsigned long)hr);
        return 0;
    }
    qd3d_rt_set_limits(r.max_cmd, r.max_reply);
    g_d3d_id = id;
    g_session = 1;
    return 1;
}

/* Native Direct3DCreate9 returns a new object on every call. One session
 * serves the whole process, so objects stay valid whatever the application
 * releases and creates later; GOODBYE at process detach ends it. */
IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion)
{
    struct qd3d_obj *o = NULL;
    int session;

    if (qd3d_rt_connect()) {
        qd3d_rt_lock();
        session = g_session || session_start(SDKVersion);
        if (session)
            o = qd3d_obj_new_local(QD3D_T_IDirect3D9, g_d3d_id);
        qd3d_rt_unlock();
        if (session)
            return (IDirect3D9 *)o;
    } else if (first_fallback()) {
        qd3d_rt_event("Direct3DCreate9: using the stock runtime (%s)",
                      qd3d_rt_transport() == QD3D_TRANSPORT_NONE ? "not connected" : "channel dead");
    }
    return forward_create(SDKVersion);
}

/* ------------------------------------------------------------------------ */
/* Other exports: harmless stubs                                            */
/* ------------------------------------------------------------------------ */

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    return -1;
}

int WINAPI D3DPERF_EndEvent(void)
{
    return -1;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    return FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
}

DWORD WINAPI D3DPERF_GetStatus(void)
{
    return 0;
}

/* The shader validator is a CPU-side object of the stock runtime (D3DX's
 * shader assembler uses it too). */
void *WINAPI Direct3DShaderValidatorCreate9(void)
{
    typedef void *(WINAPI *qd3d_validator_fn)(void);
    qd3d_validator_fn fn = (qd3d_validator_fn)(void (*)(void))stock_proc("Direct3DShaderValidatorCreate9");

    return fn ? fn() : NULL;
}

void WINAPI DebugSetMute(void)
{
}

int WINAPI DebugSetLevel(void)
{
    return 0;
}

void WINAPI PSGPError(void *a, DWORD b, DWORD c)
{
}

void WINAPI PSGPSampleTexture(void *a, DWORD b, float (*c)[4], DWORD d, float (*e)[4])
{
}

/* ------------------------------------------------------------------------ */
/* DLL entry                                                                */
/* ------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        qd3d_rt_init();
        InitializeCriticalSection(&g_poll_cs);
        break;
    case DLL_PROCESS_DETACH:
        /* The VM driver's process cleanup ends the session, even when
         * another thread died holding our lock. FreeLibrary still sends
         * GOODBYE before explicitly releasing the slot. Loopback retains
         * its best-effort GOODBYE at process exit. */
        if (g_session && (!reserved || qd3d_rt_transport() != QD3D_TRANSPORT_VM) &&
            qd3d_rt_trylock()) {
            struct qd3d_call k;
            qd3d_rt_set_timeout(1000);
            qd3d_call_begin(&k, QD3D_OP_GOODBYE, 0, 0, 0u);
            (void)qd3d_call_end(&k);
            qd3d_call_done(&k);
            g_session = 0;
            qd3d_rt_unlock();
        }
        /* FreeLibrary: give the channel slot back and unmap the channel;
         * at process exit the kernel does both. */
        if (!reserved) {
            qd3d_rt_disconnect();
            if (g_stock) {
                FreeLibrary(g_stock);
                g_stock = NULL;
            }
        }
        break;
    default:
        break;
    }
    return TRUE;
}
