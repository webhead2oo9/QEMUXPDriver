#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"
#include "qxp_process_lineage.h"

#define MAX_PAYNE_DIR "C:\\Program Files\\Max Payne Demo"
#define MAX_PAYNE_NAME "MaxPayneDemo.exe"
#define MAX_PAYNE_EXE MAX_PAYNE_DIR "\\" MAX_PAYNE_NAME

typedef struct window_search {
    DWORD process_id;
    HWND window;
    LONG area;
} WINDOW_SEARCH;

typedef struct button_search {
    HWND button;
    char caption[64];
} BUTTON_SEARCH;

static void send_message_all_ports(const char *message)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    int length;
    unsigned port;

    qxp_report_serial(message);
    if (WSAStartup(MAKEWORD(2, 0), &winsock)) return;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle != INVALID_SOCKET) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("10.0.2.2");
        length = (int)strlen(message);
        for (port = 44574; port <= 44590; ++port) {
            address.sin_port = htons((u_short)port);
            sendto(socket_handle, message, length, 0,
                   (const struct sockaddr *)&address, sizeof(address));
        }
        closesocket(socket_handle);
    }
    WSACleanup();
}

static void report_stage(const char *stage, const char *detail)
{
    char message[384];

    if (detail && detail[0]) {
        _snprintf(message, sizeof(message),
                  "QXP_D3D_GAME_STAGE rc=0 stage=%s %s", stage, detail);
    } else {
        _snprintf(message, sizeof(message),
                  "QXP_D3D_GAME_STAGE rc=0 stage=%s", stage);
    }
    message[sizeof(message) - 1] = '\0';
    send_message_all_ports(message);
}

static int report_result(int rc, const char *stage, const char *detail)
{
    char message[384];

    if (detail && detail[0]) {
        _snprintf(message, sizeof(message),
                  "QXP_D3D_GAME_RESULT rc=%d stage=%s %s",
                  rc, stage, detail);
    } else {
        _snprintf(message, sizeof(message),
                  "QXP_D3D_GAME_RESULT rc=%d stage=%s", rc, stage);
    }
    message[sizeof(message) - 1] = '\0';
    send_message_all_ports(message);
    return rc;
}

static void inventory_executables(void)
{
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle;
    char pattern[MAX_PATH];
    char detail[192];
    unsigned count = 0;

    _snprintf(pattern, sizeof(pattern), "%s\\*.exe", MAX_PAYNE_DIR);
    pattern[sizeof(pattern) - 1] = '\0';
    find_handle = FindFirstFileA(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        _snprintf(detail, sizeof(detail), "winerr=%lu",
                  (unsigned long)GetLastError());
        detail[sizeof(detail) - 1] = '\0';
        report_stage("inventory-empty", detail);
        return;
    }
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            _snprintf(detail, sizeof(detail), "exe=%s", find_data.cFileName);
            detail[sizeof(detail) - 1] = '\0';
            report_stage("inventory", detail);
            ++count;
        }
    } while (FindNextFileA(find_handle, &find_data));
    FindClose(find_handle);
    _snprintf(detail, sizeof(detail), "count=%u", count);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("inventory-complete", detail);
}

static const char *find_private_runtime(void)
{
    static const char *files[] = {
        "d3d8.dll", "d3d9.dll", "ddraw.dll", "wined3d.dll",
        "wined8.dll", "wined9.dll", "winedd.dll", "opengl32.dll",
        "wrapgl32.ext"
    };
    static char found[32];
    char path[MAX_PATH];
    unsigned i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        _snprintf(path, sizeof(path), "%s\\%s", MAX_PAYNE_DIR, files[i]);
        path[sizeof(path) - 1] = '\0';
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            strcpy(found, files[i]);
            return found;
        }
    }
    return NULL;
}

static BOOL CALLBACK find_window_callback(HWND window, LPARAM opaque)
{
    WINDOW_SEARCH *search = (WINDOW_SEARCH *)opaque;
    DWORD process_id = 0;
    RECT rect;
    LONG width;
    LONG height;
    LONG area;
    char title[2];

    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search->process_id || !IsWindowVisible(window) ||
        !GetWindowTextA(window, title, sizeof(title)) ||
        !GetWindowRect(window, &rect)) {
        return TRUE;
    }
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 0x7fffffffL / height) {
        return TRUE;
    }
    area = width * height;
    if (!search->window || area > search->area ||
        (window == GetForegroundWindow() &&
         search->window != GetForegroundWindow())) {
        search->window = window;
        search->area = area;
    }
    return TRUE;
}

static HWND find_process_window(DWORD process_id)
{
    WINDOW_SEARCH search;

    search.process_id = process_id;
    search.window = NULL;
    search.area = 0;
    EnumWindows(find_window_callback, (LPARAM)&search);
    return search.window;
}

static void sanitize_control_text(char *text)
{
    size_t index;

    for (index = 0; text[index]; ++index) {
        if (text[index] == '\r' || text[index] == '\n' ||
            text[index] == '\t') {
            text[index] = ' ';
        }
    }
}

static BOOL CALLBACK report_control_callback(HWND window, LPARAM opaque)
{
    char class_name[32];
    char caption[224];
    char detail[320];
    LRESULT count;
    LRESULT selected;

    (void)opaque;
    if (!IsWindowVisible(window) ||
        !GetClassNameA(window, class_name, sizeof(class_name)) ||
        !GetWindowTextA(window, caption, sizeof(caption))) {
        return TRUE;
    }
    class_name[sizeof(class_name) - 1] = '\0';
    caption[sizeof(caption) - 1] = '\0';
    sanitize_control_text(caption);
    _snprintf(detail, sizeof(detail), "class=%s text=%s",
              class_name, caption);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("control", detail);
    if (!_stricmp(class_name, "ComboBox")) {
        count = SendMessageA(window, CB_GETCOUNT, 0, 0);
        selected = SendMessageA(window, CB_GETCURSEL, 0, 0);
        _snprintf(detail, sizeof(detail),
                  "class=ComboBox count=%ld selected=%ld",
                  (long)count, (long)selected);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("control-state", detail);
    } else if (!_stricmp(class_name, "ListBox")) {
        count = SendMessageA(window, LB_GETCOUNT, 0, 0);
        selected = SendMessageA(window, LB_GETCURSEL, 0, 0);
        _snprintf(detail, sizeof(detail),
                  "class=ListBox count=%ld selected=%ld",
                  (long)count, (long)selected);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("control-state", detail);
    }
    return TRUE;
}

static BOOL CALLBACK select_unset_choice_callback(HWND window, LPARAM opaque)
{
    char class_name[32];
    LRESULT count;
    LRESULT selected;
    UINT notification;
    UINT control_id;
    HWND parent;

    (void)opaque;
    if (!IsWindowVisible(window) || !IsWindowEnabled(window) ||
        !GetClassNameA(window, class_name, sizeof(class_name))) {
        return TRUE;
    }
    class_name[sizeof(class_name) - 1] = '\0';
    if (!_stricmp(class_name, "ComboBox")) {
        count = SendMessageA(window, CB_GETCOUNT, 0, 0);
        selected = SendMessageA(window, CB_GETCURSEL, 0, 0);
        notification = CBN_SELCHANGE;
        if (count > 0 && selected == CB_ERR) {
            SendMessageA(window, CB_SETCURSEL, 0, 0);
        } else {
            return TRUE;
        }
    } else if (!_stricmp(class_name, "ListBox")) {
        count = SendMessageA(window, LB_GETCOUNT, 0, 0);
        selected = SendMessageA(window, LB_GETCURSEL, 0, 0);
        notification = LBN_SELCHANGE;
        if (count > 0 && selected == LB_ERR) {
            SendMessageA(window, LB_SETCURSEL, 0, 0);
        } else {
            return TRUE;
        }
    } else {
        return TRUE;
    }
    parent = GetParent(window);
    control_id = (UINT)GetDlgCtrlID(window);
    if (parent) {
        SendMessageA(parent, WM_COMMAND,
                     MAKEWPARAM(control_id, notification), (LPARAM)window);
    }
    return TRUE;
}

static int caption_is_launch_action(const char *caption)
{
    char lower[64];
    size_t i;

    for (i = 0; caption[i] && i + 1 < sizeof(lower); ++i) {
        char ch = caption[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    lower[i] = '\0';
    return strstr(lower, "play") || strstr(lower, "start") ||
           strstr(lower, "launch") || !strcmp(lower, "ok");
}

static BOOL CALLBACK find_button_callback(HWND window, LPARAM opaque)
{
    BUTTON_SEARCH *search = (BUTTON_SEARCH *)opaque;
    char class_name[16];
    char caption[64];

    if (!GetClassNameA(window, class_name, sizeof(class_name)) ||
        _stricmp(class_name, "Button") ||
        !GetWindowTextA(window, caption, sizeof(caption)) ||
        !caption_is_launch_action(caption) || !IsWindowEnabled(window)) {
        return TRUE;
    }
    search->button = window;
    strcpy(search->caption, caption);
    return FALSE;
}

static int activate_launcher(HWND window, int use_escape,
                             char *detail, size_t detail_size)
{
    BUTTON_SEARCH search;

    EnumChildWindows(window, select_unset_choice_callback, 0);
    search.button = NULL;
    search.caption[0] = '\0';
    EnumChildWindows(window, find_button_callback, (LPARAM)&search);
    SetForegroundWindow(window);
    if (search.button) {
        SendMessageA(search.button, BM_CLICK, 0, 0);
        _snprintf(detail, detail_size, "action=button caption=%s",
                  search.caption);
        detail[detail_size - 1] = '\0';
        return 1;
    }
    if (use_escape) {
        PostMessageA(window, WM_KEYDOWN, VK_ESCAPE, 0);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        Sleep(30);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        PostMessageA(window, WM_KEYUP, VK_ESCAPE, 0);
        _snprintf(detail, detail_size, "action=escape");
    } else {
        PostMessageA(window, WM_COMMAND, IDOK, 0);
        PostMessageA(window, WM_KEYDOWN, VK_RETURN, 0);
        keybd_event(VK_RETURN, 0, 0, 0);
        Sleep(30);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
        PostMessageA(window, WM_KEYUP, VK_RETURN, 0);
        _snprintf(detail, detail_size, "action=enter");
    }
    detail[detail_size - 1] = '\0';
    return 0;
}

static void describe_window(HWND window, char *detail, size_t detail_size)
{
    char title[160];
    char class_name[64];
    RECT rect;
    RECT client;

    title[0] = '\0';
    class_name[0] = '\0';
    memset(&rect, 0, sizeof(rect));
    memset(&client, 0, sizeof(client));
    GetWindowTextA(window, title, sizeof(title));
    GetClassNameA(window, class_name, sizeof(class_name));
    GetWindowRect(window, &rect);
    GetClientRect(window, &client);
    title[sizeof(title) - 1] = '\0';
    class_name[sizeof(class_name) - 1] = '\0';
    sanitize_control_text(title);
    _snprintf(detail, detail_size,
              "hwnd=%08lx class=%s title=%s rect=%ld,%ld,%ld,%ld "
              "client=%ldx%ld foreground=%d",
              (unsigned long)(ULONG_PTR)window,
              class_name[0] ? class_name : "(none)",
              title[0] ? title : "(none)",
              (long)rect.left, (long)rect.top,
              (long)rect.right, (long)rect.bottom,
              (long)(client.right - client.left),
              (long)(client.bottom - client.top),
              window == GetForegroundWindow());
    detail[detail_size - 1] = '\0';
}

static void report_window(HWND window, int report_controls)
{
    char detail[320];

    describe_window(window, detail, sizeof(detail));
    report_stage("window", detail);
    if (report_controls) EnumChildWindows(window, report_control_callback, 0);
}

static int process_has_system_d3d8(DWORD process_id, const char *system_d3d8,
                                   char *loaded_path, size_t path_size,
                                   int *private_module)
{
    MODULEENTRY32 module;
    HANDLE snapshot;
    int found = 0;

    *private_module = 0;
    loaded_path[0] = '\0';
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&module, 0, sizeof(module));
    module.dwSize = sizeof(module);
    if (Module32First(snapshot, &module)) {
        do {
            if (!_stricmp(module.szModule, "d3d8.dll")) {
                lstrcpynA(loaded_path, module.szExePath, (int)path_size);
                found = !_stricmp(module.szExePath, system_d3d8);
                *private_module = !found;
                break;
            }
        } while (Module32Next(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

static ULONGLONG process_creation_stamp(HANDLE process_handle)
{
    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER stamp;

    if (!process_handle ||
        !GetProcessTimes(process_handle, &creation, &exit_time,
                         &kernel, &user)) return 0;
    stamp.LowPart = creation.dwLowDateTime;
    stamp.HighPart = creation.dwHighDateTime;
    return stamp.QuadPart;
}

static DWORD find_owned_process(
    DWORD launcher_process_id, ULONGLONG launcher_creation_stamp,
    DWORD active_process_id, DWORD retained_process_id)
{
    PROCESSENTRY32 process;
    HANDLE snapshot;
    HANDLE candidate;
    ULONGLONG candidate_creation_stamp;
    DWORD process_id = 0;
    int executable_matches;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&process, 0, sizeof(process));
    process.dwSize = sizeof(process);
    if (Process32First(snapshot, &process)) {
        do {
            executable_matches =
                !_stricmp(process.szExeFile, MAX_PAYNE_NAME);
            if (!executable_matches ||
                process.th32ParentProcessID != launcher_process_id) continue;
            candidate = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                    process.th32ProcessID);
            if (!candidate) continue;
            candidate_creation_stamp = process_creation_stamp(candidate);
            CloseHandle(candidate);
            if (QxpProcessIsOwnedSuccessor(
                    launcher_process_id, process.th32ProcessID,
                    process.th32ParentProcessID, active_process_id,
                    retained_process_id, executable_matches,
                    launcher_creation_stamp, candidate_creation_stamp)) {
                process_id = process.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &process));
    }
    CloseHandle(snapshot);
    return process_id;
}

static void close_game(HANDLE process_handle, HWND window)
{
    if (window) PostMessageA(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process_handle, 3000) != WAIT_OBJECT_0) {
        TerminateProcess(process_handle, 0);
        WaitForSingleObject(process_handle, 3000);
    }
}

static void close_game_pair(HANDLE active_process, HWND active_window,
                            HANDLE retained_process)
{
    if (active_process) {
        close_game(active_process, active_window);
        CloseHandle(active_process);
    }
    if (retained_process && retained_process != active_process) {
        close_game(retained_process, NULL);
        CloseHandle(retained_process);
    }
}

int main(int argc, char **argv)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE active_process;
    HANDLE retained_process = NULL;
    HANDLE next_process;
    DWORD active_process_id;
    DWORD launcher_process_id;
    DWORD retained_process_id = 0;
    ULONGLONG launcher_creation_stamp;
    DWORD started;
    DWORD loaded_at = 0;
    DWORD play_at = 0;
    DWORD tutorial_at = 0;
    DWORD scene_at = 0;
    DWORD last_activation = 0;
    DWORD wait_status;
    DWORD adopted_process_id;
    HWND window = NULL;
    HWND last_window = NULL;
    HWND candidate_window;
    char window_detail[320];
    char last_window_detail[320];
    char command[MAX_PATH * 2];
    char system_d3d8[MAX_PATH];
    char loaded_path[MAX_PATH];
    char candidate_loaded_path[MAX_PATH];
    char detail[320];
    const char *private_runtime;
    int private_module;
    int candidate_private_module;
    int activation_count = 0;
    int tutorial_mode = 0;
    int normal_launch = 0;
    int argument_index;

    for (argument_index = 1; argument_index < argc; ++argument_index) {
        if (!_stricmp(argv[argument_index], "--tutorial")) {
            tutorial_mode = 1;
        } else if (!_stricmp(argv[argument_index], "--normal-launch")) {
            normal_launch = 1;
        } else {
            _snprintf(detail, sizeof(detail), "error=unknown-argument arg=%s",
                      argv[argument_index]);
            detail[sizeof(detail) - 1] = '\0';
            return report_result(1, "arguments", detail);
        }
    }

    if (tutorial_mode && normal_launch) {
        return report_result(
            1, "arguments", "error=tutorial-normal-launch-conflict");
    }
    last_window_detail[0] = '\0';

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    report_stage("started", NULL);
    if (tutorial_mode) {
        report_stage("benchmark-mode", "scene=tutorial launch=skipstartup");
    } else {
        report_stage("benchmark-mode", normal_launch ?
                     "scene=menu launch=normal" :
                     "scene=menu launch=skipstartup");
    }
    inventory_executables();

    if (GetFileAttributesA(MAX_PAYNE_EXE) == INVALID_FILE_ATTRIBUTES) {
        return report_result(2, "preflight", "error=maxpayne-exe-missing");
    }
    private_runtime = find_private_runtime();
    if (private_runtime) {
        _snprintf(detail, sizeof(detail), "error=private-runtime file=%s",
                  private_runtime);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(3, "preflight", detail);
    }
    if (!GetSystemDirectoryA(system_d3d8, sizeof(system_d3d8))) {
        return report_result(4, "preflight", "error=no-system-directory");
    }
    strncat(system_d3d8, "\\d3d8.dll",
            sizeof(system_d3d8) - strlen(system_d3d8) - 1);
    report_stage("preflight-complete", "runtime=system32");

    if (normal_launch) {
        _snprintf(command, sizeof(command), "\"%s\"", MAX_PAYNE_EXE);
    } else {
        _snprintf(command, sizeof(command),
                  "\"%s\" -skipstartup", MAX_PAYNE_EXE);
    }
    command[sizeof(command) - 1] = '\0';
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(MAX_PAYNE_EXE, command, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP, NULL, MAX_PAYNE_DIR,
                        &startup, &process)) {
        _snprintf(detail, sizeof(detail), "error=create-process winerr=%lu",
                  (unsigned long)GetLastError());
        detail[sizeof(detail) - 1] = '\0';
        return report_result(5, "launch", detail);
    }
    CloseHandle(process.hThread);
    active_process = process.hProcess;
    active_process_id = process.dwProcessId;
    launcher_process_id = process.dwProcessId;
    launcher_creation_stamp = process_creation_stamp(active_process);
    _snprintf(detail, sizeof(detail), "pid=%lu",
              (unsigned long)active_process_id);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("launched", detail);

    WaitForInputIdle(active_process, 10000);
    started = GetTickCount();
    report_stage("launch-ready", normal_launch ?
                 "mode=startup-dialog-normal" :
                 "mode=startup-dialog-skipstartup");
    while (GetTickCount() - started < 150000) {
        wait_status = WaitForSingleObject(active_process, 0);
        if (wait_status == WAIT_OBJECT_0) {
            adopted_process_id = find_owned_process(
                launcher_process_id, launcher_creation_stamp,
                active_process_id, retained_process_id);
            if (!adopted_process_id) {
                DWORD exit_code = 0;
                GetExitCodeProcess(active_process, &exit_code);
                _snprintf(detail, sizeof(detail),
                          "error=early-exit exit=%lu",
                          (unsigned long)exit_code);
                detail[sizeof(detail) - 1] = '\0';
                close_game_pair(active_process, NULL, retained_process);
                return report_result(6, "running", detail);
            }
            next_process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE |
                                       PROCESS_QUERY_INFORMATION,
                                       FALSE, adopted_process_id);
            if (!next_process) {
                _snprintf(detail, sizeof(detail),
                          "error=adopt-process winerr=%lu",
                          (unsigned long)GetLastError());
                detail[sizeof(detail) - 1] = '\0';
                close_game_pair(active_process, NULL, retained_process);
                return report_result(7, "running", detail);
            }
            CloseHandle(active_process);
            active_process = next_process;
            active_process_id = adopted_process_id;
            last_window = NULL;
            last_window_detail[0] = '\0';
            _snprintf(detail, sizeof(detail), "pid=%lu source=exited-stub",
                      (unsigned long)active_process_id);
            detail[sizeof(detail) - 1] = '\0';
            report_stage("adopted", detail);
        }

        window = find_process_window(active_process_id);
        /* Some launcher stubs remain alive after spawning the same-named game
         * process. Adopt only a sibling that has reached a visible window or
         * loaded D3D8, and retain the stub handle for deterministic teardown. */
        if (!window && !retained_process && !loaded_at &&
            GetTickCount() - started >= 5000) {
            adopted_process_id = find_owned_process(
                launcher_process_id, launcher_creation_stamp,
                active_process_id, retained_process_id);
            if (adopted_process_id) {
                candidate_window = find_process_window(adopted_process_id);
                candidate_private_module = 0;
                if (candidate_window ||
                    process_has_system_d3d8(
                        adopted_process_id, system_d3d8,
                        candidate_loaded_path, sizeof(candidate_loaded_path),
                        &candidate_private_module)) {
                    next_process = OpenProcess(
                        SYNCHRONIZE | PROCESS_TERMINATE |
                        PROCESS_QUERY_INFORMATION,
                        FALSE, adopted_process_id);
                    if (next_process) {
                        retained_process = active_process;
                        retained_process_id = active_process_id;
                        active_process = next_process;
                        active_process_id = adopted_process_id;
                        window = candidate_window;
                        last_window = NULL;
                        last_window_detail[0] = '\0';
                        _snprintf(detail, sizeof(detail),
                                  "pid=%lu source=live-stub",
                                  (unsigned long)active_process_id);
                        detail[sizeof(detail) - 1] = '\0';
                        report_stage("adopted", detail);
                    }
                }
            }
        }
        if (window) {
            describe_window(window, window_detail, sizeof(window_detail));
        }
        if (window && (window != last_window ||
                       strcmp(window_detail, last_window_detail))) {
            report_window(window, !play_at);
            last_window = window;
            lstrcpynA(last_window_detail, window_detail,
                      sizeof(last_window_detail));
        }
        /* Once the Play button succeeds, leave the game alone. Repeated
         * foreground changes and Enter presses can pause the renderer or
         * drive a different window, turning the test harness itself into a
         * source of false presentation failures. */
        if (window && !play_at && activation_count < 9 &&
            GetTickCount() - started >= (DWORD)(8000 + activation_count * 4000) &&
            GetTickCount() - last_activation >= 3000) {
            if (activate_launcher(window,
                                  FALSE,
                                  detail, sizeof(detail)) &&
                !play_at && strstr(detail, "Play")) {
                play_at = GetTickCount();
                report_stage("play-started", NULL);
            }
            report_stage("activate", detail);
            last_activation = GetTickCount();
            ++activation_count;
        }

        if (process_has_system_d3d8(active_process_id, system_d3d8,
                                    loaded_path, sizeof(loaded_path),
                                    &private_module)) {
            if (!loaded_at) {
                _snprintf(detail, sizeof(detail), "path=%s", loaded_path);
                detail[sizeof(detail) - 1] = '\0';
                report_stage("system-d3d8-loaded", detail);
                loaded_at = GetTickCount();
            }
            if (tutorial_mode && play_at && !tutorial_at && window &&
                GetTickCount() - play_at >= 5000) {
                SetForegroundWindow(window);
                PostMessageA(window, WM_KEYDOWN, VK_RETURN, 0);
                keybd_event(VK_RETURN, 0, 0, 0);
                Sleep(100);
                keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
                PostMessageA(window, WM_KEYUP, VK_RETURN, 0);
                tutorial_at = GetTickCount();
                report_stage("tutorial-input", "action=enter");
            }
            if (tutorial_mode && tutorial_at && !scene_at &&
                GetTickCount() - tutorial_at >= 15000) {
                scene_at = GetTickCount();
                report_stage("scene-started",
                             "scene=tutorial warmup-ms=15000");
            }
            if (tutorial_mode) {
                if (scene_at && GetTickCount() - scene_at >= 45000) break;
            } else if (play_at && GetTickCount() - play_at >= 45000) {
                break;
            }
        } else if (private_module) {
            _snprintf(detail, sizeof(detail), "error=wrong-d3d8 path=%s",
                      loaded_path);
            detail[sizeof(detail) - 1] = '\0';
            close_game_pair(active_process, window, retained_process);
            return report_result(8, "runtime", detail);
        }
        Sleep(250);
    }

    if (!loaded_at) {
        _snprintf(detail, sizeof(detail),
                  "error=no-system-d3d8 pid=%lu activations=%d",
                  (unsigned long)active_process_id, activation_count);
        detail[sizeof(detail) - 1] = '\0';
        close_game_pair(active_process, window, retained_process);
        return report_result(9, "runtime", detail);
    }
    if (!play_at) {
        _snprintf(detail, sizeof(detail),
                  "error=play-not-started pid=%lu activations=%d",
                  (unsigned long)active_process_id, activation_count);
        detail[sizeof(detail) - 1] = '\0';
        close_game_pair(active_process, window, retained_process);
        return report_result(10, "launcher", detail);
    }
    if (tutorial_mode && !scene_at) {
        _snprintf(detail, sizeof(detail),
                  "error=tutorial-not-started pid=%lu input=%d",
                  (unsigned long)active_process_id, tutorial_at != 0);
        detail[sizeof(detail) - 1] = '\0';
        close_game_pair(active_process, window, retained_process);
        return report_result(11, "scene", detail);
    }

    report_stage("observation-complete", NULL);
    close_game_pair(active_process, window, retained_process);
    _snprintf(detail, sizeof(detail),
              "runtime=%s scene=%s launch=%s observed-ms=45000",
              loaded_path, tutorial_mode ? "tutorial" : "menu",
              normal_launch ? "normal" : "skipstartup");
    detail[sizeof(detail) - 1] = '\0';
    return report_result(0, "complete", detail);
}
