/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7_smoke: deterministic Direct3D 7 scenes with pixel checks.
 *
 * Links against ddraw.dll by import, so it exercises whichever ddraw.dll the
 * loader picks: the system runtime, or QD3D's DirectDraw / Direct3D 7 front
 * end (qd3d/ddraw) when its ddraw.dll sits beside the executable.  The first
 * output lines report the loaded module paths; --expect-local /
 * --expect-stock turn the ddraw.dll location into a hard check.  What QD3D
 * does beyond the stock HAL (sampling a system-memory P8 texture) runs only
 * with --expect-local; elsewhere that scene prints [SKIP] with the reason.
 *
 * Windowed: DDSCL_NORMAL on a small visible window, a 128x128 X8R8G8B8
 * video-memory render target with an attached 16-bit z-buffer, pixels read
 * back with Lock outside BeginScene/EndScene, and a clipped primary for the
 * present scene.  Nothing here touches the display mode, except
 * --fullscreen, which adds a scene that takes exclusive mode at 640x480x32
 * with a flip chain: run that only inside a VM.
 *
 * Expected colours follow the Direct3D 7 formulas; where Direct3D 7 leaves
 * room (default states, state block coverage, return codes) the test holds
 * what the stock runtime (Windows 11 SysWOW64 ddraw.dll) does.  Colours
 * compare exactly, except lit and blended results (+-1 per channel) and fog
 * (+-2).  Every scene first sets the state it relies on (base_state),
 * samples pixel centres at least 2 pixels inside primitive edges, and
 * prints [PASS], [FAIL] or [SKIP] with its name.
 */
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>

#define W 128 /* render target and window client size */
#define H 128
#define SKIP 2 /* scene result: not applicable here, g_skip_reason says why */

static HWND g_wnd;
static IDirectDraw7 *g_dd;
static IDirect3D7 *g_d3d;
static IDirect3DDevice7 *g_dev;
static IDirectDrawSurface7 *g_rt;      /* the device's render target */
static IDirectDrawSurface7 *g_primary; /* clipped to g_wnd */
static const GUID *g_devguid;          /* IID_IDirect3DTnLHalDevice or IID_IDirect3DHALDevice */
static D3DDEVICEDESC7 g_caps;
static int g_failures, g_scenes, g_skipped;
static const char *g_only; /* --only <scene[,scene]> */
static int g_expect_local; /* QD3D's ddraw.dll: scenes stock cannot run */
static const char *g_skip_reason;

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

static ULONG refs(IUnknown *u)
{
    u->AddRef();
    return u->Release();
}

static DWORD fdw(float f)
{
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}

static float absf(float f)
{
    return f < 0.0f ? -f : f;
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

/* Reads 32-bit pixels of s (outside BeginScene/EndScene), masked to RGB. */
static int read_pixels(IDirectDrawSurface7 *s, const Px *p, int n, DWORD *got)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(s->Lock(NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL));
    for (int i = 0; i < n; ++i)
        got[i] = ((const DWORD *)((const BYTE *)d.lpSurface + p[i].y * d.lPitch))[p[i].x] & 0x00ffffffu;
    CHECK(s->Unlock(NULL));
    return 1;
}

/* Every mismatch is printed; tol is the allowed difference per channel. */
static int expect_pixels(IDirectDrawSurface7 *s, const Px *p, int n, int tol)
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

#define EXPECT_PIXELS(s, arr, tol)                                                   \
    do {                                                                             \
        if (!expect_pixels((s), (arr), (int)(sizeof(arr) / sizeof((arr)[0])), (tol))) \
            return 0;                                                                \
    } while (0)

/* A primitive covering pixels [x0,x1) x [y0,y1) of the render target: the
 * centre and points 2-3 pixels inside each corner want `in` (+-tol), points
 * 3 pixels outside the middle of each edge want `out` (exact). */
static int expect_rect(int x0, int y0, int x1, int y1, DWORD in, DWORD out, int tol)
{
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, n = 0;
    Px inside[5] = {{x0 + 2, y0 + 2, in}, {x1 - 3, y0 + 2, in}, {x0 + 2, y1 - 3, in}, {x1 - 3, y1 - 3, in},
                    {cx, cy, in}};
    Px around[4] = {{x0 - 3, cy, out}, {x1 + 2, cy, out}, {cx, y0 - 3, out}, {cx, y1 + 2, out}};
    Px outside[4];
    for (int i = 0; i < 4; ++i)
        if (around[i].x >= 0 && around[i].x < W && around[i].y >= 0 && around[i].y < H)
            outside[n++] = around[i];
    int ok = expect_pixels(g_rt, inside, 5, tol);
    return expect_pixels(g_rt, outside, n, 0) && ok;
}

#define EXPECT_RECT(x0, y0, x1, y1, in, out, tol)            \
    do {                                                     \
        if (!expect_rect((x0), (y0), (x1), (y1), (in), (out), (tol))) \
            return 0;                                        \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Vertices                                                                  */

#define FVF_TL (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_TLT (D3DFVF_XYZRHW | D3DFVF_TEX1)
#define FVF_LV (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define FVF_N (D3DFVF_XYZ | D3DFVF_NORMAL)
#define FVF_ND (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE)
#define FVF_LT (D3DFVF_XYZ | D3DFVF_TEX1)
#define FVF_CUBE (D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0))

struct TLV { /* FVF_TL */
    float x, y, z, rhw;
    DWORD c;
};
struct TLT { /* FVF_TLT */
    float x, y, z, rhw, u, v;
};
struct LV { /* FVF_LV */
    float x, y, z;
    DWORD c;
};
struct NV { /* FVF_N */
    float x, y, z, nx, ny, nz;
};
struct NDV { /* FVF_ND */
    float x, y, z, nx, ny, nz;
    DWORD c;
};
struct LT { /* FVF_LT */
    float x, y, z, u, v;
};
struct CUBEV { /* FVF_CUBE */
    float x, y, z, rhw, s, t, r;
};

static const WORD kQuadIndices[6] = {0, 1, 2, 2, 1, 3}; /* strip-ordered corners as a list */

/* Pre-transformed quad over pixels [x0,x1) x [y0,y1), strip order. */
static void tl_rect(TLV *v, float x0, float y0, float x1, float y1, float z, DWORD c)
{
    TLV q[4] = {{x0, y0, z, 1.0f, c}, {x1, y0, z, 1.0f, c}, {x0, y1, z, 1.0f, c}, {x1, y1, z, 1.0f, c}};
    memcpy(v, q, sizeof(q));
}

static HRESULT draw_rect(float x0, float y0, float x1, float y1, float z, DWORD c)
{
    TLV v[4];
    tl_rect(v, x0, y0, x1, y1, z, c);
    return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TL, v, 4, 0);
}

/* Pre-transformed quad over pixels [x0,x1) x [y0,y1) with texture
 * coordinates 0..1 mapped texel-exact. */
static HRESULT draw_tex_rect(float x0, float y0, float x1, float y1)
{
    TLT q[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
        {x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f},
        {x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f},
    };
    return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TLT, q, 4, 0);
}

/* With identity transforms and the full viewport, x and y in [-1, 1] land
 * on pixel (x + 1) * 64, (1 - y) * 64. */
static float ndc_x(int px)
{
    return px / (W / 2.0f) - 1.0f;
}

static float ndc_y(int py)
{
    return 1.0f - py / (H / 2.0f);
}

/* Untransformed quad that identity transforms put over pixels
 * [x0,x1) x [y0,y1). */
static void lv_rect(LV *v, int x0, int y0, int x1, int y1, float z, DWORD c)
{
    float l = ndc_x(x0), r = ndc_x(x1), t = ndc_y(y0), b = ndc_y(y1);
    LV q[4] = {{l, t, z, c}, {r, t, z, c}, {l, b, z, c}, {r, b, z, c}};
    memcpy(v, q, sizeof(q));
}

static HRESULT draw_lv_rect(int x0, int y0, int x1, int y1, float z, DWORD c)
{
    LV v[4];
    lv_rect(v, x0, y0, x1, y1, z, c);
    return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_LV, v, 4, 0);
}

/* ------------------------------------------------------------------------ */
/* Device state                                                              */

static const D3DMATRIX kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static D3DMATRIX translation(float x, float y, float z)
{
    D3DMATRIX m = kIdentity;
    m._41 = x;
    m._42 = y;
    m._43 = z;
    return m;
}

static D3DMATRIX scaling(float x, float y, float z)
{
    D3DMATRIX m = kIdentity;
    m._11 = x;
    m._22 = y;
    m._33 = z;
    return m;
}

/* a * b, row vectors (Direct3D order: a applies first). */
static D3DMATRIX mat_mul(const D3DMATRIX &a, const D3DMATRIX &b)
{
    const float *pa = &a._11, *pb = &b._11;
    D3DMATRIX r;
    float *pr = &r._11;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            pr[i * 4 + j] = pa[i * 4] * pb[j] + pa[i * 4 + 1] * pb[4 + j] + pa[i * 4 + 2] * pb[8 + j] +
                            pa[i * 4 + 3] * pb[12 + j];
    return r;
}

static int set_transform(IDirect3DDevice7 *dev, D3DTRANSFORMSTATETYPE t, const D3DMATRIX &m)
{
    D3DMATRIX c = m;
    CHECK(dev->SetTransform(t, &c));
    return 1;
}

static int transform_is(D3DTRANSFORMSTATETYPE t, const D3DMATRIX &want)
{
    D3DMATRIX m;
    memset(&m, 0xcc, sizeof(m));
    CHECK(g_dev->GetTransform(t, &m));
    return !memcmp(&m, &want, sizeof(m));
}

static D3DVIEWPORT7 viewport(DWORD x, DWORD y, DWORD w, DWORD h, float minz, float maxz)
{
    D3DVIEWPORT7 vp;
    vp.dwX = x;
    vp.dwY = y;
    vp.dwWidth = w;
    vp.dwHeight = h;
    vp.dvMinZ = minz;
    vp.dvMaxZ = maxz;
    return vp;
}

static int set_viewport(DWORD x, DWORD y, DWORD w, DWORD h, float minz, float maxz)
{
    D3DVIEWPORT7 vp = viewport(x, y, w, h, minz, maxz);
    CHECK(g_dev->SetViewport(&vp));
    return 1;
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

/* Lights the scenes use on the main device; base_state switches them off. */
static const DWORD kSceneLights[] = {0, 3};

/* The state every scene starts from: no lighting, z, blending, alpha test,
 * colour key or fog; stage 0 passes the diffuse colour, point filtered;
 * identity transforms, full viewport, black material. */
static int base_state(void)
{
    static const struct {
        D3DRENDERSTATETYPE state;
        DWORD value;
    } rs[] = {
        {D3DRENDERSTATE_ZENABLE, D3DZB_FALSE},
        {D3DRENDERSTATE_ZWRITEENABLE, TRUE},
        {D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL},
        {D3DRENDERSTATE_ZBIAS, 0},
        {D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID},
        {D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRENDERSTATE_CULLMODE, D3DCULL_NONE},
        {D3DRENDERSTATE_DITHERENABLE, FALSE},
        {D3DRENDERSTATE_ALPHABLENDENABLE, FALSE},
        {D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE},
        {D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO},
        {D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
        {D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS},
        {D3DRENDERSTATE_ALPHAREF, 0},
        {D3DRENDERSTATE_COLORKEYENABLE, FALSE},
        {D3DRENDERSTATE_STENCILENABLE, FALSE},
        {D3DRENDERSTATE_FOGENABLE, FALSE},
        {D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE},
        {D3DRENDERSTATE_FOGVERTEXMODE, D3DFOG_NONE},
        {D3DRENDERSTATE_RANGEFOGENABLE, FALSE},
        {D3DRENDERSTATE_SPECULARENABLE, FALSE},
        {D3DRENDERSTATE_TEXTUREFACTOR, 0xffffffff},
        {D3DRENDERSTATE_WRAP0, 0},
        {D3DRENDERSTATE_CLIPPING, TRUE},
        {D3DRENDERSTATE_CLIPPLANEENABLE, 0},
        {D3DRENDERSTATE_LIGHTING, FALSE},
        {D3DRENDERSTATE_AMBIENT, 0},
        {D3DRENDERSTATE_COLORVERTEX, TRUE},
        {D3DRENDERSTATE_LOCALVIEWER, TRUE},
        {D3DRENDERSTATE_NORMALIZENORMALS, FALSE},
        {D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1},
        {D3DRENDERSTATE_SPECULARMATERIALSOURCE, D3DMCS_COLOR2},
        {D3DRENDERSTATE_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL},
        {D3DRENDERSTATE_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL},
        {D3DRENDERSTATE_VERTEXBLEND, D3DVBLEND_DISABLE},
    };
    static const struct {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value;
    } tss[] = {
        {0, D3DTSS_COLOROP, D3DTOP_SELECTARG1},
        {0, D3DTSS_COLORARG1, D3DTA_DIFFUSE},
        {0, D3DTSS_COLORARG2, D3DTA_CURRENT},
        {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1},
        {0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE},
        {0, D3DTSS_ALPHAARG2, D3DTA_CURRENT},
        {0, D3DTSS_TEXCOORDINDEX, 0},
        {0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP},
        {0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP},
        {0, D3DTSS_MAGFILTER, D3DTFG_POINT},
        {0, D3DTSS_MINFILTER, D3DTFN_POINT},
        {0, D3DTSS_MIPFILTER, D3DTFP_NONE},
        {0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE},
        {1, D3DTSS_COLOROP, D3DTOP_DISABLE},
        {1, D3DTSS_ALPHAOP, D3DTOP_DISABLE},
    };
    for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i) {
        HRESULT hr = g_dev->SetRenderState(rs[i].state, rs[i].value);
        if (FAILED(hr)) {
            printf("    SetRenderState(%d, 0x%lx) failed: hr=0x%08lx\n", (int)rs[i].state,
                   (unsigned long)rs[i].value, (unsigned long)hr);
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(tss) / sizeof(tss[0]); ++i) {
        HRESULT hr = g_dev->SetTextureStageState(tss[i].stage, tss[i].type, tss[i].value);
        if (FAILED(hr)) {
            printf("    SetTextureStageState(%lu, %d, 0x%lx) failed: hr=0x%08lx\n", (unsigned long)tss[i].stage,
                   (int)tss[i].type, (unsigned long)tss[i].value, (unsigned long)hr);
            return 0;
        }
    }
    CHECK(g_dev->SetTexture(0, NULL));
    CHECK(g_dev->SetTexture(1, NULL));
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, kIdentity) ||
        !set_transform(g_dev, D3DTRANSFORMSTATE_VIEW, kIdentity) ||
        !set_transform(g_dev, D3DTRANSFORMSTATE_PROJECTION, kIdentity) || !set_viewport(0, 0, W, H, 0.0f, 1.0f))
        return 0;
    D3DMATERIAL7 m;
    memset(&m, 0, sizeof(m));
    CHECK(g_dev->SetMaterial(&m));
    for (size_t i = 0; i < sizeof(kSceneLights) / sizeof(kSceneLights[0]); ++i)
        CHECK(g_dev->LightEnable(kSceneLights[i], FALSE));
    return 1;
}

/* Stage 0 shows texture t, point sampled without mipmapping. */
static int use_texture(IDirectDrawSurface7 *t)
{
    CHECK(g_dev->SetTexture(0, t));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE));
    return 1;
}

static HRESULT clear(DWORD rgb)
{
    return g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u | rgb, 1.0f, 0);
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
static const Fmt kX1R5G5B5 = {"X1R5G5B5", DDPF_RGB, 16, 0x7c00, 0x03e0, 0x001f, 0};
static const Fmt kA1R5G5B5 = {"A1R5G5B5", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000};
static const Fmt kA4R4G4B4 = {"A4R4G4B4", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000};
static const Fmt kP8 = {"P8", DDPF_PALETTEINDEXED8 | DDPF_RGB, 8, 0, 0, 0, 0};

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
    if (f->flags & DDPF_PALETTEINDEXED8)
        return (pf->dwFlags & DDPF_PALETTEINDEXED8) && pf->dwRGBBitCount == 8;
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

/* Whether the device's EnumTextureFormats reports f. */
static int texture_format_enumerated(const Fmt *f)
{
    FormatList l;
    l.n = 0;
    if (FAILED(g_dev->EnumTextureFormats(collect_format, &l)))
        return 0;
    for (int i = 0; i < l.n; ++i)
        if (format_matches(&l.f[i], f))
            return 1;
    return 0;
}

/* 32-bit X8R8G8B8 where enumerated, else R5G6B5 (the enum scene checks one
 * of them is). */
static const Fmt *plain_format(void)
{
    return texture_format_enumerated(&kX8R8G8B8) ? &kX8R8G8B8 : &kR5G6B5;
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

/* An 8-bit channel value as its top bits in `mask`. */
static DWORD channel_pack(DWORD c8, DWORD mask)
{
    if (!mask)
        return 0;
    return ((c8 >> (8 - channel_bits(mask))) << channel_shift(mask)) & mask;
}

/* The channel in `mask` widened to 8 bits: round(v * 255 / max).  The
 * colour levels below are chosen so that this equals bit replication. */
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
 * every format here (0x84 is 16 in 5 bits, 33 in 6, 8 in 4, and widens back
 * the same by rounding and by bit replication). */
static DWORD level_colour(int i)
{
    static const DWORD level[3] = {0x00, 0x84, 0xff};
    return 0xff000000u | level[i % 3] << 16 | level[i / 3 % 3] << 8 | level[i / 9 % 3];
}

static HRESULT create_texture(const Fmt *f, DWORD w, DWORD h, DWORD caps, DWORD caps2, DWORD mips,
                              IDirectDrawSurface7 **out)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | caps;
    d.ddsCaps.dwCaps2 = caps2;
    if (mips) {
        d.dwFlags |= DDSD_MIPMAPCOUNT;
        d.dwMipMapCount = mips;
    }
    set_pixel_format(&d.ddpfPixelFormat, f);
    return g_dd->CreateSurface(&d, out, NULL);
}

/* Writes w x h raw texels (row-major) through Lock. */
static int write_texels(IDirectDrawSurface7 *s, DWORD bits, const DWORD *raw, int w, int h)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(s->Lock(NULL, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL));
    for (int y = 0; y < h; ++y) {
        BYTE *row = (BYTE *)d.lpSurface + y * d.lPitch;
        for (int x = 0; x < w; ++x) {
            DWORD v = raw[y * w + x];
            if (bits == 32)
                ((DWORD *)row)[x] = v;
            else if (bits == 16)
                ((WORD *)row)[x] = (WORD)v;
            else
                row[x] = (BYTE)v;
        }
    }
    CHECK(s->Unlock(NULL));
    return 1;
}

static int fill_texels(IDirectDrawSurface7 *s, DWORD bits, DWORD raw, int w, int h)
{
    DWORD all[32 * 32];
    for (int i = 0; i < w * h; ++i)
        all[i] = raw;
    return write_texels(s, bits, all, w, h);
}

/* A w x h X8R8G8B8 DDSCAPS_3DDEVICE surface in video memory; kind is
 * DDSCAPS_OFFSCREENPLAIN or DDSCAPS_TEXTURE. */
static HRESULT create_target(IDirectDraw7 *dd, DWORD w, DWORD h, DWORD kind, IDirectDrawSurface7 **out)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddsCaps.dwCaps = kind | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    set_pixel_format(&d.ddpfPixelFormat, &kX8R8G8B8);
    return dd->CreateSurface(&d, out, NULL);
}

/* Attaches a new 16-bit video-memory z-buffer to rt. */
static int attach_z(IDirectDraw7 *dd, IDirectDrawSurface7 *rt, DWORD w, DWORD h)
{
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    d.ddpfPixelFormat.dwZBufferBitDepth = 16;
    d.ddpfPixelFormat.dwZBitMask = 0xffff;
    Com<IDirectDrawSurface7> z;
    CHECK(dd->CreateSurface(&d, &z.p, NULL));
    CHECK(rt->AddAttachedSurface(z));
    return 1;
}

/* T&L HAL, else HAL. */
static HRESULT create_device(IDirect3D7 *d3d, IDirectDrawSurface7 *rt, const GUID **guid, IDirect3DDevice7 **dev)
{
    *guid = &IID_IDirect3DTnLHalDevice;
    HRESULT hr = d3d->CreateDevice(IID_IDirect3DTnLHalDevice, rt, dev);
    if (FAILED(hr)) {
        *guid = &IID_IDirect3DHALDevice;
        hr = d3d->CreateDevice(IID_IDirect3DHALDevice, rt, dev);
    }
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Lighting                                                                  */

/* One white-ish directional light shining down the view direction at an
 * angle: N = (0, 0, -1) gives N.L = 0.8. */
static const float kLightDiffuse[3] = {1.0f, 0.5f, 1.0f};
static const float kLightDir[3] = {0.0f, 0.6f, 0.8f};
static const float kMatDiffuse[3] = {0.5f, 1.0f, 0.25f};
static const float kMatAmbient[3] = {1.0f, 0.5f, 0.0f};
static const float kMatEmissive[3] = {0.0f, 0.0f, 0.25f};
#define LIT_AMBIENT 0x00404040u /* D3DRENDERSTATE_AMBIENT */

static int set_lighting(void)
{
    D3DLIGHT7 l;
    memset(&l, 0, sizeof(l));
    l.dltType = D3DLIGHT_DIRECTIONAL;
    l.dcvDiffuse = colour_value(kLightDiffuse[0], kLightDiffuse[1], kLightDiffuse[2], 1.0f);
    l.dvDirection = vec(kLightDir[0], kLightDir[1], kLightDir[2]);
    CHECK(g_dev->SetLight(0, &l));
    CHECK(g_dev->LightEnable(0, TRUE));
    D3DMATERIAL7 m;
    memset(&m, 0, sizeof(m));
    m.diffuse = colour_value(kMatDiffuse[0], kMatDiffuse[1], kMatDiffuse[2], 1.0f);
    m.ambient = colour_value(kMatAmbient[0], kMatAmbient[1], kMatAmbient[2], 1.0f);
    m.emissive = colour_value(kMatEmissive[0], kMatEmissive[1], kMatEmissive[2], 0.0f);
    CHECK(g_dev->SetMaterial(&m));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_AMBIENT, LIT_AMBIENT));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORVERTEX, FALSE));
    return 1;
}

/* The Direct3D lighting equation for normal (0, 0, -1) and the light above:
 * emissive + ambient * AMBIENT + diffuse * light * N.L, clamped, as
 * 0xRRGGBB.  `diffuse` is the material (or vertex) diffuse colour. */
static DWORD lit_rgb(const float *diffuse, int light_on)
{
    float ndotl = kLightDir[2]; /* N . -dir with N = (0, 0, -1) */
    DWORD out = 0;
    for (int i = 0; i < 3; ++i) {
        float ambient = ((LIT_AMBIENT >> (16 - 8 * i)) & 0xff) / 255.0f;
        float v = kMatEmissive[i] + kMatAmbient[i] * ambient;
        if (light_on)
            v += diffuse[i] * kLightDiffuse[i] * ndotl;
        if (v > 1.0f)
            v = 1.0f;
        out = out << 8 | (DWORD)(v * 255.0f + 0.5f);
    }
    return out;
}

/* ------------------------------------------------------------------------ */
/* Scenes                                                                    */

/* 1. Device, z-buffer and texture format enumeration. */

struct DeviceRecord {
    GUID guid;
    char name[64], desc[128];
    DWORD devcaps;
};

struct DeviceList {
    DeviceRecord d[8];
    int n;
    HRESULT ret;
};

static HRESULT CALLBACK collect_device(char *desc, char *name, D3DDEVICEDESC7 *dd, void *ctx)
{
    DeviceList *l = (DeviceList *)ctx;
    if (l->n < 8) {
        DeviceRecord *r = &l->d[l->n];
        r->guid = dd->deviceGUID;
        snprintf(r->name, sizeof(r->name), "%s", name ? name : "(null)");
        snprintf(r->desc, sizeof(r->desc), "%s", desc ? desc : "(null)");
        r->devcaps = dd->dwDevCaps;
    }
    ++l->n;
    return l->ret;
}

static const char *device_guid_name(const GUID &g)
{
    if (g == IID_IDirect3DRGBDevice)
        return "IID_IDirect3DRGBDevice";
    if (g == IID_IDirect3DHALDevice)
        return "IID_IDirect3DHALDevice";
    if (g == IID_IDirect3DTnLHalDevice)
        return "IID_IDirect3DTnLHalDevice";
    return "an unknown GUID";
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

static int scene_enum(void)
{
    static const GUID *const guids[3] = {&IID_IDirect3DRGBDevice, &IID_IDirect3DHALDevice,
                                         &IID_IDirect3DTnLHalDevice};
    /* The T&L HAL name is what the stock runtime calls it. */
    static const char *const names[3] = {"RGB Emulation", "Direct3D HAL", "Direct3D T&L HAL"};
    DeviceList l;
    memset(&l, 0, sizeof(l));
    l.ret = DDENUMRET_OK;
    CHECK(g_d3d->EnumDevices(collect_device, &l));
    for (int i = 0; i < l.n && i < 8; ++i)
        printf("    device %d: \"%s\" / \"%s\", %s, devcaps 0x%08lx\n", i, l.d[i].name, l.d[i].desc,
               device_guid_name(l.d[i].guid), (unsigned long)l.d[i].devcaps);
    EXPECT(l.n == 3, "%d devices", l.n);
    for (int i = 0; i < 3; ++i) {
        EXPECT(l.d[i].guid == *guids[i], "device %d is %s", i, device_guid_name(l.d[i].guid));
        EXPECT(!strcmp(l.d[i].name, names[i]), "device %d is called \"%s\"", i, l.d[i].name);
    }
    EXPECT(!(l.d[1].devcaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT), "the HAL claims hardware T&L");
    EXPECT(l.d[2].devcaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT, "the T&L HAL lacks hardware T&L");
    EXPECT_HR(g_d3d->EnumDevices(NULL, NULL), DDERR_INVALIDPARAMS);
    memset(&l, 0, sizeof(l));
    l.ret = DDENUMRET_CANCEL;
    EXPECT_HR(g_d3d->EnumDevices(collect_device, &l), D3D_OK);
    EXPECT(l.n == 1, "%d callbacks after DDENUMRET_CANCEL", l.n);

    FormatList z;
    z.n = 0;
    CHECK(g_d3d->EnumZBufferFormats(IID_IDirect3DHALDevice, collect_format, &z));
    int z16 = 0;
    for (int i = 0; i < z.n; ++i) {
        const DDPIXELFORMAT *f = &z.f[i];
        printf("    zfmt: flags=0x%08lx bits=%lu zmask=%08lx stencil=%lu/%08lx\n", (unsigned long)f->dwFlags,
               (unsigned long)f->dwZBufferBitDepth, (unsigned long)f->dwZBitMask,
               (unsigned long)f->dwStencilBitDepth, (unsigned long)f->dwStencilBitMask);
        if ((f->dwFlags & DDPF_ZBUFFER) && f->dwZBufferBitDepth == 16 && f->dwZBitMask == 0xffff)
            z16 = 1;
    }
    EXPECT(z16, "no 16-bit z-buffer format among %d", z.n);

    FormatList t;
    t.n = 0;
    CHECK(g_dev->EnumTextureFormats(collect_format, &t));
    for (int i = 0; i < t.n; ++i)
        print_format("texfmt", &t.f[i]);
    EXPECT(texture_format_enumerated(&kX8R8G8B8) || texture_format_enumerated(&kA8R8G8B8),
           "neither X8R8G8B8 nor A8R8G8B8 among %d texture formats", t.n);
    EXPECT(texture_format_enumerated(&kR5G6B5), "R5G6B5 not among %d texture formats", t.n);
    return 1;
}

/* 2. Full and rectangle clears, exact to the rectangle edges; a z-only
 * rectangle clear that keeps a z-tested quad out; Clear covers the viewport
 * only, rectangles count in render target pixels and are clipped to the
 * viewport; misuse return codes. */
static void set_d3drect(D3DRECT *r, LONG x1, LONG y1, LONG x2, LONG y2)
{
    r->x1 = x1;
    r->y1 = y1;
    r->x2 = x2;
    r->y2 = y2;
}

static int scene_clear(void)
{
    const DWORD bg = 0x2040c0, c = 0x00ff80, quad = 0xffff00;
    if (!base_state())
        return 0;
    CHECK(clear(bg));
    Px full[] = {{0, 0, bg}, {W - 1, 0, bg}, {0, H - 1, bg}, {W - 1, H - 1, bg}, {64, 64, bg}};
    EXPECT_PIXELS(g_rt, full, 0);

    D3DRECT r[2];
    set_d3drect(&r[0], 8, 8, 40, 24);
    set_d3drect(&r[1], 64, 72, 120, 120);
    CHECK(g_dev->Clear(2, r, D3DCLEAR_TARGET, 0xff000000u | c, 1.0f, 0));
    Px part[] = {
        {8, 8, c},     {39, 23, c},    {24, 16, c},    {7, 8, bg},      {40, 8, bg},     {8, 7, bg},   {8, 24, bg},
        {64, 72, c},   {119, 119, c},  {90, 96, c},    {63, 100, bg},   {120, 100, bg},  {100, 71, bg},
        {100, 120, bg}, {52, 48, bg},
    };
    EXPECT_PIXELS(g_rt, part, 0);

    D3DRECT zr;
    set_d3drect(&zr, 16, 40, 48, 64);
    CHECK(g_dev->Clear(1, &zr, D3DCLEAR_ZBUFFER, 0, 0.0f, 0));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(0, 0, W, H, 0.5f, 0xff000000u | quad));
    CHECK(g_dev->EndScene());
    Px z[] = {
        {16, 40, bg},   {47, 63, bg},   {32, 52, bg},   {15, 52, quad}, {48, 52, quad},
        {32, 39, quad}, {32, 64, quad}, {8, 8, quad},   {100, 100, quad},
    };
    EXPECT_PIXELS(g_rt, z, 0);

    if (!set_viewport(32, 32, 64, 64, 0.0f, 1.0f))
        return 0;
    CHECK(g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xff000000u | c, 1.0f, 0));
    set_d3drect(&r[0], 0, 0, 48, 48);
    CHECK(g_dev->Clear(1, r, D3DCLEAR_TARGET, 0xffff0000u, 1.0f, 0));
    Px vp[] = {
        {32, 32, 0xff0000}, {47, 47, 0xff0000}, {48, 40, c},   {40, 48, c},    {95, 95, c},    {64, 64, c},
        {31, 36, quad},     {36, 31, quad},     {96, 64, quad}, {64, 96, quad}, {20, 20, quad}, {30, 44, bg},
    };
    EXPECT_PIXELS(g_rt, vp, 0);
    EXPECT_HR(g_dev->Clear(0, NULL, 0, 0, 1.0f, 0), DDERR_INVALIDPARAMS);
    EXPECT_HR(g_dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 0), D3DERR_STENCILBUFFER_NOTPRESENT);
    return set_viewport(0, 0, W, H, 0.0f, 1.0f);
}

/* 3. Pre-transformed, flat-coloured triangle strips; a draw outside
 * BeginScene/EndScene renders as well; flat shading takes each triangle's
 * first vertex colour. */
static int scene_tlquad(void)
{
    const DWORD bg = 0x000080;
    if (!base_state())
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(16, 16, 64, 64, 0.5f, 0xffff0000u));
    CHECK(g_dev->EndScene());
    CHECK(draw_rect(72, 72, 112, 112, 0.5f, 0xff00ff00u));
    EXPECT_RECT(16, 16, 64, 64, 0xff0000, bg, 0);
    EXPECT_RECT(72, 72, 112, 112, 0x00ff00, bg, 0);

    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_FLAT));
    TLV strip[4] = {
        {16, 72, 0.5f, 1.0f, 0xffff0000u}, {64, 72, 0.5f, 1.0f, 0xff00ff00u},
        {16, 120, 0.5f, 1.0f, 0xff0000ffu}, {64, 120, 0.5f, 1.0f, 0xffffffffu},
    };
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TL, strip, 4, 0));
    CHECK(g_dev->EndScene());
    Px flat[] = {{20, 76, 0xff0000}, {30, 90, 0xff0000}, {58, 114, 0x00ff00}, {50, 100, 0x00ff00}, {13, 96, bg}};
    EXPECT_PIXELS(g_rt, flat, 0);
    return 1;
}

/* 4. World, view and perspective projection with a viewport; clipping to
 * the viewport; GetTransform and MultiplyTransform. */

/* Clears the whole target (Clear covers only the viewport), then draws the
 * strip through viewport (32, 32, 64, 64). */
static int draw_in_small_viewport(LV *quad, DWORD bg)
{
    if (!set_viewport(0, 0, W, H, 0.0f, 1.0f))
        return 0;
    CHECK(clear(bg));
    if (!set_viewport(32, 32, 64, 64, 0.0f, 1.0f))
        return 0;
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_LV, quad, 4, 0));
    CHECK(g_dev->EndScene());
    return 1;
}

static int scene_transform(void)
{
    const DWORD bg = 0x101010, green = 0x00ff00;
    const float q = 10.0f / 9.0f; /* z near 1, far 10 */
    const D3DMATRIX world = {1.5f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 1, 0, 0.75f, 0.75f, 0, 1};
    const D3DMATRIX view = translation(0.0f, 0.0f, 3.0f);
    const D3DMATRIX proj = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, q, 1, 0, 0, -q, 0};
    LV quad[4] = {
        {-1, -1, 0, 0xff000000u | green}, {1, -1, 0, 0xff000000u | green},
        {-1, 1, 0, 0xff000000u | green},  {1, 1, 0, 0xff000000u | green},
    };
    if (!base_state())
        return 0;
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, world) || !set_transform(g_dev, D3DTRANSFORMSTATE_VIEW, view) ||
        !set_transform(g_dev, D3DTRANSFORMSTATE_PROJECTION, proj))
        return 0;
    EXPECT(transform_is(D3DTRANSFORMSTATE_WORLD, world), "GetTransform(WORLD) differs from SetTransform");
    EXPECT(transform_is(D3DTRANSFORMSTATE_VIEW, view), "GetTransform(VIEW) differs from SetTransform");
    EXPECT(transform_is(D3DTRANSFORMSTATE_PROJECTION, proj), "GetTransform(PROJECTION) differs from SetTransform");

    /* x, y in [-1, 1] -> world [-0.75, 2.25] -> w = 3 -> NDC [-0.25, 0.75]
     * -> pixels x [48, 112), y [16, 80). */
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_LV, quad, 4, 0));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(48, 16, 112, 80, green, bg, 0);

    /* The same through viewport (32, 32, 64, 64): x [56, 88), y [40, 72). */
    if (!draw_in_small_viewport(quad, bg))
        return 0;
    EXPECT_RECT(56, 40, 88, 72, green, bg, 0);

    /* Scaled 4x more it reaches NDC [-2, 2] and is clipped to the viewport. */
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, scaling(6.0f, 6.0f, 1.0f)) ||
        !draw_in_small_viewport(quad, bg))
        return 0;
    EXPECT_RECT(32, 32, 96, 96, green, bg, 0);

    /* MultiplyTransform(state, m) makes state m * state (m applies first). */
    const D3DMATRIX a = translation(1.0f, 2.0f, 3.0f), b = scaling(2.0f, 3.0f, 4.0f);
    D3DMATRIX bc = b;
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, a))
        return 0;
    CHECK(g_dev->MultiplyTransform(D3DTRANSFORMSTATE_WORLD, &bc));
    D3DMATRIX got;
    CHECK(g_dev->GetTransform(D3DTRANSFORMSTATE_WORLD, &got));
    const D3DMATRIX want = mat_mul(b, a), other = mat_mul(a, b);
    EXPECT(!memcmp(&got, &want, sizeof(got)), "MultiplyTransform gave row 4 = %g %g %g %g%s", got._41, got._42,
           got._43, got._44, !memcmp(&got, &other, sizeof(got)) ? " (state * m)" : "");
    return 1;
}

/* 5. Fixed-function lighting: one directional light, material, ambient
 * render state; vertex colour as diffuse material (COLORVERTEX,
 * DIFFUSEMATERIALSOURCE = COLOR1) or not; light switched off. */
static HRESULT draw_lit_rect(int x0, int y0, int x1, int y1, int with_colour, DWORD c)
{
    float l = ndc_x(x0), r = ndc_x(x1), t = ndc_y(y0), b = ndc_y(y1);
    if (with_colour) {
        NDV v[4] = {{l, t, 0.5f, 0, 0, -1, c}, {r, t, 0.5f, 0, 0, -1, c}, {l, b, 0.5f, 0, 0, -1, c},
                    {r, b, 0.5f, 0, 0, -1, c}};
        return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_ND, v, 4, 0);
    }
    NV v[4] = {{l, t, 0.5f, 0, 0, -1}, {r, t, 0.5f, 0, 0, -1}, {l, b, 0.5f, 0, 0, -1}, {r, b, 0.5f, 0, 0, -1}};
    return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_N, v, 4, 0);
}

static int scene_lit(void)
{
    const DWORD bg = 0x000000, vertex_colour = 0xff20c0ffu;
    const float vertex_diffuse[3] = {0x20 / 255.0f, 0xc0 / 255.0f, 1.0f};
    if (!base_state() || !set_lighting())
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_lit_rect(8, 8, 56, 56, 0, 0));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORVERTEX, TRUE));
    CHECK(draw_lit_rect(72, 8, 120, 56, 1, vertex_colour));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORVERTEX, FALSE));
    CHECK(draw_lit_rect(8, 72, 56, 120, 1, vertex_colour));
    CHECK(g_dev->LightEnable(0, FALSE));
    CHECK(draw_lit_rect(72, 72, 120, 120, 0, 0));
    CHECK(g_dev->EndScene());
    printf("    expected lit colours: material %06lx, vertex %06lx, light off %06lx\n",
           (unsigned long)lit_rgb(kMatDiffuse, 1), (unsigned long)lit_rgb(vertex_diffuse, 1),
           (unsigned long)lit_rgb(kMatDiffuse, 0));
    EXPECT_RECT(8, 8, 56, 56, lit_rgb(kMatDiffuse, 1), bg, 1);
    EXPECT_RECT(72, 8, 120, 56, lit_rgb(vertex_diffuse, 1), bg, 1);
    EXPECT_RECT(8, 72, 56, 120, lit_rgb(kMatDiffuse, 1), bg, 1);
    EXPECT_RECT(72, 72, 120, 120, lit_rgb(kMatDiffuse, 0), bg, 1);
    return 1;
}

/* 6. Z test: the nearer quad wins in either drawing order; without z
 * writes, or without z test, the later quad wins. */
static int scene_ztest(void)
{
    const DWORD bg = 0x000000, near_c = 0x00ff00, far_c = 0xff0000;
    struct {
        int near_first, zwrite, zenable;
        DWORD overlap;
    } cases[] = {
        {0, TRUE, D3DZB_TRUE, near_c},
        {1, TRUE, D3DZB_TRUE, near_c},
        {1, FALSE, D3DZB_TRUE, far_c},
        {1, TRUE, D3DZB_FALSE, far_c},
    };
    if (!base_state())
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ZENABLE, cases[i].zenable));
        CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, cases[i].zwrite));
        CHECK(clear(bg));
        CHECK(g_dev->BeginScene());
        for (int k = 0; k < 2; ++k) {
            if ((k == 0) == (cases[i].near_first != 0))
                CHECK(draw_rect(16, 16, 80, 80, 0.25f, 0xff000000u | near_c));
            else
                CHECK(draw_rect(48, 48, 112, 112, 0.75f, 0xff000000u | far_c));
        }
        CHECK(g_dev->EndScene());
        Px p[] = {{20, 20, near_c}, {44, 44, near_c}, {64, 64, cases[i].overlap}, {51, 76, cases[i].overlap},
                  {84, 84, far_c},  {108, 108, far_c}, {100, 20, bg},           {20, 100, bg}};
        if (!expect_pixels(g_rt, p, (int)(sizeof(p) / sizeof(p[0])), 0)) {
            printf("    case %d: near %s, ZWRITEENABLE %d, ZENABLE %d\n", (int)i,
                   cases[i].near_first ? "first" : "last", cases[i].zwrite, cases[i].zenable);
            return 0;
        }
    }
    return 1;
}

/* 7. SRCALPHA / INVSRCALPHA blending with diffuse alpha 0x80 (+-1), then the
 * alpha test GREATER against ALPHAREF exactly at the boundary. */
static int scene_alpha(void)
{
    const DWORD bg = 0x0080ff;
    if (!base_state())
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(32, 32, 96, 96, 0.5f, 0x80ff0040u));
    CHECK(g_dev->EndScene());
    /* src * a + dst * (1 - a), a = 128/255: R 255a = 128, G 128(1-a) = 63.75,
     * B 64a + 255(1-a) = 159.1. */
    EXPECT_RECT(32, 32, 96, 96, 0x80409f, bg, 1);

    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0x7f));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(8, 8, 56, 56, 0.5f, 0x80ff0000u));   /* 0x80 > 0x7f: kept */
    CHECK(draw_rect(72, 8, 120, 56, 0.5f, 0x7f00ff00u)); /* 0x7f > 0x7f: killed */
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0x80));
    CHECK(draw_rect(8, 72, 56, 120, 0.5f, 0x80ffff00u));  /* 0x80 > 0x80: killed */
    CHECK(draw_rect(72, 72, 120, 120, 0.5f, 0x81ff00ffu)); /* 0x81 > 0x80: kept */
    CHECK(g_dev->EndScene());
    EXPECT_RECT(8, 8, 56, 56, 0xff0000, bg, 0);
    EXPECT_RECT(72, 8, 120, 56, bg, bg, 0);
    EXPECT_RECT(8, 72, 56, 120, bg, bg, 0);
    EXPECT_RECT(72, 72, 120, 120, 0xff00ff, bg, 0);
    return 1;
}

/* 4x4 texture drawn over [32,96)^2, 16x16 pixels per texel: the centre of
 * texel t is pixel (40 + 16 (t % 4), 40 + 16 (t / 4)). */
static int expect_texels(const DWORD *rgb, DWORD bg)
{
    Px p[17];
    for (int t = 0; t < 16; ++t) {
        p[t].x = 40 + 16 * (t % 4);
        p[t].y = 40 + 16 * (t / 4);
        p[t].want = rgb[t];
    }
    p[16].x = 20;
    p[16].y = 64;
    p[16].want = bg;
    return expect_pixels(g_rt, p, 17, 0);
}

static int draw_texels(DWORD bg)
{
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_tex_rect(32, 32, 96, 96));
    CHECK(g_dev->EndScene());
    return 1;
}

/* 8. Point-sampled 4x4 textures in each RGB format the device enumerates,
 * texels written through Lock; rewritten texels show up on the next draw;
 * SetTexture / GetTexture references. */
static int texture_format(const Fmt *f)
{
    const DWORD bg = 0x400040;
    DWORD raw[16], rgb[16];
    Com<IDirectDrawSurface7> tex;
    CHECK(create_texture(f, 4, 4, 0, 0, 0, &tex.p));
    for (int t = 0; t < 16; ++t) {
        raw[t] = format_pack(f, level_colour(t));
        rgb[t] = format_rgb(f, raw[t]);
    }
    if (!write_texels(tex, f->bits, raw, 4, 4))
        return 0;
    ULONG before = refs(tex);
    if (!use_texture(tex))
        return 0;
    EXPECT(refs(tex) == before + 1, "SetTexture took %lu references", (unsigned long)(refs(tex) - before));
    IDirectDrawSurface7 *got = NULL;
    CHECK(g_dev->GetTexture(0, &got));
    EXPECT(got == tex, "GetTexture returned %p, not %p", (void *)got, (void *)tex.p);
    EXPECT(refs(tex) == before + 2, "GetTexture took %lu references", (unsigned long)(refs(tex) - before - 1));
    got->Release();
    if (!draw_texels(bg) || !expect_texels(rgb, bg))
        return 0;

    for (int t = 0; t < 16; ++t) {
        raw[t] = format_pack(f, level_colour((t + 7) % 27));
        rgb[t] = format_rgb(f, raw[t]);
    }
    if (!write_texels(tex, f->bits, raw, 4, 4) || !draw_texels(bg))
        return 0;
    if (!expect_texels(rgb, bg)) {
        printf("    (after rewriting the texels)\n");
        return 0;
    }
    CHECK(g_dev->SetTexture(0, NULL));
    EXPECT(refs(tex) == before, "SetTexture(NULL) left %lu references", (unsigned long)(refs(tex) - before));
    return 1;
}

static int scene_texture(void)
{
    static const Fmt *const formats[] = {&kX8R8G8B8, &kA8R8G8B8, &kR5G6B5, &kX1R5G5B5, &kA1R5G5B5, &kA4R4G4B4};
    int tested = 0;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        if (!texture_format_enumerated(formats[i])) {
            printf("    %s: not enumerated, skipped\n", formats[i]->name);
            continue;
        }
        if (!base_state())
            return 0;
        if (!texture_format(formats[i])) {
            printf("    format %s\n", formats[i]->name);
            return 0;
        }
        ++tested;
    }
    EXPECT(tested >= 2, "only %d formats tested", tested);
    return 1;
}

/* 9. Source colour keys: keyed texels leave the background with
 * COLORKEYENABLE and draw in their colour without it; a texture without a
 * key is not keyed; a key set on a bound texture applies to the next draw;
 * an A8R8G8B8 texture keyed at creation (DDSD_CKSRCBLT).  The stock runtime
 * ignores keys set after a texture's first draw, so every texture here gets
 * its key before it is drawn. */
static int scene_colorkey(void)
{
    const DWORD bg = 0x123456, magenta = 0xffff00ffu;
    const Fmt *f = texture_format_enumerated(&kR5G6B5) ? &kR5G6B5 : &kX8R8G8B8;
    DWORD raw[16], keyed[16], unkeyed[16];
    if (!base_state())
        return 0;
    DWORD key = format_pack(f, magenta);
    for (int t = 0; t < 16; ++t) {
        int diagonal = t % 5 == 0;
        raw[t] = format_pack(f, diagonal ? magenta : level_colour(t)); /* level_colour(t < 16) is never magenta */
        unkeyed[t] = format_rgb(f, raw[t]);
        keyed[t] = diagonal ? bg : unkeyed[t];
    }
    Com<IDirectDrawSurface7> plain, tex;
    CHECK(create_texture(f, 4, 4, 0, 0, 0, &plain.p));
    CHECK(create_texture(f, 4, 4, 0, 0, 0, &tex.p));
    if (!write_texels(plain, f->bits, raw, 4, 4) || !write_texels(tex, f->bits, raw, 4, 4) || !use_texture(plain))
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE));
    if (!draw_texels(bg) || !expect_texels(unkeyed, bg)) {
        printf("    (%s without a key, COLORKEYENABLE)\n", f->name);
        return 0;
    }
    if (!use_texture(tex))
        return 0;
    DDCOLORKEY ck;
    ck.dwColorSpaceLowValue = key;
    ck.dwColorSpaceHighValue = key;
    CHECK(tex->SetColorKey(DDCKEY_SRCBLT, &ck));
    if (!draw_texels(bg) || !expect_texels(keyed, bg)) {
        printf("    (%s, COLORKEYENABLE, key %04lx)\n", f->name, (unsigned long)key);
        return 0;
    }
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE));
    if (!draw_texels(bg) || !expect_texels(unkeyed, bg)) {
        printf("    (%s, COLORKEYENABLE FALSE)\n", f->name);
        return 0;
    }

    if (!texture_format_enumerated(&kA8R8G8B8)) {
        printf("    A8R8G8B8: not enumerated, skipped\n");
        return 1;
    }
    const DWORD green = 0xff00ff00u;
    for (int t = 0; t < 16; ++t) {
        int anti = t == 3 || t == 6 || t == 9 || t == 12;
        raw[t] = anti ? green : level_colour(t); /* only level_colour(6) is green */
        unkeyed[t] = raw[t] & 0xffffff;
        keyed[t] = anti ? bg : unkeyed[t];
    }
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CKSRCBLT;
    d.dwWidth = 4;
    d.dwHeight = 4;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    set_pixel_format(&d.ddpfPixelFormat, &kA8R8G8B8);
    d.ddckCKSrcBlt.dwColorSpaceLowValue = green;
    d.ddckCKSrcBlt.dwColorSpaceHighValue = green;
    Com<IDirectDrawSurface7> argb;
    CHECK(g_dd->CreateSurface(&d, &argb.p, NULL));
    memset(&ck, 0, sizeof(ck));
    CHECK(argb->GetColorKey(DDCKEY_SRCBLT, &ck));
    EXPECT(ck.dwColorSpaceLowValue == green && ck.dwColorSpaceHighValue == green, "key %08lx-%08lx",
           (unsigned long)ck.dwColorSpaceLowValue, (unsigned long)ck.dwColorSpaceHighValue);
    if (!write_texels(argb, 32, raw, 4, 4) || !use_texture(argb))
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE));
    if (!draw_texels(bg) || !expect_texels(keyed, bg)) {
        printf("    (A8R8G8B8, COLORKEYENABLE, key %08lx)\n", (unsigned long)green);
        return 0;
    }
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE));
    if (!draw_texels(bg) || !expect_texels(unkeyed, bg)) {
        printf("    (A8R8G8B8, COLORKEYENABLE FALSE)\n");
        return 0;
    }
    return 1;
}

/* 10. A palettized texture with an attached palette: colours come from the
 * palette; SetEntries and rewritten indices change the next draw.  A
 * video-memory texture where the device enumerates P8; otherwise QD3D
 * (--expect-local) samples a system-memory P8 texture through its palette,
 * which the stock HAL cannot. */
static void set_entry(PALETTEENTRY *e, DWORD argb)
{
    e->peRed = (BYTE)(argb >> 16);
    e->peGreen = (BYTE)(argb >> 8);
    e->peBlue = (BYTE)argb;
    e->peFlags = 0;
}

static int scene_p8(void)
{
    const DWORD bg = 0x102030;
    DWORD memory;
    if (texture_format_enumerated(&kP8)) {
        memory = DDSCAPS_VIDEOMEMORY;
    } else if (g_expect_local) {
        memory = DDSCAPS_SYSTEMMEMORY;
    } else {
        g_skip_reason = "HAL cannot sample system memory";
        return SKIP;
    }
    if (!base_state())
        return 0;
    PALETTEENTRY pal[256];
    DWORD index[16], rgb[16];
    memset(pal, 0, sizeof(pal));
    for (int t = 0; t < 16; ++t) {
        index[t] = (DWORD)t * 17;
        set_entry(&pal[index[t]], level_colour(t));
        rgb[t] = level_colour(t) & 0xffffff;
    }
    Com<IDirectDrawPalette> palette;
    CHECK(g_dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pal, &palette.p, NULL));
    Com<IDirectDrawSurface7> tex;
    CHECK(create_texture(&kP8, 4, 4, memory, 0, 0, &tex.p));
    printf("    P8 texture in %s memory\n", memory == DDSCAPS_VIDEOMEMORY ? "video" : "system");
    CHECK(tex->SetPalette(palette));
    if (!write_texels(tex, 8, index, 4, 4) || !use_texture(tex) || !draw_texels(bg) || !expect_texels(rgb, bg))
        return 0;
    for (int t = 0; t < 16; ++t) {
        set_entry(&pal[index[t]], level_colour((t + 7) % 27));
        rgb[t] = level_colour((t + 7) % 27) & 0xffffff;
    }
    CHECK(palette->SetEntries(0, 0, 256, pal));
    if (!draw_texels(bg) || !expect_texels(rgb, bg)) {
        printf("    (after SetEntries)\n");
        return 0;
    }
    DWORD index2[16], rgb2[16];
    for (int t = 0; t < 16; ++t) {
        index2[t] = index[15 - t];
        rgb2[t] = rgb[15 - t];
    }
    if (!write_texels(tex, 8, index2, 4, 4) || !draw_texels(bg) || !expect_texels(rgb2, bg)) {
        printf("    (after rewriting the indices)\n");
        return 0;
    }
    return 1;
}

/* 11. A three-level mipmap chain, each level a different colour: a quad
 * the size of level 0 shows level 0, a quarter-size quad level 2 (point mip
 * filter); without mip filtering both show level 0. */
static int scene_mipmap(void)
{
    const DWORD bg = 0x000000;
    static const DWORD colours[3] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};
    const Fmt *f = plain_format();
    if (!base_state())
        return 0;
    Com<IDirectDrawSurface7> tex;
    CHECK(create_texture(f, 32, 32, DDSCAPS_MIPMAP | DDSCAPS_COMPLEX, 0, 3, &tex.p));
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(tex->GetSurfaceDesc(&d));
    EXPECT((d.dwFlags & DDSD_MIPMAPCOUNT) && d.dwMipMapCount == 3, "flags 0x%lx, %lu levels",
           (unsigned long)d.dwFlags, (unsigned long)d.dwMipMapCount);
    IDirectDrawSurface7 *level = tex;
    level->AddRef();
    for (int l = 0; l < 3; ++l) {
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        HRESULT hr = level->GetSurfaceDesc(&d);
        int ok = SUCCEEDED(hr) && d.dwWidth == (32u >> l) && d.dwHeight == (32u >> l) &&
                 fill_texels(level, f->bits, format_pack(f, colours[l]), 32 >> l, 32 >> l);
        DDSCAPS2 caps;
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_MIPMAP;
        IDirectDrawSurface7 *next = NULL;
        hr = level->GetAttachedSurface(&caps, &next);
        level->Release();
        level = next;
        if (!ok) {
            printf("    level %d: %lux%lu\n", l, (unsigned long)d.dwWidth, (unsigned long)d.dwHeight);
            if (level)
                level->Release();
            return 0;
        }
        if (l < 2 && FAILED(hr)) {
            printf("    GetAttachedSurface(DDSCAPS_MIPMAP) on level %d: hr=0x%08lx\n", l, (unsigned long)hr);
            return 0;
        }
        if (l == 2 && hr != DDERR_NOTFOUND) {
            printf("    level 2 has an attached level: hr=0x%08lx\n", (unsigned long)hr);
            if (level)
                level->Release();
            return 0;
        }
    }
    if (!use_texture(tex))
        return 0;
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_POINT));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_tex_rect(8, 8, 40, 40));
    CHECK(draw_tex_rect(56, 8, 72, 24));
    CHECK(draw_tex_rect(88, 8, 96, 16));
    CHECK(g_dev->EndScene());
    Px p[] = {{24, 24, 0xff0000}, {10, 10, 0xff0000}, {37, 37, 0xff0000}, {64, 16, 0x00ff00},
              {58, 10, 0x00ff00}, {92, 12, 0x0000ff}, {48, 24, bg}};
    EXPECT_PIXELS(g_rt, p, 0);
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_tex_rect(56, 8, 72, 24));
    CHECK(draw_tex_rect(88, 8, 96, 16));
    CHECK(g_dev->EndScene());
    Px none[] = {{64, 16, 0xff0000}, {92, 12, 0xff0000}};
    EXPECT_PIXELS(g_rt, none, 0);
    return 1;
}

/* 12. (optional) A cube map, one colour per face, sampled with
 * three-component direction coordinates pointing at each face. */
static int scene_cubemap(void)
{
    const DWORD bg = 0x000000;
    static const DWORD colours[6] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u, 0xffff00ffu, 0xff00ffffu};
    static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    if (!(g_caps.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_CUBEMAP)) {
        g_skip_reason = "no D3DPTEXTURECAPS_CUBEMAP";
        return SKIP;
    }
    const Fmt *f = plain_format();
    if (!base_state())
        return 0;
    Com<IDirectDrawSurface7> cube;
    CHECK(create_texture(f, 4, 4, DDSCAPS_COMPLEX, DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES, 0, &cube.p));
    for (int i = 0; i < 6; ++i) {
        Com<IDirectDrawSurface7> face;
        if (i == 0) {
            face.p = cube;
            face->AddRef();
        } else {
            DDSCAPS2 caps;
            memset(&caps, 0, sizeof(caps));
            caps.dwCaps2 = DDSCAPS2_CUBEMAP | (DDSCAPS2_CUBEMAP_POSITIVEX << i);
            CHECK(cube->GetAttachedSurface(&caps, &face.p));
        }
        DDSURFACEDESC2 d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        CHECK(face->GetSurfaceDesc(&d));
        EXPECT(d.ddsCaps.dwCaps2 & (DDSCAPS2_CUBEMAP_POSITIVEX << i), "face %d has caps2 0x%08lx", i,
               (unsigned long)d.ddsCaps.dwCaps2);
        if (!fill_texels(face, f->bits, format_pack(f, colours[i]), 4, 4))
            return 0;
    }
    if (!use_texture(cube))
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    for (int i = 0; i < 6; ++i) {
        float x0 = 8.0f + 40.0f * (i % 3), y0 = 16.0f + 56.0f * (i / 3), x1 = x0 + 24.0f, y1 = y0 + 24.0f;
        const float *v = dirs[i];
        CUBEV q[4] = {{x0, y0, 0.5f, 1.0f, v[0], v[1], v[2]}, {x1, y0, 0.5f, 1.0f, v[0], v[1], v[2]},
                      {x0, y1, 0.5f, 1.0f, v[0], v[1], v[2]}, {x1, y1, 0.5f, 1.0f, v[0], v[1], v[2]}};
        CHECK(g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_CUBE, q, 4, 0));
    }
    CHECK(g_dev->EndScene());
    for (int i = 0; i < 6; ++i) {
        int x0 = 8 + 40 * (i % 3), y0 = 16 + 56 * (i / 3);
        if (!expect_rect(x0, y0, x0 + 24, y0 + 24, colours[i] & 0xffffff, bg, 0)) {
            printf("    face %d\n", i);
            return 0;
        }
    }
    return 1;
}

/* 13. A managed texture (DDSCAPS2_TEXTUREMANAGE): new texels after Lock,
 * still right after EvictManagedTextures, and lockable after eviction. */
static int scene_managed(void)
{
    const DWORD bg = 0x202020;
    const Fmt *f = plain_format();
    DWORD raw[2][16], rgb[2][16];
    if (!base_state())
        return 0;
    for (int s = 0; s < 2; ++s)
        for (int t = 0; t < 16; ++t) {
            raw[s][t] = format_pack(f, level_colour((t + 11 * s) % 27));
            rgb[s][t] = format_rgb(f, raw[s][t]);
        }
    Com<IDirectDrawSurface7> tex;
    CHECK(create_texture(f, 4, 4, 0, DDSCAPS2_TEXTUREMANAGE, 0, &tex.p));
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(tex->GetSurfaceDesc(&d));
    EXPECT(d.ddsCaps.dwCaps == (DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY) && d.ddsCaps.dwCaps2 == DDSCAPS2_TEXTUREMANAGE,
           "caps 0x%08lx, caps2 0x%08lx", (unsigned long)d.ddsCaps.dwCaps, (unsigned long)d.ddsCaps.dwCaps2);
    if (!write_texels(tex, f->bits, raw[0], 4, 4) || !use_texture(tex) || !draw_texels(bg) ||
        !expect_texels(rgb[0], bg))
        return 0;
    if (!write_texels(tex, f->bits, raw[1], 4, 4) || !draw_texels(bg) || !expect_texels(rgb[1], bg)) {
        printf("    (after rewriting the texels)\n");
        return 0;
    }
    CHECK(g_d3d->EvictManagedTextures());
    if (!draw_texels(bg) || !expect_texels(rgb[1], bg)) {
        printf("    (after EvictManagedTextures)\n");
        return 0;
    }
    CHECK(g_d3d->EvictManagedTextures());
    if (!write_texels(tex, f->bits, raw[0], 4, 4) || !draw_texels(bg) || !expect_texels(rgb[0], bg)) {
        printf("    (rewritten after EvictManagedTextures)\n");
        return 0;
    }
    return 1;
}

/* 14. Vertex buffers of each kind: description, discard lock, a draw from a
 * non-zero start vertex and an indexed draw whose indices count from it. */
static int scene_vb(void)
{
    const DWORD bg = 0x000000;
    static const DWORD caps[3] = {0, D3DVBCAPS_WRITEONLY, D3DVBCAPS_SYSTEMMEMORY};
    static const DWORD colours[3] = {0x00ff00, 0x0000ff, 0xffff00};
    WORD indices[6];
    memcpy(indices, kQuadIndices, sizeof(indices));
    for (int i = 0; i < 3; ++i) {
        if (!base_state())
            return 0;
        D3DVERTEXBUFFERDESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwCaps = caps[i];
        desc.dwFVF = FVF_TL;
        desc.dwNumVertices = 8;
        Com<IDirect3DVertexBuffer7> vb;
        CHECK(g_d3d->CreateVertexBuffer(&desc, &vb.p, 0));
        for (int pass = 0; pass < 2; ++pass) {
            D3DVERTEXBUFFERDESC got;
            memset(&got, 0xcc, sizeof(got));
            got.dwSize = pass ? sizeof(got) : 0;
            CHECK(vb->GetVertexBufferDesc(&got));
            EXPECT(got.dwSize == (pass ? sizeof(got) : 0) && got.dwCaps == caps[i] && got.dwFVF == FVF_TL &&
                       got.dwNumVertices == 8,
                   "caps 0x%lx: size %lu, caps 0x%lx, fvf 0x%lx, %lu vertices", (unsigned long)caps[i],
                   (unsigned long)got.dwSize, (unsigned long)got.dwCaps, (unsigned long)got.dwFVF,
                   (unsigned long)got.dwNumVertices);
        }
        void *data = NULL;
        DWORD size = 0;
        CHECK(vb->Lock(DDLOCK_DISCARDCONTENTS | DDLOCK_WAIT, &data, &size));
        EXPECT(size == 8 * sizeof(TLV), "Lock reported %lu bytes", (unsigned long)size);
        TLV *v = (TLV *)data;
        tl_rect(v, 0, 0, W, H, 0.5f, 0xffff0000u); /* vertices 0-3 must not be drawn */
        tl_rect(v + 4, 32, 32, 96, 96, 0.5f, 0xff000000u | colours[i]);
        CHECK(vb->Unlock());
        CHECK(clear(bg));
        CHECK(g_dev->BeginScene());
        CHECK(g_dev->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, vb, 4, 4, 0));
        CHECK(g_dev->EndScene());
        if (!expect_rect(32, 32, 96, 96, colours[i], bg, 0)) {
            printf("    DrawPrimitiveVB, caps 0x%lx\n", (unsigned long)caps[i]);
            return 0;
        }
        CHECK(clear(bg));
        CHECK(g_dev->BeginScene());
        CHECK(g_dev->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, vb, 4, 4, indices, 6, 0));
        CHECK(g_dev->EndScene());
        if (!expect_rect(32, 32, 96, 96, colours[i], bg, 0)) {
            printf("    DrawIndexedPrimitiveVB, caps 0x%lx\n", (unsigned long)caps[i]);
            return 0;
        }
        if (!caps[i]) {
            CHECK(vb->Lock(0xdeadbeef, &data, NULL));
            CHECK(vb->Unlock());
        }
    }
    return 1;
}

/* 15. Strided draws: positions and diffuse colours in separate arrays with
 * different strides. */
static int scene_strided(void)
{
    const DWORD bg = 0x000000;
    struct {
        float x, y, z, pad;
    } pos[4];
    struct {
        DWORD c, pad;
    } col[4];
    if (!base_state())
        return 0;
    LV v[4];
    lv_rect(v, 16, 16, 64, 64, 0.5f, 0xff00ff00u);
    for (int i = 0; i < 4; ++i) {
        pos[i].x = v[i].x;
        pos[i].y = v[i].y;
        pos[i].z = v[i].z;
        pos[i].pad = 99.0f;
        col[i].c = v[i].c;
        col[i].pad = 0xffff0000u;
    }
    D3DDRAWPRIMITIVESTRIDEDDATA sd;
    memset(&sd, 0, sizeof(sd));
    sd.position.lpvData = pos;
    sd.position.dwStride = sizeof(pos[0]);
    sd.diffuse.lpvData = col;
    sd.diffuse.dwStride = sizeof(col[0]);
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitiveStrided(D3DPT_TRIANGLESTRIP, FVF_LV, &sd, 4, 0));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(16, 16, 64, 64, 0x00ff00, bg, 0);

    lv_rect(v, 64, 72, 120, 112, 0.5f, 0xffff8000u);
    for (int i = 0; i < 4; ++i) {
        pos[i].x = v[i].x;
        pos[i].y = v[i].y;
        col[i].c = v[i].c;
    }
    WORD indices[6];
    memcpy(indices, kQuadIndices, sizeof(indices));
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawIndexedPrimitiveStrided(D3DPT_TRIANGLELIST, FVF_LV, &sd, 4, indices, 6, 0));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(64, 72, 120, 112, 0xff8000, bg, 0);
    EXPECT_RECT(16, 16, 64, 64, 0x00ff00, bg, 0);
    return 1;
}

/* 16. ProcessVertices: D3DVOP_TRANSFORM into XYZRHW against the matrices and
 * viewport (relative 1e-3), D3DVOP_LIGHT into XYZRHW | DIFFUSE against the
 * lighting equation (+-1); both outputs drawn with DrawPrimitiveVB. */
static void expect_vertex(const D3DMATRIX &wvp, const D3DVIEWPORT7 &vp, const float *in, float *out)
{
    float c[4];
    const float *m = &wvp._11;
    for (int j = 0; j < 4; ++j)
        c[j] = in[0] * m[j] + in[1] * m[4 + j] + in[2] * m[8 + j] + m[12 + j];
    out[0] = vp.dwX + (1.0f + c[0] / c[3]) * vp.dwWidth / 2.0f;
    out[1] = vp.dwY + (1.0f - c[1] / c[3]) * vp.dwHeight / 2.0f;
    out[2] = vp.dvMinZ + c[2] / c[3] * (vp.dvMaxZ - vp.dvMinZ);
    out[3] = 1.0f / c[3];
}

static int vertex_close(const float *got, const float *want)
{
    for (int k = 0; k < 4; ++k) {
        float tol = 1e-3f * (absf(want[k]) > 1.0f ? absf(want[k]) : 1.0f);
        if (absf(got[k] - want[k]) > tol)
            return 0;
    }
    return 1;
}

static int check_processed(IDirect3DVertexBuffer7 *dst, DWORD stride, int count, const float (*src)[3],
                           const D3DMATRIX &wvp, const D3DVIEWPORT7 &vp, DWORD want_diffuse)
{
    void *data = NULL;
    CHECK(dst->Lock(DDLOCK_READONLY | DDLOCK_WAIT, &data, NULL));
    int ok = 1;
    for (int i = 0; i < count; ++i) {
        const float *got = (const float *)((const BYTE *)data + i * stride);
        float want[4];
        expect_vertex(wvp, vp, src[i], want);
        if (!vertex_close(got, want)) {
            printf("    vertex %d: %g %g %g %g, want %g %g %g %g\n", i, got[0], got[1], got[2], got[3], want[0],
                   want[1], want[2], want[3]);
            ok = 0;
        }
        if (want_diffuse) {
            DWORD c = *(const DWORD *)(got + 4);
            if ((c >> 24) != (want_diffuse >> 24) || !rgb_near(c, want_diffuse, 1)) {
                printf("    vertex %d diffuse %08lx, want %08lx +-1\n", i, (unsigned long)c,
                       (unsigned long)want_diffuse);
                ok = 0;
            }
        }
    }
    CHECK(dst->Unlock());
    return ok;
}

static HRESULT create_vb(DWORD fvf, DWORD count, IDirect3DVertexBuffer7 **vb)
{
    D3DVERTEXBUFFERDESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFVF = fvf;
    desc.dwNumVertices = count;
    return g_d3d->CreateVertexBuffer(&desc, vb, 0);
}

static int scene_processvertices(void)
{
    const DWORD bg = 0x000000;
    const float q = 10.0f / 9.0f;
    /* Rotation by 90 degrees about z, then (0.75, 0.75, 0); camera 3 units
     * back; 1.5x perspective.  The z = 0 quad lands on pixels [56, 120) x
     * [8, 72) of the full viewport. */
    const D3DMATRIX world = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.75f, 0.75f, 0, 1};
    const D3DMATRIX view = translation(0.0f, 0.0f, 3.0f);
    const D3DMATRIX proj = {1.5f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, q, 1, 0, 0, -q, 0};
    const D3DMATRIX wvp = mat_mul(mat_mul(world, view), proj);
    static const float src[8][3] = {{-1, 1, 0},          {1, 1, 0},           {-1, -1, 0},       {1, -1, 0},
                                    {0.5f, -0.25f, 0.5f}, {-0.75f, 0.5f, -1}, {0.25f, 0.25f, 2}, {-1, 1, 1}};
    if (!base_state())
        return 0;
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, world) || !set_transform(g_dev, D3DTRANSFORMSTATE_VIEW, view) ||
        !set_transform(g_dev, D3DTRANSFORMSTATE_PROJECTION, proj))
        return 0;
    Com<IDirect3DVertexBuffer7> in, out;
    CHECK(create_vb(D3DFVF_XYZ, 8, &in.p));
    CHECK(create_vb(D3DFVF_XYZRHW, 8, &out.p));
    void *data = NULL;
    CHECK(in->Lock(DDLOCK_WAIT, &data, NULL));
    memcpy(data, src, sizeof(src));
    CHECK(in->Unlock());

    D3DVIEWPORT7 vp = viewport(16, 8, 96, 112, 0.25f, 0.75f);
    CHECK(g_dev->SetViewport(&vp));
    CHECK(out->ProcessVertices(D3DVOP_TRANSFORM, 0, 8, in, 0, g_dev, 0));
    if (!check_processed(out, 16, 8, src, wvp, vp, 0)) {
        printf("    (viewport 16,8 96x112, z 0.25-0.75)\n");
        return 0;
    }
    vp = viewport(0, 0, W, H, 0.0f, 1.0f);
    CHECK(g_dev->SetViewport(&vp));
    CHECK(out->ProcessVertices(D3DVOP_TRANSFORM, 0, 8, in, 0, g_dev, 0));
    if (!check_processed(out, 16, 8, src, wvp, vp, 0))
        return 0;
    /* XYZRHW without colour: stage 0 takes the texture factor. */
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0xff00c0c0u));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, out, 0, 4, 0));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(56, 8, 120, 72, 0x00c0c0, bg, 0);

    /* Lit: the rotation leaves the (0, 0, -1) normals alone. */
    Com<IDirect3DVertexBuffer7> nin, lit;
    CHECK(create_vb(FVF_N, 4, &nin.p));
    CHECK(create_vb(D3DFVF_XYZRHW | D3DFVF_DIFFUSE, 4, &lit.p));
    CHECK(nin->Lock(DDLOCK_WAIT, &data, NULL));
    NV *nv = (NV *)data;
    for (int i = 0; i < 4; ++i) {
        NV v = {src[i][0], src[i][1], src[i][2], 0, 0, -1};
        nv[i] = v;
    }
    CHECK(nin->Unlock());
    if (!set_lighting())
        return 0;
    CHECK(lit->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_LIGHT, 0, 4, nin, 0, g_dev, 0));
    if (!check_processed(lit, 20, 4, src, wvp, vp, 0xff000000u | lit_rgb(kMatDiffuse, 1)))
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitiveVB(D3DPT_TRIANGLESTRIP, lit, 0, 4, 0));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(56, 8, 120, 72, lit_rgb(kMatDiffuse, 1), bg, 1);
    return 1;
}

/* 17. State blocks: recording, applying, capturing, deleting, and what
 * D3DSBT_ALL / PIXELSTATE / VERTEXSTATE capture. */
enum {
    SB_ALPHABLENDENABLE,
    SB_ZENABLE,
    SB_FOGCOLOR,
    SB_FOGENABLE,
    SB_LIGHTING,
    SB_AMBIENT,
    SB_COLOROP,
    SB_ADDRESSU,
    SB_TEXCOORDINDEX,
    SB_TEXTURE,
    SB_LIGHT,
    SB_LIGHTENABLE,
    SB_WORLD,
    SB_MATERIAL,
    SB_VIEWPORT,
    SB_CLIPPLANE,
    SB_COUNT
};

static const char *const kSbNames[SB_COUNT] = {
    "ALPHABLENDENABLE", "ZENABLE",  "FOGCOLOR", "FOGENABLE",   "LIGHTING", "AMBIENT",  "TSS COLOROP",
    "TSS ADDRESSU",     "TSS TEXCOORDINDEX", "texture", "light 3", "light 3 enable", "world transform",
    "material",         "viewport", "clip plane 0",
};

/* Puts state `probe` in variant v (0 or 1). */
static int sb_set(int probe, int v, IDirectDrawSurface7 *tex)
{
    HRESULT hr = E_FAIL;
    switch (probe) {
    case SB_ALPHABLENDENABLE:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, v ? FALSE : TRUE);
        break;
    case SB_ZENABLE:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_ZENABLE, v ? D3DZB_FALSE : D3DZB_TRUE);
        break;
    case SB_FOGCOLOR:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_FOGCOLOR, v ? 0x445566 : 0x112233);
        break;
    case SB_FOGENABLE:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, v ? FALSE : TRUE);
        break;
    case SB_LIGHTING:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_LIGHTING, v ? TRUE : FALSE);
        break;
    case SB_AMBIENT:
        hr = g_dev->SetRenderState(D3DRENDERSTATE_AMBIENT, v ? 0x202020 : 0x101010);
        break;
    case SB_COLOROP:
        hr = g_dev->SetTextureStageState(0, D3DTSS_COLOROP, v ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
        break;
    case SB_ADDRESSU:
        hr = g_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, v ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        break;
    case SB_TEXCOORDINDEX:
        hr = g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, v ? 0 : 1);
        break;
    case SB_TEXTURE:
        hr = g_dev->SetTexture(0, v ? NULL : tex);
        break;
    case SB_LIGHT: {
        D3DLIGHT7 l;
        memset(&l, 0, sizeof(l));
        l.dltType = D3DLIGHT_DIRECTIONAL;
        l.dcvDiffuse = colour_value(v ? 0.75f : 0.25f, 0.0f, 0.0f, 1.0f);
        l.dvDirection = vec(0.0f, 0.0f, 1.0f);
        hr = g_dev->SetLight(3, &l);
        break;
    }
    case SB_LIGHTENABLE:
        hr = g_dev->LightEnable(3, v ? FALSE : TRUE);
        break;
    case SB_WORLD: {
        D3DMATRIX m = translation(v ? 2.0f : 1.0f, 0.0f, 0.0f);
        hr = g_dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &m);
        break;
    }
    case SB_MATERIAL: {
        D3DMATERIAL7 m;
        memset(&m, 0, sizeof(m));
        m.diffuse = colour_value(v ? 0.75f : 0.25f, 0.0f, 0.0f, 1.0f);
        hr = g_dev->SetMaterial(&m);
        break;
    }
    case SB_VIEWPORT: {
        D3DVIEWPORT7 vp = v ? viewport(8, 8, 64, 64, 0.0f, 1.0f) : viewport(0, 0, W, H, 0.0f, 1.0f);
        hr = g_dev->SetViewport(&vp);
        break;
    }
    case SB_CLIPPLANE: {
        float p[4] = {1.0f, 0.0f, 0.0f, v ? 0.75f : 0.25f};
        hr = g_dev->SetClipPlane(0, p);
        break;
    }
    }
    if (FAILED(hr)) {
        printf("    setting %s failed: hr=0x%08lx\n", kSbNames[probe], (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* The variant state `probe` is in, -1 for neither. */
static int sb_get(int probe, IDirectDrawSurface7 *tex)
{
    DWORD d = 0;
    HRESULT hr = E_FAIL;
    switch (probe) {
    case SB_ALPHABLENDENABLE:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &d);
        return FAILED(hr) ? -1 : d == TRUE ? 0 : d == FALSE ? 1 : -1;
    case SB_ZENABLE:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_ZENABLE, &d);
        return FAILED(hr) ? -1 : d == D3DZB_TRUE ? 0 : d == D3DZB_FALSE ? 1 : -1;
    case SB_FOGCOLOR:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_FOGCOLOR, &d);
        return FAILED(hr) ? -1 : d == 0x112233 ? 0 : d == 0x445566 ? 1 : -1;
    case SB_FOGENABLE:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_FOGENABLE, &d);
        return FAILED(hr) ? -1 : d == TRUE ? 0 : d == FALSE ? 1 : -1;
    case SB_LIGHTING:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_LIGHTING, &d);
        return FAILED(hr) ? -1 : d == FALSE ? 0 : d == TRUE ? 1 : -1;
    case SB_AMBIENT:
        hr = g_dev->GetRenderState(D3DRENDERSTATE_AMBIENT, &d);
        return FAILED(hr) ? -1 : d == 0x101010 ? 0 : d == 0x202020 ? 1 : -1;
    case SB_COLOROP:
        hr = g_dev->GetTextureStageState(0, D3DTSS_COLOROP, &d);
        return FAILED(hr) ? -1 : d == D3DTOP_MODULATE ? 0 : d == D3DTOP_SELECTARG1 ? 1 : -1;
    case SB_ADDRESSU:
        hr = g_dev->GetTextureStageState(0, D3DTSS_ADDRESSU, &d);
        return FAILED(hr) ? -1 : d == D3DTADDRESS_CLAMP ? 0 : d == D3DTADDRESS_WRAP ? 1 : -1;
    case SB_TEXCOORDINDEX:
        hr = g_dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &d);
        return FAILED(hr) ? -1 : d == 1 ? 0 : d == 0 ? 1 : -1;
    case SB_TEXTURE: {
        IDirectDrawSurface7 *t = NULL;
        if (FAILED(g_dev->GetTexture(0, &t)))
            return -1;
        if (t)
            t->Release();
        return t == tex ? 0 : !t ? 1 : -1;
    }
    case SB_LIGHT: {
        D3DLIGHT7 l;
        if (FAILED(g_dev->GetLight(3, &l)))
            return -1;
        return l.dcvDiffuse.r == 0.25f ? 0 : l.dcvDiffuse.r == 0.75f ? 1 : -1;
    }
    case SB_LIGHTENABLE: {
        BOOL on = FALSE;
        if (FAILED(g_dev->GetLightEnable(3, &on)))
            return -1;
        return on ? 0 : 1;
    }
    case SB_WORLD: {
        D3DMATRIX m;
        if (FAILED(g_dev->GetTransform(D3DTRANSFORMSTATE_WORLD, &m)))
            return -1;
        return m._41 == 1.0f ? 0 : m._41 == 2.0f ? 1 : -1;
    }
    case SB_MATERIAL: {
        D3DMATERIAL7 m;
        if (FAILED(g_dev->GetMaterial(&m)))
            return -1;
        return m.diffuse.r == 0.25f ? 0 : m.diffuse.r == 0.75f ? 1 : -1;
    }
    case SB_VIEWPORT: {
        D3DVIEWPORT7 vp;
        if (FAILED(g_dev->GetViewport(&vp)))
            return -1;
        return vp.dwX == 0 ? 0 : vp.dwX == 8 ? 1 : -1;
    }
    case SB_CLIPPLANE: {
        float p[4];
        if (FAILED(g_dev->GetClipPlane(0, p)))
            return -1;
        return p[3] == 0.25f ? 0 : p[3] == 0.75f ? 1 : -1;
    }
    }
    return -1;
}

/* Which probes a CreateStateBlock(type) block restores, as a bit mask. */
static int sb_coverage(D3DSTATEBLOCKTYPE type, IDirectDrawSurface7 *tex, DWORD *mask)
{
    DWORD h = 0;
    for (int p = 0; p < SB_COUNT; ++p)
        if (!sb_set(p, 0, tex))
            return 0;
    CHECK(g_dev->CreateStateBlock(type, &h));
    for (int p = 0; p < SB_COUNT; ++p)
        if (!sb_set(p, 1, tex))
            return 0;
    CHECK(g_dev->ApplyStateBlock(h));
    CHECK(g_dev->DeleteStateBlock(h));
    *mask = 0;
    for (int p = 0; p < SB_COUNT; ++p) {
        int v = sb_get(p, tex);
        if (v < 0) {
            printf("    %s is in neither variant after ApplyStateBlock\n", kSbNames[p]);
            return 0;
        }
        if (!v)
            *mask |= 1u << p;
    }
    return 1;
}

static void print_sb_mask(const char *what, DWORD mask)
{
    printf("    %s restores:", what);
    for (int p = 0; p < SB_COUNT; ++p)
        if (mask & (1u << p))
            printf(" %s,", kSbNames[p]);
    printf("\n");
}

/* The state the recorded block below changes, as base_state leaves it. */
static int sb_recorded_absent(void)
{
    DWORD abe = 0, src = 0, dst = 0, arg = 0;
    IDirectDrawSurface7 *t = NULL;
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &abe));
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_SRCBLEND, &src));
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_DESTBLEND, &dst));
    CHECK(g_dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg));
    CHECK(g_dev->GetTexture(0, &t));
    if (t)
        t->Release();
    EXPECT(abe == FALSE && src == D3DBLEND_ONE && dst == D3DBLEND_ZERO && arg == D3DTA_DIFFUSE && !t &&
               transform_is(D3DTRANSFORMSTATE_WORLD, kIdentity),
           "recorded state already applied: ALPHABLENDENABLE %lu, blend %lu/%lu, COLORARG1 %lu, texture %p",
           (unsigned long)abe, (unsigned long)src, (unsigned long)dst, (unsigned long)arg, (void *)t);
    return 1;
}

/* A textured quad in model space over pixels [16, 48) x [48, 80). */
static HRESULT draw_lt_quad(void)
{
    float l = ndc_x(16), r = ndc_x(48), t = ndc_y(48), b = ndc_y(80);
    LT q[4] = {{l, t, 0.5f, 0, 0}, {r, t, 0.5f, 1, 0}, {l, b, 0.5f, 0, 1}, {r, b, 0.5f, 1, 1}};
    return g_dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_LT, q, 4, 0);
}

static int scene_stateblock(void)
{
    const DWORD bg = 0x200000;
    const Fmt *f = plain_format();
    if (!base_state())
        return 0;
    Com<IDirectDrawSurface7> tex;
    CHECK(create_texture(f, 4, 4, 0, 0, 0, &tex.p));
    if (!fill_texels(tex, f->bits, format_pack(f, 0xff0000ffu), 4, 4))
        return 0;

    DWORD all = 0, rec = 0, h = 0xdeadbeef;
    CHECK(g_dev->CreateStateBlock(D3DSBT_ALL, &all)); /* the base state */
    CHECK(g_dev->BeginStateBlock());
    EXPECT_HR(g_dev->BeginStateBlock(), D3DERR_INBEGINSTATEBLOCK);
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE));
    CHECK(g_dev->SetTexture(0, tex));
    CHECK(g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    if (!set_transform(g_dev, D3DTRANSFORMSTATE_WORLD, translation(1.0f, 0.0f, 0.0f)))
        return 0;
    if (!sb_recorded_absent())
        return 0;
    EXPECT_HR(g_dev->ApplyStateBlock(all), D3DERR_INBEGINSTATEBLOCK);
    EXPECT_HR(g_dev->CaptureStateBlock(all), D3DERR_INBEGINSTATEBLOCK);
    EXPECT_HR(g_dev->CreateStateBlock(D3DSBT_ALL, &h), D3DERR_INBEGINSTATEBLOCK);
    CHECK(g_dev->EndStateBlock(&rec));
    EXPECT(rec != 0, "EndStateBlock returned handle 0");
    h = 0xdeadbeef;
    EXPECT_HR(g_dev->EndStateBlock(&h), D3DERR_NOTINBEGINSTATEBLOCK);
    EXPECT(h == 0xdeadbeef, "the failed EndStateBlock wrote handle 0x%lx", (unsigned long)h);
    if (!sb_recorded_absent())
        return 0;

    /* Applied, the recorded block adds the texture (blue) to the background
     * one unit to the right: pixels [80, 112) x [48, 80). */
    CHECK(g_dev->ApplyStateBlock(rec));
    DWORD abe = 0, arg = 0;
    IDirectDrawSurface7 *t = NULL;
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &abe));
    CHECK(g_dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg));
    CHECK(g_dev->GetTexture(0, &t));
    if (t)
        t->Release();
    EXPECT(abe == TRUE && arg == D3DTA_TEXTURE && t == tex &&
               transform_is(D3DTRANSFORMSTATE_WORLD, translation(1.0f, 0.0f, 0.0f)),
           "after ApplyStateBlock: ALPHABLENDENABLE %lu, COLORARG1 %lu, texture %p", (unsigned long)abe,
           (unsigned long)arg, (void *)t);
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_lt_quad());
    CHECK(g_dev->EndScene());
    Px p[] = {{96, 64, 0x2000ff}, {83, 51, 0x2000ff}, {108, 76, 0x2000ff}, {32, 64, bg}, {76, 64, bg}};
    EXPECT_PIXELS(g_rt, p, 1);

    /* Capturing into a recorded block updates only what it recorded. */
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE));
    CHECK(g_dev->CaptureStateBlock(rec));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE));
    CHECK(g_dev->ApplyStateBlock(rec));
    DWORD lighting = 0;
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &abe));
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_LIGHTING, &lighting));
    EXPECT(abe == FALSE && lighting == FALSE, "after capture and apply: ALPHABLENDENABLE %lu, LIGHTING %lu",
           (unsigned long)abe, (unsigned long)lighting);

    /* The D3DSBT_ALL block restores the base state ... */
    CHECK(g_dev->ApplyStateBlock(all));
    if (!sb_recorded_absent())
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(16, 16, 48, 48, 0.5f, 0xff00ff00u));
    CHECK(g_dev->EndScene());
    EXPECT_RECT(16, 16, 48, 48, 0x00ff00, bg, 0);
    /* ... and after CaptureStateBlock the state captured then. */
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0x00abcdef));
    CHECK(g_dev->CaptureStateBlock(all));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0x00fedcba));
    CHECK(g_dev->ApplyStateBlock(all));
    DWORD factor = 0;
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, &factor));
    EXPECT(factor == 0x00abcdef, "TEXTUREFACTOR 0x%08lx after capture and apply", (unsigned long)factor);
    CHECK(g_dev->DeleteStateBlock(all));
    CHECK(g_dev->DeleteStateBlock(rec));
    /* Stale handles: Delete and Capture refuse them, Apply ignores them. */
    EXPECT_HR(g_dev->DeleteStateBlock(rec), D3DERR_INVALIDSTATEBLOCK);
    EXPECT_HR(g_dev->CaptureStateBlock(rec), D3DERR_INVALIDSTATEBLOCK);
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0x00fedcba));
    EXPECT_HR(g_dev->ApplyStateBlock(all), D3D_OK);
    CHECK(g_dev->GetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, &factor));
    EXPECT(factor == 0x00fedcba, "applying a deleted block set TEXTUREFACTOR 0x%08lx", (unsigned long)factor);

    /* Coverage, as the stock runtime has it: FOGCOLOR and FOGENABLE are
     * vertex state only, TEXCOORDINDEX is both, a vertex state block keeps
     * a light's enable but not its parameters, and neither partial kind
     * keeps the texture, transforms, material, viewport or clip planes. */
    static const struct {
        D3DSTATEBLOCKTYPE type;
        const char *name;
        DWORD want;
    } cover[] = {
        {D3DSBT_ALL, "D3DSBT_ALL", (1u << SB_COUNT) - 1},
        {D3DSBT_PIXELSTATE, "D3DSBT_PIXELSTATE",
         1u << SB_ALPHABLENDENABLE | 1u << SB_ZENABLE | 1u << SB_COLOROP | 1u << SB_ADDRESSU |
             1u << SB_TEXCOORDINDEX},
        {D3DSBT_VERTEXSTATE, "D3DSBT_VERTEXSTATE",
         1u << SB_FOGCOLOR | 1u << SB_FOGENABLE | 1u << SB_LIGHTING | 1u << SB_AMBIENT | 1u << SB_TEXCOORDINDEX |
             1u << SB_LIGHTENABLE},
    };
    for (size_t i = 0; i < sizeof(cover) / sizeof(cover[0]); ++i) {
        DWORD mask = 0;
        if (!sb_coverage(cover[i].type, tex, &mask))
            return 0;
        if (mask != cover[i].want) {
            print_sb_mask(cover[i].name, mask);
            print_sb_mask("    expected", cover[i].want);
            return 0;
        }
    }
    return 1;
}

/* 18. Render to texture: SetRenderTarget / GetRenderTarget identity and
 * references, then the texture drawn texel-exact. */
static int scene_rtt(void)
{
    const DWORD bg = 0x101010;
    static const DWORD quads[4] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
    if (!base_state())
        return 0;
    Com<IDirectDrawSurface7> rtt;
    CHECK(create_target(g_dd, 64, 64, DDSCAPS_TEXTURE, &rtt.p));
    ULONG rt_refs = refs(g_rt), tex_refs = refs(rtt);
    IDirectDrawSurface7 *cur = NULL;
    CHECK(g_dev->GetRenderTarget(&cur));
    EXPECT(cur == g_rt, "GetRenderTarget returned %p, not %p", (void *)cur, (void *)g_rt);
    EXPECT(refs(g_rt) == rt_refs + 1, "GetRenderTarget took %ld references", (long)(refs(g_rt) - rt_refs));
    cur->Release();

    CHECK(g_dev->SetRenderTarget(rtt, 0));
    EXPECT(refs(rtt) == tex_refs + 1 && refs(g_rt) == rt_refs - 1,
           "after SetRenderTarget: new target %+ld references, old %+ld", (long)(refs(rtt) - tex_refs),
           (long)(refs(g_rt) - rt_refs));
    CHECK(g_dev->GetRenderTarget(&cur));
    int same = cur == rtt;
    cur->Release();
    EXPECT(same, "GetRenderTarget did not return the new target");
    if (!set_viewport(0, 0, 64, 64, 0.0f, 1.0f))
        return 0;
    CHECK(g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0));
    CHECK(g_dev->BeginScene());
    for (int i = 0; i < 4; ++i)
        CHECK(draw_rect(32.0f * (i % 2), 32.0f * (i / 2), 32.0f * (i % 2) + 32, 32.0f * (i / 2) + 32, 0.5f,
                        0xff000000u | quads[i]));
    CHECK(g_dev->EndScene());
    CHECK(g_dev->SetRenderTarget(g_rt, 0));
    EXPECT(refs(rtt) == tex_refs && refs(g_rt) == rt_refs, "after switching back: %+ld / %+ld references",
           (long)(refs(rtt) - tex_refs), (long)(refs(g_rt) - rt_refs));
    if (!set_viewport(0, 0, W, H, 0.0f, 1.0f))
        return 0;

    Px direct[] = {{2, 2, quads[0]}, {29, 29, quads[0]}, {34, 2, quads[1]}, {61, 29, quads[1]},
                   {2, 34, quads[2]}, {29, 61, quads[2]}, {34, 34, quads[3]}, {61, 61, quads[3]}};
    EXPECT_PIXELS(rtt, direct, 0);
    if (!use_texture(rtt))
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_tex_rect(32, 32, 96, 96));
    CHECK(g_dev->EndScene());
    for (int i = 0; i < 4; ++i) {
        int x0 = 32 + 32 * (i % 2), y0 = 32 + 32 * (i / 2);
        Px p[] = {{x0 + 2, y0 + 2, quads[i]}, {x0 + 29, y0 + 29, quads[i]}, {x0 + 16, y0 + 16, quads[i]}};
        EXPECT_PIXELS(g_rt, p, 0);
    }
    Px out[] = {{29, 64, bg}, {98, 64, bg}};
    EXPECT_PIXELS(g_rt, out, 0);
    return 1;
}

/* 19. The render target written by the CPU between draws: CPU pixels
 * survive later draws, and blending reads them. */
static int scene_rtlock(void)
{
    const DWORD bg = 0x202020, cpu = 0x336699, green = 0x00ff00;
    if (!base_state())
        return 0;
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(8, 8, 56, 56, 0.5f, 0xffff0000u));
    CHECK(g_dev->EndScene());

    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    CHECK(g_rt->Lock(NULL, &d, DDLOCK_WAIT, NULL));
    for (int y = 0; y < 32; ++y) {
        DWORD *row = (DWORD *)((BYTE *)d.lpSurface + (8 + y) * d.lPitch);
        for (int x = 64; x < 96; ++x)
            row[x] = cpu;
    }
    for (int y = 24; y < 40; ++y) {
        DWORD *row = (DWORD *)((BYTE *)d.lpSurface + y * d.lPitch);
        for (int x = 24; x < 40; ++x)
            row[x] = green;
    }
    CHECK(g_rt->Unlock(NULL));

    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(72, 72, 120, 120, 0.5f, 0xff0000ffu));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE));
    CHECK(draw_rect(64, 16, 96, 32, 0.5f, 0xff402010u));
    CHECK(g_dev->EndScene());
    Px exact[] = {{10, 10, 0xff0000}, {53, 53, 0xff0000}, {26, 26, green}, {37, 37, green}, {44, 30, 0xff0000},
                  {66, 10, cpu},      {93, 37, cpu},      {80, 12, cpu},   {80, 36, cpu},    {75, 75, 0x0000ff},
                  {117, 117, 0x0000ff}, {100, 60, bg},    {60, 100, bg}};
    EXPECT_PIXELS(g_rt, exact, 0);
    Px blended[] = {{66, 18, 0x7386a9}, {93, 29, 0x7386a9}, {80, 24, 0x7386a9}};
    EXPECT_PIXELS(g_rt, blended, 1);
    return 1;
}

/* 20. Render, texture stage, light, material, clip plane and viewport state:
 * defaults of a fresh device, round trips, rejected states and the error
 * codes of misuse. */

/* GetRenderState on a fresh device, stock runtime, as ranges of states with
 * one default each.  Render states 1-152 not listed fail with
 * DDERR_INVALIDPARAMS and leave the output alone: TEXTUREHANDLE,
 * TEXTUREADDRESS, WRAPU/V, MONOENABLE, ROP2, PLANEMASK, TEXTUREMAG/MIN,
 * TEXTUREMAPBLEND, SUBPIXEL(X), STIPPLEENABLE, BORDERCOLOR,
 * TEXTUREADDRESSU/V, MIPMAPLODBIAS, ANISOTROPY, TRANSLUCENTSORTINDEPENDENT
 * and STIPPLEPATTERN00-31.  The 0xffffffff entries (ANTIALIAS,
 * EDGEANTIALIAS, FLUSHBATCH and the unnamed states) are what the stock
 * runtime returns, not a Direct3D 7 default. */
#define RS(name, value) {D3DRENDERSTATE_##name, D3DRENDERSTATE_##name, value}
static const struct {
    DWORD first, last, value;
} kRsDefaults[] = {
    RS(ANTIALIAS, 0xffffffff),
    RS(TEXTUREPERSPECTIVE, TRUE),
    RS(ZENABLE, D3DZB_TRUE),
    RS(FILLMODE, D3DFILL_SOLID),
    RS(SHADEMODE, D3DSHADE_GOURAUD),
    RS(LINEPATTERN, 0),
    RS(ZWRITEENABLE, TRUE),
    RS(ALPHATESTENABLE, FALSE),
    RS(LASTPIXEL, TRUE),
    RS(SRCBLEND, D3DBLEND_ONE),
    RS(DESTBLEND, D3DBLEND_ZERO),
    RS(CULLMODE, D3DCULL_CCW),
    RS(ZFUNC, D3DCMP_LESSEQUAL),
    RS(ALPHAREF, 0),
    RS(ALPHAFUNC, D3DCMP_ALWAYS),
    RS(DITHERENABLE, FALSE),
    RS(ALPHABLENDENABLE, FALSE),
    RS(FOGENABLE, FALSE),
    RS(SPECULARENABLE, FALSE),
    RS(ZVISIBLE, FALSE),
    RS(STIPPLEDALPHA, FALSE),
    RS(FOGCOLOR, 0),
    RS(FOGTABLEMODE, D3DFOG_NONE),
    RS(FOGSTART, 0),          /* 0.0f */
    RS(FOGEND, 0x3f800000),   /* 1.0f */
    RS(FOGDENSITY, 0x3f800000), /* 1.0f */
    RS(EDGEANTIALIAS, 0xffffffff),
    RS(COLORKEYENABLE, FALSE),
    {42, 42, 0xffffffff},
    RS(ZBIAS, 0),
    RS(RANGEFOGENABLE, FALSE),
    RS(FLUSHBATCH, 0xffffffff),
    RS(STENCILENABLE, FALSE),
    RS(STENCILFAIL, D3DSTENCILOP_KEEP),
    RS(STENCILZFAIL, D3DSTENCILOP_KEEP),
    RS(STENCILPASS, D3DSTENCILOP_KEEP),
    RS(STENCILFUNC, D3DCMP_ALWAYS),
    RS(STENCILREF, 0),
    RS(STENCILMASK, 0xffffffff),
    RS(STENCILWRITEMASK, 0xffffffff),
    RS(TEXTUREFACTOR, 0xffffffff),
    {61, 63, 0xffffffff},
    {96, 127, 0xffffffff},
    {D3DRENDERSTATE_WRAP0, D3DRENDERSTATE_WRAP7, 0},
    RS(CLIPPING, TRUE),
    RS(LIGHTING, TRUE),
    RS(EXTENTS, FALSE),
    RS(AMBIENT, 0),
    RS(FOGVERTEXMODE, D3DFOG_NONE),
    RS(COLORVERTEX, TRUE),
    RS(LOCALVIEWER, TRUE),
    RS(NORMALIZENORMALS, FALSE),
    RS(COLORKEYBLENDENABLE, FALSE),
    RS(DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1),
    RS(SPECULARMATERIALSOURCE, D3DMCS_COLOR2),
    RS(AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL),
    RS(EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL),
    {149, 150, 0xffffffff},
    RS(VERTEXBLEND, D3DVBLEND_DISABLE),
    RS(CLIPPLANEENABLE, 0),
};
#undef RS

/* GetTextureStageState defaults, D3DTSS_COLOROP (1) to
 * D3DTSS_TEXTURETRANSFORMFLAGS (24), stages 0 and 1.  Stage 1's
 * TEXCOORDINDEX is 0, not 1 as in Direct3D 8/9. */
#define TSS_DEFAULTS(colorop, alphaop)                                                              \
    {                                                                                               \
        colorop, D3DTA_TEXTURE, D3DTA_CURRENT, alphaop, D3DTA_TEXTURE, D3DTA_CURRENT, 0, 0, 0, 0,   \
            0 /* TEXCOORDINDEX */, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP,            \
            0 /* BORDERCOLOR */, D3DTFG_POINT, D3DTFN_POINT, D3DTFP_NONE, 0 /* MIPMAPLODBIAS */,    \
            0 /* MAXMIPLEVEL */, 1 /* MAXANISOTROPY */, 0, 0, D3DTTFF_DISABLE                       \
    }
static const DWORD kTssDefaults[2][24] = {
    TSS_DEFAULTS(D3DTOP_MODULATE, D3DTOP_SELECTARG1),
    TSS_DEFAULTS(D3DTOP_DISABLE, D3DTOP_DISABLE),
};
#undef TSS_DEFAULTS

static int check_defaults(IDirect3DDevice7 *dev)
{
    int ok = 1;
    for (DWORD s = 1; s <= 152; ++s) {
        int listed = -1;
        for (size_t i = 0; i < sizeof(kRsDefaults) / sizeof(kRsDefaults[0]); ++i)
            if (kRsDefaults[i].first <= s && s <= kRsDefaults[i].last)
                listed = (int)i;
        DWORD v = 0xdeadbeef;
        HRESULT hr = dev->GetRenderState((D3DRENDERSTATETYPE)s, &v);
        if (listed >= 0 ? (hr != D3D_OK || v != kRsDefaults[listed].value)
                        : (hr != DDERR_INVALIDPARAMS || v != 0xdeadbeef)) {
            printf("    render state %lu: hr=0x%08lx value 0x%08lx, want ", (unsigned long)s, (unsigned long)hr,
                   (unsigned long)v);
            if (listed >= 0)
                printf("D3D_OK value 0x%08lx\n", (unsigned long)kRsDefaults[listed].value);
            else
                printf("DDERR_INVALIDPARAMS, output untouched\n");
            ok = 0;
        }
    }
    for (DWORD stage = 0; stage < 2; ++stage)
        for (DWORD t = 1; t <= 24; ++t) {
            DWORD v = 0xdeadbeef;
            HRESULT hr = dev->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)t, &v);
            if (hr != D3D_OK || v != kTssDefaults[stage][t - 1]) {
                printf("    stage %lu state %lu: hr=0x%08lx value 0x%08lx, want 0x%08lx\n", (unsigned long)stage,
                       (unsigned long)t, (unsigned long)hr, (unsigned long)v,
                       (unsigned long)kTssDefaults[stage][t - 1]);
                ok = 0;
            }
        }
    return ok;
}

/* Defaults and the light rules, on a device of its own. */
static int states_fresh_device(void)
{
    Com<IDirectDrawSurface7> rt;
    CHECK(create_target(g_dd, 64, 64, DDSCAPS_OFFSCREENPLAIN, &rt.p));
    if (!attach_z(g_dd, rt, 64, 64))
        return 0;
    Com<IDirect3DDevice7> dev;
    CHECK(g_d3d->CreateDevice(*g_devguid, rt, &dev.p));
    if (!check_defaults(dev))
        return 0;

    D3DMATERIAL7 m, zero;
    memset(&m, 0xcc, sizeof(m));
    memset(&zero, 0, sizeof(zero));
    CHECK(dev->GetMaterial(&m));
    EXPECT(!memcmp(&m, &zero, sizeof(m)), "the default material is not all zero");
    D3DVIEWPORT7 vp;
    memset(&vp, 0xcc, sizeof(vp));
    CHECK(dev->GetViewport(&vp));
    EXPECT(vp.dwX == 0 && vp.dwY == 0 && vp.dwWidth == 64 && vp.dwHeight == 64 && vp.dvMinZ == 0.0f &&
               vp.dvMaxZ == 1.0f,
           "default viewport %lu,%lu %lux%lu z %g-%g", (unsigned long)vp.dwX, (unsigned long)vp.dwY,
           (unsigned long)vp.dwWidth, (unsigned long)vp.dwHeight, vp.dvMinZ, vp.dvMaxZ);
    float plane[4];
    memset(plane, 0xcc, sizeof(plane));
    CHECK(dev->GetClipPlane(0, plane));
    EXPECT(!plane[0] && !plane[1] && !plane[2] && !plane[3], "default clip plane %g %g %g %g", plane[0], plane[1],
           plane[2], plane[3]);
    static const D3DTRANSFORMSTATETYPE transforms[] = {D3DTRANSFORMSTATE_WORLD, D3DTRANSFORMSTATE_VIEW,
                                                       D3DTRANSFORMSTATE_PROJECTION, D3DTRANSFORMSTATE_TEXTURE0};
    for (size_t i = 0; i < sizeof(transforms) / sizeof(transforms[0]); ++i) {
        D3DMATRIX t;
        CHECK(dev->GetTransform(transforms[i], &t));
        EXPECT(!memcmp(&t, &kIdentity, sizeof(t)), "default transform %d is not identity", (int)transforms[i]);
    }
    IDirectDrawSurface7 *tex = (IDirectDrawSurface7 *)1;
    CHECK(dev->GetTexture(0, &tex));
    EXPECT(!tex, "a texture is bound by default");

    /* Lights: an unset one is an error; LightEnable creates the default
     * white directional light; SetLight alone leaves it disabled. */
    D3DLIGHT7 l, want;
    BOOL on = 7;
    EXPECT_HR(dev->GetLight(0, &l), DDERR_INVALIDPARAMS);
    EXPECT_HR(dev->GetLightEnable(0, &on), DDERR_INVALIDPARAMS);
    CHECK(dev->LightEnable(1, TRUE));
    memset(&want, 0, sizeof(want));
    want.dltType = D3DLIGHT_DIRECTIONAL;
    want.dcvDiffuse = colour_value(1.0f, 1.0f, 1.0f, 0.0f);
    want.dvDirection = vec(0.0f, 0.0f, 1.0f);
    memset(&l, 0xcc, sizeof(l));
    CHECK(dev->GetLight(1, &l));
    EXPECT(!memcmp(&l, &want, sizeof(l)), "LightEnable on an unset light made type %d, diffuse %g %g %g %g",
           (int)l.dltType, l.dcvDiffuse.r, l.dcvDiffuse.g, l.dcvDiffuse.b, l.dcvDiffuse.a);
    CHECK(dev->GetLightEnable(1, &on));
    EXPECT(on == 128, "GetLightEnable returned %d for an enabled light, the stock runtime 128", on);
    memset(&l, 0, sizeof(l));
    l.dltType = D3DLIGHT_POINT;
    l.dcvDiffuse = colour_value(0.5f, 0.25f, 1.0f, 1.0f);
    l.dvPosition = vec(1.0f, 2.0f, 3.0f);
    l.dvRange = 10.0f;
    l.dvAttenuation0 = 1.0f;
    CHECK(dev->SetLight(2, &l));
    memset(&want, 0xcc, sizeof(want));
    CHECK(dev->GetLight(2, &want));
    EXPECT(!memcmp(&l, &want, sizeof(l)), "GetLight differs from SetLight");
    CHECK(dev->GetLightEnable(2, &on));
    EXPECT(!on, "SetLight enabled the light");
    CHECK(dev->LightEnable(1, FALSE));
    CHECK(dev->GetLightEnable(1, &on));
    EXPECT(!on, "LightEnable(FALSE) left the light on");
    EXPECT_HR(dev->SetLight(3, NULL), DDERR_INVALIDPARAMS);
    l.dltType = (D3DLIGHTTYPE)0;
    EXPECT_HR(dev->SetLight(3, &l), DDERR_INVALIDPARAMS);
    return 1;
}

static int scene_states(void)
{
    if (!states_fresh_device())
        return 0;
    if (!base_state())
        return 0;

    /* Direct3D 7 states, some renamed or gone in later versions. */
    static const struct {
        D3DRENDERSTATETYPE state;
        DWORD value;
    } round_trip[] = {
        {D3DRENDERSTATE_ANTIALIAS, D3DANTIALIAS_SORTDEPENDENT},
        {D3DRENDERSTATE_TEXTUREPERSPECTIVE, FALSE},
        {D3DRENDERSTATE_LINEPATTERN, 0x00f00003},
        {D3DRENDERSTATE_ZVISIBLE, TRUE},
        {D3DRENDERSTATE_STIPPLEDALPHA, TRUE},
        {D3DRENDERSTATE_EDGEANTIALIAS, TRUE},
        {D3DRENDERSTATE_COLORKEYENABLE, TRUE},
        {D3DRENDERSTATE_COLORKEYBLENDENABLE, TRUE},
        {D3DRENDERSTATE_EXTENTS, TRUE},
        {D3DRENDERSTATE_ZBIAS, 5},
        {D3DRENDERSTATE_WRAP0, D3DWRAP_U | D3DWRAP_V},
    };
    for (size_t i = 0; i < sizeof(round_trip) / sizeof(round_trip[0]); ++i) {
        DWORD old = 0, v = 0;
        CHECK(g_dev->GetRenderState(round_trip[i].state, &old));
        CHECK(g_dev->SetRenderState(round_trip[i].state, round_trip[i].value));
        CHECK(g_dev->GetRenderState(round_trip[i].state, &v));
        CHECK(g_dev->SetRenderState(round_trip[i].state, old));
        EXPECT(v == round_trip[i].value, "render state %d: set 0x%lx, got 0x%lx", (int)round_trip[i].state,
               (unsigned long)round_trip[i].value, (unsigned long)v);
    }
    DWORD v = 0xdeadbeef;
    EXPECT_HR(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0), DDERR_INVALIDPARAMS);
    EXPECT_HR(g_dev->GetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, &v), DDERR_INVALIDPARAMS);
    EXPECT(v == 0xdeadbeef, "the failed GetRenderState wrote 0x%lx", (unsigned long)v);
    EXPECT_HR(g_dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE), DDERR_INVALIDPARAMS);
    EXPECT_HR(g_dev->GetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, &v), DDERR_INVALIDPARAMS);
    EXPECT(v == 0xdeadbeef, "the failed GetRenderState wrote 0x%lx", (unsigned long)v);

    /* Filters keep Direct3D 7 numbering. */
    static const struct {
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value;
    } filters[] = {
        {D3DTSS_MAGFILTER, D3DTFG_LINEAR},  {D3DTSS_MAGFILTER, D3DTFG_ANISOTROPIC}, {D3DTSS_MAGFILTER, D3DTFG_POINT},
        {D3DTSS_MINFILTER, D3DTFN_LINEAR},  {D3DTSS_MINFILTER, D3DTFN_ANISOTROPIC}, {D3DTSS_MINFILTER, D3DTFN_POINT},
        {D3DTSS_MIPFILTER, D3DTFP_POINT},   {D3DTSS_MIPFILTER, D3DTFP_LINEAR},      {D3DTSS_MIPFILTER, D3DTFP_NONE},
    };
    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); ++i) {
        CHECK(g_dev->SetTextureStageState(0, filters[i].type, filters[i].value));
        CHECK(g_dev->GetTextureStageState(0, filters[i].type, &v));
        EXPECT(v == filters[i].value, "stage state %d: set %lu, got %lu", (int)filters[i].type,
               (unsigned long)filters[i].value, (unsigned long)v);
    }

    EXPECT_HR(g_dev->SetMaterial(NULL), DDERR_INVALIDPARAMS);
    D3DMATERIAL7 m, mg;
    memset(&m, 0, sizeof(m));
    m.diffuse = colour_value(0.1f, 0.2f, 0.3f, 0.4f);
    m.ambient = colour_value(0.5f, 0.6f, 0.7f, 0.8f);
    m.specular = colour_value(0.9f, 1.0f, 0.0f, 0.5f);
    m.emissive = colour_value(0.25f, 0.5f, 0.75f, 1.0f);
    m.power = 12.5f;
    CHECK(g_dev->SetMaterial(&m));
    CHECK(g_dev->GetMaterial(&mg));
    EXPECT(!memcmp(&m, &mg, sizeof(m)), "GetMaterial differs from SetMaterial");

    float plane[4] = {1.0f, -2.0f, 3.5f, 0.25f}, pg[4];
    DWORD last = g_caps.wMaxUserClipPlanes ? g_caps.wMaxUserClipPlanes - 1u : 0u;
    CHECK(g_dev->SetClipPlane(0, plane));
    CHECK(g_dev->GetClipPlane(0, pg));
    EXPECT(!memcmp(plane, pg, sizeof(pg)), "clip plane 0 differs");
    CHECK(g_dev->SetClipPlane(last, plane));
    CHECK(g_dev->GetClipPlane(last, pg));
    EXPECT(!memcmp(plane, pg, sizeof(pg)), "clip plane %lu differs", (unsigned long)last);

    D3DVIEWPORT7 vp = viewport(8, 16, 64, 32, 0.25f, 0.75f), vg;
    CHECK(g_dev->SetViewport(&vp));
    CHECK(g_dev->GetViewport(&vg));
    EXPECT(!memcmp(&vp, &vg, sizeof(vp)), "GetViewport differs from SetViewport");
    vp = viewport(64, 0, W, H, 0.0f, 1.0f);
    EXPECT_HR(g_dev->SetViewport(&vp), E_INVALIDARG);
    EXPECT_HR(g_dev->SetViewport(NULL), E_INVALIDARG);
    CHECK(g_dev->GetViewport(&vg));
    EXPECT(vg.dwX == 8 && vg.dwWidth == 64, "a rejected viewport changed it to %lu,%lu %lux%lu",
           (unsigned long)vg.dwX, (unsigned long)vg.dwY, (unsigned long)vg.dwWidth, (unsigned long)vg.dwHeight);
    if (!set_viewport(0, 0, W, H, 0.0f, 1.0f))
        return 0;

    /* The retail runtime has no device statistics: S_FALSE. */
    BYTE info[64];
    HRESULT hr = g_dev->GetInfo(D3DDEVINFOID_TEXTUREMANAGER, info, sizeof(info));
    printf("    GetInfo(D3DDEVINFOID_TEXTUREMANAGER) = 0x%08lx\n", (unsigned long)hr);
    EXPECT(hr == S_FALSE, "GetInfo returned 0x%08lx", (unsigned long)hr);
    DWORD passes = 0xdeadbeef;
    hr = g_dev->ValidateDevice(&passes);
    printf("    ValidateDevice = 0x%08lx, %lu passes\n", (unsigned long)hr, (unsigned long)passes);
    EXPECT(hr == D3D_OK && passes == 1, "ValidateDevice");

    /* Identity transforms: the view volume is -1 <= x, y <= 1, 0 <= z <= 1.
     * Per sphere, D3DCLIP_* bits for planes it crosses or lies beyond, plus
     * D3DSTATUS_CLIPINTERSECTION* for planes it lies entirely beyond. */
    D3DVECTOR centres[3] = {vec(0.0f, 0.0f, 0.5f), vec(1.0f, 0.0f, 0.5f), vec(3.0f, 0.0f, 0.5f)};
    float radii[3] = {0.25f, 0.25f, 0.25f};
    DWORD vis[3] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    CHECK(g_dev->ComputeSphereVisibility(centres, radii, 3, 0, vis));
    printf("    ComputeSphereVisibility: inside 0x%lx, crossing x = 1 0x%lx, beyond 0x%lx\n", (unsigned long)vis[0],
           (unsigned long)vis[1], (unsigned long)vis[2]);
    EXPECT(vis[0] == 0 && vis[1] == D3DCLIP_RIGHT && vis[2] == (D3DSTATUS_CLIPINTERSECTIONRIGHT | D3DCLIP_RIGHT),
           "ComputeSphereVisibility");

    EXPECT_HR(g_dev->EndScene(), D3DERR_SCENE_NOT_IN_SCENE);
    CHECK(g_dev->BeginScene());
    EXPECT_HR(g_dev->BeginScene(), D3DERR_SCENE_IN_SCENE);
    CHECK(g_dev->EndScene());
    EXPECT_HR(g_dev->EndScene(), D3DERR_SCENE_NOT_IN_SCENE);
    return 1;
}

/* 21. (optional) Linear fog: per-vertex from view-space z on untransformed
 * vertices, per-pixel from z on pre-transformed ones (+-2). */
static int scene_fog(void)
{
    const DWORD bg = 0x000000;
    if (!base_state())
        return 0;
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGCOLOR, 0x00ff0000));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGSTART, fdw(0.0f)));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGEND, fdw(1.0f)));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGVERTEXMODE, D3DFOG_LINEAR));
    CHECK(clear(bg));
    CHECK(g_dev->BeginScene());
    CHECK(draw_lv_rect(16, 16, 64, 64, 0.25f, 0xff0000ffu)); /* f = (1 - 0.25) / 1 */
    CHECK(g_dev->EndScene());
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGVERTEXMODE, D3DFOG_NONE));
    CHECK(g_dev->SetRenderState(D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_LINEAR));
    CHECK(g_dev->BeginScene());
    CHECK(draw_rect(72, 72, 120, 120, 0.5f, 0xff0000ffu)); /* f = 0.5 */
    CHECK(g_dev->EndScene());
    /* f * colour + (1 - f) * fog colour */
    EXPECT_RECT(16, 16, 64, 64, 0x4000bf, bg, 2);
    EXPECT_RECT(72, 72, 120, 120, 0x800080, bg, 2);
    return 1;
}

/* 22. Presentation: the render target blitted to the clipped primary over
 * the window's client area. */
static int scene_present(void)
{
    if (!base_state())
        return 0;
    CHECK(clear(0x336699));
    RECT client;
    GetClientRect(g_wnd, &client);
    POINT o = {0, 0};
    ClientToScreen(g_wnd, &o);
    OffsetRect(&client, o.x, o.y);
    EXPECT_HR(g_primary->Blt(&client, g_rt, NULL, DDBLT_WAIT, NULL), DD_OK);
    return 1;
}

/* 23. --fullscreen, VM only: exclusive 640x480x32, a primary flip chain with
 * one 3D back buffer and a z-buffer, render, Flip, render again and read
 * the back buffer. */
static int fullscreen_frame(IDirect3DDevice7 *dev, DWORD bg, DWORD colour, float x0, float y0)
{
    CHECK(dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u | bg, 1.0f, 0));
    CHECK(dev->BeginScene());
    TLV v[4];
    tl_rect(v, x0, y0, x0 + 200, y0 + 200, 0.5f, 0xff000000u | colour);
    CHECK(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, FVF_TL, v, 4, 0));
    CHECK(dev->EndScene());
    return 1;
}

static int fullscreen_run(IDirectDraw7 *dd)
{
    CHECK(dd->SetDisplayMode(640, 480, 32, 0, 0));
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    d.dwBackBufferCount = 1;
    Com<IDirectDrawSurface7> primary, back;
    CHECK(dd->CreateSurface(&d, &primary.p, NULL));
    DDSCAPS2 caps;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    CHECK(primary->GetAttachedSurface(&caps, &back.p));
    if (!attach_z(dd, back, 640, 480))
        return 0;
    Com<IDirect3D7> d3d;
    CHECK(dd->QueryInterface(IID_IDirect3D7, (void **)&d3d.p));
    Com<IDirect3DDevice7> dev;
    const GUID *guid = NULL;
    CHECK(create_device(d3d, back, &guid, &dev.p));
    CHECK(dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE));
    CHECK(dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE));
    CHECK(dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    if (!fullscreen_frame(dev, 0xff0000, 0x00ff00, 100, 100))
        return 0;
    CHECK(primary->Flip(NULL, DDFLIP_WAIT));
    if (!fullscreen_frame(dev, 0x0000ff, 0xffff00, 340, 180))
        return 0;
    Com<IDirectDrawSurface7> back2;
    CHECK(primary->GetAttachedSurface(&caps, &back2.p));
    EXPECT(back2 == back, "Flip changed the back buffer interface");
    Px p[] = {{200, 200, 0x0000ff}, {440, 280, 0xffff00}, {342, 182, 0xffff00}, {537, 377, 0xffff00},
              {336, 280, 0x0000ff}, {10, 10, 0x0000ff},  {630, 470, 0x0000ff}};
    EXPECT_PIXELS(back, p, 0);
    CHECK(primary->Flip(NULL, DDFLIP_WAIT));
    return 1;
}

static int scene_fullscreen(void)
{
    HWND wnd = CreateWindowA("qdd_d3d7_smoke", "qdd d3d7 fullscreen", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL,
                             NULL, GetModuleHandleA(NULL), NULL);
    EXPECT(wnd, "CreateWindow failed");
    pump();
    int ok = 0;
    IDirectDraw7 *dd = NULL;
    HRESULT hr = DirectDrawCreateEx(NULL, (void **)&dd, IID_IDirectDraw7, NULL);
    if (FAILED(hr)) {
        printf("    DirectDrawCreateEx failed: hr=0x%08lx\n", (unsigned long)hr);
    } else {
        hr = dd->SetCooperativeLevel(wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        if (FAILED(hr))
            printf("    SetCooperativeLevel(EXCLUSIVE | FULLSCREEN) failed: hr=0x%08lx\n", (unsigned long)hr);
        else
            ok = fullscreen_run(dd);
        dd->RestoreDisplayMode();
        dd->SetCooperativeLevel(wnd, DDSCL_NORMAL);
        dd->Release();
    }
    DestroyWindow(wnd);
    pump();
    return ok;
}

/* ------------------------------------------------------------------------ */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

/* After a failed scene: no open scene, recording or render target switch. */
static void recover(void)
{
    DWORD sb = 0;
    g_dev->EndScene();
    if (SUCCEEDED(g_dev->EndStateBlock(&sb)) && sb)
        g_dev->DeleteStateBlock(sb);
    IDirectDrawSurface7 *rt = NULL;
    if (SUCCEEDED(g_dev->GetRenderTarget(&rt)) && rt) {
        if (rt != g_rt)
            g_dev->SetRenderTarget(g_rt, 0);
        rt->Release();
    }
    for (DWORD i = 0; i < 8; ++i)
        g_dev->SetTexture(i, NULL);
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
    g_skip_reason = NULL;
    int r = fn();
    if (r == SKIP) {
        ++g_skipped;
        printf("[SKIP] %s (%s)\n", name, g_skip_reason ? g_skip_reason : "not applicable");
    } else {
        ++g_scenes;
        if (!r) {
            ++g_failures;
            if (g_dev)
                recover();
        }
        printf("[%s] %s\n", r ? "PASS" : "FAIL", name);
    }
    fflush(stdout);
}

/* The module holding the object's QueryInterface (the stock runtime keeps
 * the device's vtable in heap memory, but its methods in d3dim700.dll). */
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

/* Window, DirectDraw object, primary with clipper, render target with
 * z-buffer, device.  Prints what it found; 0 on failure. */
static int setup(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qdd_d3d7_smoke";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("qdd_d3d7_smoke", "qdd d3d7 smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) {
        printf("FAIL: CreateWindow\n");
        return 0;
    }
    pump();

    HRESULT hr = DirectDrawCreateEx(NULL, (void **)&g_dd, IID_IDirectDraw7, NULL);
    if (FAILED(hr)) {
        printf("FAIL: DirectDrawCreateEx hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    HMODULE d9 = GetModuleHandleA("d3d9.dll");
    char d9path[MAX_PATH] = "";
    if (d9)
        GetModuleFileNameA(d9, d9path, MAX_PATH);
    printf("d3d9.dll in process: %s\n", d9 ? d9path : "(not loaded)");
    HMODULE owner = object_module(g_dd);
    print_module("IDirectDraw7 object from", owner);
    if (g_expect_local && owner != GetModuleHandleA("ddraw.dll")) {
        printf("FAIL: --expect-local, but the IDirectDraw7 object is not from the ddraw.dll beside the executable\n");
        return 0;
    }
    DDDEVICEIDENTIFIER2 id;
    memset(&id, 0, sizeof(id));
    if (SUCCEEDED(g_dd->GetDeviceIdentifier(&id, 0)))
        printf("adapter: %s (driver %s, vendor 0x%04lx device 0x%04lx)\n", id.szDescription, id.szDriver,
               (unsigned long)id.dwVendorId, (unsigned long)id.dwDeviceId);

    hr = g_dd->SetCooperativeLevel(g_wnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        printf("FAIL: SetCooperativeLevel(DDSCL_NORMAL) hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    hr = g_dd->QueryInterface(IID_IDirect3D7, (void **)&g_d3d);
    if (FAILED(hr)) {
        printf("FAIL: QueryInterface(IID_IDirect3D7) hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }

    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawClipper *clipper = NULL;
    hr = g_dd->CreateSurface(&d, &g_primary, NULL);
    if (SUCCEEDED(hr))
        hr = g_dd->CreateClipper(0, &clipper, NULL);
    if (SUCCEEDED(hr))
        hr = clipper->SetHWnd(0, g_wnd);
    if (SUCCEEDED(hr))
        hr = g_primary->SetClipper(clipper);
    if (clipper)
        clipper->Release();
    if (FAILED(hr)) {
        printf("FAIL: primary surface with clipper hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }

    hr = create_target(g_dd, W, H, DDSCAPS_OFFSCREENPLAIN, &g_rt);
    if (FAILED(hr)) {
        printf("FAIL: %dx%d X8R8G8B8 render target hr=0x%08lx\n", W, H, (unsigned long)hr);
        return 0;
    }
    if (!attach_z(g_dd, g_rt, W, H)) {
        printf("FAIL: 16-bit z-buffer\n");
        return 0;
    }
    hr = create_device(g_d3d, g_rt, &g_devguid, &g_dev);
    if (FAILED(hr)) {
        printf("FAIL: CreateDevice hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    printf("device: %s\n", device_guid_name(*g_devguid));
    print_module("IDirect3DDevice7 object from", object_module(g_dev));
    memset(&g_caps, 0, sizeof(g_caps));
    hr = g_dev->GetCaps(&g_caps);
    if (FAILED(hr)) {
        printf("FAIL: GetCaps hr=0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    printf("caps: devcaps 0x%08lx, texture caps 0x%08lx, filter caps 0x%08lx, texture op caps 0x%08lx\n",
           (unsigned long)g_caps.dwDevCaps, (unsigned long)g_caps.dpcTriCaps.dwTextureCaps,
           (unsigned long)g_caps.dpcTriCaps.dwTextureFilterCaps, (unsigned long)g_caps.dwTextureOpCaps);
    printf("caps: textures %lux%lu to %lux%lu, %u stages, %u simultaneous, %lu lights, %u clip planes, "
           "fvf caps 0x%08lx, vertex processing caps 0x%08lx\n",
           (unsigned long)g_caps.dwMinTextureWidth, (unsigned long)g_caps.dwMinTextureHeight,
           (unsigned long)g_caps.dwMaxTextureWidth, (unsigned long)g_caps.dwMaxTextureHeight,
           g_caps.wMaxTextureBlendStages, g_caps.wMaxSimultaneousTextures, (unsigned long)g_caps.dwMaxActiveLights,
           g_caps.wMaxUserClipPlanes, (unsigned long)g_caps.dwFVFCaps, (unsigned long)g_caps.dwVertexProcessingCaps);
    return 1;
}

static void teardown(void)
{
    if (g_dev)
        printf("final refcounts: device %lu", (unsigned long)g_dev->Release());
    else
        printf("final refcounts: no device");
    if (g_rt)
        printf(", render target %lu", (unsigned long)g_rt->Release());
    if (g_primary)
        g_primary->Release();
    if (g_d3d)
        g_d3d->Release();
    if (g_dd)
        printf(", ddraw %lu", (unsigned long)g_dd->Release());
    printf("\n");
    g_dev = NULL;
    g_rt = g_primary = NULL;
    g_d3d = NULL;
    g_dd = NULL;
    if (g_wnd)
        DestroyWindow(g_wnd);
    g_wnd = NULL;
    pump();
}

int main(int argc, char **argv)
{
    int expect_stock = 0, fullscreen = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--expect-local"))
            g_expect_local = 1;
        else if (!strcmp(argv[i], "--expect-stock"))
            expect_stock = 1;
        else if (!strcmp(argv[i], "--fullscreen"))
            fullscreen = 1; /* VM only: changes the display mode */
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_only = argv[++i];
        else {
            printf("usage: %s [--expect-local | --expect-stock] [--only scene[,scene]] [--fullscreen]\n"
                   "  --fullscreen adds a scene that switches to 640x480x32 exclusive mode: VM only\n",
                   argv[0]);
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
        teardown();
        return 1;
    }
    run("enum", scene_enum);
    run("clear", scene_clear);
    run("tlquad", scene_tlquad);
    run("transform", scene_transform);
    run("lit", scene_lit);
    run("ztest", scene_ztest);
    run("alpha", scene_alpha);
    run("texture", scene_texture);
    run("colorkey", scene_colorkey);
    run("p8", scene_p8);
    run("mipmap", scene_mipmap);
    run("cubemap", scene_cubemap);
    run("managed", scene_managed);
    run("vb", scene_vb);
    run("strided", scene_strided);
    run("processvertices", scene_processvertices);
    run("stateblock", scene_stateblock);
    run("rtt", scene_rtt);
    run("rtlock", scene_rtlock);
    run("states", scene_states);
    run("fog", scene_fog);
    run("present", scene_present);
    teardown();
    if (fullscreen)
        run("fullscreen", scene_fullscreen);

    printf("%s: %d/%d scenes passed", g_failures ? "FAIL" : "PASS", g_scenes - g_failures, g_scenes);
    if (g_skipped)
        printf(", %d skipped", g_skipped);
    printf(" (%lu ms)\n", (unsigned long)(GetTickCount() - start));
    return g_failures ? 1 : 0;
}
