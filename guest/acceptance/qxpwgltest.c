#include <winsock2.h>
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#include "qxp_escape.h"
#include "../qxpresult.h"

#define ICD_KEY "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\qxpicd"

static int (WINAPI *real_ext_escape)(HDC, int, int, LPCSTR, int, LPSTR);
static FILE *log_file;
static DWORD result_error;

static void report_status(const char *message)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    int length = lstrlenA(message);
    unsigned port;

    qxp_report_serial(message);
    if (WSAStartup(MAKEWORD(2, 0), &winsock)) return;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle != INVALID_SOCKET) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("10.0.2.2");
        if (length > 0) {
            for (port = 44574; port <= 44590; ++port) {
                address.sin_port = htons((u_short)port);
                sendto(socket_handle, message, length, 0,
                       (const struct sockaddr *)&address, sizeof(address));
            }
        }
        closesocket(socket_handle);
    }
    WSACleanup();
}

static void log_line(const char *text, DWORD value)
{
    if (!log_file) return;
    fprintf(log_file, "%s=%lu\n", text, (unsigned long)value);
    fflush(log_file);
}

static void report_result(int rc)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    char message[64];
    int length;
    unsigned port;

    length = result_error ?
        _snprintf(message, sizeof(message),
                  "QXPICD_RESULT rc=%d error=%lu", rc,
                  (unsigned long)result_error) :
        _snprintf(message, sizeof(message),
                  "QXPICD_RESULT rc=%d", rc);
    if (length <= 0) return;
    if (length >= (int)sizeof(message)) length = sizeof(message) - 1;
    qxp_report_serial(message);
    if (WSAStartup(MAKEWORD(2, 0), &winsock)) return;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle != INVALID_SOCKET) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("10.0.2.2");
        for (port = 44574; port <= 44590; ++port) {
            address.sin_port = htons((u_short)port);
            sendto(socket_handle, message, length, 0,
                   (const struct sockaddr *)&address, sizeof(address));
        }
        closesocket(socket_handle);
    }
    WSACleanup();
}

static int WINAPI mock_ext_escape(HDC dc, int escape, int input_size,
                                  LPCSTR input, int output_size, LPSTR output)
{
    if (escape == QUERYESCSUPPORT && input &&
        input_size >= (int)sizeof(DWORD) &&
        *(const DWORD *)input == QXP_OPENGL_GETINFO) {
        return (int)qxp_escape((uint32_t)escape, (uint32_t)input_size, input,
                               (uint32_t)output_size, output);
    }
    if (escape == QXP_OPENGL_GETINFO) {
        return (int)qxp_escape((uint32_t)escape, (uint32_t)input_size, input,
                               (uint32_t)output_size, output);
    }
    return real_ext_escape ? real_ext_escape(dc, escape, input_size, input,
                                              output_size, output) : 0;
}

static int patch_import(HMODULE module, const char *dll_wanted,
                        const char *function_wanted, PROC replacement,
                        PROC *original)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;
    DWORD old_protect;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS32 *)((BYTE *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)module +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char *dll_name = (const char *)module + descriptor->Name;
        IMAGE_THUNK_DATA32 *names;
        IMAGE_THUNK_DATA32 *iat;

        if (lstrcmpiA(dll_name, dll_wanted)) continue;
        names = (IMAGE_THUNK_DATA32 *)((BYTE *)module +
            (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk :
                                              descriptor->FirstThunk));
        iat = (IMAGE_THUNK_DATA32 *)((BYTE *)module +
                                     descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            IMAGE_IMPORT_BY_NAME *name;

            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            name = (IMAGE_IMPORT_BY_NAME *)((BYTE *)module +
                                             names->u1.AddressOfData);
            if (strcmp((const char *)name->Name, function_wanted)) continue;
            *original = (PROC)(ULONG_PTR)iat->u1.Function;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                                PAGE_READWRITE, &old_protect)) return 0;
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

static int install_icd(void)
{
    char source[MAX_PATH];
    char target[MAX_PATH];
    char *slash;
    HKEY key;
    DWORD version = 2;
    DWORD driver_version = 1;
    DWORD flags = 1;
    LONG status;

    if (!GetModuleFileNameA(NULL, source, sizeof(source))) return 0;
    slash = strrchr(source, '\\');
    if (!slash) return 0;
    strcpy(slash + 1, "qxpicd.dll");
    if (!GetSystemDirectoryA(target, sizeof(target)) ||
        strlen(target) + sizeof("\\qxpicd.dll") > sizeof(target)) return 0;
    strcat(target, "\\qxpicd.dll");
    /* An installed display package can make its catalog-owned ICD
     * non-overwritable.  Controlled WGL mode only needs to supply a missing
     * file; if the installed target exists, retain it and refresh the tuple. */
    if (!CopyFileA(source, target, FALSE) &&
        GetFileAttributesA(target) == INVALID_FILE_ATTRIBUTES) return 0;

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, ICD_KEY, 0, NULL, 0,
                             KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) return 0;
    status = RegSetValueExA(key, "Dll", 0, REG_SZ,
                            (const BYTE *)"qxpicd.dll", 11);
    if (status == ERROR_SUCCESS) status = RegSetValueExA(
        key, "Version", 0, REG_DWORD, (const BYTE *)&version, sizeof(version));
    if (status == ERROR_SUCCESS) status = RegSetValueExA(
        key, "DriverVersion", 0, REG_DWORD,
        (const BYTE *)&driver_version, sizeof(driver_version));
    if (status == ERROR_SUCCESS) status = RegSetValueExA(
        key, "Flags", 0, REG_DWORD, (const BYTE *)&flags, sizeof(flags));
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) status = RegFlushKey(HKEY_LOCAL_MACHINE);
    return status == ERROR_SUCCESS;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static int fallback_child(int native_escape)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR request;
    PIXELFORMATDESCRIPTOR actual;
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    HWND window = NULL;
    HDC dc = NULL;
    HGLRC context = NULL;
    const GLubyte *renderer;
    int format;
    int rc = 20;

    log_file = fopen("C:\\QXPWGL-CHILD.LOG", "w");
    if (!native_escape &&
        !patch_import(opengl, "GDI32.dll", "ExtEscape",
                      (PROC)mock_ext_escape, (PROC *)&real_ext_escape)) {
        rc = 21;
        goto done;
    }
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPWGLFallback";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        rc = 22;
        goto done;
    }
    window = CreateWindowA(wc.lpszClassName, "QXPICD fallback acceptance",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!window) { rc = 22; goto done; }
    dc = GetDC(window);
    memset(&request, 0, sizeof(request));
    request.nSize = sizeof(request);
    request.nVersion = 1;
    request.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                      PFD_DOUBLEBUFFER;
    request.iPixelType = PFD_TYPE_RGBA;
    request.cColorBits = 32;
    request.cDepthBits = 24;
    request.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &request);
    memset(&actual, 0, sizeof(actual));
    if (!format || !DescribePixelFormat(dc, format, sizeof(actual), &actual) ||
        !(actual.dwFlags & PFD_GENERIC_FORMAT)) {
        rc = 23;
        goto done;
    }
    if (!SetPixelFormat(dc, format, &actual)) { rc = 24; goto done; }
    context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) { rc = 25; goto done; }
    renderer = glGetString(GL_RENDERER);
    if (!renderer || !strstr((const char *)renderer, "GDI Generic")) {
        rc = 26;
        goto done;
    }
    fprintf(log_file, "FORMAT=%d FLAGS=%08lx RENDERER=%s\n", format,
            (unsigned long)actual.dwFlags, (const char *)renderer);
    glViewport(0, 0, 320, 240);
    glClearColor(0.1f, 0.15f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    rc = SwapBuffers(dc) ? 0 : 27;

done:
    if (context) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context);
    }
    if (dc) ReleaseDC(window, dc);
    if (window) DestroyWindow(window);
    log_line("RESULT", rc);
    if (log_file) fclose(log_file);
    return rc;
}

static int launch_fallback_child(int native_escape)
{
    char executable[MAX_PATH];
    char command[MAX_PATH + 32];
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD result;

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!GetModuleFileNameA(NULL, executable, sizeof(executable)) ||
        _snprintf(command, sizeof(command), "\"%s\" --fallback-child%s",
                  executable, native_escape ? " --native" : "") < 0 ||
        !CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process)) {
        return 30;
    }
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &result)) {
        TerminateProcess(process.hProcess, 31);
        CloseHandle(process.hProcess);
        return 31;
    }
    CloseHandle(process.hProcess);
    return (int)result;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    HMODULE opengl;
    HWND window = NULL;
    HDC dc = NULL;
    HGLRC context = NULL;
    const GLubyte *renderer;
    int format;
    int frame;
    int rc = 1;
    int native_escape = 0;

    if (argc > 1 && !strcmp(argv[1], "--fallback-child")) {
        return fallback_child(argc > 2 && !strcmp(argv[2], "--native"));
    }
    native_escape = argc > 1 && !strcmp(argv[1], "--native");

    log_file = fopen("C:\\QXPWGL-TEST.LOG", "w");
    if (!log_file) goto done;
    if (!native_escape && !install_icd()) { rc = 2; goto done; }
    opengl = GetModuleHandleA("opengl32.dll");
    log_line("NATIVE_ESCAPE", native_escape);
    if (!native_escape &&
        !patch_import(opengl, "GDI32.dll", "ExtEscape",
                      (PROC)mock_ext_escape, (PROC *)&real_ext_escape)) {
        rc = 3;
        goto done;
    }

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPWGLTest";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        rc = 4;
        goto done;
    }
    window = CreateWindowA(wc.lpszClassName, "QXPICD WGL acceptance",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                           CW_USEDEFAULT, 640, 480, NULL, NULL, wc.hInstance,
                           NULL);
    if (!window) { rc = 4; goto done; }
    dc = GetDC(window);
    if (native_escape) {
        QXP_OPENGL_INFO info;
        DWORD query = QXP_OPENGL_GETINFO;
        DWORD subescape = QXP_OPENGL_GETINFO_DRVNAME;
        char name[16];
        char message[160];
        int query_result;
        int info_result;
        unsigned index;

        memset(&info, 0, sizeof(info));
        memset(name, 0, sizeof(name));
        query_result = ExtEscape(dc, QUERYESCSUPPORT, sizeof(query),
                                 (const char *)&query, 0, NULL);
        info_result = ExtEscape(dc, QXP_OPENGL_GETINFO, sizeof(subescape),
                                (const char *)&subescape, sizeof(info),
                                (char *)&info);
        for (index = 0; index + 1 < sizeof(name) &&
                        index < sizeof(info.driver_name) /
                                    sizeof(info.driver_name[0]); ++index) {
            name[index] = (char)info.driver_name[index];
            if (!name[index]) break;
        }
        _snprintf(message, sizeof(message),
                  "QXP_WGL_STAGE escape-query=%d getinfo=%d version=%lu driver-version=%lu name=%s",
                  query_result, info_result, (unsigned long)info.version,
                  (unsigned long)info.driver_version, name);
        message[sizeof(message) - 1] = '\0';
        report_status(message);
        {
            HKEY key;
            char dll[32];
            char system_path[MAX_PATH];
            DWORD type;
            DWORD bytes;
            DWORD version = 0;
            DWORD driver_version = 0;
            DWORD flags = 0;
            LONG open_status;
            int file_present = 0;

            memset(dll, 0, sizeof(dll));
            open_status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, ICD_KEY, 0,
                                        KEY_QUERY_VALUE, &key);
            if (open_status == ERROR_SUCCESS) {
                bytes = sizeof(dll);
                RegQueryValueExA(key, "Dll", NULL, &type,
                                 (BYTE *)dll, &bytes);
                bytes = sizeof(version);
                RegQueryValueExA(key, "Version", NULL, &type,
                                 (BYTE *)&version, &bytes);
                bytes = sizeof(driver_version);
                RegQueryValueExA(key, "DriverVersion", NULL, &type,
                                 (BYTE *)&driver_version, &bytes);
                bytes = sizeof(flags);
                RegQueryValueExA(key, "Flags", NULL, &type,
                                 (BYTE *)&flags, &bytes);
                RegCloseKey(key);
            }
            if (GetSystemDirectoryA(system_path, sizeof(system_path)) &&
                strlen(system_path) + sizeof("\\qxpicd.dll") <=
                    sizeof(system_path)) {
                strcat(system_path, "\\qxpicd.dll");
                file_present = GetFileAttributesA(system_path) !=
                               INVALID_FILE_ATTRIBUTES;
            }
            _snprintf(message, sizeof(message),
                      "QXP_WGL_STAGE registry=%ld dll=%s version=%lu driver-version=%lu flags=%lu file=%d",
                      (long)open_status, dll, (unsigned long)version,
                      (unsigned long)driver_version, (unsigned long)flags,
                      file_present);
            message[sizeof(message) - 1] = '\0';
            report_status(message);
        }
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &pfd);
    log_line("CHOOSE", format);
    if (!format) {
        result_error = (DWORD)format;
        rc = 5;
        goto done;
    }
    if (!SetPixelFormat(dc, format, &pfd)) {
        result_error = GetLastError();
        rc = 12;
        goto done;
    }
    context = wglCreateContext(dc);
    if (!context) {
        result_error = GetLastError();
        log_line("CREATE_CONTEXT_ERROR", result_error);
        rc = 6;
        goto done;
    }
    if (!wglMakeCurrent(dc, context)) {
        result_error = GetLastError();
        log_line("MAKE_CURRENT_ERROR", result_error);
        rc = 10;
        goto done;
    }
    renderer = glGetString(GL_RENDERER);
    if (!renderer) {
        result_error = GetModuleHandleA("qxpicd.dll") ? 101 : 100;
        rc = 7;
        goto done;
    }
    fprintf(log_file, "RENDERER=%s\n", (const char *)renderer);
    fflush(log_file);
    if (strstr((const char *)renderer, "GDI Generic")) {
        result_error = GetModuleHandleA("qxpicd.dll") ? 201 : 200;
        rc = 7;
        goto done;
    }
    rc = launch_fallback_child(native_escape);
    if (rc != 0) {
        rc += 100;
        goto done;
    }
    renderer = glGetString(GL_RENDERER);
    if (!renderer || strstr((const char *)renderer, "GDI Generic")) {
        rc = 11;
        goto done;
    }

    for (frame = 0; frame < 90; ++frame) {
        glViewport(0, 0, 640, 480);
        glClearColor(0.03f, 0.04f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.1f, 0.1f);
        glVertex3f(-0.75f, -0.65f, 0.0f);
        glColor3f(0.1f, 1.0f, 0.1f);
        glVertex3f(0.75f, -0.65f, 0.0f);
        glColor3f(0.1f, 0.3f, 1.0f);
        glVertex3f(0.0f, 0.75f, 0.0f);
        glEnd();
        if (!SwapBuffers(dc)) { rc = 8; goto done; }
        Sleep(16);
    }
    rc = glGetError() == GL_NO_ERROR ? 0 : 9;

done:
    if (context) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context);
    }
    if (dc) ReleaseDC(window, dc);
    if (window) DestroyWindow(window);
    log_line("RESULT", rc);
    if (log_file) fclose(log_file);
    report_result(rc);
    return rc;
}
