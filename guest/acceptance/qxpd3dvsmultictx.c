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

static void report_stage(const char *stage, DWORD shader_a, DWORD shader_b)
{
    char message[224];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_MULTI_STAGE stage=%s shader-a=%lu shader-b=%lu",
              stage, (unsigned long)shader_a, (unsigned long)shader_b);
    message[sizeof(message) - 1] = '\0';
    puts(message);
    qxp_report_serial(message);
}

static void report_result(int rc, HRESULT hr, const char *stage,
                          const char *detail)
{
    char message[384];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_MULTI_RESULT rc=%d hr=0x%08lx stage=%s%s%s",
              rc, (unsigned long)hr, stage,
              detail && detail[0] ? " " : "",
              detail && detail[0] ? detail : "");
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

static HRESULT qxp_verify_shader(IDirect3DDevice8 *device,
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

static HRESULT qxp_render(IDirect3DDevice8 *device, HWND window,
                          DWORD shader, QXP_VS_VERTEX vertices[6],
                          DWORD clear_color)
{
    HRESULT hr;

    if (!SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = IDirect3DDevice8_SetVertexShader(device, shader);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
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

static void qxp_initialize_present(D3DPRESENT_PARAMETERS *present,
                                   HWND window,
                                   const D3DDISPLAYMODE *mode)
{
    memset(present, 0, sizeof(*present));
    present->BackBufferWidth = mode->Width;
    present->BackBufferHeight = mode->Height;
    present->BackBufferFormat = mode->Format;
    present->BackBufferCount = 1;
    present->MultiSampleType = D3DMULTISAMPLE_NONE;
    present->SwapEffect = D3DSWAPEFFECT_COPY;
    present->hDeviceWindow = window;
    present->Windowed = TRUE;
    present->FullScreen_RefreshRateInHz = 0;
    present->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
}

int main(void)
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
    static const float offset_a[4] = {0.4f, 0.0f, 0.0f, 0.0f};
    static const float offset_b[4] = {-0.4f, 0.0f, 0.0f, 0.0f};
    static const DWORD clear_color = 0x000b1d2eUL;
    HMODULE module = NULL;
    QXP_D3D8_ENTRY entry;
    PFN_DIRECT3DCREATE8 create;
    IDirect3D8 *d3d = NULL;
    IDirect3DDevice8 *device_a = NULL;
    IDirect3DDevice8 *device_b = NULL;
    D3DDISPLAYMODE mode;
    D3DCAPS8 caps;
    D3DPRESENT_PARAMETERS present_a;
    D3DPRESENT_PARAMETERS present_b;
    WNDCLASSA wc;
    HWND window_a = NULL;
    HWND window_b = NULL;
    QXP_VS_VERTEX vertices[6];
    DWORD shader_a = 0;
    DWORD shader_b = 0;
    BOOL handles_equal = FALSE;
    ULONG release_count;
    HRESULT hr = E_FAIL;
    int rc = 1;
    char detail[224];

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
    if (FAILED(hr) || caps.VertexShaderVersion < D3DVS_VERSION(1, 1) ||
        caps.MaxVertexShaderConst < 96) {
        report_result(4, FAILED(hr) ? hr : D3DERR_NOTAVAILABLE,
                      "caps", "vs-1-1-96-required");
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
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPD3DVSMultiContext";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(6, hr, "window-class", "register");
        goto done;
    }
    window_a = CreateWindowA(
        wc.lpszClassName, "XPDriver D3D8 multi-context A",
        WS_POPUP | WS_VISIBLE, 0, 0, mode.Width, mode.Height,
        NULL, NULL, wc.hInstance, NULL);
    window_b = CreateWindowA(
        wc.lpszClassName, "XPDriver D3D8 multi-context B",
        WS_POPUP | WS_VISIBLE, 0, 0, mode.Width, mode.Height,
        NULL, NULL, wc.hInstance, NULL);
    if (!window_a || !window_b) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        report_result(7, hr, "window", "create-pair");
        goto done;
    }
    qxp_initialize_present(&present_a, window_a, &mode);
    qxp_initialize_present(&present_b, window_b, &mode);
    hr = IDirect3D8_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_a,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &present_a, &device_a);
    if (FAILED(hr)) {
        report_result(8, hr, "create-device-a", "windowed-hardware-vp");
        goto done;
    }
    hr = IDirect3D8_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_b,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &present_b, &device_b);
    if (FAILED(hr)) {
        report_result(9, hr, "create-device-b", "windowed-hardware-vp");
        goto done;
    }
    report_stage("devices-live", 0, 0);

    hr = IDirect3DDevice8_CreateVertexShader(
        device_a, declaration, function, &shader_a, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateVertexShader(
        device_b, declaration, function, &shader_b, 0);
    if (FAILED(hr)) {
        report_result(10, hr, "create-shaders", "owner-pair");
        goto done;
    }
    handles_equal = shader_a == shader_b;
    hr = IDirect3DDevice8_SetVertexShaderConstant(
        device_a, 0, offset_a, 1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShaderConstant(
        device_b, 0, offset_b, 1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device_a, shader_a);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device_b, shader_b);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader(device_a, shader_a, offset_a);
    if (SUCCEEDED(hr)) hr = qxp_verify_shader(device_b, shader_b, offset_b);
    if (FAILED(hr)) {
        report_result(11, hr, "initialize-shaders", "bind-and-verify");
        goto done;
    }
    report_stage("shaders-live", shader_a, shader_b);

    qxp_fill_quad(vertices, 0xffff2080UL);
    report_stage("device-a-frame-enter", shader_a, shader_b);
    hr = qxp_render(device_a, window_a, shader_a, vertices, clear_color);
    if (FAILED(hr)) {
        report_result(12, hr, "device-a-draw", "initial");
        goto done;
    }
    Sleep(3500);
    report_stage("device-a-frame-leave", shader_a, shader_b);

    qxp_fill_quad(vertices, 0xff4060e0UL);
    report_stage("device-b-frame-enter", shader_a, shader_b);
    hr = qxp_render(device_b, window_b, shader_b, vertices, clear_color);
    if (FAILED(hr)) {
        report_result(13, hr, "device-b-draw", "initial");
        goto done;
    }
    Sleep(3500);
    report_stage("device-b-frame-leave", shader_a, shader_b);

    qxp_fill_quad(vertices, 0xffe0a020UL);
    report_stage("device-a-return-enter", shader_a, shader_b);
    hr = qxp_verify_shader(device_a, shader_a, offset_a);
    if (SUCCEEDED(hr)) hr = qxp_render(
        device_a, window_a, shader_a, vertices, clear_color);
    if (FAILED(hr)) {
        report_result(14, hr, "device-a-draw", "return-after-b");
        goto done;
    }
    Sleep(3500);
    report_stage("device-a-return-leave", shader_a, shader_b);

    hr = IDirect3DDevice8_SetVertexShader(
        device_a, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device_a, shader_a);
    if (FAILED(hr)) {
        report_result(15, hr, "delete-shader-a", "while-b-live");
        goto done;
    }
    shader_a = 0;
    release_count = IDirect3DDevice8_Release(device_a);
    device_a = NULL;
    if (release_count != 0) {
        report_result(16, E_FAIL, "destroy-device-a", "release-count-nonzero");
        goto done;
    }
    DestroyWindow(window_a);
    window_a = NULL;
    report_stage("device-a-destroyed", 0, shader_b);
    Sleep(500);

    hr = qxp_verify_shader(device_b, shader_b, offset_b);
    qxp_fill_quad(vertices, 0xff20c060UL);
    report_stage("device-b-survivor-enter", 0, shader_b);
    if (SUCCEEDED(hr)) hr = qxp_render(
        device_b, window_b, shader_b, vertices, clear_color);
    if (FAILED(hr)) {
        report_result(17, hr, "device-b-draw", "after-a-destroy");
        goto done;
    }
    Sleep(4500);
    report_stage("device-b-survivor-leave", 0, shader_b);

    hr = IDirect3DDevice8_SetVertexShader(
        device_b, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DeleteVertexShader(
        device_b, shader_b);
    if (FAILED(hr)) {
        report_result(18, hr, "delete-shader-b", "survivor");
        goto done;
    }
    _snprintf(detail, sizeof(detail),
              "devices=2 simultaneous=1 a-destroyed=1 survivor=1 handles-equal=%u draws=4 presents=4",
              handles_equal ? 1U : 0U);
    detail[sizeof(detail) - 1] = '\0';
    shader_b = 0;
    report_result(0, S_OK, "complete", detail);
    rc = 0;

done:
    if (shader_a && device_a) {
        IDirect3DDevice8_SetVertexShader(
            device_a, D3DFVF_XYZ | D3DFVF_DIFFUSE);
        IDirect3DDevice8_DeleteVertexShader(device_a, shader_a);
    }
    if (shader_b && device_b) {
        IDirect3DDevice8_SetVertexShader(
            device_b, D3DFVF_XYZ | D3DFVF_DIFFUSE);
        IDirect3DDevice8_DeleteVertexShader(device_b, shader_b);
    }
    if (device_a) IDirect3DDevice8_Release(device_a);
    if (device_b) IDirect3DDevice8_Release(device_b);
    if (d3d) IDirect3D8_Release(d3d);
    if (window_a) DestroyWindow(window_a);
    if (window_b) DestroyWindow(window_b);
    if (module) FreeLibrary(module);
    return rc;
}
