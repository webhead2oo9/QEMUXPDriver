/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: Direct3D 8 implemented on top of Direct3D 9.
 *
 * The MinGW d3d8 and d3d9 headers cannot share a translation unit in the
 * global namespace (they define the same type names with different
 * layouts).  The D3D9 headers are included normally and the D3D8 headers are
 * included inside namespace d8, so every D3D8 type is spelled d8::X.  Both
 * header sets define identical macro values where they overlap; the build
 * runs with -Werror, so any divergent macro redefinition would fail loudly.
 */
#ifndef QD3D8_H
#define QD3D8_H

#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <d3d9.h>

namespace d8 {
#include <d3d8.h>
}

/* Present in the DirectX 8.1 SDK headers but not in MinGW's d3d8types.h. */
#ifndef D3DVSD_SKIPBYTES
#define D3DVSD_SKIPBYTES(n) \
    (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAMDATA) | 0x18000000u | ((DWORD)(n) << D3DVSD_SKIPCOUNTSHIFT))
#endif

#define QD3D8_MAX_STAGES 8
#define QD3D8_MAX_VS_TOKENS 65536u  /* bound on vertex/pixel shader token scans */
#define QD3D8_MAX_DECL_TOKENS 4096u /* bound on D3D8 declaration token scans */

/* Interface IDs (values from the DirectX 8 SDK). */
extern const GUID QIID_IDirect3D8;
extern const GUID QIID_IDirect3DDevice8;
extern const GUID QIID_IDirect3DResource8;
extern const GUID QIID_IDirect3DBaseTexture8;
extern const GUID QIID_IDirect3DTexture8;
extern const GUID QIID_IDirect3DCubeTexture8;
extern const GUID QIID_IDirect3DVolumeTexture8;
extern const GUID QIID_IDirect3DVertexBuffer8;
extern const GUID QIID_IDirect3DIndexBuffer8;
extern const GUID QIID_IDirect3DSurface8;
extern const GUID QIID_IDirect3DVolume8;
extern const GUID QIID_IDirect3DSwapChain8;

extern CRITICAL_SECTION g_q8_lock;

void q8_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------------ */
/* Conversions                                                               */

void q8_convert_caps(const D3DCAPS9 *in, d8::D3DCAPS8 *out);
bool q8_convert_pp(const d8::D3DPRESENT_PARAMETERS *in, D3DPRESENT_PARAMETERS *out);
void q8_writeback_pp(const D3DPRESENT_PARAMETERS *in, d8::D3DPRESENT_PARAMETERS *out);
UINT q8_surface_size(D3DFORMAT fmt, UINT width, UINT height, UINT depth);
/* Bytes per block row unit: block width/height in pixels and bytes per block.
 * Returns false for formats that cannot be copied byte-wise. */
bool q8_format_blocks(D3DFORMAT fmt, UINT *bw, UINT *bh, UINT *bytes);

/* ------------------------------------------------------------------------ */
/* Simple growable pointer table (handles are 1-based indices).               */

struct Q8Table {
    void **items;
    UINT count;
    UINT capacity;
};
UINT q8_table_add(Q8Table *t, void *p); /* returns index+1, 0 on failure */
void *q8_table_get(const Q8Table *t, UINT index1);
void q8_table_clear_slot(Q8Table *t, UINT index1);
void q8_table_free(Q8Table *t);

/* ------------------------------------------------------------------------ */
/* Shaders                                                                   */

struct Q8VertexShader {
    DWORD *decl;          /* original D3D8 declaration tokens, incl. END */
    UINT decl_count;
    DWORD *func;          /* original D3D8 function tokens, incl. END; may be NULL */
    UINT func_count;
    IDirect3DVertexDeclaration9 *decl9;
    IDirect3DVertexShader9 *vs9; /* NULL: fixed function with declaration */
    DWORD *consts;        /* D3DVSD_CONST payload: groups of (reg, count, data...) */
    UINT consts_count;    /* DWORDs in consts */
};

struct Q8PixelShader {
    DWORD *func;
    UINT func_count;
    IDirect3DPixelShader9 *ps9;
};

/* Growable text for validator messages. */
struct Q8Text {
    char *buf;
    size_t len;
    size_t cap;
};
void q8_text_free(Q8Text *t);
void q8_text_add(Q8Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* What a shader is validated against. */
struct Q8ShaderLimits {
    DWORD max_version; /* highest version token allowed; ~0u: every supported version */
    UINT max_const;    /* vertex shaders: constant registers; ~0u: no limit */
    DWORD inputs;      /* vertex shaders: declared input registers; ~0u: no declaration */
};

/* The native D3D8 shader validator's rules (measured on the stock runtime,
 * which applies them in ValidateVertexShader/ValidatePixelShader and in
 * CreateVertexShader/CreatePixelShader): D3D_OK or E_FAIL, the native-style
 * messages appended to msg (may be NULL), the token count including END in
 * *length when the stream is well formed. */
HRESULT q8_check_vertex_shader(const DWORD *code, const Q8ShaderLimits *lim, Q8Text *msg, UINT *length);
HRESULT q8_check_pixel_shader(const DWORD *code, const Q8ShaderLimits *lim, Q8Text *msg, UINT *length);
/* Input registers (bits 0-16) a well-formed declaration loads. */
bool q8_decl_inputs(const DWORD *decl, DWORD *regs);

/* max_const: constant registers the function may use; max_version: highest
 * pixel shader version the device takes. */
HRESULT q8_create_vertex_shader(IDirect3DDevice9 *dev, const DWORD *decl, const DWORD *func, UINT max_const,
                                Q8VertexShader **out);
void q8_destroy_vertex_shader(Q8VertexShader *vs);
HRESULT q8_create_pixel_shader(IDirect3DDevice9 *dev, const DWORD *func, DWORD max_version, Q8PixelShader **out);
void q8_destroy_pixel_shader(Q8PixelShader *ps);

/* ------------------------------------------------------------------------ */
/* Object classes                                                            */

class Q8Device;

class Q8Direct3D final : public d8::IDirect3D8 {
public:
    LONG refs;
    IDirect3D9 *d3d9;

    explicit Q8Direct3D(IDirect3D9 *d);

    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(RegisterSoftwareDevice)(void *init);
    STDMETHOD_(UINT, GetAdapterCount)();
    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, d8::D3DADAPTER_IDENTIFIER8 *id);
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter);
    STDMETHOD(EnumAdapterModes)(UINT adapter, UINT mode, d8::D3DDISPLAYMODE *out);
    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, d8::D3DDISPLAYMODE *out);
    STDMETHOD(CheckDeviceType)(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT display, d8::D3DFORMAT bb, WINBOOL windowed);
    STDMETHOD(CheckDeviceFormat)(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT adapter_fmt, DWORD usage, d8::D3DRESOURCETYPE rtype, d8::D3DFORMAT fmt);
    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT fmt, WINBOOL windowed, d8::D3DMULTISAMPLE_TYPE ms);
    STDMETHOD(CheckDepthStencilMatch)(UINT adapter, d8::D3DDEVTYPE type, d8::D3DFORMAT adapter_fmt, d8::D3DFORMAT rt, d8::D3DFORMAT ds);
    STDMETHOD(GetDeviceCaps)(UINT adapter, d8::D3DDEVTYPE type, d8::D3DCAPS8 *caps);
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT adapter);
    STDMETHOD(CreateDevice)(UINT adapter, d8::D3DDEVTYPE type, HWND focus, DWORD flags,
                            d8::D3DPRESENT_PARAMETERS *pp, d8::IDirect3DDevice8 **out);
};

/* Kinds of wrapper the registry tells apart. */
enum : unsigned {
    Q8K_SURFACE = 1u << 0,
    Q8K_VOLUME = 1u << 1,
    Q8K_TEXTURE = 1u << 2,
    Q8K_CUBE = 1u << 3,
    Q8K_VOLTEX = 1u << 4,
    Q8K_VB = 1u << 5,
    Q8K_IB = 1u << 6,
    Q8K_SWAPCHAIN = 1u << 7,
};
#define Q8K_BASETEX (Q8K_TEXTURE | Q8K_CUBE | Q8K_VOLTEX)

template <class B> struct Q8KindOf;
template <> struct Q8KindOf<d8::IDirect3DSurface8> { static const unsigned value = Q8K_SURFACE; };
template <> struct Q8KindOf<d8::IDirect3DVolume8> { static const unsigned value = Q8K_VOLUME; };
template <> struct Q8KindOf<d8::IDirect3DTexture8> { static const unsigned value = Q8K_TEXTURE; };
template <> struct Q8KindOf<d8::IDirect3DCubeTexture8> { static const unsigned value = Q8K_CUBE; };
template <> struct Q8KindOf<d8::IDirect3DVolumeTexture8> { static const unsigned value = Q8K_VOLTEX; };
template <> struct Q8KindOf<d8::IDirect3DVertexBuffer8> { static const unsigned value = Q8K_VB; };
template <> struct Q8KindOf<d8::IDirect3DIndexBuffer8> { static const unsigned value = Q8K_IB; };
template <> struct Q8KindOf<d8::IDirect3DSwapChain8> { static const unsigned value = Q8K_SWAPCHAIN; };

/*
 * Object lifetime follows the native runtime (measured against the stock
 * 32-bit d3d8.dll; docs/design/d3d9-remoting.md, Direct3D 8):
 *  - binding an object (texture, stream, indices, render target, depth
 *    stencil, state block) leaves its public count alone;
 *  - an object the application releases while something still holds it
 *    stays alive with its identity and gives up its device reference at
 *    once; Get* or AddRef brings it back (count 1, device reference again);
 *  - level surfaces and volumes share their texture's count and life;
 *  - the device owns its back buffers and automatic depth stencil until
 *    Reset or its end, an additional swap chain its back buffers;
 *  - Release at count 0 does nothing.
 * refs is the public count and holds the internal one (slots, state blocks,
 * owners); the object is destroyed when both are zero.  A level keeps
 * neither and forwards to its container.  Every wrapper keeps one reference
 * on its D3D9 object for its whole life.  g_q8_lock guards refs, holds, all
 * slots and owner tables; destruction and D3D9 calls happen outside it.
 *
 * Identity: every wrapper is registered under the interface pointer the
 * application sees, and an interface the application passes in is mapped
 * back through that registry only (q8_lookup), never by calling it, so a
 * foreign or damaged object cannot be mistaken for a wrapper.
 */
class Q8Object {
public:
    LONG refs;
    LONG holds;
    Q8Device *device;
    Q8Object *container; /* level: the texture whose count and life it shares */
    const void *key;     /* the registered interface pointer; NULL until registered */
    IUnknown *obj9;      /* the wrapped D3D9 object */
    unsigned kind;       /* Q8K_* */
    DWORD pool;          /* D3D8 pool; levels report their container's */
    bool counted;        /* in device->default_objects */

    Q8Object(Q8Device *dev, Q8Object *cont, unsigned k, IUnknown *o9);
    virtual ~Q8Object();
    /* The interface pointer the application sees. */
    virtual ::IUnknown *iface() = 0;
    /* Releases the D3D9 reference(s) and frees the wrapper. */
    virtual void destroy() = 0;
    ULONG add_ref();
    ULONG release();
};

/* Registers o under its interface pointer with the given pool (levels: their
 * container's); false when out of memory.  Destruction unregisters it. */
bool q8_register(Q8Object *o, DWORD pool);
/* The registered wrapper behind interface pointer p when it is one of kinds,
 * else NULL.  No reference change, and *p is never touched. */
Q8Object *q8_lookup(const void *p, unsigned kinds);

/* Internal references; q8_unhold destroys the object once nothing
 * references it. */
void q8_hold(Q8Object *o);
void q8_unhold(Q8Object *o);
/* Points *slot at o, holding it, and lets go of what it held. */
void q8_slot_set(Q8Object **slot, Q8Object *o);
/* The object in *slot with one new application reference, or NULL. */
Q8Object *q8_slot_ref(Q8Object *const *slot);

template <class I>
static inline I *q8_iface(Q8Object *o)
{
    return o ? static_cast<I *>(o->iface()) : NULL;
}

/* Common wrapper base: Base is the D3D8 interface, D9 the wrapped object. */
template <class Base, class D9>
class Q8Child : public Base, public Q8Object {
public:
    D9 *d9;

    Q8Child(Q8Device *dev, D9 *obj, Q8Object *cont = NULL)
        : Q8Object(dev, cont, Q8KindOf<Base>::value, obj), d9(obj) {}
    ::IUnknown *iface() override { return static_cast<Base *>(this); }
    void destroy() override
    {
        d9->Release();
        delete this;
    }
    STDMETHOD_(ULONG, AddRef)() override { return add_ref(); }
    STDMETHOD_(ULONG, Release)() override { return release(); }
};

#define Q8_MAX_LEVELS 16u /* mip levels a level table covers (16384 texels need 15) */

/* Level wrappers of a texture, created on first request and destroyed with
 * it (index: level, cube maps face * Q8_MAX_LEVELS + level). */
struct Q8Levels {
    Q8Object **items;
    UINT count;
};
void q8_levels_destroy(Q8Levels *t);

template <class Base, class D9>
class Q8ResourceT : public Q8Child<Base, D9> {
public:
    Q8ResourceT(Q8Device *dev, D9 *obj) : Q8Child<Base, D9>(dev, obj) {}
    STDMETHOD(GetDevice)(d8::IDirect3DDevice8 **out);
    STDMETHOD(SetPrivateData)(REFGUID guid, const void *data, DWORD size, DWORD flags);
    STDMETHOD(GetPrivateData)(REFGUID guid, void *data, DWORD *size);
    STDMETHOD(FreePrivateData)(REFGUID guid);
    STDMETHOD_(DWORD, SetPriority)(DWORD p);
    STDMETHOD_(DWORD, GetPriority)();
    STDMETHOD_(void, PreLoad)();
    STDMETHOD_(d8::D3DRESOURCETYPE, GetType)();
};

template <class Base, class D9>
class Q8BaseTextureT : public Q8ResourceT<Base, D9> {
public:
    Q8Levels levels;
    Q8BaseTextureT(Q8Device *dev, D9 *obj) : Q8ResourceT<Base, D9>(dev, obj), levels() {}
    void destroy() override
    {
        q8_levels_destroy(&levels); /* their D3D9 references go first */
        this->d9->Release();
        delete this;
    }
    STDMETHOD_(DWORD, SetLOD)(DWORD lod);
    STDMETHOD_(DWORD, GetLOD)();
    STDMETHOD_(DWORD, GetLevelCount)();
};

/* Surface flags. */
#define Q8SF_IMAGE 1u   /* from CreateImageSurface: a SYSTEMMEM surface without usage */
#define Q8SF_CHECKED 2u /* the native lock rules of image, cube face and device surfaces:
                         * the rectangle is checked, a failed lock clears its output,
                         * unlocking an unlocked surface fails */

/* cont: the texture a level surface belongs to; NULL for device surfaces. */
class Q8Surface : public Q8Child<d8::IDirect3DSurface8, IDirect3DSurface9> {
public:
    D3DSURFACE_DESC desc9; /* the D3D9 description, read at creation */
    unsigned flags;        /* Q8SF_* */
    UINT sub;              /* cube faces: face * Q8_MAX_LEVELS + level */
    bool locked;           /* Q8SF_CHECKED surfaces that are not cube faces */

    Q8Surface(Q8Device *dev, IDirect3DSurface9 *s, Q8Object *cont = NULL);
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(GetDevice)(d8::IDirect3DDevice8 **out);
    STDMETHOD(SetPrivateData)(REFGUID guid, const void *data, DWORD size, DWORD flags);
    STDMETHOD(GetPrivateData)(REFGUID guid, void *data, DWORD *size);
    STDMETHOD(FreePrivateData)(REFGUID guid);
    STDMETHOD(GetContainer)(REFIID riid, void **out);
    STDMETHOD(GetDesc)(d8::D3DSURFACE_DESC *desc);
    STDMETHOD(LockRect)(d8::D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags);
    STDMETHOD(UnlockRect)();
};

class Q8Volume : public Q8Child<d8::IDirect3DVolume8, IDirect3DVolume9> {
public:
    Q8Volume(Q8Device *dev, IDirect3DVolume9 *v, Q8Object *cont = NULL) : Q8Child(dev, v, cont) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(GetDevice)(d8::IDirect3DDevice8 **out);
    STDMETHOD(SetPrivateData)(REFGUID guid, const void *data, DWORD size, DWORD flags);
    STDMETHOD(GetPrivateData)(REFGUID guid, void *data, DWORD *size);
    STDMETHOD(FreePrivateData)(REFGUID guid);
    STDMETHOD(GetContainer)(REFIID riid, void **out);
    STDMETHOD(GetDesc)(d8::D3DVOLUME_DESC *desc);
    STDMETHOD(LockBox)(d8::D3DLOCKED_BOX *lb, const d8::D3DBOX *box, DWORD flags);
    STDMETHOD(UnlockBox)();
};

class Q8Texture : public Q8BaseTextureT<d8::IDirect3DTexture8, IDirect3DTexture9> {
public:
    Q8Texture(Q8Device *dev, IDirect3DTexture9 *t) : Q8BaseTextureT(dev, t) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(GetLevelDesc)(UINT level, d8::D3DSURFACE_DESC *desc);
    STDMETHOD(GetSurfaceLevel)(UINT level, d8::IDirect3DSurface8 **out);
    STDMETHOD(LockRect)(UINT level, d8::D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags);
    STDMETHOD(UnlockRect)(UINT level);
    STDMETHOD(AddDirtyRect)(const RECT *rect);
};

class Q8CubeTexture : public Q8BaseTextureT<d8::IDirect3DCubeTexture8, IDirect3DCubeTexture9> {
public:
    UINT edge;                /* level 0 edge length */
    WORD locked[6];           /* per face: locked levels (bit per level) */
    Q8CubeTexture(Q8Device *dev, IDirect3DCubeTexture9 *t);
    /* The native lock rules shared by LockRect and the face surfaces. */
    HRESULT lock_face(UINT face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags);
    HRESULT unlock_face(UINT face, UINT level);
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(GetLevelDesc)(UINT level, d8::D3DSURFACE_DESC *desc);
    STDMETHOD(GetCubeMapSurface)(d8::D3DCUBEMAP_FACES face, UINT level, d8::IDirect3DSurface8 **out);
    STDMETHOD(LockRect)(d8::D3DCUBEMAP_FACES face, UINT level, d8::D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags);
    STDMETHOD(UnlockRect)(d8::D3DCUBEMAP_FACES face, UINT level);
    STDMETHOD(AddDirtyRect)(d8::D3DCUBEMAP_FACES face, const RECT *rect);
};

class Q8VolumeTexture : public Q8BaseTextureT<d8::IDirect3DVolumeTexture8, IDirect3DVolumeTexture9> {
public:
    Q8VolumeTexture(Q8Device *dev, IDirect3DVolumeTexture9 *t) : Q8BaseTextureT(dev, t) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(GetLevelDesc)(UINT level, d8::D3DVOLUME_DESC *desc);
    STDMETHOD(GetVolumeLevel)(UINT level, d8::IDirect3DVolume8 **out);
    STDMETHOD(LockBox)(UINT level, d8::D3DLOCKED_BOX *lb, const d8::D3DBOX *box, DWORD flags);
    STDMETHOD(UnlockBox)(UINT level);
    STDMETHOD(AddDirtyBox)(const d8::D3DBOX *box);
};

class Q8VertexBuffer : public Q8ResourceT<d8::IDirect3DVertexBuffer8, IDirect3DVertexBuffer9> {
public:
    DWORD usage;
    Q8VertexBuffer(Q8Device *dev, IDirect3DVertexBuffer9 *b);
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(Lock)(UINT offset, UINT size, BYTE **data, DWORD flags);
    STDMETHOD(Unlock)();
    STDMETHOD(GetDesc)(d8::D3DVERTEXBUFFER_DESC *desc);
};

class Q8IndexBuffer : public Q8ResourceT<d8::IDirect3DIndexBuffer8, IDirect3DIndexBuffer9> {
public:
    DWORD usage;
    Q8IndexBuffer(Q8Device *dev, IDirect3DIndexBuffer9 *b);
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(Lock)(UINT offset, UINT size, BYTE **data, DWORD flags);
    STDMETHOD(Unlock)();
    STDMETHOD(GetDesc)(d8::D3DINDEXBUFFER_DESC *desc);
};

class Q8SwapChain : public Q8Child<d8::IDirect3DSwapChain8, IDirect3DSwapChain9> {
public:
    Q8Table owned; /* its back buffers (Q8Surface *), held */
    HWND window;   /* where it presents: its hDeviceWindow, else the device's focus window */
    Q8SwapChain(Q8Device *dev, IDirect3DSwapChain9 *sc) : Q8Child(dev, sc), owned(), window(NULL) {}
    void destroy() override;
    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD(Present)(const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty);
    STDMETHOD(GetBackBuffer)(UINT index, d8::D3DBACKBUFFER_TYPE type, d8::IDirect3DSurface8 **out);
};

#define Q8_TEX_SLOTS 16u    /* D3D9 samplers 0-15 */
#define Q8_STREAM_SLOTS 16u

/* What the device or a state block binds, with the values set alongside. */
struct Q8Binds {
    Q8Object *tex[Q8_TEX_SLOTS];
    Q8Object *stream[Q8_STREAM_SLOTS];
    UINT stride[Q8_STREAM_SLOTS];
    Q8Object *indices;
    UINT base; /* BaseVertexIndex from SetIndices */
};

/* D3D8-level state captured alongside each D3D9 state block: shader
 * handles, and the bindings it contains (masks) and holds (b). */
struct Q8StateBlock {
    IDirect3DStateBlock9 *sb9;
    bool has_vs, has_ps, has_indices;
    DWORD vs, ps;
    DWORD tex_mask, stream_mask;
    Q8Binds b;
};

class Q8Device : public d8::IDirect3DDevice8 {
public:
    LONG refs;
    Q8Direct3D *parent;
    IDirect3DDevice9 *d9;
    Q8Table vshaders;     /* Q8VertexShader*, handle = (index1 << 1) | 1 */
    Q8Table pshaders;     /* Q8PixelShader*, handle = index1 */
    Q8Table stateblocks;  /* Q8StateBlock*, token = index1 */
    DWORD cur_vs;
    DWORD cur_ps;
    /* Render states removed from D3D9; values kept for GetRenderState. */
    DWORD rs_linepattern, rs_zvisible, rs_zbias, rs_patchsegments;
    Q8StateBlock *recording; /* non-NULL between BeginStateBlock and EndStateBlock */
    /* What the device keeps alive (see Q8Object).  Get* answers from here. */
    Q8Binds b;
    Q8Object *rt;    /* render target; NULL: the implicit back buffer, not wrapped yet */
    Q8Object *ds;    /* depth stencil once ds_known (NULL: none) */
    bool ds_known;
    Q8Table owned;   /* implicit back buffers and automatic depth stencil (Q8Surface *) */
    /* D3DCREATE_MULTITHREADED: serialises the D3D8-side tables and cached
     * state above (D3D9 serialises its own state). */
    bool mt;
    CRITICAL_SECTION cs;
    /* Creation and presentation facts the D3D8 rules check against. */
    UINT adapter;
    D3DDEVTYPE devtype;
    DWORD behavior;
    HWND focus;                /* focus window from CreateDevice */
    HWND window;               /* presentation window: hDeviceWindow, else focus */
    BOOL windowed;
    UINT bb_w, bb_h;           /* implicit back buffer */
    D3DMULTISAMPLE_TYPE bb_ms;
    UINT vs_consts;            /* constant registers CreateVertexShader allows */
    DWORD ps_version;          /* highest pixel shader version (D3D8 caps); ~0u: unknown */
    bool needs_reset;          /* a Reset failed: only a successful Reset revives the device */
    LONG default_objects;      /* live D3DPOOL_DEFAULT objects; any of them fails Reset */

    Q8Device(Q8Direct3D *parent, IDirect3DDevice9 *d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
             const D3DPRESENT_PARAMETERS *pp9);
    virtual ~Q8Device();
    void reset_d3d8_state();
    void set_presentation(const D3DPRESENT_PARAMETERS *pp9);
    void drop_state();
    Q8VertexShader *vs_from_handle(DWORD h);
    Q8PixelShader *ps_from_handle(DWORD h);
    HRESULT apply_vertex_shader(DWORD h);
    void target_size(UINT *w, UINT *h, D3DMULTISAMPLE_TYPE *ms);
    bool sysmem_texture_bound();

    STDMETHOD(QueryInterface)(REFIID riid, void **obj);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(TestCooperativeLevel)();
    STDMETHOD_(UINT, GetAvailableTextureMem)();
    STDMETHOD(ResourceManagerDiscardBytes)(DWORD bytes);
    STDMETHOD(GetDirect3D)(d8::IDirect3D8 **out);
    STDMETHOD(GetDeviceCaps)(d8::D3DCAPS8 *caps);
    STDMETHOD(GetDisplayMode)(d8::D3DDISPLAYMODE *mode);
    STDMETHOD(GetCreationParameters)(d8::D3DDEVICE_CREATION_PARAMETERS *p);
    STDMETHOD(SetCursorProperties)(UINT x, UINT y, d8::IDirect3DSurface8 *bitmap);
    STDMETHOD_(void, SetCursorPosition)(UINT x, UINT y, DWORD flags);
    STDMETHOD_(WINBOOL, ShowCursor)(WINBOOL show);
    STDMETHOD(CreateAdditionalSwapChain)(d8::D3DPRESENT_PARAMETERS *pp, d8::IDirect3DSwapChain8 **out);
    STDMETHOD(Reset)(d8::D3DPRESENT_PARAMETERS *pp);
    STDMETHOD(Present)(const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty);
    STDMETHOD(GetBackBuffer)(UINT index, d8::D3DBACKBUFFER_TYPE type, d8::IDirect3DSurface8 **out);
    STDMETHOD(GetRasterStatus)(d8::D3DRASTER_STATUS *status);
    STDMETHOD_(void, SetGammaRamp)(DWORD flags, const d8::D3DGAMMARAMP *ramp);
    STDMETHOD_(void, GetGammaRamp)(d8::D3DGAMMARAMP *ramp);
    STDMETHOD(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, d8::D3DFORMAT fmt, d8::D3DPOOL pool, d8::IDirect3DTexture8 **out);
    STDMETHOD(CreateVolumeTexture)(UINT w, UINT h, UINT d, UINT levels, DWORD usage, d8::D3DFORMAT fmt, d8::D3DPOOL pool, d8::IDirect3DVolumeTexture8 **out);
    STDMETHOD(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, d8::D3DFORMAT fmt, d8::D3DPOOL pool, d8::IDirect3DCubeTexture8 **out);
    STDMETHOD(CreateVertexBuffer)(UINT len, DWORD usage, DWORD fvf, d8::D3DPOOL pool, d8::IDirect3DVertexBuffer8 **out);
    STDMETHOD(CreateIndexBuffer)(UINT len, DWORD usage, d8::D3DFORMAT fmt, d8::D3DPOOL pool, d8::IDirect3DIndexBuffer8 **out);
    STDMETHOD(CreateRenderTarget)(UINT w, UINT h, d8::D3DFORMAT fmt, d8::D3DMULTISAMPLE_TYPE ms, WINBOOL lockable, d8::IDirect3DSurface8 **out);
    STDMETHOD(CreateDepthStencilSurface)(UINT w, UINT h, d8::D3DFORMAT fmt, d8::D3DMULTISAMPLE_TYPE ms, d8::IDirect3DSurface8 **out);
    STDMETHOD(CreateImageSurface)(UINT w, UINT h, d8::D3DFORMAT fmt, d8::IDirect3DSurface8 **out);
    STDMETHOD(CopyRects)(d8::IDirect3DSurface8 *src, const RECT *rects, UINT count, d8::IDirect3DSurface8 *dst, const POINT *points);
    STDMETHOD(UpdateTexture)(d8::IDirect3DBaseTexture8 *src, d8::IDirect3DBaseTexture8 *dst);
    STDMETHOD(GetFrontBuffer)(d8::IDirect3DSurface8 *dst);
    STDMETHOD(SetRenderTarget)(d8::IDirect3DSurface8 *rt, d8::IDirect3DSurface8 *ds);
    STDMETHOD(GetRenderTarget)(d8::IDirect3DSurface8 **out);
    STDMETHOD(GetDepthStencilSurface)(d8::IDirect3DSurface8 **out);
    STDMETHOD(BeginScene)();
    STDMETHOD(EndScene)();
    STDMETHOD(Clear)(DWORD count, const d8::D3DRECT *rects, DWORD flags, d8::D3DCOLOR color, float z, DWORD stencil);
    STDMETHOD(SetTransform)(d8::D3DTRANSFORMSTATETYPE state, const d8::D3DMATRIX *m);
    STDMETHOD(GetTransform)(d8::D3DTRANSFORMSTATETYPE state, d8::D3DMATRIX *m);
    STDMETHOD(MultiplyTransform)(d8::D3DTRANSFORMSTATETYPE state, const d8::D3DMATRIX *m);
    STDMETHOD(SetViewport)(const d8::D3DVIEWPORT8 *vp);
    STDMETHOD(GetViewport)(d8::D3DVIEWPORT8 *vp);
    STDMETHOD(SetMaterial)(const d8::D3DMATERIAL8 *m);
    STDMETHOD(GetMaterial)(d8::D3DMATERIAL8 *m);
    STDMETHOD(SetLight)(DWORD index, const d8::D3DLIGHT8 *l);
    STDMETHOD(GetLight)(DWORD index, d8::D3DLIGHT8 *l);
    STDMETHOD(LightEnable)(DWORD index, WINBOOL enable);
    STDMETHOD(GetLightEnable)(DWORD index, WINBOOL *enable);
    STDMETHOD(SetClipPlane)(DWORD index, const float *plane);
    STDMETHOD(GetClipPlane)(DWORD index, float *plane);
    STDMETHOD(SetRenderState)(d8::D3DRENDERSTATETYPE state, DWORD value);
    STDMETHOD(GetRenderState)(d8::D3DRENDERSTATETYPE state, DWORD *value);
    STDMETHOD(BeginStateBlock)();
    STDMETHOD(EndStateBlock)(DWORD *token);
    STDMETHOD(ApplyStateBlock)(DWORD token);
    STDMETHOD(CaptureStateBlock)(DWORD token);
    STDMETHOD(DeleteStateBlock)(DWORD token);
    STDMETHOD(CreateStateBlock)(d8::D3DSTATEBLOCKTYPE type, DWORD *token);
    STDMETHOD(SetClipStatus)(const d8::D3DCLIPSTATUS8 *cs);
    STDMETHOD(GetClipStatus)(d8::D3DCLIPSTATUS8 *cs);
    STDMETHOD(GetTexture)(DWORD stage, d8::IDirect3DBaseTexture8 **out);
    STDMETHOD(SetTexture)(DWORD stage, d8::IDirect3DBaseTexture8 *tex);
    STDMETHOD(GetTextureStageState)(DWORD stage, d8::D3DTEXTURESTAGESTATETYPE type, DWORD *value);
    STDMETHOD(SetTextureStageState)(DWORD stage, d8::D3DTEXTURESTAGESTATETYPE type, DWORD value);
    STDMETHOD(ValidateDevice)(DWORD *passes);
    STDMETHOD(GetInfo)(DWORD id, void *data, DWORD size);
    STDMETHOD(SetPaletteEntries)(UINT palette, const PALETTEENTRY *entries);
    STDMETHOD(GetPaletteEntries)(UINT palette, PALETTEENTRY *entries);
    STDMETHOD(SetCurrentTexturePalette)(UINT palette);
    STDMETHOD(GetCurrentTexturePalette)(UINT *palette);
    STDMETHOD(DrawPrimitive)(d8::D3DPRIMITIVETYPE type, UINT start, UINT count);
    STDMETHOD(DrawIndexedPrimitive)(d8::D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices, UINT start_index, UINT count);
    STDMETHOD(DrawPrimitiveUP)(d8::D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride);
    STDMETHOD(DrawIndexedPrimitiveUP)(d8::D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices, UINT count,
                                      const void *indices, d8::D3DFORMAT index_fmt, const void *data, UINT stride);
    STDMETHOD(ProcessVertices)(UINT src_start, UINT dst_index, UINT count, d8::IDirect3DVertexBuffer8 *dst, DWORD flags);
    STDMETHOD(CreateVertexShader)(const DWORD *decl, const DWORD *func, DWORD *handle, DWORD usage);
    STDMETHOD(SetVertexShader)(DWORD handle);
    STDMETHOD(GetVertexShader)(DWORD *handle);
    STDMETHOD(DeleteVertexShader)(DWORD handle);
    STDMETHOD(SetVertexShaderConstant)(DWORD reg, const void *data, DWORD count);
    STDMETHOD(GetVertexShaderConstant)(DWORD reg, void *data, DWORD count);
    STDMETHOD(GetVertexShaderDeclaration)(DWORD handle, void *data, DWORD *size);
    STDMETHOD(GetVertexShaderFunction)(DWORD handle, void *data, DWORD *size);
    STDMETHOD(SetStreamSource)(UINT stream, d8::IDirect3DVertexBuffer8 *vb, UINT stride);
    STDMETHOD(GetStreamSource)(UINT stream, d8::IDirect3DVertexBuffer8 **vb, UINT *stride);
    STDMETHOD(SetIndices)(d8::IDirect3DIndexBuffer8 *ib, UINT base);
    STDMETHOD(GetIndices)(d8::IDirect3DIndexBuffer8 **ib, UINT *base);
    STDMETHOD(CreatePixelShader)(const DWORD *func, DWORD *handle);
    STDMETHOD(SetPixelShader)(DWORD handle);
    STDMETHOD(GetPixelShader)(DWORD *handle);
    STDMETHOD(DeletePixelShader)(DWORD handle);
    STDMETHOD(SetPixelShaderConstant)(DWORD reg, const void *data, DWORD count);
    STDMETHOD(GetPixelShaderConstant)(DWORD reg, void *data, DWORD count);
    STDMETHOD(GetPixelShaderFunction)(DWORD handle, void *data, DWORD *size);
    STDMETHOD(DrawRectPatch)(UINT handle, const float *segs, const d8::D3DRECTPATCH_INFO *info);
    STDMETHOD(DrawTriPatch)(UINT handle, const float *segs, const d8::D3DTRIPATCH_INFO *info);
    STDMETHOD(DeletePatch)(UINT handle);
};

class Q8DevLock {
    Q8Device *dev;

public:
    explicit Q8DevLock(Q8Device *d) : dev(d)
    {
        if (dev->mt)
            EnterCriticalSection(&dev->cs);
    }
    ~Q8DevLock()
    {
        if (dev->mt)
            LeaveCriticalSection(&dev->cs);
    }
};

/* ------------------------------------------------------------------------ */
/* Wrapper creation and lookup                                               */

/* A new registered wrapper with one application reference; takes over the
 * reference on obj (released when out of memory). */
template <class W, class D9>
static inline W *q8_new(Q8Device *dev, D9 *obj, DWORD pool)
{
    W *w = new W(dev, obj);
    if (!w) {
        obj->Release();
        return NULL;
    }
    if (!q8_register(w, pool)) {
        w->release(); /* destroys it and lets go of its device reference */
        return NULL;
    }
    return w;
}

/* The wrapper for an implicit surface of the device or of an additional swap
 * chain (owned: its owner's table, which holds it), with one new
 * application reference; takes over the reference on s.  NULL when out of
 * memory. */
Q8Surface *q8_implicit_surface(Q8Device *dev, Q8Table *owned, IDirect3DSurface9 *s);
/* Lets go of every surface an owner table holds. */
void q8_release_owned(Q8Table *owned);

/* Registry lookups of application interfaces (no reference change). */
static inline Q8Surface *q8_find_surface(d8::IDirect3DSurface8 *s)
{
    return static_cast<Q8Surface *>(q8_lookup(s, Q8K_SURFACE));
}
static inline Q8VertexBuffer *q8_find_vb(d8::IDirect3DVertexBuffer8 *b)
{
    return static_cast<Q8VertexBuffer *>(q8_lookup(b, Q8K_VB));
}
static inline Q8IndexBuffer *q8_find_ib(d8::IDirect3DIndexBuffer8 *b)
{
    return static_cast<Q8IndexBuffer *>(q8_lookup(b, Q8K_IB));
}
/* The wrapper behind an application texture (NULL when t is NULL or not a
 * texture wrapper) and, when t9 is not NULL, its D3D9 texture. */
Q8Object *q8_texture(d8::IDirect3DBaseTexture8 *t, IDirect3DBaseTexture9 **t9);

#endif
