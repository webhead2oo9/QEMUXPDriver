#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

#define QXP_HARDWARE_ID        L"PCI\\VEN_10DE&DEV_0250&SUBSYS_11001AF4"
#define QXP_HARDWARE_ID_LEGACY L"PCI\\VEN_1234&DEV_1111"
#define INSTALLFLAG_FORCE 0x00000001
#define INSTALLFLAG_NONINTERACTIVE 0x00000004
/* setupapi.h: no present device carries the requested ID. */
#define QXP_ERROR_NO_SUCH_DEVINST 0xE000020BUL

typedef BOOL (WINAPI *UPDATE_DRIVER_W)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

static FILE *log_file;
static char package_driver_ver[64];
static char package_signer_thumbprint[41];

static int sibling_path(WCHAR *path, const WCHAR *name);

static int read_inf_driver_ver(const WCHAR *path)
{
    HANDLE file;
    DWORD size;
    DWORD read;
    char *text;
    char *line;
    int found = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (!size || size == INVALID_FILE_SIZE || size > 65535) {
        CloseHandle(file);
        return 0;
    }
    text = HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!text) {
        CloseHandle(file);
        return 0;
    }
    if (!ReadFile(file, text, size, &read, NULL) || read != size) {
        HeapFree(GetProcessHeap(), 0, text);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    text[size] = '\0';
    line = text;
    while (*line) {
        char *cursor = line;
        char *end = strpbrk(line, "\r\n");
        char *equals;
        char *value;
        size_t output = 0;

        if (!end) end = line + strlen(line);
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        if ((size_t)(end - cursor) >= 9 &&
            !_strnicmp(cursor, "DriverVer", 9) &&
            (cursor + 9 == end || cursor[9] == ' ' ||
             cursor[9] == '\t' || cursor[9] == '=')) {
            equals = (char *)memchr(cursor + 9, '=', end - (cursor + 9));
            if (equals) {
                value = equals + 1;
                while (value < end) {
                    if (*value == ';') break;
                    if (*value != ' ' && *value != '\t') {
                        if (output + 1 >= sizeof(package_driver_ver)) {
                            output = 0;
                            break;
                        }
                        package_driver_ver[output++] = *value;
                    }
                    ++value;
                }
                package_driver_ver[output] = '\0';
                if (output && strchr(package_driver_ver, ',')) {
                    if (found) {
                        found = 0;
                        break;
                    }
                    found = 1;
                }
            }
        }
        line = *end ? end + 1 : end;
        if (*line == '\n' && end[0] == '\r') ++line;
    }
    HeapFree(GetProcessHeap(), 0, text);
    return found;
}

static void send_status(const char *message)
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
        ZeroMemory(&address, sizeof(address));
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

static void report_failure(int rc, DWORD error)
{
    char message[96];

    if (_snprintf(message, sizeof(message),
                  "QXPICD_RESULT rc=%d error=%lu", rc,
                  (unsigned long)error) > 0) {
        send_status(message);
    }
}

static void log_status(const char *stage, DWORD value)
{
    if (!log_file) return;
    fprintf(log_file, "%s=%lu\n", stage, (unsigned long)value);
    fflush(log_file);
}

static void set_signing_policy(HKEY root)
{
    static const BYTE ignore = 0;
    HKEY key;

    if (RegCreateKeyExA(root, "SOFTWARE\\Microsoft\\Driver Signing", 0,
                        NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) ==
        ERROR_SUCCESS) {
        RegSetValueExA(key, "Policy", 0, REG_BINARY, &ignore, sizeof(ignore));
        RegCloseKey(key);
    }
}

static int trust_test_certificate(void)
{
    static const WCHAR *store_names[] = { L"ROOT", L"TrustedPublisher" };
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD size;
    DWORD read;
    BYTE *encoded;
    PCCERT_CONTEXT certificate = NULL;
    BYTE thumbprint[20];
    DWORD thumbprint_size = sizeof(thumbprint);
    ULONG index;
    int result = 1;

    if (!sibling_path(path, L"xpdriver-test.cer")) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (!size || size == INVALID_FILE_SIZE) {
        CloseHandle(file);
        return 0;
    }
    encoded = HeapAlloc(GetProcessHeap(), 0, size);
    if (!encoded) {
        CloseHandle(file);
        return 0;
    }
    if (!ReadFile(file, encoded, size, &read, NULL) || read != size) result = 0;
    CloseHandle(file);
    certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded, size);
    if (!certificate ||
        !CertGetCertificateContextProperty(
            certificate, CERT_SHA1_HASH_PROP_ID, thumbprint,
            &thumbprint_size) || thumbprint_size != sizeof(thumbprint)) {
        result = 0;
    }
    if (result) {
        static const char hex[] = "0123456789ABCDEF";
        for (index = 0; index < sizeof(thumbprint); ++index) {
            package_signer_thumbprint[index * 2] =
                hex[(thumbprint[index] >> 4) & 15];
            package_signer_thumbprint[index * 2 + 1] =
                hex[thumbprint[index] & 15];
        }
        package_signer_thumbprint[40] = '\0';
    }
    for (index = 0; result && index < sizeof(store_names) / sizeof(store_names[0]);
         ++index) {
        HCERTSTORE store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE,
            store_names[index]);
        if (!store ||
            !CertAddEncodedCertificateToStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded, size,
                CERT_STORE_ADD_REPLACE_EXISTING, NULL)) {
            result = 0;
        }
        if (store) CertCloseStore(store, 0);
    }
    if (certificate) CertFreeCertificateContext(certificate);
    HeapFree(GetProcessHeap(), 0, encoded);
    return result;
}

static int report_package_identity(void)
{
    char message[176];
    int length;

    if (!package_driver_ver[0] || !package_signer_thumbprint[0]) return 0;
    length = _snprintf(
        message, sizeof(message),
        "QXPSETUP identity driverVer=%s signerThumbprint=%s",
        package_driver_ver, package_signer_thumbprint);
    if (length <= 0 || length >= (int)sizeof(message)) return 0;
    send_status(message);
    return 1;
}

static int sibling_path(WCHAR *path, const WCHAR *name)
{
    WCHAR *slash;
    size_t prefix;
    size_t name_length = lstrlenW(name);

    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    prefix = (size_t)(slash + 1 - path);
    if (prefix + name_length + 1 > MAX_PATH) return 0;
    lstrcpyW(slash + 1, name);
    return 1;
}

static int schedule_native_test(void)
{
    WCHAR executable[MAX_PATH];
    WCHAR command[MAX_PATH + 32];
    HKEY key;
    LONG status;

    if (!sibling_path(executable, L"qxpwgltest.exe")) return 0;
    if (wsprintfW(command, L"\"%s\" --native", executable) <= 0) return 0;
    status = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
        NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) return 0;
    status = RegSetValueExW(key, L"XPDriverNativeWGL", 0, REG_SZ,
                            (const BYTE *)command,
                            (lstrlenW(command) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static int installed_driver_files_exist(void)
{
    WCHAR path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);

    if (!length || length >= MAX_PATH - 19) return 0;
    lstrcpyW(path + length, L"\\qxpdisp.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    lstrcpyW(path + length, L"\\qxpicd.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    lstrcpyW(path + length, L"\\qxpcoin2.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    lstrcpyW(path + length, L"\\drivers\\qxpmp.sys");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
    lstrcpyW(path + length, L"\\drivers\\qxpmap.sys");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int pending_map_replacement(void)
{
    HKEY key;
    WCHAR *entries = NULL;
    DWORD type;
    DWORD size = 0;
    DWORD capacity;
    DWORD count;
    DWORD offset;
    LONG status;
    int found = 0;

    status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
        0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return 0;
    status = RegQueryValueExW(key, L"PendingFileRenameOperations", NULL,
                              &type, NULL, &size);
    if (status != ERROR_SUCCESS || type != REG_MULTI_SZ ||
        size < sizeof(WCHAR) || size % sizeof(WCHAR)) goto done;
    capacity = size;
    entries = HeapAlloc(GetProcessHeap(), 0, capacity);
    if (!entries) goto done;
    status = RegQueryValueExW(key, L"PendingFileRenameOperations", NULL,
                              &type, (BYTE *)entries, &size);
    if (status != ERROR_SUCCESS || type != REG_MULTI_SZ || size > capacity ||
        size < sizeof(WCHAR) || size % sizeof(WCHAR)) goto done;
    count = size / sizeof(WCHAR);
    /* Empty deletion destinations can occur inside this REG_MULTI_SZ.
     * Walk the returned extent, not just the first double terminator. */
    offset = 0;
    while (offset < count) {
        DWORD basename = offset;

        while (offset < count && entries[offset]) {
            if (entries[offset] == L'\\' || entries[offset] == L'/') {
                basename = offset + 1;
            }
            ++offset;
        }
        if (offset == count) {
            found = 0;
            break;
        }
        if (!lstrcmpiW(entries + basename, L"qxpmap.sys")) found = 1;
        ++offset;
    }
done:
    if (entries) HeapFree(GetProcessHeap(), 0, entries);
    RegCloseKey(key);
    return found;
}

static DWORD verify_installed_map_service(void)
{
    static const WCHAR *names[] = { L"Type", L"Start" };
    static const DWORD expected[] = {
        SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START
    };
    HKEY key;
    LONG status;
    ULONG index;

    status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\qxpmap",
        0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return (DWORD)status;
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        DWORD type;
        DWORD value;
        DWORD size = sizeof(value);

        status = RegQueryValueExW(key, names[index], NULL, &type,
                                  (BYTE *)&value, &size);
        if (status != ERROR_SUCCESS) break;
        if (type != REG_DWORD || size != sizeof(value) ||
            (value != expected[index] &&
             !(index == 1 && value == SERVICE_AUTO_START &&
               pending_map_replacement()))) {
            status = ERROR_INVALID_DATA;
            break;
        }
    }
    RegCloseKey(key);
    return (DWORD)status;
}

static int enable_shutdown_privilege(void)
{
    HANDLE token;
    TOKEN_PRIVILEGES privileges;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return 0;
    }
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME,
                               &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return 0;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

int main(int argc, char **argv)
{
    WCHAR inf_path[MAX_PATH];
    HMODULE newdev;
    UPDATE_DRIVER_W update_driver;
    BOOL reboot_required = FALSE;
    DWORD error;
    int rc = 0;
    int stage_only = argc > 1 && !lstrcmpiA(argv[1], "--stage-only");
    /* --user (the tools CD's SETUP.CMD): the same install, verification and
     * restart, without scheduling the harness's native WGL test. */
    int user_mode = argc > 1 && !lstrcmpiA(argv[1], "--user");

    log_file = fopen("C:\\QXPSETUP.LOG", "w");
    send_status("QXPSETUP stage=start");
    if (!sibling_path(inf_path, L"qxp.inf")) {
        rc = 201;
        error = GetLastError();
        goto failed;
    }
    if (!read_inf_driver_ver(inf_path)) {
        rc = 209;
        error = ERROR_INVALID_DATA;
        goto failed;
    }
    set_signing_policy(HKEY_CURRENT_USER);
    set_signing_policy(HKEY_LOCAL_MACHINE);
    if (!trust_test_certificate()) {
        rc = 206;
        error = GetLastError();
        goto failed;
    }
    newdev = LoadLibraryA("newdev.dll");
    if (!newdev) {
        rc = 202;
        error = GetLastError();
        goto failed;
    }
    update_driver = (UPDATE_DRIVER_W)GetProcAddress(
        newdev, "UpdateDriverForPlugAndPlayDevicesW");
    send_status("QXPSETUP stage=updating");
    if (!update_driver) {
        rc = 203;
        error = GetLastError();
        FreeLibrary(newdev);
        goto failed;
    }
    /* Install against the NVIDIA GeForce4 Ti 4600 identity first. Only a VM
     * without that device falls back to the legacy QEMU std-vga ID; any other
     * failure belongs to the device that is present, so its error is the one
     * reported (the fallback would only add "no such device"). */
    reboot_required = FALSE;
    if (!update_driver(NULL, QXP_HARDWARE_ID, inf_path,
                       INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE,
                       &reboot_required)) {
        error = GetLastError();
        log_status("UPDATE_PRIMARY_ERROR", error);
        if (error != QXP_ERROR_NO_SUCH_DEVINST ||
            !update_driver(NULL, QXP_HARDWARE_ID_LEGACY, inf_path,
                           INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE,
                           &reboot_required)) {
            if (error == QXP_ERROR_NO_SUCH_DEVINST) error = GetLastError();
            rc = 203;
            FreeLibrary(newdev);
            goto failed;
        }
    }
    FreeLibrary(newdev);
    send_status("QXPSETUP stage=installed");
    log_status("UPDATE_OK", 1);
    log_status("REBOOT_REQUIRED", reboot_required);
    if (!installed_driver_files_exist()) {
        rc = 207;
        error = ERROR_FILE_NOT_FOUND;
        goto failed;
    }
    log_status("FILES_OK", 1);
    error = verify_installed_map_service();
    if (error != ERROR_SUCCESS) {
        rc = 211;
        goto failed;
    }
    log_status("QXPMAP_SERVICE_OK", 1);
    send_status("QXPSETUP stage=files-verified");
    if (!report_package_identity()) {
        rc = 210;
        error = ERROR_INVALID_DATA;
        goto failed;
    }
    if (!user_mode && !schedule_native_test()) {
        rc = 204;
        error = GetLastError();
        goto failed;
    }
    error = RegFlushKey(HKEY_LOCAL_MACHINE);
    if (error != ERROR_SUCCESS) {
        rc = 208;
        goto failed;
    }
    send_status("QXPSETUP stage=registry-flushed");
    if (stage_only) {
        send_status("QXPSETUP stage=poweroff-ready");
        if (log_file) fclose(log_file);
        return 0;
    }
    send_status("QXPSETUP stage=rebooting");
    if (!enable_shutdown_privilege() ||
        !ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                       SHTDN_REASON_MAJOR_HARDWARE |
                       SHTDN_REASON_MINOR_INSTALLATION)) {
        rc = 205;
        error = GetLastError();
        goto failed;
    }
    if (log_file) fclose(log_file);
    return 0;

failed:
    log_status("RESULT", rc);
    log_status("ERROR", error);
    if (log_file) fclose(log_file);
    report_failure(rc, error);
    return rc;
}
