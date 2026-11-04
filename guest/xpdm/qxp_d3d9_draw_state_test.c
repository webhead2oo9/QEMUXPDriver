#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qxp_d3d9_draw_state.h"

#define QXP_TEST_CANARY_A 0x13579bdfUL
#define QXP_TEST_CANARY_B 0xfdb97531UL
#define QXP_TEST_BUFFER_BYTES 8192UL

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        return 0; \
    } \
} while (0)

typedef struct qxp_test_guarded_state {
    DWORD before;
    QXP_D3D9_DRAW_STATE state;
    DWORD after;
} QXP_TEST_GUARDED_STATE;

static QXP_TEST_GUARDED_STATE qxp_guarded;
static QXP_D3D9_DRAW_STATE qxp_before;
static QXP_D3D9_DRAW_STATE qxp_second;
static QXP_D3D9_DRAW_STATE qxp_expected;
static BYTE qxp_buffer[QXP_TEST_BUFFER_BYTES];

typedef struct qxp_test_dispatch_state {
    DWORD allocations;
    DWORD releases;
    DWORD locks;
    DWORD unlocks;
    DWORD depth;
    DWORD failures;
    BOOL fail_allocation;
    QXP_D3D9_DRAW_STATE *race_state;
    DWORD race_handle;
    DWORD race_at_lock;
    DWORD record_calls;
    DWORD fail_record_call;
    DWORD recorded_registers[8];
    DWORD recorded_values[8];
} QXP_TEST_DISPATCH_STATE;

static QXP_TEST_DISPATCH_STATE qxp_dispatch;

static void *qxp_test_allocate(ULONG byte_count, void *opaque)
{
    QXP_TEST_DISPATCH_STATE *state =
        (QXP_TEST_DISPATCH_STATE *)opaque;

    ++state->allocations;
    if (state->fail_allocation) return NULL;
    return malloc((size_t)byte_count);
}

static void qxp_test_release(void *allocation, void *opaque)
{
    QXP_TEST_DISPATCH_STATE *state =
        (QXP_TEST_DISPATCH_STATE *)opaque;

    ++state->releases;
    free(allocation);
}

static void qxp_test_lock(void *opaque)
{
    QXP_TEST_DISPATCH_STATE *state =
        (QXP_TEST_DISPATCH_STATE *)opaque;

    ++state->locks;
    if (state->depth) ++state->failures;
    ++state->depth;
    if (state->race_state && state->locks == state->race_at_lock) {
        QxpD3d9DrawStateUnbindSurface(
            state->race_state, state->race_handle);
        state->race_state = NULL;
    }
}

static void qxp_test_unlock(void *opaque)
{
    QXP_TEST_DISPATCH_STATE *state =
        (QXP_TEST_DISPATCH_STATE *)opaque;

    ++state->unlocks;
    if (!state->depth) {
        ++state->failures;
        return;
    }
    --state->depth;
}

static BOOL qxp_test_record_float(
    DWORD register_index, const DWORD *values, void *opaque)
{
    QXP_TEST_DISPATCH_STATE *state =
        (QXP_TEST_DISPATCH_STATE *)opaque;
    DWORD call;

    call = state->record_calls++;
    if (call < 8) {
        state->recorded_registers[call] = register_index;
        state->recorded_values[call] = values[0];
    }
    return !state->fail_record_call ||
           state->record_calls != state->fail_record_call;
}

static int qxp_status_zero(const QXP_D3D9_DRAW_STATE_STATUS *status)
{
    QXP_D3D9_DRAW_STATE_STATUS zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(status, &zero, sizeof(zero)) == 0;
}

static int qxp_state_equal(const QXP_D3D9_DRAW_STATE *left,
                           const QXP_D3D9_DRAW_STATE *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static void qxp_reset_guarded(void)
{
    memset(&qxp_guarded, 0xa5, sizeof(qxp_guarded));
    qxp_guarded.before = QXP_TEST_CANARY_A;
    qxp_guarded.after = QXP_TEST_CANARY_B;
    QxpD3d9DrawStateInitialize(&qxp_guarded.state);
}

static int qxp_canaries_ok(void)
{
    return qxp_guarded.before == QXP_TEST_CANARY_A &&
           qxp_guarded.after == QXP_TEST_CANARY_B;
}

static ULONG qxp_append_dword(ULONG offset, DWORD value)
{
    if (offset <= QXP_TEST_BUFFER_BYTES - sizeof(value))
        memcpy(qxp_buffer + offset, &value, sizeof(value));
    return offset + sizeof(value);
}

static ULONG qxp_append_constant_record(
    ULONG offset, DWORD register_index, DWORD count,
    DWORD element_dwords, DWORD seed)
{
    DWORD index;

    offset = qxp_append_dword(offset, register_index);
    offset = qxp_append_dword(offset, count);
    for (index = 0; index < count * element_dwords; ++index)
        offset = qxp_append_dword(offset, seed + index);
    return offset;
}

static int qxp_test_initialize(void)
{
    DWORD stream;
    QXP_D3D9_DRAW_STATE expected;

    qxp_reset_guarded();
    memset(&expected, 0, sizeof(expected));
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream)
        expected.streams[stream].divider = 1;
    CHECK(qxp_state_equal(&qxp_guarded.state, &expected));
    CHECK(qxp_canaries_ok());
    QxpD3d9DrawStateInitialize(NULL);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_bindings(void)
{
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_DRAW_STATE_STATUS status;

    qxp_reset_guarded();
    memset(&binding, 0, sizeof(binding));
    memset(&status, 0xa5, sizeof(status));
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_NONE,
        &binding, 0, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_NONE);

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x112UL, &status));
    CHECK(qxp_guarded.state.legacy_fvf == 0x112UL);
    CHECK(!qxp_guarded.state.declaration_binding.handle);

    binding.handle = 0x101UL;
    binding.generation = 9;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(qxp_guarded.state.declaration_binding.handle == 0x101UL);
    CHECK(qxp_guarded.state.declaration_binding.generation == 9);
    CHECK(!qxp_guarded.state.legacy_fvf);
    qxp_before = qxp_guarded.state;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &qxp_guarded.state.declaration_binding, 0, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    qxp_before = qxp_guarded.state;
    binding.handle = 0x100UL;
    binding.generation = 10;
    CHECK(!QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_DECLARATION);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    binding.handle = 0x101UL;
    binding.generation = 0;
    CHECK(!QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    binding.handle = 0;
    binding.generation = 0;
    CHECK(!QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x113UL, &status));
    CHECK(!QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, 99, &binding, 0, &status));
    CHECK(!QxpD3d9DrawStateSetDeclaration(
        NULL, QXP_D3D9_DRAW_DECLARATION_NONE,
        &binding, 0, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);

    binding.handle = 44;
    binding.generation = 55;
    CHECK(QxpD3d9DrawStateSetVertexFunction(
        &qxp_guarded.state, &binding, &status));
    CHECK(qxp_guarded.state.vertex_function_binding.handle == 44);
    qxp_before = qxp_guarded.state;
    CHECK(QxpD3d9DrawStateSetVertexFunction(
        &qxp_guarded.state,
        &qxp_guarded.state.vertex_function_binding, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    QxpD3d9DrawStateInitialize(&qxp_before);
    QxpD3d9DrawStateInitialize(&qxp_second);
    memset(&binding, 0x11, sizeof(binding));
    binding.handle = 0x101UL;
    binding.generation = 17;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_before, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(QxpD3d9DrawStateSetVertexFunction(
        &qxp_before, &binding, &status));
    memset(&binding, 0xee, sizeof(binding));
    binding.handle = 0x101UL;
    binding.generation = 17;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_second, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(QxpD3d9DrawStateSetVertexFunction(
        &qxp_second, &binding, &status));
    CHECK(qxp_state_equal(&qxp_before, &qxp_second));

    binding.handle = 44;
    binding.generation = 55;
    qxp_before = qxp_guarded.state;
    binding.generation = 0;
    CHECK(!QxpD3d9DrawStateSetVertexFunction(
        &qxp_guarded.state, &binding, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_FUNCTION);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    qxp_reset_guarded();
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.indices.stride = 2;
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.indices.stride = 0;
    qxp_guarded.state.declaration_binding.handle = 0x121UL;
    qxp_guarded.state.declaration_binding.generation = 71;
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    CHECK(QxpD3d9DrawStateSynchronizeObjectBindings(
        &qxp_guarded.state, &status));
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_OBJECT);
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    binding = qxp_guarded.state.declaration_binding;
    CHECK(QxpD3d9DrawStateSetTranslatedDeclaration(
        &qxp_guarded.state, &binding, 0x102UL, 20, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0x220UL, 16, 20, FALSE, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 1, 0x221UL, 4, 20, FALSE, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 1, 0x221UL, 0, 20, FALSE, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.vertex_function_binding.handle = 0x301UL;
    qxp_guarded.state.vertex_function_binding.generation = 72;
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    memset(&qxp_guarded.state.vertex_function_binding, 0,
           sizeof(qxp_guarded.state.vertex_function_binding));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0x220UL, 0, 20, FALSE, &status));

    memset(&qxp_guarded.state.declaration_binding, 0,
           sizeof(qxp_guarded.state.declaration_binding));
    qxp_guarded.state.legacy_fvf = 0x112UL;
    CHECK(QxpD3d9DrawStateSynchronizeObjectBindings(
        &qxp_guarded.state, &status));
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_FVF);
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.legacy_fvf = 0;
    qxp_guarded.state.vertex_function_binding.handle = 88;
    qxp_guarded.state.vertex_function_binding.generation = 89;
    CHECK(QxpD3d9DrawStateSynchronizeObjectBindings(
        &qxp_guarded.state, &status));
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_NONE);
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    memset(&qxp_guarded.state.vertex_function_binding, 0,
           sizeof(qxp_guarded.state.vertex_function_binding));
    qxp_guarded.state.declaration_mode =
        QXP_D3D9_DRAW_DECLARATION_NONE;
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.declaration_binding.handle = 0x123UL;
    qxp_guarded.state.declaration_binding.generation = 91;
    qxp_guarded.state.legacy_fvf = 0x112UL;
    qxp_before = qxp_guarded.state;
    CHECK(!QxpD3d9DrawStateSynchronizeObjectBindings(
        &qxp_guarded.state, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_DECLARATION);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    CHECK(!QxpD3d9DrawStateSynchronizeObjectBindings(NULL, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(NULL));
    CHECK(qxp_canaries_ok());
    return 1;
}

/*
 * Direct3D never runs a vertex shader for a pre-transformed declaration, so
 * a handle left bound across such a draw is dormant.  Treating it as live
 * forced the native path, which has no pre-transformed route and refused
 * the draw outright - the legacy FVF path draws XYZRHW vertices already.
 */
static int qxp_test_pretransformed_vertex_binding_is_dormant(void)
{
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_DRAW_STATE_STATUS status;

    qxp_reset_guarded();
    memset(&binding, 0, sizeof(binding));
    memset(&status, 0xa5, sizeof(status));

    qxp_guarded.state.declaration_binding.handle = 0x141UL;
    qxp_guarded.state.declaration_binding.generation = 11;
    CHECK(QxpD3d9DrawStateSynchronizeObjectBindings(
        &qxp_guarded.state, &status));
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_OBJECT);
    binding = qxp_guarded.state.declaration_binding;

    /* XYZRHW | TEX1: pre-transformed, so the bound shader cannot run. */
    CHECK(QxpD3d9DrawStateSetTranslatedDeclaration(
        &qxp_guarded.state, &binding, 0x104UL, 24, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.vertex_function_binding.handle = 0x301UL;
    qxp_guarded.state.vertex_function_binding.generation = 12;
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    /* XYZ | TEX1 is transformed by the shader, so the same binding is live
     * and the draw must still take the native path. */
    CHECK(QxpD3d9DrawStateSetTranslatedDeclaration(
        &qxp_guarded.state, &binding, 0x102UL, 20, &status));
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    /* The stride is what the legacy path consumes, so a translation missing
     * it does not qualify and the bound shader stays live. */
    CHECK(QxpD3d9DrawStateSetTranslatedDeclaration(
        &qxp_guarded.state, &binding, 0x104UL, 24, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    qxp_guarded.state.translated_vertex_stride = 0;
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_legacy_fvf_resolution(void)
{
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_DRAW_STATE_STATUS status;

    memset(&binding, 0, sizeof(binding));
    qxp_reset_guarded();
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x0c4UL, &status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x0c4UL);
    /* r529 retained XYZRHW|DIFFUSE|SPECULAR from the prior draw while the
     * DrawPrimitiveUP call supplied XYZRHW|DIFFUSE in dwVertexType. */
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0x044UL) == 0x044UL);
    CHECK(qxp_guarded.state.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_FVF &&
          qxp_guarded.state.legacy_fvf == 0x0c4UL);

    qxp_reset_guarded();
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0x044UL) == 0x044UL);
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x0c4UL);

    binding.handle =
        QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT | 7UL;
    binding.generation = 9;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    CHECK(QxpD3d9DrawStateSetTranslatedDeclaration(
        &qxp_guarded.state, &binding, 0x102UL, 20, &status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0x044UL) == 0x102UL);
    qxp_guarded.state.translated_declaration_binding.generation = 8;
    CHECK(!QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0x044UL));
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_hwtl_fvf_publication_sequence(void)
{
    QXP_D3D9_SHADER_BINDING binding;
    QXP_D3D9_DRAW_STATE_STATUS status;
    QXP_D3D9_DRAW_STATE before;

    /* The r530 D3dVisual probe published exactly this fixed-function FVF
     * order before the rejected HW TnL draws: specular, minimal point-size,
     * fog, vertex-fog specular again, then the object-space HW TnL FVF.  The
     * resolver must always answer with the last publication while a call
     * without dwVertexType keeps the persistent mirror authoritative only
     * when no FVF-mode declaration exists. */
    memset(&binding, 0, sizeof(binding));
    qxp_reset_guarded();

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x0c4UL, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x0c4UL);

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x404UL, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x404UL);

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x044UL, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x044UL);

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x0c4UL, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x0c4UL);

    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x052UL, &status));
    CHECK(qxp_status_zero(&status));

    /* The HW TnL scene drew over the runtime ring with dwVertexType zero, so
     * the last published FVF decides - never an earlier one, and never the
     * stale persistent mirror. */
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x0c4UL, 0) == 0x052UL);
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x052UL, 0) == 0x052UL);

    /* The runtime bound its transform ring as an ordinary stream-0 vertex
     * buffer; the observed calls carried USERMEMVERTICES without UM source
     * authority, so the buffered stride stays authoritative. */
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 8, 0, 32, FALSE, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x052UL, 0) == 0x052UL);

    /* A call copy for the observed shape (dwVertexType zero, no UM stride)
     * must keep the published declaration and the ring binding while leaving
     * persistent state byte-exact. */
    before = qxp_guarded.state;
    CHECK(QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0, 0, &qxp_second, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &before));
    CHECK(qxp_second.declaration_mode ==
          QXP_D3D9_DRAW_DECLARATION_FVF);
    CHECK(qxp_second.legacy_fvf == 0x052UL);
    CHECK(!qxp_second.streams[0].user_memory);
    CHECK(qxp_second.streams[0].handle == 8);
    CHECK(qxp_second.streams[0].stride == 32);

    /* If a later replay publishes the older specular FVF again, the resolver
     * answers 0xc4: any such value at a draw therefore proves a real
     * SETVERTEXSHADER or state-block publication happened after the scene's
     * own bind, which is exactly what the driver history dump attributes. */
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_FVF,
        &binding, 0x0c4UL, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(QxpD3d9ResolveLegacyFvf(
        &qxp_guarded.state, 0x052UL, 0) == 0x0c4UL);

    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_user_memory_draw_state(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    QXP_D3D9_SHADER_BINDING binding;
    ULONGLONG generation;

    /* A real UM draw is bound by SETSTREAMSOURCEUM.  The immutable call
     * snapshot validates the outer vertex size but never republishes or
     * advances that already-authoritative binding. */
    qxp_reset_guarded();
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0, 0, 32, TRUE, &status));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 1, 9, 4, 24, FALSE, &status));
    qxp_guarded.state.vs_float[17][2] = 0x12345678UL;
    qxp_before = qxp_guarded.state;
    generation = qxp_before.stream_source_generation;

    CHECK(QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0x164UL, 32,
        &qxp_second, &status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    qxp_expected = qxp_before;
    qxp_expected.declaration_mode = QXP_D3D9_DRAW_DECLARATION_FVF;
    qxp_expected.legacy_fvf = 0x164UL;
    CHECK(qxp_state_equal(&qxp_second, &qxp_expected));
    CHECK(qxp_second.stream_source_generation == generation);
    CHECK(qxp_status_zero(&status));

    /* USERMEMVERTICES is outer-call metadata, not source authority.  A
     * surface binding therefore remains exact while the call FVF is applied. */
    qxp_reset_guarded();
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 7, 6, 32, FALSE, &status));
    qxp_before = qxp_guarded.state;
    CHECK(QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0x164UL, 0,
        &qxp_second, &status));
    qxp_expected = qxp_before;
    qxp_expected.declaration_mode = QXP_D3D9_DRAW_DECLARATION_FVF;
    qxp_expected.legacy_fvf = 0x164UL;
    CHECK(qxp_state_equal(&qxp_second, &qxp_expected));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    /* A native DDI-v4 declaration object is authoritative even if the outer
     * call structure carries a legacy field value. */
    memset(&binding, 0, sizeof(binding));
    binding.handle =
        QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT | 7UL;
    binding.generation = 9;
    CHECK(QxpD3d9DrawStateSetDeclaration(
        &qxp_guarded.state, QXP_D3D9_DRAW_DECLARATION_OBJECT,
        &binding, 0, &status));
    qxp_before = qxp_guarded.state;
    CHECK(QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0x164UL, 0,
        &qxp_second, &status));
    CHECK(qxp_state_equal(&qxp_second, &qxp_before));

    /* A bound UM stream requires the exact outer vertex size. */
    qxp_reset_guarded();
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0, 0, 32, TRUE, &status));
    qxp_before = qxp_guarded.state;
    qxp_second = qxp_before;
    CHECK(!QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0, 0, &qxp_second, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE);
    CHECK(qxp_state_equal(&qxp_second, &qxp_before));
    CHECK(!QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0, 16, &qxp_second, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE);
    CHECK(qxp_state_equal(&qxp_second, &qxp_before));

    /* No generation is consumed by an immutable per-call snapshot. */
    qxp_guarded.state.stream_source_generation = ~(ULONGLONG)0;
    qxp_before = qxp_guarded.state;
    CHECK(QxpD3d9BuildCallDrawState(
        &qxp_guarded.state, 0, 32, &qxp_second, &status));
    CHECK(qxp_state_equal(&qxp_second, &qxp_before));
    CHECK(qxp_status_zero(&status));

    CHECK(!QxpD3d9BuildCallDrawState(
        NULL, 0, 0, &qxp_second, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);
    CHECK(!QxpD3d9BuildCallDrawState(
        &qxp_before, 0, 32, NULL, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);

    qxp_reset_guarded();
    qxp_before = qxp_guarded.state;
    qxp_second = qxp_before;
    CHECK(!QxpD3d9BuildCallDrawState(
        &qxp_guarded.state,
        QXP_D3D9_VERTEX_DECLARATION_HANDLE_PRIVATE_BIT | 3UL,
        0, &qxp_second, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_DECLARATION);
    CHECK(qxp_state_equal(&qxp_second, &qxp_before));
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_streams(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    DWORD stream;
    ULONGLONG generation;

    qxp_reset_guarded();
    for (stream = 0; stream < QXP_D3D9_DRAW_STREAM_COUNT; ++stream) {
        CHECK(QxpD3d9DrawStateSetStreamSource(
            &qxp_guarded.state, stream, 100 + stream,
            4 * stream, stream == 3 ? 0 : 16 + stream,
            FALSE, &status));
        CHECK(qxp_guarded.state.streams[stream].handle == 100 + stream);
        CHECK(qxp_guarded.state.streams[stream].offset == 4 * stream);
    }
    CHECK(qxp_guarded.state.streams[3].stride == 0);
    qxp_before = qxp_guarded.state;
    CHECK(!QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 16, 1, 0, 4, FALSE, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM);
    CHECK(status.stream == 16);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(!QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 1, 0, 0, 12, TRUE, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE);
    CHECK(!QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 1, 0, 12, TRUE, &status));
    CHECK(!QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0, 4, 12, TRUE, &status));
    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 0, 0, 0, 12, TRUE, &status));
    CHECK(qxp_guarded.state.streams[0].user_memory);

    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 0, 1, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 1, 7, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 0,
        QXP_D3D9_STREAM_SOURCE_INDEXED_DATA | 3UL, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 1,
        QXP_D3D9_STREAM_SOURCE_INDEXED_DATA, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 2,
        QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA | 2UL, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 3,
        QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA, &status));
    qxp_before = qxp_guarded.state;
    CHECK(!QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 0, 0, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY);
    CHECK(!QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 0,
        QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA | 1UL, &status));
    CHECK(!QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 1,
        QXP_D3D9_STREAM_SOURCE_FLAG_MASK | 1UL, &status));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    CHECK(QxpD3d9DrawStateSetIndices(
        &qxp_guarded.state, 0x776UL, 2, &status));
    CHECK(qxp_guarded.state.indices.handle == 0x776UL &&
          qxp_guarded.state.indices.stride == 2);
    qxp_before = qxp_guarded.state;
    CHECK(!QxpD3d9DrawStateSetIndices(
        &qxp_guarded.state, 0x776UL, 3, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(QxpD3d9DrawStateSetIndices(
        &qxp_guarded.state, 0, 99, &status));
    CHECK(!qxp_guarded.state.indices.handle &&
          !qxp_guarded.state.indices.stride);

    CHECK(QxpD3d9DrawStateSetStreamSource(
        &qxp_guarded.state, 4, 0x777UL, 24, 32, FALSE, &status));
    CHECK(QxpD3d9DrawStateSetStreamFrequency(
        &qxp_guarded.state, 4, 5, &status));
    CHECK(QxpD3d9DrawStateSetIndices(
        &qxp_guarded.state, 0x777UL, 4, &status));
    generation = qxp_guarded.state.stream_source_generation;
    QxpD3d9DrawStateUnbindSurface(&qxp_guarded.state, 0x777UL);
    CHECK(!qxp_guarded.state.streams[4].handle);
    CHECK(!qxp_guarded.state.streams[4].offset);
    CHECK(!qxp_guarded.state.streams[4].stride);
    CHECK(qxp_guarded.state.streams[4].divider == 5);
    CHECK(!qxp_guarded.state.indices.handle &&
          !qxp_guarded.state.indices.stride);
    CHECK(qxp_guarded.state.stream_source_generation == generation + 1);
    generation = qxp_guarded.state.stream_source_generation;
    QxpD3d9DrawStateUnbindSurface(&qxp_guarded.state, 0x778UL);
    CHECK(qxp_guarded.state.stream_source_generation == generation + 1);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_constants(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    DWORD values[16];
    DWORD index;

    qxp_reset_guarded();
    for (index = 0; index < 16; ++index) values[index] = 0x1000UL + index;
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 255, 1, values, &status));
    CHECK(qxp_guarded.state.vs_float[255][3] == values[3]);
    CHECK(qxp_guarded.state.vs_float_set_mask[7] == 0x80000000UL);
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_INTEGER, 14, 2, values, &status));
    CHECK(qxp_guarded.state.vs_integer[15][3] == values[7]);
    CHECK(qxp_guarded.state.vs_integer_set_mask == 0xc000UL);
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_BOOLEAN, 12, 4, values, &status));
    CHECK(qxp_guarded.state.vs_boolean[15] == values[3]);
    CHECK(qxp_guarded.state.vs_boolean_set_mask == 0xf000UL);
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 30, 2, values, &status));
    CHECK(qxp_guarded.state.ps_float[31][3] == values[7]);
    CHECK(qxp_guarded.state.ps_float_set_mask == 0xc0000000UL);
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_INTEGER, 15, 1, values, &status));
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_BOOLEAN, 15, 1, values, &status));

    qxp_before = qxp_guarded.state;
    CHECK(!QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, 3, QXP_D3D9_DRAW_CONSTANT_FLOAT,
        0, 1, values, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STAGE);
    CHECK(!QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        9, 0, 1, values, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_KIND);
    CHECK(!QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 0, 0, values, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_COUNT);
    CHECK(!QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 255, 2, values, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_RANGE);
    CHECK(!QxpD3d9DrawStateSetConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 0, 1, NULL, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_fixed_prefixes(
    BYTE opcode, const void *records, ULONG record_size)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    DWORD error_offset;
    ULONG prefix;
    ULONG total;

    total = 2 * record_size;
    qxp_reset_guarded();
    qxp_before = qxp_guarded.state;
    for (prefix = 0; prefix < total; ++prefix) {
        qxp_guarded.state = qxp_before;
        CHECK(!QxpD3d9ProcessDrawStateBatch(
            opcode, 2, records, prefix, &qxp_guarded.state,
            0x4455UL, &consumed, &error_offset, &status));
        CHECK(!consumed && error_offset == 0x4455UL);
        CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD);
        CHECK(status.record_index == prefix / record_size);
        CHECK(status.byte_offset ==
              (prefix / record_size) * record_size);
        CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    }
    return 1;
}

static int qxp_test_fixed_batches(void)
{
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE2 source2[2];
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE source[2];
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM source_um[2];
    QXP_D3D9_DRAW_DP2_INDICES indices[2];
    QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY frequency[2];
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    DWORD error_offset;

    qxp_reset_guarded();
    source2[0].stream = 0;
    source2[0].handle = 100;
    source2[0].offset = 12;
    source2[0].stride = 24;
    source2[1].stream = 15;
    source2[1].handle = 200;
    source2[1].offset = 20;
    source2[1].stride = 0;
    CHECK(qxp_test_fixed_prefixes(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, source2,
        sizeof(source2[0])));
    qxp_reset_guarded();
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 2,
        source2, sizeof(source2), &qxp_guarded.state, 0x1234UL,
        &consumed, &error_offset, &status));
    CHECK(consumed == sizeof(source2));
    CHECK(!error_offset && qxp_status_zero(&status));
    CHECK(qxp_guarded.state.streams[0].offset == 12);
    CHECK(qxp_guarded.state.streams[15].stride == 0);
    qxp_before = qxp_guarded.state;
    source2[1].stream = 16;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 2,
        source2, sizeof(source2), &qxp_guarded.state, 77,
        &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 77);
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM);
    CHECK(status.record_index == 1 && status.stream == 16);
    CHECK(status.byte_offset == sizeof(source2[0]));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    source[0].stream = 3;
    source[0].handle = 333;
    source[0].stride = 28;
    source[1].stream = 4;
    source[1].handle = 444;
    source[1].stride = 32;
    CHECK(qxp_test_fixed_prefixes(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, source,
        sizeof(source[0])));
    qxp_reset_guarded();
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 2,
        source, sizeof(source), &qxp_guarded.state, 1,
        &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[3].handle == 333);
    CHECK(qxp_guarded.state.streams[4].stride == 32);
    CHECK(!qxp_guarded.state.streams[3].offset);
    qxp_before = qxp_guarded.state;
    source[1].stream = 16;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 2,
        source, sizeof(source), &qxp_guarded.state, 0x1001UL,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM);
    CHECK(status.record_index == 1);
    CHECK(status.byte_offset == sizeof(source[0]));
    CHECK(!consumed && error_offset == 0x1001UL);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    source_um[0].stream = 0;
    source_um[0].stride = 36;
    source_um[1].stream = 0;
    source_um[1].stride = 40;
    CHECK(qxp_test_fixed_prefixes(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM, source_um,
        sizeof(source_um[0])));
    qxp_reset_guarded();
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM, 2,
        source_um, sizeof(source_um), &qxp_guarded.state, 1,
        &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[0].user_memory);
    CHECK(qxp_guarded.state.streams[0].stride == 40);
    qxp_before = qxp_guarded.state;
    source_um[1].stream = 1;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM, 2,
        source_um, sizeof(source_um), &qxp_guarded.state, 0x1002UL,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_SOURCE);
    CHECK(status.record_index == 1);
    CHECK(status.byte_offset == sizeof(source_um[0]));
    CHECK(!consumed && error_offset == 0x1002UL);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    frequency[0].stream = 1;
    frequency[0].divider = QXP_D3D9_STREAM_SOURCE_INDEXED_DATA;
    frequency[1].stream = 2;
    frequency[1].divider = QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA;
    CHECK(qxp_test_fixed_prefixes(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, frequency,
        sizeof(frequency[0])));
    qxp_reset_guarded();
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 2,
        frequency, sizeof(frequency), &qxp_guarded.state, 3,
        &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[1].divider == frequency[0].divider);
    CHECK(qxp_guarded.state.streams[2].divider == frequency[1].divider);
    frequency[0].stream = 0;
    frequency[0].divider = QXP_D3D9_STREAM_SOURCE_INDEXED_DATA | 4UL;
    frequency[1].stream = 1;
    frequency[1].divider = QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA | 2UL;
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 2,
        frequency, sizeof(frequency), &qxp_guarded.state, 4,
        &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[0].divider == frequency[0].divider);
    CHECK(qxp_guarded.state.streams[1].divider == frequency[1].divider);
    qxp_before = qxp_guarded.state;
    frequency[1].stream = 0;
    frequency[1].divider = QXP_D3D9_STREAM_SOURCE_INSTANCE_DATA;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 2,
        frequency, sizeof(frequency), &qxp_guarded.state, 0x1003UL,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM_FREQUENCY);
    CHECK(status.record_index == 1);
    CHECK(status.byte_offset == sizeof(frequency[0]));
    CHECK(!consumed && error_offset == 0x1003UL);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    indices[0].handle = 501;
    indices[0].stride = 2;
    indices[1].handle = 502;
    indices[1].stride = 4;
    CHECK(qxp_test_fixed_prefixes(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, indices,
        sizeof(indices[0])));
    qxp_reset_guarded();
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 2,
        indices, sizeof(indices), &qxp_guarded.state, 0x1004UL,
        &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.indices.handle == 502 &&
          qxp_guarded.state.indices.stride == 4);
    qxp_before = qxp_guarded.state;
    indices[1].stride = 3;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 2,
        indices, sizeof(indices), &qxp_guarded.state, 0x1005UL,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_INDEX_SOURCE &&
          status.record_index == 1 &&
          status.byte_offset == sizeof(indices[0]));
    CHECK(!consumed && error_offset == 0x1005UL);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    indices[0].handle = 0;
    indices[0].stride = 99;
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 1,
        indices, sizeof(indices[0]), &qxp_guarded.state, 0x1006UL,
        &consumed, &error_offset, &status));
    CHECK(!qxp_guarded.state.indices.handle &&
          !qxp_guarded.state.indices.stride);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_fixed_alias_input(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    DWORD error_offset;
    const void *records;

    qxp_reset_guarded();
    /* Two SOURCE2 records occupy the first 32 bytes of streams[].  Applying
     * record zero directly would overwrite record one's handle before it was
     * decoded.  The implementation must decode into its bounded candidate. */
    qxp_guarded.state.streams[0].handle = 1;
    qxp_guarded.state.streams[0].offset = 111;
    qxp_guarded.state.streams[0].stride = 12;
    qxp_guarded.state.streams[0].divider = 24;
    qxp_guarded.state.streams[0].user_memory = 2;
    qxp_guarded.state.streams[0].source2 = 222;
    qxp_guarded.state.streams[1].handle = 16;
    qxp_guarded.state.streams[1].offset = 32;
    records = (const void *)qxp_guarded.state.streams;
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 2,
        records, 2 * sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE2),
        &qxp_guarded.state, 0x7777UL,
        &consumed, &error_offset, &status));
    CHECK(consumed == 2 * sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE2));
    CHECK(!error_offset && qxp_status_zero(&status));
    CHECK(qxp_guarded.state.streams[1].handle == 111);
    CHECK(qxp_guarded.state.streams[1].offset == 12);
    CHECK(qxp_guarded.state.streams[1].stride == 24);
    CHECK(qxp_guarded.state.streams[1].divider == 1);
    CHECK(!qxp_guarded.state.streams[1].user_memory);
    CHECK(qxp_guarded.state.streams[1].source2);
    CHECK(qxp_guarded.state.streams[2].handle == 222);
    CHECK(qxp_guarded.state.streams[2].offset == 16);
    CHECK(qxp_guarded.state.streams[2].stride == 32);
    CHECK(qxp_guarded.state.streams[2].divider == 1);
    CHECK(!qxp_guarded.state.streams[2].user_memory);
    CHECK(qxp_guarded.state.streams[2].source2);
    CHECK(qxp_canaries_ok());
    return 1;
}

static DWORD qxp_test_constant_value(
    const QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD kind,
    DWORD register_index, DWORD component)
{
    if (stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX) {
        if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT)
            return state->vs_float[register_index][component];
        if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER)
            return state->vs_integer[register_index][component];
        return state->vs_boolean[register_index];
    }
    if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT)
        return state->ps_float[register_index][component];
    if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER)
        return state->ps_integer[register_index][component];
    return state->ps_boolean[register_index];
}

static DWORD qxp_test_constant_mask(
    const QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD kind,
    DWORD word)
{
    if (stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX) {
        if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT)
            return state->vs_float_set_mask[word];
        if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER)
            return state->vs_integer_set_mask;
        return state->vs_boolean_set_mask;
    }
    if (kind == QXP_D3D9_DRAW_CONSTANT_FLOAT)
        return state->ps_float_set_mask;
    if (kind == QXP_D3D9_DRAW_CONSTANT_INTEGER)
        return state->ps_integer_set_mask;
    return state->ps_boolean_set_mask;
}

static int qxp_test_other_constant_masks_zero(
    const QXP_D3D9_DRAW_STATE *state, DWORD stage, DWORD kind)
{
    DWORD word;

    for (word = 0; word < 8; ++word) {
        if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX &&
              kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) &&
            state->vs_float_set_mask[word]) return 0;
    }
    if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX &&
          kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) &&
        state->vs_integer_set_mask) return 0;
    if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_VERTEX &&
          kind == QXP_D3D9_DRAW_CONSTANT_BOOLEAN) &&
        state->vs_boolean_set_mask) return 0;
    if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_PIXEL &&
          kind == QXP_D3D9_DRAW_CONSTANT_FLOAT) &&
        state->ps_float_set_mask) return 0;
    if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_PIXEL &&
          kind == QXP_D3D9_DRAW_CONSTANT_INTEGER) &&
        state->ps_integer_set_mask) return 0;
    if (!(stage == QXP_D3D9_DRAW_SHADER_STAGE_PIXEL &&
          kind == QXP_D3D9_DRAW_CONSTANT_BOOLEAN) &&
        state->ps_boolean_set_mask) return 0;
    return 1;
}

static int qxp_test_constant_batch_one(
    BYTE opcode, DWORD stage, DWORD kind, DWORD register_index,
    DWORD register_limit, DWORD element_dwords)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG first_end;
    ULONG total;
    ULONG prefix;
    ULONG consumed;
    DWORD error_offset;

    memset(qxp_buffer, 0, sizeof(qxp_buffer));
    first_end = qxp_append_constant_record(
        0, register_index, 1, element_dwords, 0x2000UL + opcode);
    total = qxp_append_constant_record(
        first_end, register_limit - 1, 1,
        element_dwords, 0x3000UL + opcode);
    CHECK(total <= sizeof(qxp_buffer));
    qxp_reset_guarded();
    qxp_before = qxp_guarded.state;
    qxp_second = qxp_before;
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_second, stage, kind, register_index, 1,
        qxp_buffer + sizeof(QXP_D3D9_DRAW_DP2_CONSTANT), &status));
    qxp_expected = qxp_second;
    CHECK(QxpD3d9DrawStateSetConstants(
        &qxp_expected, stage, kind, register_limit - 1, 1,
        qxp_buffer + first_end + sizeof(QXP_D3D9_DRAW_DP2_CONSTANT),
        &status));
    for (prefix = 0; prefix < first_end; ++prefix) {
        qxp_guarded.state = qxp_before;
        CHECK(!QxpD3d9ProcessDrawStateBatch(
            opcode, 2, qxp_buffer, prefix, &qxp_guarded.state,
            0x9000UL + opcode, &consumed, &error_offset, &status));
        CHECK(!consumed);
        CHECK(error_offset == 0x9000UL + opcode);
        CHECK(status.record_index == 0);
        CHECK(status.byte_offset == 0);
        CHECK(status.stage == stage);
        CHECK(status.constant_kind == kind);
        CHECK(status.code ==
              (prefix < sizeof(QXP_D3D9_DRAW_DP2_CONSTANT) ?
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD :
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD));
        if (prefix >= sizeof(QXP_D3D9_DRAW_DP2_CONSTANT))
            CHECK(status.register_index == register_index);
        CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    }
    for (prefix = first_end; prefix < total; ++prefix) {
        qxp_guarded.state = qxp_before;
        CHECK(!QxpD3d9ProcessDrawStateBatch(
            opcode, 2, qxp_buffer, prefix, &qxp_guarded.state,
            0x9000UL + opcode, &consumed, &error_offset, &status));
        CHECK(consumed == first_end);
        CHECK(error_offset == 0x9000UL + opcode);
        CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_second));
        CHECK(status.record_index == 1);
        CHECK(status.byte_offset == first_end);
        CHECK(status.stage == stage);
        CHECK(status.constant_kind == kind);
        CHECK(status.code ==
              (prefix - first_end < sizeof(QXP_D3D9_DRAW_DP2_CONSTANT) ?
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD :
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD));
        if (prefix - first_end >= sizeof(QXP_D3D9_DRAW_DP2_CONSTANT))
            CHECK(status.register_index == register_limit - 1);
    }
    qxp_guarded.state = qxp_before;
    CHECK(QxpD3d9ProcessDrawStateBatch(
        opcode, 2, qxp_buffer, total, &qxp_guarded.state,
        0x9000UL + opcode, &consumed, &error_offset, &status));
    CHECK(consumed == total);
    CHECK(!error_offset && qxp_status_zero(&status));
    CHECK(qxp_test_constant_value(
        &qxp_guarded.state, stage, kind, register_index, 0) ==
        0x2000UL + opcode);
    CHECK(qxp_test_constant_value(
        &qxp_guarded.state, stage, kind, register_limit - 1,
        element_dwords - 1) ==
        0x3000UL + opcode + element_dwords - 1);
    CHECK(qxp_test_constant_mask(
        &qxp_guarded.state, stage, kind, register_index / 32UL) &
        (1UL << (register_index & 31UL)));
    CHECK(qxp_test_constant_mask(
        &qxp_guarded.state, stage, kind,
        (register_limit - 1) / 32UL) &
        (1UL << ((register_limit - 1) & 31UL)));
    CHECK(qxp_test_other_constant_masks_zero(&qxp_guarded.state,
                                              stage, kind));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_expected));
    memset(qxp_buffer, 0xee, total);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_expected));
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_constant_batches(void)
{
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF,
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 7,
        QXP_D3D9_DRAW_VS_FLOAT_CONSTANT_COUNT, 4));
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI,
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_INTEGER, 3,
        QXP_D3D9_DRAW_VS_INTEGER_CONSTANT_COUNT, 4));
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB,
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        QXP_D3D9_DRAW_CONSTANT_BOOLEAN, 2,
        QXP_D3D9_DRAW_VS_BOOLEAN_CONSTANT_COUNT, 1));
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF,
        QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_FLOAT, 5,
        QXP_D3D9_DRAW_PS_FLOAT_CONSTANT_COUNT, 4));
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI,
        QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_INTEGER, 1,
        QXP_D3D9_DRAW_PS_INTEGER_CONSTANT_COUNT, 4));
    CHECK(qxp_test_constant_batch_one(
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB,
        QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        QXP_D3D9_DRAW_CONSTANT_BOOLEAN, 1,
        QXP_D3D9_DRAW_PS_BOOLEAN_CONSTANT_COUNT, 1));
    return 1;
}

static int qxp_test_batch_errors(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    DWORD error_offset;
    ULONG bytes;

    qxp_reset_guarded();
    qxp_before = qxp_guarded.state;
    consumed = 9;
    error_offset = 9;
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        0xff, 0, NULL, 0, &qxp_guarded.state, 88,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_OPCODE);
    CHECK(!consumed && error_offset == 88);
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 0,
        NULL, 0, &qxp_guarded.state, 88,
        &consumed, &error_offset, &status));
    CHECK(!consumed && !error_offset && qxp_status_zero(&status));
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 1,
        NULL, 0, &qxp_guarded.state, 89,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);
    CHECK(error_offset == 89);

    bytes = qxp_append_constant_record(0, 255, 2, 4, 1);
    CHECK(!QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 1,
        qxp_buffer, bytes, &qxp_guarded.state, 90,
        &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_RANGE);
    CHECK(!consumed && error_offset == 90);
    CHECK(status.register_index == 255);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_shared_float_publication(void)
{
    QXP_D3D9_DRAW_STATE_STATUS status;
    DWORD legacy_vs[96][4];
    DWORD legacy_ps[8][4];
    DWORD values[16];
    DWORD index;
    DWORD locks;
    ULONG first_end;
    ULONG total;
    ULONG prefix;
    ULONG consumed;
    DWORD error_offset;

    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    memset(legacy_vs, 0xcd, sizeof(legacy_vs));
    memset(legacy_ps, 0xef, sizeof(legacy_ps));
    for (index = 0; index < 16; ++index)
        values[index] = 0xa000UL + index;
    qxp_reset_guarded();

    CHECK(QxpD3d9SharedFloatRangeFitsLegacy(
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX, 95, 1, 96));
    CHECK(!QxpD3d9SharedFloatRangeFitsLegacy(
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX, 95, 2, 96));
    CHECK(QxpD3d9SharedFloatRangeFitsLegacy(
        QXP_D3D9_DRAW_SHADER_STAGE_PIXEL, 7, 1, 8));
    CHECK(!QxpD3d9SharedFloatRangeFitsLegacy(
        QXP_D3D9_DRAW_SHADER_STAGE_PIXEL, 8, 1, 8));
    CHECK(!QxpD3d9SharedFloatRangeFitsLegacy(0, 0, 1, 96));
    CHECK(!QxpD3d9SharedFloatRangeFitsLegacy(
        QXP_D3D9_DRAW_SHADER_STAGE_VERTEX, 0, 0, 96));

    CHECK(QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        94, 4, values, legacy_vs, 96,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(qxp_status_zero(&status));
    CHECK(qxp_guarded.state.vs_float[94][0] == values[0]);
    CHECK(qxp_guarded.state.vs_float[97][3] == values[15]);
    CHECK(legacy_vs[94][0] == values[0]);
    CHECK(legacy_vs[95][3] == values[7]);
    CHECK(legacy_vs[93][0] == 0xcdcdcdcdUL);
    CHECK(qxp_guarded.state.vs_float_set_mask[2] & (1UL << 30));
    CHECK(qxp_guarded.state.vs_float_set_mask[3] & (1UL << 1));

    CHECK(QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        7, 2, values + 4, legacy_ps, 8,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(qxp_guarded.state.ps_float[7][0] == values[4]);
    CHECK(qxp_guarded.state.ps_float[8][3] == values[11]);
    CHECK(legacy_ps[7][0] == values[4]);
    CHECK(legacy_ps[6][0] == 0xefefefefUL);

    CHECK(QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        255, 1, values, NULL, 0,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(qxp_guarded.state.vs_float[255][3] == values[3]);
    CHECK(qxp_guarded.state.vs_float_set_mask[7] & (1UL << 31));

    legacy_vs[0][0] = 0x12345678UL;
    legacy_vs[0][1] = 0x23456789UL;
    legacy_vs[0][2] = 0x3456789aUL;
    legacy_vs[0][3] = 0x456789abUL;
    CHECK(QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        0, 1, legacy_vs[0], legacy_vs, 96,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(qxp_guarded.state.vs_float[0][0] == 0x12345678UL);
    CHECK(legacy_vs[0][3] == 0x456789abUL);

    qxp_before = qxp_guarded.state;
    locks = qxp_dispatch.locks;
    CHECK(!QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_PIXEL,
        31, 2, values, legacy_ps, 8,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_CONSTANT_RANGE);
    CHECK(qxp_dispatch.locks == locks &&
          qxp_dispatch.unlocks == locks);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(!QxpD3d9PublishSharedFloatConstants(
        &qxp_guarded.state, QXP_D3D9_DRAW_SHADER_STAGE_VERTEX,
        0, 1, values, NULL, 96,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_NULL_ARGUMENT);
    CHECK(qxp_dispatch.locks == locks);
    CHECK(!qxp_dispatch.depth && !qxp_dispatch.failures);

    first_end = qxp_append_constant_record(0, 95, 1, 4, 0xb000UL);
    total = qxp_append_constant_record(
        first_end, 96, 1, 4, 0xb100UL);
    qxp_reset_guarded();
    memset(legacy_vs, 0, sizeof(legacy_vs));
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    CHECK(QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 2,
        qxp_buffer, total, &qxp_guarded.state,
        legacy_vs, 96, FALSE, qxp_test_record_float, &qxp_dispatch,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x660UL, &consumed, &error_offset, &status));
    CHECK(consumed == total && !error_offset && qxp_status_zero(&status));
    CHECK(legacy_vs[95][0] == 0xb000UL);
    CHECK(qxp_guarded.state.vs_float[96][3] == 0xb103UL);
    CHECK(!qxp_dispatch.record_calls);
    CHECK(qxp_dispatch.locks == 2 && qxp_dispatch.unlocks == 2);

    qxp_reset_guarded();
    memset(legacy_vs, 0, sizeof(legacy_vs));
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    CHECK(!QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 2,
        qxp_buffer, total, &qxp_guarded.state,
        legacy_vs, 96, TRUE, qxp_test_record_float, &qxp_dispatch,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x664UL, &consumed, &error_offset, &status));
    CHECK(consumed == first_end && error_offset == 0x664UL);
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK &&
          status.record_index == 1 && status.byte_offset == first_end &&
          status.register_index == 96);
    CHECK(!legacy_vs[95][0] &&
          !qxp_guarded.state.vs_float[95][0] &&
          !qxp_guarded.state.vs_float[96][0]);
    CHECK(qxp_dispatch.record_calls == 1 &&
          !qxp_dispatch.locks && !qxp_dispatch.unlocks);

    total = qxp_append_constant_record(0, 10, 2, 4, 0xc000UL);
    qxp_reset_guarded();
    memset(legacy_vs, 0, sizeof(legacy_vs));
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    qxp_dispatch.fail_record_call = 2;
    CHECK(!QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 1,
        qxp_buffer, total, &qxp_guarded.state,
        legacy_vs, 96, TRUE, qxp_test_record_float, &qxp_dispatch,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x668UL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x668UL);
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_RESOURCE &&
          status.register_index == 11);
    CHECK(!qxp_guarded.state.vs_float[10][0] &&
          !qxp_guarded.state.vs_float[11][3]);
    CHECK(!legacy_vs[10][0] && !legacy_vs[11][3]);
    CHECK(qxp_dispatch.record_calls == 2 &&
          !qxp_dispatch.locks && !qxp_dispatch.unlocks);

    first_end = qxp_append_constant_record(0, 7, 1, 4, 0xc100UL);
    qxp_reset_guarded();
    memset(legacy_ps, 0, sizeof(legacy_ps));
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    CHECK(!QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF, 1,
        qxp_buffer, first_end, &qxp_guarded.state,
        legacy_ps, 8, TRUE, NULL, NULL,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x66aUL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x66aUL &&
          status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK &&
          status.register_index == 7);
    CHECK(!legacy_ps[7][0] &&
          !qxp_guarded.state.ps_float[7][0] &&
          !qxp_dispatch.locks && !qxp_dispatch.unlocks);

    first_end = qxp_append_constant_record(0, 31, 1, 4, 0xd000UL);
    for (prefix = 0; prefix < first_end; ++prefix) {
        qxp_reset_guarded();
        qxp_before = qxp_guarded.state;
        memset(legacy_ps, 0, sizeof(legacy_ps));
        memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
        CHECK(!QxpD3d9DispatchSharedFloatBatch(
            QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF, 1,
            qxp_buffer, prefix, &qxp_guarded.state,
            legacy_ps, 8, FALSE, NULL, NULL,
            qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
            0x66cUL, &consumed, &error_offset, &status));
        CHECK(!consumed && error_offset == 0x66cUL);
        CHECK(status.code ==
              (prefix < sizeof(QXP_D3D9_DRAW_DP2_CONSTANT) ?
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD :
               QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_PAYLOAD));
        CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
        CHECK(!qxp_dispatch.locks && !qxp_dispatch.unlocks);
    }
    qxp_reset_guarded();
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    CHECK(!QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTF, 1,
        qxp_buffer, first_end, &qxp_guarded.state,
        legacy_ps, 8, TRUE, NULL, NULL,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x670UL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK &&
          status.register_index == 31 && !qxp_dispatch.locks);
    CHECK(!QxpD3d9DispatchSharedFloatBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI, 0,
        NULL, 0, &qxp_guarded.state, legacy_vs, 96, FALSE,
        NULL, NULL, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x674UL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_OPCODE &&
          error_offset == 0x674UL);
    CHECK(!qxp_dispatch.depth && !qxp_dispatch.failures);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_shared_resource_dispatch(void)
{
    QXP_D3D9_LEGACY_RESOURCE_VIEW legacy;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE sources[2];
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE_UM source_um[2];
    QXP_D3D9_DRAW_DP2_INDICES indices[2];
    QXP_D3D9_DRAW_STATE_STATUS status;
    DWORD stream_handles[QXP_D3D9_DRAW_STREAM_COUNT];
    DWORD stream_strides[QXP_D3D9_DRAW_STREAM_COUNT];
    DWORD before_handles[QXP_D3D9_DRAW_STREAM_COUNT];
    DWORD before_strides[QXP_D3D9_DRAW_STREAM_COUNT];
    DWORD index_handle;
    DWORD index_stride;
    BOOL stream0_user_memory;
    ULONG consumed;
    ULONG prefix;
    DWORD error_offset;
    DWORD locks;
    ULONGLONG generation;

    legacy.stream_handles = stream_handles;
    legacy.stream_strides = stream_strides;
    legacy.stream_count = QXP_D3D9_DRAW_STREAM_COUNT;
    legacy.stream0_user_memory = &stream0_user_memory;
    legacy.index_handle = &index_handle;
    legacy.index_stride = &index_stride;
    memset(stream_handles, 0, sizeof(stream_handles));
    memset(stream_strides, 0, sizeof(stream_strides));
    index_handle = 0;
    index_stride = 0;
    stream0_user_memory = TRUE;
    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    qxp_reset_guarded();
    qxp_guarded.state.streams[0].divider = 7;
    qxp_guarded.state.streams[0].handle = 9;
    qxp_guarded.state.streams[0].offset = 5;
    qxp_guarded.state.streams[0].stride = 8;
    qxp_guarded.state.streams[0].source2 = TRUE;
    sources[0].stream = 0;
    sources[0].handle = 100;
    sources[0].stride = 20;
    sources[1].stream = 15;
    sources[1].handle = 115;
    sources[1].stride = 35;
    generation = qxp_guarded.state.stream_source_generation;
    CHECK(QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 2,
        sources, sizeof(sources), &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x710UL, &consumed, &error_offset, &status));
    CHECK(consumed == sizeof(sources) && !error_offset &&
          qxp_status_zero(&status));
    CHECK(qxp_guarded.state.streams[0].handle == 100 &&
          !qxp_guarded.state.streams[0].offset &&
          qxp_guarded.state.streams[0].stride == 20 &&
          qxp_guarded.state.streams[0].divider == 7 &&
          !qxp_guarded.state.streams[0].user_memory &&
          !qxp_guarded.state.streams[0].source2);
    CHECK(qxp_guarded.state.streams[15].handle == 115 &&
          stream_handles[0] == 100 && stream_strides[0] == 20 &&
          stream_handles[15] == 115 && stream_strides[15] == 35 &&
          !stream0_user_memory);
    CHECK(qxp_guarded.state.stream_source_generation == generation + 1);
    CHECK(qxp_dispatch.locks == 2 && qxp_dispatch.unlocks == 2);

    source_um[0].stream = 0;
    source_um[0].stride = 24;
    source_um[1].stream = 0;
    source_um[1].stride = 28;
    CHECK(QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_UM, 2,
        source_um, sizeof(source_um), &qxp_guarded.state, &legacy, TRUE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x714UL, &consumed, &error_offset, &status));
    CHECK(!qxp_guarded.state.streams[0].handle &&
          qxp_guarded.state.streams[0].stride == 28 &&
          qxp_guarded.state.streams[0].user_memory &&
          !qxp_guarded.state.streams[0].source2 &&
          !stream_handles[0] && stream_strides[0] == 28 &&
          stream0_user_memory);

    indices[0].handle = 701;
    indices[0].stride = 2;
    indices[1].handle = 702;
    indices[1].stride = 4;
    CHECK(QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 2,
        indices, sizeof(indices), &qxp_guarded.state, &legacy, TRUE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x718UL, &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.indices.handle == 702 &&
          qxp_guarded.state.indices.stride == 4 &&
          index_handle == 702 && index_stride == 4);
    indices[0].handle = 0;
    indices[0].stride = 99;
    CHECK(QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 1,
        indices, sizeof(indices[0]), &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x71cUL, &consumed, &error_offset, &status));
    CHECK(!qxp_guarded.state.indices.handle &&
          !qxp_guarded.state.indices.stride &&
          !index_handle && !index_stride);

    qxp_before = qxp_guarded.state;
    memcpy(before_handles, stream_handles, sizeof(before_handles));
    memcpy(before_strides, stream_strides, sizeof(before_strides));
    sources[0].stream = 1;
    sources[0].handle = 201;
    sources[0].stride = 16;
    sources[1].stream = 16;
    sources[1].handle = 202;
    sources[1].stride = 20;
    locks = qxp_dispatch.locks;
    CHECK(!QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 2,
        sources, sizeof(sources), &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x720UL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x720UL &&
          status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM &&
          status.record_index == 1 &&
          status.byte_offset == sizeof(sources[0]));
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));
    CHECK(!memcmp(stream_handles, before_handles, sizeof(before_handles)) &&
          !memcmp(stream_strides, before_strides, sizeof(before_strides)));
    CHECK(qxp_dispatch.locks == locks + 1 &&
          qxp_dispatch.unlocks == locks + 1);

    for (prefix = 0; prefix < sizeof(indices); ++prefix) {
        locks = qxp_dispatch.locks;
        CHECK(!QxpD3d9DispatchSharedResourceBatch(
            QXP_D3D9_DRAW_DP2OP_SET_INDICES, 2,
            indices, prefix, &qxp_guarded.state, &legacy, FALSE,
            qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
            0x724UL, &consumed, &error_offset, &status));
        CHECK(!consumed && error_offset == 0x724UL &&
              status.code == QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD &&
              status.record_index == prefix / sizeof(indices[0]) &&
              status.byte_offset ==
                  (prefix / sizeof(indices[0])) * sizeof(indices[0]));
        CHECK(qxp_dispatch.locks == locks &&
              qxp_dispatch.unlocks == locks);
    }

    qxp_reset_guarded();
    memset(stream_handles, 0, sizeof(stream_handles));
    memset(stream_strides, 0, sizeof(stream_strides));
    stream0_user_memory = FALSE;
    sources[0].stream = 3;
    sources[0].handle = 66;
    sources[0].stride = 12;
    qxp_dispatch.race_state = &qxp_guarded.state;
    qxp_dispatch.race_handle = 66;
    qxp_dispatch.race_at_lock = qxp_dispatch.locks + 2;
    CHECK(!QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 1,
        sources, sizeof(sources[0]), &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x728UL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x728UL &&
          status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_CHANGED);
    CHECK(!qxp_guarded.state.streams[3].handle &&
          !stream_handles[3]);

    qxp_guarded.state.stream_source_generation = ~(ULONGLONG)0;
    indices[0].handle = 801;
    indices[0].stride = 2;
    CHECK(!QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_INDICES, 1,
        indices, sizeof(indices[0]), &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x72cUL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x72cUL &&
          status.code == QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED &&
          !qxp_guarded.state.indices.handle && !index_handle);

    qxp_reset_guarded();
    memset(stream_handles, 0, sizeof(stream_handles));
    stream_handles[0] = 4;
    stream_handles[1] = 333;
    stream_handles[2] = 44;
    CHECK(QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE, 1,
        stream_handles, sizeof(QXP_D3D9_DRAW_DP2_STREAM_SOURCE),
        &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x730UL, &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[4].handle == 333 &&
          qxp_guarded.state.streams[4].stride == 44 &&
          stream_handles[4] == 333 && stream_strides[4] == 44);
    locks = qxp_dispatch.locks;
    CHECK(!QxpD3d9DispatchSharedResourceBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 0,
        NULL, 0, &qxp_guarded.state, &legacy, FALSE,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x734UL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_OPCODE &&
          error_offset == 0x734UL && qxp_dispatch.locks == locks);
    CHECK(!qxp_dispatch.depth && !qxp_dispatch.failures);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_production_dispatch(void)
{
    static const BYTE constant_opcodes[4] = {
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI,
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB,
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTI,
        QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB
    };
    QXP_D3D9_SHADER_ALLOCATOR allocator;
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE2 source2;
    QXP_D3D9_DRAW_DP2_STREAM_FREQUENCY frequency;
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    ULONG bytes;
    ULONG first_end;
    DWORD error_offset;
    DWORD index;
    DWORD before_allocations;

    memset(&qxp_dispatch, 0, sizeof(qxp_dispatch));
    allocator.allocate = qxp_test_allocate;
    allocator.release = qxp_test_release;
    allocator.opaque = &qxp_dispatch;
    qxp_reset_guarded();
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTF, 0,
        NULL, 0, &qxp_guarded.state, FALSE, &allocator,
        qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x1fcUL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_OPCODE);
    CHECK(!consumed && error_offset == 0x1fcUL &&
          !qxp_dispatch.allocations && !qxp_dispatch.locks);

    for (index = 0; index < 4; ++index) {
        bytes = qxp_append_constant_record(
            0, index, 1,
            (constant_opcodes[index] ==
                 QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTB ||
             constant_opcodes[index] ==
                 QXP_D3D9_DRAW_DP2OP_SET_PIXEL_CONSTB) ? 1 : 4,
            0x7000UL + index * 0x10UL);
        CHECK(QxpD3d9DispatchDrawStateBatch(
            constant_opcodes[index], 1, qxp_buffer, bytes,
            &qxp_guarded.state, FALSE, &allocator,
            qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
            0x200UL + index, &consumed, &error_offset, &status));
        CHECK(consumed == bytes && !error_offset && qxp_status_zero(&status));
    }
    CHECK(qxp_guarded.state.vs_integer[0][0] == 0x7000UL);
    CHECK(qxp_guarded.state.vs_boolean[1] == 0x7010UL);
    CHECK(qxp_guarded.state.ps_integer[2][3] == 0x7023UL);
    CHECK(qxp_guarded.state.ps_boolean[3] == 0x7030UL);
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    source2.stream = 0;
    source2.handle = 55;
    source2.offset = 0;
    source2.stride = 24;
    CHECK(QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 1,
        &source2, sizeof(source2), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x300UL, &consumed, &error_offset, &status));
    CHECK(qxp_guarded.state.streams[0].source2);
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    QxpD3d9DrawStateUnbindSurface(&qxp_guarded.state, 55);
    CHECK(!qxp_guarded.state.streams[0].source2);
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    frequency.stream = 1;
    frequency.divider = 7;
    CHECK(QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 1,
        &frequency, sizeof(frequency), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x304UL, &consumed, &error_offset, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    frequency.stream = 0;
    CHECK(QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 1,
        &frequency, sizeof(frequency), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x308UL, &consumed, &error_offset, &status));
    CHECK(QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));
    frequency.divider = 1;
    CHECK(QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE_FREQ, 1,
        &frequency, sizeof(frequency), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x30cUL, &consumed, &error_offset, &status));
    CHECK(!QxpD3d9DrawStateRequiresUnsupportedExecution(
        &qxp_guarded.state));

    first_end = qxp_append_constant_record(0, 4, 1, 4, 0x8800UL);
    qxp_append_dword(first_end, 5);
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI, 2,
        qxp_buffer, first_end + sizeof(DWORD), &qxp_guarded.state,
        FALSE, &allocator, qxp_test_lock, qxp_test_unlock,
        &qxp_dispatch, 0x440UL, &consumed, &error_offset, &status));
    CHECK(consumed == first_end && error_offset == 0x440UL);
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_TRUNCATED_RECORD &&
          status.record_index == 1 && status.byte_offset == first_end);
    CHECK(qxp_guarded.state.vs_integer[4][0] == 0x8800UL);

    qxp_before = qxp_guarded.state;
    source2.stream = QXP_D3D9_DRAW_STREAM_COUNT;
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 1,
        &source2, sizeof(source2), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x448UL, &consumed, &error_offset, &status));
    CHECK(!consumed && error_offset == 0x448UL);
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STREAM);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    before_allocations = qxp_dispatch.allocations;
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI, 1,
        qxp_buffer, first_end, &qxp_guarded.state, TRUE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x450UL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_BLOCK);
    CHECK(!consumed && error_offset == 0x450UL);
    CHECK(qxp_dispatch.allocations == before_allocations);
    CHECK(qxp_state_equal(&qxp_guarded.state, &qxp_before));

    CHECK(!qxp_guarded.state.streams[2].handle);
    source2.stream = 2;
    source2.handle = 66;
    source2.offset = 0;
    source2.stride = 20;
    qxp_dispatch.race_state = &qxp_guarded.state;
    qxp_dispatch.race_handle = 66;
    qxp_dispatch.race_at_lock = qxp_dispatch.locks + 2;
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 1,
        &source2, sizeof(source2), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x452UL, &consumed, &error_offset, &status));
    CHECK(status.code == QXP_D3D9_DRAW_STATE_ERROR_STATE_CHANGED);
    CHECK(!consumed && error_offset == 0x452UL);
    CHECK(!qxp_guarded.state.streams[2].handle &&
          !qxp_guarded.state.streams[2].source2);

    qxp_guarded.state.stream_source_generation = ~(ULONGLONG)0;
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 1,
        &source2, sizeof(source2), &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x453UL, &consumed, &error_offset, &status));
    CHECK(status.code ==
          QXP_D3D9_DRAW_STATE_ERROR_GENERATION_EXHAUSTED);
    CHECK(!consumed && error_offset == 0x453UL);

    qxp_dispatch.fail_allocation = TRUE;
    CHECK(!QxpD3d9DispatchDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_VERTEX_CONSTI, 1,
        qxp_buffer, first_end, &qxp_guarded.state, FALSE,
        &allocator, qxp_test_lock, qxp_test_unlock, &qxp_dispatch,
        0x454UL, &consumed, &error_offset, &status));
    CHECK(QxpD3d9DrawStateStatusIsResourceFailure(&status));
    CHECK(!consumed && error_offset == 0x454UL);
    qxp_dispatch.fail_allocation = FALSE;
    CHECK(!QxpD3d9DrawStateStatusIsResourceFailure(NULL));
    CHECK(qxp_dispatch.allocations == qxp_dispatch.releases + 1);
    CHECK(qxp_dispatch.locks == qxp_dispatch.unlocks);
    CHECK(!qxp_dispatch.depth && !qxp_dispatch.failures);
    CHECK(qxp_canaries_ok());
    return 1;
}

static int qxp_test_determinism(void)
{
    QXP_D3D9_DRAW_DP2_STREAM_SOURCE2 source[2];
    QXP_D3D9_DRAW_STATE_STATUS status;
    ULONG consumed;
    DWORD error_offset;

    source[0].stream = 1;
    source[0].handle = 11;
    source[0].offset = 12;
    source[0].stride = 13;
    source[1].stream = 2;
    source[1].handle = 21;
    source[1].offset = 22;
    source[1].stride = 23;
    QxpD3d9DrawStateInitialize(&qxp_before);
    QxpD3d9DrawStateInitialize(&qxp_second);
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 2,
        source, sizeof(source), &qxp_before, 1,
        &consumed, &error_offset, &status));
    CHECK(QxpD3d9ProcessDrawStateBatch(
        QXP_D3D9_DRAW_DP2OP_SET_STREAM_SOURCE2, 2,
        source, sizeof(source), &qxp_second, 1,
        &consumed, &error_offset, &status));
    CHECK(qxp_state_equal(&qxp_before, &qxp_second));
    return 1;
}

int main(void)
{
    if (!qxp_test_initialize() ||
        !qxp_test_bindings() ||
        !qxp_test_pretransformed_vertex_binding_is_dormant() ||
        !qxp_test_legacy_fvf_resolution() ||
        !qxp_test_hwtl_fvf_publication_sequence() ||
        !qxp_test_streams() ||
        !qxp_test_constants() ||
        !qxp_test_user_memory_draw_state() ||
        !qxp_test_fixed_batches() ||
        !qxp_test_fixed_alias_input() ||
        !qxp_test_constant_batches() ||
        !qxp_test_batch_errors() ||
        !qxp_test_shared_float_publication() ||
        !qxp_test_shared_resource_dispatch() ||
        !qxp_test_production_dispatch() ||
        !qxp_test_determinism()) return 1;
    printf("qxp d3d9 draw state tests passed\n");
    return 0;
}
