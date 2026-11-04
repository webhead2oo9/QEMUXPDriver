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

typedef struct qxp_fixed_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
} QXP_FIXED_VERTEX;

typedef struct qxp_shader_vertex {
    float position[3];
    DWORD color;
} QXP_SHADER_VERTEX;

typedef struct qxp_benchmark_result {
    ULONGLONG device_create_us;
    ULONGLONG shader_create_us;
    ULONGLONG submit_ns_per_draw;
    ULONGLONG present_us;
} QXP_BENCHMARK_RESULT;

typedef enum qxp_benchmark_mode {
    QXP_BENCHMARK_FIXED,
    QXP_BENCHMARK_SHADER
} QXP_BENCHMARK_MODE;

#define QXP_D3D8_PARAMETER_TOKEN 0x80000000UL
#define QXP_D3D8_DEST(type, number, mask) \
    (QXP_D3D8_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     (DWORD)(mask))
#define QXP_D3D8_SOURCE(type, number) \
    (QXP_D3D8_PARAMETER_TOKEN | (DWORD)(type) | (DWORD)(number) | \
     D3DVS_NOSWIZZLE)

#define QXP_BENCHMARK_ROUNDS 2U
#define QXP_BENCHMARK_WARMUP_FRAMES 1U
#define QXP_BENCHMARK_FRAMES 16U
#define QXP_BENCHMARK_DRAWS_PER_FRAME 16U
#define QXP_BENCHMARK_TIMED_API_DRAWS \
    (QXP_BENCHMARK_FRAMES * QXP_BENCHMARK_DRAWS_PER_FRAME)
#define QXP_BENCHMARK_TOTAL_API_DRAWS \
    ((QXP_BENCHMARK_WARMUP_FRAMES + QXP_BENCHMARK_FRAMES) * \
     QXP_BENCHMARK_DRAWS_PER_FRAME)

static const DWORD qxp_declaration[] = {
    D3DVSD_STREAM(0),
    D3DVSD_REG(0, D3DVSDT_FLOAT3),
    D3DVSD_REG(1, D3DVSDT_D3DCOLOR),
    D3DVSD_END()
};

static const DWORD qxp_function[] = {
    D3DVS_VERSION(1, 1),
    D3DSIO_MOV,
    QXP_D3D8_DEST(D3DSPR_RASTOUT, D3DSRO_POSITION,
                  D3DSP_WRITEMASK_ALL),
    QXP_D3D8_SOURCE(D3DSPR_INPUT, 0),
    D3DSIO_MOV,
    QXP_D3D8_DEST(D3DSPR_ATTROUT, 0, D3DSP_WRITEMASK_ALL),
    QXP_D3D8_SOURCE(D3DSPR_INPUT, 1),
    D3DVS_END()
};

static const QXP_FIXED_VERTEX qxp_fixed_triangle[3] = {
    { 80.0f,  60.0f, 0.25f, 1.0f, 0xff40c0e0UL },
    { 320.0f, 240.0f, 0.25f, 1.0f, 0xff40c0e0UL },
    { 80.0f, 240.0f, 0.25f, 1.0f, 0xff40c0e0UL }
};

static const QXP_SHADER_VERTEX qxp_shader_triangle[3] = {
    {{-0.6f,  0.6f, 0.25f}, 0xff40c0e0UL},
    {{ 0.6f, -0.6f, 0.25f}, 0xff40c0e0UL},
    {{-0.6f, -0.6f, 0.25f}, 0xff40c0e0UL}
};

static const char *qxp_mode_name(QXP_BENCHMARK_MODE mode)
{
    return mode == QXP_BENCHMARK_SHADER ? "shader" : "fixed";
}

static void qxp_emit(const char *message)
{
    puts(message);
    qxp_report_serial(message);
}

static void qxp_report_result(int rc, HRESULT hr, const char *stage,
                              const char *detail)
{
    char message[448];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_BENCH_RESULT rc=%d hr=0x%08lx stage=%s%s%s",
              rc, (unsigned long)hr, stage,
              detail && detail[0] ? " " : "",
              detail && detail[0] ? detail : "");
    message[sizeof(message) - 1] = '\0';
    qxp_emit(message);
}

static void qxp_report_phase(unsigned order, QXP_BENCHMARK_MODE mode,
                             const QXP_BENCHMARK_RESULT *result)
{
    char message[320];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_BENCH_PHASE rc=0 order=%u mode=%s device-us=%I64u shader-create-us=%I64u submit-ns-per-draw=%I64u present-us=%I64u warmup-frames=%u frames=%u draws-per-frame=%u timed-api-draws=%u total-api-draws=%u",
              order, qxp_mode_name(mode), result->device_create_us,
              result->shader_create_us, result->submit_ns_per_draw,
              result->present_us, QXP_BENCHMARK_WARMUP_FRAMES,
              QXP_BENCHMARK_FRAMES, QXP_BENCHMARK_DRAWS_PER_FRAME,
              QXP_BENCHMARK_TIMED_API_DRAWS,
              QXP_BENCHMARK_TOTAL_API_DRAWS);
    message[sizeof(message) - 1] = '\0';
    qxp_emit(message);
}

static LRESULT CALLBACK qxp_window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static ULONGLONG qxp_ticks_to_units(ULONGLONG ticks,
                                    const LARGE_INTEGER *frequency,
                                    ULONGLONG scale, ULONGLONG count)
{
    if (!frequency || frequency->QuadPart <= 0 || !count) return 0;
    return ticks * scale / ((ULONGLONG)frequency->QuadPart * count);
}

static HRESULT qxp_draw(IDirect3DDevice8 *device, QXP_BENCHMARK_MODE mode)
{
    if (mode == QXP_BENCHMARK_SHADER) {
        return IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, qxp_shader_triangle,
            sizeof(qxp_shader_triangle[0]));
    }
    return IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, 1, qxp_fixed_triangle,
        sizeof(qxp_fixed_triangle[0]));
}

static HRESULT qxp_configure_device(IDirect3DDevice8 *device)
{
    HRESULT hr;

    hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
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
    return hr;
}

static HRESULT qxp_run_phase(IDirect3D8 *d3d, HWND window,
                             D3DFORMAT display_format,
                             const LARGE_INTEGER *frequency,
                             QXP_BENCHMARK_MODE mode,
                             QXP_BENCHMARK_RESULT *result)
{
    IDirect3DDevice8 *device = NULL;
    D3DPRESENT_PARAMETERS present;
    LARGE_INTEGER start;
    LARGE_INTEGER middle;
    LARGE_INTEGER finish;
    ULONGLONG submit_ticks = 0;
    ULONGLONG present_ticks = 0;
    ULONGLONG device_ticks;
    ULONGLONG shader_ticks = 0;
    DWORD shader = 0;
    HRESULT hr;
    unsigned frame;
    unsigned draw;

    if (!d3d || !window || !frequency || !result) return E_INVALIDARG;
    memset(result, 0, sizeof(*result));
    memset(&present, 0, sizeof(present));
    present.BackBufferWidth = 400;
    present.BackBufferHeight = 300;
    present.BackBufferFormat = display_format;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = TRUE;
    present.FullScreen_RefreshRateInHz = 0;
    present.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    QueryPerformanceCounter(&start);
    hr = IDirect3D8_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &present, &device);
    QueryPerformanceCounter(&finish);
    device_ticks = (ULONGLONG)(finish.QuadPart - start.QuadPart);
    if (FAILED(hr)) goto done;
    result->device_create_us = qxp_ticks_to_units(
        device_ticks, frequency, 1000000ULL, 1);

    hr = qxp_configure_device(device);
    if (FAILED(hr)) goto done;
    if (mode == QXP_BENCHMARK_SHADER) {
        QueryPerformanceCounter(&start);
        hr = IDirect3DDevice8_CreateVertexShader(
            device, qxp_declaration, qxp_function, &shader, 0);
        QueryPerformanceCounter(&finish);
        shader_ticks = (ULONGLONG)(finish.QuadPart - start.QuadPart);
        if (FAILED(hr)) goto done;
        result->shader_create_us = qxp_ticks_to_units(
            shader_ticks, frequency, 1000000ULL, 1);
        hr = IDirect3DDevice8_SetVertexShader(device, shader);
    } else {
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, 0xff0b1d2eUL, 1.0f, 0);
    for (draw = 0; SUCCEEDED(hr) &&
         draw < QXP_BENCHMARK_DRAWS_PER_FRAME; ++draw) {
        hr = qxp_draw(device, mode);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
        device, NULL, NULL, NULL, NULL);
    if (FAILED(hr)) goto done;

    for (frame = 0; frame < QXP_BENCHMARK_FRAMES; ++frame) {
        QueryPerformanceCounter(&start);
        for (draw = 0; draw < QXP_BENCHMARK_DRAWS_PER_FRAME; ++draw) {
            hr = qxp_draw(device, mode);
            if (FAILED(hr)) goto done;
        }
        QueryPerformanceCounter(&middle);
        hr = IDirect3DDevice8_Present(
            device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&finish);
        if (FAILED(hr)) goto done;
        submit_ticks += (ULONGLONG)(middle.QuadPart - start.QuadPart);
        present_ticks += (ULONGLONG)(finish.QuadPart - middle.QuadPart);
    }
    result->submit_ns_per_draw = qxp_ticks_to_units(
        submit_ticks, frequency, 1000000000ULL,
        (ULONGLONG)QXP_BENCHMARK_FRAMES *
            QXP_BENCHMARK_DRAWS_PER_FRAME);
    result->present_us = qxp_ticks_to_units(
        present_ticks, frequency, 1000000ULL, QXP_BENCHMARK_FRAMES);

done:
    if (device && shader) {
        HRESULT cleanup_hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        if (SUCCEEDED(cleanup_hr)) cleanup_hr =
            IDirect3DDevice8_DeleteVertexShader(device, shader);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    if (device) IDirect3DDevice8_Release(device);
    return hr;
}

static void qxp_average_result(QXP_BENCHMARK_RESULT *destination,
                               const QXP_BENCHMARK_RESULT *a,
                               const QXP_BENCHMARK_RESULT *b)
{
    destination->device_create_us =
        (a->device_create_us + b->device_create_us) / 2;
    destination->shader_create_us =
        (a->shader_create_us + b->shader_create_us) / 2;
    destination->submit_ns_per_draw =
        (a->submit_ns_per_draw + b->submit_ns_per_draw) / 2;
    destination->present_us = (a->present_us + b->present_us) / 2;
}

int main(int argc, char **argv)
{
    static const QXP_BENCHMARK_MODE order[4] = {
        QXP_BENCHMARK_FIXED, QXP_BENCHMARK_SHADER,
        QXP_BENCHMARK_SHADER, QXP_BENCHMARK_FIXED
    };
    IDirect3D8 *d3d = NULL;
    HMODULE module = NULL;
    QXP_D3D8_ENTRY entry;
    PFN_DIRECT3DCREATE8 create;
    D3DDISPLAYMODE display_mode;
    D3DCAPS8 caps;
    WNDCLASSA wc;
    HWND window = NULL;
    LARGE_INTEGER frequency;
    QXP_BENCHMARK_RESULT phases[4];
    QXP_BENCHMARK_RESULT fixed;
    QXP_BENCHMARK_RESULT shader;
    HRESULT hr = E_FAIL;
    unsigned phase;
    char detail[320];
    int rc = 1;

    (void)argv;

    if (argc != 1) {
        qxp_report_result(22, E_INVALIDARG, "arguments", "unknown-option");
        return 22;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        qxp_report_result(1, E_FAIL, "clock", "qpc-unavailable");
        goto done;
    }
    module = LoadLibraryA("C:\\Windows\\System32\\d3d8.dll");
    if (!module) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        qxp_report_result(2, hr, "module", "system32-d3d8");
        goto done;
    }
    entry.proc = GetProcAddress(module, "Direct3DCreate8");
    create = entry.create;
    if (!create) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        qxp_report_result(3, hr, "module", "Direct3DCreate8-export");
        goto done;
    }
    d3d = create(D3D_SDK_VERSION);
    if (!d3d) {
        hr = E_FAIL;
        qxp_report_result(4, hr, "create8", "null");
        goto done;
    }
    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D8_GetDeviceCaps(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (FAILED(hr) || caps.VertexShaderVersion < D3DVS_VERSION(1, 1) ||
        caps.MaxVertexShaderConst < 96) {
        _snprintf(detail, sizeof(detail), "caps-vs=%08lx maxconst=%lu",
                  (unsigned long)caps.VertexShaderVersion,
                  (unsigned long)caps.MaxVertexShaderConst);
        detail[sizeof(detail) - 1] = '\0';
        qxp_report_result(5, FAILED(hr) ? hr : D3DERR_NOTAVAILABLE,
                          "caps", detail);
        goto done;
    }
    memset(&display_mode, 0, sizeof(display_mode));
    hr = IDirect3D8_GetAdapterDisplayMode(
        d3d, D3DADAPTER_DEFAULT, &display_mode);
    if (FAILED(hr) || display_mode.Format != D3DFMT_X8R8G8B8) {
        qxp_report_result(6, FAILED(hr) ? hr : D3DERR_NOTAVAILABLE,
                          "display", "x8r8g8b8-required");
        goto done;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = qxp_window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPD3DVSBenchmark";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        qxp_report_result(7, hr, "window-class", "register");
        goto done;
    }
    window = CreateWindowA(
        wc.lpszClassName, "XPDriver D3D8 VS1.1 benchmark",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        400, 300, NULL, NULL, wc.hInstance, NULL);
    if (!window) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        qxp_report_result(8, hr, "window", "create");
        goto done;
    }

    for (phase = 0; phase < 4; ++phase) {
        hr = qxp_run_phase(d3d, window, display_mode.Format, &frequency,
                           order[phase], &phases[phase]);
        if (FAILED(hr)) {
            _snprintf(detail, sizeof(detail), "order=%u mode=%s",
                      phase + 1, qxp_mode_name(order[phase]));
            detail[sizeof(detail) - 1] = '\0';
            qxp_report_result(9, hr, "phase", detail);
            goto done;
        }
        qxp_report_phase(phase + 1, order[phase], &phases[phase]);
    }
    qxp_average_result(&fixed, &phases[0], &phases[3]);
    qxp_average_result(&shader, &phases[1], &phases[2]);
    _snprintf(detail, sizeof(detail),
              "rounds=%u order=fixed,shader,shader,fixed warmup-frames=%u frames-per-phase=%u draws-per-frame=%u timed-api-draws=%u total-api-draws=%u fixed-device-us=%I64u shader-device-us=%I64u shader-create-us=%I64u fixed-submit-ns=%I64u shader-submit-ns=%I64u fixed-present-us=%I64u shader-present-us=%I64u",
              QXP_BENCHMARK_ROUNDS, QXP_BENCHMARK_WARMUP_FRAMES,
              QXP_BENCHMARK_FRAMES, QXP_BENCHMARK_DRAWS_PER_FRAME,
              QXP_BENCHMARK_TIMED_API_DRAWS,
              QXP_BENCHMARK_TOTAL_API_DRAWS, fixed.device_create_us,
              shader.device_create_us, shader.shader_create_us,
              fixed.submit_ns_per_draw, shader.submit_ns_per_draw,
              fixed.present_us, shader.present_us);
    detail[sizeof(detail) - 1] = '\0';
    qxp_report_result(0, S_OK, "complete", detail);
    rc = 0;

done:
    if (d3d) IDirect3D8_Release(d3d);
    if (window) DestroyWindow(window);
    if (module) FreeLibrary(module);
    return rc;
}
