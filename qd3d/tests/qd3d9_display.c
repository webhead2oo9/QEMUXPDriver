/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d9_display - the display policy end to end (qd3d/host/qd3dx.h): what
 * the VM shows while a game presents only when its picture changes, is
 * minimized or hidden, or dies. Through the loopback harness, which runs
 * the VM device's policy and logs its transitions to
 * QD3D_FRAME_DUMPS\display.txt:
 *
 *   powershell -File qd3d\tests\run-loopback.ps1 -Slots 2
 *       -Program qd3d\tests\build\qd3d9_display.exe
 *
 * 1. Static: one present, then 6.5 s without presenting (past the 5 s after
 *    which the device used to return to the desktop) and without moving
 *    anything: 3D output stays on.
 * 2. Minimized, then restored; hidden, then shown: the desktop at once, the
 *    frame again at once when the window shows, without a present.
 * 3. Killed: a child copy of this program presents on slot 1 and terminates
 *    itself without GOODBYE: the desktop comes back once its heartbeat has
 *    been silent for the idle time (2.5 to 6 s after its death).
 *
 * Output: INFO, PASS and FAIL lines, a RESULT line. Exit 0 when every check
 * passed, 1 on a failure, 2 when it refused to run (no QD3D session or no
 * frame dumps).
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "../proto/qd3d_proto.h"

static char g_dir[MAX_PATH];
static int g_failures;

static void pump(DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG m;

    do {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(10);
    } while (GetTickCount() - t0 < ms);
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        g_failures++;
}

/* Line index (counted from 0) of the first display.txt line from line from
 * on that contains text, waiting up to ms; -1 when none came. */
static int wait_line(int from, const char *text, DWORD ms)
{
    char path[MAX_PATH + 16], line[200];
    DWORD t0 = GetTickCount();

    snprintf(path, sizeof(path), "%s\\display.txt", g_dir);
    for (;;) {
        FILE *f = fopen(path, "r");
        int i = 0, hit = -1;

        while (f && hit < 0 && fgets(line, sizeof(line), f)) {
            if (i >= from && strstr(line, text))
                hit = i;
            i++;
        }
        if (f)
            fclose(f);
        if (hit >= 0 || GetTickCount() - t0 >= ms)
            return hit;
        pump(20);
    }
}

/* Number of lines in display.txt. */
static int lines(void)
{
    char path[MAX_PATH + 16], line[200];
    FILE *f;
    int n = 0;

    snprintf(path, sizeof(path), "%s\\display.txt", g_dir);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f))
        n++;
    if (f)
        fclose(f);
    return n;
}

static IDirect3DDevice9 *device(IDirect3D9 *d3d, HWND w, UINT size)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *dev = NULL;

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = size;
    pp.BackBufferHeight = size;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = w;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev)))
        return NULL;
    return dev;
}

static HWND window(int x, int size)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "qd3d9_display";
    RegisterClassA(&wc);
    return CreateWindowExA(0, wc.lpszClassName, "qd3d9_display", WS_POPUP | WS_VISIBLE, x, 0, size, size, NULL, NULL,
                           wc.hInstance, NULL);
}

/* The child: presents one frame, lets it be published, and dies without
 * GOODBYE (TerminateProcess skips DLL detach). */
static int child(void)
{
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND w = window(300, 128);
    IDirect3DDevice9 *dev = d3d && w ? device(d3d, w, 128) : NULL;

    if (!dev) {
        printf("FAIL child: no device\n");
        return 1;
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x80, 0x20, 0x20), 1.0f, 0);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    pump(1000);
    printf("INFO child: presented, terminating without GOODBYE\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH + 32], cmd[MAX_PATH + 48], what[160];
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    IUnknown *remote = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HWND w = NULL;
    DWORD dead, off_ms;
    int at, mark, i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--child"))
        return child();
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, sizeof(path)))
        printf("INFO d3d9: %s\n", path);
    if (!d3d || FAILED(IDirect3D9_QueryInterface(d3d, &IID_QD3D_REMOTE, (void **)&remote)) || !remote ||
        !GetEnvironmentVariableA("QD3D_FRAME_DUMPS", g_dir, sizeof(g_dir))) {
        printf("SKIP: needs the QD3D proxy and frame dumps (run-loopback.ps1 -Slots 2)\n");
        if (remote)
            remote->lpVtbl->Release(remote);
        if (d3d)
            IDirect3D9_Release(d3d);
        return 2;
    }
    remote->lpVtbl->Release(remote);
    w = window(0, 256);
    dev = w ? device(d3d, w, 256) : NULL;
    if (!dev) {
        printf("FAIL no device\n");
        IDirect3D9_Release(d3d);
        return 1;
    }

    /* 1. Static. */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0x20, 0x40, 0x80), 1.0f, 0);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    at = wait_line(0, "on slot 0 frame 1", 3000);
    check(at >= 0, "the frame turns 3D output on (on slot 0 frame 1)");
    pump(6500);
    check(at >= 0 && wait_line(at + 1, "off", 0) < 0,
          "a static screen stays up for 6.5 s without a present (no off line)");

    /* 2. Minimized and restored, hidden and shown. */
    mark = lines();
    ShowWindow(w, SW_MINIMIZE);
    at = wait_line(mark, "off hidden slot 0", 2000);
    check(at >= 0, "minimized: the desktop at once (off hidden slot 0)");
    ShowWindow(w, SW_RESTORE);
    check(at >= 0 && wait_line(at + 1, "on slot 0 frame 1 repeat", 2000) >= 0,
          "restored: the frame again without a present (on slot 0 frame 1 repeat)");
    mark = lines();
    ShowWindow(w, SW_HIDE);
    at = wait_line(mark, "off hidden slot 0", 2000);
    check(at >= 0, "hidden: the desktop at once (off hidden slot 0)");
    ShowWindow(w, SW_SHOW);
    check(at >= 0 && wait_line(at + 1, "on slot 0 frame 1 repeat", 2000) >= 0,
          "shown: the frame again without a present (on slot 0 frame 1 repeat)");
    snprintf(path, sizeof(path), "%s\\frame_000002.bmp", g_dir);
    check(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "no frame was presented meanwhile");

    /* 3. Killed. */
    mark = lines();
    GetModuleFileNameA(NULL, path, sizeof(path));
    snprintf(cmd, sizeof(cmd), "\"%s\" --child", path);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(path, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL CreateProcess error %lu\n", (unsigned long)GetLastError());
        g_failures++;
    } else {
        for (i = 0; i < 1000 && WaitForSingleObject(pi.hProcess, 10) == WAIT_TIMEOUT; i++)
            pump(0);
        dead = GetTickCount();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        at = wait_line(mark, "on slot 1 frame", 0);
        check(at >= 0, "the child's frame was shown (on slot 1)");
        at = wait_line(at >= 0 ? at + 1 : mark, "off idle slot 1", 8000);
        off_ms = GetTickCount() - dead;
        snprintf(what, sizeof(what), "killed without GOODBYE: the desktop %lu ms after its death (off idle slot 1)",
                 (unsigned long)off_ms);
        check(at >= 0 && off_ms >= 2500u && off_ms <= 6000u, what);
    }

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(w);
    printf("RESULT %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
