/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Test fixture for run-host-test.ps1: a ddraw.dll that identifies itself the
 * way Wine's, and so WineD3D for Windows', does (wine_common_ver.rc and
 * dlls/ddraw/version.rc in the Wine sources): company "Microsoft
 * Corporation", product "Wine". The installer must treat it as foreign.
 */
#include <windows.h>

BOOL WINAPI wine_ddraw_entry(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reason;
    (void)reserved;
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID *guid, void **dd, IUnknown *outer)
{
    (void)guid;
    (void)outer;
    if (dd)
        *dd = NULL;
    return E_FAIL;
}
