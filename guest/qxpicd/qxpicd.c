/* XPDriver Phase-1 OpenGL ICD. Derived portions are GPLv2. */
#include <windows.h>
#include <GL/gl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "transport.h"
#include "mglfunci.h"

#define DHGLRC ULONG
#define GL_DISPATCH_ENTRIES 336

typedef struct qxp_proc_table {
    int cEntries;
    PROC entries[GL_DISPATCH_ENTRIES];
} QXP_PROC_TABLE;
typedef void (APIENTRY *PFN_SETPROCTABLE)(const QXP_PROC_TABLE *table);

enum qxp_state {
    QXP_UNKNOWN, QXP_DEVICE_OK, QXP_ADVERTISING, QXP_GRANTED, QXP_DEAD,
    QXP_INERT
};

static volatile LONG state = QXP_UNKNOWN;
static uint16_t owner_token;
static volatile LONG heartbeat_running;
static HANDLE heartbeat_thread;
static volatile LONG context_exists;
static volatile LONG current_context;
static PIXELFORMATDESCRIPTOR advertised_pfd;
static char vendor_string[64];
static char renderer_string[128];
static char version_string[80];
static const char extension_string[] = "";

typedef struct qxp_client_array {
    BOOL enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const GLvoid *pointer;
} QXP_CLIENT_ARRAY;

static QXP_CLIENT_ARRAY color_array;
static QXP_CLIENT_ARRAY edge_array;
static QXP_CLIENT_ARRAY index_array;
static QXP_CLIENT_ARRAY normal_array;
static QXP_CLIENT_ARRAY texcoord_array;
static QXP_CLIENT_ARRAY vertex_array;
static BOOL interleaved_active;
static GLenum interleaved_format;
static GLsizei interleaved_stride;
static const GLvoid *interleaved_pointer;
static GLint pack_row_length;
static GLint unpack_row_length;
static GLint unpack_alignment;
static GLint unpack_skip_rows;
static GLint unpack_skip_pixels;
static GLenum current_render_mode = GL_RENDER;
static GLfloat *feedback_buffer;
static unsigned feedback_elements;
static GLuint *selection_buffer;
static unsigned selection_elements;

void qxp_install_scalar_stubs(PROC *entries);
int szgldata(int format, int type);
int szglname(int name);

static DWORD WINAPI heartbeat_main(LPVOID unused)
{
    (void)unused;
    while (InterlockedCompareExchange(&heartbeat_running, 1, 1) == 1) {
        qxp_transport_heartbeat(owner_token);
        Sleep(1000);
    }
    return 0;
}

static void start_heartbeat(void)
{
    if (heartbeat_thread) return;
    InterlockedExchange(&heartbeat_running, 1);
    heartbeat_thread = CreateThread(NULL, 0, heartbeat_main, NULL, 0, NULL);
    if (!heartbeat_thread) InterlockedExchange(&heartbeat_running, 0);
}

static void stop_heartbeat(void)
{
    HANDLE thread = heartbeat_thread;

    if (!thread) return;
    InterlockedExchange(&heartbeat_running, 0);
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
    heartbeat_thread = NULL;
}

#define USE_GL_FUNC(name, proto_args, call_args, offset, stack) \
    static void APIENTRY NoOp##name proto_args { (void)0; }
#define USE_GL_FUNC_RET(name, ret_type, proto_args, call_args, offset, stack) \
    static ret_type APIENTRY NoOp##name proto_args { return (ret_type)0; }
#include "third_party/reactos/glfuncs.h"

static const GLubyte *APIENTRY QxpGetString(GLenum name)
{
    char *buffer;
    unsigned capacity;

    switch (name) {
    case GL_VENDOR: buffer = vendor_string; capacity = sizeof(vendor_string); break;
    case GL_RENDERER: buffer = renderer_string; capacity = sizeof(renderer_string); break;
    case GL_VERSION: buffer = version_string; capacity = sizeof(version_string); break;
    /*
     * Do not advertise host extensions until DrvGetProcAddress can provide
     * their entry points. An empty string is the truthful GL 1.1 contract and
     * keeps legacy games from calling unsupported host-only functionality.
     */
    case GL_EXTENSIONS: return (const GLubyte *)extension_string;
    default: return NULL;
    }
    if (!buffer[0] && !qxp_transport_get_string(name, buffer, capacity)) {
        return NULL;
    }
    return (const GLubyte *)buffer;
}

static uint32_t float_bits(GLfloat value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static QXP_CLIENT_ARRAY *client_array(GLenum name)
{
    switch (name) {
    case GL_COLOR_ARRAY: return &color_array;
    case GL_EDGE_FLAG_ARRAY: return &edge_array;
    case GL_INDEX_ARRAY: return &index_array;
    case GL_NORMAL_ARRAY: return &normal_array;
    case GL_TEXTURE_COORD_ARRAY: return &texcoord_array;
    case GL_VERTEX_ARRAY: return &vertex_array;
    default: return NULL;
    }
}

static void reset_client_arrays(void)
{
    ZeroMemory(&color_array, sizeof(color_array));
    ZeroMemory(&edge_array, sizeof(edge_array));
    ZeroMemory(&index_array, sizeof(index_array));
    ZeroMemory(&normal_array, sizeof(normal_array));
    ZeroMemory(&texcoord_array, sizeof(texcoord_array));
    ZeroMemory(&vertex_array, sizeof(vertex_array));
    interleaved_active = FALSE;
    interleaved_format = 0;
    interleaved_stride = 0;
    interleaved_pointer = NULL;
    pack_row_length = 0;
    unpack_row_length = 0;
    unpack_alignment = 4;
    unpack_skip_rows = 0;
    unpack_skip_pixels = 0;
    current_render_mode = GL_RENDER;
    feedback_buffer = NULL;
    feedback_elements = 0;
    selection_buffer = NULL;
    selection_elements = 0;
}

static void set_client_pointer(QXP_CLIENT_ARRAY *array, GLint size,
                               GLenum type, GLsizei stride,
                               const GLvoid *pointer, uint32_t function,
                               const uint32_t *arguments, unsigned count)
{
    qxp_transport_submit(function, arguments, count);
    interleaved_active = FALSE;
    array->size = size;
    array->type = type;
    array->stride = stride;
    array->pointer = pointer;
}

static void APIENTRY QxpColorPointer(GLint size, GLenum type, GLsizei stride,
                                     const GLvoid *pointer)
{
    uint32_t arguments[] = {
        (uint32_t)size, type, (uint32_t)stride,
        (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&color_array, size, type, stride, pointer,
                       FEnum_glColorPointer, arguments, 4);
}

static void APIENTRY QxpNormalPointer(GLenum type, GLsizei stride,
                                      const GLvoid *pointer)
{
    uint32_t arguments[] = {
        type, (uint32_t)stride, (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&normal_array, 3, type, stride, pointer,
                       FEnum_glNormalPointer, arguments, 3);
}

static void APIENTRY QxpEdgeFlagPointer(GLsizei stride,
                                        const GLvoid *pointer)
{
    uint32_t arguments[] = {
        (uint32_t)stride, (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&edge_array, 1, GL_BYTE, stride, pointer,
                       FEnum_glEdgeFlagPointer, arguments, 2);
}

static void APIENTRY QxpIndexPointer(GLenum type, GLsizei stride,
                                     const GLvoid *pointer)
{
    uint32_t arguments[] = {
        type, (uint32_t)stride, (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&index_array, 1, type, stride, pointer,
                       FEnum_glIndexPointer, arguments, 3);
}

static void APIENTRY QxpTexCoordPointer(GLint size, GLenum type,
                                        GLsizei stride, const GLvoid *pointer)
{
    uint32_t arguments[] = {
        (uint32_t)size, type, (uint32_t)stride,
        (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&texcoord_array, size, type, stride, pointer,
                       FEnum_glTexCoordPointer, arguments, 4);
}

static void APIENTRY QxpVertexPointer(GLint size, GLenum type, GLsizei stride,
                                      const GLvoid *pointer)
{
    uint32_t arguments[] = {
        (uint32_t)size, type, (uint32_t)stride,
        (uint32_t)(uintptr_t)pointer
    };
    set_client_pointer(&vertex_array, size, type, stride, pointer,
                       FEnum_glVertexPointer, arguments, 4);
}

static void APIENTRY QxpEnableClientState(GLenum name)
{
    QXP_CLIENT_ARRAY *array = client_array(name);
    uint32_t arguments[] = { name };
    qxp_transport_submit(FEnum_glEnableClientState, arguments, 1);
    if (array) array->enabled = TRUE;
}

static void APIENTRY QxpDisableClientState(GLenum name)
{
    QXP_CLIENT_ARRAY *array = client_array(name);
    uint32_t arguments[] = { name };
    qxp_transport_submit(FEnum_glDisableClientState, arguments, 1);
    if (array) array->enabled = FALSE;
}

static void APIENTRY QxpInterleavedArrays(GLenum format, GLsizei stride,
                                          const GLvoid *pointer)
{
    uint32_t arguments[] = {
        format, (uint32_t)stride, (uint32_t)(uintptr_t)pointer
    };
    qxp_transport_submit(FEnum_glInterleavedArrays, arguments, 3);
    interleaved_format = format;
    interleaved_stride = stride;
    interleaved_pointer = pointer;
    interleaved_active = pointer != NULL;
}

static BOOL pack_interleaved(GLint first, GLsizei count, void **packed,
                             unsigned *packed_bytes)
{
    unsigned record, step, offset, bytes;
    const uint8_t *source;

    *packed = NULL;
    *packed_bytes = 0;
    if (!interleaved_active) return TRUE;
    if (first < 0 || count <= 0 || interleaved_stride < 0) return FALSE;
    record = (unsigned)szgldata(interleaved_format, 0);
    step = interleaved_stride ? (unsigned)interleaved_stride : record;
    if (!record || step < record || (unsigned)first > UINT_MAX / step ||
        (unsigned)(count - 1) > (UINT_MAX - record) / step) return FALSE;
    offset = (unsigned)first * step;
    bytes = (unsigned)(count - 1) * step + record;
    source = (const uint8_t *)interleaved_pointer + offset;
    if (bytes > UINT_MAX - 7U || IsBadReadPtr(source, bytes)) return FALSE;
    *packed_bytes = (bytes + 7U) & ~7U;
    *packed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *packed_bytes);
    if (!*packed) {
        *packed_bytes = 0;
        return FALSE;
    }
    memcpy(*packed, source, bytes);
    return TRUE;
}

static BOOL pack_client_arrays(GLint first, GLsizei count, void **packed,
                               unsigned *packed_bytes);

static BOOL pack_active_arrays(GLint first, GLsizei count, void **packed,
                               unsigned *packed_bytes)
{
    if (interleaved_active) {
        uint32_t arguments[] = {
            interleaved_format, (uint32_t)interleaved_stride,
            (uint32_t)(uintptr_t)interleaved_pointer
        };
        qxp_transport_submit(FEnum_glInterleavedArrays, arguments, 3);
        return pack_interleaved(first, count, packed, packed_bytes);
    }
    return pack_client_arrays(first, count, packed, packed_bytes);
}

static BOOL client_array_span(const QXP_CLIENT_ARRAY *array, GLint first,
                              GLsizei count, unsigned *source_offset,
                              unsigned *bytes)
{
    unsigned element, step, offset, span;

    *source_offset = 0;
    *bytes = 0;
    if (!array->enabled || !array->pointer) return TRUE;
    if (first < 0 || count <= 0 || array->stride < 0) return FALSE;
    element = (unsigned)szgldata(array->size, array->type);
    step = array->stride ? (unsigned)array->stride : element;
    if (!element || step < element || (unsigned)first > UINT_MAX / step ||
        (unsigned)(count - 1) > (UINT_MAX - element) / step) return FALSE;
    offset = (unsigned)first * step;
    span = (unsigned)(count - 1) * step + element;
    *source_offset = offset;
    *bytes = span;
    return TRUE;
}

static BOOL pack_client_arrays(GLint first, GLsizei count, void **packed,
                               unsigned *packed_bytes)
{
    QXP_CLIENT_ARRAY *arrays[] = {
        &color_array, &edge_array, &index_array, &normal_array,
        &texcoord_array, &vertex_array
    };
    unsigned offsets[6], spans[6], total = 0, cursor = 0, i;
    uint8_t *output;

    *packed = NULL;
    *packed_bytes = 0;
    for (i = 0; i < 6; ++i) {
        unsigned aligned;
        if (!client_array_span(arrays[i], first, count, &offsets[i],
                               &spans[i])) return FALSE;
        if (!spans[i]) continue;
        if (spans[i] > UINT_MAX - 7U) return FALSE;
        aligned = (spans[i] + 7U) & ~7U;
        if (total > UINT_MAX - aligned) return FALSE;
        total += aligned;
    }
    if (!total) return TRUE;
    output = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!output) return FALSE;
    for (i = 0; i < 6; ++i) {
        const uint8_t *source;
        if (!spans[i]) continue;
        source = (const uint8_t *)arrays[i]->pointer + offsets[i];
        if (IsBadReadPtr(source, spans[i])) {
            HeapFree(GetProcessHeap(), 0, output);
            return FALSE;
        }
        memcpy(output + cursor, source, spans[i]);
        cursor += (spans[i] + 7U) & ~7U;
    }
    *packed = output;
    *packed_bytes = total;
    return TRUE;
}

static void APIENTRY QxpDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    uint32_t arguments[] = { mode, (uint32_t)first, (uint32_t)count };
    void *packed;
    unsigned packed_bytes;

    if (count <= 0) {
        qxp_transport_submit(FEnum_glDrawArrays, arguments, 3);
        return;
    }
    if (!pack_active_arrays(first, count, &packed, &packed_bytes)) return;
    if (!packed_bytes) {
        qxp_transport_submit(FEnum_glDrawArrays, arguments, 3);
        return;
    }
    qxp_transport_submit_input(FEnum_glDrawArrays, arguments, 3, packed,
                             packed_bytes);
    HeapFree(GetProcessHeap(), 0, packed);
}

static BOOL element_range(GLsizei count, GLenum type, const GLvoid *indices,
                          GLint *first, GLsizei *span)
{
    unsigned element_bytes, i;
    uint32_t low = UINT_MAX, high = 0;

    if (count <= 0 || !indices) return FALSE;
    switch (type) {
    case GL_UNSIGNED_BYTE: element_bytes = 1; break;
    case GL_UNSIGNED_SHORT: element_bytes = 2; break;
    case GL_UNSIGNED_INT: element_bytes = 4; break;
    default: return FALSE;
    }
    if ((unsigned)count > UINT_MAX / element_bytes ||
        IsBadReadPtr(indices, (unsigned)count * element_bytes)) return FALSE;
    for (i = 0; i < (unsigned)count; ++i) {
        uint32_t value;
        if (element_bytes == 1) value = ((const uint8_t *)indices)[i];
        else if (element_bytes == 2) value = ((const uint16_t *)indices)[i];
        else value = ((const uint32_t *)indices)[i];
        if (value < low) low = value;
        if (value > high) high = value;
    }
    if (high > INT_MAX || high - low >= INT_MAX) return FALSE;
    *first = (GLint)low;
    *span = (GLsizei)(high - low + 1U);
    return TRUE;
}

static void APIENTRY QxpDrawElements(GLenum mode, GLsizei count, GLenum type,
                                     const GLvoid *indices)
{
    uint32_t arguments[] = {
        mode, (uint32_t)count, type, (uint32_t)(uintptr_t)indices
    };
    unsigned element_bytes, index_bytes, aligned_indices, array_bytes;
    GLint first;
    GLsizei span;
    void *arrays = NULL;
    uint8_t *packed;

    if (count <= 0) {
        qxp_transport_submit(FEnum_glDrawElements, arguments, 4);
        return;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: element_bytes = 1; break;
    case GL_UNSIGNED_SHORT: element_bytes = 2; break;
    case GL_UNSIGNED_INT: element_bytes = 4; break;
    default: return;
    }
    if (!element_range(count, type, indices, &first, &span) ||
        !pack_active_arrays(first, span, &arrays, &array_bytes)) return;
    index_bytes = (unsigned)count * element_bytes;
    aligned_indices = (index_bytes + 7U) & ~7U;
    if (array_bytes > UINT_MAX - aligned_indices) {
        if (arrays) HeapFree(GetProcessHeap(), 0, arrays);
        return;
    }
    packed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                       aligned_indices + array_bytes);
    if (!packed) {
        if (arrays) HeapFree(GetProcessHeap(), 0, arrays);
        return;
    }
    memcpy(packed, indices, index_bytes);
    if (array_bytes) memcpy(packed + aligned_indices, arrays, array_bytes);
    qxp_transport_submit_input(FEnum_glDrawElements, arguments, 4, packed,
                             aligned_indices + array_bytes);
    if (arrays) HeapFree(GetProcessHeap(), 0, arrays);
    HeapFree(GetProcessHeap(), 0, packed);
}

static void APIENTRY QxpClear(GLbitfield mask)
{
    uint32_t arguments[] = { mask };
    qxp_transport_submit(FEnum_glClear, arguments, 1);
}

static void APIENTRY QxpFeedbackBuffer(GLsizei size, GLenum type,
                                       GLfloat *buffer)
{
    unsigned effective = size > 0 ? (unsigned)size : 0;
    uint32_t arguments[3];

    if (effective > (3U * 4096U) / sizeof(*buffer)) {
        effective = (3U * 4096U) / sizeof(*buffer);
    }
    feedback_buffer = buffer;
    feedback_elements = buffer ? effective : 0;
    arguments[0] = effective;
    arguments[1] = type;
    arguments[2] = (uint32_t)(uintptr_t)buffer;
    qxp_transport_submit(FEnum_glFeedbackBuffer, arguments, 3);
}

static void APIENTRY QxpSelectBuffer(GLsizei size, GLuint *buffer)
{
    unsigned effective = size > 0 ? (unsigned)size : 0;
    uint32_t arguments[2];

    if (effective > (3U * 4096U) / sizeof(*buffer)) {
        effective = (3U * 4096U) / sizeof(*buffer);
    }
    selection_buffer = buffer;
    selection_elements = buffer ? effective : 0;
    arguments[0] = effective;
    arguments[1] = (uint32_t)(uintptr_t)buffer;
    qxp_transport_submit(FEnum_glSelectBuffer, arguments, 2);
}

static GLint APIENTRY QxpRenderMode(GLenum mode)
{
    uint32_t arguments[] = { mode };
    void *output = NULL;
    unsigned bytes = 0;
    GLint result;

    if (current_render_mode == GL_FEEDBACK && feedback_buffer) {
        output = feedback_buffer;
        bytes = feedback_elements * sizeof(*feedback_buffer);
    } else if (current_render_mode == GL_SELECT && selection_buffer) {
        output = selection_buffer;
        bytes = selection_elements * sizeof(*selection_buffer);
    }
    result = (GLint)qxp_transport_call_return_output(
        FEnum_glRenderMode, arguments, 1, output, bytes);
    current_render_mode = mode;
    return result;
}

static void APIENTRY QxpBegin(GLenum mode)
{
    uint32_t arguments[] = { mode };
    qxp_transport_submit(FEnum_glBegin, arguments, 1);
}

static void APIENTRY QxpEnd(void)
{
    qxp_transport_submit(FEnum_glEnd, NULL, 0);
}

static void APIENTRY QxpColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
    uint32_t arguments[] = { float_bits(red), float_bits(green), float_bits(blue) };
    qxp_transport_submit(FEnum_glColor3f, arguments, 3);
}

static void APIENTRY QxpVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    uint32_t arguments[] = { float_bits(x), float_bits(y), float_bits(z) };
    qxp_transport_submit(FEnum_glVertex3f, arguments, 3);
}

static void rect_vectors(uint32_t function, const void *first,
                         const void *second, unsigned element_size)
{
    uint32_t arguments[] = {
        (uint32_t)(uintptr_t)first, (uint32_t)(uintptr_t)second
    };
    uint8_t packed[32];
    unsigned vector_bytes = 2U * element_size;
    unsigned stride = (vector_bytes + 7U) & ~7U;

    if (!first || !second || stride * 2U > sizeof(packed)) return;
    ZeroMemory(packed, sizeof(packed));
    memcpy(packed, first, vector_bytes);
    memcpy(packed + stride, second, vector_bytes);
    qxp_transport_submit_input(function, arguments, 2, packed, stride * 2U);
}

static void APIENTRY QxpRectdv(const GLdouble *first, const GLdouble *second)
{ rect_vectors(FEnum_glRectdv, first, second, sizeof(*first)); }
static void APIENTRY QxpRectfv(const GLfloat *first, const GLfloat *second)
{ rect_vectors(FEnum_glRectfv, first, second, sizeof(*first)); }
static void APIENTRY QxpRectiv(const GLint *first, const GLint *second)
{ rect_vectors(FEnum_glRectiv, first, second, sizeof(*first)); }
static void APIENTRY QxpRectsv(const GLshort *first, const GLshort *second)
{ rect_vectors(FEnum_glRectsv, first, second, sizeof(*first)); }

static unsigned pixel_data_bytes(GLsizei width, GLsizei height,
                                 GLenum format, GLenum type,
                                 GLint row_length)
{
    unsigned columns, rows, element;

    if (width <= 0 || height <= 0 || row_length < 0) return 0;
    columns = row_length ? (unsigned)row_length : (unsigned)width;
    rows = (unsigned)height;
    element = (unsigned)szgldata(format, type);
    if (!element || columns > UINT_MAX / rows ||
        columns * rows > UINT_MAX / element) return 0;
    return columns * rows * element;
}

static void APIENTRY QxpPixelStorei(GLenum name, GLint value)
{
    uint32_t arguments[] = { name, (uint32_t)value };
    qxp_transport_submit(FEnum_glPixelStorei, arguments, 2);
    if (name == GL_PACK_ROW_LENGTH) pack_row_length = value;
    if (name == GL_UNPACK_ROW_LENGTH) unpack_row_length = value;
    if (name == GL_UNPACK_ALIGNMENT &&
        (value == 1 || value == 2 || value == 4 || value == 8)) {
        unpack_alignment = value;
    }
    if (name == GL_UNPACK_SKIP_ROWS) unpack_skip_rows = value;
    if (name == GL_UNPACK_SKIP_PIXELS) unpack_skip_pixels = value;
}

static void APIENTRY QxpPixelStoref(GLenum name, GLfloat value)
{
    QxpPixelStorei(name, (GLint)value);
}

static void pixel_map_input(uint32_t function, GLenum map, GLint count,
                            const void *values, unsigned element_size)
{
    unsigned maximum_elements = element_size == sizeof(GLushort) ? 4096U :
                                                                  2048U;
    unsigned effective_count;
    uint32_t arguments[] = {
        map, 0, (uint32_t)(uintptr_t)values
    };

    if (count > 0 && values) {
        effective_count = (unsigned)count;
        if (effective_count > maximum_elements) {
            effective_count = maximum_elements;
        }
        arguments[1] = effective_count;
        qxp_transport_submit_input(function, arguments, 3, values,
                                 effective_count * element_size);
    }
}

static void APIENTRY QxpPixelMapfv(GLenum map, GLint count,
                                   const GLfloat *values)
{ pixel_map_input(FEnum_glPixelMapfv, map, count, values, sizeof(*values)); }
static void APIENTRY QxpPixelMapuiv(GLenum map, GLint count,
                                    const GLuint *values)
{ pixel_map_input(FEnum_glPixelMapuiv, map, count, values, sizeof(*values)); }
static void APIENTRY QxpPixelMapusv(GLenum map, GLint count,
                                    const GLushort *values)
{ pixel_map_input(FEnum_glPixelMapusv, map, count, values, sizeof(*values)); }

static void APIENTRY QxpGetPixelMapfv(GLenum map, GLfloat *values)
{
    uint32_t arguments[] = { map, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetPixelMapfv, arguments, 2, values, sizeof(*values), 2048);
}

static void APIENTRY QxpGetPixelMapuiv(GLenum map, GLuint *values)
{
    uint32_t arguments[] = { map, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetPixelMapuiv, arguments, 2, values, sizeof(*values), 2048);
}

static void APIENTRY QxpGetPixelMapusv(GLenum map, GLushort *values)
{
    uint32_t arguments[] = { map, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetPixelMapusv, arguments, 2, values, sizeof(*values), 4096);
}

static void APIENTRY QxpGetPolygonStipple(GLubyte *mask)
{
    uint32_t arguments[] = { (uint32_t)(uintptr_t)mask };
    if (mask) qxp_transport_call_output(
        FEnum_glGetPolygonStipple, arguments, 1, mask, 128);
}

static unsigned bitmap_data_bytes(GLsizei width, GLsizei height)
{
    uint64_t row_bits, row_bytes, stride, last_bytes, total;
    unsigned alignment;

    if (width <= 0 || height <= 0 || unpack_row_length < 0 ||
        unpack_alignment <= 0 || unpack_skip_rows < 0 ||
        unpack_skip_pixels < 0) return 0;
    alignment = (unsigned)unpack_alignment;
    row_bits = unpack_row_length ? (unsigned)unpack_row_length : (unsigned)width;
    row_bits += (unsigned)unpack_skip_pixels;
    row_bytes = (row_bits + 7U) >> 3;
    stride = (row_bytes + alignment - 1U) & ~(uint64_t)(alignment - 1U);
    last_bytes = ((uint64_t)(unsigned)width +
                  (unsigned)unpack_skip_pixels + 7U) >> 3;
    total = (uint64_t)(unsigned)unpack_skip_rows * stride;
    total += (uint64_t)(unsigned)(height - 1) * stride + last_bytes;
    return total > UINT_MAX ? 0 : (unsigned)total;
}

static void APIENTRY QxpBitmap(GLsizei width, GLsizei height,
                               GLfloat xorig, GLfloat yorig,
                               GLfloat xmove, GLfloat ymove,
                               const GLubyte *bitmap)
{
    uint32_t arguments[] = {
        (uint32_t)width, (uint32_t)height, float_bits(xorig),
        float_bits(yorig), float_bits(xmove), float_bits(ymove),
        (uint32_t)(uintptr_t)bitmap
    };
    unsigned bytes = bitmap ? bitmap_data_bytes(width, height) : 0;

    if (bitmap && !bytes) return;
    if (bitmap) {
        qxp_transport_call_fbtm(FEnum_glBitmap, arguments, 7, bitmap, bytes);
    } else {
        qxp_transport_submit(FEnum_glBitmap, arguments, 7);
    }
}

static BOOL multiply_size(uint64_t *value, uint64_t factor)
{
    if (factor && *value > UINT_MAX / factor) return FALSE;
    *value *= factor;
    return TRUE;
}

static void map_input(uint32_t function, const uint32_t *arguments,
                      unsigned argument_count, const void *points,
                      unsigned element_size, uint64_t host_elements,
                      uint64_t actual_elements)
{
    uint8_t *packed;
    unsigned host_bytes, actual_bytes;

    if (!points || !host_elements || actual_elements > host_elements ||
        !multiply_size(&host_elements, element_size) ||
        !multiply_size(&actual_elements, element_size)) return;
    host_bytes = (unsigned)host_elements;
    actual_bytes = (unsigned)actual_elements;
    packed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, host_bytes);
    if (!packed) return;
    if (IsBadReadPtr(points, actual_bytes)) {
        HeapFree(GetProcessHeap(), 0, packed);
        return;
    }
    memcpy(packed, points, actual_bytes);
    qxp_transport_submit_input(function, arguments, argument_count, packed,
                             host_bytes);
    HeapFree(GetProcessHeap(), 0, packed);
}

static void APIENTRY QxpMap1f(GLenum target, GLfloat u1, GLfloat u2,
                              GLint stride, GLint order,
                              const GLfloat *points)
{
    uint32_t arguments[] = {
        target, float_bits(u1), float_bits(u2), (uint32_t)stride,
        (uint32_t)order, (uint32_t)(uintptr_t)points
    };
    uint64_t components = (uint64_t)(unsigned)szglname(target);
    uint64_t host = components, actual;

    if (stride <= 0 || order <= 0 ||
        !multiply_size(&host, (unsigned)stride) ||
        !multiply_size(&host, (unsigned)order)) return;
    actual = (uint64_t)(unsigned)(order - 1) * (unsigned)stride + components;
    map_input(FEnum_glMap1f, arguments, 6, points, sizeof(*points),
              host, actual);
}

static void APIENTRY QxpMap1d(GLenum target, GLdouble u1, GLdouble u2,
                              GLint stride, GLint order,
                              const GLdouble *points)
{
    uint32_t arguments[8];
    uint64_t components = (uint64_t)(unsigned)szglname(target);
    uint64_t host = components, actual;

    arguments[0] = target;
    memcpy(&arguments[1], &u1, sizeof(u1));
    memcpy(&arguments[3], &u2, sizeof(u2));
    arguments[5] = (uint32_t)stride;
    arguments[6] = (uint32_t)order;
    arguments[7] = (uint32_t)(uintptr_t)points;
    if (stride <= 0 || order <= 0 ||
        !multiply_size(&host, (unsigned)stride) ||
        !multiply_size(&host, (unsigned)order)) return;
    actual = (uint64_t)(unsigned)(order - 1) * (unsigned)stride + components;
    map_input(FEnum_glMap1d, arguments, 8, points, sizeof(*points),
              host, actual);
}

static void APIENTRY QxpMap2f(GLenum target, GLfloat u1, GLfloat u2,
                              GLint ustride, GLint uorder, GLfloat v1,
                              GLfloat v2, GLint vstride, GLint vorder,
                              const GLfloat *points)
{
    uint32_t arguments[] = {
        target, float_bits(u1), float_bits(u2), (uint32_t)ustride,
        (uint32_t)uorder, float_bits(v1), float_bits(v2), (uint32_t)vstride,
        (uint32_t)vorder, (uint32_t)(uintptr_t)points
    };
    uint64_t components = (uint64_t)(unsigned)szglname(target);
    uint64_t host = components, actual;

    if (ustride <= 0 || uorder <= 0 || vstride <= 0 || vorder <= 0 ||
        !multiply_size(&host, (unsigned)ustride) ||
        !multiply_size(&host, (unsigned)uorder) ||
        !multiply_size(&host, (unsigned)vstride) ||
        !multiply_size(&host, (unsigned)vorder)) return;
    actual = (uint64_t)(unsigned)(uorder - 1) * (unsigned)ustride +
        (uint64_t)(unsigned)(vorder - 1) * (unsigned)vstride + components;
    map_input(FEnum_glMap2f, arguments, 10, points, sizeof(*points),
              host, actual);
}

static void APIENTRY QxpMap2d(GLenum target, GLdouble u1, GLdouble u2,
                              GLint ustride, GLint uorder, GLdouble v1,
                              GLdouble v2, GLint vstride, GLint vorder,
                              const GLdouble *points)
{
    uint32_t arguments[14];
    uint64_t components = (uint64_t)(unsigned)szglname(target);
    uint64_t host = components, actual;

    arguments[0] = target;
    memcpy(&arguments[1], &u1, sizeof(u1));
    memcpy(&arguments[3], &u2, sizeof(u2));
    arguments[5] = (uint32_t)ustride;
    arguments[6] = (uint32_t)uorder;
    memcpy(&arguments[7], &v1, sizeof(v1));
    memcpy(&arguments[9], &v2, sizeof(v2));
    arguments[11] = (uint32_t)vstride;
    arguments[12] = (uint32_t)vorder;
    arguments[13] = (uint32_t)(uintptr_t)points;
    if (ustride <= 0 || uorder <= 0 || vstride <= 0 || vorder <= 0 ||
        !multiply_size(&host, (unsigned)ustride) ||
        !multiply_size(&host, (unsigned)uorder) ||
        !multiply_size(&host, (unsigned)vstride) ||
        !multiply_size(&host, (unsigned)vorder)) return;
    actual = (uint64_t)(unsigned)(uorder - 1) * (unsigned)ustride +
        (uint64_t)(unsigned)(vorder - 1) * (unsigned)vstride + components;
    map_input(FEnum_glMap2d, arguments, 14, points, sizeof(*points),
              host, actual);
}

static void APIENTRY QxpDrawPixels(GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const GLvoid *pixels)
{
    uint32_t arguments[] = {
        (uint32_t)width, (uint32_t)height, format, type,
        (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, height, format, type, unpack_row_length) : 0;

    if (bytes) qxp_transport_submit_input(
        FEnum_glDrawPixels, arguments, 5, pixels, bytes);
}

static void APIENTRY QxpReadPixels(GLint x, GLint y, GLsizei width,
                                   GLsizei height, GLenum format, GLenum type,
                                   GLvoid *pixels)
{
    uint32_t arguments[] = {
        (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height,
        format, type, (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, height, format, type, pack_row_length) : 0;

    if (bytes) qxp_transport_call_fbtm_output(
        FEnum_glReadPixels, arguments, 7, pixels, bytes, 0);
}

static void APIENTRY QxpGetTexImage(GLenum target, GLint level, GLenum format,
                                    GLenum type, GLvoid *pixels)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, format, type,
        (uint32_t)(uintptr_t)pixels
    };
    if (pixels) qxp_transport_call_fbtm_counted_output(
        FEnum_glGetTexImage, arguments, 5, pixels);
}

static const GLvoid *interleaved_array_pointer(GLenum name)
{
    unsigned color = UINT_MAX, normal = UINT_MAX, texcoord = UINT_MAX;
    unsigned vertex = UINT_MAX;

    switch (interleaved_format) {
    case GL_V2F: case GL_V3F: vertex = 0; break;
    case GL_C4UB_V2F: case GL_C4UB_V3F: color = 0; vertex = 4; break;
    case GL_C3F_V3F: color = 0; vertex = 12; break;
    case GL_N3F_V3F: normal = 0; vertex = 12; break;
    case GL_C4F_N3F_V3F: color = 0; normal = 16; vertex = 28; break;
    case GL_T2F_V3F: texcoord = 0; vertex = 8; break;
    case GL_T4F_V4F: texcoord = 0; vertex = 16; break;
    case GL_T2F_C4UB_V3F: texcoord = 0; color = 8; vertex = 12; break;
    case GL_T2F_C3F_V3F: texcoord = 0; color = 8; vertex = 20; break;
    case GL_T2F_N3F_V3F: texcoord = 0; normal = 8; vertex = 20; break;
    case GL_T2F_C4F_N3F_V3F:
        texcoord = 0; color = 8; normal = 24; vertex = 36; break;
    case GL_T4F_C4F_N3F_V4F:
        texcoord = 0; color = 16; normal = 32; vertex = 44; break;
    default: break;
    }
    switch (name) {
    case GL_COLOR_ARRAY_POINTER:
        return color == UINT_MAX ? NULL :
            (const uint8_t *)interleaved_pointer + color;
    case GL_NORMAL_ARRAY_POINTER:
        return normal == UINT_MAX ? NULL :
            (const uint8_t *)interleaved_pointer + normal;
    case GL_TEXTURE_COORD_ARRAY_POINTER:
        return texcoord == UINT_MAX ? NULL :
            (const uint8_t *)interleaved_pointer + texcoord;
    case GL_VERTEX_ARRAY_POINTER:
        return vertex == UINT_MAX ? NULL :
            (const uint8_t *)interleaved_pointer + vertex;
    default: return NULL;
    }
}

static void APIENTRY QxpGetPointerv(GLenum name, GLvoid **value)
{
    if (!value) return;
    if (interleaved_active) {
        *value = (GLvoid *)interleaved_array_pointer(name);
        return;
    }
    switch (name) {
    case GL_COLOR_ARRAY_POINTER: *value = (GLvoid *)color_array.pointer; break;
    case GL_EDGE_FLAG_ARRAY_POINTER: *value = (GLvoid *)edge_array.pointer; break;
    case GL_INDEX_ARRAY_POINTER: *value = (GLvoid *)index_array.pointer; break;
    case GL_NORMAL_ARRAY_POINTER: *value = (GLvoid *)normal_array.pointer; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER:
        *value = (GLvoid *)texcoord_array.pointer; break;
    case GL_VERTEX_ARRAY_POINTER: *value = (GLvoid *)vertex_array.pointer; break;
    default: *value = NULL; break;
    }
}

static void APIENTRY QxpGenTextures(GLsizei count, GLuint *textures)
{
    uint32_t arguments[] = {
        (uint32_t)count, (uint32_t)(uintptr_t)textures
    };

    if (count > 0 && textures) {
        qxp_transport_call_output(FEnum_glGenTextures, arguments, 2, textures,
                                  (unsigned)count * sizeof(*textures));
    }
}

static void APIENTRY QxpDeleteTextures(GLsizei count, const GLuint *textures)
{
    uint32_t arguments[] = {
        (uint32_t)count, (uint32_t)(uintptr_t)textures
    };

    if (count > 0 && textures) {
        qxp_transport_submit_input(FEnum_glDeleteTextures, arguments, 2, textures,
                                 (unsigned)count * sizeof(*textures));
    }
}

static GLboolean APIENTRY QxpAreTexturesResident(GLsizei count,
                                                  const GLuint *textures,
                                                  GLboolean *residences)
{
    uint32_t arguments[] = {
        (uint32_t)count, (uint32_t)(uintptr_t)textures,
        (uint32_t)(uintptr_t)residences
    };
    uint32_t result;

    if (count <= 0 || !textures || !residences ||
        (unsigned)count > UINT_MAX / sizeof(*textures)) return GL_FALSE;
    result = qxp_transport_call_input_output_return(
        FEnum_glAreTexturesResident, arguments, 3, textures,
        (unsigned)count * sizeof(*textures), residences, (unsigned)count);
    return (GLboolean)result;
}

static void APIENTRY QxpPrioritizeTextures(GLsizei count,
                                            const GLuint *textures,
                                            const GLclampf *priorities)
{
    uint32_t arguments[] = {
        (uint32_t)count, (uint32_t)(uintptr_t)textures,
        (uint32_t)(uintptr_t)priorities
    };
    unsigned bytes, stride;
    uint8_t *packed;

    if (count <= 0 || !textures || !priorities ||
        (unsigned)count > (UINT_MAX - 7U) / sizeof(*textures)) return;
    bytes = (unsigned)count * sizeof(*textures);
    stride = (bytes + 7U) & ~7U;
    if (stride > UINT_MAX / 2U) return;
    packed = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stride * 2U);
    if (!packed) return;
    memcpy(packed, textures, bytes);
    memcpy(packed + stride, priorities,
           (unsigned)count * sizeof(*priorities));
    qxp_transport_submit_input(FEnum_glPrioritizeTextures, arguments, 3,
                             packed, stride * 2U);
    HeapFree(GetProcessHeap(), 0, packed);
}

static void APIENTRY QxpTexImage2D(GLenum target, GLint level,
                                   GLint internal_format, GLsizei width,
                                   GLsizei height, GLint border, GLenum format,
                                   GLenum type, const GLvoid *pixels)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, (uint32_t)internal_format, (uint32_t)width,
        (uint32_t)height, (uint32_t)border, format, type,
        (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, height, format, type, unpack_row_length) : 0;

    qxp_transport_call_fbtm(FEnum_glTexImage2D, arguments, 9, pixels, bytes);
}

static void APIENTRY QxpTexSubImage2D(GLenum target, GLint level, GLint x,
                                      GLint y, GLsizei width, GLsizei height,
                                      GLenum format, GLenum type,
                                      const GLvoid *pixels)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, (uint32_t)x, (uint32_t)y, (uint32_t)width,
        (uint32_t)height, format, type, (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, height, format, type, unpack_row_length) : 0;

    qxp_transport_call_fbtm(FEnum_glTexSubImage2D, arguments, 9, pixels, bytes);
}

static void APIENTRY QxpGetBooleanv(GLenum name, GLboolean *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetBooleanv, arguments, 2, values, sizeof(*values), 32);
}

static void APIENTRY QxpGetDoublev(GLenum name, GLdouble *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetDoublev, arguments, 2, values, sizeof(*values), 16);
}

static void APIENTRY QxpGetFloatv(GLenum name, GLfloat *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetFloatv, arguments, 2, values, sizeof(*values), 16);
}

static void APIENTRY QxpGetIntegerv(GLenum name, GLint *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetIntegerv, arguments, 2, values, sizeof(*values), 16);
}

static void APIENTRY QxpGetMapdv(GLenum target, GLenum query, GLdouble *values)
{
    uint32_t arguments[] = {
        target, query, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetMapdv, arguments, 3, values, sizeof(*values), 1024);
}

static void APIENTRY QxpGetMapfv(GLenum target, GLenum query, GLfloat *values)
{
    uint32_t arguments[] = {
        target, query, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetMapfv, arguments, 3, values, sizeof(*values), 2048);
}

static void APIENTRY QxpGetMapiv(GLenum target, GLenum query, GLint *values)
{
    uint32_t arguments[] = {
        target, query, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetMapiv, arguments, 3, values, sizeof(*values), 2048);
}

static void APIENTRY QxpGetTexParameterfv(GLenum target, GLenum name,
                                          GLfloat *values)
{
    uint32_t arguments[] = {
        target, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetTexParameterfv, arguments, 3, values, sizeof(*values), 4);
}

static void APIENTRY QxpGetTexParameteriv(GLenum target, GLenum name,
                                          GLint *values)
{
    uint32_t arguments[] = {
        target, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetTexParameteriv, arguments, 3, values, sizeof(*values), 4);
}

static void APIENTRY QxpGetTexLevelParameterfv(GLenum target, GLint level,
                                               GLenum name, GLfloat *values)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetTexLevelParameterfv, arguments, 4, values,
        sizeof(*values), 4);
}

static void APIENTRY QxpGetTexLevelParameteriv(GLenum target, GLint level,
                                               GLenum name, GLint *values)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetTexLevelParameteriv, arguments, 4, values,
        sizeof(*values), 4);
}

static void APIENTRY QxpGetClipPlane(GLenum plane, GLdouble *equation)
{
    uint32_t arguments[] = {
        plane, (uint32_t)(uintptr_t)equation
    };
    if (equation) qxp_transport_call_output(
        FEnum_glGetClipPlane, arguments, 2, equation, 4 * sizeof(*equation));
}

static void APIENTRY QxpClipPlane(GLenum plane, const GLdouble *equation)
{
    uint32_t arguments[] = {
        plane, (uint32_t)(uintptr_t)equation
    };
    if (equation) qxp_transport_submit_input(
        FEnum_glClipPlane, arguments, 2, equation, 4 * sizeof(*equation));
}

static void get_named_2f(uint32_t function, GLenum first, GLenum name,
                         GLfloat *values)
{
    uint32_t arguments[] = {
        first, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        function, arguments, 3, values, sizeof(*values), 4);
}

static void get_named_2i(uint32_t function, GLenum first, GLenum name,
                         GLint *values)
{
    uint32_t arguments[] = {
        first, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        function, arguments, 3, values, sizeof(*values), 4);
}

static void APIENTRY QxpGetLightfv(GLenum light, GLenum name, GLfloat *values)
{ get_named_2f(FEnum_glGetLightfv, light, name, values); }
static void APIENTRY QxpGetLightiv(GLenum light, GLenum name, GLint *values)
{ get_named_2i(FEnum_glGetLightiv, light, name, values); }
static void APIENTRY QxpGetMaterialfv(GLenum face, GLenum name,
                                      GLfloat *values)
{ get_named_2f(FEnum_glGetMaterialfv, face, name, values); }
static void APIENTRY QxpGetMaterialiv(GLenum face, GLenum name, GLint *values)
{ get_named_2i(FEnum_glGetMaterialiv, face, name, values); }
static void APIENTRY QxpGetTexEnvfv(GLenum target, GLenum name,
                                    GLfloat *values)
{ get_named_2f(FEnum_glGetTexEnvfv, target, name, values); }
static void APIENTRY QxpGetTexEnviv(GLenum target, GLenum name, GLint *values)
{ get_named_2i(FEnum_glGetTexEnviv, target, name, values); }
static void APIENTRY QxpGetTexGenfv(GLenum coordinate, GLenum name,
                                    GLfloat *values)
{ get_named_2f(FEnum_glGetTexGenfv, coordinate, name, values); }
static void APIENTRY QxpGetTexGeniv(GLenum coordinate, GLenum name,
                                    GLint *values)
{ get_named_2i(FEnum_glGetTexGeniv, coordinate, name, values); }

static void APIENTRY QxpGetTexGendv(GLenum coordinate, GLenum name,
                                    GLdouble *values)
{
    uint32_t arguments[] = {
        coordinate, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_call_counted_output(
        FEnum_glGetTexGendv, arguments, 3, values, sizeof(*values), 4);
}

static void named_vector_1f(uint32_t function, GLenum name,
                            const GLfloat *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_submit_input(function, arguments, 2, values,
        (unsigned)szglname(name) * sizeof(*values));
}

static void named_vector_1i(uint32_t function, GLenum name,
                            const GLint *values)
{
    uint32_t arguments[] = { name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_submit_input(function, arguments, 2, values,
        (unsigned)szglname(name) * sizeof(*values));
}

static void named_vector_2f(uint32_t function, GLenum first, GLenum name,
                            const GLfloat *values)
{
    uint32_t arguments[] = { first, name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_submit_input(function, arguments, 3, values,
        (unsigned)szglname(name) * sizeof(*values));
}

static void named_vector_2i(uint32_t function, GLenum first, GLenum name,
                            const GLint *values)
{
    uint32_t arguments[] = { first, name, (uint32_t)(uintptr_t)values };
    if (values) qxp_transport_submit_input(function, arguments, 3, values,
        (unsigned)szglname(name) * sizeof(*values));
}

static void APIENTRY QxpFogfv(GLenum name, const GLfloat *values)
{ named_vector_1f(FEnum_glFogfv, name, values); }
static void APIENTRY QxpFogiv(GLenum name, const GLint *values)
{ named_vector_1i(FEnum_glFogiv, name, values); }
static void APIENTRY QxpLightfv(GLenum light, GLenum name, const GLfloat *values)
{ named_vector_2f(FEnum_glLightfv, light, name, values); }
static void APIENTRY QxpLightiv(GLenum light, GLenum name, const GLint *values)
{ named_vector_2i(FEnum_glLightiv, light, name, values); }
static void APIENTRY QxpLightModelfv(GLenum name, const GLfloat *values)
{ named_vector_1f(FEnum_glLightModelfv, name, values); }
static void APIENTRY QxpLightModeliv(GLenum name, const GLint *values)
{ named_vector_1i(FEnum_glLightModeliv, name, values); }
static void APIENTRY QxpMaterialfv(GLenum face, GLenum name,
                                   const GLfloat *values)
{ named_vector_2f(FEnum_glMaterialfv, face, name, values); }
static void APIENTRY QxpMaterialiv(GLenum face, GLenum name,
                                   const GLint *values)
{ named_vector_2i(FEnum_glMaterialiv, face, name, values); }
static void APIENTRY QxpTexEnvfv(GLenum target, GLenum name,
                                 const GLfloat *values)
{ named_vector_2f(FEnum_glTexEnvfv, target, name, values); }
static void APIENTRY QxpTexEnviv(GLenum target, GLenum name,
                                 const GLint *values)
{ named_vector_2i(FEnum_glTexEnviv, target, name, values); }
static void APIENTRY QxpTexGenfv(GLenum coordinate, GLenum name,
                                 const GLfloat *values)
{ named_vector_2f(FEnum_glTexGenfv, coordinate, name, values); }
static void APIENTRY QxpTexGeniv(GLenum coordinate, GLenum name,
                                 const GLint *values)
{ named_vector_2i(FEnum_glTexGeniv, coordinate, name, values); }
static void APIENTRY QxpTexParameterfv(GLenum target, GLenum name,
                                       const GLfloat *values)
{ named_vector_2f(FEnum_glTexParameterfv, target, name, values); }
static void APIENTRY QxpTexParameteriv(GLenum target, GLenum name,
                                       const GLint *values)
{ named_vector_2i(FEnum_glTexParameteriv, target, name, values); }

static void APIENTRY QxpTexGendv(GLenum coordinate, GLenum name,
                                 const GLdouble *values)
{
    uint32_t arguments[] = {
        coordinate, name, (uint32_t)(uintptr_t)values
    };
    if (values) qxp_transport_submit_input(FEnum_glTexGendv, arguments, 3,
        values, (unsigned)szglname(name) * sizeof(*values));
}

static void APIENTRY QxpCallLists(GLsizei count, GLenum type,
                                  const GLvoid *lists)
{
    uint32_t arguments[] = {
        (uint32_t)count, type, (uint32_t)(uintptr_t)lists
    };
    if (count > 0 && lists) qxp_transport_submit_input(
        FEnum_glCallLists, arguments, 3, lists,
        (unsigned)count * (unsigned)szgldata(0, type));
}

static void APIENTRY QxpTexImage1D(GLenum target, GLint level,
                                   GLint internal_format, GLsizei width,
                                   GLint border, GLenum format, GLenum type,
                                   const GLvoid *pixels)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, (uint32_t)internal_format, (uint32_t)width,
        (uint32_t)border, format, type, (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, 1, format, type, unpack_row_length) : 0;
    qxp_transport_call_fbtm(FEnum_glTexImage1D, arguments, 8, pixels, bytes);
}

static void APIENTRY QxpTexSubImage1D(GLenum target, GLint level, GLint x,
                                      GLsizei width, GLenum format, GLenum type,
                                      const GLvoid *pixels)
{
    uint32_t arguments[] = {
        target, (uint32_t)level, (uint32_t)x, (uint32_t)width, format, type,
        (uint32_t)(uintptr_t)pixels
    };
    unsigned bytes = pixels ? pixel_data_bytes(
        width, 1, format, type, unpack_row_length) : 0;
    qxp_transport_call_fbtm(FEnum_glTexSubImage1D, arguments, 7, pixels, bytes);
}

static void APIENTRY QxpClearColor(GLclampf red, GLclampf green,
                                   GLclampf blue, GLclampf alpha)
{
    uint32_t arguments[] = {
        float_bits(red), float_bits(green), float_bits(blue), float_bits(alpha)
    };
    qxp_transport_submit(FEnum_glClearColor, arguments, 4);
}

static void APIENTRY QxpViewport(GLint x, GLint y, GLsizei width,
                                 GLsizei height)
{
    uint32_t arguments[] = {
        (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height
    };
    qxp_transport_submit(FEnum_glViewport, arguments, 4);
}

static QXP_PROC_TABLE proc_table = {
    GL_DISPATCH_ENTRIES,
    {
#define USE_GL_FUNC(name, proto_args, call_args, offset, stack) \
        [offset] = (PROC)NoOp##name,
#define USE_GL_FUNC_RET(name, ret_type, proto_args, call_args, offset, stack) \
        [offset] = (PROC)NoOp##name,
#include "third_party/reactos/glfuncs.h"
    }
};

static void fill_pixel_format(PIXELFORMATDESCRIPTOR *pfd)
{
    ZeroMemory(pfd, sizeof(*pfd));
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = pfd->cAlphaBits = 8;
    pfd->cDepthBits = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
}

static BOOL ensure_device(void)
{
    LONG current = state;

    if (current == QXP_DEVICE_OK || current == QXP_ADVERTISING ||
        current == QXP_GRANTED || current == QXP_INERT) {
        return TRUE;
    }
    if (current == QXP_DEAD) {
        return FALSE;
    }
    if (!qxp_transport_open() || !qxp_transport_compatible()) {
        InterlockedExchange(&state, QXP_DEAD);
        return FALSE;
    }
    owner_token = (uint16_t)GetCurrentProcessId();
    if (!owner_token) owner_token = 1;
    fill_pixel_format(&advertised_pfd);
    InterlockedExchange(&state, QXP_DEVICE_OK);
    return TRUE;
}

BOOL APIENTRY DrvValidateVersion(ULONG version)
{
    (void)version;
    return ensure_device();
}

LONG APIENTRY DrvDescribePixelFormat(HDC hdc, INT format, ULONG bytes,
                                     PIXELFORMATDESCRIPTOR *pfd)
{
    (void)hdc;
    if (!ensure_device() || !qxp_transport_reserve(owner_token)) return 0;
    if (state != QXP_GRANTED) {
        InterlockedExchange(&state, QXP_ADVERTISING);
    }
    if (pfd) {
        if (format != 1 || bytes != sizeof(*pfd)) return 0;
        memcpy(pfd, &advertised_pfd, sizeof(*pfd));
    }
    return 1;
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG format)
{
    (void)hdc;
    if (state != QXP_ADVERTISING || format != 1 ||
        !qxp_transport_reserve(owner_token) ||
        !qxp_transport_bind(format, &advertised_pfd, owner_token)) {
        return FALSE;
    }
    InterlockedExchange(&state, QXP_GRANTED);
    start_heartbeat();
    return TRUE;
}

DHGLRC APIENTRY DrvCreateContext(HDC hdc)
{
    if (InterlockedCompareExchange(&context_exists, 1, 0) != 0) return 0;
    if (state == QXP_INERT) return 1;
    if (state == QXP_DEVICE_OK) {
        if (!qxp_transport_reserve(owner_token) ||
            !qxp_transport_bind(1, &advertised_pfd, owner_token)) {
            InterlockedExchange(&context_exists, 0);
            return 0;
        }
        InterlockedExchange(&state, QXP_GRANTED);
        start_heartbeat();
    }
    reset_client_arrays();
    if (state != QXP_GRANTED ||
        !qxp_transport_create_context((uint32_t)(uintptr_t)hdc)) {
        InterlockedExchange(&context_exists, 0);
        return 0;
    }
    return 1;
}
DHGLRC APIENTRY DrvCreateLayerContext(HDC hdc, int layer)
{ return layer == 0 ? DrvCreateContext(hdc) : 0; }
QXP_PROC_TABLE *APIENTRY DrvSetContext(HDC hdc, DHGLRC context,
                                       PFN_SETPROCTABLE callback)
{
    (void)hdc; (void)callback;
    if (context != 1 || !context_exists ||
        (state != QXP_GRANTED && state != QXP_INERT)) return NULL;
    if (state == QXP_GRANTED && !qxp_transport_make_current(context)) return NULL;
    InterlockedExchange(&current_context, (LONG)context);
    qxp_install_scalar_stubs(proc_table.entries);
    proc_table.entries[3] = (PROC)QxpCallLists;
    proc_table.entries[8] = (PROC)QxpBitmap;
    proc_table.entries[7] = (PROC)QxpBegin;
    proc_table.entries[13] = (PROC)QxpColor3f;
    proc_table.entries[43] = (PROC)QxpEnd;
    proc_table.entries[87] = (PROC)QxpRectdv;
    proc_table.entries[89] = (PROC)QxpRectfv;
    proc_table.entries[91] = (PROC)QxpRectiv;
    proc_table.entries[93] = (PROC)QxpRectsv;
    proc_table.entries[136] = (PROC)QxpVertex3f;
    proc_table.entries[150] = (PROC)QxpClipPlane;
    proc_table.entries[154] = (PROC)QxpFogfv;
    proc_table.entries[156] = (PROC)QxpFogiv;
    proc_table.entries[160] = (PROC)QxpLightfv;
    proc_table.entries[162] = (PROC)QxpLightiv;
    proc_table.entries[164] = (PROC)QxpLightModelfv;
    proc_table.entries[166] = (PROC)QxpLightModeliv;
    proc_table.entries[170] = (PROC)QxpMaterialfv;
    proc_table.entries[172] = (PROC)QxpMaterialiv;
    proc_table.entries[179] = (PROC)QxpTexParameterfv;
    proc_table.entries[181] = (PROC)QxpTexParameteriv;
    proc_table.entries[182] = (PROC)QxpTexImage1D;
    proc_table.entries[183] = (PROC)QxpTexImage2D;
    proc_table.entries[185] = (PROC)QxpTexEnvfv;
    proc_table.entries[187] = (PROC)QxpTexEnviv;
    proc_table.entries[189] = (PROC)QxpTexGendv;
    proc_table.entries[191] = (PROC)QxpTexGenfv;
    proc_table.entries[193] = (PROC)QxpTexGeniv;
    proc_table.entries[194] = (PROC)QxpFeedbackBuffer;
    proc_table.entries[195] = (PROC)QxpSelectBuffer;
    proc_table.entries[196] = (PROC)QxpRenderMode;
    proc_table.entries[203] = (PROC)QxpClear;
    proc_table.entries[206] = (PROC)QxpClearColor;
    proc_table.entries[220] = (PROC)QxpMap1d;
    proc_table.entries[221] = (PROC)QxpMap1f;
    proc_table.entries[222] = (PROC)QxpMap2d;
    proc_table.entries[223] = (PROC)QxpMap2f;
    proc_table.entries[249] = (PROC)QxpPixelStoref;
    proc_table.entries[250] = (PROC)QxpPixelStorei;
    proc_table.entries[251] = (PROC)QxpPixelMapfv;
    proc_table.entries[252] = (PROC)QxpPixelMapuiv;
    proc_table.entries[253] = (PROC)QxpPixelMapusv;
    proc_table.entries[256] = (PROC)QxpReadPixels;
    proc_table.entries[257] = (PROC)QxpDrawPixels;
    proc_table.entries[258] = (PROC)QxpGetBooleanv;
    proc_table.entries[259] = (PROC)QxpGetClipPlane;
    proc_table.entries[260] = (PROC)QxpGetDoublev;
    proc_table.entries[262] = (PROC)QxpGetFloatv;
    proc_table.entries[263] = (PROC)QxpGetIntegerv;
    proc_table.entries[264] = (PROC)QxpGetLightfv;
    proc_table.entries[265] = (PROC)QxpGetLightiv;
    proc_table.entries[266] = (PROC)QxpGetMapdv;
    proc_table.entries[267] = (PROC)QxpGetMapfv;
    proc_table.entries[268] = (PROC)QxpGetMapiv;
    proc_table.entries[269] = (PROC)QxpGetMaterialfv;
    proc_table.entries[270] = (PROC)QxpGetMaterialiv;
    proc_table.entries[271] = (PROC)QxpGetPixelMapfv;
    proc_table.entries[272] = (PROC)QxpGetPixelMapuiv;
    proc_table.entries[273] = (PROC)QxpGetPixelMapusv;
    proc_table.entries[274] = (PROC)QxpGetPolygonStipple;
    proc_table.entries[275] = (PROC)QxpGetString;
    proc_table.entries[276] = (PROC)QxpGetTexEnvfv;
    proc_table.entries[277] = (PROC)QxpGetTexEnviv;
    proc_table.entries[278] = (PROC)QxpGetTexGendv;
    proc_table.entries[279] = (PROC)QxpGetTexGenfv;
    proc_table.entries[280] = (PROC)QxpGetTexGeniv;
    proc_table.entries[281] = (PROC)QxpGetTexImage;
    proc_table.entries[282] = (PROC)QxpGetTexParameterfv;
    proc_table.entries[283] = (PROC)QxpGetTexParameteriv;
    proc_table.entries[284] = (PROC)QxpGetTexLevelParameterfv;
    proc_table.entries[285] = (PROC)QxpGetTexLevelParameteriv;
    proc_table.entries[305] = (PROC)QxpViewport;
    proc_table.entries[308] = (PROC)QxpColorPointer;
    proc_table.entries[309] = (PROC)QxpDisableClientState;
    proc_table.entries[310] = (PROC)QxpDrawArrays;
    proc_table.entries[311] = (PROC)QxpDrawElements;
    proc_table.entries[312] = (PROC)QxpEdgeFlagPointer;
    proc_table.entries[313] = (PROC)QxpEnableClientState;
    proc_table.entries[314] = (PROC)QxpIndexPointer;
    proc_table.entries[317] = (PROC)QxpInterleavedArrays;
    proc_table.entries[318] = (PROC)QxpNormalPointer;
    proc_table.entries[320] = (PROC)QxpTexCoordPointer;
    proc_table.entries[321] = (PROC)QxpVertexPointer;
    proc_table.entries[322] = (PROC)QxpAreTexturesResident;
    proc_table.entries[327] = (PROC)QxpDeleteTextures;
    proc_table.entries[328] = (PROC)QxpGenTextures;
    proc_table.entries[329] = (PROC)QxpGetPointerv;
    proc_table.entries[331] = (PROC)QxpPrioritizeTextures;
    proc_table.entries[332] = (PROC)QxpTexSubImage1D;
    proc_table.entries[333] = (PROC)QxpTexSubImage2D;
    return &proc_table;
}
BOOL APIENTRY DrvReleaseContext(DHGLRC context)
{
    if (context != 1 || !context_exists || current_context != (LONG)context) {
        return FALSE;
    }
    if (state == QXP_GRANTED && !qxp_transport_release_context()) return FALSE;
    InterlockedExchange(&current_context, 0);
    return TRUE;
}
BOOL APIENTRY DrvDeleteContext(DHGLRC context)
{
    if (context != 1 ||
        InterlockedCompareExchange(&context_exists, 0, 1) != 1) return FALSE;
    if (current_context == (LONG)context &&
        state == QXP_GRANTED && !qxp_transport_release_context()) {
        InterlockedExchange(&context_exists, 1);
        return FALSE;
    }
    InterlockedExchange(&current_context, 0);
    stop_heartbeat();
    if (state == QXP_GRANTED) qxp_transport_release();
    InterlockedExchange(&state, QXP_DEVICE_OK);
    return TRUE;
}
BOOL APIENTRY DrvCopyContext(DHGLRC a, DHGLRC b, UINT mask)
{ (void)a; (void)b; (void)mask; return FALSE; }
BOOL APIENTRY DrvShareLists(DHGLRC a, DHGLRC b)
{ (void)a; (void)b; return FALSE; }
PROC APIENTRY DrvGetProcAddress(LPCSTR name) { (void)name; return NULL; }
VOID APIENTRY DrvSetCallbackProcs(INT count, PROC *callbacks)
{ (void)count; (void)callbacks; }
BOOL APIENTRY DrvSwapBuffers(HDC hdc)
{
    (void)hdc;
    if (state == QXP_INERT) return FALSE;
    return state == QXP_GRANTED && current_context != 0 &&
           qxp_transport_swap();
}
BOOL APIENTRY DrvDescribeLayerPlane(HDC h, INT f, INT l, UINT b,
                                    LPLAYERPLANEDESCRIPTOR d)
{ (void)h; (void)f; (void)l; (void)b; (void)d; return FALSE; }
INT APIENTRY DrvSetLayerPaletteEntries(HDC h, INT l, INT s, INT c,
                                       const COLORREF *v)
{ (void)h; (void)l; (void)s; (void)c; (void)v; return 0; }
INT APIENTRY DrvGetLayerPaletteEntries(HDC h, INT l, INT s, INT c, COLORREF *v)
{ (void)h; (void)l; (void)s; (void)c; (void)v; return 0; }
BOOL APIENTRY DrvRealizeLayerPalette(HDC h, INT l, BOOL r)
{ (void)h; (void)l; (void)r; return FALSE; }
BOOL APIENTRY DrvSwapLayerBuffers(HDC h, UINT p)
{ return p == 1 ? DrvSwapBuffers(h) : FALSE; }
BOOL APIENTRY DrvPresentBuffers(HDC h, void *d)
{ (void)h; (void)d; return FALSE; }
HRESULT APIENTRY DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
