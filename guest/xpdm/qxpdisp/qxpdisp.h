#ifndef QXPDISP_H
#define QXPDISP_H

#include <stddef.h>

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#pragma warning(pop)

#include "../qxp_d3d_transport.h"

#define QXPDISP_TAG 'DPXQ'

VOID QxpDebug(const char *message);
VOID QxpDebugValue(ULONG value);
VOID QxpDebugRecord(const char *record, const char *event,
                    const ULONG *values, ULONG count);
VOID QxpDebugVerbose(const char *message);
VOID QxpDebugVerboseValue(ULONG value);
BOOL QxpD3d9DrawMemoryMutationBegin(VOID);
VOID QxpD3d9DrawMemoryMutationEnd(BOOL active);

typedef struct qxp_pdev {
    HANDLE miniport;
    HDEV engine_device;
    HSURF engine_surface;
    HPALETTE default_palette;
    PVOID framebuffer;
    ULONG video_memory_bytes;
    ULONG mode_index;
    ULONG width;
    ULONG height;
    LONG stride;
    ULONG bits_per_pixel;
} QXP_PDEV;

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *mode, LPWSTR logical_address,
                              ULONG pattern_count, HSURF *patterns,
                              ULONG caps_bytes, ULONG *caps,
                              ULONG devinfo_bytes, DEVINFO *devinfo,
                              HDEV device, LPWSTR device_name,
                              HANDLE miniport);
VOID APIENTRY DrvCompletePDEV(DHPDEV device, HDEV engine_device);
VOID APIENTRY DrvDisablePDEV(DHPDEV device);
HSURF APIENTRY DrvEnableSurface(DHPDEV device);
VOID APIENTRY DrvDisableSurface(DHPDEV device);
BOOL APIENTRY DrvAssertMode(DHPDEV device, BOOL enable);
BOOL APIENTRY DrvSetPalette(DHPDEV device, PALOBJ *palette, FLONG flags,
                            ULONG start, ULONG count);
ULONG APIENTRY DrvGetModes(HANDLE miniport, ULONG bytes, DEVMODEW *modes);
BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV device, DD_HALINFO *hal_info,
                                   DWORD *heap_count,
                                   VIDEOMEMORY *heaps,
                                   DWORD *fourcc_count,
                                   DWORD *fourcc_codes);
BOOL APIENTRY DrvEnableDirectDraw(DHPDEV device, DD_CALLBACKS *callbacks,
                                  DD_SURFACECALLBACKS *surface_callbacks,
                                  DD_PALETTECALLBACKS *palette_callbacks);
VOID APIENTRY DrvDisableDirectDraw(DHPDEV device);

#endif
