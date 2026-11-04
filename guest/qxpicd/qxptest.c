#include <winsock2.h>
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

typedef ULONG DHGLRC;
typedef BOOL (APIENTRY *validate_fn)(ULONG);
typedef LONG (APIENTRY *describe_fn)(HDC, INT, ULONG, PIXELFORMATDESCRIPTOR *);
typedef BOOL (APIENTRY *set_pixel_format_fn)(HDC, LONG);
typedef DHGLRC (APIENTRY *create_context_fn)(HDC);
typedef void *(APIENTRY *set_context_fn)(HDC, DHGLRC, void *);
typedef BOOL (APIENTRY *release_context_fn)(DHGLRC);
typedef BOOL (APIENTRY *delete_context_fn)(DHGLRC);
typedef BOOL (APIENTRY *swap_buffers_fn)(HDC);
typedef void (APIENTRY *clear_fn)(GLbitfield);
typedef void (APIENTRY *clear_color_fn)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *clip_plane_fn)(GLenum, const GLdouble *);
typedef void (APIENTRY *begin_fn)(GLenum);
typedef void (APIENTRY *bitmap_fn)(GLsizei, GLsizei, GLfloat, GLfloat,
                                   GLfloat, GLfloat, const GLubyte *);
typedef void (APIENTRY *bind_texture_fn)(GLenum, GLuint);
typedef void (APIENTRY *color3f_fn)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *color3fv_fn)(const GLfloat *);
typedef void (APIENTRY *delete_textures_fn)(GLsizei, const GLuint *);
typedef void (APIENTRY *draw_arrays_fn)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *draw_elements_fn)(GLenum, GLsizei, GLenum,
                                          const GLvoid *);
typedef void (APIENTRY *draw_pixels_fn)(GLsizei, GLsizei, GLenum, GLenum,
                                        const GLvoid *);
typedef void (APIENTRY *end_fn)(void);
typedef void (APIENTRY *enable_fn)(GLenum);
typedef void (APIENTRY *feedback_buffer_fn)(GLsizei, GLenum, GLfloat *);
typedef void (APIENTRY *select_buffer_fn)(GLsizei, GLuint *);
typedef GLint (APIENTRY *render_mode_fn)(GLenum);
typedef void (APIENTRY *pass_through_fn)(GLfloat);
typedef void (APIENTRY *client_state_fn)(GLenum);
typedef GLenum (APIENTRY *get_error_fn)(void);
typedef void (APIENTRY *get_booleanv_fn)(GLenum, GLboolean *);
typedef void (APIENTRY *get_clip_plane_fn)(GLenum, GLdouble *);
typedef void (APIENTRY *get_floatv_fn)(GLenum, GLfloat *);
typedef void (APIENTRY *get_integerv_fn)(GLenum, GLint *);
typedef void (APIENTRY *get_pointerv_fn)(GLenum, GLvoid **);
typedef void (APIENTRY *get_tex_image_fn)(GLenum, GLint, GLenum, GLenum,
                                          GLvoid *);
typedef const GLubyte *(APIENTRY *get_string_fn)(GLenum);
typedef void (APIENTRY *get_tex_level_parameteriv_fn)(GLenum, GLint, GLenum,
                                                       GLint *);
typedef void (APIENTRY *get_tex_parameteriv_fn)(GLenum, GLenum, GLint *);
typedef GLboolean (APIENTRY *is_enabled_fn)(GLenum);
typedef GLboolean (APIENTRY *is_texture_fn)(GLuint);
typedef GLboolean (APIENTRY *are_textures_resident_fn)(GLsizei,
                                                       const GLuint *,
                                                       GLboolean *);
typedef void (APIENTRY *load_identity_fn)(void);
typedef void (APIENTRY *load_matrixf_fn)(const GLfloat *);
typedef void (APIENTRY *matrix_mode_fn)(GLenum);
typedef void (APIENTRY *map1d_fn)(GLenum, GLdouble, GLdouble, GLint, GLint,
                                  const GLdouble *);
typedef void (APIENTRY *map1f_fn)(GLenum, GLfloat, GLfloat, GLint, GLint,
                                  const GLfloat *);
typedef void (APIENTRY *map2d_fn)(GLenum, GLdouble, GLdouble, GLint, GLint,
                                  GLdouble, GLdouble, GLint, GLint,
                                  const GLdouble *);
typedef void (APIENTRY *map2f_fn)(GLenum, GLfloat, GLfloat, GLint, GLint,
                                  GLfloat, GLfloat, GLint, GLint,
                                  const GLfloat *);
typedef void (APIENTRY *get_mapdv_fn)(GLenum, GLenum, GLdouble *);
typedef void (APIENTRY *get_mapfv_fn)(GLenum, GLenum, GLfloat *);
typedef void (APIENTRY *get_mapiv_fn)(GLenum, GLenum, GLint *);
typedef void (APIENTRY *ortho_fn)(GLdouble, GLdouble, GLdouble, GLdouble,
                                  GLdouble, GLdouble);
typedef void (APIENTRY *rotatef_fn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *raster_pos2i_fn)(GLint, GLint);
typedef void (APIENTRY *read_pixels_fn)(GLint, GLint, GLsizei, GLsizei,
                                        GLenum, GLenum, GLvoid *);
typedef void (APIENTRY *rectdv_fn)(const GLdouble *, const GLdouble *);
typedef void (APIENTRY *rectfv_fn)(const GLfloat *, const GLfloat *);
typedef void (APIENTRY *rectiv_fn)(const GLint *, const GLint *);
typedef void (APIENTRY *rectsv_fn)(const GLshort *, const GLshort *);
typedef void (APIENTRY *pixel_storei_fn)(GLenum, GLint);
typedef void (APIENTRY *pixel_storef_fn)(GLenum, GLfloat);
typedef void (APIENTRY *pixel_mapfv_fn)(GLenum, GLint, const GLfloat *);
typedef void (APIENTRY *pixel_mapuiv_fn)(GLenum, GLint, const GLuint *);
typedef void (APIENTRY *pixel_mapusv_fn)(GLenum, GLint, const GLushort *);
typedef void (APIENTRY *get_pixel_mapfv_fn)(GLenum, GLfloat *);
typedef void (APIENTRY *get_pixel_mapuiv_fn)(GLenum, GLuint *);
typedef void (APIENTRY *get_pixel_mapusv_fn)(GLenum, GLushort *);
typedef void (APIENTRY *polygon_stipple_fn)(const GLubyte *);
typedef void (APIENTRY *get_polygon_stipple_fn)(GLubyte *);
typedef void (APIENTRY *tex_coord2f_fn)(GLfloat, GLfloat);
typedef void (APIENTRY *tex_coord2fv_fn)(const GLfloat *);
typedef void (APIENTRY *tex_enviv_fn)(GLenum, GLenum, const GLint *);
typedef void (APIENTRY *get_tex_enviv_fn)(GLenum, GLenum, GLint *);
typedef void (APIENTRY *tex_image2d_fn)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                        GLint, GLenum, GLenum, const GLvoid *);
typedef void (APIENTRY *tex_parameteri_fn)(GLenum, GLenum, GLint);
typedef void (APIENTRY *prioritize_textures_fn)(GLsizei, const GLuint *,
                                                const GLclampf *);
typedef void (APIENTRY *color_pointer_fn)(GLint, GLenum, GLsizei,
                                          const GLvoid *);
typedef void (APIENTRY *texcoord_pointer_fn)(GLint, GLenum, GLsizei,
                                             const GLvoid *);
typedef void (APIENTRY *vertex_pointer_fn)(GLint, GLenum, GLsizei,
                                           const GLvoid *);
typedef void (APIENTRY *normal_pointer_fn)(GLenum, GLsizei, const GLvoid *);
typedef void (APIENTRY *interleaved_arrays_fn)(GLenum, GLsizei,
                                               const GLvoid *);
typedef void (APIENTRY *gen_textures_fn)(GLsizei, GLuint *);
typedef void (APIENTRY *vertex3f_fn)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *vertex3fv_fn)(const GLfloat *);
typedef void (APIENTRY *viewport_fn)(GLint, GLint, GLsizei, GLsizei);

typedef struct qxp_proc_table {
    int cEntries;
    PROC entries[336];
} QXP_PROC_TABLE;

static void report_result(int rc)
{
    WSADATA winsock;
    struct sockaddr_in address;
    SOCKET socket_handle;
    char message[64];
    int length;
    unsigned port;

    length = _snprintf(message, sizeof(message),
                       "QXPICD_RESULT rc=%d", rc);
    if (length <= 0) return;
    if (length >= (int)sizeof(message)) length = sizeof(message) - 1;
    qxp_report_serial(message);
    if (WSAStartup(MAKEWORD(2, 0), &winsock)) return;
    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle != INVALID_SOCKET) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("10.0.2.2");
        for (port = 44574; port <= 44590; ++port) {
            address.sin_port = htons((u_short)port);
            sendto(socket_handle, message, length, 0,
                   (const struct sockaddr *)&address, sizeof(address));
        }
        closesocket(socket_handle);
    }
    WSACleanup();
}

int main(int argc, char **argv)
{
    HMODULE module = LoadLibraryA("qxpicd.dll");
    validate_fn validate;
    describe_fn describe;
    set_pixel_format_fn set_pixel_format;
    create_context_fn create_context;
    set_context_fn set_context;
    release_context_fn release_context;
    delete_context_fn delete_context;
    swap_buffers_fn drv_swap_buffers;
    QXP_PROC_TABLE *table;
    clear_fn clear;
    clear_color_fn clear_color;
    clip_plane_fn clip_plane;
    begin_fn begin;
    bitmap_fn bitmap;
    bind_texture_fn bind_texture;
    color3f_fn color3f;
    color3fv_fn color3fv;
    delete_textures_fn delete_textures = NULL;
    draw_arrays_fn draw_arrays;
    draw_elements_fn draw_elements;
    draw_pixels_fn draw_pixels;
    end_fn end;
    enable_fn disable;
    enable_fn enable;
    feedback_buffer_fn feedback_buffer;
    select_buffer_fn select_buffer;
    render_mode_fn render_mode;
    pass_through_fn pass_through;
    client_state_fn disable_client_state;
    client_state_fn enable_client_state;
    get_error_fn get_error;
    get_booleanv_fn get_booleanv;
    get_clip_plane_fn get_clip_plane;
    get_floatv_fn get_floatv;
    get_integerv_fn get_integerv;
    get_pointerv_fn get_pointerv;
    get_tex_image_fn get_tex_image;
    get_string_fn get_string;
    get_tex_level_parameteriv_fn get_tex_level_parameteriv;
    get_tex_parameteriv_fn get_tex_parameteriv;
    is_enabled_fn is_enabled;
    is_texture_fn is_texture;
    are_textures_resident_fn are_textures_resident;
    load_identity_fn load_identity;
    load_matrixf_fn load_matrixf;
    matrix_mode_fn matrix_mode;
    map1d_fn map1d;
    map1f_fn map1f;
    map2d_fn map2d;
    map2f_fn map2f;
    get_mapdv_fn get_mapdv;
    get_mapfv_fn get_mapfv;
    get_mapiv_fn get_mapiv;
    ortho_fn ortho;
    rotatef_fn rotatef;
    raster_pos2i_fn raster_pos2i;
    read_pixels_fn read_pixels;
    rectdv_fn rectdv;
    rectfv_fn rectfv;
    rectiv_fn rectiv;
    rectsv_fn rectsv;
    pixel_storei_fn pixel_storei;
    pixel_storef_fn pixel_storef;
    pixel_mapfv_fn pixel_mapfv;
    pixel_mapuiv_fn pixel_mapuiv;
    pixel_mapusv_fn pixel_mapusv;
    get_pixel_mapfv_fn get_pixel_mapfv;
    get_pixel_mapuiv_fn get_pixel_mapuiv;
    get_pixel_mapusv_fn get_pixel_mapusv;
    polygon_stipple_fn polygon_stipple;
    get_polygon_stipple_fn get_polygon_stipple;
    tex_coord2f_fn tex_coord2f;
    tex_coord2fv_fn tex_coord2fv;
    tex_enviv_fn tex_enviv;
    get_tex_enviv_fn get_tex_enviv;
    tex_image2d_fn tex_image2d;
    tex_parameteri_fn tex_parameteri;
    prioritize_textures_fn prioritize_textures;
    color_pointer_fn color_pointer;
    texcoord_pointer_fn texcoord_pointer;
    vertex_pointer_fn vertex_pointer;
    normal_pointer_fn normal_pointer;
    interleaved_arrays_fn interleaved_arrays;
    gen_textures_fn gen_textures;
    vertex3f_fn vertex3f;
    vertex3fv_fn vertex3fv;
    viewport_fn viewport;
    const GLubyte *renderer;
    PIXELFORMATDESCRIPTOR pfd;
    DHGLRC context;
    HDC dc;
    LONG formats;
    BOOL ok;
    unsigned frame;
    GLuint texture = 0;
    GLboolean boolean_value = GL_FALSE;
    GLboolean texture_residence = (GLboolean)0x7f;
    GLboolean all_textures_resident;
    GLclampf texture_priority = 0.75f;
    GLfloat feedback_values[8];
    GLuint selection_values[8];
    GLint render_result;
    GLint integer_value = 0;
    GLfloat matrix[16];
    GLfloat map_coefficients[6];
    GLfloat map_order = 0.0f;
    GLfloat red_map_copy = 1.0f;
    GLuint red_map_uint_copy = 1;
    GLushort red_map_ushort_copy = 1;
    GLubyte texture_copy[16];
    GLubyte pixel_copy[4];
    GLubyte stipple_copy[128];
    GLdouble clip_equation[4];
    GLvoid *pointer_value = NULL;
    static const GLdouble expected_clip_equation[] = { 1.0, -2.0, 3.0, -4.0 };
    static const GLubyte checker[] = {
        255, 255, 255, 255,  255, 32, 32, 255,
        32, 255, 32, 255,    32, 64, 255, 255
    };
    static const GLfloat identity[] = {
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1
    };
    static const GLfloat colors[][3] = {
        {1.0f, 0.15f, 0.1f}, {0.1f, 1.0f, 0.2f}, {0.15f, 0.3f, 1.0f}
    };
    static const GLfloat coordinates[][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}
    };
    static const GLfloat vertices[][3] = {
        {-0.65f, -0.55f, 0.0f}, {0.65f, -0.55f, 0.0f}, {0.0f, 0.7f, 0.0f}
    };
    static const GLfloat normals[][3] = {
        {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}
    };
    static const GLushort triangle_indices[] = { 0, 1, 2 };
    static const GLubyte test_pixel[] = { 7, 129, 241, 255 };
    static const GLubyte test_bitmap[] = { 0xaa };
    static GLubyte stipple_mask[128];
    static const GLdouble rect_d[] = { 0.0, 0.0 };
    static const GLfloat rect_f[] = { 0.0f, 0.0f };
    static const GLint rect_i[] = { 0, 0 };
    static const GLshort rect_s[] = { 0, 0 };
    static const GLfloat red_map[] = { 0.0f };
    static const GLuint red_map_uint[] = { 0 };
    static const GLushort red_map_ushort[] = { 0 };
    static const GLfloat map_points[] = {
        -0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f
    };
    static const GLfloat interleaved_vertices[][8] = {
        {0.0f, 0.0f, 1.0f, 0.15f, 0.1f, -0.65f, -0.55f, 0.0f},
        {1.0f, 0.0f, 0.1f, 1.0f, 0.2f, 0.65f, -0.55f, 0.0f},
        {0.5f, 1.0f, 0.15f, 0.3f, 1.0f, 0.0f, 0.7f, 0.0f}
    };
    FILE *log = fopen("C:\\QXPICD-TEST.LOG", "w");
    int rc = 20;

    if (!log) return 20;
    fprintf(log, "LOAD=%d ERROR=%lu\n", module != NULL, GetLastError());
    if (!module) goto done;
    rc = 2;
    validate = (validate_fn)GetProcAddress(module, "DrvValidateVersion");
    describe = (describe_fn)GetProcAddress(module, "DrvDescribePixelFormat");
    set_pixel_format = (set_pixel_format_fn)GetProcAddress(module,
                                                            "DrvSetPixelFormat");
    create_context = (create_context_fn)GetProcAddress(module,
                                                        "DrvCreateContext");
    set_context = (set_context_fn)GetProcAddress(module, "DrvSetContext");
    release_context = (release_context_fn)GetProcAddress(module,
                                                          "DrvReleaseContext");
    delete_context = (delete_context_fn)GetProcAddress(module,
                                                        "DrvDeleteContext");
    drv_swap_buffers = (swap_buffers_fn)GetProcAddress(module,
                                                        "DrvSwapBuffers");
    if (!validate || !describe || !set_pixel_format || !create_context ||
        !set_context || !release_context || !delete_context ||
        !drv_swap_buffers) {
        fprintf(log, "MISSING_EXPORT\n");
        rc = 3;
        goto done;
    }
    dc = GetDC(NULL);
    memset(&pfd, 0, sizeof(pfd));
    ok = validate(1);
    fprintf(log, "VALIDATE=%d\n", ok);
    if (!ok) { rc = 4; goto release_dc; }
    formats = describe(dc, 1, sizeof(pfd), &pfd);
    fprintf(log, "DESCRIBE=%ld\n", (long)formats);
    if (formats != 1) { rc = 5; goto release_dc; }
    ok = set_pixel_format(dc, 1);
    fprintf(log, "SET_PIXEL_FORMAT=%d\n", ok);
    if (!ok) { rc = 6; goto release_dc; }
    context = create_context(dc);
    fprintf(log, "CREATE_CONTEXT=%lu\n", (unsigned long)context);
    if (!context) { rc = 7; goto release_dc; }
    table = (QXP_PROC_TABLE *)set_context(dc, context, NULL);
    ok = table != NULL && table->cEntries == 336;
    fprintf(log, "SET_CONTEXT=%d\n", ok);
    if (!ok) { rc = 8; goto delete_context; }
    if (create_context(dc) != 0 || !release_context(context) ||
        release_context(context) || set_context(dc, 2, NULL) != NULL ||
        set_context(dc, context, NULL) != table) {
        rc = 27;
        goto delete_context;
    }
    if (!delete_context(context)) {
        rc = 27;
        context = 0;
        goto release_dc;
    }
    context = create_context(dc);
    table = context ? (QXP_PROC_TABLE *)set_context(dc, context, NULL) : NULL;
    if (context != 1 || !table || table->cEntries != 336) {
        rc = 27;
        goto delete_context;
    }
    begin = (begin_fn)table->entries[7];
    bitmap = (bitmap_fn)table->entries[8];
    color3f = (color3f_fn)table->entries[13];
    color3fv = (color3fv_fn)table->entries[14];
    end = (end_fn)table->entries[43];
    vertex3f = (vertex3f_fn)table->entries[136];
    vertex3fv = (vertex3fv_fn)table->entries[137];
    tex_coord2f = (tex_coord2f_fn)table->entries[104];
    tex_coord2fv = (tex_coord2fv_fn)table->entries[105];
    clip_plane = (clip_plane_fn)table->entries[150];
    tex_enviv = (tex_enviv_fn)table->entries[187];
    feedback_buffer = (feedback_buffer_fn)table->entries[194];
    select_buffer = (select_buffer_fn)table->entries[195];
    render_mode = (render_mode_fn)table->entries[196];
    pass_through = (pass_through_fn)table->entries[199];
    clear = (clear_fn)table->entries[203];
    clear_color = (clear_color_fn)table->entries[206];
    pixel_storef = (pixel_storef_fn)table->entries[249];
    pixel_storei = (pixel_storei_fn)table->entries[250];
    pixel_mapfv = (pixel_mapfv_fn)table->entries[251];
    pixel_mapuiv = (pixel_mapuiv_fn)table->entries[252];
    pixel_mapusv = (pixel_mapusv_fn)table->entries[253];
    polygon_stipple = (polygon_stipple_fn)table->entries[175];
    read_pixels = (read_pixels_fn)table->entries[256];
    draw_pixels = (draw_pixels_fn)table->entries[257];
    tex_parameteri = (tex_parameteri_fn)table->entries[180];
    tex_image2d = (tex_image2d_fn)table->entries[183];
    disable = (enable_fn)table->entries[214];
    enable = (enable_fn)table->entries[215];
    get_error = (get_error_fn)table->entries[261];
    get_booleanv = (get_booleanv_fn)table->entries[258];
    get_clip_plane = (get_clip_plane_fn)table->entries[259];
    get_floatv = (get_floatv_fn)table->entries[262];
    get_integerv = (get_integerv_fn)table->entries[263];
    get_pixel_mapfv = (get_pixel_mapfv_fn)table->entries[271];
    get_pixel_mapuiv = (get_pixel_mapuiv_fn)table->entries[272];
    get_pixel_mapusv = (get_pixel_mapusv_fn)table->entries[273];
    get_polygon_stipple = (get_polygon_stipple_fn)table->entries[274];
    get_string = (get_string_fn)table->entries[275];
    get_tex_image = (get_tex_image_fn)table->entries[281];
    get_tex_enviv = (get_tex_enviv_fn)table->entries[277];
    get_tex_parameteriv = (get_tex_parameteriv_fn)table->entries[283];
    get_tex_level_parameteriv =
        (get_tex_level_parameteriv_fn)table->entries[285];
    is_enabled = (is_enabled_fn)table->entries[286];
    load_identity = (load_identity_fn)table->entries[290];
    load_matrixf = (load_matrixf_fn)table->entries[291];
    matrix_mode = (matrix_mode_fn)table->entries[293];
    map1d = (map1d_fn)table->entries[220];
    map1f = (map1f_fn)table->entries[221];
    map2d = (map2d_fn)table->entries[222];
    map2f = (map2f_fn)table->entries[223];
    get_mapdv = (get_mapdv_fn)table->entries[266];
    get_mapfv = (get_mapfv_fn)table->entries[267];
    get_mapiv = (get_mapiv_fn)table->entries[268];
    ortho = (ortho_fn)table->entries[296];
    rotatef = (rotatef_fn)table->entries[300];
    raster_pos2i = (raster_pos2i_fn)table->entries[66];
    rectdv = (rectdv_fn)table->entries[87];
    rectfv = (rectfv_fn)table->entries[89];
    rectiv = (rectiv_fn)table->entries[91];
    rectsv = (rectsv_fn)table->entries[93];
    viewport = (viewport_fn)table->entries[305];
    bind_texture = (bind_texture_fn)table->entries[307];
    delete_textures = (delete_textures_fn)table->entries[327];
    gen_textures = (gen_textures_fn)table->entries[328];
    is_texture = (is_texture_fn)table->entries[330];
    are_textures_resident =
        (are_textures_resident_fn)table->entries[322];
    prioritize_textures =
        (prioritize_textures_fn)table->entries[331];
    get_pointerv = (get_pointerv_fn)table->entries[329];
    color_pointer = (color_pointer_fn)table->entries[308];
    disable_client_state = (client_state_fn)table->entries[309];
    draw_arrays = (draw_arrays_fn)table->entries[310];
    draw_elements = (draw_elements_fn)table->entries[311];
    enable_client_state = (client_state_fn)table->entries[313];
    normal_pointer = (normal_pointer_fn)table->entries[318];
    interleaved_arrays = (interleaved_arrays_fn)table->entries[317];
    texcoord_pointer = (texcoord_pointer_fn)table->entries[320];
    vertex_pointer = (vertex_pointer_fn)table->entries[321];
    if (!begin || !bitmap || !color3f || !color3fv || !end ||
        !vertex3f || !vertex3fv ||
        !clear || !clear_color ||
        !disable || !enable || !get_error || !get_string || !is_enabled ||
        !get_booleanv || !get_clip_plane || !get_floatv || !get_integerv ||
        !get_tex_image || !get_pointerv || !pixel_storef || !pixel_storei ||
        !pixel_mapfv || !pixel_mapuiv || !pixel_mapusv ||
        !get_pixel_mapfv || !get_pixel_mapuiv || !get_pixel_mapusv ||
        !polygon_stipple || !get_polygon_stipple ||
        !read_pixels || !draw_pixels || !raster_pos2i ||
        !rectdv || !rectfv || !rectiv || !rectsv ||
        !clip_plane || !tex_enviv || !get_tex_enviv ||
        !feedback_buffer || !select_buffer || !render_mode || !pass_through ||
        !get_tex_level_parameteriv || !get_tex_parameteriv ||
        !map1d || !map1f || !map2d || !map2f ||
        !get_mapdv || !get_mapfv || !get_mapiv ||
        !load_identity || !load_matrixf || !matrix_mode || !ortho || !rotatef ||
        !viewport ||
        !color_pointer || !disable_client_state || !draw_arrays ||
        !draw_elements || !enable_client_state || !interleaved_arrays ||
        !normal_pointer ||
        !texcoord_pointer || !vertex_pointer ||
        !bind_texture || !delete_textures || !gen_textures || !is_texture ||
        !are_textures_resident || !prioritize_textures ||
        !tex_coord2f || !tex_coord2fv || !tex_image2d || !tex_parameteri) {
        rc = 9;
        goto delete_context;
    }
    renderer = get_string(GL_RENDERER);
    fprintf(log, "RENDERER=%s\n", renderer ? (const char *)renderer : "(null)");
    if (!renderer) { rc = 9; goto delete_context; }
    if (argc > 1 && strcmp(argv[1], "--crash-owner") == 0) {
        fprintf(log, "CRASH_OWNER=1\n");
        fflush(log);
        Sleep(2000);
        TerminateProcess(GetCurrentProcess(), 0);
        rc = 17;
        goto delete_context;
    }
    if (argc > 1 && strcmp(argv[1], "--idle-owner") == 0) {
        fprintf(log, "IDLE_OWNER=1\n");
        fflush(log);
        Sleep(12000);
    }
    enable(GL_DEPTH_TEST);
    if (!is_enabled(GL_DEPTH_TEST)) { rc = 12; goto delete_context; }
    disable(GL_DEPTH_TEST);
    if (is_enabled(GL_DEPTH_TEST)) { rc = 12; goto delete_context; }
    if (get_error() != GL_NO_ERROR) { rc = 13; goto delete_context; }
    pixel_mapfv(GL_PIXEL_MAP_R_TO_R, 1, red_map);
    get_pixel_mapfv(GL_PIXEL_MAP_R_TO_R, &red_map_copy);
    pixel_mapuiv(GL_PIXEL_MAP_R_TO_R, 1, red_map_uint);
    get_pixel_mapuiv(GL_PIXEL_MAP_R_TO_R, &red_map_uint_copy);
    pixel_mapusv(GL_PIXEL_MAP_R_TO_R, 1, red_map_ushort);
    get_pixel_mapusv(GL_PIXEL_MAP_R_TO_R, &red_map_ushort_copy);
    if (red_map_copy != 0.0f || red_map_uint_copy != 0 ||
        red_map_ushort_copy != 0 || get_error() != GL_NO_ERROR) {
        rc = 26;
        goto delete_context;
    }
    memset(feedback_values, 0, sizeof(feedback_values));
    feedback_buffer(8, GL_2D, feedback_values);
    if (render_mode(GL_FEEDBACK) != 0) { rc = 25; goto delete_context; }
    pass_through(1.25f);
    render_result = render_mode(GL_RENDER);
    if (render_result != 2 ||
        feedback_values[0] != (GLfloat)GL_PASS_THROUGH_TOKEN ||
        feedback_values[1] != 1.25f) {
        rc = 25;
        goto delete_context;
    }
    memset(selection_values, 0, sizeof(selection_values));
    select_buffer(8, selection_values);
    if (render_mode(GL_SELECT) != 0 || render_mode(GL_RENDER) != 0 ||
        get_error() != GL_NO_ERROR) {
        rc = 25;
        goto delete_context;
    }
    for (frame = 0; frame < sizeof(stipple_mask); ++frame) {
        stipple_mask[frame] = (GLubyte)(frame ^ 0x5aU);
    }
    polygon_stipple(stipple_mask);
    memset(stipple_copy, 0, sizeof(stipple_copy));
    get_polygon_stipple(stipple_copy);
    if (memcmp(stipple_copy, stipple_mask, sizeof(stipple_mask)) != 0 ||
        get_error() != GL_NO_ERROR) {
        rc = 24;
        goto delete_context;
    }
    rectdv(rect_d, rect_d);
    rectfv(rect_f, rect_f);
    rectiv(rect_i, rect_i);
    rectsv(rect_s, rect_s);
    if (get_error() != GL_NO_ERROR) { rc = 20; goto delete_context; }
    map1f(GL_MAP1_VERTEX_3, 0.0f, 1.0f, 3, 2, map_points);
    get_mapfv(GL_MAP1_VERTEX_3, GL_ORDER, &map_order);
    memset(map_coefficients, 0, sizeof(map_coefficients));
    get_mapfv(GL_MAP1_VERTEX_3, GL_COEFF, map_coefficients);
    if (map_order != 2.0f ||
        memcmp(map_coefficients, map_points, sizeof(map_points)) != 0 ||
        get_error() != GL_NO_ERROR) {
        rc = 23;
        goto delete_context;
    }
    gen_textures(1, &texture);
    if (!texture) { rc = 14; goto delete_context; }
    bind_texture(GL_TEXTURE_2D, texture);
    tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    tex_image2d(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, checker);
    if (!is_texture(texture) || get_error() != GL_NO_ERROR) {
        rc = 14;
        goto delete_context;
    }
    prioritize_textures(1, &texture, &texture_priority);
    all_textures_resident =
        are_textures_resident(1, &texture, &texture_residence);
    if (texture_residence > GL_TRUE ||
        (all_textures_resident && !texture_residence) ||
        get_error() != GL_NO_ERROR) {
        rc = 21;
        goto delete_context;
    }
    memset(texture_copy, 0, sizeof(texture_copy));
    pixel_storei(GL_PACK_ROW_LENGTH, 0);
    pixel_storef(GL_UNPACK_ROW_LENGTH, 0.0f);
    get_tex_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_copy);
    if (memcmp(texture_copy, checker, sizeof(checker)) != 0) {
        rc = 18;
        goto delete_context;
    }
    enable(GL_TEXTURE_2D);
    get_booleanv(GL_TEXTURE_2D, &boolean_value);
    if (!boolean_value) { rc = 15; goto delete_context; }
    get_integerv(GL_TEXTURE_BINDING_2D, &integer_value);
    if ((GLuint)integer_value != texture) { rc = 15; goto delete_context; }
    get_tex_parameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &integer_value);
    if (integer_value != GL_NEAREST) { rc = 15; goto delete_context; }
    get_tex_level_parameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                              &integer_value);
    if (integer_value != 2) { rc = 15; goto delete_context; }
    integer_value = GL_REPLACE;
    tex_enviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &integer_value);
    integer_value = 0;
    get_tex_enviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &integer_value);
    if (integer_value != GL_REPLACE) { rc = 16; goto delete_context; }
    integer_value = GL_MODULATE;
    tex_enviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &integer_value);
    clip_plane(GL_CLIP_PLANE0, expected_clip_equation);
    memset(clip_equation, 0, sizeof(clip_equation));
    get_clip_plane(GL_CLIP_PLANE0, clip_equation);
    if (memcmp(clip_equation, expected_clip_equation,
               sizeof(expected_clip_equation)) != 0) {
        rc = 16;
        goto delete_context;
    }
    viewport(0, 0, 640, 480);
    matrix_mode(GL_PROJECTION);
    load_identity();
    ortho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    matrix_mode(GL_MODELVIEW);
    load_identity();
    get_floatv(GL_MODELVIEW_MATRIX, matrix);
    if (matrix[0] != 1.0f || matrix[5] != 1.0f || matrix[10] != 1.0f ||
        matrix[15] != 1.0f) {
        rc = 15;
        goto delete_context;
    }
    clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    raster_pos2i(0, 0);
    bitmap(8, 1, 0.0f, 0.0f, 0.0f, 0.0f, test_bitmap);
    if (get_error() != GL_NO_ERROR) { rc = 22; goto delete_context; }
    raster_pos2i(0, 0);
    draw_pixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, test_pixel);
    memset(pixel_copy, 0, sizeof(pixel_copy));
    read_pixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel_copy);
    if (memcmp(pixel_copy, test_pixel, sizeof(test_pixel)) != 0) {
        rc = 19;
        goto delete_context;
    }
    vertex_pointer(3, GL_FLOAT, 0, vertices);
    get_pointerv(GL_VERTEX_ARRAY_POINTER, &pointer_value);
    if (pointer_value != vertices) { rc = 19; goto delete_context; }
    for (frame = 0; frame < 180; ++frame) {
        load_matrixf(identity);
        rotatef((GLfloat)frame * 2.0f, 0.0f, 0.0f, 1.0f);
        clear_color(0.04f, 0.05f, 0.08f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        if (frame % 4 == 1) {
            color_pointer(3, GL_FLOAT, 0, colors);
            normal_pointer(GL_FLOAT, 0, normals);
            texcoord_pointer(2, GL_FLOAT, 0, coordinates);
            vertex_pointer(3, GL_FLOAT, 0, vertices);
            enable_client_state(GL_COLOR_ARRAY);
            enable_client_state(GL_NORMAL_ARRAY);
            enable_client_state(GL_TEXTURE_COORD_ARRAY);
            enable_client_state(GL_VERTEX_ARRAY);
            draw_arrays(GL_TRIANGLES, 0, 3);
        } else if (frame % 4 == 2) {
            color_pointer(3, GL_FLOAT, 0, colors);
            normal_pointer(GL_FLOAT, 0, normals);
            texcoord_pointer(2, GL_FLOAT, 0, coordinates);
            vertex_pointer(3, GL_FLOAT, 0, vertices);
            enable_client_state(GL_COLOR_ARRAY);
            enable_client_state(GL_NORMAL_ARRAY);
            enable_client_state(GL_TEXTURE_COORD_ARRAY);
            enable_client_state(GL_VERTEX_ARRAY);
            draw_elements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT,
                          triangle_indices);
        } else if (frame % 4 == 3) {
            interleaved_arrays(GL_T2F_C3F_V3F, 0, interleaved_vertices);
            draw_arrays(GL_TRIANGLES, 0, 3);
        } else {
            begin(GL_TRIANGLES);
            color3fv(colors[0]);
            tex_coord2fv(coordinates[0]);
            vertex3fv(vertices[0]);
            color3fv(colors[1]);
            tex_coord2fv(coordinates[1]);
            vertex3fv(vertices[1]);
            color3fv(colors[2]);
            tex_coord2fv(coordinates[2]);
            vertex3fv(vertices[2]);
            end();
        }
        if (!drv_swap_buffers(dc)) {
            rc = 10;
            goto delete_context;
        }
        Sleep(16);
    }
    disable_client_state(GL_VERTEX_ARRAY);
    disable_client_state(GL_TEXTURE_COORD_ARRAY);
    disable_client_state(GL_NORMAL_ARRAY);
    disable_client_state(GL_COLOR_ARRAY);
    disable(GL_TEXTURE_2D);
    delete_textures(1, &texture);
    texture = 0;
    rc = 0;
delete_context:
    if (texture && delete_textures) delete_textures(1, &texture);
    ok = delete_context(context);
    fprintf(log, "DELETE_CONTEXT=%d\n", ok);
    if (!ok) rc = 11;
release_dc:
    ReleaseDC(NULL, dc);
done:
    if (module) FreeLibrary(module);
    fprintf(log, "EXIT=%d\n", rc);
    fclose(log);
    report_result(rc);
    return rc;
}
