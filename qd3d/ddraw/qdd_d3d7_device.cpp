/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: IDirect3DDevice7 - lifetime, state (Get/Set, application to
 * the D3D9 device), state blocks, lights, transforms, viewport, misc.
 * Drawing, clears and render targets are in qdd_d3d7_draw.cpp.
 *
 * Every entry point takes the process lock (qdd_lock).  Set* calls change
 * the device's Q7State and, while the device's state is the one applied to
 * the shared D3D9 device (Q7D3D::active), the D3D9 state too; a device that
 * becomes active applies everything (apply_all).  Between BeginStateBlock
 * and EndStateBlock, Set* calls only record (native: the recorded values do
 * not reach the device).
 */
#include "qdd_d3d7.h"

#define Q7_ALIVE()                   \
    do {                             \
        if (!d3d)                    \
            return DDERR_INVALIDOBJECT; \
    } while (0)

static inline void q7_mask_set(DWORD *mask, DWORD bit)
{
    mask[bit >> 5] |= 1u << (bit & 31);
}

static inline bool q7_mask_has(const DWORD *mask, DWORD bit)
{
    return (mask[bit >> 5] >> (bit & 31)) & 1u;
}

Q7Device *q7_device_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_DEVICE) ? static_cast<Q7Device *>((d7::IDirect3DDevice7 *)p) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                  */

Q7Device::Q7Device(Q7D3D *d, const GUID *type, QddSurface *target, UINT ver)
    : refs(1), d3d(d), dd(d->dd), next(NULL), guid(*type), hal(false), rt(target), rt_ref(NULL), rt_valid(true), st(),
      in_scene(false), recording(NULL), sbs(NULL), nsbs(0), derived(), scratch(NULL), scratch_size(0), clipstatus(),
      pv_power(0.0f), version(ver), outer(NULL), viewports(NULL), cur_vp(NULL), material(0), tmblend(0),
      legacy_blend(false), tex0_handle(false), tss0_set(0), tss0_val(), legacy_proj(), legacy_clip(), bv_type(0),
      bv_fvf(0), bv_flags(0),
      bv_buf(NULL), bv_count(0), bv_cap(0), bv_stride(0), bv_indexed(false), bv_verts(NULL), bv_vcount(0),
      bv_idx(NULL), bv_icount(0), bv_icap(0), mx_world(0), mx_view(0), mx_proj(0), lrs(), lit_buf(NULL), lit_cap(0),
      ls(), if3(this), if2(this), if1(this)
{
    hal = IsEqualGUID(*type, Q7IID_IDirect3DHALDevice) || IsEqualGUID(*type, Q7IID_IDirect3DTnLHalDevice);
    q7_state_defaults(&st, qdd_surface_attached(target, DDSCAPS_ZBUFFER, 0) != NULL);
    const DDSURFACEDESC2 *desc = qdd_surface_desc(target);
    st.vp.dwWidth = desc->dwWidth;
    st.vp.dwHeight = desc->dwHeight;
    /* Native initial clip status (measured). */
    clipstatus.dwFlags = D3DCLIPSTATUS_EXTENTS2;
    clipstatus.dwStatus = 0x01fff000u; /* D3DSTATUS_CLIPINTERSECTIONALL */
    clipstatus.minx = clipstatus.miny = 2048.0f;
}

Q7Device::~Q7Device()
{
    for (UINT i = 0; i < nsbs; ++i)
        q7_sb_free(sbs[i]);
    free(sbs);
    q7_sb_free(recording);
    q7_state_free(&st);
    free(scratch);
}

/* The DirectDraw object went away under the device: everything it pointed
 * at is gone or going, so drop the pointers without releasing anything. */
void Q7Device::detach()
{
    d3d = NULL;
    dd = NULL;
    rt = NULL;
    rt_ref = NULL;
    for (UINT i = 0; i < Q7_STAGES; ++i)
        st.tex[i] = NULL;
    for (UINT i = 0; i < nsbs; ++i)
        if (sbs[i])
            for (UINT s = 0; s < Q7_STAGES; ++s)
                sbs[i]->st.tex[s] = NULL;
    if (recording)
        for (UINT s = 0; s < Q7_STAGES; ++s)
            recording->st.tex[s] = NULL;
}

/* A surface is being freed: state blocks keep no reference, so they forget
 * it (the device's own bindings hold references and cannot be freed). */
void Q7Device::forget_surface(QddSurface *s)
{
    for (UINT i = 0; i < nsbs; ++i)
        if (sbs[i])
            for (UINT t = 0; t < Q7_STAGES; ++t)
                if (sbs[i]->st.tex[t] == s)
                    sbs[i]->st.tex[t] = NULL;
    if (recording)
        for (UINT t = 0; t < Q7_STAGES; ++t)
            if (recording->st.tex[t] == s)
                recording->st.tex[t] = NULL;
    for (UINT t = 0; t < Q7_STAGES; ++t)
        if (st.tex[t] == s)
            st.tex[t] = NULL;
    if (rt == s) {
        rt = NULL;
        rt_ref = NULL;
        rt_valid = false;
    }
}

HRESULT STDMETHODCALLTYPE Q7Device::QueryInterface(REFIID riid, void **obj)
{
    return inner_query(riid, obj);
}

ULONG STDMETHODCALLTYPE Q7Device::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q7Device::Release()
{
    IUnknown *drop[Q7_STAGES + 1];
    UINT ndrop = 0;
    {
        Q7Lock lock;
        LONG r = InterlockedDecrement(&refs);
        if (r > 0)
            return (ULONG)r;
        if (r < 0) {
            InterlockedIncrement(&refs);
            return 0;
        }
        q7_registry_remove(static_cast<d7::IDirect3DDevice7 *>(this));
        legacy_teardown();
        if (d3d) {
            for (Q7Device **p = &d3d->devices; *p; p = &(*p)->next)
                if (*p == this) {
                    *p = next;
                    break;
                }
            if (d3d->active == this)
                d3d->active = NULL;
            for (UINT i = 0; i < Q7_STAGES; ++i)
                if (st.tex[i])
                    drop[ndrop++] = qdd_surface_iface7(st.tex[i]);
            if (rt_ref)
                drop[ndrop++] = rt_ref;
            qdd_draw_3d_devices(dd, -1);
        }
        delete this;
        /* Releasing the render target may release the DirectDraw object. */
        for (UINT i = 0; i < ndrop; ++i)
            drop[i]->Release();
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Applying state to the D3D9 device                                         */

static DWORD q7_magfilter(DWORD v)
{
    switch (v) {
    case d7::D3DTFG_LINEAR:
    case d7::D3DTFG_FLATCUBIC:
    case d7::D3DTFG_GAUSSIANCUBIC: return D3DTEXF_LINEAR;
    case d7::D3DTFG_ANISOTROPIC: return D3DTEXF_ANISOTROPIC;
    default: return D3DTEXF_POINT;
    }
}

static DWORD q7_minfilter(DWORD v)
{
    switch (v) {
    case d7::D3DTFN_LINEAR: return D3DTEXF_LINEAR;
    case d7::D3DTFN_ANISOTROPIC: return D3DTEXF_ANISOTROPIC;
    default: return D3DTEXF_POINT;
    }
}

static DWORD q7_mipfilter(DWORD v)
{
    switch (v) {
    case d7::D3DTFP_POINT: return D3DTEXF_POINT;
    case 0: /* unchecked by native */
    case d7::D3DTFP_LINEAR: return D3DTEXF_LINEAR;
    default: return D3DTEXF_NONE;
    }
}

void Q7Device::apply_rs(DWORD s, DWORD v)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    switch (s) {
    case d7::D3DRENDERSTATE_ALPHATESTENABLE:
    case d7::D3DRENDERSTATE_ALPHAREF:
    case d7::D3DRENDERSTATE_ALPHAFUNC:
    case d7::D3DRENDERSTATE_COLORKEYENABLE:
    case d7::D3DRENDERSTATE_COLORKEYBLENDENABLE:
        derived.valid = false;
        return;
    case d7::D3DRENDERSTATE_ALPHABLENDENABLE:
        d9->SetRenderState(D3DRS_ALPHABLENDENABLE, v);
        derived.valid = false;
        return;
    case d7::D3DRENDERSTATE_SHADEMODE:
        /* Phong is accepted and shades like Gouraud (native, measured by
         * Wine's test_shademode). */
        d9->SetRenderState(D3DRS_SHADEMODE, v == d7::D3DSHADE_PHONG ? (DWORD)D3DSHADE_GOURAUD : v);
        return;
    case d7::D3DRENDERSTATE_EDGEANTIALIAS:
        /* Never set reads 0xffffffff on the stock runtime; it means off. */
        d9->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, v && v != 0xffffffffu);
        return;
    case d7::D3DRENDERSTATE_ZBIAS:
        /* One ZBIAS step is one 16-bit depth unit (as in qd3d8). */
        d9->SetRenderState(D3DRS_DEPTHBIAS, q7_f2dw((float)v * (-1.0f / 65535.0f)));
        return;
    default:
        if (q7_rs_kind(s) == Q7RS_D3D9)
            d9->SetRenderState((D3DRENDERSTATETYPE)s, v);
        return;
    }
}

DWORD Q7Device::tss_eff(DWORD stage, DWORD type) const
{
    if (version < 7) {
        /* Legacy devices have no texture transforms (a DirectX 7 state they
         * store only), and stage 0 follows TEXTUREMAPBLEND for the states it
         * set last. */
        if (type == d7::D3DTSS_TEXTURETRANSFORMFLAGS)
            return 0;
        if (stage == 0 && type < 32 && ((tss0_set >> type) & 1u))
            return tss0_val[type];
    }
    return st.tss[stage][type];
}

void Q7Device::apply_tss(DWORD stage, DWORD type, DWORD v)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    if (version < 7)
        v = tss_eff(stage, type);
    switch (type) {
    case d7::D3DTSS_COLOROP:
    case d7::D3DTSS_COLORARG1:
    case d7::D3DTSS_COLORARG2:
    case d7::D3DTSS_BUMPENVMAT00:
    case d7::D3DTSS_BUMPENVMAT01:
    case d7::D3DTSS_BUMPENVMAT10:
    case d7::D3DTSS_BUMPENVMAT11:
    case d7::D3DTSS_TEXCOORDINDEX:
    case d7::D3DTSS_BUMPENVLSCALE:
    case d7::D3DTSS_BUMPENVLOFFSET:
    case d7::D3DTSS_TEXTURETRANSFORMFLAGS:
        d9->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, v);
        if (stage == 0 && type == d7::D3DTSS_COLOROP)
            derived.valid = false;
        return;
    case d7::D3DTSS_ALPHAOP:
    case d7::D3DTSS_ALPHAARG1:
    case d7::D3DTSS_ALPHAARG2:
        if (stage == 0) {
            derived.valid = false; /* the colour-key fixup owns stage 0 alpha */
            return;
        }
        d9->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, v);
        return;
    case d7::D3DTSS_ADDRESS:
        d9->SetSamplerState(stage, D3DSAMP_ADDRESSU, v);
        d9->SetSamplerState(stage, D3DSAMP_ADDRESSV, v);
        return;
    case d7::D3DTSS_ADDRESSU: d9->SetSamplerState(stage, D3DSAMP_ADDRESSU, v); return;
    case d7::D3DTSS_ADDRESSV: d9->SetSamplerState(stage, D3DSAMP_ADDRESSV, v); return;
    case d7::D3DTSS_BORDERCOLOR: d9->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, v); return;
    case d7::D3DTSS_MAGFILTER: d9->SetSamplerState(stage, D3DSAMP_MAGFILTER, q7_magfilter(v)); return;
    case d7::D3DTSS_MINFILTER: d9->SetSamplerState(stage, D3DSAMP_MINFILTER, q7_minfilter(v)); return;
    case d7::D3DTSS_MIPFILTER: d9->SetSamplerState(stage, D3DSAMP_MIPFILTER, q7_mipfilter(v)); return;
    case d7::D3DTSS_MIPMAPLODBIAS: d9->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, v); return;
    case d7::D3DTSS_MAXMIPLEVEL: d9->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, v); return;
    case d7::D3DTSS_MAXANISOTROPY: d9->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, v); return;
    default: return; /* 0 and 25-31: kept only */
    }
}

void Q7Device::apply_transform(unsigned slot)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (d9)
        d9->SetTransform(q7_xf_d3d9(slot), &st.xf[slot]);
}

/* The D3D7 viewport may be larger than a render target set later
 * (SetRenderTarget keeps it, measured) and use any Z range; D3D9 wants it
 * inside the target and Z in 0..1. */
void Q7Device::apply_viewport()
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9 || !rt)
        return;
    const DDSURFACEDESC2 *desc = qdd_surface_desc(rt);
    D3DVIEWPORT9 vp;
    DWORD w = desc->dwWidth, h = desc->dwHeight;
    vp.X = st.vp.dwX < w ? st.vp.dwX : w;
    vp.Y = st.vp.dwY < h ? st.vp.dwY : h;
    vp.Width = st.vp.dwWidth < w - vp.X ? st.vp.dwWidth : w - vp.X;
    vp.Height = st.vp.dwHeight < h - vp.Y ? st.vp.dwHeight : h - vp.Y;
    float mn = st.vp.dvMinZ, mx = st.vp.dvMaxZ;
    vp.MinZ = !(mn > 0.0f) ? 0.0f : mn > 1.0f ? 1.0f : mn;
    vp.MaxZ = !(mx > 0.0f) ? 0.0f : mx > 1.0f ? 1.0f : mx;
    d9->SetViewport(&vp);
}

/* Direct3D 1-6 attenuation is a0 + a1 x + a2 x^2 with x = (range - d) /
 * range, nothing at or past the range (Wine's legacy lighting); Direct3D 9
 * computes 1 / (A0 + A1 d + A2 d^2).  A constant (a1 = a2 = 0) maps exactly
 * (A0 = 1 / a0; a0 <= 0 lights nothing).  Otherwise the reciprocal is
 * fitted at the light and at half the range: an approximation the CPU paths
 * (ProcessVertices, execute buffers) do not need. */
static void q7_legacy_attenuation(D3DLIGHT9 *l)
{
    static const D3DCOLORVALUE black = {0.0f, 0.0f, 0.0f, 0.0f};
    float a0 = l->Attenuation0, a1 = l->Attenuation1, a2 = l->Attenuation2, r = l->Range;
    float f0 = a0 + a1 + a2, fh = a0 + 0.5f * a1 + 0.25f * a2;
    l->Attenuation1 = l->Attenuation2 = 0.0f;
    if (a1 == 0.0f && a2 == 0.0f) {
        if (a0 > 0.0f) {
            l->Attenuation0 = 1.0f / a0;
            return;
        }
    } else if (f0 > 0.0f && fh > 0.0f && r > 0.0f) {
        l->Attenuation0 = 1.0f / f0;
        float a = (1.0f / fh - l->Attenuation0) / (0.5f * r);
        l->Attenuation1 = a > 0.0f ? a : 0.0f;
        return;
    }
    l->Attenuation0 = 1.0f;
    l->Diffuse = l->Specular = l->Ambient = black;
}

/* Light l in D3D9 slot (its index in the device's table).  D3D9 hardware
 * vertex processing lights at most MaxActiveLights: the first ones enabled,
 * in creation order, are the ones on (native HAL devices light any number
 * in software; docs/design/ddraw-remoting.md). */
void Q7Device::apply_light(Q7Light *l)
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    DWORD slot = (DWORD)(l - st.lights);
    D3DLIGHT9 l9;
    memcpy(&l9, &l->light, sizeof(l9));
    if (version < 7 && (l9.Type == D3DLIGHT_POINT || l9.Type == D3DLIGHT_SPOT))
        q7_legacy_attenuation(&l9);
    d9->SetLight(slot, &l9);
    if (slot >= d3d->d9_lights)
        d3d->d9_lights = slot + 1;
}

void Q7Device::apply_lights()
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    DWORD max = d3d->get_caps() && d3d->caps9.MaxActiveLights ? d3d->caps9.MaxActiveLights : 8;
    DWORD on = 0;
    for (UINT i = 0; i < st.nlights; ++i) {
        apply_light(&st.lights[i]);
        bool enable = st.lights[i].enabled && on < max;
        if (enable)
            ++on;
        d9->LightEnable(i, enable);
    }
    /* Slots another device used. */
    for (UINT i = st.nlights; i < d3d->d9_lights; ++i)
        d9->LightEnable(i, FALSE);
    d3d->d9_lights = st.nlights;
}

void Q7Device::apply_all()
{
    IDirect3DDevice9 *d9 = d3d->device9();
    if (!d9)
        return;
    for (DWORD s = 0; s < Q7_RS; ++s) {
        Q7RSKind k = q7_rs_kind(s);
        if (k == Q7RS_D3D9 || k == Q7RS_SPECIAL)
            apply_rs(s, st.rs[s]);
    }
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage)
        for (DWORD t = 1; t <= d7::D3DTSS_TEXTURETRANSFORMFLAGS; ++t)
            if (t != d7::D3DTSS_ADDRESS) /* ADDRESSU/V carry it */
                apply_tss(stage, t, st.tss[stage][t]);
    for (unsigned i = 0; i < Q7_XF; ++i)
        apply_transform(i);
    D3DMATERIAL9 m;
    memcpy(&m, &st.mat, sizeof(m));
    d9->SetMaterial(&m);
    apply_lights();
    DWORD planes = d3d->get_caps() ? d3d->caps9.MaxUserClipPlanes : 6;
    for (DWORD i = 0; i < planes && i < Q7_CLIPPLANES; ++i)
        d9->SetClipPlane(i, st.clip[i]);
    derived.valid = false;
    apply_viewport();
}

void Q7Device::make_active()
{
    if (d3d->active == this)
        return;
    d3d->active = this;
    apply_all();
}

/* ------------------------------------------------------------------------ */
/* Setters (shared by the methods and ApplyStateBlock)                       */

HRESULT Q7Device::set_rs(DWORD s, DWORD v)
{
    Q7RSKind k = q7_rs_kind(s);
    if (k == Q7RS_INVALID)
        return DDERR_INVALIDPARAMS;
    if (recording) {
        recording->st.rs[s] = v;
        q7_mask_set(recording->rs_mask, s);
        return D3D_OK;
    }
    st.rs[s] = v;
    if (is_active() && k != Q7RS_STORE)
        apply_rs(s, v);
    return D3D_OK;
}

HRESULT Q7Device::set_tss(DWORD stage, DWORD type, DWORD v)
{
    /* Native: stages past the eighth and unknown types are accepted. */
    if (stage >= Q7_STAGES || type >= Q7_TSS)
        return D3D_OK;
    /* D3DTSS_ADDRESS sets both directions; it reads back as ADDRESSU. */
    DWORD mask = 1u << type;
    if (type == d7::D3DTSS_ADDRESS || type == d7::D3DTSS_ADDRESSU)
        mask |= (1u << d7::D3DTSS_ADDRESS) | (1u << d7::D3DTSS_ADDRESSU);
    if (type == d7::D3DTSS_ADDRESS)
        mask |= 1u << d7::D3DTSS_ADDRESSV;
    Q7State *dst = recording ? &recording->st : &st;
    for (DWORD t = 0; t < Q7_TSS; ++t)
        if (mask & (1u << t))
            dst->tss[stage][t] = v;
    if (recording) {
        recording->tss_mask[stage] |= mask;
        return D3D_OK;
    }
    if (is_active())
        apply_tss(stage, type, v);
    return D3D_OK;
}

HRESULT Q7Device::set_texture(DWORD stage, QddSurface *s)
{
    if (stage >= Q7_STAGES)
        return D3D_OK;
    if (recording) {
        recording->st.tex[stage] = s;
        recording->tex_mask |= 1u << stage;
        return D3D_OK;
    }
    QddSurface *old = st.tex[stage];
    if (old == s)
        return D3D_OK;
    if (s)
        qdd_surface_iface7(s)->AddRef();
    st.tex[stage] = s;
    derived.valid = false;
    if (old)
        qdd_surface_iface7(old)->Release();
    return D3D_OK;
}

HRESULT Q7Device::set_transform(DWORD state, const D3DMATRIX *m)
{
    int slot = q7_xf_slot(state);
    if (slot < 0)
        return D3D_OK; /* native accepts and ignores other numbers */
    if (recording) {
        recording->st.xf[slot] = *m;
        recording->xf_mask |= 1u << slot;
        return D3D_OK;
    }
    st.xf[slot] = *m;
    if (is_active())
        apply_transform((unsigned)slot);
    return D3D_OK;
}

HRESULT Q7Device::set_viewport(const d7::D3DVIEWPORT7 *vp)
{
    if (recording) {
        recording->st.vp = *vp;
        recording->has_vp = true;
        return D3D_OK;
    }
    st.vp = *vp;
    if (is_active())
        apply_viewport();
    return D3D_OK;
}

HRESULT Q7Device::set_material(const d7::D3DMATERIAL7 *m)
{
    if (recording) {
        recording->st.mat = *m;
        recording->has_mat = true;
        return D3D_OK;
    }
    st.mat = *m;
    if (m->dvPower != 0.0f)
        pv_power = m->dvPower;
    IDirect3DDevice9 *d9;
    if (is_active() && (d9 = d3d->device9())) {
        D3DMATERIAL9 m9;
        memcpy(&m9, m, sizeof(m9));
        d9->SetMaterial(&m9);
    }
    return D3D_OK;
}

HRESULT Q7Device::set_light(DWORD index, const d7::D3DLIGHT7 *l)
{
    if (recording) {
        Q7SBLight *sl = q7_sb_light(recording, index, true);
        if (!sl)
            return DDERR_OUTOFMEMORY;
        sl->light = *l;
        sl->has_light = true;
        return D3D_OK;
    }
    Q7Light *L = q7_light_add(&st, index);
    if (!L)
        return DDERR_OUTOFMEMORY;
    L->light = *l;
    if (is_active())
        apply_light(L);
    return D3D_OK;
}

HRESULT Q7Device::light_enable(DWORD index, BOOL enable)
{
    if (recording) {
        Q7SBLight *sl = q7_sb_light(recording, index, true);
        if (!sl)
            return DDERR_OUTOFMEMORY;
        sl->has_enable = true;
        sl->enabled = enable != 0;
        return D3D_OK;
    }
    /* Enabling a light that was never set creates the default light. */
    Q7Light *L = q7_light_add(&st, index);
    if (!L)
        return DDERR_OUTOFMEMORY;
    bool was = L->enabled;
    L->enabled = enable != 0;
    if (is_active() && was != L->enabled)
        apply_lights(); /* which lights are on can shift (MaxActiveLights) */
    else if (is_active())
        apply_light(L);
    return D3D_OK;
}

HRESULT Q7Device::set_clip_plane(DWORD index, const float *plane)
{
    if (index >= Q7_CLIPPLANES)
        return D3D_OK; /* native accepts any index */
    if (recording) {
        memcpy(recording->st.clip[index], plane, sizeof(float) * 4);
        recording->clip_mask |= 1u << index;
        return D3D_OK;
    }
    memcpy(st.clip[index], plane, sizeof(float) * 4);
    IDirect3DDevice9 *d9;
    if (is_active() && (d9 = d3d->device9()) && d3d->get_caps() && index < d3d->caps9.MaxUserClipPlanes)
        d9->SetClipPlane(index, st.clip[index]);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Methods                                                                   */

HRESULT STDMETHODCALLTYPE Q7Device::GetCaps(d7::D3DDEVICEDESC7 *desc)
{
    if (!desc)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    HRESULT hr = d3d->device_desc(&guid, desc);
    /* Measured: a HAL device's GetCaps reports the T&L device's device caps
     * (HWTRANSFORMANDLIGHT included), unlike its EnumDevices entry. */
    if (SUCCEEDED(hr) && IsEqualGUID(guid, Q7IID_IDirect3DHALDevice) && d3d->get_caps())
        desc->dwDevCaps |= d3d->caps9.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    return hr;
}

HRESULT STDMETHODCALLTYPE Q7Device::GetDirect3D(d7::IDirect3D7 **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    *out = NULL;
    Q7_ALIVE();
    d3d->AddRef();
    *out = d3d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::BeginScene()
{
    Q7Lock lock;
    Q7_ALIVE();
    if (in_scene)
        return D3DERR_SCENE_IN_SCENE;
    in_scene = true;
    /* The D3D9 scene opens with the first draw. */
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::EndScene()
{
    Q7Lock lock;
    Q7_ALIVE();
    if (!in_scene)
        return D3DERR_SCENE_NOT_IN_SCENE;
    in_scene = false;
    d3d->end_scene();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::SetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    return set_transform(state, (const D3DMATRIX *)m);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    int slot = q7_xf_slot(state);
    if (slot < 0)
        return DDERR_INVALIDPARAMS;
    memcpy(m, &st.xf[slot], sizeof(*m));
    return D3D_OK;
}

/* Native: the product applies to the device's state at once, also while a
 * state block records (Wine's ddraw does the same). */
HRESULT STDMETHODCALLTYPE Q7Device::MultiplyTransform(d7::D3DTRANSFORMSTATETYPE state, d7::D3DMATRIX *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    int slot = q7_xf_slot(state);
    if (slot < 0)
        return D3D_OK;
    D3DMATRIX r;
    q7_mat_mul(&r, (const D3DMATRIX *)m, &st.xf[slot]);
    st.xf[slot] = r;
    if (is_active())
        apply_transform((unsigned)slot);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::SetViewport(d7::D3DVIEWPORT7 *vp)
{
    if (!vp)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (!rt || !rt_valid)
        return DDERR_INVALIDCAPS;
    const DDSURFACEDESC2 *desc = qdd_surface_desc(rt);
    if (vp->dwX > desc->dwWidth || vp->dwWidth > desc->dwWidth - vp->dwX || vp->dwY > desc->dwHeight ||
        vp->dwHeight > desc->dwHeight - vp->dwY)
        return E_INVALIDARG;
    return set_viewport(vp);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetViewport(d7::D3DVIEWPORT7 *vp)
{
    if (!vp)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    *vp = st.vp;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::SetMaterial(d7::D3DMATERIAL7 *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    return set_material(m);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetMaterial(d7::D3DMATERIAL7 *m)
{
    if (!m)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    *m = st.mat;
    return D3D_OK;
}

/* Native validation (Wine's d3d.c LightTest): known types only; point and
 * spot lights need non-negative attenuation (NaN passes). */
static bool q7_light_valid(const d7::D3DLIGHT7 *l)
{
    switch (l->dltType) {
    case d7::D3DLIGHT_DIRECTIONAL:
        return true;
    case d7::D3DLIGHT_POINT:
    case d7::D3DLIGHT_SPOT:
        return !(l->dvAttenuation0 < 0.0f) && !(l->dvAttenuation1 < 0.0f) && !(l->dvAttenuation2 < 0.0f);
    default:
        return false;
    }
}

HRESULT STDMETHODCALLTYPE Q7Device::SetLight(DWORD index, d7::D3DLIGHT7 *l)
{
    if (!l || !q7_light_valid(l))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    return set_light(index, l);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetLight(DWORD index, d7::D3DLIGHT7 *l)
{
    if (!l)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    Q7Light *L = q7_light_find(&st, index);
    if (!L)
        return DDERR_INVALIDPARAMS;
    *l = L->light;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::LightEnable(DWORD index, WINBOOL enable)
{
    Q7Lock lock;
    Q7_ALIVE();
    return light_enable(index, enable);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetLightEnable(DWORD index, WINBOOL *enable)
{
    if (!enable)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    Q7Light *L = q7_light_find(&st, index);
    if (!L)
        return DDERR_INVALIDPARAMS;
    *enable = L->enabled ? 128 : FALSE; /* the stock runtime's TRUE (measured) */
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::SetRenderState(d7::D3DRENDERSTATETYPE state, DWORD value)
{
    Q7Lock lock;
    Q7_ALIVE();
    return set_rs((DWORD)state, value);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetRenderState(d7::D3DRENDERSTATETYPE state, LPDWORD value)
{
    if (!value)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (q7_rs_kind((DWORD)state) == Q7RS_INVALID)
        return DDERR_INVALIDPARAMS;
    *value = st.rs[state];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::GetTextureStageState(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type,
                                                         LPDWORD value)
{
    if (!value)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (type >= Q7_TSS)
        return D3D_OK;
    *value = stage < Q7_STAGES ? st.tss[stage][type] : 0; /* native: 0 past the eighth stage */
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::SetTextureStageState(DWORD stage, d7::D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    Q7Lock lock;
    Q7_ALIVE();
    return set_tss(stage, (DWORD)type, value);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetTexture(DWORD stage, IDirectDrawSurface7 **out)
{
    if (!out)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    *out = NULL;
    Q7_ALIVE();
    if (stage >= Q7_STAGES || !st.tex[stage])
        return D3D_OK;
    IDirectDrawSurface7 *s = qdd_surface_iface7(st.tex[stage]);
    s->AddRef();
    *out = s;
    return D3D_OK;
}

/* Native: a surface without DDSCAPS_TEXTURE is accepted but not bound
 * (GetTexture then returns NULL, Wine's test_texture_wrong_caps); a pointer
 * that is no surface of ours unbinds. */
HRESULT STDMETHODCALLTYPE Q7Device::SetTexture(DWORD stage, IDirectDrawSurface7 *surface)
{
    Q7Lock lock;
    Q7_ALIVE();
    QddSurface *s = surface ? qdd_surface_lookup(surface) : NULL;
    if (s && (qdd_surface_draw(s) != dd || !(qdd_surface_desc(s)->ddsCaps.dwCaps & DDSCAPS_TEXTURE)))
        s = NULL;
    return set_texture(stage, s);
}

HRESULT STDMETHODCALLTYPE Q7Device::SetClipPlane(DWORD index, d7::D3DVALUE *plane)
{
    if (!plane)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    return set_clip_plane(index, plane);
}

HRESULT STDMETHODCALLTYPE Q7Device::GetClipPlane(DWORD index, d7::D3DVALUE *plane)
{
    if (!plane)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (index >= Q7_CLIPPLANES)
        memset(plane, 0, sizeof(float) * 4);
    else
        memcpy(plane, st.clip[index], sizeof(float) * 4);
    return D3D_OK;
}

/* Clip status is kept as set; the extents are not tracked (the stock HAL
 * runtime computes them only for software vertex processing). */
HRESULT STDMETHODCALLTYPE Q7Device::SetClipStatus(d7::D3DCLIPSTATUS *cs)
{
    if (!cs)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (cs->dwFlags & D3DCLIPSTATUS_STATUS)
        clipstatus.dwStatus = cs->dwStatus;
    if (cs->dwFlags & (D3DCLIPSTATUS_EXTENTS2 | D3DCLIPSTATUS_EXTENTS3)) {
        clipstatus.minx = cs->minx;
        clipstatus.maxx = cs->maxx;
        clipstatus.miny = cs->miny;
        clipstatus.maxy = cs->maxy;
    }
    if (cs->dwFlags & D3DCLIPSTATUS_EXTENTS3) {
        clipstatus.minz = cs->minz;
        clipstatus.maxz = cs->maxz;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::GetClipStatus(d7::D3DCLIPSTATUS *cs)
{
    if (!cs)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    /* Native writes the 2D extents only (minz/maxz stay untouched). */
    cs->dwFlags = clipstatus.dwFlags;
    cs->dwStatus = clipstatus.dwStatus;
    cs->minx = clipstatus.minx;
    cs->maxx = clipstatus.maxx;
    cs->miny = clipstatus.miny;
    cs->maxy = clipstatus.maxy;
    return D3D_OK;
}

/* Native retail runtimes answer S_FALSE for every identifier (measured). */
HRESULT STDMETHODCALLTYPE Q7Device::GetInfo(DWORD id, void *info, DWORD size)
{
    return S_FALSE;
}

/* ------------------------------------------------------------------------ */
/* State blocks                                                              */

Q7StateBlock *Q7Device::sb_get(DWORD handle)
{
    if (!handle || handle > nsbs)
        return NULL;
    return sbs[handle - 1];
}

static HRESULT q7_sb_add(Q7Device *dev, Q7StateBlock *sb, DWORD *handle)
{
    for (UINT i = 0; i < dev->nsbs; ++i)
        if (!dev->sbs[i]) {
            dev->sbs[i] = sb;
            *handle = i + 1;
            return D3D_OK;
        }
    Q7StateBlock **n = (Q7StateBlock **)realloc(dev->sbs, (dev->nsbs + 1) * sizeof(*n));
    if (!n)
        return DDERR_OUTOFMEMORY;
    dev->sbs = n;
    dev->sbs[dev->nsbs++] = sb;
    *handle = dev->nsbs;
    return D3D_OK;
}

void Q7Device::sb_capture(Q7StateBlock *sb)
{
    for (DWORD s = 0; s < Q7_RS; ++s)
        if (q7_mask_has(sb->rs_mask, s))
            sb->st.rs[s] = st.rs[s];
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage)
        for (DWORD t = 0; t < Q7_TSS; ++t)
            if (sb->tss_mask[stage] & (1u << t))
                sb->st.tss[stage][t] = st.tss[stage][t];
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage)
        if (sb->tex_mask & (1u << stage))
            sb->st.tex[stage] = st.tex[stage];
    for (DWORD i = 0; i < Q7_XF; ++i)
        if (sb->xf_mask & (1u << i))
            sb->st.xf[i] = st.xf[i];
    for (DWORD i = 0; i < Q7_CLIPPLANES; ++i)
        if (sb->clip_mask & (1u << i))
            memcpy(sb->st.clip[i], st.clip[i], sizeof(st.clip[i]));
    if (sb->has_vp)
        sb->st.vp = st.vp;
    if (sb->has_mat)
        sb->st.mat = st.mat;
    if (sb->all_lights) {
        /* Every light the device knows (vertex blocks: the enables only). */
        for (UINT i = 0; i < st.nlights; ++i) {
            Q7SBLight *sl = q7_sb_light(sb, st.lights[i].index, true);
            if (!sl)
                break;
            sl->light = st.lights[i].light;
            sl->enabled = st.lights[i].enabled;
            sl->has_light = !sb->light_enables_only;
            sl->has_enable = true;
        }
    } else {
        for (UINT i = 0; i < sb->nlights; ++i) {
            Q7SBLight *sl = &sb->lights[i];
            Q7Light *L = q7_light_find(&st, sl->index);
            if (!L)
                continue;
            if (sl->has_light)
                sl->light = L->light;
            if (sl->has_enable)
                sl->enabled = L->enabled;
        }
    }
}

void Q7Device::sb_apply(Q7StateBlock *sb)
{
    for (DWORD s = 0; s < Q7_RS; ++s)
        if (q7_mask_has(sb->rs_mask, s))
            set_rs(s, sb->st.rs[s]);
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage)
        for (DWORD t = 0; t < Q7_TSS; ++t)
            if (sb->tss_mask[stage] & (1u << t))
                set_tss(stage, t, sb->st.tss[stage][t]);
    for (DWORD stage = 0; stage < Q7_STAGES; ++stage)
        if (sb->tex_mask & (1u << stage))
            set_texture(stage, sb->st.tex[stage]);
    for (DWORD i = 0; i < Q7_XF; ++i)
        if (sb->xf_mask & (1u << i))
            set_transform(i < 6 ? i + 1 : d7::D3DTRANSFORMSTATE_TEXTURE0 + (i - 6), &sb->st.xf[i]);
    for (DWORD i = 0; i < Q7_CLIPPLANES; ++i)
        if (sb->clip_mask & (1u << i))
            set_clip_plane(i, sb->st.clip[i]);
    if (sb->has_vp)
        set_viewport(&sb->st.vp);
    if (sb->has_mat)
        set_material(&sb->st.mat);
    for (UINT i = 0; i < sb->nlights; ++i) {
        Q7SBLight *sl = &sb->lights[i];
        if (sl->has_light)
            set_light(sl->index, &sl->light);
        if (sl->has_enable)
            light_enable(sl->index, sl->enabled);
    }
}

HRESULT STDMETHODCALLTYPE Q7Device::BeginStateBlock()
{
    Q7Lock lock;
    Q7_ALIVE();
    if (recording)
        return D3DERR_INBEGINSTATEBLOCK;
    Q7StateBlock *sb = (Q7StateBlock *)calloc(1, sizeof(*sb));
    if (!sb)
        return DDERR_OUTOFMEMORY;
    q7_state_defaults(&sb->st, false);
    recording = sb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::EndStateBlock(LPDWORD handle)
{
    if (!handle)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (!recording)
        return D3DERR_NOTINBEGINSTATEBLOCK;
    Q7StateBlock *sb = recording;
    recording = NULL;
    HRESULT hr = q7_sb_add(this, sb, handle);
    if (FAILED(hr)) {
        q7_sb_free(sb);
        *handle = 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Q7Device::ApplyStateBlock(DWORD handle)
{
    Q7Lock lock;
    Q7_ALIVE();
    if (recording)
        return D3DERR_INBEGINSTATEBLOCK;
    /* Native (measured): a zero, deleted or random handle applies nothing
     * and succeeds; Capture and Delete refuse it. */
    Q7StateBlock *sb = sb_get(handle);
    if (!sb)
        return D3D_OK;
    sb_apply(sb);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::CaptureStateBlock(DWORD handle)
{
    Q7Lock lock;
    Q7_ALIVE();
    if (recording)
        return D3DERR_INBEGINSTATEBLOCK;
    Q7StateBlock *sb = sb_get(handle);
    if (!sb)
        return D3DERR_INVALIDSTATEBLOCK;
    sb_capture(sb);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::DeleteStateBlock(DWORD handle)
{
    Q7Lock lock;
    Q7_ALIVE();
    Q7StateBlock *sb = sb_get(handle);
    if (!sb)
        return D3DERR_INVALIDSTATEBLOCK;
    sbs[handle - 1] = NULL;
    q7_sb_free(sb);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7Device::CreateStateBlock(d7::D3DSTATEBLOCKTYPE type, LPDWORD handle)
{
    if (!handle)
        return DDERR_INVALIDPARAMS;
    if (type != d7::D3DSBT_ALL && type != d7::D3DSBT_PIXELSTATE && type != d7::D3DSBT_VERTEXSTATE)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7_ALIVE();
    if (recording)
        return D3DERR_INBEGINSTATEBLOCK;
    Q7StateBlock *sb = (Q7StateBlock *)calloc(1, sizeof(*sb));
    if (!sb)
        return DDERR_OUTOFMEMORY;
    q7_state_defaults(&sb->st, false);
    q7_sb_init_type(sb, (DWORD)type);
    sb_capture(sb);
    HRESULT hr = q7_sb_add(this, sb, handle);
    if (FAILED(hr))
        q7_sb_free(sb);
    return hr;
}
