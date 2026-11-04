/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: DLL entry, exports, runtime support, shared helpers.
 *
 * Fallback to the stock runtime.  Installed, this d3d8.dll lives in the
 * system directory beside the QD3D d3d9.dll proxy, and the stock d3d8.dll
 * is kept there as qd3d8_orig.dll.  Each Direct3DCreate8 asks d3d9.dll for
 * an IDirect3D9; when that is no QD3D session (it does not answer
 * IID_QD3D_REMOTE: no host, or the host refused), the call goes to
 * qd3d8_orig.dll instead, so the application runs on the native Direct3D 8
 * runtime rather than on this front end over the stock Direct3D 9, and
 * from then on ValidateVertexShader, ValidatePixelShader and DebugSetMute
 * go there too.  qd3d8_orig.dll is loaded on first need under g_q8_lock
 * (never in DllMain); a QD3D copy or this module itself is refused, and it
 * stays loaded.  QD3D8_TEST_SYSTEM_DIR=<dir> lets <dir> stand for the
 * system directory, so tests exercise all this without touching System32.
 */
#include "qd3d8.h"
#include <stdio.h>
#include <stdarg.h>
#include "../proto/qd3d_proto.h"

CRITICAL_SECTION g_q8_lock;
static HMODULE g_d3d9_module;
static HMODULE g_q8_self;
static HMODULE g_q8_orig;     /* the stock runtime once loaded */
static bool g_q8_orig_tried;
static bool g_q8_fallback;    /* a Direct3DCreate8 went to the stock runtime */

/* IIDs from the DirectX 8 SDK d3d8.h. */
const GUID QIID_IDirect3D8 = {0x1dd9e8da, 0x1c77, 0x4d40, {0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12}};
const GUID QIID_IDirect3DDevice8 = {0x7385e5df, 0x8fe8, 0x41d5, {0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf}};
const GUID QIID_IDirect3DResource8 = {0x1b36bb7b, 0x09b7, 0x410a, {0xb4, 0x45, 0x7d, 0x14, 0x30, 0xd7, 0xb3, 0x3f}};
const GUID QIID_IDirect3DBaseTexture8 = {0xb4211cfa, 0x51b9, 0x4a9f, {0xab, 0x78, 0xdb, 0x99, 0xb2, 0xbb, 0x67, 0x8e}};
const GUID QIID_IDirect3DTexture8 = {0xe4cdd575, 0x2866, 0x4f01, {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}};
const GUID QIID_IDirect3DCubeTexture8 = {0x3ee5b968, 0x2aca, 0x4c34, {0x8b, 0xb5, 0x7e, 0x0c, 0x3d, 0x19, 0xb7, 0x50}};
const GUID QIID_IDirect3DVolumeTexture8 = {0x4b8aaafa, 0x140f, 0x42ba, {0x91, 0x31, 0x59, 0x7e, 0xaf, 0xaa, 0x2e, 0xad}};
const GUID QIID_IDirect3DVertexBuffer8 = {0x8aeeeac7, 0x05f9, 0x44d4, {0xb5, 0x91, 0x00, 0x0b, 0x0d, 0xf1, 0xcb, 0x95}};
const GUID QIID_IDirect3DIndexBuffer8 = {0x0e689c9a, 0x053d, 0x44a0, {0x9d, 0x92, 0xdb, 0x0e, 0x3d, 0x75, 0x0f, 0x86}};
const GUID QIID_IDirect3DSurface8 = {0xb96eebca, 0xb326, 0x4ea5, {0x88, 0x2f, 0x2f, 0xf5, 0xba, 0xe0, 0x21, 0xdd}};
const GUID QIID_IDirect3DVolume8 = {0xbd7349f5, 0x14f1, 0x42e4, {0x9c, 0x79, 0x97, 0x23, 0x80, 0xdb, 0x40, 0xc0}};
const GUID QIID_IDirect3DSwapChain8 = {0x928c088b, 0x76b9, 0x4c6b, {0xa5, 0x36, 0xa5, 0x90, 0x85, 0x38, 0x76, 0xcd}};

/* ------------------------------------------------------------------------ */
/* Freestanding C++ runtime: no libstdc++, no exceptions.                    */

void *operator new(size_t n) { return malloc(n ? n : 1); }
void *operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual(void) { abort(); }

/* ------------------------------------------------------------------------ */

void q8_log(const char *fmt, ...)
{
    char buf[512];
    char path[MAX_PATH];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3)
        n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    OutputDebugStringA(buf);

    DWORD len = GetEnvironmentVariableA("QD3D8_LOG", path, sizeof(path));
    if (len > 0 && len < sizeof(path)) {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(f, buf, (DWORD)n, &written, NULL);
            CloseHandle(f);
        }
    }
}

UINT q8_table_add(Q8Table *t, void *p)
{
    for (UINT i = 0; i < t->count; ++i) {
        if (!t->items[i]) {
            t->items[i] = p;
            return i + 1;
        }
    }
    if (t->count == t->capacity) {
        UINT cap = t->capacity ? t->capacity * 2 : 16;
        if (cap > 0x00ffffffu)
            return 0;
        void **n = (void **)realloc(t->items, cap * sizeof(void *));
        if (!n)
            return 0;
        t->items = n;
        t->capacity = cap;
    }
    t->items[t->count++] = p;
    return t->count;
}

void *q8_table_get(const Q8Table *t, UINT index1)
{
    if (index1 == 0 || index1 > t->count)
        return NULL;
    return t->items[index1 - 1];
}

void q8_table_clear_slot(Q8Table *t, UINT index1)
{
    if (index1 != 0 && index1 <= t->count)
        t->items[index1 - 1] = NULL;
}

void q8_table_free(Q8Table *t)
{
    free(t->items);
    t->items = NULL;
    t->count = t->capacity = 0;
}

/* ------------------------------------------------------------------------ */
/* Format geometry                                                           */

bool q8_format_blocks(D3DFORMAT fmt, UINT *bw, UINT *bh, UINT *bytes)
{
    *bw = *bh = 1;
    switch ((DWORD)fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16: case 65 /* D3D8 W11V11U10 */: case D3DFMT_A2W10V10U10:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10:
    case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_INDEX32:
        *bytes = 4;
        return true;
    case D3DFMT_R8G8B8:
        *bytes = 3;
        return true;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_D16:
    case D3DFMT_INDEX16: case D3DFMT_L16:
        *bytes = 2;
        return true;
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4:
        *bytes = 1;
        return true;
    case D3DFMT_UYVY: case D3DFMT_YUY2:
        *bw = 2;
        *bytes = 4;
        return true;
    case D3DFMT_DXT1:
        *bw = *bh = 4;
        *bytes = 8;
        return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        *bw = *bh = 4;
        *bytes = 16;
        return true;
    default:
        *bytes = 0;
        return false;
    }
}

UINT q8_surface_size(D3DFORMAT fmt, UINT width, UINT height, UINT depth)
{
    UINT bw, bh, bytes;
    if (!q8_format_blocks(fmt, &bw, &bh, &bytes)) {
        /* 3Dc formats have 4x4 blocks of 8 (ATI1) or 16 (ATI2) bytes, but
         * D3D9 locks them byte per texel, so they are never copied here. */
        if (fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1'))
            bytes = 8;
        else if (fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'))
            bytes = 16;
        else
            return 0;
        bw = bh = 4;
    }
    UINT64 cols = (width + bw - 1) / bw;
    UINT64 rows = (height + bh - 1) / bh;
    UINT64 total = cols * rows * bytes * (depth ? depth : 1);
    return total > 0xffffffffu ? 0xffffffffu : (UINT)total;
}

/* ------------------------------------------------------------------------ */
/* Runtime selection                                                         */

static IDirect3D9 *q8_create_d3d9(void)
{
    typedef IDirect3D9 *(WINAPI * PFN_Direct3DCreate9)(UINT);
    char path[MAX_PATH + 16];

    EnterCriticalSection(&g_q8_lock);
    if (!g_d3d9_module) {
        /* A QD3D proxy the process already has (a game's own copy, loaded
         * before this front end) serves us too: loading another d3d9.dll by
         * path would start a second proxy, and each claims a channel slot. */
        HMODULE loaded = GetModuleHandleA("d3d9.dll");
        if (loaded && FindResourceA(loaded, "QD3D_MARKER", RT_RCDATA)) {
            DWORD k = GetModuleFileNameA(loaded, path, MAX_PATH);
            if (k > 0 && k < MAX_PATH)
                g_d3d9_module = LoadLibraryA(path);
            if (g_d3d9_module)
                q8_log("qd3d8: loaded %s (already in the process)", path);
        }
    }
    if (!g_d3d9_module) {
        /* Otherwise prefer the d3d9.dll in this DLL's own directory:
         * installed, both live in System32 (the QD3D proxy); in a test
         * directory, the proxy placed beside us. Only without one there,
         * use the system copy. */
        DWORD m = GetModuleFileNameA(g_q8_self, path, MAX_PATH);
        char *slash = (m > 0 && m < MAX_PATH) ? strrchr(path, '\\') : NULL;

        if (slash && (size_t)(slash - path) + sizeof("\\d3d9.dll") <= sizeof path) {
            memcpy(slash, "\\d3d9.dll", sizeof("\\d3d9.dll"));
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
                g_d3d9_module = LoadLibraryA(path);
        }
        if (!g_d3d9_module) {
            UINT n = GetSystemDirectoryA(path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) {
                LeaveCriticalSection(&g_q8_lock);
                q8_log("qd3d8: GetSystemDirectory failed");
                return NULL;
            }
            memcpy(path + n, "\\d3d9.dll", sizeof("\\d3d9.dll"));
            g_d3d9_module = LoadLibraryA(path);
        }
        if (!g_d3d9_module)
            q8_log("qd3d8: LoadLibrary(%s) failed, error %lu", path, GetLastError());
        else
            q8_log("qd3d8: loaded %s", path);
    }
    HMODULE mod = g_d3d9_module;
    LeaveCriticalSection(&g_q8_lock);
    if (!mod)
        return NULL;
    PFN_Direct3DCreate9 create = (PFN_Direct3DCreate9)(void *)GetProcAddress(mod, "Direct3DCreate9");
    if (!create) {
        q8_log("qd3d8: d3d9.dll has no Direct3DCreate9");
        return NULL;
    }
    return create(D3D_SDK_VERSION);
}

/* Whether d3d9 belongs to a connected QD3D session. */
static bool q8_is_remote(IDirect3D9 *d3d9)
{
    void *p = NULL;
    if (!d3d9 || FAILED(d3d9->QueryInterface(IID_QD3D_REMOTE, &p)) || !p)
        return false;
    ((IUnknown *)p)->Release();
    return true;
}

/* Whether this module is installed: it lives in the system directory
 * (System32, or SysWOW64 when a 64-bit Windows loads it from there), or in
 * the directory QD3D8_TEST_SYSTEM_DIR names.  sysdir receives the directory
 * the stock runtime is kept in. */
static bool q8_installed(char *sysdir, DWORD cap)
{
    typedef UINT(WINAPI * PFN_GetSystemWow64DirectoryA)(LPSTR, UINT);
    char self[MAX_PATH], wow[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_q8_self, self, MAX_PATH);
    char *slash = n > 0 && n < MAX_PATH ? strrchr(self, '\\') : NULL;
    if (!slash)
        return false;
    *slash = 0;
    n = GetEnvironmentVariableA("QD3D8_TEST_SYSTEM_DIR", sysdir, cap);
    if (n > 0 && n < cap) {
        if (sysdir[n - 1] == '\\')
            sysdir[n - 1] = 0;
        return lstrcmpiA(self, sysdir) == 0;
    }
    n = GetSystemDirectoryA(sysdir, cap);
    if (n == 0 || n >= cap)
        return false;
    if (lstrcmpiA(self, sysdir) == 0)
        return true;
    PFN_GetSystemWow64DirectoryA wow64dir = (PFN_GetSystemWow64DirectoryA)(void *)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetSystemWow64DirectoryA");
    n = wow64dir ? wow64dir(wow, MAX_PATH) : 0;
    return n > 0 && n < MAX_PATH && lstrcmpiA(self, wow) == 0;
}

/* The stock runtime kept as qd3d8_orig.dll in sysdir, or NULL when it is
 * missing or is not the stock runtime (a QD3D copy, or this module).
 * Called under g_q8_lock; loaded once and never unloaded. */
static HMODULE q8_orig_module(const char *sysdir)
{
    static const char name[] = "\\qd3d8_orig.dll";
    char path[MAX_PATH + sizeof(name)];
    if (g_q8_orig_tried)
        return g_q8_orig;
    g_q8_orig_tried = true;
    size_t n = strlen(sysdir);
    if (n >= MAX_PATH)
        return NULL;
    memcpy(path, sysdir, n);
    memcpy(path + n, name, sizeof(name));
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        q8_log("qd3d8: stock runtime %s not loadable, error %lu", path, GetLastError());
        return NULL;
    }
    if (m == g_q8_self || FindResourceA(m, "QD3D_MARKER", RT_RCDATA) || !GetProcAddress(m, "Direct3DCreate8")) {
        q8_log("qd3d8: %s is not the stock runtime; refused", path);
        FreeLibrary(m);
        return NULL;
    }
    q8_log("qd3d8: stock runtime %s", path);
    g_q8_orig = m;
    return m;
}

/* An export of the stock runtime once a Direct3DCreate8 went there. */
static FARPROC q8_orig_export(const char *name)
{
    FARPROC f = NULL;
    EnterCriticalSection(&g_q8_lock);
    if (g_q8_fallback)
        f = GetProcAddress(g_q8_orig, name);
    LeaveCriticalSection(&g_q8_lock);
    return f;
}

/* ------------------------------------------------------------------------ */
/* Exports                                                                   */

extern "C" d8::IDirect3D8 *WINAPI Direct3DCreate8(UINT sdk_version)
{
    typedef d8::IDirect3D8 *(WINAPI * PFN_Direct3DCreate8)(UINT);
    char sysdir[MAX_PATH];
    bool installed = q8_installed(sysdir, sizeof(sysdir));
    IDirect3D9 *d3d9 = q8_create_d3d9();
    if (installed && !q8_is_remote(d3d9)) {
        PFN_Direct3DCreate8 create = NULL;
        EnterCriticalSection(&g_q8_lock);
        HMODULE orig = q8_orig_module(sysdir);
        if (orig) {
            create = (PFN_Direct3DCreate8)(void *)GetProcAddress(orig, "Direct3DCreate8");
            g_q8_fallback = true;
        }
        LeaveCriticalSection(&g_q8_lock);
        if (create) {
            if (d3d9)
                d3d9->Release();
            d8::IDirect3D8 *d3d = create(sdk_version);
            q8_log("qd3d8: no QD3D session; Direct3DCreate8(%u) -> stock runtime %p", sdk_version, (void *)d3d);
            return d3d;
        }
        /* Without a stock runtime the front end runs on whatever d3d9.dll
         * gave. */
    }
    if (!d3d9)
        return NULL;
    Q8Direct3D *d3d = new Q8Direct3D(d3d9);
    if (!d3d) {
        d3d9->Release();
        return NULL;
    }
    q8_log("qd3d8: Direct3DCreate8(%u) -> %p", sdk_version, (void *)d3d);
    return d3d;
}

/* The validator text in a process-heap string, as the stock runtime
 * returns it (empty unless return_errors). */
static void q8_return_errors(const Q8Text *msg, BOOL return_errors, char **errors)
{
    const char *text = return_errors && msg->buf ? msg->buf : "";
    size_t n = strlen(text) + 1;
    char *s = (char *)HeapAlloc(GetProcessHeap(), 0, n);
    if (s)
        memcpy(s, text, n);
    *errors = s;
}

/* Exported by the stock d3d8.dll; same semantics (measured): a string
 * whenever errors is given, the declaration's inputs checked against the
 * reads, the caps' version and constant count when caps are given; a
 * declaration alone passes. */
extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD *vs, const DWORD *decl, const d8::D3DCAPS8 *caps,
                                               BOOL return_errors, char **errors)
{
    typedef HRESULT(WINAPI * PFN)(const DWORD *, const DWORD *, const d8::D3DCAPS8 *, BOOL, char **);
    PFN orig = (PFN)(void *)q8_orig_export("ValidateVertexShader");
    if (orig)
        return orig(vs, decl, caps, return_errors, errors);
    Q8Text msg = {NULL, 0, 0};
    HRESULT hr = D3D_OK;
    if (!vs) {
        if (!decl) {
            q8_text_add(&msg, "(Global Validation Error) Version Token: Code and declaration pointers passed into "
                        "shader vertex shader validator cannot both be NULL.\n");
            hr = E_FAIL;
        }
    } else {
        Q8ShaderLimits lim = {caps ? caps->VertexShaderVersion : ~0u, caps ? caps->MaxVertexShaderConst : ~0u, ~0u};
        if (decl && !q8_decl_inputs(decl, &lim.inputs)) {
            q8_text_add(&msg, "(Global Validation Error) Invalid vertex shader declaration.\n");
            hr = E_FAIL;
        } else {
            hr = q8_check_vertex_shader(vs, &lim, &msg, NULL);
        }
    }
    if (errors)
        q8_return_errors(&msg, return_errors, errors);
    q8_text_free(&msg);
    return hr;
}

/* As the stock runtime: a NULL shader fails without touching errors. */
extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD *ps, const d8::D3DCAPS8 *caps, BOOL return_errors,
                                              char **errors)
{
    typedef HRESULT(WINAPI * PFN)(const DWORD *, const d8::D3DCAPS8 *, BOOL, char **);
    PFN orig = (PFN)(void *)q8_orig_export("ValidatePixelShader");
    if (orig)
        return orig(ps, caps, return_errors, errors);
    if (!ps)
        return E_FAIL;
    Q8ShaderLimits lim = {caps ? caps->PixelShaderVersion : ~0u, ~0u, ~0u};
    Q8Text msg = {NULL, 0, 0};
    HRESULT hr = q8_check_pixel_shader(ps, &lim, &msg, NULL);
    if (errors)
        q8_return_errors(&msg, return_errors, errors);
    q8_text_free(&msg);
    return hr;
}

extern "C" void WINAPI DebugSetMute(void)
{
    typedef void(WINAPI * PFN)(void);
    PFN orig = (PFN)(void *)q8_orig_export("DebugSetMute");
    if (orig)
        orig();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_q8_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_q8_lock);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        DeleteCriticalSection(&g_q8_lock);
    }
    return TRUE;
}
