/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: render targets, scenes, clears, every DrawPrimitive variant,
 * the colour-key emulation's derived state, ComputeSphereVisibility,
 * ValidateDevice, EnumTextureFormats.
 *
 * Draw path: make_active() applies the device's state if another device
 * drew last; bind_targets() binds the render target and its attached
 * z-buffer (what backs them may change at any DirectDraw call, so this is
 * resolved per draw through qdd_surface_d3d9); bind_textures() uploads and
 * binds stage textures; apply_derived() sets the alpha test and stage-0
 * alpha arguments colour keying needs; the D3D9 scene opens lazily
 * (D3D7 draws outside BeginScene, D3D9 does not: the core ends the scene
 * before presenting, qdd_d3d7_flush).
 */
#include "qdd_d3d7.h"

#define Q7_ALIVE()                      \
    do {                                \
        if (!d3d)                       \
            return DDERR_INVALIDOBJECT; \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Targets, textures, derived state, scene                                   */

bool Q7Device::bind_targets(bool need_draw)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9 || !rt || !rt_valid)
        return false;
    IDirect3DSurface9 *rt9 = qdd_surface_d3d9(rt, true);
    if (!rt9)
        return false;
    if (rt9 != d3d->bound_rt9) {
        if (FAILED(d9->SetRenderTarget(0, rt9)))
            return false;
        d3d->bound_rt9 = rt9;
        apply_viewport(); /* D3D9 resets it with the target */
    }
    QddSurface *z = qdd_surface_attached(rt, DDSCAPS_ZBUFFER, 0);
    IDirect3DSurface9 *ds9 = NULL;
    if (z) {
        bool writes = !need_draw || st.rs[d7::D3DRENDERSTATE_ZENABLE] || st.rs[d7::D3DRENDERSTATE_STENCILENABLE];
        ds9 = qdd_surface_d3d9(z, writes);
    }
    if (ds9 != d3d->bound_ds9) {
        d9->SetDepthStencilSurface(ds9);
        d3d->bound_ds9 = ds9;
    }
    return true;
}

/* Stage textures: keyed variants while colour keying is on.  Stages after
 * the first disabled one are not looked at (D3D ignores them). */
void Q7Device::bind_textures()
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    bool ck = st.rs[d7::D3DRENDERSTATE_COLORKEYENABLE] != 0;
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage) {
        if (tss_eff(stage, d7::D3DTSS_COLOROP) == d7::D3DTOP_DISABLE)
            break;
        QddSurface *s = st.tex[stage];
        IDirect3DBaseTexture9 *t9 = NULL;
        /* Legacy hardware devices keep but do not sample a surface without
         * DDSCAPS_TEXTURE (Wine's test_texture_wrong_caps). */
        if (s && version < 7 && hal && !(qdd_surface_desc(s)->ddsCaps.dwCaps & DDSCAPS_TEXTURE))
            s = NULL;
        if (s && s != rt) {
            bool src_alpha = false;
            t9 = q7_tex_d3d9(d3d, s, ck, &src_alpha);
        }
        if (t9 != d3d->bound_tex9[stage]) {
            d9->SetTexture(stage, t9);
            d3d->bound_tex9[stage] = t9;
        }
    }
}

/*
 * Colour keying (native drivers kill keyed texels): with
 * D3DRENDERSTATE_COLORKEYENABLE on and a source-keyed texture in stage 0,
 * the texture is uploaded with keyed texels at alpha 0 and an alpha test
 * discards them (keyed fragments write no depth, Wine's test_ck_rgba) unless
 * COLORKEYBLENDENABLE asks for blending instead; when the source format has
 * no alpha, stage 0 must pass the texture's alpha on (the arguments games
 * use often select diffuse alpha), as Wine's ddraw does.
 */
void Q7Device::apply_derived()
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    Q7Derived want;
    want.alphatest = st.rs[d7::D3DRENDERSTATE_ALPHATESTENABLE] ? TRUE : FALSE;
    want.alphafunc = st.rs[d7::D3DRENDERSTATE_ALPHAFUNC];
    want.alpharef = st.rs[d7::D3DRENDERSTATE_ALPHAREF] & 0xff; /* native compares the low byte */
    want.aop0 = tss_eff(0, d7::D3DTSS_ALPHAOP);
    want.aarg1_0 = tss_eff(0, d7::D3DTSS_ALPHAARG1);
    want.aarg2_0 = tss_eff(0, d7::D3DTSS_ALPHAARG2);
    want.valid = true;

    DWORD key;
    QddSurface *t0 = st.tex[0];
    /* Legacy D3DTBLEND_MODULATE: alpha from the texture when it has alpha,
     * else from the diffuse colour (and without a texture). */
    if (version < 7 && legacy_blend && tmblend == d7::D3DTBLEND_MODULATE && ((tss0_set >> d7::D3DTSS_ALPHAOP) & 1u)) {
        Q7Tex *lt = t0 && t0 != rt ? q7_tex(t0) : NULL;
        want.aop0 = lt && lt->fmt.has_alpha ? d7::D3DTOP_SELECTARG1 : d7::D3DTOP_SELECTARG2;
    }
    if (st.rs[d7::D3DRENDERSTATE_COLORKEYENABLE] && t0 && t0 != rt && q7_surface_ckey(t0, &key) &&
        tss_eff(0, d7::D3DTSS_COLOROP) != d7::D3DTOP_DISABLE) {
        bool own_test = !st.rs[d7::D3DRENDERSTATE_COLORKEYBLENDENABLE] && !want.alphatest;
        if (own_test) {
            want.alphatest = TRUE;
            want.alphafunc = D3DCMP_NOTEQUAL;
            want.alpharef = 0;
        }
        Q7Tex *t = q7_tex(t0);
        if (t && !t->fmt.has_alpha) {
            DWORD op = want.aop0, a1 = want.aarg1_0, a2 = want.aarg2_0;
            bool blend = st.rs[d7::D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
            if (own_test && !blend) {
                /* The alpha only feeds the key test: the key alone, whatever
                 * the stage would make of it (native keys by texel; Wine's
                 * ddraw1 test_colorkey_precision modulates by alpha 0). */
                a1 = D3DTA_TEXTURE;
                op = d7::D3DTOP_SELECTARG1;
            } else if (op == d7::D3DTOP_DISABLE) {
                a1 = D3DTA_TEXTURE;
                op = d7::D3DTOP_SELECTARG1;
            } else if (op == d7::D3DTOP_SELECTARG1 && a1 != D3DTA_TEXTURE) {
                if (blend) {
                    a2 = D3DTA_TEXTURE;
                    op = d7::D3DTOP_MODULATE;
                } else {
                    a1 = D3DTA_TEXTURE;
                }
            } else if (op == d7::D3DTOP_SELECTARG2 && a2 != D3DTA_TEXTURE) {
                if (blend) {
                    a1 = D3DTA_TEXTURE;
                    op = d7::D3DTOP_MODULATE;
                } else {
                    a2 = D3DTA_TEXTURE;
                }
            }
            want.aop0 = op;
            want.aarg1_0 = a1;
            want.aarg2_0 = a2;
        }
    }

    bool all = !derived.valid;
    if (all || want.alphatest != derived.alphatest)
        d9->SetRenderState(D3DRS_ALPHATESTENABLE, want.alphatest);
    if (all || want.alphafunc != derived.alphafunc)
        d9->SetRenderState(D3DRS_ALPHAFUNC, want.alphafunc);
    if (all || want.alpharef != derived.alpharef)
        d9->SetRenderState(D3DRS_ALPHAREF, want.alpharef);
    if (all || want.aop0 != derived.aop0)
        d9->SetTextureStageState(0, D3DTSS_ALPHAOP, want.aop0);
    if (all || want.aarg1_0 != derived.aarg1_0)
        d9->SetTextureStageState(0, D3DTSS_ALPHAARG1, want.aarg1_0);
    if (all || want.aarg2_0 != derived.aarg2_0)
        d9->SetTextureStageState(0, D3DTSS_ALPHAARG2, want.aarg2_0);
    derived = want;
}

bool Q7Device::begin_scene9()
{
    if (d3d->in_scene9)
        return true;
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9 || FAILED(d9->BeginScene()))
        return false;
    d3d->in_scene9 = true;
    return true;
}

/* Everything a draw needs.  D3D_OK with *skip set when there is nothing to
 * draw into (a failed SetRenderTarget left a non-target selected). */
HRESULT Q7Device::prepare_draw()
{
    if (rt && qdd_surface_lost(rt))
        return DDERR_SURFACELOST;
    make_active();
    if (!bind_targets(true))
        return S_FALSE;
    bind_textures();
    apply_derived();
    if (!begin_scene9())
        return S_FALSE;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Render target                                                             */

HRESULT STDMETHODCALLTYPE Q7Device::SetRenderTarget(IDirectDrawSurface7 *surface, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE();
    QddSurface *s = surface ? qdd_surface_lookup(surface) : NULL;
    if (!s || qdd_surface_draw(s) != dd)
        return DDERR_INVALIDPARAMS;
    const DDSURFACEDESC2 *desc = qdd_surface_desc(s);
    PALETTEENTRY pal[256];
    if ((desc->ddpfPixelFormat.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 |
                                          DDPF_PALETTEINDEXED8)) &&
        !qdd_surface_palette(s, pal))
        return DDERR_INVALIDCAPS;
    if (!(desc->ddsCaps.dwCaps & DDSCAPS_3DDEVICE))
        return DDERR_INVALIDCAPS;
    if (hal && !(desc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY))
        return DDERR_INVALIDPARAMS;
    if (s == rt && rt_valid)
        return D3D_OK;
    /* Native: a z-buffer is refused but still becomes the stored target
     * (references move), and drawing then has no target. */
    bool zbuf = (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) != 0;
    if (!zbuf && !qdd_surface_d3d9(s, false))
        return DDERR_INVALIDPIXELFORMAT;
    IUnknown *nref = qdd_surface_iface7(s);
    nref->AddRef();
    IUnknown *old = rt_ref;
    rt = s;
    rt_ref = nref;
    rt_valid = !zbuf;
    if (old)
        old->Release();
    if (!rt_valid)
        return DDERR_INVALIDPIXELFORMAT;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::GetRenderTarget(IDirectDrawSurface7 **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (!rt) {
        *out = NULL;
        return DDERR_GENERIC;
    }
    IDirectDrawSurface7 *s = qdd_surface_iface7(rt);
    s->AddRef();
    *out = s;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Clear                                                                     */

/* Native (Wine's test_clear): rectangles are render-target absolute and
 * clipped to the viewport; NULL rectangles clear the viewport, (0, rects)
 * clears nothing, reversed rectangles succeed without clearing. */
HRESULT STDMETHODCALLTYPE Q7Device::Clear(DWORD count, d7::D3DRECT *rects, DWORD flags, D3DCOLOR color,
                                          d7::D3DVALUE z, DWORD stencil)
{
    Q7Lock lock;
    Q7_ALIVE();
    /* Native order (measured): no flags fails first, then (0, rects) is a
     * no-op and (n, NULL) clears the viewport. */
    if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
        return DDERR_INVALIDPARAMS;
    flags &= D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
    if (rects && !count)
        return D3D_OK;
    if (!rects)
        count = 0;
    if (rt && qdd_surface_lost(rt))
        return DDERR_SURFACELOST;
    if (!rt || !rt_valid)
        return D3D_OK;
    if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        QddSurface *zs = qdd_surface_attached(rt, DDSCAPS_ZBUFFER, 0);
        if (!zs)
            return D3DERR_ZBUFFER_NOTPRESENT;
        if ((flags & D3DCLEAR_STENCIL) && !(qdd_surface_desc(zs)->ddpfPixelFormat.dwFlags & DDPF_STENCILBUFFER))
            return D3DERR_STENCILBUFFER_NOTPRESENT;
    }

    D3DRECT local[16];
    D3DRECT *list = NULL;
    DWORD n = 0;
    if (count) {
        list = count <= 16 ? local : (D3DRECT *)malloc(count * sizeof(*list));
        if (!list)
            return DDERR_OUTOFMEMORY;
        for (DWORD i = 0; i < count; ++i)
            if (rects[i].x1 < rects[i].x2 && rects[i].y1 < rects[i].y2) {
                list[n].x1 = rects[i].x1;
                list[n].y1 = rects[i].y1;
                list[n].x2 = rects[i].x2;
                list[n].y2 = rects[i].y2;
                ++n;
            }
        if (!n) {
            if (list != local)
                free(list);
            return D3D_OK;
        }
    }

    make_active();
    if (bind_targets(false)) {
        IDirect3DDevice9 *d9 = d3d->device9();
        d9->Clear(n, n ? list : NULL, flags, color, z, stencil);
    }
    if (list && list != local)
        free(list);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Draws                                                                     */

static bool q7_prim_type_valid(DWORD type)
{
    return type >= d7::D3DPT_POINTLIST && type <= d7::D3DPT_TRIANGLEFAN;
}

/* Smallest and largest index; false when there are none. */
static void q7_index_range(const WORD *idx, DWORD n, UINT *lo, UINT *hi)
{
    UINT a = 0xffff, b = 0;
    for (DWORD i = 0; i < n; ++i) {
        UINT v = idx[i];
        if (v < a)
            a = v;
        if (v > b)
            b = v;
    }
    *lo = a;
    *hi = b;
}

static void q7_log_once(bool *done, const char *what)
{
    if (*done)
        return;
    *done = true;
    qdd_log("qdd_d3d7: %s (logged once)", what);
}

HRESULT Q7Device::draw_up(DWORD type, DWORD fvf, const void *vertices, DWORD count)
{
    /* Native draws nothing and succeeds for an invalid FVF or primitive
     * type (measured). */
    UINT stride = q7_fvf_size(fvf);
    if (!stride || !q7_prim_type_valid(type))
        return D3D_OK;
    UINT prims = q7_prim_count(type, count);
    if (!prims)
        return D3D_OK;
    HRESULT hr = prepare_draw();
    if (hr != D3D_OK)
        return hr == S_FALSE ? D3D_OK : hr;
    IDirect3DDevice9 *d9 = d3d->device9();
    IDirect3DVertexDeclaration9 *decl = d3d->decl_for(fvf);
    if (!decl)
        return D3D_OK;
    if (decl != d3d->bound_decl9) {
        d9->SetVertexDeclaration(decl);
        d3d->bound_decl9 = decl;
    }
    d9->DrawPrimitiveUP((D3DPRIMITIVETYPE)type, prims, vertices, stride);
    return D3D_OK;
}

HRESULT Q7Device::draw_indexed_up(DWORD type, DWORD fvf, const void *vertices, DWORD vcount, const WORD *indices,
                                  DWORD icount)
{
    static bool warned;
    UINT stride = q7_fvf_size(fvf);
    if (!stride || !q7_prim_type_valid(type))
        return D3D_OK;
    UINT prims = q7_prim_count(type, icount);
    if (!prims)
        return D3D_OK;
    UINT lo, hi;
    q7_index_range(indices, icount, &lo, &hi);
    if (hi >= vcount) {
        /* The application's index points past its vertices: native reads
         * garbage, the host would refuse the copy. */
        q7_log_once(&warned, "DrawIndexedPrimitive index past the vertex count, draw skipped");
        return D3D_OK;
    }
    HRESULT hr = prepare_draw();
    if (hr != D3D_OK)
        return hr == S_FALSE ? D3D_OK : hr;
    IDirect3DDevice9 *d9 = d3d->device9();
    IDirect3DVertexDeclaration9 *decl = d3d->decl_for(fvf);
    if (!decl)
        return D3D_OK;
    if (decl != d3d->bound_decl9) {
        d9->SetVertexDeclaration(decl);
        d3d->bound_decl9 = decl;
    }
    d9->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)type, lo, hi - lo + 1, prims, indices, D3DFMT_INDEX16, vertices,
                               stride);
    d3d->ib_bound = false; /* UP draws unbind the indices */
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawPrimitive(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices, DWORD count,
                                                  DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE();
    if (!count)
        return D3D_OK;
    if (!vertices)
        return DDERR_INVALIDPARAMS;
    return draw_up(type, fvf, vertices, count);
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawIndexedPrimitive(d7::D3DPRIMITIVETYPE type, DWORD fvf, void *vertices,
                                                         DWORD vcount, WORD *indices, DWORD icount, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE();
    if (!vcount || !icount)
        return D3D_OK;
    if (!vertices || !indices)
        return DDERR_INVALIDPARAMS;
    return draw_indexed_up(type, fvf, vertices, vcount, indices, icount);
}

/* Interleaves count strided vertices starting at first into the scratch
 * buffer in FVF order; NULL on failure. */
BYTE *Q7Device::assemble_strided(DWORD fvf, const d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD first, DWORD count,
                                 UINT *stride_out)
{
    Q7FvfLayout l;
    if (!q7_fvf_layout(fvf, &l))
        return NULL;
    UINT stride = l.size;
    if (count > 0xffffffffu / stride)
        return NULL;
    UINT need = count * stride;
    if (need > scratch_size) {
        BYTE *n = (BYTE *)realloc(scratch, need);
        if (!n)
            return NULL;
        scratch = n;
        scratch_size = need;
    }
    struct Part {
        const d7::D3DDP_PTRSTRIDE *src;
        UINT off, size;
    } parts[12];
    UINT np = 0;
    parts[np].src = &data->position;
    parts[np].off = 0;
    parts[np].size = (l.posfloats + (UINT)l.nbeta) * 4u;
    ++np;
    if (l.normal >= 0) {
        parts[np].src = &data->normal;
        parts[np].off = (UINT)l.normal;
        parts[np].size = 12;
        ++np;
    }
    if (l.diffuse >= 0) {
        parts[np].src = &data->diffuse;
        parts[np].off = (UINT)l.diffuse;
        parts[np].size = 4;
        ++np;
    }
    if (l.specular >= 0) {
        parts[np].src = &data->specular;
        parts[np].off = (UINT)l.specular;
        parts[np].size = 4;
        ++np;
    }
    for (UINT t = 0; t < l.ntex; ++t) {
        parts[np].src = &data->textureCoords[t];
        parts[np].off = (UINT)l.tex[t];
        parts[np].size = l.texdim[t] * 4u;
        ++np;
    }
    for (UINT p = 0; p < np; ++p)
        if (!parts[p].src->lpvData)
            return NULL;
    if (l.reserved >= 0)
        for (DWORD v = 0; v < count; ++v)
            memset(scratch + v * stride + l.reserved, 0, 4);
    for (UINT p = 0; p < np; ++p) {
        const BYTE *src = (const BYTE *)parts[p].src->lpvData + (size_t)first * parts[p].src->dwStride;
        UINT sstride = parts[p].src->dwStride;
        BYTE *dst = scratch + parts[p].off;
        for (DWORD v = 0; v < count; ++v) {
            memcpy(dst, src, parts[p].size);
            dst += stride;
            src += sstride;
        }
    }
    *stride_out = stride;
    return scratch;
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawPrimitiveStrided(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                                         d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count,
                                                         DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE();
    if (!count)
        return D3D_OK;
    if (!data)
        return DDERR_INVALIDPARAMS;
    if (!q7_prim_type_valid(type) || !q7_fvf_size(fvf))
        return D3D_OK;
    if (!q7_prim_count(type, count))
        return D3D_OK;
    UINT stride;
    BYTE *v = assemble_strided(fvf, data, 0, count, &stride);
    if (!v)
        return DDERR_INVALIDPARAMS;
    return draw_up(type, fvf, v, count);
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawIndexedPrimitiveStrided(d7::D3DPRIMITIVETYPE type, DWORD fvf,
                                                                d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD vcount,
                                                                WORD *indices, DWORD icount, DWORD flags)
{
    static bool warned;
    Q7Lock lock;
    Q7_ALIVE();
    if (!vcount || !icount)
        return D3D_OK;
    if (!data || !indices)
        return DDERR_INVALIDPARAMS;
    if (!q7_prim_type_valid(type) || !q7_fvf_size(fvf))
        return D3D_OK;
    if (!q7_prim_count(type, icount))
        return D3D_OK;
    UINT lo, hi;
    q7_index_range(indices, icount, &lo, &hi);
    if (hi >= vcount) {
        q7_log_once(&warned, "DrawIndexedPrimitiveStrided index past the vertex count, draw skipped");
        return D3D_OK;
    }
    /* Only the referenced vertices, rebased to 0. */
    WORD local[256];
    WORD *rebased = icount <= 256 ? local : (WORD *)malloc(icount * sizeof(WORD));
    if (!rebased)
        return DDERR_OUTOFMEMORY;
    for (DWORD i = 0; i < icount; ++i)
        rebased[i] = (WORD)(indices[i] - lo);
    UINT stride;
    BYTE *v = assemble_strided(fvf, data, lo, hi - lo + 1, &stride);
    HRESULT hr = v ? draw_indexed_up(type, fvf, v, hi - lo + 1, rebased, icount) : DDERR_INVALIDPARAMS;
    if (rebased != local)
        free(rebased);
    return hr;
}

/* The streaming index buffer, (re)created to hold count indices. */
static bool q7_ensure_ib(Q7D3D *d3d, IDirect3DDevice9 *d9, DWORD count)
{
    if (d3d->ib9 && count <= d3d->ib_size)
        return true;
    UINT size = 65536;
    while (size < count)
        size *= 2;
    IDirect3DIndexBuffer9 *ib = NULL;
    if (FAILED(d9->CreateIndexBuffer(size * sizeof(WORD), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                     D3DPOOL_DEFAULT, &ib, NULL)) ||
        !ib)
        return false;
    if (d3d->ib9)
        d3d->ib9->Release();
    d3d->ib9 = ib;
    d3d->ib_size = size;
    d3d->ib_pos = size; /* forces a discard on the next upload */
    d3d->ib_bound = false;
    return true;
}

HRESULT Q7Device::upload_indices(const WORD *indices, DWORD count, UINT *start)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return DDERR_GENERIC;
    if (!q7_ensure_ib(d3d, d9, count))
        return DDERR_OUTOFMEMORY;
    DWORD lock_flags = D3DLOCK_NOOVERWRITE;
    if (d3d->ib_pos + count > d3d->ib_size) {
        d3d->ib_pos = 0;
        lock_flags = D3DLOCK_DISCARD;
    }
    void *p = NULL;
    if (FAILED(d3d->ib9->Lock(d3d->ib_pos * sizeof(WORD), count * sizeof(WORD), &p, lock_flags)) || !p)
        return DDERR_GENERIC;
    memcpy(p, indices, count * sizeof(WORD));
    d3d->ib9->Unlock();
    *start = d3d->ib_pos;
    d3d->ib_pos += count;
    if (!d3d->ib_bound) {
        d9->SetIndices(d3d->ib9);
        d3d->ib_bound = true;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawPrimitiveVB(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer7 *vb,
                                                    DWORD start, DWORD count, DWORD flags)
{
    Q7Lock lock;
    Q7_ALIVE();
    Q7VB *v = q7_vb_lookup(vb);
    if (!v || !v->vb9 || v->dd != dd)
        return DDERR_INVALIDPARAMS;
    if (!count)
        return D3D_OK;
    /* Native draws from a locked buffer too (measured). */
    if (!q7_prim_type_valid(type) || !v->stride)
        return D3D_OK;
    if (start > v->count || count > v->count - start)
        return DDERR_INVALIDPARAMS;
    UINT prims = q7_prim_count(type, count);
    if (!prims)
        return D3D_OK;
    HRESULT hr = prepare_draw();
    if (hr != D3D_OK)
        return hr == S_FALSE ? D3D_OK : hr;
    IDirect3DDevice9 *d9 = d3d->device9();
    IDirect3DVertexDeclaration9 *decl = d3d->decl_for(v->fvf);
    if (!decl)
        return DDERR_INVALIDPARAMS;
    if (decl != d3d->bound_decl9) {
        d9->SetVertexDeclaration(decl);
        d3d->bound_decl9 = decl;
    }
    d9->SetStreamSource(0, v->vb9, 0, v->stride);
    d9->DrawPrimitive((D3DPRIMITIVETYPE)type, start, prims);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::DrawIndexedPrimitiveVB(d7::D3DPRIMITIVETYPE type, d7::IDirect3DVertexBuffer7 *vb,
                                                           DWORD start, DWORD vcount, WORD *indices, DWORD icount,
                                                           DWORD flags)
{
    static bool warned;
    Q7Lock lock;
    Q7_ALIVE();
    Q7VB *v = q7_vb_lookup(vb);
    if (!v || !v->vb9 || v->dd != dd)
        return DDERR_INVALIDPARAMS;
    if (!vcount || !icount) {
        /* Native (Wine's test_draw_primitive): with no indices the first
         * indexed buffer draw fails, creating the device's index stream;
         * later ones succeed. */
        IDirect3DDevice9 *d9 = d3d->device9();
        if (!icount && !d3d->ib9 && d9) {
            q7_ensure_ib(d3d, d9, 0);
            return E_FAIL;
        }
        return D3D_OK;
    }
    if (!indices)
        return DDERR_INVALIDPARAMS;
    if (!q7_prim_type_valid(type) || !v->stride)
        return D3D_OK;
    UINT prims = q7_prim_count(type, icount);
    if (!prims)
        return D3D_OK;
    UINT lo, hi;
    q7_index_range(indices, icount, &lo, &hi);
    if (start > v->count || hi >= v->count - start) {
        q7_log_once(&warned, "DrawIndexedPrimitiveVB index past the buffer, draw skipped");
        return D3D_OK;
    }
    HRESULT hr = prepare_draw();
    if (hr != D3D_OK)
        return hr == S_FALSE ? D3D_OK : hr;
    IDirect3DDevice9 *d9 = d3d->device9();
    IDirect3DVertexDeclaration9 *decl = d3d->decl_for(v->fvf);
    if (!decl)
        return DDERR_INVALIDPARAMS;
    UINT first;
    hr = upload_indices(indices, icount, &first);
    if (FAILED(hr))
        return hr;
    if (decl != d3d->bound_decl9) {
        d9->SetVertexDeclaration(decl);
        d3d->bound_decl9 = decl;
    }
    d9->SetStreamSource(0, v->vb9, 0, v->stride);
    d9->DrawIndexedPrimitive((D3DPRIMITIVETYPE)type, (INT)start, lo, hi - lo + 1, first, prims);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Misc                                                                      */

HRESULT STDMETHODCALLTYPE Q7Device::ComputeSphereVisibility(D3DVECTOR *centers, d7::D3DVALUE *radii, DWORD count,
                                                            DWORD flags, DWORD *ret)
{
    if (!centers || !radii || !ret)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    q7_sphere_visibility(&st, centers, radii, count, ret);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::ValidateDevice(LPDWORD passes)
{
    if (!passes)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return DDERR_GENERIC;
    make_active();
    bind_textures();
    apply_derived();
    DWORD p = 1;
    HRESULT hr = d9->ValidateDevice(&p);
    *passes = SUCCEEDED(hr) ? p : 0;
    return hr;
}

/* What the stock runtime lists per interface version (measured on the
 * Windows 11 host): IDirect3DDevice3 leaves out alpha-only and luminance
 * bump formats, IDirect3DDevice2 and IDirect3DDevice list plain RGB
 * formats only. */
static bool q7_texfmt_listed(UINT version, const DDPIXELFORMAT *pf)
{
    if (version >= 7)
        return true;
    if (version == 3)
        return !(pf->dwFlags & (DDPF_ALPHA | DDPF_BUMPLUMINANCE));
    return (pf->dwFlags & DDPF_RGB) && !(pf->dwFlags & (DDPF_FOURCC | DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE));
}

HRESULT STDMETHODCALLTYPE Q7Device::EnumTextureFormats(d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    return texture_formats(version, cb, ctx);
}

HRESULT Q7Device::texture_formats(UINT v, d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    if (!cb)
        return DDERR_INVALIDPARAMS;
    /* The stock runtime's order (measured), each as the core creates video
     * memory textures of it (qdd_draw_texture_format: what the device
     * samples natively; P8 and other converted formats are left out, like
     * stock, though system-memory textures of them still draw). */
    static const D3DFORMAT list[] = {
        D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8,
        D3DFMT_P8,       D3DFMT_V8U8,     D3DFMT_X8L8V8U8, D3DFMT_L6V5U5, D3DFMT_DXT1,     D3DFMT_DXT2,
        D3DFMT_DXT3,     D3DFMT_DXT4,     D3DFMT_DXT5,     D3DFMT_L8,     D3DFMT_A8,       D3DFMT_A8L8,
        D3DFMT_A4L4,
    };
    DDPIXELFORMAT pf[sizeof(list) / sizeof(list[0])];
    UINT n = 0;
    {
        Q7Lock lock;
        Q7_ALIVE();
        for (UINT i = 0; i < sizeof(list) / sizeof(list[0]); ++i)
            if (q7_ddpf_from_d3d9(list[i], &pf[n]) && qdd_draw_texture_format(dd, &pf[n]) &&
                q7_texfmt_listed(v, &pf[n]))
                ++n;
    }
    for (UINT i = 0; i < n; ++i)
        if (cb(&pf[i], ctx) != DDENUMRET_OK)
            break;
    return D3D_OK;
}
