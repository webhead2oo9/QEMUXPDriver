#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxpdisp/qxp_d3d9_draw_access.h"

#define QXP_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_PARAMETER 0x80000000UL
#define QXP_INST(op, length) \
    ((DWORD)(op) | ((DWORD)(length) << D3DSI_INSTLENGTH_SHIFT))
#define QXP_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_DST(type, number, mask) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     ((DWORD)(mask) << 16))
#define QXP_SRC(type, number) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     D3DSP_NOSWIZZLE | D3DSPSM_NONE)
#define QXP_DCL_USAGE(usage, index) \
    (QXP_PARAMETER | ((DWORD)(usage) << D3DSP_DCL_USAGE_SHIFT) | \
     ((DWORD)(index) << D3DSP_DCL_USAGEINDEX_SHIFT))

#define QXP_STREAM_HANDLE_BASE 0x900UL
#define QXP_INDEX_HANDLE 0xa00UL
#define QXP_COPY_CAPACITY 2048UL

typedef struct qxp_test_points {
    WORD count;
    WORD start;
} QXP_TEST_POINTS;

typedef struct qxp_test_draw {
    DWORD primitive_type;
    DWORD vertex_start;
    DWORD primitive_count;
} QXP_TEST_DRAW;

typedef struct qxp_test_indexed {
    DWORD primitive_type;
    LONG base_vertex;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index;
    DWORD primitive_count;
} QXP_TEST_INDEXED;

typedef struct qxp_test_fan {
    DWORD first_vertex_offset;
    DWORD edge_flags;
    DWORD primitive_count;
} QXP_TEST_FAN;

typedef struct qxp_test_draw2 {
    DWORD primitive_type;
    DWORD first_vertex_offset;
    DWORD primitive_count;
} QXP_TEST_DRAW2;

typedef struct qxp_fixture {
    QXP_SHADER_OWNER owner;
    DWORD device_identity;
    QXP_D3D9_SHADER_OBJECT_TABLE table;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel_shader;
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_BACKING_OWNER backing_owner;
    DWORD stream_lengths[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    DWORD index_length;
} QXP_FIXTURE;

static QXP_FIXTURE qxp_fixture;
static QXP_D3D9_SHADER_ALLOCATOR qxp_allocator;
static DWORD qxp_allocations;
static DWORD qxp_releases;
static DWORD qxp_live_allocations;
static DWORD qxp_lock_acquires;
static DWORD qxp_lock_releases;
static DWORD qxp_lock_depth;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_backings;
static QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT qxp_envelope;
static QXP_D3D9_DRAW_ACCESS_PLAN qxp_plan;
static QXP_D3D9_DRAW_ACCESS_STATUS qxp_status;
static QXP_D3D9_DRAW_BACKING_STATUS qxp_backing_status;
static QXP_D3D9_DRAW_ENVELOPE_STATUS qxp_envelope_status;
static BYTE qxp_copy_storage[QXP_COPY_CAPACITY + 2UL];
static int qxp_failures;

static const QXP_VERTEX_ELEMENT9 qxp_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_POSITION, 0 },
    { 1, 0, QXP_VERTEX_DECLTYPE9_FLOAT1,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 0 },
    { 2, 0, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 1 },
    { 3, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 2 },
    { 4, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 3 },
    { 5, 0, QXP_VERTEX_DECLTYPE9_FLOAT1,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 4 },
    { 6, 0, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 5 },
    { 7, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 6 },
    { 8, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 7 },
    { 9, 0, QXP_VERTEX_DECLTYPE9_FLOAT1,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 8 },
    { 10, 0, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 9 },
    { 11, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 10 },
    { 12, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 11 },
    { 13, 0, QXP_VERTEX_DECLTYPE9_FLOAT1,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 12 },
    { 14, 0, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 13 },
    { 15, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_TEXCOORD, 14 }
};

static const DWORD qxp_vertex_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 0),
        QXP_DST(D3DSPR_INPUT, 1, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 1),
        QXP_DST(D3DSPR_INPUT, 2, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 2),
        QXP_DST(D3DSPR_INPUT, 3, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 3),
        QXP_DST(D3DSPR_INPUT, 4, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 4),
        QXP_DST(D3DSPR_INPUT, 5, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 5),
        QXP_DST(D3DSPR_INPUT, 6, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 6),
        QXP_DST(D3DSPR_INPUT, 7, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 7),
        QXP_DST(D3DSPR_INPUT, 8, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 8),
        QXP_DST(D3DSPR_INPUT, 9, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 9),
        QXP_DST(D3DSPR_INPUT, 10, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 10),
        QXP_DST(D3DSPR_INPUT, 11, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 11),
        QXP_DST(D3DSPR_INPUT, 12, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 12),
        QXP_DST(D3DSPR_INPUT, 13, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 13),
        QXP_DST(D3DSPR_INPUT, 14, 15),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 14),
        QXP_DST(D3DSPR_INPUT, 15, 15),
    QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_pixel_shader[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DEF, 5), QXP_DST(D3DSPR_CONST, 0, 15),
        0x3f800000UL, 0UL, 0UL, 0x3f800000UL,
    QXP_INST(D3DSIO_MOV, 2), QXP_DST(D3DSPR_COLOROUT, 0, 15),
        QXP_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_access: failed: %s\n", name);
}

static void *qxp_allocate(ULONG byte_count, void *opaque)
{
    void *result;

    (void)opaque;
    ++qxp_allocations;
    result = malloc((size_t)byte_count);
    if (result) ++qxp_live_allocations;
    return result;
}

static void qxp_release(void *allocation, void *opaque)
{
    (void)opaque;
    ++qxp_releases;
    if (allocation && qxp_live_allocations) --qxp_live_allocations;
    free(allocation);
}

static void qxp_lock(void *opaque)
{
    (void)opaque;
    ++qxp_lock_acquires;
    ++qxp_lock_depth;
}

static void qxp_unlock(void *opaque)
{
    (void)opaque;
    ++qxp_lock_releases;
    if (qxp_lock_depth) --qxp_lock_depth;
}

static BOOL qxp_lookup(
    const QXP_D3D9_DRAW_BACKING_OWNER *owner, DWORD handle, DWORD role,
    QXP_D3D9_DRAW_BACKING_METADATA *metadata, void *opaque)
{
    DWORD stream;

    (void)opaque;
    if (!owner || !metadata || !qxp_lock_depth ||
        owner->dd_local_identity != qxp_fixture.backing_owner.dd_local_identity)
        return FALSE;
    memset(metadata, 0, sizeof(*metadata));
    if (role == QXP_D3D9_DRAW_BACKING_ROLE_VERTEX &&
        handle >= QXP_STREAM_HANDLE_BASE &&
        handle < QXP_STREAM_HANDLE_BASE + 16UL) {
        stream = handle - QXP_STREAM_HANDLE_BASE;
        metadata->valid = TRUE;
        metadata->handle = handle;
        metadata->incarnation = (ULONGLONG)stream + 1;
        metadata->content_generation = stream + 11UL;
        metadata->byte_length = qxp_fixture.stream_lengths[stream];
        metadata->kind = (stream & 1UL) ?
            QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR :
            QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
        metadata->role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_VERTEX;
        metadata->video_memory_offset = (stream & 1UL) ?
            0x1000UL + stream * 0x100UL : 0;
        return TRUE;
    }
    if (role == QXP_D3D9_DRAW_BACKING_ROLE_INDEX &&
        handle == QXP_INDEX_HANDLE) {
        metadata->valid = TRUE;
        metadata->handle = handle;
        metadata->incarnation = 0x101;
        metadata->content_generation = 0x202;
        metadata->byte_length = qxp_fixture.index_length;
        metadata->kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
        metadata->role_mask = QXP_D3D9_DRAW_BACKING_ROLE_MASK_INDEX;
        return TRUE;
    }
    return FALSE;
}

static int qxp_publish_objects(void)
{
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel;
    BOOL result;

    declaration = NULL;
    vertex = NULL;
    pixel = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        &qxp_fixture.owner, 0x101UL, qxp_elements, QXP_COUNT(qxp_elements),
        &qxp_allocator, &declaration, &status);
    if (!result) return 0;
    qxp_lock(NULL);
    result = QxpD3d9PublishVertexDeclaration(
        &qxp_fixture.table, declaration, &status);
    qxp_unlock(NULL);
    if (!result) return 0;
    qxp_fixture.declaration = declaration;

    result = QxpD3d9PrepareVertexFunction(
        &qxp_fixture.owner, 0x201UL, qxp_vertex_shader,
        (DWORD)sizeof(qxp_vertex_shader), &qxp_allocator, &vertex, &status);
    if (!result) return 0;
    qxp_lock(NULL);
    result = QxpD3d9PublishVertexFunction(
        &qxp_fixture.table, vertex, &status);
    qxp_unlock(NULL);
    if (!result) return 0;
    qxp_fixture.vertex = vertex;

    result = QxpD3d9PreparePixelFunction(
        &qxp_fixture.owner, 0x301UL, qxp_pixel_shader,
        (DWORD)sizeof(qxp_pixel_shader), &qxp_allocator, &pixel, &status);
    if (!result) return 0;
    qxp_lock(NULL);
    result = QxpD3d9PublishPixelFunctionUnified(
        &qxp_fixture.table, pixel, FALSE, &status);
    qxp_unlock(NULL);
    if (!result) return 0;
    qxp_fixture.pixel_shader = pixel;
    return 1;
}

static int qxp_bind_objects(void)
{
    QXP_D3D9_SHADER_OBJECT_STATUS object_status;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING vertex_binding;
    QXP_D3D9_PIXEL_FUNCTION_ROUTE route;
    DWORD legacy_fvf;
    DWORD stream;
    DWORD span;
    DWORD stride;
    BOOL result;

    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&vertex_binding, 0, sizeof(vertex_binding));
    legacy_fvf = 0;
    route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE;
    qxp_lock(NULL);
    result = QxpD3d9SetVertexDeclaration(
        &qxp_fixture.table, &qxp_fixture.owner,
        qxp_fixture.declaration->handle, &declaration_binding,
        &legacy_fvf, &object_status);
    if (result) result = QxpD3d9SetVertexFunction(
        &qxp_fixture.table, &qxp_fixture.owner,
        qxp_fixture.vertex->handle, &vertex_binding, &object_status);
    if (result) result = QxpD3d9SetPixelFunctionUnified(
        &qxp_fixture.table, &qxp_fixture.owner,
        qxp_fixture.pixel_shader->handle, FALSE, &qxp_fixture.pixel,
        &route, &object_status);
    qxp_unlock(NULL);
    if (!result || route != QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2) return 0;
    if (!QxpD3d9DrawStateSetDeclaration(
            &qxp_fixture.draw, QXP_D3D9_DRAW_DECLARATION_OBJECT,
            &declaration_binding, legacy_fvf, &draw_status) ||
        !QxpD3d9DrawStateSetVertexFunction(
            &qxp_fixture.draw, &vertex_binding, &draw_status)) return 0;
    for (stream = 0; stream < 16; ++stream) {
        span = stream == 0 ? 12UL :
            4UL * (1UL + ((stream - 1UL) & 3UL));
        if (stream == 15) span = 16;
        stride = span + 8UL + stream;
        if (!QxpD3d9DrawStateSetStreamSource(
                &qxp_fixture.draw, stream,
                QXP_STREAM_HANDLE_BASE + stream, 1UL + stream * 3UL,
                stride, FALSE, &draw_status)) return 0;
    }
    if (!QxpD3d9DrawStateSetIndices(
            &qxp_fixture.draw, QXP_INDEX_HANDLE, 2, &draw_status)) return 0;
    return 1;
}

static int qxp_refresh_prepared(DWORD index_stride)
{
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    QXP_D3D9_DRAW_PREPARE_STATUS status;

    if (qxp_fixture.prepared) {
        QxpD3d9DestroyPreparedDraw(qxp_fixture.prepared, &qxp_allocator);
        qxp_fixture.prepared = NULL;
    }
    if (!QxpD3d9DrawStateSetIndices(
            &qxp_fixture.draw, QXP_INDEX_HANDLE,
            index_stride, &draw_status)) return 0;
    qxp_fixture.prepared = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock, NULL,
        NULL,
        &status);
    return qxp_fixture.prepared != NULL;
}

static int qxp_fixture_initialize(void)
{
    DWORD stream;

    memset(&qxp_fixture, 0, sizeof(qxp_fixture));
    qxp_shader_owner_initialize(
        &qxp_fixture.owner, 0x41UL, 0x51UL,
        &qxp_fixture.device_identity);
    QxpD3d9ShaderObjectTableInitialize(&qxp_fixture.table);
    QxpD3d9DrawStateInitialize(&qxp_fixture.draw);
    QxpD3d9ClearPixelBindingState(&qxp_fixture.pixel);
    for (stream = 0; stream < 16; ++stream)
        qxp_fixture.stream_lengths[stream] = 0x100000UL;
    qxp_fixture.index_length = 0x100000UL;
    qxp_fixture.backing_owner.shader_owner.context_handle =
        qxp_fixture.owner.context_handle;
    qxp_fixture.backing_owner.shader_owner.process_id =
        qxp_fixture.owner.process_id;
    qxp_fixture.backing_owner.shader_owner.device_identity =
        (ULONG_PTR)qxp_fixture.owner.device;
    qxp_fixture.backing_owner.dd_local_identity = 0xabcUL;
    if (!qxp_publish_objects() || !qxp_bind_objects() ||
        !qxp_refresh_prepared(2)) return 0;
    return 1;
}

static void qxp_fixture_destroy(void)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel;

    if (qxp_fixture.prepared) {
        QxpD3d9DestroyPreparedDraw(qxp_fixture.prepared, &qxp_allocator);
        qxp_fixture.prepared = NULL;
    }
    for (;;) {
        qxp_lock(NULL);
        declaration = QxpD3d9DetachOwnerVertexDeclaration(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);
    }
    for (;;) {
        qxp_lock(NULL);
        vertex = QxpD3d9DetachOwnerVertexFunction(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!vertex) break;
        QxpD3d9DestroyVertexFunction(vertex, &qxp_allocator);
    }
    for (;;) {
        qxp_lock(NULL);
        pixel = QxpD3d9DetachOwnerPixelFunction(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!pixel) break;
        QxpD3d9DestroyPixelFunction(pixel, &qxp_allocator);
    }
}

static BOOL qxp_make_envelope(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes)
{
    BOOL indexed;
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE um;

    indexed = opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
              opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2;
    memset(&um, 0, sizeof(um));
    if (!QxpD3d9SnapshotDrawBackings(
            qxp_fixture.prepared, &qxp_fixture.draw,
            &qxp_fixture.backing_owner, indexed, &um, qxp_lookup, NULL,
            qxp_lock, qxp_unlock, NULL, &qxp_backings,
            &qxp_backing_status)) return FALSE;
    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes,
        qxp_fixture.prepared, &qxp_backings, &qxp_envelope,
        &qxp_envelope_status);
}

static ULONG qxp_copy_length(void)
{
    if (!qxp_envelope.index.valid) return 0;
    return (ULONG)(qxp_envelope.index.byte_last_exclusive -
                   qxp_envelope.index.byte_first);
}

static void qxp_write_index(
    BYTE *copy, ULONGLONG absolute_byte, DWORD stride, DWORD value)
{
    ULONG offset;

    offset = (ULONG)(absolute_byte - qxp_envelope.index.byte_first);
    copy[offset] = (BYTE)(value & 0xffUL);
    copy[offset + 1] = (BYTE)((value >> 8) & 0xffUL);
    if (stride == 4) {
        copy[offset + 2] = (BYTE)((value >> 16) & 0xffUL);
        copy[offset + 3] = (BYTE)((value >> 24) & 0xffUL);
    }
}

static BOOL qxp_build_access(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const void *copy, ULONG copy_length)
{
    return QxpD3d9BuildDrawAccess(
        &qxp_envelope, opcode, record_count, records, available_bytes,
        qxp_fixture.prepared, &qxp_backings, copy, copy_length,
        &qxp_plan, &qxp_status);
}

static int qxp_plan_zero(void)
{
    const BYTE *bytes;
    ULONG index;

    bytes = (const BYTE *)&qxp_plan;
    for (index = 0; index < (ULONG)sizeof(qxp_plan); ++index) {
        if (bytes[index]) return 0;
    }
    return 1;
}

static int qxp_bytes_zero(const void *value, ULONG byte_count)
{
    const BYTE *bytes;
    ULONG index;

    bytes = (const BYTE *)value;
    for (index = 0; index < byte_count; ++index) {
        if (bytes[index]) return 0;
    }
    return 1;
}

static int qxp_status_zero(void)
{
    return qxp_bytes_zero(&qxp_status, (ULONG)sizeof(qxp_status));
}

static void qxp_test_real_foundation_and_nonindexed(void)
{
    QXP_TEST_POINTS points;
    QXP_TEST_DRAW draw;
    QXP_TEST_FAN fan;
    QXP_TEST_DRAW2 draw2;
    DWORD stream;
    DWORD alloc_before;
    DWORD lock_before;

    qxp_check(qxp_fixture.prepared != NULL &&
                  qxp_fixture.prepared->valid &&
                  qxp_fixture.prepared->required_stream_mask == 0xffffUL,
              "real B6E builder requires all sixteen streams");

    points.count = 2;
    points.start = 3;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points)),
              "real B6G1 and B6G2 POINTS foundation");
    alloc_before = qxp_allocations;
    lock_before = qxp_lock_acquires;
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
                  NULL, 0) && qxp_plan.valid && !qxp_plan.indexed &&
                  qxp_plan.scanned_index_count.low == 0 &&
                  qxp_plan.scanned_index_count.high == 0 &&
                  memcmp(qxp_plan.streams, qxp_envelope.streams,
                         sizeof(qxp_plan.streams)) == 0 &&
                  memcmp(&qxp_plan.index, &qxp_envelope.index,
                         sizeof(qxp_plan.index)) == 0 &&
                  qxp_allocations == alloc_before &&
                  qxp_lock_acquires == lock_before,
              "POINTS carries B6G2 ranges without allocation or lock");
    for (stream = 0; stream < 16; ++stream) {
        QXP_D3D9_DRAW_STREAM_ENVELOPE *range;
        QXP_D3D9_DRAW_BACKING_RECORD *backing;

        range = &qxp_plan.streams[stream];
        backing = &qxp_backings.streams[stream];
        qxp_check(range->valid && range->stream == stream &&
                      range->logical_first == 3 &&
                      range->logical_last_exclusive == 5 &&
                      range->byte_first ==
                          (ULONGLONG)backing->source2_offset +
                          (ULONGLONG)3 * backing->stride &&
                      range->byte_last_exclusive ==
                          (ULONGLONG)backing->source2_offset +
                          (ULONGLONG)4 * backing->stride +
                          backing->required_span,
                  "all streams keep SOURCE2 stride and declaration span");
    }

    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw.vertex_start = 2;
    draw.primitive_count = 2;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1,
                  &draw, sizeof(draw)) &&
                  qxp_build_access(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1,
                      &draw, sizeof(draw), NULL, 0),
              "DRAWPRIMITIVE carries exact B6G2 streams");

    fan.first_vertex_offset =
        qxp_fixture.prepared->raw_state.streams[0].stride * 2UL;
    fan.edge_flags = 0x55UL;
    fan.primitive_count = 2;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1,
                  &fan, sizeof(fan)) &&
                  qxp_build_access(
                      QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1,
                      &fan, sizeof(fan), NULL, 0),
              "CLIPPEDTRIANGLEFAN carries exact B6G2 streams");

    draw2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLESTRIP;
    draw2.first_vertex_offset =
        qxp_fixture.prepared->raw_state.streams[0].stride * 2UL;
    draw2.primitive_count = 2;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
                  &draw2, sizeof(draw2)) &&
                  qxp_build_access(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
                      &draw2, sizeof(draw2), NULL, 0),
              "DRAWPRIMITIVE2 carries exact B6G2 streams");
}

static void qxp_reset_lengths(void)
{
    DWORD stream;

    for (stream = 0; stream < 16; ++stream)
        qxp_fixture.stream_lengths[stream] = 0x100000UL;
    qxp_fixture.index_length = 0x100000UL;
}

static void qxp_expect_access_failure(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const void *copy, ULONG copy_length,
    DWORD code, DWORD record_index, DWORD ordinal, const char *name)
{
    BOOL result;
    DWORD field;

    field = code == QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_VALUE ?
        QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_VALUE :
        QXP_D3D9_DRAW_ACCESS_FIELD_INDEX_COPY;
    memset(&qxp_plan, 0xcc, sizeof(qxp_plan));
    memset(&qxp_status, 0xcc, sizeof(qxp_status));
    result = qxp_build_access(
        opcode, record_count, records, available_bytes, copy, copy_length);
    qxp_check(!result && qxp_status.code == code &&
                  qxp_status.record_index == record_index &&
                  qxp_status.index_ordinal == ordinal &&
                  qxp_status.stream == 0 && qxp_status.field == field &&
                  qxp_bytes_zero(&qxp_status.envelope_error,
                                 (ULONG)sizeof(qxp_status.envelope_error)) &&
                  qxp_plan_zero(),
              name);
}

static void qxp_test_indexed_forms_and_bases(void)
{
    QXP_TEST_INDEXED record;
    BYTE *copy;
    ULONG length;
    DWORD stride0;

    qxp_check(qxp_refresh_prepared(2), "refresh B6E for 16-bit indices");
    record.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    record.base_vertex = 3;
    record.min_index = 4;
    record.num_vertices = 10;
    record.start_index = 5;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build indexed 16-bit B6G2 envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(qxp_copy_storage, 0x5a, sizeof(qxp_copy_storage));
    qxp_write_index(copy, 10, 2, 7);
    qxp_write_index(copy, 12, 2, 4);
    qxp_write_index(copy, 14, 2, 9);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.scanned_index_count.low == 3 &&
                  qxp_plan.scanned_index_count.high == 0 &&
                  qxp_plan.streams[0].logical_first == 7 &&
                  qxp_plan.streams[0].logical_last_exclusive == 13 &&
                  qxp_envelope.streams[0].logical_first == 7 &&
                  qxp_envelope.streams[0].logical_last_exclusive == 17 &&
                  qxp_copy_storage[0] == 0x5a &&
                  qxp_copy_storage[length + 1] == 0x5a,
              "opcode 6 tight positive base and 16-bit unaligned copy");

    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = -3;
    record.min_index = 3;
    record.num_vertices = 6;
    record.start_index = 0;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build negative index-unit base envelope");
    length = qxp_copy_length();
    memset(copy, 0, length);
    qxp_write_index(copy, 0, 2, 3);
    qxp_write_index(copy, 2, 2, 8);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.streams[0].logical_first == 0 &&
                  qxp_plan.streams[0].logical_last_exclusive == 6,
              "opcode 6 negative effective base stays nonnegative");

    qxp_check(qxp_refresh_prepared(4), "refresh B6E for 32-bit indices");
    stride0 = qxp_fixture.prepared->raw_state.streams[0].stride;
    record.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    record.base_vertex = (LONG)(stride0 * 2UL);
    record.min_index = 0;
    record.num_vertices = 20;
    record.start_index = 3;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record)),
              "build DIP2 unaligned byte-offset envelope");
    length = qxp_copy_length();
    memset(qxp_copy_storage, 0x7b, sizeof(qxp_copy_storage));
    qxp_write_index(copy, 3, 4, 4);
    qxp_write_index(copy, 7, 4, 10);
    qxp_write_index(copy, 11, 4, 6);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.index.byte_first == 3 &&
                  qxp_plan.index.byte_last_exclusive == 15 &&
                  qxp_plan.streams[0].logical_first == 6 &&
                  qxp_plan.streams[0].logical_last_exclusive == 13 &&
                  qxp_copy_storage[0] == 0x7b &&
                  qxp_copy_storage[length + 1] == 0x7b,
              "opcode 15 accepts unaligned offset and exact copy canaries");

    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = -(LONG)(stride0 * 2UL);
    record.min_index = 2;
    record.num_vertices = 6;
    record.start_index = 1;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record)),
              "build DIP2 negative byte base envelope");
    length = qxp_copy_length();
    memset(copy, 0, length);
    qxp_write_index(copy, 1, 4, 2);
    qxp_write_index(copy, 5, 4, 7);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.streams[0].logical_first == 0 &&
                  qxp_plan.streams[0].logical_last_exclusive == 6,
              "opcode 15 normalizes negative base by stream0 stride");
}

static void qxp_test_full_width_and_large_index_start(void)
{
    QXP_TEST_INDEXED record;
    BYTE *copy;
    ULONG length;
    DWORD stream;

    qxp_check(qxp_refresh_prepared(2),
              "refresh full-width 16-bit fixture");
    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = 0;
    record.min_index = 0x1200UL;
    record.num_vertices = 0x1200UL;
    record.start_index = 2;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build full-width 16-bit envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 4, 2, 0x1234UL);
    qxp_write_index(copy, 6, 2, 0x2345UL);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.streams[0].logical_first == 0x1234UL &&
                  qxp_plan.streams[0].logical_last_exclusive ==
                      0x2346UL && qxp_status_zero(),
              "16-bit decoder consumes nonzero high bytes exactly");

    qxp_fixture.index_length = 0xfffffffeUL;
    record.min_index = 0x1230UL;
    record.num_vertices = 0x20UL;
    record.start_index = 0x7ffffffdUL;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)) &&
                  qxp_envelope.index.byte_first ==
                      (ULONGLONG)0xfffffffaUL &&
                  qxp_envelope.index.byte_last_exclusive ==
                      (ULONGLONG)0xfffffffeUL,
              "build four-byte copy near ULONG index boundary");
    length = qxp_copy_length();
    memset(copy, 0, length);
    qxp_write_index(
        copy, (ULONGLONG)0xfffffffaUL, 2, 0x1234UL);
    qxp_write_index(
        copy, (ULONGLONG)0xfffffffcUL, 2, 0x1235UL);
    qxp_check(length == 4 && qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.index.byte_first ==
                      (ULONGLONG)0xfffffffaUL &&
                  qxp_plan.index.byte_last_exclusive ==
                      (ULONGLONG)0xfffffffeUL,
              "small exact copy maps safely near ULONG boundary");
    qxp_fixture.index_length = 0x100000UL;

    qxp_check(qxp_refresh_prepared(4),
              "refresh full-width 32-bit fixture");
    for (stream = 0; stream < 16; ++stream)
        qxp_fixture.stream_lengths[stream] = 0x40000000UL;
    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = 0;
    record.min_index = 0x01230000UL;
    record.num_vertices = 0x00010000UL;
    record.start_index = 3;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record)),
              "build full-width 32-bit envelope");
    length = qxp_copy_length();
    memset(copy, 0, length);
    qxp_write_index(copy, 3, 4, 0x01234567UL);
    qxp_write_index(copy, 7, 4, 0x01235678UL);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, 1,
                  &record, sizeof(record), copy, length) &&
                  qxp_plan.streams[0].logical_first ==
                      (ULONGLONG)0x01234567UL &&
                  qxp_plan.streams[0].logical_last_exclusive ==
                      (ULONGLONG)0x01235679UL && qxp_status_zero(),
              "32-bit decoder consumes all four nonzero bytes exactly");
    qxp_reset_lengths();
}

static void qxp_test_shapes_records_and_count(void)
{
    static const DWORD types[6] = { 1, 2, 3, 4, 5, 6 };
    static const DWORD starts[6] = { 0, 5, 2, 15, 12, 22 };
    QXP_TEST_INDEXED records[7];
    BYTE *copy;
    ULONG length;
    DWORD index;
    DWORD position;

    qxp_check(qxp_refresh_prepared(2), "refresh 16-bit shape fixture");
    memset(records, 0, sizeof(records));
    for (index = 0; index < 6; ++index) {
        records[index].primitive_type = types[index];
        records[index].base_vertex = 1;
        records[index].min_index = 0;
        records[index].num_vertices = 64;
        records[index].start_index = starts[index];
        records[index].primitive_count = 2;
    }
    records[6].primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLEFAN;
    records[6].base_vertex = (LONG)0x80000000UL;
    records[6].min_index = 0xffffffffUL;
    records[6].num_vertices = 0;
    records[6].start_index = 0xffffffffUL;
    records[6].primitive_count = 0;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 7,
                  records, sizeof(records)),
              "build six-shape gapped overlapping B6G2 envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    for (position = 0; position < length / 2UL; ++position)
        qxp_write_index(copy, (ULONGLONG)position * 2, 2, position + 2UL);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 7,
                  records, sizeof(records), copy, length) &&
                  qxp_plan.record_count == 7 &&
                  qxp_plan.nonempty_record_count == 6 &&
                  qxp_plan.scanned_index_count.low == 23 &&
                  qxp_plan.scanned_index_count.high == 0 &&
                  qxp_plan.streams[0].logical_first == 3 &&
                  qxp_plan.streams[0].logical_last_exclusive == 29,
              "all source shapes count gapped overlapping records exactly");
}

static void qxp_test_index_promises_and_copy_contract(void)
{
    QXP_TEST_INDEXED records[2];
    BYTE *copy;
    ULONG length;

    qxp_check(qxp_refresh_prepared(2), "refresh promise fixture");
    memset(records, 0, sizeof(records));
    records[0].primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    records[0].base_vertex = 0;
    records[0].min_index = 5;
    records[0].num_vertices = 4;
    records[0].start_index = 0;
    records[0].primitive_count = 1;
    records[1] = records[0];
    records[1].start_index = 2;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2,
                  records, sizeof(records)),
              "build broad promise diagnostic envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 0, 2, 5);
    qxp_write_index(copy, 2, 2, 8);
    qxp_write_index(copy, 4, 2, 4);
    qxp_write_index(copy, 6, 2, 7);
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2, records,
        sizeof(records), copy, length,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_VALUE, 1, 0,
        "below broad MinIndex reports exact record and ordinal");
    qxp_write_index(copy, 4, 2, 6);
    qxp_write_index(copy, 6, 2, 9);
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2, records,
        sizeof(records), copy, length,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_VALUE, 1, 1,
        "exact exclusive broad end reports exact record and ordinal");

    qxp_write_index(copy, 6, 2, 7);
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2, records,
        sizeof(records), NULL, length,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY, 0, 0,
        "null exact-length copy rejected");
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2, records,
        sizeof(records), copy, length - 1,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY, 0, 0,
        "short index copy rejected");
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2,
                  records, sizeof(records), copy, length),
              "exact index copy accepted");
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 2, records,
        sizeof(records), copy, length + 1,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY, 0, 0,
        "long index copy rejected");

    records[0].primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    records[0].primitive_count = 0;
    records[0].base_vertex = (LONG)0x80000000UL;
    records[0].min_index = 0xffffffffUL;
    records[0].num_vertices = 0;
    records[0].start_index = 0xffffffffUL;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  records, sizeof(records[0])) &&
                  !qxp_envelope.index.valid &&
                  qxp_build_access(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                      records, sizeof(records[0]), NULL, 0) &&
                  qxp_plan.scanned_index_count.low == 0 &&
                  !qxp_plan.streams[0].valid,
              "zero source record reads no index and accepts null zero");
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, records,
        sizeof(records[0]), copy, 0,
        QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY, 0, 0,
        "zero source record rejects nonnull zero copy");
}

static void qxp_test_exact_backing_ends(void)
{
    QXP_TEST_INDEXED record;
    BYTE *copy;
    ULONG length;
    DWORD stream;
    DWORD stride;
    DWORD span;
    DWORD offset;

    qxp_check(qxp_refresh_prepared(4), "refresh exact-end fixture");
    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = 0;
    record.min_index = 2;
    record.num_vertices = 3;
    record.start_index = 3;
    record.primitive_count = 1;
    for (stream = 0; stream < 16; ++stream) {
        stride = qxp_fixture.prepared->raw_state.streams[stream].stride;
        span = qxp_fixture.prepared->required_stream_spans[stream];
        offset = qxp_fixture.prepared->raw_state.streams[stream].offset;
        qxp_fixture.stream_lengths[stream] = offset + 4UL * stride + span;
    }
    qxp_fixture.index_length = 20;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "B6G2 accepts exact stream and index backing ends");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 12, 4, 2);
    qxp_write_index(copy, 16, 4, 4);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length),
              "exact aggregate backing end succeeds");
    for (stream = 0; stream < 16; ++stream) {
        qxp_check(qxp_plan.streams[stream].byte_last_exclusive ==
                      qxp_backings.streams[stream].byte_length,
                  "every exact stream end equals backing length");
    }
    qxp_check(qxp_plan.index.byte_last_exclusive ==
                  qxp_backings.index.byte_length,
              "exact index end equals backing length");
    qxp_reset_lengths();
}

static void qxp_expect_envelope_failure_status(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, ULONG copy_length, const char *name)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS expected;
    BOOL result;

    memset(&expected, 0xcc, sizeof(expected));
    result = QxpD3d9ValidateDrawEnvelope(
        &qxp_envelope, opcode, record_count, records, available_bytes,
        qxp_fixture.prepared, &qxp_backings, &expected);
    qxp_check(!result, "direct B6G2 failure fixture is discriminating");
    memset(&qxp_plan, 0xcc, sizeof(qxp_plan));
    memset(&qxp_status, 0xcc, sizeof(qxp_status));
    result = qxp_build_access(
        opcode, record_count, records, available_bytes, NULL, copy_length);
    qxp_check(!result && qxp_plan_zero() &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE &&
                  qxp_status.record_index == expected.record_index &&
                  qxp_status.index_ordinal == 0 &&
                  qxp_status.stream == expected.stream &&
                  qxp_status.field == QXP_D3D9_DRAW_ACCESS_FIELD_ENVELOPE &&
                  memcmp(&qxp_status.envelope_error, &expected,
                         sizeof(expected)) == 0,
              name);
}

static void qxp_test_foundation_precedes_index_reads(void)
{
    QXP_TEST_INDEXED record;
    QXP_TEST_INDEXED changed_record;
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT saved_envelope;
    QXP_D3D9_DRAW_BACKING_SNAPSHOT saved_backings;
    BYTE *copy;
    ULONG length;

    qxp_check(qxp_refresh_prepared(2), "refresh precedence fixture");
    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = 0;
    record.min_index = 1;
    record.num_vertices = 8;
    record.start_index = 2;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build precedence envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 4, 2, 2);
    qxp_write_index(copy, 6, 2, 4);
    saved_envelope = qxp_envelope;
    saved_backings = qxp_backings;

    qxp_envelope.streams[0].byte_first++;
    qxp_expect_envelope_failure_status(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &record,
        sizeof(record), length,
        "changed B6G2 envelope preserves exact nested status");
    qxp_envelope = saved_envelope;

    changed_record = record;
    changed_record.primitive_type = 0;
    qxp_expect_envelope_failure_status(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &changed_record,
        sizeof(changed_record), length,
        "malformed record preserves exact nested status");
    qxp_expect_envelope_failure_status(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &record,
        sizeof(record) - 1, length,
        "truncated record preserves exact nested status");
    qxp_expect_envelope_failure_status(
        0xffU, 1, &record, sizeof(record), length,
        "invalid opcode preserves exact nested status");

    qxp_backings.streams[0].byte_length =
        (DWORD)qxp_envelope.streams[0].byte_last_exclusive - 1UL;
    qxp_expect_envelope_failure_status(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &record,
        sizeof(record), length,
        "short changed backing preserves exact nested status");
    qxp_backings = saved_backings;
    qxp_backings.index.byte_length =
        (DWORD)qxp_envelope.index.byte_last_exclusive - 1UL;
    qxp_expect_envelope_failure_status(
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1, &record,
        sizeof(record), length,
        "changed index backing preserves exact nested status");
    qxp_backings = saved_backings;
}

static int qxp_validate_current(
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan, BYTE opcode,
    DWORD record_count, const void *records, ULONG available_bytes,
    const void *copy, ULONG copy_length)
{
    return QxpD3d9ValidateDrawAccess(
        plan, &qxp_envelope, opcode, record_count, records,
        available_bytes, qxp_fixture.prepared, &qxp_backings,
        copy, copy_length, &qxp_status) ? 1 : 0;
}

static void qxp_expect_validation_mutation(
    const QXP_D3D9_DRAW_ACCESS_PLAN *mutated,
    const QXP_TEST_INDEXED *record, const BYTE *copy, ULONG length,
    const char *name)
{
    qxp_check(!qxp_validate_current(
                  mutated, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, record, sizeof(*record), copy, length) &&
                  qxp_status.code == QXP_D3D9_DRAW_ACCESS_ERROR_VALIDATION,
              name);
}

static void qxp_test_validator_and_mutations(void)
{
    QXP_TEST_INDEXED record;
    QXP_TEST_INDEXED changed_record;
    QXP_D3D9_DRAW_ACCESS_PLAN saved_plan;
    QXP_D3D9_DRAW_ACCESS_PLAN mutated;
    QXP_D3D9_DRAW_BACKING_SNAPSHOT saved_backings;
    BYTE *copy;
    ULONG length;
    BYTE *padding;
    size_t padding_offset;

    qxp_check(qxp_refresh_prepared(2), "refresh validator fixture");
    record.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    record.base_vertex = 2;
    record.min_index = 3;
    record.num_vertices = 10;
    record.start_index = 0;
    record.primitive_count = 1;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record)),
              "build validator envelope");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 0, 2, 3);
    qxp_write_index(copy, 2, 2, 8);
    qxp_write_index(copy, 4, 2, 5);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  &record, sizeof(record), copy, length),
              "build canonical validation plan");
    saved_plan = qxp_plan;
    qxp_check(qxp_validate_current(
                  &saved_plan, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, &record, sizeof(record), copy, length),
              "canonical plan validates");

    mutated = saved_plan;
    mutated.valid = FALSE;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects valid scalar mutation");
    mutated = saved_plan;
    mutated.opcode++;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects opcode mutation");
    mutated = saved_plan;
    mutated.record_count++;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects count mutation");
    mutated = saved_plan;
    mutated.nonempty_record_count = 0;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects nonempty mutation");
    mutated = saved_plan;
    mutated.indexed = FALSE;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects indexed mutation");
    mutated = saved_plan;
    mutated.stream_mask ^= 1UL;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects stream mask mutation");
    mutated = saved_plan;
    mutated.scanned_index_count.low++;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects scanned count mutation");
    mutated = saved_plan;
    mutated.streams[0].byte_first++;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects exact range mutation");
    mutated = saved_plan;
    mutated.index.byte_first++;
    qxp_expect_validation_mutation(&mutated, &record, copy, length,
                                   "validator rejects index range mutation");

    padding_offset = offsetof(QXP_D3D9_DRAW_INDEX_ENVELOPE, byte_first);
    qxp_check(padding_offset > sizeof(BOOL),
              "index envelope exposes canonical padding byte");
    if (padding_offset > sizeof(BOOL)) {
        mutated = saved_plan;
        padding = (BYTE *)&mutated.index;
        padding[sizeof(BOOL)] = 1;
        qxp_expect_validation_mutation(
            &mutated, &record, copy, length,
            "validator byte-compares zero-initialized padding");
    }

    qxp_write_index(copy, 2, 2, 5);
    qxp_write_index(copy, 4, 2, 8);
    qxp_check(qxp_validate_current(
                  &saved_plan, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, &record, sizeof(record), copy, length),
              "different immutable indices with same extrema validate");
    qxp_write_index(copy, 4, 2, 7);
    qxp_check(!qxp_validate_current(
                  &saved_plan, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, &record, sizeof(record), copy, length) &&
                  qxp_status.code == QXP_D3D9_DRAW_ACCESS_ERROR_VALIDATION,
              "changed index extrema fail validation");

    qxp_write_index(copy, 2, 2, 8);
    qxp_write_index(copy, 4, 2, 5);
    changed_record = record;
    changed_record.base_vertex = 3;
    qxp_check(!qxp_validate_current(
                  &saved_plan, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, &changed_record, sizeof(changed_record), copy, length),
              "record change between build and validation rejected");
    saved_backings = qxp_backings;
    qxp_backings.streams[0].byte_length =
        (DWORD)qxp_envelope.streams[0].byte_last_exclusive - 1UL;
    qxp_check(!qxp_validate_current(
                  &saved_plan, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
                  1, &record, sizeof(record), copy, length) &&
                  qxp_status.code == QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE,
              "backing change between build and validation rejected");
    qxp_backings = saved_backings;
}

static void qxp_test_nonindexed_copy_contract(void)
{
    QXP_TEST_POINTS points;
    BYTE byte;

    points.count = 1;
    points.start = 0;
    byte = 0;
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points)),
              "build nonindexed copy-contract envelope");
    qxp_expect_access_failure(
        QXP_D3D9_DP2OP_POINTS, 1, &points, sizeof(points),
        &byte, 0, QXP_D3D9_DRAW_ACCESS_ERROR_INDEX_COPY, 0, 0,
        "nonindexed plan requires null zero index copy");
}

static void qxp_test_unaligned_records_and_nulls(void)
{
    QXP_TEST_INDEXED record;
    BYTE guarded_record[sizeof(QXP_TEST_INDEXED) + 2UL];
    BYTE *records;
    BYTE *copy;
    ULONG length;
    BOOL result;

    qxp_check(qxp_refresh_prepared(2), "refresh unaligned record fixture");
    record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    record.base_vertex = 1;
    record.min_index = 2;
    record.num_vertices = 5;
    record.start_index = 1;
    record.primitive_count = 1;
    memset(guarded_record, 0xa7, sizeof(guarded_record));
    records = guarded_record + 1;
    memcpy(records, &record, sizeof(record));
    qxp_check(qxp_make_envelope(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  records, sizeof(record)),
              "B6G2 accepts unaligned indexed record bytes");
    length = qxp_copy_length();
    copy = qxp_copy_storage + 1;
    memset(copy, 0, length);
    qxp_write_index(copy, 2, 2, 2);
    qxp_write_index(copy, 4, 2, 6);
    qxp_check(qxp_build_access(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, 1,
                  records, sizeof(record), copy, length) &&
                  guarded_record[0] == 0xa7 &&
                  guarded_record[sizeof(guarded_record) - 1] == 0xa7,
              "planner byte-reads unaligned records without canary damage");

    memset(&qxp_plan, 0xcc, sizeof(qxp_plan));
    result = QxpD3d9BuildDrawAccess(
        &qxp_envelope, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        1, NULL, sizeof(record), qxp_fixture.prepared, &qxp_backings,
        copy, length, &qxp_plan, &qxp_status);
    qxp_check(!result && qxp_plan_zero() &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_ACCESS_ERROR_NULL_ARGUMENT,
              "null records fail with fully zero output");

    memset(&qxp_plan, 0xcc, sizeof(qxp_plan));
    result = QxpD3d9BuildDrawAccess(
        NULL, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        1, records, sizeof(record), qxp_fixture.prepared, &qxp_backings,
        copy, length, &qxp_plan, &qxp_status);
    qxp_check(!result && qxp_plan_zero(),
              "null envelope fails with fully zero output");

    memset(&qxp_plan, 0xcc, sizeof(qxp_plan));
    result = QxpD3d9BuildDrawAccess(
        &qxp_envelope, QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        1, records, sizeof(record), qxp_fixture.prepared, &qxp_backings,
        copy, length, &qxp_plan, NULL);
    qxp_check(!result && qxp_plan_zero(),
              "null status still leaves output fully zero");
}

int main(void)
{
    memset(&qxp_allocator, 0, sizeof(qxp_allocator));
    qxp_allocator.allocate = qxp_allocate;
    qxp_allocator.release = qxp_release;

    qxp_check(qxp_fixture_initialize(), "initialize real builder fixture");
    if (qxp_fixture.prepared) {
        qxp_test_real_foundation_and_nonindexed();
        qxp_test_indexed_forms_and_bases();
        qxp_test_full_width_and_large_index_start();
        qxp_test_shapes_records_and_count();
        qxp_test_index_promises_and_copy_contract();
        qxp_test_exact_backing_ends();
        qxp_test_foundation_precedes_index_reads();
        qxp_test_validator_and_mutations();
        qxp_test_nonindexed_copy_contract();
        qxp_test_unaligned_records_and_nulls();
    }
    qxp_fixture_destroy();

    qxp_check(qxp_lock_depth == 0 &&
                  qxp_lock_acquires == qxp_lock_releases,
              "fixture locks balance");
    qxp_check(qxp_live_allocations == 0 &&
                  qxp_allocations == qxp_releases,
              "fixture allocations balance");
    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw access tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw access tests: PASS\n");
    return 0;
}
