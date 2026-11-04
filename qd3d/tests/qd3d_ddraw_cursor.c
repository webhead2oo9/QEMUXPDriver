/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d_ddraw_cursor - the cursor follows the mouse over a DirectDraw
 * exclusive-mode frame between flips, through the DirectDraw front end
 * (qd3d/ddraw), which presents through the QD3D proxy and so through its
 * cursor poller. Loopback only:
 *
 *   powershell -File qd3d\tests\run-loopback.ps1 -WithDDraw
 *       -Program qd3d\tests\build\qd3d_ddraw_cursor.exe
 *
 * It refuses to run (exit 2) unless the d3d9.dll it loaded answers
 * IID_QD3D_REMOTE, the ddraw.dll it loaded is the front end (QD3D_MARKER
 * resource) and QDDRAW_VIRTUAL_MODES=1: the front end then never touches
 * the real display (virtual modes), where the stock runtime would take the
 * screen. One flip of a solid 800x600x32 exclusive-mode frame with the
 * window's cursor (32x32 opaque white, hot spot 0,0) at a first point, then
 * SetCursorPos to two more points over the window: the harness's repeats
 * of that frame (QD3D_FRAME_DUMPS\frame_N_cK.bmp) show the cursor at each,
 * and no further frame was presented. Leaving exclusive mode releases the
 * front end's Direct3D 9 device: the harness logs its frame source
 * released (source_released.txt).
 *
 * Output: INFO lines, PASS or FAIL lines, a RESULT line. Exit 0 when every
 * check passed, 1 on a failure, 2 when it refused to run.
 */
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "../proto/qd3d_proto.h"

#define MODE_W 800
#define MODE_H 600
#define BG 0x00203040u
#define CUR 0x00ffffffu

static const POINT g_at[3] = { { 100, 100 }, { 400, 300 }, { 700, 500 } };
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
        Sleep(5);
    } while (GetTickCount() - t0 < ms);
}

static void dump_path(char *buf, size_t size, unsigned frame, unsigned k)
{
    if (k)
        snprintf(buf, size, "%s\\frame_%06u_c%03u.bmp", g_dir, frame, k);
    else
        snprintf(buf, size, "%s\\frame_%06u.bmp", g_dir, frame);
}

/* Pixel (x, y) of a complete 32-bit BMP the harness wrote: 1, else 0. */
static int bmp_px(const char *path, LONG x, LONG y, DWORD *px)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "rb");
    long size = 0;
    DWORD v = 0;
    int ok;

    if (!f)
        return 0;
    ok = fread(&fh, sizeof(fh), 1, f) == 1 && fread(&ih, sizeof(ih), 1, f) == 1 && fh.bfType == 0x4d42 &&
         ih.biBitCount == 32 && x >= 0 && y >= 0 && ih.biWidth > x && ih.biHeight > y &&
         fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 &&
         (unsigned long)size >= fh.bfOffBits + (unsigned long)ih.biWidth * (unsigned long)ih.biHeight * 4u &&
         fseek(f, (long)(fh.bfOffBits + ((DWORD)(ih.biHeight - 1 - y) * (DWORD)ih.biWidth + (DWORD)x) * 4u),
               SEEK_SET) == 0 &&
         fread(&v, 4, 1, f) == 1;
    fclose(f);
    if (ok)
        *px = v & 0x00ffffffu;
    return ok;
}

static void check(int ok, const char *what, DWORD got)
{
    if (ok) {
        printf("PASS %s\n", what);
    } else {
        g_failures++;
        printf("FAIL %s (got %06lx)\n", what, (unsigned long)got);
    }
}

int main(void)
{
    char v[8], path[MAX_PATH + 64], line[96];
    BYTE and_bits[32 * 4], xor_bits[32 * 4];
    IDirect3D9 *d3d;
    IUnknown *remote = NULL;
    IDirectDraw *dd = NULL;
    IDirectDrawSurface *prim = NULL, *back = NULL;
    HMODULE ddraw;
    DDSURFACEDESC sd;
    DDSCAPS caps;
    DDBLTFX fx;
    WNDCLASSA wc;
    HCURSOR cur;
    HWND w = NULL;
    POINT saved, now, got[3] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
    DWORD px = 0, a = 0, b = 0, t0;
    unsigned n = 0, k = 0, i, slot, after;
    int found, exclusive = 0;
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ddraw = GetModuleHandleA("ddraw.dll");
    if (GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, sizeof(path)))
        printf("INFO d3d9: %s\n", path);
    if (GetModuleFileNameA(ddraw, path, sizeof(path)))
        printf("INFO ddraw: %s\n", path);
    if (!d3d || FAILED(IDirect3D9_QueryInterface(d3d, &IID_QD3D_REMOTE, (void **)&remote)) || !remote || !ddraw ||
        !FindResourceA(ddraw, "QD3D_MARKER", RT_RCDATA) ||
        GetEnvironmentVariableA("QDDRAW_VIRTUAL_MODES", v, sizeof(v)) != 1 || v[0] != '1' ||
        !GetEnvironmentVariableA("QD3D_FRAME_DUMPS", g_dir, sizeof(g_dir))) {
        printf("SKIP: needs the QD3D proxy, the DirectDraw front end with QDDRAW_VIRTUAL_MODES=1 and frame dumps "
               "(run-loopback.ps1 -WithDDraw); exclusive mode could reach the real display otherwise\n");
        if (remote)
            remote->lpVtbl->Release(remote);
        if (d3d)
            IDirect3D9_Release(d3d);
        return 2;
    }
    remote->lpVtbl->Release(remote);

    memset(and_bits, 0x00, sizeof(and_bits));
    memset(xor_bits, 0xff, sizeof(xor_bits));
    cur = CreateCursor(GetModuleHandleA(NULL), 0, 0, 32, 32, and_bits, xor_bits);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = cur;
    wc.lpszClassName = "qd3d_ddraw_cursor";
    RegisterClassA(&wc);
    w = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "qd3d_ddraw_cursor", WS_POPUP | WS_VISIBLE, 0, 0, MODE_W,
                        MODE_H, NULL, NULL, wc.hInstance, NULL);
    GetCursorPos(&saved);

    hr = w ? DirectDrawCreate(NULL, &dd, NULL) : E_FAIL;
    if (SUCCEEDED(hr))
        hr = IDirectDraw_SetCooperativeLevel(dd, w, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    exclusive = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
        hr = IDirectDraw_SetDisplayMode(dd, MODE_W, MODE_H, 32);
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        sd.dwBackBufferCount = 1;
        hr = IDirectDraw_CreateSurface(dd, &sd, &prim, NULL);
    }
    if (SUCCEEDED(hr)) {
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        hr = IDirectDrawSurface_GetAttachedSurface(prim, &caps, &back);
    }
    if (SUCCEEDED(hr)) {
        memset(&fx, 0, sizeof(fx));
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = BG;
        hr = IDirectDrawSurface_Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    }
    /* The window's cursor at the first point, then one flip. Display
     * scaling may round a requested point: the checks use where the mouse
     * went (the window and the frame are the screen's top-left 800x600). */
    if (SUCCEEDED(hr)) {
        SetCursorPos(g_at[0].x, g_at[0].y);
        GetCursorPos(&got[0]);
        pump(300);
        hr = IDirectDrawSurface_Flip(prim, NULL, DDFLIP_WAIT);
    }
    if (FAILED(hr)) {
        printf("FAIL exclusive %ux%ux32 flip chain: hr 0x%08lx\n", MODE_W, MODE_H, (unsigned long)hr);
        g_failures++;
        goto out;
    }
    /* Frame N: the newest one dumped once the flip is published. */
    pump(500);
    for (i = 1; i <= 8u; i++) {
        dump_path(path, sizeof(path), i, 0);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            n = i;
    }
    printf("INFO frame %u is the flipped frame\n", n);
    dump_path(path, sizeof(path), n, 0);
    check(n && bmp_px(path, MODE_W / 2, MODE_H / 2, &px) && px == BG, "the flip is published (frame dump)", px);
    GetCursorPos(&now);
    if (now.x != got[0].x || now.y != got[0].y) {
        printf("INFO the mouse moved during the test; the cursor is not checked\n");
        goto released;
    }
    check(n && bmp_px(path, got[0].x, got[0].y, &a) && a == CUR && bmp_px(path, got[0].x - 1, got[0].y, &b) &&
              b == BG,
          "the flipped frame shows the cursor at the first point", a);
    for (i = 1; i < 3u && !g_failures; i++) {
        SetCursorPos(g_at[i].x, g_at[i].y);
        GetCursorPos(&got[i]);
        for (found = 0, t0 = GetTickCount(); !found && GetTickCount() - t0 < 3000u;) {
            pump(0);
            dump_path(path, sizeof(path), n, k + 1u);
            if (!bmp_px(path, got[i].x, got[i].y, &a)) {
                Sleep(5);
                continue;
            }
            k++;
            found = a == CUR && bmp_px(path, got[i - 1].x, got[i - 1].y, &b) && b == BG;
        }
        GetCursorPos(&now);
        if (now.x != got[i].x || now.y != got[i].y) {
            printf("INFO the mouse moved during the test; repeats not checked further\n");
            break;
        }
        snprintf(line, sizeof(line), "repeat %u of frame %u shows the cursor at %ld,%ld without a present", k, n,
                 got[i].x, got[i].y);
        check(found, line, a);
    }
released:
    dump_path(path, sizeof(path), n + 1u, 0);
    check(!g_failures && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES,
          "no frame was presented while only the cursor moved", 0);

out:
    if (back)
        IDirectDrawSurface_Release(back);
    if (prim)
        IDirectDrawSurface_Release(prim);
    /* Leaving exclusive mode with no Direct3D device releases the front
     * end's Direct3D 9 device: the host hands the display back at once. */
    if (dd && exclusive) {
        IDirectDraw_RestoreDisplayMode(dd);
        IDirectDraw_SetCooperativeLevel(dd, w, DDSCL_NORMAL);
    }
    if (dd)
        IDirectDraw_Release(dd);
    if (n && !g_failures) {
        snprintf(path, sizeof(path), "%s\\source_released.txt", g_dir);
        for (found = 0, t0 = GetTickCount(); !found && GetTickCount() - t0 < 3000u; Sleep(20)) {
            FILE *f = fopen(path, "r");

            while (f && !found && fgets(line, sizeof(line), f))
                found = sscanf(line, "slot %u after frame %u", &slot, &after) == 2 && after >= n;
            if (f)
                fclose(f);
        }
        check(found, "leaving exclusive mode released the frame source (source_released.txt)", 0);
    }
    SetCursorPos(saved.x, saved.y);
    if (w)
        DestroyWindow(w);
    if (cur)
        DestroyCursor(cur);
    IDirect3D9_Release(d3d);
    printf("RESULT %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
