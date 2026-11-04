/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: IDirectDrawClipper.  A window clipper clips to the window's visible
 * client area in screen coordinates (GetRandomRgn SYSRGN of its DC, as
 * Wine does); a clip list is used as given, in the destination surface's
 * coordinates.
 *
 * Native validates the object in every method (ddraw7.c
 * test_clipper_refcount): a pointer that is no clipper (freed, fake,
 * 0xdeadbeef) gets 0 from AddRef/Release and DDERR_INVALIDPARAMS from the
 * other methods without touching their arguments; with a damaged vtable
 * the methods still work, except Release, which does nothing.  Validity
 * comes from the interface registry, never from dereferencing.
 */
#include "qdd_priv.h"

static const void *g_clipper_vtbl; /* our vtable */

QddClipper *qdd_clipper_from(const void *iface)
{
    return iface ? (QddClipper *)qdd_reg_find(iface, QDD_T_CLIPPER, NULL) : NULL;
}

HRESULT qdd_clipper_create(DWORD flags, QddClipper **out, bool initialized)
{
    if (flags)
        return DDERR_INVALIDPARAMS;
    QddClipper *c = new QddClipper;
    if (!c)
        return DDERR_OUTOFMEMORY;
    c->iface.c = c;
    c->ref = 1;
    c->initialized = initialized;
    if (!g_clipper_vtbl)
        g_clipper_vtbl = *(const void *const *)(const void *)&c->iface;
    qdd_trace_vtbl(&c->iface, QDD_TK_CLIP, 1);
    if (!qdd_reg_add(&c->iface, c, QDD_T_CLIPPER, 0)) {
        delete c;
        return DDERR_OUTOFMEMORY;
    }
    *out = c;
    return DD_OK;
}

void qdd_clipper_addref(QddClipper *c)
{
    InterlockedIncrement(&c->ref);
}

/* Drops one reference of a clipper still registered; a destroyed one is
 * left alone (an application may have released the surface's reference). */
void qdd_clipper_release(QddClipper *c)
{
    QddLock lock;
    if (qdd_clipper_from(&c->iface) != c || c->ref <= 0)
        return;
    if (InterlockedDecrement(&c->ref))
        return;
    IUnknown *dd_ref = c->dd_ref;
    qdd_reg_remove(&c->iface);
    if (c->region)
        DeleteObject(c->region);
    delete c;
    if (dd_ref)
        dd_ref->Release();
}

HRGN qdd_clipper_region(QddClipper *c, bool *none)
{
    *none = false;
    if (c->window) {
        if (!IsWindow(c->window))
            return NULL;
        HDC dc = GetDC(c->window);
        if (!dc)
            return NULL;
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        if (rgn && GetRandomRgn(dc, rgn, SYSRGN) != 1) {
            DeleteObject(rgn);
            rgn = NULL;
        }
        ReleaseDC(c->window, dc);
        return rgn;
    }
    if (!c->region) {
        *none = true;
        return NULL;
    }
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn && CombineRgn(rgn, c->region, NULL, RGN_COPY) == ERROR) {
        DeleteObject(rgn);
        rgn = NULL;
    }
    return rgn;
}

/* The registered clipper behind this, else DDERR_INVALIDPARAMS (this is
 * never dereferenced first). */
#define QDD_CLIPPER_CHECK()                     \
    QddLock lock;                               \
    QddClipper *cl = qdd_clipper_from(this);    \
    if (!cl)                                    \
        return DDERR_INVALIDPARAMS

HRESULT QddClipperIface::QueryInterface(REFIID riid, void **out)
{
    QDD_CLIPPER_CHECK();
    if (!out)
        return DDERR_INVALIDPARAMS;
    if (qdd_iid_null(&riid))
        return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, IID_IDirectDrawClipper) || IsEqualGUID(riid, IID_IUnknown)) {
        qdd_clipper_addref(cl);
        *out = this;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

ULONG QddClipperIface::AddRef()
{
    QddLock lock;
    QddClipper *obj = qdd_clipper_from(this);
    if (!obj)
        return 0;
    return (ULONG)InterlockedIncrement(&obj->ref);
}

ULONG QddClipperIface::Release()
{
    QddLock lock;
    QddClipper *obj = qdd_clipper_from(this);
    if (!obj || *(const void *const *)(const void *)this != g_clipper_vtbl || obj->ref <= 0)
        return 0;
    LONG r = obj->ref - 1;
    qdd_clipper_release(obj);
    return (ULONG)r;
}

HRESULT QddClipperIface::GetClipList(RECT *rect, RGNDATA *data, DWORD *size)
{
    QDD_CLIPPER_CHECK();
    if (!size)
        return DDERR_INVALIDPARAMS;
    bool none;
    HRGN rgn = qdd_clipper_region(cl, &none);
    if (!rgn)
        return none ? DDERR_NOCLIPLIST : E_FAIL;
    if (rect) {
        HRGN r = CreateRectRgnIndirect(rect);
        if (!r) {
            DeleteObject(rgn);
            return E_FAIL;
        }
        CombineRgn(rgn, rgn, r, RGN_AND);
        DeleteObject(r);
    }
    DWORD need = GetRegionData(rgn, 0, NULL);
    if (data) {
        if (*size < need) {
            DeleteObject(rgn);
            *size = need;
            return DDERR_REGIONTOOSMALL;
        }
        need = GetRegionData(rgn, *size, data);
    }
    *size = need;
    DeleteObject(rgn);
    return DD_OK;
}

HRESULT QddClipperIface::GetHWnd(HWND *wnd)
{
    QDD_CLIPPER_CHECK();
    if (!wnd)
        return DDERR_INVALIDPARAMS;
    *wnd = cl->window;
    return DD_OK;
}

HRESULT QddClipperIface::Initialize(IDirectDraw *ddraw, DWORD flags)
{
    QDD_CLIPPER_CHECK();
    if (cl->initialized)
        return DDERR_ALREADYINITIALIZED;
    cl->initialized = true;
    return DD_OK;
}

HRESULT QddClipperIface::IsClipListChanged(WINBOOL *changed)
{
    QDD_CLIPPER_CHECK();
    if (!changed)
        return DDERR_INVALIDPARAMS;
    *changed = FALSE;
    return DD_OK;
}

HRESULT QddClipperIface::SetClipList(RGNDATA *data, DWORD flags)
{
    QDD_CLIPPER_CHECK();
    if (cl->window)
        return DDERR_CLIPPERISUSINGHWND;
    if (cl->region)
        DeleteObject(cl->region);
    cl->region = NULL;
    if (data) {
        DWORD size = sizeof(RGNDATAHEADER) + data->rdh.nCount * sizeof(RECT);
        if (!(cl->region = ExtCreateRegion(NULL, size, data)))
            return E_FAIL;
    }
    return DD_OK;
}

HRESULT QddClipperIface::SetHWnd(DWORD flags, HWND wnd)
{
    QDD_CLIPPER_CHECK();
    if (flags)
        return DDERR_INVALIDPARAMS;
    cl->window = wnd;
    return DD_OK;
}
