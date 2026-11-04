#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

typedef IDirect3D8 *(WINAPI *PFN_DIRECT3DCREATE8)(UINT sdk_version);

typedef union qxp_d3d8_entry {
    FARPROC proc;
    PFN_DIRECT3DCREATE8 create;
} QXP_D3D8_ENTRY;

typedef struct qxp_vs_vertex {
    float position[3];
    DWORD color;
} QXP_VS_VERTEX;

#define QXP_D3D8_PARAMETER_TOKEN 0x80000000UL
#define QXP_D3D8_DEST(type, number, mask) \
    (QXP_D3D8_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     (DWORD)(mask))
#define QXP_D3D8_SOURCE(type, number) \
    (QXP_D3D8_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     D3DVS_NOSWIZZLE)

static unsigned g_succession_leg;

static void report_result(int rc, HRESULT hr, const char *stage,
                          const char *detail)
{
    char message[384];

    if (g_succession_leg) {
        _snprintf(message, sizeof(message),
                  "QXP_D3DVS_SUCCESSION_CHILD_RESULT leg=%u rc=%d hr=0x%08lx stage=%s%s%s",
                  g_succession_leg, rc, (unsigned long)hr, stage,
                  detail && detail[0] ? " " : "",
                  detail && detail[0] ? detail : "");
    } else {
        _snprintf(message, sizeof(message),
                  "QXP_D3DVS_RESULT rc=%d hr=0x%08lx stage=%s%s%s",
                  rc, (unsigned long)hr, stage,
                  detail && detail[0] ? " " : "",
                  detail && detail[0] ? detail : "");
    }
    message[sizeof(message) - 1] = '\0';
    puts(message);
    qxp_report_serial(message);
}

static void report_stage(const char *stage)
{
    char message[160];

    if (g_succession_leg) {
        _snprintf(message, sizeof(message),
                  "QXP_D3DVS_SUCCESSION_STAGE leg=%u stage=%s",
                  g_succession_leg, stage);
    } else {
        _snprintf(message, sizeof(message), "QXP_D3DVS_STAGE stage=%s",
                  stage);
    }
    message[sizeof(message) - 1] = '\0';
    puts(message);
    qxp_report_serial(message);
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

static void qxp_fill_quad(QXP_VS_VERTEX vertices[6], DWORD color)
{
    static const float positions[6][3] = {
        {-0.8f, -0.8f, 0.5f},
        { 0.8f, -0.8f, 0.5f},
        { 0.8f,  0.8f, 0.5f},
        {-0.8f, -0.8f, 0.5f},
        { 0.8f,  0.8f, 0.5f},
        {-0.8f,  0.8f, 0.5f}
    };
    unsigned index;

    for (index = 0; index < 6; ++index) {
        memcpy(vertices[index].position, positions[index],
               sizeof(vertices[index].position));
        vertices[index].color = color;
    }
}

static HRESULT qxp_render_current(IDirect3DDevice8 *device,
                                  QXP_VS_VERTEX vertices[6],
                                  DWORD clear_color, DWORD clip_mask)
{
    HRESULT hr;

    hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, clip_mask);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, clear_color, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_EndScene(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
        device, NULL, NULL, NULL, NULL);
    return hr;
}

static HRESULT qxp_render(IDirect3DDevice8 *device, DWORD shader,
                          QXP_VS_VERTEX vertices[6], DWORD clear_color,
                          DWORD clip_mask)
{
    HRESULT hr;

    hr = IDirect3DDevice8_SetVertexShader(device, shader);
    if (SUCCEEDED(hr)) hr = qxp_render_current(
        device, vertices, clear_color, clip_mask);
    return hr;
}

static HRESULT qxp_verify_shader_state(IDirect3DDevice8 *device,
                                       DWORD expected_shader,
                                       const float expected_constant[4])
{
    DWORD shader = 0;
    float constant[4];
    HRESULT hr;

    memset(constant, 0, sizeof(constant));
    hr = IDirect3DDevice8_GetVertexShader(device, &shader);
    if (SUCCEEDED(hr) && shader != expected_shader) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetVertexShaderConstant(
        device, 0, constant, 1);
    if (SUCCEEDED(hr) && memcmp(
            constant, expected_constant, sizeof(constant)) != 0) hr = E_FAIL;
    return hr;
}

int main(int argc, char **argv)
{
    static const DWORD declaration[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
        D3DVSD_END()
    };
    static const DWORD function[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_ADD,
        QXP_D3D8_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                      D3DSP_WRITEMASK_ALL),
        QXP_D3D8_SOURCE(D3DSPR_INPUT, 0),
        QXP_D3D8_SOURCE(D3DSPR_CONST, 0),
        D3DSIO_MOV,
        QXP_D3D8_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
        QXP_D3D8_SOURCE(D3DSPR_INPUT, 1),
        D3DVS_END()
    };
    static const DWORD right_declaration[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
        D3DVSD_CONST(0, 1),
        0x3ecccccdUL, 0x00000000UL, 0x00000000UL, 0x00000000UL,
        D3DVSD_END()
    };
    static const DWORD left_declaration[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(0, D3DVSDT_FLOAT3),
        D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
        D3DVSD_CONST(0, 1),
        0xbecccccdUL, 0x00000000UL, 0x00000000UL, 0x00000000UL,
        D3DVSD_END()
    };
    static const DWORD invalid_function[] = {
        D3DVS_VERSION(1, 1),
        D3DSIO_MOV,
        D3DSPR_RASTOUT | D3DSRO_POSITION | D3DSP_WRITEMASK_ALL,
        QXP_D3D8_SOURCE(D3DSPR_INPUT, 0),
        D3DVS_END()
    };
    static const float clip_plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const float position_offset[4] = {0.4f, 0.0f, 0.0f, 0.0f};
    static const float negative_offset[4] = {-0.4f, 0.0f, 0.0f, 0.0f};
    static const DWORD clear_color = 0x000b1d2eUL;
    IDirect3D8 *d3d = NULL;
    IDirect3DDevice8 *device = NULL;
    HMODULE module = NULL;
    QXP_D3D8_ENTRY entry;
    PFN_DIRECT3DCREATE8 create;
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS present;
    D3DCAPS8 caps;
    WNDCLASSA wc;
    HWND window = NULL;
    QXP_VS_VERTEX vertices[6];
    DWORD shader = 0;
    DWORD right_shader = 0;
    DWORD left_shader = 0;
    DWORD state_block = 0;
    DWORD lifetime_shader = 0;
    DWORD lifetime_state_block = 0;
    DWORD invalid_shader = 0;
    HRESULT hr = E_FAIL;
    int rc = 1;
    char detail[192];

    if (argc == 2 && strcmp(argv[1], "--succession-child-1") == 0) {
        g_succession_leg = 1;
    } else if (argc == 2 &&
               strcmp(argv[1], "--succession-child-2") == 0) {
        g_succession_leg = 2;
    } else if (argc != 1) {
        report_result(22, E_INVALIDARG, "arguments", "unknown-option");
        return 22;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    module = LoadLibraryA("C:\\Windows\\System32\\d3d8.dll");
    if (!module) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(1, hr, "module", "system32-d3d8");
        goto done;
    }
    entry.proc = GetProcAddress(module, "Direct3DCreate8");
    create = entry.create;
    if (!create) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(2, hr, "module", "Direct3DCreate8-export");
        goto done;
    }
    d3d = create(D3D_SDK_VERSION);
    if (!d3d) {
        hr = E_FAIL;
        report_result(3, hr, "create8", "null");
        goto done;
    }
    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D8_GetDeviceCaps(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr)) {
        report_result(4, hr, "caps", "hal");
        goto done;
    }
    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    if (FAILED(hr) || mode.Format != D3DFMT_X8R8G8B8) {
        report_result(5, FAILED(hr) ? hr : D3DERR_NOTAVAILABLE,
                      "display", "x8r8g8b8-required");
        goto done;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPD3DVSProbe";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(6, hr, "window-class", "register");
        goto done;
    }
    window = CreateWindowA(
        wc.lpszClassName, "XPDriver D3D8 vs_1_1 probe",
        WS_POPUP | WS_VISIBLE, 0, 0, mode.Width, mode.Height,
        NULL, NULL, wc.hInstance, NULL);
    if (!window) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(7, hr, "window", "create");
        goto done;
    }
    memset(&present, 0, sizeof(present));
    present.BackBufferWidth = mode.Width;
    present.BackBufferHeight = mode.Height;
    present.BackBufferFormat = mode.Format;
    present.BackBufferCount = 1;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = FALSE;
    present.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    hr = IDirect3D8_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &present, &device);
    if (FAILED(hr)) {
        report_result(8, hr, "create-device", "hardware-vp");
        goto done;
    }

    hr = IDirect3DDevice8_CreateVertexShader(
        device, declaration, function, &shader, 0);
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "caps-vs=%08lx maxconst=%lu",
                  (unsigned long)caps.VertexShaderVersion,
                  (unsigned long)caps.MaxVertexShaderConst);
        detail[sizeof(detail) - 1] = '\0';
        report_result(9, hr, "create-shader", detail);
        goto done;
    }
    hr = IDirect3DDevice8_SetVertexShaderConstant(
        device, 0, position_offset, 1);
    if (FAILED(hr)) {
        report_result(10, hr, "set-constant", "c0-x=0.4");
        goto done;
    }

    hr = IDirect3DDevice8_CreateVertexShader(
        device, right_declaration, function, &right_shader, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateVertexShader(
        device, left_declaration, function, &left_shader, 0);
    if (FAILED(hr)) {
        report_result(11, hr, "state-block-create-shaders",
                      "declaration-constants");
        goto done;
    }
    report_stage("state-block-enter");
    hr = IDirect3DDevice8_BeginStateBlock(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, right_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_EndStateBlock(
        device, &state_block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, left_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(
        device, state_block);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader_state(
        device, right_shader, position_offset);
    qxp_fill_quad(vertices, 0xff4060e0UL);
    if (SUCCEEDED(hr)) hr = qxp_render_current(
        device, vertices, clear_color, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, left_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CaptureStateBlock(
        device, state_block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, right_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(
        device, state_block);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader_state(
        device, left_shader, negative_offset);
    qxp_fill_quad(vertices, 0xffe0a020UL);
    if (SUCCEEDED(hr)) hr = qxp_render_current(
        device, vertices, clear_color, 0);
    if (state_block) {
        HRESULT delete_hr = IDirect3DDevice8_DeleteStateBlock(
            device, state_block);
        state_block = 0;
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }
    report_stage("state-block-leave");
    if (FAILED(hr)) {
        report_result(12, hr, "state-block",
                      "apply-capture-shader-declaration-constant");
        goto done;
    }

    hr = IDirect3DDevice8_CreateVertexShader(
        device, right_declaration, function, &lifetime_shader, 0);
    if (FAILED(hr)) {
        report_result(20, hr, "shader-lifetime-create",
                      "declaration-constant");
        goto done;
    }
    report_stage("shader-lifetime-enter");
    hr = IDirect3DDevice8_BeginStateBlock(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, lifetime_shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_EndStateBlock(
        device, &lifetime_state_block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(
        device, lifetime_state_block);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader_state(
        device, lifetime_shader, position_offset);
    qxp_fill_quad(vertices, 0xff4060e0UL);
    if (SUCCEEDED(hr)) hr = qxp_render_current(
        device, vertices, clear_color, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (lifetime_state_block) {
        HRESULT delete_hr = IDirect3DDevice8_DeleteStateBlock(
            device, lifetime_state_block);
        lifetime_state_block = 0;
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device, lifetime_shader);
    if (SUCCEEDED(hr)) lifetime_shader = 0;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateVertexShader(
        device, left_declaration, function, &lifetime_shader, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, lifetime_shader);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader_state(
        device, lifetime_shader, negative_offset);
    if (SUCCEEDED(hr)) report_stage("shader-lifetime-recreated");
    qxp_fill_quad(vertices, 0xffe0a020UL);
    if (SUCCEEDED(hr)) hr = qxp_render_current(
        device, vertices, clear_color, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device, lifetime_shader);
    if (SUCCEEDED(hr)) lifetime_shader = 0;
    report_stage("shader-lifetime-leave");
    if (FAILED(hr)) {
        report_result(21, hr, "shader-lifetime",
                      "live-state-block-delete-recreate-draw");
        goto done;
    }

    hr = IDirect3DDevice8_SetVertexShader(device, shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShaderConstant(
        device, 0, position_offset, 1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 0, clip_plane);
    qxp_fill_quad(vertices, 0xffff2080UL);
    report_stage("clip-frame-enter");
    if (SUCCEEDED(hr)) hr = qxp_render(
        device, shader, vertices, clear_color, D3DCLIPPLANE0);
    if (FAILED(hr)) {
        report_result(13, hr, "clip-draw", "plane0");
        goto done;
    }
    Sleep(4500);
    report_stage("clip-frame-leave");
    qxp_fill_quad(vertices, 0xff20c060UL);
    report_stage("fast-frame-enter");
    hr = qxp_render(device, shader, vertices, clear_color, 0);
    if (FAILED(hr)) {
        report_result(14, hr, "fast-draw", "no-clip");
        goto done;
    }
    Sleep(4500);
    report_stage("fast-frame-leave");
    hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device, shader);
    if (FAILED(hr)) {
        report_result(15, hr, "delete-shader", "explicit");
        goto done;
    }
    shader = 0;
    hr = IDirect3DDevice8_DeleteVertexShader(device, right_shader);
    if (SUCCEEDED(hr)) right_shader = 0;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device, left_shader);
    if (SUCCEEDED(hr)) left_shader = 0;
    if (FAILED(hr)) {
        report_result(16, hr, "delete-shader", "state-block-pair");
        goto done;
    }

    report_stage("reset-enter");
    hr = IDirect3DDevice8_TestCooperativeLevel(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Reset(device, &present);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateVertexShader(
        device, declaration, function, &shader, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShaderConstant(
        device, 0, position_offset, 1);
    qxp_fill_quad(vertices, 0xff20c060UL);
    if (SUCCEEDED(hr)) hr = qxp_render(
        device, shader, vertices, clear_color, 0);
    report_stage("reset-leave");
    if (FAILED(hr)) {
        report_result(17, hr, "reset-recreate", "shader-draw");
        goto done;
    }
    hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device, shader);
    if (FAILED(hr)) {
        report_result(18, hr, "reset-delete", "recreated-shader");
        goto done;
    }
    shader = 0;

    hr = IDirect3DDevice8_CreateVertexShader(
        device, declaration, invalid_function, &invalid_shader, 0);
    if (SUCCEEDED(hr) || invalid_shader) {
        report_result(19, SUCCEEDED(hr) ? E_FAIL : hr,
                      "invalid-shader", "unmarked-destination-accepted");
        hr = E_FAIL;
        goto done;
    }
    _snprintf(detail, sizeof(detail),
              "caps-vs=%08lx maxconst=%lu c0x=0.4 state-block=1 lifetime=delete-recreate reset=1 invalid=1 clip=plane0 fast=no-clip",
              (unsigned long)caps.VertexShaderVersion,
              (unsigned long)caps.MaxVertexShaderConst);
    detail[sizeof(detail) - 1] = '\0';
    report_result(0, S_OK, "complete", detail);
    rc = 0;

done:
    if (lifetime_state_block && device)
        IDirect3DDevice8_DeleteStateBlock(device, lifetime_state_block);
    if (lifetime_shader && device)
        IDirect3DDevice8_DeleteVertexShader(device, lifetime_shader);
    if (state_block && device)
        IDirect3DDevice8_DeleteStateBlock(device, state_block);
    if (invalid_shader && device)
        IDirect3DDevice8_DeleteVertexShader(device, invalid_shader);
    if (left_shader && device)
        IDirect3DDevice8_DeleteVertexShader(device, left_shader);
    if (right_shader && device)
        IDirect3DDevice8_DeleteVertexShader(device, right_shader);
    if (shader && device) IDirect3DDevice8_DeleteVertexShader(device, shader);
    if (device) IDirect3DDevice8_Release(device);
    if (d3d) IDirect3D8_Release(d3d);
    if (window) DestroyWindow(window);
    if (module) FreeLibrary(module);
    return rc;
}
