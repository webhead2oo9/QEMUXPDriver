#include "qxp_ffpshader_glsl.h"

#include "qxp_shader_ir.h"

#define QXP_TEXTURE_OP_STANDALONE
#include "qxpmp/qxp_texture_op.h"
#undef QXP_TEXTURE_OP_STANDALONE

/* The display driver links no C runtime; zeroing stays local, as it does in
 * every other qxpdisp translation unit.  The pointer is volatile so the
 * compiler cannot recognise the loop and emit a call to memset, which would
 * not resolve at link time. */
static void qxp_ff_zero(void *destination, ULONG count)
{
    volatile BYTE *bytes = (volatile BYTE *)destination;

    while (count--) *bytes++ = 0;
}

/* Bounded appender.  A stage is emitted only after every operand has been
 * accepted, so a capacity failure can never leave a half-written chain. */
typedef struct qxp_ff_output {
    char *text;
    DWORD capacity;
    DWORD length;
    BOOL valid;
} QXP_FF_OUTPUT;

static void qxp_ff_append(QXP_FF_OUTPUT *out, const char *text)
{
    DWORD index;

    if (!out->valid || !text) return;
    for (index = 0; text[index]; ++index) {
        if (out->length + 1UL >= out->capacity) {
            out->valid = FALSE;
            return;
        }
        out->text[out->length++] = text[index];
    }
    out->text[out->length] = '\0';
}

static void qxp_ff_append_uint(QXP_FF_OUTPUT *out, DWORD value)
{
    char digits[12];
    DWORD count = 0;

    if (!value) {
        qxp_ff_append(out, "0");
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
        qxp_ff_append(out, single);
    }
}

typedef struct qxp_ff_emit {
    QXP_FF_OUTPUT output;
    const QXP_FF_PIXEL_STATE *state;
    QXP_PIXEL_SHADER_GLSL_METADATA *metadata;
    QXP_FF_PIXEL_ERROR *error;
    DWORD stage;
} QXP_FF_EMIT;

static BOOL qxp_ff_fail(QXP_FF_EMIT *emit, DWORD code, DWORD value)
{
    if (emit->error) {
        emit->error->code = code;
        emit->error->stage = emit->stage;
        emit->error->value = value;
    }
    return FALSE;
}

/* Records that a stage samples, and which coordinate set it consumes. */
static BOOL qxp_ff_use_texture(QXP_FF_EMIT *emit, DWORD stage)
{
    const QXP_FF_PIXEL_STAGE *entry = &emit->state->stages[stage];

    if (!entry->sampler_type)
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_TEXTURE, stage);
    if (entry->coordinate_index >= 8UL)
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_COORDINATE,
                           entry->coordinate_index);
    emit->metadata->sampler_mask |= 1UL << stage;
    emit->metadata->sampler_types[stage] = (BYTE)entry->sampler_type;
    emit->metadata->input_texture_mask |= 1UL << entry->coordinate_index;
    /* A cube or volume lookup consumes a third coordinate component. */
    emit->metadata->input_texture_components[entry->coordinate_index] |=
        (entry->sampler_type == QXP_SHADER_SAMPLER_2D) ? 3U : 7U;
    return TRUE;
}

/* Emits the .a of a named blend source. */
static BOOL qxp_ff_emit_blend_alpha(QXP_FF_EMIT *emit, DWORD source)
{
    switch (source) {
    case QXP_D3DTA_DIFFUSE:
        emit->metadata->input_color_mask |= 1UL;
        emit->metadata->input_color_components[0] |= 8U;
        qxp_ff_append(&emit->output, "qxpPsInV0.a");
        return TRUE;
    case QXP_D3DTA_CURRENT:
        qxp_ff_append(&emit->output, "qxpCur.a");
        return TRUE;
    case QXP_D3DTA_TEXTURE:
        if (!qxp_ff_use_texture(emit, emit->stage)) return FALSE;
        qxp_ff_append(&emit->output, "qxpTex");
        qxp_ff_append_uint(&emit->output, emit->stage);
        qxp_ff_append(&emit->output, ".a");
        return TRUE;
    case QXP_D3DTA_TFACTOR:
        qxp_ff_append(&emit->output, "qxpTFactor.a");
        return TRUE;
    default:
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_ARGUMENT, source);
    }
}

/* Emits one combiner argument.  D3D replicates alpha before complementing,
 * so the two modifiers are not interchangeable. */
static BOOL qxp_ff_emit_argument(QXP_FF_EMIT *emit, DWORD argument,
                                 BOOL alpha)
{
    DWORD select = argument & QXP_D3DTA_SELECTMASK;
    BOOL complement = (argument & QXP_D3DTA_COMPLEMENT) ? TRUE : FALSE;
    BOOL replicate = (argument & QXP_D3DTA_ALPHAREPLICATE) ? TRUE : FALSE;

    if (argument & ~(QXP_D3DTA_SELECTMASK | QXP_D3DTA_COMPLEMENT |
                     QXP_D3DTA_ALPHAREPLICATE))
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_ARGUMENT, argument);

    if (complement)
        qxp_ff_append(&emit->output, alpha ? "(1.0 - " : "(vec3(1.0) - ");

    switch (select) {
    case QXP_D3DTA_DIFFUSE:
        emit->metadata->input_color_mask |= 1UL;
        emit->metadata->input_color_components[0] |=
            (alpha || replicate) ? 8U : 7U;
        qxp_ff_append(&emit->output, "qxpPsInV0");
        break;
    case QXP_D3DTA_SPECULAR:
        emit->metadata->input_color_mask |= 2UL;
        emit->metadata->input_color_components[1] |=
            (alpha || replicate) ? 8U : 7U;
        qxp_ff_append(&emit->output, "qxpPsInV1");
        break;
    case QXP_D3DTA_CURRENT:
        qxp_ff_append(&emit->output, "qxpCur");
        break;
    case QXP_D3DTA_TEXTURE:
        if (!qxp_ff_use_texture(emit, emit->stage)) return FALSE;
        qxp_ff_append(&emit->output, "qxpTex");
        qxp_ff_append_uint(&emit->output, emit->stage);
        break;
    case QXP_D3DTA_TFACTOR:
        qxp_ff_append(&emit->output, "qxpTFactor");
        break;
    default:
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_ARGUMENT, argument);
    }

    if (alpha) qxp_ff_append(&emit->output, ".a");
    else if (replicate) qxp_ff_append(&emit->output, ".aaa");
    else qxp_ff_append(&emit->output, ".rgb");

    if (complement) qxp_ff_append(&emit->output, ")");
    return TRUE;
}

typedef struct qxp_ff_operands {
    DWORD argument1;
    DWORD argument2;
    DWORD argument0;
} QXP_FF_OPERANDS;

static BOOL qxp_ff_emit_binary(QXP_FF_EMIT *emit,
                               const QXP_FF_OPERANDS *operands,
                               const char *infix, BOOL alpha)
{
    if (!qxp_ff_emit_argument(emit, operands->argument1, alpha)) return FALSE;
    qxp_ff_append(&emit->output, infix);
    return qxp_ff_emit_argument(emit, operands->argument2, alpha);
}

/* mix(arg2, arg1, weight) is D3D's blend: arg1*weight + arg2*(1-weight). */
static BOOL qxp_ff_emit_blend(QXP_FF_EMIT *emit,
                              const QXP_FF_OPERANDS *operands,
                              DWORD source, BOOL alpha)
{
    qxp_ff_append(&emit->output, "mix(");
    if (!qxp_ff_emit_argument(emit, operands->argument2, alpha)) return FALSE;
    qxp_ff_append(&emit->output, ", ");
    if (!qxp_ff_emit_argument(emit, operands->argument1, alpha)) return FALSE;
    qxp_ff_append(&emit->output, ", ");
    if (!qxp_ff_emit_blend_alpha(emit, source)) return FALSE;
    qxp_ff_append(&emit->output, ")");
    return TRUE;
}

static BOOL qxp_ff_emit_operation(QXP_FF_EMIT *emit, DWORD operation,
                                  const QXP_FF_OPERANDS *operands,
                                  BOOL alpha)
{
    switch (operation) {
    case QXP_D3DTOP_SELECTARG1:
        return qxp_ff_emit_argument(emit, operands->argument1, alpha);
    case QXP_D3DTOP_SELECTARG2:
        return qxp_ff_emit_argument(emit, operands->argument2, alpha);
    case QXP_D3DTOP_MODULATE:
        return qxp_ff_emit_binary(emit, operands, " * ", alpha);
    case QXP_D3DTOP_MODULATE2X:
        qxp_ff_append(&emit->output, "(");
        if (!qxp_ff_emit_binary(emit, operands, " * ", alpha)) return FALSE;
        qxp_ff_append(&emit->output, ") * 2.0");
        return TRUE;
    case QXP_D3DTOP_MODULATE4X:
        qxp_ff_append(&emit->output, "(");
        if (!qxp_ff_emit_binary(emit, operands, " * ", alpha)) return FALSE;
        qxp_ff_append(&emit->output, ") * 4.0");
        return TRUE;
    case QXP_D3DTOP_ADD:
        return qxp_ff_emit_binary(emit, operands, " + ", alpha);
    case QXP_D3DTOP_SUBTRACT:
        return qxp_ff_emit_binary(emit, operands, " - ", alpha);
    case QXP_D3DTOP_ADDSIGNED:
        if (!qxp_ff_emit_binary(emit, operands, " + ", alpha)) return FALSE;
        qxp_ff_append(&emit->output, alpha ? " - 0.5" : " - vec3(0.5)");
        return TRUE;
    case QXP_D3DTOP_ADDSIGNED2X:
        qxp_ff_append(&emit->output, "(");
        if (!qxp_ff_emit_binary(emit, operands, " + ", alpha)) return FALSE;
        qxp_ff_append(&emit->output,
                      alpha ? " - 0.5) * 2.0" : " - vec3(0.5)) * 2.0");
        return TRUE;
    case QXP_D3DTOP_ADDSMOOTH:
        if (!qxp_ff_emit_binary(emit, operands, " + ", alpha)) return FALSE;
        qxp_ff_append(&emit->output, " - ");
        return qxp_ff_emit_binary(emit, operands, " * ", alpha);
    case QXP_D3DTOP_BLENDDIFFUSEALPHA:
        return qxp_ff_emit_blend(emit, operands, QXP_D3DTA_DIFFUSE, alpha);
    case QXP_D3DTOP_BLENDTEXTUREALPHA:
        return qxp_ff_emit_blend(emit, operands, QXP_D3DTA_TEXTURE, alpha);
    case QXP_D3DTOP_BLENDFACTORALPHA:
        return qxp_ff_emit_blend(emit, operands, QXP_D3DTA_TFACTOR, alpha);
    case QXP_D3DTOP_BLENDCURRENTALPHA:
        return qxp_ff_emit_blend(emit, operands, QXP_D3DTA_CURRENT, alpha);
    case QXP_D3DTOP_BLENDTEXTUREALPHAPM:
        /* Premultiplied: the texture alpha attenuates arg2 alone. */
        if (!qxp_ff_emit_argument(emit, operands->argument1, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, " + ");
        if (!qxp_ff_emit_argument(emit, operands->argument2, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, " * (1.0 - ");
        if (!qxp_ff_emit_blend_alpha(emit, QXP_D3DTA_TEXTURE)) return FALSE;
        qxp_ff_append(&emit->output, ")");
        return TRUE;
    case QXP_D3DTOP_MULTIPLYADD:
        if (!qxp_ff_emit_argument(emit, operands->argument0, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, " + ");
        return qxp_ff_emit_binary(emit, operands, " * ", alpha);
    case QXP_D3DTOP_LERP:
        qxp_ff_append(&emit->output, "mix(");
        if (!qxp_ff_emit_argument(emit, operands->argument2, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, ", ");
        if (!qxp_ff_emit_argument(emit, operands->argument1, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, ", ");
        if (!qxp_ff_emit_argument(emit, operands->argument0, alpha))
            return FALSE;
        qxp_ff_append(&emit->output, ")");
        return TRUE;
    default:
        break;
    }

    if (alpha) return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_OPERATION,
                                  operation);

    /* Colour-only forms.  Each folds arg1's alpha into an arg1/arg2 blend. */
    switch (operation) {
    case QXP_D3DTOP_MODULATEALPHA_ADDCOLOR:
        if (!qxp_ff_emit_argument(emit, operands->argument1, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, " + ");
        if (!qxp_ff_emit_argument(emit, operands->argument1, TRUE))
            return FALSE;
        qxp_ff_append(&emit->output, " * ");
        return qxp_ff_emit_argument(emit, operands->argument2, FALSE);
    case QXP_D3DTOP_MODULATECOLOR_ADDALPHA:
        if (!qxp_ff_emit_binary(emit, operands, " * ", FALSE)) return FALSE;
        qxp_ff_append(&emit->output, " + vec3(");
        if (!qxp_ff_emit_argument(emit, operands->argument1, TRUE))
            return FALSE;
        qxp_ff_append(&emit->output, ")");
        return TRUE;
    case QXP_D3DTOP_MODULATEINVALPHA_ADDCOLOR:
        qxp_ff_append(&emit->output, "(1.0 - ");
        if (!qxp_ff_emit_argument(emit, operands->argument1, TRUE))
            return FALSE;
        qxp_ff_append(&emit->output, ") * ");
        if (!qxp_ff_emit_argument(emit, operands->argument2, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, " + ");
        return qxp_ff_emit_argument(emit, operands->argument1, FALSE);
    case QXP_D3DTOP_MODULATEINVCOLOR_ADDALPHA:
        qxp_ff_append(&emit->output, "(vec3(1.0) - ");
        if (!qxp_ff_emit_argument(emit, operands->argument1, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, ") * ");
        if (!qxp_ff_emit_argument(emit, operands->argument2, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, " + vec3(");
        if (!qxp_ff_emit_argument(emit, operands->argument1, TRUE))
            return FALSE;
        qxp_ff_append(&emit->output, ")");
        return TRUE;
    case QXP_D3DTOP_DOTPRODUCT3:
        /* Signed-scaled dot replicated across the stage result. */
        qxp_ff_append(&emit->output, "vec3(4.0 * dot(");
        if (!qxp_ff_emit_argument(emit, operands->argument1, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, " - vec3(0.5), ");
        if (!qxp_ff_emit_argument(emit, operands->argument2, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, " - vec3(0.5)))");
        return TRUE;
    default:
        return qxp_ff_fail(emit, QXP_FF_PIXEL_ERROR_OPERATION, operation);
    }
}

static void qxp_ff_emit_interface(QXP_FF_EMIT *emit, DWORD active_stages,
                                  BOOL uses_factor)
{
    DWORD index;

    qxp_ff_append(&emit->output, "#version 120\n");
    for (index = 0; index < 2; ++index) {
        if (!(emit->metadata->input_color_mask & (1UL << index))) continue;
        qxp_ff_append(&emit->output, "centroid varying vec4 qxpPsInV");
        qxp_ff_append_uint(&emit->output, index);
        qxp_ff_append(&emit->output, ";\n");
    }
    for (index = 0; index < 8; ++index) {
        if (!(emit->metadata->input_texture_mask & (1UL << index))) continue;
        qxp_ff_append(&emit->output, "varying vec4 qxpPsInT");
        qxp_ff_append_uint(&emit->output, index);
        qxp_ff_append(&emit->output, ";\n");
    }
    for (index = 0; index < QXP_FF_PIXEL_STAGE_COUNT; ++index) {
        if (!(emit->metadata->sampler_mask & (1UL << index))) continue;
        if (emit->metadata->sampler_types[index] == QXP_SHADER_SAMPLER_CUBE)
            qxp_ff_append(&emit->output, "uniform samplerCube qxpPsS");
        else if (emit->metadata->sampler_types[index] ==
                 QXP_SHADER_SAMPLER_VOLUME)
            qxp_ff_append(&emit->output, "uniform sampler3D qxpPsS");
        else
            qxp_ff_append(&emit->output, "uniform sampler2D qxpPsS");
        qxp_ff_append_uint(&emit->output, index);
        qxp_ff_append(&emit->output, ";\n");
    }
    if (uses_factor) {
        /* A uniform, not a baked constant: the factor is per-draw state that
         * titles animate, and folding it into the source would make every
         * colour its own program. */
        qxp_ff_append(&emit->output, "uniform vec4 qxpTFactor;\n");
    }
    qxp_ff_append(&emit->output, "void main()\n{\n");
    /* Stage 0 inherits the diffuse colour as `current`. */
    qxp_ff_append(&emit->output, "  vec4 qxpCur = qxpPsInV0;\n");
    for (index = 0; index < QXP_FF_PIXEL_STAGE_COUNT; ++index) {
        if (!(active_stages & (1UL << index))) continue;
        if (!(emit->metadata->sampler_mask & (1UL << index))) continue;
        qxp_ff_append(&emit->output, "  vec4 qxpTex");
        qxp_ff_append_uint(&emit->output, index);
        if (emit->metadata->sampler_types[index] == QXP_SHADER_SAMPLER_CUBE)
            qxp_ff_append(&emit->output, " = textureCube(qxpPsS");
        else if (emit->metadata->sampler_types[index] ==
                 QXP_SHADER_SAMPLER_VOLUME)
            qxp_ff_append(&emit->output, " = texture3D(qxpPsS");
        else
            qxp_ff_append(&emit->output, " = texture2D(qxpPsS");
        qxp_ff_append_uint(&emit->output, index);
        qxp_ff_append(&emit->output, ", qxpPsInT");
        qxp_ff_append_uint(&emit->output,
                           emit->state->stages[index].coordinate_index);
        if (emit->metadata->sampler_types[index] == QXP_SHADER_SAMPLER_CUBE ||
            emit->metadata->sampler_types[index] == QXP_SHADER_SAMPLER_VOLUME)
            qxp_ff_append(&emit->output, ".xyz);\n");
        else
            qxp_ff_append(&emit->output, ".xy);\n");
    }
}

/*
 * Two passes over the chain.  The first resolves the interface - which
 * varyings and samplers the combiner reads - because the declarations must
 * precede the body they describe.  The second emits the body itself.
 */
static BOOL qxp_ff_emit_body(QXP_FF_EMIT *emit, DWORD active_stages)
{
    DWORD index;

    for (index = 0; index < QXP_FF_PIXEL_STAGE_COUNT; ++index) {
        const QXP_FF_PIXEL_STAGE *entry = &emit->state->stages[index];
        QXP_FF_OPERANDS operands;
        BOOL dot3;

        if (!(active_stages & (1UL << index))) continue;
        emit->stage = index;
        dot3 = entry->color_op == QXP_D3DTOP_DOTPRODUCT3 ? TRUE : FALSE;

        operands.argument1 = entry->color_arg1;
        operands.argument2 = entry->color_arg2;
        operands.argument0 = entry->color_arg0;
        qxp_ff_append(&emit->output, "  qxpCur = clamp(vec4(");
        if (!qxp_ff_emit_operation(emit, entry->color_op, &operands, FALSE))
            return FALSE;
        qxp_ff_append(&emit->output, ", ");
        if (dot3) {
            /* DOTPRODUCT3 writes the dot to all four channels; the alpha
             * operation is not consulted. */
            qxp_ff_append(&emit->output, "4.0 * dot(");
            if (!qxp_ff_emit_argument(emit, entry->color_arg1, FALSE))
                return FALSE;
            qxp_ff_append(&emit->output, " - vec3(0.5), ");
            if (!qxp_ff_emit_argument(emit, entry->color_arg2, FALSE))
                return FALSE;
            qxp_ff_append(&emit->output, " - vec3(0.5))");
        } else if (entry->alpha_op == QXP_D3DTOP_DISABLE) {
            /* Colour-only stage: the running alpha passes through. */
            qxp_ff_append(&emit->output, "qxpCur.a");
        } else {
            operands.argument1 = entry->alpha_arg1;
            operands.argument2 = entry->alpha_arg2;
            operands.argument0 = entry->alpha_arg0;
            if (!qxp_ff_emit_operation(emit, entry->alpha_op, &operands,
                                       TRUE))
                return FALSE;
        }
        qxp_ff_append(&emit->output, "), 0.0, 1.0);\n");
    }
    qxp_ff_append(&emit->output, "  gl_FragColor = qxpCur;\n}\n");
    return TRUE;
}

BOOL QxpEmitFixedFunctionPixelGlsl(
    const QXP_FF_PIXEL_STATE *state,
    char *output, DWORD output_bytes, DWORD *source_bytes,
    QXP_PIXEL_SHADER_GLSL_METADATA *metadata,
    QXP_FF_PIXEL_ERROR *error)
{
    QXP_FF_EMIT emit;
    QXP_PIXEL_SHADER_GLSL_METADATA scratch;
    DWORD active_stages = 0;
    DWORD index;
    BOOL uses_factor = FALSE;

    if (error) {
        error->code = QXP_FF_PIXEL_OK;
        error->stage = 0;
        error->value = 0;
    }
    if (source_bytes) *source_bytes = 0;
    if (!state || !output || !output_bytes || !source_bytes || !metadata) {
        if (error) error->code = QXP_FF_PIXEL_ERROR_NULL_ARGUMENT;
        return FALSE;
    }

    /* D3D terminates the chain at the first stage whose colour operation is
     * DISABLE; later stages are inert regardless of how they are set. */
    for (index = 0; index < QXP_FF_PIXEL_STAGE_COUNT; ++index) {
        const QXP_FF_PIXEL_STAGE *entry = &state->stages[index];

        if (entry->color_op == QXP_D3DTOP_DISABLE) break;
        active_stages |= 1UL << index;
        if ((entry->color_arg1 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            (entry->color_arg2 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            (entry->color_arg0 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            (entry->alpha_arg1 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            (entry->alpha_arg2 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            (entry->alpha_arg0 & QXP_D3DTA_SELECTMASK) ==
                QXP_D3DTA_TFACTOR ||
            entry->color_op == QXP_D3DTOP_BLENDFACTORALPHA ||
            entry->alpha_op == QXP_D3DTOP_BLENDFACTORALPHA)
            uses_factor = TRUE;
    }

    qxp_ff_zero(&scratch, (ULONG)sizeof(scratch));
    scratch.color_output_mask = 1;
    /* `current` is seeded from the diffuse colour before the first stage
     * runs, so all four of its components are read unconditionally. */
    scratch.input_color_mask = 1;
    scratch.input_color_components[0] = 15;
    scratch.input_centroid_mask = 3;

    qxp_ff_zero(&emit, (ULONG)sizeof(emit));
    emit.state = state;
    emit.metadata = &scratch;
    emit.error = error;

    /* Interface pass: discard the text, keep the recorded interface. */
    emit.output.text = output;
    emit.output.capacity = output_bytes;
    emit.output.valid = TRUE;
    if (!qxp_ff_emit_body(&emit, active_stages)) return FALSE;

    emit.output.length = 0;
    emit.output.valid = TRUE;
    output[0] = '\0';
    qxp_ff_emit_interface(&emit, active_stages, uses_factor);
    if (!qxp_ff_emit_body(&emit, active_stages)) return FALSE;
    if (!emit.output.valid)
        return qxp_ff_fail(&emit, QXP_FF_PIXEL_ERROR_OUTPUT_CAPACITY,
                           output_bytes);

    *source_bytes = emit.output.length;
    *metadata = scratch;
    return TRUE;
}

DWORD QxpFixedFunctionPixelSignature(const QXP_FF_PIXEL_STATE *state)
{
    const ULONG *words;
    DWORD hash = 2166136261UL;
    DWORD index;

    if (!state) return 0;
    words = (const ULONG *)(const void *)state;
    for (index = 0; index < sizeof(*state) / sizeof(ULONG); ++index) {
        DWORD byte;

        for (byte = 0; byte < 4UL; ++byte) {
            hash ^= (words[index] >> (byte * 8UL)) & 0xffUL;
            hash *= 16777619UL;
        }
    }
    /* Zero marks "no synthesized stage" in the cache key. */
    return hash ? hash : 1UL;
}
