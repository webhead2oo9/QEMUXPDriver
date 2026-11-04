/*
 * XPDriver real-XP fallback probe ICD.
 *
 * This is deliberately a diagnostic ICD, not a renderer. Its behavior is
 * selected through HKLM\SOFTWARE\XPDriver\StubICD\FailMode.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define STUB_KEY "SOFTWARE\\XPDriver\\StubICD"
#define STUB_LOG "C:\\stubicd.log"
#define GL_DISPATCH_ENTRIES 336
#define DHGLRC ULONG

enum stub_mode {
    MODE_VALIDATE_FAIL = 1,
    MODE_ZERO_FORMATS = 2,
    MODE_REFUSE_SET_PIXEL_FORMAT = 3,
    MODE_REFUSE_CREATE_CONTEXT = 4,
    MODE_REFUSE_SET_CONTEXT = 5,
    MODE_INERT_SUCCESS = 6
};

typedef struct stub_proc_table {
    int cEntries;
    PROC entries[GL_DISPATCH_ENTRIES];
} STUB_PROC_TABLE;

typedef void (APIENTRY *PFN_SETPROCTABLE)(const STUB_PROC_TABLE *table);

static void stub_log(const char *format, ...)
{
    char message[768];
    char line[896];
    DWORD written;
    HANDLE file;
    SYSTEMTIME now;
    va_list args;
    DWORD saved_error = GetLastError();

    va_start(args, format);
    _vsnprintf(message, sizeof(message) - 1, format, args);
    va_end(args);
    message[sizeof(message) - 1] = '\0';

    GetLocalTime(&now);
    _snprintf(line, sizeof(line) - 1,
              "%02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n",
              now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
              (unsigned long)GetCurrentProcessId(),
              (unsigned long)GetCurrentThreadId(), message);
    line[sizeof(line) - 1] = '\0';
    OutputDebugStringA(line);

    file = CreateFileA(STUB_LOG, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        SetLastError(saved_error);
        return;
    }
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(file);
    SetLastError(saved_error);
}

static DWORD stub_mode(void)
{
    HKEY key;
    DWORD mode = MODE_ZERO_FORMATS;
    DWORD type = 0;
    DWORD size = sizeof(mode);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, STUB_KEY, 0, KEY_QUERY_VALUE,
                      &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, "FailMode", NULL, &type, (BYTE *)&mode,
                             &size) != ERROR_SUCCESS || type != REG_DWORD) {
            mode = MODE_ZERO_FORMATS;
        }
        RegCloseKey(key);
    }
    if (mode < MODE_VALIDATE_FAIL || mode > MODE_INERT_SUCCESS) {
        mode = MODE_ZERO_FORMATS;
    }
    return mode;
}

/* ReactOS's glfuncs.h is the canonical 336-slot OpenGL 1.1 ICD order. Each
 * no-op has its real signature so __stdcall pops the correct byte count on
 * 32-bit Windows. A single generic no-op function is not ABI-safe here. */
#define USE_GL_FUNC(name, proto_args, call_args, offset, stack) \
    static void APIENTRY NoOp##name proto_args { (void)0; }
#define USE_GL_FUNC_RET(name, ret_type, proto_args, call_args, offset, stack) \
    static ret_type APIENTRY NoOp##name proto_args { return (ret_type)0; }
#include "../../refs/reactos/dll/opengl/opengl32/glfuncs.h"

static const GLubyte *APIENTRY StubGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"XPDriver";
    case GL_RENDERER:
        return (const GLubyte *)"StubICD";
    case GL_VERSION:
        return (const GLubyte *)"1.1 StubICD";
    case GL_EXTENSIONS:
        return (const GLubyte *)"";
    default:
        return NULL;
    }
}

static STUB_PROC_TABLE proc_table = {
    GL_DISPATCH_ENTRIES,
    {
#define USE_GL_FUNC(name, proto_args, call_args, offset, stack) \
        [offset] = (PROC)NoOp##name,
#define USE_GL_FUNC_RET(name, ret_type, proto_args, call_args, offset, stack) \
        [offset] = (PROC)NoOp##name,
#include "../../refs/reactos/dll/opengl/opengl32/glfuncs.h"
    }
};

static void fill_pixel_format(PIXELFORMATDESCRIPTOR *pfd, ULONG bytes)
{
    PIXELFORMATDESCRIPTOR value;
    ULONG copy;

    memset(&value, 0, sizeof(value));
    value.nSize = sizeof(value);
    value.nVersion = 1;
    value.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                    PFD_DOUBLEBUFFER;
    value.iPixelType = PFD_TYPE_RGBA;
    value.cColorBits = 32;
    value.cRedBits = 8;
    value.cGreenBits = 8;
    value.cBlueBits = 8;
    value.cAlphaBits = 8;
    value.cDepthBits = 24;
    value.cStencilBits = 8;
    value.iLayerType = PFD_MAIN_PLANE;

    if (!pfd || !bytes) {
        return;
    }
    copy = bytes < sizeof(value) ? bytes : sizeof(value);
    memcpy(pfd, &value, copy);
}

BOOL APIENTRY DrvValidateVersion(ULONG version)
{
    DWORD mode = stub_mode();
    BOOL result = mode != MODE_VALIDATE_FAIL;
    stub_log("DrvValidateVersion(version=%lu) mode=%lu -> %d",
             (unsigned long)version, (unsigned long)mode, result);
    return result;
}

LONG APIENTRY DrvDescribePixelFormat(HDC hdc, INT format, ULONG bytes,
                                     PIXELFORMATDESCRIPTOR *pfd)
{
    DWORD mode = stub_mode();
    LONG result = mode == MODE_VALIDATE_FAIL || mode == MODE_ZERO_FORMATS ? 0 : 1;
    if (result && format == 1) {
        fill_pixel_format(pfd, bytes);
    }
    stub_log("DrvDescribePixelFormat(hdc=%08lx, format=%d, bytes=%lu) mode=%lu -> %ld",
             (unsigned long)(ULONG_PTR)hdc, format, (unsigned long)bytes,
             (unsigned long)mode, (long)result);
    return result;
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG format)
{
    DWORD mode = stub_mode();
    BOOL result = mode >= MODE_REFUSE_CREATE_CONTEXT;
    stub_log("DrvSetPixelFormat(hdc=%08lx, format=%ld) mode=%lu -> %d",
             (unsigned long)(ULONG_PTR)hdc, (long)format,
             (unsigned long)mode, result);
    return result;
}

DHGLRC APIENTRY DrvCreateContext(HDC hdc)
{
    DWORD mode = stub_mode();
    DHGLRC result = mode >= MODE_REFUSE_SET_CONTEXT ? 1 : 0;
    stub_log("DrvCreateContext(hdc=%08lx) mode=%lu -> %lu",
             (unsigned long)(ULONG_PTR)hdc, (unsigned long)mode,
             (unsigned long)result);
    return result;
}

DHGLRC APIENTRY DrvCreateLayerContext(HDC hdc, int layer)
{
    DWORD mode = stub_mode();
    DHGLRC result = layer == 0 && mode >= MODE_REFUSE_SET_CONTEXT ? 1 : 0;
    stub_log("DrvCreateLayerContext(hdc=%08lx, layer=%d) mode=%lu -> %lu",
             (unsigned long)(ULONG_PTR)hdc, layer, (unsigned long)mode,
             (unsigned long)result);
    return result;
}

STUB_PROC_TABLE *APIENTRY DrvSetContext(HDC hdc, DHGLRC context,
                                        PFN_SETPROCTABLE callback)
{
    DWORD mode = stub_mode();
    STUB_PROC_TABLE *result = NULL;
    (void)callback;

    if (mode == MODE_INERT_SUCCESS && context) {
        proc_table.entries[275] = (PROC)StubGetString;
        result = &proc_table;
    }
    stub_log("DrvSetContext(hdc=%08lx, context=%lu, callback=%08lx) mode=%lu -> %08lx",
             (unsigned long)(ULONG_PTR)hdc, (unsigned long)context,
             (unsigned long)(ULONG_PTR)callback, (unsigned long)mode,
             (unsigned long)(ULONG_PTR)result);
    return result;
}

BOOL APIENTRY DrvReleaseContext(DHGLRC context)
{
    stub_log("DrvReleaseContext(context=%lu) -> 1", (unsigned long)context);
    return TRUE;
}

BOOL APIENTRY DrvDeleteContext(DHGLRC context)
{
    stub_log("DrvDeleteContext(context=%lu) -> 1", (unsigned long)context);
    return TRUE;
}

BOOL APIENTRY DrvCopyContext(DHGLRC source, DHGLRC destination, UINT mask)
{
    stub_log("DrvCopyContext(source=%lu, destination=%lu, mask=%u) -> 0",
             (unsigned long)source, (unsigned long)destination, mask);
    return FALSE;
}

BOOL APIENTRY DrvShareLists(DHGLRC source, DHGLRC destination)
{
    stub_log("DrvShareLists(source=%lu, destination=%lu) -> 0",
             (unsigned long)source, (unsigned long)destination);
    return FALSE;
}

PROC APIENTRY DrvGetProcAddress(LPCSTR name)
{
    stub_log("DrvGetProcAddress(name=%s) -> 0", name ? name : "(null)");
    return NULL;
}

VOID APIENTRY DrvSetCallbackProcs(INT count, PROC *callbacks)
{
    stub_log("DrvSetCallbackProcs(count=%d, callbacks=%08lx)", count,
             (unsigned long)(ULONG_PTR)callbacks);
}

BOOL APIENTRY DrvSwapBuffers(HDC hdc)
{
    stub_log("DrvSwapBuffers(hdc=%08lx) -> 0",
             (unsigned long)(ULONG_PTR)hdc);
    return FALSE;
}

BOOL APIENTRY DrvDescribeLayerPlane(HDC hdc, INT pixel_format, INT layer,
                                    UINT bytes,
                                    LPLAYERPLANEDESCRIPTOR descriptor)
{
    (void)hdc;
    (void)pixel_format;
    (void)layer;
    (void)bytes;
    (void)descriptor;
    stub_log("DrvDescribeLayerPlane -> 0");
    return FALSE;
}

INT APIENTRY DrvSetLayerPaletteEntries(HDC hdc, INT layer, INT start,
                                       INT count, const COLORREF *colors)
{
    (void)hdc;
    (void)layer;
    (void)start;
    (void)count;
    (void)colors;
    stub_log("DrvSetLayerPaletteEntries -> 0");
    return 0;
}

INT APIENTRY DrvGetLayerPaletteEntries(HDC hdc, INT layer, INT start,
                                       INT count, COLORREF *colors)
{
    (void)hdc;
    (void)layer;
    (void)start;
    (void)count;
    (void)colors;
    stub_log("DrvGetLayerPaletteEntries -> 0");
    return 0;
}

BOOL APIENTRY DrvRealizeLayerPalette(HDC hdc, INT layer, BOOL realize)
{
    (void)hdc;
    (void)layer;
    (void)realize;
    stub_log("DrvRealizeLayerPalette -> 0");
    return FALSE;
}

BOOL APIENTRY DrvSwapLayerBuffers(HDC hdc, UINT planes)
{
    (void)hdc;
    (void)planes;
    stub_log("DrvSwapLayerBuffers -> 0");
    return FALSE;
}

BOOL APIENTRY DrvPresentBuffers(HDC hdc, void *data)
{
    (void)hdc;
    (void)data;
    stub_log("DrvPresentBuffers -> 0");
    return FALSE;
}

HRESULT APIENTRY DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
