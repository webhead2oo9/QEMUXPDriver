#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxpdisp/qxp_d3d9_draw_gate.h"

#define QXP_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_PARAMETER 0x80000000UL
#define QXP_GUARD_A 0x13579bdfUL
#define QXP_GUARD_B 0x2468ace0UL
#define QXP_TAIL_BYTE 0x5aU

#define QXP_INST(op, length) \
    ((DWORD)(op) | ((DWORD)(length) << D3DSI_INSTLENGTH_SHIFT))
#define QXP_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_DST(type, number, mask, modifier) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     ((DWORD)(mask) << 16) | (DWORD)(modifier))
#define QXP_SRC(type, number) \
    (QXP_PARAMETER | QXP_REGTYPE(type) | (DWORD)(number) | \
     D3DSP_NOSWIZZLE | D3DSPSM_NONE)
#define QXP_DCL_USAGE(usage, index) \
    (QXP_PARAMETER | ((DWORD)(usage) << D3DSP_DCL_USAGE_SHIFT) | \
     ((DWORD)(index) << D3DSP_DCL_USAGEINDEX_SHIFT))

typedef struct qxp_allocation_header {
    DWORD guard_a;
    DWORD guard_b;
    ULONG size;
    DWORD identifier;
} QXP_ALLOCATION_HEADER;

typedef struct qxp_allocator_state {
    DWORD calls;
    DWORD releases;
    DWORD live;
    DWORD fail_at;
    DWORD next_identifier;
    DWORD canary_failures;
    DWORD allocations_while_locked;
    DWORD releases_while_locked;
} QXP_ALLOCATOR_STATE;

typedef struct qxp_lock_state {
    DWORD acquires;
    DWORD releases;
    DWORD depth;
    DWORD maximum_depth;
    DWORD violations;
    DWORD mutate_at_acquire;
    QXP_D3D9_DRAW_STATE *mutate_state;
} QXP_LOCK_STATE;

typedef struct qxp_fixture {
    QXP_SHADER_OWNER owner;
    QXP_SHADER_OWNER other_owner;
    DWORD device_identity;
    DWORD other_device_identity;
    QXP_D3D9_SHADER_OBJECT_TABLE table;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel_shader;
} QXP_FIXTURE;

typedef struct qxp_guarded_prepare_status {
    DWORD before[8];
    QXP_D3D9_DRAW_PREPARE_STATUS value;
    DWORD after[8];
} QXP_GUARDED_PREPARE_STATUS;

static QXP_ALLOCATOR_STATE qxp_alloc_state;
static QXP_LOCK_STATE qxp_lock_state;
static QXP_D3D9_SHADER_ALLOCATOR qxp_allocator;
static QXP_FIXTURE qxp_fixture;
static QXP_GUARDED_PREPARE_STATUS qxp_guarded_status;
static QXP_D3D9_DRAW_PREPARE_STATUS qxp_status;
static QXP_D3D9_SHADER_OBJECT_STATUS qxp_object_status;
static int qxp_failures;

static const QXP_VERTEX_ELEMENT9 qxp_programmable_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_POSITION, 0 },
    { 0, 12, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_TEXCOORD, 0 },
    { 1, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_COLOR, 1 }
};

static const QXP_VERTEX_ELEMENT9 qxp_positiont_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_POSITIONT, 0 }
};

/* The layout Direct3D produces for D3DFVF_XYZRHW | D3DFVF_PSIZE |
 * D3DFVF_DIFFUSE | D3DFVF_TEX1 - the shape a real title draws screen-space
 * geometry with, and the one that carries an element no stage consumes. */
static const QXP_VERTEX_ELEMENT9 qxp_positiont_psize_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT4,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_POSITIONT, 0 },
    { 0, 16, QXP_VERTEX_DECLTYPE9_FLOAT1,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_PSIZE, 0 },
    { 0, 20, QXP_VERTEX_DECLTYPE9_D3DCOLOR,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_COLOR, 0 },
    { 0, 24, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_TEXCOORD, 0 }
};

static const DWORD qxp_interface_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_TEXCOORD, 1),
        QXP_DST(D3DSPR_INPUT, 3, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 2, 15, D3DSPDM_NONE),
        0x3f800000UL, 0x40000000UL, 0x40400000UL, 0x40800000UL,
    QXP_INST(D3DSIO_DEFI, 5),
        QXP_DST(D3DSPR_CONSTINT, 1, 15, D3DSPDM_NONE),
        11UL, 12UL, 13UL, 14UL,
    QXP_INST(D3DSIO_DEFB, 2),
        QXP_DST(D3DSPR_CONSTBOOL, 2, 15, D3DSPDM_NONE),
        1UL,
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEXCRDOUT, 0, 3, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_missing_output_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2),
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

/* vs_1_1 form of qxp_missing_output_vs in real SM1 encoding: every
 * instruction token has a zero length field, which the decoder derives
 * from the opcode schema. */
static const DWORD qxp_interface_vs11[] = {
    QXP_SHADER_VERSION_VS_1_1,
    D3DSIO_DCL,
        QXP_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_MOV,
        QXP_DST(D3DSPR_ATTROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_interface_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_INST(D3DSIO_DCL, 2), QXP_PARAMETER,
        QXP_DST(D3DSPR_TEXTURE, 0, 3, D3DSPDM_MSAMPCENTROID),
    QXP_INST(D3DSIO_DEF, 5),
        QXP_DST(D3DSPR_CONST, 1, 15, D3DSPDM_NONE),
        0x41000000UL, 0x41100000UL, 0x41200000UL, 0x41300000UL,
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_TEMP, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_INPUT, 0),
    QXP_INST(D3DSIO_MOV, 2),
        QXP_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
        QXP_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD qxp_a8_ps11[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_TEX,
        0xb00f0000UL,
    D3DSIO_MUL,
        0x800f0000UL,
        0xb0e40000UL,
        0x90e40000UL,
    D3DSIO_END
};

static void qxp_check(int condition, const char *name)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_draw_prepare: failed: %s\n", name);
}

static void *qxp_allocate(ULONG byte_count, void *opaque)
{
    QXP_ALLOCATOR_STATE *state;
    QXP_ALLOCATION_HEADER *header;
    BYTE *payload;
    BYTE *tail;
    size_t total;
    DWORD index;

    state = (QXP_ALLOCATOR_STATE *)opaque;
    ++state->calls;
    if (qxp_lock_state.depth) ++state->allocations_while_locked;
    if (state->fail_at && state->calls == state->fail_at) return NULL;
    total = sizeof(*header) + (size_t)byte_count + 16U;
    header = (QXP_ALLOCATION_HEADER *)malloc(total);
    if (!header) return NULL;
    header->guard_a = QXP_GUARD_A;
    header->guard_b = QXP_GUARD_B;
    header->size = byte_count;
    header->identifier = ++state->next_identifier;
    payload = (BYTE *)(header + 1);
    memset(payload, 0xa5, byte_count);
    tail = payload + byte_count;
    for (index = 0; index < 16; ++index) tail[index] = QXP_TAIL_BYTE;
    ++state->live;
    return payload;
}

static void qxp_release(void *allocation, void *opaque)
{
    QXP_ALLOCATOR_STATE *state;
    QXP_ALLOCATION_HEADER *header;
    BYTE *tail;
    DWORD index;

    state = (QXP_ALLOCATOR_STATE *)opaque;
    if (qxp_lock_state.depth) ++state->releases_while_locked;
    if (!allocation) {
        ++state->canary_failures;
        return;
    }
    header = ((QXP_ALLOCATION_HEADER *)allocation) - 1;
    if (header->guard_a != QXP_GUARD_A ||
        header->guard_b != QXP_GUARD_B) ++state->canary_failures;
    tail = (BYTE *)allocation + header->size;
    for (index = 0; index < 16; ++index) {
        if (tail[index] != QXP_TAIL_BYTE) ++state->canary_failures;
    }
    if (!state->live) {
        ++state->canary_failures;
    } else {
        --state->live;
    }
    ++state->releases;
    memset(allocation, 0xdd, header->size);
    free(header);
}

static void qxp_lock(void *opaque)
{
    QXP_LOCK_STATE *state;

    state = (QXP_LOCK_STATE *)opaque;
    ++state->acquires;
    if (state->depth) ++state->violations;
    ++state->depth;
    if (state->depth > state->maximum_depth)
        state->maximum_depth = state->depth;
    if (state->mutate_at_acquire == state->acquires &&
        state->mutate_state) {
        state->mutate_state->vs_boolean[0] ^= 1UL;
    }
}

static void qxp_unlock(void *opaque)
{
    QXP_LOCK_STATE *state;

    state = (QXP_LOCK_STATE *)opaque;
    ++state->releases;
    if (state->depth != 1) {
        ++state->violations;
        if (!state->depth) return;
    }
    --state->depth;
}

static void qxp_set_fail_after(DWORD allocation_offset)
{
    qxp_alloc_state.fail_at = qxp_alloc_state.calls + allocation_offset;
}

static void qxp_clear_failure(void)
{
    qxp_alloc_state.fail_at = 0;
}

static int qxp_owner_equal(
    const QXP_D3D9_DRAW_OWNER_IDENTITY *left,
    const QXP_SHADER_OWNER *right)
{
    return left->context_handle == right->context_handle &&
           left->process_id == right->process_id &&
           left->device_identity == (ULONG_PTR)right->device;
}

static int qxp_binding_equal(
    const QXP_D3D9_SHADER_BINDING *left,
    const QXP_D3D9_SHADER_BINDING *right)
{
    return left->handle == right->handle &&
           left->generation == right->generation;
}

static void qxp_prepare_status_guard(void)
{
    DWORD index;

    memset(&qxp_guarded_status, 0xcc, sizeof(qxp_guarded_status));
    for (index = 0; index < 8; ++index) {
        qxp_guarded_status.before[index] = QXP_GUARD_A + index;
        qxp_guarded_status.after[index] = QXP_GUARD_B - index;
    }
}

static int qxp_status_guard_ok(void)
{
    DWORD index;

    for (index = 0; index < 8; ++index) {
        if (qxp_guarded_status.before[index] != QXP_GUARD_A + index ||
            qxp_guarded_status.after[index] != QXP_GUARD_B - index)
            return 0;
    }
    return 1;
}

static int qxp_publish_declaration(
    QXP_FIXTURE *fixture, DWORD handle,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;
    BOOL result;

    object = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        &fixture->owner, handle, elements, element_count,
        &qxp_allocator, &object, &qxp_object_status);
    if (!result || !object) return 0;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9PublishVertexDeclaration(
        &fixture->table, object, &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    if (!result) {
        QxpD3d9DestroyVertexDeclaration(object, &qxp_allocator);
        return 0;
    }
    fixture->declaration = object;
    return 1;
}

static int qxp_publish_vertex(
    QXP_FIXTURE *fixture, DWORD handle,
    const DWORD *tokens, DWORD token_count)
{
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;
    BOOL result;

    object = NULL;
    result = QxpD3d9PrepareVertexFunction(
        &fixture->owner, handle, tokens,
        token_count * (DWORD)sizeof(DWORD), &qxp_allocator,
        &object, &qxp_object_status);
    if (!result || !object) return 0;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9PublishVertexFunction(
        &fixture->table, object, &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    if (!result) {
        QxpD3d9DestroyVertexFunction(object, &qxp_allocator);
        return 0;
    }
    fixture->vertex = object;
    return 1;
}

static int qxp_publish_pixel(
    QXP_FIXTURE *fixture, DWORD handle,
    const DWORD *tokens, DWORD token_count)
{
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;
    BOOL result;

    object = NULL;
    result = QxpD3d9PreparePixelFunction(
        &fixture->owner, handle, tokens,
        token_count * (DWORD)sizeof(DWORD), &qxp_allocator,
        &object, &qxp_object_status);
    if (!result || !object) return 0;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9PublishPixelFunctionUnified(
        &fixture->table, object, FALSE, &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    if (!result) {
        QxpD3d9DestroyPixelFunction(object, &qxp_allocator);
        return 0;
    }
    fixture->pixel_shader = object;
    return 1;
}

static int qxp_bind_fixture(QXP_FIXTURE *fixture)
{
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING vertex_binding;
    QXP_D3D9_PIXEL_FUNCTION_ROUTE route;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    DWORD legacy_fvf;
    BOOL result;

    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&vertex_binding, 0, sizeof(vertex_binding));
    legacy_fvf = 0;
    route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9SetVertexDeclaration(
        &fixture->table, &fixture->owner, fixture->declaration->handle,
        &declaration_binding, &legacy_fvf, &qxp_object_status);
    if (result) {
        result = QxpD3d9SetVertexFunction(
            &fixture->table, &fixture->owner, fixture->vertex->handle,
            &vertex_binding, &qxp_object_status);
    }
    if (result) {
        result = QxpD3d9SetPixelFunctionUnified(
            &fixture->table, &fixture->owner,
            fixture->pixel_shader->handle, FALSE, &fixture->pixel,
            &route, &qxp_object_status);
    }
    qxp_unlock(&qxp_lock_state);
    if (!result || route != QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2) return 0;
    if (!QxpD3d9DrawStateSetDeclaration(
            &fixture->draw, QXP_D3D9_DRAW_DECLARATION_OBJECT,
            &declaration_binding, legacy_fvf, &draw_status)) return 0;
    if (!QxpD3d9DrawStateSetVertexFunction(
            &fixture->draw, &vertex_binding, &draw_status)) return 0;
    if (!QxpD3d9DrawStateSetStreamSource(
            &fixture->draw, 0, 0x900UL, 0, 20, FALSE,
            &draw_status)) return 0;
    return 1;
}

static int qxp_fixture_initialize(
    QXP_FIXTURE *fixture,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    const DWORD *vertex_tokens, DWORD vertex_token_count)
{
    memset(fixture, 0, sizeof(*fixture));
    qxp_shader_owner_initialize(
        &fixture->owner, 0x41UL, 0x51UL, &fixture->device_identity);
    qxp_shader_owner_initialize(
        &fixture->other_owner, 0x42UL, 0x52UL,
        &fixture->other_device_identity);
    QxpD3d9ShaderObjectTableInitialize(&fixture->table);
    QxpD3d9DrawStateInitialize(&fixture->draw);
    QxpD3d9ClearPixelBindingState(&fixture->pixel);
    if (!qxp_publish_declaration(
            fixture, 0x101UL, elements, element_count)) return 0;
    if (!qxp_publish_vertex(
            fixture, 0x201UL, vertex_tokens,
            vertex_token_count)) return 0;
    if (!qxp_publish_pixel(
            fixture, 0x301UL, qxp_interface_ps,
            QXP_COUNT(qxp_interface_ps))) return 0;
    return qxp_bind_fixture(fixture);
}

static void qxp_fixture_destroy(QXP_FIXTURE *fixture)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel;

    for (;;) {
        qxp_lock(&qxp_lock_state);
        declaration = QxpD3d9DetachOwnerVertexDeclaration(
            &fixture->table, &fixture->owner);
        qxp_unlock(&qxp_lock_state);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);
    }
    for (;;) {
        qxp_lock(&qxp_lock_state);
        vertex = QxpD3d9DetachOwnerVertexFunction(
            &fixture->table, &fixture->owner);
        qxp_unlock(&qxp_lock_state);
        if (!vertex) break;
        QxpD3d9DestroyVertexFunction(vertex, &qxp_allocator);
    }
    for (;;) {
        qxp_lock(&qxp_lock_state);
        pixel = QxpD3d9DetachOwnerPixelFunction(
            &fixture->table, &fixture->owner);
        qxp_unlock(&qxp_lock_state);
        if (!pixel) break;
        QxpD3d9DestroyPixelFunction(pixel, &qxp_allocator);
    }
    for (;;) {
        qxp_lock(&qxp_lock_state);
        declaration = QxpD3d9DetachOwnerVertexDeclaration(
            &fixture->table, &fixture->other_owner);
        qxp_unlock(&qxp_lock_state);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static QXP_D3D9_DRAW_PREPARED *qxp_prepare(
    QXP_FIXTURE *fixture, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    return QxpD3d9PrepareDrawSnapshot(
        owner, draw, pixel, NULL, &fixture->table, &qxp_allocator,
        qxp_lock, qxp_unlock, &qxp_lock_state, NULL, status);
}

static QXP_D3D9_DRAW_GATE_RESULT qxp_gate(
    QXP_FIXTURE *fixture, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    return QxpD3d9GateDrawExecution(owner, draw, pixel, NULL, &fixture->table,
        &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2, NULL, NULL, NULL, status);
}

static int qxp_validate(
    QXP_FIXTURE *fixture, const QXP_D3D9_DRAW_PREPARED *prepared,
    const QXP_D3D9_DRAW_STATE *draw,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel,
    QXP_D3D9_DRAW_PREPARE_STATUS *status)
{
    return QxpD3d9ValidatePreparedDraw(
        prepared, &fixture->owner, draw, pixel, NULL, &fixture->table,
        &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state,
        status) ? 1 : 0;
}

static void qxp_expect_prepared_validation_failure(
    QXP_FIXTURE *fixture, const QXP_D3D9_DRAW_PREPARED *prepared,
    DWORD expected_code, const char *name)
{
    qxp_check(!qxp_validate(
                  fixture, prepared, &fixture->draw,
                  &fixture->pixel, &qxp_status) &&
                  qxp_status.code == expected_code,
              name);
}

static void qxp_expect_prepare_failure(
    QXP_FIXTURE *fixture, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_DRAW_STATE *draw,
    const QXP_D3D9_PIXEL_BINDING_STATE *pixel,
    DWORD expected_code, const char *name)
{
    QXP_D3D9_DRAW_PREPARED *prepared;

    memset(&qxp_status, 0xa5, sizeof(qxp_status));
    prepared = qxp_prepare(fixture, owner, draw, pixel, &qxp_status);
    qxp_check(prepared == NULL, name);
    qxp_check(qxp_status.code == expected_code, name);
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
}

static void qxp_test_happy_immutable(void)
{
    QXP_D3D9_DRAW_PREPARED *first;
    QXP_D3D9_DRAW_PREPARED *second;
    QXP_D3D9_DRAW_PREPARED *saved;
    QXP_D3D9_DRAW_STATE original_draw;
    QXP_D3D9_PIXEL_BINDING_STATE original_pixel;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    DWORD raw_vs_float[4];
    DWORD raw_vs_integer[4];
    DWORD raw_vs_boolean;
    DWORD raw_ps_float[4];
    DWORD index;
    DWORD calls_before;
    DWORD releases_before;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize happy fixture");
    raw_vs_float[0] = 0xdead0001UL;
    raw_vs_float[1] = 0xdead0002UL;
    raw_vs_float[2] = 0xdead0003UL;
    raw_vs_float[3] = 0xdead0004UL;
    raw_vs_integer[0] = 21UL;
    raw_vs_integer[1] = 22UL;
    raw_vs_integer[2] = 23UL;
    raw_vs_integer[3] = 24UL;
    raw_vs_boolean = 0UL;
    raw_ps_float[0] = 0xbeef0001UL;
    raw_ps_float[1] = 0xbeef0002UL;
    raw_ps_float[2] = 0xbeef0003UL;
    raw_ps_float[3] = 0xbeef0004UL;
    qxp_check(QxpD3d9DrawStateSetConstants(
                  &qxp_fixture.draw, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
                  QXP_D3D9_DRAW_CONSTANT_FLOAT, 2, 1, raw_vs_float,
                  &draw_status),
              "set raw VS float constant");
    qxp_check(QxpD3d9DrawStateSetConstants(
                  &qxp_fixture.draw, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
                  QXP_D3D9_DRAW_CONSTANT_INTEGER, 1, 1,
                  raw_vs_integer, &draw_status),
              "set raw VS integer constant");
    qxp_check(QxpD3d9DrawStateSetConstants(
                  &qxp_fixture.draw, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
                  QXP_D3D9_DRAW_CONSTANT_BOOLEAN, 2, 1,
                  &raw_vs_boolean, &draw_status),
              "set raw VS boolean constant");
    qxp_check(QxpD3d9DrawStateSetConstants(
                  &qxp_fixture.draw, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
                  QXP_D3D9_DRAW_CONSTANT_FLOAT, 1, 1, raw_ps_float,
                  &draw_status),
              "set raw PS float constant");
    original_draw = qxp_fixture.draw;
    original_pixel = qxp_fixture.pixel;

    qxp_prepare_status_guard();
    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    first = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_guarded_status.value);
    qxp_check(first != NULL && first->valid &&
                  first->mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE,
              "happy preparation succeeds");
    qxp_check(qxp_guarded_status.value.code ==
                  QXP_D3D9_DRAW_PREPARE_OK && qxp_status_guard_ok(),
              "happy status zero and guarded");
    qxp_check(qxp_alloc_state.calls == calls_before + 3 &&
                  qxp_alloc_state.releases == releases_before + 2,
              "prepare exact three allocations and two temporary frees");
    if (!first) {
        qxp_fixture_destroy(&qxp_fixture);
        return;
    }

    qxp_check(qxp_owner_equal(&first->owner, &qxp_fixture.owner) &&
                  qxp_binding_equal(
                      &first->declaration_binding,
                      &qxp_fixture.draw.declaration_binding) &&
                  qxp_binding_equal(
                      &first->vertex_shader_binding,
                      &qxp_fixture.draw.vertex_function_binding) &&
                  qxp_binding_equal(
                      &first->pixel_shader_binding,
                      &qxp_fixture.pixel.sm2),
              "exact owner and three bindings retained");
    qxp_check(first->declaration.valid &&
                  first->vertex_shader_summary.version_token ==
                      QXP_SHADER_VERSION_VS_2_0 &&
                  first->pixel_shader_summary.version_token ==
                      QXP_SHADER_VERSION_PS_2_0,
              "parsed declaration and exact decode summaries retained");
    qxp_check(first->vertex_link.undefined_input_mask == (1UL << 3) &&
                  first->undefined_vertex_input_mask == (1UL << 3),
              "undefined vertex input mask retained");
    for (index = 0; index < 16; ++index) {
        qxp_check(first->undefined_vertex_input_defaults[index][0] == 0 &&
                      first->undefined_vertex_input_defaults[index][1] == 0 &&
                      first->undefined_vertex_input_defaults[index][2] == 0 &&
                      first->undefined_vertex_input_defaults[index][3] == 0,
                  "undefined vertex defaults are raw zero DWORDs");
    }
    qxp_check(first->required_stream_mask == 1UL &&
                  first->required_stream_spans[0] == 12 &&
                  first->required_stream_spans[1] == 0,
              "extra declaration stream and element ignored");
    qxp_check(first->stage_interface.pixel_required_mask == 5UL &&
                  first->stage_interface.vertex_available_mask == 5UL &&
                  first->stage_interface.matched_mask == 5UL &&
                  first->stage_interface.missing_mask == 0 &&
                  first->stage_interface.matched_centroid_mask == 5UL &&
                  first->vertex_metadata.output_centroid_mask == 5UL,
              "matched ten-slot interface and centroid derived");
    qxp_check(first->stage_interface.pixel_required_components[0] == 15 &&
                  first->stage_interface.pixel_required_components[2] == 3 &&
                  first->stage_interface.matched_components[0] == 15 &&
                  first->stage_interface.matched_components[2] == 3,
              "stage interface components exact");

    qxp_check(memcmp(first->raw_state.vs_float[2], raw_vs_float,
                     sizeof(raw_vs_float)) == 0 &&
                  first->effective_constants.vs_float[2][0] ==
                      0x3f800000UL &&
                  first->effective_constants.vs_float[2][1] ==
                      0x40000000UL &&
                  first->effective_constants.vs_float[2][2] ==
                      0x40400000UL &&
                  first->effective_constants.vs_float[2][3] ==
                      0x40800000UL,
              "VS DEF overlays effective float only");
    qxp_check(first->effective_constants.vs_integer[1][0] == 11 &&
                  first->effective_constants.vs_integer[1][3] == 14 &&
                  first->raw_state.vs_integer[1][0] == 21,
              "VS DEFI overlays effective integer only");
    qxp_check(first->effective_constants.vs_boolean[2] == 1 &&
                  first->raw_state.vs_boolean[2] == 0,
              "VS DEFB overlays effective boolean only");
    qxp_check(memcmp(first->raw_state.ps_float[1], raw_ps_float,
                     sizeof(raw_ps_float)) == 0 &&
                  first->effective_constants.ps_float[1][0] ==
                      0x41000000UL &&
                  first->effective_constants.ps_float[1][3] ==
                      0x41300000UL,
              "stored PS DEF metadata overlays effective float only");
    qxp_check(memcmp(&qxp_fixture.draw, &original_draw,
                     sizeof(original_draw)) == 0 &&
                  memcmp(&qxp_fixture.pixel, &original_pixel,
                         sizeof(original_pixel)) == 0,
              "preparation leaves raw inputs unchanged");

    second = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(second != NULL &&
                  memcmp(first, second, sizeof(*first)) == 0,
              "repeat preparation is byte-identical");
    if (second) QxpD3d9DestroyPreparedDraw(second, &qxp_allocator);

    saved = (QXP_D3D9_DRAW_PREPARED *)malloc(sizeof(*saved));
    qxp_check(saved != NULL, "allocate immutable comparison copy");
    if (saved) {
        memcpy(saved, first, sizeof(*saved));
        qxp_fixture.draw.vs_boolean[5] ^= 1UL;
        qxp_fixture.pixel.legacy_handle = 0x77UL;
        qxp_fixture.declaration->declaration.elements[0].offset = 4;
        qxp_check(memcmp(saved, first, sizeof(*saved)) == 0,
                  "prepared result retains no mutable input storage");
        qxp_fixture.draw = original_draw;
        qxp_fixture.pixel = original_pixel;
        qxp_fixture.declaration->declaration.elements[0].offset = 0;
        free(saved);
    }

    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    qxp_check(qxp_validate(
                  &qxp_fixture, first, &qxp_fixture.draw,
                  &qxp_fixture.pixel, &qxp_status) &&
                  qxp_status.code == QXP_D3D9_DRAW_PREPARE_OK,
              "public freshness validation succeeds");
    qxp_check(qxp_alloc_state.calls == calls_before + 3 &&
                  qxp_alloc_state.releases == releases_before + 3,
              "public validation canonical reconstruction balance");

    QxpD3d9DestroyPreparedDraw(first, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);
}

/* The cache may only share the half of a snapshot that is a pure function of
 * the bound program. Per-draw state written between two draws must still
 * reach the reused snapshot, so a cached build has to stay byte-identical to
 * a full rebuild while doing strictly less allocation work. */
static void qxp_test_prepare_cache_reuse(void)
{
    static QXP_D3D9_DRAW_PREPARE_CACHE cache;
    QXP_D3D9_DRAW_PREPARED *cold;
    QXP_D3D9_DRAW_PREPARED *cached;
    QXP_D3D9_DRAW_PREPARED *rebuilt;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    DWORD constant[4];
    DWORD calls_before;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize cache fixture");
    memset(&cache, 0, sizeof(cache));

    cold = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, &cache, &qxp_status);
    qxp_check(cold != NULL && cache.ways[0].valid,
              "cold build succeeds and populates the cache");
    if (!cold) {
        qxp_fixture_destroy(&qxp_fixture);
        return;
    }
    QxpD3d9DestroyPreparedDraw(cold, &qxp_allocator);

    /* Per-draw state only: the program is untouched, so this must hit. */
    constant[0] = 0x51510001UL;
    constant[1] = 0x51510002UL;
    constant[2] = 0x51510003UL;
    constant[3] = 0x51510004UL;
    qxp_check(QxpD3d9DrawStateSetConstants(
                  &qxp_fixture.draw, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
                  QXP_D3D9_DRAW_CONSTANT_FLOAT, 3, 1, constant,
                  &draw_status),
              "write a constant between draws");

    calls_before = qxp_alloc_state.calls;
    cached = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, &cache, &qxp_status);
    qxp_check(cached != NULL && qxp_alloc_state.calls == calls_before + 2,
              "cache hit skips the bytecode and shader-IR allocations");

    rebuilt = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, NULL, &qxp_status);
    qxp_check(cached && rebuilt &&
                  memcmp(cached, rebuilt, sizeof(*cached)) == 0,
              "reused snapshot is byte-identical to a full rebuild");

    if (cached) QxpD3d9DestroyPreparedDraw(cached, &qxp_allocator);
    if (rebuilt) QxpD3d9DestroyPreparedDraw(rebuilt, &qxp_allocator);

    /* Retiring the bound vertex declaration must retire the cache with it. */
    qxp_lock(&qxp_lock_state);
    qxp_fixture.table.generation_clock++;
    qxp_unlock(&qxp_lock_state);
    calls_before = qxp_alloc_state.calls;
    cached = QxpD3d9PrepareDrawSnapshot(
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, &cache, &qxp_status);
    qxp_check(cached != NULL && qxp_alloc_state.calls == calls_before + 3,
              "a table generation bump forces a full rebuild");
    if (cached) QxpD3d9DestroyPreparedDraw(cached, &qxp_allocator);

    qxp_fixture_destroy(&qxp_fixture);
}

/* Titles interleave programs inside one frame, so alternating draws must not
 * evict each other.  Two combiner configurations used A, B, A, B have to hit
 * on every repeat; a single-entry cache would miss all four. */
static void qxp_test_prepare_cache_associativity(void)
{
    static QXP_D3D9_DRAW_PREPARE_CACHE cache;
    static QXP_FF_PIXEL_STATE ff_a;
    static QXP_FF_PIXEL_STATE ff_b;
    QXP_D3D9_DRAW_PREPARED *prepared;
    DWORD calls_before;
    DWORD index;
    int alternating_rebuilds = 0;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize associativity fixture");
    memset(&cache, 0, sizeof(cache));
    memset(&ff_a, 0, sizeof(ff_a));
    memset(&ff_b, 0, sizeof(ff_b));
    ff_b.stages[0].color_op = 1UL;

    /* Warm both programs. */
    for (index = 0; index < 2; ++index) {
        prepared = QxpD3d9PrepareDrawSnapshot(
            &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel,
            index == 0 ? &ff_a : &ff_b, &qxp_fixture.table, &qxp_allocator,
            qxp_lock, qxp_unlock, &qxp_lock_state, &cache, &qxp_status);
        qxp_check(prepared != NULL, "warm an alternating program");
        if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }

    for (index = 0; index < 4; ++index) {
        calls_before = qxp_alloc_state.calls;
        prepared = QxpD3d9PrepareDrawSnapshot(
            &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel,
            (index & 1) ? &ff_b : &ff_a, &qxp_fixture.table, &qxp_allocator,
            qxp_lock, qxp_unlock, &qxp_lock_state, &cache, &qxp_status);
        if (!prepared || qxp_alloc_state.calls != calls_before + 2)
            ++alternating_rebuilds;
        if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }
    qxp_check(alternating_rebuilds == 0,
              "alternating programs stay resident instead of evicting");

    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_vs11_vertex_binding(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements),
                  qxp_interface_vs11, QXP_COUNT(qxp_interface_vs11)),
              "initialize vs_1_1 fixture");
    memset(&qxp_status, 0xa5, sizeof(qxp_status));
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL && prepared->valid &&
              prepared->mode ==
                  QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE,
              "vs_1_1 bound function passes the vertex-binding check");
    if (prepared) {
        qxp_check(prepared->vertex_shader_summary.version_token ==
                      QXP_SHADER_VERSION_VS_1_1 &&
                  prepared->pixel_shader_summary.version_token ==
                      QXP_SHADER_VERSION_PS_2_0 &&
                  qxp_status.code == QXP_D3D9_DRAW_PREPARE_OK,
                  "vs_1_1 summary retained and status zero");
        qxp_check(prepared->vertex_link.valid &&
                  prepared->vertex_link.mode ==
                      QXP_D3D9_VERTEX_LINK_MODE_PROGRAMMABLE &&
                  prepared->vertex_link.input_mask == 1 &&
                  prepared->vertex_link.matched_input_mask == 1 &&
                  prepared->vertex_link.inputs[0].matched &&
                  prepared->vertex_link.inputs[0].usage ==
                      QXP_VERTEX_DECLUSAGE9_POSITION &&
                  prepared->vertex_link.inputs[0].source_stream == 0 &&
                  prepared->vertex_link.inputs[0].source_offset == 0,
                  "vs_1_1 semantic link retained");
        QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }
    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_stream_variants_and_malformed_state(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_DRAW_STATE saved;
    QXP_D3D9_DRAW_STATE_STATUS state_status;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE2 source2;
    ULONG consumed;
    DWORD error_offset;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize stream fixture");
    saved = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateValidate(&saved, &state_status) &&
                  memcmp(&saved, &qxp_fixture.draw, sizeof(saved)) == 0,
              "canonical validator is side-effect-free");

    source2.stream = 0;
    source2.handle = 0x901UL;
    source2.offset = 0;
    source2.stride = 20;
    consumed = 0;
    error_offset = 0;
    draw = saved;
    qxp_check(QxpD3d9ProcessDrawStateBatch(
                  QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 1, &source2,
                  sizeof(source2), &draw, 17, &consumed, &error_offset,
                  &state_status) && draw.streams[0].source2 &&
                  draw.streams[0].offset == 0,
              "SOURCE2 offset zero provenance retained");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL, "SOURCE2 offset zero prepares");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);

    draw = saved;
    qxp_check(QxpD3d9DrawStateSetStreamSource(
                  &draw, 0, 0, 0, 20, TRUE, &state_status),
              "canonical stream-zero UM state");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL &&
                  prepared->required_user_memory_stream_mask == 1UL,
              "required stream-zero user memory prepares");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);

    draw = saved;
    draw.streams[0].stride = 11;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_STRIDE,
        "short required stride rejected");
    draw.streams[0].stride = 0;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_STRIDE,
        "zero required stride rejected");

    draw = saved;
    draw.streams[0].handle = 0;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_SOURCE,
        "missing required stream handle rejected");

    draw = saved;
    draw.streams[0].source2 = TRUE;
    draw.streams[0].offset = 0xfffffff8UL;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_STREAM_OVERFLOW,
        "stream offset plus span overflow rejected");

    draw = saved;
    draw.streams[0].divider = 0;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE,
        "malformed stream divider rejected");
    qxp_check(qxp_status.draw_state_error.code ==
                  QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY,
              "malformed divider nested status");

    draw = saved;
    draw.indices.handle = 0x444UL;
    draw.indices.stride = 3;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE,
        "malformed index state rejected");
    qxp_check(qxp_status.draw_state_error.code ==
                  QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE,
              "malformed index nested status");

    draw = saved;
    draw.streams[0].user_memory = TRUE;
    draw.streams[0].handle = 0x900UL;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE,
        "malformed canonical user-memory state rejected");

    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_positiont_and_interface_missing(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_positiont_elements,
                  QXP_COUNT(qxp_positiont_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize POSITIONT fixture");
    qxp_check(QxpD3d9DrawStateSetStreamSource(
                  &qxp_fixture.draw, 0, 0x900UL, 0, 16, FALSE,
                  &draw_status),
              "POSITIONT stream span");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    /* The bound shader is ignored for a pre-transformed draw, so the stage is
     * synthesized from the declaration: one POSITIONT input, both colour
     * varyings always published, and no texture coordinate because the
     * declaration supplies none.  The fixture's pixel stage reads v0 and t0,
     * so exactly the texcoord stays missing - the colour is satisfied by the
     * unconditional varying, the coordinate genuinely is not there. */
    qxp_check(prepared != NULL && prepared->valid &&
                  prepared->mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS &&
                  prepared->required_stream_mask == 1UL &&
                  prepared->required_stream_spans[0] == 16 &&
                  prepared->positiont_vertex_signature != 0 &&
                  prepared->positiont_vertex.color_mask == 0 &&
                  prepared->positiont_vertex.texcoord_mask == 0 &&
                  prepared->vertex_glsl_source_bytes != 0 &&
                  prepared->vertex_metadata.input_mask == 1UL &&
                  prepared->vertex_metadata.declaration_count == 1UL &&
                  prepared->vertex_metadata.attribute_output_mask == 3UL &&
                  prepared->vertex_metadata.texcoord_output_mask == 0 &&
                  prepared->vertex_metadata.writes_position == TRUE &&
                  prepared->stage_interface.pixel_required_mask == 5UL &&
                  prepared->stage_interface.vertex_available_mask == 3UL &&
                  prepared->stage_interface.matched_mask == 1UL &&
                  prepared->stage_interface.missing_mask == 4UL &&
                  prepared->undefined_vertex_input_mask == 0,
              "POSITIONT bypass synthesizes its stage from the declaration");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements),
                  qxp_missing_output_vs, QXP_COUNT(qxp_missing_output_vs)),
              "initialize missing interface fixture");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL &&
                  prepared->stage_interface.pixel_required_mask == 5UL &&
                  prepared->stage_interface.vertex_available_mask == 1UL &&
                  prepared->stage_interface.matched_mask == 1UL &&
                  prepared->stage_interface.missing_mask == 4UL &&
                  prepared->stage_interface.matched_centroid_mask == 1UL &&
                  prepared->stage_interface.missing_policy[2] ==
                      QXP_D3D9_DRAW_INTERFACE_MISSING_ZERO &&
                  prepared->vertex_metadata.output_centroid_mask == 1UL,
              "missing VS output remains explicit zero-default, not linked");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);
}

/* Fixed-function vertex processing paired with a native pixel shader.  The
 * bound pixel object forces the native route, and that route used to demand
 * a vertex object such a draw never has, so the draw was refused and the
 * runtime lost the device.  The stage is now synthesized from the
 * pre-transformed declaration exactly as the ignored-shader route
 * synthesizes it, with no vertex object of any kind. */
static void qxp_test_fixed_function_vertex_with_pixel_shader(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_SHADER_BINDING zero_binding;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_positiont_elements,
                  QXP_COUNT(qxp_positiont_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize fixed-function vertex fixture");
    memset(&zero_binding, 0, sizeof(zero_binding));
    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetStreamSource(
                  &draw, 0, 0x900UL, 0, 16, FALSE, &draw_status),
              "fixed-function vertex stream span");
    qxp_check(QxpD3d9DrawStateSetVertexFunction(
                  &draw, &zero_binding, &draw_status),
              "unbind the vertex object");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        &qxp_status);
    qxp_check(prepared != NULL && prepared->valid &&
                  prepared->mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS &&
                  prepared->vertex_shader_binding.handle == 0 &&
                  prepared->vertex_shader_binding.generation == 0 &&
                  prepared->vertex_shader_bytecode_size == 0 &&
                  prepared->positiont_vertex_signature != 0 &&
                  prepared->vertex_glsl_source_bytes != 0 &&
                  prepared->vertex_metadata.input_mask == 1UL &&
                  prepared->vertex_metadata.writes_position == TRUE &&
                  prepared->required_stream_mask == 1UL &&
                  prepared->required_stream_spans[0] == 16,
              "fixed-function vertex draw synthesizes its stage");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);

    /* A per-vertex point size is declared by real screen-space layouts and
     * reaches no execution path in this driver, so it is accepted and left
     * ungathered rather than refusing the draw.  Everything the stage does
     * consume is still gathered, so the span covers the whole vertex. */
    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_positiont_psize_elements,
                  QXP_COUNT(qxp_positiont_psize_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize point-size declaration fixture");
    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetStreamSource(
                  &draw, 0, 0x900UL, 0, 32, FALSE, &draw_status),
              "point-size stream span");
    qxp_check(QxpD3d9DrawStateSetVertexFunction(
                  &draw, &zero_binding, &draw_status),
              "unbind the vertex object for the point-size draw");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        &qxp_status);
    qxp_check(prepared != NULL && prepared->valid &&
                  prepared->mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS &&
                  prepared->positiont_vertex.color_mask == 1UL &&
                  prepared->positiont_vertex.texcoord_mask == 1UL &&
                  prepared->positiont_vertex.texcoord_components[0] == 2 &&
                  prepared->vertex_metadata.input_mask == 0xbUL &&
                  prepared->vertex_link.matched_input_count == 3UL &&
                  prepared->required_stream_spans[0] == 32,
              "a declared point size is accepted and left ungathered");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_fvf_vertex_with_ps11(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_SHADER_BINDING zero_binding;
    QXP_D3D9_PIXEL_FUNCTION_ROUTE route;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    BOOL result;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_positiont_psize_elements,
                  QXP_COUNT(qxp_positiont_psize_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize FVF plus ps11 fixture");
    qxp_check(qxp_publish_pixel(
                  &qxp_fixture, 0x302UL, qxp_a8_ps11,
                  QXP_COUNT(qxp_a8_ps11)),
              "publish native A8 ps11 object");
    QxpD3d9ClearPixelBindingState(&pixel);
    route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_fixture.table, &qxp_fixture.owner, 0x302UL, FALSE,
        &pixel, &route, &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2,
              "bind native A8 ps11 object");

    memset(&zero_binding, 0, sizeof(zero_binding));
    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetDeclaration(
                  &draw, QXP_D3D9_DRAW_DECLARATION_FVF,
                  &zero_binding,
                  D3DFVF_XYZRHW | D3DFVF_PSIZE |
                      D3DFVF_DIFFUSE | D3DFVF_TEX1,
                  &draw_status) &&
                  QxpD3d9DrawStateSetVertexFunction(
                      &draw, &zero_binding, &draw_status) &&
                  QxpD3d9DrawStateSetStreamSource(
                      &draw, 0, 0, 0, 32, TRUE, &draw_status),
              "construct r522 FVF draw state");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &draw, &pixel, &qxp_status);
    qxp_check(prepared != NULL && prepared->valid &&
                  prepared->raw_state.declaration_mode ==
                      QXP_D3D9_DRAW_DECLARATION_FVF &&
                  prepared->raw_state.legacy_fvf ==
                      (D3DFVF_XYZRHW | D3DFVF_PSIZE |
                       D3DFVF_DIFFUSE | D3DFVF_TEX1) &&
                  prepared->mode ==
                      QXP_D3D9_DRAW_PREPARED_MODE_POSITIONT_BYPASS &&
                  prepared->declaration_binding.handle == 0 &&
                  prepared->declaration.element_count == 4 &&
                  prepared->positiont_vertex.color_mask == 1UL &&
                  prepared->positiont_vertex.texcoord_mask == 1UL &&
                  prepared->positiont_vertex.texcoord_components[0] == 2 &&
                  prepared->pixel_shader_summary.version_token ==
                      D3DPS_VERSION(1, 1) &&
                  prepared->required_stream_mask == 1UL &&
                  prepared->required_user_memory_stream_mask == 1UL &&
                  prepared->required_stream_spans[0] == 32UL,
              "r522 FVF plus ps11 prepares natively");
    if (prepared) {
        qxp_check(qxp_validate(
                      &qxp_fixture, prepared, &draw, &pixel,
                      &qxp_status),
                  "r522 FVF plus ps11 validates freshly");
        QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }
    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_binding_rejections(void)
{
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_SHADER_BINDING zero_binding;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize binding rejection fixture");
    memset(&zero_binding, 0, sizeof(zero_binding));

    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetDeclaration(
                  &draw, QXP_D3D9_DRAW_DECLARATION_FVF,
                  &zero_binding, D3DFVF_XYZB1, &draw_status),
              "construct unsupported blended FVF state");
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
        "unsupported blended FVF declaration rejected");

    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetDeclaration(
                  &draw, QXP_D3D9_DRAW_DECLARATION_NONE,
                  &zero_binding, 0, &draw_status),
              "construct valid no-declaration state");
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
        "no declaration rejected");

    /* Unbinding the vertex object no longer refuses the draw for want of a
     * binding: the stage is synthesized instead.  This fixture's
     * declaration is programmable, not pre-transformed, so the refusal
     * moves to the declaration that cannot express a fixed-function stage -
     * a more accurate cause, and still fail-closed. */
    draw = qxp_fixture.draw;
    qxp_check(QxpD3d9DrawStateSetVertexFunction(
                  &draw, &zero_binding, &draw_status),
              "construct partial no-VS state");
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_DECLARATION_BINDING,
        "no-VS state with a programmable declaration rejected");

    pixel = qxp_fixture.pixel;
    memset(&pixel.sm2, 0, sizeof(pixel.sm2));
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw, &pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
        "partial no-PS2 state rejected");

    pixel = qxp_fixture.pixel;
    pixel.legacy_handle = 0x66UL;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw, &pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
        "dual legacy and SM2 pixel state rejected");

    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.other_owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT,
        "foreign exact-owner bindings rejected");
    qxp_check(qxp_status.stage ==
                  QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION &&
                  qxp_status.shader_object_error.code ==
                      QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND,
              "foreign binding deterministic nested status");

    draw = qxp_fixture.draw;
    ++draw.vertex_function_binding.generation;
    qxp_expect_prepare_failure(
        &qxp_fixture, &qxp_fixture.owner, &draw, &qxp_fixture.pixel,
        QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT,
        "stale generation binding rejected");
    qxp_check(qxp_status.stage == QXP_D3D9_DRAW_PREPARE_STAGE_VERTEX,
              "stale VS reports exact stage");

    qxp_fixture_destroy(&qxp_fixture);
}

static int qxp_publish_unrelated_declaration(QXP_FIXTURE *fixture)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;
    BOOL result;

    object = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        &fixture->other_owner, 0x111UL, qxp_programmable_elements,
        QXP_COUNT(qxp_programmable_elements), &qxp_allocator, &object,
        &qxp_object_status);
    if (!result || !object) return 0;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9PublishVertexDeclaration(
        &fixture->table, object, &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    if (!result) {
        QxpD3d9DestroyVertexDeclaration(object, &qxp_allocator);
        return 0;
    }
    return 1;
}

static void qxp_test_freshness_generation_and_race(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *detached;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *replacement;
    QXP_D3D9_SHADER_BINDING deletion_binding;
    ULONGLONG clock_before;
    BOOL result;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize freshness fixture");
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL, "prepare freshness baseline");
    clock_before = qxp_fixture.table.generation_clock;
    qxp_check(qxp_publish_unrelated_declaration(&qxp_fixture) &&
                  qxp_fixture.table.generation_clock > clock_before,
              "publish unrelated object advances table generation");
    if (prepared) {
        qxp_check(qxp_validate(
                      &qxp_fixture, prepared, &qxp_fixture.draw,
                      &qxp_fixture.pixel, &qxp_status),
                  "unrelated table generation does not stale prepared draw");
    }

    detached = NULL;
    deletion_binding = qxp_fixture.draw.declaration_binding;
    qxp_lock(&qxp_lock_state);
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_fixture.table, &qxp_fixture.owner,
        qxp_fixture.declaration->handle, &deletion_binding, &detached,
        &qxp_object_status);
    qxp_unlock(&qxp_lock_state);
    qxp_check(result && detached == qxp_fixture.declaration,
              "delete bound declaration through public API");
    replacement = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        &qxp_fixture.owner, 0x101UL, qxp_programmable_elements,
        QXP_COUNT(qxp_programmable_elements), &qxp_allocator,
        &replacement, &qxp_object_status);
    qxp_check(result && replacement != NULL,
              "prepare same-handle replacement declaration");
    if (replacement) {
        qxp_lock(&qxp_lock_state);
        result = QxpD3d9PublishVertexDeclaration(
            &qxp_fixture.table, replacement, &qxp_object_status);
        qxp_unlock(&qxp_lock_state);
        qxp_check(result && replacement->generation !=
                      qxp_fixture.draw.declaration_binding.generation,
                  "recreate same handle gets a new generation");
        if (result) qxp_fixture.declaration = replacement;
    }
    if (prepared) {
        qxp_check(!qxp_validate(
                      &qxp_fixture, prepared, &qxp_fixture.draw,
                      &qxp_fixture.pixel, &qxp_status) &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED &&
                      qxp_status.stage ==
                          QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION,
                  "delete/recreate stales exact-generation prepared draw");
        QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }
    if (detached)
        QxpD3d9DestroyVertexDeclaration(detached, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize final race fixture");
    qxp_lock_state.mutate_state = &qxp_fixture.draw;
    qxp_lock_state.mutate_at_acquire = qxp_lock_state.acquires + 2;
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared == NULL && qxp_status.code ==
                  QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
              "final-validation lock race rejects candidate");
    qxp_lock_state.mutate_at_acquire = 0;
    qxp_lock_state.mutate_state = NULL;

    qxp_fixture.table.generation_clock = ~(ULONGLONG)0;
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL,
              "existing exact-generation objects remain usable at max clock");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_allocation_failures_and_ps_snapshot(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    DWORD failure_index;
    DWORD saved_ps_version;
    DWORD calls_before;
    DWORD releases_before;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize allocation fixture");
    for (failure_index = 1; failure_index <= 3; ++failure_index) {
        calls_before = qxp_alloc_state.calls;
        releases_before = qxp_alloc_state.releases;
        qxp_set_fail_after(failure_index);
        prepared = qxp_prepare(
            &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
            &qxp_fixture.pixel, &qxp_status);
        qxp_check(prepared == NULL &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION &&
                      QxpD3d9DrawPrepareStatusIsResourceFailure(
                          &qxp_status),
                  "each preparation allocation failure is a resource failure");
        qxp_check(qxp_alloc_state.calls == calls_before + failure_index &&
                      qxp_alloc_state.releases ==
                          releases_before + failure_index - 1,
                  "allocation failure rolls back exact successful prefix");
        qxp_clear_failure();
    }

    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL, "prepare for validation allocation failure");
    if (prepared) {
        qxp_set_fail_after(1);
        qxp_check(!qxp_validate(
                      &qxp_fixture, prepared, &qxp_fixture.draw,
                      &qxp_fixture.pixel, &qxp_status) &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION &&
                      QxpD3d9DrawPrepareStatusIsResourceFailure(
                          &qxp_status),
                  "validation scratch allocation failure classified");
        qxp_clear_failure();
        QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }

    saved_ps_version = qxp_fixture.pixel_shader->bytecode[0];
    qxp_fixture.pixel_shader->bytecode[0] = 0;
    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL &&
                  (prepared->pixel_metadata.definition_mask &
                   (1UL << 1)) != 0,
              "preparation uses generation-checked stored PS metadata only");
    if (prepared) QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    qxp_fixture.pixel_shader->bytecode[0] = saved_ps_version;

    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_null_and_prepared_validation(void)
{
    QXP_D3D9_DRAW_PREPARED *prepared;
    QXP_D3D9_DRAW_PREPARED saved;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize validation argument fixture");
    prepared = QxpD3d9PrepareDrawSnapshot(
        NULL, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, NULL, &qxp_status);
    qxp_check(prepared == NULL && qxp_status.code ==
                  QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT &&
                  !QxpD3d9DrawPrepareStatusIsResourceFailure(&qxp_status),
              "null argument deterministic nonresource failure");

    prepared = qxp_prepare(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(prepared != NULL, "prepare for invalid prepared test");
    if (prepared) {
        saved = *prepared;
        prepared->valid = FALSE;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "public validation rejects invalid prepared object");
        *prepared = saved;

        prepared->declaration.elements[0].offset ^= 4;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile declaration payload rejected");
        *prepared = saved;
        prepared->vertex_link.inputs[0].source_offset ^= 4;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile semantic-link payload rejected");
        *prepared = saved;
        prepared->stage_interface.matched_mask ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile stage-interface payload rejected");
        *prepared = saved;
        prepared->required_stream_spans[0] ^= 4UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile stream requirement payload rejected");
        *prepared = saved;
        prepared->undefined_vertex_input_defaults[3][2] ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile undefined-input default rejected");
        *prepared = saved;
        prepared->vertex_metadata.input_mask ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile vertex metadata rejected");
        *prepared = saved;
        prepared->pixel_metadata.input_color_mask ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile pixel metadata rejected");
        *prepared = saved;
        prepared->effective_constants.vs_float[2][0] ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PREPARED_STATE,
            "hostile effective constant rejected");
        *prepared = saved;
        prepared->raw_state.vs_boolean[0] ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED,
            "hostile raw-state payload rejected");
        *prepared = saved;
        prepared->raw_pixel_binding.legacy_handle ^= 1UL;
        qxp_expect_prepared_validation_failure(
            &qxp_fixture, prepared,
            QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING,
            "hostile raw pixel binding rejected");
        *prepared = saved;
        QxpD3d9DestroyPreparedDraw(prepared, &qxp_allocator);
    }
    qxp_fixture_destroy(&qxp_fixture);
}

static void qxp_test_execution_gate(void)
{
    QXP_FIXTURE canonical;
    QXP_D3D9_DRAW_STATE draw;
    QXP_D3D9_PIXEL_BINDING_STATE pixel;
    QXP_D3D9_DRAW_PREPARE_STATUS zero_status;
    QXP_D3D9_DRAW_GATE_RESULT gate_result;
    QXP_SHADER_OWNER owner;
    DWORD device_identity;
    DWORD calls_before;
    DWORD releases_before;
    DWORD live_before;
    DWORD acquires_before;
    DWORD lock_releases_before;
    DWORD locked_allocations_before;
    DWORD locked_releases_before;
    DWORD failure_index;
    DWORD stream_mask_before;

    memset(&canonical, 0, sizeof(canonical));
    memset(&zero_status, 0, sizeof(zero_status));
    device_identity = 0x9191UL;
    qxp_shader_owner_initialize(
        &owner, 0x7171UL, 0x8181UL, &device_identity);
    QxpD3d9ShaderObjectTableInitialize(&canonical.table);
    QxpD3d9DrawStateInitialize(&canonical.draw);
    QxpD3d9ClearPixelBindingState(&canonical.pixel);

    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    acquires_before = qxp_lock_state.acquires;
    lock_releases_before = qxp_lock_state.releases;
    memset(&qxp_status, 0xa5, sizeof(qxp_status));
    gate_result = qxp_gate(
        &canonical, &owner, &canonical.draw, &canonical.pixel,
        &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_LEGACY_ALLOWED &&
                  memcmp(&qxp_status, &zero_status,
                         sizeof(qxp_status)) == 0,
              "canonical state passes execution gate with zero status");
    qxp_check(qxp_alloc_state.calls == calls_before &&
                  qxp_alloc_state.releases == releases_before &&
                  qxp_lock_state.acquires == acquires_before &&
                  qxp_lock_state.releases == lock_releases_before,
              "canonical gate path takes no allocation or lock");

    canonical.pixel.legacy_handle = 0x61UL;
    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    acquires_before = qxp_lock_state.acquires;
    lock_releases_before = qxp_lock_state.releases;
    memset(&qxp_status, 0xa5, sizeof(qxp_status));
    gate_result = qxp_gate(
        &canonical, &owner, &canonical.draw, &canonical.pixel,
        &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_LEGACY_ALLOWED &&
                  memcmp(&qxp_status, &zero_status,
                         sizeof(qxp_status)) == 0 &&
                  qxp_alloc_state.calls == calls_before &&
                  qxp_alloc_state.releases == releases_before &&
                  qxp_lock_state.acquires == acquires_before &&
                  qxp_lock_state.releases == lock_releases_before,
              "ordinary PS1.x binding remains allocation-free legacy");

    canonical.pixel.legacy_handle = 0;
    canonical.pixel.sm2.generation = 9;
    calls_before = qxp_alloc_state.calls;
    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &canonical, &owner, &canonical.draw, &canonical.pixel,
        &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code != QXP_D3D9_DRAW_PREPARE_OK &&
                  qxp_alloc_state.calls == calls_before + 3 &&
                  qxp_alloc_state.live == live_before,
              "generation-only PS2 binding cannot bypass preparation");
    canonical.pixel.sm2.generation = 0;
    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    acquires_before = qxp_lock_state.acquires;
    lock_releases_before = qxp_lock_state.releases;

    gate_result = QxpD3d9GateDrawExecution(&owner, &canonical.draw,
        &canonical.pixel, NULL, NULL, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, QXP_D3D9_DP2OP_DRAWPRIMITIVE2, NULL, NULL,
        NULL,
        &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_NULL_ARGUMENT &&
                  qxp_alloc_state.calls == calls_before &&
                  qxp_alloc_state.releases == releases_before &&
                  qxp_lock_state.acquires == acquires_before &&
                  qxp_lock_state.releases == lock_releases_before,
              "gate validates required arguments before legacy shortcut");

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize execution gate fixture");
    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    live_before = qxp_alloc_state.live;
    acquires_before = qxp_lock_state.acquires;
    lock_releases_before = qxp_lock_state.releases;
    locked_allocations_before =
        qxp_alloc_state.allocations_while_locked;
    locked_releases_before = qxp_alloc_state.releases_while_locked;
    memset(&qxp_status, 0xa5, sizeof(qxp_status));
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  memcmp(&qxp_status, &zero_status,
                         sizeof(qxp_status)) == 0,
              "complete declaration VS2 PS2 prepares then rejects");
    qxp_check(qxp_alloc_state.calls == calls_before + 3 &&
                  qxp_alloc_state.releases == releases_before + 3 &&
                  qxp_alloc_state.live == live_before &&
                  qxp_lock_state.acquires == acquires_before + 2 &&
                  qxp_lock_state.releases == lock_releases_before + 2 &&
                  qxp_alloc_state.allocations_while_locked ==
                      locked_allocations_before &&
                  qxp_alloc_state.releases_while_locked ==
                      locked_releases_before,
              "gate destroys full canonical preparation outside locks");

    for (failure_index = 1; failure_index <= 3; ++failure_index) {
        calls_before = qxp_alloc_state.calls;
        releases_before = qxp_alloc_state.releases;
        live_before = qxp_alloc_state.live;
        acquires_before = qxp_lock_state.acquires;
        lock_releases_before = qxp_lock_state.releases;
        locked_allocations_before =
            qxp_alloc_state.allocations_while_locked;
        locked_releases_before = qxp_alloc_state.releases_while_locked;
        qxp_set_fail_after(failure_index);
        gate_result = qxp_gate(
            &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
            &qxp_fixture.pixel, &qxp_status);
        qxp_check(gate_result ==
                          QXP_D3D9_DRAW_GATE_RESOURCE_FAILURE &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION &&
                      QxpD3d9DrawPrepareStatusIsResourceFailure(
                          &qxp_status),
                  "gate preserves allocation failure classification");
        qxp_check(qxp_alloc_state.calls ==
                          calls_before + failure_index &&
                      qxp_alloc_state.releases ==
                          releases_before + failure_index - 1 &&
                      qxp_alloc_state.live == live_before &&
                      qxp_lock_state.acquires == acquires_before &&
                      qxp_lock_state.releases == lock_releases_before &&
                      qxp_alloc_state.allocations_while_locked ==
                          locked_allocations_before &&
                      qxp_alloc_state.releases_while_locked ==
                          locked_releases_before,
                  "gate allocation rollback has exact counts and no lock");
        qxp_clear_failure();
    }

    draw = qxp_fixture.draw;
    draw.streams[0].divider = 0;
    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.owner, &draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_STATE &&
                  qxp_status.draw_state_error.code ==
                      QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY &&
                  qxp_alloc_state.live == live_before,
              "malformed private draw state keeps detailed gate status");

    pixel = qxp_fixture.pixel;
    memset(&pixel.sm2, 0, sizeof(pixel.sm2));
    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_PIXEL_BINDING &&
                  qxp_alloc_state.live == live_before,
              "incomplete private shader state rejects without leak");

    stream_mask_before =
        qxp_fixture.declaration->declaration.stream_mask;
    qxp_fixture.declaration->declaration.stream_mask ^= 4UL;
    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_SEMANTIC_LINK &&
                  qxp_status.semantic_link_error.code != 0 &&
                  qxp_alloc_state.live == live_before,
              "hostile declaration semantic state remains unsupported");
    qxp_fixture.declaration->declaration.stream_mask =
        stream_mask_before;

    draw = qxp_fixture.draw;
    qxp_lock_state.mutate_state = &draw;
    qxp_lock_state.mutate_at_acquire = qxp_lock_state.acquires + 2;
    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.owner, &draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_STATE_CHANGED &&
                  qxp_alloc_state.live == live_before,
              "gate final freshness race rejects with detailed status");
    qxp_lock_state.mutate_at_acquire = 0;
    qxp_lock_state.mutate_state = NULL;

    live_before = qxp_alloc_state.live;
    gate_result = qxp_gate(
        &qxp_fixture, &qxp_fixture.other_owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_OBJECT_SNAPSHOT &&
                  qxp_status.stage ==
                      QXP_D3D9_DRAW_PREPARE_STAGE_DECLARATION &&
                  qxp_status.shader_object_error.code ==
                      QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
                  qxp_alloc_state.live == live_before,
              "foreign gate owner cannot cross object namespaces");

    qxp_fixture_destroy(&qxp_fixture);
}

typedef struct qxp_command_oracle_counters {
    DWORD host_draws;
    DWORD frame_marks;
    DWORD batch_submissions;
} QXP_COMMAND_ORACLE_COUNTERS;

static void qxp_observe_legacy_command(
    QXP_D3D9_DRAW_COMMAND_GATE_RESULT result,
    DWORD record_count,
    QXP_COMMAND_ORACLE_COUNTERS *counters)
{
    DWORD record;

    if (!counters ||
        result != QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED) return;
    for (record = 0; record < record_count; ++record) {
        ++counters->host_draws;
        ++counters->frame_marks;
        ++counters->batch_submissions;
    }
}

static void qxp_test_production_command_gate(void)
{
    static const BYTE opcodes[6] = {
        QXP_D3D9_DP2OP_POINTS,
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2
    };
    static const DWORD sizes[6] = {
        QXP_D3D9_DRAW_RECORD_BYTES_POINTS,
        QXP_D3D9_DRAW_RECORD_BYTES_CLIPPEDTRIANGLEFAN,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWINDEXEDPRIMITIVE2
    };
    QXP_D3D9_DRAW_STATE legacy_draw;
    QXP_D3D9_DRAW_STATE offset_draw;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_PIXEL_BINDING_STATE legacy_pixel;
    QXP_D3D9_DRAW_COMMAND_GATE_RESULT result;
    QXP_D3D9_DRAW_GATE_RESULT gate_result;
    QXP_COMMAND_ORACLE_COUNTERS counters;
    QXP_D3D9_DRAW_PREPARE_STATUS zero_status;
    DWORD error_offset;
    DWORD calls_before;
    DWORD releases_before;
    DWORD locks_before;
    DWORD unlocks_before;
    DWORD opcode_index;
    DWORD outer_offset;

    memset(&zero_status, 0, sizeof(zero_status));
    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize production command gate fixture");

    for (opcode_index = 0; opcode_index < QXP_COUNT(opcodes);
         ++opcode_index) {
        outer_offset = 0x120UL + opcode_index * 0x20UL;
        qxp_check(QxpD3d9DrawCommandRecordSize(
                      opcodes[opcode_index]) == sizes[opcode_index],
                  "production seam classifies exact draw opcode size");
        calls_before = qxp_alloc_state.calls;
        releases_before = qxp_alloc_state.releases;
        locks_before = qxp_lock_state.acquires;
        unlocks_before = qxp_lock_state.releases;
        error_offset = 0;
        memset(&counters, 0, sizeof(counters));
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
            sizes[opcode_index], outer_offset, &error_offset,
            &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
            &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
            &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 1, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                      error_offset == outer_offset &&
                      memcmp(&qxp_status, &zero_status,
                             sizeof(qxp_status)) == 0 &&
                      qxp_alloc_state.calls == calls_before + 3 &&
                      qxp_alloc_state.releases == releases_before + 3 &&
                      qxp_lock_state.acquires == locks_before + 2 &&
                      qxp_lock_state.releases == unlocks_before + 2 &&
                      counters.host_draws == 0 &&
                      counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "all live draw opcodes prepare before transport rejection");

        calls_before = qxp_alloc_state.calls;
        locks_before = qxp_lock_state.acquires;
        error_offset = 0xdeadUL;
        memset(&counters, 0, sizeof(counters));
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 0, 0,
            outer_offset, &error_offset, &qxp_fixture.owner,
            &qxp_fixture.draw,
            &qxp_fixture.pixel, NULL, &qxp_fixture.table, &qxp_allocator,
            qxp_lock, qxp_unlock, &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 0, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED &&
                      error_offset == 0 &&
                      qxp_alloc_state.calls == calls_before &&
                      qxp_lock_state.acquires == locks_before &&
                      counters.host_draws == 0 &&
                      counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "zero-count draw is allocation-free no-op");

        calls_before = qxp_alloc_state.calls;
        locks_before = qxp_lock_state.acquires;
        error_offset = 0;
        memset(&counters, 0, sizeof(counters));
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 2,
            sizes[opcode_index] * 2 - 1, outer_offset, &error_offset,
            &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
            &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
            &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 2, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_TRUNCATED &&
                      error_offset == outer_offset &&
                      qxp_alloc_state.calls == calls_before &&
                      qxp_lock_state.acquires == locks_before &&
                      counters.host_draws == 0 &&
                      counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "truncated draw frames before gate and transport");
    }

    QxpD3d9DrawStateInitialize(&legacy_draw);
    QxpD3d9ClearPixelBindingState(&legacy_pixel);
    for (opcode_index = 0; opcode_index < QXP_COUNT(opcodes);
         ++opcode_index) {
        calls_before = qxp_alloc_state.calls;
        locks_before = qxp_lock_state.acquires;
        error_offset = 0xdeadUL;
        memset(&counters, 0, sizeof(counters));
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 2,
            sizes[opcode_index] * 2, 0x330UL, &error_offset,
            &qxp_fixture.owner, &legacy_draw, &legacy_pixel, NULL,
            &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
            &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 2, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED &&
                      error_offset == 0 &&
                      qxp_alloc_state.calls == calls_before &&
                      qxp_lock_state.acquires == locks_before &&
                      counters.host_draws == 2 &&
                      counters.frame_marks == 2 &&
                      counters.batch_submissions == 2,
                  "legacy command alone reaches transport and frame seam");
    }

    QxpD3d9DrawStateInitialize(&offset_draw);
    memset(&draw_status, 0, sizeof(draw_status));
    declaration_binding.handle =
        QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT;
    declaration_binding.generation = 1;
    qxp_check(QxpD3d9DrawStateSetDeclaration(
                  &offset_draw, QXP_D3D9_DRAW_DECLARATION_OBJECT,
                  &declaration_binding, 0, &draw_status) &&
                  QxpD3d9DrawStateSetTranslatedDeclaration(
                      &offset_draw, &declaration_binding, 0x102UL, 16,
                      &draw_status) &&
                  QxpD3d9DrawStateSetStreamSource(
                      &offset_draw, 0, 0x222UL, 32, 16, FALSE,
                      &draw_status),
              "construct supported stream-0 offset gate state");
    for (opcode_index = 0; opcode_index < QXP_COUNT(opcodes);
         ++opcode_index) {
        calls_before = qxp_alloc_state.calls;
        locks_before = qxp_lock_state.acquires;
        error_offset = 0xdeadUL;
        memset(&counters, 0, sizeof(counters));
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
            sizes[opcode_index], 0x3a0UL, &error_offset, &qxp_fixture.owner,
            &offset_draw, &legacy_pixel, NULL, &qxp_fixture.table,
            &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state, NULL,
            NULL, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 1, &counters);
        /* Both transport cores resolve a SETSTREAMSOURCE2 binding and apply
         * its byte offset, so every live draw opcode honours a stream-0
         * offset - the indexed forms no longer divert to the native path. */
        qxp_check(
            result == QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED &&
                error_offset == 0 &&
                qxp_alloc_state.calls == calls_before &&
                qxp_lock_state.acquires == locks_before &&
                counters.host_draws == 1 &&
                counters.frame_marks == 1 &&
                counters.batch_submissions == 1,
            "supported stream-0 offset reaches transport for every opcode");
    }

    QxpD3d9DrawStateInitialize(&offset_draw);
    qxp_check(QxpD3d9DrawStateSetStreamSource(
                  &offset_draw, 0, 0x222UL, 0, 16, FALSE,
                  &draw_status),
              "construct declaration-less SOURCE2 provenance state");
    offset_draw.streams[0].source2 = TRUE;
    gate_result = QxpD3d9GateDrawExecution(&qxp_fixture.owner, &offset_draw,
        &legacy_pixel, NULL, &qxp_fixture.table, &qxp_allocator, qxp_lock,
        qxp_unlock, &qxp_lock_state, QXP_D3D9_DP2OP_DRAWPRIMITIVE2, NULL,
        NULL, NULL, &qxp_status);
    qxp_check(gate_result == QXP_D3D9_DRAW_GATE_UNSUPPORTED,
              "declaration-less SOURCE2 provenance remains fail-closed");

    calls_before = qxp_alloc_state.calls;
    locks_before = qxp_lock_state.acquires;
    error_offset = 0xdeadUL;
    memset(&counters, 0, sizeof(counters));
    result = QxpD3d9GateDrawCommand(55, 3, 0, 0x440UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
    qxp_observe_legacy_command(result, 3, &counters);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_NOT_DRAW &&
                  error_offset == 0 &&
                  QxpD3d9DrawCommandRecordSize(55) == 0 &&
                  qxp_alloc_state.calls == calls_before &&
                  qxp_lock_state.acquires == locks_before &&
                  counters.host_draws == 0 &&
                  counters.frame_marks == 0 &&
                  counters.batch_submissions == 0,
              "nondraw command bypasses draw gate and draw counters");

    calls_before = qxp_alloc_state.calls;
    locks_before = qxp_lock_state.acquires;
    error_offset = 0;
    memset(&counters, 0, sizeof(counters));
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2, 0x4c0UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, NULL, NULL, NULL, NULL);
    qxp_observe_legacy_command(result, 1, &counters);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                  error_offset == 0x4c0UL &&
                  qxp_alloc_state.calls == calls_before &&
                  qxp_lock_state.acquires == locks_before &&
                  counters.host_draws == 0 &&
                  counters.frame_marks == 0 &&
                  counters.batch_submissions == 0,
              "recognized null-status failure preserves outer offset");

    calls_before = qxp_alloc_state.calls;
    releases_before = qxp_alloc_state.releases;
    qxp_set_fail_after(1);
    error_offset = 0;
    memset(&counters, 0, sizeof(counters));
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2, 0x550UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, NULL, NULL, NULL, &qxp_status);
    qxp_observe_legacy_command(result, 1, &counters);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE &&
                  error_offset == 0x550UL &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION &&
                  qxp_alloc_state.calls == calls_before + 1 &&
                  qxp_alloc_state.releases == releases_before &&
                  counters.host_draws == 0 &&
                  counters.frame_marks == 0 &&
                  counters.batch_submissions == 0,
              "production seam preserves resource failure before transport");
    qxp_clear_failure();

    qxp_fixture_destroy(&qxp_fixture);
}

typedef struct qxp_inspector_state {
    DWORD calls;
    BYTE last_opcode;
    BOOL last_indexed;
    BOOL saw_valid_prepared;
    BOOL saw_programmable;
    BOOL saw_zero_failure_code;
    DWORD live_at_call;
    DWORD depth_at_call;
    DWORD failure_code_to_set;
    BOOL decline;
} QXP_INSPECTOR_STATE;

static QXP_INSPECTOR_STATE qxp_inspector;

static BOOL qxp_inspect_prepared(
    const QXP_D3D9_DRAW_PREPARED *prepared, BYTE opcode,
    DWORD *failure_code, void *opaque)
{
    QXP_INSPECTOR_STATE *state;

    state = (QXP_INSPECTOR_STATE *)opaque;
    ++state->calls;
    state->last_opcode = opcode;
    state->last_indexed =
        (opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE ||
         opcode == QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2) ? TRUE : FALSE;
    if (prepared && prepared->valid) state->saw_valid_prepared = TRUE;
    if (prepared &&
        prepared->mode == QXP_D3D9_DRAW_PREPARED_MODE_PROGRAMMABLE)
        state->saw_programmable = TRUE;
    state->live_at_call = qxp_alloc_state.live;
    state->depth_at_call = qxp_lock_state.depth;
    if (failure_code && *failure_code == 0)
        state->saw_zero_failure_code = TRUE;
    if (failure_code) *failure_code = state->failure_code_to_set;
    return state->decline ? FALSE : TRUE;
}

static void qxp_test_production_prepared_inspector(void)
{
    static const BYTE opcodes[6] = {
        QXP_D3D9_DP2OP_POINTS,
        QXP_D3D9_DP2OP_CLIPPEDTRIANGLEFAN,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWPRIMITIVE2,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE,
        QXP_D3D9_DP2OP_DRAWINDEXEDPRIMITIVE2
    };
    static const BOOL indexed_expected[6] = {
        FALSE, FALSE, FALSE, FALSE, TRUE, TRUE
    };
    static const DWORD capture_semantic_codes[2] = {
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE,
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ACCESS
    };
    QXP_D3D9_DRAW_STATE legacy_draw;
    QXP_D3D9_PIXEL_BINDING_STATE legacy_pixel;
    QXP_D3D9_DRAW_COMMAND_GATE_RESULT result;
    QXP_COMMAND_ORACLE_COUNTERS counters;
    DWORD error_offset;
    DWORD opcode_index;
    DWORD outer_offset;
    DWORD live_before;
    DWORD calls_before;
    DWORD releases_before;
    DWORD status_index;

    qxp_check(qxp_fixture_initialize(
                  &qxp_fixture, qxp_programmable_elements,
                  QXP_COUNT(qxp_programmable_elements), qxp_interface_vs,
                  QXP_COUNT(qxp_interface_vs)),
              "initialize prepared inspector fixture");
    qxp_check(QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE == 20UL &&
                  QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE ==
                      21UL &&
                  QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ACCESS == 22UL,
              "capture gate status values remain exactly 20, 21, and 22");

    for (opcode_index = 0; opcode_index < QXP_COUNT(opcodes);
         ++opcode_index) {
        outer_offset = 0x600UL + opcode_index * 0x10UL;
        memset(&qxp_inspector, 0, sizeof(qxp_inspector));
        qxp_inspector.decline = TRUE;
        qxp_inspector.failure_code_to_set =
            QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING;
        memset(&counters, 0, sizeof(counters));
        live_before = qxp_alloc_state.live;
        calls_before = qxp_alloc_state.calls;
        releases_before = qxp_alloc_state.releases;
        error_offset = 0;
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
            QxpD3d9DrawCommandRecordSize(opcodes[opcode_index]),
            outer_offset, &error_offset, &qxp_fixture.owner,
            &qxp_fixture.draw, &qxp_fixture.pixel, NULL, &qxp_fixture.table,
            &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state,
            qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 1, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                      error_offset == outer_offset &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING &&
                      !QxpD3d9DrawPrepareStatusIsResourceFailure(
                          &qxp_status),
                  "declining inspector refines status not resource failure");
        qxp_check(qxp_inspector.calls == 1 &&
                      qxp_inspector.last_opcode == opcodes[opcode_index] &&
                      qxp_inspector.last_indexed ==
                          indexed_expected[opcode_index] &&
                      qxp_inspector.saw_valid_prepared &&
                      qxp_inspector.saw_programmable &&
                      qxp_inspector.saw_zero_failure_code,
                  "inspector runs once with initialized failure output");
        qxp_check(qxp_inspector.depth_at_call == 0 &&
                      qxp_inspector.live_at_call == live_before + 1 &&
                      qxp_alloc_state.live == live_before &&
                      qxp_alloc_state.calls == calls_before + 3 &&
                      qxp_alloc_state.releases == releases_before + 3,
                  "inspector runs before exact three-allocation release");
        qxp_check(counters.host_draws == 0 && counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "declined draw reaches no transport or frame mark");

        memset(&qxp_inspector, 0, sizeof(qxp_inspector));
        qxp_inspector.failure_code_to_set =
            QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ENVELOPE;
        memset(&counters, 0, sizeof(counters));
        error_offset = 0;
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
            QxpD3d9DrawCommandRecordSize(opcodes[opcode_index]),
            outer_offset, &error_offset, &qxp_fixture.owner,
            &qxp_fixture.draw, &qxp_fixture.pixel, NULL, &qxp_fixture.table,
            &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state,
            qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 1, &counters);
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                      error_offset == outer_offset &&
                      qxp_status.code == QXP_D3D9_DRAW_PREPARE_OK &&
                      qxp_inspector.calls == 1 &&
                      qxp_inspector.saw_zero_failure_code &&
                      counters.host_draws == 0 &&
                      counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "capture success still rejects before transport");

        memset(&qxp_inspector, 0, sizeof(qxp_inspector));
        qxp_inspector.decline = TRUE;
        qxp_inspector.failure_code_to_set =
            QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE;
        memset(&counters, 0, sizeof(counters));
        error_offset = 0;
        result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
            QxpD3d9DrawCommandRecordSize(opcodes[opcode_index]),
            outer_offset, &error_offset, &qxp_fixture.owner,
            &qxp_fixture.draw, &qxp_fixture.pixel, NULL, &qxp_fixture.table,
            &qxp_allocator, qxp_lock, qxp_unlock, &qxp_lock_state,
            qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
        qxp_observe_legacy_command(result, 1, &counters);
        /* Status 21 is the only capture status that may claim a transient
         * resource failure; it is now reachable only through an allocator
         * failure inside B6G3B1.  It must still map to the outer resource
         * failure (QXP_DDERR_CURRENTLYNOTAVAIL) at the exact offset. */
        qxp_check(result == QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE &&
                      error_offset == outer_offset &&
                      qxp_status.code ==
                          QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_CAPTURE_RESOURCE &&
                      qxp_inspector.calls == 1 &&
                      counters.host_draws == 0 &&
                      counters.frame_marks == 0 &&
                      counters.batch_submissions == 0,
                  "capture resource maps before transport for every opcode");

        for (status_index = 0; status_index <
                QXP_COUNT(capture_semantic_codes); ++status_index) {
            memset(&qxp_inspector, 0, sizeof(qxp_inspector));
            qxp_inspector.decline = TRUE;
            qxp_inspector.failure_code_to_set =
                capture_semantic_codes[status_index];
            memset(&counters, 0, sizeof(counters));
            error_offset = 0;
            result = QxpD3d9GateDrawCommand(opcodes[opcode_index], 1,
                QxpD3d9DrawCommandRecordSize(opcodes[opcode_index]),
                outer_offset, &error_offset, &qxp_fixture.owner,
                &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
                &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
                &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector,
                NULL,
                &qxp_status);
            qxp_observe_legacy_command(result, 1, &counters);
            /* Statuses 20 and 22 are semantic: they must take the outer
             * unsupported (QXP_DDERR_INVALIDPARAMS) path, never the
             * transient-resource path.  A secure-callback failure now
             * classifies here, not as status 21. */
            qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                          error_offset == outer_offset &&
                          qxp_status.code ==
                              capture_semantic_codes[status_index] &&
                          qxp_inspector.calls == 1 &&
                          counters.host_draws == 0 &&
                          counters.frame_marks == 0 &&
                          counters.batch_submissions == 0,
                      "capture and access semantic status stays unsupported");
            qxp_check(!QxpD3d9DrawPrepareStatusIsResourceFailure(
                          &qxp_status) &&
                          result != QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE,
                      "capture and access semantic status is never resource");
        }
    }

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    qxp_inspector.decline = TRUE;
    qxp_inspector.failure_code_to_set =
        QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ENVELOPE;
    error_offset = 0;
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2, 0x760UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_DRAW_ENVELOPE &&
                  qxp_inspector.calls == 1 &&
                  qxp_inspector.saw_zero_failure_code,
              "declining inspector classifies draw envelope as 19");

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    qxp_inspector.decline = TRUE;
    qxp_inspector.failure_code_to_set = 0x7777UL;
    error_offset = 0;
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2, 0x770UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_UNSUPPORTED &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_RESOURCE_BACKING &&
                  qxp_inspector.calls == 1,
              "invalid inspector failure code normalizes to backing 18");

    QxpD3d9DrawStateInitialize(&legacy_draw);
    QxpD3d9ClearPixelBindingState(&legacy_pixel);

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    error_offset = 0xdeadUL;
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 2,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2 * 2, 0x800UL,
        &error_offset, &qxp_fixture.owner, &legacy_draw, &legacy_pixel,
        NULL, &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED &&
                  qxp_inspector.calls == 0,
              "legacy command never reaches prepared inspector");

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    result = QxpD3d9GateDrawCommand(55, 1, 0, 0x810UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_NOT_DRAW &&
                  qxp_inspector.calls == 0,
              "nondraw command never reaches prepared inspector");

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 0, 0,
        0x820UL, &error_offset, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, NULL, &qxp_fixture.table, &qxp_allocator,
        qxp_lock, qxp_unlock, &qxp_lock_state, qxp_inspect_prepared,
        &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_LEGACY_ALLOWED &&
                  qxp_inspector.calls == 0,
              "zero-count draw never reaches prepared inspector");

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 2,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2 * 2 - 1, 0x830UL,
        &error_offset, &qxp_fixture.owner, &qxp_fixture.draw,
        &qxp_fixture.pixel, NULL, &qxp_fixture.table, &qxp_allocator,
        qxp_lock, qxp_unlock, &qxp_lock_state, qxp_inspect_prepared,
        &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_TRUNCATED &&
                  qxp_inspector.calls == 0,
              "truncated draw never reaches prepared inspector");

    memset(&qxp_inspector, 0, sizeof(qxp_inspector));
    qxp_set_fail_after(1);
    result = QxpD3d9GateDrawCommand(QXP_D3D9_DP2OP_DRAWPRIMITIVE2, 1,
        QXP_D3D9_DRAW_RECORD_BYTES_DRAWPRIMITIVE2, 0x840UL, &error_offset,
        &qxp_fixture.owner, &qxp_fixture.draw, &qxp_fixture.pixel, NULL,
        &qxp_fixture.table, &qxp_allocator, qxp_lock, qxp_unlock,
        &qxp_lock_state, qxp_inspect_prepared, &qxp_inspector, NULL, &qxp_status);
    qxp_check(result == QXP_D3D9_DRAW_COMMAND_RESOURCE_FAILURE &&
                  qxp_inspector.calls == 0 &&
                  qxp_status.code ==
                      QXP_D3D9_DRAW_PREPARE_ERROR_ALLOCATION,
              "allocation failure never reaches prepared inspector");
    qxp_clear_failure();

    qxp_fixture_destroy(&qxp_fixture);
}

int main(void)
{
    memset(&qxp_alloc_state, 0, sizeof(qxp_alloc_state));
    memset(&qxp_lock_state, 0, sizeof(qxp_lock_state));
    qxp_allocator.allocate = qxp_allocate;
    qxp_allocator.release = qxp_release;
    qxp_allocator.opaque = &qxp_alloc_state;

    qxp_test_happy_immutable();
    qxp_test_prepare_cache_reuse();
    qxp_test_prepare_cache_associativity();
    qxp_test_vs11_vertex_binding();
    qxp_test_stream_variants_and_malformed_state();
    qxp_test_positiont_and_interface_missing();
    qxp_test_fixed_function_vertex_with_pixel_shader();
    qxp_test_fvf_vertex_with_ps11();
    qxp_test_binding_rejections();
    qxp_test_freshness_generation_and_race();
    qxp_test_allocation_failures_and_ps_snapshot();
    qxp_test_null_and_prepared_validation();
    qxp_test_execution_gate();
    qxp_test_production_command_gate();
    qxp_test_production_prepared_inspector();

    qxp_check(qxp_lock_state.depth == 0 &&
                  qxp_lock_state.acquires == qxp_lock_state.releases &&
                  qxp_lock_state.maximum_depth <= 1 &&
                  qxp_lock_state.violations == 0,
              "exact lock balance and no nesting");
    qxp_check(qxp_alloc_state.live == 0 &&
                  qxp_alloc_state.canary_failures == 0 &&
                  qxp_alloc_state.allocations_while_locked == 0 &&
                  qxp_alloc_state.releases_while_locked == 0,
              "exact allocation/free balance, canaries, and no lock allocation");

    if (qxp_failures) {
        fprintf(stderr, "qxp d3d9 draw prepare tests: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    printf("qxp d3d9 draw prepare tests: PASS\n");
    return 0;
}
