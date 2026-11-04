/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Whether the stock Direct3D 9 runtime can create a windowed HAL device on
 * the default adapter right now. It cannot while another desktop has the
 * input (locked workstation, secure desktop): CreateDevice then fails with
 * D3DERR_NOTAVAILABLE while Direct3D 9Ex, which the QD3D executor uses,
 * still works, so stock and QD3D runs would not be comparable.
 *
 * Loads the system d3d9.dll explicitly, so a proxy beside it does not
 * matter. Prints the result; exit status 0 when the device was created.
 *
 * Also prints every display attached to the desktop with its current mode
 * and position ("native_probe: displays ..."), so that the runner can stop
 * when a test changed the host's display configuration.
 */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>

static void print_displays(void)
{
    DISPLAY_DEVICEA dd;
    DEVMODEA dm;
    DWORD i;

    printf("native_probe: displays");
    for (i = 0;; ++i) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0))
            break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            printf(" %s=?", dd.DeviceName);
            continue;
        }
        printf(" %s=%lux%lux%lu@%lu%+ld%+ld", dd.DeviceName, (unsigned long)dm.dmPelsWidth,
               (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmBitsPerPel,
               (unsigned long)dm.dmDisplayFrequency, (long)dm.dmPosition.x, (long)dm.dmPosition.y);
    }
    printf("\n");
}

int main(void)
{
    typedef IDirect3D9 *(WINAPI *create9_fn)(UINT);
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
    HMODULE mod;
    create9_fn create;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND wnd;
    HRESULT hr;

    print_displays();
    if (!n || n >= MAX_PATH - 16)
        return 2;
    strcpy(path + n, "\\d3d9.dll");
    mod = LoadLibraryA(path);
    create = mod ? (create9_fn)(void (*)(void))GetProcAddress(mod, "Direct3DCreate9") : NULL;
    if (!create || !(d3d = create(D3D_SDK_VERSION))) {
        printf("native_probe: cannot load %s\n", path);
        return 2;
    }
    wnd = CreateWindowA("static", "qd3d native probe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = wnd;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &dev);
    printf("native_probe: CreateDevice hr %#lx\n", (unsigned long)hr);
    if (dev)
        IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(wnd);
    return SUCCEEDED(hr) ? 0 : 1;
}
