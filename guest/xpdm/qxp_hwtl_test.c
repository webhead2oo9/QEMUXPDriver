#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qxpdisp/qxp_hwtl.h"

static unsigned allocations;
static unsigned releases;

static void *test_allocate(ULONG bytes, void *opaque)
{
    (void)opaque;
    ++allocations;
    return calloc(1, bytes);
}

static void test_release(void *allocation, void *opaque)
{
    (void)opaque;
    ++releases;
    free(allocation);
}

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "qxp_hwtl_test: %s\n", message);
    return 0;
}

static int matrix_is_identity(const QXP_D3D_MATRIX *matrix)
{
    ULONG row;
    ULONG column;

    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            ULONG expected = row == column ? 0x3f800000UL : 0;
            if (matrix->element[row * 4 + column] != expected) return 0;
        }
    }
    return 1;
}

int main(void)
{
    QXP_HWTL_STATE state;
    QXP_D3D_MATRIX matrix;
    QXP_D3D_MATERIAL material;
    QXP_D3D_LIGHT light;
    ULONG plane[4] = { 1, 2, 3, 4 };
    ULONG index;

    qxp_hwtl_initialize(&state, test_allocate, test_release, NULL);
    if (!require(matrix_is_identity(&state.transport.world[0]),
                 "world matrix default") ||
        !require(matrix_is_identity(&state.transport.world[3]),
                 "world3 matrix default") ||
        !require(matrix_is_identity(&state.transport.view),
                 "view matrix default") ||
        !require(matrix_is_identity(&state.transport.projection),
                 "projection matrix default") ||
        !require(matrix_is_identity(&state.transport.texture[7]),
                 "texture matrix default") ||
        !require(state.transport.z_min_bits == 0 &&
                 state.transport.z_max_bits == 0x3f800000UL,
                 "depth range default") ||
        !require(state.transport.lighting_enable == TRUE &&
                 state.transport.color_vertex_enable == TRUE &&
                 state.transport.local_viewer == TRUE &&
                 state.transport.clipping_enable == TRUE,
                 "boolean defaults") ||
        !require(state.transport.diffuse_material_source == 1 &&
                 state.transport.specular_material_source == 2 &&
                 state.transport.ambient_material_source == 0 &&
                 state.transport.emissive_material_source == 0,
                 "material source defaults")) return 1;

    memset(&matrix, 0, sizeof(matrix));
    matrix.element[7] = 0x41200000UL;
    if (!require(qxp_hwtl_set_transform(
                     &state, QXP_D3D_TRANSFORM_LEGACY_WORLD0, &matrix) &&
                 state.transport.world[0].element[7] == 0x41200000UL,
                 "legacy world transform") ||
        !require(qxp_hwtl_set_transform(
                     &state, QXP_D3D_TRANSFORM_WORLD0 + 3, &matrix) &&
                 state.transport.world[3].element[7] == 0x41200000UL,
                 "world transform") ||
        !require(qxp_hwtl_set_transform(
                     &state, QXP_D3D_TRANSFORM_VIEW, &matrix) &&
                 state.transport.view.element[7] == 0x41200000UL,
                 "view transform") ||
        !require(qxp_hwtl_set_transform(
                     &state, QXP_D3D_TRANSFORM_TEXTURE0 + 7, &matrix) &&
                 state.transport.texture[7].element[7] == 0x41200000UL,
                 "texture transform") ||
        !require(!qxp_hwtl_set_transform(&state, 24, &matrix),
                 "unsupported transform accepted") ||
        !require(qxp_hwtl_set_clip_plane(&state, 5, plane) &&
                 state.transport.clip_planes[5][3] == 4,
                 "clip plane") ||
        !require(!qxp_hwtl_set_clip_plane(
                     &state, QXP_D3D_MAX_USER_CLIP_PLANES, plane),
                 "out of range clip plane accepted")) return 1;

    {
        QXP_D3D_MATRIX ignored_world;

        if (!require(qxp_hwtl_set_transform(
                         &state, QXP_D3D_TRANSFORM_WORLD0 + 4, &matrix),
                     "valid unadvertised world transform rejected") ||
            !require(qxp_hwtl_get_transform(
                         &state, QXP_D3D_TRANSFORM_WORLD0 + 4,
                         &ignored_world) && matrix_is_identity(&ignored_world),
                     "unadvertised world transform not inert") ||
            !require(!qxp_hwtl_set_transform(
                         &state, QXP_D3D_TRANSFORM_WORLD0 +
                                 QXP_D3D_MAX_WORLD_TRANSFORMS,
                         &matrix),
                     "out of range world transform accepted")) return 1;
    }

    memset(&material, 0, sizeof(material));
    material.diffuse.component[2] = 0x3f000000UL;
    qxp_hwtl_set_material(&state, &material);
    qxp_hwtl_set_z_range(&state, 0x3dcccccdUL, 0x3f666666UL);
    if (!require(state.transport.material.diffuse.component[2] ==
                     0x3f000000UL,
                 "material copy") ||
        !require(state.transport.z_min_bits == 0x3dcccccdUL &&
                 state.transport.z_max_bits == 0x3f666666UL,
                 "depth range copy")) return 1;

    memset(&light, 0, sizeof(light));
    light.type = 3;
    light.diffuse.component[0] = 0x3f800000UL;
    if (!require(qxp_hwtl_create_light(&state, 77),
                 "create arbitrary light") ||
        !require(qxp_hwtl_set_light(&state, 77, &light),
                 "set arbitrary light") ||
        !require(qxp_hwtl_enable_light(&state, 77, TRUE),
                 "enable arbitrary light") ||
        !require(state.transport.active_light_count == 1 &&
                 state.transport.active_lights[0].logical_index == 77 &&
                 state.transport.active_lights[0].light.type == 3,
                 "active light snapshot")) return 1;
    {
        QXP_D3D_LIGHT captured_light;
        BOOL captured_enabled = FALSE;
        QXP_D3D_MATRIX captured_matrix;

        if (!require(qxp_hwtl_get_light(
                         &state, 77, &captured_light, &captured_enabled) &&
                     captured_enabled && captured_light.type == 3,
                     "light snapshot query") ||
            !require(qxp_hwtl_get_transform(
                         &state, QXP_D3D_TRANSFORM_TEXTURE0 + 7,
                         &captured_matrix) &&
                     captured_matrix.element[7] == 0x41200000UL,
                     "transform snapshot query") ||
            !require(qxp_hwtl_get_transform(
                         &state, QXP_D3D_TRANSFORM_LEGACY_WORLD3,
                         &captured_matrix) &&
                     captured_matrix.element[7] == 0x41200000UL,
                     "legacy world snapshot query")) return 1;
    }

    light.type = 1;
    if (!require(qxp_hwtl_set_light(&state, 77, &light) &&
                 state.transport.active_lights[0].light.type == 1,
                 "live light update") ||
        !require(qxp_hwtl_enable_light(&state, 77, FALSE) &&
                 state.transport.active_light_count == 0,
                 "disable light")) return 1;

    for (index = 0; index < QXP_D3D_MAX_ACTIVE_LIGHTS + 1; ++index) {
        ULONG logical_index = 1000 + index * 31;
        if (!require(qxp_hwtl_create_light(&state, logical_index),
                     "create bounded light set")) return 1;
        if (index < QXP_D3D_MAX_ACTIVE_LIGHTS) {
            if (!require(qxp_hwtl_enable_light(
                             &state, logical_index, TRUE),
                         "enable bounded light set")) return 1;
        } else if (!require(!qxp_hwtl_enable_light(
                                &state, logical_index, TRUE),
                            "ninth active light accepted")) return 1;
    }
    if (!require(state.transport.active_light_count ==
                     QXP_D3D_MAX_ACTIVE_LIGHTS,
                 "active light maximum")) return 1;
    for (index = 1; index < state.transport.active_light_count; ++index) {
        if (!require(state.transport.active_lights[index - 1].logical_index <
                         state.transport.active_lights[index].logical_index,
                     "active lights not deterministic")) return 1;
    }

    qxp_hwtl_destroy(&state);
    if (!require(allocations == releases, "light allocation leak")) return 1;
    puts("qxp_hwtl_test: ok");
    return 0;
}
