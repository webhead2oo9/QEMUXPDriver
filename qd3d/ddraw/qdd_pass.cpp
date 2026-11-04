/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: Direct3D 9 passes the front end draws itself.
 *
 * Depth.  A z-buffer's Direct3D 9 copy is a D16_LOCKABLE or D32F_LOCKABLE
 * depth stencil when the format has no stencil and the device has one,
 * read back by locking it.  With stencil it is level 0 of an INTZ texture,
 * whose depth a pixel shader resolves into an R32F render target.  Writes
 * always draw the depth through oDepth (CPU writes into a lockable depth
 * buffer do not reach the depth test).  Stencil moves bit by bit through
 * stencil-tested draws: REPLACE where a shader keeps the pixels with the bit
 * set, and additive constant colors where the stencil has it.
 *
 * P8.  A P8 copy is drawn from L8 indices through a 256x1 palette texture.
 *
 * Gamma.  SetGammaRamp applies to every QD3D frame: presentation draws the
 * frame through a 256x1 lookup texture (three dependent reads).
 *
 * The shaders are ps_2_0 token streams (no compiler at run time).  Every
 * pass saves and restores the device state it touches, so the Direct3D 7
 * device's binding caches stay valid; it runs outside the Direct3D 7
 * device's scene (qdd_d3d7_flush ends that).
 */
#include "qdd_priv.h"

#define QDD_FMT_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))

/* ------------------------------------------------------------------------ */
/* Shaders                                                                   */

enum {
    QDD_PS_RESOLVE, QDD_PS_DEPTH, QDD_PS_DEPTH_X, QDD_PS_GAMMA, QDD_PS_P8, QDD_PS_CONST, QDD_PS_STENCIL_BIT,
    QDD_PS_ALL
};

/* Token fields: dst/src register = 0x80000000 | type bits | number, write
 * mask << 16 (dst) or swizzle << 16 (src).  Types: r 0x00000000, c
 * 0x20000000, t 0x30000000, oC 0x00000800, oDepth 0x10000800, s 0x20000800. */
static const DWORD g_ps_resolve[] = {
    0xffff0200,                                     /* ps_2_0 */
    0x0200001f, 0x80000000, 0xb0030000,             /* dcl t0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, /* texld r0, t0, s0 */
    0x02000001, 0x800f0800, 0x80e40000,             /* mov oC0, r0 */
    0x0000ffff,
};

/* Writes the R32F texel as depth; oC0 is required by ps_2_0 (color writes
 * are masked off by the pass). */
static const DWORD g_ps_depth[] = {
    0xffff0200,
    0x0200001f, 0x80000000, 0xb0030000,             /* dcl t0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, /* texld r0, t0, s0 */
    0x02000001, 0x900f0800, 0x80000000,             /* mov oDepth, r0.x */
    0x02000001, 0x800f0800, 0x80e40000,             /* mov oC0, r0 */
    0x0000ffff,
};

/* The same with an .x write mask on oDepth, for runtimes that want one. */
static const DWORD g_ps_depth_x[] = {
    0xffff0200,
    0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x90000000, 0xa00f0800,
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
    0x02000001, 0x90010800, 0x80000000,             /* mov oDepth.x, r0.x */
    0x02000001, 0x800f0800, 0x80e40000,
    0x0000ffff,
};

/* s0: the frame; s1: the 256x1 ramp (red, green, blue channels). */
static const DWORD g_ps_gamma[] = {
    0xffff0200,
    0x05000051, 0xa00f0000, 0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000, /* def c0, 255/256, 0.5/256, 0.5, 1 */
    0x0200001f, 0x80000000, 0xb0030000,             /* dcl t0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x0200001f, 0x90000000, 0xa00f0801,             /* dcl_2d s1 */
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, /* texld r0, t0, s0 */
    0x04000004, 0x800f0000, 0x80e40000, 0xa0000000, 0xa0550000, /* mad r0, r0, c0.x, c0.y */
    0x02000001, 0x800f0001, 0xa0aa0000,             /* mov r1, c0.z */
    0x02000001, 0x80010001, 0x80000000,             /* mov r1.x, r0.x */
    0x03000042, 0x800f0002, 0x80e40001, 0xa0e40801, /* texld r2, r1, s1 */
    0x02000001, 0x80010001, 0x80550000,             /* mov r1.x, r0.y */
    0x03000042, 0x800f0003, 0x80e40001, 0xa0e40801, /* texld r3, r1, s1 */
    0x02000001, 0x80010001, 0x80aa0000,             /* mov r1.x, r0.z */
    0x03000042, 0x800f0004, 0x80e40001, 0xa0e40801, /* texld r4, r1, s1 */
    0x02000001, 0x80010005, 0x80000002,             /* mov r5.x, r2.x */
    0x02000001, 0x80020005, 0x80550003,             /* mov r5.y, r3.y */
    0x02000001, 0x80040005, 0x80aa0004,             /* mov r5.z, r4.z */
    0x02000001, 0x80080005, 0xa0ff0000,             /* mov r5.w, c0.w */
    0x02000001, 0x800f0800, 0x80e40005,             /* mov oC0, r5 */
    0x0000ffff,
};

/* s0: the 8-bit indices (L8), s1: the 256x1 palette. */
static const DWORD g_ps_p8[] = {
    0xffff0200,
    0x05000051, 0xa00f0000, 0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000, /* def c0, 255/256, 0.5/256, 0.5, 1 */
    0x0200001f, 0x80000000, 0xb0030000,             /* dcl t0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x0200001f, 0x90000000, 0xa00f0801,             /* dcl_2d s1 */
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, /* texld r0, t0, s0 */
    0x04000004, 0x80010000, 0x80000000, 0xa0000000, 0xa0550000, /* mad r0.x, r0.x, c0.x, c0.y */
    0x02000001, 0x80020000, 0xa0aa0000,             /* mov r0.y, c0.z */
    0x03000042, 0x800f0001, 0x80e40000, 0xa0e40801, /* texld r1, r0, s1 */
    0x02000001, 0x800f0800, 0x80e40001,             /* mov oC0, r1 */
    0x0000ffff,
};

/* Stencil readback: a constant color (c0) per stencil bit, added up. */
static const DWORD g_ps_const[] = {
    0xffff0200,
    0x02000001, 0x800f0800, 0xa0e40000, /* mov oC0, c0 */
    0x0000ffff,
};

/* Stencil upload: keeps the pixels whose stencil byte (L8 in s0) has the
 * bit c1.x = 255 / 2^(bit + 1) selects: frac(v * c1.x) >= 0.5. */
static const DWORD g_ps_stencil_bit[] = {
    0xffff0200,
    0x05000051, 0xa00f0000, 0xbefef9db, 0x00000000, 0x00000000, 0x00000000, /* def c0, -0.498, 0, 0, 0 */
    0x0200001f, 0x80000000, 0xb0030000,             /* dcl t0.xy */
    0x0200001f, 0x90000000, 0xa00f0800,             /* dcl_2d s0 */
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, /* texld r0, t0, s0 */
    0x03000005, 0x800f0001, 0x80000000, 0xa0000001, /* mul r1, r0.x, c1.x */
    0x02000013, 0x800f0001, 0x80e40001,             /* frc r1, r1 */
    0x03000002, 0x800f0001, 0x80e40001, 0xa0000000, /* add r1, r1, c0.x */
    0x01000041, 0x800f0001,                         /* texkill r1 */
    0x02000001, 0x800f0800, 0x80e40000,             /* mov oC0, r0 */
    0x0000ffff,
};

static const DWORD *const g_ps_code[QDD_PS_ALL] = {g_ps_resolve, g_ps_depth, g_ps_depth_x, g_ps_gamma,
                                                   g_ps_p8,      g_ps_const, g_ps_stencil_bit};

struct QddPasses {
    IDirect3DPixelShader9 *ps[QDD_PS_ALL];
    bool ps_failed[QDD_PS_ALL];
    IDirect3DTexture9 *frame;   /* gamma: copy of the presented rectangle */
    UINT frame_w, frame_h;
    IDirect3DTexture9 *lut, *lut_sys;
    DWORD lut_version;
    bool lut_valid;
};

struct QddDepthXfer {
    IDirect3DSurface9 *rt;      /* R32F: resolve target, upload color target */
    IDirect3DSurface9 *rt_sys;
    IDirect3DTexture9 *src;     /* R32F: depth the upload draws */
    IDirect3DTexture9 *src_sys;
    IDirect3DSurface9 *srt, *srt_sys;   /* A8R8G8B8: stencil read back, bit by bit */
    IDirect3DTexture9 *stex, *stex_sys; /* L8: stencil the upload draws */
};

/* A P8 surface's Direct3D 9 copy is drawn from its indices (L8, a quarter
 * of the 32-bit frame) through its palette (256x1): palette changes upload
 * 1 KiB and redraw, frames upload only the indices. */
struct QddIndexXfer {
    IDirect3DTexture9 *idx, *idx_sys;
    IDirect3DTexture9 *pal, *pal_sys;
    bool failed, idx_valid, pal_valid;
    PALETTEENTRY pal_shadow[256];
};

static QddPasses *qdd_passes(QddDraw *dd)
{
    if (!dd->passes)
        dd->passes = (QddPasses *)calloc(1, sizeof(QddPasses));
    return dd->passes;
}

static IDirect3DPixelShader9 *qdd_ps(QddDraw *dd, int which)
{
    QddPasses *p = qdd_passes(dd);
    if (!p || !dd->dev9)
        return NULL;
    if (!p->ps[which] && !p->ps_failed[which]) {
        HRESULT hr = dd->dev9->CreatePixelShader(g_ps_code[which], &p->ps[which]);
        if (FAILED(hr)) {
            p->ps[which] = NULL;
            p->ps_failed[which] = true;
            qdd_event("qddraw: pixel shader %d not accepted, hr 0x%08lx", which, (unsigned long)hr);
        }
    }
    return p->ps[which];
}

template <typename T> static void qdd_release(T *&p)
{
    if (p) {
        p->Release();
        p = NULL;
    }
}

void qdd_pass_buffer_free(QddBuffer *b)
{
    if (QddIndexXfer *px = b->px) {
        qdd_release(px->idx);
        qdd_release(px->idx_sys);
        qdd_release(px->pal);
        qdd_release(px->pal_sys);
        free(px);
        b->px = NULL;
    }
    QddDepthXfer *x = b->zx;
    if (!x)
        return;
    qdd_release(x->rt);
    qdd_release(x->rt_sys);
    qdd_release(x->src);
    qdd_release(x->src_sys);
    qdd_release(x->srt);
    qdd_release(x->srt_sys);
    qdd_release(x->stex);
    qdd_release(x->stex_sys);
    free(x);
    b->zx = NULL;
}

void qdd_pass_free(QddDraw *dd)
{
    QddPasses *p = dd->passes;
    if (!p)
        return;
    for (int i = 0; i < QDD_PS_ALL; ++i)
        qdd_release(p->ps[i]);
    qdd_release(p->frame);
    qdd_release(p->lut);
    qdd_release(p->lut_sys);
    free(p);
    dd->passes = NULL;
}

/* ------------------------------------------------------------------------ */
/* Device state the passes touch                                             */

static const D3DRENDERSTATETYPE g_rs[] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
    D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE,
    D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE, D3DRS_CLIPPLANEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS,
    D3DRS_TWOSIDEDSTENCILMODE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
    D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
};
#define QDD_NRS (sizeof(g_rs) / sizeof(g_rs[0]))
static const D3DSAMPLERSTATETYPE g_ss[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
};
#define QDD_NSS (sizeof(g_ss) / sizeof(g_ss[0]))

struct QddSaved {
    IDirect3DSurface9 *rt, *ds;
    D3DVIEWPORT9 vp;
    IDirect3DPixelShader9 *ps;
    IDirect3DVertexShader9 *vs;
    IDirect3DVertexDeclaration9 *decl;
    DWORD fvf;
    IDirect3DVertexBuffer9 *vb;
    UINT vb_off, vb_stride;
    IDirect3DBaseTexture9 *tex[2];
    DWORD rs[QDD_NRS];
    DWORD ss[2][QDD_NSS];
    DWORD tci[2], ttf[2];
    float psc[8];              /* pixel shader constants c0, c1 */
};

static void qdd_save(IDirect3DDevice9 *d, QddSaved *s)
{
    memset(s, 0, sizeof(*s));
    d->GetRenderTarget(0, &s->rt);
    d->GetDepthStencilSurface(&s->ds);
    d->GetViewport(&s->vp);
    d->GetPixelShader(&s->ps);
    d->GetVertexShader(&s->vs);
    d->GetVertexDeclaration(&s->decl);
    d->GetFVF(&s->fvf);
    d->GetStreamSource(0, &s->vb, &s->vb_off, &s->vb_stride);
    for (UINT i = 0; i < QDD_NRS; ++i)
        d->GetRenderState(g_rs[i], &s->rs[i]);
    for (DWORD st = 0; st < 2; ++st) {
        d->GetTexture(st, &s->tex[st]);
        for (UINT i = 0; i < QDD_NSS; ++i)
            d->GetSamplerState(st, g_ss[i], &s->ss[st][i]);
        d->GetTextureStageState(st, D3DTSS_TEXCOORDINDEX, &s->tci[st]);
        d->GetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, &s->ttf[st]);
    }
    d->GetPixelShaderConstantF(0, s->psc, 2);
}

static void qdd_restore(IDirect3DDevice9 *d, QddSaved *s)
{
    d->SetRenderTarget(0, s->rt);
    d->SetDepthStencilSurface(s->ds);
    d->SetViewport(&s->vp);
    d->SetPixelShader(s->ps);
    d->SetVertexShader(s->vs);
    if (s->fvf)
        d->SetFVF(s->fvf);
    else if (s->decl)
        d->SetVertexDeclaration(s->decl);
    d->SetStreamSource(0, s->vb, s->vb_off, s->vb_stride);
    for (UINT i = 0; i < QDD_NRS; ++i)
        d->SetRenderState(g_rs[i], s->rs[i]);
    for (DWORD st = 0; st < 2; ++st) {
        d->SetTexture(st, s->tex[st]);
        for (UINT i = 0; i < QDD_NSS; ++i)
            d->SetSamplerState(st, g_ss[i], s->ss[st][i]);
        d->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, s->tci[st]);
        d->SetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, s->ttf[st]);
    }
    d->SetPixelShaderConstantF(0, s->psc, 2);
    qdd_release(s->rt);
    qdd_release(s->ds);
    qdd_release(s->ps);
    qdd_release(s->vs);
    qdd_release(s->decl);
    qdd_release(s->vb);
    qdd_release(s->tex[0]);
    qdd_release(s->tex[1]);
}

/* The common pass state: no depth/stencil/blend/fog, all color channels
 * (color_write) or none, point-sampled clamped stages 0 and 1. */
static void qdd_pass_state(IDirect3DDevice9 *d, DWORD color_write, bool depth_write)
{
    d->SetRenderState(D3DRS_ZENABLE, depth_write ? D3DZB_TRUE : D3DZB_FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, depth_write);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, color_write);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_DEPTHBIAS, 0);
    d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    for (DWORD st = 0; st < 2; ++st) {
        d->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, FALSE);
        d->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, st);
        d->SetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    d->SetVertexShader(NULL);
}

/* A w x h quad covering the render target, texels mapped 1:1. */
static void qdd_quad(IDirect3DDevice9 *d, UINT w, UINT h)
{
    struct V {
        float x, y, z, rhw, u, v;
    } q[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {(float)w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, (float)h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {(float)w - 0.5f, (float)h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    if (SUCCEEDED(d->BeginScene())) {
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
        d->EndScene();
    }
}

/* ------------------------------------------------------------------------ */
/* Depth                                                                     */

static UINT qdd_ctz(DWORD m)
{
    UINT n = 0;
    while (m && !(m & 1)) {
        m >>= 1;
        ++n;
    }
    return n;
}

/* Moves depth between the system-memory copy (the surface's format) and
 * rows of floats (D32F_LOCKABLE, R32F) or 16-bit unorm (D16_LOCKABLE). */
static void qdd_depth_rows(QddSurface *s, BYTE *g, LONG gpitch, bool g16, bool to_gpu)
{
    QddBuffer *b = s->buf;
    const QddFormat *f = s->fmt;
    DWORD zm = f->mask_r;
    UINT shift = qdd_ctz(zm);
    DWORD zmax = zm >> shift;
    double scale = 1.0 / (double)zmax;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    /* The x87 unit may run at 24-bit precision (Direct3D sets it so unless
     * the application asks to preserve it): products are clamped in
     * floating point before the integer conversion. */
    double dmax = (double)zmax;
    for (UINT y = 0; y < h; ++y) {
        BYTE *sys = b->mem.ptr + (size_t)y * b->pitch;
        BYTE *gr = g + (size_t)y * gpitch;
        for (UINT x = 0; x < w; ++x) {
            DWORD p = f->bpp == 16 ? ((WORD *)sys)[x] : ((DWORD *)sys)[x];
            if (to_gpu) {
                double z = (double)((p & zm) >> shift) * scale;
                if (z > 1.0)
                    z = 1.0;
                if (g16) {
                    double v = z * 65535.0 + 0.5;
                    ((WORD *)gr)[x] = v >= 65535.0 ? (WORD)65535 : (WORD)v;
                } else {
                    ((float *)gr)[x] = (float)z;
                }
                continue;
            }
            double z = g16 ? ((WORD *)gr)[x] / 65535.0 : (double)((float *)gr)[x];
            if (!(z > 0.0))
                z = 0.0;
            else if (z > 1.0)
                z = 1.0;
            double v = z * dmax + 0.5;
            DWORD zi = v >= dmax ? zmax : (DWORD)v;
            p = (p & ~zm) | ((zi << shift) & zm);
            if (f->bpp == 16)
                ((WORD *)sys)[x] = (WORD)p;
            else
                ((DWORD *)sys)[x] = p;
        }
    }
}

static bool qdd_depth_lockable(D3DFORMAT f)
{
    return f == D3DFMT_D32F_LOCKABLE || f == D3DFMT_D16_LOCKABLE;
}

/* The INTZ transfer surfaces of s (created once). */
static QddDepthXfer *qdd_zx(QddSurface *s)
{
    QddBuffer *b = s->buf;
    if (b->zx)
        return b->zx;
    IDirect3DDevice9 *d = s->dd->dev9;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    QddDepthXfer *x = (QddDepthXfer *)calloc(1, sizeof(*x));
    if (!x)
        return NULL;
    b->zx = x;
    if (FAILED(d->CreateRenderTarget(w, h, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &x->rt, NULL)) ||
        FAILED(d->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &x->rt_sys, NULL)) ||
        FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &x->src, NULL)) ||
        FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &x->src_sys, NULL))) {
        qdd_event("qddraw: depth transfer surfaces (%ux%u R32F) not available", w, h);
        qdd_pass_buffer_free(b);
        return NULL;
    }
    return x;
}

/* Stencil: the INTZ copy's stencil moves bit by bit through stencil tests,
 * as no API reads or writes it directly (stencil formats only). */

static UINT qdd_stencil_bits(const QddFormat *f)
{
    UINT n = 0;
    for (DWORD m = f->mask_a; m; m &= m - 1)
        ++n;
    return n;
}

static bool qdd_stencil_surfaces(QddSurface *s, QddDepthXfer *x)
{
    if (x->srt)
        return true;
    IDirect3DDevice9 *d = s->dd->dev9;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    if (FAILED(d->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &x->srt, NULL)) ||
        FAILED(d->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &x->srt_sys, NULL)) ||
        FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &x->stex, NULL)) ||
        FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &x->stex_sys, NULL))) {
        qdd_release(x->srt);
        qdd_release(x->srt_sys);
        qdd_release(x->stex);
        qdd_release(x->stex_sys);
        return false;
    }
    return true;
}

/* Writes the system copy's stencil into the bound INTZ depth stencil: clear,
 * then per bit REPLACE where the bit is set (the shader kills the rest). */
static void qdd_stencil_upload_passes(QddSurface *s, QddDepthXfer *x, IDirect3DDevice9 *d, IDirect3DPixelShader9 *ps)
{
    QddBuffer *b = s->buf;
    const QddFormat *f = s->fmt;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight, shift = qdd_ctz(f->mask_a), n = qdd_stencil_bits(f);
    D3DLOCKED_RECT lr;
    if (FAILED(x->stex_sys->LockRect(0, &lr, NULL, 0)))
        return;
    for (UINT y = 0; y < h; ++y) {
        const BYTE *sys = b->mem.ptr + (size_t)y * b->pitch;
        BYTE *t = (BYTE *)lr.pBits + (size_t)y * lr.Pitch;
        for (UINT i = 0; i < w; ++i) {
            DWORD p = f->bpp == 16 ? ((const WORD *)sys)[i] : ((const DWORD *)sys)[i];
            t[i] = (BYTE)((p & f->mask_a) >> shift);
        }
    }
    x->stex_sys->UnlockRect(0);
    if (FAILED(d->UpdateTexture(x->stex_sys, x->stex)))
        return;
    d->SetRenderTarget(0, x->srt);
    d->SetDepthStencilSurface(b->gpu);
    d->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0);
    qdd_pass_state(d, 0, false);
    d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    d->SetRenderState(D3DRS_STENCILREF, 0xff);
    d->SetRenderState(D3DRS_STENCILMASK, 0xff);
    d->SetTexture(0, x->stex);
    d->SetTexture(1, NULL);
    d->SetPixelShader(ps);
    for (UINT bit = 0; bit < n; ++bit) {
        float c[4] = {255.0f / (float)(2u << bit), 0.0f, 0.0f, 0.0f};
        d->SetPixelShaderConstantF(1, c, 1);
        d->SetRenderState(D3DRS_STENCILWRITEMASK, 1u << bit);
        qdd_quad(d, w, h);
    }
}

/* Adds 2^bit / 255 into x->srt's red channel where the stencil has the bit:
 * afterwards red holds the stencil value. */
static void qdd_stencil_readback_passes(QddSurface *s, QddDepthXfer *x, IDirect3DDevice9 *d,
                                        IDirect3DPixelShader9 *ps)
{
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight, n = qdd_stencil_bits(s->fmt);
    d->SetRenderTarget(0, x->srt);
    d->SetDepthStencilSurface(s->buf->gpu);
    d->SetTexture(0, NULL);
    d->SetTexture(1, NULL);
    d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 0.0f, 0);
    qdd_pass_state(d, 0xf, false);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    d->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    d->SetPixelShader(ps);
    for (UINT bit = 0; bit < n; ++bit) {
        float c[4] = {(float)(1u << bit) / 255.0f, 0.0f, 0.0f, 0.0f};
        d->SetPixelShaderConstantF(0, c, 1);
        d->SetRenderState(D3DRS_STENCILREF, 1u << bit);
        d->SetRenderState(D3DRS_STENCILMASK, 1u << bit);
        qdd_quad(d, w, h);
    }
}

static void qdd_stencil_store(QddSurface *s, QddDepthXfer *x, IDirect3DDevice9 *d)
{
    QddBuffer *b = s->buf;
    const QddFormat *f = s->fmt;
    D3DLOCKED_RECT lr;
    if (FAILED(d->GetRenderTargetData(x->srt, x->srt_sys)) || FAILED(x->srt_sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight, shift = qdd_ctz(f->mask_a);
    for (UINT y = 0; y < h; ++y) {
        BYTE *sys = b->mem.ptr + (size_t)y * b->pitch;
        const DWORD *src = (const DWORD *)((const BYTE *)lr.pBits + (size_t)y * lr.Pitch);
        for (UINT i = 0; i < w; ++i) {
            DWORD v = ((src[i] >> 16) & 0xff) << shift & f->mask_a;
            if (f->bpp == 16)
                ((WORD *)sys)[i] = (WORD)((((WORD *)sys)[i] & ~f->mask_a) | v);
            else
                ((DWORD *)sys)[i] = (((DWORD *)sys)[i] & ~f->mask_a) | v;
        }
    }
    x->srt_sys->UnlockRect();
}
bool qdd_depth_upload(QddSurface *s)
{
    QddBuffer *b = s->buf;
    QddDraw *dd = s->dd;
    if (!b->gpu || !dd || !dd->dev9)
        return false;
    /* Always through a draw: CPU writes into a lockable depth buffer are
     * ignored by the depth test on current drivers (Measured on the RTX
     * 4090 host: LockRect zeros, then a z=0.5 LESSEQUAL quad still passes
     * there), so the depth is drawn with oDepth from an R32F texture. */
    D3DLOCKED_RECT lr;
    if (!qdd_depth_lockable(b->gpu_fmt) && b->gpu_fmt != QDD_FMT_INTZ)
        return false;
    QddDepthXfer *x = qdd_zx(s);
    IDirect3DPixelShader9 *ps = qdd_ps(dd, QDD_PS_DEPTH);
    if (!ps)
        ps = qdd_ps(dd, QDD_PS_DEPTH_X);
    if (!x || !ps || FAILED(x->src_sys->LockRect(0, &lr, NULL, 0)))
        return false;
    qdd_depth_rows(s, (BYTE *)lr.pBits, lr.Pitch, false, true);
    x->src_sys->UnlockRect(0);
    IDirect3DDevice9 *d = dd->dev9;
    if (FAILED(d->UpdateTexture(x->src_sys, x->src)))
        return false;
    qdd_d3d7_flush(dd);
    QddSaved saved;
    qdd_save(d, &saved);
    d->SetRenderTarget(0, x->rt);
    d->SetDepthStencilSurface(b->gpu);
    qdd_pass_state(d, 0, true);
    d->SetTexture(0, x->src);
    d->SetTexture(1, NULL);
    d->SetPixelShader(ps);
    qdd_quad(d, s->desc.dwWidth, s->desc.dwHeight);
    if (b->gpu_fmt == QDD_FMT_INTZ && s->fmt->mask_a && qdd_stencil_surfaces(s, x)) {
        if (IDirect3DPixelShader9 *sps = qdd_ps(dd, QDD_PS_STENCIL_BIT))
            qdd_stencil_upload_passes(s, x, d, sps);
    }
    qdd_restore(d, &saved);
    return true;
}

bool qdd_depth_readback(QddSurface *s)
{
    QddBuffer *b = s->buf;
    QddDraw *dd = s->dd;
    if (!b->gpu || !dd || !dd->dev9)
        return false;
    D3DLOCKED_RECT lr;
    if (qdd_depth_lockable(b->gpu_fmt)) {
        if (FAILED(b->gpu->LockRect(&lr, NULL, D3DLOCK_READONLY)))
            return false;
        qdd_depth_rows(s, (BYTE *)lr.pBits, lr.Pitch, b->gpu_fmt == D3DFMT_D16_LOCKABLE, false);
        b->gpu->UnlockRect();
        return true;
    }
    if (b->gpu_fmt != QDD_FMT_INTZ || !b->gpu_tex)
        return false;
    QddDepthXfer *x = qdd_zx(s);
    IDirect3DPixelShader9 *ps = qdd_ps(dd, QDD_PS_RESOLVE);
    if (!x || !ps)
        return false;
    IDirect3DDevice9 *d = dd->dev9;
    qdd_d3d7_flush(dd);
    QddSaved saved;
    qdd_save(d, &saved);
    d->SetRenderTarget(0, x->rt);
    d->SetDepthStencilSurface(NULL);
    qdd_pass_state(d, 0xf, false);
    d->SetTexture(0, b->gpu_tex);
    d->SetTexture(1, NULL);
    d->SetPixelShader(ps);
    qdd_quad(d, s->desc.dwWidth, s->desc.dwHeight);
    bool stencil = s->fmt->mask_a && qdd_stencil_surfaces(s, x);
    IDirect3DPixelShader9 *cps = stencil ? qdd_ps(dd, QDD_PS_CONST) : NULL;
    if (cps)
        qdd_stencil_readback_passes(s, x, d, cps);
    qdd_restore(d, &saved);
    if (FAILED(d->GetRenderTargetData(x->rt, x->rt_sys)))
        return false;
    if (FAILED(x->rt_sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return false;
    qdd_depth_rows(s, (BYTE *)lr.pBits, lr.Pitch, false, false);
    x->rt_sys->UnlockRect();
    if (cps)
        qdd_stencil_store(s, x, d);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Gamma                                                                     */

bool qdd_gamma_pass(QddDraw *dd, IDirect3DSurface9 *src, const RECT *r, IDirect3DSurface9 *dst)
{
    IDirect3DDevice9 *d = dd->dev9;
    IDirect3DPixelShader9 *ps = qdd_ps(dd, QDD_PS_GAMMA);
    QddPasses *p = dd->passes;
    if (!d || !ps || !p)
        return false;
    UINT w = (UINT)(r->right - r->left), h = (UINT)(r->bottom - r->top);
    if (p->frame && (p->frame_w != w || p->frame_h != h))
        qdd_release(p->frame);
    if (!p->frame) {
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &p->frame, NULL))) {
            p->frame = NULL;
            return false;
        }
        p->frame_w = w;
        p->frame_h = h;
    }
    if (!p->lut) {
        if (FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &p->lut, NULL)) ||
            FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &p->lut_sys, NULL))) {
            qdd_release(p->lut);
            qdd_release(p->lut_sys);
            return false;
        }
        p->lut_valid = false;
    }
    if (!p->lut_valid || p->lut_version != dd->gamma_version) {
        D3DLOCKED_RECT lr;
        if (FAILED(p->lut_sys->LockRect(0, &lr, NULL, 0)))
            return false;
        DWORD *t = (DWORD *)lr.pBits;
        for (UINT i = 0; i < 256; ++i)
            t[i] = 0xff000000u | (DWORD)(dd->gamma.red[i] >> 8) << 16 | (DWORD)(dd->gamma.green[i] >> 8) << 8 |
                   (DWORD)(dd->gamma.blue[i] >> 8);
        p->lut_sys->UnlockRect(0);
        if (FAILED(d->UpdateTexture(p->lut_sys, p->lut)))
            return false;
        p->lut_version = dd->gamma_version;
        p->lut_valid = true;
    }
    IDirect3DSurface9 *fs = NULL;
    if (FAILED(p->frame->GetSurfaceLevel(0, &fs)))
        return false;
    HRESULT hr = d->StretchRect(src, r, fs, NULL, D3DTEXF_POINT);
    fs->Release();
    if (FAILED(hr))
        return false;
    qdd_d3d7_flush(dd);
    QddSaved saved;
    qdd_save(d, &saved);
    d->SetRenderTarget(0, dst);
    d->SetDepthStencilSurface(NULL);
    qdd_pass_state(d, 0xf, false);
    d->SetTexture(0, p->frame);
    d->SetTexture(1, p->lut);
    d->SetPixelShader(ps);
    qdd_quad(d, w, h);
    qdd_restore(d, &saved);
    return true;
}

/* ------------------------------------------------------------------------ */
/* P8                                                                        */

bool qdd_p8_upload(QddSurface *s, bool indices)
{
    QddBuffer *b = s->buf;
    QddDraw *dd = s->dd;
    if (!b->gpu || !dd || !dd->dev9 || s->fmt->cls != QFC_PAL || s->fmt->bpp != 8)
        return false;
    IDirect3DDevice9 *d = dd->dev9;
    IDirect3DPixelShader9 *ps = qdd_ps(dd, QDD_PS_P8);
    if (!ps)
        return false;
    UINT w = s->desc.dwWidth, h = s->desc.dwHeight;
    QddIndexXfer *x = b->px;
    if (!x) {
        x = (QddIndexXfer *)calloc(1, sizeof(*x));
        if (!x)
            return false;
        b->px = x;
        if (FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &x->idx, NULL)) ||
            FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &x->idx_sys, NULL)) ||
            FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &x->pal, NULL)) ||
            FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &x->pal_sys, NULL))) {
            qdd_event("qddraw: P8 index textures (%ux%u L8) not available; converting on the CPU", w, h);
            qdd_release(x->idx);
            qdd_release(x->idx_sys);
            qdd_release(x->pal);
            qdd_release(x->pal_sys);
            x->failed = true; /* remembered: the CPU conversion from now on */
        }
    }
    if (x->failed)
        return false;
    D3DLOCKED_RECT lr;
    if (indices || !x->idx_valid) {
        if (FAILED(x->idx_sys->LockRect(0, &lr, NULL, 0)))
            return false;
        for (UINT y = 0; y < h; ++y)
            memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, b->mem.ptr + (size_t)y * b->pitch, w);
        x->idx_sys->UnlockRect(0);
        if (FAILED(d->UpdateTexture(x->idx_sys, x->idx)))
            return false;
        x->idx_valid = true;
    }
    static const PALETTEENTRY black[256] = {};
    const PALETTEENTRY *pe = qdd_surf_pal_entries(s);
    if (!pe)
        pe = black;
    if (!x->pal_valid || memcmp(x->pal_shadow, pe, sizeof(x->pal_shadow))) {
        if (FAILED(x->pal_sys->LockRect(0, &lr, NULL, 0)))
            return false;
        DWORD *t = (DWORD *)lr.pBits;
        for (UINT i = 0; i < 256; ++i)
            t[i] = 0xff000000u | (DWORD)pe[i].peRed << 16 | (DWORD)pe[i].peGreen << 8 | pe[i].peBlue;
        x->pal_sys->UnlockRect(0);
        if (FAILED(d->UpdateTexture(x->pal_sys, x->pal)))
            return false;
        memcpy(x->pal_shadow, pe, sizeof(x->pal_shadow));
        x->pal_valid = true;
    }
    qdd_d3d7_flush(dd);
    QddSaved saved;
    qdd_save(d, &saved);
    d->SetRenderTarget(0, b->gpu);
    d->SetDepthStencilSurface(NULL);
    qdd_pass_state(d, 0xf, false);
    d->SetTexture(0, x->idx);
    d->SetTexture(1, x->pal);
    d->SetPixelShader(ps);
    qdd_quad(d, w, h);
    qdd_restore(d, &saved);
    return true;
}
