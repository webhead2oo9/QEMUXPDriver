#include <windows.h>
#include <stdio.h>
#include <string.h>

#define OPENGL_GETINFO 0x1101

typedef struct opengl_info {
    DWORD version;
    DWORD driver_version;
    WCHAR driver_name[MAX_PATH + 1];
} OPENGL_INFO;

int main(void)
{
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    DWORD query = OPENGL_GETINFO;
    OPENGL_INFO info;
    int supported;
    int result;
    char name[MAX_PATH + 1];

    if (!dc) {
        fprintf(stderr, "CreateDC(DISPLAY) failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    supported = ExtEscape(dc, QUERYESCSUPPORT, sizeof(query),
                          (LPCSTR)&query, 0, NULL);
    printf("QUERYESCSUPPORT(OPENGL_GETINFO)=%d\n", supported);
    memset(&info, 0, sizeof(info));
    result = ExtEscape(dc, OPENGL_GETINFO, 0, NULL, sizeof(info),
                       (LPSTR)&info);
    printf("OPENGL_GETINFO=%d buffer_size=0x%lx\n", result,
           (unsigned long)sizeof(info));
    if (result > 0) {
        WideCharToMultiByte(CP_ACP, 0, info.driver_name, -1, name,
                            sizeof(name), NULL, NULL);
        name[sizeof(name) - 1] = '\0';
        printf("Version=%lu\nDriverVersion=%lu\nDriverName=%s\n",
               (unsigned long)info.version,
               (unsigned long)info.driver_version, name);
    }
    DeleteDC(dc);
    return result > 0 ? 0 : 2;
}

