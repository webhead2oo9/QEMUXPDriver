#include "qxp_shader_owner.h"

void qxp_shader_owner_initialize(
    QXP_SHADER_OWNER *owner, unsigned long context_handle,
    unsigned long process_id, const void *device)
{
    if (!owner) return;
    owner->context_handle = context_handle;
    owner->process_id = process_id;
    owner->device = device;
}

int qxp_shader_owner_is_set(const QXP_SHADER_OWNER *owner)
{
    return owner && owner->context_handle != 0;
}

int qxp_shader_owner_matches(
    const QXP_SHADER_OWNER *owner, unsigned long context_handle,
    unsigned long process_id, const void *device)
{
    return qxp_shader_owner_is_set(owner) && context_handle != 0 &&
           process_id != 0 && device != 0 &&
           owner->context_handle == context_handle &&
           owner->process_id == process_id && owner->device == device;
}
