/*
 * Pre-transformed vertex processing expressed as compatibility GLSL.
 *
 * qxp_positiont_vshader_glsl.h carries the contract this file implements;
 * the commentary here covers only the how.  The emit is one bounded append
 * walk.  The draw path calls it twice - once with a NULL sink to count,
 * once to write - and because both calls run the same walk the counted
 * length and the written length are the same bytes by construction.
 */

#include "qxp_positiont_vshader_glsl.h"

/* Usage values mirror QXP_VERTEX_DECLUSAGE9_* in qxp_vdecl9.h.  They are
 * repeated here so this translation unit depends only on its contract
 * header; the metadata consumer compares against those same values. */
#define QXP_POSITIONT_USAGE_POSITIONT 9UL
#define QXP_POSITIONT_USAGE_COLOR     10UL
#define QXP_POSITIONT_USAGE_TEXCOORD  5UL

/* The display driver links no C runtime; zeroing stays local, as it does in
 * every other qxpdisp translation unit.  The pointer is volatile so the
 * compiler cannot recognise the loop and emit a call to memset, which would
 * not resolve at link time. */
static void qxp_positiont_zero(void *destination, ULONG count)
{
    volatile BYTE *bytes = (volatile BYTE *)destination;

    while (count--) *bytes++ = 0;
}

/* Bounded appender.  A NULL text pointer counts bytes without storing them,
 * so the count-only query and the real emit share one code path and can
 * never disagree.  A stage is handed out only after the whole walk has
 * succeeded, so a capacity failure can never leave a half-written stage. */
typedef struct qxp_positiont_output {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_POSITIONT_OUTPUT;

static void qxp_positiont_append(QXP_POSITIONT_OUTPUT *out, const char *text)
{
    DWORD index;

    if (!out->valid || !text) return;
    for (index = 0; text[index]; ++index) {
        if (out->length + 1UL >= out->capacity) {
            out->valid = FALSE;
            return;
        }
        if (out->text) out->text[out->length] = text[index];
        ++out->length;
    }
    if (out->text) out->text[out->length] = '\0';
}

static void qxp_positiont_append_uint(QXP_POSITIONT_OUTPUT *out, DWORD value)
{
    char digits[12];
    DWORD count = 0;

    if (!value) {
        qxp_positiont_append(out, "0");
        return;
    }
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }
    while (count) {
        char single[2];

        single[0] = digits[--count];
        single[1] = '\0';
        qxp_positiont_append(out, single);
    }
}

/* Every refusal reports through the error sink and returns FALSE.  The
 * caller's outputs were cleared on entry, so a failure can never leak a
 * partial stage or a stale byte count. */
static BOOL qxp_positiont_fail(QXP_POSITIONT_VERTEX_ERROR *error,
                               DWORD code, DWORD index, DWORD value)
{
    error->code = code;
    error->index = index;
    error->value = value;
    return FALSE;
}

/* Fail-closed validation.  A declaration element the stage cannot express
 * is a refused draw, never a silently dropped attribute, so anything
 * outside the contracted masks stops the emit before any text is written. */
static BOOL qxp_positiont_validate(const QXP_POSITIONT_VERTEX_STATE *state,
                                   QXP_POSITIONT_VERTEX_ERROR *error)
{
    DWORD offending;
    DWORD index;

    offending = state->color_mask & ~3UL;
    if (offending) {
        for (index = 0; !(offending & 1UL); ++index) offending >>= 1;
        return qxp_positiont_fail(error, QXP_POSITIONT_VERTEX_ERROR_COLOR,
                                  index, state->color_mask);
    }
    offending = state->texcoord_mask & ~0xffUL;
    if (offending) {
        for (index = 0; !(offending & 1UL); ++index) offending >>= 1;
        return qxp_positiont_fail(error, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD,
                                  index, state->texcoord_mask);
    }
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        DWORD components = (DWORD)state->texcoord_components[index];

        if (state->texcoord_mask & (1UL << index)) {
            /* A declared coordinate set carries one to four components. */
            if (components < 1UL || components > 4UL)
                return qxp_positiont_fail(
                    error, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD,
                    index, components);
        } else if (components) {
            /* A component count without its declaration bit is a caller
             * bookkeeping bug; accepting it would hide that bug. */
            return qxp_positiont_fail(error,
                                      QXP_POSITIONT_VERTEX_ERROR_TEXCOORD,
                                      index, components);
        }
    }
    return TRUE;
}

/* Reports the stage through the same metadata shape a compiled vs_2_0 stage
 * produces, so the draw path treats synthesized and compiled stages alike.
 * declaration_count mirrors the attribute count because the execute path's
 * validator requires it to match the input mask; the remaining accounting
 * fields stay zero - they describe decoded bytecode, and this stage has
 * none.  The struct is filled in the caller's storage directly - it is far
 * too large to stage on a display-driver stack. */
static void qxp_positiont_metadata(const QXP_POSITIONT_VERTEX_STATE *state,
                                   QXP_VERTEX_SHADER_GLSL_METADATA *metadata)
{
    DWORD index;
    DWORD declarations = 1UL;

    qxp_positiont_zero(metadata, (ULONG)sizeof(*metadata));
    metadata->input_mask = 1UL | (state->color_mask << 1) |
                           (state->texcoord_mask << 3);
    metadata->input_usage[0] = QXP_POSITIONT_USAGE_POSITIONT;
    metadata->input_usage_index[0] = 0;
    metadata->input_component_mask[0] = 15;
    for (index = 0; index < QXP_POSITIONT_COLOR_COUNT; ++index) {
        if (!(state->color_mask & (1UL << index))) continue;
        metadata->input_usage[1UL + index] = QXP_POSITIONT_USAGE_COLOR;
        metadata->input_usage_index[1UL + index] = index;
        metadata->input_component_mask[1UL + index] = 15;
        ++declarations;
    }
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        if (!(state->texcoord_mask & (1UL << index))) continue;
        metadata->input_usage[3UL + index] = QXP_POSITIONT_USAGE_TEXCOORD;
        metadata->input_usage_index[3UL + index] = index;
        metadata->input_component_mask[3UL + index] = 15;
        ++declarations;
    }
    metadata->declaration_count = declarations;
    /* Both colour varyings are always written - white diffuse, black
     * specular - so the stage pair always links with a defined value. */
    metadata->attribute_output_mask = 3;
    metadata->attribute_output_components[0] = 15;
    metadata->attribute_output_components[1] = 15;
    metadata->texcoord_output_mask = state->texcoord_mask;
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        if (!(state->texcoord_mask & (1UL << index))) continue;
        /* The attribute is padded to vec4, so the varying is written in
         * full even when the declaration supplies fewer components. */
        metadata->texcoord_output_components[index] = 15;
    }
    metadata->writes_position = TRUE;
    metadata->position_component_mask = 15;
    /* gl_FogFragCoord is derived from position unconditionally, exactly
     * like the compiled path's non-fog branch, which also reports FALSE. */
    metadata->writes_fog = FALSE;
}

/*
 * The stage itself.  The clip epilogue is copied verbatim from the compiled
 * vs_2_0 path (qxp_vs_emit_commit in qxp_vshader20_glsl.c) so orientation,
 * the half-pixel offset and the D3D [0,w] to GL [-w,w] depth convention
 * stay identical between synthesized and compiled stages; only the position
 * comes from the pre-transformed attribute instead of a shader output
 * register.  qxpVsPositiont carries the viewport scale and bias the
 * miniport programs per draw, and the reciprocal of the rhw is what turns
 * screen-space pixels back into clip-space units.
 */
static void qxp_positiont_emit_source(
    QXP_POSITIONT_OUTPUT *out, const QXP_POSITIONT_VERTEX_STATE *state)
{
    DWORD index;

    qxp_positiont_append(out, "#version 120\n");
    qxp_positiont_append(out, "uniform vec4 qxpVsClipControl;\n");
    qxp_positiont_append(out, "uniform vec4 qxpVsPositiont;\n");
    qxp_positiont_append(out, "attribute vec4 qxpVsInput0;\n");
    for (index = 0; index < QXP_POSITIONT_COLOR_COUNT; ++index) {
        if (!(state->color_mask & (1UL << index))) continue;
        qxp_positiont_append(out, "attribute vec4 qxpVsInput");
        qxp_positiont_append_uint(out, 1UL + index);
        qxp_positiont_append(out, ";\n");
    }
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        if (!(state->texcoord_mask & (1UL << index))) continue;
        qxp_positiont_append(out, "attribute vec4 qxpVsInput");
        qxp_positiont_append_uint(out, 3UL + index);
        qxp_positiont_append(out, ";\n");
    }
    qxp_positiont_append(out, "varying vec4 qxpPsInV0;\n");
    qxp_positiont_append(out, "varying vec4 qxpPsInV1;\n");
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        if (!(state->texcoord_mask & (1UL << index))) continue;
        qxp_positiont_append(out, "varying vec4 qxpPsInT");
        qxp_positiont_append_uint(out, index);
        qxp_positiont_append(out, ";\n");
    }

    qxp_positiont_append(out, "void main(void)\n");
    qxp_positiont_append(out, "{\n");
    qxp_positiont_append(out, "  vec4 qxpP;\n");
    qxp_positiont_append(out, "  float qxpW;\n");
    qxp_positiont_append(out, "  qxpW = qxpVsInput0.w != 0.0 ?"
                              " 1.0 / qxpVsInput0.w : 1.0;\n");
    qxp_positiont_append(out, "  qxpP.xy = ((qxpVsInput0.xy -"
                              " qxpVsPositiont.zw) * qxpVsPositiont.xy +"
                              " vec2(-1.0, 1.0)) * qxpW;\n");
    qxp_positiont_append(out, "  qxpP.z = qxpVsInput0.z * qxpW;\n");
    qxp_positiont_append(out, "  qxpP.w = qxpW;\n");
    qxp_positiont_append(out, "  gl_ClipVertex = qxpP;\n");
    qxp_positiont_append(out, "  gl_Position.x = qxpP.x +"
                              " qxpP.w*qxpVsClipControl.z;\n");
    qxp_positiont_append(out, "  gl_Position.y = qxpP.y*qxpVsClipControl.x +"
                              " qxpP.w*qxpVsClipControl.w;\n");
    qxp_positiont_append(out, "  gl_Position.z = 2.0*qxpP.z - qxpP.w;\n");
    qxp_positiont_append(out, "  gl_Position.w = qxpP.w;\n");
    qxp_positiont_append(out, "  gl_FogFragCoord = qxpP.z / qxpP.w;\n");

    /* Both varyings are always written so the stage pair always links with
     * a defined value: white diffuse makes a modulate a no-op, black
     * specular makes an add a no-op.  This mirrors the compiled path's
     * defaults for unwritten colour outputs. */
    for (index = 0; index < QXP_POSITIONT_COLOR_COUNT; ++index) {
        qxp_positiont_append(out, "  qxpPsInV");
        qxp_positiont_append_uint(out, index);
        if (state->color_mask & (1UL << index)) {
            qxp_positiont_append(out, " = qxpVsInput");
            qxp_positiont_append_uint(out, 1UL + index);
            qxp_positiont_append(out, ";\n");
        } else if (index) {
            qxp_positiont_append(out, " = vec4(0.0, 0.0, 0.0, 0.0);\n");
        } else {
            qxp_positiont_append(out, " = vec4(1.0, 1.0, 1.0, 1.0);\n");
        }
    }
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        if (!(state->texcoord_mask & (1UL << index))) continue;
        qxp_positiont_append(out, "  qxpPsInT");
        qxp_positiont_append_uint(out, index);
        qxp_positiont_append(out, " = qxpVsInput");
        qxp_positiont_append_uint(out, 3UL + index);
        qxp_positiont_append(out, ";\n");
    }
    qxp_positiont_append(out, "}\n");
}

BOOL QxpEmitPositiontVertexGlsl(
    const QXP_POSITIONT_VERTEX_STATE *state,
    char *output, DWORD output_bytes, DWORD *source_bytes,
    QXP_VERTEX_SHADER_GLSL_METADATA *metadata,
    QXP_POSITIONT_VERTEX_ERROR *error)
{
    QXP_POSITIONT_OUTPUT out;

    if (error) {
        error->code = QXP_POSITIONT_VERTEX_OK;
        error->index = 0;
        error->value = 0;
    }
    if (source_bytes) *source_bytes = 0;
    if (metadata) qxp_positiont_zero(metadata, (ULONG)sizeof(*metadata));
    if (output && output_bytes) output[0] = '\0';

    if (!state || !source_bytes || !metadata || !error ||
        (!output) != (!output_bytes)) {
        if (error) error->code = QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT;
        return FALSE;
    }
    if (!qxp_positiont_validate(state, error)) return FALSE;

    qxp_positiont_metadata(state, metadata);

    /* Capacity is capped at QXP_POSITIONT_MAX_SOURCE in both modes, which
     * is what keeps a synthesized stage inside the scratch the draw path
     * reserves no matter how generous the caller's buffer is. */
    out.text = output;
    out.capacity = output ? output_bytes : QXP_POSITIONT_MAX_SOURCE;
    if (out.capacity > QXP_POSITIONT_MAX_SOURCE)
        out.capacity = QXP_POSITIONT_MAX_SOURCE;
    out.length = 0;
    out.valid = TRUE;

    qxp_positiont_emit_source(&out, state);
    if (!out.valid) {
        /* The walk may have written into the caller's buffer before the
         * capacity ran out, so the fail-closed clearing is repeated here
         * rather than trusted to the entry clearing. */
        qxp_positiont_zero(metadata, (ULONG)sizeof(*metadata));
        if (output && output_bytes) output[0] = '\0';
        return qxp_positiont_fail(
            error, QXP_POSITIONT_VERTEX_ERROR_OUTPUT_CAPACITY,
            0, output_bytes);
    }

    *source_bytes = out.length;
    return TRUE;
}

DWORD QxpPositiontVertexSignature(const QXP_POSITIONT_VERTEX_STATE *state)
{
    DWORD hash = 2166136261UL;
    DWORD index;

    if (!state) return 0;
    /* FNV-1a over the declaration bytes, field by field, so two layouts
     * that differ anywhere produce different cache keys. */
    for (index = 0; index < 4UL; ++index) {
        hash ^= (state->color_mask >> (index * 8UL)) & 0xffUL;
        hash *= 16777619UL;
    }
    for (index = 0; index < 4UL; ++index) {
        hash ^= (state->texcoord_mask >> (index * 8UL)) & 0xffUL;
        hash *= 16777619UL;
    }
    for (index = 0; index < QXP_POSITIONT_TEXCOORD_COUNT; ++index) {
        hash ^= (DWORD)state->texcoord_components[index];
        hash *= 16777619UL;
    }
    /* Zero marks "no synthesized stage" in the cache key. */
    return hash ? hash : 1UL;
}
