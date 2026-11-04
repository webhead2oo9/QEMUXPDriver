#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

#define QXP_ICD_KEY \
    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\qxpicd"

static LONG qxp_set_dword(HKEY key, const char *name, DWORD value)
{
    return RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&value,
                          sizeof(value));
}

static DWORD qxp_register_icd(void)
{
    static const char dll_name[] = "qxpicd.dll";
    HKEY key = NULL;
    DWORD disposition;
    LONG result;

    result = RegCreateKeyExA(HKEY_LOCAL_MACHINE, QXP_ICD_KEY, 0, NULL,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                             &key, &disposition);
    if (result != ERROR_SUCCESS) return (DWORD)result;

    result = RegSetValueExA(key, "Dll", 0, REG_SZ,
                            (const BYTE *)dll_name, sizeof(dll_name));
    if (result == ERROR_SUCCESS) result = qxp_set_dword(key, "Version", 2);
    if (result == ERROR_SUCCESS) {
        result = qxp_set_dword(key, "DriverVersion", 1);
    }
    if (result == ERROR_SUCCESS) result = qxp_set_dword(key, "Flags", 1);
    if (result == ERROR_SUCCESS) result = RegFlushKey(key);
    RegCloseKey(key);
    return (DWORD)result;
}

static int qxp_map_image_is_ours(const WCHAR *image)
{
    static const WCHAR suffix[] = L"\\system32\\drivers\\qxpmap.sys";
    WCHAR path[MAX_PATH + 4];
    UINT length;

    if (!image) return 0;
    if (!lstrcmpiW(image, L"\\SystemRoot\\system32\\drivers\\qxpmap.sys") ||
        !lstrcmpiW(image, L"system32\\drivers\\qxpmap.sys") ||
        !lstrcmpiW(image, L"%SystemRoot%\\system32\\drivers\\qxpmap.sys")) {
        return 1;
    }
    length = GetWindowsDirectoryW(path + 4, MAX_PATH);
    if (!length || length >= MAX_PATH) return 0;
    if (path[4 + length - 1] == L'\\') --length;
    if (length + sizeof(suffix) / sizeof(suffix[0]) > MAX_PATH) return 0;
    lstrcpyW(path + 4 + length, suffix);
    if (!lstrcmpiW(image, path + 4)) return 1;
    CopyMemory(path, L"\\??\\", 4 * sizeof(WCHAR));
    return !lstrcmpiW(image, path);
}

static void qxp_install_map_service(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    union {
        QUERY_SERVICE_CONFIGW value;
        BYTE bytes[8192];
    } config;
    DWORD needed;
    DWORD desired_start = SERVICE_NO_CHANGE;

    manager = OpenSCManagerW(NULL, NULL,
                            SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) return;
    service = OpenServiceW(manager, L"qxpmap",
                           SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG |
                           SERVICE_START);
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            service = CreateServiceW(
                manager, L"qxpmap", L"XPDriver guest memory-window driver",
                SERVICE_START, SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START,
                SERVICE_ERROR_NORMAL,
                L"\\SystemRoot\\system32\\drivers\\qxpmap.sys",
                NULL, NULL, NULL, NULL, NULL);
        }
    } else {
        if (!QueryServiceConfigW(service, &config.value, sizeof(config),
                                  &needed) ||
            config.value.dwServiceType != SERVICE_KERNEL_DRIVER ||
            !qxp_map_image_is_ours(config.value.lpBinaryPathName)) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return;
        }
        /* AUTO_START may be preserving a queued replacement across this boot. */
        if (config.value.dwStartType == SERVICE_DEMAND_START ||
            config.value.dwStartType == SERVICE_DISABLED) {
            desired_start = SERVICE_SYSTEM_START;
        }
        if ((desired_start != SERVICE_NO_CHANGE ||
             config.value.dwErrorControl != SERVICE_ERROR_NORMAL) &&
            !ChangeServiceConfigW(service, SERVICE_NO_CHANGE,
                                  desired_start, SERVICE_ERROR_NORMAL,
                                  NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return;
        }
    }
    if (service) {
        /* A running/busy driver or a start failure must not block display setup. */
        (void)StartServiceW(service, 0, NULL);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
}

__declspec(dllexport) DWORD CALLBACK
QxpCoInstaller(DI_FUNCTION install_function, HDEVINFO device_info_set,
               PSP_DEVINFO_DATA device_info,
               PCOINSTALLER_CONTEXT_DATA context_data)
{
    (void)device_info_set;
    (void)device_info;
    if (install_function != DIF_INSTALLDEVICE) return NO_ERROR;
    if (context_data && context_data->PostProcessing) {
        if (context_data->InstallResult == NO_ERROR) qxp_install_map_service();
        return NO_ERROR;
    } else {
        DWORD result = qxp_register_icd();

        if (result != NO_ERROR || !context_data) return result;
        return ERROR_DI_POSTPROCESSING_REQUIRED;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
