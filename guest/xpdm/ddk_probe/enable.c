#include <stddef.h>

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#pragma warning(pop)

ULONG APIENTRY DrvEscape(SURFOBJ *surface, ULONG escape, ULONG input_bytes,
                         PVOID input, ULONG output_bytes, PVOID output);

static DRVFN driver_functions[] = {
    { INDEX_DrvEscape, (PFN)DrvEscape },
};

BOOL APIENTRY DrvEnableDriver(ULONG engine_version, ULONG bytes,
                              PDRVENABLEDATA enable_data)
{
    UNREFERENCED_PARAMETER(engine_version);
    if (bytes < sizeof(*enable_data)) return FALSE;
    enable_data->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    enable_data->c = sizeof(driver_functions) / sizeof(driver_functions[0]);
    enable_data->pdrvfn = driver_functions;
    return TRUE;
}
