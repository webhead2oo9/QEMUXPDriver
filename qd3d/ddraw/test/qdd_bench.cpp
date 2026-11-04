/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd_bench: frame-rate measurements of 2D DirectDraw workloads, for the
 * stock runtime and the front end (qd3d/tests/run-loopback.ps1 -WithDDraw).
 *
 *   qdd_bench [--seconds N] [--exclusive] [--with-3d] [--primary-dc]
 *             [--palette N] [--text N] [--sprites N] [--vblank] [--gamma]
 *
 * The "menu" frame mimics a 2D game's menu screen (Age of Empires II
 * style): background Blt, color-keyed sprite Blts, GDI text through
 * GetDC/TextOut/ReleaseDC on the back buffer, a software cursor drawn
 * through Lock/Unlock, optional palette changes (SetEntries) and GDI
 * drawing on the primary, then Flip (DDFLIP_WAIT) or, windowed, a Blt to
 * the primary.
 *
 * --exclusive uses an 800x600x8 flip chain in exclusive mode.  It refuses
 * to run unless the loaded ddraw.dll is the front end and
 * QDDRAW_VIRTUAL_MODES=1 (the real display mode never changes); the stock
 * runtime only runs the windowed variant (desktop format).  --with-3d
 * creates a Direct3D 7 device first (the front end then presents through
 * QD3D even windowed).  Prints frames per second and the time per frame
 * spent in each phase (Measured on the machine it runs on).
 */
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LONGLONG now_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static LONGLONG g_freq;

static double ms(LONGLONG t)
{
    return (double)t * 1000.0 / (double)g_freq;
}

static bool front_end_loaded(void)
{
    HMODULE m = GetModuleHandleA("ddraw.dll");
    return m && FindResourceA(m, "QD3D_MARKER", RT_RCDATA);
}

enum { PH_BG, PH_SPRITES, PH_TEXT, PH_CURSOR, PH_PALETTE, PH_PRIMARY_DC, PH_PRESENT, PH_COUNT };
static const char *const g_phase[PH_COUNT] = {"background", "sprites", "text", "cursor", "palette", "primary-dc",
                                              "present"};

int main(int argc, char **argv)
{
    double seconds = 3.0;
    bool exclusive = false, with3d = false, primary_dc = false, vblank = false, gamma = false;
    int palette_calls = 0, text_calls = 10, sprites = 20;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--exclusive"))
            exclusive = true;
        else if (!strcmp(argv[i], "--with-3d"))
            with3d = true;
        else if (!strcmp(argv[i], "--primary-dc"))
            primary_dc = true;
        else if (!strcmp(argv[i], "--gamma"))
            gamma = true;
        else if (!strcmp(argv[i], "--vblank"))
            vblank = true;
        else if (!strcmp(argv[i], "--palette") && i + 1 < argc)
            palette_calls = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--text") && i + 1 < argc)
            text_calls = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sprites") && i + 1 < argc)
            sprites = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: qdd_bench [--seconds N] [--exclusive] [--with-3d] [--primary-dc] [--gamma] [--palette N]"
                            " [--text N] [--sprites N] [--vblank]\n");
            return 2;
        }
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;

    const int W = 800, H = 600;
    HWND win = CreateWindowExA(0, "static", "qdd_bench", WS_POPUP | WS_VISIBLE, 0, 0, W, H, NULL, NULL, NULL, NULL);
    IDirectDraw7 *dd = NULL;
    if (FAILED(DirectDrawCreateEx(NULL, (void **)&dd, IID_IDirectDraw7, NULL))) {
        printf("FAIL: DirectDrawCreateEx\n");
        return 1;
    }
    bool ours = front_end_loaded();
    printf("ddraw.dll: %s\n", ours ? "front end" : "stock");
    if (exclusive) {
        char v[8];
        DWORD n = GetEnvironmentVariableA("QDDRAW_VIRTUAL_MODES", v, sizeof(v));
        if (!ours || !(n > 0 && n < sizeof(v) && v[0] == '1')) {
            printf("SKIP: --exclusive needs the front end with QDDRAW_VIRTUAL_MODES=1 (never the real display)\n");
            return 0;
        }
    }
    HRESULT hr;
    IDirectDrawSurface7 *primary = NULL, *back = NULL;
    DDSURFACEDESC2 d;
    if (exclusive) {
        hr = dd->SetCooperativeLevel(win, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        if (SUCCEEDED(hr))
            hr = dd->SetDisplayMode(W, H, 8, 0, 0);
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        d.dwBackBufferCount = 1;
        if (SUCCEEDED(hr))
            hr = dd->CreateSurface(&d, &primary, NULL);
        DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
        if (SUCCEEDED(hr))
            hr = primary->GetAttachedSurface(&caps, &back);
    } else {
        hr = dd->SetCooperativeLevel(win, DDSCL_NORMAL);
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (SUCCEEDED(hr))
            hr = dd->CreateSurface(&d, &primary, NULL);
        IDirectDrawClipper *clip = NULL;
        if (SUCCEEDED(hr) && SUCCEEDED(dd->CreateClipper(0, &clip, NULL))) {
            clip->SetHWnd(0, win);
            primary->SetClipper(clip);
            clip->Release();
        }
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        d.dwWidth = W;
        d.dwHeight = H;
        if (SUCCEEDED(hr))
            hr = dd->CreateSurface(&d, &back, NULL);
    }
    if (FAILED(hr)) {
        printf("FAIL: surface setup hr 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    IDirectDrawPalette *pal = NULL;
    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; ++i) {
        pe[i].peRed = (BYTE)i;
        pe[i].peGreen = (BYTE)(255 - i);
        pe[i].peBlue = (BYTE)(i * 7);
        pe[i].peFlags = 0;
    }
    if (exclusive && SUCCEEDED(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL)))
        primary->SetPalette(pal);
    if (gamma) {
        /* An inverting ramp: presented frames show 255 - each channel. */
        IDirectDrawGammaControl *gc = NULL;
        if (SUCCEEDED(primary->QueryInterface(IID_IDirectDrawGammaControl, (void **)&gc))) {
            DDGAMMARAMP ramp;
            for (int i = 0; i < 256; ++i)
                ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)((255 - i) * 257);
            printf("gamma: SetGammaRamp hr 0x%08lx\n", (unsigned long)gc->SetGammaRamp(0, &ramp));
            gc->Release();
        }
    }

    IDirect3D7 *d3d = NULL;
    IDirect3DDevice7 *dev = NULL;
    IDirectDrawSurface7 *rt3d = NULL;
    if (with3d) {
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
        d.dwWidth = 64;
        d.dwHeight = 64;
        if (FAILED(dd->QueryInterface(IID_IDirect3D7, (void **)&d3d)) || FAILED(dd->CreateSurface(&d, &rt3d, NULL)) ||
            FAILED(d3d->CreateDevice(IID_IDirect3DHALDevice, rt3d, &dev)))
            printf("note: no Direct3D 7 device (--with-3d ignored)\n");
    }

    /* Content: a background and a color-keyed sprite in the back buffer's format. */
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    back->GetSurfaceDesc(&d);
    DDPIXELFORMAT pf = d.ddpfPixelFormat;
    IDirectDrawSurface7 *bg = NULL, *sprite = NULL;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    d.ddpfPixelFormat = pf;
    d.dwWidth = W;
    d.dwHeight = H;
    if (FAILED(dd->CreateSurface(&d, &bg, NULL))) {
        printf("FAIL: background surface\n");
        return 1;
    }
    d.dwWidth = d.dwHeight = 64;
    if (FAILED(dd->CreateSurface(&d, &sprite, NULL))) {
        printf("FAIL: sprite surface\n");
        return 1;
    }
    DDBLTFX fx;
    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x12;
    bg->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    fx.dwFillColor = 0;
    sprite->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    RECT inner = {16, 16, 48, 48};
    fx.dwFillColor = 0x3c;
    sprite->Blt(&inner, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    DDCOLORKEY ck = {0, 0};
    sprite->SetColorKey(DDCKEY_SRCBLT, &ck);

    LONGLONG phase[PH_COUNT] = {0};
    LONGLONG start = now_ticks(), end = start + (LONGLONG)(seconds * (double)g_freq);
    int frames = 0;
    POINT origin = {0, 0};
    if (!exclusive)
        ClientToScreen(win, &origin);
    while (now_ticks() < end) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        LONGLONG t = now_ticks(), t1;
        back->Blt(NULL, bg, NULL, DDBLT_WAIT, NULL);
        t1 = now_ticks();
        phase[PH_BG] += t1 - t;
        t = t1;
        for (int i = 0; i < sprites; ++i) {
            RECT r = {(i * 37 + frames * 3) % (W - 64), (i * 53) % (H - 64), 0, 0};
            r.right = r.left + 64;
            r.bottom = r.top + 64;
            back->Blt(&r, sprite, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
        }
        t1 = now_ticks();
        phase[PH_SPRITES] += t1 - t;
        t = t1;
        for (int i = 0; i < text_calls; ++i) {
            HDC dc;
            if (SUCCEEDED(back->GetDC(&dc))) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 0));
                char s[64];
                int n = _snprintf(s, sizeof(s), "Menu item %d, frame %d", i, frames);
                TextOutA(dc, 40, 40 + i * 20, s, n);
                back->ReleaseDC(dc);
            }
        }
        t1 = now_ticks();
        phase[PH_TEXT] += t1 - t;
        t = t1;
        RECT cr = {(frames * 5) % (W - 32), (frames * 3) % (H - 32), 0, 0};
        cr.right = cr.left + 32;
        cr.bottom = cr.top + 32;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (SUCCEEDED(back->Lock(&cr, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL))) {
            DWORD bytes = d.ddpfPixelFormat.dwRGBBitCount / 8;
            for (int y = 0; y < 32; ++y)
                memset((BYTE *)d.lpSurface + y * d.lPitch, 0xff, (size_t)(32 - y) * bytes);
            back->Unlock(&cr);
        }
        t1 = now_ticks();
        phase[PH_CURSOR] += t1 - t;
        t = t1;
        if (pal) {
            for (int i = 0; i < palette_calls; ++i) {
                PALETTEENTRY e = {(BYTE)(frames + i), 0, 0, 0};
                pal->SetEntries(0, 200 + (DWORD)(i & 31), 1, &e);
            }
        }
        t1 = now_ticks();
        phase[PH_PALETTE] += t1 - t;
        t = t1;
        if (primary_dc) {
            HDC dc;
            if (SUCCEEDED(primary->GetDC(&dc))) {
                TextOutA(dc, origin.x + 600, origin.y + 10, "status", 6);
                primary->ReleaseDC(dc);
            }
        }
        t1 = now_ticks();
        phase[PH_PRIMARY_DC] += t1 - t;
        t = t1;
        if (vblank)
            dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, NULL);
        if (exclusive) {
            primary->Flip(NULL, DDFLIP_WAIT);
        } else {
            RECT dr = {origin.x, origin.y, origin.x + W, origin.y + H};
            primary->Blt(&dr, back, NULL, DDBLT_WAIT, NULL);
        }
        t1 = now_ticks();
        phase[PH_PRESENT] += t1 - t;
        ++frames;
    }
    double total = ms(now_ticks() - start);
    printf("frames %d in %.0f ms: %.1f fps (Measured)\n", frames, total, frames * 1000.0 / total);
    printf("per frame:");
    for (int i = 0; i < PH_COUNT; ++i)
        printf(" %s %.2f ms", g_phase[i], frames ? ms(phase[i]) / frames : 0.0);
    printf("\n");
    if (dev)
        dev->Release();
    if (rt3d)
        rt3d->Release();
    if (d3d)
        d3d->Release();
    sprite->Release();
    bg->Release();
    if (pal)
        pal->Release();
    if (!exclusive)
        back->Release();
    primary->Release();
    if (exclusive)
        dd->RestoreDisplayMode();
    dd->Release();
    DestroyWindow(win);
    return 0;
}
