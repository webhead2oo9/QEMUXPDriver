/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: state tables and defaults, state block membership, FVF layout,
 * DirectDraw texture format classification, the object registry.
 *
 * Defaults and the accepted render state / transform / stage state numbers
 * were measured on the stock 32-bit ddraw.dll (Windows 11, T&L HAL device
 * on a 64x64 X8R8G8B8 target with a 16-bit z-buffer attached).
 */
#include "qdd_d3d7.h"

/* ------------------------------------------------------------------------ */
/* Transforms                                                                */

int q7_xf_slot(DWORD state)
{
    if (state >= d7::D3DTRANSFORMSTATE_WORLD && state <= d7::D3DTRANSFORMSTATE_WORLD3)
        return (int)state - 1; /* world, view, projection, world1-3 -> 0-5 */
    if (state >= d7::D3DTRANSFORMSTATE_TEXTURE0 && state <= d7::D3DTRANSFORMSTATE_TEXTURE7)
        return (int)(state - d7::D3DTRANSFORMSTATE_TEXTURE0) + 6;
    return -1;
}

D3DTRANSFORMSTATETYPE q7_xf_d3d9(unsigned slot)
{
    switch (slot) {
    case 0: return D3DTS_WORLD;
    case 1: return D3DTS_VIEW;
    case 2: return D3DTS_PROJECTION;
    case 3: return D3DTS_WORLD1;
    case 4: return D3DTS_WORLD2;
    case 5: return D3DTS_WORLD3;
    default: return (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + (slot - 6));
    }
}

void q7_mat_mul(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] +
                        a->m[i][3] * b->m[3][j];
    *out = r;
}

/* ------------------------------------------------------------------------ */
/* Render states                                                             */

/*
 * Measured on the stock runtime: GetRenderState/SetRenderState fail with
 * DDERR_INVALIDPARAMS for the retired numbers below; every other number
 * below 256 is accepted and kept (unknown ones read 0xffffffff until set).
 */
Q7RSKind q7_rs_kind(DWORD s)
{
    if (s >= Q7_RS)
        return Q7RS_INVALID;
    switch (s) {
    case 1: case 3: case 5: case 6: case 11: case 12: case 13: case 17: case 18: case 21:
    case 31: case 32: case 39: case 43: case 44: case 45: case 46: case 49: case 51:
        return Q7RS_INVALID;
    case d7::D3DRENDERSTATE_ALPHATESTENABLE:
    case d7::D3DRENDERSTATE_ALPHAREF:
    case d7::D3DRENDERSTATE_ALPHAFUNC:
    case d7::D3DRENDERSTATE_ALPHABLENDENABLE:
    case d7::D3DRENDERSTATE_SHADEMODE:
    case d7::D3DRENDERSTATE_EDGEANTIALIAS:
    case d7::D3DRENDERSTATE_COLORKEYENABLE:
    case d7::D3DRENDERSTATE_ZBIAS:
    case d7::D3DRENDERSTATE_COLORKEYBLENDENABLE:
        return Q7RS_SPECIAL;
    case d7::D3DRENDERSTATE_ZENABLE:
    case d7::D3DRENDERSTATE_FILLMODE:
    case d7::D3DRENDERSTATE_ZWRITEENABLE:
    case d7::D3DRENDERSTATE_LASTPIXEL:
    case d7::D3DRENDERSTATE_SRCBLEND:
    case d7::D3DRENDERSTATE_DESTBLEND:
    case d7::D3DRENDERSTATE_CULLMODE:
    case d7::D3DRENDERSTATE_ZFUNC:
    case d7::D3DRENDERSTATE_DITHERENABLE:
    case d7::D3DRENDERSTATE_FOGENABLE:
    case d7::D3DRENDERSTATE_SPECULARENABLE:
    case d7::D3DRENDERSTATE_FOGCOLOR:
    case d7::D3DRENDERSTATE_FOGTABLEMODE:
    case d7::D3DRENDERSTATE_FOGSTART:
    case d7::D3DRENDERSTATE_FOGEND:
    case d7::D3DRENDERSTATE_FOGDENSITY:
    case d7::D3DRENDERSTATE_RANGEFOGENABLE:
    case d7::D3DRENDERSTATE_CLIPPING:
    case d7::D3DRENDERSTATE_LIGHTING:
    case d7::D3DRENDERSTATE_AMBIENT:
    case d7::D3DRENDERSTATE_FOGVERTEXMODE:
    case d7::D3DRENDERSTATE_COLORVERTEX:
    case d7::D3DRENDERSTATE_LOCALVIEWER:
    case d7::D3DRENDERSTATE_NORMALIZENORMALS:
    case d7::D3DRENDERSTATE_DIFFUSEMATERIALSOURCE:
    case d7::D3DRENDERSTATE_SPECULARMATERIALSOURCE:
    case d7::D3DRENDERSTATE_AMBIENTMATERIALSOURCE:
    case d7::D3DRENDERSTATE_EMISSIVEMATERIALSOURCE:
    case d7::D3DRENDERSTATE_VERTEXBLEND:
    case d7::D3DRENDERSTATE_CLIPPLANEENABLE:
        return Q7RS_D3D9;
    default:
        if (s >= 64 && s <= 95) /* STIPPLEPATTERN00-31 */
            return Q7RS_INVALID;
        if (s >= d7::D3DRENDERSTATE_STENCILENABLE && s <= d7::D3DRENDERSTATE_TEXTUREFACTOR)
            return Q7RS_D3D9;
        if (s >= d7::D3DRENDERSTATE_WRAP0 && s <= d7::D3DRENDERSTATE_WRAP7)
            return Q7RS_D3D9;
        /* ANTIALIAS, TEXTUREPERSPECTIVE, LINEPATTERN, ZVISIBLE, STIPPLEDALPHA,
         * FLUSHBATCH, EXTENTS and unassigned numbers: kept only. */
        return Q7RS_STORE;
    }
}

void q7_default_light(d7::D3DLIGHT7 *l)
{
    memset(l, 0, sizeof(*l));
    l->dltType = d7::D3DLIGHT_DIRECTIONAL;
    l->dcvDiffuse.r = 1.0f;
    l->dcvDiffuse.g = 1.0f;
    l->dcvDiffuse.b = 1.0f;
    l->dvDirection.z = 1.0f;
}

void q7_state_defaults(Q7State *st, bool zbuffer)
{
    Q7Light *lights = st->lights;
    UINT cap = st->caplights;

    memset(st, 0, sizeof(*st));
    st->lights = lights;
    st->caplights = cap;
    for (UINT i = 0; i < Q7_RS; ++i)
        st->rs[i] = 0xffffffffu;
    static const struct {
        DWORD state, value;
    } rs[] = {
        {d7::D3DRENDERSTATE_TEXTUREPERSPECTIVE, TRUE},
        {d7::D3DRENDERSTATE_FILLMODE, d7::D3DFILL_SOLID},
        {d7::D3DRENDERSTATE_SHADEMODE, d7::D3DSHADE_GOURAUD},
        {d7::D3DRENDERSTATE_LINEPATTERN, 0},
        {d7::D3DRENDERSTATE_ZWRITEENABLE, TRUE},
        {d7::D3DRENDERSTATE_ALPHATESTENABLE, FALSE},
        {d7::D3DRENDERSTATE_LASTPIXEL, TRUE},
        {d7::D3DRENDERSTATE_SRCBLEND, d7::D3DBLEND_ONE},
        {d7::D3DRENDERSTATE_DESTBLEND, d7::D3DBLEND_ZERO},
        {d7::D3DRENDERSTATE_CULLMODE, d7::D3DCULL_CCW},
        {d7::D3DRENDERSTATE_ZFUNC, d7::D3DCMP_LESSEQUAL},
        {d7::D3DRENDERSTATE_ALPHAREF, 0},
        {d7::D3DRENDERSTATE_ALPHAFUNC, d7::D3DCMP_ALWAYS},
        {d7::D3DRENDERSTATE_DITHERENABLE, FALSE},
        {d7::D3DRENDERSTATE_ALPHABLENDENABLE, FALSE},
        {d7::D3DRENDERSTATE_FOGENABLE, FALSE},
        {d7::D3DRENDERSTATE_SPECULARENABLE, FALSE},
        {d7::D3DRENDERSTATE_ZVISIBLE, FALSE},
        {d7::D3DRENDERSTATE_STIPPLEDALPHA, FALSE},
        {d7::D3DRENDERSTATE_FOGCOLOR, 0},
        {d7::D3DRENDERSTATE_FOGTABLEMODE, d7::D3DFOG_NONE},
        {d7::D3DRENDERSTATE_FOGSTART, 0},
        {d7::D3DRENDERSTATE_FOGEND, 0x3f800000u},
        {d7::D3DRENDERSTATE_FOGDENSITY, 0x3f800000u},
        {d7::D3DRENDERSTATE_COLORKEYENABLE, FALSE},
        {d7::D3DRENDERSTATE_ZBIAS, 0},
        {d7::D3DRENDERSTATE_RANGEFOGENABLE, FALSE},
        {d7::D3DRENDERSTATE_STENCILENABLE, FALSE},
        {d7::D3DRENDERSTATE_STENCILFAIL, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILZFAIL, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILPASS, d7::D3DSTENCILOP_KEEP},
        {d7::D3DRENDERSTATE_STENCILFUNC, d7::D3DCMP_ALWAYS},
        {d7::D3DRENDERSTATE_STENCILREF, 0},
        {d7::D3DRENDERSTATE_STENCILMASK, 0xffffffffu},
        {d7::D3DRENDERSTATE_STENCILWRITEMASK, 0xffffffffu},
        {d7::D3DRENDERSTATE_TEXTUREFACTOR, 0xffffffffu},
        {d7::D3DRENDERSTATE_CLIPPING, TRUE},
        {d7::D3DRENDERSTATE_LIGHTING, TRUE},
        {d7::D3DRENDERSTATE_EXTENTS, FALSE},
        {d7::D3DRENDERSTATE_AMBIENT, 0},
        {d7::D3DRENDERSTATE_FOGVERTEXMODE, d7::D3DFOG_NONE},
        {d7::D3DRENDERSTATE_COLORVERTEX, TRUE},
        {d7::D3DRENDERSTATE_LOCALVIEWER, TRUE},
        {d7::D3DRENDERSTATE_NORMALIZENORMALS, FALSE},
        {d7::D3DRENDERSTATE_COLORKEYBLENDENABLE, FALSE},
        {d7::D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, d7::D3DMCS_COLOR1},
        {d7::D3DRENDERSTATE_SPECULARMATERIALSOURCE, d7::D3DMCS_COLOR2},
        {d7::D3DRENDERSTATE_AMBIENTMATERIALSOURCE, d7::D3DMCS_MATERIAL},
        {d7::D3DRENDERSTATE_EMISSIVEMATERIALSOURCE, d7::D3DMCS_MATERIAL},
        {d7::D3DRENDERSTATE_VERTEXBLEND, d7::D3DVBLEND_DISABLE},
        {d7::D3DRENDERSTATE_CLIPPLANEENABLE, 0},
    };
    for (UINT i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i)
        st->rs[rs[i].state] = rs[i].value;
    for (UINT i = 0; i < 8; ++i)
        st->rs[d7::D3DRENDERSTATE_WRAP0 + i] = 0;
    st->rs[d7::D3DRENDERSTATE_ZENABLE] = zbuffer ? d7::D3DZB_TRUE : d7::D3DZB_FALSE;

    for (UINT s = 0; s < Q7_STAGES; ++s) {
        DWORD *t = st->tss[s];
        for (UINT i = 0; i < Q7_TSS; ++i)
            t[i] = 0;
        t[0] = 0xffffffffu;
        for (UINT i = 25; i < Q7_TSS; ++i)
            t[i] = 0xffffffffu;
        t[d7::D3DTSS_COLOROP] = s ? d7::D3DTOP_DISABLE : d7::D3DTOP_MODULATE;
        t[d7::D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[d7::D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[d7::D3DTSS_ALPHAOP] = s ? d7::D3DTOP_DISABLE : d7::D3DTOP_SELECTARG1;
        t[d7::D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[d7::D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[d7::D3DTSS_TEXCOORDINDEX] = 0; /* every stage, unlike D3D8/9 */
        t[d7::D3DTSS_ADDRESS] = d7::D3DTADDRESS_WRAP;
        t[d7::D3DTSS_ADDRESSU] = d7::D3DTADDRESS_WRAP;
        t[d7::D3DTSS_ADDRESSV] = d7::D3DTADDRESS_WRAP;
        t[d7::D3DTSS_MAGFILTER] = d7::D3DTFG_POINT;
        t[d7::D3DTSS_MINFILTER] = d7::D3DTFN_POINT;
        t[d7::D3DTSS_MIPFILTER] = d7::D3DTFP_NONE;
        t[d7::D3DTSS_MAXANISOTROPY] = 1;
    }
    for (UINT i = 0; i < Q7_XF; ++i) {
        D3DMATRIX *m = &st->xf[i];
        memset(m, 0, sizeof(*m));
        m->_11 = m->_22 = m->_33 = m->_44 = 1.0f;
    }
    st->vp.dvMaxZ = 1.0f;
}

void q7_state_free(Q7State *st)
{
    free(st->lights);
    st->lights = NULL;
    st->nlights = st->caplights = 0;
}

Q7Light *q7_light_find(Q7State *st, DWORD index)
{
    for (UINT i = 0; i < st->nlights; ++i)
        if (st->lights[i].index == index)
            return &st->lights[i];
    return NULL;
}

Q7Light *q7_light_add(Q7State *st, DWORD index)
{
    Q7Light *l = q7_light_find(st, index);
    if (l)
        return l;
    if (st->nlights == st->caplights) {
        UINT cap = st->caplights ? st->caplights * 2 : 8;
        if (cap > 0x100000u)
            return NULL;
        Q7Light *n = (Q7Light *)realloc(st->lights, cap * sizeof(*n));
        if (!n)
            return NULL;
        st->lights = n;
        st->caplights = cap;
    }
    l = &st->lights[st->nlights++];
    l->index = index;
    q7_default_light(&l->light);
    l->enabled = false;
    return l;
}

/* ------------------------------------------------------------------------ */
/* State block membership                                                    */

/* The Direct3D 8/9 predefined state block lists, restricted to the
 * Direct3D 7 states (the D3D7-only pixel states join the pixel list). */
static const BYTE q7_pixel_rs[] = {
    d7::D3DRENDERSTATE_ANTIALIAS, d7::D3DRENDERSTATE_TEXTUREPERSPECTIVE, d7::D3DRENDERSTATE_ZENABLE,
    d7::D3DRENDERSTATE_FILLMODE, d7::D3DRENDERSTATE_SHADEMODE, d7::D3DRENDERSTATE_LINEPATTERN,
    d7::D3DRENDERSTATE_ZWRITEENABLE, d7::D3DRENDERSTATE_ALPHATESTENABLE, d7::D3DRENDERSTATE_LASTPIXEL,
    d7::D3DRENDERSTATE_SRCBLEND, d7::D3DRENDERSTATE_DESTBLEND, d7::D3DRENDERSTATE_ZFUNC,
    d7::D3DRENDERSTATE_ALPHAREF, d7::D3DRENDERSTATE_ALPHAFUNC, d7::D3DRENDERSTATE_DITHERENABLE,
    d7::D3DRENDERSTATE_ALPHABLENDENABLE, d7::D3DRENDERSTATE_ZVISIBLE, d7::D3DRENDERSTATE_STIPPLEDALPHA,
    d7::D3DRENDERSTATE_FOGSTART, d7::D3DRENDERSTATE_FOGEND, d7::D3DRENDERSTATE_FOGDENSITY,
    d7::D3DRENDERSTATE_EDGEANTIALIAS, d7::D3DRENDERSTATE_COLORKEYENABLE, d7::D3DRENDERSTATE_ZBIAS,
    d7::D3DRENDERSTATE_STENCILENABLE, d7::D3DRENDERSTATE_STENCILFAIL, d7::D3DRENDERSTATE_STENCILZFAIL,
    d7::D3DRENDERSTATE_STENCILPASS, d7::D3DRENDERSTATE_STENCILFUNC, d7::D3DRENDERSTATE_STENCILREF,
    d7::D3DRENDERSTATE_STENCILMASK, d7::D3DRENDERSTATE_STENCILWRITEMASK, d7::D3DRENDERSTATE_TEXTUREFACTOR,
    d7::D3DRENDERSTATE_WRAP0, d7::D3DRENDERSTATE_WRAP1, d7::D3DRENDERSTATE_WRAP2, d7::D3DRENDERSTATE_WRAP3,
    d7::D3DRENDERSTATE_WRAP4, d7::D3DRENDERSTATE_WRAP5, d7::D3DRENDERSTATE_WRAP6, d7::D3DRENDERSTATE_WRAP7,
    d7::D3DRENDERSTATE_COLORKEYBLENDENABLE,
};

static const BYTE q7_vertex_rs[] = {
    d7::D3DRENDERSTATE_SHADEMODE, d7::D3DRENDERSTATE_CULLMODE, d7::D3DRENDERSTATE_FOGENABLE,
    d7::D3DRENDERSTATE_SPECULARENABLE, d7::D3DRENDERSTATE_FOGCOLOR, d7::D3DRENDERSTATE_FOGTABLEMODE,
    d7::D3DRENDERSTATE_FOGSTART, d7::D3DRENDERSTATE_FOGEND, d7::D3DRENDERSTATE_FOGDENSITY,
    d7::D3DRENDERSTATE_RANGEFOGENABLE, d7::D3DRENDERSTATE_CLIPPING, d7::D3DRENDERSTATE_LIGHTING,
    d7::D3DRENDERSTATE_EXTENTS, d7::D3DRENDERSTATE_AMBIENT, d7::D3DRENDERSTATE_FOGVERTEXMODE,
    d7::D3DRENDERSTATE_COLORVERTEX, d7::D3DRENDERSTATE_LOCALVIEWER, d7::D3DRENDERSTATE_NORMALIZENORMALS,
    d7::D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, d7::D3DRENDERSTATE_SPECULARMATERIALSOURCE,
    d7::D3DRENDERSTATE_AMBIENTMATERIALSOURCE, d7::D3DRENDERSTATE_EMISSIVEMATERIALSOURCE,
    d7::D3DRENDERSTATE_VERTEXBLEND, d7::D3DRENDERSTATE_CLIPPLANEENABLE,
};

static void q7_mask_set(DWORD *mask, DWORD bit)
{
    mask[bit >> 5] |= 1u << (bit & 31);
}

void q7_sb_init_type(Q7StateBlock *sb, DWORD type)
{
    switch (type) {
    case d7::D3DSBT_ALL:
        for (UINT i = 0; i < Q7_RS; ++i)
            if (q7_rs_kind(i) != Q7RS_INVALID)
                q7_mask_set(sb->rs_mask, i);
        for (UINT s = 0; s < Q7_STAGES; ++s)
            sb->tss_mask[s] = 0xfffffffeu;
        sb->tex_mask = (1u << Q7_STAGES) - 1u;
        sb->xf_mask = (1u << Q7_XF) - 1u;
        sb->clip_mask = 0xffffffffu;
        sb->has_vp = sb->has_mat = true;
        sb->all_lights = true;
        break;
    case d7::D3DSBT_PIXELSTATE:
        for (UINT i = 0; i < sizeof(q7_pixel_rs); ++i)
            q7_mask_set(sb->rs_mask, q7_pixel_rs[i]);
        for (UINT s = 0; s < Q7_STAGES; ++s)
            sb->tss_mask[s] = ((1u << 25) - 1u) & ~1u; /* D3DTSS 1-24 */
        break;
    case d7::D3DSBT_VERTEXSTATE:
        for (UINT i = 0; i < sizeof(q7_vertex_rs); ++i)
            q7_mask_set(sb->rs_mask, q7_vertex_rs[i]);
        for (UINT s = 0; s < Q7_STAGES; ++s)
            sb->tss_mask[s] = (1u << d7::D3DTSS_TEXCOORDINDEX) | (1u << d7::D3DTSS_TEXTURETRANSFORMFLAGS);
        /* Measured: the stock vertex block keeps light enables, not their
         * parameters; neither type keeps transforms, material, viewport,
         * clip planes or textures (only D3DSBT_ALL does). */
        sb->all_lights = true;
        sb->light_enables_only = true;
        break;
    }
}

void q7_sb_free(Q7StateBlock *sb)
{
    if (!sb)
        return;
    free(sb->lights);
    free(sb);
}

Q7SBLight *q7_sb_light(Q7StateBlock *sb, DWORD index, bool add)
{
    for (UINT i = 0; i < sb->nlights; ++i)
        if (sb->lights[i].index == index)
            return &sb->lights[i];
    if (!add)
        return NULL;
    if (sb->nlights == sb->caplights) {
        UINT cap = sb->caplights ? sb->caplights * 2 : 8;
        if (cap > 0x100000u)
            return NULL;
        Q7SBLight *n = (Q7SBLight *)realloc(sb->lights, cap * sizeof(*n));
        if (!n)
            return NULL;
        sb->lights = n;
        sb->caplights = cap;
    }
    Q7SBLight *l = &sb->lights[sb->nlights++];
    memset(l, 0, sizeof(*l));
    l->index = index;
    q7_default_light(&l->light);
    return l;
}

/* ------------------------------------------------------------------------ */
/* FVF                                                                       */

bool q7_fvf_layout(DWORD fvf, Q7FvfLayout *l)
{
    UINT off = 0;

    memset(l, 0, sizeof(*l));
    l->pos = l->normal = l->reserved = l->diffuse = l->specular = -1;
    if (fvf & (D3DFVF_RESERVED0 | 0xf000u))
        return false;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: l->posfloats = 3; break;
    case D3DFVF_XYZRHW: l->posfloats = 4; break;
    case D3DFVF_XYZB1: l->posfloats = 3; l->nbeta = 1; break;
    case D3DFVF_XYZB2: l->posfloats = 3; l->nbeta = 2; break;
    case D3DFVF_XYZB3: l->posfloats = 3; l->nbeta = 3; break;
    case D3DFVF_XYZB4: l->posfloats = 3; l->nbeta = 4; break;
    case D3DFVF_XYZB5: l->posfloats = 3; l->nbeta = 5; break;
    default: return false;
    }
    l->pos = 0;
    off = (l->posfloats + (UINT)l->nbeta) * 4u;
    if (fvf & D3DFVF_NORMAL) {
        l->normal = (int)off;
        off += 12;
    }
    if (fvf & D3DFVF_RESERVED1) {
        l->reserved = (int)off;
        off += 4;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        l->diffuse = (int)off;
        off += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        l->specular = (int)off;
        off += 4;
    }
    l->ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (l->ntex > 8)
        return false;
    for (UINT i = 0; i < l->ntex; ++i) {
        static const UINT dims[4] = {2, 3, 4, 1};
        l->tex[i] = (int)off;
        l->texdim[i] = dims[(fvf >> (16 + 2 * i)) & 3];
        off += l->texdim[i] * 4u;
    }
    for (UINT i = l->ntex; i < 8; ++i)
        l->tex[i] = -1;
    l->size = off;
    return true;
}

UINT q7_fvf_size(DWORD fvf)
{
    Q7FvfLayout l;
    return q7_fvf_layout(fvf, &l) ? l.size : 0;
}

UINT q7_prim_count(DWORD type, DWORD n)
{
    switch (type) {
    case d7::D3DPT_POINTLIST: return n;
    case d7::D3DPT_LINELIST: return n / 2;
    case d7::D3DPT_LINESTRIP: return n > 1 ? n - 1 : 0;
    case d7::D3DPT_TRIANGLELIST: return n / 3;
    case d7::D3DPT_TRIANGLESTRIP:
    case d7::D3DPT_TRIANGLEFAN: return n > 2 ? n - 2 : 0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Texture formats                                                           */

static bool q7_masks(const DDPIXELFORMAT *pf, DWORD r, DWORD g, DWORD b)
{
    return pf->dwRBitMask == r && pf->dwGBitMask == g && pf->dwBBitMask == b;
}

static void q7_fmt(Q7Format *f, D3DFORMAT src, D3DFORMAT d9, Q7Conv conv, D3DFORMAT d9_key, Q7Conv conv_key,
                   UINT bpp, DWORD key_mask, bool alpha)
{
    f->src = src;
    f->d9 = d9;
    f->conv = conv;
    f->d9_key = d9_key;
    f->conv_key = conv_key;
    f->bpp = bpp;
    f->block = false;
    f->key_mask = key_mask;
    f->has_alpha = alpha;
}

bool q7_texture_format(const DDPIXELFORMAT *pf, Q7Format *f)
{
    memset(f, 0, sizeof(*f));
    DWORD flags = pf->dwFlags;
    bool alpha = (flags & DDPF_ALPHAPIXELS) != 0;

    if (flags & DDPF_FOURCC) {
        D3DFORMAT d;
        switch (pf->dwFourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): d = D3DFMT_DXT1; break;
        case MAKEFOURCC('D', 'X', 'T', '2'): d = D3DFMT_DXT2; break;
        case MAKEFOURCC('D', 'X', 'T', '3'): d = D3DFMT_DXT3; break;
        case MAKEFOURCC('D', 'X', 'T', '4'): d = D3DFMT_DXT4; break;
        case MAKEFOURCC('D', 'X', 'T', '5'): d = D3DFMT_DXT5; break;
        default: return false;
        }
        q7_fmt(f, d, d, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 0, 0, true);
        f->block = true;
        return true;
    }
    if (flags & DDPF_PALETTEINDEXED8) {
        q7_fmt(f, D3DFMT_UNKNOWN, D3DFMT_A8R8G8B8, Q7C_PAL8, D3DFMT_A8R8G8B8, Q7C_PAL8, 8, 0xff, false);
        return true;
    }
    if (flags & DDPF_PALETTEINDEXED4) {
        q7_fmt(f, D3DFMT_UNKNOWN, D3DFMT_A8R8G8B8, Q7C_PAL4, D3DFMT_A8R8G8B8, Q7C_PAL4, 4, 0xf, false);
        return true;
    }
    if (flags & (DDPF_BUMPDUDV)) {
        DWORD bpp = pf->dwBumpBitCount;
        if (flags & DDPF_BUMPLUMINANCE) {
            if (bpp == 16 && pf->dwBumpDuBitMask == 0x001f && pf->dwBumpDvBitMask == 0x03e0)
                q7_fmt(f, D3DFMT_L6V5U5, D3DFMT_L6V5U5, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 16, 0, false);
            else if (bpp == 32 && pf->dwBumpDuBitMask == 0xff && pf->dwBumpDvBitMask == 0xff00)
                q7_fmt(f, D3DFMT_X8L8V8U8, D3DFMT_X8L8V8U8, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 32, 0, false);
            else
                return false;
        } else {
            if (bpp == 16 && pf->dwBumpDuBitMask == 0xff && pf->dwBumpDvBitMask == 0xff00)
                q7_fmt(f, D3DFMT_V8U8, D3DFMT_V8U8, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 16, 0, false);
            else if (bpp == 32 && pf->dwBumpDuBitMask == 0xffff && pf->dwBumpDvBitMask == 0xffff0000u)
                q7_fmt(f, D3DFMT_V16U16, D3DFMT_V16U16, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 32, 0, false);
            else
                return false;
        }
        return true;
    }
    if (flags & DDPF_LUMINANCE) {
        DWORD bpp = pf->dwLuminanceBitCount;
        if (!alpha && bpp == 8 && pf->dwLuminanceBitMask == 0xff)
            q7_fmt(f, D3DFMT_L8, D3DFMT_L8, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_L8_TO_8888, 8, 0xff, false);
        else if (alpha && bpp == 16 && pf->dwLuminanceBitMask == 0xff && pf->dwLuminanceAlphaBitMask == 0xff00)
            q7_fmt(f, D3DFMT_A8L8, D3DFMT_A8L8, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_A8L8_TO_8888, 16, 0xffff, true);
        else if (alpha && bpp == 8 && pf->dwLuminanceBitMask == 0x0f && pf->dwLuminanceAlphaBitMask == 0xf0)
            q7_fmt(f, D3DFMT_A4L4, D3DFMT_A4L4, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_A4L4_TO_8888, 8, 0xff, true);
        else
            return false;
        return true;
    }
    if (flags & DDPF_ALPHA) {
        if (pf->dwAlphaBitDepth != 8)
            return false;
        q7_fmt(f, D3DFMT_A8, D3DFMT_A8, Q7C_COPY, D3DFMT_UNKNOWN, Q7C_NONE, 8, 0, true);
        return true;
    }
    if (!(flags & DDPF_RGB))
        return false;
    DWORD a = alpha ? pf->dwRGBAlphaBitMask : 0;
    switch (pf->dwRGBBitCount) {
    case 32:
        if (q7_masks(pf, 0xff0000, 0xff00, 0xff)) {
            if (a == 0xff000000u)
                q7_fmt(f, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_COPY, 32, 0xffffffffu,
                       true);
            else
                q7_fmt(f, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_X8_TO_A8, 32, 0x00ffffff,
                       false);
            return true;
        }
        if (q7_masks(pf, 0xff, 0xff00, 0xff0000)) {
            q7_fmt(f, a == 0xff000000u ? D3DFMT_A8B8G8R8 : D3DFMT_X8B8G8R8, D3DFMT_A8R8G8B8, Q7C_BGRX_TO_8888,
                   D3DFMT_A8R8G8B8, Q7C_BGRX_TO_8888, 32, a == 0xff000000u ? 0xffffffffu : 0x00ffffffu,
                   a == 0xff000000u);
            return true;
        }
        return false;
    case 24:
        if (q7_masks(pf, 0xff0000, 0xff00, 0xff)) {
            q7_fmt(f, D3DFMT_R8G8B8, D3DFMT_X8R8G8B8, Q7C_888_TO_8888, D3DFMT_A8R8G8B8, Q7C_888_TO_8888, 24,
                   0xffffff, false);
            return true;
        }
        return false;
    case 16:
        if (q7_masks(pf, 0xf800, 0x07e0, 0x001f)) {
            q7_fmt(f, D3DFMT_R5G6B5, D3DFMT_R5G6B5, Q7C_COPY, D3DFMT_A8R8G8B8, Q7C_565_TO_8888, 16, 0xffff, false);
            return true;
        }
        if (q7_masks(pf, 0x7c00, 0x03e0, 0x001f)) {
            if (a == 0x8000)
                q7_fmt(f, D3DFMT_A1R5G5B5, D3DFMT_A1R5G5B5, Q7C_COPY, D3DFMT_A1R5G5B5, Q7C_COPY, 16, 0xffff, true);
            else
                q7_fmt(f, D3DFMT_X1R5G5B5, D3DFMT_X1R5G5B5, Q7C_COPY, D3DFMT_A1R5G5B5, Q7C_555_TO_1555, 16, 0x7fff,
                       false);
            return true;
        }
        if (q7_masks(pf, 0x0f00, 0x00f0, 0x000f)) {
            if (a == 0xf000)
                q7_fmt(f, D3DFMT_A4R4G4B4, D3DFMT_A4R4G4B4, Q7C_COPY, D3DFMT_A4R4G4B4, Q7C_COPY, 16, 0xffff, true);
            else
                q7_fmt(f, D3DFMT_X4R4G4B4, D3DFMT_A4R4G4B4, Q7C_444_TO_4444, D3DFMT_A4R4G4B4, Q7C_444_TO_4444, 16,
                       0x0fff, false);
            return true;
        }
        if (q7_masks(pf, 0x00e0, 0x001c, 0x0003) && a == 0xff00) {
            q7_fmt(f, D3DFMT_A8R3G3B2, D3DFMT_A8R8G8B8, Q7C_8332_TO_8888, D3DFMT_A8R8G8B8, Q7C_8332_TO_8888, 16,
                   0xffff, true);
            return true;
        }
        return false;
    case 8:
        if (q7_masks(pf, 0xe0, 0x1c, 0x03)) {
            q7_fmt(f, D3DFMT_R3G3B2, D3DFMT_X8R8G8B8, Q7C_332_TO_8888, D3DFMT_A8R8G8B8, Q7C_332_TO_8888, 8, 0xff,
                   false);
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool q7_ddpf_from_d3d9(D3DFORMAT f, DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    switch ((DWORD)f) {
    case D3DFMT_X1R5G5B5:
        pf->dwFlags = DDPF_RGB;
        pf->dwRGBBitCount = 16;
        pf->dwRBitMask = 0x7c00;
        pf->dwGBitMask = 0x03e0;
        pf->dwBBitMask = 0x001f;
        return true;
    case D3DFMT_A1R5G5B5:
        pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        pf->dwRGBBitCount = 16;
        pf->dwRBitMask = 0x7c00;
        pf->dwGBitMask = 0x03e0;
        pf->dwBBitMask = 0x001f;
        pf->dwRGBAlphaBitMask = 0x8000;
        return true;
    case D3DFMT_A4R4G4B4:
        pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        pf->dwRGBBitCount = 16;
        pf->dwRBitMask = 0x0f00;
        pf->dwGBitMask = 0x00f0;
        pf->dwBBitMask = 0x000f;
        pf->dwRGBAlphaBitMask = 0xf000;
        return true;
    case D3DFMT_R5G6B5:
        pf->dwFlags = DDPF_RGB;
        pf->dwRGBBitCount = 16;
        pf->dwRBitMask = 0xf800;
        pf->dwGBitMask = 0x07e0;
        pf->dwBBitMask = 0x001f;
        return true;
    case D3DFMT_X8R8G8B8:
        pf->dwFlags = DDPF_RGB;
        pf->dwRGBBitCount = 32;
        pf->dwRBitMask = 0xff0000;
        pf->dwGBitMask = 0x00ff00;
        pf->dwBBitMask = 0x0000ff;
        return true;
    case D3DFMT_A8R8G8B8:
        pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        pf->dwRGBBitCount = 32;
        pf->dwRBitMask = 0xff0000;
        pf->dwGBitMask = 0x00ff00;
        pf->dwBBitMask = 0x0000ff;
        pf->dwRGBAlphaBitMask = 0xff000000u;
        return true;
    case D3DFMT_P8:
        pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf->dwRGBBitCount = 8;
        return true;
    case D3DFMT_V8U8:
        pf->dwFlags = DDPF_BUMPDUDV;
        pf->dwBumpBitCount = 16;
        pf->dwBumpDuBitMask = 0x00ff;
        pf->dwBumpDvBitMask = 0xff00;
        return true;
    case D3DFMT_X8L8V8U8:
        pf->dwFlags = DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE;
        pf->dwBumpBitCount = 32;
        pf->dwBumpDuBitMask = 0x0000ff;
        pf->dwBumpDvBitMask = 0x00ff00;
        pf->dwBumpLuminanceBitMask = 0xff0000;
        return true;
    case D3DFMT_L6V5U5:
        pf->dwFlags = DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE;
        pf->dwBumpBitCount = 16;
        pf->dwBumpDuBitMask = 0x001f;
        pf->dwBumpDvBitMask = 0x03e0;
        pf->dwBumpLuminanceBitMask = 0xfc00;
        return true;
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        pf->dwFlags = DDPF_FOURCC;
        pf->dwFourCC = (DWORD)f;
        return true;
    case D3DFMT_L8:
        pf->dwFlags = DDPF_LUMINANCE;
        pf->dwLuminanceBitCount = 8;
        pf->dwLuminanceBitMask = 0xff;
        return true;
    case D3DFMT_A8:
        pf->dwFlags = DDPF_ALPHA;
        pf->dwAlphaBitDepth = 8;
        return true;
    case D3DFMT_A8L8:
        pf->dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        pf->dwLuminanceBitCount = 16;
        pf->dwLuminanceBitMask = 0x00ff;
        pf->dwLuminanceAlphaBitMask = 0xff00;
        return true;
    case D3DFMT_A4L4:
        pf->dwFlags = DDPF_LUMINANCE | DDPF_ALPHAPIXELS;
        pf->dwLuminanceBitCount = 8;
        pf->dwLuminanceBitMask = 0x0f;
        pf->dwLuminanceAlphaBitMask = 0xf0;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Registry                                                                  */

struct Q7RegEntry {
    const void *key;
    unsigned kind;
};

static Q7RegEntry *g_q7_reg;
static UINT g_q7_reg_count, g_q7_reg_cap;

bool q7_registry_add(const void *key, unsigned kind)
{
    if (g_q7_reg_count == g_q7_reg_cap) {
        UINT cap = g_q7_reg_cap ? g_q7_reg_cap * 2 : 64;
        Q7RegEntry *n = (Q7RegEntry *)realloc(g_q7_reg, cap * sizeof(*n));
        if (!n)
            return false;
        g_q7_reg = n;
        g_q7_reg_cap = cap;
    }
    /* Kept sorted by key for binary search. */
    UINT lo = 0, hi = g_q7_reg_count;
    while (lo < hi) {
        UINT mid = (lo + hi) / 2;
        if ((ULONG_PTR)g_q7_reg[mid].key < (ULONG_PTR)key)
            lo = mid + 1;
        else
            hi = mid;
    }
    memmove(&g_q7_reg[lo + 1], &g_q7_reg[lo], (g_q7_reg_count - lo) * sizeof(*g_q7_reg));
    g_q7_reg[lo].key = key;
    g_q7_reg[lo].kind = kind;
    ++g_q7_reg_count;
    return true;
}

static int q7_registry_find(const void *key)
{
    UINT lo = 0, hi = g_q7_reg_count;
    while (lo < hi) {
        UINT mid = (lo + hi) / 2;
        if ((ULONG_PTR)g_q7_reg[mid].key < (ULONG_PTR)key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g_q7_reg_count && g_q7_reg[lo].key == key ? (int)lo : -1;
}

void q7_registry_remove(const void *key)
{
    int i = q7_registry_find(key);
    if (i < 0)
        return;
    memmove(&g_q7_reg[i], &g_q7_reg[i + 1], (g_q7_reg_count - (UINT)i - 1) * sizeof(*g_q7_reg));
    --g_q7_reg_count;
}

bool q7_registry_has(const void *key, unsigned kind)
{
    int i = key ? q7_registry_find(key) : -1;
    return i >= 0 && g_q7_reg[i].kind == kind;
}
