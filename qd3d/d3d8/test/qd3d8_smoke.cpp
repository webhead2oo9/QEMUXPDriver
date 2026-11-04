/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8_smoke: deterministic Direct3D 8 scenes with pixel checks.
 *
 * Links against d3d8.dll by import, so it exercises whichever d3d8.dll the
 * loader picks: the system runtime, or qd3d8's d3d8.dll when that file sits
 * beside the executable.  The first output lines report the loaded module
 * path and the module the IDirect3D8 object comes from; --expect-local /
 * --expect-stock turn that into a hard check, and --expect-fallback checks
 * that qd3d8's d3d8.dll beside it handed the application to the stock
 * runtime saved as qd3d8_orig.dll ("make run-fallback").
 *
 * Pixels are read back from a lockable back buffer (before Present), and for
 * render-to-texture through CopyRects into an image surface.
 *
 * No D3DX or shader compiler: shaders are hand-assembled tokens.  Encoding
 * (D3D8 SDK d3d8types.h):
 *   version      vs.1.1 = 0xFFFE0101, ps.1.1 = 0xFFFF0101
 *   instruction  opcode in bits 0-15 (MOV 1, ADD 2, MUL 5), no length field
 *   parameter    bit 31 set; register type in bits 28-30; register number
 *                in bits 0-10
 *     destination: write mask in bits 16-19 (0xF = .xyzw)
 *     source:      swizzle in bits 16-23 (0xE4 = .xyzw identity)
 *   register types: TEMP r = 0, INPUT v = 1, CONST c = 2,
 *                   RASTOUT oPos = 4, ATTROUT oD = 5
 *   end          0x0000FFFF
 */
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <string.h>

#define W 128
#define H 128

/* IID_IDirect3DTexture8 (DirectX 8 SDK); defined locally to avoid dxguid. */
static const GUID kIID_IDirect3DTexture8 = {0xe4cdd575, 0x2866, 0x4f01,
                                            {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}};
static HWND g_wnd;
static IDirect3D8 *g_d3d;
static IDirect3DDevice8 *g_dev;
static D3DPRESENT_PARAMETERS g_pp;
static BOOL g_mixed;
static int g_failures;
static int g_scenes;
static const char *g_only; /* --only <scene>: run a single scene */

#define FVF_TL (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_TLT (D3DFVF_XYZRHW | D3DFVF_TEX1)

struct TLV {
    float x, y, z, rhw;
    DWORD color;
};

struct TLT {
    float x, y, z, rhw, u, v;
};

/* ------------------------------------------------------------------------ */
/* Hand-assembled shaders                                                    */

/* Destination / source parameter tokens. */
#define DST(type, reg) (0x80000000u | ((DWORD)(type) << 28) | 0x000F0000u | (reg))
#define SRC(type, reg) (0x80000000u | ((DWORD)(type) << 28) | 0x00E40000u | (reg))
#define R_TEMP 0
#define R_INPUT 1
#define R_CONST 2
#define R_RASTOUT 4
#define R_ATTROUT 5

/* vs.1.1
 *   mov oPos, v0         ; clip-space position straight through
 *   mul r0, v5, c4       ; vertex colour * c4 (c4 loaded by D3DVSD_CONST)
 *   add oD0, r0, c5      ; + c5 (set with SetVertexShaderConstant)
 */
static const DWORD g_vs_tokens[] = {
    0xFFFE0101u,
    0x00000001u, DST(R_RASTOUT, 0), SRC(R_INPUT, 0),
    0x00000005u, DST(R_TEMP, 0), SRC(R_INPUT, 5), SRC(R_CONST, 4),
    0x00000002u, DST(R_ATTROUT, 0), SRC(R_TEMP, 0), SRC(R_CONST, 5),
    0x0000FFFFu,
};

/* Declaration: stream 0 = float3 position (v0), D3DCOLOR diffuse (v5);
 * c4 = (0, 1, 0, 1). */
static const float g_c4[4] = {0.0f, 1.0f, 0.0f, 1.0f};
static DWORD g_vs_decl[] = {
    D3DVSD_STREAM(0),
    D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
    D3DVSD_CONST(4, 1), 0, 0, 0, 0, /* payload patched from g_c4 */
    D3DVSD_END(),
};

/* ps.1.1
 *   mov r0, c0           ; output the constant
 */
static const DWORD g_ps_tokens[] = {
    0xFFFF0101u,
    0x00000001u, DST(R_TEMP, 0), SRC(R_CONST, 0),
    0x0000FFFFu,
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

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

#define EXPECT(cond, ...)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("    expectation failed: " #cond "  ");  \
            printf(__VA_ARGS__);          \
            printf("\n");                 \
            return 0;                     \
        }                                 \
    } while (0)

static int color_close(DWORD a, DWORD b)
{
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)((a >> s) & 0xff), cb = (int)((b >> s) & 0xff);
        if (ca - cb > 2 || cb - ca > 2)
            return 0;
    }
    return 1;
}

/* Reads pixels from a locked 32-bit surface. */
static int read_surface(IDirect3DSurface8 *s, const POINT *pts, DWORD *out, int n)
{
    D3DSURFACE_DESC d;
    CHECK(s->GetDesc(&d));
    EXPECT(d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8, "format %d not 32-bit", (int)d.Format);
    D3DLOCKED_RECT lr;
    CHECK(s->LockRect(&lr, NULL, D3DLOCK_READONLY));
    for (int i = 0; i < n; ++i)
        out[i] = ((const DWORD *)((const BYTE *)lr.pBits + pts[i].y * lr.Pitch))[pts[i].x] & 0x00ffffffu;
    s->UnlockRect();
    return 1;
}

/* Expects colours at points in the back buffer. */
static int expect_bb(const POINT *pts, const DWORD *want, int n)
{
    IDirect3DSurface8 *bb = NULL;
    CHECK(g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb));
    DWORD got[16];
    int ok = read_surface(bb, pts, got, n);
    bb->Release();
    if (!ok)
        return 0;
    for (int i = 0; i < n; ++i) {
        if (!color_close(got[i], want[i])) {
            printf("    pixel (%ld,%ld) = %06lx, want %06lx\n", pts[i].x, pts[i].y, (unsigned long)got[i],
                   (unsigned long)want[i]);
            return 0;
        }
    }
    return 1;
}

static void use_diffuse(void)
{
    g_dev->SetTexture(0, NULL);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

static void use_texture(IDirect3DBaseTexture8 *t)
{
    g_dev->SetTexture(0, t);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
}

static void base_state(void)
{
    g_dev->SetVertexShader(FVF_TL);
    g_dev->SetPixelShader(0);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    use_diffuse();
}

static int clear(DWORD color)
{
    CHECK(g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, color, 1.0f, 0));
    return 1;
}

static void tri_tl(float x0, float y0, float x1, float y1, float x2, float y2, DWORD color, TLV *v)
{
    v[0] = TLV{x0, y0, 0.5f, 1.0f, color};
    v[1] = TLV{x1, y1, 0.5f, 1.0f, color};
    v[2] = TLV{x2, y2, 0.5f, 1.0f, color};
}

/* Full-target textured quad with exact texel-to-pixel mapping. */
static int draw_tex_quad(float w, float h)
{
    TLT q[4] = {
        {-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {w - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f},
        {w - 0.5f, h - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f},
    };
    CHECK(g_dev->SetVertexShader(FVF_TLT));
    CHECK(g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TLT)));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Scenes                                                                    */

static int scene_clear(void)
{
    CHECK(g_dev->BeginScene());
    if (!clear(0xff2040c0u))
        return 0;
    CHECK(g_dev->EndScene());
    POINT p[] = {{0, 0}, {64, 64}, {127, 127}};
    DWORD w[] = {0x2040c0, 0x2040c0, 0x2040c0};
    return expect_bb(p, w, 3);
}

static int scene_dpup(void)
{
    TLV v[3];
    base_state();
    CHECK(g_dev->BeginScene());
    if (!clear(0xff000080u))
        return 0;
    tri_tl(0, 0, W, 0, 0, H, 0xff00ff00u, v);
    CHECK(g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(TLV)));
    CHECK(g_dev->EndScene());
    POINT p[] = {{20, 20}, {110, 110}};
    DWORD w[] = {0x00ff00, 0x000080};
    return expect_bb(p, w, 2);
}

static int scene_vbib(void)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DIndexBuffer8 *ib = NULL;
    int ok = 0;
    base_state();
    /* Vertices 0-3: a full-screen red quad that must NOT be used.
     * Vertices 4-7: the (32,32)-(96,96) yellow quad, reached through
     * BaseVertexIndex = 4 with indices 0-3. */
    TLV verts[8] = {
        {0, 0, 0.5f, 1, 0xffff0000u}, {W, 0, 0.5f, 1, 0xffff0000u},
        {0, H, 0.5f, 1, 0xffff0000u}, {W, H, 0.5f, 1, 0xffff0000u},
        {32, 32, 0.5f, 1, 0xffffff00u}, {96, 32, 0.5f, 1, 0xffffff00u},
        {32, 96, 0.5f, 1, 0xffffff00u}, {96, 96, 0.5f, 1, 0xffffff00u},
    };
    static const WORD idx[6] = {0, 1, 2, 2, 1, 3};
    BYTE *p;
    IDirect3DIndexBuffer8 *gib = NULL;
    UINT gbase = 0;
    IDirect3DVertexBuffer8 *gvb = NULL;
    UINT gstride = 0;

    if (!check_hr("CreateVertexBuffer", g_dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, FVF_TL,
                                                                  D3DPOOL_MANAGED, &vb)))
        goto out;
    if (!check_hr("CreateIndexBuffer", g_dev->CreateIndexBuffer(sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                                 D3DPOOL_MANAGED, &ib)))
        goto out;
    if (!check_hr("VB Lock", vb->Lock(0, 0, &p, 0)))
        goto out;
    memcpy(p, verts, sizeof(verts));
    vb->Unlock();
    if (!check_hr("IB Lock", ib->Lock(0, 0, &p, 0)))
        goto out;
    memcpy(p, idx, sizeof(idx));
    ib->Unlock();

    if (!check_hr("SetStreamSource", g_dev->SetStreamSource(0, vb, sizeof(TLV))) ||
        !check_hr("SetIndices", g_dev->SetIndices(ib, 4)))
        goto out;
    if (!check_hr("GetIndices", g_dev->GetIndices(&gib, &gbase)))
        goto out;
    if (gib)
        gib->Release();
    if (gib != ib || gbase != 4) {
        printf("    GetIndices returned %p/%u, want %p/4\n", (void *)gib, gbase, (void *)ib);
        goto out;
    }
    if (!check_hr("GetStreamSource", g_dev->GetStreamSource(0, &gvb, &gstride)))
        goto out;
    if (gvb)
        gvb->Release();
    if (gvb != vb || gstride != sizeof(TLV)) {
        printf("    GetStreamSource identity/stride mismatch\n");
        goto out;
    }
    if (!check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000080u))
        goto out;
    if (!check_hr("DrawIndexedPrimitive", g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, 2)))
        goto out;
    g_dev->EndScene();
    {
        POINT pt[] = {{64, 64}, {40, 90}, {10, 10}, {120, 120}};
        DWORD w[] = {0xffff00, 0xffff00, 0x000080, 0x000080};
        ok = expect_bb(pt, w, 4);
    }
out:
    g_dev->SetStreamSource(0, NULL, 0);
    g_dev->SetIndices(NULL, 0);
    if (vb)
        vb->Release();
    if (ib)
        ib->Release();
    return ok;
}

static int scene_texture(void)
{
    IDirect3DTexture8 *tex = NULL;
    IDirect3DBaseTexture8 *got = NULL;
    D3DLOCKED_RECT lr;
    DWORD v = 0;
    int ok = 0;
    base_state();
    if (!check_hr("CreateTexture", g_dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)))
        return 0;
    if (!check_hr("LockRect", tex->LockRect(0, &lr, NULL, 0)))
        goto out;
    ((DWORD *)lr.pBits)[0] = 0xffff0000u;
    ((DWORD *)lr.pBits)[1] = 0xff00ff00u;
    ((DWORD *)((BYTE *)lr.pBits + lr.Pitch))[0] = 0xff0000ffu;
    ((DWORD *)((BYTE *)lr.pBits + lr.Pitch))[1] = 0xffffffffu;
    tex->UnlockRect(0);

    use_texture(tex);
    /* Sampler states set through D3D8 texture stage states. */
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    g_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    if (!check_hr("GetTextureStageState", g_dev->GetTextureStageState(0, D3DTSS_MAGFILTER, &v)) ||
        v != D3DTEXF_POINT) {
        printf("    MAGFILTER readback %lu\n", (unsigned long)v);
        goto out;
    }
    if (!check_hr("GetTexture", g_dev->GetTexture(0, &got)))
        goto out;
    if (got)
        got->Release();
    if (got != tex) {
        printf("    GetTexture identity mismatch %p != %p\n", (void *)got, (void *)tex);
        goto out;
    }
    if (!check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000000u) || !draw_tex_quad(W, H))
        goto out;
    g_dev->EndScene();
    {
        /* Points adjacent to the texel boundary: POINT sampling keeps them
         * pure, bilinear filtering would blend. */
        POINT pt[] = {{62, 62}, {65, 62}, {62, 65}, {65, 65}, {5, 5}, {122, 122}};
        DWORD w[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0xff0000, 0xffffff};
        ok = expect_bb(pt, w, 6);
    }
out:
    g_dev->SetTexture(0, NULL);
    if (tex)
        tex->Release();
    return ok;
}

static int scene_vs(void)
{
    DWORD handle = 0, cur = 0;
    int ok = 0;
    static const float c5[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    struct V {
        float x, y, z;
        DWORD color;
    } tri[3] = {
        {-1.0f, -1.0f, 0.5f, 0xffffffffu},
        {-1.0f, 3.0f, 0.5f, 0xffffffffu},
        {3.0f, -1.0f, 0.5f, 0xffffffffu},
    };
    BYTE buf[256];
    DWORD size;

    base_state();
    memcpy(&g_vs_decl[4], g_c4, sizeof(g_c4));
    if (!check_hr("CreateVertexShader", g_dev->CreateVertexShader(g_vs_decl, g_vs_tokens, &handle, 0)))
        return 0;
    if (!check_hr("SetVertexShader", g_dev->SetVertexShader(handle)) ||
        !check_hr("SetVertexShaderConstant", g_dev->SetVertexShaderConstant(5, c5, 1)))
        goto out;
    if (!check_hr("GetVertexShader", g_dev->GetVertexShader(&cur)) || cur != handle) {
        printf("    GetVertexShader returned %lx, want %lx\n", (unsigned long)cur, (unsigned long)handle);
        goto out;
    }
    size = 0;
    if (!check_hr("GetVertexShaderDeclaration(size)", g_dev->GetVertexShaderDeclaration(handle, NULL, &size)) ||
        size != sizeof(g_vs_decl)) {
        printf("    declaration size %lu, want %u\n", (unsigned long)size, (unsigned)sizeof(g_vs_decl));
        goto out;
    }
    size = sizeof(buf);
    if (!check_hr("GetVertexShaderDeclaration", g_dev->GetVertexShaderDeclaration(handle, buf, &size)) ||
        size != sizeof(g_vs_decl) || memcmp(buf, g_vs_decl, size)) {
        printf("    declaration tokens differ\n");
        goto out;
    }
    size = sizeof(buf);
    if (!check_hr("GetVertexShaderFunction", g_dev->GetVertexShaderFunction(handle, buf, &size)) ||
        size != sizeof(g_vs_tokens) || memcmp(buf, g_vs_tokens, size)) {
        printf("    function tokens differ (size %lu)\n", (unsigned long)size);
        goto out;
    }
    if (!check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000080u))
        goto out;
    if (!check_hr("DrawPrimitiveUP", g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]))))
        goto out;
    g_dev->EndScene();
    {
        /* white * c4 (0,1,0) + c5 (1,0,0) = yellow; red means the
         * declaration constant was lost, green means c5 was. */
        POINT pt[] = {{64, 64}, {2, 2}, {125, 125}};
        DWORD w[] = {0xffff00, 0xffff00, 0xffff00};
        ok = expect_bb(pt, w, 3);
    }
out:
    g_dev->SetVertexShader(FVF_TL);
    if (handle && !check_hr("DeleteVertexShader", g_dev->DeleteVertexShader(handle)))
        ok = 0;
    return ok;
}

static int scene_ps(void)
{
    DWORD handle = 0;
    int ok = 0;
    static const float c0[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    TLV v[3];
    BYTE buf[64];
    DWORD size = sizeof(buf);

    base_state();
    if (!check_hr("CreatePixelShader", g_dev->CreatePixelShader(g_ps_tokens, &handle)))
        return 0;
    if (!check_hr("GetPixelShaderFunction", g_dev->GetPixelShaderFunction(handle, buf, &size)) ||
        size != sizeof(g_ps_tokens) || memcmp(buf, g_ps_tokens, size)) {
        printf("    pixel shader tokens differ\n");
        goto out;
    }
    if (!check_hr("SetPixelShader", g_dev->SetPixelShader(handle)) ||
        !check_hr("SetPixelShaderConstant", g_dev->SetPixelShaderConstant(0, c0, 1)))
        goto out;
    if (!check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000080u))
        goto out;
    tri_tl(0, 0, W, 0, 0, H, 0xff00ff00u, v);
    if (!check_hr("DrawPrimitiveUP", g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(TLV))))
        goto out;
    g_dev->EndScene();
    {
        POINT pt[] = {{20, 20}, {110, 110}};
        DWORD w[] = {0xff00ff, 0x000080};
        ok = expect_bb(pt, w, 2);
    }
out:
    g_dev->SetPixelShader(0);
    if (handle && !check_hr("DeletePixelShader", g_dev->DeletePixelShader(handle)))
        ok = 0;
    return ok;
}

static int scene_rtt(void)
{
    IDirect3DTexture8 *rtt = NULL, *mtex = NULL;
    IDirect3DSurface8 *rt = NULL, *rt2 = NULL, *bb = NULL, *ds = NULL, *cur = NULL, *img = NULL, *mlevel = NULL;
    IDirect3DTexture8 *container = NULL;
    int ok = 0;
    TLV v[3];
    base_state();

    if (!check_hr("CreateTexture(RT)", g_dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                            D3DPOOL_DEFAULT, &rtt)))
        return 0;
    if (!check_hr("GetSurfaceLevel", rtt->GetSurfaceLevel(0, &rt)) ||
        !check_hr("GetSurfaceLevel#2", rtt->GetSurfaceLevel(0, &rt2)))
        goto out;
    if (rt != rt2) {
        printf("    GetSurfaceLevel identity mismatch %p != %p\n", (void *)rt, (void *)rt2);
        goto out;
    }
    if (!check_hr("GetContainer", rt->GetContainer(kIID_IDirect3DTexture8, (void **)&container)))
        goto out;
    if (container != rtt) {
        printf("    GetContainer identity mismatch\n");
        goto out;
    }
    if (!check_hr("GetRenderTarget", g_dev->GetRenderTarget(&bb)) ||
        !check_hr("GetDepthStencilSurface", g_dev->GetDepthStencilSurface(&ds)))
        goto out;
    if (!check_hr("SetRenderTarget(rtt)", g_dev->SetRenderTarget(rt, NULL)))
        goto out;
    if (!check_hr("GetRenderTarget#2", g_dev->GetRenderTarget(&cur)))
        goto out;
    if (cur != rt) {
        printf("    GetRenderTarget identity mismatch\n");
        goto out;
    }
    if (!check_hr("BeginScene", g_dev->BeginScene()))
        goto out;
    if (!check_hr("Clear(rtt)", g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xffff8000u, 1.0f, 0)))
        goto out;
    tri_tl(0, 0, 64, 0, 0, 64, 0xff00ffffu, v);
    if (!check_hr("DrawPrimitiveUP(rtt)", g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(TLV))))
        goto out;
    g_dev->EndScene();
    if (!check_hr("SetRenderTarget(bb)", g_dev->SetRenderTarget(bb, ds)))
        goto out;

    /* Read the render target back through CopyRects into an image surface. */
    if (!check_hr("CreateImageSurface", g_dev->CreateImageSurface(64, 64, D3DFMT_X8R8G8B8, &img)) ||
        !check_hr("CopyRects(rt->image)", g_dev->CopyRects(rt, NULL, 0, img, NULL)))
        goto out;
    {
        POINT pt[] = {{5, 5}, {60, 60}};
        DWORD got[2];
        if (!read_surface(img, pt, got, 2))
            goto out;
        if (!color_close(got[0], 0x00ffff) || !color_close(got[1], 0xff8000)) {
            printf("    CopyRects readback %06lx %06lx\n", (unsigned long)got[0], (unsigned long)got[1]);
            goto out;
        }
    }
    /* Sub-rectangle upload through CopyRects into a managed texture. */
    if (!check_hr("CreateTexture(managed)", g_dev->CreateTexture(64, 64, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED,
                                                                 &mtex)) ||
        !check_hr("GetSurfaceLevel(managed)", mtex->GetSurfaceLevel(0, &mlevel)))
        goto out;
    {
        RECT r = {0, 0, 16, 16};
        POINT dst = {32, 32};
        POINT pt[] = {{40, 40}};
        DWORD got[1];
        if (!check_hr("CopyRects(image->managed)", g_dev->CopyRects(img, &r, 1, mlevel, &dst)))
            goto out;
        if (!read_surface(mlevel, pt, got, 1))
            goto out;
        if (!color_close(got[0], 0x00ffff)) {
            printf("    managed CopyRects readback %06lx\n", (unsigned long)got[0]);
            goto out;
        }
    }

    use_texture(rtt);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    if (!check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000000u) || !draw_tex_quad(W, H))
        goto out;
    g_dev->EndScene();
    {
        POINT pt[] = {{10, 10}, {120, 120}};
        DWORD w[] = {0x00ffff, 0xff8000};
        ok = expect_bb(pt, w, 2);
    }
out:
    g_dev->SetTexture(0, NULL);
    if (bb)
        g_dev->SetRenderTarget(bb, ds);
    if (container)
        container->Release();
    if (cur)
        cur->Release();
    if (img)
        img->Release();
    if (mlevel)
        mlevel->Release();
    if (mtex)
        mtex->Release();
    if (rt2)
        rt2->Release();
    if (rt)
        rt->Release();
    if (rtt)
        rtt->Release();
    if (ds)
        ds->Release();
    if (bb)
        bb->Release();
    return ok;
}

static int scene_stateblock(void)
{
    DWORD tok = 0, tok2 = 0, v = 0, vs = 0;
    int ok = 0;
    TLV t[3];
    base_state();
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff000000u);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);

    CHECK(g_dev->BeginStateBlock());
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffff00ffu);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetVertexShader(FVF_TL);
    CHECK(g_dev->EndStateBlock(&tok));

    /* Recording must not have applied anything. */
    g_dev->GetRenderState(D3DRS_TEXTUREFACTOR, &v);
    EXPECT(v == 0xff000000u, "TEXTUREFACTOR %08lx after recording", (unsigned long)v);
    g_dev->GetTextureStageState(0, D3DTSS_MAGFILTER, &v);
    EXPECT(v == D3DTEXF_POINT, "MAGFILTER %lu after recording", (unsigned long)v);

    CHECK(g_dev->ApplyStateBlock(tok));
    g_dev->GetTextureStageState(0, D3DTSS_MAGFILTER, &v);
    EXPECT(v == D3DTEXF_LINEAR, "MAGFILTER %lu after apply", (unsigned long)v);
    g_dev->GetVertexShader(&vs);
    EXPECT(vs == FVF_TL, "vertex shader %08lx after apply", (unsigned long)vs);

    CHECK(g_dev->BeginScene());
    if (!clear(0xff000080u))
        return 0;
    tri_tl(0, 0, W, 0, 0, H, 0xff00ff00u, t);
    CHECK(g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t, sizeof(TLV)));
    CHECK(g_dev->EndScene());
    {
        POINT pt[] = {{20, 20}, {110, 110}};
        DWORD w[] = {0xff00ff, 0x000080};
        if (!expect_bb(pt, w, 2))
            goto out;
    }

    /* CreateStateBlock captures; Capture refreshes; Apply restores. */
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffff0000u);
    CHECK(g_dev->CreateStateBlock(D3DSBT_ALL, &tok2));
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff0000ffu);
    CHECK(g_dev->ApplyStateBlock(tok2));
    g_dev->GetRenderState(D3DRS_TEXTUREFACTOR, &v);
    EXPECT(v == 0xffff0000u, "TEXTUREFACTOR %08lx after CreateStateBlock/Apply", (unsigned long)v);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00u);
    CHECK(g_dev->CaptureStateBlock(tok2));
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff123456u);
    CHECK(g_dev->ApplyStateBlock(tok2));
    g_dev->GetRenderState(D3DRS_TEXTUREFACTOR, &v);
    EXPECT(v == 0xff00ff00u, "TEXTUREFACTOR %08lx after Capture/Apply", (unsigned long)v);
    ok = 1;
out:
    if (tok)
        g_dev->DeleteStateBlock(tok);
    if (tok2)
        g_dev->DeleteStateBlock(tok2);
    return ok;
}

static int scene_states(void)
{
    DWORD v = 0;
    D3DCAPS8 caps;
    CHECK(g_dev->GetDeviceCaps(&caps));
    printf("    caps: VS %lu.%lu (%lu consts), PS %lu.%lu, MaxTextureBlendStages %lu, RasterCaps %08lx\n",
           (unsigned long)((caps.VertexShaderVersion >> 8) & 0xff), (unsigned long)(caps.VertexShaderVersion & 0xff),
           (unsigned long)caps.MaxVertexShaderConst, (unsigned long)((caps.PixelShaderVersion >> 8) & 0xff),
           (unsigned long)(caps.PixelShaderVersion & 0xff), (unsigned long)caps.MaxTextureBlendStages,
           (unsigned long)caps.RasterCaps);
    EXPECT(caps.VertexShaderVersion >= D3DVS_VERSION(1, 1) && caps.VertexShaderVersion <= D3DVS_VERSION(1, 1) + 0xff,
           "vertex shader version %08lx", (unsigned long)caps.VertexShaderVersion);
    EXPECT(caps.PixelShaderVersion >= D3DPS_VERSION(1, 1) && caps.PixelShaderVersion <= D3DPS_VERSION(1, 4),
           "pixel shader version %08lx", (unsigned long)caps.PixelShaderVersion);

    /* Render states renamed or removed in D3D9 must still round-trip. */
    CHECK(g_dev->SetRenderState(D3DRS_ZBIAS, 5));
    CHECK(g_dev->GetRenderState(D3DRS_ZBIAS, &v));
    EXPECT(v == 5, "ZBIAS %lu", (unsigned long)v);
    CHECK(g_dev->SetRenderState(D3DRS_ZBIAS, 0));
    CHECK(g_dev->SetRenderState(D3DRS_EDGEANTIALIAS, TRUE));
    CHECK(g_dev->GetRenderState(D3DRS_EDGEANTIALIAS, &v));
    EXPECT(v == TRUE, "EDGEANTIALIAS %lu", (unsigned long)v);
    CHECK(g_dev->SetRenderState(D3DRS_EDGEANTIALIAS, FALSE));
    {
        float segs = 1.0f;
        DWORD dw;
        memcpy(&dw, &segs, 4);
        CHECK(g_dev->SetRenderState(D3DRS_PATCHSEGMENTS, dw));
        CHECK(g_dev->GetRenderState(D3DRS_PATCHSEGMENTS, &v));
        EXPECT(v == dw, "PATCHSEGMENTS %08lx", (unsigned long)v);
    }
    if (g_mixed) {
        CHECK(g_dev->SetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, TRUE));
        CHECK(g_dev->GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, &v));
        EXPECT(v == TRUE, "SOFTWAREVERTEXPROCESSING %lu", (unsigned long)v);
        CHECK(g_dev->SetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, FALSE));
        CHECK(g_dev->GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, &v));
        EXPECT(v == FALSE, "SOFTWAREVERTEXPROCESSING %lu", (unsigned long)v);
    }

    /* Every D3D8 texture stage state that became a D3D9 sampler state. */
    static const struct {
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value;
    } tss[] = {
        {D3DTSS_ADDRESSU, D3DTADDRESS_MIRROR},   {D3DTSS_ADDRESSV, D3DTADDRESS_WRAP},
        {D3DTSS_ADDRESSW, D3DTADDRESS_CLAMP},    {D3DTSS_BORDERCOLOR, 0xff336699u},
        {D3DTSS_MAGFILTER, D3DTEXF_LINEAR},      {D3DTSS_MINFILTER, D3DTEXF_LINEAR},
        {D3DTSS_MIPFILTER, D3DTEXF_POINT},       {D3DTSS_MIPMAPLODBIAS, 0x3f000000u},
        {D3DTSS_MAXMIPLEVEL, 2},                 {D3DTSS_MAXANISOTROPY, 4},
    };
    for (unsigned i = 0; i < sizeof(tss) / sizeof(tss[0]); ++i) {
        CHECK(g_dev->SetTextureStageState(1, tss[i].type, tss[i].value));
        CHECK(g_dev->GetTextureStageState(1, tss[i].type, &v));
        EXPECT(v == tss[i].value, "TSS %d = %08lx, want %08lx", (int)tss[i].type, (unsigned long)v,
               (unsigned long)tss[i].value);
    }
    return 1;
}

static int scene_reset(void)
{
    D3DPRESENT_PARAMETERS pp = g_pp;
    IDirect3DSurface8 *bb = NULL;
    D3DSURFACE_DESC d;
    TLV v[3];
    pp.BackBufferWidth = 96;
    pp.BackBufferHeight = 96;
    pp.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;
    CHECK(g_dev->Reset(&pp));
    g_pp = pp;
    CHECK(g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb));
    HRESULT hr = bb->GetDesc(&d);
    bb->Release();
    CHECK(hr);
    EXPECT(d.Width == 96 && d.Height == 96, "back buffer %ux%u after Reset", d.Width, d.Height);
    base_state();
    CHECK(g_dev->BeginScene());
    if (!clear(0xff400000u))
        return 0;
    tri_tl(0, 0, 96, 0, 0, 96, 0xff0080ffu, v);
    CHECK(g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(TLV)));
    CHECK(g_dev->EndScene());
    POINT pt[] = {{10, 10}, {90, 90}};
    DWORD w[] = {0x0080ff, 0x400000};
    if (!expect_bb(pt, w, 2))
        return 0;
    CHECK(g_dev->Present(NULL, NULL, NULL, NULL));
    return 1;
}

/* The front buffer of a windowed device is the screen with the window's
 * last presented image at its client area (natively the desktop): both
 * halves of a presented frame read back at their screen positions. The
 * window goes on top so the stock runtime's desktop shows it. */
static int scene_frontbuffer(void)
{
    const DWORD left = 0xc04010, right = 0x1060d0;
    D3DDISPLAYMODE dm = {};
    IDirect3DSurface8 *img = NULL;
    D3DRECT half = {(LONG)g_pp.BackBufferWidth / 2, 0, (LONG)g_pp.BackBufferWidth, (LONG)g_pp.BackBufferHeight};
    RECT cr;
    POINT pt[2];
    DWORD got[2], want[2] = {left, right};
    int ok = 1;

    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (int i = 0; i < 3 && ok; ++i) {
        ok = SUCCEEDED(g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xff000000u | left, 1.0f, 0)) &&
             SUCCEEDED(g_dev->Clear(1, &half, D3DCLEAR_TARGET, 0xff000000u | right, 1.0f, 0)) &&
             SUCCEEDED(g_dev->Present(NULL, NULL, NULL, NULL));
        pump();
        Sleep(40);
    }
    GetClientRect(g_wnd, &cr);
    pt[0].x = cr.right / 4;
    pt[1].x = cr.right * 3 / 4;
    pt[0].y = pt[1].y = cr.bottom / 2;
    ClientToScreen(g_wnd, &pt[0]);
    ClientToScreen(g_wnd, &pt[1]);
    if (ok)
        ok = SUCCEEDED(g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm)) &&
             SUCCEEDED(g_dev->CreateImageSurface(dm.Width, dm.Height, D3DFMT_A8R8G8B8, &img));
    if (ok) {
        HRESULT hr = g_dev->GetFrontBuffer(img);
        ok = check_hr("GetFrontBuffer", hr);
    }
    SetWindowPos(g_wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ok && (pt[0].x < 0 || pt[0].y < 0 || (UINT)pt[1].x >= dm.Width || (UINT)pt[1].y >= dm.Height)) {
        printf("    window partly off the screen; front buffer contents not checked\n");
    } else if (ok && read_surface(img, pt, got, 2)) {
        for (int i = 0; i < 2; ++i) {
            if (!color_close(got[i], want[i])) {
                printf("    front buffer at screen (%ld,%ld) = %06lx, want %06lx\n", pt[i].x, pt[i].y,
                       (unsigned long)got[i], (unsigned long)want[i]);
                ok = 0;
            }
        }
    } else {
        ok = 0;
    }
    if (img)
        img->Release();
    return ok;
}

/* Declaration-only vertex shader (fixed-function processing).  The D3D8
 * runtime requires fixed-function declarations to follow FVF order without
 * gaps, so D3DVSD_SKIP is exercised by the programmable vs.1.0 scene. */
static int scene_ff_decl(void)
{
    static DWORD decl[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
        D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
        D3DVSD_END(),
    };
    struct V {
        float x, y, z;
        DWORD color;
    } tri[3] = {
        {-1.0f, 1.0f, 0.5f, 0xffc08040u},
        {1.0f, 1.0f, 0.5f, 0xffc08040u},
        {-1.0f, -1.0f, 0.5f, 0xffc08040u},
    };
    D3DMATRIX id;
    memset(&id, 0, sizeof(id));
    id._11 = id._22 = id._33 = id._44 = 1.0f;
    DWORD handle = 0;
    int ok = 0;
    base_state();
    g_dev->SetTransform(D3DTS_WORLD, &id);
    g_dev->SetTransform(D3DTS_VIEW, &id);
    g_dev->SetTransform(D3DTS_PROJECTION, &id);
    CHECK(g_dev->CreateVertexShader(decl, NULL, &handle, 0));
    {
        DWORD size = 0;
        if (!check_hr("GetVertexShaderFunction", g_dev->GetVertexShaderFunction(handle, NULL, &size)) || size != 0) {
            printf("    function size %lu for declaration-only shader\n", (unsigned long)size);
            goto out;
        }
    }
    if (!check_hr("SetVertexShader", g_dev->SetVertexShader(handle)) ||
        !check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000080u))
        goto out;
    if (!check_hr("DrawPrimitiveUP", g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]))))
        goto out;
    g_dev->EndScene();
    {
        /* Clip-space top-left half of the target. */
        POINT pt[] = {{20, 20}, {110, 110}};
        DWORD w[] = {0xc08040, 0x000080};
        ok = expect_bb(pt, w, 2);
    }
out:
    g_dev->SetVertexShader(FVF_TL);
    g_dev->DeleteVertexShader(handle);
    return ok;
}

/* vs.1.0 function: the runtime must accept it (qd3d8 promotes to vs_1_1)
 * and GetVertexShaderFunction must return the original tokens.  The
 * declaration skips one DWORD (D3DVSD_SKIP) between position and colour;
 * the skipped value is black, so misplacing v5 would turn the result red. */
static int scene_vs10(void)
{
    DWORD tokens[sizeof(g_vs_tokens) / sizeof(DWORD)];
    memcpy(tokens, g_vs_tokens, sizeof(tokens));
    tokens[0] = 0xFFFE0100u;
    DWORD decl[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
        D3DVSD_SKIP(1),
        D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
        D3DVSD_CONST(4, 1), 0, 0, 0, 0,
        D3DVSD_END(),
    };
    memcpy(&decl[5], g_c4, sizeof(g_c4));
    static const float c5[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    struct V {
        float x, y, z;
        DWORD skipped;
        DWORD color;
    } tri[3] = {
        {-1.0f, -1.0f, 0.5f, 0xff000000u, 0xffffffffu},
        {-1.0f, 3.0f, 0.5f, 0xff000000u, 0xffffffffu},
        {3.0f, -1.0f, 0.5f, 0xff000000u, 0xffffffffu},
    };
    DWORD handle = 0, buf[64], size = sizeof(buf);
    int ok = 0;
    base_state();
    CHECK(g_dev->CreateVertexShader(decl, tokens, &handle, 0));
    if (!check_hr("GetVertexShaderFunction", g_dev->GetVertexShaderFunction(handle, buf, &size)) ||
        size != sizeof(tokens) || buf[0] != 0xFFFE0100u) {
        printf("    vs.1.0 function not returned verbatim\n");
        goto out;
    }
    if (!check_hr("SetVertexShader", g_dev->SetVertexShader(handle)) ||
        !check_hr("SetVertexShaderConstant", g_dev->SetVertexShaderConstant(5, c5, 1)) ||
        !check_hr("BeginScene", g_dev->BeginScene()) || !clear(0xff000080u))
        goto out;
    if (!check_hr("DrawPrimitiveUP", g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]))))
        goto out;
    g_dev->EndScene();
    {
        POINT pt[] = {{64, 64}};
        DWORD w[] = {0xffff00};
        ok = expect_bb(pt, w, 1);
    }
out:
    g_dev->SetVertexShader(FVF_TL);
    g_dev->DeleteVertexShader(handle);
    return ok;
}

/* CopyRects across pools: SYSTEMMEM -> DEFAULT render target, DEFAULT RT
 * -> DEFAULT RT texture level (multiple rects), DEFAULT -> SYSTEMMEM. */
static int scene_copyrects(void)
{
    IDirect3DSurface8 *img = NULL, *img2 = NULL, *rts = NULL, *level = NULL, *bb = NULL, *ds = NULL;
    IDirect3DTexture8 *rtt = NULL;
    D3DLOCKED_RECT lr;
    int ok = 0;
    base_state();
    if (!check_hr("CreateImageSurface", g_dev->CreateImageSurface(64, 64, D3DFMT_X8R8G8B8, &img)) ||
        !check_hr("CreateImageSurface#2", g_dev->CreateImageSurface(64, 64, D3DFMT_X8R8G8B8, &img2)) ||
        !check_hr("CreateRenderTarget", g_dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, FALSE,
                                                                  &rts)) ||
        !check_hr("CreateTexture(RT)", g_dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                            D3DPOOL_DEFAULT, &rtt)) ||
        !check_hr("GetSurfaceLevel", rtt->GetSurfaceLevel(0, &level)))
        goto out;
    /* Source pattern: left half red, right half blue. */
    if (!check_hr("LockRect", img->LockRect(&lr, NULL, 0)))
        goto out;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = x < 32 ? 0xffff0000u : 0xff0000ffu;
    img->UnlockRect();
    if (!check_hr("CopyRects(image->rt)", g_dev->CopyRects(img, NULL, 0, rts, NULL)))
        goto out;
    /* Green background in the texture level. */
    if (!check_hr("GetRenderTarget", g_dev->GetRenderTarget(&bb)) ||
        !check_hr("GetDepthStencilSurface", g_dev->GetDepthStencilSurface(&ds)) ||
        !check_hr("SetRenderTarget", g_dev->SetRenderTarget(level, NULL)) ||
        !check_hr("Clear", g_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xff00ff00u, 1.0f, 0)) ||
        !check_hr("SetRenderTarget(bb)", g_dev->SetRenderTarget(bb, ds)))
        goto out;
    {
        RECT rects[2] = {{0, 0, 32, 32}, {40, 0, 56, 16}};
        POINT pts[2] = {{16, 16}, {0, 48}};
        if (!check_hr("CopyRects(rt->rt texture)", g_dev->CopyRects(rts, rects, 2, level, pts)))
            goto out;
    }
    if (!check_hr("CopyRects(rt texture->image)", g_dev->CopyRects(level, NULL, 0, img2, NULL)))
        goto out;
    {
        POINT pt[] = {{5, 5}, {20, 20}, {40, 40}, {50, 50}, {4, 52}, {20, 60}};
        DWORD want[] = {0x00ff00, 0xff0000, 0xff0000, 0x00ff00, 0x0000ff, 0x00ff00};
        DWORD got[6];
        if (!read_surface(img2, pt, got, 6))
            goto out;
        ok = 1;
        for (int i = 0; i < 6; ++i) {
            if (!color_close(got[i], want[i])) {
                printf("    (%ld,%ld) = %06lx, want %06lx\n", pt[i].x, pt[i].y, (unsigned long)got[i],
                       (unsigned long)want[i]);
                ok = 0;
            }
        }
    }
out:
    if (img)
        img->Release();
    if (img2)
        img2->Release();
    if (rts)
        rts->Release();
    if (level)
        level->Release();
    if (rtt)
        rtt->Release();
    if (ds)
        ds->Release();
    if (bb)
        bb->Release();
    return ok;
}

static const GUID kIID_IDirect3DCubeTexture8 = {0x3ee5b968, 0x2aca, 0x4c34,
                                                {0x8b, 0xb5, 0x7e, 0x0c, 0x3d, 0x19, 0xb7, 0x50}};

static int scene_cube_volume(void)
{
    IDirect3DCubeTexture8 *cube = NULL, *cube_c = NULL;
    IDirect3DSurface8 *f0 = NULL, *f1 = NULL;
    IDirect3DVolumeTexture8 *vol = NULL;
    IDirect3DVolume8 *v0 = NULL, *v1 = NULL;
    IDirect3DTexture8 *dxt = NULL;
    D3DSURFACE_DESC sd;
    D3DVOLUME_DESC vd;
    D3DLOCKED_RECT lr;
    D3DLOCKED_BOX lb;
    int ok = 0;
    if (!check_hr("CreateCubeTexture", g_dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube)) ||
        !check_hr("GetCubeMapSurface", cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &f0)) ||
        !check_hr("GetCubeMapSurface#2", cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &f1)))
        goto out;
    if (f0 != f1) {
        printf("    cube face identity mismatch\n");
        goto out;
    }
    if (!check_hr("GetContainer(cube)", f0->GetContainer(kIID_IDirect3DCubeTexture8, (void **)&cube_c)) ||
        cube_c != cube) {
        printf("    cube GetContainer mismatch\n");
        goto out;
    }
    if (!check_hr("GetLevelDesc(cube)", cube->GetLevelDesc(0, &sd)) || sd.Size != 16 * 16 * 4 ||
        sd.Type != D3DRTYPE_SURFACE || sd.Pool != D3DPOOL_MANAGED) {
        printf("    cube level desc size %u type %d pool %d\n", sd.Size, (int)sd.Type, (int)sd.Pool);
        goto out;
    }
    if (!check_hr("LockRect(cube)", cube->LockRect(D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &lr, NULL, 0)))
        goto out;
    memset(lr.pBits, 0x7f, 16 * 4);
    cube->UnlockRect(D3DCUBEMAP_FACE_NEGATIVE_Z, 0);

    if (!check_hr("CreateVolumeTexture", g_dev->CreateVolumeTexture(8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                                    &vol)) ||
        !check_hr("GetVolumeLevel", vol->GetVolumeLevel(0, &v0)) ||
        !check_hr("GetVolumeLevel#2", vol->GetVolumeLevel(0, &v1)))
        goto out;
    if (v0 != v1) {
        printf("    volume identity mismatch\n");
        goto out;
    }
    if (!check_hr("GetDesc(volume)", v0->GetDesc(&vd)) || vd.Size != 8 * 8 * 4 * 4 || vd.Depth != 4 ||
        vd.Type != D3DRTYPE_VOLUME) {
        printf("    volume desc size %u depth %u type %d\n", vd.Size, vd.Depth, (int)vd.Type);
        goto out;
    }
    if (!check_hr("LockBox", v0->LockBox(&lb, NULL, 0)))
        goto out;
    ((DWORD *)lb.pBits)[0] = 0x12345678u;
    v0->UnlockBox();
    if (!check_hr("LockBox(texture)", vol->LockBox(0, &lb, NULL, D3DLOCK_READONLY)))
        goto out;
    {
        DWORD first = ((DWORD *)lb.pBits)[0];
        vol->UnlockBox(0);
        if (first != 0x12345678u) {
            printf("    volume readback %08lx\n", (unsigned long)first);
            goto out;
        }
    }
    if (!check_hr("CreateTexture(DXT1)", g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &dxt)) ||
        !check_hr("GetLevelDesc(DXT1)", dxt->GetLevelDesc(0, &sd)) || sd.Size != 128) {
        printf("    DXT1 16x16 size %u, want 128\n", sd.Size);
        goto out;
    }
    ok = 1;
out:
    if (dxt)
        dxt->Release();
    if (v1)
        v1->Release();
    if (v0)
        v0->Release();
    if (vol)
        vol->Release();
    if (cube_c)
        cube_c->Release();
    if (f1)
        f1->Release();
    if (f0)
        f0->Release();
    if (cube)
        cube->Release();
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Object lifetime                                                           */

/* Private data names an object without touching a released pointer; an
 * IUnknown stored with D3DSPD_IUNKNOWN is released when its object dies. */
static const GUID kTagGuid = {0x3c1f8d2e, 0x5a7b, 0x4c90, {0x8e, 0x21, 0x6d, 0x4f, 0x0b, 0x93, 0xa7, 0x51}};
static const GUID kTrackGuid = {0x3c1f8d2e, 0x5a7b, 0x4c90, {0x8e, 0x21, 0x6d, 0x4f, 0x0b, 0x93, 0xa7, 0x52}};

struct Tracker : public IUnknown {
    LONG refs;
    STDMETHOD(QueryInterface)(REFIID, void **out)
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() { return (ULONG)InterlockedDecrement(&refs); }
};

/* Static: an object a failed check leaves alive may release them later. */
static Tracker g_trackers[16];

template <class T> static void tag_set(T *o, DWORD v)
{
    o->SetPrivateData(kTagGuid, &v, sizeof(v), 0);
}

template <class T> static DWORD tag_get(T *o)
{
    DWORD v = 0, n = sizeof(v);
    return o && SUCCEEDED(o->GetPrivateData(kTagGuid, &v, &n)) ? v : 0;
}

template <class T> static Tracker *track(T *o, unsigned i)
{
    Tracker *t = &g_trackers[i];
    t->refs = 1;
    o->SetPrivateData(kTrackGuid, static_cast<IUnknown *>(t), sizeof(IUnknown *), D3DSPD_IUNKNOWN);
    t->Release();
    return t;
}

static int alive(const Tracker *t)
{
    return t->refs > 0;
}

static ULONG count(IUnknown *u)
{
    u->AddRef();
    return u->Release();
}

/* A texture released while bound; its level surfaces; cube and volume
 * levels. */
static int life_texture(void)
{
    IDirect3DTexture8 *tex = NULL, *cont = NULL;
    IDirect3DBaseTexture8 *bt = NULL;
    IDirect3DSurface8 *lvl = NULL, *lvl2 = NULL, *face = NULL;
    IDirect3DCubeTexture8 *cube = NULL;
    IDirect3DVolumeTexture8 *vt = NULL;
    IDirect3DVolume8 *vol = NULL;
    ULONG d0 = count(g_dev), r;

    CHECK(g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex));
    EXPECT(count(g_dev) == d0 + 1, "device count %lu after CreateTexture, want %lu", count(g_dev), d0 + 1);
    tag_set(tex, 0x7e57u);
    Tracker *t = track(tex, 0);
    CHECK(g_dev->SetTexture(0, tex));
    EXPECT(count(tex) == 1, "texture count %lu while bound, want 1", count(tex));
    r = tex->Release();
    EXPECT(r == 0, "Release of the bound texture returned %lu", r);
    EXPECT(count(g_dev) == d0, "device count %lu with the released texture bound, want %lu", count(g_dev), d0);
    EXPECT(alive(t), "the released texture died while bound");
    CHECK(g_dev->GetTexture(0, &bt));
    EXPECT(bt && (void *)bt == (void *)tex && tag_get(bt) == 0x7e57u,
           "GetTexture returned %p (tag %lx), not the released texture %p", (void *)bt, tag_get(bt), (void *)tex);
    EXPECT(count(bt) == 1 && count(g_dev) == d0 + 1, "texture count %lu, device %lu after GetTexture", count(bt),
           count(g_dev));

    /* Level surfaces share the texture's count and add no device reference. */
    CHECK(((IDirect3DTexture8 *)bt)->GetSurfaceLevel(0, &lvl));
    EXPECT(count(bt) == 2 && count(lvl) == 2, "texture/level counts %lu/%lu after GetSurfaceLevel, want 2/2",
           count(bt), count(lvl));
    EXPECT(count(g_dev) == d0 + 1, "device count %lu after GetSurfaceLevel, want %lu", count(g_dev), d0 + 1);
    tag_set(lvl, 0x1e7e1u);
    r = lvl->Release();
    EXPECT(r == 1, "level Release returned %lu, want the texture's count 1", r);
    CHECK(((IDirect3DTexture8 *)bt)->GetSurfaceLevel(0, &lvl2));
    EXPECT(lvl2 == lvl && tag_get(lvl2) == 0x1e7e1u, "GetSurfaceLevel returned another surface after its release");
    CHECK(lvl2->GetContainer(kIID_IDirect3DTexture8, (void **)&cont));
    EXPECT((void *)cont == (void *)bt, "the level's container %p is not its texture %p", (void *)cont, (void *)bt);
    cont->Release();
    lvl2->Release();
    r = bt->Release();
    EXPECT(r == 0 && count(g_dev) == d0, "texture Release returned %lu, device %lu", r, count(g_dev));
    CHECK(g_dev->SetTexture(0, NULL));
    EXPECT(!alive(t), "the released texture outlived its binding");

    CHECK(g_dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube));
    CHECK(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Y, 0, &face));
    EXPECT(count(cube) == 2, "cube count %lu with a face held, want 2", count(cube));
    r = face->Release();
    EXPECT(r == 1, "cube face Release returned %lu, want 1", r);
    cube->Release();
    CHECK(g_dev->CreateVolumeTexture(8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vt));
    CHECK(vt->GetVolumeLevel(0, &vol));
    EXPECT(count(vt) == 2, "volume texture count %lu with its level held, want 2", count(vt));
    r = vol->Release();
    EXPECT(r == 1, "volume Release returned %lu, want 1", r);
    vt->Release();
    EXPECT(count(g_dev) == d0, "device count %lu after the level checks, want %lu", count(g_dev), d0);
    return 1;
}

/* Vertex and index buffers released while bound; a NULL buffer keeps the
 * stride and base vertex index; DrawPrimitiveUP unbinds stream 0. */
static int life_buffers(void)
{
    IDirect3DVertexBuffer8 *vb = NULL, *gvb = NULL;
    IDirect3DIndexBuffer8 *ib = NULL, *gib = NULL;
    UINT stride = 0, base = 0;
    ULONG d0 = count(g_dev);
    TLV v[3];

    CHECK(g_dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, FVF_TL, D3DPOOL_MANAGED, &vb));
    CHECK(g_dev->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib));
    tag_set(vb, 0xb0b0u);
    tag_set(ib, 0x1b1bu);
    Tracker *tv = track(vb, 1), *ti = track(ib, 2);
    CHECK(g_dev->SetStreamSource(0, vb, sizeof(TLV)));
    CHECK(g_dev->SetIndices(ib, 3));
    ULONG rv = vb->Release(), ri = ib->Release();
    EXPECT(rv == 0 && ri == 0, "Release of the bound buffers returned %lu/%lu", rv, ri);
    EXPECT(count(g_dev) == d0, "device count %lu with the released buffers bound, want %lu", count(g_dev), d0);
    EXPECT(alive(tv) && alive(ti), "a released buffer died while bound");
    CHECK(g_dev->GetStreamSource(0, &gvb, &stride));
    EXPECT(gvb && (void *)gvb == (void *)vb && tag_get(gvb) == 0xb0b0u && stride == sizeof(TLV),
           "GetStreamSource returned %p (tag %lx) stride %u", (void *)gvb, tag_get(gvb), stride);
    gvb->Release();
    CHECK(g_dev->GetIndices(&gib, &base));
    EXPECT(gib && (void *)gib == (void *)ib && tag_get(gib) == 0x1b1bu && base == 3,
           "GetIndices returned %p (tag %lx) base %u", (void *)gib, tag_get(gib), base);
    gib->Release();
    CHECK(g_dev->SetStreamSource(0, NULL, 0));
    CHECK(g_dev->SetIndices(NULL, 0));
    EXPECT(!alive(tv) && !alive(ti), "released buffers outlived their binding");
    CHECK(g_dev->GetStreamSource(0, &gvb, &stride));
    CHECK(g_dev->GetIndices(&gib, &base));
    EXPECT(!gvb && stride == sizeof(TLV) && !gib && base == 3,
           "after binding NULL: stream %p stride %u, indices %p base %u", (void *)gvb, stride, (void *)gib, base);

    CHECK(g_dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, FVF_TL, D3DPOOL_MANAGED, &vb));
    CHECK(g_dev->SetStreamSource(0, vb, sizeof(TLV)));
    vb->Release();
    base_state();
    tri_tl(0, 0, 8, 0, 0, 8, 0xff00ff00u, v);
    CHECK(g_dev->BeginScene());
    CHECK(g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(TLV)));
    CHECK(g_dev->EndScene());
    CHECK(g_dev->GetStreamSource(0, &gvb, &stride));
    EXPECT(!gvb && stride == 0, "after DrawPrimitiveUP: stream 0 %p stride %u", (void *)gvb, stride);
    return 1;
}

/* Render targets, depth stencils and the device's implicit surfaces. */
static int life_targets(void)
{
    IDirect3DSurface8 *bb = NULL, *ads = NULL, *rt = NULL, *ds = NULL, *g = NULL, *g2 = NULL, *lvl = NULL;
    IDirect3DTexture8 *rtt = NULL, *cont = NULL;
    ULONG d0;

    CHECK(g_dev->GetRenderTarget(&bb));
    CHECK(g_dev->GetDepthStencilSurface(&ads));
    d0 = count(g_dev);
    CHECK(g_dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &rt));
    CHECK(g_dev->CreateDepthStencilSurface(64, 64, D3DFMT_D16, D3DMULTISAMPLE_NONE, &ds));
    tag_set(rt, 0x4744u);
    tag_set(ds, 0xd544u);
    Tracker *trt = track(rt, 3), *tds = track(ds, 4);
    CHECK(g_dev->SetRenderTarget(rt, ds));
    EXPECT(count(rt) == 1 && count(ds) == 1, "render target/depth counts %lu/%lu while set", count(rt), count(ds));
    ULONG r1 = rt->Release(), r2 = ds->Release();
    EXPECT(r1 == 0 && r2 == 0, "Release of the set render target/depth stencil returned %lu/%lu", r1, r2);
    EXPECT(count(g_dev) == d0, "device count %lu with them released while set, want %lu", count(g_dev), d0);
    EXPECT(alive(trt) && alive(tds), "a released render target or depth stencil died while set");
    CHECK(g_dev->GetRenderTarget(&g));
    CHECK(g_dev->GetDepthStencilSurface(&g2));
    EXPECT((void *)g == (void *)rt && tag_get(g) == 0x4744u && (void *)g2 == (void *)ds && tag_get(g2) == 0xd544u,
           "GetRenderTarget/GetDepthStencilSurface returned other surfaces");
    g->Release();
    g2->Release();
    CHECK(g_dev->SetRenderTarget(bb, ads));
    EXPECT(!alive(trt) && !alive(tds), "released render target/depth stencil outlived being set");

    /* A texture level set as render target keeps its texture alive. */
    CHECK(g_dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rtt));
    CHECK(rtt->GetSurfaceLevel(0, &lvl));
    tag_set(rtt, 0x7e77u);
    tag_set(lvl, 0x1e77u);
    Tracker *trtt = track(rtt, 5);
    CHECK(g_dev->SetRenderTarget(lvl, NULL));
    g = (IDirect3DSurface8 *)1;
    EXPECT(g_dev->GetDepthStencilSurface(&g) == D3DERR_NOTFOUND && !g, "GetDepthStencilSurface with none set");
    r1 = lvl->Release();
    r2 = rtt->Release();
    EXPECT(r1 == 1 && r2 == 0, "level/texture Release returned %lu/%lu, want 1/0", r1, r2);
    EXPECT(count(g_dev) == d0 && alive(trtt), "device count %lu, texture alive %d", count(g_dev), alive(trtt));
    CHECK(g_dev->GetRenderTarget(&g));
    EXPECT((void *)g == (void *)lvl && tag_get(g) == 0x1e77u, "GetRenderTarget returned another surface");
    EXPECT(count(g) == 1 && count(g_dev) == d0 + 1, "level count %lu, device %lu", count(g), count(g_dev));
    CHECK(g->GetContainer(kIID_IDirect3DTexture8, (void **)&cont));
    EXPECT(tag_get(cont) == 0x7e77u, "the render target level's container is another texture");
    cont->Release();
    g->Release();
    CHECK(g_dev->SetRenderTarget(bb, ads));
    EXPECT(!alive(trtt), "released render target texture outlived being set");

    /* The device owns its back buffer and automatic depth stencil. */
    tag_set(bb, 0xbbu);
    tag_set(ads, 0xadu);
    r1 = bb->Release();
    r2 = ads->Release();
    EXPECT(r1 == 0 && r2 == 0, "back buffer/depth stencil Release returned %lu/%lu", r1, r2);
    EXPECT(count(g_dev) == d0 - 2, "device count %lu after releasing them, want %lu", count(g_dev), d0 - 2);
    CHECK(g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &g));
    CHECK(g_dev->GetRenderTarget(&g2));
    EXPECT(g == bb && g2 == bb && tag_get(g) == 0xbbu, "back buffer %p / render target %p, want %p", (void *)g,
           (void *)g2, (void *)bb);
    g->Release();
    g2->Release();
    CHECK(g_dev->GetDepthStencilSurface(&g));
    EXPECT(g == ads && tag_get(g) == 0xadu, "GetDepthStencilSurface returned another surface");
    g->Release();
    return 1;
}

/* State blocks keep what they captured or recorded; pixel state keeps no
 * textures. */
static int life_state_blocks(void)
{
    IDirect3DTexture8 *a = NULL, *b = NULL, *c = NULL;
    IDirect3DBaseTexture8 *g = NULL;
    DWORD tok = 0, tok2 = 0, tok3 = 0;

    CHECK(g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &a));
    tag_set(a, 0xa1u);
    Tracker *ta = track(a, 6);
    CHECK(g_dev->SetTexture(1, a));
    CHECK(g_dev->CreateStateBlock(D3DSBT_ALL, &tok));
    CHECK(g_dev->SetTexture(1, NULL));
    a->Release();
    EXPECT(alive(ta), "a texture captured by D3DSBT_ALL died");
    CHECK(g_dev->ApplyStateBlock(tok));
    CHECK(g_dev->GetTexture(1, &g));
    EXPECT(g && tag_get(g) == 0xa1u, "ApplyStateBlock bound another texture");
    g->Release();
    CHECK(g_dev->SetTexture(1, NULL));
    EXPECT(alive(ta), "the state block let go of its texture");
    CHECK(g_dev->DeleteStateBlock(tok));
    EXPECT(!alive(ta), "the texture outlived its state block");

    CHECK(g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &b));
    tag_set(b, 0xb2u);
    Tracker *tb = track(b, 7);
    CHECK(g_dev->BeginStateBlock());
    CHECK(g_dev->SetTexture(2, b));
    CHECK(g_dev->EndStateBlock(&tok2));
    g = (IDirect3DBaseTexture8 *)1;
    CHECK(g_dev->GetTexture(2, &g));
    EXPECT(!g, "recording a state block bound the texture");
    b->Release();
    EXPECT(alive(tb), "a recorded texture died");
    CHECK(g_dev->ApplyStateBlock(tok2));
    CHECK(g_dev->GetTexture(2, &g));
    EXPECT(g && tag_get(g) == 0xb2u, "the recorded state block bound another texture");
    g->Release();
    CHECK(g_dev->SetTexture(2, NULL));
    CHECK(g_dev->DeleteStateBlock(tok2));
    EXPECT(!alive(tb), "the recorded texture outlived its state block");

    CHECK(g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c));
    Tracker *tc = track(c, 8);
    CHECK(g_dev->SetTexture(3, c));
    CHECK(g_dev->CreateStateBlock(D3DSBT_PIXELSTATE, &tok3));
    CHECK(g_dev->SetTexture(3, NULL));
    c->Release();
    EXPECT(!alive(tc), "a pixel state block kept a texture alive");
    CHECK(g_dev->DeleteStateBlock(tok3));
    return 1;
}

/* A released device lets go of everything it keeps alive. */
static int life_device_end(void)
{
    IDirect3DDevice8 *dev = NULL;
    IDirect3DTexture8 *t = NULL;
    IDirect3DSurface8 *bb = NULL;
    D3DPRESENT_PARAMETERS pp = g_pp;

    CHECK(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                              &dev));
    if (!check_hr("CreateTexture", dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) ||
        !check_hr("GetBackBuffer", dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        dev->Release();
        return 0;
    }
    Tracker *tt = track(t, 9), *tb = track(bb, 10);
    dev->SetTexture(0, t);
    t->Release();
    bb->Release();
    ULONG r = dev->Release();
    EXPECT(r == 0, "second device Release returned %lu", r);
    EXPECT(!alive(tt) && !alive(tb), "the device's end left its bound texture (%d) or back buffer (%d) alive",
           alive(tt), alive(tb));
    return 1;
}

/* Reset drops every binding and all state blocks. */
static int life_reset(void)
{
    IDirect3DTexture8 *t = NULL;
    IDirect3DBaseTexture8 *g = NULL;
    DWORD tok = 0;

    CHECK(g_dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t));
    Tracker *tt = track(t, 11);
    CHECK(g_dev->SetTexture(0, t));
    t->Release();
    CHECK(g_dev->CreateStateBlock(D3DSBT_PIXELSTATE, &tok));
    CHECK(g_dev->Reset(&g_pp));
    EXPECT(!alive(tt), "a released texture bound across Reset survived it");
    g = (IDirect3DBaseTexture8 *)1;
    CHECK(g_dev->GetTexture(0, &g));
    EXPECT(!g, "texture stage 0 still bound after Reset");
    EXPECT(g_dev->DeleteStateBlock(tok) == D3DERR_INVALIDCALL, "a state block survived Reset");
    return 1;
}

/* Native object lifetime games rely on (Need for Speed: Underground 2
 * releases bound objects twice when its video options change), measured on
 * the stock d3d8.dll.  Released pointers are only compared, never used. */
static int scene_bound_lifetime(void)
{
    return life_texture() && life_buffers() && life_targets() && life_state_blocks() && life_device_end() &&
           life_reset();
}

/* The display mode list games build resolution menus from, as the stock
 * d3d8.dll returns it (measured): D3D8 display formats only, grouped
 * X8R8G8B8, R5G6B5, X1R5G5B5 in that order (D3D9's per-format lists,
 * concatenated, so each group keeps the driver's own order, which need not
 * be ascending), no mode twice; past the end, or on a missing adapter,
 * D3DERR_INVALIDCALL with the mode untouched.  Whether 16-bit modes appear
 * at all is the system's per-process choice. */
static int scene_adapter_modes(void)
{
    UINT n = g_d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT), counts[3] = {0, 0, 0}, group = 0;
    D3DDISPLAYMODE m;
    int prev_rank = -1;
    for (UINT i = 0; i < n; ++i) {
        if (FAILED(g_d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, i, &m))) {
            printf("    EnumAdapterModes(%u) failed\n", i);
            return 0;
        }
        int rank = m.Format == D3DFMT_X8R8G8B8 ? 0 : m.Format == D3DFMT_R5G6B5 ? 1 : m.Format == D3DFMT_X1R5G5B5 ? 2 : -1;
        if (rank < 0 || rank < prev_rank) {
            printf("    mode %u: %ux%u@%u format %u out of place\n", i, m.Width, m.Height, m.RefreshRate, m.Format);
            return 0;
        }
        if (rank != prev_rank)
            group = i;
        for (UINT j = group; j < i; ++j) {
            D3DDISPLAYMODE o;
            if (SUCCEEDED(g_d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, j, &o)) && o.Width == m.Width &&
                o.Height == m.Height && o.RefreshRate == m.RefreshRate) {
                printf("    mode %u: %ux%u@%u format %u repeats mode %u\n", i, m.Width, m.Height, m.RefreshRate,
                       m.Format, j);
                return 0;
            }
        }
        ++counts[rank];
        prev_rank = rank;
    }
    memset(&m, 0xcc, sizeof(m));
    HRESULT hr = g_d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, n, &m);
    HRESULT hr_bad = g_d3d->EnumAdapterModes(g_d3d->GetAdapterCount(), 0, &m);
    printf("    %u display modes: %u X8R8G8B8, %u R5G6B5, %u X1R5G5B5\n", n, counts[0], counts[1], counts[2]);
    if (!counts[0] || hr != D3DERR_INVALIDCALL || hr_bad != D3DERR_INVALIDCALL || m.Width != 0xccccccccu ||
        g_d3d->GetAdapterModeCount(g_d3d->GetAdapterCount())) {
        printf("    past the end: hr %08lx, missing adapter: hr %08lx\n", (unsigned long)hr, (unsigned long)hr_bad);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void run(const char *name, int (*fn)(void))
{
    if (g_only) {
        /* Comma-separated scene list. */
        size_t n = strlen(name);
        const char *p = g_only;
        int found = 0;
        while (p && *p) {
            if (!strncmp(p, name, n) && (p[n] == ',' || p[n] == 0))
                found = 1;
            p = strchr(p, ',');
            if (p)
                ++p;
        }
        if (!found)
            return;
    }
    pump();
    ++g_scenes;
    int ok = fn();
    if (!ok)
        ++g_failures;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
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

int main(int argc, char **argv)
{
    int expect_local = 0, expect_stock = 0, expect_fallback = 0, dump_caps = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--expect-local"))
            expect_local = 1;
        else if (!strcmp(argv[i], "--expect-stock"))
            expect_stock = 1;
        else if (!strcmp(argv[i], "--expect-fallback"))
            expect_fallback = 1; /* qd3d8's d3d8.dll beside us hands over to qd3d8_orig.dll */
        else if (!strcmp(argv[i], "--dump-caps"))
            dump_caps = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            g_only = argv[++i];
    }
    expect_local |= expect_fallback;

    /* Which d3d8.dll did the loader bind? */
    char mod[MAX_PATH] = "", exe[MAX_PATH] = "", sys[MAX_PATH] = "";
    GetModuleFileNameA(GetModuleHandleA("d3d8.dll"), mod, MAX_PATH);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    GetSystemDirectoryA(sys, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash)
        slash[1] = 0;
    int local = _strnicmp(mod, exe, strlen(exe)) == 0 && !strchr(mod + strlen(exe), '\\');
    printf("d3d8.dll loaded from: %s (%s)\n", mod, local ? "beside the executable" : "not beside the executable");
    if ((expect_local && !local) || (expect_stock && local)) {
        printf("FAIL: d3d8.dll location does not match %s\n", expect_local ? "--expect-local" : "--expect-stock");
        return 2;
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qd3d8_smoke";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("qd3d8_smoke", "qd3d8 smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                          r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) {
        printf("FAIL: CreateWindow\n");
        return 1;
    }
    pump();

    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_d3d) {
        printf("FAIL: Direct3DCreate8 returned NULL\n");
        return 1;
    }
    {
        char d9[MAX_PATH] = "";
        HMODULE h9 = GetModuleHandleA("d3d9.dll");
        if (h9)
            GetModuleFileNameA(h9, d9, MAX_PATH);
        printf("d3d9.dll in process after Direct3DCreate8: %s\n", h9 ? d9 : "(not loaded)");
    }
    {
        /* Which runtime made the object: qd3d8 itself, or the stock runtime
         * it fell back to. */
        HMODULE owner = vtable_module(g_d3d), orig = GetModuleHandleA("qd3d8_orig.dll");
        char path[MAX_PATH] = "";
        GetModuleFileNameA(owner, path, MAX_PATH);
        printf("IDirect3D8 object from: %s\n", owner ? path : "(unknown)");
        if (expect_fallback && (!orig || owner != orig)) {
            printf("FAIL: --expect-fallback, but the object is not from qd3d8_orig.dll\n");
            return 2;
        }
        if (expect_local && !expect_fallback && owner != GetModuleHandleA("d3d8.dll")) {
            printf("FAIL: --expect-local, but the object is not from the d3d8.dll beside the executable\n");
            return 2;
        }
    }
    D3DADAPTER_IDENTIFIER8 id;
    if (SUCCEEDED(g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &id)))
        printf("adapter: %s, %u display modes\n", id.Description, g_d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT));

    D3DDISPLAYMODE dm;
    if (FAILED(g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm))) {
        printf("FAIL: GetAdapterDisplayMode\n");
        return 1;
    }
    memset(&g_pp, 0, sizeof(g_pp));
    g_pp.BackBufferWidth = W;
    g_pp.BackBufferHeight = H;
    g_pp.BackBufferFormat = dm.Format;
    g_pp.BackBufferCount = 1;
    g_pp.SwapEffect = D3DSWAPEFFECT_COPY;
    g_pp.hDeviceWindow = g_wnd;
    g_pp.Windowed = TRUE;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    g_pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    g_pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    /* MULTITHREADED exercises qd3d8's device lock path. */
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd,
                                     D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &g_pp, &g_dev);
    g_mixed = SUCCEEDED(hr);
    if (FAILED(hr))
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &g_pp, &g_dev);
    if (FAILED(hr)) {
        printf("FAIL: CreateDevice hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    printf("device: %s vertex processing, back buffer format %d\n", g_mixed ? "mixed" : "software",
           (int)g_pp.BackBufferFormat);
    if (dump_caps) {
        D3DCAPS8 c;
        if (SUCCEEDED(g_dev->GetDeviceCaps(&c))) {
            const DWORD *f = (const DWORD *)&c;
            for (unsigned i = 0; i < sizeof(c) / sizeof(DWORD); ++i)
                printf("caps[%2u] = %08lx\n", i, (unsigned long)f[i]);
        }
    }

    run("adapter_modes", scene_adapter_modes);
    run("clear", scene_clear);
    run("drawprimitiveup", scene_dpup);
    run("vb_ib_basevertexindex", scene_vbib);
    run("texture_point_filter_tss", scene_texture);
    run("vertex_shader_vs11_decl_const", scene_vs);
    run("pixel_shader_ps11", scene_ps);
    run("render_to_texture_copyrects", scene_rtt);
    run("state_block", scene_stateblock);
    run("render_and_sampler_states", scene_states);
    run("vertex_declaration_fixed_function", scene_ff_decl);
    run("vertex_shader_vs10_promotion", scene_vs10);
    run("copyrects_pool_paths", scene_copyrects);
    run("cube_volume_dxt_resources", scene_cube_volume);
    run("reset", scene_reset);
    run("bound_lifetime", scene_bound_lifetime);
    run("getfrontbuffer", scene_frontbuffer);

    ULONG dev_refs = g_dev->Release();
    ULONG d3d_refs = g_d3d->Release();
    printf("final refcounts: device %lu, d3d %lu\n", dev_refs, d3d_refs);
    DestroyWindow(g_wnd);
    printf("%s: %d/%d scenes passed\n", g_failures ? "FAIL" : "PASS", g_scenes - g_failures, g_scenes);
    return g_failures ? 1 : 0;
}
