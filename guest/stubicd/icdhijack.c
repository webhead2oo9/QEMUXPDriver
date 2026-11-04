#include <windows.h>
#include <stdio.h>
#include <string.h>

#define ICD_ROOT "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\"
#define BACKUP_VALUE "XPDriverOriginalDll"
#define SHIM_NAME "XPDriverStub"

static int copy_stub_to_system(void)
{
    char source[MAX_PATH];
    char target[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, source, sizeof(source))) {
        return 0;
    }
    slash = strrchr(source, '\\');
    if (!slash) {
        return 0;
    }
    strcpy(slash + 1, "stubicd.dll");
    if (!GetSystemDirectoryA(target, sizeof(target))) {
        return 0;
    }
    if (strlen(target) + strlen("\\stubicd.dll") + 1 > sizeof(target)) {
        return 0;
    }
    strcat(target, "\\stubicd.dll");
    if (!CopyFileA(source, target, FALSE)) {
        fprintf(stderr, "CopyFile(%s -> %s) failed: %lu\n", source, target,
                (unsigned long)GetLastError());
        return 0;
    }
    printf("Installed %s\n", target);
    return 1;
}

static int open_driver_key(const char *name, HKEY *key)
{
    char path[512];
    if (_snprintf(path, sizeof(path), "%s%s", ICD_ROOT, name) < 0) {
        return 0;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, key) != ERROR_SUCCESS) {
        fprintf(stderr, "Cannot open HKLM\\%s; run as Administrator.\n", path);
        return 0;
    }
    return 1;
}

static int install(const char *name)
{
    HKEY key;
    char original[MAX_PATH];
    DWORD type = 0;
    DWORD size = sizeof(original);
    LONG status;

    if (!open_driver_key(name, &key)) {
        return 1;
    }
    status = RegQueryValueExA(key, BACKUP_VALUE, NULL, &type,
                              (BYTE *)original, &size);
    if (status != ERROR_SUCCESS) {
        size = sizeof(original);
        status = RegQueryValueExA(key, "Dll", NULL, &type,
                                  (BYTE *)original, &size);
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
            fprintf(stderr, "Cannot read original Dll value.\n");
            RegCloseKey(key);
            return 1;
        }
        status = RegSetValueExA(key, BACKUP_VALUE, 0, REG_SZ,
                                (const BYTE *)original,
                                (DWORD)strlen(original) + 1);
        if (status != ERROR_SUCCESS) {
            fprintf(stderr, "Cannot store backup value: %ld\n", (long)status);
            RegCloseKey(key);
            return 1;
        }
    }
    if (!copy_stub_to_system()) {
        RegCloseKey(key);
        return 1;
    }
    status = RegSetValueExA(key, "Dll", 0, REG_SZ,
                            (const BYTE *)"stubicd.dll", 12);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "Cannot set Dll value: %ld\n", (long)status);
        return 1;
    }
    printf("ICD %s now points to stubicd.dll; Version values were preserved.\n", name);
    return 0;
}

static int restore(const char *name)
{
    HKEY key;
    char original[MAX_PATH];
    DWORD type = 0;
    DWORD size = sizeof(original);
    LONG status;

    if (!open_driver_key(name, &key)) {
        return 1;
    }
    status = RegQueryValueExA(key, BACKUP_VALUE, NULL, &type,
                              (BYTE *)original, &size);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        fprintf(stderr, "No %s backup value exists.\n", BACKUP_VALUE);
        RegCloseKey(key);
        return 1;
    }
    status = RegSetValueExA(key, "Dll", 0, type, (const BYTE *)original,
                            (DWORD)strlen(original) + 1);
    if (status == ERROR_SUCCESS) {
        RegDeleteValueA(key, BACKUP_VALUE);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "Restore failed: %ld\n", (long)status);
        return 1;
    }
    printf("ICD %s restored to %s\n", name, original);
    return 0;
}

static int shim_install(void)
{
    HKEY key;
    DWORD disposition;
    DWORD version = 2;
    DWORD driver_version = 1;
    DWORD flags = 1;
    char path[512];
    LONG status;

    if (!copy_stub_to_system()) {
        return 1;
    }
    _snprintf(path, sizeof(path), "%s%s", ICD_ROOT, SHIM_NAME);
    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key,
                             &disposition);
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "Cannot create HKLM\\%s: %ld\n", path, (long)status);
        return 1;
    }
    status = RegSetValueExA(key, "Dll", 0, REG_SZ,
                            (const BYTE *)"stubicd.dll", 12);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExA(key, "Version", 0, REG_DWORD,
                                (const BYTE *)&version, sizeof(version));
    }
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExA(key, "DriverVersion", 0, REG_DWORD,
                                (const BYTE *)&driver_version,
                                sizeof(driver_version));
    }
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExA(key, "Flags", 0, REG_DWORD,
                                (const BYTE *)&flags, sizeof(flags));
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "Cannot populate shim registry key: %ld\n", (long)status);
        return 1;
    }
    printf("Created OpenGLDrivers\\%s for the glprobe escape shim.\n", SHIM_NAME);
    return 0;
}

static int shim_remove(void)
{
    char path[512];
    LONG status;
    _snprintf(path, sizeof(path), "%s%s", ICD_ROOT, SHIM_NAME);
    status = RegDeleteKeyA(HKEY_LOCAL_MACHINE, path);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        fprintf(stderr, "Cannot remove HKLM\\%s: %ld\n", path, (long)status);
        return 1;
    }
    printf("Removed OpenGLDrivers\\%s.\n", SHIM_NAME);
    return 0;
}

static int load_test(void)
{
    static const char *exports[] = {
        "DrvCopyContext", "DrvCreateContext", "DrvCreateLayerContext",
        "DrvDeleteContext", "DrvDescribeLayerPlane", "DrvDescribePixelFormat",
        "DrvGetLayerPaletteEntries", "DrvGetProcAddress", "DrvReleaseContext",
        "DrvRealizeLayerPalette", "DrvSetContext", "DrvSetLayerPaletteEntries",
        "DrvSetPixelFormat", "DrvShareLists", "DrvSwapBuffers",
        "DrvSwapLayerBuffers", "DrvValidateVersion", "DrvSetCallbackProcs"
    };
    typedef BOOL (APIENTRY *validate_fn)(ULONG);
    typedef LONG (APIENTRY *describe_fn)(HDC, INT, ULONG,
                                         PIXELFORMATDESCRIPTOR *);
    HMODULE module;
    validate_fn validate;
    describe_fn describe;
    HDC dc;
    PIXELFORMATDESCRIPTOR pfd;
    LONG formats;
    unsigned i;
    int missing = 0;

    SetLastError(0);
    module = LoadLibraryA("stubicd.dll");
    printf("LoadLibrary(stubicd.dll)=%08lx error=%lu\n",
           (unsigned long)(ULONG_PTR)module,
           (unsigned long)(module ? ERROR_SUCCESS : GetLastError()));
    if (!module) {
        return 1;
    }
    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); ++i) {
        if (!GetProcAddress(module, exports[i])) {
            printf("missing export: %s\n", exports[i]);
            missing = 1;
        }
    }
    validate = (validate_fn)GetProcAddress(module, "DrvValidateVersion");
    describe = (describe_fn)GetProcAddress(module, "DrvDescribePixelFormat");
    if (!missing && validate && describe) {
        memset(&pfd, 0, sizeof(pfd));
        dc = GetDC(NULL);
        printf("DrvValidateVersion(1)=%d\n", validate(1));
        formats = describe(dc, 1, sizeof(pfd), &pfd);
        printf("DrvDescribePixelFormat(1)=%ld\n", (long)formats);
        ReleaseDC(NULL, dc);
    }
    FreeLibrary(module);
    printf("export check: %s\n", missing ? "FAILED" : "OK");
    return missing;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "load-test")) {
        return load_test();
    }
    if (argc == 2 && !strcmp(argv[1], "shim-install")) {
        return shim_install();
    }
    if (argc == 2 && !strcmp(argv[1], "shim-remove")) {
        return shim_remove();
    }
    if (argc != 3 || (strcmp(argv[1], "install") && strcmp(argv[1], "restore"))) {
        fprintf(stderr, "Usage:\n"
                        "  icdhijack install|restore DriverName\n"
                        "  icdhijack shim-install|shim-remove|load-test\n");
        return 2;
    }
    return !strcmp(argv[1], "install") ? install(argv[2]) : restore(argv[2]);
}
