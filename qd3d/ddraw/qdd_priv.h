/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: DirectDraw implemented in the guest over a QD3D Direct3D 9 device.
 * Internal declarations of the core (qdd_*.cpp except qdd_d3d7*).
 * docs/design/ddraw-remoting.md describes the design.
 *
 * Every COM object is a plain struct with one facet per interface version
 * (a small class deriving from that interface, pointing back at the
 * object).  Native DirectDraw counts references per interface (each
 * IDirectDrawN / IDirectDrawSurfaceN has its own count, Wine's refcount
 * tests), so every facet keeps its own count and the object lives while
 * any facet count is non-zero (complex surfaces: while their root lives).
 */
#ifndef QDD_PRIV_H
#define QDD_PRIV_H

#include "qdd.h"
#include <string.h>
#include <stdlib.h>
#include "../proto/qd3d_proto.h"

/* Version index of an interface (DirectDraw: 1,2,4,7; surfaces: 1,2,3,4,7). */
static inline int qdd_vidx(int v)
{
    return v == 7 ? 4 : v - 1;
}

/* ------------------------------------------------------------------------ */
/* Process-wide state                                                        */

extern CRITICAL_SECTION g_qdd_cs;
extern HMODULE g_qdd_self;

/* QDDRAW_TRACE=1 statistics (qdd_trace.cpp): off, nothing is patched and
 * these cost a branch on g_qdd_trace. */
extern bool g_qdd_trace;
enum { QDD_TK_DRAW, QDD_TK_SURF, QDD_TK_PAL, QDD_TK_CLIP, QDD_TK_GAMMA };
enum { QDD_TX_UPLOAD, QDD_TX_READBACK, QDD_TX_DEPTH_UP, QDD_TX_DEPTH_DOWN };
void qdd_trace_init(void);                                /* DllMain */
void qdd_trace_vtbl(void *iface, int kind, int version); /* time iface's methods */
LONGLONG qdd_trace_now(void);
void qdd_trace_xfer(int kind, DWORD bytes, LONGLONG t0);
void qdd_trace_present(LONGLONG t0);
void qdd_trace_vblank(LONGLONG t0);
void qdd_trace_mark_presenter(void);
void qdd_trace_lock(CRITICAL_SECTION *cs);
/* Counts one transfer of `bytes` for its lifetime. */
struct QddXferTrace {
    int kind;
    DWORD bytes;
    LONGLONG t0;
    QddXferTrace(int k, DWORD b) : kind(k), bytes(b), t0(g_qdd_trace ? qdd_trace_now() : 0) {}
    ~QddXferTrace()
    {
        if (g_qdd_trace)
            qdd_trace_xfer(kind, bytes, t0);
    }
};

struct QddLock {
    QddLock()
    {
        if (g_qdd_trace)
            qdd_trace_lock(&g_qdd_cs);
        else
            EnterCriticalSection(&g_qdd_cs);
    }
    ~QddLock() { LeaveCriticalSection(&g_qdd_cs); }
    QddLock(const QddLock &) = delete;
    QddLock &operator=(const QddLock &) = delete;
};

/* Environment switches (read once). */
struct QddEnv {
    bool virtual_modes; /* QDDRAW_VIRTUAL_MODES=1: never touch the real display mode */
};
extern QddEnv g_qdd_env;

/* Interface pointer registry: pointer -> object (no dereference). */
enum : unsigned {
    QDD_T_DRAW = 1,
    QDD_T_SURFACE = 2,
    QDD_T_GAMMA = 3,
    QDD_T_PALETTE = 4,
    QDD_T_CLIPPER = 5,
};
bool qdd_reg_add(const void *p, void *obj, unsigned type, int vidx);
void qdd_reg_remove(const void *p);
void *qdd_reg_find(const void *p, unsigned type, int *vidx);

/* ------------------------------------------------------------------------ */
/* Pixel formats                                                             */

enum QddFmtClass : unsigned char {
    QFC_NONE,
    QFC_RGB,      /* masks, maybe alpha */
    QFC_PAL,      /* palette indexed 1/2/4/8 */
    QFC_LUM,      /* luminance (+alpha) */
    QFC_BUMP,     /* bump du/dv (+luminance) */
    QFC_ALPHA,    /* alpha only */
    QFC_DEPTH,    /* z (+stencil) */
    QFC_FOURCC_BLOCK, /* DXTn */
    QFC_FOURCC_YUV,   /* YUY2, UYVY */
    QFC_FOURCC_PLANAR, /* YV12: Y plane, then V and U at half the pitch and height */
};

struct QddFormat {
    QddFmtClass cls;
    BYTE bpp;          /* bits per pixel; block formats: 0 */
    BYTE block_bytes;  /* DXTn: bytes per 4x4 block */
    DWORD fourcc;
    DWORD mask_r, mask_g, mask_b, mask_a; /* RGB/LUM(r=lum)/BUMP(r=u,g=v,b=l); DEPTH: r=z, a=stencil */
    D3DFORMAT d3d;     /* same layout in Direct3D 9, or D3DFMT_UNKNOWN */
    DDPIXELFORMAT ddpf; /* canonical description */
};

/* The format a pixel format describes, or NULL when unsupported.
 * Z formats may also come from a legacy DDSD_ZBUFFERBITDEPTH. */
const QddFormat *qdd_format_from_ddpf(const DDPIXELFORMAT *pf);
const QddFormat *qdd_format_for_depth_bits(DWORD bits);
/* DXT1-5 blocks (qdd_dxt.cpp): one block to/from 16 A8R8G8B8 pixels, and
 * the block-aligned rect [x0, x1) x [y0, y1) of a surface's memory to/from
 * rows of A8R8G8B8 with pitch (x1 - x0) * 4. */
void qdd_dxt_decode(const QddFormat *f, const BYTE *blk, DWORD out[16]);
void qdd_dxt_encode(const QddFormat *f, const DWORD in[16], BYTE *blk);
void qdd_dxt_decode_rect(const QddFormat *f, const BYTE *bits, LONG pitch, LONG x0, LONG y0, LONG x1, LONG y1,
                         DWORD *out);
void qdd_dxt_encode_rect(const QddFormat *f, BYTE *bits, LONG pitch, LONG x0, LONG y0, LONG x1, LONG y1,
                         const DWORD *in);
/* Display formats: 8 (P8), 16 (R5G6B5), 24 (R8G8B8), 32 (X8R8G8B8). */
const QddFormat *qdd_format_for_bpp(DWORD bpp);
const QddFormat *qdd_format_d3d(D3DFORMAT f);
/* Row pitch (bytes) of a w-pixel row; DXTn: of a row of blocks. */
DWORD qdd_format_pitch(const QddFormat *f, DWORD w);
/* Rows of storage (h, or block rows). */
DWORD qdd_format_rows(const QddFormat *f, DWORD h);
/* Bytes per pixel for CPU blits (0: not blittable per pixel). */
static inline UINT qdd_format_bytes(const QddFormat *f)
{
    return (f->cls == QFC_FOURCC_BLOCK || f->bpp < 8) ? 0u : (UINT)f->bpp / 8u;
}
static inline bool qdd_format_is_pal(const QddFormat *f) { return f->cls == QFC_PAL; }

/* Color conversion helpers.  ARGB here is 0xAARRGGBB. */
DWORD qdd_pixel_to_argb(const QddFormat *f, DWORD px, const PALETTEENTRY *pal);
DWORD qdd_argb_to_pixel(const QddFormat *f, DWORD argb);
/* Converts a row of n pixels from (sf, src) to (df, dst).  Formats must be
 * per-pixel (qdd_format_bytes != 0) or the YUV formats as sources;
 * palette indexed sources use pal.  Returns false when unsupported. */
bool qdd_convert_row(const QddFormat *df, BYTE *dst, const QddFormat *sf, const BYTE *src, UINT n,
                     const PALETTEENTRY *pal);
/* BT.601 studio-range YUV to A8R8G8B8. */
DWORD qdd_yuv_to_argb(int y, int u, int v);
static inline DWORD qdd_read_px(const BYTE *p, UINT bytes)
{
    switch (bytes) {
    case 1: return p[0];
    case 2: return *(const WORD *)p;
    case 3: return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16);
    default: return *(const DWORD *)p;
    }
}
static inline void qdd_write_px(BYTE *p, UINT bytes, DWORD v)
{
    switch (bytes) {
    case 1: p[0] = (BYTE)v; break;
    case 2: *(WORD *)p = (WORD)v; break;
    case 3: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break;
    default: *(DWORD *)p = v; break;
    }
}

/* ------------------------------------------------------------------------ */
/* Surface memory                                                            */

/* A BITMAPINFO with room for masks or a 256-entry color table. */
struct QddBmi {
    BITMAPINFOHEADER h;
    union {
        DWORD masks[3];
        RGBQUAD colors[256];
    };
};

enum QddMemKind : unsigned char {
    QDD_MEM_KIND_CLIENT, /* the application's (DDSD_LPSURFACE) */
    QDD_MEM_KIND_HEAP,
    QDD_MEM_KIND_VIRTUAL,
    QDD_MEM_KIND_DIB,    /* allocated by CreateDIBSection: GetDC selects dib */
};

struct QddMem {
    BYTE *ptr;
    DWORD size;
    QddMemKind kind;
    HBITMAP dib;      /* QDD_MEM_KIND_DIB: as wide as the pitch */
};
/* Memory for size bytes of a surface of format f (NULL: not a GDI view)
 * with the given pitch and height; zero-filled. */
bool qdd_mem_alloc(QddMem *m, DWORD size, const QddFormat *f, DWORD pitch, DWORD h);
void qdd_mem_free(QddMem *m);
/* The GDI view (BITMAPINFO) of such memory, false for non-GDI formats;
 * pal fills a palette's color table. */
bool qdd_mem_bmi(const QddFormat *f, DWORD pitch, DWORD h, QddBmi *bmi, const PALETTEENTRY *pal);

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */

struct QddDraw;
struct QddSurface;
struct QddPalette;
struct QddClipper;
struct QddPasses;
struct QddDepthXfer;
struct QddIndexXfer;

template <int V> struct QddDrawT;
template <> struct QddDrawT<1> {
    typedef IDirectDraw I;
    typedef IDirectDrawSurface Surf;
    typedef DDSURFACEDESC Desc;
    typedef DDSCAPS Caps;
    typedef LPDDENUMMODESCALLBACK ModeCb;
    typedef LPDDENUMSURFACESCALLBACK SurfCb;
    typedef DDDEVICEIDENTIFIER DevId;
};
template <> struct QddDrawT<2> {
    typedef IDirectDraw2 I;
    typedef IDirectDrawSurface Surf;
    typedef DDSURFACEDESC Desc;
    typedef DDSCAPS Caps;
    typedef LPDDENUMMODESCALLBACK ModeCb;
    typedef LPDDENUMSURFACESCALLBACK SurfCb;
    typedef DDDEVICEIDENTIFIER DevId;
};
template <> struct QddDrawT<4> {
    typedef IDirectDraw4 I;
    typedef IDirectDrawSurface4 Surf;
    typedef DDSURFACEDESC2 Desc;
    typedef DDSCAPS2 Caps;
    typedef LPDDENUMMODESCALLBACK2 ModeCb;
    typedef LPDDENUMSURFACESCALLBACK2 SurfCb;
    typedef DDDEVICEIDENTIFIER DevId;
};
template <> struct QddDrawT<7> {
    typedef IDirectDraw7 I;
    typedef IDirectDrawSurface7 Surf;
    typedef DDSURFACEDESC2 Desc;
    typedef DDSCAPS2 Caps;
    typedef LPDDENUMMODESCALLBACK2 ModeCb;
    typedef LPDDENUMSURFACESCALLBACK7 SurfCb;
    typedef DDDEVICEIDENTIFIER2 DevId;
};

/* One IDirectDraw* interface of a QddDraw. */
template <int V>
class QddDrawIface final : public QddDrawT<V>::I {
public:
    typedef QddDrawT<V> T;
    /* The surface interface version this DirectDraw interface hands out. */
    enum { SV = V == 2 ? 1 : V };
    QddDraw *dd;

    STDMETHOD(QueryInterface)(REFIID riid, void **out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(Compact)();
    STDMETHOD(CreateClipper)(DWORD flags, IDirectDrawClipper **out, IUnknown *outer);
    STDMETHOD(CreatePalette)(DWORD flags, PALETTEENTRY *entries, IDirectDrawPalette **out, IUnknown *outer);
    STDMETHOD(CreateSurface)(typename T::Desc *desc, typename T::Surf **out, IUnknown *outer);
    STDMETHOD(DuplicateSurface)(typename T::Surf *src, typename T::Surf **out);
    STDMETHOD(EnumDisplayModes)(DWORD flags, typename T::Desc *desc, void *ctx, typename T::ModeCb cb);
    STDMETHOD(EnumSurfaces)(DWORD flags, typename T::Desc *desc, void *ctx, typename T::SurfCb cb);
    STDMETHOD(FlipToGDISurface)();
    STDMETHOD(GetCaps)(DDCAPS *hal, DDCAPS *hel);
    STDMETHOD(GetDisplayMode)(typename T::Desc *desc);
    STDMETHOD(GetFourCCCodes)(DWORD *count, DWORD *codes);
    STDMETHOD(GetGDISurface)(typename T::Surf **out);
    STDMETHOD(GetMonitorFrequency)(DWORD *freq);
    STDMETHOD(GetScanLine)(DWORD *line);
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL *in_vb);
    STDMETHOD(Initialize)(GUID *guid);
    STDMETHOD(RestoreDisplayMode)();
    STDMETHOD(SetCooperativeLevel)(HWND window, DWORD flags);
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp);
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags);
    STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE event);
    STDMETHOD(GetAvailableVidMem)(typename T::Caps *caps, DWORD *total, DWORD *free_mem);
    STDMETHOD(GetSurfaceFromDC)(HDC dc, typename T::Surf **out);
    STDMETHOD(RestoreAllSurfaces)();
    STDMETHOD(TestCooperativeLevel)();
    STDMETHOD(GetDeviceIdentifier)(typename T::DevId *id, DWORD flags);
    STDMETHOD(StartModeTest)(SIZE *modes, DWORD count, DWORD flags);
    STDMETHOD(EvaluateMode)(DWORD flags, DWORD *timeout);
};

/* A display mode as the application sees it (emulated depth). */
struct QddMode {
    DWORD w, h, bpp, refresh;
};

/* A cached Direct3D 9 staging surface for uploads/readbacks. */
struct QddDraw {
    QddDrawIface<1> if1;
    QddDrawIface<2> if2;
    QddDrawIface<4> if4;
    QddDrawIface<7> if7;
    LONG ref[4];          /* per interface: 1, 2, 4, 7 */
    LONG ifaces;          /* interfaces with a non-zero count */
    bool ex;              /* DirectDrawCreateEx / CLSID_DirectDraw7 object */
    bool initialized;     /* CoCreateInstance objects need Initialize */
    bool scl_ddraw1;      /* last SetCooperativeLevel through IDirectDraw */
    bool scl_recursive;
    /* DDCREATE_EMULATIONONLY objects have no hardware: everything lives in
     * system memory and the driver caps are empty; DDCREATE_HARDWAREONLY
     * objects report no emulation caps (ddraw7.c test_caps). */
    enum { QDD_HW_BOTH, QDD_HW_ONLY, QDD_HEL_ONLY } hw;

    IDirect3D9 *d3d9;
    IDirect3DDevice9 *dev9;
    bool dev9_failed;
    UINT bb_w, bb_h;      /* back buffer size of dev9 */
    HWND hidden_window;   /* device window when there is no other */

    DWORD coop;
    HWND window;          /* cooperative-level (device) window */
    HWND focus_window;
    HWND device_window;   /* DDSCL_CREATEDEVICEWINDOW */
    bool window_setup;    /* window moved/sized for fullscreen */

    QddSurface *surfaces; /* every surface of the object */
    QddSurface *primary;  /* the primary surface (front buffer), if any */
    bool primary_dirty;   /* QD3D presentation pending */
    LONGLONG present_vblank; /* emulated vertical blank of the last present */
    DWORD gamma_version;
    DDGAMMARAMP gamma;
    bool gamma_set;
    QddPasses *passes;    /* qdd_pass.cpp, with dev9 */

    void *d3d7;           /* qdd_d3d7_draw_slot */
    QddDraw *next;        /* g_qdd_draws */
};

extern QddDraw *g_qdd_draws;

/* Creates the object behind DirectDrawCreate/Ex and CoCreateInstance. */
HRESULT qdd_draw_create(IDirect3D9 *d3d9, bool ex, bool initialized, QddDraw **out);
/* DDCREATE_EMULATIONONLY / DDCREATE_HARDWAREONLY / a device GUID. */
void qdd_draw_set_guid(QddDraw *dd, const GUID *guid);
void *qdd_draw_iface(QddDraw *dd, int v);
ULONG qdd_draw_addref(QddDraw *dd, int v);
ULONG qdd_draw_release(QddDraw *dd, int v);
HRESULT qdd_draw_qi(QddDraw *dd, REFIID riid, void **out);

/* Display mode bookkeeping (process-wide). */
void qdd_mode_current(QddMode *m);
const QddFormat *qdd_mode_format(void);
bool qdd_exclusive(QddDraw *dd);       /* dd holds exclusive mode */
/* Whether surfaces of dd present through QD3D (exclusive mode, or a
 * Direct3D device in the process), else through GDI. */
bool qdd_present_qd3d(QddDraw *dd);
QddDraw *qdd_exclusive_owner(void);
/* A Direct3D interface was handed out in this process (surface QI quirk). */
extern bool g_qdd_d3d_loaded;
/* Video memory accounting for GetAvailableVidMem/GetCaps. */
void qdd_vidmem_add(DWORD bytes);
void qdd_vidmem_sub(DWORD bytes);
bool qdd_vidmem_fits(DWORD bytes);

/* Emulated vertical blank (refresh of the current mode). */
void qdd_vblank_wait(void);
bool qdd_vblank_in(DWORD *scanline);
LONGLONG qdd_vblank_count(void);

/* ------------------------------------------------------------------------ */
/* Surfaces                                                                  */

template <int V> struct QddSurfT;
template <> struct QddSurfT<1> {
    typedef IDirectDrawSurface I;
    typedef DDSURFACEDESC Desc;
    typedef DDSCAPS Caps;
    typedef LPDDENUMSURFACESCALLBACK EnumCb;
    typedef IDirectDrawSurface CbSurf;
    typedef void *UnlockArg;
};
template <> struct QddSurfT<2> {
    typedef IDirectDrawSurface2 I;
    typedef DDSURFACEDESC Desc;
    typedef DDSCAPS Caps;
    typedef LPDDENUMSURFACESCALLBACK EnumCb;
    typedef IDirectDrawSurface CbSurf;
    typedef void *UnlockArg;
};
template <> struct QddSurfT<3> {
    typedef IDirectDrawSurface3 I;
    typedef DDSURFACEDESC Desc;
    typedef DDSCAPS Caps;
    typedef LPDDENUMSURFACESCALLBACK EnumCb;
    typedef IDirectDrawSurface CbSurf;
    typedef void *UnlockArg;
};
template <> struct QddSurfT<4> {
    typedef IDirectDrawSurface4 I;
    typedef DDSURFACEDESC2 Desc;
    typedef DDSCAPS2 Caps;
    typedef LPDDENUMSURFACESCALLBACK2 EnumCb;
    typedef IDirectDrawSurface4 CbSurf;
    typedef RECT *UnlockArg;
};
template <> struct QddSurfT<7> {
    typedef IDirectDrawSurface7 I;
    typedef DDSURFACEDESC2 Desc;
    typedef DDSCAPS2 Caps;
    typedef LPDDENUMSURFACESCALLBACK7 EnumCb;
    typedef IDirectDrawSurface7 CbSurf;
    typedef RECT *UnlockArg;
};

template <int V>
class QddSurfIface final : public QddSurfT<V>::I {
public:
    typedef QddSurfT<V> T;
    typedef typename T::I I;
    QddSurface *s;

    STDMETHOD(QueryInterface)(REFIID riid, void **out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(AddAttachedSurface)(I *att);
    STDMETHOD(AddOverlayDirtyRect)(RECT *r);
    STDMETHOD(Blt)(RECT *dr, I *src, RECT *sr, DWORD flags, DDBLTFX *fx);
    STDMETHOD(BltBatch)(DDBLTBATCH *batch, DWORD count, DWORD flags);
    STDMETHOD(BltFast)(DWORD x, DWORD y, I *src, RECT *sr, DWORD trans);
    STDMETHOD(DeleteAttachedSurface)(DWORD flags, I *att);
    STDMETHOD(EnumAttachedSurfaces)(void *ctx, typename T::EnumCb cb);
    STDMETHOD(EnumOverlayZOrders)(DWORD flags, void *ctx, typename T::EnumCb cb);
    STDMETHOD(Flip)(I *target, DWORD flags);
    STDMETHOD(GetAttachedSurface)(typename T::Caps *caps, I **out);
    STDMETHOD(GetBltStatus)(DWORD flags);
    STDMETHOD(GetCaps)(typename T::Caps *caps);
    STDMETHOD(GetClipper)(IDirectDrawClipper **out);
    STDMETHOD(GetColorKey)(DWORD flags, DDCOLORKEY *key);
    STDMETHOD(GetDC)(HDC *dc);
    STDMETHOD(GetFlipStatus)(DWORD flags);
    STDMETHOD(GetOverlayPosition)(LONG *x, LONG *y);
    STDMETHOD(GetPalette)(IDirectDrawPalette **out);
    STDMETHOD(GetPixelFormat)(DDPIXELFORMAT *pf);
    STDMETHOD(GetSurfaceDesc)(typename T::Desc *desc);
    STDMETHOD(Initialize)(IDirectDraw *ddraw, typename T::Desc *desc);
    STDMETHOD(IsLost)();
    STDMETHOD(Lock)(RECT *r, typename T::Desc *desc, DWORD flags, HANDLE event);
    STDMETHOD(ReleaseDC)(HDC dc);
    STDMETHOD(Restore)();
    STDMETHOD(SetClipper)(IDirectDrawClipper *clipper);
    STDMETHOD(SetColorKey)(DWORD flags, DDCOLORKEY *key);
    STDMETHOD(SetOverlayPosition)(LONG x, LONG y);
    STDMETHOD(SetPalette)(IDirectDrawPalette *pal);
    STDMETHOD(Unlock)(typename T::UnlockArg r);
    STDMETHOD(UpdateOverlay)(RECT *sr, I *dst, RECT *dr, DWORD flags, DDOVERLAYFX *fx);
    STDMETHOD(UpdateOverlayDisplay)(DWORD flags);
    STDMETHOD(UpdateOverlayZOrder)(DWORD flags, I *ref);
    STDMETHOD(GetDDInterface)(void **out);
    STDMETHOD(PageLock)(DWORD flags);
    STDMETHOD(PageUnlock)(DWORD flags);
    STDMETHOD(SetSurfaceDesc)(typename T::Desc *desc, DWORD flags);
    STDMETHOD(SetPrivateData)(REFGUID tag, void *data, DWORD size, DWORD flags);
    STDMETHOD(GetPrivateData)(REFGUID tag, void *data, DWORD *size);
    STDMETHOD(FreePrivateData)(REFGUID tag);
    STDMETHOD(GetUniquenessValue)(DWORD *value);
    STDMETHOD(ChangeUniquenessValue)();
    STDMETHOD(SetPriority)(DWORD prio);
    STDMETHOD(GetPriority)(DWORD *prio);
    STDMETHOD(SetLOD)(DWORD lod);
    STDMETHOD(GetLOD)(DWORD *lod);
};

class QddGammaIface final : public IDirectDrawGammaControl {
public:
    QddSurface *s;
    STDMETHOD(QueryInterface)(REFIID riid, void **out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetGammaRamp)(DWORD flags, DDGAMMARAMP *ramp);
    STDMETHOD(SetGammaRamp)(DWORD flags, DDGAMMARAMP *ramp);
};

/* Memory and Direct3D 9 copies of one surface's contents.  Flip rotates
 * buffers among the surfaces of a chain. */
struct QddBuffer {
    QddMem mem;
    LONG pitch;
    bool sys_valid;           /* the system-memory copy is current */
    bool gpu_valid;           /* gpu is current */
    IDirect3DSurface9 *gpu;   /* render target / depth stencil */
    IDirect3DTexture9 *gpu_tex; /* render-target texture whose level 0 is gpu */
    IDirect3DSurface9 *stage; /* system-memory surface for transfers */
    D3DFORMAT gpu_fmt;
    DWORD gpu_pal_version;    /* palette version a P8 upload used */
    QddDepthXfer *zx;         /* qdd_pass.cpp: INTZ depth transfer surfaces */
    QddIndexXfer *px;         /* qdd_pass.cpp: P8 index/palette textures */
};

struct QddAttach {
    QddSurface *surf;
    int vidx;                 /* interface it was attached through (holds a reference) */
};

struct QddPrivate {
    GUID tag;
    DWORD flags;
    DWORD size;
    void *data;               /* or IUnknown* with DDSPD_IUNKNOWNPOINTER */
    DWORD version_set;        /* surface version when set (DDSPD_VOLATILE) */
    QddPrivate *next;
};

struct QddSurface {
    QddSurfIface<1> if1;
    QddSurfIface<2> if2;
    QddSurfIface<3> if3;
    QddSurfIface<4> if4;
    QddSurfIface<7> if7;
    QddGammaIface ifg;
    LONG ref[6];              /* v1, v2, v3, v4, v7, gamma */
    LONG ifaces;
    int version;              /* 1 (IDirectDraw/2), 4, 7: QI and caps2 rules */
    int creator_v;            /* creating IDirectDraw interface: 1, 2, 4, 7 */
    bool legacy_z;            /* created with DDSD_ZBUFFERBITDEPTH */
    bool map_write;           /* qdd_surface_map(write) outstanding */
    LONGLONG last_flip;       /* emulated vertical blank of the last Flip */
    QddDraw *dd;
    IUnknown *dd_ref;         /* creating IDirectDraw4/7 interface, referenced by the root */

    DDSURFACEDESC2 desc;      /* as GetSurfaceDesc reports it (lpSurface NULL) */
    const QddFormat *fmt;
    QddBuffer *buf;
    QddBuffer own_buf;        /* the buffer the surface was created with */

    /* Complex structure: the root owns every surface created with it. */
    QddSurface *root;         /* complex root (this for roots and simple surfaces) */
    QddSurface *complex_next; /* next surface of the root's structure */
    QddSurface *complex_attached[8]; /* implicit attachments, Wine's complex_array order */
    UINT face, level;         /* cube face / mip level in the structure */
    UINT levels;              /* root: mip levels per face */

    QddAttach *attached;      /* explicit attachments (AddAttachedSurface) */
    UINT attached_count, attached_cap;
    UINT attached_to;         /* how many surfaces this one is explicitly attached to */

    QddPalette *palette;
    QddClipper *clipper;

    bool locked;
    RECT lock_rect;
    bool lock_write;
    HDC dc;
    HBITMAP dc_dib;           /* DIB of the DC (buf->dib or a temporary) */
    HGDIOBJ dc_old;
    bool dc_copy;             /* DC on a temporary copy (client memory) */

    bool lost;
    DWORD version_counter;    /* qdd_surface_version */
    DWORD uniqueness;
    DWORD priority, lod;
    QddPrivate *priv;
    void *d3d7;               /* qdd_surface_d3d7_slot */
    QddSurface *dd_next, *dd_prev;
    QddSurface *dup_of;       /* DuplicateSurface: the source, held (ifaces) */
};

void *qdd_surf_iface(QddSurface *s, int v);
ULONG qdd_surf_addref(QddSurface *s, int vidx);
ULONG qdd_surf_release(QddSurface *s, int vidx);
QddSurface *qdd_surf_from(const void *iface);
/* DuplicateSurface: a surface object on src's buffer, its interface v. */
HRESULT qdd_surface_duplicate(QddSurface *src, int creator_v, int v, void **out);
static inline DWORD qdd_surf_caps(const QddSurface *s) { return s->desc.ddsCaps.dwCaps; }
static inline DWORD qdd_surf_w(const QddSurface *s) { return s->desc.dwWidth; }
static inline DWORD qdd_surf_h(const QddSurface *s) { return s->desc.dwHeight; }
static inline bool qdd_surf_is_primary(const QddSurface *s) { return (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0; }
static inline bool qdd_surf_busy(const QddSurface *s) { return s->locked || s->dc; }
void qdd_surf_touch(QddSurface *s);   /* contents changed: version++ */

/* Creates the surface(s) a CreateSurface describes (validated desc). */
HRESULT qdd_surface_create(QddDraw *dd, int v, const DDSURFACEDESC2 *in, QddSurface **out);
void qdd_surface_mark_lost(QddDraw *dd, bool lost_primary_only);

/* Location management.  Make the system-memory copy current (readback). */
HRESULT qdd_buf_sys(QddSurface *s);
/* The CPU wrote the system-memory copy: it is now the only current one. */
void qdd_buf_sys_written(QddSurface *s);
/* Frees a buffer's memory and Direct3D 9 objects. */
void qdd_buf_free(QddBuffer *b);

/* The palette entries for the pixels of s (P8): its palette, else the
 * primary's, else NULL. */
const PALETTEENTRY *qdd_surf_pal_entries(QddSurface *s);

/* ------------------------------------------------------------------------ */
/* Palettes, clippers                                                        */

class QddPaletteIface final : public IDirectDrawPalette {
public:
    QddPalette *p;
    STDMETHOD(QueryInterface)(REFIID riid, void **out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetCaps)(DWORD *caps);
    STDMETHOD(GetEntries)(DWORD flags, DWORD base, DWORD count, PALETTEENTRY *entries);
    STDMETHOD(Initialize)(IDirectDraw *ddraw, DWORD flags, PALETTEENTRY *entries);
    STDMETHOD(SetEntries)(DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries);
};

struct QddPalette {
    QddPaletteIface iface;
    LONG ref;
    QddDraw *dd;
    IUnknown *dd_ref;         /* IDirectDraw4/7 that created it */
    DWORD caps;
    UINT count;               /* entries: 2, 4, 16, 256 */
    PALETTEENTRY entries[256];
    DWORD version;            /* SetEntries count, for P8 uploads */
    struct QddInvPal *inv;    /* qdd_palette_nearest cache */
};
/* The palette index nearest to rgb (0x00RRGGBB), for reading rendered
 * P8 surfaces back; cached per 15-bit color, reset by SetEntries. */
BYTE qdd_palette_nearest(QddPalette *p, DWORD rgb);
HRESULT qdd_palette_create(QddDraw *dd, int v, DWORD flags, const PALETTEENTRY *entries, QddPalette **out);
QddPalette *qdd_palette_from(const void *iface);

class QddClipperIface final : public IDirectDrawClipper {
public:
    QddClipper *c;
    STDMETHOD(QueryInterface)(REFIID riid, void **out);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetClipList)(RECT *rect, RGNDATA *data, DWORD *size);
    STDMETHOD(GetHWnd)(HWND *wnd);
    STDMETHOD(Initialize)(IDirectDraw *ddraw, DWORD flags);
    STDMETHOD(IsClipListChanged)(WINBOOL *changed);
    STDMETHOD(SetClipList)(RGNDATA *data, DWORD flags);
    STDMETHOD(SetHWnd)(DWORD flags, HWND wnd);
};

struct QddClipper {
    QddClipperIface iface;
    LONG ref;
    bool initialized;
    HWND window;
    HRGN region;              /* SetClipList (destination coordinates), or NULL */
    IUnknown *dd_ref;         /* IDirectDraw4/7 that created it */
};
HRESULT qdd_clipper_create(DWORD flags, QddClipper **out, bool initialized);
/* References held by surfaces: release validates the clipper first. */
void qdd_clipper_addref(QddClipper *c);
void qdd_clipper_release(QddClipper *c);
QddClipper *qdd_clipper_from(const void *iface);
/* The clip region in screen coordinates (window's visible client area or
 * the clip list); NULL with *none = true when nothing clips; NULL with
 * *none = false on failure. Caller deletes the region. */
HRGN qdd_clipper_region(QddClipper *c, bool *none);

/* ------------------------------------------------------------------------ */
/* 2D operations (qdd_blt.cpp)                                               */

/* call_v: the interface version of the call (destination keys differ). */
HRESULT qdd_blt(QddSurface *dst, const RECT *dr, QddSurface *src, const RECT *sr, DWORD flags, const DDBLTFX *fx,
                int call_v);
HRESULT qdd_bltfast(QddSurface *dst, DWORD x, DWORD y, QddSurface *src, const RECT *sr, DWORD trans, int call_v);

/* ------------------------------------------------------------------------ */
/* Presentation and Direct3D 9 (qdd_gpu.cpp)                                 */

/* A write to the primary (dst rect in primary coordinates, NULL: all). */
void qdd_primary_written(QddSurface *s, const RECT *r);
/* Before CPU reads of the primary in GDI mode: pull r from the screen. */
void qdd_primary_read(QddSurface *s, const RECT *r);
/* Presents the front buffer through QD3D now (Flip). */
HRESULT qdd_present(QddDraw *dd);
/* Releases dd's Direct3D 9 device and every surface's Direct3D 9 copies. */
void qdd_gpu_release(QddDraw *dd);
/* The surface's Direct3D 9 copy, created and made current. */
IDirect3DSurface9 *qdd_gpu_surface(QddSurface *s);
/* Starts the presenter thread when needed (QD3D presentation of a
 * primary written outside Flip). */
void qdd_presenter_kick(void);
void qdd_presenter_stop(void);

/* ------------------------------------------------------------------------ */
/* Direct3D 9 passes the front end draws itself (qdd_pass.cpp)               */

/* z-buffer copies whose Direct3D 9 surface is lockable (D32F_LOCKABLE,
 * D16_LOCKABLE) or an INTZ texture level (read and written by shaders):
 * moves the depth between the system-memory copy and dev9. */
bool qdd_depth_upload(QddSurface *s);
bool qdd_depth_readback(QddSurface *s);
/* Draws a P8 surface's Direct3D 9 copy from its indices and palette on the
 * GPU (indices: the system-memory copy changed); false: not available. */
bool qdd_p8_upload(QddSurface *s, bool indices);
/* Draws src (rect r) onto dst through dd's gamma ramp; false: not done. */
bool qdd_gamma_pass(QddDraw *dd, IDirect3DSurface9 *src, const RECT *r, IDirect3DSurface9 *dst);
/* Releases the pass resources of a buffer / of dd (before dev9 goes). */
void qdd_pass_buffer_free(QddBuffer *b);
void qdd_pass_free(QddDraw *dd);

/* ------------------------------------------------------------------------ */
/* Misc                                                                      */

extern const GUID QDD_IID_IDirectDraw3;
/* C callers pass a NULL REFIID; the reference then sits at address 0. */
static inline bool qdd_iid_null(const IID *p)
{
    const IID *volatile v = p;
    return v == NULL;
}
HRESULT qdd_copy_desc_out(const DDSURFACEDESC2 *in, void *out, DWORD out_size);
/* GDI region data of the window's visible client area in screen coordinates. */
bool qdd_rect_valid(const RECT *r, DWORD w, DWORD h);
DWORD qdd_tick_ms(void);

#endif /* QDD_PRIV_H */
