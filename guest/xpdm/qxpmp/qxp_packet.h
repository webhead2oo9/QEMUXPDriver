#ifndef QXP_PACKET_H
#define QXP_PACKET_H

#include "../qxp_d3d_transport.h"


typedef struct qxp_d3d_draw_packet_view {
    const QXP_D3D_OBJECT_VERTEX_DATA *object_data;
    const QXP_D3D_HWTL_STATE *hwtl_state;
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program;
    const QXP_D3D_SM2_DRAW_STATE *sm2_draw_state;
    const UCHAR *program_vertex_data;
    const QXP_D3D_COMPACT_VERTEX_PAYLOAD *compact;
    const UCHAR *compact_vertex_data;
    const QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *compact_indexed;
    /* ABI 47 stores the host-consumed index stream first, then aligned
     * transformed vertices in the same validated packet span. */
    const UCHAR *compact_indexed_index_data;
    const UCHAR *compact_indexed_vertex_data;
    const QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *raw_indexed_object;
    const UCHAR *raw_indexed_object_index_data;
    const UCHAR *raw_indexed_object_vertex_data;
    /* Canonical modes read vertices through this pointer. ABI 63 indexed
     * modes additionally map each expanded corner through vertex_indices. */
    const QXP_D3D_VERTEX *vertices;
    const UCHAR *vertex_indices;
    ULONG source_vertex_count;
    ULONG failure_index;
    ULONG failure_detail;
} QXP_D3D_DRAW_PACKET_VIEW;

typedef struct qxp_d3d_compact_fvf_layout {
    ULONG vertex_stride;
    ULONG texture_count;
    ULONG diffuse_offset;
    ULONG specular_offset;
    ULONG texture_offsets[4];
} QXP_D3D_COMPACT_FVF_LAYOUT;

typedef struct qxp_d3d_raw_object_fvf_layout {
    ULONG vertex_stride;
    ULONG normal_offset;
    ULONG diffuse_offset;
    ULONG texture_offset;
} QXP_D3D_RAW_OBJECT_FVF_LAYOUT;

#define QXP_PACKET_BATCH_FAILURE_NONE 0UL
#define QXP_PACKET_BATCH_FAILURE_HEADER 1UL
#define QXP_PACKET_BATCH_FAILURE_ENTRY_HEADER 2UL
#define QXP_PACKET_BATCH_FAILURE_ENTRY_SIZE 3UL
#define QXP_PACKET_BATCH_FAILURE_DRAW_IDENTITY 4UL
#define QXP_PACKET_BATCH_FAILURE_DRAW_PACKET 5UL
#define QXP_PACKET_BATCH_FAILURE_TRAILING 6UL

typedef struct qxp_d3d_draw_batch_view {
    const UCHAR *cursor;
    ULONG remaining;
    ULONG failure_reason;
    ULONG failure_entry;
    const QXP_D3D_DRAW_REQUEST *failure_draw;
    ULONG failure_draw_bytes;
    ULONG draw_failure_index;
    ULONG draw_failure_detail;
} QXP_D3D_DRAW_BATCH_VIEW;

typedef struct qxp_d3d_texcoord_plan {
    ULONG modes[4];
    ULONG selectors[4];
    ULONG passthrough_stages;
    ULONG generated_stages;
} QXP_D3D_TEXCOORD_PLAN;

#define QXP_PACKET_TARGET_DEPTH_REJECT 0UL
#define QXP_PACKET_TARGET_DEPTH_REUSE 1UL
#define QXP_PACKET_TARGET_DEPTH_RECREATE 2UL

/* Portable ceiling shared by 2D/cube/volume texture width, height, and
 * volume depth.  Matches the miniport's device-independent 2048 clamp. */
#define QXP_PACKET_TEXTURE_MAX_DIMENSION 2048UL
/* One host allocation is bounded independently of per-axis dimensions.
 * 64 MiB admits a 256^3 four-byte volume and every 2048^2 four-byte 2D face
 * while refusing dimension products that can wrap or exhaust host memory. */
#define QXP_PACKET_TEXTURE_MAX_ALLOCATION_BYTES (64UL * 1024UL * 1024UL)

ULONG qxp_packet_target_depth_plan(
    const QXP_D3D_TARGET_REQUEST *request, ULONG existing_width,
    ULONG existing_height, ULONG existing_format);

BOOLEAN qxp_packet_compact_fvf_layout(
    ULONG fvf, QXP_D3D_COMPACT_FVF_LAYOUT *layout);
BOOLEAN qxp_packet_raw_indexed_object_fvf_layout(
    ULONG fvf, QXP_D3D_RAW_OBJECT_FVF_LAYOUT *layout);

BOOLEAN qxp_packet_program_texture_contract(
    const QXP_D3D_PROGRAM_VERTEX_PAYLOAD *program,
    ULONG coordinate_stages, ULONG projected_stages);

/* Structural validation of a texture-define packet.  Pure: no device state.
 * Checks version, identity (process/name, reserved-name rejection), texture
 * type, per-type face/depth rules, non-zero and bounded dimensions, and
 * level/total-level ordering.  Stateful identity (existing texture id,
 * format/type reuse, and mip matching) stays in the miniport. */
BOOLEAN qxp_packet_texture_define_contract(
    const QXP_D3D_TEXTURE_DEFINE_REQUEST *request);
/* Checked width*height*depth*bytes-per-pixel allocation bound.  The format
 * resolver supplies bytes_per_pixel before any host/FIFO allocation call. */
BOOLEAN qxp_packet_texture_allocation_contract(
    const QXP_D3D_TEXTURE_DEFINE_REQUEST *request,
    ULONG bytes_per_pixel);

/* Structural and arithmetic validation of a texture-upload packet.  Pure:
 * the caller supplies the base (level-0) width/height/depth, the defined
 * level count, and the resolved bytes-per-pixel.  Validates version, type,
 * per-type face/z/depth rules, level ordering, the checked X/Y/Z sub-box
 * against the requested mip level, the overflow-checked
 * width*height*depth*bpp payload size, and the exact packet length (both
 * truncated and trailing payloads are rejected). */
BOOLEAN qxp_packet_texture_upload_contract(
    const QXP_D3D_TEXTURE_UPLOAD_REQUEST *request, ULONG request_bytes,
    ULONG base_width, ULONG base_height, ULONG base_depth,
    ULONG level_count, ULONG bytes_per_pixel);

/* Extent of one mip level of a base dimension: D3D halves each dimension per
 * level and clamps at 1.  Shared so the readback path can name a level by the
 * extent the display driver asks for, exactly as the upload contract sizes a
 * level's sub-box. */
ULONG qxp_packet_mip_dimension(ULONG base, ULONG level);

/* Pixel-stage mask contract shared by every draw path.  Each mask is bounded
 * to stages 0-3; cube and volume masks are disjoint; cube and volume are each
 * subsets of the sampled mask; and the sampled mask is a subset of the
 * coordinate mask. */
BOOLEAN qxp_packet_draw_mask_contract(
    ULONG sampled_stages, ULONG coordinate_stages,
    ULONG cube_stages, ULONG volume_stages);

/* Independently validates fixed-function TEXCOORDINDEX routing at the
 * miniport trust boundary and separates array-fed from camera-generated
 * stages without indexing fixed-FVF metadata through a raw mode value. */
BOOLEAN qxp_packet_texcoord_plan(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_DRAW_PACKET_VIEW *view,
    ULONG coordinate_stages, QXP_D3D_TEXCOORD_PLAN *plan);

/* Selects complete internal GLSL where fixed GL would bypass generated
 * coordinates, consume a nondeterministic missing normal, or cannot express
 * D3D's independent diffuse/specular/ambient/emissive vertex sources. */
BOOLEAN qxp_packet_object_glsl_required(
    const QXP_D3D_DRAW_REQUEST *request,
    const QXP_D3D_DRAW_PACKET_VIEW *view,
    const QXP_D3D_TEXCOORD_PLAN *plan, BOOLEAN internal_program);

/* Resource-target containment for one resolved texture stage.  Fixed/legacy
 * stages retain existing 2D/cube handling but may never consume a diagnostic
 * volume resource.  A programmable stage must name exactly the resource type
 * selected by its mutually exclusive cube/volume bits. */
BOOLEAN qxp_packet_texture_binding_contract(
    ULONG texture_type, BOOLEAN programmable_sample,
    BOOLEAN programmable_cube, BOOLEAN programmable_volume);

BOOLEAN qxp_packet_surface_readback_contract(
    const QXP_D3D_SURFACE_READBACK_REQUEST *request);
BOOLEAN qxp_packet_present_contract(
    const QXP_D3D_PRESENT_REQUEST *request);
BOOLEAN qxp_packet_surface_copy_contract(
    const QXP_D3D_SURFACE_COPY_REQUEST *request);
BOOLEAN qxp_packet_sm2_program_define_contract(
    const QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST *request,
    ULONG request_bytes);
BOOLEAN qxp_packet_sm2_program_delete_contract(
    const QXP_D3D_SM2_PROGRAM_DELETE_REQUEST *request,
    ULONG request_bytes);
BOOLEAN qxp_packet_sm2_constants_contract(
    const QXP_D3D_SM2_CONSTANTS_REQUEST *request,
    ULONG request_bytes, ULONG owner_process_id,
    ULONG owner_program_name);

BOOLEAN qxp_packet_view_draw(const QXP_D3D_DRAW_REQUEST *request,
                             ULONG request_bytes,
                             QXP_D3D_DRAW_PACKET_VIEW *view);

BOOLEAN qxp_packet_view_draw_batch(
    const QXP_D3D_DRAW_BATCH_REQUEST *request, ULONG request_bytes,
    QXP_D3D_DRAW_BATCH_VIEW *view);

BOOLEAN qxp_packet_next_batch_draw(
    QXP_D3D_DRAW_BATCH_VIEW *view,
    const QXP_D3D_DRAW_REQUEST **request, PULONG request_bytes);

#endif
