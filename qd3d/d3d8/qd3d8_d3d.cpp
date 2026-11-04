/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: IDirect3D8 on IDirect3D9, caps and present-parameter conversion.
 */
#include "qd3d8.h"

/* D3D8 bit sets (DirectX 8.1 d3d8caps.h) used to drop D3D9-only caps bits. */
#define Q8_CAPS2_MASK8 (0x00000002u | 0x00020000u | 0x00080000u | 0x00100000u | 0x02000000u | \
                        0x10000000u | 0x20000000u)
/* Plus 0x400, which the stock runtime passes through from current drivers. */
#define Q8_CAPS3_MASK8 (0x8000001fu | 0x00000020u | 0x00000400u)
#define Q8_PMISC_MASK8 0x00001ff6u
#define Q8_VTXPCAPS_MASK8 0x000000fbu
#define Q8_PRASTER_ANTIALIASEDGES 0x00001000u
#define Q8_PRASTER_STRETCHBLTMULTISAMPLE 0x00800000u
#define Q8_PRASTER_ZBIAS 0x00004000u
#define Q8_PRASTER_MASK8 (0x00000001u | 0x00000002u | 0x00000008u | 0x00000010u | 0x00000080u | \
                          0x00000100u | 0x00001000u | 0x00002000u | 0x00004000u | 0x00008000u | \
                          0x00010000u | 0x00020000u | 0x00040000u | 0x00100000u | 0x00200000u | \
                          0x00400000u | 0x00800000u)
#define Q8_DEVCAPS_RTPATCHES 0x00400000u
#define Q8_DEVCAPS_QUINTICRTPATCHES 0x00200000u
#define Q8_DEVCAPS_RTPATCHHANDLEZERO 0x00800000u
#define Q8_PTEXTURECAPS_MASK8 0x0007ffffu

void q8_convert_caps(const D3DCAPS9 *in, d8::D3DCAPS8 *out)
{
    memset(out, 0, sizeof(*out));
    out->DeviceType = (d8::D3DDEVTYPE)in->DeviceType;
    out->AdapterOrdinal = in->AdapterOrdinal;
    out->Caps = in->Caps;
    /* D3D9 dropped D3DCAPS2_CANRENDERWINDOWED (0x80000) because every D3D9
     * device renders windowed; D3D8 games test for it. */
    out->Caps2 = (in->Caps2 & Q8_CAPS2_MASK8) | 0x00080000u;
    out->Caps3 = in->Caps3 & Q8_CAPS3_MASK8;
    out->PresentationIntervals = in->PresentationIntervals;
    out->CursorCaps = in->CursorCaps;
    /* Rect/tri patches with tessellator declarations are not translated. */
    out->DevCaps = in->DevCaps & ~(Q8_DEVCAPS_RTPATCHES | Q8_DEVCAPS_QUINTICRTPATCHES | Q8_DEVCAPS_RTPATCHHANDLEZERO);
    out->PrimitiveMiscCaps = in->PrimitiveMiscCaps & Q8_PMISC_MASK8;
    /* The stock runtime reports antialiased edges and stretch-blit
     * multisampling on every current device (measured); D3D9 has no such
     * bits. */
    DWORD raster = (in->RasterCaps & Q8_PRASTER_MASK8) | Q8_PRASTER_ANTIALIASEDGES | Q8_PRASTER_STRETCHBLTMULTISAMPLE;
    if (in->RasterCaps & D3DPRASTERCAPS_DEPTHBIAS)
        raster |= Q8_PRASTER_ZBIAS;
    out->RasterCaps = raster;
    out->ZCmpCaps = in->ZCmpCaps;
    /* D3DPBLENDCAPS_BLENDFACTOR / INVSRCCOLOR2 / SRCCOLOR2 are D3D9-only. */
    out->SrcBlendCaps = in->SrcBlendCaps & 0x1fffu;
    out->DestBlendCaps = in->DestBlendCaps & 0x1fffu;
    out->AlphaCmpCaps = in->AlphaCmpCaps;
    out->ShadeCaps = in->ShadeCaps;
    out->TextureCaps = in->TextureCaps & Q8_PTEXTURECAPS_MASK8;
    /* D3D8 filter caps share D3D9's MIN/MIP/MAG point, linear and anisotropic
     * bits; bits 0x08000000/0x10000000 mean cubic in D3D8 but quad in D3D9. */
    out->TextureFilterCaps = in->TextureFilterCaps & 0x07030700u;
    out->CubeTextureFilterCaps = in->CubeTextureFilterCaps & 0x07030700u;
    out->VolumeTextureFilterCaps = in->VolumeTextureFilterCaps & 0x07030700u;
    out->TextureAddressCaps = in->TextureAddressCaps & 0x3fu;
    out->VolumeTextureAddressCaps = in->VolumeTextureAddressCaps & 0x3fu;
    out->LineCaps = in->LineCaps & 0x1fu; /* D3DLINECAPS_ANTIALIAS is D3D9-only */
    out->MaxTextureWidth = in->MaxTextureWidth;
    out->MaxTextureHeight = in->MaxTextureHeight;
    out->MaxVolumeExtent = in->MaxVolumeExtent;
    out->MaxTextureRepeat = in->MaxTextureRepeat;
    out->MaxTextureAspectRatio = in->MaxTextureAspectRatio;
    out->MaxAnisotropy = in->MaxAnisotropy;
    out->MaxVertexW = in->MaxVertexW;
    out->GuardBandLeft = in->GuardBandLeft;
    out->GuardBandTop = in->GuardBandTop;
    out->GuardBandRight = in->GuardBandRight;
    out->GuardBandBottom = in->GuardBandBottom;
    out->ExtentsAdjust = in->ExtentsAdjust;
    out->StencilCaps = in->StencilCaps & 0xffu; /* drop D3DSTENCILCAPS_TWOSIDED */
    out->FVFCaps = in->FVFCaps;
    out->TextureOpCaps = in->TextureOpCaps;
    out->MaxTextureBlendStages = in->MaxTextureBlendStages > QD3D8_MAX_STAGES ? QD3D8_MAX_STAGES : in->MaxTextureBlendStages;
    out->MaxSimultaneousTextures = in->MaxSimultaneousTextures > QD3D8_MAX_STAGES ? QD3D8_MAX_STAGES : in->MaxSimultaneousTextures;
    out->VertexProcessingCaps = in->VertexProcessingCaps & Q8_VTXPCAPS_MASK8;
    out->MaxActiveLights = in->MaxActiveLights;
    out->MaxUserClipPlanes = in->MaxUserClipPlanes;
    out->MaxVertexBlendMatrices = in->MaxVertexBlendMatrices;
    out->MaxVertexBlendMatrixIndex = in->MaxVertexBlendMatrixIndex;
    out->MaxPointSize = in->MaxPointSize;
    out->MaxPrimitiveCount = in->MaxPrimitiveCount;
    out->MaxVertexIndex = in->MaxVertexIndex;
    out->MaxStreams = in->MaxStreams > 16 ? 16 : in->MaxStreams;
    out->MaxStreamStride = in->MaxStreamStride;
    out->VertexShaderVersion = in->VertexShaderVersion > D3DVS_VERSION(1, 1) ? D3DVS_VERSION(1, 1) : in->VertexShaderVersion;
    out->MaxVertexShaderConst = in->MaxVertexShaderConst;
    out->PixelShaderVersion = in->PixelShaderVersion > D3DPS_VERSION(1, 4) ? D3DPS_VERSION(1, 4) : in->PixelShaderVersion;
    out->MaxPixelShaderValue = in->PixelShader1xMaxValue;
}

bool q8_convert_pp(const d8::D3DPRESENT_PARAMETERS *in, D3DPRESENT_PARAMETERS *out)
{
    if (!in)
        return false;
    memset(out, 0, sizeof(*out));
    out->BackBufferWidth = in->BackBufferWidth;
    out->BackBufferHeight = in->BackBufferHeight;
    out->BackBufferFormat = (D3DFORMAT)in->BackBufferFormat;
    out->BackBufferCount = in->BackBufferCount;
    out->MultiSampleType = (D3DMULTISAMPLE_TYPE)in->MultiSampleType;
    out->MultiSampleQuality = 0;
    out->hDeviceWindow = in->hDeviceWindow;
    out->Windowed = in->Windowed;
    out->EnableAutoDepthStencil = in->EnableAutoDepthStencil;
    out->AutoDepthStencilFormat = (D3DFORMAT)in->AutoDepthStencilFormat;
    out->Flags = in->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    out->FullScreen_RefreshRateInHz = in->FullScreen_RefreshRateInHz;

    switch (in->SwapEffect) {
    case d8::D3DSWAPEFFECT_DISCARD:
        out->SwapEffect = D3DSWAPEFFECT_DISCARD;
        break;
    case d8::D3DSWAPEFFECT_FLIP:
        out->SwapEffect = D3DSWAPEFFECT_FLIP;
        break;
    case d8::D3DSWAPEFFECT_COPY:
    case d8::D3DSWAPEFFECT_COPY_VSYNC:
        out->SwapEffect = D3DSWAPEFFECT_COPY;
        break;
    default:
        return false;
    }

    if (in->Windowed) {
        /* D3D8 windowed presents are unsynchronised unless COPY_VSYNC; D3D9's
         * windowed DEFAULT interval would wait for vblank. */
        out->PresentationInterval = in->SwapEffect == d8::D3DSWAPEFFECT_COPY_VSYNC
                                        ? D3DPRESENT_INTERVAL_ONE
                                        : D3DPRESENT_INTERVAL_IMMEDIATE;
    } else {
        /* D3D8 FullScreen_PresentationInterval uses D3D9's encoding. */
        out->PresentationInterval = in->FullScreen_PresentationInterval;
    }
    return true;
}

void q8_writeback_pp(const D3DPRESENT_PARAMETERS *in, d8::D3DPRESENT_PARAMETERS *out)
{
    /* D3D9 fills in defaulted fields (zero sizes, UNKNOWN format, zero count). */
    out->BackBufferWidth = in->BackBufferWidth;
    out->BackBufferHeight = in->BackBufferHeight;
    out->BackBufferFormat = (d8::D3DFORMAT)in->BackBufferFormat;
    out->BackBufferCount = in->BackBufferCount;
}

/* ------------------------------------------------------------------------ */

/* Display formats the stock D3D8 runtime enumerates, in its order: its mode
 * list is D3D9's list for each of these, concatenated (measured: identical
 * lists and counts in the same process; whether 16-bit modes exist at all is
 * the system's per-process choice, shared by D3D8 and D3D9). */
static const D3DFORMAT q8_mode_formats[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5};

Q8Direct3D::Q8Direct3D(IDirect3D9 *d) : refs(1), d3d9(d) {}

HRESULT STDMETHODCALLTYPE Q8Direct3D::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return E_POINTER;
    if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, QIID_IDirect3D8)) {
        AddRef();
        *obj = this;
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q8Direct3D::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q8Direct3D::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) {
        d3d9->Release();
        delete this;
    }
    return (ULONG)r;
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::RegisterSoftwareDevice(void *init)
{
    return d3d9->RegisterSoftwareDevice(init);
}

UINT STDMETHODCALLTYPE Q8Direct3D::GetAdapterCount()
{
    return d3d9->GetAdapterCount();
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::GetAdapterIdentifier(UINT adapter, DWORD flags, d8::D3DADAPTER_IDENTIFIER8 *id)
{
    if (!id)
        return D3DERR_INVALIDCALL;
    D3DADAPTER_IDENTIFIER9 id9;
    /* D3D8 D3DENUM_NO_WHQL_LEVEL (2) inverts D3D9 D3DENUM_WHQL_LEVEL (2). */
    DWORD flags9 = (flags & D3DENUM_NO_WHQL_LEVEL) ? 0 : D3DENUM_WHQL_LEVEL;
    HRESULT hr = d3d9->GetAdapterIdentifier(adapter, flags9, &id9);
    if (FAILED(hr))
        return hr;
    memcpy(id->Driver, id9.Driver, sizeof(id->Driver));
    memcpy(id->Description, id9.Description, sizeof(id->Description));
    id->DriverVersion = id9.DriverVersion;
    id->VendorId = id9.VendorId;
    id->DeviceId = id9.DeviceId;
    id->SubSysId = id9.SubSysId;
    id->Revision = id9.Revision;
    id->DeviceIdentifier = id9.DeviceIdentifier;
    id->WHQLLevel = id9.WHQLLevel;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE Q8Direct3D::GetAdapterModeCount(UINT adapter)
{
    UINT total = 0;
    for (UINT i = 0; i < sizeof(q8_mode_formats) / sizeof(q8_mode_formats[0]); ++i)
        total += d3d9->GetAdapterModeCount(adapter, q8_mode_formats[i]);
    return total;
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::EnumAdapterModes(UINT adapter, UINT mode, d8::D3DDISPLAYMODE *out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < sizeof(q8_mode_formats) / sizeof(q8_mode_formats[0]); ++i) {
        UINT n = d3d9->GetAdapterModeCount(adapter, q8_mode_formats[i]);
        if (mode < n)
            return d3d9->EnumAdapterModes(adapter, q8_mode_formats[i], mode, (D3DDISPLAYMODE *)out);
        mode -= n;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::GetAdapterDisplayMode(UINT adapter, d8::D3DDISPLAYMODE *out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    return d3d9->GetAdapterDisplayMode(adapter, (D3DDISPLAYMODE *)out);
}

/* The alpha twin a D3D8 back buffer may have for a display format. */
static D3DFORMAT q8_alpha_twin(D3DFORMAT f)
{
    return f == D3DFMT_X8R8G8B8 ? D3DFMT_A8R8G8B8 : f == D3DFMT_X1R5G5B5 ? D3DFMT_A1R5G5B5 : f;
}

/* Native D3D8 (measured), windowed or not: the back buffer has the display
 * format or its alpha twin, D3DFMT_UNKNOWN is invalid, and only D3D8
 * display formats exist (D3D9 adds A2R10G10B10 and windowed format
 * conversion). */
HRESULT STDMETHODCALLTYPE Q8Direct3D::CheckDeviceType(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT display,
                                                      d8::D3DFORMAT bb, WINBOOL windowed)
{
    D3DFORMAT d = (D3DFORMAT)display, f = (D3DFORMAT)bb;
    if (d == D3DFMT_UNKNOWN || f == D3DFMT_UNKNOWN || adapter >= d3d9->GetAdapterCount())
        return D3DERR_INVALIDCALL;
    if ((d != D3DFMT_X8R8G8B8 && d != D3DFMT_R5G6B5 && d != D3DFMT_X1R5G5B5) || (f != d && f != q8_alpha_twin(d)))
        return D3DERR_NOTAVAILABLE;
    return d3d9->CheckDeviceType(adapter, (D3DDEVTYPE)type, d, f, windowed);
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::CheckDeviceFormat(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT adapter_fmt,
                                                        DWORD usage, d8::D3DRESOURCETYPE rtype, d8::D3DFORMAT fmt)
{
    return d3d9->CheckDeviceFormat(adapter, (D3DDEVTYPE)type, (D3DFORMAT)adapter_fmt, usage, (D3DRESOURCETYPE)rtype,
                                   (D3DFORMAT)fmt);
}

/* D3D8 has no D3DMULTISAMPLE_NONMASKABLE (D3D9's type 1). */
HRESULT STDMETHODCALLTYPE Q8Direct3D::CheckDeviceMultiSampleType(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT fmt,
                                                                 WINBOOL windowed, d8::D3DMULTISAMPLE_TYPE ms)
{
    if ((DWORD)ms == 1)
        return D3DERR_INVALIDCALL;
    return d3d9->CheckDeviceMultiSampleType(adapter, (D3DDEVTYPE)type, (D3DFORMAT)fmt, windowed,
                                            (D3DMULTISAMPLE_TYPE)ms, NULL);
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::CheckDepthStencilMatch(UINT adapter, d8::D3DDEVTYPE type,
                                                             d8::D3DFORMAT adapter_fmt, d8::D3DFORMAT rt,
                                                             d8::D3DFORMAT ds)
{
    return d3d9->CheckDepthStencilMatch(adapter, (D3DDEVTYPE)type, (D3DFORMAT)adapter_fmt, (D3DFORMAT)rt,
                                        (D3DFORMAT)ds);
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::GetDeviceCaps(UINT adapter, d8::D3DDEVTYPE type, d8::D3DCAPS8 *caps)
{
    if (!caps)
        return D3DERR_INVALIDCALL;
    D3DCAPS9 c9;
    HRESULT hr = d3d9->GetDeviceCaps(adapter, (D3DDEVTYPE)type, &c9);
    if (FAILED(hr))
        return hr;
    q8_convert_caps(&c9, caps);
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE Q8Direct3D::GetAdapterMonitor(UINT adapter)
{
    return d3d9->GetAdapterMonitor(adapter);
}

HRESULT STDMETHODCALLTYPE Q8Direct3D::CreateDevice(UINT adapter, d8::D3DDEVTYPE type, HWND focus, DWORD flags,
                                                   d8::D3DPRESENT_PARAMETERS *pp, d8::IDirect3DDevice8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    D3DPRESENT_PARAMETERS pp9;
    if (!q8_convert_pp(pp, &pp9))
        return D3DERR_INVALIDCALL;
    IDirect3DDevice9 *dev9 = NULL;
    HRESULT hr = d3d9->CreateDevice(adapter, (D3DDEVTYPE)type, focus, flags, &pp9, &dev9);
    if (FAILED(hr)) {
        q8_log("qd3d8: CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        return hr;
    }
    q8_writeback_pp(&pp9, pp);
    Q8Device *dev = new Q8Device(this, dev9, adapter, (D3DDEVTYPE)type, focus, flags, &pp9);
    if (!dev) {
        dev9->Release();
        return E_OUTOFMEMORY;
    }
    *out = dev;
    return D3D_OK;
}
