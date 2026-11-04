/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: IDirect3DDevice8 on IDirect3DDevice9.
 *
 * Where D3D8 and D3D9 answer differently, the device answers like the stock
 * 32-bit d3d8.dll (measured; docs/design/d3d9-remoting.md, Direct3D 8), so
 * the result does not depend on the Direct3D 9 runtime underneath.
 */
#include "qd3d8.h"
#include <new>

/* D3D8 ZBIAS is an integer 0-16 where larger values pull geometry towards
 * the viewer; D3D9 DEPTHBIAS is a float offset in normalised depth units.
 * One ZBIAS step maps to one D16 depth unit. */
#define Q8_ZBIAS_SCALE (-1.0f / 65535.0f)

/* Constant registers a vs.1.x function may use on a software vertex
 * processing device (and what the device caps report either way). */
#define Q8_SW_VS_CONSTS 96u

static inline DWORD q8_f2dw(float f)
{
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}

static inline float q8_dw2f(DWORD d)
{
    float f;
    memcpy(&f, &d, sizeof(f));
    return f;
}

Q8Device::Q8Device(Q8Direct3D *p, IDirect3DDevice9 *d, UINT adapter_, D3DDEVTYPE type, HWND focus_, DWORD behavior_,
                   const D3DPRESENT_PARAMETERS *pp9)
    : refs(1), parent(p), d9(d), recording(NULL), b(), rt(NULL), ds(NULL), ds_known(false), owned(),
      mt((behavior_ & D3DCREATE_MULTITHREADED) != 0), adapter(adapter_), devtype(type), behavior(behavior_),
      focus(focus_), window(NULL), windowed(TRUE), bb_w(0), bb_h(0), bb_ms(D3DMULTISAMPLE_NONE),
      vs_consts(Q8_SW_VS_CONSTS), ps_version(~0u), needs_reset(false), default_objects(0)
{
    memset(&vshaders, 0, sizeof(vshaders));
    memset(&pshaders, 0, sizeof(pshaders));
    memset(&stateblocks, 0, sizeof(stateblocks));
    InitializeCriticalSection(&cs);
    parent->AddRef();
    D3DCAPS9 c9;
    if (SUCCEEDED(d9->GetDeviceCaps(&c9))) {
        d8::D3DCAPS8 c8;
        q8_convert_caps(&c9, &c8);
        ps_version = c8.PixelShaderVersion;
        /* Measured: a hardware device takes the adapter's constants, a
         * software one 96. */
        if (!(behavior & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
            vs_consts = c9.MaxVertexShaderConst;
    }
    set_presentation(pp9);
    reset_d3d8_state();
}

Q8Device::~Q8Device()
{
    DeleteCriticalSection(&cs);
}

/* Front-end state of a new or Reset device, and the D3D8 render state
 * defaults D3D9 changed (D3DRS_POINTSIZE_MIN: 0.0 in D3D8, 1.0 in D3D9). */
void Q8Device::reset_d3d8_state()
{
    cur_vs = 0;
    cur_ps = 0;
    rs_linepattern = 0;
    rs_zvisible = 0;
    rs_zbias = 0;
    rs_patchsegments = q8_f2dw(1.0f);
    d9->SetRenderState(D3DRS_POINTSIZE_MIN, q8_f2dw(0.0f));
}

/* What CreateDevice or Reset set up (after D3D9 filled in its defaults). */
void Q8Device::set_presentation(const D3DPRESENT_PARAMETERS *pp9)
{
    window = pp9->hDeviceWindow ? pp9->hDeviceWindow : focus;
    windowed = pp9->Windowed;
    bb_w = pp9->BackBufferWidth;
    bb_h = pp9->BackBufferHeight;
    bb_ms = pp9->MultiSampleType;
}

/* ------------------------------------------------------------------------ */
/* Bindings: what the device and its state blocks keep alive (Q8Object).     */

/* A NULL buffer leaves the stride or base vertex index as it was (native). */
static void q8_set_stream(Q8Binds *b, UINT i, Q8Object *vb, UINT stride)
{
    q8_slot_set(&b->stream[i], vb);
    if (vb)
        b->stride[i] = stride;
}

static void q8_set_indices(Q8Binds *b, Q8Object *ib, UINT base)
{
    q8_slot_set(&b->indices, ib);
    if (ib)
        b->base = base;
}

static void q8_binds_clear(Q8Binds *b)
{
    for (UINT i = 0; i < Q8_TEX_SLOTS; ++i)
        q8_slot_set(&b->tex[i], NULL);
    for (UINT i = 0; i < Q8_STREAM_SLOTS; ++i) {
        q8_slot_set(&b->stream[i], NULL);
        b->stride[i] = 0;
    }
    q8_slot_set(&b->indices, NULL);
    b->base = 0;
}

/* The device bindings a state block contains go into the block. */
static void q8_binds_capture(Q8StateBlock *sb, const Q8Binds *dev)
{
    for (UINT i = 0; i < Q8_TEX_SLOTS; ++i)
        if (sb->tex_mask & (1u << i))
            q8_slot_set(&sb->b.tex[i], dev->tex[i]);
    for (UINT i = 0; i < Q8_STREAM_SLOTS; ++i) {
        if (sb->stream_mask & (1u << i)) {
            q8_slot_set(&sb->b.stream[i], dev->stream[i]);
            sb->b.stride[i] = dev->stride[i];
        }
    }
    if (sb->has_indices) {
        q8_slot_set(&sb->b.indices, dev->indices);
        sb->b.base = dev->base;
    }
}

/* A state block's bindings go to the device, as the Set* calls would. */
static void q8_binds_apply(const Q8StateBlock *sb, Q8Binds *dev)
{
    for (UINT i = 0; i < Q8_TEX_SLOTS; ++i)
        if (sb->tex_mask & (1u << i))
            q8_slot_set(&dev->tex[i], sb->b.tex[i]);
    for (UINT i = 0; i < Q8_STREAM_SLOTS; ++i)
        if (sb->stream_mask & (1u << i))
            q8_set_stream(dev, i, sb->b.stream[i], sb->b.stride[i]);
    if (sb->has_indices)
        q8_set_indices(dev, sb->b.indices, sb->b.base);
}

static void q8_sb_free(Q8StateBlock *sb)
{
    if (!sb)
        return;
    q8_binds_clear(&sb->b);
    if (sb->sb9)
        sb->sb9->Release();
    free(sb);
}

/* Reset (also a failing one, as natively) and the device's end: the
 * recording and all state blocks go, nothing stays bound and the device
 * lets go of its implicit surfaces.  D3D9 then sees no reference the D3D8
 * device kept, so its own Reset can succeed. */
void Q8Device::drop_state()
{
    if (recording) {
        IDirect3DStateBlock9 *sb = NULL;
        if (SUCCEEDED(d9->EndStateBlock(&sb)) && sb)
            sb->Release();
        q8_sb_free(recording);
        recording = NULL;
    }
    for (UINT i = 1; i <= stateblocks.count; ++i) {
        q8_sb_free((Q8StateBlock *)q8_table_get(&stateblocks, i));
        q8_table_clear_slot(&stateblocks, i);
    }
    q8_binds_clear(&b);
    q8_slot_set(&rt, NULL);
    q8_slot_set(&ds, NULL);
    ds_known = false;
    q8_release_owned(&owned);
}

Q8VertexShader *Q8Device::vs_from_handle(DWORD h)
{
    if (!(h & 1))
        return NULL;
    return (Q8VertexShader *)q8_table_get(&vshaders, h >> 1);
}

Q8PixelShader *Q8Device::ps_from_handle(DWORD h)
{
    return (Q8PixelShader *)q8_table_get(&pshaders, h);
}

/* Size and multisampling of the current render target. */
void Q8Device::target_size(UINT *w, UINT *h, D3DMULTISAMPLE_TYPE *ms)
{
    if (rt) {
        const D3DSURFACE_DESC *d = &static_cast<Q8Surface *>(rt)->desc9;
        *w = d->Width;
        *h = d->Height;
        *ms = d->MultiSampleType;
    } else {
        *w = bb_w;
        *h = bb_h;
        *ms = bb_ms;
    }
}

/* Native D3D8 refuses to draw while a SYSTEMMEM texture is set on any stage
 * (E_FAIL), whether or not the stage samples it. */
bool Q8Device::sysmem_texture_bound()
{
    for (UINT i = 0; i < Q8_TEX_SLOTS; ++i)
        if (b.tex[i] && b.tex[i]->pool == D3DPOOL_SYSTEMMEM)
            return true;
    return false;
}

/* ------------------------------------------------------------------------ */
/* A device whose last reference is gone keeps this husk in its place: late
 * calls through a stale pointer (the Wine test_refcount cleanup calls
 * DeleteStateBlock after the device died with its last child) answer
 * D3DERR_INVALIDCALL instead of running on freed memory.  Everything the
 * device owned is released before; only these few bytes stay.             */

class Q8DeadDevice final : public d8::IDirect3DDevice8 {
    LONG refs;

public:
    Q8DeadDevice() : refs(0) {}
    STDMETHOD(QueryInterface)(REFIID, void **obj) override
    {
        if (obj)
            *obj = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (r < 0) {
            InterlockedIncrement(&refs);
            return 0;
        }
        return (ULONG)r;
    }
#define Q8_DEAD(name, args) \
    STDMETHOD(name) args override { return D3DERR_INVALIDCALL; }
    Q8_DEAD(TestCooperativeLevel, ())
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 0; }
    Q8_DEAD(ResourceManagerDiscardBytes, (DWORD))
    Q8_DEAD(GetDirect3D, (d8::IDirect3D8 **))
    Q8_DEAD(GetDeviceCaps, (d8::D3DCAPS8 *))
    Q8_DEAD(GetDisplayMode, (d8::D3DDISPLAYMODE *))
    Q8_DEAD(GetCreationParameters, (d8::D3DDEVICE_CREATION_PARAMETERS *))
    Q8_DEAD(SetCursorProperties, (UINT, UINT, d8::IDirect3DSurface8 *))
    STDMETHOD_(void, SetCursorPosition)(UINT, UINT, DWORD) override {}
    STDMETHOD_(WINBOOL, ShowCursor)(WINBOOL) override { return FALSE; }
    Q8_DEAD(CreateAdditionalSwapChain, (d8::D3DPRESENT_PARAMETERS *, d8::IDirect3DSwapChain8 **))
    Q8_DEAD(Reset, (d8::D3DPRESENT_PARAMETERS *))
    Q8_DEAD(Present, (const RECT *, const RECT *, HWND, const RGNDATA *))
    Q8_DEAD(GetBackBuffer, (UINT, d8::D3DBACKBUFFER_TYPE, d8::IDirect3DSurface8 **))
    Q8_DEAD(GetRasterStatus, (d8::D3DRASTER_STATUS *))
    STDMETHOD_(void, SetGammaRamp)(DWORD, const d8::D3DGAMMARAMP *) override {}
    STDMETHOD_(void, GetGammaRamp)(d8::D3DGAMMARAMP *) override {}
    Q8_DEAD(CreateTexture, (UINT, UINT, UINT, DWORD, d8::D3DFORMAT, d8::D3DPOOL, d8::IDirect3DTexture8 **))
    Q8_DEAD(CreateVolumeTexture,
            (UINT, UINT, UINT, UINT, DWORD, d8::D3DFORMAT, d8::D3DPOOL, d8::IDirect3DVolumeTexture8 **))
    Q8_DEAD(CreateCubeTexture, (UINT, UINT, DWORD, d8::D3DFORMAT, d8::D3DPOOL, d8::IDirect3DCubeTexture8 **))
    Q8_DEAD(CreateVertexBuffer, (UINT, DWORD, DWORD, d8::D3DPOOL, d8::IDirect3DVertexBuffer8 **))
    Q8_DEAD(CreateIndexBuffer, (UINT, DWORD, d8::D3DFORMAT, d8::D3DPOOL, d8::IDirect3DIndexBuffer8 **))
    Q8_DEAD(CreateRenderTarget,
            (UINT, UINT, d8::D3DFORMAT, d8::D3DMULTISAMPLE_TYPE, WINBOOL, d8::IDirect3DSurface8 **))
    Q8_DEAD(CreateDepthStencilSurface, (UINT, UINT, d8::D3DFORMAT, d8::D3DMULTISAMPLE_TYPE, d8::IDirect3DSurface8 **))
    Q8_DEAD(CreateImageSurface, (UINT, UINT, d8::D3DFORMAT, d8::IDirect3DSurface8 **))
    Q8_DEAD(CopyRects, (d8::IDirect3DSurface8 *, const RECT *, UINT, d8::IDirect3DSurface8 *, const POINT *))
    Q8_DEAD(UpdateTexture, (d8::IDirect3DBaseTexture8 *, d8::IDirect3DBaseTexture8 *))
    Q8_DEAD(GetFrontBuffer, (d8::IDirect3DSurface8 *))
    Q8_DEAD(SetRenderTarget, (d8::IDirect3DSurface8 *, d8::IDirect3DSurface8 *))
    Q8_DEAD(GetRenderTarget, (d8::IDirect3DSurface8 **))
    Q8_DEAD(GetDepthStencilSurface, (d8::IDirect3DSurface8 **))
    Q8_DEAD(BeginScene, ())
    Q8_DEAD(EndScene, ())
    Q8_DEAD(Clear, (DWORD, const d8::D3DRECT *, DWORD, d8::D3DCOLOR, float, DWORD))
    Q8_DEAD(SetTransform, (d8::D3DTRANSFORMSTATETYPE, const d8::D3DMATRIX *))
    Q8_DEAD(GetTransform, (d8::D3DTRANSFORMSTATETYPE, d8::D3DMATRIX *))
    Q8_DEAD(MultiplyTransform, (d8::D3DTRANSFORMSTATETYPE, const d8::D3DMATRIX *))
    Q8_DEAD(SetViewport, (const d8::D3DVIEWPORT8 *))
    Q8_DEAD(GetViewport, (d8::D3DVIEWPORT8 *))
    Q8_DEAD(SetMaterial, (const d8::D3DMATERIAL8 *))
    Q8_DEAD(GetMaterial, (d8::D3DMATERIAL8 *))
    Q8_DEAD(SetLight, (DWORD, const d8::D3DLIGHT8 *))
    Q8_DEAD(GetLight, (DWORD, d8::D3DLIGHT8 *))
    Q8_DEAD(LightEnable, (DWORD, WINBOOL))
    Q8_DEAD(GetLightEnable, (DWORD, WINBOOL *))
    Q8_DEAD(SetClipPlane, (DWORD, const float *))
    Q8_DEAD(GetClipPlane, (DWORD, float *))
    Q8_DEAD(SetRenderState, (d8::D3DRENDERSTATETYPE, DWORD))
    Q8_DEAD(GetRenderState, (d8::D3DRENDERSTATETYPE, DWORD *))
    Q8_DEAD(BeginStateBlock, ())
    Q8_DEAD(EndStateBlock, (DWORD *))
    Q8_DEAD(ApplyStateBlock, (DWORD))
    Q8_DEAD(CaptureStateBlock, (DWORD))
    Q8_DEAD(DeleteStateBlock, (DWORD))
    Q8_DEAD(CreateStateBlock, (d8::D3DSTATEBLOCKTYPE, DWORD *))
    Q8_DEAD(SetClipStatus, (const d8::D3DCLIPSTATUS8 *))
    Q8_DEAD(GetClipStatus, (d8::D3DCLIPSTATUS8 *))
    Q8_DEAD(GetTexture, (DWORD, d8::IDirect3DBaseTexture8 **))
    Q8_DEAD(SetTexture, (DWORD, d8::IDirect3DBaseTexture8 *))
    Q8_DEAD(GetTextureStageState, (DWORD, d8::D3DTEXTURESTAGESTATETYPE, DWORD *))
    Q8_DEAD(SetTextureStageState, (DWORD, d8::D3DTEXTURESTAGESTATETYPE, DWORD))
    Q8_DEAD(ValidateDevice, (DWORD *))
    Q8_DEAD(GetInfo, (DWORD, void *, DWORD))
    Q8_DEAD(SetPaletteEntries, (UINT, const PALETTEENTRY *))
    Q8_DEAD(GetPaletteEntries, (UINT, PALETTEENTRY *))
    Q8_DEAD(SetCurrentTexturePalette, (UINT))
    Q8_DEAD(GetCurrentTexturePalette, (UINT *))
    Q8_DEAD(DrawPrimitive, (d8::D3DPRIMITIVETYPE, UINT, UINT))
    Q8_DEAD(DrawIndexedPrimitive, (d8::D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT))
    Q8_DEAD(DrawPrimitiveUP, (d8::D3DPRIMITIVETYPE, UINT, const void *, UINT))
    Q8_DEAD(DrawIndexedPrimitiveUP,
            (d8::D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, d8::D3DFORMAT, const void *, UINT))
    Q8_DEAD(ProcessVertices, (UINT, UINT, UINT, d8::IDirect3DVertexBuffer8 *, DWORD))
    Q8_DEAD(CreateVertexShader, (const DWORD *, const DWORD *, DWORD *, DWORD))
    Q8_DEAD(SetVertexShader, (DWORD))
    Q8_DEAD(GetVertexShader, (DWORD *))
    Q8_DEAD(DeleteVertexShader, (DWORD))
    Q8_DEAD(SetVertexShaderConstant, (DWORD, const void *, DWORD))
    Q8_DEAD(GetVertexShaderConstant, (DWORD, void *, DWORD))
    Q8_DEAD(GetVertexShaderDeclaration, (DWORD, void *, DWORD *))
    Q8_DEAD(GetVertexShaderFunction, (DWORD, void *, DWORD *))
    Q8_DEAD(SetStreamSource, (UINT, d8::IDirect3DVertexBuffer8 *, UINT))
    Q8_DEAD(GetStreamSource, (UINT, d8::IDirect3DVertexBuffer8 **, UINT *))
    Q8_DEAD(SetIndices, (d8::IDirect3DIndexBuffer8 *, UINT))
    Q8_DEAD(GetIndices, (d8::IDirect3DIndexBuffer8 **, UINT *))
    Q8_DEAD(CreatePixelShader, (const DWORD *, DWORD *))
    Q8_DEAD(SetPixelShader, (DWORD))
    Q8_DEAD(GetPixelShader, (DWORD *))
    Q8_DEAD(DeletePixelShader, (DWORD))
    Q8_DEAD(SetPixelShaderConstant, (DWORD, const void *, DWORD))
    Q8_DEAD(GetPixelShaderConstant, (DWORD, void *, DWORD))
    Q8_DEAD(GetPixelShaderFunction, (DWORD, void *, DWORD *))
    Q8_DEAD(DrawRectPatch, (UINT, const float *, const d8::D3DRECTPATCH_INFO *))
    Q8_DEAD(DrawTriPatch, (UINT, const float *, const d8::D3DTRIPATCH_INFO *))
    Q8_DEAD(DeletePatch, (UINT))
#undef Q8_DEAD
};

/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE Q8Device::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return E_POINTER;
    if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, QIID_IDirect3DDevice8)) {
        AddRef();
        *obj = static_cast<d8::IDirect3DDevice8 *>(this);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q8Device::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q8Device::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r != 0)
        return (ULONG)r;
    /* A released device lets go of everything it keeps alive. */
    drop_state();
    for (UINT i = 1; i <= vshaders.count; ++i)
        q8_destroy_vertex_shader((Q8VertexShader *)q8_table_get(&vshaders, i));
    for (UINT i = 1; i <= pshaders.count; ++i)
        q8_destroy_pixel_shader((Q8PixelShader *)q8_table_get(&pshaders, i));
    q8_table_free(&stateblocks);
    q8_table_free(&vshaders);
    q8_table_free(&pshaders);
    d9->Release();
    Q8Direct3D *p = parent;
    /* The storage stays as the husk (Q8DeadDevice above); it is never freed. */
    static_assert(sizeof(Q8DeadDevice) <= sizeof(Q8Device), "husk must fit the device");
    this->~Q8Device();
    new (static_cast<void *>(this)) Q8DeadDevice();
    p->Release();
    return 0;
}

HRESULT STDMETHODCALLTYPE Q8Device::TestCooperativeLevel()
{
    if (needs_reset)
        return D3DERR_DEVICENOTRESET;
    return d9->TestCooperativeLevel();
}

UINT STDMETHODCALLTYPE Q8Device::GetAvailableTextureMem()
{
    return d9->GetAvailableTextureMem();
}

HRESULT STDMETHODCALLTYPE Q8Device::ResourceManagerDiscardBytes(DWORD bytes)
{
    /* D3D9 can only evict everything; D3D8 treats the byte count as a hint. */
    return d9->EvictManagedResources();
}

HRESULT STDMETHODCALLTYPE Q8Device::GetDirect3D(d8::IDirect3D8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    parent->AddRef();
    *out = parent;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetDeviceCaps(d8::D3DCAPS8 *caps)
{
    if (!caps)
        return D3DERR_INVALIDCALL;
    D3DCAPS9 c9;
    HRESULT hr = d9->GetDeviceCaps(&c9);
    if (FAILED(hr))
        return hr;
    q8_convert_caps(&c9, caps);
    /* Measured: the device reports 96 constants (hardware and software
     * vertex processing alike) where the adapter reports its own count. */
    if (caps->MaxVertexShaderConst > Q8_SW_VS_CONSTS)
        caps->MaxVertexShaderConst = Q8_SW_VS_CONSTS;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetDisplayMode(d8::D3DDISPLAYMODE *mode)
{
    if (!mode)
        return D3DERR_INVALIDCALL;
    return d9->GetDisplayMode(0, (D3DDISPLAYMODE *)mode);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetCreationParameters(d8::D3DDEVICE_CREATION_PARAMETERS *p)
{
    if (!p)
        return D3DERR_INVALIDCALL;
    return d9->GetCreationParameters((D3DDEVICE_CREATION_PARAMETERS *)p);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetCursorProperties(UINT x, UINT y, d8::IDirect3DSurface8 *bitmap)
{
    Q8Surface *s = q8_find_surface(bitmap);
    if (!s)
        return D3DERR_INVALIDCALL;
    return d9->SetCursorProperties(x, y, s->d9);
}

void STDMETHODCALLTYPE Q8Device::SetCursorPosition(UINT x, UINT y, DWORD flags)
{
    d9->SetCursorPosition((int)x, (int)y, flags);
}

WINBOOL STDMETHODCALLTYPE Q8Device::ShowCursor(WINBOOL show)
{
    return d9->ShowCursor(show);
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateAdditionalSwapChain(d8::D3DPRESENT_PARAMETERS *pp,
                                                              d8::IDirect3DSwapChain8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    D3DPRESENT_PARAMETERS pp9;
    if (!q8_convert_pp(pp, &pp9))
        return D3DERR_INVALIDCALL;
    IDirect3DSwapChain9 *sc = NULL;
    HRESULT hr = d9->CreateAdditionalSwapChain(&pp9, &sc);
    if (FAILED(hr))
        return hr;
    /* Native D3D8 writes back only the defaulted back buffer count here:
     * zero sizes stay zero (Wine's test_swapchain on the stock runtime). */
    pp->BackBufferCount = pp9.BackBufferCount;
    Q8SwapChain *w = q8_new<Q8SwapChain>(this, sc, D3DPOOL_DEFAULT);
    if (!w)
        return E_OUTOFMEMORY;
    w->window = pp9.hDeviceWindow ? pp9.hDeviceWindow : focus;
    *out = w;
    return D3D_OK;
}

/*
 * Native Reset (measured): every binding, state block and implicit surface
 * goes even when it fails; with a D3DPOOL_DEFAULT object still alive it
 * fails with D3DERR_DEVICELOST; invalid parameters fail with
 * D3DERR_INVALIDCALL.  After any failure the device stays lost until a
 * Reset succeeds: TestCooperativeLevel answers D3DERR_DEVICENOTRESET and
 * Present D3DERR_DEVICELOST.
 */
HRESULT STDMETHODCALLTYPE Q8Device::Reset(d8::D3DPRESENT_PARAMETERS *pp)
{
    Q8DevLock lock(this);
    D3DPRESENT_PARAMETERS pp9;
    if (!q8_convert_pp(pp, &pp9)) {
        needs_reset = true;
        return D3DERR_INVALIDCALL;
    }
    drop_state();
    if (default_objects > 0) {
        q8_log("qd3d8: Reset with %ld D3DPOOL_DEFAULT objects alive: device lost", (long)default_objects);
        needs_reset = true;
        return D3DERR_DEVICELOST;
    }
    HRESULT hr = d9->Reset(&pp9);
    if (FAILED(hr)) {
        q8_log("qd3d8: Reset failed hr=0x%08lx", (unsigned long)hr);
        needs_reset = true;
        /* D3D9 refuses an unusable back buffer format as not available;
         * D3D8 calls such parameters invalid. */
        return hr == D3DERR_NOTAVAILABLE ? D3DERR_INVALIDCALL : hr;
    }
    needs_reset = false;
    q8_writeback_pp(&pp9, pp);
    set_presentation(&pp9);
    reset_d3d8_state();
    return D3D_OK;
}

/* Native Present: a lost device presents nothing, a destroyed target window
 * is invalid, and D3D9's success codes (S_PRESENT_OCCLUDED) read as D3D_OK. */
HRESULT STDMETHODCALLTYPE Q8Device::Present(const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (needs_reset)
        return D3DERR_DEVICELOST;
    if (!IsWindow(wnd ? wnd : window))
        return D3DERR_INVALIDCALL;
    HRESULT hr = d9->Present(src, dst, wnd, dirty);
    return SUCCEEDED(hr) ? D3D_OK : hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetBackBuffer(UINT index, d8::D3DBACKBUFFER_TYPE type,
                                                  d8::IDirect3DSurface8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->GetBackBuffer(0, index, (D3DBACKBUFFER_TYPE)type, &s);
    if (FAILED(hr))
        return hr;
    *out = q8_implicit_surface(this, &owned, s);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetRasterStatus(d8::D3DRASTER_STATUS *status)
{
    if (!status)
        return D3DERR_INVALIDCALL;
    return d9->GetRasterStatus(0, (D3DRASTER_STATUS *)status);
}

void STDMETHODCALLTYPE Q8Device::SetGammaRamp(DWORD flags, const d8::D3DGAMMARAMP *ramp)
{
    if (ramp)
        d9->SetGammaRamp(0, flags, (const D3DGAMMARAMP *)ramp);
}

void STDMETHODCALLTYPE Q8Device::GetGammaRamp(d8::D3DGAMMARAMP *ramp)
{
    if (ramp)
        d9->GetGammaRamp(0, (D3DGAMMARAMP *)ramp);
}

/* ------------------------------------------------------------------------ */
/* Resource creation                                                         */

/* Native D3D8 refuses D3DUSAGE_DYNAMIC in the managed and scratch pools for
 * every resource type (measured; D3D9 agrees). */
static bool q8_usage_valid(d8::D3DPOOL pool, DWORD usage)
{
    return !(usage & D3DUSAGE_DYNAMIC) || (pool != d8::D3DPOOL_MANAGED && pool != d8::D3DPOOL_SCRATCH);
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, d8::D3DFORMAT fmt,
                                                  d8::D3DPOOL pool, d8::IDirect3DTexture8 **out)
{
    /* Native: D3DFMT_UNKNOWN fails before the output is touched. */
    if (!out || fmt == d8::D3DFMT_UNKNOWN)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_usage_valid(pool, usage))
        return D3DERR_INVALIDCALL;
    IDirect3DTexture9 *t = NULL;
    HRESULT hr = d9->CreateTexture(w, h, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &t, NULL);
    if (FAILED(hr))
        return hr;
    *out = q8_new<Q8Texture>(this, t, pool);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                                        d8::D3DFORMAT fmt, d8::D3DPOOL pool,
                                                        d8::IDirect3DVolumeTexture8 **out)
{
    if (!out || fmt == d8::D3DFMT_UNKNOWN)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_usage_valid(pool, usage))
        return D3DERR_INVALIDCALL;
    IDirect3DVolumeTexture9 *t = NULL;
    HRESULT hr = d9->CreateVolumeTexture(w, h, d, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &t, NULL);
    if (FAILED(hr))
        return hr;
    *out = q8_new<Q8VolumeTexture>(this, t, pool);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, d8::D3DFORMAT fmt,
                                                      d8::D3DPOOL pool, d8::IDirect3DCubeTexture8 **out)
{
    if (!out || fmt == d8::D3DFMT_UNKNOWN)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_usage_valid(pool, usage))
        return D3DERR_INVALIDCALL;
    IDirect3DCubeTexture9 *t = NULL;
    HRESULT hr = d9->CreateCubeTexture(edge, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &t, NULL);
    if (FAILED(hr))
        return hr;
    *out = q8_new<Q8CubeTexture>(this, t, pool);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, d8::D3DPOOL pool,
                                                       d8::IDirect3DVertexBuffer8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_usage_valid(pool, usage))
        return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9 *b = NULL;
    HRESULT hr = d9->CreateVertexBuffer(len, usage, fvf, (D3DPOOL)pool, &b, NULL);
    if (FAILED(hr))
        return hr;
    *out = q8_new<Q8VertexBuffer>(this, b, pool);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateIndexBuffer(UINT len, DWORD usage, d8::D3DFORMAT fmt, d8::D3DPOOL pool,
                                                      d8::IDirect3DIndexBuffer8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_usage_valid(pool, usage))
        return D3DERR_INVALIDCALL;
    IDirect3DIndexBuffer9 *b = NULL;
    HRESULT hr = d9->CreateIndexBuffer(len, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &b, NULL);
    if (FAILED(hr))
        return hr;
    *out = q8_new<Q8IndexBuffer>(this, b, pool);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

/* A multisample type D3D8 knows: none, or 2 to 16 samples (D3D9's
 * D3DMULTISAMPLE_NONMASKABLE, 1, does not exist there). */
static bool q8_ms_valid(d8::D3DMULTISAMPLE_TYPE ms)
{
    return ms == d8::D3DMULTISAMPLE_NONE || (ms >= d8::D3DMULTISAMPLE_2_SAMPLES && ms <= d8::D3DMULTISAMPLE_16_SAMPLES);
}

/* A failed multisampled creation fails natively with D3DERR_NOTAVAILABLE
 * when the type is not available for the format. */
static HRESULT q8_ms_failure(Q8Device *dev, HRESULT hr, D3DFORMAT fmt, d8::D3DMULTISAMPLE_TYPE ms)
{
    if (ms != d8::D3DMULTISAMPLE_NONE &&
        FAILED(dev->parent->d3d9->CheckDeviceMultiSampleType(dev->adapter, dev->devtype, fmt, dev->windowed,
                                                             (D3DMULTISAMPLE_TYPE)ms, NULL)))
        return D3DERR_NOTAVAILABLE;
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateRenderTarget(UINT w, UINT h, d8::D3DFORMAT fmt, d8::D3DMULTISAMPLE_TYPE ms,
                                                       WINBOOL lockable, d8::IDirect3DSurface8 **out)
{
    if (!out || fmt == d8::D3DFMT_UNKNOWN)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_ms_valid(ms))
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->CreateRenderTarget(w, h, (D3DFORMAT)fmt, (D3DMULTISAMPLE_TYPE)ms, 0, lockable, &s, NULL);
    if (FAILED(hr))
        return q8_ms_failure(this, hr, (D3DFORMAT)fmt, ms);
    *out = q8_new<Q8Surface>(this, s, D3DPOOL_DEFAULT);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateDepthStencilSurface(UINT w, UINT h, d8::D3DFORMAT fmt,
                                                              d8::D3DMULTISAMPLE_TYPE ms, d8::IDirect3DSurface8 **out)
{
    if (!out || fmt == d8::D3DFMT_UNKNOWN)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!q8_ms_valid(ms))
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->CreateDepthStencilSurface(w, h, (D3DFORMAT)fmt, (D3DMULTISAMPLE_TYPE)ms, 0, FALSE, &s, NULL);
    if (FAILED(hr))
        return q8_ms_failure(this, hr, (D3DFORMAT)fmt, ms);
    *out = q8_new<Q8Surface>(this, s, D3DPOOL_DEFAULT);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateImageSurface(UINT w, UINT h, d8::D3DFORMAT fmt, d8::IDirect3DSurface8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    IDirect3DSurface9 *s = NULL;
    /* D3D8 image surfaces are lockable system-memory surfaces of any format;
     * SCRATCH covers formats the device cannot use. */
    HRESULT hr = d9->CreateOffscreenPlainSurface(w, h, (D3DFORMAT)fmt, D3DPOOL_SYSTEMMEM, &s, NULL);
    if (FAILED(hr))
        hr = d9->CreateOffscreenPlainSurface(w, h, (D3DFORMAT)fmt, D3DPOOL_SCRATCH, &s, NULL);
    if (FAILED(hr))
        return hr;
    Q8Surface *w8 = q8_new<Q8Surface>(this, s, D3DPOOL_SYSTEMMEM);
    if (!w8)
        return E_OUTOFMEMORY;
    w8->flags |= Q8SF_IMAGE;
    *out = w8;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* CopyRects                                                                 */

static bool q8_rect_aligned(const RECT *r, UINT bw, UINT bh, UINT width, UINT height)
{
    if ((r->left % bw) || (r->top % bh))
        return false;
    if (((UINT)r->right % bw) && (UINT)r->right != width)
        return false;
    if (((UINT)r->bottom % bh) && (UINT)r->bottom != height)
        return false;
    return true;
}

static void q8_copy_rows(const D3DLOCKED_RECT *s, D3DLOCKED_RECT *d, UINT w, UINT h, D3DFORMAT fmt)
{
    UINT bw, bh, bytes;
    q8_format_blocks(fmt, &bw, &bh, &bytes);
    UINT rows = (h + bh - 1) / bh;
    UINT row_bytes = ((w + bw - 1) / bw) * bytes;
    const BYTE *sp = (const BYTE *)s->pBits;
    BYTE *dp = (BYTE *)d->pBits;
    for (UINT y = 0; y < rows; ++y)
        memcpy(dp + (size_t)y * d->Pitch, sp + (size_t)y * s->Pitch, row_bytes);
}

/* Copies r from a lockable source to point p of a lockable destination. */
static HRESULT q8_lock_copy(IDirect3DSurface9 *src, const RECT *r, IDirect3DSurface9 *dst, const POINT *p,
                            D3DFORMAT fmt)
{
    UINT w = (UINT)(r->right - r->left), h = (UINT)(r->bottom - r->top);
    RECT dr = {p->x, p->y, p->x + (LONG)w, p->y + (LONG)h};
    D3DLOCKED_RECT sl, dl;
    HRESULT hr = src->LockRect(&sl, r, D3DLOCK_READONLY);
    if (FAILED(hr))
        return hr;
    hr = dst->LockRect(&dl, &dr, 0);
    if (FAILED(hr)) {
        src->UnlockRect();
        return hr;
    }
    q8_copy_rows(&sl, &dl, w, h, fmt);
    dst->UnlockRect();
    src->UnlockRect();
    return D3D_OK;
}

static bool q8_surface_lockable(const D3DSURFACE_DESC *d)
{
    return d->Pool != D3DPOOL_DEFAULT;
}

/* A system-memory copy of a default-pool surface.  D3D9 reads back only
 * single-sampled render targets, so anything else (multisampled targets,
 * plain default-pool texture levels) is first stretched into one. */
static HRESULT q8_read_target(IDirect3DDevice9 *d9, IDirect3DSurface9 *src, const D3DSURFACE_DESC *sd,
                              IDirect3DSurface9 **out)
{
    IDirect3DSurface9 *resolved = NULL, *mem = NULL;
    HRESULT hr = D3D_OK;
    if (sd->MultiSampleType != D3DMULTISAMPLE_NONE || !(sd->Usage & D3DUSAGE_RENDERTARGET)) {
        hr = d9->CreateRenderTarget(sd->Width, sd->Height, sd->Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved,
                                    NULL);
        if (SUCCEEDED(hr))
            hr = d9->StretchRect(src, NULL, resolved, NULL, D3DTEXF_NONE);
        src = resolved;
    }
    if (SUCCEEDED(hr))
        hr = d9->CreateOffscreenPlainSurface(sd->Width, sd->Height, sd->Format, D3DPOOL_SYSTEMMEM, &mem, NULL);
    if (SUCCEEDED(hr))
        hr = d9->GetRenderTargetData(src, mem);
    if (resolved)
        resolved->Release();
    if (FAILED(hr) && mem) {
        mem->Release();
        mem = NULL;
    }
    *out = mem;
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::CopyRects(d8::IDirect3DSurface8 *src8, const RECT *rects, UINT count,
                                              d8::IDirect3DSurface8 *dst8, const POINT *points)
{
    Q8Surface *sw = q8_find_surface(src8), *dw = q8_find_surface(dst8);
    if (!sw || !dw || sw == dw || sw->device != this || dw->device != this)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9 *src = sw->d9, *dst = dw->d9;
    const D3DSURFACE_DESC sd = sw->desc9, dd = dw->desc9;
    /* Native copies multisampled surfaces too (resolving them), but never
     * depth stencils. */
    if (sd.Format != dd.Format || (sd.Usage & D3DUSAGE_DEPTHSTENCIL) || (dd.Usage & D3DUSAGE_DEPTHSTENCIL))
        return D3DERR_INVALIDCALL;
    UINT bw, bh, bytes;
    if (!q8_format_blocks(sd.Format, &bw, &bh, &bytes))
        return D3DERR_INVALIDCALL;

    RECT full = {0, 0, (LONG)sd.Width, (LONG)sd.Height};
    if (!rects || count == 0) {
        rects = &full;
        count = 1;
        points = NULL;
    }

    /* Validate everything before touching either surface. */
    for (UINT i = 0; i < count; ++i) {
        const RECT *r = &rects[i];
        if (r->left < 0 || r->top < 0 || r->left >= r->right || r->top >= r->bottom ||
            (UINT)r->right > sd.Width || (UINT)r->bottom > sd.Height)
            return D3DERR_INVALIDCALL;
        POINT p = points ? points[i] : POINT{r->left, r->top};
        RECT dr = {p.x, p.y, p.x + (r->right - r->left), p.y + (r->bottom - r->top)};
        if (p.x < 0 || p.y < 0 || (UINT)dr.right > dd.Width || (UINT)dr.bottom > dd.Height)
            return D3DERR_INVALIDCALL;
        if (!q8_rect_aligned(r, bw, bh, sd.Width, sd.Height) || !q8_rect_aligned(&dr, bw, bh, dd.Width, dd.Height))
            return D3DERR_INVALIDCALL;
    }

    HRESULT hr = D3D_OK;
    IDirect3DSurface9 *readable = NULL;  /* lockable copy of src (or src) */
    IDirect3DSurface9 *staging = NULL;   /* sysmem image of dst for UpdateSurface */
    bool dst_lockable = q8_surface_lockable(&dd);

    for (UINT i = 0; i < count && SUCCEEDED(hr); ++i) {
        const RECT *r = &rects[i];
        POINT p = points ? points[i] : POINT{r->left, r->top};
        RECT dr = {p.x, p.y, p.x + (r->right - r->left), p.y + (r->bottom - r->top)};

        /* GPU-side paths first (StretchRect also resolves multisampling). */
        if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_DEFAULT &&
            SUCCEEDED(d9->StretchRect(src, r, dst, &dr, D3DTEXF_NONE)))
            continue;
        if (sd.Pool == D3DPOOL_SYSTEMMEM && dd.Pool == D3DPOOL_DEFAULT &&
            SUCCEEDED(d9->UpdateSurface(src, r, dst, &p)))
            continue;

        /* CPU path: obtain a lockable source. */
        if (!readable) {
            if (q8_surface_lockable(&sd) || (sd.Usage & D3DUSAGE_DYNAMIC)) {
                /* Lockable: not default pool, or a dynamic texture level. */
                readable = src;
                readable->AddRef();
            } else {
                hr = q8_read_target(d9, src, &sd, &readable);
                if (FAILED(hr))
                    break;
            }
        }

        if (dst_lockable) {
            hr = q8_lock_copy(readable, r, dst, &p, sd.Format);
            continue;
        }
        if (FAILED(q8_lock_copy(readable, r, dst, &p, sd.Format))) {
            /* Non-lockable default-pool destination: stage then upload. */
            if (!staging) {
                hr = d9->CreateOffscreenPlainSurface(dd.Width, dd.Height, dd.Format, D3DPOOL_SYSTEMMEM, &staging,
                                                     NULL);
                if (FAILED(hr))
                    break;
            }
            hr = q8_lock_copy(readable, r, staging, &p, sd.Format);
            if (SUCCEEDED(hr))
                hr = d9->UpdateSurface(staging, &dr, dst, &p);
        }
    }
    if (readable)
        readable->Release();
    if (staging)
        staging->Release();
    if (FAILED(hr))
        q8_log("qd3d8: CopyRects failed hr=0x%08lx (src pool %d ms %d, dst pool %d ms %d)", (unsigned long)hr,
               (int)sd.Pool, (int)sd.MultiSampleType, (int)dd.Pool, (int)dd.MultiSampleType);
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::UpdateTexture(d8::IDirect3DBaseTexture8 *src, d8::IDirect3DBaseTexture8 *dst)
{
    IDirect3DBaseTexture9 *s = NULL, *d = NULL;
    q8_texture(src, &s);
    q8_texture(dst, &d);
    if (!s || !d)
        return D3DERR_INVALIDCALL;
    return d9->UpdateTexture(s, d);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetFrontBuffer(d8::IDirect3DSurface8 *dst)
{
    Q8Surface *s = q8_find_surface(dst);
    if (!s)
        return D3DERR_INVALIDCALL;
    return d9->GetFrontBufferData(0, s->d9);
}

/* Render target and depth stencil are not state-block state: they always
 * change the device.  Native rules (measured): both must belong to this
 * device and have the right usage, and the depth stencil must be at least
 * as large as the render target it is used with (the new one, or the
 * current one when rt8 is NULL) with the same multisampling; a failure
 * changes nothing. */
HRESULT STDMETHODCALLTYPE Q8Device::SetRenderTarget(d8::IDirect3DSurface8 *rt8, d8::IDirect3DSurface8 *ds8)
{
    Q8DevLock lock(this);
    Q8Surface *r = NULL, *z = NULL;
    if (rt8) {
        r = q8_find_surface(rt8);
        if (!r || r->device != this || !(r->desc9.Usage & D3DUSAGE_RENDERTARGET))
            return D3DERR_INVALIDCALL;
    }
    if (ds8) {
        z = q8_find_surface(ds8);
        if (!z || z->device != this || !(z->desc9.Usage & D3DUSAGE_DEPTHSTENCIL))
            return D3DERR_INVALIDCALL;
        UINT w, h;
        D3DMULTISAMPLE_TYPE ms;
        if (r) {
            w = r->desc9.Width;
            h = r->desc9.Height;
            ms = r->desc9.MultiSampleType;
        } else {
            target_size(&w, &h, &ms);
        }
        if (z->desc9.Width < w || z->desc9.Height < h || z->desc9.MultiSampleType != ms)
            return D3DERR_INVALIDCALL;
    }
    /* D3D8: a NULL render target keeps the current one; a NULL depth
     * surface disables depth buffering. */
    if (r) {
        HRESULT hr = d9->SetRenderTarget(0, r->d9);
        if (FAILED(hr))
            return hr;
        q8_slot_set(&rt, r);
    }
    HRESULT hr = d9->SetDepthStencilSurface(z ? z->d9 : NULL);
    if (FAILED(hr))
        return hr;
    ds_known = true;
    q8_slot_set(&ds, z);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetRenderTarget(d8::IDirect3DSurface8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    Q8DevLock lock(this);
    Q8Object *o = q8_slot_ref(&rt);
    if (o) {
        *out = q8_iface<d8::IDirect3DSurface8>(o);
        return D3D_OK;
    }
    /* The implicit back buffer: the device owns it; it stays in the slot. */
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->GetRenderTarget(0, &s);
    if (FAILED(hr))
        return hr;
    Q8Surface *w = q8_implicit_surface(this, &owned, s);
    if (!w)
        return E_OUTOFMEMORY;
    q8_slot_set(&rt, w);
    *out = w;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetDepthStencilSurface(d8::IDirect3DSurface8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    Q8DevLock lock(this);
    if (ds_known) {
        Q8Object *o = q8_slot_ref(&ds);
        if (!o)
            return D3DERR_NOTFOUND;
        *out = q8_iface<d8::IDirect3DSurface8>(o);
        return D3D_OK;
    }
    /* The automatic depth stencil, if any: the device owns it; it stays in
     * the slot. */
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->GetDepthStencilSurface(&s);
    if (FAILED(hr) && hr != D3DERR_NOTFOUND)
        return hr;
    Q8Surface *w = NULL;
    if (SUCCEEDED(hr) && s) {
        w = q8_implicit_surface(this, &owned, s);
        if (!w)
            return E_OUTOFMEMORY;
    }
    ds_known = true;
    q8_slot_set(&ds, w);
    if (!w)
        return D3DERR_NOTFOUND;
    *out = w;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Fixed-function state                                                      */

HRESULT STDMETHODCALLTYPE Q8Device::BeginScene()
{
    return d9->BeginScene();
}

HRESULT STDMETHODCALLTYPE Q8Device::EndScene()
{
    return d9->EndScene();
}

HRESULT STDMETHODCALLTYPE Q8Device::Clear(DWORD count, const d8::D3DRECT *rects, DWORD flags, d8::D3DCOLOR color,
                                          float z, DWORD stencil)
{
    return d9->Clear(count, (const D3DRECT *)rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetTransform(d8::D3DTRANSFORMSTATETYPE state, const d8::D3DMATRIX *m)
{
    return d9->SetTransform((D3DTRANSFORMSTATETYPE)state, (const D3DMATRIX *)m);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetTransform(d8::D3DTRANSFORMSTATETYPE state, d8::D3DMATRIX *m)
{
    return d9->GetTransform((D3DTRANSFORMSTATETYPE)state, (D3DMATRIX *)m);
}

HRESULT STDMETHODCALLTYPE Q8Device::MultiplyTransform(d8::D3DTRANSFORMSTATETYPE state, const d8::D3DMATRIX *m)
{
    return d9->MultiplyTransform((D3DTRANSFORMSTATETYPE)state, (const D3DMATRIX *)m);
}

/* Native (measured): outside state block recording, a viewport must lie
 * inside the current render target. */
HRESULT STDMETHODCALLTYPE Q8Device::SetViewport(const d8::D3DVIEWPORT8 *vp)
{
    if (!vp)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    if (!recording) {
        UINT w, h;
        D3DMULTISAMPLE_TYPE ms;
        target_size(&w, &h, &ms);
        if ((UINT64)vp->X + vp->Width > w || (UINT64)vp->Y + vp->Height > h)
            return D3DERR_INVALIDCALL;
    }
    return d9->SetViewport((const D3DVIEWPORT9 *)vp);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetViewport(d8::D3DVIEWPORT8 *vp)
{
    return d9->GetViewport((D3DVIEWPORT9 *)vp);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetMaterial(const d8::D3DMATERIAL8 *m)
{
    return d9->SetMaterial((const D3DMATERIAL9 *)m);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetMaterial(d8::D3DMATERIAL8 *m)
{
    return d9->GetMaterial((D3DMATERIAL9 *)m);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetLight(DWORD index, const d8::D3DLIGHT8 *l)
{
    return d9->SetLight(index, (const D3DLIGHT9 *)l);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetLight(DWORD index, d8::D3DLIGHT8 *l)
{
    return d9->GetLight(index, (D3DLIGHT9 *)l);
}

HRESULT STDMETHODCALLTYPE Q8Device::LightEnable(DWORD index, WINBOOL enable)
{
    return d9->LightEnable(index, enable);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetLightEnable(DWORD index, WINBOOL *enable)
{
    return d9->GetLightEnable(index, enable);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetClipPlane(DWORD index, const float *plane)
{
    return d9->SetClipPlane(index, plane);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetClipPlane(DWORD index, float *plane)
{
    return d9->GetClipPlane(index, plane);
}

/* The render states D3D8 defines (d3d8types.h), one bit per state 0-255.
 * Native D3D8 accepts and ignores any other number below 256, so D3D8
 * applications cannot reach states D3D9 added. */
static const DWORD q8_rs_valid[8] = {
    0x7fd9c780u, /*   0- 31: 7-10, 14-16, 19-20, 22-30 */
    0x1ff1817cu, /*  32- 63: 34-38, 40, 47-48, 52-60 */
    0, 0,        /*  64-127 */
    0xff9efbffu, /* 128-159: 128-137, 139-143, 145-148, 151-159 */
    0x00003dffu, /* 160-191: 160-168, 170-173 */
    0, 0,        /* 192-255 */
};

static bool q8_rs_known(DWORD state)
{
    return state < 256 && (q8_rs_valid[state >> 5] & (1u << (state & 31)));
}

/*
 * Render states that changed between D3D8 and D3D9:
 *   LINEPATTERN (10)              removed; value kept for GetRenderState
 *   ZVISIBLE (30)                 never supported; value kept
 *   EDGEANTIALIAS (40)            -> D3DRS_ANTIALIASEDLINEENABLE
 *   ZBIAS (47)                    -> D3DRS_DEPTHBIAS (float, scaled); value kept
 *   SOFTWAREVERTEXPROCESSING (153)-> SetSoftwareVertexProcessing (always D3D_OK,
 *                                    as D3D8 render states do not fail)
 *   PATCHSEGMENTS (164)           -> SetNPatchMode; value kept
 * POSITIONORDER/NORMALORDER keep their numbers as POSITIONDEGREE/NORMALDEGREE.
 * Every other D3D8 state has the same number and encoding in D3D9.
 */
HRESULT STDMETHODCALLTYPE Q8Device::SetRenderState(d8::D3DRENDERSTATETYPE state, DWORD value)
{
    switch (state) {
    case d8::D3DRS_LINEPATTERN:
        rs_linepattern = value;
        return D3D_OK;
    case d8::D3DRS_ZVISIBLE:
        rs_zvisible = value;
        return D3D_OK;
    case d8::D3DRS_EDGEANTIALIAS:
        return d9->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, value);
    case d8::D3DRS_ZBIAS:
        rs_zbias = value;
        return d9->SetRenderState(D3DRS_DEPTHBIAS, q8_f2dw((float)value * Q8_ZBIAS_SCALE));
    case d8::D3DRS_SOFTWAREVERTEXPROCESSING:
        d9->SetSoftwareVertexProcessing(value ? TRUE : FALSE);
        return D3D_OK;
    case d8::D3DRS_PATCHSEGMENTS:
        rs_patchsegments = value;
        d9->SetNPatchMode(q8_dw2f(value)); /* fails without N-patch support; D3D8 ignores it too */
        return D3D_OK;
    default:
        if (!q8_rs_known(state))
            return D3D_OK;
        return d9->SetRenderState((D3DRENDERSTATETYPE)state, value);
    }
}

HRESULT STDMETHODCALLTYPE Q8Device::GetRenderState(d8::D3DRENDERSTATETYPE state, DWORD *value)
{
    if (!value)
        return D3DERR_INVALIDCALL;
    switch (state) {
    case d8::D3DRS_LINEPATTERN:
        *value = rs_linepattern;
        return D3D_OK;
    case d8::D3DRS_ZVISIBLE:
        *value = rs_zvisible;
        return D3D_OK;
    case d8::D3DRS_EDGEANTIALIAS:
        return d9->GetRenderState(D3DRS_ANTIALIASEDLINEENABLE, value);
    case d8::D3DRS_ZBIAS:
        *value = rs_zbias;
        return D3D_OK;
    case d8::D3DRS_SOFTWAREVERTEXPROCESSING:
        *value = d9->GetSoftwareVertexProcessing();
        return D3D_OK;
    case d8::D3DRS_PATCHSEGMENTS:
        *value = rs_patchsegments;
        return D3D_OK;
    default:
        /* Native: numbers past 255 are invalid, other unknown ones answer
         * D3D_OK without a value. */
        if ((DWORD)state >= 256)
            return D3DERR_INVALIDCALL;
        if (!q8_rs_known(state))
            return D3D_OK;
        return d9->GetRenderState((D3DRENDERSTATETYPE)state, value);
    }
}

/* ------------------------------------------------------------------------ */
/* State blocks                                                              */

HRESULT STDMETHODCALLTYPE Q8Device::BeginStateBlock()
{
    Q8DevLock lock(this);
    if (recording)
        return D3DERR_INVALIDCALL;
    Q8StateBlock *sb = (Q8StateBlock *)calloc(1, sizeof(*sb));
    if (!sb)
        return E_OUTOFMEMORY;
    HRESULT hr = d9->BeginStateBlock();
    if (FAILED(hr)) {
        free(sb);
        return hr;
    }
    recording = sb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::EndStateBlock(DWORD *token)
{
    Q8DevLock lock(this);
    if (!token || !recording)
        return D3DERR_INVALIDCALL;
    Q8StateBlock *sb = recording;
    recording = NULL;
    HRESULT hr = d9->EndStateBlock(&sb->sb9);
    if (FAILED(hr)) {
        sb->sb9 = NULL;
        q8_sb_free(sb);
        return hr;
    }
    UINT idx = q8_table_add(&stateblocks, sb);
    if (!idx) {
        q8_sb_free(sb);
        return E_OUTOFMEMORY;
    }
    *token = idx;
    return D3D_OK;
}

/* Native (measured): applying or capturing fails while a recording is open;
 * applying a token that names no state block (0, a deleted one) does
 * nothing and succeeds, capturing one fails. */
HRESULT STDMETHODCALLTYPE Q8Device::ApplyStateBlock(DWORD token)
{
    Q8DevLock lock(this);
    if (recording)
        return D3DERR_INVALIDCALL;
    Q8StateBlock *sb = (Q8StateBlock *)q8_table_get(&stateblocks, token);
    if (!sb)
        return D3D_OK;
    HRESULT hr = sb->sb9->Apply();
    if (FAILED(hr))
        return hr;
    if (sb->has_vs)
        cur_vs = sb->vs;
    if (sb->has_ps)
        cur_ps = sb->ps;
    q8_binds_apply(sb, &b);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::CaptureStateBlock(DWORD token)
{
    Q8DevLock lock(this);
    if (recording)
        return D3DERR_INVALIDCALL;
    Q8StateBlock *sb = (Q8StateBlock *)q8_table_get(&stateblocks, token);
    if (!sb)
        return D3DERR_INVALIDCALL;
    HRESULT hr = sb->sb9->Capture();
    if (FAILED(hr))
        return hr;
    if (sb->has_vs)
        sb->vs = cur_vs;
    if (sb->has_ps)
        sb->ps = cur_ps;
    q8_binds_capture(sb, &b);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::DeleteStateBlock(DWORD token)
{
    Q8DevLock lock(this);
    Q8StateBlock *sb = (Q8StateBlock *)q8_table_get(&stateblocks, token);
    if (!sb)
        return D3DERR_INVALIDCALL;
    q8_sb_free(sb);
    q8_table_clear_slot(&stateblocks, token);
    return D3D_OK;
}

/* Textures, streams and indices belong to D3DSBT_ALL only (measured: pixel
 * and vertex state blocks keep none of them alive). */
HRESULT STDMETHODCALLTYPE Q8Device::CreateStateBlock(d8::D3DSTATEBLOCKTYPE type, DWORD *token)
{
    Q8DevLock lock(this);
    if (!token || recording)
        return D3DERR_INVALIDCALL;
    if (type != d8::D3DSBT_ALL && type != d8::D3DSBT_PIXELSTATE && type != d8::D3DSBT_VERTEXSTATE)
        return D3DERR_INVALIDCALL;
    Q8StateBlock *sb = (Q8StateBlock *)calloc(1, sizeof(*sb));
    if (!sb)
        return E_OUTOFMEMORY;
    HRESULT hr = d9->CreateStateBlock((D3DSTATEBLOCKTYPE)type, &sb->sb9);
    if (FAILED(hr)) {
        free(sb);
        return hr;
    }
    sb->has_vs = type == d8::D3DSBT_ALL || type == d8::D3DSBT_VERTEXSTATE;
    sb->has_ps = type == d8::D3DSBT_ALL || type == d8::D3DSBT_PIXELSTATE;
    sb->vs = cur_vs;
    sb->ps = cur_ps;
    if (type == d8::D3DSBT_ALL) {
        sb->tex_mask = (1u << Q8_TEX_SLOTS) - 1u;
        sb->stream_mask = (1u << Q8_STREAM_SLOTS) - 1u;
        sb->has_indices = true;
        q8_binds_capture(sb, &b);
    }
    UINT idx = q8_table_add(&stateblocks, sb);
    if (!idx) {
        q8_sb_free(sb);
        return E_OUTOFMEMORY;
    }
    *token = idx;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetClipStatus(const d8::D3DCLIPSTATUS8 *cs)
{
    return d9->SetClipStatus((const D3DCLIPSTATUS9 *)cs);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetClipStatus(d8::D3DCLIPSTATUS8 *cs)
{
    return d9->GetClipStatus((D3DCLIPSTATUS9 *)cs);
}

/* ------------------------------------------------------------------------ */
/* Textures and texture stage / sampler split                                */

HRESULT STDMETHODCALLTYPE Q8Device::GetTexture(DWORD stage, d8::IDirect3DBaseTexture8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (stage >= Q8_TEX_SLOTS)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    *out = q8_iface<d8::IDirect3DBaseTexture8>(q8_slot_ref(&b.tex[stage]));
    return D3D_OK;
}

/* The texture is found by pointer, never called (applications hand in
 * objects with a damaged vtable and expect success).  Like the native
 * runtime, a pointer that is no texture of ours sets no texture. */
HRESULT STDMETHODCALLTYPE Q8Device::SetTexture(DWORD stage, d8::IDirect3DBaseTexture8 *tex)
{
    if (stage >= Q8_TEX_SLOTS)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    IDirect3DBaseTexture9 *t9 = NULL;
    Q8Object *o = q8_texture(tex, &t9);
    if (tex && !o)
        q8_log("qd3d8: SetTexture(%lu, %p): not a texture of this runtime; unbinding", (unsigned long)stage,
               (void *)tex);
    HRESULT hr = d9->SetTexture(stage, t9);
    if (FAILED(hr))
        return hr;
    if (recording)
        recording->tex_mask |= 1u << stage;
    q8_slot_set(&(recording ? recording->b : b).tex[stage], o);
    return D3D_OK;
}

/* D3D8 texture stage states that became D3D9 sampler states. */
static D3DSAMPLERSTATETYPE q8_sampler_state(d8::D3DTEXTURESTAGESTATETYPE type)
{
    switch (type) {
    case d8::D3DTSS_ADDRESSU: return D3DSAMP_ADDRESSU;
    case d8::D3DTSS_ADDRESSV: return D3DSAMP_ADDRESSV;
    case d8::D3DTSS_ADDRESSW: return D3DSAMP_ADDRESSW;
    case d8::D3DTSS_BORDERCOLOR: return D3DSAMP_BORDERCOLOR;
    case d8::D3DTSS_MAGFILTER: return D3DSAMP_MAGFILTER;
    case d8::D3DTSS_MINFILTER: return D3DSAMP_MINFILTER;
    case d8::D3DTSS_MIPFILTER: return D3DSAMP_MIPFILTER;
    case d8::D3DTSS_MIPMAPLODBIAS: return D3DSAMP_MIPMAPLODBIAS;
    case d8::D3DTSS_MAXMIPLEVEL: return D3DSAMP_MAXMIPLEVEL;
    case d8::D3DTSS_MAXANISOTROPY: return D3DSAMP_MAXANISOTROPY;
    default: return (D3DSAMPLERSTATETYPE)0;
    }
}

/* Native: stages past the eighth are accepted and ignored. */
HRESULT STDMETHODCALLTYPE Q8Device::GetTextureStageState(DWORD stage, d8::D3DTEXTURESTAGESTATETYPE type, DWORD *value)
{
    if (!value)
        return D3DERR_INVALIDCALL;
    if (stage >= QD3D8_MAX_STAGES)
        return D3D_OK;
    D3DSAMPLERSTATETYPE s = q8_sampler_state(type);
    if (s)
        return d9->GetSamplerState(stage, s, value);
    return d9->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, value);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetTextureStageState(DWORD stage, d8::D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage >= QD3D8_MAX_STAGES)
        return D3D_OK;
    D3DSAMPLERSTATETYPE s = q8_sampler_state(type);
    if (s) {
        /* D3D8 FLATCUBIC (4) and GAUSSIANCUBIC (5) magnification filters have
         * no D3D9 equivalent; both degrade to bilinear. */
        if ((s == D3DSAMP_MAGFILTER || s == D3DSAMP_MINFILTER) && (value == 4 || value == 5))
            value = D3DTEXF_LINEAR;
        return d9->SetSamplerState(stage, s, value);
    }
    return d9->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, value);
}

/* The native D3D8 runtime validates every state in one pass, filters and
 * texture operations included (measured on the stock d3d8.dll: invalid
 * filters, an unknown colour operation and a disabled alpha stage all
 * pass); the host renders whatever is set. */
HRESULT STDMETHODCALLTYPE Q8Device::ValidateDevice(DWORD *passes)
{
    if (!passes)
        return D3DERR_INVALIDCALL;
    *passes = 1;
    return D3D_OK;
}

/* Native (measured): the buffer is cleared; identifiers below
 * D3DDEVINFOID_VCACHE (4) fail, the others report no data (S_FALSE). */
HRESULT STDMETHODCALLTYPE Q8Device::GetInfo(DWORD id, void *data, DWORD size)
{
    if (!data || !size)
        return D3DERR_INVALIDCALL;
    memset(data, 0, size);
    return id < 4 ? E_FAIL : S_FALSE;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetPaletteEntries(UINT palette, const PALETTEENTRY *entries)
{
    return d9->SetPaletteEntries(palette, entries);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetPaletteEntries(UINT palette, PALETTEENTRY *entries)
{
    return d9->GetPaletteEntries(palette, entries);
}

HRESULT STDMETHODCALLTYPE Q8Device::SetCurrentTexturePalette(UINT palette)
{
    return d9->SetCurrentTexturePalette(palette);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetCurrentTexturePalette(UINT *palette)
{
    return d9->GetCurrentTexturePalette(palette);
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */

HRESULT STDMETHODCALLTYPE Q8Device::DrawPrimitive(d8::D3DPRIMITIVETYPE type, UINT start, UINT count)
{
    Q8DevLock lock(this);
    if (sysmem_texture_bound())
        return E_FAIL;
    return d9->DrawPrimitive((D3DPRIMITIVETYPE)type, start, count);
}

HRESULT STDMETHODCALLTYPE Q8Device::DrawIndexedPrimitive(d8::D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                                                         UINT start_index, UINT count)
{
    Q8DevLock lock(this);
    if (sysmem_texture_bound())
        return E_FAIL;
    /* D3D8 supplies the base vertex index through SetIndices. */
    return d9->DrawIndexedPrimitive((D3DPRIMITIVETYPE)type, (INT)b.base, min_index, num_vertices, start_index,
                                    count);
}

/* Like the native runtime (and D3D9), the user-pointer draws unbind stream 0
 * (stride 0) and DrawIndexedPrimitiveUP also the indices (base 0); a buffer
 * the application released while bound goes with that. */
HRESULT STDMETHODCALLTYPE Q8Device::DrawPrimitiveUP(d8::D3DPRIMITIVETYPE type, UINT count, const void *data,
                                                    UINT stride)
{
    Q8DevLock lock(this);
    if (sysmem_texture_bound())
        return E_FAIL;
    HRESULT hr = d9->DrawPrimitiveUP((D3DPRIMITIVETYPE)type, count, data, stride);
    if (SUCCEEDED(hr)) {
        q8_slot_set(&b.stream[0], NULL);
        b.stride[0] = 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::DrawIndexedPrimitiveUP(d8::D3DPRIMITIVETYPE type, UINT min_index,
                                                           UINT num_vertices, UINT count, const void *indices,
                                                           d8::D3DFORMAT index_fmt, const void *data, UINT stride)
{
    Q8DevLock lock(this);
    if (sysmem_texture_bound())
        return E_FAIL;
    HRESULT hr = d9->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)type, min_index, num_vertices, count, indices,
                                            (D3DFORMAT)index_fmt, data, stride);
    if (SUCCEEDED(hr)) {
        q8_slot_set(&b.stream[0], NULL);
        b.stride[0] = 0;
        q8_slot_set(&b.indices, NULL);
        b.base = 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::ProcessVertices(UINT src_start, UINT dst_index, UINT count,
                                                    d8::IDirect3DVertexBuffer8 *dst, DWORD flags)
{
    Q8VertexBuffer *w = q8_find_vb(dst);
    if (!w)
        return D3DERR_INVALIDCALL;
    return d9->ProcessVertices(src_start, dst_index, count, w->d9, NULL, flags);
}

/* ------------------------------------------------------------------------ */
/* Vertex shaders                                                            */

HRESULT Q8Device::apply_vertex_shader(DWORD h)
{
    if (!(h & 1)) {
        /* Fixed-function handle: the value is an FVF code. */
        HRESULT hr = d9->SetVertexShader(NULL);
        if (SUCCEEDED(hr))
            hr = d9->SetFVF(h);
        return hr;
    }
    Q8VertexShader *vs = vs_from_handle(h);
    if (!vs)
        return D3DERR_INVALIDCALL;
    HRESULT hr = d9->SetVertexDeclaration(vs->decl9);
    if (SUCCEEDED(hr))
        hr = d9->SetVertexShader(vs->vs9);
    for (UINT i = 0; SUCCEEDED(hr) && i + 2 <= vs->consts_count;) {
        DWORD reg = vs->consts[i], cnt = vs->consts[i + 1];
        if (i + 2 + cnt * 4 > vs->consts_count)
            break;
        if (cnt)
            hr = d9->SetVertexShaderConstantF(reg, (const float *)(vs->consts + i + 2), cnt);
        i += 2 + cnt * 4;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Device::CreateVertexShader(const DWORD *decl, const DWORD *func, DWORD *handle,
                                                       DWORD usage)
{
    Q8DevLock lock(this);
    if (!handle || !decl)
        return D3DERR_INVALIDCALL;
    Q8VertexShader *vs = NULL;
    HRESULT hr = q8_create_vertex_shader(d9, decl, func, vs_consts, &vs);
    if (FAILED(hr))
        return hr;
    UINT idx = q8_table_add(&vshaders, vs);
    if (!idx || idx > 0x7fffffffu) {
        q8_destroy_vertex_shader(vs);
        return E_OUTOFMEMORY;
    }
    *handle = ((DWORD)idx << 1) | 1u;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetVertexShader(DWORD handle)
{
    Q8DevLock lock(this);
    if ((handle & 1) && !vs_from_handle(handle))
        return D3DERR_INVALIDCALL;
    HRESULT hr = apply_vertex_shader(handle);
    if (FAILED(hr))
        return hr;
    if (recording) {
        recording->has_vs = true;
        recording->vs = handle;
    } else {
        cur_vs = handle;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetVertexShader(DWORD *handle)
{
    Q8DevLock lock(this);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *handle = cur_vs;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::DeleteVertexShader(DWORD handle)
{
    Q8DevLock lock(this);
    Q8VertexShader *vs = vs_from_handle(handle);
    if (!vs)
        return D3DERR_INVALIDCALL;
    if (cur_vs == handle) {
        d9->SetVertexShader(NULL);
        d9->SetFVF(0);
        cur_vs = 0;
    }
    q8_table_clear_slot(&vshaders, handle >> 1);
    q8_destroy_vertex_shader(vs);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetVertexShaderConstant(DWORD reg, const void *data, DWORD count)
{
    if (!data)
        return D3DERR_INVALIDCALL;
    return d9->SetVertexShaderConstantF(reg, (const float *)data, count);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetVertexShaderConstant(DWORD reg, void *data, DWORD count)
{
    if (!data)
        return D3DERR_INVALIDCALL;
    return d9->GetVertexShaderConstantF(reg, (float *)data, count);
}

/* Native (measured): a buffer too small fails with D3DERR_INVALIDCALL and
 * leaves *size alone. */
static HRESULT q8_return_tokens(const DWORD *tokens, UINT count, void *data, DWORD *size)
{
    if (!size)
        return D3DERR_INVALIDCALL;
    DWORD bytes = count * sizeof(DWORD);
    if (!data) {
        *size = bytes;
        return D3D_OK;
    }
    if (*size < bytes)
        return D3DERR_INVALIDCALL;
    if (bytes)
        memcpy(data, tokens, bytes);
    *size = bytes;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetVertexShaderDeclaration(DWORD handle, void *data, DWORD *size)
{
    Q8DevLock lock(this);
    Q8VertexShader *vs = vs_from_handle(handle);
    if (!vs)
        return D3DERR_INVALIDCALL;
    return q8_return_tokens(vs->decl, vs->decl_count, data, size);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetVertexShaderFunction(DWORD handle, void *data, DWORD *size)
{
    Q8DevLock lock(this);
    Q8VertexShader *vs = vs_from_handle(handle);
    if (!vs)
        return D3DERR_INVALIDCALL;
    return q8_return_tokens(vs->func, vs->func_count, data, size);
}

/* ------------------------------------------------------------------------ */
/* Streams and indices                                                       */

HRESULT STDMETHODCALLTYPE Q8Device::SetStreamSource(UINT stream, d8::IDirect3DVertexBuffer8 *vb, UINT stride)
{
    if (stream >= Q8_STREAM_SLOTS)
        return D3DERR_INVALIDCALL;
    Q8VertexBuffer *w = q8_find_vb(vb);
    if (vb && !w)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    HRESULT hr = d9->SetStreamSource(stream, w ? w->d9 : NULL, 0, stride);
    if (FAILED(hr))
        return hr;
    if (recording)
        recording->stream_mask |= 1u << stream;
    q8_set_stream(recording ? &recording->b : &b, stream, w, stride);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetStreamSource(UINT stream, d8::IDirect3DVertexBuffer8 **vb, UINT *stride)
{
    if (!vb || !stride)
        return D3DERR_INVALIDCALL;
    *vb = NULL;
    if (stream >= Q8_STREAM_SLOTS)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    *vb = q8_iface<d8::IDirect3DVertexBuffer8>(q8_slot_ref(&b.stream[stream]));
    *stride = b.stride[stream];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetIndices(d8::IDirect3DIndexBuffer8 *ib, UINT base)
{
    Q8IndexBuffer *w = q8_find_ib(ib);
    if (ib && !w)
        return D3DERR_INVALIDCALL;
    Q8DevLock lock(this);
    HRESULT hr = d9->SetIndices(w ? w->d9 : NULL);
    if (FAILED(hr))
        return hr;
    if (recording)
        recording->has_indices = true;
    q8_set_indices(recording ? &recording->b : &b, w, base);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetIndices(d8::IDirect3DIndexBuffer8 **ib, UINT *base)
{
    Q8DevLock lock(this);
    if (!ib || !base)
        return D3DERR_INVALIDCALL;
    *ib = q8_iface<d8::IDirect3DIndexBuffer8>(q8_slot_ref(&b.indices));
    *base = b.base;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Pixel shaders                                                             */

HRESULT STDMETHODCALLTYPE Q8Device::CreatePixelShader(const DWORD *func, DWORD *handle)
{
    Q8DevLock lock(this);
    if (!handle || !func)
        return D3DERR_INVALIDCALL;
    Q8PixelShader *ps = NULL;
    HRESULT hr = q8_create_pixel_shader(d9, func, ps_version, &ps);
    if (FAILED(hr))
        return hr;
    UINT idx = q8_table_add(&pshaders, ps);
    if (!idx) {
        q8_destroy_pixel_shader(ps);
        return E_OUTOFMEMORY;
    }
    *handle = idx;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetPixelShader(DWORD handle)
{
    Q8DevLock lock(this);
    IDirect3DPixelShader9 *ps9 = NULL;
    if (handle) {
        Q8PixelShader *ps = ps_from_handle(handle);
        if (!ps)
            return D3DERR_INVALIDCALL;
        ps9 = ps->ps9;
    }
    HRESULT hr = d9->SetPixelShader(ps9);
    if (FAILED(hr))
        return hr;
    if (recording) {
        recording->has_ps = true;
        recording->ps = handle;
    } else {
        cur_ps = handle;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::GetPixelShader(DWORD *handle)
{
    Q8DevLock lock(this);
    if (!handle)
        return D3DERR_INVALIDCALL;
    *handle = cur_ps;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::DeletePixelShader(DWORD handle)
{
    Q8DevLock lock(this);
    Q8PixelShader *ps = ps_from_handle(handle);
    if (!ps)
        return D3DERR_INVALIDCALL;
    if (cur_ps == handle) {
        d9->SetPixelShader(NULL);
        cur_ps = 0;
    }
    q8_table_clear_slot(&pshaders, handle);
    q8_destroy_pixel_shader(ps);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Device::SetPixelShaderConstant(DWORD reg, const void *data, DWORD count)
{
    if (!data)
        return D3DERR_INVALIDCALL;
    return d9->SetPixelShaderConstantF(reg, (const float *)data, count);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetPixelShaderConstant(DWORD reg, void *data, DWORD count)
{
    if (!data)
        return D3DERR_INVALIDCALL;
    return d9->GetPixelShaderConstantF(reg, (float *)data, count);
}

HRESULT STDMETHODCALLTYPE Q8Device::GetPixelShaderFunction(DWORD handle, void *data, DWORD *size)
{
    Q8DevLock lock(this);
    Q8PixelShader *ps = ps_from_handle(handle);
    if (!ps)
        return D3DERR_INVALIDCALL;
    return q8_return_tokens(ps->func, ps->func_count, data, size);
}

/* ------------------------------------------------------------------------ */
/* Patches: D3D8 and D3D9 share the patch info layouts.                      */

HRESULT STDMETHODCALLTYPE Q8Device::DrawRectPatch(UINT handle, const float *segs, const d8::D3DRECTPATCH_INFO *info)
{
    return d9->DrawRectPatch(handle, segs, (const D3DRECTPATCH_INFO *)info);
}

HRESULT STDMETHODCALLTYPE Q8Device::DrawTriPatch(UINT handle, const float *segs, const d8::D3DTRIPATCH_INFO *info)
{
    return d9->DrawTriPatch(handle, segs, (const D3DTRIPATCH_INFO *)info);
}

HRESULT STDMETHODCALLTYPE Q8Device::DeletePatch(UINT handle)
{
    return d9->DeletePatch(handle);
}
