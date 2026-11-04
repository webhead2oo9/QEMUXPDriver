#include "qxpdisp.h"

typedef struct qxp_modes {
    VIDEO_MODE_INFORMATION *data;
    ULONG count;
    ULONG stride;
} QXP_MODES;

static void qxp_zero(void *destination, ULONG count)
{
    volatile BYTE *bytes = destination;
    ULONG index;

    for (index = 0; index < count; ++index) bytes[index] = 0;
}

static void qxp_copy(void *destination, const void *source, ULONG count)
{
    volatile BYTE *out = destination;
    const volatile BYTE *in = source;
    ULONG index;

    for (index = 0; index < count; ++index) out[index] = in[index];
}

static VIDEO_MODE_INFORMATION *qxp_mode_at(const QXP_MODES *modes,
                                            ULONG index)
{
    return (VIDEO_MODE_INFORMATION *)
        ((BYTE *)modes->data + index * modes->stride);
}

static BOOL qxp_supported_mode(const VIDEO_MODE_INFORMATION *mode)
{
    ULONG bits = mode->NumberOfPlanes * mode->BitsPerPlane;

    return mode->Length >= sizeof(*mode) && mode->NumberOfPlanes == 1 &&
           (bits == 8 || bits == 16 || bits == 32) &&
           (mode->AttributeFlags & VIDEO_MODE_GRAPHICS) != 0;
}

static BOOL qxp_query_modes(HANDLE miniport, QXP_MODES *modes)
{
    VIDEO_NUM_MODES summary;
    ULONG returned;
    ULONG bytes;

    QxpDebug("QXPDISP Q");
    qxp_zero(modes, sizeof(*modes));
    if (EngDeviceIoControl(miniport, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES,
                           NULL, 0, &summary, sizeof(summary), &returned) != 0 ||
        summary.NumModes == 0 ||
        summary.ModeInformationLength < sizeof(VIDEO_MODE_INFORMATION)) {
        QxpDebug("QXPDISP Q1");
        return FALSE;
    }
    if (summary.NumModes > ((ULONG)-1) / summary.ModeInformationLength) {
        QxpDebug("QXPDISP Q2");
        return FALSE;
    }
    bytes = summary.NumModes * summary.ModeInformationLength;
    modes->data = EngAllocMem(0, bytes, QXPDISP_TAG);
    if (!modes->data) {
        QxpDebug("QXPDISP Q3");
        return FALSE;
    }
    if (EngDeviceIoControl(miniport, IOCTL_VIDEO_QUERY_AVAIL_MODES,
                           NULL, 0, modes->data, bytes, &returned) != 0 ||
        returned < bytes) {
        EngFreeMem(modes->data);
        modes->data = NULL;
        QxpDebug("QXPDISP Q4");
        return FALSE;
    }
    modes->count = summary.NumModes;
    modes->stride = summary.ModeInformationLength;
    QxpDebug("QXPDISP Q+");
    return TRUE;
}

static BOOL qxp_mode_matches(const VIDEO_MODE_INFORMATION *candidate,
                             const DEVMODEW *requested)
{
    ULONG bits = candidate->NumberOfPlanes * candidate->BitsPerPlane;

    if (!qxp_supported_mode(candidate)) return FALSE;
    if (requested->dmPelsWidth &&
        requested->dmPelsWidth != candidate->VisScreenWidth) return FALSE;
    if (requested->dmPelsHeight &&
        requested->dmPelsHeight != candidate->VisScreenHeight) return FALSE;
    if (requested->dmBitsPerPel && requested->dmBitsPerPel != bits) return FALSE;
    if (requested->dmDisplayFrequency > 1 &&
        requested->dmDisplayFrequency != candidate->Frequency) return FALSE;
    return TRUE;
}

static VIDEO_MODE_INFORMATION *qxp_select_mode(const QXP_MODES *modes,
                                                const DEVMODEW *requested)
{
    ULONG index;

    for (index = 0; index < modes->count; ++index) {
        VIDEO_MODE_INFORMATION *candidate = qxp_mode_at(modes, index);
        if (qxp_mode_matches(candidate, requested)) return candidate;
    }
    return NULL;
}

static void qxp_set_face(WCHAR *destination, const WCHAR *source)
{
    ULONG index;

    for (index = 0; index + 1 < LF_FACESIZE && source[index]; ++index) {
        destination[index] = source[index];
    }
    destination[index] = 0;
}

static void qxp_set_font(LOGFONTW *font, LONG height, LONG width, LONG weight,
                         BYTE pitch, const WCHAR *face)
{
    qxp_zero(font, sizeof(*font));
    font->lfHeight = height;
    font->lfWidth = width;
    font->lfWeight = weight;
    font->lfCharSet = ANSI_CHARSET;
    font->lfOutPrecision = OUT_DEFAULT_PRECIS;
    font->lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font->lfQuality = DEFAULT_QUALITY;
    font->lfPitchAndFamily = pitch;
    qxp_set_face(font->lfFaceName, face);
}

static void qxp_fill_caps(const VIDEO_MODE_INFORMATION *mode,
                          const DEVMODEW *requested, GDIINFO *caps,
                          DEVINFO *devinfo)
{
    qxp_zero(caps, sizeof(*caps));
    qxp_zero(devinfo, sizeof(*devinfo));

    caps->ulVersion = GDI_DRIVER_VERSION;
    caps->ulTechnology = DT_RASDISPLAY;
    caps->ulHorzSize = mode->XMillimeter ? mode->XMillimeter : 320;
    caps->ulVertSize = mode->YMillimeter ? mode->YMillimeter : 240;
    caps->ulHorzRes = mode->VisScreenWidth;
    caps->ulVertRes = mode->VisScreenHeight;
    caps->cBitsPixel = mode->BitsPerPlane;
    caps->cPlanes = mode->NumberOfPlanes;
    caps->ulNumColors = mode->BitsPerPlane == 8 ? 20 : (ULONG)-1;
    caps->ulNumPalReg = mode->BitsPerPlane == 8 ? 256 : 0;
    caps->ulLogPixelsX = requested->dmLogPixels ? requested->dmLogPixels : 96;
    caps->ulLogPixelsY = caps->ulLogPixelsX;
    caps->flTextCaps = TC_RA_ABLE;
    caps->ulDACRed = mode->NumberRedBits;
    caps->ulDACGreen = mode->NumberGreenBits;
    caps->ulDACBlue = mode->NumberBlueBits;
    caps->ulAspectX = 36;
    caps->ulAspectY = 36;
    caps->ulAspectXY = 51;
    caps->xStyleStep = 1;
    caps->yStyleStep = 1;
    caps->denStyleStep = 3;
    caps->ulPrimaryOrder = PRIMARY_ORDER_CBA;
    caps->ulHTPatternSize = HT_PATSIZE_4x4_M;
    caps->ulHTOutputFormat = mode->BitsPerPlane == 8 ? HT_FORMAT_8BPP :
        (mode->BitsPerPlane == 16 ? HT_FORMAT_16BPP : HT_FORMAT_32BPP);
    caps->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    caps->ulVRefresh = mode->Frequency;
    caps->ulBltAlignment = 1;
    caps->ulPanningHorzRes = mode->VisScreenWidth;
    caps->ulPanningVertRes = mode->VisScreenHeight;

    devinfo->iDitherFormat = mode->BitsPerPlane == 8 ? BMF_8BPP :
        (mode->BitsPerPlane == 16 ? BMF_16BPP : BMF_32BPP);
    devinfo->flGraphicsCaps = GCAPS_OPAQUERECT | GCAPS_WINDINGFILL |
                              GCAPS_GEOMETRICWIDE | GCAPS_DIRECTDRAW;
    if (mode->BitsPerPlane == 8)
        devinfo->flGraphicsCaps |= GCAPS_PALMANAGED;
    devinfo->flGraphicsCaps2 = GCAPS2_RESERVED1;
    devinfo->cxDither = 8;
    devinfo->cyDither = 8;
    qxp_set_font(&devinfo->lfDefaultFont, 16, 7, 700,
                 VARIABLE_PITCH | FF_DONTCARE, L"System");
    qxp_set_font(&devinfo->lfAnsiVarFont, 12, 9, 400,
                 VARIABLE_PITCH | FF_DONTCARE, L"MS Sans Serif");
    qxp_set_font(&devinfo->lfAnsiFixFont, 12, 9, 400,
                 FIXED_PITCH | FF_DONTCARE, L"Courier");
}

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *mode, LPWSTR logical_address,
                              ULONG pattern_count, HSURF *patterns,
                              ULONG caps_bytes, ULONG *caps,
                              ULONG devinfo_bytes, DEVINFO *devinfo,
                              HDEV device, LPWSTR device_name,
                              HANDLE miniport)
{
    QXP_MODES modes;
    VIDEO_MODE_INFORMATION *selected;
    QXP_PDEV *pdev;
    GDIINFO full_caps;
    DEVINFO full_devinfo;
    ULONG copy_bytes;
    ULONG palette[256];
    ULONG index;

    QxpDebug("QXPDISP P");
    UNREFERENCED_PARAMETER(logical_address);
    UNREFERENCED_PARAMETER(pattern_count);
    UNREFERENCED_PARAMETER(patterns);
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(device_name);
    if (!mode || !caps || !devinfo || !qxp_query_modes(miniport, &modes)) {
        QxpDebug("QXPDISP P1");
        return NULL;
    }
    selected = qxp_select_mode(&modes, mode);
    if (!selected) {
        EngFreeMem(modes.data);
        QxpDebug("QXPDISP P2");
        return NULL;
    }
    pdev = EngAllocMem(FL_ZERO_MEMORY, sizeof(*pdev), QXPDISP_TAG);
    if (!pdev) {
        EngFreeMem(modes.data);
        QxpDebug("QXPDISP P3");
        return NULL;
    }
    pdev->miniport = miniport;
    pdev->mode_index = selected->ModeIndex;
    pdev->width = selected->VisScreenWidth;
    pdev->height = selected->VisScreenHeight;
    pdev->stride = selected->ScreenStride;
    pdev->bits_per_pixel = selected->BitsPerPlane * selected->NumberOfPlanes;
    qxp_fill_caps(selected, mode, &full_caps, &full_devinfo);
    if (pdev->bits_per_pixel == 8) {
        for (index = 0; index < 256; ++index) {
            ULONG red = ((index >> 5) & 7) * 255 / 7;
            ULONG green = ((index >> 2) & 7) * 255 / 7;
            ULONG blue = (index & 3) * 255 / 3;
            palette[index] = red | (green << 8) | (blue << 16);
        }
        pdev->default_palette = EngCreatePalette(
            PAL_INDEXED, 256, palette, 0, 0, 0);
    } else {
        pdev->default_palette = EngCreatePalette(
            PAL_BITFIELDS, 0, NULL, selected->RedMask, selected->GreenMask,
            selected->BlueMask);
    }
    if (!pdev->default_palette) {
        EngFreeMem(pdev);
        EngFreeMem(modes.data);
        QxpDebug("QXPDISP P4");
        return NULL;
    }
    full_devinfo.hpalDefault = pdev->default_palette;

    copy_bytes = caps_bytes < sizeof(full_caps) ? caps_bytes : sizeof(full_caps);
    qxp_copy(caps, &full_caps, copy_bytes);
    copy_bytes = devinfo_bytes < sizeof(full_devinfo) ?
                 devinfo_bytes : sizeof(full_devinfo);
    qxp_copy(devinfo, &full_devinfo, copy_bytes);
    EngFreeMem(modes.data);
    QxpDebug("QXPDISP P+");
    return (DHPDEV)pdev;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV device, HDEV engine_device)
{
    QxpDebug("QXPDISP C");
    ((QXP_PDEV *)device)->engine_device = engine_device;
}

VOID APIENTRY DrvDisablePDEV(DHPDEV device)
{
    QXP_PDEV *pdev = (QXP_PDEV *)device;
    BOOL mutating;

    if (!pdev) return;
    mutating = QxpD3d9DrawMemoryMutationBegin();
    if (pdev->default_palette) EngDeletePalette(pdev->default_palette);
    EngFreeMem(pdev);
    QxpD3d9DrawMemoryMutationEnd(mutating);
}

HSURF APIENTRY DrvEnableSurface(DHPDEV device)
{
    QXP_PDEV *pdev = (QXP_PDEV *)device;
    VIDEO_MODE set_mode;
    VIDEO_MEMORY request;
    VIDEO_MEMORY_INFORMATION mapping;
    SIZEL size;
    ULONG returned;
    HSURF surface;

    QxpDebug("QXPDISP S");
    set_mode.RequestedMode = pdev->mode_index;
    if (EngDeviceIoControl(pdev->miniport, IOCTL_VIDEO_SET_CURRENT_MODE,
                           &set_mode, sizeof(set_mode), NULL, 0, &returned) != 0) {
        QxpDebug("QXPDISP S1");
        return NULL;
    }
    request.RequestedVirtualAddress = NULL;
    if (EngDeviceIoControl(pdev->miniport, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
                           &request, sizeof(request), &mapping, sizeof(mapping),
                           &returned) != 0) {
        QxpDebug("QXPDISP S2");
        return NULL;
    }
    pdev->framebuffer = mapping.FrameBufferBase;
    pdev->video_memory_bytes = mapping.VideoRamLength;
    size.cx = (LONG)pdev->width;
    size.cy = (LONG)pdev->height;
    surface = (HSURF)EngCreateBitmap(
        size, pdev->stride,
        pdev->bits_per_pixel == 8 ? BMF_8BPP :
            (pdev->bits_per_pixel == 16 ? BMF_16BPP : BMF_32BPP),
        BMF_TOPDOWN, pdev->framebuffer);
    if (!surface || !EngAssociateSurface(surface, pdev->engine_device, 0)) {
        if (surface) EngDeleteSurface(surface);
        request.RequestedVirtualAddress = pdev->framebuffer;
        EngDeviceIoControl(pdev->miniport, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                           &request, sizeof(request), NULL, 0, &returned);
        pdev->framebuffer = NULL;
        pdev->video_memory_bytes = 0;
        QxpDebug("QXPDISP S3");
        return NULL;
    }
    pdev->engine_surface = surface;
    QxpDebug("QXPDISP S+");
    return surface;
}

VOID APIENTRY DrvDisableSurface(DHPDEV device)
{
    QXP_PDEV *pdev = (QXP_PDEV *)device;
    VIDEO_MEMORY request;
    ULONG returned;
    BOOL mutating;

    if (!pdev) return;
    mutating = QxpD3d9DrawMemoryMutationBegin();

    if (pdev->engine_surface) {
        EngDeleteSurface(pdev->engine_surface);
        pdev->engine_surface = NULL;
    }
    if (pdev->framebuffer) {
        request.RequestedVirtualAddress = pdev->framebuffer;
        EngDeviceIoControl(pdev->miniport, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                           &request, sizeof(request), NULL, 0, &returned);
        pdev->framebuffer = NULL;
        pdev->video_memory_bytes = 0;
    }
    QxpD3d9DrawMemoryMutationEnd(mutating);
}

BOOL APIENTRY DrvAssertMode(DHPDEV device, BOOL enable)
{
    QXP_PDEV *pdev = (QXP_PDEV *)device;
    VIDEO_MODE mode;
    ULONG returned;
    BOOL mutating;
    BOOL result;

    if (!pdev) return FALSE;
    mutating = QxpD3d9DrawMemoryMutationBegin();
    if (!enable) {
        result = EngDeviceIoControl(
            pdev->miniport, IOCTL_VIDEO_RESET_DEVICE,
            NULL, 0, NULL, 0, &returned) == 0;
    } else {
        mode.RequestedMode = pdev->mode_index;
        result = EngDeviceIoControl(
            pdev->miniport, IOCTL_VIDEO_SET_CURRENT_MODE,
            &mode, sizeof(mode), NULL, 0, &returned) == 0;
    }
    QxpD3d9DrawMemoryMutationEnd(mutating);
    return result;
}

BOOL APIENTRY DrvSetPalette(DHPDEV device, PALOBJ *palette, FLONG flags,
                            ULONG start, ULONG count)
{
    QXP_PDEV *pdev = (QXP_PDEV *)device;
    PVIDEO_CLUT clut;
    ULONG bytes;
    ULONG returned;
    ULONG index;
    BOOL success;

    UNREFERENCED_PARAMETER(flags);
    if (!pdev || pdev->bits_per_pixel != 8 || !palette || !count ||
        start >= 256 || count > 256 - start ||
        count > (0xffffffffUL - FIELD_OFFSET(VIDEO_CLUT, LookupTable)) /
                    sizeof(clut->LookupTable[0])) return FALSE;
    bytes = FIELD_OFFSET(VIDEO_CLUT, LookupTable) +
            count * sizeof(clut->LookupTable[0]);
    clut = EngAllocMem(FL_ZERO_MEMORY, bytes, QXPDISP_TAG);
    if (!clut) return FALSE;
    clut->FirstEntry = (USHORT)start;
    clut->NumEntries = (USHORT)count;
    if (PALOBJ_cGetColors(palette, start, count,
                          &clut->LookupTable[0].RgbLong) != count) {
        EngFreeMem(clut);
        return FALSE;
    }
    for (index = 0; index < count; ++index)
        clut->LookupTable[index].RgbArray.Unused = 0;
    success = EngDeviceIoControl(
        pdev->miniport, IOCTL_VIDEO_SET_COLOR_REGISTERS,
        clut, bytes, NULL, 0, &returned) == 0;
    EngFreeMem(clut);
    return success;
}

static void qxp_set_device_name(WCHAR *destination)
{
    static const WCHAR name[] = L"qxpdisp";
    ULONG index;

    for (index = 0; index < sizeof(name) / sizeof(name[0]); ++index) {
        destination[index] = name[index];
    }
}

ULONG APIENTRY DrvGetModes(HANDLE miniport, ULONG bytes, DEVMODEW *output)
{
    QXP_MODES modes;
    ULONG index;
    ULONG required = 0;

    QxpDebug("QXPDISP G");
    if (!qxp_query_modes(miniport, &modes)) return 0;
    for (index = 0; index < modes.count; ++index) {
        VIDEO_MODE_INFORMATION *mode = qxp_mode_at(&modes, index);
        DEVMODEW *entry;

        if (!qxp_supported_mode(mode)) continue;
        if (output && required + sizeof(*entry) <= bytes) {
            entry = (DEVMODEW *)((BYTE *)output + required);
            qxp_zero(entry, sizeof(*entry));
            qxp_set_device_name(entry->dmDeviceName);
            entry->dmSpecVersion = DM_SPECVERSION;
            entry->dmDriverVersion = DM_SPECVERSION;
            entry->dmSize = sizeof(*entry);
            entry->dmBitsPerPel = mode->BitsPerPlane * mode->NumberOfPlanes;
            entry->dmPelsWidth = mode->VisScreenWidth;
            entry->dmPelsHeight = mode->VisScreenHeight;
            entry->dmDisplayFrequency = mode->Frequency;
            entry->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                              DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
        }
        required += sizeof(*entry);
    }
    EngFreeMem(modes.data);
    return required;
}
