/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * One QD3D proxy per process.  Every proxy instance claims a channel slot of
 * its own, so a second one in a process wastes one of the four.  A game
 * whose folder holds a QD3D d3d9.dll loads that copy first; then
 *   ddraw, d3d8: a front end installed elsewhere, with a d3d9.dll proxy
 *                beside it, must reuse the proxy the process already has;
 *   stock:       the game's proxy, without a QD3D session, must fall back to
 *                the stock DLL a system-wide install kept as qd3d9_orig.dll,
 *                never to the system-wide proxy now named d3d9.dll there
 *                (QD3D9_TEST_SYSTEM_DIR names that "System32").
 * Needs no QD3D session: the choices happen before or without one.
 *
 * usage: qd3d_one_proxy <first d3d9.dll> <front-end dll> ddraw|d3d8
 *        qd3d_one_proxy <first d3d9.dll> <system-wide d3d9.dll> stock
 * exit 0: one QD3D proxy; 1: a second one was loaded; 2: bad setup.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT(WINAPI *ddraw_create_fn)(GUID *, IDirectDraw **, IUnknown *);
typedef IDirect3D8 *(WINAPI *d3d8_create_fn)(UINT);
/* d3d9.h cannot share a file with d3d8.h; Release through IUnknown does. */
#define D3D9_SDK_VERSION 32
typedef IUnknown *(WINAPI *d3d9_create_fn)(UINT);

static int is_qd3d(HMODULE m)
{
    return m && FindResourceA(m, "QD3D_MARKER", RT_RCDATA) != NULL;
}

/* dir(path) + leaf into out; 0 when it does not fit or path has no dir. */
static int beside(char *out, size_t cap, const char *path, const char *leaf)
{
    const char *slash = strrchr(path, '\\');
    size_t dir = slash ? (size_t)(slash - path) : 0;

    if (!slash || dir + strlen(leaf) + 1 > cap)
        return 0;
    memcpy(out, path, dir);
    strcpy(out + dir, leaf);
    return 1;
}

static int front_end(const char *kind, const char *dll)
{
    HMODULE front = LoadLibraryA(dll);

    if (!is_qd3d(front)) {
        printf("SETUP: %s is not a QD3D front end (error %lu)\n", dll, GetLastError());
        return 0;
    }
    if (!strcmp(kind, "ddraw")) {
        ddraw_create_fn create = (ddraw_create_fn)(void (*)(void))GetProcAddress(front, "DirectDrawCreate");
        IDirectDraw *dd = NULL;

        if (!create)
            return 0;
        printf("DirectDrawCreate: 0x%08lx\n", (unsigned long)create(NULL, &dd, NULL));
        if (dd)
            IDirectDraw_Release(dd);
    } else {
        d3d8_create_fn create = (d3d8_create_fn)(void (*)(void))GetProcAddress(front, "Direct3DCreate8");
        IDirect3D8 *d3d;

        if (!create)
            return 0;
        d3d = create(D3D_SDK_VERSION);
        printf("Direct3DCreate8: %s\n", d3d ? "object" : "NULL");
        if (d3d)
            IDirect3D8_Release(d3d);
    }
    return 1;
}

int main(int argc, char **argv)
{
    char second[MAX_PATH], orig[MAX_PATH];
    int stock = argc == 4 && !strcmp(argv[3], "stock");
    HMODULE proxy;

    if (argc != 4 || (!stock && strcmp(argv[3], "ddraw") && strcmp(argv[3], "d3d8"))) {
        printf("usage: qd3d_one_proxy <first d3d9.dll> <front-end dll> ddraw|d3d8\n"
               "       qd3d_one_proxy <first d3d9.dll> <system-wide d3d9.dll> stock\n");
        return 2;
    }
    /* The second proxy: the system-wide one, or the front end's sibling. */
    second[0] = 0;
    if (stock) {
        if (strlen(argv[2]) < sizeof second)
            strcpy(second, argv[2]);
    } else if (!beside(second, sizeof second, argv[2], "\\d3d9.dll")) {
        second[0] = 0;
    }
    if (!second[0] || GetFileAttributesA(second) == INVALID_FILE_ATTRIBUTES || !lstrcmpiA(second, argv[1])) {
        printf("SETUP: %s must be a second QD3D d3d9.dll\n", second[0] ? second : argv[2]);
        return 2;
    }
    proxy = LoadLibraryA(argv[1]);
    if (!is_qd3d(proxy)) {
        printf("SETUP: %s is not a QD3D d3d9.dll (error %lu)\n", argv[1], GetLastError());
        return 2;
    }
    if (stock) {
        d3d9_create_fn create = (d3d9_create_fn)(void (*)(void))GetProcAddress(proxy, "Direct3DCreate9");
        IUnknown *d3d;

        if (!beside(orig, sizeof orig, second, "\\qd3d9_orig.dll") ||
            GetFileAttributesA(orig) == INVALID_FILE_ATTRIBUTES || !create) {
            printf("SETUP: no stock qd3d9_orig.dll beside %s\n", second);
            return 2;
        }
        d3d = create(D3D9_SDK_VERSION);
        printf("Direct3DCreate9: %s\n", d3d ? "object" : "NULL");
        if (d3d)
            IUnknown_Release(d3d);
        if (!GetModuleHandleA(orig)) {
            printf("FAIL: the proxy's stock runtime is not %s\n", orig);
            return 1;
        }
    } else if (!front_end(argv[3], argv[2])) {
        return 2;
    }
    if (GetModuleHandleA(second)) {
        printf("FAIL: %s loaded %s: a second QD3D proxy in one process\n", argv[3], second);
        return 1;
    }
    printf("PASS: %s: one QD3D proxy (%s)\n", argv[3], argv[1]);
    return 0;
}
