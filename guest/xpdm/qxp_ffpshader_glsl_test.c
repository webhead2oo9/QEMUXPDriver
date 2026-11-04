#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_ffpshader_glsl.h"

#define QXP_TEXTURE_OP_STANDALONE
#include "qxpmp/qxp_texture_op.h"
#undef QXP_TEXTURE_OP_STANDALONE

static int failures;

static int require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
        return 0;
    }
    return 1;
}

static char source[16384];
static QXP_PIXEL_SHADER_GLSL_METADATA metadata;
static QXP_FF_PIXEL_ERROR error;
static DWORD source_bytes;

static void zero_state(QXP_FF_PIXEL_STATE *state)
{
    DWORD stage;

    memset(state, 0, sizeof(*state));
    for (stage = 0; stage < QXP_FF_PIXEL_STAGE_COUNT; ++stage) {
        state->stages[stage].color_op = QXP_D3DTOP_DISABLE;
        state->stages[stage].alpha_op = QXP_D3DTOP_DISABLE;
    }
}

static BOOL emit(const QXP_FF_PIXEL_STATE *state)
{
    memset(source, 0, sizeof(source));
    memset(&metadata, 0, sizeof(metadata));
    memset(&error, 0, sizeof(error));
    source_bytes = 0;
    return QxpEmitFixedFunctionPixelGlsl(state, source, (DWORD)sizeof(source),
                                         &source_bytes, &metadata, &error);
}

/* Sands of Time draws with a programmable vertex shader and no pixel shader:
 * stage 0 selects the texture colour and the diffuse alpha, stage 1 is off.
 * This exact configuration is what the native draw gate refused. */
static void test_sands_of_time_combiner(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG2;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "sands of time combiner is expressible"))
        return;
    require(strstr(source, "uniform sampler2D qxpPsS0;") != NULL,
            "stage zero declares its sampler");
    require(strstr(source, "varying vec4 qxpPsInT0;") != NULL,
            "stage zero declares its coordinate set");
    require(strstr(source,
                   "qxpTex0 = texture2D(qxpPsS0, qxpPsInT0.xy);") != NULL,
            "stage zero samples its coordinate set");
    require(strstr(source,
                   "clamp(vec4(qxpTex0.rgb, qxpPsInV0.a), 0.0, 1.0)") != NULL,
            "texture colour combines with diffuse alpha");
    require(strstr(source, "gl_FragColor = qxpCur;") != NULL,
            "the chain result is the fragment colour");
    /* An unreferenced texture factor must not reach the source, or every
     * change to it would needlessly redefine the program. */
    require(strstr(source, "qxpTFactor") == NULL,
            "unreferenced texture factor is not emitted");
    require(metadata.sampler_mask == 1UL, "one sampler is reported");
    require(metadata.sampler_types[0] == QXP_SHADER_SAMPLER_2D,
            "the sampler is two-dimensional");
    require(metadata.input_texture_mask == 1UL,
            "one coordinate set is reported");
    require(metadata.input_color_mask == 1UL, "diffuse input is reported");
    require(metadata.color_output_mask == 1UL, "one colour output");
    require(!metadata.writes_depth && !metadata.uses_discard,
            "a combiner neither kills nor writes depth");
    require(source_bytes == (DWORD)strlen(source),
            "reported length matches the emitted source");
}

/* The chain stops at the first disabled stage even when later stages are
 * configured, matching how Direct3D reads the stage list. */
static void test_disabled_stage_terminates_chain(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;
    /* Stage 1 disabled, stage 2 configured: stage 2 must stay inert. */
    state.stages[2].color_op = QXP_D3DTOP_MODULATE;
    state.stages[2].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[2].color_arg2 = QXP_D3DTA_CURRENT;
    state.stages[2].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "a terminated chain is expressible")) return;
    require(metadata.sampler_mask == 1UL,
            "a stage past the terminator declares no sampler");
    require(strstr(source, "qxpTex2") == NULL,
            "a stage past the terminator emits no work");
}

/* An entirely disabled chain is the untextured fixed-function case: the
 * fragment is the interpolated diffuse colour. */
static void test_disabled_chain_passes_diffuse(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    if (!require(emit(&state), "a disabled chain is expressible")) return;
    require(metadata.sampler_mask == 0UL, "no samplers are declared");
    require(metadata.input_texture_mask == 0UL,
            "no coordinate sets are declared");
    require(strstr(source, "vec4 qxpCur = qxpPsInV0;") != NULL,
            "current starts at the diffuse colour");
    require(strstr(source, "gl_FragColor = qxpCur;") != NULL,
            "the diffuse colour reaches the framebuffer");
}

/* Two textured stages chain through `current`, which is what a lightmapped
 * or detail-textured surface configures. */
static void test_two_stage_modulate(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;
    state.stages[1].color_op = QXP_D3DTOP_MODULATE2X;
    state.stages[1].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[1].color_arg2 = QXP_D3DTA_CURRENT;
    state.stages[1].alpha_op = QXP_D3DTOP_SELECTARG2;
    state.stages[1].alpha_arg2 = QXP_D3DTA_CURRENT;
    state.stages[1].coordinate_index = 1;
    state.stages[1].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "a two stage chain is expressible")) return;
    require(metadata.sampler_mask == 3UL, "both samplers are reported");
    require(metadata.input_texture_mask == 3UL,
            "both coordinate sets are reported");
    require(strstr(source,
                   "qxpTex1 = texture2D(qxpPsS1, qxpPsInT1.xy);") != NULL,
            "stage one samples its own coordinate set");
    require(strstr(source, "(qxpTex1.rgb * qxpCur.rgb) * 2.0") != NULL,
            "modulate2x doubles the product");
}

/* Argument modifiers are not commutative: Direct3D replicates alpha first
 * and complements the replicated value. */
static void test_argument_modifiers(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_MODULATE;
    state.stages[0].color_arg1 =
        QXP_D3DTA_TEXTURE | QXP_D3DTA_ALPHAREPLICATE | QXP_D3DTA_COMPLEMENT;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE | QXP_D3DTA_COMPLEMENT;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE | QXP_D3DTA_COMPLEMENT;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "modified arguments are expressible")) return;
    require(strstr(source, "(vec3(1.0) - qxpTex0.aaa)") != NULL,
            "alpha replicate precedes complement");
    require(strstr(source, "(vec3(1.0) - qxpPsInV0.rgb)") != NULL,
            "a complemented colour argument inverts all three channels");
    require(strstr(source, "(1.0 - qxpTex0.a)") != NULL,
            "a complemented alpha argument is scalar");
}

/* The factor is per-draw state the miniport feeds through a uniform, so the
 * generated stage must declare it rather than fold a colour into the source. */
static void test_texture_factor_uniform(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TFACTOR;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TFACTOR;

    if (!require(emit(&state), "a texture factor chain is expressible"))
        return;
    require(strstr(source, "uniform vec4 qxpTFactor;") != NULL,
            "the factor arrives as a uniform");
    require(strstr(source, "const vec4 qxpTFactor") == NULL,
            "no colour is baked into the stage");
    require(metadata.sampler_mask == 0UL,
            "a factor-only stage samples nothing");
}

/* A stage that does not read the factor must not declare its uniform. */
static void test_texture_factor_absent_when_unused(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_DIFFUSE;

    if (!require(emit(&state), "a diffuse-only chain is expressible"))
        return;
    require(strstr(source, "qxpTFactor") == NULL,
            "an unused factor declares no uniform");
}

/* The signature keys the program cache; configurations that shade
 * differently must not share an entry. */
static void test_signature_separates_configurations(void)
{
    QXP_FF_PIXEL_STATE first;
    QXP_FF_PIXEL_STATE second;
    DWORD signature;

    zero_state(&first);
    first.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    first.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    first.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;
    second = first;

    signature = QxpFixedFunctionPixelSignature(&first);
    require(signature != 0UL, "a signature is never the absent marker");
    require(signature == QxpFixedFunctionPixelSignature(&second),
            "identical configurations share a signature");

    second.stages[0].color_arg1 = QXP_D3DTA_DIFFUSE;
    require(signature != QxpFixedFunctionPixelSignature(&second),
            "a different argument changes the signature");

    second = first;
    second.stages[0].color_op = QXP_D3DTOP_MODULATE;
    require(signature != QxpFixedFunctionPixelSignature(&second),
            "a different operation changes the signature");

    /* The factor is uniform state now, so it is deliberately not part of the
     * key: an animated factor must reuse one compiled program. */

    second = first;
    second.stages[0].coordinate_index = 3;
    require(signature != QxpFixedFunctionPixelSignature(&second),
            "a different coordinate set changes the signature");
}

/* A stage that reads its texture without one bound must refuse rather than
 * shade from an unbound sampler. */
static void test_missing_texture_refused(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;

    require(!emit(&state), "a stage without its texture is refused");
    require(error.code == QXP_FF_PIXEL_ERROR_TEXTURE,
            "the refusal names the missing texture");
}

/* Bump-environment stages need matrix state the packet does not carry, so
 * they stay refused instead of shading incorrectly. */
static void test_unsupported_operations_refused(void)
{
    QXP_FF_PIXEL_STATE state;
    DWORD unsupported[3];
    DWORD index;

    unsupported[0] = QXP_D3DTOP_BUMPENVMAP;
    unsupported[1] = QXP_D3DTOP_BUMPENVMAPLUMINANCE;
    unsupported[2] = QXP_D3DTOP_PREMODULATE;

    for (index = 0; index < 3; ++index) {
        zero_state(&state);
        state.stages[0].color_op = unsupported[index];
        state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
        state.stages[0].color_arg2 = QXP_D3DTA_CURRENT;
        state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG2;
        state.stages[0].alpha_arg2 = QXP_D3DTA_CURRENT;
        state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;
        require(!emit(&state), "an unexpressible operation is refused");
        require(error.code == QXP_FF_PIXEL_ERROR_OPERATION,
                "the refusal names the operation");
    }
}

/* Colour-only forms have no alpha meaning; accepting one as an alpha
 * operation would silently shade the wrong channel. */
static void test_colour_only_operation_refused_for_alpha(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_op = QXP_D3DTOP_DOTPRODUCT3;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    require(!emit(&state), "a colour-only operation is refused for alpha");
    require(error.code == QXP_FF_PIXEL_ERROR_OPERATION,
            "the refusal names the operation");
}

/* A dot-product stage writes its result to all four channels, so the alpha
 * operation is not consulted at all. */
static void test_dotproduct3_replicates_to_alpha(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_DOTPRODUCT3;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "a dot product stage is expressible")) return;
    require(strstr(source,
                   "vec3(4.0 * dot(qxpTex0.rgb - vec3(0.5), "
                   "qxpPsInV0.rgb - vec3(0.5)))") != NULL,
            "the colour result is the signed-scaled dot product");
    require(strstr(source,
                   ", 4.0 * dot(qxpTex0.rgb - vec3(0.5), "
                   "qxpPsInV0.rgb - vec3(0.5)))") != NULL,
            "the same dot product becomes the alpha result");
}

/* Blend operations read a named alpha that is independent of their two
 * arguments; a texture blend must sample the stage's own texture. */
static void test_blend_operations(void)
{
    QXP_FF_PIXEL_STATE state;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_BLENDTEXTUREALPHA;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG2;
    state.stages[0].alpha_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "a texture-alpha blend is expressible"))
        return;
    require(strstr(source,
                   "mix(qxpPsInV0.rgb, qxpTex0.rgb, qxpTex0.a)") != NULL,
            "the blend weights argument one by the texture alpha");

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_BLENDFACTORALPHA;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG2;
    state.stages[0].alpha_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    if (!require(emit(&state), "a factor-alpha blend is expressible")) return;
    require(strstr(source, "qxpTFactor.a") != NULL,
            "the blend reads the factor alpha");
    require(strstr(source, "uniform vec4 qxpTFactor;") != NULL,
            "an operation that reads the factor declares it");
}

/* Every stage result is clamped, as fixed-function processing saturates
 * between stages rather than carrying overbright values forward. */
static void test_stage_result_is_clamped(void)
{
    QXP_FF_PIXEL_STATE state;
    const char *scan;
    int clamps = 0;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_MODULATE4X;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG2;
    state.stages[0].alpha_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;
    state.stages[1].color_op = QXP_D3DTOP_ADD;
    state.stages[1].color_arg1 = QXP_D3DTA_CURRENT;
    state.stages[1].color_arg2 = QXP_D3DTA_DIFFUSE;
    state.stages[1].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[1].alpha_arg1 = QXP_D3DTA_CURRENT;

    if (!require(emit(&state), "an overbright chain is expressible")) return;
    for (scan = source; (scan = strstr(scan, ", 0.0, 1.0);")) != NULL; ++scan)
        ++clamps;
    require(clamps == 2, "each active stage saturates its own result");
}

/* The emitter must refuse rather than truncate when the buffer is too
 * small, because a truncated stage would still be handed to the compiler. */
static void test_capacity_refusal(void)
{
    QXP_FF_PIXEL_STATE state;
    char small[24];
    DWORD written = 0;
    QXP_PIXEL_SHADER_GLSL_METADATA small_metadata;
    QXP_FF_PIXEL_ERROR small_error;

    zero_state(&state);
    state.stages[0].color_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].color_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].alpha_op = QXP_D3DTOP_SELECTARG1;
    state.stages[0].alpha_arg1 = QXP_D3DTA_TEXTURE;
    state.stages[0].sampler_type = QXP_SHADER_SAMPLER_2D;

    memset(&small_metadata, 0, sizeof(small_metadata));
    memset(&small_error, 0, sizeof(small_error));
    require(!QxpEmitFixedFunctionPixelGlsl(&state, small, (DWORD)sizeof(small),
                                           &written, &small_metadata,
                                           &small_error),
            "an undersized buffer is refused");
    require(small_error.code == QXP_FF_PIXEL_ERROR_OUTPUT_CAPACITY,
            "the refusal names the capacity");
    require(written == 0UL, "a refused emit reports no length");
}

static void test_null_arguments_refused(void)
{
    QXP_FF_PIXEL_STATE state;
    DWORD written = 1;

    zero_state(&state);
    require(!QxpEmitFixedFunctionPixelGlsl(NULL, source,
                                           (DWORD)sizeof(source), &written,
                                           &metadata, &error),
            "a null state is refused");
    require(!QxpEmitFixedFunctionPixelGlsl(&state, NULL,
                                           (DWORD)sizeof(source), &written,
                                           &metadata, &error),
            "a null output is refused");
    require(!QxpEmitFixedFunctionPixelGlsl(&state, source, 0, &written,
                                           &metadata, &error),
            "a zero capacity is refused");
    require(!QxpEmitFixedFunctionPixelGlsl(&state, source,
                                           (DWORD)sizeof(source), &written,
                                           NULL, &error),
            "a null metadata sink is refused");
    require(QxpFixedFunctionPixelSignature(NULL) == 0UL,
            "a null state has no signature");
}

int main(void)
{
    test_sands_of_time_combiner();
    test_disabled_stage_terminates_chain();
    test_disabled_chain_passes_diffuse();
    test_two_stage_modulate();
    test_argument_modifiers();
    test_texture_factor_uniform();
    test_texture_factor_absent_when_unused();
    test_signature_separates_configurations();
    test_missing_texture_refused();
    test_unsupported_operations_refused();
    test_colour_only_operation_refused_for_alpha();
    test_dotproduct3_replicates_to_alpha();
    test_blend_operations();
    test_stage_result_is_clamped();
    test_capacity_refusal();
    test_null_arguments_refused();

    if (failures) {
        fprintf(stderr, "qxp_ffpshader_glsl: %d failure(s)\n", failures);
        return 1;
    }
    printf("qxp_ffpshader_glsl: PASS\n");
    return 0;
}
