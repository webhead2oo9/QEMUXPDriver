#ifndef QXP_HWTL_H
#define QXP_HWTL_H

#include <windef.h>

#include "../qxp_d3d_transport.h"

typedef void *(*QXP_HWTL_ALLOCATE)(ULONG bytes, void *opaque);
typedef void (*QXP_HWTL_RELEASE)(void *allocation, void *opaque);

typedef struct qxp_hwtl_light_entry {
    struct qxp_hwtl_light_entry *next;
    ULONG logical_index;
    BOOL enabled;
    QXP_D3D_LIGHT light;
} QXP_HWTL_LIGHT_ENTRY;

typedef struct qxp_hwtl_state {
    QXP_D3D_HWTL_STATE transport;
    QXP_HWTL_LIGHT_ENTRY *lights;
    QXP_HWTL_ALLOCATE allocate;
    QXP_HWTL_RELEASE release;
    void *allocator_context;
} QXP_HWTL_STATE;

void qxp_hwtl_initialize(QXP_HWTL_STATE *state,
                         QXP_HWTL_ALLOCATE allocate,
                         QXP_HWTL_RELEASE release,
                         void *allocator_context);
void qxp_hwtl_destroy(QXP_HWTL_STATE *state);

BOOL qxp_hwtl_set_transform(QXP_HWTL_STATE *state, ULONG transform,
                            const QXP_D3D_MATRIX *matrix);
BOOL qxp_hwtl_get_transform(const QXP_HWTL_STATE *state, ULONG transform,
                            QXP_D3D_MATRIX *matrix);
BOOL qxp_hwtl_set_clip_plane(QXP_HWTL_STATE *state, ULONG index,
                             const ULONG plane[4]);
void qxp_hwtl_set_z_range(QXP_HWTL_STATE *state, ULONG minimum_bits,
                          ULONG maximum_bits);
void qxp_hwtl_set_material(QXP_HWTL_STATE *state,
                           const QXP_D3D_MATERIAL *material);

BOOL qxp_hwtl_create_light(QXP_HWTL_STATE *state, ULONG logical_index);
BOOL qxp_hwtl_set_light(QXP_HWTL_STATE *state, ULONG logical_index,
                        const QXP_D3D_LIGHT *light);
BOOL qxp_hwtl_enable_light(QXP_HWTL_STATE *state, ULONG logical_index,
                           BOOL enable);
BOOL qxp_hwtl_get_light(const QXP_HWTL_STATE *state, ULONG logical_index,
                        QXP_D3D_LIGHT *light, BOOL *enabled);

#endif
