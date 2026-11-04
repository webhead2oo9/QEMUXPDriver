#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char log_path[MAX_PATH];

static void log_line(const char *fmt, ...)
{
    char line[512];
    DWORD written;
    HANDLE file;
    int len;
    va_list ap;

    va_start(ap, fmt);
    len = _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    if (len < 0) {
        len = sizeof(line) - 3;
    }
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = 0;

    file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(file, line, len, &written, NULL);
    CloseHandle(file);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL spawn_peer(const char *argument)
{
    char executable[MAX_PATH];
    char launch[MAX_PATH + 32];
    PROCESS_INFORMATION process;
    STARTUPINFOA startup;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (GetModuleFileNameA(NULL, executable, sizeof(executable)) &&
        _snprintf(launch, sizeof(launch), "\"%s\" %s", executable,
                  argument) > 0 &&
        CreateProcessA(NULL, launch, NULL, NULL, FALSE, 0, NULL, NULL,
                       &startup, &process)) {
        log_line("SECOND_STARTED pid=%lu", process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return TRUE;
    }
    log_line("SECOND_START_FAIL error=%lu", GetLastError());
    return FALSE;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    PIXELFORMATDESCRIPTOR pfd;
    WNDCLASSA wc;
    HGLRC context = NULL;
    DWORD start, now;
    MSG msg;
    HWND window;
    HDC dc;
    int format;
    int exit_code = 0;
    BOOL spawn_second = command && !strcmp(command, "first");
    BOOL late_bind = command && !strcmp(command, "revfirst");
    BOOL second_started = FALSE;
    const char *tag = command && *command ? command : "owner";

    (void)previous;
    (void)show;
    _snprintf(log_path, sizeof(log_path), "C:\\XPDRIVER-GL-%s.LOG", tag);
    DeleteFileA(log_path);
    log_line("START pid=%lu", GetCurrentProcessId());

    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "XPDriverGlOwner";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log_line("REGISTER_CLASS_FAIL error=%lu", GetLastError());
        return 10;
    }

    window = CreateWindowA(wc.lpszClassName, tag,
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                           NULL, NULL, instance, NULL);
    if (!window) {
        log_line("CREATE_WINDOW_FAIL error=%lu", GetLastError());
        return 11;
    }
    dc = GetDC(window);

    if (late_bind) {
        /* Reverse launch order: this process loaded the wrapper FIRST
         * (its DllMain claimed the shared ring at process start), but it
         * binds LAST. Spawn the second-loaded process now; that process
         * ring-sniff-shares, binds first, and must own the session. */
        DWORD wait_start = GetTickCount();

        if (!spawn_peer("second")) {
            exit_code = 16;
            goto done;
        }
        while (GetTickCount() - wait_start < 8000) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(50);
        }
        log_line("LATE_BIND_BEGIN");
    }

    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(dc, &pfd);
    log_line("CHOOSE format=%d error=%lu", format, GetLastError());
    if (!format || !SetPixelFormat(dc, format, &pfd)) {
        log_line("SET_PIXEL_FORMAT_FAIL format=%d error=%lu", format,
                 GetLastError());
        if (late_bind) {
            /* The expected clean refusal: the second-loaded process owns
             * the session and this late bind must not displace it. */
            log_line("REVERSE_REFUSED_OK");
            exit_code = 0;
        } else {
            exit_code = 12;
        }
        goto done;
    }
    log_line("SET_PIXEL_FORMAT_OK format=%d", format);
    if (late_bind) {
        /* Misgrant: two live processes now both hold a granted format on
         * one shared ring. Exit without creating a context so the owner
         * is not actively corrupted while the failure is recorded. */
        log_line("REVERSE_MISGRANT");
        exit_code = 99;
        goto done;
    }

    context = wglCreateContext(dc);
    if (!context) {
        log_line("CREATE_CONTEXT_FAIL error=%lu", GetLastError());
        exit_code = 13;
        goto done;
    }
    if (!wglMakeCurrent(dc, context)) {
        log_line("MAKE_CURRENT_FAIL error=%lu", GetLastError());
        exit_code = 14;
        goto done;
    }
    log_line("RENDERER %s", glGetString(GL_RENDERER));
    log_line("RENDER_BEGIN");

    start = GetTickCount();
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto render_done;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        now = GetTickCount() - start;
        if (spawn_second && !second_started && now >= 5000) {
            second_started = TRUE;
            spawn_peer("second");
        }
        glViewport(0, 0, 640, 480);
        glClearColor(((now / 13) & 255) / 255.0f,
                     ((now / 29) & 255) / 255.0f,
                     ((now / 47) & 255) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!SwapBuffers(dc)) {
            log_line("SWAP_FAIL error=%lu", GetLastError());
            exit_code = 15;
            break;
        }
        Sleep(16);
    }

render_done:
    log_line("RENDER_END");
done:
    if (context) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context);
    }
    ReleaseDC(window, dc);
    DestroyWindow(window);
    log_line("EXIT code=%d", exit_code);
    return exit_code;
}
