/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_smoke: deterministic DirectDraw scenes with exact pixel checks.
 *
 * Links against ddraw.dll by import, so it exercises whichever ddraw.dll the
 * loader picks: the system runtime (the oracle every expectation here was
 * measured on: the 32-bit ddraw.dll of Windows 11), or QD3D's ddraw.dll
 * when that file sits beside the executable.  The first output line names
 * the loaded ddraw.dll; once the first DirectDraw object exists, the
 * d3d9.dll in the process (the loopback runner looks for its run
 * directory's copy; printed again at exit) and the module the object's
 * vtable lives in.  --expect-local / --expect-stock make the location a
 * hard check.
 *
 * Every 2D surface is explicitly DDSCAPS_SYSTEMMEMORY (except where stock
 * only accepts a format elsewhere, noted there), so on the stock runtime
 * the work goes through its software blitter and the results do not depend
 * on the display driver; the QD3D front end keeps all surfaces in guest
 * system memory anyway.
 *
 * Output per scene: "PASS <scene>", "FAIL <scene>: <first failure>" (every
 * failure is also printed indented above it), or "SKIP <scene>: <reason>"
 * (only where the runtime's caps lack the feature).  Exit code 0 when no
 * scene failed.
 *
 * Options:
 *   --expect-local   fail unless ddraw.dll (and the DirectDraw object) is
 *                    the one beside the executable
 *   --expect-stock   fail unless ddraw.dll is the system one
 *   --only a,b       run only the named scenes
 *   --fullscreen     add the exclusive-mode scenes; they change the display
 *                    mode, so only ever inside the VM
 *   --dump-caps      print the HAL and HEL caps
 *
 * Deliberately unconstrained (stock answers depend on the driver or on the
 * ddraw.dll version, so nothing here asserts them): exact pitches and Blt
 * results of video-memory surfaces (stock only creates YUY2/UYVY offscreen
 * surfaces there); the X byte of X8R8G8B8 in color-keyed blits; the
 * pointer a sub-rect Lock returns for DXTn surfaces (stock: base + top *
 * dwLinearSize, outside the surface); GetDC on a P8 surface without a
 * palette; the IDirectDraw7 count while a mipmap chain exists (stock adds
 * one or two references depending on the object's history; it drops them
 * all with the chain, which is asserted); destination color keys are
 * tested only where the caps advertise DDCKEYCAPS_DESTBLT (not on this
 * host).
 */
#include <windows.h>
#include <ddraw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 256
#define WIN_H 256

static HWND g_wnd;
static IDirectDraw7 *g_dd;
static DDCAPS g_hal, g_hel;
static const char *g_only;
static int g_scenes, g_failed, g_skipped;

/* The running scene. */
static int g_fails;
static char g_first[512];
static char g_skip[256];

/* ------------------------------------------------------------------------ */
/* Reporting                                                                 */

static const char *hrname(HRESULT hr)
{
    switch ((unsigned long)hr) {
#define N(x)                  \
    case (unsigned long)(x): \
        return #x
        N(DD_OK);
        N(DDERR_BLTFASTCANTCLIP);
        N(DDERR_CANNOTATTACHSURFACE);
        N(DDERR_CANTCREATEDC);
        N(DDERR_CLIPPERISUSINGHWND);
        N(DDERR_DCALREADYCREATED);
        N(DDERR_HWNDALREADYSET);
        N(DDERR_INVALIDCAPS);
        N(DDERR_INVALIDCLIPLIST);
        N(DDERR_INVALIDMODE);
        N(DDERR_INVALIDOBJECT);
        N(DDERR_INVALIDPIXELFORMAT);
        N(DDERR_INVALIDRECT);
        N(DDERR_NOBLTHW);
        N(DDERR_NOCLIPLIST);
        N(DDERR_NOCOLORKEY);
        N(DDERR_NOCOLORKEYHW);
        N(DDERR_NOCOOPERATIVELEVELSET);
        N(DDERR_NODC);
        N(DDERR_NOEXCLUSIVEMODE);
        N(DDERR_NOFLIPHW);
        N(DDERR_NOMIRRORHW);
        N(DDERR_NOPALETTEATTACHED);
        N(DDERR_NOSTRETCHHW);
        N(DDERR_NOTFLIPPABLE);
        N(DDERR_NOTFOUND);
        N(DDERR_NOTLOCKED);
        N(DDERR_NOTPALETTIZED);
        N(DDERR_SURFACEBUSY);
        N(DDERR_SURFACELOST);
        N(DDERR_UNSUPPORTEDMODE);
        N(DDERR_WASSTILLDRAWING);
        N(DDERR_WRONGMODE);
        N(E_NOINTERFACE);
        N(E_FAIL);
        N(E_OUTOFMEMORY);
        N(CLASS_E_NOAGGREGATION);
#undef N
    case (unsigned long)E_INVALIDARG:
        return "DDERR_INVALIDPARAMS";
    case (unsigned long)E_NOTIMPL:
        return "DDERR_UNSUPPORTED";
    }
    static char buf[4][16];
    static unsigned next;
    char *b = buf[next++ & 3];
    snprintf(b, sizeof(buf[0]), "0x%08lx", (unsigned long)hr);
    return b;
}

static int fail(const char *fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));
static int fail(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!g_fails++)
        memcpy(g_first, msg, sizeof(msg));
    printf("    %s\n", msg);
    return 0;
}

static void skip(const char *fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));
static void skip(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_skip, sizeof(g_skip), fmt, ap);
    va_end(ap);
}

static int expect_hr(const char *what, HRESULT got, HRESULT want)
{
    if (got == want)
        return 1;
    return fail("%s: %s, expected %s", what, hrname(got), hrname(want));
}

static int check_hr(const char *what, HRESULT hr)
{
    if (SUCCEEDED(hr))
        return 1;
    return fail("%s: %s", what, hrname(hr));
}

/* ------------------------------------------------------------------------ */
/* COM helpers                                                               */

template <class T> class Com {
public:
    T *p;
    Com() : p(NULL) {}
    ~Com() { reset(); }
    void reset()
    {
        if (p)
            p->Release();
        p = NULL;
    }
    T **out()
    {
        reset();
        return &p;
    }
    T *operator->() const { return p; }
    operator T *() const { return p; }

private:
    Com(const Com &);
    Com &operator=(const Com &);
};

/* The interface's own reference count (AddRef + Release). */
static ULONG refs(void *p)
{
    ((IUnknown *)p)->AddRef();
    return ((IUnknown *)p)->Release();
}

static int expect_refs(const char *what, void *p, ULONG want)
{
    ULONG got = refs(p);
    if (got == want)
        return 1;
    return fail("%s: refcount %lu, expected %lu", what, got, want);
}

/* QueryInterface that must succeed. */
static int qi(void *obj, REFIID iid, void **out, const char *what)
{
    *out = NULL;
    HRESULT hr = ((IUnknown *)obj)->QueryInterface(iid, out);
    if (hr == S_OK && *out)
        return 1;
    *out = NULL;
    return fail("QueryInterface %s: %s", what, hrname(hr));
}

/* QueryInterface returns exactly the existing interface `want` (references
 * restored afterwards). */
static int qi_same(void *obj, REFIID iid, void *want, const char *what)
{
    void *p = NULL;
    HRESULT hr = ((IUnknown *)obj)->QueryInterface(iid, &p);
    if (hr != S_OK || !p)
        return fail("QueryInterface %s: %s", what, hrname(hr));
    ((IUnknown *)p)->Release();
    if (p != want)
        return fail("QueryInterface %s returned %p, expected the existing interface %p", what, p, want);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Formats and surfaces                                                      */

struct Fmt {
    const char *name;
    DWORD flags, fourcc, bits, r, g, b, a;
    int bytes; /* bytes per pixel (YUY2/UYVY: 2); 0 for DXTn */
    int block; /* DXTn: bytes per 4x4 block */
};

static const Fmt F_P8 = {"P8", DDPF_RGB | DDPF_PALETTEINDEXED8, 0, 8, 0, 0, 0, 0, 1, 0};
static const Fmt F_R5G6B5 = {"R5G6B5", DDPF_RGB, 0, 16, 0xf800, 0x07e0, 0x001f, 0, 2, 0};
static const Fmt F_X1R5G5B5 = {"X1R5G5B5", DDPF_RGB, 0, 16, 0x7c00, 0x03e0, 0x001f, 0, 2, 0};
static const Fmt F_A1R5G5B5 = {"A1R5G5B5", DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0x7c00, 0x03e0, 0x001f, 0x8000, 2, 0};
static const Fmt F_A4R4G4B4 = {"A4R4G4B4", DDPF_RGB | DDPF_ALPHAPIXELS, 0, 16, 0x0f00, 0x00f0, 0x000f, 0xf000, 2, 0};
static const Fmt F_R8G8B8 = {"R8G8B8", DDPF_RGB, 0, 24, 0xff0000, 0xff00, 0xff, 0, 3, 0};
static const Fmt F_X8R8G8B8 = {"X8R8G8B8", DDPF_RGB, 0, 32, 0xff0000, 0xff00, 0xff, 0, 4, 0};
static const Fmt F_A8R8G8B8 = {"A8R8G8B8", DDPF_RGB | DDPF_ALPHAPIXELS, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000u, 4, 0};
static const Fmt F_DXT1 = {"DXT1", DDPF_FOURCC, MAKEFOURCC('D', 'X', 'T', '1'), 0, 0, 0, 0, 0, 0, 8};
static const Fmt F_DXT3 = {"DXT3", DDPF_FOURCC, MAKEFOURCC('D', 'X', 'T', '3'), 0, 0, 0, 0, 0, 0, 16};
static const Fmt F_DXT5 = {"DXT5", DDPF_FOURCC, MAKEFOURCC('D', 'X', 'T', '5'), 0, 0, 0, 0, 0, 0, 16};
static const Fmt F_YUY2 = {"YUY2", DDPF_FOURCC, MAKEFOURCC('Y', 'U', 'Y', '2'), 0, 0, 0, 0, 0, 2, 0};
static const Fmt F_UYVY = {"UYVY", DDPF_FOURCC, MAKEFOURCC('U', 'Y', 'V', 'Y'), 0, 0, 0, 0, 0, 2, 0};

#define SYS_PLAIN (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY)
#define SYS_TEXTURE (DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY)

static void desc_init(DDSURFACEDESC2 *d)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
}

static void set_format(DDPIXELFORMAT *pf, const Fmt *f)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = f->flags;
    pf->dwFourCC = f->fourcc;
    pf->dwRGBBitCount = f->bits;
    pf->dwRBitMask = f->r;
    pf->dwGBitMask = f->g;
    pf->dwBBitMask = f->b;
    pf->dwRGBAlphaBitMask = f->a;
}

static HRESULT create_on(IDirectDraw7 *dd, DWORD caps, const Fmt *f, DWORD w, DWORD h, IDirectDrawSurface7 **out)
{
    DDSURFACEDESC2 d;
    desc_init(&d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = caps;
    d.dwWidth = w;
    d.dwHeight = h;
    if (f) {
        d.dwFlags |= DDSD_PIXELFORMAT;
        set_format(&d.ddpfPixelFormat, f);
    }
    *out = NULL;
    return dd->CreateSurface(&d, out, NULL);
}

/* A surface on g_dd, or NULL after reporting the failure. */
static IDirectDrawSurface7 *make(DWORD caps, const Fmt *f, DWORD w, DWORD h)
{
    IDirectDrawSurface7 *s;
    HRESULT hr = create_on(g_dd, caps, f, w, h, &s);
    if (FAILED(hr)) {
        fail("CreateSurface(%s %lux%lu caps 0x%lx): %s", f ? f->name : "default", (unsigned long)w, (unsigned long)h,
             (unsigned long)caps, hrname(hr));
        return NULL;
    }
    return s;
}

static DWORD get_px(const BYTE *p, int bytes)
{
    switch (bytes) {
    case 1:
        return p[0];
    case 2:
        return p[0] | (DWORD)p[1] << 8;
    case 3:
        return p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16;
    default:
        return p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
    }
}

static void put_px(BYTE *p, int bytes, DWORD v)
{
    for (int i = 0; i < bytes; ++i)
        p[i] = (BYTE)(v >> (8 * i));
}

static DWORD bytes_mask(int bytes)
{
    return bytes >= 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1;
}

/* Deterministic per-pixel values. */
static DWORD hash3(DWORD seed, DWORD x, DWORD y)
{
    DWORD v = seed * 0x9e3779b1u + x * 0x85ebca77u + y * 0xc2b2ae3du;
    v ^= v >> 15;
    v *= 0x2c1b3c6du;
    v ^= v >> 12;
    v *= 0x297a2d39u;
    v ^= v >> 15;
    return v;
}

/* w x h pixel values, row-major. */
struct Img {
    int w, h;
    DWORD *px;
    Img(int w_, int h_) : w(w_), h(h_), px((DWORD *)calloc((size_t)(w_ > 0 && h_ > 0 ? w_ * h_ : 1), sizeof(DWORD))) {}
    ~Img() { free(px); }
    DWORD &at(int x, int y) { return px[y * w + x]; }

private:
    Img(const Img &);
    Img &operator=(const Img &);
};

static void fill_hash(Img *im, DWORD seed, DWORD mask)
{
    for (int y = 0; y < im->h; ++y)
        for (int x = 0; x < im->w; ++x)
            im->at(x, y) = hash3(seed, (DWORD)x, (DWORD)y) & mask;
}

static int put_image(IDirectDrawSurface7 *s, int bytes, const Img *im, const char *what)
{
    DDSURFACEDESC2 d;
    desc_init(&d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT, NULL);
    if (FAILED(hr))
        return fail("%s: Lock: %s", what, hrname(hr));
    for (int y = 0; y < im->h; ++y)
        for (int x = 0; x < im->w; ++x)
            put_px((BYTE *)d.lpSurface + y * d.lPitch + x * bytes, bytes, im->px[y * im->w + x]);
    hr = s->Unlock(NULL);
    return check_hr(what, hr);
}

static int get_image(IDirectDrawSurface7 *s, int bytes, Img *im, const char *what)
{
    DDSURFACEDESC2 d;
    desc_init(&d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr))
        return fail("%s: Lock: %s", what, hrname(hr));
    for (int y = 0; y < im->h; ++y)
        for (int x = 0; x < im->w; ++x)
            im->px[y * im->w + x] = get_px((const BYTE *)d.lpSurface + y * d.lPitch + x * bytes, bytes);
    hr = s->Unlock(NULL);
    return check_hr(what, hr);
}

/* Every pixel equal under mask; reports the first difference. */
static int compare(const char *what, const Img *got, const Img *want, DWORD mask)
{
    int bad = 0, fx = 0, fy = 0;
    for (int y = 0; y < want->h; ++y)
        for (int x = 0; x < want->w; ++x)
            if ((got->px[y * got->w + x] ^ want->px[y * want->w + x]) & mask) {
                if (!bad++) {
                    fx = x;
                    fy = y;
                }
            }
    if (!bad)
        return 1;
    return fail("%s: %d of %d pixels differ, first (%d,%d) = 0x%08lx, expected 0x%08lx", what, bad,
                want->w * want->h, fx, fy, (unsigned long)(got->px[fy * got->w + fx] & mask),
                (unsigned long)(want->px[fy * want->w + fx] & mask));
}

/* Surface contents equal `want` under mask. */
static int expect_image(IDirectDrawSurface7 *s, int bytes, const Img *want, DWORD mask, const char *what)
{
    Img got(want->w, want->h);
    if (!get_image(s, bytes, &got, what))
        return 0;
    return compare(what, &got, want, mask);
}

static void init_fx(DDBLTFX *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->dwSize = sizeof(*fx);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ------------------------------------------------------------------------ */
/* qi: COM identity and reference counts                                     */

static const GUID kIID_Unknown = {0x5a3c9e21, 0x7b44, 0x4c1e, {0x9d, 0x2f, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};

/* DDSURFACEDESC (v1) for an IDirectDraw/IDirectDraw2 CreateSurface. */
static void desc1_plain(DDSURFACEDESC *d, DWORD w, DWORD h)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d->dwWidth = w;
    d->dwHeight = h;
    d->ddsCaps.dwCaps = SYS_PLAIN;
    set_format(&d->ddpfPixelFormat, &F_X8R8G8B8);
}

static void desc2_plain(DDSURFACEDESC2 *d, DWORD w, DWORD h)
{
    desc_init(d);
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d->dwWidth = w;
    d->dwHeight = h;
    d->ddsCaps.dwCaps = SYS_PLAIN;
    set_format(&d->ddpfPixelFormat, &F_X8R8G8B8);
}

struct NamedIid {
    const IID *iid;
    const char *name;
};

/* The surface interfaces; [0] is the one IUnknown answers with. */
static const NamedIid k_surface_iids[] = {
    {&IID_IDirectDrawSurface, "IDirectDrawSurface"},   {&IID_IDirectDrawSurface2, "IDirectDrawSurface2"},
    {&IID_IDirectDrawSurface3, "IDirectDrawSurface3"}, {&IID_IDirectDrawSurface4, "IDirectDrawSurface4"},
    {&IID_IDirectDrawSurface7, "IDirectDrawSurface7"}, {&IID_IDirectDrawGammaControl, "IDirectDrawGammaControl"},
};
#define N_SURFACE_IIDS 6

/* Interfaces a plain surface does not have.  Stock answers E_NOINTERFACE
 * with *out = NULL for every one of them, whichever IDirectDraw version
 * created the surface.  (Wine's test_surface_qi expects E_INVALIDARG from
 * IDirectDraw..IDirectDraw4 textures after a Direct3D device was created on
 * the object; with no Direct3D involvement this host's runtime answers
 * E_NOINTERFACE throughout, so that is what is asserted.) */
static const NamedIid k_foreign_iids[] = {
    {&IID_IDirectDrawPalette, "IDirectDrawPalette"}, {&IID_IDirectDrawClipper, "IDirectDrawClipper"},
    {&IID_IDirectDraw7, "IDirectDraw7"},             {&IID_IDirectDraw, "IDirectDraw"},
    {&IID_IDirectDrawColorControl, "IDirectDrawColorControl"}, {&kIID_Unknown, "an unregistered IID"},
};
#define N_FOREIGN_IIDS 6

/* base: the surface as created, through interface k_surface_iids[created]. */
static void surface_qi(const char *label, IUnknown *base, int created)
{
    IUnknown *ifc[N_SURFACE_IIDS] = {0};
    char what[160];
    int i, j;

    for (i = 0; i < N_SURFACE_IIDS; ++i) {
        snprintf(what, sizeof(what), "%s -> %s", label, k_surface_iids[i].name);
        if (!qi(base, *k_surface_iids[i].iid, (void **)&ifc[i], what))
            goto out;
    }
    if (ifc[created] != base)
        fail("%s: QueryInterface for its own interface returned %p, not %p", label, (void *)ifc[created], (void *)base);
    /* Each interface has its own count: 1 from the QueryInterface, plus the
     * creation reference on the created one. */
    for (i = 0; i < N_SURFACE_IIDS; ++i) {
        snprintf(what, sizeof(what), "%s %s", label, k_surface_iids[i].name);
        expect_refs(what, ifc[i], i == created ? 2 : 1);
    }
    /* One object: every interface answers with the same pointers. */
    for (i = 0; i < N_SURFACE_IIDS; ++i)
        for (j = 0; j < N_SURFACE_IIDS; ++j) {
            snprintf(what, sizeof(what), "%s %s -> %s", label, k_surface_iids[i].name, k_surface_iids[j].name);
            qi_same(ifc[i], *k_surface_iids[j].iid, ifc[j], what);
        }
    /* IUnknown is the IDirectDrawSurface interface and shares its count. */
    for (i = 0; i < N_SURFACE_IIDS; ++i) {
        IUnknown *unk = NULL;
        snprintf(what, sizeof(what), "%s %s -> IUnknown", label, k_surface_iids[i].name);
        if (!qi(ifc[i], IID_IUnknown, (void **)&unk, what))
            continue;
        if (unk != ifc[0])
            fail("%s returned %p, expected the IDirectDrawSurface interface %p", what, (void *)unk, (void *)ifc[0]);
        else
            expect_refs(what, ifc[0], created == 0 ? 3 : 2);
        unk->Release();
    }
    /* AddRef on one interface leaves the others alone. */
    ifc[3]->AddRef();
    for (i = 0; i < N_SURFACE_IIDS; ++i) {
        snprintf(what, sizeof(what), "%s %s after AddRef on IDirectDrawSurface4", label, k_surface_iids[i].name);
        expect_refs(what, ifc[i], (i == created ? 2 : 1) + (i == 3 ? 1 : 0));
    }
    ifc[3]->Release();
    /* Interfaces the surface does not have, through each of its interfaces. */
    for (i = 0; i < N_SURFACE_IIDS; ++i)
        for (j = 0; j < N_FOREIGN_IIDS; ++j) {
            void *p = (void *)1;
            HRESULT hr = ifc[i]->QueryInterface(*k_foreign_iids[j].iid, &p);
            snprintf(what, sizeof(what), "%s %s -> %s", label, k_surface_iids[i].name, k_foreign_iids[j].name);
            if (SUCCEEDED(hr) && p)
                ((IUnknown *)p)->Release();
            if (expect_hr(what, hr, E_NOINTERFACE) && p)
                fail("%s left *out = %p, expected NULL", what, p);
        }
out:
    for (i = 0; i < N_SURFACE_IIDS; ++i)
        if (ifc[i])
            ifc[i]->Release();
}

static void scene_qi(void)
{
    IDirectDraw7 *dd7 = NULL;
    IDirectDraw4 *dd4 = NULL;
    IDirectDraw2 *dd2 = NULL;
    IDirectDraw *dd1 = NULL;
    IDirectDraw *ddc = NULL;
    IDirectDraw7 *ddc7 = NULL;
    IDirectDrawPalette *pal = NULL;
    PALETTEENTRY pe[256];
    HRESULT hr;

    memset(pe, 0, sizeof(pe));
    hr = DirectDrawCreateEx(NULL, (void **)&dd7, IID_IDirectDraw7, NULL);
    if (FAILED(hr)) {
        fail("DirectDrawCreateEx(IID_IDirectDraw7): %s", hrname(hr));
        return;
    }
    expect_refs("IDirectDraw7 from DirectDrawCreateEx", dd7, 1);
    if (!qi(dd7, IID_IDirectDraw4, (void **)&dd4, "IDirectDraw7 -> IDirectDraw4") ||
        !qi(dd7, IID_IDirectDraw2, (void **)&dd2, "IDirectDraw7 -> IDirectDraw2") ||
        !qi(dd7, IID_IDirectDraw, (void **)&dd1, "IDirectDraw7 -> IDirectDraw"))
        goto out;

    /* Separate counts per interface, each starting at 1. */
    expect_refs("IDirectDraw7 after the QueryInterfaces", dd7, 1);
    expect_refs("IDirectDraw4 from QueryInterface", dd4, 1);
    expect_refs("IDirectDraw2 from QueryInterface", dd2, 1);
    expect_refs("IDirectDraw from QueryInterface", dd1, 1);
    /* One object: QueryInterface through any interface finds the others. */
    qi_same(dd1, IID_IDirectDraw7, dd7, "IDirectDraw -> IDirectDraw7");
    qi_same(dd2, IID_IDirectDraw4, dd4, "IDirectDraw2 -> IDirectDraw4");
    qi_same(dd4, IID_IDirectDraw, dd1, "IDirectDraw4 -> IDirectDraw");
    qi_same(dd7, IID_IDirectDraw2, dd2, "IDirectDraw7 -> IDirectDraw2");
    /* IUnknown is the IDirectDraw interface, sharing its count (stock, also
     * for an object made by DirectDrawCreateEx). */
    {
        IUnknown *unk = NULL;
        if (qi(dd7, IID_IUnknown, (void **)&unk, "IDirectDraw7 -> IUnknown")) {
            if ((void *)unk != (void *)dd1)
                fail("IDirectDraw7 -> IUnknown returned %p, expected the IDirectDraw interface %p", (void *)unk,
                     (void *)dd1);
            else
                expect_refs("IDirectDraw while IUnknown is held", dd1, 2);
            unk->Release();
        }
    }
    /* AddRef on one interface leaves the others alone. */
    dd4->AddRef();
    expect_refs("IDirectDraw4 after AddRef", dd4, 2);
    expect_refs("IDirectDraw7 after AddRef on IDirectDraw4", dd7, 1);
    expect_refs("IDirectDraw2 after AddRef on IDirectDraw4", dd2, 1);
    expect_refs("IDirectDraw after AddRef on IDirectDraw4", dd1, 1);
    dd4->Release();

    /* One object: the cooperative level set through IDirectDraw7 is the one
     * IDirectDraw sees. */
    hr = dd1->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL);
    expect_hr("IDirectDraw::CreatePalette before SetCooperativeLevel", hr, DDERR_NOCOOPERATIVELEVELSET);
    if (SUCCEEDED(hr))
        pal->Release();
    pal = NULL;
    if (!check_hr("IDirectDraw7::SetCooperativeLevel(DDSCL_NORMAL)", dd7->SetCooperativeLevel(g_wnd, DDSCL_NORMAL)))
        goto out;

    /* Surfaces, palettes and clippers made through IDirectDraw7 or
     * IDirectDraw4 hold a reference on that interface; through IDirectDraw2
     * or IDirectDraw none. */
    {
        DDSURFACEDESC2 d2;
        DDSURFACEDESC d1;
        desc2_plain(&d2, 16, 16);
        desc1_plain(&d1, 16, 16);
        IDirectDrawSurface7 *s7 = NULL;
        if (check_hr("IDirectDraw7::CreateSurface", dd7->CreateSurface(&d2, &s7, NULL))) {
            expect_refs("IDirectDraw7 holding a surface it created", dd7, 2);
            expect_refs("IDirectDraw4 while IDirectDraw7 has a surface", dd4, 1);
            expect_refs("the new IDirectDrawSurface7", s7, 1);
            s7->Release();
            expect_refs("IDirectDraw7 after its surface is released", dd7, 1);
        }
        IDirectDrawSurface4 *s4 = NULL;
        if (check_hr("IDirectDraw4::CreateSurface", dd4->CreateSurface(&d2, &s4, NULL))) {
            expect_refs("IDirectDraw4 holding a surface it created", dd4, 2);
            expect_refs("IDirectDraw7 while IDirectDraw4 has a surface", dd7, 1);
            s4->Release();
            expect_refs("IDirectDraw4 after its surface is released", dd4, 1);
        }
        IDirectDrawSurface *s1 = NULL;
        if (check_hr("IDirectDraw2::CreateSurface", dd2->CreateSurface(&d1, &s1, NULL))) {
            expect_refs("IDirectDraw2 after creating a surface", dd2, 1);
            expect_refs("IDirectDraw7 while IDirectDraw2 has a surface", dd7, 1);
            s1->Release();
        }
        if (check_hr("IDirectDraw::CreateSurface", dd1->CreateSurface(&d1, &s1, NULL))) {
            expect_refs("IDirectDraw after creating a surface", dd1, 1);
            expect_refs("IDirectDraw7 while IDirectDraw has a surface", dd7, 1);
            s1->Release();
        }
    }
    {
        struct {
            IUnknown *dd;
            const char *name;
            ULONG held;
        } v[4] = {
            {(IUnknown *)dd7, "IDirectDraw7", 2},
            {(IUnknown *)dd4, "IDirectDraw4", 2},
            {(IUnknown *)dd2, "IDirectDraw2", 1},
            {(IUnknown *)dd1, "IDirectDraw", 1},
        };
        char what[128];
        for (int i = 0; i < 4; ++i) {
            IDirectDrawPalette *p = NULL;
            /* CreatePalette and CreateClipper share their slots in every
             * IDirectDraw version's vtable. */
            hr = ((IDirectDraw *)v[i].dd)->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &p, NULL);
            snprintf(what, sizeof(what), "%s::CreatePalette", v[i].name);
            if (check_hr(what, hr)) {
                snprintf(what, sizeof(what), "%s holding a palette it created", v[i].name);
                expect_refs(what, v[i].dd, v[i].held);
                expect_refs("the new palette", p, 1);
                p->Release();
                snprintf(what, sizeof(what), "%s after its palette is released", v[i].name);
                expect_refs(what, v[i].dd, 1);
            }
            IDirectDrawClipper *c = NULL;
            hr = ((IDirectDraw *)v[i].dd)->CreateClipper(0, &c, NULL);
            snprintf(what, sizeof(what), "%s::CreateClipper", v[i].name);
            if (check_hr(what, hr)) {
                snprintf(what, sizeof(what), "%s holding a clipper it created", v[i].name);
                expect_refs(what, v[i].dd, v[i].held);
                expect_refs("the new clipper", c, 1);
                c->Release();
                snprintf(what, sizeof(what), "%s after its clipper is released", v[i].name);
                expect_refs(what, v[i].dd, 1);
            }
        }
    }

    /* Surface interfaces, for a surface made through each IDirectDraw. */
    {
        DDSURFACEDESC2 d2;
        DDSURFACEDESC d1;
        desc2_plain(&d2, 16, 16);
        desc1_plain(&d1, 16, 16);
        IDirectDrawSurface7 *s7 = NULL;
        IDirectDrawSurface4 *s4 = NULL;
        IDirectDrawSurface *s1 = NULL;
        if (check_hr("IDirectDraw7::CreateSurface", dd7->CreateSurface(&d2, &s7, NULL))) {
            surface_qi("IDirectDraw7 surface", s7, 4);
            expect_refs("IDirectDrawSurface7 after the QueryInterface tests", s7, 1);
            s7->Release();
        }
        if (check_hr("IDirectDraw4::CreateSurface", dd4->CreateSurface(&d2, &s4, NULL))) {
            surface_qi("IDirectDraw4 surface", s4, 3);
            s4->Release();
        }
        if (check_hr("IDirectDraw2::CreateSurface", dd2->CreateSurface(&d1, &s1, NULL))) {
            surface_qi("IDirectDraw2 surface", s1, 0);
            s1->Release();
        }
        if (check_hr("IDirectDraw::CreateSurface", dd1->CreateSurface(&d1, &s1, NULL))) {
            surface_qi("IDirectDraw surface", s1, 0);
            s1->Release();
        }
    }

    /* SetPalette holds a palette reference until the surface goes away;
     * GetPalette adds one; SetPalette(NULL) drops it. */
    {
        IDirectDrawSurface7 *s = NULL;
        if (!check_hr("CreateSurface(P8)", create_on(dd7, SYS_PLAIN, &F_P8, 16, 16, &s)) ||
            !check_hr("CreatePalette", dd7->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL))) {
            if (s)
                s->Release();
            goto out;
        }
        expect_refs("new palette", pal, 1);
        if (check_hr("SetPalette", s->SetPalette(pal))) {
            expect_refs("palette after SetPalette", pal, 2);
            expect_refs("surface after SetPalette", s, 1);
            IDirectDrawPalette *got = NULL;
            if (check_hr("GetPalette", s->GetPalette(&got))) {
                if (got != pal)
                    fail("GetPalette returned %p, expected %p", (void *)got, (void *)pal);
                expect_refs("palette while GetPalette's reference is held", pal, 3);
                got->Release();
            }
            check_hr("SetPalette(NULL)", s->SetPalette(NULL));
            expect_refs("palette after SetPalette(NULL)", pal, 1);
            check_hr("SetPalette again", s->SetPalette(pal));
        }
        s->Release();
        expect_refs("palette after its surface was released", pal, 1);
        pal->Release();
        pal = NULL;
    }
    /* SetClipper likewise. */
    {
        IDirectDrawSurface7 *s = NULL;
        IDirectDrawClipper *c = NULL;
        if (check_hr("CreateSurface", create_on(dd7, SYS_PLAIN, &F_X8R8G8B8, 16, 16, &s)) &&
            check_hr("CreateClipper", dd7->CreateClipper(0, &c, NULL))) {
            expect_refs("new clipper", c, 1);
            if (check_hr("SetClipper", s->SetClipper(c))) {
                expect_refs("clipper after SetClipper", c, 2);
                IDirectDrawClipper *got = NULL;
                if (check_hr("GetClipper", s->GetClipper(&got))) {
                    if (got != c)
                        fail("GetClipper returned %p, expected %p", (void *)got, (void *)c);
                    got->Release();
                }
            }
            s->Release();
            s = NULL;
            expect_refs("clipper after its surface was released", c, 1);
        }
        if (s)
            s->Release();
        if (c)
            c->Release();
    }
    /* GetAttachedSurface on a mipmapped texture adds a reference to the
     * level it returns (stock: a level the application never had starts at
     * 1, the chain's own), and returns the same interface every time. */
    {
        DDSURFACEDESC2 d;
        desc_init(&d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = SYS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
        d.dwWidth = 32;
        d.dwHeight = 32;
        d.dwMipMapCount = 3;
        set_format(&d.ddpfPixelFormat, &F_X8R8G8B8);
        IDirectDrawSurface7 *root = NULL, *l1 = NULL, *l1b = NULL, *l2 = NULL, *l3 = NULL;
        DDSCAPS2 caps;
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
        if (check_hr("CreateSurface(3-level mipmapped texture)", dd7->CreateSurface(&d, &root, NULL))) {
            if (check_hr("GetAttachedSurface(level 1)", root->GetAttachedSurface(&caps, &l1))) {
                expect_refs("level 1 after GetAttachedSurface", l1, 2);
                if (check_hr("GetAttachedSurface(level 1) again", root->GetAttachedSurface(&caps, &l1b))) {
                    if (l1b != l1)
                        fail("GetAttachedSurface returned %p, then %p", (void *)l1, (void *)l1b);
                    expect_refs("level 1 after a second GetAttachedSurface", l1, 3);
                    l1b->Release();
                }
                expect_refs("the root after GetAttachedSurface", root, 1);
                if (check_hr("GetAttachedSurface(level 2)", l1->GetAttachedSurface(&caps, &l2))) {
                    expect_refs("level 2 after GetAttachedSurface", l2, 2);
                    hr = l2->GetAttachedSurface(&caps, &l3);
                    expect_hr("GetAttachedSurface below the last level", hr, DDERR_NOTFOUND);
                    if (SUCCEEDED(hr))
                        l3->Release();
                    l2->Release();
                }
                l1->Release();
            }
            root->Release();
            expect_refs("IDirectDraw7 after the mipmap chain is released", dd7, 1);
        }
    }

    /* DirectDrawCreate: a separate object, same rules. */
    hr = DirectDrawCreate(NULL, &ddc, NULL);
    if (check_hr("DirectDrawCreate", hr)) {
        expect_refs("IDirectDraw from DirectDrawCreate", ddc, 1);
        if (ddc == dd1)
            fail("DirectDrawCreate returned the DirectDrawCreateEx object's interface %p", (void *)ddc);
        if (qi(ddc, IID_IDirectDraw7, (void **)&ddc7, "DirectDrawCreate object -> IDirectDraw7")) {
            expect_refs("its IDirectDraw7", ddc7, 1);
            expect_refs("its IDirectDraw", ddc, 1);
            if (ddc7 == dd7)
                fail("two DirectDraw objects share the IDirectDraw7 interface %p", (void *)ddc7);
            qi_same(ddc7, IID_IUnknown, ddc, "DirectDrawCreate object IDirectDraw7 -> IUnknown");
        }
        void *p = (void *)1;
        hr = ddc->QueryInterface(IID_IDirectDrawSurface, &p);
        if (expect_hr("IDirectDraw -> IDirectDrawSurface", hr, E_NOINTERFACE) && p)
            fail("IDirectDraw -> IDirectDrawSurface left *out = %p, expected NULL", p);
        if (SUCCEEDED(hr) && p)
            ((IUnknown *)p)->Release();
    }

out:
    if (ddc7)
        ddc7->Release();
    if (ddc) {
        ULONG left = ddc->Release();
        if (left)
            fail("DirectDrawCreate object: %lu references left after the final Release", left);
    }
    if (dd1)
        dd1->Release();
    if (dd2)
        dd2->Release();
    if (dd4)
        dd4->Release();
    if (dd7) {
        ULONG left = dd7->Release();
        if (left)
            fail("DirectDrawCreateEx object: %lu references left after the final Release", left);
    }
}

/* ------------------------------------------------------------------------ */
/* lock_formats                                                              */

/* Bytes that hold data in each row of a surface (rows of 4x4 blocks for
 * DXTn), and how many such rows. */
static void data_layout(const Fmt *f, DWORD w, DWORD h, DWORD *row_bytes, DWORD *rows)
{
    if (f->block) {
        *row_bytes = (w + 3) / 4 * (DWORD)f->block;
        *rows = (h + 3) / 4;
    } else if (f->fourcc) {
        *row_bytes = (w + 1) / 2 * 4; /* YUY2/UYVY: 4 bytes per 2 pixels */
        *rows = h;
    } else {
        *row_bytes = w * (DWORD)f->bytes;
        *rows = h;
    }
}

static BYTE pattern_byte(DWORD seed, DWORD row, DWORD i)
{
    return (BYTE)(hash3(seed, i, row) >> 7);
}

/* Writes (or, check != 0, verifies) the byte pattern through a Lock; rows
 * are `step` bytes apart (the pitch, or the packed block-row size). */
static int pattern_io(IDirectDrawSurface7 *s, const Fmt *f, DWORD w, DWORD h, DWORD seed, int check, LONG step,
                      const char *what)
{
    DDSURFACEDESC2 d;
    desc_init(&d);
    HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT | (check ? DDLOCK_READONLY : 0), NULL);
    if (FAILED(hr))
        return fail("%s: Lock: %s", what, hrname(hr));
    DWORD row_bytes, rows;
    data_layout(f, w, h, &row_bytes, &rows);
    int ok = 1;
    for (DWORD y = 0; y < rows && ok; ++y) {
        BYTE *row = (BYTE *)d.lpSurface + y * step;
        for (DWORD i = 0; i < row_bytes; ++i) {
            BYTE want = pattern_byte(seed, y, i);
            if (!check)
                row[i] = want;
            else if (row[i] != want) {
                ok = fail("%s: row %lu byte %lu = 0x%02x, expected 0x%02x", what, (unsigned long)y, (unsigned long)i,
                          row[i], want);
                break;
            }
        }
    }
    s->Unlock(NULL);
    return ok;
}

/* One format at one size: description, Lock, round trip, Blt copy and
 * sub-rect Lock.  caps: where the surface lives.  exact_pitch: the pitch
 * the rule predicts (0: only the minimum is known). */
static void lock_case(const Fmt *f, DWORD caps, DWORD w, DWORD h, DWORD exact_pitch, int check_blt_contents)
{
    char what[160];
    snprintf(what, sizeof(what), "%s %lux%lu", f->name, (unsigned long)w, (unsigned long)h);
    Com<IDirectDrawSurface7> a, b;
    HRESULT hr = create_on(g_dd, caps, f, w, h, a.out());
    if (FAILED(hr)) {
        fail("%s: CreateSurface(caps 0x%lx): %s", what, (unsigned long)caps, hrname(hr));
        return;
    }
    DWORD row_bytes, rows;
    data_layout(f, w, h, &row_bytes, &rows);

    DDSURFACEDESC2 sd;
    desc_init(&sd);
    if (!check_hr(what, a->GetSurfaceDesc(&sd)))
        return;
    DWORD size_flag = f->block ? DDSD_LINEARSIZE : DDSD_PITCH;
    DWORD other_flag = f->block ? DDSD_PITCH : DDSD_LINEARSIZE;
    if (!(sd.dwFlags & size_flag) || (sd.dwFlags & (other_flag | DDSD_LPSURFACE)))
        fail("%s: GetSurfaceDesc flags 0x%08lx, expected %s without %s or DDSD_LPSURFACE", what,
             (unsigned long)sd.dwFlags, f->block ? "DDSD_LINEARSIZE" : "DDSD_PITCH",
             f->block ? "DDSD_PITCH" : "DDSD_LINEARSIZE");
    if (sd.dwWidth != w || sd.dwHeight != h)
        fail("%s: GetSurfaceDesc size %lux%lu", what, (unsigned long)sd.dwWidth, (unsigned long)sd.dwHeight);
    if (exact_pitch && (DWORD)sd.lPitch != exact_pitch)
        fail("%s: %s %ld, expected %lu", what, f->block ? "dwLinearSize" : "lPitch", (long)sd.lPitch,
             (unsigned long)exact_pitch);
    if (!f->block && ((DWORD)sd.lPitch < row_bytes || (sd.lPitch & 3)))
        fail("%s: lPitch %ld, expected a DWORD multiple of at least %lu", what, (long)sd.lPitch,
             (unsigned long)row_bytes);

    DDSURFACEDESC2 ld;
    desc_init(&ld);
    hr = a->Lock(NULL, &ld, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) {
        fail("%s: Lock: %s", what, hrname(hr));
        return;
    }
    BYTE *base = (BYTE *)ld.lpSurface;
    a->Unlock(NULL);
    if (!base)
        fail("%s: Lock returned a NULL lpSurface", what);
    if ((ld.dwFlags & (size_flag | other_flag | DDSD_LPSURFACE)) != size_flag)
        fail("%s: Lock description flags 0x%08lx, expected %s without %s or DDSD_LPSURFACE", what,
             (unsigned long)ld.dwFlags, f->block ? "DDSD_LINEARSIZE" : "DDSD_PITCH",
             f->block ? "DDSD_PITCH" : "DDSD_LINEARSIZE");
    if (ld.lPitch != sd.lPitch)
        fail("%s: Lock lPitch %ld, GetSurfaceDesc %ld", what, (long)ld.lPitch, (long)sd.lPitch);
    if (!base || ld.lPitch != sd.lPitch)
        return;

    /* DXTn rows of blocks are packed (dwLinearSize = rows * row_bytes). */
    LONG step = f->block ? (LONG)row_bytes : sd.lPitch;
    DWORD seed = hash3(w, h, f->fourcc ? f->fourcc : f->bits);
    char sub[200];
    snprintf(sub, sizeof(sub), "%s write/read through Lock", what);
    if (!pattern_io(a, f, w, h, seed, 0, step, sub) || !pattern_io(a, f, w, h, seed, 1, step, sub))
        return;

    /* Blt into a second surface of the same format. */
    hr = create_on(g_dd, caps, f, w, h, b.out());
    if (FAILED(hr)) {
        fail("%s: second CreateSurface: %s", what, hrname(hr));
        return;
    }
    DDSURFACEDESC2 bd;
    desc_init(&bd);
    b->GetSurfaceDesc(&bd);
    LONG bstep = f->block ? (LONG)row_bytes : bd.lPitch;
    hr = b->Blt(NULL, a, NULL, DDBLT_WAIT, NULL);
    snprintf(sub, sizeof(sub), "%s Blt copy", what);
    if (check_hr(sub, hr) && check_blt_contents)
        pattern_io(b, f, w, h, seed, 1, bstep, sub);

    /* A sub-rect Lock points at the rect's origin: the bytes there are the
     * pattern's from (x0, y0) on, and for system-memory surfaces the pointer
     * is base + y0 * pitch + x0 * bytes (video-memory mappings may move
     * between Locks).  Not for DXTn: stock returns base + top * dwLinearSize
     * there, outside the surface. */
    if (!f->block && (w > 1 || h > 1)) {
        LONG x0 = w > 2 ? 2 : (LONG)w - 1, y0 = h > 1 ? 1 : 0;
        if (f->fourcc)
            x0 &= ~1; /* YUY2/UYVY: whole pixel pairs */
        RECT r = {x0, y0, (LONG)w, (LONG)h};
        DDSURFACEDESC2 rd;
        desc_init(&rd);
        hr = a->Lock(&r, &rd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
        snprintf(sub, sizeof(sub), "%s Lock(%ld,%ld,%ld,%ld)", what, r.left, r.top, r.right, r.bottom);
        if (check_hr(sub, hr)) {
            if (rd.lPitch != sd.lPitch)
                fail("%s lPitch %ld, expected %ld", sub, (long)rd.lPitch, (long)sd.lPitch);
            const BYTE *p = (const BYTE *)rd.lpSurface;
            DWORD first = (DWORD)x0 * (DWORD)f->bytes;
            for (DWORD i = first; i < row_bytes; ++i)
                if (p[i - first] != pattern_byte(seed, (DWORD)y0, i)) {
                    fail("%s: byte %lu of the rect's first row = 0x%02x, expected 0x%02x (pattern row %ld byte %lu)",
                         sub, (unsigned long)(i - first), p[i - first], pattern_byte(seed, (DWORD)y0, i), (long)y0,
                         (unsigned long)i);
                    break;
                }
            BYTE *want = base + y0 * sd.lPitch + x0 * f->bytes;
            if ((caps & DDSCAPS_SYSTEMMEMORY) && (BYTE *)rd.lpSurface != want)
                fail("%s returned base%+ld, expected base%+ld", sub, (long)((BYTE *)rd.lpSurface - base),
                     (long)(want - base));
            a->Unlock(&r);
        }
    }
}

static void scene_lock_formats(void)
{
    static const Fmt *const rgb[] = {&F_P8,       &F_R5G6B5, &F_X1R5G5B5, &F_A1R5G5B5,
                                     &F_A4R4G4B4, &F_R8G8B8, &F_X8R8G8B8, &F_A8R8G8B8};
    static const DWORD widths[] = {1, 3, 63, 64};
    static const DWORD heights[] = {1, 7};
    unsigned i, wi, hi;

    /* System-memory RGB and palettized surfaces.  Pitch rule (stock, and
     * Wine's test_create_surface_pitch for 32-bit ddraw): width * bytes per
     * pixel rounded up to a multiple of 8. */
    for (i = 0; i < sizeof(rgb) / sizeof(rgb[0]); ++i)
        for (hi = 0; hi < 2; ++hi)
            for (wi = 0; wi < 4; ++wi) {
                DWORD w = widths[wi], h = heights[hi];
                lock_case(rgb[i], SYS_PLAIN, w, h, (w * (DWORD)rgb[i]->bytes + 7) & ~7u, 1);
            }

    /* DXTn system-memory textures: sizes that are not multiples of 4 are
     * refused (stock, and Wine's test_block_formats_creation on every
     * Windows); dwLinearSize = 4x4 blocks * block size. */
    static const Fmt *const dxt[] = {&F_DXT1, &F_DXT3, &F_DXT5};
    static const DWORD dxt_sizes[][2] = {{4, 4}, {8, 4}, {64, 64}, {12, 8}, {64, 4}};
    for (i = 0; i < 3; ++i) {
        for (hi = 0; hi < 2; ++hi)
            for (wi = 0; wi < 4; ++wi) {
                IDirectDrawSurface7 *s = NULL;
                char what[96];
                HRESULT hr = create_on(g_dd, SYS_TEXTURE, dxt[i], widths[wi], heights[hi], &s);
                snprintf(what, sizeof(what), "CreateSurface(%s %lux%lu system-memory texture)", dxt[i]->name,
                         (unsigned long)widths[wi], (unsigned long)heights[hi]);
                expect_hr(what, hr, DDERR_INVALIDPARAMS);
                if (SUCCEEDED(hr))
                    s->Release();
            }
        for (unsigned k = 0; k < sizeof(dxt_sizes) / sizeof(dxt_sizes[0]); ++k) {
            DWORD w = dxt_sizes[k][0], h = dxt_sizes[k][1];
            lock_case(dxt[i], SYS_TEXTURE, w, h, (w / 4) * (h / 4) * (DWORD)dxt[i]->block, 1);
        }
    }

    /* YUY2 / UYVY.  System-memory offscreen-plain FOURCC surfaces are
     * refused (stock; Wine's test_block_formats_creation notes the same);
     * without a memory flag stock creates them (in video memory there:
     * pitch and Blt results are the driver's, so only the minimum pitch and
     * a successful Blt are asserted).  System-memory textures of them work
     * everywhere, with the RGB pitch rule. */
    static const Fmt *const yuv[] = {&F_YUY2, &F_UYVY};
    for (i = 0; i < 2; ++i)
        for (hi = 0; hi < 2; ++hi)
            for (wi = 0; wi < 4; ++wi) {
                DWORD w = widths[wi], h = heights[hi];
                IDirectDrawSurface7 *s = NULL;
                char what[96];
                HRESULT hr = create_on(g_dd, SYS_PLAIN, yuv[i], w, h, &s);
                snprintf(what, sizeof(what), "CreateSurface(%s %lux%lu system-memory offscreen plain)", yuv[i]->name,
                         (unsigned long)w, (unsigned long)h);
                expect_hr(what, hr, DDERR_INVALIDPIXELFORMAT);
                if (SUCCEEDED(hr))
                    s->Release();
                lock_case(yuv[i], DDSCAPS_OFFSCREENPLAIN, w, h, 0, 0);
                lock_case(yuv[i], SYS_TEXTURE, w, h, (w * 2 + 7) & ~7u, 1);
            }
}

/* ------------------------------------------------------------------------ */
/* palette                                                                   */

static void make_entries(PALETTEENTRY *pe, int salt)
{
    /* Unique colors (red = index) so GDI's nearest-color match is exact. */
    for (int i = 0; i < 256; ++i) {
        pe[i].peRed = (BYTE)i;
        pe[i].peGreen = (BYTE)(i * 3 + 17 + salt);
        pe[i].peBlue = (BYTE)(i * 5 + 101 + salt * 7);
        pe[i].peFlags = 0;
    }
}

static COLORREF entry_rgb(const PALETTEENTRY *e)
{
    return RGB(e->peRed, e->peGreen, e->peBlue);
}

/* GetPixel of every pixel of a 16x16 P8 surface matches the palette. */
static int expect_dc_colors(IDirectDrawSurface7 *s, const BYTE *idx, const PALETTEENTRY *pe, const char *what)
{
    HDC dc;
    HRESULT hr = s->GetDC(&dc);
    if (FAILED(hr))
        return fail("%s: GetDC: %s", what, hrname(hr));
    int ok = 1;
    for (int y = 0; y < 16 && ok; ++y)
        for (int x = 0; x < 16; ++x) {
            COLORREF got = GetPixel(dc, x, y), want = entry_rgb(&pe[idx[y * 16 + x]]);
            if (got != want) {
                ok = fail("%s: GetPixel(%d,%d) = 0x%06lx, expected 0x%06lx (index %u)", what, x, y,
                          (unsigned long)got, (unsigned long)want, idx[y * 16 + x]);
                break;
            }
        }
    hr = s->ReleaseDC(dc);
    return check_hr("ReleaseDC", hr) && ok;
}

static void scene_palette(void)
{
    PALETTEENTRY pe[256], got[256];
    BYTE idx[256];
    Com<IDirectDrawPalette> pal;
    make_entries(pe, 0);
    if (!check_hr("CreatePalette", g_dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, pal.out(), NULL)))
        return;
    Com<IDirectDrawSurface7> s, d;
    s.p = make(SYS_PLAIN, &F_P8, 16, 16);
    d.p = make(SYS_PLAIN, &F_P8, 16, 16);
    if (!s || !d || !check_hr("SetPalette", s->SetPalette(pal)))
        return;

    /* Every index once, scrambled. */
    Img im(16, 16);
    for (int i = 0; i < 256; ++i) {
        idx[i] = (BYTE)(i * 167 + 13);
        im.px[i] = idx[i];
    }
    if (!put_image(s, 1, &im, "P8 write through Lock"))
        return;
    expect_dc_colors(s, idx, pe, "GetDC on P8 with a palette");

    /* SetEntries: the next GetDC shows the new colors. */
    PALETTEENTRY changed[40];
    for (int i = 0; i < 40; ++i) {
        changed[i].peRed = (BYTE)(255 - i);
        changed[i].peGreen = (BYTE)(i * 6);
        changed[i].peBlue = (BYTE)(200 - i * 3);
        changed[i].peFlags = 0;
        pe[100 + i] = changed[i];
    }
    if (check_hr("SetEntries(100, 40)", pal->SetEntries(0, 100, 40, changed)))
        expect_dc_colors(s, idx, pe, "GetDC after SetEntries");

    memset(got, 0xcc, sizeof(got));
    if (check_hr("GetEntries(0, 256)", pal->GetEntries(0, 0, 256, got)))
        for (int i = 0; i < 256; ++i)
            if (memcmp(&got[i], &pe[i], sizeof(pe[i]))) {
                fail("GetEntries: entry %d = {%u,%u,%u,%u}, expected {%u,%u,%u,%u}", i, got[i].peRed,
                     got[i].peGreen, got[i].peBlue, got[i].peFlags, pe[i].peRed, pe[i].peGreen, pe[i].peBlue,
                     pe[i].peFlags);
                break;
            }
    memset(got, 0xcc, sizeof(got));
    if (check_hr("GetEntries(120, 5)", pal->GetEntries(0, 120, 5, got)) && memcmp(got, &pe[120], 5 * sizeof(pe[0])))
        fail("GetEntries(120, 5) does not return entries 120-124");

    /* P8 -> P8 Blt copies indices (the destination has no palette). */
    if (check_hr("Blt P8 -> P8", d->Blt(NULL, s, NULL, DDBLT_WAIT, NULL)))
        expect_image(d, 1, &im, 0xff, "P8 -> P8 Blt");
    Img im2(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            im2.at(x, y) = (x >= 3 && x < 11 && y >= 5 && y < 9) ? im.at(x - 3 + 4, y - 5 + 2) : im.at(x, y);
    RECT sr = {4, 2, 12, 6};
    if (check_hr("BltFast P8 -> P8 sub-rect", d->BltFast(3, 5, s, &sr, DDBLTFAST_WAIT | DDBLTFAST_NOCOLORKEY)))
        expect_image(d, 1, &im2, 0xff, "P8 -> P8 BltFast");
}

/* ------------------------------------------------------------------------ */
/* colorkey                                                                  */

/* Source pixels: K1 where (x + y) % 3 == 0, else K2 where (x + 2y) % 5 == 1,
 * else a hash value that is neither.  X8R8G8B8 values keep the X byte 0:
 * stock keys on RGB only and copies X, but how other runtimes treat X in
 * keyed blits differs (Wine's test_ck_operation), so it is not tested. */
static void key_source(Img *im, DWORD mask, DWORD k1, DWORD k2)
{
    for (int y = 0; y < im->h; ++y)
        for (int x = 0; x < im->w; ++x) {
            DWORD v;
            if ((x + y) % 3 == 0)
                v = k1;
            else if ((x + 2 * y) % 5 == 1)
                v = k2;
            else {
                v = hash3(77, (DWORD)x, (DWORD)y) & mask;
                if (v == k1 || v == k2)
                    v ^= 0x0101;
            }
            im->at(x, y) = v;
        }
}

/* want = dst with src pixels copied where src != key (src rect at sx,sy
 * of size w x h onto dx,dy). */
static void keyed_expect(Img *want, const Img *dst, const Img *src, int sx, int sy, int dx, int dy, int w, int h,
                         DWORD key)
{
    memcpy(want->px, dst->px, sizeof(DWORD) * (size_t)(dst->w * dst->h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            DWORD v = src->px[(sy + y) * src->w + sx + x];
            if (v != key)
                want->at(dx + x, dy + y) = v;
        }
}

static void colorkey_format(const Fmt *f, DWORD k1, DWORD k2)
{
    const int W = 16, H = 8, bytes = f->bytes;
    const DWORD mask = f->bits == 32 ? 0x00ffffffu : bytes_mask(bytes);
    char what[160];
    Com<IDirectDrawSurface7> src, dst;
    src.p = make(SYS_PLAIN, f, W, H);
    dst.p = make(SYS_PLAIN, f, W, H);
    if (!src || !dst)
        return;
    Img sim(W, H), dim(W, H), want(W, H);
    key_source(&sim, mask, k1, k2);
    fill_hash(&dim, 1234, mask);
    if (!put_image(src, bytes, &sim, "key source"))
        return;

    DDCOLORKEY ck = {k1, k1};
    if (!check_hr("SetColorKey(DDCKEY_SRCBLT)", src->SetColorKey(DDCKEY_SRCBLT, &ck)))
        return;
    DDCOLORKEY gk = {0, 0};
    if (check_hr("GetColorKey(DDCKEY_SRCBLT)", src->GetColorKey(DDCKEY_SRCBLT, &gk)) &&
        (gk.dwColorSpaceLowValue != k1 || gk.dwColorSpaceHighValue != k1))
        fail("%s GetColorKey: %08lx-%08lx, expected %08lx", f->name, (unsigned long)gk.dwColorSpaceLowValue,
             (unsigned long)gk.dwColorSpaceHighValue, (unsigned long)k1);

    /* Blt with DDBLT_KEYSRC. */
    if (put_image(dst, bytes, &dim, "key destination") &&
        check_hr("Blt(DDBLT_KEYSRC)", dst->Blt(NULL, src, NULL, DDBLT_WAIT | DDBLT_KEYSRC, NULL))) {
        keyed_expect(&want, &dim, &sim, 0, 0, 0, 0, W, H, k1);
        snprintf(what, sizeof(what), "%s Blt(DDBLT_KEYSRC)", f->name);
        expect_image(dst, bytes, &want, bytes_mask(bytes), what);
    }
    /* BltFast with DDBLTFAST_SRCCOLORKEY, sub-rect at an offset. */
    RECT sr = {2, 1, 12, 7};
    if (put_image(dst, bytes, &dim, "key destination") &&
        check_hr("BltFast(DDBLTFAST_SRCCOLORKEY)",
                 dst->BltFast(5, 2, src, &sr, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY))) {
        keyed_expect(&want, &dim, &sim, 2, 1, 5, 2, 10, 6, k1);
        snprintf(what, sizeof(what), "%s BltFast(DDBLTFAST_SRCCOLORKEY) (2,1)-(12,7) to (5,2)", f->name);
        expect_image(dst, bytes, &want, bytes_mask(bytes), what);
    }
    /* BltFast without the flag ignores the key. */
    if (put_image(dst, bytes, &dim, "key destination") &&
        check_hr("BltFast(DDBLTFAST_NOCOLORKEY)", dst->BltFast(0, 0, src, NULL, DDBLTFAST_WAIT | DDBLTFAST_NOCOLORKEY))) {
        snprintf(what, sizeof(what), "%s BltFast(DDBLTFAST_NOCOLORKEY)", f->name);
        expect_image(dst, bytes, &sim, bytes_mask(bytes), what);
    }
    /* DDBLT_KEYSRCOVERRIDE: the DDBLTFX key, not the surface's. */
    DDBLTFX fx;
    init_fx(&fx);
    fx.ddckSrcColorkey.dwColorSpaceLowValue = k2;
    fx.ddckSrcColorkey.dwColorSpaceHighValue = k2;
    RECT dr = {3, 1, 13, 7};
    if (put_image(dst, bytes, &dim, "key destination") &&
        check_hr("Blt(DDBLT_KEYSRCOVERRIDE)", dst->Blt(&dr, src, &sr, DDBLT_WAIT | DDBLT_KEYSRCOVERRIDE, &fx))) {
        keyed_expect(&want, &dim, &sim, 2, 1, 3, 1, 10, 6, k2);
        snprintf(what, sizeof(what), "%s Blt(DDBLT_KEYSRCOVERRIDE)", f->name);
        expect_image(dst, bytes, &want, bytes_mask(bytes), what);
    }
    /* Both at once is refused and changes nothing. */
    if (put_image(dst, bytes, &dim, "key destination")) {
        HRESULT hr = dst->Blt(NULL, src, NULL, DDBLT_WAIT | DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE, &fx);
        expect_hr("Blt(DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)", hr, DDERR_INVALIDPARAMS);
        snprintf(what, sizeof(what), "%s after the refused Blt", f->name);
        expect_image(dst, bytes, &dim, bytes_mask(bytes), what);
    }
}

static int have_ckey_caps(DWORD bit)
{
    return (g_hel.dwCKeyCaps & bit) || (g_hal.dwCKeyCaps & bit);
}

static void scene_colorkey(void)
{
    if (!have_ckey_caps(DDCKEYCAPS_SRCBLT)) {
        skip("DDCKEYCAPS_SRCBLT in neither HAL (dwCKeyCaps 0x%08lx) nor HEL (0x%08lx) caps",
             (unsigned long)g_hal.dwCKeyCaps, (unsigned long)g_hel.dwCKeyCaps);
        return;
    }
    colorkey_format(&F_R5G6B5, 0xf81f, 0x07e0);
    colorkey_format(&F_X8R8G8B8, 0x00ff00ff, 0x0000ff00);
}

/* DDBLT_KEYDEST: source pixels land only where the destination holds the
 * destination key `key`. */
static void colorkey_dest_format(const Fmt *f, DWORD key)
{
    const int W = 16, H = 8, bytes = f->bytes;
    const DWORD mask = f->bits == 32 ? 0x00ffffffu : bytes_mask(bytes);
    char what[160];
    Com<IDirectDrawSurface7> src, dst;
    src.p = make(SYS_PLAIN, f, W, H);
    dst.p = make(SYS_PLAIN, f, W, H);
    if (!src || !dst)
        return;
    DDCOLORKEY dk = {key, key};
    snprintf(what, sizeof(what), "%s SetColorKey(DDCKEY_DESTBLT)", f->name);
    if (!check_hr(what, dst->SetColorKey(DDCKEY_DESTBLT, &dk)))
        return;
    /* Destination: the key where (x + 2y) % 5 == 1, else non-key values;
     * source: non-key values throughout. */
    Img dim(W, H), sim(W, H), want(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            DWORD d = hash3(5, (DWORD)x, (DWORD)y) & mask, s = hash3(6, (DWORD)x, (DWORD)y) & mask;
            if (d == key)
                d ^= 0x0101;
            if ((x + 2 * y) % 5 == 1)
                d = key;
            if (s == key)
                s ^= 0x0101;
            dim.at(x, y) = d;
            sim.at(x, y) = s;
            want.at(x, y) = d == key ? s : d;
        }
    if (!put_image(src, bytes, &sim, "dest-key source") || !put_image(dst, bytes, &dim, "dest-key destination"))
        return;
    snprintf(what, sizeof(what), "%s Blt(DDBLT_KEYDEST)", f->name);
    if (check_hr(what, dst->Blt(NULL, src, NULL, DDBLT_WAIT | DDBLT_KEYDEST, NULL)))
        expect_image(dst, bytes, &want, bytes_mask(bytes), what);
}

/* Destination color keys: only where the caps advertise them. */
static void scene_colorkey_dest(void)
{
    if (!have_ckey_caps(DDCKEYCAPS_DESTBLT)) {
        skip("DDCKEYCAPS_DESTBLT in neither HAL (dwCKeyCaps 0x%08lx) nor HEL (0x%08lx) caps",
             (unsigned long)g_hal.dwCKeyCaps, (unsigned long)g_hel.dwCKeyCaps);
        return;
    }
    colorkey_dest_format(&F_R5G6B5, 0x07e0);
    colorkey_dest_format(&F_X8R8G8B8, 0x0000ff00);
}

/* ------------------------------------------------------------------------ */
/* stretch / mirror / overlap                                                */

/* dst rect (dx, dy, dw, dh) of `want` from src rect (sx, sy, sw, sh) by
 * integer nearest-point scaling (dw/sw and dh/sh integers, or sw/dw and
 * sh/dh integers for minification of constant blocks). */
static void scale_expect(Img *want, const Img *src, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh)
{
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
            want->at(dx + x, dy + y) = src->px[(sy + y * sh / dh) * src->w + sx + x * sw / dw];
}

static void stretch_format(const Fmt *f)
{
    const int bytes = f->bytes;
    const DWORD mask = bytes_mask(bytes);
    char what[160];
    static const struct {
        int sw, sh, dw, dh;
    } mags[] = {{8, 6, 16, 12}, {8, 6, 24, 18}, {8, 6, 16, 18}, {5, 3, 15, 6}};
    for (unsigned i = 0; i < sizeof(mags) / sizeof(mags[0]); ++i) {
        Com<IDirectDrawSurface7> src, dst;
        src.p = make(SYS_PLAIN, f, mags[i].sw, mags[i].sh);
        dst.p = make(SYS_PLAIN, f, mags[i].dw, mags[i].dh);
        if (!src || !dst)
            return;
        Img sim(mags[i].sw, mags[i].sh), want(mags[i].dw, mags[i].dh);
        fill_hash(&sim, 300 + i, mask);
        if (!put_image(src, bytes, &sim, "stretch source"))
            return;
        snprintf(what, sizeof(what), "%s stretch %dx%d -> %dx%d", f->name, mags[i].sw, mags[i].sh, mags[i].dw,
                 mags[i].dh);
        if (!check_hr(what, dst->Blt(NULL, src, NULL, DDBLT_WAIT, NULL)))
            continue;
        scale_expect(&want, &sim, 0, 0, mags[i].sw, mags[i].sh, 0, 0, mags[i].dw, mags[i].dh);
        expect_image(dst, bytes, &want, mask, what);
    }

    /* 2x minification of 2x2 constant blocks. */
    {
        Com<IDirectDrawSurface7> src, dst;
        src.p = make(SYS_PLAIN, f, 16, 12);
        dst.p = make(SYS_PLAIN, f, 8, 6);
        if (!src || !dst)
            return;
        Img sim(16, 12), want(8, 6);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x)
                sim.at(x, y) = hash3(400, (DWORD)(x / 2), (DWORD)(y / 2)) & mask;
        if (!put_image(src, bytes, &sim, "minify source"))
            return;
        snprintf(what, sizeof(what), "%s 2x minification 16x12 -> 8x6", f->name);
        if (check_hr(what, dst->Blt(NULL, src, NULL, DDBLT_WAIT, NULL))) {
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 8; ++x)
                    want.at(x, y) = sim.at(2 * x, 2 * y);
            expect_image(dst, bytes, &want, mask, what);
        }
    }

    /* Rect to rect inside larger surfaces: (2,1)-(6,5) doubled onto
     * (5,7)-(13,15); the rest of the destination untouched. */
    {
        Com<IDirectDrawSurface7> src, dst;
        src.p = make(SYS_PLAIN, f, 9, 7);
        dst.p = make(SYS_PLAIN, f, 20, 18);
        if (!src || !dst)
            return;
        Img sim(9, 7), dim(20, 18);
        fill_hash(&sim, 500, mask);
        fill_hash(&dim, 501, mask);
        if (!put_image(src, bytes, &sim, "stretch source") || !put_image(dst, bytes, &dim, "stretch destination"))
            return;
        RECT sr = {2, 1, 6, 5}, dr = {5, 7, 13, 15};
        snprintf(what, sizeof(what), "%s stretch (2,1)-(6,5) -> (5,7)-(13,15)", f->name);
        if (check_hr(what, dst->Blt(&dr, src, &sr, DDBLT_WAIT, NULL))) {
            scale_expect(&dim, &sim, 2, 1, 4, 4, 5, 7, 8, 8);
            expect_image(dst, bytes, &dim, mask, what);
        }
    }
}

static void scene_stretch(void)
{
    if (!(g_hel.dwCaps & DDCAPS_BLTSTRETCH) && !(g_hal.dwCaps & DDCAPS_BLTSTRETCH)) {
        skip("DDCAPS_BLTSTRETCH in neither HAL nor HEL caps");
        return;
    }
    stretch_format(&F_R5G6B5);
    stretch_format(&F_X8R8G8B8);
}

static void mirror_format(const Fmt *f)
{
    const int W = 7, H = 5, bytes = f->bytes;
    const DWORD mask = bytes_mask(bytes);
    static const struct {
        DWORD fx;
        const char *name;
    } modes[] = {
        {DDBLTFX_MIRRORLEFTRIGHT, "DDBLTFX_MIRRORLEFTRIGHT"},
        {DDBLTFX_MIRRORUPDOWN, "DDBLTFX_MIRRORUPDOWN"},
        {DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN, "DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN"},
    };
    Com<IDirectDrawSurface7> src, dst;
    src.p = make(SYS_PLAIN, f, W, H);
    dst.p = make(SYS_PLAIN, f, W, H);
    if (!src || !dst)
        return;
    Img sim(W, H), want(W, H);
    fill_hash(&sim, 600, mask);
    if (!put_image(src, bytes, &sim, "mirror source"))
        return;
    for (unsigned i = 0; i < 3; ++i) {
        char what[160];
        DDBLTFX fx;
        init_fx(&fx);
        fx.dwDDFX = modes[i].fx;
        snprintf(what, sizeof(what), "%s Blt(DDBLT_DDFX, %s)", f->name, modes[i].name);
        if (!check_hr(what, dst->Blt(NULL, src, NULL, DDBLT_WAIT | DDBLT_DDFX, &fx)))
            continue;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int sx = (modes[i].fx & DDBLTFX_MIRRORLEFTRIGHT) ? W - 1 - x : x;
                int sy = (modes[i].fx & DDBLTFX_MIRRORUPDOWN) ? H - 1 - y : y;
                want.at(x, y) = sim.at(sx, sy);
            }
        expect_image(dst, bytes, &want, mask, what);
    }
}

static void scene_mirror(void)
{
    const DWORD need = DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN;
    if ((g_hel.dwFXCaps & need) != need && (g_hal.dwFXCaps & need) != need) {
        skip("DDFXCAPS_BLTMIRRORLEFTRIGHT/UPDOWN missing (HAL dwFXCaps 0x%08lx, HEL 0x%08lx)",
             (unsigned long)g_hal.dwFXCaps, (unsigned long)g_hel.dwFXCaps);
        return;
    }
    mirror_format(&F_R5G6B5);
    mirror_format(&F_X8R8G8B8);
}

static void overlap_format(const Fmt *f)
{
    const int W = 40, H = 12, bytes = f->bytes;
    const DWORD mask = bytes_mask(bytes);
    /* Source rect and destination origin; every direction, including 1-pixel
     * shifts that a forward copy within a row would smear. */
    static const struct {
        RECT src;
        int dx, dy;
    } cases[] = {
        {{0, 0, 32, 12}, 8, 0}, {{8, 0, 40, 12}, 0, 0}, {{0, 0, 39, 12}, 1, 0}, {{1, 0, 40, 12}, 0, 0},
        {{0, 0, 40, 9}, 0, 3},  {{0, 3, 40, 12}, 0, 0}, {{0, 0, 40, 11}, 0, 1}, {{0, 1, 40, 12}, 0, 0},
        {{0, 0, 30, 8}, 5, 3},  {{5, 3, 35, 11}, 0, 0}, {{6, 0, 36, 9}, 2, 3},  {{0, 3, 30, 12}, 6, 0},
    };
    Com<IDirectDrawSurface7> s;
    s.p = make(SYS_PLAIN, f, W, H);
    if (!s)
        return;
    Img orig(W, H), want(W, H);
    fill_hash(&orig, 700, mask);
    for (int fast = 0; fast < 2; ++fast)
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            char what[160];
            const RECT *sr = &cases[i].src;
            int w = sr->right - sr->left, h = sr->bottom - sr->top;
            if (!put_image(s, bytes, &orig, "overlap pattern"))
                return;
            RECT src = *sr, dr = {cases[i].dx, cases[i].dy, cases[i].dx + w, cases[i].dy + h};
            HRESULT hr = fast ? s->BltFast(cases[i].dx, cases[i].dy, s, &src, DDBLTFAST_WAIT | DDBLTFAST_NOCOLORKEY)
                              : s->Blt(&dr, s, &src, DDBLT_WAIT, NULL);
            snprintf(what, sizeof(what), "%s %s (%ld,%ld)-(%ld,%ld) to (%d,%d) within one surface", f->name,
                     fast ? "BltFast" : "Blt", sr->left, sr->top, sr->right, sr->bottom, cases[i].dx, cases[i].dy);
            if (!check_hr(what, hr))
                continue;
            memcpy(want.px, orig.px, sizeof(DWORD) * W * H);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    want.at(cases[i].dx + x, cases[i].dy + y) = orig.at(sr->left + x, sr->top + y);
            expect_image(s, bytes, &want, mask, what);
        }
}

static void scene_blt_overlap(void)
{
    overlap_format(&F_R5G6B5);
    overlap_format(&F_X8R8G8B8);
}

/* ------------------------------------------------------------------------ */
/* fill                                                                      */

static void fill_format(const Fmt *f, DWORD color)
{
    const int W = 17, H = 11, bytes = f->bytes;
    const DWORD mask = bytes_mask(bytes);
    char what[160];
    Com<IDirectDrawSurface7> s;
    s.p = make(SYS_PLAIN, f, W, H);
    if (!s)
        return;
    Img base(W, H), want(W, H);
    fill_hash(&base, 800, mask);
    DDBLTFX fx;
    init_fx(&fx);
    fx.dwFillColor = color;

    /* Whole surface: every pixel is dwFillColor's low bytes. */
    if (!put_image(s, bytes, &base, "fill pattern"))
        return;
    snprintf(what, sizeof(what), "%s DDBLT_COLORFILL 0x%08lx whole surface", f->name, (unsigned long)color);
    if (check_hr(what, s->Blt(NULL, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &fx))) {
        for (int i = 0; i < W * H; ++i)
            want.px[i] = color & mask;
        expect_image(s, bytes, &want, mask, what);
    }

    /* Sub-rects: inside filled, outside untouched. */
    static const RECT rects[] = {{3, 2, 11, 9}, {0, 0, 1, 1}, {16, 10, 17, 11}, {0, 5, 17, 6}, {7, 0, 8, 11}};
    for (unsigned i = 0; i < sizeof(rects) / sizeof(rects[0]); ++i) {
        RECT r = rects[i];
        if (!put_image(s, bytes, &base, "fill pattern"))
            return;
        snprintf(what, sizeof(what), "%s DDBLT_COLORFILL (%ld,%ld)-(%ld,%ld)", f->name, r.left, r.top, r.right,
                 r.bottom);
        if (!check_hr(what, s->Blt(&r, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &fx)))
            continue;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                want.at(x, y) = (x >= r.left && x < r.right && y >= r.top && y < r.bottom) ? color & mask : base.at(x, y);
        expect_image(s, bytes, &want, mask, what);
    }

    /* Rects not inside the surface: DDERR_INVALIDRECT, nothing changes. */
    static const RECT bad[] = {
        {0, 0, W + 1, H}, {0, 0, W, H + 1}, {-1, 0, 4, 4}, {0, -1, 4, 4}, {9, 3, 5, 7},
        {5, 7, 9, 3},     {W, 0, W + 4, 4}, {3, 3, 3, 8},  {3, 3, 8, 3},
    };
    if (!put_image(s, bytes, &base, "fill pattern"))
        return;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        RECT r = bad[i];
        snprintf(what, sizeof(what), "%s DDBLT_COLORFILL (%ld,%ld)-(%ld,%ld)", f->name, r.left, r.top, r.right,
                 r.bottom);
        expect_hr(what, s->Blt(&r, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &fx), DDERR_INVALIDRECT);
    }
    snprintf(what, sizeof(what), "%s after the refused fills", f->name);
    expect_image(s, bytes, &base, mask, what);
}

static void scene_fill(void)
{
    fill_format(&F_P8, 0xa5);
    fill_format(&F_R5G6B5, 0xbeef);
    fill_format(&F_R8G8B8, 0x00abcdef);
    /* 32 bpp: the whole dwFillColor is written, X byte included (stock). */
    fill_format(&F_X8R8G8B8, 0x12345678);
    fill_format(&F_A8R8G8B8, 0xdeadbeef);
}

/* ------------------------------------------------------------------------ */
/* getdc                                                                     */

static void getdc_format(const Fmt *f, COLORREF fill, DWORD fill_px, COLORREF dot, DWORD dot_px,
                         IDirectDrawPalette *pal)
{
    const int W = 16, H = 16, bytes = f->bytes;
    /* GDI writes 0 into the X byte of 32-bit pixels it draws; only RGB is
     * compared there.  Pixels GDI does not draw must be untouched. */
    const DWORD drawn_mask = f->bits == 32 ? 0x00ffffffu : bytes_mask(bytes);
    char what[160];
    Com<IDirectDrawSurface7> s;
    s.p = make(SYS_PLAIN, f, W, H);
    if (!s)
        return;
    if (pal && !check_hr("SetPalette", s->SetPalette(pal)))
        return;
    Img base(W, H);
    fill_hash(&base, 900, bytes_mask(bytes));
    if (!put_image(s, bytes, &base, "GetDC pattern"))
        return;

    HDC dc;
    snprintf(what, sizeof(what), "%s GetDC", f->name);
    if (!check_hr(what, s->GetDC(&dc)))
        return;
    RECT r = {2, 3, 10, 9};
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(dc, &r, br);
    DeleteObject(br);
    SetPixel(dc, 12, 13, dot);
    /* The DC reads back what was drawn. */
    COLORREF px = GetPixel(dc, 4, 4);
    if (px != fill)
        fail("%s GetPixel inside the FillRect = 0x%06lx, expected 0x%06lx", f->name, (unsigned long)px,
             (unsigned long)fill);
    snprintf(what, sizeof(what), "%s ReleaseDC", f->name);
    if (!check_hr(what, s->ReleaseDC(dc)))
        return;

    Img got(W, H);
    if (!get_image(s, bytes, &got, "Lock after ReleaseDC"))
        return;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int in_rect = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
            int is_dot = x == 12 && y == 13;
            DWORD want = in_rect ? fill_px : is_dot ? dot_px : base.at(x, y);
            DWORD m = (in_rect || is_dot) ? drawn_mask : bytes_mask(bytes);
            if ((got.at(x, y) ^ want) & m) {
                fail("%s after FillRect/SetPixel: pixel (%d,%d) = 0x%08lx, expected 0x%08lx%s", f->name, x, y,
                     (unsigned long)(got.at(x, y) & m), (unsigned long)(want & m),
                     in_rect ? " (FillRect)" : is_dot ? " (SetPixel)" : " (untouched)");
                return;
            }
        }

    /* GetDC while locked succeeds (stock, every interface version per
     * Wine's test_getdc); Lock while a DC is out is DDERR_SURFACEBUSY. */
    DDSURFACEDESC2 d;
    desc_init(&d);
    if (check_hr("Lock", s->Lock(NULL, &d, DDLOCK_WAIT, NULL))) {
        HRESULT hr = s->GetDC(&dc);
        snprintf(what, sizeof(what), "%s GetDC while locked", f->name);
        if (expect_hr(what, hr, DD_OK))
            check_hr("ReleaseDC of the DC taken while locked", s->ReleaseDC(dc));
        check_hr("Unlock after GetDC/ReleaseDC", s->Unlock(NULL));
    }
    if (check_hr("GetDC", s->GetDC(&dc))) {
        desc_init(&d);
        HRESULT hr = s->Lock(NULL, &d, DDLOCK_WAIT, NULL);
        snprintf(what, sizeof(what), "%s Lock while a DC is out", f->name);
        expect_hr(what, hr, DDERR_SURFACEBUSY);
        if (SUCCEEDED(hr))
            s->Unlock(NULL);
        HDC dc2;
        snprintf(what, sizeof(what), "%s second GetDC", f->name);
        hr = s->GetDC(&dc2);
        expect_hr(what, hr, DDERR_DCALREADYCREATED);
        check_hr("ReleaseDC", s->ReleaseDC(dc));
        snprintf(what, sizeof(what), "%s second ReleaseDC", f->name);
        expect_hr(what, s->ReleaseDC(dc), DDERR_NODC);
    }
    /* The surface is usable again. */
    desc_init(&d);
    if (check_hr("Lock after ReleaseDC", s->Lock(NULL, &d, DDLOCK_WAIT, NULL)))
        s->Unlock(NULL);
}

static void scene_getdc(void)
{
    /* R5G6B5: channel values that 5/6-bit storage and GDI's bit-replicating
     * expansion both keep exact. */
    getdc_format(&F_R5G6B5, RGB(0x10, 0x20, 0x18), (0x10 >> 3) << 11 | (0x20 >> 2) << 5 | (0x18 >> 3),
                 RGB(0xf8, 0x00, 0x08), 0xf801, NULL);
    getdc_format(&F_X8R8G8B8, RGB(0x12, 0x34, 0x56), 0x123456, RGB(0xab, 0xcd, 0xef), 0xabcdef, NULL);
    PALETTEENTRY pe[256];
    make_entries(pe, 3);
    Com<IDirectDrawPalette> pal;
    if (check_hr("CreatePalette", g_dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, pal.out(), NULL)))
        getdc_format(&F_P8, entry_rgb(&pe[77]), 77, entry_rgb(&pe[200]), 200, pal);
}

/* ------------------------------------------------------------------------ */
/* windowed_present                                                          */

static DWORD present_value(DWORD seed, int x, int y)
{
    return hash3(seed, (DWORD)x, (DWORD)y) & 0x00ffffffu;
}

/* Reads w x h pixels at screen (sx, sy) from the primary into `out`
 * through a system-memory surface. */
static int read_primary(IDirectDrawSurface7 *primary, IDirectDrawSurface7 *rb, const Fmt *f, LONG sx, LONG sy, int w,
                        int h, Img *out)
{
    RECT sr = {sx, sy, sx + w, sy + h};
    HRESULT hr = rb->Blt(NULL, primary, &sr, DDBLT_WAIT, NULL);
    if (FAILED(hr))
        return fail("Blt from the primary: %s", hrname(hr));
    return get_image(rb, f->bytes, out, "read back from the primary");
}

/* Compares, retrying for up to ~1 s while the desktop catches up. */
static int expect_primary(IDirectDrawSurface7 *primary, IDirectDrawSurface7 *rb, const Fmt *f, LONG sx, LONG sy,
                          const Img *want, DWORD mask, const char *what)
{
    Img got(want->w, want->h);
    for (int t = 0;; ++t) {
        pump();
        if (!read_primary(primary, rb, f, sx, sy, want->w, want->h, &got))
            return 0;
        int same = 1;
        for (int i = 0; i < want->w * want->h && same; ++i)
            same = !((got.px[i] ^ want->px[i]) & mask);
        if (same)
            return 1;
        if (t == 20)
            return compare(what, &got, want, mask);
        Sleep(50);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void scene_windowed_present(void)
{
    char what[200];
    DDSURFACEDESC2 d;
    desc_init(&d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    Com<IDirectDrawSurface7> primary;
    if (!check_hr("CreateSurface(primary)", g_dd->CreateSurface(&d, primary.out(), NULL)))
        return;
    desc_init(&d);
    if (!check_hr("primary GetSurfaceDesc", primary->GetSurfaceDesc(&d)))
        return;
    /* Back buffers in the primary's format, so the copies are 1:1. */
    Fmt pf = {"primary format", d.ddpfPixelFormat.dwFlags, 0, d.ddpfPixelFormat.dwRGBBitCount,
              d.ddpfPixelFormat.dwRBitMask, d.ddpfPixelFormat.dwGBitMask, d.ddpfPixelFormat.dwBBitMask, 0,
              (int)d.ddpfPixelFormat.dwRGBBitCount / 8, 0};
    if (!(pf.flags & DDPF_RGB) || pf.bits < 16) {
        skip("primary is %lu bpp (flags 0x%lx); the scene needs an RGB desktop of 16 bpp or more",
             (unsigned long)pf.bits, (unsigned long)pf.flags);
        return;
    }
    pf.flags = DDPF_RGB;
    const DWORD mask = pf.r | pf.g | pf.b;

    Com<IDirectDrawClipper> clip;
    if (!check_hr("CreateClipper", g_dd->CreateClipper(0, clip.out(), NULL)) ||
        !check_hr("Clipper SetHWnd", clip->SetHWnd(0, g_wnd)) ||
        !check_hr("primary SetClipper", primary->SetClipper(clip)))
        return;

    SetForegroundWindow(g_wnd);
    UpdateWindow(g_wnd);
    pump();
    Sleep(100);
    pump();
    RECT cr;
    GetClientRect(g_wnd, &cr);
    POINT o = {0, 0};
    ClientToScreen(g_wnd, &o);
    const int w = cr.right, h = cr.bottom;
    if (w != WIN_W || h != WIN_H)
        printf("    note: client area is %dx%d\n", w, h);

    Com<IDirectDrawSurface7> bb, rb;
    bb.p = make(SYS_PLAIN, &pf, w, h);
    rb.p = make(SYS_PLAIN, &pf, w, h);
    if (!bb || !rb)
        return;
    Img pattern(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pattern.at(x, y) = present_value(31, x, y) & mask;
    if (!put_image(bb, pf.bytes, &pattern, "back buffer"))
        return;

    /* The back buffer onto the client area (screen coordinates). */
    RECT dr = {o.x, o.y, o.x + w, o.y + h};
    snprintf(what, sizeof(what), "Blt back buffer -> primary (%ld,%ld)-(%ld,%ld)", dr.left, dr.top, dr.right,
             dr.bottom);
    if (!check_hr(what, primary->Blt(&dr, bb, NULL, DDBLT_WAIT, NULL)))
        return;
    expect_primary(primary, rb, &pf, o.x, o.y, &pattern, mask, "client area after the Blt to the primary");

    /* Clipping: with the clipper on a child window, a Blt covering the whole
     * client area only reaches the child; the rest keeps the first image. */
    HWND child = CreateWindowA("qdd_smoke_child", "", WS_CHILD | WS_VISIBLE, 64, 48, 128, 96, g_wnd, NULL,
                               GetModuleHandleA(NULL), NULL);
    if (!child) {
        fail("CreateWindow(child): error %lu", (unsigned long)GetLastError());
        return;
    }
    pump();
    Sleep(50);
    pump();
    Img before(w, h);
    if (read_primary(primary, rb, &pf, o.x, o.y, w, h, &before) && check_hr("Clipper SetHWnd(child)", clip->SetHWnd(0, child))) {
        Img second(w, h), want(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                second.at(x, y) = present_value(32, x, y) & mask;
                int in = x >= 64 && x < 192 && y >= 48 && y < 144;
                want.at(x, y) = in ? second.at(x, y) : before.at(x, y);
            }
        if (put_image(bb, pf.bytes, &second, "back buffer") &&
            check_hr("Blt to the primary clipped to a child window", primary->Blt(&dr, bb, NULL, DDBLT_WAIT, NULL)))
            expect_primary(primary, rb, &pf, o.x, o.y, &want, mask,
                           "client area after a Blt clipped to the child (64,48)-(192,144)");
    }
    clip->SetHWnd(0, g_wnd);
    DestroyWindow(child);
    pump();
}

/* ------------------------------------------------------------------------ */
/* Fullscreen (--fullscreen only: changes the display mode)                  */

/* Fills each surface of a flipping chain with its own value, flips, and
 * checks that the surface objects stay put while the contents rotate:
 * after a Flip, buffer i holds what buffer i + 1 held (the front buffer
 * what the first back buffer held, the last back buffer what the front
 * buffer held). */
static void flip_chain(DWORD bpp, DWORD backbuffers, IDirectDrawPalette *pal)
{
    char what[160];
    DDSURFACEDESC2 d;
    desc_init(&d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = backbuffers;
    Com<IDirectDrawSurface7> front;
    snprintf(what, sizeof(what), "CreateSurface(primary, %lu back buffers) at %lu bpp", (unsigned long)backbuffers,
             (unsigned long)bpp);
    if (!check_hr(what, g_dd->CreateSurface(&d, front.out(), NULL)))
        return;
    if (pal && !check_hr("primary SetPalette", front->SetPalette(pal)))
        return;

    IDirectDrawSurface7 *chain[4] = {front.p, NULL, NULL, NULL};
    const DWORD n = backbuffers + 1;
    DDSCAPS2 caps;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    if (!check_hr("GetAttachedSurface(DDSCAPS_BACKBUFFER)", front->GetAttachedSurface(&caps, &chain[1])))
        return;
    caps.dwCaps = DDSCAPS_FLIP;
    for (DWORD i = 2; i < n; ++i)
        if (!check_hr("GetAttachedSurface(DDSCAPS_FLIP)", chain[i - 1]->GetAttachedSurface(&caps, &chain[i])))
            goto out;
    {
        IDirectDrawSurface7 *wrap = NULL;
        if (check_hr("GetAttachedSurface(DDSCAPS_FLIP) from the last back buffer",
                     chain[n - 1]->GetAttachedSurface(&caps, &wrap))) {
            if (wrap != front.p)
                fail("the last back buffer's next surface is %p, expected the front buffer %p", (void *)wrap,
                     (void *)front.p);
            wrap->Release();
        }
    }
    {
        const int bytes = (int)bpp / 8;
        DWORD value[4];
        for (DWORD i = 0; i < n; ++i) {
            DDBLTFX fx;
            init_fx(&fx);
            value[i] = bpp == 8 ? 0x11 * (i + 1) : 0x1111 * (i + 1) + 0x0842;
            fx.dwFillColor = value[i];
            snprintf(what, sizeof(what), "color fill of chain surface %lu", (unsigned long)i);
            if (!check_hr(what, chain[i]->Blt(NULL, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &fx)))
                goto out;
        }
        for (DWORD flip = 1; flip <= n; ++flip) {
            if (!check_hr("Flip", front->Flip(NULL, DDFLIP_WAIT)))
                goto out;
            /* Identities: the attached back buffer is still chain[1]. */
            IDirectDrawSurface7 *bb = NULL;
            caps.dwCaps = DDSCAPS_BACKBUFFER;
            if (check_hr("GetAttachedSurface after Flip", front->GetAttachedSurface(&caps, &bb))) {
                if (bb != chain[1])
                    fail("after Flip %lu the back buffer is %p, expected the same object %p", (unsigned long)flip,
                         (void *)bb, (void *)chain[1]);
                bb->Release();
            }
            for (DWORD i = 0; i < n; ++i) {
                DWORD want = value[(i + flip) % n];
                Img px(4, 4);
                snprintf(what, sizeof(what), "%lu bpp chain surface %lu after %lu Flip(s)", (unsigned long)bpp,
                         (unsigned long)i, (unsigned long)flip);
                if (!get_image(chain[i], bytes, &px, what))
                    goto out;
                if ((px.px[5] & bytes_mask(bytes)) != want)
                    fail("%s holds 0x%lx, expected 0x%lx", what, (unsigned long)(px.px[5] & bytes_mask(bytes)),
                         (unsigned long)want);
            }
        }
    }
out:
    for (DWORD i = 1; i < 4; ++i)
        if (chain[i])
            chain[i]->Release();
}

static void scene_fullscreen_flip(void)
{
    HRESULT hr = g_dd->SetCooperativeLevel(g_wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (!check_hr("SetCooperativeLevel(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)", hr))
        return;
    if (check_hr("SetDisplayMode(640, 480, 8)", g_dd->SetDisplayMode(640, 480, 8, 0, 0))) {
        PALETTEENTRY pe[256];
        make_entries(pe, 5);
        Com<IDirectDrawPalette> pal;
        if (check_hr("CreatePalette", g_dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, pal.out(), NULL)))
            flip_chain(8, 2, pal);
    }
    if (check_hr("SetDisplayMode(640, 480, 16)", g_dd->SetDisplayMode(640, 480, 16, 0, 0)))
        flip_chain(16, 3, NULL);
    check_hr("RestoreDisplayMode", g_dd->RestoreDisplayMode());
    check_hr("SetCooperativeLevel(DDSCL_NORMAL)", g_dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL));
    pump();
}

/* ------------------------------------------------------------------------ */
/* Driver                                                                    */

static int selected(const char *name)
{
    if (!g_only)
        return 1;
    size_t n = strlen(name);
    for (const char *p = g_only; p && *p;) {
        if (!strncmp(p, name, n) && (p[n] == ',' || p[n] == 0))
            return 1;
        p = strchr(p, ',');
        if (p)
            ++p;
    }
    return 0;
}

static void run(const char *name, void (*fn)(void))
{
    if (!selected(name))
        return;
    pump();
    ++g_scenes;
    g_fails = 0;
    g_first[0] = 0;
    g_skip[0] = 0;
    fn();
    if (g_fails) {
        ++g_failed;
        if (g_fails > 1)
            printf("FAIL %s: %s (+%d more)\n", name, g_first, g_fails - 1);
        else
            printf("FAIL %s: %s\n", name, g_first);
    } else if (g_skip[0]) {
        ++g_skipped;
        printf("SKIP %s: %s\n", name, g_skip);
    } else {
        printf("PASS %s\n", name);
    }
    fflush(stdout);
}

/* The module whose image holds the object's vtable. */
static HMODULE vtable_module(void *object)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(*(void **)object, &mbi, sizeof(mbi)))
        return NULL;
    return (HMODULE)mbi.AllocationBase;
}

static void print_d3d9(void)
{
    char path[MAX_PATH] = "";
    HMODULE h = GetModuleHandleA("d3d9.dll");
    if (h && GetModuleFileNameA(h, path, MAX_PATH))
        printf("d3d9.dll: %s\n", path);
    else
        printf("d3d9.dll: not loaded\n");
    fflush(stdout);
}

/* path's directory is dir (no trailing backslash), case-insensitively. */
static int in_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return n && !_strnicmp(path, dir, n) && path[n] == '\\' && !strchr(path + n + 1, '\\');
}

static void dump_caps(const char *name, const DDCAPS *c)
{
    printf("%s caps: dwCaps 0x%08lx dwCaps2 0x%08lx dwCKeyCaps 0x%08lx dwFXCaps 0x%08lx dwPalCaps 0x%08lx\n", name,
           (unsigned long)c->dwCaps, (unsigned long)c->dwCaps2, (unsigned long)c->dwCKeyCaps,
           (unsigned long)c->dwFXCaps, (unsigned long)c->dwPalCaps);
    printf("%s caps: SSB 0x%08lx/0x%08lx/0x%08lx SVB 0x%08lx/0x%08lx/0x%08lx\n", name, (unsigned long)c->dwSSBCaps,
           (unsigned long)c->dwSSBCKeyCaps, (unsigned long)c->dwSSBFXCaps, (unsigned long)c->dwSVBCaps,
           (unsigned long)c->dwSVBCKeyCaps, (unsigned long)c->dwSVBFXCaps);
}

int main(int argc, char **argv)
{
    int expect_local = 0, expect_stock = 0, expect_fallback = 0, fullscreen = 0, want_caps = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--expect-local"))
            expect_local = 1;
        else if (!strcmp(argv[i], "--expect-stock"))
            expect_stock = 1;
        else if (!strcmp(argv[i], "--expect-fallback"))
            expect_fallback = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            fullscreen = 1;
        else if (!strcmp(argv[i], "--dump-caps"))
            want_caps = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_only = argv[++i];
        else {
            printf("usage: qdd_smoke [--expect-local|--expect-stock|--expect-fallback] [--only scene[,scene...]] "
                   "[--fullscreen] [--dump-caps]\n");
            return 2;
        }
    }

    /* Which ddraw.dll did the loader bind? */
    char mod[MAX_PATH] = "", exe[MAX_PATH] = "", sys[MAX_PATH] = "", wow[MAX_PATH] = "";
    HMODULE hdd = GetModuleHandleA("ddraw.dll");
    GetModuleFileNameA(hdd, mod, MAX_PATH);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash)
        *slash = 0;
    GetSystemDirectoryA(sys, MAX_PATH);
    {
        typedef UINT(WINAPI * wow_dir_fn)(LPSTR, UINT);
        wow_dir_fn wow_dir =
            (wow_dir_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemWow64DirectoryA");
        if (!wow_dir || !wow_dir(wow, MAX_PATH))
            wow[0] = 0;
    }
    int local = in_dir(mod, exe), system = in_dir(mod, sys) || (wow[0] && in_dir(mod, wow));
    printf("ddraw.dll: %s (%s)\n", mod,
           local ? "beside the executable" : system ? "system directory" : "neither beside the executable nor system");
    fflush(stdout);
    if ((expect_local || expect_fallback) && !local) {
        printf("FAIL: --expect-local, but ddraw.dll is not the one beside the executable\n");
        return 2;
    }
    if (expect_stock && !system) {
        printf("FAIL: --expect-stock, but ddraw.dll is not the system one\n");
        return 2;
    }

    /* Physical pixels even with display scaling (Vista+). */
    {
        typedef BOOL(WINAPI * dpi_fn)(void);
        dpi_fn dpi = (dpi_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        if (dpi)
            dpi();
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qdd_smoke";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    RegisterClassA(&wc);
    wc.lpszClassName = "qdd_smoke_child";
    wc.hbrBackground = NULL; /* paints nothing: keeps whatever was there */
    RegisterClassA(&wc);
    RECT r = {0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("qdd_smoke", "qdd smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right - r.left,
                          r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) {
        printf("FAIL: CreateWindow\n");
        return 1;
    }
    /* Windows 11 rounds window corners, hiding client pixels from a read of
     * the composed desktop; square them (no-op before Windows 11). */
    {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        typedef HRESULT(WINAPI * set_attr_fn)(HWND, DWORD, LPCVOID, DWORD);
        set_attr_fn set_attr =
            dwm ? (set_attr_fn)(void (*)(void))GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        DWORD square = 1; /* DWMWCP_DONOTROUND */
        if (set_attr)
            set_attr(g_wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &square, sizeof(square));
    }
    pump();

    HRESULT hr = DirectDrawCreateEx(NULL, (void **)&g_dd, IID_IDirectDraw7, NULL);
    if (FAILED(hr)) {
        printf("FAIL: DirectDrawCreateEx: %s\n", hrname(hr));
        return 1;
    }
    print_d3d9();
    {
        HMODULE owner = vtable_module(g_dd);
        char path[MAX_PATH] = "";
        GetModuleFileNameA(owner, path, MAX_PATH);
        printf("IDirectDraw7 object from: %s\n", owner ? path : "(unknown)");
        if (expect_local && owner != hdd) {
            printf("FAIL: --expect-local, but the object is not from the ddraw.dll beside the executable\n");
            return 2;
        }
        /* --expect-fallback: the front end beside us handed the creation to
         * the stock runtime (no QD3D session). */
        char opath[MAX_PATH] = "";
        if (owner)
            GetModuleFileNameA(owner, opath, MAX_PATH);
        if (expect_fallback && (owner == hdd || !(in_dir(opath, sys) || (wow[0] && in_dir(opath, wow))))) {
            printf("FAIL: --expect-fallback, but the object is not from the system ddraw.dll\n");
            return 2;
        }
    }
    hr = g_dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        printf("FAIL: SetCooperativeLevel(DDSCL_NORMAL): %s\n", hrname(hr));
        return 1;
    }
    memset(&g_hal, 0, sizeof(g_hal));
    memset(&g_hel, 0, sizeof(g_hel));
    g_hal.dwSize = g_hel.dwSize = sizeof(DDCAPS);
    hr = g_dd->GetCaps(&g_hal, &g_hel);
    if (FAILED(hr)) {
        printf("FAIL: GetCaps: %s\n", hrname(hr));
        return 1;
    }
    {
        DDSURFACEDESC2 dm;
        desc_init(&dm);
        if (SUCCEEDED(g_dd->GetDisplayMode(&dm)))
            printf("display: %lux%lu, %lu bpp\n", (unsigned long)dm.dwWidth, (unsigned long)dm.dwHeight,
                   (unsigned long)dm.ddpfPixelFormat.dwRGBBitCount);
    }
    if (want_caps) {
        dump_caps("HAL", &g_hal);
        dump_caps("HEL", &g_hel);
    }
    fflush(stdout);

    run("qi", scene_qi);
    run("lock_formats", scene_lock_formats);
    run("palette", scene_palette);
    run("colorkey", scene_colorkey);
    run("colorkey_dest", scene_colorkey_dest);
    run("stretch", scene_stretch);
    run("mirror", scene_mirror);
    run("fill", scene_fill);
    run("getdc", scene_getdc);
    run("windowed_present", scene_windowed_present);
    run("blt_overlap", scene_blt_overlap);
    if (fullscreen)
        run("fullscreen_flip", scene_fullscreen_flip);

    ULONG left = g_dd->Release();
    if (left)
        printf("note: %lu references left on the DirectDraw object\n", (unsigned long)left);
    print_d3d9();
    DestroyWindow(g_wnd);
    if (g_only && !g_scenes) {
        printf("FAIL: --only %s matched no scene%s\n", g_only, fullscreen ? "" : " (fullscreen scenes need --fullscreen)");
        return 1;
    }
    printf("%s: %d/%d scenes passed, %d skipped\n", g_failed ? "FAIL" : "PASS", g_scenes - g_failed - g_skipped,
           g_scenes, g_skipped);
    return g_failed ? 1 : 0;
}
