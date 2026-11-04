/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: Direct3D 7 (IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7)
 * and, as per-version interfaces over the same objects, Direct3D 1-6
 * (qdd_d3d7_legacy.cpp, qdd_d3d7_viewport.cpp, qdd_d3d7_exec.cpp),
 * implemented on the shared Direct3D 9 device of a DirectDraw object
 * (qdd.h), so the D3D9 calls reach the QD3D host.
 *
 * Headers: d3d9.h and ddraw.h are global (qdd.h); the Direct3D 7 headers
 * collide with d3d9types.h and live in namespace d7, so every Direct3D 7
 * type is spelled d7::X.  Macros (D3DFVF_*, D3DTA_*, DDERR_*, D3DERR_*) are
 * global and identical where both header sets define them.
 *
 * Model (docs/design/ddraw-remoting.md, Direct3D 7):
 *  - Q7D3D is the IDirect3D7 of one DirectDraw object (aggregated: it shares
 *    the object's count) and owns what the object's D3D9 device carries for
 *    Direct3D: which Q7Device's state is applied, the open D3D9 scene, the
 *    bound render target / depth stencil, vertex declarations per FVF and a
 *    streaming index buffer.
 *  - Q7Device keeps the complete Direct3D 7 state as the application set it
 *    (Get* and state blocks answer from it) and applies it to the D3D9
 *    device, translated; when another device of the same DirectDraw object
 *    drew last, everything is applied again first.
 *  - Textures are DirectDraw surfaces; each bound one gets a Q7Tex (its own
 *    managed D3D9 texture) uploaded from the surface's system-memory bits
 *    when qdd_surface_version says they changed, converting formats D3D9
 *    cannot sample (P8 through the palette, 24-bit, R3G3B2, ...) and, while
 *    D3DRENDERSTATE_COLORKEYENABLE is on, turning source-colour-keyed texels
 *    into alpha 0 (discarded by an alpha test, like native drivers).
 *  - Direct3D 1-6: IDirect3D3/2/1 are further interfaces of Q7D3D,
 *    IDirect3DDevice3/2/1 of Q7Device (version-1 devices aggregated into
 *    their surface); legacy render/light states, handles, viewports, lights
 *    and materials map onto the Direct3D 7 state; legacy lighting and
 *    execute buffers run on the CPU (q7_process_vertices).
 *  - Everything runs under qdd_lock().
 */
#ifndef QDD_D3D7_H
#define QDD_D3D7_H

#include "qdd.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace d7 {
#include <d3d.h>
}

/* Interface and device IDs (DirectX 7 SDK d3d.h values). */
extern const GUID Q7IID_IDirect3D7;
extern const GUID Q7IID_IDirect3DDevice7;
extern const GUID Q7IID_IDirect3DVertexBuffer7;
extern const GUID Q7IID_IDirect3DRGBDevice;
extern const GUID Q7IID_IDirect3DHALDevice;
extern const GUID Q7IID_IDirect3DTnLHalDevice;
extern const GUID Q7IID_IDirect3DMMXDevice;
extern const GUID Q7IID_IDirect3DRefDevice;
extern const GUID Q7IID_IDirect3DNullDevice;
extern const GUID Q7IID_IDirect3DRampDevice;
/* Direct3D 1-6 interface IDs (d3d.h values). */
extern const GUID Q7IID_IDirect3DDevice;
extern const GUID Q7IID_IDirect3DDevice2;
extern const GUID Q7IID_IDirect3DDevice3;
extern const GUID Q7IID_IDirect3DTexture;
extern const GUID Q7IID_IDirect3DTexture2;
extern const GUID Q7IID_IDirect3DLight;
extern const GUID Q7IID_IDirect3DMaterial;
extern const GUID Q7IID_IDirect3DMaterial2;
extern const GUID Q7IID_IDirect3DMaterial3;
extern const GUID Q7IID_IDirect3DExecuteBuffer;
extern const GUID Q7IID_IDirect3DViewport;
extern const GUID Q7IID_IDirect3DViewport2;
extern const GUID Q7IID_IDirect3DViewport3;
extern const GUID Q7IID_IDirect3DVertexBuffer;

#define Q7_STAGES 8u       /* texture stages */
#define Q7_TSS 32u         /* D3DTSS_* numbers kept (native stores 0-31; 1-24 are defined) */
#define Q7_RS 256u         /* render state numbers kept */
#define Q7_XF 14u          /* world, view, projection, world1-3, texture0-7 */
#define Q7_CLIPPLANES 32u  /* D3DMAXUSERCLIPPLANES */
#define Q7_MAX_LEVELS 16u  /* mip levels per face (16384 texels need 15) */

static inline DWORD q7_f2dw(float f)
{
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}

static inline float q7_dw2f(DWORD d)
{
    float f;
    memcpy(&f, &d, sizeof(f));
    return f;
}

/* Process lock (qdd_lock) for the scope. */
class Q7Lock {
public:
    Q7Lock() { qdd_lock(); }
    ~Q7Lock() { qdd_unlock(); }
};

class Q7D3D;
class Q7Device;
class Q7VB;
struct Q7Tex;
struct Q7Source;

/* ------------------------------------------------------------------------ */
/* State                                                                     */

struct Q7Light {
    DWORD index;           /* the application's light number */
    d7::D3DLIGHT7 light;
    bool enabled;
};

/* The complete Direct3D 7 state of a device as the application set it. */
struct Q7State {
    DWORD rs[Q7_RS];                  /* render states, D3D7 values */
    DWORD tss[Q7_STAGES][Q7_TSS];     /* texture stage states, D3D7 numbering */
    QddSurface *tex[Q7_STAGES];       /* bound textures (the device holds an iface7 reference) */
    D3DMATRIX xf[Q7_XF];
    d7::D3DVIEWPORT7 vp;
    d7::D3DMATERIAL7 mat;
    float clip[Q7_CLIPPLANES][4];
    Q7Light *lights;                  /* every light ever set or enabled, in creation order */
    UINT nlights, caplights;
};

/* What a state block contains, and the captured values. */
struct Q7SBLight {
    DWORD index;
    d7::D3DLIGHT7 light;
    bool has_light;   /* parameters included */
    bool has_enable;  /* enable flag included */
    bool enabled;
};

struct Q7StateBlock {
    DWORD rs_mask[Q7_RS / 32];
    DWORD tss_mask[Q7_STAGES];        /* bit per D3DTSS type */
    DWORD tex_mask;                   /* stages */
    DWORD xf_mask;                    /* Q7_XF slots */
    DWORD clip_mask;                  /* planes */
    bool has_vp, has_mat;
    bool all_lights;                  /* CreateStateBlock(ALL/VERTEX): every light known at capture */
    bool light_enables_only;          /* VERTEX: only the enable flags (measured) */
    Q7State st;                       /* values; st.lights unused */
    Q7SBLight *lights;
    UINT nlights, caplights;
};

/* Transform slot of a D3D7 transform state, or -1 when it is no transform. */
int q7_xf_slot(DWORD state);
/* The D3D9 transform state of a slot. */
D3DTRANSFORMSTATETYPE q7_xf_d3d9(unsigned slot);

/* Kind of a D3D7 render state number. */
enum Q7RSKind {
    Q7RS_INVALID = 0,  /* DDERR_INVALIDPARAMS on Set and Get */
    Q7RS_D3D9,         /* same number and meaning in D3D9 */
    Q7RS_STORE,        /* no D3D9 equivalent: kept for Get, no effect */
    Q7RS_SPECIAL,      /* translated or emulated by Q7Device::apply_rs */
};
Q7RSKind q7_rs_kind(DWORD state);
/* Native defaults of a fresh device (D3D7). */
void q7_state_defaults(Q7State *st, bool zbuffer);
void q7_state_free(Q7State *st);
/* Light table helpers (sorted by insertion; linear search is fine for the
 * handful of lights games use). */
Q7Light *q7_light_find(Q7State *st, DWORD index);
Q7Light *q7_light_add(Q7State *st, DWORD index);
void q7_default_light(d7::D3DLIGHT7 *l);

/* State block membership by type (D3DSBT_ALL, PIXELSTATE, VERTEXSTATE). */
void q7_sb_init_type(Q7StateBlock *sb, DWORD type);
void q7_sb_free(Q7StateBlock *sb);
Q7SBLight *q7_sb_light(Q7StateBlock *sb, DWORD index, bool add);

/* ------------------------------------------------------------------------ */
/* Formats                                                                   */

/* How a DirectDraw texture format is uploaded. */
enum Q7Conv {
    Q7C_NONE = 0,      /* not a texture format we can sample */
    Q7C_COPY,          /* bytes as they are */
    Q7C_X8_TO_A8,      /* 32-bit RGB without alpha -> A8R8G8B8 (alpha 1) */
    Q7C_565_TO_8888,   /* R5G6B5 -> A8R8G8B8 */
    Q7C_555_TO_1555,   /* X1R5G5B5 -> A1R5G5B5 (alpha 1) */
    Q7C_444_TO_4444,   /* X4R4G4B4 -> A4R4G4B4 (alpha 1) */
    Q7C_888_TO_8888,   /* 24-bit R8G8B8 -> A8R8G8B8 */
    Q7C_332_TO_8888,   /* R3G3B2 -> A8R8G8B8 */
    Q7C_8332_TO_8888,  /* A8R3G3B2 -> A8R8G8B8 */
    Q7C_PAL8,          /* P8 through the palette -> A8R8G8B8 */
    Q7C_PAL4,          /* P4 through the palette -> A8R8G8B8 */
    Q7C_L8_TO_8888,    /* L8 -> A8R8G8B8 (when D3D9 has no L8) */
    Q7C_A8L8_TO_8888,  /* A8L8 -> A8R8G8B8 */
    Q7C_A4L4_TO_8888,  /* A4L4 -> A8R8G8B8 */
    Q7C_BGRX_TO_8888,  /* 32-bit with R in the low byte -> A8R8G8B8 */
};

struct Q7Format {
    D3DFORMAT src;       /* the D3D9 name of the DirectDraw format (D3DFMT_UNKNOWN for P8/P4/24-bit) */
    D3DFORMAT d9;        /* what the D3D9 texture is created as without colour key */
    D3DFORMAT d9_key;    /* ... with a colour key applied (D3DFMT_UNKNOWN: keys ignored) */
    Q7Conv conv;
    Q7Conv conv_key;
    UINT bpp;            /* source bits per pixel (0 for block formats) */
    bool block;          /* DXTn: 4x4 blocks */
    DWORD key_mask;      /* bits of a texel compared with the colour key */
    bool has_alpha;      /* the source format has alpha */
};

/* Classifies a DirectDraw pixel format for texturing; false when the
 * device cannot sample it. */
bool q7_texture_format(const DDPIXELFORMAT *pf, Q7Format *out);
/* DDPIXELFORMAT of a D3D9 format (enumeration); false when it has none. */
bool q7_ddpf_from_d3d9(D3DFORMAT f, DDPIXELFORMAT *pf);

/* ------------------------------------------------------------------------ */
/* FVF                                                                       */

/* Bytes per vertex of a D3D7 FVF; 0 when the FVF is invalid. */
UINT q7_fvf_size(DWORD fvf);
/* Byte offsets of the elements of an FVF vertex (-1: absent). */
struct Q7FvfLayout {
    UINT size;
    int pos, nbeta, normal, reserved, diffuse, specular;
    UINT ntex;
    int tex[8];
    UINT texdim[8];  /* 1-4 floats */
    UINT posfloats;  /* 3 (XYZ), 4 (XYZRHW) */
};
bool q7_fvf_layout(DWORD fvf, Q7FvfLayout *l);
/* Primitive count for a vertex/index count; 0 for none or an invalid type. */
UINT q7_prim_count(DWORD type, DWORD count);

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */

class Q7D3D;

/* IDirect3D3, IDirect3D2 and IDirect3D of a DirectDraw object
 * (qdd_d3d7_legacy.cpp): counted on its IDirectDraw like native. */
class Q7D3D3 final : public d7::IDirect3D3 {
public:
    Q7D3D *o;
    explicit Q7D3D3(Q7D3D *d) : o(d) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(EnumDevices)(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx) override;
    STDMETHOD(CreateLight)(d7::IDirect3DLight **light, IUnknown *outer) override;
    STDMETHOD(CreateMaterial)(d7::IDirect3DMaterial3 **material, IUnknown *outer) override;
    STDMETHOD(CreateViewport)(d7::IDirect3DViewport3 **viewport, IUnknown *outer) override;
    STDMETHOD(FindDevice)(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result) override;
    STDMETHOD(CreateDevice)(REFCLSID rclsid, IDirectDrawSurface4 *surface, d7::IDirect3DDevice3 **device,
                            IUnknown *outer) override;
    STDMETHOD(CreateVertexBuffer)(d7::D3DVERTEXBUFFERDESC *desc, d7::IDirect3DVertexBuffer **buffer, DWORD flags,
                                  IUnknown *outer) override;
    STDMETHOD(EnumZBufferFormats)(REFCLSID device_iid, d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(EvictManagedTextures)() override;
};

class Q7D3D2 final : public d7::IDirect3D2 {
public:
    Q7D3D *o;
    explicit Q7D3D2(Q7D3D *d) : o(d) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(EnumDevices)(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx) override;
    STDMETHOD(CreateLight)(d7::IDirect3DLight **light, IUnknown *outer) override;
    STDMETHOD(CreateMaterial)(d7::IDirect3DMaterial2 **material, IUnknown *outer) override;
    STDMETHOD(CreateViewport)(d7::IDirect3DViewport2 **viewport, IUnknown *outer) override;
    STDMETHOD(FindDevice)(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result) override;
    STDMETHOD(CreateDevice)(REFCLSID rclsid, IDirectDrawSurface *surface, d7::IDirect3DDevice2 **device) override;
};

class Q7D3D1 final : public d7::IDirect3D {
public:
    Q7D3D *o;
    explicit Q7D3D1(Q7D3D *d) : o(d) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(REFIID riid) override;
    STDMETHOD(EnumDevices)(d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx) override;
    STDMETHOD(CreateLight)(d7::IDirect3DLight **light, IUnknown *outer) override;
    STDMETHOD(CreateMaterial)(d7::IDirect3DMaterial **material, IUnknown *outer) override;
    STDMETHOD(CreateViewport)(d7::IDirect3DViewport **viewport, IUnknown *outer) override;
    STDMETHOD(FindDevice)(d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result) override;
};

/* A per-DirectDraw-object Direct3D record and its IDirect3D7. */
class Q7D3D final : public d7::IDirect3D7 {
public:
    QddDraw *dd;
    Q7Device *devices;          /* live devices (linked through next) */
    Q7Device *active;           /* whose state is applied to the D3D9 device; NULL: none */
    bool in_scene9;             /* a D3D9 BeginScene is open */
    /* What is bound on the D3D9 device (NULL: unknown / nothing). */
    IDirect3DSurface9 *bound_rt9;
    IDirect3DSurface9 *bound_ds9;
    IDirect3DBaseTexture9 *bound_tex9[Q7_STAGES];
    IDirect3DVertexDeclaration9 *bound_decl9;
    /* Vertex declarations per FVF (decl9 holds a reference each). */
    DWORD *decl_fvf;
    IDirect3DVertexDeclaration9 **decl9;
    UINT ndecl, capdecl;
    /* Streaming index buffer for DrawIndexedPrimitiveVB. */
    IDirect3DIndexBuffer9 *ib9;
    UINT ib_size, ib_pos;       /* in indices */
    bool ib_bound;
    /* Caps (D3D9, from qdd_draw_d3d9) and what the D3D7 descriptions derive. */
    bool caps_ok;
    D3DCAPS9 caps9;
    D3DFORMAT display_format;
    UINT d9_lights;             /* D3D9 light slots in use (0..d9_lights-1) */
    Q7VB *vbs;                  /* live vertex buffers (linked through next) */
    Q7D3D3 if3;                 /* Direct3D 1-6 interfaces (DirectDrawCreate objects) */
    Q7D3D2 if2;
    Q7D3D1 if1;

    explicit Q7D3D(QddDraw *d);
    ~Q7D3D();

    IDirect3DDevice9 *device9() { return qdd_draw_device9(dd); }
    bool get_caps();
    void reset_bindings();
    void end_scene();
    IDirect3DVertexDeclaration9 *decl_for(DWORD fvf);
    HRESULT device_desc(const GUID *guid, d7::D3DDEVICEDESC7 *desc);
    /* Direct3D 1-6: EnumDevices of interface version v (3, 2, 1). */
    HRESULT legacy_enum(UINT v, d7::LPD3DENUMDEVICESCALLBACK cb, void *ctx);
    HRESULT legacy_find(UINT v, d7::D3DFINDDEVICESEARCH *search, d7::D3DFINDDEVICERESULT *result);
    /* The HAL/HEL descriptions of a device type as EnumDevices of version v
     * reports them; false for a type version v does not enumerate. */
    bool legacy_desc(UINT v, const GUID *guid, d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel);
    HRESULT create_device(REFCLSID rclsid, QddSurface *s, IUnknown *rt_iface, UINT version, Q7Device **out);
    HRESULT create_vb(d7::D3DVERTEXBUFFERDESC *desc, UINT version, Q7VB **out);

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(EnumDevices)(d7::LPD3DENUMDEVICESCALLBACK7 cb, void *ctx) override;
    STDMETHOD(CreateDevice)(REFCLSID rclsid, IDirectDrawSurface7 *surface, d7::IDirect3DDevice7 **device) override;
    STDMETHOD(CreateVertexBuffer)(d7::D3DVERTEXBUFFERDESC *desc, d7::IDirect3DVertexBuffer7 **buffer,
                                  DWORD flags) override;
    STDMETHOD(EnumZBufferFormats)(REFCLSID device_iid, d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(EvictManagedTextures)() override;
};

/* The Q7D3D of a DirectDraw object, created on first use; NULL when out of
 * memory. */
Q7D3D *q7_d3d(QddDraw *dd);

/* A bound texture's Direct3D 9 copy: one plain or cube texture per
 * colour-key variant, refreshed from the DirectDraw surfaces when their
 * versions change. */
struct Q7TexVariant {
    IDirect3DBaseTexture9 *tex9;
    D3DFORMAT fmt9;
    Q7Conv conv;
    DWORD key;                                   /* colour key the upload used */
    DWORD root_version;                          /* palette/key source version uploaded */
    DWORD versions[6][Q7_MAX_LEVELS];            /* per face and level, 0: not uploaded */
};

struct Q7Tex {
    QddSurface *root;
    UINT w, h, levels;
    bool cube;
    Q7Format fmt;
    DWORD desc_caps, desc_caps2;
    DWORD pf_flags, pf_bpp;                      /* format identity at creation */
    Q7TexVariant var[2];                         /* [0] no key, [1] colour key -> alpha */
};

/* What the Direct3D code keeps per DirectDraw surface (the surface's
 * d3d7 slot): the texture record, and for surfaces not made through
 * IDirectDraw7 the IDirect3DTexture2/IDirect3DTexture interfaces (counted
 * on the surface), the legacy texture handle and the version-1 device a
 * QueryInterface created. */
class Q7Surf;
class Q7Tex2If final : public d7::IDirect3DTexture2 {
public:
    Q7Surf *o;
    explicit Q7Tex2If(Q7Surf *x) : o(x) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetHandle)(d7::IDirect3DDevice2 *device, d7::D3DTEXTUREHANDLE *handle) override;
    STDMETHOD(PaletteChanged)(DWORD start, DWORD count) override;
    STDMETHOD(Load)(d7::IDirect3DTexture2 *src) override;
};
class Q7Tex1If final : public d7::IDirect3DTexture {
public:
    Q7Surf *o;
    explicit Q7Tex1If(Q7Surf *x) : o(x) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3DDevice *device, IDirectDrawSurface *surface) override;
    STDMETHOD(GetHandle)(d7::IDirect3DDevice *device, d7::D3DTEXTUREHANDLE *handle) override;
    STDMETHOD(PaletteChanged)(DWORD start, DWORD count) override;
    STDMETHOD(Load)(d7::IDirect3DTexture *src) override;
    STDMETHOD(Unload)() override;
};
class Q7Surf {
public:
    QddSurface *s;
    Q7Tex *tex;                 /* texture record, made on first texture use */
    DWORD handle;               /* legacy texture handle, 0 before GetHandle */
    Q7Device *device1[2];       /* version-1 devices aggregated into the surface: [0] HAL, [1] RGB
                                 * (native makes one per device type) */
    Q7Tex2If t2;
    Q7Tex1If t1;
    explicit Q7Surf(QddSurface *surface) : s(surface), tex(NULL), handle(0), device1(), t2(this), t1(this) {}
    /* What the texture interfaces count on: IDirectDrawSurface4 for
     * surfaces made through IDirectDraw4, else IDirectDrawSurface. */
    IUnknown *outer();
};
/* The record of a surface, made on first use; NULL when out of memory. */
Q7Surf *q7_surf(QddSurface *s);
/* The surface behind an IDirect3DTexture2 / IDirect3DTexture pointer, else NULL. */
QddSurface *q7_tex2_surface(const void *p);
QddSurface *q7_tex1_surface(const void *p);

/* The texture record of a surface (created on first use), NULL when the
 * surface cannot be a texture. */
Q7Tex *q7_tex(QddSurface *s);
void q7_tex_free(Q7Tex *t);
/* The current D3D9 texture for sampling s (uploading what changed), with
 * the colour key applied when keyed; NULL on failure.  *key_alpha: the
 * texture was keyed (alpha 0 marks keyed texels) and its source format
 * has no alpha of its own. */
IDirect3DBaseTexture9 *q7_tex_d3d9(Q7D3D *d3d, QddSurface *s, bool keyed, bool *source_alpha);
/* Whether a DirectDraw texture has a source colour key. */
bool q7_surface_ckey(QddSurface *s, DWORD *key);

/* IDirect3DVertexBuffer7 on a D3D9 vertex buffer. */
class Q7VB final : public d7::IDirect3DVertexBuffer7 {
public:
    LONG refs;
    Q7D3D *d3d;               /* NULL once its DirectDraw object is gone */
    QddDraw *dd;
    IDirect3DVertexBuffer9 *vb9;
    DWORD caps;
    DWORD fvf;
    DWORD count;              /* vertices */
    UINT version;             /* 7 (IDirect3DVertexBuffer7) or 3 (IDirect3DVertexBuffer) */
    UINT stride;
    UINT size;
    LONG locks;
    BYTE *locked;             /* pointer handed out by Lock */
    Q7VB *next;               /* live buffers of the same DirectDraw object */

    Q7VB(Q7D3D *d, DWORD caps, DWORD fvf, DWORD count, UINT version);
    void detach();
    /* Readable bits of the buffer (D3D9 lock, read only); NULL on failure. */
    const BYTE *map_read();
    void unmap_read();

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Lock)(DWORD flags, void **data, DWORD *data_size) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(ProcessVertices)(DWORD vertex_op, DWORD dst_idx, DWORD count, d7::IDirect3DVertexBuffer7 *src_buffer,
                               DWORD src_idx, d7::IDirect3DDevice7 *device, DWORD flags) override;
    STDMETHOD(GetVertexBufferDesc)(d7::D3DVERTEXBUFFERDESC *desc) override;
    STDMETHOD(Optimize)(d7::IDirect3DDevice7 *device, DWORD flags) override;
    STDMETHOD(ProcessVerticesStrided)(DWORD vertex_op, DWORD dst_idx, DWORD count,
                                      d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD fvf,
                                      d7::IDirect3DDevice7 *device, DWORD flags) override;
};

/* The registered Q7VB behind an application pointer, else NULL (never
 * dereferences p). */
Q7VB *q7_vb_lookup(const void *p);

/* Derived D3D9 state the colour-key emulation and stage fixups set. */
struct Q7Derived {
    DWORD alphatest, alphafunc, alpharef;
    DWORD aop0, aarg1_0, aarg2_0;
    bool valid;
};

/* ------------------------------------------------------------------------ */
/* Direct3D 1-6 interfaces of a device (qdd_d3d7_legacy.cpp)                 */

class Q7Viewport;

/* IDirect3DDevice3 of a version-3 device. */
class Q7Dev3 final : public d7::IDirect3DDevice3 {
public:
    Q7Device *o;
    explicit Q7Dev3(Q7Device *dev) : o(dev) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel) override;
    STDMETHOD(GetStats)(d7::D3DSTATS *stats) override;
    STDMETHOD(AddViewport)(d7::IDirect3DViewport3 *vp) override;
    STDMETHOD(DeleteViewport)(d7::IDirect3DViewport3 *vp) override;
    STDMETHOD(NextViewport)(d7::IDirect3DViewport3 *ref, d7::IDirect3DViewport3 **vp, DWORD flags) override;
    STDMETHOD(EnumTextureFormats)(d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(d7::IDirect3D3 **d3d) override;
    STDMETHOD(SetCurrentViewport)(d7::IDirect3DViewport3 *vp) override;
    STDMETHOD(GetCurrentViewport)(d7::IDirect3DViewport3 **vp) override;
    STDMETHOD(SetRenderTarget)(IDirectDrawSurface4 *surface, DWORD flags) override;
    STDMETHOD(GetRenderTarget)(IDirectDrawSurface4 **surface) override;
    STDMETHOD(Begin)(d7::D3DPRIMITIVETYPE type, DWORD fvf, DWORD flags) override;
    STDMETHOD(BeginIndexed)(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count, DWORD flags) override;
    STDMETHOD(Vertex)(void *vertex) override;
    STDMETHOD(Index)(WORD index) override;
    STDMETHOD(End)(DWORD flags) override;
    STDMETHOD(GetRenderState)(d7::D3DRENDERSTATETYPE state, LPDWORD value) override;
    STDMETHOD(SetRenderState)(d7::D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetLightState)(d7::D3DLIGHTSTATETYPE state, LPDWORD value) override;
    STDMETHOD(SetLightState)(d7::D3DLIGHTSTATETYPE state, DWORD value) override;
    STDMETHOD(SetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(GetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(MultiplyTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(DrawPrimitive)(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitive)(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD vcount,
                                    WORD *indices, DWORD icount, DWORD flags) override;
    STDMETHOD(SetClipStatus)(d7::D3DCLIPSTATUS *status) override;
    STDMETHOD(GetClipStatus)(d7::D3DCLIPSTATUS *status) override;
    STDMETHOD(DrawPrimitiveStrided)(d7::D3DPRIMITIVETYPE type, DWORD fvf, d7::D3DDRAWPRIMITIVESTRIDEDDATA *data,
                                    DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveStrided)(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                           d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD vcount, WORD *indices,
                                           DWORD icount, DWORD flags) override;
    STDMETHOD(DrawPrimitiveVB)(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer *vb, DWORD start, DWORD count,
                               DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveVB)(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer *vb, WORD *indices,
                                      DWORD icount, DWORD flags) override;
    STDMETHOD(ComputeSphereVisibility)(D3DVECTOR *centers, d7::D3DVALUE *radii, DWORD count, DWORD flags,
                                       DWORD *ret) override;
    STDMETHOD(GetTexture)(DWORD stage, d7::IDirect3DTexture2 **tex) override;
    STDMETHOD(SetTexture)(DWORD stage, d7::IDirect3DTexture2 *tex) override;
    STDMETHOD(GetTextureStageState)(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, LPDWORD value) override;
    STDMETHOD(SetTextureStageState)(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    STDMETHOD(ValidateDevice)(LPDWORD passes) override;
};

/* IDirect3DDevice2 of a version-2 or -3 device. */
class Q7Dev2 final : public d7::IDirect3DDevice2 {
public:
    Q7Device *o;
    explicit Q7Dev2(Q7Device *dev) : o(dev) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel) override;
    STDMETHOD(SwapTextureHandles)(d7::IDirect3DTexture2 *t1, d7::IDirect3DTexture2 *t2) override;
    STDMETHOD(GetStats)(d7::D3DSTATS *stats) override;
    STDMETHOD(AddViewport)(d7::IDirect3DViewport2 *vp) override;
    STDMETHOD(DeleteViewport)(d7::IDirect3DViewport2 *vp) override;
    STDMETHOD(NextViewport)(d7::IDirect3DViewport2 *ref, d7::IDirect3DViewport2 **vp, DWORD flags) override;
    STDMETHOD(EnumTextureFormats)(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(d7::IDirect3D2 **d3d) override;
    STDMETHOD(SetCurrentViewport)(d7::IDirect3DViewport2 *vp) override;
    STDMETHOD(GetCurrentViewport)(d7::IDirect3DViewport2 **vp) override;
    STDMETHOD(SetRenderTarget)(IDirectDrawSurface *surface, DWORD flags) override;
    STDMETHOD(GetRenderTarget)(IDirectDrawSurface **surface) override;
    STDMETHOD(Begin)(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, DWORD flags) override;
    STDMETHOD(BeginIndexed)(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, void *vertices, DWORD count,
                            DWORD flags) override;
    STDMETHOD(Vertex)(void *vertex) override;
    STDMETHOD(Index)(WORD index) override;
    STDMETHOD(End)(DWORD flags) override;
    STDMETHOD(GetRenderState)(d7::D3DRENDERSTATETYPE state, LPDWORD value) override;
    STDMETHOD(SetRenderState)(d7::D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetLightState)(d7::D3DLIGHTSTATETYPE state, LPDWORD value) override;
    STDMETHOD(SetLightState)(d7::D3DLIGHTSTATETYPE state, DWORD value) override;
    STDMETHOD(SetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(GetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(MultiplyTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(DrawPrimitive)(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, void *vertices, DWORD count,
                             DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitive)(d7::D3DPRIMITIVETYPE type, d7::D3DVERTEXTYPE vt, void *vertices, DWORD vcount,
                                    WORD *indices, DWORD icount, DWORD flags) override;
    STDMETHOD(SetClipStatus)(d7::D3DCLIPSTATUS *status) override;
    STDMETHOD(GetClipStatus)(d7::D3DCLIPSTATUS *status) override;
};

/* IDirect3DDevice of a version-1, -2 or -3 device. */
class Q7Dev1 final : public d7::IDirect3DDevice {
public:
    Q7Device *o;
    explicit Q7Dev1(Q7Device *dev) : o(dev) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3D *d3d, GUID *guid, d7::D3DDEVICEDESC *desc) override;
    STDMETHOD(GetCaps)(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel) override;
    STDMETHOD(SwapTextureHandles)(d7::IDirect3DTexture *t1, d7::IDirect3DTexture *t2) override;
    STDMETHOD(CreateExecuteBuffer)(d7::D3DEXECUTEBUFFERDESC *desc, d7::IDirect3DExecuteBuffer **buffer,
                                   IUnknown *outer) override;
    STDMETHOD(GetStats)(d7::D3DSTATS *stats) override;
    STDMETHOD(Execute)(d7::IDirect3DExecuteBuffer *buffer, d7::IDirect3DViewport *vp, DWORD flags) override;
    STDMETHOD(AddViewport)(d7::IDirect3DViewport *vp) override;
    STDMETHOD(DeleteViewport)(d7::IDirect3DViewport *vp) override;
    STDMETHOD(NextViewport)(d7::IDirect3DViewport *ref, d7::IDirect3DViewport **vp, DWORD flags) override;
    STDMETHOD(Pick)(d7::IDirect3DExecuteBuffer *buffer, d7::IDirect3DViewport *vp, DWORD flags,
                    d7::D3DRECT *rect) override;
    STDMETHOD(GetPickRecords)(DWORD *count, d7::D3DPICKRECORD *records) override;
    STDMETHOD(EnumTextureFormats)(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(CreateMatrix)(d7::D3DMATRIXHANDLE *matrix) override;
    STDMETHOD(SetMatrix)(d7::D3DMATRIXHANDLE handle, d7::D3DMATRIX *matrix) override;
    STDMETHOD(GetMatrix)(d7::D3DMATRIXHANDLE handle, d7::D3DMATRIX *matrix) override;
    STDMETHOD(DeleteMatrix)(d7::D3DMATRIXHANDLE handle) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(d7::IDirect3D **d3d) override;
};

class Q7Device final : public d7::IDirect3DDevice7 {
public:
    LONG refs;
    Q7D3D *d3d;                 /* NULL once its DirectDraw object is gone */
    QddDraw *dd;
    Q7Device *next;             /* devices of the same DirectDraw object */
    GUID guid;                  /* device type */
    bool hal;                   /* HAL or T&L HAL (video-memory targets only) */
    QddSurface *rt;             /* render target (held through rt_ref) */
    IUnknown *rt_ref;           /* the interface holding rt: iface7 (v7), IDirectDrawSurface4 (v3),
                                 * IDirectDrawSurface (v2); NULL for v1 (aggregated into rt) */
    bool rt_valid;              /* false: a failed SetRenderTarget left a non-target */
    Q7State st;
    bool in_scene;              /* between BeginScene and EndScene */
    Q7StateBlock *recording;    /* between BeginStateBlock and EndStateBlock */
    Q7StateBlock **sbs;         /* state blocks, handle = index + 1 */
    UINT nsbs;
    Q7Derived derived;          /* what was last set for the derived states */
    BYTE *scratch;              /* strided assembly */
    UINT scratch_size;
    d7::D3DCLIPSTATUS clipstatus;
    float pv_power;             /* last non-zero material power (ProcessVertices keeps it) */

    UINT version;               /* 7, 3, 2 or 1: which CreateDevice / surface QI made it */
    IUnknown *outer;            /* version 1: the render target's IDirectDrawSurface (aggregation) */
    /* Direct3D 1-6 (qdd_d3d7_legacy.cpp). */
    Q7Viewport *viewports;      /* AddViewport list, most recent first */
    Q7Viewport *cur_vp;         /* SetCurrentViewport (referenced) */
    DWORD material;             /* D3DLIGHTSTATE_MATERIAL handle */
    DWORD tmblend;              /* D3DRENDERSTATE_TEXTUREMAPBLEND */
    bool legacy_blend;          /* TEXTUREMAPBLEND was set: stage 0 follows it */
    bool tex0_handle;           /* stage 0's texture came from TEXTUREHANDLE (GetTexture hides it) */
    DWORD tss0_set;             /* stage-0 D3DTSS types TEXTUREMAPBLEND last set (bit per type) */
    DWORD tss0_val[Q7_TSS];
    D3DMATRIX legacy_proj;      /* the application's projection (versions < 7) */
    D3DMATRIX legacy_clip;      /* the current viewport's clip-space mapping */
    DWORD bv_type, bv_fvf, bv_flags; /* Begin/Vertex/End */
    BYTE *bv_buf;
    UINT bv_count, bv_cap, bv_stride;
    bool bv_indexed;            /* BeginIndexed: the application's vertices, Index() indices */
    const BYTE *bv_verts;
    DWORD bv_vcount;
    WORD *bv_idx;
    UINT bv_icount, bv_icap;
    DWORD mx_world, mx_view, mx_proj; /* execute-buffer matrix handles (version 1) */
    DWORD lrs[Q7_RS];           /* render states as the application set them (native legacy values) */
    BYTE *lit_buf;              /* CPU-lit vertices of legacy draws */
    UINT lit_cap;
    DWORD ls[9];                /* light states 1-8 */
    Q7Dev3 if3;
    Q7Dev2 if2;
    Q7Dev1 if1;

    Q7Device(Q7D3D *d, const GUID *type, QddSurface *target, UINT version);
    ~Q7Device();
    void detach();
    /* The IUnknown of the object (the outer surface for version 1). */
    ULONG outer_addref();
    ULONG outer_release();
    HRESULT outer_query(REFIID riid, void **obj);
    /* A texture stage state as applied (legacy TEXTUREMAPBLEND overrides stage 0). */
    DWORD tss_eff(DWORD stage, DWORD type) const;
    /* Direct3D 1-6 (qdd_d3d7_legacy.cpp). */
    void legacy_init();         /* right after construction, versions < 7 */
    void legacy_teardown();     /* when the last reference goes */
    HRESULT inner_query(REFIID riid, void **obj);
    /* Whether a legacy draw is lit: a material is set and the vertices
     * carry normals (untransformed), unless D3DDP_DONOTLIGHT. */
    bool legacy_lit(DWORD fvf, DWORD flags) const;
    /* Lights count vertices on the CPU the DirectX 3-6 way into lit_buf:
     * positions copied, diffuse and specular computed, texture coordinates
     * copied (*out_fvf, *out_stride describe them); NULL when out of memory. */
    BYTE *legacy_light(const Q7Source *src, DWORD count, DWORD *out_fvf, UINT *out_stride);
    void legacy_set_clip(const D3DMATRIX *clip);
    void legacy_fog();
    void legacy_wrap();
    void set_tmblend(DWORD mode);
    HRESULT legacy_set_rs(DWORD state, DWORD value);
    HRESULT legacy_get_rs(DWORD state, DWORD *value);
    HRESULT legacy_set_ls(DWORD state, DWORD value);
    HRESULT legacy_get_ls(DWORD state, DWORD *value);
    HRESULT legacy_set_transform(DWORD state, const D3DMATRIX *m);
    HRESULT legacy_get_transform(DWORD state, D3DMATRIX *m);
    HRESULT legacy_multiply_transform(DWORD state, const D3DMATRIX *m);
    HRESULT legacy_caps(d7::D3DDEVICEDESC *hal, d7::D3DDEVICEDESC *hel);
    HRESULT legacy_set_texture(DWORD stage, QddSurface *s);
    HRESULT legacy_set_tss(DWORD stage, DWORD type, DWORD value);
    HRESULT legacy_set_target(QddSurface *s, IUnknown *iface);
    HRESULT add_viewport(Q7Viewport *vp);
    HRESULT delete_viewport(Q7Viewport *vp);
    HRESULT next_viewport(Q7Viewport *ref, Q7Viewport **out, DWORD flags);
    HRESULT set_current_viewport(Q7Viewport *vp);
    HRESULT begin(DWORD type, DWORD fvf, DWORD flags);
    HRESULT begin_indexed(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, DWORD flags);
    HRESULT vertex(const void *v);
    HRESULT index(WORD i);
    HRESULT end(DWORD flags);
    HRESULT legacy_draw(DWORD type, DWORD fvf, const void *vertices, DWORD count, DWORD flags);
    HRESULT legacy_draw_indexed(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, const WORD *indices,
                                DWORD icount, DWORD flags);
    HRESULT legacy_sphere_visibility(const D3DVECTOR *centers, const float *radii, DWORD count, DWORD *ret);
    HRESULT legacy_texture_formats(d7::LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx);
    /* EnumTextureFormats as interface version v lists them. */
    HRESULT texture_formats(UINT v, d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx);

    /* State application. */
    void make_active();
    void apply_all();
    void apply_rs(DWORD state, DWORD value);
    void apply_tss(DWORD stage, DWORD type, DWORD value);
    void apply_transform(unsigned slot);
    void apply_viewport();
    void apply_lights();
    void apply_light(Q7Light *l);
    HRESULT prepare_draw();
    HRESULT prepare_clear(DWORD flags);
    bool bind_targets(bool need_draw);
    void bind_textures();
    void apply_derived();
    bool begin_scene9();
    bool is_active() const { return d3d && d3d->active == this; }

    /* Setters shared by the methods and state blocks (record when recording). */
    HRESULT set_rs(DWORD state, DWORD value);
    HRESULT set_tss(DWORD stage, DWORD type, DWORD value);
    HRESULT set_texture(DWORD stage, QddSurface *s);
    HRESULT set_transform(DWORD state, const D3DMATRIX *m);
    HRESULT set_viewport(const d7::D3DVIEWPORT7 *vp);
    HRESULT set_material(const d7::D3DMATERIAL7 *m);
    HRESULT set_light(DWORD index, const d7::D3DLIGHT7 *l);
    HRESULT light_enable(DWORD index, BOOL enable);
    HRESULT set_clip_plane(DWORD index, const float *plane);
    void sb_capture(Q7StateBlock *sb);
    void sb_apply(Q7StateBlock *sb);
    Q7StateBlock *sb_get(DWORD handle);
    void forget_surface(QddSurface *s);

    /* Draw helpers. */
    HRESULT draw_up(DWORD type, DWORD fvf, const void *vertices, DWORD count);
    HRESULT draw_indexed_up(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, const WORD *indices,
                            DWORD icount);
    BYTE *assemble_strided(DWORD fvf, const d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD first, DWORD count,
                           UINT *stride);
    HRESULT upload_indices(const WORD *indices, DWORD count, UINT *start);

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(d7::D3DDEVICEDESC7 *desc) override;
    STDMETHOD(EnumTextureFormats)(d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(d7::IDirect3D7 **d3d) override;
    STDMETHOD(SetRenderTarget)(IDirectDrawSurface7 *surface, DWORD flags) override;
    STDMETHOD(GetRenderTarget)(IDirectDrawSurface7 **surface) override;
    STDMETHOD(Clear)(DWORD count, d7::D3DRECT *rects, DWORD flags, D3DCOLOR color, d7::D3DVALUE z,
                     DWORD stencil) override;
    STDMETHOD(SetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(GetTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(SetViewport)(d7::D3DVIEWPORT7 *data) override;
    STDMETHOD(MultiplyTransform)(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *matrix) override;
    STDMETHOD(GetViewport)(d7::D3DVIEWPORT7 *data) override;
    STDMETHOD(SetMaterial)(d7::D3DMATERIAL7 *data) override;
    STDMETHOD(GetMaterial)(d7::D3DMATERIAL7 *data) override;
    STDMETHOD(SetLight)(DWORD idx, d7::D3DLIGHT7 *data) override;
    STDMETHOD(GetLight)(DWORD idx, d7::D3DLIGHT7 *data) override;
    STDMETHOD(SetRenderState)(d7::D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetRenderState)(d7::D3DRENDERSTATETYPE state, LPDWORD value) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(LPDWORD handle) override;
    STDMETHOD(PreLoad)(IDirectDrawSurface7 *surface) override;
    STDMETHOD(DrawPrimitive)(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitive)(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD vcount,
                                    WORD *indices, DWORD icount, DWORD flags) override;
    STDMETHOD(SetClipStatus)(d7::D3DCLIPSTATUS *status) override;
    STDMETHOD(GetClipStatus)(d7::D3DCLIPSTATUS *status) override;
    STDMETHOD(DrawPrimitiveStrided)(d7::D3DPRIMITIVETYPE type, DWORD fvf, d7::D3DDRAWPRIMITIVESTRIDEDDATA *data,
                                    DWORD count, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveStrided)(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                           d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD vcount, WORD *indices,
                                           DWORD icount, DWORD flags) override;
    STDMETHOD(DrawPrimitiveVB)(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer7 *vb, DWORD start, DWORD count,
                               DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveVB)(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer7 *vb, DWORD start,
                                      DWORD vcount, WORD *indices, DWORD icount, DWORD flags) override;
    STDMETHOD(ComputeSphereVisibility)(D3DVECTOR *centers, d7::D3DVALUE *radii, DWORD count, DWORD flags,
                                       DWORD *ret) override;
    STDMETHOD(GetTexture)(DWORD stage, IDirectDrawSurface7 **surface) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirectDrawSurface7 *surface) override;
    STDMETHOD(GetTextureStageState)(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, LPDWORD value) override;
    STDMETHOD(SetTextureStageState)(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    STDMETHOD(ValidateDevice)(LPDWORD passes) override;
    STDMETHOD(ApplyStateBlock)(DWORD handle) override;
    STDMETHOD(CaptureStateBlock)(DWORD handle) override;
    STDMETHOD(DeleteStateBlock)(DWORD handle) override;
    STDMETHOD(CreateStateBlock)(d7::D3DSTATEBLOCKTYPE type, LPDWORD handle) override;
    STDMETHOD(Load)(IDirectDrawSurface7 *dst, POINT *dst_point, IDirectDrawSurface7 *src, RECT *src_rect,
                    DWORD flags) override;
    STDMETHOD(LightEnable)(DWORD index, WINBOOL enable) override;
    STDMETHOD(GetLightEnable)(DWORD index, WINBOOL *enable) override;
    STDMETHOD(SetClipPlane)(DWORD index, d7::D3DVALUE *plane) override;
    STDMETHOD(GetClipPlane)(DWORD index, d7::D3DVALUE *plane) override;
    STDMETHOD(GetInfo)(DWORD id, void *info, DWORD size) override;
};

/* The registered Q7Device behind an application pointer, else NULL. */
Q7Device *q7_device_lookup(const void *p);

/* ------------------------------------------------------------------------ */
/* CPU vertex processing (ProcessVertices, ComputeSphereVisibility)          */

/* Source vertices as element pointers and strides. */
struct Q7Stream {
    const BYTE *ptr;
    UINT stride;
};
struct Q7Source {
    DWORD fvf;
    Q7Stream pos, normal, diffuse, specular, tex[8];
};
/* ProcessVertices: transforms (and lights, with D3DVOP_LIGHT and lighting
 * on) count source vertices into dst (dst_fvf layout, dst_stride bytes per
 * vertex) using the device's current state. */
void q7_process_vertices(const Q7State *st, float power0, DWORD vertex_op, const Q7Source *src, DWORD count,
                         BYTE *dst, DWORD dst_fvf, UINT dst_stride, UINT dst_avail);
/* legacy: IDirect3DDevice3's measured plane rule (q7_in_plane). */
void q7_sphere_visibility(const Q7State *st, const D3DVECTOR *centers, const float *radii, DWORD count,
                          DWORD *ret, bool legacy = false);
/* Source vertices of an FVF at base, stride bytes apart. */
void q7_source_from_fvf(Q7Source *s, DWORD fvf, const BYTE *base, UINT stride);
void q7_mat_mul(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b);

/* Object registry: application pointer -> object (lookups never call or
 * dereference the pointer). */
bool q7_registry_add(const void *key, unsigned kind);
void q7_registry_remove(const void *key);
bool q7_registry_has(const void *key, unsigned kind);
enum { Q7K_DEVICE = 1, Q7K_VB = 2, Q7K_DEVICE3, Q7K_DEVICE2, Q7K_DEVICE1, Q7K_VIEWPORT, Q7K_LIGHT,
       Q7K_MATERIAL3, Q7K_MATERIAL2, Q7K_MATERIAL1, Q7K_TEX2, Q7K_TEX1, Q7K_EXECBUF };

/* ------------------------------------------------------------------------ */
/* Direct3D 1-6 objects (qdd_d3d7_viewport.cpp, qdd_d3d7_exec.cpp)           */

/* Legacy handles (textures, materials, matrices): process-wide like the
 * stock runtime's; a handle is its index + 1, 0 is never one. */
enum { Q7H_FREE = 0, Q7H_TEXTURE, Q7H_MATERIAL, Q7H_MATRIX };
DWORD q7_handle_alloc(unsigned kind, void *obj);
void *q7_handle_get(DWORD handle, unsigned kind);
void q7_handle_free(DWORD handle);
void q7_handle_set(DWORD handle, void *obj);

/* The device behind an IDirect3DDevice3/2/1 pointer, else NULL. */
Q7Device *q7_device3_lookup(const void *p);
Q7Device *q7_device2_lookup(const void *p);
Q7Device *q7_device1_lookup(const void *p);
/* A version-1 device's surface record is going away. */
void q7_surf_release_legacy(Q7Surf *x);
/* The hook DirectDraw surfaces' QueryInterface calls (qdd.h). */
HRESULT qdd_d3d7_surface_query(QddSurface *s, REFIID riid, void **out);
/* IDirect3DTexture2::Load of dst from src (palette, source colour key, levels). */
HRESULT q7_texture_load(QddSurface *dst, QddSurface *src);

class Q7LightObj;

/* IDirect3DViewport3 (also serves IDirect3DViewport2 and IDirect3DViewport,
 * which it extends). */
class Q7Viewport final : public d7::IDirect3DViewport3 {
public:
    LONG refs;
    Q7D3D *d3d;
    Q7Device *device;           /* AddViewport'ed to */
    Q7Viewport *next;           /* the device's list */
    UINT vp_version;            /* 0 nothing set, 1 D3DVIEWPORT, 2 D3DVIEWPORT2 */
    d7::D3DVIEWPORT vp1;
    d7::D3DVIEWPORT2 vp2;
    DWORD background;           /* material handle */
    Q7LightObj *lights;         /* AddLight list, in order */
    DWORD active_mask;          /* light slots in use (bit i: device light i + 1) */

    explicit Q7Viewport(Q7D3D *d);
    /* Make this the device's viewport (D3D7 viewport, clip space, lights). */
    void activate(bool lights_too);
    void deactivate();
    void alloc_light_slot(Q7LightObj *l);
    void free_light_slot(Q7LightObj *l);

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3D *d3d) override;
    STDMETHOD(GetViewport)(d7::D3DVIEWPORT *data) override;
    STDMETHOD(SetViewport)(d7::D3DVIEWPORT *data) override;
    STDMETHOD(TransformVertices)(DWORD count, d7::D3DTRANSFORMDATA *data, DWORD flags, DWORD *offscreen) override;
    STDMETHOD(LightElements)(DWORD count, d7::D3DLIGHTDATA *data) override;
    STDMETHOD(SetBackground)(d7::D3DMATERIALHANDLE material) override;
    STDMETHOD(GetBackground)(d7::D3DMATERIALHANDLE *material, WINBOOL *valid) override;
    STDMETHOD(SetBackgroundDepth)(IDirectDrawSurface *surface) override;
    STDMETHOD(GetBackgroundDepth)(IDirectDrawSurface **surface, WINBOOL *valid) override;
    STDMETHOD(Clear)(DWORD count, d7::D3DRECT *rects, DWORD flags) override;
    STDMETHOD(AddLight)(d7::IDirect3DLight *light) override;
    STDMETHOD(DeleteLight)(d7::IDirect3DLight *light) override;
    STDMETHOD(NextLight)(d7::IDirect3DLight *ref, d7::IDirect3DLight **light, DWORD flags) override;
    STDMETHOD(GetViewport2)(d7::D3DVIEWPORT2 *data) override;
    STDMETHOD(SetViewport2)(d7::D3DVIEWPORT2 *data) override;
    STDMETHOD(SetBackgroundDepth2)(IDirectDrawSurface4 *surface) override;
    STDMETHOD(GetBackgroundDepth2)(IDirectDrawSurface4 **surface, WINBOOL *valid) override;
    STDMETHOD(Clear2)(DWORD count, d7::D3DRECT *rects, DWORD flags, DWORD color, d7::D3DVALUE z,
                      DWORD stencil) override;
};
Q7Viewport *q7_viewport_lookup(const void *p);

/* IDirect3DLight. */
class Q7LightObj final : public d7::IDirect3DLight {
public:
    LONG refs;
    Q7Viewport *viewport;       /* AddLight'ed to */
    Q7LightObj *next;           /* the viewport's list */
    d7::D3DLIGHT2 light;        /* as set (dwSize of the caller's structure) */
    bool set;
    DWORD slot;                 /* device light index while active (1-based), 0 inactive */

    Q7LightObj();
    void activate();
    void deactivate();
    void update();

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3D *d3d) override;
    STDMETHOD(SetLight)(d7::D3DLIGHT *data) override;
    STDMETHOD(GetLight)(d7::D3DLIGHT *data) override;
};
Q7LightObj *q7_light_lookup(const void *p);

/* IDirect3DMaterial3, IDirect3DMaterial2, IDirect3DMaterial. */
class Q7Material;
class Q7Mat3 final : public d7::IDirect3DMaterial3 {
public:
    Q7Material *o;
    explicit Q7Mat3(Q7Material *m) : o(m) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(SetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetHandle)(d7::IDirect3DDevice3 *device, d7::D3DMATERIALHANDLE *handle) override;
};
class Q7Mat2 final : public d7::IDirect3DMaterial2 {
public:
    Q7Material *o;
    explicit Q7Mat2(Q7Material *m) : o(m) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(SetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetHandle)(d7::IDirect3DDevice2 *device, d7::D3DMATERIALHANDLE *handle) override;
};
class Q7Mat1 final : public d7::IDirect3DMaterial {
public:
    Q7Material *o;
    explicit Q7Mat1(Q7Material *m) : o(m) {}
    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3D *d3d) override;
    STDMETHOD(SetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetMaterial)(d7::D3DMATERIAL *data) override;
    STDMETHOD(GetHandle)(d7::IDirect3DDevice *device, d7::D3DMATERIALHANDLE *handle) override;
    STDMETHOD(Reserve)() override;
    STDMETHOD(Unreserve)() override;
};
class Q7Material {
public:
    LONG refs;
    d7::D3DMATERIAL mat;
    /* One handle per device that asked (native: GetHandle on another device
     * gives another handle, Wine's test_material). */
    struct Handle {
        const void *device;
        DWORD handle;
    } *handles;
    UINT nhandles;
    Q7Mat3 if3;
    Q7Mat2 if2;
    Q7Mat1 if1;
    Q7Material();
    ~Q7Material();
    ULONG add_ref() { return (ULONG)InterlockedIncrement(&refs); }
    ULONG release();
    HRESULT query(REFIID riid, void **obj);
    HRESULT set(const d7::D3DMATERIAL *m);
    HRESULT get(d7::D3DMATERIAL *m);
    HRESULT get_handle(const void *device, d7::D3DMATERIALHANDLE *handle);
};
/* The D3D7 material of a legacy one. */
void q7_material7(const d7::D3DMATERIAL *m, d7::D3DMATERIAL7 *out);

/* IDirect3DExecuteBuffer (version-1 devices). */
class Q7ExecBuf final : public d7::IDirect3DExecuteBuffer {
public:
    LONG refs;
    Q7Device *device;
    d7::D3DEXECUTEBUFFERDESC desc;
    d7::D3DEXECUTEDATA data;
    BYTE *mem;
    bool own_mem;
    bool locked;
    BYTE *tl;                   /* transformed vertices, 32 bytes (D3DTLVERTEX) each */
    UINT tl_count;

    Q7ExecBuf(Q7Device *dev);
    ~Q7ExecBuf();
    HRESULT execute(Q7Viewport *vp);

    STDMETHOD(QueryInterface)(REFIID riid, void **obj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(d7::IDirect3DDevice *device, d7::D3DEXECUTEBUFFERDESC *desc) override;
    STDMETHOD(Lock)(d7::D3DEXECUTEBUFFERDESC *desc) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(SetExecuteData)(d7::D3DEXECUTEDATA *data) override;
    STDMETHOD(GetExecuteData)(d7::D3DEXECUTEDATA *data) override;
    STDMETHOD(Validate)(DWORD *offset, d7::LPD3DVALIDATECALLBACK cb, void *ctx, DWORD reserved) override;
    STDMETHOD(Optimize)(DWORD dummy) override;
};
Q7ExecBuf *q7_execbuf_lookup(const void *p);

#endif
