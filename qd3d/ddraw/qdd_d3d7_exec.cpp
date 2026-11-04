/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_d3d7: Direct3D execute buffers (IDirect3DExecuteBuffer, version-1
 * devices).  An execute buffer is application memory holding vertices and
 * an instruction stream; Execute runs the stream on the CPU like the DirectX
 * 3 runtime did: PROCESSVERTICES transforms (and lights) source vertices
 * into the buffer's transformed-vertex array with the state current at that
 * point, POINT/LINE/TRIANGLE draw from that array (clipped against the near
 * plane in clip space, then projected through the viewport) as
 * pre-transformed vertices, the STATE* instructions set the legacy
 * render/light states and transforms (matrix handles).
 */
#include "qdd_d3d7.h"

/* One transformed vertex: the D3DTLVERTEX layout, position either in clip
 * space (x, y, z, w) or, for D3DPROCESSVERTICES_COPY, already on screen. */
struct Q7ExecVertex {
    float p[4];
    DWORD diffuse, specular;
    float tu, tv;
};

Q7ExecBuf *q7_execbuf_lookup(const void *p)
{
    return q7_registry_has(p, Q7K_EXECBUF) ? static_cast<Q7ExecBuf *>((d7::IDirect3DExecuteBuffer *)p) : NULL;
}

Q7ExecBuf::Q7ExecBuf(Q7Device *dev)
    : refs(1), device(dev), desc(), data(), mem(NULL), own_mem(false), locked(false), tl(NULL), tl_count(0)
{
}

Q7ExecBuf::~Q7ExecBuf()
{
    if (own_mem)
        free(mem);
    free(tl);
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::QueryInterface(REFIID riid, void **obj)
{
    if (!obj)
        return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, Q7IID_IDirect3DExecuteBuffer) || IsEqualGUID(riid, IID_IUnknown)) {
        AddRef();
        *obj = static_cast<d7::IDirect3DExecuteBuffer *>(this);
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Q7ExecBuf::AddRef()
{
    return (ULONG)InterlockedIncrement(&refs);
}

ULONG STDMETHODCALLTYPE Q7ExecBuf::Release()
{
    Q7Lock lock;
    LONG r = InterlockedDecrement(&refs);
    if (r > 0)
        return (ULONG)r;
    if (r < 0) {
        InterlockedIncrement(&refs);
        return 0;
    }
    q7_registry_remove(static_cast<d7::IDirect3DExecuteBuffer *>(this));
    delete this;
    return 0;
}

/* CreateExecuteBuffer's setup (Wine): the application's memory with
 * D3DDEB_LPDATA, else a zeroed block of dwBufferSize. */
HRESULT STDMETHODCALLTYPE Q7ExecBuf::Initialize(d7::IDirect3DDevice *dev, d7::D3DEXECUTEBUFFERDESC *d)
{
    if (!d || d->dwSize < sizeof(DWORD) * 2)
        return DDERR_INVALIDPARAMS;
    if (mem)
        return D3D_OK;
    memset(&desc, 0, sizeof(desc));
    memcpy(&desc, d, d->dwSize < sizeof(desc) ? d->dwSize : sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (!(desc.dwFlags & D3DDEB_LPDATA))
        desc.lpData = NULL;
    if (!(desc.dwFlags & D3DDEB_BUFSIZE))
        desc.dwBufferSize = 0;
    if (!desc.lpData && desc.dwBufferSize) {
        mem = (BYTE *)calloc(1, desc.dwBufferSize);
        if (!mem)
            return DDERR_OUTOFMEMORY;
        own_mem = true;
        desc.lpData = mem;
    } else {
        mem = (BYTE *)desc.lpData;
    }
    desc.dwFlags |= D3DDEB_LPDATA;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::Lock(d7::D3DEXECUTEBUFFERDESC *d)
{
    if (!d)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    DWORD size = d->dwSize < sizeof(desc) ? d->dwSize : sizeof(desc);
    memcpy(d, &desc, size);
    d->dwSize = size;
    locked = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::Unlock()
{
    Q7Lock lock;
    locked = false;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::SetExecuteData(d7::D3DEXECUTEDATA *d)
{
    if (!d || d->dwSize != sizeof(*d))
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    data = *d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::GetExecuteData(d7::D3DEXECUTEDATA *d)
{
    if (!d)
        return DDERR_INVALIDPARAMS;
    Q7Lock lock;
    *d = data; /* the caller's dwSize is not looked at (Wine) */
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::Validate(DWORD *offset, d7::LPD3DVALIDATECALLBACK cb, void *ctx, DWORD reserved)
{
    return DDERR_UNSUPPORTED; /* a debugging aid of the DirectX 3 runtime */
}

HRESULT STDMETHODCALLTYPE Q7ExecBuf::Optimize(DWORD dummy)
{
    return DDERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------------ */
/* Execute                                                                   */

/* Interpolation between two vertices in clip space. */
static Q7ExecVertex q7_lerp(const Q7ExecVertex &a, const Q7ExecVertex &b, float t)
{
    Q7ExecVertex r;
    for (int i = 0; i < 4; ++i)
        r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    DWORD ca[2] = {a.diffuse, a.specular}, cb[2] = {b.diffuse, b.specular}, cr[2];
    for (int k = 0; k < 2; ++k) {
        DWORD v = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            float x = (float)((ca[k] >> sh) & 0xff), y = (float)((cb[k] >> sh) & 0xff);
            DWORD c = (DWORD)(x + (y - x) * t + 0.5f);
            v |= (c > 255 ? 255 : c) << sh;
        }
        cr[k] = v;
    }
    r.diffuse = cr[0];
    r.specular = cr[1];
    r.tu = a.tu + (b.tu - a.tu) * t;
    r.tv = a.tv + (b.tv - a.tv) * t;
    return r;
}

struct Q7Projector {
    float vx, vy, vw, vh, mn, mx;
    /* Clip space -> D3DTLVERTEX (D3D7's viewport mapping). */
    void out(const Q7ExecVertex &v, bool screen, BYTE *dst) const
    {
        float o[4];
        if (screen) {
            memcpy(o, v.p, sizeof(o));
        } else {
            float rhw = 1.0f / v.p[3];
            o[0] = v.p[0] * rhw * (vw / 2.0f) + vw / 2.0f + vx;
            o[1] = -v.p[1] * rhw * (vh / 2.0f) + vh / 2.0f + vy;
            o[2] = v.p[2] * rhw * (mx - mn) + mn;
            o[3] = rhw;
        }
        memcpy(dst, o, 16);
        memcpy(dst + 16, &v.diffuse, 4);
        memcpy(dst + 20, &v.specular, 4);
        memcpy(dst + 24, &v.tu, 4);
        memcpy(dst + 28, &v.tv, 4);
    }
};

/* Output vertex accumulation for one draw instruction. */
struct Q7ExecOut {
    BYTE *buf;
    UINT count, cap;
    bool push(const Q7Projector &pr, const Q7ExecVertex &v, bool screen)
    {
        if (count == cap) {
            UINT n = cap ? cap * 2 : 192;
            BYTE *b = (BYTE *)realloc(buf, (size_t)n * 32);
            if (!b)
                return false;
            buf = b;
            cap = n;
        }
        pr.out(v, screen, buf + (size_t)count * 32);
        ++count;
        return true;
    }
};

/* A polygon of up to 3 clip-space vertices clipped against the near plane
 * (z >= 0, D3D clip space), at most 4 vertices. */
static UINT q7_clip_near(const Q7ExecVertex *in, UINT n, Q7ExecVertex *out)
{
    UINT m = 0;
    for (UINT i = 0; i < n; ++i) {
        const Q7ExecVertex &a = in[i], &b = in[(i + 1) % n];
        bool ain = a.p[2] >= 0.0f, bin = b.p[2] >= 0.0f;
        if (ain)
            out[m++] = a;
        if (ain != bin) {
            float t = a.p[2] / (a.p[2] - b.p[2]);
            out[m++] = q7_lerp(a, b, t);
        }
    }
    return m;
}

HRESULT Q7ExecBuf::execute(Q7Viewport *vp)
{
    Q7Device *dev = device;
    if (!mem || !dev || !dev->d3d)
        return DDERR_INVALIDPARAMS;
    DWORD size = desc.dwBufferSize ? desc.dwBufferSize : 0xffffffffu;
    if (data.dwInstructionOffset >= size)
        return DDERR_INVALIDPARAMS;
    /* The transformed-vertex array covers the buffer's vertices. */
    UINT nv = data.dwVertexCount;
    if (nv > tl_count) {
        BYTE *n = (BYTE *)realloc(tl, (size_t)nv * (sizeof(Q7ExecVertex) + 1));
        if (!n)
            return DDERR_OUTOFMEMORY;
        tl = n;
        tl_count = nv;
        memset(tl, 0, (size_t)nv * (sizeof(Q7ExecVertex) + 1));
    }
    Q7ExecVertex *tv = (Q7ExecVertex *)tl;
    BYTE *screen = tl + (size_t)tl_count * sizeof(Q7ExecVertex); /* 1: COPY'd, already on screen */
    const BYTE *src_vertices = mem + data.dwVertexOffset;
    Q7ExecOut out = {NULL, 0, 0};

    const BYTE *ip = mem + data.dwInstructionOffset;
    const BYTE *end = desc.dwBufferSize ? mem + desc.dwBufferSize : NULL;
    for (;;) {
        if (end && ip + sizeof(d7::D3DINSTRUCTION) > end)
            break;
        const d7::D3DINSTRUCTION *ins = (const d7::D3DINSTRUCTION *)ip;
        const BYTE *op = ip + sizeof(*ins);
        DWORD count = ins->wCount, isize = ins->bSize;
        const BYTE *next = op + (size_t)count * isize;
        switch (ins->bOpcode) {
        case d7::D3DOP_POINT:
        case d7::D3DOP_LINE:
        case d7::D3DOP_TRIANGLE: {
            const d7::D3DVIEWPORT7 &v7 = dev->st.vp;
            Q7Projector pr = {(float)v7.dwX, (float)v7.dwY, (float)v7.dwWidth, (float)v7.dwHeight, v7.dvMinZ,
                              v7.dvMaxZ};
            out.count = 0;
            DWORD prim = d7::D3DPT_TRIANGLELIST;
            for (DWORD i = 0; i < count; ++i) {
                const BYTE *e = op + (size_t)i * isize;
                if (ins->bOpcode == d7::D3DOP_POINT) {
                    const d7::D3DPOINT *pt = (const d7::D3DPOINT *)e;
                    prim = d7::D3DPT_POINTLIST;
                    for (DWORD k = pt->wFirst; k < (DWORD)pt->wFirst + pt->wCount && k < nv; ++k)
                        if (screen[k] || (tv[k].p[3] > 0.0f && tv[k].p[2] >= 0.0f))
                            out.push(pr, tv[k], screen[k] != 0);
                    continue;
                }
                WORD idx[3];
                UINT n;
                if (ins->bOpcode == d7::D3DOP_LINE) {
                    const d7::D3DLINE *l = (const d7::D3DLINE *)e;
                    idx[0] = l->v1;
                    idx[1] = l->v2;
                    n = 2;
                    prim = d7::D3DPT_LINELIST;
                } else {
                    const d7::D3DTRIANGLE *t = (const d7::D3DTRIANGLE *)e;
                    idx[0] = t->v1;
                    idx[1] = t->v2;
                    idx[2] = t->v3;
                    n = 3;
                }
                if (idx[0] >= nv || idx[1] >= nv || (n == 3 && idx[2] >= nv))
                    continue;
                bool all_screen = screen[idx[0]] && screen[idx[1]] && (n == 2 || screen[idx[2]]);
                bool inside = true;
                for (UINT k = 0; k < n; ++k)
                    if (!screen[idx[k]] && tv[idx[k]].p[2] < 0.0f)
                        inside = false;
                if (all_screen || inside) {
                    for (UINT k = 0; k < n; ++k)
                        out.push(pr, tv[idx[k]], screen[idx[k]] != 0);
                    continue;
                }
                /* Crosses the near plane (clip-space vertices only). */
                Q7ExecVertex poly[3] = {tv[idx[0]], tv[idx[1]], tv[idx[2 % n]]}, clipped[4];
                UINT m = q7_clip_near(poly, n, clipped);
                if (n == 2) {
                    if (m >= 2) {
                        out.push(pr, clipped[0], false);
                        out.push(pr, clipped[1], false);
                    }
                    continue;
                }
                for (UINT k = 1; k + 1 < m; ++k) {
                    out.push(pr, clipped[0], false);
                    out.push(pr, clipped[k], false);
                    out.push(pr, clipped[k + 1], false);
                }
            }
            if (out.count)
                dev->legacy_draw(prim, D3DFVF_TLVERTEX, out.buf, out.count, 0);
            break;
        }
        case d7::D3DOP_MATRIXLOAD:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DMATRIXLOAD *ml = (const d7::D3DMATRIXLOAD *)(op + (size_t)i * isize);
                D3DMATRIX *d = (D3DMATRIX *)q7_handle_get(ml->hDestMatrix, Q7H_MATRIX);
                D3DMATRIX *s = (D3DMATRIX *)q7_handle_get(ml->hSrcMatrix, Q7H_MATRIX);
                if (d && s)
                    *d = *s;
            }
            break;
        case d7::D3DOP_MATRIXMULTIPLY:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DMATRIXMULTIPLY *mm = (const d7::D3DMATRIXMULTIPLY *)(op + (size_t)i * isize);
                D3DMATRIX *d = (D3DMATRIX *)q7_handle_get(mm->hDestMatrix, Q7H_MATRIX);
                D3DMATRIX *a = (D3DMATRIX *)q7_handle_get(mm->hSrcMatrix1, Q7H_MATRIX);
                D3DMATRIX *b = (D3DMATRIX *)q7_handle_get(mm->hSrcMatrix2, Q7H_MATRIX);
                if (d && a && b)
                    q7_mat_mul(d, a, b);
            }
            break;
        case d7::D3DOP_STATETRANSFORM:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DSTATE *s = (const d7::D3DSTATE *)(op + (size_t)i * isize);
                D3DMATRIX *m = (D3DMATRIX *)q7_handle_get(s->dwArg[0], Q7H_MATRIX);
                if (!m)
                    continue;
                DWORD t = s->dtstTransformStateType;
                if (t == d7::D3DTRANSFORMSTATE_WORLD)
                    dev->mx_world = s->dwArg[0];
                else if (t == d7::D3DTRANSFORMSTATE_VIEW)
                    dev->mx_view = s->dwArg[0];
                else if (t == d7::D3DTRANSFORMSTATE_PROJECTION)
                    dev->mx_proj = s->dwArg[0];
                dev->legacy_set_transform(t, m);
            }
            break;
        case d7::D3DOP_STATELIGHT:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DSTATE *s = (const d7::D3DSTATE *)(op + (size_t)i * isize);
                dev->legacy_set_ls(s->dlstLightStateType, s->dwArg[0]);
            }
            break;
        case d7::D3DOP_STATERENDER:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DSTATE *s = (const d7::D3DSTATE *)(op + (size_t)i * isize);
                dev->legacy_set_rs(s->drstRenderStateType, s->dwArg[0]);
            }
            break;
        case d7::D3DOP_PROCESSVERTICES:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DPROCESSVERTICES *pv = (const d7::D3DPROCESSVERTICES *)(op + (size_t)i * isize);
                DWORD first = pv->wStart, dest = pv->wDest, n = pv->dwCount;
                if (first > nv || n > nv - first || dest > nv || n > nv - dest)
                    continue;
                const BYTE *sv = src_vertices + (size_t)first * 32;
                DWORD what = pv->dwFlags & D3DPROCESSVERTICES_OPMASK;
                if (what == D3DPROCESSVERTICES_COPY) {
                    /* D3DTLVERTEX as they are. */
                    for (DWORD k = 0; k < n; ++k) {
                        memcpy(&tv[dest + k], sv + (size_t)k * 32, 32);
                        screen[dest + k] = 1;
                    }
                    continue;
                }
                DWORD fvf = what == D3DPROCESSVERTICES_TRANSFORMLIGHT ? D3DFVF_VERTEX : D3DFVF_LVERTEX;
                Q7FvfLayout l;
                q7_fvf_layout(fvf, &l);
                Q7Source s;
                memset(&s, 0, sizeof(s));
                s.fvf = fvf;
                s.pos.ptr = sv;
                s.pos.stride = 32;
                if (l.normal >= 0) {
                    s.normal.ptr = sv + l.normal;
                    s.normal.stride = 32;
                }
                if (l.diffuse >= 0) {
                    s.diffuse.ptr = sv + l.diffuse;
                    s.diffuse.stride = 32;
                }
                if (l.specular >= 0) {
                    s.specular.ptr = sv + l.specular;
                    s.specular.stride = 32;
                }
                s.tex[0].ptr = sv + l.tex[0];
                s.tex[0].stride = 32;
                /* Clip-space positions, legacy lighting when lit. */
                DWORD op = D3DVOP_TRANSFORM | 0x60000000u;
                if (what == D3DPROCESSVERTICES_TRANSFORMLIGHT && dev->legacy_lit(fvf, 0))
                    op |= D3DVOP_LIGHT | 0x08000000u;
                q7_process_vertices(&dev->st, dev->pv_power, op, &s, n, (BYTE *)&tv[dest], D3DFVF_TLVERTEX, 32,
                                    (UINT)(nv - dest) * 32);
                for (DWORD k = 0; k < n; ++k)
                    screen[dest + k] = 0;
            }
            break;
        case d7::D3DOP_TEXTURELOAD:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DTEXTURELOAD *tlo = (const d7::D3DTEXTURELOAD *)(op + (size_t)i * isize);
                Q7Surf *d = (Q7Surf *)q7_handle_get(tlo->hDestTexture, Q7H_TEXTURE);
                Q7Surf *s = (Q7Surf *)q7_handle_get(tlo->hSrcTexture, Q7H_TEXTURE);
                if (d && s)
                    q7_texture_load(d->s, s->s);
            }
            break;
        case d7::D3DOP_EXIT:
            free(out.buf);
            return D3D_OK;
        case d7::D3DOP_BRANCHFORWARD:
            for (DWORD i = 0; i < count; ++i) {
                const d7::D3DBRANCH *b = (const d7::D3DBRANCH *)(op + (size_t)i * isize);
                bool match = (data.dsStatus.dwStatus & b->dwMask) == b->dwValue;
                if (match != (b->bNegate != 0) && b->dwOffset) {
                    next = ip + b->dwOffset;
                    break;
                }
            }
            break;
        case d7::D3DOP_SETSTATUS:
            for (DWORD i = 0; i < count; ++i)
                data.dsStatus = *(const d7::D3DSTATUS *)(op + (size_t)i * isize);
            break;
        case d7::D3DOP_SPAN:
        default:
            /* SPAN (rasterizer spans of the DirectX 3 software devices) and
             * unknown instructions are skipped. */
            break;
        }
        if (next <= ip)
            break; /* a malformed stream must not loop */
        ip = next;
    }
    free(out.buf);
    return D3D_OK;
}
