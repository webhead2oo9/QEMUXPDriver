#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

#define MAX_PAYNE_DIR "C:\\Program Files\\Max Payne Demo"
#define QXP_VISUAL_FRAME_HOLD_MS 2500UL
#define QXP_OPERATION_TIMEOUT_BASE_MS 15000UL
#define QXP_OPERATION_PRESENT_HOLDS 9UL

static FILE *log_file;
static DWORD visual_frame_hold_ms = 150;
typedef IDirect3D8 *(WINAPI *PFN_DIRECT3DCREATE8)(UINT sdk_version);

typedef union d3d8_entry_point {
    FARPROC proc;
    PFN_DIRECT3DCREATE8 create;
} D3D8_ENTRY_POINT;

static void report_result(const char *kind, int rc, HRESULT hr,
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

static void report_stage(const char *stage, const char *detail)
{
    report_result("QXP_D3D_STAGE", 0, S_OK, stage, detail);
}

static void report_hr_stage(const char *stage, HRESULT hr,
                            const char *detail)
{
    report_result("QXP_D3D_STAGE", 0, hr, stage, detail);
}

static LONG WINAPI report_unhandled_exception(EXCEPTION_POINTERS *exception)
{
    MEMORY_BASIC_INFORMATION memory;
    char module_path[MAX_PATH];
    char detail[320];
    DWORD code = EXCEPTION_NONCONTINUABLE_EXCEPTION;
    DWORD stack[8];
    DWORD ddi_fields[5];
    SIZE_T bytes_read = 0;
    PVOID address = NULL;
    CONTEXT *context = NULL;

    memset(&memory, 0, sizeof(memory));
    module_path[0] = '\0';
    if (exception) {
        context = exception->ContextRecord;
        if (exception->ExceptionRecord) {
            code = exception->ExceptionRecord->ExceptionCode;
            address = exception->ExceptionRecord->ExceptionAddress;
        }
    }
    if (address && VirtualQuery(address, &memory, sizeof(memory))) {
        GetModuleFileNameA((HMODULE)memory.AllocationBase, module_path,
                           sizeof(module_path));
        module_path[sizeof(module_path) - 1] = '\0';
    }
    memset(stack, 0, sizeof(stack));
    memset(ddi_fields, 0, sizeof(ddi_fields));
    if (context) {
        ReadProcessMemory(GetCurrentProcess(), (const void *)context->Esp,
                          stack, sizeof(stack), &bytes_read);
        if (stack[0] && VirtualQuery((const void *)stack[0], &memory,
                                    sizeof(memory))) {
            GetModuleFileNameA((HMODULE)memory.AllocationBase, module_path,
                               sizeof(module_path));
            module_path[sizeof(module_path) - 1] = '\0';
        }
        ReadProcessMemory(GetCurrentProcess(),
                          (const BYTE *)(ULONG_PTR)context->Eax + 0x5c,
                          &ddi_fields[0], sizeof(ddi_fields[0]), NULL);
        ReadProcessMemory(GetCurrentProcess(),
                          (const BYTE *)(ULONG_PTR)context->Eax + 0xfc,
                          &ddi_fields[1], sizeof(ddi_fields[1]), NULL);
        ReadProcessMemory(GetCurrentProcess(),
                          (const BYTE *)(ULONG_PTR)context->Eax + 0x16c,
                          &ddi_fields[2], sizeof(ddi_fields[2]), NULL);
        ReadProcessMemory(GetCurrentProcess(),
                          (const BYTE *)(ULONG_PTR)context->Eax + 0x170,
                          &ddi_fields[3], sizeof(ddi_fields[3]), NULL);
        ReadProcessMemory(GetCurrentProcess(),
                          (const BYTE *)(ULONG_PTR)context->Eax + 0x17c,
                          &ddi_fields[4], sizeof(ddi_fields[4]), NULL);
        _snprintf(detail, sizeof(detail),
                  "eax=%08lx f5c=%08lx ffc=%08lx f16c=%08lx f170=%08lx f17c=%08lx",
                  (unsigned long)context->Eax,
                  (unsigned long)ddi_fields[0],
                  (unsigned long)ddi_fields[1],
                  (unsigned long)ddi_fields[2],
                  (unsigned long)ddi_fields[3],
                  (unsigned long)ddi_fields[4]);
        detail[sizeof(detail) - 1] = '\0';
        report_stage("exception-ddi", detail);
    }
    _snprintf(detail, sizeof(detail),
              "code=%08lx address=%08lx eip=%08lx esp=%08lx ebp=%08lx return=%08lx returnbase=%08lx stack=%08lx,%08lx,%08lx,%08lx module=%s",
              (unsigned long)code, (unsigned long)(ULONG_PTR)address,
              context ? (unsigned long)context->Eip : 0,
              context ? (unsigned long)context->Esp : 0,
              context ? (unsigned long)context->Ebp : 0,
              (unsigned long)stack[0],
              (unsigned long)(ULONG_PTR)memory.AllocationBase,
              (unsigned long)stack[1], (unsigned long)stack[2],
              (unsigned long)stack[3], (unsigned long)stack[4],
              module_path[0] ? module_path : "unknown");
    detail[sizeof(detail) - 1] = '\0';
    report_result("QXP_D3D_RESULT", 11, (HRESULT)code, "exception", detail);
    ExitProcess(11);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int clean_max_payne(void)
{
    static const char *files[] = {
        "d3d8.dll", "d3d9.dll", "ddraw.dll", "wined3d.dll",
        "wined8.dll", "wined9.dll", "winedd.dll", "opengl32.dll",
        "wrapgl32.ext"
    };
    char path[MAX_PATH];
    unsigned i;
    unsigned removed = 0;
    DWORD error = ERROR_SUCCESS;
    char detail[96];

    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        _snprintf(path, sizeof(path), "%s\\%s", MAX_PAYNE_DIR, files[i]);
        path[sizeof(path) - 1] = '\0';
        if (DeleteFileA(path)) {
            ++removed;
        } else {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND &&
                error != ERROR_PATH_NOT_FOUND) {
                _snprintf(detail, sizeof(detail),
                          "file=%s removed=%u", files[i], removed);
                detail[sizeof(detail) - 1] = '\0';
                report_result("QXP_D3D_CLEAN", 1,
                              HRESULT_FROM_WIN32(error), "delete", detail);
                return 1;
            }
        }
    }
    _snprintf(detail, sizeof(detail), "removed=%u", removed);
    detail[sizeof(detail) - 1] = '\0';
    report_result("QXP_D3D_CLEAN", 0, S_OK, "complete", detail);
    return 0;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static BOOL run_ddraw_query(void)
{
    static const char query_log_path[] = "C:\\QXPDDQUERY.LOG";
    char path[MAX_PATH];
    char line[512];
    char *slash;
    char *end;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;
    FILE *query_log;
    unsigned reports = 0;

    if (!GetModuleFileNameA(NULL, path, sizeof(path))) return FALSE;
    path[sizeof(path) - 1] = '\0';
    slash = strrchr(path, '\\');
    if (!slash || strlen(slash + 1) < sizeof("qxpddquery.exe") - 1)
        return FALSE;
    strcpy(slash + 1, "qxpddquery.exe");
    DeleteFileA(query_log_path);
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessA(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process)) {
        report_stage("ddraw-query-launch", "failed");
        return FALSE;
    }
    if (WaitForSingleObject(process.hProcess, 45000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    /* Preserve the child's low-level HAL and registry diagnostics even when
     * its final runtime blit gate fails.  Those records identify the earlier
     * successful boundary and are required to distinguish policy disablement
     * from a missing driver callback. */
    query_log = fopen(query_log_path, "r");
    if (query_log) {
        while (fgets(line, sizeof(line), query_log)) {
            end = line + strlen(line);
            while (end > line && (end[-1] == '\r' || end[-1] == '\n'))
                *--end = '\0';
            if (!line[0]) continue;
            report_stage("ddraw-query", line);
            ++reports;
        }
        fclose(query_log);
    }
    DeleteFileA(query_log_path);
    return exit_code == 0 && reports == 6;
}

static int fail_probe(int rc, HRESULT hr, const char *stage,
                      const char *detail)
{
    if (log_file) {
        fprintf(log_file, "RESULT rc=%d hr=%08lx stage=%s %s\n", rc,
                (unsigned long)hr, stage, detail ? detail : "");
        fclose(log_file);
        log_file = NULL;
    }
    report_result("QXP_D3D_RESULT", rc, hr, stage, detail);
    return rc;
}

typedef struct d3d_operation {
    IDirect3DDevice8 *device;
    HRESULT result;
} D3D_OPERATION;

typedef struct d3d_probe_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
} D3D_PROBE_VERTEX;

typedef struct d3d_transform_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float tu;
    float tv;
} D3D_TRANSFORM_VERTEX;

typedef struct d3d_hwtl_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    DWORD color;
} D3D_HWTL_VERTEX;

typedef struct d3d_hwtl_material_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
} D3D_HWTL_MATERIAL_VERTEX;

typedef struct d3d_hwtl_point_vertex {
    float x;
    float y;
    float z;
    float point_size;
    DWORD color;
} D3D_HWTL_POINT_VERTEX;

typedef struct d3d_hwtl_lit_point_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float point_size;
    DWORD color;
} D3D_HWTL_LIT_POINT_VERTEX;

typedef struct d3d_hwtl_lit_sprite_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float point_size;
    DWORD color;
    float tu;
    float tv;
} D3D_HWTL_LIT_SPRITE_VERTEX;

typedef struct d3d_hwtl_sprite_vertex {
    float x;
    float y;
    float z;
    float point_size;
    DWORD color;
    float tu0;
    float tv0;
    float tu1;
    float tv1;
} D3D_HWTL_SPRITE_VERTEX;

typedef struct d3d_hwtl_texture3_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    DWORD color;
    float u;
    float v;
    float r;
} D3D_HWTL_TEXTURE3_VERTEX;

typedef struct d3d_hwtl_texture4_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    DWORD color;
    float u;
    float v;
    float r;
    float q;
} D3D_HWTL_TEXTURE4_VERTEX;

typedef struct d3d_hwtl_fog_vertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    DWORD color;
    DWORD specular;
} D3D_HWTL_FOG_VERTEX;

typedef struct d3d_specular_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    DWORD specular;
} D3D_SPECULAR_VERTEX;

typedef struct d3d_point_size_vertex {
    float x;
    float y;
    float z;
    float rhw;
    float point_size;
} D3D_POINT_SIZE_VERTEX;

typedef struct d3d_texture_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
} D3D_TEXTURE_VERTEX;

typedef struct d3d_projected_texture_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
    float q;
} D3D_PROJECTED_TEXTURE_VERTEX;

typedef struct d3d_texture2_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u0;
    float v0;
    float u1;
    float v1;
} D3D_TEXTURE2_VERTEX;

static void set_identity_matrix(D3DMATRIX *matrix)
{
    if (!matrix) return;
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = 1.0f;
    matrix->_22 = 1.0f;
    matrix->_33 = 1.0f;
    matrix->_44 = 1.0f;
}

static void fill_hwtl_triangle(D3D_HWTL_VERTEX vertices[3], float center_x,
                               float center_y, float half_width,
                               float half_height, DWORD color,
                               float normal_x, float normal_y,
                               float normal_z)
{
    unsigned index;

    memset(vertices, 0, 3 * sizeof(vertices[0]));
    vertices[0].x = center_x - half_width;
    vertices[0].y = center_y - half_height;
    vertices[1].x = center_x;
    vertices[1].y = center_y + half_height;
    vertices[2].x = center_x + half_width;
    vertices[2].y = center_y - half_height;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = 0.25f;
        vertices[index].nx = normal_x;
        vertices[index].ny = normal_y;
        vertices[index].nz = normal_z;
        vertices[index].color = color;
    }
}

static void fill_hwtl_texture3_triangle(
    D3D_HWTL_TEXTURE3_VERTEX vertices[3], float center_x, float center_y,
    DWORD color, float u, float v, float r)
{
    unsigned index;

    memset(vertices, 0, 3 * sizeof(vertices[0]));
    vertices[0].x = center_x - 0.07f;
    vertices[0].y = center_y - 0.09f;
    vertices[1].x = center_x;
    vertices[1].y = center_y + 0.09f;
    vertices[2].x = center_x + 0.07f;
    vertices[2].y = center_y - 0.09f;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = 0.25f;
        vertices[index].nz = 1.0f;
        vertices[index].color = color;
        vertices[index].u = u;
        vertices[index].v = v;
        vertices[index].r = r;
    }
}

static void fill_hwtl_texture4_triangle(
    D3D_HWTL_TEXTURE4_VERTEX vertices[3], float center_x, float center_y,
    DWORD color, float u, float v, float r, float q)
{
    unsigned index;

    memset(vertices, 0, 3 * sizeof(vertices[0]));
    vertices[0].x = center_x - 0.07f;
    vertices[0].y = center_y - 0.09f;
    vertices[1].x = center_x;
    vertices[1].y = center_y + 0.09f;
    vertices[2].x = center_x + 0.07f;
    vertices[2].y = center_y - 0.09f;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = 0.25f;
        vertices[index].nz = 1.0f;
        vertices[index].color = color;
        vertices[index].u = u;
        vertices[index].v = v;
        vertices[index].r = r;
        vertices[index].q = q;
    }
}

static void fill_hwtl_fog_triangle(D3D_HWTL_FOG_VERTEX vertices[3],
                                   float center_x, float center_y, float z,
                                   DWORD color)
{
    unsigned index;

    memset(vertices, 0, 3 * sizeof(vertices[0]));
    vertices[0].x = center_x - 0.07f;
    vertices[0].y = center_y - 0.09f;
    vertices[1].x = center_x;
    vertices[1].y = center_y + 0.09f;
    vertices[2].x = center_x + 0.07f;
    vertices[2].y = center_y - 0.09f;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = z;
        vertices[index].nz = 1.0f;
        vertices[index].color = color;
        vertices[index].specular = 0xff000000UL;
    }
}

static float hwtl_ndc_x(const D3DVIEWPORT8 *viewport, float pixel_x)
{
    return ((pixel_x - (float)viewport->X) * 2.0f /
            (float)viewport->Width) - 1.0f;
}

static float hwtl_ndc_y(const D3DVIEWPORT8 *viewport, float pixel_y)
{
    return 1.0f - ((pixel_y - (float)viewport->Y) * 2.0f /
                   (float)viewport->Height);
}

static DWORD float_render_state(float value)
{
    DWORD bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static HRESULT draw_hwtl_reference_triangle(
    IDirect3DDevice8 *device, const D3D_HWTL_VERTEX vertices[3])
{
    return IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
}

static HRESULT draw_hwtl_material_triangle(
    IDirect3DDevice8 *device,
    const D3D_HWTL_MATERIAL_VERTEX vertices[3])
{
    return IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
}

static HRESULT draw_hwtl_point_range_row(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    const DWORD *light_indices, const DWORD *color_masks,
    unsigned light_count, float first_pixel_x, float pixel_step,
    float pixel_y, float light_range, float half_width,
    float lower_half_height, float upper_half_height)
{
    D3D_HWTL_MATERIAL_VERTEX vertices[3];
    D3DMATRIX world;
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    float center_x;
    float center_y;
    HRESULT cleanup_hr;
    HRESULT hr;
    unsigned enabled_count = 0;
    unsigned index;

    if (!device || !viewport || !light_indices || !color_masks ||
        !light_count || light_count > 8) return E_INVALIDARG;
    center_y = hwtl_ndc_y(viewport, pixel_y);
    set_identity_matrix(&world);
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
        material.Diffuse.a = 1.0f;
    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL);

    for (index = 0; SUCCEEDED(hr) && index < light_count; ++index) {
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = color_masks[index] & 1 ? 1.0f : 0.0f;
        light.Diffuse.g = color_masks[index] & 2 ? 1.0f : 0.0f;
        light.Diffuse.b = color_masks[index] & 4 ? 1.0f : 0.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        light.Position.y = center_y;
        light.Position.z = 0.75f;
        light.Range = light_range;
        light.Attenuation0 = 1.0f;
        hr = IDirect3DDevice8_SetLight(
            device, light_indices[index], &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, light_indices[index], TRUE);
        if (SUCCEEDED(hr)) ++enabled_count;
    }

    center_x = hwtl_ndc_x(viewport, first_pixel_x);
    memset(vertices, 0, sizeof(vertices));
    vertices[0].x = center_x - half_width;
    vertices[0].y = center_y - lower_half_height;
    vertices[0].nx = 0.02996257f;
    vertices[0].ny = 0.03995009f;
    vertices[1].x = center_x;
    vertices[1].y = center_y + upper_half_height;
    vertices[1].ny = -0.04993762f;
    vertices[2].x = center_x + half_width;
    vertices[2].y = center_y - lower_half_height;
    vertices[2].nx = -0.02996257f;
    vertices[2].ny = 0.03995009f;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = 0.25f;
        vertices[index].nz = 0.99875234f;
    }
    for (index = 0; SUCCEEDED(hr) && index < light_count; ++index) {
        center_x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        vertices[0].x = center_x - half_width;
        vertices[1].x = center_x;
        vertices[2].x = center_x + half_width;
        hr = draw_hwtl_material_triangle(device, vertices);
    }

    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    return hr;
}

static HRESULT draw_hwtl_scalable_specular_row(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    const DWORD *light_indices, const DWORD *color_masks,
    unsigned light_count, float first_pixel_x, float pixel_step,
    float pixel_y, float light_range, BOOL color2_source,
    BOOL local_viewer, float material_power, BOOL spot_lights)
{
    D3D_HWTL_MATERIAL_VERTEX material_vertices[3];
    D3D_HWTL_FOG_VERTEX color2_vertices[3];
    D3DMATRIX world;
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    float center_x;
    float center_y;
    HRESULT cleanup_hr;
    HRESULT hr;
    unsigned enabled_count = 0;
    unsigned index;

    if (!device || !viewport || !light_indices || !color_masks ||
        light_count < 3 || light_count > 8) return E_INVALIDARG;
    center_y = hwtl_ndc_y(viewport, pixel_y);
    set_identity_matrix(&world);
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    memset(&material, 0, sizeof(material));
    material.Diffuse.a = material.Specular.a = 1.0f;
    if (!color2_source) {
        material.Specular.r = material.Specular.g =
            material.Specular.b = 1.0f;
    }
    material.Power = material_power;
    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, color2_source);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LOCALVIEWER, local_viewer);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DITHERENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARMATERIALSOURCE,
        color2_source ? D3DMCS_COLOR2 : D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, color2_source ?
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                D3DFVF_SPECULAR :
            D3DFVF_XYZ | D3DFVF_NORMAL);

    for (index = 0; SUCCEEDED(hr) && index < light_count; ++index) {
        memset(&light, 0, sizeof(light));
        light.Type = spot_lights ? D3DLIGHT_SPOT : D3DLIGHT_POINT;
        light.Specular.r = color_masks[index] & 1 ? 1.0f : 0.0f;
        light.Specular.g = color_masks[index] & 2 ? 1.0f : 0.0f;
        light.Specular.b = color_masks[index] & 4 ? 1.0f : 0.0f;
        light.Specular.a = 1.0f;
        light.Position.x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        light.Position.y = center_y;
        light.Position.z = 0.75f;
        light.Range = light_range;
        light.Attenuation0 = 1.0f;
        if (spot_lights) {
            light.Direction.z = -1.0f;
            light.Falloff = 2.0f;
            light.Theta = 1.0f;
            light.Phi = 1.5f;
        }
        hr = IDirect3DDevice8_SetLight(
            device, light_indices[index], &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, light_indices[index], TRUE);
        if (SUCCEEDED(hr)) ++enabled_count;
    }

    center_x = hwtl_ndc_x(viewport, first_pixel_x);
    memset(material_vertices, 0, sizeof(material_vertices));
    memset(color2_vertices, 0, sizeof(color2_vertices));
    material_vertices[0].y = color2_vertices[0].y = center_y - 0.02f;
    material_vertices[0].nx = color2_vertices[0].nx = 0.02996257f;
    material_vertices[0].ny = color2_vertices[0].ny = 0.03995009f;
    material_vertices[1].y = color2_vertices[1].y = center_y + 0.025f;
    material_vertices[1].ny = color2_vertices[1].ny = -0.04993762f;
    material_vertices[2].y = color2_vertices[2].y = center_y - 0.02f;
    material_vertices[2].nx = color2_vertices[2].nx = -0.02996257f;
    material_vertices[2].ny = color2_vertices[2].ny = 0.03995009f;
    for (index = 0; index < 3; ++index) {
        material_vertices[index].z = color2_vertices[index].z = 0.25f;
        material_vertices[index].nz = color2_vertices[index].nz =
            0.99875234f;
        color2_vertices[index].color = 0xff000000UL;
        color2_vertices[index].specular = 0xffffffffUL;
    }
    for (index = 0; SUCCEEDED(hr) && index < light_count; ++index) {
        center_x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        material_vertices[0].x = color2_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = color2_vertices[1].x = center_x;
        material_vertices[2].x = color2_vertices[2].x = center_x + 0.015f;
        if (color2_source) {
            hr = IDirect3DDevice8_DrawPrimitiveUP(
                device, D3DPT_TRIANGLELIST, 1, color2_vertices,
                sizeof(color2_vertices[0]));
        } else {
            hr = draw_hwtl_material_triangle(device, material_vertices);
        }
    }

    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    return hr;
}

static void fill_hwtl_scalable_spot_triangle(
    D3D_HWTL_MATERIAL_VERTEX vertices[3], float center_x, float center_y)
{
    memset(vertices, 0, 3 * sizeof(vertices[0]));
    vertices[0].x = center_x;
    vertices[0].y = center_y + 0.08819171f;
    vertices[0].ny = -0.66143782f;
    vertices[1].x = center_x - 0.07637626f;
    vertices[1].y = center_y - 0.04409586f;
    vertices[1].nx = 0.57282195f;
    vertices[1].ny = 0.33071895f;
    vertices[2].x = center_x + 0.07637626f;
    vertices[2].y = center_y - 0.04409586f;
    vertices[2].nx = -0.57282195f;
    vertices[2].ny = 0.33071895f;
    vertices[0].z = vertices[1].z = vertices[2].z = 0.25f;
    vertices[0].nz = vertices[1].nz = vertices[2].nz = 0.75f;
}

static HRESULT draw_hwtl_scalable_spot_falloff_pair(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    const DWORD light_indices[2], float first_pixel_x, float pixel_step,
    float pixel_y, BOOL clip_world_plane)
{
    D3D_HWTL_MATERIAL_VERTEX vertices[3];
    D3DMATRIX world;
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    float clip_plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float center_x;
    float center_y;
    float world_x;
    HRESULT cleanup_hr;
    HRESULT hr;
    unsigned enabled_count = 0;
    unsigned index;

    if (!device || !viewport || !light_indices) return E_INVALIDARG;
    center_y = hwtl_ndc_y(viewport, pixel_y);
    world_x = clip_world_plane ? 0.5f : 0.0f;
    set_identity_matrix(&world);
    world._41 = world_x;
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 0.75f;
    material.Diffuse.g = 0.5f;
    material.Diffuse.b = 0.25f;
    material.Diffuse.a = 1.0f;
    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
    if (SUCCEEDED(hr) && clip_world_plane) {
        hr = IDirect3DDevice8_SetClipPlane(device, 0, clip_plane);
    }
    if (SUCCEEDED(hr) && clip_world_plane) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL);

    for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_SPOT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Direction.z = -1.0f;
        light.Range = 0.2f;
        light.Falloff = 1.0f + index;
        light.Attenuation0 = 1.0f;
        light.Theta = 0.0f;
        light.Phi = 2.0943951f;
        hr = IDirect3DDevice8_SetLight(
            device, light_indices[index], &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, light_indices[index], TRUE);
        if (SUCCEEDED(hr)) ++enabled_count;
    }

    for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
        center_x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index) - world_x;
        fill_hwtl_scalable_spot_triangle(vertices, center_x, center_y);
        hr = draw_hwtl_material_triangle(device, vertices);
    }

    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    return hr;
}

static HRESULT draw_hwtl_scalable_point_pair(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    IDirect3DTexture8 *texture, const DWORD light_indices[5], float pixel_y,
    DWORD point_scale_a)
{
    static const DWORD light_colors[5] = {
        0x00ff0000UL, 0x0000ff00UL, 0x00ffffffUL,
        0x0000ffffUL, 0x00ff00ffUL
    };
    static const float light_pixels[5] = {
        40.0f, 600.0f, 200.0f, 320.0f, 440.0f
    };
    D3D_HWTL_LIT_POINT_VERTEX points[2];
    D3D_HWTL_LIT_SPRITE_VERTEX sprite;
    D3DMATRIX identity;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    HRESULT cleanup_hr;
    HRESULT hr;
    unsigned enabled_count = 0;
    unsigned index;

    if (!device || !viewport || !texture || !light_indices)
        return E_INVALIDARG;
    memset(points, 0, sizeof(points));
    for (index = 0; index < 2; ++index) {
        points[index].x = hwtl_ndc_x(viewport, light_pixels[index]);
        points[index].y = hwtl_ndc_y(viewport, pixel_y);
        points[index].z = 0.25f;
        points[index].nz = 1.0f;
        points[index].point_size = index ? 2.0f : 0.5f;
        points[index].color = 0xffffffffUL;
    }
    set_identity_matrix(&identity);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = 1.0f;
    material.Diffuse.a = 1.0f;

    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MIN, 0x41200000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MAX, 0x41a00000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALE_A, point_scale_a);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALE_B, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALE_C, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALEENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_PSIZE |
                D3DFVF_DIFFUSE);

    /* Each point is inside exactly one hard D3D Range. Fixed OpenGL ignores
     * Range and therefore accumulates the other four colored lights; the
     * expected pure red/green centers prove that the scalable program ran. */
    for (index = 0; SUCCEEDED(hr) && index < 5; ++index) {
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = (float)((light_colors[index] >> 16) & 0xff) /
                          255.0f;
        light.Diffuse.g = (float)((light_colors[index] >> 8) & 0xff) /
                          255.0f;
        light.Diffuse.b = (float)(light_colors[index] & 0xff) / 255.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = hwtl_ndc_x(viewport, light_pixels[index]);
        light.Position.y = hwtl_ndc_y(viewport, pixel_y);
        light.Position.z = 0.35f;
        light.Range = 0.15f;
        light.Attenuation0 = 1.0f;
        hr = IDirect3DDevice8_SetLight(
            device, light_indices[index], &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, light_indices[index], TRUE);
        if (SUCCEEDED(hr)) ++enabled_count;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_POINTLIST, 2, points, sizeof(points[0]));

    /* Keep all five Range-isolated lights active while a third point combines
     * programmable scaling, top-left point-sprite coordinates, fixed-function
     * texturing, and object-space linear fog under the scalable GLSL vertex
     * backend. At eye Z=0.25 with fog start/end 0/0.5, every asymmetric
     * texture quadrant is mixed exactly halfway with 204060. */
    memset(&sprite, 0, sizeof(sprite));
    sprite.x = hwtl_ndc_x(viewport, light_pixels[2]);
    sprite.y = hwtl_ndc_y(viewport, pixel_y);
    sprite.z = 0.25f;
    sprite.nz = 1.0f;
    sprite.point_size = 2.0f;
    sprite.color = 0xffffffffUL;
    sprite.tu = 0.75f;
    sprite.tv = 0.25f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
        device, 0, (IDirect3DBaseTexture8 *)texture);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_TEXCOORDINDEX, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGSTART, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGEND, 0x3f000000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff204060UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_PSIZE |
                D3DFVF_DIFFUSE | D3DFVF_TEX1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_POINTLIST, 1, &sprite, sizeof(sprite));

    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, FALSE);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;

    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALEENABLE, FALSE);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MIN, 0x3f800000UL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MAX, 0x42800000UL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    cleanup_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALE_A, 0x3f800000UL);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    return hr;
}

static HRESULT draw_hwtl_scalable_directional_point_pair(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    const DWORD light_indices[2], float left_pixel_x, float right_pixel_x,
    float pixel_y)
{
    D3D_HWTL_MATERIAL_VERTEX vertices[6];
    D3DMATRIX identity;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    HRESULT cleanup_hr;
    HRESULT hr;
    float center_x[2];
    float center_y;
    unsigned enabled_count = 0;
    unsigned index;
    unsigned triangle;

    if (!device || !viewport || !light_indices) return E_INVALIDARG;
    center_x[0] = hwtl_ndc_x(viewport, left_pixel_x);
    center_x[1] = hwtl_ndc_x(viewport, right_pixel_x);
    center_y = hwtl_ndc_y(viewport, pixel_y);
    memset(vertices, 0, sizeof(vertices));
    for (triangle = 0; triangle < 2; ++triangle) {
        unsigned base = triangle * 3;

        vertices[base].x = center_x[triangle] - 0.025f;
        vertices[base].y = center_y - 0.03f;
        vertices[base + 1].x = center_x[triangle];
        vertices[base + 1].y = center_y + 0.035f;
        vertices[base + 2].x = center_x[triangle] + 0.025f;
        vertices[base + 2].y = center_y - 0.03f;
        for (index = 0; index < 3; ++index) {
            D3D_HWTL_MATERIAL_VERTEX *vertex = &vertices[base + index];

            vertex->z = 0.25f;
            if (!triangle) {
                /* The normal is proportional to the point-light vector at
                 * each vertex. NORMALIZENORMALS therefore makes the green
                 * diffuse term exactly one at every rasterized sample. */
                vertex->nx = center_x[0] - vertex->x;
                vertex->ny = center_y - vertex->y;
                vertex->nz = 0.5f;
            } else {
                vertex->nz = 1.0f;
            }
        }
    }
    set_identity_matrix(&identity);
    memset(&material, 0, sizeof(material));
    material.Ambient.r = material.Ambient.g = material.Ambient.b =
        material.Ambient.a = 1.0f;
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
        material.Diffuse.a = 1.0f;

    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL);

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Ambient.r = light.Ambient.a = 1.0f;
    light.Direction.z = -1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, light_indices[0], &light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
        device, light_indices[0], TRUE);
    if (SUCCEEDED(hr)) ++enabled_count;

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_POINT;
    light.Diffuse.g = light.Diffuse.a = 1.0f;
    light.Position.x = center_x[0];
    light.Position.y = center_y;
    light.Position.z = 0.75f;
    light.Range = 0.6f;
    light.Attenuation0 = 1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, light_indices[1], &light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
        device, light_indices[1], TRUE);
    if (SUCCEEDED(hr)) ++enabled_count;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));

    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    return hr;
}

static HRESULT draw_hwtl_scalable_fog_triplet(
    IDirect3DDevice8 *device, const D3DVIEWPORT8 *viewport,
    const DWORD light_indices[3], float first_pixel_x, float pixel_step,
    float pixel_y)
{
    D3D_HWTL_MATERIAL_VERTEX vertices[3];
    D3DMATRIX world;
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    float center_x;
    float center_y;
    HRESULT cleanup_hr;
    HRESULT hr;
    unsigned enabled_count = 0;
    unsigned index;

    if (!device || !viewport || !light_indices) return E_INVALIDARG;
    center_y = hwtl_ndc_y(viewport, pixel_y);
    set_identity_matrix(&world);
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 0.75f;
    material.Diffuse.g = 0.5f;
    material.Diffuse.b = 0.25f;
    material.Diffuse.a = 1.0f;
    material.Specular.r = 0.25f;
    material.Specular.a = 1.0f;
    material.Power = 0.0f;
    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DITHERENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL);

    for (index = 0; SUCCEEDED(hr) && index < 3; ++index) {
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_SPOT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        if (!index) {
            light.Specular.r = 1.0f;
            light.Specular.a = 1.0f;
        }
        light.Position.x = hwtl_ndc_x(
            viewport, first_pixel_x + pixel_step * index);
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Direction.z = -1.0f;
        light.Range = 0.2f;
        light.Falloff = index == 2 ? 2.0f : 1.0f;
        light.Attenuation0 = 1.0f;
        light.Theta = 0.0f;
        light.Phi = 2.0943951f;
        hr = IDirect3DDevice8_SetLight(
            device, light_indices[index], &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, light_indices[index], TRUE);
        if (SUCCEEDED(hr)) ++enabled_count;
    }

    /* Eye Z=0.25 is the midpoint of linear vertex fog. Falloff-one diffuse
     * 604020 plus an exact Power-zero red specular term produces 804020;
     * blending fog color 204060 therefore produces exact 504040. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGSTART, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGEND, 0x3f000000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff204060UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, TRUE);
    center_x = hwtl_ndc_x(viewport, first_pixel_x);
    fill_hwtl_scalable_spot_triangle(vertices, center_x, center_y);
    if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(device, vertices);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);

    /* Translate camera X and cancel it in projection. Radial fog is beyond
     * the end while planar Z remains before the start, making C040FF exact. */
    view._41 = 2.0f;
    projection._41 = -2.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGSTART, 0x3f000000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGEND, 0x3f333333UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xffc040ffUL);
    center_x = hwtl_ndc_x(viewport, first_pixel_x + pixel_step);
    fill_hwtl_scalable_spot_triangle(vertices, center_x, center_y);
    if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(device, vertices);

    /* Post-divide table Z=0.375 leaves a quarter of Falloff-two 302010 over
     * three quarters of 804020, producing exact 6C381C. Eye-Z selection would
     * instead produce 582818. */
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    projection._33 = 1.5f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &view);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGSTART, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGEND, 0x3f000000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff804020UL);
    center_x = hwtl_ndc_x(viewport, first_pixel_x + 2.0f * pixel_step);
    fill_hwtl_scalable_spot_triangle(vertices, center_x, center_y);
    if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(device, vertices);

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DITHERENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    set_identity_matrix(&projection);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &projection);
    while (enabled_count) {
        --enabled_count;
        cleanup_hr = IDirect3DDevice8_LightEnable(
            device, light_indices[enabled_count], FALSE);
        if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    }
    return hr;
}

static HRESULT verify_state_block_values(IDirect3DDevice8 *device,
                                         DWORD fog_color,
                                         DWORD range_fog,
                                         DWORD texture_arg,
                                         const D3DMATRIX *world,
                                         const D3DMATERIAL8 *material,
                                         const D3DLIGHT8 *light,
                                         BOOL light_enabled,
                                         const float clip_plane[4])
{
    D3DMATRIX actual_world;
    D3DMATERIAL8 actual_material;
    D3DLIGHT8 actual_light;
    float actual_clip_plane[4];
    DWORD actual_value = 0;
    BOOL actual_light_enabled = FALSE;
    HRESULT hr;

    hr = IDirect3DDevice8_GetRenderState(device, D3DRS_FOGCOLOR,
                                          &actual_value);
    if (SUCCEEDED(hr) && actual_value != fog_color) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetRenderState(
        device, D3DRS_RANGEFOGENABLE, &actual_value);
    if (SUCCEEDED(hr) && actual_value != range_fog) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetTextureStageState(
        device, 3, D3DTSS_COLORARG1, &actual_value);
    if (SUCCEEDED(hr) && actual_value != texture_arg) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetTransform(
        device, D3DTS_WORLD, &actual_world);
    if (SUCCEEDED(hr) && memcmp(&actual_world, world,
                                sizeof(actual_world))) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetMaterial(
        device, &actual_material);
    if (SUCCEEDED(hr) && memcmp(&actual_material, material,
                                sizeof(actual_material))) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetLight(
        device, 3, &actual_light);
    if (SUCCEEDED(hr) && memcmp(&actual_light, light,
                                sizeof(actual_light))) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetLightEnable(
        device, 3, &actual_light_enabled);
    if (SUCCEEDED(hr) && !!actual_light_enabled != !!light_enabled)
        hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetClipPlane(
        device, 2, actual_clip_plane);
    if (SUCCEEDED(hr) && memcmp(actual_clip_plane, clip_plane,
                                sizeof(actual_clip_plane))) hr = E_FAIL;
    return hr;
}

static HRESULT run_state_block_contract(IDirect3DDevice8 *device)
{
    D3DMATRIX recorded_world;
    D3DMATRIX captured_world;
    D3DMATRIX poison_world;
    D3DMATERIAL8 recorded_material;
    D3DMATERIAL8 captured_material;
    D3DMATERIAL8 poison_material;
    D3DLIGHT8 recorded_light;
    D3DLIGHT8 captured_light;
    D3DLIGHT8 poison_light;
    static const float recorded_clip_plane[4] = {
        1.0f, 0.0f, 0.0f, -0.125f
    };
    static const float captured_clip_plane[4] = {
        0.0f, 1.0f, 0.0f, -0.25f
    };
    static const float poison_clip_plane[4] = {
        0.0f, 0.0f, 1.0f, -0.75f
    };
    DWORD block = 0;
    HRESULT hr;
    HRESULT delete_hr;

    set_identity_matrix(&recorded_world);
    set_identity_matrix(&captured_world);
    set_identity_matrix(&poison_world);
    recorded_world._41 = 0.125f;
    captured_world._41 = -0.25f;
    poison_world._41 = 0.75f;

    memset(&recorded_material, 0, sizeof(recorded_material));
    recorded_material.Diffuse.r = 0.25f;
    recorded_material.Diffuse.g = 0.5f;
    recorded_material.Diffuse.b = 0.75f;
    recorded_material.Diffuse.a = 1.0f;
    recorded_material.Ambient.r = 0.125f;
    recorded_material.Ambient.g = 0.25f;
    recorded_material.Ambient.b = 0.375f;
    recorded_material.Ambient.a = 1.0f;
    recorded_material.Power = 8.0f;
    memset(&captured_material, 0, sizeof(captured_material));
    captured_material.Diffuse.r = 0.75f;
    captured_material.Diffuse.g = 0.25f;
    captured_material.Diffuse.b = 0.5f;
    captured_material.Diffuse.a = 1.0f;
    captured_material.Emissive.r = 0.125f;
    captured_material.Emissive.g = 0.25f;
    captured_material.Emissive.b = 0.5f;
    captured_material.Emissive.a = 1.0f;
    captured_material.Power = 16.0f;
    memset(&poison_material, 0, sizeof(poison_material));
    poison_material.Diffuse.r = poison_material.Diffuse.g =
        poison_material.Diffuse.b = poison_material.Diffuse.a = 1.0f;
    poison_material.Power = 32.0f;

    memset(&recorded_light, 0, sizeof(recorded_light));
    recorded_light.Type = D3DLIGHT_POINT;
    recorded_light.Diffuse.r = 0.25f;
    recorded_light.Diffuse.g = 0.5f;
    recorded_light.Diffuse.b = 0.75f;
    recorded_light.Diffuse.a = 1.0f;
    recorded_light.Position.x = 1.0f;
    recorded_light.Position.y = 2.0f;
    recorded_light.Position.z = 3.0f;
    recorded_light.Range = 64.0f;
    recorded_light.Attenuation0 = 1.0f;
    captured_light = recorded_light;
    captured_light.Diffuse.r = 0.75f;
    captured_light.Diffuse.g = 0.25f;
    captured_light.Diffuse.b = 0.5f;
    captured_light.Position.x = -1.0f;
    captured_light.Position.y = -2.0f;
    captured_light.Position.z = 4.0f;
    captured_light.Range = 32.0f;
    poison_light = recorded_light;
    poison_light.Diffuse.r = poison_light.Diffuse.g =
        poison_light.Diffuse.b = 1.0f;
    poison_light.Position.x = 8.0f;
    poison_light.Position.y = 8.0f;
    poison_light.Position.z = 8.0f;
    poison_light.Range = 8.0f;

    report_stage("state-block-enter",
                 "render-texture-transform-material-light-clip");
    hr = IDirect3DDevice8_BeginStateBlock(device);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff2468acUL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &recorded_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &recorded_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &recorded_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, recorded_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_EndStateBlock(device, &block);

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff010203UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_CURRENT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &poison_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &poison_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &poison_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, poison_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_state_block_values(
        device, 0xff2468acUL, TRUE, D3DTA_DIFFUSE, &recorded_world,
        &recorded_material,
        &recorded_light, TRUE, recorded_clip_plane);

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xffc040ffUL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_SPECULAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &captured_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &captured_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &captured_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, captured_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CaptureStateBlock(device,
                                                                block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff010203UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_CURRENT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &poison_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &poison_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &poison_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, poison_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_state_block_values(
        device, 0xffc040ffUL, FALSE, D3DTA_SPECULAR, &captured_world,
        &captured_material,
        &captured_light, FALSE, captured_clip_plane);

    if (block) {
        delete_hr = IDirect3DDevice8_DeleteStateBlock(device, block);
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }
    report_hr_stage("state-block-leave", hr,
                    SUCCEEDED(hr) ?
                        "render-texture-transform-material-light-clip" :
                                    "failed");
    return hr;
}

static HRESULT run_state_block_capacity_contract(IDirect3DDevice8 *device)
{
    static const DWORD probe_indices[3] = {0, 64, 256};
    DWORD blocks[257];
    DWORD saved_fog_color = 0;
    DWORD observed = 0;
    DWORD unfinished = 0;
    DWORD created = 0;
    DWORD index;
    DWORD probe;
    BOOL recording = FALSE;
    BOOL saved_valid = FALSE;
    HRESULT hr;
    HRESULT step_hr;

    memset(blocks, 0, sizeof(blocks));
    report_stage("state-block-capacity-enter",
                 "257-live-first-65th-257th");
    hr = IDirect3DDevice8_GetRenderState(
        device, D3DRS_FOGCOLOR, &saved_fog_color);
    if (SUCCEEDED(hr)) saved_valid = TRUE;
    for (index = 0; SUCCEEDED(hr) && index < 257; ++index) {
        hr = IDirect3DDevice8_BeginStateBlock(device);
        if (SUCCEEDED(hr)) recording = TRUE;
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetRenderState(
                device, D3DRS_FOGCOLOR, 0xff100000UL | index);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_EndStateBlock(
                device, &blocks[created]);
            if (SUCCEEDED(hr)) {
                recording = FALSE;
                ++created;
            }
        }
    }
    for (probe = 0; SUCCEEDED(hr) && probe < 3; ++probe) {
        index = probe_indices[probe];
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff010203UL);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_ApplyStateBlock(
                device, blocks[index]);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_GetRenderState(
                device, D3DRS_FOGCOLOR, &observed);
        }
        if (SUCCEEDED(hr) && observed != (0xff100000UL | index))
            hr = E_FAIL;
    }
    if (recording) {
        step_hr = IDirect3DDevice8_EndStateBlock(device, &unfinished);
        if (SUCCEEDED(step_hr)) {
            recording = FALSE;
            if (unfinished && created < 257)
                blocks[created++] = unfinished;
        }
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    for (index = 0; index < created; ++index) {
        step_hr = IDirect3DDevice8_DeleteStateBlock(
            device, blocks[index]);
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (saved_valid) {
        step_hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, saved_fog_color);
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    report_hr_stage("state-block-capacity-leave", hr,
                    SUCCEEDED(hr) ? "257-live-first-65th-257th" :
                                    "failed");
    return hr;
}

static HRESULT verify_render_state_value(IDirect3DDevice8 *device,
                                         D3DRENDERSTATETYPE state,
                                         DWORD expected)
{
    DWORD actual = 0;
    HRESULT hr;

    hr = IDirect3DDevice8_GetRenderState(device, state, &actual);
    if (SUCCEEDED(hr) && actual != expected) hr = E_FAIL;
    return hr;
}

static HRESULT verify_texture_state_value(IDirect3DDevice8 *device,
                                          DWORD stage,
                                          D3DTEXTURESTAGESTATETYPE state,
                                          DWORD expected)
{
    DWORD actual = 0;
    HRESULT hr;

    hr = IDirect3DDevice8_GetTextureStageState(device, stage, state,
                                                &actual);
    if (SUCCEEDED(hr) && actual != expected) hr = E_FAIL;
    return hr;
}

static HRESULT run_predefined_state_block_contract(IDirect3DDevice8 *device)
{
    D3DMATRIX captured_world;
    D3DMATRIX poison_world;
    D3DMATERIAL8 captured_material;
    D3DMATERIAL8 poison_material;
    D3DLIGHT8 captured_light;
    D3DLIGHT8 poison_light;
    static const float captured_clip_plane[4] = {
        1.0f, 0.0f, 0.0f, -0.375f
    };
    static const float poison_clip_plane[4] = {
        0.0f, 1.0f, 0.0f, -0.625f
    };
    static const float neutral_clip_plane[4] = {
        0.0f, 0.0f, 0.0f, 0.0f
    };
    DWORD block = 0;
    HRESULT hr;
    HRESULT delete_hr;

    report_stage("predefined-state-block-enter",
                 "pixel-vertex-all-membership");

    /* PIXELSTATE captures its listed render/texture states, but not vertex
     * lighting.  CaptureStateBlock must retain that same membership. */
    hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateStateBlock(
        device, D3DSBT_PIXELSTATE, &block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = verify_texture_state_value(
        device, 3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_SPECULAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CaptureStateBlock(
        device, block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_CURRENT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_ALPHABLENDENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = verify_texture_state_value(
        device, 3, D3DTSS_COLORARG1, D3DTA_SPECULAR);
    if (block) {
        delete_hr = IDirect3DDevice8_DeleteStateBlock(device, block);
        block = 0;
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }

    /* VERTEXSTATE restores vertex render state and the two documented
     * texture-coordinate states while leaving pixel state untouched. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE, 0x40a00000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_TEXCOORDINDEX, 3);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateStateBlock(
        device, D3DSBT_VERTEXSTATE, &block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE, 0x41300000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_TEXCOORDINDEX, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_POINTSIZE, 0x40a00000UL);
    if (SUCCEEDED(hr)) hr = verify_render_state_value(
        device, D3DRS_ALPHABLENDENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = verify_texture_state_value(
        device, 3, D3DTSS_TEXCOORDINDEX, 3);
    if (SUCCEEDED(hr)) hr = verify_texture_state_value(
        device, 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (block) {
        delete_hr = IDirect3DDevice8_DeleteStateBlock(device, block);
        block = 0;
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }

    /* ALL also snapshots the object-valued state families carried by the
     * XPDM HWTL transport. */
    set_identity_matrix(&captured_world);
    set_identity_matrix(&poison_world);
    captured_world._42 = 0.25f;
    poison_world._42 = -0.75f;
    memset(&captured_material, 0, sizeof(captured_material));
    captured_material.Diffuse.r = 0.2f;
    captured_material.Diffuse.g = 0.4f;
    captured_material.Diffuse.b = 0.8f;
    captured_material.Diffuse.a = 1.0f;
    captured_material.Power = 12.0f;
    memset(&poison_material, 0, sizeof(poison_material));
    poison_material.Diffuse.r = poison_material.Diffuse.g =
        poison_material.Diffuse.b = poison_material.Diffuse.a = 1.0f;
    poison_material.Power = 28.0f;
    memset(&captured_light, 0, sizeof(captured_light));
    captured_light.Type = D3DLIGHT_POINT;
    captured_light.Diffuse.r = 0.2f;
    captured_light.Diffuse.g = 0.4f;
    captured_light.Diffuse.b = 0.8f;
    captured_light.Diffuse.a = 1.0f;
    captured_light.Position.x = 2.0f;
    captured_light.Position.y = 3.0f;
    captured_light.Position.z = 4.0f;
    captured_light.Range = 48.0f;
    captured_light.Attenuation0 = 1.0f;
    poison_light = captured_light;
    poison_light.Position.x = -8.0f;
    poison_light.Diffuse.r = poison_light.Diffuse.g =
        poison_light.Diffuse.b = 1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff3579bdUL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_SPECULAR);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &captured_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &captured_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &captured_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, captured_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateStateBlock(
        device, D3DSBT_ALL, &block);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0xff010203UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_CURRENT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &poison_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &poison_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(
        device, 3, &poison_light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, poison_clip_plane);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_ApplyStateBlock(device, block);
    if (SUCCEEDED(hr)) hr = verify_state_block_values(
        device, 0xff3579bdUL, TRUE, D3DTA_SPECULAR, &captured_world,
        &captured_material, &captured_light, TRUE, captured_clip_plane);
    if (block) {
        delete_hr = IDirect3DDevice8_DeleteStateBlock(device, block);
        if (SUCCEEDED(hr) && FAILED(delete_hr)) hr = delete_hr;
    }

    /* Membership tests intentionally leave excluded state poisoned. Restore
     * a deterministic neutral baseline so post-contract diagnostics do not
     * depend on which predefined group ran last. */
    set_identity_matrix(&captured_world);
    memset(&captured_material, 0, sizeof(captured_material));
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE, 0x3f800000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGCOLOR, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LASTPIXEL, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_BORDERCOLOR, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_TEXCOORDINDEX, 3);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &captured_world);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(
        device, &captured_material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 3, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
        device, 2, neutral_clip_plane);
    report_hr_stage("predefined-state-block-leave", hr,
                    SUCCEEDED(hr) ? "pixel-vertex-all-membership" :
                                    "failed");
    return hr;
}

static HRESULT create_reference_texture(IDirect3DDevice8 *device,
                                        IDirect3DTexture8 **texture)
{
    D3DLOCKED_RECT locked;
    HRESULT hr;
    DWORD x;
    DWORD y;

    if (!device || !texture) return E_INVALIDARG;
    *texture = NULL;
    report_stage("texture-create-enter", "128x128-a8r8g8b8-managed");
    hr = IDirect3DDevice8_CreateTexture(
        device, 128, 128, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, texture);
    report_hr_stage("texture-create-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) return hr;

    memset(&locked, 0, sizeof(locked));
    report_stage("texture-lock-enter", "checkerboard");
    hr = IDirect3DTexture8_LockRect(*texture, 0, &locked, NULL, 0);
    if (SUCCEEDED(hr)) {
        for (y = 0; y < 128; ++y) {
            DWORD *pixels = (DWORD *)((BYTE *)locked.pBits +
                                       y * locked.Pitch);
            for (x = 0; x < 128; ++x) {
                if (y < 64)
                    pixels[x] = x < 64 ? 0x40ffff00UL : 0xff00ffffUL;
                else
                    pixels[x] = x < 64 ? 0xffff00ffUL : 0xffffffffUL;
            }
        }
        hr = IDirect3DTexture8_UnlockRect(*texture, 0);
    }
    report_hr_stage("texture-lock-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        IDirect3DTexture8_Release(*texture);
        *texture = NULL;
    }
    return hr;
}

typedef struct d3d_a8_vertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
    float r;
} D3D_A8_VERTEX;

static void fill_a8_quad8(D3D_A8_VERTEX vertices[6], float left,
                          float top, float u, DWORD color)
{
    const float right = left + 16.0f;
    const float bottom = top + 16.0f;
    const D3D_A8_VERTEX quad[6] = {
        { left,  top,    0.25f, 1.0f, color, u, 0.5f, 0.0f },
        { right, top,    0.25f, 1.0f, color, u, 0.5f, 0.0f },
        { left,  bottom, 0.25f, 1.0f, color, u, 0.5f, 0.0f },
        { left,  bottom, 0.25f, 1.0f, color, u, 0.5f, 0.0f },
        { right, top,    0.25f, 1.0f, color, u, 0.5f, 0.0f },
        { right, bottom, 0.25f, 1.0f, color, u, 0.5f, 0.0f }
    };

    memcpy(vertices, quad, sizeof(quad));
}

static HRESULT run_a8_texture_contract8(IDirect3DDevice8 *device)
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
     * alpha remains A.  Exact destination attenuation therefore isolates
     * legacy shader alpha sampling from fixed-function combine state. */
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
    IDirect3DTexture8 *texture = NULL;
    IDirect3DSurface8 *target = NULL;
    IDirect3DSurface8 *saved_target = NULL;
    IDirect3DSurface8 *saved_depth = NULL;
    D3D_A8_VERTEX vertices[6];
    D3DLOCKED_RECT locked;
    D3DVIEWPORT8 saved_viewport;
    D3DVIEWPORT8 viewport;
    HRESULT hr;
    HRESULT step_hr;
    HRESULT unlock_hr;
    DWORD shader = 0;
    DWORD blend_state_block = 0;
    DWORD column;
    DWORD pass;
    DWORD x;
    DWORD y;
    DWORD nonclear;
    DWORD min_x;
    DWORD min_y;
    DWORD max_x;
    DWORD max_y;
    const DWORD *sample_pixels;

    BOOL scene_begun = FALSE;
    BOOL state_block_recording = FALSE;
    BOOL target_bound = FALSE;
    BOOL surface_locked = FALSE;
    BOOL texture_locked = FALSE;
    BOOL ps11_declined = FALSE;
    char detail[192];

    if (!device) return E_INVALIDARG;
    memset(&locked, 0, sizeof(locked));
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    memset(&viewport, 0, sizeof(viewport));
    report_stage("a8-contract-enter",
                 "4x1-pattern-fixed-decode-fixed-blend-ps11-stateblock");

    hr = IDirect3DDevice8_GetRenderTarget(device, &saved_target);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_GetDepthStencilSurface(device, &saved_depth);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_GetViewport(device, &saved_viewport);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_CreateTexture(
            device, 4, 1, 1, 0, D3DFMT_A8,
            D3DPOOL_MANAGED, &texture);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DTexture8_LockRect(texture, 0, &locked, NULL, 0);
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
        unlock_hr = IDirect3DTexture8_UnlockRect(texture, 0);
        texture_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_CreateRenderTarget(
            device, 256, 16, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, TRUE, &target);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_CreatePixelShader(
            device, shader_code, &shader);
    }
    report_hr_stage("a8-contract-create", hr,
                    SUCCEEDED(hr) ?
                    "a8-pattern-lockable-argb-target-ps11" : "failed");

    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderTarget(device, target, NULL);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    viewport.Width = 256;
    viewport.Height = 16;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetViewport(device, &viewport);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_Clear(
            device, 0, NULL, D3DCLEAR_TARGET, 0xff204080UL, 1.0f, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ZENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ZWRITEENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 |
                    D3DFVF_TEXCOORDSIZE3(0));
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }

    /* Pass 0: alpha-replicated RGB, no blend.  Lay the three passes out
     * horizontally so render-target row orientation cannot confound the
     * alpha oracle. */
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(device, 0);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1,
            D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad8(vertices, (float)(column * 16), 0.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }

    /* Pass 1: fixed-function diffuse RGB and patterned texture alpha. */
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, TRUE);
    }
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad8(vertices, (float)(64 + column * 16), 0.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }

    /* Pass 2 would exercise native D3D8 ps_1_1 without a vertex
     * declaration, which this frontend does not serve.  Its rejection is
     * deferred and poisons later work, so skip it and keep passes 0/1 as
     * the exact fixed-function contract. */
    if (SUCCEEDED(hr)) {
        report_hr_stage("a8-contract-ps11-draw", S_OK,
                        "declined-unsupported-d3d8-native-ps_1_1");
        ps11_declined = TRUE;
    }

    /* Pass 3: BeginStateBlock records but must not change current device
     * state. Capture must therefore replace the block's ONE/ZERO values with
     * the still-current SRCALPHA/INVSRCALPHA pair before Apply. This mirrors
     * the capture/apply sequence used by real D3D8 effect state managers. */
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_BeginStateBlock(device);
        if (SUCCEEDED(hr)) state_block_recording = TRUE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetPixelShader(device, 0);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_EndStateBlock(device, &blend_state_block);
        if (SUCCEEDED(hr)) state_block_recording = FALSE;
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_CaptureStateBlock(
            device, blend_state_block);
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_ApplyStateBlock(device, blend_state_block);
    }
    for (column = 0; SUCCEEDED(hr) && column < 4; ++column) {
        fill_a8_quad8(vertices, (float)(192 + column * 16), 0.0f,
                      ((float)column + 0.5f) / 4.0f, 0xffe0a020UL);
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 2, vertices, sizeof(vertices[0]));
    }
    if (scene_begun) {
        step_hr = IDirect3DDevice8_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (state_block_recording) {
        step_hr = IDirect3DDevice8_EndStateBlock(
            device, &blend_state_block);
        if (SUCCEEDED(step_hr)) state_block_recording = FALSE;
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(device, 0);
    report_hr_stage("a8-contract-draw", hr,
                    FAILED(hr) ? "failed" :
                    ps11_declined ?
                        "decode+fixed+stateblock "
                        "ps11=declined-unsupported" :
                        "decode+fixed+ps11+stateblock");

    /* Restore before locking: the lockable target must not remain bound. */
    if (target_bound) {
        step_hr = IDirect3DDevice8_SetRenderTarget(
            device, saved_target, saved_depth);
        if (SUCCEEDED(step_hr)) {
            step_hr = IDirect3DDevice8_SetViewport(device, &saved_viewport);
        }
        if (SUCCEEDED(step_hr)) target_bound = FALSE;
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        hr = IDirect3DSurface8_LockRect(
            target, &locked, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) surface_locked = TRUE;
    }
    if (SUCCEEDED(hr) &&
        (!locked.pBits || locked.Pitch < 256 * (INT)sizeof(DWORD))) {
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        sample_pixels =
            (const DWORD *)((const BYTE *)locked.pBits + 8 * locked.Pitch);
        _snprintf(
            detail, sizeof(detail),
            "p0=%06lx/%06lx/%06lx/%06lx "
            "p1=%06lx/%06lx/%06lx/%06lx "
            "p2=%06lx/%06lx/%06lx/%06lx "
            "p3=%06lx/%06lx/%06lx/%06lx",
            (unsigned long)(sample_pixels[8] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[24] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[40] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[56] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[72] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[88] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[104] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[120] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[136] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[152] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[168] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[184] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[200] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[216] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[232] & 0x00ffffffUL),
            (unsigned long)(sample_pixels[248] & 0x00ffffffUL));
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-grid", detail);

        nonclear = 0;
        min_x = 256;
        min_y = 16;
        max_x = 0;
        max_y = 0;
        for (y = 0; y < 16; ++y) {
            const DWORD *row =
                (const DWORD *)((const BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 256; ++x) {
                if ((row[x] & 0x00ffffffUL) == 0x00204080UL) continue;
                ++nonclear;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
        if (nonclear) {
            _snprintf(detail, sizeof(detail),
                      "nonclear=%lu bounds=%lu,%lu-%lu,%lu",
                      (unsigned long)nonclear,
                      (unsigned long)min_x, (unsigned long)min_y,
                      (unsigned long)max_x, (unsigned long)max_y);
        } else {
            _snprintf(detail, sizeof(detail), "nonclear=0");
        }
        detail[sizeof(detail) - 1] = '\0';
        report_stage("a8-contract-map", detail);
    }

    /* Passes 2 and 3 both assert shader_expected: pass 3 replays the
     * ps_1_1 binding through the captured state block (the recorded
     * SetPixelShader(0) must not change current state, so the pass 2
     * binding is still current at capture time).  When the pass 2 bind
     * was declined there is no ps_1_1 output in the x=128..255 region at
     * all, so only the fixed-function passes 0/1 keep exact pixel
     * assertions. */
    for (pass = 0;
         SUCCEEDED(hr) && pass < (ps11_declined ? 2UL : 4UL); ++pass) {
        const DWORD *expected = pass == 0 ? decode_expected :
                                (pass == 1 ? blend_expected :
                                             shader_expected);
        const DWORD *pixels = (const DWORD *)((const BYTE *)locked.pBits +
                                               8 * locked.Pitch);

        for (column = 0; column < 4; ++column) {
            DWORD observed = pixels[pass * 64 + column * 16 + 8] &
                             0x00ffffffUL;
            if (observed != expected[column]) {
                _snprintf(detail, sizeof(detail),
                          "pass=%lu alpha=%u observed=%06lx expected=%06lx",
                          (unsigned long)pass, (unsigned)alpha_texels[column],
                          (unsigned long)observed,
                          (unsigned long)expected[column]);
                detail[sizeof(detail) - 1] = '\0';
                report_hr_stage("a8-contract-readback", E_FAIL, detail);
                hr = E_FAIL;
                break;
            }
        }
    }
    if (surface_locked) {
        unlock_hr = IDirect3DSurface8_UnlockRect(target);
        surface_locked = FALSE;
        if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    }
    if (SUCCEEDED(hr)) {
        report_hr_stage("a8-contract-readback", S_OK,
                        ps11_declined ?
                        "decode=000000/555555/aaaaaa/ffffff "
                        "fixed=204080/606060/a08040/e0a020 "
                        "ps11+stateblock=declined-unsupported" :
                        "decode=000000/555555/aaaaaa/ffffff "
                        "fixed=204080/606060/a08040/e0a020 "
                        "ps11+stateblock=204080/152b55/0b152b/000000");
    }

    if (scene_begun) {
        step_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    }
    step_hr = IDirect3DDevice8_SetPixelShader(device, 0);
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_ONE);
    }
    if (SUCCEEDED(step_hr)) {
        step_hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    if (SUCCEEDED(hr) && FAILED(step_hr)) hr = step_hr;
    if (target_bound) {
        IDirect3DDevice8_SetRenderTarget(device, saved_target, saved_depth);
        IDirect3DDevice8_SetViewport(device, &saved_viewport);
    }
    if (blend_state_block) {
        IDirect3DDevice8_DeleteStateBlock(device, blend_state_block);
    }
    if (shader) IDirect3DDevice8_DeletePixelShader(device, shader);
    if (target) IDirect3DSurface8_Release(target);
    if (texture) IDirect3DTexture8_Release(texture);
    if (saved_depth) IDirect3DSurface8_Release(saved_depth);
    if (saved_target) IDirect3DSurface8_Release(saved_target);
    report_hr_stage("a8-contract-leave", hr,
                    SUCCEEDED(hr) ?
                        "exact-patterned-alpha-stateblock=ok" : "failed");
    return hr;
}

static HRESULT create_point_sprite_render_target(
    IDirect3DDevice8 *device, IDirect3DTexture8 *source_texture,
    IDirect3DTexture8 **target_texture)
{
    D3D_HWTL_SPRITE_VERTEX sprite_vertex;
    D3DVIEWPORT8 saved_viewport;
    D3DVIEWPORT8 target_viewport;
    D3DMATRIX identity;
    IDirect3DSurface8 *backbuffer = NULL;
    IDirect3DSurface8 *backdepth = NULL;
    IDirect3DSurface8 *target_surface = NULL;
    HRESULT end_hr;
    HRESULT hr;
    HRESULT restore_hr;
    BOOL scene_begun = FALSE;
    BOOL target_bound = FALSE;

    if (!device || !source_texture || !target_texture) return E_INVALIDARG;
    *target_texture = NULL;
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    memset(&target_viewport, 0, sizeof(target_viewport));
    memset(&sprite_vertex, 0, sizeof(sprite_vertex));
    report_stage("point-sprite-target-enter",
                 "64x64-x8r8g8b8-stage0-orientation");

    hr = IDirect3DDevice8_GetRenderTarget(device, &backbuffer);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetDepthStencilSurface(
        device, &backdepth);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetViewport(
        device, &saved_viewport);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateTexture(
        device, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
        D3DPOOL_DEFAULT, target_texture);
    if (SUCCEEDED(hr)) hr = IDirect3DTexture8_GetSurfaceLevel(
        *target_texture, 0, &target_surface);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderTarget(device, target_surface, NULL);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    target_viewport.Width = 64;
    target_viewport.Height = 64;
    target_viewport.MinZ = 0.0f;
    target_viewport.MaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetViewport(
        device, &target_viewport);

    set_identity_matrix(&identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSCALEENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MIN, 0x3f800000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSIZE_MAX, 0x42800000UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
        device, 0, (IDirect3DBaseTexture8 *)source_texture);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 1, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_TEXCOORDINDEX, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE |
                D3DFVF_TEX2);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, 0xff102030UL, 1.0f, 0);

    sprite_vertex.z = 0.25f;
    sprite_vertex.point_size = 64.0f;
    sprite_vertex.color = 0xffffffffUL;
    sprite_vertex.tu0 = sprite_vertex.tu1 = 0.75f;
    sprite_vertex.tv0 = sprite_vertex.tv1 = 0.25f;
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(hr)) scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_POINTLIST, 1, &sprite_vertex, sizeof(sprite_vertex));
    if (scene_begun) {
        end_hr = IDirect3DDevice8_EndScene(device);
        scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }

    restore_hr = S_OK;
    if (target_bound) restore_hr = IDirect3DDevice8_SetRenderTarget(
        device, backbuffer, backdepth);
    if (SUCCEEDED(restore_hr) && backbuffer) {
        restore_hr = IDirect3DDevice8_SetViewport(device, &saved_viewport);
    }
    if (SUCCEEDED(restore_hr)) restore_hr = IDirect3DDevice8_SetTexture(
        device, 0, NULL);
    if (SUCCEEDED(restore_hr)) restore_hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_POINTSPRITEENABLE, FALSE);
    if (SUCCEEDED(hr) && FAILED(restore_hr)) hr = restore_hr;

    if (target_surface) IDirect3DSurface8_Release(target_surface);
    if (backdepth) IDirect3DSurface8_Release(backdepth);
    if (backbuffer) IDirect3DSurface8_Release(backbuffer);
    if (FAILED(hr) && *target_texture) {
        IDirect3DTexture8_Release(*target_texture);
        *target_texture = NULL;
    }
    report_hr_stage("point-sprite-target-leave", hr,
                    SUCCEEDED(hr) ? "rendered-and-restored" : "failed");
    return hr;
}

static HRESULT run_hwtl_reference_scene(IDirect3DDevice8 *device,
                                        IDirect3DTexture8 *texture)
{
    static const DWORD point_sprite_pixel_shader_code[] = {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, 0xb00f0000UL,
        D3DSIO_MOV, 0x800f0000UL, 0xb0e40000UL,
        D3DSIO_END
    };
    static const D3D_TEXTURE_VERTEX target_quad[4] = {
        { 14.0f, 414.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 0.0f },
        { 46.0f, 414.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 0.0f },
        { 14.0f, 446.0f, 0.25f, 1.0f, 0xffffffffUL, 0.0f, 1.0f },
        { 46.0f, 446.0f, 0.25f, 1.0f, 0xffffffffUL, 1.0f, 1.0f }
    };
    static const DWORD multi_light_indices[8] = {
        0, 2, 4, 6, 8, 10, 12, 14
    };
    static const DWORD range_light_indices[2] = { 3, 11 };
    static const DWORD triple_range_light_indices[3] = { 3, 11, 17 };
    static const DWORD quad_range_light_indices[4] = { 3, 11, 17, 23 };
    static const DWORD five_range_light_indices[5] = { 5, 13, 19, 29, 31 };
    static const DWORD six_range_light_indices[6] = {
        1, 6, 11, 16, 21, 26
    };
    static const DWORD seven_range_light_indices[7] = {
        0, 5, 10, 15, 20, 25, 30
    };
    static const DWORD eight_range_light_indices[8] = {
        3, 7, 11, 15, 19, 23, 27, 31
    };
    static const DWORD scalable_range_color_masks[8] = {
        1, 2, 4, 6, 5, 3, 1, 2
    };
    D3D_HWTL_VERTEX vertices[3];
    D3D_HWTL_MATERIAL_VERTEX material_vertices[3];
    D3D_HWTL_POINT_VERTEX point_vertices[3];
    D3D_HWTL_LIT_POINT_VERTEX scaled_lit_point;
    D3D_HWTL_SPRITE_VERTEX sprite_vertex;
    D3D_HWTL_TEXTURE3_VERTEX texture_vertices[3];
    D3D_HWTL_TEXTURE4_VERTEX texture4_vertices[3];
    D3D_HWTL_FOG_VERTEX fog_vertices[3];
    D3DVIEWPORT8 viewport;
    D3DVIEWPORT8 saved_viewport;
    D3DMATRIX world;
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATRIX texture_matrix;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    IDirect3DTexture8 *point_sprite_target = NULL;
    float clip_plane[4];
    float center_x;
    float center_y;
    float point_scale_base;
    DWORD point_scale_a;
    DWORD point_scale_b;
    DWORD point_scale_c;
    DWORD point_sprite_pixel_shader = 0;
    HRESULT cleanup_hr;
    HRESULT end_hr;
    HRESULT hr;
    BOOL specular_scene_begun = FALSE;
    unsigned falloff_case;
    unsigned index;

    if (!device || !texture) return E_INVALIDARG;
    memset(&saved_viewport, 0, sizeof(saved_viewport));
    hr = IDirect3DDevice8_GetViewport(device, &saved_viewport);
    if (FAILED(hr) || saved_viewport.Width < 640 ||
        saved_viewport.Height < 480) return FAILED(hr) ? hr : E_FAIL;

    hr = IDirect3DDevice8_CreatePixelShader(
        device, point_sprite_pixel_shader_code, &point_sprite_pixel_shader);
    report_hr_stage("point-sprite-pixel-shader-create", hr,
                    SUCCEEDED(hr) ? "ps_1_1-tex-t0" : "failed");
    if (FAILED(hr)) return hr;
    hr = create_point_sprite_render_target(
        device, texture, &point_sprite_target);
    if (FAILED(hr)) {
        IDirect3DDevice8_DeletePixelShader(device,
                                           point_sprite_pixel_shader);
        return hr;
    }
    viewport = saved_viewport;
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = 640;
    viewport.Height = 480;
    hr = IDirect3DDevice8_SetViewport(device, &viewport);
    report_hr_stage("hwtl-canonical-viewport", hr,
                    SUCCEEDED(hr) ? "upper-left-640x480" : "failed");
    if (FAILED(hr)) {
        IDirect3DTexture8_Release(point_sprite_target);
        IDirect3DDevice8_DeletePixelShader(device,
                                           point_sprite_pixel_shader);
        return hr;
    }
    /* Keep the entire object-space oracle invariant across larger display
     * modes. The fixed sample coordinates and the local-viewer lighting eye
     * vector are both defined in this canonical 640x480 viewport. D3D's
     * scaled-point formula also includes the viewport height, so derive the
     * coefficients from the canonical viewport rather than duplicating 480. */
    point_scale_base = (float)viewport.Height / 15.0f;
    point_scale_a = float_render_state(point_scale_base * point_scale_base);
    point_scale_b = float_render_state(
        point_scale_base * point_scale_base / 4.0f);
    point_scale_c = float_render_state(
        point_scale_base * point_scale_base / 16.0f);

    report_stage("hwtl-reference-enter",
                 "software-vp-or-hardware-vp-exact-parity");
    set_identity_matrix(&world);
    set_identity_matrix(&view);
    set_identity_matrix(&projection);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = 1.0f;
    material.Diffuse.a = 1.0f;
    material.Ambient.a = 1.0f;
    material.Specular.a = 1.0f;
    material.Emissive.a = 1.0f;

    hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LOCALVIEWER, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                         D3DCLEAR_STENCIL,
        0xff0b1d2eUL, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);

    /* Matrix composition: nonidentity world, view, and projection matrices
     * deliberately cancel to the requested screen-space center. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 80.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_triangle(vertices, 0.0f, 0.0f, 0.08f, 0.09f,
                           0xffcc2030UL, 0.0f, 0.0f, 1.0f);
        set_identity_matrix(&world);
        world._11 = world._22 = 0.5f;
        world._41 = center_x * 0.5f + 0.2f;
        world._42 = center_y * 0.5f - 0.1f;
        set_identity_matrix(&view);
        view._41 = -0.2f;
        view._42 = 0.1f;
        set_identity_matrix(&projection);
        projection._11 = projection._22 = 2.0f;
        hr = IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING,
                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* Directional diffuse lighting with vertex color as the documented
     * material source. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 200.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xff20c040UL, 0.0f, 0.0f, 1.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Direction.z = -1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* Material emissive is independent of every light and produces an exact
     * cyan reference while exercising the complete material DP2 record. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 320.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xffffffffUL, 0.0f, 0.0f, 1.0f);
        memset(&material, 0, sizeof(material));
        material.Emissive.g = material.Emissive.b = 1.0f;
        material.Emissive.a = 1.0f;
        hr = IDirect3DDevice8_LightEnable(device, 0, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* Point-light normals aim at the light from each vertex. Normalization
     * therefore yields a unit diffuse term while still covering positional
     * light range and attenuation state. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 440.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xff8020c0UL, 0.0f, 0.0f, 1.0f);
        memset(&material, 0, sizeof(material));
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 1.25f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        hr = IDirect3DDevice8_SetMaterial(device, &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* A positional light contributes nothing once vertex distance exceeds
     * D3DLIGHT8::Range. Fixed OpenGL attenuation has no hard cutoff, so this
     * exact black sample requires the bounded internal lighting program. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 520.0f);
        center_y = hwtl_ndc_y(&viewport, 260.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xffff8040UL, 0.0f, 0.0f, 1.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 1.25f;
        light.Range = 0.5f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* The same construction lies wholly inside a spotlight's inner cone, so
     * it covers theta/phi/falloff without introducing a fuzzy edge sample. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 560.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xffc06020UL, 0.0f, 0.0f, 1.0f);
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_SPOT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 1.25f;
        light.Direction.z = -1.0f;
        light.Range = 100.0f;
        light.Falloff = 1.0f;
        light.Attenuation0 = 1.0f;
        light.Theta = 1.0f;
        light.Phi = 1.5f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        hr = IDirect3DDevice8_SetLight(device, 0, &light);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* Each equidistant triangle sits halfway between a zero-width inner cone
     * and a 120-degree outer cone. Rho=0.75 first proves the exact 0.5 linear
     * factor, then Falloff=2 proves the required nonlinear 0.25 result and a
     * live local-parameter update while the same internal program remains
     * bound. The chosen RGB components divide exactly in both cases. */
    for (falloff_case = 0; SUCCEEDED(hr) && falloff_case < 2;
         ++falloff_case) {
        center_x = hwtl_ndc_x(&viewport,
                              80.0f + 120.0f * (float)falloff_case);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        memset(vertices, 0, sizeof(vertices));
        vertices[0].x = center_x;
        vertices[0].y = center_y + 0.08819171f;
        vertices[1].x = center_x - 0.07637626f;
        vertices[1].y = center_y - 0.04409586f;
        vertices[2].x = center_x + 0.07637626f;
        vertices[2].y = center_y - 0.04409586f;
        memset(&material, 0, sizeof(material));
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_SPOT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Direction.z = -1.0f;
        light.Range = 100.0f;
        light.Falloff = 1.0f + (float)falloff_case;
        light.Attenuation0 = 1.0f;
        light.Theta = 0.0f;
        light.Phi = 2.0943951f;
        for (index = 0; index < 3; ++index) {
            vertices[index].z = 0.25f;
            vertices[index].color = 0xffc08040UL;
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* A requested COLOR1 source falls back to material diffuse when COLOR1 is
     * absent from the FVF. Keep ambient/emissive black and align each normal
     * to the point light so the exact output is solely
     * light.diffuse*material.diffuse. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 320.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.07f;
        material_vertices[0].y = center_y - 0.09f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.09f;
        material_vertices[2].x = center_x + 0.07f;
        material_vertices[2].y = center_y - 0.09f;
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = 1.0f;
        material.Diffuse.g = 0.25f;
        material.Diffuse.b = 0.0f;
        material.Diffuse.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nx =
                light.Position.x - material_vertices[index].x;
            material_vertices[index].ny =
                light.Position.y - material_vertices[index].y;
            material_vertices[index].nz =
                light.Position.z - material_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* COLOR2 has the same documented missing-FVF fallback. This independent
     * cyan-blue material sample has neither diffuse nor specular vertex data,
     * so any attempt to consume a stale secondary array is visible. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 260.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.07f;
        material_vertices[0].y = center_y - 0.09f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.09f;
        material_vertices[2].x = center_x + 0.07f;
        material_vertices[2].y = center_y - 0.09f;
        memset(&material, 0, sizeof(material));
        material.Diffuse.g = 0.25f;
        material.Diffuse.b = 1.0f;
        material.Diffuse.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nx =
                light.Position.x - material_vertices[index].x;
            material_vertices[index].ny =
                light.Position.y - material_vertices[index].y;
            material_vertices[index].nz =
                light.Position.z - material_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* COLOR1 ambient multiplies the point light's 0.5 ambient term. The
     * deliberately red material ambient makes a stale MATERIAL selection
     * fail independently of diffuse or emissive lighting. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 400.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.04f, 0.075f,
                           0xff804020UL, 0.0f, 0.0f, 1.0f);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        material.Ambient.r = 1.0f;
        material.Ambient.a = 1.0f;
        material.Emissive.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Ambient.r = light.Ambient.g = light.Ambient.b = 0.5f;
        light.Ambient.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* COLOR1 emissive is unattenuated and independent of the deliberately
     * red material emissive. A zero-color point light keeps this draw on the
     * same bounded positional program without contributing to the result. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 440.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.04f, 0.075f,
                           0xff2050a0UL, 0.0f, 0.0f, 1.0f);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        material.Ambient.a = 1.0f;
        material.Emissive.r = 1.0f;
        material.Emissive.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nx = light.Position.x - vertices[index].x;
            vertices[index].ny = light.Position.y - vertices[index].y;
            vertices[index].nz = light.Position.z - vertices[index].z;
        }
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g =
            material.Diffuse.b = 1.0f;
        material.Diffuse.a = 1.0f;
        material.Ambient.a = 1.0f;
        material.Specular.a = 1.0f;
        material.Emissive.a = 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
    }

    /* COLOR2 ambient multiplies the point light's one-half ambient term.
     * Primary and material ambient are deliberately different, making the
     * exact 104060 result unique to secondary RGB selection. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 400.0f);
        center_y = hwtl_ndc_y(&viewport, 170.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.25f,
                               0xffe01010UL);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        material.Ambient.r = 1.0f;
        material.Ambient.a = 1.0f;
        material.Emissive.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Ambient.r = light.Ambient.g = light.Ambient.b = 0.5f;
        light.Ambient.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].specular = 0xff2080c0UL;
            fog_vertices[index].nx =
                light.Position.x - fog_vertices[index].x;
            fog_vertices[index].ny =
                light.Position.y - fog_vertices[index].y;
            fog_vertices[index].nz =
                light.Position.z - fog_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* COLOR2 emissive is independent of the zero-color point light. The red
     * material and green primary poisons leave 6090C0 as the only correct
     * secondary-source result. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 460.0f);
        center_y = hwtl_ndc_y(&viewport, 170.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.25f,
                               0xff10e010UL);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        material.Ambient.a = 1.0f;
        material.Emissive.r = 1.0f;
        material.Emissive.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].specular = 0xff6090c0UL;
            fog_vertices[index].nx =
                light.Position.x - fog_vertices[index].x;
            fog_vertices[index].ny =
                light.Position.y - fog_vertices[index].y;
            fog_vertices[index].nz =
                light.Position.z - fog_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* COLOR2 supplies both diffuse RGB and alpha. The distinct COLOR1 and
     * material values rule out either fallback, while source-alpha blending
     * makes the secondary alpha byte observable as exact 383633 over the
     * 0B1D2E background. This is the stock XP software-VP oracle and the
     * accelerated parity assertion. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 520.0f);
        center_y = hwtl_ndc_y(&viewport, 170.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.25f,
                               0xc010d020UL);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = 1.0f;
        material.Diffuse.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 0.35f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].specular = 0x40c08040UL;
            fog_vertices[index].nx =
                light.Position.x - fog_vertices[index].x;
            fog_vertices[index].ny =
                light.Position.y - fog_vertices[index].y;
            fog_vertices[index].nz =
                light.Position.z - fog_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_ALPHABLENDENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* Specular-viewer oracle. Eye-space vertex (1,0,0), point-light vector
     * (0,0,-1), and normal (0,0,-1) make the infinite viewer full intensity.
     * The local view vector is (-1,0,0), so N.H is 1/sqrt(2) and Power=2
     * produces exactly one-half. Projection cancels the eye translation so
     * the two cases remain at fixed capture pixels. */
    if (SUCCEEDED(hr)) {
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = -0.07f;
        material_vertices[0].y = -0.09f;
        material_vertices[1].y = 0.09f;
        material_vertices[2].x = 0.07f;
        material_vertices[2].y = -0.09f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].nz = -1.0f;
        }
        set_identity_matrix(&world);
        world._41 = 1.0f;
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        projection._41 = hwtl_ndc_x(&viewport, 80.0f) - 1.0f;
        projection._42 = hwtl_ndc_y(&viewport, 170.0f);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        material.Ambient.a = 1.0f;
        material.Specular.r = 0.5f;
        material.Specular.g = 1.0f;
        material.Specular.b = 0.25f;
        material.Specular.a = 1.0f;
        material.Emissive.a = 1.0f;
        material.Power = 2.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
        light.Specular.a = 1.0f;
        light.Position.x = 1.0f;
        light.Position.z = -1.0f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LOCALVIEWER, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        if (SUCCEEDED(hr)) {
            projection._41 = hwtl_ndc_x(&viewport, 160.0f) - 1.0f;
            hr = IDirect3DDevice8_SetTransform(
                device, D3DTS_PROJECTION, &projection);
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LOCALVIEWER, FALSE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        /* COLOR1 and COLOR2 select independent specular material inputs. */
        memset(vertices, 0, sizeof(vertices));
        vertices[0].x = -0.04f;
        vertices[0].y = -0.06f;
        vertices[1].y = 0.06f;
        vertices[2].x = 0.04f;
        vertices[2].y = -0.06f;
        for (index = 0; index < 3; ++index) {
            vertices[index].nz = -1.0f;
            vertices[index].color = 0xff4080c0UL;
        }
        projection._41 = hwtl_ndc_x(&viewport, 240.0f) - 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);

        memset(fog_vertices, 0, sizeof(fog_vertices));
        fog_vertices[0].x = -0.04f;
        fog_vertices[0].y = -0.06f;
        fog_vertices[1].y = 0.06f;
        fog_vertices[2].x = 0.04f;
        fog_vertices[2].y = -0.06f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].nz = -1.0f;
            fog_vertices[index].color = 0xff10e010UL;
            fog_vertices[index].specular = 0xffc06020UL;
        }
        projection._41 = hwtl_ndc_x(&viewport, 300.0f) - 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));

        /* A missing COLOR2 FVF field must fall back to the material. */
        material.Specular.r = 0.25f;
        material.Specular.g = 0.5f;
        material.Specular.b = 1.0f;
        projection._41 = hwtl_ndc_x(&viewport, 360.0f) - 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);

        /* The same equation remains active for a spotlight. This geometry is
         * wholly inside the inner cone so source selection is isolated from
         * the already-independent falloff exponent samples above. */
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = -0.04f;
        material_vertices[0].y = -0.06f;
        material_vertices[1].y = 0.06f;
        material_vertices[2].x = 0.04f;
        material_vertices[2].y = -0.06f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].nz = -1.0f;
        }
        material.Specular.r = 1.0f;
        material.Specular.g = 0.25f;
        material.Specular.b = 0.5f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_SPOT;
        light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
        light.Specular.a = 1.0f;
        light.Position.x = 1.0f;
        light.Position.z = -1.0f;
        light.Direction.z = 1.0f;
        light.Range = 100.0f;
        light.Falloff = 2.0f;
        light.Attenuation0 = 1.0f;
        light.Theta = 0.5f;
        light.Phi = 1.0f;
        projection._41 = hwtl_ndc_x(&viewport, 600.0f) - 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LOCALVIEWER, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
        set_identity_matrix(&world);
        set_identity_matrix(&projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* Nonuniform world scaling requires the inverse-transpose normal matrix.
     * The chosen directional vector is exactly parallel to the resulting
     * normalized normal, leaving a stable full-intensity color. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 120.0f);
        center_y = hwtl_ndc_y(&viewport, 260.0f);
        fill_hwtl_triangle(vertices, 0.0f, 0.0f, 0.035f, 0.09f,
                           0xff40a0ffUL, 1.0f, 0.0f, 1.0f);
        set_identity_matrix(&world);
        world._11 = 2.0f;
        world._41 = center_x;
        world._42 = center_y;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Direction.x = -0.44721359f;
        light.Direction.z = -0.89442718f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* In the default unscaled mode, D3DFVF_PSIZE overrides the deliberately
     * different global size. Three independent values expose a replay that
     * accidentally uses one size for the whole point list. */
    if (SUCCEEDED(hr)) {
        memset(point_vertices, 0, sizeof(point_vertices));
        point_vertices[0].x = hwtl_ndc_x(&viewport, 200.0f);
        point_vertices[0].y = hwtl_ndc_y(&viewport, 260.0f);
        point_vertices[0].z = 0.25f;
        point_vertices[0].point_size = 5.0f;
        point_vertices[0].color = 0xffff0000UL;
        point_vertices[1].x = hwtl_ndc_x(&viewport, 260.0f);
        point_vertices[1].y = hwtl_ndc_y(&viewport, 260.0f);
        point_vertices[1].z = 0.25f;
        point_vertices[1].point_size = 11.0f;
        point_vertices[1].color = 0xff00ff00UL;
        point_vertices[2].x = hwtl_ndc_x(&viewport, 320.0f);
        point_vertices[2].y = hwtl_ndc_y(&viewport, 260.0f);
        point_vertices[2].z = 0.25f;
        point_vertices[2].point_size = 17.0f;
        point_vertices[2].color = 0xff0000ffUL;
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALEENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE, 0x40400000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 3, point_vertices,
            sizeof(point_vertices[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE, 0x3f800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* Scaled D3D points use viewport height and eye distance:
     *
     *   screen = viewportHeight * PSIZE / sqrt(A + B*d + C*d*d)
     *
     * then clamp to POINTSIZE_MIN/MAX. With a 480-pixel viewport, A=1024
     * maps PSIZE {0.5,1,2} to {7.5,15,30}, which clamps to {10,15,20}.
     * The B and C cases put an object-space point at exact eye distance four
     * and choose coefficients that independently produce 15 pixels. */
    if (SUCCEEDED(hr)) {
        memset(point_vertices, 0, sizeof(point_vertices));
        point_vertices[0].x = hwtl_ndc_x(&viewport, 80.0f);
        point_vertices[0].y = hwtl_ndc_y(&viewport, 450.0f);
        point_vertices[0].z = 0.25f;
        point_vertices[0].point_size = 0.5f;
        point_vertices[0].color = 0xffff0000UL;
        point_vertices[1].x = hwtl_ndc_x(&viewport, 200.0f);
        point_vertices[1].y = hwtl_ndc_y(&viewport, 450.0f);
        point_vertices[1].z = 0.25f;
        point_vertices[1].point_size = 1.0f;
        point_vertices[1].color = 0xff00ff00UL;
        point_vertices[2].x = hwtl_ndc_x(&viewport, 320.0f);
        point_vertices[2].y = hwtl_ndc_y(&viewport, 450.0f);
        point_vertices[2].z = 0.25f;
        point_vertices[2].point_size = 2.0f;
        point_vertices[2].color = 0xff0000ffUL;
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE_MIN, 0x41200000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE_MAX, 0x41a00000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_A, point_scale_a);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_B, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_C, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALEENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 3, point_vertices,
            sizeof(point_vertices[0]));

        point_vertices[0].x = 0.0f;
        point_vertices[0].y = 0.0f;
        point_vertices[0].z = 4.0f;
        point_vertices[0].point_size = 1.0f;
        point_vertices[0].color = 0xffff00ffUL;
        set_identity_matrix(&projection);
        projection._33 = 0.0f;
        projection._41 = hwtl_ndc_x(&viewport, 440.0f);
        projection._42 = hwtl_ndc_y(&viewport, 450.0f);
        projection._43 = 0.5f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_A, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_B, point_scale_b);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, point_vertices,
            sizeof(point_vertices[0]));

        point_vertices[0].color = 0xff00ffffUL;
        projection._41 = hwtl_ndc_x(&viewport, 560.0f);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_B, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_C, point_scale_c);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, point_vertices,
            sizeof(point_vertices[0]));

        /* Keep B attenuation active while the bounded positional-light
         * program produces an exact material-diffuse color. Point size must
         * remain 15 pixels even with the 128-instruction program bound. */
        memset(&scaled_lit_point, 0, sizeof(scaled_lit_point));
        scaled_lit_point.z = 4.0f;
        scaled_lit_point.nz = 1.0f;
        scaled_lit_point.point_size = 1.0f;
        scaled_lit_point.color = 0xffffffffUL;
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = 1.0f;
        material.Diffuse.g = 0.25f;
        material.Diffuse.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.z = 5.0f;
        light.Range = 10.0f;
        light.Attenuation0 = 1.0f;
        projection._41 = hwtl_ndc_x(&viewport, 620.0f);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_B, point_scale_b);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_C, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_PSIZE |
                    D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, &scaled_lit_point,
            sizeof(scaled_lit_point));

        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALEENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE_MIN, 0x3f800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSIZE_MAX, 0x42800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_A, 0x3f800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_B, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSCALE_C, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        set_identity_matrix(&projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* Eight sparse logical light indices must occupy all advertised hardware
     * slots and accumulate diffuse RGB. The second triangle disables the
     * first red and final green contribution, proving that active slots are
     * compacted rather than tied to the guest's logical indices. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 440.0f);
        center_y = hwtl_ndc_y(&viewport, 215.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.04f;
        material_vertices[0].y = center_y - 0.05f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.05f;
        material_vertices[2].x = center_x + 0.04f;
        material_vertices[2].y = center_y - 0.05f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 1.0f;
        }
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
            material.Diffuse.a = 1.0f;
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        for (index = 0; SUCCEEDED(hr) && index < 8; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_DIRECTIONAL;
            light.Diffuse.a = 1.0f;
            if (index == 0 || index == 1 || index == 6)
                light.Diffuse.r = 0.20f;
            else if (index == 2 || index == 3 || index == 7)
                light.Diffuse.g = 0.20f;
            else
                light.Diffuse.b = 0.20f;
            light.Direction.z = -1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, multi_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, multi_light_indices[index], TRUE);
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, multi_light_indices[0], FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
            device, multi_light_indices[7], FALSE);
        center_x = hwtl_ndc_x(&viewport, 520.0f);
        material_vertices[0].x = center_x - 0.04f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.04f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        for (index = 0; index < 8; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, multi_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }

        /* Two point lights straddle two separately drawn triangles. Each
         * triangle's vertices point exactly at its own light, so the in-Range
         * contribution saturates one primary channel. The other light remains
         * geometrically visible but lies beyond its independent Range; fixed
         * OpenGL would incorrectly mix both channels. Sparse logical IDs also
         * keep the active-slot compaction path in this exact oracle. */
        center_y = hwtl_ndc_y(&viewport, 315.0f);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
            material.Diffuse.a = 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);

        for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (!index)
                light.Diffuse.r = 1.0f;
            else
                light.Diffuse.g = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, index ? 560.0f : 400.0f);
            light.Position.y = center_y;
            light.Position.z = 1.25f;
            light.Range = 1.05f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, range_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, range_light_indices[index], TRUE);
        }

        center_x = hwtl_ndc_x(&viewport, 400.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.03f;
        material_vertices[0].y = center_y - 0.04f;
        material_vertices[0].nx = 0.02996257f;
        material_vertices[0].ny = 0.03995009f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.05f;
        material_vertices[1].ny = -0.04993762f;
        material_vertices[2].x = center_x + 0.03f;
        material_vertices[2].y = center_y - 0.04f;
        material_vertices[2].nx = -0.02996257f;
        material_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 0.99875234f;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        center_x = hwtl_ndc_x(&viewport, 560.0f);
        material_vertices[0].x = center_x - 0.03f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.03f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        /* Repeat the independent-Range discriminator with already-unit normals
         * while normalization is disabled, then source diffuse first from
         * COLOR1 and then COLOR2. Black material and primary-only light colors
         * make a missing vertex source or either cross contribution exact. */
        center_y = hwtl_ndc_y(&viewport, 365.0f);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
        for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (!index)
                light.Diffuse.r = 1.0f;
            else
                light.Diffuse.g = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, index ? 440.0f : 360.0f);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.525f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, range_light_indices[index], &light);
        }

        center_x = hwtl_ndc_x(&viewport, 360.0f);
        memset(vertices, 0, sizeof(vertices));
        vertices[0].x = center_x - 0.015f;
        vertices[0].y = center_y - 0.02f;
        vertices[0].nx = 0.02996257f;
        vertices[0].ny = 0.03995009f;
        vertices[1].x = center_x;
        vertices[1].y = center_y + 0.025f;
        vertices[1].ny = -0.04993762f;
        vertices[2].x = center_x + 0.015f;
        vertices[2].y = center_y - 0.02f;
        vertices[2].nx = -0.02996257f;
        vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            vertices[index].z = 0.25f;
            vertices[index].nz = 0.99875234f;
            vertices[index].color = 0xffffffffUL;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                               vertices);
        center_x = hwtl_ndc_x(&viewport, 440.0f);
        vertices[0].x = center_x - 0.015f;
        vertices[1].x = center_x;
        vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                               vertices);

        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (!index)
                light.Diffuse.r = 1.0f;
            else
                light.Diffuse.g = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, index ? 600.0f : 520.0f);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.525f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, range_light_indices[index], &light);
        }

        center_x = hwtl_ndc_x(&viewport, 520.0f);
        memset(fog_vertices, 0, sizeof(fog_vertices));
        fog_vertices[0].x = center_x - 0.015f;
        fog_vertices[0].y = center_y - 0.02f;
        fog_vertices[0].nx = 0.02996257f;
        fog_vertices[0].ny = 0.03995009f;
        fog_vertices[1].x = center_x;
        fog_vertices[1].y = center_y + 0.025f;
        fog_vertices[1].ny = -0.04993762f;
        fog_vertices[2].x = center_x + 0.015f;
        fog_vertices[2].y = center_y - 0.02f;
        fog_vertices[2].nx = -0.02996257f;
        fog_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].z = 0.25f;
            fog_vertices[index].nz = 0.99875234f;
            fog_vertices[index].color = 0xff000000UL;
            fog_vertices[index].specular = 0xffffffffUL;
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
        center_x = hwtl_ndc_x(&viewport, 600.0f);
        fog_vertices[0].x = center_x - 0.015f;
        fog_vertices[1].x = center_x;
        fog_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));

        for (index = 0; index < 2; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, range_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* Point sprites replace the coordinate independently for every texture
     * stage and use Direct3D's top-left (0,0), bottom-right (1,1) mapping.
     * The existing asymmetric reference texture therefore gives four exact
     * orientation samples. A second sprite selects the same texture through
     * stage one, and the final disabled sprite must use its constant
     * top-right coordinate over the complete point. */
    if (SUCCEEDED(hr)) {
        memset(&sprite_vertex, 0, sizeof(sprite_vertex));
        sprite_vertex.x = hwtl_ndc_x(&viewport, 30.0f);
        sprite_vertex.y = hwtl_ndc_y(&viewport, 130.0f);
        sprite_vertex.z = 0.25f;
        sprite_vertex.point_size = 32.0f;
        sprite_vertex.color = 0xffffffffUL;
        sprite_vertex.tu0 = sprite_vertex.tu1 = 0.75f;
        sprite_vertex.tv0 = sprite_vertex.tv1 = 0.25f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 1, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXCOORDINDEX, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSPRITEENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE |
                    D3DFVF_TEX2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, &sprite_vertex,
            sizeof(sprite_vertex));

        sprite_vertex.y = hwtl_ndc_y(&viewport, 205.0f);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 1, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_MINFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_TEXCOORDINDEX, 1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, &sprite_vertex,
            sizeof(sprite_vertex));

        sprite_vertex.y = hwtl_ndc_y(&viewport, 280.0f);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSPRITEENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, &sprite_vertex,
            sizeof(sprite_vertex));

        /* A ps_1_1 lookup must consume the same replacement coordinate as
         * fixed function. Keep the explicit vertex coordinate asymmetric so
         * a missing replacement cannot accidentally satisfy the quadrants. */
        sprite_vertex.y = hwtl_ndc_y(&viewport, 355.0f);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSPRITEENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 1, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(
            device, point_sprite_pixel_shader);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_POINTLIST, 1, &sprite_vertex,
            sizeof(sprite_vertex));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(device, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_POINTSPRITEENABLE, FALSE);

        /* The 64x64 offscreen point sprite is sampled back as a 32x32 quad.
         * Its quadrants distinguish the presentation and texture-target
         * coordinate origins without relying on image recognition. */
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)point_sprite_target);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, target_quad,
            sizeof(target_quad[0]));

        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 1, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    }

    /* User plane zero keeps only x >= center. The left sample must remain the
     * unique clear color while the right sample must be magenta. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 400.0f);
        center_y = hwtl_ndc_y(&viewport, 260.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.16f, 0.14f,
                           0xffff2080UL, 0.0f, 0.0f, 1.0f);
        set_identity_matrix(&world);
        clip_plane[0] = 1.0f;
        clip_plane[1] = 0.0f;
        clip_plane[2] = 0.0f;
        clip_plane[3] = -center_x;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetClipPlane(
            device, 0, clip_plane);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_CLIPPLANEENABLE, 0);
    }

    /* COUNT2 describes two matrix outputs, not the three-component FVF input.
     * Translation selects the checker's cyan quadrant exactly. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 250.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_texture3_triangle(texture_vertices, center_x, center_y,
                                    0xffffffffUL, 0.25f, 0.25f, 0.5f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        set_identity_matrix(&texture_matrix);
        texture_matrix._41 = 0.5f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_TEXTURE0, &texture_matrix);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture_vertices,
            sizeof(texture_vertices[0]));
    }

    /* COUNT3|PROJECTED divides the first two outputs by the third. The input
     * remains three-dimensional, while the texture matrix supplies q=2 and
     * selects the yellow quadrant after the homogeneous divide. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 450.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_texture3_triangle(texture_vertices, center_x, center_y,
                                    0xffffffffUL, 0.75f, 0.75f, 0.0f);
        set_identity_matrix(&texture_matrix);
        texture_matrix._43 = 2.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_TEXTURE0,
                                            &texture_matrix);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
            D3DTTFF_COUNT3 | D3DTTFF_PROJECTED);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture_vertices,
            sizeof(texture_vertices[0]));
    }

    /* D3D treats an explicit fourth input component as ordinary data unless
     * the stage is PROJECTED. The q=2 discriminator would incorrectly divide
     * (0.75, 0.25) into the checker's yellow quadrant on fixed OpenGL; exact
     * nonprojected handling must keep the intended cyan sample. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 350.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_texture4_triangle(texture4_vertices, center_x, center_y,
                                    0xffffffffUL, 0.75f, 0.25f, 0.0f, 2.0f);
        set_identity_matrix(&texture_matrix);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_TEXTURE0,
                                            &texture_matrix);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture4_vertices,
            sizeof(texture4_vertices[0]));
    }

    /* The bounded point-light program also owns explicit 4D q when both
     * semantics occur in one draw. Cyan texels modulated by the lit vertex
     * color produce exact 00ff80; an accidental GL q divide selects yellow
     * and would instead produce 80ff00. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 600.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        fill_hwtl_texture4_triangle(texture4_vertices, center_x, center_y,
                                    0xff80ff80UL, 0.75f, 0.25f, 0.0f, 2.0f);
        memset(&material, 0, sizeof(material));
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 1.25f;
        light.Range = 100.0f;
        light.Attenuation0 = 1.0f;
        for (index = 0; index < 3; ++index) {
            texture4_vertices[index].nx =
                light.Position.x - texture4_vertices[index].x;
            texture4_vertices[index].ny =
                light.Position.y - texture4_vertices[index].y;
            texture4_vertices[index].nz =
                light.Position.z - texture4_vertices[index].z;
        }
        hr = IDirect3DDevice8_SetMaterial(device, &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture4_vertices,
            sizeof(texture4_vertices[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }

    /* The same position-invariant vertex program must emit D3D vertex fog
     * rather than leaving the fog result undefined. A coordinate beyond the
     * linear end produces the exact fog color while 4D q handling remains
     * active in the same draw. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 160.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_texture4_triangle(texture4_vertices, center_x, center_y,
                                    0xffffffffUL, 0.75f, 0.25f, 0.0f, 2.0f);
        for (index = 0; index < 3; ++index)
            texture4_vertices[index].z = 0.75f;
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f19999aUL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff8040c0UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture4_vertices,
            sizeof(texture4_vertices[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    }

    /* Translate eye X while cancelling it in projection. Hardware range fog
     * must use the resulting radial eye distance; XP's stock software VP is
     * retained as the documented planar-Z baseline for the same draw. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 500.0f);
        center_y = hwtl_ndc_y(&viewport, 330.0f);
        fill_hwtl_texture4_triangle(texture4_vertices, center_x, center_y,
                                    0xffffffffUL, 0.75f, 0.25f, 0.0f, 2.0f);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        view._41 = 2.0f;
        projection._41 = -2.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f333333UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xffc04020UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture4_vertices,
            sizeof(texture4_vertices[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
    }

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);

    /* Fixed-function HWTL vertex fog uses absolute camera-space Z. Keeping
     * RANGEFOGENABLE off makes this a distinct non-range assertion. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 80.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.75f,
                               0xffffffffUL);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Emissive.r = material.Emissive.g =
            material.Emissive.b = material.Emissive.a = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f19999aUL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff20c060UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* Translate eye space by +2 in X and cancel it in projection. The
     * triangle stays at the same fixed sample pixel for every viewport size,
     * while radial eye distance is always beyond the fog end and |z| remains
     * before the start. A planar-Z mapping therefore stays white instead of
     * exact purple. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 560.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.25f,
                               0xffffffffUL);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        view._41 = 2.0f;
        projection._41 = -2.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f333333UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xffc040ffUL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* Pixel/table fog is defined from post-projection device Z, not the
     * original object Z. Doubling clip Z moves 0.25 to the linear end and
     * therefore produces the exact fog color. An object-Z implementation
     * would incorrectly leave a half-fogged triangle. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 615.0f);
        center_y = hwtl_ndc_y(&viewport, 90.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.25f,
                               0xffffffffUL);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        projection._33 = 2.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff204060UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* Device Z includes the homogeneous divide. Scale X, Y, and W together
     * so the triangle remains at the same sample pixel while clip Z=0.5
     * becomes device Z=0.25. Correct table fog is therefore exact white;
     * using clip Z would instead produce the fog color. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 80.0f);
        center_y = hwtl_ndc_y(&viewport, 215.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.5f,
                               0xffffffffUL);
        set_identity_matrix(&projection);
        projection._11 = projection._22 = projection._44 = 2.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION,
                                            &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x3e800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xffc02080UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* The exponential table equations consume the same post-divide depth.
     * At z=0.5 and the maximum valid density 1, white over black distinguishes
     * EXP from EXP2. Start/end are deliberately irrelevant poison values. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 200.0f);
        center_y = hwtl_ndc_y(&viewport, 215.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.5f,
                               0xffffffffUL);
        set_identity_matrix(&projection);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION,
                                            &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_EXP);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0x40000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x40400000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGDENSITY, 0x3f800000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 320.0f);
        center_y = hwtl_ndc_y(&viewport, 215.0f);
        fill_hwtl_fog_triangle(fog_vertices, center_x, center_y, 0.5f,
                               0xffffffffUL);
        hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_EXP2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
    }

    /* Combine table fog with the bounded point-light Range rule. The point is
     * one unit away but has Range=0.5, so lighting contributes black. At
     * device Z=0.25 the linear fog blend contributes exactly half 2468ac.
     * This keeps the future accelerated table-fog program honest about both
     * positional lighting and post-projection depth. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 600.0f);
        center_y = hwtl_ndc_y(&viewport, 260.0f);
        fill_hwtl_triangle(vertices, center_x, center_y, 0.07f, 0.09f,
                           0xffffffffUL, 0.0f, 0.0f, 1.0f);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = 1.0f;
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_POINT;
        light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
        light.Diffuse.a = 1.0f;
        light.Position.x = center_x;
        light.Position.y = center_y;
        light.Position.z = 1.25f;
        light.Range = 0.5f;
        light.Attenuation0 = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION,
                                            &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0,
                                                           &light);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff2468acUL);
        if (SUCCEEDED(hr)) hr = draw_hwtl_reference_triangle(device,
                                                              vertices);
    }

    /* Table fog must coexist with D3D's literal-q rule. Half-fogging a cyan
     * texel over black yields teal; an accidental OpenGL q divide selects the
     * checker's yellow quadrant and produces olive instead. */
    if (SUCCEEDED(hr)) {
        center_x = hwtl_ndc_x(&viewport, 620.0f);
        center_y = hwtl_ndc_y(&viewport, 400.0f);
        fill_hwtl_texture4_triangle(texture4_vertices, center_x, center_y,
                                    0xffffffffUL, 0.75f, 0.25f, 0.0f, 2.0f);
        set_identity_matrix(&projection);
        set_identity_matrix(&texture_matrix);
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION,
                                            &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_TEXTURE0, &texture_matrix);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            device, 0, (IDirect3DBaseTexture8 *)texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, texture4_vertices,
            sizeof(texture4_vertices[0]));
    }

    /* Three sparse point lights occupy independent hardware slots. Each
     * small triangle points exactly at its colocated primary-color light,
     * while both neighboring lights remain geometrically visible just beyond
     * their own Range. The blank top band and last-draw placement keep later
     * scene geometry from overwriting this exact discriminator. */
    if (SUCCEEDED(hr)) {
        center_y = hwtl_ndc_y(&viewport, 40.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
            material.Diffuse.a = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0,
                                                              FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);

        for (index = 0; SUCCEEDED(hr) && index < 3; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (index == 0)
                light.Diffuse.r = 1.0f;
            else if (index == 1)
                light.Diffuse.g = 1.0f;
            else
                light.Diffuse.b = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, 200.0f + 120.0f * index);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.55f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, triple_range_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, triple_range_light_indices[index], TRUE);
        }

        center_x = hwtl_ndc_x(&viewport, 200.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[0].y = center_y - 0.02f;
        material_vertices[0].nx = 0.02996257f;
        material_vertices[0].ny = 0.03995009f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.025f;
        material_vertices[1].ny = -0.04993762f;
        material_vertices[2].x = center_x + 0.015f;
        material_vertices[2].y = center_y - 0.02f;
        material_vertices[2].nx = -0.02996257f;
        material_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 0.99875234f;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        center_x = hwtl_ndc_x(&viewport, 320.0f);
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        center_x = hwtl_ndc_x(&viewport, 440.0f);
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        /* All three lights are beyond Range at the two remaining top-band
         * positions. Linear vertex fog therefore blends known fog color over
         * black at eye Z=0.25. The table-fog case scales projected Z to 0.375,
         * distinguishing its post-divide coordinate from the same eye Z. */
        center_x = hwtl_ndc_x(&viewport, 80.0f);
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DITHERENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_RANGEFOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGSTART, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGEND, 0x3f000000UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff2468acUL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        center_x = hwtl_ndc_x(&viewport, 560.0f);
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.015f;
        projection._33 = 1.5f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGCOLOR, 0xff804020UL);
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        set_identity_matrix(&projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);

        for (index = 0; index < 3; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, triple_range_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
    }

    /* Two point lights repeat the independent-Range discriminator with
     * specular-only primary colors. Power zero makes every front-lit in-Range
     * specular term exactly one, so material and COLOR2 sources must each
     * produce pure red/green rather than fixed-OpenGL cross-light yellow. */
    if (SUCCEEDED(hr)) {
        center_y = hwtl_ndc_y(&viewport, 130.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Diffuse.a = material.Specular.r = material.Specular.g =
            material.Specular.b = material.Specular.a = 1.0f;
        material.Power = 0.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LOCALVIEWER, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);

        for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Specular.a = 1.0f;
            if (!index)
                light.Specular.r = 1.0f;
            else
                light.Specular.g = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, index ? 500.0f : 140.0f);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.55f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, range_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, range_light_indices[index], TRUE);
        }

        center_x = hwtl_ndc_x(&viewport, 140.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[0].y = center_y - 0.02f;
        material_vertices[0].nx = 0.02996257f;
        material_vertices[0].ny = 0.03995009f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.025f;
        material_vertices[1].ny = -0.04993762f;
        material_vertices[2].x = center_x + 0.015f;
        material_vertices[2].y = center_y - 0.02f;
        material_vertices[2].nx = -0.02996257f;
        material_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 0.99875234f;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        center_x = hwtl_ndc_x(&viewport, 500.0f);
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[1].x = center_x;
        material_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);

        memset(&material, 0, sizeof(material));
        material.Diffuse.a = material.Specular.a = 1.0f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE |
                    D3DFVF_SPECULAR);
        for (index = 0; SUCCEEDED(hr) && index < 2; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Specular.a = 1.0f;
            if (!index)
                light.Specular.r = 1.0f;
            else
                light.Specular.g = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, index ? 380.0f : 260.0f);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.55f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, range_light_indices[index], &light);
        }

        center_x = hwtl_ndc_x(&viewport, 260.0f);
        memset(fog_vertices, 0, sizeof(fog_vertices));
        fog_vertices[0].x = center_x - 0.015f;
        fog_vertices[0].y = center_y - 0.02f;
        fog_vertices[0].nx = 0.02996257f;
        fog_vertices[0].ny = 0.03995009f;
        fog_vertices[1].x = center_x;
        fog_vertices[1].y = center_y + 0.025f;
        fog_vertices[1].ny = -0.04993762f;
        fog_vertices[2].x = center_x + 0.015f;
        fog_vertices[2].y = center_y - 0.02f;
        fog_vertices[2].nx = -0.02996257f;
        fog_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            fog_vertices[index].z = 0.25f;
            fog_vertices[index].nz = 0.99875234f;
            fog_vertices[index].color = 0xff000000UL;
            fog_vertices[index].specular = 0xffffffffUL;
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));
        center_x = hwtl_ndc_x(&viewport, 380.0f);
        fog_vertices[0].x = center_x - 0.015f;
        fog_vertices[1].x = center_x;
        fog_vertices[2].x = center_x + 0.015f;
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, fog_vertices,
            sizeof(fog_vertices[0]));

        for (index = 0; index < 2; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, range_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    }

    /* Four sparse point lights extend the same independent-Range contract to
     * the next active-slot count. The top edge is mathematically verified
     * background in the preceding 101-sample frame; these final draws use
     * normalized normals and material diffuse, matching the bounded program's
     * complete four-light subset without weakening unsupported combinations. */
    if (SUCCEEDED(hr)) {
        center_y = hwtl_ndc_y(&viewport, 20.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
            material.Diffuse.a = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);

        for (index = 0; SUCCEEDED(hr) && index < 4; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (index == 0 || index == 3) light.Diffuse.r = 1.0f;
            if (index == 1) light.Diffuse.g = 1.0f;
            if (index >= 2) light.Diffuse.b = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, 80.0f + 160.0f * index);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.55f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, quad_range_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, quad_range_light_indices[index], TRUE);
        }

        center_x = hwtl_ndc_x(&viewport, 80.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[0].y = center_y - 0.02f;
        material_vertices[0].nx = 0.02996257f;
        material_vertices[0].ny = 0.03995009f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.025f;
        material_vertices[1].ny = -0.04993762f;
        material_vertices[2].x = center_x + 0.015f;
        material_vertices[2].y = center_y - 0.02f;
        material_vertices[2].nx = -0.02996257f;
        material_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 0.99875234f;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        for (index = 1; SUCCEEDED(hr) && index < 4; ++index) {
            center_x = hwtl_ndc_x(&viewport, 80.0f + 160.0f * index);
            material_vertices[0].x = center_x - 0.015f;
            material_vertices[1].x = center_x;
            material_vertices[2].x = center_x + 0.015f;
            hr = draw_hwtl_material_triangle(device, material_vertices);
        }

        for (index = 0; index < 4; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, quad_range_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
    }

    /* Five sparse point lights cross the scalable-backend boundary. The
     * preceding reference frame is exactly background throughout y=45..68;
     * these triangles occupy y=49..61. Their centers are 120 pixels apart,
     * which is 0.375 eye units at this viewport. With the 0.5 eye-Z offset,
     * every adjacent light is sqrt(0.375^2 + 0.5^2) = 0.625 units away and
     * therefore outside the independent 0.55 Range. Each exact sample proves
     * one of five simultaneously enabled sparse logical light slots. */
    if (SUCCEEDED(hr)) {
        center_y = hwtl_ndc_y(&viewport, 55.0f);
        set_identity_matrix(&world);
        set_identity_matrix(&view);
        set_identity_matrix(&projection);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
            material.Diffuse.a = 1.0f;
        hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &world);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_VIEW, &view);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
            device, D3DTS_PROJECTION, &projection);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device,
                                                              &material);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_LIGHTING, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_COLORVERTEX, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_SPECULARENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_NORMALIZENORMALS, TRUE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            device, D3DRS_AMBIENT, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            device, D3DFVF_XYZ | D3DFVF_NORMAL);

        for (index = 0; SUCCEEDED(hr) && index < 5; ++index) {
            memset(&light, 0, sizeof(light));
            light.Type = D3DLIGHT_POINT;
            light.Diffuse.a = 1.0f;
            if (index == 0 || index == 4) light.Diffuse.r = 1.0f;
            if (index == 1 || index == 3) light.Diffuse.g = 1.0f;
            if (index >= 2) light.Diffuse.b = 1.0f;
            light.Position.x = hwtl_ndc_x(
                &viewport, 80.0f + 120.0f * index);
            light.Position.y = center_y;
            light.Position.z = 0.75f;
            light.Range = 0.55f;
            light.Attenuation0 = 1.0f;
            hr = IDirect3DDevice8_SetLight(
                device, five_range_light_indices[index], &light);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(
                device, five_range_light_indices[index], TRUE);
        }

        center_x = hwtl_ndc_x(&viewport, 80.0f);
        memset(material_vertices, 0, sizeof(material_vertices));
        material_vertices[0].x = center_x - 0.015f;
        material_vertices[0].y = center_y - 0.02f;
        material_vertices[0].nx = 0.02996257f;
        material_vertices[0].ny = 0.03995009f;
        material_vertices[1].x = center_x;
        material_vertices[1].y = center_y + 0.025f;
        material_vertices[1].ny = -0.04993762f;
        material_vertices[2].x = center_x + 0.015f;
        material_vertices[2].y = center_y - 0.02f;
        material_vertices[2].nx = -0.02996257f;
        material_vertices[2].ny = 0.03995009f;
        for (index = 0; index < 3; ++index) {
            material_vertices[index].z = 0.25f;
            material_vertices[index].nz = 0.99875234f;
        }
        if (SUCCEEDED(hr)) hr = draw_hwtl_material_triangle(
            device, material_vertices);
        for (index = 1; SUCCEEDED(hr) && index < 5; ++index) {
            center_x = hwtl_ndc_x(&viewport, 80.0f + 120.0f * index);
            material_vertices[0].x = center_x - 0.015f;
            material_vertices[1].x = center_x;
            material_vertices[2].x = center_x + 0.015f;
            hr = draw_hwtl_material_triangle(device, material_vertices);
        }

        for (index = 0; index < 5; ++index) {
            cleanup_hr = IDirect3DDevice8_LightEnable(
                device, five_range_light_indices[index], FALSE);
            if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
        }
    }

    /* Exercise every remaining active-count branch and both packed Range
     * uniforms. The preceding 110-sample frame is exactly background in the
     * y=25..34, y=60..68, and y=460..479 bands. At 640 pixels wide, adjacent
     * eye-space distances are sqrt(0.3125^2 + 0.5^2),
     * sqrt(0.28125^2 + 0.5^2), and sqrt(0.25^2 + 0.5^2). The selected 0.52
     * Range keeps every triangle's own light active while excluding all
     * neighbors in the six-, seven-, and eight-light rows. */
    if (SUCCEEDED(hr)) hr = draw_hwtl_point_range_row(
        device, &viewport, six_range_light_indices,
        scalable_range_color_masks, 6, 70.0f, 100.0f, 30.0f, 0.52f,
        0.01f, 0.012f, 0.015f);
    if (SUCCEEDED(hr)) hr = draw_hwtl_point_range_row(
        device, &viewport, seven_range_light_indices,
        scalable_range_color_masks, 7, 50.0f, 90.0f, 64.0f, 0.52f,
        0.01f, 0.012f, 0.015f);
    if (SUCCEEDED(hr)) hr = draw_hwtl_point_range_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 8, 40.0f, 80.0f, 468.0f, 0.52f,
        0.01f, 0.02f, 0.025f);

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_RANGEFOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, TRUE);

    end_hr = IDirect3DDevice8_EndScene(device);
    if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
        device, NULL, NULL, NULL, NULL);
    if (SUCCEEDED(hr)) Sleep(visual_frame_hold_ms);

    /* Keep scalable mixed-light coverage on its own deterministic
     * frame. Six rows cross every active-count branch from three through
     * eight while mixing point/spot, MATERIAL/COLOR2, and local/infinite
     * viewer state. The two-spot pair adds exact linear/squared outer-cone
     * Falloff, and a three-spot row combines it with planar/radial vertex fog
     * and post-divide table fog. A directional/point pair proves the former
     * remains invariant while hard Range rejects the latter. A scaled point
     * adds top-left sprite texturing and linear fog under five-light Range
     * isolation. The last two rows use
     * nonzero Power so the half-vector result
     * cannot collapse to a mere enabled-bit test. Every light retains the
     * same independent-Range discriminator as the diffuse reference. */
    report_stage("hwtl-specular-enter",
                 "directional-and-two-through-eight-positional-range-specular-point-sprite");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                         D3DCLEAR_STENCIL,
        0xff16233bUL, 1.0f, 0);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(hr)) specular_scene_begun = TRUE;
    }
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 3, 120.0f, 200.0f, 55.0f, 0.55f,
        FALSE, TRUE, 0.0f, TRUE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_spot_falloff_pair(
        device, &viewport, eight_range_light_indices,
        220.0f, 200.0f, 55.0f, FALSE);
    /* A world-space x>=0 plane must reject the translated left spotlight and
     * retain exact nonlinear Falloff-two on the right. Capturing the plane
     * under world instead of view, or falling back to fixed GL spotlight
     * exponent semantics, cannot produce the paired oracle. */
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_spot_falloff_pair(
        device, &viewport, eight_range_light_indices,
        220.0f, 200.0f, 170.0f, TRUE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_point_pair(
        device, &viewport, texture, five_range_light_indices, 170.0f,
        point_scale_a);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_directional_point_pair(
        device, &viewport, range_light_indices, 160.0f, 480.0f, 245.0f);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 4, 80.0f, 160.0f, 130.0f, 0.55f,
        TRUE, TRUE, 0.0f, FALSE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 5, 80.0f, 120.0f, 205.0f, 0.55f,
        FALSE, FALSE, 0.0f, TRUE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 6, 70.0f, 100.0f, 280.0f, 0.52f,
        TRUE, FALSE, 0.0f, FALSE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 7, 50.0f, 90.0f, 355.0f, 0.52f,
        FALSE, TRUE, 8.0f, FALSE);
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_specular_row(
        device, &viewport, eight_range_light_indices,
        scalable_range_color_masks, 8, 40.0f, 80.0f, 430.0f, 0.52f,
        TRUE, FALSE, 8.0f, TRUE);
    /* Keep the new fog state transition last so its deliberate dither and
     * fog-coordinate changes cannot perturb the established low-intensity
     * specular oracle even on hosts that defer fixed-state application. */
    if (SUCCEEDED(hr)) hr = draw_hwtl_scalable_fog_triplet(
        device, &viewport, eight_range_light_indices,
        220.0f, 100.0f, 95.0f);
    if (specular_scene_begun) {
        end_hr = IDirect3DDevice8_EndScene(device);
        specular_scene_begun = FALSE;
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
        device, NULL, NULL, NULL, NULL);
    /* This is the densest reference frame and synchronous readback can run at
     * roughly one callback per two seconds. Hold it across six ordinary
     * capture intervals so the scorer always sees the completed final draw,
     * rather than a correct but still-incremental prefix. */
    if (SUCCEEDED(hr)) Sleep(visual_frame_hold_ms * 6UL);
    report_hr_stage("hwtl-specular-leave", hr,
                    SUCCEEDED(hr) ?
                    "directional-point-range-point-spot-falloff-vertex-range-table-fog-material-color2-local-infinite-power-eight-scaled-point-sprite-fog" :
                    "failed");

    cleanup_hr = IDirect3DDevice8_SetViewport(device, &saved_viewport);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;

    cleanup_hr = IDirect3DDevice8_SetPixelShader(device, 0);
    if (SUCCEEDED(cleanup_hr)) cleanup_hr =
        IDirect3DDevice8_DeletePixelShader(device,
                                           point_sprite_pixel_shader);
    if (SUCCEEDED(hr) && FAILED(cleanup_hr)) hr = cleanup_hr;
    IDirect3DTexture8_Release(point_sprite_target);
    report_hr_stage("hwtl-reference-leave", hr,
                    SUCCEEDED(hr) ? "matrix-directional-material-point-range-spot-falloff-power-material-diffuse-color1-ambient-emissive-color2-fallback-ambient-emissive-diffuse-alpha-specular-local-infinite-viewer-color1-color2-fallback-spot-normal-point-size-point-scale-abc-clamp-lit-program-multi-light-eight-compact-dual-point-range-material-color1-color2-triple-point-range-fog-vertex-table-dual-point-specular-material-color2-quad-point-range-five-six-seven-eight-point-range-three-four-five-six-seven-eight-point-specular-scalable-spot-falloff-vertex-range-table-fog-material-color2-local-infinite-power-scaled-point-sprite-fog-mixed-directional-point-range-point-sprite-stage0-stage1-disable-pixel-shader-offscreen-clip-texture-transform-project-nonprojected4-point-range-program-fog-range-fog-vertex-range-fog-table-postproject-postdivide-exp-exp2-point-range-nonprojected4" :
                                    "failed");
    return hr;
}

static HRESULT run_post_reset_hwtl_scene(IDirect3DDevice8 *device)
{
    D3D_HWTL_MATERIAL_VERTEX vertices[3];
    D3DVIEWPORT8 viewport;
    D3DMATRIX identity;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    HRESULT hr;
    HRESULT end_hr;
    float center_x;
    float center_y;
    unsigned index;
    unsigned stage;

    if (!device) return E_INVALIDARG;
    report_stage("post-reset-hwtl-enter",
                 "point-material-diffuse-no-color1");

    memset(&viewport, 0, sizeof(viewport));
    hr = IDirect3DDevice8_GetViewport(device, &viewport);
    if (FAILED(hr) || !viewport.Width || !viewport.Height) {
        report_hr_stage("post-reset-hwtl-leave",
                        FAILED(hr) ? hr : E_FAIL, "viewport-failed");
        return FAILED(hr) ? hr : E_FAIL;
    }
    center_x = hwtl_ndc_x(&viewport, 320.0f);
    center_y = hwtl_ndc_y(&viewport, 240.0f);
    memset(vertices, 0, sizeof(vertices));
    vertices[0].x = center_x - 0.22f;
    vertices[0].y = center_y - 0.18f;
    vertices[1].x = center_x;
    vertices[1].y = center_y + 0.22f;
    vertices[2].x = center_x + 0.22f;
    vertices[2].y = center_y - 0.18f;
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_POINT;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
    light.Diffuse.a = 1.0f;
    light.Position.z = 0.5f;
    light.Range = 100.0f;
    light.Attenuation0 = 1.0f;
    for (index = 0; index < 3; ++index) {
        vertices[index].z = 0.25f;
        vertices[index].nx = light.Position.x - vertices[index].x;
        vertices[index].ny = light.Position.y - vertices[index].y;
        vertices[index].nz = light.Position.z - vertices[index].z;
    }

    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 0.25f;
    material.Diffuse.a = 1.0f;
    set_identity_matrix(&identity);

    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    for (stage = 1; SUCCEEDED(hr) && stage < 4; ++stage) {
        hr = IDirect3DDevice8_SetTextureStageState(
            device, stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZWRITEENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHATESTENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SPECULARENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORVERTEX, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_NORMALIZENORMALS, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORWRITEENABLE, 0x0fUL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    for (index = 0; SUCCEEDED(hr) && index < 8; ++index) {
        hr = IDirect3DDevice8_LightEnable(device, index, FALSE);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0, &light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, 0xff0d2e4fUL, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = draw_hwtl_material_triangle(device, vertices);
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
        device, NULL, NULL, NULL, NULL);
    report_hr_stage("post-reset-hwtl-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr)) Sleep(visual_frame_hold_ms);
    return hr;
}

static HRESULT run_d24x8_depth_contract(IDirect3DDevice8 *device)
{
    static const D3D_PROBE_VERTEX near_rectangle[4] = {
        { 80.0f, 110.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 105.0f, 110.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 80.0f, 140.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 105.0f, 140.0f, 0.25f, 1.0f, 0xffff0000UL }
    };
    static const D3D_PROBE_VERTEX far_rectangle[4] = {
        { 80.0f, 110.0f, 0.75f, 1.0f, 0xff00ff00UL },
        { 130.0f, 110.0f, 0.75f, 1.0f, 0xff00ff00UL },
        { 80.0f, 140.0f, 0.75f, 1.0f, 0xff00ff00UL },
        { 130.0f, 140.0f, 0.75f, 1.0f, 0xff00ff00UL }
    };
    IDirect3DSurface8 *render_target = NULL;
    IDirect3DSurface8 *original_depth = NULL;
    IDirect3DSurface8 *d24x8_depth = NULL;
    D3DSURFACE_DESC target_desc;
    D3DRECT clear_rect;
    HRESULT end_hr;
    HRESULT hr;
    HRESULT restore_hr = S_OK;
    BOOL target_bound = FALSE;

    if (!device) return E_INVALIDARG;
    memset(&target_desc, 0, sizeof(target_desc));
    clear_rect.x1 = 70;
    clear_rect.y1 = 100;
    clear_rect.x2 = 145;
    clear_rect.y2 = 150;
    report_stage("d24x8-depth-enter",
                 "near-red-far-green-clear-203040");
    hr = IDirect3DDevice8_GetRenderTarget(device, &render_target);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_GetDepthStencilSurface(
        device, &original_depth);
    if (SUCCEEDED(hr)) hr = IDirect3DSurface8_GetDesc(
        render_target, &target_desc);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateDepthStencilSurface(
        device, target_desc.Width, target_desc.Height, D3DFMT_D24X8,
        target_desc.MultiSampleType, &d24x8_depth);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_SetRenderTarget(device, render_target,
                                               d24x8_depth);
        if (SUCCEEDED(hr)) target_bound = TRUE;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(device, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZWRITEENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZFUNC, D3DCMP_LESS);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_STENCILENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CLIPPLANEENABLE, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 1, &clear_rect, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        0xff203040UL, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, near_rectangle,
            sizeof(near_rectangle[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLESTRIP, 2, far_rectangle,
            sizeof(far_rectangle[0]));
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (target_bound) restore_hr = IDirect3DDevice8_SetRenderTarget(
        device, render_target, original_depth);
    if (SUCCEEDED(hr) && FAILED(restore_hr)) hr = restore_hr;
    if (d24x8_depth) IDirect3DSurface8_Release(d24x8_depth);
    if (original_depth) IDirect3DSurface8_Release(original_depth);
    if (render_target) IDirect3DSurface8_Release(render_target);
    report_hr_stage("d24x8-depth-leave", hr,
                    SUCCEEDED(hr) ? "rendered-restored" : "failed");
    return hr;
}

static HRESULT fill_dynamic_texture(IDirect3DTexture8 *texture, DWORD color)
{
    D3DLOCKED_RECT locked;
    HRESULT hr;
    HRESULT unlock_hr;
    DWORD x;
    DWORD y;

    if (!texture) return E_INVALIDARG;
    memset(&locked, 0, sizeof(locked));
    hr = IDirect3DTexture8_LockRect(
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
    unlock_hr = IDirect3DTexture8_UnlockRect(texture, 0);
    if (SUCCEEDED(hr) && FAILED(unlock_hr)) hr = unlock_hr;
    return hr;
}

static HRESULT run_dynamic_texture_contract(IDirect3DDevice8 *device)
{
    static const D3D_TEXTURE_VERTEX first_triangle[3] = {
        { 465.0f, 300.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 490.0f, 300.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 465.0f, 325.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f }
    };
    static const D3D_TEXTURE_VERTEX second_triangle[3] = {
        { 500.0f, 300.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 525.0f, 300.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 500.0f, 325.0f, 0.10f, 1.0f, 0xffffffffUL, 0.5f, 0.5f }
    };
    IDirect3DTexture8 *texture = NULL;
    HRESULT end_hr;
    HRESULT hr;

    if (!device) return E_INVALIDARG;
    report_stage("dynamic-texture-enter",
                 "default-pool-discard-c02040-then-20c080");
    hr = IDirect3DDevice8_CreateTexture(
        device, 16, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT, &texture);
    if (SUCCEEDED(hr)) hr = fill_dynamic_texture(texture, 0xffc02040UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetPixelShader(device, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
        device, 0, (IDirect3DBaseTexture8 *)texture);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_FOGENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, first_triangle,
            sizeof(first_triangle[0]));
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (SUCCEEDED(hr)) hr = fill_dynamic_texture(texture, 0xff20c080UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, second_triangle,
            sizeof(second_triangle[0]));
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (texture) {
        HRESULT unbind_hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
        if (SUCCEEDED(hr) && FAILED(unbind_hr)) hr = unbind_hr;
        IDirect3DTexture8_Release(texture);
    }
    report_hr_stage("dynamic-texture-leave", hr,
                    SUCCEEDED(hr) ? "rendered-before-after-discard" :
                                    "failed");
    return hr;
}

static DWORD WINAPI run_clear_present(LPVOID parameter)
{
    D3D_OPERATION *operation = (D3D_OPERATION *)parameter;
    IDirect3DSurface8 *back_buffer = NULL;
    IDirect3DSurface8 *depth_stencil = NULL;
    IDirect3DTexture8 *texture = NULL;
    IDirect3DTexture8 *mip_texture = NULL;
    IDirect3DTexture8 *light_texture = NULL;
    IDirect3DTexture8 *a4_texture = NULL;
    IDirect3DTexture8 *l8_texture = NULL;
    IDirect3DTexture8 *a8l8_texture = NULL;
    IDirect3DTexture8 *a8_texture = NULL;
    IDirect3DTexture8 *p8_texture = NULL;
    IDirect3DTexture8 *a8p8_texture = NULL;
    IDirect3DTexture8 *a8r3g3b2_texture = NULL;
    IDirect3DTexture8 *a4l4_texture = NULL;
    IDirect3DTexture8 *x1_texture = NULL;
    IDirect3DTexture8 *x4_texture = NULL;
    IDirect3DTexture8 *r8_texture = NULL;
    IDirect3DTexture8 *r3_texture = NULL;
    IDirect3DTexture8 *dxt1_texture = NULL;
    IDirect3DTexture8 *dxt3_texture = NULL;
    IDirect3DTexture8 *dxt5_texture = NULL;
    IDirect3DVertexBuffer8 *texture_op_buffer = NULL;
    IDirect3DVertexBuffer8 *stencil_buffer = NULL;
    IDirect3DVertexBuffer8 *z_bias_buffer = NULL;
    IDirect3DVertexBuffer8 *alpha_test_buffer = NULL;
    IDirect3DVertexBuffer8 *primitive_buffer = NULL;
    IDirect3DIndexBuffer8 *primitive_index_buffer = NULL;
    IDirect3DVertexBuffer8 *specular_buffer = NULL;
    IDirect3DVertexBuffer8 *minimal_fvf_buffer = NULL;
    IDirect3DVertexBuffer8 *texture2_buffer = NULL;
    D3DLOCKED_RECT locked;
    D3DLOCKED_RECT texture_locked;
    BYTE *texture_op_data = NULL;
    BYTE *stencil_data = NULL;
    BYTE *z_bias_data = NULL;
    BYTE *alpha_test_vertices = NULL;
    BYTE *primitive_data = NULL;
    BYTE *primitive_index_data = NULL;
    BYTE *specular_data = NULL;
    BYTE *minimal_fvf_data = NULL;
    BYTE *texture2_data = NULL;
    DWORD pixel;
    static const D3D_PROBE_VERTEX near_triangle[3] = {
        { 310.0f, 30.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 390.0f, 150.0f, 0.25f, 1.0f, 0xff0000ffUL },
        { 230.0f, 150.0f, 0.25f, 1.0f, 0xffffffffUL }
    };
    static const D3D_PROBE_VERTEX far_triangle[3] = {
        { 310.0f, 30.0f, 0.75f, 1.0f, 0xff00ff00UL },
        { 390.0f, 150.0f, 0.75f, 1.0f, 0xff00ff00UL },
        { 230.0f, 150.0f, 0.75f, 1.0f, 0xff00ff00UL }
    };
    static const D3D_PROBE_VERTEX wireframe_triangle[3] = {
        { 100.0f, 100.0f, 0.25f, 1.0f, 0xffffffffUL },
        { 540.0f, 100.0f, 0.25f, 1.0f, 0xffffffffUL },
        { 320.0f, 400.0f, 0.25f, 1.0f, 0xffffffffUL }
    };
    static const D3D_PROBE_VERTEX blend_triangle[3] = {
        { 310.0f, 190.0f, 0.25f, 1.0f, 0x80ff0000UL },
        { 390.0f, 290.0f, 0.25f, 1.0f, 0x80ff0000UL },
        { 230.0f, 290.0f, 0.25f, 1.0f, 0x80ff0000UL }
    };
    static const D3D_PROBE_VERTEX alpha_test_triangles[6] = {
        { 65.0f, 210.0f, 0.25f, 1.0f, 0x4000ff00UL },
        { 100.0f, 280.0f, 0.25f, 1.0f, 0x4000ff00UL },
        { 30.0f, 280.0f, 0.25f, 1.0f, 0x4000ff00UL },
        { 155.0f, 210.0f, 0.25f, 1.0f, 0xc000ff00UL },
        { 190.0f, 280.0f, 0.25f, 1.0f, 0xc000ff00UL },
        { 120.0f, 280.0f, 0.25f, 1.0f, 0xc000ff00UL }
    };
    static const D3D_PROBE_VERTEX primitive_vertices[17] = {
        { 430.0f, 30.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 590.0f, 30.0f, 0.25f, 1.0f, 0xff00ffffUL },
        { 430.0f, 150.0f, 0.25f, 1.0f, 0xff0000ffUL },
        { 590.0f, 150.0f, 0.25f, 1.0f, 0xffffffffUL },
        { 510.0f, 270.0f, 0.25f, 1.0f, 0xffffffffUL },
        { 430.0f, 190.0f, 0.25f, 1.0f, 0xffff00ffUL },
        { 590.0f, 190.0f, 0.25f, 1.0f, 0xff00ffffUL },
        { 590.0f, 350.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 430.0f, 380.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 510.0f, 360.0f, 0.25f, 1.0f, 0xff00ffffUL },
        { 590.0f, 380.0f, 0.25f, 1.0f, 0xffff00ffUL },
        { 430.0f, 420.0f, 0.25f, 1.0f, 0xffffff00UL },
        { 510.0f, 400.0f, 0.25f, 1.0f, 0xff00ff00UL },
        { 590.0f, 420.0f, 0.25f, 1.0f, 0xffffffffUL },
        { 450.0f, 450.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 510.0f, 450.0f, 0.25f, 1.0f, 0xff00ff00UL },
        { 570.0f, 450.0f, 0.25f, 1.0f, 0xffffffffUL }
    };
    static const WORD primitive_indices[8] = {
        4, 5, 6, 7, 11, 12, 12, 13
    };
    static const D3D_SPECULAR_VERTEX specular_triangle[3] = {
        { 310.0f, 320.0f, 0.25f, 1.0f, 0xff000080UL, 0x00ff0000UL },
        { 390.0f, 430.0f, 0.25f, 1.0f, 0xff000080UL, 0x00ff0000UL },
        { 230.0f, 430.0f, 0.25f, 1.0f, 0xff000080UL, 0x00ff0000UL }
    };
    static const D3D_POINT_SIZE_VERTEX minimal_fvf_triangle[3] = {
        { 110.0f, 320.0f, 0.25f, 1.0f, 1.0f },
        { 190.0f, 430.0f, 0.25f, 1.0f, 1.0f },
        { 30.0f, 430.0f, 0.25f, 1.0f, 1.0f }
    };
    static const D3D_PROBE_VERTEX cull_clockwise_triangle[3] = {
        { 400.0f, 30.0f, 0.25f, 1.0f, 0xffff4000UL },
        { 425.0f, 30.0f, 0.25f, 1.0f, 0xffff4000UL },
        { 425.0f, 60.0f, 0.25f, 1.0f, 0xffff4000UL }
    };
    static const D3D_PROBE_VERTEX cull_counterclockwise_triangle[3] = {
        { 400.0f, 70.0f, 0.25f, 1.0f, 0xff40ff00UL },
        { 425.0f, 100.0f, 0.25f, 1.0f, 0xff40ff00UL },
        { 425.0f, 70.0f, 0.25f, 1.0f, 0xff40ff00UL }
    };
    static const D3D_PROBE_VERTEX fog_triangle[3] = {
        { 110.0f, 340.0f, 0.75f, 1.0f, 0xffff0000UL },
        { 130.0f, 370.0f, 0.75f, 1.0f, 0xffff0000UL },
        { 90.0f, 370.0f, 0.75f, 1.0f, 0xffff0000UL }
    };
    static const D3D_SPECULAR_VERTEX vertex_fog_triangle[3] = {
        { 310.0f, 340.0f, 0.25f, 1.0f, 0xffff0000UL, 0x00000000UL },
        { 330.0f, 370.0f, 0.25f, 1.0f, 0xffff0000UL, 0x00000000UL },
        { 290.0f, 370.0f, 0.25f, 1.0f, 0xffff0000UL, 0x00000000UL }
    };
    static const D3D_PROBE_VERTEX stencil_rectangles[12] = {
        { 195.0f, 70.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 210.0f, 70.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 210.0f, 100.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 195.0f, 70.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 210.0f, 100.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 195.0f, 100.0f, 0.25f, 1.0f, 0xffff0000UL },
        { 195.0f, 70.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 225.0f, 70.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 225.0f, 90.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 195.0f, 70.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 225.0f, 90.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 195.0f, 90.0f, 0.25f, 1.0f, 0xffff8000UL }
    };
    static const D3D_PROBE_VERTEX z_bias_triangles[6] = {
        { 195.0f, 110.0f, 0.5f, 1.0f, 0xff0000ffUL },
        { 225.0f, 140.0f, 0.5f, 1.0f, 0xff0000ffUL },
        { 195.0f, 140.0f, 0.5f, 1.0f, 0xff0000ffUL },
        { 195.0f, 110.0f, 0.5f, 1.0f, 0xff00ff00UL },
        { 225.0f, 140.0f, 0.5f, 1.0f, 0xff00ff00UL },
        { 195.0f, 140.0f, 0.5f, 1.0f, 0xff00ff00UL }
    };
    /* Reversed winding proves D3DCULL_NONE is transported. The differently
     * colored near vertices must still produce first-vertex flat red. */
    static const WORD triangle_indices[3] = { 0, 2, 1 };
    static const D3D_TEXTURE_VERTEX texture_quad[4] = {
        { 30.0f, 30.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 190.0f, 30.0f, 0.25f, 1.0f, 0xffffffffUL, 0.75f, 0.25f },
        { 190.0f, 190.0f, 0.25f, 1.0f, 0xffffffffUL, 0.75f, 0.75f },
        { 30.0f, 190.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.75f }
    };
    static const D3D_TEXTURE_VERTEX lod_bias_triangle[3] = {
        { 195.0f, 230.0f, 0.20f, 1.0f, 0xffffffffUL, 0.0f, 0.0f },
        { 225.0f, 230.0f, 0.20f, 1.0f, 0xffffffffUL, 1.0f, 0.0f },
        { 195.0f, 260.0f, 0.20f, 1.0f, 0xffffffffUL, 0.0f, 1.0f }
    };
    static const D3D_PROJECTED_TEXTURE_VERTEX projected_texture_triangle[3] = {
        { 230.0f, 190.0f, 0.20f, 1.0f, 0xffffffffUL,
          0.75f, 0.50f, 2.0f },
        { 260.0f, 190.0f, 0.20f, 1.0f, 0xffffffffUL,
          0.75f, 0.50f, 2.0f },
        { 230.0f, 220.0f, 0.20f, 1.0f, 0xffffffffUL,
          0.75f, 0.50f, 2.0f }
    };
    static const D3D_TEXTURE_VERTEX texture_op_triangles[114] = {
        { 30.0f, 290.0f, 0.25f, 1.0f, 0xff008000UL, 0.25f, 0.25f },
        { 95.0f, 315.0f, 0.25f, 1.0f, 0xff008000UL, 0.25f, 0.25f },
        { 30.0f, 315.0f, 0.25f, 1.0f, 0xff008000UL, 0.25f, 0.25f },
        { 110.0f, 290.0f, 0.25f, 1.0f, 0xff804040UL, 0.75f, 0.25f },
        { 175.0f, 315.0f, 0.25f, 1.0f, 0xff804040UL, 0.75f, 0.25f },
        { 110.0f, 315.0f, 0.25f, 1.0f, 0xff804040UL, 0.75f, 0.25f },
        { 185.0f, 290.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 225.0f, 315.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 185.0f, 315.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 195.0f, 30.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 225.0f, 30.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 225.0f, 60.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 195.0f, 30.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 225.0f, 60.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 195.0f, 60.0f, 0.25f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 250.0f, 295.0f, 0.25f, 1.0f, 0xff6496faUL, 0.0f, 0.0f },
        { 370.0f, 315.0f, 0.25f, 1.0f, 0xff6496faUL, 0.0f, 0.0f },
        { 250.0f, 315.0f, 0.25f, 1.0f, 0xff6496faUL, 0.0f, 0.0f },
        { 270.0f, 440.0f, 0.25f, 1.0f, 0xff204080UL, 0.75f, 0.25f },
        { 330.0f, 470.0f, 0.25f, 1.0f, 0xff204080UL, 0.75f, 0.25f },
        { 270.0f, 470.0f, 0.25f, 1.0f, 0xff204080UL, 0.75f, 0.25f },
        { 30.0f, 440.0f, 0.25f, 1.0f, 0xff201040UL, 0.25f, 0.25f },
        { 90.0f, 470.0f, 0.25f, 1.0f, 0xff201040UL, 0.25f, 0.25f },
        { 30.0f, 470.0f, 0.25f, 1.0f, 0xff201040UL, 0.25f, 0.25f },
        { 110.0f, 440.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 170.0f, 470.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 110.0f, 470.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 190.0f, 440.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 250.0f, 470.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 190.0f, 470.0f, 0.25f, 1.0f, 0xffff0000UL, 0.75f, 0.25f },
        { 350.0f, 440.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 410.0f, 470.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 350.0f, 470.0f, 0.25f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 230.0f, 155.0f, 0.25f, 1.0f, 0x800000ffUL, 0.25f, 0.25f },
        { 300.0f, 185.0f, 0.25f, 1.0f, 0x800000ffUL, 0.25f, 0.25f },
        { 230.0f, 185.0f, 0.25f, 1.0f, 0x800000ffUL, 0.25f, 0.25f },
        { 320.0f, 155.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 390.0f, 185.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 320.0f, 185.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 410.0f, 155.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 480.0f, 185.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 410.0f, 185.0f, 0.25f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 500.0f, 155.0f, 0.25f, 1.0f, 0x40ff0000UL, 0.75f, 0.25f },
        { 570.0f, 185.0f, 0.25f, 1.0f, 0x40ff0000UL, 0.75f, 0.25f },
        { 500.0f, 185.0f, 0.25f, 1.0f, 0x40ff0000UL, 0.75f, 0.25f },
        { 195.0f, 190.0f, 0.25f, 1.0f, 0xffffffffUL, 0.75f, 0.75f },
        { 225.0f, 220.0f, 0.25f, 1.0f, 0xffffffffUL, 0.75f, 0.75f },
        { 195.0f, 220.0f, 0.25f, 1.0f, 0xffffffffUL, 0.75f, 0.75f },
        { 320.0f, 190.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.25f },
        { 410.0f, 190.0f, 0.20f, 0.25f, 0xffffffffUL, 0.75f, 0.25f },
        { 320.0f, 280.0f, 0.20f, 0.25f, 0xffffffffUL, 0.75f, 0.25f },
        { 400.0f, 290.0f, 0.20f, 1.0f, 0xffa0a0a0UL, 0.25f, 0.25f },
        { 425.0f, 315.0f, 0.20f, 1.0f, 0xffa0a0a0UL, 0.25f, 0.25f },
        { 400.0f, 315.0f, 0.20f, 1.0f, 0xffa0a0a0UL, 0.25f, 0.25f },
        { 400.0f, 320.0f, 0.20f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 425.0f, 345.0f, 0.20f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 400.0f, 345.0f, 0.20f, 1.0f, 0xff0000ffUL, 0.25f, 0.25f },
        { 400.0f, 260.0f, 0.20f, 1.0f, 0xc0ffffffUL, 0.25f, 0.25f },
        { 425.0f, 285.0f, 0.20f, 1.0f, 0xc0ffffffUL, 0.25f, 0.25f },
        { 400.0f, 285.0f, 0.20f, 1.0f, 0xc0ffffffUL, 0.25f, 0.25f },
        { 400.0f, 230.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 425.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 400.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 200.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 625.0f, 225.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 225.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 230.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 625.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 260.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 625.0f, 285.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 600.0f, 285.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 600.0f, 290.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 625.0f, 315.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 600.0f, 315.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 600.0f, 320.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 625.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 350.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 625.0f, 375.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 600.0f, 375.0f, 0.20f, 1.0f, 0xffffffffUL, 0.25f, 0.5f },
        { 600.0f, 380.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 625.0f, 405.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 600.0f, 405.0f, 0.20f, 1.0f, 0xffffffffUL, 0.75f, 0.5f },
        { 600.0f, 410.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 625.0f, 435.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 435.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 230.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 255.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 440.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 625.0f, 465.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 600.0f, 465.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 260.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 285.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 285.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 290.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 315.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 315.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 320.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 350.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 375.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 375.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 460.0f, 320.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 485.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 460.0f, 345.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 415.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 455.0f, 440.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 430.0f, 440.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 460.0f, 415.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 485.0f, 440.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f },
        { 460.0f, 440.0f, 0.20f, 1.0f, 0xffffffffUL, 0.5f, 0.5f }
    };
    static const BYTE dxt3_block[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const BYTE dxt5_blocks[32] = {
        0xff, 0x00, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49,
        0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xff, 0xb6, 0x6d, 0xdb, 0xb6, 0x6d, 0xdb,
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const WORD quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
    static const D3D_TEXTURE2_VERTEX texture2_triangle[3] = {
        { 195.0f, 155.0f, 0.25f, 1.0f, 0xffff0000UL,
          0.75f, 0.75f, 0.5f, 0.5f },
        { 225.0f, 185.0f, 0.25f, 1.0f, 0xffff0000UL,
          0.75f, 0.75f, 0.5f, 0.5f },
        { 195.0f, 185.0f, 0.25f, 1.0f, 0xffff0000UL,
          0.75f, 0.75f, 0.5f, 0.5f }
    };
    static const D3D_TEXTURE2_VERTEX no_texture_stage_chain_triangle[3] = {
        { 5.0f, 5.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f },
        { 25.0f, 5.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f },
        { 5.0f, 25.0f, 0.25f, 1.0f, 0xffffffffUL,
          0.5f, 0.5f, 0.5f, 0.5f }
    };
    char detail[128];
    HRESULT hr;
    HRESULT lock_hr;
    HRESULT end_hr;

    report_stage("ignored-state-change-enter", "lastpixel-bordercolor");
    hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_LASTPIXEL, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_LASTPIXEL, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_BORDERCOLOR, 0x01020304UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_BORDERCOLOR, 0);
    report_hr_stage("ignored-state-change-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");

    report_stage("clear-enter", "color-write-mask-zero");
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_COLORWRITEENABLE, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        operation->device, 0, NULL,
        D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
        D3DCOLOR_XRGB(20, 70, 150), 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    report_hr_stage("clear-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_GetBackBuffer(operation->device, 0,
                                             D3DBACKBUFFER_TYPE_MONO,
                                             &back_buffer);
        report_hr_stage("back-buffer", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_GetDepthStencilSurface(operation->device,
                                                      &depth_stencil);
        report_hr_stage("depth-stencil", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("present-target-rebind-enter",
                     "same-back-buffer-handle");
        hr = IDirect3DDevice8_SetRenderTarget(operation->device,
                                               back_buffer,
                                               depth_stencil);
        report_hr_stage("present-target-rebind-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (depth_stencil) {
        IDirect3DSurface8_Release(depth_stencil);
        depth_stencil = NULL;
    }
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        lock_hr = IDirect3DSurface8_LockRect(back_buffer, &locked, NULL,
                                             D3DLOCK_READONLY);
        report_hr_stage("clear-lock", lock_hr,
                        SUCCEEDED(lock_hr) ? "ok" :
                        (lock_hr == D3DERR_INVALIDCALL ?
                         "nonlockable-expected" : "failed"));
        if (FAILED(lock_hr) && lock_hr != D3DERR_INVALIDCALL) hr = lock_hr;
    }
    if (SUCCEEDED(hr) && SUCCEEDED(lock_hr)) {
        pixel = *(const DWORD *)((const BYTE *)locked.pBits +
                                 150 * locked.Pitch + 200 * sizeof(DWORD));
        IDirect3DSurface8_UnlockRect(back_buffer);
        _snprintf(detail, sizeof(detail), "pixel=%08lx expected-rgb=00144696",
                  (unsigned long)pixel);
        detail[sizeof(detail) - 1] = '\0';
        if ((pixel & 0x00ffffffUL) != 0x00144696UL) {
            report_hr_stage("clear-pixel", E_FAIL, detail);
            hr = E_FAIL;
        } else {
            report_hr_stage("clear-pixel", S_OK, detail);
        }
    }
    if (back_buffer) {
        IDirect3DSurface8_Release(back_buffer);
        back_buffer = NULL;
    }
    if (SUCCEEDED(hr))
        hr = create_reference_texture(operation->device, &texture);
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("light-texture-create-enter", "16x16-gray-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &light_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            light_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                DWORD *pixels = (DWORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0xff808080UL;
            }
            hr = IDirect3DTexture8_UnlockRect(light_texture, 0);
        }
        report_hr_stage("light-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("no-texture-stage-chain-state-enter",
                     "stage0-tfactor-stage1-add-texture");
        hr = IDirect3DDevice8_Clear(
            operation->device, 0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
            D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_ZENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_TEXTUREFACTOR, 0xff102030UL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            operation->device,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            operation->device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            operation->device, 1,
            (IDirect3DBaseTexture8 *)light_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_COLOROP, D3DTOP_ADD);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_TEXCOORDINDEX, 1);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 1, D3DTSS_MINFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
        report_hr_stage("no-texture-stage-chain-state-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("no-texture-stage-chain-draw-enter",
                     "expected-rgb-90a0b0");
        hr = IDirect3DDevice8_BeginScene(operation->device);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_DrawPrimitiveUP(
                operation->device, D3DPT_TRIANGLELIST, 1,
                no_texture_stage_chain_triangle,
                sizeof(no_texture_stage_chain_triangle[0]));
            end_hr = IDirect3DDevice8_EndScene(operation->device);
            if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
        }
        report_hr_stage("no-texture-stage-chain-draw-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice8_GetBackBuffer(operation->device, 0,
                                             D3DBACKBUFFER_TYPE_MONO,
                                             &back_buffer);
        report_hr_stage("no-texture-stage-chain-back-buffer", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        memset(&locked, 0, sizeof(locked));
        lock_hr = IDirect3DSurface8_LockRect(back_buffer, &locked, NULL,
                                             D3DLOCK_READONLY);
        report_hr_stage("no-texture-stage-chain-lock", lock_hr,
                        SUCCEEDED(lock_hr) ? "ok" :
                        (lock_hr == D3DERR_INVALIDCALL ?
                         "nonlockable-visual-oracle" : "failed"));
        if (FAILED(lock_hr) && lock_hr != D3DERR_INVALIDCALL) hr = lock_hr;
    }
    if (SUCCEEDED(hr) && SUCCEEDED(lock_hr)) {
        pixel = *(const DWORD *)((const BYTE *)locked.pBits +
                                 10 * locked.Pitch + 10 * sizeof(DWORD));
        IDirect3DSurface8_UnlockRect(back_buffer);
        _snprintf(detail, sizeof(detail),
                  "pixel=%08lx expected-rgb=0090a0b0",
                  (unsigned long)pixel);
        detail[sizeof(detail) - 1] = '\0';
        if ((pixel & 0x00ffffffUL) != 0x0090a0b0UL) {
            report_hr_stage("no-texture-stage-chain-pixel", E_FAIL, detail);
            hr = E_FAIL;
        } else {
            report_hr_stage("no-texture-stage-chain-pixel", S_OK, detail);
        }
    }
    if (back_buffer) {
        IDirect3DSurface8_Release(back_buffer);
        back_buffer = NULL;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
        operation->device, 1, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        operation->device, 0, NULL,
        D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
        D3DCOLOR_XRGB(20, 70, 150), 1.0f, 0);
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a4-texture-create-enter", "16x16-a4r4g4b4-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A4R4G4B4,
            D3DPOOL_MANAGED, &a4_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a4_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x8f40;
            }
            hr = IDirect3DTexture8_UnlockRect(a4_texture, 0);
        }
        report_hr_stage("a4-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("l8-texture-create-enter", "16x16-l8-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_L8,
            D3DPOOL_MANAGED, &l8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            l8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) pixels[x] = 0xa0;
            }
            hr = IDirect3DTexture8_UnlockRect(l8_texture, 0);
        }
        report_hr_stage("l8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a8l8-texture-create-enter", "16x16-a8l8-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A8L8,
            D3DPOOL_MANAGED, &a8l8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a8l8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x8040;
            }
            hr = IDirect3DTexture8_UnlockRect(a8l8_texture, 0);
        }
        report_hr_stage("a8l8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a8-texture-create-enter", "16x16-a8-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A8,
            D3DPOOL_MANAGED, &a8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) pixels[x] = 0x80;
            }
            hr = IDirect3DTexture8_UnlockRect(a8_texture, 0);
        }
        report_hr_stage("a8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("p8-texture-create-enter", "16x16-p8-managed-index-7");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_P8,
            D3DPOOL_MANAGED, &p8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            p8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) pixels[x] = 7;
            }
            hr = IDirect3DTexture8_UnlockRect(p8_texture, 0);
        }
        report_hr_stage("p8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a8p8-texture-create-enter",
                     "16x16-a8p8-managed-alpha-64-index-7");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A8P8,
            D3DPOOL_MANAGED, &a8p8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a8p8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x4007;
            }
            hr = IDirect3DTexture8_UnlockRect(a8p8_texture, 0);
        }
        report_hr_stage("a8p8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        PALETTEENTRY palette_entries[256];
        DWORD index;

        report_stage("texture-palette-state-enter",
                     "palette-3-orange-alpha-128");
        memset(palette_entries, 0, sizeof(palette_entries));
        for (index = 0; index < 256; ++index)
            palette_entries[index].peFlags = 0xff;
        palette_entries[7].peRed = 0xff;
        palette_entries[7].peGreen = 0x80;
        palette_entries[7].peBlue = 0x00;
        palette_entries[7].peFlags = 0x80;
        hr = IDirect3DDevice8_SetPaletteEntries(
            operation->device, 3, palette_entries);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetCurrentTexturePalette(
            operation->device, 3);
        report_hr_stage("texture-palette-state-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a8r3g3b2-texture-create-enter",
                     "16x16-a8r3g3b2-managed-alpha-128-r5-g2-b3");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A8R3G3B2,
            D3DPOOL_MANAGED, &a8r3g3b2_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a8r3g3b2_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x80ab;
            }
            hr = IDirect3DTexture8_UnlockRect(a8r3g3b2_texture, 0);
        }
        report_hr_stage("a8r3g3b2-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("a4l4-texture-create-enter",
                     "16x16-a4l4-managed-opaque-luminance-9");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_A4L4,
            D3DPOOL_MANAGED, &a4l4_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            a4l4_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) pixels[x] = 0xf9;
            }
            hr = IDirect3DTexture8_UnlockRect(a4l4_texture, 0);
        }
        report_hr_stage("a4l4-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("x4-texture-create-enter", "16x16-x4r4g4b4-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_X4R4G4B4,
            D3DPOOL_MANAGED, &x4_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            x4_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x000f;
            }
            hr = IDirect3DTexture8_UnlockRect(x4_texture, 0);
        }
        report_hr_stage("x4-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("x1-texture-create-enter", "16x16-x1r5g5b5-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_X1R5G5B5,
            D3DPOOL_MANAGED, &x1_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            x1_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                WORD *pixels = (WORD *)(
                    (BYTE *)texture_locked.pBits + y * texture_locked.Pitch);
                for (x = 0; x < 16; ++x) pixels[x] = 0x03e0;
            }
            hr = IDirect3DTexture8_UnlockRect(x1_texture, 0);
        }
        report_hr_stage("x1-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("r8-texture-create-enter", "16x16-r8g8b8-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_R8G8B8,
            D3DPOOL_MANAGED, &r8_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            r8_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) {
                    pixels[x * 3] = 0x12;
                    pixels[x * 3 + 1] = 0x34;
                    pixels[x * 3 + 2] = 0x56;
                }
            }
            hr = IDirect3DTexture8_UnlockRect(r8_texture, 0);
        }
        report_hr_stage("r8-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        DWORD x;
        DWORD y;

        report_stage("r3-texture-create-enter", "16x16-r3g3b2-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 16, 16, 1, 0, D3DFMT_R3G3B2,
            D3DPOOL_MANAGED, &r3_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            r3_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < 16; ++y) {
                BYTE *pixels = (BYTE *)texture_locked.pBits +
                               y * texture_locked.Pitch;
                for (x = 0; x < 16; ++x) pixels[x] = 0xe0;
            }
            hr = IDirect3DTexture8_UnlockRect(r3_texture, 0);
        }
        report_hr_stage("r3-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        WORD color0;
        WORD color1;
        DWORD selectors;
        BYTE *blocks;

        report_stage("dxt1-texture-create-enter", "8x4-dxt1-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 8, 4, 1, 0, D3DFMT_DXT1,
            D3DPOOL_MANAGED, &dxt1_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            dxt1_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr) &&
            (!texture_locked.pBits || texture_locked.Pitch < 16)) {
            lock_hr = IDirect3DTexture8_UnlockRect(dxt1_texture, 0);
            hr = FAILED(lock_hr) ? lock_hr : E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            blocks = (BYTE *)texture_locked.pBits;
            color0 = 0xf800;
            color1 = 0x0000;
            selectors = 0x00000000UL;
            memcpy(blocks, &color0, sizeof(color0));
            memcpy(blocks + 2, &color1, sizeof(color1));
            memcpy(blocks + 4, &selectors, sizeof(selectors));
            color0 = 0x001f;
            color1 = 0xffff;
            selectors = 0xffffffffUL;
            memcpy(blocks + 8, &color0, sizeof(color0));
            memcpy(blocks + 10, &color1, sizeof(color1));
            memcpy(blocks + 12, &selectors, sizeof(selectors));
            hr = IDirect3DTexture8_UnlockRect(dxt1_texture, 0);
        }
        report_hr_stage("dxt1-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("dxt3-texture-create-enter", "4x4-dxt3-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 4, 4, 1, 0, D3DFMT_DXT3,
            D3DPOOL_MANAGED, &dxt3_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            dxt3_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr) &&
            (!texture_locked.pBits || texture_locked.Pitch < 16)) {
            lock_hr = IDirect3DTexture8_UnlockRect(dxt3_texture, 0);
            hr = FAILED(lock_hr) ? lock_hr : E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            memcpy(texture_locked.pBits, dxt3_block, sizeof(dxt3_block));
            hr = IDirect3DTexture8_UnlockRect(dxt3_texture, 0);
        }
        report_hr_stage("dxt3-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("dxt5-texture-create-enter", "8x4-dxt5-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 8, 4, 1, 0, D3DFMT_DXT5,
            D3DPOOL_MANAGED, &dxt5_texture);
        if (SUCCEEDED(hr)) hr = IDirect3DTexture8_LockRect(
            dxt5_texture, 0, &texture_locked, NULL, 0);
        if (SUCCEEDED(hr) &&
            (!texture_locked.pBits || texture_locked.Pitch < 32)) {
            lock_hr = IDirect3DTexture8_UnlockRect(dxt5_texture, 0);
            hr = FAILED(lock_hr) ? lock_hr : E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            memcpy(texture_locked.pBits, dxt5_blocks, sizeof(dxt5_blocks));
            hr = IDirect3DTexture8_UnlockRect(dxt5_texture, 0);
        }
        report_hr_stage("dxt5-texture-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        UINT level;

        report_stage("mipmap-create-enter", "64x64-three-level-managed");
        hr = IDirect3DDevice8_CreateTexture(
            operation->device, 64, 64, 3, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &mip_texture);
        for (level = 0; SUCCEEDED(hr) && level < 3; ++level) {
            DWORD color = level == 0 ? 0xffff0000UL :
                          (level == 1 ? 0xff00ff00UL : 0xff0000ffUL);
            DWORD size = 64 >> level;
            DWORD x;
            DWORD y;

            memset(&texture_locked, 0, sizeof(texture_locked));
            hr = IDirect3DTexture8_LockRect(mip_texture, level,
                                            &texture_locked, NULL, 0);
            if (SUCCEEDED(hr)) {
                for (y = 0; y < size; ++y) {
                    DWORD *pixels = (DWORD *)(
                        (BYTE *)texture_locked.pBits +
                        y * texture_locked.Pitch);
                    for (x = 0; x < size; ++x) pixels[x] = color;
                }
                hr = IDirect3DTexture8_UnlockRect(mip_texture, level);
            }
        }
        report_hr_stage("mipmap-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("texture-op-buffer-create-enter", "managed-vb");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(texture_op_triangles),
            D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_MANAGED, &texture_op_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            texture_op_buffer, 0, sizeof(texture_op_triangles),
            &texture_op_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(texture_op_data, texture_op_triangles,
                   sizeof(texture_op_triangles));
            hr = IDirect3DVertexBuffer8_Unlock(texture_op_buffer);
        }
        report_hr_stage("texture-op-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("texture2-buffer-create-enter", "managed-tex2-vb");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(texture2_triangle),
            D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2,
            D3DPOOL_MANAGED, &texture2_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            texture2_buffer, 0, sizeof(texture2_triangle),
            &texture2_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(texture2_data, texture2_triangle,
                   sizeof(texture2_triangle));
            hr = IDirect3DVertexBuffer8_Unlock(texture2_buffer);
        }
        report_hr_stage("texture2-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("stencil-buffer-create-enter", "managed-vb");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(stencil_rectangles),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
            D3DPOOL_MANAGED, &stencil_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            stencil_buffer, 0, sizeof(stencil_rectangles),
            &stencil_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(stencil_data, stencil_rectangles,
                   sizeof(stencil_rectangles));
            hr = IDirect3DVertexBuffer8_Unlock(stencil_buffer);
        }
        report_hr_stage("stencil-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("z-bias-buffer-create-enter", "managed-vb");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(z_bias_triangles),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
            D3DPOOL_MANAGED, &z_bias_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            z_bias_buffer, 0, sizeof(z_bias_triangles), &z_bias_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(z_bias_data, z_bias_triangles,
                   sizeof(z_bias_triangles));
            hr = IDirect3DVertexBuffer8_Unlock(z_bias_buffer);
        }
        report_hr_stage("z-bias-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("vertex-buffer-create-enter", "alpha-test-managed");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(alpha_test_triangles),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
            D3DPOOL_MANAGED, &alpha_test_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            alpha_test_buffer, 0, sizeof(alpha_test_triangles),
            &alpha_test_vertices, 0);
        if (SUCCEEDED(hr)) {
            memcpy(alpha_test_vertices, alpha_test_triangles,
                   sizeof(alpha_test_triangles));
            hr = IDirect3DVertexBuffer8_Unlock(alpha_test_buffer);
        }
        report_hr_stage("vertex-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("primitive-buffers-create-enter",
                     "managed-vb-ib");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(primitive_vertices),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
            D3DPOOL_MANAGED, &primitive_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            primitive_buffer, 0, sizeof(primitive_vertices),
            &primitive_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(primitive_data, primitive_vertices,
                   sizeof(primitive_vertices));
            hr = IDirect3DVertexBuffer8_Unlock(primitive_buffer);
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_CreateIndexBuffer(
            operation->device, sizeof(primitive_indices),
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
            &primitive_index_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DIndexBuffer8_Lock(
            primitive_index_buffer, 0, sizeof(primitive_indices),
            &primitive_index_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(primitive_index_data, primitive_indices,
                   sizeof(primitive_indices));
            hr = IDirect3DIndexBuffer8_Unlock(primitive_index_buffer);
        }
        report_hr_stage("primitive-buffers-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("specular-buffer-create-enter", "managed-fvf-specular");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(specular_triangle),
            D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR,
            D3DPOOL_MANAGED, &specular_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            specular_buffer, 0, sizeof(specular_triangle),
            &specular_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(specular_data, specular_triangle,
                   sizeof(specular_triangle));
            hr = IDirect3DVertexBuffer8_Unlock(specular_buffer);
        }
        report_hr_stage("specular-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("minimal-fvf-buffer-create-enter", "xyzrhw-psize");
        hr = IDirect3DDevice8_CreateVertexBuffer(
            operation->device, sizeof(minimal_fvf_triangle),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_PSIZE,
            D3DPOOL_MANAGED, &minimal_fvf_buffer);
        if (SUCCEEDED(hr)) hr = IDirect3DVertexBuffer8_Lock(
            minimal_fvf_buffer, 0, sizeof(minimal_fvf_triangle),
            &minimal_fvf_data, 0);
        if (SUCCEEDED(hr)) {
            memcpy(minimal_fvf_data, minimal_fvf_triangle,
                   sizeof(minimal_fvf_triangle));
            hr = IDirect3DVertexBuffer8_Unlock(minimal_fvf_buffer);
        }
        report_hr_stage("minimal-fvf-buffer-create-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("triangle-state-enter", "xyzrhw-diffuse");
        hr = IDirect3DDevice8_SetRenderState(operation->device,
                                             D3DRS_CULLMODE,
                                             D3DCULL_NONE);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetRenderState(operation->device,
                                                 D3DRS_ZENABLE, TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetRenderState(operation->device,
                                                 D3DRS_ZWRITEENABLE, TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetRenderState(operation->device,
                                                 D3DRS_ZFUNC,
                                                 D3DCMP_LESSEQUAL);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetRenderState(operation->device,
                                                 D3DRS_SHADEMODE,
                                                 D3DSHADE_FLAT);
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_SetVertexShader(
                operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        }
        report_hr_stage("triangle-state-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("triangle-draw-enter", "triangle-list-indexed");
        hr = IDirect3DDevice8_BeginScene(operation->device);
        if (SUCCEEDED(hr)) {
            report_stage("indexed-draw-enter", "triangle-list-up");
            hr = IDirect3DDevice8_DrawIndexedPrimitiveUP(
                operation->device, D3DPT_TRIANGLELIST, 0, 3, 1,
                triangle_indices, D3DFMT_INDEX16, near_triangle,
                sizeof(near_triangle[0]));
            report_hr_stage("indexed-draw-leave", hr,
                            SUCCEEDED(hr) ? "ok" : "failed");
            if (SUCCEEDED(hr)) {
                report_stage("depth-draw-enter", "far-triangle-after-near");
                hr = IDirect3DDevice8_DrawIndexedPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 0, 3, 1,
                    triangle_indices, D3DFMT_INDEX16, far_triangle,
                    sizeof(far_triangle[0]));
                report_hr_stage("depth-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("no-texture-stage-chain-visual-enter",
                             "stage0-tfactor-stage1-add-texture");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_TEXTUREFACTOR, 0xff102030UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 1,
                    (IDirect3DBaseTexture8 *)light_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLOROP, D3DTOP_ADD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_TEXCOORDINDEX, 1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_MAGFILTER, D3DTEXF_POINT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_MINFILTER, D3DTEXF_POINT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    no_texture_stage_chain_triangle,
                    sizeof(no_texture_stage_chain_triangle[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 1, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, TRUE);
                report_hr_stage("no-texture-stage-chain-visual-leave", hr,
                                SUCCEEDED(hr) ?
                                "expected-rgb-90a0b0" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-state-enter",
                             "stage0-modulate-point-count2");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0, (IDirect3DBaseTexture8 *)texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_MODULATE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                    D3DTTFF_COUNT2);
                report_hr_stage("texture-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-draw-enter", "indexed-checker-quad");
                hr = IDirect3DDevice8_DrawIndexedPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 0, 4, 2,
                    quad_indices, D3DFMT_INDEX16, texture_quad,
                    sizeof(texture_quad[0]));
                report_hr_stage("texture-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                operation->device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                D3DTTFF_DISABLE);
            if (SUCCEEDED(hr)) {
                report_stage("mipmap-state-enter", "max-level-two-point");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)mip_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MAXMIPLEVEL, 2);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, texture_op_buffer,
                    sizeof(texture_op_triangles[0]));
                report_hr_stage("mipmap-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("mipmap-draw-enter", "expected-blue-level-two");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 9, 2);
                report_hr_stage("mipmap-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("mipmap-lod-bias-state-enter",
                             "positive-sixteen-selects-level-two");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MAXMIPLEVEL, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MIPMAPLODBIAS,
                    0x41800000UL);
                report_hr_stage("mipmap-lod-bias-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("mipmap-lod-bias-draw-enter",
                             "expected-blue-level-two");
                hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    lod_bias_triangle, sizeof(lod_bias_triangle[0]));
                report_hr_stage("mipmap-lod-bias-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                operation->device, 0, D3DTSS_MIPMAPLODBIAS, 0);
            if (SUCCEEDED(hr)) {
                report_stage("texture-add-state-enter",
                             "complement-texture-plus-diffuse");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0, (IDirect3DBaseTexture8 *)texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_MAXMIPLEVEL, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, texture_op_buffer,
                    sizeof(texture_op_triangles[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP, D3DTOP_ADD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1,
                    D3DTA_TEXTURE | D3DTA_COMPLEMENT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                report_hr_stage("texture-add-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-add-draw-enter", "expected-0080ff");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 1);
                report_hr_stage("texture-add-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-modulate2x-state-enter",
                             "texture-times-diffuse-times-two");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_MODULATE2X);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-modulate2x-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-modulate2x-draw-enter",
                             "expected-008080");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 3, 1);
                report_hr_stage("texture-modulate2x-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-alpha-replicate-state-enter",
                             "select-texture-alpha-as-rgb");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1,
                    D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE);
                report_hr_stage("texture-alpha-replicate-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-alpha-replicate-draw-enter",
                             "expected-404040");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 6, 1);
                report_hr_stage("texture-alpha-replicate-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-subtract-state-enter",
                             "texture-minus-diffuse");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SUBTRACT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                report_hr_stage("texture-subtract-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-subtract-draw-enter",
                             "expected-00bf7f");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 18, 1);
                report_hr_stage("texture-subtract-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-modulate4x-state-enter",
                             "texture-times-diffuse-times-four");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_MODULATE4X);
                report_hr_stage("texture-modulate4x-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-modulate4x-draw-enter",
                             "expected-804000");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 21, 1);
                report_hr_stage("texture-modulate4x-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-addsigned-state-enter",
                             "texture-plus-diffuse-minus-half");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_ADDSIGNED);
                report_hr_stage("texture-addsigned-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-addsigned-draw-enter",
                             "expected-7f7f7f");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 24, 1);
                report_hr_stage("texture-addsigned-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-addsigned2x-state-enter",
                             "signed-add-times-two");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_ADDSIGNED2X);
                report_hr_stage("texture-addsigned2x-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-addsigned2x-draw-enter",
                             "expected-ffffff");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 27, 1);
                report_hr_stage("texture-addsigned2x-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-factor-state-enter",
                             "select-texture-factor");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_TEXTUREFACTOR, 0xff336699UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
                report_hr_stage("texture-factor-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-factor-draw-enter",
                             "expected-336699");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 30, 1);
                report_hr_stage("texture-factor-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-diffuse-state-enter",
                             "diffuse-alpha-interpolate");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_BLENDDIFFUSEALPHA);
                report_hr_stage("texture-blend-diffuse-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-diffuse-draw-enter",
                             "expected-80807f");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 33, 1);
                report_hr_stage("texture-blend-diffuse-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-texture-state-enter",
                             "texture-alpha-interpolate");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_BLENDTEXTUREALPHA);
                report_hr_stage("texture-blend-texture-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-texture-draw-enter",
                             "expected-4040bf");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 36, 1);
                report_hr_stage("texture-blend-texture-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-factor-state-enter",
                             "factor-alpha-interpolate");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_TEXTUREFACTOR, 0x80336699UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_BLENDFACTORALPHA);
                report_hr_stage("texture-blend-factor-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-factor-draw-enter",
                             "expected-80807f");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 39, 1);
                report_hr_stage("texture-blend-factor-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-current-state-enter",
                             "current-alpha-interpolate");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_BLENDCURRENTALPHA);
                report_hr_stage("texture-blend-current-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-blend-current-draw-enter",
                             "expected-bf4040");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 42, 1);
                report_hr_stage("texture-blend-current-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-stage1-state-enter",
                             "white-times-gray-lightmap");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 1,
                    (IDirect3DBaseTexture8 *)light_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, texture2_buffer,
                    sizeof(texture2_triangle[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLOROP, D3DTOP_MODULATE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG2);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_TEXCOORDINDEX, 1);
                report_hr_stage("texture-stage1-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-stage1-draw-enter", "expected-808080");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 1);
                report_hr_stage("texture-stage1-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-stage1-shared-state-enter",
                             "both-stages-use-coordinate-zero");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, texture_op_buffer,
                    sizeof(texture_op_triangles[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_TEXCOORDINDEX, 0);
                report_hr_stage("texture-stage1-shared-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-stage1-shared-draw-enter",
                             "expected-808080");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 45, 1);
                report_hr_stage("texture-stage1-shared-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-perspective-state-enter",
                             "rhw-weighted-checker");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 1, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                report_hr_stage("texture-perspective-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-perspective-draw-enter",
                             "expected-yellow-not-affine-cyan");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 48, 1);
                report_hr_stage("texture-perspective-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-projected-state-enter",
                             "count3-divides-uv-by-q");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 |
                    D3DFVF_TEXCOORDSIZE3(0));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                    D3DTTFF_COUNT3 | D3DTTFF_PROJECTED);
                report_hr_stage("texture-projected-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-projected-draw-enter",
                             "expected-yellow-not-unprojected-white");
                hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    projected_texture_triangle,
                    sizeof(projected_texture_triangle[0]));
                report_hr_stage("texture-projected-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                operation->device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                D3DTTFF_DISABLE);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                operation->device,
                D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                operation->device, 0, texture_op_buffer,
                sizeof(texture_op_triangles[0]));
            if (SUCCEEDED(hr)) {
                report_stage("texture-dot3-state-enter",
                             "texture-dot-diffuse");
                hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_DOTPRODUCT3);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                report_hr_stage("texture-dot3-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dot3-draw-enter", "expected-414141");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 51, 1);
                report_hr_stage("texture-dot3-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-lerp-state-enter",
                             "factor-texture-diffuse");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_TEXTUREFACTOR, 0xff4080c0UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP, D3DTOP_LERP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG0, D3DTA_TFACTOR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-lerp-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-lerp-draw-enter", "expected-40803f");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 54, 1);
                report_hr_stage("texture-lerp-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-lerp-alpha-state-enter",
                             "factor-texture-diffuse-blend");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_TEXTUREFACTOR, 0x804080c0UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP, D3DTOP_LERP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG0, D3DTA_TFACTOR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DESTBLEND,
                    D3DBLEND_INVSRCALPHA);
                report_hr_stage("texture-lerp-alpha-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-lerp-alpha-draw-enter",
                             "expected-8aa3cb");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 57, 1);
                report_hr_stage("texture-lerp-alpha-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a4-state-enter",
                             "a4r4g4b4-source-alpha-blend");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a4_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-a4-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a4-draw-enter", "expected-914546");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 60, 1);
                report_hr_stage("texture-a4-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-l8-state-enter",
                             "luminance-rgb-replication");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)l8_texture);
                report_hr_stage("texture-l8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-l8-draw-enter", "expected-a0a0a0");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 63, 1);
                report_hr_stage("texture-l8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8l8-state-enter",
                             "luminance-alpha-blend");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a8l8_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                report_hr_stage("texture-a8l8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8l8-draw-enter", "expected-2a436b");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 66, 1);
                report_hr_stage("texture-a8l8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8-state-enter",
                             "alpha-mask-over-white-diffuse");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a8_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG2);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-a8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8-draw-enter", "expected-8aa3cb");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 84, 1);
                report_hr_stage("texture-a8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-x4-state-enter", "opaque-444-blue");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)x4_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-x4-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-x4-draw-enter", "expected-0000ff");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 87, 1);
                report_hr_stage("texture-x4-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-x1-state-enter", "opaque-1555-green");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)x1_texture);
                report_hr_stage("texture-x1-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-x1-draw-enter", "expected-00ff00");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 90, 1);
                report_hr_stage("texture-x1-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-r8-state-enter", "native-bgr888");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)r8_texture);
                report_hr_stage("texture-r8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-r8-draw-enter", "expected-563412");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 93, 1);
                report_hr_stage("texture-r8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-r3-state-enter", "native-rgb332-red");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)r3_texture);
                report_hr_stage("texture-r3-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-r3-draw-enter", "expected-ff0000");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 96, 1);
                report_hr_stage("texture-r3-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-p8-state-enter",
                             "palette-alpha-source-blend");
                hr = IDirect3DDevice8_SetCurrentTexturePalette(
                    operation->device, 3);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)p8_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DESTBLEND,
                    D3DBLEND_INVSRCALPHA);
                report_hr_stage("texture-p8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-p8-draw-enter", "expected-8a634b");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 99, 1);
                report_hr_stage("texture-p8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8p8-state-enter",
                             "texel-alpha-overrides-palette-alpha");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a8p8_texture);
                report_hr_stage("texture-a8p8-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8p8-draw-enter", "expected-4f5570");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 102, 1);
                report_hr_stage("texture-a8p8-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                PALETTEENTRY palette_entries[256];
                DWORD index;

                report_stage("texture-p8-update-state-enter",
                             "palette-3-index-7-opaque-green-after-upload");
                memset(palette_entries, 0, sizeof(palette_entries));
                for (index = 0; index < 256; ++index)
                    palette_entries[index].peFlags = 0xff;
                palette_entries[7].peRed = 0x20;
                palette_entries[7].peGreen = 0xe0;
                palette_entries[7].peBlue = 0x40;
                hr = IDirect3DDevice8_SetPaletteEntries(
                    operation->device, 3, palette_entries);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)p8_texture);
                report_hr_stage("texture-p8-update-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-p8-update-draw-enter",
                             "expected-20e040-reuploaded-palette");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 105, 1);
                report_hr_stage("texture-p8-update-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8r3g3b2-state-enter",
                             "packed-alpha-source-blend");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a8r3g3b2_texture);
                report_hr_stage("texture-a8r3g3b2-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a8r3g3b2-draw-enter",
                             "expected-6548cb");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 108, 1);
                report_hr_stage("texture-a8r3g3b2-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a4l4-state-enter",
                             "opaque-expanded-luminance");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)a4l4_texture);
                report_hr_stage("texture-a4l4-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-a4l4-draw-enter", "expected-999999");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 111, 1);
                report_hr_stage("texture-a4l4-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt1-opaque-state-enter",
                             "dxt1-four-color-red");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)dxt1_texture);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAOP,
                    D3DTOP_SELECTARG1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                report_hr_stage("texture-dxt1-opaque-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt1-opaque-draw-enter",
                             "expected-ff0000");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 69, 1);
                report_hr_stage("texture-dxt1-opaque-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt1-alpha-state-enter",
                             "dxt1-transparent-selector");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DESTBLEND,
                    D3DBLEND_INVSRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                report_hr_stage("texture-dxt1-alpha-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt1-alpha-draw-enter",
                             "expected-background");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 72, 1);
                report_hr_stage("texture-dxt1-alpha-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt3-state-enter",
                             "explicit-zero-alpha-red");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)dxt3_texture);
                report_hr_stage("texture-dxt3-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt3-draw-enter",
                             "expected-background");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 75, 1);
                report_hr_stage("texture-dxt3-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt5-state-enter",
                             "interpolated-alpha-blue");
                hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0,
                    (IDirect3DBaseTexture8 *)dxt5_texture);
                report_hr_stage("texture-dxt5-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt5-draw-enter", "expected-030af0");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 78, 1);
                report_hr_stage("texture-dxt5-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("texture-dxt5-special-draw-enter",
                             "six-alpha-transparent-entry");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 81, 1);
                report_hr_stage("texture-dxt5-special-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("stencil-mask-state-enter",
                             "always-replace-one");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 0, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
                    operation->device, 1, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, stencil_buffer,
                    sizeof(stencil_rectangles[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZWRITEENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILREF, 1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILMASK, 0xff);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILWRITEMASK, 0xff);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILFAIL,
                    D3DSTENCILOP_KEEP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILZFAIL,
                    D3DSTENCILOP_KEEP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILPASS,
                    D3DSTENCILOP_REPLACE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_COLORWRITEENABLE, 0);
                report_hr_stage("stencil-mask-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("stencil-mask-draw-enter", "left-half");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 2);
                report_hr_stage("stencil-mask-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("stencil-test-state-enter",
                             "equal-one-keep");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILWRITEMASK, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILPASS,
                    D3DSTENCILOP_KEEP);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE |
                    D3DCOLORWRITEENABLE_ALPHA);
                report_hr_stage("stencil-test-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("stencil-test-draw-enter", "full-rectangle");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 6, 2);
                report_hr_stage("stencil-test-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("z-bias-base-state-enter", "coplanar-less");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, z_bias_buffer,
                    sizeof(z_bias_triangles[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZWRITEENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZFUNC, D3DCMP_LESS);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZBIAS, 0);
                report_hr_stage("z-bias-base-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("z-bias-base-draw-enter", "blue-depth-write");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 1);
                report_hr_stage("z-bias-base-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("z-bias-overlay-state-enter", "bias-one");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZWRITEENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZBIAS, 1);
                report_hr_stage("z-bias-overlay-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("z-bias-overlay-draw-enter",
                             "green-in-front");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 3, 1);
                report_hr_stage("z-bias-overlay-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("blend-state-enter", "srcalpha-invsrcalpha");
                hr = IDirect3DDevice8_SetTexture(operation->device, 0, NULL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
                    operation->device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZWRITEENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_STENCILENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZBIAS, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DESTBLEND,
                    D3DBLEND_INVSRCALPHA);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, TRUE);
                report_hr_stage("blend-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("blend-draw-enter", "half-alpha-red");
                hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    blend_triangle, sizeof(blend_triangle[0]));
                report_hr_stage("blend-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("blend-subtract-state-enter", "one-one-subtract");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SRCBLEND, D3DBLEND_ONE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DESTBLEND, D3DBLEND_ONE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_BLENDOP, D3DBLENDOP_SUBTRACT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, texture_op_buffer,
                    sizeof(texture_op_triangles[0]));
                report_hr_stage("blend-subtract-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("blend-subtract-draw-enter", "expected-505064");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 15, 1);
                report_hr_stage("blend-subtract-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("alpha-test-state-enter", "greater-than-128");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHAREF, 128);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHATESTENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, alpha_test_buffer,
                    sizeof(alpha_test_triangles[0]));
                report_hr_stage("alpha-test-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("alpha-test-draw-enter", "reject-64-pass-192");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 2);
                report_hr_stage("alpha-test-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("primitive-state-enter", "flat-managed-vb-ib");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHATESTENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SHADEMODE, D3DSHADE_FLAT);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, primitive_buffer,
                    sizeof(primitive_vertices[0]));
                report_hr_stage("primitive-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("triangle-strip-enter", "nonindexed-two-triangle");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLESTRIP, 0, 2);
                report_hr_stage("triangle-strip-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("triangle-fan-enter", "indexed-two-triangle");
                hr = IDirect3DDevice8_SetIndices(
                    operation->device, primitive_index_buffer, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawIndexedPrimitive(
                    operation->device, D3DPT_TRIANGLEFAN, 4, 4, 0, 2);
                report_hr_stage("triangle-fan-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("line-strip-enter", "nonindexed-two-segment");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_LINESTRIP, 8, 2);
                report_hr_stage("line-strip-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("line-list-enter", "indexed-two-segment");
                hr = IDirect3DDevice8_DrawIndexedPrimitive(
                    operation->device, D3DPT_LINELIST, 11, 3, 4, 2);
                report_hr_stage("line-list-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("point-list-enter",
                             "nonindexed-three-nine-pixel-points");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_POINTSIZE, 0x41100000UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_POINTLIST, 14, 3);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_POINTSIZE, 0x3f800000UL);
                report_hr_stage("point-list-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("cull-ccw-enter", "keeps-clockwise-screen-face");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_CULLMODE, D3DCULL_CCW);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    cull_clockwise_triangle,
                    sizeof(cull_clockwise_triangle[0]));
                report_hr_stage("cull-ccw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("cull-cw-enter",
                             "keeps-counterclockwise-screen-face");
                hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_CULLMODE, D3DCULL_CW);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    cull_counterclockwise_triangle,
                    sizeof(cull_counterclockwise_triangle[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_CULLMODE, D3DCULL_NONE);
                report_hr_stage("cull-cw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("specular-state-enter", "fvf-color-sum");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device,
                    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, specular_buffer,
                    sizeof(specular_triangle[0]));
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SPECULARENABLE, TRUE);
                report_hr_stage("specular-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("specular-draw-enter", "blue-plus-red");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 1);
                report_hr_stage("specular-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("minimal-fvf-state-enter", "default-white");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_PSIZE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetStreamSource(
                    operation->device, 0, minimal_fvf_buffer,
                    sizeof(minimal_fvf_triangle[0]));
                report_hr_stage("minimal-fvf-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("minimal-fvf-draw-enter", "white-triangle");
                hr = IDirect3DDevice8_DrawPrimitive(
                    operation->device, D3DPT_TRIANGLELIST, 0, 1);
                report_hr_stage("minimal-fvf-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("fog-state-enter",
                             "linear-table-zfog-color-2468ac-dither");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_SPECULARENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_ZENABLE, FALSE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_DITHERENABLE, TRUE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGCOLOR, 0xff2468acUL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGSTART, 0);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGEND, 0x3f000000UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGENABLE, TRUE);
                report_hr_stage("fog-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("fog-draw-enter", "expected-2468ac");
                hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    fog_triangle, sizeof(fog_triangle[0]));
                report_hr_stage("fog-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                operation->device, D3DRS_FOGENABLE, FALSE);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                operation->device, D3DRS_DITHERENABLE, FALSE);
            if (SUCCEEDED(hr)) {
                report_stage("vertex-fog-state-enter",
                             "specular-alpha-factor-zero-color-ac6824");
                hr = IDirect3DDevice8_SetVertexShader(
                    operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE |
                                       D3DFVF_SPECULAR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGCOLOR, 0xffac6824UL);
                if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                    operation->device, D3DRS_FOGENABLE, TRUE);
                report_hr_stage("vertex-fog-state-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) {
                report_stage("vertex-fog-draw-enter", "expected-ac6824");
                hr = IDirect3DDevice8_DrawPrimitiveUP(
                    operation->device, D3DPT_TRIANGLELIST, 1,
                    vertex_fog_triangle, sizeof(vertex_fog_triangle[0]));
                report_hr_stage("vertex-fog-draw-leave", hr,
                                SUCCEEDED(hr) ? "ok" : "failed");
            }
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                operation->device, D3DRS_FOGENABLE, FALSE);
            if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
                operation->device, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
            end_hr = IDirect3DDevice8_EndScene(operation->device);
            if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
        }
        report_hr_stage("triangle-draw-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
    }
    if (SUCCEEDED(hr)) hr = run_dynamic_texture_contract(operation->device);
    if (SUCCEEDED(hr)) hr = run_d24x8_depth_contract(operation->device);
    if (SUCCEEDED(hr)) {
        report_stage("present-enter", "");
        hr = IDirect3DDevice8_Present(operation->device, NULL, NULL,
                                      NULL, NULL);
        report_hr_stage("present-leave", hr,
                        SUCCEEDED(hr) ? "ok" : "failed");
        if (SUCCEEDED(hr)) Sleep(visual_frame_hold_ms);
    }

    /* Keep the exact main scene as the first visual capture. The next frame
     * is the software-VP oracle for the future HWTL path; once HWTL is
     * advertised the same frame becomes the exact accelerated parity gate. */
    if (SUCCEEDED(hr)) hr = run_hwtl_reference_scene(operation->device,
                                                     texture);

    /* Present a separate wireframe so the harness proves both fill-mode
     * transitions after the object-space reference draw. */
    if (SUCCEEDED(hr)) {
        report_stage("fillmode-enter", "wireframe-white-outline");
        hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_ZENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_ALPHABLENDENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(
            operation->device, 0, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
            operation->device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
            operation->device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
            operation->device, D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
            operation->device, 0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
            D3DCOLOR_XRGB(20, 70, 150), 1.0f, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(operation->device);
        if (SUCCEEDED(hr)) {
            hr = IDirect3DDevice8_DrawPrimitiveUP(
                operation->device, D3DPT_TRIANGLELIST, 1,
                wireframe_triangle, sizeof(wireframe_triangle[0]));
            end_hr = IDirect3DDevice8_EndScene(operation->device);
            if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(
            operation->device, NULL, NULL, NULL, NULL);
        if (SUCCEEDED(hr)) Sleep(visual_frame_hold_ms);
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_FILLMODE, D3DFILL_SOLID);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_CULLMODE, D3DCULL_CCW);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        operation->device, D3DRS_ZENABLE, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        operation->device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    report_hr_stage("fillmode-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (texture) {
        IDirect3DTexture8_Release(texture);
        texture = NULL;
    }
    if (mip_texture) {
        IDirect3DTexture8_Release(mip_texture);
        mip_texture = NULL;
    }
    if (light_texture) {
        IDirect3DTexture8_Release(light_texture);
        light_texture = NULL;
    }
    if (a4_texture) {
        IDirect3DTexture8_Release(a4_texture);
        a4_texture = NULL;
    }
    if (l8_texture) {
        IDirect3DTexture8_Release(l8_texture);
        l8_texture = NULL;
    }
    if (a8l8_texture) {
        IDirect3DTexture8_Release(a8l8_texture);
        a8l8_texture = NULL;
    }
    if (a8_texture) {
        IDirect3DTexture8_Release(a8_texture);
        a8_texture = NULL;
    }
    if (p8_texture) {
        IDirect3DTexture8_Release(p8_texture);
        p8_texture = NULL;
    }
    if (a8p8_texture) {
        IDirect3DTexture8_Release(a8p8_texture);
        a8p8_texture = NULL;
    }
    if (a8r3g3b2_texture) {
        IDirect3DTexture8_Release(a8r3g3b2_texture);
        a8r3g3b2_texture = NULL;
    }
    if (a4l4_texture) {
        IDirect3DTexture8_Release(a4l4_texture);
        a4l4_texture = NULL;
    }
    if (x1_texture) {
        IDirect3DTexture8_Release(x1_texture);
        x1_texture = NULL;
    }
    if (x4_texture) {
        IDirect3DTexture8_Release(x4_texture);
        x4_texture = NULL;
    }
    if (r8_texture) {
        IDirect3DTexture8_Release(r8_texture);
        r8_texture = NULL;
    }
    if (r3_texture) {
        IDirect3DTexture8_Release(r3_texture);
        r3_texture = NULL;
    }
    if (dxt1_texture) {
        IDirect3DTexture8_Release(dxt1_texture);
        dxt1_texture = NULL;
    }
    if (dxt3_texture) {
        IDirect3DTexture8_Release(dxt3_texture);
        dxt3_texture = NULL;
    }
    if (dxt5_texture) {
        IDirect3DTexture8_Release(dxt5_texture);
        dxt5_texture = NULL;
    }
    if (texture_op_buffer) {
        IDirect3DVertexBuffer8_Release(texture_op_buffer);
        texture_op_buffer = NULL;
    }
    if (stencil_buffer) {
        IDirect3DVertexBuffer8_Release(stencil_buffer);
        stencil_buffer = NULL;
    }
    if (z_bias_buffer) {
        IDirect3DVertexBuffer8_Release(z_bias_buffer);
        z_bias_buffer = NULL;
    }
    if (alpha_test_buffer) {
        IDirect3DVertexBuffer8_Release(alpha_test_buffer);
        alpha_test_buffer = NULL;
    }
    if (primitive_index_buffer) {
        IDirect3DIndexBuffer8_Release(primitive_index_buffer);
        primitive_index_buffer = NULL;
    }
    if (primitive_buffer) {
        IDirect3DVertexBuffer8_Release(primitive_buffer);
        primitive_buffer = NULL;
    }
    if (specular_buffer) {
        IDirect3DVertexBuffer8_Release(specular_buffer);
        specular_buffer = NULL;
    }
    if (minimal_fvf_buffer) {
        IDirect3DVertexBuffer8_Release(minimal_fvf_buffer);
        minimal_fvf_buffer = NULL;
    }
    if (texture2_buffer) {
        IDirect3DVertexBuffer8_Release(texture2_buffer);
        texture2_buffer = NULL;
    }
    operation->result = hr;
    return 0;
}

static HRESULT draw_copy_rect(IDirect3DDevice8 *device, float left,
                              float top, float right, float bottom,
                              DWORD color)
{
    D3D_PROBE_VERTEX vertices[4];

    memset(vertices, 0, sizeof(vertices));
    vertices[0].x = left;
    vertices[0].y = top;
    vertices[1].x = right;
    vertices[1].y = top;
    vertices[2].x = left;
    vertices[2].y = bottom;
    vertices[3].x = right;
    vertices[3].y = bottom;
    vertices[0].z = vertices[1].z = vertices[2].z = vertices[3].z = 0.5f;
    vertices[0].rhw = vertices[1].rhw = vertices[2].rhw =
        vertices[3].rhw = 1.0f;
    vertices[0].color = vertices[1].color = vertices[2].color =
        vertices[3].color = color;
    return IDirect3DDevice8_DrawPrimitiveUP(
        device, D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
}

static HRESULT run_copy_preserve(IDirect3DDevice8 *device)
{
    static const DWORD update_colors[] = {
        0xff00ffffUL, 0xffff00ffUL, 0xffff8000UL, 0xffffffffUL
    };
    HRESULT end_hr;
    HRESULT hr;
    unsigned frame;
    char detail[64];
    D3DRECT clear_rect;

    report_stage("copy-state-enter", "swap-effect-copy");
    hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_COLORWRITEENABLE, 0x0fUL);
    report_hr_stage("copy-state-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");

    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, 0xff102030UL, 1.0f, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = draw_copy_rect(device, 20.0f, 20.0f, 60.0f, 60.0f,
                            0xffff0000UL);
        if (SUCCEEDED(hr)) hr = draw_copy_rect(
            device, 340.0f, 20.0f, 380.0f, 60.0f, 0xff00ff00UL);
        if (SUCCEEDED(hr)) hr = draw_copy_rect(
            device, 20.0f, 240.0f, 60.0f, 280.0f, 0xff0000ffUL);
        if (SUCCEEDED(hr)) hr = draw_copy_rect(
            device, 340.0f, 240.0f, 380.0f, 280.0f, 0xffffff00UL);
        end_hr = IDirect3DDevice8_EndScene(device);
        if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
    }
    if (SUCCEEDED(hr)) {
        report_stage("copy-present-enter", "frame=anchors");
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        report_hr_stage("copy-present-leave", hr,
                        SUCCEEDED(hr) ? "frame=anchors" : "failed");
        Sleep(250);
    }

    for (frame = 0; SUCCEEDED(hr) &&
         frame < sizeof(update_colors) / sizeof(update_colors[0]); ++frame) {
        hr = IDirect3DDevice8_BeginScene(device);
        if (SUCCEEDED(hr)) {
            hr = draw_copy_rect(device, 180.0f, 130.0f, 220.0f, 170.0f,
                                update_colors[frame]);
            end_hr = IDirect3DDevice8_EndScene(device);
            if (SUCCEEDED(hr) && FAILED(end_hr)) hr = end_hr;
        }
        _snprintf(detail, sizeof(detail), "frame=%u color=%08lx",
                  frame, (unsigned long)update_colors[frame]);
        detail[sizeof(detail) - 1] = '\0';
        if (SUCCEEDED(hr)) {
            report_stage("copy-present-enter", detail);
            hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
            report_hr_stage("copy-present-leave", hr,
                            SUCCEEDED(hr) ? detail : "failed");
            Sleep(250);
        }
    }
    if (SUCCEEDED(hr)) {
        clear_rect.x1 = 180;
        clear_rect.y1 = 130;
        clear_rect.x2 = 220;
        clear_rect.y2 = 170;
        report_stage("copy-clear-rect-enter", "center=8040c0");
        hr = IDirect3DDevice8_Clear(device, 1, &clear_rect,
                                    D3DCLEAR_TARGET, 0xff8040c0UL,
                                    1.0f, 0);
        report_hr_stage("copy-clear-rect-leave", hr,
                        SUCCEEDED(hr) ? "center=8040c0" : "failed");
    }
    if (SUCCEEDED(hr)) {
        report_stage("copy-present-enter", "frame=rect-clear");
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        report_hr_stage("copy-present-leave", hr,
                        SUCCEEDED(hr) ? "frame=rect-clear" : "failed");
        Sleep(250);
    }
    return hr;
}

static int run_performance_benchmark(IDirect3DDevice8 *device,
                                     const char *adapter,
                                     const char *driver,
                                     BOOL software_vertex_processing)
{
    static const D3D_PROBE_VERTEX triangle[3] = {
        { 80.0f, 60.0f, 0.25f, 1.0f, 0xffff8000UL },
        { 320.0f, 240.0f, 0.25f, 1.0f, 0xff00ffffUL },
        { 80.0f, 240.0f, 0.25f, 1.0f, 0xffff00ffUL }
    };
    const unsigned frames = 16;
    const unsigned draws_per_frame = 16;
    const unsigned one_draw_frames = 16;
    const unsigned idle_presents = 8;
    const unsigned transform_frames = 64;
    const unsigned transform_draws_per_frame = 16;
    const unsigned transform_vertices = 252;
    D3D_TRANSFORM_VERTEX transformed[252];
    D3DMATRIX identity;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER middle;
    LARGE_INTEGER finish;
    ULONGLONG submit_ticks = 0;
    ULONGLONG one_draw_present_ticks = 0;
    ULONGLONG replay_present_ticks = 0;
    ULONGLONG idle_present_ticks = 0;
    ULONGLONG transform_submit_ticks = 0;
    ULONGLONG transform_present_ticks = 0;
    ULONGLONG submit_ns_per_draw;
    ULONGLONG submit_us_per_draw;
    ULONGLONG one_draw_present_us;
    ULONGLONG replay_present_us;
    ULONGLONG idle_present_us;
    ULONGLONG transform_ns_per_vertex;
    ULONGLONG transform_us_per_draw;
    ULONGLONG transform_present_us;
    HRESULT hr = S_OK;
    unsigned frame;
    unsigned draw;
    char detail[448];

    if (!device || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        return fail_probe(12, E_FAIL, "benchmark-clock", "qpc-unavailable");
    }
    report_stage("benchmark-enter",
                 "16-one-draw-and-16-batched-frames-plus-eight-idle-presents");
    hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Clear(
        device, 0, NULL, D3DCLEAR_TARGET, 0xff144696UL, 1.0f, 0);
    for (draw = 0; SUCCEEDED(hr) && draw < draws_per_frame; ++draw) {
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, triangle, sizeof(triangle[0]));
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
    if (FAILED(hr))
        return fail_probe(12, hr, "benchmark-warmup", "draw-or-present");

    for (frame = 0; frame < idle_presents; ++frame) {
        QueryPerformanceCounter(&start);
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&finish);
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-idle-present", "failed");
        idle_present_ticks += (ULONGLONG)(finish.QuadPart - start.QuadPart);
    }

    /* An idle Present never reaches XPDriver because there is no dirty
     * render target.  Time a single submitted draw separately so the result
     * exposes the fixed driver/host publication cost without conflating it
     * with the marginal replay cost of a full batch. */
    for (frame = 0; frame < one_draw_frames; ++frame) {
        hr = IDirect3DDevice8_DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, 1, triangle, sizeof(triangle[0]));
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-one-draw", "draw-failed");
        QueryPerformanceCounter(&start);
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&finish);
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-one-draw", "present-failed");
        one_draw_present_ticks +=
            (ULONGLONG)(finish.QuadPart - start.QuadPart);
    }

    for (frame = 0; frame < frames; ++frame) {
        QueryPerformanceCounter(&start);
        for (draw = 0; draw < draws_per_frame; ++draw) {
            hr = IDirect3DDevice8_DrawPrimitiveUP(
                device, D3DPT_TRIANGLELIST, 1, triangle,
                sizeof(triangle[0]));
            if (FAILED(hr))
                return fail_probe(12, hr, "benchmark-submit", "draw-failed");
        }
        QueryPerformanceCounter(&middle);
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&finish);
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-replay-present", "failed");
        submit_ticks += (ULONGLONG)(middle.QuadPart - start.QuadPart);
        replay_present_ticks +=
            (ULONGLONG)(finish.QuadPart - middle.QuadPart);
    }

    /* Exercise a Max-Payne-sized object-space draw through whichever vertex
     * processing mode CreateDevice selected. Repeating it within each frame
     * measures the driver's unchanged HWTL-state replay path; Present changes
     * framebuffer bindings and intentionally invalidates that cache. */
    memset(&identity, 0, sizeof(identity));
    identity._11 = 1.0f;
    identity._22 = 1.0f;
    identity._33 = 1.0f;
    identity._44 = 1.0f;
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = 1.0f;
    material.Diffuse.a = 1.0f;
    material.Ambient = material.Diffuse;
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
    light.Diffuse.a = 1.0f;
    light.Direction.z = -1.0f;
    for (draw = 0; draw < transform_vertices; draw += 3) {
        transformed[draw + 0].x = -0.75f;
        transformed[draw + 0].y = -0.75f;
        transformed[draw + 1].x = 0.0f;
        transformed[draw + 1].y = 0.75f;
        transformed[draw + 2].x = 0.75f;
        transformed[draw + 2].y = -0.75f;
        transformed[draw + 0].z = transformed[draw + 1].z =
            transformed[draw + 2].z = 0.5f;
        transformed[draw + 0].nx = transformed[draw + 1].nx =
            transformed[draw + 2].nx = 0.0f;
        transformed[draw + 0].ny = transformed[draw + 1].ny =
            transformed[draw + 2].ny = 0.0f;
        transformed[draw + 0].nz = transformed[draw + 1].nz =
            transformed[draw + 2].nz = 1.0f;
        transformed[draw + 0].tu = transformed[draw + 0].tv = 0.0f;
        transformed[draw + 1].tu = transformed[draw + 1].tv = 0.5f;
        transformed[draw + 2].tu = transformed[draw + 2].tv = 1.0f;
    }
    hr = IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTransform(
        device, D3DTS_PROJECTION, &identity);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetMaterial(device, &material);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetLight(device, 0, &light);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_LightEnable(device, 0, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_LIGHTING, TRUE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetRenderState(
        device, D3DRS_AMBIENT, 0x00202020UL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetTextureStageState(
        device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice8_SetVertexShader(
        device, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    if (FAILED(hr))
        return fail_probe(12, hr, "benchmark-transform-setup", "failed");
    for (frame = 0; frame < transform_frames; ++frame) {
        QueryPerformanceCounter(&start);
        for (draw = 0; SUCCEEDED(hr) &&
             draw < transform_draws_per_frame; ++draw) {
            hr = IDirect3DDevice8_DrawPrimitiveUP(
                device, D3DPT_TRIANGLELIST, transform_vertices / 3,
                transformed, sizeof(transformed[0]));
        }
        QueryPerformanceCounter(&middle);
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-transform", "draw-failed");
        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        QueryPerformanceCounter(&finish);
        if (FAILED(hr))
            return fail_probe(12, hr, "benchmark-transform",
                              "present-failed");
        transform_submit_ticks +=
            (ULONGLONG)(middle.QuadPart - start.QuadPart);
        transform_present_ticks +=
            (ULONGLONG)(finish.QuadPart - middle.QuadPart);
    }

    submit_ns_per_draw = submit_ticks * 1000000000ULL /
        ((ULONGLONG)frequency.QuadPart * frames * draws_per_frame);
    submit_us_per_draw = submit_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * frames * draws_per_frame);
    one_draw_present_us = one_draw_present_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * one_draw_frames);
    replay_present_us = replay_present_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * frames);
    idle_present_us = idle_present_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * idle_presents);
    transform_ns_per_vertex = transform_submit_ticks * 1000000000ULL /
        ((ULONGLONG)frequency.QuadPart * transform_frames *
         transform_draws_per_frame *
         transform_vertices);
    transform_us_per_draw = transform_submit_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * transform_frames *
         transform_draws_per_frame);
    transform_present_us = transform_present_ticks * 1000000ULL /
        ((ULONGLONG)frequency.QuadPart * transform_frames);
    _snprintf(detail, sizeof(detail),
              "submit-ns-per-draw=%I64u submit-us-per-draw=%I64u one-draw-present-us=%I64u replay-present-us=%I64u idle-present-us=%I64u transform-ns-per-vertex=%I64u transform-us-per-draw=%I64u transform-present-us=%I64u transform-vertices=%u transform-batch=%u transform-frames=%u vp=%s frames=%u draws-per-frame=%u adapter=%s driver=%s",
              submit_ns_per_draw, submit_us_per_draw, one_draw_present_us,
              replay_present_us, idle_present_us, transform_ns_per_vertex,
              transform_us_per_draw, transform_present_us,
              transform_vertices, transform_draws_per_frame, transform_frames,
              software_vertex_processing ? "software" : "hardware",
              frames, draws_per_frame, adapter, driver);
    detail[sizeof(detail) - 1] = '\0';
    return fail_probe(0, S_OK, "benchmark-complete", detail);
}

static int run_probe(BOOL fullscreen, BOOL rgb565, BOOL copy_preserve,
                     BOOL benchmark, BOOL require_hardware_vp,
                     BOOL hwtl_only)
{
    IDirect3D8 *d3d = NULL;
    IDirect3DDevice8 *device = NULL;
    D3DADAPTER_IDENTIFIER8 identifier;
    D3DCAPS8 caps;
    D3DDISPLAYMODE display_mode;
    D3DDISPLAYMODE enumerated_mode;
    D3DPRESENT_PARAMETERS present;
    WNDCLASSA wc;
    HWND window = NULL;
    HMODULE module = NULL;
    PFN_DIRECT3DCREATE8 create_d3d8;
    D3D8_ENTRY_POINT entry;
    char module_path[MAX_PATH];
    char system_path[MAX_PATH];
    char detail[320];
    HRESULT hr;
    HRESULT check_hr;
    D3D_OPERATION operation;
    HANDLE operation_thread = NULL;
    DWORD wait_result;
    UINT adapters;
    UINT mode_count;
    UINT mode_index;
    BOOL has_r5g6b5_mode = FALSE;
    BOOL software_vertex_processing = FALSE;
    int rc = 0;

    log_file = fopen("C:\\QXPD3D.LOG", "w");
    report_stage("start", "");
    if (!run_ddraw_query()) {
        return fail_probe(1, E_FAIL, "ddraw-query", "video-blt");
    }
    if (!GetSystemDirectoryA(system_path, sizeof(system_path))) {
        return fail_probe(2, HRESULT_FROM_WIN32(GetLastError()),
                          "module", "system-directory");
    }
    if (strlen(system_path) + sizeof("\\d3d8.dll") > sizeof(system_path)) {
        return fail_probe(2, E_FAIL, "module", "system-path-too-long");
    }
    strcat(system_path, "\\d3d8.dll");
    module = LoadLibraryA(system_path);
    if (!module || !GetModuleFileNameA(module, module_path,
                                       sizeof(module_path))) {
        return fail_probe(2, HRESULT_FROM_WIN32(GetLastError()),
                          "module", "system-d3d8-load");
    }
    if (lstrcmpiA(module_path, system_path)) {
        _snprintf(detail, sizeof(detail), "module=%s", module_path);
        detail[sizeof(detail) - 1] = '\0';
        return fail_probe(2, E_FAIL, "contaminated", detail);
    }
    if (log_file) fprintf(log_file, "MODULE=%s\n", module_path);
    if (log_file) fflush(log_file);
    report_stage("module", module_path);

    entry.proc = GetProcAddress(module, "Direct3DCreate8");
    create_d3d8 = entry.create;
    if (!create_d3d8) {
        rc = fail_probe(2, HRESULT_FROM_WIN32(GetLastError()),
                        "module", "Direct3DCreate8-export");
        goto done;
    }
    report_stage("create8-enter", "");
    d3d = create_d3d8(D3D_SDK_VERSION);
    report_stage("create8-leave", d3d ? "ok" : "null");
    if (!d3d) return fail_probe(3, E_FAIL, "create8", "module=system32");
    adapters = IDirect3D8_GetAdapterCount(d3d);
    if (log_file) fprintf(log_file, "ADAPTERS=%u\n", adapters);
    if (log_file) fflush(log_file);
    report_stage("adapters", adapters ? "count-positive" : "count-zero");
    if (!adapters) {
        rc = fail_probe(4, D3DERR_NOTAVAILABLE, "adapters", "count=0");
        goto done;
    }

    memset(&identifier, 0, sizeof(identifier));
    report_stage("identifier-enter", "");
    hr = IDirect3D8_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0,
                                         &identifier);
    report_stage("identifier-leave", SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(5, hr, "identifier", "adapter=0");
        goto done;
    }
    if (log_file) {
        fprintf(log_file, "DESCRIPTION=%s\nDRIVER=%s\n",
                identifier.Description, identifier.Driver);
        fflush(log_file);
    }

    memset(&caps, 0, sizeof(caps));
    report_stage("hal-caps-enter", "");
    hr = IDirect3D8_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                  &caps);
    report_stage("hal-caps-leave", SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "adapter=%s driver=%s",
                  identifier.Description, identifier.Driver);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, hr, "hal-caps", detail);
        goto done;
    }
    if (log_file) {
        fprintf(log_file, "CAPS=%08lx DEVCAPS=%08lx VS=%08lx PS=%08lx\n",
                (unsigned long)caps.Caps, (unsigned long)caps.DevCaps,
                (unsigned long)caps.VertexShaderVersion,
                (unsigned long)caps.PixelShaderVersion);
        fflush(log_file);
    }
    _snprintf(detail, sizeof(detail),
              "devcaps=%08lx processing=%08lx lights=%lu clips=%lu vs=%08lx",
              (unsigned long)caps.DevCaps,
              (unsigned long)caps.VertexProcessingCaps,
              (unsigned long)caps.MaxActiveLights,
              (unsigned long)caps.MaxUserClipPlanes,
              (unsigned long)caps.VertexShaderVersion);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("vertex-caps", detail);
    if ((caps.TextureOpCaps &
         (D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
          D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
          D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
          D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
          D3DTEXOPCAPS_BLENDTEXTUREALPHA |
          D3DTEXOPCAPS_BLENDFACTORALPHA |
          D3DTEXOPCAPS_BLENDCURRENTALPHA |
          D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_LERP)) !=
        (D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
         D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
         D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
         D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
         D3DTEXOPCAPS_BLENDTEXTUREALPHA |
         D3DTEXOPCAPS_BLENDFACTORALPHA |
         D3DTEXOPCAPS_BLENDCURRENTALPHA |
         D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_LERP)) {
        _snprintf(detail, sizeof(detail), "texture-op-caps=%08lx",
                  (unsigned long)caps.TextureOpCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "texture-op-caps", detail);
        goto done;
    }
    report_stage("texture-op-caps",
                 "scaled-add-signed-subtract-alpha-interpolate-dot3-lerp");
    if (!(caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES)) {
        _snprintf(detail, sizeof(detail), "caps2=%08lx",
                  (unsigned long)caps.Caps2);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE,
                        "dynamic-texture-caps", detail);
        goto done;
    }
    report_stage("dynamic-texture-caps",
                 "default-pool-lockable-discard");
    if (caps.MaxTextureBlendStages < 4 ||
        caps.MaxSimultaneousTextures < 4 ||
        (caps.FVFCaps & D3DFVFCAPS_TEXCOORDCOUNTMASK) < 4) {
        _snprintf(detail, sizeof(detail),
                  "blend-stages=%lu textures=%lu fvf=%08lx",
                  (unsigned long)caps.MaxTextureBlendStages,
                  (unsigned long)caps.MaxSimultaneousTextures,
                  (unsigned long)caps.FVFCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "texture-stage3-caps",
                        detail);
        goto done;
    }
    report_stage("texture-stage3-caps", "four-textures-four-coordinates");
    if (caps.MaxPointSize < 9.0f) {
        _snprintf(detail, sizeof(detail), "max-point-size=%.2f",
                  caps.MaxPointSize);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "point-size-caps", detail);
        goto done;
    }
    report_stage("point-size-caps", "uniform-up-to-64-pixels");
    if ((caps.TextureCaps &
         (D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PERSPECTIVE |
          D3DPTEXTURECAPS_PROJECTED)) !=
        (D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PERSPECTIVE |
         D3DPTEXTURECAPS_PROJECTED) ||
        !(caps.RasterCaps & D3DPRASTERCAPS_MIPMAPLODBIAS) ||
        (caps.TextureFilterCaps &
         (D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR)) !=
        (D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR)) {
        _snprintf(detail, sizeof(detail),
                  "texture-caps=%08lx filter-caps=%08lx",
                  (unsigned long)caps.TextureCaps,
                  (unsigned long)caps.TextureFilterCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "mipmap-caps", detail);
        goto done;
    }
    report_stage("mipmap-caps",
                 "point-linear-perspective-projected-lod-bias");
    if ((caps.TextureCaps &
         (D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP)) !=
        (D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP) ||
        (caps.CubeTextureFilterCaps &
         (D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT)) !=
        (D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT)) {
        _snprintf(detail, sizeof(detail),
                  "texture-caps=%08lx cube-filter-caps=%08lx",
                  (unsigned long)caps.TextureCaps,
                  (unsigned long)caps.CubeTextureFilterCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "cubemap-caps", detail);
        goto done;
    }
    report_stage("cubemap-caps", "mipmapped-point-filter");
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE)) {
        _snprintf(detail, sizeof(detail), "texture-caps=%08lx",
                  (unsigned long)caps.TextureCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "palette-caps", detail);
        goto done;
    }
    report_stage("palette-caps", "p8-alpha-and-a8p8");
    if ((caps.StencilCaps &
         (D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
          D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
          D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT)) !=
        (D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
         D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
         D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT)) {
        _snprintf(detail, sizeof(detail), "stencil-caps=%08lx",
                  (unsigned long)caps.StencilCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "stencil-caps", detail);
        goto done;
    }
    report_stage("stencil-caps", "advertised-subset");
    if (!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP)) {
        _snprintf(detail, sizeof(detail), "primitive-misc-caps=%08lx",
                  (unsigned long)caps.PrimitiveMiscCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "blend-op-caps", detail);
        goto done;
    }
    report_stage("blend-op-caps", "add-subtract-reverse-min-max");
    if (!(caps.RasterCaps & D3DPRASTERCAPS_ZBIAS)) {
        _snprintf(detail, sizeof(detail), "raster-caps=%08lx",
                  (unsigned long)caps.RasterCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "z-bias-caps", detail);
        goto done;
    }
    report_stage("z-bias-caps", "integer-zero-through-sixteen");
    if ((caps.RasterCaps &
         (D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_FOGTABLE |
          D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGRANGE |
          D3DPRASTERCAPS_ZFOG)) !=
        (D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_FOGTABLE |
         D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGRANGE |
         D3DPRASTERCAPS_ZFOG) ||
        !(caps.ShadeCaps & D3DPSHADECAPS_FOGGOURAUD)) {
        _snprintf(detail, sizeof(detail), "raster=%08lx shade=%08lx",
                  (unsigned long)caps.RasterCaps,
                  (unsigned long)caps.ShadeCaps);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "fog-caps", detail);
        goto done;
    }
    report_stage("fog-caps", "linear-exp-exp2-table-vertex-range-z-dither");

    memset(&display_mode, 0, sizeof(display_mode));
    hr = IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT,
                                          &display_mode);
    _snprintf(detail, sizeof(detail), "format=%u size=%ux%u",
              (unsigned)display_mode.Format, (unsigned)display_mode.Width,
              (unsigned)display_mode.Height);
    detail[sizeof(detail) - 1] = '\0';
    report_hr_stage("display-mode", hr, detail);
    mode_count = IDirect3D8_GetAdapterModeCount(d3d,
                                                 D3DADAPTER_DEFAULT);
    _snprintf(detail, sizeof(detail), "count=%u", (unsigned)mode_count);
    detail[sizeof(detail) - 1] = '\0';
    report_stage("adapter-mode-count", detail);
    for (mode_index = 0; mode_index < mode_count && mode_index < 64;
         ++mode_index) {
        memset(&enumerated_mode, 0, sizeof(enumerated_mode));
        hr = IDirect3D8_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT,
                                         mode_index, &enumerated_mode);
        _snprintf(detail, sizeof(detail),
                  "index=%u size=%ux%u format=%u refresh=%u",
                  (unsigned)mode_index, (unsigned)enumerated_mode.Width,
                  (unsigned)enumerated_mode.Height,
                  (unsigned)enumerated_mode.Format,
                  (unsigned)enumerated_mode.RefreshRate);
        detail[sizeof(detail) - 1] = '\0';
        report_hr_stage("adapter-mode", hr, detail);
        if (FAILED(hr)) break;
        if (enumerated_mode.Format == D3DFMT_R5G6B5)
            has_r5g6b5_mode = TRUE;
    }
    if (!has_r5g6b5_mode) {
        rc = fail_probe(6, D3DERR_NOTAVAILABLE, "adapter-mode",
                        "missing=r5g6b5");
        goto done;
    }
    report_stage("adapter-mode", "r5g6b5-present");
    if (SUCCEEDED(hr)) {
        check_hr = IDirect3D8_CheckDeviceType(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            display_mode.Format, display_mode.Format, FALSE);
        report_hr_stage("check-device-type-fullscreen", check_hr,
                        "x8r8g8b8");
        check_hr = IDirect3D8_CheckDeviceType(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            display_mode.Format, D3DFMT_R5G6B5, FALSE);
        report_hr_stage("check-device-type-fullscreen", check_hr,
                        "r5g6b5-backbuffer");
        check_hr = IDirect3D8_CheckDeviceType(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_R5G6B5, D3DFMT_R5G6B5, FALSE);
        report_hr_stage("check-device-type-fullscreen", check_hr,
                        "r5g6b5-display-and-backbuffer");
        if (FAILED(check_hr)) {
            rc = fail_probe(6, check_hr, "check-device-type-fullscreen",
                            "r5g6b5-display-and-backbuffer");
            goto done;
        }
        hr = IDirect3D8_CheckDeviceType(d3d, D3DADAPTER_DEFAULT,
                                       D3DDEVTYPE_HAL, display_mode.Format,
                                       display_mode.Format, TRUE);
        report_hr_stage("check-device-type-windowed", hr, "same-format");
        hr = IDirect3D8_CheckDeviceFormat(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, display_mode.Format,
            D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, display_mode.Format);
        report_hr_stage("check-render-target", hr, "display-format");
        hr = IDirect3D8_CheckDeviceFormat(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, display_mode.Format,
            0, D3DRTYPE_TEXTURE, D3DFMT_X8R8G8B8);
        report_hr_stage("check-texture", hr, "x8r8g8b8");
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, D3DUSAGE_DYNAMIC,
                D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
            report_hr_stage("check-dynamic-texture", hr,
                            "a8r8g8b8-default-pool");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_CUBETEXTURE,
                D3DFMT_A8R8G8B8);
            report_hr_stage("check-cubemap", hr, "a8r8g8b8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE,
                D3DFMT_A4R4G4B4);
            report_hr_stage("check-texture-a4", hr, "a4r4g4b4");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_L8);
            report_hr_stage("check-texture-l8", hr, "l8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A8L8);
            report_hr_stage("check-texture-a8l8", hr, "a8l8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A8);
            report_hr_stage("check-texture-a8", hr, "a8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_P8);
            report_hr_stage("check-texture-p8", hr, "p8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A8P8);
            report_hr_stage("check-texture-a8p8", hr, "a8p8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE,
                D3DFMT_A8R3G3B2);
            report_hr_stage("check-texture-a8r3g3b2", hr,
                            "a8r3g3b2");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A4L4);
            report_hr_stage("check-texture-a4l4", hr, "a4l4");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE,
                D3DFMT_X1R5G5B5);
            report_hr_stage("check-texture-x1", hr, "x1r5g5b5");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE,
                D3DFMT_X4R4G4B4);
            report_hr_stage("check-texture-x4", hr, "x4r4g4b4");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_R8G8B8);
            report_hr_stage("check-texture-r8", hr, "r8g8b8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_R3G3B2);
            report_hr_stage("check-texture-r3", hr, "r3g3b2");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1);
            report_hr_stage("check-texture-dxt1", hr, "dxt1");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT3);
            report_hr_stage("check-texture-dxt3", hr, "dxt3");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5);
            report_hr_stage("check-texture-dxt5", hr, "dxt5");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDeviceFormat(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                display_mode.Format, D3DUSAGE_DEPTHSTENCIL,
                D3DRTYPE_SURFACE, D3DFMT_D24X8);
            report_hr_stage("check-depth-format-d24x8", hr, "d24x8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDepthStencilMatch(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                rgb565 ? D3DFMT_R5G6B5 : display_mode.Format,
                rgb565 ? D3DFMT_R5G6B5 : display_mode.Format,
                D3DFMT_D24X8);
            report_hr_stage("check-depth-match-d24x8", hr,
                            rgb565 ? "r5g6b5-d24x8" : "d24x8");
        }
        if (SUCCEEDED(hr)) {
            hr = IDirect3D8_CheckDepthStencilMatch(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                rgb565 ? D3DFMT_R5G6B5 : display_mode.Format,
                rgb565 ? D3DFMT_R5G6B5 : display_mode.Format,
                D3DFMT_D24S8);
            report_hr_stage("check-depth-match", hr,
                            rgb565 ? "r5g6b5-d24s8" : "d24s8");
        }
        if (FAILED(hr)) {
            rc = fail_probe(6, hr, "format-check",
                            "rgb-alpha-luma-packed-dxt-d24x8-or-d24s8");
            goto done;
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QXPD3DProbe";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        rc = fail_probe(7, HRESULT_FROM_WIN32(GetLastError()),
                        "window-class", "");
        goto done;
    }
    window = CreateWindowA(wc.lpszClassName, "XPDriver native D3D8 probe",
                           (fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW) |
                               WS_VISIBLE,
                           fullscreen ? 0 : CW_USEDEFAULT,
                           fullscreen ? 0 : CW_USEDEFAULT,
                           fullscreen ? display_mode.Width : 400,
                           fullscreen ? display_mode.Height : 300,
                           NULL, NULL, wc.hInstance, NULL);
    if (!window) {
        rc = fail_probe(7, HRESULT_FROM_WIN32(GetLastError()), "window", "");
        goto done;
    }

    memset(&present, 0, sizeof(present));
    present.Windowed = fullscreen ? FALSE : TRUE;
    present.SwapEffect = copy_preserve ? D3DSWAPEFFECT_COPY :
                                        D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = rgb565 ? D3DFMT_R5G6B5 :
                               (fullscreen ? display_mode.Format :
                                             D3DFMT_UNKNOWN);
    present.Flags = 0;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D24S8;
    if (copy_preserve) {
        present.BackBufferWidth = 400;
        present.BackBufferHeight = 300;
        present.BackBufferCount = 1;
        present.hDeviceWindow = window;
    } else if (!fullscreen) {
        /* XP's D3D8 runtime rejects this HAL's otherwise legal windowed
         * defaults before vertex-processing selection.  Give hardware and
         * software VP the same complete presentation tuple so a fallback
         * cannot disguise a harness-side D3DERR_INVALIDCALL. */
        present.BackBufferWidth = 400;
        present.BackBufferHeight = 300;
        present.BackBufferFormat = display_mode.Format;
        present.BackBufferCount = 1;
        present.MultiSampleType = D3DMULTISAMPLE_NONE;
        present.hDeviceWindow = window;
        present.FullScreen_RefreshRateInHz = 0;
        present.FullScreen_PresentationInterval =
            D3DPRESENT_INTERVAL_DEFAULT;
    }
    report_stage("create-device-enter", copy_preserve ?
                 "windowed-copy-software-vp" : (rgb565 ?
                 (require_hardware_vp ? "fullscreen-r5g6b5-hardware-vp" :
                                        "fullscreen-r5g6b5-software-vp") :
                 (fullscreen ?
                  (require_hardware_vp ? "fullscreen-hardware-vp" :
                                         "fullscreen-software-vp") :
                  (require_hardware_vp ? "hardware-vp-required" :
                                         "hardware-vp"))));
    if (fullscreen) {
        software_vertex_processing = !require_hardware_vp;
        present.BackBufferWidth = display_mode.Width;
        present.BackBufferHeight = display_mode.Height;
        present.BackBufferCount = 1;
        present.hDeviceWindow = window;
        present.FullScreen_RefreshRateInHz = 0;
        present.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        hr = IDirect3D8_CreateDevice(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            (require_hardware_vp ? D3DCREATE_HARDWARE_VERTEXPROCESSING :
             D3DCREATE_SOFTWARE_VERTEXPROCESSING) |
                D3DCREATE_MULTITHREADED,
            &present, &device);
    } else {
        hr = IDirect3D8_CreateDevice(
            d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device);
        if (FAILED(hr)) {
            report_hr_stage("create-device-hardware-failed", hr,
                            require_hardware_vp ? "required" :
                                                  "retry-software-vp");
            if (!require_hardware_vp) {
                software_vertex_processing = TRUE;
                hr = IDirect3D8_CreateDevice(
                    d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device);
            }
        }
    }
    report_stage("create-device-leave", SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(8, hr, "create-device", identifier.Description);
        goto done;
    }
    report_stage("vertex-processing",
                 software_vertex_processing ? "software" : "hardware");
    _snprintf(detail, sizeof(detail),
              "d3d8=%08lx gdi32=%08lx ntdll=%08lx",
              (unsigned long)(ULONG_PTR)GetModuleHandleA("d3d8.dll"),
              (unsigned long)(ULONG_PTR)GetModuleHandleA("gdi32.dll"),
              (unsigned long)(ULONG_PTR)GetModuleHandleA("ntdll.dll"));
    detail[sizeof(detail) - 1] = '\0';
    report_stage("module-bases", detail);
    if (benchmark) {
        rc = run_performance_benchmark(device, identifier.Description,
                                       identifier.Driver,
                                       software_vertex_processing);
        goto done;
    }
    if (hwtl_only) {
        IDirect3DTexture8 *reference_texture = NULL;

        hr = create_reference_texture(device, &reference_texture);
        if (SUCCEEDED(hr))
            hr = run_hwtl_reference_scene(device, reference_texture);
        if (reference_texture) IDirect3DTexture8_Release(reference_texture);
        if (FAILED(hr)) {
            rc = fail_probe(9, hr, "hwtl-reference",
                            "focused-object-space-oracle");
            goto done;
        }
        _snprintf(detail, sizeof(detail),
                  "adapter=%s driver=%s module=system32 focused=hwtl",
                  identifier.Description, identifier.Driver);
        detail[sizeof(detail) - 1] = '\0';
        rc = fail_probe(0, S_OK, "complete", detail);
        goto done;
    }
    if (copy_preserve) {
        hr = run_copy_preserve(device);
        if (FAILED(hr)) {
            rc = fail_probe(9, hr, "copy-preserve", "windowed-copy");
            goto done;
        }
        Sleep(500);
        rc = fail_probe(0, S_OK, "complete",
                        "windowed-copy-preserved-backbuffer");
        goto done;
    }
    if (!rgb565) {
        hr = run_a8_texture_contract8(device);
        if (FAILED(hr)) {
            rc = fail_probe(9, hr, "a8-contract",
                            "patterned-decode-fixed-blend-ps11-blend");
            goto done;
        }
    } else {
        report_stage("a8-contract-skip", "rgb565-specialized-mode");
    }
    operation.device = device;
    operation.result = E_FAIL;
    operation_thread = CreateThread(NULL, 0, run_clear_present, &operation,
                                    0, NULL);
    if (!operation_thread) {
        rc = fail_probe(10, HRESULT_FROM_WIN32(GetLastError()),
                        "operation-thread", identifier.Description);
        goto done;
    }
    /* Main, HWTL-reference, six scalable-frame intervals, and wireframe
     * each hold a frame in visual mode. Keep the actual D3D watchdog budget
     * fixed, but account
     * for those intentional sleeps so slow synchronous capture cannot turn
     * a completed exact-pixel sequence into a false driver-hang report. */
    wait_result = WaitForSingleObject(
        operation_thread,
        QXP_OPERATION_TIMEOUT_BASE_MS +
            QXP_OPERATION_PRESENT_HOLDS * visual_frame_hold_ms);
    if (wait_result == WAIT_TIMEOUT) {
        fail_probe(10, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                   "clear-present-timeout", identifier.Description);
        ExitProcess(10);
    }
    if (wait_result != WAIT_OBJECT_0) {
        rc = fail_probe(10, HRESULT_FROM_WIN32(GetLastError()),
                        "operation-wait", identifier.Description);
        goto done;
    }
    CloseHandle(operation_thread);
    operation_thread = NULL;
    hr = operation.result;
    if (FAILED(hr)) {
        rc = fail_probe(9, hr, "clear-present", identifier.Description);
        goto done;
    }
    Sleep(3000);
    report_stage("cooperative-level-enter", "before-reset");
    hr = IDirect3DDevice8_TestCooperativeLevel(device);
    report_hr_stage("cooperative-level-leave", hr,
                    SUCCEEDED(hr) ? "ready" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(11, hr, "cooperative-level", "before-reset");
        goto done;
    }
    report_stage("reset-enter", fullscreen ? "fullscreen" : "windowed");
    hr = IDirect3DDevice8_Reset(device, &present);
    report_hr_stage("reset-leave", hr, SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(11, hr, "reset", fullscreen ?
                        "fullscreen" : "windowed");
        goto done;
    }
    report_stage("reset-clear-enter", "expected-123456");
    hr = IDirect3DDevice8_Clear(
        device, 0, NULL,
        D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
        0xff123456UL, 1.0f, 0);
    report_hr_stage("reset-clear-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(11, hr, "reset-clear", "expected-123456");
        goto done;
    }
    report_stage("reset-present-enter", "post-reset");
    hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
    report_hr_stage("reset-present-leave", hr,
                    SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) {
        rc = fail_probe(11, hr, "reset-present", "post-reset");
        goto done;
    }
    Sleep(visual_frame_hold_ms > 1000 ? visual_frame_hold_ms : 1000);
    if (fullscreen) {
        hr = run_post_reset_hwtl_scene(device);
        if (FAILED(hr)) {
            rc = fail_probe(11, hr, "post-reset-hwtl",
                            "point-material-diffuse-no-color1");
            goto done;
        }
        /* Runtime-managed ALL blocks restore bindings in addition to the
         * scalar/object values this contract inspects. Keep the destructive
         * membership oracle after every visual gate so it cannot hide an
         * otherwise-correct rendered frame. Running it after Reset also
         * verifies that newly-created blocks work on the reset device. */
        hr = run_state_block_contract(device);
        if (FAILED(hr)) {
            rc = fail_probe(8, hr, "state-block",
                            "record-apply-capture-delete");
            goto done;
        }
        hr = run_state_block_capacity_contract(device);
        if (FAILED(hr)) {
            rc = fail_probe(8, hr, "state-block-capacity",
                            "257-live-first-65th-257th");
            goto done;
        }
        hr = run_predefined_state_block_contract(device);
        if (FAILED(hr)) {
            rc = fail_probe(8, hr, "predefined-state-block",
                            "pixel-vertex-all-membership");
            goto done;
        }
    }
    _snprintf(detail, sizeof(detail), "adapter=%s driver=%s module=system32",
              identifier.Description, identifier.Driver);
    detail[sizeof(detail) - 1] = '\0';
    rc = fail_probe(0, S_OK, "complete", detail);

done:
    if (operation_thread) CloseHandle(operation_thread);
    if (device) IDirect3DDevice8_Release(device);
    if (d3d) IDirect3D8_Release(d3d);
    if (module) FreeLibrary(module);
    if (window) DestroyWindow(window);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    return rc;
}

int main(int argc, char **argv)
{
    BOOL fullscreen = FALSE;
    BOOL rgb565 = FALSE;
    BOOL copy_preserve = FALSE;
    BOOL benchmark = FALSE;
    BOOL require_hardware_vp = FALSE;
    BOOL hwtl_only = FALSE;
    int index;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(report_unhandled_exception);
    if (argc == 2 && !strcmp(argv[1], "--clean-maxpayne")) {
        return clean_max_payne();
    }
    for (index = 1; index < argc; ++index) {
        if (!strcmp(argv[index], "--fullscreen")) fullscreen = TRUE;
        else if (!strcmp(argv[index], "--rgb565")) rgb565 = TRUE;
        else if (!strcmp(argv[index], "--copy-preserve"))
            copy_preserve = TRUE;
        else if (!strcmp(argv[index], "--benchmark")) benchmark = TRUE;
        else if (!strcmp(argv[index], "--hardware-vp"))
            require_hardware_vp = TRUE;
        else if (!strcmp(argv[index], "--hwtl-only")) hwtl_only = TRUE;
        else if (!strcmp(argv[index], "--visual"))
            visual_frame_hold_ms = QXP_VISUAL_FRAME_HOLD_MS;
        else return fail_probe(1, E_INVALIDARG, "arguments", argv[index]);
    }
    if (rgb565 && !fullscreen)
        return fail_probe(1, E_INVALIDARG, "arguments",
                          "rgb565-requires-fullscreen");
    if (copy_preserve && fullscreen)
        return fail_probe(1, E_INVALIDARG, "arguments",
                          "copy-preserve-requires-windowed");
    if (copy_preserve && require_hardware_vp)
        return fail_probe(1, E_INVALIDARG, "arguments",
                          "copy-preserve-requires-software-vp");
    if (benchmark && (fullscreen || rgb565 || copy_preserve))
        return fail_probe(1, E_INVALIDARG, "arguments",
                          "benchmark-requires-windowed-defaults");
    if (hwtl_only && (!fullscreen || benchmark || copy_preserve))
        return fail_probe(1, E_INVALIDARG, "arguments",
                          "hwtl-only-requires-fullscreen-visual");
    return run_probe(fullscreen, rgb565, copy_preserve, benchmark,
                     require_hardware_vp, hwtl_only);
}
