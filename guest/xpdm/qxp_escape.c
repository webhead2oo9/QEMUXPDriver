#include "qxp_escape.h"

static void zero_bytes(void *destination, qxp_u32 count)
{
    volatile qxp_u8 *bytes = destination;
    qxp_u32 index;

    for (index = 0; index < count; ++index) bytes[index] = 0;
}

qxp_u32 qxp_escape(qxp_u32 escape, qxp_u32 input_bytes,
                   const void *input, qxp_u32 output_bytes, void *output)
{
    static const qxp_u16 driver_name[] = {
        'q', 'x', 'p', 'i', 'c', 'd', 0
    };
    const qxp_u32 *subescape = input;
    QXP_OPENGL_INFO *info;
    qxp_u32 index;

    if (escape == QXP_QUERYESCSUPPORT) {
        return input && input_bytes >= sizeof(*subescape) &&
               *subescape == QXP_OPENGL_GETINFO;
    }
    if (escape != QXP_OPENGL_GETINFO || !output ||
        output_bytes < sizeof(*info)) {
        return 0;
    }
    if (input_bytes &&
        (!input || input_bytes < sizeof(*subescape) ||
         *subescape != QXP_OPENGL_GETINFO_DRVNAME)) {
        return 0;
    }

    info = output;
    zero_bytes(info, sizeof(*info));
    info->version = QXP_ICD_INTERFACE_VERSION;
    info->driver_version = QXP_ICD_DRIVER_VERSION;
    for (index = 0; index < sizeof(driver_name) / sizeof(driver_name[0]);
         ++index) {
        info->driver_name[index] = driver_name[index];
    }
    return 1;
}
