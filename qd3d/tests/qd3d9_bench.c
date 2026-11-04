/* SPDX-License-Identifier: GPL-2.0-only
 *
 * qd3d9_bench: Direct3D 9 per-draw throughput.  800x600 windowed, one
 * SetRenderState plus one DrawPrimitiveUP quad per draw, 300 measured frames
 * after 30 warm-up frames.  Run it beside the QD3D proxy, or alone for the
 * stock runtime (the tools CD's BENCH.CMD runs both inside XP).
 *
 * Usage: qd3d9_bench [draws-per-frame]   (default 2000)
 * Prints: RESULT frames=N draws/frame=N fps=F us/draw=U
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAMES 300
#define WARMUP 30

struct vtx {
    float x, y, z, rhw;
    DWORD c;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc = {0};
    HWND hw;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    char path[MAX_PATH];
    LARGE_INTEGER f, t0, t1;
    struct vtx q[4];
    int draws = argc > 1 ? atoi(argv[1]) : 2000;
    int fr, i;

    if (draws < 1)
        draws = 1;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "qd3dbench";
    RegisterClassA(&wc);
    hw = CreateWindowA("qd3dbench", "qd3d9_bench", WS_OVERLAPPEDWINDOW, 0, 0, 816, 639, NULL, NULL,
                       wc.hInstance, NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, sizeof path))
        printf("INFO d3d9: %s\n", path);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 800;
    pp.BackBufferHeight = 600;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!d3d || FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hw, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                               &dev))) {
        printf("FAIL device creation\n");
        if (d3d)
            IDirect3D9_Release(d3d);
        return 2;
    }
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (fr = 0; fr < FRAMES + WARMUP; fr++) {
        if (fr == WARMUP)
            QueryPerformanceCounter(&t0);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00102030, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        for (i = 0; i < draws; i++) {
            float x = (float)(i % 50) * 16.0f, y = (float)(i / 50 % 40) * 15.0f;
            DWORD c = 0xff000000u | ((DWORD)i * 2654435761u);
            q[0] = (struct vtx){x, y, 0.5f, 1.0f, c};
            q[1] = (struct vtx){x + 12, y, 0.5f, 1.0f, c};
            q[2] = (struct vtx){x, y + 12, 0.5f, 1.0f, c};
            q[3] = (struct vtx){x + 12, y + 12, 0.5f, 1.0f, c};
            IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, (i & 1) ? D3DCULL_NONE : D3DCULL_CCW);
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        }
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }
    QueryPerformanceCounter(&t1);
    {
        double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        printf("RESULT frames=%d draws/frame=%d fps=%.1f us/draw=%.3f\n", FRAMES, draws, FRAMES / s,
               s * 1e6 / ((double)FRAMES * draws));
    }
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(hw);
    return 0;
}
