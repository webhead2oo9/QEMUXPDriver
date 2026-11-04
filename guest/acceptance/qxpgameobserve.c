#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../qxpresult.h"
#include "qxp_process_lineage.h"
#include "qxp_ut2003_log.h"

typedef struct game_config {
    const char *mode;
    const char *const *names;
    unsigned name_count;
    const char *const *candidates;
    unsigned candidate_count;
    const char *log_name;
    const char *ini_name;
} GAME_CONFIG;

typedef struct window_search {
    DWORD process_id;
    HWND window;
    LONG area;
} WINDOW_SEARCH;

static const char *const ut_names[] = { "UT2003.exe" };
static const char *const ut_candidates[] = {
    "C:\\UT2003Demo\\System\\UT2003.exe",
    "C:\\Program Files\\UT2003Demo\\System\\UT2003.exe",
    "C:\\Program Files\\UT2003 Demo\\System\\UT2003.exe",
    "C:\\Program Files\\Unreal Tournament 2003 Demo\\System\\UT2003.exe"
};
static const char *const war3_names[] = {
    "War3.exe", "Warcraft III.exe", "Warcraft III Demo.exe"
};
static const char *const war3_candidates[] = {
    "C:\\Program Files\\Warcraft III Demo\\War3.exe",
    "C:\\Program Files\\Warcraft III Demo\\Warcraft III.exe",
    "C:\\Program Files\\Warcraft III\\War3.exe",
    "C:\\Program Files\\Warcraft III\\Warcraft III.exe",
    "C:\\Warcraft III Demo\\War3.exe",
    "C:\\Warcraft III Demo\\Warcraft III.exe"
};
static const char *const nfsu2_names[] = { "speed2.exe" };
static const char *const nfsu2_candidates[] = {
    "C:\\Program Files\\EA GAMES\\Need for Speed Underground 2\\speed2.exe",
    "C:\\Program Files\\Need for Speed Underground 2\\speed2.exe",
    "C:\\EA GAMES\\Need for Speed Underground 2\\speed2.exe"
};

static void send_message_all_ports(const char *message)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    unsigned port;
    int length;

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

static void sanitize(char *text)
{
    size_t index;
    size_t length;

    for (index = 0; text[index]; ++index) {
        if (text[index] == '\r' || text[index] == '\n' ||
            text[index] == '\t') text[index] = ' ';
    }
    length = strlen(text);
    while (length && text[length - 1] == ' ') text[--length] = '\0';
}

static void report_stage(const char *mode, const char *stage,
                         const char *detail)
{
    char message[448];

    _snprintf(message, sizeof(message),
              "QXP_GAME_OBSERVE_STAGE rc=0 game=%s stage=%s%s%s",
              mode, stage, detail && detail[0] ? " " : "",
              detail && detail[0] ? detail : "");
    message[sizeof(message) - 1] = '\0';
    send_message_all_ports(message);
}

static int prepare_fresh_log(const GAME_CONFIG *config,
                             const char *directory)
{
    DWORD attributes;
    DWORD error;
    char path[MAX_PATH];
    char detail[384];
    int existed;

    if (!config->log_name) return 0;
    _snprintf(path, sizeof(path), "%s\\%s", directory, config->log_name);
    path[sizeof(path) - 1] = '\0';
    attributes = GetFileAttributesA(path);
    existed = attributes != INVALID_FILE_ATTRIBUTES;
    if (!existed) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            _snprintf(detail, sizeof(detail),
                      "error=attributes winerr=%lu",
                      (unsigned long)error);
            detail[sizeof(detail) - 1] = '\0';
            report_stage(config->mode, "log-reset-failed", detail);
            return 0;
        }
    } else if (!DeleteFileA(path)) {
        _snprintf(detail, sizeof(detail), "error=delete winerr=%lu",
                  (unsigned long)GetLastError());
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config->mode, "log-reset-failed", detail);
        return 0;
    }
    _snprintf(detail, sizeof(detail), "file=%s existed=%d",
              config->log_name, existed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config->mode, "log-reset", detail);
    return 1;
}

static void read_ut_log_state(const GAME_CONFIG *config,
                              const char *directory,
                              QXP_UT2003_LOG_STATE *state)
{
    FILE *file;
    char path[MAX_PATH];
    char line[384];

    qxp_ut2003_log_init(state);
    _snprintf(path, sizeof(path), "%s\\%s", directory, config->log_name);
    path[sizeof(path) - 1] = '\0';
    file = fopen(path, "rb");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        sanitize(line);
        qxp_ut2003_log_consume(state, line);
    }
    fclose(file);
}


static int read_ut_gameplay_semantic(const GAME_CONFIG *config,
                                     const char *directory,
                                     unsigned *phase_out)
{
    QXP_UT2003_LOG_STATE state;

    read_ut_log_state(config, directory, &state);
    if (phase_out) *phase_out = state.gameplay_phase;
    return qxp_ut2003_log_gameplay_ready(&state);
}

static int report_result(const char *mode, int rc, const char *stage,
                         const char *detail)
{
    char message[448];
    unsigned attempt;

    _snprintf(message, sizeof(message),
              "QXP_GAME_OBSERVE_RESULT rc=%d game=%s stage=%s%s%s",
              rc, mode, stage, detail && detail[0] ? " " : "",
              detail && detail[0] ? detail : "");
    message[sizeof(message) - 1] = '\0';
    /* The harness receives diagnostics over serial or UDP while QEMU is also
     * dumping frames and debug output. Space a few copies of the terminal
     * result so a transiently busy channel cannot turn a completed
     * observation into a timeout. */
    for (attempt = 0; attempt < 5; ++attempt) {
        send_message_all_ports(message);
        if (attempt != 4) Sleep(75);
    }
    return rc;
}

static int name_matches(const char *name, const GAME_CONFIG *config)
{
    unsigned index;

    for (index = 0; index < config->name_count; ++index) {
        if (!_stricmp(name, config->names[index])) return 1;
    }
    return 0;
}

static int find_recursive(const char *directory, const char *name,
                          unsigned depth, char *result, size_t result_size)
{
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[MAX_PATH];
    char path[MAX_PATH];

    _snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    pattern[sizeof(pattern) - 1] = '\0';
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, ".."))
            continue;
        _snprintf(path, sizeof(path), "%s\\%s", directory, entry.cFileName);
        path[sizeof(path) - 1] = '\0';
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth && _stricmp(entry.cFileName, "Windows") &&
                _stricmp(entry.cFileName, "Documents and Settings") &&
                find_recursive(path, name, depth - 1, result,
                               result_size)) {
                FindClose(search);
                return 1;
            }
        } else if (!_stricmp(entry.cFileName, name)) {
            lstrcpynA(result, path, (int)result_size);
            FindClose(search);
            return 1;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    return 0;
}

static int find_executable(const GAME_CONFIG *config, char *path,
                           size_t path_size)
{
    unsigned index;

    for (index = 0; index < config->candidate_count; ++index) {
        if (GetFileAttributesA(config->candidates[index]) !=
                INVALID_FILE_ATTRIBUTES) {
            lstrcpynA(path, config->candidates[index], (int)path_size);
            return 1;
        }
    }
    /* Directory enumeration order must not override the configured name
     * priority. Games commonly install a friendly launcher beside or above
     * the real renderer executable. Search all roots for each preferred name
     * before falling back to the next name. */
    for (index = 0; index < config->name_count; ++index) {
        if (find_recursive("C:\\Program Files", config->names[index], 4,
                           path, path_size) ||
            find_recursive("C:\\", config->names[index], 2,
                           path, path_size)) return 1;
    }
    return 0;
}

static void directory_from_path(const char *path, char *directory,
                                size_t directory_size)
{
    char *slash;

    lstrcpynA(directory, path, (int)directory_size);
    slash = strrchr(directory, '\\');
    if (slash) *slash = '\0';
}

static BOOL CALLBACK find_window_callback(HWND window, LPARAM opaque)
{
    WINDOW_SEARCH *search = (WINDOW_SEARCH *)opaque;
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

static HWND find_process_window(DWORD process_id)
{
    WINDOW_SEARCH search;

    search.process_id = process_id;
    search.window = NULL;
    search.area = 0;
    EnumWindows(find_window_callback, (LPARAM)&search);
    return search.window;
}

static BOOL CALLBACK report_control_callback(HWND window, LPARAM opaque)
{
    const char *mode = (const char *)opaque;
    char class_name[48];
    char caption[272];
    char detail[352];
    DWORD_PTR copied = 0;

    if (!IsWindowVisible(window) ||
        !GetClassNameA(window, class_name, sizeof(class_name))) return TRUE;
    caption[0] = '\0';
    GetWindowTextA(window, caption, sizeof(caption));
    if (!caption[0]) {
        SendMessageTimeoutA(window, WM_GETTEXT, sizeof(caption),
                            (LPARAM)caption, SMTO_ABORTIFHUNG, 500, &copied);
    }
    class_name[sizeof(class_name) - 1] = '\0';
    caption[sizeof(caption) - 1] = '\0';
    sanitize(caption);
    if (!caption[0]) return TRUE;
    _snprintf(detail, sizeof(detail), "class=%s text=%s", class_name,
              caption);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(mode, "control", detail);
    return TRUE;
}

static void report_window(const GAME_CONFIG *config, HWND window)
{
    char class_name[64];
    char title[224];
    char detail[384];
    RECT rect;
    RECT client;

    class_name[0] = '\0';
    title[0] = '\0';
    memset(&rect, 0, sizeof(rect));
    memset(&client, 0, sizeof(client));
    GetClassNameA(window, class_name, sizeof(class_name));
    GetWindowTextA(window, title, sizeof(title));
    GetWindowRect(window, &rect);
    GetClientRect(window, &client);
    sanitize(title);
    _snprintf(detail, sizeof(detail),
              "hwnd=%08lx class=%s title=%s rect=%ld,%ld,%ld,%ld "
              "client=%ldx%ld foreground=%d",
              (unsigned long)(ULONG_PTR)window,
              class_name[0] ? class_name : "(none)",
              title[0] ? title : "(none)",
              (long)rect.left, (long)rect.top, (long)rect.right,
              (long)rect.bottom, (long)(client.right - client.left),
              (long)(client.bottom - client.top),
              window == GetForegroundWindow());
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config->mode, "window", detail);
    EnumChildWindows(window, report_control_callback, (LPARAM)config->mode);
}

static int system_module_path(const char *path, const char *name)
{
    char system_directory[MAX_PATH];
    char expected[MAX_PATH];
    UINT length;

    if (!path || !name) return 0;
    length = GetSystemDirectoryA(system_directory, sizeof(system_directory));
    if (!length || length >= sizeof(system_directory) ||
        length + 1 + strlen(name) + 1 > sizeof(expected)) return 0;
    _snprintf(expected, sizeof(expected), "%s\\%s", system_directory, name);
    expected[sizeof(expected) - 1] = '\0';
    return !_stricmp(path, expected);
}

static unsigned report_modules(const GAME_CONFIG *config, DWORD process_id,
                               unsigned reported, unsigned *system_modules)
{
    static const char *const modules[] = {
        "d3d8.dll", "d3d9.dll", "ddraw.dll", "opengl32.dll"
    };
    MODULEENTRY32 module;
    HANDLE snapshot;
    unsigned index;
    int is_system;
    char detail[352];

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return reported;
    memset(&module, 0, sizeof(module));
    module.dwSize = sizeof(module);
    if (Module32First(snapshot, &module)) {
        do {
            for (index = 0; index < sizeof(modules) / sizeof(modules[0]);
                 ++index) {
                if ((reported & (1U << index)) ||
                    _stricmp(module.szModule, modules[index])) continue;
                is_system = system_module_path(
                    module.szExePath, modules[index]);
                if (system_modules && is_system)
                    *system_modules |= 1U << index;
                _snprintf(detail, sizeof(detail),
                          "name=%s path=%s system32=%d",
                          modules[index], module.szExePath, is_system);
                detail[sizeof(detail) - 1] = '\0';
                report_stage(config->mode, "module", detail);
                reported |= 1U << index;
            }
        } while (Module32Next(snapshot, &module));
    }
    CloseHandle(snapshot);
    return reported;
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

#define SUCCESSOR_TABLE_MAX 256

/* Successor adoption is fail-closed: a candidate must be a launch-time
 * descendant of the observed process, directly or through ancestry whose
 * every live hop was created no earlier than the launcher.  The previous
 * global same-name scan could adopt a healthy pre-existing game process or
 * a reused pid and credit that foreign process with this run's evidence. */
static DWORD find_successor_process(const GAME_CONFIG *config,
                                    const DWORD *owned_process_ids,
                                    unsigned owned_process_count,
                                    DWORD observed_process_id,
                                    ULONGLONG launcher_creation_stamp,
                                    char *name, size_t name_size,
                                    char *relationship,
                                    size_t relationship_size)
{
    PROCESSENTRY32 process;
    HANDLE snapshot;
    HANDLE candidate_handle;
    DWORD process_id = 0;
    unsigned best_score = 0;
    DWORD table_process_ids[SUCCESSOR_TABLE_MAX];
    DWORD table_parent_process_ids[SUCCESSOR_TABLE_MAX];
    ULONGLONG table_creation_stamps[SUCCESSOR_TABLE_MAX];
    char table_names[SUCCESSOR_TABLE_MAX][MAX_PATH];
    unsigned table_count = 0;
    unsigned index;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&process, 0, sizeof(process));
    process.dwSize = sizeof(process);
    if (Process32First(snapshot, &process)) {
        do {
            if (table_count >= SUCCESSOR_TABLE_MAX) break;
            table_process_ids[table_count] = process.th32ProcessID;
            table_parent_process_ids[table_count] =
                process.th32ParentProcessID;
            lstrcpynA(table_names[table_count], process.szExeFile,
                      MAX_PATH);
            ++table_count;
        } while (Process32Next(snapshot, &process));
    }
    CloseHandle(snapshot);
    for (index = 0; index < table_count; ++index) {
        table_creation_stamps[index] = 0;
        if (!table_process_ids[index]) continue;
        candidate_handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                       table_process_ids[index]);
        if (!candidate_handle) continue;
        table_creation_stamps[index] =
            process_creation_stamp(candidate_handle);
        CloseHandle(candidate_handle);
    }
    for (index = 0; index < table_count; ++index) {
        unsigned score = 0;
        const char *candidate_relationship = NULL;

        if (!QxpProcessIsOwnedDescendant(
                table_process_ids[index], table_creation_stamps[index],
                table_process_ids, table_parent_process_ids,
                table_creation_stamps, table_count,
                owned_process_ids, owned_process_count,
                launcher_creation_stamp)) continue;
        if (name_matches(table_names[index], config)) {
            score = 3;
            candidate_relationship = "recognized-name";
        } else if (table_parent_process_ids[index] ==
                   observed_process_id) {
            score = find_process_window(table_process_ids[index]) ? 2 : 1;
            candidate_relationship = score == 2 ? "visible-child" :
                                                  "direct-child";
        }
        if (score > best_score) {
            best_score = score;
            process_id = table_process_ids[index];
            lstrcpynA(name, table_names[index], (int)name_size);
            lstrcpynA(relationship, candidate_relationship,
                      (int)relationship_size);
        }
    }
    return process_id;
}

static int contains_diagnostic_word(const char *line)
{
    char lower[384];
    size_t index;

    for (index = 0; line[index] && index + 1 < sizeof(lower); ++index) {
        char ch = line[index];
        lower[index] = ch >= 'A' && ch <= 'Z' ?
                       (char)(ch - 'A' + 'a') : ch;
    }
    lower[index] = '\0';
    return strstr(lower, "error") || strstr(lower, "fail") ||
           strstr(lower, "d3d") || strstr(lower, "render") ||
           strstr(lower, "device") || strstr(lower, "caps") ||
           strstr(lower, "warning");
}

static int contains_case_insensitive(const char *text, const char *needle)
{
    size_t text_length;
    size_t needle_length;
    size_t index;
    size_t offset;

    if (!text || !needle) return 0;
    text_length = strlen(text);
    needle_length = strlen(needle);
    if (!needle_length || needle_length > text_length) return 0;
    for (offset = 0; offset + needle_length <= text_length; ++offset) {
        for (index = 0; index < needle_length; ++index) {
            char left = text[offset + index];
            char right = needle[index];

            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z')
                right = (char)(right - 'A' + 'a');
            if (left != right) break;
        }
        if (index == needle_length) return 1;
    }
    return 0;
}

static void report_application_events(const GAME_CONFIG *config,
                                      time_t earliest)
{
    BYTE buffer[32768];
    HANDLE log;
    DWORD bytes_read;
    DWORD bytes_needed;
    DWORD offset;
    unsigned reported = 0;
    const char *match = !_strnicmp(config->mode, "warcraft3", 9) ?
                        "warcraft" :
                        (!_strnicmp(config->mode, "nfsu2", 5) ?
                         "speed2" : "ut2003");

    log = OpenEventLogA(NULL, "Application");
    if (!log) return;
    while (reported < 16 &&
           ReadEventLogA(log, EVENTLOG_BACKWARDS_READ |
                              EVENTLOG_SEQUENTIAL_READ,
                         0, buffer, sizeof(buffer), &bytes_read,
                         &bytes_needed)) {
        offset = 0;
        while (offset + sizeof(EVENTLOGRECORD) <= bytes_read &&
               reported < 16) {
            EVENTLOGRECORD *record = (EVENTLOGRECORD *)(buffer + offset);
            const char *source;
            const char *string;
            const char *limit;
            char combined[300];
            char detail[420];
            unsigned string_index;
            int relevant = 0;

            if (record->Length < sizeof(*record) ||
                offset + record->Length > bytes_read) break;
            if ((time_t)record->TimeGenerated < earliest) {
                CloseEventLog(log);
                return;
            }
            source = (const char *)record + sizeof(*record);
            combined[0] = '\0';
            limit = (const char *)record + record->Length;
            if (record->StringOffset < record->Length) {
                string = (const char *)record + record->StringOffset;
                for (string_index = 0;
                     string_index < record->NumStrings && string < limit;
                     ++string_index) {
                    size_t length = strnlen(string, (size_t)(limit - string));

                    if (string + length >= limit) break;
                    if (contains_case_insensitive(string, match)) relevant = 1;
                    if (combined[0] && strlen(combined) + 2 < sizeof(combined))
                        strcat(combined, " | ");
                    if (strlen(combined) + length + 1 < sizeof(combined))
                        strcat(combined, string);
                    string += length + 1;
                }
            }
            if (relevant) {
                sanitize(combined);
                _snprintf(detail, sizeof(detail),
                          "source=%s id=%lu type=%u text=%s",
                          source, (unsigned long)(record->EventID & 0xffffUL),
                          (unsigned)record->EventType,
                          combined[0] ? combined : "(none)");
                detail[sizeof(detail) - 1] = '\0';
                report_stage(config->mode, "event", detail);
                ++reported;
            }
            offset += record->Length;
        }
    }
    CloseEventLog(log);
}

static void report_ini(const GAME_CONFIG *config, const char *directory)
{
    FILE *file;
    char path[MAX_PATH];
    char line[384];
    char lower[384];
    char detail[420];
    char *trimmed;
    size_t index;
    unsigned count = 0;
    int in_renderer_section = 0;

    if (!config->ini_name) return;
    _snprintf(path, sizeof(path), "%s\\%s", directory, config->ini_name);
    path[sizeof(path) - 1] = '\0';
    file = fopen(path, "rb");
    if (!file) {
        report_stage(config->mode, "ini-missing", config->ini_name);
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        sanitize(line);
        for (index = 0; line[index] && index + 1 < sizeof(lower); ++index) {
            char ch = line[index];
            lower[index] = ch >= 'A' && ch <= 'Z' ?
                           (char)(ch - 'A' + 'a') : ch;
        }
        lower[index] = '\0';
        trimmed = lower;
        while (*trimmed == ' ') ++trimmed;
        if (*trimmed == '[') {
            in_renderer_section =
                !_stricmp(trimmed, "[d3ddrv.d3drenderdevice]");
        }
        if (!in_renderer_section && !strstr(lower, "viewport") &&
            !strstr(lower, "colorbits") &&
            !strstr(lower, "renderdevice") &&
            !strstr(lower, "startupfullscreen") &&
            !strstr(lower, "refreshrate")) continue;
        if (!line[0]) continue;
        _snprintf(detail, sizeof(detail), "line=%s", line);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config->mode, "ini", detail);
        if (++count >= 64) break;
    }
    fclose(file);
}

static int override_ut_software_vp(const GAME_CONFIG *config,
                                   const char *directory)
{
    static const char section[] = "D3DDrv.D3DRenderDevice";
    char path[MAX_PATH];
    char detail[352];
    char tl_value[16];
    char vs_value[16];
    BOOL hardware_tl;
    BOOL hardware_vs;
    BOOL flushed;
    int verified;

    _snprintf(path, sizeof(path), "%s\\%s", directory, config->ini_name);
    path[sizeof(path) - 1] = '\0';
    hardware_tl = WritePrivateProfileStringA(
        section, "UseHardwareTL", "False", path);
    hardware_vs = WritePrivateProfileStringA(
        section, "UseHardwareVS", "False", path);
    flushed = WritePrivateProfileStringA(NULL, NULL, NULL, path);
    GetPrivateProfileStringA(section, "UseHardwareTL", "", tl_value,
                             sizeof(tl_value), path);
    GetPrivateProfileStringA(section, "UseHardwareVS", "", vs_value,
                             sizeof(vs_value), path);
    verified = !_stricmp(tl_value, "False") &&
               !_stricmp(vs_value, "False");
    _snprintf(detail, sizeof(detail),
              "UseHardwareTL=%s UseHardwareVS=%s wrote=%d flushed=%d verified=%d",
              tl_value, vs_value, hardware_tl && hardware_vs, flushed,
              verified);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config->mode, "override", detail);
    return hardware_tl && hardware_vs && verified;
}

static void report_log(const GAME_CONFIG *config, const char *directory)
{
    FILE *file;
    char path[MAX_PATH];
    char line[384];
    char detail[420];
    char diagnostics[32][384];
    char tail[32][384];
    long length;
    long offset;
    unsigned diagnostic_total = 0;
    unsigned tail_total = 0;
    unsigned first;
    unsigned index;

    if (!config->log_name) return;
    _snprintf(path, sizeof(path), "%s\\%s", directory, config->log_name);
    path[sizeof(path) - 1] = '\0';
    file = fopen(path, "rb");
    if (!file) {
        report_stage(config->mode, "log-missing", config->log_name);
        return;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    offset = length > 65536 ? length - 65536 : 0;
    fseek(file, offset, SEEK_SET);
    if (offset) fgets(line, sizeof(line), file);
    /* The game is still writing this file. Stop at the length captured above
     * so a fast-growing log cannot keep the observer chasing a moving EOF. */
    while (ftell(file) < length && fgets(line, sizeof(line), file)) {
        sanitize(line);
        if (line[0]) {
            lstrcpynA(tail[tail_total % 32], line,
                      sizeof(tail[tail_total % 32]));
            ++tail_total;
        }
        if (!contains_diagnostic_word(line)) continue;
        lstrcpynA(diagnostics[diagnostic_total % 32], line,
                  sizeof(diagnostics[diagnostic_total % 32]));
        ++diagnostic_total;
    }
    fclose(file);
    first = diagnostic_total > 32 ? diagnostic_total - 32 : 0;
    for (index = first; index < diagnostic_total; ++index) {
        _snprintf(detail, sizeof(detail), "line=%s",
                  diagnostics[index % 32]);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config->mode, "log", detail);
    }
    first = tail_total > 32 ? tail_total - 32 : 0;
    for (index = first; index < tail_total; ++index) {
        _snprintf(detail, sizeof(detail), "line=%s", tail[index % 32]);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config->mode, "log-tail", detail);
    }
}

static void close_game(HANDLE process, HWND window)
{
    if (window) PostMessageA(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 3000);
    }
}

int main(int argc, char **argv)
{
    GAME_CONFIG config;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE active_process;
    DWORD started;
    DWORD wait_status;
    DWORD exit_code = STILL_ACTIVE;
    DWORD active_process_id;
    DWORD child_process_id;
    DWORD owned_process_ids[16];
    unsigned owned_process_count = 0;
    ULONGLONG launcher_creation_stamp;
    unsigned adopt_attempt;
    HWND window = NULL;
    HWND last_window = NULL;
    char executable[MAX_PATH];
    char directory[MAX_PATH];
    char command[MAX_PATH * 2];
    char detail[384];
    char child_name[MAX_PATH];
    char child_relationship[32];
    const char *performance_renderer = NULL;
    const char *visual_renderer = "d3d8";
    unsigned performance_renderer_module = 0;
    unsigned visual_renderer_module = 1U << 0;
    unsigned modules = 0;
    unsigned system_modules = 0;
    unsigned fire_attempts = 0;
    unsigned gameplay_phase = QXP_UT2003_GAMEPLAY_NONE;
    DWORD last_fire = 0;
    DWORD elapsed;
    DWORD measurement_started = 0;
    DWORD measured_ms = 0;
    DWORD visual_observed_ms = 0;
    DWORD visual_ready_started = 0;
    int exited = 0;
    int visual_ready = 0;
    int gameplay_reached = 0;
    int gameplay_input_ready = 0;
    int ut_gameplay = 0;
    int performance_observation = 0;
    int visual_observation = 0;
    int warcraft_d3d8_oracle = 0;
    int warcraft_d3d8 = 0;
    int performance_requires_system = 0;
    time_t event_start;

    if (argc != 2) return report_result("unknown", 2, "arguments",
        "expected=ut2003-ut2003visual-ut2003performance-"
        "ut2003gameplay-ut2003gameplayperformance-"
        "ut2003gameplayswvp-warcraft3-warcraft3performance-"
        "warcraft3d3d8-warcraft3d3d8performance-"
        "nfsu2-nfsu2visual-or-nfsu2performance");
    memset(&config, 0, sizeof(config));
    if (!_stricmp(argv[1], "ut2003") ||
        !_stricmp(argv[1], "ut2003visual") ||
        !_stricmp(argv[1], "ut2003performance") ||
        !_stricmp(argv[1], "ut2003gameplay") ||
        !_stricmp(argv[1], "ut2003gameplayperformance") ||
        !_stricmp(argv[1], "ut2003gameplayswvp")) {
        config.mode = !_stricmp(argv[1], "ut2003gameplayswvp") ?
                      "ut2003gameplayswvp" :
                      (!_stricmp(argv[1], "ut2003gameplay") ||
                       !_stricmp(argv[1], "ut2003gameplayperformance") ?
                       "ut2003gameplay" : "ut2003");
        ut_gameplay = _stricmp(config.mode, "ut2003") != 0;
        performance_observation =
            !_stricmp(argv[1], "ut2003performance") ||
            !_stricmp(argv[1], "ut2003gameplayperformance");
        visual_observation = !_stricmp(argv[1], "ut2003visual");
        config.names = ut_names;
        config.name_count = sizeof(ut_names) / sizeof(ut_names[0]);
        config.candidates = ut_candidates;
        config.candidate_count = sizeof(ut_candidates) /
                                 sizeof(ut_candidates[0]);
        config.log_name = "UT2003.log";
        config.ini_name = "UT2003.ini";
    } else if (!_stricmp(argv[1], "warcraft3") ||
               !_stricmp(argv[1], "warcraft3performance") ||
               !_stricmp(argv[1], "warcraft3d3d8") ||
               !_stricmp(argv[1], "warcraft3d3d8performance")) {
        warcraft_d3d8 =
            !_stricmp(argv[1], "warcraft3d3d8") ||
            !_stricmp(argv[1], "warcraft3d3d8performance");
        performance_observation =
            !_stricmp(argv[1], "warcraft3performance") ||
            !_stricmp(argv[1], "warcraft3d3d8performance");
        warcraft_d3d8_oracle =
            warcraft_d3d8 && !performance_observation;
        config.mode = warcraft_d3d8 ? "warcraft3d3d8" : "warcraft3";
        config.names = war3_names;
        config.name_count = sizeof(war3_names) / sizeof(war3_names[0]);
        config.candidates = war3_candidates;
        config.candidate_count = sizeof(war3_candidates) /
                                 sizeof(war3_candidates[0]);
    } else if (!_stricmp(argv[1], "nfsu2") ||
               !_stricmp(argv[1], "nfsu2visual") ||
               !_stricmp(argv[1], "nfsu2performance")) {
        config.mode = "nfsu2";
        performance_observation =
            !_stricmp(argv[1], "nfsu2performance");
        visual_observation = !_stricmp(argv[1], "nfsu2visual");
        config.names = nfsu2_names;
        config.name_count = sizeof(nfsu2_names) / sizeof(nfsu2_names[0]);
        config.candidates = nfsu2_candidates;
        config.candidate_count = sizeof(nfsu2_candidates) /
                                 sizeof(nfsu2_candidates[0]);
    } else {
        return report_result(argv[1], 2, "arguments", "error=unknown-game");
    }
    if (!_stricmp(config.mode, "nfsu2")) {
        visual_renderer = "d3d9";
        visual_renderer_module = 1U << 1;
    }
    if (performance_observation || warcraft_d3d8_oracle) {
        if (!_stricmp(config.mode, "warcraft3")) {
            performance_renderer = "opengl";
            performance_renderer_module = 1U << 3;
        } else if (!_stricmp(config.mode, "nfsu2")) {
            performance_renderer = "d3d9";
            performance_renderer_module = 1U << 1;
        } else {
            performance_renderer = "d3d8";
            performance_renderer_module = 1U << 0;
        }
        performance_requires_system =
            warcraft_d3d8 || !_stricmp(config.mode, "nfsu2");
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    report_stage(config.mode, "started", NULL);
    event_start = time(NULL) - 5;
    if (!find_executable(&config, executable, sizeof(executable)))
        return report_result(config.mode, 3, "preflight",
                             "error=executable-not-found");
    directory_from_path(executable, directory, sizeof(directory));
    _snprintf(detail, sizeof(detail), "exe=%s cwd=%s", executable,
              directory);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config.mode, "located", detail);
    report_ini(&config, directory);
    if (!_stricmp(config.mode, "ut2003gameplayswvp") &&
        !override_ut_software_vp(&config, directory)) {
        return report_result(config.mode, 5, "override",
                             "error=software-vp-ini-write");
    }
    if ((visual_observation || ut_gameplay) && config.log_name &&
        !prepare_fresh_log(&config, directory)) {
        return report_result(config.mode, 11, "preflight",
                             "error=fresh-log-reset");
    }
    if (ut_gameplay) {
        _snprintf(command, sizeof(command),
                  "\"%s\" DM-Antalus?game=XGame.xDeathMatch -nosound",
                  executable);
        report_stage(config.mode, "launch-mode",
                     performance_observation ?
                     "url=DM-Antalus?game=XGame.xDeathMatch "
                     "audio=disabled benchmark=capture-free" :
                     "url=DM-Antalus?game=XGame.xDeathMatch audio=disabled");
    } else if (performance_observation &&
               !_stricmp(config.mode, "ut2003")) {
        _snprintf(command, sizeof(command), "\"%s\" -nosound", executable);
        report_stage(config.mode, "launch-mode",
                     "renderer=d3d8 audio=disabled benchmark=capture-free");
    } else if (visual_observation &&
               !_stricmp(config.mode, "ut2003")) {
        _snprintf(command, sizeof(command), "\"%s\"", executable);
        report_stage(config.mode, "launch-mode",
                     "renderer=d3d8 audio=default visual=extended "
                     "launch=normal");
    } else if (warcraft_d3d8) {
        _snprintf(command, sizeof(command), "\"%s\"", executable);
        report_stage(config.mode, "launch-mode",
                     performance_observation ?
                     "renderer=d3d8 selection=default "
                     "benchmark=capture-free" :
                     "renderer=d3d8 selection=default "
                     "oracle=black-cadence");
    } else if (!_stricmp(config.mode, "warcraft3")) {
        _snprintf(command, sizeof(command), "\"%s\" -opengl", executable);
        report_stage(config.mode, "launch-mode",
                     performance_observation ?
                     "renderer=opengl benchmark=capture-free" :
                     "renderer=opengl");
    } else if (!_stricmp(config.mode, "nfsu2")) {
        _snprintf(command, sizeof(command), "\"%s\"", executable);
        _snprintf(detail, sizeof(detail),
                  performance_observation ?
                  "renderer=%s benchmark=capture-free" :
                  (visual_observation ?
                   "renderer=%s visual=extended launch=normal" :
                   "renderer=%s"),
                  visual_renderer);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config.mode, "launch-mode", detail);
    } else {
        _snprintf(command, sizeof(command), "\"%s\"", executable);
    }
    command[sizeof(command) - 1] = '\0';
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(executable, command, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP, NULL, directory,
                        &startup, &process)) {
        _snprintf(detail, sizeof(detail), "error=create-process winerr=%lu",
                  (unsigned long)GetLastError());
        detail[sizeof(detail) - 1] = '\0';
        return report_result(config.mode, 4, "launch", detail);
    }
    CloseHandle(process.hThread);
    active_process = process.hProcess;
    active_process_id = process.dwProcessId;
    launcher_creation_stamp = process_creation_stamp(active_process);
    owned_process_ids[owned_process_count++] = active_process_id;
    _snprintf(detail, sizeof(detail), "pid=%lu",
              (unsigned long)process.dwProcessId);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config.mode, "launched", detail);

    WaitForInputIdle(active_process, 10000);
    started = GetTickCount();
    for (;;) {
        elapsed = GetTickCount() - started;
        wait_status = WaitForSingleObject(active_process, 0);
        if (ut_gameplay && !gameplay_reached &&
            read_ut_gameplay_semantic(&config, directory,
                                      &gameplay_phase)) {
            gameplay_reached = 1;
            report_stage(config.mode, "semantic",
                         "transition=dm-antalus-ready "
                         "evidence=browse-load-deathmatch-level-play");
        }
        /* Toolhelp module snapshots can block in old XP loader states.  Once
         * a 3D runtime has been identified, its path is already proven and
         * repeating the snapshot adds no diagnostic value. */
        if (!(modules & ((1U << 0) | (1U << 1) | (1U << 3)))) {
            modules = report_modules(
                &config, active_process_id, modules, &system_modules);
        }
        window = find_process_window(active_process_id);
        if (window && window != last_window) {
            report_window(&config, window);
            last_window = window;
        }
        if (visual_observation && !ut_gameplay && !visual_ready &&
            elapsed >= 30000UL && window &&
            window == GetForegroundWindow() &&
            (modules & visual_renderer_module) &&
            (system_modules & visual_renderer_module)) {
            visual_ready = 1;
            visual_ready_started = GetTickCount();
            _snprintf(detail, sizeof(detail),
                      "boundary=foreground-%s-window-stable warmup-ms=30000",
                      visual_renderer);
            detail[sizeof(detail) - 1] = '\0';
            report_stage(config.mode, "visual-ready", detail);
        }
        if ((performance_observation || warcraft_d3d8_oracle) &&
            !measurement_started && elapsed >= 15000UL && window &&
            (!ut_gameplay ||
             (gameplay_reached && gameplay_input_ready)) &&
            (modules & performance_renderer_module) &&
            (!performance_requires_system ||
             (system_modules & performance_renderer_module))) {
            measurement_started = GetTickCount();
            _snprintf(detail, sizeof(detail),
                      "warmup-ms=15000 renderer=%s%s",
                      performance_renderer,
                      performance_requires_system ? " system32=1" : "");
            detail[sizeof(detail) - 1] = '\0';
            report_stage(config.mode,
                         warcraft_d3d8_oracle ? "oracle-start" :
                                               "measurement-start",
                         detail);
        }
        if (window && ut_gameplay &&
            elapsed >= 12000 && fire_attempts < 3 &&
            (!fire_attempts || elapsed - last_fire >= 8000)) {
            INPUT input[2];
            UINT sent;

            memset(input, 0, sizeof(input));
            input[0].type = INPUT_MOUSE;
            input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            input[1].type = INPUT_MOUSE;
            input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SetForegroundWindow(window);
            sent = SendInput(2, input, sizeof(input[0]));
            if (sent == 2 && window == GetForegroundWindow())
                gameplay_input_ready = 1;
            ++fire_attempts;
            last_fire = elapsed;
            _snprintf(detail, sizeof(detail),
                      "action=fire attempt=%u injected=%u foreground=%d ready=%d",
                      fire_attempts, sent,
                      window == GetForegroundWindow(),
                      gameplay_input_ready);
            detail[sizeof(detail) - 1] = '\0';
            report_stage(config.mode, "input", detail);
        }
        if (wait_status == WAIT_OBJECT_0) {
            child_process_id = 0;
            child_name[0] = '\0';
            child_relationship[0] = '\0';
            for (adopt_attempt = 0; adopt_attempt < 20; ++adopt_attempt) {
                child_process_id = find_successor_process(
                    &config, owned_process_ids, owned_process_count,
                    active_process_id, launcher_creation_stamp,
                    child_name, sizeof(child_name), child_relationship,
                    sizeof(child_relationship));
                if (child_process_id) break;
                Sleep(100);
            }
            if (child_process_id && visual_observation && !ut_gameplay) {
                CloseHandle(active_process);
                report_log(&config, directory);
                report_application_events(&config, event_start);
                _snprintf(detail, sizeof(detail),
                          "error=process-handoff-not-allowed "
                          "successor-pid=%lu successor=%s relation=%s",
                          (unsigned long)child_process_id,
                          child_name[0] ? child_name : "(unknown)",
                          child_relationship[0] ? child_relationship :
                                                  "(unknown)");
                detail[sizeof(detail) - 1] = '\0';
                return report_result(config.mode, 12, "visual", detail);
            }
            if (child_process_id) {
                HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE |
                                           PROCESS_QUERY_INFORMATION,
                                           FALSE, child_process_id);
                if (child) {
                    if (warcraft_d3d8_oracle && measurement_started) {
                        measured_ms = GetTickCount() - measurement_started;
                        CloseHandle(child);
                        CloseHandle(active_process);
                        _snprintf(detail, sizeof(detail),
                                  "error=process-handoff-during-oracle "
                                  "successor-pid=%lu observed-ms=%lu",
                                  (unsigned long)child_process_id,
                                  (unsigned long)measured_ms);
                        detail[sizeof(detail) - 1] = '\0';
                        return report_result(
                            config.mode, 8, "oracle", detail);
                    }
                    CloseHandle(active_process);
                    active_process = child;
                    active_process_id = child_process_id;
                    if (owned_process_count <
                        sizeof(owned_process_ids) /
                            sizeof(owned_process_ids[0])) {
                        owned_process_ids[owned_process_count++] =
                            active_process_id;
                    }
                    modules = 0;
                    system_modules = 0;
                    window = NULL;
                    last_window = NULL;
                    if (warcraft_d3d8_oracle) started = GetTickCount();
                    _snprintf(detail, sizeof(detail),
                              "pid=%lu exe=%s relation=%s%s",
                              (unsigned long)active_process_id,
                              child_name[0] ? child_name : "(unknown)",
                              child_relationship[0] ? child_relationship :
                                                      "(unknown)",
                              warcraft_d3d8_oracle ?
                                  " oracle-warmup-reset=1" : "");
                    detail[sizeof(detail) - 1] = '\0';
                    report_stage(config.mode, "adopted", detail);
                    continue;
                }
            }
            GetExitCodeProcess(active_process, &exit_code);
            exited = 1;
            break;
        }
        if (visual_observation && !ut_gameplay &&
            qxp_ut2003_visual_should_stop(
                (unsigned long)elapsed, visual_ready,
                visual_ready ?
                    (unsigned long)(GetTickCount() -
                                    visual_ready_started) : 0UL))
            break;
        /* Process liveness wins over the elapsed-time boundary.  A process
         * signaled on the same poll that reaches the selected 25- or 45-second
         * sample threshold is an early exit, never a completed observation. */
        if (measurement_started &&
            GetTickCount() - measurement_started >=
                (warcraft_d3d8_oracle ? 25000UL : 45000UL)) {
            if (WaitForSingleObject(active_process, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(active_process, &exit_code);
                exited = 1;
            }
            break;
        }
        if ((performance_observation &&
             elapsed >= (ut_gameplay ? 120000UL : 90000UL)) ||
            (!performance_observation && !visual_observation &&
             elapsed >= 45000UL))
            break;
        Sleep(250);
    }

    if (visual_observation && !exited &&
        WaitForSingleObject(active_process, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(active_process, &exit_code);
        exited = 1;
    }
    if (ut_gameplay && !gameplay_reached &&
        read_ut_gameplay_semantic(&config, directory, &gameplay_phase)) {
        gameplay_reached = 1;
        report_stage(config.mode, "semantic",
                     "transition=dm-antalus-ready "
                     "evidence=browse-load-deathmatch-level-play");
    }
    if (visual_observation && !ut_gameplay && !visual_ready) {
        _snprintf(detail, sizeof(detail),
                  "boundary=foreground-%s-window-stable "
                  "elapsed-ms=%lu modules=%02x system-modules=%02x "
                  "foreground=%d",
                  visual_renderer,
                  (unsigned long)(GetTickCount() - started), modules,
                  system_modules, window && window == GetForegroundWindow());
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config.mode, "visual-ready-missing", detail);
        report_log(&config, directory);
        report_application_events(&config, event_start);
        if (!exited) close_game(active_process, window);
        CloseHandle(active_process);
        _snprintf(detail, sizeof(detail),
                  "error=foreground-system32-%s-window-not-stable",
                  visual_renderer);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(config.mode, 11, "visual-ready", detail);
    }
    if (ut_gameplay && (!gameplay_reached || !gameplay_input_ready)) {
        _snprintf(detail, sizeof(detail),
                  "transition=dm-antalus-ready "
                  "evidence=incomplete-fresh-log phase=%u input=%d",
                  gameplay_phase, gameplay_input_ready);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config.mode, "semantic-missing", detail);
        report_log(&config, directory);
        report_application_events(&config, event_start);
        if (!exited) close_game(active_process, window);
        CloseHandle(active_process);
        _snprintf(detail, sizeof(detail),
                  "error=gameplay-not-ready phase=%u input=%d",
                  gameplay_phase, gameplay_input_ready);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(config.mode, 11, "semantic", detail);
    }
    if (visual_observation) {
        visual_observed_ms = GetTickCount() - visual_ready_started;
        if (exited || visual_observed_ms < QXP_UT2003_MENU_OBSERVE_MS) {
            _snprintf(detail, sizeof(detail),
                      "error=observation-incomplete observed-ms=%lu "
                      "required-ms=%lu exited=%d exit=%lu",
                      (unsigned long)visual_observed_ms,
                      (unsigned long)QXP_UT2003_MENU_OBSERVE_MS,
                      exited, (unsigned long)exit_code);
            detail[sizeof(detail) - 1] = '\0';
            report_log(&config, directory);
            report_application_events(&config, event_start);
            if (!exited) close_game(active_process, window);
            CloseHandle(active_process);
            return report_result(config.mode, 10, "visual", detail);
        }
    }
    if ((visual_observation || ut_gameplay) &&
        (!(modules & visual_renderer_module) ||
         !(system_modules & visual_renderer_module))) {
        report_log(&config, directory);
        report_application_events(&config, event_start);
        if (!exited) close_game(active_process, window);
        CloseHandle(active_process);
        _snprintf(detail, sizeof(detail),
                  "error=system32-%s-not-ready", visual_renderer);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(
            config.mode, 9, ut_gameplay ? "gameplay" : "visual", detail);
    }
    if ((performance_observation || warcraft_d3d8_oracle) &&
        !measurement_started) {
        report_log(&config, directory);
        report_application_events(&config, event_start);
        if (!exited) close_game(active_process, window);
        CloseHandle(active_process);
        if (performance_requires_system &&
            !(system_modules & performance_renderer_module)) {
            _snprintf(detail, sizeof(detail),
                      "error=system32-%s-not-ready",
                      performance_renderer);
            detail[sizeof(detail) - 1] = '\0';
        } else {
            strcpy(detail, "error=renderer-scene-not-ready");
        }
        return report_result(
            config.mode, 6,
            warcraft_d3d8_oracle ? "oracle" : "measurement", detail);
    }
    if (warcraft_d3d8_oracle && measurement_started &&
        (exited || GetTickCount() - measurement_started < 25000UL)) {
        measured_ms = GetTickCount() - measurement_started;
        report_log(&config, directory);
        report_application_events(&config, event_start);
        CloseHandle(active_process);
        _snprintf(detail, sizeof(detail),
                  "error=game-exited-during-oracle observed-ms=%lu",
                  (unsigned long)measured_ms);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(config.mode, 7, "oracle", detail);
    }
    if (performance_observation && measurement_started &&
        (exited || GetTickCount() - measurement_started < 45000UL)) {
        measured_ms = GetTickCount() - measurement_started;
        report_log(&config, directory);
        report_application_events(&config, event_start);
        if (!exited) close_game(active_process, window);
        CloseHandle(active_process);
        _snprintf(detail, sizeof(detail),
                  "error=measurement-incomplete observed-ms=%lu "
                  "exited=%d exit=%lu",
                  (unsigned long)measured_ms, exited,
                  (unsigned long)exit_code);
        detail[sizeof(detail) - 1] = '\0';
        return report_result(config.mode, 7, "measurement", detail);
    }
    if (visual_observation) {
        _snprintf(detail, sizeof(detail),
                  "observed-ms=%lu required-ms=%lu renderer=%s "
                  "system32=1 boundary=post-visual-ready",
                  (unsigned long)visual_observed_ms,
                  (unsigned long)QXP_UT2003_MENU_OBSERVE_MS,
                  visual_renderer);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config.mode, "visual-complete", detail);
    }
    if (measurement_started) {
        measured_ms = GetTickCount() - measurement_started;
        _snprintf(detail, sizeof(detail),
                      "observed-ms=%lu renderer=%s%s",
                      (unsigned long)measured_ms, performance_renderer,
                      performance_requires_system ? " system32=1" : "");
        detail[sizeof(detail) - 1] = '\0';
        report_stage(config.mode,
                     warcraft_d3d8_oracle ? "oracle-complete" :
                                           "measurement-complete",
                     detail);
    }

    report_log(&config, directory);
    if (exited) {
        Sleep(500);
        report_application_events(&config, event_start);
    }
    _snprintf(detail, sizeof(detail), "outcome=%s exit=%lu modules=%02x",
              exited ? "exited" : "observed",
              (unsigned long)exit_code, modules);
    detail[sizeof(detail) - 1] = '\0';
    report_stage(config.mode, "observation-complete", detail);
    /* Give the host enough time to record the inclusive end of the observed
     * game interval before closing restores the desktop.  The D3D8 oracle's
     * host freezes its ring synchronously on oracle-complete; its longer grace
     * precedes game closure so the miniport can emit an attributable teardown
     * counter block without contaminating that frozen visual boundary. */
    if (!exited) {
        Sleep(warcraft_d3d8_oracle ? 3000UL : 1500UL);
        close_game(active_process, window);
        if (warcraft_d3d8_oracle) Sleep(500);
    }
    CloseHandle(active_process);
    if (warcraft_d3d8_oracle) {
        _snprintf(detail, sizeof(detail),
                  "outcome=%s exit=%lu modules=%02x observed-ms=%lu "
                  "renderer=d3d8 system32=1",
                  exited ? "exited" : "observed",
                  (unsigned long)exit_code, modules,
                  (unsigned long)measured_ms);
    } else {
        _snprintf(detail, sizeof(detail),
                  "outcome=%s exit=%lu modules=%02x observed-ms=%lu%s%s",
                  exited ? "exited" : "observed",
                  (unsigned long)exit_code, modules,
                  (unsigned long)(visual_observation ? visual_observed_ms :
                      (measurement_started ? measured_ms :
                       GetTickCount() - started)),
                  performance_observation ? " renderer=" : "",
                  performance_observation ? performance_renderer : "");
    }
    detail[sizeof(detail) - 1] = '\0';
    return report_result(config.mode, 0, "complete", detail);
}
