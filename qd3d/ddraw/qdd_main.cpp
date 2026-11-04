/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: DLL entry, exports, runtime selection, logging, the process lock and
 * the interface registry.
 *
 * Runtime selection.  Every DirectDraw creation (DirectDrawCreate,
 * DirectDrawCreateEx, the class factory) asks d3d9.dll for an IDirect3D9:
 * a QD3D proxy the process already has (a game's own copy), else the one
 * in this DLL's own directory (installed: the QD3D proxy in System32; in a
 * test directory the proxy placed beside us), else the system copy.  When
 * that object does not answer IID_QD3D_REMOTE (no host,
 * every slot busy, ...) the call, and every later export, goes to the stock
 * DirectDraw runtime: System32\qddraw_orig.dll when present (the
 * system-wide installer keeps the stock DLL under that name), else
 * System32\ddraw.dll (this DLL is a per-game copy).  A QD3D copy or this
 * module itself is refused as the stock runtime.  QDDRAW_TEST_SYSTEM_DIR
 * names a directory that stands for System32 in tests.
 *
 * The internal exports other system DLLs use (dsound's DSoundHelp,
 * d3dim/ddrawex helpers) are forwarded to the stock runtime at run time.
 */
#include "qdd_priv.h"
#include <stdio.h>
#include <stdarg.h>

CRITICAL_SECTION g_qdd_cs;
HMODULE g_qdd_self;
QddEnv g_qdd_env;

static HMODULE g_d3d9_module;
static bool g_d3d9_logged;
static HMODULE g_stock;          /* the stock runtime once loaded */
static bool g_stock_tried;
static bool g_fallback;          /* a creation went to the stock runtime */
static bool g_frontend_logged;
static LONG g_objects;           /* live COM objects of ours (DllCanUnloadNow) */

const GUID QDD_IID_IDirect3D = {0x3bba0080, 0x2421, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
const GUID QDD_IID_IDirect3D2 = {0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}};
const GUID QDD_IID_IDirect3D3 = {0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}};
const GUID QDD_IID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID QDD_IID_IDirectDraw3 = {0x618f8ad4, 0x8b7a, 0x11d0, {0x8f, 0xcc, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};

/* ------------------------------------------------------------------------ */
/* Freestanding C++ runtime: no libstdc++, no exceptions.                    */

void *operator new(size_t n) { return calloc(1, n ? n : 1); }
void *operator new[](size_t n) { return calloc(1, n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual(void) { abort(); }

/* ------------------------------------------------------------------------ */
/* Lock, logging                                                             */

void qdd_lock(void) { EnterCriticalSection(&g_qdd_cs); }
void qdd_unlock(void) { LeaveCriticalSection(&g_qdd_cs); }

DWORD qdd_tick_ms(void)
{
    return GetTickCount();
}

static int qdd_vformat(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n = _vsnprintf(buf, size - 1u, fmt, ap);
    if (n < 0 || (size_t)n > size - 1u)
        n = (int)(size - 1u);
    buf[n] = 0;
    return n;
}

static void qdd_log_line(const char *msg)
{
    char path[MAX_PATH], line[600];
    int n = _snprintf(line, sizeof(line) - 3, "%s", msg);
    if (n < 0 || n > (int)sizeof(line) - 3)
        n = (int)sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    OutputDebugStringA(line);
    DWORD len = GetEnvironmentVariableA("QDDRAW_LOG", path, sizeof(path));
    if (len > 0 && len < sizeof(path)) {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(f, line, (DWORD)n, &written, NULL);
            CloseHandle(f);
        }
    }
}

void qdd_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    qdd_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qdd_log_line(buf);
}

/* One line "YYYY-MM-DD hh:mm:ss pid exe: msg" appended to %TEMP%\qd3d.log
 * (the proxy's field log), emptied once past 1 MiB. */
static void qdd_field_log(const char *msg)
{
    static const char name[] = "qd3d.log";
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    char path[MAX_PATH + sizeof(name) + 1], exe[MAX_PATH], line[640];
    const char *base = exe;
    SYSTEMTIME t;
    DWORD n, hi = 0, wr;

    n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH)
        return;
    if (path[n - 1] != '\\')
        path[n++] = '\\';
    memcpy(path + n, name, sizeof(name));
    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n == 0 || n >= sizeof(exe))
        memcpy(exe, "?", 2);
    for (const char *p = exe; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    GetLocalTime(&t);
    int len = _snprintf(line, sizeof(line) - 3, "%04u-%02u-%02u %02u:%02u:%02u %lu %s: %s", (unsigned)t.wYear,
                        (unsigned)t.wMonth, (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute,
                        (unsigned)t.wSecond, (unsigned long)GetCurrentProcessId(), base, msg);
    if (len < 0 || len > (int)sizeof(line) - 3)
        len = (int)sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, share, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSize(f, &hi) > 1024u * 1024u || hi) {
        CloseHandle(f);
        f = CreateFileA(path, GENERIC_WRITE, share, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE)
            return;
    }
    WriteFile(f, line, (DWORD)len, &wr, NULL);
    CloseHandle(f);
}

void qdd_event(const char *fmt, ...)
{
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    qdd_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qdd_log_line(buf);
    qdd_field_log(buf);
}

/* ------------------------------------------------------------------------ */
/* Interface registry: open addressing on the pointer value.                 */

struct QddRegEntry {
    const void *key; /* NULL: empty; (void *)1: deleted */
    void *obj;
    unsigned type;
    int vidx;
};
static QddRegEntry *g_reg;
static UINT g_reg_cap, g_reg_used, g_reg_live;

#define QDD_REG_TOMB ((const void *)1)

static UINT qdd_reg_hash(const void *p)
{
    UINT_PTR v = (UINT_PTR)p;
    v ^= v >> 13;
    v *= 0x9e3779b1u;
    return (UINT)(v ^ (v >> 16));
}

static bool qdd_reg_grow(void)
{
    UINT cap = g_reg_cap ? g_reg_cap * 2 : 256;
    if (g_reg_live * 2 < g_reg_cap)
        cap = g_reg_cap; /* only tombstones: rehash in place size */
    QddRegEntry *n = (QddRegEntry *)calloc(cap, sizeof(QddRegEntry));
    if (!n)
        return false;
    for (UINT i = 0; i < g_reg_cap; ++i) {
        const QddRegEntry *e = &g_reg[i];
        if (!e->key || e->key == QDD_REG_TOMB)
            continue;
        UINT h = qdd_reg_hash(e->key) & (cap - 1);
        while (n[h].key)
            h = (h + 1) & (cap - 1);
        n[h] = *e;
    }
    free(g_reg);
    g_reg = n;
    g_reg_cap = cap;
    g_reg_used = g_reg_live;
    return true;
}

bool qdd_reg_add(const void *p, void *obj, unsigned type, int vidx)
{
    QddLock lock;
    if ((g_reg_used + 1) * 4 >= g_reg_cap * 3 && !qdd_reg_grow())
        return false;
    UINT h = qdd_reg_hash(p) & (g_reg_cap - 1);
    while (g_reg[h].key && g_reg[h].key != QDD_REG_TOMB)
        h = (h + 1) & (g_reg_cap - 1);
    if (!g_reg[h].key)
        g_reg_used++;
    g_reg[h].key = p;
    g_reg[h].obj = obj;
    g_reg[h].type = type;
    g_reg[h].vidx = vidx;
    g_reg_live++;
    InterlockedIncrement(&g_objects);
    return true;
}

static QddRegEntry *qdd_reg_slot(const void *p)
{
    if (!g_reg_cap || !p || p == QDD_REG_TOMB)
        return NULL;
    UINT h = qdd_reg_hash(p) & (g_reg_cap - 1);
    while (g_reg[h].key) {
        if (g_reg[h].key == p)
            return &g_reg[h];
        h = (h + 1) & (g_reg_cap - 1);
    }
    return NULL;
}

void qdd_reg_remove(const void *p)
{
    QddLock lock;
    QddRegEntry *e = qdd_reg_slot(p);
    if (e) {
        e->key = QDD_REG_TOMB;
        e->obj = NULL;
        g_reg_live--;
        InterlockedDecrement(&g_objects);
    }
}

void *qdd_reg_find(const void *p, unsigned type, int *vidx)
{
    QddLock lock;
    QddRegEntry *e = qdd_reg_slot(p);
    if (!e || e->type != type)
        return NULL;
    if (vidx)
        *vidx = e->vidx;
    return e->obj;
}

/* ------------------------------------------------------------------------ */
/* Runtime selection                                                         */

/* The system directory, or the one QDDRAW_TEST_SYSTEM_DIR names. */
static bool qdd_system_dir(char *dir, DWORD cap)
{
    DWORD n = GetEnvironmentVariableA("QDDRAW_TEST_SYSTEM_DIR", dir, cap);
    if (n > 0 && n < cap) {
        if (dir[n - 1] == '\\')
            dir[n - 1] = 0;
        return true;
    }
    n = GetSystemDirectoryA(dir, cap);
    return n > 0 && n < cap;
}

static bool qdd_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b + 1 > cap)
        return false;
    memcpy(out, dir, a);
    out[a] = '\\';
    memcpy(out + a + 1, name, b + 1);
    return true;
}

static IDirect3D9 *qdd_create_d3d9(char *why, size_t why_size)
{
    typedef IDirect3D9 *(WINAPI * PFN_Direct3DCreate9)(UINT);
    char path[MAX_PATH + 16];

    EnterCriticalSection(&g_qdd_cs);
    if (!g_d3d9_module) {
        /* A QD3D proxy the process already has (a game's own copy, loaded
         * before this front end) serves us too: loading another d3d9.dll by
         * path would start a second proxy, and each claims a channel slot. */
        HMODULE loaded = GetModuleHandleA("d3d9.dll");
        if (loaded && FindResourceA(loaded, "QD3D_MARKER", RT_RCDATA)) {
            DWORD k = GetModuleFileNameA(loaded, path, MAX_PATH);
            if (k > 0 && k < MAX_PATH)
                g_d3d9_module = LoadLibraryA(path);
        }
    }
    if (!g_d3d9_module) {
        DWORD m = GetModuleFileNameA(g_qdd_self, path, MAX_PATH);
        char *slash = (m > 0 && m < MAX_PATH) ? strrchr(path, '\\') : NULL;
        if (slash && (size_t)(slash - path) + sizeof("\\d3d9.dll") <= sizeof path) {
            memcpy(slash, "\\d3d9.dll", sizeof("\\d3d9.dll"));
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
                g_d3d9_module = LoadLibraryA(path);
        }
        if (!g_d3d9_module) {
            UINT n = GetSystemDirectoryA(path, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                memcpy(path + n, "\\d3d9.dll", sizeof("\\d3d9.dll"));
                g_d3d9_module = LoadLibraryA(path);
            }
        }
    }
    if (!g_d3d9_module) {
        _snprintf(why, why_size, "d3d9.dll not loadable, error %lu", GetLastError());
        why[why_size - 1] = 0;
    } else if (!g_d3d9_logged) {
        g_d3d9_logged = true;
        if (!GetModuleFileNameA(g_d3d9_module, path, MAX_PATH))
            path[0] = 0;
        path[MAX_PATH - 1] = 0;
        qdd_log("qddraw: loaded %s", path);
    }
    HMODULE mod = g_d3d9_module;
    LeaveCriticalSection(&g_qdd_cs);
    if (!mod)
        return NULL;
    PFN_Direct3DCreate9 create = (PFN_Direct3DCreate9)(void *)GetProcAddress(mod, "Direct3DCreate9");
    if (!create) {
        _snprintf(why, why_size, "d3d9.dll has no Direct3DCreate9");
        why[why_size - 1] = 0;
        return NULL;
    }
    IDirect3D9 *d = create(D3D_SDK_VERSION);
    if (!d) {
        _snprintf(why, why_size, "Direct3DCreate9 failed");
        why[why_size - 1] = 0;
    }
    return d;
}

static bool qdd_is_remote(IDirect3D9 *d3d9)
{
    void *p = NULL;
    if (!d3d9 || FAILED(d3d9->QueryInterface(IID_QD3D_REMOTE, &p)) || !p)
        return false;
    ((IUnknown *)p)->Release();
    return true;
}

/* The stock runtime (loaded once, never unloaded); called under the lock. */
static HMODULE qdd_stock_module(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 32];
    if (g_stock_tried)
        return g_stock;
    g_stock_tried = true;
    if (!qdd_system_dir(dir, sizeof(dir)))
        return NULL;
    static const char *const names[] = {"qddraw_orig.dll", "ddraw.dll"};
    for (UINT i = 0; i < 2; ++i) {
        if (!qdd_join(path, sizeof(path), dir, names[i]) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        HMODULE m = LoadLibraryA(path);
        if (!m) {
            qdd_log("qddraw: stock runtime %s not loadable, error %lu", path, GetLastError());
            continue;
        }
        if (m == g_qdd_self || FindResourceA(m, "QD3D_MARKER", RT_RCDATA) ||
            !GetProcAddress(m, "DirectDrawCreateEx")) {
            qdd_log("qddraw: %s is not the stock runtime; refused", path);
            if (m != g_qdd_self)
                FreeLibrary(m);
            continue;
        }
        g_stock = m;
        return m;
    }
    return NULL;
}

static FARPROC qdd_stock_export(const char *name)
{
    QddLock lock;
    HMODULE m = qdd_stock_module();
    return m ? GetProcAddress(m, name) : NULL;
}

/* Whether exports go to the stock runtime (a creation went there). */
static FARPROC qdd_fallback_export(const char *name)
{
    QddLock lock;
    return g_fallback && g_stock ? GetProcAddress(g_stock, name) : NULL;
}

/* Decides where a DirectDraw creation goes: true with *d3d9 for the front
 * end, false with *stock_fn (may be NULL: nothing usable) for the stock
 * runtime. */
static bool qdd_select(const char *call, IDirect3D9 **d3d9, FARPROC *stock_fn)
{
    char why[160] = "";
    *d3d9 = NULL;
    *stock_fn = NULL;
    {
        QddLock lock;
        if (g_fallback && g_stock) {
            *stock_fn = GetProcAddress(g_stock, call);
            return false;
        }
    }
    IDirect3D9 *d = qdd_create_d3d9(why, sizeof(why));
    if (d && qdd_is_remote(d)) {
        QddLock lock;
        if (!g_frontend_logged) {
            char used[MAX_PATH];
            g_frontend_logged = true;
            if (!g_d3d9_module || !GetModuleFileNameA(g_d3d9_module, used, MAX_PATH))
                used[0] = 0;
            used[MAX_PATH - 1] = 0;
            qdd_event("qddraw: QD3D session; %s -> QD3D front end on %s", call, used);
        }
        *d3d9 = d;
        return true;
    }
    if (d && !why[0])
        _snprintf(why, sizeof(why), "d3d9.dll gave no QD3D session");
    why[sizeof(why) - 1] = 0;
    QddLock lock;
    HMODULE stock = qdd_stock_module();
    if (stock) {
        char path[MAX_PATH];
        if (!GetModuleFileNameA(stock, path, sizeof(path)))
            path[0] = 0;
        path[MAX_PATH - 1] = 0;
        if (!g_fallback)
            qdd_event("qddraw: no QD3D session (%s); %s -> stock runtime %s", why, call, path);
        g_fallback = true;
        if (d)
            d->Release();
        *stock_fn = GetProcAddress(stock, call);
        return false;
    }
    /* Without a stock runtime the front end runs on whatever d3d9.dll gave. */
    if (d) {
        if (!g_frontend_logged) {
            g_frontend_logged = true;
            qdd_event("qddraw: no QD3D session (%s) and no stock runtime; %s -> front end on plain Direct3D 9",
                      why, call);
        }
        *d3d9 = d;
        return true;
    }
    qdd_event("qddraw: no QD3D session (%s) and no stock runtime; %s fails", why, call);
    return false;
}

/* ------------------------------------------------------------------------ */
/* Class factory                                                             */

static const GUID QDD_CLSID_DirectDraw = {0xd7b70ee0, 0x4340, 0x11cf, {0xb0, 0x63, 0x00, 0x20, 0xaf, 0xc2, 0xcd, 0x35}};
static const GUID QDD_CLSID_DirectDraw7 = {0x3c305196, 0x50db, 0x11d3, {0x9c, 0xfe, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5}};
static const GUID QDD_CLSID_DirectDrawClipper = {0x593817a0, 0x7db3, 0x11cf, {0xa2, 0xde, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
static const GUID QDD_IID_IClassFactory = {0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

class QddFactory final : public IClassFactory {
public:
    LONG refs;
    int kind; /* 0 DirectDraw, 1 DirectDraw7, 2 clipper */

    STDMETHOD(QueryInterface)(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, QDD_IID_IClassFactory)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            delete this;
            InterlockedDecrement(&g_objects);
        }
        return (ULONG)r;
    }
    STDMETHOD(CreateInstance)(IUnknown *outer, REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = NULL;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        if (kind == 2) {
            QddClipper *c;
            HRESULT hr = qdd_clipper_create(0, &c, false);
            if (FAILED(hr))
                return hr;
            hr = c->iface.QueryInterface(riid, out);
            c->iface.Release();
            return hr;
        }
        IDirect3D9 *d3d9;
        FARPROC stock;
        if (!qdd_select("DllGetClassObject", &d3d9, &stock)) {
            typedef HRESULT(WINAPI * PFN)(REFCLSID, REFIID, void **);
            PFN get = (PFN)(void *)stock;
            IClassFactory *cf = NULL;
            if (!get)
                return DDERR_NODIRECTDRAWSUPPORT;
            HRESULT hr = get(kind ? QDD_CLSID_DirectDraw7 : QDD_CLSID_DirectDraw, QDD_IID_IClassFactory, (void **)&cf);
            if (FAILED(hr))
                return hr;
            hr = cf->CreateInstance(outer, riid, out);
            cf->Release();
            return hr;
        }
        QddDraw *dd;
        HRESULT hr = qdd_draw_create(d3d9, kind == 1 || IsEqualGUID(riid, IID_IDirectDraw7), false, &dd);
        if (FAILED(hr))
            return hr;
        hr = qdd_draw_qi(dd, riid, out);
        qdd_draw_release(dd, 7);
        return hr;
    }
    STDMETHOD(LockServer)(WINBOOL lock) override
    {
        if (lock)
            InterlockedIncrement(&g_objects);
        else
            InterlockedDecrement(&g_objects);
        return S_OK;
    }
};

/* ------------------------------------------------------------------------ */
/* Exports                                                                   */

extern "C" HRESULT WINAPI DirectDrawCreate(GUID *guid, IDirectDraw **out, IUnknown *outer)
{
    IDirect3D9 *d3d9;
    FARPROC stock;
    if (!qdd_select("DirectDrawCreate", &d3d9, &stock)) {
        typedef HRESULT(WINAPI * PFN)(GUID *, IDirectDraw **, IUnknown *);
        return stock ? ((PFN)(void *)stock)(guid, out, outer) : DDERR_NODIRECTDRAWSUPPORT;
    }
    if (!out) {
        d3d9->Release();
        return DDERR_INVALIDPARAMS;
    }
    *out = NULL;
    if (outer) {
        d3d9->Release();
        return CLASS_E_NOAGGREGATION;
    }
    QddDraw *dd;
    HRESULT hr = qdd_draw_create(d3d9, false, true, &dd);
    if (FAILED(hr))
        return hr;
    qdd_draw_set_guid(dd, guid);
    hr = qdd_draw_qi(dd, IID_IDirectDraw, (void **)out);
    qdd_draw_release(dd, 7);
    return hr;
}

extern "C" HRESULT WINAPI DirectDrawCreateEx(GUID *guid, void **out, REFIID iid, IUnknown *outer)
{
    if (!IsEqualGUID(iid, IID_IDirectDraw7))
        return DDERR_INVALIDPARAMS;
    IDirect3D9 *d3d9;
    FARPROC stock;
    if (!qdd_select("DirectDrawCreateEx", &d3d9, &stock)) {
        typedef HRESULT(WINAPI * PFN)(GUID *, void **, REFIID, IUnknown *);
        return stock ? ((PFN)(void *)stock)(guid, out, iid, outer) : DDERR_NODIRECTDRAWSUPPORT;
    }
    if (!out) {
        d3d9->Release();
        return DDERR_INVALIDPARAMS;
    }
    *out = NULL;
    if (outer) {
        d3d9->Release();
        return CLASS_E_NOAGGREGATION;
    }
    QddDraw *dd;
    HRESULT hr = qdd_draw_create(d3d9, true, true, &dd);
    if (FAILED(hr))
        return hr;
    qdd_draw_set_guid(dd, guid);
    *out = qdd_draw_iface(dd, 7);
    return DD_OK;
}

extern "C" HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, IDirectDrawClipper **out, IUnknown *outer)
{
    typedef HRESULT(WINAPI * PFN)(DWORD, IDirectDrawClipper **, IUnknown *);
    PFN f = (PFN)(void *)qdd_fallback_export("DirectDrawCreateClipper");
    if (f)
        return f(flags, out, outer);
    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!out)
        return DDERR_INVALIDPARAMS;
    QddClipper *c;
    HRESULT hr = qdd_clipper_create(flags, &c, true);
    if (FAILED(hr))
        return hr;
    *out = &c->iface;
    return DD_OK;
}

static const char g_drv_desc[] = "Primary Display Driver";
static const char g_drv_name[] = "display";

extern "C" HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, void *ctx, DWORD flags)
{
    typedef HRESULT(WINAPI * PFN)(LPDDENUMCALLBACKEXA, void *, DWORD);
    PFN f = (PFN)(void *)qdd_fallback_export("DirectDrawEnumerateExA");
    if (f)
        return f(cb, ctx, flags);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    if (flags & ~(DWORD)(DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES | DDENUM_NONDISPLAYDEVICES))
        return DDERR_INVALIDPARAMS;
    cb(NULL, (char *)g_drv_desc, (char *)g_drv_name, ctx, NULL);
    return DD_OK;
}

struct QddEnumA {
    LPDDENUMCALLBACKA cb;
    void *ctx;
};

static WINBOOL CALLBACK qdd_enum_thunk(GUID *guid, LPSTR desc, LPSTR name, LPVOID ctx, HMONITOR)
{
    QddEnumA *e = (QddEnumA *)ctx;
    return e->cb(guid, desc, name, e->ctx);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, void *ctx)
{
    typedef HRESULT(WINAPI * PFN)(LPDDENUMCALLBACKA, void *);
    PFN f = (PFN)(void *)qdd_fallback_export("DirectDrawEnumerateA");
    if (f)
        return f(cb, ctx);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    QddEnumA e = {cb, ctx};
    return DirectDrawEnumerateExA(qdd_enum_thunk, &e, 0);
}

/* Not implemented by the Windows runtime either. */
extern "C" HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, void *ctx)
{
    typedef HRESULT(WINAPI * PFN)(LPDDENUMCALLBACKW, void *);
    PFN f = (PFN)(void *)qdd_fallback_export("DirectDrawEnumerateW");
    if (f)
        return f(cb, ctx);
    return cb ? DDERR_UNSUPPORTED : DDERR_INVALIDPARAMS;
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, void *ctx, DWORD flags)
{
    typedef HRESULT(WINAPI * PFN)(LPDDENUMCALLBACKEXW, void *, DWORD);
    PFN f = (PFN)(void *)qdd_fallback_export("DirectDrawEnumerateExW");
    if (f)
        return f(cb, ctx, flags);
    return DDERR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    QddLock lock;
    return (g_objects > 0 || g_stock) ? S_FALSE : S_OK;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    typedef HRESULT(WINAPI * PFN)(REFCLSID, REFIID, void **);
    PFN f = (PFN)(void *)qdd_fallback_export("DllGetClassObject");
    if (f)
        return f(clsid, riid, out);
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    int kind;
    if (IsEqualGUID(clsid, QDD_CLSID_DirectDraw))
        kind = 0;
    else if (IsEqualGUID(clsid, QDD_CLSID_DirectDraw7))
        kind = 1;
    else if (IsEqualGUID(clsid, QDD_CLSID_DirectDrawClipper))
        kind = 2;
    else
        return CLASS_E_CLASSNOTAVAILABLE;
    QddFactory *cf = new QddFactory;
    if (!cf)
        return E_OUTOFMEMORY;
    cf->refs = 1;
    cf->kind = kind;
    InterlockedIncrement(&g_objects);
    HRESULT hr = cf->QueryInterface(riid, out);
    cf->Release();
    return hr;
}

/* d3dim700.dll/d3dim.dll/ddrawex.dll import these from "ddraw.dll", so with
 * the front end installed the stock runtime's Direct3D binds to them: while
 * the stock runtime is the one in use they must take its lock. */
extern "C" void WINAPI AcquireDDThreadLock(void)
{
    typedef void(WINAPI * PFN)(void);
    PFN f = (PFN)(void *)qdd_fallback_export("AcquireDDThreadLock");
    if (f)
        f();
    else
        EnterCriticalSection(&g_qdd_cs);
}

extern "C" void WINAPI ReleaseDDThreadLock(void)
{
    typedef void(WINAPI * PFN)(void);
    PFN f = (PFN)(void *)qdd_fallback_export("ReleaseDDThreadLock");
    if (f)
        f();
    else
        LeaveCriticalSection(&g_qdd_cs);
}

/* The surface whose DC is dc, in any DirectDraw object of ours. */
extern "C" HRESULT WINAPI GetSurfaceFromDC(HDC dc, IDirectDrawSurface4 **surface, HDC *device_dc)
{
    typedef HRESULT(WINAPI * PFN)(HDC, IDirectDrawSurface4 **, HDC *);
    PFN f = (PFN)(void *)qdd_fallback_export("GetSurfaceFromDC");
    if (f)
        return f(dc, surface, device_dc);
    if (!surface)
        return E_INVALIDARG;
    if (!device_dc) {
        *surface = NULL;
        return E_INVALIDARG;
    }
    QddLock lock;
    for (QddDraw *dd = g_qdd_draws; dd; dd = dd->next) {
        IDirectDrawSurface4 *s4 = NULL;
        if (SUCCEEDED(dd->if4.GetSurfaceFromDC(dc, &s4))) {
            /* Native returns the IDirectDrawSurface interface and a display
             * DC (ddraw7.c test_get_surface_from_dc). */
            QddSurface *s = qdd_surf_from(s4);
            qdd_surf_addref(s, 0);
            s4->Release();
            *surface = (IDirectDrawSurface4 *)qdd_surf_iface(s, 1);
            static HDC display;
            if (!display)
                display = CreateDCA("DISPLAY", NULL, NULL, NULL);
            *device_dc = display;
            return DD_OK;
        }
    }
    *surface = NULL;
    *device_dc = NULL;
    return DDERR_NOTFOUND;
}

/* Internal exports of the stock runtime (argument sizes measured on the XP
 * SP3 ddraw.dll), forwarded; without a stock runtime they fail. */
#define QDD_FORWARD0(name)                                         \
    extern "C" DWORD WINAPI name(void)                             \
    {                                                              \
        typedef DWORD(WINAPI * PFN)(void);                         \
        PFN f = (PFN)(void *)qdd_stock_export(#name);              \
        return f ? f() : 0;                                        \
    }
#define QDD_FORWARD1(name, fail)                                   \
    extern "C" DWORD WINAPI name(DWORD a)                          \
    {                                                              \
        typedef DWORD(WINAPI * PFN)(DWORD);                        \
        PFN f = (PFN)(void *)qdd_stock_export(#name);              \
        return f ? f(a) : (DWORD)(fail);                           \
    }
#define QDD_FORWARD2(name, fail)                                   \
    extern "C" DWORD WINAPI name(DWORD a, DWORD b)                 \
    {                                                              \
        typedef DWORD(WINAPI * PFN)(DWORD, DWORD);                 \
        PFN f = (PFN)(void *)qdd_stock_export(#name);              \
        return f ? f(a, b) : (DWORD)(fail);                        \
    }
#define QDD_FORWARD3(name, fail)                                   \
    extern "C" DWORD WINAPI name(DWORD a, DWORD b, DWORD c)        \
    {                                                              \
        typedef DWORD(WINAPI * PFN)(DWORD, DWORD, DWORD);          \
        PFN f = (PFN)(void *)qdd_stock_export(#name);              \
        return f ? f(a, b, c) : (DWORD)(fail);                     \
    }
#define QDD_FORWARD4(name, fail)                                    \
    extern "C" DWORD WINAPI name(DWORD a, DWORD b, DWORD c, DWORD d) \
    {                                                               \
        typedef DWORD(WINAPI * PFN)(DWORD, DWORD, DWORD, DWORD);    \
        PFN f = (PFN)(void *)qdd_stock_export(#name);               \
        return f ? f(a, b, c, d) : (DWORD)(fail);                   \
    }

QDD_FORWARD0(CheckFullscreen)
QDD_FORWARD1(CompleteCreateSysmemSurface, DDERR_GENERIC)
QDD_FORWARD2(D3DParseUnknownCommand, DDERR_GENERIC)
QDD_FORWARD3(DDGetAttachedSurfaceLcl, DDERR_GENERIC)
QDD_FORWARD2(DDInternalLock, DDERR_GENERIC)
QDD_FORWARD1(DDInternalUnlock, DDERR_GENERIC)
QDD_FORWARD3(DSoundHelp, 0)
QDD_FORWARD3(GetDDSurfaceLocal, DDERR_GENERIC)
QDD_FORWARD1(GetOLEThunkData, 0)
QDD_FORWARD4(RegisterSpecialCase, DDERR_GENERIC)

static bool qdd_env_flag(const char *name)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) && v[0] != '0';
}

/* The stock runtime's vtables are writable and applications (overlays,
 * hooks) patch them (ddraw1.c test_vtbl_protection): make .rdata, where
 * the compiler puts ours, writable. */
static void qdd_vtables_writable(HMODULE m)
{
    BYTE *base = (BYTE *)m;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".rdata", 7))
            continue;
        DWORD old;
        VirtualProtect(base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, PAGE_READWRITE, &old);
    }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_qdd_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_qdd_cs);
        qdd_trace_init();
        g_qdd_env.virtual_modes = qdd_env_flag("QDDRAW_VIRTUAL_MODES");
        qdd_vtables_writable(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!reserved) {
            qdd_presenter_stop();
            DeleteCriticalSection(&g_qdd_cs);
        }
    }
    return TRUE;
}
