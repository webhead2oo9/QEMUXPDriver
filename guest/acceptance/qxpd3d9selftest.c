/* Executes the driver's guarded draw-source transaction against a real
 * DirectDraw-backed vertex buffer and checks what the driver observed.
 *
 * The SM2 draw path cannot run while DDI-v4/SM2 capabilities are declined,
 * because the stock runtime never creates the shader objects a capture
 * eligible draw requires.  This probe reaches the same production code out of
 * band through DrvEscape, so the lifetime gate, the surface resolver, and the
 * resolved base address are exercised in a live guest instead of only in
 * portable tests. */
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"
#include "../xpdm/qxp_escape.h"

typedef IDirect3D9 *(WINAPI *PFN_DIRECT3DCREATE9)(UINT sdk_version);

typedef union d3d9_entry_point {
    FARPROC proc;
    PFN_DIRECT3DCREATE9 create;
} D3D9_ENTRY_POINT;

#define QXP_SELFTEST_VERTEX_BYTES 4096U
#define QXP_SELFTEST_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* House convention: progress lines carry a STAGE prefix and the single
 * terminal line carries a RESULT prefix the harness matches to end the run. */
static void report(const char *stage, const char *detail)
{
    char message[4096];
    int length;

    /* _snprintf returns a negative value when the result does not fit.  This
     * used to return early on that, which silently discarded the whole line:
     * a stage whose detail outgrew the buffer vanished from the log while its
     * pass/fail bit still appeared in the summary, so a failing stage could
     * report no diagnostic at all.  That cost a full acceptance cycle to
     * rediscover.  Emit the truncated text instead, marked so truncation is
     * never mistaken for a complete record. */
    length = _snprintf(message, sizeof(message),
                       "QXP_D3D9_SELFTEST_STAGE stage=%s %s",
                       stage, detail ? detail : "");
    if (length < 0 || length >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
        qxp_report_serial(message);
        qxp_report_serial("QXP_D3D9_SELFTEST_STAGE stage=report TRUNCATED");
        return;
    }
    message[length] = '\0';
    qxp_report_serial(message);
}

/* Same FNV-1a the driver applies to the bytes it reads at the resolved base. */
static unsigned long selftest_hash(const unsigned char *data, unsigned long bytes)
{
    unsigned long hash = 2166136261UL;
    unsigned long index;

    for (index = 0; index < bytes; ++index) {
        hash ^= data[index];
        hash *= 16777619UL;
    }
    return hash;
}

static void selftest_fill(unsigned char *pattern, unsigned long bytes)
{
    unsigned long index;

    for (index = 0; index < bytes; ++index)
        pattern[index] = (unsigned char)((index * 31U + 7U) & 0xffU);
}

/* Compressed-surface contract probe.
 *
 * Every other stage here reaches the driver through DrvEscape, so the DDI
 * surface-creation and Lock contract that an ordinary game uses is the one
 * thing this suite never proves.  A game fills a compressed surface by
 * locking it and writing block rows at the pitch the driver reported, so a
 * pitch that disagrees with the driver's own block geometry corrupts guest
 * memory rather than merely drawing the wrong pixels.
 *
 * Declining the format outright is a legal answer here, because the driver
 * deliberately advertises no compressed formats.  Accepting the surface and
 * then reporting an inconsistent pitch, losing the written blocks, or
 * decoding them to the wrong colour is not. */
#define QXP_SELFTEST_DXT_BLOCKS 2U
#define QXP_SELFTEST_DXT_DIM (QXP_SELFTEST_DXT_BLOCKS * 4U)
#define QXP_SELFTEST_DXT_TARGET 64U
#define QXP_SELFTEST_DXT_MAX_BYTES (QXP_SELFTEST_DXT_BLOCKS * \
                                    QXP_SELFTEST_DXT_BLOCKS * 16U)
#define QXP_SELFTEST_OK 0
#define QXP_SELFTEST_DECLINED 1
#define QXP_SELFTEST_BAD 2

typedef struct selftest_dxt_vertex {
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
} SELFTEST_DXT_VERTEX;

/* Endpoints sit at the extremes of 5:6:5 so any correct expansion to eight
 * bits lands exactly on 0 or 255 and the comparison needs no tolerance. */
static const unsigned short selftest_dxt_colours[4] = {
    0xf800U, 0x07e0U, 0x001fU, 0xffffU
};
static const unsigned long selftest_dxt_expected[4] = {
    0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL, 0x00ffffffUL
};
/* Quadrant centres of the render target, one per block. */
static const unsigned long selftest_dxt_points[4][2] = {
    {16UL, 16UL}, {48UL, 16UL}, {16UL, 48UL}, {48UL, 48UL}
};

static const char *selftest_state_name(int state)
{
    if (state == QXP_SELFTEST_OK) return "ok";
    if (state == QXP_SELFTEST_DECLINED) return "declined";
    return "BAD";
}

/* Identical endpoints decode to that one colour for index 0 in both the
 * four-colour and the three-colour form, so the block needs no mode care. */
static void selftest_dxt_colour_block(unsigned char *block,
                                      unsigned short colour)
{
    block[0] = (unsigned char)(colour & 0xffU);
    block[1] = (unsigned char)((colour >> 8) & 0xffU);
    block[2] = block[0];
    block[3] = block[1];
    block[4] = 0;
    block[5] = 0;
    block[6] = 0;
    block[7] = 0;
}

static void selftest_dxt_build_blocks(unsigned char *blocks,
                                      unsigned long block_bytes)
{
    unsigned long index;

    memset(blocks, 0, QXP_SELFTEST_DXT_MAX_BYTES);
    for (index = 0; index < 4; ++index) {
        unsigned char *block = blocks + index * block_bytes;

        if (block_bytes == 16UL) {
            /* DXT5 carries an alpha block first; both endpoints opaque and
             * every index zero keeps the colour comparison alpha-free. */
            block[0] = 0xffU;
            block[1] = 0xffU;
            selftest_dxt_colour_block(block + 8, selftest_dxt_colours[index]);
        } else {
            selftest_dxt_colour_block(block, selftest_dxt_colours[index]);
        }
    }
}

static int selftest_dxt_stage(IDirect3D9 *d3d, IDirect3DDevice9 *device,
                              D3DFORMAT adapter_format, D3DFORMAT format,
                              const char *name)
{
    IDirect3DSurface9 *saved_target = NULL;
    IDirect3DSurface9 *target = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DTexture9 *texture = NULL;
    SELFTEST_DXT_VERTEX quad[4];
    D3DLOCKED_RECT locked;
    unsigned char blocks[QXP_SELFTEST_DXT_MAX_BYTES];
    unsigned long observed[4];
    unsigned long block_bytes;
    unsigned long row_bytes;
    unsigned long expected_pitch;
    unsigned long reported_pitch = 0;
    unsigned long row;
    unsigned long index;
    HRESULT check_hr;
    HRESULT create_hr;
    HRESULT lock_hr = S_OK;
    HRESULT target_hr = S_OK;
    HRESULT draw_hr = S_OK;
    HRESULT copy_hr = S_OK;
    int readback_match = 0;
    int colours_match = 0;
    int sampled = 0;
    int pitch_ok = 0;
    int state = QXP_SELFTEST_BAD;
    char detail[1024];

    memset(observed, 0, sizeof(observed));
    memset(&locked, 0, sizeof(locked));
    block_bytes = (format == D3DFMT_DXT1) ? 8UL : 16UL;
    row_bytes = QXP_SELFTEST_DXT_BLOCKS * block_bytes;
    expected_pitch = row_bytes;
    selftest_dxt_build_blocks(blocks, block_bytes);

    check_hr = IDirect3D9_CheckDeviceFormat(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, adapter_format, 0,
        D3DRTYPE_TEXTURE, format);
    create_hr = IDirect3DDevice9_CreateTexture(
        device, QXP_SELFTEST_DXT_DIM, QXP_SELFTEST_DXT_DIM, 1, 0, format,
        D3DPOOL_MANAGED, &texture, NULL);
    if (FAILED(create_hr) || !texture) {
        state = QXP_SELFTEST_DECLINED;
        goto finish;
    }

    lock_hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    if (FAILED(lock_hr) || !locked.pBits) goto finish;
    reported_pitch = (unsigned long)locked.Pitch;
    pitch_ok = reported_pitch == expected_pitch;
    for (row = 0; row < QXP_SELFTEST_DXT_BLOCKS; ++row) {
        memcpy((unsigned char *)locked.pBits + row * reported_pitch,
               blocks + row * row_bytes, row_bytes);
    }
    (void)IDirect3DTexture9_UnlockRect(texture, 0);

    /* The written blocks must survive the round trip: a short allocation, or
     * a pitch the surface cannot actually hold, loses them here. */
    if (SUCCEEDED(IDirect3DTexture9_LockRect(texture, 0, &locked, NULL,
                                             D3DLOCK_READONLY))) {
        readback_match = 1;
        for (row = 0; row < QXP_SELFTEST_DXT_BLOCKS; ++row) {
            if (memcmp((const unsigned char *)locked.pBits +
                           row * reported_pitch,
                       blocks + row * row_bytes, row_bytes) != 0)
                readback_match = 0;
        }
        (void)IDirect3DTexture9_UnlockRect(texture, 0);
    }

    target_hr = IDirect3DDevice9_CreateRenderTarget(
        device, QXP_SELFTEST_DXT_TARGET, QXP_SELFTEST_DXT_TARGET,
        D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, NULL);
    if (SUCCEEDED(target_hr)) {
        target_hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
            device, QXP_SELFTEST_DXT_TARGET, QXP_SELFTEST_DXT_TARGET,
            D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    }
    if (SUCCEEDED(target_hr))
        target_hr = IDirect3DDevice9_GetRenderTarget(device, 0, &saved_target);
    if (SUCCEEDED(target_hr))
        target_hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
    if (SUCCEEDED(target_hr)) {
        for (index = 0; index < 4; ++index) {
            quad[index].x = ((index & 1U) ?
                             (float)QXP_SELFTEST_DXT_TARGET : 0.0f) - 0.5f;
            quad[index].y = ((index & 2U) ?
                             (float)QXP_SELFTEST_DXT_TARGET : 0.0f) - 0.5f;
            quad[index].z = 0.0f;
            quad[index].rhw = 1.0f;
            quad[index].u = (index & 1U) ? 1.0f : 0.0f;
            quad[index].v = (index & 2U) ? 1.0f : 0.0f;
        }
        (void)IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        (void)IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        (void)IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        (void)IDirect3DDevice9_SetTextureStageState(
            device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        /* Point sampling keeps each block's colour exact at the quadrant
         * centres; any filtering would blend the four blocks together. */
        (void)IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        (void)IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        (void)IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        (void)IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        (void)IDirect3DDevice9_SetSamplerState(
            device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        (void)IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        (void)IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,
                                              D3DZB_FALSE);
        (void)IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
                                              D3DCULL_NONE);
        (void)IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE,
                                              FALSE);
        (void)IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE,
                                              FALSE);
        (void)IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1);
        (void)IDirect3DDevice9_SetTexture(device, 0,
                                          (IDirect3DBaseTexture9 *)texture);
        (void)IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                     0xff202020UL, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(device))) {
            draw_hr = IDirect3DDevice9_DrawPrimitiveUP(
                device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
            (void)IDirect3DDevice9_EndScene(device);
        }
        (void)IDirect3DDevice9_SetTexture(device, 0, NULL);
        copy_hr = IDirect3DDevice9_GetRenderTargetData(device, target,
                                                       readback);
        if (SUCCEEDED(draw_hr) && SUCCEEDED(copy_hr) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(readback, &locked, NULL,
                                                 D3DLOCK_READONLY))) {
            sampled = 1;
            colours_match = 1;
            for (index = 0; index < 4; ++index) {
                const unsigned char *pixel =
                    (const unsigned char *)locked.pBits +
                    selftest_dxt_points[index][1] *
                        (unsigned long)locked.Pitch +
                    selftest_dxt_points[index][0] * 4UL;

                observed[index] = ((unsigned long)pixel[2] << 16) |
                                  ((unsigned long)pixel[1] << 8) |
                                  (unsigned long)pixel[0];
                if (observed[index] != selftest_dxt_expected[index])
                    colours_match = 0;
            }
            (void)IDirect3DSurface9_UnlockRect(readback);
        }
    }
    if (saved_target)
        (void)IDirect3DDevice9_SetRenderTarget(device, 0, saved_target);

    /* Sampling is only proved when the offscreen target path itself worked;
     * a target the driver will not create is a separate limitation, not a
     * compressed-format defect. */
    state = (pitch_ok && readback_match && (!sampled || colours_match)) ?
            QXP_SELFTEST_OK : QXP_SELFTEST_BAD;

finish:
    _snprintf(detail, sizeof(detail),
              "checkformat_hr=0x%08lx create_hr=0x%08lx lock_hr=0x%08lx "
              "pitch=%lu expected_pitch=%lu pitch_ok=%d readback_match=%d "
              "target_hr=0x%08lx draw_hr=0x%08lx copy_hr=0x%08lx sampled=%d "
              "observed0=0x%06lx expected0=0x%06lx "
              "observed1=0x%06lx expected1=0x%06lx "
              "observed2=0x%06lx expected2=0x%06lx "
              "observed3=0x%06lx expected3=0x%06lx overall=%s",
              (unsigned long)check_hr, (unsigned long)create_hr,
              (unsigned long)lock_hr, reported_pitch, expected_pitch,
              pitch_ok, readback_match, (unsigned long)target_hr,
              (unsigned long)draw_hr, (unsigned long)copy_hr, sampled,
              observed[0], selftest_dxt_expected[0],
              observed[1], selftest_dxt_expected[1],
              observed[2], selftest_dxt_expected[2],
              observed[3], selftest_dxt_expected[3],
              selftest_state_name(state));
    detail[sizeof(detail) - 1] = '\0';
    report(name, detail);
    if (texture) IDirect3DTexture9_Release(texture);
    if (readback) IDirect3DSurface9_Release(readback);
    if (target) IDirect3DSurface9_Release(target);
    if (saved_target) IDirect3DSurface9_Release(saved_target);
    return state;
}

/* Vertex-declaration profile probe.
 *
 * This is where Prince of Persia: The Sands of Time dies on the ordinary
 * profile.  Its PCStream::Init() builds multi-stream vertex declarations; the
 * D3D9 runtime refuses them because the driver reports no D3D9 DDI version
 * and MaxStreams=1, so CreateVertexDeclaration returns E_FAIL; the game does
 * not check, and faults inside d3d9.dll on the declaration it never got.
 *
 * A single-stream declaration must always work.  The multi-stream one is
 * reported, not required: it is declined by design until the native D3D9
 * profile is implemented and qualified, and this line is how a future
 * package proves it changed. */
static int selftest_vertexdecl_stage(IDirect3D9 *d3d, D3DFORMAT adapter_format,
                                     HINSTANCE instance)
{
    static const D3DVERTEXELEMENT9 single_stream[3] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 multi_stream[3] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    /* Software vertex processing hides the driver entirely: the runtime then
     * reports its own emulated profile (MaxStreams 16, vs_3_0) and accepts
     * any declaration.  A game asks for hardware, so this stage must too, or
     * it proves nothing about the driver. */
    static const DWORD behaviours[3] = {
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        D3DCREATE_MIXED_VERTEXPROCESSING,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING
    };
    IDirect3DVertexDeclaration9 *declaration = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS present;
    D3DCAPS9 caps;
    HWND window;
    HRESULT device_hr = E_FAIL;
    HRESULT single_hr = E_FAIL;
    HRESULT multi_hr = E_FAIL;
    DWORD behaviour = 0;
    unsigned long index;
    int state;
    char detail[512];

    memset(&caps, 0, sizeof(caps));
    window = CreateWindowExA(0, "STATIC", "qxpvertexdecl",
                             WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                             NULL, NULL, instance, NULL);
    if (window) {
        for (index = 0; index < 3; ++index) {
            memset(&present, 0, sizeof(present));
            present.Windowed = TRUE;
            present.SwapEffect = D3DSWAPEFFECT_DISCARD;
            present.BackBufferFormat = adapter_format;
            present.hDeviceWindow = window;
            behaviour = behaviours[index];
            device_hr = IDirect3D9_CreateDevice(
                d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                behaviour, &present, &device);
            if (SUCCEEDED(device_hr) && device) break;
            device = NULL;
        }
    }
    if (!device) {
        state = QXP_SELFTEST_BAD;
        goto finish;
    }
    (void)IDirect3DDevice9_GetDeviceCaps(device, &caps);
    single_hr = IDirect3DDevice9_CreateVertexDeclaration(
        device, single_stream, &declaration);
    if (SUCCEEDED(single_hr) && declaration) {
        IDirect3DVertexDeclaration9_Release(declaration);
        declaration = NULL;
    }
    multi_hr = IDirect3DDevice9_CreateVertexDeclaration(
        device, multi_stream, &declaration);
    if (SUCCEEDED(multi_hr) && declaration) {
        IDirect3DVertexDeclaration9_Release(declaration);
        declaration = NULL;
    }
    if (FAILED(single_hr)) {
        state = QXP_SELFTEST_BAD;
    } else if (SUCCEEDED(multi_hr)) {
        state = QXP_SELFTEST_OK;
    } else {
        state = QXP_SELFTEST_DECLINED;
    }
finish:
    _snprintf(detail, sizeof(detail),
              "device_hr=0x%08lx behaviour=0x%08lx single_stream_hr=0x%08lx "
              "multi_stream_hr=0x%08lx max_streams=%lu max_stream_stride=%lu "
              "vs_version=0x%08lx ps_version=0x%08lx decl_types=0x%08lx "
              "overall=%s",
              (unsigned long)device_hr, (unsigned long)behaviour,
              (unsigned long)single_hr, (unsigned long)multi_hr,
              (unsigned long)caps.MaxStreams,
              (unsigned long)caps.MaxStreamStride,
              (unsigned long)caps.VertexShaderVersion,
              (unsigned long)caps.PixelShaderVersion,
              (unsigned long)caps.DeclTypes,
              selftest_state_name(state));
    detail[sizeof(detail) - 1] = '\0';
    report("vertexdecl", detail);
    if (device) IDirect3DDevice9_Release(device);
    if (window) DestroyWindow(window);
    return state;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
                   LPSTR command_line, int show)
{
    static unsigned char pattern[QXP_SELFTEST_VERTEX_BYTES];
    QXP_D3D9_SOURCE_SELFTEST_RESULT result;
    QXP_D3D9_SOURCE_SELFTEST_REQUEST request;
    QXP_D3D9_SM2_LINK_SELFTEST_RESULT sm2_result;
    QXP_D3D9_SM2_LINK_SELFTEST_REQUEST sm2_request;
    QXP_D3D9_SM2_DRAW_SELFTEST_RESULT sm2_draw_result;
    QXP_D3D9_SM2_DRAW_SELFTEST_REQUEST sm2_draw_request;
    QXP_D3D9_SM2_TEXTURE_SELFTEST_RESULT sm2_texture_result;
    QXP_D3D9_SM2_TEXTURE_SELFTEST_REQUEST sm2_texture_request;
    D3D9_ENTRY_POINT entry;
    HMODULE module = NULL;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    D3DPRESENT_PARAMETERS present;
    D3DDISPLAYMODE mode;
    HWND window = NULL;
    HDC dc = NULL;
    void *mapped = NULL;
    HRESULT hr;
    char detail[512];
    char sm2_detail[512];
    char sm2_draw_detail[1024];
    char sm2_texture_detail[6144];
    char sm2_texldb_detail[1024];
    char sm2_four_sampler_detail[1024];
    char sm2_volume_detail[1024];
    int supported = 0;
    int returned = 0;
    int sm2_supported = 0;
    int sm2_returned = 0;
    int sm2_draw_supported = 0;
    int sm2_draw_returned = 0;
    int sm2_texture_supported = 0;
    int sm2_texture_returned = 0;
    int rc = 1;
    unsigned long expected = 0;
    int lifetime_ok = 0;
    int checksum_ok = 0;
    int sm2_ok = 0;
    int sm2_draw_ok = 0;
    int sm2_texture_ok = 0;
    int dxt1_state = QXP_SELFTEST_DECLINED;
    int dxt5_state = QXP_SELFTEST_DECLINED;
    int vertexdecl_state = QXP_SELFTEST_DECLINED;

    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command_line);
    UNREFERENCED_PARAMETER(show);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    memset(&result, 0, sizeof(result));
    memset(&sm2_result, 0, sizeof(sm2_result));
    memset(&sm2_draw_result, 0, sizeof(sm2_draw_result));
    memset(&sm2_texture_result, 0, sizeof(sm2_texture_result));

    module = LoadLibraryA("d3d9.dll");
    if (!module) {
        report("load", "d3d9.dll missing");
        return 2;
    }
    entry.proc = GetProcAddress(module, "Direct3DCreate9");
    if (!entry.proc) {
        report("entry", "Direct3DCreate9 missing");
        return 2;
    }
    d3d = entry.create(D3D_SDK_VERSION);
    if (!d3d) {
        report("create9", "Direct3DCreate9 returned NULL");
        return 2;
    }

    window = CreateWindowExA(0, "STATIC", "qxpd3d9selftest",
                             WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                             NULL, NULL, instance, NULL);
    if (!window) {
        report("window", "CreateWindowExA failed");
        rc = 2;
        goto done;
    }

    hr = IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        report("mode", detail);
        rc = 2;
        goto done;
    }

    memset(&present, 0, sizeof(present));
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = mode.Format;
    present.hDeviceWindow = window;
    hr = IDirect3D9_CreateDevice(
        d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device);
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        report("device", detail);
        rc = 2;
        goto done;
    }

    /* A system-memory vertex buffer is exactly the backing kind the guarded
     * source transaction accepts today. */
    hr = IDirect3DDevice9_CreateVertexBuffer(
        device, QXP_SELFTEST_VERTEX_BYTES, 0, QXP_SELFTEST_FVF,
        D3DPOOL_SYSTEMMEM, &vertex_buffer, NULL);
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        report("vertexbuffer", detail);
        rc = 2;
        goto done;
    }

    selftest_fill(pattern, QXP_SELFTEST_VERTEX_BYTES);
    hr = IDirect3DVertexBuffer9_Lock(
        vertex_buffer, 0, QXP_SELFTEST_VERTEX_BYTES, &mapped, 0);
    if (SUCCEEDED(hr)) {
        memcpy(mapped, pattern, QXP_SELFTEST_VERTEX_BYTES);
        hr = IDirect3DVertexBuffer9_Unlock(vertex_buffer);
    }
    if (FAILED(hr)) {
        _snprintf(detail, sizeof(detail), "hr=0x%08lx", (unsigned long)hr);
        report("fill", detail);
        rc = 2;
        goto done;
    }

    dc = GetDC(NULL);
    if (!dc) {
        report("dc", "GetDC failed");
        rc = 2;
        goto done;
    }

    {
        DWORD queried = QXP_D3D9_SOURCE_SELFTEST;

        supported = ExtEscape(dc, QXP_QUERYESCSUPPORT, (int)sizeof(queried),
                              (LPCSTR)&queried, 0, NULL);
    }
    if (supported <= 0) {
        _snprintf(detail, sizeof(detail), "queryescsupport=%d", supported);
        report("support", detail);
        rc = 3;
        goto done;
    }

    /* The runtime creates several system-memory vertex buffers per process, so
     * this probe cannot name its own surface handle.  Hand the driver the
     * checksum to look for instead and let it compare every eligible surface. */
    memset(&request, 0, sizeof(request));
    request.version = QXP_D3D9_SOURCE_SELFTEST_VERSION;
    request.byte_count = QXP_D3D9_SOURCE_SELFTEST_MAX_BYTES;
    expected = selftest_hash(pattern, request.byte_count);
    request.expected_checksum = (qxp_u32)expected;

    returned = ExtEscape(dc, QXP_D3D9_SOURCE_SELFTEST,
                         (int)sizeof(request), (LPCSTR)&request,
                         (int)sizeof(result), (LPSTR)&result);
    if (returned <= 0) {
        _snprintf(detail, sizeof(detail), "escape=%d", returned);
        report("escape", detail);
        rc = 3;
        goto done;
    }

    /* The safety-critical invariants: every admitted reader was revalidated
     * and released, and the gate drained back to zero.  A leaked reader would
     * make surface teardown spin forever. */
    lifetime_ok = result.gate_reader_count == 0 &&
                  result.admitted >= 1 &&
                  result.revalidated == result.admitted &&
                  result.released == result.admitted;

    /* The correctness invariant: at least one resolved base address really did
     * name this process's buffer contents. */
    checksum_ok = result.matches >= 1;

    _snprintf(detail, sizeof(detail),
              "examined=%lu admitted=%lu refused=%lu revalidated=%lu "
              "released=%lu readers=%lu matches=%lu match_handle=0x%lx "
              "first_handle=0x%lx bytes=%lu first_checksum=0x%08lx "
              "expected=0x%08lx status=%lu lifetime=%s checksum_match=%s",
              (unsigned long)result.surfaces_examined,
              (unsigned long)result.admitted,
              (unsigned long)result.refused,
              (unsigned long)result.revalidated,
              (unsigned long)result.released,
              (unsigned long)result.gate_reader_count,
              (unsigned long)result.matches,
              (unsigned long)result.match_handle,
              (unsigned long)result.first_handle,
              (unsigned long)result.first_byte_count,
              (unsigned long)result.first_checksum,
              expected, (unsigned long)result.last_status,
              lifetime_ok ? "ok" : "BAD",
              checksum_ok ? "ok" : "BAD");
    report("observed", detail);

    {
        DWORD queried = QXP_D3D9_SM2_LINK_SELFTEST;

        sm2_supported = ExtEscape(
            dc, QXP_QUERYESCSUPPORT, (int)sizeof(queried),
            (LPCSTR)&queried, 0, NULL);
    }
    memset(&sm2_request, 0, sizeof(sm2_request));
    sm2_request.version = QXP_D3D9_SM2_LINK_SELFTEST_VERSION;
    sm2_returned = ExtEscape(
        dc, QXP_D3D9_SM2_LINK_SELFTEST,
        (int)sizeof(sm2_request), (LPCSTR)&sm2_request,
        (int)sizeof(sm2_result), (LPSTR)&sm2_result);
    sm2_ok = sm2_supported > 0 && sm2_returned > 0 &&
             sm2_result.version == QXP_D3D9_SM2_LINK_SELFTEST_VERSION &&
             sm2_result.context_found != 0 &&
             sm2_result.stage == QXP_D3D9_SM2_LINK_STAGE_COMPLETE &&
             sm2_result.vertex_decode_status == 0 &&
             sm2_result.pixel_decode_status == 0 &&
             sm2_result.vertex_glsl_bytes != 0 &&
             sm2_result.pixel_glsl_bytes != 0 &&
             sm2_result.define_succeeded != 0 &&
             sm2_result.delete_succeeded != 0 &&
             sm2_result.overall != 0;
    _snprintf(sm2_detail, sizeof(sm2_detail),
              "sm2_supported=%d sm2_returned=%d sm2_version=%lu "
              "sm2_context=%lu sm2_stage=%lu sm2_vs_decode=%lu "
              "sm2_ps_decode=%lu sm2_vs_bytes=%lu sm2_ps_bytes=%lu "
              "sm2_defined=%lu sm2_deleted=%lu sm2_ok=%lu",
              sm2_supported, sm2_returned,
              (unsigned long)sm2_result.version,
              (unsigned long)sm2_result.context_found,
              (unsigned long)sm2_result.stage,
              (unsigned long)sm2_result.vertex_decode_status,
              (unsigned long)sm2_result.pixel_decode_status,
              (unsigned long)sm2_result.vertex_glsl_bytes,
              (unsigned long)sm2_result.pixel_glsl_bytes,
              (unsigned long)sm2_result.define_succeeded,
              (unsigned long)sm2_result.delete_succeeded,
              (unsigned long)sm2_result.overall);
    sm2_detail[sizeof(sm2_detail) - 1] = '\0';
    report("sm2_link", sm2_detail);

    {
        DWORD queried = QXP_D3D9_SM2_DRAW_SELFTEST;

        sm2_draw_supported = ExtEscape(
            dc, QXP_QUERYESCSUPPORT, (int)sizeof(queried),
            (LPCSTR)&queried, 0, NULL);
    }
    memset(&sm2_draw_request, 0, sizeof(sm2_draw_request));
    sm2_draw_request.version = QXP_D3D9_SM2_DRAW_SELFTEST_VERSION;
    sm2_draw_returned = ExtEscape(
        dc, QXP_D3D9_SM2_DRAW_SELFTEST,
        (int)sizeof(sm2_draw_request), (LPCSTR)&sm2_draw_request,
        (int)sizeof(sm2_draw_result), (LPSTR)&sm2_draw_result);
    sm2_draw_ok = sm2_draw_supported > 0 && sm2_draw_returned > 0 &&
        sm2_draw_result.version == QXP_D3D9_SM2_DRAW_SELFTEST_VERSION &&
        sm2_draw_result.context_found != 0 &&
        sm2_draw_result.stage == QXP_D3D9_SM2_DRAW_STAGE_COMPLETE &&
        sm2_draw_result.vertex_decode_status == 0 &&
        sm2_draw_result.pixel_decode_status == 0 &&
        sm2_draw_result.vertex_glsl_bytes != 0 &&
        sm2_draw_result.pixel_glsl_bytes != 0 &&
        sm2_draw_result.define_succeeded != 0 &&
        sm2_draw_result.constants_succeeded != 0 &&
        sm2_draw_result.texture_defined != 0 &&
        sm2_draw_result.target_bound != 0 &&
        sm2_draw_result.clear_succeeded != 0 &&
        sm2_draw_result.draw_succeeded != 0 &&
        sm2_draw_result.readback_succeeded != 0 &&
        sm2_draw_result.inside_x == 10 &&
        sm2_draw_result.inside_y == 4 &&
        sm2_draw_result.outside_x == 4 &&
        sm2_draw_result.outside_y == 13 &&
        sm2_draw_result.expected_inside == 0xff2060dfUL &&
        sm2_draw_result.observed_inside ==
            sm2_draw_result.expected_inside &&
        sm2_draw_result.expected_outside == 0xff102040UL &&
        sm2_draw_result.observed_outside ==
            sm2_draw_result.expected_outside &&
        sm2_draw_result.target_restored != 0 &&
        sm2_draw_result.program_deleted != 0 &&
        sm2_draw_result.texture_deleted != 0 &&
        sm2_draw_result.memory_restored != 0 &&
        sm2_draw_result.overall != 0;
    _snprintf(sm2_draw_detail, sizeof(sm2_draw_detail),
              "supported=%d returned=%d version=%lu context_found=%lu "
              "stage=%lu vertex_decode_status=%lu "
              "pixel_decode_status=%lu vertex_glsl_bytes=%lu "
              "pixel_glsl_bytes=%lu define_succeeded=%lu "
              "constants_succeeded=%lu texture_defined=%lu "
              "target_bound=%lu clear_succeeded=%lu draw_succeeded=%lu "
              "readback_succeeded=%lu inside_x=%lu inside_y=%lu "
              "outside_x=%lu outside_y=%lu expected_inside=0x%08lx "
              "observed_inside=0x%08lx expected_outside=0x%08lx "
              "observed_outside=0x%08lx target_restored=%lu "
              "program_deleted=%lu texture_deleted=%lu "
              "memory_restored=%lu unbound_sample=opaque-black "
              "overall=%lu ok=%s",
              sm2_draw_supported, sm2_draw_returned,
              (unsigned long)sm2_draw_result.version,
              (unsigned long)sm2_draw_result.context_found,
              (unsigned long)sm2_draw_result.stage,
              (unsigned long)sm2_draw_result.vertex_decode_status,
              (unsigned long)sm2_draw_result.pixel_decode_status,
              (unsigned long)sm2_draw_result.vertex_glsl_bytes,
              (unsigned long)sm2_draw_result.pixel_glsl_bytes,
              (unsigned long)sm2_draw_result.define_succeeded,
              (unsigned long)sm2_draw_result.constants_succeeded,
              (unsigned long)sm2_draw_result.texture_defined,
              (unsigned long)sm2_draw_result.target_bound,
              (unsigned long)sm2_draw_result.clear_succeeded,
              (unsigned long)sm2_draw_result.draw_succeeded,
              (unsigned long)sm2_draw_result.readback_succeeded,
              (unsigned long)sm2_draw_result.inside_x,
              (unsigned long)sm2_draw_result.inside_y,
              (unsigned long)sm2_draw_result.outside_x,
              (unsigned long)sm2_draw_result.outside_y,
              (unsigned long)sm2_draw_result.expected_inside,
              (unsigned long)sm2_draw_result.observed_inside,
              (unsigned long)sm2_draw_result.expected_outside,
              (unsigned long)sm2_draw_result.observed_outside,
              (unsigned long)sm2_draw_result.target_restored,
              (unsigned long)sm2_draw_result.program_deleted,
              (unsigned long)sm2_draw_result.texture_deleted,
              (unsigned long)sm2_draw_result.memory_restored,
              (unsigned long)sm2_draw_result.overall,
              sm2_draw_ok ? "ok" : "BAD");
    sm2_draw_detail[sizeof(sm2_draw_detail) - 1] = '\0';
    report("sm2_draw", sm2_draw_detail);

    {
        DWORD queried = QXP_D3D9_SM2_TEXTURE_SELFTEST;

        sm2_texture_supported = ExtEscape(
            dc, QXP_QUERYESCSUPPORT, (int)sizeof(queried),
            (LPCSTR)&queried, 0, NULL);
    }
    memset(&sm2_texture_request, 0, sizeof(sm2_texture_request));
    sm2_texture_request.version = QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION;
    sm2_texture_returned = ExtEscape(
        dc, QXP_D3D9_SM2_TEXTURE_SELFTEST,
        (int)sizeof(sm2_texture_request),
        (LPCSTR)&sm2_texture_request,
        (int)sizeof(sm2_texture_result), (LPSTR)&sm2_texture_result);
    sm2_texture_ok = sm2_texture_supported > 0 &&
        sm2_texture_returned > 0 &&
        sizeof(sm2_texture_result) == 0x360 &&
        sm2_texture_result.version ==
            QXP_D3D9_SM2_TEXTURE_SELFTEST_VERSION &&
        sm2_texture_result.context_found != 0 &&
        sm2_texture_result.stage ==
            QXP_D3D9_SM2_TEXTURE_STAGE_VOLUME_COMPLETE &&
        sm2_texture_result.vertex_decode_status == 0 &&
        sm2_texture_result.pixel_decode_status == 0 &&
        sm2_texture_result.vertex_glsl_bytes != 0 &&
        sm2_texture_result.pixel_glsl_bytes != 0 &&
        sm2_texture_result.define_succeeded != 0 &&
        sm2_texture_result.sample_texture_defined != 0 &&
        sm2_texture_result.sample_texture_uploaded != 0 &&
        sm2_texture_result.target_texture_defined != 0 &&
        sm2_texture_result.target_bound != 0 &&
        sm2_texture_result.clear_succeeded != 0 &&
        sm2_texture_result.draw_succeeded != 0 &&
        sm2_texture_result.readback_succeeded != 0 &&
        sm2_texture_result.sample0_x == 4 &&
        sm2_texture_result.sample0_y == 10 &&
        sm2_texture_result.expected_sample0 == 0xff459709UL &&
        sm2_texture_result.observed_sample0 ==
            sm2_texture_result.expected_sample0 &&
        sm2_texture_result.sample1_x == 10 &&
        sm2_texture_result.sample1_y == 4 &&
        sm2_texture_result.expected_sample1 == 0xff8f3d11UL &&
        sm2_texture_result.observed_sample1 ==
            sm2_texture_result.expected_sample1 &&
        sm2_texture_result.target_restored != 0 &&
        sm2_texture_result.program_deleted != 0 &&
        sm2_texture_result.sample_texture_deleted != 0 &&
        sm2_texture_result.target_texture_deleted != 0 &&
        sm2_texture_result.memory_restored != 0 &&
        sm2_texture_result.overall != 0 &&
        sm2_texture_result.multi_texture_defined != 0 &&
        sm2_texture_result.multi_texture_uploaded != 0 &&
        sm2_texture_result.multi_vertex_decode_status == 0 &&
        sm2_texture_result.multi_vertex_glsl_bytes != 0 &&
        sm2_texture_result.multi_pixel_decode_status == 0 &&
        sm2_texture_result.multi_pixel_glsl_bytes != 0 &&
        sm2_texture_result.multi_define_succeeded != 0 &&
        sm2_texture_result.multi_draw_succeeded != 0 &&
        sm2_texture_result.multi_readback_succeeded != 0 &&
        sm2_texture_result.multi_expected_sample0 == 0xff55c749UL &&
        sm2_texture_result.multi_observed_sample0 ==
            sm2_texture_result.multi_expected_sample0 &&
        sm2_texture_result.multi_expected_sample1 == 0xffbf4d51UL &&
        sm2_texture_result.multi_observed_sample1 ==
            sm2_texture_result.multi_expected_sample1 &&
        sm2_texture_result.multi_program_deleted != 0 &&
        sm2_texture_result.multi_texture_deleted != 0 &&
        sm2_texture_result.multi_overall != 0 &&
        sm2_texture_result.proj_vertex_decode_status == 0 &&
        sm2_texture_result.proj_vertex_glsl_bytes != 0 &&
        sm2_texture_result.proj_pixel_decode_status == 0 &&
        sm2_texture_result.proj_pixel_glsl_bytes != 0 &&
        sm2_texture_result.proj_define_succeeded != 0 &&
        sm2_texture_result.proj_constants_succeeded != 0 &&
        sm2_texture_result.proj_draw_succeeded != 0 &&
        sm2_texture_result.proj_readback_succeeded != 0 &&
        sm2_texture_result.proj_expected_sample0 == 0xff459709UL &&
        sm2_texture_result.proj_observed_sample0 ==
            sm2_texture_result.proj_expected_sample0 &&
        sm2_texture_result.proj_expected_sample1 == 0xff8f3d11UL &&
        sm2_texture_result.proj_observed_sample1 ==
            sm2_texture_result.proj_expected_sample1 &&
        sm2_texture_result.proj_overall != 0 &&
        sm2_texture_result.cube_vertex_decode_status == 0 &&
        sm2_texture_result.cube_vertex_glsl_bytes != 0 &&
        sm2_texture_result.cube_pixel_decode_status == 0 &&
        sm2_texture_result.cube_pixel_glsl_bytes != 0 &&
        sm2_texture_result.cube_define_succeeded != 0 &&
        sm2_texture_result.cube_constants_succeeded != 0 &&
        sm2_texture_result.cube_draw_succeeded != 0 &&
        sm2_texture_result.cube_readback_succeeded != 0 &&
        sm2_texture_result.cube_expected_sample0 == 0xff2060a0UL &&
        sm2_texture_result.cube_observed_sample0 ==
            sm2_texture_result.cube_expected_sample0 &&
        sm2_texture_result.cube_expected_sample1 == 0xff2060a0UL &&
        sm2_texture_result.cube_observed_sample1 ==
            sm2_texture_result.cube_expected_sample1 &&
        sm2_texture_result.cube_program_deleted != 0 &&
        sm2_texture_result.cube_overall != 0 &&
        sm2_texture_result.depth_vertex_decode_status == 0 &&
        sm2_texture_result.depth_vertex_glsl_bytes != 0 &&
        sm2_texture_result.depth_pixel_decode_status == 0 &&
        sm2_texture_result.depth_pixel_glsl_bytes != 0 &&
        sm2_texture_result.depth_define_succeeded != 0 &&
        sm2_texture_result.depth_constants_succeeded != 0 &&
        sm2_texture_result.depth_target_bound != 0 &&
        sm2_texture_result.depth_clear_succeeded != 0 &&
        sm2_texture_result.depth_draw_succeeded != 0 &&
        sm2_texture_result.depth_readback_succeeded != 0 &&
        sm2_texture_result.depth_overlap_x == 5 &&
        sm2_texture_result.depth_overlap_y == 5 &&
        sm2_texture_result.depth_far_only_x == 12 &&
        sm2_texture_result.depth_far_only_y == 12 &&
        sm2_texture_result.depth_expected_pass_a_overlap == 0xffdf9f20UL &&
        sm2_texture_result.depth_observed_pass_a_overlap ==
            sm2_texture_result.depth_expected_pass_a_overlap &&
        sm2_texture_result.depth_expected_pass_a_far == 0xff2060dfUL &&
        sm2_texture_result.depth_observed_pass_a_far ==
            sm2_texture_result.depth_expected_pass_a_far &&
        sm2_texture_result.depth_expected_pass_b_overlap == 0xffdf9f20UL &&
        sm2_texture_result.depth_observed_pass_b_overlap ==
            sm2_texture_result.depth_expected_pass_b_overlap &&
        sm2_texture_result.depth_expected_pass_b_far == 0xff2060dfUL &&
        sm2_texture_result.depth_observed_pass_b_far ==
            sm2_texture_result.depth_expected_pass_b_far &&
        sm2_texture_result.depth_program_deleted != 0 &&
        sm2_texture_result.depth_buffer_deleted != 0 &&
        sm2_texture_result.depth_overall != 0 &&
        sm2_texture_result.scissor_vertex_decode_status == 0 &&
        sm2_texture_result.scissor_vertex_glsl_bytes != 0 &&
        sm2_texture_result.scissor_pixel_decode_status == 0 &&
        sm2_texture_result.scissor_pixel_glsl_bytes != 0 &&
        sm2_texture_result.scissor_define_succeeded != 0 &&
        sm2_texture_result.scissor_constants_succeeded != 0 &&
        sm2_texture_result.scissor_target_bound != 0 &&
        sm2_texture_result.scissor_clear_succeeded != 0 &&
        sm2_texture_result.scissor_draw_succeeded != 0 &&
        sm2_texture_result.scissor_readback_succeeded != 0 &&
        sm2_texture_result.scissor_excluded_x == 4 &&
        sm2_texture_result.scissor_excluded_y == 10 &&
        sm2_texture_result.scissor_included_x == 10 &&
        sm2_texture_result.scissor_included_y == 4 &&
        sm2_texture_result.scissor_expected_pass_a_excluded ==
            0xff07131fUL &&
        sm2_texture_result.scissor_observed_pass_a_excluded ==
            sm2_texture_result.scissor_expected_pass_a_excluded &&
        sm2_texture_result.scissor_expected_pass_a_included ==
            0xffdf9f20UL &&
        sm2_texture_result.scissor_observed_pass_a_included ==
            sm2_texture_result.scissor_expected_pass_a_included &&
        sm2_texture_result.scissor_expected_pass_b_excluded ==
            0xffdf9f20UL &&
        sm2_texture_result.scissor_observed_pass_b_excluded ==
            sm2_texture_result.scissor_expected_pass_b_excluded &&
        sm2_texture_result.scissor_expected_pass_b_included ==
            0xffdf9f20UL &&
        sm2_texture_result.scissor_observed_pass_b_included ==
            sm2_texture_result.scissor_expected_pass_b_included &&
        sm2_texture_result.scissor_program_deleted != 0 &&
        sm2_texture_result.scissor_overall != 0 &&
        sm2_texture_result.stream_offset_vertex_decode_status == 0 &&
        sm2_texture_result.stream_offset_vertex_glsl_bytes != 0 &&
        sm2_texture_result.stream_offset_pixel_decode_status == 0 &&
        sm2_texture_result.stream_offset_pixel_glsl_bytes != 0 &&
        sm2_texture_result.stream_offset_define_succeeded != 0 &&
        sm2_texture_result.stream_offset_constants_succeeded != 0 &&
        sm2_texture_result.stream_offset_target_bound != 0 &&
        sm2_texture_result.stream_offset_clear_succeeded != 0 &&
        sm2_texture_result.stream_offset_draw_succeeded != 0 &&
        sm2_texture_result.stream_offset_readback_succeeded != 0 &&
        sm2_texture_result.stream_offset_stride == 16 &&
        sm2_texture_result.stream_offset_buffer_bytes == 96 &&
        sm2_texture_result.stream_offset_pass_a_offset == 0 &&
        sm2_texture_result.stream_offset_pass_b_offset == 48 &&
        sm2_texture_result.stream_offset_left_x == 2 &&
        sm2_texture_result.stream_offset_left_y == 7 &&
        sm2_texture_result.stream_offset_right_x == 12 &&
        sm2_texture_result.stream_offset_right_y == 7 &&
        sm2_texture_result.stream_offset_expected_pass_a_left ==
            0xffdf9f20UL &&
        sm2_texture_result.stream_offset_observed_pass_a_left ==
            sm2_texture_result.stream_offset_expected_pass_a_left &&
        sm2_texture_result.stream_offset_expected_pass_a_right ==
            0xff07131fUL &&
        sm2_texture_result.stream_offset_observed_pass_a_right ==
            sm2_texture_result.stream_offset_expected_pass_a_right &&
        sm2_texture_result.stream_offset_expected_pass_b_left ==
            0xff07131fUL &&
        sm2_texture_result.stream_offset_observed_pass_b_left ==
            sm2_texture_result.stream_offset_expected_pass_b_left &&
        sm2_texture_result.stream_offset_expected_pass_b_right ==
            0xffdf9f20UL &&
        sm2_texture_result.stream_offset_observed_pass_b_right ==
            sm2_texture_result.stream_offset_expected_pass_b_right &&
        sm2_texture_result.stream_offset_expected_pass_a_left !=
            sm2_texture_result.stream_offset_expected_pass_b_left &&
        sm2_texture_result.stream_offset_expected_pass_a_right !=
            sm2_texture_result.stream_offset_expected_pass_b_right &&
        sm2_texture_result.stream_offset_program_deleted != 0 &&
        sm2_texture_result.stream_offset_overall == 1 &&
        sm2_texture_result.texldb_texture_defined != 0 &&
        sm2_texture_result.texldb_texture_uploaded != 0 &&
        sm2_texture_result.texldb_vertex_decode_status == 0 &&
        sm2_texture_result.texldb_vertex_glsl_bytes != 0 &&
        sm2_texture_result.texldb_pixel_decode_status == 0 &&
        sm2_texture_result.texldb_pixel_glsl_bytes != 0 &&
        sm2_texture_result.texldb_define_succeeded != 0 &&
        sm2_texture_result.texldb_target_bound != 0 &&
        sm2_texture_result.texldb_sample_x == 7 &&
        sm2_texture_result.texldb_sample_y == 7 &&
        sm2_texture_result.texldb_control_clear_succeeded != 0 &&
        sm2_texture_result.texldb_control_draw_succeeded != 0 &&
        sm2_texture_result.texldb_control_readback_succeeded != 0 &&
        sm2_texture_result.texldb_expected_control == 0xffe04020UL &&
        sm2_texture_result.texldb_observed_control ==
            sm2_texture_result.texldb_expected_control &&
        sm2_texture_result.texldb_biased_clear_succeeded != 0 &&
        sm2_texture_result.texldb_biased_draw_succeeded != 0 &&
        sm2_texture_result.texldb_biased_readback_succeeded != 0 &&
        sm2_texture_result.texldb_expected_biased == 0xff40a0e0UL &&
        sm2_texture_result.texldb_observed_biased ==
            sm2_texture_result.texldb_expected_biased &&
        sm2_texture_result.texldb_expected_control !=
            sm2_texture_result.texldb_expected_biased &&
        sm2_texture_result.texldb_program_deleted != 0 &&
        sm2_texture_result.texldb_texture_deleted != 0 &&
        sm2_texture_result.texldb_overall != 0 &&
        sm2_texture_result.four_texture2_defined != 0 &&
        sm2_texture_result.four_texture2_uploaded != 0 &&
        sm2_texture_result.four_texture3_defined != 0 &&
        sm2_texture_result.four_texture3_uploaded != 0 &&
        sm2_texture_result.four_vertex_decode_status == 0 &&
        sm2_texture_result.four_vertex_glsl_bytes != 0 &&
        sm2_texture_result.four_pixel_decode_status == 0 &&
        sm2_texture_result.four_pixel_glsl_bytes != 0 &&
        sm2_texture_result.four_define_succeeded != 0 &&
        sm2_texture_result.four_target_bound != 0 &&
        sm2_texture_result.four_clear_succeeded != 0 &&
        sm2_texture_result.four_draw_succeeded != 0 &&
        sm2_texture_result.four_readback_succeeded != 0 &&
        sm2_texture_result.four_expected_sample0 == 0xff6ddb57UL &&
        sm2_texture_result.four_observed_sample0 ==
            sm2_texture_result.four_expected_sample0 &&
        sm2_texture_result.four_expected_sample1 == 0xffd7615fUL &&
        sm2_texture_result.four_observed_sample1 ==
            sm2_texture_result.four_expected_sample1 &&
        sm2_texture_result.four_expected_sample0 !=
            sm2_texture_result.multi_expected_sample0 &&
        sm2_texture_result.four_expected_sample1 !=
            sm2_texture_result.multi_expected_sample1 &&
        sm2_texture_result.four_program_deleted != 0 &&
        sm2_texture_result.four_texture2_deleted != 0 &&
        sm2_texture_result.four_texture3_deleted != 0 &&
        sm2_texture_result.four_overall != 0 &&
        sm2_texture_result.volume_texture_defined != 0 &&
        sm2_texture_result.volume_texture_uploaded != 0 &&
        sm2_texture_result.volume_vertex_decode_status == 0 &&
        sm2_texture_result.volume_vertex_glsl_bytes != 0 &&
        sm2_texture_result.volume_pixel_decode_status == 0 &&
        sm2_texture_result.volume_pixel_glsl_bytes != 0 &&
        sm2_texture_result.volume_define_succeeded != 0 &&
        sm2_texture_result.volume_target_bound != 0 &&
        sm2_texture_result.volume_clear_succeeded != 0 &&
        sm2_texture_result.volume_draw_succeeded != 0 &&
        sm2_texture_result.volume_readback_succeeded != 0 &&
        sm2_texture_result.volume_sample0_x == 4 &&
        sm2_texture_result.volume_sample0_y == 7 &&
        sm2_texture_result.volume_sample1_x == 10 &&
        sm2_texture_result.volume_sample1_y == 7 &&
        sm2_texture_result.volume_expected_sample0 == 0xff23500aUL &&
        sm2_texture_result.volume_expected_sample1 == 0xff835a2aUL &&
        sm2_texture_result.volume_expected_sample0 !=
            sm2_texture_result.volume_expected_sample1 &&
        sm2_texture_result.volume_observed_sample0 ==
            sm2_texture_result.volume_expected_sample0 &&
        sm2_texture_result.volume_observed_sample1 ==
            sm2_texture_result.volume_expected_sample1 &&
        sm2_texture_result.volume_program_deleted != 0 &&
        sm2_texture_result.volume_texture_deleted != 0 &&
        sm2_texture_result.volume_overall != 0;
    _snprintf(sm2_texture_detail, sizeof(sm2_texture_detail),
              "supported=%d returned=%d version=%lu context_found=%lu "
              "stage=%lu vertex_decode_status=%lu "
              "pixel_decode_status=%lu vertex_glsl_bytes=%lu "
              "pixel_glsl_bytes=%lu define_succeeded=%lu "
              "sample_texture_defined=%lu sample_texture_uploaded=%lu "
              "target_texture_defined=%lu target_bound=%lu "
              "clear_succeeded=%lu draw_succeeded=%lu "
              "readback_succeeded=%lu sample0_x=%lu sample0_y=%lu "
              "expected_sample0=0x%08lx observed_sample0=0x%08lx "
              "sample1_x=%lu sample1_y=%lu expected_sample1=0x%08lx "
              "observed_sample1=0x%08lx target_restored=%lu "
              "program_deleted=%lu sample_texture_deleted=%lu "
              "target_texture_deleted=%lu memory_restored=%lu "
              "overall=%lu multi_vertex_glsl_bytes=%lu "
              "multi_pixel_glsl_bytes=%lu multi_define_succeeded=%lu "
              "multi_draw_succeeded=%lu multi_readback_succeeded=%lu "
              "expected_multi0=0x%08lx observed_multi0=0x%08lx "
              "expected_multi1=0x%08lx observed_multi1=0x%08lx "
              "multi_overall=%lu proj_vertex_glsl_bytes=%lu "
              "proj_pixel_glsl_bytes=%lu proj_define_succeeded=%lu "
              "proj_constants_succeeded=%lu proj_draw_succeeded=%lu "
              "proj_readback_succeeded=%lu expected_proj0=0x%08lx "
              "observed_proj0=0x%08lx expected_proj1=0x%08lx "
              "observed_proj1=0x%08lx proj_overall=%lu "
              "cube_vertex_decode_status=%lu "
              "cube_vertex_glsl_bytes=%lu "
              "cube_pixel_decode_status=%lu cube_pixel_glsl_bytes=%lu "
              "cube_define_succeeded=%lu cube_constants_succeeded=%lu "
              "cube_draw_succeeded=%lu cube_readback_succeeded=%lu "
              "expected_cube0=0x%08lx observed_cube0=0x%08lx "
              "expected_cube1=0x%08lx observed_cube1=0x%08lx "
              "cube_program_deleted=%lu cube_overall=%lu "
              "depth_vertex_decode_status=%lu "
              "depth_vertex_glsl_bytes=%lu "
              "depth_pixel_decode_status=%lu "
              "depth_pixel_glsl_bytes=%lu depth_define_succeeded=%lu "
              "depth_constants_succeeded=%lu depth_target_bound=%lu "
              "depth_clear_succeeded=%lu depth_draw_succeeded=%lu "
              "depth_readback_succeeded=%lu depth_overlap_x=%lu "
              "depth_overlap_y=%lu depth_far_only_x=%lu "
              "depth_far_only_y=%lu "
              "expected_depth_a_overlap=0x%08lx "
              "observed_depth_a_overlap=0x%08lx "
              "expected_depth_a_far=0x%08lx "
              "observed_depth_a_far=0x%08lx "
              "expected_depth_b_overlap=0x%08lx "
              "observed_depth_b_overlap=0x%08lx "
              "expected_depth_b_far=0x%08lx "
              "observed_depth_b_far=0x%08lx "
              "depth_program_deleted=%lu depth_buffer_deleted=%lu "
              "depth_overall=%lu "
              "scissor_vertex_decode_status=%lu "
              "scissor_vertex_glsl_bytes=%lu "
              "scissor_pixel_decode_status=%lu "
              "scissor_pixel_glsl_bytes=%lu "
              "scissor_define_succeeded=%lu "
              "scissor_constants_succeeded=%lu "
              "scissor_target_bound=%lu scissor_clear_succeeded=%lu "
              "scissor_draw_succeeded=%lu "
              "scissor_readback_succeeded=%lu "
              "scissor_excluded_x=%lu scissor_excluded_y=%lu "
              "scissor_included_x=%lu scissor_included_y=%lu "
              "expected_scissor_a_excluded=0x%08lx "
              "observed_scissor_a_excluded=0x%08lx "
              "expected_scissor_a_included=0x%08lx "
              "observed_scissor_a_included=0x%08lx "
              "expected_scissor_b_excluded=0x%08lx "
              "observed_scissor_b_excluded=0x%08lx "
              "expected_scissor_b_included=0x%08lx "
              "observed_scissor_b_included=0x%08lx "
              "scissor_program_deleted=%lu scissor_overall=%lu "
              "stream_offset_vertex_decode_status=%lu "
              "stream_offset_vertex_glsl_bytes=%lu "
              "stream_offset_pixel_decode_status=%lu "
              "stream_offset_pixel_glsl_bytes=%lu "
              "stream_offset_define_succeeded=%lu "
              "stream_offset_constants_succeeded=%lu "
              "stream_offset_target_bound=%lu "
              "stream_offset_clear_succeeded=%lu "
              "stream_offset_draw_succeeded=%lu "
              "stream_offset_readback_succeeded=%lu "
              "stream_offset_stride=%lu stream_offset_buffer_bytes=%lu "
              "stream_offset_pass_a_offset=%lu "
              "stream_offset_pass_b_offset=%lu "
              "stream_offset_left_x=%lu stream_offset_left_y=%lu "
              "stream_offset_right_x=%lu stream_offset_right_y=%lu "
              "expected_stream_offset_a_left=0x%08lx "
              "observed_stream_offset_a_left=0x%08lx "
              "expected_stream_offset_a_right=0x%08lx "
              "observed_stream_offset_a_right=0x%08lx "
              "expected_stream_offset_b_left=0x%08lx "
              "observed_stream_offset_b_left=0x%08lx "
              "expected_stream_offset_b_right=0x%08lx "
              "observed_stream_offset_b_right=0x%08lx "
              "stream_offset_program_deleted=%lu "
              "stream_offset_overall=%lu ok=%s",
              sm2_texture_supported, sm2_texture_returned,
              (unsigned long)sm2_texture_result.version,
              (unsigned long)sm2_texture_result.context_found,
              (unsigned long)sm2_texture_result.stage,
              (unsigned long)sm2_texture_result.vertex_decode_status,
              (unsigned long)sm2_texture_result.pixel_decode_status,
              (unsigned long)sm2_texture_result.vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.define_succeeded,
              (unsigned long)sm2_texture_result.sample_texture_defined,
              (unsigned long)sm2_texture_result.sample_texture_uploaded,
              (unsigned long)sm2_texture_result.target_texture_defined,
              (unsigned long)sm2_texture_result.target_bound,
              (unsigned long)sm2_texture_result.clear_succeeded,
              (unsigned long)sm2_texture_result.draw_succeeded,
              (unsigned long)sm2_texture_result.readback_succeeded,
              (unsigned long)sm2_texture_result.sample0_x,
              (unsigned long)sm2_texture_result.sample0_y,
              (unsigned long)sm2_texture_result.expected_sample0,
              (unsigned long)sm2_texture_result.observed_sample0,
              (unsigned long)sm2_texture_result.sample1_x,
              (unsigned long)sm2_texture_result.sample1_y,
              (unsigned long)sm2_texture_result.expected_sample1,
              (unsigned long)sm2_texture_result.observed_sample1,
              (unsigned long)sm2_texture_result.target_restored,
              (unsigned long)sm2_texture_result.program_deleted,
              (unsigned long)sm2_texture_result.sample_texture_deleted,
              (unsigned long)sm2_texture_result.target_texture_deleted,
              (unsigned long)sm2_texture_result.memory_restored,
              (unsigned long)sm2_texture_result.overall,
              (unsigned long)sm2_texture_result.multi_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.multi_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.multi_define_succeeded,
              (unsigned long)sm2_texture_result.multi_draw_succeeded,
              (unsigned long)sm2_texture_result.multi_readback_succeeded,
              (unsigned long)sm2_texture_result.multi_expected_sample0,
              (unsigned long)sm2_texture_result.multi_observed_sample0,
              (unsigned long)sm2_texture_result.multi_expected_sample1,
              (unsigned long)sm2_texture_result.multi_observed_sample1,
              (unsigned long)sm2_texture_result.multi_overall,
              (unsigned long)sm2_texture_result.proj_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.proj_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.proj_define_succeeded,
              (unsigned long)sm2_texture_result.proj_constants_succeeded,
              (unsigned long)sm2_texture_result.proj_draw_succeeded,
              (unsigned long)sm2_texture_result.proj_readback_succeeded,
              (unsigned long)sm2_texture_result.proj_expected_sample0,
              (unsigned long)sm2_texture_result.proj_observed_sample0,
              (unsigned long)sm2_texture_result.proj_expected_sample1,
              (unsigned long)sm2_texture_result.proj_observed_sample1,
              (unsigned long)sm2_texture_result.proj_overall,
              (unsigned long)sm2_texture_result.cube_vertex_decode_status,
              (unsigned long)sm2_texture_result.cube_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.cube_pixel_decode_status,
              (unsigned long)sm2_texture_result.cube_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.cube_define_succeeded,
              (unsigned long)sm2_texture_result.cube_constants_succeeded,
              (unsigned long)sm2_texture_result.cube_draw_succeeded,
              (unsigned long)sm2_texture_result.cube_readback_succeeded,
              (unsigned long)sm2_texture_result.cube_expected_sample0,
              (unsigned long)sm2_texture_result.cube_observed_sample0,
              (unsigned long)sm2_texture_result.cube_expected_sample1,
              (unsigned long)sm2_texture_result.cube_observed_sample1,
              (unsigned long)sm2_texture_result.cube_program_deleted,
              (unsigned long)sm2_texture_result.cube_overall,
              (unsigned long)sm2_texture_result.depth_vertex_decode_status,
              (unsigned long)sm2_texture_result.depth_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.depth_pixel_decode_status,
              (unsigned long)sm2_texture_result.depth_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.depth_define_succeeded,
              (unsigned long)sm2_texture_result.depth_constants_succeeded,
              (unsigned long)sm2_texture_result.depth_target_bound,
              (unsigned long)sm2_texture_result.depth_clear_succeeded,
              (unsigned long)sm2_texture_result.depth_draw_succeeded,
              (unsigned long)sm2_texture_result.depth_readback_succeeded,
              (unsigned long)sm2_texture_result.depth_overlap_x,
              (unsigned long)sm2_texture_result.depth_overlap_y,
              (unsigned long)sm2_texture_result.depth_far_only_x,
              (unsigned long)sm2_texture_result.depth_far_only_y,
              (unsigned long)
                  sm2_texture_result.depth_expected_pass_a_overlap,
              (unsigned long)
                  sm2_texture_result.depth_observed_pass_a_overlap,
              (unsigned long)sm2_texture_result.depth_expected_pass_a_far,
              (unsigned long)sm2_texture_result.depth_observed_pass_a_far,
              (unsigned long)
                  sm2_texture_result.depth_expected_pass_b_overlap,
              (unsigned long)
                  sm2_texture_result.depth_observed_pass_b_overlap,
              (unsigned long)sm2_texture_result.depth_expected_pass_b_far,
              (unsigned long)sm2_texture_result.depth_observed_pass_b_far,
              (unsigned long)sm2_texture_result.depth_program_deleted,
              (unsigned long)sm2_texture_result.depth_buffer_deleted,
              (unsigned long)sm2_texture_result.depth_overall,
              (unsigned long)
                  sm2_texture_result.scissor_vertex_decode_status,
              (unsigned long)sm2_texture_result.scissor_vertex_glsl_bytes,
              (unsigned long)
                  sm2_texture_result.scissor_pixel_decode_status,
              (unsigned long)sm2_texture_result.scissor_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.scissor_define_succeeded,
              (unsigned long)
                  sm2_texture_result.scissor_constants_succeeded,
              (unsigned long)sm2_texture_result.scissor_target_bound,
              (unsigned long)sm2_texture_result.scissor_clear_succeeded,
              (unsigned long)sm2_texture_result.scissor_draw_succeeded,
              (unsigned long)sm2_texture_result.scissor_readback_succeeded,
              (unsigned long)sm2_texture_result.scissor_excluded_x,
              (unsigned long)sm2_texture_result.scissor_excluded_y,
              (unsigned long)sm2_texture_result.scissor_included_x,
              (unsigned long)sm2_texture_result.scissor_included_y,
              (unsigned long)
                  sm2_texture_result.scissor_expected_pass_a_excluded,
              (unsigned long)
                  sm2_texture_result.scissor_observed_pass_a_excluded,
              (unsigned long)
                  sm2_texture_result.scissor_expected_pass_a_included,
              (unsigned long)
                  sm2_texture_result.scissor_observed_pass_a_included,
              (unsigned long)
                  sm2_texture_result.scissor_expected_pass_b_excluded,
              (unsigned long)
                  sm2_texture_result.scissor_observed_pass_b_excluded,
              (unsigned long)
                  sm2_texture_result.scissor_expected_pass_b_included,
              (unsigned long)
                  sm2_texture_result.scissor_observed_pass_b_included,
              (unsigned long)sm2_texture_result.scissor_program_deleted,
              (unsigned long)sm2_texture_result.scissor_overall,
              (unsigned long)
                  sm2_texture_result.stream_offset_vertex_decode_status,
              (unsigned long)
                  sm2_texture_result.stream_offset_vertex_glsl_bytes,
              (unsigned long)
                  sm2_texture_result.stream_offset_pixel_decode_status,
              (unsigned long)
                  sm2_texture_result.stream_offset_pixel_glsl_bytes,
              (unsigned long)
                  sm2_texture_result.stream_offset_define_succeeded,
              (unsigned long)
                  sm2_texture_result.stream_offset_constants_succeeded,
              (unsigned long)
                  sm2_texture_result.stream_offset_target_bound,
              (unsigned long)
                  sm2_texture_result.stream_offset_clear_succeeded,
              (unsigned long)
                  sm2_texture_result.stream_offset_draw_succeeded,
              (unsigned long)
                  sm2_texture_result.stream_offset_readback_succeeded,
              (unsigned long)sm2_texture_result.stream_offset_stride,
              (unsigned long)sm2_texture_result.stream_offset_buffer_bytes,
              (unsigned long)
                  sm2_texture_result.stream_offset_pass_a_offset,
              (unsigned long)
                  sm2_texture_result.stream_offset_pass_b_offset,
              (unsigned long)sm2_texture_result.stream_offset_left_x,
              (unsigned long)sm2_texture_result.stream_offset_left_y,
              (unsigned long)sm2_texture_result.stream_offset_right_x,
              (unsigned long)sm2_texture_result.stream_offset_right_y,
              (unsigned long)
                  sm2_texture_result.stream_offset_expected_pass_a_left,
              (unsigned long)
                  sm2_texture_result.stream_offset_observed_pass_a_left,
              (unsigned long)
                  sm2_texture_result.stream_offset_expected_pass_a_right,
              (unsigned long)
                  sm2_texture_result.stream_offset_observed_pass_a_right,
              (unsigned long)
                  sm2_texture_result.stream_offset_expected_pass_b_left,
              (unsigned long)
                  sm2_texture_result.stream_offset_observed_pass_b_left,
              (unsigned long)
                  sm2_texture_result.stream_offset_expected_pass_b_right,
              (unsigned long)
                  sm2_texture_result.stream_offset_observed_pass_b_right,
              (unsigned long)
                  sm2_texture_result.stream_offset_program_deleted,
              (unsigned long)sm2_texture_result.stream_offset_overall,
              sm2_texture_ok ? "ok" : "BAD");
    sm2_texture_detail[sizeof(sm2_texture_detail) - 1] = '\0';
    report("sm2_texture", sm2_texture_detail);

    /* The texldb phase telemetry is reported on its own line so neither this
     * detail nor the already-large sm2_texture detail exceeds report()'s
     * 4096-byte buffer. */
    _snprintf(sm2_texldb_detail, sizeof(sm2_texldb_detail),
              "version=%lu stage=%lu texldb_texture_defined=%lu "
              "texldb_texture_uploaded=%lu texldb_vertex_decode_status=%lu "
              "texldb_vertex_glsl_bytes=%lu texldb_pixel_decode_status=%lu "
              "texldb_pixel_glsl_bytes=%lu texldb_define_succeeded=%lu "
              "texldb_target_bound=%lu texldb_sample_x=%lu "
              "texldb_sample_y=%lu texldb_control_clear=%lu "
              "texldb_control_draw=%lu texldb_control_readback=%lu "
              "texldb_expected_control=0x%08lx texldb_observed_control=0x%08lx "
              "texldb_biased_clear=%lu texldb_biased_draw=%lu "
              "texldb_biased_readback=%lu texldb_expected_biased=0x%08lx "
              "texldb_observed_biased=0x%08lx texldb_program_deleted=%lu "
              "texldb_texture_deleted=%lu texldb_overall=%lu",
              (unsigned long)sm2_texture_result.version,
              (unsigned long)sm2_texture_result.stage,
              (unsigned long)sm2_texture_result.texldb_texture_defined,
              (unsigned long)sm2_texture_result.texldb_texture_uploaded,
              (unsigned long)sm2_texture_result.texldb_vertex_decode_status,
              (unsigned long)sm2_texture_result.texldb_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.texldb_pixel_decode_status,
              (unsigned long)sm2_texture_result.texldb_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.texldb_define_succeeded,
              (unsigned long)sm2_texture_result.texldb_target_bound,
              (unsigned long)sm2_texture_result.texldb_sample_x,
              (unsigned long)sm2_texture_result.texldb_sample_y,
              (unsigned long)sm2_texture_result.texldb_control_clear_succeeded,
              (unsigned long)sm2_texture_result.texldb_control_draw_succeeded,
              (unsigned long)
                  sm2_texture_result.texldb_control_readback_succeeded,
              (unsigned long)sm2_texture_result.texldb_expected_control,
              (unsigned long)sm2_texture_result.texldb_observed_control,
              (unsigned long)sm2_texture_result.texldb_biased_clear_succeeded,
              (unsigned long)sm2_texture_result.texldb_biased_draw_succeeded,
              (unsigned long)
                  sm2_texture_result.texldb_biased_readback_succeeded,
              (unsigned long)sm2_texture_result.texldb_expected_biased,
              (unsigned long)sm2_texture_result.texldb_observed_biased,
              (unsigned long)sm2_texture_result.texldb_program_deleted,
              (unsigned long)sm2_texture_result.texldb_texture_deleted,
              (unsigned long)sm2_texture_result.texldb_overall);
    sm2_texldb_detail[sizeof(sm2_texldb_detail) - 1] = '\0';
    report("sm2_texldb", sm2_texldb_detail);

    /* Keep the appended four-sampler telemetry independently bounded and
     * visible even when the legacy aggregate detail is truncated. */
    _snprintf(sm2_four_sampler_detail, sizeof(sm2_four_sampler_detail),
              "version=%lu stage=%lu four_texture2_defined=%lu "
              "four_texture2_uploaded=%lu four_texture3_defined=%lu "
              "four_texture3_uploaded=%lu four_vertex_decode_status=%lu "
              "four_vertex_glsl_bytes=%lu four_pixel_decode_status=%lu "
              "four_pixel_glsl_bytes=%lu four_define_succeeded=%lu "
              "four_target_bound=%lu four_clear=%lu four_draw=%lu "
              "four_readback=%lu four_expected_sample0=0x%08lx "
              "four_observed_sample0=0x%08lx "
              "four_expected_sample1=0x%08lx "
              "four_observed_sample1=0x%08lx four_program_deleted=%lu "
              "four_texture2_deleted=%lu four_texture3_deleted=%lu "
              "four_overall=%lu",
              (unsigned long)sm2_texture_result.version,
              (unsigned long)sm2_texture_result.stage,
              (unsigned long)sm2_texture_result.four_texture2_defined,
              (unsigned long)sm2_texture_result.four_texture2_uploaded,
              (unsigned long)sm2_texture_result.four_texture3_defined,
              (unsigned long)sm2_texture_result.four_texture3_uploaded,
              (unsigned long)sm2_texture_result.four_vertex_decode_status,
              (unsigned long)sm2_texture_result.four_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.four_pixel_decode_status,
              (unsigned long)sm2_texture_result.four_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.four_define_succeeded,
              (unsigned long)sm2_texture_result.four_target_bound,
              (unsigned long)sm2_texture_result.four_clear_succeeded,
              (unsigned long)sm2_texture_result.four_draw_succeeded,
              (unsigned long)sm2_texture_result.four_readback_succeeded,
              (unsigned long)sm2_texture_result.four_expected_sample0,
              (unsigned long)sm2_texture_result.four_observed_sample0,
              (unsigned long)sm2_texture_result.four_expected_sample1,
              (unsigned long)sm2_texture_result.four_observed_sample1,
              (unsigned long)sm2_texture_result.four_program_deleted,
              (unsigned long)sm2_texture_result.four_texture2_deleted,
              (unsigned long)sm2_texture_result.four_texture3_deleted,
              (unsigned long)sm2_texture_result.four_overall);
    sm2_four_sampler_detail[sizeof(sm2_four_sampler_detail) - 1] = '\0';
    report("sm2_four_sampler", sm2_four_sampler_detail);

    /* Keep the appended volume (3D) telemetry independently bounded and
     * visible even when the legacy aggregate detail is truncated. */
    _snprintf(sm2_volume_detail, sizeof(sm2_volume_detail),
              "version=%lu stage=%lu result_bytes=%lu "
              "volume_texture_defined=%lu volume_texture_uploaded=%lu "
              "volume_vertex_decode_status=%lu "
              "volume_vertex_glsl_bytes=%lu "
              "volume_pixel_decode_status=%lu "
              "volume_pixel_glsl_bytes=%lu volume_define_succeeded=%lu "
              "volume_target_bound=%lu volume_clear_succeeded=%lu "
              "volume_draw_succeeded=%lu volume_readback_succeeded=%lu "
              "volume_sample0_x=%lu volume_sample0_y=%lu "
              "volume_sample1_x=%lu volume_sample1_y=%lu "
              "volume_expected_sample0=0x%08lx "
              "volume_observed_sample0=0x%08lx "
              "volume_expected_sample1=0x%08lx "
              "volume_observed_sample1=0x%08lx "
              "volume_program_deleted=%lu volume_texture_deleted=%lu "
              "volume_overall=%lu",
              (unsigned long)sm2_texture_result.version,
              (unsigned long)sm2_texture_result.stage,
              (unsigned long)sizeof(sm2_texture_result),
              (unsigned long)sm2_texture_result.volume_texture_defined,
              (unsigned long)sm2_texture_result.volume_texture_uploaded,
              (unsigned long)sm2_texture_result.volume_vertex_decode_status,
              (unsigned long)sm2_texture_result.volume_vertex_glsl_bytes,
              (unsigned long)sm2_texture_result.volume_pixel_decode_status,
              (unsigned long)sm2_texture_result.volume_pixel_glsl_bytes,
              (unsigned long)sm2_texture_result.volume_define_succeeded,
              (unsigned long)sm2_texture_result.volume_target_bound,
              (unsigned long)sm2_texture_result.volume_clear_succeeded,
              (unsigned long)sm2_texture_result.volume_draw_succeeded,
              (unsigned long)sm2_texture_result.volume_readback_succeeded,
              (unsigned long)sm2_texture_result.volume_sample0_x,
              (unsigned long)sm2_texture_result.volume_sample0_y,
              (unsigned long)sm2_texture_result.volume_sample1_x,
              (unsigned long)sm2_texture_result.volume_sample1_y,
              (unsigned long)sm2_texture_result.volume_expected_sample0,
              (unsigned long)sm2_texture_result.volume_observed_sample0,
              (unsigned long)sm2_texture_result.volume_expected_sample1,
              (unsigned long)sm2_texture_result.volume_observed_sample1,
              (unsigned long)sm2_texture_result.volume_program_deleted,
              (unsigned long)sm2_texture_result.volume_texture_deleted,
              (unsigned long)sm2_texture_result.volume_overall);
    sm2_volume_detail[sizeof(sm2_volume_detail) - 1] = '\0';
    report("sm2_volume", sm2_volume_detail);

    dxt1_state = selftest_dxt_stage(d3d, device, mode.Format, D3DFMT_DXT1,
                                    "dxt1");
    dxt5_state = selftest_dxt_stage(d3d, device, mode.Format, D3DFMT_DXT5,
                                    "dxt5");
    vertexdecl_state = selftest_vertexdecl_stage(d3d, mode.Format, instance);

    rc = (lifetime_ok && checksum_ok && sm2_ok && sm2_draw_ok &&
          sm2_texture_ok && dxt1_state != QXP_SELFTEST_BAD &&
          dxt5_state != QXP_SELFTEST_BAD &&
          vertexdecl_state != QXP_SELFTEST_BAD) ? 0 : 4;
    {
        char terminal[768];
        int written;

        written = _snprintf(terminal, sizeof(terminal),
                            "QXP_D3D9_SELFTEST_RESULT rc=%d lifetime=%s "
                            "checksum=%s %s sm2=%s sm2_draw=%s "
                            "sm2_texture=%s dxt1=%s dxt5=%s vertexdecl=%s",
                            rc, lifetime_ok ? "ok" : "BAD",
                            checksum_ok ? "ok" : "BAD", detail,
                            sm2_ok ? "ok" : "BAD",
                            sm2_draw_ok ? "ok" : "BAD",
                            sm2_texture_ok ? "ok" : "BAD",
                            selftest_state_name(dxt1_state),
                            selftest_state_name(dxt5_state),
                            selftest_state_name(vertexdecl_state));
        if (written > 0) {
            if (written >= (int)sizeof(terminal))
                written = (int)sizeof(terminal) - 1;
            terminal[written] = '\0';
            qxp_report_serial(terminal);
        }
    }

done:
    if (dc) ReleaseDC(NULL, dc);
    if (vertex_buffer) IDirect3DVertexBuffer9_Release(vertex_buffer);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    if (window) DestroyWindow(window);
    return rc;
}
