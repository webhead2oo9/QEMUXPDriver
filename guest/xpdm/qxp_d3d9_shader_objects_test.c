#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <windef.h>
#include <d3d9types.h>

#include "qxpdisp/qxp_d3d9_draw_state.h"
#include "qxpdisp/qxp_d3d9_shader_objects.h"

#define QXP_ARRAY_COUNT(a) ((DWORD)(sizeof(a) / sizeof((a)[0])))
#define QXP_TEST_PARAMETER 0x80000000UL
#define QXP_TEST_GUARD_A   0x13579bdfUL
#define QXP_TEST_GUARD_B   0x2468ace0UL
#define QXP_TEST_TAIL_BYTE 0x5aU
#define QXP_TEST_FILL_BYTE 0xa5U

#define QXP_TEST_INST(op, length) \
    ((DWORD)(op) | ((DWORD)(length) << D3DSI_INSTLENGTH_SHIFT))
#define QXP_TEST_REGTYPE(type) \
    ((((DWORD)(type) << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) | \
     (((DWORD)(type) << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2))
#define QXP_TEST_DST(type, number, mask, modifier) \
    (QXP_TEST_PARAMETER | QXP_TEST_REGTYPE(type) | (DWORD)(number) | \
     ((DWORD)(mask) << 16) | (DWORD)(modifier))
#define QXP_TEST_SRC(type, number) \
    (QXP_TEST_PARAMETER | QXP_TEST_REGTYPE(type) | (DWORD)(number) | \
     D3DSP_NOSWIZZLE | D3DSPSM_NONE)
#define QXP_TEST_DCL_USAGE(usage, index) \
    (QXP_TEST_PARAMETER | ((DWORD)(usage) << D3DSP_DCL_USAGE_SHIFT) | \
     ((DWORD)(index) << D3DSP_DCL_USAGEINDEX_SHIFT))

typedef char qxp_private_create_declaration_size_is_8[
    sizeof(QXP_D3D9_PRIVATE_CREATE_DECLARATION) == 8 ? 1 : -1];
typedef char qxp_private_create_function_size_is_8[
    sizeof(QXP_D3D9_PRIVATE_CREATE_FUNCTION) == 8 ? 1 : -1];
typedef char qxp_private_handle_size_is_4[
    sizeof(QXP_D3D9_PRIVATE_HANDLE) == 4 ? 1 : -1];
typedef char qxp_private_element_size_is_8[
    sizeof(QXP_VERTEX_ELEMENT9) == 8 ? 1 : -1];

typedef struct qxp_test_allocation_header {
    DWORD guards[4];
    ULONG size;
    DWORD identifier;
    DWORD padding[2];
} QXP_TEST_ALLOCATION_HEADER;

typedef struct qxp_test_allocator_state {
    DWORD calls;
    DWORD releases;
    DWORD live;
    DWORD peak_live;
    DWORD fail_at;
    DWORD next_identifier;
    DWORD canary_failures;
    DWORD allocations_while_locked;
    DWORD releases_while_locked;
    ULONG last_size;
} QXP_TEST_ALLOCATOR_STATE;

typedef struct qxp_test_lock_state {
    DWORD acquires;
    DWORD releases;
    DWORD depth;
    DWORD maximum_depth;
    DWORD violations;
} QXP_TEST_LOCK_STATE;

typedef struct qxp_guarded_status {
    DWORD before[4];
    QXP_D3D9_SHADER_OBJECT_STATUS value;
    DWORD after[4];
} QXP_GUARDED_STATUS;

typedef struct qxp_guarded_declaration_snapshot {
    DWORD before[4];
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT value;
    DWORD after[4];
} QXP_GUARDED_DECLARATION_SNAPSHOT;

typedef struct qxp_guarded_function_snapshot {
    DWORD before[4];
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT value;
    DWORD after[4];
} QXP_GUARDED_FUNCTION_SNAPSHOT;

typedef struct qxp_guarded_pixel_snapshot {
    DWORD before[4];
    QXP_D3D9_PIXEL_FUNCTION_SNAPSHOT value;
    DWORD after[4];
} QXP_GUARDED_PIXEL_SNAPSHOT;

typedef struct qxp_guarded_bytecode {
    DWORD before[4];
    DWORD value[8];
    DWORD after[4];
} QXP_GUARDED_BYTECODE;

static int qxp_failures;
static QXP_TEST_ALLOCATOR_STATE qxp_allocator_state;
static QXP_TEST_LOCK_STATE qxp_lock_state;
static QXP_D3D9_SHADER_ALLOCATOR qxp_allocator;
static QXP_D3D9_SHADER_OBJECT_STATUS qxp_zero_status;
static QXP_D3D9_SHADER_OBJECT_TABLE qxp_table;
static QXP_D3D9_SHADER_OBJECT_TABLE qxp_table_copy;
static BYTE qxp_object_copy[
    sizeof(QXP_D3D9_PIXEL_FUNCTION_OBJECT) +
    QXP_SHADER_MAX_TOKENS * sizeof(DWORD)];
static QXP_SHADER_IR qxp_direct_ir;
static QXP_VERTEX_DECLARATION9 qxp_direct_declaration;
static BYTE qxp_declaration_frame[
    QXP_D3D9_CREATE_RECORD_BYTES +
    QXP_VERTEX_DECLARATION9_MAX_ELEMENTS * QXP_VERTEX_ELEMENT9_BYTES];
static BYTE qxp_function_frame[
    QXP_D3D9_CREATE_RECORD_BYTES +
    QXP_SHADER_MAX_TOKENS * sizeof(DWORD)];
static BYTE qxp_batch_frame[
    2 * (QXP_D3D9_CREATE_RECORD_BYTES +
         QXP_SHADER_MAX_TOKENS * sizeof(DWORD))];
static QXP_VERTEX_ELEMENT9 qxp_max_elements[
    QXP_VERTEX_DECLARATION9_MAX_ELEMENTS];

static const QXP_VERTEX_ELEMENT9 qxp_valid_elements[] = {
    { 0, 0, QXP_VERTEX_DECLTYPE9_FLOAT3,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_POSITION, 0 },
    { 0, 12, QXP_VERTEX_DECLTYPE9_FLOAT2,
      QXP_VERTEX_DECLMETHOD9_DEFAULT,
      QXP_VERTEX_DECLUSAGE9_TEXCOORD, 0 }
};

static const DWORD qxp_minimal_vs[] = {
    D3DVS_VERSION(2, 0),
    QXP_TEST_INST(D3DSIO_DCL, 2),
    QXP_TEST_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
    QXP_TEST_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    QXP_TEST_INST(D3DSIO_MOV, 2),
    QXP_TEST_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
    QXP_TEST_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

/* Minimal vs_1_1 in real SM1 encoding: every instruction token has a zero
 * length field, which the decoder derives from the opcode schema. */
static const DWORD qxp_minimal_vs11[] = {
    QXP_SHADER_VERSION_VS_1_1,
    D3DSIO_DCL,
    QXP_TEST_DCL_USAGE(D3DDECLUSAGE_POSITION, 0),
    QXP_TEST_DST(D3DSPR_INPUT, 0, 15, D3DSPDM_NONE),
    D3DSIO_MOV,
    QXP_TEST_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 15, D3DSPDM_NONE),
    QXP_TEST_SRC(D3DSPR_INPUT, 0),
    D3DSIO_END
};

static const DWORD qxp_minimal_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_TEST_INST(D3DSIO_MOV, 2),
    QXP_TEST_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
    QXP_TEST_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static const DWORD qxp_minimal_ps11[] = {
    D3DPS_VERSION(1, 1),
    D3DSIO_END
};

/* ps_1_3 is a real legacy profile: the object table cannot lower it, so the
 * production dispatcher must keep routing it to the ARB pipeline.  It is what
 * discriminates the classification now that ps_1_1 lowers natively. */
static const DWORD qxp_minimal_ps13[] = {
    D3DPS_VERSION(1, 3),
    D3DSIO_END
};

/* Malformed output form used to discriminate decoder error propagation. */
static const DWORD qxp_pixel_output_error_ps[] = {
    D3DPS_VERSION(2, 0),
    QXP_TEST_INST(D3DSIO_ADD, 3),
    QXP_TEST_DST(D3DSPR_COLOROUT, 0, 15, D3DSPDM_NONE),
    QXP_TEST_SRC(D3DSPR_CONST, 0),
    QXP_TEST_SRC(D3DSPR_CONST, 0),
    D3DSIO_END
};

static void qxp_check(int condition, const char *message)
{
    if (condition) return;
    ++qxp_failures;
    fprintf(stderr, "qxp_d3d9_shader_objects: failed: %s\n", message);
}

static void qxp_reset_allocator(DWORD fail_at)
{
    qxp_check(qxp_allocator_state.live == 0,
              "allocator reset must not hide a leak");
    qxp_check(qxp_lock_state.depth == 0,
              "allocator reset requires an unlocked table");
    memset(&qxp_allocator_state, 0, sizeof(qxp_allocator_state));
    qxp_allocator_state.fail_at = fail_at;
    qxp_allocator.opaque = &qxp_allocator_state;
}

static void qxp_reset_lock_state(void)
{
    qxp_check(qxp_lock_state.depth == 0,
              "lock reset must not hide an imbalance");
    memset(&qxp_lock_state, 0, sizeof(qxp_lock_state));
}

static void qxp_test_lock(void *opaque)
{
    QXP_TEST_LOCK_STATE *state;

    state = (QXP_TEST_LOCK_STATE *)opaque;
    ++state->acquires;
    if (state->depth) ++state->violations;
    ++state->depth;
    if (state->depth > state->maximum_depth)
        state->maximum_depth = state->depth;
}

static void qxp_test_unlock(void *opaque)
{
    QXP_TEST_LOCK_STATE *state;

    state = (QXP_TEST_LOCK_STATE *)opaque;
    ++state->releases;
    if (state->depth != 1) {
        ++state->violations;
        if (!state->depth) return;
    }
    --state->depth;
}

static int qxp_lock_contract_ok(DWORD expected_acquires)
{
    return qxp_lock_state.acquires == expected_acquires &&
           qxp_lock_state.releases == expected_acquires &&
           qxp_lock_state.depth == 0 &&
           qxp_lock_state.maximum_depth <= 1 &&
           qxp_lock_state.violations == 0 &&
           qxp_allocator_state.allocations_while_locked == 0 &&
           qxp_allocator_state.releases_while_locked == 0;
}

static void *qxp_test_allocate(ULONG byte_count, void *opaque)
{
    QXP_TEST_ALLOCATOR_STATE *state;
    QXP_TEST_ALLOCATION_HEADER *header;
    BYTE *payload;
    BYTE *tail;
    ULONG index;
    size_t total;

    state = (QXP_TEST_ALLOCATOR_STATE *)opaque;
    ++state->calls;
    if (qxp_lock_state.depth) ++state->allocations_while_locked;
    state->last_size = byte_count;
    if (state->fail_at && state->calls == state->fail_at) return NULL;
    total = sizeof(*header) + (size_t)byte_count + 16U;
    header = (QXP_TEST_ALLOCATION_HEADER *)malloc(total);
    if (!header) return NULL;
    for (index = 0; index < QXP_ARRAY_COUNT(header->guards); ++index)
        header->guards[index] = QXP_TEST_GUARD_A + index;
    header->size = byte_count;
    header->identifier = ++state->next_identifier;
    header->padding[0] = QXP_TEST_GUARD_B;
    header->padding[1] = ~QXP_TEST_GUARD_B;
    payload = (BYTE *)(header + 1);
    memset(payload, QXP_TEST_FILL_BYTE, byte_count);
    tail = payload + byte_count;
    for (index = 0; index < 16; ++index) tail[index] = QXP_TEST_TAIL_BYTE;
    ++state->live;
    if (state->live > state->peak_live) state->peak_live = state->live;
    return payload;
}

static void qxp_test_release(void *allocation, void *opaque)
{
    QXP_TEST_ALLOCATOR_STATE *state;
    QXP_TEST_ALLOCATION_HEADER *header;
    BYTE *tail;
    ULONG index;

    state = (QXP_TEST_ALLOCATOR_STATE *)opaque;
    if (qxp_lock_state.depth) ++state->releases_while_locked;
    if (!allocation) {
        ++state->canary_failures;
        return;
    }
    header = ((QXP_TEST_ALLOCATION_HEADER *)allocation) - 1;
    for (index = 0; index < QXP_ARRAY_COUNT(header->guards); ++index) {
        if (header->guards[index] != QXP_TEST_GUARD_A + index)
            ++state->canary_failures;
    }
    if (header->padding[0] != QXP_TEST_GUARD_B ||
        header->padding[1] != ~QXP_TEST_GUARD_B)
        ++state->canary_failures;
    tail = (BYTE *)allocation + header->size;
    for (index = 0; index < 16; ++index) {
        if (tail[index] != QXP_TEST_TAIL_BYTE)
            ++state->canary_failures;
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

static void qxp_initialize_owner(QXP_SHADER_OWNER *owner, DWORD context,
                                 DWORD process, const void *device)
{
    qxp_shader_owner_initialize(
        owner, (unsigned long)context, (unsigned long)process, device);
}

static int qxp_owner_is(const QXP_SHADER_OWNER *owner,
                        const QXP_SHADER_OWNER *expected)
{
    return qxp_shader_owner_matches(
        owner, expected->context_handle, expected->process_id,
        expected->device);
}

static int qxp_status_is_zero(
    const QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    return memcmp(status, &qxp_zero_status, sizeof(*status)) == 0;
}

static void qxp_prepare_guarded_status(QXP_GUARDED_STATUS *guard)
{
    DWORD index;

    memset(guard, 0xcc, sizeof(*guard));
    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        guard->before[index] = QXP_TEST_GUARD_A + index;
        guard->after[index] = QXP_TEST_GUARD_B - index;
    }
}

static int qxp_guarded_status_ok(const QXP_GUARDED_STATUS *guard)
{
    DWORD index;

    for (index = 0; index < QXP_ARRAY_COUNT(guard->before); ++index) {
        if (guard->before[index] != QXP_TEST_GUARD_A + index ||
            guard->after[index] != QXP_TEST_GUARD_B - index) return 0;
    }
    return 1;
}

static void qxp_prepare_snapshot_guards(DWORD *before, DWORD *after,
                                        DWORD count)
{
    DWORD index;

    for (index = 0; index < count; ++index) {
        before[index] = QXP_TEST_GUARD_B + index;
        after[index] = QXP_TEST_GUARD_A - index;
    }
}

static int qxp_snapshot_guards_ok(const DWORD *before, const DWORD *after,
                                  DWORD count)
{
    DWORD index;

    for (index = 0; index < count; ++index) {
        if (before[index] != QXP_TEST_GUARD_B + index ||
            after[index] != QXP_TEST_GUARD_A - index) return 0;
    }
    return 1;
}

static ULONG qxp_append_declaration_record(
    BYTE *buffer, ULONG capacity, ULONG offset, DWORD handle,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count)
{
    QXP_D3D9_PRIVATE_CREATE_DECLARATION header;
    ULONG payload_bytes;
    ULONG record_bytes;

    payload_bytes = element_count * QXP_VERTEX_ELEMENT9_BYTES;
    record_bytes = QXP_D3D9_CREATE_RECORD_BYTES + payload_bytes;
    if (!buffer || !elements || offset > capacity ||
        record_bytes > capacity - offset) {
        qxp_check(0, "declaration batch fixture capacity");
        return offset;
    }
    header.handle = handle;
    header.count = element_count;
    memcpy(buffer + offset, &header, sizeof(header));
    memcpy(buffer + offset + QXP_D3D9_CREATE_RECORD_BYTES,
           elements, payload_bytes);
    return offset + record_bytes;
}

static ULONG qxp_append_function_record(
    BYTE *buffer, ULONG capacity, ULONG offset, DWORD handle,
    const DWORD *tokens, DWORD bytecode_size)
{
    QXP_D3D9_PRIVATE_CREATE_FUNCTION header;
    ULONG record_bytes;

    record_bytes = QXP_D3D9_CREATE_RECORD_BYTES + bytecode_size;
    if (!buffer || !tokens || offset > capacity ||
        record_bytes > capacity - offset) {
        qxp_check(0, "function batch fixture capacity");
        return offset;
    }
    header.handle = handle;
    header.size = bytecode_size;
    memcpy(buffer + offset, &header, sizeof(header));
    memcpy(buffer + offset + QXP_D3D9_CREATE_RECORD_BYTES,
           tokens, bytecode_size);
    return offset + record_bytes;
}

static ULONG qxp_append_handle_record(
    BYTE *buffer, ULONG capacity, ULONG offset, DWORD handle)
{
    QXP_D3D9_PRIVATE_HANDLE record;

    if (!buffer || offset > capacity ||
        QXP_D3D9_HANDLE_RECORD_BYTES > capacity - offset) {
        qxp_check(0, "handle batch fixture capacity");
        return offset;
    }
    record.handle = handle;
    memcpy(buffer + offset, &record, sizeof(record));
    return offset + QXP_D3D9_HANDLE_RECORD_BYTES;
}

static BOOL qxp_process_batch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_BINDING *declaration_binding, DWORD *legacy_fvf,
    QXP_D3D9_SHADER_BINDING *function_binding, DWORD command_offset,
    ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    return QxpD3d9ProcessShaderObjectBatch(
        opcode, record_count, records, available_bytes, owner, &qxp_table,
        declaration_binding, legacy_fvf, function_binding, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state, command_offset,
        consumed_bytes, error_offset, status);
}

static BOOL qxp_process_pixel_batch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_BINDING *pixel_binding, DWORD command_offset,
    ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    return QxpD3d9ProcessPixelFunctionBatch(
        opcode, record_count, records, available_bytes, owner, &qxp_table,
        pixel_binding, &qxp_allocator, qxp_test_lock, qxp_test_unlock,
        &qxp_lock_state, command_offset, consumed_bytes, error_offset,
        status);
}

typedef struct qxp_test_pixel_dispatch_state {
    DWORD total_calls;
    DWORD create_calls;
    DWORD set_calls;
    DWORD delete_calls;
    DWORD fail_call;
    DWORD fail_code;
    DWORD handles[8];
    DWORD bytecode_sizes[8];
    DWORD versions[8];
    BYTE kinds[8];
    BYTE ps20[8];
} QXP_TEST_PIXEL_DISPATCH_STATE;

static BOOL qxp_test_pixel_dispatch_finish_call(
    QXP_TEST_PIXEL_DISPATCH_STATE *state,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    if (state->fail_call && state->total_calls == state->fail_call) {
        memset(status, 0, sizeof(*status));
        status->code = state->fail_code;
        return FALSE;
    }
    return TRUE;
}

static BOOL qxp_test_pixel_dispatch_create(
    DWORD handle, const DWORD *tokens, DWORD bytecode_size, BOOL ps20,
    void *opaque, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_TEST_PIXEL_DISPATCH_STATE *state =
        (QXP_TEST_PIXEL_DISPATCH_STATE *)opaque;
    DWORD slot = state->total_calls;

    if (slot < QXP_ARRAY_COUNT(state->handles)) {
        state->handles[slot] = handle;
        state->bytecode_sizes[slot] = bytecode_size;
        state->versions[slot] = tokens ? tokens[0] : 0;
        state->kinds[slot] = 1;
        state->ps20[slot] = ps20 ? 1 : 0;
    }
    ++state->total_calls;
    ++state->create_calls;
    return qxp_test_pixel_dispatch_finish_call(state, status);
}

static BOOL qxp_test_pixel_dispatch_set(
    DWORD handle, void *opaque, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_TEST_PIXEL_DISPATCH_STATE *state =
        (QXP_TEST_PIXEL_DISPATCH_STATE *)opaque;
    DWORD slot = state->total_calls;

    if (slot < QXP_ARRAY_COUNT(state->handles)) {
        state->handles[slot] = handle;
        state->kinds[slot] = 2;
    }
    ++state->total_calls;
    ++state->set_calls;
    return qxp_test_pixel_dispatch_finish_call(state, status);
}

static BOOL qxp_test_pixel_dispatch_delete(
    DWORD handle, void *opaque, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_TEST_PIXEL_DISPATCH_STATE *state =
        (QXP_TEST_PIXEL_DISPATCH_STATE *)opaque;
    DWORD slot = state->total_calls;

    if (slot < QXP_ARRAY_COUNT(state->handles)) {
        state->handles[slot] = handle;
        state->kinds[slot] = 3;
    }
    ++state->total_calls;
    ++state->delete_calls;
    return qxp_test_pixel_dispatch_finish_call(state, status);
}

static DWORD qxp_random(DWORD *state)
{
    *state = *state * 1664525UL + 1013904223UL;
    return *state;
}

static void qxp_build_max_declaration(void)
{
    static const BYTE safe_usages[] = {
        QXP_VERTEX_DECLUSAGE9_BLENDWEIGHT,
        QXP_VERTEX_DECLUSAGE9_BLENDINDICES,
        QXP_VERTEX_DECLUSAGE9_NORMAL,
        QXP_VERTEX_DECLUSAGE9_PSIZE,
        QXP_VERTEX_DECLUSAGE9_TEXCOORD,
        QXP_VERTEX_DECLUSAGE9_TANGENT,
        QXP_VERTEX_DECLUSAGE9_BINORMAL,
        QXP_VERTEX_DECLUSAGE9_COLOR,
        QXP_VERTEX_DECLUSAGE9_FOG,
        QXP_VERTEX_DECLUSAGE9_DEPTH
    };
    DWORD index;

    for (index = 0; index < QXP_ARRAY_COUNT(qxp_max_elements); ++index) {
        QXP_VERTEX_ELEMENT9 *element;

        element = &qxp_max_elements[index];
        element->stream = 0;
        element->offset = (WORD)(index * 4UL);
        element->type = QXP_VERTEX_DECLTYPE9_FLOAT1;
        element->method = QXP_VERTEX_DECLMETHOD9_DEFAULT;
        element->usage = safe_usages[index % QXP_ARRAY_COUNT(safe_usages)];
        element->usage_index = (BYTE)(index /
            QXP_ARRAY_COUNT(safe_usages));
    }
}

static void qxp_test_layout_and_literals(void)
{
    qxp_check(QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION == 71U &&
              QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION == 72U &&
              QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION == 73U &&
              QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION == 74U &&
              QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION == 75U &&
              QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION == 76U,
              "private opcode literals 71 through 76");
    qxp_check(QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION == 54U &&
              QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION == 55U &&
              QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION == 56U,
              "standard pixel-function opcode literals 54 through 56");
    qxp_check(sizeof(QXP_D3D9_PRIVATE_CREATE_DECLARATION) == 8 &&
              offsetof(QXP_D3D9_PRIVATE_CREATE_DECLARATION, handle) == 0 &&
              offsetof(QXP_D3D9_PRIVATE_CREATE_DECLARATION, count) == 4 &&
              sizeof(QXP_D3D9_PRIVATE_CREATE_FUNCTION) == 8 &&
              offsetof(QXP_D3D9_PRIVATE_CREATE_FUNCTION, handle) == 0 &&
              offsetof(QXP_D3D9_PRIVATE_CREATE_FUNCTION, size) == 4 &&
              sizeof(QXP_D3D9_PRIVATE_HANDLE) == 4 &&
              offsetof(QXP_D3D9_PRIVATE_HANDLE, handle) == 0,
              "private record byte layout");
    qxp_check(sizeof(QXP_VERTEX_ELEMENT9) == 8 &&
              QXP_D3D9_CREATE_RECORD_BYTES == 8 &&
              QXP_D3D9_HANDLE_RECORD_BYTES == 4 &&
              QXP_D3D9_SHADER_OBJECT_CAPACITY == 256 &&
              QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT == 1,
              "private size, handle-bit, and table literals");
    qxp_check(sizeof(QXP_D3D9_SHADER_DECODE_SUMMARY) <
                  sizeof(QXP_SHADER_IR) &&
              sizeof(QXP_D3D9_VERTEX_FUNCTION_OBJECT) <
                  sizeof(QXP_SHADER_IR) &&
              sizeof(QXP_D3D9_PIXEL_FUNCTION_OBJECT) <
                  sizeof(QXP_SHADER_IR) &&
              offsetof(QXP_D3D9_PIXEL_FUNCTION_OBJECT, bytecode) +
                  sizeof(DWORD) <=
                  sizeof(QXP_D3D9_PIXEL_FUNCTION_OBJECT) &&
              QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE == 20 &&
              QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_LOWERING == 21,
              "compact pixel layout and appended error numbering");
}

static void qxp_test_declaration_framing(void)
{
    QXP_D3D9_PRIVATE_CREATE_DECLARATION *header;
    QXP_GUARDED_STATUS guarded;
    DWORD handle;
    DWORD count;
    ULONG consumed;
    ULONG prefix;
    ULONG total;
    BOOL result;

    memset(qxp_declaration_frame, 0, sizeof(qxp_declaration_frame));
    header = (QXP_D3D9_PRIVATE_CREATE_DECLARATION *)qxp_declaration_frame;
    header->handle = 17;
    header->count = 1;
    memcpy(qxp_declaration_frame + QXP_D3D9_CREATE_RECORD_BYTES,
           qxp_valid_elements, sizeof(QXP_VERTEX_ELEMENT9));
    total = QXP_D3D9_CREATE_RECORD_BYTES + QXP_VERTEX_ELEMENT9_BYTES;
    for (prefix = 0; prefix < total; ++prefix) {
        handle = 0xaaaaaaaaUL;
        count = 0xbbbbbbbbUL;
        consumed = 0xccccccccUL;
        qxp_prepare_guarded_status(&guarded);
        result = QxpD3d9FrameCreateVertexDeclaration(
            qxp_declaration_frame, prefix, &handle, &count, &consumed,
            &guarded.value);
        qxp_check(!result && !handle && !count && !consumed,
                  "declaration every-prefix output zeroing");
        qxp_check(guarded.value.code ==
                      (prefix < QXP_D3D9_CREATE_RECORD_BYTES ?
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER :
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD) &&
                  guarded.value.byte_offset == prefix,
                  "declaration every-prefix exact status");
        qxp_check(qxp_guarded_status_ok(&guarded),
                  "declaration framing status canaries");
    }
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, total + 9, &handle, &count, &consumed,
        &guarded.value);
    qxp_check(result && handle == 17 && count == 1 && consumed == total &&
              qxp_status_is_zero(&guarded.value),
              "declaration exact consumed bytes with trailing input");

    header->handle = 0;
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, total, &handle, &count, &consumed,
        &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE &&
              !handle && !count && !consumed,
              "declaration zero handle");
    header->handle = 16;
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, total, &handle, &count, &consumed,
        &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE &&
              !handle && !count && !consumed,
              "declaration create rejects an even FVF handle");
    header->handle = 17;
    header->count = 0;
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &count, &consumed, &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT &&
              guarded.value.byte_offset == 4,
              "declaration zero count");
    header->count = QXP_VERTEX_DECLARATION9_MAX_ELEMENTS + 1;
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &count, &consumed, &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT,
              "declaration over-bound count before payload read");

    header->count = QXP_VERTEX_DECLARATION9_MAX_ELEMENTS;
    total = QXP_D3D9_CREATE_RECORD_BYTES +
        QXP_VERTEX_DECLARATION9_MAX_ELEMENTS * QXP_VERTEX_ELEMENT9_BYTES;
    result = QxpD3d9FrameCreateVertexDeclaration(
        qxp_declaration_frame, total, &handle, &count, &consumed,
        &guarded.value);
    qxp_check(result && count == QXP_VERTEX_DECLARATION9_MAX_ELEMENTS &&
              consumed == total,
              "declaration maximum frame");
}

static void qxp_test_function_framing(void)
{
    QXP_D3D9_PRIVATE_CREATE_FUNCTION *header;
    QXP_GUARDED_STATUS guarded;
    DWORD handle;
    DWORD size;
    ULONG consumed;
    ULONG prefix;
    ULONG total;
    BOOL result;

    memset(qxp_function_frame, 0, sizeof(qxp_function_frame));
    header = (QXP_D3D9_PRIVATE_CREATE_FUNCTION *)qxp_function_frame;
    header->handle = 23;
    header->size = sizeof(qxp_minimal_vs);
    memcpy(qxp_function_frame + QXP_D3D9_CREATE_RECORD_BYTES,
           qxp_minimal_vs, sizeof(qxp_minimal_vs));
    total = QXP_D3D9_CREATE_RECORD_BYTES + sizeof(qxp_minimal_vs);
    for (prefix = 0; prefix < total; ++prefix) {
        handle = 0xaaaaaaaaUL;
        size = 0xbbbbbbbbUL;
        consumed = 0xccccccccUL;
        qxp_prepare_guarded_status(&guarded);
        result = QxpD3d9FrameCreateVertexFunction(
            qxp_function_frame, prefix, &handle, &size, &consumed,
            &guarded.value);
        qxp_check(!result && !handle && !size && !consumed,
                  "function every-prefix output zeroing");
        qxp_check(guarded.value.code ==
                      (prefix < QXP_D3D9_CREATE_RECORD_BYTES ?
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER :
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD) &&
                  guarded.value.byte_offset == prefix,
                  "function every-prefix exact status");
        qxp_check(qxp_guarded_status_ok(&guarded),
                  "function framing status canaries");
    }
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, total + 11, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(result && handle == 23 && size == sizeof(qxp_minimal_vs) &&
              consumed == total && qxp_status_is_zero(&guarded.value),
              "function exact consumed bytes with trailing input");

    header->handle = 0;
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, total, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE,
              "function zero handle");
    header->handle = 23;
    header->size = 0;
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &size, &consumed, &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE &&
              guarded.value.byte_offset == 4,
              "function zero size");
    header->size = 6;
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &size, &consumed, &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE,
              "function non-DWORD size");
    header->size = (QXP_SHADER_MAX_TOKENS + 1UL) * sizeof(DWORD);
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &size, &consumed, &guarded.value);
    qxp_check(!result && guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE &&
              !handle && !size && !consumed,
              "function over-bound size before payload read");

    header->size = QXP_SHADER_MAX_TOKENS * sizeof(DWORD);
    total = QXP_D3D9_CREATE_RECORD_BYTES + header->size;
    result = QxpD3d9FrameCreateVertexFunction(
        qxp_function_frame, total, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(result && size == QXP_SHADER_MAX_TOKENS * sizeof(DWORD) &&
              consumed == total,
              "function maximum token frame");
}

static void qxp_test_pixel_function_framing(void)
{
    QXP_D3D9_PRIVATE_CREATE_FUNCTION *header;
    QXP_GUARDED_STATUS guarded;
    DWORD handle;
    DWORD size;
    ULONG consumed;
    ULONG prefix;
    ULONG total;
    BOOL result;

    memset(qxp_function_frame, 0, sizeof(qxp_function_frame));
    header = (QXP_D3D9_PRIVATE_CREATE_FUNCTION *)qxp_function_frame;
    header->handle = 29;
    header->size = sizeof(qxp_minimal_ps);
    memcpy(qxp_function_frame + QXP_D3D9_CREATE_RECORD_BYTES,
           qxp_minimal_ps, sizeof(qxp_minimal_ps));
    total = QXP_D3D9_CREATE_RECORD_BYTES + sizeof(qxp_minimal_ps);
    for (prefix = 0; prefix < total; ++prefix) {
        handle = 0xaaaaaaaaUL;
        size = 0xbbbbbbbbUL;
        consumed = 0xccccccccUL;
        qxp_prepare_guarded_status(&guarded);
        result = QxpD3d9FrameCreatePixelFunction(
            qxp_function_frame, prefix, &handle, &size, &consumed,
            &guarded.value);
        qxp_check(!result && !handle && !size && !consumed,
                  "pixel frame every-prefix output zeroing");
        qxp_check(guarded.value.code ==
                      (prefix < QXP_D3D9_CREATE_RECORD_BYTES ?
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER :
                       QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD) &&
                  guarded.value.byte_offset == prefix &&
                  qxp_guarded_status_ok(&guarded),
                  "pixel frame every-prefix exact guarded status");
    }
    result = QxpD3d9FrameCreatePixelFunction(
        qxp_function_frame, total + 13UL, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(result && handle == 29 && size == sizeof(qxp_minimal_ps) &&
              consumed == total && qxp_status_is_zero(&guarded.value),
              "pixel frame exact record with trailing bytes");

    header->handle = 0;
    result = QxpD3d9FrameCreatePixelFunction(
        qxp_function_frame, total, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(!result && !handle && !size && !consumed &&
              guarded.value.code == QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE,
              "pixel frame zero handle");
    header->handle = 29;
    header->size = 2;
    result = QxpD3d9FrameCreatePixelFunction(
        qxp_function_frame, total, &handle, &size, &consumed,
        &guarded.value);
    qxp_check(!result && !handle && !size && !consumed &&
              guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE &&
              guarded.value.byte_offset == 4,
              "pixel frame rejects non-DWORD bytecode size");
    header->size = (QXP_SHADER_MAX_TOKENS + 1UL) * sizeof(DWORD);
    result = QxpD3d9FrameCreatePixelFunction(
        qxp_function_frame, QXP_D3D9_CREATE_RECORD_BYTES,
        &handle, &size, &consumed, &guarded.value);
    qxp_check(!result && !handle && !size && !consumed &&
              guarded.value.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE,
              "pixel frame rejects over-bound bytecode before payload");
}

static void qxp_test_mutation_framing(void)
{
    QXP_D3D9_PRIVATE_CREATE_DECLARATION *declaration_header;
    QXP_D3D9_PRIVATE_CREATE_FUNCTION *function_header;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    DWORD random_state;
    DWORD iteration;
    DWORD handle;
    DWORD value;
    DWORD expected_code;
    ULONG available;
    ULONG consumed;
    ULONG payload;
    BOOL expected;
    BOOL result;

    declaration_header =
        (QXP_D3D9_PRIVATE_CREATE_DECLARATION *)qxp_declaration_frame;
    function_header =
        (QXP_D3D9_PRIVATE_CREATE_FUNCTION *)qxp_function_frame;
    random_state = 0x31415926UL;
    for (iteration = 0; iteration < 3000; ++iteration) {
        declaration_header->handle = qxp_random(&random_state) & 7UL;
        declaration_header->count = qxp_random(&random_state) % 70UL;
        available = qxp_random(&random_state) %
            (sizeof(qxp_declaration_frame) + 1UL);
        handle = 0xaaaaaaaaUL;
        value = 0xbbbbbbbbUL;
        consumed = 0xccccccccUL;
        expected = FALSE;
        if (available < QXP_D3D9_CREATE_RECORD_BYTES) {
            expected_code =
                QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER;
        } else if (!declaration_header->handle) {
            expected_code = QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE;
        } else if (!(declaration_header->handle &
                     QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT)) {
            expected_code =
                QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE;
        } else if (!declaration_header->count ||
                   declaration_header->count >
                       QXP_VERTEX_DECLARATION9_MAX_ELEMENTS) {
            expected_code =
                QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT;
        } else {
            payload = declaration_header->count *
                      QXP_VERTEX_ELEMENT9_BYTES;
            if (payload > available - QXP_D3D9_CREATE_RECORD_BYTES) {
                expected_code =
                    QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD;
            } else {
                expected = TRUE;
                expected_code = QXP_D3D9_SHADER_OBJECT_OK;
            }
        }
        result = QxpD3d9FrameCreateVertexDeclaration(
            qxp_declaration_frame, available, &handle, &value, &consumed,
            &status);
        qxp_check(result == expected && status.code == expected_code,
                  "bounded declaration framing mutation result");
        if (expected) {
            qxp_check(handle == declaration_header->handle &&
                      value == declaration_header->count &&
                      consumed == QXP_D3D9_CREATE_RECORD_BYTES +
                          declaration_header->count *
                              QXP_VERTEX_ELEMENT9_BYTES,
                      "bounded declaration framing mutation consumed");
        } else {
            qxp_check(!handle && !value && !consumed,
                      "bounded declaration framing mutation zeroing");
        }

        function_header->handle = qxp_random(&random_state) & 7UL;
        function_header->size = qxp_random(&random_state) %
            (QXP_SHADER_MAX_TOKENS * sizeof(DWORD) + 9UL);
        available = qxp_random(&random_state) %
            (sizeof(qxp_function_frame) + 1UL);
        handle = 0xaaaaaaaaUL;
        value = 0xbbbbbbbbUL;
        consumed = 0xccccccccUL;
        expected = FALSE;
        if (available < QXP_D3D9_CREATE_RECORD_BYTES) {
            expected_code =
                QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER;
        } else if (!function_header->handle) {
            expected_code = QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE;
        } else if (!function_header->size ||
                   (function_header->size & 3UL) ||
                   function_header->size / sizeof(DWORD) >
                       QXP_SHADER_MAX_TOKENS) {
            expected_code = QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE;
        } else if (function_header->size >
                   available - QXP_D3D9_CREATE_RECORD_BYTES) {
            expected_code =
                QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD;
        } else {
            expected = TRUE;
            expected_code = QXP_D3D9_SHADER_OBJECT_OK;
        }
        result = QxpD3d9FrameCreateVertexFunction(
            qxp_function_frame, available, &handle, &value, &consumed,
            &status);
        qxp_check(result == expected && status.code == expected_code,
                  "bounded function framing mutation result");
        if (expected) {
            qxp_check(handle == function_header->handle &&
                      value == function_header->size &&
                      consumed == QXP_D3D9_CREATE_RECORD_BYTES +
                          function_header->size,
                      "bounded function framing mutation consumed");
        } else {
            qxp_check(!handle && !value && !consumed,
                      "bounded function framing mutation zeroing");
        }
    }
}

static void qxp_test_prepare_success_and_retention(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_VERTEX_ELEMENT9 mutable_elements[2];
    DWORD mutable_tokens[QXP_ARRAY_COUNT(qxp_minimal_vs)];
    ULONG expected_function_bytes;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 7, 11, &device);
    qxp_reset_allocator(0);
    memcpy(mutable_elements, qxp_valid_elements, sizeof(mutable_elements));
    declaration = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 41, mutable_elements, QXP_ARRAY_COUNT(mutable_elements),
        &qxp_allocator, &declaration, &status);
    qxp_check(result && declaration && qxp_status_is_zero(&status) &&
              declaration->handle == 41 && !declaration->generation &&
              qxp_owner_is(&declaration->owner, &owner) &&
              declaration->declaration.valid &&
              declaration->declaration.element_count == 2 &&
              qxp_allocator_state.calls == 1 &&
              qxp_allocator_state.live == 1,
              "valid declaration prepare");
    mutable_elements[0].stream = 15;
    mutable_elements[0].offset = 400;
    mutable_elements[0].usage = QXP_VERTEX_DECLUSAGE9_COLOR;
    qxp_check(declaration->declaration.elements[0].stream == 0 &&
              declaration->declaration.elements[0].offset == 0 &&
              declaration->declaration.elements[0].usage ==
                  QXP_VERTEX_DECLUSAGE9_POSITION,
              "declaration retains no input storage");
    QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "declaration destroy and canaries");

    qxp_build_max_declaration();
    declaration = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 43, qxp_max_elements, QXP_ARRAY_COUNT(qxp_max_elements),
        &qxp_allocator, &declaration, &status);
    qxp_check(result && declaration &&
              declaration->declaration.element_count ==
                  QXP_VERTEX_DECLARATION9_MAX_ELEMENTS,
              "maximum declaration prepare");
    QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);

    memcpy(mutable_tokens, qxp_minimal_vs, sizeof(mutable_tokens));
    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 51, mutable_tokens, sizeof(mutable_tokens),
        &qxp_allocator, &function, &status);
    expected_function_bytes = (ULONG)sizeof(*function) +
        sizeof(mutable_tokens) - sizeof(DWORD);
    qxp_check(result && function && qxp_status_is_zero(&status) &&
              function->handle == 51 && !function->generation &&
              qxp_owner_is(&function->owner, &owner) &&
              function->bytecode_size == sizeof(mutable_tokens) &&
              function->summary.stage == QXP_SHADER_STAGE_VERTEX &&
              function->summary.version_token ==
                  QXP_SHADER_VERSION_VS_2_0 &&
              function->summary.token_count ==
                  QXP_ARRAY_COUNT(mutable_tokens) &&
              function->summary.instruction_count == 2 &&
              function->summary.declaration_count == 1 &&
              function->summary.block_count != 0 &&
              qxp_allocator_state.last_size == expected_function_bytes &&
              qxp_allocator_state.live == 1,
              "exact vs_2_0 function prepare and compact summary");
    mutable_tokens[0] = 0;
    mutable_tokens[1] = 0;
    qxp_check(memcmp(function->bytecode, qxp_minimal_vs,
                     sizeof(qxp_minimal_vs)) == 0,
              "function bytecode deep copy");
    QxpD3d9DestroyVertexFunction(function, &qxp_allocator);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures &&
              qxp_allocator_state.calls == 4 &&
              qxp_allocator_state.releases == 4,
              "prepare allocation balance");
}

static void qxp_test_prepare_errors_and_failures(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_OBJECT_STATUS first_status;
    QXP_D3D9_SHADER_OBJECT_STATUS second_status;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_VERTEX_ELEMENT9 bad_elements[2];
    QXP_VERTEX_DECLARATION9_ERROR direct_parse_error;
    QXP_SHADER_DECODE_ERROR direct_decode_error;
    DWORD bad_tokens[QXP_ARRAY_COUNT(qxp_minimal_vs)];
    DWORD failure;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 13, 17, &device);
    memcpy(bad_elements, qxp_valid_elements, sizeof(bad_elements));
    bad_elements[1].usage = bad_elements[0].usage;
    bad_elements[1].usage_index = bad_elements[0].usage_index;
    memset(&direct_parse_error, 0, sizeof(direct_parse_error));
    result = QxpParseVertexDeclaration9(
        bad_elements, QXP_ARRAY_COUNT(bad_elements),
        &qxp_direct_declaration, &direct_parse_error);
    qxp_check(!result && direct_parse_error.code != 0,
              "direct parser error fixture");
    qxp_reset_allocator(0);
    declaration = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 60, qxp_valid_elements,
        QXP_ARRAY_COUNT(qxp_valid_elements), &qxp_allocator,
        &declaration, &first_status);
    qxp_check(!result && !declaration && first_status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE &&
              !qxp_allocator_state.calls,
              "declaration prepare rejects an even FVF handle");
    memset(&first_status, 0xaa, sizeof(first_status));
    declaration = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 61, bad_elements, QXP_ARRAY_COUNT(bad_elements),
        &qxp_allocator, &declaration, &first_status);
    qxp_check(!result && !declaration &&
              first_status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_PARSE &&
              first_status.byte_offset == direct_parse_error.byte_offset &&
              memcmp(&first_status.declaration_error, &direct_parse_error,
                     sizeof(direct_parse_error)) == 0 &&
              first_status.decode_error.code == 0 &&
              qxp_allocator_state.calls == 0 &&
              qxp_allocator_state.live == 0,
              "exact nested parser error propagation and zero output");
    memset(&second_status, 0x55, sizeof(second_status));
    declaration = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 61, bad_elements, QXP_ARRAY_COUNT(bad_elements),
        &qxp_allocator, &declaration, &second_status);
    qxp_check(!result && !declaration &&
              memcmp(&first_status, &second_status,
                     sizeof(first_status)) == 0,
              "deterministic parser failure status");

    memcpy(bad_tokens, qxp_minimal_vs, sizeof(bad_tokens));
    bad_tokens[1] = QXP_TEST_INST(0x1234UL, 0);
    memset(&direct_decode_error, 0, sizeof(direct_decode_error));
    result = QxpDecodeShader20(
        bad_tokens, QXP_ARRAY_COUNT(bad_tokens), &qxp_direct_ir,
        &direct_decode_error);
    qxp_check(!result && direct_decode_error.code != 0,
              "direct decoder error fixture");
    memset(&first_status, 0xaa, sizeof(first_status));
    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 62, bad_tokens, sizeof(bad_tokens), &qxp_allocator,
        &function, &first_status);
    qxp_check(!result && !function &&
              first_status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE &&
              first_status.byte_offset ==
                  direct_decode_error.token_offset * sizeof(DWORD) &&
              memcmp(&first_status.decode_error, &direct_decode_error,
                     sizeof(direct_decode_error)) == 0 &&
              first_status.declaration_error.code == 0 &&
              qxp_allocator_state.calls == 1 &&
              qxp_allocator_state.releases == 1 &&
              !qxp_allocator_state.live,
              "exact nested decoder error propagation and rollback");
    memset(&second_status, 0x55, sizeof(second_status));
    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 62, bad_tokens, sizeof(bad_tokens), &qxp_allocator,
        &function, &second_status);
    qxp_check(!result && !function &&
              memcmp(&first_status, &second_status,
                     sizeof(first_status)) == 0,
              "deterministic decoder failure status");

    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 63, qxp_minimal_ps, sizeof(qxp_minimal_ps),
        &qxp_allocator, &function, &first_status);
    qxp_check(!result && !function &&
              first_status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE &&
              first_status.decode_error.code == 0 &&
              !qxp_allocator_state.live,
              "valid ps_2_0 explicitly rejected as vertex function");

    qxp_reset_allocator(1);
    declaration = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexDeclaration(
        &owner, 65, qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements),
        &qxp_allocator, &declaration, &first_status);
    qxp_check(!result && !declaration &&
              first_status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&first_status) &&
              qxp_allocator_state.calls == 1 && !qxp_allocator_state.live,
              "declaration every-allocation failure rollback");

    for (failure = 1; failure <= 2; ++failure) {
        qxp_reset_allocator(failure);
        function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
        result = QxpD3d9PrepareVertexFunction(
            &owner, 65, qxp_minimal_vs, sizeof(qxp_minimal_vs),
            &qxp_allocator, &function, &first_status);
        qxp_check(!result && !function &&
                  first_status.code ==
                      QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
                  QxpD3d9ShaderObjectStatusIsResourceFailure(
                      &first_status) &&
                  qxp_allocator_state.calls == failure &&
                  !qxp_allocator_state.live &&
                  !qxp_allocator_state.canary_failures,
                  "function every-allocation failure rollback");
    }
    qxp_reset_allocator(0);
}

static void qxp_test_prepare_vertex_sm1_profile(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    DWORD tokens[QXP_ARRAY_COUNT(qxp_minimal_vs11)];
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 19, 23, &device);
    qxp_reset_allocator(0);
    memcpy(tokens, qxp_minimal_vs11, sizeof(tokens));
    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 90, tokens, sizeof(tokens), &qxp_allocator, &function,
        &status);
    qxp_check(result && function && qxp_status_is_zero(&status) &&
              function->handle == 90 && !function->generation &&
              qxp_owner_is(&function->owner, &owner) &&
              function->summary.stage == QXP_SHADER_STAGE_VERTEX &&
              function->summary.version_token ==
                  QXP_SHADER_VERSION_VS_1_1 &&
              function->summary.token_count ==
                  QXP_ARRAY_COUNT(qxp_minimal_vs11) &&
              function->summary.instruction_count == 2 &&
              function->summary.declaration_count == 1 &&
              memcmp(function->bytecode, qxp_minimal_vs11,
                     sizeof(qxp_minimal_vs11)) == 0,
              "vs_1_1 function prepare and summary version token");
    if (function) {
        QxpD3d9ShaderObjectTableInitialize(&qxp_table);
        result = QxpD3d9PublishVertexFunction(
            &qxp_table, function, &status);
        qxp_check(result && function->generation != 0 &&
                  qxp_status_is_zero(&status),
                  "vs_1_1 prepared object passes publish re-check");
        qxp_check(QxpD3d9FindVertexFunction(&qxp_table, &owner, 90) ==
                      function,
                  "vs_1_1 function discoverable after publish");
        function = QxpD3d9DetachOwnerVertexFunction(&qxp_table, &owner);
        qxp_check(function != NULL, "vs_1_1 function detach");
        if (function)
            QxpD3d9DestroyVertexFunction(function, &qxp_allocator);
    }
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "vs_1_1 lifecycle allocation balance");

    /* ps_1_3 bytecode is not decodable, so the widened gate must still
     * reject it rather than admitting any non-vs_2_0 token. */
    memcpy(tokens, qxp_minimal_vs11, sizeof(tokens));
    tokens[0] = 0xffff0103UL;
    function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PrepareVertexFunction(
        &owner, 91, tokens, sizeof(tokens), &qxp_allocator, &function,
        &status);
    qxp_check(!result && !function &&
              status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE &&
              status.decode_error.code == QXP_SHADER_DECODE_VERSION &&
              !qxp_allocator_state.live,
              "ps_1_3 vertex function still rejected");
    qxp_reset_allocator(0);
}

static void qxp_test_pixel_prepare_and_failures(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel;
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_PIXEL_SHADER_GLSL_ERROR glsl_error;
    QXP_PIXEL_SHADER_GLSL_METADATA metadata;
    DWORD mutable_tokens[QXP_ARRAY_COUNT(qxp_minimal_ps)];
    DWORD bad_tokens[QXP_ARRAY_COUNT(qxp_minimal_ps)];
    DWORD source_bytes;
    DWORD failure;
    ULONG expected_bytes;
    BOOL direct_result;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 67, 69, &device);
    qxp_reset_allocator(0);
    memcpy(mutable_tokens, qxp_minimal_ps, sizeof(mutable_tokens));
    pixel = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PreparePixelFunction(
        &owner, 70, mutable_tokens, sizeof(mutable_tokens),
        &qxp_allocator, &pixel, &status);
    expected_bytes = (ULONG)sizeof(*pixel) + sizeof(mutable_tokens) -
                     (ULONG)sizeof(DWORD);
    memset(&decode_error, 0, sizeof(decode_error));
    direct_result = QxpDecodeShader20(
        qxp_minimal_ps, QXP_ARRAY_COUNT(qxp_minimal_ps),
        &qxp_direct_ir, &decode_error);
    memset(&metadata, 0, sizeof(metadata));
    memset(&glsl_error, 0, sizeof(glsl_error));
    source_bytes = 0;
    direct_result = direct_result && QxpEmitPixelShader20Glsl(
        &qxp_direct_ir, NULL, 0, &source_bytes, &metadata, &glsl_error);
    qxp_check(result && direct_result && pixel &&
              qxp_status_is_zero(&status) &&
              pixel->handle == 70 && !pixel->generation &&
              qxp_owner_is(&pixel->owner, &owner) &&
              pixel->bytecode_size == sizeof(mutable_tokens) &&
              pixel->glsl_source_bytes == source_bytes && source_bytes &&
              pixel->summary.stage == QXP_SHADER_STAGE_PIXEL &&
              pixel->summary.version_token == QXP_SHADER_VERSION_PS_2_0 &&
              pixel->summary.token_count ==
                  QXP_ARRAY_COUNT(qxp_minimal_ps) &&
              pixel->summary.instruction_count == 1 &&
              memcmp(&pixel->metadata, &metadata, sizeof(metadata)) == 0 &&
              pixel->metadata.color_output_mask == 1 &&
              qxp_allocator_state.calls == 2 &&
              qxp_allocator_state.releases == 1 &&
              qxp_allocator_state.live == 1 &&
              qxp_allocator_state.last_size == expected_bytes,
              "exact ps_2_0 prepare retains B0 summary and B2 count-only data");
    mutable_tokens[0] = 0;
    mutable_tokens[1] = 0;
    qxp_check(memcmp(pixel->bytecode, qxp_minimal_ps,
                     sizeof(qxp_minimal_ps)) == 0,
              "pixel prepare deep-copies transient bytecode");
    QxpD3d9DestroyPixelFunction(pixel, &qxp_allocator);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures &&
              qxp_allocator_state.calls == 2 &&
              qxp_allocator_state.releases == 2,
              "pixel prepare frees IR before compact object and balances");

    pixel = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PreparePixelFunction(
        &owner, 71, qxp_minimal_vs, sizeof(qxp_minimal_vs),
        &qxp_allocator, &pixel, &status);
    qxp_check(!result && !pixel && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE &&
              status.decode_error.code == 0 && status.glsl_error.code == 0 &&
              !qxp_allocator_state.live,
              "pixel prepare rejects exact vs_2_0 profile");

    memcpy(bad_tokens, qxp_minimal_ps, sizeof(bad_tokens));
    bad_tokens[1] = QXP_TEST_INST(0x1234UL, 0);
    memset(&decode_error, 0, sizeof(decode_error));
    direct_result = QxpDecodeShader20(
        bad_tokens, QXP_ARRAY_COUNT(bad_tokens), &qxp_direct_ir,
        &decode_error);
    pixel = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9PreparePixelFunction(
        &owner, 72, bad_tokens, sizeof(bad_tokens), &qxp_allocator,
        &pixel, &status);
    qxp_check(!direct_result && !result && !pixel &&
              status.code == QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE &&
              status.byte_offset ==
                  decode_error.token_offset * sizeof(DWORD) &&
              memcmp(&status.decode_error, &decode_error,
                     sizeof(decode_error)) == 0 &&
              status.glsl_error.code == 0 && !qxp_allocator_state.live,
              "pixel prepare propagates exact nested decoder failure");

    for (failure = 1; failure <= 2; ++failure) {
        qxp_reset_allocator(failure);
        pixel = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
        result = QxpD3d9PreparePixelFunction(
            &owner, 74, qxp_minimal_ps, sizeof(qxp_minimal_ps),
            &qxp_allocator, &pixel, &status);
        qxp_check(!result && !pixel && status.code ==
                      QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
                  QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
                  qxp_allocator_state.calls == failure &&
                  !qxp_allocator_state.live &&
                  !qxp_allocator_state.canary_failures,
                  "pixel prepare every-allocation rollback");
    }
    qxp_reset_allocator(0);
}

static QXP_D3D9_VERTEX_DECLARATION_OBJECT *qxp_prepare_declaration(
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = NULL;
    result = QxpD3d9PrepareVertexDeclaration(
        owner, handle, qxp_valid_elements,
        QXP_ARRAY_COUNT(qxp_valid_elements), &qxp_allocator, &object,
        &status);
    qxp_check(result && object && qxp_status_is_zero(&status),
              "lifecycle declaration prepare helper");
    return object;
}

static QXP_D3D9_VERTEX_FUNCTION_OBJECT *qxp_prepare_function(
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = NULL;
    result = QxpD3d9PrepareVertexFunction(
        owner, handle, qxp_minimal_vs, sizeof(qxp_minimal_vs),
        &qxp_allocator, &object, &status);
    qxp_check(result && object && qxp_status_is_zero(&status),
              "lifecycle function prepare helper");
    return object;
}

static QXP_D3D9_PIXEL_FUNCTION_OBJECT *qxp_prepare_pixel_function(
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = NULL;
    result = QxpD3d9PreparePixelFunction(
        owner, handle, qxp_minimal_ps, sizeof(qxp_minimal_ps),
        &qxp_allocator, &object, &status);
    qxp_check(result && object && qxp_status_is_zero(&status),
              "lifecycle pixel function prepare helper");
    return object;
}

static QXP_D3D9_VERTEX_DECLARATION_OBJECT *qxp_publish_declaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = qxp_prepare_declaration(owner, handle);
    if (!object) return NULL;
    result = QxpD3d9PublishVertexDeclaration(table, object, &status);
    qxp_check(result && object->generation != 0 &&
              qxp_status_is_zero(&status),
              "lifecycle declaration publish helper");
    if (!result) {
        QxpD3d9DestroyVertexDeclaration(object, &qxp_allocator);
        return NULL;
    }
    return object;
}

static QXP_D3D9_VERTEX_FUNCTION_OBJECT *qxp_publish_function(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle)
{
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = qxp_prepare_function(owner, handle);
    if (!object) return NULL;
    result = QxpD3d9PublishVertexFunction(table, object, &status);
    qxp_check(result && object->generation != 0 &&
              qxp_status_is_zero(&status),
              "lifecycle function publish helper");
    if (!result) {
        QxpD3d9DestroyVertexFunction(object, &qxp_allocator);
        return NULL;
    }
    return object;
}

static QXP_D3D9_PIXEL_FUNCTION_OBJECT *qxp_publish_pixel_function(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle)
{
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    object = qxp_prepare_pixel_function(owner, handle);
    if (!object) return NULL;
    result = QxpD3d9PublishPixelFunction(table, object, &status);
    qxp_check(result && object->generation != 0 &&
              qxp_status_is_zero(&status),
              "lifecycle pixel function publish helper");
    if (!result) {
        QxpD3d9DestroyPixelFunction(object, &qxp_allocator);
        return NULL;
    }
    return object;
}

static void qxp_destroy_all_for_owner(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel_function;

    for (;;) {
        declaration = QxpD3d9DetachOwnerVertexDeclaration(table, owner);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);
    }
    for (;;) {
        function = QxpD3d9DetachOwnerVertexFunction(table, owner);
        if (!function) break;
        QxpD3d9DestroyVertexFunction(function, &qxp_allocator);
    }
    for (;;) {
        pixel_function = QxpD3d9DetachOwnerPixelFunction(table, owner);
        if (!pixel_function) break;
        QxpD3d9DestroyPixelFunction(pixel_function, &qxp_allocator);
    }
}

static void qxp_test_namespaces_owners_bindings_and_snapshots(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *first_declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *second_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *first_function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *second_function;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_BINDING saved_binding;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_GUARDED_DECLARATION_SNAPSHOT declaration_snapshot;
    QXP_GUARDED_FUNCTION_SNAPSHOT function_snapshot;
    QXP_GUARDED_BYTECODE bytecode;
    DWORD legacy_fvf;
    DWORD saved_fvf;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 71, 73, &first_device);
    qxp_initialize_owner(&second_owner, 71, 73, &second_device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    first_declaration = qxp_publish_declaration(
        &qxp_table, &first_owner, 81);
    first_function = qxp_publish_function(&qxp_table, &first_owner, 81);
    second_declaration = qxp_publish_declaration(
        &qxp_table, &second_owner, 81);
    second_function = qxp_publish_function(&qxp_table, &second_owner, 81);
    qxp_check(first_declaration && first_function && second_declaration &&
              second_function &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 2 &&
              QxpD3d9VertexFunctionCount(&qxp_table) == 2 &&
              first_declaration->generation != first_function->generation &&
              first_declaration->generation !=
                  second_declaration->generation,
              "separate namespaces, owners, and shared generations");
    qxp_check(QxpD3d9FindVertexDeclaration(
                  &qxp_table, &first_owner, 81) == first_declaration &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &second_owner, 81) == second_declaration &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &first_owner, 81) == first_function &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &second_owner, 81) == second_function,
              "owner-scoped find in separate namespaces");

    qxp_publish_declaration(&qxp_table, &second_owner, 83);
    qxp_publish_function(&qxp_table, &second_owner, 82);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    legacy_fvf = 0;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 81, &declaration_binding, &legacy_fvf,
        &status);
    saved_binding = declaration_binding;
    qxp_check(result && declaration_binding.handle == 81 &&
              declaration_binding.generation ==
                  first_declaration->generation && !legacy_fvf,
              "object declaration set binds generation and clears FVF");
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 999, &declaration_binding, &legacy_fvf,
        &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              declaration_binding.handle == saved_binding.handle &&
              declaration_binding.generation == saved_binding.generation,
              "missing declaration set preserves prior binding");
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 83, &declaration_binding, &legacy_fvf,
        &status);
    qxp_check(!result && declaration_binding.handle ==
                  saved_binding.handle &&
              declaration_binding.generation == saved_binding.generation &&
              !legacy_fvf,
              "foreign declaration set preserves object binding and FVF");
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 0x11223344UL,
        &declaration_binding, &legacy_fvf, &status);
    qxp_check(result && !declaration_binding.handle &&
              !declaration_binding.generation &&
              legacy_fvf == 0x11223344UL,
              "even declaration set records raw legacy FVF");
    saved_binding = declaration_binding;
    saved_fvf = legacy_fvf;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 999, &declaration_binding, &legacy_fvf,
        &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              declaration_binding.handle == saved_binding.handle &&
              declaration_binding.generation == saved_binding.generation &&
              legacy_fvf == saved_fvf,
              "missing object preserves legacy FVF atomically");
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 81, &declaration_binding, &legacy_fvf,
        &status);
    qxp_check(result && declaration_binding.handle == 81 && !legacy_fvf,
              "object declaration replaces legacy FVF atomically");
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 0, &declaration_binding, &legacy_fvf,
        &status);
    qxp_check(result && !declaration_binding.handle &&
              !declaration_binding.generation && !legacy_fvf,
              "zero declaration set clears object and legacy FVF");

    memset(&function_binding, 0, sizeof(function_binding));
    result = QxpD3d9SetVertexFunction(
        &qxp_table, &first_owner, 81, &function_binding, &status);
    saved_binding = function_binding;
    qxp_check(result && function_binding.generation ==
                  first_function->generation,
              "set existing function");
    result = QxpD3d9SetVertexFunction(
        &qxp_table, &first_owner, 82, &function_binding, &status);
    qxp_check(!result && function_binding.handle == saved_binding.handle &&
              function_binding.generation == saved_binding.generation,
              "foreign function set preserves prior binding");
    result = QxpD3d9SetVertexFunction(
        &qxp_table, &first_owner, 999, &function_binding, &status);
    qxp_check(!result && function_binding.handle == saved_binding.handle &&
              function_binding.generation == saved_binding.generation,
              "missing function set preserves prior binding");

    memset(&declaration_snapshot, 0xcc, sizeof(declaration_snapshot));
    qxp_prepare_snapshot_guards(
        declaration_snapshot.before, declaration_snapshot.after,
        QXP_ARRAY_COUNT(declaration_snapshot.before));
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 81, &declaration_binding, &legacy_fvf,
        &status);
    result = result && QxpD3d9SnapshotVertexDeclaration(
        &qxp_table, &first_owner, &declaration_binding,
        &declaration_snapshot.value, &status);
    qxp_check(result && declaration_snapshot.value.binding.handle == 81 &&
              declaration_snapshot.value.binding.generation ==
                  first_declaration->generation &&
              declaration_snapshot.value.declaration.valid &&
              qxp_owner_is(&declaration_snapshot.value.owner, &first_owner) &&
              qxp_snapshot_guards_ok(
                  declaration_snapshot.before, declaration_snapshot.after,
                  QXP_ARRAY_COUNT(declaration_snapshot.before)),
              "generation-checked declaration snapshot and canaries");

    memset(&function_snapshot, 0xcc, sizeof(function_snapshot));
    memset(&bytecode, 0xee, sizeof(bytecode));
    qxp_prepare_snapshot_guards(
        function_snapshot.before, function_snapshot.after,
        QXP_ARRAY_COUNT(function_snapshot.before));
    qxp_prepare_snapshot_guards(
        bytecode.before, bytecode.after, QXP_ARRAY_COUNT(bytecode.before));
    result = QxpD3d9SnapshotVertexFunction(
        &qxp_table, &first_owner, &function_binding,
        &function_snapshot.value, bytecode.value, sizeof(bytecode.value),
        &status);
    qxp_check(result && function_snapshot.value.binding.handle == 81 &&
              function_snapshot.value.bytecode_size ==
                  sizeof(qxp_minimal_vs) &&
              function_snapshot.value.summary.stage ==
                  QXP_SHADER_STAGE_VERTEX &&
              memcmp(bytecode.value, qxp_minimal_vs,
                     sizeof(qxp_minimal_vs)) == 0 &&
              qxp_snapshot_guards_ok(
                  function_snapshot.before, function_snapshot.after,
                  QXP_ARRAY_COUNT(function_snapshot.before)) &&
              qxp_snapshot_guards_ok(
                  bytecode.before, bytecode.after,
                  QXP_ARRAY_COUNT(bytecode.before)),
              "function metadata and exact-bytecode snapshot canaries");
    memset(&function_snapshot.value, 0xaa,
           sizeof(function_snapshot.value));
    result = QxpD3d9SnapshotVertexFunction(
        &qxp_table, &first_owner, &function_binding,
        &function_snapshot.value, bytecode.value, sizeof(DWORD), &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_OUTPUT_BUFFER &&
              !function_snapshot.value.binding.handle &&
              !function_snapshot.value.binding.generation &&
              !function_snapshot.value.bytecode_size,
              "short function snapshot buffer zeroes fixed output");

    QxpD3d9ClearShaderBinding(&declaration_binding);
    memset(&declaration_snapshot.value, 0xaa,
           sizeof(declaration_snapshot.value));
    result = QxpD3d9SnapshotVertexDeclaration(
        &qxp_table, &first_owner, &declaration_binding,
        &declaration_snapshot.value, &status);
    qxp_check(result && !declaration_snapshot.value.binding.handle &&
              !declaration_snapshot.value.binding.generation &&
              !declaration_snapshot.value.declaration.valid,
              "unbound declaration snapshot is all zero");
    result = QxpD3d9SetVertexFunction(
        &qxp_table, &first_owner, 0, &function_binding, &status);
    memset(&function_snapshot.value, 0xaa,
           sizeof(function_snapshot.value));
    result = result && QxpD3d9SnapshotVertexFunction(
        &qxp_table, &first_owner, &function_binding,
        &function_snapshot.value, NULL, 0, &status);
    qxp_check(result && !function_snapshot.value.binding.handle &&
              !function_snapshot.value.binding.generation &&
              !function_snapshot.value.bytecode_size,
              "zero function set and unbound metadata snapshot");

    qxp_destroy_all_for_owner(&qxp_table, &first_owner);
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);
    qxp_check(!QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !QxpD3d9VertexFunctionCount(&qxp_table) &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "namespace/owner test cleanup");
}

static void qxp_test_delete_recreate_and_aba(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *detached_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *detached_function;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_BINDING stale_declaration;
    QXP_D3D9_SHADER_BINDING stale_function;
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT declaration_snapshot;
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT function_snapshot;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONGLONG first_declaration_generation;
    ULONGLONG first_function_generation;
    DWORD legacy_fvf;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 91, 93, &device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    declaration = qxp_publish_declaration(&qxp_table, &owner, 101);
    function = qxp_publish_function(&qxp_table, &owner, 101);
    first_declaration_generation = declaration->generation;
    first_function_generation = function->generation;
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &owner, 101, &declaration_binding, &legacy_fvf,
        &status);
    result = result && QxpD3d9SetVertexFunction(
        &qxp_table, &owner, 101, &function_binding, &status);
    stale_declaration = declaration_binding;
    stale_function = function_binding;
    detached_declaration =
        (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = result && QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 101, &declaration_binding,
        &detached_declaration, &status);
    qxp_check(result && detached_declaration == declaration &&
              !declaration_binding.handle &&
              !declaration_binding.generation &&
              !QxpD3d9FindVertexDeclaration(&qxp_table, &owner, 101),
              "delete current declaration clears exact binding");
    QxpD3d9DestroyVertexDeclaration(
        detached_declaration, &qxp_allocator);
    detached_function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeleteVertexFunction(
        &qxp_table, &owner, 101, &function_binding, &detached_function,
        &status);
    qxp_check(result && detached_function == function &&
              !function_binding.handle && !function_binding.generation,
              "delete current function clears exact binding");
    QxpD3d9DestroyVertexFunction(detached_function, &qxp_allocator);

    detached_declaration =
        (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 101, &declaration_binding,
        &detached_declaration, &status);
    qxp_check(result && !detached_declaration && qxp_status_is_zero(&status),
              "missing declaration delete is idempotent success");
    detached_function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeleteVertexFunction(
        &qxp_table, &owner, 101, &function_binding, &detached_function,
        &status);
    qxp_check(result && !detached_function && qxp_status_is_zero(&status),
              "missing function delete is idempotent success");
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 0, &declaration_binding,
        &detached_declaration, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE &&
              !detached_declaration,
              "zero declaration delete invalid");
    result = QxpD3d9DeleteVertexFunction(
        &qxp_table, &owner, 0, &function_binding, &detached_function,
        &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE &&
              !detached_function,
              "zero function delete invalid");
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 100, &declaration_binding,
        &detached_declaration, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE &&
              !detached_declaration,
              "even declaration delete rejects legacy FVF values");

    declaration = qxp_publish_declaration(&qxp_table, &owner, 101);
    function = qxp_publish_function(&qxp_table, &owner, 101);
    qxp_check(declaration->generation != first_declaration_generation &&
              function->generation != first_function_generation &&
              !declaration_binding.handle && !function_binding.handle,
              "handle recreation gets new generations and stays unbound");
    memset(&declaration_snapshot, 0xaa, sizeof(declaration_snapshot));
    result = QxpD3d9SnapshotVertexDeclaration(
        &qxp_table, &owner, &stale_declaration, &declaration_snapshot,
        &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !declaration_snapshot.binding.handle,
              "stale declaration binding cannot alias recreation");
    memset(&function_snapshot, 0xaa, sizeof(function_snapshot));
    result = QxpD3d9SnapshotVertexFunction(
        &qxp_table, &owner, &stale_function, &function_snapshot,
        NULL, 0, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !function_snapshot.binding.handle,
              "stale function binding cannot alias recreation");

    declaration_binding = stale_declaration;
    detached_declaration = NULL;
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 101, &declaration_binding,
        &detached_declaration, &status);
    qxp_check(result && detached_declaration == declaration &&
              declaration_binding.handle == stale_declaration.handle &&
              declaration_binding.generation ==
                  stale_declaration.generation,
              "delete only clears the exact current generation binding");
    QxpD3d9DestroyVertexDeclaration(detached_declaration, &qxp_allocator);
    function_binding = stale_function;
    detached_function = NULL;
    result = QxpD3d9DeleteVertexFunction(
        &qxp_table, &owner, 101, &function_binding, &detached_function,
        &status);
    qxp_check(result && detached_function == function &&
              function_binding.handle == stale_function.handle &&
              function_binding.generation == stale_function.generation,
              "function delete is generation exact");
    QxpD3d9DestroyVertexFunction(detached_function, &qxp_allocator);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "delete/recreate cleanup");
}

static void qxp_test_foreign_delete_isolation(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *detached_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *detached_function;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 111, 113, &first_device);
    qxp_initialize_owner(&second_owner, 111, 113, &second_device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    declaration = qxp_publish_declaration(
        &qxp_table, &second_owner, 121);
    function = qxp_publish_function(&qxp_table, &second_owner, 121);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    detached_declaration =
        (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &first_owner, 121, &declaration_binding,
        &detached_declaration, &status);
    detached_function = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = result && QxpD3d9DeleteVertexFunction(
        &qxp_table, &first_owner, 121, &function_binding,
        &detached_function, &status);
    qxp_check(result && !detached_declaration && !detached_function &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &second_owner, 121) == declaration &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &second_owner, 121) == function,
              "foreign missing-delete success cannot touch another owner");
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);
    qxp_check(!qxp_allocator_state.live,
              "foreign delete isolation cleanup");
}

static void qxp_test_duplicate_atomicity(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *installed_declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *duplicate_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *installed_function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *duplicate_function;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONGLONG clock_before;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 131, 137, &first_device);
    qxp_initialize_owner(&second_owner, 131, 137, &second_device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    installed_declaration = qxp_publish_declaration(
        &qxp_table, &first_owner, 141);
    clock_before = qxp_table.generation_clock;
    duplicate_declaration = qxp_prepare_declaration(&first_owner, 141);
    result = QxpD3d9PublishVertexDeclaration(
        &qxp_table, duplicate_declaration, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE &&
              !duplicate_declaration->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 1 &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &first_owner, 141) == installed_declaration,
              "declaration duplicate publication atomicity");
    QxpD3d9DestroyVertexDeclaration(
        duplicate_declaration, &qxp_allocator);
    qxp_check(qxp_publish_declaration(
                  &qxp_table, &second_owner, 141) != NULL,
              "same declaration handle under another owner");

    installed_function = qxp_publish_function(
        &qxp_table, &first_owner, 141);
    clock_before = qxp_table.generation_clock;
    duplicate_function = qxp_prepare_function(&first_owner, 141);
    result = QxpD3d9PublishVertexFunction(
        &qxp_table, duplicate_function, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE &&
              !duplicate_function->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9VertexFunctionCount(&qxp_table) == 1 &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &first_owner, 141) == installed_function,
              "function duplicate publication atomicity");
    QxpD3d9DestroyVertexFunction(duplicate_function, &qxp_allocator);
    qxp_check(qxp_publish_function(
                  &qxp_table, &second_owner, 141) != NULL,
              "same function handle under another owner");
    qxp_destroy_all_for_owner(&qxp_table, &first_owner);
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "duplicate atomicity cleanup");
}

static ULONGLONG qxp_max_generation(void)
{
    ULONGLONG value;

    value = (ULONGLONG)0xffffffffUL;
    return (value << 32) | value;
}

static void qxp_test_generation_exhaustion(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *prepared_declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *detached_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *prepared_function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *detached_function;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_BINDING stale_declaration;
    QXP_D3D9_SHADER_BINDING stale_function;
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT declaration_snapshot;
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT function_snapshot;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    DWORD legacy_fvf;
    ULONG function_bytes;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 151, 157, &device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    declaration = qxp_publish_declaration(&qxp_table, &owner, 161);
    function = qxp_publish_function(&qxp_table, &owner, 161);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &owner, 161, &declaration_binding, &legacy_fvf,
        &status);
    result = result && QxpD3d9SetVertexFunction(
        &qxp_table, &owner, 161, &function_binding, &status);
    stale_declaration = declaration_binding;
    stale_function = function_binding;
    qxp_check(result && stale_declaration.generation == (ULONGLONG)1 &&
              stale_function.generation == (ULONGLONG)2,
              "generation exhaustion stale binding fixture");

    detached_declaration = NULL;
    result = QxpD3d9DeleteVertexDeclaration(
        &qxp_table, &owner, 161, &declaration_binding,
        &detached_declaration, &status);
    detached_function = NULL;
    result = result && QxpD3d9DeleteVertexFunction(
        &qxp_table, &owner, 161, &function_binding, &detached_function,
        &status);
    qxp_check(result && detached_declaration == declaration &&
              detached_function == function &&
              !QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !QxpD3d9VertexFunctionCount(&qxp_table),
              "generation exhaustion deletes original objects");
    QxpD3d9DestroyVertexDeclaration(detached_declaration, &qxp_allocator);
    QxpD3d9DestroyVertexFunction(detached_function, &qxp_allocator);

    qxp_table.generation_clock = qxp_max_generation();
    prepared_declaration = qxp_prepare_declaration(&owner, 161);
    prepared_function = qxp_prepare_function(&owner, 161);
    function_bytes = (ULONG)sizeof(*prepared_function) +
        prepared_function->bytecode_size - (ULONG)sizeof(DWORD);
    memcpy(&qxp_table_copy, &qxp_table, sizeof(qxp_table_copy));
    memcpy(qxp_object_copy, prepared_declaration,
           sizeof(*prepared_declaration));

    result = QxpD3d9PublishVertexDeclaration(
        &qxp_table, prepared_declaration, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              memcmp(&qxp_table, &qxp_table_copy,
                     sizeof(qxp_table_copy)) == 0 &&
              memcmp(prepared_declaration, qxp_object_copy,
                     sizeof(*prepared_declaration)) == 0 &&
              !QxpD3d9FindVertexDeclaration(&qxp_table, &owner, 161),
              "declaration publication permanently exhausts atomically");
    memcpy(qxp_object_copy, prepared_function, function_bytes);
    result = QxpD3d9PublishVertexFunction(
        &qxp_table, prepared_function, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              memcmp(&qxp_table, &qxp_table_copy,
                     sizeof(qxp_table_copy)) == 0 &&
              memcmp(prepared_function, qxp_object_copy,
                     function_bytes) == 0 &&
              !QxpD3d9FindVertexFunction(&qxp_table, &owner, 161),
              "function publication permanently exhausts atomically");

    memset(&declaration_snapshot, 0xaa, sizeof(declaration_snapshot));
    result = QxpD3d9SnapshotVertexDeclaration(
        &qxp_table, &owner, &stale_declaration, &declaration_snapshot,
        &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !declaration_snapshot.binding.handle,
              "generation-one stale declaration cannot resolve at max");
    memset(&function_snapshot, 0xaa, sizeof(function_snapshot));
    result = QxpD3d9SnapshotVertexFunction(
        &qxp_table, &owner, &stale_function, &function_snapshot,
        NULL, 0, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !function_snapshot.binding.handle,
              "stale function cannot resolve at generation max");
    QxpD3d9DestroyVertexDeclaration(
        prepared_declaration, &qxp_allocator);
    QxpD3d9DestroyVertexFunction(prepared_function, &qxp_allocator);
    qxp_check(qxp_table.generation_clock == qxp_max_generation() &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "generation exhaustion leak-free cleanup");
}

static void qxp_test_owner_teardown(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    DWORD declaration_count;
    DWORD function_count;
    DWORD legacy_fvf;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 171, 173, &first_device);
    qxp_initialize_owner(&second_owner, 171, 173, &second_device);
    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_declaration(&qxp_table, &first_owner, 1);
    qxp_publish_declaration(&qxp_table, &first_owner, 3);
    qxp_publish_declaration(&qxp_table, &first_owner, 5);
    qxp_publish_function(&qxp_table, &first_owner, 1);
    qxp_publish_function(&qxp_table, &first_owner, 2);
    qxp_publish_declaration(&qxp_table, &second_owner, 1);
    qxp_publish_declaration(&qxp_table, &second_owner, 3);
    qxp_publish_function(&qxp_table, &second_owner, 1);
    qxp_publish_function(&qxp_table, &second_owner, 2);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &first_owner, 3, &declaration_binding, &legacy_fvf,
        &status);
    result = result && QxpD3d9SetVertexFunction(
        &qxp_table, &first_owner, 2, &function_binding, &status);
    QxpD3d9ClearShaderBinding(&declaration_binding);
    QxpD3d9ClearShaderBinding(&function_binding);
    legacy_fvf = 0;
    qxp_check(result && !declaration_binding.handle &&
              !function_binding.handle && !legacy_fvf,
              "context state clears before owner teardown");

    qxp_reset_lock_state();
    result = QxpD3d9DestroyOwnerShaderObjects(
        &qxp_table, &first_owner, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state,
        &declaration_count, &function_count, &status);
    qxp_check(result && qxp_status_is_zero(&status) &&
              declaration_count == 3 && function_count == 2 &&
              qxp_lock_contract_ok(7) &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 2 &&
              QxpD3d9VertexFunctionCount(&qxp_table) == 2 &&
              !QxpD3d9FindVertexDeclaration(
                  &qxp_table, &first_owner, 1) &&
              !QxpD3d9FindVertexFunction(&qxp_table, &first_owner, 1) &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &second_owner, 1) != NULL &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &second_owner, 1) != NULL,
              "shared teardown frees exact owner outside balanced locks");

    qxp_reset_lock_state();
    result = QxpD3d9DestroyOwnerShaderObjects(
        &qxp_table, &second_owner, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state,
        &declaration_count, &function_count, &status);
    qxp_check(result && declaration_count == 2 && function_count == 2 &&
              qxp_lock_contract_ok(6) &&
              !QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !QxpD3d9VertexFunctionCount(&qxp_table) &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "shared teardown covers both namespaces with canaries");
}

static void qxp_test_table_exhaustion(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONGLONG clock_before;
    DWORD handle;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 181, 191, &device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    for (handle = 1; handle <= QXP_D3D9_SHADER_OBJECT_CAPACITY; ++handle)
        qxp_publish_declaration(
            &qxp_table, &owner, handle * 2UL - 1UL);
    qxp_check(QxpD3d9VertexDeclarationCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "declaration table exact capacity");
    clock_before = qxp_table.generation_clock;
    declaration = qxp_prepare_declaration(
        &owner, QXP_D3D9_SHADER_OBJECT_CAPACITY * 2UL + 1UL);
    result = QxpD3d9PublishVertexDeclaration(
        &qxp_table, declaration, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              !declaration->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9VertexDeclarationCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "declaration table exhaustion is atomic resource failure");
    QxpD3d9DestroyVertexDeclaration(declaration, &qxp_allocator);

    function = qxp_publish_function(&qxp_table, &owner, 1);
    qxp_check(function != NULL &&
              QxpD3d9VertexFunctionCount(&qxp_table) == 1,
              "full declaration table cannot consume function namespace");
    for (handle = 2; handle <= QXP_D3D9_SHADER_OBJECT_CAPACITY; ++handle)
        qxp_publish_function(&qxp_table, &owner, handle);
    qxp_check(QxpD3d9VertexFunctionCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "function table exact capacity");
    clock_before = qxp_table.generation_clock;
    function = qxp_prepare_function(
        &owner, QXP_D3D9_SHADER_OBJECT_CAPACITY + 1UL);
    result = QxpD3d9PublishVertexFunction(
        &qxp_table, function, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL &&
              !function->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9VertexFunctionCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "function table exhaustion is atomic resource failure");
    QxpD3d9DestroyVertexFunction(function, &qxp_allocator);
    qxp_destroy_all_for_owner(&qxp_table, &owner);
    qxp_check(!QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !QxpD3d9VertexFunctionCount(&qxp_table) &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "table exhaustion complete leak-free cleanup");
}

static void qxp_test_batch_success_paths(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG declaration_bytes;
    ULONG function_bytes;
    ULONG handle_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    DWORD legacy_fvf;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 211, 223, &device);
    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;

    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    declaration_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 201,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    declaration_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), declaration_bytes, 203,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION, 2,
        qxp_batch_frame, declaration_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x40UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == declaration_bytes &&
              !error_offset && qxp_status_is_zero(&status) &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 2 &&
              qxp_lock_contract_ok(2),
              "batch create declaration success and exact advance");

    qxp_reset_lock_state();
    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    function_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 202,
        qxp_minimal_vs, sizeof(qxp_minimal_vs));
    function_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), function_bytes, 204,
        qxp_minimal_vs, sizeof(qxp_minimal_vs));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION, 2,
        qxp_batch_frame, function_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x44UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == function_bytes &&
              !error_offset && QxpD3d9VertexFunctionCount(&qxp_table) == 2 &&
              qxp_lock_contract_ok(2),
              "batch create function success outside balanced locks");

    qxp_reset_lock_state();
    handle_bytes = 0;
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 201);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes,
        0x11223344UL);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 203);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 3,
        qxp_batch_frame, handle_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x48UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == handle_bytes && !error_offset &&
              declaration_binding.handle == 203 &&
              declaration_binding.generation != 0 && !legacy_fvf &&
              qxp_lock_contract_ok(3),
              "batch declaration object-FVF-object transitions");

    qxp_reset_lock_state();
    handle_bytes = 0;
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 202);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 0);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 204);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION, 3,
        qxp_batch_frame, handle_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x4cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == handle_bytes && !error_offset &&
              function_binding.handle == 204 &&
              function_binding.generation != 0 &&
              qxp_lock_contract_ok(3),
              "batch function set-clear-set transitions");

    qxp_reset_lock_state();
    handle_bytes = 0;
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 201);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 203);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION, 2,
        qxp_batch_frame, handle_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x50UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == handle_bytes && !error_offset &&
              !QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !declaration_binding.handle && !declaration_binding.generation &&
              qxp_lock_contract_ok(2),
              "batch delete declaration detaches and frees outside locks");

    qxp_reset_lock_state();
    handle_bytes = 0;
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 202);
    handle_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), handle_bytes, 204);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION, 2,
        qxp_batch_frame, handle_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x54UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == handle_bytes && !error_offset &&
              !QxpD3d9VertexFunctionCount(&qxp_table) &&
              !function_binding.handle && !function_binding.generation &&
              qxp_lock_contract_ok(2) && !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "all six production batch paths balance locks and allocations");
}

static void qxp_test_production_draw_state_bridge(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_DRAW_STATE draw_state;
    QXP_D3D9_DRAW_STATE_STATUS draw_status;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG record_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 241, 251, &device);
    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    QxpD3d9DrawStateInitialize(&draw_state);
    qxp_publish_declaration(&qxp_table, &owner, 701);
    qxp_publish_function(&qxp_table, &owner, 702);
    qxp_check(
        !QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
        "initialized production draw state admits legacy execution");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 701);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x180UL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result && consumed_bytes == record_bytes && !error_offset &&
              draw_state.declaration_mode ==
                  QXP_D3D9_DRAW_DECLARATION_OBJECT &&
              draw_state.declaration_binding.handle == 701 &&
              QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
              "production bridge synchronizes private declaration SET");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 702);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x184UL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result && draw_state.vertex_function_binding.handle == 702 &&
              draw_state.vertex_function_binding.generation != 0,
              "production bridge synchronizes VS2 function SET");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 701);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x188UL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result && draw_state.declaration_mode ==
                  QXP_D3D9_DRAW_DECLARATION_NONE &&
              !draw_state.declaration_binding.handle &&
              draw_state.vertex_function_binding.handle == 702,
              "bound declaration DELETE derives NONE and preserves VS2");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 0);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x18cUL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result &&
              !QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
              "cleared production object state admits legacy execution");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 0x112UL);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x190UL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result && draw_state.declaration_mode ==
                  QXP_D3D9_DRAW_DECLARATION_FVF &&
              draw_state.legacy_fvf == 0x112UL &&
              !QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
              "production bridge admits exact legacy FVF mode");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 0);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x194UL,
        &consumed_bytes, &error_offset, &status);
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status) && result;
    qxp_check(result &&
              !QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
              "production bridge clear returns canonical NONE mode");

    qxp_publish_declaration(&qxp_table, &owner, 703);
    record_bytes = 0;
    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), record_bytes, 703);
    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), record_bytes, 705);
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 2,
        qxp_batch_frame, record_bytes, &owner,
        &draw_state.declaration_binding, &draw_state.legacy_fvf,
        &draw_state.vertex_function_binding, 0x198UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && consumed_bytes == QXP_D3D9_HANDLE_RECORD_BYTES &&
              error_offset == 0x198UL && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND,
              "production bridge fixture reaches later semantic failure");
    result = QxpD3d9DrawStateSynchronizeObjectBindings(
        &draw_state, &draw_status);
    qxp_check(result && draw_state.declaration_mode ==
                  QXP_D3D9_DRAW_DECLARATION_OBJECT &&
              draw_state.declaration_binding.handle == 703 &&
              QxpD3d9DrawStateRequiresUnsupportedExecution(&draw_state),
              "prefix failure synchronizes prior successful declaration");

    qxp_destroy_all_for_owner(&qxp_table, &owner);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "production draw-state bridge cleanup is leak-free");
}

static void qxp_test_batch_partial_and_truncation(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_BINDING saved_declaration;
    QXP_D3D9_SHADER_BINDING saved_function;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG first_bytes;
    ULONG total_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    DWORD legacy_fvf;
    DWORD saved_fvf;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 227, 229, &device);
    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;

    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    first_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 211,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    total_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 213,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION, 2,
        qxp_batch_frame, first_bytes + 7UL, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x84UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && total_bytes > first_bytes + 7UL &&
              status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER &&
              consumed_bytes == first_bytes && error_offset == 0x84UL &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 1 &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &owner, 211) != NULL &&
              qxp_lock_contract_ok(1),
              "malformed second declaration preserves first success");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 214,
        qxp_minimal_vs, sizeof(qxp_minimal_vs));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 216,
        qxp_minimal_vs, sizeof(qxp_minimal_vs));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x88UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD &&
              consumed_bytes == first_bytes && error_offset == 0x88UL &&
              QxpD3d9VertexFunctionCount(&qxp_table) == 1 &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &owner, 214) != NULL &&
              qxp_lock_contract_ok(1),
              "short second function payload preserves first success");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    first_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 221,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    total_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 221,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION, 2,
        qxp_batch_frame, total_bytes, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x8cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE &&
              consumed_bytes == first_bytes && error_offset == 0x8cUL &&
              QxpD3d9VertexDeclarationCount(&qxp_table) == 1 &&
              qxp_lock_contract_ok(2) && qxp_allocator_state.live == 1,
              "duplicate second create frees prepared object outside lock");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_declaration(&qxp_table, &owner, 225);
    qxp_publish_function(&qxp_table, &owner, 226);
    legacy_fvf = 0;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &owner, 225, &declaration_binding, &legacy_fvf,
        &status);
    result = result && QxpD3d9SetVertexFunction(
        &qxp_table, &owner, 226, &function_binding, &status);
    saved_declaration = declaration_binding;
    saved_function = function_binding;
    saved_fvf = legacy_fvf;
    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes,
        0x55667788UL);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 225);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x120UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD &&
              !consumed_bytes && error_offset == 0x120UL &&
              declaration_binding.handle == saved_declaration.handle &&
              declaration_binding.generation ==
                  saved_declaration.generation &&
              legacy_fvf == saved_fvf && qxp_lock_contract_ok(0),
              "fixed set truncation preflights before first mutation");

    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 226);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 999);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x124UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && !consumed_bytes && error_offset == 0x124UL &&
              function_binding.handle == saved_function.handle &&
              function_binding.generation == saved_function.generation &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &owner, 226) != NULL &&
              qxp_lock_contract_ok(0),
              "fixed delete truncation cannot detach first record");
    qxp_destroy_all_for_owner(&qxp_table, &owner);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "partial and truncation batch cleanup");
}

static void qxp_test_batch_failures_and_offsets(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_SHADER_BINDING declaration_binding;
    QXP_D3D9_SHADER_BINDING function_binding;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG record_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    DWORD legacy_fvf;
    DWORD handle;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 231, 233, &first_device);
    qxp_initialize_owner(&second_owner, 231, 233, &second_device);
    memset(&declaration_binding, 0, sizeof(declaration_binding));
    memset(&function_binding, 0, sizeof(function_binding));
    legacy_fvf = 0;

    qxp_reset_allocator(1);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    record_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 231,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x204UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              !consumed_bytes && error_offset == 0x204UL &&
              qxp_lock_contract_ok(0) && !qxp_allocator_state.live,
              "opcode 71 OOM reports outer nonzero command offset");

    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_declaration(&qxp_table, &second_owner, 233);
    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 2);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x208UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE &&
              !consumed_bytes && error_offset == 0x208UL &&
              qxp_lock_contract_ok(1),
              "opcode 72 even delete reports outer command offset");

    legacy_fvf = 0x55667788UL;
    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 233);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x20cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !consumed_bytes && error_offset == 0x20cUL &&
              !declaration_binding.handle &&
              legacy_fvf == 0x55667788UL && qxp_lock_contract_ok(1) &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &second_owner, 233) != NULL,
              "opcode 73 foreign odd set preserves state and owner");

    record_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 234,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x210UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE &&
              !consumed_bytes && error_offset == 0x210UL &&
              qxp_lock_contract_ok(0),
              "opcode 74 profile failure reports outer command offset");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 0);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x214UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE &&
              !consumed_bytes && error_offset == 0x214UL &&
              qxp_lock_contract_ok(1),
              "opcode 75 failure reports outer command offset");

    record_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 236);
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x218UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !consumed_bytes && error_offset == 0x218UL &&
              qxp_lock_contract_ok(1),
              "opcode 76 failure reports outer command offset");
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    for (handle = 1; handle <= QXP_D3D9_SHADER_OBJECT_CAPACITY; ++handle)
        qxp_publish_declaration(
            &qxp_table, &first_owner, handle * 2UL - 1UL);
    record_bytes = qxp_append_declaration_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0,
        QXP_D3D9_SHADER_OBJECT_CAPACITY * 2UL + 1UL,
        qxp_valid_elements, QXP_ARRAY_COUNT(qxp_valid_elements));
    qxp_reset_lock_state();
    result = qxp_process_batch(
        QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION, 1,
        qxp_batch_frame, record_bytes, &first_owner,
        &declaration_binding, &legacy_fvf, &function_binding, 0x220UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              !consumed_bytes && error_offset == 0x220UL &&
              QxpD3d9VertexDeclarationCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY &&
              qxp_lock_contract_ok(1) &&
              qxp_allocator_state.live ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "batch table-full publication rolls back outside lock");
    qxp_destroy_all_for_owner(&qxp_table, &first_owner);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "batch failure matrix leak-free cleanup");
}

static void qxp_test_pixel_lifecycle(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *vertex_function;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *pixel_function;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *second_pixel;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *detached;
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_SHADER_BINDING saved;
    QXP_D3D9_SHADER_BINDING stale;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_GUARDED_PIXEL_SNAPSHOT snapshot;
    QXP_GUARDED_BYTECODE bytecode;
    ULONGLONG first_generation;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 301, 307, &first_device);
    qxp_initialize_owner(&second_owner, 301, 307, &second_device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    declaration = qxp_publish_declaration(&qxp_table, &first_owner, 301);
    vertex_function = qxp_publish_function(
        &qxp_table, &first_owner, 301);
    pixel_function = qxp_publish_pixel_function(
        &qxp_table, &first_owner, 301);
    second_pixel = qxp_publish_pixel_function(
        &qxp_table, &second_owner, 301);
    qxp_publish_pixel_function(&qxp_table, &second_owner, 303);
    first_generation = pixel_function->generation;
    qxp_check(declaration && vertex_function && pixel_function &&
              second_pixel && declaration->generation == (ULONGLONG)1 &&
              vertex_function->generation == (ULONGLONG)2 &&
              pixel_function->generation == (ULONGLONG)3 &&
              second_pixel->generation == (ULONGLONG)4 &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 3 &&
              QxpD3d9FindVertexDeclaration(
                  &qxp_table, &first_owner, 301) == declaration &&
              QxpD3d9FindVertexFunction(
                  &qxp_table, &first_owner, 301) == vertex_function &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &first_owner, 301) == pixel_function &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &second_owner, 301) == second_pixel,
              "pixel namespace is owner-scoped and shares generation clock");

    memset(&binding, 0, sizeof(binding));
    result = QxpD3d9SetPixelFunction(
        &qxp_table, &first_owner, 301, &binding, &status);
    saved = binding;
    qxp_check(result && binding.handle == 301 &&
              binding.generation == first_generation,
              "pixel set binds exact generation");
    result = QxpD3d9SetPixelFunction(
        &qxp_table, &first_owner, 999, &binding, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              binding.handle == saved.handle &&
              binding.generation == saved.generation,
              "missing pixel set preserves prior binding");
    result = QxpD3d9SetPixelFunction(
        &qxp_table, &first_owner, 303, &binding, &status);
    qxp_check(!result && binding.handle == saved.handle &&
              binding.generation == saved.generation,
              "foreign pixel set preserves prior binding");

    memset(&snapshot, 0xcc, sizeof(snapshot));
    memset(&bytecode, 0xee, sizeof(bytecode));
    qxp_prepare_snapshot_guards(
        snapshot.before, snapshot.after,
        QXP_ARRAY_COUNT(snapshot.before));
    qxp_prepare_snapshot_guards(
        bytecode.before, bytecode.after, QXP_ARRAY_COUNT(bytecode.before));
    result = QxpD3d9SnapshotPixelFunction(
        &qxp_table, &first_owner, &binding, &snapshot.value,
        bytecode.value, sizeof(bytecode.value), &status);
    qxp_check(result && snapshot.value.binding.handle == 301 &&
              snapshot.value.binding.generation == first_generation &&
              snapshot.value.bytecode_size == sizeof(qxp_minimal_ps) &&
              snapshot.value.glsl_source_bytes ==
                  pixel_function->glsl_source_bytes &&
              snapshot.value.summary.stage == QXP_SHADER_STAGE_PIXEL &&
              snapshot.value.metadata.color_output_mask == 1 &&
              memcmp(bytecode.value, qxp_minimal_ps,
                     sizeof(qxp_minimal_ps)) == 0 &&
              qxp_snapshot_guards_ok(
                  snapshot.before, snapshot.after,
                  QXP_ARRAY_COUNT(snapshot.before)) &&
              qxp_snapshot_guards_ok(
                  bytecode.before, bytecode.after,
                  QXP_ARRAY_COUNT(bytecode.before)),
              "pixel scalar snapshot, bytecode, metadata, and canaries");
    memset(&snapshot.value, 0xaa, sizeof(snapshot.value));
    result = QxpD3d9SnapshotPixelFunction(
        &qxp_table, &first_owner, &binding, &snapshot.value,
        bytecode.value, sizeof(DWORD), &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_OUTPUT_BUFFER &&
              !snapshot.value.binding.handle &&
              !snapshot.value.binding.generation &&
              !snapshot.value.bytecode_size &&
              !snapshot.value.glsl_source_bytes,
              "short pixel snapshot capacity zeros scalar output");

    stale = binding;
    detached = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeletePixelFunction(
        &qxp_table, &first_owner, 301, &binding, &detached, &status);
    qxp_check(result && detached == pixel_function && !binding.handle &&
              !binding.generation &&
              !QxpD3d9FindPixelFunction(&qxp_table, &first_owner, 301),
              "pixel delete clears exact current binding and detaches");
    QxpD3d9DestroyPixelFunction(detached, &qxp_allocator);
    detached = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeletePixelFunction(
        &qxp_table, &first_owner, 301, &binding, &detached, &status);
    qxp_check(result && !detached && qxp_status_is_zero(&status),
              "missing pixel delete is idempotent");
    result = QxpD3d9DeletePixelFunction(
        &qxp_table, &first_owner, 0, &binding, &detached, &status);
    qxp_check(!result && !detached && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE,
              "zero pixel delete fails deterministically");

    pixel_function = qxp_publish_pixel_function(
        &qxp_table, &first_owner, 301);
    qxp_check(pixel_function->generation != first_generation,
              "pixel recreation receives a fresh generation");
    memset(&snapshot.value, 0xaa, sizeof(snapshot.value));
    result = QxpD3d9SnapshotPixelFunction(
        &qxp_table, &first_owner, &stale, &snapshot.value,
        NULL, 0, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              !snapshot.value.binding.handle,
              "stale pixel snapshot cannot alias recreation");
    binding = stale;
    detached = NULL;
    result = QxpD3d9DeletePixelFunction(
        &qxp_table, &first_owner, 301, &binding, &detached, &status);
    qxp_check(result && detached == pixel_function &&
              binding.handle == stale.handle &&
              binding.generation == stale.generation,
              "pixel delete clears binding only for exact generation");
    QxpD3d9DestroyPixelFunction(detached, &qxp_allocator);

    result = QxpD3d9SetPixelFunction(
        &qxp_table, &first_owner, 0, &binding, &status);
    memset(&snapshot.value, 0xaa, sizeof(snapshot.value));
    result = result && QxpD3d9SnapshotPixelFunction(
        &qxp_table, &first_owner, &binding, &snapshot.value,
        NULL, 0, &status);
    qxp_check(result && !binding.handle && !binding.generation &&
              !snapshot.value.binding.handle &&
              !snapshot.value.bytecode_size,
              "zero pixel set clears and snapshots all zero");

    qxp_destroy_all_for_owner(&qxp_table, &first_owner);
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);
    qxp_check(!QxpD3d9VertexDeclarationCount(&qxp_table) &&
              !QxpD3d9VertexFunctionCount(&qxp_table) &&
              !QxpD3d9PixelFunctionCount(&qxp_table) &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "pixel lifecycle leak-free cleanup");
}

static void qxp_mutate_pixel_prepared(
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object, DWORD mutation)
{
    if (!object) return;
    switch (mutation) {
    case 0:
        object->metadata.required_ps_temp_count =
            QXP_SHADER_MAX_PS_TEMPS + 1UL;
        break;
    case 1:
        object->metadata.required_ps20_caps = 0x80000000UL;
        break;
    case 2:
        object->metadata.required_ps_instruction_slots = 0;
        break;
    case 3:
        object->metadata.input_color_mask = 4;
        break;
    case 4:
        object->metadata.input_texture_mask = 0x100UL;
        break;
    case 5:
        object->metadata.input_centroid_mask = 0x400UL;
        break;
    case 6:
        object->metadata.input_color_mask = 1;
        object->metadata.input_color_components[0] = 15;
        break;
    case 7:
        object->metadata.input_texture_mask = 1;
        object->metadata.input_texture_components[0] = 15;
        object->metadata.input_centroid_mask = 1UL << 3;
        break;
    case 8:
        object->metadata.sampler_mask = 1UL << 16;
        break;
    case 9:
        object->metadata.color_output_mask = 2;
        break;
    case 10:
        object->metadata.writes_depth = (BOOL)2;
        break;
    case 11:
        object->metadata.uses_discard = (BOOL)2;
        break;
    case 12:
        object->metadata.input_color_mask = 1;
        object->metadata.input_centroid_mask = 1;
        break;
    case 13:
        object->metadata.input_color_components[0] = 1;
        break;
    case 14:
        object->metadata.input_texture_mask = 1;
        break;
    case 15:
        object->metadata.input_texture_components[0] = 1;
        break;
    case 16:
        object->metadata.sampler_mask = 1;
        break;
    case 17:
        object->metadata.sampler_types[0] = QXP_SHADER_SAMPLER_2D;
        break;
    case 18:
        object->metadata.sampler_mask = 1;
        object->metadata.sampler_types[0] = 0xffU;
        break;
    case 19:
        object->summary.block_count = 0;
        break;
    case 20:
        object->summary.flow_slot_count = 1;
        break;
    case 21:
        object->summary.slot_count = 0;
        object->summary.arithmetic_slot_count = 1;
        break;
    case 22:
        object->summary.slot_count = 1;
        object->summary.arithmetic_slot_count = 1;
        object->summary.texture_slot_count = 1;
        break;
    case 23:
        object->glsl_source_bytes = 0;
        break;
    case 24:
        object->glsl_source_bytes =
            QXP_PIXEL_SHADER_GLSL_MAX_SOURCE + 1UL;
        break;
    case 25:
        object->bytecode_size = 0;
        break;
    case 26:
        object->generation = 1;
        break;
    case 27:
        object->summary.version_token = QXP_SHADER_VERSION_VS_2_0;
        break;
    case 28:
        object->metadata.required_constant_count =
            QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT + 1UL;
        break;
    default:
        object->summary.token_count += 1;
        break;
    }
}

static void qxp_test_production_pixel_dispatch(void)
{
    QXP_TEST_PIXEL_DISPATCH_STATE dispatch_state;
    QXP_D3D9_PIXEL_DISPATCH_CALLBACKS callbacks;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG first_bytes;
    ULONG total_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    BOOL result;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.create = qxp_test_pixel_dispatch_create;
    callbacks.set = qxp_test_pixel_dispatch_set;
    callbacks.delete_function = qxp_test_pixel_dispatch_delete;
    callbacks.opaque = &dispatch_state;

    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 581,
        qxp_minimal_ps13, sizeof(qxp_minimal_ps13));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 582,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x340UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              qxp_status_is_zero(&status) &&
              dispatch_state.total_calls == 2 &&
              dispatch_state.create_calls == 2 &&
              dispatch_state.kinds[0] == 1 &&
              dispatch_state.kinds[1] == 1 &&
              dispatch_state.handles[0] == 581 &&
              dispatch_state.handles[1] == 582 &&
              dispatch_state.bytecode_sizes[0] ==
                  sizeof(qxp_minimal_ps13) &&
              dispatch_state.bytecode_sizes[1] == sizeof(qxp_minimal_ps) &&
              dispatch_state.versions[0] == D3DPS_VERSION(1, 3) &&
              dispatch_state.versions[1] == QXP_SHADER_VERSION_PS_2_0 &&
              !dispatch_state.ps20[0] && dispatch_state.ps20[1],
              "production CREATE dispatch classifies each framed profile");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    dispatch_state.fail_call = 2;
    dispatch_state.fail_code = QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION;
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x344UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && consumed_bytes == first_bytes &&
              error_offset == 0x344UL && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              dispatch_state.total_calls == 2 &&
              !dispatch_state.ps20[0] && dispatch_state.ps20[1],
              "production CREATE preserves prefix and resource status");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, first_bytes + 7UL, &callbacks, 0x348UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && consumed_bytes == first_bytes &&
              error_offset == 0x348UL && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER &&
              dispatch_state.total_calls == 1,
              "production CREATE short second header preserves first call");

    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 591);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 592);
    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &callbacks, 0x34cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && !consumed_bytes && error_offset == 0x34cUL &&
              status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD &&
              !dispatch_state.total_calls,
              "production fixed SET preflights before mutation");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    dispatch_state.fail_call = 2;
    dispatch_state.fail_code = QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND;
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x350UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && consumed_bytes == QXP_D3D9_HANDLE_RECORD_BYTES &&
              error_offset == 0x350UL && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              dispatch_state.set_calls == 2 &&
              dispatch_state.handles[0] == 591 &&
              dispatch_state.handles[1] == 592,
              "production SET reports outer offset after prior success");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    dispatch_state.fail_call = 1;
    dispatch_state.fail_code = QXP_D3D9_SHADER_OBJECT_ERROR_BINDING;
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x354UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && !consumed_bytes && error_offset == 0x354UL &&
              status.code == QXP_D3D9_SHADER_OBJECT_ERROR_BINDING &&
              dispatch_state.delete_calls == 1 &&
              dispatch_state.handles[0] == 591,
              "production DELETE propagates first semantic failure");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x358UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              dispatch_state.delete_calls == 2 &&
              dispatch_state.kinds[0] == 3 &&
              dispatch_state.kinds[1] == 3,
              "production DELETE dispatches exact fixed records");

    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        0xffU, 0, NULL, 0, &callbacks, 0x35cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && !consumed_bytes && error_offset == 0x35cUL &&
              status.code == QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE &&
              !dispatch_state.total_calls,
              "production dispatch rejects unknown opcode deterministically");
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION, 0, NULL, 0,
        &callbacks, 0x360UL, &consumed_bytes, &error_offset, &status);
    qxp_check(result && !consumed_bytes && !error_offset &&
              qxp_status_is_zero(&status),
              "production dispatch accepts empty standard batch");

    /* ps_1_1 lowers to GLSL, so it must classify native beside ps_2_0.
     * Only profiles the object table cannot represent stay legacy. */
    memset(qxp_batch_frame, 0, sizeof(qxp_batch_frame));
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 583,
        qxp_minimal_ps11, sizeof(qxp_minimal_ps11));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 584,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    memset(&dispatch_state, 0, sizeof(dispatch_state));
    result = QxpD3d9DispatchPixelFunctionBatch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &callbacks, 0x34cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              dispatch_state.create_calls == 2 &&
              dispatch_state.versions[0] == D3DPS_VERSION(1, 1) &&
              dispatch_state.versions[1] == QXP_SHADER_VERSION_PS_2_0 &&
              dispatch_state.ps20[0] && dispatch_state.ps20[1],
              "production CREATE routes ps_1_1 natively beside ps_2_0");
}

static void qxp_test_unified_pixel_routing(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *installed;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *foreign;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *detached;
    QXP_D3D9_PIXEL_BINDING_STATE binding;
    QXP_D3D9_PIXEL_BINDING_STATE saved;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    QXP_D3D9_PIXEL_FUNCTION_ROUTE route;
    ULONGLONG clock_before;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 331, 337, &first_device);
    qxp_initialize_owner(&second_owner, 331, 337, &second_device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(&binding, 0, sizeof(binding));

    prepared = qxp_prepare_pixel_function(&first_owner, 601);
    clock_before = qxp_table.generation_clock;
    result = QxpD3d9PublishPixelFunctionUnified(
        &qxp_table, prepared, TRUE, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE &&
              !prepared->generation &&
              qxp_table.generation_clock == clock_before &&
              !QxpD3d9PixelFunctionCount(&qxp_table),
              "legacy collision blocks SM2 publication atomically");
    result = QxpD3d9PublishPixelFunctionUnified(
        &qxp_table, prepared, FALSE, &status);
    installed = prepared;
    qxp_check(result && qxp_status_is_zero(&status) &&
              installed->generation == (ULONGLONG)1 &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &first_owner, 601) == installed,
              "unified publication transfers prepared SM2 object");

    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 601, FALSE, &binding, &route, &status);
    saved = binding;
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2 &&
              !binding.legacy_handle && binding.sm2.handle == 601 &&
              binding.sm2.generation == installed->generation &&
              QxpD3d9PixelBindingRequiresSm2Execution(&binding),
              "unified SET selects exact-generation SM2 object");

    route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY;
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 601, TRUE, &binding, &route, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_BINDING &&
              route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              binding.legacy_handle == saved.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation,
              "impossible dual SET match fails without state mutation");

    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 602, TRUE, &binding, &route, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY &&
              binding.legacy_handle == 602 && !binding.sm2.handle &&
              !binding.sm2.generation &&
              !QxpD3d9PixelBindingRequiresSm2Execution(&binding),
              "legacy SET atomically switches both representation fields");
    detached = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    result = QxpD3d9DeletePixelFunctionUnified(
        &qxp_table, &first_owner, 602, TRUE, &binding, &route,
        &detached, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY &&
              !detached && !binding.legacy_handle &&
              !binding.sm2.handle && !binding.sm2.generation,
              "legacy DELETE clears matching live binding atomically");
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 602, TRUE, &binding, &route, &status);
    saved = binding;
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 603, FALSE, &binding, &route, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND &&
              route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              binding.legacy_handle == saved.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation,
              "missing unified SET preserves both representation fields");

    foreign = qxp_publish_pixel_function(&qxp_table, &second_owner, 604);
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 604, TRUE, &binding, &route, &status);
    qxp_check(result && foreign &&
              route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY &&
              binding.legacy_handle == 604 && !binding.sm2.handle &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &second_owner, 604) == foreign,
              "foreign SM2 numeric match cannot shadow exact-owner legacy");

    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 601, FALSE, &binding, &route, &status);
    saved = binding;
    detached = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)(ULONG_PTR)1;
    route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY;
    result = QxpD3d9DeletePixelFunctionUnified(
        &qxp_table, &first_owner, 601, TRUE, &binding, &route,
        &detached, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_BINDING &&
              route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              !detached &&
              binding.legacy_handle == saved.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &first_owner, 601) == installed,
              "impossible dual DELETE match is atomic");

    result = QxpD3d9DeletePixelFunctionUnified(
        &qxp_table, &first_owner, 602, TRUE, &binding, &route,
        &detached, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY &&
              !detached && !binding.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation,
              "legacy DELETE route preserves unrelated SM2 binding");
    result = QxpD3d9DeletePixelFunctionUnified(
        &qxp_table, &first_owner, 603, FALSE, &binding, &route,
        &detached, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              !detached && !binding.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation,
              "missing unified DELETE is idempotent and nonmutating");

    result = QxpD3d9DeletePixelFunctionUnified(
        &qxp_table, &first_owner, 601, FALSE, &binding, &route,
        &detached, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2 &&
              detached == installed && !binding.legacy_handle &&
              !binding.sm2.handle && !binding.sm2.generation &&
              !QxpD3d9PixelBindingRequiresSm2Execution(&binding) &&
              !QxpD3d9FindPixelFunction(&qxp_table, &first_owner, 601),
              "unified SM2 DELETE detaches and clears exact binding");
    QxpD3d9DestroyPixelFunction(detached, &qxp_allocator);

    binding.legacy_handle = 602;
    binding.sm2.handle = 0;
    binding.sm2.generation = 1;
    saved = binding;
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 602, TRUE, &binding, &route, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_BINDING &&
              route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              binding.legacy_handle == saved.legacy_handle &&
              binding.sm2.handle == saved.sm2.handle &&
              binding.sm2.generation == saved.sm2.generation,
              "corrupt unified binding fails before legacy routing");
    memset(&binding, 0, sizeof(binding));
    result = QxpD3d9SetPixelFunctionUnified(
        &qxp_table, &first_owner, 0, FALSE, &binding, &route, &status);
    qxp_check(result && route == QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE &&
              !binding.legacy_handle && !binding.sm2.handle &&
              !binding.sm2.generation &&
              !QxpD3d9PixelBindingRequiresSm2Execution(&binding),
              "unified zero SET clears both representation fields");

    qxp_destroy_all_for_owner(&qxp_table, &first_owner);
    qxp_destroy_all_for_owner(&qxp_table, &second_owner);
    qxp_check(!qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "unified pixel routing is leak-free with canaries");
}

static void qxp_test_pixel_atomicity_and_teardown(void)
{
    DWORD first_device;
    DWORD second_device;
    QXP_SHADER_OWNER first_owner;
    QXP_SHADER_OWNER second_owner;
    QXP_SHADER_OWNER invalid_owner;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *installed;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONGLONG clock_before;
    ULONG object_bytes;
    DWORD handle;
    DWORD count;
    DWORD mutation;
    BOOL result;

    first_device = 0;
    second_device = 0;
    qxp_initialize_owner(&first_owner, 311, 313, &first_device);
    qxp_initialize_owner(&second_owner, 311, 313, &second_device);
    memset(&invalid_owner, 0, sizeof(invalid_owner));
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    installed = qxp_publish_pixel_function(
        &qxp_table, &first_owner, 401);
    clock_before = qxp_table.generation_clock;
    prepared = qxp_prepare_pixel_function(&first_owner, 401);
    result = QxpD3d9PublishPixelFunction(
        &qxp_table, prepared, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE &&
              !prepared->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 1 &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &first_owner, 401) == installed,
              "pixel duplicate publication is atomic");
    QxpD3d9DestroyPixelFunction(prepared, &qxp_allocator);

    for (mutation = 0; mutation < 30; ++mutation) {
        prepared = qxp_prepare_pixel_function(
            &first_owner, 402 + mutation);
        qxp_mutate_pixel_prepared(prepared, mutation);
        object_bytes = (ULONG)sizeof(*prepared) +
            prepared->bytecode_size - (ULONG)sizeof(DWORD);
        if (mutation == 25) {
            object_bytes = (ULONG)sizeof(*prepared) -
                           (ULONG)sizeof(DWORD);
        }
        memcpy(&qxp_table_copy, &qxp_table, sizeof(qxp_table_copy));
        memcpy(qxp_object_copy, prepared, object_bytes);
        result = QxpD3d9PublishPixelFunction(
            &qxp_table, prepared, &status);
        qxp_check(!result && status.code ==
                      QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT &&
                  memcmp(&qxp_table, &qxp_table_copy,
                         sizeof(qxp_table_copy)) == 0 &&
                  memcmp(prepared, qxp_object_copy, object_bytes) == 0,
                  "pixel publication rejects each corrupt invariant atomically");
        QxpD3d9DestroyPixelFunction(prepared, &qxp_allocator);
    }
    qxp_destroy_all_for_owner(&qxp_table, &first_owner);

    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_table.generation_clock = qxp_max_generation();
    prepared = qxp_prepare_pixel_function(&first_owner, 403);
    object_bytes = (ULONG)sizeof(*prepared) + prepared->bytecode_size -
                   (ULONG)sizeof(DWORD);
    memcpy(&qxp_table_copy, &qxp_table, sizeof(qxp_table_copy));
    memcpy(qxp_object_copy, prepared, object_bytes);
    result = QxpD3d9PublishPixelFunction(
        &qxp_table, prepared, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              memcmp(&qxp_table, &qxp_table_copy,
                     sizeof(qxp_table_copy)) == 0 &&
              memcmp(prepared, qxp_object_copy, object_bytes) == 0,
              "pixel permanent generation exhaustion is atomic");
    QxpD3d9DestroyPixelFunction(prepared, &qxp_allocator);

    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    for (handle = 1; handle <= QXP_D3D9_SHADER_OBJECT_CAPACITY; ++handle)
        qxp_publish_pixel_function(&qxp_table, &first_owner, handle);
    clock_before = qxp_table.generation_clock;
    prepared = qxp_prepare_pixel_function(
        &first_owner, QXP_D3D9_SHADER_OBJECT_CAPACITY + 1UL);
    result = QxpD3d9PublishPixelFunction(
        &qxp_table, prepared, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL &&
              QxpD3d9ShaderObjectStatusIsResourceFailure(&status) &&
              !prepared->generation &&
              qxp_table.generation_clock == clock_before &&
              QxpD3d9PixelFunctionCount(&qxp_table) ==
                  QXP_D3D9_SHADER_OBJECT_CAPACITY,
              "pixel table-full rejection consumes no generation");
    QxpD3d9DestroyPixelFunction(prepared, &qxp_allocator);
    qxp_destroy_all_for_owner(&qxp_table, &first_owner);

    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_pixel_function(&qxp_table, &first_owner, 411);
    qxp_publish_pixel_function(&qxp_table, &first_owner, 412);
    qxp_publish_pixel_function(&qxp_table, &first_owner, 413);
    qxp_publish_pixel_function(&qxp_table, &second_owner, 411);
    qxp_publish_pixel_function(&qxp_table, &second_owner, 412);
    qxp_reset_lock_state();
    result = QxpD3d9DestroyOwnerPixelFunctions(
        &qxp_table, &first_owner, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state, &count, &status);
    qxp_check(result && qxp_status_is_zero(&status) && count == 3 &&
              qxp_lock_contract_ok(4) &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 2 &&
              !QxpD3d9FindPixelFunction(
                  &qxp_table, &first_owner, 411) &&
              QxpD3d9FindPixelFunction(
                  &qxp_table, &second_owner, 411) != NULL,
              "pixel teardown detaches exact owner and frees after unlock");
    qxp_reset_lock_state();
    count = 99;
    result = QxpD3d9DestroyOwnerPixelFunctions(
        &qxp_table, &invalid_owner, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state, &count, &status);
    qxp_check(!result && !count && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_OWNER &&
              qxp_lock_contract_ok(0) &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 2,
              "invalid pixel teardown is deterministic and nonmutating");
    qxp_reset_lock_state();
    result = QxpD3d9DestroyOwnerPixelFunctions(
        &qxp_table, &second_owner, &qxp_allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_lock_state, &count, &status);
    qxp_check(result && count == 2 && qxp_lock_contract_ok(3) &&
              !QxpD3d9PixelFunctionCount(&qxp_table) &&
              !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "pixel teardown count, lock balance, and canaries");
}

static void qxp_test_pixel_batches(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_SHADER_BINDING saved;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    ULONG first_bytes;
    ULONG total_bytes;
    ULONG consumed_bytes;
    DWORD error_offset;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 317, 331, &device);
    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    memset(&binding, 0, sizeof(binding));
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 501,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 502,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x300UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              qxp_status_is_zero(&status) &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 2 &&
              qxp_lock_contract_ok(2) &&
              qxp_allocator_state.live == 2,
              "pixel batch create prepares outside balanced locks");

    qxp_reset_lock_state();
    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 501);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 0);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 502);
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION, 3,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x304UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              binding.handle == 502 && binding.generation != 0 &&
              qxp_lock_contract_ok(3),
              "pixel batch set-clear-set sequential semantics");

    qxp_reset_lock_state();
    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 501);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 502);
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x308UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(result && consumed_bytes == total_bytes && !error_offset &&
              !binding.handle && !binding.generation &&
              !QxpD3d9PixelFunctionCount(&qxp_table) &&
              qxp_lock_contract_ok(2) && !qxp_allocator_state.live &&
              !qxp_allocator_state.canary_failures,
              "pixel batch delete detaches under lock and frees after unlock");

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 511,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 512,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner, &binding, 0x30cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD &&
              consumed_bytes == first_bytes && error_offset == 0x30cUL &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 1 &&
              QxpD3d9FindPixelFunction(&qxp_table, &owner, 511) != NULL &&
              qxp_lock_contract_ok(1),
              "pixel variable create preserves earlier complete prefix");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_pixel_function(&qxp_table, &owner, 521);
    qxp_publish_pixel_function(&qxp_table, &owner, 522);
    memset(&binding, 0, sizeof(binding));
    result = QxpD3d9SetPixelFunction(
        &qxp_table, &owner, 522, &binding, &status);
    saved = binding;
    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 0);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 521);
    qxp_reset_lock_state();
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner, &binding, 0x310UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD &&
              !consumed_bytes && error_offset == 0x310UL &&
              binding.handle == saved.handle &&
              binding.generation == saved.generation &&
              qxp_lock_contract_ok(0),
              "pixel fixed SET truncation preflights before mutation");
    qxp_reset_lock_state();
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes - 1UL, &owner, &binding, 0x314UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && !consumed_bytes && error_offset == 0x314UL &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 2 &&
              binding.handle == saved.handle &&
              binding.generation == saved.generation &&
              qxp_lock_contract_ok(0),
              "pixel fixed DELETE truncation preflights before detach");

    total_bytes = 0;
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 521);
    total_bytes = qxp_append_handle_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), total_bytes, 0);
    qxp_reset_lock_state();
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x318UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE &&
              consumed_bytes == QXP_D3D9_HANDLE_RECORD_BYTES &&
              error_offset == 0x318UL &&
              !QxpD3d9FindPixelFunction(&qxp_table, &owner, 521) &&
              QxpD3d9FindPixelFunction(&qxp_table, &owner, 522) != NULL &&
              binding.handle == saved.handle &&
              binding.generation == saved.generation &&
              qxp_lock_contract_ok(2),
              "pixel fixed batch mutates sequentially after full preflight");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    first_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 531,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), first_bytes, 532,
        qxp_pixel_output_error_ps, sizeof(qxp_pixel_output_error_ps));
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 2,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x31cUL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE &&
              status.decode_error.code != 0 &&
              consumed_bytes == first_bytes && error_offset == 0x31cUL &&
              QxpD3d9PixelFunctionCount(&qxp_table) == 1 &&
              qxp_lock_contract_ok(1),
              "pixel batch decoder failure reports outer offset and prefix");
    qxp_destroy_all_for_owner(&qxp_table, &owner);

    qxp_reset_allocator(1);
    qxp_reset_lock_state();
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    total_bytes = qxp_append_function_record(
        qxp_batch_frame, sizeof(qxp_batch_frame), 0, 541,
        qxp_minimal_ps, sizeof(qxp_minimal_ps));
    result = qxp_process_pixel_batch(
        QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION, 1,
        qxp_batch_frame, total_bytes, &owner, &binding, 0x320UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION &&
              !consumed_bytes && error_offset == 0x320UL &&
              qxp_lock_contract_ok(0) && !qxp_allocator_state.live,
              "pixel batch allocation failure rolls back before lock");

    qxp_reset_allocator(0);
    qxp_reset_lock_state();
    result = qxp_process_pixel_batch(
        (BYTE)57, 0, NULL, 0, &owner, &binding, 0x324UL,
        &consumed_bytes, &error_offset, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE &&
              !consumed_bytes && error_offset == 0x324UL &&
              qxp_lock_contract_ok(0),
              "pixel batch accepts only exact opcodes 54 through 56");
}

static void qxp_test_binding_validation(void)
{
    DWORD device;
    QXP_SHADER_OWNER owner;
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_SHADER_BINDING saved;
    QXP_D3D9_SHADER_OBJECT_STATUS status;
    DWORD legacy_fvf;
    BOOL result;

    device = 0;
    qxp_initialize_owner(&owner, 193, 197, &device);
    qxp_reset_allocator(0);
    QxpD3d9ShaderObjectTableInitialize(&qxp_table);
    qxp_publish_declaration(&qxp_table, &owner, 199);
    binding.handle = 199;
    binding.generation = 0;
    saved = binding;
    legacy_fvf = 0x11223344UL;
    result = QxpD3d9SetVertexDeclaration(
        &qxp_table, &owner, 199, &binding, &legacy_fvf, &status);
    qxp_check(!result && status.code ==
                  QXP_D3D9_SHADER_OBJECT_ERROR_BINDING &&
              binding.handle == saved.handle &&
              binding.generation == saved.generation &&
              legacy_fvf == 0x11223344UL,
              "malformed binding preserves binding and FVF");
    qxp_destroy_all_for_owner(&qxp_table, &owner);
    qxp_check(!qxp_allocator_state.live,
              "binding validation cleanup");
}

int main(void)
{
    qxp_allocator.allocate = qxp_test_allocate;
    qxp_allocator.release = qxp_test_release;
    qxp_allocator.opaque = &qxp_allocator_state;

    qxp_test_layout_and_literals();
    qxp_test_declaration_framing();
    qxp_test_function_framing();
    qxp_test_pixel_function_framing();
    qxp_test_mutation_framing();
    qxp_test_prepare_success_and_retention();
    qxp_test_prepare_errors_and_failures();
    qxp_test_prepare_vertex_sm1_profile();
    qxp_test_pixel_prepare_and_failures();
    qxp_test_namespaces_owners_bindings_and_snapshots();
    qxp_test_delete_recreate_and_aba();
    qxp_test_foreign_delete_isolation();
    qxp_test_duplicate_atomicity();
    qxp_test_generation_exhaustion();
    qxp_test_owner_teardown();
    qxp_test_table_exhaustion();
    qxp_test_batch_success_paths();
    qxp_test_production_draw_state_bridge();
    qxp_test_batch_partial_and_truncation();
    qxp_test_batch_failures_and_offsets();
    qxp_test_pixel_lifecycle();
    qxp_test_production_pixel_dispatch();
    qxp_test_unified_pixel_routing();
    qxp_test_pixel_atomicity_and_teardown();
    qxp_test_pixel_batches();
    qxp_test_binding_validation();

    qxp_check(qxp_allocator_state.live == 0,
              "final allocator leak audit");
    qxp_check(qxp_allocator_state.canary_failures == 0,
              "final allocator canary audit");
    qxp_check(qxp_lock_state.depth == 0 &&
              qxp_lock_state.acquires == qxp_lock_state.releases,
              "final lock balance audit");
    qxp_check(!qxp_allocator_state.allocations_while_locked &&
              !qxp_allocator_state.releases_while_locked,
              "final no allocation or free while locked audit");
    if (qxp_failures) {
        fprintf(stderr, "qxp_d3d9_shader_objects: %d failure(s)\n",
                qxp_failures);
        return 1;
    }
    puts("qxp d3d9 shader object tests passed");
    return 0;
}
