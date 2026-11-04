#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

#define MANAGER_HANDLE ((SC_HANDLE)(ULONG_PTR)1)
#define SERVICE_HANDLE ((SC_HANDLE)(ULONG_PTR)2)
#define REGISTRY_HANDLE ((HKEY)(ULONG_PTR)3)

static unsigned failures;
static unsigned cases;
static unsigned manager_calls, open_calls, create_calls, query_calls;
static unsigned change_calls, start_calls, manager_closes, service_closes;
static unsigned registry_calls, registry_writes, registry_flushes, registry_closes;
static DWORD open_error, manager_error, query_error, change_error, start_error;
static DWORD create_error, registry_error;
static DWORD service_type, start_type, error_control;
static DWORD expected_change_start;
static const WCHAR *image_path;
static const WCHAR *windows_directory;

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "co-installer test failed: %s\n", message);
        ++failures;
    }
}

static void reset_fixture(void)
{
    ++cases;
    manager_calls = open_calls = create_calls = query_calls = 0;
    change_calls = start_calls = manager_closes = service_closes = 0;
    registry_calls = registry_writes = registry_flushes = registry_closes = 0;
    manager_error = query_error = change_error = start_error = 0;
    create_error = registry_error = 0;
    open_error = ERROR_SERVICE_DOES_NOT_EXIST;
    service_type = SERVICE_KERNEL_DRIVER;
    start_type = SERVICE_SYSTEM_START;
    error_control = SERVICE_ERROR_NORMAL;
    expected_change_start = SERVICE_NO_CHANGE;
    image_path = L"\\SystemRoot\\system32\\drivers\\qxpmap.sys";
    windows_directory = L"C:\\WINDOWS";
}

static SC_HANDLE WINAPI fake_OpenSCManagerW(LPCWSTR machine, LPCWSTR database,
                                           DWORD access)
{
    ++manager_calls;
    require(!machine && !database &&
            access == (SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE),
            "SCM access must be limited to connect/create");
    SetLastError(manager_error);
    return manager_error ? NULL : MANAGER_HANDLE;
}

static SC_HANDLE WINAPI fake_OpenServiceW(SC_HANDLE manager, LPCWSTR name,
                                          DWORD access)
{
    ++open_calls;
    require(manager == MANAGER_HANDLE && !lstrcmpW(name, L"qxpmap") &&
            access == (SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG |
                       SERVICE_START), "open exact shared service without delete/stop access");
    SetLastError(open_error);
    return open_error ? NULL : SERVICE_HANDLE;
}

static SC_HANDLE WINAPI fake_CreateServiceW(
    SC_HANDLE manager, LPCWSTR name, LPCWSTR display, DWORD access,
    DWORD type, DWORD start, DWORD control, LPCWSTR binary, LPCWSTR group,
    LPDWORD tag, LPCWSTR dependencies, LPCWSTR account, LPCWSTR password)
{
    ++create_calls;
    require(manager == MANAGER_HANDLE && !lstrcmpW(name, L"qxpmap") &&
            !lstrcmpW(display, L"XPDriver guest memory-window driver") &&
            access == SERVICE_START && type == SERVICE_KERNEL_DRIVER &&
            start == SERVICE_SYSTEM_START && control == SERVICE_ERROR_NORMAL &&
            !lstrcmpW(binary, L"\\SystemRoot\\system32\\drivers\\qxpmap.sys") &&
            !group && !tag && !dependencies && !account && !password,
            "create must use the exact shared service identity");
    SetLastError(create_error);
    return create_error ? NULL : SERVICE_HANDLE;
}

static BOOL WINAPI fake_QueryServiceConfigW(
    SC_HANDLE service, LPQUERY_SERVICE_CONFIGW config, DWORD size, LPDWORD needed)
{
    ++query_calls;
    require(service == SERVICE_HANDLE && size >= sizeof(*config),
            "query service configuration buffer");
    *needed = sizeof(*config);
    SetLastError(query_error);
    if (query_error) return FALSE;
    ZeroMemory(config, sizeof(*config));
    config->dwServiceType = service_type;
    config->dwStartType = start_type;
    config->dwErrorControl = error_control;
    config->lpBinaryPathName = (LPWSTR)image_path;
    return TRUE;
}

static BOOL WINAPI fake_ChangeServiceConfigW(
    SC_HANDLE service, DWORD type, DWORD start, DWORD control, LPCWSTR binary,
    LPCWSTR group, LPDWORD tag, LPCWSTR dependencies, LPCWSTR account,
    LPCWSTR password, LPCWSTR display)
{
    ++change_calls;
    require(service == SERVICE_HANDLE && type == SERVICE_NO_CHANGE &&
            start == expected_change_start && control == SERVICE_ERROR_NORMAL &&
            !binary && !group && !tag && !dependencies && !account &&
            !password && !display, "reconfigure only start type/error control");
    SetLastError(change_error);
    return change_error == NO_ERROR;
}

static BOOL WINAPI fake_StartServiceW(SC_HANDLE service, DWORD count,
                                      LPCWSTR *arguments)
{
    ++start_calls;
    require(service == SERVICE_HANDLE && count == 0 && !arguments,
            "start only the owned shared service");
    SetLastError(start_error);
    return start_error == NO_ERROR;
}

static BOOL WINAPI fake_CloseServiceHandle(SC_HANDLE handle)
{
    if (handle == MANAGER_HANDLE) ++manager_closes;
    else if (handle == SERVICE_HANDLE) ++service_closes;
    else require(0, "unexpected service handle closed");
    return TRUE;
}

static UINT WINAPI fake_GetWindowsDirectoryW(LPWSTR path, UINT size)
{
    UINT length = (UINT)lstrlenW(windows_directory);

    if (size <= length) return length + 1;
    lstrcpyW(path, windows_directory);
    return length;
}

static LONG WINAPI fake_RegCreateKeyExA(
    HKEY root, LPCSTR name, DWORD reserved, LPSTR class_name, DWORD options,
    REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY key,
    LPDWORD disposition)
{
    ++registry_calls;
    require(root == HKEY_LOCAL_MACHINE && !strcmp(name,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\qxpicd") &&
            !reserved && !class_name && options == REG_OPTION_NON_VOLATILE &&
            access == KEY_SET_VALUE && !security, "preserve ICD key creation");
    if (registry_error) return (LONG)registry_error;
    *key = REGISTRY_HANDLE;
    *disposition = REG_CREATED_NEW_KEY;
    return ERROR_SUCCESS;
}

static LONG WINAPI fake_RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved,
                                       DWORD type, const BYTE *data, DWORD size)
{
    static const char *names[] = { "Dll", "Version", "DriverVersion", "Flags" };
    static const DWORD values[] = { 0, 2, 1, 1 };
    DWORD value = 0;

    require(key == REGISTRY_HANDLE && !reserved && registry_writes < 4,
            "preserve ICD write count");
    if (registry_writes >= 4) return ERROR_INVALID_DATA;
    require(!strcmp(name, names[registry_writes]), "preserve ICD tuple order");
    if (!registry_writes) {
        require(type == REG_SZ && size == sizeof("qxpicd.dll") &&
                !memcmp(data, "qxpicd.dll", sizeof("qxpicd.dll")),
                "preserve ICD DLL value");
    } else {
        if (size == sizeof(value)) memcpy(&value, data, sizeof(value));
        require(type == REG_DWORD && size == sizeof(value) &&
                value == values[registry_writes], "preserve ICD DWORD tuple");
    }
    ++registry_writes;
    return ERROR_SUCCESS;
}

static LONG WINAPI fake_RegFlushKey(HKEY key)
{
    require(key == REGISTRY_HANDLE, "flush only fake registry key");
    ++registry_flushes;
    return ERROR_SUCCESS;
}

static LONG WINAPI fake_RegCloseKey(HKEY key)
{
    require(key == REGISTRY_HANDLE, "close only fake registry key");
    ++registry_closes;
    return ERROR_SUCCESS;
}

/* Include the production callback with every stateful Windows API intercepted. */
#define OpenSCManagerW fake_OpenSCManagerW
#define OpenServiceW fake_OpenServiceW
#define CreateServiceW fake_CreateServiceW
#define QueryServiceConfigW fake_QueryServiceConfigW
#define ChangeServiceConfigW fake_ChangeServiceConfigW
#define StartServiceW fake_StartServiceW
#define CloseServiceHandle fake_CloseServiceHandle
#define GetWindowsDirectoryW fake_GetWindowsDirectoryW
#define RegCreateKeyExA fake_RegCreateKeyExA
#define RegSetValueExA fake_RegSetValueExA
#define RegFlushKey fake_RegFlushKey
#define RegCloseKey fake_RegCloseKey
#include "qxpcoinst.c"

static DWORD post_install(DWORD result)
{
    COINSTALLER_CONTEXT_DATA context;

    ZeroMemory(&context, sizeof(context));
    context.PostProcessing = TRUE;
    context.InstallResult = result;
    return QxpCoInstaller(DIF_INSTALLDEVICE, NULL, NULL, &context);
}

int main(void)
{
    static const WCHAR *accepted[] = {
        L"\\systemroot\\SYSTEM32\\DRIVERS\\QXPMAP.SYS",
        L"SYSTEM32\\DRIVERS\\QXPMAP.SYS",
        L"%systemroot%\\SYSTEM32\\DRIVERS\\QXPMAP.SYS",
        L"\\??\\c:\\windows\\SYSTEM32\\DRIVERS\\QXPMAP.SYS",
        L"c:\\windows\\SYSTEM32\\DRIVERS\\QXPMAP.SYS"
    };
    static const WCHAR *foreign[] = {
        L"C:\\Other\\qxpmap.sys",
        L"\\SystemRoot\\system32\\drivers\\foreign.sys",
        L"\"C:\\WINDOWS\\system32\\drivers\\qxpmap.sys\""
    };
    COINSTALLER_CONTEXT_DATA context;
    unsigned index;

    reset_fixture();
    ZeroMemory(&context, sizeof(context));
    require(QxpCoInstaller(DIF_INSTALLDEVICE, NULL, NULL, &context) ==
            ERROR_DI_POSTPROCESSING_REQUIRED, "request post-processing");
    require(registry_calls == 1 && registry_writes == 4 && registry_flushes == 1 &&
            registry_closes == 1 && !manager_calls,
            "pre-processing keeps ICD registration and does not touch SCM");

    reset_fixture();
    registry_error = ERROR_ACCESS_DENIED;
    require(QxpCoInstaller(DIF_INSTALLDEVICE, NULL, NULL, &context) ==
            ERROR_ACCESS_DENIED && !manager_calls,
            "preserve ICD registration failure");

    reset_fixture();
    require(post_install(NO_ERROR) == NO_ERROR, "absent service is best effort");
    require(create_calls == 1 && !query_calls && !change_calls && start_calls == 1 &&
            manager_closes == 1 && service_closes == 1 && !registry_calls,
            "absent service is created, started, closed without rewriting ICD");

    reset_fixture();
    open_error = NO_ERROR;
    start_type = SERVICE_DEMAND_START;
    error_control = SERVICE_ERROR_IGNORE;
    expected_change_start = SERVICE_SYSTEM_START;
    require(post_install(NO_ERROR) == NO_ERROR, "owned service reconfigure");
    require(!create_calls && query_calls == 1 && change_calls == 1 &&
            start_calls == 1 && manager_closes == 1 && service_closes == 1,
            "owned service gets SYSTEM_START and NORMAL");

    for (index = SERVICE_BOOT_START; index <= SERVICE_AUTO_START; ++index) {
        reset_fixture();
        open_error = NO_ERROR;
        start_type = index;
        require(post_install(NO_ERROR) == NO_ERROR && !change_calls &&
                start_calls == 1 && manager_closes == 1 && service_closes == 1,
                "owned BOOT/SYSTEM/AUTO start type remains untouched");

        reset_fixture();
        open_error = NO_ERROR;
        start_type = index;
        error_control = SERVICE_ERROR_IGNORE;
        require(post_install(NO_ERROR) == NO_ERROR && change_calls == 1 &&
                start_calls == 1 && manager_closes == 1 && service_closes == 1,
                "error-control correction preserves BOOT/SYSTEM/AUTO start type");
    }
    for (index = SERVICE_DEMAND_START; index <= SERVICE_DISABLED; ++index) {
        reset_fixture();
        open_error = NO_ERROR;
        start_type = index;
        expected_change_start = SERVICE_SYSTEM_START;
        require(post_install(NO_ERROR) == NO_ERROR && change_calls == 1 &&
                start_calls == 1 && manager_closes == 1 && service_closes == 1,
                "owned DEMAND/DISABLED service is promoted to SYSTEM_START");
    }

    for (index = 0; index < sizeof(accepted) / sizeof(accepted[0]); ++index) {
        reset_fixture();
        open_error = NO_ERROR;
        image_path = accepted[index];
        require(post_install(NO_ERROR) == NO_ERROR, "accepted image spelling");
        require(!create_calls && !change_calls && start_calls == 1 &&
                manager_closes == 1 && service_closes == 1,
                "case-insensitive known path is reused without rewriting it");
    }
    reset_fixture();
    open_error = NO_ERROR;
    windows_directory = L"D:\\WINXP";
    image_path = L"\\??\\D:\\WINXP\\system32\\drivers\\qxpmap.sys";
    require(post_install(NO_ERROR) == NO_ERROR && start_calls == 1,
            "native path uses the actual Windows directory");

    for (index = 0; index < sizeof(foreign) / sizeof(foreign[0]); ++index) {
        reset_fixture();
        open_error = NO_ERROR;
        image_path = foreign[index];
        require(post_install(NO_ERROR) == NO_ERROR, "foreign path is nonfatal");
        require(!create_calls && !change_calls && !start_calls &&
                manager_closes == 1 && service_closes == 1,
                "foreign path remains untouched");
    }
    reset_fixture();
    open_error = NO_ERROR;
    service_type = SERVICE_WIN32_OWN_PROCESS;
    require(post_install(NO_ERROR) == NO_ERROR && !change_calls && !start_calls,
            "foreign service type remains untouched");

    reset_fixture();
    open_error = ERROR_SERVICE_MARKED_FOR_DELETE;
    require(post_install(NO_ERROR) == NO_ERROR && !create_calls && !query_calls &&
            !change_calls && !start_calls && manager_closes == 1 && !service_closes,
            "marked-for-deletion service remains untouched");

    reset_fixture();
    require(post_install(ERROR_ACCESS_DENIED) == NO_ERROR && !manager_calls &&
            !registry_calls, "failed display installation does nothing");
    context.PostProcessing = TRUE;
    context.InstallResult = NO_ERROR;
    require(QxpCoInstaller(DIF_REMOVE, NULL, NULL, &context) == NO_ERROR &&
            !manager_calls && !registry_calls, "removal never deletes shared service");

    reset_fixture();
    manager_error = ERROR_ACCESS_DENIED;
    require(post_install(NO_ERROR) == NO_ERROR && !open_calls,
            "SCM access failure does not fail display install");
    reset_fixture();
    create_error = ERROR_SERVICE_MARKED_FOR_DELETE;
    require(post_install(NO_ERROR) == NO_ERROR && create_calls == 1 && !start_calls &&
            manager_closes == 1, "create race is nonfatal and does not start service");
    reset_fixture();
    open_error = NO_ERROR;
    query_error = ERROR_ACCESS_DENIED;
    require(post_install(NO_ERROR) == NO_ERROR && !change_calls && !start_calls &&
            manager_closes == 1 && service_closes == 1,
            "unreadable configuration is left untouched");
    reset_fixture();
    open_error = NO_ERROR;
    start_type = SERVICE_AUTO_START;
    error_control = SERVICE_ERROR_IGNORE;
    change_error = ERROR_SERVICE_MARKED_FOR_DELETE;
    require(post_install(NO_ERROR) == NO_ERROR && change_calls == 1 && !start_calls &&
            manager_closes == 1 && service_closes == 1,
            "service deleted during reconfiguration is not started");
    reset_fixture();
    start_error = ERROR_SERVICE_ALREADY_RUNNING;
    require(post_install(NO_ERROR) == NO_ERROR && start_calls == 1 &&
            manager_closes == 1 && service_closes == 1,
            "already-running service is nonfatal");
    reset_fixture();
    start_error = ERROR_FILE_NOT_FOUND;
    require(post_install(NO_ERROR) == NO_ERROR && start_calls == 1 &&
            manager_closes == 1 && service_closes == 1,
            "any start failure is nonfatal");

    if (failures) return 1;
    printf("co-installer tests passed (%u fake registry/SCM cases)\n", cases);
    return 0;
}
