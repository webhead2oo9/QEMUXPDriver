/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: IDirect3DVertexBuffer7 on a D3D9 dynamic vertex buffer (the
 * QD3D proxy keeps every buffer's bytes in guest memory, so locks read back
 * what was written), and the CPU vertex processing of ProcessVertices and
 * ComputeSphereVisibility, following the Direct3D fixed-function equations
 * the way the stock runtime's software pipeline produces them (Wine's
 * test_process_vertices, test_fog_process_vertices and
 * test_compute_sphere_visibility pin the results).
 */
#include "qdd_d3d7.h"

Q7VB *q7_vb_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_VB) ? static_cast<Q7VB *>((d7::IDirect3DVertexBuffer7 *)p) : NULL;
}

Q7VB::Q7VB(Q7D3D *d, DWORD caps_, DWORD fvf_, DWORD count_, UINT version_)
    : refs(1), d3d(d), dd(d->dd), vb9(NULL), caps(caps_), fvf(fvf_), count(count_), version(version_),
      stride(q7_fvf_size(fvf_)), size(0), locks(0), locked(NULL), next(NULL)
{
    size = stride * count;
    IDirect3DDevice9 *d9 = d->device9();
    /* D3DVBCAPS_WRITEONLY is a hint; native buffers stay readable.  A
     * buffer of an invalid FVF has no bytes; D3D9 wants some. */
    if (d9 && FAILED(d9->CreateVertexBuffer(size ? size : 16, D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb9, NULL)))
        vb9 = NULL;
}

void Q7VB::detach()
{
    if (vb9) {
        while (locks > 0) {
            vb9->Unlock();
            --locks;
        }
        vb9->Release();
        vb9 = NULL;
    }
    d3d = NULL;
    dd = NULL;
}

HRESULT STDMETHODCALLTYPE Q7VB::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    /* IDirect3DVertexBuffer7 for buffers of IDirect3D7, IDirect3DVertexBuffer
     * for IDirect3D3's (Wine; one object, the same vtable layout). */
    if (IsEqualGUID(riid, IID_IUnknown) || (version == 7 && IsEqualGUID(riid, Q7IID_IDirect3DVertexBuffer7)) ||
        (version == 3 && IsEqualGUID(riid, Q7IID_IDirect3DVertexBuffer))) {
        AddRef();
        *obj = static_cast<d7::IDirect3DVertexBuffer7 *>(this);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q7VB::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q7VB::Release()
{
    Q7Lock lock;
    LONG r = InterlockedDecrement(&refs);
    if (r > 0)
        return (ULONG)r;
    if (r < 0) {
        InterlockedIncrement(&refs);
        return 0;
    }
    q7_registry_remove(static_cast<d7::IDirect3DVertexBuffer7 *>(this));
    IDirectDraw7 *owner = NULL;
    if (d3d) {
        for (Q7VB **p = &d3d->vbs; *p; p = &(*p)->next)
            if (*p == this) {
                *p = next;
                break;
            }
        /* Only IDirect3D7's buffers hold their DirectDraw object (Wine). */
        if (version == 7)
            owner = qdd_draw_iface7(dd);
    }
    detach();
    delete this;
    if (owner)
        owner->Release(); /* may free the DirectDraw object */
    return 0;
}

/* Native accepts any flags (Wine's VertexBufferLockRest, 0xdeadbeef too). */
HRESULT STDMETHODCALLTYPE Q7VB::Lock(DWORD flags, void **data, DWORD *data_size)
{
    if (!data)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    if (!vb9)
        return DDERR_INVALIDOBJECT;
    /* Native (measured): an optimized buffer no longer locks. */
    if (caps & D3DVBCAPS_OPTIMIZED)
        return D3DERR_VERTEXBUFFEROPTIMIZED;
    DWORD f9 = 0;
    /* DirectX 6 buffers ignore NOOVERWRITE and DISCARDCONTENTS (Wine). */
    if (version < 7)
        flags &= ~(DDLOCK_NOOVERWRITE | DDLOCK_DISCARDCONTENTS);
    if (flags & DDLOCK_READONLY)
        f9 = D3DLOCK_READONLY;
    else if (flags & DDLOCK_DISCARDCONTENTS)
        f9 = D3DLOCK_DISCARD;
    else if (flags & DDLOCK_NOOVERWRITE)
        f9 = D3DLOCK_NOOVERWRITE;
    void *p = NULL;
    HRESULT hr = vb9->Lock(0, 0, &p, f9);
    if (FAILED(hr) || !p)
        return FAILED(hr) ? hr : DDERR_GENERIC;
    ++locks;
    locked = (BYTE *)p;
    *data = p;
    if (data_size)
        *data_size = size;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7VB::Unlock()
{
    Q7Lock lock;
    if (!vb9 || locks <= 0)
        return D3D_OK;
    vb9->Unlock();
    if (--locks == 0)
        locked = NULL;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7VB::GetVertexBufferDesc(d7::D3DVERTEXBUFFERDESC *desc)
{
    if (!desc)
        return DDERR_INVALIDPARAMS;
    /* dwSize is the caller's and stays as it is (Wine's VertexBufferDescTest). */
    desc->dwCaps = caps;
    desc->dwFVF = fvf;
    desc->dwNumVertices = count;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7VB::Optimize(d7::IDirect3DDevice7 *device, DWORD flags)
{
    Q7Lock lock;
    caps |= D3DVBCAPS_OPTIMIZED;
    return D3D_OK;
}

const BYTE *Q7VB::map_read()
{
    void *p = NULL;
    if (!vb9 || FAILED(vb9->Lock(0, 0, &p, D3DLOCK_READONLY)))
        return NULL;
    return (const BYTE *)p;
}

void Q7VB::unmap_read()
{
    vb9->Unlock();
}

void q7_source_from_fvf(Q7Source *s, DWORD fvf, const BYTE *base, UINT stride)
{
    Q7FvfLayout l;
    memset(s, 0, sizeof(*s));
    s->fvf = fvf;
    if (!q7_fvf_layout(fvf, &l))
        return;
    s->pos.ptr = base;
    s->pos.stride = stride;
    if (l.normal >= 0) {
        s->normal.ptr = base + l.normal;
        s->normal.stride = stride;
    }
    if (l.diffuse >= 0) {
        s->diffuse.ptr = base + l.diffuse;
        s->diffuse.stride = stride;
    }
    if (l.specular >= 0) {
        s->specular.ptr = base + l.specular;
        s->specular.stride = stride;
    }
    for (UINT i = 0; i < l.ntex; ++i) {
        s->tex[i].ptr = base + l.tex[i];
        s->tex[i].stride = stride;
    }
}

static HRESULT q7_process(Q7VB *dst, DWORD vertex_op, DWORD dst_idx, DWORD count, const Q7Source *src,
                          Q7Device *dev, DWORD flags)
{
    BYTE *p = NULL;
    UINT off = dst_idx * dst->stride;
    if (FAILED(dst->vb9->Lock(off, dst->size - off, (void **)&p, 0)) || !p)
        return DDERR_GENERIC;
    if (flags & D3DPV_DONOTCOPYDATA)
        vertex_op |= 0x80000000u; /* q7_process_vertices: skip unprocessed data */
    if (dev->version < 7) {
        /* DirectX 6: lit when a material is set and the source has normals
         * (Wine), the legacy way. */
        vertex_op |= 0x40000000u;
        if ((vertex_op & D3DVOP_LIGHT) && dev->legacy_lit(src->fvf, 0))
            vertex_op |= 0x08000000u;
        else
            vertex_op &= ~(DWORD)D3DVOP_LIGHT;
    }
    q7_process_vertices(&dev->st, dev->pv_power, vertex_op, src, count, p, dst->fvf, dst->stride, dst->size - off);
    dst->vb9->Unlock();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7VB::ProcessVertices(DWORD vertex_op, DWORD dst_idx, DWORD n,
                                                d7::IDirect3DVertexBuffer7 *src_buffer, DWORD src_idx,
                                                d7::IDirect3DDevice7 *device, DWORD flags)
{
    if (!(vertex_op & D3DVOP_TRANSFORM))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7VB *src = q7_vb_lookup(src_buffer);
    /* IDirect3DVertexBuffer::ProcessVertices takes an IDirect3DDevice3. */
    Q7Device *dev = version == 7 ? q7_device_lookup(device) : q7_device3_lookup(device);
    if (!src || !dev || !vb9 || !src->vb9 || !dev->d3d)
        return DDERR_INVALIDPARAMS;
    if (dst_idx > count || n > count - dst_idx || src_idx > src->count || n > src->count - src_idx)
        return DDERR_INVALIDPARAMS;
    if (!stride || !src->stride)
        return DDERR_INVALIDPARAMS;
    if (!n)
        return D3D_OK;
    const BYTE *base = src->map_read();
    if (!base)
        return DDERR_GENERIC;
    Q7Source s;
    q7_source_from_fvf(&s, src->fvf, base + (size_t)src_idx * src->stride, src->stride);
    HRESULT hr;
    if (src == this) {
        /* In-place processing: work from a copy of the source range. */
        size_t bytes = (size_t)n * src->stride;
        BYTE *copy = (BYTE *)malloc(bytes);
        if (!copy) {
            src->unmap_read();
            return DDERR_OUTOFMEMORY;
        }
        memcpy(copy, base + (size_t)src_idx * src->stride, bytes);
        src->unmap_read();
        q7_source_from_fvf(&s, src->fvf, copy, src->stride);
        hr = q7_process(this, vertex_op, dst_idx, n, &s, dev, flags);
        free(copy);
        return hr;
    }
    hr = q7_process(this, vertex_op, dst_idx, n, &s, dev, flags);
    src->unmap_read();
    return hr;
}

HRESULT STDMETHODCALLTYPE Q7VB::ProcessVerticesStrided(DWORD vertex_op, DWORD dst_idx, DWORD n,
                                                       d7::D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD src_fvf,
                                                       d7::IDirect3DDevice7 *device, DWORD flags)
{
    if (!(vertex_op & D3DVOP_TRANSFORM) || !data)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    Q7Device *dev = q7_device_lookup(device);
    Q7FvfLayout l;
    if (!dev || !vb9 || !dev->d3d || !q7_fvf_layout(src_fvf, &l) || !data->position.lpvData)
        return DDERR_INVALIDPARAMS;
    if (dst_idx > count || n > count - dst_idx)
        return DDERR_INVALIDPARAMS;
    if (!stride)
        return DDERR_INVALIDPARAMS;
    if (!n)
        return D3D_OK;
    Q7Source s;
    memset(&s, 0, sizeof(s));
    s.fvf = src_fvf;
    s.pos.ptr = (const BYTE *)data->position.lpvData;
    s.pos.stride = data->position.dwStride;
    if (l.normal >= 0) {
        s.normal.ptr = (const BYTE *)data->normal.lpvData;
        s.normal.stride = data->normal.dwStride;
    }
    if (l.diffuse >= 0) {
        s.diffuse.ptr = (const BYTE *)data->diffuse.lpvData;
        s.diffuse.stride = data->diffuse.dwStride;
    }
    if (l.specular >= 0) {
        s.specular.ptr = (const BYTE *)data->specular.lpvData;
        s.specular.stride = data->specular.dwStride;
    }
    for (UINT i = 0; i < l.ntex; ++i) {
        s.tex[i].ptr = (const BYTE *)data->textureCoords[i].lpvData;
        s.tex[i].stride = data->textureCoords[i].dwStride;
    }
    return q7_process(this, vertex_op, dst_idx, n, &s, dev, flags);
}

/* ------------------------------------------------------------------------ */
/* CPU transform and lighting                                                */

struct Q7Vec3 {
    float x, y, z;
};

struct Q7Color {
    float r, g, b, a;
};

static inline float q7_dot(const Q7Vec3 &a, const Q7Vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline void q7_normalize(Q7Vec3 *v)
{
    float len2 = q7_dot(*v, *v);
    if (len2 > 0.0f) {
        float r = 1.0f / sqrtf(len2);
        if (r == r && r < 3.0e38f) {
            v->x *= r;
            v->y *= r;
            v->z *= r;
        }
    }
}

static Q7Color q7_color_from_dword(DWORD c)
{
    Q7Color r;
    r.r = ((c >> 16) & 0xff) / 255.0f;
    r.g = ((c >> 8) & 0xff) / 255.0f;
    r.b = (c & 0xff) / 255.0f;
    r.a = (c >> 24) / 255.0f;
    return r;
}

static Q7Color q7_color_from_d3d(const d7::D3DCOLORVALUE &c)
{
    Q7Color r;
    r.r = c.r;
    r.g = c.g;
    r.b = c.b;
    r.a = c.a;
    return r;
}

static DWORD q7_dword_from_color(Q7Color c)
{
    float v[4] = {c.a, c.r, c.g, c.b};
    DWORD out = 0;
    for (int i = 0; i < 4; ++i) {
        float f = v[i];
        if (!(f > 0.0f))
            f = 0.0f;
        if (f > 1.0f)
            f = 1.0f;
        out = (out << 8) | (DWORD)(f * 255.0f + 0.5f);
    }
    return out;
}

static Q7Vec3 q7_xform_point(const D3DMATRIX *m, float x, float y, float z, float *w)
{
    Q7Vec3 r;
    r.x = x * m->_11 + y * m->_21 + z * m->_31 + m->_41;
    r.y = x * m->_12 + y * m->_22 + z * m->_32 + m->_42;
    r.z = x * m->_13 + y * m->_23 + z * m->_33 + m->_43;
    *w = x * m->_14 + y * m->_24 + z * m->_34 + m->_44;
    return r;
}

static Q7Vec3 q7_xform_dir(const D3DMATRIX *m, const Q7Vec3 &v)
{
    Q7Vec3 r;
    r.x = v.x * m->_11 + v.y * m->_21 + v.z * m->_31;
    r.y = v.x * m->_12 + v.y * m->_22 + v.z * m->_32;
    r.z = v.x * m->_13 + v.y * m->_23 + v.z * m->_33;
    return r;
}

/* Normals: the upper 3x3 of the inverse transpose of the whole 4x4
 * world-view matrix, as the stock software pipeline computes it (Wine's
 * test_lighting: a non-affine matrix lights nothing, nor does a singular
 * one, whose normals become zero).  For affine matrices this is the usual
 * inverse transpose of the 3x3. */
static void q7_normal_matrix(const D3DMATRIX *m, D3DMATRIX *out)
{
    const float *a = &m->_11;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    memset(out, 0, sizeof(*out));
    if (det == 0.0f || det != det)
        return;
    float r = 1.0f / det;
    /* inv[] is the adjugate (row-major, inv[4*row+col]); the normal
     * matrix is its transpose's upper 3x3 scaled by 1/det. */
    out->_11 = inv[0] * r;
    out->_12 = inv[4] * r;
    out->_13 = inv[8] * r;
    out->_21 = inv[1] * r;
    out->_22 = inv[5] * r;
    out->_23 = inv[9] * r;
    out->_31 = inv[2] * r;
    out->_32 = inv[6] * r;
    out->_33 = inv[10] * r;
    out->_44 = 1.0f;
}

struct Q7LightX {
    DWORD type;
    Q7Color diffuse, specular, ambient;
    Q7Vec3 pos, dir; /* view space; dir points from the light (normalized) */
    float range, falloff, a0, a1, a2, cos_htheta, cos_hphi;
};

static float q7_fog(DWORD mode, float c, float start, float end, float density)
{
    switch (mode) {
    case D3DFOG_LINEAR:
        return end == start ? 1.0f : (end - c) / (end - start);
    case D3DFOG_EXP:
        return expf(-c * density);
    case D3DFOG_EXP2:
        return expf(-c * c * density * density);
    default:
        return 1.0f;
    }
}

/* vertex_op flag bits beyond D3DVOP_*:
 *   bit 31  D3DPV_DONOTCOPYDATA
 *   bit 30  Direct3D 1-6 lighting: legacy attenuation, normals through the
 *           world-view matrix itself, specular only with a power, specular
 *           alpha 0
 *   bit 29  positions in homogeneous clip space (execute buffers)
 *   bit 28  light only: positions copied untransformed (XYZ), no fog (the
 *           GPU transforms and fogs the pre-lit vertices)
 *   bit 27  light whatever D3DRENDERSTATE_LIGHTING says */
void q7_process_vertices(const Q7State *st, float power0, DWORD vertex_op, const Q7Source *src, DWORD count,
                         BYTE *dst, DWORD dst_fvf, UINT dst_stride, UINT dst_avail)
{
    Q7FvfLayout dl, sl;
    if (!q7_fvf_layout(dst_fvf, &dl) || !q7_fvf_layout(src->fvf, &sl))
        return;
    bool nocopy = (vertex_op & 0x80000000u) != 0;
    const D3DMATRIX *world = &st->xf[0], *view = &st->xf[1], *proj = &st->xf[2];
    D3DMATRIX wv, m;
    q7_mat_mul(&wv, world, view);
    q7_mat_mul(&m, &wv, proj);

    /* Viewport; a degenerate or reversed depth range gets 0.001 (native). */
    float vx = (float)st->vp.dwX, vy = (float)st->vp.dwY, vw = (float)st->vp.dwWidth, vh = (float)st->vp.dwHeight;
    float min_z = st->vp.dvMinZ, max_z = st->vp.dvMaxZ;
    if (!(max_z >= min_z + 0.001f))
        max_z = min_z + 0.001f;

    bool lighting = (vertex_op & D3DVOP_LIGHT) &&
                    (st->rs[d7::D3DRENDERSTATE_LIGHTING] || (vertex_op & 0x08000000u)) &&
                    (dl.diffuse >= 0 || dl.specular >= 0);
    bool legacy = (vertex_op & 0x40000000u) != 0;
    bool light_only = (vertex_op & 0x10000000u) != 0;
    DWORD fog_mode = st->rs[d7::D3DRENDERSTATE_FOGVERTEXMODE];
    bool fog = fog_mode != D3DFOG_NONE && dl.specular >= 0 && !light_only;
    bool range_fog = st->rs[d7::D3DRENDERSTATE_RANGEFOGENABLE] != 0;
    float fog_start = q7_dw2f(st->rs[d7::D3DRENDERSTATE_FOGSTART]);
    float fog_end = q7_dw2f(st->rs[d7::D3DRENDERSTATE_FOGEND]);
    float fog_density = q7_dw2f(st->rs[d7::D3DRENDERSTATE_FOGDENSITY]);

    /* Lights in view space. */
    Q7LightX lights[64];
    UINT nl = 0;
    D3DMATRIX nm;
    Q7Color global_ambient = q7_color_from_dword(st->rs[d7::D3DRENDERSTATE_AMBIENT]);
    bool spec_enable = st->rs[d7::D3DRENDERSTATE_SPECULARENABLE] != 0;
    bool local_viewer = st->rs[d7::D3DRENDERSTATE_LOCALVIEWER] != 0;
    bool normalize = st->rs[d7::D3DRENDERSTATE_NORMALIZENORMALS] != 0;
    if (lighting) {
        if (legacy) {
            /* The DirectX 3-6 pipeline transforms normals by the matrix
             * itself (measured through Wine's ddraw1/2/4 test_lighting:
             * singular and non-uniform matrices light as such). */
            nm = wv;
            nm._41 = nm._42 = nm._43 = 0.0f;
        } else {
            q7_normal_matrix(&wv, &nm);
        }
        for (UINT i = 0; i < st->nlights && nl < 64; ++i) {
            const Q7Light *L = &st->lights[i];
            if (!L->enabled)
                continue;
            Q7LightX *x = &lights[nl++];
            const d7::D3DLIGHT7 *l = &L->light;
            x->type = l->dltType;
            x->diffuse = q7_color_from_d3d(l->dcvDiffuse);
            x->specular = q7_color_from_d3d(l->dcvSpecular);
            x->ambient = q7_color_from_d3d(l->dcvAmbient);
            float w;
            x->pos = q7_xform_point(view, l->dvPosition.x, l->dvPosition.y, l->dvPosition.z, &w);
            Q7Vec3 d = {l->dvDirection.x, l->dvDirection.y, l->dvDirection.z};
            x->dir = q7_xform_dir(view, d);
            q7_normalize(&x->dir);
            x->range = l->dvRange;
            x->falloff = l->dvFalloff;
            x->a0 = l->dvAttenuation0;
            x->a1 = l->dvAttenuation1;
            x->a2 = l->dvAttenuation2;
            x->cos_htheta = cosf(l->dvTheta * 0.5f);
            x->cos_hphi = cosf(l->dvPhi * 0.5f);
        }
    }
    DWORD cv = st->rs[d7::D3DRENDERSTATE_COLORVERTEX];
    DWORD src_d = cv ? st->rs[d7::D3DRENDERSTATE_DIFFUSEMATERIALSOURCE] : (DWORD)D3DMCS_MATERIAL;
    DWORD src_s = cv ? st->rs[d7::D3DRENDERSTATE_SPECULARMATERIALSOURCE] : (DWORD)D3DMCS_MATERIAL;
    DWORD src_a = cv ? st->rs[d7::D3DRENDERSTATE_AMBIENTMATERIALSOURCE] : (DWORD)D3DMCS_MATERIAL;
    DWORD src_e = cv ? st->rs[d7::D3DRENDERSTATE_EMISSIVEMATERIALSOURCE] : (DWORD)D3DMCS_MATERIAL;

    for (DWORD i = 0; i < count; ++i) {
        BYTE *out = dst + (size_t)i * dst_stride;
        const float *p = (const float *)(src->pos.ptr + (size_t)i * src->pos.stride);
        float w;
        Q7Vec3 c = q7_xform_point(&m, p[0], p[1], p[2], &w);
        if (light_only) {
            memcpy(out, p, 12);
        } else if (vertex_op & 0x20000000u) {
            /* Homogeneous clip space (execute buffers clip, then project). */
            float hc[4] = {c.x, c.y, c.z, w};
            memcpy(out, hc, 16);
        } else {
            float x = c.x / w, y = c.y / w, z = c.z / w;
            x = x * (vw / 2.0f) + vw / 2.0f + vx;
            y = -y * (vh / 2.0f) + vh / 2.0f + vy;
            z = z * (max_z - min_z) + min_z;
            float rhw = 1.0f / w;
            float pos[4] = {x, y, z, rhw};
            memcpy(out, pos, 12);
            /* The fourth float lands in the next element even for an XYZ
             * destination (native writes it, Wine's test_process_vertices). */
            if ((size_t)i * dst_stride + 16 <= dst_avail)
                memcpy(out + 12, &rhw, 4);
        }

        if (dl.normal >= 0 && src->normal.ptr && !nocopy)
            memcpy(out + dl.normal, src->normal.ptr + (size_t)i * src->normal.stride, 12);

        DWORD vdiff = src->diffuse.ptr ? *(const DWORD *)(src->diffuse.ptr + (size_t)i * src->diffuse.stride)
                                       : 0xffffffffu;
        DWORD vspec = src->specular.ptr ? *(const DWORD *)(src->specular.ptr + (size_t)i * src->specular.stride)
                                        : 0x00000000u;
        Q7Color mdiff = src_d == D3DMCS_COLOR1 && src->diffuse.ptr   ? q7_color_from_dword(vdiff)
                        : src_d == D3DMCS_COLOR2 && src->specular.ptr ? q7_color_from_dword(vspec)
                                                                      : q7_color_from_d3d(st->mat.dcvDiffuse);
        Q7Color mspec = src_s == D3DMCS_COLOR1 && src->diffuse.ptr   ? q7_color_from_dword(vdiff)
                        : src_s == D3DMCS_COLOR2 && src->specular.ptr ? q7_color_from_dword(vspec)
                                                                      : q7_color_from_d3d(st->mat.dcvSpecular);
        Q7Color mamb = src_a == D3DMCS_COLOR1 && src->diffuse.ptr   ? q7_color_from_dword(vdiff)
                       : src_a == D3DMCS_COLOR2 && src->specular.ptr ? q7_color_from_dword(vspec)
                                                                     : q7_color_from_d3d(st->mat.dcvAmbient);
        Q7Color memis = src_e == D3DMCS_COLOR1 && src->diffuse.ptr   ? q7_color_from_dword(vdiff)
                        : src_e == D3DMCS_COLOR2 && src->specular.ptr ? q7_color_from_dword(vspec)
                                                                      : q7_color_from_d3d(st->mat.dcvEmissive);

        Q7Color out_d, out_s;
        Q7Vec3 pv = {0, 0, 0};
        if (lighting || fog) {
            float pw;
            pv = q7_xform_point(&wv, p[0], p[1], p[2], &pw);
            if (pw != 0.0f && pw != 1.0f) {
                pv.x /= pw;
                pv.y /= pw;
                pv.z /= pw;
            }
        }
        if (lighting) {
            Q7Color amb = global_ambient, dif = {0, 0, 0, 0}, spe = {0, 0, 0, 0};
            Q7Vec3 n = {0, 0, 0};
            bool has_n = src->normal.ptr != NULL;
            if (has_n) {
                const float *np = (const float *)(src->normal.ptr + (size_t)i * src->normal.stride);
                Q7Vec3 n0 = {np[0], np[1], np[2]};
                n = q7_xform_dir(&nm, n0);
                if (normalize)
                    q7_normalize(&n);
            }
            Q7Vec3 eye;
            if (local_viewer) {
                eye.x = -pv.x;
                eye.y = -pv.y;
                eye.z = -pv.z;
                q7_normalize(&eye);
            } else {
                eye.x = 0.0f;
                eye.y = 0.0f;
                eye.z = -1.0f;
            }
            /* Native ProcessVertices keeps the last non-zero shininess when
             * the material's is 0 (Wine's test_specular_lighting). */
            float power = legacy || st->mat.dvPower != 0.0f ? st->mat.dvPower : power0;
            for (UINT li = 0; li < nl; ++li) {
                const Q7LightX *L = &lights[li];
                Q7Vec3 ldir;
                float att = 1.0f;
                if (L->type == D3DLIGHT_DIRECTIONAL) {
                    ldir.x = -L->dir.x;
                    ldir.y = -L->dir.y;
                    ldir.z = -L->dir.z;
                } else {
                    ldir.x = L->pos.x - pv.x;
                    ldir.y = L->pos.y - pv.y;
                    ldir.z = L->pos.z - pv.z;
                    float d2 = q7_dot(ldir, ldir), d = sqrtf(d2);
                    if (legacy) {
                        /* Direct3D 1-6: a0 + a1 x + a2 x^2 with x = (range - d) / range,
                         * nothing at or past the range (Wine's legacy lighting). */
                        if (!(d < L->range))
                            continue;
                        float x = (L->range - d) / L->range;
                        att = L->a0 + L->a1 * x + L->a2 * x * x;
                    } else {
                        if (!(d <= L->range))
                            continue;
                        float den = L->a0 + L->a1 * d + L->a2 * d2;
                        att = den > 0.0f ? 1.0f / den : 1.0f;
                    }
                    q7_normalize(&ldir);
                    if (L->type == D3DLIGHT_SPOT) {
                        float rho = -q7_dot(ldir, L->dir);
                        if (rho <= L->cos_hphi)
                            att = 0.0f;
                        else if (rho <= L->cos_htheta)
                            att *= powf((rho - L->cos_hphi) / (L->cos_htheta - L->cos_hphi), L->falloff);
                    }
                }
                amb.r += L->ambient.r * att;
                amb.g += L->ambient.g * att;
                amb.b += L->ambient.b * att;
                if (!has_n)
                    continue;
                float ndl = q7_dot(n, ldir);
                if (ndl > 0.0f) {
                    dif.r += L->diffuse.r * ndl * att;
                    dif.g += L->diffuse.g * ndl * att;
                    dif.b += L->diffuse.b * ndl * att;
                    if (spec_enable) {
                        Q7Vec3 h = {ldir.x + eye.x, ldir.y + eye.y, ldir.z + eye.z};
                        q7_normalize(&h);
                        float ndh = q7_dot(n, h);
                        if (ndh > 0.0f && (!legacy || power > 0.0f)) {
                            float sp = powf(ndh, power) * att;
                            spe.r += L->specular.r * sp;
                            spe.g += L->specular.g * sp;
                            spe.b += L->specular.b * sp;
                        }
                    }
                }
            }
            out_d.r = amb.r * mamb.r + dif.r * mdiff.r + memis.r;
            out_d.g = amb.g * mamb.g + dif.g * mdiff.g + memis.g;
            out_d.b = amb.b * mamb.b + dif.b * mdiff.b + memis.b;
            out_d.a = mdiff.a;
            if (spec_enable) {
                out_s.r = spe.r * mspec.r;
                out_s.g = spe.g * mspec.g;
                out_s.b = spe.b * mspec.b;
            } else {
                out_s.r = out_s.g = out_s.b = 0.0f;
            }
            out_s.a = legacy ? 0.0f : mspec.a;
        } else {
            out_d = q7_color_from_dword(vdiff);
            out_s = q7_color_from_dword(vspec);
        }
        if (fog) {
            float fc = range_fog ? sqrtf(q7_dot(pv, pv)) : fabsf(pv.z);
            out_s.a = q7_fog(fog_mode, fc, fog_start, fog_end, fog_density);
        }
        if (dl.diffuse >= 0 && (lighting || !nocopy)) {
            DWORD v = lighting ? q7_dword_from_color(out_d) : vdiff;
            memcpy(out + dl.diffuse, &v, 4);
        }
        if (dl.specular >= 0 && (lighting || fog || !nocopy)) {
            DWORD v = (lighting || fog) ? q7_dword_from_color(out_s) : vspec;
            memcpy(out + dl.specular, &v, 4);
        }
        if (!nocopy)
            for (UINT t = 0; t < dl.ntex; ++t)
                if (src->tex[t].ptr)
                    memcpy(out + dl.tex[t], src->tex[t].ptr + (size_t)i * src->tex[t].stride, dl.texdim[t] * 4u);
    }
}

/* ------------------------------------------------------------------------ */
/* ComputeSphereVisibility                                                   */

static DWORD q7_in_plane(UINT idx, const float *p, const D3DVECTOR &c, float radius, bool legacy)
{
    float norm = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    float dist = (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3]) / norm;
    if (legacy) {
        /* IDirect3DDevice3 (measured on the stock runtime with sweeps over
         * centres, radii, projections and viewport clip volumes; fits every
         * row of Wine's ddraw4 test_compute_sphere_visibility): outside only
         * when the whole sphere is behind the plane, inside only when it
         * stays at least half a unit in front of it. */
        if (dist + radius < 0.0f)
            return (D3DSTATUS_CLIPUNIONLEFT | D3DSTATUS_CLIPINTERSECTIONLEFT) << idx;
        if (dist - radius < 0.5f)
            return D3DSTATUS_CLIPUNIONLEFT << idx;
        return 0;
    }
    if (fabsf(dist) < radius)
        return D3DSTATUS_CLIPUNIONLEFT << idx;
    if (dist < -radius)
        return (D3DSTATUS_CLIPUNIONLEFT | D3DSTATUS_CLIPINTERSECTIONLEFT) << idx;
    return 0;
}

void q7_sphere_visibility(const Q7State *st, const D3DVECTOR *centers, const float *radii, DWORD count,
                          DWORD *ret, bool legacy)
{
    D3DMATRIX wv, m;
    q7_mat_mul(&wv, &st->xf[0], &st->xf[1]);
    q7_mat_mul(&m, &wv, &st->xf[2]);
    float planes[12][4] = {
        {m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41}, /* left */
        {m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41}, /* right */
        {m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42}, /* top */
        {m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42}, /* bottom */
        {m._13, m._23, m._33, m._43},                                 /* front */
        {m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43}, /* back */
    };
    DWORD enabled = 0x3f | ((st->rs[d7::D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3f) << 6);
    for (UINT i = 0; i < 6; ++i)
        memcpy(planes[6 + i], st->clip[i], sizeof(planes[6 + i]));
    for (DWORD s = 0; s < count; ++s) {
        DWORD r = 0;
        for (UINT j = 0; j < 12; ++j)
            if (enabled & (1u << j))
                r |= q7_in_plane(j, planes[j], centers[s], radii[s], legacy);
        ret[s] = r;
    }
}
