#define COBJMACROS
#include <winsock2.h>
#include <windows.h>

#define DIRECT3D_VERSION 0x0700
#include <ddrawi.h>
#include <d3d.h>
#include <d3dhal.h>
#include <ddrawgdi.h>

#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

typedef BOOL (WINAPI *PFN_DD_CREATE)(LPDDRAWI_DIRECTDRAW_GBL, HDC);
typedef BOOL (WINAPI *PFN_DD_QUERY)(
    LPDDRAWI_DIRECTDRAW_GBL, LPDDHALINFO, LPDDHAL_DDCALLBACKS,
    LPDDHAL_DDSURFACECALLBACKS, LPDDHAL_DDPALETTECALLBACKS,
    LPD3DHAL_CALLBACKS, LPD3DHAL_GLOBALDRIVERDATA,
    LPDDHAL_DDEXEBUFCALLBACKS, LPDDSURFACEDESC, LPDWORD, LPVIDMEM);
typedef BOOL (WINAPI *PFN_DD_DELETE)(LPDDRAWI_DIRECTDRAW_GBL);

typedef union gdi_entry {
    FARPROC proc;
    PFN_DD_CREATE create;
    PFN_DD_QUERY query;
    PFN_DD_DELETE delete_object;
} GDI_ENTRY;

typedef struct qxp_getdriverinfo2_data {
    DWORD reserved;
    DWORD magic;
    DWORD type;
    DWORD expected_size;
} QXP_GETDRIVERINFO2_DATA;

static FILE *query_log;
static const char *video_blt_stage = "not-started";
static DWORD video_blt_caps;
static DWORD video_blt_vid_total;
static DWORD video_blt_vid_free;
static const IID qxp_iid_directdraw7 = {
    0x15e65ec0, 0x3b9c, 0x11d2,
    { 0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b }
};
static const IID qxp_iid_direct3d7 = {
    0xf5049e77, 0x4861, 0x11d2,
    { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 }
};
static const IID qxp_iid_direct3d_hal_device = {
    0x84e63de0, 0x46aa, 0x11cf,
    { 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e }
};
static const GUID qxp_guid_zpixel_formats = {
    0x93869880, 0x36cf, 0x11d1,
    { 0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae }
};

static BOOL bytes_are_zero(const void *data, size_t bytes)
{
    const BYTE *cursor = (const BYTE *)data;

    while (bytes--) {
        if (*cursor++) return FALSE;
    }
    return TRUE;
}

typedef struct qxp_surface_sample {
    HRESULT rval;
    LONG pitch;
    DWORD colored;
    DWORD white;
    DWORD left;
    DWORD top;
    DWORD right;
    DWORD bottom;
} QXP_SURFACE_SAMPLE;

static BOOL sample_surface32(IDirectDrawSurface *surface,
                             QXP_SURFACE_SAMPLE *sample,
                             DWORD expected_left, DWORD expected_top)
{
    DDSURFACEDESC locked;
    DWORD x;
    DWORD y;
    DWORD color;
    DWORD expected_white = 0;
    DWORD *row;

    memset(sample, 0, sizeof(*sample));
    sample->left = 0xffffffffUL;
    sample->top = 0xffffffffUL;
    memset(&locked, 0, sizeof(locked));
    locked.dwSize = sizeof(locked);
    sample->rval = IDirectDrawSurface_Lock(
        surface, NULL, &locked, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(sample->rval)) return FALSE;
    sample->pitch = locked.lPitch;
    if (locked.ddpfPixelFormat.dwRGBBitCount != 32 ||
        locked.dwWidth != 640 || locked.dwHeight != 480) {
        IDirectDrawSurface_Unlock(surface, NULL);
        sample->rval = E_FAIL;
        return FALSE;
    }
    for (y = 0; y < locked.dwHeight; ++y) {
        row = (DWORD *)((BYTE *)locked.lpSurface + y * locked.lPitch);
        for (x = 0; x < locked.dwWidth; ++x) {
            color = row[x] & 0x00ffffffUL;
            if (!color) continue;
            ++sample->colored;
            if (color == 0x00ffffffUL) {
                ++sample->white;
                if (x >= expected_left && x < expected_left + 100 &&
                    y >= expected_top && y < expected_top + 100)
                    ++expected_white;
            }
            if (x < sample->left) sample->left = x;
            if (y < sample->top) sample->top = y;
            if (x + 1 > sample->right) sample->right = x + 1;
            if (y + 1 > sample->bottom) sample->bottom = y + 1;
        }
    }
    if (FAILED(IDirectDrawSurface_Unlock(surface, NULL))) {
        sample->rval = E_FAIL;
        return FALSE;
    }
    return expected_white == 10000 && sample->white >= 10000 &&
           sample->colored >= sample->white &&
           sample->colored - sample->white <= 1024;
}

static HRESULT run_fullscreen_flip(char *detail, size_t detail_bytes)
{
    HWND window = NULL;
    IDirectDraw *direct_draw = NULL;
    IDirectDrawSurface *primary = NULL;
    IDirectDrawSurface *backbuffer = NULL;
    DDSURFACEDESC surface_desc;
    DDSCAPS backbuffer_caps;
    DDBLTFX fill;
    QXP_SURFACE_SAMPLE back_sample;
    QXP_SURFACE_SAMPLE primary_sample;
    HDC dc = NULL;
    RECT square;
    HRESULT hr;
    BOOL exclusive = FALSE;
    BOOL cursor_hidden = FALSE;
    BOOL back_ok = FALSE;
    BOOL primary_ok = FALSE;
    DWORD frame;
    LONG square_x = 0;
    LONG square_y = 10;
    LONG x_gain = 2;
    LONG y_gain = 2;
    const char *stage = "window";

    memset(&back_sample, 0, sizeof(back_sample));
    memset(&primary_sample, 0, sizeof(primary_sample));
    window = CreateWindowExA(0, "STATIC", "QXP DirectDraw flip probe",
                             WS_POPUP, 0, 0, 640, 480, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    if (!window) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    ShowCursor(FALSE);
    cursor_hidden = TRUE;
    stage = "create";
    hr = DirectDrawCreate(NULL, &direct_draw, NULL);
    if (FAILED(hr)) goto done;
    stage = "cooperative-level";
    hr = IDirectDraw_SetCooperativeLevel(
        direct_draw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (FAILED(hr)) goto done;
    exclusive = TRUE;
    stage = "display-mode";
    hr = IDirectDraw_SetDisplayMode(direct_draw, 640, 480, 32);
    if (FAILED(hr)) goto done;
    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                                  DDSCAPS_COMPLEX;
    surface_desc.dwBackBufferCount = 1;
    stage = "primary";
    hr = IDirectDraw_CreateSurface(
        direct_draw, &surface_desc, &primary, NULL);
    if (FAILED(hr)) goto done;
    memset(&backbuffer_caps, 0, sizeof(backbuffer_caps));
    backbuffer_caps.dwCaps = DDSCAPS_BACKBUFFER;
    stage = "backbuffer";
    hr = IDirectDrawSurface_GetAttachedSurface(
        primary, &backbuffer_caps, &backbuffer);
    if (FAILED(hr)) goto done;
    memset(&fill, 0, sizeof(fill));
    fill.dwSize = sizeof(fill);
    fill.dwFillColor = 0;
    for (frame = 0; frame < 32; ++frame) {
        stage = "clear";
        hr = IDirectDrawSurface_Blt(
            backbuffer, NULL, NULL, NULL,
            DDBLT_COLORFILL | DDBLT_WAIT, &fill);
        if (FAILED(hr)) goto done;
        stage = "get-dc";
        hr = IDirectDrawSurface_GetDC(backbuffer, &dc);
        if (FAILED(hr)) goto done;
        square.left = square_x;
        square.top = square_y;
        square.right = square_x + 100;
        square.bottom = square_y + 100;
        if (!FillRect(dc, &square, (HBRUSH)GetStockObject(WHITE_BRUSH))) {
            hr = E_FAIL;
            goto done;
        }
        stage = "release-dc";
        hr = IDirectDrawSurface_ReleaseDC(backbuffer, dc);
        dc = NULL;
        if (FAILED(hr)) goto done;
        if (!frame) {
            stage = "sample-backbuffer";
            back_ok = sample_surface32(
                backbuffer, &back_sample, square_x, square_y);
            if (!back_ok) {
                hr = back_sample.rval;
                if (SUCCEEDED(hr)) hr = E_FAIL;
                goto done;
            }
        }
        stage = "flip";
        hr = IDirectDrawSurface_Flip(primary, NULL, DDFLIP_WAIT);
        if (FAILED(hr)) goto done;
        if (square_x >= 540) x_gain = -x_gain;
        if (square_y >= 380) y_gain = -y_gain;
        if (square_x < 0) x_gain = -x_gain;
        if (square_y < 0) y_gain = -y_gain;
        if (frame + 1 < 32) {
            square_x += x_gain;
            square_y += y_gain;
        }
    }
    stage = "sample-primary";
    primary_ok = sample_surface32(
        primary, &primary_sample, square_x, square_y);
    if (!primary_ok) {
        hr = primary_sample.rval;
        if (SUCCEEDED(hr)) hr = E_FAIL;
        goto done;
    }
    stage = "complete";
    hr = S_OK;

done:
    if (dc && backbuffer) IDirectDrawSurface_ReleaseDC(backbuffer, dc);
    _snprintf(detail, detail_bytes,
              "fullscreen-flip stage=%s rval=%08lx "
              "back=%u,%ld,%lu,%lu,%lu,%lu,%lu,%lu "
              "primary=%u,%ld,%lu,%lu,%lu,%lu,%lu,%lu",
              stage, (unsigned long)hr,
              back_ok, back_sample.pitch,
              (unsigned long)back_sample.colored,
              (unsigned long)back_sample.white,
              (unsigned long)back_sample.left,
              (unsigned long)back_sample.top,
              (unsigned long)back_sample.right,
              (unsigned long)back_sample.bottom,
              primary_ok, primary_sample.pitch,
              (unsigned long)primary_sample.colored,
              (unsigned long)primary_sample.white,
              (unsigned long)primary_sample.left,
              (unsigned long)primary_sample.top,
              (unsigned long)primary_sample.right,
              (unsigned long)primary_sample.bottom);
    if (detail_bytes) detail[detail_bytes - 1] = '\0';
    if (backbuffer) IDirectDrawSurface_Release(backbuffer);
    if (primary) IDirectDrawSurface_Release(primary);
    if (direct_draw) {
        if (exclusive) IDirectDraw_RestoreDisplayMode(direct_draw);
        IDirectDraw_Release(direct_draw);
    }
    if (window) DestroyWindow(window);
    if (cursor_hidden) ShowCursor(TRUE);
    return hr;
}

#define QXP_GDI_LEFT 300
#define QXP_GDI_TOP 100
#define QXP_GDI_SIZE 16

static BOOL sample_surface8(IDirectDrawSurface *surface,
                            QXP_SURFACE_SAMPLE *sample,
                            BYTE expected_background,
                            const BYTE *expected_gdi,
                            BYTE *captured_gdi,
                            DWORD *gdi_nonzero)
{
    DDSURFACEDESC locked;
    DWORD x;
    DWORD y;
    DWORD expected_value = 0;
    DWORD gdi_index;
    BOOL matches = TRUE;
    BYTE value;
    BYTE *row;

    memset(sample, 0, sizeof(*sample));
    sample->left = 0xffffffffUL;
    sample->top = 0xffffffffUL;
    if (gdi_nonzero) *gdi_nonzero = 0;
    memset(&locked, 0, sizeof(locked));
    locked.dwSize = sizeof(locked);
    sample->rval = IDirectDrawSurface_Lock(
        surface, NULL, &locked, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(sample->rval)) return FALSE;
    sample->pitch = locked.lPitch;
    if (locked.ddpfPixelFormat.dwRGBBitCount != 8 ||
        !(locked.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) ||
        locked.dwWidth != 800 || locked.dwHeight != 600) {
        IDirectDrawSurface_Unlock(surface, NULL);
        sample->rval = E_FAIL;
        return FALSE;
    }
    for (y = 0; y < locked.dwHeight; ++y) {
        row = (BYTE *)locked.lpSurface + y * locked.lPitch;
        for (x = 0; x < locked.dwWidth; ++x) {
            value = row[x];
            if (x >= 100 && x < 200 && y >= 100 && y < 200) {
                if (value == 42) ++expected_value;
                else matches = FALSE;
            } else if (x >= QXP_GDI_LEFT &&
                       x < QXP_GDI_LEFT + QXP_GDI_SIZE &&
                       y >= QXP_GDI_TOP &&
                       y < QXP_GDI_TOP + QXP_GDI_SIZE) {
                gdi_index = (y - QXP_GDI_TOP) * QXP_GDI_SIZE +
                            x - QXP_GDI_LEFT;
                if (captured_gdi) captured_gdi[gdi_index] = value;
                if (value && gdi_nonzero) ++*gdi_nonzero;
                if (expected_gdi && value != expected_gdi[gdi_index])
                    matches = FALSE;
            } else if (value != expected_background) {
                matches = FALSE;
            }
            if (!value) continue;
            ++sample->colored;
            if (value == 42) ++sample->white;
            if (x < sample->left) sample->left = x;
            if (y < sample->top) sample->top = y;
            if (x + 1 > sample->right) sample->right = x + 1;
            if (y + 1 > sample->bottom) sample->bottom = y + 1;
        }
    }
    if (FAILED(IDirectDrawSurface_Unlock(surface, NULL))) {
        sample->rval = E_FAIL;
        return FALSE;
    }
    return matches && expected_value == 10000 && gdi_nonzero &&
           *gdi_nonzero == QXP_GDI_SIZE * QXP_GDI_SIZE;
}

static HRESULT run_paletted_flip(char *detail, size_t detail_bytes)
{
    HWND window = NULL;
    IDirectDraw *direct_draw = NULL;
    IDirectDrawSurface *primary = NULL;
    IDirectDrawSurface *backbuffer = NULL;
    IDirectDrawPalette *palette = NULL;
    DDSURFACEDESC surface_desc;
    DDSURFACEDESC display_mode;
    DDSURFACEDESC locked;
    DDCAPS driver_caps;
    DDCAPS hel_caps;
    DDCOLORKEY source_key;
    DDBLTFX fill;
    PALETTEENTRY entries[256];
    QXP_SURFACE_SAMPLE back_sample;
    QXP_SURFACE_SAMPLE primary_sample;
    BYTE gdi_pixels[QXP_GDI_SIZE * QXP_GDI_SIZE];
    BYTE *row;
    HDC dc = NULL;
    HDC memory_dc = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ old_bitmap = NULL;
    DWORD back_gdi_nonzero = 0;
    DWORD primary_gdi_nonzero = 0;
    DWORD y;
    COLORREF palette_rgb = CLR_INVALID;
    HRESULT hr;
    BOOL exclusive = FALSE;
    BOOL cursor_hidden = FALSE;
    BOOL back_ok = FALSE;
    BOOL primary_ok = FALSE;
    const char *stage = "window";

    memset(&back_sample, 0, sizeof(back_sample));
    memset(&primary_sample, 0, sizeof(primary_sample));
    memset(&driver_caps, 0, sizeof(driver_caps));
    memset(&hel_caps, 0, sizeof(hel_caps));
    memset(entries, 0, sizeof(entries));
    entries[42].peRed = 0x20;
    entries[42].peGreen = 0xd0;
    entries[42].peBlue = 0x40;
    window = CreateWindowExA(0, "STATIC", "QXP 8-bit DirectDraw probe",
                             WS_POPUP, 0, 0, 800, 600, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    if (!window) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    ShowCursor(FALSE);
    cursor_hidden = TRUE;
    stage = "create";
    hr = DirectDrawCreate(NULL, &direct_draw, NULL);
    if (FAILED(hr)) goto done;
    stage = "cooperative-level";
    hr = IDirectDraw_SetCooperativeLevel(
        direct_draw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (FAILED(hr)) goto done;
    exclusive = TRUE;
    stage = "display-mode";
    hr = IDirectDraw_SetDisplayMode(direct_draw, 800, 600, 8);
    if (FAILED(hr)) goto done;
    memset(&display_mode, 0, sizeof(display_mode));
    display_mode.dwSize = sizeof(display_mode);
    stage = "get-display-mode";
    hr = IDirectDraw_GetDisplayMode(direct_draw, &display_mode);
    if (FAILED(hr) || display_mode.ddpfPixelFormat.dwRGBBitCount != 8 ||
        (display_mode.ddpfPixelFormat.dwFlags &
         (DDPF_RGB | DDPF_PALETTEINDEXED8)) !=
            (DDPF_RGB | DDPF_PALETTEINDEXED8) ||
        display_mode.ddpfPixelFormat.dwRBitMask ||
        display_mode.ddpfPixelFormat.dwGBitMask ||
        display_mode.ddpfPixelFormat.dwBBitMask) {
        if (SUCCEEDED(hr)) hr = E_FAIL;
        goto done;
    }
    memset(&driver_caps, 0, sizeof(driver_caps));
    memset(&hel_caps, 0, sizeof(hel_caps));
    driver_caps.dwSize = sizeof(driver_caps);
    hel_caps.dwSize = sizeof(hel_caps);
    stage = "caps";
    hr = IDirectDraw_GetCaps(direct_draw, &driver_caps, &hel_caps);
    if (FAILED(hr)) goto done;
    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE |
                                  DDSCAPS_VIDEOMEMORY;
    stage = "primary";
    hr = IDirectDraw_CreateSurface(
        direct_draw, &surface_desc, &primary, NULL);
    if (FAILED(hr)) goto done;
    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.dwWidth = 800;
    surface_desc.dwHeight = 600;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN |
                                  DDSCAPS_VIDEOMEMORY;
    stage = "offscreen";
    hr = IDirectDraw_CreateSurface(
        direct_draw, &surface_desc, &backbuffer, NULL);
    if (FAILED(hr)) goto done;
    stage = "palette";
    hr = IDirectDraw_CreatePalette(
        direct_draw, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
        entries, &palette, NULL);
    if (FAILED(hr)) goto done;
    stage = "set-palette";
    hr = IDirectDrawSurface_SetPalette(primary, palette);
    if (FAILED(hr)) goto done;
    stage = "get-dc-offscreen";
    hr = IDirectDrawSurface_GetDC(backbuffer, &dc);
    if (FAILED(hr)) goto done;
    stage = "create-compatible-dc";
    memory_dc = CreateCompatibleDC(dc);
    bitmap = CreateCompatibleBitmap(dc, QXP_GDI_SIZE, QXP_GDI_SIZE);
    if (!memory_dc || !bitmap) {
        hr = E_FAIL;
        goto done;
    }
    old_bitmap = SelectObject(memory_dc, bitmap);
    stage = "gdi-source-fill";
    if (!old_bitmap || !PatBlt(memory_dc, 0, 0, QXP_GDI_SIZE,
                               QXP_GDI_SIZE, WHITENESS)) {
        hr = E_FAIL;
        goto done;
    }
    stage = "gdi-bitblt";
    if (!BitBlt(dc, QXP_GDI_LEFT, QXP_GDI_TOP, QXP_GDI_SIZE,
                QXP_GDI_SIZE, memory_dc, 0, 0, SRCCOPY)) {
        hr = E_FAIL;
        goto done;
    }
    SelectObject(memory_dc, old_bitmap);
    old_bitmap = NULL;
    DeleteObject(bitmap);
    bitmap = NULL;
    DeleteDC(memory_dc);
    memory_dc = NULL;
    stage = "release-dc-offscreen";
    hr = IDirectDrawSurface_ReleaseDC(backbuffer, dc);
    dc = NULL;
    if (FAILED(hr)) goto done;
    memset(&locked, 0, sizeof(locked));
    locked.dwSize = sizeof(locked);
    stage = "lock-backbuffer";
    hr = IDirectDrawSurface_Lock(
        backbuffer, NULL, &locked, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) goto done;
    for (y = 0; y < locked.dwHeight; ++y) {
        row = (BYTE *)locked.lpSurface + y * locked.lPitch;
        if (y >= QXP_GDI_TOP && y < QXP_GDI_TOP + QXP_GDI_SIZE) {
            memset(row, 0, QXP_GDI_LEFT);
            memset(row + QXP_GDI_LEFT + QXP_GDI_SIZE, 0,
                   locked.dwWidth - QXP_GDI_LEFT - QXP_GDI_SIZE);
        } else {
            memset(row, 0, locked.dwWidth);
        }
        if (y >= 100 && y < 200) memset(row + 100, 42, 100);
    }
    stage = "unlock-backbuffer";
    hr = IDirectDrawSurface_Unlock(backbuffer, NULL);
    if (FAILED(hr)) goto done;
    stage = "sample-backbuffer";
    back_ok = sample_surface8(backbuffer, &back_sample, 0, NULL,
                              gdi_pixels, &back_gdi_nonzero);
    if (!back_ok) {
        hr = back_sample.rval;
        if (SUCCEEDED(hr)) hr = E_FAIL;
        goto done;
    }
    memset(&fill, 0, sizeof(fill));
    fill.dwSize = sizeof(fill);
    fill.dwFillColor = 7;
    stage = "fill-primary-sentinel";
    hr = IDirectDrawSurface_Blt(primary, NULL, NULL, NULL,
                                DDBLT_COLORFILL | DDBLT_WAIT, &fill);
    if (FAILED(hr)) goto done;
    source_key.dwColorSpaceLowValue = 0;
    source_key.dwColorSpaceHighValue = 0;
    stage = "source-color-key";
    hr = IDirectDrawSurface_SetColorKey(
        backbuffer, DDCKEY_SRCBLT, &source_key);
    if (FAILED(hr)) goto done;
    stage = "blt-primary";
    hr = IDirectDrawSurface_Blt(
        primary, NULL, backbuffer, NULL,
        DDBLT_WAIT | DDBLT_KEYSRC, NULL);
    if (FAILED(hr)) goto done;
    stage = "sample-primary";
    primary_ok = sample_surface8(primary, &primary_sample, 7, gdi_pixels,
                                 NULL, &primary_gdi_nonzero);
    if (!primary_ok) {
        hr = primary_sample.rval;
        if (SUCCEEDED(hr)) hr = E_FAIL;
        goto done;
    }
    stage = "palette-rgb";
    hr = IDirectDrawSurface_GetDC(primary, &dc);
    if (FAILED(hr)) goto done;
    palette_rgb = GetPixel(dc, 100, 100);
    hr = IDirectDrawSurface_ReleaseDC(primary, dc);
    dc = NULL;
    if (FAILED(hr)) goto done;
    if (palette_rgb != RGB(0x20, 0xd0, 0x40)) {
        hr = E_FAIL;
        goto done;
    }
    stage = "complete";
    hr = S_OK;

done:
    _snprintf(detail, detail_bytes,
              "paletted-offscreen stage=%s rval=%08lx palcaps=%08lx "
              "gdi=%lu,%lu rgb=%08lx "
              "back=%u,%ld,%lu,%lu,%lu,%lu,%lu,%lu "
              "primary=%u,%ld,%lu,%lu,%lu,%lu,%lu,%lu",
              stage, (unsigned long)hr,
              (unsigned long)driver_caps.dwPalCaps,
              (unsigned long)back_gdi_nonzero,
              (unsigned long)primary_gdi_nonzero,
              (unsigned long)palette_rgb,
              back_ok, back_sample.pitch,
              (unsigned long)back_sample.colored,
              (unsigned long)back_sample.white,
              (unsigned long)back_sample.left,
              (unsigned long)back_sample.top,
              (unsigned long)back_sample.right,
              (unsigned long)back_sample.bottom,
              primary_ok, primary_sample.pitch,
              (unsigned long)primary_sample.colored,
              (unsigned long)primary_sample.white,
              (unsigned long)primary_sample.left,
              (unsigned long)primary_sample.top,
              (unsigned long)primary_sample.right,
              (unsigned long)primary_sample.bottom);
    if (detail_bytes) detail[detail_bytes - 1] = '\0';
    if (old_bitmap && memory_dc) SelectObject(memory_dc, old_bitmap);
    if (bitmap) DeleteObject(bitmap);
    if (memory_dc) DeleteDC(memory_dc);
    if (dc && backbuffer) IDirectDrawSurface_ReleaseDC(backbuffer, dc);
    if (palette) IDirectDrawPalette_Release(palette);
    if (backbuffer) IDirectDrawSurface_Release(backbuffer);
    if (primary) IDirectDrawSurface_Release(primary);
    if (direct_draw) {
        if (exclusive) IDirectDraw_RestoreDisplayMode(direct_draw);
        IDirectDraw_Release(direct_draw);
    }
    if (window) DestroyWindow(window);
    if (cursor_hidden) ShowCursor(TRUE);
    return hr;
}

static void report_query(const char *detail)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    char message[512];
    int length;
    unsigned port;

    if (query_log) {
        fprintf(query_log, "%s\n", detail);
        fflush(query_log);
    }

    length = _snprintf(message, sizeof(message),
                       "QXP_D3D_STAGE rc=0 hr=0x00000000 "
                       "stage=ddraw-query %s", detail);
    if (length <= 0) return;
    if (length >= (int)sizeof(message)) length = sizeof(message) - 1;
    qxp_report_serial(message);
    if (WSAStartup(MAKEWORD(2, 0), &winsock)) return;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle != INVALID_SOCKET) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("10.0.2.2");
        for (port = 44574; port <= 44590; ++port) {
            address.sin_port = htons((u_short)port);
            sendto(socket_handle, message, length, 0,
                   (const struct sockaddr *)&address, sizeof(address));
        }
        closesocket(socket_handle);
    }
    WSACleanup();
}

static DWORD query_dword(HKEY root, const char *path, const char *name)
{
    HKEY key;
    DWORD value = 0xffffffff;
    DWORD type = 0;
    DWORD bytes = sizeof(value);

    if (RegOpenKeyExA(root, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return value;
    }
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&value, &bytes) !=
            ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(value)) {
        value = 0xffffffff;
    }
    RegCloseKey(key);
    return value;
}

static DWORD query_adapter_dword(const char *name)
{
    HKEY key;
    char path[512];
    DWORD type = 0;
    DWORD bytes = sizeof(path);
    const char prefix[] = "\\Registry\\Machine\\";

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\VIDEO", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0xffffffff;
    }
    if (RegQueryValueExA(key, "\\Device\\Video0", NULL, &type,
                         (BYTE *)path, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return 0xffffffff;
    }
    RegCloseKey(key);
    path[sizeof(path) - 1] = '\0';
    if (strncmp(path, prefix, sizeof(prefix) - 1)) return 0xffffffff;
    return query_dword(HKEY_LOCAL_MACHINE, path + sizeof(prefix) - 1, name);
}

static HRESULT run_video_blt(void)
{
    IDirectDraw *direct_draw = NULL;
    IDirectDraw7 *direct_draw7 = NULL;
    IDirect3D7 *direct3d7 = NULL;
    IDirect3DDevice7 *device7 = NULL;
    IDirectDrawSurface *source = NULL;
    IDirectDrawSurface *destination = NULL;
    IDirectDrawSurface7 *primary7 = NULL;
    IDirectDrawSurface7 *target7 = NULL;
    DDCAPS driver_caps;
    DDCAPS hel_caps;
    DDSCAPS2 memory_caps;
    DDSURFACEDESC surface_desc;
    DDSURFACEDESC locked;
    DDSURFACEDESC2 primary_desc;
    DDSURFACEDESC2 target_desc;
    DDSURFACEDESC2 display_mode;
    HRESULT hr;
    BYTE *row;
    DWORD bytes_per_pixel;
    DWORD row_bytes;
    DWORD x;
    DWORD y;

    video_blt_stage = "create";
    hr = DirectDrawCreate(NULL, &direct_draw, NULL);
    if (FAILED(hr)) goto done;
    video_blt_stage = "cooperative-level";
    hr = IDirectDraw_SetCooperativeLevel(direct_draw, GetDesktopWindow(),
                                         DDSCL_NORMAL);
    if (FAILED(hr)) goto done;

    video_blt_stage = "create-directdraw7";
    hr = DirectDrawCreateEx(
        NULL, (void **)&direct_draw7, &qxp_iid_directdraw7, NULL);
    if (FAILED(hr)) goto done;
    video_blt_stage = "directdraw7-cooperative-level";
    hr = IDirectDraw7_SetCooperativeLevel(
        direct_draw7, GetDesktopWindow(), DDSCL_NORMAL);
    if (FAILED(hr)) goto done;
    memset(&driver_caps, 0, sizeof(driver_caps));
    memset(&hel_caps, 0, sizeof(hel_caps));
    driver_caps.dwSize = sizeof(driver_caps);
    hel_caps.dwSize = sizeof(hel_caps);
    video_blt_stage = "directdraw7-caps";
    hr = IDirectDraw7_GetCaps(direct_draw7, &driver_caps, &hel_caps);
    if (FAILED(hr)) goto done;
    video_blt_caps = driver_caps.dwCaps;
    if (!(driver_caps.dwCaps & DDCAPS_BLT)) {
        hr = DDERR_NODIRECTDRAWHW;
        goto done;
    }
    memset(&memory_caps, 0, sizeof(memory_caps));
    memory_caps.dwCaps = DDSCAPS_VIDEOMEMORY;
    video_blt_stage = "directdraw7-vidmem";
    hr = IDirectDraw7_GetAvailableVidMem(
        direct_draw7, &memory_caps, &video_blt_vid_total,
        &video_blt_vid_free);
    if (FAILED(hr) || !video_blt_vid_total || !video_blt_vid_free) goto done;
    memset(&primary_desc, 0, sizeof(primary_desc));
    primary_desc.dwSize = sizeof(primary_desc);
    primary_desc.dwFlags = DDSD_CAPS;
    primary_desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    video_blt_stage = "directdraw7-primary";
    hr = IDirectDraw7_CreateSurface(
        direct_draw7, &primary_desc, &primary7, NULL);
    if (FAILED(hr)) goto done;

    video_blt_stage = "query-direct3d7";
    hr = IDirectDraw7_QueryInterface(
        direct_draw7, &qxp_iid_direct3d7, (void **)&direct3d7);
    if (FAILED(hr)) goto done;
    memset(&display_mode, 0, sizeof(display_mode));
    display_mode.dwSize = sizeof(display_mode);
    video_blt_stage = "direct3d7-display-mode";
    hr = IDirectDraw7_GetDisplayMode(direct_draw7, &display_mode);
    if (FAILED(hr)) goto done;
    memset(&target_desc, 0, sizeof(target_desc));
    target_desc.dwSize = sizeof(target_desc);
    target_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT |
                          DDSD_PIXELFORMAT;
    target_desc.dwWidth = 64;
    target_desc.dwHeight = 64;
    target_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN |
                                 DDSCAPS_3DDEVICE |
                                 DDSCAPS_VIDEOMEMORY;
    target_desc.ddpfPixelFormat = display_mode.ddpfPixelFormat;
    video_blt_stage = "direct3d7-target";
    hr = IDirectDraw7_CreateSurface(
        direct_draw7, &target_desc, &target7, NULL);
    if (FAILED(hr)) goto done;
    video_blt_stage = "direct3d7-device";
    hr = IDirect3D7_CreateDevice(
        direct3d7, &qxp_iid_direct3d_hal_device, target7, &device7);
    if (FAILED(hr)) goto done;

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.dwWidth = 16;
    surface_desc.dwHeight = 16;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN |
                                  DDSCAPS_VIDEOMEMORY;
    video_blt_stage = "create-source";
    hr = IDirectDraw_CreateSurface(direct_draw, &surface_desc, &source,
                                   NULL);
    if (FAILED(hr)) goto done;
    video_blt_stage = "create-destination";
    hr = IDirectDraw_CreateSurface(direct_draw, &surface_desc, &destination,
                                   NULL);
    if (FAILED(hr)) goto done;

    memset(&locked, 0, sizeof(locked));
    locked.dwSize = sizeof(locked);
    video_blt_stage = "lock-source";
    hr = IDirectDrawSurface_Lock(source, NULL, &locked, DDLOCK_WAIT, NULL);
    if (FAILED(hr)) goto done;
    bytes_per_pixel = locked.ddpfPixelFormat.dwRGBBitCount / 8;
    if (!bytes_per_pixel || bytes_per_pixel > 4 || locked.lPitch <= 0) {
        IDirectDrawSurface_Unlock(source, NULL);
        hr = E_FAIL;
        goto done;
    }
    row_bytes = surface_desc.dwWidth * bytes_per_pixel;
    for (y = 0; y < surface_desc.dwHeight; ++y) {
        row = (BYTE *)locked.lpSurface + y * locked.lPitch;
        memset(row, 0x5a, row_bytes);
    }
    video_blt_stage = "unlock-source";
    hr = IDirectDrawSurface_Unlock(source, NULL);
    if (FAILED(hr)) goto done;

    video_blt_stage = "blt";
    hr = IDirectDrawSurface_Blt(destination, NULL, source, NULL,
                                DDBLT_WAIT, NULL);
    if (FAILED(hr)) goto done;

    memset(&locked, 0, sizeof(locked));
    locked.dwSize = sizeof(locked);
    video_blt_stage = "lock-destination";
    hr = IDirectDrawSurface_Lock(destination, NULL, &locked,
                                 DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) goto done;
    for (y = 0; y < surface_desc.dwHeight && SUCCEEDED(hr); ++y) {
        row = (BYTE *)locked.lpSurface + y * locked.lPitch;
        for (x = 0; x < row_bytes; ++x) {
            if (row[x] != 0x5a) {
                hr = E_FAIL;
                break;
            }
        }
    }
    video_blt_stage = "unlock-destination";
    if (FAILED(IDirectDrawSurface_Unlock(destination, NULL)) && SUCCEEDED(hr))
        hr = E_FAIL;
    if (SUCCEEDED(hr)) video_blt_stage = "complete";

done:
    if (device7) IDirect3DDevice7_Release(device7);
    if (target7) IDirectDrawSurface7_Release(target7);
    if (direct3d7) IDirect3D7_Release(direct3d7);
    if (primary7) IDirectDrawSurface7_Release(primary7);
    if (destination) IDirectDrawSurface_Release(destination);
    if (source) IDirectDrawSurface_Release(source);
    if (direct_draw7) IDirectDraw7_Release(direct_draw7);
    if (direct_draw) IDirectDraw_Release(direct_draw);
    return hr;
}

int main(void)
{
    DDRAWI_DIRECTDRAW_GBL global;
    DDHALINFO hal;
    DDHAL_DDCALLBACKS dd_callbacks;
    DDHAL_DDSURFACECALLBACKS surface_callbacks;
    DDHAL_DDPALETTECALLBACKS palette_callbacks;
    D3DHAL_CALLBACKS d3d_callbacks;
    D3DHAL_GLOBALDRIVERDATA d3d_global;
    D3DHAL_CALLBACKS3 d3d_callbacks3;
    DDHAL_DDEXEBUFCALLBACKS buffer_callbacks;
    HDC dc;
    BOOL created;
    BOOL queried_first = FALSE;
    BOOL queried_second = FALSE;
    char detail[320];
    HMODULE gdi;
    GDI_ENTRY entry;
    PFN_DD_CREATE create_object = NULL;
    PFN_DD_QUERY query_object = NULL;
    PFN_DD_DELETE delete_object = NULL;
    DDHAL_GETDRIVERINFODATA info;
    BYTE info_buffer[512];
    QXP_GETDRIVERINFO2_DATA *gdi2;
    DWORD info_result = 0;
    DWORD info_actual = 0;
    HRESULT info_rval = E_FAIL;
    DWORD callbacks3_result = 0;
    DWORD callbacks3_actual = 0;
    HRESULT callbacks3_rval = E_FAIL;
    DWORD zpixel_result = 0;
    DWORD zpixel_actual = 0;
    HRESULT zpixel_rval = E_FAIL;
    DWORD zpixel_valid = 0;
    BOOL zpixel_padding = TRUE;
    BOOL zpixel_ok;
    DWORD index;
    DDPIXELFORMAT zpixel_formats[32];
    DDPIXELFORMAT expected_zpixel_formats[3];
    DDSURFACEDESC texture_formats[64];
    VIDMEM heaps[4];
    DWORD fourcc[16];
    DWORD acceleration;
    DWORD capability_override;
    DWORD emulation_only;
    DWORD software_only;
    HRESULT fullscreen_rval;
    HRESULT paletted_rval;
    HRESULT blt_rval;

    query_log = fopen("C:\\QXPDDQUERY.LOG", "w");
    if (!query_log) return 2;

    memset(&global, 0, sizeof(global));
    memset(&hal, 0, sizeof(hal));
    memset(&dd_callbacks, 0, sizeof(dd_callbacks));
    memset(&surface_callbacks, 0, sizeof(surface_callbacks));
    memset(&palette_callbacks, 0, sizeof(palette_callbacks));
    memset(&d3d_callbacks, 0, sizeof(d3d_callbacks));
    memset(&d3d_global, 0, sizeof(d3d_global));
    memset(&d3d_callbacks3, 0, sizeof(d3d_callbacks3));
    memset(&buffer_callbacks, 0, sizeof(buffer_callbacks));
    memset(zpixel_formats, 0xa5, sizeof(zpixel_formats));
    memset(expected_zpixel_formats, 0, sizeof(expected_zpixel_formats));
    expected_zpixel_formats[0].dwSize = sizeof(DDPIXELFORMAT);
    expected_zpixel_formats[0].dwFlags = DDPF_ZBUFFER;
    expected_zpixel_formats[0].dwZBufferBitDepth = 16;
    expected_zpixel_formats[0].dwZBitMask = 0x0000ffffUL;
    expected_zpixel_formats[1].dwSize = sizeof(DDPIXELFORMAT);
    expected_zpixel_formats[1].dwFlags =
        DDPF_ZBUFFER | DDPF_STENCILBUFFER;
    expected_zpixel_formats[1].dwZBufferBitDepth = 32;
    expected_zpixel_formats[1].dwStencilBitDepth = 8;
    expected_zpixel_formats[1].dwZBitMask = 0xffffff00UL;
    expected_zpixel_formats[1].dwStencilBitMask = 0x000000ffUL;
    expected_zpixel_formats[2].dwSize = sizeof(DDPIXELFORMAT);
    expected_zpixel_formats[2].dwFlags = DDPF_ZBUFFER;
    expected_zpixel_formats[2].dwZBufferBitDepth = 32;
    expected_zpixel_formats[2].dwZBitMask = 0xffffff00UL;
    memset(texture_formats, 0, sizeof(texture_formats));
    memset(heaps, 0, sizeof(heaps));
    memset(fourcc, 0, sizeof(fourcc));

    gdi = GetModuleHandleA("gdi32.dll");
    if (gdi) {
        entry.proc = GetProcAddress(gdi, "GdiEntry1");
        create_object = entry.create;
        entry.proc = GetProcAddress(gdi, "GdiEntry2");
        query_object = entry.query;
        entry.proc = GetProcAddress(gdi, "GdiEntry3");
        delete_object = entry.delete_object;
    }
    dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    created = dc && create_object ? create_object(&global, dc) : FALSE;
    if (created) {
        queried_first = query_object && query_object(
            &global, &hal, &dd_callbacks, &surface_callbacks,
            &palette_callbacks, &d3d_callbacks, &d3d_global,
            &buffer_callbacks, NULL, NULL, NULL);
        queried_second = query_object && query_object(
            &global, &hal, &dd_callbacks, &surface_callbacks,
            &palette_callbacks, &d3d_callbacks, &d3d_global,
            &buffer_callbacks, texture_formats, fourcc, heaps);
    }
    memset(&info, 0, sizeof(info));
    memset(info_buffer, 0, sizeof(info_buffer));
    gdi2 = (QXP_GETDRIVERINFO2_DATA *)info_buffer;
    gdi2->magic = 0xffffffff;
    gdi2->type = 1;
    gdi2->expected_size = 212;
    info.dwSize = sizeof(info);
    info.guidInfo.Data1 = 0xf828169c;
    info.guidInfo.Data2 = 0xa8e8;
    info.guidInfo.Data3 = 0x11d2;
    info.guidInfo.Data4[0] = 0xa1;
    info.guidInfo.Data4[1] = 0xf2;
    info.guidInfo.Data4[2] = 0x00;
    info.guidInfo.Data4[3] = 0xa0;
    info.guidInfo.Data4[4] = 0xc9;
    info.guidInfo.Data4[5] = 0x83;
    info.guidInfo.Data4[6] = 0xea;
    info.guidInfo.Data4[7] = 0xf6;
    info.dwExpectedSize = sizeof(info_buffer);
    info.lpvData = info_buffer;
    info.dwContext = global.hDD;
    if (queried_second && hal.GetDriverInfo) {
        info_result = hal.GetDriverInfo(&info);
        info_actual = info.dwActualSize;
        info_rval = info.ddRVal;
    }
    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    info.guidInfo.Data1 = 0xddf41230;
    info.guidInfo.Data2 = 0xec0a;
    info.guidInfo.Data3 = 0x11d0;
    info.guidInfo.Data4[0] = 0xa9;
    info.guidInfo.Data4[1] = 0xb6;
    info.guidInfo.Data4[2] = 0x00;
    info.guidInfo.Data4[3] = 0xaa;
    info.guidInfo.Data4[4] = 0x00;
    info.guidInfo.Data4[5] = 0xc0;
    info.guidInfo.Data4[6] = 0x99;
    info.guidInfo.Data4[7] = 0x3e;
    info.dwExpectedSize = sizeof(d3d_callbacks3);
    info.lpvData = &d3d_callbacks3;
    info.dwContext = global.hDD;
    if (queried_second && hal.GetDriverInfo) {
        callbacks3_result = hal.GetDriverInfo(&info);
        callbacks3_actual = info.dwActualSize;
        callbacks3_rval = info.ddRVal;
    }
    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    info.guidInfo = qxp_guid_zpixel_formats;
    info.dwExpectedSize = sizeof(zpixel_formats);
    info.lpvData = zpixel_formats;
    info.dwContext = global.hDD;
    if (queried_second && hal.GetDriverInfo) {
        zpixel_result = hal.GetDriverInfo(&info);
        zpixel_actual = info.dwActualSize;
        zpixel_rval = info.ddRVal;
    }
    for (index = 0; index < 3; ++index) {
        if (!memcmp(&zpixel_formats[index], &expected_zpixel_formats[index],
                    sizeof(zpixel_formats[index]))) ++zpixel_valid;
    }
    for (index = 3; index < 32; ++index) {
        if (!bytes_are_zero(&zpixel_formats[index],
                            sizeof(zpixel_formats[index])))
            zpixel_padding = FALSE;
    }
    zpixel_ok = zpixel_result == DDHAL_DRIVER_HANDLED &&
                zpixel_actual == sizeof(zpixel_formats) &&
                SUCCEEDED(zpixel_rval) && zpixel_valid == 3 &&
                zpixel_padding;
    acceleration = query_adapter_dword("Acceleration.Level");
    capability_override = query_adapter_dword("CapabilityOverride");
    emulation_only = query_dword(HKEY_LOCAL_MACHINE,
                                 "SOFTWARE\\Microsoft\\DirectDraw",
                                 "EmulationOnly");
    software_only = query_dword(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Direct3D\\Drivers",
                                "SoftwareOnly");
    _snprintf(detail, sizeof(detail),
              "create=%u q1=%u q2=%u hflags=%08lx ddcaps=%08lx "
              "gsize=%lu hsize=%lu formats=%lu hwflags=%08lx "
              "devcaps=%08lx csize=%lu context=%p gi=%lu actual=%lu "
              "girval=%08lx capstype=%lu gflags=%08lx heaps=%lu "
              "zformats=%lu vidtotal=%lu vidfree=%lu accel=%lu capovr=%lu "
              "emu=%lu soft=%lu",
              created, queried_first, queried_second,
              (unsigned long)hal.dwFlags,
              (unsigned long)hal.ddCaps.dwCaps,
              (unsigned long)d3d_global.dwSize,
              (unsigned long)d3d_global.hwCaps.dwSize,
              (unsigned long)d3d_global.dwNumTextureFormats,
              (unsigned long)d3d_global.hwCaps.dwFlags,
              (unsigned long)d3d_global.hwCaps.dwDevCaps,
              (unsigned long)d3d_callbacks.dwSize,
              d3d_callbacks.ContextCreate, (unsigned long)info_result,
              (unsigned long)info_actual,
              (unsigned long)info_rval,
              (unsigned long)*(DWORD *)info_buffer,
              (unsigned long)global.dwFlags,
              (unsigned long)global.vmiData.dwNumHeaps,
              (unsigned long)global.dwNumZPixelFormats,
              (unsigned long)hal.ddCaps.dwVidMemTotal,
              (unsigned long)hal.ddCaps.dwVidMemFree,
              (unsigned long)acceleration,
              (unsigned long)capability_override,
              (unsigned long)emulation_only,
              (unsigned long)software_only);
    detail[sizeof(detail) - 1] = '\0';
    report_query(detail);
    _snprintf(detail, sizeof(detail),
              "g3result=%lu actual=%lu rval=%08lx size=%lu flags=%08lx "
              "clear=%p validate=%p dp2=%p",
              (unsigned long)callbacks3_result,
              (unsigned long)callbacks3_actual,
              (unsigned long)callbacks3_rval,
              (unsigned long)d3d_callbacks3.dwSize,
              (unsigned long)d3d_callbacks3.dwFlags,
              d3d_callbacks3.Clear2,
              d3d_callbacks3.ValidateTextureStageState,
              d3d_callbacks3.DrawPrimitives2);
    detail[sizeof(detail) - 1] = '\0';
    report_query(detail);
    _snprintf(detail, sizeof(detail),
              "zpf result=%lu actual=%lu rval=%08lx valid=%lu padding=%u",
              (unsigned long)zpixel_result,
              (unsigned long)zpixel_actual,
              (unsigned long)zpixel_rval,
              (unsigned long)zpixel_valid, zpixel_padding);
    detail[sizeof(detail) - 1] = '\0';
    report_query(detail);
    if (created && delete_object) delete_object(&global);
    if (dc) DeleteDC(dc);
    paletted_rval = run_paletted_flip(detail, sizeof(detail));
    report_query(detail);
    fullscreen_rval = run_fullscreen_flip(detail, sizeof(detail));
    report_query(detail);
    blt_rval = run_video_blt();
    _snprintf(detail, sizeof(detail),
              "video-blt stage=%s rval=%08lx caps=%08lx total=%lu free=%lu",
              video_blt_stage, (unsigned long)blt_rval,
              (unsigned long)video_blt_caps,
              (unsigned long)video_blt_vid_total,
              (unsigned long)video_blt_vid_free);
    detail[sizeof(detail) - 1] = '\0';
    report_query(detail);
    fclose(query_log);
    query_log = NULL;
    return queried_second && zpixel_ok && SUCCEEDED(fullscreen_rval) &&
           SUCCEEDED(paletted_rval) && SUCCEEDED(blt_rval) ? 0 : 1;
}
