#include "qxpdisp.h"

ULONG APIENTRY DrvEscape(SURFOBJ *surface, ULONG escape, ULONG input_bytes,
                         PVOID input, ULONG output_bytes, PVOID output);

static DRVFN qxp_functions[] = {
    { INDEX_DrvEnablePDEV, (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV, (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV, (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface, (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode, (PFN)DrvAssertMode },
    { INDEX_DrvSetPalette, (PFN)DrvSetPalette },
    { INDEX_DrvGetModes, (PFN)DrvGetModes },
    { INDEX_DrvEscape, (PFN)DrvEscape },
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw, (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw },
};

BOOL APIENTRY DrvEnableDriver(ULONG engine_version, ULONG bytes,
                              DRVENABLEDATA *enable_data)
{
    QxpDebug("QXPDISP E");
    UNREFERENCED_PARAMETER(engine_version);
    if (!enable_data || bytes < sizeof(*enable_data)) {
        QxpDebug("QXPDISP E1");
        return FALSE;
    }
    enable_data->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    enable_data->c = sizeof(qxp_functions) / sizeof(qxp_functions[0]);
    enable_data->pdrvfn = qxp_functions;
    QxpDebug("QXPDISP E+");
    return TRUE;
}
