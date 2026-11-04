#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_LOG "glprobe.log"
#define CLASS_NAME "XPDriverGLProbe"
#define OPENGL_GETINFO 0x1101
#define SHIM_DRIVER_NAME L"XPDriverStub"
#define SHIM_INTERFACE_VERSION 2

static FILE *log_file;
static int (WINAPI *real_ext_escape)(HDC, int, int, LPCSTR, int, LPSTR);
static LSTATUS (WINAPI *real_reg_open_key_ex_w)(HKEY, LPCWSTR, DWORD, REGSAM,
                                                PHKEY);
static LSTATUS (WINAPI *real_reg_query_value_ex_w)(HKEY, LPCWSTR, LPDWORD,
                                                   LPDWORD, LPBYTE, LPDWORD);
static HMODULE (WINAPI *real_load_library_w)(LPCWSTR);
static FARPROC (WINAPI *real_get_proc_address)(HMODULE, LPCSTR);

typedef struct opengl_info {
    DWORD version;
    DWORD driver_version;
    WCHAR driver_name[MAX_PATH + 1];
} OPENGL_INFO;

static void log_line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);

    if (log_file) {
        va_start(args, format);
        vfprintf(log_file, format, args);
        va_end(args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static int WINAPI mock_ext_escape(HDC dc, int escape, int input_size,
                                  LPCSTR input, int output_size, LPSTR output)
{
    DWORD saved_error;

    if (escape == QUERYESCSUPPORT && input && input_size >= (int)sizeof(DWORD) &&
        *(const DWORD *)input == OPENGL_GETINFO) {
        saved_error = GetLastError();
        log_line("escape-shim QUERYESCSUPPORT(OPENGL_GETINFO) -> 1");
        SetLastError(saved_error);
        return 1;
    }
    if (escape == OPENGL_GETINFO && output &&
        output_size >= (int)sizeof(OPENGL_INFO)) {
        OPENGL_INFO *info = (OPENGL_INFO *)output;
        memset(info, 0, sizeof(*info));
        info->version = SHIM_INTERFACE_VERSION;
        info->driver_version = 1;
        lstrcpyW(info->driver_name, SHIM_DRIVER_NAME);
        saved_error = GetLastError();
        log_line("escape-shim OPENGL_GETINFO -> version=%u driver_version=1 "
                 "name=XPDriverStub", SHIM_INTERFACE_VERSION);
        SetLastError(saved_error);
        return 1;
    }
    return real_ext_escape ? real_ext_escape(dc, escape, input_size, input,
                                              output_size, output) : 0;
}

static const char *wide_text(LPCWSTR input, char *output, int output_size)
{
    if (!input) {
        return "(null)";
    }
    if (!WideCharToMultiByte(CP_ACP, 0, input, -1, output, output_size,
                             NULL, NULL)) {
        return "(conversion-failed)";
    }
    output[output_size - 1] = '\0';
    return output;
}

static LSTATUS WINAPI trace_reg_open_key_ex_w(HKEY root, LPCWSTR subkey,
                                               DWORD options, REGSAM access,
                                               PHKEY result_key)
{
    char name[512];
    DWORD error;
    LSTATUS result = real_reg_open_key_ex_w(root, subkey, options, access,
                                             result_key);
    error = GetLastError();
    log_line("loader RegOpenKeyExW(root=%08lx, subkey=%s) -> %ld",
             (unsigned long)(ULONG_PTR)root,
             wide_text(subkey, name, sizeof(name)), (long)result);
    SetLastError(error);
    return result;
}

static LSTATUS WINAPI trace_reg_query_value_ex_w(HKEY key, LPCWSTR value_name,
                                                  LPDWORD reserved, LPDWORD type,
                                                  LPBYTE data, LPDWORD size)
{
    char name[256];
    DWORD error;
    LSTATUS result = real_reg_query_value_ex_w(key, value_name, reserved, type,
                                                data, size);
    error = GetLastError();
    log_line("loader RegQueryValueExW(value=%s) -> %ld type=%lu size=%lu",
             wide_text(value_name, name, sizeof(name)), (long)result,
             (unsigned long)(type ? *type : 0),
             (unsigned long)(size ? *size : 0));
    SetLastError(error);
    return result;
}

static HMODULE WINAPI trace_load_library_w(LPCWSTR filename)
{
    char name[MAX_PATH + 1];
    HMODULE result;
    DWORD error;
    SetLastError(0);
    result = real_load_library_w(filename);
    error = result ? ERROR_SUCCESS : GetLastError();
    log_line("loader LoadLibraryW(%s) -> %08lx error=%lu",
             wide_text(filename, name, sizeof(name)),
             (unsigned long)(ULONG_PTR)result, (unsigned long)error);
    SetLastError(error);
    return result;
}

static FARPROC WINAPI trace_get_proc_address(HMODULE module, LPCSTR name)
{
    FARPROC result = real_get_proc_address(module, name);
    DWORD error = GetLastError();
    if (((ULONG_PTR)name >> 16) != 0) {
        log_line("loader GetProcAddress(module=%08lx, name=%s) -> %08lx",
                 (unsigned long)(ULONG_PTR)module, name,
                 (unsigned long)(ULONG_PTR)result);
    }
    SetLastError(error);
    return result;
}

static int patch_import(HMODULE module, const char *dll_wanted,
                        const char *function_wanted, PROC replacement,
                        PROC *original)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;
    DWORD old_protect;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (IMAGE_NT_HEADERS32 *)((BYTE *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)module +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char *dll_name = (const char *)module + descriptor->Name;
        IMAGE_THUNK_DATA32 *names;
        IMAGE_THUNK_DATA32 *iat;
        if (lstrcmpiA(dll_name, dll_wanted)) {
            continue;
        }
        names = (IMAGE_THUNK_DATA32 *)((BYTE *)module +
            (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk :
                                              descriptor->FirstThunk));
        iat = (IMAGE_THUNK_DATA32 *)((BYTE *)module + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            IMAGE_IMPORT_BY_NAME *import_name;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                continue;
            }
            import_name = (IMAGE_IMPORT_BY_NAME *)((BYTE *)module +
                                                    names->u1.AddressOfData);
            if (strcmp((const char *)import_name->Name, function_wanted)) {
                continue;
            }
            *original = (PROC)(ULONG_PTR)iat->u1.Function;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                                PAGE_READWRITE, &old_protect)) {
                return 0;
            }
            iat->u1.Function = (DWORD)(ULONG_PTR)replacement;
            VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                           old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function,
                                  sizeof(iat->u1.Function));
            return 1;
        }
    }
    return 0;
}

static int install_escape_shim(int trace_loader)
{
    HMODULE module = GetModuleHandleA("opengl32.dll");

    if (!module) {
        log_line("escape-shim: opengl32.dll is not loaded");
        return 0;
    }
    if (!patch_import(module, "GDI32.dll", "ExtEscape", (PROC)mock_ext_escape,
                      (PROC *)&real_ext_escape)) {
        log_line("escape-shim: GDI32!ExtEscape import not found");
        return 0;
    }
    if (trace_loader) {
        patch_import(module, "ADVAPI32.dll", "RegOpenKeyExW",
                     (PROC)trace_reg_open_key_ex_w,
                     (PROC *)&real_reg_open_key_ex_w);
        patch_import(module, "ADVAPI32.dll", "RegQueryValueExW",
                     (PROC)trace_reg_query_value_ex_w,
                     (PROC *)&real_reg_query_value_ex_w);
        patch_import(module, "KERNEL32.dll", "LoadLibraryW",
                     (PROC)trace_load_library_w, (PROC *)&real_load_library_w);
        patch_import(module, "KERNEL32.dll", "GetProcAddress",
                     (PROC)trace_get_proc_address, (PROC *)&real_get_proc_address);
    }
    log_line("escape-shim installed in XP opengl32.dll IAT%s",
             trace_loader ? " with loader tracing" : "");
    return 1;
}

static HWND make_window(const char *title)
{
    return CreateWindowA(CLASS_NAME, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 480, 360,
                         NULL, NULL, GetModuleHandleA(NULL), NULL);
}

static int set_stub_mode(DWORD mode)
{
    HKEY key;
    LONG status;

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                             "SOFTWARE\\XPDriver\\StubICD", 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) {
        log_line("mode flip: RegCreateKeyEx failed status=%ld", (long)status);
        return 0;
    }
    status = RegSetValueExA(key, "FailMode", 0, REG_DWORD,
                            (const BYTE *)&mode, sizeof(mode));
    RegCloseKey(key);
    log_line("mode flip: FailMode=%lu status=%ld", (unsigned long)mode,
             (long)status);
    return status == ERROR_SUCCESS;
}

static PIXELFORMATDESCRIPTOR requested_format(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    return pfd;
}

static int enumerate_formats(HDC dc, int *first_generic)
{
    PIXELFORMATDESCRIPTOR pfd;
    int total;
    int index;

    memset(&pfd, 0, sizeof(pfd));
    total = DescribePixelFormat(dc, 1, sizeof(pfd), &pfd);
    *first_generic = 0;
    log_line("DescribePixelFormat total=%d error=%lu", total,
             (unsigned long)GetLastError());
    for (index = 1; index <= total; ++index) {
        memset(&pfd, 0, sizeof(pfd));
        if (!DescribePixelFormat(dc, index, sizeof(pfd), &pfd)) {
            log_line("  format=%d describe failed error=%lu", index,
                     (unsigned long)GetLastError());
            continue;
        }
        log_line("  format=%d flags=%08lx generic=%d color=%u depth=%u stencil=%u",
                 index, (unsigned long)pfd.dwFlags,
                 !!(pfd.dwFlags & PFD_GENERIC_FORMAT), pfd.cColorBits,
                 pfd.cDepthBits, pfd.cStencilBits);
        if (!*first_generic && (pfd.dwFlags & PFD_GENERIC_FORMAT)) {
            *first_generic = index;
        }
    }
    return total;
}

static void render_probe(HDC dc)
{
    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    BOOL swapped;
    DWORD error;

    log_line("GL_VENDOR=%s", vendor ? (const char *)vendor : "(null)");
    log_line("GL_RENDERER=%s", renderer ? (const char *)renderer : "(null)");
    log_line("GL_VERSION=%s", version ? (const char *)version : "(null)");

    glViewport(0, 0, 480, 360);
    glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.1f, 0.1f);
    glVertex3f(-0.8f, -0.7f, 0.0f);
    glColor3f(0.1f, 1.0f, 0.1f);
    glVertex3f(0.8f, -0.7f, 0.0f);
    glColor3f(0.1f, 0.3f, 1.0f);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();
    glFlush();
    SetLastError(0);
    swapped = SwapBuffers(dc);
    error = GetLastError();
    log_line("SwapBuffers=%d error=%lu", swapped, (unsigned long)error);
}

static void try_context(HDC dc, const char *label)
{
    HGLRC context;
    BOOL current;
    BOOL deleted;
    DWORD error;

    SetLastError(0);
    context = wglCreateContext(dc);
    log_line("%s wglCreateContext=%08lx error=%lu", label,
             (unsigned long)(ULONG_PTR)context, (unsigned long)GetLastError());
    if (!context) {
        return;
    }
    SetLastError(0);
    current = wglMakeCurrent(dc, context);
    log_line("%s wglMakeCurrent=%d error=%lu", label, current,
             (unsigned long)GetLastError());
    if (current) {
        render_probe(dc);
        wglMakeCurrent(NULL, NULL);
    }
    SetLastError(0);
    deleted = wglDeleteContext(context);
    error = GetLastError();
    log_line("%s wglDeleteContext=%d error=%lu", label, deleted,
             (unsigned long)error);
}

static int run_primary(int round, int *generic_out)
{
    PIXELFORMATDESCRIPTOR request = requested_format();
    HWND window = make_window("XPDriver GL primary");
    HDC dc;
    int chosen;
    int generic;
    BOOL set;

    if (!window) {
        log_line("round=%d CreateWindow failed error=%lu", round,
                 (unsigned long)GetLastError());
        return 0;
    }
    dc = GetDC(window);
    log_line("=== round %d primary hwnd=%08lx hdc=%08lx ===", round,
             (unsigned long)(ULONG_PTR)window, (unsigned long)(ULONG_PTR)dc);
    enumerate_formats(dc, &generic);
    *generic_out = generic;
    SetLastError(0);
    chosen = ChoosePixelFormat(dc, &request);
    log_line("ChoosePixelFormat=%d error=%lu first_generic=%d", chosen,
             (unsigned long)GetLastError(), generic);
    if (chosen) {
        SetLastError(0);
        set = SetPixelFormat(dc, chosen, &request);
        log_line("SetPixelFormat(chosen=%d)=%d error=%lu", chosen, set,
                 (unsigned long)GetLastError());
        if (set) {
            try_context(dc, "primary");
        }
    }

    if (generic) {
        SetLastError(0);
        set = SetPixelFormat(dc, generic, &request);
        log_line("same-window SetPixelFormat(generic=%d)=%d error=%lu",
                 generic, set, (unsigned long)GetLastError());
        if (set) {
            try_context(dc, "same-window-generic");
        }
    }
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return 1;
}

static void run_fresh_generic(int generic)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND window;
    HDC dc;
    BOOL set;

    if (!generic) {
        log_line("fresh generic skipped: no PFD_GENERIC_FORMAT index found");
        return;
    }
    window = make_window("XPDriver GL fresh generic");
    if (!window) {
        log_line("fresh CreateWindow failed error=%lu",
                 (unsigned long)GetLastError());
        return;
    }
    dc = GetDC(window);
    memset(&pfd, 0, sizeof(pfd));
    DescribePixelFormat(dc, generic, sizeof(pfd), &pfd);
    SetLastError(0);
    set = SetPixelFormat(dc, generic, &pfd);
    log_line("fresh-window SetPixelFormat(generic=%d)=%d error=%lu",
             generic, set, (unsigned long)GetLastError());
    if (set) {
        try_context(dc, "fresh-window-generic");
    }
    ReleaseDC(window, dc);
    DestroyWindow(window);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    int rounds = 1;
    int round;
    int generic = 0;
    int use_escape_shim = 0;
    int trace_loader = 0;
    int flip_mode = 0;
    int arg;

    for (arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--rounds") && arg + 1 < argc) {
            rounds = atoi(argv[++arg]);
            if (rounds < 1 || rounds > 20) {
                fprintf(stderr, "--rounds must be between 1 and 20\n");
                return 2;
            }
        } else if (!strcmp(argv[arg], "--mock-icd-escape")) {
            use_escape_shim = 1;
        } else if (!strcmp(argv[arg], "--trace-loader")) {
            use_escape_shim = 1;
            trace_loader = 1;
        } else if (!strcmp(argv[arg], "--flip-mode") && arg + 1 < argc) {
            flip_mode = atoi(argv[++arg]);
            if (flip_mode < 1 || flip_mode > 6) {
                fprintf(stderr, "--flip-mode must be between 1 and 6\n");
                return 2;
            }
            if (rounds < 2) {
                rounds = 2;
            }
        } else {
            fprintf(stderr, "Usage: glprobe [--rounds N] [--mock-icd-escape] "
                            "[--trace-loader] [--flip-mode N]\n");
            return 2;
        }
    }
    log_file = fopen(PROBE_LOG, "a");
    log_line("glprobe start pid=%lu rounds=%d", (unsigned long)GetCurrentProcessId(), rounds);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log_line("RegisterClass failed error=%lu", (unsigned long)GetLastError());
        return 1;
    }
    if (use_escape_shim && !install_escape_shim(trace_loader)) {
        log_line("escape-shim installation failed");
        return 3;
    }
    for (round = 1; round <= rounds; ++round) {
        if (!run_primary(round, &generic)) {
            break;
        }
        if (round == 1 && flip_mode && !set_stub_mode((DWORD)flip_mode)) {
            break;
        }
    }
    run_fresh_generic(generic);
    log_line("glprobe end");
    if (log_file) {
        fclose(log_file);
    }
    return 0;
}
