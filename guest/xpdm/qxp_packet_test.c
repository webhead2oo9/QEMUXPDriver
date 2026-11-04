#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windef.h>

#include "qxpmp/qxp_packet.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "qxp_packet_test: %s\n", message);
    return 0;
}

int main(void)
{
    union {
        ULONG align;
        UCHAR bytes[QXP_D3D_MAX_TEXTURE_UPLOAD_BYTES + 64UL];
    } storage;
    QXP_D3D_DRAW_REQUEST *request;
    QXP_D3D_OBJECT_VERTEX_DATA *object_data;
    QXP_D3D_HWTL_STATE *state;
    QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program;
    QXP_D3D_SM2_DRAW_STATE *sm2_draw_state;
    QXP_D3D_COMPACT_VERTEX_PAYLOAD *compact;
    QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *compact_indexed;
    QXP_D3D_INDEXED_VERTEX_PAYLOAD *canonical_indexed;
    QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *raw_indexed_object;
    UCHAR *compact_index_data;
    UCHAR *compact_vertex_data;
    UCHAR *canonical_index_data;
    UCHAR *raw_index_data;
    UCHAR *raw_vertex_data;
    QXP_D3D_VERTEX *canonical_vertices;
    QXP_D3D_DRAW_PACKET_VIEW view;
    QXP_D3D_RAW_OBJECT_FVF_LAYOUT raw_layout;
    QXP_D3D_TEXCOORD_PLAN texcoord_plan;
    QXP_D3D_TEXCOORD_PLAN empty_texcoord_plan;
    QXP_D3D_DRAW_BATCH_REQUEST *batch;
    QXP_D3D_DRAW_BATCH_ENTRY *batch_entry;
    QXP_D3D_DRAW_BATCH_VIEW batch_view;
    QXP_D3D_SURFACE_COPY_REQUEST copy_request;
    QXP_D3D_SURFACE_READBACK_REQUEST readback_request;
    QXP_D3D_PRESENT_REQUEST present_request;
    QXP_D3D_TARGET_REQUEST target_request;
    QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *sm2_define;
    QXP_D3D_SM2_PROGRAM_DELETE_REQUEST sm2_delete;
    QXP_D3D_SM2_CONSTANTS_REQUEST *sm2_constants;
    QXP_D3D_TEXTURE_DEFINE_REQUEST define_request;
    QXP_D3D_TEXTURE_UPLOAD_REQUEST *upload_request;
    ULONG upload_header_bytes;
    ULONG upload_bytes;
    const QXP_D3D_DRAW_REQUEST *batch_draw;
    UCHAR *batch_cursor;
    ULONG header_bytes = sizeof(*request) - sizeof(request->vertices);
    ULONG batch_bytes;
    ULONG batch_draw_bytes;
    ULONG batch_next_bytes;
    ULONG packet_bytes;
    ULONG saved;
    ULONG vertex;
    ULONG padding;
    ULONG sm2_header_bytes;
    ULONG sm2_bytes;
    ULONG sm2_constants_header_bytes;
    ULONG sm2_constants_bytes;
    UCHAR *sm2_pixel_source;

    memset(&storage, 0, sizeof(storage));
    sm2_define = (QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *)storage.bytes;
    sm2_header_bytes =
        (ULONG)offsetof(QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST, source);
    sm2_define->version = QXP_D3D_TRANSPORT_VERSION;
    sm2_define->process_id = 7;
    sm2_define->program_name = QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;
    sm2_define->vertex_source_bytes = 4;
    sm2_define->pixel_source_bytes = 4;
    sm2_define->source[0] = 'v';
    sm2_define->source[1] = 's';
    sm2_define->source[2] = '\n';
    sm2_define->source[3] = 0;
    sm2_pixel_source = sm2_define->source +
                       sm2_define->vertex_source_bytes;
    sm2_pixel_source[0] = 'p';
    sm2_pixel_source[1] = 's';
    sm2_pixel_source[2] = '\n';
    sm2_pixel_source[3] = 0;
    sm2_bytes = sm2_header_bytes + sm2_define->vertex_source_bytes +
                sm2_define->pixel_source_bytes;
    if (!require(sm2_header_bytes == 32,
                 "SM2 define header wire size") ||
        !require(qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "valid SM2 program define")) return 1;
    sm2_define->sampler_mask = 0x8001UL;
    sm2_define->cube_sampler_mask = 0x8000UL;
    sm2_define->volume_sampler_mask = 1UL;
    if (!require(qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "valid 16-sampler SM2 declaration masks")) return 1;
    sm2_define->cube_sampler_mask = 0x10000UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted out-of-range SM2 cube sampler")) return 1;
    sm2_define->cube_sampler_mask = 0x8002UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted undeclared SM2 cube sampler")) return 1;
    sm2_define->cube_sampler_mask = 0x8001UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted overlapping SM2 cube and volume masks")) return 1;
    sm2_define->sampler_mask = 0;
    sm2_define->cube_sampler_mask = 0;
    sm2_define->volume_sampler_mask = 0;

    --sm2_define->version;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted stale SM2 program version")) return 1;
    ++sm2_define->version;
    sm2_define->process_id = 0;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted zero SM2 program process")) return 1;
    sm2_define->process_id = 7;
    sm2_define->program_name = 0;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted zero SM2 program name")) return 1;
    sm2_define->program_name = QXP_D3D_PIXEL_PROGRAM_NAMESPACE | 9UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted SM2 program in pixel namespace")) return 1;
    sm2_define->program_name = QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;

    sm2_define->vertex_source_bytes = 0;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted zero-length SM2 vertex source")) return 1;
    sm2_define->vertex_source_bytes = 4;
    sm2_define->pixel_source_bytes = 0;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted zero-length SM2 pixel source")) return 1;
    sm2_define->pixel_source_bytes = 4;
    sm2_define->vertex_source_bytes =
        QXP_D3D_SM2_MAX_VERTEX_SOURCE_BYTES + 1UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted oversized SM2 vertex source")) return 1;
    sm2_define->vertex_source_bytes = 4;
    sm2_define->pixel_source_bytes =
        QXP_D3D_SM2_MAX_PIXEL_SOURCE_BYTES + 1UL;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted oversized SM2 pixel source")) return 1;
    sm2_define->pixel_source_bytes = 4;

    sm2_define->source[sm2_define->vertex_source_bytes - 1] = 'x';
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted unterminated SM2 vertex source")) return 1;
    sm2_define->source[sm2_define->vertex_source_bytes - 1] = 0;
    sm2_pixel_source[sm2_define->pixel_source_bytes - 1] = 'x';
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted unterminated SM2 pixel source")) return 1;
    sm2_pixel_source[sm2_define->pixel_source_bytes - 1] = 0;

    sm2_define->vertex_source_bytes = 0xffffffffUL;
    sm2_define->pixel_source_bytes = 2;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes),
                 "accepted overflowing SM2 source lengths")) return 1;
    sm2_define->vertex_source_bytes = 4;
    sm2_define->pixel_source_bytes = 4;
    if (!require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes - 1),
                 "accepted truncated SM2 source payload") ||
        !require(!qxp_packet_sm2_program_define_contract(
                     sm2_define, sm2_bytes + 1),
                 "accepted trailing SM2 source payload")) return 1;

    memset(&sm2_delete, 0, sizeof(sm2_delete));
    sm2_delete.version = QXP_D3D_TRANSPORT_VERSION;
    sm2_delete.process_id = 7;
    sm2_delete.program_name = QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;
    if (!require(sizeof(sm2_delete) == 12,
                 "SM2 delete wire size") ||
        !require(qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete)),
                 "valid SM2 program delete")) return 1;
    --sm2_delete.version;
    if (!require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete)),
                 "accepted stale SM2 delete version")) return 1;
    ++sm2_delete.version;
    sm2_delete.process_id = 0;
    if (!require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete)),
                 "accepted zero SM2 delete process")) return 1;
    sm2_delete.process_id = 7;
    sm2_delete.program_name = 0;
    if (!require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete)),
                 "accepted zero SM2 delete name")) return 1;
    sm2_delete.program_name = QXP_D3D_PIXEL_PROGRAM_NAMESPACE | 9UL;
    if (!require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete)),
                 "accepted SM2 delete in pixel namespace")) return 1;
    sm2_delete.program_name = QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;
    if (!require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete) - 1),
                 "accepted truncated SM2 delete request") ||
        !require(!qxp_packet_sm2_program_delete_contract(
                     &sm2_delete, sizeof(sm2_delete) + 1),
                 "accepted trailing SM2 delete request")) return 1;

    memset(&storage, 0, sizeof(storage));
    sm2_constants = (QXP_D3D_SM2_CONSTANTS_REQUEST *)storage.bytes;
    sm2_constants_header_bytes = 7UL * sizeof(ULONG);
    sm2_constants->version = QXP_D3D_TRANSPORT_VERSION;
    sm2_constants->process_id = 7;
    sm2_constants->program_name =
        QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;
    sm2_constants->vertex_start_register = 254;
    sm2_constants->vertex_register_count = 2;
    sm2_constants->pixel_start_register = 31;
    sm2_constants->pixel_register_count = 1;
    sm2_constants->data[0] = 0x3f800000UL;
    sm2_constants->data[11] = 0x3e000000UL;
    sm2_constants_bytes = sm2_constants_header_bytes + 12UL * sizeof(ULONG);
    if (!require(sm2_constants_header_bytes == 28,
                 "SM2 constants header wire size") ||
        !require(qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL),
                 "valid SM2 constants update")) return 1;

    --sm2_constants->version;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL),
                 "accepted stale SM2 constants version")) return 1;
    ++sm2_constants->version;
    sm2_constants->process_id = 8;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL),
                 "accepted wrong SM2 constants process")) return 1;
    sm2_constants->process_id = 7;
    sm2_constants->program_name =
        QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 10UL;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL),
                 "accepted unknown SM2 constants program")) return 1;
    sm2_constants->program_name =
        QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 9UL;

    sm2_constants->vertex_start_register = 255;
    sm2_constants->vertex_register_count = 2;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     sm2_constants->program_name),
                 "accepted SM2 vertex constant range overflow")) return 1;
    sm2_constants->vertex_start_register = 0xffffffffUL;
    sm2_constants->vertex_register_count = 1;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     sm2_constants->program_name),
                 "accepted wrapping SM2 vertex constant start")) return 1;
    sm2_constants->vertex_start_register = 254;
    sm2_constants->vertex_register_count = 2;
    sm2_constants->pixel_start_register = 32;
    sm2_constants->pixel_register_count = 1;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     sm2_constants->program_name),
                 "accepted SM2 pixel constant range overflow")) return 1;
    sm2_constants->pixel_start_register = 0xffffffffUL;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     sm2_constants->program_name),
                 "accepted wrapping SM2 pixel constant start")) return 1;
    sm2_constants->pixel_start_register = 31;

    sm2_constants->vertex_register_count = 0;
    sm2_constants->pixel_register_count = 0;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_header_bytes, 7,
                     sm2_constants->program_name),
                 "accepted empty SM2 constants update")) return 1;
    sm2_constants->vertex_start_register = 254;
    sm2_constants->vertex_register_count = 2;
    sm2_constants->pixel_start_register = 31;
    sm2_constants->pixel_register_count = 1;
    if (!require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes - 1UL, 7,
                     sm2_constants->program_name),
                 "accepted truncated SM2 constants payload") ||
        !require(!qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes + 1UL, 7,
                     sm2_constants->program_name),
                 "accepted trailing SM2 constants payload")) return 1;

    sm2_constants->vertex_start_register = 0;
    sm2_constants->vertex_register_count =
        QXP_D3D_SM2_VERTEX_CONSTANT_COUNT;
    sm2_constants->pixel_start_register = 0;
    sm2_constants->pixel_register_count =
        QXP_D3D_SM2_PIXEL_CONSTANT_COUNT;
    sm2_constants_bytes = sm2_constants_header_bytes +
        (QXP_D3D_SM2_VERTEX_CONSTANT_COUNT +
         QXP_D3D_SM2_PIXEL_CONSTANT_COUNT) * 4UL * sizeof(ULONG);
    if (!require(sm2_constants_bytes <= sizeof(storage.bytes),
                 "SM2 constants maximum test buffer") ||
        !require(qxp_packet_sm2_constants_contract(
                     sm2_constants, sm2_constants_bytes, 7,
                     sm2_constants->program_name),
                 "rejected full SM2 constants files")) return 1;

    memset(&target_request, 0, sizeof(target_request));
    target_request.version = QXP_D3D_TRANSPORT_VERSION;
    target_request.process_id = 1;
    target_request.texture_name = 2;
    target_request.width = 800;
    target_request.height = 600;
    target_request.depth_name = 3;
    target_request.depth_width = 800;
    target_request.depth_height = 600;
    target_request.depth_format = 75;
    if (!require(QXP_D3D_TRANSPORT_VERSION == 64UL,
                 "target request transport version") ||
        !require(sizeof(target_request) == 40,
                 "target request wire size") ||
        !require(offsetof(QXP_D3D_TARGET_REQUEST, depth_width) == 24,
                 "target depth width wire offset") ||
        !require(offsetof(QXP_D3D_TARGET_REQUEST, depth_height) == 28,
                 "target depth height wire offset") ||
        !require(offsetof(QXP_D3D_TARGET_REQUEST, depth_format) == 32,
                 "target depth format wire offset") ||
        !require(offsetof(QXP_D3D_TARGET_REQUEST, flags) == 36,
                 "target flags wire offset") ||
        !require(qxp_packet_target_depth_plan(
                     &target_request, 64, 64, 75) ==
                     QXP_PACKET_TARGET_DEPTH_RECREATE,
                 "changed true depth dimensions did not recreate") ||
        !require(qxp_packet_target_depth_plan(
                     &target_request, 800, 600, 75) ==
                     QXP_PACKET_TARGET_DEPTH_REUSE,
                 "matching true depth dimensions did not reuse") ||
        !require(qxp_packet_target_depth_plan(
                     &target_request, 800, 600, 80) ==
                     QXP_PACKET_TARGET_DEPTH_REJECT,
                 "unchanged depth generation accepted format mismatch"))
        return 1;
    target_request.depth_width = 640;
    target_request.depth_height = 480;
    if (!require(qxp_packet_target_depth_plan(
                     &target_request, 800, 600, 75) ==
                     QXP_PACKET_TARGET_DEPTH_REJECT,
                 "depth smaller than target accepted as recreation"))
        return 1;

    memset(&copy_request, 0, sizeof(copy_request));
    copy_request.version = QXP_D3D_TRANSPORT_VERSION;
    copy_request.process_id = 1;
    copy_request.source_texture_name = 2;
    copy_request.source_width = 800;
    copy_request.source_height = 600;
    copy_request.source_format = QXP_D3D_COPY_FORMAT_X8R8G8B8;
    copy_request.source_rect.right = 800;
    copy_request.source_rect.bottom = 600;
    copy_request.destination_texture_name = 3;
    copy_request.destination_width = 512;
    copy_request.destination_height = 512;
    copy_request.destination_format = QXP_D3D_COPY_FORMAT_A8R8G8B8;
    copy_request.destination_rect.right = 512;
    copy_request.destination_rect.bottom = 512;
    copy_request.filter = QXP_D3D_SURFACE_COPY_POINT;
    if (!require(sizeof(copy_request) == 76,
                 "surface copy wire size") ||
        !require(qxp_packet_surface_copy_contract(&copy_request),
                 "valid surface copy")) return 1;

    --copy_request.version;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted stale surface copy version")) return 1;
    ++copy_request.version;
    copy_request.process_id = 0;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted zero surface copy process")) return 1;
    copy_request.process_id = 1;
    copy_request.destination_texture_name =
        copy_request.source_texture_name;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted self surface copy")) return 1;
    copy_request.destination_texture_name = 3;
    copy_request.source_width = 0;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted zero source width")) return 1;
    copy_request.source_width = 800;
    copy_request.destination_height = 2049;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted oversized destination")) return 1;
    copy_request.destination_height = 512;
    copy_request.source_format = QXP_D3D_COPY_FORMAT_R5G6B5;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted incompatible surface copy formats")) return 1;
    copy_request.destination_format = QXP_D3D_COPY_FORMAT_R5G6B5;
    if (!require(qxp_packet_surface_copy_contract(&copy_request),
                 "rejected equal R5G6B5 formats")) return 1;
    copy_request.source_format = QXP_D3D_COPY_FORMAT_X8R8G8B8;
    copy_request.destination_format = QXP_D3D_COPY_FORMAT_A8R8G8B8;
    copy_request.source_rect.left = -1;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted negative source rectangle")) return 1;
    copy_request.source_rect.left = 0;
    copy_request.source_rect.right = 801;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted source rectangle beyond width")) return 1;
    copy_request.source_rect.right = 800;
    copy_request.destination_rect.bottom = 0;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted empty destination rectangle")) return 1;
    copy_request.destination_rect.bottom = 512;
    copy_request.filter = 1;
    if (!require(!qxp_packet_surface_copy_contract(&copy_request),
                 "accepted unsupported surface copy filter")) return 1;
    copy_request.filter = QXP_D3D_SURFACE_COPY_POINT;
    copy_request.source_rect.left = 17;
    copy_request.source_rect.top = 19;
    copy_request.source_rect.right = 117;
    copy_request.source_rect.bottom = 219;
    copy_request.destination_rect.left = 5;
    copy_request.destination_rect.top = 7;
    copy_request.destination_rect.right = 55;
    copy_request.destination_rect.bottom = 107;
    if (!require(qxp_packet_surface_copy_contract(&copy_request),
                 "rejected bounded partial surface copy")) return 1;

    memset(&readback_request, 0, sizeof(readback_request));
    readback_request.version = QXP_D3D_TRANSPORT_VERSION;
    readback_request.process_id = 1;
    readback_request.texture_name = 3;
    readback_request.width = 512;
    readback_request.height = 512;
    readback_request.format = QXP_D3D_COPY_FORMAT_A8R8G8B8;
    readback_request.vidmem_offset = 0x00400000UL;
    readback_request.pitch = 2048;
    if (!require(sizeof(readback_request) == 32,
                 "surface readback wire size") ||
        !require(qxp_packet_surface_readback_contract(&readback_request),
                 "valid surface readback")) return 1;
    --readback_request.version;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted stale surface readback version")) return 1;
    ++readback_request.version;
    readback_request.process_id = 0;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted zero surface readback process")) return 1;
    readback_request.process_id = 1;
    readback_request.texture_name = 0;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted zero surface readback texture")) return 1;
    readback_request.texture_name = 3;
    readback_request.width = 0;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted zero surface readback width")) return 1;
    readback_request.width = 512;
    readback_request.height = 2049;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted oversized surface readback height")) return 1;
    readback_request.height = 512;
    readback_request.format = 24;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted unsupported surface readback format")) return 1;
    readback_request.format = QXP_D3D_COPY_FORMAT_A8R8G8B8;
    readback_request.pitch = 2047;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted pitch below the row size")) return 1;
    readback_request.pitch = 0x00100001UL;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted oversized surface readback pitch")) return 1;
    readback_request.pitch = 2048;
    readback_request.vidmem_offset =
        0xffffffffUL - 512UL * 2048UL + 1UL;
    if (!require(!qxp_packet_surface_readback_contract(&readback_request),
                 "accepted wrapping surface readback offset")) return 1;
    readback_request.vidmem_offset = 0xffffffffUL - 512UL * 2048UL;
    if (!require(qxp_packet_surface_readback_contract(&readback_request),
                 "rejected maximal surface readback offset")) return 1;
    readback_request.vidmem_offset = 0;
    readback_request.format = QXP_D3D_COPY_FORMAT_R5G6B5;
    readback_request.width = 511;
    readback_request.pitch = 1022;
    if (!require(qxp_packet_surface_readback_contract(&readback_request),
                 "rejected tight R5G6B5 surface readback")) return 1;

    memset(&present_request, 0, sizeof(present_request));
    present_request.version = QXP_D3D_TRANSPORT_VERSION;
    present_request.process_id = 1;
    if (!require(sizeof(present_request) == 36,
                 "present request wire size") ||
        !require(qxp_packet_present_contract(&present_request),
                 "valid legacy present request")) return 1;
    --present_request.version;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted stale present version")) return 1;
    ++present_request.version;
    present_request.process_id = 0;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted zero present process")) return 1;
    present_request.process_id = 1;
    present_request.flags = 4;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted unknown present flags")) return 1;
    present_request.flags =
        QXP_D3D_PRESENT_CPU_SOURCE | QXP_D3D_PRESENT_GPU_TEXTURE;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted both present flags")) return 1;
    present_request.flags = 0;
    present_request.width = 800;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted legacy present with payload")) return 1;
    present_request.width = 0;
    present_request.flags = QXP_D3D_PRESENT_GPU_TEXTURE;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted texture present without name")) return 1;
    present_request.texture_name = QXP_D3D_NEUTRAL_TEXTURE_NAME;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted neutral-texture present")) return 1;
    present_request.texture_name = QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted neutral-cube present")) return 1;
    present_request.texture_name = QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted neutral-volume present")) return 1;
    present_request.texture_name = QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted scratch-texture present")) return 1;
    present_request.texture_name = 42;
    if (!require(qxp_packet_present_contract(&present_request),
                 "rejected valid texture present")) return 1;
    present_request.pitch = 3200;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted texture present with memory fields")) return 1;
    present_request.pitch = 0;
    present_request.texture_name = 0;
    present_request.flags = QXP_D3D_PRESENT_CPU_SOURCE;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted cpu present without dimensions")) return 1;
    present_request.width = 800;
    present_request.height = 600;
    present_request.format = QXP_D3D_COPY_FORMAT_X8R8G8B8;
    present_request.vidmem_offset = 0;
    present_request.pitch = 3200;
    if (!require(qxp_packet_present_contract(&present_request),
                 "rejected valid cpu present")) return 1;
    present_request.pitch = 3199;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted cpu present pitch below row size")) return 1;
    present_request.pitch = 3200;
    present_request.width = 2049;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted oversized cpu present width")) return 1;
    present_request.width = 800;
    present_request.format = 24;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted unsupported cpu present format")) return 1;
    present_request.format = QXP_D3D_COPY_FORMAT_R5G6B5;
    present_request.pitch = 1600;
    if (!require(qxp_packet_present_contract(&present_request),
                 "rejected R5G6B5 cpu present")) return 1;
    present_request.texture_name = 7;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted cpu present with texture name")) return 1;
    present_request.texture_name = 0;
    present_request.format = QXP_D3D_COPY_FORMAT_X8R8G8B8;
    present_request.pitch = 3200;
    present_request.vidmem_offset =
        0xffffffffUL - 600UL * 3200UL + 1UL;
    if (!require(!qxp_packet_present_contract(&present_request),
                 "accepted wrapping cpu present offset")) return 1;

    /* --- Texture define contract (ABI 58) --- */
    if (!require(QXP_D3D_TEXTURE_VOLUME == 3UL,
                 "volume texture type value") ||
        !require(sizeof(define_request) == 44,
                 "texture define wire size") ||
        !require(offsetof(QXP_D3D_TEXTURE_DEFINE_REQUEST, depth) == 0x28,
                 "texture define depth wire offset")) return 1;

    memset(&define_request, 0, sizeof(define_request));
    define_request.version = QXP_D3D_TRANSPORT_VERSION;
    define_request.process_id = 7;
    define_request.texture_name = 5;
    define_request.width = 64;
    define_request.height = 64;
    define_request.texture_type = QXP_D3D_TEXTURE_2D;
    define_request.level = 0;
    define_request.total_levels = 1;
    define_request.depth = 1;
    if (!require(qxp_packet_texture_define_contract(&define_request),
                 "valid 2D texture define") ||
        !require(qxp_packet_texture_allocation_contract(
                     &define_request, 4),
                 "valid 2D allocation") ||
        !require(!qxp_packet_texture_allocation_contract(
                     &define_request, 0),
                 "accepted zero-byte texture format")) return 1;
    --define_request.version;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted stale define version")) return 1;
    ++define_request.version;
    define_request.process_id = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted zero define process")) return 1;
    define_request.process_id = 7;
    define_request.texture_name = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted zero define name")) return 1;
    define_request.texture_name = QXP_D3D_NEUTRAL_TEXTURE_NAME;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted neutral define name")) return 1;
    define_request.texture_name = QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted neutral cube define name")) return 1;
    define_request.texture_name = QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted neutral volume define name")) return 1;
    define_request.texture_name = QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted scratch define name")) return 1;
    define_request.texture_name = 5;
    define_request.width = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted zero define width")) return 1;
    define_request.width = QXP_PACKET_TEXTURE_MAX_DIMENSION + 1UL;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted oversized define width")) return 1;
    define_request.width = 64;
    define_request.height = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted zero define height")) return 1;
    define_request.height = 64;
    define_request.total_levels = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted zero define total levels")) return 1;
    define_request.total_levels = QXP_D3D_MAX_TEXTURE_LEVELS + 1UL;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted oversized define total levels")) return 1;
    define_request.total_levels = 1;
    define_request.level = 1;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted define level beyond total")) return 1;
    define_request.level = 0;
    define_request.texture_type = 99;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted unknown define texture type")) return 1;
    define_request.texture_type = QXP_D3D_TEXTURE_2D;
    define_request.face = 1;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted 2D define with face")) return 1;
    define_request.face = 0;
    define_request.depth = 2;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted 2D define with depth")) return 1;
    define_request.depth = 1;

    define_request.texture_type = QXP_D3D_TEXTURE_CUBE;
    define_request.face = 2;
    define_request.width = 32;
    define_request.height = 32;
    define_request.depth = 1;
    if (!require(qxp_packet_texture_define_contract(&define_request),
                 "valid cube texture define")) return 1;
    define_request.face = 6;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted cube define with sixth face")) return 1;
    define_request.face = 2;
    define_request.height = 16;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted non-square cube define")) return 1;
    define_request.height = 32;
    define_request.depth = 2;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted cube define with depth")) return 1;
    define_request.depth = 1;

    define_request.texture_type = QXP_D3D_TEXTURE_VOLUME;
    define_request.face = 0;
    define_request.width = 16;
    define_request.height = 8;
    define_request.depth = 4;
    if (!require(qxp_packet_texture_define_contract(&define_request),
                 "valid volume texture define")) return 1;
    define_request.face = 1;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted volume define with face")) return 1;
    define_request.face = 0;
    define_request.depth = 0;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted volume define with zero depth")) return 1;
    define_request.depth = QXP_PACKET_TEXTURE_MAX_DIMENSION + 1UL;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted oversized volume define depth")) return 1;
    define_request.depth = 4;
    define_request.width = QXP_PACKET_TEXTURE_MAX_DIMENSION + 1UL;
    if (!require(!qxp_packet_texture_define_contract(&define_request),
                 "accepted oversized volume define width")) return 1;
    define_request.width = 16;
    define_request.width = 256;
    define_request.height = 256;
    define_request.depth = 256;
    if (!require(qxp_packet_texture_allocation_contract(
                     &define_request, 4),
                 "rejected exact maximum volume allocation")) return 1;
    define_request.width = 257;
    if (!require(!qxp_packet_texture_allocation_contract(
                     &define_request, 4),
                 "accepted volume allocation above byte ceiling")) return 1;
    define_request.width = QXP_PACKET_TEXTURE_MAX_DIMENSION;
    define_request.height = QXP_PACKET_TEXTURE_MAX_DIMENSION;
    define_request.depth = QXP_PACKET_TEXTURE_MAX_DIMENSION;
    if (!require(!qxp_packet_texture_allocation_contract(
                     &define_request, 4),
                 "accepted excessive maximum-dimension volume")) return 1;
    define_request.width = 16;
    define_request.height = 8;
    define_request.depth = 4;

    /* --- Texture upload contract (ABI 58) --- */
    memset(&storage, 0, sizeof(storage));
    upload_request = (QXP_D3D_TEXTURE_UPLOAD_REQUEST *)storage.bytes;
    upload_header_bytes =
        (ULONG)offsetof(QXP_D3D_TEXTURE_UPLOAD_REQUEST, data);
    if (!require(upload_header_bytes == 56,
                 "texture upload header wire size") ||
        !require(offsetof(QXP_D3D_TEXTURE_UPLOAD_REQUEST, z) == 0x30,
                 "texture upload z wire offset") ||
        !require(offsetof(QXP_D3D_TEXTURE_UPLOAD_REQUEST, depth) == 0x34,
                 "texture upload depth wire offset") ||
        !require(offsetof(QXP_D3D_TEXTURE_UPLOAD_REQUEST, data) == 0x38,
                 "texture upload data wire offset")) return 1;

    upload_request->version = QXP_D3D_TRANSPORT_VERSION;
    upload_request->process_id = 7;
    upload_request->texture_name = 5;
    upload_request->x = 2;
    upload_request->y = 3;
    upload_request->width = 8;
    upload_request->height = 4;
    upload_request->texture_type = QXP_D3D_TEXTURE_2D;
    upload_request->face = 0;
    upload_request->level = 0;
    upload_request->z = 0;
    upload_request->depth = 1;
    upload_request->data_bytes = 8UL * 4UL * 4UL;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "valid 2D texture upload")) return 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes - 1UL, 64, 64, 1, 1, 4),
                 "accepted truncated 2D upload payload") ||
        !require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes + 1UL, 64, 64, 1, 1, 4),
                 "accepted trailing 2D upload payload")) return 1;
    --upload_request->version;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted stale upload version")) return 1;
    ++upload_request->version;
    upload_request->process_id = 0;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted zero upload process")) return 1;
    upload_request->process_id = 7;
    upload_request->texture_name = 0;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted zero upload name")) return 1;
    upload_request->texture_name = 5;
    upload_request->texture_type = 99;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted unknown upload texture type")) return 1;
    upload_request->texture_type = QXP_D3D_TEXTURE_2D;
    upload_request->face = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted 2D upload with face")) return 1;
    upload_request->face = 0;
    upload_request->z = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted 2D upload with z")) return 1;
    upload_request->z = 0;
    upload_request->depth = 2;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted 2D upload with depth")) return 1;
    upload_request->depth = 1;
    upload_request->level = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted upload level beyond count")) return 1;
    upload_request->level = 0;
    upload_request->x = 60;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted upload X box beyond width")) return 1;
    upload_request->x = 2;
    upload_request->y = 62;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 64, 64, 1, 1, 4),
                 "accepted upload Y box beyond height")) return 1;
    upload_request->y = 3;
    upload_request->data_bytes = 8UL * 4UL * 4UL - 4UL;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request,
                     upload_header_bytes + upload_request->data_bytes,
                     64, 64, 1, 1, 4),
                 "accepted mismatched upload byte count")) return 1;
    upload_request->data_bytes = 8UL * 4UL * 4UL;

    upload_request->x = 0;
    upload_request->y = 0;
    upload_request->z = 0;
    upload_request->width = 0x40000000UL;
    upload_request->height = 4;
    upload_request->depth = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes,
                     0xffffffffUL, 64, 1, 1, 4),
                 "accepted overflowing upload dimensions")) return 1;
    upload_request->width = 256;
    upload_request->height = 64;
    upload_request->depth = 1;
    upload_request->data_bytes = 256UL * 64UL * 4UL;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request,
                     upload_header_bytes + upload_request->data_bytes,
                     256, 64, 1, 1, 4),
                 "accepted oversized upload payload")) return 1;

    memset(&storage, 0, sizeof(storage));
    upload_request = (QXP_D3D_TEXTURE_UPLOAD_REQUEST *)storage.bytes;
    upload_request->version = QXP_D3D_TRANSPORT_VERSION;
    upload_request->process_id = 7;
    upload_request->texture_name = 5;
    upload_request->width = 120;
    upload_request->height = 128;
    upload_request->depth = 1;
    upload_request->texture_type = QXP_D3D_TEXTURE_2D;
    upload_request->data_bytes = QXP_D3D_MAX_TEXTURE_UPLOAD_BYTES;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes,
                     120, 128, 1, 1, 4),
                 "rejected exact maximum upload payload")) return 1;

    upload_request->width = 0x20000000UL;
    upload_request->height = 2;
    upload_request->data_bytes = 0;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_header_bytes,
                     0x20000000UL, 2, 1, 1, 4),
                 "accepted upload height multiplication overflow"))
        return 1;
    upload_request->width = 0x10000UL;
    upload_request->height = 0x1000UL;
    upload_request->depth = 4;
    upload_request->texture_type = QXP_D3D_TEXTURE_VOLUME;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_header_bytes,
                     0x10000UL, 0x1000UL, 4, 1, 4),
                 "accepted upload depth multiplication overflow"))
        return 1;

    upload_request->x = 0;
    upload_request->y = 0;
    upload_request->z = 0;
    upload_request->width = 16;
    upload_request->height = 8;
    upload_request->depth = 4;
    upload_request->texture_type = QXP_D3D_TEXTURE_VOLUME;
    upload_request->face = 0;
    upload_request->level = 0;
    upload_request->data_bytes = 16UL * 8UL * 4UL * 4UL;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 16, 8, 4, 1, 4),
                 "valid volume texture upload")) return 1;
    upload_request->z = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 16, 8, 4, 1, 4),
                 "accepted volume upload Z box beyond depth")) return 1;
    upload_request->z = 0;
    upload_request->depth = 0;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 16, 8, 4, 1, 4),
                 "accepted volume upload with zero depth")) return 1;
    upload_request->depth = 4;

    upload_request->x = 1;
    upload_request->y = 1;
    upload_request->z = 1;
    upload_request->width = 7;
    upload_request->height = 3;
    upload_request->depth = 3;
    upload_request->level = 1;
    upload_request->data_bytes = 7UL * 3UL * 3UL * 4UL;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 16, 8, 8, 3, 4),
                 "rejected valid level-one volume sub-box")) return 1;
    upload_request->depth = 4;
    upload_request->data_bytes = 7UL * 3UL * 4UL * 4UL;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 16, 8, 8, 3, 4),
                 "accepted level-one volume box beyond mip depth"))
        return 1;

    memset(&storage, 0, sizeof(storage));
    upload_request = (QXP_D3D_TEXTURE_UPLOAD_REQUEST *)storage.bytes;
    upload_request->version = QXP_D3D_TRANSPORT_VERSION;
    upload_request->process_id = 7;
    upload_request->texture_name = 5;
    upload_request->x = 0;
    upload_request->y = 0;
    upload_request->z = 0;
    upload_request->width = 32;
    upload_request->height = 32;
    upload_request->depth = 1;
    upload_request->texture_type = QXP_D3D_TEXTURE_CUBE;
    upload_request->face = 3;
    upload_request->level = 0;
    upload_request->data_bytes = 32UL * 32UL * 4UL;
    upload_bytes = upload_header_bytes + upload_request->data_bytes;
    if (!require(qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 32, 32, 1, 1, 4),
                 "valid cube texture upload")) return 1;
    upload_request->face = 6;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 32, 32, 1, 1, 4),
                 "accepted cube upload with sixth face")) return 1;
    upload_request->face = 3;
    upload_request->z = 1;
    if (!require(!qxp_packet_texture_upload_contract(
                     upload_request, upload_bytes, 32, 32, 1, 1, 4),
                 "accepted cube upload with z")) return 1;
    upload_request->z = 0;

    /* --- Draw pixel-stage mask contract (ABI 58) --- */
    if (!require(qxp_packet_draw_mask_contract(0, 0, 0, 0),
                 "rejected empty draw mask") ||
        !require(qxp_packet_draw_mask_contract(3, 3, 1, 2),
                 "rejected valid disjoint cube/volume mask") ||
        !require(!qxp_packet_draw_mask_contract(3, 3, 1, 1),
                 "accepted overlapping cube/volume mask") ||
        !require(!qxp_packet_draw_mask_contract(0x10, 0x10, 0, 0),
                 "accepted sampled mask above stage 3") ||
        !require(!qxp_packet_draw_mask_contract(0, 0x10, 0, 0),
                 "accepted coordinate mask above stage 3") ||
        !require(!qxp_packet_draw_mask_contract(1, 1, 0, 0x10),
                 "accepted volume mask above stage 3") ||
        !require(!qxp_packet_draw_mask_contract(1, 0, 0, 0),
                 "accepted sampled outside coordinate mask") ||
        !require(!qxp_packet_draw_mask_contract(1, 1, 2, 0),
                 "accepted cube outside sampled mask") ||
        !require(!qxp_packet_draw_mask_contract(1, 1, 0, 2),
                 "accepted volume outside sampled mask")) return 1;

    /* A fixed/legacy draw may retain the established 2D/cube behavior, but
     * naming an existing volume with a zero volume mask must be rejected.
     * Only an explicitly sampled programmable-volume stage may consume it. */
    if (!require(qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_2D, FALSE, FALSE, FALSE),
                 "rejected fixed 2D texture binding") ||
        !require(qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_CUBE, FALSE, FALSE, FALSE),
                 "rejected fixed cube texture binding") ||
        !require(!qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_VOLUME, FALSE, FALSE, FALSE),
                 "accepted fixed draw naming a volume texture") ||
        !require(qxp_packet_texture_binding_contract(
                     0, TRUE, FALSE, FALSE) &&
                 qxp_packet_texture_binding_contract(
                     0, TRUE, TRUE, FALSE) &&
                 qxp_packet_texture_binding_contract(
                     0, TRUE, FALSE, TRUE),
                 "rejected unbound programmable dummy target") ||
        !require(!qxp_packet_texture_binding_contract(
                     0, TRUE, TRUE, TRUE),
                 "accepted ambiguous unbound programmable target") ||
        !require(qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_VOLUME, TRUE, FALSE, TRUE),
                 "rejected programmable volume texture binding") ||
        !require(!qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_VOLUME, TRUE, FALSE, FALSE),
                 "accepted volume without programmable volume bit") ||
        !require(!qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_2D, TRUE, FALSE, TRUE),
                 "accepted 2D resource as programmable volume") ||
        !require(!qxp_packet_texture_binding_contract(
                     QXP_D3D_TEXTURE_CUBE, TRUE, TRUE, TRUE),
                 "accepted simultaneous cube and volume selection")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    if (!require(sizeof(*request) == 796,
                 "draw request wire size") ||
        !require(offsetof(QXP_D3D_DRAW_REQUEST, pixel_volume_stages) ==
                     0x2bc,
                 "draw pixel volume mask wire offset") ||
        !require(offsetof(QXP_D3D_DRAW_REQUEST, vertices) == 0x2c0,
                 "draw vertices ABI-62 wire offset") ||
        !require(sizeof(QXP_D3D_SM2_DRAW_STATE) == 904,
                 "SM2 draw-state wire size") ||
        !require(offsetof(QXP_D3D_SM2_DRAW_STATE, samplers) == 328,
                 "SM2 sampler bank wire offset")) return 1;
    request->vertex_mode = QXP_D3D_VERTEX_TRANSFORMED;
    request->vertex_count = 3;
    request->fvf = 0x44UL;
    request->pixel_sampled_stages = 3;
    request->pixel_coordinate_stages = 3;
    request->pixel_cube_stages = 1;
    request->pixel_volume_stages = 2;
    packet_bytes = header_bytes +
                   request->vertex_count * sizeof(request->vertices[0]);
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected draw with valid volume mask")) return 1;
    request->fvf |= 0x2000UL;
    request->vertices[0].fog_bits = 0x3f400000UL;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected finite transformed FVF fog")) return 1;
    request->vertices[0].fog_bits = 0x7fc00000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite transformed FVF fog")) return 1;
    request->vertices[0].fog_bits = 0;
    request->fvf &= ~0x2000UL;
    request->pixel_volume_stages = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted draw with overlapping cube/volume mask"))
        return 1;
    request->pixel_volume_stages = 0x10;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted draw with volume mask above stage 3")) return 1;
    request->pixel_volume_stages = 4;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted draw with volume outside sampled")) return 1;
    request->pixel_volume_stages = 2;
    request->pixel_cube_stages = 4;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted draw with cube outside sampled")) return 1;
    request->pixel_cube_stages = 1;

    request->pixel_sampled_stages = 1;
    request->pixel_coordinate_stages = 0;
    request->pixel_cube_stages = 0;
    request->pixel_volume_stages = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted non-SM2 sampled stage without coordinates"))
        return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_OBJECT;
    request->vertex_count = 3;
    request->fvf = 0x112UL;
    request->texture_coord_dimensions[0] = 2;
    request->object_data_offset = header_bytes +
        request->vertex_count * sizeof(request->vertices[0]);
    request->hwtl_state_offset = request->object_data_offset +
        request->vertex_count * sizeof(QXP_D3D_OBJECT_VERTEX_DATA);
    packet_bytes = request->hwtl_state_offset +
                   sizeof(QXP_D3D_HWTL_STATE);
    if (!require(packet_bytes <= sizeof(storage.bytes), "test buffer"))
        return 1;
    object_data = (QXP_D3D_OBJECT_VERTEX_DATA *)
        (storage.bytes + request->object_data_offset);
    state = (QXP_D3D_HWTL_STATE *)
        (storage.bytes + request->hwtl_state_offset);
    state->z_max_bits = 0x3f800000UL;
    for (vertex = 0; vertex < request->vertex_count; ++vertex) {
        request->vertices[vertex].rhw_bits = 0x3f800000UL;
        object_data[vertex].point_size_bits = 0x3f800000UL;
    }

    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid object packet") ||
        !require(view.object_data == object_data && view.hwtl_state == state,
                 "object packet view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short object packet")) return 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "object packet view restoration")) return 1;
    request->fvf |= 0x2000UL;
    request->vertices[0].fog_bits = 0x3f000000UL;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected finite object FVF fog")) return 1;
    request->vertices[0].fog_bits = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite object FVF fog")) return 1;
    request->vertices[0].fog_bits = 0;
    request->fvf &= ~0x2000UL;

    request->texture_coord_index = 0;
    request->texture1_coord_index =
        QXP_D3D_TCI_CAMERASPACEPOSITION | 7UL;
    if (!require(qxp_packet_texcoord_plan(
                     request, &view, 3, &texcoord_plan) &&
                 texcoord_plan.passthrough_stages == 1 &&
                 texcoord_plan.generated_stages == 2 &&
                 texcoord_plan.selectors[1] == 7 &&
                 texcoord_plan.modes[1] ==
                     QXP_D3D_TCI_CAMERASPACEPOSITION,
                 "rejected mixed pass-through/generated TCI plan"))
        return 1;
    saved = request->fvf;
    request->fvf = 0x2UL;
    request->texture_coord_dimensions[0] = 0;
    request->texture_coord_index = QXP_D3D_TCI_CAMERASPACEPOSITION;
    if (!require(qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan) &&
                 texcoord_plan.generated_stages == 1 &&
                 !texcoord_plan.passthrough_stages,
                 "rejected camera position without normal or FVF texcoords"))
        return 1;
    state->lighting_enable = 1;
    if (!require(qxp_packet_object_glsl_required(
                     request, &view, &texcoord_plan, FALSE),
                 "missed lit generated TCI without declared normal") ||
        !require(qxp_packet_object_glsl_required(
                     request, &view, &texcoord_plan, TRUE),
                 "missed generated TCI with internal program"))
        return 1;
    memset(&empty_texcoord_plan, 0, sizeof(empty_texcoord_plan));
    if (!require(qxp_packet_object_glsl_required(
                     request, &view, &empty_texcoord_plan, FALSE),
                 "missed lit declaration without normal or texgen"))
        return 1;
    empty_texcoord_plan.passthrough_stages = 1;
    request->texture_coord_dimensions[0] = 4;
    if (!require(qxp_packet_object_glsl_required(
                     request, &view, &empty_texcoord_plan, TRUE),
                 "missed lit no-normal pass-through 4D containment"))
        return 1;
    request->fvf = 0x52UL;
    state->color_vertex_enable = 1;
    state->diffuse_material_source = 1;
    state->ambient_material_source = 1;
    if (!require(!qxp_packet_object_glsl_required(
                     request, &view, &empty_texcoord_plan, FALSE),
                 "rejected representable ambient/diffuse color material"))
        return 1;
    request->specular_enable = 1;
    state->ambient_material_source = 0;
    state->specular_material_source = 1;
    if (!require(qxp_packet_object_glsl_required(
                     request, &view, &empty_texcoord_plan, FALSE),
                 "missed unrepresentable diffuse/specular color material"))
        return 1;
    request->specular_enable = 0;
    state->diffuse_material_source = 2;
    state->specular_material_source = 0;
    if (!require(qxp_packet_object_glsl_required(
                     request, &view, &empty_texcoord_plan, FALSE),
                 "missed secondary-color material source"))
        return 1;
    request->fvf = 0x2UL;
    request->texture_coord_dimensions[0] = 0;
    state->color_vertex_enable = 0;
    state->diffuse_material_source = 0;
    state->lighting_enable = 0;
    if (!require(!qxp_packet_object_glsl_required(
                     request, &view, &texcoord_plan, FALSE),
                 "selected GLSL for unlit generated TCI without program"))
        return 1;
    request->fvf |= 0x10UL;
    if (!require(!qxp_packet_object_glsl_required(
                     request, &view, &texcoord_plan, FALSE),
                 "selected GLSL for fixed-compatible generated TCI"))
        return 1;
    request->fvf = saved;
    request->texture_coord_dimensions[0] = 2;
    request->texture_coord_index = QXP_D3D_TCI_CAMERASPACENORMAL;
    request->texture1_coord_index =
        QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR;
    if (!require(qxp_packet_texcoord_plan(
                     request, &view, 3, &texcoord_plan) &&
                 texcoord_plan.generated_stages == 3,
                 "rejected camera normal/reflection TCI plan")) return 1;
    request->texture_coord_index =
        QXP_D3D_TCI_CAMERASPACEPOSITION | 8UL;
    if (!require(!qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "accepted generated TCI wrap selector 8")) return 1;
    request->texture_coord_index = 0x00040000UL;
    if (!require(!qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "accepted unsupported generated TCI mode")) return 1;
    request->texture_coord_index = QXP_D3D_TCI_CAMERASPACENORMAL;
    request->fvf &= ~0x10UL;
    if (!require(!qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "accepted generated normal without FVF normal")) return 1;
    request->fvf |= 0x10UL;
    request->vertex_mode = QXP_D3D_VERTEX_TRANSFORMED;
    if (!require(!qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "accepted generated TCI for transformed vertices"))
        return 1;
    request->vertex_mode = QXP_D3D_VERTEX_OBJECT;
    request->texture_coord_index = 1;
    if (!require(!qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "accepted pass-through selector outside FVF")) return 1;
    request->texture_coord_index = 0;
    request->texture1_coord_index = 0x00040000UL;
    if (!require(qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan),
                 "rejected dormant unsupported TCI state")) return 1;
    request->texture1_coord_index = 0;

    request->fvf |= 2UL << 16;
    request->texture_coord_dimensions[0] = 4;
    request->vertices[0].texcoord[0][3] = 0x40000000UL;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected finite 4D object texture coordinate")) return 1;
    request->vertices[0].texcoord[0][3] = 0x7fc00000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite 4D object texture coordinate")) return 1;
    request->vertices[0].texcoord[0][3] = 0;
    request->fvf &= ~(3UL << 16);
    request->texture_coord_dimensions[0] = 2;

    request->point_scale_enable = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted invalid point scale enable")) return 1;
    request->point_scale_enable = 0;

    request->point_sprite_enable = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted invalid point sprite enable")) return 1;
    request->point_sprite_enable = 0;

    saved = request->object_data_offset;
    request->object_data_offset += 4;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted overlapping offsets")) return 1;
    request->object_data_offset = saved;

    saved = request->fvf;
    request->fvf = (request->fvf & ~0x0eUL) | 0x06UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted blended position")) return 1;
    request->fvf = saved;

    object_data[0].normal[1] = 0x7fc00000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite normal")) return 1;
    object_data[0].normal[1] = 0;

    state->world[0].element[5] = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite matrix")) return 1;
    state->world[0].element[5] = 0;

    state->active_light_count = QXP_D3D_MAX_ACTIVE_LIGHTS + 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted excess lights")) return 1;
    state->active_light_count = 0;

    state->texture_transform_flags[0] = 0x100UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted projected transform without count")) return 1;
    state->texture_transform_flags[0] = 0;

    request->fvf |= 3UL << 18;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unused texture format")) return 1;
    request->fvf = saved;

    object_data[0].point_size_bits = 0x3f000000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                  "accepted undersized point")) return 1;
    request->point_scale_enable = 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected positive scaled point")) return 1;
    object_data[0].point_size_bits = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted zero scaled point")) return 1;
    object_data[0].point_size_bits = 0x3f800000UL;
    request->point_scale_enable = 0;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_TRANSFORMED;
    request->vertex_count = 3;
    request->fvf = 0x44UL;
    request->vertices[0].rhw_bits = 0x7fc00000UL;
    packet_bytes = header_bytes +
                   request->vertex_count * sizeof(request->vertices[0]);
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected legacy transformed packet") ||
        !require(!view.object_data && !view.hwtl_state,
                 "transformed packet view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short transformed packet")) return 1;
    request->vertex_mode = 99;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unknown vertex mode")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_COMPACT_TRANSFORMED;
    request->vertex_count = 3;
    request->fvf = 0x1c4UL;
    request->texture_coord_dimensions[0] = 2;
    compact = (QXP_D3D_COMPACT_VERTEX_PAYLOAD *)request->vertices;
    compact->vertex_stride = 32;
    compact->vertex_data_bytes =
        compact->vertex_stride * request->vertex_count;
    packet_bytes = header_bytes + sizeof(*compact) -
                   sizeof(compact->vertex_data) +
                   compact->vertex_data_bytes;
    if (!require(packet_bytes <= sizeof(storage.bytes),
                 "compact test buffer") ||
        !require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid compact transformed packet") ||
        !require(view.compact == compact &&
                 view.compact_vertex_data ==
                     (const UCHAR *)compact->vertex_data &&
                 !view.program && !view.object_data && !view.hwtl_state,
                 "compact packet view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short compact packet")) return 1;

    ++compact->vertex_stride;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched compact stride")) return 1;
    --compact->vertex_stride;
    ++compact->vertex_data_bytes;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched compact byte count")) return 1;
    --compact->vertex_data_bytes;
    request->texture_projected_stages = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted projected compact packet")) return 1;
    request->texture_projected_stages = 0;
    request->texture_coord_dimensions[0] = 3;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched compact texture dimension")) return 1;
    request->texture_coord_dimensions[0] = 2;
    compact->vertex_data[0] = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite compact position")) return 1;
    compact->vertex_data[0] = 0;
    request->fvf |= 3UL << 16;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unsupported compact texture dimension")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_COMPACT_INDEXED_TRANSFORMED;
    request->vertex_count = 3;
    request->fvf = 0x1c4UL;
    request->texture_coord_dimensions[0] = 2;
    compact_indexed =
        (QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)request->vertices;
    compact_indexed->vertex_stride = 32;
    compact_indexed->vertex_count = 2;
    compact_indexed->vertex_data_bytes = 64;
    compact_indexed->index_count = request->vertex_count;
    compact_indexed->index_data_bytes = request->vertex_count;
    compact_index_data = (UCHAR *)compact_indexed->data;
    compact_vertex_data = compact_index_data +
                          QXP_D3D_COMPACT_INDEX_ALIGNMENT;
    compact_index_data[0] = 0;
    compact_index_data[1] = 1;
    compact_index_data[2] = 0;
    ((ULONG *)compact_vertex_data)[0] = 0x3f800000UL;
    ((ULONG *)compact_vertex_data)[8] = 0x40000000UL;
    packet_bytes = header_bytes + sizeof(*compact_indexed) -
                   sizeof(compact_indexed->data) +
                   QXP_D3D_COMPACT_INDEX_ALIGNMENT +
                   compact_indexed->vertex_data_bytes;
    if (!require(packet_bytes <= sizeof(storage.bytes),
                 "compact indexed test buffer") ||
        !require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid compact indexed transformed packet") ||
        !require(view.compact_indexed == compact_indexed &&
                 view.compact_indexed_index_data == compact_index_data &&
                 view.compact_indexed_vertex_data == compact_vertex_data &&
                 !view.compact && !view.program && !view.object_data &&
                 !view.hwtl_state,
                 "compact indexed packet view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short compact indexed packet")) return 1;
    compact_index_data[1] = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted out-of-range compact index")) return 1;
    compact_index_data[1] = 1;
    compact_index_data[1] = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unused compact vertex slot")) return 1;
    compact_index_data[1] = 1;
    for (padding = request->vertex_count;
         padding < QXP_D3D_COMPACT_INDEX_ALIGNMENT; ++padding) {
        compact_index_data[padding] = 1;
        if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                     "accepted nonzero compact index padding")) return 1;
        compact_index_data[padding] = 0;
    }
    ++compact_indexed->index_data_bytes;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched compact index bytes")) return 1;
    --compact_indexed->index_data_bytes;
    ++compact_indexed->vertex_count;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched compact indexed vertex bytes"))
        return 1;
    --compact_indexed->vertex_count;
    ((ULONG *)compact_vertex_data)[0] = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite compact indexed position")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_INDEXED_TRANSFORMED;
    request->vertex_count = 6;
    request->fvf = 0x44UL;
    canonical_indexed =
        (QXP_D3D_INDEXED_VERTEX_PAYLOAD *)request->vertices;
    canonical_indexed->vertex_count = 3;
    canonical_indexed->index_count = request->vertex_count;
    canonical_index_data = (UCHAR *)canonical_indexed->data;
    canonical_vertices = (QXP_D3D_VERTEX *)(canonical_index_data +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT);
    canonical_index_data[0] = 0;
    canonical_index_data[1] = 1;
    canonical_index_data[2] = 2;
    canonical_index_data[3] = 0;
    canonical_index_data[4] = 2;
    canonical_index_data[5] = 1;
    for (vertex = 0; vertex < canonical_indexed->vertex_count; ++vertex)
        canonical_vertices[vertex].rhw_bits = 0x3f800000UL;
    packet_bytes = header_bytes + sizeof(*canonical_indexed) -
        sizeof(canonical_indexed->data) +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT +
        canonical_indexed->vertex_count * sizeof(*canonical_vertices);
    if (!require(packet_bytes <= sizeof(storage.bytes),
                 "canonical indexed test buffer") ||
        !require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid canonical indexed transformed packet") ||
        !require(view.vertices == canonical_vertices &&
                 view.vertex_indices == canonical_index_data &&
                 view.source_vertex_count == 3 &&
                 !view.object_data && !view.hwtl_state &&
                 !view.compact && !view.compact_indexed && !view.program,
                 "canonical indexed transformed view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short canonical indexed packet"))
        return 1;
    canonical_index_data[1] = 3;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted out-of-range canonical index")) return 1;
    canonical_index_data[1] = 1;
    canonical_index_data[2] = 1;
    canonical_index_data[4] = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unused canonical vertex slot")) return 1;
    canonical_index_data[2] = 2;
    canonical_index_data[4] = 2;
    for (padding = request->vertex_count;
         padding < QXP_D3D_COMPACT_INDEX_ALIGNMENT; ++padding) {
        canonical_index_data[padding] = 1;
        if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                     "accepted nonzero canonical index padding")) return 1;
        canonical_index_data[padding] = 0;
    }
    --canonical_indexed->index_count;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched canonical index count")) return 1;
    ++canonical_indexed->index_count;
    ++canonical_indexed->vertex_count;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched canonical vertex count")) return 1;
    --canonical_indexed->vertex_count;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_INDEXED_OBJECT;
    request->vertex_count = 6;
    request->fvf = 0x112UL;
    request->texture_coord_dimensions[0] = 2;
    canonical_indexed =
        (QXP_D3D_INDEXED_VERTEX_PAYLOAD *)request->vertices;
    canonical_indexed->vertex_count = 3;
    canonical_indexed->index_count = request->vertex_count;
    canonical_index_data = (UCHAR *)canonical_indexed->data;
    canonical_vertices = (QXP_D3D_VERTEX *)(canonical_index_data +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT);
    canonical_index_data[0] = 0;
    canonical_index_data[1] = 1;
    canonical_index_data[2] = 2;
    canonical_index_data[3] = 0;
    canonical_index_data[4] = 2;
    canonical_index_data[5] = 1;
    request->object_data_offset = header_bytes +
        sizeof(*canonical_indexed) - sizeof(canonical_indexed->data) +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT +
        canonical_indexed->vertex_count * sizeof(*canonical_vertices);
    request->hwtl_state_offset = request->object_data_offset +
        canonical_indexed->vertex_count * sizeof(*object_data);
    packet_bytes = request->hwtl_state_offset + sizeof(*state);
    object_data = (QXP_D3D_OBJECT_VERTEX_DATA *)
        (storage.bytes + request->object_data_offset);
    state = (QXP_D3D_HWTL_STATE *)
        (storage.bytes + request->hwtl_state_offset);
    state->z_max_bits = 0x3f800000UL;
    for (vertex = 0; vertex < canonical_indexed->vertex_count; ++vertex) {
        canonical_vertices[vertex].rhw_bits = 0x3f800000UL;
        object_data[vertex].point_size_bits = 0x3f800000UL;
    }
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid canonical indexed object packet") ||
        !require(view.vertices == canonical_vertices &&
                 view.vertex_indices == canonical_index_data &&
                 view.source_vertex_count == 3 &&
                 view.object_data == object_data &&
                 view.hwtl_state == state,
                 "canonical indexed object view"))
        return 1;
    request->texture_coord_index = QXP_D3D_TCI_CAMERASPACEPOSITION;
    if (!require(qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan) &&
                 texcoord_plan.generated_stages == 1 &&
                 !texcoord_plan.passthrough_stages,
                 "rejected generated TCI for canonical indexed object"))
        return 1;
    request->texture_coord_index = 0;
    ++request->object_data_offset;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted shifted canonical object data")) return 1;

    if (!require(qxp_packet_raw_indexed_object_fvf_layout(
                     0x142UL, &raw_layout) &&
                 raw_layout.vertex_stride == 24 &&
                 raw_layout.normal_offset == 0xffffffffUL &&
                 raw_layout.diffuse_offset == 12 &&
                 raw_layout.texture_offset == 16,
                 "raw prelit FVF layout") ||
        !require(qxp_packet_raw_indexed_object_fvf_layout(
                     0x152UL, &raw_layout) &&
                 raw_layout.vertex_stride == 36 &&
                 raw_layout.normal_offset == 12 &&
                 raw_layout.diffuse_offset == 24 &&
                 raw_layout.texture_offset == 28,
                 "raw normal prelit FVF layout") ||
        !require(qxp_packet_raw_indexed_object_fvf_layout(
                     0x112UL, &raw_layout) &&
                 raw_layout.vertex_stride == 32 &&
                 raw_layout.normal_offset == 12 &&
                 raw_layout.diffuse_offset == 0xffffffffUL &&
                 raw_layout.texture_offset == 24,
                 "raw uncolored normal prelit FVF layout") ||
        !require(!qxp_packet_raw_indexed_object_fvf_layout(
                     0x1c2UL, &raw_layout),
                 "accepted raw specular FVF layout"))
        return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_RAW_INDEXED_OBJECT;
    request->vertex_count = 3;
    request->fvf = 0x142UL;
    request->texture_coord_dimensions[0] = 2;
    request->texture_coord_index = 0;
    request->texture1_coord_index = 1;
    request->texture2_coord_index = 2;
    request->texture3_coord_index = 3;
    request->pixel_coordinate_stages = 0;
    request->shade_mode = 2;
    raw_indexed_object =
        (QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *)request->vertices;
    raw_indexed_object->vertex_stride = 24;
    raw_indexed_object->vertex_count = 2;
    raw_indexed_object->vertex_data_bytes = 48;
    raw_indexed_object->index_count = request->vertex_count;
    raw_indexed_object->index_data_bytes = request->vertex_count;
    raw_index_data = (UCHAR *)raw_indexed_object->data;
    raw_vertex_data =
        raw_index_data + QXP_D3D_COMPACT_INDEX_ALIGNMENT;
    raw_index_data[0] = 0;
    raw_index_data[1] = 1;
    raw_index_data[2] = 0;
    request->hwtl_state_offset = header_bytes +
        sizeof(*raw_indexed_object) -
        sizeof(raw_indexed_object->data) +
        QXP_D3D_COMPACT_INDEX_ALIGNMENT +
        raw_indexed_object->vertex_data_bytes;
    packet_bytes = request->hwtl_state_offset + sizeof(*state);
    state = (QXP_D3D_HWTL_STATE *)
        (storage.bytes + request->hwtl_state_offset);
    state->z_max_bits = 0x3f800000UL;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid raw indexed object packet") ||
        !require(view.raw_indexed_object == raw_indexed_object &&
                 view.raw_indexed_object_index_data == raw_index_data &&
                 view.raw_indexed_object_vertex_data == raw_vertex_data &&
                 view.source_vertex_count == 2 &&
                 !view.vertices && !view.vertex_indices &&
                 !view.object_data && view.hwtl_state == state,
                 "raw indexed object packet view") ||
        !require(qxp_packet_texcoord_plan(
                     request, &view, 1, &texcoord_plan) &&
                 texcoord_plan.passthrough_stages == 1 &&
                 !texcoord_plan.generated_stages,
                 "raw indexed object texcoord plan") ||
        !require(!qxp_packet_view_draw(
                     request, packet_bytes - 1, &view),
                 "accepted short raw indexed object packet"))
        return 1;
    request->specular_enable = 1;
    request->fog_table_mode = 3;
    request->shade_mode = 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected irrelevant raw fixed-function state"))
        return 1;
    request->specular_enable = 0;
    request->fog_table_mode = 0;
    request->shade_mode = 2;
    raw_index_data[1] = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted out-of-range raw index")) return 1;
    raw_index_data[1] = 1;
    raw_index_data[1] = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unused raw vertex")) return 1;
    raw_index_data[1] = 1;
    raw_index_data[3] = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonzero raw index padding")) return 1;
    raw_index_data[3] = 0;
    state->texture_transform_flags[0] = 2;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected raw texture transform state"))
        return 1;
    state->texture_transform_flags[0] = 0;
    ((ULONG *)raw_vertex_data)[0] = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite raw position")) return 1;
    ((ULONG *)raw_vertex_data)[0] = 0;
    state->lighting_enable = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted lit raw packet")) return 1;
    state->lighting_enable = 0;
    ++request->hwtl_state_offset;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted shifted raw HWTL state")) return 1;
    --request->hwtl_state_offset;
    request->fvf |= 0x80UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unsupported raw FVF")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_PROGRAMMABLE;
    request->vertex_count = 3;
    request->texture_coord_index = 0;
    request->texture1_coord_index = 1;
    request->texture2_coord_index = 2;
    request->texture3_coord_index = 3;
    program = (QXP_D3D_PROGRAM_VERTEX_PAYLOAD *)request->vertices;
    program->program_name = 0x03000001UL;
    program->input_mask = (1UL << 0) | (1UL << 3);
    program->used_inputs = 1UL << 0;
    program->written_outputs = QXP_D3D_VS_OUTPUT_POSITION |
                               QXP_D3D_VS_OUTPUT_COLOR0 |
                               QXP_D3D_VS_OUTPUT_TEXCOORD0 |
                               (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 1);
    program->vertex_stride = 16;
    program->vertex_data_bytes =
        program->vertex_stride * request->vertex_count;
    program->clipping_enable = 1;
    program->clip_plane_enable = 2;
    program->elements[0].format = QXP_D3D_VERTEX_FORMAT_FLOAT3;
    program->elements[0].byte_offset = 0;
    program->elements[3].format = QXP_D3D_VERTEX_FORMAT_D3DCOLOR;
    program->elements[3].byte_offset = 12;
    packet_bytes = header_bytes + sizeof(*program) -
                   sizeof(program->vertex_data) +
                   program->vertex_data_bytes;
    if (!require(packet_bytes <= sizeof(storage.bytes),
                 "programmable test buffer") ||
        !require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "valid programmable packet") ||
        !require(view.program == program &&
                 view.program_vertex_data ==
                     (const UCHAR *)program->vertex_data &&
                 !view.object_data && !view.hwtl_state,
                 "programmable packet view") ||
        !require(!qxp_packet_view_draw(request, packet_bytes - 1, &view),
                 "accepted short programmable packet")) return 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "programmable packet view restoration") ||
        !require(qxp_packet_texcoord_plan(
                     request, &view, 3, &texcoord_plan) &&
                 texcoord_plan.passthrough_stages == 3 &&
                 !texcoord_plan.generated_stages,
                 "rejected canonical programmable TCI plan")) return 1;

    request->texture3_coord_index = 7;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted noncanonical programmable TEXCOORDINDEX"))
        return 1;
    request->texture3_coord_index = 3;

    /* The programmable path accepts the four-stage projected mask but
     * rejects bits above stage 3. */
    request->texture_projected_stages = 3;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected programmable projected mask 3")) return 1;
    request->texture_projected_stages = 0x10;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted programmable projected mask 0x10")) return 1;
    request->texture_projected_stages = 0;

    if (!require(qxp_packet_program_texture_contract(program, 3, 3),
                 "rejected valid programmable texture contract") ||
        !require(!qxp_packet_program_texture_contract(program, 1, 2),
                 "accepted projected stage outside active coordinates") ||
        !require(!qxp_packet_program_texture_contract(program, 4, 0),
                 "accepted active stage without matching oT output") ||
        !require(!qxp_packet_program_texture_contract(program, 0x10, 0),
                 "accepted programmable coordinate stage above 3") ||
        !require(!qxp_packet_program_texture_contract(NULL, 0, 0),
                 "accepted null programmable texture contract")) return 1;

    /* SM2 sampler s# units and pixel-input t# varyings are independent.
     * Sampling s0 while consuming only t3 is valid when the vertex program
     * writes oT3.  The SM2-only snapshot aliases the otherwise-unused
     * VS1.x constant bank instead of growing every legacy draw header. */
    sm2_draw_state = (QXP_D3D_SM2_DRAW_STATE *)program->constants;
    request->sm2_program_name = program->program_name;
    request->pixel_sampled_stages = 1;
    request->pixel_coordinate_stages = 8;
    sm2_draw_state->samplers[0].texture_name = 11;
    sm2_draw_state->samplers[0].texture_type = QXP_D3D_TEXTURE_2D;
    sm2_draw_state->samplers[0].address_u = 1;
    sm2_draw_state->samplers[0].address_v = 1;
    sm2_draw_state->samplers[0].mag_filter = 1;
    sm2_draw_state->samplers[0].min_filter = 1;
    program->written_outputs |= (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 2) |
                                (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3);
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected SM2 s0/t3 packet") ||
        !require(view.sm2_draw_state == sm2_draw_state,
                 "SM2 draw-state packet view") ||
        !require(qxp_packet_program_texture_contract(program, 8, 0),
                 "rejected SM2 t3 written output")) return 1;
    memset(sm2_draw_state->samplers, 0,
           sizeof(sm2_draw_state->samplers));
    request->pixel_sampled_stages = 0x8010UL;
    request->pixel_cube_stages = 0x10UL;
    sm2_draw_state->samplers[4].texture_name = 14;
    sm2_draw_state->samplers[4].texture_type = QXP_D3D_TEXTURE_CUBE;
    sm2_draw_state->samplers[4].address_u = 1;
    sm2_draw_state->samplers[4].address_v = 2;
    sm2_draw_state->samplers[4].mag_filter = 1;
    sm2_draw_state->samplers[4].min_filter = 2;
    sm2_draw_state->samplers[15].texture_name = 25;
    sm2_draw_state->samplers[15].texture_type = QXP_D3D_TEXTURE_2D;
    sm2_draw_state->samplers[15].address_u = 3;
    sm2_draw_state->samplers[15].address_v = 3;
    sm2_draw_state->samplers[15].mag_filter = 2;
    sm2_draw_state->samplers[15].min_filter = 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected SM2 s4/s15 sampler packet")) return 1;
    sm2_draw_state->samplers[15].texture_type = QXP_D3D_TEXTURE_CUBE;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted SM2 sampler type mismatching its mask")) return 1;
    sm2_draw_state->samplers[15].texture_type = QXP_D3D_TEXTURE_2D;
    sm2_draw_state->samplers[15].texture_name = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted missing high SM2 sampler resource")) return 1;
    sm2_draw_state->samplers[15].texture_name = 25;
    sm2_draw_state->samplers[3].texture_name = 99;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonzero inactive SM2 sampler state")) return 1;
    sm2_draw_state->samplers[3].texture_name = 0;
    request->pixel_sampled_stages = 0x10000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted SM2 sampler mask above s15")) return 1;
    /* A fully zeroed record is the unbound form: legal for a programmable
     * sample and resolved to the opaque-black dummy. Any nonzero field
     * makes the same unbound record malformed again. */
    request->pixel_sampled_stages = 0x8010UL;
    memset(sm2_draw_state->samplers, 0,
           sizeof(sm2_draw_state->samplers));
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected fully zeroed unbound SM2 sampler")) return 1;
    sm2_draw_state->samplers[4].mag_filter = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unbound SM2 sampler carrying filter state")) return 1;
    memset(sm2_draw_state->samplers, 0,
           sizeof(sm2_draw_state->samplers));
    request->pixel_sampled_stages = 1;
    request->pixel_cube_stages = 0;
    sm2_draw_state->samplers[0].texture_name = 11;
    sm2_draw_state->samplers[0].texture_type = QXP_D3D_TEXTURE_2D;
    sm2_draw_state->samplers[0].address_u = 1;
    sm2_draw_state->samplers[0].address_v = 1;
    sm2_draw_state->samplers[0].mag_filter = 1;
    sm2_draw_state->samplers[0].min_filter = 1;

    program->written_outputs &= ~(QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3);
    if (!require(!qxp_packet_program_texture_contract(program, 8, 0),
                 "accepted SM2 t3 without written oT3")) return 1;

    request->pixel_coordinate_stages = 0x80;
    program->written_outputs |= QXP_D3D_VS_OUTPUT_TEXCOORD0 << 7;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected SM2 t7 packet") ||
        !require(qxp_packet_program_texture_contract(program, 0x80, 0),
                 "rejected SM2 t7 written output")) return 1;
    program->written_outputs &= ~(QXP_D3D_VS_OUTPUT_TEXCOORD0 << 7);
    program->written_outputs |= QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3;
    request->pixel_coordinate_stages = 8;

    request->pixel_cube_stages = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted SM2 cube stage outside sampled mask")) return 1;
    request->pixel_cube_stages = 0;
    request->pixel_volume_stages = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted SM2 volume stage outside sampled mask")) return 1;
    request->pixel_cube_stages = 1;
    request->pixel_volume_stages = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted overlapping SM2 cube/volume masks")) return 1;
    memset(sm2_draw_state->samplers, 0,
           sizeof(sm2_draw_state->samplers));
    request->sm2_program_name = 0;
    request->pixel_sampled_stages = 0;
    request->pixel_coordinate_stages = 0;
    request->pixel_cube_stages = 0;
    request->pixel_volume_stages = 0;
    program->written_outputs &= ~((QXP_D3D_VS_OUTPUT_TEXCOORD0 << 2) |
                                  (QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3));

    program->input_mask &= ~(1UL << 0);
    program->elements[0].format = 0;
    program->elements[0].byte_offset = 0;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted missing vertex attribute zero")) return 1;
    program->input_mask |= 1UL << 0;
    program->elements[0].format = QXP_D3D_VERTEX_FORMAT_FLOAT3;

    program->used_inputs |= 1UL << 1;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected shader input with zero default")) return 1;
    program->used_inputs &= ~(1UL << 1);

    program->elements[2].byte_offset = 1;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted metadata for absent input")) return 1;
    program->elements[2].byte_offset = 0;

    program->elements[3].format = 99;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted unknown input format")) return 1;
    program->elements[3].format = QXP_D3D_VERTEX_FORMAT_D3DCOLOR;
    program->elements[3].byte_offset = 13;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted input outside vertex stride")) return 1;
    program->elements[3].byte_offset = 12;

    program->written_outputs &= ~QXP_D3D_VS_OUTPUT_POSITION;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted shader without position output")) return 1;
    program->written_outputs |= QXP_D3D_VS_OUTPUT_POSITION;

    program->program_name = QXP_D3D_PIXEL_PROGRAM_NAMESPACE | 1UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted vertex program in pixel namespace")) return 1;
    program->program_name = QXP_D3D_VERTEX_PROGRAM_NAMESPACE | 1UL;

    program->written_outputs |= QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3;
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected sparse texture outputs")) return 1;
    program->written_outputs &=
        ~(QXP_D3D_VS_OUTPUT_TEXCOORD0 << 3);

    program->clip_planes[1][2] = 0x7f800000UL;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted nonfinite programmable clip plane")) return 1;
    program->clip_planes[1][2] = 0;

    ++program->vertex_data_bytes;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted mismatched programmable vertex bytes")) return 1;
    --program->vertex_data_bytes;

    request->fvf = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted programmable FVF")) return 1;
    request->fvf = 0;
    request->texture_coord_dimensions[0] = 2;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted fixed coordinate metadata")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_PROGRAMMABLE;
    request->vertex_count = 3;
    request->texture_coord_index = 0;
    request->texture1_coord_index = 1;
    request->texture2_coord_index = 2;
    request->texture3_coord_index = 3;
    program = (QXP_D3D_PROGRAM_VERTEX_PAYLOAD *)request->vertices;
    program->program_name = 0x03000002UL;
    program->input_mask = 0xffffUL;
    program->used_inputs = 0xffffUL;
    program->written_outputs = QXP_D3D_VS_OUTPUT_POSITION;
    program->vertex_stride = 256;
    program->vertex_data_bytes =
        program->vertex_stride * request->vertex_count;
    for (vertex = 0; vertex < 16; ++vertex) {
        program->elements[vertex].format =
            QXP_D3D_VERTEX_FORMAT_FLOAT4;
        program->elements[vertex].byte_offset = vertex * 16;
    }
    packet_bytes = header_bytes + sizeof(*program) -
                   sizeof(program->vertex_data) +
                   program->vertex_data_bytes;
    if (!require(packet_bytes <= sizeof(storage.bytes),
                 "256-byte programmable test buffer") ||
        !require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "rejected 256-byte programmable stride")) return 1;
    program->vertex_stride = 257;
    program->vertex_data_bytes =
        program->vertex_stride * request->vertex_count;
    packet_bytes = header_bytes + sizeof(*program) -
                   sizeof(program->vertex_data) +
                   program->vertex_data_bytes;
    if (!require(!qxp_packet_view_draw(request, packet_bytes, &view),
                 "accepted 257-byte programmable stride")) return 1;

    memset(&storage, 0, sizeof(storage));
    request = (QXP_D3D_DRAW_REQUEST *)storage.bytes;
    request->vertex_mode = QXP_D3D_VERTEX_PROGRAMMABLE;
    request->vertex_count = 3;
    request->texture_coord_index = 0;
    request->texture1_coord_index = 1;
    request->texture2_coord_index = 2;
    request->texture3_coord_index = 3;
    program = (QXP_D3D_PROGRAM_VERTEX_PAYLOAD *)request->vertices;
    program->program_name = 0x03000002UL;
    program->written_outputs = QXP_D3D_VS_OUTPUT_POSITION;
    packet_bytes = header_bytes + sizeof(*program) -
                   sizeof(program->vertex_data);
    if (!require(qxp_packet_view_draw(request, packet_bytes, &view),
                 "constant-only programmable packet")) return 1;

    memset(&storage, 0, sizeof(storage));
    batch = (QXP_D3D_DRAW_BATCH_REQUEST *)storage.bytes;
    batch->version = QXP_D3D_TRANSPORT_VERSION;
    batch->process_id = 7;
    batch->draw_count = 2;
    batch_cursor = storage.bytes + QXP_D3D_DRAW_BATCH_HEADER_BYTES;
    batch_draw_bytes = header_bytes + sizeof(request->vertices[0]);
    batch_entry = (QXP_D3D_DRAW_BATCH_ENTRY *)batch_cursor;
    batch_entry->request_bytes = batch_draw_bytes;
    request = (QXP_D3D_DRAW_REQUEST *)batch_entry->request;
    request->version = batch->version;
    request->process_id = batch->process_id;
    request->vertex_mode = QXP_D3D_VERTEX_TRANSFORMED;
    request->vertex_count = 1;
    request->fvf = 0x44UL;
    batch_cursor += QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                    batch_draw_bytes;
    batch_entry = (QXP_D3D_DRAW_BATCH_ENTRY *)batch_cursor;
    batch_entry->request_bytes = batch_draw_bytes;
    request = (QXP_D3D_DRAW_REQUEST *)batch_entry->request;
    request->version = batch->version;
    request->process_id = batch->process_id;
    request->vertex_mode = QXP_D3D_VERTEX_TRANSFORMED;
    request->vertex_count = 1;
    request->fvf = 0x44UL;
    batch_cursor += QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                    batch_draw_bytes;
    batch->payload_bytes = (ULONG)(batch_cursor -
        (storage.bytes + QXP_D3D_DRAW_BATCH_HEADER_BYTES));
    batch_bytes = QXP_D3D_DRAW_BATCH_HEADER_BYTES +
                  batch->payload_bytes;

    if (!require(qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "valid draw batch") ||
        !require(qxp_packet_next_batch_draw(
                     &batch_view, &batch_draw, &batch_next_bytes) &&
                 batch_next_bytes == batch_draw_bytes &&
                 batch_draw->process_id == batch->process_id,
                 "first batch draw") ||
        !require(qxp_packet_next_batch_draw(
                     &batch_view, &batch_draw, &batch_next_bytes) &&
                 batch_next_bytes == batch_draw_bytes &&
                 batch_view.remaining == 0,
                 "second batch draw") ||
        !require(!qxp_packet_next_batch_draw(
                     &batch_view, &batch_draw, &batch_next_bytes),
                 "batch iterator overrun") ||
        !require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes - 1, &batch_view),
                 "accepted truncated draw batch")) return 1;

    ++batch->payload_bytes;
    if (!require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "accepted mismatched batch payload bytes")) return 1;
    --batch->payload_bytes;
    batch_entry = (QXP_D3D_DRAW_BATCH_ENTRY *)(
        storage.bytes + QXP_D3D_DRAW_BATCH_HEADER_BYTES);
    batch->draw_count = 1;
    batch->payload_bytes = QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES +
                           sizeof(ULONG);
    batch_entry->request_bytes = sizeof(ULONG);
    if (!require(!qxp_packet_view_draw_batch(
                     batch, QXP_D3D_DRAW_BATCH_HEADER_BYTES +
                            batch->payload_bytes, &batch_view),
                 "accepted undersized nested draw header")) return 1;
    batch_entry->request_bytes = batch_draw_bytes;
    batch->payload_bytes = batch_bytes - QXP_D3D_DRAW_BATCH_HEADER_BYTES;
    batch->draw_count = 0;
    if (!require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "accepted empty draw batch")) return 1;
    batch->draw_count = 2;
    ++batch_entry->request_bytes;
    if (!require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "accepted unaligned batch entry")) return 1;
    --batch_entry->request_bytes;
    request = (QXP_D3D_DRAW_REQUEST *)batch_entry->request;
    ++request->process_id;
    if (!require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "accepted mixed-process draw batch")) return 1;
    --request->process_id;
    request->vertex_mode = 99;
    if (!require(!qxp_packet_view_draw_batch(
                     batch, batch_bytes, &batch_view),
                 "accepted invalid nested draw")) return 1;

    puts("qxp_packet_test: ok");
    return 0;
}
