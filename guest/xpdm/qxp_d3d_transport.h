#ifndef QXP_D3D_TRANSPORT_H
#define QXP_D3D_TRANSPORT_H

#include <devioctl.h>

#define QXP_D3D_TRANSPORT_VERSION 64UL
#define QXP_D3D_STRINGIFY_INNER(value) #value
#define QXP_D3D_STRINGIFY(value) QXP_D3D_STRINGIFY_INNER(value)
#define QXP_D3D_TRANSPORT_ABI_MARKER \
    "QXP_D3D_TRANSPORT_ABI=" \
    QXP_D3D_STRINGIFY(QXP_D3D_TRANSPORT_VERSION)


#define IOCTL_VIDEO_QXP_D3D_OPEN \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_CLOSE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_CLEAR \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_DRAW \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_PRESENT \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_TEXTURE_DEFINE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_TEXTURE_UPLOAD \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_TEXTURE_DELETE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SHADER_DEFINE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SHADER_DELETE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SET_TARGET \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80a, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_PUBLISH \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80b, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_DRAW_BATCH \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80c, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SURFACE_COPY \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80d, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SURFACE_READBACK \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80e, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* ABI 54.  Shader Model 2 needs a linked GLSL pair, which the existing shader
 * define cannot express: that path targets ARB assembly programs through
 * glProgramStringARB.  These carry their own request shapes so every existing
 * contract stays byte-identical. */
#define IOCTL_VIDEO_QXP_D3D_SM2_PROGRAM_DEFINE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x80f, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VIDEO_QXP_D3D_SM2_PROGRAM_DELETE \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* ABI 55.  Sticky SM2 float-constant updates are owner/program scoped and
 * transported independently from draw packets. */
#define IOCTL_VIDEO_QXP_D3D_SM2_CONSTANTS \
    CTL_CODE(FILE_DEVICE_VIDEO, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define QXP_D3D_MAX_DRAW_VERTICES 255UL
#define QXP_D3D_MAX_VERTEX_STRIDE 255UL
#define QXP_D3D_MAX_PROGRAM_VERTEX_STRIDE 256UL
#define QXP_D3D_COMPACT_INDEX_ALIGNMENT 8UL
#define QXP_D3D_MAX_DRAW_BATCH_BYTES (512UL * 1024UL)
#define QXP_D3D_MAX_DRAW_BATCH_REQUESTS 64UL
#define QXP_D3D_MAX_CLEAR_RECTS 32UL
#define QXP_D3D_MAX_TEXTURE_UPLOAD_BYTES (60UL * 1024UL)
#define QXP_D3D_MAX_TEXTURE_LEVELS 12UL
#define QXP_D3D_PIXEL_CONSTANT_COUNT 8UL
#define QXP_D3D_VERTEX_CONSTANT_COUNT 96UL
/* Shader Model 2 constant files are far larger than the fixed-function and
 * SM1.x files above, so they are sized separately rather than growing those
 * and enlarging every legacy packet. */
#define QXP_D3D_SM2_VERTEX_CONSTANT_COUNT 256UL
#define QXP_D3D_SM2_PIXEL_CONSTANT_COUNT 32UL
#define QXP_D3D_SM2_SAMPLER_COUNT 16UL
#define QXP_D3D_SM2_SAMPLER_MASK 0xffffUL
#define QXP_D3D_SM2_VERTEX_INTEGER_CONSTANT_COUNT 16UL
#define QXP_D3D_SM2_VERTEX_BOOLEAN_CONSTANT_COUNT 16UL
/* The generated ps_2_0 coverage shader is already 8191 bytes, so the 8 KiB
 * SM1.x pixel limit cannot hold a real SM2 fragment shader. */
#define QXP_D3D_SM2_MAX_VERTEX_SOURCE_BYTES (64UL * 1024UL)
#define QXP_D3D_SM2_MAX_PIXEL_SOURCE_BYTES (64UL * 1024UL)
#define QXP_D3D_MAX_PIXEL_SHADER_SOURCE_BYTES 8192UL
#define QXP_D3D_MAX_VERTEX_SHADER_SOURCE_BYTES (64UL * 1024UL)
#define QXP_D3D_MAX_VERTEX_CLIP_SOURCE_BYTES (128UL * 1024UL)
#define QXP_D3D_MAX_SHADER_SOURCE_BYTES \
    QXP_D3D_MAX_VERTEX_CLIP_SOURCE_BYTES
#define QXP_D3D_MAX_ACTIVE_LIGHTS 8UL
#define QXP_D3D_MAX_USER_CLIP_PLANES 6UL
#define QXP_D3D_MAX_WORLD_MATRICES 4UL
#define QXP_D3D_MAX_WORLD_TRANSFORMS 256UL
#define QXP_D3D_MAX_TEXTURE_MATRICES 8UL

#define QXP_D3D_SHADER_VERTEX 1UL
#define QXP_D3D_SHADER_PIXEL 2UL
#define QXP_D3D_SHADER_VERTEX_CLIP 3UL
#define QXP_D3D_PROGRAM_NAMESPACE_MASK 0xff000000UL
#define QXP_D3D_PIXEL_PROGRAM_NAMESPACE 0x01000000UL
#define QXP_D3D_VERTEX_PROGRAM_NAMESPACE 0x03000000UL
#define QXP_D3D_TEXTURE_2D 1UL
#define QXP_D3D_TEXTURE_CUBE 2UL
/* ABI 58.  Diagnostic volume (3D) texture.  Carries an explicit depth in the
 * define/upload packets and a dedicated pixel-stage mask in the draw packet.
 * No production runtime path infers volume support from this type. */
#define QXP_D3D_TEXTURE_VOLUME 3UL
/* The display allocator never publishes this name. The miniport binds it to
 * one complete neutral texture when a fixed stage has no sampling operands. */
#define QXP_D3D_NEUTRAL_TEXTURE_NAME 0xfffffffeUL
#define QXP_D3D_NEUTRAL_CUBE_TEXTURE_NAME 0xfffffffbUL
#define QXP_D3D_NEUTRAL_VOLUME_TEXTURE_NAME 0xfffffffaUL
/* Miniport-owned scratch texture that carries CPU-authored present pixels.
 * Reserved exactly like the neutral texture: the display never allocates it
 * and the miniport rejects guest texture requests naming it. */
#define QXP_D3D_PRESENT_SCRATCH_TEXTURE_NAME 0xfffffffdUL
/* Display/miniport-private render target used only by escape 0x1104. */
#define QXP_D3D_SM2_SELFTEST_TEXTURE_NAME 0xfffffffcUL

#define QXP_D3D_VERTEX_TRANSFORMED 0UL
#define QXP_D3D_VERTEX_OBJECT 1UL
#define QXP_D3D_VERTEX_PROGRAMMABLE 2UL
#define QXP_D3D_VERTEX_COMPACT_TRANSFORMED 3UL
#define QXP_D3D_VERTEX_COMPACT_INDEXED_TRANSFORMED 4UL
/* ABI 63. Canonical indexed modes carry each validated source vertex once and
 * a byte index for each expanded primitive corner. The miniport expands the
 * same canonical columns it consumed before, preserving fixed-function
 * semantics while removing duplicate guest-to-kernel payload. */
#define QXP_D3D_VERTEX_INDEXED_TRANSFORMED 5UL
#define QXP_D3D_VERTEX_INDEXED_OBJECT 6UL
/* ABI 64.  The narrow raw-object mode carries the validated source-FVF
 * record once per referenced vertex, followed by byte indices and one HWTL
 * snapshot.  The miniport binds those records directly to fixed GL arrays;
 * unsupported FVF/state combinations remain on ABI 63's canonical path. */
#define QXP_D3D_VERTEX_RAW_INDEXED_OBJECT 7UL

/* Raw D3DTSS_TEXCOORDINDEX contract. The low selector remains meaningful for
 * wrap-state selection even when fixed vertex processing generates the actual
 * camera-space coordinate. Existing draw fields carry this value unchanged. */
#define QXP_D3D_TCI_INDEX_MASK 0x0000ffffUL
#define QXP_D3D_TCI_MODE_MASK 0xffff0000UL
#define QXP_D3D_TCI_PASSTHRU 0x00000000UL
#define QXP_D3D_TCI_CAMERASPACENORMAL 0x00010000UL
#define QXP_D3D_TCI_CAMERASPACEPOSITION 0x00020000UL
#define QXP_D3D_TCI_CAMERASPACEREFLECTIONVECTOR 0x00030000UL

#define QXP_D3D_VERTEX_FORMAT_FLOAT1 0UL
#define QXP_D3D_VERTEX_FORMAT_FLOAT2 1UL
#define QXP_D3D_VERTEX_FORMAT_FLOAT3 2UL
#define QXP_D3D_VERTEX_FORMAT_FLOAT4 3UL
#define QXP_D3D_VERTEX_FORMAT_D3DCOLOR 4UL
#define QXP_D3D_VERTEX_FORMAT_UBYTE4 5UL
#define QXP_D3D_VERTEX_FORMAT_SHORT2 6UL
#define QXP_D3D_VERTEX_FORMAT_SHORT4 7UL

#define QXP_D3D_VS_OUTPUT_POSITION 0x00000001UL
#define QXP_D3D_VS_OUTPUT_FOG 0x00000002UL
#define QXP_D3D_VS_OUTPUT_POINT_SIZE 0x00000004UL
#define QXP_D3D_VS_OUTPUT_COLOR0 0x00000008UL
#define QXP_D3D_VS_OUTPUT_COLOR1 0x00000010UL
#define QXP_D3D_VS_OUTPUT_TEXCOORD0 0x00000100UL
#define QXP_D3D_VS_OUTPUT_TEXCOORD_MASK 0x0000ff00UL
#define QXP_D3D_VS_OUTPUT_MASK \
    (QXP_D3D_VS_OUTPUT_POSITION | QXP_D3D_VS_OUTPUT_FOG | \
     QXP_D3D_VS_OUTPUT_POINT_SIZE | QXP_D3D_VS_OUTPUT_COLOR0 | \
     QXP_D3D_VS_OUTPUT_COLOR1 | QXP_D3D_VS_OUTPUT_TEXCOORD_MASK)

#define QXP_D3D_TRANSFORM_VIEW 2UL
#define QXP_D3D_TRANSFORM_PROJECTION 3UL
#define QXP_D3D_TRANSFORM_TEXTURE0 16UL
#define QXP_D3D_TRANSFORM_WORLD0 256UL
#define QXP_D3D_TRANSFORM_LEGACY_WORLD0 1UL
#define QXP_D3D_TRANSFORM_LEGACY_WORLD1 4UL
#define QXP_D3D_TRANSFORM_LEGACY_WORLD2 5UL
#define QXP_D3D_TRANSFORM_LEGACY_WORLD3 6UL

typedef struct qxp_d3d_session_request {
    ULONG version;
    ULONG process_id;
} QXP_D3D_SESSION_REQUEST;

/* Present/publish request, routed by where the flipped surface's newest
 * image actually lives (surface-level authority):
 * - QXP_D3D_PRESENT_CPU_SOURCE: the newest image is CPU-authored guest
 *   video memory (a CPU compositor wrote it, e.g. POP:SoT's menu).  The
 *   miniport uploads the described span into the reserved scratch texture
 *   and blits it to the output framebuffer before publishing.  The memory
 *   fields describe the span; texture_name must be zero.
 * - QXP_D3D_PRESENT_GPU_TEXTURE: the newest image is the named host
 *   texture (an accelerated copy or draw wrote it and no readback has
 *   consumed it).  The miniport blits that texture; the memory fields
 *   must be zero.
 * - flags zero: legacy presentation of the session's current render
 *   target; every payload field must be zero.  This is the proven path
 *   for contexts that drew the flipped surface this frame.
 * The flags are mutually exclusive. */
#define QXP_D3D_PRESENT_CPU_SOURCE 0x00000001UL
#define QXP_D3D_PRESENT_GPU_TEXTURE 0x00000002UL

typedef struct qxp_d3d_present_request {
    ULONG version;
    ULONG process_id;
    ULONG flags;
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG vidmem_offset;
    ULONG pitch;
    ULONG texture_name;
} QXP_D3D_PRESENT_REQUEST;

typedef struct qxp_d3d_rect {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} QXP_D3D_RECT;

typedef struct qxp_d3d_clear_request {
    ULONG version;
    ULONG process_id;
    ULONG flags;
    ULONG color;
    ULONG depth_bits;
    ULONG stencil;
    ULONG target_width;
    ULONG target_height;
    ULONG rectangle_count;
    QXP_D3D_RECT rectangles[QXP_D3D_MAX_CLEAR_RECTS];
} QXP_D3D_CLEAR_REQUEST;

typedef struct qxp_d3d_target_request {
    ULONG version;
    ULONG process_id;
    ULONG texture_name;
    ULONG width;
    ULONG height;
    ULONG depth_name;
    ULONG depth_width;
    ULONG depth_height;
    ULONG depth_format;
    ULONG flags;
} QXP_D3D_TARGET_REQUEST;

#define QXP_D3D_TARGET_PRESENT 0x00000001UL

/* Surface-copy formats use their D3DFORMAT numeric values without importing
 * mutually exclusive public D3D headers into the miniport/test boundary. */
#define QXP_D3D_COPY_FORMAT_R5G6B5 23UL
#define QXP_D3D_COPY_FORMAT_A8R8G8B8 21UL
#define QXP_D3D_COPY_FORMAT_X8R8G8B8 22UL
#define QXP_D3D_SURFACE_COPY_POINT 0UL

typedef struct qxp_d3d_surface_copy_request {
    ULONG version;
    ULONG process_id;
    ULONG source_texture_name;
    ULONG source_width;
    ULONG source_height;
    ULONG source_format;
    QXP_D3D_RECT source_rect;
    ULONG destination_texture_name;
    ULONG destination_width;
    ULONG destination_height;
    ULONG destination_format;
    QXP_D3D_RECT destination_rect;
    ULONG filter;
} QXP_D3D_SURFACE_COPY_REQUEST;

/* Downloads one host-authoritative texture image into guest video memory so
 * CPU access (DirectDraw Lock, CPU blts) observes what the GPU produced.
 * vidmem_offset/pitch address the destination inside the linear framebuffer
 * aperture; the miniport validates them against the aperture length before
 * writing a single byte. */
typedef struct qxp_d3d_surface_readback_request {
    ULONG version;
    ULONG process_id;
    ULONG texture_name;
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG vidmem_offset;
    ULONG pitch;
} QXP_D3D_SURFACE_READBACK_REQUEST;

/* Hardware T&L state is transported as raw IEEE-754 bit patterns.  Keeping
 * floating-point work out of both XPDM halves makes the guest ABI explicit
 * and lets the host choose an exact fixed-function or vertex-program replay
 * without changing the command stream. */
typedef struct qxp_d3d_matrix {
    ULONG element[16];
} QXP_D3D_MATRIX;

typedef struct qxp_d3d_color_value {
    ULONG component[4];
} QXP_D3D_COLOR_VALUE;

typedef struct qxp_d3d_material {
    QXP_D3D_COLOR_VALUE diffuse;
    QXP_D3D_COLOR_VALUE ambient;
    QXP_D3D_COLOR_VALUE specular;
    QXP_D3D_COLOR_VALUE emissive;
    ULONG power_bits;
} QXP_D3D_MATERIAL;

typedef struct qxp_d3d_light {
    ULONG type;
    QXP_D3D_COLOR_VALUE diffuse;
    QXP_D3D_COLOR_VALUE specular;
    QXP_D3D_COLOR_VALUE ambient;
    ULONG position[3];
    ULONG direction[3];
    ULONG range_bits;
    ULONG falloff_bits;
    ULONG attenuation0_bits;
    ULONG attenuation1_bits;
    ULONG attenuation2_bits;
    ULONG theta_bits;
    ULONG phi_bits;
} QXP_D3D_LIGHT;

typedef struct qxp_d3d_active_light {
    ULONG logical_index;
    QXP_D3D_LIGHT light;
} QXP_D3D_ACTIVE_LIGHT;

typedef struct qxp_d3d_hwtl_state {
    QXP_D3D_MATRIX world[QXP_D3D_MAX_WORLD_MATRICES];
    QXP_D3D_MATRIX view;
    QXP_D3D_MATRIX projection;
    QXP_D3D_MATRIX texture[QXP_D3D_MAX_TEXTURE_MATRICES];
    QXP_D3D_MATERIAL material;
    QXP_D3D_ACTIVE_LIGHT active_lights[QXP_D3D_MAX_ACTIVE_LIGHTS];
    ULONG active_light_count;
    ULONG clip_planes[QXP_D3D_MAX_USER_CLIP_PLANES][4];
    ULONG z_min_bits;
    ULONG z_max_bits;
    ULONG lighting_enable;
    ULONG ambient;
    ULONG fog_vertex_mode;
    ULONG color_vertex_enable;
    ULONG local_viewer;
    ULONG normalize_normals;
    ULONG diffuse_material_source;
    ULONG specular_material_source;
    ULONG ambient_material_source;
    ULONG emissive_material_source;
    ULONG clipping_enable;
    ULONG clip_plane_enable;
    ULONG range_fog_enable;
    ULONG texture_transform_flags[4];
} QXP_D3D_HWTL_STATE;

typedef struct qxp_d3d_vertex {
    ULONG x_bits;
    ULONG y_bits;
    ULONG z_bits;
    ULONG rhw_bits;
    ULONG color;
    ULONG specular;
    ULONG texcoord[4][4];
    /* ABI 59. D3DFVF_FOG is a separate per-vertex fog intensity. Keeping it
     * after the established texture-coordinate payload preserves every
     * existing canonical vertex member offset. */
    ULONG fog_bits;
} QXP_D3D_VERTEX;

typedef struct qxp_d3d_object_vertex_data {
    ULONG normal[3];
    ULONG point_size_bits;
} QXP_D3D_OBJECT_VERTEX_DATA;

/* Programmable draws reuse the byte offset of QXP_D3D_DRAW_REQUEST.vertices,
 * but begin with this metadata instead of canonical fixed-function vertices.
 * Elements are indexed by vertex input register. Inputs read by the shader but
 * absent from input_mask are supplied as (0, 0, 0, 0) for inputs 1-15; a
 * missing input 0 is rejected because ARB_vertex_program defines no current
 * generic attribute zero. Only declared inputs carry element metadata. Raw
 * records retain the declaration stride, including SKIP bytes; D3DCOLOR
 * records alone are canonicalized from little-endian BGRA
 * storage to RGBA byte order. */
typedef struct qxp_d3d_program_vertex_element {
    ULONG format;
    ULONG byte_offset;
} QXP_D3D_PROGRAM_VERTEX_ELEMENT;
/* ABI 61.  Native SM2 carries all sixteen programmable sampler bindings and
 * executable sampler state independently of the legacy four fixed-function
 * texture stages.  texture_name is a host texture name, not a DDI handle. */
typedef struct qxp_d3d_sm2_sampler_state {
    ULONG texture_name;
    ULONG texture_type;
    ULONG address_u;
    ULONG address_v;
    ULONG mag_filter;
    ULONG min_filter;
    ULONG mip_filter;
    ULONG mip_lod_bias_bits;
    ULONG max_mip_level;
} QXP_D3D_SM2_SAMPLER_STATE;

/* ABI 60 aliases the otherwise-unused legacy VS1.x constant bank in native
 * SM2 packets.  ABI 62 also places the sampler snapshot in that bank, keeping
 * both SM2-only records off every legacy draw while preserving the
 * programmable packet header size. */
typedef struct qxp_d3d_sm2_vertex_draw_state {
    ULONG z_min_bits;
    ULONG z_max_bits;
    ULONG integer_constants[
        QXP_D3D_SM2_VERTEX_INTEGER_CONSTANT_COUNT][4];
    ULONG boolean_constants[
        QXP_D3D_SM2_VERTEX_BOOLEAN_CONSTANT_COUNT];
} QXP_D3D_SM2_VERTEX_DRAW_STATE;

typedef struct qxp_d3d_sm2_draw_state {
    QXP_D3D_SM2_VERTEX_DRAW_STATE vertex;
    QXP_D3D_SM2_SAMPLER_STATE
        samplers[QXP_D3D_SM2_SAMPLER_COUNT];
} QXP_D3D_SM2_DRAW_STATE;

typedef struct qxp_d3d_program_vertex_payload {
    ULONG program_name;
    ULONG input_mask;
    ULONG used_inputs;
    ULONG written_outputs;
    ULONG vertex_stride;
    ULONG vertex_data_bytes;
    ULONG clipping_enable;
    ULONG clip_plane_enable;
    ULONG clip_planes[QXP_D3D_MAX_USER_CLIP_PLANES][4];
    QXP_D3D_PROGRAM_VERTEX_ELEMENT elements[16];
    /* Native SM2 packets alias the prefix with QXP_D3D_SM2_DRAW_STATE;
     * VS1.x packets retain all 96 vec4s. */
    ULONG constants[QXP_D3D_VERTEX_CONSTANT_COUNT][4];
    /* Byte-addressed vertex data; ULONG keeps the variable tail aligned and
     * makes sizeof(payload) - sizeof(vertex_data) its exact header size. */
    ULONG vertex_data[1];
} QXP_D3D_PROGRAM_VERTEX_PAYLOAD;
typedef char QXP_D3D_SM2_DRAW_STATE_FITS_LEGACY_BANK[
    sizeof(QXP_D3D_SM2_DRAW_STATE) <=
        sizeof(((QXP_D3D_PROGRAM_VERTEX_PAYLOAD *)0)->constants) ? 1 : -1];

/* Compact transformed draws retain the exact source-FVF record layout. The
 * packet validator derives every element offset from QXP_D3D_DRAW_REQUEST.fvf;
 * no guest-provided pointer or offset is trusted. The miniport can therefore
 * bind all elements to one negotiated host-interleaved allocation. */
typedef struct qxp_d3d_compact_vertex_payload {
    ULONG vertex_stride;
    ULONG vertex_data_bytes;
    ULONG vertex_data[1];
} QXP_D3D_COMPACT_VERTEX_PAYLOAD;

/* Indexed payloads keep one byte per expanded triangle/line-list corner,
 * zero padding through QXP_D3D_COMPACT_INDEX_ALIGNMENT, then one record per
 * unique vertex. Compact transformed and ABI-64 raw-object modes carry
 * source-FVF records for direct glDrawElements consumption. ABI 63 canonical
 * indexed modes instead carry QXP_D3D_VERTEX records; object mode appends one
 * object record per unique vertex and one HWTL snapshot. */
typedef struct qxp_d3d_compact_indexed_vertex_payload {
    ULONG vertex_stride;
    ULONG vertex_count;
    ULONG vertex_data_bytes;
    ULONG index_count;
    ULONG index_data_bytes;
    ULONG data[1];
} QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD;

typedef struct qxp_d3d_raw_indexed_object_payload {
    ULONG vertex_stride;
    ULONG vertex_count;
    ULONG vertex_data_bytes;
    ULONG index_count;
    ULONG index_data_bytes;
    ULONG data[1];
} QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD;

typedef struct qxp_d3d_indexed_vertex_payload {
    ULONG vertex_count;
    ULONG index_count;
    ULONG data[1];
} QXP_D3D_INDEXED_VERTEX_PAYLOAD;


typedef struct qxp_d3d_draw_request {
    ULONG version;
    ULONG process_id;
    ULONG primitive_type;
    ULONG fvf;
    ULONG texture_coord_dimensions[4];
    ULONG texture_projected_stages;
    ULONG target_width;
    ULONG target_height;
    ULONG viewport_x;
    ULONG viewport_y;
    ULONG viewport_width;
    ULONG viewport_height;
    ULONG z_enable;
    ULONG z_write_enable;
    ULONG z_func;
    ULONG z_bias;
    ULONG scissor_enable;
    QXP_D3D_RECT scissor_rectangle;
    ULONG cull_mode;
    ULONG fill_mode;
    ULONG shade_mode;
    ULONG point_size_bits;
    ULONG point_scale_enable;
    ULONG point_sprite_enable;
    ULONG point_size_min_bits;
    ULONG point_size_max_bits;
    ULONG point_scale_a_bits;
    ULONG point_scale_b_bits;
    ULONG point_scale_c_bits;
    ULONG alpha_blend_enable;
    ULONG source_blend;
    ULONG destination_blend;
    ULONG blend_operation;
    ULONG alpha_test_enable;
    ULONG alpha_reference;
    ULONG alpha_function;
    ULONG stencil_enable;
    ULONG stencil_fail;
    ULONG stencil_z_fail;
    ULONG stencil_pass;
    ULONG stencil_function;
    ULONG stencil_reference;
    ULONG stencil_mask;
    ULONG stencil_write_mask;
    ULONG color_write_enable;
    ULONG specular_enable;
    ULONG dither_enable;
    ULONG fog_enable;
    ULONG fog_color;
    ULONG fog_table_mode;
    ULONG fog_start_bits;
    ULONG fog_end_bits;
    ULONG fog_density_bits;
    ULONG texture_name;
    ULONG texture_factor;
    ULONG texture_color_op;
    ULONG texture_color_arg1;
    ULONG texture_color_arg2;
    ULONG texture_color_arg0;
    ULONG texture_alpha_op;
    ULONG texture_alpha_arg1;
    ULONG texture_alpha_arg2;
    ULONG texture_alpha_arg0;
    ULONG texture_address_u;
    ULONG texture_address_v;
    ULONG texture_mag_filter;
    ULONG texture_min_filter;
    ULONG texture_mip_filter;
    ULONG texture_mip_lod_bias_bits;
    ULONG texture_max_mip_level;
    ULONG texture_coord_index;
    ULONG texture1_name;
    ULONG texture1_color_op;
    ULONG texture1_color_arg1;
    ULONG texture1_color_arg2;
    ULONG texture1_color_arg0;
    ULONG texture1_alpha_op;
    ULONG texture1_alpha_arg1;
    ULONG texture1_alpha_arg2;
    ULONG texture1_alpha_arg0;
    ULONG texture1_address_u;
    ULONG texture1_address_v;
    ULONG texture1_mag_filter;
    ULONG texture1_min_filter;
    ULONG texture1_mip_filter;
    ULONG texture1_mip_lod_bias_bits;
    ULONG texture1_max_mip_level;
    ULONG texture1_coord_index;
    ULONG texture1_bumpenv_mat00_bits;
    ULONG texture1_bumpenv_mat01_bits;
    ULONG texture1_bumpenv_mat10_bits;
    ULONG texture1_bumpenv_mat11_bits;
    ULONG texture1_bumpenv_lscale_bits;
    ULONG texture1_bumpenv_loffset_bits;
    ULONG texture2_name;
    ULONG texture2_color_op;
    ULONG texture2_color_arg1;
    ULONG texture2_color_arg2;
    ULONG texture2_color_arg0;
    ULONG texture2_alpha_op;
    ULONG texture2_alpha_arg1;
    ULONG texture2_alpha_arg2;
    ULONG texture2_alpha_arg0;
    ULONG texture2_address_u;
    ULONG texture2_address_v;
    ULONG texture2_mag_filter;
    ULONG texture2_min_filter;
    ULONG texture2_mip_filter;
    ULONG texture2_mip_lod_bias_bits;
    ULONG texture2_max_mip_level;
    ULONG texture2_coord_index;
    ULONG texture3_name;
    ULONG texture3_color_op;
    ULONG texture3_color_arg1;
    ULONG texture3_color_arg2;
    ULONG texture3_color_arg0;
    ULONG texture3_alpha_op;
    ULONG texture3_alpha_arg1;
    ULONG texture3_alpha_arg2;
    ULONG texture3_alpha_arg0;
    ULONG texture3_address_u;
    ULONG texture3_address_v;
    ULONG texture3_mag_filter;
    ULONG texture3_min_filter;
    ULONG texture3_mip_filter;
    ULONG texture3_mip_lod_bias_bits;
    ULONG texture3_max_mip_level;
    ULONG texture3_coord_index;
    ULONG pixel_program_name;
    ULONG pixel_sampled_stages;
    ULONG pixel_coordinate_stages;
    ULONG pixel_cube_stages;
    ULONG pixel_constants[QXP_D3D_PIXEL_CONSTANT_COUNT][4];
    ULONG vertex_mode;
    /* Object-space draws append vertex_count normal/point-size records and one
     * QXP_D3D_HWTL_STATE after vertices. Programmable draws instead place one
     * QXP_D3D_PROGRAM_VERTEX_PAYLOAD, QXP_D3D_COMPACT_VERTEX_PAYLOAD, or
     * QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD at vertices[0]. These byte
     * offsets are zero for transformed, compact, and programmable draws. */
    ULONG object_data_offset;
    ULONG hwtl_state_offset;
    ULONG vertex_count;
    /* ABI 55 appends this to the fixed draw header.  Zero retains the legacy
     * ARB/fixed path; nonzero selects an owner-scoped linked SM2 program. */
    ULONG sm2_program_name;
    /* ABI 58.  Per-stage volume (3D) sampling mask.  Disjoint from
     * pixel_cube_stages and a subset of pixel_sampled_stages.  Legacy
     * fixed/ARB draws leave it zero.  ABI 62 moves the sixteen-entry SM2-only
     * sampler snapshot into QXP_D3D_SM2_DRAW_STATE, restoring this variable
     * vertex tail to its 0x2c0 offset. */
    ULONG pixel_volume_stages;
    QXP_D3D_VERTEX vertices[1];
} QXP_D3D_DRAW_REQUEST;

/* One buffered IOCTL can carry several already self-contained draw packets.
 * Each entry is ULONG aligned and retains the exact single-draw byte stream,
 * so validation and replay use the same trust boundary as the legacy IOCTL.
 * The batch has prefix semantics on failure, matching a DP2 command buffer
 * whose earlier draws have already completed before a later draw is rejected. */
typedef struct qxp_d3d_draw_batch_entry {
    ULONG request_bytes;
    ULONG request[1];
} QXP_D3D_DRAW_BATCH_ENTRY;

typedef struct qxp_d3d_draw_batch_request {
    ULONG version;
    ULONG process_id;
    ULONG draw_count;
    ULONG payload_bytes;
    ULONG entries[1];
} QXP_D3D_DRAW_BATCH_REQUEST;

#define QXP_D3D_DRAW_BATCH_HEADER_BYTES \
    (sizeof(QXP_D3D_DRAW_BATCH_REQUEST) - \
     sizeof(((QXP_D3D_DRAW_BATCH_REQUEST *)0)->entries))
#define QXP_D3D_DRAW_BATCH_ENTRY_HEADER_BYTES \
    (sizeof(QXP_D3D_DRAW_BATCH_ENTRY) - \
     sizeof(((QXP_D3D_DRAW_BATCH_ENTRY *)0)->request))

typedef struct qxp_d3d_texture_define_request {
    ULONG version;
    ULONG process_id;
    ULONG texture_name;
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG texture_type;
    ULONG face;
    ULONG level;
    ULONG total_levels;
    /* ABI 58.  Volume textures carry their mip-level depth here; 2D and cube
     * defines set it to 1.  Appended so every prior field keeps its offset. */
    ULONG depth;
} QXP_D3D_TEXTURE_DEFINE_REQUEST;

typedef struct qxp_d3d_texture_upload_request {
    ULONG version;
    ULONG process_id;
    ULONG texture_name;
    ULONG x;
    ULONG y;
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG texture_type;
    ULONG face;
    ULONG level;
    ULONG data_bytes;
    /* ABI 58.  Volume sub-image origin/extent depth; 2D and cube uploads set
     * z to 0 and depth to 1.  Inserted immediately before the variable data
     * tail so the fixed header grows from 48 to 56 bytes. */
    ULONG z;
    ULONG depth;
    ULONG data[1];
} QXP_D3D_TEXTURE_UPLOAD_REQUEST;

typedef struct qxp_d3d_texture_delete_request {
    ULONG version;
    ULONG process_id;
    ULONG texture_name;
} QXP_D3D_TEXTURE_DELETE_REQUEST;

typedef struct qxp_d3d_shader_define_request {
    ULONG version;
    ULONG process_id;
    ULONG shader_type;
    ULONG program_name;
    ULONG source_bytes;
    UCHAR source[1];
} QXP_D3D_SHADER_DEFINE_REQUEST;

typedef struct qxp_d3d_shader_delete_request {
    ULONG version;
    ULONG process_id;
    ULONG shader_type;
    ULONG program_name;
} QXP_D3D_SHADER_DELETE_REQUEST;

/* ABI 61.  One linked Shader Model 2 GLSL program.  source holds the vertex
 * source first and the pixel source immediately after it; each is individually
 * NUL terminated and its byte count includes that terminator.  The immutable
 * sampler declaration masks come from the validated pixel-shader metadata.
 * The miniport compiles both stages, binds the qxpV0..qxpV15 attribute
 * locations, links once, and caches the resulting uniform locations.
 * program_name is owner scoped and lives in the vertex namespace. */
typedef struct qxp_d3d_sm2_program_define_request {
    ULONG version;
    ULONG process_id;
    ULONG program_name;
    ULONG vertex_source_bytes;
    ULONG pixel_source_bytes;
    ULONG sampler_mask;
    ULONG cube_sampler_mask;
    ULONG volume_sampler_mask;
    UCHAR source[1];
} QXP_D3D_SM2_PROGRAM_DEFINE_REQUEST;

typedef struct qxp_d3d_sm2_program_delete_request {
    ULONG version;
    ULONG process_id;
    ULONG program_name;
} QXP_D3D_SM2_PROGRAM_DELETE_REQUEST;

/* ABI 55.  data contains vertex_register_count vec4 values followed by
 * pixel_register_count vec4 values.  Every component is a raw IEEE-754 ULONG
 * bit pattern, matching SetVertexShaderConstantF(start, data, count). */
typedef struct qxp_d3d_sm2_constants_request {
    ULONG version;
    ULONG process_id;
    ULONG program_name;
    ULONG vertex_start_register;
    ULONG vertex_register_count;
    ULONG pixel_start_register;
    ULONG pixel_register_count;
    ULONG data[1];
} QXP_D3D_SM2_CONSTANTS_REQUEST;

#endif
