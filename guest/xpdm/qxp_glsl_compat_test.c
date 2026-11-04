#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#define QXP_MAX_INTERNAL_GLSL_SOURCE_BYTES 16384L
#define QXP_MAX_CLIP_GLSL_SOURCE_BYTES (128L * 1024L)
#define QXP_MAX_PS20_GLSL_SOURCE_BYTES (512L * 1024L)
#define QXP_MAX_VS20_GLSL_SOURCE_BYTES (512L * 1024L)
#define QXP_MAX_ARBVP_SOURCE_BYTES (128L * 1024L)
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qxp_glsl_api {
    PFNGLATTACHSHADERPROC attach_shader;
    PFNGLBINDATTRIBLOCATIONPROC bind_attrib_location;
    PFNGLBINDPROGRAMARBPROC bind_program_arb;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer;
    PFNGLBLITFRAMEBUFFERPROC blit_framebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer_status;
    PFNGLCOMPILESHADERPROC compile_shader;
    PFNGLCREATEPROGRAMPROC create_program;
    PFNGLCREATESHADERPROC create_shader;
    PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers;
    PFNGLDELETEPROGRAMPROC delete_program;
    PFNGLDELETEPROGRAMSARBPROC delete_programs_arb;
    PFNGLDELETESHADERPROC delete_shader;
    PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture_2d;
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers;
    PFNGLGENPROGRAMSARBPROC gen_programs_arb;
    PFNGLGETPROGRAMINFOLOGPROC get_program_info_log;
    PFNGLGETPROGRAMIVPROC get_program_iv;
    PFNGLGETPROGRAMIVARBPROC get_program_iv_arb;
    PFNGLGETSHADERINFOLOGPROC get_shader_info_log;
    PFNGLGETSHADERIVPROC get_shader_iv;
    PFNGLGETUNIFORMLOCATIONPROC get_uniform_location;
    PFNGLLINKPROGRAMPROC link_program;
    PFNGLPOINTPARAMETERFPROC point_parameter_f;
    PFNGLPOINTPARAMETERFVPROC point_parameter_fv;
    PFNGLPOINTPARAMETERIPROC point_parameter_i;
    PFNGLPROGRAMSTRINGARBPROC program_string_arb;
    PFNGLSHADERSOURCEPROC shader_source;
    PFNGLUNIFORM1IPROC uniform_1i;
    PFNGLUNIFORM4FVPROC uniform_4fv;
    PFNGLUNIFORM4IVPROC uniform_4iv;
    PFNGLUSEPROGRAMPROC use_program;
    PFNGLVERTEXATTRIBPOINTERARBPROC vertex_attrib_pointer_arb;
    PFNGLENABLEVERTEXATTRIBARRAYARBPROC enable_vertex_attrib_array_arb;
    PFNGLDISABLEVERTEXATTRIBARRAYARBPROC disable_vertex_attrib_array_arb;
} QXP_GLSL_API;

static LRESULT CALLBACK qxp_window_proc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static PROC qxp_gl_proc(const char *name)
{
    PROC proc = wglGetProcAddress(name);

    if (proc == NULL || proc == (PROC)1 || proc == (PROC)2 ||
        proc == (PROC)3 || proc == (PROC)-1) return NULL;
    return proc;
}

static int qxp_load_glsl(QXP_GLSL_API *api)
{
#define QXP_LOAD(member, name)                                                \
    do {                                                                      \
        api->member = (void *)qxp_gl_proc(name);                              \
        if (!api->member) {                                                   \
            fprintf(stderr, "missing %s\n", name);                          \
            return 0;                                                         \
        }                                                                     \
    } while (0)

    memset(api, 0, sizeof(*api));
    QXP_LOAD(attach_shader, "glAttachShader");
    QXP_LOAD(bind_attrib_location, "glBindAttribLocation");
    QXP_LOAD(bind_program_arb, "glBindProgramARB");
    QXP_LOAD(bind_framebuffer, "glBindFramebuffer");
    QXP_LOAD(blit_framebuffer, "glBlitFramebuffer");
    QXP_LOAD(check_framebuffer_status, "glCheckFramebufferStatus");
    QXP_LOAD(compile_shader, "glCompileShader");
    QXP_LOAD(create_program, "glCreateProgram");
    QXP_LOAD(create_shader, "glCreateShader");
    QXP_LOAD(delete_framebuffers, "glDeleteFramebuffers");
    QXP_LOAD(delete_program, "glDeleteProgram");
    QXP_LOAD(delete_programs_arb, "glDeleteProgramsARB");
    QXP_LOAD(delete_shader, "glDeleteShader");
    QXP_LOAD(framebuffer_texture_2d, "glFramebufferTexture2D");
    QXP_LOAD(gen_framebuffers, "glGenFramebuffers");
    QXP_LOAD(gen_programs_arb, "glGenProgramsARB");
    QXP_LOAD(get_program_info_log, "glGetProgramInfoLog");
    QXP_LOAD(get_program_iv, "glGetProgramiv");
    QXP_LOAD(get_program_iv_arb, "glGetProgramivARB");
    QXP_LOAD(get_shader_info_log, "glGetShaderInfoLog");
    QXP_LOAD(get_shader_iv, "glGetShaderiv");
    QXP_LOAD(get_uniform_location, "glGetUniformLocation");
    QXP_LOAD(link_program, "glLinkProgram");
    QXP_LOAD(point_parameter_f, "glPointParameterf");
    QXP_LOAD(point_parameter_fv, "glPointParameterfv");
    QXP_LOAD(point_parameter_i, "glPointParameteri");
    QXP_LOAD(program_string_arb, "glProgramStringARB");
    QXP_LOAD(shader_source, "glShaderSource");
    QXP_LOAD(uniform_1i, "glUniform1i");
    QXP_LOAD(uniform_4fv, "glUniform4fv");
    QXP_LOAD(uniform_4iv, "glUniform4iv");
    QXP_LOAD(use_program, "glUseProgram");
    QXP_LOAD(vertex_attrib_pointer_arb, "glVertexAttribPointerARB");
    QXP_LOAD(enable_vertex_attrib_array_arb,
             "glEnableVertexAttribArrayARB");
    QXP_LOAD(disable_vertex_attrib_array_arb,
             "glDisableVertexAttribArrayARB");
#undef QXP_LOAD
    return 1;
}

static void qxp_print_shader_log(const QXP_GLSL_API *api, GLuint shader)
{
    GLchar log[1024];
    GLsizei length = 0;

    memset(log, 0, sizeof(log));
    api->get_shader_info_log(shader, (GLsizei)(sizeof(log) - 1), &length, log);
    if (length > 0) fprintf(stderr, "shader log: %s\n", log);
}

static void qxp_print_program_log(const QXP_GLSL_API *api, GLuint program)
{
    GLchar log[1024];
    GLsizei length = 0;

    memset(log, 0, sizeof(log));
    api->get_program_info_log(program, (GLsizei)(sizeof(log) - 1), &length, log);
    if (length > 0) fprintf(stderr, "program log: %s\n", log);
}

static GLuint qxp_build_vertex_only_program(const QXP_GLSL_API *api,
                                             const GLchar *source)
{
    const GLchar *source_pointer = source;
    GLint status = GL_FALSE;
    GLuint shader;
    GLuint program;
    GLuint index;
    char attribute[16];

    shader = api->create_shader(GL_VERTEX_SHADER);
    if (!shader) {
        fprintf(stderr, "glCreateShader returned zero\n");
        return 0;
    }
    api->shader_source(shader, 1, &source_pointer, NULL);
    api->compile_shader(shader);
    api->get_shader_iv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "vertex shader compilation failed\n");
        qxp_print_shader_log(api, shader);
        api->delete_shader(shader);
        return 0;
    }

    program = api->create_program();
    if (!program) {
        fprintf(stderr, "glCreateProgram returned zero\n");
        api->delete_shader(shader);
        return 0;
    }
    api->attach_shader(program, shader);
    for (index = 0; index < 16; ++index) {
        sprintf(attribute, "qxpV%lu", (unsigned long)index);
        api->bind_attrib_location(program, index, attribute);
    }
    api->link_program(program);
    api->get_program_iv(program, GL_LINK_STATUS, &status);
    api->delete_shader(shader);
    if (status != GL_TRUE) {
        fprintf(stderr, "vertex-only program link failed\n");
        qxp_print_program_log(api, program);
        api->delete_program(program);
        return 0;
    }
    return program;
}

static int qxp_pixel_near(const GLubyte *pixel, int red, int green, int blue);

static GLuint qxp_build_ps20_program(const QXP_GLSL_API *api,
                                      const GLchar *fragment_source)
{
    static const GLchar vertex_source[] =
        "#version 120\n"
        "centroid varying vec4 qxpPsInV0;\n"
        "centroid varying vec4 qxpPsInT0;\n"
        "void main(void) {\n"
        "  gl_Position = gl_Vertex;\n"
        "  qxpPsInV0 = gl_Color;\n"
        "  qxpPsInT0 = gl_MultiTexCoord0;\n"
        "}\n";
    const GLchar *source_pointer;
    GLint status;
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;

    status = GL_FALSE;
    vertex_shader = api->create_shader(GL_VERTEX_SHADER);
    fragment_shader = api->create_shader(GL_FRAGMENT_SHADER);
    if (!vertex_shader || !fragment_shader) {
        fprintf(stderr, "ps20 glCreateShader returned zero\n");
        if (vertex_shader) api->delete_shader(vertex_shader);
        if (fragment_shader) api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = vertex_source;
    api->shader_source(vertex_shader, 1, &source_pointer, NULL);
    api->compile_shader(vertex_shader);
    api->get_shader_iv(vertex_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "ps20 bridge vertex compilation failed\n");
        qxp_print_shader_log(api, vertex_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = fragment_source;
    api->shader_source(fragment_shader, 1, &source_pointer, NULL);
    api->compile_shader(fragment_shader);
    api->get_shader_iv(fragment_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "ps20 fragment compilation failed\n");
        qxp_print_shader_log(api, fragment_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    program = api->create_program();
    if (!program) {
        fprintf(stderr, "ps20 glCreateProgram returned zero\n");
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    api->attach_shader(program, vertex_shader);
    api->attach_shader(program, fragment_shader);
    api->link_program(program);
    api->get_program_iv(program, GL_LINK_STATUS, &status);
    api->delete_shader(vertex_shader);
    api->delete_shader(fragment_shader);
    if (status != GL_TRUE) {
        fprintf(stderr, "ps20 GLSL program link failed\n");
        qxp_print_program_log(api, program);
        api->delete_program(program);
        return 0;
    }
    return program;
}

static GLuint qxp_build_vs20_program(const QXP_GLSL_API *api,
                                      const GLchar *vertex_source,
                                      int output_mode)
{
    static const GLchar coverage_fragment_source[] =
        "#version 120\n"
        "centroid varying vec4 qxpPsInV0;\n"
        "centroid varying vec4 qxpPsInV1;\n"
        "centroid varying vec4 qxpPsInT0;\n"
        "varying vec4 qxpPsInT1;\n"
        "void main(void) { gl_FragColor = qxpPsInV0; }\n";
    static const GLchar primary_fragment_source[] =
        "#version 120\n"
        "centroid varying vec4 qxpPsInV0;\n"
        "centroid varying vec4 qxpPsInV1;\n"
        "void main(void) { gl_FragColor = qxpPsInV0; }\n";
    static const GLchar secondary_fragment_source[] =
        "#version 120\n"
        "centroid varying vec4 qxpPsInV0;\n"
        "centroid varying vec4 qxpPsInV1;\n"
        "void main(void) { gl_FragColor = qxpPsInV1; }\n";
    static const GLchar fog_fragment_source[] =
        "#version 120\n"
        "void main(void) { gl_FragColor = vec4(gl_FogFragCoord, gl_FogFragCoord, gl_FogFragCoord, 1.0); }\n";
    const GLchar *fragment_source;
    const GLchar *source_pointer;
    GLint status;
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;

    fragment_source = output_mode == 1 ? primary_fragment_source :
                      (output_mode == 2 ? secondary_fragment_source :
                       (output_mode == 3 ? fog_fragment_source :
                                           coverage_fragment_source));
    status = GL_FALSE;
    vertex_shader = api->create_shader(GL_VERTEX_SHADER);
    fragment_shader = api->create_shader(GL_FRAGMENT_SHADER);
    if (!vertex_shader || !fragment_shader) {
        fprintf(stderr, "vs20 glCreateShader returned zero\n");
        if (vertex_shader) api->delete_shader(vertex_shader);
        if (fragment_shader) api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = vertex_source;
    api->shader_source(vertex_shader, 1, &source_pointer, NULL);
    api->compile_shader(vertex_shader);
    api->get_shader_iv(vertex_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "vs20 vertex compilation failed\n");
        qxp_print_shader_log(api, vertex_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = fragment_source;
    api->shader_source(fragment_shader, 1, &source_pointer, NULL);
    api->compile_shader(fragment_shader);
    api->get_shader_iv(fragment_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "vs20 bridge fragment compilation failed\n");
        qxp_print_shader_log(api, fragment_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    program = api->create_program();
    if (!program) {
        fprintf(stderr, "vs20 glCreateProgram returned zero\n");
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    api->attach_shader(program, vertex_shader);
    api->attach_shader(program, fragment_shader);
    api->bind_attrib_location(program, 0, "qxpVsInput0");
    api->bind_attrib_location(program, 1, "qxpVsInput1");
    api->link_program(program);
    api->get_program_iv(program, GL_LINK_STATUS, &status);
    api->delete_shader(vertex_shader);
    api->delete_shader(fragment_shader);
    if (status != GL_TRUE) {
        fprintf(stderr, "vs20 GLSL program link failed\n");
        qxp_print_program_log(api, program);
        api->delete_program(program);
        return 0;
    }
    return program;
}

static GLuint qxp_build_sm20_pair(const QXP_GLSL_API *api,
                                   const GLchar *vertex_source,
                                   const GLchar *fragment_source)
{
    const GLchar *source_pointer;
    GLint status;
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;

    status = GL_FALSE;
    vertex_shader = api->create_shader(GL_VERTEX_SHADER);
    fragment_shader = api->create_shader(GL_FRAGMENT_SHADER);
    if (!vertex_shader || !fragment_shader) {
        fprintf(stderr, "sm20 pair glCreateShader returned zero\n");
        if (vertex_shader) api->delete_shader(vertex_shader);
        if (fragment_shader) api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = vertex_source;
    api->shader_source(vertex_shader, 1, &source_pointer, NULL);
    api->compile_shader(vertex_shader);
    api->get_shader_iv(vertex_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "sm20 pair vertex compilation failed\n");
        qxp_print_shader_log(api, vertex_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    source_pointer = fragment_source;
    api->shader_source(fragment_shader, 1, &source_pointer, NULL);
    api->compile_shader(fragment_shader);
    api->get_shader_iv(fragment_shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        fprintf(stderr, "sm20 pair fragment compilation failed\n");
        qxp_print_shader_log(api, fragment_shader);
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    program = api->create_program();
    if (!program) {
        api->delete_shader(vertex_shader);
        api->delete_shader(fragment_shader);
        return 0;
    }
    api->attach_shader(program, vertex_shader);
    api->attach_shader(program, fragment_shader);
    api->bind_attrib_location(program, 0, "qxpVsInput0");
    api->bind_attrib_location(program, 1, "qxpVsInput1");
    api->link_program(program);
    api->get_program_iv(program, GL_LINK_STATUS, &status);
    api->delete_shader(vertex_shader);
    api->delete_shader(fragment_shader);
    if (status != GL_TRUE) {
        fprintf(stderr, "generated SM2 pair link failed\n");
        qxp_print_program_log(api, program);
        api->delete_program(program);
        return 0;
    }
    return program;
}

static int qxp_run_vs20_pixels(const QXP_GLSL_API *api, GLuint program)
{
    static const GLfloat positions[12] = {
        -0.8f, 0.2f, 0.25f, 1.0f,
         0.8f, 0.2f, 0.25f, 1.0f,
         0.0f, 0.9f, 0.25f, 1.0f
    };
    static const GLfloat point_position[4] = {
        0.0f, 0.0f, 0.25f, 1.0f
    };
    static const GLfloat colors[12] = {
        0.2f, 0.3f, 0.4f, 1.0f,
        0.2f, 0.3f, 0.4f, 1.0f,
        0.2f, 0.3f, 0.4f, 1.0f
    };
    static const GLint int_constants[8] = {
        2, 0, 0, 0, 2, 0, 1, 0
    };
    static const GLfloat clip_positive[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    static const GLfloat clip_negative[4] = {
        -1.0f, 0.0f, 0.0f, 0.0f
    };
    GLubyte upper[4];
    GLubyte lower[4];
    GLfloat depth;
    GLfloat float_constants[256 * 4];
    GLint constant_location;
    GLint int_location;
    GLint bool_location;
    GLint clip_location;
    GLenum error;
    int passed;

    passed = 0;
    while (glGetError() != GL_NO_ERROR) { }
    constant_location = api->get_uniform_location(
        program, "qxpVsFloatConst[0]");
    int_location = api->get_uniform_location(program, "qxpVsIntConst[0]");
    bool_location = api->get_uniform_location(program, "qxpVsBoolConst[0]");
    clip_location = api->get_uniform_location(program, "qxpVsClipControl");
    if (constant_location < 0 || int_location < 0 || bool_location < 0 ||
        clip_location < 0) {
        fprintf(stderr, "vs20 flow/constant/clip uniform was optimized\n");
        return 0;
    }
    memset(float_constants, 0, sizeof(float_constants));
    float_constants[0] = 0.05f;
    float_constants[1] = 0.10f;
    float_constants[2] = 0.15f;
    float_constants[3] = 1.0f;
    float_constants[4] = 0.01f;
    float_constants[5] = 0.02f;
    float_constants[6] = 0.03f;
    float_constants[8] = 0.02f;
    float_constants[9] = 0.03f;
    float_constants[10] = 0.04f;
    float_constants[12] = 0.03f;
    float_constants[13] = 0.04f;
    float_constants[14] = 0.05f;
    float_constants[16] = 2.0f;
    float_constants[17] = 2.0f;
    float_constants[18] = 2.0f;
    float_constants[19] = 1.0f;
    float_constants[20] = 0.10f;
    float_constants[21] = 0.10f;
    float_constants[22] = 0.10f;
    float_constants[28] = 5.0f;
    float_constants[40] = -255.0f;
    api->use_program(program);
    api->uniform_4fv(constant_location, 256, float_constants);
    api->uniform_4iv(int_location, 2, int_constants);
    api->uniform_1i(bool_location, 1);
    api->uniform_4fv(clip_location, 1, clip_positive);
    api->vertex_attrib_pointer_arb(0, 4, GL_FLOAT, GL_FALSE, 0, positions);
    api->vertex_attrib_pointer_arb(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
    api->enable_vertex_attrib_array_arb(0);
    api->enable_vertex_attrib_array_arb(1);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glViewport(0, 0, 32, 32);
    glClearDepth(1.0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    memset(upper, 0, sizeof(upper));
    memset(lower, 0, sizeof(lower));
    depth = 1.0f;
    glReadPixels(16, 23, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, upper);
    glReadPixels(16, 9, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lower);
    glReadPixels(16, 23, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    if (!qxp_pixel_near(upper, 87, 133, 179) ||
        !qxp_pixel_near(lower, 0, 0, 0) ||
        depth < 0.23f || depth > 0.27f) {
        fprintf(stderr,
                "vs20 clip/depth mismatch: upper %u %u %u %u, "
                "lower %u %u %u %u, depth %.6f\n",
                upper[0], upper[1], upper[2], upper[3],
                lower[0], lower[1], lower[2], lower[3], depth);
        goto cleanup;
    }

    api->uniform_4fv(clip_location, 1, clip_negative);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    memset(upper, 0, sizeof(upper));
    memset(lower, 0, sizeof(lower));
    glReadPixels(16, 23, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, upper);
    glReadPixels(16, 9, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lower);
    if (!qxp_pixel_near(upper, 0, 0, 0) ||
        !qxp_pixel_near(lower, 87, 133, 179)) {
        fprintf(stderr,
                "vs20 Y-control mismatch: upper %u %u %u %u, "
                "lower %u %u %u %u\n",
                upper[0], upper[1], upper[2], upper[3],
                lower[0], lower[1], lower[2], lower[3]);
        goto cleanup;
    }
    {
        static const GLdouble clip_plane[4] = {
            0.0, 1.0, 0.0, 0.0
        };

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glClipPlane(GL_CLIP_PLANE0, clip_plane);
        glEnable(GL_CLIP_PLANE0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        memset(lower, 0, sizeof(lower));
        glReadPixels(16, 9, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lower);
        glDisable(GL_CLIP_PLANE0);
        if (!qxp_pixel_near(lower, 87, 133, 179)) {
            fprintf(stderr,
                    "vs20 original-clip-space mismatch: %u %u %u %u\n",
                    lower[0], lower[1], lower[2], lower[3]);
            goto cleanup;
        }
    }
    api->uniform_4fv(clip_location, 1, clip_positive);
    api->vertex_attrib_pointer_arb(
        0, 4, GL_FLOAT, GL_FALSE, 0, point_position);
    api->vertex_attrib_pointer_arb(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 1);
    glFinish();
    memset(upper, 0, sizeof(upper));
    memset(lower, 0, sizeof(lower));
    glReadPixels(17, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, upper);
    glReadPixels(21, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lower);
    glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
    if (!qxp_pixel_near(upper, 87, 133, 179) ||
        !qxp_pixel_near(lower, 0, 0, 0)) {
        fprintf(stderr,
                "vs20 point-size footprint mismatch: inner %u %u %u %u, "
                "outer %u %u %u %u\n",
                upper[0], upper[1], upper[2], upper[3],
                lower[0], lower[1], lower[2], lower[3]);
        goto cleanup;
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "vs20 pixel GL error: 0x%04x\n",
                (unsigned int)error);
        goto cleanup;
    }
    passed = 1;

cleanup:
    glDisable(GL_DEPTH_TEST);
    api->disable_vertex_attrib_array_arb(0);
    api->disable_vertex_attrib_array_arb(1);
    api->use_program(0);
    return passed;
}

static int qxp_run_vs20_fog_pixels(const QXP_GLSL_API *api,
                                    GLuint program, int writes_fog)
{
    static const GLfloat positions[24] = {
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f,  1.0f, 0.25f, 1.0f
    };
    static const GLfloat fog_inputs[24] = {
        0.25f, 0.0f, 0.0f, 1.0f, 0.25f, 0.0f, 0.0f, 1.0f,
        0.25f, 0.0f, 0.0f, 1.0f, 0.25f, 0.0f, 0.0f, 1.0f,
        0.25f, 0.0f, 0.0f, 1.0f, 0.25f, 0.0f, 0.0f, 1.0f
    };
    static const GLfloat vertex_fog_control[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    static const GLfloat table_fog_control[4] = {
        1.0f, 1.0f, 0.0f, 0.0f
    };
    GLubyte pixel[4];
    GLint clip_location;
    int passed;

    passed = 0;
    clip_location = api->get_uniform_location(program, "qxpVsClipControl");
    if (clip_location < 0) {
        fprintf(stderr, "vs20 fog clip uniform was optimized\n");
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) { }
    api->use_program(program);
    api->uniform_4fv(clip_location, 1, vertex_fog_control);
    api->vertex_attrib_pointer_arb(0, 4, GL_FLOAT, GL_FALSE, 0, positions);
    api->vertex_attrib_pointer_arb(1, 4, GL_FLOAT, GL_FALSE, 0, fog_inputs);
    api->enable_vertex_attrib_array_arb(0);
    api->enable_vertex_attrib_array_arb(1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, writes_fog ? 191 : 64,
                        writes_fog ? 191 : 64,
                        writes_fog ? 191 : 64)) {
        fprintf(stderr, "vs20 %s fog mismatch: %u %u %u %u\n",
                writes_fog ? "vertex" : "fallback",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }
    if (writes_fog) {
        api->uniform_4fv(clip_location, 1, table_fog_control);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        memset(pixel, 0, sizeof(pixel));
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        if (!qxp_pixel_near(pixel, 64, 64, 64)) {
            fprintf(stderr,
                    "vs20 table fog mismatch: %u %u %u %u\n",
                    pixel[0], pixel[1], pixel[2], pixel[3]);
            goto cleanup;
        }
    }
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "vs20 fog GL error\n");
        goto cleanup;
    }
    passed = 1;

cleanup:
    api->disable_vertex_attrib_array_arb(0);
    api->disable_vertex_attrib_array_arb(1);
    api->use_program(0);
    return passed;
}

static int qxp_run_vs20_special_pixels(const QXP_GLSL_API *api,
                                        GLuint program, int secondary)
{
    static const GLfloat positions[24] = {
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f,  1.0f, 0.25f, 1.0f
    };
    static const GLfloat clip_control[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    GLfloat colors[24];
    GLfloat constants[8];
    union {
        DWORD bits;
        GLfloat value;
    } qnan;
    GLubyte pixel[4];
    GLint constant_location;
    GLint clip_location;
    GLenum error;
    size_t index;
    int passed;

    passed = 0;
    qnan.bits = 0x7fc00000UL;
    for (index = 0; index < 6; ++index) {
        colors[index * 4 + 0] = 0.0f;
        colors[index * 4 + 1] = 0.0f;
        colors[index * 4 + 2] = 0.0f;
        colors[index * 4 + 3] = qnan.value;
    }
    memset(constants, 0, sizeof(constants));
    constants[1] = 1.5f;
    constants[2] = 3.0f;
    constants[4] = 0.1f;
    constants[5] = 0.1f;
    constants[6] = 0.1f;
    constants[7] = 0.1f;
    constant_location = api->get_uniform_location(
        program, "qxpVsFloatConst[0]");
    clip_location = api->get_uniform_location(program, "qxpVsClipControl");
    if (constant_location < 0 || clip_location < 0) {
        fprintf(stderr, "vs20 special uniforms were optimized\n");
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) { }
    api->use_program(program);
    api->uniform_4fv(constant_location, 2, constants);
    api->uniform_4fv(clip_location, 1, clip_control);
    api->vertex_attrib_pointer_arb(0, 4, GL_FLOAT, GL_FALSE, 0, positions);
    api->vertex_attrib_pointer_arb(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
    api->enable_vertex_attrib_array_arb(0);
    api->enable_vertex_attrib_array_arb(1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if ((!secondary &&
         (pixel[0] < 253 || pixel[1] < 253 || pixel[2] > 2 ||
          pixel[3] > 2)) ||
        (secondary &&
         (pixel[0] < 70 || pixel[0] > 74 ||
          pixel[1] < 38 || pixel[1] > 42 ||
          pixel[2] < 70 || pixel[2] > 74 ||
          pixel[3] < 70 || pixel[3] > 74))) {
        fprintf(stderr,
                "vs20 special %s mismatch: %u %u %u %u\n",
                secondary ? "EXPP/LOGP" : "zero/NaN",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "vs20 special GL error: 0x%04x\n",
                (unsigned int)error);
        goto cleanup;
    }
    passed = 1;

cleanup:
    api->disable_vertex_attrib_array_arb(0);
    api->disable_vertex_attrib_array_arb(1);
    api->use_program(0);
    return passed;
}

static int qxp_run_vs20_arithmetic_pixels(const QXP_GLSL_API *api,
                                           GLuint program, int secondary)
{
    static const GLfloat positions[24] = {
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f, -1.0f, 0.25f, 1.0f,
         1.0f,  1.0f, 0.25f, 1.0f,
        -1.0f,  1.0f, 0.25f, 1.0f
    };
    static const GLfloat clip_control[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    GLfloat constants[28 * 4];
    GLubyte pixel[4];
    GLint constant_location;
    GLint clip_location;
    int expected[4];
    int passed;

    passed = 0;
    memset(constants, 0, sizeof(constants));
    constants[0 * 4 + 0] = 0.2f;
    constants[0 * 4 + 1] = 0.3f;
    constants[0 * 4 + 2] = 0.4f;
    constants[0 * 4 + 3] = 0.5f;
    constants[3 * 4 + 0] = 0.2f;
    constants[3 * 4 + 1] = 0.2f;
    constants[3 * 4 + 2] = 0.2f;
    constants[5 * 4 + 0] = 0.3f;
    constants[6 * 4 + 0] = 0.1f;
    constants[7 * 4 + 0] = 0.25f;
    constants[8 * 4 + 0] = 0.2f;
    constants[8 * 4 + 1] = 0.2f;
    constants[8 * 4 + 2] = 0.2f;
    constants[8 * 4 + 3] = 0.2f;
    constants[11 * 4 + 1] = 1.0f;
    constants[13 * 4 + 1] = 0.5f;
    constants[13 * 4 + 3] = 2.0f;
    constants[15 * 4 + 0] = 3.0f;
    constants[15 * 4 + 2] = 4.0f;
    constants[15 * 4 + 3] = 1.0f;
    constants[16 * 4 + 0] = 2.0f;
    constants[17 * 4 + 2] = 1.0f;
    constants[18 * 4 + 0] = 1.0f;
    constants[19 * 4 + 0] = 1.0471975512f;
    constants[20 * 4 + 0] = 0.5f;
    constants[20 * 4 + 1] = 0.5f;
    constants[20 * 4 + 3] = 0.5f;
    constants[21 * 4 + 0] = 1.0f;
    constants[22 * 4 + 0] = 2.0f;
    constants[23 * 4 + 0] = 0.1f;
    constants[23 * 4 + 1] = 0.2f;
    constants[23 * 4 + 2] = 0.3f;
    constants[23 * 4 + 3] = 0.4f;
    constants[24 * 4 + 0] = 1.0f;
    constants[25 * 4 + 1] = 1.0f;
    constants[26 * 4 + 2] = 1.0f;
    constants[27 * 4 + 3] = 1.0f;

    constant_location = api->get_uniform_location(
        program, "qxpVsFloatConst[0]");
    clip_location = api->get_uniform_location(program, "qxpVsClipControl");
    if (constant_location < 0 || clip_location < 0) {
        fprintf(stderr, "vs20 arithmetic uniforms were optimized\n");
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) { }
    api->use_program(program);
    api->uniform_4fv(constant_location, 28, constants);
    api->uniform_4fv(clip_location, 1, clip_control);
    api->vertex_attrib_pointer_arb(0, 4, GL_FLOAT, GL_FALSE, 0, positions);
    api->enable_vertex_attrib_array_arb(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (secondary) {
        expected[0] = 92;
        expected[1] = 128;
        expected[2] = 128;
        expected[3] = 115;
    } else {
        expected[0] = 161;
        expected[1] = 128;
        expected[2] = 41;
        expected[3] = 16;
    }
    if (pixel[0] < expected[0] - 2 || pixel[0] > expected[0] + 2 ||
        pixel[1] < expected[1] - 2 || pixel[1] > expected[1] + 2 ||
        pixel[2] < expected[2] - 2 || pixel[2] > expected[2] + 2 ||
        pixel[3] < expected[3] - 2 || pixel[3] > expected[3] + 2) {
        fprintf(stderr,
                "vs20 arithmetic %s mismatch: %u %u %u %u\n",
                secondary ? "geometric/matrix" : "scalar/vector",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "vs20 arithmetic GL error\n");
        goto cleanup;
    }
    passed = 1;

cleanup:
    api->disable_vertex_attrib_array_arb(0);
    api->use_program(0);
    return passed;
}

static int qxp_run_ps20_pixels(const QXP_GLSL_API *api, GLuint program)
{
    static const GLfloat constants[4] = {
        0.25f, 0.5f, 0.75f, 1.0f
    };
    static const GLfloat kill_constants[4] = {
        -0.25f, 0.5f, 0.75f, 1.0f
    };
    GLubyte level0[4 * 4 * 4];
    GLubyte level1[2 * 2 * 4];
    GLubyte level2[4];
    GLubyte pixel[4];
    GLint location;
    GLint sampler_location;
    GLenum error;
    GLuint texture;
    size_t texel_index;
    int passed;

    texture = 0;
    passed = 0;
    while (glGetError() != GL_NO_ERROR) { }
    api->use_program(program);
    location = api->get_uniform_location(program, "qxpPsConstants[31]");
    if (location < 0) {
        fprintf(stderr, "ps20 constant c31 was optimized or missing\n");
        goto cleanup;
    }
    api->uniform_4fv(location, 1, constants);
    sampler_location = api->get_uniform_location(program, "qxpPsS0");
    if (sampler_location < 0) {
        fprintf(stderr, "ps20 live 2D sampler was optimized or missing\n");
        goto cleanup;
    }
    api->uniform_1i(sampler_location, 0);
    sampler_location = api->get_uniform_location(program, "qxpPsS1");
    if (sampler_location >= 0) api->uniform_1i(sampler_location, 1);
    sampler_location = api->get_uniform_location(program, "qxpPsS2");
    if (sampler_location >= 0) api->uniform_1i(sampler_location, 2);
    memset(level0, 0, sizeof(level0));
    memset(level1, 0, sizeof(level1));
    memset(level2, 0, sizeof(level2));
    for (texel_index = 3; texel_index < sizeof(level0);
         texel_index += 4) level0[texel_index] = 255;
    for (texel_index = 3; texel_index < sizeof(level1);
         texel_index += 4) level1[texel_index] = 255;
    level2[3] = 255;
    level0[(3 * 4 + 3) * 4 + 0] = 255;
    level0[(1 * 4 + 1) * 4 + 1] = 255;
    level2[2] = 255;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, level0);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, level1);
    glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, level2);
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(-0.25f, 0.2f, 0.3f, 1.0f);
    glTexCoord4f(-3.2f, -3.2f, 0.75f, 2.0f);
    glVertex4f(-1.0f, -1.0f, 0.0f, 1.0f);
    glTexCoord4f( 4.8f, -3.2f, 0.75f, 2.0f);
    glVertex4f( 1.0f, -1.0f, 0.0f, 1.0f);
    glTexCoord4f( 4.8f,  4.8f, 0.75f, 2.0f);
    glVertex4f( 1.0f,  1.0f, 0.0f, 1.0f);
    glTexCoord4f(-3.2f,  4.8f, 0.75f, 2.0f);
    glVertex4f(-1.0f,  1.0f, 0.0f, 1.0f);
    glEnd();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    /* DP2ADD yields .5625; declared v0.x clamps -.25 to zero and
     * omitted channels supply defaults. Plain (.8), projected (.4),
     * and +2-biased samples select red, green, and blue respectively. */
    if (!qxp_pixel_near(pixel, 143, 143, 255)) {
        fprintf(stderr, "ps20 semantic pixel mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }

    api->uniform_4fv(location, 1, kill_constants);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glColor4f(-0.25f, 0.2f, 0.3f, 1.0f);
    glTexCoord4f(-3.2f, -3.2f, 0.75f, 2.0f);
    glVertex4f(-1.0f, -1.0f, 0.0f, 1.0f);
    glTexCoord4f( 4.8f, -3.2f, 0.75f, 2.0f);
    glVertex4f( 1.0f, -1.0f, 0.0f, 1.0f);
    glTexCoord4f( 4.8f,  4.8f, 0.75f, 2.0f);
    glVertex4f( 1.0f,  1.0f, 0.0f, 1.0f);
    glTexCoord4f(-3.2f,  4.8f, 0.75f, 2.0f);
    glVertex4f(-1.0f,  1.0f, 0.0f, 1.0f);
    glEnd();
    memset(pixel, 0xff, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 0, 0)) {
        fprintf(stderr, "ps20 TEXKILL pixel mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "ps20 pixel GL error: 0x%04x\n",
                (unsigned int)error);
        goto cleanup;
    }
    passed = 1;

cleanup:
    glBindTexture(GL_TEXTURE_2D, 0);
    if (texture) glDeleteTextures(1, &texture);
    api->use_program(0);
    return passed;
}

static GLchar *qxp_read_shader(const char *path, size_t *source_bytes,
                               long maximum_bytes)
{
    FILE *file;
    GLchar *source;
    long length;

    if (!path || !source_bytes) return NULL;
    *source_bytes = 0;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || length > maximum_bytes ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    source = (GLchar *)malloc((size_t)length + 1);
    if (!source) {
        fclose(file);
        return NULL;
    }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        free(source);
        fclose(file);
        return NULL;
    }
    fclose(file);
    source[length] = 0;
    *source_bytes = (size_t)length;
    return source;
}

static int qxp_compile_arbvp(const QXP_GLSL_API *api,
                             const GLchar *source, size_t source_bytes)
{
    const GLubyte *error_string;
    GLenum error;
    GLint error_position = -1;
    GLint instructions = 0;
    GLint maximum_instructions = 0;
    GLint maximum_temporaries = 0;
    GLint native = GL_FALSE;
    GLint temporaries = 0;
    GLuint program = 0;

    while (glGetError() != GL_NO_ERROR) { }
    api->gen_programs_arb(1, &program);
    api->bind_program_arb(GL_VERTEX_PROGRAM_ARB, program);
    api->program_string_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_PROGRAM_FORMAT_ASCII_ARB,
                            (GLsizei)source_bytes, source);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &error_position);
        error_string = glGetString(GL_PROGRAM_ERROR_STRING_ARB);
        fprintf(stderr,
                "ARB vertex program definition failed: GL 0x%04x at %ld: %s\n",
                (unsigned int)error, (long)error_position,
                error_string ? (const char *)error_string : "unknown error");
        api->bind_program_arb(GL_VERTEX_PROGRAM_ARB, 0);
        api->delete_programs_arb(1, &program);
        return 0;
    }
    api->get_program_iv_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_PROGRAM_INSTRUCTIONS_ARB, &instructions);
    api->get_program_iv_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_PROGRAM_TEMPORARIES_ARB, &temporaries);
    api->get_program_iv_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_MAX_PROGRAM_INSTRUCTIONS_ARB,
                            &maximum_instructions);
    api->get_program_iv_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_MAX_PROGRAM_TEMPORARIES_ARB,
                            &maximum_temporaries);
    api->get_program_iv_arb(GL_VERTEX_PROGRAM_ARB,
                            GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &native);
    printf("ARBvp source=%lu bytes instructions=%ld/%ld "
           "temporaries=%ld/%ld native=%ld\n",
           (unsigned long)source_bytes, (long)instructions,
           (long)maximum_instructions, (long)temporaries,
           (long)maximum_temporaries, (long)native);
    api->bind_program_arb(GL_VERTEX_PROGRAM_ARB, 0);
    api->delete_programs_arb(1, &program);
    return native == GL_TRUE;
}

static int qxp_check_production_uniforms(const QXP_GLSL_API *api,
                                         GLuint program)
{
    static const char *names[] = {
        "qxpLightCount", "qxpRange0", "qxpRange1", "qxpProjected",
        "qxpLiteral", "qxpSource", "qxpSecondary", "qxpNormalize",
        "qxpMaterialSpecular", "qxpFog", "qxpTexgen"
    };
    size_t index;

    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (api->get_uniform_location(program, names[index]) < 0) {
            fprintf(stderr, "missing production uniform %s\n", names[index]);
            return 0;
        }
    }
    return 1;
}

static int qxp_pixel_near(const GLubyte *pixel, int red, int green, int blue);

static int qxp_run_clip_pixels(const QXP_GLSL_API *api, GLuint program)
{
    static const GLfloat positions[24] = {
        -1.0f, -1.0f, 0.5f, 1.0f,
         1.0f, -1.0f, 0.5f, 1.0f,
         1.0f,  1.0f, 0.5f, 1.0f,
        -1.0f, -1.0f, 0.5f, 1.0f,
         1.0f,  1.0f, 0.5f, 1.0f,
        -1.0f,  1.0f, 0.5f, 1.0f
    };
    static const GLfloat colors[24] = {
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f
    };
    static const GLfloat control[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const GLdouble plane[4] = {1.0, 0.0, 0.0, 0.0};
    GLubyte left[4];
    GLubyte right[4];
    GLint control_location;
    unsigned int plane_index;

    control_location = api->get_uniform_location(program, "qxpControl");
    if (control_location < 0) return 0;
    api->use_program(program);
    api->uniform_4fv(control_location, 1, control);
    api->vertex_attrib_pointer_arb(0, 4, GL_FLOAT, GL_FALSE, 0, positions);
    api->vertex_attrib_pointer_arb(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
    api->enable_vertex_attrib_array_arb(0);
    api->enable_vertex_attrib_array_arb(1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glViewport(0, 0, 32, 32);
    for (plane_index = 0; plane_index < 6; ++plane_index) {
        glClipPlane(GL_CLIP_PLANE0 + plane_index, plane);
        glEnable(GL_CLIP_PLANE0 + plane_index);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        glReadPixels(8, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
        glReadPixels(24, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
        glDisable(GL_CLIP_PLANE0 + plane_index);
        if (!qxp_pixel_near(left, 0, 0, 0) ||
            !qxp_pixel_near(right, 255, 0, 0)) {
            fprintf(stderr,
                    "clip GLSL plane %u pixel mismatch: "
                    "left %u %u %u %u, right %u %u %u %u\n",
                    plane_index,
                    left[0], left[1], left[2], left[3],
                    right[0], right[1], right[2], right[3]);
            api->disable_vertex_attrib_array_arb(0);
            api->disable_vertex_attrib_array_arb(1);
            api->use_program(0);
            return 0;
        }
    }
    api->disable_vertex_attrib_array_arb(0);
    api->disable_vertex_attrib_array_arb(1);
    api->use_program(0);
    return 1;
}

static void qxp_draw_triangle(void)
{
    glBegin(GL_TRIANGLES);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(1.0f, -1.0f);
    glTexCoord2f(0.5f, 1.0f);
    glVertex2f(0.0f, 1.0f);
    glEnd();
}

static void qxp_draw_flat_triangle(GLfloat center_x, GLfloat point_x,
                                   GLfloat point_y, int face_point)
{
    GLfloat x;
    GLfloat y;

    glBegin(GL_TRIANGLES);
    x = center_x - 0.12f;
    y = -0.12f;
    glNormal3f(face_point ? point_x - x : 0.0f,
               face_point ? point_y - y : 0.0f,
               face_point ? 0.5f : 1.0f);
    glVertex3f(x, y, 0.25f);
    x = center_x + 0.12f;
    glNormal3f(face_point ? point_x - x : 0.0f,
               face_point ? point_y - y : 0.0f,
               face_point ? 0.5f : 1.0f);
    glVertex3f(x, y, 0.25f);
    x = center_x;
    y = 0.12f;
    glNormal3f(face_point ? point_x - x : 0.0f,
               face_point ? point_y - y : 0.0f,
               face_point ? 0.5f : 1.0f);
    glVertex3f(x, y, 0.25f);
    glEnd();
}

static void qxp_draw_specular_triangle(GLfloat center_x, GLfloat center_y)
{
    glBegin(GL_TRIANGLES);
    glNormal3f(0.02996257f, 0.03995009f, 0.99875234f);
    glVertex3f(center_x - 0.015f, center_y - 0.02f, 0.25f);
    glNormal3f(0.0f, -0.04993762f, 0.99875234f);
    glVertex3f(center_x, center_y + 0.025f, 0.25f);
    glNormal3f(-0.02996257f, 0.03995009f, 0.99875234f);
    glVertex3f(center_x + 0.015f, center_y - 0.02f, 0.25f);
    glEnd();
}

static void qxp_draw_spot_triangle(GLfloat center_x)
{
    glBegin(GL_TRIANGLES);
    glNormal3f(0.0f, -0.66143782f, 0.75f);
    glVertex3f(center_x, 0.08819171f, 0.25f);
    glNormal3f(0.57282195f, 0.33071895f, 0.75f);
    glVertex3f(center_x - 0.07637626f, -0.04409586f, 0.25f);
    glNormal3f(-0.57282195f, 0.33071895f, 0.75f);
    glVertex3f(center_x + 0.07637626f, -0.04409586f, 0.25f);
    glEnd();
}

static int qxp_pixel_near(const GLubyte *pixel, int red, int green, int blue)
{
    const int tolerance = 2;

    return pixel[0] >= red - tolerance && pixel[0] <= red + tolerance &&
           pixel[1] >= green - tolerance && pixel[1] <= green + tolerance &&
           pixel[2] >= blue - tolerance && pixel[2] <= blue + tolerance &&
           pixel[3] >= 253;
}

static int qxp_set_uniform4(const QXP_GLSL_API *api, GLuint program,
                            const char *name, const GLfloat values[4])
{
    GLint location = api->get_uniform_location(program, name);

    if (location < 0) return 0;
    api->uniform_4fv(location, 1, values);
    return 1;
}

static int qxp_run_production_texgen_pixels(
    const QXP_GLSL_API *api, GLuint program)
{
    static const GLchar texcoord_fragment_program[] =
        "!!ARBfp1.0\n"
        "MOV result.color, fragment.texcoord[0];\n"
        "END\n";
    static const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const GLfloat passthrough[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat normal_mode[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat position_mode[4] = {2.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat reflection_mode[4] = {3.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat normalize_reflection[4] = {
        1.0f, 0.0f, 0.0f, 0.0f
    };
    static const GLfloat normal_present[4] = {
        0.0f, 0.0f, 1.0f, 0.0f
    };
    static const GLfloat point_attenuation[3] = {1.0f, 0.0f, 0.0f};
    GLubyte pixel[4];
    GLint light_count;
    GLuint fragment_program = 0;
    int passed = 0;

    api->use_program(program);
    light_count = api->get_uniform_location(program, "qxpLightCount");
    if (light_count < 0) goto cleanup;
    api->uniform_1i(light_count, 0);
    if (!qxp_set_uniform4(api, program, "qxpRange0", zero) ||
        !qxp_set_uniform4(api, program, "qxpRange1", zero) ||
        !qxp_set_uniform4(api, program, "qxpProjected", zero) ||
        !qxp_set_uniform4(api, program, "qxpLiteral", one) ||
        !qxp_set_uniform4(api, program, "qxpSource", zero) ||
        !qxp_set_uniform4(api, program, "qxpSecondary", zero) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", zero) ||
        !qxp_set_uniform4(api, program, "qxpMaterialSpecular", zero) ||
        !qxp_set_uniform4(api, program, "qxpFog", zero) ||
        !qxp_set_uniform4(api, program, "qxpTexgen", passthrough))
        goto cleanup;

    api->gen_programs_arb(1, &fragment_program);
    api->bind_program_arb(GL_FRAGMENT_PROGRAM_ARB, fragment_program);
    api->program_string_arb(
        GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB,
        (GLsizei)(sizeof(texcoord_fragment_program) - 1),
        texcoord_fragment_program);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "production texgen fragment definition failed\n");
        goto cleanup;
    }
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE_ARB);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    glViewport(0, 0, 32, 32);
    glPointSize(8.0f);
    api->point_parameter_fv(
        GL_POINT_DISTANCE_ATTENUATION, point_attenuation);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glTexCoord3f(0.125f, 0.375f, 0.875f);
    glVertex3f(0.0f, 0.0f, 0.5f);
    glEnd();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 32, 96, 223)) {
        fprintf(stderr, "production texgen passthrough mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }

    if (!qxp_set_uniform4(api, program, "qxpFog", normal_present) ||
        !qxp_set_uniform4(api, program, "qxpTexgen", normal_mode))
        goto cleanup;
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glTexCoord3f(0.75f, 0.125f, 0.25f);
    glNormal3f(0.125f, 0.375f, 0.875f);
    glVertex3f(0.0f, 0.0f, 0.5f);
    glEnd();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 32, 96, 223)) {
        fprintf(stderr, "production texgen normal mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }

    if (!qxp_set_uniform4(api, program, "qxpFog", zero) ||
        !qxp_set_uniform4(api, program, "qxpTexgen", position_mode))
        goto cleanup;
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glVertex3f(0.25f, 0.375f, 0.75f);
    glEnd();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(20, 22, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 64, 96, 191)) {
        fprintf(stderr, "production texgen position mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }

    if (!qxp_set_uniform4(
            api, program, "qxpNormalize", normalize_reflection) ||
        !qxp_set_uniform4(api, program, "qxpFog", normal_present) ||
        !qxp_set_uniform4(api, program, "qxpTexgen", reflection_mode))
        goto cleanup;
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glTranslatef(0.5f, 0.5f, 0.5f);
    glScalef(0.5f, 0.5f, 0.5f);
    glMatrixMode(GL_MODELVIEW);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glVertex3f(0.0f, 0.0f, 0.5f);
    glEnd();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 128, 128, 0)) {
        fprintf(stderr, "production texgen reflection mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        goto cleanup;
    }
    passed = 1;

cleanup:
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_VERTEX_PROGRAM_POINT_SIZE_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    api->bind_program_arb(GL_FRAGMENT_PROGRAM_ARB, 0);
    if (fragment_program)
        api->delete_programs_arb(1, &fragment_program);
    api->use_program(0);
    return passed;
}

static int qxp_run_production_pixels(const QXP_GLSL_API *api, GLuint program)
{
    static const GLdouble clip_equation[4] = {1.0, 0.0, 0.0, 0.0};
    static const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat point_attenuation[3] = {4.0f, 0.0f, 0.0f};
    static const GLfloat point_source[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    static const GLfloat black_alpha_one[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const GLfloat range0[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const GLfloat range1[4] = {200.0f, 0.0f, 0.0f, 0.0f};
    static const GLfloat mixed_range0[4] = {0.0f, 0.6f, 0.0f, 0.0f};
    static const GLfloat normalize[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const GLfloat normal_fog[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    static const GLfloat missing_normal_fog[4] =
        {0.0f, 0.0f, 0.0f, 1.0f};
    static const GLfloat specular_range0[4] = {1.0f, 1.0f, 200.0f, 0.0f};
    static const GLfloat specular_control[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    static const GLfloat material_specular[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    static const GLfloat row_range[4] = {0.52f, 0.52f, 0.52f, 0.52f};
    static const GLfloat row_control[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const GLfloat row_material[4] = {1.0f, 1.0f, 1.0f, 8.0f};
    static const GLfloat spot_range[4] = {0.2f, 0.2f, 0.0f, 0.0f};
    static const GLfloat spot_direction[4] = {0.0f, 0.0f, -1.0f, 0.0f};
    static const GLfloat fog_color[4] = {
        32.0f / 255.0f, 64.0f / 255.0f, 96.0f / 255.0f, 1.0f
    };
    static const GLubyte point_texels[16] = {
        255, 0, 255, 255, 255, 255, 255, 255,
        255, 255, 0, 255, 0, 255, 255, 255
    };
    static const GLubyte row_masks[7] = {1, 2, 4, 6, 5, 3, 1};
    static const GLubyte row_values[7] = {5, 4, 2, 2, 2, 4, 5};
    GLubyte pixel[4];
    GLfloat position[4];
    GLfloat diffuse[4];
    GLfloat specular[4];
    GLfloat light_ambient[4];
    GLint location;
    GLuint point_texture = 0;
    unsigned int index;
    int row_ok = 1;

    api->use_program(program);
    location = api->get_uniform_location(program, "qxpLightCount");
    if (location < 0) return 0;
    api->uniform_1i(location, 5);
    if (!qxp_set_uniform4(api, program, "qxpRange0", range0) ||
        !qxp_set_uniform4(api, program, "qxpRange1", range1) ||
        !qxp_set_uniform4(api, program, "qxpProjected", zero) ||
        !qxp_set_uniform4(api, program, "qxpLiteral", one) ||
        !qxp_set_uniform4(api, program, "qxpSource", zero) ||
        !qxp_set_uniform4(api, program, "qxpSecondary", zero) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", normalize) ||
        !qxp_set_uniform4(api, program, "qxpMaterialSpecular",
                          material_specular) ||
        !qxp_set_uniform4(api, program, "qxpFog", normal_fog) ||
        !qxp_set_uniform4(api, program, "qxpTexgen", zero)) {
        api->use_program(0);
        return 0;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
    for (index = 0; index < 5; ++index) {
        position[0] = index < 4 ? 100.0f : 0.0f;
        position[1] = 0.0f;
        position[2] = index < 4 ? 1.0f : 100.0f;
        position[3] = 1.0f;
        diffuse[0] = 1.0f;
        diffuse[1] = index < 4 ? 1.0f : 0.0f;
        diffuse[2] = index < 4 ? 1.0f : 0.0f;
        diffuse[3] = 1.0f;
        glLightfv(GL_LIGHT0 + index, GL_POSITION, position);
        glLightfv(GL_LIGHT0 + index, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0 + index, GL_DIFFUSE, diffuse);
        glLightf(GL_LIGHT0 + index, GL_CONSTANT_ATTENUATION, 1.0f);
        glLightf(GL_LIGHT0 + index, GL_LINEAR_ATTENUATION, 0.0f);
        glLightf(GL_LIGHT0 + index, GL_QUADRATIC_ATTENUATION, 0.0f);
    }
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, 32, 32);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    api->use_program(0);
    if (!qxp_pixel_near(pixel, 255, 0, 0)) {
        fprintf(stderr, "production five-light mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        return 0;
    }

    /* Compatibility GL's current normal defaults to +Z when no normal array
     * is present. D3D instead supplies zero, so prove the explicit declaration
     * branch suppresses the same otherwise-red diffuse contribution without
     * evaluating even a singular inverse-transpose normal matrix. */
    api->use_program(program);
    if (!qxp_set_uniform4(api, program, "qxpFog", missing_normal_fog)) {
        api->use_program(0);
        return 0;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 0, 0)) {
        fprintf(stderr, "production missing-normal mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        api->use_program(0);
        return 0;
    }
    glPushMatrix();
    glScalef(1.0f, 1.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_triangle();
    glPopMatrix();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 0, 0)) {
        fprintf(stderr, "production singular missing-normal mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        api->use_program(0);
        return 0;
    }
    if (!qxp_set_uniform4(api, program, "qxpFog", normal_fog)) {
        api->use_program(0);
        return 0;
    }
    api->use_program(0);

    /* A red directional light must remain distance-invariant while the green
     * point light affects only the left primitive inside its hard D3D Range.
     * Fixed OpenGL ignores Range and incorrectly turns both samples yellow. */
    api->use_program(program);
    api->uniform_1i(location, 2);
    if (!qxp_set_uniform4(api, program, "qxpRange0", mixed_range0) ||
        !qxp_set_uniform4(api, program, "qxpRange1", zero) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", normalize)) {
        api->use_program(0);
        return 0;
    }
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
    position[0] = 0.0f;
    position[1] = 0.0f;
    position[2] = 1.0f;
    position[3] = 0.0f;
    light_ambient[0] = 1.0f;
    light_ambient[1] = light_ambient[2] = 0.0f;
    light_ambient[3] = 1.0f;
    memcpy(diffuse, zero, sizeof(diffuse));
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, zero);
    /* Pixel (8,16) is centered at (-0.46875,0.03125) in this 32-pixel
     * viewport. Put the point directly above that sample for an exact unit
     * diffuse term rather than accepting raster-position latitude. */
    position[0] = -0.46875f;
    position[1] = 0.03125f;
    position[2] = 0.75f;
    position[3] = 1.0f;
    diffuse[0] = 0.0f;
    diffuse[1] = 1.0f;
    diffuse[3] = 1.0f;
    glLightfv(GL_LIGHT1, GL_POSITION, position);
    glLightfv(GL_LIGHT1, GL_AMBIENT, zero);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT1, GL_SPECULAR, zero);
    glLightf(GL_LIGHT1, GL_CONSTANT_ATTENUATION, 1.0f);
    glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.0f);
    glLightf(GL_LIGHT1, GL_QUADRATIC_ATTENUATION, 0.0f);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_LIGHTING);
    glViewport(0, 0, 32, 32);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_flat_triangle(-0.5f, position[0], position[1], 1);
    qxp_draw_flat_triangle(0.5f, position[0], position[1], 0);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(8, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 255, 255, 0)) {
        fprintf(stderr, "production mixed directional/point left mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(24, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 255, 0, 0)) {
        fprintf(stderr, "production mixed directional/point right mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    glDisable(GL_LIGHT0);
    glDisable(GL_LIGHT1);
    glDisable(GL_LIGHTING);
    api->use_program(0);
    if (!row_ok) return 0;

    api->use_program(program);
    api->uniform_1i(location, 3);
    if (!qxp_set_uniform4(api, program, "qxpRange0", specular_range0) ||
        !qxp_set_uniform4(api, program, "qxpRange1", zero) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", specular_control)) {
        api->use_program(0);
        return 0;
    }
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, black_alpha_one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, one);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0.0f);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
    glEnable(GL_COLOR_SUM);
    glEnable(GL_LIGHTING);
    for (index = 0; index < 3; ++index) {
        position[0] = index < 2 ? 100.0f : 0.0f;
        position[1] = 0.0f;
        position[2] = index < 2 ? 1.0f : 100.0f;
        position[3] = 1.0f;
        specular[0] = index == 2 ? 1.0f : 0.0f;
        specular[1] = specular[2] = 0.0f;
        specular[3] = 1.0f;
        glLightfv(GL_LIGHT0 + index, GL_POSITION, position);
        glLightfv(GL_LIGHT0 + index, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0 + index, GL_DIFFUSE, zero);
        glLightfv(GL_LIGHT0 + index, GL_SPECULAR, specular);
        glEnable(GL_LIGHT0 + index);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    for (index = 0; index < 3; ++index) glDisable(GL_LIGHT0 + index);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_SUM);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
    api->use_program(0);
    if (!qxp_pixel_near(pixel, 255, 0, 0)) {
        fprintf(stderr, "production three-light specular mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        return 0;
    }

    api->use_program(program);
    api->uniform_1i(location, 7);
    if (!qxp_set_uniform4(api, program, "qxpRange0", row_range) ||
        !qxp_set_uniform4(api, program, "qxpRange1", row_range) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", row_control) ||
        !qxp_set_uniform4(api, program, "qxpMaterialSpecular",
                          row_material)) {
        api->use_program(0);
        return 0;
    }
    glViewport(0, 0, 640, 480);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    for (index = 0; index < 7; ++index) {
        position[0] = ((50.0f + 90.0f * index) * 2.0f / 640.0f) - 1.0f;
        position[1] = 1.0f - (355.0f * 2.0f / 480.0f);
        position[2] = 0.75f;
        position[3] = 1.0f;
        specular[0] = (row_masks[index] & 1) != 0;
        specular[1] = (row_masks[index] & 2) != 0;
        specular[2] = (row_masks[index] & 4) != 0;
        specular[3] = 1.0f;
        glLightfv(GL_LIGHT0 + index, GL_POSITION, position);
        glLightfv(GL_LIGHT0 + index, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0 + index, GL_DIFFUSE, zero);
        glLightfv(GL_LIGHT0 + index, GL_SPECULAR, specular);
        glLightf(GL_LIGHT0 + index, GL_CONSTANT_ATTENUATION, 1.0f);
        glLightf(GL_LIGHT0 + index, GL_LINEAR_ATTENUATION, 0.0f);
        glLightf(GL_LIGHT0 + index, GL_QUADRATIC_ATTENUATION, 0.0f);
        glEnable(GL_LIGHT0 + index);
    }
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_SUM);
    glClear(GL_COLOR_BUFFER_BIT);
    for (index = 0; index < 7; ++index) {
        qxp_draw_specular_triangle(
            ((50.0f + 90.0f * index) * 2.0f / 640.0f) - 1.0f,
            1.0f - (355.0f * 2.0f / 480.0f));
    }
    glFinish();
    for (index = 0; index < 7; ++index) {
        int expected_red = row_masks[index] & 1 ? row_values[index] : 0;
        int expected_green = row_masks[index] & 2 ? row_values[index] : 0;
        int expected_blue = row_masks[index] & 4 ? row_values[index] : 0;

        memset(pixel, 0, sizeof(pixel));
        glReadPixels(50 + 90 * index, 124, 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixel);
        if (abs((int)pixel[0] - expected_red) >
                (expected_red ? 1 : 0) ||
            abs((int)pixel[1] - expected_green) >
                (expected_green ? 1 : 0) ||
            abs((int)pixel[2] - expected_blue) >
                (expected_blue ? 1 : 0) || pixel[3] < 253) {
            fprintf(stderr, "production seven-light local specular %u "
                            "mismatch: %u %u %u %u expected %u\n",
                    index, pixel[0], pixel[1], pixel[2], pixel[3],
                    row_values[index]);
            row_ok = 0;
        }
    }
    for (index = 0; index < 7; ++index) glDisable(GL_LIGHT0 + index);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_SUM);
    api->use_program(0);
    if (!row_ok) return 0;

    api->use_program(program);
    api->uniform_1i(location, 2);
    if (!qxp_set_uniform4(api, program, "qxpRange0", spot_range) ||
        !qxp_set_uniform4(api, program, "qxpRange1", zero) ||
        !qxp_set_uniform4(api, program, "qxpNormalize", normalize)) {
        api->use_program(0);
        return 0;
    }
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
    for (index = 0; index < 2; ++index) {
        position[0] = index ? 0.5f : -0.5f;
        position[1] = 0.0f;
        position[2] = 0.35f;
        position[3] = 1.0f;
        memset(diffuse, 0, sizeof(diffuse));
        diffuse[index] = 1.0f;
        memset(light_ambient, 0, sizeof(light_ambient));
        light_ambient[3] = 1.0f + index;
        glLightfv(GL_LIGHT0 + index, GL_POSITION, position);
        glLightfv(GL_LIGHT0 + index, GL_AMBIENT, light_ambient);
        glLightfv(GL_LIGHT0 + index, GL_DIFFUSE, diffuse);
        glLightfv(GL_LIGHT0 + index, GL_SPECULAR, zero);
        glLightfv(GL_LIGHT0 + index, GL_SPOT_DIRECTION, spot_direction);
        glLightf(GL_LIGHT0 + index, GL_SPOT_CUTOFF, 60.0f);
        glLightf(GL_LIGHT0 + index, GL_SPOT_EXPONENT, 0.0f);
        glLightf(GL_LIGHT0 + index, GL_CONSTANT_ATTENUATION, 1.0f);
        glLightf(GL_LIGHT0 + index, GL_LINEAR_ATTENUATION, 0.0f);
        glLightf(GL_LIGHT0 + index, GL_QUADRATIC_ATTENUATION, 0.0f);
        glEnable(GL_LIGHT0 + index);
    }
    glEnable(GL_LIGHTING);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_spot_triangle(-0.5f);
    qxp_draw_spot_triangle(0.5f);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(160, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 128, 0, 0) || pixel[3] < 253) {
        fprintf(stderr, "production two-spot linear mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(480, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 64, 0) || pixel[3] < 253) {
        fprintf(stderr, "production two-spot squared mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
    glFogf(GL_FOG_START, 0.0f);
    glFogf(GL_FOG_END, 0.5f);
    glFogfv(GL_FOG_COLOR, fog_color);
    glEnable(GL_FOG);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_spot_triangle(-0.5f);
    qxp_draw_spot_triangle(0.5f);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(160, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 80, 32, 48) || pixel[3] < 253) {
        fprintf(stderr, "production scalable vertex-fog mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    glDisable(GL_FOG);
    /* Match the miniport's D3D world-plane rule: define x>=0 under view only,
     * then apply a translated world matrix. The left primitive must disappear
     * while the right keeps the exact nonlinear spotlight result. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClipPlane(GL_CLIP_PLANE0, clip_equation);
    glEnable(GL_CLIP_PLANE0);
    glTranslatef(0.5f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    qxp_draw_spot_triangle(-1.0f);
    qxp_draw_spot_triangle(0.0f);
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(160, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 0, 0) || pixel[3] < 253) {
        fprintf(stderr, "production scalable clip reject mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(480, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 64, 0) || pixel[3] < 253) {
        fprintf(stderr, "production scalable clip keep mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    glDisable(GL_CLIP_PLANE0);
    glLoadIdentity();
    for (index = 0; index < 2; ++index) glDisable(GL_LIGHT0 + index);
    glDisable(GL_LIGHTING);
    /* A=4 must attenuate the 16-pixel fixed point size to exactly 8 pixels
     * when programmable point size is enabled. Two asymmetric textured
     * quadrants additionally prove top-left point-sprite replacement and a
     * half-distance fog mix survive the vertex-only production program. */
    api->uniform_1i(location, 0);
    if (!qxp_set_uniform4(api, program, "qxpSource", point_source)) {
        api->use_program(0);
        return 0;
    }
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, one);
    glColor4ub(255, 255, 255, 255);
    glViewport(0, 0, 32, 32);
    glPointSize(16.0f);
    api->point_parameter_f(GL_POINT_SIZE_MIN, 1.0f);
    api->point_parameter_f(GL_POINT_SIZE_MAX, 64.0f);
    api->point_parameter_fv(GL_POINT_DISTANCE_ATTENUATION,
                            point_attenuation);
    api->point_parameter_i(GL_POINT_SPRITE_COORD_ORIGIN, GL_UPPER_LEFT);
    glGenTextures(1, &point_texture);
    glBindTexture(GL_TEXTURE_2D, point_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, point_texels);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_POINT_SPRITE);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
    glFogf(GL_FOG_START, 0.0f);
    glFogf(GL_FOG_END, 0.5f);
    glFogfv(GL_FOG_COLOR, fog_color);
    glEnable(GL_FOG);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE_ARB);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glTexCoord2f(0.75f, 0.25f);
    glVertex3f(0.0f, 0.0f, 0.25f);
    glEnd();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(13, 18, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 144, 32, 176)) {
        unsigned int scan_x;

        fprintf(stderr, "production scalable point sprite upper mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        fprintf(stderr, "production scalable point covered x:");
        for (scan_x = 0; scan_x < 32; ++scan_x) {
            glReadPixels((GLint)scan_x, 16, 1, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixel);
            if (pixel[0] || pixel[1] || pixel[2])
                fprintf(stderr, " %u", scan_x);
        }
        fprintf(stderr, "\n");
        row_ok = 0;
    }
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(18, 13, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 16, 160, 176)) {
        fprintf(stderr, "production scalable point sprite lower mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(22, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 0, 0, 0)) {
        fprintf(stderr, "production scalable point outer mismatch: "
                        "%u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        row_ok = 0;
    }
    glDisable(GL_VERTEX_PROGRAM_POINT_SIZE_ARB);
    glDisable(GL_POINT_SPRITE);
    glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_FALSE);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &point_texture);
    glDisable(GL_FOG);
    glPointSize(1.0f);
    api->point_parameter_fv(GL_POINT_DISTANCE_ATTENUATION, one);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
    api->use_program(0);
    if (!row_ok) return 0;
    return 1;
}

static int qxp_run_pixels(const QXP_GLSL_API *api, GLuint program)
{
    static const GLchar fragment_program[] =
        "!!ARBfp1.0\n"
        "MOV result.color, fragment.texcoord[1];\n"
        "END\n";
    const GLubyte green_texel[4] = {0, 255, 0, 255};
    GLubyte pixel[4];
    GLuint arb_program = 0;
    GLuint texture = 0;

    api->use_program(program);
    glViewport(0, 0, 32, 32);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4ub(255, 0, 0, 255);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (!qxp_pixel_near(pixel, 255, 0, 0)) {
        fprintf(stderr, "fixed color mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        api->use_program(0);
        return 0;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, green_texel);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_TEXTURE_2D);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4ub(255, 255, 255, 255);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &texture);
    api->use_program(0);
    if (!qxp_pixel_near(pixel, 0, 255, 0)) {
        fprintf(stderr, "fixed texture mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        return 0;
    }

    api->gen_programs_arb(1, &arb_program);
    api->bind_program_arb(GL_FRAGMENT_PROGRAM_ARB, arb_program);
    api->program_string_arb(GL_FRAGMENT_PROGRAM_ARB,
                            GL_PROGRAM_FORMAT_ASCII_ARB,
                            (GLsizei)(sizeof(fragment_program) - 1),
                            fragment_program);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "ARB fragment program definition failed\n");
        api->delete_programs_arb(1, &arb_program);
        api->use_program(0);
        return 0;
    }
    /* Keep the vertex-only GLSL executable active while the production D3D
     * pixel path supplies an ARB fragment program. Texcoord one is authored
     * only by smoke_source, so its nonzero value proves both stages execute. */
    api->use_program(program);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4ub(0, 0, 255, 255);
    qxp_draw_triangle();
    glFinish();
    memset(pixel, 0, sizeof(pixel));
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    api->bind_program_arb(GL_FRAGMENT_PROGRAM_ARB, 0);
    api->delete_programs_arb(1, &arb_program);
    api->use_program(0);
    if (!qxp_pixel_near(pixel, 32, 96, 223)) {
        fprintf(stderr, "ARB fragment bridge mismatch: %u %u %u %u\n",
                pixel[0], pixel[1], pixel[2], pixel[3]);
        return 0;
    }
    return 1;
}

static int qxp_run_surface_copy(const QXP_GLSL_API *api)
{
    static const GLubyte source_pixels[4 * 4 * 3] = {
        0, 0, 255,  0, 0, 255,  255, 255, 0,  255, 255, 0,
        0, 0, 255,  0, 0, 255,  255, 255, 0,  255, 255, 0,
        255, 0, 0,  255, 0, 0,  0, 255, 0,  0, 255, 0,
        255, 0, 0,  255, 0, 0,  0, 255, 0,  0, 255, 0
    };
    static const GLubyte poison_pixels[2 * 2 * 4] = {
        255, 0, 255, 0, 255, 0, 255, 0,
        255, 0, 255, 0, 255, 0, 255, 0
    };
    static const int expected[4][3] = {
        {255, 0, 0}, {0, 255, 0},
        {0, 0, 255}, {255, 255, 0}
    };
    GLubyte pixels[2 * 2 * 4];
    GLuint textures[2] = {0, 0};
    GLuint framebuffers[2] = {0, 0};
    GLint binding = 0;
    GLenum error;
    int index;
    int passed = 0;

    while (glGetError() != GL_NO_ERROR) { }
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 4, 4, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, source_pixels);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, poison_pixels);
    api->gen_framebuffers(2, framebuffers);
    api->bind_framebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    api->framebuffer_texture_2d(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, textures[0], 0);
    if (api->check_framebuffer_status(GL_READ_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "surface-copy read framebuffer incomplete\n");
        goto cleanup;
    }
    api->bind_framebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
    api->framebuffer_texture_2d(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, textures[1], 0);
    if (api->check_framebuffer_status(GL_DRAW_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "surface-copy draw framebuffer incomplete\n");
        goto cleanup;
    }
    api->blit_framebuffer(0, 4, 4, 0, 0, 0, 2, 2,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    api->bind_framebuffer(GL_FRAMEBUFFER, framebuffers[1]);
    memset(pixels, 0, sizeof(pixels));
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (index = 0; index < 4; ++index) {
        if (!qxp_pixel_near(
                &pixels[index * 4], expected[index][0],
                expected[index][1], expected[index][2]) ||
            pixels[index * 4 + 3] != 255) {
            fprintf(stderr,
                    "surface-copy pixel %d mismatch: %u %u %u %u\n",
                    index, pixels[index * 4], pixels[index * 4 + 1],
                    pixels[index * 4 + 2], pixels[index * 4 + 3]);
            goto cleanup;
        }
    }
    api->bind_framebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
    if ((GLuint)binding != framebuffers[0]) {
        fprintf(stderr, "surface-copy framebuffer restore mismatch\n");
        goto cleanup;
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "surface-copy GL error: 0x%04x\n",
                (unsigned int)error);
        goto cleanup;
    }
    passed = 1;

cleanup:
    api->bind_framebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffers[0] || framebuffers[1])
        api->delete_framebuffers(2, framebuffers);
    if (textures[0] || textures[1]) glDeleteTextures(2, textures);
    return passed;
}

/* Forces every layout-affecting pixel-pack parameter to its canonical value,
 * exactly as the miniport's ABI-51 readback sequence does. */
static void qxp_reset_pack_state(void)
{
    glPixelStorei(GL_PACK_SWAP_BYTES, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

/* Mirrors the miniport's ABI-51 surface readback exactly: the texture is
 * attached to a framebuffer as the READ target, every pixel-pack parameter
 * is forced to its canonical value, and the full image is read back with the
 * same format/type pair the upload path uses. A byte-for-byte roundtrip
 * against the uploaded D3D-ordered image proves channel order and that row
 * zero of the readback is the texture's top row, i.e. the exact layout
 * DirectDraw Lock hands to the game. Pack state is poisoned first so the
 * sequence's sanitation is what makes the result correct, and both the
 * 32-bit BGRA and 16-bit R5G6B5 paths are exercised. */
static int qxp_run_surface_readback(const QXP_GLSL_API *api)
{
    static const GLubyte source_pixels[4 * 2 * 4] = {
        10, 20, 30, 40,  50, 60, 70, 80,
        90, 100, 110, 120,  130, 140, 150, 160,
        170, 180, 190, 200,  210, 220, 230, 240,
        11, 22, 33, 44,  55, 66, 77, 88
    };
    static const GLushort source_565[2 * 2] = {
        0xf800, 0x07e0,
        0x001f, 0xffff
    };
    GLubyte pixels[4 * 2 * 4];
    GLushort pixels_565[2 * 2];
    GLuint textures[2] = {0, 0};
    GLuint framebuffer = 0;
    GLint binding = 0;
    GLenum error;
    int passed = 0;

    while (glGetError() != GL_NO_ERROR) { }
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 2, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, source_pixels);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, source_565);
    api->gen_framebuffers(1, &framebuffer);

    /* Poison every pack parameter the miniport sequence must sanitize. */
    glPixelStorei(GL_PACK_SWAP_BYTES, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 7);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 2);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);

    api->bind_framebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    api->framebuffer_texture_2d(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, textures[0], 0);
    if (api->check_framebuffer_status(GL_READ_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "surface-readback framebuffer incomplete\n");
        goto cleanup;
    }
    qxp_reset_pack_state();
    memset(pixels, 0, sizeof(pixels));
    glReadPixels(0, 0, 4, 2, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    if (memcmp(pixels, source_pixels, sizeof(source_pixels)) != 0) {
        fprintf(stderr,
                "surface-readback roundtrip mismatch: "
                "row0 %u %u %u %u row1 %u %u %u %u\n",
                pixels[0], pixels[1], pixels[2], pixels[3],
                pixels[16], pixels[17], pixels[18], pixels[19]);
        goto cleanup;
    }

    api->framebuffer_texture_2d(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, textures[1], 0);
    if (api->check_framebuffer_status(GL_READ_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "surface-readback 565 framebuffer incomplete\n");
        goto cleanup;
    }
    qxp_reset_pack_state();
    memset(pixels_565, 0, sizeof(pixels_565));
    glReadPixels(0, 0, 2, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels_565);
    if (memcmp(pixels_565, source_565, sizeof(source_565)) != 0) {
        fprintf(stderr,
                "surface-readback 565 roundtrip mismatch: "
                "%04x %04x %04x %04x\n",
                pixels_565[0], pixels_565[1],
                pixels_565[2], pixels_565[3]);
        goto cleanup;
    }

    api->bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
    if ((GLuint)binding != framebuffer) {
        fprintf(stderr, "surface-readback framebuffer restore mismatch\n");
        goto cleanup;
    }
    error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "surface-readback GL error: 0x%04x\n",
                (unsigned int)error);
        goto cleanup;
    }
    passed = 1;

cleanup:
    api->bind_framebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffer) api->delete_framebuffers(1, &framebuffer);
    if (textures[0] || textures[1]) glDeleteTextures(2, textures);
    return passed;
}

int main(int argc, char **argv)
{
    static const GLchar smoke_source[] =
        "#version 110\n"
        "void main(void)\n"
        "{\n"
        "    gl_Position = gl_Vertex;\n"
        "    gl_FrontColor = gl_Color;\n"
        "    gl_BackColor = gl_Color;\n"
        "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
        "    gl_TexCoord[1] = vec4(0.125, 0.375, 0.875, 1.0);\n"
        "}\n";
    static const char class_name[] = "QxpGlslCompatSmoke";
    PIXELFORMATDESCRIPTOR pfd;
    QXP_GLSL_API api;
    WNDCLASSA window_class;
    HGLRC context = NULL;
    HINSTANCE instance = GetModuleHandleA(NULL);
    GLuint program = 0;
    GLuint production_program = 0;
    GLchar *production_source = NULL;
    GLchar *arbvp_source = NULL;
    size_t production_bytes = 0;
    HWND window = NULL;
    HDC dc = NULL;
    int pixel_format;
    int passed = 0;
    const char *pass_message =
        "PASS vertex-only GLSL preserves fixed and ARB fragment stages";

    memset(&window_class, 0, sizeof(window_class));
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = qxp_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClassA(&window_class)) {
        fprintf(stderr, "RegisterClass failed: %lu\n", GetLastError());
        goto cleanup;
    }
    window = CreateWindowExA(0, class_name, class_name, WS_POPUP,
                             0, 0, 640, 480, NULL, NULL, instance, NULL);
    if (!window) {
        fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError());
        goto cleanup;
    }
    dc = GetDC(window);
    if (!dc) {
        fprintf(stderr, "GetDC failed\n");
        goto cleanup;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pixel_format = ChoosePixelFormat(dc, &pfd);
    if (!pixel_format || !SetPixelFormat(dc, pixel_format, &pfd)) {
        fprintf(stderr, "pixel format setup failed: %lu\n", GetLastError());
        goto cleanup;
    }
    context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) {
        fprintf(stderr, "OpenGL context setup failed: %lu\n", GetLastError());
        goto cleanup;
    }
    printf("GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
    if (!qxp_load_glsl(&api)) goto cleanup;
    program = qxp_build_vertex_only_program(&api, smoke_source);
    if (!program) goto cleanup;
    if (!qxp_run_pixels(&api, program)) goto cleanup;
    if (argc == 4 && strcmp(argv[1], "--sm20-pair") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read bounded paired VS20 source\n");
            goto cleanup;
        }
        arbvp_source = qxp_read_shader(
            argv[3], &production_bytes, QXP_MAX_PS20_GLSL_SOURCE_BYTES);
        if (!arbvp_source) {
            fprintf(stderr, "could not read bounded paired PS20 source\n");
            goto cleanup;
        }
        production_program = qxp_build_sm20_pair(
            &api, production_source, arbvp_source);
        if (!production_program) goto cleanup;
        pass_message = "PASS generated VS20/PS20 pair compile and link";
    } else if (argc == 4 && strcmp(argv[1], "--vs20-fog") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read generated VS20 fog source\n");
            goto cleanup;
        }
        arbvp_source = qxp_read_shader(
            argv[3], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!arbvp_source) {
            fprintf(stderr, "could not read generated VS20 no-fog source\n");
            goto cleanup;
        }
        production_program = qxp_build_vs20_program(
            &api, production_source, 3);
        if (!production_program ||
            !qxp_run_vs20_fog_pixels(
                &api, production_program, 1)) goto cleanup;
        api.delete_program(production_program);
        production_program = qxp_build_vs20_program(
            &api, arbvp_source, 3);
        if (!production_program ||
            !qxp_run_vs20_fog_pixels(
                &api, production_program, 0)) goto cleanup;
        pass_message =
            "PASS VS20 vertex, table, and fallback fog pixel oracles";
    } else if (argc == 2 && strcmp(argv[1], "--surface-copy") == 0) {
        if (!qxp_run_surface_copy(&api)) goto cleanup;
        pass_message =
            "PASS surface copy orientation, point scale, alpha, and restore";
    } else if (argc == 2 && strcmp(argv[1], "--surface-readback") == 0) {
        if (!qxp_run_surface_readback(&api)) goto cleanup;
        pass_message =
            "PASS surface readback channel order, row order, and restore";
    } else if (argc == 3 && strcmp(argv[1], "--ps20-glsl") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_PS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read bounded ps20 GLSL source\n");
            goto cleanup;
        }
        production_program = qxp_build_ps20_program(
            &api, production_source);
        if (!production_program ||
            !qxp_run_ps20_pixels(&api, production_program)) goto cleanup;
        printf("ps20 GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
        pass_message = "PASS ps_2_0 GLSL compile, link, and pixel smoke";
    } else if (argc == 3 && strcmp(argv[1], "--vs20-glsl") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read bounded vs20 GLSL source\n");
            goto cleanup;
        }
        production_program = qxp_build_vs20_program(
            &api, production_source, 0);
        if (!production_program ||
            !qxp_run_vs20_pixels(&api, production_program)) goto cleanup;
        printf("vs20 GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
        pass_message = "PASS vs_2_0 GLSL compile, link, and pixel smoke";
    } else if (argc == 3 && strcmp(argv[1], "--vs20-special") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read bounded special VS20 source\n");
            goto cleanup;
        }
        production_program = qxp_build_vs20_program(
            &api, production_source, 1);
        if (!production_program ||
            !qxp_run_vs20_special_pixels(
                &api, production_program, 0)) goto cleanup;
        api.delete_program(production_program);
        production_program = qxp_build_vs20_program(
            &api, production_source, 2);
        if (!production_program ||
            !qxp_run_vs20_special_pixels(
                &api, production_program, 1)) goto cleanup;
        printf("special vs20 GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
        pass_message =
            "PASS vs_2_0 zero, NaN, EXPP, and LOGP pixel oracles";
    } else if (argc == 3 && strcmp(argv[1], "--vs20-arithmetic") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_VS20_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read arithmetic VS20 source\n");
            goto cleanup;
        }
        production_program = qxp_build_vs20_program(
            &api, production_source, 1);
        if (!production_program ||
            !qxp_run_vs20_arithmetic_pixels(
                &api, production_program, 0)) goto cleanup;
        api.delete_program(production_program);
        production_program = qxp_build_vs20_program(
            &api, production_source, 2);
        if (!production_program ||
            !qxp_run_vs20_arithmetic_pixels(
                &api, production_program, 1)) goto cleanup;
        printf("arithmetic vs20 GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
        pass_message =
            "PASS vs_2_0 arithmetic, geometric, and matrix pixel oracles";
    } else if (argc == 3 && strcmp(argv[1], "--arbvp") == 0) {
        arbvp_source = qxp_read_shader(argv[2], &production_bytes,
                                       QXP_MAX_ARBVP_SOURCE_BYTES);
        if (!arbvp_source) {
            fprintf(stderr, "could not read translated ARBvp source\n");
            goto cleanup;
        }
        if (!qxp_compile_arbvp(&api, arbvp_source, production_bytes))
            goto cleanup;
    } else if (argc == 3 &&
               (strcmp(argv[1], "--clip-glsl") == 0 ||
                strcmp(argv[1], "--clip-glsl-visual") == 0)) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_CLIP_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read clip GLSL source\n");
            goto cleanup;
        }
        production_program = qxp_build_vertex_only_program(
            &api, production_source);
        if (!production_program ||
            api.get_uniform_location(production_program, "qxpControl") < 0) {
            fprintf(stderr, "clip GLSL uniforms were optimized or missing\n");
            goto cleanup;
        }
        if (strcmp(argv[1], "--clip-glsl") == 0 &&
            (api.get_uniform_location(
                 production_program, "qxpConstants[0]") < 0 ||
             api.get_uniform_location(
                 production_program, "qxpProjected") < 0)) {
            fprintf(stderr,
                    "clip GLSL constant/projection uniforms were optimized\n");
            goto cleanup;
        }
        if (strcmp(argv[1], "--clip-glsl-visual") == 0 &&
            !qxp_run_clip_pixels(&api, production_program)) goto cleanup;
        printf("clip GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
    } else if (argc == 3 && strcmp(argv[1], "--positiont-glsl") == 0) {
        production_source = qxp_read_shader(
            argv[2], &production_bytes, QXP_MAX_CLIP_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read positiont GLSL source\n");
            goto cleanup;
        }
        /* A synthesized pre-transformed stage carries no shader constants, so
         * the only uniforms that must survive linking are the viewport it
         * transforms through and the shared clip control it reorients with. */
        production_program = qxp_build_vertex_only_program(
            &api, production_source);
        if (!production_program ||
            api.get_uniform_location(
                production_program, "qxpVsPositiont") < 0 ||
            api.get_uniform_location(
                production_program, "qxpVsClipControl") < 0) {
            fprintf(stderr,
                    "positiont GLSL uniforms were optimized or missing\n");
            goto cleanup;
        }
        printf("positiont GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
        pass_message =
            "PASS synthesized pre-transformed GLSL compiles and links";
    } else if (argc == 2) {
        production_source = qxp_read_shader(
            argv[1], &production_bytes,
            QXP_MAX_INTERNAL_GLSL_SOURCE_BYTES);
        if (!production_source) {
            fprintf(stderr, "could not read bounded production shader\n");
            goto cleanup;
        }
        production_program = qxp_build_vertex_only_program(
            &api, production_source);
        if (!production_program ||
            !qxp_check_production_uniforms(&api, production_program) ||
            !qxp_run_production_texgen_pixels(&api, production_program) ||
            !qxp_run_production_pixels(&api, production_program))
            goto cleanup;
        printf("production GLSL source=%lu bytes\n",
               (unsigned long)production_bytes);
    } else if (argc != 1) {
        fprintf(stderr,
                "usage: qxp_glsl_compat_test [production.glsl | "
                "--ps20-glsl source | --vs20-glsl source | "
                "--vs20-special source | --vs20-arithmetic source | "
                "--vs20-fog fog nofog | "
                "--sm20-pair vertex fragment | --arbvp source | "
                "--clip-glsl source | --clip-glsl-visual source | "
                "--positiont-glsl source | "
                "--surface-copy | "
                "--surface-readback]\n");
        goto cleanup;
    }
    passed = 1;
    printf("%s\n", pass_message);

cleanup:
    if (production_program) api.delete_program(production_program);
    if (program) api.delete_program(program);
    free(production_source);
    free(arbvp_source);
    if (context) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context);
    }
    if (dc && window) ReleaseDC(window, dc);
    if (window) DestroyWindow(window);
    UnregisterClassA(class_name, instance);
    return passed ? 0 : 1;
}
