#ifndef QXP_SHADER_OWNER_H
#define QXP_SHADER_OWNER_H

typedef struct qxp_shader_owner {
    unsigned long context_handle;
    unsigned long process_id;
    const void *device;
} QXP_SHADER_OWNER;

void qxp_shader_owner_initialize(
    QXP_SHADER_OWNER *owner, unsigned long context_handle,
    unsigned long process_id, const void *device);
int qxp_shader_owner_is_set(const QXP_SHADER_OWNER *owner);
int qxp_shader_owner_matches(
    const QXP_SHADER_OWNER *owner, unsigned long context_handle,
    unsigned long process_id, const void *device);

#endif
