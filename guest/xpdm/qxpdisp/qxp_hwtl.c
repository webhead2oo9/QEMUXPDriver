#include "qxp_hwtl.h"

static void qxp_hwtl_zero(void *destination, ULONG bytes)
{
    volatile UCHAR *output = (volatile UCHAR *)destination;

    while (bytes--) *output++ = 0;
}

static void qxp_hwtl_copy(void *destination, const void *source, ULONG bytes)
{
    volatile UCHAR *output = (volatile UCHAR *)destination;
    const volatile UCHAR *input = (const volatile UCHAR *)source;

    while (bytes--) *output++ = *input++;
}

static void qxp_hwtl_identity(QXP_D3D_MATRIX *matrix)
{
    ULONG index;

    if (!matrix) return;
    qxp_hwtl_zero(matrix, sizeof(*matrix));
    for (index = 0; index < 4; ++index)
        matrix->element[index * 4 + index] = 0x3f800000UL;
}

static BOOL qxp_hwtl_world_index(ULONG transform, ULONG *index)
{
    if (!index) return FALSE;
    if (transform >= QXP_D3D_TRANSFORM_WORLD0 &&
        transform < QXP_D3D_TRANSFORM_WORLD0 +
                    QXP_D3D_MAX_WORLD_TRANSFORMS) {
        *index = transform - QXP_D3D_TRANSFORM_WORLD0;
        return TRUE;
    }
    switch (transform) {
    case QXP_D3D_TRANSFORM_LEGACY_WORLD0: *index = 0; return TRUE;
    case QXP_D3D_TRANSFORM_LEGACY_WORLD1: *index = 1; return TRUE;
    case QXP_D3D_TRANSFORM_LEGACY_WORLD2: *index = 2; return TRUE;
    case QXP_D3D_TRANSFORM_LEGACY_WORLD3: *index = 3; return TRUE;
    }
    return FALSE;
}

static QXP_HWTL_LIGHT_ENTRY *qxp_hwtl_find_light(
    QXP_HWTL_STATE *state, ULONG logical_index)
{
    QXP_HWTL_LIGHT_ENTRY *entry;

    if (!state) return NULL;
    entry = state->lights;
    while (entry && entry->logical_index < logical_index)
        entry = entry->next;
    return entry && entry->logical_index == logical_index ? entry : NULL;
}

static void qxp_hwtl_rebuild_active_lights(QXP_HWTL_STATE *state)
{
    QXP_HWTL_LIGHT_ENTRY *entry;
    ULONG active = 0;

    if (!state) return;
    qxp_hwtl_zero(state->transport.active_lights,
                  sizeof(state->transport.active_lights));
    entry = state->lights;
    while (entry && active < QXP_D3D_MAX_ACTIVE_LIGHTS) {
        if (entry->enabled) {
            state->transport.active_lights[active].logical_index =
                entry->logical_index;
            qxp_hwtl_copy(&state->transport.active_lights[active].light,
                          &entry->light, sizeof(entry->light));
            ++active;
        }
        entry = entry->next;
    }
    state->transport.active_light_count = active;
}

void qxp_hwtl_initialize(QXP_HWTL_STATE *state,
                         QXP_HWTL_ALLOCATE allocate,
                         QXP_HWTL_RELEASE release,
                         void *allocator_context)
{
    ULONG index;

    if (!state) return;
    qxp_hwtl_zero(state, sizeof(*state));
    state->allocate = allocate;
    state->release = release;
    state->allocator_context = allocator_context;
    for (index = 0; index < QXP_D3D_MAX_WORLD_MATRICES; ++index)
        qxp_hwtl_identity(&state->transport.world[index]);
    qxp_hwtl_identity(&state->transport.view);
    qxp_hwtl_identity(&state->transport.projection);
    for (index = 0; index < QXP_D3D_MAX_TEXTURE_MATRICES; ++index)
        qxp_hwtl_identity(&state->transport.texture[index]);
    state->transport.z_min_bits = 0;
    state->transport.z_max_bits = 0x3f800000UL;
    state->transport.lighting_enable = TRUE;
    state->transport.color_vertex_enable = TRUE;
    state->transport.local_viewer = TRUE;
    state->transport.diffuse_material_source = 1;
    state->transport.specular_material_source = 2;
    state->transport.clipping_enable = TRUE;
}

void qxp_hwtl_destroy(QXP_HWTL_STATE *state)
{
    QXP_HWTL_LIGHT_ENTRY *entry;
    QXP_HWTL_LIGHT_ENTRY *next;
    QXP_HWTL_RELEASE release;
    void *allocator_context;

    if (!state) return;
    release = state->release;
    allocator_context = state->allocator_context;
    entry = state->lights;
    while (entry) {
        next = entry->next;
        if (release) release(entry, allocator_context);
        entry = next;
    }
    qxp_hwtl_zero(state, sizeof(*state));
}

BOOL qxp_hwtl_set_transform(QXP_HWTL_STATE *state, ULONG transform,
                            const QXP_D3D_MATRIX *matrix)
{
    QXP_D3D_MATRIX *destination = NULL;
    ULONG world_index;

    if (!state || !matrix) return FALSE;
    if (qxp_hwtl_world_index(transform, &world_index)) {
        /* Software VP initializes the complete 256-entry world namespace.
         * Only the advertised, transported prefix affects hardware draws. */
        if (world_index >= QXP_D3D_MAX_WORLD_MATRICES) return TRUE;
        destination = &state->transport.world[world_index];
    } else if (transform == QXP_D3D_TRANSFORM_VIEW) {
        destination = &state->transport.view;
    } else if (transform == QXP_D3D_TRANSFORM_PROJECTION) {
        destination = &state->transport.projection;
    } else if (transform >= QXP_D3D_TRANSFORM_TEXTURE0 &&
               transform < QXP_D3D_TRANSFORM_TEXTURE0 +
                           QXP_D3D_MAX_TEXTURE_MATRICES) {
        destination = &state->transport.texture[
            transform - QXP_D3D_TRANSFORM_TEXTURE0];
    }
    if (!destination) return FALSE;
    qxp_hwtl_copy(destination, matrix, sizeof(*destination));
    return TRUE;
}

BOOL qxp_hwtl_get_transform(const QXP_HWTL_STATE *state, ULONG transform,
                            QXP_D3D_MATRIX *matrix)
{
    const QXP_D3D_MATRIX *source = NULL;
    ULONG world_index;

    if (!state || !matrix) return FALSE;
    if (qxp_hwtl_world_index(transform, &world_index)) {
        if (world_index >= QXP_D3D_MAX_WORLD_MATRICES) {
            qxp_hwtl_identity(matrix);
            return TRUE;
        }
        source = &state->transport.world[world_index];
    } else if (transform == QXP_D3D_TRANSFORM_VIEW) {
        source = &state->transport.view;
    } else if (transform == QXP_D3D_TRANSFORM_PROJECTION) {
        source = &state->transport.projection;
    } else if (transform >= QXP_D3D_TRANSFORM_TEXTURE0 &&
               transform < QXP_D3D_TRANSFORM_TEXTURE0 +
                           QXP_D3D_MAX_TEXTURE_MATRICES) {
        source = &state->transport.texture[
            transform - QXP_D3D_TRANSFORM_TEXTURE0];
    }
    if (!source) return FALSE;
    qxp_hwtl_copy(matrix, source, sizeof(*matrix));
    return TRUE;
}

BOOL qxp_hwtl_set_clip_plane(QXP_HWTL_STATE *state, ULONG index,
                             const ULONG plane[4])
{
    if (!state || !plane || index >= QXP_D3D_MAX_USER_CLIP_PLANES)
        return FALSE;
    qxp_hwtl_copy(state->transport.clip_planes[index], plane,
                  sizeof(state->transport.clip_planes[index]));
    return TRUE;
}

void qxp_hwtl_set_z_range(QXP_HWTL_STATE *state, ULONG minimum_bits,
                          ULONG maximum_bits)
{
    if (!state) return;
    state->transport.z_min_bits = minimum_bits;
    state->transport.z_max_bits = maximum_bits;
}

void qxp_hwtl_set_material(QXP_HWTL_STATE *state,
                           const QXP_D3D_MATERIAL *material)
{
    if (!state || !material) return;
    qxp_hwtl_copy(&state->transport.material, material, sizeof(*material));
}

BOOL qxp_hwtl_create_light(QXP_HWTL_STATE *state, ULONG logical_index)
{
    QXP_HWTL_LIGHT_ENTRY **link;
    QXP_HWTL_LIGHT_ENTRY *entry;

    if (!state) return FALSE;
    link = &state->lights;
    while (*link && (*link)->logical_index < logical_index)
        link = &(*link)->next;
    if (*link && (*link)->logical_index == logical_index) return TRUE;
    if (!state->allocate) return FALSE;
    entry = (QXP_HWTL_LIGHT_ENTRY *)state->allocate(
        sizeof(*entry), state->allocator_context);
    if (!entry) return FALSE;
    qxp_hwtl_zero(entry, sizeof(*entry));
    entry->logical_index = logical_index;
    entry->next = *link;
    *link = entry;
    return TRUE;
}

BOOL qxp_hwtl_set_light(QXP_HWTL_STATE *state, ULONG logical_index,
                        const QXP_D3D_LIGHT *light)
{
    QXP_HWTL_LIGHT_ENTRY *entry;

    if (!state || !light) return FALSE;
    entry = qxp_hwtl_find_light(state, logical_index);
    if (!entry) {
        if (!qxp_hwtl_create_light(state, logical_index)) return FALSE;
        entry = qxp_hwtl_find_light(state, logical_index);
    }
    qxp_hwtl_copy(&entry->light, light, sizeof(*light));
    if (entry->enabled) qxp_hwtl_rebuild_active_lights(state);
    return TRUE;
}

BOOL qxp_hwtl_enable_light(QXP_HWTL_STATE *state, ULONG logical_index,
                           BOOL enable)
{
    QXP_HWTL_LIGHT_ENTRY *entry;

    if (!state) return FALSE;
    entry = qxp_hwtl_find_light(state, logical_index);
    if (!entry) return FALSE;
    enable = enable != FALSE;
    if (entry->enabled == enable) return TRUE;
    if (enable && state->transport.active_light_count >=
                  QXP_D3D_MAX_ACTIVE_LIGHTS) return FALSE;
    entry->enabled = enable;
    qxp_hwtl_rebuild_active_lights(state);
    return TRUE;
}

BOOL qxp_hwtl_get_light(const QXP_HWTL_STATE *state, ULONG logical_index,
                        QXP_D3D_LIGHT *light, BOOL *enabled)
{
    const QXP_HWTL_LIGHT_ENTRY *entry;

    if (!state || !light || !enabled) return FALSE;
    entry = state->lights;
    while (entry && entry->logical_index < logical_index)
        entry = entry->next;
    if (!entry || entry->logical_index != logical_index) return FALSE;
    qxp_hwtl_copy(light, &entry->light, sizeof(*light));
    *enabled = entry->enabled;
    return TRUE;
}
