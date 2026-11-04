/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: resources, surfaces, volumes and swap chains on D3D9, and the
 * object lifetime they share (Q8Object in qd3d8.h).
 *
 * Identity: one wrapper per D3D9 object, reached again only through what
 * holds it (the device's binding slots and owner table, state blocks, a
 * texture's level table, a swap chain's owner table).  Get* answers from
 * those; nothing maps a D3D9 object back to its wrapper.  Interfaces the
 * application passes in are mapped to wrappers by pointer through the
 * registry below, like the native runtime, which never calls them.
 */
#include "qd3d8.h"

/* ------------------------------------------------------------------------ */
/* Registry: interface pointer -> wrapper (open addressing, linear probing,
 * backward-shift deletion), guarded by g_q8_lock.                           */

struct Q8RegSlot {
    const void *key;
    Q8Object *obj;
};

static Q8RegSlot *g_reg;
static UINT g_reg_cap; /* power of two, or 0 */
static UINT g_reg_count;

static UINT q8_reg_hash(const void *p)
{
    UINT v = (UINT)(UINT_PTR)p;
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    return v;
}

static void q8_reg_put(Q8RegSlot *slots, UINT cap, const void *key, Q8Object *obj)
{
    UINT i = q8_reg_hash(key) & (cap - 1);
    while (slots[i].key)
        i = (i + 1) & (cap - 1);
    slots[i].key = key;
    slots[i].obj = obj;
}

bool q8_register(Q8Object *o, DWORD pool)
{
    const void *key = o->iface();
    EnterCriticalSection(&g_q8_lock);
    if ((g_reg_count + 1) * 2 > g_reg_cap) {
        UINT cap = g_reg_cap ? g_reg_cap * 2 : 256;
        Q8RegSlot *n = cap < 0x10000000u ? (Q8RegSlot *)calloc(cap, sizeof(*n)) : NULL;
        if (!n) {
            LeaveCriticalSection(&g_q8_lock);
            return false;
        }
        for (UINT i = 0; i < g_reg_cap; ++i)
            if (g_reg[i].key)
                q8_reg_put(n, cap, g_reg[i].key, g_reg[i].obj);
        free(g_reg);
        g_reg = n;
        g_reg_cap = cap;
    }
    q8_reg_put(g_reg, g_reg_cap, key, o);
    ++g_reg_count;
    o->key = key;
    o->pool = o->container ? o->container->pool : pool;
    /* A level lives and dies with its container, which is counted. */
    o->counted = !o->container && pool == D3DPOOL_DEFAULT;
    LeaveCriticalSection(&g_q8_lock);
    if (o->counted)
        InterlockedIncrement(&o->device->default_objects);
    return true;
}

static void q8_unregister(const void *key)
{
    EnterCriticalSection(&g_q8_lock);
    UINT mask = g_reg_cap - 1, i = q8_reg_hash(key) & mask;
    while (g_reg[i].key && g_reg[i].key != key)
        i = (i + 1) & mask;
    if (g_reg[i].key) {
        /* Shift later members of the probe run back over the hole. */
        UINT hole = i;
        for (UINT j = (i + 1) & mask; g_reg[j].key; j = (j + 1) & mask) {
            UINT home = q8_reg_hash(g_reg[j].key) & mask;
            if (((j - home) & mask) >= ((j - hole) & mask)) {
                g_reg[hole] = g_reg[j];
                hole = j;
            }
        }
        g_reg[hole].key = NULL;
        g_reg[hole].obj = NULL;
        --g_reg_count;
    }
    LeaveCriticalSection(&g_q8_lock);
}

Q8Object *q8_lookup(const void *p, unsigned kinds)
{
    Q8Object *o = NULL;
    if (!p)
        return NULL;
    EnterCriticalSection(&g_q8_lock);
    if (g_reg_cap) {
        UINT mask = g_reg_cap - 1, i = q8_reg_hash(p) & mask;
        while (g_reg[i].key && g_reg[i].key != p)
            i = (i + 1) & mask;
        if (g_reg[i].key && (g_reg[i].obj->kind & kinds))
            o = g_reg[i].obj;
    }
    LeaveCriticalSection(&g_q8_lock);
    return o;
}

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                  */

Q8Object::Q8Object(Q8Device *dev, Q8Object *cont, unsigned k, IUnknown *o9)
    : refs(cont ? 0 : 1), holds(0), device(dev), container(cont), key(NULL), obj9(o9), kind(k),
      pool(D3DPOOL_DEFAULT), counted(false)
{
    /* The application's first reference comes with one on the device; a
     * level shares its container's. */
    if (!cont)
        dev->AddRef();
}

/* Destruction happens before the device's own end (objects that outlive
 * their last application reference are held by the device or by objects
 * that hold it), so device is still valid here. */
Q8Object::~Q8Object()
{
    if (key)
        q8_unregister(key);
    if (counted)
        InterlockedDecrement(&device->default_objects);
}

ULONG Q8Object::add_ref()
{
    Q8Object *o = container ? container : this;
    EnterCriticalSection(&g_q8_lock);
    LONG r = ++o->refs;
    if (r == 1)
        o->device->AddRef(); /* a held object the application had released */
    LeaveCriticalSection(&g_q8_lock);
    return (ULONG)r;
}

ULONG Q8Object::release()
{
    Q8Object *o = container ? container : this;
    EnterCriticalSection(&g_q8_lock);
    LONG r = o->refs;
    if (r <= 0) {
        /* The native runtime ignores Release at count 0. */
        LeaveCriticalSection(&g_q8_lock);
        return 0;
    }
    o->refs = --r;
    bool dead = r == 0 && o->holds == 0;
    LeaveCriticalSection(&g_q8_lock);
    if (r == 0) {
        /* The device reference goes now, even while something holds o. */
        Q8Device *dev = o->device;
        if (dead)
            o->destroy();
        dev->Release();
    }
    return (ULONG)r;
}

void q8_hold(Q8Object *o)
{
    if (o->container)
        o = o->container;
    EnterCriticalSection(&g_q8_lock);
    ++o->holds;
    LeaveCriticalSection(&g_q8_lock);
}

void q8_unhold(Q8Object *o)
{
    if (o->container)
        o = o->container;
    EnterCriticalSection(&g_q8_lock);
    bool dead = --o->holds == 0 && o->refs == 0;
    LeaveCriticalSection(&g_q8_lock);
    if (dead)
        o->destroy();
}

void q8_slot_set(Q8Object **slot, Q8Object *o)
{
    EnterCriticalSection(&g_q8_lock);
    Q8Object *old = *slot;
    if (old == o) {
        LeaveCriticalSection(&g_q8_lock);
        return;
    }
    if (o)
        q8_hold(o);
    *slot = o;
    LeaveCriticalSection(&g_q8_lock);
    if (old)
        q8_unhold(old);
}

Q8Object *q8_slot_ref(Q8Object *const *slot)
{
    EnterCriticalSection(&g_q8_lock);
    Q8Object *o = *slot;
    if (o)
        o->add_ref();
    LeaveCriticalSection(&g_q8_lock);
    return o;
}

/* ------------------------------------------------------------------------ */
/* Owned implicit surfaces                                                   */

static Q8Surface *q8_owned_find(Q8Table *owned, IDirect3DSurface9 *s)
{
    for (UINT i = 1; i <= owned->count; ++i) {
        Q8Surface *w = (Q8Surface *)q8_table_get(owned, i);
        if (w && w->d9 == s)
            return w;
    }
    return NULL;
}

Q8Surface *q8_implicit_surface(Q8Device *dev, Q8Table *owned, IDirect3DSurface9 *s)
{
    EnterCriticalSection(&g_q8_lock);
    Q8Surface *w = q8_owned_find(owned, s);
    if (w)
        w->add_ref();
    LeaveCriticalSection(&g_q8_lock);
    if (w) {
        s->Release();
        return w;
    }
    /* Made outside the lock (the wrapper reads its D3D9 description). */
    Q8Surface *made = q8_new<Q8Surface>(dev, s, D3DPOOL_DEFAULT);
    if (!made)
        return NULL;
    EnterCriticalSection(&g_q8_lock);
    w = q8_owned_find(owned, made->d9);
    if (w) {
        w->add_ref(); /* another thread wrapped it first */
    } else {
        w = made;
        made = NULL;
        if (q8_table_add(owned, w))
            q8_hold(w);
    }
    LeaveCriticalSection(&g_q8_lock);
    if (made)
        made->release();
    return w;
}

void q8_release_owned(Q8Table *owned)
{
    EnterCriticalSection(&g_q8_lock);
    Q8Table t = *owned;
    memset(owned, 0, sizeof(*owned));
    LeaveCriticalSection(&g_q8_lock);
    for (UINT i = 1; i <= t.count; ++i) {
        Q8Surface *w = (Q8Surface *)q8_table_get(&t, i);
        if (w)
            q8_unhold(w);
    }
    q8_table_free(&t);
}

/* ------------------------------------------------------------------------ */
/* Level tables                                                              */

/* The wrapper at index i with one new reference, or NULL if not made yet. */
static Q8Object *q8_levels_ref(Q8Levels *t, UINT i)
{
    EnterCriticalSection(&g_q8_lock);
    Q8Object *o = i < t->count ? t->items[i] : NULL;
    if (o)
        o->add_ref();
    LeaveCriticalSection(&g_q8_lock);
    return o;
}

/* Stores w at index i of a table of cap entries unless another thread got
 * there first, and returns the stored wrapper with one new reference (NULL
 * when out of memory).  Takes over w. */
static Q8Object *q8_levels_add(Q8Levels *t, UINT cap, UINT i, Q8Object *w)
{
    bool stored = false;
    EnterCriticalSection(&g_q8_lock);
    if (!t->items) {
        t->items = (Q8Object **)calloc(cap, sizeof(*t->items));
        t->count = t->items ? cap : 0;
    }
    Q8Object *o = i < t->count ? t->items[i] : NULL;
    if (!o && i < t->count) {
        t->items[i] = o = w;
        stored = true;
    }
    if (o)
        o->add_ref();
    LeaveCriticalSection(&g_q8_lock);
    if (!stored)
        w->destroy();
    return o;
}

void q8_levels_destroy(Q8Levels *t)
{
    for (UINT i = 0; i < t->count; ++i)
        if (t->items[i])
            t->items[i]->destroy();
    free(t->items);
    t->items = NULL;
    t->count = 0;
}

/* Level wrappers of cube textures follow the native lock rules of cube
 * faces (Q8SF_CHECKED); 2D texture levels keep D3D9's, which match. */
static void q8_level_init(Q8Surface *w, UINT i)
{
    if (w->container->kind == Q8K_CUBE) {
        w->flags = Q8SF_CHECKED;
        w->sub = i;
    }
}

static void q8_level_init(Q8Volume *, UINT)
{
}

/* Sub-resource i of container c (a table of cap entries) with one new
 * reference, made on first request; fetch obtains its D3D9 object. */
template <class W, class I, class S9, class F>
static HRESULT q8_get_level(Q8Object *c, Q8Levels *t, UINT cap, UINT i, F fetch, I **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (i >= cap)
        return D3DERR_INVALIDCALL;
    Q8Object *o = q8_levels_ref(t, i);
    if (!o) {
        S9 *s = NULL;
        HRESULT hr = fetch(&s);
        if (FAILED(hr))
            return hr;
        if (!s)
            return D3DERR_INVALIDCALL;
        W *w = new W(c->device, s, c);
        if (!w) {
            s->Release();
            return E_OUTOFMEMORY;
        }
        q8_level_init(w, i);
        if (!q8_register(w, c->pool)) {
            w->destroy();
            return E_OUTOFMEMORY;
        }
        o = q8_levels_add(t, cap, i, w);
        if (!o)
            return E_OUTOFMEMORY;
    }
    *out = q8_iface<I>(o);
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Unwrapping                                                                */

Q8Object *q8_texture(d8::IDirect3DBaseTexture8 *t, IDirect3DBaseTexture9 **t9)
{
    Q8Object *o = q8_lookup(t, Q8K_BASETEX);
    if (t9)
        *t9 = o ? static_cast<IDirect3DBaseTexture9 *>(o->obj9) : NULL;
    return o;
}

/* A level's container is its texture; any other surface's is the device. */
static HRESULT q8_get_container(Q8Object *self, REFIID riid, void **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (self->container)
        return self->container->iface()->QueryInterface(riid, out);
    return self->device->QueryInterface(riid, out);
}

/* ------------------------------------------------------------------------ */
/* Q8ResourceT / Q8BaseTextureT                                              */

template <class Base, class D9>
HRESULT STDMETHODCALLTYPE Q8ResourceT<Base, D9>::GetDevice(d8::IDirect3DDevice8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    this->device->AddRef();
    *out = this->device;
    return D3D_OK;
}

template <class Base, class D9>
HRESULT STDMETHODCALLTYPE Q8ResourceT<Base, D9>::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    return this->d9->SetPrivateData(guid, data, size, flags);
}

template <class Base, class D9>
HRESULT STDMETHODCALLTYPE Q8ResourceT<Base, D9>::GetPrivateData(REFGUID guid, void *data, DWORD *size)
{
    return this->d9->GetPrivateData(guid, data, size);
}

template <class Base, class D9>
HRESULT STDMETHODCALLTYPE Q8ResourceT<Base, D9>::FreePrivateData(REFGUID guid)
{
    return this->d9->FreePrivateData(guid);
}

template <class Base, class D9>
DWORD STDMETHODCALLTYPE Q8ResourceT<Base, D9>::SetPriority(DWORD p)
{
    return this->d9->SetPriority(p);
}

template <class Base, class D9>
DWORD STDMETHODCALLTYPE Q8ResourceT<Base, D9>::GetPriority()
{
    return this->d9->GetPriority();
}

template <class Base, class D9>
void STDMETHODCALLTYPE Q8ResourceT<Base, D9>::PreLoad()
{
    this->d9->PreLoad();
}

template <class Base, class D9>
d8::D3DRESOURCETYPE STDMETHODCALLTYPE Q8ResourceT<Base, D9>::GetType()
{
    return (d8::D3DRESOURCETYPE)this->d9->GetType();
}

template <class Base, class D9>
DWORD STDMETHODCALLTYPE Q8BaseTextureT<Base, D9>::SetLOD(DWORD lod)
{
    return this->d9->SetLOD(lod);
}

template <class Base, class D9>
DWORD STDMETHODCALLTYPE Q8BaseTextureT<Base, D9>::GetLOD()
{
    return this->d9->GetLOD();
}

template <class Base, class D9>
DWORD STDMETHODCALLTYPE Q8BaseTextureT<Base, D9>::GetLevelCount()
{
    return this->d9->GetLevelCount();
}


/* ------------------------------------------------------------------------ */

static void q8_surface_desc(const D3DSURFACE_DESC *in, d8::D3DSURFACE_DESC *out)
{
    out->Format = (d8::D3DFORMAT)in->Format;
    out->Type = (d8::D3DRESOURCETYPE)in->Type;
    out->Usage = in->Usage;
    out->Pool = (d8::D3DPOOL)in->Pool;
    out->Size = q8_surface_size(in->Format, in->Width, in->Height, 1);
    out->MultiSampleType = (d8::D3DMULTISAMPLE_TYPE)in->MultiSampleType;
    out->Width = in->Width;
    out->Height = in->Height;
}

static void q8_volume_desc(const D3DVOLUME_DESC *in, d8::D3DVOLUME_DESC *out)
{
    out->Format = (d8::D3DFORMAT)in->Format;
    out->Type = (d8::D3DRESOURCETYPE)in->Type;
    out->Usage = in->Usage;
    out->Pool = (d8::D3DPOOL)in->Pool;
    out->Size = q8_surface_size(in->Format, in->Width, in->Height, in->Depth);
    out->Width = in->Width;
    out->Height = in->Height;
    out->Depth = in->Depth;
}

#define Q8_QI_BEGIN(obj)                       \
    if (!(obj))                                \
        return E_POINTER;                      \
    *(obj) = NULL;
#define Q8_QI_MATCH(riid, iid, obj, ptr)       \
    if (IsEqualGUID(riid, iid)) {              \
        AddRef();                              \
        *(obj) = (ptr);                        \
        return S_OK;                           \
    }

/* ------------------------------------------------------------------------ */
/* Surface                                                                   */

/* Native lock rules (measured on the stock d3d8.dll): image surfaces, cube
 * map faces and device surfaces (Q8SF_CHECKED) take only a rectangle inside
 * the surface that is not empty, fail a second lock and the unlock of an
 * unlocked surface, and clear the output of a failed lock.  2D texture
 * levels check none of this, like D3D9. */
static bool q8_rect_inside(const RECT *r, UINT w, UINT h)
{
    return !r || (r->left >= 0 && r->top >= 0 && r->left < r->right && r->top < r->bottom &&
                  (UINT)r->right <= w && (UINT)r->bottom <= h);
}

static void q8_lock_failed(D3DLOCKED_RECT *lr)
{
    if (lr) {
        lr->pBits = NULL;
        lr->Pitch = 0;
    }
}

Q8Surface::Q8Surface(Q8Device *dev, IDirect3DSurface9 *s, Q8Object *cont)
    : Q8Child(dev, s, cont), flags(cont ? 0u : Q8SF_CHECKED), sub(0), locked(false)
{
    if (FAILED(s->GetDesc(&desc9)))
        memset(&desc9, 0, sizeof(desc9));
}

HRESULT STDMETHODCALLTYPE Q8Surface::QueryInterface(REFIID riid, void **obj)
{
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, static_cast<d8::IDirect3DSurface8 *>(this));
    Q8_QI_MATCH(riid, QIID_IDirect3DSurface8, obj, static_cast<d8::IDirect3DSurface8 *>(this));
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8Surface::GetDevice(d8::IDirect3DDevice8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    device->AddRef();
    *out = device;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Surface::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    return d9->SetPrivateData(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE Q8Surface::GetPrivateData(REFGUID guid, void *data, DWORD *size)
{
    return d9->GetPrivateData(guid, data, size);
}

HRESULT STDMETHODCALLTYPE Q8Surface::FreePrivateData(REFGUID guid)
{
    return d9->FreePrivateData(guid);
}

HRESULT STDMETHODCALLTYPE Q8Surface::GetContainer(REFIID riid, void **out)
{
    return q8_get_container(this, riid, out);
}

HRESULT STDMETHODCALLTYPE Q8Surface::GetDesc(d8::D3DSURFACE_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    q8_surface_desc(&desc9, desc);
    if (flags & Q8SF_IMAGE) {
        /* D3D8 image surfaces are SYSTEMMEM whatever pool holds them here
         * (formats the device cannot use live in SCRATCH). */
        desc->Pool = d8::D3DPOOL_SYSTEMMEM;
        desc->Usage = 0;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Surface::LockRect(d8::D3DLOCKED_RECT *lr8, const RECT *rect, DWORD lock_flags)
{
    D3DLOCKED_RECT *lr = (D3DLOCKED_RECT *)lr8;
    if (!(flags & Q8SF_CHECKED))
        return d9->LockRect(lr, rect, lock_flags);
    if (container)
        return static_cast<Q8CubeTexture *>(container)->lock_face(sub / Q8_MAX_LEVELS, sub % Q8_MAX_LEVELS, lr,
                                                                    rect, lock_flags);
    if (locked || !q8_rect_inside(rect, desc9.Width, desc9.Height)) {
        q8_lock_failed(lr);
        return D3DERR_INVALIDCALL;
    }
    HRESULT hr = d9->LockRect(lr, rect, lock_flags);
    if (FAILED(hr))
        q8_lock_failed(lr);
    else
        locked = true;
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Surface::UnlockRect()
{
    if (!(flags & Q8SF_CHECKED))
        return d9->UnlockRect();
    if (container)
        return static_cast<Q8CubeTexture *>(container)->unlock_face(sub / Q8_MAX_LEVELS, sub % Q8_MAX_LEVELS);
    if (!locked)
        return D3DERR_INVALIDCALL;
    HRESULT hr = d9->UnlockRect();
    if (SUCCEEDED(hr))
        locked = false;
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Volume                                                                    */

HRESULT STDMETHODCALLTYPE Q8Volume::QueryInterface(REFIID riid, void **obj)
{
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, static_cast<d8::IDirect3DVolume8 *>(this));
    Q8_QI_MATCH(riid, QIID_IDirect3DVolume8, obj, static_cast<d8::IDirect3DVolume8 *>(this));
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8Volume::GetDevice(d8::IDirect3DDevice8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    device->AddRef();
    *out = device;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Q8Volume::SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    return d9->SetPrivateData(guid, data, size, flags);
}

HRESULT STDMETHODCALLTYPE Q8Volume::GetPrivateData(REFGUID guid, void *data, DWORD *size)
{
    return d9->GetPrivateData(guid, data, size);
}

HRESULT STDMETHODCALLTYPE Q8Volume::FreePrivateData(REFGUID guid)
{
    return d9->FreePrivateData(guid);
}

HRESULT STDMETHODCALLTYPE Q8Volume::GetContainer(REFIID riid, void **out)
{
    return q8_get_container(this, riid, out);
}

HRESULT STDMETHODCALLTYPE Q8Volume::GetDesc(d8::D3DVOLUME_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC d;
    HRESULT hr = d9->GetDesc(&d);
    if (SUCCEEDED(hr))
        q8_volume_desc(&d, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Volume::LockBox(d8::D3DLOCKED_BOX *lb, const d8::D3DBOX *box, DWORD flags)
{
    return d9->LockBox((D3DLOCKED_BOX *)lb, (const D3DBOX *)box, flags);
}

HRESULT STDMETHODCALLTYPE Q8Volume::UnlockBox()
{
    return d9->UnlockBox();
}

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */

HRESULT STDMETHODCALLTYPE Q8Texture::QueryInterface(REFIID riid, void **obj)
{
    d8::IDirect3DTexture8 *self = this;
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DResource8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DBaseTexture8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DTexture8, obj, self);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8Texture::GetLevelDesc(UINT level, d8::D3DSURFACE_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d;
    HRESULT hr = d9->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
        q8_surface_desc(&d, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8Texture::GetSurfaceLevel(UINT level, d8::IDirect3DSurface8 **out)
{
    return q8_get_level<Q8Surface, d8::IDirect3DSurface8, IDirect3DSurface9>(
        this, &levels, Q8_MAX_LEVELS, level,
        [this, level](IDirect3DSurface9 **s) { return d9->GetSurfaceLevel(level, s); }, out);
}

HRESULT STDMETHODCALLTYPE Q8Texture::LockRect(UINT level, d8::D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    return d9->LockRect(level, (D3DLOCKED_RECT *)lr, rect, flags);
}

HRESULT STDMETHODCALLTYPE Q8Texture::UnlockRect(UINT level)
{
    return d9->UnlockRect(level);
}

HRESULT STDMETHODCALLTYPE Q8Texture::AddDirtyRect(const RECT *rect)
{
    return d9->AddDirtyRect(rect);
}

Q8CubeTexture::Q8CubeTexture(Q8Device *dev, IDirect3DCubeTexture9 *t) : Q8BaseTextureT(dev, t), edge(0), locked()
{
    D3DSURFACE_DESC d;
    if (SUCCEEDED(t->GetLevelDesc(0, &d)))
        edge = d.Width;
}

HRESULT Q8CubeTexture::lock_face(UINT face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    UINT size = level < 32 ? edge >> level : 0;
    if (face >= 6 || level >= Q8_MAX_LEVELS || (locked[face] & (1u << level)) ||
        !q8_rect_inside(rect, size ? size : 1, size ? size : 1)) {
        q8_lock_failed(lr);
        return D3DERR_INVALIDCALL;
    }
    HRESULT hr = d9->LockRect((D3DCUBEMAP_FACES)face, level, lr, rect, flags);
    if (FAILED(hr))
        q8_lock_failed(lr);
    else
        locked[face] = (WORD)(locked[face] | (1u << level));
    return hr;
}

HRESULT Q8CubeTexture::unlock_face(UINT face, UINT level)
{
    if (face >= 6 || level >= Q8_MAX_LEVELS || !(locked[face] & (1u << level)))
        return D3DERR_INVALIDCALL;
    HRESULT hr = d9->UnlockRect((D3DCUBEMAP_FACES)face, level);
    if (SUCCEEDED(hr))
        locked[face] = (WORD)(locked[face] & ~(1u << level));
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::QueryInterface(REFIID riid, void **obj)
{
    d8::IDirect3DCubeTexture8 *self = this;
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DResource8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DBaseTexture8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DCubeTexture8, obj, self);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::GetLevelDesc(UINT level, d8::D3DSURFACE_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC d;
    HRESULT hr = d9->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
        q8_surface_desc(&d, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::GetCubeMapSurface(d8::D3DCUBEMAP_FACES face, UINT level,
                                                           d8::IDirect3DSurface8 **out)
{
    const UINT cap = 6 * Q8_MAX_LEVELS;
    UINT i = (UINT)face < 6 && level < Q8_MAX_LEVELS ? (UINT)face * Q8_MAX_LEVELS + level : cap;
    return q8_get_level<Q8Surface, d8::IDirect3DSurface8, IDirect3DSurface9>(
        this, &levels, cap, i,
        [this, face, level](IDirect3DSurface9 **s) { return d9->GetCubeMapSurface((D3DCUBEMAP_FACES)face, level, s); },
        out);
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::LockRect(d8::D3DCUBEMAP_FACES face, UINT level, d8::D3DLOCKED_RECT *lr,
                                                  const RECT *rect, DWORD flags)
{
    return lock_face((UINT)face, level, (D3DLOCKED_RECT *)lr, rect, flags);
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::UnlockRect(d8::D3DCUBEMAP_FACES face, UINT level)
{
    return unlock_face((UINT)face, level);
}

HRESULT STDMETHODCALLTYPE Q8CubeTexture::AddDirtyRect(d8::D3DCUBEMAP_FACES face, const RECT *rect)
{
    return d9->AddDirtyRect((D3DCUBEMAP_FACES)face, rect);
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::QueryInterface(REFIID riid, void **obj)
{
    d8::IDirect3DVolumeTexture8 *self = this;
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DResource8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DBaseTexture8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DVolumeTexture8, obj, self);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::GetLevelDesc(UINT level, d8::D3DVOLUME_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DVOLUME_DESC d;
    HRESULT hr = d9->GetLevelDesc(level, &d);
    if (SUCCEEDED(hr))
        q8_volume_desc(&d, desc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::GetVolumeLevel(UINT level, d8::IDirect3DVolume8 **out)
{
    return q8_get_level<Q8Volume, d8::IDirect3DVolume8, IDirect3DVolume9>(
        this, &levels, Q8_MAX_LEVELS, level,
        [this, level](IDirect3DVolume9 **v) { return d9->GetVolumeLevel(level, v); }, out);
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::LockBox(UINT level, d8::D3DLOCKED_BOX *lb, const d8::D3DBOX *box,
                                                   DWORD flags)
{
    return d9->LockBox(level, (D3DLOCKED_BOX *)lb, (const D3DBOX *)box, flags);
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::UnlockBox(UINT level)
{
    return d9->UnlockBox(level);
}

HRESULT STDMETHODCALLTYPE Q8VolumeTexture::AddDirtyBox(const d8::D3DBOX *box)
{
    return d9->AddDirtyBox((const D3DBOX *)box);
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                   */

/* D3D8 ignores DISCARD/NOOVERWRITE on static buffers; D3D9 rejects them in
 * the debug runtime and may mishandle them in drivers. */
static DWORD q8_buffer_lock_flags(DWORD usage, DWORD flags)
{
    if (!(usage & D3DUSAGE_DYNAMIC))
        flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    return flags;
}

Q8VertexBuffer::Q8VertexBuffer(Q8Device *dev, IDirect3DVertexBuffer9 *b) : Q8ResourceT(dev, b), usage(0)
{
    D3DVERTEXBUFFER_DESC d;
    if (SUCCEEDED(b->GetDesc(&d)))
        usage = d.Usage;
}

HRESULT STDMETHODCALLTYPE Q8VertexBuffer::QueryInterface(REFIID riid, void **obj)
{
    d8::IDirect3DVertexBuffer8 *self = this;
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DResource8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DVertexBuffer8, obj, self);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8VertexBuffer::Lock(UINT offset, UINT size, BYTE **data, DWORD flags)
{
    return d9->Lock(offset, size, (void **)data, q8_buffer_lock_flags(usage, flags));
}

HRESULT STDMETHODCALLTYPE Q8VertexBuffer::Unlock()
{
    return d9->Unlock();
}

HRESULT STDMETHODCALLTYPE Q8VertexBuffer::GetDesc(d8::D3DVERTEXBUFFER_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DVERTEXBUFFER_DESC d;
    HRESULT hr = d9->GetDesc(&d);
    if (FAILED(hr))
        return hr;
    desc->Format = (d8::D3DFORMAT)d.Format;
    desc->Type = (d8::D3DRESOURCETYPE)d.Type;
    desc->Usage = d.Usage;
    desc->Pool = (d8::D3DPOOL)d.Pool;
    desc->Size = d.Size;
    desc->FVF = d.FVF;
    return D3D_OK;
}

Q8IndexBuffer::Q8IndexBuffer(Q8Device *dev, IDirect3DIndexBuffer9 *b) : Q8ResourceT(dev, b), usage(0)
{
    D3DINDEXBUFFER_DESC d;
    if (SUCCEEDED(b->GetDesc(&d)))
        usage = d.Usage;
}

HRESULT STDMETHODCALLTYPE Q8IndexBuffer::QueryInterface(REFIID riid, void **obj)
{
    d8::IDirect3DIndexBuffer8 *self = this;
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DResource8, obj, self);
    Q8_QI_MATCH(riid, QIID_IDirect3DIndexBuffer8, obj, self);
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8IndexBuffer::Lock(UINT offset, UINT size, BYTE **data, DWORD flags)
{
    return d9->Lock(offset, size, (void **)data, q8_buffer_lock_flags(usage, flags));
}

HRESULT STDMETHODCALLTYPE Q8IndexBuffer::Unlock()
{
    return d9->Unlock();
}

HRESULT STDMETHODCALLTYPE Q8IndexBuffer::GetDesc(d8::D3DINDEXBUFFER_DESC *desc)
{
    if (!desc)
        return D3DERR_INVALIDCALL;
    D3DINDEXBUFFER_DESC d;
    HRESULT hr = d9->GetDesc(&d);
    if (FAILED(hr))
        return hr;
    desc->Format = (d8::D3DFORMAT)d.Format;
    desc->Type = (d8::D3DRESOURCETYPE)d.Type;
    desc->Usage = d.Usage;
    desc->Pool = (d8::D3DPOOL)d.Pool;
    desc->Size = d.Size;
    return D3D_OK;
}

/* ------------------------------------------------------------------------ */
/* Swap chain (additional swap chains only; D3D8 exposes no GetSwapChain).    */

/* Its back buffers go with it unless the application still references
 * them; those stay usable on their own (as natively). */
void Q8SwapChain::destroy()
{
    q8_release_owned(&owned);
    d9->Release();
    delete this;
}

HRESULT STDMETHODCALLTYPE Q8SwapChain::QueryInterface(REFIID riid, void **obj)
{
    Q8_QI_BEGIN(obj);
    Q8_QI_MATCH(riid, __uuidof(IUnknown), obj, static_cast<d8::IDirect3DSwapChain8 *>(this));
    Q8_QI_MATCH(riid, QIID_IDirect3DSwapChain8, obj, static_cast<d8::IDirect3DSwapChain8 *>(this));
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE Q8SwapChain::Present(const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (device->needs_reset)
        return D3DERR_DEVICELOST;
    if (!IsWindow(wnd ? wnd : window))
        return D3DERR_INVALIDCALL;
    HRESULT hr = d9->Present(src, dst, wnd, dirty, 0);
    return SUCCEEDED(hr) ? D3D_OK : hr;
}

/* A failed GetBackBuffer leaves *out as it was (Wine's test_swapchain on the
 * stock runtime). */
HRESULT STDMETHODCALLTYPE Q8SwapChain::GetBackBuffer(UINT index, d8::D3DBACKBUFFER_TYPE type,
                                                     d8::IDirect3DSurface8 **out)
{
    if (!out)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = d9->GetBackBuffer(index, (D3DBACKBUFFER_TYPE)type, &s);
    if (FAILED(hr))
        return hr;
    *out = q8_implicit_surface(device, &owned, s);
    return *out ? D3D_OK : E_OUTOFMEMORY;
}
