#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

#define QUAKE_DIRECTORY "C:\\WINDOWS\\desktop\\Quake2 Demo\\Install\\Data"
#define QUAKE_EXE QUAKE_DIRECTORY "\\quake2.exe"
#define PRIVATE_OPENGL QUAKE_DIRECTORY "\\opengl32.dll"
#define QUAKE_LOG QUAKE_DIRECTORY "\\baseq2\\qconsole.log"

typedef struct quake_window_search {
    DWORD process_id;
    HWND window;
    LONG area;
} QUAKE_WINDOW_SEARCH;

static BOOL CALLBACK find_quake_window_callback(HWND window, LPARAM opaque)
{
    QUAKE_WINDOW_SEARCH *search = (QUAKE_WINDOW_SEARCH *)opaque;
    DWORD process_id = 0;
    RECT rect;
    LONG width;
    LONG height;
    LONG area;

    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search->process_id || !IsWindowVisible(window) ||
        !GetWindowRect(window, &rect)) return TRUE;
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 0x7fffffffL / height)
        return TRUE;
    area = width * height;
    if (!search->window || area > search->area) {
        search->window = window;
        search->area = area;
    }
    return TRUE;
}

static HWND find_quake_window(DWORD process_id)
{
    QUAKE_WINDOW_SEARCH search;

    search.process_id = process_id;
    search.window = NULL;
    search.area = 0;
    EnumWindows(find_quake_window_callback, (LPARAM)&search);
    return search.window;
}

static void send_message(const char *message)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    int length = (int)strlen(message);
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

static void report_stage(const char *stage)
{
    char message[128];

    if (_snprintf(message, sizeof(message), "QXP_GAME_STAGE stage=%s",
                  stage) > 0) {
        message[sizeof(message) - 1] = '\0';
        send_message(message);
    }
}

static void report_result(int rc, const char *detail)
{
    char message[384];

    if (detail && detail[0]) {
        _snprintf(message, sizeof(message),
                  "QXP_GAME_RESULT rc=%d %s", rc, detail);
    } else {
        _snprintf(message, sizeof(message), "QXP_GAME_RESULT rc=%d", rc);
    }
    message[sizeof(message) - 1] = '\0';
    send_message(message);
}

static int read_log(char *buffer, size_t size)
{
    FILE *file;
    size_t count;

    file = fopen(QUAKE_LOG, "rb");
    if (!file) return 0;
    count = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[count] = '\0';
    return count > 0;
}

static void copy_log_value(char *output, size_t output_size,
                           const char *log, const char *label)
{
    const char *value = strstr(log, label);
    size_t length = 0;

    if (!output_size) return;
    output[0] = '\0';
    if (!value) return;
    value += strlen(label);
    while (*value == ' ' || *value == '\t') ++value;
    while (value[length] && value[length] != '\r' && value[length] != '\n' &&
           length + 1 < output_size) {
        output[length] = value[length];
        ++length;
    }
    output[length] = '\0';
}

static int renderer_initialized(const char *log)
{
    const char *version = strstr(log, "GL_VERSION:");

    return version && strstr(version, "\n------------------------------------");
}

int main(int argc, char **argv)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD wait_status;
    DWORD exit_code = 0;
    DWORD started;
    DWORD ready_at = 0;
    DWORD focus_attempt;
    HWND quake_window = NULL;
    char command[768];
    char log_buffer[65536];
    char renderer[160];
    char detail[256];
    int rc = 0;
    int performance = 0;
    int performance_reported = 0;

    if (argc == 2 && !_stricmp(argv[1], "--performance")) {
        performance = 1;
    } else if (argc != 1) {
        report_result(11, "error=arguments expected=--performance");
        return 11;
    }

    if (GetFileAttributesA(QUAKE_EXE) == INVALID_FILE_ATTRIBUTES) {
        report_result(2, "error=quake2-missing");
        return 2;
    }
    if (GetFileAttributesA(PRIVATE_OPENGL) != INVALID_FILE_ATTRIBUTES) {
        report_result(3, "error=private-opengl32");
        return 3;
    }

    DeleteFileA(QUAKE_LOG);
    _snprintf(command, sizeof(command),
              performance ?
              "\"%s\" +set vid_ref gl +set gl_driver opengl32 "
              "+set vid_fullscreen 0 +set gl_mode 3 +set timedemo 1 "
              "+set cl_maxfps 1000 +set gl_swapinterval 0 "
              "+set logfile 2 +map demo1" :
              "\"%s\" +set vid_ref gl +set gl_driver opengl32 "
              "+set vid_fullscreen 0 +set logfile 2 +map demo1", QUAKE_EXE);
    command[sizeof(command) - 1] = '\0';

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(QUAKE_EXE, command, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP, NULL, QUAKE_DIRECTORY,
                        &startup, &process)) {
        _snprintf(detail, sizeof(detail), "error=create-process winerr=%lu",
                  (unsigned long)GetLastError());
        detail[sizeof(detail) - 1] = '\0';
        report_result(4, detail);
        return 4;
    }
    report_stage("process-started");

    started = GetTickCount();
    do {
        wait_status = WaitForSingleObject(process.hProcess, 500);
        if (wait_status == WAIT_OBJECT_0) {
            GetExitCodeProcess(process.hProcess, &exit_code);
            _snprintf(detail, sizeof(detail),
                      "error=early-exit exit=%lu",
                      (unsigned long)exit_code);
            detail[sizeof(detail) - 1] = '\0';
            rc = 5;
            break;
        }
        if (!ready_at && read_log(log_buffer, sizeof(log_buffer))) {
            if (renderer_initialized(log_buffer) &&
                strstr(log_buffer, "Map: demo1")) {
                ready_at = GetTickCount();
                report_stage("rendering");
                if (performance) {
                    for (focus_attempt = 0; focus_attempt < 20;
                         ++focus_attempt) {
                        quake_window = find_quake_window(process.dwProcessId);
                        if (quake_window) break;
                        Sleep(100);
                    }
                    if (!quake_window) {
                        rc = 14;
                        strcpy(detail, "error=quake-window-missing");
                        break;
                    }
                    ShowWindow(quake_window, SW_RESTORE);
                    SetForegroundWindow(quake_window);
                    Sleep(250);
                    if (GetForegroundWindow() != quake_window) {
                        rc = 15;
                        strcpy(detail, "error=quake-window-not-foreground");
                        break;
                    }
                    report_stage("foreground-ready");
                    copy_log_value(renderer, sizeof(renderer), log_buffer,
                                   "GL_RENDERER:");
                    if (!renderer[0]) {
                        rc = 7;
                        strcpy(detail, "error=no-renderer");
                        break;
                    }
                    if (strstr(renderer, "GDI Generic")) {
                        rc = 8;
                        strcpy(detail, "error=gdi-generic");
                        break;
                    }
                    _snprintf(detail, sizeof(detail),
                              "renderer=%s benchmark-ready=1 timedemo=1 "
                              "cl_maxfps=1000 foreground=1", renderer);
                    detail[sizeof(detail) - 1] = '\0';
                    report_stage("benchmark-ready");
                    report_result(0, detail);
                    performance_reported = 1;
                }
            }
        }
        if (performance_reported && GetForegroundWindow() != quake_window) {
            ShowWindow(quake_window, SW_RESTORE);
            SetForegroundWindow(quake_window);
            Sleep(250);
            if (GetForegroundWindow() != quake_window) {
                rc = 15;
                strcpy(detail, "error=quake-window-lost-foreground");
                break;
            }
        }
        if (ready_at && !performance &&
            GetTickCount() - ready_at >= 10000UL) break;
    } while (performance || GetTickCount() - started < 45000UL);

    if (!rc && !performance) {
        if (!read_log(log_buffer, sizeof(log_buffer))) {
            rc = 6;
            strcpy(detail, "error=no-qconsole-log");
        } else if (!renderer_initialized(log_buffer)) {
            rc = 9;
            strcpy(detail, "error=renderer-init-incomplete");
        } else if (!strstr(log_buffer, "Map: demo1")) {
            rc = 10;
            strcpy(detail, "error=map-not-loaded");
        } else {
            copy_log_value(renderer, sizeof(renderer), log_buffer,
                           "GL_RENDERER:");
            if (!renderer[0]) {
                rc = 7;
                strcpy(detail, "error=no-renderer");
            } else if (strstr(renderer, "GDI Generic")) {
                rc = 8;
                strcpy(detail, "error=gdi-generic");
            } else {
                _snprintf(detail, sizeof(detail), "renderer=%s", renderer);
                detail[sizeof(detail) - 1] = '\0';
            }
        }
    }

    if (ready_at && !performance) report_stage("observation-complete");
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (rc || !performance_reported) report_result(rc, detail);
    return rc;
}
