#include "qxp_vertex.h"

#define QXP_FVF_POSITION_MASK 0x00eUL
#define QXP_FVF_XYZ 0x002UL
#define QXP_FVF_XYZRHW 0x004UL
#define QXP_FVF_NORMAL 0x010UL
#define QXP_FVF_PSIZE 0x020UL
#define QXP_FVF_DIFFUSE 0x040UL
#define QXP_FVF_SPECULAR 0x080UL
#define QXP_FVF_FOG 0x2000UL
#define QXP_FVF_TEXCOUNT_MASK 0xf00UL
#define QXP_FVF_TEX1 0x100UL
#define QXP_FVF_TEX4 0x400UL
#define QXP_FVF_TEXTURE_FORMAT_MASK 0x00ff0000UL
#define QXP_FVF_TEXTURE_FORMAT1 3UL
#define QXP_FVF_TEXTURE_FORMAT3 1UL
#define QXP_FVF_TEXTURE_FORMAT4 2UL

#if defined(_MSC_VER) && defined(_M_IX86)
void __movsb(unsigned char *, unsigned char const *, size_t);
void __movsd(unsigned long *, unsigned long const *, size_t);
void __stosb(unsigned char *, unsigned char, size_t);
void __stosd(unsigned long *, unsigned long, size_t);
#pragma intrinsic(__movsb, __movsd, __stosb, __stosd)
#endif

static void qxp_vertex_zero(void *destination, ULONG bytes)
{
    ULONG words = bytes >> 2;
    ULONG tail = bytes & 3UL;
    ULONG offset = words << 2;
#if !defined(_MSC_VER) || !defined(_M_IX86)
    ULONG index;
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
    if (words)
        __stosd((unsigned long *)destination, 0UL, (size_t)words);
    if (tail)
        __stosb((UCHAR *)destination + offset, 0, (size_t)tail);
#else
    for (index = 0; index < words; ++index)
        ((ULONG *)destination)[index] = 0;
    for (index = 0; index < tail; ++index)
        ((UCHAR *)destination)[offset + index] = 0;
#endif
}

static void qxp_vertex_copy_bytes(void *destination, const void *source,
                                  ULONG bytes)
{
    ULONG words = bytes >> 2;
    ULONG tail = bytes & 3UL;
    ULONG offset = words << 2;
#if !defined(_MSC_VER) || !defined(_M_IX86)
    ULONG index;
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
    if (words)
        __movsd((unsigned long *)destination,
                (unsigned long const *)source, (size_t)words);
    if (tail)
        __movsb((UCHAR *)destination + offset,
                (const UCHAR *)source + offset, (size_t)tail);
#else
    for (index = 0; index < words; ++index)
        ((ULONG *)destination)[index] = ((const ULONG *)source)[index];
    for (index = 0; index < tail; ++index)
        ((UCHAR *)destination)[offset + index] =
            ((const UCHAR *)source)[offset + index];
#endif
}
/* D3D leaves non-finite sampler coordinates undefined.  Keep them out of
 * the host GL boundary without rejecting the otherwise valid draw. */
static void qxp_vertex_copy_texcoord(ULONG *destination,
                                     const UCHAR *source)
{
    qxp_vertex_copy_bytes(destination, source, sizeof(*destination));
    if ((*destination & 0x7f800000UL) == 0x7f800000UL)
        *destination = 0;
}

BOOL qxp_vertex_decode_layout(ULONG fvf, QXP_D3D_VERTEX_LAYOUT *layout)
{
    ULONG allowed = QXP_FVF_POSITION_MASK | QXP_FVF_NORMAL |
                    QXP_FVF_PSIZE | QXP_FVF_DIFFUSE |
                    QXP_FVF_SPECULAR | QXP_FVF_FOG |
                    QXP_FVF_TEXCOUNT_MASK |
                    QXP_FVF_TEXTURE_FORMAT_MASK;
    ULONG position;
    ULONG offset;
    ULONG texture_format;
    ULONG texture;

    if (!layout || (fvf & ~allowed) ||
        (fvf & QXP_FVF_TEXCOUNT_MASK) > QXP_FVF_TEX4) return FALSE;
    position = fvf & QXP_FVF_POSITION_MASK;
    if (position != QXP_FVF_XYZ && position != QXP_FVF_XYZRHW)
        return FALSE;
    if (position == QXP_FVF_XYZRHW && (fvf & QXP_FVF_NORMAL))
        return FALSE;

    qxp_vertex_zero(layout, sizeof(*layout));
    layout->transformed = position == QXP_FVF_XYZRHW;
    offset = layout->transformed ? 16UL : 12UL;
    if (fvf & QXP_FVF_NORMAL) {
        layout->has_normal = TRUE;
        layout->normal_offset = offset;
        offset += 12;
    }
    if (fvf & QXP_FVF_PSIZE) {
        layout->has_point_size = TRUE;
        layout->point_size_offset = offset;
        offset += 4;
    }
    if (fvf & QXP_FVF_DIFFUSE) {
        layout->has_diffuse = TRUE;
        layout->diffuse_offset = offset;
        offset += 4;
    }
    if (fvf & QXP_FVF_SPECULAR) {
        layout->has_specular = TRUE;
        layout->specular_offset = offset;
        offset += 4;
    }
    layout->texture_count =
        (fvf & QXP_FVF_TEXCOUNT_MASK) / QXP_FVF_TEX1;
    for (texture = 0; texture < 4; ++texture) {
        texture_format = (fvf >> (16 + texture * 2)) & 3UL;
        if (texture >= layout->texture_count) {
            if (texture_format) return FALSE;
            continue;
        }
        layout->texcoord_offset[texture] = offset;
        if (texture_format == QXP_FVF_TEXTURE_FORMAT1) {
            layout->texcoord_dimensions[texture] = 1;
        } else if (texture_format == QXP_FVF_TEXTURE_FORMAT3) {
            layout->texcoord_dimensions[texture] = 3;
        } else if (texture_format == QXP_FVF_TEXTURE_FORMAT4) {
            layout->texcoord_dimensions[texture] = 4;
        } else {
            layout->texcoord_dimensions[texture] = 2;
        }
        offset += layout->texcoord_dimensions[texture] * 4;
    }
    /* The XP D3D9 runtime strips this DDI-only bit while building the
     * ordinary FVF declaration, then appends a FLOAT1 fog element. */
    if (fvf & QXP_FVF_FOG) {
        layout->has_fog = TRUE;
        layout->fog_offset = offset;
        offset += 4;
    }
    layout->vertex_bytes = offset;
    return TRUE;
}

void qxp_vertex_copy(QXP_D3D_VERTEX *destination,
                     QXP_D3D_OBJECT_VERTEX_DATA *object_data,
                     const UCHAR *source,
                     const QXP_D3D_VERTEX_LAYOUT *layout)
{
    ULONG component;
    ULONG texture;

    if (!destination || !source || !layout) return;
    qxp_vertex_zero(destination, sizeof(*destination));
    qxp_vertex_copy_bytes(&destination->x_bits, source, 4);
    qxp_vertex_copy_bytes(&destination->y_bits, source + 4, 4);
    qxp_vertex_copy_bytes(&destination->z_bits, source + 8, 4);
    if (layout->transformed) {
        qxp_vertex_copy_bytes(&destination->rhw_bits, source + 12, 4);
    } else {
        destination->rhw_bits = 0x3f800000UL;
    }
    if (object_data) {
        qxp_vertex_zero(object_data, sizeof(*object_data));
        object_data->point_size_bits = 0x3f800000UL;
        if (layout->has_normal) {
            qxp_vertex_copy_bytes(object_data->normal,
                                  source + layout->normal_offset, 12);
        }
        if (layout->has_point_size) {
            qxp_vertex_copy_bytes(&object_data->point_size_bits,
                                  source + layout->point_size_offset, 4);
        }
    }
    destination->color = 0xffffffffUL;
    if (layout->has_diffuse) {
        qxp_vertex_copy_bytes(&destination->color,
                              source + layout->diffuse_offset, 4);
    }
    if (layout->has_specular) {
        qxp_vertex_copy_bytes(&destination->specular,
                              source + layout->specular_offset, 4);
    }
    if (layout->has_fog) {
        qxp_vertex_copy_bytes(&destination->fog_bits,
                              source + layout->fog_offset, 4);
    }
    for (texture = 0; texture < layout->texture_count; ++texture) {
        for (component = 0;
             component < layout->texcoord_dimensions[texture];
             ++component) {
            qxp_vertex_copy_texcoord(
                &destination->texcoord[texture][component],
                source + layout->texcoord_offset[texture] + component * 4);
        }
    }
}

BOOL qxp_vertex_packet_layout(ULONG vertex_mode, ULONG vertex_count,
                              ULONG *object_data_offset,
                              ULONG *hwtl_state_offset,
                              ULONG *request_bytes)
{
    ULONG header_bytes = sizeof(QXP_D3D_DRAW_REQUEST) -
                         sizeof(QXP_D3D_VERTEX);
    ULONG bytes;

    if (!object_data_offset || !hwtl_state_offset || !request_bytes ||
        !vertex_count || vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        vertex_count > (0xffffffffUL - header_bytes) /
                       sizeof(QXP_D3D_VERTEX)) return FALSE;
    bytes = header_bytes + vertex_count * sizeof(QXP_D3D_VERTEX);
    *object_data_offset = 0;
    *hwtl_state_offset = 0;
    if (vertex_mode == QXP_D3D_VERTEX_TRANSFORMED) {
        *request_bytes = bytes;
        return TRUE;
    }
    if (vertex_mode != QXP_D3D_VERTEX_OBJECT ||
        vertex_count > (0xffffffffUL - bytes) /
                       sizeof(QXP_D3D_OBJECT_VERTEX_DATA))
        return FALSE;
    *object_data_offset = bytes;
    bytes += vertex_count * sizeof(QXP_D3D_OBJECT_VERTEX_DATA);
    if (bytes > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE)) return FALSE;
    *hwtl_state_offset = bytes;
    *request_bytes = bytes + sizeof(QXP_D3D_HWTL_STATE);
    return TRUE;
}

BOOL qxp_vertex_compact_supported(const QXP_D3D_VERTEX_LAYOUT *layout)
{
    ULONG texture;

    if (!layout || !layout->transformed || layout->has_normal ||
        layout->has_point_size || layout->has_fog ||
        !layout->vertex_bytes ||
        layout->vertex_bytes > QXP_D3D_MAX_VERTEX_STRIDE) return FALSE;
    for (texture = 0; texture < layout->texture_count; ++texture) {
        if (layout->texcoord_dimensions[texture] != 2) return FALSE;
    }
    return TRUE;
}

static BOOL qxp_vertex_float_finite(const UCHAR *source)
{
    ULONG value;

    if (!source) return FALSE;
    qxp_vertex_copy_bytes(&value, source, sizeof(value));
    return (value & 0x7f800000UL) != 0x7f800000UL;
}

BOOL qxp_vertex_compact_source_valid(
    const UCHAR *source, const QXP_D3D_VERTEX_LAYOUT *layout)
{
    ULONG component;
    ULONG texture;

    if (!source || !qxp_vertex_compact_supported(layout)) return FALSE;
    for (component = 0; component < 4; ++component) {
        if (!qxp_vertex_float_finite(source + component * sizeof(ULONG)))
            return FALSE;
    }
    for (texture = 0; texture < layout->texture_count; ++texture) {
        for (component = 0;
             component < layout->texcoord_dimensions[texture]; ++component) {
            if (!qxp_vertex_float_finite(
                    source + layout->texcoord_offset[texture] +
                    component * sizeof(ULONG))) return FALSE;
        }
    }
    return TRUE;
}

BOOL qxp_vertex_raw_indexed_object_supported(
    const QXP_D3D_VERTEX_LAYOUT *layout)
{
    return layout && !layout->transformed &&
           !layout->has_point_size && !layout->has_specular &&
           !layout->has_fog && layout->texture_count == 1 &&
           layout->texcoord_dimensions[0] == 2 &&
           layout->vertex_bytes && layout->vertex_bytes <=
                                      QXP_D3D_MAX_VERTEX_STRIDE;
}

BOOL qxp_vertex_raw_indexed_object_source_valid(
    const UCHAR *source, const QXP_D3D_VERTEX_LAYOUT *layout)
{
    ULONG component;

    if (!source || !qxp_vertex_raw_indexed_object_supported(layout))
        return FALSE;
    for (component = 0; component < 3; ++component) {
        if (!qxp_vertex_float_finite(
                source + component * sizeof(ULONG)) ||
            (layout->has_normal &&
             !qxp_vertex_float_finite(
                 source + layout->normal_offset +
                     component * sizeof(ULONG))))
            return FALSE;
    }
    for (component = 0; component < 2; ++component) {
        if (!qxp_vertex_float_finite(
                source + layout->texcoord_offset[0] +
                    component * sizeof(ULONG)))
            return FALSE;
    }
    return TRUE;
}

BOOL qxp_vertex_compact_packet_layout(ULONG vertex_stride,
                                      ULONG vertex_count,
                                      ULONG *request_bytes)
{
    ULONG draw_header = sizeof(QXP_D3D_DRAW_REQUEST) -
                        sizeof(QXP_D3D_VERTEX);
    ULONG compact_header = sizeof(QXP_D3D_COMPACT_VERTEX_PAYLOAD) -
                           sizeof(((QXP_D3D_COMPACT_VERTEX_PAYLOAD *)0)
                                      ->vertex_data);
    ULONG vertex_bytes;

    if (!request_bytes || !vertex_stride ||
        vertex_stride > QXP_D3D_MAX_VERTEX_STRIDE || !vertex_count ||
        vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        vertex_count > 0xffffffffUL / vertex_stride)
        return FALSE;
    vertex_bytes = vertex_count * vertex_stride;
    if (draw_header > 0xffffffffUL - compact_header ||
        vertex_bytes > 0xffffffffUL - draw_header - compact_header)
        return FALSE;
    *request_bytes = draw_header + compact_header + vertex_bytes;
    return TRUE;
}

BOOL qxp_vertex_compact_indexed_packet_layout(
    ULONG vertex_stride, ULONG vertex_count, ULONG index_count,
    ULONG *request_bytes)
{
    ULONG draw_header = sizeof(QXP_D3D_DRAW_REQUEST) -
                        sizeof(QXP_D3D_VERTEX);
    ULONG compact_header =
        sizeof(QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD) -
        sizeof(((QXP_D3D_COMPACT_INDEXED_VERTEX_PAYLOAD *)0)->data);
    ULONG vertex_bytes;
    ULONG aligned_index_bytes;

    if (!request_bytes || !vertex_stride ||
        vertex_stride > QXP_D3D_MAX_VERTEX_STRIDE || !vertex_count ||
        vertex_count > QXP_D3D_MAX_DRAW_VERTICES || !index_count ||
        index_count > QXP_D3D_MAX_DRAW_VERTICES ||
        vertex_count > index_count ||
        vertex_count > 0xffffffffUL / vertex_stride)
        return FALSE;
    vertex_bytes = vertex_count * vertex_stride;
    aligned_index_bytes =
        (index_count + QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    if (draw_header > 0xffffffffUL - compact_header ||
        vertex_bytes > 0xffffffffUL - draw_header - compact_header ||
        aligned_index_bytes >
            0xffffffffUL - draw_header - compact_header - vertex_bytes)
        return FALSE;
    *request_bytes = draw_header + compact_header + vertex_bytes +
                     aligned_index_bytes;
    return TRUE;
}

BOOL qxp_vertex_raw_indexed_object_packet_layout(
    ULONG vertex_stride, ULONG vertex_count, ULONG index_count,
    ULONG *hwtl_state_offset, ULONG *request_bytes)
{
    ULONG draw_header = sizeof(QXP_D3D_DRAW_REQUEST) -
                        sizeof(QXP_D3D_VERTEX);
    ULONG raw_header =
        sizeof(QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD) -
        sizeof(((QXP_D3D_RAW_INDEXED_OBJECT_PAYLOAD *)0)->data);
    ULONG aligned_index_bytes;
    ULONG vertex_bytes;
    ULONG bytes;

    if (!hwtl_state_offset || !request_bytes || !vertex_stride ||
        vertex_stride > QXP_D3D_MAX_VERTEX_STRIDE || !vertex_count ||
        vertex_count > QXP_D3D_MAX_DRAW_VERTICES || !index_count ||
        index_count > QXP_D3D_MAX_DRAW_VERTICES ||
        vertex_count > index_count ||
        vertex_count > 0xffffffffUL / vertex_stride)
        return FALSE;
    aligned_index_bytes =
        (index_count + QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    vertex_bytes = vertex_count * vertex_stride;
    if (draw_header > 0xffffffffUL - raw_header ||
        aligned_index_bytes >
            0xffffffffUL - draw_header - raw_header ||
        vertex_bytes >
            0xffffffffUL - draw_header - raw_header -
                aligned_index_bytes)
        return FALSE;
    bytes = draw_header + raw_header + aligned_index_bytes + vertex_bytes;
    if (bytes > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE))
        return FALSE;
    *hwtl_state_offset = bytes;
    *request_bytes = bytes + sizeof(QXP_D3D_HWTL_STATE);
    return TRUE;
}

BOOL qxp_vertex_indexed_packet_layout(
    ULONG vertex_mode, ULONG vertex_count, ULONG index_count,
    ULONG *object_data_offset, ULONG *hwtl_state_offset,
    ULONG *request_bytes)
{
    ULONG draw_header =
        sizeof(QXP_D3D_DRAW_REQUEST) -
        sizeof(((QXP_D3D_DRAW_REQUEST *)0)->vertices);
    ULONG indexed_header =
        sizeof(QXP_D3D_INDEXED_VERTEX_PAYLOAD) -
        sizeof(((QXP_D3D_INDEXED_VERTEX_PAYLOAD *)0)->data);
    ULONG aligned_index_bytes;
    ULONG vertex_bytes;
    ULONG bytes;

    if (!object_data_offset || !hwtl_state_offset || !request_bytes ||
        !vertex_count || vertex_count > 0xffUL ||
        !index_count || index_count > QXP_D3D_MAX_DRAW_VERTICES ||
        vertex_count > index_count ||
        vertex_count > 0xffffffffUL / sizeof(QXP_D3D_VERTEX) ||
        index_count > 0xffffffffUL -
            (QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL))
        return FALSE;
    aligned_index_bytes =
        (index_count + QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL) &
        ~(QXP_D3D_COMPACT_INDEX_ALIGNMENT - 1UL);
    vertex_bytes = vertex_count * sizeof(QXP_D3D_VERTEX);
    if (draw_header > 0xffffffffUL - indexed_header ||
        aligned_index_bytes >
            0xffffffffUL - draw_header - indexed_header ||
        vertex_bytes >
            0xffffffffUL - draw_header - indexed_header -
                aligned_index_bytes)
        return FALSE;
    bytes = draw_header + indexed_header + aligned_index_bytes +
            vertex_bytes;
    *object_data_offset = 0;
    *hwtl_state_offset = 0;
    if (vertex_mode == QXP_D3D_VERTEX_INDEXED_TRANSFORMED) {
        *request_bytes = bytes;
        return TRUE;
    }
    if (vertex_mode != QXP_D3D_VERTEX_INDEXED_OBJECT ||
        vertex_count > (0xffffffffUL - bytes) /
                       sizeof(QXP_D3D_OBJECT_VERTEX_DATA))
        return FALSE;
    *object_data_offset = bytes;
    bytes += vertex_count * sizeof(QXP_D3D_OBJECT_VERTEX_DATA);
    if (bytes > 0xffffffffUL - sizeof(QXP_D3D_HWTL_STATE)) return FALSE;
    *hwtl_state_offset = bytes;
    *request_bytes = bytes + sizeof(QXP_D3D_HWTL_STATE);
    return TRUE;
}

void qxp_vertex_compact_index_map_reset(ULONG *map_vertices)
{
    ULONG slot;

    if (!map_vertices) return;
    for (slot = 0; slot < QXP_VERTEX_COMPACT_MAP_SLOTS; ++slot)
        map_vertices[slot] = QXP_VERTEX_COMPACT_MAP_EMPTY;
}

BOOL qxp_vertex_compact_index_map(
    ULONG vertex_index, ULONG *unique_count, BOOL add,
    UCHAR *mapped_index, ULONG *map_vertices, UCHAR *map_indices)
{
    ULONG slot;
    ULONG probe;
    ULONG assigned;

    if (vertex_index == QXP_VERTEX_COMPACT_MAP_EMPTY || !unique_count ||
        *unique_count > QXP_D3D_MAX_DRAW_VERTICES || !mapped_index ||
        !map_vertices || !map_indices)
        return FALSE;
    slot = ((vertex_index * 0x9e3779b1UL) >> 23) &
           (QXP_VERTEX_COMPACT_MAP_SLOTS - 1);
    for (probe = 0; probe < QXP_VERTEX_COMPACT_MAP_SLOTS; ++probe) {
        ULONG current = (slot + probe) &
                        (QXP_VERTEX_COMPACT_MAP_SLOTS - 1);
        if (map_vertices[current] == vertex_index) {
            if (map_indices[current] >= *unique_count) return FALSE;
            *mapped_index = map_indices[current];
            return TRUE;
        }
        if (map_vertices[current] != QXP_VERTEX_COMPACT_MAP_EMPTY)
            continue;
        if (!add || *unique_count >= QXP_D3D_MAX_DRAW_VERTICES)
            return FALSE;
        assigned = *unique_count;
        map_vertices[current] = vertex_index;
        map_indices[current] = (UCHAR)assigned;
        *mapped_index = (UCHAR)assigned;
        *unique_count = assigned + 1;
        return TRUE;
    }
    return FALSE;
}

BOOL qxp_vertex_ring_wrap_required(
    ULONG source_offset, ULONG stride, ULONG vertex_count,
    ULONG element_bytes, ULONG source_bytes)
{
    ULONGLONG end;

    if (!stride || !vertex_count || !element_bytes ||
        element_bytes > stride || !source_bytes ||
        element_bytes > source_bytes) return FALSE;

    end = (ULONGLONG)source_offset +
          (ULONGLONG)(vertex_count - 1UL) * stride +
          element_bytes;
    return end > source_bytes && end <= 0xffffffffUL;
}

BOOL qxp_vertex_compact_indexed_densify(
    UCHAR *index_data, ULONG index_count, UCHAR *vertex_data,
    ULONG vertex_stride, ULONG vertex_count,
    const UCHAR *vertex_written, ULONG *dense_vertex_count)
{
    volatile UCHAR referenced[QXP_D3D_MAX_DRAW_VERTICES];
    UCHAR remap[QXP_D3D_MAX_DRAW_VERTICES];
    ULONG index;
    ULONG source_vertex;
    ULONG destination_vertex;
    ULONG byte;

    if (!index_data || !index_count ||
        index_count > QXP_D3D_MAX_DRAW_VERTICES || !vertex_data ||
        !vertex_stride || vertex_stride > QXP_D3D_MAX_VERTEX_STRIDE ||
        !vertex_count || vertex_count > QXP_D3D_MAX_DRAW_VERTICES ||
        !vertex_written || !dense_vertex_count)
        return FALSE;
    for (source_vertex = 0; source_vertex < vertex_count;
         ++source_vertex) {
        referenced[source_vertex] = 0;
    }
    for (index = 0; index < index_count; ++index) {
        if (index_data[index] >= vertex_count) return FALSE;
        referenced[index_data[index]] = 1;
    }
    for (source_vertex = 0; source_vertex < vertex_count;
         ++source_vertex) {
        if (referenced[source_vertex] && !vertex_written[source_vertex])
            return FALSE;
    }
    destination_vertex = 0;
    for (source_vertex = 0; source_vertex < vertex_count;
         ++source_vertex) {
        if (!referenced[source_vertex]) continue;
        remap[source_vertex] = (UCHAR)destination_vertex;
        if (destination_vertex != source_vertex) {
            for (byte = 0; byte < vertex_stride; ++byte) {
                vertex_data[destination_vertex * vertex_stride + byte] =
                    vertex_data[source_vertex * vertex_stride + byte];
            }
        }
        ++destination_vertex;
    }
    for (index = 0; index < index_count; ++index)
        index_data[index] = remap[index_data[index]];
    *dense_vertex_count = destination_vertex;
    return destination_vertex != 0;
}
