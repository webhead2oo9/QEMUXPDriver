#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxpdisp/qxp_d3d9_draw_capture.h"

/*
 * Deterministic strict-C89 standalone test for B6G3B1 (live draw capture).
 *
 * The headline positive coverage drives a REAL minimal SM2 fixture whose
 * declaration and vertex shader require only stream 0, whose stream-0 vertex
 * source is a USER_MEMORY_CALL block, through the production B6E/B6G1/B6G2/B6G3A
 * functions and then QxpD3d9CaptureNonindexedDraw.  Precise cap-boundary and
 * rejection matrices use a canonically consistent synthetic B6G2 foundation
 * (hand-built prepared/backing snapshots fed to the production BuildDrawEnvelope
 * so the capture module re-validates them from its own inputs).
 *
 * The capture allocator wraps malloc with head/tail canaries, fills every
 * allocation with a nonzero pattern (so the module's own zeroing is exercised),
 * injects allocation failure, and tracks the lock depth at allocate/release.
 * The secure/unsecure/copy callbacks record exact addresses, byte counts and
 * ordering, hand out distinct opaque handles, can fail each stage, and assert
 * the lock depth is zero (capture holds no lock across a callback).
 */

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

/* Real fixture: single stream-0 user-memory vertex block. */
#define QXP_REAL_STRIDE 16UL
#define QXP_REAL_SPAN 12UL
#define QXP_REAL_VSIZE 16UL
#define QXP_REAL_VLEN 256UL
#define QXP_REAL_UM_BYTES (QXP_REAL_VSIZE * QXP_REAL_VLEN)
#define QXP_REAL_UM_OFFSET 32UL
#define QXP_REAL_GUARD 16UL
#define QXP_REAL_PREFIX (QXP_REAL_GUARD + QXP_REAL_UM_OFFSET)
#define QXP_REAL_GUARD_BYTE 0xC3

/* Capture allocator layout: [header 8][head canary 8][payload][tail canary]. */
#define QXP_CAP_CANARY 8UL
#define QXP_CAP_OFFSET 16UL
#define QXP_CAP_HEAD_BYTE 0xA5
#define QXP_CAP_TAIL_BYTE 0x5A
#define QXP_CAP_FILL_BYTE 0xDD
#define QXP_CAP_ALLOC_MAGIC 0x43415043UL
#define QXP_CB_MAX 20UL

typedef struct qxp_test_points {
    WORD count;
    WORD start;
} QXP_TEST_POINTS;

typedef struct qxp_test_draw {
    DWORD primitive_type;
    DWORD vertex_start;
    DWORD primitive_count;
} QXP_TEST_DRAW;

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

typedef struct qxp_test_indexed {
    DWORD primitive_type;
    LONG base_vertex;
    DWORD min_index;
    DWORD num_vertices;
    DWORD start_index;
    DWORD primitive_count;
} QXP_TEST_INDEXED;

typedef struct qxp_cap_alloc_header {
    ULONG size;
    ULONG magic;
} QXP_CAP_ALLOC_HEADER;

/* Callback recorder shared by every capture attempt. */
typedef struct qxp_cb_recorder {
    DWORD secure_count;
    const void *secure_addr[QXP_CB_MAX];
    ULONG secure_bytes[QXP_CB_MAX];
    void *secure_handle[QXP_CB_MAX];
    DWORD unsecure_count;
    void *unsecure_handle[QXP_CB_MAX];
    DWORD copy_count;
    void *copy_dest[QXP_CB_MAX];
    const void *copy_src[QXP_CB_MAX];
    ULONG copy_bytes[QXP_CB_MAX];
    DWORD fail_secure;
    DWORD fail_copy;
    BOOL corrupt_records;
    DWORD lock_violation;
    void *tokens[QXP_CB_MAX];
    DWORD token_next;
} QXP_CB_RECORDER;

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
} QXP_FIXTURE;

static QXP_FIXTURE qxp_fixture;

static QXP_D3D9_SHADER_ALLOCATOR qxp_plain_allocator;
static DWORD qxp_plain_alloc;
static DWORD qxp_plain_release;
static DWORD qxp_plain_live;

static QXP_D3D9_SHADER_ALLOCATOR qxp_cap_allocator;
static DWORD qxp_cap_alloc;
static DWORD qxp_cap_release;
static DWORD qxp_cap_live;
static DWORD qxp_cap_canary_fail;
static DWORD qxp_cap_lock_violation;
static BOOL qxp_cap_alloc_fail;

static DWORD qxp_lock_acquires;
static DWORD qxp_lock_releases;
static DWORD qxp_lock_depth;

static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_backings;
static QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT qxp_envelope;
static QXP_D3D9_DRAW_BACKING_STATUS qxp_backing_status;
static QXP_D3D9_DRAW_ENVELOPE_STATUS qxp_envelope_status;

static QXP_D3D9_DRAW_PREPARED qxp_syn_prepared;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_syn_backings;
static QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT qxp_syn_envelope;

static QXP_D3D9_DRAW_PREPARED qxp_multi_prepared;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_multi_backings;
static QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT qxp_multi_envelope;
static QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
    qxp_multi_sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];

static QXP_CB_RECORDER qxp_recorder;

static BYTE qxp_um_buffer[
    QXP_REAL_PREFIX + QXP_REAL_UM_BYTES + QXP_REAL_GUARD];
static BYTE *qxp_um_base;
static BYTE qxp_syn_buffer[4096];
static BYTE qxp_multi_buffer[QXP_D3D9_DRAW_BACKING_STREAM_COUNT][4096];

static int qxp_failures;

static const QXP_VERTEX_ELEMENT9 qxp_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT, QXP_VERTEX_DECLUSAGE9_POSITION, 0 }
};

static const DWORD qxp_vertex_shader[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15),
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
    fprintf(stderr, "qxp_d3d9_draw_capture: failed: %s\n", name);
}

/* -- allocators / lock ---------------------------------------------------- */

static void *qxp_plain_allocate(ULONG byte_count, void *opaque)
{
    void *result;

    (void)opaque;
    ++qxp_plain_alloc;
    result = malloc((size_t)byte_count);
    if (result) ++qxp_plain_live;
    return result;
}

static void qxp_plain_free(void *allocation, void *opaque)
{
    (void)opaque;
    ++qxp_plain_release;
    if (allocation && qxp_plain_live) --qxp_plain_live;
    free(allocation);
}

static void *qxp_cap_allocate(ULONG byte_count, void *opaque)
{
    BYTE *raw;
    QXP_CAP_ALLOC_HEADER *header;
    ULONG index;

    (void)opaque;
    if (qxp_lock_depth != 0) qxp_cap_lock_violation = 1;
    if (qxp_cap_alloc_fail) return NULL;
    raw = (BYTE *)malloc((size_t)(QXP_CAP_OFFSET + byte_count + QXP_CAP_CANARY));
    if (!raw) return NULL;
    header = (QXP_CAP_ALLOC_HEADER *)raw;
    header->size = byte_count;
    header->magic = QXP_CAP_ALLOC_MAGIC;
    for (index = 0; index < QXP_CAP_CANARY; ++index)
        raw[8 + index] = (BYTE)QXP_CAP_HEAD_BYTE;
    for (index = 0; index < byte_count; ++index)
        raw[QXP_CAP_OFFSET + index] = (BYTE)QXP_CAP_FILL_BYTE;
    for (index = 0; index < QXP_CAP_CANARY; ++index)
        raw[QXP_CAP_OFFSET + byte_count + index] = (BYTE)QXP_CAP_TAIL_BYTE;
    ++qxp_cap_alloc;
    ++qxp_cap_live;
    return raw + QXP_CAP_OFFSET;
}

static void qxp_cap_free(void *allocation, void *opaque)
{
    BYTE *raw;
    QXP_CAP_ALLOC_HEADER *header;
    ULONG size;
    ULONG index;

    (void)opaque;
    if (qxp_lock_depth != 0) qxp_cap_lock_violation = 1;
    ++qxp_cap_release;
    if (!allocation) return;
    raw = (BYTE *)allocation - QXP_CAP_OFFSET;
    header = (QXP_CAP_ALLOC_HEADER *)raw;
    if (header->magic != QXP_CAP_ALLOC_MAGIC) {
        ++qxp_cap_canary_fail;
        return;
    }
    size = header->size;
    for (index = 0; index < QXP_CAP_CANARY; ++index)
        if (raw[8 + index] != (BYTE)QXP_CAP_HEAD_BYTE) ++qxp_cap_canary_fail;
    for (index = 0; index < QXP_CAP_CANARY; ++index)
        if (raw[QXP_CAP_OFFSET + size + index] != (BYTE)QXP_CAP_TAIL_BYTE)
            ++qxp_cap_canary_fail;
    if (qxp_cap_live) --qxp_cap_live;
    free(raw);
}

static BOOL qxp_lookup_unused(
    const QXP_D3D9_DRAW_BACKING_OWNER *owner, DWORD handle, DWORD role,
    QXP_D3D9_DRAW_BACKING_METADATA *metadata, void *opaque)
{
    /* A stream-0 user-memory nonindexed draw never consults the lookup; the
     * backing snapshot only requires the pointer to be non-NULL. */
    (void)owner;
    (void)handle;
    (void)role;
    (void)metadata;
    (void)opaque;
    return FALSE;
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

/* -- capture callbacks ---------------------------------------------------- */

static void qxp_recorder_reset(void)
{
    memset(&qxp_recorder, 0, sizeof(qxp_recorder));
}

static void *qxp_secure(const void *address, ULONG byte_count, void *opaque)
{
    QXP_CB_RECORDER *recorder;
    DWORD ordinal;
    void *handle;

    recorder = (QXP_CB_RECORDER *)opaque;
    if (qxp_lock_depth != 0) recorder->lock_violation = 1;
    ordinal = recorder->secure_count;
    if (ordinal < QXP_CB_MAX) {
        recorder->secure_addr[ordinal] = address;
        recorder->secure_bytes[ordinal] = byte_count;
    }
    ++recorder->secure_count;
    if (recorder->fail_secure == recorder->secure_count) {
        if (ordinal < QXP_CB_MAX) recorder->secure_handle[ordinal] = NULL;
        return NULL;
    }
    handle = &recorder->tokens[recorder->token_next];
    ++recorder->token_next;
    if (ordinal < QXP_CB_MAX) recorder->secure_handle[ordinal] = handle;
    return handle;
}

static void qxp_unsecure(void *handle, void *opaque)
{
    QXP_CB_RECORDER *recorder;

    recorder = (QXP_CB_RECORDER *)opaque;
    if (qxp_lock_depth != 0) recorder->lock_violation = 1;
    if (recorder->unsecure_count < QXP_CB_MAX)
        recorder->unsecure_handle[recorder->unsecure_count] = handle;
    ++recorder->unsecure_count;
}

static BOOL qxp_copy(
    void *destination, const void *source, ULONG byte_count, void *opaque)
{
    QXP_CB_RECORDER *recorder;
    DWORD ordinal;

    recorder = (QXP_CB_RECORDER *)opaque;
    if (qxp_lock_depth != 0) recorder->lock_violation = 1;
    ordinal = recorder->copy_count;
    if (ordinal < QXP_CB_MAX) {
        recorder->copy_dest[ordinal] = destination;
        recorder->copy_src[ordinal] = source;
        recorder->copy_bytes[ordinal] = byte_count;
    }
    ++recorder->copy_count;
    if (recorder->fail_copy == recorder->copy_count) return FALSE;
    memcpy(destination, source, (size_t)byte_count);
    if (recorder->corrupt_records && ordinal == 0 && byte_count)
        ((BYTE *)destination)[0] ^= 0xffU;
    return TRUE;
}

/* -- small byte helpers --------------------------------------------------- */

static int qxp_bytes_equal(const void *left, const void *right, ULONG count)
{
    const BYTE *left_bytes;
    const BYTE *right_bytes;
    ULONG index;

    left_bytes = (const BYTE *)left;
    right_bytes = (const BYTE *)right;
    for (index = 0; index < count; ++index)
        if (left_bytes[index] != right_bytes[index]) return 0;
    return 1;
}

static int qxp_ring_bytes_equal(
    const void *linear, const BYTE *ring, DWORD ring_bytes,
    DWORD ring_start, DWORD count)
{
    const BYTE *linear_bytes;
    DWORD index;

    if (!linear || !ring || !ring_bytes || ring_start >= ring_bytes)
        return 0;
    linear_bytes = (const BYTE *)linear;
    for (index = 0; index < count; ++index) {
        if (linear_bytes[index] != ring[(ring_start + index) % ring_bytes])
            return 0;
    }
    return 1;
}

static int qxp_bytes_zero(const void *value, ULONG count)
{
    const BYTE *bytes;
    ULONG index;

    bytes = (const BYTE *)value;
    for (index = 0; index < count; ++index)
        if (bytes[index]) return 0;
    return 1;
}

/* Test-only resealing lets kind mutations reach semantic revalidation instead
 * of being stopped by the preceding integrity gate. */
static DWORD qxp_test_blob_hash(const BYTE *blob, DWORD length)
{
    DWORD hash_offset;
    DWORD hash;
    DWORD index;

    hash_offset = (DWORD)offsetof(QXP_D3D9_DRAW_CAPTURE_HEADER, hash);
    hash = 2166136261UL;
    for (index = 0; index < length; ++index) {
        BYTE value;

        value = blob[index];
        if (index >= hash_offset &&
            index - hash_offset < (DWORD)sizeof(DWORD)) value = 0;
        hash ^= (DWORD)value;
        hash *= 16777619UL;
    }
    return hash;
}

static int qxp_region_contains_pointer(
    const BYTE *base, DWORD length, const void *pointer)
{
    const BYTE *pointer_bytes;
    DWORD pointer_size;
    DWORD index;

    pointer_bytes = (const BYTE *)&pointer;
    pointer_size = (DWORD)sizeof(pointer);
    if (length < pointer_size) return 0;
    for (index = 0; index + pointer_size <= length; ++index) {
        DWORD component;
        int match;

        match = 1;
        for (component = 0; component < pointer_size; ++component) {
            if (base[index + component] != pointer_bytes[component]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

static int qxp_um_guard_intact(void)
{
    DWORD index;

    for (index = 0; index < QXP_REAL_PREFIX; ++index)
        if (qxp_um_buffer[index] != (BYTE)QXP_REAL_GUARD_BYTE) return 0;
    for (index = 0; index < QXP_REAL_GUARD; ++index) {
        if (qxp_um_buffer[QXP_REAL_PREFIX + QXP_REAL_UM_BYTES + index] !=
                (BYTE)QXP_REAL_GUARD_BYTE) return 0;
    }
    return 1;
}

/* -- real SM2 fixture ----------------------------------------------------- */

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
        &qxp_plain_allocator, &declaration, &status);
    if (!result) return 0;
    qxp_lock(NULL);
    result = QxpD3d9PublishVertexDeclaration(
        &qxp_fixture.table, declaration, &status);
    qxp_unlock(NULL);
    if (!result) return 0;
    qxp_fixture.declaration = declaration;

    result = QxpD3d9PrepareVertexFunction(
        &qxp_fixture.owner, 0x201UL, qxp_vertex_shader,
        (DWORD)sizeof(qxp_vertex_shader), &qxp_plain_allocator, &vertex,
        &status);
    if (!result) return 0;
    qxp_lock(NULL);
    result = QxpD3d9PublishVertexFunction(
        &qxp_fixture.table, vertex, &status);
    qxp_unlock(NULL);
    if (!result) return 0;
    qxp_fixture.vertex = vertex;

    result = QxpD3d9PreparePixelFunction(
        &qxp_fixture.owner, 0x301UL, qxp_pixel_shader,
        (DWORD)sizeof(qxp_pixel_shader), &qxp_plain_allocator, &pixel, &status);
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
    if (!QxpD3d9DrawStateSetStreamSource(
            &qxp_fixture.draw, 0, 0, 0, QXP_REAL_STRIDE, TRUE, &draw_status))
        return 0;
    return 1;
}

static int qxp_fixture_initialize(void)
{
    QXP_D3D9_DRAW_PREPARE_STATUS status;

    memset(&qxp_fixture, 0, sizeof(qxp_fixture));
    qxp_shader_owner_initialize(
        &qxp_fixture.owner, 0x41UL, 0x51UL, &qxp_fixture.device_identity);
    QxpD3d9ShaderObjectTableInitialize(&qxp_fixture.table);
    QxpD3d9DrawStateInitialize(&qxp_fixture.draw);
    QxpD3d9ClearPixelBindingState(&qxp_fixture.pixel);
    qxp_fixture.backing_owner.shader_owner.context_handle =
        qxp_fixture.owner.context_handle;
    qxp_fixture.backing_owner.shader_owner.process_id =
        qxp_fixture.owner.process_id;
    qxp_fixture.backing_owner.shader_owner.device_identity =
        (ULONG_PTR)qxp_fixture.owner.device;
    qxp_fixture.backing_owner.dd_local_identity = 0xabcUL;
    if (!qxp_publish_objects() || !qxp_bind_objects()) return 0;
    qxp_fixture.prepared = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_plain_allocator, qxp_lock, qxp_unlock, NULL,
        NULL,
        &status);
    return qxp_fixture.prepared != NULL;
}

static void qxp_fixture_destroy(void)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel;

    if (qxp_fixture.prepared) {
        QxpD3d9DestroyPreparedDraw(qxp_fixture.prepared, &qxp_plain_allocator);
        qxp_fixture.prepared = NULL;
    }
    for (;;) {
        qxp_lock(NULL);
        declaration = QxpD3d9DetachOwnerVertexDeclaration(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, &qxp_plain_allocator);
    }
    for (;;) {
        qxp_lock(NULL);
        vertex = QxpD3d9DetachOwnerVertexFunction(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!vertex) break;
        QxpD3d9DestroyVertexFunction(vertex, &qxp_plain_allocator);
    }
    for (;;) {
        qxp_lock(NULL);
        pixel = QxpD3d9DetachOwnerPixelFunction(
            &qxp_fixture.table, &qxp_fixture.owner);
        qxp_unlock(NULL);
        if (!pixel) break;
        QxpD3d9DestroyPixelFunction(pixel, &qxp_plain_allocator);
    }
}

static BOOL qxp_real_make_envelope(
    BYTE opcode, DWORD record_count, const void *records, ULONG available_bytes)
{
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE um;

    memset(&um, 0, sizeof(um));
    um.active = TRUE;
    um.nonnull_pointer_observed = TRUE;
    um.address_range_valid = TRUE;
    um.vertex_size = QXP_REAL_VSIZE;
    um.vertex_length = QXP_REAL_VLEN;
    um.vertex_offset = QXP_REAL_UM_OFFSET;
    if (!QxpD3d9SnapshotDrawBackings(
            qxp_fixture.prepared, &qxp_fixture.draw, &qxp_fixture.backing_owner,
            FALSE, &um, qxp_lookup_unused, NULL, qxp_lock, qxp_unlock, NULL,
            &qxp_backings, &qxp_backing_status)) {
        return FALSE;
    }
    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes, qxp_fixture.prepared,
        &qxp_backings, &qxp_envelope, &qxp_envelope_status);
}

/* -- synthetic canonical B6G2 foundation ---------------------------------- */

static BOOL qxp_syn_build(
    BYTE opcode, DWORD record_count, const void *records, ULONG available_bytes,
    DWORD stride, DWORD span, DWORD byte_length, BOOL user_memory, BOOL indexed,
    DWORD index_handle, DWORD index_stride, DWORD index_byte_length)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;
    QXP_D3D9_DRAW_BACKING_RECORD *stream0;
    QXP_D3D9_DRAW_BACKING_RECORD *index;

    memset(&qxp_syn_prepared, 0, sizeof(qxp_syn_prepared));
    memset(&qxp_syn_backings, 0, sizeof(qxp_syn_backings));
    memset(&qxp_syn_envelope, 0, sizeof(qxp_syn_envelope));

    qxp_syn_prepared.valid = TRUE;
    qxp_syn_prepared.mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    qxp_syn_prepared.owner.context_handle = 0x41UL;
    qxp_syn_prepared.owner.process_id = 0x51UL;
    qxp_syn_prepared.owner.device_identity = (ULONG_PTR)0x61UL;
    qxp_syn_prepared.raw_state.stream_source_generation = 7;
    qxp_syn_prepared.raw_state.streams[0].handle = user_memory ? 0UL : 0x900UL;
    qxp_syn_prepared.raw_state.streams[0].offset = 0;
    qxp_syn_prepared.raw_state.streams[0].stride = stride;
    qxp_syn_prepared.raw_state.streams[0].divider = 1;
    qxp_syn_prepared.raw_state.streams[0].user_memory =
        user_memory ? TRUE : FALSE;
    qxp_syn_prepared.raw_state.streams[0].source2 = FALSE;
    qxp_syn_prepared.required_stream_mask = 1;
    qxp_syn_prepared.required_user_memory_stream_mask = user_memory ? 1UL : 0UL;
    qxp_syn_prepared.required_stream_spans[0] = span;
    if (indexed) {
        qxp_syn_prepared.raw_state.indices.handle = index_handle;
        qxp_syn_prepared.raw_state.indices.stride = index_stride;
    }

    qxp_syn_backings.valid = TRUE;
    qxp_syn_backings.owner.shader_owner = qxp_syn_prepared.owner;
    qxp_syn_backings.owner.dd_local_identity = (ULONG_PTR)0xabcUL;
    qxp_syn_backings.stream_source_generation = 7;
    qxp_syn_backings.required_stream_mask = 1;
    qxp_syn_backings.required_user_memory_stream_mask =
        user_memory ? 1UL : 0UL;
    qxp_syn_backings.populated_stream_mask = 1;
    qxp_syn_backings.indexed = indexed;
    stream0 = &qxp_syn_backings.streams[0];
    stream0->role = QXP_D3D9_DRAW_BACKING_ROLE_VERTEX;
    stream0->stream = 0;
    stream0->stride = stride;
    stream0->source2_offset = 0;
    stream0->divider = 1;
    stream0->required_span = span;
    stream0->byte_length = byte_length;
    if (user_memory) {
        stream0->kind = QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    } else {
        stream0->kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
        stream0->handle = 0x900UL;
        stream0->incarnation = 1;
        stream0->content_generation = 1;
    }
    if (indexed) {
        index = &qxp_syn_backings.index;
        index->kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
        index->role = QXP_D3D9_DRAW_BACKING_ROLE_INDEX;
        index->stream = 0;
        index->handle = index_handle;
        index->incarnation = 1;
        index->content_generation = 1;
        index->byte_length = index_byte_length;
        index->stride = index_stride;
        index->source2_offset = 0;
        index->divider = 0;
        index->required_span = index_stride;
    }

    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes, &qxp_syn_prepared,
        &qxp_syn_backings, &qxp_syn_envelope, &status);
}

static BOOL qxp_syn_rebuild_stream_offset(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, DWORD offset)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;

    qxp_syn_prepared.raw_state.streams[0].offset = offset;
    qxp_syn_prepared.raw_state.streams[0].source2 = TRUE;
    qxp_syn_backings.streams[0].source2_offset = offset;
    memset(&qxp_syn_envelope, 0, sizeof(qxp_syn_envelope));
    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes, &qxp_syn_prepared,
        &qxp_syn_backings, &qxp_syn_envelope, &status);
}

static BOOL qxp_syn_capture(
    BYTE opcode, DWORD record_count, const void *records, ULONG available_bytes,
    const void *base, DWORD user_memory_bytes, void **blob, DWORD *blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    qxp_recorder_reset();
    return QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, opcode, record_count, records, available_bytes,
        &qxp_syn_prepared, &qxp_syn_backings, base, user_memory_bytes,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy, &qxp_recorder,
        blob, blob_bytes, status);
}

static BOOL qxp_syn_indexed_capture(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const void *vertex_base, DWORD vertex_bytes,
    const void *index_base, DWORD index_bytes, void **blob,
    DWORD *blob_bytes, QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW
        sources[QXP_D3D9_DRAW_BACKING_STREAM_COUNT];
    QXP_D3D9_DRAW_CAPTURE_SOURCE_VIEW index_source;

    memset(sources, 0, sizeof(sources));
    memset(&index_source, 0, sizeof(index_source));
    sources[0].base = vertex_base;
    sources[0].byte_count = vertex_bytes;
    sources[0].kind = qxp_syn_backings.streams[0].kind;
    index_source.base = index_base;
    index_source.byte_count = index_bytes;
    index_source.kind = qxp_syn_backings.index.kind;
    qxp_recorder_reset();
    return QxpD3d9CaptureDrawStreams(
        &qxp_syn_envelope, opcode, record_count, records, available_bytes,
        &qxp_syn_prepared, &qxp_syn_backings, sources, &index_source,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, blob, blob_bytes, status);
}

static BOOL qxp_syn_surface_capture(
    BYTE opcode, DWORD record_count, const void *records, ULONG available_bytes,
    const void *base, DWORD surface_bytes, void **blob, DWORD *blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    qxp_recorder_reset();
    return QxpD3d9CaptureNonindexedSurfaceDraw(
        &qxp_syn_envelope, opcode, record_count, records, available_bytes,
        &qxp_syn_prepared, &qxp_syn_backings, base, surface_bytes,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy, &qxp_recorder,
        blob, blob_bytes, status);
}

static BOOL qxp_multi_build(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, DWORD stream_mask, BOOL stream0_user_memory,
    DWORD byte_length)
{
    QXP_D3D9_DRAW_ENVELOPE_STATUS status;
    DWORD stream;

    memset(&qxp_multi_prepared, 0, sizeof(qxp_multi_prepared));
    memset(&qxp_multi_backings, 0, sizeof(qxp_multi_backings));
    memset(&qxp_multi_envelope, 0, sizeof(qxp_multi_envelope));
    memset(qxp_multi_sources, 0, sizeof(qxp_multi_sources));

    qxp_multi_prepared.valid = TRUE;
    qxp_multi_prepared.mode = QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE;
    qxp_multi_prepared.owner.context_handle = 0x141UL;
    qxp_multi_prepared.owner.process_id = 0x151UL;
    qxp_multi_prepared.owner.device_identity = (ULONG_PTR)0x161UL;
    qxp_multi_prepared.raw_state.stream_source_generation = 17;
    qxp_multi_prepared.required_stream_mask = stream_mask;
    qxp_multi_prepared.required_user_memory_stream_mask =
        stream0_user_memory ? 1UL : 0UL;
    if (!(stream_mask & 1UL)) {
        /* DP2 byte-offset records retain stream zero's canonical divisor
         * even when the linked shader consumes only another stream. */
        qxp_multi_prepared.raw_state.streams[0].stride = 13UL;
        qxp_multi_prepared.raw_state.streams[0].divider = 1UL;
    }

    qxp_multi_backings.valid = TRUE;
    qxp_multi_backings.owner.shader_owner = qxp_multi_prepared.owner;
    qxp_multi_backings.owner.dd_local_identity = (ULONG_PTR)0x1abcUL;
    qxp_multi_backings.stream_source_generation = 17;
    qxp_multi_backings.required_stream_mask = stream_mask;
    qxp_multi_backings.required_user_memory_stream_mask =
        stream0_user_memory ? 1UL : 0UL;
    qxp_multi_backings.populated_stream_mask = stream_mask;
    qxp_multi_backings.indexed = FALSE;

    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        QXP_D3D9_DRAW_STREAM_STATE *state;
        QXP_D3D9_DRAW_BACKING_RECORD *backing;
        DWORD stride;
        DWORD span;
        DWORD offset;
        BOOL user_memory;

        if (!(stream_mask & (1UL << stream))) continue;
        state = &qxp_multi_prepared.raw_state.streams[stream];
        backing = &qxp_multi_backings.streams[stream];
        stride = 13UL + stream * 3UL;
        span = 5UL + stream % 5UL;
        user_memory = stream == 0 && stream0_user_memory;
        offset = user_memory ? 0UL : 3UL + stream * 2UL;

        state->handle = user_memory ? 0UL : 0x1900UL + stream;
        state->offset = offset;
        state->stride = stride;
        state->divider = 1UL;
        state->user_memory = user_memory ? TRUE : FALSE;
        state->source2 = offset ? TRUE : FALSE;
        qxp_multi_prepared.required_stream_spans[stream] = span;

        backing->kind = user_memory ?
            QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL :
            QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
        backing->role = QXP_D3D9_DRAW_BACKING_ROLE_VERTEX;
        backing->stream = stream;
        backing->handle = state->handle;
        backing->incarnation = user_memory ? 0 : (ULONGLONG)(stream + 1UL);
        backing->content_generation = user_memory ? 0 : stream + 21UL;
        backing->byte_length = byte_length;
        backing->stride = stride;
        backing->source2_offset = offset;
        backing->divider = 1UL;
        backing->required_span = span;

        qxp_multi_sources[stream].base = qxp_multi_buffer[stream];
        qxp_multi_sources[stream].byte_count = byte_length;
        qxp_multi_sources[stream].kind = backing->kind;
    }
    return QxpD3d9BuildDrawEnvelope(
        opcode, record_count, records, available_bytes, &qxp_multi_prepared,
        &qxp_multi_backings, &qxp_multi_envelope, &status);
}

static BOOL qxp_multi_capture(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, void **blob, DWORD *blob_bytes,
    QXP_D3D9_DRAW_CAPTURE_STATUS *status)
{
    qxp_recorder_reset();
    return QxpD3d9CaptureNonindexedDrawStreams(
        &qxp_multi_envelope, opcode, record_count, records, available_bytes,
        &qxp_multi_prepared, &qxp_multi_backings, qxp_multi_sources,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy, &qxp_recorder,
        blob, blob_bytes, status);
}

/* -- shared verification -------------------------------------------------- */

static void qxp_check_plan(
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header, const char *tag)
{
    const QXP_D3D9_DRAW_ACCESS_PLAN *plan;
    DWORD stream;
    int streams_exact;

    plan = &header->access_plan;
    streams_exact = 1;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (header->stream_mask & (1UL << stream)) {
            if (plan->streams[stream].valid != TRUE ||
                plan->streams[stream].stream != stream ||
                !qxp_bytes_equal(
                    &plan->streams[stream], &header->envelope.streams[stream],
                    (ULONG)sizeof(plan->streams[stream]))) streams_exact = 0;
        } else if (!qxp_bytes_zero(
                       &plan->streams[stream],
                       (ULONG)sizeof(plan->streams[stream]))) {
            streams_exact = 0;
        }
    }
    qxp_check(plan->valid == TRUE && plan->indexed == FALSE &&
                  plan->stream_mask == header->stream_mask &&
                  plan->scanned_index_count.low == 0 &&
                  plan->scanned_index_count.high == 0 && streams_exact &&
                  qxp_bytes_zero(&plan->index, (ULONG)sizeof(plan->index)),
              tag);
}

/* Capture through the real fixture and verify the whole immutable blob:
 * validation, the exact access map, blob-owned record and vertex bytes, the
 * user-memory guard canaries, callback ordering/addresses/bytes and distinct
 * reverse-unsecured handles.  The caller owns and destroys *out_blob. */
static void qxp_capture_and_verify(
    BYTE opcode, DWORD record_count, const void *records, ULONG available_bytes,
    void **out_blob, DWORD *out_bytes, const char *tag)
{
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *blob_records;
    const void *blob_vertex;
    void *blob;
    DWORD blob_bytes;
    DWORD byte_first;
    DWORD byte_last;
    DWORD vertex_bytes;
    DWORD records_count_bytes;
    DWORD exact_record_bytes;

    *out_blob = NULL;
    *out_bytes = 0;
    if (!qxp_real_make_envelope(opcode, record_count, records,
                                available_bytes)) {
        qxp_check(0, tag);
        return;
    }
    byte_first = (DWORD)qxp_envelope.streams[0].byte_first;
    byte_last = (DWORD)qxp_envelope.streams[0].byte_last_exclusive;
    vertex_bytes = byte_last - byte_first;
    records_count_bytes = 0;
    exact_record_bytes =
        record_count * QxpD3d9DrawCommandRecordSize(opcode);

    blob = (void *)0;
    blob_bytes = 0xdeadu;
    qxp_recorder_reset();
    memset(&status, 0xcc, sizeof(status));
    qxp_check(QxpD3d9CaptureNonindexedDraw(
                  &qxp_envelope, opcode, record_count, records, available_bytes,
                  qxp_fixture.prepared, &qxp_backings, qxp_um_base,
                  QXP_REAL_UM_BYTES, &qxp_cap_allocator, qxp_secure,
                  qxp_unsecure, qxp_copy, &qxp_recorder, &blob, &blob_bytes,
                  &status) &&
                  blob != NULL && status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              tag);
    if (!blob) return;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;

    qxp_check(QxpD3d9ValidateCapturedDraw(blob, blob_bytes, &status) &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              "captured blob revalidates from its own bytes");
    qxp_check_plan(header, "captured access plan is the exact stream-0 map");

    blob_records = QxpD3d9CapturedDrawRecords(blob, &records_count_bytes);
    qxp_check(records_count_bytes == exact_record_bytes &&
                  qxp_bytes_equal(blob_records, records, exact_record_bytes),
              "blob owns an exact copy of the records");
    blob_vertex = QxpD3d9CapturedDrawVertexPayload(blob, &vertex_bytes);
    qxp_check(vertex_bytes == byte_last - byte_first &&
                  qxp_bytes_equal(blob_vertex, qxp_um_base + byte_first,
                                  vertex_bytes),
              "blob owns the exact user-memory source bytes");
    qxp_check(qxp_um_guard_intact(), "user-memory guard canaries intact");

    qxp_check(qxp_recorder.secure_count == 2 &&
                  qxp_recorder.secure_addr[0] == records &&
                  qxp_recorder.secure_bytes[0] == exact_record_bytes &&
                  qxp_recorder.secure_addr[1] == qxp_um_base + byte_first &&
                  qxp_recorder.secure_bytes[1] == byte_last - byte_first &&
                  qxp_recorder.secure_handle[0] != NULL &&
                  qxp_recorder.secure_handle[1] != NULL &&
                  qxp_recorder.secure_handle[0] != qxp_recorder.secure_handle[1],
              "secure records exact ranges with distinct handles");
    qxp_check(qxp_recorder.copy_count == 2 &&
                  qxp_recorder.copy_dest[0] ==
                      (BYTE *)blob + header->records_offset &&
                  qxp_recorder.copy_src[0] == records &&
                  qxp_recorder.copy_bytes[0] == exact_record_bytes &&
                  qxp_recorder.copy_dest[1] ==
                      (BYTE *)blob + header->streams[0].payload_offset &&
                  qxp_recorder.copy_src[1] == qxp_um_base + byte_first &&
                  qxp_recorder.copy_bytes[1] == byte_last - byte_first,
              "copy targets the exact blob offsets and source ranges");
    qxp_check(qxp_recorder.unsecure_count == 2 &&
                  qxp_recorder.unsecure_handle[0] ==
                      qxp_recorder.secure_handle[1] &&
                  qxp_recorder.unsecure_handle[1] ==
                      qxp_recorder.secure_handle[0],
              "unsecure runs in exact reverse order");
    qxp_check(qxp_recorder.lock_violation == 0 && qxp_lock_depth == 0,
              "capture holds no lock across any callback");

    *out_blob = blob;
    *out_bytes = blob_bytes;
}

/* -- tests ---------------------------------------------------------------- */

static void qxp_test_four_opcodes(void)
{
    QXP_TEST_POINTS points[3];
    QXP_TEST_DRAW draw;
    QXP_TEST_FAN fan;
    QXP_TEST_DRAW2 draw2;
    BYTE framed_draw[sizeof(QXP_TEST_DRAW) + 7UL];
    void *blob;
    DWORD bytes;

    qxp_check(qxp_fixture.prepared != NULL &&
                  qxp_fixture.prepared->valid &&
                  qxp_fixture.prepared->required_stream_mask == 1UL &&
                  qxp_fixture.prepared->required_user_memory_stream_mask ==
                      1UL &&
                  qxp_fixture.prepared->required_stream_spans[0] <=
                      QXP_REAL_STRIDE,
              "real B6E fixture requires only stream 0 user memory");

    /* Multiple POINTS records; effective base points inside the guarded array
     * at a nonzero B6G2 stream byte_first. */
    points[0].count = 2; points[0].start = 1;
    points[1].count = 1; points[1].start = 5;
    points[2].count = 3; points[2].start = 8;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_POINTS, 3, points, (ULONG)sizeof(points), &blob, &bytes,
        "POINTS multi-record capture");
    if (blob) {
        qxp_check(qxp_envelope.streams[0].byte_first == (ULONGLONG)16 &&
                      ((const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob)
                              ->streams[0].byte_first != 0,
                  "capture records a nonzero stream-0 byte_first");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw.vertex_start = 1;
    draw.primitive_count = 2;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw), &blob,
        &bytes, "DRAWPRIMITIVE capture");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    memset(framed_draw, 0x6d, sizeof(framed_draw));
    draw.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    draw.vertex_start = 4UL;
    draw.primitive_count = 2UL;
    memcpy(framed_draw, &draw, sizeof(draw));
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, framed_draw,
        (ULONG)sizeof(framed_draw), &blob, &bytes,
        "capture ignores trailing command-stream bytes");
    if (blob) {
        qxp_check(((QXP_D3D9_DRAW_CAPTURE_HEADER *)blob)->records_bytes ==
                      QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE,
                  "capture owns only the exact framed record bytes");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    fan.first_vertex_offset = QXP_REAL_STRIDE * 2UL;
    fan.edge_flags = 0x55UL;
    fan.primitive_count = 2;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1, &fan, (ULONG)sizeof(fan), &blob,
        &bytes, "CLIPPEDTRIANGLEFAN capture");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    draw2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLESTRIP;
    draw2.first_vertex_offset = QXP_REAL_STRIDE * 3UL;
    draw2.primitive_count = 2;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw2, (ULONG)sizeof(draw2), &blob,
        &bytes, "DRAWPRIMITIVE2 capture");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_independence_and_no_pointers(void)
{
    QXP_TEST_DRAW draw;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *blob_records;
    const void *blob_vertex;
    void *blob;
    DWORD bytes;
    DWORD records_bytes;
    DWORD vertex_bytes;
    DWORD scan_length;
    QXP_TEST_DRAW saved_records;
    BYTE saved_vertex[QXP_REAL_UM_BYTES];
    DWORD byte_first;

    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.vertex_start = 2;
    draw.primitive_count = 2;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw), &blob,
        &bytes, "independence base capture");
    if (!blob) return;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    byte_first = header->streams[0].byte_first;

    /* No transient pointer must survive in the fixed header or the records. */
    scan_length = QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET + header->records_bytes;
    qxp_check(!qxp_region_contains_pointer(
                  (const BYTE *)blob, scan_length, &draw) &&
                  !qxp_region_contains_pointer(
                      (const BYTE *)blob, scan_length, qxp_um_base) &&
                  !qxp_region_contains_pointer(
                      (const BYTE *)blob, scan_length, &qxp_cap_allocator),
              "no transient pointer is retained in header or records");

    saved_records = draw;
    memcpy(saved_vertex, qxp_um_base, QXP_REAL_UM_BYTES);

    /* Scribble the transient originals; the blob must stay valid and unchanged. */
    memset(&draw, 0x77, sizeof(draw));
    memset(qxp_um_base, 0x77, QXP_REAL_UM_BYTES);
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "blob still validates after originals are overwritten");
    blob_records = QxpD3d9CapturedDrawRecords(blob, &records_bytes);
    blob_vertex = QxpD3d9CapturedDrawVertexPayload(blob, &vertex_bytes);
    qxp_check(records_bytes == (DWORD)sizeof(saved_records) &&
                  qxp_bytes_equal(blob_records, &saved_records,
                                  (ULONG)sizeof(saved_records)),
              "blob-owned records are independent of the transient records");
    qxp_check(qxp_bytes_equal(blob_vertex, saved_vertex + byte_first,
                              vertex_bytes),
              "blob-owned vertex bytes are independent of user memory");

    memcpy(qxp_um_base, saved_vertex, QXP_REAL_UM_BYTES);
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_expect_reject_blob(
    void *blob, DWORD bytes, const char *name)
{
    QXP_D3D9_DRAW_CAPTURE_STATUS status;

    memset(&status, 0, sizeof(status));
    qxp_check(!QxpD3d9ValidateCapturedDraw(blob, bytes, &status), name);
}

static void qxp_test_blob_mutations(void)
{
    QXP_TEST_DRAW draw;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    BYTE *bytes_ptr;
    void *blob;
    DWORD bytes;
    DWORD record_offset;
    DWORD plan_offset;
    DWORD envelope_offset;
    DWORD padding_offset;
    DWORD vertex_offset;
    BYTE saved;

    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw.vertex_start = 3;
    draw.primitive_count = 1;
    qxp_capture_and_verify(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw), &blob,
        &bytes, "mutation base capture");
    if (!blob) return;
    header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    bytes_ptr = (BYTE *)blob;
    record_offset = header->records_offset;
    plan_offset = (DWORD)offsetof(QXP_D3D9_DRAW_CAPTURE_HEADER, access_plan);
    envelope_offset = (DWORD)offsetof(QXP_D3D9_DRAW_CAPTURE_HEADER, envelope);
    padding_offset = QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE;
    vertex_offset = header->streams[0].payload_offset;

    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "mutation base blob validates before tampering");
    qxp_check(!QxpD3d9ValidateCapturedDraw(blob, bytes - 1UL, &status) &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
              "validator rejects a truncated caller-held blob extent");
    qxp_check(!QxpD3d9ValidateCapturedDraw(
                  blob, QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES + 1UL,
                  &status) &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_LAYOUT,
              "validator rejects a caller extent above the cap");

    saved = bytes_ptr[record_offset];
    bytes_ptr[record_offset] ^= 0xffU;
    qxp_expect_reject_blob(blob, bytes,
                           "one record byte mutation is rejected");
    bytes_ptr[record_offset] = saved;

    saved = bytes_ptr[plan_offset];
    bytes_ptr[plan_offset] ^= 0xffU;
    qxp_expect_reject_blob(blob, bytes,
                           "one access-plan byte mutation is rejected");
    bytes_ptr[plan_offset] = saved;

    saved = bytes_ptr[envelope_offset];
    bytes_ptr[envelope_offset] ^= 0xffU;
    qxp_expect_reject_blob(blob, bytes,
                           "one embedded header byte mutation is rejected");
    bytes_ptr[envelope_offset] = saved;

    if (QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET >
            QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE) {
        saved = bytes_ptr[padding_offset];
        bytes_ptr[padding_offset] ^= 0xffU;
        qxp_expect_reject_blob(blob, bytes,
                               "one rounded-padding byte mutation is rejected");
        bytes_ptr[padding_offset] = saved;
    }

    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "blob validates again after each byte is restored");

    /* The opaque vertex payload is excluded from the integrity hash; mutating
     * it is tolerated because caller immutability is the contract. */
    saved = bytes_ptr[vertex_offset];
    bytes_ptr[vertex_offset] ^= 0xffU;
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "vertex payload mutation is accepted (excluded from hash)");
    bytes_ptr[vertex_offset] = saved;

    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_expect_capture_failure(
    BOOL result, void *blob, DWORD blob_bytes,
    const QXP_D3D9_DRAW_CAPTURE_STATUS *status, DWORD code, BOOL resource,
    const char *name)
{
    qxp_check(!result && blob == NULL && blob_bytes == 0 &&
                  status->code == code &&
                  QxpD3d9CaptureStatusIsResourceFailure(status) ==
                      (resource ? TRUE : FALSE),
              name);
}

static void qxp_test_circular_surface_capture(void)
{
    QXP_TEST_DRAW draw;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    QXP_D3D9_CAPTURED_DRAW2 converted;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *payload;
    void *blob;
    DWORD bytes;
    DWORD payload_bytes;
    DWORD alloc_before;
    DWORD release_before;
    BOOL built;
    BOOL result;

    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.vertex_start = 0x36UL;
    draw.primitive_count = 8UL;
    built = qxp_syn_build(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw),
        0x20UL, 0x20UL, 4096UL, FALSE, FALSE, 0, 0, 0);
    if (built) {
        qxp_syn_backings.streams[0].byte_length = 0x140UL;
        qxp_syn_backings.streams[0].flags =
            QXP_D3D9_DRAW_BACKING_FLAG_CIRCULAR;
        memset(&qxp_syn_envelope, 0, sizeof(qxp_syn_envelope));
        built = QxpD3d9BuildDrawEnvelope(
            QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw),
            &qxp_syn_prepared, &qxp_syn_backings, &qxp_syn_envelope,
            &envelope_status);
    }
    qxp_check(
        built &&
            qxp_syn_envelope.streams[0].logical_first ==
                (ULONGLONG)0x36UL &&
            qxp_syn_envelope.streams[0].logical_last_exclusive ==
                (ULONGLONG)0x4eUL &&
            qxp_syn_envelope.streams[0].byte_first ==
                (ULONGLONG)0x80UL &&
            qxp_syn_envelope.streams[0].byte_last_exclusive ==
                (ULONGLONG)0x380UL,
        "r525 A8 execute ring builds one exact unrolled envelope");
    if (!built) return;

    blob = NULL;
    bytes = 0;
    result = qxp_syn_surface_capture(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw),
        qxp_syn_buffer, 0x140UL, &blob, &bytes, &status);
    qxp_check(
        result && blob && status.code == QXP_D3D9_DRAW_CAPTURE_OK,
        "r525 A8 execute ring captures successfully");
    if (!blob) return;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    payload = QxpD3d9CapturedDrawVertexPayload(blob, &payload_bytes);
    qxp_check(
        QxpD3d9ValidateCapturedDraw(blob, bytes, &status) &&
            payload_bytes == 0x300UL &&
            qxp_ring_bytes_equal(
                payload, qxp_syn_buffer, 0x140UL, 0x80UL, payload_bytes),
        "execute-ring capture owns the exact three wrapped spans");
    qxp_check_plan(header, "execute-ring access plan is canonical");
    qxp_check(
        QxpD3d9PrepareCapturedDrawExecution(
            blob, bytes, &plan, &status) &&
            QxpD3d9CapturedDrawRecordToDraw2(
                &plan, 0, &converted, &status) &&
            plan.logical_first == 0x36UL &&
            plan.logical_last_exclusive == 0x4eUL &&
            plan.streams[0].payload_bytes == 0x300UL &&
            converted.first_vertex_offset == 0x6c0UL &&
            converted.primitive_count == 8UL,
        "linearized execute-ring payload prepares the original draw");
    qxp_check(
        qxp_recorder.secure_count == 2 &&
            qxp_recorder.secure_addr[1] == qxp_syn_buffer &&
            qxp_recorder.secure_bytes[1] == 0x140UL &&
            qxp_recorder.copy_count == 4 &&
            qxp_recorder.copy_src[1] == qxp_syn_buffer + 0x80UL &&
            qxp_recorder.copy_bytes[1] == 0xc0UL &&
            qxp_recorder.copy_src[2] == qxp_syn_buffer &&
            qxp_recorder.copy_bytes[2] == 0x140UL &&
            qxp_recorder.copy_src[3] == qxp_syn_buffer &&
            qxp_recorder.copy_bytes[3] == 0x100UL &&
            qxp_recorder.unsecure_count == 2,
        "execute ring secures once and copies each wrapped segment");
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    alloc_before = qxp_cap_alloc;
    release_before = qxp_cap_release;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_recorder_reset();
    qxp_recorder.fail_copy = 3;
    result = QxpD3d9CaptureNonindexedSurfaceDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
        (ULONG)sizeof(draw), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 0x140UL, &qxp_cap_allocator, qxp_secure,
        qxp_unsecure, qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_COPY,
        FALSE, "execute-ring wrapped-copy failure is atomic");
    qxp_check(
        qxp_recorder.secure_count == 2 &&
            qxp_recorder.copy_count == 3 &&
            qxp_recorder.unsecure_count == 2 &&
            qxp_cap_alloc == alloc_before + 1UL &&
            qxp_cap_release == release_before + 1UL,
        "execute-ring copy failure releases every secured resource");
}

static void qxp_test_surface_capture(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *blob_records;
    const void *blob_vertex;
    void *blob;
    DWORD bytes;
    DWORD records_bytes;
    DWORD vertex_bytes;
    DWORD byte_first;
    DWORD byte_last;
    DWORD saved_kind;

    points.count = 3;
    points.start = 2;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "system-linear capture foundation");
    byte_first = (DWORD)qxp_syn_envelope.streams[0].byte_first;
    byte_last = (DWORD)qxp_syn_envelope.streams[0].byte_last_exclusive;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    memset(&status, 0xcc, sizeof(status));
    qxp_check(qxp_syn_surface_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob, &bytes,
                  &status) && blob != NULL &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              "system-linear surface capture succeeds");
    if (!blob) return;
    header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;

    qxp_check(header->backings.streams[0].kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                  header->backings.streams[0].handle != 0 &&
                  header->backings.streams[0].incarnation != 0 &&
                  header->backings.streams[0].content_generation != 0 &&
                  header->prepared.raw_state.streams[0].handle ==
                      header->backings.streams[0].handle &&
                  header->prepared.raw_state.streams[0].user_memory == FALSE &&
                  header->backings.streams[0].byte_length == 4096,
              "surface blob records the exact nonzero identity contract");
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status) &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              "system-linear blob canonically revalidates from its own bytes");
    qxp_check_plan(header, "system-linear access plan is the exact stream-0 map");

    blob_records = QxpD3d9CapturedDrawRecords(blob, &records_bytes);
    blob_vertex = QxpD3d9CapturedDrawVertexPayload(blob, &vertex_bytes);
    qxp_check(records_bytes == (DWORD)sizeof(points) &&
                  qxp_bytes_equal(blob_records, &points, records_bytes) &&
                  vertex_bytes == byte_last - byte_first &&
                  qxp_bytes_equal(blob_vertex, qxp_syn_buffer + byte_first,
                                  vertex_bytes),
              "surface blob owns the exact records and resolved source bytes");
    qxp_check(qxp_recorder.secure_count == 2 &&
                  qxp_recorder.secure_addr[0] == &points &&
                  qxp_recorder.secure_bytes[0] == (ULONG)sizeof(points) &&
                  qxp_recorder.secure_addr[1] ==
                      qxp_syn_buffer + byte_first &&
                  qxp_recorder.secure_bytes[1] == byte_last - byte_first &&
                  qxp_recorder.copy_count == 2 &&
                  qxp_recorder.unsecure_count == 2,
              "surface capture secures and copies only the exact extents");

    /* Reseal after switching only the recorded kind so semantic dispatch,
     * rather than the hash gate, must reject the identity disagreement. */
    saved_kind = header->backings.streams[0].kind;
    header->backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob,
        QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET + header->records_bytes);
    qxp_check(!QxpD3d9ValidateCapturedDraw(blob, bytes, &status) &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
              "a resealed blob switched to the other kind fails revalidation");
    header->backings.streams[0].kind = saved_kind;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob,
        QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET + header->records_bytes);
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "surface blob revalidates after its recorded kind is restored");

    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_stream_offset_capture(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    const void *blob_vertex;
    void *blob;
    DWORD bytes;
    DWORD vertex_bytes;
    DWORD byte_first;
    DWORD byte_last;

    points.count = 3;
    points.start = 2;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 16, 12, 128, FALSE, FALSE,
                  0, 0, 0) &&
                  qxp_syn_rebuild_stream_offset(
                      QXP_D3D9_DP2OP_POINTS, 1, &points,
                      (ULONG)sizeof(points), 32),
              "stream-offset surface foundation");
    byte_first = (DWORD)qxp_syn_envelope.streams[0].byte_first;
    byte_last = (DWORD)qxp_syn_envelope.streams[0].byte_last_exclusive;
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_syn_surface_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), qxp_syn_buffer, 128, &blob, &bytes,
                  &status) && blob != NULL && byte_first == 64 &&
                  byte_last == 108,
              "nonzero stream offset captures the shifted source range");
    if (blob) {
        blob_vertex = QxpD3d9CapturedDrawVertexPayload(
            blob, &vertex_bytes);
        qxp_check(vertex_bytes == 44 &&
                      qxp_bytes_equal(blob_vertex,
                                      qxp_syn_buffer + byte_first,
                                      vertex_bytes) &&
                      qxp_recorder.secure_addr[1] ==
                          qxp_syn_buffer + byte_first &&
                      qxp_recorder.secure_bytes[1] == vertex_bytes,
                  "offset application secures and copies base plus offset");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 16, 12, 128, FALSE, FALSE,
                  0, 0, 0) &&
                  qxp_syn_rebuild_stream_offset(
                      QXP_D3D9_DP2OP_POINTS, 1, &points,
                      (ULONG)sizeof(points), 0),
              "zero-offset SOURCE2 provenance foundation");
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_syn_surface_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), qxp_syn_buffer, 128, &blob, &bytes,
                  &status) && blob != NULL,
              "zero-offset SOURCE2 provenance is consumed");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 16, 12, 128, FALSE, FALSE,
                  0, 0, 0) &&
                  !qxp_syn_rebuild_stream_offset(
                      QXP_D3D9_DP2OP_POINTS, 1, &points,
                      (ULONG)sizeof(points), 80),
              "offset plus stride and last extent beyond capacity refuses");
}

static void qxp_test_sparse_multistream(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    QXP_D3D9_DRAW_CAPTURE_STREAM_DESCRIPTOR saved_descriptor;
    const void *stream0_payload;
    const void *stream7_payload;
    void *blob;
    DWORD bytes;
    DWORD stream0_bytes;
    DWORD stream7_bytes;
    DWORD missing_bytes;
    DWORD stream;
    DWORD expected_payload_bytes;
    DWORD saved_mask;
    DWORD saved_count;
    DWORD saved_aggregate;
    BYTE saved_payload;
    int nonrequired_zero;

    points.count = 4;
    points.start = 2;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), TRUE,
                  4096),
              "sparse stream-0+7 foundation builds");

    /* Unrelated bound raw stream and index state are legal.  Neither is
     * capture-authoritative unless the command and required mask select it. */
    qxp_multi_prepared.raw_state.streams[3].handle = 0x3333UL;
    qxp_multi_prepared.raw_state.streams[3].stride = 32UL;
    qxp_multi_prepared.raw_state.streams[3].divider = 1UL;
    qxp_multi_prepared.raw_state.indices.handle = 0x4444UL;
    qxp_multi_prepared.raw_state.indices.stride = 2UL;

    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) &&
                  blob != NULL && status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              "sparse capture tolerates unrelated raw stream and index bindings");
    if (!blob) return;
    header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    expected_payload_bytes = header->streams[0].payload_bytes +
                             header->streams[7].payload_bytes;
    qxp_check(header->version == QXP_D3D9_DRAW_CAPTURE_VERSION &&
                  header->stream_mask == ((1UL << 0) | (1UL << 7)) &&
                  header->stream_count == 2UL &&
                  header->stream_payload_bytes == expected_payload_bytes &&
                  header->streams[0].source_kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL &&
                  header->streams[7].source_kind ==
                      QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                  header->streams[0].byte_first !=
                      header->streams[7].byte_first &&
                  header->prepared.raw_state.streams[3].handle == 0x3333UL &&
                  header->prepared.raw_state.streams[3].stride == 32UL &&
                  header->prepared.raw_state.streams[3].divider == 1UL &&
                  header->prepared.raw_state.indices.handle == 0x4444UL &&
                  header->prepared.raw_state.indices.stride == 2UL,
              "v2 header preserves nonrequired raw stream and index state");
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "sparse multistream blob revalidates");
    qxp_check_plan(header, "sparse access plan is mask-exact");

    stream0_payload = QxpD3d9CapturedDrawStreamPayload(
        blob, 0, &stream0_bytes);
    stream7_payload = QxpD3d9CapturedDrawStreamPayload(
        blob, 7, &stream7_bytes);
    missing_bytes = 0xdeadu;
    qxp_check(stream0_payload == QxpD3d9CapturedDrawVertexPayload(
                      blob, &missing_bytes) &&
                  missing_bytes == stream0_bytes &&
                  stream0_bytes == header->streams[0].payload_bytes &&
                  stream7_bytes == header->streams[7].payload_bytes &&
                  qxp_bytes_equal(
                      stream0_payload,
                      (const BYTE *)qxp_multi_sources[0].base +
                          header->streams[0].byte_first,
                      stream0_bytes) &&
                  qxp_bytes_equal(
                      stream7_payload,
                      (const BYTE *)qxp_multi_sources[7].base +
                          header->streams[7].byte_first,
                      stream7_bytes),
              "per-stream getters return exact shifted bytes and stream-0 alias");
    missing_bytes = 0xdeadu;
    qxp_check(QxpD3d9CapturedDrawStreamPayload(blob, 1, &missing_bytes) ==
                      NULL && missing_bytes == 0 &&
                  QxpD3d9CapturedDrawStreamPayload(
                      blob, QXP_D3D9_DRAW_BACKING_STREAM_COUNT,
                      &missing_bytes) == NULL && missing_bytes == 0,
              "getter rejects nonrequired and out-of-range streams");

    qxp_check(qxp_recorder.secure_count == 3 &&
                  qxp_recorder.secure_addr[0] == &points &&
                  qxp_recorder.secure_addr[1] ==
                      (const BYTE *)qxp_multi_sources[0].base +
                          header->streams[0].byte_first &&
                  qxp_recorder.secure_addr[2] ==
                      (const BYTE *)qxp_multi_sources[7].base +
                          header->streams[7].byte_first &&
                  qxp_recorder.copy_count == 3 &&
                  qxp_recorder.copy_src[1] == qxp_recorder.secure_addr[1] &&
                  qxp_recorder.copy_src[2] == qxp_recorder.secure_addr[2] &&
                  qxp_recorder.unsecure_count == 3 &&
                  qxp_recorder.unsecure_handle[0] ==
                      qxp_recorder.secure_handle[2] &&
                  qxp_recorder.unsecure_handle[1] ==
                      qxp_recorder.secure_handle[1] &&
                  qxp_recorder.unsecure_handle[2] ==
                      qxp_recorder.secure_handle[0],
              "sparse callbacks run records then streams ascending and reverse");

    nonrequired_zero = 1;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        if (header->stream_mask & (1UL << stream)) continue;
        if (!qxp_bytes_zero(
                &header->streams[stream],
                (ULONG)sizeof(header->streams[stream])) ||
            !qxp_bytes_zero(
                &header->backings.streams[stream],
                (ULONG)sizeof(header->backings.streams[stream])) ||
            !qxp_bytes_zero(
                &header->envelope.streams[stream],
                (ULONG)sizeof(header->envelope.streams[stream])) ||
            !qxp_bytes_zero(
                &header->access_plan.streams[stream],
                (ULONG)sizeof(header->access_plan.streams[stream]))) {
            nonrequired_zero = 0;
        }
    }
    qxp_check(nonrequired_zero,
              "all nonrequired descriptors and canonical records are zero");
    qxp_check(!qxp_region_contains_pointer(
                  (const BYTE *)blob, QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE,
                  qxp_multi_sources[0].base) &&
                  !qxp_region_contains_pointer(
                      (const BYTE *)blob, QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE,
                      qxp_multi_sources[7].base) &&
                  !qxp_region_contains_pointer(
                      (const BYTE *)blob, QXP_D3D9_DRAW_CAPTURE_HEADER_SIZE,
                      &points),
              "no multistream source or record pointer enters fixed metadata");

    /* Resealed metadata mutations must reach and fail exact layout or
     * semantic validation rather than relying only on the hash gate. */
    saved_descriptor = header->streams[1];
    header->streams[1].valid = TRUE;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed nonrequired descriptor tampering rejects");
    header->streams[1] = saved_descriptor;

    saved_mask = header->stream_mask;
    header->stream_mask |= 1UL << 1;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes, "resealed stream mask tampering rejects");
    header->stream_mask = saved_mask;

    saved_count = header->stream_count;
    ++header->stream_count;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed stream count tampering rejects");
    header->stream_count = saved_count;

    saved_descriptor = header->streams[7];
    header->streams[7].source_kind =
        QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed stream kind tampering rejects");
    header->streams[7] = saved_descriptor;

    ++header->streams[7].payload_offset;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed payload offset tampering rejects");
    header->streams[7] = saved_descriptor;

    ++header->streams[7].logical_first;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed descriptor range tampering rejects");
    header->streams[7] = saved_descriptor;

    saved_aggregate = header->stream_payload_bytes;
    ++header->stream_payload_bytes;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_expect_reject_blob(blob, bytes,
                           "resealed aggregate payload tampering rejects");
    header->stream_payload_bytes = saved_aggregate;
    header->hash = qxp_test_blob_hash(
        (const BYTE *)blob, header->records_offset + header->records_bytes);
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "sparse blob validates after metadata restoration");

    saved_payload = ((BYTE *)blob)[header->streams[7].payload_offset];
    ((BYTE *)blob)[header->streams[7].payload_offset] ^= 0x5aU;
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "opaque stream payload mutation remains hash-excluded");
    ((BYTE *)blob)[header->streams[7].payload_offset] = saved_payload;
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), FALSE,
                  4096),
              "all-system sparse foundation builds");
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) && blob != NULL,
              "system-linear streams 0 and 7 capture together");
    if (blob) {
        header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
        qxp_check(header->streams[0].source_kind ==
                          QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                      header->streams[7].source_kind ==
                          QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR &&
                      QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
                  "all-system sparse capture is canonical");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 1UL << 7, FALSE, 4096),
              "stream-7-only foundation builds");
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) && blob != NULL,
              "a required nonzero stream does not require stream 0");
    if (blob) {
        missing_bytes = 0xdeadu;
        qxp_check(QxpD3d9CapturedDrawVertexPayload(blob, &missing_bytes) ==
                          NULL && missing_bytes == 0 &&
                      QxpD3d9CapturedDrawStreamPayload(
                          blob, 7, &stream7_bytes) != NULL &&
                      QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
                  "stream-0 alias is null for a valid stream-7-only blob");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), TRUE,
                  4096),
              "nonrequired-source rejection foundation");
    qxp_multi_sources[1].base = qxp_multi_buffer[1];
    qxp_multi_sources[1].byte_count = 4096;
    qxp_multi_sources[1].kind = QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(!qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) &&
                  blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING &&
                  status.stream == 1 && qxp_recorder.secure_count == 0,
              "a nonrequired source view must be wholly zero");

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), TRUE,
                  4096),
              "video-source rejection foundation");
    qxp_multi_sources[7].kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(!qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) &&
                  blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING &&
                  status.stream == 7,
              "source kind mismatch is rejected");

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), TRUE,
                  4096),
              "video-linear sparse capture foundation");
    qxp_multi_backings.streams[7].kind =
        QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    qxp_multi_backings.streams[7].video_memory_offset = 0x2000UL;
    qxp_multi_sources[7].kind =
        QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    memset(&qxp_multi_envelope, 0, sizeof(qxp_multi_envelope));
    qxp_check(QxpD3d9BuildDrawEnvelope(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &qxp_multi_prepared,
                  &qxp_multi_backings, &qxp_multi_envelope,
                  &envelope_status),
              "video-linear backing rebuilds the canonical envelope");
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) &&
                  blob != NULL && status.code == QXP_D3D9_DRAW_CAPTURE_OK,
              "mixed user-memory and video-linear streams capture");
    qxp_check(qxp_recorder.secure_count == 2UL &&
                  qxp_recorder.copy_count == 3UL &&
                  qxp_recorder.unsecure_count == 2UL,
              "video-linear stream bypasses user-memory securing");
    if (blob) {
        header = (QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
        qxp_check(header->streams[7].source_kind ==
                          QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR &&
                      header->backings.streams[7].video_memory_offset ==
                          0x2000UL &&
                      QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
                  "video-linear identity and payload revalidate from the blob");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }
}

static void qxp_check_captured_execution_record(
    BYTE opcode, const void *record, ULONG record_bytes,
    DWORD expected_type, DWORD expected_offset, DWORD expected_count,
    DWORD expected_first, DWORD expected_last, const char *tag)
{
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    QXP_D3D9_CAPTURED_DRAW2 draw;
    const void *stream0_payload;
    const void *stream7_payload;
    void *blob;
    DWORD blob_bytes;
    DWORD stream0_bytes;
    DWORD stream7_bytes;
    BOOL result;

    blob = NULL;
    blob_bytes = 0;
    result = qxp_multi_build(
        opcode, 1, record, record_bytes, (1UL << 0) | (1UL << 7),
        FALSE, 4096) &&
        qxp_multi_capture(
            opcode, 1, record, record_bytes, &blob, &blob_bytes, &status);
    qxp_check(result && blob != NULL, tag);
    if (!blob) return;

    memset(&plan, 0xcc, sizeof(plan));
    result = QxpD3d9PrepareCapturedDrawExecution(
        blob, blob_bytes, &plan, &status);
    stream0_payload = QxpD3d9CapturedDrawStreamPayload(
        blob, 0, &stream0_bytes);
    stream7_payload = QxpD3d9CapturedDrawStreamPayload(
        blob, 7, &stream7_bytes);
    qxp_check(result && plan.records != NULL &&
                  plan.records_bytes == record_bytes &&
                  plan.opcode == opcode && plan.record_count == 1UL &&
                  plan.stream_mask == ((1UL << 0) | (1UL << 7)) &&
                  plan.logical_first == expected_first &&
                  plan.logical_last_exclusive == expected_last &&
                  plan.streams[0].payload == stream0_payload &&
                  plan.streams[0].payload_bytes == stream0_bytes &&
                  plan.streams[0].stride == 13UL &&
                  plan.streams[0].required_span == 5UL &&
                  plan.streams[7].payload == stream7_payload &&
                  plan.streams[7].payload_bytes == stream7_bytes &&
                  plan.streams[7].stride == 34UL &&
                  plan.streams[7].required_span == 7UL &&
                  plan.streams[1].payload == NULL,
              "captured execution plan preserves exact streams and range");

    memset(&draw, 0xcc, sizeof(draw));
    result = QxpD3d9CapturedDrawRecordToDraw2(
        &plan, 0, &draw, &status);
    qxp_check(result && draw.primitive_type == expected_type &&
                  draw.first_vertex_offset == expected_offset &&
                  draw.primitive_count == expected_count,
              "captured record normalizes to exact draw2 fields");
    memset(&draw, 0xcc, sizeof(draw));
    qxp_check(!QxpD3d9CapturedDrawRecordToDraw2(
                  &plan, 1, &draw, &status) &&
                  draw.primitive_type == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
              "captured record rejects an out-of-range index and zeros output");
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_single_stream_byte_phase_execution(void)
{
    QXP_TEST_DRAW2 draw;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    QXP_D3D9_CAPTURED_DRAW2 captured;
    void *blob;
    DWORD blob_bytes;
    BOOL result;

    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.first_vertex_offset = 66UL;
    draw.primitive_count = 1UL;
    blob = NULL;
    blob_bytes = 0;
    result = qxp_multi_build(
                 QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw,
                 (ULONG)sizeof(draw), 1UL, FALSE, 4096) &&
             qxp_multi_capture(
                 QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw,
                 (ULONG)sizeof(draw), &blob, &blob_bytes, &status);
    qxp_check(result && blob != NULL,
              "single-stream arbitrary byte phase capture builds");
    if (!blob) return;

    memset(&plan, 0xcc, sizeof(plan));
    memset(&captured, 0xcc, sizeof(captured));
    result = QxpD3d9PrepareCapturedDrawExecution(
                 blob, blob_bytes, &plan, &status) &&
             QxpD3d9CapturedDrawRecordToDraw2(
                 &plan, 0, &captured, &status);
    qxp_check(result && plan.stream_mask == 1UL &&
                  plan.logical_first == 5UL &&
                  plan.logical_last_exclusive == 8UL &&
                  plan.streams[0].payload != NULL &&
                  plan.streams[0].payload_bytes == 31UL &&
                  memcmp(
                      plan.streams[0].payload,
                      qxp_multi_buffer[0] + 69UL, 31UL) == 0 &&
                  captured.primitive_type ==
                      QXP_D3D9_PRIMITIVE_TRIANGLELIST &&
                  captured.first_vertex_offset == 66UL &&
                  captured.primitive_count == 1UL,
              "single-stream arbitrary byte phase normalizes for execution");
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_stream1_only_execution(void)
{
    QXP_TEST_DRAW2 draw;
    QXP_TEST_INDEXED indexed;
    QXP_D3D9_DRAW_PREPARED indexed_prepared;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    QXP_D3D9_CAPTURED_DRAW2 captured;
    QXP_D3D9_CAPTURED_INDEXED_DRAW2 captured_indexed;
    WORD indices[3];
    void *blob;
    DWORD blob_bytes;
    BOOL result;

    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.first_vertex_offset = 26UL;
    draw.primitive_count = 1UL;
    blob = NULL;
    blob_bytes = 0;
    result = qxp_multi_build(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw,
        (ULONG)sizeof(draw), 1UL << 7, FALSE, 4096) &&
        qxp_multi_capture(
            QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1, &draw,
            (ULONG)sizeof(draw), &blob, &blob_bytes, &status);
    qxp_check(result && blob != NULL,
              "stream-one-only executable capture builds");
    if (blob) {
        memset(&plan, 0xcc, sizeof(plan));
        memset(&captured, 0xcc, sizeof(captured));
        result = QxpD3d9PrepareCapturedDrawExecution(
                     blob, blob_bytes, &plan, &status) &&
                 QxpD3d9CapturedDrawRecordToDraw2(
                     &plan, 0, &captured, &status);
        qxp_check(result && plan.stream_mask == (1UL << 7) &&
                      plan.streams[0].payload == NULL &&
                      plan.streams[7].payload != NULL &&
                      plan.logical_first == 2UL &&
                      plan.logical_last_exclusive == 5UL &&
                      captured.primitive_type ==
                          QXP_D3D9_PRIMITIVE_TRIANGLELIST &&
                      captured.first_vertex_offset == 26UL &&
                      captured.primitive_count == 1UL,
                  "stream-one-only execution uses raw stream-zero divisor");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }

    memset(&indexed_prepared, 0, sizeof(indexed_prepared));
    indexed_prepared.raw_state.streams[0].stride = 13UL;
    indexed.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    indexed.base_vertex = 2;
    indexed.min_index = 0;
    indexed.num_vertices = 3;
    indexed.start_index = 0;
    indexed.primitive_count = 1;
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    memset(&plan, 0, sizeof(plan));
    plan.prepared = &indexed_prepared;
    plan.records = &indexed;
    plan.records_bytes = (DWORD)sizeof(indexed);
    plan.opcode = QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE;
    plan.record_count = 1;
    plan.indexed = TRUE;
    plan.stream_mask = 1UL << 7;
    plan.index.valid = TRUE;
    plan.index.stride = 2;
    plan.index.byte_first = 0;
    plan.index.byte_last_exclusive = (DWORD)sizeof(indices);
    plan.index.payload_bytes = (DWORD)sizeof(indices);
    plan.index_payload = indices;
    memset(&captured_indexed, 0xcc, sizeof(captured_indexed));
    result = QxpD3d9CapturedDrawRecordToIndexedDraw2(
        &plan, 0, &captured_indexed, &status);
    qxp_check(result &&
                  captured_indexed.base_vertex_offset == 26 &&
                  captured_indexed.start_index_offset == 0 &&
                  captured_indexed.num_vertices == 3,
              "stream-one-only indexed conversion uses raw stream-zero divisor");
}

static void qxp_test_legacy_multistream_prepared_capture(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_PREPARED prepared;
    QXP_D3D9_DRAW_OWNER_IDENTITY owner;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_ENVELOPE_SNAPSHOT envelope;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    DWORD spans[QXP_D3D9_DRAW_STREAM_COUNT];
    DWORD saved_handle;
    DWORD blob_bytes;
    DWORD stream;
    void *blob;

    points.count = 4;
    points.start = 2;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), FALSE,
                  4096),
              "legacy multistream prepared foundation builds");
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        if (!qxp_multi_prepared.raw_state.streams[stream].divider)
            qxp_multi_prepared.raw_state.streams[stream].divider = 1;
        spans[stream] = qxp_multi_prepared.required_stream_spans[stream];
    }
    owner = qxp_multi_prepared.owner;
    memset(&pixel, 0, sizeof(pixel));
    memset(&prepared, 0xcc, sizeof(prepared));
    qxp_check(QxpD3d9BuildLegacyMultistreamPreparedCapture(
                  &qxp_multi_prepared.raw_state, &pixel, &owner,
                  (1UL << 0) | (1UL << 7), spans, &prepared, &status) &&
                  prepared.valid == TRUE &&
                  prepared.mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE &&
                  !memcmp(&prepared.owner, &owner, sizeof(owner)) &&
                  !memcmp(&prepared.raw_state,
                          &qxp_multi_prepared.raw_state,
                          sizeof(prepared.raw_state)) &&
                  prepared.required_stream_mask ==
                      ((1UL << 0) | (1UL << 7)) &&
                  prepared.required_user_memory_stream_mask == 0 &&
                  prepared.required_stream_spans[0] == 5UL &&
                  prepared.required_stream_spans[7] == 7UL,
              "legacy VS1.1 multistream authority is canonical");

    memset(&envelope, 0, sizeof(envelope));
    blob = NULL;
    blob_bytes = 0;
    qxp_recorder_reset();
    qxp_check(QxpD3d9BuildDrawEnvelope(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &prepared, &qxp_multi_backings,
                  &envelope, &envelope_status) &&
                  QxpD3d9CaptureNonindexedDrawStreams(
                      &envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
                      (ULONG)sizeof(points), &prepared, &qxp_multi_backings,
                      qxp_multi_sources, &qxp_cap_allocator, qxp_secure,
                      qxp_unsecure, qxp_copy, &qxp_recorder,
                      &blob, &blob_bytes, &status) &&
                  QxpD3d9PrepareCapturedDrawExecution(
                      blob, blob_bytes, &plan, &status) &&
                  plan.stream_mask == ((1UL << 0) | (1UL << 7)) &&
                  plan.logical_first == 2UL &&
                  plan.logical_last_exclusive == 6UL,
              "legacy VS1.1 producer reaches executable immutable capture");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    pixel.sm2.handle = 3;
    pixel.sm2.generation = 1;
    memset(&prepared, 0xcc, sizeof(prepared));
    qxp_check(!QxpD3d9BuildLegacyMultistreamPreparedCapture(
                  &qxp_multi_prepared.raw_state, &pixel, &owner,
                  (1UL << 0) | (1UL << 7), spans, &prepared, &status) &&
                  prepared.valid == FALSE &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
              "legacy producer rejects native SM2 pixel authority");
    memset(&pixel, 0, sizeof(pixel));

    qxp_multi_prepared.raw_state.vertex_function_binding.handle = 5;
    qxp_multi_prepared.raw_state.vertex_function_binding.generation = 1;
    memset(&prepared, 0xcc, sizeof(prepared));
    qxp_check(!QxpD3d9BuildLegacyMultistreamPreparedCapture(
                  &qxp_multi_prepared.raw_state, &pixel, &owner,
                  (1UL << 0) | (1UL << 7), spans, &prepared, &status) &&
                  prepared.valid == FALSE &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
              "legacy producer rejects native vertex-function authority");
    qxp_multi_prepared.raw_state.vertex_function_binding.handle = 0;
    qxp_multi_prepared.raw_state.vertex_function_binding.generation = 0;

    saved_handle = qxp_multi_prepared.raw_state.streams[7].handle;
    qxp_multi_prepared.raw_state.streams[7].handle = 0;
    memset(&prepared, 0xcc, sizeof(prepared));
    qxp_check(!QxpD3d9BuildLegacyMultistreamPreparedCapture(
                  &qxp_multi_prepared.raw_state, &pixel, &owner,
                  (1UL << 0) | (1UL << 7), spans, &prepared, &status) &&
                  prepared.valid == FALSE && status.stream == 7,
              "legacy producer rejects an unbound required stream");
    qxp_multi_prepared.raw_state.streams[7].handle = saved_handle;

    memset(&prepared, 0xcc, sizeof(prepared));
    qxp_check(!QxpD3d9BuildLegacyMultistreamPreparedCapture(
                  &qxp_multi_prepared.raw_state, &pixel, &owner,
                  1UL << 7, spans, &prepared, &status) &&
                  prepared.valid == FALSE,
              "legacy producer requires stream zero and another stream");
}

static void qxp_test_captured_execution_plan(void)
{
    QXP_TEST_POINTS points;
    QXP_TEST_DRAW draw;
    QXP_TEST_FAN fan;
    QXP_TEST_FAN fan_batch[2];
    QXP_TEST_DRAW2 draw2;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN plan;
    QXP_D3D9_CAPTURED_DRAW2 normalized;
    void *blob;
    DWORD blob_bytes;

    points.count = 4;
    points.start = 2;
    qxp_check_captured_execution_record(
        QXP_D3D9_DP2OP_POINTS, &points, (ULONG)sizeof(points),
        QXP_D3D9_PRIMITIVE_POINTLIST, 26UL, 4UL, 2UL, 6UL,
        "points capture prepares for execution");

    draw.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw.vertex_start = 3;
    draw.primitive_count = 2;
    qxp_check_captured_execution_record(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, &draw, (ULONG)sizeof(draw),
        QXP_D3D9_PRIMITIVE_TRIANGLELIST, 39UL, 2UL, 3UL, 9UL,
        "drawprimitive capture prepares for execution");

    fan.first_vertex_offset = 52;
    fan.edge_flags = 0;
    fan.primitive_count = 2;
    qxp_check_captured_execution_record(
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, &fan, (ULONG)sizeof(fan),
        QXP_D3D9_PRIMITIVE_TRIANGLEFAN, 52UL, 2UL, 4UL, 8UL,
        "clipped fan capture prepares for execution");

    draw2.primitive_type = QXP_D3D9_PRIMITIVE_TRIANGLELIST;
    draw2.first_vertex_offset = 65;
    draw2.primitive_count = 1;
    qxp_check_captured_execution_record(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, &draw2, (ULONG)sizeof(draw2),
        QXP_D3D9_PRIMITIVE_TRIANGLELIST, 65UL, 1UL, 5UL, 8UL,
        "drawprimitive2 capture prepares for execution");

    fan.edge_flags = 1;
    blob = NULL;
    blob_bytes = 0;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1, &fan,
                  (ULONG)sizeof(fan), (1UL << 0) | (1UL << 7), FALSE,
                  4096) &&
                  qxp_multi_capture(
                      QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 1, &fan,
                      (ULONG)sizeof(fan), &blob, &blob_bytes, &status) &&
                  QxpD3d9PrepareCapturedDrawExecution(
                      blob, blob_bytes, &plan, &status) &&
                  QxpD3d9CapturedDrawRecordToDraw2(
                      &plan, 0, &normalized, &status) &&
                  normalized.primitive_type ==
                      QXP_D3D9_PRIMITIVE_TRIANGLEFAN &&
                  normalized.first_vertex_offset == fan.first_vertex_offset &&
                  normalized.primitive_count == fan.primitive_count,
              "r535 clipped fan edge flags capture as ordinary solid fill");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    fan_batch[0].first_vertex_offset = 52;
    fan_batch[0].edge_flags = 0;
    fan_batch[0].primitive_count = 2;
    fan_batch[1].first_vertex_offset = 65;
    fan_batch[1].edge_flags = 1;
    fan_batch[1].primitive_count = 1;
    blob = NULL;
    blob_bytes = 0;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 2, fan_batch,
                  (ULONG)sizeof(fan_batch), (1UL << 0) | (1UL << 7),
                  FALSE, 4096) &&
                  qxp_multi_capture(
                      QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN, 2, fan_batch,
                      (ULONG)sizeof(fan_batch), &blob, &blob_bytes,
                      &status) &&
                  QxpD3d9PrepareCapturedDrawExecution(
                      blob, blob_bytes, &plan, &status) &&
                  QxpD3d9CapturedDrawRecordToDraw2(
                      &plan, 0, &normalized, &status) &&
                  QxpD3d9CapturedDrawRecordToDraw2(
                      &plan, 1, &normalized, &status) &&
                  normalized.first_vertex_offset == 65UL &&
                  normalized.primitive_count == 1UL,
              "r535 mixed flag batch preflights and converts every record");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    blob = NULL;
    blob_bytes = 0;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 1UL << 7, FALSE, 4096) &&
                  qxp_multi_capture(
                      QXP_D3D9_DP2OP_POINTS, 1, &points,
                      (ULONG)sizeof(points), &blob, &blob_bytes, &status) &&
                  QxpD3d9PrepareCapturedDrawExecution(
                      blob, blob_bytes, &plan, &status) &&
                  plan.stream_mask == (1UL << 7) &&
                  plan.streams[0].payload == NULL &&
                  plan.streams[7].payload != NULL &&
                  plan.logical_first == 2UL &&
                  plan.logical_last_exclusive == 6UL,
              "captured execution accepts a linked nonzero stream");
    if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_all_sixteen_streams_and_failures(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *payload;
    void *blob;
    DWORD bytes;
    DWORD stream;
    DWORD ordinal;
    DWORD release_index;
    DWORD cursor;
    DWORD payload_bytes;
    DWORD callback_count;
    int order_exact;
    BOOL result;

    points.count = 3;
    points.start = 3;
    callback_count = QXP_D3D9_DRAW_BACKING_STREAM_COUNT + 1UL;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points),
                  QXP_D3D9_DRAW_CAPTURE_STREAM_MASK, TRUE, 4096),
              "all-streams-0-through-15 foundation builds");
    qxp_multi_sources[14].base = qxp_multi_sources[15].base;
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) && blob,
              "all required streams 0 through 15 capture successfully");
    if (!blob) return;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    order_exact =
        header->stream_mask == QXP_D3D9_DRAW_CAPTURE_STREAM_MASK &&
        header->stream_count == QXP_D3D9_DRAW_BACKING_STREAM_COUNT &&
        qxp_recorder.secure_count == callback_count &&
        qxp_recorder.copy_count == callback_count &&
        qxp_recorder.unsecure_count == callback_count;
    cursor = header->records_offset + header->records_bytes;
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        payload = QxpD3d9CapturedDrawStreamPayload(
            blob, stream, &payload_bytes);
        if (header->streams[stream].valid != TRUE ||
            header->streams[stream].source_kind !=
                (stream == 0 ?
                 QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL :
                 QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR) ||
            header->streams[stream].payload_offset != cursor ||
            payload != (const BYTE *)blob + cursor ||
            payload_bytes != header->streams[stream].payload_bytes ||
            !qxp_bytes_equal(
                payload,
                (const BYTE *)qxp_multi_sources[stream].base +
                    header->streams[stream].byte_first,
                payload_bytes) ||
            qxp_recorder.secure_addr[stream + 1UL] !=
                (const BYTE *)qxp_multi_sources[stream].base +
                    header->streams[stream].byte_first ||
            qxp_recorder.copy_src[stream + 1UL] !=
                qxp_recorder.secure_addr[stream + 1UL] ||
            qxp_recorder.copy_dest[stream + 1UL] !=
                (BYTE *)blob + header->streams[stream].payload_offset) {
            order_exact = 0;
        }
        cursor += header->streams[stream].payload_bytes;
    }
    for (stream = 0; stream < callback_count; ++stream) {
        if (qxp_recorder.unsecure_handle[stream] !=
                qxp_recorder.secure_handle[
                    callback_count - 1UL - stream]) order_exact = 0;
    }
    qxp_check(order_exact && cursor == bytes,
              "all-sixteen descriptors/layout/callback order are exact");
    qxp_check(header->streams[14].byte_first !=
                      header->streams[15].byte_first &&
                  qxp_bytes_equal(
                      QxpD3d9CapturedDrawStreamPayload(blob, 14, NULL),
                      (const BYTE *)qxp_multi_sources[14].base +
                          header->streams[14].byte_first,
                      header->streams[14].payload_bytes) &&
                  qxp_bytes_equal(
                      QxpD3d9CapturedDrawStreamPayload(blob, 15, NULL),
                      (const BYTE *)qxp_multi_sources[15].base +
                          header->streams[15].byte_first,
                      header->streams[15].payload_bytes),
              "shared high-stream source bytes keep distinct offsets/ranges");
    qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "all-sixteen capture revalidates");
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

    for (ordinal = 1; ordinal <= callback_count; ++ordinal) {
        qxp_recorder_reset();
        qxp_recorder.fail_secure = ordinal;
        blob = (void *)0xdeadu;
        bytes = 0xdeadu;
        result = QxpD3d9CaptureNonindexedDrawStreams(
            &qxp_multi_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
            (ULONG)sizeof(points), &qxp_multi_prepared, &qxp_multi_backings,
            qxp_multi_sources, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
            qxp_copy, &qxp_recorder, &blob, &bytes, &status);
        order_exact = 1;
        if (ordinal > 1UL) {
            for (release_index = 0; release_index < ordinal - 2UL;
                 ++release_index) {
                if (qxp_recorder.unsecure_handle[release_index] !=
                        qxp_recorder.secure_handle[
                            ordinal - 2UL - release_index]) {
                    order_exact = 0;
                }
            }
            if (qxp_recorder.unsecure_handle[ordinal - 2UL] !=
                    qxp_recorder.secure_handle[0]) order_exact = 0;
        }
        qxp_check(!result && blob == NULL && bytes == 0 &&
                      qxp_recorder.secure_count == ordinal &&
                      qxp_recorder.unsecure_count == ordinal - 1UL &&
                      qxp_recorder.copy_count == 0 && order_exact &&
                      status.code ==
                          (ordinal == 1UL ?
                           QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_RECORDS :
                           QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY) &&
                      (ordinal == 1UL || status.stream == ordinal - 2UL),
                  "every all-sixteen secure ordinal reverse-releases exactly");
    }

    for (ordinal = 1; ordinal <= callback_count; ++ordinal) {
        qxp_recorder_reset();
        qxp_recorder.fail_copy = ordinal;
        blob = (void *)0xdeadu;
        bytes = 0xdeadu;
        result = QxpD3d9CaptureNonindexedDrawStreams(
            &qxp_multi_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
            (ULONG)sizeof(points), &qxp_multi_prepared, &qxp_multi_backings,
            qxp_multi_sources, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
            qxp_copy, &qxp_recorder, &blob, &bytes, &status);
        order_exact = 1;
        for (stream = 0; stream < callback_count; ++stream) {
            if (qxp_recorder.unsecure_handle[stream] !=
                    qxp_recorder.secure_handle[
                        callback_count - 1UL - stream]) order_exact = 0;
        }
        qxp_check(!result && blob == NULL && bytes == 0 &&
                      qxp_recorder.secure_count == callback_count &&
                      qxp_recorder.copy_count == ordinal &&
                      qxp_recorder.unsecure_count == callback_count &&
                      order_exact &&
                      status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_COPY &&
                      (ordinal == 1UL || status.stream == ordinal - 2UL),
                  "every all-sixteen copy ordinal reverse-releases exactly");
    }
}

static void qxp_test_stream15_boundary(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    void *blob;
    DWORD bytes;
    DWORD payload_bytes;

    points.count = 2;
    points.start = 4;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 15), TRUE,
                  4096),
              "stream-15 boundary foundation builds");
    blob = NULL;
    bytes = 0;
    qxp_check(qxp_multi_capture(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &blob, &bytes, &status) && blob,
              "sparse stream-0+15 boundary capture succeeds");
    if (!blob) return;
    header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
    qxp_check(header->stream_mask == ((1UL << 0) | (1UL << 15)) &&
                  header->stream_count == 2UL &&
                  header->streams[15].valid == TRUE &&
                  QxpD3d9CapturedDrawStreamPayload(
                      blob, 15, &payload_bytes) != NULL &&
                  payload_bytes == header->streams[15].payload_bytes &&
                  qxp_recorder.secure_count == 3UL &&
                  qxp_recorder.secure_addr[2] ==
                      (const BYTE *)qxp_multi_sources[15].base +
                          header->streams[15].byte_first &&
                  qxp_recorder.unsecure_handle[0] ==
                      qxp_recorder.secure_handle[2] &&
                  QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
              "stream-15 descriptor, payload and callback boundary are exact");
    QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
}

static void qxp_test_aggregate_capacity(void)
{
    QXP_TEST_DRAW small_draw;
    QXP_TEST_DRAW draw;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    BYTE *big;
    void *blob;
    DWORD bytes;
    DWORD aggregate_room;
    DWORD each;
    DWORD stream;
    BOOL result;

    small_draw.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    small_draw.vertex_start = 0;
    small_draw.primitive_count = 1;
    aggregate_room = QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES -
                     QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET -
                     QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE;
    each = aggregate_room / 2UL;
    big = (BYTE *)malloc((size_t)each);
    qxp_check(big != NULL && each * 2UL == aggregate_room,
              "aggregate cap fixture allocation and parity");
    if (big) {
        memset(big, 0x6b, (size_t)each);
        qxp_check(qxp_multi_build(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &small_draw,
                      (ULONG)sizeof(small_draw), (1UL << 0) | (1UL << 7),
                      TRUE, each),
                  "aggregate exact-cap base foundation");
        for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
             ++stream) {
            if (!((qxp_multi_prepared.required_stream_mask >> stream) & 1UL))
                continue;
            qxp_multi_prepared.raw_state.streams[stream].offset = 0;
            qxp_multi_prepared.raw_state.streams[stream].stride = 1;
            qxp_multi_prepared.raw_state.streams[stream].source2 = FALSE;
            qxp_multi_prepared.required_stream_spans[stream] = 1;
            qxp_multi_backings.streams[stream].source2_offset = 0;
            qxp_multi_backings.streams[stream].stride = 1;
            qxp_multi_backings.streams[stream].required_span = 1;
            qxp_multi_sources[stream].base = big;
        }
        draw = small_draw;
        draw.primitive_count = each;
        qxp_check(QxpD3d9BuildDrawEnvelope(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                      (ULONG)sizeof(draw), &qxp_multi_prepared,
                      &qxp_multi_backings, &qxp_multi_envelope,
                      &envelope_status),
                  "aggregate exact-cap envelope rebuilds");
        blob = NULL;
        bytes = 0;
        qxp_check(qxp_multi_capture(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                      (ULONG)sizeof(draw), &blob, &bytes, &status) && blob &&
                      bytes == QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES,
                  "aggregate stream payload exactly at cap succeeds");
        if (blob) QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);

        ++draw.primitive_count;
        for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT;
             ++stream) {
            if (!((qxp_multi_prepared.required_stream_mask >> stream) & 1UL))
                continue;
            qxp_multi_backings.streams[stream].byte_length = each + 1UL;
            qxp_multi_sources[stream].byte_count = each + 1UL;
        }
        qxp_check(QxpD3d9BuildDrawEnvelope(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                      (ULONG)sizeof(draw), &qxp_multi_prepared,
                      &qxp_multi_backings, &qxp_multi_envelope,
                      &envelope_status),
                  "aggregate over-cap envelope rebuilds");
        blob = (void *)0xdeadu;
        bytes = 0xdeadu;
        result = qxp_multi_capture(
            QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
            (ULONG)sizeof(draw), &blob, &bytes, &status);
        qxp_check(!result && blob == NULL && bytes == 0 &&
                      status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY &&
                      qxp_recorder.secure_count == 0,
                  "aggregate stream payload over cap rejects before resources");
        free(big);
    }

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &small_draw,
                  (ULONG)sizeof(small_draw), 0x0fUL, FALSE, 4096),
              "aggregate DWORD-overflow base foundation");
    for (stream = 0; stream < 4UL; ++stream) {
        qxp_multi_prepared.raw_state.streams[stream].offset = 0;
        qxp_multi_prepared.raw_state.streams[stream].stride = 1;
        qxp_multi_prepared.raw_state.streams[stream].source2 = FALSE;
        qxp_multi_prepared.required_stream_spans[stream] = 1;
        qxp_multi_backings.streams[stream].source2_offset = 0;
        qxp_multi_backings.streams[stream].stride = 1;
        qxp_multi_backings.streams[stream].required_span = 1;
        qxp_multi_backings.streams[stream].byte_length = 0x40000000UL;
        qxp_multi_sources[stream].base = (const void *)(ULONG_PTR)0x10000UL;
        qxp_multi_sources[stream].byte_count = 0x40000000UL;
    }
    draw = small_draw;
    draw.primitive_count = 0x40000000UL;
    qxp_check(QxpD3d9BuildDrawEnvelope(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                  (ULONG)sizeof(draw), &qxp_multi_prepared,
                  &qxp_multi_backings, &qxp_multi_envelope,
                  &envelope_status),
              "aggregate DWORD-overflow envelope builds");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_multi_capture(
        QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw),
        &blob, &bytes, &status);
    qxp_check(!result && blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY &&
                  qxp_recorder.secure_count == 0,
              "aggregate DWORD overflow rejects before any callback");
}

static void qxp_expect_contract_rejection(
    BOOL surface, const QXP_TEST_POINTS *points, const char *name)
{
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    void *blob;
    DWORD bytes;
    BOOL result;

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    if (surface) {
        result = qxp_syn_surface_capture(
            QXP_D3D9_DP2OP_POINTS, 1, points, (ULONG)sizeof(*points),
            qxp_syn_buffer, 4096, &blob, &bytes, &status);
    } else {
        result = qxp_syn_capture(
            QXP_D3D9_DP2OP_POINTS, 1, points, (ULONG)sizeof(*points),
            qxp_syn_buffer, 4096, &blob, &bytes, &status);
    }
    qxp_check(!result && blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE &&
                  qxp_recorder.secure_count == 0 &&
                  qxp_recorder.copy_count == 0,
              name);
}

static void qxp_test_cross_identity_contracts(void)
{
    QXP_TEST_POINTS points;

    points.count = 2;
    points.start = 1;

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "user identity handle foundation");
    qxp_syn_backings.streams[0].handle = 1;
    qxp_expect_contract_rejection(
        FALSE, &points, "user capture rejects a nonzero backing handle");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "user identity incarnation foundation");
    qxp_syn_backings.streams[0].incarnation = 1;
    qxp_expect_contract_rejection(
        FALSE, &points, "user capture rejects a nonzero incarnation");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "user identity generation foundation");
    qxp_syn_backings.streams[0].content_generation = 1;
    qxp_expect_contract_rejection(
        FALSE, &points, "user capture rejects a nonzero content generation");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface identity handle foundation");
    qxp_syn_backings.streams[0].handle = 0;
    qxp_expect_contract_rejection(
        TRUE, &points, "surface capture rejects a zero backing handle");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface identity incarnation foundation");
    qxp_syn_backings.streams[0].incarnation = 0;
    qxp_expect_contract_rejection(
        TRUE, &points, "surface capture rejects a zero incarnation");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface identity generation foundation");
    qxp_syn_backings.streams[0].content_generation = 0;
    qxp_expect_contract_rejection(
        TRUE, &points, "surface capture rejects a zero content generation");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface prepared provenance foundation");
    qxp_syn_prepared.raw_state.streams[0].user_memory = TRUE;
    qxp_expect_contract_rejection(
        TRUE, &points, "surface capture rejects user_memory TRUE");

    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "user prepared provenance foundation");
    qxp_syn_prepared.raw_state.streams[0].user_memory = FALSE;
    qxp_expect_contract_rejection(
        FALSE, &points, "user capture rejects user_memory FALSE");
}

static void qxp_test_surface_eligibility(void)
{
    QXP_TEST_POINTS points;
    DWORD saved_kind;

    points.count = 2;
    points.start = 1;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface eligibility foundation");
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, &qxp_syn_prepared,
                  &qxp_syn_backings) == TRUE,
              "exact system-linear stream-0 shape is surface eligible");
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, FALSE,
                  &qxp_syn_prepared, &qxp_syn_backings) == FALSE &&
                  QxpD3d9DrawCaptureIsSurfaceEligible(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2, FALSE,
                      &qxp_syn_prepared, &qxp_syn_backings) == FALSE &&
                  QxpD3d9DrawCaptureIsSurfaceEligible(
                      QXP_D3D9_DP2OP_POINTS, TRUE, &qxp_syn_prepared,
                      &qxp_syn_backings) == FALSE,
              "indexed opcodes and indexed classification are surface ineligible");

    saved_kind = qxp_syn_backings.streams[0].kind;
    qxp_syn_backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_USER_MEMORY_CALL;
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, &qxp_syn_prepared,
                  &qxp_syn_backings) == FALSE,
              "a user-memory stream-0 kind is surface ineligible");
    qxp_syn_backings.streams[0].kind = saved_kind;

    qxp_syn_backings.populated_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, &qxp_syn_prepared,
                  &qxp_syn_backings) == FALSE,
              "surface populated_stream_mask 3 is ineligible");
    qxp_syn_backings.populated_stream_mask = 0;
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, &qxp_syn_prepared,
                  &qxp_syn_backings) == FALSE,
              "surface populated_stream_mask 0 is ineligible");
    qxp_syn_backings.populated_stream_mask = 1UL;

    qxp_syn_backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    qxp_check(QxpD3d9DrawCaptureIsSurfaceEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, &qxp_syn_prepared,
                  &qxp_syn_backings) == TRUE,
              "a video-linear stream-0 kind is surface eligible");
}

static void qxp_test_surface_extent_arithmetic(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    void *blob;
    DWORD bytes;
    BOOL result;

    points.count = 1;
    points.start = 0;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface short-extent foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_surface_capture(
        QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
        qxp_syn_buffer, 11, &blob, &bytes, &status);
    qxp_check(!result && blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING &&
                  qxp_recorder.secure_count == 0,
              "surface required span cannot be truncated by declared length");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_surface_capture(
        QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
        (const void *)(ULONG_PTR)0xfffff800UL, 4096, &blob, &bytes, &status);
    qxp_check(!result && blob == NULL && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY &&
                  qxp_recorder.secure_count == 0,
              "surface base plus declared byte length cannot wrap");
}

static void qxp_test_callback_failures(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    void *blob;
    DWORD bytes;
    BOOL result;

    points.count = 2;
    points.start = 1;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "synthetic callback-failure foundation");

    qxp_cap_alloc_fail = TRUE;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_ALLOCATION, TRUE,
        "allocation failure yields ALLOCATION and NULL output");
    qxp_check(qxp_recorder.secure_count == 0 && qxp_recorder.copy_count == 0,
              "allocation failure secures and copies nothing");
    qxp_cap_alloc_fail = FALSE;

    qxp_recorder_reset();
    qxp_recorder.fail_secure = 1;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(QxpD3d9CaptureNonindexedDraw(
                  &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
                  qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure,
                  qxp_unsecure, qxp_copy, &qxp_recorder, &blob, &bytes,
                  &status) == FALSE,
              "secure records failure returns FALSE");
    qxp_expect_capture_failure(
        FALSE, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_RECORDS, FALSE,
        "secure call 1 failure yields SECURE_RECORDS and NULL output");
    qxp_check(qxp_recorder.secure_count == 1 &&
                  qxp_recorder.unsecure_count == 0 &&
                  qxp_recorder.copy_count == 0,
              "secure call 1 failure unsecures nothing and copies nothing");

    qxp_recorder_reset();
    qxp_recorder.fail_secure = 2;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    (void)QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        FALSE, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY, FALSE,
        "secure call 2 failure yields SECURE_USER_MEMORY and NULL output");
    qxp_check(qxp_recorder.secure_count == 2 &&
                  qxp_recorder.copy_count == 0 &&
                  qxp_recorder.unsecure_count == 1 &&
                  qxp_recorder.unsecure_handle[0] ==
                      qxp_recorder.secure_handle[0],
              "secure call 2 failure unsecures exactly the record handle");

    qxp_recorder_reset();
    qxp_recorder.fail_copy = 1;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    (void)QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        FALSE, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_COPY, FALSE,
        "copy call 1 failure yields COPY and NULL output");
    qxp_check(qxp_recorder.copy_count == 1 && qxp_recorder.secure_count == 2 &&
                  qxp_recorder.unsecure_count == 2 &&
                  qxp_recorder.unsecure_handle[0] ==
                      qxp_recorder.secure_handle[1] &&
                  qxp_recorder.unsecure_handle[1] ==
                      qxp_recorder.secure_handle[0] &&
                  status.field == QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_PAYLOAD,
              "copy call 1 failure reverse-unsecures both handles");

    qxp_recorder_reset();
    qxp_recorder.fail_copy = 2;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    (void)QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        FALSE, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_COPY, FALSE,
        "copy call 2 failure yields COPY and NULL output");
    qxp_check(qxp_recorder.copy_count == 2 &&
                  qxp_recorder.unsecure_count == 2 &&
                  status.field == QXP_D3D9_DRAW_CAPTURE_FIELD_VERTEX_PAYLOAD,
              "copy call 2 failure reports the vertex payload field");

    /* A successful but hostile record copy is caught by live B6G3A after both
     * ranges have been reverse-unsecured, and the owned blob is released. */
    qxp_recorder_reset();
    qxp_recorder.corrupt_records = TRUE;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    (void)QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        FALSE, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ACCESS,
        FALSE, "live B6G3A rejects a corrupted owned record copy");
    qxp_check(status.access_error.code ==
                  QXP_D3D9_DRAW_ACCESS_ERROR_ENVELOPE &&
                  qxp_recorder.copy_count == 2 &&
                  qxp_recorder.unsecure_count == 2,
              "access failure preserves status after reverse unsecure");
    qxp_check(qxp_recorder.lock_violation == 0 && qxp_cap_lock_violation == 0,
              "every callback and allocation ran outside the lock");
}

static void qxp_test_null_arguments(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    void *blob;
    DWORD bytes;
    BOOL result;

    points.count = 2;
    points.start = 1;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "synthetic null-argument foundation");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        NULL, QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
        &qxp_syn_prepared, &qxp_syn_backings, qxp_syn_buffer, 4096,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null envelope is rejected");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, NULL,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null records is rejected");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), NULL, &qxp_syn_backings, qxp_syn_buffer,
        4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null prepared is rejected");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, NULL, qxp_syn_buffer,
        4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null backings is rejected");

    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure,
        qxp_unsecure, qxp_copy, &qxp_recorder, NULL, &bytes, &status);
    qxp_check(result == FALSE && bytes == 0 &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
              "null out_blob is rejected");

    blob = (void *)0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure,
        qxp_unsecure, qxp_copy, &qxp_recorder, &blob, NULL, &status);
    qxp_check(result == FALSE && blob == NULL &&
                  status.code == QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT,
              "null out_blob_bytes is rejected");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, qxp_secure,
        qxp_unsecure, qxp_copy, &qxp_recorder, &blob, &bytes, NULL);
    qxp_check(result == FALSE && blob == NULL && bytes == 0,
              "null status still leaves outputs NULL and zero");

    /* Allocator and callback nulls are only reached after canonical B6G2. */
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, NULL, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null allocator is rejected after B6G2");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        qxp_syn_buffer, 4096, &qxp_cap_allocator, NULL, qxp_unsecure,
        qxp_copy, &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_NULL_ARGUMENT, FALSE,
        "null secure callback is rejected after B6G2");

    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        NULL, 4096, &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY, FALSE,
        "null user-memory base is rejected");
}

static void qxp_test_semantic_rejects(void)
{
    QXP_TEST_POINTS points;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    void *blob;
    DWORD bytes;
    DWORD saved_kind;
    BOOL result;

    points.count = 2;
    points.start = 1;

    /* Truncated framing: a valid envelope re-validated against too few bytes. */
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "truncation foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points) - 1, qxp_syn_buffer, 4096,
                             &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "truncated record framing is rejected");

    /* Unknown opcode re-derives an empty envelope and mismatches. */
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(0xFEU, 1, &points, (ULONG)sizeof(points),
                             qxp_syn_buffer, 4096, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "unknown opcode is rejected");

    /* Zero record count with an otherwise valid empty envelope. */
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 0, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "zero-count foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 0, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_COUNT,
        FALSE, "zero record count is rejected");

    /* Empty accessed range: a single record with zero effective vertices. */
    points.count = 0;
    points.start = 1;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "empty-range foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_STREAM0,
        FALSE, "empty accessed range is rejected");

    /* Surface-backed stream 0 (no user-memory mask): a prepared rejection. */
    points.count = 2;
    points.start = 1;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, FALSE, FALSE, 0, 0, 0),
              "surface-backed foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_PREPARED,
        FALSE, "surface-backed stream 0 is rejected");

    /* Range disagreement: user_memory_bytes must equal the backing length. */
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points),
                  16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "range-mismatch foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4095, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_BACKING,
        FALSE, "user-memory length disagreeing with backing is rejected");

    /* Checked record framing rejects a count whose fixed-size multiplication
     * cannot fit a DWORD, without allocation, secure, or copy callbacks. */
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 16, 12, 4096,
                  TRUE, FALSE, 0, 0, 0),
              "overflow rejection baseline foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_recorder_reset();
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_DRAWPRIMITIVE,
        0x40000000UL, &points, 0xffffffffUL, &qxp_syn_prepared,
        &qxp_syn_backings, qxp_syn_buffer, 4096, &qxp_cap_allocator,
        qxp_secure, qxp_unsecure, qxp_copy, &qxp_recorder,
        &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "overflowing exact record byte count is rejected");
    qxp_check(qxp_recorder.secure_count == 0 && qxp_recorder.copy_count == 0,
              "record overflow reaches no capture callback");

    /* The full effective user-memory range must not wrap ULONG_PTR. */
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_recorder_reset();
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_POINTS, 1, &points,
        (ULONG)sizeof(points), &qxp_syn_prepared, &qxp_syn_backings,
        (const void *)(ULONG_PTR)0xfffff800UL, 4096,
        &qxp_cap_allocator, qxp_secure, qxp_unsecure, qxp_copy,
        &qxp_recorder, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_USER_MEMORY, FALSE,
        "wrapping effective user-memory range is rejected");
    qxp_check(qxp_recorder.secure_count == 0,
              "address overflow reaches no secure callback");

    /* Hostile B6G2 range mutation is rejected before capture resources. */
    ++qxp_syn_envelope.streams[0].byte_last_exclusive;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096,
                             &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "hostile stream range mutation is rejected through B6G2");
    --qxp_syn_envelope.streams[0].byte_last_exclusive;

    /* Tampered backing kind: the module re-derives the envelope and rejects. */
    saved_kind = qxp_syn_backings.streams[0].kind;
    qxp_syn_backings.streams[0].kind = QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_POINTS, 1, &points,
                             (ULONG)sizeof(points), qxp_syn_buffer, 4096, &blob,
                             &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "tampered backing kind is rejected through B6G2");
    qxp_syn_backings.streams[0].kind = saved_kind;
}

static void qxp_test_indexed_opcodes(void)
{
    static const BYTE opcodes[2] = {
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2
    };
    QXP_TEST_INDEXED record;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_D3D9_DRAW_ENVELOPE_STATUS envelope_status;
    QXP_D3D9_CAPTURED_EXECUTION_PLAN execution;
    QXP_D3D9_CAPTURED_INDEXED_DRAW2 translated;
    const QXP_D3D9_DRAW_CAPTURE_HEADER *header;
    const void *index_payload;
    WORD index_values[32];
    void *blob;
    DWORD bytes;
    DWORD index_bytes;
    DWORD opcode_index;
    BOOL result;

    memset(index_values, 0, sizeof(index_values));
    index_values[1] = 2;
    index_values[2] = 1;
    memset(qxp_syn_buffer, 0x5a, sizeof(qxp_syn_buffer));
    for (opcode_index = 0; opcode_index < QXP_COUNT(opcodes);
         ++opcode_index) {
        memset(&record, 0, sizeof(record));
        record.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
        record.base_vertex = opcode_index ?
            16L : 1L;
        record.min_index = 1;
        record.num_vertices = 2;
        record.start_index = opcode_index ?
            2UL : 1UL;
        record.primitive_count = 1;

        qxp_check(qxp_syn_build(
                      opcodes[opcode_index], 1, &record,
                      (ULONG)sizeof(record), 16, 12, 4096, FALSE, TRUE,
                      0xB00, 2, (DWORD)sizeof(index_values)),
                  opcode_index ?
                      "synthetic indexed foundation (opcode 60)" :
                      "synthetic indexed foundation (opcode 53)");
        if (opcode_index) {
            qxp_syn_backings.streams[0].kind =
                QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
            qxp_syn_backings.streams[0].video_memory_offset = 0x1000UL;
            qxp_syn_backings.index.kind =
                QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
            qxp_syn_backings.index.video_memory_offset = 0x3000UL;
            memset(&qxp_syn_envelope, 0, sizeof(qxp_syn_envelope));
            qxp_check(QxpD3d9BuildDrawEnvelope(
                          opcodes[opcode_index], 1, &record,
                          (ULONG)sizeof(record), &qxp_syn_prepared,
                          &qxp_syn_backings, &qxp_syn_envelope,
                          &envelope_status),
                      "video-linear indexed envelope rebuilds");
        }
        blob = NULL;
        bytes = 0;
        result = qxp_syn_indexed_capture(
            opcodes[opcode_index], 1, &record, (ULONG)sizeof(record),
            qxp_syn_buffer, (DWORD)sizeof(qxp_syn_buffer), index_values,
            (DWORD)sizeof(index_values), &blob, &bytes, &status);
        qxp_check(result && blob && bytes &&
                      status.code == QXP_D3D9_DRAW_CAPTURE_OK,
                  opcode_index ?
                      "DRAWINDEXEDPRIMITIVE2 capture succeeds" :
                      "DRAWINDEXEDPRIMITIVE capture succeeds");
        if (!blob) continue;
        header = (const QXP_D3D9_DRAW_CAPTURE_HEADER *)blob;
        qxp_check(
            header->version == QXP_D3D9_DRAW_CAPTURE_VERSION &&
                header->indexed == TRUE &&
                header->index.valid == TRUE &&
                header->index.source_kind ==
                    (opcode_index ?
                         QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR :
                         QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR) &&
                header->streams[0].source_kind ==
                    (opcode_index ?
                         QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR :
                         QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR) &&
                header->index.stride == 2UL &&
                header->index.byte_first == 2UL &&
                header->index.byte_last_exclusive == 6UL &&
                header->index.payload_bytes == 4UL &&
                header->streams[0].logical_first == 2UL &&
                header->streams[0].logical_last_exclusive == 4UL &&
                header->streams[0].byte_first == 32UL &&
                header->streams[0].byte_last_exclusive == 60UL,
            opcode_index ?
                "opcode 60 captures scanned index and vertex ranges" :
                "opcode 53 captures scanned index and vertex ranges");
        index_payload =
            QxpD3d9CapturedDrawIndexPayload(blob, &index_bytes);
        qxp_check(index_payload && index_bytes == 4UL &&
                      qxp_bytes_equal(
                          index_payload, (const BYTE *)index_values + 2,
                          index_bytes),
                  "indexed blob owns the exact referenced index bytes");
        qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
                  "indexed blob canonically revalidates");
        qxp_check(QxpD3d9PrepareCapturedDrawExecution(
                      blob, bytes, &execution, &status) &&
                      execution.indexed == TRUE &&
                      execution.index_payload == index_payload,
                  "indexed blob creates a blob-owned execution plan");
        qxp_check(QxpD3d9CapturedDrawRecordToIndexedDraw2(
                      &execution, 0, &translated, &status) &&
                      translated.primitive_type ==
                          QXP_D3D9_PRIMITIVE_LINELIST &&
                      translated.base_vertex_offset == 16L &&
                      translated.min_index == 1UL &&
                      translated.num_vertices == 2UL &&
                      translated.start_index_offset == 0UL &&
                      translated.primitive_count == 1UL,
                  "indexed record normalizes to the ...2 byte-offset form");
        qxp_check(qxp_recorder.secure_count ==
                      (opcode_index ? 1UL : 3UL) &&
                      qxp_recorder.copy_count == 3UL &&
                      qxp_recorder.unsecure_count ==
                      (opcode_index ? 1UL : 3UL),
                  opcode_index ?
                      "video-linear indexed capture secures records only" :
                      "system-linear indexed capture secures all inputs");
        QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
    }
}

static void qxp_test_capacity(void)
{
    QXP_TEST_DRAW draw;
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    BYTE *big;
    void *blob;
    DWORD bytes;
    DWORD exact_vertex;
    DWORD dummy;
    BOOL result;

    /* One blob exactly at the mebibyte cap: header + 12 record bytes + T. */
    exact_vertex = QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES -
                   QXP_D3D9_DRAW_CAPTURE_PAYLOAD_OFFSET -
                   QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE;
    big = (BYTE *)malloc((size_t)exact_vertex);
    qxp_check(big != NULL, "cap-exact user memory allocation");
    if (big) {
        DWORD index;

        for (index = 0; index < exact_vertex; ++index)
            big[index] = (BYTE)(index * 5U + 1U);
        draw.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
        draw.vertex_start = 0;
        draw.primitive_count = exact_vertex;
        qxp_check(qxp_syn_build(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                      (ULONG)sizeof(draw), 1, 1, exact_vertex, TRUE, FALSE, 0,
                      0, 0),
                  "cap-exact foundation");
        blob = (void *)0xdeadu;
        bytes = 0xdeadu;
        qxp_check(qxp_syn_capture(
                      QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                      (ULONG)sizeof(draw), big, exact_vertex, &blob, &bytes,
                      &status) &&
                      blob != NULL &&
                      bytes == QXP_D3D9_DRAW_CAPTURE_MAX_BLOB_BYTES,
                  "a blob exactly at the cap is captured");
        if (blob) {
            qxp_check(QxpD3d9ValidateCapturedDraw(blob, bytes, &status),
                      "cap-exact blob revalidates");
            QxpD3d9DestroyCapturedDraw(blob, &qxp_cap_allocator);
        }
        free(big);
    }

    /* One byte over the cap is rejected before any allocation or securing. */
    draw.primitive_type = QXP_D3D9_PRIMITIVE_POINTLIST;
    draw.vertex_start = 0;
    draw.primitive_count = exact_vertex + 1U;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw, (ULONG)sizeof(draw),
                  1, 1, exact_vertex + 1U, TRUE, FALSE, 0, 0, 0),
              "cap-plus-one foundation");
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    result = qxp_syn_capture(QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                             (ULONG)sizeof(draw), &dummy, exact_vertex + 1U,
                             &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_CAPACITY,
        FALSE, "one byte over the cap is rejected");
    qxp_check(qxp_recorder.secure_count == 0 && qxp_recorder.copy_count == 0,
              "over-cap rejection secures and copies nothing");
}

/* -- production helper coverage ------------------------------------------- */

static QXP_D3D9_DRAW_PREPARED qxp_elig_prepared;
static QXP_D3D9_DRAW_BACKING_SNAPSHOT qxp_elig_backings;

static void qxp_expect_um_reject(
    BOOL active, const void *vertices, DWORD offset, DWORD size, DWORD length,
    const char *name)
{
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE um;
    const void *base;
    DWORD bytes;

    memset(&um, 0xcc, sizeof(um));
    base = (const void *)(ULONG_PTR)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(QxpD3d9BuildDrawUserMemoryEnvelope(
                  active, vertices, offset, size, length, &um, &base,
                  &bytes) == FALSE &&
                  um.address_range_valid == FALSE && base == NULL &&
                  bytes == 0 && um.active == (active ? TRUE : FALSE) &&
                  um.nonnull_pointer_observed ==
                      (vertices ? TRUE : FALSE) &&
                  um.vertex_size == size && um.vertex_length == length &&
                  um.vertex_offset == offset,
              name);
}

/* The exact production effective-base arithmetic, now shared with
 * directdraw.c, is exercised directly: one exact valid case plus every
 * documented rejection.  No case dereferences the vertex pointer. */
static void qxp_test_user_memory_envelope_helper(void)
{
    QXP_D3D9_DRAW_BACKING_UM_ENVELOPE um;
    const void *base;
    DWORD bytes;

    memset(&um, 0xcc, sizeof(um));
    base = (const void *)(ULONG_PTR)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(QxpD3d9BuildDrawUserMemoryEnvelope(
                  TRUE, qxp_um_buffer + QXP_REAL_GUARD, QXP_REAL_UM_OFFSET,
                  QXP_REAL_VSIZE, QXP_REAL_VLEN, &um, &base,
                  &bytes) == TRUE &&
                  um.active == TRUE && um.nonnull_pointer_observed == TRUE &&
                  um.address_range_valid == TRUE &&
                  um.vertex_size == QXP_REAL_VSIZE &&
                  um.vertex_length == QXP_REAL_VLEN &&
                  um.vertex_offset == QXP_REAL_UM_OFFSET &&
                  base == (const void *)qxp_um_base &&
                  bytes == QXP_REAL_VSIZE * QXP_REAL_VLEN,
              "user-memory helper yields the exact base and byte count");

    qxp_expect_um_reject(
        FALSE, qxp_um_buffer + QXP_REAL_GUARD, QXP_REAL_UM_OFFSET,
        QXP_REAL_VSIZE, QXP_REAL_VLEN,
        "an inactive user-memory flag yields no effective range");
    qxp_expect_um_reject(
        TRUE, NULL, QXP_REAL_UM_OFFSET, QXP_REAL_VSIZE, QXP_REAL_VLEN,
        "a null vertex pointer yields no effective range");
    qxp_expect_um_reject(
        TRUE, qxp_um_buffer + QXP_REAL_GUARD, QXP_REAL_UM_OFFSET, 0,
        QXP_REAL_VLEN, "a zero vertex size yields no effective range");
    qxp_expect_um_reject(
        TRUE, qxp_um_buffer + QXP_REAL_GUARD, QXP_REAL_UM_OFFSET,
        QXP_REAL_VSIZE, 0, "a zero vertex length yields no effective range");
    qxp_expect_um_reject(
        TRUE, qxp_um_buffer + QXP_REAL_GUARD, 0, 0x10000UL, 0x10001UL,
        "an overflowing size times length yields no effective range");
    qxp_expect_um_reject(
        TRUE, (const void *)(ULONG_PTR)0xfffff000UL, 0x2000UL, 1UL, 1UL,
        "an overflowing vertex offset yields no effective range");
    qxp_expect_um_reject(
        TRUE, (const void *)(ULONG_PTR)0x80000000UL, 0, 1UL, 0x80000001UL,
        "an overflowing base plus byte count yields no effective range");

    base = (const void *)(ULONG_PTR)0xdeadu;
    bytes = 0xdeadu;
    qxp_check(QxpD3d9BuildDrawUserMemoryEnvelope(
                  TRUE, qxp_um_buffer + QXP_REAL_GUARD, QXP_REAL_UM_OFFSET,
                  QXP_REAL_VSIZE, QXP_REAL_VLEN, NULL, &base,
                  &bytes) == FALSE && base == NULL && bytes == 0,
              "a null envelope yields no effective range");
}

/* The exact production activation rule, now shared with directdraw.c, is
 * exercised over the real B6E/B6G1 fixture state. */
static void qxp_test_capture_eligibility_helper(void)
{
    static const BYTE eligible_opcodes[4] = {
        QXP_D3D9_DP2OP_POINTS,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE,
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2
    };
    static const BYTE indexed_opcodes[2] = {
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2
    };
    QXP_TEST_POINTS points;
    DWORD index;
    int all_eligible;
    int none_eligible;

    points.count = 2;
    points.start = 1;
    if (!qxp_real_make_envelope(
            QXP_D3D9_DP2OP_POINTS, 1, &points, (ULONG)sizeof(points))) {
        qxp_check(0, "eligibility fixture builds real B6G1/B6G2 state");
        return;
    }

    all_eligible = 1;
    for (index = 0; index < QXP_COUNT(eligible_opcodes); ++index) {
        if (QxpD3d9DrawCaptureIsEligible(
                eligible_opcodes[index], FALSE, TRUE, qxp_fixture.prepared,
                &qxp_backings) != TRUE) all_eligible = 0;
    }
    qxp_check(all_eligible,
              "all four nonindexed opcodes activate live capture");

    none_eligible = 1;
    for (index = 0; index < QXP_COUNT(indexed_opcodes); ++index) {
        if (QxpD3d9DrawCaptureIsEligible(
                indexed_opcodes[index], TRUE, TRUE, qxp_fixture.prepared,
                &qxp_backings)) none_eligible = 0;
        /* Even a caller that wrongly reports nonindexed cannot activate. */
        if (QxpD3d9DrawCaptureIsEligible(
                indexed_opcodes[index], FALSE, TRUE, qxp_fixture.prepared,
                &qxp_backings)) none_eligible = 0;
    }
    qxp_check(none_eligible,
              "both indexed opcodes never activate live capture");

    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, FALSE, qxp_fixture.prepared,
                  &qxp_backings) == FALSE,
              "an inactive user-memory flag never activates capture");
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, NULL,
                  &qxp_backings) == FALSE &&
                  QxpD3d9DrawCaptureIsEligible(
                      QXP_D3D9_DP2OP_POINTS, FALSE, TRUE,
                      qxp_fixture.prepared, NULL) == FALSE,
              "a null prepared or backing snapshot never activates capture");

    /* A surface-backed stream-0 vertex block stops at the B6G2 boundary. */
    memcpy(&qxp_elig_backings, &qxp_backings, sizeof(qxp_elig_backings));
    qxp_elig_backings.streams[0].kind =
        QXP_D3D9_DRAW_BACKING_KIND_SYSTEM_LINEAR;
    qxp_elig_backings.required_user_memory_stream_mask = 0;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, qxp_fixture.prepared,
                  &qxp_elig_backings) == FALSE,
              "a surface-backed stream-0 backing never activates capture");

    /* Any multi-stream requirement, in either snapshot, stops capture. */
    memcpy(&qxp_elig_backings, &qxp_backings, sizeof(qxp_elig_backings));
    qxp_elig_backings.required_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, qxp_fixture.prepared,
                  &qxp_elig_backings) == FALSE,
              "a multi-stream backing requirement never activates capture");
    memcpy(&qxp_elig_backings, &qxp_backings, sizeof(qxp_elig_backings));
    qxp_elig_backings.required_user_memory_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, qxp_fixture.prepared,
                  &qxp_elig_backings) == FALSE,
              "a multi-stream user-memory backing mask never activates");

    memcpy(&qxp_elig_prepared, qxp_fixture.prepared,
           sizeof(qxp_elig_prepared));
    qxp_elig_prepared.required_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, &qxp_elig_prepared,
                  &qxp_backings) == FALSE,
              "a multi-stream prepared requirement never activates capture");
    memcpy(&qxp_elig_prepared, qxp_fixture.prepared,
           sizeof(qxp_elig_prepared));
    qxp_elig_prepared.required_user_memory_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, &qxp_elig_prepared,
                  &qxp_backings) == FALSE,
              "a multi-stream prepared user-memory mask never activates");

    /* populated_stream_mask is part of the exact production activation rule:
     * a required-stream-0 snapshot that reports any other populated stream
     * stops at the B6G2 boundary instead of entering capture. */
    memcpy(&qxp_elig_backings, &qxp_backings, sizeof(qxp_elig_backings));
    qxp_elig_backings.populated_stream_mask = 3UL;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, qxp_fixture.prepared,
                  &qxp_elig_backings) == FALSE,
              "an extra populated stream never activates capture");
    memcpy(&qxp_elig_backings, &qxp_backings, sizeof(qxp_elig_backings));
    qxp_elig_backings.populated_stream_mask = 0;
    qxp_check(QxpD3d9DrawCaptureIsEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE, qxp_fixture.prepared,
                  &qxp_elig_backings) == FALSE,
              "an unpopulated stream-0 backing never activates capture");
}

static void qxp_test_multistream_capture_eligibility(void)
{
    QXP_TEST_POINTS points;

    points.count = 2;
    points.start = 1;
    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), TRUE,
                  4096),
              "mixed multistream eligibility foundation");
    qxp_check(QxpD3d9DrawCaptureStreamsAreEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, TRUE,
                  &qxp_multi_prepared, &qxp_multi_backings) == TRUE &&
                  QxpD3d9DrawCaptureStreamsAreEligible(
                      QXP_D3D9_DP2OP_POINTS, FALSE, FALSE,
                      &qxp_multi_prepared, &qxp_multi_backings) == FALSE,
              "mixed UM/system streams require the observed UM call source");

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), (1UL << 0) | (1UL << 7), FALSE,
                  4096),
              "all-system multistream eligibility foundation");
    qxp_check(QxpD3d9DrawCaptureStreamsAreEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, FALSE,
                  &qxp_multi_prepared, &qxp_multi_backings) == TRUE &&
                  QxpD3d9DrawCaptureStreamsAreEligible(
                      QXP_D3D9_DP2OP_POINTS, FALSE, TRUE,
                      &qxp_multi_prepared, &qxp_multi_backings) == TRUE,
              "all-system streams ignore an unused UM call block");

    qxp_check(qxp_multi_build(
                  QXP_D3D9_DP2OP_POINTS, 1, &points,
                  (ULONG)sizeof(points), 1UL << 7, FALSE, 4096),
              "stream-seven-only eligibility foundation");
    qxp_check(QxpD3d9DrawCaptureStreamsAreEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, FALSE,
                  &qxp_multi_prepared, &qxp_multi_backings) == TRUE,
              "a nonzero required stream does not require stream zero");
    qxp_check(QxpD3d9DrawCaptureStreamsAreEligible(
                  QXP_D3D9_DP2OP_POINTS, TRUE, FALSE,
                  &qxp_multi_prepared, &qxp_multi_backings) == FALSE &&
                  QxpD3d9DrawCaptureStreamsAreEligible(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE, FALSE, FALSE,
                      &qxp_multi_prepared, &qxp_multi_backings) == FALSE &&
                  QxpD3d9DrawCaptureStreamsAreEligible(
                      QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2,
                      FALSE, FALSE, &qxp_multi_prepared,
                      &qxp_multi_backings) == FALSE,
              "indexed flag and indexed opcodes independently refuse");
    qxp_multi_backings.streams[7].kind =
        QXP_D3D9_DRAW_BACKING_KIND_VIDEO_LINEAR;
    qxp_check(QxpD3d9DrawCaptureStreamsAreEligible(
                  QXP_D3D9_DP2OP_POINTS, FALSE, FALSE,
                  &qxp_multi_prepared, &qxp_multi_backings) == TRUE,
              "video-linear streams are capture-eligible");
}

/* Allocation failure is the only resource failure, and the appended checked
 * record range is rejected before any allocation, secure, or copy. */
static void qxp_test_failure_classification(void)
{
    QXP_D3D9_DRAW_CAPTURE_STATUS status;
    QXP_TEST_DRAW draw;
    void *blob;
    DWORD bytes;
    DWORD code;
    DWORD alloc_before;
    DWORD release_before;
    int resource_classes_exact;
    BOOL result;

    resource_classes_exact = 1;
    for (code = 0;
         code <= (DWORD)QXP_D3D9_DRAW_CAPTURE_ERROR_TRANSPORT;
         ++code) {
        BOOL expected;

        memset(&status, 0, sizeof(status));
        status.code = code;
        expected =
            code == (DWORD)QXP_D3D9_DRAW_CAPTURE_ERROR_ALLOCATION ||
            code == (DWORD)QXP_D3D9_DRAW_CAPTURE_ERROR_TRANSPORT;
        if (QxpD3d9CaptureStatusIsResourceFailure(&status) != expected)
            resource_classes_exact = 0;
    }
    qxp_check(resource_classes_exact &&
                  QxpD3d9CaptureStatusIsResourceFailure(NULL) == FALSE,
              "allocation and transport are the exact resource failures");

    memset(&status, 0, sizeof(status));
    status.code = QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_RECORDS;
    qxp_check(QxpD3d9CaptureStatusIsResourceFailure(&status) == FALSE,
              "a record-range secure failure is not a resource failure");
    status.code = QXP_D3D9_DRAW_CAPTURE_ERROR_SECURE_USER_MEMORY;
    qxp_check(QxpD3d9CaptureStatusIsResourceFailure(&status) == FALSE,
              "a user-memory secure failure is not a resource failure");

    draw.primitive_type = QXP_D3D9_PRIMITIVE_LINELIST;
    draw.vertex_start = 1;
    draw.primitive_count = 2;
    qxp_check(qxp_syn_build(
                  QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1, &draw,
                  (ULONG)sizeof(draw), 16, 12, 4096, TRUE, FALSE, 0, 0, 0),
              "record-range foundation");

    alloc_before = qxp_cap_alloc;
    release_before = qxp_cap_release;
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_recorder_reset();
    result = QxpD3d9CaptureNonindexedDraw(
        &qxp_syn_envelope, QXP_D3D9_DP2OP_DRAWPRIMITIVE, 1,
        (const void *)(ULONG_PTR)0xfffffff8UL,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE, &qxp_syn_prepared,
        &qxp_syn_backings, qxp_syn_buffer, 4096, &qxp_cap_allocator,
        qxp_secure, qxp_unsecure, qxp_copy, &qxp_recorder, &blob, &bytes,
        &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status,
        QXP_D3D9_DRAW_CAPTURE_ERROR_RECORD_RANGE, FALSE,
        "a wrapping record range is a semantic rejection");
    qxp_check(status.field == QXP_D3D9_DRAW_CAPTURE_FIELD_RECORD_RANGE &&
                  qxp_recorder.secure_count == 0 &&
                  qxp_recorder.unsecure_count == 0 &&
                  qxp_recorder.copy_count == 0 &&
                  qxp_cap_alloc == alloc_before &&
                  qxp_cap_release == release_before,
              "record-range rejection allocates, secures and copies nothing");

    /* The record-range guard never pre-empts the canonical envelope-first
     * classification of an ineligible opcode or an unrepresentable count. */
    blob = (void *)0xdeadu;
    bytes = 0xdeadu;
    qxp_recorder_reset();
    result = qxp_syn_capture(0xFEU, 1, &draw, (ULONG)sizeof(draw),
                             qxp_syn_buffer, 4096, &blob, &bytes, &status);
    qxp_expect_capture_failure(
        result, blob, bytes, &status, QXP_D3D9_DRAW_CAPTURE_ERROR_ENVELOPE,
        FALSE, "an ineligible opcode still classifies through B6G2");
}

int main(void)
{
    DWORD index;
    DWORD stream;

    memset(&qxp_plain_allocator, 0, sizeof(qxp_plain_allocator));
    qxp_plain_allocator.allocate = qxp_plain_allocate;
    qxp_plain_allocator.release = qxp_plain_free;
    memset(&qxp_cap_allocator, 0, sizeof(qxp_cap_allocator));
    qxp_cap_allocator.allocate = qxp_cap_allocate;
    qxp_cap_allocator.release = qxp_cap_free;

    qxp_um_base = qxp_um_buffer + QXP_REAL_PREFIX;
    memset(qxp_um_buffer, QXP_REAL_GUARD_BYTE, sizeof(qxp_um_buffer));
    for (index = 0; index < QXP_REAL_UM_BYTES; ++index)
        qxp_um_base[index] = (BYTE)(index * 7U + 0x11U);
    for (index = 0; index < (DWORD)sizeof(qxp_syn_buffer); ++index)
        qxp_syn_buffer[index] = (BYTE)(index * 3U + 0x21U);
    for (stream = 0; stream < QXP_D3D9_DRAW_BACKING_STREAM_COUNT; ++stream) {
        for (index = 0; index < 4096UL; ++index) {
            qxp_multi_buffer[stream][index] =
                (BYTE)(stream * 29UL + index * 7UL + 0x31UL);
        }
    }

    qxp_check(qxp_fixture_initialize(), "initialize real SM2 fixture");
    if (qxp_fixture.prepared) {
        qxp_test_four_opcodes();
        qxp_test_independence_and_no_pointers();
        qxp_test_blob_mutations();
        qxp_test_surface_capture();
        qxp_test_circular_surface_capture();
        qxp_test_stream_offset_capture();
        qxp_test_sparse_multistream();
        qxp_test_legacy_multistream_prepared_capture();
        qxp_test_captured_execution_plan();
        qxp_test_single_stream_byte_phase_execution();
        qxp_test_stream1_only_execution();
        qxp_test_all_sixteen_streams_and_failures();
        qxp_test_stream15_boundary();
        qxp_test_aggregate_capacity();
        qxp_test_cross_identity_contracts();
        qxp_test_surface_eligibility();
        qxp_test_surface_extent_arithmetic();
        qxp_test_callback_failures();
        qxp_test_null_arguments();
        qxp_test_semantic_rejects();
        qxp_test_indexed_opcodes();
        qxp_test_capacity();
        qxp_test_user_memory_envelope_helper();
        qxp_test_capture_eligibility_helper();
        qxp_test_multistream_capture_eligibility();
        qxp_test_failure_classification();
    }
    qxp_fixture_destroy();

    qxp_check(qxp_lock_depth == 0 && qxp_lock_acquires == qxp_lock_releases,
              "fixture locks balance");
    qxp_check(qxp_cap_live == 0 && qxp_cap_alloc == qxp_cap_release &&
                  qxp_cap_canary_fail == 0 && qxp_cap_lock_violation == 0,
              "capture allocations balance with intact canaries");
    qxp_check(qxp_plain_live == 0 && qxp_plain_alloc == qxp_plain_release,
              "fixture allocations balance");
    qxp_check(qxp_recorder.lock_violation == 0,
              "no callback ever observed a held lock");

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw capture tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw capture tests: PASS\n");
    return 0;
}
