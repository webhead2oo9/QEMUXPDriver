/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7_legacy_smoke: deterministic Direct3D 1-6 scenes with pixel checks.
 *
 * The interfaces before Direct3D 7: IDirect3D3 with IDirect3DDevice3
 * (DirectX 6), IDirect3D2 with IDirect3DDevice2 (DirectX 5) and IDirect3D
 * with the execute-buffer IDirect3DDevice (DirectX 3), with their
 * viewports, lights, materials, texture handles and vertex buffers.  They
 * exist only on DirectDraw objects from DirectDrawCreate (a
 * DirectDrawCreateEx object answers E_NOINTERFACE), so every scene makes its
 * own: DirectDrawCreate, DDSCL_NORMAL on a small visible window, a 64x64
 * X8R8G8B8 video-memory DDSCAPS_3DDEVICE render target with an attached
 * 16-bit z-buffer, the HAL device of the scene's Direct3D version and a
 * viewport over the whole target.  Pixels are read back with Lock outside
 * BeginScene/EndScene.
 *
 * Links against ddraw.dll by import, so it exercises whichever ddraw.dll the
 * loader picks: the system runtime, or QD3D's DirectDraw / Direct3D front
 * end (qd3d/ddraw) when its ddraw.dll sits beside the executable.  The first
 * output lines report the loaded module paths; --expect-local /
 * --expect-stock turn the ddraw.dll location into a hard check.
 *
 * Expected colours follow the Direct3D formulas; where the legacy interfaces
 * leave room (device lists, defaults, return codes, clipping, the
 * TEXTUREMAPBLEND / texture stage interplay) the test holds what the stock
 * runtime (Windows 11 SysWOW64 ddraw.dll with d3dim.dll) does.  Colours
 * compare exactly, except lit, modulated and blended results (+-1 per
 * channel) and fog (+-2).  Each scene sets the state it relies on, samples
 * pixels at least 2 pixels inside primitive edges, and prints [PASS] or
 * [FAIL] with its name.
 */
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>

#define W 64 /* render target size */
#define H 64
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static HWND g_wnd;
static int g_expect_local;
static int g_failures, g_scenes;
static const char *g_only; /* --only <scene[,scene]> */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

/* Holds one reference, released when it goes out of scope. */
template <class T> struct Com {
    T *p;
    Com() : p(NULL) {}
    ~Com()
    {
        if (p)
            p->Release();
    }
    T *operator->() const { return p; }
    operator T *() const { return p; }

private:
    Com(const Com &);
    void operator=(const Com &);
};

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int check_hr(const char *what, HRESULT hr)
{
    if (FAILED(hr)) {
        printf("    %s failed: hr=0x%08lx\n", what, (unsigned long)hr);
        return 0;
    }
    return 1;
}

#define CHECK(expr)                       \
    do {                                  \
        if (!check_hr(#expr, (expr)))     \
            return 0;                     \
    } while (0)

#define EXPECT(cond, ...)                                    \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("    expectation failed: " #cond "  ");  \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            return 0;                                        \
        }                                                    \
    } while (0)

/* An exact return code. */
#define EXPECT_HR(expr, want)                                                                 \
    do {                                                                                      \
        HRESULT hr_ = (expr);                                                                 \
        if (hr_ != (HRESULT)(want)) {                                                         \
            printf("    %s returned 0x%08lx, want 0x%08lx\n", #expr, (unsigned long)hr_,     \
                   (unsigned long)(HRESULT)(want));                                           \
            return 0;                                                                         \
        }                                                                                     \
    } while (0)

static DWORD fdw(float f)
{
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}

static void set_d3drect(D3DRECT *r, LONG x1, LONG y1, LONG x2, LONG y2)
{
    r->x1 = x1;
    r->y1 = y1;
    r->x2 = x2;
    r->y2 = y2;
}

static D3DCOLORVALUE colour_value(float r, float g, float b, float a)
{
    D3DCOLORVALUE c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

static D3DVECTOR vec(float x, float y, float z)
{
    D3DVECTOR v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static const char *guid_name(const GUID &g)
{
    if (g == IID_IDirect3DRampDevice)
        return "IID_IDirect3DRampDevice";
    if (g == IID_IDirect3DRGBDevice)
        return "IID_IDirect3DRGBDevice";
    if (g == IID_IDirect3DHALDevice)
        return "IID_IDirect3DHALDevice";
    if (g == IID_IDirect3DMMXDevice)
        return "IID_IDirect3DMMXDevice";
    if (g == IID_IDirect3DRefDevice)
        return "IID_IDirect3DRefDevice";
    if (g == IID_IDirect3DNullDevice)
        return "IID_IDirect3DNullDevice";
    if (g == IID_IDirect3DTnLHalDevice)
        return "IID_IDirect3DTnLHalDevice";
    return "an unknown GUID";
}

/* The module holding the object's QueryInterface (the stock runtime keeps
 * some vtables in heap memory, but the methods in a DLL). */
static HMODULE object_module(void *object)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((*(void ***)object)[0], &mbi, sizeof(mbi)))
        return NULL;
    return (HMODULE)mbi.AllocationBase;
}

static void print_module(const char *what, HMODULE m)
{
    char path[MAX_PATH] = "";
    if (!m || !GetModuleFileNameA(m, path, MAX_PATH))
        snprintf(path, sizeof(path), "(no module at %p)", (void *)m);
    printf("%s: %s\n", what, path);
}

/* ------------------------------------------------------------------------ */
/* Pixels                                                                    */

struct Px {
    int x, y;
    DWORD want; /* 0xRRGGBB */
};

static int rgb_near(DWORD a, DWORD b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)((a >> s) & 0xff), cb = (int)((b >> s) & 0xff);
        if (ca - cb > tol || cb - ca > tol)
            return 0;
    }
    return 1;
}

/* 32-bit pixels of s (outside BeginScene/EndScene), masked to RGB. */
template <class S, class D> static int read_pixels_t(S *s, const Px *p, int n, DWORD *got)
{
    D d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(s->Lock(NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL));
    for (int i = 0; i < n; ++i)
        got[i] = ((const DWORD *)((const BYTE *)d.lpSurface + p[i].y * d.lPitch))[p[i].x] & 0x00ffffffu;
    CHECK(s->Unlock(NULL));
    return 1;
}

static int read_pixels(IDirectDrawSurface4 *s, const Px *p, int n, DWORD *got)
{
    return read_pixels_t<IDirectDrawSurface4, DDSURFACEDESC2>(s, p, n, got);
}

static int read_pixels(IDirectDrawSurface *s, const Px *p, int n, DWORD *got)
{
    return read_pixels_t<IDirectDrawSurface, DDSURFACEDESC>(s, p, n, got);
}

/* Every mismatch is printed; tol is the allowed difference per channel. */
template <class S> static int expect_pixels(S *s, const Px *p, int n, int tol)
{
    DWORD got[64];
    if (n > 64 || !read_pixels(s, p, n, got))
        return 0;
    int ok = 1;
    for (int i = 0; i < n; ++i) {
        if (!rgb_near(got[i], p[i].want, tol)) {
            printf("    pixel (%d,%d) = %06lx, want %06lx", p[i].x, p[i].y, (unsigned long)got[i],
                   (unsigned long)p[i].want);
            if (tol)
                printf(" +-%d", tol);
            printf("\n");
            ok = 0;
        }
    }
    return ok;
}

#define EXPECT_PIXELS(s, arr, tol)                                                     \
    do {                                                                               \
        if (!expect_pixels((s), (arr), (int)ARRAY_SIZE(arr), (tol)))                   \
            return 0;                                                                  \
    } while (0)

/* A primitive covering pixels [x0,x1) x [y0,y1): the centre and points 2-3
 * pixels inside each corner want `in` (+-tol), points 3 pixels outside the
 * middle of each edge want `out` (exact). */
template <class S> static int expect_rect(S *s, int x0, int y0, int x1, int y1, DWORD in, DWORD out, int tol)
{
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, n = 0;
    Px inside[5] = {{x0 + 2, y0 + 2, in}, {x1 - 3, y0 + 2, in}, {x0 + 2, y1 - 3, in}, {x1 - 3, y1 - 3, in},
                    {cx, cy, in}};
    Px around[4] = {{x0 - 3, cy, out}, {x1 + 2, cy, out}, {cx, y0 - 3, out}, {cx, y1 + 2, out}};
    Px outside[4];
    for (int i = 0; i < 4; ++i)
        if (around[i].x >= 0 && around[i].x < W && around[i].y >= 0 && around[i].y < H)
            outside[n++] = around[i];
    int ok = expect_pixels(s, inside, 5, tol);
    return expect_pixels(s, outside, n, 0) && ok;
}

#define EXPECT_RECT(s, x0, y0, x1, y1, in, out, tol)                    \
    do {                                                                \
        if (!expect_rect((s), (x0), (y0), (x1), (y1), (in), (out), (tol))) \
            return 0;                                                   \
    } while (0)

/* A 4x4 texture drawn over the size x size square at (x0, y0): the centre
 * pixel of each texel. */
template <class S> static int expect_texels(S *s, int x0, int y0, int size, const DWORD *rgb, int tol)
{
    Px p[16];
    int step = size / 4;
    for (int t = 0; t < 16; ++t) {
        p[t].x = x0 + step / 2 + step * (t % 4);
        p[t].y = y0 + step / 2 + step * (t / 4);
        p[t].want = rgb[t];
    }
    return expect_pixels(s, p, 16, tol);
}

/* ------------------------------------------------------------------------ */
/* Vertices                                                                  */

#define FVF_TD (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_TDT (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_XD (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define FVF_XN (D3DFVF_XYZ | D3DFVF_NORMAL)

struct TD { /* FVF_TD */
    float x, y, z, rhw;
    DWORD c;
};
struct TDT { /* FVF_TDT */
    float x, y, z, rhw;
    DWORD c;
    float u, v;
};
struct TLV { /* D3DTLVERTEX, D3DFVF_TLVERTEX, D3DVT_TLVERTEX */
    float x, y, z, rhw;
    DWORD c, s;
    float u, v;
};
struct LVX { /* D3DLVERTEX, D3DVT_LVERTEX */
    float x, y, z;
    DWORD reserved, c, s;
    float u, v;
};
struct VX { /* D3DVERTEX, D3DVT_VERTEX */
    float x, y, z, nx, ny, nz, u, v;
};
struct XD { /* FVF_XD */
    float x, y, z;
    DWORD c;
};
struct XN { /* FVF_XN */
    float x, y, z, nx, ny, nz;
};
static_assert(sizeof(TLV) == sizeof(D3DTLVERTEX), "D3DTLVERTEX layout");
static_assert(sizeof(LVX) == sizeof(D3DLVERTEX), "D3DLVERTEX layout");
static_assert(sizeof(VX) == sizeof(D3DVERTEX), "D3DVERTEX layout");

/* Execute buffers hold all three vertex types, 32 bytes each. */
union V32 {
    TLV tl;
    LVX l;
    VX v;
};
static_assert(sizeof(V32) == 32, "execute buffer vertex size");

/* Quads are in strip order: top left, top right, bottom left, bottom
 * right; as a list: kQuadList. */
static const WORD kQuadList[6] = {0, 1, 2, 2, 1, 3};

/* Pre-transformed quad over pixels [x0,x1) x [y0,y1). */
static void td_quad(TD *v, float x0, float y0, float x1, float y1, float z, DWORD c)
{
    TD q[4] = {{x0, y0, z, 1.0f, c}, {x1, y0, z, 1.0f, c}, {x0, y1, z, 1.0f, c}, {x1, y1, z, 1.0f, c}};
    memcpy(v, q, sizeof(q));
}

/* Pre-transformed quad over pixels [x0,x1) x [y0,y1) with texture
 * coordinates 0..1 mapped texel-exact (pixel centres are integer). */
static void tdt_quad(TDT *v, float x0, float y0, float x1, float y1, DWORD c)
{
    TDT q[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, c, 0.0f, 0.0f},
        {x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, c, 1.0f, 0.0f},
        {x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c, 0.0f, 1.0f},
        {x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, c, 1.0f, 1.0f},
    };
    memcpy(v, q, sizeof(q));
}

/* D3DTLVERTEX quad over pixels [x0,x1) x [y0,y1), texel-exact like
 * tdt_quad. */
static void tlv_quad(TLV *v, float x0, float y0, float x1, float y1, float z, DWORD c, DWORD s)
{
    TLV q[4] = {
        {x0 - 0.5f, y0 - 0.5f, z, 1.0f, c, s, 0.0f, 0.0f},
        {x1 - 0.5f, y0 - 0.5f, z, 1.0f, c, s, 1.0f, 0.0f},
        {x0 - 0.5f, y1 - 0.5f, z, 1.0f, c, s, 0.0f, 1.0f},
        {x1 - 0.5f, y1 - 0.5f, z, 1.0f, c, s, 1.0f, 1.0f},
    };
    memcpy(v, q, sizeof(q));
}

/* With identity transforms and a viewport over the whole 64x64 target
 * (clip volume -1..1), x and y land on pixel (x + 1) * 32, (1 - y) * 32. */
static float ndc_x(int px)
{
    return px / (W / 2.0f) - 1.0f;
}

static float ndc_y(int py)
{
    return 1.0f - py / (H / 2.0f);
}

/* Untransformed quads from left, top, right, bottom model coordinates. */
static void xd_quad(XD *v, float l, float t, float r, float b, float z, DWORD c)
{
    XD q[4] = {{l, t, z, c}, {r, t, z, c}, {l, b, z, c}, {r, b, z, c}};
    memcpy(v, q, sizeof(q));
}

static void xn_quad(XN *v, float l, float t, float r, float b, float z)
{
    XN q[4] = {{l, t, z, 0, 0, -1}, {r, t, z, 0, 0, -1}, {l, b, z, 0, 0, -1}, {r, b, z, 0, 0, -1}};
    memcpy(v, q, sizeof(q));
}

static void lvx_quad(LVX *v, float l, float t, float r, float b, float z, DWORD c)
{
    LVX q[4] = {{l, t, z, 0, c, 0, 0.0f, 0.0f},
                {r, t, z, 0, c, 0, 1.0f, 0.0f},
                {l, b, z, 0, c, 0, 0.0f, 1.0f},
                {r, b, z, 0, c, 0, 1.0f, 1.0f}};
    memcpy(v, q, sizeof(q));
}

static void vx_quad(VX *v, float l, float t, float r, float b, float z)
{
    VX q[4] = {{l, t, z, 0, 0, -1, 0.0f, 0.0f},
               {r, t, z, 0, 0, -1, 1.0f, 0.0f},
               {l, b, z, 0, 0, -1, 0.0f, 1.0f},
               {r, b, z, 0, 0, -1, 1.0f, 1.0f}};
    memcpy(v, q, sizeof(q));
}

/* ------------------------------------------------------------------------ */
/* Matrices, states, lighting                                                */

static const D3DMATRIX kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static D3DMATRIX translation(float x, float y, float z)
{
    D3DMATRIX m = kIdentity;
    m._41 = x;
    m._42 = y;
    m._43 = z;
    return m;
}

struct RS {
    D3DRENDERSTATETYPE state;
    DWORD value;
};

/* What every scene starts from (all Direct3D versions): no z, culling,
 * blending, alpha test, fog or specular; Gouraud; point sampling. */
static const RS kBaseStates[] = {
    {D3DRENDERSTATE_ZENABLE, D3DZB_FALSE},
    {D3DRENDERSTATE_ZWRITEENABLE, TRUE},
    {D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL},
    {D3DRENDERSTATE_CULLMODE, D3DCULL_NONE},
    {D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD},
    {D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID},
    {D3DRENDERSTATE_DITHERENABLE, FALSE},
    {D3DRENDERSTATE_ALPHABLENDENABLE, FALSE},
    {D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE},
    {D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO},
    {D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
    {D3DRENDERSTATE_FOGENABLE, FALSE},
    {D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE},
    {D3DRENDERSTATE_SPECULARENABLE, FALSE},
    {D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_NEAREST},
    {D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_NEAREST},
};

template <class D> static int set_states(D *dev, const RS *rs, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        HRESULT hr = dev->SetRenderState(rs[i].state, rs[i].value);
        if (FAILED(hr)) {
            printf("    SetRenderState(%d, 0x%lx) failed: hr=0x%08lx\n", (int)rs[i].state,
                   (unsigned long)rs[i].value, (unsigned long)hr);
            return 0;
        }
    }
    return 1;
}

/* IDirect3DDevice2 and IDirect3DDevice3: identity transforms, no material,
 * no ambient light. */
template <class D> static int reset_transforms_and_lights(D *dev)
{
    D3DMATRIX m = kIdentity;
    CHECK(dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &m));
    CHECK(dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &m));
    CHECK(dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &m));
    CHECK(dev->SetLightState(D3DLIGHTSTATE_MATERIAL, 0));
    CHECK(dev->SetLightState(D3DLIGHTSTATE_AMBIENT, 0));
    CHECK(dev->SetLightState(D3DLIGHTSTATE_FOGMODE, D3DFOG_NONE));
    return 1;
}

/* One coloured directional light shining into the screen at an angle:
 * normal (0, 0, -1) gives N.L = 0.8. */
static const float kLightColour[3] = {1.0f, 0.5f, 1.0f};
static const float kLightDir[3] = {0.0f, 0.6f, 0.8f};
static const float kMatDiffuse[3] = {0.5f, 1.0f, 0.25f};
static const float kMatAmbient[3] = {1.0f, 0.5f, 0.0f};
static const float kMatEmissive[3] = {0.0f, 0.0f, 0.25f};
#define LIT_AMBIENT 0x00404040u /* D3DLIGHTSTATE_AMBIENT */

static void lit_material(D3DMATERIAL *m)
{
    memset(m, 0, sizeof(*m));
    m->dwSize = sizeof(*m);
    m->diffuse = colour_value(kMatDiffuse[0], kMatDiffuse[1], kMatDiffuse[2], 1.0f);
    m->ambient = colour_value(kMatAmbient[0], kMatAmbient[1], kMatAmbient[2], 1.0f);
    m->emissive = colour_value(kMatEmissive[0], kMatEmissive[1], kMatEmissive[2], 0.0f);
}

/* size: sizeof(D3DLIGHT) (Direct3D 1, always active) or sizeof(D3DLIGHT2)
 * (dwFlags decides). */
static void lit_light(D3DLIGHT2 *l, DWORD size, DWORD flags)
{
    memset(l, 0, sizeof(*l));
    l->dwSize = size;
    l->dltType = D3DLIGHT_DIRECTIONAL;
    l->dcvColor = colour_value(kLightColour[0], kLightColour[1], kLightColour[2], 1.0f);
    l->dvDirection = vec(kLightDir[0], kLightDir[1], kLightDir[2]);
    l->dwFlags = flags;
}

/* The lighting equation for normal (0, 0, -1), the light and material
 * above: emissive + AMBIENT * ambient + light * diffuse * N.L, clamped,
 * as 0xRRGGBB. */
static DWORD lit_rgb(int light_on)
{
    float ndotl = kLightDir[2];
    DWORD out = 0;
    for (int i = 0; i < 3; ++i) {
        float ambient = ((LIT_AMBIENT >> (16 - 8 * i)) & 0xff) / 255.0f;
        float v = kMatEmissive[i] + kMatAmbient[i] * ambient;
        if (light_on)
            v += kMatDiffuse[i] * kLightColour[i] * ndotl;
        if (v > 1.0f)
            v = 1.0f;
        out = out << 8 | (DWORD)(v * 255.0f + 0.5f);
    }
    return out;
}

/* A material with diffuse colour (r, g, b) (backgrounds: 0 or 1 only, so
 * the colour is exact). */
template <class M> static int set_diffuse_material(M *m, float r, float g, float b)
{
    D3DMATERIAL d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.diffuse = colour_value(r, g, b, 1.0f);
    CHECK(m->SetMaterial(&d));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Surfaces and formats                                                      */

struct Fmt {
    const char *name;
    DWORD flags, bits, r, g, b, a;
};

static const Fmt kX8R8G8B8 = {"X8R8G8B8", DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0};
static const Fmt kA8R8G8B8 = {"A8R8G8B8", DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xff0000, 0xff00, 0xff, 0xff000000};
static const Fmt kR5G6B5 = {"R5G6B5", DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0};

static void set_pixel_format(DDPIXELFORMAT *pf, const Fmt *f)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = f->flags;
    pf->dwRGBBitCount = f->bits;
    pf->dwRBitMask = f->r;
    pf->dwGBitMask = f->g;
    pf->dwBBitMask = f->b;
    pf->dwRGBAlphaBitMask = f->a;
}

static int format_matches(const DDPIXELFORMAT *pf, const Fmt *f)
{
    const DWORD kind = DDPF_RGB | DDPF_ALPHAPIXELS | DDPF_ALPHA | DDPF_FOURCC | DDPF_PALETTEINDEXED8 | DDPF_LUMINANCE |
                       DDPF_BUMPDUDV;
    return (pf->dwFlags & kind) == f->flags && pf->dwRGBBitCount == f->bits && pf->dwRBitMask == f->r &&
           pf->dwGBitMask == f->g && pf->dwBBitMask == f->b &&
           (!(f->flags & DDPF_ALPHAPIXELS) || pf->dwRGBAlphaBitMask == f->a);
}

struct FormatList {
    DDPIXELFORMAT f[64];
    int n;
};

static HRESULT CALLBACK collect_format(DDPIXELFORMAT *pf, void *ctx)
{
    FormatList *l = (FormatList *)ctx;
    if (l->n < 64)
        l->f[l->n++] = *pf;
    return DDENUMRET_OK;
}

/* IDirect3DDevice2::EnumTextureFormats reports surface descriptions. */
static HRESULT CALLBACK collect_format_desc(DDSURFACEDESC *d, void *ctx)
{
    return collect_format(&d->ddpfPixelFormat, ctx);
}

static int listed(const FormatList *l, const Fmt *f)
{
    for (int i = 0; i < l->n; ++i)
        if (format_matches(&l->f[i], f))
            return 1;
    return 0;
}

static void print_format(const char *tag, const DDPIXELFORMAT *f)
{
    printf("    %s: flags=0x%08lx bpp=%lu masks=%08lx/%08lx/%08lx/%08lx", tag, (unsigned long)f->dwFlags,
           (unsigned long)f->dwRGBBitCount, (unsigned long)f->dwRBitMask, (unsigned long)f->dwGBitMask,
           (unsigned long)f->dwBBitMask, (unsigned long)f->dwRGBAlphaBitMask);
    if (f->dwFlags & DDPF_FOURCC)
        printf(" fourcc=%.4s", (const char *)&f->dwFourCC);
    printf("\n");
}

static int channel_shift(DWORD mask)
{
    int s = 0;
    while (s < 32 && !((mask >> s) & 1))
        ++s;
    return s;
}

static int channel_bits(DWORD mask)
{
    int s = channel_shift(mask), n = 0;
    while (s + n < 32 && ((mask >> (s + n)) & 1))
        ++n;
    return n;
}

static DWORD channel_pack(DWORD c8, DWORD mask)
{
    if (!mask)
        return 0;
    return ((c8 >> (8 - channel_bits(mask))) << channel_shift(mask)) & mask;
}

static DWORD channel_expand(DWORD raw, DWORD mask)
{
    DWORD max = (1u << channel_bits(mask)) - 1, v = (raw & mask) >> channel_shift(mask);
    return (v * 255 + max / 2) / max;
}

/* 0xAARRGGBB as a texel of format f. */
static DWORD format_pack(const Fmt *f, DWORD argb)
{
    return channel_pack((argb >> 16) & 0xff, f->r) | channel_pack((argb >> 8) & 0xff, f->g) |
           channel_pack(argb & 0xff, f->b) | channel_pack(argb >> 24, f->a);
}

/* The 0xRRGGBB a point-sampled texel of format f draws. */
static DWORD format_rgb(const Fmt *f, DWORD raw)
{
    return channel_expand(raw, f->r) << 16 | channel_expand(raw, f->g) << 8 | channel_expand(raw, f->b);
}

/* 27 opaque colours from the channel levels 0x00, 0x84, 0xff: exact in
 * 8, 6 and 5 bits (0x84 widens back the same by rounding and by bit
 * replication). */
static DWORD level_colour(int i)
{
    static const DWORD level[3] = {0x00, 0x84, 0xff};
    return 0xff000000u | level[i % 3] << 16 | level[i / 3 % 3] << 8 | level[i / 9 % 3];
}

static HRESULT create_surface4(IDirectDraw4 *dd, DWORD caps, DWORD w, DWORD h, const Fmt *f, IDirectDrawSurface4 **out)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddsCaps.dwCaps = caps;
    set_pixel_format(&d.ddpfPixelFormat, f);
    return dd->CreateSurface(&d, out, NULL);
}

static HRESULT create_surface1(IDirectDraw *dd, DWORD caps, DWORD w, DWORD h, const Fmt *f, IDirectDrawSurface **out)
{
    DDSURFACEDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddsCaps.dwCaps = caps;
    set_pixel_format(&d.ddpfPixelFormat, f);
    return dd->CreateSurface(&d, out, NULL);
}

#define RT_CAPS (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY)

/* A 16-bit video-memory z-buffer attached to rt: DDPF_ZBUFFER for the
 * DirectX 6 interfaces, DDSD_ZBUFFERBITDEPTH as DirectX 3/5 programs did. */
static int attach_z4(IDirectDraw4 *dd, IDirectDrawSurface4 *rt)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = W;
    d.dwHeight = H;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    d.ddpfPixelFormat.dwZBufferBitDepth = 16;
    d.ddpfPixelFormat.dwZBitMask = 0xffff;
    Com<IDirectDrawSurface4> z;
    CHECK(dd->CreateSurface(&d, &z.p, NULL));
    CHECK(rt->AddAttachedSurface(z));
    return 1;
}

static int attach_z1(IDirectDraw *dd, IDirectDrawSurface *rt)
{
    DDSURFACEDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
    d.dwWidth = W;
    d.dwHeight = H;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    d.dwZBufferBitDepth = 16;
    Com<IDirectDrawSurface> z;
    CHECK(dd->CreateSurface(&d, &z.p, NULL));
    CHECK(rt->AddAttachedSurface(z));
    return 1;
}

/* Writes w x h raw texels (row-major) through Lock. */
template <class S, class D> static int write_texels_t(S *s, DWORD bits, const DWORD *raw, int w, int h)
{
    D d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(s->Lock(NULL, &d, DDLOCK_WAIT, NULL));
    for (int y = 0; y < h; ++y) {
        BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
        for (int x = 0; x < w; ++x) {
            if (bits == 32)
                ((DWORD *)row)[x] = raw[y * w + x];
            else
                ((WORD *)row)[x] = (WORD)raw[y * w + x];
        }
    }
    CHECK(s->Unlock(NULL));
    return 1;
}

static int write_texels(IDirectDrawSurface4 *s, DWORD bits, const DWORD *raw)
{
    return write_texels_t<IDirectDrawSurface4, DDSURFACEDESC2>(s, bits, raw, 4, 4);
}

static int write_texels(IDirectDrawSurface *s, DWORD bits, const DWORD *raw)
{
    return write_texels_t<IDirectDrawSurface, DDSURFACEDESC>(s, bits, raw, 4, 4);
}

template <class S> static int fill_texels(S *s, DWORD bits, DWORD raw)
{
    DWORD all[16];
    for (int i = 0; i < 16; ++i)
        all[i] = raw;
    return write_texels(s, bits, all);
}

/* 4x4 texel values and the colours they draw: level_colour(t + offset). */
static void level_texels(const Fmt *f, int offset, DWORD *raw, DWORD *rgb)
{
    for (int t = 0; t < 16; ++t) {
        raw[t] = format_pack(f, level_colour((t + offset) % 27));
        rgb[t] = format_rgb(f, raw[t]);
    }
}

/* ------------------------------------------------------------------------ */
/* Device enumeration                                                        */

struct DeviceRecord {
    GUID guid;
    char name[64], desc[128];
    DWORD hal_size, hal_flags, hal_caps, hel_caps;
    D3DCOLORMODEL hal_model, hel_model;
};

struct DeviceList {
    DeviceRecord d[8];
    int n;
    HRESULT ret;
};

static HRESULT CALLBACK collect_device(GUID *guid, char *desc, char *name, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel,
                                       void *ctx)
{
    DeviceList *l = (DeviceList *)ctx;
    if (l->n < 8) {
        DeviceRecord *r = &l->d[l->n];
        memset(r, 0, sizeof(*r));
        if (guid)
            r->guid = *guid;
        snprintf(r->name, sizeof(r->name), "%s", name ? name : "(null)");
        snprintf(r->desc, sizeof(r->desc), "%s", desc ? desc : "(null)");
        if (hal) {
            r->hal_size = hal->dwSize;
            r->hal_flags = hal->dwFlags;
            r->hal_caps = hal->dwDevCaps;
            r->hal_model = hal->dcmColorModel;
        }
        if (hel) {
            r->hel_caps = hel->dwDevCaps;
            r->hel_model = hel->dcmColorModel;
        }
    }
    ++l->n;
    return l->ret;
}

struct DeviceWant {
    const GUID *guid;
    const char *name;
};

/* Enumerates, prints, and checks the list against want; then that
 * EnumDevices(NULL) fails and a cancelling callback ends it. */
template <class D3D> static int check_devices(D3D *d3d, const DeviceWant *want, int n)
{
    DeviceList l;
    memset(&l, 0, sizeof(l));
    l.ret = DDENUMRET_OK;
    CHECK(d3d->EnumDevices(collect_device, &l));
    for (int i = 0; i < l.n && i < 8; ++i)
        printf("    device %d: \"%s\" / \"%s\", %s, hal: size %lu flags 0x%08lx devcaps 0x%08lx colour model %lu, "
               "hel: devcaps 0x%08lx colour model %lu\n",
               i, l.d[i].name, l.d[i].desc, guid_name(l.d[i].guid), (unsigned long)l.d[i].hal_size,
               (unsigned long)l.d[i].hal_flags, (unsigned long)l.d[i].hal_caps, (unsigned long)l.d[i].hal_model,
               (unsigned long)l.d[i].hel_caps, (unsigned long)l.d[i].hel_model);
    EXPECT(l.n == n, "%d devices, want %d", l.n, n);
    for (int i = 0; i < n; ++i) {
        EXPECT(l.d[i].guid == *want[i].guid, "device %d is %s, want %s", i, guid_name(l.d[i].guid),
               guid_name(*want[i].guid));
        EXPECT(!strcmp(l.d[i].name, want[i].name), "device %d is called \"%s\", want \"%s\"", i, l.d[i].name,
               want[i].name);
    }
    EXPECT_HR(d3d->EnumDevices(NULL, NULL), DDERR_INVALIDPARAMS);
    memset(&l, 0, sizeof(l));
    l.ret = DDENUMRET_CANCEL;
    EXPECT_HR(d3d->EnumDevices(collect_device, &l), D3D_OK);
    EXPECT(l.n == 1, "%d callbacks after DDENUMRET_CANCEL", l.n);
    return 1;
}

/* FindDevice for a hardware / software device. */
template <class D3D> static int check_find_device(D3D *d3d, BOOL hardware, const GUID &want)
{
    D3DFINDDEVICESEARCH s;
    D3DFINDDEVICERESULT r;
    memset(&s, 0, sizeof(s));
    s.dwSize = sizeof(s);
    s.dwFlags = D3DFDS_HARDWARE;
    s.bHardware = hardware;
    memset(&r, 0, sizeof(r));
    r.dwSize = sizeof(r);
    CHECK(d3d->FindDevice(&s, &r));
    EXPECT(r.guid == want, "FindDevice(D3DFDS_HARDWARE, %d) found %s, want %s", hardware, guid_name(r.guid),
           guid_name(want));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Direct3D 3: IDirectDraw4, IDirect3D3, IDirect3DDevice3, IDirect3DViewport3 */

struct Ctx3 {
    Com<IDirectDraw4> dd;
    Com<IDirect3D3> d3d;
    Com<IDirectDrawSurface4> rt;
    Com<IDirect3DDevice3> dev;
    Com<IDirect3DViewport3> vp;
    ~Ctx3()
    {
        if (dev.p && vp.p)
            dev->DeleteViewport(vp);
    }
};

static int set_viewport2(IDirect3DViewport3 *vp, DWORD x, DWORD y, DWORD w, DWORD h, float clip_x, float clip_y,
                         float clip_w, float clip_h)
{
    D3DVIEWPORT2 v;
    memset(&v, 0, sizeof(v));
    v.dwSize = sizeof(v);
    v.dwX = x;
    v.dwY = y;
    v.dwWidth = w;
    v.dwHeight = h;
    v.dvClipX = clip_x;
    v.dvClipY = clip_y;
    v.dvClipWidth = clip_w;
    v.dvClipHeight = clip_h;
    v.dvMinZ = 0.0f;
    v.dvMaxZ = 1.0f;
    CHECK(vp->SetViewport2(&v));
    return 1;
}

static int ctx3_init(Ctx3 &c)
{
    static int printed;
    Com<IDirectDraw> dd1;
    CHECK(DirectDrawCreate(NULL, &dd1.p, NULL));
    CHECK(dd1->QueryInterface(IID_IDirectDraw4, (void **)&c.dd.p));
    CHECK(c.dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL));
    CHECK(c.dd->QueryInterface(IID_IDirect3D3, (void **)&c.d3d.p));
    CHECK(create_surface4(c.dd, RT_CAPS, W, H, &kX8R8G8B8, &c.rt.p));
    if (!attach_z4(c.dd, c.rt))
        return 0;
    CHECK(c.d3d->CreateDevice(IID_IDirect3DHALDevice, c.rt, &c.dev.p, NULL));
    CHECK(c.d3d->CreateViewport(&c.vp.p, NULL));
    CHECK(c.dev->AddViewport(c.vp));
    if (!set_viewport2(c.vp, 0, 0, W, H, -1.0f, 1.0f, 2.0f, 2.0f))
        return 0;
    CHECK(c.dev->SetCurrentViewport(c.vp));
    if (!printed++)
        print_module("    IDirect3DDevice3 object from", object_module(c.dev));
    return 1;
}

/* The base states; TEXTUREMAPBLEND keeps its default (MODULATE). */
static int base3(Ctx3 &c)
{
    if (!set_states(c.dev.p, kBaseStates, ARRAY_SIZE(kBaseStates)))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE));
    CHECK(c.dev->SetTexture(0, NULL));
    return reset_transforms_and_lights(c.dev.p);
}

static int clear3(Ctx3 &c, DWORD rgb)
{
    D3DRECT r;
    set_d3drect(&r, 0, 0, W, H);
    CHECK(c.vp->Clear2(1, &r, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u | rgb, 1.0f, 0));
    return 1;
}

/* A 4x4 texture of format f and its IDirect3DTexture2. */
static int texture4(Ctx3 &c, const Fmt *f, IDirectDrawSurface4 **s, IDirect3DTexture2 **t)
{
    CHECK(create_surface4(c.dd, DDSCAPS_TEXTURE, 4, 4, f, s));
    CHECK((*s)->QueryInterface(IID_IDirect3DTexture2, (void **)t));
    return 1;
}

/* Clears to bg, then draws a textured quad over the whole target. */
static int draw_full_textured3(Ctx3 &c, DWORD bg, DWORD diffuse)
{
    TDT q[4];
    tdt_quad(q, 0, 0, W, H, diffuse);
    if (!clear3(c, bg))
        return 0;
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TDT, q, 4, 0));
    CHECK(c.dev->EndScene());
    return 1;
}

/* EnumDevices, FindDevice, EnumZBufferFormats, EnumTextureFormats. */
static int scene_enum3(void)
{
    static const DeviceWant want[] = {
        {&IID_IDirect3DRGBDevice, "RGB Emulation"},
        {&IID_IDirect3DHALDevice, "Direct3D HAL"},
    };
    Ctx3 c;
    if (!ctx3_init(c))
        return 0;
    if (!check_devices(c.d3d.p, want, ARRAY_SIZE(want)) ||
        !check_find_device(c.d3d.p, TRUE, IID_IDirect3DHALDevice) ||
        !check_find_device(c.d3d.p, FALSE, IID_IDirect3DRGBDevice))
        return 0;

    D3DDEVICEDESC hal, hel;
    memset(&hal, 0, sizeof(hal));
    memset(&hel, 0, sizeof(hel));
    hal.dwSize = sizeof(hal);
    hel.dwSize = sizeof(hel);
    CHECK(c.dev->GetCaps(&hal, &hel));
    printf("    caps: devcaps 0x%08lx, texture caps 0x%08lx, texture blend caps 0x%08lx, filter caps 0x%08lx, "
           "%u stages\n",
           (unsigned long)hal.dwDevCaps, (unsigned long)hal.dpcTriCaps.dwTextureCaps,
           (unsigned long)hal.dpcTriCaps.dwTextureBlendCaps, (unsigned long)hal.dpcTriCaps.dwTextureFilterCaps,
           hal.wMaxTextureBlendStages);

    FormatList z;
    z.n = 0;
    CHECK(c.d3d->EnumZBufferFormats(IID_IDirect3DHALDevice, collect_format, &z));
    int z16 = 0;
    for (int i = 0; i < z.n; ++i) {
        printf("    zfmt: flags=0x%08lx bits=%lu zmask=%08lx stencil=%lu/%08lx\n", (unsigned long)z.f[i].dwFlags,
               (unsigned long)z.f[i].dwZBufferBitDepth, (unsigned long)z.f[i].dwZBitMask,
               (unsigned long)z.f[i].dwStencilBitDepth, (unsigned long)z.f[i].dwStencilBitMask);
        if ((z.f[i].dwFlags & DDPF_ZBUFFER) && z.f[i].dwZBufferBitDepth == 16 && z.f[i].dwZBitMask == 0xffff)
            z16 = 1;
    }
    EXPECT(z16, "no 16-bit z-buffer format among %d", z.n);

    FormatList t;
    t.n = 0;
    CHECK(c.dev->EnumTextureFormats(collect_format, &t));
    for (int i = 0; i < t.n; ++i)
        print_format("texfmt3", &t.f[i]);
    EXPECT(listed(&t, &kX8R8G8B8) && listed(&t, &kA8R8G8B8) && listed(&t, &kR5G6B5),
           "X8R8G8B8 %d, A8R8G8B8 %d, R5G6B5 %d among %d texture formats", listed(&t, &kX8R8G8B8),
           listed(&t, &kA8R8G8B8), listed(&t, &kR5G6B5), t.n);
    return 1;
}

/* Viewport3 clears: Clear2 of the target and z exact to the rectangle
 * edges, a z-only rectangle that keeps a z-tested quad out, Clear with the
 * background material, and a viewport smaller than the target. */
static HRESULT draw_td3(IDirect3DDevice3 *dev, float x0, float y0, float x1, float y1, float z, DWORD c)
{
    TD v[4];
    td_quad(v, x0, y0, x1, y1, z, c);
    return dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TD, v, 4, 0);
}

static int scene_clear2(void)
{
    const DWORD bg = 0x2040c0, c1 = 0x00ff80, quad = 0xffff00, material = 0xff00ff;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    D3DVIEWPORT2 v;
    memset(&v, 0xcc, sizeof(v));
    v.dwSize = sizeof(v);
    CHECK(c.vp->GetViewport2(&v));
    EXPECT(v.dwX == 0 && v.dwY == 0 && v.dwWidth == W && v.dwHeight == H && v.dvClipX == -1.0f &&
               v.dvClipY == 1.0f && v.dvClipWidth == 2.0f && v.dvClipHeight == 2.0f && v.dvMinZ == 0.0f &&
               v.dvMaxZ == 1.0f,
           "GetViewport2: %lu,%lu %lux%lu clip %g,%g %gx%g z %g-%g", (unsigned long)v.dwX, (unsigned long)v.dwY,
           (unsigned long)v.dwWidth, (unsigned long)v.dwHeight, v.dvClipX, v.dvClipY, v.dvClipWidth,
           v.dvClipHeight, v.dvMinZ, v.dvMaxZ);

    D3DRECT full;
    set_d3drect(&full, 0, 0, W, H);
    CHECK(c.vp->Clear2(1, &full, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u | bg, 1.0f, 0));
    Px all[] = {{0, 0, bg}, {W - 1, 0, bg}, {0, H - 1, bg}, {W - 1, H - 1, bg}, {32, 32, bg}};
    EXPECT_PIXELS(c.rt.p, all, 0);

    D3DRECT r[2];
    set_d3drect(&r[0], 4, 4, 20, 12);
    set_d3drect(&r[1], 32, 36, 60, 60);
    CHECK(c.vp->Clear2(2, r, D3DCLEAR_TARGET, 0xff000000u | c1, 1.0f, 0));
    Px part[] = {
        {4, 4, c1},   {19, 11, c1}, {12, 8, c1},   {3, 4, bg},    {20, 4, bg},   {4, 3, bg},   {4, 12, bg},  {32, 36, c1},
        {59, 59, c1}, {46, 48, c1}, {31, 48, bg},  {60, 48, bg},  {46, 35, bg},  {46, 60, bg}, {26, 24, bg},
    };
    EXPECT_PIXELS(c.rt.p, part, 0);

    D3DRECT zr;
    set_d3drect(&zr, 8, 20, 24, 32);
    CHECK(c.vp->Clear2(1, &zr, D3DCLEAR_ZBUFFER, 0, 0.0f, 0));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE));
    CHECK(c.dev->BeginScene());
    CHECK(draw_td3(c.dev, 0, 0, W, H, 0.5f, 0xff000000u | quad));
    CHECK(c.dev->EndScene());
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE));
    Px z[] = {{8, 20, bg},   {23, 31, bg},   {16, 26, bg},  {7, 26, quad},
              {24, 26, quad}, {16, 19, quad}, {16, 32, quad}, {50, 50, quad}};
    EXPECT_PIXELS(c.rt.p, z, 0);

    Com<IDirect3DMaterial3> mat;
    CHECK(c.d3d->CreateMaterial(&mat.p, NULL));
    if (!set_diffuse_material(mat.p, 1.0f, 0.0f, 1.0f))
        return 0;
    D3DMATERIALHANDLE h = 0, got = 0;
    CHECK(mat->GetHandle(c.dev, &h));
    EXPECT(h != 0, "material handle 0");
    CHECK(c.vp->SetBackground(h));
    BOOL valid = FALSE;
    CHECK(c.vp->GetBackground(&got, &valid));
    EXPECT(got == h && valid, "GetBackground: handle 0x%lx valid %d, want 0x%lx", (unsigned long)got, valid,
           (unsigned long)h);
    D3DRECT br;
    set_d3drect(&br, 40, 4, 60, 28);
    CHECK(c.vp->Clear(1, &br, D3DCLEAR_TARGET));
    Px mc[] = {{40, 4, material}, {59, 27, material}, {50, 16, material}, {39, 16, quad},
               {60, 16, quad},    {50, 3, quad},      {50, 28, quad}};
    EXPECT_PIXELS(c.rt.p, mc, 0);

    /* A 32x32 viewport at (16, 16): Clear2 of the whole target clears the
     * viewport only. */
    if (!set_viewport2(c.vp, 16, 16, 32, 32, -1.0f, 1.0f, 2.0f, 2.0f))
        return 0;
    CHECK(c.vp->Clear2(1, &full, D3DCLEAR_TARGET, 0xff000000u | c1, 1.0f, 0));
    Px vpc[] = {{16, 16, c1},   {47, 47, c1},   {32, 32, c1},   {13, 36, quad},
                {50, 36, quad}, {36, 13, quad}, {36, 50, quad}};
    EXPECT_PIXELS(c.rt.p, vpc, 0);
    return 1;
}

/* D3DFVF_TLVERTEX quads: exact colours, the specular colour added only
 * with SPECULARENABLE, flat shading from each triangle's first vertex; a
 * draw outside BeginScene/EndScene succeeds and renders. */
static int scene_tl3(void)
{
    const DWORD bg = 0x000080;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c) || !clear3(c, bg))
        return 0;
    TLV q[4];
    CHECK(c.dev->BeginScene());
    tlv_quad(q, 4, 4, 28, 28, 0.5f, 0xffff0000u, 0x00404040u);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, q, 4, 0));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, TRUE));
    tlv_quad(q, 36, 4, 60, 28, 0.5f, 0xff208040u, 0x00402010u);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, q, 4, 0));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_FLAT));
    TLV f[4] = {
        {36, 36, 0.5f, 1.0f, 0xffff0000u, 0, 0.0f, 0.0f}, {60, 36, 0.5f, 1.0f, 0xff00ff00u, 0, 0.0f, 0.0f},
        {36, 60, 0.5f, 1.0f, 0xff0000ffu, 0, 0.0f, 0.0f}, {60, 60, 0.5f, 1.0f, 0xffffffffu, 0, 0.0f, 0.0f},
    };
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, f, 4, 0));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD));
    CHECK(c.dev->EndScene());
    tlv_quad(q, 4, 36, 28, 60, 0.5f, 0xff00ff00u, 0);
    HRESULT hr = c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, q, 4, 0);
    printf("    DrawPrimitive outside BeginScene/EndScene: hr=0x%08lx\n", (unsigned long)hr);
    EXPECT(hr == D3D_OK, "DrawPrimitive outside a scene returned 0x%08lx", (unsigned long)hr);

    EXPECT_RECT(c.rt.p, 4, 4, 28, 28, 0xff0000, bg, 0);
    EXPECT_RECT(c.rt.p, 36, 4, 60, 28, 0x60a050, bg, 0);
    EXPECT_RECT(c.rt.p, 4, 36, 28, 60, 0x00ff00, bg, 0);
    Px flat[] = {{38, 38, 0xff0000}, {44, 40, 0xff0000}, {57, 57, 0x00ff00}, {52, 56, 0x00ff00}};
    EXPECT_PIXELS(c.rt.p, flat, 0);
    return 1;
}

/* Untransformed D3DFVF_XYZ | D3DFVF_DIFFUSE quads (no normal: unlit):
 * identity transforms, a world translation, and a clip volume twice as wide
 * and high, which halves the picture. */
static int draw_xd3(Ctx3 &c, DWORD bg, const XD *q)
{
    if (!clear3(c, bg))
        return 0;
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XD, (void *)q, 4, 0));
    CHECK(c.dev->EndScene());
    return 1;
}

static int scene_xform3(void)
{
    const DWORD bg = 0x101010, green = 0x00ff00;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    XD q[4];
    xd_quad(q, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0xff000000u | green);
    if (!draw_xd3(c, bg, q))
        return 0;
    EXPECT_RECT(c.rt.p, 16, 16, 48, 48, green, bg, 0);

    D3DMATRIX world = translation(0.25f, -0.25f, 0.0f), got;
    CHECK(c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &world));
    memset(&got, 0xcc, sizeof(got));
    CHECK(c.dev->GetTransform(D3DTRANSFORMSTATE_WORLD, &got));
    EXPECT(!memcmp(&got, &world, sizeof(got)), "GetTransform(WORLD) differs from SetTransform");
    if (!draw_xd3(c, bg, q))
        return 0;
    EXPECT_RECT(c.rt.p, 24, 24, 56, 56, green, bg, 0);

    world = kIdentity;
    CHECK(c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &world));
    if (!set_viewport2(c.vp, 0, 0, W, H, -2.0f, 2.0f, 4.0f, 4.0f) || !draw_xd3(c, bg, q))
        return 0;
    EXPECT_RECT(c.rt.p, 24, 24, 40, 40, green, bg, 0);
    return 1;
}

/* A D3DLIGHT2 directional light added to the viewport, an
 * IDirect3DMaterial3 handle in D3DLIGHTSTATE_MATERIAL, the ambient light
 * state: D3DFVF_XYZ | D3DFVF_NORMAL quads lit by the lighting equation; no
 * lighting with D3DDP_DONOTLIGHT or without a material (white, the default
 * vertex colour); a light without D3DLIGHT_ACTIVE adds nothing. */
static int scene_light3(void)
{
    const DWORD bg = 0x102030;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c) || !clear3(c, bg))
        return 0;
    Com<IDirect3DMaterial3> mat;
    CHECK(c.d3d->CreateMaterial(&mat.p, NULL));
    D3DMATERIAL m;
    lit_material(&m);
    CHECK(mat->SetMaterial(&m));
    D3DMATERIALHANDLE h = 0;
    CHECK(mat->GetHandle(c.dev, &h));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_MATERIAL, h));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_AMBIENT, LIT_AMBIENT));

    Com<IDirect3DLight> light;
    CHECK(c.d3d->CreateLight(&light.p, NULL));
    D3DLIGHT2 l;
    lit_light(&l, sizeof(D3DLIGHT2), D3DLIGHT_ACTIVE);
    CHECK(light->SetLight((D3DLIGHT *)&l));
    CHECK(c.vp->AddLight(light));
    EXPECT_HR(c.vp->AddLight(light), D3DERR_LIGHTHASVIEWPORT);
    D3DLIGHT2 lg;
    memset(&lg, 0, sizeof(lg));
    lg.dwSize = sizeof(lg);
    CHECK(light->GetLight((D3DLIGHT *)&lg));
    EXPECT(!memcmp(&lg, &l, sizeof(lg)), "GetLight differs from SetLight (flags 0x%lx)", (unsigned long)lg.dwFlags);

    XN q[4];
    CHECK(c.dev->BeginScene());
    xn_quad(q, ndc_x(4), ndc_y(4), ndc_x(28), ndc_y(28), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XN, q, 4, 0));
    xn_quad(q, ndc_x(36), ndc_y(4), ndc_x(60), ndc_y(28), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XN, q, 4, D3DDP_DONOTLIGHT));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_MATERIAL, 0));
    xn_quad(q, ndc_x(4), ndc_y(36), ndc_x(28), ndc_y(60), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XN, q, 4, 0));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_MATERIAL, h));
    lit_light(&l, sizeof(D3DLIGHT2), 0);
    CHECK(light->SetLight((D3DLIGHT *)&l));
    xn_quad(q, ndc_x(36), ndc_y(36), ndc_x(60), ndc_y(60), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XN, q, 4, 0));
    CHECK(c.dev->EndScene());
    CHECK(c.vp->DeleteLight(light));

    printf("    expected: lit %06lx, light inactive %06lx\n", (unsigned long)lit_rgb(1), (unsigned long)lit_rgb(0));
    EXPECT_RECT(c.rt.p, 4, 4, 28, 28, lit_rgb(1), bg, 1);
    EXPECT_RECT(c.rt.p, 36, 4, 60, 28, 0xffffff, bg, 0);
    EXPECT_RECT(c.rt.p, 4, 36, 28, 60, 0xffffff, bg, 0);
    EXPECT_RECT(c.rt.p, 36, 36, 60, 60, lit_rgb(0), bg, 1);
    return 1;
}

/* TEXTUREMAPBLEND with IDirect3DTexture2 textures from the surfaces'
 * QueryInterface: MODULATE (the default) takes alpha from the texture when
 * it has alpha, else from the diffuse colour, and multiplies colours;
 * DECAL and COPY show the texel; ADD adds.  The stage states an
 * application reads back never follow TEXTUREMAPBLEND or the texture; a
 * stage state the application changes takes over until TEXTUREMAPBLEND
 * changes. */
static int draw_blend3(Ctx3 &c, DWORD top, DWORD bottom)
{
    TDT q[4];
    if (!clear3(c, 0x000000))
        return 0;
    CHECK(c.dev->BeginScene());
    tdt_quad(q, 0, 0, W, H / 2, top);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TDT, q, 4, 0));
    tdt_quad(q, 0, H / 2, W, H, bottom);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TDT, q, 4, 0));
    CHECK(c.dev->EndScene());
    return 1;
}

static int expect_blend3(Ctx3 &c, DWORD top, DWORD bottom, int tol, const char *what)
{
    Px p[] = {{4, 4, top}, {32, 16, top}, {59, 27, top}, {4, 36, bottom}, {32, 48, bottom}, {59, 59, bottom}};
    if (!expect_pixels(c.rt.p, p, (int)ARRAY_SIZE(p), tol)) {
        printf("    (%s)\n", what);
        return 0;
    }
    return 1;
}

static int stage_ops3(Ctx3 &c, DWORD colorop, DWORD alphaop, const char *what)
{
    DWORD co = 0, ao = 0;
    CHECK(c.dev->GetTextureStageState(0, D3DTSS_COLOROP, &co));
    CHECK(c.dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &ao));
    EXPECT(co == colorop && ao == alphaop, "%s: COLOROP %lu ALPHAOP %lu, want %lu %lu", what, (unsigned long)co,
           (unsigned long)ao, (unsigned long)colorop, (unsigned long)alphaop);
    return 1;
}

static int scene_blend3(void)
{
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    DWORD v = 0;
    CHECK(c.dev->GetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, &v));
    EXPECT(v == D3DTBLEND_MODULATE, "default TEXTUREMAPBLEND %lu", (unsigned long)v);
    if (!stage_ops3(c, D3DTOP_MODULATE, D3DTOP_SELECTARG1, "defaults"))
        return 0;

    Com<IDirectDrawSurface4> as, xs;
    Com<IDirect3DTexture2> at, xt;
    if (!texture4(c, &kA8R8G8B8, &as.p, &at.p) || !texture4(c, &kX8R8G8B8, &xs.p, &xt.p) ||
        !fill_texels(as.p, 32, 0x800000ffu) || !fill_texels(xs.p, 32, 0x000000ffu))
        return 0;

    /* src * a + dst * (1 - a) over black. */
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA));
    CHECK(c.dev->SetTexture(0, at));
    if (!draw_blend3(c, 0xffffffffu, 0x80ffffffu) ||
        !expect_blend3(c, 0x000080, 0x000080, 1, "MODULATE, A8R8G8B8 texel alpha 0x80"))
        return 0;
    CHECK(c.dev->SetTexture(0, xt));
    if (!draw_blend3(c, 0xffffffffu, 0x80ffffffu) ||
        !expect_blend3(c, 0x0000ff, 0x000080, 1, "MODULATE, X8R8G8B8: diffuse alpha"))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE));

    if (!fill_texels(xs.p, 32, 0x00ffff80u) || !draw_blend3(c, 0xffff0080u, 0xff8000ffu) ||
        !expect_blend3(c, 0xff0040, 0x800080, 1, "MODULATE colours"))
        return 0;
    static const struct {
        DWORD mode;
        const char *name;
    } exact[] = {{D3DTBLEND_DECAL, "DECAL"}, {D3DTBLEND_COPY, "COPY"}};
    for (size_t i = 0; i < ARRAY_SIZE(exact); ++i) {
        CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, exact[i].mode));
        if (!stage_ops3(c, D3DTOP_MODULATE, D3DTOP_SELECTARG1, exact[i].name) ||
            !draw_blend3(c, 0xffff0080u, 0xff8000ffu) || !expect_blend3(c, 0xffff80, 0xffff80, 0, exact[i].name))
            return 0;
    }
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_ADD));
    if (!stage_ops3(c, D3DTOP_MODULATE, D3DTOP_SELECTARG1, "ADD") || !fill_texels(xs.p, 32, 0x00204080u) ||
        !draw_blend3(c, 0xff402010u, 0xffff8040u) || !expect_blend3(c, 0x606090, 0xffc0c0, 0, "ADD"))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE));
    if (!stage_ops3(c, D3DTOP_MODULATE, D3DTOP_SELECTARG1, "MODULATE again") ||
        !draw_blend3(c, 0xffffffffu, 0xff00ff00u) || !expect_blend3(c, 0x204080, 0x004000, 1, "MODULATE again"))
        return 0;

    CHECK(c.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2));
    if (!stage_ops3(c, D3DTOP_SELECTARG2, D3DTOP_SELECTARG1, "COLOROP SELECTARG2") ||
        !draw_blend3(c, 0xffff0080u, 0xff8000ffu) ||
        !expect_blend3(c, 0xff0080, 0x8000ff, 0, "COLOROP SELECTARG2: the diffuse colour"))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE));
    if (!draw_blend3(c, 0xffff0080u, 0xff8000ffu) ||
        !expect_blend3(c, 0xff0080, 0x8000ff, 0, "TEXTUREMAPBLEND MODULATE again, unchanged"))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_DECAL));
    if (!stage_ops3(c, D3DTOP_SELECTARG2, D3DTOP_SELECTARG1, "DECAL after COLOROP") ||
        !draw_blend3(c, 0xffff0080u, 0xff8000ffu) ||
        !expect_blend3(c, 0x204080, 0x204080, 0, "DECAL after COLOROP: the texel"))
        return 0;
    CHECK(c.dev->SetTexture(0, NULL));
    return 1;
}

/* Texture handles on a Direct3D 3 device: IDirect3DTexture2::GetHandle
 * with the device's IDirect3DDevice2 (the same handle every time), bound
 * through TEXTUREHANDLE: GetRenderState returns the handle, GetTexture does
 * not report the texture, the texels draw; handle 0 unbinds. */
static int scene_handle3(void)
{
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    Com<IDirectDrawSurface4> s;
    Com<IDirect3DTexture2> t;
    DWORD raw[16], rgb[16];
    level_texels(&kX8R8G8B8, 0, raw, rgb);
    if (!texture4(c, &kX8R8G8B8, &s.p, &t.p) || !write_texels(s.p, 32, raw))
        return 0;
    Com<IDirect3DDevice2> dev2;
    CHECK(c.dev->QueryInterface(IID_IDirect3DDevice2, (void **)&dev2.p));
    D3DTEXTUREHANDLE h = 0, h2 = 0;
    CHECK(t->GetHandle(dev2, &h));
    CHECK(t->GetHandle(dev2, &h2));
    EXPECT(h && h == h2, "GetHandle gave 0x%lx, then 0x%lx", (unsigned long)h, (unsigned long)h2);
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, h));
    DWORD got = 0;
    CHECK(c.dev->GetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, &got));
    EXPECT(got == h, "GetRenderState(TEXTUREHANDLE) 0x%lx, want 0x%lx", (unsigned long)got, (unsigned long)h);
    /* The texture is bound, but GetTexture does not report it. */
    IDirect3DTexture2 *bound = (IDirect3DTexture2 *)0xdeadbeef;
    CHECK(c.dev->GetTexture(0, &bound));
    if (bound)
        bound->Release();
    EXPECT(!bound, "GetTexture(0) returned %p (the texture is %p)", (void *)bound, (void *)t.p);
    if (!draw_full_textured3(c, 0x000000, 0xffffffffu) || !expect_texels(c.rt.p, 0, 0, W, rgb, 0))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0));
    if (!draw_full_textured3(c, 0x000000, 0xffff3366u))
        return 0;
    Px p[] = {{8, 8, 0xff3366}, {56, 56, 0xff3366}, {32, 32, 0xff3366}};
    EXPECT_PIXELS(c.rt.p, p, 0);
    return 1;
}

/* IDirect3D3::CreateVertexBuffer: description, lock, DrawPrimitiveVB from
 * a start vertex, DrawIndexedPrimitiveVB, and ProcessVertices from an
 * untransformed buffer drawn later. */
static int scene_vb3(void)
{
    static const DWORD caps[2] = {0, D3DVBCAPS_SYSTEMMEMORY};
    static const DWORD colours[2] = {0x00ff00, 0x0000ff};
    const DWORD bg = 0x000000;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    WORD idx[6];
    for (int i = 0; i < 6; ++i)
        idx[i] = (WORD)(kQuadList[i] + 4);
    for (int i = 0; i < 2; ++i) {
        D3DVERTEXBUFFERDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwCaps = caps[i];
        d.dwFVF = FVF_TD;
        d.dwNumVertices = 8;
        Com<IDirect3DVertexBuffer> vb;
        CHECK(c.d3d->CreateVertexBuffer(&d, &vb.p, 0, NULL));
        D3DVERTEXBUFFERDESC g;
        memset(&g, 0xcc, sizeof(g));
        g.dwSize = sizeof(g);
        CHECK(vb->GetVertexBufferDesc(&g));
        EXPECT(g.dwCaps == caps[i] && g.dwFVF == FVF_TD && g.dwNumVertices == 8,
               "caps 0x%lx: GetVertexBufferDesc caps 0x%lx fvf 0x%lx count %lu", (unsigned long)caps[i],
               (unsigned long)g.dwCaps, (unsigned long)g.dwFVF, (unsigned long)g.dwNumVertices);
        void *data = NULL;
        DWORD size = 0;
        CHECK(vb->Lock(DDLOCK_WAIT, &data, &size));
        EXPECT(size == 8 * sizeof(TD), "Lock reported %lu bytes", (unsigned long)size);
        td_quad((TD *)data, 0, 0, W, H, 0.5f, 0xffff0000u); /* vertices 0-3 must not be drawn */
        td_quad((TD *)data + 4, 16, 16, 48, 48, 0.5f, 0xff000000u | colours[i]);
        CHECK(vb->Unlock());
        if (!clear3(c, bg))
            return 0;
        CHECK(c.dev->BeginScene());
        CHECK(c.dev->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, vb, 4, 4, 0));
        CHECK(c.dev->EndScene());
        if (!expect_rect(c.rt.p, 16, 16, 48, 48, colours[i], bg, 0)) {
            printf("    DrawPrimitiveVB, caps 0x%lx\n", (unsigned long)caps[i]);
            return 0;
        }
        if (!clear3(c, bg))
            return 0;
        CHECK(c.dev->BeginScene());
        CHECK(c.dev->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, vb, idx, 6, 0));
        CHECK(c.dev->EndScene());
        if (!expect_rect(c.rt.p, 16, 16, 48, 48, colours[i], bg, 0)) {
            printf("    DrawIndexedPrimitiveVB, caps 0x%lx\n", (unsigned long)caps[i]);
            return 0;
        }
    }

    D3DVERTEXBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFVF = FVF_XD;
    d.dwNumVertices = 4;
    Com<IDirect3DVertexBuffer> src, dst;
    CHECK(c.d3d->CreateVertexBuffer(&d, &src.p, 0, NULL));
    d.dwFVF = FVF_TD;
    CHECK(c.d3d->CreateVertexBuffer(&d, &dst.p, 0, NULL));
    void *data = NULL;
    CHECK(src->Lock(DDLOCK_WAIT, &data, NULL));
    xd_quad((XD *)data, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0xffffff00u);
    CHECK(src->Unlock());
    D3DMATRIX world = translation(0.25f, -0.25f, 0.0f);
    CHECK(c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &world));
    CHECK(dst->ProcessVertices(D3DVOP_TRANSFORM, 0, 4, src, 0, c.dev, 0));
    world = kIdentity;
    CHECK(c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &world));
    if (!clear3(c, bg))
        return 0;
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, dst, 0, 4, 0));
    CHECK(c.dev->EndScene());
    if (!expect_rect(c.rt.p, 24, 24, 56, 56, 0xffff00, bg, 0)) {
        printf("    ProcessVertices output\n");
        return 0;
    }
    return 1;
}

/* Begin / Vertex / End and BeginIndexed / Index / End, pre-transformed and
 * untransformed. */
static int scene_beginend3(void)
{
    const DWORD bg = 0x202020;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c) || !clear3(c, bg))
        return 0;
    TD a[4], b[4];
    XD x[4];
    td_quad(a, 4, 4, 28, 28, 0.5f, 0xffff0000u);
    td_quad(b, 36, 4, 60, 28, 0.5f, 0xff00ff00u);
    xd_quad(x, ndc_x(4), ndc_y(36), ndc_x(28), ndc_y(60), 0.5f, 0xff0000ffu);
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->Begin(D3DPT_TRIANGLESTRIP, FVF_TD, 0));
    for (int i = 0; i < 4; ++i)
        CHECK(c.dev->Vertex(&a[i]));
    CHECK(c.dev->End(0));
    CHECK(c.dev->BeginIndexed(D3DPT_TRIANGLELIST, FVF_TD, b, 4, 0));
    for (int i = 0; i < 6; ++i)
        CHECK(c.dev->Index(kQuadList[i]));
    CHECK(c.dev->End(0));
    CHECK(c.dev->Begin(D3DPT_TRIANGLELIST, FVF_XD, 0));
    for (int i = 0; i < 6; ++i)
        CHECK(c.dev->Vertex(&x[kQuadList[i]]));
    CHECK(c.dev->End(0));
    CHECK(c.dev->EndScene());
    EXPECT_RECT(c.rt.p, 4, 4, 28, 28, 0xff0000, bg, 0);
    EXPECT_RECT(c.rt.p, 36, 4, 60, 28, 0x00ff00, bg, 0);
    EXPECT_RECT(c.rt.p, 4, 36, 28, 60, 0x0000ff, bg, 0);
    Px rest[] = {{48, 48, bg}};
    EXPECT_PIXELS(c.rt.p, rest, 0);
    return 1;
}

/* Linear vertex fog through the light states (FOGMODE, FOGSTART, FOGEND)
 * on untransformed vertices, and linear table fog through the render states
 * on pre-transformed ones: f * colour + (1 - f) * fog colour (+-2). */
static int scene_fog3(void)
{
    const DWORD bg = 0x000000;
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c) || !clear3(c, bg))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_FOGCOLOR, 0x00ff0000));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_FOGMODE, D3DFOG_LINEAR));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_FOGSTART, fdw(0.0f)));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_FOGEND, fdw(1.0f)));
    XD x[4];
    xd_quad(x, ndc_x(4), ndc_y(4), ndc_x(28), ndc_y(60), 0.25f, 0xff0000ffu);
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_XD, x, 4, 0));
    CHECK(c.dev->EndScene());
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_FOGMODE, D3DFOG_NONE));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_LINEAR));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_FOGTABLESTART, fdw(0.0f)));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_FOGTABLEEND, fdw(1.0f)));
    CHECK(c.dev->BeginScene());
    CHECK(draw_td3(c.dev, 36, 4, 60, 60, 0.5f, 0xff0000ffu));
    CHECK(c.dev->EndScene());
    EXPECT_RECT(c.rt.p, 4, 4, 28, 60, 0x4000bf, bg, 2); /* f = 0.75 */
    EXPECT_RECT(c.rt.p, 36, 4, 60, 60, 0x800080, bg, 2); /* f = 0.5 */
    return 1;
}

/* COLORKEYENABLE with an R5G6B5 texture keyed before its first draw:
 * keyed texels leave the background; without a key, or with
 * COLORKEYENABLE off, every texel draws. */
static int scene_ckey3(void)
{
    const DWORD bg = 0x123456, magenta = 0xffff00ffu;
    const Fmt *f = &kR5G6B5;
    DWORD raw[16], keyed[16], unkeyed[16];
    Ctx3 c;
    if (!ctx3_init(c) || !base3(c))
        return 0;
    DWORD key = format_pack(f, magenta);
    for (int t = 0; t < 16; ++t) {
        int diagonal = t % 5 == 0;
        raw[t] = format_pack(f, diagonal ? magenta : level_colour(t)); /* level_colour(t < 16) is never magenta */
        unkeyed[t] = format_rgb(f, raw[t]);
        keyed[t] = diagonal ? bg : unkeyed[t];
    }
    Com<IDirectDrawSurface4> ps, ks;
    Com<IDirect3DTexture2> pt, kt;
    if (!texture4(c, f, &ps.p, &pt.p) || !texture4(c, f, &ks.p, &kt.p) || !write_texels(ps.p, 16, raw) ||
        !write_texels(ks.p, 16, raw))
        return 0;
    DDCOLORKEY ck;
    ck.dwColorSpaceLowValue = key;
    ck.dwColorSpaceHighValue = key;
    CHECK(ks->SetColorKey(DDCKEY_SRCBLT, &ck));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE));
    CHECK(c.dev->SetTexture(0, pt));
    if (!draw_full_textured3(c, bg, 0xffffffffu) || !expect_texels(c.rt.p, 0, 0, W, unkeyed, 0)) {
        printf("    (no key, COLORKEYENABLE)\n");
        return 0;
    }
    CHECK(c.dev->SetTexture(0, kt));
    if (!draw_full_textured3(c, bg, 0xffffffffu) || !expect_texels(c.rt.p, 0, 0, W, keyed, 0)) {
        printf("    (key %04lx, COLORKEYENABLE)\n", (unsigned long)key);
        return 0;
    }
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE));
    if (!draw_full_textured3(c, bg, 0xffffffffu) || !expect_texels(c.rt.p, 0, 0, W, unkeyed, 0)) {
        printf("    (key %04lx, COLORKEYENABLE FALSE)\n", (unsigned long)key);
        return 0;
    }
    CHECK(c.dev->SetTexture(0, NULL));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Direct3D 2: IDirectDraw, IDirect3D2, IDirect3DDevice2, IDirect3DViewport2 */

struct Ctx2 {
    Com<IDirectDraw> dd;
    Com<IDirect3D2> d3d;
    Com<IDirectDrawSurface> rt;
    Com<IDirect3DDevice2> dev;
    Com<IDirect3DViewport2> vp;
    Com<IDirect3DMaterial2> bg; /* the viewport background */
    ~Ctx2()
    {
        if (dev.p && vp.p)
            dev->DeleteViewport(vp);
    }
};

/* A D3DVIEWPORT at (x, y), w x h, scale w/2, h/2: -1..1 covers it. */
static int set_viewport1(IDirect3DViewport *vp, DWORD x, DWORD y, DWORD w, DWORD h)
{
    D3DVIEWPORT v;
    memset(&v, 0, sizeof(v));
    v.dwSize = sizeof(v);
    v.dwX = x;
    v.dwY = y;
    v.dwWidth = w;
    v.dwHeight = h;
    v.dvScaleX = w / 2.0f;
    v.dvScaleY = h / 2.0f;
    v.dvMaxX = 1.0f;
    v.dvMaxY = 1.0f;
    v.dvMinZ = 0.0f;
    v.dvMaxZ = 1.0f;
    CHECK(vp->SetViewport(&v));
    return 1;
}

static int ctx2_init(Ctx2 &c)
{
    static int printed;
    CHECK(DirectDrawCreate(NULL, &c.dd.p, NULL));
    CHECK(c.dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL));
    CHECK(c.dd->QueryInterface(IID_IDirect3D2, (void **)&c.d3d.p));
    CHECK(create_surface1(c.dd, RT_CAPS, W, H, &kX8R8G8B8, &c.rt.p));
    if (!attach_z1(c.dd, c.rt))
        return 0;
    CHECK(c.d3d->CreateDevice(IID_IDirect3DHALDevice, c.rt, &c.dev.p));
    CHECK(c.d3d->CreateViewport(&c.vp.p, NULL));
    CHECK(c.dev->AddViewport(c.vp));
    if (!set_viewport1(c.vp, 0, 0, W, H))
        return 0;
    CHECK(c.dev->SetCurrentViewport(c.vp));
    CHECK(c.d3d->CreateMaterial(&c.bg.p, NULL));
    D3DMATERIALHANDLE h = 0;
    if (!set_diffuse_material(c.bg.p, 0.0f, 0.0f, 0.0f))
        return 0;
    CHECK(c.bg->GetHandle(c.dev, &h));
    CHECK(c.vp->SetBackground(h));
    if (!printed++)
        print_module("    IDirect3DDevice2 object from", object_module(c.dev));
    return 1;
}

static int base2(Ctx2 &c)
{
    if (!set_states(c.dev.p, kBaseStates, ARRAY_SIZE(kBaseStates)))
        return 0;
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0));
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE));
    return reset_transforms_and_lights(c.dev.p);
}

/* Viewport::Clear of target and z with the background material, colour
 * components 0 or 1. */
template <class C> static int clear_bg(C &c, float r, float g, float b)
{
    if (!set_diffuse_material(c.bg.p, r, g, b))
        return 0;
    D3DRECT rect;
    set_d3drect(&rect, 0, 0, W, H);
    CHECK(c.vp->Clear(1, &rect, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER));
    return 1;
}

static int scene_enum2(void)
{
    static const DeviceWant want[] = {
        {&IID_IDirect3DRampDevice, "Ramp Emulation"},
        {&IID_IDirect3DRGBDevice, "RGB Emulation"},
        {&IID_IDirect3DHALDevice, "Direct3D HAL"},
        {&IID_IDirect3DMMXDevice, "MMX Emulation"},
    };
    Ctx2 c;
    if (!ctx2_init(c))
        return 0;
    if (!check_devices(c.d3d.p, want, ARRAY_SIZE(want)) ||
        !check_find_device(c.d3d.p, TRUE, IID_IDirect3DHALDevice))
        return 0;
    FormatList t;
    t.n = 0;
    CHECK(c.dev->EnumTextureFormats(collect_format_desc, &t));
    for (int i = 0; i < t.n; ++i)
        print_format("texfmt2", &t.f[i]);
    EXPECT(listed(&t, &kX8R8G8B8) && listed(&t, &kR5G6B5), "X8R8G8B8 %d, R5G6B5 %d among %d texture formats",
           listed(&t, &kX8R8G8B8), listed(&t, &kR5G6B5), t.n);
    return 1;
}

/* What fresh Direct3D 3 and Direct3D 2 HAL devices report, as the stock
 * runtime has it: render states 1-60, the stipple patterns 64-95 and
 * WRAP0-7 (all accepted, all 0; the states only later versions define are
 * left out), light states, Direct3D 3 texture stage states 1-23 of stages 0
 * and 1, identity transforms.  The two versions differ in
 * TEXTUREPERSPECTIVE (off in Direct3D 2) and SPECULARENABLE (on in
 * Direct3D 2); Direct3D 2 has no D3DLIGHTSTATE_COLORVERTEX. */
static const RS kRsDefaults3[] = {
    {D3DRENDERSTATE_TEXTUREHANDLE, 0},
    {D3DRENDERSTATE_ANTIALIAS, D3DANTIALIAS_NONE},
    {D3DRENDERSTATE_TEXTUREADDRESS, D3DTADDRESS_WRAP},
    {D3DRENDERSTATE_TEXTUREPERSPECTIVE, TRUE},
    {D3DRENDERSTATE_WRAPU, FALSE},
    {D3DRENDERSTATE_WRAPV, FALSE},
    {D3DRENDERSTATE_ZENABLE, D3DZB_TRUE},
    {D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID},
    {D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD},
    {D3DRENDERSTATE_LINEPATTERN, 0},
    {D3DRENDERSTATE_MONOENABLE, FALSE},
    {D3DRENDERSTATE_ROP2, 0x0d /* R2_COPYPEN */},
    {D3DRENDERSTATE_PLANEMASK, 0xffffffff},
    {D3DRENDERSTATE_ZWRITEENABLE, TRUE},
    {D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
    {D3DRENDERSTATE_LASTPIXEL, TRUE},
    {D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_NEAREST},
    {D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_NEAREST},
    {D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE},
    {D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO},
    {D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE},
    {D3DRENDERSTATE_CULLMODE, D3DCULL_CCW},
    {D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL},
    {D3DRENDERSTATE_ALPHAREF, 0},
    {D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS},
    {D3DRENDERSTATE_DITHERENABLE, FALSE},
    {D3DRENDERSTATE_ALPHABLENDENABLE, FALSE},
    {D3DRENDERSTATE_FOGENABLE, FALSE},
    {D3DRENDERSTATE_SPECULARENABLE, FALSE},
    {D3DRENDERSTATE_ZVISIBLE, FALSE},
    {D3DRENDERSTATE_SUBPIXEL, FALSE},
    {D3DRENDERSTATE_SUBPIXELX, FALSE},
    {D3DRENDERSTATE_STIPPLEDALPHA, FALSE},
    {D3DRENDERSTATE_FOGCOLOR, 0},
    {D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE},
    {D3DRENDERSTATE_FOGTABLESTART, 0 /* 0.0f */},
    {D3DRENDERSTATE_FOGTABLEEND, 0x3f800000 /* 1.0f */},
    {D3DRENDERSTATE_FOGTABLEDENSITY, 0x3f800000 /* 1.0f */},
    {D3DRENDERSTATE_STIPPLEENABLE, FALSE},
    {D3DRENDERSTATE_EDGEANTIALIAS, FALSE},
    {D3DRENDERSTATE_COLORKEYENABLE, FALSE},
    {(D3DRENDERSTATETYPE)42, 0xffffffff}, /* DirectX 5's ALPHABLENDENABLE */
    {D3DRENDERSTATE_BORDERCOLOR, 0},
    {D3DRENDERSTATE_TEXTUREADDRESSU, D3DTADDRESS_WRAP},
    {D3DRENDERSTATE_TEXTUREADDRESSV, D3DTADDRESS_WRAP},
    {D3DRENDERSTATE_MIPMAPLODBIAS, 0},
    {D3DRENDERSTATE_ZBIAS, 0},
    {D3DRENDERSTATE_RANGEFOGENABLE, FALSE},
    {D3DRENDERSTATE_ANISOTROPY, 1},
    {D3DRENDERSTATE_FLUSHBATCH, 0xffffffff},
    {D3DRENDERSTATE_TRANSLUCENTSORTINDEPENDENT, 0xffffffff},
    {D3DRENDERSTATE_STENCILENABLE, FALSE},
    {D3DRENDERSTATE_STENCILFAIL, D3DSTENCILOP_KEEP},
    {D3DRENDERSTATE_STENCILZFAIL, D3DSTENCILOP_KEEP},
    {D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_KEEP},
    {D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS},
    {D3DRENDERSTATE_STENCILREF, 0},
    {D3DRENDERSTATE_STENCILMASK, 0xffffffff},
    {D3DRENDERSTATE_STENCILWRITEMASK, 0xffffffff},
    {D3DRENDERSTATE_TEXTUREFACTOR, 0xffffffff},
};
static_assert(ARRAY_SIZE(kRsDefaults3) == 60, "render states 1-60, in order");

/* D3DTSS_COLOROP (1) to D3DTSS_BUMPENVLOFFSET (23) of stage 0; stage 1 has
 * D3DTOP_DISABLE in both ops. */
static const DWORD kTssDefaults3[23] = {
    D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT,                        /* COLOROP, COLORARG1, 2 */
    D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT,                      /* ALPHAOP, ALPHAARG1, 2 */
    0, 0, 0, 0,                                                           /* BUMPENVMAT00-11 */
    0,                                                                    /* TEXCOORDINDEX */
    D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, 0,              /* ADDRESS, U, V, BORDERCOLOR */
    D3DTFG_POINT, D3DTFN_POINT, D3DTFP_NONE, 0, 0, 1,                     /* filters, LOD bias, levels, aniso */
    0, 0,                                                                 /* BUMPENVLSCALE, LOFFSET */
};

/* Render states (table: states 1-60), transforms. */
template <class D> static int check_rs_defaults(D *dev, const RS *table, const char *what)
{
    int ok = 1;
    for (DWORD s = 1; s <= 135; ++s) {
        if ((s > 60 && s < 64) || (s > 95 && s < 128))
            continue;
        if (s <= 60 && (DWORD)table[s - 1].state != s) {
            printf("    defaults table entry %lu is state %d\n", (unsigned long)s, (int)table[s - 1].state);
            return 0;
        }
        DWORD v = 0xdeadbeef, w = s <= 60 ? table[s - 1].value : 0;
        HRESULT hr = dev->GetRenderState((D3DRENDERSTATETYPE)s, &v);
        if (hr != D3D_OK || v != w) {
            printf("    %s render state %lu: hr=0x%08lx value 0x%08lx, want 0x%08lx\n", what, (unsigned long)s,
                   (unsigned long)hr, (unsigned long)v, (unsigned long)w);
            ok = 0;
        }
    }
    D3DMATRIX m;
    for (int t = D3DTRANSFORMSTATE_WORLD; t <= D3DTRANSFORMSTATE_PROJECTION; ++t) {
        memset(&m, 0xcc, sizeof(m));
        CHECK(dev->GetTransform((D3DTRANSFORMSTATETYPE)t, &m));
        if (memcmp(&m, &kIdentity, sizeof(m))) {
            printf("    %s transform %d is not identity\n", what, t);
            ok = 0;
        }
    }
    return ok;
}

/* Light states up to `last` are 0 (COLORVERTEX TRUE); later ones fail with
 * DDERR_INVALIDPARAMS and leave the output alone. */
template <class D> static int check_ls_defaults(D *dev, DWORD last, const char *what)
{
    int ok = 1;
    for (DWORD s = D3DLIGHTSTATE_MATERIAL; s <= D3DLIGHTSTATE_COLORVERTEX; ++s) {
        DWORD v = 0xdeadbeef, w = s > last ? 0xdeadbeef : s == D3DLIGHTSTATE_COLORVERTEX ? TRUE : 0;
        HRESULT hr = dev->GetLightState((D3DLIGHTSTATETYPE)s, &v);
        HRESULT want_hr = s > last ? DDERR_INVALIDPARAMS : D3D_OK;
        if (hr != want_hr || v != w) {
            printf("    %s light state %lu: hr=0x%08lx value 0x%08lx, want hr=0x%08lx value 0x%08lx\n", what,
                   (unsigned long)s, (unsigned long)hr, (unsigned long)v, (unsigned long)want_hr, (unsigned long)w);
            ok = 0;
        }
    }
    return ok;
}

static int scene_defaults(void)
{
    Ctx3 c3;
    Ctx2 c2;
    if (!ctx3_init(c3) || !ctx2_init(c2))
        return 0;
    int ok = check_rs_defaults(c3.dev.p, kRsDefaults3, "Direct3D 3");
    ok = check_ls_defaults(c3.dev.p, D3DLIGHTSTATE_COLORVERTEX, "Direct3D 3") && ok;
    for (DWORD stage = 0; stage < 2; ++stage)
        for (DWORD t = 1; t <= 23; ++t) {
            DWORD v = 0xdeadbeef, w = kTssDefaults3[t - 1];
            if (stage && (t == D3DTSS_COLOROP || t == D3DTSS_ALPHAOP))
                w = D3DTOP_DISABLE;
            HRESULT hr = c3.dev->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)t, &v);
            if (hr != D3D_OK || v != w) {
                printf("    Direct3D 3 stage %lu state %lu: hr=0x%08lx value 0x%08lx, want 0x%08lx\n",
                       (unsigned long)stage, (unsigned long)t, (unsigned long)hr, (unsigned long)v,
                       (unsigned long)w);
                ok = 0;
            }
        }
    RS rs2[ARRAY_SIZE(kRsDefaults3)];
    memcpy(rs2, kRsDefaults3, sizeof(rs2));
    rs2[D3DRENDERSTATE_TEXTUREPERSPECTIVE - 1].value = FALSE;
    rs2[D3DRENDERSTATE_SPECULARENABLE - 1].value = TRUE;
    ok = check_rs_defaults(c2.dev.p, rs2, "Direct3D 2") && ok;
    return check_ls_defaults(c2.dev.p, D3DLIGHTSTATE_FOGDENSITY, "Direct3D 2") && ok;
}

/* DrawPrimitive with the three vertex types: D3DVT_TLVERTEX as given,
 * D3DVT_LVERTEX transformed through a D3DVIEWPORT (scale w/2, h/2) at an
 * offset, D3DVT_VERTEX lit by a light and material (or unlit, white, with
 * D3DDP_DONOTLIGHT). */
static int scene_vt2(void)
{
    const DWORD bg = 0x000000;
    Ctx2 c;
    if (!ctx2_init(c) || !base2(c) || !clear_bg(c, 0.0f, 0.0f, 0.0f))
        return 0;

    TLV t[4];
    tlv_quad(t, 4, 4, 20, 20, 0.5f, 0xffff0000u, 0);
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_TLVERTEX, t, 4, 0));
    CHECK(c.dev->EndScene());

    if (!set_viewport1(c.vp, 32, 0, 32, 32))
        return 0;
    LVX l[4];
    lvx_quad(l, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0xff00ff00u);
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_LVERTEX, l, 4, 0));
    CHECK(c.dev->EndScene());
    if (!set_viewport1(c.vp, 0, 0, W, H))
        return 0;

    Com<IDirect3DMaterial2> mat;
    CHECK(c.d3d->CreateMaterial(&mat.p, NULL));
    D3DMATERIAL m;
    lit_material(&m);
    CHECK(mat->SetMaterial(&m));
    D3DMATERIALHANDLE h = 0;
    CHECK(mat->GetHandle(c.dev, &h));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_MATERIAL, h));
    CHECK(c.dev->SetLightState(D3DLIGHTSTATE_AMBIENT, LIT_AMBIENT));
    Com<IDirect3DLight> light;
    CHECK(c.d3d->CreateLight(&light.p, NULL));
    D3DLIGHT2 ld;
    lit_light(&ld, sizeof(D3DLIGHT2), D3DLIGHT_ACTIVE);
    CHECK(light->SetLight((D3DLIGHT *)&ld));
    CHECK(c.vp->AddLight(light));
    VX x[4];
    CHECK(c.dev->BeginScene());
    vx_quad(x, ndc_x(4), ndc_y(36), ndc_x(28), ndc_y(60), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_VERTEX, x, 4, 0));
    vx_quad(x, ndc_x(36), ndc_y(36), ndc_x(60), ndc_y(60), 0.5f);
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_VERTEX, x, 4, D3DDP_DONOTLIGHT));
    CHECK(c.dev->EndScene());
    CHECK(c.vp->DeleteLight(light));

    EXPECT_RECT(c.rt.p, 4, 4, 20, 20, 0xff0000, bg, 0);
    EXPECT_RECT(c.rt.p, 40, 8, 56, 24, 0x00ff00, bg, 0);
    EXPECT_RECT(c.rt.p, 4, 36, 28, 60, lit_rgb(1), bg, 1);
    EXPECT_RECT(c.rt.p, 36, 36, 60, 60, 0xffffff, bg, 0);
    return 1;
}

/* A texture handle from IDirect3DTexture2::GetHandle in TEXTUREHANDLE:
 * MODULATE multiplies the diffuse colour in (+-1), COPY shows the texel. */
static int scene_tex2(void)
{
    Ctx2 c;
    if (!ctx2_init(c) || !base2(c) || !clear_bg(c, 0.0f, 0.0f, 0.0f))
        return 0;
    DWORD raw[16], rgb[16], half_green[16];
    level_texels(&kX8R8G8B8, 5, raw, rgb);
    for (int t = 0; t < 16; ++t) {
        DWORD g = (rgb[t] >> 8) & 0xff;
        half_green[t] = (rgb[t] & 0xff00ff) | ((g * 0x80 + 127) / 255) << 8;
    }
    Com<IDirectDrawSurface> s;
    CHECK(create_surface1(c.dd, DDSCAPS_TEXTURE, 4, 4, &kX8R8G8B8, &s.p));
    if (!write_texels(s.p, 32, raw))
        return 0;
    Com<IDirect3DTexture2> t;
    CHECK(s->QueryInterface(IID_IDirect3DTexture2, (void **)&t.p));
    D3DTEXTUREHANDLE h = 0;
    CHECK(t->GetHandle(c.dev, &h));
    EXPECT(h != 0, "texture handle 0");
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, h));
    TLV q[4];
    tlv_quad(q, 0, 0, W, H, 0.5f, 0xffff80ffu, 0);
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_TLVERTEX, q, 4, 0));
    CHECK(c.dev->EndScene());
    if (!expect_texels(c.rt.p, 0, 0, W, half_green, 1)) {
        printf("    (MODULATE with diffuse ff80ff)\n");
        return 0;
    }
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_COPY));
    tlv_quad(q, 0, 0, W, H, 0.5f, 0xff123456u, 0);
    if (!clear_bg(c, 0.0f, 0.0f, 0.0f))
        return 0;
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_TLVERTEX, q, 4, 0));
    CHECK(c.dev->EndScene());
    if (!expect_texels(c.rt.p, 0, 0, W, rgb, 0)) {
        printf("    (COPY)\n");
        return 0;
    }
    CHECK(c.dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0));
    return 1;
}

/* The palette index at (x, y) of an 8-bit surface. */
static int read_index(IDirectDrawSurface *s, int x, int y, BYTE *out)
{
    DDSURFACEDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL));
    *out = ((const BYTE *)d.lpSurface)[y * d.lPitch + x];
    CHECK(s->Unlock(NULL));
    return 1;
}

/* A P8 render target with a palette: the software devices render into it,
 * the HAL refuses it (CreateDevice on system memory, SetRenderTarget).
 * Stock Windows 11 writes indices that do not depend on the palette
 * (measured: black 0xff and white 0x28 under a 3-3-2 and a grey palette
 * alike), so the rendered indices are only checked with --expect-local,
 * where QD3D maps every colour to its nearest palette entry. */
static int scene_p8rt2(void)
{
    static const Fmt kP8 = {"P8", DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0};
    Com<IDirectDraw> dd;
    Com<IDirect3D2> d3d;
    CHECK(DirectDrawCreate(NULL, &dd.p, NULL));
    CHECK(dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL));
    CHECK(dd->QueryInterface(IID_IDirect3D2, (void **)&d3d.p));
    PALETTEENTRY pe[256]; /* 3-3-2 */
    for (int i = 0; i < 256; ++i) {
        pe[i].peRed = (BYTE)(i & 0xe0);
        pe[i].peGreen = (BYTE)((i & 0x1c) << 3);
        pe[i].peBlue = (BYTE)((i & 0x03) << 6);
        pe[i].peFlags = 0;
    }
    Com<IDirectDrawPalette> pal;
    CHECK(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal.p, NULL));
    Com<IDirectDrawSurface> p8;
    CHECK(create_surface1(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY, W, H, &kP8, &p8.p));

    Com<IDirect3DDevice2> dev;
    EXPECT_HR(d3d->CreateDevice(IID_IDirect3DRGBDevice, p8, &dev.p), DDERR_NOPALETTEATTACHED);
    CHECK(p8->SetPalette(pal));
    EXPECT_HR(d3d->CreateDevice(IID_IDirect3DHALDevice, p8, &dev.p), D3DERR_SURFACENOTINVIDMEM);

    Com<IDirectDrawSurface> rt;
    CHECK(create_surface1(dd, RT_CAPS, W, H, &kX8R8G8B8, &rt.p));
    Com<IDirect3DDevice2> hal;
    CHECK(d3d->CreateDevice(IID_IDirect3DHALDevice, rt, &hal.p));
    EXPECT_HR(hal->SetRenderTarget(p8, 0), DDERR_INVALIDCAPS);
    Com<IDirectDrawSurface> target;
    CHECK(hal->GetRenderTarget(&target.p));
    EXPECT(target.p == rt.p, "the HAL device's target changed to %p", (void *)target.p);

    CHECK(d3d->CreateDevice(IID_IDirect3DRGBDevice, p8, &dev.p));
    Com<IDirect3DViewport2> vp;
    CHECK(d3d->CreateViewport(&vp.p, NULL));
    CHECK(dev->AddViewport(vp));
    if (!set_viewport1(vp, 0, 0, W, H))
        return 0;
    CHECK(dev->SetCurrentViewport(vp));
    Com<IDirect3DMaterial2> bg;
    CHECK(d3d->CreateMaterial(&bg.p, NULL));
    if (!set_diffuse_material(bg.p, 0.0f, 0.0f, 1.0f))
        return 0;
    D3DMATERIALHANDLE h = 0;
    CHECK(bg->GetHandle(dev, &h));
    CHECK(vp->SetBackground(h));
    CHECK(dev->SetRenderState(D3DRENDERSTATE_DITHERENABLE, FALSE));
    D3DRECT rect;
    set_d3drect(&rect, 0, 0, W, H);
    EXPECT_HR(vp->Clear(1, &rect, D3DCLEAR_TARGET), D3D_OK);
    TLV t[4];
    tlv_quad(t, 0, 0, W / 2, H / 2, 0.5f, 0xffff0000u, 0);
    CHECK(dev->BeginScene());
    EXPECT_HR(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DVT_TLVERTEX, t, 4, 0), D3D_OK);
    CHECK(dev->EndScene());
    BYTE in = 0, out = 0;
    if (!read_index(p8, 8, 8, &in) || !read_index(p8, 48, 48, &out))
        return 0;
    printf("    rendered indices: quad 0x%02x, background 0x%02x\n", in, out);
    if (g_expect_local) {
        EXPECT(in == 0xe0, "red drew index 0x%02x, want 0xe0 (224,0,0)", in);
        EXPECT(out == 0x03, "the blue background is index 0x%02x, want 0x03 (0,0,192)", out);
    }
    CHECK(dev->DeleteViewport(vp));

    /* A software device also takes it as a new target. */
    Com<IDirect3DDevice2> rgb;
    CHECK(d3d->CreateDevice(IID_IDirect3DRGBDevice, rt, &rgb.p));
    EXPECT_HR(rgb->SetRenderTarget(p8, 0), D3D_OK);
    Com<IDirectDrawSurface> now;
    CHECK(rgb->GetRenderTarget(&now.p));
    EXPECT(now.p == p8.p, "GetRenderTarget gave %p, want the P8 surface %p", (void *)now.p, (void *)p8.p);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Direct3D 1: IDirect3D, IDirect3DDevice from the render target's
 * QueryInterface, execute buffers                                           */

struct Ctx1 {
    Com<IDirectDraw> dd;
    Com<IDirect3D> d3d;
    Com<IDirectDrawSurface> rt;
    Com<IDirect3DDevice> dev;
    Com<IDirect3DViewport> vp;
    Com<IDirect3DMaterial> bg; /* the viewport background */
    D3DMATRIXHANDLE world, view, proj;
    Ctx1() : world(0), view(0), proj(0) {}
    ~Ctx1()
    {
        if (!dev.p)
            return;
        if (world)
            dev->DeleteMatrix(world);
        if (view)
            dev->DeleteMatrix(view);
        if (proj)
            dev->DeleteMatrix(proj);
        if (vp.p)
            dev->DeleteViewport(vp);
    }
};

static int ctx1_init(Ctx1 &c)
{
    static int printed;
    CHECK(DirectDrawCreate(NULL, &c.dd.p, NULL));
    CHECK(c.dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL));
    CHECK(c.dd->QueryInterface(IID_IDirect3D, (void **)&c.d3d.p));
    CHECK(create_surface1(c.dd, RT_CAPS, W, H, &kX8R8G8B8, &c.rt.p));
    if (!attach_z1(c.dd, c.rt))
        return 0;
    CHECK(c.rt->QueryInterface(IID_IDirect3DHALDevice, (void **)&c.dev.p));
    CHECK(c.d3d->CreateViewport(&c.vp.p, NULL));
    CHECK(c.dev->AddViewport(c.vp));
    if (!set_viewport1(c.vp, 0, 0, W, H))
        return 0;
    CHECK(c.d3d->CreateMaterial(&c.bg.p, NULL));
    D3DMATERIALHANDLE h = 0;
    if (!set_diffuse_material(c.bg.p, 0.0f, 0.0f, 0.0f))
        return 0;
    CHECK(c.bg->GetHandle(c.dev, &h));
    CHECK(c.vp->SetBackground(h));
    D3DMATRIX m = kIdentity;
    CHECK(c.dev->CreateMatrix(&c.world));
    CHECK(c.dev->CreateMatrix(&c.view));
    CHECK(c.dev->CreateMatrix(&c.proj));
    CHECK(c.dev->SetMatrix(c.world, &m));
    CHECK(c.dev->SetMatrix(c.view, &m));
    CHECK(c.dev->SetMatrix(c.proj, &m));
    if (!printed++)
        print_module("    IDirect3DDevice object from", object_module(c.dev));
    return 1;
}

/* Interfaces exist where stock has them: the legacy Direct3D interfaces on
 * DirectDrawCreate objects only, IDirect3D7 on DirectDrawCreateEx objects
 * only; an IDirect3DDevice3 also answers as IDirect3DDevice2 and
 * IDirect3DDevice but not as a surface, while the Direct3D 1 device, which
 * the render target's QueryInterface made, answers as the surface. */
static int qi_check(IUnknown *u, const GUID &iid, const char *name, HRESULT want)
{
    IUnknown *out = (IUnknown *)0xdeadbeef;
    HRESULT hr = u->QueryInterface(iid, (void **)&out);
    if (SUCCEEDED(hr) && out)
        out->Release();
    if (hr != want) {
        printf("    QueryInterface(%s) returned 0x%08lx, want 0x%08lx\n", name, (unsigned long)hr,
               (unsigned long)want);
        return 0;
    }
    if (FAILED(hr) && out) {
        printf("    the failed QueryInterface(%s) left %p\n", name, (void *)out);
        return 0;
    }
    return 1;
}

#define QI(obj, iid, want) qi_check((IUnknown *)(obj), iid, #iid, want)

static int scene_qi(void)
{
    Com<IDirectDraw> dd;
    CHECK(DirectDrawCreate(NULL, &dd.p, NULL));
    if (!QI(dd.p, IID_IDirect3D, S_OK) || !QI(dd.p, IID_IDirect3D2, S_OK) || !QI(dd.p, IID_IDirect3D3, S_OK) ||
        !QI(dd.p, IID_IDirect3D7, E_NOINTERFACE) || !QI(dd.p, IID_IDirectDraw4, S_OK) ||
        !QI(dd.p, IID_IDirectDraw7, S_OK))
        return 0;
    Com<IDirect3D3> d3d3;
    CHECK(dd->QueryInterface(IID_IDirect3D3, (void **)&d3d3.p));
    if (!QI(d3d3.p, IID_IDirect3D2, S_OK) || !QI(d3d3.p, IID_IDirect3D, S_OK) ||
        !QI(d3d3.p, IID_IDirectDraw, S_OK) || !QI(d3d3.p, IID_IDirectDraw4, S_OK))
        return 0;

    Com<IDirectDraw7> dd7;
    CHECK(DirectDrawCreateEx(NULL, (void **)&dd7.p, IID_IDirectDraw7, NULL));
    if (!QI(dd7.p, IID_IDirect3D, E_NOINTERFACE) || !QI(dd7.p, IID_IDirect3D2, E_NOINTERFACE) ||
        !QI(dd7.p, IID_IDirect3D3, E_NOINTERFACE) || !QI(dd7.p, IID_IDirect3D7, S_OK))
        return 0;

    Ctx3 c;
    if (!ctx3_init(c))
        return 0;
    if (!QI(c.dev.p, IID_IDirect3DDevice2, S_OK) || !QI(c.dev.p, IID_IDirect3DDevice, S_OK) ||
        !QI(c.dev.p, IID_IDirect3DDevice7, E_NOINTERFACE) || !QI(c.dev.p, IID_IDirectDrawSurface4, E_NOINTERFACE) ||
        !QI(c.dev.p, IID_IDirectDrawSurface, E_NOINTERFACE))
        return 0;
    Ctx1 c1;
    if (!ctx1_init(c1))
        return 0;
    /* The stock runtime answers E_INVALIDARG (not E_NOINTERFACE) here. */
    if (!QI(c1.dev.p, IID_IDirectDrawSurface, S_OK) || !QI(c1.dev.p, IID_IDirect3DDevice2, E_INVALIDARG) ||
        !QI(c1.dev.p, IID_IDirect3DDevice3, E_INVALIDARG))
        return 0;
    return 1;
}

/* An execute buffer instruction stream. */
struct Exec {
    BYTE buf[2048];
    BYTE *p;
    Exec() : p(buf) {}
    void op(BYTE opcode, BYTE size, WORD count)
    {
        D3DINSTRUCTION *i = (D3DINSTRUCTION *)p;
        i->bOpcode = opcode;
        i->bSize = size;
        i->wCount = count;
        p += sizeof(*i);
    }
    void state(BYTE opcode, DWORD type, DWORD value)
    {
        op(opcode, sizeof(D3DSTATE), 1);
        DWORD s[2] = {type, value}; /* D3DSTATE */
        memcpy(p, s, sizeof(s));
        p += sizeof(D3DSTATE);
    }
    void rs(D3DRENDERSTATETYPE s, DWORD v) { state(D3DOP_STATERENDER, s, v); }
    void ls(D3DLIGHTSTATETYPE s, DWORD v) { state(D3DOP_STATELIGHT, s, v); }
    void ts(D3DTRANSFORMSTATETYPE s, DWORD v) { state(D3DOP_STATETRANSFORM, s, v); }
    void process(DWORD flags, WORD start, WORD dest, DWORD count)
    {
        op(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
        D3DPROCESSVERTICES *pv = (D3DPROCESSVERTICES *)p;
        pv->dwFlags = flags;
        pv->wStart = start;
        pv->wDest = dest;
        pv->dwCount = count;
        pv->dwReserved = 0;
        p += sizeof(*pv);
    }
    /* Triangle data QWORD aligned, as the DirectX SDK's d3dmacs.h did it:
     * an empty D3DOP_TRIANGLE as padding. */
    void triangles(const WORD *v, int n)
    {
        if (!((p - buf) & 7))
            op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 0);
        op(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), (WORD)n);
        for (int i = 0; i < n; ++i) {
            D3DTRIANGLE *t = (D3DTRIANGLE *)p;
            t->v1 = v[3 * i];
            t->v2 = v[3 * i + 1];
            t->v3 = v[3 * i + 2];
            t->wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            p += sizeof(*t);
        }
    }
    void quad(WORD base)
    {
        WORD v[6];
        for (int i = 0; i < 6; ++i)
            v[i] = (WORD)(kQuadList[i] + base);
        triangles(v, 2);
    }
    void exit() { op(D3DOP_EXIT, 0, 0); }
    DWORD length() const { return (DWORD)(p - buf); }
};

/* The base states through the instruction stream (an IDirect3DDevice has
 * no SetRenderState). */
static void emit_base1(Exec &e, const Ctx1 &c)
{
    for (size_t i = 0; i < ARRAY_SIZE(kBaseStates); ++i)
        e.rs(kBaseStates[i].state, kBaseStates[i].value);
    e.rs(D3DRENDERSTATE_TEXTUREHANDLE, 0);
    e.rs(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE);
    e.ls(D3DLIGHTSTATE_MATERIAL, 0);
    e.ls(D3DLIGHTSTATE_AMBIENT, 0);
    e.ts(D3DTRANSFORMSTATE_WORLD, c.world);
    e.ts(D3DTRANSFORMSTATE_VIEW, c.view);
    e.ts(D3DTRANSFORMSTATE_PROJECTION, c.proj);
}

/* Vertices at the start of a new execute buffer, the instructions after
 * them; SetExecuteData; Execute(D3DEXECUTE_CLIPPED) in a scene. */
static int run_exec(Ctx1 &c, const V32 *vertices, DWORD count, const Exec &e)
{
    D3DEXECUTEBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = D3DDEB_BUFSIZE | D3DDEB_CAPS;
    d.dwCaps = D3DDEBCAPS_SYSTEMMEMORY;
    d.dwBufferSize = count * sizeof(V32) + e.length();
    Com<IDirect3DExecuteBuffer> eb;
    CHECK(c.dev->CreateExecuteBuffer(&d, &eb.p, NULL));
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(eb->Lock(&d));
    memcpy(d.lpData, vertices, count * sizeof(V32));
    memcpy((BYTE *)d.lpData + count * sizeof(V32), e.buf, e.length());
    CHECK(eb->Unlock());
    D3DEXECUTEDATA x;
    memset(&x, 0, sizeof(x));
    x.dwSize = sizeof(x);
    x.dwVertexOffset = 0;
    x.dwVertexCount = count;
    x.dwInstructionOffset = count * sizeof(V32);
    x.dwInstructionLength = e.length();
    CHECK(eb->SetExecuteData(&x));
    CHECK(c.dev->BeginScene());
    CHECK(c.dev->Execute(eb, c.vp, D3DEXECUTE_CLIPPED));
    CHECK(c.dev->EndScene());
    return 1;
}

/* Matrix handles in D3DOP_STATETRANSFORM, SHADEMODE through
 * D3DOP_STATERENDER, D3DPROCESSVERTICES_TRANSFORM of D3DLVERTEX and _COPY
 * of D3DTLVERTEX, each followed by its triangles; the matrix contents count
 * at Execute time. */
static int scene_exec1(void)
{
    const DWORD bg = 0x0000ff;
    Ctx1 c;
    if (!ctx1_init(c))
        return 0;
    D3DMATRIX world = translation(-0.5f, 0.5f, 0.0f);
    CHECK(c.dev->SetMatrix(c.world, &world));
    V32 v[8];
    lvx_quad(&v[0].l, -0.25f, 0.25f, 0.25f, -0.25f, 0.5f, 0xffff0000u);
    v[1].l.c = 0xff0000ffu; /* flat shading: each triangle takes its first vertex, 0 and 2 */
    v[3].l.c = 0xff0000ffu;
    tlv_quad(&v[4].tl, 36, 36, 60, 60, 0.5f, 0xff00ff00u, 0);
    Exec e;
    emit_base1(e, c);
    e.rs(D3DRENDERSTATE_SHADEMODE, D3DSHADE_FLAT);
    e.process(D3DPROCESSVERTICES_TRANSFORM, 0, 0, 4);
    e.quad(0);
    e.process(D3DPROCESSVERTICES_COPY, 4, 4, 4);
    e.quad(4);
    e.exit();
    if (!clear_bg(c, 0.0f, 0.0f, 1.0f) || !run_exec(c, v, 8, e))
        return 0;
    EXPECT_RECT(c.rt.p, 8, 8, 24, 24, 0xff0000, bg, 0);
    EXPECT_RECT(c.rt.p, 36, 36, 60, 60, 0x00ff00, bg, 0);

    world = translation(0.5f, 0.5f, 0.0f);
    CHECK(c.dev->SetMatrix(c.world, &world));
    if (!clear_bg(c, 0.0f, 0.0f, 1.0f) || !run_exec(c, v, 8, e))
        return 0;
    EXPECT_RECT(c.rt.p, 40, 8, 56, 24, 0xff0000, bg, 0);
    Px gone[] = {{16, 16, bg}};
    EXPECT_PIXELS(c.rt.p, gone, 0);
    return 1;
}

/* D3DPROCESSVERTICES_TRANSFORMLIGHT of D3DVERTEX with a directional
 * D3DLIGHT on the viewport, the material handle and ambient light set by
 * D3DOP_STATELIGHT (+-1); vertices processed after the material handle goes
 * back to 0 are not lit. */
static int scene_execlight1(void)
{
    const DWORD bg = 0x000000;
    Ctx1 c;
    if (!ctx1_init(c))
        return 0;
    Com<IDirect3DMaterial> mat;
    CHECK(c.d3d->CreateMaterial(&mat.p, NULL));
    D3DMATERIAL m;
    lit_material(&m);
    CHECK(mat->SetMaterial(&m));
    D3DMATERIALHANDLE h = 0;
    CHECK(mat->GetHandle(c.dev, &h));
    Com<IDirect3DLight> light;
    CHECK(c.d3d->CreateLight(&light.p, NULL));
    D3DLIGHT2 l;
    lit_light(&l, sizeof(D3DLIGHT), 0);
    CHECK(light->SetLight((D3DLIGHT *)&l));
    CHECK(c.vp->AddLight(light));
    V32 v[8];
    vx_quad(&v[0].v, ndc_x(4), ndc_y(4), ndc_x(28), ndc_y(60), 0.5f);
    vx_quad(&v[4].v, ndc_x(36), ndc_y(4), ndc_x(60), ndc_y(60), 0.5f);
    Exec e;
    emit_base1(e, c);
    e.ls(D3DLIGHTSTATE_MATERIAL, h);
    e.ls(D3DLIGHTSTATE_AMBIENT, LIT_AMBIENT);
    e.process(D3DPROCESSVERTICES_TRANSFORMLIGHT, 0, 0, 4);
    e.ls(D3DLIGHTSTATE_MATERIAL, 0);
    e.process(D3DPROCESSVERTICES_TRANSFORMLIGHT, 4, 4, 4);
    e.quad(0);
    e.quad(4);
    e.exit();
    int ok = clear_bg(c, 0.0f, 0.0f, 0.0f) && run_exec(c, v, 8, e);
    CHECK(c.vp->DeleteLight(light));
    if (!ok)
        return 0;
    printf("    expected lit colour %06lx\n", (unsigned long)lit_rgb(1));
    EXPECT_RECT(c.rt.p, 4, 4, 28, 60, lit_rgb(1), bg, 1);
    EXPECT_RECT(c.rt.p, 36, 4, 60, 60, 0xffffff, bg, 0);
    return 1;
}

/* One D3DLVERTEX triangle crossing the near plane (z = 0 at x = 0, behind
 * it to the left), D3DEXECUTE_CLIPPED: the part in front is drawn, the
 * part behind is not. */
static int scene_execclip1(void)
{
    const DWORD bg = 0x0000ff, red = 0xff0000;
    Ctx1 c;
    if (!ctx1_init(c))
        return 0;
    V32 v[3];
    LVX tri[3] = {
        {-0.75f, 0.75f, -1.0f, 0, 0xffff0000u, 0, 0.0f, 0.0f},
        {-0.75f, -0.75f, -1.0f, 0, 0xffff0000u, 0, 0.0f, 0.0f},
        {0.75f, 0.0f, 1.0f, 0, 0xffff0000u, 0, 0.0f, 0.0f},
    };
    for (int i = 0; i < 3; ++i)
        v[i].l = tri[i];
    static const WORD idx[3] = {0, 1, 2};
    Exec e;
    emit_base1(e, c);
    e.process(D3DPROCESSVERTICES_TRANSFORM, 0, 0, 3);
    e.triangles(idx, 1);
    e.exit();
    if (!clear_bg(c, 0.0f, 0.0f, 1.0f) || !run_exec(c, v, 3, e))
        return 0;
    Px p[] = {{34, 32, red}, {44, 32, red}, {40, 30, red}, {29, 32, bg}, {20, 32, bg}, {12, 32, bg},
              {12, 14, bg},  {56, 8, bg},   {56, 56, bg}};
    EXPECT_PIXELS(c.rt.p, p, 0);
    return 1;
}

/* Texture handles from IDirect3DTexture::GetHandle, bound by TEXTUREHANDLE
 * in D3DOP_STATERENDER; one execute buffer draws two objects, each as its
 * D3DPROCESSVERTICES_COPY, its states and its triangles (on the stock
 * runtime a TEXTUREHANDLE set before the D3DOP_PROCESSVERTICES of its
 * triangles may not texture them).  MODULATE with white and COPY with a
 * colour both show the texels. */
static int scene_tex1(void)
{
    const DWORD bg = 0x000000;
    Ctx1 c;
    if (!ctx1_init(c))
        return 0;
    DWORD raw[2][16], rgb[2][16];
    Com<IDirectDrawSurface> s[2];
    Com<IDirect3DTexture> t[2];
    D3DTEXTUREHANDLE h[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        level_texels(&kX8R8G8B8, 11 * i, raw[i], rgb[i]);
        CHECK(create_surface1(c.dd, DDSCAPS_TEXTURE, 4, 4, &kX8R8G8B8, &s[i].p));
        if (!write_texels(s[i].p, 32, raw[i]))
            return 0;
        CHECK(s[i]->QueryInterface(IID_IDirect3DTexture, (void **)&t[i].p));
        CHECK(t[i]->GetHandle(c.dev, &h[i]));
        EXPECT(h[i] != 0, "texture %d handle 0", i);
    }
    EXPECT(h[0] != h[1], "both textures have handle 0x%lx", (unsigned long)h[0]);
    V32 v[8];
    tlv_quad(&v[0].tl, 0, 0, 32, 32, 0.5f, 0xffffffffu, 0);
    tlv_quad(&v[4].tl, 32, 32, 64, 64, 0.5f, 0xff123456u, 0);
    Exec e;
    emit_base1(e, c);
    e.process(D3DPROCESSVERTICES_COPY, 0, 0, 4);
    e.rs(D3DRENDERSTATE_TEXTUREHANDLE, h[0]);
    e.quad(0);
    e.process(D3DPROCESSVERTICES_COPY, 4, 4, 4);
    e.rs(D3DRENDERSTATE_TEXTUREHANDLE, h[1]);
    e.rs(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_COPY);
    e.quad(4);
    e.exit();
    if (!clear_bg(c, 0.0f, 0.0f, 0.0f) || !run_exec(c, v, 8, e))
        return 0;
    if (!expect_texels(c.rt.p, 0, 0, 32, rgb[0], 0)) {
        printf("    (texture 0, MODULATE with white)\n");
        return 0;
    }
    if (!expect_texels(c.rt.p, 32, 32, 32, rgb[1], 0)) {
        printf("    (texture 1, COPY)\n");
        return 0;
    }
    Px rest[] = {{48, 16, bg}, {16, 48, bg}};
    EXPECT_PIXELS(c.rt.p, rest, 0);
    return 1;
}

/* ------------------------------------------------------------------------ */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

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

static void run(const char *name, int (*fn)(void))
{
    if (!selected(name))
        return;
    pump();
    ++g_scenes;
    int ok = fn();
    if (!ok)
        ++g_failures;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    fflush(stdout);
}

static void print_d3d9(const char *when)
{
    HMODULE d9 = GetModuleHandleA("d3d9.dll");
    char path[MAX_PATH] = "";
    if (d9)
        GetModuleFileNameA(d9, path, MAX_PATH);
    printf("d3d9.dll in process%s: %s\n", when, d9 ? path : "(not loaded)");
}

/* The window, and what the DirectDraw object and its Direct3D interfaces
 * are; 0 on failure. */
static int setup(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qdd_d3d7_legacy_smoke";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 160, 160};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("qdd_d3d7_legacy_smoke", "qdd d3d legacy smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) {
        printf("FAIL: CreateWindow\n");
        return 0;
    }
    pump();

    Com<IDirectDraw> dd;
    HRESULT hr = DirectDrawCreate(NULL, &dd.p, NULL);
    if (FAILED(hr)) {
        printf("FAIL: DirectDrawCreate hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    print_d3d9("");
    /* Compatibility shims (apphelp.dll) may hook the methods of a system
     * DirectDraw object, so the vtable's own module counts as well. */
    HMODULE owner = object_module(dd), table = NULL, local_ddraw = GetModuleHandleA("ddraw.dll");
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(*(void **)dd.p, &mbi, sizeof(mbi)))
        table = (HMODULE)mbi.AllocationBase;
    print_module("IDirectDraw object from", owner);
    if (table != owner)
        print_module("IDirectDraw vtable in", table);
    if (g_expect_local && owner != local_ddraw && table != local_ddraw) {
        printf("FAIL: --expect-local, but the IDirectDraw object is not from the ddraw.dll beside the executable\n");
        return 0;
    }
    Com<IDirectDraw4> dd4;
    if (SUCCEEDED(dd->QueryInterface(IID_IDirectDraw4, (void **)&dd4.p))) {
        DDDEVICEIDENTIFIER id;
        memset(&id, 0, sizeof(id));
        if (SUCCEEDED(dd4->GetDeviceIdentifier(&id, 0)))
            printf("adapter: %s (driver %s, vendor 0x%04lx device 0x%04lx)\n", id.szDescription, id.szDriver,
                   (unsigned long)id.dwVendorId, (unsigned long)id.dwDeviceId);
    }
    static const struct {
        const GUID *iid;
        const char *name;
    } d3ds[] = {{&IID_IDirect3D3, "IDirect3D3"}, {&IID_IDirect3D2, "IDirect3D2"}, {&IID_IDirect3D, "IDirect3D"}};
    for (size_t i = 0; i < ARRAY_SIZE(d3ds); ++i) {
        IUnknown *u = NULL;
        char what[64];
        snprintf(what, sizeof(what), "%s object from", d3ds[i].name);
        hr = dd->QueryInterface(*d3ds[i].iid, (void **)&u);
        if (FAILED(hr)) {
            printf("%s: QueryInterface failed, hr=0x%08lx\n", what, (unsigned long)hr);
            continue;
        }
        print_module(what, object_module(u));
        u->Release();
    }
    return 1;
}

int main(int argc, char **argv)
{
    int expect_stock = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--expect-local"))
            g_expect_local = 1;
        else if (!strcmp(argv[i], "--expect-stock"))
            expect_stock = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_only = argv[++i];
        else {
            printf("usage: %s [--expect-local | --expect-stock] [--only scene[,scene]]\n", argv[0]);
            return 2;
        }
    }

    /* Which ddraw.dll did the loader bind? */
    char mod[MAX_PATH] = "", exe[MAX_PATH] = "";
    GetModuleFileNameA(GetModuleHandleA("ddraw.dll"), mod, MAX_PATH);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash)
        slash[1] = 0;
    int local = _strnicmp(mod, exe, strlen(exe)) == 0 && !strchr(mod + strlen(exe), '\\');
    printf("ddraw.dll loaded from: %s (%s)\n", mod, local ? "beside the executable" : "not beside the executable");
    if ((g_expect_local && !local) || (expect_stock && local)) {
        printf("FAIL: ddraw.dll location does not match %s\n", g_expect_local ? "--expect-local" : "--expect-stock");
        return 2;
    }

    DWORD start = GetTickCount();
    if (!setup()) {
        if (g_wnd)
            DestroyWindow(g_wnd);
        return 1;
    }
    run("qi", scene_qi);
    run("enum3", scene_enum3);
    run("clear2", scene_clear2);
    run("tl3", scene_tl3);
    run("xform3", scene_xform3);
    run("light3", scene_light3);
    run("blend3", scene_blend3);
    run("handle3", scene_handle3);
    run("vb3", scene_vb3);
    run("beginend3", scene_beginend3);
    run("fog3", scene_fog3);
    run("ckey3", scene_ckey3);
    run("enum2", scene_enum2);
    run("defaults", scene_defaults);
    run("vt2", scene_vt2);
    run("tex2", scene_tex2);
    run("p8rt2", scene_p8rt2);
    run("exec1", scene_exec1);
    run("execlight1", scene_execlight1);
    run("execclip1", scene_execclip1);
    run("tex1", scene_tex1);
    print_d3d9(" after the scenes");
    DestroyWindow(g_wnd);
    pump();

    printf("%s: %d/%d scenes passed (%lu ms)\n", g_failures ? "FAIL" : "PASS", g_scenes - g_failures, g_scenes,
           (unsigned long)(GetTickCount() - start));
    return g_failures ? 1 : 0;
}
