#include <windef.h>

#include "qxp_d3d9_shader_objects.h"

static void qxp_d3d9_zero(void *memory, ULONG byte_count)
{
    volatile BYTE *bytes;
    ULONG index;

    if (!memory) return;
    /* Volatile prevents an old WDK optimizer from introducing CRT memset. */
    bytes = (volatile BYTE *)memory;
    for (index = 0; index < byte_count; ++index) bytes[index] = 0;
}

static void qxp_d3d9_copy(void *destination, const void *source,
                          ULONG byte_count)
{
    volatile BYTE *out;
    const BYTE *in;
    ULONG index;

    out = (volatile BYTE *)destination;
    in = (const BYTE *)source;
    for (index = 0; index < byte_count; ++index) out[index] = in[index];
}

static void qxp_d3d9_reset_status(QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    if (status) qxp_d3d9_zero(status, (ULONG)sizeof(*status));
}

static BOOL qxp_d3d9_fail(QXP_D3D9_SHADER_OBJECT_STATUS *status,
                          DWORD code, DWORD byte_offset)
{
    if (status) {
        status->code = code;
        status->byte_offset = byte_offset;
    }
    return FALSE;
}

static BOOL qxp_d3d9_owner_valid(const QXP_SHADER_OWNER *owner)
{
    if (!owner) return FALSE;
    return qxp_shader_owner_matches(
        owner, owner->context_handle, owner->process_id, owner->device) != 0;
}

static BOOL qxp_d3d9_owner_equal(const QXP_SHADER_OWNER *left,
                                 const QXP_SHADER_OWNER *right)
{
    if (!right) return FALSE;
    return qxp_shader_owner_matches(
        left, right->context_handle, right->process_id, right->device) != 0;
}

static BOOL qxp_d3d9_allocator_valid(
    const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    return allocator && allocator->allocate && allocator->release;
}

static BOOL qxp_d3d9_binding_valid(
    const QXP_D3D9_SHADER_BINDING *binding)
{
    if (!binding) return FALSE;
    if (!binding->handle) return binding->generation == 0;
    return binding->generation != 0;
}

static BOOL qxp_d3d9_declaration_handle_is_private(DWORD handle)
{
    return (handle &
            QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT) != 0;
}

BOOL QxpD3d9FrameCreateVertexDeclaration(
    const void *record, ULONG available_bytes, DWORD *handle, DWORD *count,
    ULONG *consumed_bytes, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_D3D9_PRIVATE_CREATE_DECLARATION header;
    ULONG remaining;
    ULONG payload_bytes;

    if (handle) *handle = 0;
    if (count) *count = 0;
    if (consumed_bytes) *consumed_bytes = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !handle || !count || !consumed_bytes || !record)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (available_bytes < QXP_D3D9_CREATE_RECORD_BYTES)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER,
            available_bytes);

    qxp_d3d9_copy(&header, record, (ULONG)sizeof(header));
    if (!header.handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!qxp_d3d9_declaration_handle_is_private(header.handle))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE, 0);
    if (!header.count ||
        header.count > QXP_VERTEX_DECLARATION9_MAX_ELEMENTS)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT,
            (DWORD)sizeof(header.handle));

    payload_bytes = (ULONG)header.count * QXP_VERTEX_ELEMENT9_BYTES;
    remaining = available_bytes - QXP_D3D9_CREATE_RECORD_BYTES;
    if (payload_bytes > remaining)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD,
            available_bytes);

    *handle = header.handle;
    *count = header.count;
    *consumed_bytes = QXP_D3D9_CREATE_RECORD_BYTES + payload_bytes;
    return TRUE;
}

BOOL QxpD3d9FrameCreateVertexFunction(
    const void *record, ULONG available_bytes, DWORD *handle,
    DWORD *bytecode_size, ULONG *consumed_bytes,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_D3D9_PRIVATE_CREATE_FUNCTION header;
    ULONG remaining;
    DWORD token_count;

    if (handle) *handle = 0;
    if (bytecode_size) *bytecode_size = 0;
    if (consumed_bytes) *consumed_bytes = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !handle || !bytecode_size || !consumed_bytes || !record)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (available_bytes < QXP_D3D9_CREATE_RECORD_BYTES)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_HEADER,
            available_bytes);

    qxp_d3d9_copy(&header, record, (ULONG)sizeof(header));
    if (!header.handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!header.size || (header.size & (sizeof(DWORD) - 1UL)))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE,
            (DWORD)sizeof(header.handle));
    token_count = header.size / (DWORD)sizeof(DWORD);
    if (token_count > QXP_SHADER_MAX_TOKENS)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE,
            (DWORD)sizeof(header.handle));

    remaining = available_bytes - QXP_D3D9_CREATE_RECORD_BYTES;
    if (header.size > remaining)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD,
            available_bytes);

    *handle = header.handle;
    *bytecode_size = header.size;
    *consumed_bytes = QXP_D3D9_CREATE_RECORD_BYTES + header.size;
    return TRUE;
}

BOOL QxpD3d9FrameCreatePixelFunction(
    const void *record, ULONG available_bytes, DWORD *handle,
    DWORD *bytecode_size, ULONG *consumed_bytes,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    return QxpD3d9FrameCreateVertexFunction(
        record, available_bytes, handle, bytecode_size, consumed_bytes,
        status);
}

BOOL QxpD3d9PrepareVertexDeclaration(
    const QXP_SHADER_OWNER *owner, DWORD handle,
    const QXP_VERTEX_ELEMENT9 *elements, DWORD element_count,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_VERTEX_DECLARATION9 declaration;
    QXP_VERTEX_DECLARATION9_ERROR parse_error;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;
    DWORD legacy_fvf;
    DWORD legacy_vertex_stride;
    BOOL legacy_fvf_valid;

    if (prepared) *prepared = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !prepared || !elements ||
        !qxp_d3d9_allocator_valid(allocator))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!qxp_d3d9_declaration_handle_is_private(handle))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE, 0);
    if (!element_count ||
        element_count > QXP_VERTEX_DECLARATION9_MAX_ELEMENTS)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_COUNT, 0);

    qxp_d3d9_zero(&declaration, (ULONG)sizeof(declaration));
    qxp_d3d9_zero(&parse_error, (ULONG)sizeof(parse_error));
    if (!QxpParseVertexDeclaration9(
            elements, element_count, &declaration, &parse_error)) {
        qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_PARSE,
            parse_error.byte_offset);
        qxp_d3d9_copy(&status->declaration_error, &parse_error,
                      (ULONG)sizeof(parse_error));
        return FALSE;
    }

    legacy_fvf = 0;
    legacy_vertex_stride = 0;
    legacy_fvf_valid = QxpTranslateVertexDeclaration9ToFvf(
        &declaration, &legacy_fvf, &legacy_vertex_stride);

    object = (QXP_D3D9_VERTEX_DECLARATION_OBJECT *)allocator->allocate(
        (ULONG)sizeof(*object), allocator->opaque);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION, 0);
    qxp_d3d9_zero(object, (ULONG)sizeof(*object));
    qxp_d3d9_copy(&object->owner, owner, (ULONG)sizeof(object->owner));
    object->handle = handle;
    qxp_d3d9_copy(&object->declaration, &declaration,
                  (ULONG)sizeof(object->declaration));
    object->legacy_fvf_valid = legacy_fvf_valid;
    object->legacy_fvf = legacy_fvf;
    object->legacy_vertex_stride = legacy_vertex_stride;
    *prepared = object;
    return TRUE;
}

static void qxp_d3d9_summarize_shader(
    const QXP_SHADER_IR *ir, QXP_D3D9_SHADER_DECODE_SUMMARY *summary)
{
    summary->stage = ir->stage;
    summary->version_token = ir->version_token;
    summary->token_count = ir->token_count;
    summary->instruction_count = ir->instruction_count;
    summary->declaration_count = ir->declaration_count;
    summary->definition_count = ir->definition_count;
    summary->comment_count = ir->comment_count;
    summary->slot_count = ir->slot_count;
    summary->arithmetic_slot_count = ir->arithmetic_slot_count;
    summary->texture_slot_count = ir->texture_slot_count;
    summary->flow_slot_count = ir->flow_slot_count;
    summary->block_count = ir->block_count;
}

BOOL QxpD3d9PrepareVertexFunction(
    const QXP_SHADER_OWNER *owner, DWORD handle, const DWORD *tokens,
    DWORD bytecode_size, const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_SHADER_IR *ir;
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;
    DWORD token_count;
    ULONG allocation_bytes;

    if (prepared) *prepared = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !prepared || !tokens ||
        !qxp_d3d9_allocator_valid(allocator))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!bytecode_size ||
        (bytecode_size & ((DWORD)sizeof(DWORD) - 1UL)))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE, 0);
    token_count = bytecode_size / (DWORD)sizeof(DWORD);
    if (token_count > QXP_SHADER_MAX_TOKENS)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE, 0);

    ir = (QXP_SHADER_IR *)allocator->allocate(
        (ULONG)sizeof(*ir), allocator->opaque);
    if (!ir)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION, 0);
    qxp_d3d9_zero(&decode_error, (ULONG)sizeof(decode_error));
    if (!QxpDecodeShader20(tokens, token_count, ir, &decode_error)) {
        qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE,
            decode_error.token_offset * (DWORD)sizeof(DWORD));
        qxp_d3d9_copy(&status->decode_error, &decode_error,
                      (ULONG)sizeof(decode_error));
        allocator->release(ir, allocator->opaque);
        return FALSE;
    }
    if (ir->stage != QXP_SHADER_STAGE_VERTEX ||
        !QXP_SHADER_VERSION_IS_VERTEX(ir->version_token)) {
        allocator->release(ir, allocator->opaque);
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE, 0);
    }
    qxp_d3d9_summarize_shader(ir, &summary);
    allocator->release(ir, allocator->opaque);

    allocation_bytes = (ULONG)sizeof(*object) +
                       (ULONG)bytecode_size - (ULONG)sizeof(DWORD);
    object = (QXP_D3D9_VERTEX_FUNCTION_OBJECT *)allocator->allocate(
        allocation_bytes, allocator->opaque);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION, 0);
    qxp_d3d9_zero(object, allocation_bytes);
    qxp_d3d9_copy(&object->owner, owner, (ULONG)sizeof(object->owner));
    object->handle = handle;
    object->bytecode_size = bytecode_size;
    qxp_d3d9_copy(&object->summary, &summary,
                  (ULONG)sizeof(object->summary));
    qxp_d3d9_copy(object->bytecode, tokens, (ULONG)bytecode_size);
    *prepared = object;
    return TRUE;
}

BOOL QxpD3d9PreparePixelFunction(
    const QXP_SHADER_OWNER *owner, DWORD handle, const DWORD *tokens,
    DWORD bytecode_size, const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_SHADER_IR *ir;
    QXP_SHADER_DECODE_ERROR decode_error;
    QXP_PIXEL_SHADER_GLSL_ERROR glsl_error;
    QXP_PIXEL_SHADER_GLSL_METADATA metadata;
    QXP_D3D9_SHADER_DECODE_SUMMARY summary;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;
    DWORD token_count;
    DWORD source_bytes;
    ULONG fixed_bytes;
    ULONG allocation_bytes;

    if (prepared) *prepared = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !prepared || !tokens ||
        !qxp_d3d9_allocator_valid(allocator))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!bytecode_size ||
        (bytecode_size & ((DWORD)sizeof(DWORD) - 1UL)))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE, 0);
    token_count = bytecode_size / (DWORD)sizeof(DWORD);
    if (token_count > QXP_SHADER_MAX_TOKENS)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE, 0);

    ir = (QXP_SHADER_IR *)allocator->allocate(
        (ULONG)sizeof(*ir), allocator->opaque);
    if (!ir)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION, 0);
    qxp_d3d9_zero(&decode_error, (ULONG)sizeof(decode_error));
    if (!QxpDecodeShader20(tokens, token_count, ir, &decode_error)) {
        qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_DECODE,
            decode_error.token_offset * (DWORD)sizeof(DWORD));
        qxp_d3d9_copy(&status->decode_error, &decode_error,
                      (ULONG)sizeof(decode_error));
        allocator->release(ir, allocator->opaque);
        return FALSE;
    }
    if (ir->stage != QXP_SHADER_STAGE_PIXEL ||
        !QXP_SHADER_VERSION_IS_PIXEL(ir->version_token)) {
        allocator->release(ir, allocator->opaque);
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_PROFILE, 0);
    }

    qxp_d3d9_zero(&metadata, (ULONG)sizeof(metadata));
    qxp_d3d9_zero(&glsl_error, (ULONG)sizeof(glsl_error));
    source_bytes = 0;
    /* B0 and B2 deliberately admit the same exact ps_1_1 and ps_2_0 forms,
     * so no token fixture should reach this defensive cross-layer failure.
     * Hostile typed-IR failures remain owned by qxp_pshader_glsl_test. */
    if (!QxpEmitPixelShader20Glsl(
            ir, NULL, 0, &source_bytes, &metadata, &glsl_error)) {
        DWORD byte_offset;

        byte_offset = glsl_error.token_offset == QXP_SHADER_INDEX_NONE ?
            0 : glsl_error.token_offset * (DWORD)sizeof(DWORD);
        qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_LOWERING,
            byte_offset);
        qxp_d3d9_copy(&status->glsl_error, &glsl_error,
                      (ULONG)sizeof(glsl_error));
        allocator->release(ir, allocator->opaque);
        return FALSE;
    }
    qxp_d3d9_zero(&summary, (ULONG)sizeof(summary));
    qxp_d3d9_summarize_shader(ir, &summary);
    allocator->release(ir, allocator->opaque);

    fixed_bytes = (ULONG)sizeof(*object) - (ULONG)sizeof(DWORD);
    if ((ULONG)bytecode_size > ~(ULONG)0 - fixed_bytes)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_FUNCTION_SIZE, 0);
    allocation_bytes = fixed_bytes + (ULONG)bytecode_size;
    object = (QXP_D3D9_PIXEL_FUNCTION_OBJECT *)allocator->allocate(
        allocation_bytes, allocator->opaque);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION, 0);
    qxp_d3d9_zero(object, allocation_bytes);
    qxp_d3d9_copy(&object->owner, owner, (ULONG)sizeof(object->owner));
    object->handle = handle;
    object->bytecode_size = bytecode_size;
    object->glsl_source_bytes = source_bytes;
    qxp_d3d9_copy(&object->summary, &summary,
                  (ULONG)sizeof(object->summary));
    qxp_d3d9_copy(&object->metadata, &metadata,
                  (ULONG)sizeof(object->metadata));
    qxp_d3d9_copy(object->bytecode, tokens, (ULONG)bytecode_size);
    *prepared = object;
    return TRUE;
}

void QxpD3d9DestroyVertexDeclaration(
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    if (object && qxp_d3d9_allocator_valid(allocator))
        allocator->release(object, allocator->opaque);
}

void QxpD3d9DestroyVertexFunction(
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    if (object && qxp_d3d9_allocator_valid(allocator))
        allocator->release(object, allocator->opaque);
}

void QxpD3d9DestroyPixelFunction(
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *object,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator)
{
    if (object && qxp_d3d9_allocator_valid(allocator))
        allocator->release(object, allocator->opaque);
}

void QxpD3d9ShaderObjectTableInitialize(
    QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    if (table) qxp_d3d9_zero(table, (ULONG)sizeof(*table));
}

BOOL QxpD3d9PublishVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;
    DWORD free_index;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !prepared)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(&prepared->owner) || !prepared->handle ||
        !qxp_d3d9_declaration_handle_is_private(prepared->handle) ||
        prepared->generation || !prepared->declaration.valid)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT, 0);
    if (table->generation_clock == ~(ULONGLONG)0)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED, 0);

    free_index = QXP_D3D9_SHADER_OBJECT_CAPACITY;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

        object = table->declarations[index];
        if (!object) {
            if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
                free_index = index;
        } else if (object->handle == prepared->handle &&
                   qxp_d3d9_owner_equal(
                       &object->owner, &prepared->owner)) {
            return qxp_d3d9_fail(
                status, QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE, 0);
        }
    }
    if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL, 0);

    ++table->generation_clock;
    prepared->generation = table->generation_clock;
    table->declarations[free_index] = prepared;
    return TRUE;
}

BOOL QxpD3d9PublishVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;
    DWORD free_index;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !prepared)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(&prepared->owner) || !prepared->handle ||
        prepared->generation || !prepared->bytecode_size ||
        prepared->summary.stage != QXP_SHADER_STAGE_VERTEX ||
        !QXP_SHADER_VERSION_IS_VERTEX(prepared->summary.version_token))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT, 0);
    if (table->generation_clock == ~(ULONGLONG)0)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED, 0);

    free_index = QXP_D3D9_SHADER_OBJECT_CAPACITY;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

        object = table->functions[index];
        if (!object) {
            if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
                free_index = index;
        } else if (object->handle == prepared->handle &&
                   qxp_d3d9_owner_equal(
                       &object->owner, &prepared->owner)) {
            return qxp_d3d9_fail(
                status, QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE, 0);
        }
    }
    if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL, 0);

    ++table->generation_clock;
    prepared->generation = table->generation_clock;
    table->functions[free_index] = prepared;
    return TRUE;
}

static BOOL qxp_d3d9_pixel_metadata_valid(
    const QXP_PIXEL_SHADER_GLSL_METADATA *metadata)
{
    DWORD index;
    DWORD allowed_caps;

    if (!metadata) return FALSE;
    allowed_caps = QXP_SHADER_PS20CAPS_ARBITRARYSWIZZLE |
                   QXP_SHADER_PS20CAPS_NODEPENDENTREADLIMIT |
                   QXP_SHADER_PS20CAPS_NOTEXINSTRUCTIONLIMIT;
    if (metadata->required_ps_temp_count > QXP_SHADER_MAX_PS_TEMPS ||
        metadata->required_constant_count >
            QXP_PIXEL_SHADER_GLSL_CONSTANT_COUNT ||
        (metadata->required_ps20_caps & ~allowed_caps) != 0 ||
        !metadata->required_ps_instruction_slots ||
        (metadata->input_color_mask & ~3UL) != 0 ||
        (metadata->input_texture_mask & ~0xffUL) != 0 ||
        (metadata->input_centroid_mask & ~0x3ffUL) != 0 ||
        (metadata->input_centroid_mask & 3UL) !=
            metadata->input_color_mask ||
        ((metadata->input_centroid_mask >> 2) &
         ~metadata->input_texture_mask) != 0 ||
        (metadata->sampler_mask & ~0xffffUL) != 0 ||
        (metadata->color_output_mask != 1UL &&
         metadata->color_output_mask != 3UL &&
         metadata->color_output_mask != 7UL &&
         metadata->color_output_mask != 15UL) ||
        (metadata->writes_depth != FALSE &&
         metadata->writes_depth != TRUE) ||
        (metadata->uses_discard != FALSE &&
         metadata->uses_discard != TRUE)) return FALSE;
    for (index = 0; index < 2; ++index) {
        if (metadata->input_color_mask & (1UL << index)) {
            if (!metadata->input_color_components[index] ||
                metadata->input_color_components[index] > 15)
                return FALSE;
        } else if (metadata->input_color_components[index]) {
            return FALSE;
        }
    }
    for (index = 0; index < 8; ++index) {
        if (metadata->input_texture_mask & (1UL << index)) {
            if (!metadata->input_texture_components[index] ||
                metadata->input_texture_components[index] > 15)
                return FALSE;
        } else if (metadata->input_texture_components[index]) {
            return FALSE;
        }
    }
    for (index = 0;
         index < QXP_PIXEL_SHADER_GLSL_SAMPLER_COUNT; ++index) {
        if (metadata->sampler_mask & (1UL << index)) {
            if (metadata->sampler_types[index] != QXP_SHADER_SAMPLER_2D &&
                metadata->sampler_types[index] !=
                    QXP_SHADER_SAMPLER_CUBE &&
                metadata->sampler_types[index] !=
                    QXP_SHADER_SAMPLER_VOLUME)
                return FALSE;
        } else if (metadata->sampler_types[index]) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL qxp_d3d9_pixel_prepared_valid(
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared)
{
    const QXP_D3D9_SHADER_DECODE_SUMMARY *summary;
    DWORD token_count;

    if (!prepared || !qxp_d3d9_owner_valid(&prepared->owner) ||
        !prepared->handle || prepared->generation ||
        !prepared->bytecode_size ||
        (prepared->bytecode_size & ((DWORD)sizeof(DWORD) - 1UL)) ||
        !prepared->glsl_source_bytes ||
        prepared->glsl_source_bytes > QXP_PIXEL_SHADER_GLSL_MAX_SOURCE)
        return FALSE;
    token_count = prepared->bytecode_size / (DWORD)sizeof(DWORD);
    if (token_count > QXP_SHADER_MAX_TOKENS) return FALSE;
    summary = &prepared->summary;
    if (summary->stage != QXP_SHADER_STAGE_PIXEL ||
        !QXP_SHADER_VERSION_IS_PIXEL(summary->version_token) ||
        summary->token_count != token_count ||
        summary->instruction_count > QXP_SHADER_MAX_INSTRUCTIONS ||
        summary->block_count != 1 || summary->flow_slot_count ||
        summary->arithmetic_slot_count > summary->slot_count ||
        summary->texture_slot_count >
            summary->slot_count - summary->arithmetic_slot_count)
        return FALSE;
    return qxp_d3d9_pixel_metadata_valid(&prepared->metadata);
}

BOOL QxpD3d9PublishPixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;
    DWORD free_index;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !prepared)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_pixel_prepared_valid(prepared))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT, 0);
    if (table->generation_clock == ~(ULONGLONG)0)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED, 0);

    free_index = QXP_D3D9_SHADER_OBJECT_CAPACITY;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

        object = table->pixel_functions[index];
        if (!object) {
            if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
                free_index = index;
        } else if (object->handle == prepared->handle &&
                   qxp_d3d9_owner_equal(
                       &object->owner, &prepared->owner)) {
            return qxp_d3d9_fail(
                status, QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE, 0);
        }
    }
    if (free_index == QXP_D3D9_SHADER_OBJECT_CAPACITY)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL, 0);

    ++table->generation_clock;
    prepared->generation = table->generation_clock;
    table->pixel_functions[free_index] = prepared;
    return TRUE;
}

BOOL QxpD3d9PublishPixelFunctionUnified(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared,
    BOOL legacy_handle_exists,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    qxp_d3d9_reset_status(status);
    if (!status || !table || !prepared)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_pixel_prepared_valid(prepared))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_PREPARED_OBJECT, 0);
    if (legacy_handle_exists)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DUPLICATE, 0);
    return QxpD3d9PublishPixelFunction(table, prepared, status);
}

const QXP_D3D9_VERTEX_DECLARATION_OBJECT *QxpD3d9FindVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner) || !handle) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        const QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

        object = table->declarations[index];
        if (object && object->handle == handle &&
            qxp_d3d9_owner_equal(&object->owner, owner)) return object;
    }
    return NULL;
}

const QXP_D3D9_VERTEX_FUNCTION_OBJECT *QxpD3d9FindVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner) || !handle) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        const QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

        object = table->functions[index];
        if (object && object->handle == handle &&
            qxp_d3d9_owner_equal(&object->owner, owner)) return object;
    }
    return NULL;
}

const QXP_D3D9_PIXEL_FUNCTION_OBJECT *QxpD3d9FindPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner) || !handle) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

        object = table->pixel_functions[index];
        if (object && object->handle == handle &&
            qxp_d3d9_owner_equal(&object->owner, owner)) return object;
    }
    return NULL;
}

void QxpD3d9ClearShaderBinding(QXP_D3D9_SHADER_BINDING *binding)
{
    if (binding) qxp_d3d9_zero(binding, (ULONG)sizeof(*binding));
}

BOOL QxpD3d9SetVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding, DWORD *legacy_fvf,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !legacy_fvf)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle) {
        QxpD3d9ClearShaderBinding(binding);
        *legacy_fvf = 0;
        return TRUE;
    }
    if (!qxp_d3d9_declaration_handle_is_private(handle)) {
        QxpD3d9ClearShaderBinding(binding);
        *legacy_fvf = handle;
        return TRUE;
    }

    object = QxpD3d9FindVertexDeclaration(table, owner, handle);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    binding->handle = object->handle;
    binding->generation = object->generation;
    *legacy_fvf = 0;
    return TRUE;
}

BOOL QxpD3d9SetVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle) {
        QxpD3d9ClearShaderBinding(binding);
        return TRUE;
    }

    object = QxpD3d9FindVertexFunction(table, owner, handle);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    binding->handle = object->handle;
    binding->generation = object->generation;
    return TRUE;
}

BOOL QxpD3d9SetPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle) {
        QxpD3d9ClearShaderBinding(binding);
        return TRUE;
    }

    object = QxpD3d9FindPixelFunction(table, owner, handle);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    binding->handle = object->handle;
    binding->generation = object->generation;
    return TRUE;
}

void QxpD3d9ClearPixelBindingState(
    QXP_D3D9_PIXEL_BINDING_STATE *binding)
{
    if (!binding) return;
    binding->legacy_handle = 0;
    QxpD3d9ClearShaderBinding(&binding->sm2);
}

BOOL QxpD3d9PixelBindingRequiresSm2Execution(
    const QXP_D3D9_PIXEL_BINDING_STATE *binding)
{
    return binding && binding->sm2.handle != 0;
}

BOOL QxpD3d9SetPixelFunctionUnified(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    BOOL legacy_handle_exists, QXP_D3D9_PIXEL_BINDING_STATE *binding,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE *route,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

    if (route) *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !route)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(&binding->sm2))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle) {
        QxpD3d9ClearPixelBindingState(binding);
        return TRUE;
    }

    object = QxpD3d9FindPixelFunction(table, owner, handle);
    if (legacy_handle_exists && object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (legacy_handle_exists) {
        binding->legacy_handle = handle;
        QxpD3d9ClearShaderBinding(&binding->sm2);
        *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY;
        return TRUE;
    }
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    binding->legacy_handle = 0;
    binding->sm2.handle = object->handle;
    binding->sm2.generation = object->generation;
    *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2;
    return TRUE;
}

BOOL QxpD3d9DeleteVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_DECLARATION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;

    if (detached) *detached = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !detached)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);
    if (!qxp_d3d9_declaration_handle_is_private(handle))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_DECLARATION_HANDLE, 0);

    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

        object = table->declarations[index];
        if (!object || object->handle != handle ||
            !qxp_d3d9_owner_equal(&object->owner, owner)) continue;
        if (binding->handle == object->handle &&
            binding->generation == object->generation)
            QxpD3d9ClearShaderBinding(binding);
        table->declarations[index] = NULL;
        *detached = object;
        return TRUE;
    }
    return TRUE;
}

BOOL QxpD3d9DeleteVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;

    if (detached) *detached = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !detached)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);

    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

        object = table->functions[index];
        if (!object || object->handle != handle ||
            !qxp_d3d9_owner_equal(&object->owner, owner)) continue;
        if (binding->handle == object->handle &&
            binding->generation == object->generation)
            QxpD3d9ClearShaderBinding(binding);
        table->functions[index] = NULL;
        *detached = object;
        return TRUE;
    }
    return TRUE;
}

BOOL QxpD3d9DeletePixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    DWORD handle, QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    DWORD index;

    if (detached) *detached = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !detached)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);

    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

        object = table->pixel_functions[index];
        if (!object || object->handle != handle ||
            !qxp_d3d9_owner_equal(&object->owner, owner)) continue;
        if (binding->handle == object->handle &&
            binding->generation == object->generation)
            QxpD3d9ClearShaderBinding(binding);
        table->pixel_functions[index] = NULL;
        *detached = object;
        return TRUE;
    }
    return TRUE;
}

BOOL QxpD3d9DeletePixelFunctionUnified(
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, DWORD handle,
    BOOL legacy_handle_exists, QXP_D3D9_PIXEL_BINDING_STATE *binding,
    QXP_D3D9_PIXEL_FUNCTION_ROUTE *route,
    QXP_D3D9_PIXEL_FUNCTION_OBJECT **detached,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;
    BOOL result;

    if (route) *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_NONE;
    if (detached) *detached = NULL;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !route || !detached)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(&binding->sm2))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!handle)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_ZERO_HANDLE, 0);

    object = QxpD3d9FindPixelFunction(table, owner, handle);
    if (legacy_handle_exists && object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (legacy_handle_exists) {
        if (binding->legacy_handle == handle) binding->legacy_handle = 0;
        *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_LEGACY;
        return TRUE;
    }
    if (!object) return TRUE;

    result = QxpD3d9DeletePixelFunction(
        table, owner, handle, &binding->sm2, detached, status);
    if (result) *route = QXP_D3D9_PIXEL_FUNCTION_ROUTE_SM2;
    return result;
}

static const QXP_D3D9_VERTEX_DECLARATION_OBJECT *
qxp_d3d9_find_bound_declaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding)
{
    const QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

    object = QxpD3d9FindVertexDeclaration(table, owner, binding->handle);
    if (!object || object->generation != binding->generation) return NULL;
    return object;
}

static const QXP_D3D9_VERTEX_FUNCTION_OBJECT *qxp_d3d9_find_bound_function(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding)
{
    const QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

    object = QxpD3d9FindVertexFunction(table, owner, binding->handle);
    if (!object || object->generation != binding->generation) return NULL;
    return object;
}

static const QXP_D3D9_PIXEL_FUNCTION_OBJECT *
qxp_d3d9_find_bound_pixel_function(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding)
{
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

    object = QxpD3d9FindPixelFunction(table, owner, binding->handle);
    if (!object || object->generation != binding->generation) return NULL;
    return object;
}

BOOL QxpD3d9SnapshotVertexDeclaration(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_DECLARATION_SNAPSHOT *snapshot,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

    if (snapshot) qxp_d3d9_zero(snapshot, (ULONG)sizeof(*snapshot));
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !snapshot)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!binding->handle) return TRUE;

    object = qxp_d3d9_find_bound_declaration(table, owner, binding);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    qxp_d3d9_copy(&snapshot->owner, &object->owner,
                  (ULONG)sizeof(snapshot->owner));
    snapshot->binding.handle = object->handle;
    snapshot->binding.generation = object->generation;
    qxp_d3d9_copy(&snapshot->declaration, &object->declaration,
                  (ULONG)sizeof(snapshot->declaration));
    return TRUE;
}

BOOL QxpD3d9SnapshotVertexFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_VERTEX_FUNCTION_SNAPSHOT *snapshot, void *bytecode,
    ULONG bytecode_capacity, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

    if (snapshot) qxp_d3d9_zero(snapshot, (ULONG)sizeof(*snapshot));
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !snapshot)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!bytecode && bytecode_capacity)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!binding->handle) return TRUE;

    object = qxp_d3d9_find_bound_function(table, owner, binding);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    if (bytecode && bytecode_capacity < object->bytecode_size)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OUTPUT_BUFFER, 0);

    qxp_d3d9_copy(&snapshot->owner, &object->owner,
                  (ULONG)sizeof(snapshot->owner));
    snapshot->binding.handle = object->handle;
    snapshot->binding.generation = object->generation;
    snapshot->bytecode_size = object->bytecode_size;
    qxp_d3d9_copy(&snapshot->summary, &object->summary,
                  (ULONG)sizeof(snapshot->summary));
    if (bytecode)
        qxp_d3d9_copy(bytecode, object->bytecode,
                      (ULONG)object->bytecode_size);
    return TRUE;
}

BOOL QxpD3d9SnapshotPixelFunction(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table,
    const QXP_SHADER_OWNER *owner, const QXP_D3D9_SHADER_BINDING *binding,
    QXP_D3D9_PIXEL_FUNCTION_SNAPSHOT *snapshot, void *bytecode,
    ULONG bytecode_capacity, QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

    if (snapshot) qxp_d3d9_zero(snapshot, (ULONG)sizeof(*snapshot));
    qxp_d3d9_reset_status(status);
    if (!status || !table || !binding || !snapshot)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (!qxp_d3d9_binding_valid(binding))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
    if (!bytecode && bytecode_capacity)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    if (!binding->handle) return TRUE;

    object = qxp_d3d9_find_bound_pixel_function(table, owner, binding);
    if (!object)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NOT_FOUND, 0);
    if (bytecode && bytecode_capacity < object->bytecode_size)
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OUTPUT_BUFFER, 0);

    qxp_d3d9_copy(&snapshot->owner, &object->owner,
                  (ULONG)sizeof(snapshot->owner));
    snapshot->binding.handle = object->handle;
    snapshot->binding.generation = object->generation;
    snapshot->bytecode_size = object->bytecode_size;
    snapshot->glsl_source_bytes = object->glsl_source_bytes;
    qxp_d3d9_copy(&snapshot->summary, &object->summary,
                  (ULONG)sizeof(snapshot->summary));
    qxp_d3d9_copy(&snapshot->metadata, &object->metadata,
                  (ULONG)sizeof(snapshot->metadata));
    if (bytecode)
        qxp_d3d9_copy(bytecode, object->bytecode,
                      (ULONG)object->bytecode_size);
    return TRUE;
}

QXP_D3D9_VERTEX_DECLARATION_OBJECT *QxpD3d9DetachOwnerVertexDeclaration(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner)) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_DECLARATION_OBJECT *object;

        object = table->declarations[index];
        if (!object || !qxp_d3d9_owner_equal(&object->owner, owner))
            continue;
        table->declarations[index] = NULL;
        return object;
    }
    return NULL;
}

QXP_D3D9_VERTEX_FUNCTION_OBJECT *QxpD3d9DetachOwnerVertexFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner)) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_VERTEX_FUNCTION_OBJECT *object;

        object = table->functions[index];
        if (!object || !qxp_d3d9_owner_equal(&object->owner, owner))
            continue;
        table->functions[index] = NULL;
        return object;
    }
    return NULL;
}

QXP_D3D9_PIXEL_FUNCTION_OBJECT *QxpD3d9DetachOwnerPixelFunction(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner)
{
    DWORD index;

    if (!table || !qxp_d3d9_owner_valid(owner)) return NULL;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        QXP_D3D9_PIXEL_FUNCTION_OBJECT *object;

        object = table->pixel_functions[index];
        if (!object || !qxp_d3d9_owner_equal(&object->owner, owner))
            continue;
        table->pixel_functions[index] = NULL;
        return object;
    }
    return NULL;
}

DWORD QxpD3d9VertexDeclarationCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    DWORD index;
    DWORD count;

    if (!table) return 0;
    count = 0;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        if (table->declarations[index]) ++count;
    }
    return count;
}

DWORD QxpD3d9VertexFunctionCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    DWORD index;
    DWORD count;

    if (!table) return 0;
    count = 0;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        if (table->functions[index]) ++count;
    }
    return count;
}

DWORD QxpD3d9PixelFunctionCount(
    const QXP_D3D9_SHADER_OBJECT_TABLE *table)
{
    DWORD index;
    DWORD count;

    if (!table) return 0;
    count = 0;
    for (index = 0; index < QXP_D3D9_SHADER_OBJECT_CAPACITY; ++index) {
        if (table->pixel_functions[index]) ++count;
    }
    return count;
}

BOOL QxpD3d9ShaderObjectStatusIsResourceFailure(
    const QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    return status &&
           (status->code == QXP_D3D9_SHADER_OBJECT_ERROR_ALLOCATION ||
            status->code == QXP_D3D9_SHADER_OBJECT_ERROR_TABLE_FULL ||
            status->code ==
                QXP_D3D9_SHADER_OBJECT_ERROR_GENERATION_EXHAUSTED);
}

static BOOL qxp_d3d9_batch_fail(
    DWORD command_offset, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status, DWORD code, DWORD byte_offset)
{
    qxp_d3d9_fail(status, code, byte_offset);
    if (error_offset) *error_offset = command_offset;
    return FALSE;
}

static BOOL qxp_d3d9_batch_propagate_fail(
    DWORD command_offset, DWORD *error_offset)
{
    if (error_offset) *error_offset = command_offset;
    return FALSE;
}

BOOL QxpD3d9ProcessShaderObjectBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_SHADER_BINDING *declaration_binding, DWORD *legacy_fvf,
    QXP_D3D9_SHADER_BINDING *function_binding,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const BYTE *cursor;
    QXP_D3D9_PRIVATE_HANDLE handle_record;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *prepared_declaration;
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *detached_declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *prepared_function;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *detached_function;
    DWORD handle;
    DWORD value;
    DWORD index;
    ULONG record_bytes;
    BOOL result;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !consumed_bytes || !error_offset ||
        (!records && record_count) || !table || !declaration_binding ||
        !legacy_fvf || !function_binding ||
        !qxp_d3d9_allocator_valid(allocator) || !lock_callback ||
        !unlock_callback) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    }
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (opcode <
            QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION ||
        opcode > QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_FUNCTION) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE, 0);
    }

    if (opcode != QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION &&
        opcode != QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION &&
        record_count > available_bytes / QXP_D3D9_HANDLE_RECORD_BYTES) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD,
            available_bytes);
    }

    cursor = (const BYTE *)records;
    for (index = 0; index < record_count; ++index) {
        if (opcode ==
                QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_DECLARATION) {
            prepared_declaration = NULL;
            handle = 0;
            value = 0;
            record_bytes = 0;
            result = QxpD3d9FrameCreateVertexDeclaration(
                cursor + *consumed_bytes,
                available_bytes - *consumed_bytes, &handle, &value,
                &record_bytes, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            result = QxpD3d9PrepareVertexDeclaration(
                owner, handle,
                (const QXP_VERTEX_ELEMENT9 *)(
                    cursor + *consumed_bytes +
                    QXP_D3D9_CREATE_RECORD_BYTES),
                value, allocator, &prepared_declaration, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            lock_callback(lock_opaque);
            result = QxpD3d9PublishVertexDeclaration(
                table, prepared_declaration, status);
            unlock_callback(lock_opaque);
            if (!result) {
                QxpD3d9DestroyVertexDeclaration(
                    prepared_declaration, allocator);
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            }
            *consumed_bytes += record_bytes;
            continue;
        }
        if (opcode == QXP_D3D9_PRIVATE_DP2OP_CREATE_VERTEX_FUNCTION) {
            prepared_function = NULL;
            handle = 0;
            value = 0;
            record_bytes = 0;
            result = QxpD3d9FrameCreateVertexFunction(
                cursor + *consumed_bytes,
                available_bytes - *consumed_bytes, &handle, &value,
                &record_bytes, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            result = QxpD3d9PrepareVertexFunction(
                owner, handle,
                (const DWORD *)(cursor + *consumed_bytes +
                    QXP_D3D9_CREATE_RECORD_BYTES),
                value, allocator, &prepared_function, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            lock_callback(lock_opaque);
            result = QxpD3d9PublishVertexFunction(
                table, prepared_function, status);
            unlock_callback(lock_opaque);
            if (!result) {
                QxpD3d9DestroyVertexFunction(prepared_function, allocator);
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            }
            *consumed_bytes += record_bytes;
            continue;
        }

        qxp_d3d9_copy(
            &handle_record, cursor + *consumed_bytes,
            (ULONG)sizeof(handle_record));
        if (opcode ==
                QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_DECLARATION) {
            detached_declaration = NULL;
            lock_callback(lock_opaque);
            result = QxpD3d9DeleteVertexDeclaration(
                table, owner, handle_record.handle, declaration_binding,
                &detached_declaration, status);
            unlock_callback(lock_opaque);
            if (detached_declaration)
                QxpD3d9DestroyVertexDeclaration(
                    detached_declaration, allocator);
        } else if (opcode ==
                       QXP_D3D9_PRIVATE_DP2OP_SET_VERTEX_DECLARATION) {
            lock_callback(lock_opaque);
            result = QxpD3d9SetVertexDeclaration(
                table, owner, handle_record.handle, declaration_binding,
                legacy_fvf, status);
            unlock_callback(lock_opaque);
        } else if (opcode ==
                       QXP_D3D9_PRIVATE_DP2OP_DELETE_VERTEX_FUNCTION) {
            detached_function = NULL;
            lock_callback(lock_opaque);
            result = QxpD3d9DeleteVertexFunction(
                table, owner, handle_record.handle, function_binding,
                &detached_function, status);
            unlock_callback(lock_opaque);
            if (detached_function)
                QxpD3d9DestroyVertexFunction(detached_function, allocator);
        } else {
            lock_callback(lock_opaque);
            result = QxpD3d9SetVertexFunction(
                table, owner, handle_record.handle, function_binding,
                status);
            unlock_callback(lock_opaque);
        }
        if (!result)
            return qxp_d3d9_batch_propagate_fail(
                command_offset, error_offset);
        *consumed_bytes += QXP_D3D9_HANDLE_RECORD_BYTES;
    }
    return TRUE;
}

BOOL QxpD3d9DestroyOwnerShaderObjects(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD *declaration_count, DWORD *function_count,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_D3D9_VERTEX_DECLARATION_OBJECT *declaration;
    QXP_D3D9_VERTEX_FUNCTION_OBJECT *function;

    if (declaration_count) *declaration_count = 0;
    if (function_count) *function_count = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !declaration_count || !function_count ||
        !qxp_d3d9_allocator_valid(allocator) || !lock_callback ||
        !unlock_callback) {
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    }
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);

    for (;;) {
        lock_callback(lock_opaque);
        declaration = QxpD3d9DetachOwnerVertexDeclaration(table, owner);
        unlock_callback(lock_opaque);
        if (!declaration) break;
        QxpD3d9DestroyVertexDeclaration(declaration, allocator);
        ++*declaration_count;
    }
    for (;;) {
        lock_callback(lock_opaque);
        function = QxpD3d9DetachOwnerVertexFunction(table, owner);
        unlock_callback(lock_opaque);
        if (!function) break;
        QxpD3d9DestroyVertexFunction(function, allocator);
        ++*function_count;
    }
    return TRUE;
}

BOOL QxpD3d9DispatchPixelFunctionBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes,
    const QXP_D3D9_PIXEL_DISPATCH_CALLBACKS *callbacks,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const BYTE *cursor;
    QXP_D3D9_PRIVATE_HANDLE handle_record;
    DWORD handle;
    DWORD bytecode_size;
    DWORD version_token;
    DWORD index;
    ULONG record_bytes;
    BOOL native;
    BOOL result;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !consumed_bytes || !error_offset ||
        (!records && record_count) || !callbacks || !callbacks->create ||
        !callbacks->set || !callbacks->delete_function) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    }
    if (opcode != QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION &&
        opcode != QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION &&
        opcode != QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE, 0);
    }
    if (opcode != QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION &&
        record_count > available_bytes / QXP_D3D9_HANDLE_RECORD_BYTES) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD,
            available_bytes);
    }

    cursor = (const BYTE *)records;
    for (index = 0; index < record_count; ++index) {
        if (opcode == QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION) {
            handle = 0;
            bytecode_size = 0;
            record_bytes = 0;
            result = QxpD3d9FrameCreatePixelFunction(
                cursor + *consumed_bytes,
                available_bytes - *consumed_bytes, &handle,
                &bytecode_size, &record_bytes, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            qxp_d3d9_copy(
                &version_token, cursor + *consumed_bytes +
                    QXP_D3D9_CREATE_RECORD_BYTES,
                (ULONG)sizeof(version_token));
            /* Route by what the D3D9 object representation can hold: both
             * ps_1_1 and ps_2_0 lower to the GLSL a native combined program
             * needs, while every other legacy profile stays on the ARB
             * pipeline that only fixed-function execution can consume. */
            native = QXP_SHADER_VERSION_IS_PIXEL(version_token);
            qxp_d3d9_reset_status(status);
            result = callbacks->create(
                handle, (const DWORD *)(cursor + *consumed_bytes +
                    QXP_D3D9_CREATE_RECORD_BYTES), bytecode_size, native,
                callbacks->opaque, status);
        } else {
            qxp_d3d9_copy(
                &handle_record, cursor + *consumed_bytes,
                (ULONG)sizeof(handle_record));
            qxp_d3d9_reset_status(status);
            if (opcode ==
                    QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION) {
                result = callbacks->delete_function(
                    handle_record.handle, callbacks->opaque, status);
            } else {
                result = callbacks->set(
                    handle_record.handle, callbacks->opaque, status);
            }
            record_bytes = QXP_D3D9_HANDLE_RECORD_BYTES;
        }
        if (!result) {
            if (status->code == QXP_D3D9_SHADER_OBJECT_OK) {
                qxp_d3d9_fail(
                    status, QXP_D3D9_SHADER_OBJECT_ERROR_BINDING, 0);
            }
            return qxp_d3d9_batch_propagate_fail(
                command_offset, error_offset);
        }
        *consumed_bytes += record_bytes;
    }
    qxp_d3d9_reset_status(status);
    return TRUE;
}

BOOL QxpD3d9ProcessPixelFunctionBatch(
    BYTE opcode, DWORD record_count, const void *records,
    ULONG available_bytes, const QXP_SHADER_OWNER *owner,
    QXP_D3D9_SHADER_OBJECT_TABLE *table,
    QXP_D3D9_SHADER_BINDING *pixel_function_binding,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD command_offset, ULONG *consumed_bytes, DWORD *error_offset,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    const BYTE *cursor;
    QXP_D3D9_PRIVATE_HANDLE handle_record;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *prepared;
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *detached;
    DWORD handle;
    DWORD bytecode_size;
    DWORD index;
    ULONG record_bytes;
    BOOL result;

    if (consumed_bytes) *consumed_bytes = 0;
    if (error_offset) *error_offset = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !consumed_bytes || !error_offset ||
        (!records && record_count) || !table || !pixel_function_binding ||
        !qxp_d3d9_allocator_valid(allocator) || !lock_callback ||
        !unlock_callback) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    }
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);
    if (opcode != QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION &&
        opcode != QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION &&
        opcode != QXP_D3D9_STANDARD_DP2OP_SET_PIXEL_FUNCTION) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_OPCODE, 0);
    }
    if (opcode != QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION &&
        record_count > available_bytes / QXP_D3D9_HANDLE_RECORD_BYTES) {
        return qxp_d3d9_batch_fail(
            command_offset, error_offset, status,
            QXP_D3D9_SHADER_OBJECT_ERROR_TRUNCATED_PAYLOAD,
            available_bytes);
    }

    cursor = (const BYTE *)records;
    for (index = 0; index < record_count; ++index) {
        if (opcode == QXP_D3D9_STANDARD_DP2OP_CREATE_PIXEL_FUNCTION) {
            prepared = NULL;
            handle = 0;
            bytecode_size = 0;
            record_bytes = 0;
            result = QxpD3d9FrameCreatePixelFunction(
                cursor + *consumed_bytes,
                available_bytes - *consumed_bytes, &handle,
                &bytecode_size, &record_bytes, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            result = QxpD3d9PreparePixelFunction(
                owner, handle,
                (const DWORD *)(cursor + *consumed_bytes +
                    QXP_D3D9_CREATE_RECORD_BYTES),
                bytecode_size, allocator, &prepared, status);
            if (!result)
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            lock_callback(lock_opaque);
            result = QxpD3d9PublishPixelFunction(
                table, prepared, status);
            unlock_callback(lock_opaque);
            if (!result) {
                QxpD3d9DestroyPixelFunction(prepared, allocator);
                return qxp_d3d9_batch_propagate_fail(
                    command_offset, error_offset);
            }
            *consumed_bytes += record_bytes;
            continue;
        }

        qxp_d3d9_copy(
            &handle_record, cursor + *consumed_bytes,
            (ULONG)sizeof(handle_record));
        if (opcode == QXP_D3D9_STANDARD_DP2OP_DELETE_PIXEL_FUNCTION) {
            detached = NULL;
            lock_callback(lock_opaque);
            result = QxpD3d9DeletePixelFunction(
                table, owner, handle_record.handle,
                pixel_function_binding, &detached, status);
            unlock_callback(lock_opaque);
            if (detached)
                QxpD3d9DestroyPixelFunction(detached, allocator);
        } else {
            lock_callback(lock_opaque);
            result = QxpD3d9SetPixelFunction(
                table, owner, handle_record.handle,
                pixel_function_binding, status);
            unlock_callback(lock_opaque);
        }
        if (!result)
            return qxp_d3d9_batch_propagate_fail(
                command_offset, error_offset);
        *consumed_bytes += QXP_D3D9_HANDLE_RECORD_BYTES;
    }
    return TRUE;
}

BOOL QxpD3d9DestroyOwnerPixelFunctions(
    QXP_D3D9_SHADER_OBJECT_TABLE *table, const QXP_SHADER_OWNER *owner,
    const QXP_D3D9_SHADER_ALLOCATOR *allocator,
    QXP_D3D9_SHADER_LOCK_CALLBACK lock_callback,
    QXP_D3D9_SHADER_LOCK_CALLBACK unlock_callback, void *lock_opaque,
    DWORD *pixel_function_count,
    QXP_D3D9_SHADER_OBJECT_STATUS *status)
{
    QXP_D3D9_PIXEL_FUNCTION_OBJECT *function;

    if (pixel_function_count) *pixel_function_count = 0;
    qxp_d3d9_reset_status(status);
    if (!status || !table || !pixel_function_count ||
        !qxp_d3d9_allocator_valid(allocator) || !lock_callback ||
        !unlock_callback) {
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_NULL_ARGUMENT, 0);
    }
    if (!qxp_d3d9_owner_valid(owner))
        return qxp_d3d9_fail(
            status, QXP_D3D9_SHADER_OBJECT_ERROR_OWNER, 0);

    for (;;) {
        lock_callback(lock_opaque);
        function = QxpD3d9DetachOwnerPixelFunction(table, owner);
        unlock_callback(lock_opaque);
        if (!function) break;
        QxpD3d9DestroyPixelFunction(function, allocator);
        ++*pixel_function_count;
    }
    return TRUE;
}
