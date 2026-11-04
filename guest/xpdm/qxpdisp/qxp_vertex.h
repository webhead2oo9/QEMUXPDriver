#ifndef QXP_VERTEX_H
#define QXP_VERTEX_H

#include <windef.h>

#include "../qxp_d3d_transport.h"

typedef struct qxp_d3d_vertex_layout {
    ULONG vertex_bytes;
    ULONG normal_offset;
    ULONG point_size_offset;
    ULONG diffuse_offset;
    ULONG specular_offset;
    ULONG fog_offset;
    ULONG texture_count;
    ULONG texcoord_offset[4];
    ULONG texcoord_dimensions[4];
    BOOL transformed;
    BOOL has_normal;
    BOOL has_point_size;
    BOOL has_diffuse;
    BOOL has_specular;
    BOOL has_fog;
} QXP_D3D_VERTEX_LAYOUT;

BOOL qxp_vertex_decode_layout(ULONG fvf, QXP_D3D_VERTEX_LAYOUT *layout);
void qxp_vertex_copy(QXP_D3D_VERTEX *destination,
                     QXP_D3D_OBJECT_VERTEX_DATA *object_data,
                     const UCHAR *source,
                     const QXP_D3D_VERTEX_LAYOUT *layout);
BOOL qxp_vertex_ring_wrap_required(
    ULONG source_offset, ULONG stride, ULONG vertex_count,
    ULONG element_bytes, ULONG source_bytes);

BOOL qxp_vertex_packet_layout(ULONG vertex_mode, ULONG vertex_count,
                              ULONG *object_data_offset,
                              ULONG *hwtl_state_offset,
                              ULONG *request_bytes);
BOOL qxp_vertex_compact_supported(const QXP_D3D_VERTEX_LAYOUT *layout);
BOOL qxp_vertex_compact_source_valid(
    const UCHAR *source, const QXP_D3D_VERTEX_LAYOUT *layout);
BOOL qxp_vertex_raw_indexed_object_supported(
    const QXP_D3D_VERTEX_LAYOUT *layout);
BOOL qxp_vertex_raw_indexed_object_source_valid(
    const UCHAR *source, const QXP_D3D_VERTEX_LAYOUT *layout);
BOOL qxp_vertex_compact_packet_layout(ULONG vertex_stride,
                                      ULONG vertex_count,
                                      ULONG *request_bytes);
BOOL qxp_vertex_compact_indexed_packet_layout(
    ULONG vertex_stride, ULONG vertex_count, ULONG index_count,
    ULONG *request_bytes);
BOOL qxp_vertex_raw_indexed_object_packet_layout(
    ULONG vertex_stride, ULONG vertex_count, ULONG index_count,
    ULONG *hwtl_state_offset, ULONG *request_bytes);
BOOL qxp_vertex_indexed_packet_layout(
    ULONG vertex_mode, ULONG vertex_count, ULONG index_count,
    ULONG *object_data_offset, ULONG *hwtl_state_offset,
    ULONG *request_bytes);
#define QXP_VERTEX_COMPACT_MAP_SLOTS 512UL
#define QXP_VERTEX_COMPACT_MAP_EMPTY 0xffffffffUL

void qxp_vertex_compact_index_map_reset(ULONG *map_vertices);
BOOL qxp_vertex_compact_index_map(
    ULONG vertex_index, ULONG *unique_count, BOOL add,
    UCHAR *mapped_index, ULONG *map_vertices, UCHAR *map_indices);
BOOL qxp_vertex_compact_indexed_densify(
    UCHAR *index_data, ULONG index_count, UCHAR *vertex_data,
    ULONG vertex_stride, ULONG vertex_count,
    const UCHAR *vertex_written, ULONG *dense_vertex_count);

#endif
