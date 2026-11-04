/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: the DirectDraw core's internal API for the Direct3D 7 device
 * (qdd_d3d7*.cpp).  docs/design/ddraw-remoting.md describes the design.
 *
 * Headers: d3d9.h and ddraw.h live in the global namespace; the Direct3D 7
 * headers collide with d3d9types.h and are only ever included as
 * `namespace d7 { #include <d3d.h> }`.  Nothing here needs d3d.h.
 *
 * Objects.  QddDraw is one DirectDraw object (every IDirectDraw* and
 * IDirect3D* interface of it), QddSurface one DirectDraw surface (every
 * IDirectDrawSurface* interface of it).  Both are opaque here.
 *
 * Locking.  Every DirectDraw entry point runs under one recursive process
 * lock (the lock AcquireDDThreadLock takes).  Direct3D 7 entry points take
 * it too (qdd_lock/qdd_unlock) before touching any QddDraw or QddSurface;
 * everything below expects it held.
 *
 * Locations.  Every surface has a system-memory copy in its native layout
 * (what Lock returns) and, when needed, a Direct3D 9 copy on the shared
 * device of its DirectDraw object: a render target for DDSCAPS_3DDEVICE
 * surfaces (level 0 of a D3DUSAGE_RENDERTARGET texture when the surface is
 * also a DDSCAPS_TEXTURE), a depth stencil for DDSCAPS_ZBUFFER surfaces,
 * and the presentation copy of the primary chain.  The core tracks which
 * copies are current, uploads before Direct3D 9 work reads a surface the
 * CPU changed, and reads back before CPU work (Lock, Blt, GetDC, ...) on a
 * surface the GPU changed.  Textures belong to the Direct3D 7 device: it
 * reads the current system-memory bits (qdd_surface_map) and keeps its own
 * Direct3D 9 textures, using qdd_surface_version to skip unchanged ones.
 *
 * Device state.  The core's Direct3D 9 work never leaves device state
 * changed: it uses StretchRect, ColorFill, UpdateSurface,
 * GetRenderTargetData, Present and, for depth fills, Clear with the depth
 * stencil and viewport restored.  The only exception is Reset, bracketed by
 * qdd_d3d7_before_reset / qdd_d3d7_after_reset.
 */
#ifndef QDD_H
#define QDD_H

#include <windows.h>
#include <d3d9.h>
#include <ddraw.h>

struct QddDraw;
struct QddSurface;

/* ------------------------------------------------------------------------ */
/* Process-wide                                                              */

/* The DirectDraw lock (recursive). */
void qdd_lock(void);
void qdd_unlock(void);

/* Debug output (OutputDebugString, and the file QDD_LOG names). */
void qdd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Rare events: also one line in %TEMP%\qd3d.log ("YYYY-MM-DD hh:mm:ss pid
 * exe: message").  Never per call or per frame. */
void qdd_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Direct3D interface IDs (DirectX 7 SDK values), for code without d3d.h. */
extern const GUID QDD_IID_IDirect3D;
extern const GUID QDD_IID_IDirect3D2;
extern const GUID QDD_IID_IDirect3D3;
extern const GUID QDD_IID_IDirect3D7;

/* ------------------------------------------------------------------------ */
/* DirectDraw object                                                         */

/* Its IDirectDraw7 interface, no reference added.  The aggregated
 * IDirect3D7 forwards AddRef/Release/QueryInterface to it, as native does
 * (one count shared by IDirectDraw7 and IDirect3D7). */
IDirectDraw7 *qdd_draw_iface7(QddDraw *dd);
/* The registered DirectDraw object behind any of its IDirectDraw,
 * IDirectDraw2, IDirectDraw4 or IDirectDraw7 pointers, else NULL; p is
 * never called or dereferenced. */
QddDraw *qdd_draw_lookup(const void *p);
/* The QD3D IDirect3D9 the object was created on (caps, CheckDeviceFormat).
 * No reference added; lives as long as the object. */
IDirect3D9 *qdd_draw_d3d9(QddDraw *dd);
/* The object's one shared Direct3D 9 device, created on first use: windowed
 * on the cooperative-level window (else a hidden window of the core),
 * D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, no automatic
 * depth stencil, X8R8G8B8 back buffer used only for presentation.  No
 * reference added; lives as long as the object; NULL when it cannot be
 * created. */
IDirect3DDevice9 *qdd_draw_device9(QddDraw *dd);
/* DDSCL_* flags of the last successful SetCooperativeLevel (0 before). */
DWORD qdd_draw_coop(QddDraw *dd);
/* The cooperative-level window (NULL before SetCooperativeLevel or with a
 * NULL window). */
HWND qdd_draw_window(QddDraw *dd);
/* A pointer the Direct3D 7 code owns per DirectDraw object (NULL at
 * creation; qdd_d3d7_draw_destroy runs before the object is freed). */
void **qdd_draw_d3d7_slot(QddDraw *dd);
/* Its IDirectDraw (1), IDirectDraw2 (2), IDirectDraw4 (4) or IDirectDraw7
 * (7) interface, no reference added. */
void *qdd_draw_iface(QddDraw *dd, int version);
/* Whether the object was created by DirectDrawCreateEx (or as
 * CLSID_DirectDraw7 / IID_IDirectDraw7).  The core answers IID_IDirect3D7
 * only on such objects and IID_IDirect3D/2/3 only on the others (native,
 * Wine's refcount.c), so qdd_d3d7_query only sees those combinations. */
bool qdd_draw_created_ex(QddDraw *dd);
/* A Direct3D device was created (+1) or destroyed (-1).  While any exists
 * in the process, the primary presents through QD3D even when windowed
 * (presentation rules, docs/design/ddraw-remoting.md). */
void qdd_draw_3d_devices(QddDraw *dd, int delta);
/* Whether a video-memory texture of format pf is creatable on dd: the
 * formats the device samples natively (Direct3D 9 texture formats the
 * host supports: the RGB, luminance, alpha, bump and DXTn formats; not P8,
 * not YUV, not depth).  CreateSurface fails explicit DDSCAPS_VIDEOMEMORY
 * textures of other formats and places default-placement ones in system
 * memory (native, ddraw7.c:6598-6627); EnumTextureFormats lists exactly
 * the formats for which this is true.  System-memory textures of every
 * format stay creatable. */
bool qdd_draw_texture_format(QddDraw *dd, const DDPIXELFORMAT *pf);

/* ------------------------------------------------------------------------ */
/* Surfaces                                                                  */

/* The registered surface behind any of its IDirectDrawSurface,
 * IDirectDrawSurface2/3/4/7 pointers, else NULL; p is never called or
 * dereferenced. */
QddSurface *qdd_surface_lookup(const void *p);
/* Its IDirectDrawSurface7 interface, no reference added.  AddRef/Release
 * through it hold the surface (and everything it keeps alive) like an
 * application reference. */
IDirectDrawSurface7 *qdd_surface_iface7(QddSurface *s);
/* Its IDirectDrawSurface (1), 2, 3, 4 or 7 interface, no reference added. */
void *qdd_surface_iface(QddSurface *s, int version);
/* The IDirectDraw interface version whose CreateSurface made s: 1, 2, 4
 * or 7. */
int qdd_surface_create_version(QddSurface *s);
/* QueryInterface on surfaces created through IDirectDraw/2/4 (not 7) for
 * IIDs the core does not answer (IID_IDirect3DTexture/2, the Direct3D
 * device GUIDs): when the Direct3D code has set this hook (e.g. from a
 * static constructor) and it returns S_OK with a referenced pointer, that
 * is the answer; otherwise the core returns its native error.  Called
 * under the DirectDraw lock with *out NULL. */
typedef HRESULT (*QddSurfaceQueryHook)(QddSurface *s, REFIID riid, void **out);
extern QddSurfaceQueryHook qdd_d3d_surface_query;
QddDraw *qdd_surface_draw(QddSurface *s);
/* The surface's current description, as GetSurfaceDesc would report it
 * (DDSD_CAPS/HEIGHT/WIDTH/PIXELFORMAT always; DDSD_PITCH or DDSD_LINEARSIZE;
 * DDSD_MIPMAPCOUNT for mipmaps; DDSD_CKSRCBLT etc. with the keys set;
 * ddsCaps.dwCaps2 DDSCAPS2_*).  Valid until the next DirectDraw call on
 * the surface. */
const DDSURFACEDESC2 *qdd_surface_desc(QddSurface *s);
/* The first surface attached to s whose caps contain caps and caps2, as
 * GetAttachedSurface finds it, without a reference; NULL when none.
 * E.g. (rt, DDSCAPS_ZBUFFER, 0) the z-buffer, (back, DDSCAPS_BACKBUFFER, 0). */
QddSurface *qdd_surface_attached(QddSurface *s, DWORD caps, DWORD caps2);
/* Mip chains and cube maps: level `level` (0 = s itself) of face `face`
 * (0-5 in DDSCAPS2_CUBEMAP_POSITIVEX..NEGATIVEZ order; 0 for plain
 * textures) of the complex surface rooted at `root`, NULL when absent.
 * qdd_surface_levels: mip levels per face (1 without mipmaps). */
QddSurface *qdd_surface_sub(QddSurface *root, UINT face, UINT level);
UINT qdd_surface_levels(QddSurface *root);
/* The palette entries the surface's pixels map through: its own palette,
 * else (a P8 surface without one) none -> false.  out holds 256 entries;
 * entries beyond the palette's size are zero.  The palette's alpha
 * (DDPCAPS_ALPHA) is in peFlags. */
bool qdd_surface_palette(QddSurface *s, PALETTEENTRY out[256]);
/* Changes on every change of the surface's contents through any path
 * (Unlock after a write lock, Blt/BltFast/color fill destination,
 * ReleaseDC, Restore, SetSurfaceDesc, Flip rotation, GPU writes through
 * qdd_surface_d3d9) and on SetColorKey, SetPalette and SetEntries of its
 * palette.  Never 0. */
DWORD qdd_surface_version(QddSurface *s);
/* DDERR_SURFACELOST state (after a display mode or cooperative level
 * change, until Restore). */
bool qdd_surface_lost(QddSurface *s);
/* A pointer the Direct3D 7 code owns per surface (NULL at creation;
 * qdd_d3d7_surface_destroy runs before the surface is freed). */
void **qdd_surface_d3d7_slot(QddSurface *s);

/* The current system-memory bits (read back first when the GPU copy is
 * newer), with the surface's native pitch (bytes per row; per row of 4x4
 * blocks for DXTn).  write: the caller changes them, so at unmap the
 * system-memory copy becomes the only current one and the version
 * changes.  No DirectDraw lock rules apply (the surface may be locked by
 * the application).  Pair every successful map with qdd_surface_unmap. */
HRESULT qdd_surface_map(QddSurface *s, bool write, BYTE **bits, LONG *pitch);
void qdd_surface_unmap(QddSurface *s);

/* The Direct3D 9 copy backing s on qdd_draw_device9: the render target of
 * a DDSCAPS_3DDEVICE surface (system or video memory), the depth stencil of
 * a DDSCAPS_ZBUFFER surface, created on first use and made current (CPU
 * changes uploaded first).  write: the caller is about to change it, so it
 * becomes the only current copy (a later Lock/Blt/GetDC reads back) and
 * the version changes.  No reference added.  What backs a surface may
 * change at any DirectDraw call (Flip rotates the copies of a chain), so
 * call this before every use (it is cheap when nothing changed) and rebind
 * when the pointer differs.  NULL on failure (no device, unsupported
 * format, out of memory). */
IDirect3DSurface9 *qdd_surface_d3d9(QddSurface *s, bool write);
/* For a DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE surface: the render-target
 * texture whose level 0 qdd_surface_d3d9 returns (made current the same
 * way, read only), so render-to-texture samples it without a copy.  NULL
 * for other surfaces or on failure. */
IDirect3DTexture9 *qdd_surface_rt_texture(QddSurface *s);

/* ------------------------------------------------------------------------ */
/* Hooks the Direct3D 7 code implements (qdd_d3d7*.cpp)                      */

/* IDirectDraw/2/4/7::QueryInterface for IID_IDirect3D, 2, 3 and 7: S_OK
 * with a referenced interface in *out, or E_NOINTERFACE with *out NULL. */
HRESULT qdd_d3d7_query(QddDraw *dd, REFIID riid, void **out);
/* The DirectDraw object's last reference went away; runs before its
 * surfaces' Direct3D 9 copies and its device are released and before it is
 * freed. */
void qdd_d3d7_draw_destroy(QddDraw *dd);
/* The surface is about to be freed (its Direct3D 9 copies still exist). */
void qdd_d3d7_surface_destroy(QddSurface *s);
/* Around every IDirect3DDevice9::Reset of qdd_draw_device9 (presentation
 * size changes): after it the device has its default state and nothing
 * bound; Direct3D 9 objects survive (the QD3D host resets with ResetEx). */
void qdd_d3d7_before_reset(QddDraw *dd);
void qdd_d3d7_after_reset(QddDraw *dd);
/* Right before every Present and Reset of qdd_draw_device9: end any open
 * Direct3D 9 scene (Direct3D 9 refuses Present inside one). */
void qdd_d3d7_flush(QddDraw *dd);

#endif /* QDD_H */
