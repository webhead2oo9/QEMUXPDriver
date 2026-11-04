#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

typedef IDirect3D9 *(WINAPI *PFN_DIRECT3DCREATE9)(UINT sdk_version);

typedef union d3d9_entry_point {
    FARPROC proc;
    PFN_DIRECT3DCREATE9 create;
} D3D9_ENTRY_POINT;

typedef struct d3d9_probe_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
} D3D9_PROBE_VERTEX;

typedef struct d3d9_texture_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
} D3D9_TEXTURE_VERTEX;

typedef struct d3d9_a8_vertex {
    float x;
    float y;
    float z;
    float rhw;
    float point_size;
    DWORD color;
    float u;
    float v;
} D3D9_A8_VERTEX;

typedef struct d3d9_bump_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u0;
    float v0;
    float u1;
    float v1;
} D3D9_BUMP_VERTEX;

typedef struct d3d9_matrix_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u0;
    float v0;
    float u1;
    float v1;
    float u2;
    float v2;
} D3D9_MATRIX_VERTEX;

typedef struct d3d9_four_texture_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u0;
    float v0;
    float u1;
    float v1;
    float u2;
    float v2;
    float u3;
    float v3;
} D3D9_FOUR_TEXTURE_VERTEX;

typedef struct d3d9_cube_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u0;
    float v0;
    float x1;
    float y1;
    float z1;
    float x2;
    float y2;
    float z2;
    float x3;
    float y3;
    float z3;
} D3D9_CUBE_VERTEX;

typedef struct d3d9_fog_vertex {
    float x;
    float y;
    float z;
    DWORD diffuse;
    DWORD specular;
} D3D9_FOG_VERTEX;

static void report_message(const char *kind, int rc, HRESULT hr,
                           const char *stage, const char *detail)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    char message[512];
    int length;
    unsigned port;

    length = _snprintf(message, sizeof(message),
                       "%s rc=%d hr=0x%08lx stage=%s%s%s",
                       kind, rc, (unsigned long)hr, stage,
                       detail && detail[0] ? " " : "",
                       detail && detail[0] ? detail : "");
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

static void report_stage(const char *stage, HRESULT hr, const char *detail)
{
    report_message("QXP_D3D9_STAGE", 0, hr, stage, detail);
}

static int fail_probe(int rc, HRESULT hr, const char *stage,
                      const char *detail)
{
    report_message("QXP_D3D9_RESULT", rc, hr, stage, detail);
    return rc;
}

static BOOL wait_capture_ack(const char *stage, const char *detail,
                             int virtual_key)
{
    const DWORD timeout_ms = 300000;
    DWORD started;
    DWORD last_reported;
    DWORD now;
    SHORT key_state;

    /* Discard a stale transition before advertising that this scene is held. */
    key_state = GetAsyncKeyState(virtual_key);
    started = GetTickCount();
    while (key_state & 0x8000) {
        if ((DWORD)(GetTickCount() - started) >= timeout_ms) return FALSE;
        Sleep(10);
        key_state = GetAsyncKeyState(virtual_key);
    }

    report_stage(stage, S_OK, detail);
    started = GetTickCount();
    last_reported = started;
    for (;;) {
        key_state = GetAsyncKeyState(virtual_key);
        if (key_state & 0x8001) return TRUE;
        now = GetTickCount();
        if ((DWORD)(now - started) >= timeout_ms) return FALSE;
        /*
         * Keep the terminal wait state observable to a growing-file reader.
         * Repeating this acceptance-only marker also closes the interval in
         * which an early synthetic key can expire while report_stage returns.
         */
        if ((DWORD)(now - last_reported) >= 5000) {
            report_stage(stage, S_OK, detail);
            last_reported = GetTickCount();
        }
        Sleep(10);
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HRESULT fill_dynamic_texture9(IDirect3DTexture9 *texture, DWORD color)
{
    D3DLOCKED_RECT locked;
    HRESULT hr;
    HRESULT unlock_hr;
    DWORD x;
    DWORD y;

    if (!texture) return E_INVALIDARG;
    memset(&locked, 0, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(
        texture, 0, &locked, NULL, D3DLOCK_DISCARD);
    if (FAILED(hr)) return hr;
    if (!locked.pBits || locked.Pitch < 16 * (INT)sizeof(DWORD)) {
        hr = E_FAIL;
    } else {
        for (y = 0; y < 16; ++y) {
            DWORD *pixels = (DWORD *)((BYTE *)locked.pBits +
                                      y * locked.Pitch);
            for (x = 0; x < 16; ++x) pixels[x] = color;
        }
    }
    unlock_hr = IDirect3DTexture9_UnlockRect(texture, 0);
    if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    return hr;
}

static void fill_a8_quad9(D3D9_A8_VERTEX vertices[6], float left,
                          float top, float u, DWORD color)
{
    const float right = left + 16.0f;
    const float bottom = top + 16.0f;
    const D3D9_A8_VERTEX quad[6] = {
        { left,  top,    0.25f, 1.0f, 1.0f, color, u, 0.5f },
        { right, top,    0.25f, 1.0f, 1.0f, color, u, 0.5f },
        { left,  bottom, 0.25f, 1.0f, 1.0f, color, u, 0.5f },
        { left,  bottom, 0.25f, 1.0f, 1.0f, color, u, 0.5f },
        { right, top,    0.25f, 1.0f, 1.0f, color, u, 0.5f },
        { right, bottom, 0.25f, 1.0f, 1.0f, color, u, 0.5f }
    };

    memcpy(vertices, quad, sizeof(quad));
}

static HRESULT run_a8_texture_contract9(IDirect3DDevice9 *device)
{
    static const BYTE alpha_texels[4] = { 0x00, 0x55, 0xaa, 0xff };
    static const DWORD decode_expected[4] = {
        0x000000UL, 0x555555UL, 0xaaaaaaUL, 0xffffffUL
    };
    static const DWORD blend_expected[4] = {
        0x204080UL, 0x606060UL, 0xa08040UL, 0xe0a020UL
    };
    static const DWORD shader_expected[4] = {
        0x204080UL, 0x152b55UL, 0x0b152bUL, 0x000000UL
    };
    /* ps_1_1: tex t0; mul r0, t0, v0.  D3D defines A8's otherwise
     * undefined RGB channels as zero, so source RGB is zero while source
     * alpha remains A.  Exact destination attenuation therefore verifies
     * legacy shader sampling and alpha preservation independently. */
    static const DWORD shader_code[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX,
        0xb00f0000UL,
        D3DSIO_MUL,
        0x800f0000UL,
        0xb0e40000UL,
        0x90e40000UL,
        D3DSIO_END
    };
    IDirect3DTexture9 *texture = NULL;
    IDirect3DTexture9 *target_texture = NULL;
    IDirect3DSurface9 *target = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *saved_target = NULL;
    IDirect3DSurface9 *saved_depth = NULL;
    IDirect3DPixelShader9 *shader = NULL;
    IDirect3DVertexBuffer9 *sentinel_vb = NULL;
    D3D9_A8_VERTEX vertices[6];
    void *vertex_data = NULL;
    D3DLOCKED_RECT locked;
    D3DVIEWPORT9 saved_viewport;
    D3DVIEWPORT9 viewport;
    HRESULT hr;
    HRESULT step_hr;
    HRESULT unlock_hr;
    DWORD column;
    DWORD pass;
    DWORD x;
    DWORD y;
    BOOL scene_begun = FALSE;
    BOOL depth_detached = FALSE;
    BOOL target_bound = FALSE;
    BOOL texture_locked = FALSE;
    BOOL sentinel_vb_locked = FALSE;
    BOOL normal_match = TRUE;
    BOOL transposed_match = TRUE;
    DWORD first_pass = 0;
    DWORD first_column = 0;
    DWORD first_observed = 0;
    DWORD first_expected = 0;
    DWORD sentinel_observed = 0;
    DWORD sentinel_vb_observed = 0;
    DWORD color_write_state = 0;
    DWORD alpha_test_state = 0;
    DWORD scissor_state = 0;
    DWORD fill_state = 0;
    DWORD cull_state = 0;
    DWORD nonclear_count = 0;
    DWORD nonclear_min_x = 64;
    DWORD nonclear_min_y = 64;
    DWORD nonclear_max_x = 0;
    DWORD nonclear_max_y = 0;
    DWORD distinct_nonclear[3] = { 0, 0, 0 };
    DWORD distinct_nonclear_count = 0;
    DWORD normal_observed[3][4];
    DWORD transposed_observed[3][4];
    char detail[192];

    if (!device) return E_INVALIDARG;
    memset(&locked, 0, sizeof(locked));
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    memset(&viewport, 0, sizeof(viewport));
    memset(normal_observed, 0, sizeof(normal_observed));
    memset(transposed_observed, 0, sizeof(transposed_observed));
    report_stage("a8-contract-enter", S_OK,
                 "4x1-pattern-fixed-decode-fixed-blend-ps11-blend");

    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &saved_target);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetDepthStencilSurface(device, &saved_depth);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetViewport(device, &saved_viewport);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 4, 1, 1, 0, D3DFMT_A8,
            D3DPOOL_MANAGED, &texture, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
        if (SUCCEEDED(hr)) texture_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        if (!locked.pBits || locked.Pitch < 4) {
            hr = E_FAIL;
        } else {
            memcpy(locked.pBits, alpha_texels, sizeof(alpha_texels));
        }
    }
    if (texture_locked) {
        unlock_hr = IDirect3DTexture9_UnlockRect(texture, 0);
        texture_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 64, 64, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_texture, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DTexture9_GetSurfaceLevel(target_texture, 0, &target);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
            device, 64, 64, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &readback, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreatePixelShader(
            device, shader_code, &shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexBuffer(
            device, sizeof(vertices), 0,
            D3DFVF_XYZRHW | D3DFVF_PSIZE |
                D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_MANAGED, &sentinel_vb, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DVertexBuffer9_Lock(
            sentinel_vb, 0, sizeof(vertices), &vertex_data, 0);
        if (SUCCEEDED(hr)) sentinel_vb_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        fill_a8_quad9(vertices, 32.0f, 48.0f, 0.5f, 0xffb52ac8UL);
        memcpy(vertex_data, vertices, sizeof(vertices));
    }
    if (sentinel_vb_locked) {
        unlock_hr = IDirect3DVertexBuffer9_Unlock(sentinel_vb);
        sentinel_vb_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    }
    report_stage("a8-contract-create", hr,
                 SUCCEEDED(hr) ?
                    "a8-pattern-argb-target-texture-systemmem-copy-ps11" :
                    "failed");

    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
        if (SUCCEEDED(hr)) depth_detached = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    viewport.Width = 64;
    viewport.Height = 64;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetViewport(device, &viewport);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderState(
            device, D3DRS_COLORWRITEENABLE, &color_write_state);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderState(
            device, D3DRS_ALPHATESTENABLE, &alpha_test_state);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderState(
            device, D3DRS_SCISSORTESTENABLE, &scissor_state);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderState(
            device, D3DRS_FILLMODE, &fill_state);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderState(
            device, D3DRS_CULLMODE, &cull_state);
    }
    if (SUCCEEDED(hr)) {
        _snprintf(detail, sizeof(detail),
                  "colorwrite=%08lx alphatest=%lu scissor=%lu "
                  "fill=%lu cull=%lu",
                  (unsigned long)color_write_state,
                  (unsigned long)alpha_test_state,
                  (unsigned long)scissor_state,
                  (unsigned long)fill_state,
                  (unsigned long)cull_state);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-state", S_OK, detail);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff204080UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_COLORWRITEENABLE, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetFVF(
            device, D3DFVF_XYZRHW | D3DFVF_PSIZE |
                    D3DFVF_DIFFUSE | D3DFVF_TEX1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }

    /* First draw a texture-free diffuse sentinel into the otherwise unused
     * fourth row.  This distinguishes a general D3D9 off-screen draw failure
     * from one specific to the A8 texture path. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        fill_a8_quad9(vertices, 0.0f, 48.0f, 0.5f, 0xff12d34aUL);
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetStreamSource(
            device, 0, sentinel_vb, 0, sizeof(vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitive(
            device, D3DPT_TRIANGLELIST, 0, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }

    /* Pass 0: alpha-replicated RGB, no blend.  Force a TRUE->FALSE
     * transition before the first patterned draw so neither runtime
     * default-state elision nor stale host blend state can explain an
     * untouched cell.  The square 3x4 matrix is also sampled through its
     * transpose below. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1,
            D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad9(vertices, (float)(column * 16), 0.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }

    /* Pass 1: fixed-function diffuse RGB and patterned texture alpha. */
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, TRUE);
    }
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad9(vertices, (float)(column * 16), 16.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }

    /* Pass 2: the equivalent ps_1_1 sample/multiply path. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, shader);
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad9(vertices, (float)(column * 16), 32.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }
    if (scene_begun) {
        step_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    report_stage("a8-contract-draw", hr,
                 SUCCEEDED(hr) ? "decode+fixed+ps11" : "failed");

    /* Restore before locking: the lockable target must not remain bound. */
    if (target_bound || depth_detached) {
        step_hr = S_OK;
        if (target_bound) {
            step_hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
            if (SUCCEEDED(step_hr)) {
                step_hr = IDirect3DDevice9_SetRenderTarget(
                    device, 0, saved_target);
            }
        }
        if (SUCCEEDED(step_hr) && depth_detached) {
            step_hr = IDirect3DDevice9_SetDepthStencilSurface(
                device, saved_depth);
        }
        if (SUCCEEDED(step_hr) && target_bound) {
            step_hr = IDirect3DDevice9_SetViewport(
                device, &saved_viewport);
        }
        if (SUCCEEDED(step_hr)) {
            target_bound = FALSE;
            depth_detached = FALSE;
        }
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderTargetData(
            device, target, readback);
    }
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) texture_locked = TRUE;
    }
    if (SUCCEEDED(hr) &&
        (!locked.pBits || locked.Pitch < 64 * (INT)sizeof(DWORD))) {
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 64; ++y) {
            const DWORD *coverage_pixels = (const DWORD *)(
                (const BYTE *)locked.pBits + y * locked.Pitch);

            for (x = 0; x < 64; ++x) {
                DWORD coverage = coverage_pixels[x] & 0x00ffffffUL;
                DWORD distinct;
                BOOL seen = FALSE;

                if (coverage == 0x204080UL) continue;
                ++nonclear_count;
                if (x < nonclear_min_x) nonclear_min_x = x;
                if (y < nonclear_min_y) nonclear_min_y = y;
                if (x > nonclear_max_x) nonclear_max_x = x;
                if (y > nonclear_max_y) nonclear_max_y = y;
                for (distinct = 0;
                     distinct < distinct_nonclear_count; ++distinct) {
                    if (distinct_nonclear[distinct] == coverage) {
                        seen = TRUE;
                        break;
                    }
                }
                if (!seen && distinct_nonclear_count < 3)
                    distinct_nonclear[distinct_nonclear_count++] = coverage;
            }
        }
        _snprintf(detail, sizeof(detail),
                  "nonclear=%lu bbox=%lu,%lu-%lu,%lu "
                  "colors=%06lx/%06lx/%06lx",
                  (unsigned long)nonclear_count,
                  (unsigned long)(nonclear_count ? nonclear_min_x : 0),
                  (unsigned long)(nonclear_count ? nonclear_min_y : 0),
                  (unsigned long)(nonclear_count ? nonclear_max_x : 0),
                  (unsigned long)(nonclear_count ? nonclear_max_y : 0),
                  (unsigned long)distinct_nonclear[0],
                  (unsigned long)distinct_nonclear[1],
                  (unsigned long)distinct_nonclear[2]);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-coverage",
                     nonclear_count ? S_OK : E_FAIL, detail);
    }
    if (SUCCEEDED(hr)) {
        const DWORD *sentinel_pixels = (const DWORD *)(
            (const BYTE *)locked.pBits + 56 * locked.Pitch);
        sentinel_observed = sentinel_pixels[8] & 0x00ffffffUL;
        sentinel_vb_observed = sentinel_pixels[40] & 0x00ffffffUL;
        _snprintf(detail, sizeof(detail),
                  "up=%06lx expected=12d34a vb=%06lx expected=b52ac8",
                  (unsigned long)sentinel_observed,
                  (unsigned long)sentinel_vb_observed);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-sentinel",
                     sentinel_observed == 0x12d34aUL &&
                     sentinel_vb_observed == 0xb52ac8UL ? S_OK : E_FAIL,
                     detail);
    }
    for (pass = 0; SUCCEEDED(hr) && pass < 3; ++pass) {
        const DWORD *expected = pass == 0 ? decode_expected :
                                (pass == 1 ? blend_expected :
                                             shader_expected);
        const DWORD *normal_pixels = (const DWORD *)(
            (const BYTE *)locked.pBits +
            (pass * 16 + 8) * locked.Pitch);

        for (column = 0; column < 4; ++column) {
            const DWORD *transposed_pixels = (const DWORD *)(
                (const BYTE *)locked.pBits +
                (column * 16 + 8) * locked.Pitch);
            DWORD observed = normal_pixels[column * 16 + 8] &
                             0x00ffffffUL;
            DWORD transposed = transposed_pixels[pass * 16 + 8] &
                               0x00ffffffUL;

            normal_observed[pass][column] = observed;
            transposed_observed[pass][column] = transposed;
            if (observed != expected[column]) {
                if (normal_match) {
                    first_pass = pass;
                    first_column = column;
                    first_observed = observed;
                    first_expected = expected[column];
                }
                normal_match = FALSE;
            }
            if (transposed != expected[column]) transposed_match = FALSE;
        }
    }
    if (SUCCEEDED(hr) && !normal_match) {
        for (pass = 0; pass < 3; ++pass) {
            _snprintf(detail, sizeof(detail),
                      "normal%lu=%06lx/%06lx/%06lx/%06lx",
                      (unsigned long)pass,
                      (unsigned long)normal_observed[pass][0],
                      (unsigned long)normal_observed[pass][1],
                      (unsigned long)normal_observed[pass][2],
                      (unsigned long)normal_observed[pass][3]);
            detail[sizeof(detail) - 1] = '\0';
            report_stage("a8-contract-observed", S_OK, detail);
        }
        for (pass = 0; pass < 3; ++pass) {
            _snprintf(detail, sizeof(detail),
                      "transpose%lu=%06lx/%06lx/%06lx/%06lx",
                      (unsigned long)pass,
                      (unsigned long)transposed_observed[pass][0],
                      (unsigned long)transposed_observed[pass][1],
                      (unsigned long)transposed_observed[pass][2],
                      (unsigned long)transposed_observed[pass][3]);
            detail[sizeof(detail) - 1] = '\0';
            report_stage("a8-contract-observed", S_OK, detail);
        }
        _snprintf(detail, sizeof(detail),
                  "pass=%lu alpha=%u observed=%06lx expected=%06lx "
                  "transpose=%s",
                  (unsigned long)first_pass,
                  (unsigned)alpha_texels[first_column],
                  (unsigned long)first_observed,
                  (unsigned long)first_expected,
                  transposed_match ? "exact" : "mismatch");
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-readback", E_FAIL, detail);
        hr = E_FAIL;
    }
    if (texture_locked) {
        unlock_hr = IDirect3DSurface9_UnlockRect(readback);
        texture_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    }
    if (SUCCEEDED(hr)) {
        report_stage("a8-contract-readback", S_OK,
                     "decode=000000/555555/aaaaaa/ffffff "
                     "fixed=204080/606060/a08040/e0a020 "
                     "ps11=204080/152b55/0b152b/000000");
    }

    if (scene_begun) {
        step_hr = IDirect3DDevice9_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    step_hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    if (target_bound || depth_detached) {
        IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
        if (target_bound) {
            IDirect3DDevice9_SetRenderTarget(device, 0, saved_target);
            IDirect3DDevice9_SetViewport(device, &saved_viewport);
        }
        if (depth_detached) {
            IDirect3DDevice9_SetDepthStencilSurface(device, saved_depth);
        }
    }
    if (sentinel_vb_locked && sentinel_vb)
        IDirect3DVertexBuffer9_Unlock(sentinel_vb);
    if (sentinel_vb) IDirect3DVertexBuffer9_Release(sentinel_vb);
    if (shader) IDirect3DPixelShader9_Release(shader);
    if (readback) IDirect3DSurface9_Release(readback);
    if (target) IDirect3DSurface9_Release(target);
    if (target_texture) IDirect3DTexture9_Release(target_texture);
    if (texture) IDirect3DTexture9_Release(texture);
    if (saved_depth) IDirect3DSurface9_Release(saved_depth);
    if (saved_target) IDirect3DSurface9_Release(saved_target);
    report_stage("a8-contract-leave", hr,
                 SUCCEEDED(hr) ? "exact-patterned-alpha=ok" : "failed");
    return hr;
}

static HRESULT run_separate_fog_contract9(IDirect3DDevice9 *device)
{
    static const D3D9_FOG_VERTEX vertices[4] = {
        { -1.0f, -1.0f, 0.25f, 0xffff0000UL, 0x40000000UL },
        { -1.0f,  1.0f, 0.25f, 0xffff0000UL, 0x40000000UL },
        {  1.0f, -1.0f, 0.25f, 0xffff0000UL, 0x40000000UL },
        {  1.0f,  1.0f, 0.25f, 0xffff0000UL, 0x40000000UL }
    };
    IDirect3DTexture9 *target_texture = NULL;
    IDirect3DSurface9 *target = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *saved_target = NULL;
    IDirect3DSurface9 *saved_depth = NULL;
    D3DVIEWPORT9 saved_viewport;
    D3DVIEWPORT9 viewport;
    D3DLOCKED_RECT locked;
    D3DMATRIX identity;
    HRESULT hr;
    HRESULT cleanup_hr;
    DWORD observed = 0;
    DWORD alpha;
    DWORD red;
    DWORD green;
    DWORD blue;
    BOOL target_bound = FALSE;
    BOOL depth_detached = FALSE;
    BOOL viewport_saved = FALSE;
    BOOL scene_begun = FALSE;
    BOOL readback_locked = FALSE;
    BOOL saved_software_vp = FALSE;
    BOOL software_vp_selected = FALSE;
    char detail[160];

    if (!device) return E_INVALIDARG;
    report_stage("separate-fog-enter", S_OK,
                 "software-vp-linear-fog-specular-alpha-readback");
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    memset(&viewport, 0, sizeof(viewport));
    memset(&locked, 0, sizeof(locked));
    memset(&identity, 0, sizeof(identity));
    identity._11 = 1.0f;
    identity._22 = 1.0f;
    identity._33 = 1.0f;
    identity._44 = 1.0f;

    saved_software_vp =
        IDirect3DDevice9_GetSoftwareVertexProcessing(device);
    hr = IDirect3DDevice9_SetSoftwareVertexProcessing(device, TRUE);
    if (SUCCEEDED(hr)) software_vp_selected = TRUE;
    if (FAILED(hr)) goto cleanup;

cleanup:
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTarget(device, 0, &saved_target);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetDepthStencilSurface(device, &saved_depth);
    if (hr == D3DERR_NOTFOUND) hr = S_OK;
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetViewport(device, &saved_viewport);
        if (SUCCEEDED(hr)) viewport_saved = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 16, 16, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_texture, NULL);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DTexture9_GetSurfaceLevel(target_texture, 0, &target);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
            device, 16, 16, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &readback, NULL);
    }
    report_stage("separate-fog-create", hr,
                 SUCCEEDED(hr) ? "argb-target-systemmem-copy" : "failed");

    if (SUCCEEDED(hr) && saved_depth) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
        if (SUCCEEDED(hr)) depth_detached = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    if (SUCCEEDED(hr)) {
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = 16;
        viewport.Height = 16;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        hr = IDirect3DDevice9_SetViewport(device, &viewport);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTransform(
            device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTransform(
            device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTransform(
            device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SPECULARENABLE, TRUE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_COLORWRITEENABLE, 0x0fUL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff0000ffUL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGSTART, 0x00000000UL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGEND, 0x3f800000UL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetVertexShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetFVF(
            device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_SPECULAR);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage("separate-fog-draw", hr,
                 SUCCEEDED(hr) ? "linear-z-quarter-alpha-40" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, readback);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        alpha = (observed >> 24) & 0xffUL;
        red = (observed >> 16) & 0xffUL;
        green = (observed >> 8) & 0xffUL;
        blue = observed & 0xffUL;
        if (alpha != 0x40UL || red < 0xbdUL || red > 0xc1UL ||
            green > 0x02UL || blue < 0x3dUL || blue > 0x42UL) {
            hr = E_FAIL;
        }
    }
    _snprintf(detail, sizeof(detail),
              "center=%08lx expected-alpha=40 expected-rgb=bf0040",
              (unsigned long)observed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("separate-fog-readback", hr, detail);

    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, FALSE);
    if (target_bound && saved_target) {
        cleanup_hr = IDirect3DDevice9_SetRenderTarget(
            device, 0, saved_target);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (depth_detached && saved_depth) {
        cleanup_hr = IDirect3DDevice9_SetDepthStencilSurface(
            device, saved_depth);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (viewport_saved) {
        cleanup_hr = IDirect3DDevice9_SetViewport(device, &saved_viewport);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (software_vp_selected) {
        cleanup_hr = IDirect3DDevice9_SetSoftwareVertexProcessing(
            device, saved_software_vp);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (readback) IDirect3DSurface9_Release(readback);
    if (target) IDirect3DSurface9_Release(target);
    if (target_texture) IDirect3DTexture9_Release(target_texture);
    if (saved_depth) IDirect3DSurface9_Release(saved_depth);
    if (saved_target) IDirect3DSurface9_Release(saved_target);
    report_stage("separate-fog-leave", hr,
                 SUCCEEDED(hr) ?
                    "per-vertex-fog-and-specular-alpha=ok" : "failed");
    return hr;
}

#define QXP_MS_POSITION_STRIDE 32U
#define QXP_MS_POSITION_ELEMENT_OFFSET 16U
#define QXP_MS_COLOR_STRIDE 12U
#define QXP_MS_COLOR_ELEMENT_OFFSET 4U
#define QXP_MS_ORACLE_COLOR 0xff2ad46fUL
#define QXP_MS_PARAMETER_TOKEN 0x80000000UL
#define QXP_SM2_FULL_SAMPLER_COUNT 16U
#define QXP_SM2_FULL_SAMPLER_SHADER_DWORDS 180U
#define QXP_SM2_FULL_SAMPLER_FIRST_BATCH 11U
#define QXP_SM2_FULL_SAMPLER_EXPECTED 0xffff2040UL
#define QXP_MS_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_MS_DEST(type, number, mask) \
    (QXP_MS_PARAMETER_TOKEN | QXP_MS_REGTYPE(type) | (DWORD)(number) | \
     (DWORD)(mask))
#define QXP_MS_SOURCE(type, number) \
    (QXP_MS_PARAMETER_TOKEN | QXP_MS_REGTYPE(type) | (DWORD)(number) | \
     D3DVS_NOSWIZZLE)

typedef char qxp_ms_opos_token_is_exact[
    QXP_MS_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                D3DSP_WRITEMASK_ALL) == 0xc00f0000UL ? 1 : -1];
typedef char qxp_ms_od0_token_is_exact[
    QXP_MS_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL) ==
        0xd00f0000UL ? 1 : -1];
typedef char qxp_ms_v0_token_is_exact[
    QXP_MS_SOURCE(D3DSPR_INPUT, 0) == 0x90e40000UL ? 1 : -1];
typedef char qxp_ms_v1_token_is_exact[
    QXP_MS_SOURCE(D3DSPR_INPUT, 1) == 0x90e40001UL ? 1 : -1];
typedef char qxp_ms_position_usage_token_is_exact[
    (QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_POSITION) ==
        0x80000000UL ? 1 : -1];
typedef char qxp_ms_color_usage_token_is_exact[
    (QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_COLOR) ==
        0x8000000aUL ? 1 : -1];
typedef char qxp_ms_v0_declaration_token_is_exact[
    QXP_MS_DEST(D3DSPR_INPUT, 0, D3DSP_WRITEMASK_ALL) ==
        0x900f0000UL ? 1 : -1];
typedef char qxp_ms_v1_declaration_token_is_exact[
    QXP_MS_DEST(D3DSPR_INPUT, 1, D3DSP_WRITEMASK_ALL) ==
        0x900f0001UL ? 1 : -1];
typedef char qxp_ms_sampler2d_usage_token_is_exact[
    (QXP_MS_PARAMETER_TOKEN | (DWORD)D3DSTT_2D) ==
        0x90000000UL ? 1 : -1];


typedef struct d3d9_sm2_vertex {
    float x;
    float y;
    float z;
    float w;
} D3D9_SM2_VERTEX;

typedef struct d3d9_sm2_texture_vertex {
    float x;
    float y;
    float z;
    float w;
    float u;
    float v;
} D3D9_SM2_TEXTURE_VERTEX;

static HRESULT run_vs11_create_contract9(IDirect3DDevice9 *device)
{
    static const DWORD vertex_shader_code[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_DCL,
        QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_POSITION,
        QXP_MS_DEST(D3DSPR_INPUT, 0, D3DSP_WRITEMASK_ALL),
        D3DSIO_DCL,
        QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_COLOR,
        QXP_MS_DEST(D3DSPR_INPUT, 1, D3DSP_WRITEMASK_ALL),
        D3DSIO_MOV,
        QXP_MS_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                    D3DSP_WRITEMASK_ALL),
        QXP_MS_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_MOV,
        QXP_MS_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
        QXP_MS_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_END
    };
    IDirect3DVertexShader9 *shader = NULL;
    HRESULT hr;
    HRESULT cleanup_hr;

    if (!device) return E_INVALIDARG;
    hr = IDirect3DDevice9_CreateVertexShader(
        device, vertex_shader_code, &shader);
    if (SUCCEEDED(hr) && !shader) hr = E_FAIL;
    report_stage("vs11-create", hr,
                 SUCCEEDED(hr) ? "advertised-backward-profile=ok" :
                                 "advertised-backward-profile=failed");
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetVertexShader(device, shader);
        report_stage("vs11-set", hr, SUCCEEDED(hr) ? "ok" : "failed");
    }
    cleanup_hr = IDirect3DDevice9_SetVertexShader(device, NULL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    if (shader) IDirect3DVertexShader9_Release(shader);
    return hr;
}

static HRESULT run_sm2_contract9(IDirect3DDevice9 *device)
{
    static const D3DVERTEXELEMENT9 declaration_elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_POSITION, 0 },
        D3DDECL_END()
    };
    static const DWORD vertex_shader_code[] = {
        0xfffe0200UL,
        0x0200001fUL, 0x80000000UL, 0x900f0000UL,
        0x02000001UL, 0xc00f0000UL, 0x90e40000UL,
        0x0000ffffUL
    };
    static const DWORD pixel_shader_code[] = {
        0xffff0200UL,
        0x02000001UL, 0x800f0800UL, 0xa0e40000UL,
        0x0000ffffUL
    };
    static const D3D9_SM2_VERTEX vertices[] = {
        { -1.0f, -1.0f, 0.5f, 1.0f },
        { -1.0f,  3.0f, 0.5f, 1.0f },
        {  3.0f, -1.0f, 0.5f, 1.0f }
    };
    static const WORD indices[] = { 0, 1, 2 };
    static const D3DVERTEXELEMENT9 texture_declaration_elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    static const DWORD texture_vertex_shader_code[] = {
        0xfffe0200UL,
        0x0200001fUL, 0x80000000UL, 0x900f0000UL,
        0x0200001fUL, 0x80000005UL, 0x900f0001UL,
        0x02000001UL, 0xc00f0000UL, 0x90e40000UL,
        0x02000001UL, 0xe00f0000UL, 0x90e40001UL,
        0x0000ffffUL
    };
    static const DWORD texture_pixel_shader_code[] = {
        0xffff0200UL,
        0x0200001fUL, 0x80000000UL, 0xb00f0000UL,
        0x0200001fUL, 0x90000000UL, 0xa00f0800UL,
        0x03000042UL, 0x800f0000UL, 0xb0e40000UL, 0xa0e40800UL,
        0x02000001UL, 0x800f0800UL, 0x80e40000UL,
        0x0000ffffUL
    };
    static const DWORD texture_pixel_shader4_code[] = {
        0xffff0200UL,
        0x0200001fUL, 0x80000000UL, 0xb00f0000UL,
        0x0200001fUL, 0x90000000UL, 0xa00f0804UL,
        0x03000042UL, 0x800f0000UL, 0xb0e40000UL, 0xa0e40804UL,
        0x02000001UL, 0x800f0800UL, 0x80e40000UL,
        0x0000ffffUL
    };
    static const DWORD texture_pixel_shader15_code[] = {
        0xffff0200UL,
        0x0200001fUL, 0x80000000UL, 0xb00f0000UL,
        0x0200001fUL, 0x90000000UL, 0xa00f080fUL,
        0x03000042UL, 0x800f0000UL, 0xb0e40000UL, 0xa0e4080fUL,
        0x02000001UL, 0x800f0800UL, 0x80e40000UL,
        0x0000ffffUL
    };
    static const DWORD *const high_sampler_shader_code[2] = {
        texture_pixel_shader4_code, texture_pixel_shader15_code
    };
    static const DWORD high_sampler_registers[2] = { 4, 15 };
    static const DWORD high_sampler_colors[2] = {
        0xff2458a4UL, 0xffa4386cUL
    };
    static const D3D9_SM2_TEXTURE_VERTEX texture_vertices[] = {
        { -1.0f, -1.0f, 0.5f, 1.0f, 0.5f, 0.5f },
        { -1.0f,  3.0f, 0.5f, 1.0f, 0.5f, 0.5f },
        {  3.0f, -1.0f, 0.5f, 1.0f, 0.5f, 0.5f }
    };
    float pixel_constant[4] = {
        0.125f, 0.375f, 0.875f, 1.0f
    };
    IDirect3DTexture9 *target_texture = NULL;
    IDirect3DSurface9 *target = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *saved_target = NULL;
    IDirect3DSurface9 *saved_depth = NULL;
    IDirect3DVertexDeclaration9 *declaration = NULL;
    IDirect3DVertexShader9 *vertex_shader = NULL;
    IDirect3DPixelShader9 *pixel_shader = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DVertexBuffer9 *default_vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *default_index_buffer = NULL;
    IDirect3DTexture9 *sample_texture = NULL;
    IDirect3DVertexDeclaration9 *texture_declaration = NULL;
    IDirect3DVertexShader9 *texture_vertex_shader = NULL;
    IDirect3DPixelShader9 *texture_pixel_shader = NULL;
    IDirect3DTexture9 *high_sampler_textures[2] = { NULL, NULL };
    IDirect3DPixelShader9 *high_sampler_shaders[2] = { NULL, NULL };
    IDirect3DTexture9 *full_sampler_textures[
        QXP_SM2_FULL_SAMPLER_COUNT] = { NULL };
    IDirect3DPixelShader9 *full_sampler_shader = NULL;
    DWORD full_sampler_shader_code[
        QXP_SM2_FULL_SAMPLER_SHADER_DWORDS];
    DWORD *full_sampler_cursor;
    D3DVIEWPORT9 saved_viewport;
    D3DVIEWPORT9 viewport;
    D3DLOCKED_RECT locked;
    HRESULT hr;
    HRESULT cleanup_hr;
    void *locked_buffer = NULL;
    DWORD observed = 0;
    DWORD high_sampler_index;
    DWORD full_sampler_index;
    BOOL target_bound = FALSE;
    BOOL depth_detached = FALSE;
    BOOL viewport_saved = FALSE;
    BOOL scene_begun = FALSE;
    BOOL readback_locked = FALSE;
    char detail[128];
    char stage_name[64];

    if (!device) return E_INVALIDARG;
    report_stage("sm2-contract-enter", S_OK,
                 "stock-ddi-v4-vs20-ps20-draw-readback");
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    memset(&viewport, 0, sizeof(viewport));
    memset(&locked, 0, sizeof(locked));
    full_sampler_cursor = full_sampler_shader_code;
    *full_sampler_cursor++ = 0xffff0200UL;
    *full_sampler_cursor++ = 0x0200001fUL;
    *full_sampler_cursor++ = QXP_MS_PARAMETER_TOKEN;
    *full_sampler_cursor++ =
        QXP_MS_DEST(D3DSPR_TEXTURE, 0, D3DSP_WRITEMASK_ALL);
    for (full_sampler_index = 0;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_COUNT;
         ++full_sampler_index) {
        *full_sampler_cursor++ = 0x0200001fUL;
        *full_sampler_cursor++ =
            QXP_MS_PARAMETER_TOKEN | (DWORD)D3DSTT_2D;
        *full_sampler_cursor++ =
            QXP_MS_DEST(
                D3DSPR_SAMPLER, full_sampler_index,
                D3DSP_WRITEMASK_ALL);
    }
    /*
     * Match fxc's ps_2_0 schedule: stock runtimes admit independent texture
     * reads in bounded batches rather than sixteen alternating TEX/ADD phases.
     */
    for (full_sampler_index = 0;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_FIRST_BATCH;
         ++full_sampler_index) {
        *full_sampler_cursor++ = 0x03000042UL;
        *full_sampler_cursor++ =
            QXP_MS_DEST(
                D3DSPR_TEMP, full_sampler_index,
                D3DSP_WRITEMASK_ALL);
        *full_sampler_cursor++ = QXP_MS_SOURCE(D3DSPR_TEXTURE, 0);
        *full_sampler_cursor++ =
            QXP_MS_SOURCE(D3DSPR_SAMPLER, full_sampler_index);
    }
    for (full_sampler_index = 1;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_FIRST_BATCH;
         ++full_sampler_index) {
        *full_sampler_cursor++ = 0x03000002UL;
        *full_sampler_cursor++ =
            QXP_MS_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL);
        *full_sampler_cursor++ = QXP_MS_SOURCE(D3DSPR_TEMP, 0);
        *full_sampler_cursor++ =
            QXP_MS_SOURCE(D3DSPR_TEMP, full_sampler_index);
    }
    for (full_sampler_index = QXP_SM2_FULL_SAMPLER_FIRST_BATCH;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_COUNT;
         ++full_sampler_index) {
        DWORD temporary = full_sampler_index -
                          QXP_SM2_FULL_SAMPLER_FIRST_BATCH + 1;
        *full_sampler_cursor++ = 0x03000042UL;
        *full_sampler_cursor++ =
            QXP_MS_DEST(D3DSPR_TEMP, temporary, D3DSP_WRITEMASK_ALL);
        *full_sampler_cursor++ = QXP_MS_SOURCE(D3DSPR_TEXTURE, 0);
        *full_sampler_cursor++ =
            QXP_MS_SOURCE(D3DSPR_SAMPLER, full_sampler_index);
    }
    for (full_sampler_index = 1;
         full_sampler_index <=
             QXP_SM2_FULL_SAMPLER_COUNT -
                 QXP_SM2_FULL_SAMPLER_FIRST_BATCH;
         ++full_sampler_index) {
        *full_sampler_cursor++ = 0x03000002UL;
        *full_sampler_cursor++ =
            QXP_MS_DEST(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_ALL);
        *full_sampler_cursor++ = QXP_MS_SOURCE(D3DSPR_TEMP, 0);
        *full_sampler_cursor++ =
            QXP_MS_SOURCE(D3DSPR_TEMP, full_sampler_index);
    }
    *full_sampler_cursor++ = 0x02000001UL;
    *full_sampler_cursor++ =
        QXP_MS_DEST(D3DSPR_COLOROUT, 0, D3DSP_WRITEMASK_ALL);
    *full_sampler_cursor++ = QXP_MS_SOURCE(D3DSPR_TEMP, 0);
    *full_sampler_cursor++ = 0x0000ffffUL;
    if ((DWORD)(full_sampler_cursor - full_sampler_shader_code) !=
        QXP_SM2_FULL_SAMPLER_SHADER_DWORDS) {
        report_stage(
            "sm2-sampler16-build", E_FAIL, "token-count-mismatch");
        return E_FAIL;
    }
    hr = IDirect3DDevice9_CreatePixelShader(
        device, full_sampler_shader_code, &full_sampler_shader);
    report_stage(
        "sm2-sampler16-shader-create", hr,
        SUCCEEDED(hr) ? "sixteen-sampler-ps20" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTarget(device, 0, &saved_target);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetDepthStencilSurface(device, &saved_depth);
    if (hr == D3DERR_NOTFOUND) hr = S_OK;
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetViewport(device, &saved_viewport);
        if (SUCCEEDED(hr)) viewport_saved = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 16, 16, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_texture, NULL);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DTexture9_GetSurfaceLevel(target_texture, 0, &target);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
            device, 16, 16, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &readback, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexDeclaration(
            device, declaration_elements, &declaration);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexShader(
            device, vertex_shader_code, &vertex_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreatePixelShader(
            device, pixel_shader_code, &pixel_shader);
    }
    report_stage("sm2-contract-create", hr,
                 SUCCEEDED(hr) ? "declaration-vs20-ps20" : "failed");

    if (SUCCEEDED(hr) && saved_depth) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
        if (SUCCEEDED(hr)) depth_detached = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    if (SUCCEEDED(hr)) {
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = 16;
        viewport.Height = 16;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        hr = IDirect3DDevice9_SetViewport(device, &viewport);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_COLORWRITEENABLE, 0x0fUL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetVertexDeclaration(device, declaration);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetVertexShader(device, vertex_shader);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetPixelShader(device, pixel_shader);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, pixel_constant, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage("sm2-contract-draw", hr,
                 SUCCEEDED(hr) ? "triangle-vs20-ps20" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, readback);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        if (observed != 0xff2060dfUL) hr = E_FAIL;
    }
    _snprintf(detail, sizeof(detail),
              "center=%08lx expected=ff2060df",
              (unsigned long)observed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("sm2-contract-readback", hr, detail);

    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        readback_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexBuffer(
            device, sizeof(vertices), D3DUSAGE_WRITEONLY, 0,
            D3DPOOL_MANAGED, &vertex_buffer, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DVertexBuffer9_Lock(
            vertex_buffer, 0, sizeof(vertices), &locked_buffer, 0);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked_buffer, vertices, sizeof(vertices));
        hr = IDirect3DVertexBuffer9_Unlock(vertex_buffer);
        locked_buffer = NULL;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateIndexBuffer(
            device, sizeof(indices), 0, D3DFMT_INDEX16,
            D3DPOOL_MANAGED, &index_buffer, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DIndexBuffer9_Lock(
            index_buffer, 0, sizeof(indices), &locked_buffer, 0);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked_buffer, indices, sizeof(indices));
        hr = IDirect3DIndexBuffer9_Unlock(index_buffer);
        locked_buffer = NULL;
    }
    report_stage("sm2-indexed-create", hr,
                 SUCCEEDED(hr) ? "managed-vb-ib16" : "failed");

    if (SUCCEEDED(hr)) {
        pixel_constant[0] = 0.75f;
        pixel_constant[1] = 0.25f;
        pixel_constant[2] = 0.125f;
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, pixel_constant, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetStreamSource(
            device, 0, vertex_buffer, 0, sizeof(vertices[0]));
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetIndices(device, index_buffer);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawIndexedPrimitive(
            device, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage("sm2-indexed-draw", hr,
                 SUCCEEDED(hr) ? "triangle-managed-vb-ib16" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, readback);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        if (observed != 0xffbf4020UL) hr = E_FAIL;
    }
    _snprintf(detail, sizeof(detail),
              "center=%08lx expected=ffbf4020",
              (unsigned long)observed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("sm2-indexed-readback", hr, detail);

    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        readback_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    IDirect3DDevice9_SetIndices(device, NULL);
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexBuffer(
            device, sizeof(vertices),
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
            D3DPOOL_DEFAULT, &default_vertex_buffer, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DVertexBuffer9_Lock(
            default_vertex_buffer, 0, sizeof(vertices), &locked_buffer,
            D3DLOCK_DISCARD);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked_buffer, vertices, sizeof(vertices));
        hr = IDirect3DVertexBuffer9_Unlock(default_vertex_buffer);
        locked_buffer = NULL;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateIndexBuffer(
            device, sizeof(indices),
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
            D3DPOOL_DEFAULT, &default_index_buffer, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DIndexBuffer9_Lock(
            default_index_buffer, 0, sizeof(indices), &locked_buffer,
            D3DLOCK_DISCARD);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked_buffer, indices, sizeof(indices));
        hr = IDirect3DIndexBuffer9_Unlock(default_index_buffer);
        locked_buffer = NULL;
    }
    report_stage("sm2-default-create", hr,
                 SUCCEEDED(hr) ? "dynamic-default-vb-ib16" : "failed");

    if (SUCCEEDED(hr)) {
        pixel_constant[0] = 0.125f;
        pixel_constant[1] = 0.75f;
        pixel_constant[2] = 0.375f;
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, pixel_constant, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetStreamSource(
            device, 0, default_vertex_buffer, 0, sizeof(vertices[0]));
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetIndices(device, default_index_buffer);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawIndexedPrimitive(
            device, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage("sm2-default-draw", hr,
                 SUCCEEDED(hr) ?
                    "triangle-dynamic-default-vb-ib16" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, readback);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        if (observed != 0xff20bf60UL) hr = E_FAIL;
    }
    _snprintf(detail, sizeof(detail),
              "center=%08lx expected=ff20bf60",
              (unsigned long)observed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("sm2-default-readback", hr, detail);

    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        readback_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    IDirect3DDevice9_SetIndices(device, NULL);
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &sample_texture, NULL);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DTexture9_LockRect(sample_texture, 0, &locked, NULL, 0);
    if (SUCCEEDED(hr)) {
        *(DWORD *)locked.pBits = 0xff3090d0UL;
        hr = IDirect3DTexture9_UnlockRect(sample_texture, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexDeclaration(
            device, texture_declaration_elements, &texture_declaration);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateVertexShader(
            device, texture_vertex_shader_code, &texture_vertex_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreatePixelShader(
            device, texture_pixel_shader_code, &texture_pixel_shader);
    }
    report_stage("sm2-texture-create", hr,
                 SUCCEEDED(hr) ? "managed-a8r8g8b8-vs20-ps20" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)sample_texture);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetVertexDeclaration(
            device, texture_declaration);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetVertexShader(
            device, texture_vertex_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShader(
            device, texture_pixel_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture_vertices,
            sizeof(texture_vertices[0]));
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage("sm2-texture-draw", hr,
                 SUCCEEDED(hr) ? "triangle-managed-2d-sampler" : "failed");

    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, readback);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        if (observed != 0xff3090d0UL) hr = E_FAIL;
    }
    _snprintf(detail, sizeof(detail),
              "center=%08lx expected=ff3090d0",
              (unsigned long)observed);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("sm2-texture-readback", hr, detail);

    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        readback_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    for (high_sampler_index = 0;
         high_sampler_index < 2 && SUCCEEDED(hr);
         ++high_sampler_index) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &high_sampler_textures[high_sampler_index], NULL);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DTexture9_LockRect(
                high_sampler_textures[high_sampler_index], 0,
                &locked, NULL, 0);
        }
        if (SUCCEEDED(hr)) {
            *(DWORD *)locked.pBits =
                high_sampler_colors[high_sampler_index];
            hr = IDirect3DTexture9_UnlockRect(
                high_sampler_textures[high_sampler_index], 0);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_CreatePixelShader(
                device, high_sampler_shader_code[high_sampler_index],
                &high_sampler_shaders[high_sampler_index]);
        }
        _snprintf(
            stage_name, sizeof(stage_name), "sm2-s%lu-create",
            (unsigned long)high_sampler_registers[high_sampler_index]);
        stage_name[sizeof(stage_name) - 1] = '\0';
        report_stage(
            stage_name, hr,
            SUCCEEDED(hr) ? "managed-a8r8g8b8-ps20" : "failed");

        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTexture(
                device, high_sampler_registers[high_sampler_index],
                (IDirect3DBaseTexture9 *)
                    high_sampler_textures[high_sampler_index]);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, high_sampler_registers[high_sampler_index],
                D3DSAMP_MINFILTER, D3DTEXF_POINT);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, high_sampler_registers[high_sampler_index],
                D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, high_sampler_registers[high_sampler_index],
                D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, high_sampler_registers[high_sampler_index],
                D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, high_sampler_registers[high_sampler_index],
                D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetPixelShader(
                device, high_sampler_shaders[high_sampler_index]);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_Clear(
                device, 0, NULL, D3DCLEAR_TARGET,
                0xff000000UL, 1.0f, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_BeginScene(device);
            if (SUCCEEDED(hr)) scene_begun = TRUE;
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_DrawPrimitiveUP(
                device, D3DPT_TRIANGLELIST, 1, texture_vertices,
                sizeof(texture_vertices[0]));
        }
        if (scene_begun) {
            cleanup_hr = IDirect3DDevice9_EndScene(device);
            scene_begun = FALSE;
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
        _snprintf(
            stage_name, sizeof(stage_name), "sm2-s%lu-draw",
            (unsigned long)high_sampler_registers[high_sampler_index]);
        stage_name[sizeof(stage_name) - 1] = '\0';
        report_stage(
            stage_name, hr,
            SUCCEEDED(hr) ? "triangle-high-sampler" : "failed");

        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_GetRenderTargetData(
                device, target, readback);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DSurface9_LockRect(
                readback, &locked, NULL, D3DLOCK_READONLY);
            if (SUCCEEDED(hr)) readback_locked = TRUE;
        }
        if (SUCCEEDED(hr)) {
            observed = *(const DWORD *)(
                (const BYTE *)locked.pBits + 8 * locked.Pitch +
                8 * sizeof(DWORD));
            if (observed != high_sampler_colors[high_sampler_index])
                hr = E_FAIL;
        }
        _snprintf(
            detail, sizeof(detail),
            "s%lu-center=%08lx expected=%08lx",
            (unsigned long)high_sampler_registers[high_sampler_index],
            (unsigned long)observed,
            (unsigned long)high_sampler_colors[high_sampler_index]);
        detail[sizeof(detail) - 1] = '\0';
        _snprintf(
            stage_name, sizeof(stage_name), "sm2-s%lu-readback",
            (unsigned long)high_sampler_registers[high_sampler_index]);
        stage_name[sizeof(stage_name) - 1] = '\0';
        report_stage(stage_name, hr, detail);
        if (readback_locked) {
            cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
            readback_locked = FALSE;
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
        cleanup_hr = IDirect3DDevice9_SetTexture(
            device, high_sampler_registers[high_sampler_index], NULL);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    for (full_sampler_index = 0;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_COUNT &&
         SUCCEEDED(hr);
         ++full_sampler_index) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &full_sampler_textures[full_sampler_index], NULL);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DTexture9_LockRect(
                full_sampler_textures[full_sampler_index], 0,
                &locked, NULL, 0);
        }
        if (SUCCEEDED(hr)) {
            *(DWORD *)locked.pBits =
                full_sampler_index + 1 ==
                    QXP_SM2_FULL_SAMPLER_COUNT ?
                    0xff002040UL : 0xff110000UL;
            hr = IDirect3DTexture9_UnlockRect(
                full_sampler_textures[full_sampler_index], 0);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTexture(
                device, full_sampler_index,
                (IDirect3DBaseTexture9 *)
                    full_sampler_textures[full_sampler_index]);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, full_sampler_index,
                D3DSAMP_MINFILTER, D3DTEXF_POINT);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, full_sampler_index,
                D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, full_sampler_index,
                D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, full_sampler_index,
                D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetSamplerState(
                device, full_sampler_index,
                D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
    }
    report_stage(
        "sm2-sampler16-create", hr,
        SUCCEEDED(hr) ?
            "sixteen-independent-managed-2d-bindings" : "failed");
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShader(
            device, full_sampler_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture_vertices,
            sizeof(texture_vertices[0]));
    }
    if (scene_begun) {
        cleanup_hr = IDirect3DDevice9_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    report_stage(
        "sm2-sampler16-draw", hr,
        SUCCEEDED(hr) ? "triangle-sixteen-samplers" : "failed");

    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetRenderTargetData(
            device, target, readback);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DSurface9_LockRect(
            readback, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) readback_locked = TRUE;
    }
    if (SUCCEEDED(hr)) {
        observed = *(const DWORD *)(
            (const BYTE *)locked.pBits + 8 * locked.Pitch +
            8 * sizeof(DWORD));
        if (observed != QXP_SM2_FULL_SAMPLER_EXPECTED) hr = E_FAIL;
    }
    _snprintf(
        detail, sizeof(detail),
        "all-mask=ffff center=%08lx expected=%08lx",
        (unsigned long)observed,
        (unsigned long)QXP_SM2_FULL_SAMPLER_EXPECTED);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("sm2-sampler16-readback", hr, detail);
    if (readback_locked) {
        cleanup_hr = IDirect3DSurface9_UnlockRect(readback);
        readback_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    for (full_sampler_index = 0;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_COUNT;
         ++full_sampler_index) {
        cleanup_hr = IDirect3DDevice9_SetTexture(
            device, full_sampler_index, NULL);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetPixelShader(device, NULL);
    IDirect3DDevice9_SetVertexShader(device, NULL);
    IDirect3DDevice9_SetVertexDeclaration(device, NULL);
    if (target_bound && saved_target) {
        cleanup_hr = IDirect3DDevice9_SetRenderTarget(
            device, 0, saved_target);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (depth_detached && saved_depth) {
        cleanup_hr = IDirect3DDevice9_SetDepthStencilSurface(
            device, saved_depth);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (viewport_saved) {
        cleanup_hr = IDirect3DDevice9_SetViewport(device, &saved_viewport);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    for (high_sampler_index = 0; high_sampler_index < 2;
         ++high_sampler_index) {
        if (high_sampler_shaders[high_sampler_index]) {
            IDirect3DPixelShader9_Release(
                high_sampler_shaders[high_sampler_index]);
        }
        if (high_sampler_textures[high_sampler_index]) {
            IDirect3DTexture9_Release(
                high_sampler_textures[high_sampler_index]);
        }
    }
    if (full_sampler_shader)
        IDirect3DPixelShader9_Release(full_sampler_shader);
    for (full_sampler_index = 0;
         full_sampler_index < QXP_SM2_FULL_SAMPLER_COUNT;
         ++full_sampler_index) {
        if (full_sampler_textures[full_sampler_index]) {
            IDirect3DTexture9_Release(
                full_sampler_textures[full_sampler_index]);
        }
    }
    if (texture_pixel_shader)
        IDirect3DPixelShader9_Release(texture_pixel_shader);
    if (texture_vertex_shader)
        IDirect3DVertexShader9_Release(texture_vertex_shader);
    if (texture_declaration)
        IDirect3DVertexDeclaration9_Release(texture_declaration);
    if (sample_texture) IDirect3DTexture9_Release(sample_texture);
    if (default_index_buffer)
        IDirect3DIndexBuffer9_Release(default_index_buffer);
    if (default_vertex_buffer)
        IDirect3DVertexBuffer9_Release(default_vertex_buffer);
    if (index_buffer) IDirect3DIndexBuffer9_Release(index_buffer);
    if (vertex_buffer) IDirect3DVertexBuffer9_Release(vertex_buffer);
    if (pixel_shader) IDirect3DPixelShader9_Release(pixel_shader);
    if (vertex_shader) IDirect3DVertexShader9_Release(vertex_shader);
    if (declaration)
        IDirect3DVertexDeclaration9_Release(declaration);
    if (readback) IDirect3DSurface9_Release(readback);
    if (target) IDirect3DSurface9_Release(target);
    if (target_texture) IDirect3DTexture9_Release(target_texture);
    if (saved_depth) IDirect3DSurface9_Release(saved_depth);
    if (saved_target) IDirect3DSurface9_Release(saved_target);
    report_stage("sm2-contract-leave", hr,
                 SUCCEEDED(hr) ? "exact-pixel=ok" : "failed");
    return hr;
}

typedef struct d3d9_multistream7_state {
    IDirect3DVertexBuffer9 *position_buffer;
    IDirect3DVertexBuffer9 *color_buffer;
    IDirect3DVertexDeclaration9 *declaration;
    IDirect3DVertexShader9 *shader;
    IDirect3DPixelShader9 *pixel_shader;
} D3D9_MULTISTREAM7_STATE;

static void release_multistream7_state(D3D9_MULTISTREAM7_STATE *state)
{
    if (!state) return;
    if (state->pixel_shader)
        IDirect3DPixelShader9_Release(state->pixel_shader);
    if (state->shader) IDirect3DVertexShader9_Release(state->shader);
    if (state->declaration)
        IDirect3DVertexDeclaration9_Release(state->declaration);
    if (state->color_buffer)
        IDirect3DVertexBuffer9_Release(state->color_buffer);
    if (state->position_buffer)
        IDirect3DVertexBuffer9_Release(state->position_buffer);
    memset(state, 0, sizeof(*state));
}

static void write_dword(void *destination, DWORD value)
{
    memcpy(destination, &value, sizeof(value));
}

static HRESULT prepare_multistream7(
    IDirect3DDevice9 *device, const D3DDISPLAYMODE *mode,
    D3D9_MULTISTREAM7_STATE *state, const char **failure_stage)
{
    static const D3DVERTEXELEMENT9 declaration[] = {
        { 0, QXP_MS_POSITION_ELEMENT_OFFSET, D3DDECLTYPE_FLOAT3,
          D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 7, QXP_MS_COLOR_ELEMENT_OFFSET, D3DDECLTYPE_D3DCOLOR,
          D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    static const DWORD vertex_shader_code[] = {
        D3DVS_VERSION(2, 0),
        0x0200001fUL,
        QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_POSITION,
        QXP_MS_DEST(D3DSPR_INPUT, 0, D3DSP_WRITEMASK_ALL),
        0x0200001fUL,
        QXP_MS_PARAMETER_TOKEN | D3DDECLUSAGE_COLOR,
        QXP_MS_DEST(D3DSPR_INPUT, 1, D3DSP_WRITEMASK_ALL),
        0x02000001UL,
        QXP_MS_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                    D3DSP_WRITEMASK_ALL),
        QXP_MS_SOURCE(D3DSPR_INPUT, 0),
        0x02000001UL,
        QXP_MS_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
        QXP_MS_SOURCE(D3DSPR_INPUT, 1),
        D3DSIO_END
    };
    static const DWORD pixel_shader_code[] = {
        D3DPS_VERSION(2, 0),
        0x0200001fUL,
        QXP_MS_PARAMETER_TOKEN,
        QXP_MS_DEST(D3DSPR_INPUT, 0, D3DSP_WRITEMASK_ALL),
        0x02000001UL,
        QXP_MS_DEST(D3DSPR_COLOROUT, 0, D3DSP_WRITEMASK_ALL),
        QXP_MS_SOURCE(D3DSPR_INPUT, 0),
        D3DSIO_END
    };
    static const float screen_positions[3][2] = {
        { 365.0f, 170.0f }, { 395.0f, 170.0f }, { 380.0f, 195.0f }
    };
    BYTE position_bytes[3 * QXP_MS_POSITION_STRIDE];
    BYTE color_bytes[3 * QXP_MS_COLOR_STRIDE];
    BYTE *record;
    void *locked;
    float position[3];
    HRESULT hr;
    UINT index;

    if (failure_stage) *failure_stage = "multistream7-arguments";
    if (!device || !mode || !mode->Width || !mode->Height || !state)
        return E_INVALIDARG;
    memset(state, 0, sizeof(*state));
    memset(position_bytes, 0x7f, sizeof(position_bytes));
    memset(color_bytes, 0x5a, sizeof(color_bytes));
    for (index = 0; index < 3; ++index) {
        position[0] = 2.0f * screen_positions[index][0] /
                      (float)mode->Width - 1.0f;
        position[1] = 1.0f - 2.0f * screen_positions[index][1] /
                      (float)mode->Height;
        position[2] = 0.25f;
        record = position_bytes + index * QXP_MS_POSITION_STRIDE;
        write_dword(record, 0xff1040f0UL);
        write_dword(record + 12, 0xffd020e0UL);
        memcpy(record + QXP_MS_POSITION_ELEMENT_OFFSET,
               position, sizeof(position));
        write_dword(record + 28, 0xff1040f0UL);

        record = color_bytes + index * QXP_MS_COLOR_STRIDE;
        write_dword(record, 0xffff00ffUL);
        write_dword(record + QXP_MS_COLOR_ELEMENT_OFFSET,
                    QXP_MS_ORACLE_COLOR);
        write_dword(record + 8, 0xffe02020UL);
    }

    if (failure_stage) *failure_stage = "multistream7-create-stream0";
    hr = IDirect3DDevice9_CreateVertexBuffer(
        device, sizeof(position_bytes), D3DUSAGE_WRITEONLY, 0,
        D3DPOOL_MANAGED, &state->position_buffer, NULL);
    locked = NULL;
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage = "multistream7-lock-stream0";
        hr = IDirect3DVertexBuffer9_Lock(
            state->position_buffer, 0, sizeof(position_bytes), &locked, 0);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked, position_bytes, sizeof(position_bytes));
        if (failure_stage) *failure_stage = "multistream7-unlock-stream0";
        hr = IDirect3DVertexBuffer9_Unlock(state->position_buffer);
    }
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage = "multistream7-create-stream7";
        hr = IDirect3DDevice9_CreateVertexBuffer(
            device, sizeof(color_bytes), D3DUSAGE_WRITEONLY, 0,
            D3DPOOL_MANAGED, &state->color_buffer, NULL);
    }
    locked = NULL;
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage = "multistream7-lock-stream7";
        hr = IDirect3DVertexBuffer9_Lock(
            state->color_buffer, 0, sizeof(color_bytes), &locked, 0);
    }
    if (SUCCEEDED(hr)) {
        memcpy(locked, color_bytes, sizeof(color_bytes));
        if (failure_stage) *failure_stage = "multistream7-unlock-stream7";
        hr = IDirect3DVertexBuffer9_Unlock(state->color_buffer);
    }
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage = "multistream7-create-declaration";
        hr = IDirect3DDevice9_CreateVertexDeclaration(
            device, declaration, &state->declaration);
    }
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage =
            "multistream7-create-vertex-shader";
        hr = IDirect3DDevice9_CreateVertexShader(
            device, vertex_shader_code, &state->shader);
    }
    if (SUCCEEDED(hr)) {
        if (failure_stage) *failure_stage =
            "multistream7-create-pixel-shader";
        hr = IDirect3DDevice9_CreatePixelShader(
            device, pixel_shader_code, &state->pixel_shader);
    }
    if (FAILED(hr)) {
        release_multistream7_state(state);
    } else if (failure_stage) {
        *failure_stage = "multistream7-ready";
    }
    return hr;
}

static HRESULT draw_multistream7(
    IDirect3DDevice9 *device, D3D9_MULTISTREAM7_STATE *state)
{
    HRESULT hr;

    if (!device || !state || !state->position_buffer ||
        !state->color_buffer || !state->declaration || !state->shader ||
        !state->pixel_shader)
        return E_INVALIDARG;
    hr = IDirect3DDevice9_SetPixelShader(device, state->pixel_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_ZENABLE, D3DZB_FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_ZWRITEENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_STENCILENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_ALPHATESTENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_SCISSORTESTENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderState(
        device, D3DRS_COLORWRITEENABLE, 0x0fUL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTextureStageState(
        device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetVertexDeclaration(
        device, state->declaration);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetVertexShader(
        device, state->shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetStreamSource(
        device, 0, state->position_buffer, 0, QXP_MS_POSITION_STRIDE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetStreamSource(
        device, 7, state->color_buffer, 0, QXP_MS_COLOR_STRIDE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_DrawPrimitive(
        device, D3DPT_TRIANGLELIST, 0, 1);
    return hr;
}

static HRESULT release_multistream7(
    IDirect3DDevice9 *device, D3D9_MULTISTREAM7_STATE *state,
    int *released)
{
    HRESULT hr;
    HRESULT cleanup_hr;

    if (released) *released = 0;
    if (!device || !state || !state->position_buffer ||
        !state->color_buffer || !state->declaration || !state->shader ||
        !state->pixel_shader)
        return E_INVALIDARG;
    hr = IDirect3DDevice9_SetStreamSource(device, 7, NULL, 0, 0);
    cleanup_hr = IDirect3DDevice9_SetStreamSource(
        device, 0, NULL, 0, 0);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice9_SetVertexShader(device, NULL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice9_SetFVF(
        device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    release_multistream7_state(state);
    if (released && SUCCEEDED(hr)) *released = 1;
    return hr;
}

int main(int argc, char **argv)
{
    static const DWORD vertex_fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    static const DWORD texture_vertex_fvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    static const DWORD bump_vertex_fvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2;
    static const DWORD matrix_vertex_fvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX3;
    static const DWORD four_texture_vertex_fvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX4;
    static const DWORD cube_vertex_fvf =
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX4 |
        D3DFVF_TEXCOORDSIZE3(1) | D3DFVF_TEXCOORDSIZE3(2) |
        D3DFVF_TEXCOORDSIZE3(3);
    /* Minimal ps_2_0: mov oC0, c0.  Only attempted when the driver actually
     * advertises Shader Model 2, so this stays inert while ps_1_3 is
     * reported.  Destination oC0 encodes D3DSPR_COLOROUT (8) as low bits 0 in
     * 28-30 and high bit 1 in 11-12, with a full write mask. */
    static const DWORD ps20_shader_code[] = {
        D3DPS_VERSION(2, 0),
        D3DSIO_MOV | (2UL << 24),
        0x800f0800UL,
        0xa0e40000UL,
        D3DSIO_END
    };
    static const DWORD pixel_shader_code[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX,
        0xb00f0000UL,
        D3DSIO_ADD,
        0x800f0000UL,
        0xb0e40000UL,
        0xa0e40000UL,
        D3DSIO_END
    };
    static const DWORD fourth_texture_pixel_shader_code[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX,
        0xb00f0003UL,
        D3DSIO_MOV,
        0x800f0000UL,
        0xb0e40003UL,
        D3DSIO_END
    };

    static const DWORD cube_face_colors[3][6] = {
        { 0xff204060UL, 0xffffff00UL, 0xffff0000UL,
          0xff00ff00UL, 0xff0000ffUL, 0xffffffffUL },
        { 0xff408020UL, 0xffffc000UL, 0xffff4000UL,
          0xff00c040UL, 0xff0040c0UL, 0xffc0c0c0UL },
        { 0xff8040ffUL, 0xff808000UL, 0xff800000UL,
          0xff008000UL, 0xff000080UL, 0xff808080UL }
    };
    static const DWORD mip_texture_colors[3] = {
        0xffe04020UL, 0xff20e040UL, 0xff40a0e0UL
    };
    D3D9_PROBE_VERTEX vertices[3] = {
        { 40.0f, 80.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 360.0f, 80.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 200.0f, 400.0f, 0.25f, 1.0f, 0xffff8000UL }
    };
    D3D9_TEXTURE_VERTEX texture_vertices[4] = {
        { 400.0f, 200.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 0.0f },
        { 600.0f, 200.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 0.0f },
        { 400.0f, 400.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 1.0f },
        { 600.0f, 400.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 1.0f }
    };
    D3D9_TEXTURE_VERTEX shader_vertices[3] = {
        { 250.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 0.0f },
        { 390.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 0.0f },
        { 320.0f, 470.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 1.0f }
    };
    D3D9_BUMP_VERTEX bump_vertices[3] = {
        { 420.0f, 40.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.5f },
        { 600.0f, 40.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.5f },
        { 510.0f, 160.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.5f }
    };
    D3D9_BUMP_VERTEX luminance_bump_vertices[3] = {
        { 420.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f },
        { 600.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f },
        { 510.0f, 470.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f }
    };
    D3D9_MATRIX_VERTEX matrix_vertices[3] = {
        { 40.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 0.5f },
        { 220.0f, 410.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 0.5f },
        { 130.0f, 470.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 0.5f }
    };
    D3D9_MATRIX_VERTEX register_vertices[3] = {
        { 80.0f, 100.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 160.0f, 100.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 120.0f, 160.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f }
    };
    D3D9_FOUR_TEXTURE_VERTEX fourth_texture_vertices[3] = {
        { 250.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 390.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 320.0f, 70.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f }
    };
    D3D9_FOUR_TEXTURE_VERTEX fixed_fourth_vertices[3] = {
        { 170.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 230.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
        { 200.0f, 70.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f }
    };
    D3D9_CUBE_VERTEX cube_vertices[3] = {
        { 20.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f,
          1.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 1.0f },
        { 140.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f,
          1.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 1.0f },
        { 80.0f, 70.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f,
          1.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 1.0f }
    };
    D3D9_BUMP_VERTEX pixel_shader_12_vertices[3] = {
        { 605.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f, 0.0f, 0.0f },
        { 635.0f, 20.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f, 0.0f, 0.0f },
        { 620.0f, 60.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.25f, 0.5f, 0.0f, 0.0f }
    };
    D3D9_PROBE_VERTEX depth_background_vertices[3] = {
        { 605.0f, 100.0f, 0.5f, 1.0f, 0xffff0000UL },
        { 635.0f, 100.0f, 0.5f, 1.0f, 0xffff0000UL },
        { 620.0f, 140.0f, 0.5f, 1.0f, 0xffff0000UL }
    };
    D3D9_MATRIX_VERTEX pixel_shader_13_depth_vertices[3] = {
        { 605.0f, 100.0f, 0.9f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 1.0f },
        { 635.0f, 100.0f, 0.9f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 1.0f },
        { 620.0f, 140.0f, 0.9f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.0f, 0.25f, 0.0f, 1.0f }
    };
    D3D9_PROBE_VERTEX render_target_no_depth_vertices[3] = {
        { 0.0f, 0.0f, 0.25f, 1.0f, 0xff20c060UL },
        { 32.0f, 0.0f, 0.25f, 1.0f, 0xff20c060UL },
        { 0.0f, 32.0f, 0.25f, 1.0f, 0xff20c060UL }
    };
    D3D9_PROBE_VERTEX render_target_front_vertices[3] = {
        { 32.0f, 32.0f, 0.25f, 1.0f, 0xffe0a020UL },
        { 64.0f, 32.0f, 0.25f, 1.0f, 0xffe0a020UL },
        { 32.0f, 64.0f, 0.25f, 1.0f, 0xffe0a020UL }
    };
    D3D9_PROBE_VERTEX render_target_back_vertices[3] = {
        { 32.0f, 32.0f, 0.75f, 1.0f, 0xffff0000UL },
        { 64.0f, 32.0f, 0.75f, 1.0f, 0xffff0000UL },
        { 32.0f, 64.0f, 0.75f, 1.0f, 0xffff0000UL }
    };
    D3D9_PROBE_VERTEX destination_alpha_vertices[6] = {
        { 32.0f, 0.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 32.0f, 32.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 64.0f, 32.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 0.0f, 32.0f, 0.25f, 1.0f, 0xff00ff00UL },
        { 32.0f, 32.0f, 0.25f, 1.0f, 0xff00ff00UL },
        { 0.0f, 64.0f, 0.25f, 1.0f, 0xff00ff00UL }
    };
    D3D9_TEXTURE_VERTEX render_target_sample_vertices[4] = {
        { 605.0f, 165.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 0.0f },
        { 635.0f, 165.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 0.0f },
        { 605.0f, 225.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 1.0f },
        { 635.0f, 225.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 1.0f }
    };
    D3D9_TEXTURE_VERTEX mip_texture_vertices[4] = {
        { 60.0f, 200.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 90.0f, 200.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 60.0f, 230.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 90.0f, 230.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f }
    };
    D3D9_TEXTURE_VERTEX dynamic_texture_triangles[6] = {
        { 20.0f, 200.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 45.0f, 200.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 20.0f, 225.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 20.0f, 240.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 45.0f, 240.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 20.0f, 265.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f }
    };
    float shader_constants[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    float pixel_shader_12_constants[4][4] = {
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { -0.25f, -0.25f, -0.25f, -0.25f }
    };
    float pixel_shader_13_depth_constants[4] = {
        1.0f, 1.0f, 0.0f, 1.0f
    };
    WNDCLASSA window_class;
    D3DADAPTER_IDENTIFIER9 identifier;
    D3DCAPS9 caps;
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS present;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DTexture9 *bump_texture = NULL;
    IDirect3DTexture9 *luminance_bump_texture = NULL;
    IDirect3DTexture9 *bump_target_texture = NULL;
    IDirect3DTexture9 *register_source_texture = NULL;
    IDirect3DTexture9 *fourth_texture = NULL;
    IDirect3DTexture9 *mip_texture = NULL;
    IDirect3DTexture9 *dynamic_texture = NULL;
    IDirect3DTexture9 *render_target_texture = NULL;
    IDirect3DCubeTexture9 *cube_texture = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DSurface9 *backdepth = NULL;
    IDirect3DSurface9 *render_target_surface = NULL;
    IDirect3DSurface9 *render_depth = NULL;
    IDirect3DVertexBuffer9 *register_vertex_buffer = NULL;
    IDirect3DPixelShader9 *pixel_shader = NULL;
    IDirect3DPixelShader9 *bump_pixel_shader = NULL;
    IDirect3DPixelShader9 *luminance_bump_pixel_shader = NULL;
    IDirect3DPixelShader9 *matrix_pixel_shader = NULL;
    IDirect3DPixelShader9 *register_pixel_shader = NULL;
    IDirect3DPixelShader9 *fourth_texture_pixel_shader = NULL;
    IDirect3DPixelShader9 *cube_pixel_shader = NULL;
    IDirect3DPixelShader9 *pixel_shader_12 = NULL;
    IDirect3DPixelShader9 *pixel_shader_13_depth = NULL;
    D3DLOCKED_RECT locked;
    D3DVIEWPORT9 main_viewport;
    D3DVIEWPORT9 render_viewport;
    D3D9_MULTISTREAM7_STATE multistream7_state;
    const char *multistream7_stage = "multistream7-ready";
    void *vertex_data = NULL;
    HMODULE module = NULL;
    HWND window = NULL;
    D3D9_ENTRY_POINT entry;
    PFN_DIRECT3DCREATE9 create_d3d9;
    char system_path[MAX_PATH];
    char module_path[MAX_PATH];
    char detail[320];
    HRESULT hr = E_FAIL;
    UINT system_length;
    DWORD x;
    DWORD y;
    DWORD cube_face;
    DWORD cube_level;
    DWORD cube_edge;
    DWORD cube_x;
    DWORD cube_y;
    UINT fmt_mode_count;
    UINT fmt_mode_count16;
    HRESULT fmt_type_hr;
    HRESULT fmt_tex_hr;
    HRESULT fmt_rt_hr;
    HRESULT fmt_surf_hr;
    DWORD behavior_flags;
    int argument_index;
    int capture_handshake = 0;
    int multistream7_required = 0;
    int multistream7_executed = 0;
    int multistream7_released = 0;
    int rc = 0;

    memset(&multistream7_state, 0, sizeof(multistream7_state));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (argument_index = 1; argument_index < argc; ++argument_index) {
        if (!strcmp(argv[argument_index], "--capture-handshake")) {
            capture_handshake = 1;
        } else if (!strcmp(argv[argument_index], "--multistream7")) {
            multistream7_required = 1;
        } else {
            return fail_probe(1, E_INVALIDARG, "arguments",
                              argv[argument_index]);
        }
    }
    report_stage("start", S_OK, "");
    system_length = GetSystemDirectoryA(system_path, sizeof(system_path));
    if (!system_length || system_length >= sizeof(system_path) ||
        system_length + sizeof("\\d3d9.dll") > sizeof(system_path)) {
        return fail_probe(1, HRESULT_FROM_WIN32(GetLastError()),
                          "module", "system-directory");
    }
    strcat(system_path, "\\d3d9.dll");
    module = LoadLibraryA(system_path);
    if (!module || !GetModuleFileNameA(module, module_path,
                                       sizeof(module_path))) {
        return fail_probe(1, HRESULT_FROM_WIN32(GetLastError()),
                          "module", "system-d3d9-load");
    }
    module_path[sizeof(module_path) - 1] = '\0';
    if (lstrcmpiA(module_path, system_path)) {
        _snprintf(detail, sizeof(detail), "module=%s", module_path);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(1, E_FAIL, "contaminated", detail);
        goto done;
    }
    report_stage("module", S_OK, module_path);
    entry.proc = GetProcAddress(module, "Direct3DCreate9");
    create_d3d9 = entry.create;
    if (!create_d3d9) {
        rc = fail_probe(2, HRESULT_FROM_WIN32(GetLastError()),
                        "module", "Direct3DCreate9-export");
        goto done;
    }

    d3d = create_d3d9(D3D_SDK_VERSION);
    report_stage("create9", d3d ? S_OK : E_FAIL,
                 d3d ? "ok" : "null");
    if (!d3d) {
        rc = fail_probe(3, E_FAIL, "create9", "module=system32");
        goto done;
    }
    if (!IDirect3D9_GetAdapterCount(d3d)) {
        rc = fail_probe(4, D3DERR_NOTAVAILABLE, "adapters", "count=0");
        goto done;
    }

    memset(&identifier, 0, sizeof(identifier));
    hr = IDirect3D9_GetAdapterIdentifier(
        d3d, D3DADAPTER_DEFAULT, 0, &identifier);
    _snprintf(detail, sizeof(detail), "adapter=%s driver=%s",
              identifier.Description, identifier.Driver);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("identifier", hr, detail);
    if (FAILED(hr)) {
        rc = fail_probe(5, hr, "identifier", detail);
        goto done;
    }

    /* Observe the runtime's adapter format-operations list using public APIs
     * only.  GetDeviceCaps(HAL) rejects the adapter when no entry in that
     * list carries D3DFORMAT_OP_3DACCELERATION, and the list is built
     * all-or-nothing, so "never populated" and "rejected during validation"
     * are indistinguishable from the driver side.  This stage is
     * observational: it is reported and never fails the probe.
     *
     * Note that a CheckDeviceFormat failure is NOT by itself evidence that the
     * list is unpopulated.  Decoding it (0x4fdf88d0) shows it dispatches on
     * resource type through a jump table and applies per-capability tests --
     * one arm at 0x4fdf8ada rejects on a D3DCAPS9.TextureCaps bit before the
     * format list matters.  Only the most permissive query below, a plain
     * surface with no usage flags, is a fair test of list population. */
    fmt_mode_count = IDirect3D9_GetAdapterModeCount(
        d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    /* The runtime recomputes each format's D3DFORMAT_OP_DISPLAYMODE bit from
     * the real mode list, then rejects any record that claims
     * D3DFORMAT_OP_3DACCELERATION without DISPLAYMODE.  A format we advertise
     * as an accelerated display format but which enumerates zero modes would
     * therefore poison the whole operation-format list. */
    fmt_mode_count16 = IDirect3D9_GetAdapterModeCount(
        d3d, D3DADAPTER_DEFAULT, D3DFMT_R5G6B5);
    fmt_type_hr = IDirect3D9_CheckDeviceType(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
        D3DFMT_X8R8G8B8, FALSE);
    fmt_tex_hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
        0, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);
    fmt_rt_hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_R5G6B5,
        D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_R5G6B5);
    /* The discriminator: no usage flags and the plainest resource type, so no
     * capability arm can reject it and only list membership decides.  Success
     * under v4 means the list is populated and the defect is the acceleration
     * bit being lost during validation; failure under v4 with success under v3
     * is consistent with the list never being stored. */
    fmt_surf_hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
        0, D3DRTYPE_SURFACE, D3DFMT_X8R8G8B8);
    _snprintf(detail, sizeof(detail),
              "modecount=%lu modecount16=%lu devtype=0x%08lx tex=0x%08lx "
              "rt=0x%08lx surf=0x%08lx",
              (unsigned long)fmt_mode_count,
              (unsigned long)fmt_mode_count16,
              (unsigned long)fmt_type_hr,
              (unsigned long)fmt_tex_hr,
              (unsigned long)fmt_rt_hr,
              (unsigned long)fmt_surf_hr);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("format-list", S_OK, detail);

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D9_GetDeviceCaps(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    _snprintf(detail, sizeof(detail),
              "caps2=%08lx devcaps=%08lx vs=%08lx ps=%08lx rts=%lu "
              "streams=%lu stride=%lu srcblend=%08lx dstblend=%08lx",
              (unsigned long)caps.Caps2,
              (unsigned long)caps.DevCaps,
              (unsigned long)caps.VertexShaderVersion,
              (unsigned long)caps.PixelShaderVersion,
              (unsigned long)caps.NumSimultaneousRTs,
              (unsigned long)caps.MaxStreams,
              (unsigned long)caps.MaxStreamStride,
              (unsigned long)caps.SrcBlendCaps,
              (unsigned long)caps.DestBlendCaps);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("hal-caps", hr, detail);
    if (SUCCEEDED(hr)) {
        /* Dump the whole D3DCAPS9 the runtime produced.  When the driver is
         * accepted this is a known-good capability block authored by the
         * runtime itself, which is the only reference oracle available for
         * what a Direct3D 9 capability block must look like here. */
        const DWORD *caps_words = (const DWORD *)&caps;
        unsigned int caps_index;
        unsigned int caps_count =
            (unsigned int)(sizeof(caps) / sizeof(DWORD));

        for (caps_index = 0; caps_index < caps_count; caps_index += 8) {
            char line[128];
            unsigned int column;
            int used = 0;

            used += _snprintf(line + used, sizeof(line) - used,
                              "caps9[%02u]", caps_index);
            for (column = 0;
                 column < 8 && caps_index + column < caps_count;
                 ++column) {
                used += _snprintf(line + used, sizeof(line) - used,
                                  " %08lx",
                                  (unsigned long)caps_words[
                                      caps_index + column]);
            }
            line[sizeof(line) - 1] = '\0';
            report_stage("caps9-dump", 0, line);
        }
    }
    if (FAILED(hr) || caps.NumSimultaneousRTs < 1 ||
        !(caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES) ||
        !(caps.SrcBlendCaps & D3DPBLENDCAPS_DESTALPHA) ||
        !(caps.SrcBlendCaps & D3DPBLENDCAPS_INVDESTALPHA) ||
        !(caps.DestBlendCaps & D3DPBLENDCAPS_ZERO) ||
        caps.PixelShaderVersion < D3DPS_VERSION(1, 3) ||
        (caps.TextureCaps &
         (D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP)) !=
        (D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP) ||
        !(caps.CubeTextureFilterCaps & D3DPTFILTERCAPS_MINFPOINT) ||
        !(caps.CubeTextureFilterCaps & D3DPTFILTERCAPS_MAGFPOINT)) {
        rc = fail_probe(6, FAILED(hr) ? hr : D3DERR_NOTAVAILABLE,
                        "hal-caps", detail);
        goto done;
    }

    if (multistream7_required &&
        (caps.MaxStreams < 8 || caps.MaxStreamStride < QXP_MS_POSITION_STRIDE ||
         caps.VertexShaderVersion < D3DVS_VERSION(1, 1))) {
        _snprintf(detail, sizeof(detail),
                  "streams=%lu stride=%lu vs=%08lx",
                  (unsigned long)caps.MaxStreams,
                  (unsigned long)caps.MaxStreamStride,
                  (unsigned long)caps.VertexShaderVersion);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE,
                        "multistream7-caps", detail);
        goto done;
    }

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    _snprintf(detail, sizeof(detail), "format=%u size=%ux%u",
              (unsigned)mode.Format, (unsigned)mode.Width,
              (unsigned)mode.Height);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("display-mode", hr, detail);
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "display-mode", detail);
        goto done;
    }

    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        D3DUSAGE_QUERY_LEGACYBUMPMAP, D3DRTYPE_TEXTURE, D3DFMT_V8U8);
    report_stage("check-v8u8", hr,
                 SUCCEEDED(hr) ? "legacy-bumpmap=1" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-v8u8", "D3DFMT_V8U8");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        D3DUSAGE_QUERY_LEGACYBUMPMAP, D3DRTYPE_TEXTURE, D3DFMT_L6V5U5);
    report_stage("check-l6v5u5", hr,
                 SUCCEEDED(hr) ? "legacy-luminance-bumpmap=1" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-l6v5u5", "D3DFMT_L6V5U5");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        0, D3DRTYPE_CUBETEXTURE, D3DFMT_A8R8G8B8);
    report_stage("check-cubemap", hr,
                 SUCCEEDED(hr) ? "a8r8g8b8-mipmapped" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-cubemap", "D3DFMT_A8R8G8B8");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        D3DUSAGE_DYNAMIC, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    report_stage("check-dynamic-texture", hr,
                 SUCCEEDED(hr) ? "a8r8g8b8-default-pool" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-dynamic-texture",
                        "D3DFMT_A8R8G8B8");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        0, D3DRTYPE_TEXTURE, D3DFMT_A8);
    report_stage("check-a8-texture", hr,
                 SUCCEEDED(hr) ? "alpha-only-texture=1" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-a8-texture", "D3DFMT_A8");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    report_stage("check-alpha-render-target", hr,
                 SUCCEEDED(hr) ? "a8r8g8b8-destination-alpha" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-alpha-render-target",
                        "D3DFMT_A8R8G8B8");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        0, D3DRTYPE_TEXTURE, D3DFMT_A8R3G3B2);
    report_stage("check-a8r3g3b2", hr,
                 SUCCEEDED(hr) ? "packed-alpha-rgb332=1" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-a8r3g3b2", "D3DFMT_A8R3G3B2");
        goto done;
    }
    hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
        0, D3DRTYPE_TEXTURE, D3DFMT_A4L4);
    report_stage("check-a4l4", hr,
                 SUCCEEDED(hr) ? "packed-alpha-luminance=1" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-a4l4", "D3DFMT_A4L4");
        goto done;
    }

    hr = IDirect3D9_CheckDeviceType(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        mode.Format, mode.Format, FALSE);
    report_stage("check-device-type", hr, "fullscreen-same-format");
    if (FAILED(hr)) {
        rc = fail_probe(7, hr, "check-device-type", "fullscreen");
        goto done;
    }

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "QXPD3D9Probe";
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassA(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        rc = fail_probe(8, HRESULT_FROM_WIN32(GetLastError()),
                        "window-class", "");
        goto done;
    }
    window = CreateWindowA(window_class.lpszClassName, "XPDriver D3D9",
                           WS_POPUP, 0, 0, mode.Width, mode.Height,
                           NULL, NULL, window_class.hInstance, NULL);
    if (!window) {
        rc = fail_probe(8, HRESULT_FROM_WIN32(GetLastError()), "window", "");
        goto done;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    memset(&present, 0, sizeof(present));
    present.BackBufferWidth = mode.Width;
    present.BackBufferHeight = mode.Height;
    present.BackBufferFormat = mode.Format;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = FALSE;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D24S8;
    present.FullScreen_RefreshRateInHz = 0;
    present.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    behavior_flags =
        D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    report_stage("create-device-enter", S_OK, "fullscreen-mixed-vp");
    hr = IDirect3D9_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        behavior_flags, &present, &device);
    report_stage("create-device-leave", hr,
                 SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(9, hr, "create-device", identifier.Description);
        goto done;
    }

    hr = run_separate_fog_contract9(device);
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "separate-fog",
                        "software-vp-fog-specular-alpha");
        goto done;
    }

    hr = run_a8_texture_contract9(device);
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "a8-contract",
                        "patterned-decode-fixed-blend-ps11-blend");
        goto done;
    }

    if (caps.VertexShaderVersion >= D3DVS_VERSION(1, 1)) {
        hr = run_vs11_create_contract9(device);
        if (FAILED(hr)) {
            rc = fail_probe(10, hr, "vs11-create-set",
                            "advertised-backward-profile");
            goto done;
        }
    } else {
        report_stage("vs11-create", S_OK, "skipped-not-advertised");
    }


    if (caps.PixelShaderVersion >= D3DPS_VERSION(2, 0)) {
        IDirect3DPixelShader9 *ps20_shader = NULL;

        hr = IDirect3DDevice9_CreatePixelShader(
            device, ps20_shader_code, &ps20_shader);
        if (SUCCEEDED(hr) && !ps20_shader) hr = E_FAIL;
        report_stage("ps20-create", hr,
                     SUCCEEDED(hr) ? "ps_2_0-mov-oc0-c0" : "failed");
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetPixelShader(device, ps20_shader);
            report_stage("ps20-set", hr, SUCCEEDED(hr) ? "ok" : "failed");
            IDirect3DDevice9_SetPixelShader(device, NULL);
        }
        if (ps20_shader) IDirect3DPixelShader9_Release(ps20_shader);
        if (FAILED(hr)) {
            rc = fail_probe(10, hr, "ps20-create-set",
                            "advertised-ps20-contract");
            goto done;
        }
    } else {
        report_stage("ps20-create", 0, "skipped-not-advertised");
    }
    if (SUCCEEDED(hr) &&
        caps.VertexShaderVersion >= D3DVS_VERSION(2, 0) &&
        caps.PixelShaderVersion >= D3DPS_VERSION(2, 0)) {
        hr = run_sm2_contract9(device);
        if (FAILED(hr)) {
            rc = fail_probe(10, hr, "sm2-contract",
                            "stock-ddi-v4-vs20-ps20-draw-readback");
            goto done;
        }
    } else if (SUCCEEDED(hr)) {
        report_stage("sm2-contract", S_OK, "skipped-not-advertised");
    }
    IDirect3DDevice9_Release(device);
    device = NULL;
    behavior_flags =
        (multistream7_required ?
            D3DCREATE_HARDWARE_VERTEXPROCESSING :
            D3DCREATE_SOFTWARE_VERTEXPROCESSING) |
        D3DCREATE_MULTITHREADED;
    report_stage("visual-device-enter", S_OK,
                 multistream7_required ?
                    "fullscreen-hardware-vp" : "fullscreen-software-vp");
    hr = IDirect3D9_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        behavior_flags, &present, &device);
    report_stage("visual-device-leave", hr,
                 SUCCEEDED(hr) ? "fresh-after-sm2-contract" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(9, hr, "visual-device", identifier.Description);
        goto done;
    }

    hr = IDirect3DDevice9_CreatePixelShader(
        device, pixel_shader_code, &pixel_shader);
    report_stage("pixel-shader-create", hr,
                 SUCCEEDED(hr) ? "ps_1_1-tex-add" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "pixel-shader-create", "ps_1_1");
        goto done;
    }


    /* Do not submit known-unimplemented ps_1_x texture-addressing forms to
     * the stock runtime.  Their driver refusal is sticky: later draws can
     * report success while Present returns D3DERR_DRIVERINTERNALERROR.  The
     * games and probe paths in scope use the supported ps_1_1 forms below. */
    bump_pixel_shader = NULL;
    luminance_bump_pixel_shader = NULL;
    matrix_pixel_shader = NULL;
    register_pixel_shader = NULL;
    report_stage("bump-pixel-shader-create", S_OK,
                 "declined-unsupported-texbem");
    report_stage("luminance-bump-pixel-shader-create", S_OK,
                 "declined-unsupported-texbeml");
    report_stage("matrix-pixel-shader-create", S_OK,
                 "declined-unsupported-texm3x2");
    report_stage("register-pixel-shader-create", S_OK,
                 "declined-unsupported-texreg2");
    hr = IDirect3DDevice9_CreatePixelShader(
        device, fourth_texture_pixel_shader_code,
        &fourth_texture_pixel_shader);
    report_stage("fourth-texture-pixel-shader-create", hr,
                 SUCCEEDED(hr) ? "ps_1_1-tex-t3" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "fourth-texture-pixel-shader-create",
                        "tex-t3");
        goto done;
    }
    cube_pixel_shader = NULL;
    pixel_shader_12 = NULL;
    pixel_shader_13_depth = NULL;
    report_stage("cube-pixel-shader-create", S_OK,
                 "declined-unsupported-texm3x3tex");
    report_stage("pixel-shader-12-create", S_OK,
                 "declined-unsupported-texreg2rgb");
    report_stage("pixel-shader-13-depth-create", S_OK,
                 "declined-unsupported-texm3x2depth");

    hr = IDirect3DDevice9_CreateTexture(
        device, 4, 4, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 4; ++y) {
            DWORD *row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 4; ++x) row[x] = 0xff00ff00UL;
        }
        hr = IDirect3DTexture9_UnlockRect(texture, 0);
    }
    report_stage("managed-texture", hr,
                 SUCCEEDED(hr) ? "4x4-a8r8g8b8-green" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-texture", "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 4, 4, 1, 0, D3DFMT_V8U8,
        D3DPOOL_MANAGED, &bump_texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(bump_texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 4; ++y) {
            WORD *row = (WORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 4; ++x) row[x] = 0x0040;
        }
        hr = IDirect3DTexture9_UnlockRect(bump_texture, 0);
    }
    report_stage("managed-bump-texture", hr,
                 SUCCEEDED(hr) ? "4x4-v8u8-du=64-dv=0" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-bump-texture", "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 4, 4, 1, 0, D3DFMT_L6V5U5,
        D3DPOOL_MANAGED, &luminance_bump_texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(
            luminance_bump_texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 4; ++y) {
            WORD *row = (WORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 4; ++x) row[x] = 0xfc00;
        }
        hr = IDirect3DTexture9_UnlockRect(luminance_bump_texture, 0);
    }
    report_stage("managed-luminance-bump-texture", hr,
                 SUCCEEDED(hr) ? "4x4-l6v5u5-l=63-du=0-dv=0" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-luminance-bump-texture",
                        "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 4, 4, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &bump_target_texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(
            bump_target_texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 4; ++y) {
            DWORD *row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 4; ++x)
                row[x] = x < 2 ? 0xffff0000UL : 0xff0000ffUL;
        }
        hr = IDirect3DTexture9_UnlockRect(bump_target_texture, 0);
    }
    report_stage("managed-bump-target", hr,
                 SUCCEEDED(hr) ? "4x4-red-blue" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-bump-target", "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &register_source_texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(
            register_source_texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        *(DWORD *)locked.pBits = 0xbf4040bfUL;
        hr = IDirect3DTexture9_UnlockRect(register_source_texture, 0);
    }
    report_stage("managed-register-source", hr,
                 SUCCEEDED(hr) ? "1x1-a8r8g8b8-ar-blue-gb-red" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-register-source",
                        "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &fourth_texture, NULL);
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(
            fourth_texture, 0, &locked, NULL, 0);
    }
    if (SUCCEEDED(hr)) {
        *(DWORD *)locked.pBits = 0xff00ffffUL;
        hr = IDirect3DTexture9_UnlockRect(fourth_texture, 0);
    }
    report_stage("managed-fourth-texture", hr,
                 SUCCEEDED(hr) ? "1x1-a8r8g8b8-cyan" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-fourth-texture",
                        "create-lock-fill");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 4, 4, 3, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &mip_texture, NULL);
    for (cube_level = 0;
         SUCCEEDED(hr) && cube_level < 3; ++cube_level) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(
            mip_texture, cube_level, &locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            cube_edge = 4 >> cube_level;
            for (y = 0; y < cube_edge; ++y) {
                DWORD *row = (DWORD *)((BYTE *)locked.pBits +
                                       y * locked.Pitch);
                for (x = 0; x < cube_edge; ++x)
                    row[x] = mip_texture_colors[cube_level];
            }
            hr = IDirect3DTexture9_UnlockRect(mip_texture, cube_level);
        }
    }
    report_stage("managed-mip-texture", hr,
                 SUCCEEDED(hr) ?
                    "4x4-three-level-level2=40a0e0" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-mip-texture",
                        "create-lock-three-levels");
        goto done;
    }

    hr = IDirect3DDevice9_CreateCubeTexture(
        device, 4, 3, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &cube_texture, NULL);
    for (cube_face = 0;
         SUCCEEDED(hr) && cube_face < 6; ++cube_face) {
        for (cube_level = 0;
             SUCCEEDED(hr) && cube_level < 3; ++cube_level) {
            memset(&locked, 0, sizeof(locked));
            hr = IDirect3DCubeTexture9_LockRect(
                cube_texture, (D3DCUBEMAP_FACES)cube_face,
                cube_level, &locked, NULL, 0);
            if (SUCCEEDED(hr)) {
                cube_edge = 4 >> cube_level;
                for (cube_y = 0; cube_y < cube_edge; ++cube_y) {
                    DWORD *row = (DWORD *)((BYTE *)locked.pBits +
                                           cube_y * locked.Pitch);
                    for (cube_x = 0; cube_x < cube_edge; ++cube_x) {
                        row[cube_x] =
                            cube_face_colors[cube_level][cube_face];
                    }
                }
                hr = IDirect3DCubeTexture9_UnlockRect(
                    cube_texture, (D3DCUBEMAP_FACES)cube_face,
                    cube_level);
            }
        }
    }
    report_stage("managed-cube-texture", hr,
                 SUCCEEDED(hr) ?
                    "4x4-three-level-six-faces-level2-positive-x=8040ff" :
                                 "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-cube-texture",
                        "create-lock-six-faces");
        goto done;
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(
        device, sizeof(register_vertices), 0, matrix_vertex_fvf,
        D3DPOOL_MANAGED, &register_vertex_buffer, NULL);
    if (SUCCEEDED(hr)) {
        vertex_data = NULL;
        hr = IDirect3DVertexBuffer9_Lock(
            register_vertex_buffer, 0, sizeof(register_vertices),
            &vertex_data, 0);
    }
    if (SUCCEEDED(hr)) {
        memcpy(vertex_data, register_vertices, sizeof(register_vertices));
        hr = IDirect3DVertexBuffer9_Unlock(register_vertex_buffer);
    }
    report_stage("managed-register-vertices", hr,
                 SUCCEEDED(hr) ? "tex3-triangle" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "managed-register-vertices",
                        "create-lock-fill");
        goto done;
    }

    if (multistream7_required) {
        hr = prepare_multistream7(
            device, &mode, &multistream7_state, &multistream7_stage);
        _snprintf(detail, sizeof(detail),
                  "streams=%lu stream0-offset=0 stride=%u "
                  "element-offset=%u stream7-offset=0 stride=%u "
                  "element-offset=%u",
                  (unsigned long)caps.MaxStreams,
                  (unsigned)QXP_MS_POSITION_STRIDE,
                  (unsigned)QXP_MS_POSITION_ELEMENT_OFFSET,
                  (unsigned)QXP_MS_COLOR_STRIDE,
                  (unsigned)QXP_MS_COLOR_ELEMENT_OFFSET);
        detail[sizeof(detail) - 1] = '\0';
        report_stage(multistream7_stage, hr, detail);
        if (FAILED(hr)) {
            rc = fail_probe(10, hr, multistream7_stage, detail);
            goto done;
        }
    } else {
        report_stage("multistream7-ready", S_OK,
                     "skipped-not-requested");
    }


    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &backbuffer);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_GetDepthStencilSurface(device, &backdepth);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetViewport(device, &main_viewport);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateTexture(
            device, 64, 64, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
            &render_target_texture, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DTexture9_GetSurfaceLevel(
            render_target_texture, 0, &render_target_surface);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_CreateDepthStencilSurface(
            device, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
            TRUE, &render_depth, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderTarget(
            device, 0, render_target_surface);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, render_depth);
    }
    memset(&render_viewport, 0, sizeof(render_viewport));
    render_viewport.Width = 64;
    render_viewport.Height = 64;
    render_viewport.MinZ = 0.0f;
    render_viewport.MaxZ = 1.0f;
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetViewport(device, &render_viewport);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            0x40400080UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_BeginScene(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetFVF(device, vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, render_target_no_depth_vertices,
            sizeof(render_target_no_depth_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZFUNC, D3DCMP_LESS);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, render_target_front_vertices,
            sizeof(render_target_front_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, render_target_back_vertices,
            sizeof(render_target_back_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, destination_alpha_vertices,
            sizeof(destination_alpha_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_INVDESTALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, &destination_alpha_vertices[3],
            sizeof(destination_alpha_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_EndScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderTarget(device, 0, backbuffer);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetDepthStencilSurface(device, backdepth);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetViewport(device, &main_viewport);
    }
    report_stage("render-target",
                 hr, SUCCEEDED(hr) ?
                    "64x64-a8r8g8b8-d24s8-destination-alpha-draw-and-restore" :
                    "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "render-target", "create-draw-restore");
        goto done;
    }

    hr = IDirect3DDevice9_CreateTexture(
        device, 16, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT, &dynamic_texture, NULL);
    if (SUCCEEDED(hr)) {
        hr = fill_dynamic_texture9(dynamic_texture, 0xffc02040UL);
    }
    report_stage("dynamic-texture-create", hr,
                 SUCCEEDED(hr) ? "default-pool-discard-c02040" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "dynamic-texture-create",
                        "default-pool-discard");
        goto done;
    }

    hr = IDirect3DDevice9_Clear(
        device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        0xff144696UL, 1.0f, 0);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_BeginScene(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetFVF(device, vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetFVF(device, texture_vertex_fvf);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, texture_vertices,
            sizeof(texture_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)mip_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAXMIPLEVEL, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, mip_texture_vertices,
            sizeof(mip_texture_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAXMIPLEVEL, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, shader_constants, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShader(device, pixel_shader);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetFVF(device, texture_vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, shader_vertices,
            sizeof(shader_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)bump_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 1, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT00, 0x3f800000UL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT01, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT10, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT11, 0);
    }
    if (SUCCEEDED(hr) && bump_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(device, bump_pixel_shader);
    }
    if (SUCCEEDED(hr) && bump_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, bump_vertex_fvf);
    if (SUCCEEDED(hr) && bump_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, bump_vertices,
            sizeof(bump_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)luminance_bump_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 1, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT00, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT01, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT10, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVMAT11, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVLSCALE, 0x3efefeffUL);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_BUMPENVLOFFSET, 0);
    }
    if (SUCCEEDED(hr) && luminance_bump_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(
            device, luminance_bump_pixel_shader);
    }
    if (SUCCEEDED(hr) && luminance_bump_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, bump_vertex_fvf);
    if (SUCCEEDED(hr) && luminance_bump_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, luminance_bump_vertices,
            sizeof(luminance_bump_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 2, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr) && matrix_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(device, matrix_pixel_shader);
    }
    if (SUCCEEDED(hr) && matrix_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, matrix_vertex_fvf);
    if (SUCCEEDED(hr) && matrix_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, matrix_vertices,
            sizeof(matrix_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)register_source_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 1, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 2, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr) && register_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(device, register_pixel_shader);
    }
    if (SUCCEEDED(hr) && register_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, matrix_vertex_fvf);
    if (SUCCEEDED(hr) && register_pixel_shader) {
        hr = IDirect3DDevice9_SetStreamSource(
            device, 0, register_vertex_buffer, 0,
            sizeof(register_vertices[0]));
    }
    if (SUCCEEDED(hr) && register_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitive(
            device, D3DPT_TRIANGLELIST, 0, 1);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 3, (IDirect3DBaseTexture9 *)fourth_texture);
    }
    for (x = 0; SUCCEEDED(hr) && x < 4; ++x) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, x, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTextureStageState(
                device, x, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTextureStageState(
                device, x, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTextureStageState(
                device, x, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice9_SetTextureStageState(
                device, x, D3DTSS_TEXCOORDINDEX, x);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetFVF(device, four_texture_vertex_fvf);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fixed_fourth_vertices,
            sizeof(fixed_fourth_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShader(
            device, fourth_texture_pixel_shader);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fourth_texture_vertices,
            sizeof(fourth_texture_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 2, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 3, (IDirect3DBaseTexture9 *)cube_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MAXMIPLEVEL, 2);
    }
    if (SUCCEEDED(hr) && cube_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(device, cube_pixel_shader);
    }
    if (SUCCEEDED(hr) && cube_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, cube_vertex_fvf);
    if (SUCCEEDED(hr) && cube_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, cube_vertices,
            sizeof(cube_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 1, (IDirect3DBaseTexture9 *)cube_texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 2, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 3, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 1, D3DSAMP_MAXMIPLEVEL, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, &pixel_shader_12_constants[0][0], 4);
    }
    if (SUCCEEDED(hr) && pixel_shader_12) {
        hr = IDirect3DDevice9_SetPixelShader(device, pixel_shader_12);
    }
    if (SUCCEEDED(hr) && pixel_shader_12)
        hr = IDirect3DDevice9_SetFVF(device, bump_vertex_fvf);
    if (SUCCEEDED(hr) && pixel_shader_12) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, pixel_shader_12_vertices,
            sizeof(pixel_shader_12_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZWRITEENABLE, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZFUNC, D3DCMP_LESS);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetFVF(device, vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, depth_background_vertices,
            sizeof(depth_background_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetPixelShaderConstantF(
            device, 0, pixel_shader_13_depth_constants, 1);
    }
    if (SUCCEEDED(hr) && pixel_shader_13_depth) {
        hr = IDirect3DDevice9_SetPixelShader(
            device, pixel_shader_13_depth);
    }
    if (SUCCEEDED(hr) && pixel_shader_13_depth)
        hr = IDirect3DDevice9_SetFVF(device, matrix_vertex_fvf);
    if (SUCCEEDED(hr) && pixel_shader_13_depth) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1,
            pixel_shader_13_depth_vertices,
            sizeof(pixel_shader_13_depth_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, D3DZB_FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)render_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetFVF(device, texture_vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, render_target_sample_vertices,
            sizeof(render_target_sample_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)dynamic_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetFVF(
        device, texture_vertex_fvf);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, dynamic_texture_triangles,
            sizeof(dynamic_texture_triangles[0]));
    }
    report_stage("clear-sub-draw1", hr, SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_EndScene(device);
    if (SUCCEEDED(hr)) {
        hr = fill_dynamic_texture9(dynamic_texture, 0xff20c080UL);
    }
    report_stage("clear-sub-filldyn", hr, SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1,
            &dynamic_texture_triangles[3],
            sizeof(dynamic_texture_triangles[0]));
    }
    report_stage("clear-sub-draw2", hr, SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr) && multistream7_required) {
        hr = draw_multistream7(device, &multistream7_state);
        if (SUCCEEDED(hr)) multistream7_executed = 1;
        report_stage("multistream7-draw", hr,
                     multistream7_executed ? "stream0+7-draw=1" :
                                             "failed");
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_EndScene(device);
    report_stage("clear-sub-present", hr, SUCCEEDED(hr) ? "pre" : "failed");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Present(
        device, NULL, NULL, NULL, NULL);
    report_stage("clear-sub-presented", hr, SUCCEEDED(hr) ? "ok" : "failed");
    report_stage("clear-draw-present", hr,
                 SUCCEEDED(hr) ?
                    "fixed-four-stage-plus-ps_1_1-rtt-destination-alpha-managed-mip-dynamic-texture" :
                    "failed");
    if (FAILED(hr)) {
        rc = fail_probe(10, hr, "clear-draw-present", "fixed-function");
        goto done;
    }

    /* The deliberately-declined ps_1_x texture-addressing shaders (texbem,
     * texbeml, texm3x2, texreg2, texm3x3tex, ps_1_2 texreg2rgb, ps_1_3
     * texm3x2depth) draw nothing, so their pixels are not claimed here. */
    report_stage("frame-ready", S_OK,
                 "background=144696 triangle=ff8000 texture=00ff00 mip2=40a0e0 shader=ffff00 fixedfourth=00ffff fourth=00ffff rtt-top=20c060 rtt-bottom=400080 destalpha=400000 invdestalpha=00bf00 dynamic-before=c02040 dynamic-after=20c080");
    if (capture_handshake) {
        if (!wait_capture_ack("frame-capture-wait", "key=f12", VK_F12)) {
            rc = fail_probe(12, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                            "frame-capture-timeout", "key=f12");
            goto done;
        }
        report_stage("frame-capture-ack", S_OK, "key=f12");
    } else {
        Sleep(3000);
    }

    if (multistream7_required) {
        hr = release_multistream7(
            device, &multistream7_state, &multistream7_released);
        report_stage("multistream7-release", hr,
                     SUCCEEDED(hr) && multistream7_released ?
                        "system-linear-resources-released" : "failed");
        if (FAILED(hr) || !multistream7_released) {
            rc = fail_probe(11, FAILED(hr) ? hr : E_FAIL,
                            "multistream7-release",
                            "release-before-reset");
            goto done;
        }
    }

    IDirect3DSurface9_Release(render_target_surface);
    render_target_surface = NULL;
    IDirect3DTexture9_Release(render_target_texture);
    render_target_texture = NULL;
    IDirect3DSurface9_Release(render_depth);
    render_depth = NULL;
    IDirect3DSurface9_Release(backdepth);
    backdepth = NULL;
    IDirect3DSurface9_Release(backbuffer);
    backbuffer = NULL;
    IDirect3DTexture9_Release(dynamic_texture);
    dynamic_texture = NULL;

    if (multistream7_required) {
        if (!multistream7_executed || !multistream7_released ||
            multistream7_state.position_buffer ||
            multistream7_state.color_buffer ||
            multistream7_state.declaration || multistream7_state.shader ||
            multistream7_state.pixel_shader) {
            rc = fail_probe(11, E_FAIL, "multistream7-lifetime",
                            "resources-not-released-before-reset");
            goto done;
        }
        report_stage("multistream7-lifetime", S_OK,
                     "system-linear-resources-released-before-reset");
    }

    hr = IDirect3DDevice9_TestCooperativeLevel(device);
    report_stage("cooperative-level", hr,
                 SUCCEEDED(hr) ? "ready" : "failed");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Reset(device, &present);
    report_stage("reset", hr, SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff123456UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ZENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)mip_texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 2, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetTexture(device, 3, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAXMIPLEVEL, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetFVF(device, texture_vertex_fvf);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, mip_texture_vertices,
            sizeof(mip_texture_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 0, (IDirect3DBaseTexture9 *)bump_target_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetTexture(
            device, 3, (IDirect3DBaseTexture9 *)cube_texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_SetSamplerState(
            device, 3, D3DSAMP_MAXMIPLEVEL, 2);
    }
    for (x = 0; SUCCEEDED(hr) && x < 4; ++x) {
        hr = IDirect3DDevice9_SetTextureStageState(
            device, x, D3DTSS_TEXCOORDINDEX, x);
    }
    if (SUCCEEDED(hr) && cube_pixel_shader) {
        hr = IDirect3DDevice9_SetPixelShader(device, cube_pixel_shader);
    }
    if (SUCCEEDED(hr) && cube_pixel_shader)
        hr = IDirect3DDevice9_SetFVF(device, cube_vertex_fvf);
    if (SUCCEEDED(hr) && cube_pixel_shader) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, cube_vertices,
            sizeof(cube_vertices[0]));
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_EndScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    }
    report_stage("reset-clear-present", hr,
                 SUCCEEDED(hr) ?
                    "background=123456 mip2=40a0e0" :
                    "failed");
    if (FAILED(hr)) {
        rc = fail_probe(11, hr, "reset-clear-present", "fullscreen");
        goto done;
    }
    if (capture_handshake) {
        if (!wait_capture_ack("reset-capture-wait", "key=f11", VK_F11)) {
            rc = fail_probe(12, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                            "reset-capture-timeout", "key=f11");
            goto done;
        }
        report_stage("reset-capture-ack", S_OK, "key=f11");
    } else {
        Sleep(3000);
    }

done:
    release_multistream7_state(&multistream7_state);
    if (render_depth) IDirect3DSurface9_Release(render_depth);
    if (render_target_surface)
        IDirect3DSurface9_Release(render_target_surface);
    if (backdepth) IDirect3DSurface9_Release(backdepth);
    if (backbuffer) IDirect3DSurface9_Release(backbuffer);
    if (render_target_texture)
        IDirect3DTexture9_Release(render_target_texture);
    if (dynamic_texture) IDirect3DTexture9_Release(dynamic_texture);
    if (register_vertex_buffer)
        IDirect3DVertexBuffer9_Release(register_vertex_buffer);
    if (pixel_shader_13_depth)
        IDirect3DPixelShader9_Release(pixel_shader_13_depth);
    if (pixel_shader_12)
        IDirect3DPixelShader9_Release(pixel_shader_12);
    if (cube_pixel_shader)
        IDirect3DPixelShader9_Release(cube_pixel_shader);
    if (fourth_texture_pixel_shader)
        IDirect3DPixelShader9_Release(fourth_texture_pixel_shader);
    if (register_pixel_shader)
        IDirect3DPixelShader9_Release(register_pixel_shader);
    if (matrix_pixel_shader)
        IDirect3DPixelShader9_Release(matrix_pixel_shader);
    if (luminance_bump_pixel_shader)
        IDirect3DPixelShader9_Release(luminance_bump_pixel_shader);
    if (bump_pixel_shader)
        IDirect3DPixelShader9_Release(bump_pixel_shader);
    if (pixel_shader) IDirect3DPixelShader9_Release(pixel_shader);
    if (bump_target_texture)
        IDirect3DTexture9_Release(bump_target_texture);
    if (cube_texture) IDirect3DCubeTexture9_Release(cube_texture);
    if (mip_texture) IDirect3DTexture9_Release(mip_texture);
    if (fourth_texture) IDirect3DTexture9_Release(fourth_texture);
    if (register_source_texture)
        IDirect3DTexture9_Release(register_source_texture);
    if (luminance_bump_texture)
        IDirect3DTexture9_Release(luminance_bump_texture);
    if (bump_texture) IDirect3DTexture9_Release(bump_texture);
    if (texture) IDirect3DTexture9_Release(texture);
    if (device) IDirect3DDevice9_Release(device);
    if (window) DestroyWindow(window);
    if (d3d) IDirect3D9_Release(d3d);
    if (module) FreeLibrary(module);
    if (!rc) {
        /* ps_1_2/ps_1_3/texm3x2depth and the cubemap draws depended on the
         * deliberately-declined ps_1_x texture-addressing family; report
         * them from the create outcomes instead of claiming fixed =1. */
        _snprintf(detail, sizeof(detail),
                  "runtime=system32 fixed-function=1 ps_1_1=1 "
                  "ps_1_x-addressing=%s a8-pattern=1 managed-mip=1 "
                  "cubemap-mip=%s dynamic-cube=%s dynamic-texture=1 "
                  "render-target=1 separate-fog=1 "
                  "destination-alpha=1 depth-attachment=1 "
                  "multistream7=%d resources-released=%d reset=1",
                  (bump_pixel_shader || luminance_bump_pixel_shader ||
                   matrix_pixel_shader || register_pixel_shader ||
                   cube_pixel_shader || pixel_shader_12 ||
                   pixel_shader_13_depth) ?
                      "unexpectedly-admitted" : "declined-unsupported",
                  cube_pixel_shader ? "1" : "declined",
                  cube_pixel_shader ? "1" : "declined",
                  multistream7_executed, multistream7_released);
        detail[sizeof(detail) - 1] = '\0';
        report_message("QXP_D3D9_RESULT", 0, S_OK, "complete", detail);
    }
    return rc;
}
