#ifndef QXPMP_H
#define QXPMP_H

#pragma warning(push)
#pragma warning(disable: 4200 4201 4214)
#include <dderror.h>
#include <devioctl.h>
#include <miniport.h>
#include <ntddvdeo.h>
#include <video.h>
#pragma warning(pop)

#include "../qxp_d3d_transport.h"

#define QXP_DISPI_INDEX_ID 0x00
#define QXP_DISPI_INDEX_XRES 0x01
#define QXP_DISPI_INDEX_YRES 0x02
#define QXP_DISPI_INDEX_BPP 0x03
#define QXP_DISPI_INDEX_ENABLE 0x04
#define QXP_DISPI_INDEX_VIRT_WIDTH 0x06
#define QXP_DISPI_INDEX_X_OFFSET 0x08
#define QXP_DISPI_INDEX_Y_OFFSET 0x09
#define QXP_DISPI_INDEX_VRAM_64K 0x0A

#define QXP_DISPI_ID2 0xB0C2
#define QXP_DISPI_ID5 0xB0C5
#define QXP_DISPI_DISABLED 0x00
#define QXP_DISPI_ENABLED 0x01
#define QXP_DISPI_GETCAPS 0x02
#define QXP_DISPI_8BIT_DAC 0x20
#define QXP_DISPI_LFB_ENABLED 0x40

#define QXP_VGA_DAC_WRITE_INDEX 0x03C8
#define QXP_VGA_DAC_DATA 0x03C9

#define QXP_MMIO_DISPI_OFFSET 0x500
#define QXP_MMIO_LENGTH 0x1000
#define QXP_MODE_CAPACITY 24

#define QXP_MESA_REGS_BASE 0xefffe000UL
#define QXP_MESA_REGS_SIZE 0x00001000UL
#define QXP_MESA_FIFO_BASE 0xec000000UL
#define QXP_MESA_FIFO_SIZE 0x03ffc000UL
#define QXP_MESA_FBTM_BASE 0xea000000UL
#define QXP_MESA_FBTM_SIZE 0x02000000UL
/* Texture names are a dense monotonic counter, so the low bits of a name index
 * this table directly and lookups probe instead of scanning.  The capacity MUST
 * stay a power of two for the mask, and a freed slot leaves a tombstone so a
 * probe run that collided past it still terminates on the right entry. */
#define QXP_D3D_TEXTURE_CAPACITY 4096UL
#define QXP_D3D_TEXTURE_TOMBSTONE 0xffffffffUL
#define QXP_D3D_DEPTH_CAPACITY 64
#define QXP_D3D_GLSL_UNIFORM_CAPACITY 11
#define QXP_D3D_VERTEX_CLIP_CAPACITY 256

typedef struct qxp_resolution {
    USHORT width;
    USHORT height;
    USHORT bits_per_pixel;
} QXP_RESOLUTION;

typedef struct qxp_range {
    PHYSICAL_ADDRESS physical;
    ULONG length;
    UCHAR io_space;
    PUCHAR mapped;
} QXP_RANGE;


typedef struct qxp_d3d_texture_state {
    ULONG process_id;
    ULONG name;
    ULONG width;
    ULONG height;
    /* ABI 58.  Mip-level-0 depth.  Always 1 for 2D and cube textures; the
     * diagnostic volume texture stores its true depth here. */
    ULONG depth;
    ULONG format;
    ULONG texture_type;
    ULONG levels;
    ULONG defined_levels[6];
    ULONG framebuffer_depth_name;
    ULONG framebuffer_depth_format;
    BOOLEAN framebuffer_complete;
    /* The texture was verified complete as a surface-copy colour attachment.
     * FBO completeness is intrinsic to the texture's format and defined
     * levels, so one verified check stands until the texture is redefined;
     * caching it keeps the per-TEXBLT CHECK_FRAMEBUFFER_STATUS round trips
     * off the steady-state path.  Cleared wherever framebuffer_complete is,
     * for the same reason. */
    BOOLEAN copy_complete;
} QXP_D3D_TEXTURE_STATE;

typedef struct qxp_d3d_depth_state {
    ULONG process_id;
    ULONG surface_name;
    ULONG renderbuffer_name;
    ULONG width;
    ULONG height;
    ULONG format;
} QXP_D3D_DEPTH_STATE;

typedef struct qxp_d3d_vertex_clip_state {
    ULONG program_name;
    ULONG gl_program;
    LONG constants_location;
    LONG control_location;
    LONG projected_location;
} QXP_D3D_VERTEX_CLIP_STATE;

typedef struct qxp_device_extension {
    QXP_RANGE framebuffer;
    QXP_RANGE control;
    QXP_RANGE mesa_registers;
    QXP_RANGE mesa_fifo;
    QXP_RANGE mesa_fbtm;
    UCHAR supported[QXP_MODE_CAPACITY];
    ULONG mode_count;
    ULONG current_mode;
    ULONG vram_bytes;
    USHORT max_width;
    USHORT max_height;
    BOOLEAN use_mmio;
    BOOLEAN d3d_active;
    BOOLEAN d3d_async_supported; /* host advertises async draw doorbell (0xfb8) */
    BOOLEAN d3d_rhw_clamp_logged;
    BOOLEAN d3d_array_failure_logged;
    BOOLEAN d3d_texture_failure_logged;
    BOOLEAN d3d_sm2_draw_failure_logged;
    BOOLEAN d3d_target_failure_logged;
    BOOLEAN d3d_batch_failure_logged;
    BOOLEAN d3d_texq_glsl_logged;
    BOOLEAN d3d_texgen_camera_position_logged;
    BOOLEAN d3d_texture_combine3;
    BOOLEAN d3d_texture_combine4;
    BOOLEAN d3d_neutral_texture_defined;
    BOOLEAN d3d_table_fog_logged[2];
    USHORT d3d_token;
    ULONG d3d_process_id;
    ULONG d3d_references;
    ULONG d3d_pixel_program;
    ULONG d3d_vertex_program;
    BOOLEAN d3d_vertex_program_point_size;
    BOOLEAN d3d_vertex_program_clip;
    BOOLEAN d3d_texture_q_program_defined;
    BOOLEAN d3d_positional_range_program_defined;
    BOOLEAN d3d_dual_point_specular_program_defined;
    BOOLEAN d3d_quad_point_range_program_defined;
    BOOLEAN d3d_multi_point_range_program_defined;
    BOOLEAN d3d_compact_transformed_program_defined;
    ULONG d3d_glsl_program;
    BOOLEAN d3d_glsl_program_failed;
    LONG d3d_glsl_uniforms[QXP_D3D_GLSL_UNIFORM_CAPACITY];
    ULONG d3d_specular_alpha_program;
    BOOLEAN d3d_specular_alpha_program_failed;
    ULONG d3d_framebuffer;
    ULONG d3d_copy_framebuffer;
    ULONG d3d_present_cpu_count;
    ULONG d3d_present_texture_count;
    ULONG d3d_present_empty_count;
    ULONG d3d_target_texture;
    ULONG d3d_target_depth;
    BOOLEAN d3d_target_present;
    ULONG d3d_draw_count;
    ULONG d3d_draw_ioctl_count;
    ULONG d3d_draw_batch_ioctl_count;
    ULONG d3d_draw_batch_max_requests;
    ULONG d3d_present_count;
    ULONG d3d_surface_copy_count;
    ULONG d3d_surface_readback_count;
    ULONG d3d_clear_count;
    ULONG d3d_texture_upload_count;
    ULONG d3d_texture_define_count;
    ULONG d3d_texture_delete_count;
    ULONG d3d_texture_live_max;
    ULONG d3d_pending_draws;
    ULONG d3d_batch_count;
    ULONG d3d_batch_max_draws;
    ULONG d3d_fixed_state_reuse_count;
    ULONG d3d_texture_state_reuse_count;
    ULONG d3d_array_state_reuse_count;
    ULONG d3d_interleaved_draw_count;
    ULONG d3d_compact_draw_count;
    ULONG d3d_compact_indexed_draw_count;
    ULONG d3d_indexed_canonical_draw_count;
    ULONG d3d_indexed_element_draw_count;
    ULONG d3d_raw_indexed_object_draw_count;
#ifdef QXP_D3D_PROFILE
    ULONGLONG d3d_profile_draw_ticks;
    ULONGLONG d3d_profile_packet_ticks;
    ULONGLONG d3d_profile_prepare_ticks;
    ULONGLONG d3d_profile_state_ticks;
    ULONGLONG d3d_profile_submit_ticks;
    ULONGLONG d3d_profile_interleaved_copy_ticks;
    ULONGLONG d3d_profile_interleaved_copy_bytes;
    ULONG d3d_profile_interleaved_copy_calls;
    ULONGLONG d3d_profile_port_write_ticks;
    ULONGLONG d3d_profile_spin_ticks;
    ULONG d3d_profile_calibration_sink;
#endif
    BOOLEAN d3d_draw_state_valid;
    BOOLEAN d3d_last_fog_active;
    BOOLEAN d3d_last_generic_secondary;
    BOOLEAN d3d_last_target_texture;
    BOOLEAN d3d_last_target_present;
    BOOLEAN d3d_last_hwtl_valid;
    ULONG d3d_last_coordinate_stages;
    QXP_D3D_DRAW_REQUEST d3d_last_draw;
    QXP_D3D_HWTL_STATE d3d_last_hwtl_state;
    ULONG d3d_pixel_constants[QXP_D3D_PIXEL_CONSTANT_COUNT][4];
    QXP_D3D_TEXTURE_STATE d3d_textures[QXP_D3D_TEXTURE_CAPACITY];
    QXP_D3D_DEPTH_STATE d3d_depths[QXP_D3D_DEPTH_CAPACITY];
    QXP_D3D_VERTEX_CLIP_STATE
        d3d_vertex_clips[QXP_D3D_VERTEX_CLIP_CAPACITY];
} QXP_DEVICE_EXTENSION;

VP_STATUS QxpFindAdapter(PVOID extension, PVOID context, PWSTR arguments,
                         PVIDEO_PORT_CONFIG_INFO config, PUCHAR again);
BOOLEAN QxpInitialize(PVOID extension);
BOOLEAN QxpStartIO(PVOID extension, PVIDEO_REQUEST_PACKET request);
BOOLEAN QxpResetHw(PVOID extension, ULONG columns, ULONG rows);

#endif
