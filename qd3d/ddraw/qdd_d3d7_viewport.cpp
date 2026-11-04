/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: Direct3D 1-6 viewports, lights and materials.  A viewport made
 * current on a device sets the device's D3D7 viewport and the clip-space
 * mapping legacy projections go through, and turns its active lights into
 * device lights 1-32 (Wine's model); a material handle on
 * D3DLIGHTSTATE_MATERIAL sets the device material.
 */
#include "qdd_d3d7.h"

#define Q7_MAX_VP_LIGHTS 32u

Q7Viewport *q7_viewport_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_VIEWPORT) ? static_cast<Q7Viewport *>((d7::IDirect3DViewport3 *)p) : NULL;
}

Q7LightObj *q7_light_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_LIGHT) ? static_cast<Q7LightObj *>((d7::IDirect3DLight *)p) : NULL;
}

/* ------------------------------------------------------------------------ */
/* IDirect3DViewport3                                                        */

Q7Viewport::Q7Viewport(Q7D3D *d)
    : refs(1), d3d(d), device(NULL), next(NULL), vp_version(0), vp1(), vp2(), background(0), lights(NULL),
      active_mask(0)
{
}

/* The device viewport and clip space (Wine's viewport_activate). */
void Q7Viewport::activate(bool lights_too)
{
    if (!device)
        return;
    if (lights_too)
        for (Q7LightObj *l = lights; l; l = l->next)
            l->activate();
    if (!vp_version)
        return;
    d7::D3DVIEWPORT7 vp;
    D3DMATRIX clip;
    memset(&clip, 0, sizeof(clip));
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;
    if (vp_version == 2) {
        vp.dwX = vp2.dwX;
        vp.dwY = vp2.dwY;
        vp.dwWidth = vp2.dwWidth;
        vp.dwHeight = vp2.dwHeight;
        float zr = vp2.dvMaxZ - vp2.dvMinZ;
        clip._11 = 2.0f / vp2.dvClipWidth;
        clip._22 = 2.0f / vp2.dvClipHeight;
        clip._33 = 1.0f / zr;
        clip._41 = -2.0f * vp2.dvClipX / vp2.dvClipWidth - 1.0f;
        clip._42 = -2.0f * vp2.dvClipY / vp2.dvClipHeight + 1.0f;
        clip._43 = -vp2.dvMinZ / zr;
    } else {
        vp.dwX = vp1.dwX;
        vp.dwY = vp1.dwY;
        vp.dwWidth = vp1.dwWidth;
        vp.dwHeight = vp1.dwHeight;
        clip._11 = 2.0f * vp1.dvScaleX / (float)vp1.dwWidth;
        clip._22 = 2.0f * vp1.dvScaleY / (float)vp1.dwHeight;
        clip._33 = 1.0f;
    }
    clip._44 = 1.0f;
    device->legacy_set_clip(&clip);
    device->set_viewport(&vp);
}

void Q7Viewport::deactivate()
{
    for (Q7LightObj *l = lights; l; l = l->next)
        l->deactivate();
}

/* Device light slots 1-32 for the viewport's active lights; past 32 the
 * oldest active one makes room (Wine). */
void Q7Viewport::alloc_light_slot(Q7LightObj *l)
{
    if (l->slot)
        return;
    if (active_mask == 0xffffffffu) {
        for (Q7LightObj *o = lights; o; o = o->next)
            if (o->slot) {
                o->deactivate();
                break;
            }
    }
    for (DWORD i = 0; i < Q7_MAX_VP_LIGHTS; ++i)
        if (!(active_mask & (1u << i))) {
            active_mask |= 1u << i;
            l->slot = i + 1;
            return;
        }
}

void Q7Viewport::free_light_slot(Q7LightObj *l)
{
    if (!l->slot)
        return;
    active_mask &= ~(1u << (l->slot - 1));
    l->slot = 0;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, Q7IID_IDirect3DViewport3) || IsEqualGUID(riid, Q7IID_IDirect3DViewport2) ||
        IsEqualGUID(riid, Q7IID_IDirect3DViewport) || IsEqualGUID(riid, IID_IUnknown)) {
        AddRef();
        *obj = static_cast<d7::IDirect3DViewport3 *>(this);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q7Viewport::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q7Viewport::Release()
{
    Q7Lock lock;
    LONG r = InterlockedDecrement(&refs);
    if (r > 0)
        return (ULONG)r;
    if (r < 0) {
        InterlockedIncrement(&refs);
        return 0;
    }
    q7_registry_remove(static_cast<d7::IDirect3DViewport3 *>(this));
    while (lights) {
        Q7LightObj *l = lights;
        lights = l->next;
        l->next = NULL;
        l->slot = 0;
        l->viewport = NULL;
        l->Release();
    }
    delete this;
    return 0;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::Initialize(d7::IDirect3D *d)
{
    return DDERR_ALREADYINITIALIZED;
}

/* The stock runtime keeps a D3DVIEWPORT2-style clip volume: SetViewport
 * derives it from the D3DVIEWPORT's scale (dvClipWidth = width / scale,
 * centred; a zero scale gives 0), GetViewport2 reports it.  GetViewport
 * reports the dvMaxX/dvMaxY a D3DVIEWPORT gave (else those of the volume)
 * and a scale of width / clip width, infinite for 0 (measured through
 * Wine's d3d.c ViewportTest and ddraw1/4 test_viewport_object).  A
 * D3DVIEWPORT's own scale maps the picture (activate, as Wine). */
HRESULT STDMETHODCALLTYPE Q7Viewport::GetViewport(d7::D3DVIEWPORT *data)
{
    if (!data || data->dwSize != sizeof(*data))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!vp_version)
        return D3DERR_VIEWPORTDATANOTSET;
    d7::D3DVIEWPORT v;
    memset(&v, 0, sizeof(v));
    v.dwSize = sizeof(v);
    v.dwX = vp2.dwX;
    v.dwY = vp2.dwY;
    v.dwWidth = vp2.dwWidth;
    v.dwHeight = vp2.dwHeight;
    v.dvScaleX = (float)v.dwWidth / vp2.dvClipWidth;
    v.dvScaleY = (float)v.dwHeight / vp2.dvClipHeight;
    if (vp_version == 1) {
        v.dvMaxX = vp1.dvMaxX;
        v.dvMaxY = vp1.dvMaxY;
    } else {
        v.dvMaxX = vp2.dvClipWidth + vp2.dvClipX;
        v.dvMaxY = vp2.dvClipY;
    }
    v.dvMinZ = 0.0f;
    v.dvMaxZ = 1.0f;
    *data = v;
    return D3D_OK;
}

/* The viewport must lie inside the device's render target (versions 2 and
 * 3, Wine). */
static HRESULT q7_vp_check(Q7Device *dev, DWORD x, DWORD y, DWORD w, DWORD h)
{
    if (dev->version <= 1)
        return D3D_OK;
    if (!dev->rt || !dev->rt_valid)
        return DDERR_INVALIDCAPS;
    const DDSURFACEDESC2 *d = qdd_surface_desc(dev->rt);
    if (x > d->dwWidth || w > d->dwWidth - x || y > d->dwHeight || h > d->dwHeight - y)
        return DDERR_INVALIDPARAMS;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::SetViewport(d7::D3DVIEWPORT *data)
{
    if (!data || data->dwSize != sizeof(*data))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!device)
        return D3DERR_VIEWPORTHASNODEVICE;
    HRESULT hr = q7_vp_check(device, data->dwX, data->dwY, data->dwWidth, data->dwHeight);
    if (FAILED(hr))
        return hr;
    vp_version = 1;
    vp1 = *data;
    /* Games show the Z range of a D3DVIEWPORT is ignored (Wine). */
    vp1.dvMinZ = 0.0f;
    vp1.dvMaxZ = 1.0f;
    vp2.dwSize = sizeof(vp2);
    vp2.dwX = data->dwX;
    vp2.dwY = data->dwY;
    vp2.dwWidth = data->dwWidth;
    vp2.dwHeight = data->dwHeight;
    vp2.dvClipWidth = data->dvScaleX != 0.0f ? (float)data->dwWidth / data->dvScaleX : 0.0f;
    vp2.dvClipHeight = data->dvScaleY != 0.0f ? (float)data->dwHeight / data->dvScaleY : 0.0f;
    vp2.dvClipX = -vp2.dvClipWidth / 2.0f;
    vp2.dvClipY = vp2.dvClipHeight / 2.0f;
    vp2.dvMinZ = 0.0f;
    vp2.dvMaxZ = 1.0f;
    if (device->cur_vp == this)
        activate(false);
    return D3D_OK;
}

/* TransformVertices (Wine's model): world-view-projection with the clip
 * space, then the viewport; with D3DTRANSFORM_CLIPPED the homogeneous
 * results and clip flags go to lpHOut and offscreen is the AND of the
 * flags. */
HRESULT STDMETHODCALLTYPE Q7Viewport::TransformVertices(DWORD count, d7::D3DTRANSFORMDATA *data, DWORD flags,
                                                       DWORD *offscreen)
{
    if (!data || data->dwSize != sizeof(*data))
        return DDERR_INVALIDPARAMS;
    if (!(flags & (D3DTRANSFORM_UNCLIPPED | D3DTRANSFORM_CLIPPED)))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!device)
        return D3DERR_VIEWPORTHASNODEVICE;
    Q7Device *dev = device;
    /* The clip space of this viewport, the device's current one after. */
    D3DMATRIX saved_clip = dev->legacy_clip;
    bool other = dev->cur_vp != this;
    if (other)
        activate(false);
    D3DMATRIX wv, m;
    q7_mat_mul(&wv, &dev->st.xf[q7_xf_slot(d7::D3DTRANSFORMSTATE_WORLD)],
               &dev->st.xf[q7_xf_slot(d7::D3DTRANSFORMSTATE_VIEW)]);
    q7_mat_mul(&m, &wv, &dev->st.xf[q7_xf_slot(d7::D3DTRANSFORMSTATE_PROJECTION)]);
    const D3DMATRIX &c = dev->legacy_clip;
    DWORD w = vp_version == 2 ? vp2.dwWidth : vp1.dwWidth, h = vp_version == 2 ? vp2.dwHeight : vp1.dwHeight;
    DWORD x0 = vp_version == 2 ? vp2.dwX : vp1.dwX, y0 = vp_version == 2 ? vp2.dwY : vp1.dwY;
    if (offscreen)
        *offscreen = (flags & D3DTRANSFORM_CLIPPED) ? ~0u : 0u;
    d7::D3DHVERTEX *outh = data->lpHOut;
    for (DWORD i = 0; i < count; ++i) {
        const float *in = (const float *)((const BYTE *)data->lpIn + (size_t)data->dwInSize * i);
        float *out = (float *)((BYTE *)data->lpOut + (size_t)data->dwOutSize * i);
        float x = in[0] * m._11 + in[1] * m._21 + in[2] * m._31 + m._41;
        float y = in[0] * m._12 + in[1] * m._22 + in[2] * m._32 + m._42;
        float z = in[0] * m._13 + in[1] * m._23 + in[2] * m._33 + m._43;
        float wv4 = in[0] * m._14 + in[1] * m._24 + in[2] * m._34 + m._44;
        if (flags & D3DTRANSFORM_CLIPPED) {
            outh[i].hx = (x - c._41 * wv4) / c._11;
            outh[i].hy = (y - c._42 * wv4) / c._22;
            outh[i].hz = (z - c._43 * wv4) / c._33;
            DWORD f = 0;
            if (x > wv4)
                f |= D3DCLIP_RIGHT;
            if (x < -wv4)
                f |= D3DCLIP_LEFT;
            if (y > wv4)
                f |= D3DCLIP_TOP;
            if (y < -wv4)
                f |= D3DCLIP_BOTTOM;
            if (z < 0.0f)
                f |= D3DCLIP_FRONT;
            if (z > wv4)
                f |= D3DCLIP_BACK;
            outh[i].dwFlags = f;
            if (offscreen)
                *offscreen &= f;
            if (f) {
                /* Native leaves the clip-space vertex. */
                out[0] = x;
                out[1] = y;
                out[2] = z;
                out[3] = wv4;
                continue;
            }
        }
        float rhw = 1.0f / wv4;
        x *= rhw;
        y *= rhw;
        z *= rhw;
        out[0] = (x + 1.0f) * (float)w * 0.5f + (float)x0;
        out[1] = (-y + 1.0f) * (float)h * 0.5f + (float)y0;
        out[2] = z;
        out[3] = rhw;
        /* The rest of a D3DTLVERTEX-sized record is copied. */
        if (data->dwOutSize >= 32 && data->dwInSize >= 32)
            memcpy(out + 4, in + 4, 16);
    }
    if (other) {
        if (dev->cur_vp)
            dev->cur_vp->activate(false);
        else
            dev->legacy_set_clip(&saved_clip);
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::LightElements(DWORD count, d7::D3DLIGHTDATA *data)
{
    return DDERR_UNSUPPORTED; /* also native since DirectX 6 */
}

HRESULT STDMETHODCALLTYPE Q7Viewport::SetBackground(d7::D3DMATERIALHANDLE material)
{
    Q7Lock lock;
    if (!q7_handle_get(material, Q7H_MATERIAL))
        return DDERR_INVALIDPARAMS;
    background = material;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::GetBackground(d7::D3DMATERIALHANDLE *material, WINBOOL *valid)
{
    Q7Lock lock;
    bool ok = background && q7_handle_get(background, Q7H_MATERIAL);
    if (valid)
        *valid = ok;
    if (material)
        *material = ok ? background : 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::SetBackgroundDepth(IDirectDrawSurface *surface)
{
    return D3D_OK; /* background depth images are not drawn */
}

HRESULT STDMETHODCALLTYPE Q7Viewport::GetBackgroundDepth(IDirectDrawSurface **surface, WINBOOL *valid)
{
    if (surface)
        *surface = NULL;
    if (valid)
        *valid = FALSE;
    return D3D_OK;
}

/* Clear and Clear2 run with this viewport current, the device's own after
 * (Wine). */
static HRESULT q7_vp_clear(Q7Viewport *vp, DWORD count, d7::D3DRECT *rects, DWORD flags, DWORD color, float z,
                           DWORD stencil)
{
    if (!rects || !count)
        return D3D_OK;
    if (!vp->device)
        return D3DERR_VIEWPORTHASNODEVICE;
    Q7Device *dev = vp->device;
    D3DMATRIX saved_clip = dev->legacy_clip;
    d7::D3DVIEWPORT7 saved_vp = dev->st.vp;
    vp->activate(false);
    HRESULT hr = dev->Clear(count, rects, flags, color, z, stencil);
    if (dev->cur_vp) {
        dev->cur_vp->activate(false);
    } else {
        dev->legacy_set_clip(&saved_clip);
        dev->set_viewport(&saved_vp);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::Clear(DWORD count, d7::D3DRECT *rects, DWORD flags)
{
    Q7Lock lock;
    DWORD color = 0;
    if (flags & D3DCLEAR_TARGET) {
        /* The background material's diffuse colour. */
        Q7Material *m = (Q7Material *)q7_handle_get(background, Q7H_MATERIAL);
        if (m) {
            const d7::D3DCOLORVALUE &c = m->mat.diffuse;
            float rgba[4] = {c.r, c.g, c.b, c.a};
            DWORD v[4];
            for (int i = 0; i < 4; ++i) {
                float f = rgba[i] < 0.0f ? 0.0f : rgba[i] > 1.0f ? 1.0f : rgba[i];
                v[i] = (DWORD)(f * 255.0f + 0.5f);
            }
            color = (v[3] << 24) | (v[0] << 16) | (v[1] << 8) | v[2];
        }
    }
    return q7_vp_clear(this, count, rects, flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), color, 1.0f, 0);
}

HRESULT STDMETHODCALLTYPE Q7Viewport::AddLight(d7::IDirect3DLight *light)
{
    Q7Lock lock;
    Q7LightObj *l = q7_light_lookup(light);
    if (!l)
        return DDERR_INVALIDPARAMS;
    if (l->viewport)
        return D3DERR_LIGHTHASVIEWPORT;
    l->viewport = this;
    l->next = NULL;
    Q7LightObj **p = &lights;
    while (*p)
        p = &(*p)->next;
    *p = l;
    l->AddRef();
    l->activate();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::DeleteLight(d7::IDirect3DLight *light)
{
    Q7Lock lock;
    Q7LightObj *l = q7_light_lookup(light);
    if (!l || l->viewport != this)
        return DDERR_INVALIDPARAMS;
    l->deactivate();
    for (Q7LightObj **p = &lights; *p; p = &(*p)->next)
        if (*p == l) {
            *p = l->next;
            break;
        }
    l->next = NULL;
    l->viewport = NULL;
    l->Release();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::NextLight(d7::IDirect3DLight *ref, d7::IDirect3DLight **light, DWORD flags)
{
    if (!light)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7LightObj *r = q7_light_lookup(ref), *n = NULL;
    switch (flags) {
    case D3DNEXT_NEXT:
        n = r && r->viewport == this ? r->next : NULL;
        break;
    case D3DNEXT_HEAD:
        n = lights;
        break;
    case D3DNEXT_TAIL:
        for (Q7LightObj *l = lights; l; l = l->next)
            n = l;
        break;
    default:
        break;
    }
    *light = n;
    if (!n)
        return DDERR_INVALIDPARAMS;
    n->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::GetViewport2(d7::D3DVIEWPORT2 *data)
{
    if (!data || data->dwSize != sizeof(*data))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!vp_version)
        return D3DERR_VIEWPORTDATANOTSET;
    *data = vp2;
    data->dwSize = sizeof(*data);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::SetViewport2(d7::D3DVIEWPORT2 *data)
{
    if (!data || data->dwSize != sizeof(*data))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!device)
        return D3DERR_VIEWPORTHASNODEVICE;
    HRESULT hr = q7_vp_check(device, data->dwX, data->dwY, data->dwWidth, data->dwHeight);
    if (FAILED(hr))
        return hr;
    vp_version = 2;
    vp2 = *data;
    if (device->cur_vp == this)
        activate(false);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::SetBackgroundDepth2(IDirectDrawSurface4 *surface)
{
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::GetBackgroundDepth2(IDirectDrawSurface4 **surface, WINBOOL *valid)
{
    if (surface)
        *surface = NULL;
    if (valid)
        *valid = FALSE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Viewport::Clear2(DWORD count, d7::D3DRECT *rects, DWORD flags, DWORD color, d7::D3DVALUE z,
                                             DWORD stencil)
{
    Q7Lock lock;
    return q7_vp_clear(this, count, rects, flags, color, z, stencil);
}

/* ------------------------------------------------------------------------ */
/* IDirect3DLight                                                            */

Q7LightObj::Q7LightObj() : refs(1), viewport(NULL), next(NULL), light(), set(false), slot(0)
{
}

/* The D3D7 light of a legacy one (Wine): diffuse and specular from the one
 * colour (no specular with D3DLIGHT_NO_SPECULAR), no ambient; a parallel
 * point light shines like a directional one from its position. */
static void q7_light7(const d7::D3DLIGHT2 *l, d7::D3DLIGHT7 *o)
{
    memset(o, 0, sizeof(*o));
    DWORD flags = l->dwSize >= sizeof(d7::D3DLIGHT2) ? l->dwFlags : D3DLIGHT_ACTIVE;
    o->dltType = l->dltType;
    o->dcvDiffuse = l->dcvColor;
    if (!(flags & D3DLIGHT_NO_SPECULAR))
        o->dcvSpecular = l->dcvColor;
    o->dvPosition = l->dvPosition;
    o->dvDirection = l->dvDirection;
    o->dvRange = l->dvRange;
    o->dvFalloff = l->dvFalloff;
    o->dvAttenuation0 = l->dvAttenuation0;
    o->dvAttenuation1 = l->dvAttenuation1;
    o->dvAttenuation2 = l->dvAttenuation2;
    o->dvTheta = l->dvTheta;
    o->dvPhi = l->dvPhi;
    if (l->dltType == d7::D3DLIGHT_PARALLELPOINT) {
        o->dltType = d7::D3DLIGHT_DIRECTIONAL;
        o->dvDirection.x = -l->dvPosition.x;
        o->dvDirection.y = -l->dvPosition.y;
        o->dvDirection.z = -l->dvPosition.z;
    }
}

void Q7LightObj::update()
{
    if (!slot || !viewport || !viewport->device)
        return;
    d7::D3DLIGHT7 l7;
    q7_light7(&light, &l7);
    viewport->device->set_light(slot, &l7);
}

/* Active while its viewport is the current one of a device. */
void Q7LightObj::activate()
{
    if (!viewport || !viewport->device || viewport->device->cur_vp != viewport || !set)
        return;
    DWORD flags = light.dwSize >= sizeof(d7::D3DLIGHT2) ? light.dwFlags : D3DLIGHT_ACTIVE;
    if (!(flags & D3DLIGHT_ACTIVE))
        return;
    viewport->alloc_light_slot(this);
    if (!slot)
        return;
    update();
    viewport->device->light_enable(slot, TRUE);
}

void Q7LightObj::deactivate()
{
    if (!slot || !viewport)
        return;
    if (viewport->device)
        viewport->device->light_enable(slot, FALSE);
    viewport->free_light_slot(this);
}

HRESULT STDMETHODCALLTYPE Q7LightObj::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, Q7IID_IDirect3DLight) || IsEqualGUID(riid, IID_IUnknown)) {
        AddRef();
        *obj = static_cast<d7::IDirect3DLight *>(this);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q7LightObj::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q7LightObj::Release()
{
    Q7Lock lock;
    LONG r = InterlockedDecrement(&refs);
    if (r > 0)
        return (ULONG)r;
    if (r < 0) {
        InterlockedIncrement(&refs);
        return 0;
    }
    q7_registry_remove(static_cast<d7::IDirect3DLight *>(this));
    delete this;
    return 0;
}

HRESULT STDMETHODCALLTYPE Q7LightObj::Initialize(d7::IDirect3D *d)
{
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7LightObj::SetLight(d7::D3DLIGHT *data)
{
    if (!data)
        return DDERR_INVALIDPARAMS;
    if (!data->dltType || data->dltType > d7::D3DLIGHT_PARALLELPOINT)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    DWORD flags = data->dwSize >= sizeof(d7::D3DLIGHT2) ? ((d7::D3DLIGHT2 *)data)->dwFlags : D3DLIGHT_ACTIVE;
    if (!(flags & D3DLIGHT_ACTIVE))
        deactivate();
    memset(&light, 0, sizeof(light));
    memcpy(&light, data, data->dwSize < sizeof(light) ? data->dwSize : sizeof(light));
    light.dwFlags = flags;
    set = true;
    if (slot)
        update();
    else
        activate();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7LightObj::GetLight(d7::D3DLIGHT *data)
{
    if (!data)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    DWORD size = data->dwSize < sizeof(light) ? data->dwSize : sizeof(light);
    memcpy(data, &light, size);
    data->dwSize = size;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* IDirect3DMaterial3/2/1                                                    */

void q7_material7(const d7::D3DMATERIAL *m, d7::D3DMATERIAL7 *out)
{
    out->diffuse = m->diffuse;
    out->ambient = m->ambient;
    out->specular = m->specular;
    out->emissive = m->emissive;
    out->power = m->power;
}

Q7Material::Q7Material() : refs(1), mat(), handles(NULL), nhandles(0), if3(this), if2(this), if1(this)
{
    mat.dwSize = sizeof(mat);
}

Q7Material::~Q7Material()
{
    for (UINT i = 0; i < nhandles; ++i)
        q7_handle_free(handles[i].handle);
    free(handles);
    q7_registry_remove(static_cast<d7::IDirect3DMaterial3 *>(&if3));
    q7_registry_remove(static_cast<d7::IDirect3DMaterial2 *>(&if2));
    q7_registry_remove(static_cast<d7::IDirect3DMaterial *>(&if1));
}

ULONG Q7Material::release()
{
    Q7Lock lock;
    LONG r = InterlockedDecrement(&refs);
    if (r > 0)
        return (ULONG)r;
    if (r < 0) {
        InterlockedIncrement(&refs);
        return 0;
    }
    delete this;
    return 0;
}

HRESULT Q7Material::query(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    *obj = NULL;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, Q7IID_IDirect3DMaterial3))
        *obj = static_cast<d7::IDirect3DMaterial3 *>(&if3);
    else if (IsEqualGUID(riid, Q7IID_IDirect3DMaterial2))
        *obj = static_cast<d7::IDirect3DMaterial2 *>(&if2);
    else if (IsEqualGUID(riid, Q7IID_IDirect3DMaterial))
        *obj = static_cast<d7::IDirect3DMaterial *>(&if1);
    else
        return E_NOINTERFACE;
    add_ref();
    return S_OK;
}

HRESULT Q7Material::set(const d7::D3DMATERIAL *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    DWORD size = m->dwSize < sizeof(mat) ? m->dwSize : sizeof(mat);
    memset(&mat, 0, sizeof(mat));
    memcpy(&mat, m, size);
    mat.dwSize = sizeof(mat);
    return D3D_OK;
}

HRESULT Q7Material::get(d7::D3DMATERIAL *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    DWORD size = m->dwSize < sizeof(mat) ? m->dwSize : sizeof(mat);
    memcpy(m, &mat, size);
    m->dwSize = size;
    return D3D_OK;
}

HRESULT Q7Material::get_handle(const void *device, d7::D3DMATERIALHANDLE *h)
{
    if (!h)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    for (UINT i = 0; i < nhandles; ++i)
        if (handles[i].device == device) {
            *h = handles[i].handle;
            return D3D_OK;
        }
    Handle *n = (Handle *)realloc(handles, (nhandles + 1) * sizeof(*n));
    if (!n)
        return DDERR_OUTOFMEMORY;
    handles = n;
    DWORD hv = q7_handle_alloc(Q7H_MATERIAL, this);
    if (!hv)
        return DDERR_OUTOFMEMORY;
    handles[nhandles].device = device;
    handles[nhandles].handle = hv;
    ++nhandles;
    *h = hv;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Mat3::QueryInterface(REFIID riid, void **obj)
{
    return o->query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Mat3::AddRef()
{
    return o->add_ref();
}
ULONG STDMETHODCALLTYPE Q7Mat3::Release()
{
    return o->release();
}
HRESULT STDMETHODCALLTYPE Q7Mat3::SetMaterial(d7::D3DMATERIAL *data)
{
    return o->set(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat3::GetMaterial(d7::D3DMATERIAL *data)
{
    return o->get(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat3::GetHandle(d7::IDirect3DDevice3 *device, d7::D3DMATERIALHANDLE *handle)
{
    return o->get_handle(q7_device3_lookup(device), handle);
}

HRESULT STDMETHODCALLTYPE Q7Mat2::QueryInterface(REFIID riid, void **obj)
{
    return o->query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Mat2::AddRef()
{
    return o->add_ref();
}
ULONG STDMETHODCALLTYPE Q7Mat2::Release()
{
    return o->release();
}
HRESULT STDMETHODCALLTYPE Q7Mat2::SetMaterial(d7::D3DMATERIAL *data)
{
    return o->set(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat2::GetMaterial(d7::D3DMATERIAL *data)
{
    return o->get(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat2::GetHandle(d7::IDirect3DDevice2 *device, d7::D3DMATERIALHANDLE *handle)
{
    return o->get_handle(q7_device2_lookup(device), handle);
}

HRESULT STDMETHODCALLTYPE Q7Mat1::QueryInterface(REFIID riid, void **obj)
{
    return o->query(riid, obj);
}
ULONG STDMETHODCALLTYPE Q7Mat1::AddRef()
{
    return o->add_ref();
}
ULONG STDMETHODCALLTYPE Q7Mat1::Release()
{
    return o->release();
}
HRESULT STDMETHODCALLTYPE Q7Mat1::Initialize(d7::IDirect3D *d3d)
{
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Q7Mat1::SetMaterial(d7::D3DMATERIAL *data)
{
    return o->set(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat1::GetMaterial(d7::D3DMATERIAL *data)
{
    return o->get(data);
}
HRESULT STDMETHODCALLTYPE Q7Mat1::GetHandle(d7::IDirect3DDevice *device, d7::D3DMATERIALHANDLE *handle)
{
    return o->get_handle(q7_device1_lookup(device), handle);
}
HRESULT STDMETHODCALLTYPE Q7Mat1::Reserve()
{
    return DDERR_UNSUPPORTED;
}
HRESULT STDMETHODCALLTYPE Q7Mat1::Unreserve()
{
    return DDERR_UNSUPPORTED;
}
