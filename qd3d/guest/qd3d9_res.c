/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest-side resource state: descriptions recorded at creation (or fetched
 * lazily for objects the host named first), one lock backing per resource
 * laid out as the native runtime lays out system memory, UPLOAD after
 * unlocks (managed resources: of their dirty region before first use, as
 * the native runtime uploads them), READBACK for GPU-written contents, GetDC
 * through a DIB section, private data and priorities, and the overrides that
 * flag GPU-written destinations.
 */
#include <stdlib.h>
#include <string.h>

#include "qd3d9_priv.h"

/* ------------------------------------------------------------------------ */
/* Formats                                                                  */
/* ------------------------------------------------------------------------ */

struct qd3d_fmt {
    uint32_t bw, bh;    /* block size in texels */
    uint32_t bytes;     /* bytes per block */
    uint32_t planar;    /* chroma planes follow the luma plane */
    uint32_t nodata;    /* lockable, but has no contents to transfer */
};

#define QD3D_FOURCC(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#define QD3D_FMT_ATI1 QD3D_FOURCC('A', 'T', 'I', '1')
#define QD3D_FMT_ATI2 QD3D_FOURCC('A', 'T', 'I', '2')
#define QD3D_FMT_YV12 QD3D_FOURCC('Y', 'V', '1', '2')
#define QD3D_FMT_NV12 QD3D_FOURCC('N', 'V', '1', '2')
#define QD3D_FMT_NULL QD3D_FOURCC('N', 'U', 'L', 'L')

/* The lock view of every format an application can lock, as the native
 * runtime presents it (measured on the stock 32-bit runtime; the wire
 * contract is the same, qd3d_proto.h). Runtime memory (managed, system
 * memory, scratch) shows ATI1N/ATI2N and YV12/NV12 as one byte per texel
 * (no chroma space for YV12/NV12); driver memory (DEFAULT) shows ATI1N/ATI2N
 * as 4x4 blocks and YV12/NV12 with their chroma planes. D3DFMT_NULL render
 * targets lock (4 bytes per texel) but hold nothing. Returns 0 for formats
 * without a lockable layout (non-lockable depth formats, other FOURCCs). */
static int fmt_info(D3DFORMAT f, D3DPOOL pool, struct qd3d_fmt *fi)
{
    fi->bw = fi->bh = 1;
    fi->planar = fi->nodata = 0;
    switch ((DWORD)f) {
    case D3DFMT_DXT1:
        fi->bw = fi->bh = 4;
        fi->bytes = 8;
        return 1;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        fi->bw = fi->bh = 4;
        fi->bytes = 16;
        return 1;
    case QD3D_FMT_ATI1:
    case QD3D_FMT_ATI2:
        if (pool == D3DPOOL_DEFAULT) {
            fi->bw = fi->bh = 4;
            fi->bytes = (DWORD)f == QD3D_FMT_ATI1 ? 8u : 16u;
        } else {
            fi->bytes = 1;
        }
        return 1;
    case QD3D_FMT_YV12:
    case QD3D_FMT_NV12:
        fi->bytes = 1;
        fi->planar = pool == D3DPOOL_DEFAULT;
        return 1;
    case QD3D_FMT_NULL:
        fi->bytes = 4;
        fi->nodata = 1;
        return 1;
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:
    case D3DFMT_R8G8_B8G8:
    case D3DFMT_G8R8_G8B8:
        fi->bw = 2;
        fi->bytes = 4;
        return 1;
    case D3DFMT_A32B32G32R32F:
        fi->bytes = 16;
        return 1;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_Q16W16V16U16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
        fi->bytes = 8;
        return 1;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D32_LOCKABLE:
    case D3DFMT_R32F:
    case D3DFMT_G16R16F:
        fi->bytes = 4;
        return 1;
    case D3DFMT_R8G8B8:
        fi->bytes = 3;
        return 1;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_L16:
    case D3DFMT_R16F:
    case D3DFMT_CxV8U8:
        fi->bytes = 2;
        return 1;
    case D3DFMT_R3G3B2:
    case D3DFMT_A8:
    case D3DFMT_P8:
    case D3DFMT_L8:
    case D3DFMT_A4L4:
    case D3DFMT_S8_LOCKABLE:
        fi->bytes = 1;
        return 1;
    default:
        fi->bytes = 0;
        return 0;
    }
}

static uint32_t div_up(uint32_t v, uint32_t d)
{
    return (uint32_t)(((uint64_t)v + d - 1u) / d);
}

static uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = w > h ? w : h, n = 1;

    if (d > m)
        m = d;
    while (m > 1u) {
        m >>= 1;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Resource state                                                           */
/* ------------------------------------------------------------------------ */

enum {
    RES_BUFFER = 1,
    RES_SURFACE,
    RES_VOLUME,
    RES_TEXTURE,
    RES_CUBE,
    RES_VOLTEX
};

/* Half-open texel box (buffers: bytes in x); empty when any side is. */
struct qd3d_box {
    uint32_t x0, y0, z0, x1, y1, z1;
};

struct qd3d_sub {
    uint32_t off;                /* offset inside the resource's backing */
    uint32_t size;               /* bytes; 0: no lockable layout */
    uint32_t w, h, d;            /* texels (buffers: bytes, 1, 1) */
    uint32_t bx, by;             /* blocks per row, block rows */
    uint32_t row_pitch, slice_pitch;
    uint8_t gpu_dirty;           /* the host copy is newer: read back first */
    uint8_t locked;
    DWORD lock_flags;
    struct qd3d_box lk;          /* locked box, block-rounded texels */
    struct qd3d_box pend;        /* managed: what the host has not received */
};

struct qd3d_pdata {
    struct qd3d_pdata *next;
    GUID guid;
    DWORD flags;
    DWORD size;
    IUnknown *unk;               /* D3DSPD_IUNKNOWN */
    uint8_t data[];
};

struct qd3d_res {
    uint32_t kind;
    int desc_ok;
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    uint32_t width, height, depth, levels, faces;
    DWORD priority;
    struct qd3d_pdata *pdata;
    /* Level surface/volume of a texture: storage lives in the container. */
    struct qd3d_obj *container;
    uint32_t level, face;
    /* Texture: its level proxies, held so they share its lifetime (native
     * levels live as long as their container). */
    struct qd3d_obj **owned;
    uint32_t nowned, owned_cap;
    struct qd3d_sub *subs;
    uint32_t nsubs;
    /* One backing for every sub-resource, in the native system-memory
     * layout: levels one after another, cube faces one after another at a
     * 32-byte stride, 32-byte aligned. Allocated on the first lock. */
    uint8_t *mem;
    void *mem_block;
    uint32_t mem_size;
    int dirty_all;
    int pending;                 /* some sub has a pend box */
    uint32_t swapchain;          /* back buffer of swap chain swapchain - 1 */
    struct qd3d_obj *sc_container; /* back buffer of this additional swap
                                      chain (not referenced, see
                                      qd3d_res_set_sc_container) */
    D3DMULTISAMPLE_TYPE ms;      /* surfaces: multisample type */
    int nolock;                  /* render target created without Lockable,
                                    back buffer without LOCKABLE_BACKBUFFER */
    int dcs;                     /* a level has a DC out: as natively, the
                                    others neither lock nor give DCs, and
                                    unlocks succeed without effect */
    DWORD lod;                   /* managed textures: SetLOD */
    /* buffer locks nest */
    uint32_t buf_locks;
    uint32_t buf_lo, buf_hi;
    DWORD buf_flags;
    int buf_write;
    /* GetDC */
    HDC dc;
    HBITMAP dib;
    HGDIOBJ dc_old;
    void *dib_bits;
    uint32_t dib_stride;
    D3DLOCKED_RECT dc_lock;
};

static void pdata_free_all(struct qd3d_res *r)
{
    while (r->pdata) {
        struct qd3d_pdata *p = r->pdata;
        r->pdata = p->next;
        if (p->unk)
            p->unk->lpVtbl->Release(p->unk);
        free(p);
    }
}

static void res_free(struct qd3d_obj *o)
{
    struct qd3d_res *r = (struct qd3d_res *)o->ext;
    uint32_t i;

    if (r) {
        if (r->dc) {
            SelectObject(r->dc, r->dc_old);
            DeleteDC(r->dc);
            DeleteObject(r->dib);
        }
        pdata_free_all(r);
        free(r->mem_block);
        free(r->subs);
        /* Levels this container owns go with it. Bindings hold a bound
         * level's container too, so none should survive; if one did, it
         * must not reach the freed storage. */
        for (i = 0; i < r->nowned; i++) {
            struct qd3d_obj *c = r->owned[i];
            struct qd3d_res *lr = (struct qd3d_res *)c->ext;

            if (c->parent == o) {
                c->parent = NULL;
                c->owned_level = 0;
            }
            if (lr && lr->container == o)
                lr->container = NULL;
            qd3d_obj_unhold(c);
        }
        free(r->owned);
        free(r);
    }
    o->ext = NULL;
}

static uint32_t kind_of(uint32_t type)
{
    switch (type) {
    case QD3D_T_IDirect3DVertexBuffer9:
    case QD3D_T_IDirect3DIndexBuffer9:   return RES_BUFFER;
    case QD3D_T_IDirect3DSurface9:       return RES_SURFACE;
    case QD3D_T_IDirect3DVolume9:        return RES_VOLUME;
    case QD3D_T_IDirect3DTexture9:       return RES_TEXTURE;
    case QD3D_T_IDirect3DCubeTexture9:   return RES_CUBE;
    case QD3D_T_IDirect3DVolumeTexture9: return RES_VOLTEX;
    default:                             return 0;
    }
}

/* The resource state of o, created on first use (NULL on OOM or for
 * non-resource proxies). Called with the runtime lock held. */
static struct qd3d_res *res_get(struct qd3d_obj *o)
{
    struct qd3d_res *r = (struct qd3d_res *)o->ext;
    uint32_t kind = kind_of(o->type);

    if (r || !kind)
        return r;
    r = (struct qd3d_res *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->kind = kind;
    o->ext = r;
    o->ext_free = res_free;
    return r;
}

static int is_texture(const struct qd3d_res *r)
{
    return r->kind == RES_TEXTURE || r->kind == RES_CUBE || r->kind == RES_VOLTEX;
}

/* Sub-resource geometry for every (face, level), at the offsets the native
 * runtime uses in system memory: pitch is the row's bytes rounded up to 4,
 * a level takes pitch * block rows (* depth), levels follow each other, and
 * cube faces start 32-byte aligned. DEFAULT YV12/NV12 surfaces carry their
 * chroma after the luma plane. */
static int subs_init(struct qd3d_res *r)
{
    struct qd3d_fmt fi;
    uint64_t off = 0;
    int known, fits = 1;
    uint32_t f, l, i;

    r->nsubs = r->kind == RES_BUFFER ? 1u : r->faces * r->levels;
    if (!r->nsubs)
        return 0;
    r->subs = (struct qd3d_sub *)calloc(r->nsubs, sizeof(*r->subs));
    if (!r->subs) {
        r->nsubs = 0;
        return 0;
    }
    if (r->kind == RES_BUFFER) {
        struct qd3d_sub *s = &r->subs[0];
        s->w = s->row_pitch = s->slice_pitch = s->size = r->width;
        s->h = s->d = s->bx = s->by = 1;
        r->mem_size = r->width;
        return 1;
    }
    known = fmt_info(r->format, r->pool, &fi);
    for (f = 0; f < r->faces; f++) {
        for (l = 0; l < r->levels; l++) {
            struct qd3d_sub *s = &r->subs[f * r->levels + l];
            uint64_t row, slice, size;

            s->w = r->width >> l ? r->width >> l : 1u;
            s->h = r->height >> l ? r->height >> l : 1u;
            s->d = r->depth >> l ? r->depth >> l : 1u;
            if (!known)
                continue;
            s->bx = div_up(s->w, fi.bw);
            s->by = div_up(s->h, fi.bh);
            row = ((uint64_t)s->bx * fi.bytes + 3u) & ~(uint64_t)3u;
            slice = row * s->by;
            size = slice * s->d;
            if (fi.planar)
                size += row * ((s->h + 1u) / 2u);
            if (row > 0x7fffffffu || size > 0x7fffffffu || off + size > 0x7fffffffu) {
                fits = 0;
                continue;
            }
            s->off = (uint32_t)off;
            s->row_pitch = (uint32_t)row;
            s->slice_pitch = (uint32_t)slice;
            s->size = (uint32_t)size;
            off += size;
        }
        if (r->kind == RES_CUBE)
            off = (off + 31u) & ~(uint64_t)31u;
    }
    if (!fits || off > 0x7fffffffu) {
        for (i = 0; i < r->nsubs; i++)
            r->subs[i].size = 0;
        off = 0;
    }
    r->mem_size = (uint32_t)off;
    return 1;
}

/* The backing, allocated on first use (zeroed, 32-byte aligned as the
 * native runtime's). NULL when there is none (no lockable layout, OOM). */
static uint8_t *res_mem(struct qd3d_res *r)
{
    if (!r->mem && r->mem_size) {
        uint8_t *b = (uint8_t *)calloc(1, (size_t)r->mem_size + 31u);

        if (!b)
            return NULL;
        r->mem_block = b;
        r->mem = (uint8_t *)(((uintptr_t)b + 31u) & ~(uintptr_t)31u);
    }
    return r->mem;
}

static int box_empty(const struct qd3d_box *b)
{
    return b->x0 >= b->x1 || b->y0 >= b->y1 || b->z0 >= b->z1;
}

static void box_add(struct qd3d_box *a, const struct qd3d_box *b)
{
    if (box_empty(b))
        return;
    if (box_empty(a)) {
        *a = *b;
        return;
    }
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->z0 < a->z0) a->z0 = b->z0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
    if (b->z1 > a->z1) a->z1 = b->z1;
}

/* All of a sub-resource, block-rounded (buffers: every byte). */
static void box_whole(const struct qd3d_res *r, const struct qd3d_sub *s, struct qd3d_box *b)
{
    struct qd3d_fmt fi;

    memset(b, 0, sizeof(*b));
    if (r->kind == RES_BUFFER) {
        b->x1 = s->size;
        b->y1 = b->z1 = 1;
    } else if (fmt_info(r->format, r->pool, &fi)) {
        b->x1 = s->bx * fi.bw;
        b->y1 = s->by * fi.bh;
        b->z1 = s->d;
    }
}

/* Everything is waiting for an upload: a managed resource starts dirty, as
 * the native runtime's do. */
static void pend_all(struct qd3d_res *r)
{
    uint32_t i;

    for (i = 0; i < r->nsubs; i++)
        if (r->subs[i].size)
            box_whole(r, &r->subs[i], &r->subs[i].pend);
    r->pending = 1;
}

/* Native managed textures record dirty regions in level-0 coordinates and
 * count the scaled region of every level as dirty (AddDirtyRect), so a
 * write through one level's lock reaches the others too. b is in texels of
 * (face, level); NULL means the whole level. */
static void pend_scaled(struct qd3d_res *r, uint32_t face, uint32_t level, const struct qd3d_box *b)
{
    struct qd3d_fmt fi;
    uint64_t x0, y0, z0, x1, y1, z1;
    uint32_t l;
    int vol = r->kind == RES_VOLTEX;

    if (face >= r->faces || level >= r->levels || !fmt_info(r->format, r->pool, &fi))
        return;
    if (!b) {
        const struct qd3d_sub *s = &r->subs[face * r->levels + level];
        x0 = y0 = z0 = 0;
        x1 = s->w;
        y1 = s->h;
        z1 = s->d;
    } else {
        if (box_empty(b))
            return;
        x0 = b->x0;
        y0 = b->y0;
        z0 = b->z0;
        x1 = b->x1;
        y1 = b->y1;
        z1 = b->z1;
    }
    x0 <<= level;
    y0 <<= level;
    x1 <<= level;
    y1 <<= level;
    if (vol) {
        z0 <<= level;
        z1 <<= level;
    }
    for (l = 0; l < r->levels; l++) {
        struct qd3d_sub *s = &r->subs[face * r->levels + l];
        uint64_t round = ((uint64_t)1 << l) - 1u;
        struct qd3d_box nb;

        if (!s->size)
            continue;
        nb.x0 = (uint32_t)((x0 >> l) < s->w ? x0 >> l : s->w);
        nb.y0 = (uint32_t)((y0 >> l) < s->h ? y0 >> l : s->h);
        nb.x1 = (uint32_t)(((x1 + round) >> l) < s->w ? (x1 + round) >> l : s->w);
        nb.y1 = (uint32_t)(((y1 + round) >> l) < s->h ? (y1 + round) >> l : s->h);
        if (vol) {
            nb.z0 = (uint32_t)((z0 >> l) < s->d ? z0 >> l : s->d);
            nb.z1 = (uint32_t)(((z1 + round) >> l) < s->d ? (z1 + round) >> l : s->d);
        } else {
            nb.z0 = 0;
            nb.z1 = s->d;
        }
        nb.x0 -= nb.x0 % fi.bw;
        nb.y0 -= nb.y0 % fi.bh;
        nb.x1 = div_up(nb.x1, fi.bw) * fi.bw;
        nb.y1 = div_up(nb.y1, fi.bh) * fi.bh;
        box_add(&s->pend, &nb);
    }
    r->pending = 1;
}

static void res_set_desc(struct qd3d_res *r, D3DFORMAT format, DWORD usage, D3DPOOL pool,
                         uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t faces)
{
    r->format = format;
    r->usage = usage;
    r->pool = pool;
    r->width = w;
    r->height = h;
    r->depth = d;
    r->levels = levels ? levels : 1u;
    r->faces = faces;
    r->desc_ok = subs_init(r);
    if (r->desc_ok && r->dirty_all) {
        uint32_t i;
        for (i = 0; i < r->nsubs; i++)
            r->subs[i].gpu_dirty = 1;
        r->dirty_all = 0;
    }
    if (r->desc_ok && pool == D3DPOOL_MANAGED)
        pend_all(r);
}

/* Fetches the description of a proxy the host named first. */
static HRESULT res_desc(struct qd3d_obj *o, struct qd3d_res *r)
{
    HRESULT hr = D3D_OK;

    if (r->desc_ok)
        return D3D_OK;
    switch (o->type) {
    case QD3D_T_IDirect3DSurface9: {
        D3DSURFACE_DESC d;
        hr = qd3d9g_IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)o, &d);
        if (SUCCEEDED(hr)) {
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Width, d.Height, 1, 1, 1);
            r->ms = d.MultiSampleType;
        }
        break;
    }
    case QD3D_T_IDirect3DVolume9: {
        D3DVOLUME_DESC d;
        hr = qd3d9g_IDirect3DVolume9_GetDesc((IDirect3DVolume9 *)o, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Width, d.Height, d.Depth, 1, 1);
        break;
    }
    case QD3D_T_IDirect3DTexture9: {
        D3DSURFACE_DESC d;
        DWORD n = qd3d9g_IDirect3DBaseTexture9_GetLevelCount((IDirect3DBaseTexture9 *)o);
        hr = qd3d9g_IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)o, 0, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Width, d.Height, 1, n, 1);
        break;
    }
    case QD3D_T_IDirect3DCubeTexture9: {
        D3DSURFACE_DESC d;
        DWORD n = qd3d9g_IDirect3DBaseTexture9_GetLevelCount((IDirect3DBaseTexture9 *)o);
        hr = qd3d9g_IDirect3DCubeTexture9_GetLevelDesc((IDirect3DCubeTexture9 *)o, 0, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Width, d.Height, 1, n, 6);
        break;
    }
    case QD3D_T_IDirect3DVolumeTexture9: {
        D3DVOLUME_DESC d;
        DWORD n = qd3d9g_IDirect3DBaseTexture9_GetLevelCount((IDirect3DBaseTexture9 *)o);
        hr = qd3d9g_IDirect3DVolumeTexture9_GetLevelDesc((IDirect3DVolumeTexture9 *)o, 0, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Width, d.Height, d.Depth, n, 1);
        break;
    }
    case QD3D_T_IDirect3DVertexBuffer9: {
        D3DVERTEXBUFFER_DESC d;
        hr = qd3d9g_IDirect3DVertexBuffer9_GetDesc((IDirect3DVertexBuffer9 *)o, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Size, 1, 1, 1, 1);
        break;
    }
    case QD3D_T_IDirect3DIndexBuffer9: {
        D3DINDEXBUFFER_DESC d;
        hr = qd3d9g_IDirect3DIndexBuffer9_GetDesc((IDirect3DIndexBuffer9 *)o, &d);
        if (SUCCEEDED(hr))
            res_set_desc(r, d.Format, d.Usage, d.Pool, d.Size, 1, 1, 1, 1);
        break;
    }
    default:
        return D3DERR_INVALIDCALL;
    }
    if (FAILED(hr))
        return hr;
    return r->desc_ok ? D3D_OK : E_OUTOFMEMORY;
}

/* Records the creation description of a fresh proxy (best effort: without
 * memory it is fetched lazily later). */
static void res_created(void *iface, D3DFORMAT format, DWORD usage, D3DPOOL pool,
                        uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t faces)
{
    struct qd3d_obj *o = qd3d_obj(iface);
    struct qd3d_res *r;

    qd3d_rt_lock();
    r = res_get(o);
    if (r && !r->desc_ok)
        res_set_desc(r, format, usage, pool, w, h, d, levels, faces);
    qd3d_rt_unlock();
}

static uint32_t tex_levels(uint32_t requested, DWORD usage, uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t full = full_chain(w, h, d);

    if (usage & D3DUSAGE_AUTOGENMIPMAP)
        return 1;
    return (requested == 0 || requested > full) ? full : requested;
}

/* ------------------------------------------------------------------------ */
/* GPU-written marking                                                      */
/* ------------------------------------------------------------------------ */

static void mark_sub(struct qd3d_res *r, int idx)
{
    uint32_t i;

    if (!r->desc_ok) {
        r->dirty_all = 1;
        return;
    }
    /* Nothing the GPU does writes a managed or scratch resource natively
     * (such calls fail), so its guest copy stays authoritative. */
    if (r->pool == D3DPOOL_MANAGED || r->pool == D3DPOOL_SCRATCH)
        return;
    if (idx < 0) {
        for (i = 0; i < r->nsubs; i++)
            r->subs[i].gpu_dirty = 1;
    } else if ((uint32_t)idx < r->nsubs) {
        r->subs[idx].gpu_dirty = 1;
    }
}

void qd3d_res_mark_gpu(const void *iface)
{
    struct qd3d_obj *o = qd3d_obj(iface);
    struct qd3d_res *r;

    if (!o)
        return;
    qd3d_rt_lock();
    r = res_get(o);
    if (r) {
        if (r->container) {
            struct qd3d_res *c = res_get(r->container);
            if (c)
                mark_sub(c, c->desc_ok ? (int)(r->face * c->levels + r->level) : -1);
        } else {
            mark_sub(r, -1);
        }
    }
    qd3d_rt_unlock();
}

/* ------------------------------------------------------------------------ */
/* Transfers                                                                */
/* ------------------------------------------------------------------------ */

/* A lockable sub-resource and how the wire addresses it. */
struct qd3d_target {
    struct qd3d_obj *owner;      /* cmd->obj */
    struct qd3d_res *r;
    struct qd3d_sub *s;
    uint32_t kind;               /* QD3D_UPLOAD_* */
    uint32_t level, face;
};

static HRESULT resolve(struct qd3d_obj *o, uint32_t face, uint32_t level, struct qd3d_target *t)
{
    struct qd3d_res *r = res_get(o);
    HRESULT hr;

    if (!r)
        return E_OUTOFMEMORY;
    if (r->container)
        return resolve(r->container, r->face, r->level, t);
    hr = res_desc(o, r);
    if (FAILED(hr))
        return hr;
    if (level >= r->levels || face >= r->faces)
        return D3DERR_INVALIDCALL;
    t->owner = o;
    t->r = r;
    t->s = &r->subs[face * r->levels + level];
    switch (r->kind) {
    case RES_BUFFER:
        t->kind = QD3D_UPLOAD_BUFFER;
        break;
    case RES_SURFACE:
    case RES_TEXTURE:
    case RES_CUBE:
        t->kind = QD3D_UPLOAD_SURFACE;
        break;
    default:
        t->kind = QD3D_UPLOAD_VOLUME;
        break;
    }
    t->level = is_texture(r) ? level : 0u;
    t->face = r->kind == RES_CUBE ? face : 0u;
    return D3D_OK;
}

static uint8_t *sub_base(const struct qd3d_target *t)
{
    return t->r->mem + t->s->off;
}

/* One region transfer. Box in texels, block aligned and block-rounded. */
static HRESULT xfer_region(const struct qd3d_target *t, const struct qd3d_fmt *fi, int upload,
                           uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                           uint32_t flags)
{
    const struct qd3d_sub *s = t->s;
    uint8_t *base = sub_base(t);
    uint32_t row_bytes = (w / fi->bw) * fi->bytes;
    uint32_t rows = h / fi->bh;
    uint32_t slice_bytes = row_bytes * rows;
    struct qd3d_region rg;
    struct qd3d_call k;
    uint32_t zz, rr;
    HRESULT hr;

    memset(&rg, 0, sizeof(rg));
    rg.kind = t->kind;
    rg.level = t->level;
    rg.face = t->face;
    rg.lock_flags = upload ? flags : 0u;
    rg.x = x;
    rg.y = y;
    rg.z = t->kind == QD3D_UPLOAD_VOLUME ? z : 0u;
    rg.w = w;
    rg.h = h;
    rg.d = t->kind == QD3D_UPLOAD_VOLUME ? d : 1u;
    rg.row_pitch = row_bytes;
    rg.slice_pitch = t->kind == QD3D_UPLOAD_VOLUME ? slice_bytes : 0u;
    rg.data_size = slice_bytes * d;
    if (upload) {
        qd3d_call_begin(&k, QD3D_OP_UPLOAD, t->owner->id, 0, sizeof(rg) + (uint64_t)rg.data_size);
        qd3d_put_raw(&k, &rg, sizeof(rg));
        for (zz = 0; zz < d; zz++)
            for (rr = 0; rr < rows; rr++)
                qd3d_put_raw(&k, base + (size_t)(z + zz) * s->slice_pitch +
                                 (size_t)(y / fi->bh + rr) * s->row_pitch + (x / fi->bw) * fi->bytes,
                             row_bytes);
        (void)qd3d_call_end(&k);
        qd3d_call_done(&k);
        return qd3d_rt_alive() ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
    }
    qd3d_call_begin(&k, QD3D_OP_READBACK, t->owner->id, 1, sizeof(rg));
    qd3d_put_raw(&k, &rg, sizeof(rg));
    hr = (HRESULT)qd3d_call_end(&k);
    if (SUCCEEDED(hr)) {
        if (k.rleft != rg.data_size) {
            qd3d_rt_log("READBACK reply has %u bytes, expected %u", (unsigned)k.rleft,
                        (unsigned)rg.data_size);
            hr = D3DERR_DRIVERINTERNALERROR;
        } else {
            for (zz = 0; zz < d; zz++)
                for (rr = 0; rr < rows; rr++)
                    qd3d_get_raw(&k, base + (size_t)(z + zz) * s->slice_pitch +
                                     (size_t)(y / fi->bh + rr) * s->row_pitch + (x / fi->bw) * fi->bytes,
                                 row_bytes);
        }
    }
    qd3d_call_done(&k);
    return hr;
}

/* A DEFAULT YV12/NV12 surface travels whole, luma then chroma, exactly as
 * it lies in the backing (qd3d_proto.h, QD3D_UPLOAD_SURFACE). */
static HRESULT xfer_planar(const struct qd3d_target *t, int upload, uint32_t flags)
{
    const struct qd3d_sub *s = t->s;
    uint32_t max = upload ? qd3d_rt_max_upload() : qd3d_rt_max_readback();
    struct qd3d_region rg;
    struct qd3d_call k;
    HRESULT hr;

    /* The native driver does not round-trip odd-sized planar surfaces
     * through its lock (measured by the host), so they are not sent. */
    if ((s->w | s->h) & 1u)
        return D3D_OK;
    if (s->size > max) {
        qd3d_rt_log("planar surface of %u bytes exceeds the %u byte transfer limit", (unsigned)s->size,
                    (unsigned)max);
        return upload ? D3D_OK : D3DERR_INVALIDCALL;
    }
    memset(&rg, 0, sizeof(rg));
    rg.kind = QD3D_UPLOAD_SURFACE;
    rg.lock_flags = upload ? flags : 0u;
    rg.w = s->w;
    rg.h = s->h;
    rg.d = 1;
    rg.row_pitch = s->row_pitch;
    rg.data_size = s->size;
    if (upload) {
        qd3d_call_begin(&k, QD3D_OP_UPLOAD, t->owner->id, 0, sizeof(rg) + (uint64_t)rg.data_size);
        qd3d_put_raw(&k, &rg, sizeof(rg));
        qd3d_put_raw(&k, sub_base(t), s->size);
        (void)qd3d_call_end(&k);
        qd3d_call_done(&k);
        return qd3d_rt_alive() ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
    }
    qd3d_call_begin(&k, QD3D_OP_READBACK, t->owner->id, 1, sizeof(rg));
    qd3d_put_raw(&k, &rg, sizeof(rg));
    hr = (HRESULT)qd3d_call_end(&k);
    if (SUCCEEDED(hr) && (k.rleft != s->size || !qd3d_get_raw(&k, sub_base(t), s->size)))
        hr = D3DERR_DRIVERINTERNALERROR;
    qd3d_call_done(&k);
    return hr;
}

/* Transfers a box (block-rounded texels) in chunks: whole slices while a
 * slice fits, otherwise single-slice row bands with increasing y. Upload
 * continuation chunks carry exactly QD3D_UPLOAD_CONTINUE. */
static HRESULT xfer_box(const struct qd3d_target *t, int upload, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t w, uint32_t h, uint32_t d, uint32_t first_flags)
{
    struct qd3d_fmt fi;
    uint32_t max = upload ? qd3d_rt_max_upload() : qd3d_rt_max_readback();
    uint32_t row_bytes, rows, slice_bytes, flags = first_flags;
    HRESULT hr = D3D_OK;

    if (!qd3d_rt_alive())
        return D3DERR_DRIVERINTERNALERROR;
    if (!fmt_info(t->r->format, t->r->pool, &fi))
        return D3DERR_INVALIDCALL;
    if (fi.nodata)
        return D3D_OK;
    if (fi.planar)
        return xfer_planar(t, upload, first_flags);
    row_bytes = (w / fi.bw) * fi.bytes;
    rows = h / fi.bh;
    if (!row_bytes || !rows || !d)
        return D3D_OK;
    if (row_bytes > max) {
        qd3d_rt_log("row of %u bytes exceeds the %u byte transfer limit", (unsigned)row_bytes, (unsigned)max);
        return D3DERR_DRIVERINTERNALERROR;
    }
    slice_bytes = row_bytes * rows;
    if (slice_bytes <= max) {
        uint32_t per = max / slice_bytes, z0, n;
        for (z0 = 0; z0 < d && SUCCEEDED(hr); z0 += n) {
            n = d - z0 < per ? d - z0 : per;
            hr = xfer_region(t, &fi, upload, x, y, z + z0, w, h, n, flags);
            flags = QD3D_UPLOAD_CONTINUE;
        }
    } else {
        uint32_t per = max / row_bytes, zz, r0, n;
        for (zz = 0; zz < d && SUCCEEDED(hr); zz++) {
            for (r0 = 0; r0 < rows && SUCCEEDED(hr); r0 += n) {
                n = rows - r0 < per ? rows - r0 : per;
                hr = xfer_region(t, &fi, upload, x, y + r0 * fi.bh, z + zz, w, n * fi.bh, 1, flags);
                flags = QD3D_UPLOAD_CONTINUE;
            }
        }
    }
    return hr;
}

static HRESULT buffer_xfer(const struct qd3d_target *t, int upload, uint32_t off, uint32_t len,
                           uint32_t first_flags)
{
    uint32_t max = upload ? qd3d_rt_max_upload() : qd3d_rt_max_readback();
    uint32_t flags = first_flags;
    struct qd3d_region rg;
    struct qd3d_call k;
    HRESULT hr = D3D_OK;

    if (!qd3d_rt_alive())
        return D3DERR_DRIVERINTERNALERROR;
    if (!max)
        return D3DERR_DRIVERINTERNALERROR;
    while (len && SUCCEEDED(hr)) {
        uint32_t n = len < max ? len : max;

        memset(&rg, 0, sizeof(rg));
        rg.kind = QD3D_UPLOAD_BUFFER;
        rg.lock_flags = upload ? flags : 0u;
        rg.x = off;
        rg.w = n;
        rg.h = 1;
        rg.d = 1;
        rg.data_size = n;
        if (upload) {
            qd3d_call_begin(&k, QD3D_OP_UPLOAD, t->owner->id, 0, sizeof(rg) + (uint64_t)n);
            qd3d_put_raw(&k, &rg, sizeof(rg));
            qd3d_put_raw(&k, sub_base(t) + off, n);
            (void)qd3d_call_end(&k);
            qd3d_call_done(&k);
            if (!qd3d_rt_alive())
                hr = D3DERR_DRIVERINTERNALERROR;
        } else {
            qd3d_call_begin(&k, QD3D_OP_READBACK, t->owner->id, 1, sizeof(rg));
            qd3d_put_raw(&k, &rg, sizeof(rg));
            hr = (HRESULT)qd3d_call_end(&k);
            if (SUCCEEDED(hr) && (k.rleft != n || !qd3d_get_raw(&k, sub_base(t) + off, n)))
                hr = D3DERR_DRIVERINTERNALERROR;
            qd3d_call_done(&k);
        }
        flags = QD3D_UPLOAD_CONTINUE;
        off += n;
        len -= n;
    }
    return hr;
}

/* Uploads a box of a surface/volume sub-resource. A surface region needing
 * several UPLOAD chunks travels as full-width row bands. */
static HRESULT upload_box(const struct qd3d_target *t, const struct qd3d_box *b, uint32_t flags)
{
    struct qd3d_fmt fi;
    uint32_t x = b->x0, w = b->x1 - b->x0;

    if (box_empty(b))
        return D3D_OK;
    if (t->kind == QD3D_UPLOAD_SURFACE && fmt_info(t->r->format, t->r->pool, &fi) &&
        (uint64_t)(w / fi.bw) * fi.bytes * ((b->y1 - b->y0) / fi.bh) > qd3d_rt_max_upload()) {
        x = 0;
        w = t->s->bx * fi.bw;
    }
    return xfer_box(t, 1, x, b->y0, b->z0, w, b->y1 - b->y0, b->z1 - b->z0, flags);
}

static int is_dynamic(const struct qd3d_res *r)
{
    return (r->usage & D3DUSAGE_DYNAMIC) != 0;
}

static int res_locked(const struct qd3d_res *r)
{
    uint32_t i;

    for (i = 0; i < r->nsubs; i++)
        if (r->subs[i].locked)
            return 1;
    return 0;
}

/* Formats the runtime locks in blocks (compressed, packed YUV). */
static int block_format(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case D3DFMT_YUY2: case D3DFMT_UYVY: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8:
    case MAKEFOURCC('A', 'T', 'I', '1'): case MAKEFOURCC('A', 'T', 'I', '2'):
        return 1;
    default:
        return 0;
    }
}

/* Default-pool textures lock only when dynamic, or (measured: Wine's
 * test_resource_access on the stock runtime) in ATI2N. */
static int lockable(const struct qd3d_res *r)
{
    return !is_texture(r) || r->pool != D3DPOOL_DEFAULT || is_dynamic(r) ||
           r->format == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
}

/* Lock flags an upload carries to the host's own lock. System-memory
 * textures keep the dirty regions UpdateTexture copies natively, so the
 * host records them unless the application's lock asked otherwise. */
static uint32_t wire_lock_flags(const struct qd3d_res *r, DWORD flags)
{
    uint32_t w = is_dynamic(r) ? (uint32_t)(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) : 0u;

    if (r->pool == D3DPOOL_SYSTEMMEM && is_texture(r))
        w |= (uint32_t)(flags & D3DLOCK_NO_DIRTY_UPDATE);
    return w;
}

/* A buffer still mapped with writes (a draw reads it natively). */
static int buf_mapped(const struct qd3d_res *r)
{
    return r->kind == RES_BUFFER && r->buf_locks && r->buf_write && r->pool != D3DPOOL_SCRATCH;
}

/* Uploads the pending region of every sub-resource of a managed resource
 * (lock held; o owns r). Locked sub-resources keep theirs until unlocked. */
static void res_flush_locked(struct qd3d_obj *o, struct qd3d_res *r)
{
    uint32_t f, l;
    int left = 0;

    if (buf_mapped(r) && r->mem) {
        /* Drawn while mapped: the draw sees the writes so far. Later
         * uploads of this mapping must not discard what it drew from. */
        struct qd3d_target t;

        if (SUCCEEDED(resolve(o, 0, 0, &t)) &&
            SUCCEEDED(buffer_xfer(&t, 1, r->buf_lo, r->buf_hi - r->buf_lo, wire_lock_flags(r, r->buf_flags))) &&
            is_dynamic(r))
            r->buf_flags = (r->buf_flags & ~(DWORD)D3DLOCK_DISCARD) | D3DLOCK_NOOVERWRITE;
        qd3d_dev_note_flush(o->device);
    }
    if (!r->pending || !r->mem)
        return;
    for (f = 0; f < (r->kind == RES_BUFFER ? 1u : r->faces); f++) {
        for (l = 0; l < (r->kind == RES_BUFFER ? 1u : r->levels); l++) {
            struct qd3d_target t;
            struct qd3d_sub *s;
            HRESULT hr;

            if (FAILED(resolve(o, f, l, &t)))
                continue;
            s = t.s;
            if (box_empty(&s->pend))
                continue;
            if (s->locked) {
                left = 1;
                continue;
            }
            if (t.kind == QD3D_UPLOAD_BUFFER)
                hr = buffer_xfer(&t, 1, s->pend.x0, s->pend.x1 - s->pend.x0, 0);
            else
                hr = upload_box(&t, &s->pend, 0);
            if (FAILED(hr))
                qd3d_rt_log("managed upload of %#x (level %u face %u) failed (%#lx)", (unsigned)o->id,
                            (unsigned)l, (unsigned)f, (unsigned long)hr);
            memset(&s->pend, 0, sizeof(s->pend));
        }
    }
    r->pending = left;
}

void qd3d_res_flush(const void *iface)
{
    struct qd3d_obj *o = qd3d_obj(iface);
    struct qd3d_res *r;

    if (!o || !qd3d_res_has_pending(iface))
        return;
    qd3d_rt_lock();
    r = res_get(o);
    if (r && r->container) {
        o = r->container;
        r = res_get(o);
    }
    if (r)
        res_flush_locked(o, r);
    qd3d_rt_unlock();
}

int qd3d_res_has_pending(const void *iface)
{
    const struct qd3d_obj *o = (const struct qd3d_obj *)iface;
    const struct qd3d_res *r = o && kind_of(o->type) ? (const struct qd3d_res *)o->ext : NULL;

    if (r && r->container)
        r = (const struct qd3d_res *)r->container->ext;
    return r && r->mem && (r->pending || buf_mapped(r));
}

/* ------------------------------------------------------------------------ */
/* Surface/volume/texture locks                                             */
/* ------------------------------------------------------------------------ */

static HRESULT lock_box(struct qd3d_obj *o, uint32_t face, uint32_t level, const D3DBOX *box,
                        DWORD flags, D3DLOCKED_BOX *out)
{
    struct qd3d_target t;
    struct qd3d_fmt fi;
    struct qd3d_sub *s;
    uint32_t l, tp, f, rt, bt, bk;
    uint8_t *mem;
    HRESULT hr;

    hr = resolve(o, face, level, &t);
    if (FAILED(hr))
        return hr;
    s = t.s;
    if (!fmt_info(t.r->format, t.r->pool, &fi) || !s->size)
        return D3DERR_INVALIDCALL;
    if (!lockable(t.r))
        return D3DERR_INVALIDCALL;
    if (s->locked || t.r->nolock || t.r->dcs)
        return D3DERR_INVALIDCALL;
    mem = res_mem(t.r);
    if (!mem)
        return E_OUTOFMEMORY;
    if (box) {
        l = box->Left;
        tp = box->Top;
        f = box->Front;
        rt = box->Right;
        bt = box->Bottom;
        bk = box->Back;
        if (fi.nodata)
            return D3DERR_INVALIDCALL;
        if (l >= rt || tp >= bt || f >= bk || rt > s->w || bt > s->h || bk > s->d || l % fi.bw || tp % fi.bh ||
            (rt % fi.bw && rt != s->w) || (bt % fi.bh && bt != s->h)) {
            /* The native runtime checks rectangles only for volumes and for
             * block formats in the default pool (Wine's lockrect and
             * surface/volume-block tests on the stock runtime): elsewhere
             * the whole level locks and the pointer follows the rectangle. */
            if (t.r->kind == RES_VOLTEX || (block_format(t.r->format) && t.r->pool == D3DPOOL_DEFAULT))
                return D3DERR_INVALIDCALL;
            hr = lock_box(o, face, level, NULL, flags, out);
            if (SUCCEEDED(hr))
                out->pBits = (uint8_t *)out->pBits + (ptrdiff_t)(LONG)box->Front * out->SlicePitch +
                             (ptrdiff_t)((LONG)box->Top / (LONG)fi.bh) * out->RowPitch +
                             (ptrdiff_t)((LONG)box->Left / (LONG)fi.bw) * (ptrdiff_t)fi.bytes;
            return hr;
        }
    } else {
        l = tp = f = 0;
        rt = s->w;
        bt = s->h;
        bk = s->d;
    }
    rt = div_up(rt, fi.bw) * fi.bw;
    bt = div_up(bt, fi.bh) * fi.bh;
    if (flags & D3DLOCK_DISCARD) {
        s->gpu_dirty = 0;
    } else if (s->gpu_dirty) {
        hr = xfer_box(&t, 0, 0, 0, 0, s->bx * fi.bw, s->by * fi.bh, s->d, 0);
        if (FAILED(hr))
            return hr;
        s->gpu_dirty = 0;
    } else if (t.r->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        /* Rendered continuously: always read the locked rows, full width
         * so a multi-chunk upload of full-width bands stays truthful. */
        hr = fi.planar ? xfer_box(&t, 0, 0, 0, 0, s->w, s->h, 1, 0)
                       : xfer_box(&t, 0, 0, tp, f, s->bx * fi.bw, bt - tp, bk - f, 0);
        if (FAILED(hr))
            return hr;
    }
    s->locked = 1;
    s->lock_flags = flags;
    s->lk.x0 = l;
    s->lk.y0 = tp;
    s->lk.z0 = f;
    s->lk.x1 = rt;
    s->lk.y1 = bt;
    s->lk.z1 = bk;
    out->RowPitch = (INT)s->row_pitch;
    out->SlicePitch = (INT)s->slice_pitch;
    out->pBits = mem + s->off + (size_t)f * s->slice_pitch + (size_t)(tp / fi.bh) * s->row_pitch +
                 (size_t)(l / fi.bw) * fi.bytes;
    return D3D_OK;
}

/* Ends a lock. Managed resources record the native dirty region and upload
 * it before their next use; scratch resources never reach the host; the
 * others upload the locked region now. */
static HRESULT unlock_box(struct qd3d_obj *o, uint32_t face, uint32_t level)
{
    struct qd3d_target t;
    struct qd3d_sub *s;
    HRESULT hr;

    hr = resolve(o, face, level, &t);
    if (FAILED(hr))
        return hr;
    s = t.s;
    if (t.r->dcs)
        return D3D_OK;
    if (!s->locked) {
        /* As natively (Wine's lock tests on the stock runtime): unlocking an
         * unlocked level of a lockable 2D texture (through the texture or
         * its level surface) or cube texture (through the texture)
         * succeeds; volumes, cube faces and plain surfaces fail. */
        if (lockable(t.r) && (o->type == QD3D_T_IDirect3DTexture9 || o->type == QD3D_T_IDirect3DCubeTexture9 ||
                              (o->type == QD3D_T_IDirect3DSurface9 && t.r->kind == RES_TEXTURE)))
            return D3D_OK;
        return D3DERR_INVALIDCALL;
    }
    s->locked = 0;
    switch (t.r->pool) {
    case D3DPOOL_SCRATCH:
        return D3D_OK;
    case D3DPOOL_MANAGED:
        if (!(s->lock_flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)))
            pend_scaled(t.r, t.face, t.level, &s->lk);
        if (t.r->pending)
            qd3d_dev_note_flush(t.owner->device);
        return D3D_OK;
    default:
        if (s->lock_flags & D3DLOCK_READONLY)
            return D3D_OK;
        return upload_box(&t, &s->lk, wire_lock_flags(t.r, s->lock_flags));
    }
}

/* A failed lock clears the caller's description, as natively, except when
 * the sub-resource was already locked (then it is left alone). Lock held. */
static int sub_is_locked(struct qd3d_obj *o, uint32_t face, uint32_t level)
{
    struct qd3d_target t;

    return SUCCEEDED(resolve(o, face, level, &t)) && t.s->locked;
}

static HRESULT lock_rect(struct qd3d_obj *o, uint32_t face, uint32_t level, D3DLOCKED_RECT *out,
                         const RECT *rc, DWORD flags)
{
    D3DLOCKED_BOX lb;
    D3DBOX b, *pb = NULL;
    HRESULT hr = D3DERR_INVALIDCALL;
    int keep = 0;

    if (!out)
        return D3DERR_INVALIDCALL;
    if (rc) {
        b.Left = (UINT)rc->left;
        b.Top = (UINT)rc->top;
        b.Right = (UINT)rc->right;
        b.Bottom = (UINT)rc->bottom;
        b.Front = 0;
        b.Back = 1;
        pb = &b;
    }
    qd3d_rt_lock();
    /* Negative coordinates become out-of-range box values, which lock_box
     * rejects or (where the native runtime does not check) accepts. */
    hr = lock_box(o, face, level, pb, flags, &lb);
    if (FAILED(hr))
        keep = sub_is_locked(o, face, level);
    qd3d_rt_unlock();
    if (SUCCEEDED(hr)) {
        out->pBits = lb.pBits;
        out->Pitch = lb.RowPitch;
    } else if (!keep) {
        out->pBits = NULL;
        out->Pitch = 0;
    }
    return hr;
}

static HRESULT lock_volume(struct qd3d_obj *o, uint32_t level, D3DLOCKED_BOX *out, const D3DBOX *box, DWORD flags)
{
    HRESULT hr;

    if (!out)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    hr = lock_box(o, 0, level, box, flags, out);
    if (FAILED(hr) && !sub_is_locked(o, 0, level)) {
        out->pBits = NULL;
        out->RowPitch = out->SlicePitch = 0;
    }
    qd3d_rt_unlock();
    return hr;
}

static HRESULT unlock_locked(struct qd3d_obj *o, uint32_t face, uint32_t level)
{
    HRESULT hr;

    qd3d_rt_lock();
    hr = unlock_box(o, face, level);
    qd3d_rt_unlock();
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DTexture9_LockRect(IDirect3DTexture9 *This, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    return lock_rect(qd3d_obj(This), 0, Level, pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DTexture9_UnlockRect(IDirect3DTexture9 *This, UINT Level)
{
    return unlock_locked(qd3d_obj(This), 0, Level);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DCubeTexture9_LockRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    return lock_rect(qd3d_obj(This), (uint32_t)FaceType, Level, pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DCubeTexture9_UnlockRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES FaceType, UINT Level)
{
    return unlock_locked(qd3d_obj(This), (uint32_t)FaceType, Level);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolumeTexture9_LockBox(IDirect3DVolumeTexture9 *This, UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
    return lock_volume(qd3d_obj(This), Level, pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolumeTexture9_UnlockBox(IDirect3DVolumeTexture9 *This, UINT Level)
{
    return unlock_locked(qd3d_obj(This), 0, Level);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_LockRect(IDirect3DSurface9 *This, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    struct qd3d_res *r;

    qd3d_rt_lock();
    r = res_get(qd3d_obj(This));
    if (r && r->dc) {
        qd3d_rt_unlock();
        return D3DERR_INVALIDCALL; /* the DC holds the lock */
    }
    qd3d_rt_unlock();
    return lock_rect(qd3d_obj(This), 0, 0, pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_UnlockRect(IDirect3DSurface9 *This)
{
    struct qd3d_res *r;

    qd3d_rt_lock();
    r = res_get(qd3d_obj(This));
    if (r && r->dc) {
        qd3d_rt_unlock();
        return D3DERR_INVALIDCALL;
    }
    qd3d_rt_unlock();
    return unlock_locked(qd3d_obj(This), 0, 0);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_LockBox(IDirect3DVolume9 *This, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
    return lock_volume(qd3d_obj(This), 0, pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_UnlockBox(IDirect3DVolume9 *This)
{
    return unlock_locked(qd3d_obj(This), 0, 0);
}

/* ------------------------------------------------------------------------ */
/* Buffer locks                                                             */
/* ------------------------------------------------------------------------ */

static HRESULT buffer_lock(struct qd3d_obj *o, UINT off, UINT size, void **pp, DWORD flags)
{
    struct qd3d_target t;
    struct qd3d_sub *s;
    uint8_t *mem;
    HRESULT hr;

    if (!pp)
        return D3DERR_INVALIDCALL;
    *pp = NULL;
    qd3d_rt_lock();
    hr = resolve(o, 0, 0, &t);
    if (FAILED(hr))
        goto out;
    s = t.s;
    hr = D3DERR_INVALIDCALL;
    if (off > s->size)
        goto out;
    if (size == 0)
        size = s->size - off;
    if (size > s->size - off)
        goto out;
    hr = E_OUTOFMEMORY;
    mem = res_mem(t.r);
    if (!mem && s->size)
        goto out;
    if (!t.r->buf_locks) {
        if (flags & D3DLOCK_DISCARD) {
            s->gpu_dirty = 0;
        } else if (s->gpu_dirty) {
            hr = buffer_xfer(&t, 0, 0, s->size, 0);
            if (FAILED(hr))
                goto out;
            s->gpu_dirty = 0;
        }
        t.r->buf_lo = off;
        t.r->buf_hi = off + size;
        t.r->buf_flags = flags;
        t.r->buf_write = 0;
    } else {
        if (off < t.r->buf_lo)
            t.r->buf_lo = off;
        if (off + size > t.r->buf_hi)
            t.r->buf_hi = off + size;
    }
    if (!(flags & D3DLOCK_READONLY)) {
        t.r->buf_write = 1;
        if (t.r->pool != D3DPOOL_SCRATCH)
            qd3d_dev_note_flush(t.owner->device); /* a draw may read it mapped */
    }
    t.r->buf_locks++;
    *pp = mem ? mem + off : (void *)s; /* zero-length buffer: any non-NULL */
    hr = D3D_OK;
out:
    qd3d_rt_unlock();
    return hr;
}

static HRESULT buffer_unlock(struct qd3d_obj *o)
{
    struct qd3d_target t;
    HRESULT hr;

    qd3d_rt_lock();
    hr = resolve(o, 0, 0, &t);
    if (SUCCEEDED(hr) && !t.r->buf_locks)
        hr = D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && --t.r->buf_locks == 0 && t.r->buf_write) {
        if (t.r->pool == D3DPOOL_MANAGED) {
            struct qd3d_box b = {t.r->buf_lo, 0, 0, t.r->buf_hi, 1, 1};

            box_add(&t.s->pend, &b);
            t.r->pending = 1;
        } else if (t.r->pool != D3DPOOL_SCRATCH) {
            hr = buffer_xfer(&t, 1, t.r->buf_lo, t.r->buf_hi - t.r->buf_lo,
                             wire_lock_flags(t.r, t.r->buf_flags));
        }
    }
    if (SUCCEEDED(hr) && !t.r->buf_locks && t.r->pending)
        qd3d_dev_note_flush(t.owner->device);
    qd3d_rt_unlock();
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexBuffer9_Lock(IDirect3DVertexBuffer9 *This, UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags)
{
    return buffer_lock(qd3d_obj(This), OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexBuffer9_Unlock(IDirect3DVertexBuffer9 *This)
{
    return buffer_unlock(qd3d_obj(This));
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DIndexBuffer9_Lock(IDirect3DIndexBuffer9 *This, UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags)
{
    return buffer_lock(qd3d_obj(This), OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DIndexBuffer9_Unlock(IDirect3DIndexBuffer9 *This)
{
    return buffer_unlock(qd3d_obj(This));
}

/* ------------------------------------------------------------------------ */
/* GetDC / ReleaseDC                                                        */
/* ------------------------------------------------------------------------ */

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_GetDC(IDirect3DSurface9 *This, HDC *phdc)
{
    struct qd3d_obj *o = qd3d_obj(This);
    struct {
        BITMAPINFOHEADER h;
        DWORD masks[3];
    } bi;
    struct qd3d_res *r;
    struct qd3d_target t;
    D3DLOCKED_RECT lr;
    uint32_t bpp, w, h, y, row;
    void *bits = NULL;
    HBITMAP dib;
    HDC dc;
    HRESULT hr;

    if (!phdc)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    r = res_get(o);
    hr = r ? resolve(o, 0, 0, &t) : E_OUTOFMEMORY;
    if (FAILED(hr) || t.r->dcs || res_locked(t.r)) {
        qd3d_rt_unlock();
        return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
    }
    memset(&bi, 0, sizeof(bi));
    switch (t.r->format) {
    case D3DFMT_R5G6B5:
        bpp = 16;
        bi.masks[0] = 0xf800;
        bi.masks[1] = 0x07e0;
        bi.masks[2] = 0x001f;
        break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        bpp = 16;
        bi.masks[0] = 0x7c00;
        bi.masks[1] = 0x03e0;
        bi.masks[2] = 0x001f;
        break;
    case D3DFMT_R8G8B8:
        bpp = 24;
        break;
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
        bpp = 32;
        break;
    default:
        qd3d_rt_unlock();
        return D3DERR_INVALIDCALL;
    }
    w = t.s->w;
    h = t.s->h;
    hr = lock_rect(o, 0, 0, &lr, NULL, 0);
    if (FAILED(hr)) {
        qd3d_rt_unlock();
        return hr;
    }
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = (LONG)w;
    bi.h.biHeight = -(LONG)h;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = (WORD)bpp;
    bi.h.biCompression = bpp == 16 ? BI_BITFIELDS : BI_RGB;
    dib = CreateDIBSection(NULL, (const BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    dc = dib ? CreateCompatibleDC(NULL) : NULL;
    if (!dib || !dc || !bits) {
        if (dc)
            DeleteDC(dc);
        if (dib)
            DeleteObject(dib);
        unlock_box(o, 0, 0);
        qd3d_rt_unlock();
        return E_OUTOFMEMORY;
    }
    row = ((w * bpp + 31u) / 32u) * 4u;
    for (y = 0; y < h; y++)
        memcpy((uint8_t *)bits + (size_t)y * row, (uint8_t *)lr.pBits + (size_t)y * (uint32_t)lr.Pitch,
               (size_t)w * (bpp / 8u));
    r->dc_old = SelectObject(dc, dib);
    r->dc = dc;
    r->dib = dib;
    r->dib_bits = bits;
    r->dib_stride = row;
    r->dc_lock = lr;
    t.r->dcs = 1;
    *phdc = dc;
    qd3d_rt_unlock();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_ReleaseDC(IDirect3DSurface9 *This, HDC hdc)
{
    struct qd3d_obj *o = qd3d_obj(This);
    struct qd3d_target t;
    struct qd3d_res *r;
    uint32_t y, w, h, bytes;
    HRESULT hr;

    qd3d_rt_lock();
    r = res_get(o);
    if (!r || !r->dc || r->dc != hdc || FAILED(resolve(o, 0, 0, &t))) {
        qd3d_rt_unlock();
        return D3DERR_INVALIDCALL;
    }
    GdiFlush();
    w = t.s->w;
    h = t.s->h;
    bytes = t.r->format == D3DFMT_R8G8B8 ? 3u
            : (t.r->format == D3DFMT_R5G6B5 || t.r->format == D3DFMT_X1R5G5B5 || t.r->format == D3DFMT_A1R5G5B5) ? 2u
            : 4u;
    for (y = 0; y < h; y++)
        memcpy((uint8_t *)r->dc_lock.pBits + (size_t)y * (uint32_t)r->dc_lock.Pitch,
               (uint8_t *)r->dib_bits + (size_t)y * r->dib_stride, (size_t)w * bytes);
    SelectObject(r->dc, r->dc_old);
    DeleteDC(r->dc);
    DeleteObject(r->dib);
    r->dc = NULL;
    r->dib = NULL;
    r->dib_bits = NULL;
    t.r->dcs = 0;
    hr = unlock_box(o, 0, 0);
    qd3d_rt_unlock();
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Private data, priority, type, device, container                          */
/* ------------------------------------------------------------------------ */

static HRESULT set_private(struct qd3d_obj *o, REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    struct qd3d_pdata **pp, *p;
    struct qd3d_res *r;

    if (!guid || (!data && size))
        return D3DERR_INVALIDCALL;
    if ((flags & D3DSPD_IUNKNOWN) && (size != sizeof(IUnknown *) || !data))
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    r = res_get(o);
    if (!r) {
        qd3d_rt_unlock();
        return E_OUTOFMEMORY;
    }
    p = (struct qd3d_pdata *)calloc(1, sizeof(*p) + ((flags & D3DSPD_IUNKNOWN) ? 0u : size));
    if (!p) {
        qd3d_rt_unlock();
        return E_OUTOFMEMORY;
    }
    p->guid = *guid;
    p->flags = flags & D3DSPD_IUNKNOWN;
    p->size = size;
    if (flags & D3DSPD_IUNKNOWN) {
        /* pData is the interface pointer itself (measured on the stock
         * runtime; passing its address crashes natively). */
        p->unk = (IUnknown *)data;
        if (p->unk)
            p->unk->lpVtbl->AddRef(p->unk);
    } else if (size) {
        memcpy(p->data, data, size);
    }
    for (pp = &r->pdata; *pp; pp = &(*pp)->next) {
        if (qd3d_iid_equal(&(*pp)->guid, guid)) {
            struct qd3d_pdata *old = *pp;
            *pp = old->next;
            if (old->unk)
                old->unk->lpVtbl->Release(old->unk);
            free(old);
            break;
        }
    }
    p->next = r->pdata;
    r->pdata = p;
    qd3d_rt_unlock();
    return D3D_OK;
}

static HRESULT get_private(struct qd3d_obj *o, REFGUID guid, void *data, DWORD *size)
{
    struct qd3d_pdata *p = NULL;
    struct qd3d_res *r;
    HRESULT hr;

    if (!guid)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    r = res_get(o);
    if (r)
        for (p = r->pdata; p && !qd3d_iid_equal(&p->guid, guid); p = p->next)
            ;
    if (!p) {
        hr = D3DERR_NOTFOUND; /* natively even without a size */
    } else if (!size) {
        hr = D3DERR_INVALIDCALL;
    } else if (!data) {
        *size = p->size;
        hr = D3D_OK;
    } else if (*size < p->size) {
        *size = p->size;
        hr = D3DERR_MOREDATA;
    } else {
        if (p->flags & D3DSPD_IUNKNOWN) {
            if (p->unk)
                p->unk->lpVtbl->AddRef(p->unk);
            memcpy(data, &p->unk, sizeof(p->unk));
        } else {
            memcpy(data, p->data, p->size);
        }
        *size = p->size;
        hr = D3D_OK;
    }
    qd3d_rt_unlock();
    return hr;
}

static HRESULT free_private(struct qd3d_obj *o, REFGUID guid)
{
    struct qd3d_pdata **pp;
    struct qd3d_res *r;
    HRESULT hr = D3DERR_NOTFOUND;

    if (!guid)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    r = res_get(o);
    if (r) {
        for (pp = &r->pdata; *pp; pp = &(*pp)->next) {
            if (qd3d_iid_equal(&(*pp)->guid, guid)) {
                struct qd3d_pdata *old = *pp;
                *pp = old->next;
                if (old->unk)
                    old->unk->lpVtbl->Release(old->unk);
                free(old);
                hr = D3D_OK;
                break;
            }
        }
    }
    qd3d_rt_unlock();
    return hr;
}

static HRESULT get_device(const void *iface, IDirect3DDevice9 **pp)
{
    struct qd3d_obj *d = qd3d_obj_device(iface);

    if (!pp)
        return D3DERR_INVALIDCALL;
    *pp = NULL;
    if (!d)
        return D3DERR_INVALIDCALL;
    qd3d_obj_addref(d);
    *pp = (IDirect3DDevice9 *)d;
    return D3D_OK;
}

/* A level's container is its texture, a back buffer's its swap chain, any
 * other surface's the device. */
static HRESULT get_container(struct qd3d_obj *o, REFIID riid, void **pp)
{
    struct qd3d_obj *c;
    struct qd3d_res *r;
    uint32_t swapchain;
    HRESULT hr;

    if (!pp)
        return D3DERR_INVALIDCALL;
    *pp = NULL;
    qd3d_rt_lock();
    r = res_get(o);
    swapchain = r && !r->container && !r->sc_container ? r->swapchain : 0u;
    c = r && r->container ? r->container : r && r->sc_container ? r->sc_container : o->device;
    if (c)
        qd3d_obj_addref(c);
    qd3d_rt_unlock();
    if (!c)
        return E_NOINTERFACE;
    if (swapchain) {
        IDirect3DSwapChain9 *sc = NULL;

        hr = qd3d9o_IDirect3DDevice9_GetSwapChain((IDirect3DDevice9 *)c, swapchain - 1u, &sc);
        qd3d_obj_release(c);
        if (FAILED(hr) || !sc)
            return E_NOINTERFACE;
        c = qd3d_obj(sc);
    }
    hr = qd3d_query_interface(c, riid, pp);
    qd3d_obj_release(c);
    return hr;
}

void qd3d_res_set_sc_container(const void *surface, struct qd3d_obj *sc)
{
    struct qd3d_res *r;

    if (!surface)
        return;
    qd3d_rt_lock();
    r = res_get(qd3d_obj(surface));
    if (r && !r->container)
        r->sc_container = sc;
    qd3d_rt_unlock();
}

void qd3d_res_set_swapchain(const void *surface, uint32_t index, int lockable)
{
    struct qd3d_res *r;

    if (!surface)
        return;
    qd3d_rt_lock();
    r = res_get(qd3d_obj(surface));
    if (r && !r->container) {
        r->swapchain = index + 1u;
        if (lockable >= 0)
            r->nolock = !lockable;
    }
    qd3d_rt_unlock();
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_GetDevice(IDirect3DResource9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_SetPrivateData(IDirect3DResource9 *This, REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
    return set_private(qd3d_obj(This), refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_GetPrivateData(IDirect3DResource9 *This, REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
    return get_private(qd3d_obj(This), refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_FreePrivateData(IDirect3DResource9 *This, REFGUID refguid)
{
    return free_private(qd3d_obj(This), refguid);
}

/* Natively only managed textures and buffers keep a priority (Wine's
 * test_resource_priority on the stock runtime); surfaces never do. */
static int keeps_priority(struct qd3d_obj *o, struct qd3d_res *r)
{
    return o->type != QD3D_T_IDirect3DSurface9 && SUCCEEDED(res_desc(o, r)) && r->pool == D3DPOOL_MANAGED;
}

DWORD STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_SetPriority(IDirect3DResource9 *This, DWORD PriorityNew)
{
    struct qd3d_obj *o = qd3d_obj(This);
    struct qd3d_res *r;
    DWORD old = 0;

    qd3d_rt_lock();
    r = res_get(o);
    if (r && keeps_priority(o, r)) {
        old = r->priority;
        r->priority = PriorityNew;
    }
    qd3d_rt_unlock();
    return old;
}

DWORD STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_GetPriority(IDirect3DResource9 *This)
{
    struct qd3d_res *r;
    DWORD v = 0;

    qd3d_rt_lock();
    r = res_get(qd3d_obj(This));
    if (r)
        v = r->priority;
    qd3d_rt_unlock();
    return v;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DBaseTexture9_SetAutoGenFilterType(IDirect3DBaseTexture9 *This, D3DTEXTUREFILTERTYPE FilterType)
{
    if (FilterType == D3DTEXF_NONE)
        return D3DERR_INVALIDCALL;
    return qd3d9g_IDirect3DBaseTexture9_SetAutoGenFilterType(This, FilterType);
}

/* Natively only managed textures keep a LOD, clamped to the last level;
 * SetLOD returns the previous one (measured on the stock runtime). The
 * host learns it too, for its own sampling of the texture. */
DWORD STDMETHODCALLTYPE qd3d9o_IDirect3DBaseTexture9_SetLOD(IDirect3DBaseTexture9 *This, DWORD LODNew)
{
    struct qd3d_obj *o = qd3d_obj(This);
    struct qd3d_res *r;
    DWORD old = 0, lod = 0;
    int managed = 0;

    qd3d_rt_lock();
    r = res_get(o);
    if (r && SUCCEEDED(res_desc(o, r)) && r->pool == D3DPOOL_MANAGED) {
        managed = 1;
        old = r->lod;
        lod = r->levels && LODNew >= r->levels ? r->levels - 1 : LODNew;
        r->lod = lod;
    }
    qd3d_rt_unlock();
    if (managed && lod != old)
        qd3d9g_IDirect3DBaseTexture9_SetLOD(This, lod);
    return old;
}

DWORD STDMETHODCALLTYPE qd3d9o_IDirect3DBaseTexture9_GetLOD(IDirect3DBaseTexture9 *This)
{
    struct qd3d_res *r;
    DWORD v = 0;

    qd3d_rt_lock();
    r = res_get(qd3d_obj(This));
    if (r)
        v = r->lod;
    qd3d_rt_unlock();
    return v;
}

D3DRESOURCETYPE STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_GetType(IDirect3DResource9 *This)
{
    switch (qd3d_obj(This)->type) {
    case QD3D_T_IDirect3DSurface9:       return D3DRTYPE_SURFACE;
    case QD3D_T_IDirect3DTexture9:       return D3DRTYPE_TEXTURE;
    case QD3D_T_IDirect3DVolumeTexture9: return D3DRTYPE_VOLUMETEXTURE;
    case QD3D_T_IDirect3DCubeTexture9:   return D3DRTYPE_CUBETEXTURE;
    case QD3D_T_IDirect3DVertexBuffer9:  return D3DRTYPE_VERTEXBUFFER;
    case QD3D_T_IDirect3DIndexBuffer9:   return D3DRTYPE_INDEXBUFFER;
    case QD3D_T_IDirect3DVolume9:        return D3DRTYPE_VOLUME;
    default:                             return (D3DRESOURCETYPE)0;
    }
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_GetContainer(IDirect3DSurface9 *This, REFIID riid, void **ppContainer)
{
    return get_container(qd3d_obj(This), riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_GetDevice(IDirect3DVolume9 *This, IDirect3DDevice9 **ppDevice)
{
    return get_device(This, ppDevice);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_SetPrivateData(IDirect3DVolume9 *This, REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
    return set_private(qd3d_obj(This), refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_GetPrivateData(IDirect3DVolume9 *This, REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
    return get_private(qd3d_obj(This), refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_FreePrivateData(IDirect3DVolume9 *This, REFGUID refguid)
{
    return free_private(qd3d_obj(This), refguid);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_GetContainer(IDirect3DVolume9 *This, REFIID riid, void **ppContainer)
{
    return get_container(qd3d_obj(This), riid, ppContainer);
}

/* ------------------------------------------------------------------------ */
/* Creation overrides                                                       */
/* ------------------------------------------------------------------------ */

static int depth_format(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_D16_LOCKABLE: case D3DFMT_D32: case D3DFMT_D15S1: case D3DFMT_D24S8: case D3DFMT_D24X8:
    case D3DFMT_D24X4S4: case D3DFMT_D16: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8:
        return 1;
    default:
        return 0;
    }
}

/* Natively a non-Ex device shares nothing (E_NOTIMPL as on XP; Wine's
 * test_shared_handle on the stock runtime); dynamic usage needs a pool the
 * runtime renames, so managed and scratch dynamic resources fail; and
 * textures outside the default pool are neither render targets nor depth
 * stencils nor of depth formats (measured on the stock runtime and by
 * Wine's test_resource_access there). */
static HRESULT create_check(DWORD usage, D3DPOOL pool, D3DFORMAT format, int texture, const HANDLE *shared,
                            void *pp)
{
    HRESULT hr = D3D_OK;

    if (shared)
        hr = E_NOTIMPL;
    else if (((usage & D3DUSAGE_DYNAMIC) && (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SCRATCH)) ||
             (texture && pool != D3DPOOL_DEFAULT &&
              ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || depth_format(format))))
        hr = D3DERR_INVALIDCALL;
    if (FAILED(hr) && pp)
        *(void **)pp = NULL;
    return hr;
}

/* Surface metadata the device-level checks need (lock held inside). */
static void surf_created(void *iface, D3DMULTISAMPLE_TYPE ms, int nolock)
{
    struct qd3d_res *r;

    qd3d_rt_lock();
    r = res_get(qd3d_obj(iface));
    if (r) {
        r->ms = ms;
        r->nolock = nolock;
    }
    qd3d_rt_unlock();
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateTexture(IDirect3DDevice9 *This, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
{
    HRESULT hr;

    if (FAILED(hr = create_check(Usage, Pool, Format, 1, pSharedHandle, ppTexture)))
        return hr;
    hr = qd3d9g_IDirect3DDevice9_CreateTexture(This, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
    if (SUCCEEDED(hr) && ppTexture && *ppTexture)
        res_created(*ppTexture, Format, Usage, Pool, Width, Height, 1,
                    tex_levels(Levels, Usage, Width, Height, 1), 1);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateVolumeTexture(IDirect3DDevice9 *This, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle)
{
    HRESULT hr;

    if (FAILED(hr = create_check(Usage, Pool, Format, 1, pSharedHandle, ppVolumeTexture)))
        return hr;
    hr = qd3d9g_IDirect3DDevice9_CreateVolumeTexture(This, Width, Height, Depth, Levels, Usage, Format, Pool,
                                                     ppVolumeTexture, pSharedHandle);
    if (SUCCEEDED(hr) && ppVolumeTexture && *ppVolumeTexture)
        res_created(*ppVolumeTexture, Format, Usage, Pool, Width, Height, Depth,
                    tex_levels(Levels, Usage, Width, Height, Depth), 1);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateCubeTexture(IDirect3DDevice9 *This, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle)
{
    HRESULT hr;

    if (FAILED(hr = create_check(Usage, Pool, Format, 1, pSharedHandle, ppCubeTexture)))
        return hr;
    hr = qd3d9g_IDirect3DDevice9_CreateCubeTexture(This, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture,
                                                   pSharedHandle);
    if (SUCCEEDED(hr) && ppCubeTexture && *ppCubeTexture)
        res_created(*ppCubeTexture, Format, Usage, Pool, EdgeLength, EdgeLength, 1,
                    tex_levels(Levels, Usage, EdgeLength, EdgeLength, 1), 6);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateVertexBuffer(IDirect3DDevice9 *This, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle)
{
    HRESULT hr;

    if (FAILED(hr = create_check(Usage, Pool, D3DFMT_VERTEXDATA, 0, pSharedHandle, ppVertexBuffer)))
        return hr;
    hr = qd3d9g_IDirect3DDevice9_CreateVertexBuffer(This, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    if (SUCCEEDED(hr) && ppVertexBuffer && *ppVertexBuffer)
        res_created(*ppVertexBuffer, D3DFMT_VERTEXDATA, Usage, Pool, Length, 1, 1, 1, 1);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateIndexBuffer(IDirect3DDevice9 *This, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle)
{
    HRESULT hr;

    if (FAILED(hr = create_check(Usage, Pool, Format, 0, pSharedHandle, ppIndexBuffer)))
        return hr;
    hr = qd3d9g_IDirect3DDevice9_CreateIndexBuffer(This, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    if (SUCCEEDED(hr) && ppIndexBuffer && *ppIndexBuffer)
        res_created(*ppIndexBuffer, Format, Usage, Pool, Length, 1, 1, 1, 1);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateRenderTarget(IDirect3DDevice9 *This, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    HRESULT hr = create_check(0, D3DPOOL_DEFAULT, Format, 0, pSharedHandle, ppSurface);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_CreateRenderTarget(This, Width, Height, Format, MultiSample, MultisampleQuality,
                                                        Lockable, ppSurface, pSharedHandle);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        res_created(*ppSurface, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, Width, Height, 1, 1, 1);
        surf_created(*ppSurface, MultiSample, !Lockable);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateDepthStencilSurface(IDirect3DDevice9 *This, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    HRESULT hr = create_check(0, D3DPOOL_DEFAULT, Format, 0, pSharedHandle, ppSurface);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_CreateDepthStencilSurface(This, Width, Height, Format, MultiSample,
                                                               MultisampleQuality, Discard, ppSurface, pSharedHandle);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        res_created(*ppSurface, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, Width, Height, 1, 1, 1);
        surf_created(*ppSurface, MultiSample, 0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_CreateOffscreenPlainSurface(IDirect3DDevice9 *This, UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    HRESULT hr = create_check(0, Pool, Format, 0, pSharedHandle, ppSurface);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_CreateOffscreenPlainSurface(This, Width, Height, Format, Pool, ppSurface,
                                                                 pSharedHandle);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface)
        res_created(*ppSurface, Format, 0, Pool, Width, Height, 1, 1, 1);
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Level getters                                                            */
/* ------------------------------------------------------------------------ */

/* Ties a level surface/volume proxy to its container: storage and wire
 * addressing go through the container, which holds the level for its own
 * lifetime. The level has no count of its own: the references the
 * application holds on it are its container's, as native levels share their
 * texture's count (and a level does not reference the device). */
static void link_level(void *child, void *container, uint32_t level, uint32_t face)
{
    struct qd3d_obj *o = qd3d_obj(child);
    struct qd3d_obj *c = qd3d_obj(container);
    struct qd3d_obj *old = NULL;
    struct qd3d_res *r, *cr;
    LONG refs = 0;
    int owned = 0;

    qd3d_rt_lock();
    r = res_get(o);
    if (r && !r->container) {
        r->container = c;
        r->level = level;
        r->face = face;
        cr = res_get(c);
        if (cr && cr->nowned == cr->owned_cap) {
            uint32_t cap = cr->owned_cap ? cr->owned_cap * 2u : 8u;
            struct qd3d_obj **n = (struct qd3d_obj **)realloc(cr->owned, cap * sizeof(*n));

            if (n) {
                cr->owned = n;
                cr->owned_cap = cap;
            }
        }
        if (cr && cr->nowned < cr->owned_cap) {
            cr->owned[cr->nowned++] = o;
            qd3d_obj_hold(o);
            old = o->parent;
            o->parent = c;
            o->owned_level = 1;
            MemoryBarrier();
            refs = InterlockedExchange(&o->refs, 0);
            owned = 1;
        }
    }
    qd3d_rt_unlock();
    if (owned) {
        while (refs-- > 0)
            qd3d_obj_addref(c);
        if (old && old != c)
            qd3d_obj_release(old);
    } else if (r && r->container == c) {
        qd3d_obj_set_parent(o, c); /* no memory to own it: a counted level */
    }
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DTexture9_GetSurfaceLevel(IDirect3DTexture9 *This, UINT Level, IDirect3DSurface9 **ppSurfaceLevel)
{
    HRESULT hr = qd3d9g_IDirect3DTexture9_GetSurfaceLevel(This, Level, ppSurfaceLevel);

    if (SUCCEEDED(hr) && ppSurfaceLevel && *ppSurfaceLevel)
        link_level(*ppSurfaceLevel, This, Level, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DCubeTexture9_GetCubeMapSurface(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface)
{
    HRESULT hr = qd3d9g_IDirect3DCubeTexture9_GetCubeMapSurface(This, FaceType, Level, ppCubeMapSurface);

    if (SUCCEEDED(hr) && ppCubeMapSurface && *ppCubeMapSurface)
        link_level(*ppCubeMapSurface, This, Level, (uint32_t)FaceType);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolumeTexture9_GetVolumeLevel(IDirect3DVolumeTexture9 *This, UINT Level, IDirect3DVolume9 **ppVolumeLevel)
{
    HRESULT hr = qd3d9g_IDirect3DVolumeTexture9_GetVolumeLevel(This, Level, ppVolumeLevel);

    if (SUCCEEDED(hr) && ppVolumeLevel && *ppVolumeLevel)
        link_level(*ppVolumeLevel, This, Level, 0);
    return hr;
}

/* ------------------------------------------------------------------------ */
/* GPU-written destinations                                                 */
/* ------------------------------------------------------------------------ */

/* Native validation, answered from guest metadata so the calls stay
 * asynchronous (rules measured on the stock runtime and by Wine's tests
 * passing there). A description the guest cannot read leaves the call to
 * the host. */
int qd3d_res_surf_info(const void *surface, struct qd3d_surf_info *si)
{
    struct qd3d_obj *o;
    struct qd3d_target t;
    struct qd3d_res *r;
    int ok = 0;

    if (!surface)
        return 0;
    o = qd3d_obj(surface);
    if (o->type != QD3D_T_IDirect3DSurface9)
        return 0;
    qd3d_rt_lock();
    r = res_get(o);
    if (r && SUCCEEDED(resolve(o, 0, 0, &t))) {
        si->format = t.r->format;
        si->pool = t.r->pool;
        si->usage = t.r->usage;
        si->w = t.s->w;
        si->h = t.s->h;
        si->ms = r->container ? D3DMULTISAMPLE_NONE : r->ms;
        si->level = r->container != NULL;
        si->locked = t.s->locked || t.r->dcs;
        ok = 1;
    }
    qd3d_rt_unlock();
    return ok;
}

/* A rectangle inside a w x h surface, neither empty nor flipped. */
static int rect_ok(const RECT *rc, UINT w, UINT h)
{
    return !rc || (rc->left >= 0 && rc->top >= 0 && rc->left < rc->right && rc->top < rc->bottom &&
                   (UINT)rc->right <= w && (UINT)rc->bottom <= h);
}

static int same_device(IDirect3DDevice9 *dev, const void *a, const void *b)
{
    return a && b && qd3d_obj_device(a) == qd3d_obj(dev) && qd3d_obj_device(b) == qd3d_obj(dev);
}

static uint32_t dxt_block(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ? 4u : 1u;
}

/* Block formats copy whole blocks or reach the surface edge. */
static int blocks_ok(LONG l, LONG t, LONG r, LONG b, UINT w, UINT h, uint32_t bs)
{
    return !(l % (LONG)bs) && !(t % (LONG)bs) && (!(r % (LONG)bs) || (UINT)r == w) && (!(b % (LONG)bs) || (UINT)b == h);
}

static HRESULT check_update_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *sr,
                                    IDirect3DSurface9 *dst, const POINT *dp)
{
    struct qd3d_surf_info s, d;
    LONG x = dp ? dp->x : 0, y = dp ? dp->y : 0, w, h;
    uint32_t bs;

    if (!same_device(dev, src, dst))
        return D3DERR_INVALIDCALL;
    if (!qd3d_res_surf_info(src, &s) || !qd3d_res_surf_info(dst, &d))
        return D3D_OK;
    if (s.pool != D3DPOOL_SYSTEMMEM || d.pool != D3DPOOL_DEFAULT || s.ms || d.ms || s.format != d.format ||
        s.locked || d.locked || ((s.usage | d.usage) & D3DUSAGE_DEPTHSTENCIL) || !rect_ok(sr, s.w, s.h))
        return D3DERR_INVALIDCALL;
    w = sr ? sr->right - sr->left : (LONG)s.w;
    h = sr ? sr->bottom - sr->top : (LONG)s.h;
    if (x < 0 || y < 0 || x + w > (LONG)d.w || y + h > (LONG)d.h)
        return D3DERR_INVALIDCALL;
    bs = dxt_block(s.format);
    if (bs > 1 && (!blocks_ok(sr ? sr->left : 0, sr ? sr->top : 0, sr ? sr->right : (LONG)s.w,
                              sr ? sr->bottom : (LONG)s.h, s.w, s.h, bs) ||
                   !blocks_ok(x, y, x + w, y + h, d.w, d.h, bs)))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

static HRESULT check_update_texture(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    struct qd3d_obj *so, *dobj;
    struct qd3d_res *rs, *rd;
    HRESULT hr = D3D_OK;

    if (!same_device(dev, src, dst))
        return D3DERR_INVALIDCALL;
    so = qd3d_obj(src);
    dobj = qd3d_obj(dst);
    if (so->type != dobj->type)
        return D3DERR_INVALIDCALL;
    qd3d_rt_lock();
    rs = res_get(so);
    rd = res_get(dobj);
    if (rs && rd && SUCCEEDED(res_desc(so, rs)) && SUCCEEDED(res_desc(dobj, rd)) &&
        (rs->pool != D3DPOOL_SYSTEMMEM || rd->pool != D3DPOOL_DEFAULT))
        hr = D3DERR_INVALIDCALL;
    qd3d_rt_unlock();
    return hr;
}

static HRESULT check_rt_data(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt, IDirect3DSurface9 *dst)
{
    struct qd3d_surf_info s, d;

    if (!same_device(dev, rt, dst))
        return D3DERR_INVALIDCALL;
    if (!qd3d_res_surf_info(rt, &s) || !qd3d_res_surf_info(dst, &d))
        return D3D_OK;
    if (!(s.usage & D3DUSAGE_RENDERTARGET) || s.ms || d.pool != D3DPOOL_SYSTEMMEM || s.w != d.w || s.h != d.h ||
        s.format != d.format)
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

static HRESULT check_stretch(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *sr,
                             IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE filter)
{
    struct qd3d_surf_info s, d;
    const D3DCAPS9 *caps;
    LONG sw, sh, dw, dh;

    if (!same_device(dev, src, dst) || src == dst || filter > D3DTEXF_LINEAR)
        return D3DERR_INVALIDCALL;
    if (!qd3d_res_surf_info(src, &s) || !qd3d_res_surf_info(dst, &d))
        return D3D_OK;
    if (!rect_ok(sr, s.w, s.h) || !rect_ok(dr, d.w, d.h) || s.pool != D3DPOOL_DEFAULT || d.pool != D3DPOOL_DEFAULT)
        return D3DERR_INVALIDCALL;
    sw = sr ? sr->right - sr->left : (LONG)s.w;
    sh = sr ? sr->bottom - sr->top : (LONG)s.h;
    dw = dr ? dr->right - dr->left : (LONG)d.w;
    dh = dr ? dr->bottom - dr->top : (LONG)d.h;
    if ((s.usage | d.usage) & D3DUSAGE_DEPTHSTENCIL) {
        /* Whole depth stencils of one size and format, outside a scene. */
        if (!(s.usage & d.usage & D3DUSAGE_DEPTHSTENCIL) || qd3d_dev_in_scene(qd3d_obj(dev)) || s.w != d.w ||
            s.h != d.h || s.format != d.format || sw != (LONG)s.w || sh != (LONG)s.h || dw != (LONG)d.w ||
            dh != (LONG)d.h)
            return D3DERR_INVALIDCALL;
        return D3D_OK;
    }
    if (d.level && !(d.usage & D3DUSAGE_RENDERTARGET))
        return D3DERR_INVALIDCALL; /* plain texture destination */
    caps = qd3d_dev_caps(qd3d_obj(dev));
    if (s.level && !(s.usage & D3DUSAGE_RENDERTARGET) && caps &&
        !(caps->DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES))
        return D3DERR_INVALIDCALL;
    /* Offscreen plain destinations take unscaled offscreen plain sources. */
    if (!(d.usage & D3DUSAGE_RENDERTARGET) &&
        (s.level || (s.usage & D3DUSAGE_RENDERTARGET) || sw != dw || sh != dh))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

static HRESULT check_fill(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface, const RECT *rc)
{
    struct qd3d_surf_info s;

    if (!same_device(dev, surface, surface))
        return D3DERR_INVALIDCALL;
    if (!qd3d_res_surf_info(surface, &s))
        return D3D_OK;
    /* Default-pool render targets and offscreen plain surfaces only. */
    if (s.pool != D3DPOOL_DEFAULT || (s.usage & D3DUSAGE_DEPTHSTENCIL) ||
        (s.level && !(s.usage & D3DUSAGE_RENDERTARGET)) || !rect_ok(rc, s.w, s.h))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_UpdateSurface(IDirect3DDevice9 *This, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint)
{
    HRESULT hr = check_update_surface(This, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_UpdateSurface(This, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestinationSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_UpdateTexture(IDirect3DDevice9 *This, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
    HRESULT hr = check_update_texture(This, pSourceTexture, pDestinationTexture);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_UpdateTexture(This, pSourceTexture, pDestinationTexture);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestinationTexture);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetRenderTargetData(IDirect3DDevice9 *This, IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface)
{
    HRESULT hr = check_rt_data(This, pRenderTarget, pDestSurface);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_GetRenderTargetData(This, pRenderTarget, pDestSurface);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_GetFrontBufferData(IDirect3DDevice9 *This, UINT iSwapChain, IDirect3DSurface9 *pDestSurface)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_GetFrontBufferData(This, iSwapChain, pDestSurface);

    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSwapChain9_GetFrontBufferData(IDirect3DSwapChain9 *This, IDirect3DSurface9 *pDestSurface)
{
    HRESULT hr = qd3d9g_IDirect3DSwapChain9_GetFrontBufferData(This, pDestSurface);

    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_StretchRect(IDirect3DDevice9 *This, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
    HRESULT hr = check_stretch(This, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_StretchRect(This, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_ColorFill(IDirect3DDevice9 *This, IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color)
{
    HRESULT hr = check_fill(This, pSurface, pRect);

    if (SUCCEEDED(hr))
        hr = qd3d9g_IDirect3DDevice9_ColorFill(This, pSurface, pRect, color);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pSurface);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_ProcessVertices(IDirect3DDevice9 *This, UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags)
{
    HRESULT hr;

    qd3d_dev_flush(qd3d_obj(This));
    hr = qd3d9g_IDirect3DDevice9_ProcessVertices(This, SrcStartIndex, DestIndex, VertexCount, pDestBuffer,
                                                 pVertexDecl, Flags);
    if (SUCCEEDED(hr))
        qd3d_res_mark_gpu(pDestBuffer);
    return hr;
}

/* PreLoad makes a managed resource resident: its pending writes go now. */
void STDMETHODCALLTYPE qd3d9o_IDirect3DResource9_PreLoad(IDirect3DResource9 *This)
{
    qd3d_res_flush(This);
    qd3d9g_IDirect3DResource9_PreLoad(This);
}

/* AddDirtyRect/AddDirtyBox: the region (level-0 coordinates; NULL = all)
 * must lie inside level 0 and be non-empty. A managed texture uploads it at
 * its next use whatever the guest copy holds there (writes made with
 * D3DLOCK_NO_DIRTY_UPDATE included); system-memory textures keep their
 * dirty region on the host, which the call reaches as before. */
static HRESULT add_dirty(struct qd3d_obj *o, uint32_t face, const D3DBOX *box)
{
    struct qd3d_res *r;
    HRESULT hr = D3D_OK;

    qd3d_rt_lock();
    r = res_get(o);
    if (!r || FAILED(res_desc(o, r)) || face >= r->faces) {
        hr = D3DERR_INVALIDCALL;
    } else if (box) {
        const struct qd3d_sub *s = &r->subs[face * r->levels];

        if (box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back ||
            box->Right > s->w || box->Bottom > s->h || box->Back > s->d)
            hr = D3DERR_INVALIDCALL;
    }
    if (SUCCEEDED(hr) && r->pool == D3DPOOL_MANAGED) {
        struct qd3d_box b;

        if (box) {
            b.x0 = box->Left;
            b.y0 = box->Top;
            b.z0 = box->Front;
            b.x1 = box->Right;
            b.y1 = box->Bottom;
            b.z1 = box->Back;
        }
        pend_scaled(r, face, 0, box ? &b : NULL);
        qd3d_dev_note_flush(o->device);
    }
    qd3d_rt_unlock();
    return hr;
}

static HRESULT add_dirty_rect(struct qd3d_obj *o, uint32_t face, const RECT *rc)
{
    D3DBOX b;

    if (!rc)
        return add_dirty(o, face, NULL);
    if (rc->left < 0 || rc->top < 0)
        return D3DERR_INVALIDCALL;
    b.Left = (UINT)rc->left;
    b.Top = (UINT)rc->top;
    b.Right = (UINT)rc->right;
    b.Bottom = (UINT)rc->bottom;
    b.Front = 0;
    b.Back = 1;
    return add_dirty(o, face, &b);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DTexture9_AddDirtyRect(IDirect3DTexture9 *This, const RECT *pDirtyRect)
{
    HRESULT hr = add_dirty_rect(qd3d_obj(This), 0, pDirtyRect);

    return FAILED(hr) ? hr : qd3d9g_IDirect3DTexture9_AddDirtyRect(This, pDirtyRect);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DCubeTexture9_AddDirtyRect(IDirect3DCubeTexture9 *This, D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect)
{
    HRESULT hr = add_dirty_rect(qd3d_obj(This), (uint32_t)FaceType, pDirtyRect);

    return FAILED(hr) ? hr : qd3d9g_IDirect3DCubeTexture9_AddDirtyRect(This, FaceType, pDirtyRect);
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolumeTexture9_AddDirtyBox(IDirect3DVolumeTexture9 *This, const D3DBOX *pDirtyBox)
{
    HRESULT hr = add_dirty(qd3d_obj(This), 0, pDirtyBox);

    return FAILED(hr) ? hr : qd3d9g_IDirect3DVolumeTexture9_AddDirtyBox(This, pDirtyBox);
}

static void evict_one(struct qd3d_obj *o, void *dev)
{
    struct qd3d_res *r = kind_of(o->type) ? (struct qd3d_res *)o->ext : NULL;

    if (o->device == (struct qd3d_obj *)dev && r && r->desc_ok && !r->container && r->pool == D3DPOOL_MANAGED)
        pend_all(r);
}

/* Evicted managed resources upload all of their guest copy at next use. */
HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DDevice9_EvictManagedResources(IDirect3DDevice9 *This)
{
    HRESULT hr = qd3d9g_IDirect3DDevice9_EvictManagedResources(This);

    if (SUCCEEDED(hr)) {
        qd3d_obj_foreach(evict_one, qd3d_obj(This));
        qd3d_dev_note_flush(qd3d_obj(This));
    }
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Descriptions                                                             */
/* ------------------------------------------------------------------------ */

/* Direct3D 9Ex has no managed pool, so the host reports D3DPOOL_DEFAULT for
 * resources the application created in D3DPOOL_MANAGED. Descriptions report
 * the pool the application asked for (level surfaces/volumes: their
 * texture's). */
static void desc_app_pool(void *iface, D3DPOOL *pool)
{
    struct qd3d_obj *o = qd3d_obj(iface);
    struct qd3d_res *r;

    if (!o)
        return;
    qd3d_rt_lock();
    r = (struct qd3d_res *)o->ext;
    if (r && r->container)
        r = (struct qd3d_res *)r->container->ext;
    if (r && r->desc_ok)
        *pool = r->pool;
    qd3d_rt_unlock();
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DTexture9_GetLevelDesc(IDirect3DTexture9 *This, UINT Level, D3DSURFACE_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DTexture9_GetLevelDesc(This, Level, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DCubeTexture9_GetLevelDesc(IDirect3DCubeTexture9 *This, UINT Level, D3DSURFACE_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DCubeTexture9_GetLevelDesc(This, Level, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolumeTexture9_GetLevelDesc(IDirect3DVolumeTexture9 *This, UINT Level, D3DVOLUME_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DVolumeTexture9_GetLevelDesc(This, Level, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DSurface9_GetDesc(IDirect3DSurface9 *This, D3DSURFACE_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DSurface9_GetDesc(This, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVolume9_GetDesc(IDirect3DVolume9 *This, D3DVOLUME_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DVolume9_GetDesc(This, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DVertexBuffer9_GetDesc(IDirect3DVertexBuffer9 *This, D3DVERTEXBUFFER_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DVertexBuffer9_GetDesc(This, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE qd3d9o_IDirect3DIndexBuffer9_GetDesc(IDirect3DIndexBuffer9 *This, D3DINDEXBUFFER_DESC *pDesc)
{
    HRESULT hr = qd3d9g_IDirect3DIndexBuffer9_GetDesc(This, pDesc);

    if (SUCCEEDED(hr) && pDesc)
        desc_app_pool(This, &pDesc->Pool);
    return hr;
}
