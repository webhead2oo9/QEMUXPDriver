/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: Direct3D 1-6 on the Direct3D 7 objects.  IDirect3D3/2/1 are
 * further interfaces of a DirectDraw object's Q7D3D; IDirect3DDevice3/2/1
 * further interfaces of a Q7Device whose version (3, 2 or 1) says which
 * CreateDevice made it.  A legacy device keeps the state the application
 * sees in the native legacy form (lrs, ls) and maps it onto the Direct3D 7
 * state the shared paths apply: TEXTUREMAPBLEND overrides stage 0 (without
 * changing what GetTextureStageState reports, as native), the light states
 * drive ambient, vertex fog and material, vertices with normals are lit on
 * the CPU with the DirectX 3-6 lighting model while a material is set
 * (q7_process_vertices), the projection includes the current viewport's
 * clip-space mapping, and the software devices also render into P8 targets
 * with a palette.  Texture handles, material handles and matrix handles are
 * process-wide.  Defaults and descriptions are the stock runtime's (measured
 * on Windows 11, docs/design/ddraw-remoting.md).
 */
#include "qdd_d3d7.h"

#define Q7_ALIVE_DEV(dev)                \
    do {                                 \
        if (!(dev)->d3d)                 \
            return DDERR_INVALIDOBJECT;  \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Handles                                                                   */

struct Q7HandleEntry {
    void *obj;
    unsigned kind;
};
static Q7HandleEntry *g_q7_handles;
static UINT g_q7_nhandles, g_q7_caphandles;

DWORD q7_handle_alloc(unsigned kind, void *obj)
{
    for (UINT i = 0; i < g_q7_nhandles; ++i)
        if (g_q7_handles[i].kind == Q7H_FREE) {
            g_q7_handles[i].obj = obj;
            g_q7_handles[i].kind = kind;
            return i + 1;
        }
    if (g_q7_nhandles == g_q7_caphandles) {
        UINT cap = g_q7_caphandles ? g_q7_caphandles * 2 : 64;
        Q7HandleEntry *n = (Q7HandleEntry *)realloc(g_q7_handles, cap * sizeof(*n));
        if (!n)
            return 0;
        g_q7_handles = n;
        g_q7_caphandles = cap;
    }
    g_q7_handles[g_q7_nhandles].obj = obj;
    g_q7_handles[g_q7_nhandles].kind = kind;
    return ++g_q7_nhandles;
}

void *q7_handle_get(DWORD h, unsigned kind)
{
    if (!h || h > g_q7_nhandles || g_q7_handles[h - 1].kind != kind)
        return NULL;
    return g_q7_handles[h - 1].obj;
}

void q7_handle_free(DWORD h)
{
    if (h && h <= g_q7_nhandles) {
        g_q7_handles[h - 1].obj = NULL;
        g_q7_handles[h - 1].kind = Q7H_FREE;
    }
}

void q7_handle_set(DWORD h, void *obj)
{
    if (h && h <= g_q7_nhandles)
        g_q7_handles[h - 1].obj = obj;
}

/* ------------------------------------------------------------------------ */
/* Lookups                                                                   */

Q7Device *q7_device3_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_DEVICE3) ? static_cast<Q7Dev3 *>((d7::IDirect3DDevice3 *)p)->o : NULL;
}

Q7Device *q7_device2_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_DEVICE2) ? static_cast<Q7Dev2 *>((d7::IDirect3DDevice2 *)p)->o : NULL;
}

Q7Device *q7_device1_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_DEVICE1) ? static_cast<Q7Dev1 *>((d7::IDirect3DDevice *)p)->o : NULL;
}

QddSurface *q7_tex2_surface(const void *p)
{
    return q7_registry_has(p, Q7K_TEX2) ? static_cast<Q7Tex2If *>((d7::IDirect3DTexture2 *)p)->o->s : NULL;
}

QddSurface *q7_tex1_surface(const void *p)
{
    return q7_registry_has(p, Q7K_TEX1) ? static_cast<Q7Tex1If *>((d7::IDirect3DTexture *)p)->o->s : NULL;
}

/* ------------------------------------------------------------------------ */
/* Device descriptions (stock values, Windows 11)                            */

/* The HEL description of the RGB device through IDirect3D3. */
static const DWORD q7_rgb_hel3[63] = {
    0x000000fc, 0x000007ff, 0x00000002, 0x00000551, 0x00000008, 0x00000001, 0x00000001, 0x00000010,
    0x0000000f, 0x00000001, 0x00000000, 0x00000038, 0x00000070, 0x000121f7, 0x000000ff, 0x00001fff,
    0x000007ff, 0x000000ff, 0x000c528a, 0x000000df, 0x0303033f, 0x000000cf, 0x0000001f, 0x00000004,
    0x00000004, 0x00000038, 0x00000070, 0x000121f7, 0x000000ff, 0x00001fff, 0x000007ff, 0x000000ff,
    0x000c528a, 0x000000df, 0x0303033f, 0x000000cf, 0x0000001f, 0x00000004, 0x00000004, 0x00000f00,
    0x00000400, 0x00000000, 0x000007e5, 0x00000001, 0x00000001, 0x00000400, 0x00000400, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000100, 0x00000000, 0x00000001, 0xc7000000, 0xc7000000,
    0x46fffe00, 0x46fffe00, 0x00000000, 0x000000ff, 0x00000002, 0x000078ff, 0x00020002,
};
/* The HEL description of the RGB and MMX devices through IDirect3D2 and
 * IDirect3D (the first 51 / 43 DWORDs). */
static const DWORD q7_rgb_hel2[51] = {
    0x000000cc, 0x000007df, 0x00000002, 0x0000055b, 0x00000008, 0x00000001, 0x00000001, 0x00000010,
    0x0000000f, 0x00000001, 0x00000000, 0x00000038, 0x00000070, 0x000001d1, 0x000000ff, 0x00000000,
    0x00000000, 0x00000000, 0x000ca28a, 0x0000000b, 0x0000001f, 0x00000042, 0x00000001, 0x00000004,
    0x00000004, 0x00000038, 0x00000070, 0x000001d1, 0x000000ff, 0x00000000, 0x00000000, 0x00000000,
    0x000ca28a, 0x0000000b, 0x0000001f, 0x00000042, 0x00000001, 0x00000004, 0x00000004, 0x00000f00,
    0x00000400, 0x00000000, 0x000007e5, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000,
};
/* The HEL description of the Ramp device (IDirect3D2 and IDirect3D). */
static const DWORD q7_ramp_hel2[51] = {
    0x000000cc, 0x000007df, 0x00000001, 0x0000055b, 0x00000008, 0x00000001, 0x00000001, 0x00000010,
    0x0000000f, 0x00000002, 0x00000000, 0x00000038, 0x00000070, 0x00000051, 0x000000ff, 0x00000000,
    0x00000000, 0x00000000, 0x0000a145, 0x0000000b, 0x00000005, 0x00000042, 0x00000001, 0x00000004,
    0x00000004, 0x00000038, 0x00000070, 0x00000051, 0x000000ff, 0x00000000, 0x00000000, 0x00000000,
    0x0000a145, 0x0000000b, 0x00000005, 0x00000042, 0x00000001, 0x00000004, 0x00000004, 0x00000f00,
    0x00000400, 0x00000000, 0x000007e5, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000,
};

static DWORD q7_desc_size(UINT v)
{
    return v >= 3 ? 252u : v == 2 ? 204u : 172u;
}

/* What every description carries, also an empty one. */
static void q7_desc_empty(d7::D3DDEVICEDESC *d)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dtcTransformCaps.dwSize = sizeof(d->dtcTransformCaps);
    d->dlcLightingCaps.dwSize = sizeof(d->dlcLightingCaps);
    d->dpcLineCaps.dwSize = sizeof(d->dpcLineCaps);
    d->dpcTriCaps.dwSize = sizeof(d->dpcTriCaps);
}

/* The HAL device's hardware description: the T&L HAL's D3D7 one in the
 * legacy layout. */
static void q7_desc_hal(d7::D3DDEVICEDESC *o, const d7::D3DDEVICEDESC7 *d)
{
    q7_desc_empty(o);
    o->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH |
                 D3DDD_DEVICEZBUFFERBITDEPTH;
    o->dcmColorModel = D3DCOLOR_RGB;
    o->dwDevCaps = d->dwDevCaps;
    o->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    o->bClipping = TRUE;
    o->dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
    o->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    o->dlcLightingCaps.dwNumLights = 8;
    o->dpcLineCaps = d->dpcLineCaps;
    o->dpcTriCaps = d->dpcTriCaps;
    o->dwDeviceRenderBitDepth = d->dwDeviceRenderBitDepth;
    o->dwDeviceZBufferBitDepth = d->dwDeviceZBufferBitDepth;
    o->dwMaxVertexCount = 0xfffe;
    o->dwMinTextureWidth = d->dwMinTextureWidth;
    o->dwMinTextureHeight = d->dwMinTextureHeight;
    o->dwMaxTextureWidth = d->dwMaxTextureWidth;
    o->dwMaxTextureHeight = d->dwMaxTextureHeight;
    o->dwMaxTextureRepeat = d->dwMaxTextureRepeat;
    o->dwMaxTextureAspectRatio = d->dwMaxTextureAspectRatio;
    o->dwMaxAnisotropy = d->dwMaxAnisotropy;
    o->dvGuardBandLeft = d->dvGuardBandLeft;
    o->dvGuardBandTop = d->dvGuardBandTop;
    o->dvGuardBandRight = d->dvGuardBandRight;
    o->dvGuardBandBottom = d->dvGuardBandBottom;
    o->dvExtentsAdjust = d->dvExtentsAdjust;
    o->dwStencilCaps = d->dwStencilCaps;
    o->dwFVFCaps = d->dwFVFCaps;
    o->dwTextureOpCaps = d->dwTextureOpCaps;
    o->wMaxTextureBlendStages = d->wMaxTextureBlendStages;
    o->wMaxSimultaneousTextures = d->wMaxSimultaneousTextures;
}

/* The HAL device's software (HEL) description. */
static void q7_desc_hal_hel(d7::D3DDEVICEDESC *o)
{
    q7_desc_empty(o);
    o->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING;
    o->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX;
    o->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    o->bClipping = TRUE;
    o->dlcLightingCaps.dwCaps =
        D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL | D3DLIGHTCAPS_PARALLELPOINT;
    o->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    o->dwMaxVertexCount = 0xfffe;
}

struct Q7LegacyDev {
    const GUID *guid;
    const char *desc, *name;
};

static const Q7LegacyDev q7_dev_ramp = {&Q7IID_IDirect3DRampDevice, "Microsoft Direct3D Mono(Ramp) Software Emulation",
                                        "Ramp Emulation"};
static const Q7LegacyDev q7_dev_rgb = {&Q7IID_IDirect3DRGBDevice, "Microsoft Direct3D RGB Software Emulation",
                                       "RGB Emulation"};
static const Q7LegacyDev q7_dev_hal = {&Q7IID_IDirect3DHALDevice,
                                       "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
                                       "Direct3D HAL"};
static const Q7LegacyDev q7_dev_mmx = {&Q7IID_IDirect3DMMXDevice, "Microsoft Direct3D MMX Software Emulation",
                                       "MMX Emulation"};

/* The devices EnumDevices of interface version v lists, in order. */
static UINT q7_legacy_devices(UINT v, const Q7LegacyDev **out)
{
    if (v >= 3) {
        out[0] = &q7_dev_rgb;
        out[1] = &q7_dev_hal;
        return 2;
    }
    out[0] = &q7_dev_ramp;
    out[1] = &q7_dev_rgb;
    out[2] = &q7_dev_hal;
    if (v == 2) {
        out[3] = &q7_dev_mmx;
        return 4;
    }
    return 3;
}

bool Q7D3D::legacy_desc(UINT v, const GUID *guid, d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel)
{
    if (IsEqualGUID(*guid, Q7IID_IDirect3DHALDevice)) {
        d7::D3DDEVICEDESC7 d;
        if (FAILED(device_desc(&Q7IID_IDirect3DTnLHalDevice, &d)))
            return false;
        q7_desc_hal(hal, &d);
        q7_desc_hal_hel(hel);
    } else if (IsEqualGUID(*guid, Q7IID_IDirect3DRGBDevice) ||
               (v < 3 && IsEqualGUID(*guid, Q7IID_IDirect3DMMXDevice))) {
        q7_desc_empty(hal);
        memset(hel, 0, sizeof(*hel));
        if (v >= 3)
            memcpy(hel, q7_rgb_hel3, sizeof(q7_rgb_hel3));
        else
            memcpy(hel, q7_rgb_hel2, sizeof(q7_rgb_hel2));
    } else if (v < 3 && IsEqualGUID(*guid, Q7IID_IDirect3DRampDevice)) {
        q7_desc_empty(hal);
        memset(hel, 0, sizeof(*hel));
        memcpy(hel, q7_ramp_hel2, sizeof(q7_ramp_hel2));
    } else {
        return false;
    }
    /* The versions' descriptions are prefixes of the DirectX 6 one. */
    DWORD size = q7_desc_size(v);
    memset((BYTE *)hal + size, 0, sizeof(*hal) - size);
    memset((BYTE *)hel + size, 0, sizeof(*hel) - size);
    hal->dwSize = hel->dwSize = size;
    return true;
}

HRESULT Q7D3D::legacy_enum(UINT v, d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    const Q7LegacyDev *devs[4];
    UINT n = q7_legacy_devices(v, devs);
    for (UINT i = 0; i < n; ++i) {
        d7::D3DDEVICEDESC hal, hel;
        if (!legacy_desc(v, devs[i]->guid, &hal, &hel))
            continue;
        /* Writable copies: some games write into the strings. */
        char desc[128], name[64];
        size_t dl = strlen(devs[i]->desc), nl = strlen(devs[i]->name);
        memcpy(desc, devs[i]->desc, dl + 1); /* both fit: fixed strings below 128 / 64 */
        memcpy(name, devs[i]->name, nl + 1);
        GUID g = *devs[i]->guid;
        if (cb(&g, desc, name, &hal, &hel, ctx) != D3DENUMRET_OK)
            break;
    }
    return D3D_OK;
}

HRESULT Q7D3D::legacy_find(UINT v, d7::D3DFINDDEVICESEARCH *fds, d7::D3DFINDDEVICERESULT *fdr)
{
    if (!fds || !fdr || fds->dwSize != sizeof(*fds))
        return DDERR_INVALIDPARAMS;
    /* The result structure of DirectX 6, 5 or 3 (two descriptions of 252,
     * 204 or 172 bytes after the size and the GUID). */
    DWORD dsize;
    if (fdr->dwSize == 20 + 2 * 252)
        dsize = 252;
    else if (fdr->dwSize == 20 + 2 * 204)
        dsize = 204;
    else if (fdr->dwSize == 20 + 2 * 172)
        dsize = 172;
    else
        return DDERR_INVALIDPARAMS;
    const GUID *g = NULL;
    if (fds->dwFlags & D3DFDS_GUID) {
        const Q7LegacyDev *devs[4];
        UINT n = q7_legacy_devices(v, devs);
        for (UINT i = 0; i < n && !g; ++i)
            if (IsEqualGUID(*devs[i]->guid, fds->guid))
                g = devs[i]->guid;
        if (!g)
            return DDERR_NOTFOUND;
    } else if ((fds->dwFlags & D3DFDS_HARDWARE) && !fds->bHardware) {
        g = &Q7IID_IDirect3DRGBDevice;
    } else {
        g = &Q7IID_IDirect3DHALDevice;
    }
    d7::D3DDEVICEDESC hal, hel;
    if (!legacy_desc(v, g, &hal, &hel))
        return DDERR_NOTFOUND;
    BYTE *p = (BYTE *)fdr;
    memcpy(p + 4, g, sizeof(GUID));
    memcpy(p + 20, &hal, dsize);
    memcpy(p + 20 + dsize, &hel, dsize);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* IDirect3D3 / IDirect3D2 / IDirect3D                                       */

static IUnknown *q7_draw_unknown(Q7D3D *d)
{
    return (IUnknown *)qdd_draw_iface(d->dd, 1);
}

static HRESULT q7_create_light(d7::IDirect3DLight **out, IUnknown *outer)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    Q7LightObj *l = new Q7LightObj();
    if (!l)
        return DDERR_OUTOFMEMORY;
    if (!q7_registry_add(static_cast<d7::IDirect3DLight *>(l), Q7K_LIGHT)) {
        delete l;
        return DDERR_OUTOFMEMORY;
    }
    *out = l;
    return D3D_OK;
}

static HRESULT q7_create_material(Q7Material **out, IUnknown *outer)
{
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    Q7Material *m = new Q7Material();
    if (!m)
        return DDERR_OUTOFMEMORY;
    if (!q7_registry_add(static_cast<d7::IDirect3DMaterial3 *>(&m->if3), Q7K_MATERIAL3) ||
        !q7_registry_add(static_cast<d7::IDirect3DMaterial2 *>(&m->if2), Q7K_MATERIAL2) ||
        !q7_registry_add(static_cast<d7::IDirect3DMaterial *>(&m->if1), Q7K_MATERIAL1)) {
        delete m;
        return DDERR_OUTOFMEMORY;
    }
    *out = m;
    return D3D_OK;
}

static HRESULT q7_create_viewport(Q7D3D *d, Q7Viewport **out, IUnknown *outer)
{
    *out = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    Q7Viewport *vp = new Q7Viewport(d);
    if (!vp)
        return DDERR_OUTOFMEMORY;
    if (!q7_registry_add(static_cast<d7::IDirect3DViewport3 *>(vp), Q7K_VIEWPORT)) {
        delete vp;
        return DDERR_OUTOFMEMORY;
    }
    *out = vp;
    return D3D_OK;
}

/* A legacy device of version v (3: IDirectDrawSurface4 target, 2:
 * IDirectDrawSurface) made by CreateDevice. */
static HRESULT q7_create_legacy_device(Q7D3D *d, REFCLSID rclsid, IUnknown *surface, UINT v, Q7Device **out)
{
    *out = NULL;
    QddSurface *s = surface ? qdd_surface_lookup(surface) : NULL;
    Q7Device *dev;
    HRESULT hr = d->create_device(rclsid, s, surface, v, &dev);
    if (FAILED(hr))
        return hr;
    dev->legacy_init();
    *out = dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7D3D3::QueryInterface(REFIID riid, void **obj)
{
    return q7_draw_unknown(o)->QueryInterface(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7D3D3::AddRef()
{
    return q7_draw_unknown(o)->AddRef();
}
ULONG STDMETHODCALLTYPE Q7D3D3::Release()
{
    return q7_draw_unknown(o)->Release();
}
HRESULT STDMETHODCALLTYPE Q7D3D3::EnumDevices(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    Q7Lock lock;
    return o->legacy_enum(3, cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7D3D3::CreateLight(d7::IDirect3DLight **light, IUnknown *outer)
{
    Q7Lock lock;
    return q7_create_light(light, outer);
}
HRESULT STDMETHODCALLTYPE Q7D3D3::CreateMaterial(d7::IDirect3DMaterial3 **material, IUnknown *outer)
{
    if (!material)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Material *m;
    HRESULT hr = q7_create_material(&m, outer);
    *material = SUCCEEDED(hr) ? &m->if3 : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D3::CreateViewport(d7::IDirect3DViewport3 **viewport, IUnknown *outer)
{
    if (!viewport)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Viewport *vp;
    HRESULT hr = q7_create_viewport(o, &vp, outer);
    *viewport = SUCCEEDED(hr) ? vp : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D3::FindDevice(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result)
{
    Q7Lock lock;
    return o->legacy_find(3, search, result);
}
HRESULT STDMETHODCALLTYPE Q7D3D3::CreateDevice(REFCLSID rclsid, IDirectDrawSurface4 *surface,
                                               d7::IDirect3DDevice3 **device, IUnknown *outer)
{
    if (!device)
        return DDERR_INVALIDPARAMS;
    *device = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    Q7Lock lock;
    Q7Device *dev;
    HRESULT hr = q7_create_legacy_device(o, rclsid, surface, 3, &dev);
    if (SUCCEEDED(hr))
        *device = &dev->if3;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D3::CreateVertexBuffer(d7::D3DVERTEXBUFFERDESC *desc, d7::IDirect3DVertexBuffer **buffer,
                                                     DWORD flags, IUnknown *outer)
{
    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!desc || !buffer)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7VB *vb;
    HRESULT hr = o->create_vb(desc, 3, &vb);
    /* The DirectX 6 interface shares the buffer's vtable layout (its last
     * method, ProcessVerticesStrided, is new in DirectX 7). */
    *buffer = SUCCEEDED(hr) ? (d7::IDirect3DVertexBuffer *)static_cast<d7::IDirect3DVertexBuffer7 *>(vb) : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D3::EnumZBufferFormats(REFCLSID device_iid, d7::LPD3DENUMPIXELFORMATSCALLBACK cb,
                                                     void *ctx)
{
    return o->EnumZBufferFormats(device_iid, cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7D3D3::EvictManagedTextures()
{
    return o->EvictManagedTextures();
}

HRESULT STDMETHODCALLTYPE Q7D3D2::QueryInterface(REFIID riid, void **obj)
{
    return q7_draw_unknown(o)->QueryInterface(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7D3D2::AddRef()
{
    return q7_draw_unknown(o)->AddRef();
}
ULONG STDMETHODCALLTYPE Q7D3D2::Release()
{
    return q7_draw_unknown(o)->Release();
}
HRESULT STDMETHODCALLTYPE Q7D3D2::EnumDevices(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    Q7Lock lock;
    return o->legacy_enum(2, cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7D3D2::CreateLight(d7::IDirect3DLight **light, IUnknown *outer)
{
    Q7Lock lock;
    return q7_create_light(light, outer);
}
HRESULT STDMETHODCALLTYPE Q7D3D2::CreateMaterial(d7::IDirect3DMaterial2 **material, IUnknown *outer)
{
    if (!material)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Material *m;
    HRESULT hr = q7_create_material(&m, outer);
    *material = SUCCEEDED(hr) ? &m->if2 : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D2::CreateViewport(d7::IDirect3DViewport2 **viewport, IUnknown *outer)
{
    if (!viewport)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Viewport *vp;
    HRESULT hr = q7_create_viewport(o, &vp, outer);
    *viewport = SUCCEEDED(hr) ? vp : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D2::FindDevice(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result)
{
    Q7Lock lock;
    return o->legacy_find(2, search, result);
}
HRESULT STDMETHODCALLTYPE Q7D3D2::CreateDevice(REFCLSID rclsid, IDirectDrawSurface *surface,
                                               d7::IDirect3DDevice2 **device)
{
    if (!device)
        return DDERR_INVALIDPARAMS;
    *device = NULL;
    Q7Lock lock;
    Q7Device *dev;
    HRESULT hr = q7_create_legacy_device(o, rclsid, surface, 2, &dev);
    if (SUCCEEDED(hr))
        *device = &dev->if2;
    return hr;
}

HRESULT STDMETHODCALLTYPE Q7D3D1::QueryInterface(REFIID riid, void **obj)
{
    return q7_draw_unknown(o)->QueryInterface(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7D3D1::AddRef()
{
    return q7_draw_unknown(o)->AddRef();
}
ULONG STDMETHODCALLTYPE Q7D3D1::Release()
{
    return q7_draw_unknown(o)->Release();
}
HRESULT STDMETHODCALLTYPE Q7D3D1::Initialize(REFIID riid)
{
    return DDERR_ALREADYINITIALIZED;
}
HRESULT STDMETHODCALLTYPE Q7D3D1::EnumDevices(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    Q7Lock lock;
    return o->legacy_enum(1, cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7D3D1::CreateLight(d7::IDirect3DLight **light, IUnknown *outer)
{
    Q7Lock lock;
    return q7_create_light(light, outer);
}
HRESULT STDMETHODCALLTYPE Q7D3D1::CreateMaterial(d7::IDirect3DMaterial **material, IUnknown *outer)
{
    if (!material)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Material *m;
    HRESULT hr = q7_create_material(&m, outer);
    *material = SUCCEEDED(hr) ? &m->if1 : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D1::CreateViewport(d7::IDirect3DViewport **viewport, IUnknown *outer)
{
    if (!viewport)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Viewport *vp;
    HRESULT hr = q7_create_viewport(o, &vp, outer);
    *viewport = SUCCEEDED(hr) ? vp : NULL;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7D3D1::FindDevice(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result)
{
    Q7Lock lock;
    return o->legacy_find(1, search, result);
}

/* ------------------------------------------------------------------------ */
/* Device lifetime and identity                                              */

/* Render states a legacy device reports before any is set (measured,
 * Windows 11 IDirect3DDevice3/2): the DirectX 7 states read 0xffffffff,
 * the rest as listed; everything else 0. */
static void q7_legacy_rs_defaults(DWORD *rs, UINT version, bool zbuffer)
{
    memset(rs, 0, Q7_RS * sizeof(DWORD));
    static const struct {
        DWORD state, value;
    } defs[] = {
        {d7::D3DRENDERSTATE_TEXTUREADDRESS, d7::D3DTADDRESS_WRAP},
        {d7::D3DRENDERSTATE_FILLMODE, d7::D3DFILL_SOLID},
        {d7::D3DRENDERSTATE_SHADEMODE, d7::D3DSHADE_GOURAUD},
        {d7::D3DRENDERSTATE_ROP2, R2_COPYPEN},
        {d7::D3DRENDERSTATE_PLANEMASK, 0xffffffffu},
        {d7::D3DRENDERSTATE_ZWRITEENABLE, TRUE},
        {d7::D3DRENDERSTATE_LASTPIXEL, TRUE},
        {d7::D3DRENDERSTATE_TEXTUREMAG, d7::D3DFILTER_NEAREST},
        {d7::D3DRENDERSTATE_TEXTUREMIN, d7::D3DFILTER_NEAREST},
        {d7::D3DRENDERSTATE_SRCBLEND, d7::D3DBLEND_ONE},
        {d7::D3DRENDERSTATE_DESTBLEND, d7::D3DBLEND_ZERO},
        {d7::D3DRENDERSTATE_TEXTUREMAPBLEND, d7::D3DTBLEND_MODULATE},
        {d7::D3DRENDERSTATE_CULLMODE, d7::D3DCULL_CCW},
        {d7::D3DRENDERSTATE_ZFUNC, d7::D3DCMP_LESSEQUAL},
        {d7::D3DRENDERSTATE_ALPHAFUNC, d7::D3DCMP_ALWAYS},
        {d7::D3DRENDERSTATE_FOGEND, 0x3f800000u},
        {d7::D3DRENDERSTATE_FOGDENSITY, 0x3f800000u},
        {42, 0xffffffffu},
        {d7::D3DRENDERSTATE_TEXTUREADDRESSU, d7::D3DTADDRESS_WRAP},
        {d7::D3DRENDERSTATE_TEXTUREADDRESSV, d7::D3DTADDRESS_WRAP},
        {d7::D3DRENDERSTATE_ANISOTROPY, 1},
        {d7::D3DRENDERSTATE_FLUSHBATCH, 0xffffffffu},
        {51, 0xffffffffu},
        {d7::D3DRENDERSTATE_STENCILFAIL, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILZFAIL, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILPASS, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILFUNC, d7::D3DCMP_ALWAYS},
        {d7::D3DRENDERSTATE_STENCILMASK, 0xffffffffu},
        {d7::D3DRENDERSTATE_STENCILWRITEMASK, 0xffffffffu},
        {d7::D3DRENDERSTATE_TEXTUREFACTOR, 0xffffffffu},
        {61, 0xffffffffu},
        {62, 0xffffffffu},
        {63, 0xffffffffu},
    };
    for (UINT i = 0; i < sizeof(defs) / sizeof(defs[0]); ++i)
        rs[defs[i].state] = defs[i].value;
    for (DWORD s = 96; s < 128; ++s)
        rs[s] = 0xffffffffu;
    for (DWORD s = 136; s < Q7_RS; ++s)
        rs[s] = 0xffffffffu;
    rs[d7::D3DRENDERSTATE_ZENABLE] = zbuffer ? d7::D3DZB_TRUE : d7::D3DZB_FALSE;
    if (version >= 3)
        rs[d7::D3DRENDERSTATE_TEXTUREPERSPECTIVE] = TRUE;
    /* Wine's device setup: specular on for IDirect3DDevice2 (measured too),
     * colour keying on for IDirect3DDevice. */
    if (version == 2)
        rs[d7::D3DRENDERSTATE_SPECULARENABLE] = TRUE;
    if (version == 1)
        rs[d7::D3DRENDERSTATE_COLORKEYENABLE] = TRUE;
}

void Q7Device::legacy_init()
{
    bool z = st.rs[d7::D3DRENDERSTATE_ZENABLE] != d7::D3DZB_FALSE;
    q7_legacy_rs_defaults(lrs, version, z);
    memset(ls, 0, sizeof(ls));
    if (version >= 3)
        ls[d7::D3DLIGHTSTATE_COLORVERTEX] = TRUE;
    /* The applied (D3D7) state follows the legacy values for the states
     * both know. */
    for (DWORD s = 1; s < 136; ++s)
        if (q7_rs_kind(s) != Q7RS_INVALID)
            st.rs[s] = lrs[s];
    st.rs[d7::D3DRENDERSTATE_ANTIALIAS] = d7::D3DANTIALIAS_NONE;
    st.rs[d7::D3DRENDERSTATE_LIGHTING] = FALSE;
    st.rs[d7::D3DRENDERSTATE_NORMALIZENORMALS] = TRUE;
    st.rs[d7::D3DRENDERSTATE_AMBIENT] = 0;
    st.rs[d7::D3DRENDERSTATE_FOGVERTEXMODE] = d7::D3DFOG_NONE;
    st.rs[d7::D3DRENDERSTATE_COLORVERTEX] = TRUE;
    /* Texture transforms are a DirectX 7 state: stored, never applied. */
    for (UINT stage = 0; stage < Q7_STAGES; ++stage)
        st.tss[stage][d7::D3DTSS_TEXTURETRANSFORMFLAGS] = 0xffffffffu;
    static const D3DMATRIX ident = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    legacy_proj = ident;
    legacy_clip = ident;
    legacy_fog();
    legacy_wrap();
    set_tmblend(d7::D3DTBLEND_MODULATE);
    q7_registry_add(static_cast<d7::IDirect3DDevice3 *>(&if3), Q7K_DEVICE3);
    q7_registry_add(static_cast<d7::IDirect3DDevice2 *>(&if2), Q7K_DEVICE2);
    q7_registry_add(static_cast<d7::IDirect3DDevice *>(&if1), Q7K_DEVICE1);
}

void Q7Device::legacy_teardown()
{
    if (version < 7) {
        q7_registry_remove(static_cast<d7::IDirect3DDevice3 *>(&if3));
        q7_registry_remove(static_cast<d7::IDirect3DDevice2 *>(&if2));
        q7_registry_remove(static_cast<d7::IDirect3DDevice *>(&if1));
    }
    /* AddViewport's references go; so does the current viewport's on
     * IDirect3DDevice3, while IDirect3DDevice2 and older leak it (native,
     * Wine's ddraw2/ddraw4 test_viewport_object). */
    if (cur_vp) {
        Q7Viewport *vp = cur_vp;
        cur_vp = NULL;
        if (version >= 3)
            vp->Release();
    }
    while (viewports) {
        Q7Viewport *vp = viewports;
        viewports = vp->next;
        vp->next = NULL;
        vp->device = NULL;
        vp->Release();
    }
    free(bv_buf);
    bv_buf = NULL;
    free(bv_idx);
    bv_idx = NULL;
    free(lit_buf);
    lit_buf = NULL;
    lit_cap = 0;
}

ULONG Q7Device::outer_addref()
{
    return outer ? outer->AddRef() : AddRef();
}

ULONG Q7Device::outer_release()
{
    return outer ? outer->Release() : Release();
}

HRESULT Q7Device::outer_query(REFIID riid, void **obj)
{
    return outer ? outer->QueryInterface(riid, obj) : inner_query(riid, obj);
}

/* Which interfaces a device answers (Wine's d3d_device_inner_QueryInterface;
 * native: IDirect3DDevice7 only on version-7 devices). */
HRESULT Q7Device::inner_query(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    *obj = NULL;
    if (IsEqualGUID(riid, IID_IUnknown))
        *obj = static_cast<d7::IDirect3DDevice7 *>(this);
    else if (version == 7 && IsEqualGUID(riid, Q7IID_IDirect3DDevice7))
        *obj = static_cast<d7::IDirect3DDevice7 *>(this);
    else if (version == 3 && IsEqualGUID(riid, Q7IID_IDirect3DDevice3))
        *obj = static_cast<d7::IDirect3DDevice3 *>(&if3);
    else if (version >= 2 && version < 7 && IsEqualGUID(riid, Q7IID_IDirect3DDevice2))
        *obj = static_cast<d7::IDirect3DDevice2 *>(&if2);
    else if (version < 7 && IsEqualGUID(riid, Q7IID_IDirect3DDevice))
        *obj = static_cast<d7::IDirect3DDevice *>(&if1);
    if (!*obj)
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

/* ------------------------------------------------------------------------ */
/* Legacy state                                                              */

/* Vertex fog takes its parameters from the light states, table fog from the
 * render states; Direct3D 9 has one set. */
void Q7Device::legacy_fog()
{
    bool table = lrs[d7::D3DRENDERSTATE_FOGTABLEMODE] != d7::D3DFOG_NONE;
    set_rs(d7::D3DRENDERSTATE_FOGSTART, table ? lrs[d7::D3DRENDERSTATE_FOGSTART] : ls[d7::D3DLIGHTSTATE_FOGSTART]);
    set_rs(d7::D3DRENDERSTATE_FOGEND, table ? lrs[d7::D3DRENDERSTATE_FOGEND] : ls[d7::D3DLIGHTSTATE_FOGEND]);
    set_rs(d7::D3DRENDERSTATE_FOGDENSITY,
           table ? lrs[d7::D3DRENDERSTATE_FOGDENSITY] : ls[d7::D3DLIGHTSTATE_FOGDENSITY]);
}

/* WRAPU/WRAPV wrap the first texture coordinate set like WRAP0. */
void Q7Device::legacy_wrap()
{
    DWORD w = lrs[d7::D3DRENDERSTATE_WRAP0];
    if (lrs[d7::D3DRENDERSTATE_WRAPU])
        w |= D3DWRAP_U;
    if (lrs[d7::D3DRENDERSTATE_WRAPV])
        w |= D3DWRAP_V;
    set_rs(d7::D3DRENDERSTATE_WRAP0, w);
}

/* D3DRENDERSTATE_TEXTUREMAPBLEND as stage 0 states (the DirectX 6 mapping;
 * MODULATE takes alpha from the texture only when it has alpha, which
 * apply_derived decides per texture).  What GetTextureStageState reports
 * does not change (native); a later SetTextureStageState of a state wins. */
void Q7Device::set_tmblend(DWORD mode)
{
    struct Set {
        DWORD type, value;
    };
    static const Set modulate[] = {
        {d7::D3DTSS_COLOROP, d7::D3DTOP_MODULATE}, {d7::D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {d7::D3DTSS_COLORARG2, D3DTA_CURRENT},     {d7::D3DTSS_ALPHAOP, d7::D3DTOP_SELECTARG1},
        {d7::D3DTSS_ALPHAARG1, D3DTA_TEXTURE},     {d7::D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    };
    static const Set add[] = {
        {d7::D3DTSS_COLOROP, d7::D3DTOP_ADD},       {d7::D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {d7::D3DTSS_COLORARG2, D3DTA_CURRENT},      {d7::D3DTSS_ALPHAOP, d7::D3DTOP_SELECTARG2},
        {d7::D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    };
    static const Set modulatealpha[] = {
        {d7::D3DTSS_COLOROP, d7::D3DTOP_MODULATE}, {d7::D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {d7::D3DTSS_COLORARG2, D3DTA_CURRENT},     {d7::D3DTSS_ALPHAOP, d7::D3DTOP_MODULATE},
        {d7::D3DTSS_ALPHAARG1, D3DTA_TEXTURE},     {d7::D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    };
    static const Set copy[] = {
        {d7::D3DTSS_COLOROP, d7::D3DTOP_SELECTARG1},
        {d7::D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {d7::D3DTSS_ALPHAOP, d7::D3DTOP_SELECTARG1},
        {d7::D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
    };
    static const Set decalalpha[] = {
        {d7::D3DTSS_COLOROP, d7::D3DTOP_BLENDTEXTUREALPHA}, {d7::D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {d7::D3DTSS_COLORARG2, D3DTA_CURRENT},              {d7::D3DTSS_ALPHAOP, d7::D3DTOP_SELECTARG2},
        {d7::D3DTSS_ALPHAARG2, D3DTA_CURRENT},
    };
    const Set *set = NULL;
    UINT n = 0;
    switch (mode) {
    case d7::D3DTBLEND_MODULATE: set = modulate; n = sizeof(modulate) / sizeof(*set); break;
    case d7::D3DTBLEND_ADD: set = add; n = sizeof(add) / sizeof(*set); break;
    case d7::D3DTBLEND_MODULATEALPHA: set = modulatealpha; n = sizeof(modulatealpha) / sizeof(*set); break;
    case d7::D3DTBLEND_COPY:
    case d7::D3DTBLEND_DECAL: set = copy; n = sizeof(copy) / sizeof(*set); break;
    case d7::D3DTBLEND_DECALALPHA: set = decalalpha; n = sizeof(decalalpha) / sizeof(*set); break;
    default: break; /* DECALMASK, MODULATEMASK: no stage equivalent, stage 0 unchanged */
    }
    tmblend = mode;
    legacy_blend = true;
    for (UINT i = 0; i < n; ++i) {
        tss0_val[set[i].type] = set[i].value;
        tss0_set |= 1u << set[i].type;
    }
    derived.valid = false;
    if (is_active())
        for (DWORD t = d7::D3DTSS_COLOROP; t <= d7::D3DTSS_ALPHAARG2; ++t)
            apply_tss(0, t, st.tss[0][t]);
}

/* A draw is lit for vertices with normals while a material is set, unless
 * D3DDP_DONOTLIGHT (Wine's setup_lighting). */
bool Q7Device::legacy_lit(DWORD fvf, DWORD flags) const
{
    return material && (fvf & D3DFVF_NORMAL) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ &&
           !(flags & D3DDP_DONOTLIGHT);
}

/* Legacy lit draws light on the CPU, as the stock DirectX 3-6 runtime does
 * on its HAL device (no hardware T&L there): legacy attenuation, normals
 * through the world-view matrix itself, any number of lights; the GPU then
 * transforms, clips and fogs the pre-lit vertices with lighting off. */
BYTE *Q7Device::legacy_light(const Q7Source *src, DWORD count, DWORD *out_fvf, UINT *out_stride)
{
    DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | (src->fvf & (D3DFVF_TEXCOUNT_MASK | 0xffff0000u));
    UINT stride = q7_fvf_size(fvf);
    if (!stride || count > 0x7fffffffu / stride)
        return NULL;
    UINT need = count * stride;
    if (need > lit_cap) {
        BYTE *n = (BYTE *)realloc(lit_buf, need);
        if (!n)
            return NULL;
        lit_buf = n;
        lit_cap = need;
    }
    q7_process_vertices(&st, pv_power, D3DVOP_TRANSFORM | D3DVOP_LIGHT | 0x58000000u, src, count, lit_buf, fvf, stride,
                        need);
    *out_fvf = fvf;
    *out_stride = stride;
    return lit_buf;
}

void Q7Device::legacy_set_clip(const D3DMATRIX *clip)
{
    legacy_clip = *clip;
    D3DMATRIX eff;
    q7_mat_mul(&eff, &legacy_proj, &legacy_clip);
    set_transform(d7::D3DTRANSFORMSTATE_PROJECTION, &eff);
}

static DWORD q7_minfilter_of(DWORD f, DWORD *mip)
{
    switch (f) {
    case d7::D3DFILTER_LINEAR: *mip = d7::D3DTFP_NONE; return d7::D3DTFN_LINEAR;
    case d7::D3DFILTER_MIPNEAREST: *mip = d7::D3DTFP_POINT; return d7::D3DTFN_POINT;
    case d7::D3DFILTER_MIPLINEAR: *mip = d7::D3DTFP_POINT; return d7::D3DTFN_LINEAR;
    case d7::D3DFILTER_LINEARMIPNEAREST: *mip = d7::D3DTFP_LINEAR; return d7::D3DTFN_POINT;
    case d7::D3DFILTER_LINEARMIPLINEAR: *mip = d7::D3DTFP_LINEAR; return d7::D3DTFN_LINEAR;
    default: *mip = d7::D3DTFP_NONE; return d7::D3DTFN_POINT;
    }
}

HRESULT Q7Device::legacy_set_rs(DWORD s, DWORD v)
{
    if (!s || s >= Q7_RS)
        return DDERR_INVALIDPARAMS;
    switch (s) {
    case d7::D3DRENDERSTATE_TEXTUREHANDLE: {
        QddSurface *tex = NULL;
        if (v) {
            Q7Surf *x = (Q7Surf *)q7_handle_get(v, Q7H_TEXTURE);
            if (!x)
                return DDERR_INVALIDPARAMS;
            tex = x->s;
        }
        lrs[s] = v;
        HRESULT hr = legacy_set_texture(0, tex);
        /* Stock IDirect3DDevice3::GetTexture(0) reports what SetTexture
         * bound, NULL for a TEXTUREHANDLE binding (measured,
         * qdd_d3d7_legacy_smoke handle3). */
        if (SUCCEEDED(hr))
            tex0_handle = tex != NULL;
        return hr;
    }
    case d7::D3DRENDERSTATE_TEXTUREMAPBLEND:
        /* The value it already has changes nothing, so stage states set
         * since stay (Wine's ddraw4 test_texturemapblend). */
        if (v != tmblend)
            set_tmblend(v);
        lrs[s] = v;
        return D3D_OK;
    case d7::D3DRENDERSTATE_TEXTUREADDRESS:
        lrs[s] = v;
        lrs[d7::D3DRENDERSTATE_TEXTUREADDRESSU] = lrs[d7::D3DRENDERSTATE_TEXTUREADDRESSV] = v;
        return set_tss(0, d7::D3DTSS_ADDRESS, v);
    case d7::D3DRENDERSTATE_TEXTUREADDRESSU:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_ADDRESSU, v);
    case d7::D3DRENDERSTATE_TEXTUREADDRESSV:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_ADDRESSV, v);
    case d7::D3DRENDERSTATE_TEXTUREMAG:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_MAGFILTER,
                       v == d7::D3DFILTER_NEAREST || v == d7::D3DFILTER_MIPNEAREST ||
                               v == d7::D3DFILTER_LINEARMIPNEAREST
                           ? d7::D3DTFG_POINT
                           : d7::D3DTFG_LINEAR);
    case d7::D3DRENDERSTATE_TEXTUREMIN: {
        lrs[s] = v;
        DWORD mip;
        DWORD min = q7_minfilter_of(v, &mip);
        set_tss(0, d7::D3DTSS_MINFILTER, min);
        return set_tss(0, d7::D3DTSS_MIPFILTER, mip);
    }
    case d7::D3DRENDERSTATE_BORDERCOLOR:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_BORDERCOLOR, v);
    case d7::D3DRENDERSTATE_MIPMAPLODBIAS:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_MIPMAPLODBIAS, v);
    case d7::D3DRENDERSTATE_ANISOTROPY:
        lrs[s] = v;
        return set_tss(0, d7::D3DTSS_MAXANISOTROPY, v);
    case d7::D3DRENDERSTATE_WRAPU:
    case d7::D3DRENDERSTATE_WRAPV:
    case d7::D3DRENDERSTATE_WRAP0:
        lrs[s] = v;
        legacy_wrap();
        return D3D_OK;
    case d7::D3DRENDERSTATE_FOGTABLEMODE:
        lrs[s] = v;
        set_rs(s, v);
        legacy_fog();
        return D3D_OK;
    case d7::D3DRENDERSTATE_FOGSTART:
    case d7::D3DRENDERSTATE_FOGEND:
    case d7::D3DRENDERSTATE_FOGDENSITY:
        lrs[s] = v;
        legacy_fog();
        return D3D_OK;
    default:
        lrs[s] = v;
        /* DirectX 7 additions (136 and up) and the states without a
         * Direct3D 9 meaning (MONOENABLE, ROP2, PLANEMASK, stipple, ...)
         * are stored only, like native. */
        if (s < 136 && q7_rs_kind(s) != Q7RS_INVALID)
            set_rs(s, v);
        return D3D_OK;
    }
}

HRESULT Q7Device::legacy_get_rs(DWORD s, DWORD *v)
{
    if (!v)
        return DDERR_INVALIDPARAMS;
    if (!s || s >= Q7_RS)
        return DDERR_INVALIDPARAMS;
    if (s == d7::D3DRENDERSTATE_TEXTUREHANDLE) {
        /* The handle of what stage 0 samples (Wine). */
        Q7Surf *x = st.tex[0] ? q7_surf(st.tex[0]) : NULL;
        *v = x ? x->handle : 0;
        return D3D_OK;
    }
    *v = lrs[s];
    return D3D_OK;
}

HRESULT Q7Device::legacy_set_ls(DWORD s, DWORD v)
{
    DWORD last = version >= 3 ? d7::D3DLIGHTSTATE_COLORVERTEX : d7::D3DLIGHTSTATE_FOGDENSITY;
    if (!s || s > last)
        return DDERR_INVALIDPARAMS;
    switch (s) {
    case d7::D3DLIGHTSTATE_MATERIAL:
        if (v) {
            Q7Material *m = (Q7Material *)q7_handle_get(v, Q7H_MATERIAL);
            if (!m)
                return DDERR_INVALIDPARAMS;
            d7::D3DMATERIAL7 m7;
            q7_material7(&m->mat, &m7);
            set_material(&m7);
        }
        material = v;
        ls[s] = v;
        return D3D_OK;
    case d7::D3DLIGHTSTATE_AMBIENT:
        ls[s] = v;
        return set_rs(d7::D3DRENDERSTATE_AMBIENT, v);
    case d7::D3DLIGHTSTATE_COLORMODEL:
        if (v != D3DCOLOR_MONO && v != D3DCOLOR_RGB)
            return DDERR_INVALIDPARAMS;
        ls[s] = v;
        return D3D_OK;
    case d7::D3DLIGHTSTATE_FOGMODE:
        ls[s] = v;
        set_rs(d7::D3DRENDERSTATE_FOGVERTEXMODE, v);
        legacy_fog();
        return D3D_OK;
    case d7::D3DLIGHTSTATE_FOGSTART:
    case d7::D3DLIGHTSTATE_FOGEND:
    case d7::D3DLIGHTSTATE_FOGDENSITY:
        ls[s] = v;
        legacy_fog();
        return D3D_OK;
    default: /* COLORVERTEX */
        ls[s] = v;
        return set_rs(d7::D3DRENDERSTATE_COLORVERTEX, v);
    }
}

HRESULT Q7Device::legacy_get_ls(DWORD s, DWORD *v)
{
    DWORD last = version >= 3 ? d7::D3DLIGHTSTATE_COLORVERTEX : d7::D3DLIGHTSTATE_FOGDENSITY;
    if (!s || s > last || !v)
        return DDERR_INVALIDPARAMS;
    *v = s == d7::D3DLIGHTSTATE_MATERIAL ? material : ls[s];
    return D3D_OK;
}

HRESULT Q7Device::legacy_set_transform(DWORD state, const D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    if (state == d7::D3DTRANSFORMSTATE_PROJECTION) {
        legacy_proj = *m;
        D3DMATRIX eff;
        q7_mat_mul(&eff, &legacy_proj, &legacy_clip);
        return set_transform(state, &eff);
    }
    return set_transform(state, m);
}

HRESULT Q7Device::legacy_get_transform(DWORD state, D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    if (state == d7::D3DTRANSFORMSTATE_PROJECTION) {
        *m = legacy_proj;
        return D3D_OK;
    }
    return GetTransform((d7::D3DTRANSFORMSTATETYPE)state, (d7::D3DMATRIX *)m);
}

HRESULT Q7Device::legacy_multiply_transform(DWORD state, const D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    if (state == d7::D3DTRANSFORMSTATE_PROJECTION) {
        D3DMATRIX p;
        q7_mat_mul(&p, m, &legacy_proj);
        return legacy_set_transform(state, &p);
    }
    return MultiplyTransform((d7::D3DTRANSFORMSTATETYPE)state, (d7::D3DMATRIX *)m);
}

static bool q7_desc_size_ok(DWORD size)
{
    return size == 172 || size == 204 || size == 252;
}

HRESULT Q7Device::legacy_caps(d7::D3DDEVICEDESC *hal_desc, d7::D3DDEVICEDESC *hel_desc)
{
    if (!hal_desc || !hel_desc || !q7_desc_size_ok(hal_desc->dwSize) || !q7_desc_size_ok(hel_desc->dwSize))
        return DDERR_INVALIDPARAMS;
    d7::D3DDEVICEDESC h, s;
    if (!d3d->legacy_desc(version >= 3 ? 3 : 2, &guid, &h, &s))
        return DDERR_INVALIDPARAMS;
    /* The caller's structure size stays (Wine's DD_STRUCT_COPY_BYSIZE). */
    DWORD hs = hal_desc->dwSize, ss = hel_desc->dwSize;
    memcpy(hal_desc, &h, hs);
    memcpy(hel_desc, &s, ss);
    hal_desc->dwSize = hs;
    hel_desc->dwSize = ss;
    return D3D_OK;
}

HRESULT Q7Device::legacy_set_texture(DWORD stage, QddSurface *s)
{
    if (s && qdd_surface_draw(s) != dd)
        s = NULL;
    /* Stored whatever the caps (GetTexture and TEXTUREHANDLE report it);
     * hardware devices sample DDSCAPS_TEXTURE surfaces only (bind_textures). */
    return set_texture(stage, s);
}

HRESULT Q7Device::legacy_set_tss(DWORD stage, DWORD type, DWORD v)
{
    /* The application's stage 0 state overrides what TEXTUREMAPBLEND set. */
    if (stage == 0 && type < 32) {
        DWORD mask = 1u << type;
        if (type == d7::D3DTSS_ADDRESS)
            mask |= (1u << d7::D3DTSS_ADDRESSU) | (1u << d7::D3DTSS_ADDRESSV);
        tss0_set &= ~mask;
        derived.valid = false;
    }
    return set_tss(stage, type, v);
}

/* SetRenderTarget of IDirect3DDevice3/2 (Wine, stock measurements): a
 * non-3D surface is refused, and so is a palettized one except a P8 surface
 * with a palette on a software device; a z-buffer becomes the target with
 * DDERR_INVALIDPIXELFORMAT; a system-memory surface on a hardware device
 * becomes the target and nothing is drawn. */
HRESULT Q7Device::legacy_set_target(QddSurface *s, IUnknown *iface)
{
    if (!s || qdd_surface_draw(s) != dd || !iface)
        return DDERR_INVALIDPARAMS;
    const DDSURFACEDESC2 *desc = qdd_surface_desc(s);
    PALETTEENTRY pal[256];
    DWORD pf = desc->ddpfPixelFormat.dwFlags;
    if ((pf & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8)) &&
        (hal || !(pf & DDPF_PALETTEINDEXED8) || !qdd_surface_palette(s, pal)))
        return DDERR_INVALIDCAPS;
    if (!(desc->ddsCaps.dwCaps & DDSCAPS_3DDEVICE))
        return DDERR_INVALIDCAPS;
    bool zbuf = (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) != 0;
    bool unusable = zbuf || (hal && !(desc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY));
    if (!unusable && !qdd_surface_d3d9(s, false))
        return DDERR_INVALIDPIXELFORMAT;
    /* IDirect3DDevice2 keeps a z-buffer target without a reference (Wine's
     * ddraw2 test_rt_caps). */
    bool hold = !(zbuf && version == 2);
    if (hold)
        iface->AddRef();
    IUnknown *old = rt_ref;
    rt = s;
    rt_ref = hold ? iface : NULL;
    rt_valid = !unusable;
    if (old)
        old->Release();
    return zbuf ? DDERR_INVALIDPIXELFORMAT : D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Viewports of a device                                                     */

HRESULT Q7Device::add_viewport(Q7Viewport *vp)
{
    if (!vp)
        return DDERR_INVALIDPARAMS;
    vp->AddRef();
    vp->next = viewports;
    viewports = vp;
    /* Usable for Clear right away (Wine). */
    vp->device = this;
    return D3D_OK;
}

HRESULT Q7Device::delete_viewport(Q7Viewport *vp)
{
    if (!vp || vp->device != this)
        return DDERR_INVALIDPARAMS;
    /* The current viewport stops being current; IDirect3DDevice3 drops its
     * SetCurrentViewport reference, IDirect3DDevice2 and older keep it
     * (native, Wine's ddraw2/ddraw4 test_viewport_object). */
    if (cur_vp == vp) {
        vp->deactivate();
        cur_vp = NULL;
        if (version >= 3)
            vp->Release();
    }
    for (Q7Viewport **p = &viewports; *p; p = &(*p)->next)
        if (*p == vp) {
            *p = vp->next;
            break;
        }
    vp->next = NULL;
    vp->device = NULL;
    vp->Release();
    return D3D_OK;
}

HRESULT Q7Device::next_viewport(Q7Viewport *ref, Q7Viewport **out, DWORD flags)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (!ref)
        return DDERR_INVALIDPARAMS;
    switch (flags) {
    case D3DNEXT_NEXT:
        for (Q7Viewport *v = viewports; v; v = v->next)
            if (v == ref) {
                *out = v->next;
                break;
            }
        break;
    case D3DNEXT_HEAD:
        *out = viewports;
        break;
    case D3DNEXT_TAIL:
        for (Q7Viewport *v = viewports; v; v = v->next)
            if (!v->next)
                *out = v;
        break;
    default:
        return DDERR_INVALIDPARAMS;
    }
    return D3D_OK;
}

HRESULT Q7Device::set_current_viewport(Q7Viewport *vp)
{
    if (!vp)
        return DDERR_INVALIDPARAMS;
    if (cur_vp == vp)
        return D3D_OK;
    if (vp->device != this)
        return DDERR_INVALIDPARAMS;
    vp->AddRef();
    if (cur_vp) {
        Q7Viewport *old = cur_vp;
        old->deactivate();
        cur_vp = NULL;
        old->Release();
    }
    cur_vp = vp;
    vp->activate(true);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */

HRESULT Q7Device::legacy_draw(DWORD type, DWORD fvf, const void *vertices, DWORD count, DWORD flags)
{
    if (!legacy_lit(fvf, flags) || !vertices || !count)
        return DrawPrimitive((d7::D3DPRIMITIVETYPE)type, fvf, (void *)vertices, count, flags);
    Q7Source s;
    q7_source_from_fvf(&s, fvf, (const BYTE *)vertices, q7_fvf_size(fvf));
    DWORD lfvf;
    UINT lstride;
    BYTE *lit = legacy_light(&s, count, &lfvf, &lstride);
    if (!lit)
        return DDERR_OUTOFMEMORY;
    return DrawPrimitive((d7::D3DPRIMITIVETYPE)type, lfvf, lit, count, flags);
}

HRESULT Q7Device::legacy_draw_indexed(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, const WORD *indices,
                                      DWORD icount, DWORD flags)
{
    if (!legacy_lit(fvf, flags) || !vertices || !vcount)
        return DrawIndexedPrimitive((d7::D3DPRIMITIVETYPE)type, fvf, (void *)vertices, vcount, (WORD *)indices,
                                    icount, flags);
    Q7Source s;
    q7_source_from_fvf(&s, fvf, (const BYTE *)vertices, q7_fvf_size(fvf));
    DWORD lfvf;
    UINT lstride;
    BYTE *lit = legacy_light(&s, vcount, &lfvf, &lstride);
    if (!lit)
        return DDERR_OUTOFMEMORY;
    return DrawIndexedPrimitive((d7::D3DPRIMITIVETYPE)type, lfvf, lit, vcount, (WORD *)indices, icount, flags);
}

HRESULT Q7Device::begin(DWORD type, DWORD fvf, DWORD flags)
{
    bv_type = type;
    bv_fvf = fvf;
    bv_flags = flags;
    bv_stride = q7_fvf_size(fvf);
    bv_count = 0;
    bv_indexed = false;
    bv_icount = 0;
    return D3D_OK;
}

HRESULT Q7Device::begin_indexed(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, DWORD flags)
{
    begin(type, fvf, flags);
    bv_indexed = true;
    bv_verts = (const BYTE *)vertices;
    bv_vcount = vcount;
    return D3D_OK;
}

HRESULT Q7Device::vertex(const void *v)
{
    if (!v)
        return DDERR_INVALIDPARAMS;
    if (bv_indexed || !bv_stride)
        return D3D_OK;
    if ((bv_count + 1) * bv_stride > bv_cap) {
        UINT cap = bv_cap ? bv_cap * 2 : bv_stride * 64;
        BYTE *n = (BYTE *)realloc(bv_buf, cap);
        if (!n)
            return DDERR_OUTOFMEMORY;
        bv_buf = n;
        bv_cap = cap;
    }
    memcpy(bv_buf + bv_count * bv_stride, v, bv_stride);
    ++bv_count;
    return D3D_OK;
}

HRESULT Q7Device::index(WORD i)
{
    if (!bv_indexed)
        return D3D_OK;
    if (bv_icount == bv_icap) {
        UINT cap = bv_icap ? bv_icap * 2 : 256;
        WORD *n = (WORD *)realloc(bv_idx, cap * sizeof(WORD));
        if (!n)
            return DDERR_OUTOFMEMORY;
        bv_idx = n;
        bv_icap = cap;
    }
    bv_idx[bv_icount++] = i;
    return D3D_OK;
}

HRESULT Q7Device::end(DWORD flags)
{
    HRESULT hr;
    if (bv_indexed)
        hr = legacy_draw_indexed(bv_type, bv_fvf, bv_verts, bv_vcount, bv_idx, bv_icount, bv_flags);
    else
        hr = legacy_draw(bv_type, bv_fvf, bv_buf, bv_count, bv_flags);
    bv_count = 0;
    bv_icount = 0;
    return hr;
}

/* The DirectX 6 form (D3DVIS_*), against the frustum only (no user clip
 * planes before DirectX 7), with IDirect3DDevice3's plane rule. */
HRESULT Q7Device::legacy_sphere_visibility(const D3DVECTOR *centers, const float *radii, DWORD count, DWORD *ret)
{
    if (!centers || !radii || !ret)
        return DDERR_INVALIDPARAMS;
    DWORD planes = st.rs[d7::D3DRENDERSTATE_CLIPPLANEENABLE];
    st.rs[d7::D3DRENDERSTATE_CLIPPLANEENABLE] = 0;
    q7_sphere_visibility(&st, centers, radii, count, ret, true);
    st.rs[d7::D3DRENDERSTATE_CLIPPLANEENABLE] = planes;
    for (DWORD i = 0; i < count; ++i) {
        DWORD r7 = ret[i], r = 0;
        bool intersect = false, outside = false;
        for (DWORD j = 0; j < 6; ++j) {
            DWORD clip = (r7 >> j) & (D3DSTATUS_CLIPUNIONLEFT | D3DSTATUS_CLIPINTERSECTIONLEFT);
            if (clip == D3DSTATUS_CLIPUNIONLEFT) {
                r |= D3DVIS_INTERSECT_LEFT << (j * 2);
                intersect = true;
            } else if (clip) {
                r |= D3DVIS_OUTSIDE_LEFT << (j * 2);
                outside = true;
            }
        }
        if (outside)
            r |= D3DVIS_OUTSIDE_FRUSTUM;
        else if (intersect)
            r |= D3DVIS_INTERSECT_FRUSTUM;
        ret[i] = r;
    }
    return D3D_OK;
}

struct Q7TexFmtCtx {
    d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb;
    void *ctx;
};

static HRESULT CALLBACK q7_texfmt_legacy(DDPIXELFORMAT *pf, void *ctx)
{
    Q7TexFmtCtx *c = (Q7TexFmtCtx *)ctx;
    DDSURFACEDESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    sd.ddpfPixelFormat = *pf;
    return c->cb(&sd, c->ctx);
}

HRESULT Q7Device::legacy_texture_formats(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    Q7TexFmtCtx c = {cb, ctx};
    return texture_formats(2, q7_texfmt_legacy, &c);
}

/* ------------------------------------------------------------------------ */
/* IDirect3DDevice3                                                          */

static d7::IDirect3DTexture2 *q7_tex2_of(QddSurface *s)
{
    Q7Surf *x = s ? q7_surf(s) : NULL;
    if (!x)
        return NULL;
    if (!q7_registry_has(static_cast<d7::IDirect3DTexture2 *>(&x->t2), Q7K_TEX2))
        q7_registry_add(static_cast<d7::IDirect3DTexture2 *>(&x->t2), Q7K_TEX2);
    return &x->t2;
}

HRESULT STDMETHODCALLTYPE Q7Dev3::QueryInterface(REFIID riid, void **obj)
{
    return o->outer_query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Dev3::AddRef()
{
    return o->outer_addref();
}
ULONG STDMETHODCALLTYPE Q7Dev3::Release()
{
    return o->outer_release();
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetCaps(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_caps(hal, hel);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetStats(d7::D3DSTATS *stats)
{
    return E_NOTIMPL; /* stock (measured) */
}
HRESULT STDMETHODCALLTYPE Q7Dev3::AddViewport(d7::IDirect3DViewport3 *vp)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->add_viewport(q7_viewport_lookup(vp));
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DeleteViewport(d7::IDirect3DViewport3 *vp)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->delete_viewport(q7_viewport_lookup(vp));
}
HRESULT STDMETHODCALLTYPE Q7Dev3::NextViewport(d7::IDirect3DViewport3 *ref, d7::IDirect3DViewport3 **vp, DWORD flags)
{
    if (!vp)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    Q7Viewport *n = NULL;
    HRESULT hr = o->next_viewport(q7_viewport_lookup(ref), &n, flags);
    *vp = n;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::EnumTextureFormats(d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    return o->EnumTextureFormats(cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::BeginScene()
{
    return o->BeginScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev3::EndScene()
{
    return o->EndScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetDirect3D(d7::IDirect3D3 **d3d)
{
    if (!d3d)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    *d3d = &o->d3d->if3;
    (*d3d)->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetCurrentViewport(d7::IDirect3DViewport3 *vp)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->set_current_viewport(q7_viewport_lookup(vp));
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetCurrentViewport(d7::IDirect3DViewport3 **vp)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    /* Native: no current viewport fails first, the output untouched (also
     * for a NULL output). */
    if (!o->cur_vp)
        return D3DERR_NOCURRENTVIEWPORT;
    if (!vp)
        return DDERR_INVALIDPARAMS;
    *vp = o->cur_vp;
    o->cur_vp->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetRenderTarget(IDirectDrawSurface4 *surface, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_target(surface ? qdd_surface_lookup(surface) : NULL, (IUnknown *)surface);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetRenderTarget(IDirectDrawSurface4 **surface)
{
    if (!surface)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    if (!o->rt) {
        *surface = NULL;
        return DDERR_GENERIC;
    }
    *surface = (IDirectDrawSurface4 *)qdd_surface_iface(o->rt, 4);
    (*surface)->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::Begin(d7::D3DPRIMITIVETYPE type, DWORD fvf, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->begin(type, fvf, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::BeginIndexed(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count,
                                               DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->begin_indexed(type, fvf, vertices, count, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::Vertex(void *vertex)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->vertex(vertex);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::Index(WORD index)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->index(index);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::End(DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->end(flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetRenderState(d7::D3DRENDERSTATETYPE state, LPDWORD value)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_get_rs(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetRenderState(d7::D3DRENDERSTATETYPE state, DWORD value)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_rs(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetLightState(d7::D3DLIGHTSTATETYPE state, LPDWORD value)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_get_ls(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetLightState(d7::D3DLIGHTSTATETYPE state, DWORD value)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_ls(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_transform(state, (const D3DMATRIX *)matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_get_transform(state, (D3DMATRIX *)matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::MultiplyTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_multiply_transform(state, (const D3DMATRIX *)matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawPrimitive(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count,
                                                DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_draw(type, fvf, vertices, count, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawIndexedPrimitive(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices,
                                                       DWORD vcount, WORD *indices, DWORD icount, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_draw_indexed(type, fvf, vertices, vcount, indices, icount, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetClipStatus(d7::D3DCLIPSTATUS *status)
{
    return o->SetClipStatus(status);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetClipStatus(d7::D3DCLIPSTATUS *status)
{
    return o->GetClipStatus(status);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawPrimitiveStrided(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                                       d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count,
                                                       DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    if (!o->legacy_lit(fvf, flags) || !data || !count)
        return o->DrawPrimitiveStrided(type, fvf, data, count, flags);
    UINT stride;
    BYTE *v = o->assemble_strided(fvf, data, 0, count, &stride);
    if (!v)
        return DDERR_INVALIDPARAMS;
    return o->legacy_draw(type, fvf, v, count, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawIndexedPrimitiveStrided(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                                              d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD vcount,
                                                              WORD *indices, DWORD icount, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    if (!o->legacy_lit(fvf, flags) || !data || !vcount || !indices)
        return o->DrawIndexedPrimitiveStrided(type, fvf, data, vcount, indices, icount, flags);
    UINT stride;
    BYTE *v = o->assemble_strided(fvf, data, 0, vcount, &stride);
    if (!v)
        return DDERR_INVALIDPARAMS;
    return o->legacy_draw_indexed(type, fvf, v, vcount, indices, icount, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawPrimitiveVB(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer *vb,
                                                  DWORD start, DWORD count, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    Q7VB *v = q7_vb_lookup(vb);
    if (!v)
        return DDERR_INVALIDPARAMS;
    if (!o->legacy_lit(v->fvf, flags) || !count || start > v->count || count > v->count - start)
        return o->DrawPrimitiveVB(type, v, start, count, flags);
    const BYTE *base = v->map_read();
    if (!base)
        return DDERR_GENERIC;
    HRESULT hr = o->legacy_draw(type, v->fvf, base + (size_t)start * v->stride, count, flags);
    v->unmap_read();
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::DrawIndexedPrimitiveVB(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer *vb,
                                                         WORD *indices, DWORD icount, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    Q7VB *v = q7_vb_lookup(vb);
    if (!v)
        return DDERR_INVALIDPARAMS;
    /* DirectX 6 has no first-call failure for an empty index list (Wine's
     * ddraw4 test_draw_primitive). */
    if (!icount)
        return D3D_OK;
    if (!o->legacy_lit(v->fvf, flags))
        return o->DrawIndexedPrimitiveVB(type, v, 0, v->count, indices, icount, flags);
    const BYTE *base = v->map_read();
    if (!base)
        return DDERR_GENERIC;
    HRESULT hr = o->legacy_draw_indexed(type, v->fvf, base, v->count, indices, icount, flags);
    v->unmap_read();
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::ComputeSphereVisibility(D3DVECTOR *centers, d7::D3DVALUE *radii, DWORD count,
                                                          DWORD flags, DWORD *ret)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_sphere_visibility(centers, radii, count, ret);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetTexture(DWORD stage, d7::IDirect3DTexture2 **tex)
{
    if (!tex)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    *tex = stage < Q7_STAGES && !(stage == 0 && o->tex0_handle) ? q7_tex2_of(o->st.tex[stage]) : NULL;
    if (*tex)
        (*tex)->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetTexture(DWORD stage, d7::IDirect3DTexture2 *tex)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    HRESULT hr = o->legacy_set_texture(stage, tex ? q7_tex2_surface(tex) : NULL);
    if (SUCCEEDED(hr) && stage == 0)
        o->tex0_handle = false;
    return hr;
}
HRESULT STDMETHODCALLTYPE Q7Dev3::GetTextureStageState(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, LPDWORD value)
{
    return o->GetTextureStageState(stage, type, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::SetTextureStageState(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_tss(stage, type, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev3::ValidateDevice(LPDWORD passes)
{
    return o->ValidateDevice(passes);
}

/* ------------------------------------------------------------------------ */
/* IDirect3DDevice2                                                          */

/* The DirectX 5 vertex types as FVFs; 0 for anything else. */
static DWORD q7_vt_fvf(DWORD vt)
{
    switch (vt) {
    case d7::D3DVT_VERTEX: return D3DFVF_VERTEX;
    case d7::D3DVT_LVERTEX: return D3DFVF_LVERTEX;
    case d7::D3DVT_TLVERTEX: return D3DFVF_TLVERTEX;
    default: return 0;
    }
}

/* SwapTextureHandles: the two surfaces trade handles (Wine). */
static HRESULT q7_swap_handles(Q7Device *dev, QddSurface *s1, QddSurface *s2)
{
    Q7Surf *x1 = s1 ? q7_surf(s1) : NULL, *x2 = s2 ? q7_surf(s2) : NULL;
    if (!x1 || !x2 || !x1->handle || !x2->handle)
        return E_INVALIDARG;
    DWORD h = 0;
    dev->legacy_get_rs(d7::D3DRENDERSTATE_TEXTUREHANDLE, &h);
    DWORD h1 = x1->handle, h2 = x2->handle;
    q7_handle_set(h1, x2);
    q7_handle_set(h2, x1);
    x1->handle = h2;
    x2->handle = h1;
    if (h == h1 || h == h2)
        return dev->legacy_set_rs(d7::D3DRENDERSTATE_TEXTUREHANDLE, h);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Dev2::QueryInterface(REFIID riid, void **obj)
{
    return o->outer_query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Dev2::AddRef()
{
    return o->outer_addref();
}
ULONG STDMETHODCALLTYPE Q7Dev2::Release()
{
    return o->outer_release();
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetCaps(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel)
{
    return o->if3.GetCaps(hal, hel);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SwapTextureHandles(d7::IDirect3DTexture2 *t1, d7::IDirect3DTexture2 *t2)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return q7_swap_handles(o, q7_tex2_surface(t1), q7_tex2_surface(t2));
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetStats(d7::D3DSTATS *stats)
{
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Q7Dev2::AddViewport(d7::IDirect3DViewport2 *vp)
{
    return o->if3.AddViewport((d7::IDirect3DViewport3 *)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::DeleteViewport(d7::IDirect3DViewport2 *vp)
{
    return o->if3.DeleteViewport((d7::IDirect3DViewport3 *)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::NextViewport(d7::IDirect3DViewport2 *ref, d7::IDirect3DViewport2 **vp, DWORD flags)
{
    return o->if3.NextViewport((d7::IDirect3DViewport3 *)ref, (d7::IDirect3DViewport3 **)vp, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::EnumTextureFormats(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_texture_formats(cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::BeginScene()
{
    return o->BeginScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev2::EndScene()
{
    return o->EndScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetDirect3D(d7::IDirect3D2 **d3d)
{
    if (!d3d)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    *d3d = &o->d3d->if2;
    (*d3d)->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetCurrentViewport(d7::IDirect3DViewport2 *vp)
{
    return o->if3.SetCurrentViewport((d7::IDirect3DViewport3 *)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetCurrentViewport(d7::IDirect3DViewport2 **vp)
{
    return o->if3.GetCurrentViewport((d7::IDirect3DViewport3 **)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetRenderTarget(IDirectDrawSurface *surface, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_set_target(surface ? qdd_surface_lookup(surface) : NULL, (IUnknown *)surface);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetRenderTarget(IDirectDrawSurface **surface)
{
    if (!surface)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    if (!o->rt) {
        *surface = NULL;
        return DDERR_GENERIC;
    }
    *surface = (IDirectDrawSurface *)qdd_surface_iface(o->rt, 1);
    (*surface)->AddRef();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev2::Begin(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, DWORD flags)
{
    DWORD fvf = q7_vt_fvf(vt);
    if (!fvf)
        return DDERR_INVALIDPARAMS;
    return o->if3.Begin(type, fvf, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::BeginIndexed(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, void *vertices,
                                               DWORD count, DWORD flags)
{
    DWORD fvf = q7_vt_fvf(vt);
    if (!fvf)
        return DDERR_INVALIDPARAMS;
    return o->if3.BeginIndexed(type, fvf, vertices, count, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::Vertex(void *vertex)
{
    return o->if3.Vertex(vertex);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::Index(WORD index)
{
    return o->if3.Index(index);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::End(DWORD flags)
{
    return o->if3.End(flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetRenderState(d7::D3DRENDERSTATETYPE state, LPDWORD value)
{
    return o->if3.GetRenderState(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetRenderState(d7::D3DRENDERSTATETYPE state, DWORD value)
{
    return o->if3.SetRenderState(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetLightState(d7::D3DLIGHTSTATETYPE state, LPDWORD value)
{
    return o->if3.GetLightState(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetLightState(d7::D3DLIGHTSTATETYPE state, DWORD value)
{
    return o->if3.SetLightState(state, value);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    return o->if3.SetTransform(state, matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    return o->if3.GetTransform(state, matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::MultiplyTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix)
{
    return o->if3.MultiplyTransform(state, matrix);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::DrawPrimitive(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, void *vertices,
                                                DWORD count, DWORD flags)
{
    DWORD fvf = q7_vt_fvf(vt);
    if (!fvf)
        return DDERR_INVALIDPARAMS;
    return o->if3.DrawPrimitive(type, fvf, vertices, count, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::DrawIndexedPrimitive(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt,
                                                       void *vertices, DWORD vcount, WORD *indices, DWORD icount,
                                                       DWORD flags)
{
    DWORD fvf = q7_vt_fvf(vt);
    if (!fvf)
        return DDERR_INVALIDPARAMS;
    return o->if3.DrawIndexedPrimitive(type, fvf, vertices, vcount, indices, icount, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::SetClipStatus(d7::D3DCLIPSTATUS *status)
{
    return o->SetClipStatus(status);
}
HRESULT STDMETHODCALLTYPE Q7Dev2::GetClipStatus(d7::D3DCLIPSTATUS *status)
{
    return o->GetClipStatus(status);
}

/* ------------------------------------------------------------------------ */
/* IDirect3DDevice                                                           */

HRESULT STDMETHODCALLTYPE Q7Dev1::QueryInterface(REFIID riid, void **obj)
{
    return o->outer_query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Dev1::AddRef()
{
    return o->outer_addref();
}
ULONG STDMETHODCALLTYPE Q7Dev1::Release()
{
    return o->outer_release();
}
HRESULT STDMETHODCALLTYPE Q7Dev1::Initialize(d7::IDirect3D *d3d, GUID *guid, d7::D3DDEVICEDESC *desc)
{
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::GetCaps(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel)
{
    return o->if3.GetCaps(hal, hel);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::SwapTextureHandles(d7::IDirect3DTexture *t1, d7::IDirect3DTexture *t2)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return q7_swap_handles(o, q7_tex1_surface(t1), q7_tex1_surface(t2));
}
HRESULT STDMETHODCALLTYPE Q7Dev1::CreateExecuteBuffer(d7::D3DEXECUTEBUFFERDESC *desc,
                                                      d7::IDirect3DExecuteBuffer **buffer, IUnknown *outer)
{
    if (!buffer)
        return DDERR_INVALIDPARAMS;
    *buffer = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!desc)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    Q7ExecBuf *eb = new Q7ExecBuf(o);
    if (!eb)
        return DDERR_OUTOFMEMORY;
    HRESULT hr = eb->Initialize(&o->if1, desc);
    if (FAILED(hr) || !q7_registry_add(static_cast<d7::IDirect3DExecuteBuffer *>(eb), Q7K_EXECBUF)) {
        delete eb;
        return FAILED(hr) ? hr : DDERR_OUTOFMEMORY;
    }
    *buffer = eb;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::GetStats(d7::D3DSTATS *stats)
{
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::Execute(d7::IDirect3DExecuteBuffer *buffer, d7::IDirect3DViewport *vp, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    Q7ExecBuf *eb = q7_execbuf_lookup(buffer);
    if (!eb)
        return DDERR_INVALIDPARAMS;
    Q7Viewport *v = q7_viewport_lookup(vp);
    HRESULT hr = o->set_current_viewport(v);
    if (FAILED(hr))
        return hr;
    return eb->execute(v);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::AddViewport(d7::IDirect3DViewport *vp)
{
    return o->if3.AddViewport((d7::IDirect3DViewport3 *)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::DeleteViewport(d7::IDirect3DViewport *vp)
{
    return o->if3.DeleteViewport((d7::IDirect3DViewport3 *)vp);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::NextViewport(d7::IDirect3DViewport *ref, d7::IDirect3DViewport **vp, DWORD flags)
{
    return o->if3.NextViewport((d7::IDirect3DViewport3 *)ref, (d7::IDirect3DViewport3 **)vp, flags);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::Pick(d7::IDirect3DExecuteBuffer *buffer, d7::IDirect3DViewport *vp, DWORD flags,
                                       d7::D3DRECT *rect)
{
    return D3D_OK; /* picks nothing (no pick records) */
}
HRESULT STDMETHODCALLTYPE Q7Dev1::GetPickRecords(DWORD *count, d7::D3DPICKRECORD *records)
{
    if (count)
        *count = 0;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::EnumTextureFormats(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx)
{
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    return o->legacy_texture_formats(cb, ctx);
}
HRESULT STDMETHODCALLTYPE Q7Dev1::CreateMatrix(d7::D3DMATRIXHANDLE *matrix)
{
    if (!matrix)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    D3DMATRIX *m = (D3DMATRIX *)calloc(1, sizeof(*m));
    if (!m)
        return DDERR_OUTOFMEMORY;
    DWORD h = q7_handle_alloc(Q7H_MATRIX, m);
    if (!h) {
        free(m);
        return DDERR_OUTOFMEMORY;
    }
    *matrix = h;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::SetMatrix(d7::D3DMATRIXHANDLE handle, d7::D3DMATRIX *matrix)
{
    if (!matrix)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    D3DMATRIX *m = (D3DMATRIX *)q7_handle_get(handle, Q7H_MATRIX);
    if (!m)
        return DDERR_INVALIDPARAMS;
    memcpy(m, matrix, sizeof(*m));
    /* A matrix an execute buffer made current follows its changes (Wine). */
    if (handle == o->mx_world)
        o->legacy_set_transform(d7::D3DTRANSFORMSTATE_WORLD, m);
    if (handle == o->mx_view)
        o->legacy_set_transform(d7::D3DTRANSFORMSTATE_VIEW, m);
    if (handle == o->mx_proj)
        o->legacy_set_transform(d7::D3DTRANSFORMSTATE_PROJECTION, m);
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::GetMatrix(d7::D3DMATRIXHANDLE handle, d7::D3DMATRIX *matrix)
{
    if (!matrix)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    D3DMATRIX *m = (D3DMATRIX *)q7_handle_get(handle, Q7H_MATRIX);
    if (!m)
        return DDERR_INVALIDPARAMS;
    memcpy(matrix, m, sizeof(*m));
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::DeleteMatrix(d7::D3DMATRIXHANDLE handle)
{
    Q7Lock lock;
    D3DMATRIX *m = (D3DMATRIX *)q7_handle_get(handle, Q7H_MATRIX);
    if (!m)
        return DDERR_INVALIDPARAMS;
    q7_handle_free(handle);
    free(m);
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Dev1::BeginScene()
{
    return o->BeginScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev1::EndScene()
{
    return o->EndScene();
}
HRESULT STDMETHODCALLTYPE Q7Dev1::GetDirect3D(d7::IDirect3D **d3d)
{
    if (!d3d)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE_DEV(o);
    *d3d = &o->d3d->if1;
    (*d3d)->AddRef();
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* IDirect3DTexture2 / IDirect3DTexture (on surfaces not made by IDirectDraw7) */

IUnknown *Q7Surf::outer()
{
    return (IUnknown *)qdd_surface_iface(s, qdd_surface_create_version(s) == 4 ? 4 : 1);
}

static HRESULT q7_texture_handle(Q7Surf *x, const void *device, d7::D3DTEXTUREHANDLE *handle)
{
    if (!handle)
        return DDERR_INVALIDPARAMS;
    /* Stock refuses a NULL device (measured: DDERR_INVALIDOBJECT). */
    if (!device)
        return DDERR_INVALIDOBJECT;
    if (!x->handle) {
        DWORD h = q7_handle_alloc(Q7H_TEXTURE, x);
        if (!h)
            return DDERR_OUTOFMEMORY;
        x->handle = h;
    }
    *handle = x->handle;
    return D3D_OK;
}

/* IDirect3DTexture2::Load (Wine): palette entries and the source colour key
 * (only when the source has one, unlike IDirect3DDevice7::Load) copied,
 * every level of equal size blitted. */
HRESULT q7_texture_load(QddSurface *dst, QddSurface *src)
{
    if (!src)
        return DDERR_INVALIDPARAMS;
    if (src == dst)
        return D3D_OK;
    UINT levels = qdd_surface_levels(dst), slevels = qdd_surface_levels(src);
    for (UINT i = 0; i < levels && i < slevels; ++i) {
        QddSurface *d = qdd_surface_sub(dst, 0, i), *s = qdd_surface_sub(src, 0, i);
        if (!d || !s)
            break;
        IDirectDrawSurface7 *d7s = qdd_surface_iface7(d), *s7s = qdd_surface_iface7(s);
        IDirectDrawPalette *sp = NULL, *dp = NULL;
        if (SUCCEEDED(s7s->GetPalette(&sp)) && sp) {
            if (FAILED(d7s->GetPalette(&dp)) || !dp) {
                sp->Release();
                return DDERR_NOPALETTEATTACHED;
            }
            PALETTEENTRY pe[256];
            if (SUCCEEDED(sp->GetEntries(0, 0, 256, pe)))
                dp->SetEntries(0, 0, 256, pe);
            dp->Release();
            sp->Release();
        }
        DDCOLORKEY key;
        if (SUCCEEDED(s7s->GetColorKey(DDCKEY_SRCBLT, &key)))
            d7s->SetColorKey(DDCKEY_SRCBLT, &key);
        const DDSURFACEDESC2 *dd = qdd_surface_desc(d), *sd = qdd_surface_desc(s);
        if (dd->dwWidth != sd->dwWidth || dd->dwHeight != sd->dwHeight)
            return D3DERR_TEXTURE_LOAD_FAILED;
        if (FAILED(d7s->Blt(NULL, s7s, NULL, DDBLT_WAIT, NULL)))
            return D3DERR_TEXTURE_LOAD_FAILED;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Tex2If::QueryInterface(REFIID riid, void **obj)
{
    return qdd_surface_iface7(o->s)->QueryInterface(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Tex2If::AddRef()
{
    return o->outer()->AddRef();
}
ULONG STDMETHODCALLTYPE Q7Tex2If::Release()
{
    return o->outer()->Release();
}
HRESULT STDMETHODCALLTYPE Q7Tex2If::GetHandle(d7::IDirect3DDevice2 *device, d7::D3DTEXTUREHANDLE *handle)
{
    Q7Lock lock;
    return q7_texture_handle(o, device, handle);
}
HRESULT STDMETHODCALLTYPE Q7Tex2If::PaletteChanged(DWORD start, DWORD count)
{
    return D3D_OK; /* palette changes reach the texture through the surface version */
}
HRESULT STDMETHODCALLTYPE Q7Tex2If::Load(d7::IDirect3DTexture2 *src)
{
    Q7Lock lock;
    return q7_texture_load(o->s, src ? q7_tex2_surface(src) : NULL);
}

HRESULT STDMETHODCALLTYPE Q7Tex1If::QueryInterface(REFIID riid, void **obj)
{
    return qdd_surface_iface7(o->s)->QueryInterface(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Tex1If::AddRef()
{
    return o->outer()->AddRef();
}
ULONG STDMETHODCALLTYPE Q7Tex1If::Release()
{
    return o->outer()->Release();
}
HRESULT STDMETHODCALLTYPE Q7Tex1If::Initialize(d7::IDirect3DDevice *device, IDirectDrawSurface *surface)
{
    return DDERR_ALREADYINITIALIZED;
}
HRESULT STDMETHODCALLTYPE Q7Tex1If::GetHandle(d7::IDirect3DDevice *device, d7::D3DTEXTUREHANDLE *handle)
{
    Q7Lock lock;
    return q7_texture_handle(o, device, handle);
}
HRESULT STDMETHODCALLTYPE Q7Tex1If::PaletteChanged(DWORD start, DWORD count)
{
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Tex1If::Load(d7::IDirect3DTexture *src)
{
    Q7Lock lock;
    return q7_texture_load(o->s, src ? q7_tex1_surface(src) : NULL);
}
HRESULT STDMETHODCALLTYPE Q7Tex1If::Unload()
{
    return DDERR_UNSUPPORTED;
}

void q7_surf_release_legacy(Q7Surf *x)
{
    if (x->handle) {
        q7_handle_free(x->handle);
        x->handle = 0;
    }
    q7_registry_remove(static_cast<d7::IDirect3DTexture2 *>(&x->t2));
    q7_registry_remove(static_cast<d7::IDirect3DTexture *>(&x->t1));
    /* The surface held its version-1 devices' inner reference. */
    for (UINT i = 0; i < 2; ++i) {
        Q7Device *dev = x->device1[i];
        x->device1[i] = NULL;
        if (dev)
            dev->Release();
    }
}

/* QueryInterface of a surface not made by IDirectDraw7 (qdd.h hook). */
HRESULT qdd_d3d7_surface_query(QddSurface *s, REFIID riid, void **out)
{
    *out = NULL;
    if (IsEqualGUID(riid, Q7IID_IDirect3DTexture2)) {
        d7::IDirect3DTexture2 *t = q7_tex2_of(s);
        if (!t)
            return E_OUTOFMEMORY;
        t->AddRef();
        *out = t;
        return S_OK;
    }
    if (IsEqualGUID(riid, Q7IID_IDirect3DTexture)) {
        Q7Surf *x = q7_surf(s);
        if (!x)
            return E_OUTOFMEMORY;
        if (!q7_registry_has(static_cast<d7::IDirect3DTexture *>(&x->t1), Q7K_TEX1))
            q7_registry_add(static_cast<d7::IDirect3DTexture *>(&x->t1), Q7K_TEX1);
        x->t1.AddRef();
        *out = static_cast<d7::IDirect3DTexture *>(&x->t1);
        return S_OK;
    }
    bool hal = IsEqualGUID(riid, Q7IID_IDirect3DHALDevice);
    if (!hal && !IsEqualGUID(riid, Q7IID_IDirect3DRGBDevice))
        return E_NOINTERFACE;
    /* A version-1 device, aggregated into the surface: one per type. */
    Q7Surf *x = q7_surf(s);
    QddDraw *owner = qdd_surface_draw(s);
    Q7D3D *d = owner ? q7_d3d(owner) : NULL;
    if (!x || !d)
        return E_OUTOFMEMORY;
    Q7Device *&dev = x->device1[hal ? 0 : 1];
    if (!dev) {
        Q7Device *n;
        HRESULT hr = d->create_device(riid, s, NULL, 1, &n);
        if (FAILED(hr))
            return hr;
        n->outer = (IUnknown *)qdd_surface_iface(s, 1);
        n->legacy_init();
        dev = n;
    }
    dev->outer_addref();
    *out = static_cast<d7::IDirect3DDevice *>(&dev->if1);
    return S_OK;
}

namespace {
/* The surface hook is a pointer the core calls when set (qdd.h). */
struct Q7HookInit {
    Q7HookInit() { qdd_d3d_surface_query = qdd_d3d7_surface_query; }
} q7_hook_init;
}
