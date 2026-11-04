/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: the hooks the DirectDraw core calls (qdd.h), IDirect3D7, and
 * the device descriptions derived from the QD3D device's Direct3D 9 caps.
 */
#include "qdd_d3d7.h"

/* DirectX 7 SDK d3d.h values. */
const GUID Q7IID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID Q7IID_IDirect3DDevice7 = {0xf5049e79, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID Q7IID_IDirect3DVertexBuffer7 = {0xf5049e7d, 0x4861, 0x11d2,
                                           {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID Q7IID_IDirect3DRGBDevice = {0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
const GUID Q7IID_IDirect3DHALDevice = {0x84e63de0, 0x46aa, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
const GUID Q7IID_IDirect3DTnLHalDevice = {0xf5049e78, 0x4861, 0x11d2,
                                          {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID Q7IID_IDirect3DMMXDevice = {0x881949a1, 0xd6f3, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DRefDevice = {0x50936643, 0x13e9, 0x11d1, {0x89, 0xaa, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DNullDevice = {0x8767df22, 0xbacc, 0x11d1, {0x89, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID Q7IID_IDirect3DRampDevice = {0xf2086b20, 0x259f, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
const GUID Q7IID_IDirect3DDevice = {0x64108800, 0x957d, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DDevice2 = {0x93281501, 0x8cf8, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DDevice3 = {0xb0ab3b60, 0x33d7, 0x11d1, {0xa9, 0x81, 0x00, 0xc0, 0x4f, 0xd7, 0xb1, 0x74}};
const GUID Q7IID_IDirect3DTexture = {0x2cdcd9e0, 0x25a0, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
const GUID Q7IID_IDirect3DTexture2 = {0x93281502, 0x8cf8, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DLight = {0x4417c142, 0x33ad, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
const GUID Q7IID_IDirect3DMaterial = {0x4417c144, 0x33ad, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
const GUID Q7IID_IDirect3DMaterial2 = {0x93281503, 0x8cf8, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DMaterial3 = {0xca9c46f4, 0xd3c5, 0x11d1, {0xb7, 0x5a, 0x00, 0x60, 0x08, 0x52, 0xb3, 0x12}};
const GUID Q7IID_IDirect3DExecuteBuffer = {0x4417c145, 0x33ad, 0x11cf,
                                           {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
const GUID Q7IID_IDirect3DViewport = {0x4417c146, 0x33ad, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
const GUID Q7IID_IDirect3DViewport2 = {0x93281500, 0x8cf8, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};
const GUID Q7IID_IDirect3DViewport3 = {0xb0ab3b61, 0x33d7, 0x11d1, {0xa9, 0x81, 0x00, 0xc0, 0x4f, 0xd7, 0xb1, 0x74}};
const GUID Q7IID_IDirect3DVertexBuffer = {0x7a503555, 0x4a83, 0x11d1,
                                          {0xa5, 0xdb, 0x00, 0xa0, 0xc9, 0x03, 0x67, 0xf8}};

/* ------------------------------------------------------------------------ */
/* Hooks (qdd.h)                                                             */

Q7D3D *q7_d3d(QddDraw *dd)
{
    void **slot = qdd_draw_d3d7_slot(dd);
    if (!*slot)
        *slot = new Q7D3D(dd);
    return (Q7D3D *)*slot;
}

/* The core answers IID_IDirect3D7 only on DirectDrawCreateEx objects and
 * IID_IDirect3D/2/3 only on the others (qdd.h), like native. */
HRESULT qdd_d3d7_query(QddDraw *dd, REFIID riid, void **out)
{
    *out = NULL;
    void *iface;
    Q7D3D *d;
    if (IsEqualGUID(riid, Q7IID_IDirect3D7)) {
        if (!(d = q7_d3d(dd)))
            return E_OUTOFMEMORY;
        iface = static_cast<d7::IDirect3D7 *>(d);
    } else if (IsEqualGUID(riid, QDD_IID_IDirect3D3)) {
        if (!(d = q7_d3d(dd)))
            return E_OUTOFMEMORY;
        iface = static_cast<d7::IDirect3D3 *>(&d->if3);
    } else if (IsEqualGUID(riid, QDD_IID_IDirect3D2)) {
        if (!(d = q7_d3d(dd)))
            return E_OUTOFMEMORY;
        iface = static_cast<d7::IDirect3D2 *>(&d->if2);
    } else if (IsEqualGUID(riid, QDD_IID_IDirect3D)) {
        if (!(d = q7_d3d(dd)))
            return E_OUTOFMEMORY;
        iface = static_cast<d7::IDirect3D *>(&d->if1);
    } else {
        return E_NOINTERFACE;
    }
    ((IUnknown *)iface)->AddRef();
    *out = iface;
    return S_OK;
}

void qdd_d3d7_draw_destroy(QddDraw *dd)
{
    void **slot = qdd_draw_d3d7_slot(dd);
    Q7D3D *d = (Q7D3D *)*slot;
    if (!d)
        return;
    *slot = NULL;
    delete d;
}

void qdd_d3d7_surface_destroy(QddSurface *s)
{
    void **slot = qdd_surface_d3d7_slot(s);
    Q7Surf *x = (Q7Surf *)*slot;
    Q7Tex *t = x ? x->tex : NULL;
    /* A surface that outlived its DirectDraw object has no owner left. */
    QddDraw *owner = qdd_surface_draw(s);
    Q7D3D *d = owner ? (Q7D3D *)*qdd_draw_d3d7_slot(owner) : NULL;

    if (d) {
        for (Q7Device *dev = d->devices; dev; dev = dev->next)
            dev->forget_surface(s);
        if (t) {
            /* A D3D9 texture about to go may still be the cached binding. */
            for (UINT i = 0; i < Q7_STAGES; ++i)
                for (UINT v = 0; v < 2; ++v)
                    if (t->var[v].tex9 && d->bound_tex9[i] == t->var[v].tex9)
                        d->bound_tex9[i] = NULL;
        }
    }
    if (x)
        q7_surf_release_legacy(x);
    *slot = NULL;
    if (t)
        q7_tex_free(t);
    delete x;
}

void qdd_d3d7_flush(QddDraw *dd)
{
    Q7D3D *d = (Q7D3D *)*qdd_draw_d3d7_slot(dd);
    if (d)
        d->end_scene();
}

void qdd_d3d7_before_reset(QddDraw *dd)
{
    qdd_d3d7_flush(dd);
}

void qdd_d3d7_after_reset(QddDraw *dd)
{
    Q7D3D *d = (Q7D3D *)*qdd_draw_d3d7_slot(dd);
    if (d)
        d->reset_bindings();
}

/* ------------------------------------------------------------------------ */
/* Q7D3D                                                                     */

Q7D3D::Q7D3D(QddDraw *d)
    : dd(d), devices(NULL), active(NULL), in_scene9(false), bound_rt9(NULL), bound_ds9(NULL), bound_tex9(),
      bound_decl9(NULL), decl_fvf(NULL), decl9(NULL), ndecl(0), capdecl(0), ib9(NULL), ib_size(0), ib_pos(0),
      ib_bound(false), caps_ok(false), caps9(), display_format(D3DFMT_UNKNOWN), d9_lights(0), vbs(NULL), if3(this),
      if2(this), if1(this)
{
}

Q7D3D::~Q7D3D()
{
    /* Normally nothing outlives the DirectDraw object (devices hold their
     * render target, buffers the object); an application that released
     * the object too often leaves them detached. */
    while (devices) {
        Q7Device *dev = devices;
        devices = dev->next;
        dev->detach();
    }
    while (vbs) {
        Q7VB *vb = vbs;
        vbs = vb->next;
        vb->detach();
    }
    for (UINT i = 0; i < ndecl; ++i)
        decl9[i]->Release();
    free(decl9);
    free(decl_fvf);
    if (ib9)
        ib9->Release();
}

void Q7D3D::reset_bindings()
{
    active = NULL;
    in_scene9 = false;
    bound_rt9 = bound_ds9 = NULL;
    for (UINT i = 0; i < Q7_STAGES; ++i)
        bound_tex9[i] = NULL;
    bound_decl9 = NULL;
    ib_bound = false;
    d9_lights = 0;
}

void Q7D3D::end_scene()
{
    if (!in_scene9)
        return;
    in_scene9 = false;
    IDirect3DDevice9 *d9 = device9();
    if (d9)
        d9->EndScene();
}

bool Q7D3D::get_caps()
{
    if (caps_ok)
        return true;
    IDirect3D9 *d3d9 = qdd_draw_d3d9(dd);
    if (!d3d9 || FAILED(d3d9->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps9)))
        return false;
    D3DDISPLAYMODE mode;
    if (FAILED(d3d9->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode)))
        mode.Format = D3DFMT_X8R8G8B8;
    display_format = mode.Format;
    caps_ok = true;
    return true;
}

IDirect3DVertexDeclaration9 *Q7D3D::decl_for(DWORD fvf)
{
    for (UINT i = 0; i < ndecl; ++i)
        if (decl_fvf[i] == fvf)
            return decl9[i];
    Q7FvfLayout l;
    IDirect3DDevice9 *d9 = device9();
    if (!d9 || !q7_fvf_layout(fvf, &l))
        return NULL;
    D3DVERTEXELEMENT9 el[16];
    UINT n = 0;
    el[n].Stream = 0;
    el[n].Offset = 0;
    el[n].Type = l.posfloats == 4 ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT3;
    el[n].Method = D3DDECLMETHOD_DEFAULT;
    el[n].Usage = l.posfloats == 4 ? D3DDECLUSAGE_POSITIONT : D3DDECLUSAGE_POSITION;
    el[n].UsageIndex = 0;
    ++n;
    if (l.nbeta) {
        /* Direct3D 7 blends with up to three weights; the last of five
         * betas has no meaning there. */
        static const BYTE wt[4] = {D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4};
        int nb = l.nbeta > 4 ? 4 : l.nbeta;
        el[n].Stream = 0;
        el[n].Offset = 12;
        el[n].Type = wt[nb - 1];
        el[n].Method = D3DDECLMETHOD_DEFAULT;
        el[n].Usage = D3DDECLUSAGE_BLENDWEIGHT;
        el[n].UsageIndex = 0;
        ++n;
    }
    if (l.normal >= 0) {
        el[n].Stream = 0;
        el[n].Offset = (WORD)l.normal;
        el[n].Type = D3DDECLTYPE_FLOAT3;
        el[n].Method = D3DDECLMETHOD_DEFAULT;
        el[n].Usage = D3DDECLUSAGE_NORMAL;
        el[n].UsageIndex = 0;
        ++n;
    }
    /* D3DFVF_RESERVED1 (D3DLVERTEX's dwReserved) is skipped. */
    if (l.diffuse >= 0) {
        el[n].Stream = 0;
        el[n].Offset = (WORD)l.diffuse;
        el[n].Type = D3DDECLTYPE_D3DCOLOR;
        el[n].Method = D3DDECLMETHOD_DEFAULT;
        el[n].Usage = D3DDECLUSAGE_COLOR;
        el[n].UsageIndex = 0;
        ++n;
    }
    if (l.specular >= 0) {
        el[n].Stream = 0;
        el[n].Offset = (WORD)l.specular;
        el[n].Type = D3DDECLTYPE_D3DCOLOR;
        el[n].Method = D3DDECLMETHOD_DEFAULT;
        el[n].Usage = D3DDECLUSAGE_COLOR;
        el[n].UsageIndex = 1;
        ++n;
    }
    for (UINT i = 0; i < l.ntex; ++i) {
        static const BYTE tt[5] = {0, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3,
                                   D3DDECLTYPE_FLOAT4};
        el[n].Stream = 0;
        el[n].Offset = (WORD)l.tex[i];
        el[n].Type = tt[l.texdim[i]];
        el[n].Method = D3DDECLMETHOD_DEFAULT;
        el[n].Usage = D3DDECLUSAGE_TEXCOORD;
        el[n].UsageIndex = (BYTE)i;
        ++n;
    }
    static const D3DVERTEXELEMENT9 end = D3DDECL_END();
    el[n] = end;
    IDirect3DVertexDeclaration9 *decl = NULL;
    if (FAILED(d9->CreateVertexDeclaration(el, &decl)) || !decl)
        return NULL;
    if (ndecl == capdecl) {
        UINT cap = capdecl ? capdecl * 2 : 16;
        DWORD *nf = (DWORD *)realloc(decl_fvf, cap * sizeof(*nf));
        if (nf)
            decl_fvf = nf;
        IDirect3DVertexDeclaration9 **nd = (IDirect3DVertexDeclaration9 **)realloc(decl9, cap * sizeof(*nd));
        if (nd)
            decl9 = nd;
        if (!nf || !nd) {
            decl->Release();
            return NULL;
        }
        capdecl = cap;
    }
    decl_fvf[ndecl] = fvf;
    decl9[ndecl] = decl;
    ++ndecl;
    return decl;
}

/* The software rasterizer's description, as the stock runtime reports the
 * RGB device (measured). */
static void q7_rgb_desc(d7::D3DDEVICEDESC7 *d)
{
    memset(d, 0, sizeof(*d));
    d->dwDevCaps = 0x00000551;
    d7::D3DPRIMCAPS p;
    memset(&p, 0, sizeof(p));
    p.dwSize = sizeof(p);
    p.dwMiscCaps = 0x00000070;
    p.dwRasterCaps = 0x002121f7;
    p.dwZCmpCaps = 0xff;
    p.dwSrcBlendCaps = 0x00001fff;
    p.dwDestBlendCaps = 0x000007ff;
    p.dwAlphaCmpCaps = 0xff;
    p.dwShadeCaps = 0x000c528a;
    p.dwTextureCaps = 0x000000df;
    p.dwTextureFilterCaps = 0x0303033f;
    p.dwTextureBlendCaps = 0x000000cf;
    p.dwTextureAddressCaps = 0x0000001f;
    p.dwStippleWidth = 4;
    p.dwStippleHeight = 4;
    d->dpcLineCaps = p;
    d->dpcTriCaps = p;
    d->dwDeviceRenderBitDepth = DDBD_8 | DDBD_16 | DDBD_24 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16;
    d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = d->dwMaxTextureHeight = 1024;
    d->dwMaxTextureRepeat = 256;
    d->dwMaxTextureAspectRatio = 0;
    d->dwMaxAnisotropy = 1;
    d->dvGuardBandLeft = -4096.0f;
    d->dvGuardBandTop = -4096.0f;
    d->dvGuardBandRight = 4095.0f;
    d->dvGuardBandBottom = 4095.0f;
    d->dvExtentsAdjust = 0.0f;
    d->dwStencilCaps = 0xff;
    d->dwFVFCaps = 8;
    d->dwTextureOpCaps = 0x000078ff;
    d->wMaxTextureBlendStages = 8;
    d->wMaxSimultaneousTextures = 8;
    d->dwMaxActiveLights = 0xffffffffu;
    d->dvMaxVertexW = 10000.0f;
    d->deviceGUID = Q7IID_IDirect3DRGBDevice;
    d->wMaxUserClipPlanes = 6;
    d->wMaxVertexBlendMatrices = 4;
    d->dwVertexProcessingCaps = 0x3f;
}

/*
 * HAL and T&L HAL descriptions from the Direct3D 9 caps, mapped the way the
 * stock runtime maps its driver's D3D9 caps (compared field by field on
 * this host: D3D9 HAL caps against the stock IDirect3D7 descriptions).
 */
HRESULT Q7D3D::device_desc(const GUID *guid, d7::D3DDEVICEDESC7 *d)
{
    if (IsEqualGUID(*guid, Q7IID_IDirect3DRGBDevice)) {
        q7_rgb_desc(d);
        return D3D_OK;
    }
    if (!get_caps())
        return DDERR_GENERIC;
    const D3DCAPS9 *c = &caps9;
    bool tnl = IsEqualGUID(*guid, Q7IID_IDirect3DTnLHalDevice);

    memset(d, 0, sizeof(*d));
    /* 0x02000000 is undocumented; the stock runtime sets it on both
     * hardware devices (measured: D3D9 0x001bbef1 -> D3D7 0x021bbef1). */
    d->dwDevCaps = (c->DevCaps & 0x001bffffu) | D3DDEVCAPS_FLOATTLVERTEX | 0x02000000u;
    if (!tnl)
        d->dwDevCaps &= ~D3DDEVCAPS_HWTRANSFORMANDLIGHT;

    d7::D3DPRIMCAPS p;
    memset(&p, 0, sizeof(p));
    p.dwSize = sizeof(p);
    p.dwMiscCaps = c->PrimitiveMiscCaps & 0x27fu;
    p.dwRasterCaps = (c->RasterCaps & 0x003fffffu) | D3DPRASTERCAPS_SUBPIXEL;
    if (c->RasterCaps & D3DPRASTERCAPS_DEPTHBIAS)
        p.dwRasterCaps |= D3DPRASTERCAPS_ZBIAS;
    if (c->LineCaps & D3DLINECAPS_ANTIALIAS)
        p.dwRasterCaps |= D3DPRASTERCAPS_ANTIALIASEDGES;
    if (c->RasterCaps & D3DPRASTERCAPS_MULTISAMPLE_TOGGLE)
        p.dwRasterCaps |= D3DPRASTERCAPS_ANTIALIASSORTINDEPENDENT;
    p.dwZCmpCaps = c->ZCmpCaps & 0xff;
    p.dwSrcBlendCaps = c->SrcBlendCaps & 0x1fff;
    p.dwDestBlendCaps = c->DestBlendCaps & 0x1fff;
    p.dwAlphaCmpCaps = c->AlphaCmpCaps & 0xff;
    p.dwShadeCaps = c->ShadeCaps & 0x001fffffu;
    if (c->ShadeCaps & D3DPSHADECAPS_COLORGOURAUDRGB)
        p.dwShadeCaps |= D3DPSHADECAPS_COLORFLATMONO | D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDMONO;
    if (c->ShadeCaps & D3DPSHADECAPS_SPECULARGOURAUDRGB)
        p.dwShadeCaps |= D3DPSHADECAPS_SPECULARFLATRGB;
    if (c->ShadeCaps & D3DPSHADECAPS_ALPHAGOURAUDBLEND)
        p.dwShadeCaps |= D3DPSHADECAPS_ALPHAFLATBLEND;
    if (c->ShadeCaps & D3DPSHADECAPS_FOGGOURAUD)
        p.dwShadeCaps |= D3DPSHADECAPS_FOGFLAT;
    /* Colour keys are emulated (D3DRENDERSTATE_COLORKEYENABLE), so
     * D3DPTEXTURECAPS_TRANSPARENCY holds whatever the hardware does. */
    p.dwTextureCaps = (c->TextureCaps & 0x0fffu) | D3DPTEXTURECAPS_TRANSPARENCY;
    p.dwTextureFilterCaps = c->TextureFilterCaps & 0x07030700u;
    if (c->TextureFilterCaps & D3DPTFILTERCAPS_MINFPOINT)
        p.dwTextureFilterCaps |= D3DPTFILTERCAPS_NEAREST;
    if (c->TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR)
        p.dwTextureFilterCaps |= D3DPTFILTERCAPS_LINEAR;
    if (c->TextureFilterCaps & D3DPTFILTERCAPS_MIPFPOINT)
        p.dwTextureFilterCaps |= D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_MIPLINEAR;
    if (c->TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR)
        p.dwTextureFilterCaps |= D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR;
    p.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                           D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    p.dwTextureAddressCaps = c->TextureAddressCaps & 0x1f;
    d->dpcLineCaps = p;
    d->dpcTriCaps = p;

    d->dwDeviceRenderBitDepth = DDBD_8 | DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24;
    d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = c->MaxTextureWidth;
    d->dwMaxTextureHeight = c->MaxTextureHeight;
    d->dwMaxTextureRepeat = c->MaxTextureRepeat;
    d->dwMaxTextureAspectRatio = c->MaxTextureAspectRatio;
    d->dwMaxAnisotropy = c->MaxAnisotropy;
    d->dvGuardBandLeft = c->GuardBandLeft;
    d->dvGuardBandTop = c->GuardBandTop;
    d->dvGuardBandRight = c->GuardBandRight;
    d->dvGuardBandBottom = c->GuardBandBottom;
    d->dvExtentsAdjust = c->ExtentsAdjust;
    d->dwStencilCaps = c->StencilCaps;
    d->dwFVFCaps = c->FVFCaps;
    d->dwTextureOpCaps = c->TextureOpCaps;
    d->wMaxTextureBlendStages = (WORD)(c->MaxTextureBlendStages > Q7_STAGES ? Q7_STAGES : c->MaxTextureBlendStages);
    d->wMaxSimultaneousTextures =
        (WORD)(c->MaxSimultaneousTextures > Q7_STAGES ? Q7_STAGES : c->MaxSimultaneousTextures);
    d->dvMaxVertexW = c->MaxVertexW;
    d->deviceGUID = *guid;
    d->wMaxVertexBlendMatrices = (WORD)(c->MaxVertexBlendMatrices > 0xffff ? 0xffff : c->MaxVertexBlendMatrices);
    d->dwVertexProcessingCaps = c->VertexProcessingCaps & 0x3bu;
    if (c->RasterCaps & D3DPRASTERCAPS_FOGVERTEX)
        d->dwVertexProcessingCaps |= D3DVTXPCAPS_VERTEXFOG;
    if (tnl) {
        d->dwMaxActiveLights = c->MaxActiveLights;
        d->wMaxUserClipPlanes = (WORD)(c->MaxUserClipPlanes > Q7_CLIPPLANES ? Q7_CLIPPLANES : c->MaxUserClipPlanes);
    } else {
        /* Software transform and lighting: no light limit, six planes. */
        d->dwMaxActiveLights = 0xffffffffu;
        d->wMaxUserClipPlanes = 6;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7D3D::QueryInterface(REFIID riid, void **obj)
{
    return qdd_draw_iface7(dd)->QueryInterface(riid, obj);
}

ULONG STDMETHODCALLTYPE Q7D3D::AddRef()
{
    return qdd_draw_iface7(dd)->AddRef();
}

ULONG STDMETHODCALLTYPE Q7D3D::Release()
{
    /* May free this object (qdd_d3d7_draw_destroy). */
    return qdd_draw_iface7(dd)->Release();
}

/* Writable and stable across calls, as native (Wine's d3d.c tests:
 * applications rewrite the strings, some keep the pointers). */
static char q7_dev_name[3][40] = {"RGB Emulation", "Direct3D HAL", "Direct3D T&L HAL"};
static char q7_dev_desc[3][96] = {
    "Microsoft Direct3D RGB Software Emulation",
    "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
    "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device",
};

HRESULT STDMETHODCALLTYPE Q7D3D::EnumDevices(d7::LPD3DENUMDEVICESCALLBACK7 cb, void *ctx)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    static const GUID *const guids[3] = {&Q7IID_IDirect3DRGBDevice, &Q7IID_IDirect3DHALDevice,
                                         &Q7IID_IDirect3DTnLHalDevice};
    d7::D3DDEVICEDESC7 desc[3];
    {
        Q7Lock lock;
        for (UINT i = 0; i < 3; ++i) {
            HRESULT hr = device_desc(guids[i], &desc[i]);
            if (FAILED(hr))
                return hr;
        }
    }
    for (UINT i = 0; i < 3; ++i)
        if (cb(q7_dev_desc[i], q7_dev_name[i], &desc[i], ctx) != DDENUMRET_OK)
            break;
    return D3D_OK;
}

static void q7_fpu_setup(void)
{
    unsigned short cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0x0f3fu) | 0x003fu);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

/* CreateDevice of every interface version: version 7 (IDirect3D7), 3
 * (IDirect3D3, rt_iface the IDirectDrawSurface4), 2 (IDirect3D2,
 * IDirectDrawSurface) and 1 (surface QueryInterface: no rt_iface, the
 * device is aggregated into the surface).  The device holds rt_iface
 * (native: GetRenderTarget after CreateDevice reports the application's
 * count plus two). */
HRESULT Q7D3D::create_device(REFCLSID rclsid, QddSurface *s, IUnknown *rt_iface, UINT version, Q7Device **out)
{
    *out = NULL;
    if (!s || qdd_surface_draw(s) != dd)
        return DDERR_INVALIDPARAMS;
    bool hal;
    if (IsEqualGUID(rclsid, Q7IID_IDirect3DHALDevice) || IsEqualGUID(rclsid, Q7IID_IDirect3DTnLHalDevice))
        hal = true;
    else if (IsEqualGUID(rclsid, Q7IID_IDirect3DRGBDevice) ||
             (version < 7 && (IsEqualGUID(rclsid, Q7IID_IDirect3DMMXDevice) ||
                              IsEqualGUID(rclsid, Q7IID_IDirect3DRampDevice) ||
                              IsEqualGUID(rclsid, Q7IID_IDirect3DRefDevice))))
        hal = false;
    else
        return DDERR_INVALIDPARAMS;

    const DDSURFACEDESC2 *desc = qdd_surface_desc(s);
    if (!(desc->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) || (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER))
        return DDERR_INVALIDCAPS;
    PALETTEENTRY pal[256];
    bool indexed = (desc->ddpfPixelFormat.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 |
                                                     DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8)) != 0;
    if (indexed && !qdd_surface_palette(s, pal))
        return DDERR_NOPALETTEATTACHED;
    if (hal && !(desc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY))
        return D3DERR_SURFACENOTINVIDMEM;
    /* Palettized targets: the Direct3D 1-6 software devices render into P8
     * surfaces that have a palette (stock; Wine's ddraw1/2/4 test_rt_caps),
     * through the core's X8R8G8B8 copy read back to the nearest palette
     * entries.  Otherwise native refuses them, palette or not (video memory
     * DDERR_INVALIDPIXELFORMAT, system memory E_FAIL on the Direct3D 7
     * software device). */
    bool p8_ok = version < 7 && !hal && (desc->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8);
    if (indexed && !p8_ok)
        return (desc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) ? DDERR_INVALIDPIXELFORMAT : E_FAIL;
    if (!qdd_draw_device9(dd))
        return E_OUTOFMEMORY;
    if (!qdd_surface_d3d9(s, false))
        return DDERR_INVALIDPIXELFORMAT;

    Q7Device *dev = new Q7Device(this, &rclsid, s, version);
    if (!dev)
        return E_OUTOFMEMORY;
    if (!q7_registry_add(static_cast<d7::IDirect3DDevice7 *>(dev), Q7K_DEVICE)) {
        dev->rt = NULL;
        delete dev;
        return E_OUTOFMEMORY;
    }
    if (rt_iface)
        rt_iface->AddRef();
    dev->rt_ref = rt_iface;
    dev->next = devices;
    devices = dev;
    qdd_draw_3d_devices(dd, +1);
    if (!(qdd_draw_coop(dd) & DDSCL_FPUPRESERVE))
        q7_fpu_setup();
    *out = dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7D3D::CreateDevice(REFCLSID rclsid, IDirectDrawSurface7 *surface,
                                              d7::IDirect3DDevice7 **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    *out = NULL;
    Q7Lock lock;
    QddSurface *s = qdd_surface_lookup(surface);
    Q7Device *dev;
    HRESULT hr = create_device(rclsid, s, s ? qdd_surface_iface7(s) : NULL, 7, &dev);
    if (SUCCEEDED(hr))
        *out = dev;
    return hr;
}

/* CreateVertexBuffer of IDirect3D7 (version 7) and IDirect3D3 (3). */
HRESULT Q7D3D::create_vb(d7::D3DVERTEXBUFFERDESC *desc, UINT version, Q7VB **out)
{
    *out = NULL;
    /* Native (measured): 0 vertices -> DDERR_INVALIDPARAMS, more than
     * D3DMAXNUMVERTICES -> D3DERR_TOOMANYVERTICES, any FVF accepted (an
     * invalid one makes a buffer nothing draws from). */
    if (!desc->dwNumVertices)
        return DDERR_INVALIDPARAMS;
    if (desc->dwNumVertices > D3DMAXNUMVERTICES)
        return D3DERR_TOOMANYVERTICES;
    if (!qdd_draw_device9(dd))
        return E_OUTOFMEMORY;
    Q7VB *vb = new Q7VB(this, desc->dwCaps, desc->dwFVF, desc->dwNumVertices, version);
    if (!vb)
        return DDERR_OUTOFMEMORY;
    if (!vb->vb9 || !q7_registry_add(static_cast<d7::IDirect3DVertexBuffer7 *>(vb), Q7K_VB)) {
        bool oom = !vb->vb9;
        vb->detach();
        delete vb;
        return oom ? D3DERR_VBUF_CREATE_FAILED : DDERR_OUTOFMEMORY;
    }
    vb->next = vbs;
    vbs = vb;
    if (version == 7)
        qdd_draw_iface7(dd)->AddRef(); /* a DirectX 7 buffer holds its DirectDraw object (native) */
    *out = vb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7D3D::CreateVertexBuffer(d7::D3DVERTEXBUFFERDESC *desc, d7::IDirect3DVertexBuffer7 **out,
                                                    DWORD flags)
{
    if (!desc || !out)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7VB *vb;
    HRESULT hr = create_vb(desc, 7, &vb);
    /* Native leaves NULL behind on failure (Wine's test_map_synchronisation). */
    *out = SUCCEEDED(hr) ? vb : NULL;
    return hr;
}

static HRESULT q7_enum_z(IDirect3D9 *d3d9, D3DFORMAT display, D3DFORMAT f, const DDPIXELFORMAT *pf,
                         d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx, bool *stop)
{
    if (FAILED(d3d9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, display, D3DUSAGE_DEPTHSTENCIL,
                                       D3DRTYPE_SURFACE, f)))
        return D3D_OK;
    DDPIXELFORMAT copy = *pf;
    if (cb(&copy, ctx) != DDENUMRET_OK)
        *stop = true;
    return D3D_OK;
}

static DDPIXELFORMAT q7_zpf(DWORD bits, DWORD zmask, DWORD sbits, DWORD smask)
{
    DDPIXELFORMAT pf;
    memset(&pf, 0, sizeof(pf));
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_ZBUFFER | (sbits ? DDPF_STENCILBUFFER : 0);
    pf.dwZBufferBitDepth = bits;
    pf.dwStencilBitDepth = sbits;
    pf.dwZBitMask = zmask;
    pf.dwStencilBitMask = smask;
    return pf;
}

/* The stock runtime's lists (measured): HAL devices D16, D24X8 (z in the
 * high 24 bits), D24S8, the same two with the layout reversed and a 24-bit
 * D24X8; the RGB device D16 and D24S8.  Formats the device lacks are left
 * out; D15S1 and D32 follow when the device has them. */
HRESULT STDMETHODCALLTYPE Q7D3D::EnumZBufferFormats(REFCLSID device_iid, d7::LPD3DENUMPIXELFORMATSCALLBACK cb,
                                                    void *ctx)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    IDirect3D9 *d3d9;
    D3DFORMAT display;
    {
        Q7Lock lock;
        if (!get_caps())
            return DDERR_GENERIC;
        d3d9 = qdd_draw_d3d9(dd);
        display = display_format;
    }
    bool rgb = IsEqualGUID(device_iid, Q7IID_IDirect3DRGBDevice) || IsEqualGUID(device_iid, Q7IID_IDirect3DMMXDevice);
    bool stop = false;
    const DDPIXELFORMAT d16 = q7_zpf(16, 0xffff, 0, 0);
    const DDPIXELFORMAT d24x8 = q7_zpf(32, 0xffffff00u, 0, 0);
    const DDPIXELFORMAT d24s8 = q7_zpf(32, 0xffffff00u, 8, 0xff);
    const DDPIXELFORMAT s8d24 = q7_zpf(32, 0x00ffffffu, 8, 0xff000000u);
    const DDPIXELFORMAT x8d24 = q7_zpf(32, 0x00ffffffu, 0, 0);
    const DDPIXELFORMAT d24 = q7_zpf(24, 0x00ffffffu, 0, 0);
    const DDPIXELFORMAT d15s1 = q7_zpf(16, 0x7fff, 1, 0x8000);
    const DDPIXELFORMAT d32 = q7_zpf(32, 0xffffffffu, 0, 0);

    q7_enum_z(d3d9, display, D3DFMT_D16, &d16, cb, ctx, &stop);
    if (rgb) {
        if (!stop)
            q7_enum_z(d3d9, display, D3DFMT_D24S8, &d24s8, cb, ctx, &stop);
        return D3D_OK;
    }
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D24X8, &d24x8, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D24S8, &d24s8, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D24S8, &s8d24, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D24X8, &x8d24, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D24X8, &d24, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D15S1, &d15s1, cb, ctx, &stop);
    if (!stop)
        q7_enum_z(d3d9, display, D3DFMT_D32, &d32, cb, ctx, &stop);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7D3D::EvictManagedTextures()
{
    Q7Lock lock;
    IDirect3DDevice9 *d9 = qdd_draw_device9(dd);
    if (d9)
        d9->EvictManagedResources();
    return D3D_OK;
}
