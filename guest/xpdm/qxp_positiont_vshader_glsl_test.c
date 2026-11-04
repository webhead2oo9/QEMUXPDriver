/*
 * Host unit test for the pre-transformed vertex stage emitter.  Mirrors
 * qxp_ffpshader_glsl_test.c: a flat require() helper, one named assertion
 * per behaviour, PASS on stdout and a nonzero exit on any failure.
 */

#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_positiont_vshader_glsl.h"

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

static char source[QXP_POSITIONT_MAX_SOURCE];
static char source_again[QXP_POSITIONT_MAX_SOURCE];
static QXP_VERTEX_SHADER_GLSL_METADATA metadata;
static QXP_POSITIONT_VERTEX_ERROR error;
static DWORD source_bytes;

static int metadata_is_zero(const QXP_VERTEX_SHADER_GLSL_METADATA *value)
{
    const unsigned char *bytes = (const unsigned char *)value;
    DWORD index;

    for (index = 0; index < (DWORD)sizeof(*value); ++index) {
        if (bytes[index]) return 0;
    }
    return 1;
}

static void zero_state(QXP_POSITIONT_VERTEX_STATE *state)
{
    memset(state, 0, sizeof(*state));
}

/* Both colours plus texcoords 0, 1 and 3 with two, three and four
 * components: exercises every mask and every attribute row at once. */
static void representative_state(QXP_POSITIONT_VERTEX_STATE *state)
{
    zero_state(state);
    state->color_mask = 3;
    state->texcoord_mask = 0x0bUL;
    state->texcoord_components[0] = 2;
    state->texcoord_components[1] = 3;
    state->texcoord_components[3] = 4;
}

static BOOL emit(const QXP_POSITIONT_VERTEX_STATE *state)
{
    memset(source, 0, sizeof(source));
    memset(&metadata, 0, sizeof(metadata));
    memset(&error, 0, sizeof(error));
    source_bytes = 0;
    return QxpEmitPositiontVertexGlsl(state, source, (DWORD)sizeof(source),
                                      &source_bytes, &metadata, &error);
}

/* The draw path allocates source_bytes + 1 from the count-only call and
 * then re-emits, comparing both the byte count and the metadata; the two
 * calls must therefore agree exactly. */
static void test_count_only_matches_emit(void)
{
    QXP_POSITIONT_VERTEX_STATE state;
    QXP_VERTEX_SHADER_GLSL_METADATA counted_metadata;
    DWORD counted;

    zero_state(&state);
    if (!require(emit(&state), "a bare state emits")) return;
    counted = 0;
    memset(&counted_metadata, 0, sizeof(counted_metadata));
    require(QxpEmitPositiontVertexGlsl(&state, NULL, 0, &counted,
                                       &counted_metadata, &error),
            "a bare state counts");
    require(counted == source_bytes && counted != 0,
            "a bare state counts the bytes it emits");
    require(!memcmp(&counted_metadata, &metadata, sizeof(metadata)),
            "a bare state counts the same metadata");

    zero_state(&state);
    state.color_mask = 1;
    if (!require(emit(&state), "a diffuse-only state emits")) return;
    counted = 0;
    memset(&counted_metadata, 0, sizeof(counted_metadata));
    require(QxpEmitPositiontVertexGlsl(&state, NULL, 0, &counted,
                                       &counted_metadata, &error),
            "a diffuse-only state counts");
    require(counted == source_bytes,
            "a diffuse-only state counts the bytes it emits");
    require(!memcmp(&counted_metadata, &metadata, sizeof(metadata)),
            "a diffuse-only state counts the same metadata");

    representative_state(&state);
    if (!require(emit(&state), "the representative state emits")) return;
    counted = 0;
    memset(&counted_metadata, 0, sizeof(counted_metadata));
    require(QxpEmitPositiontVertexGlsl(&state, NULL, 0, &counted,
                                       &counted_metadata, &error),
            "the representative state counts");
    require(counted == source_bytes,
            "the representative state counts the bytes it emits");
    require(!memcmp(&counted_metadata, &metadata, sizeof(metadata)),
            "the representative state counts the same metadata");
}

static void test_source_contents(void)
{
    QXP_POSITIONT_VERTEX_STATE state;

    representative_state(&state);
    if (!require(emit(&state), "the representative state emits")) return;
    require(strstr(source, "#version 120\n") != NULL,
            "the source opens with the version directive");
    require(strstr(source, "uniform vec4 qxpVsClipControl;\n") != NULL,
            "the source declares the clip control uniform");
    require(strstr(source, "uniform vec4 qxpVsPositiont;\n") != NULL,
            "the source declares the viewport uniform");
    require(strstr(source, "attribute vec4 qxpVsInput0;\n") != NULL,
            "the source declares the position attribute");
    require(strstr(source, "attribute vec4 qxpVsInput6;\n") != NULL,
            "texcoord 3 declares its attribute");
    require(strstr(source, "varying vec4 qxpPsInV0;\n") != NULL,
            "the source declares the diffuse varying");
    require(strstr(source, "varying vec4 qxpPsInV1;\n") != NULL,
            "the source declares the specular varying");
    require(strstr(source, "varying vec4 qxpPsInT3;\n") != NULL,
            "texcoord 3 declares its varying");
    require(strstr(source, "gl_Position.z = 2.0*qxpP.z - qxpP.w;") != NULL,
            "the depth convention matches the compiled path");
    require(strstr(source, "qxpPsInT3 = qxpVsInput6;") != NULL,
            "texcoord 3 forwards its attribute");
    require(strstr(source, "gl_FogFragCoord = qxpP.z / qxpP.w;") != NULL,
            "the fog coordinate derives from position");
    require(source[source_bytes] == '\0' &&
            strlen(source) == (size_t)source_bytes,
            "the source is NUL-terminated at the reported length");
}

/* An undeclared colour still drives its varying, so the stage pair always
 * links with a defined value: white diffuse, black specular. */
static void test_colour_defaults(void)
{
    QXP_POSITIONT_VERTEX_STATE state;

    zero_state(&state);
    if (!require(emit(&state), "a colourless state emits")) return;
    require(strstr(source, "qxpPsInV0 = vec4(1.0, 1.0, 1.0, 1.0);") != NULL,
            "an undeclared diffuse forwards white");
    require(strstr(source, "qxpPsInV1 = vec4(0.0, 0.0, 0.0, 0.0);") != NULL,
            "an undeclared specular forwards black");
    require(strstr(source, "attribute vec4 qxpVsInput1;") == NULL,
            "an undeclared diffuse declares no attribute");

    zero_state(&state);
    state.color_mask = 3;
    if (!require(emit(&state), "a two-colour state emits")) return;
    require(strstr(source, "qxpPsInV0 = qxpVsInput1;") != NULL,
            "a declared diffuse forwards its attribute");
    require(strstr(source, "qxpPsInV1 = qxpVsInput2;") != NULL,
            "a declared specular forwards its attribute");
}

static void test_metadata(void)
{
    QXP_POSITIONT_VERTEX_STATE state;

    zero_state(&state);
    if (!require(emit(&state), "a bare state emits")) return;
    require(metadata.input_mask == 1UL,
            "a bare state declares only its position");
    require(metadata.declaration_count == 1UL,
            "a bare state counts one declaration");

    representative_state(&state);
    if (!require(emit(&state), "the representative state emits")) return;
    require(metadata.input_mask ==
                (1UL | (3UL << 1) | (0x0bUL << 3)),
            "the input mask places colours and texcoords");
    require(metadata.declaration_count == 6UL,
            "the declaration count matches the input mask popcount");
    require(metadata.input_usage[0] == 9UL &&
            metadata.input_usage_index[0] == 0UL &&
            metadata.input_component_mask[0] == 15,
            "input zero is the pre-transformed position");
    require(metadata.input_usage[1] == 10UL &&
            metadata.input_usage_index[1] == 0UL &&
            metadata.input_component_mask[1] == 15,
            "input one is the diffuse colour");
    require(metadata.input_usage[2] == 10UL &&
            metadata.input_usage_index[2] == 1UL &&
            metadata.input_component_mask[2] == 15,
            "input two is the specular colour");
    require(metadata.input_usage[3] == 5UL &&
            metadata.input_usage_index[3] == 0UL &&
            metadata.input_component_mask[3] == 15,
            "input three is texcoord zero");
    require(metadata.input_usage[4] == 5UL &&
            metadata.input_usage_index[4] == 1UL &&
            metadata.input_component_mask[4] == 15,
            "input four is texcoord one");
    require(metadata.input_usage[6] == 5UL &&
            metadata.input_usage_index[6] == 3UL &&
            metadata.input_component_mask[6] == 15,
            "input six is texcoord three");
    require(metadata.input_usage[5] == 0UL &&
            metadata.input_usage[7] == 0UL,
            "undeclared inputs stay empty");
    require(metadata.attribute_output_mask == 3UL &&
            metadata.attribute_output_components[0] == 15 &&
            metadata.attribute_output_components[1] == 15,
            "both colour varyings are always written in full");
    require(metadata.texcoord_output_mask == 0x0bUL,
            "the texcoord output mask mirrors the declaration");
    require(metadata.texcoord_output_components[0] == 15 &&
            metadata.texcoord_output_components[1] == 15 &&
            metadata.texcoord_output_components[3] == 15 &&
            metadata.texcoord_output_components[2] == 0,
            "declared texcoords are written in full");
    require(metadata.writes_position == TRUE &&
            metadata.position_component_mask == 15,
            "the stage writes a full position");
    require(metadata.writes_fog == FALSE,
            "the stage reports no fog output");
    require(metadata.output_centroid_mask == 0UL &&
            metadata.instruction_count == 0UL &&
            metadata.slot_count == 0UL,
            "bytecode accounting stays zero");
}

/* A refusal must name the failing element and leave every output cleared;
 * the metadata is poisoned first so the zeroing is actually observed. */
static void require_rejected(const QXP_POSITIONT_VERTEX_STATE *state,
                             DWORD code, DWORD index, DWORD value,
                             const char *message)
{
    DWORD written = 7;

    memset(&metadata, 0xff, sizeof(metadata));
    memset(&error, 0, sizeof(error));
    memset(source, 0, sizeof(source));
    require(!QxpEmitPositiontVertexGlsl(state, source,
                                        (DWORD)sizeof(source), &written,
                                        &metadata, &error),
            message);
    require(error.code == code, message);
    require(error.index == index, message);
    require(error.value == value, message);
    require(written == 0UL, message);
    require(metadata_is_zero(&metadata), message);
}

static void test_rejections(void)
{
    QXP_POSITIONT_VERTEX_STATE state;
    DWORD counted;

    zero_state(&state);
    state.color_mask = 4;
    require_rejected(&state, QXP_POSITIONT_VERTEX_ERROR_COLOR, 2, 4,
                     "a colour mask bit past COLOR1 is refused");

    zero_state(&state);
    state.texcoord_mask = 0x100UL;
    require_rejected(&state, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD, 8,
                     0x100UL,
                     "a texcoord mask bit past TEXCOORD7 is refused");

    zero_state(&state);
    state.texcoord_mask = 1;
    require_rejected(&state, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD, 0, 0,
                     "a declared texcoord with no components is refused");

    zero_state(&state);
    state.texcoord_mask = 1;
    state.texcoord_components[0] = 5;
    require_rejected(&state, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD, 0, 5,
                     "a declared texcoord with five components is refused");

    zero_state(&state);
    state.texcoord_components[2] = 3;
    require_rejected(&state, QXP_POSITIONT_VERTEX_ERROR_TEXCOORD, 2, 3,
                     "an undeclared texcoord with components is refused");

    /* The count-only query validates too: a refused layout must not report
     * a byte count the draw path would then allocate. */
    zero_state(&state);
    state.color_mask = 4;
    counted = 1;
    memset(&error, 0, sizeof(error));
    require(!QxpEmitPositiontVertexGlsl(&state, NULL, 0, &counted,
                                        &metadata, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_COLOR &&
            counted == 0UL,
            "the count-only query refuses an invalid state");
}

static void test_null_arguments_refused(void)
{
    QXP_POSITIONT_VERTEX_STATE state;
    DWORD written = 1;

    zero_state(&state);
    memset(&error, 0, sizeof(error));
    require(!QxpEmitPositiontVertexGlsl(NULL, source,
                                        (DWORD)sizeof(source), &written,
                                        &metadata, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT,
            "a null state is refused");
    require(!QxpEmitPositiontVertexGlsl(&state, source,
                                        (DWORD)sizeof(source), NULL,
                                        &metadata, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT,
            "a null byte count is refused");
    require(!QxpEmitPositiontVertexGlsl(&state, source,
                                        (DWORD)sizeof(source), &written,
                                        NULL, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT,
            "a null metadata sink is refused");
    require(!QxpEmitPositiontVertexGlsl(&state, source,
                                        (DWORD)sizeof(source), &written,
                                        &metadata, NULL),
            "a null error sink is refused");
    require(!QxpEmitPositiontVertexGlsl(&state, NULL,
                                        (DWORD)sizeof(source), &written,
                                        &metadata, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT,
            "a null output with a nonzero capacity is refused");
    require(!QxpEmitPositiontVertexGlsl(&state, source, 0, &written,
                                        &metadata, &error) &&
            error.code == QXP_POSITIONT_VERTEX_ERROR_NULL_ARGUMENT,
            "a zero capacity with an output is refused");
}

/* The emitter must refuse rather than truncate when the buffer is too
 * small, because a truncated stage would still be handed to the compiler. */
static void test_capacity_refusal(void)
{
    QXP_POSITIONT_VERTEX_STATE state;
    DWORD written;

    representative_state(&state);
    if (!require(emit(&state), "the representative state emits")) return;

    memset(source, 0xaa, sizeof(source));
    memset(&metadata, 0xff, sizeof(metadata));
    memset(&error, 0, sizeof(error));
    written = 9;
    /* source_bytes excludes the NUL, so a buffer of exactly source_bytes
     * is one byte too small. */
    require(!QxpEmitPositiontVertexGlsl(&state, source, source_bytes,
                                        &written, &metadata, &error),
            "a buffer one byte too small is refused");
    require(error.code == QXP_POSITIONT_VERTEX_ERROR_OUTPUT_CAPACITY,
            "the refusal names the capacity");
    require(written == 0UL, "a refused emit reports no length");
    require(metadata_is_zero(&metadata),
            "a refused emit clears the metadata");
    require(source[0] == '\0', "a refused emit clears the output");
}

static void test_determinism(void)
{
    QXP_POSITIONT_VERTEX_STATE state;
    QXP_VERTEX_SHADER_GLSL_METADATA metadata_again;
    DWORD again_bytes = 0;

    representative_state(&state);
    if (!require(emit(&state), "the representative state emits")) return;
    memset(source_again, 0, sizeof(source_again));
    memset(&metadata_again, 0, sizeof(metadata_again));
    require(QxpEmitPositiontVertexGlsl(&state, source_again,
                                       (DWORD)sizeof(source_again),
                                       &again_bytes, &metadata_again,
                                       &error),
            "the representative state emits twice");
    require(again_bytes == source_bytes &&
            !strcmp(source, source_again),
            "two emits of one state are byte-identical");
}

/* The signature keys the native program cache; layouts that shade
 * differently must not share an entry, and zero means no stage at all. */
static void test_signature(void)
{
    QXP_POSITIONT_VERTEX_STATE a;
    QXP_POSITIONT_VERTEX_STATE b;
    DWORD signature_a;

    representative_state(&a);
    signature_a = QxpPositiontVertexSignature(&a);
    require(signature_a != 0UL, "a valid state has a nonzero signature");

    b = a;
    b.texcoord_components[3] = 2;
    require(QxpPositiontVertexSignature(&b) != signature_a,
            "a component count change changes the signature");

    b = a;
    b.color_mask = 1;
    require(QxpPositiontVertexSignature(&b) != signature_a,
            "a colour mask change changes the signature");

    b = a;
    b.texcoord_mask = 0x0bUL | 0x40UL;
    b.texcoord_components[6] = 1;
    require(QxpPositiontVertexSignature(&b) != signature_a,
            "a texcoord mask change changes the signature");

    require(QxpPositiontVertexSignature(NULL) == 0UL,
            "a null state has no signature");
}

/* Writes one representative stage out so the live GL smoke can compile and
 * link it.  Substring checks prove the emitter's intent; only a real driver
 * proves the text is valid GLSL. */
static int emit_glsl(const char *path)
{
    QXP_POSITIONT_VERTEX_STATE state;
    QXP_VERTEX_SHADER_GLSL_METADATA metadata;
    QXP_POSITIONT_VERTEX_ERROR error;
    static char source[QXP_POSITIONT_MAX_SOURCE];
    DWORD source_bytes = 0;
    FILE *file;

    memset(&state, 0, sizeof(state));
    state.color_mask = 3;
    state.texcoord_mask = 0x0BUL;
    state.texcoord_components[0] = 2;
    state.texcoord_components[1] = 3;
    state.texcoord_components[3] = 4;
    if (!QxpEmitPositiontVertexGlsl(
            &state, source, (DWORD)sizeof(source), &source_bytes,
            &metadata, &error)) {
        fprintf(stderr, "positiont emit failed: code %lu\n",
                (unsigned long)error.code);
        return 1;
    }
    file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "could not open %s\n", path);
        return 1;
    }
    if (fwrite(source, 1, source_bytes, file) != source_bytes) {
        fclose(file);
        fprintf(stderr, "could not write %s\n", path);
        return 1;
    }
    fclose(file);
    printf("emitted positiont GLSL %lu bytes\n",
           (unsigned long)source_bytes);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit-glsl") == 0)
        return emit_glsl(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "usage: qxp_positiont_vshader_glsl_test "
                        "[--emit-glsl path]\n");
        return 2;
    }

    test_count_only_matches_emit();
    test_source_contents();
    test_colour_defaults();
    test_metadata();
    test_rejections();
    test_null_arguments_refused();
    test_capacity_refusal();
    test_determinism();
    test_signature();

    if (failures) {
        fprintf(stderr, "qxp_positiont_vshader_glsl: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("qxp_positiont_vshader_glsl: PASS\n");
    return 0;
}
