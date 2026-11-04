/* Mesa pass-through protocol derived from qemu-3dfx wrapgl32.c (GPLv2). */
#include "transport.h"

#include <limits.h>
#include <string.h>
#include <winioctl.h>

#include "../qxpmap/qxpmap_ioctl.h"
#include "mglfunci.h"

#define PAGE_SIZE_ 0x1000U
#define MGLSHM_SIZE 0x03ffc000U
#define MGLFBT_SIZE 0x02000000U
#define MBUFO_SIZE 0x08000000U
#define MESAGL_MAGIC 0x5b5eb5e5U
#define MESA_BUSY 0xb5b5b5b5U
#define MESA_PROBE_TAG 0x3dU
#define MESA_PROBE_VER 1U
#define FIRST_FIFO 24U
#define MAX_FIFO 0x0c0000U
#define MAX_DATA ((MGLSHM_SIZE - (4U * MAX_FIFO) - (4U * 4096U)) >> 2)
#define ALIGNED_1 8U

_Static_assert(sizeof(QXPMAP_WINDOW_REQUEST) == 8, "QXPMAP_WINDOW_REQUEST layout");
_Static_assert(sizeof(QXPMAP_MAPPING) == 16, "QXPMAP_MAPPING layout");

/* qxpmap.sys maps the Mesa windows at their fixed guest-physical addresses.
 * It is not exclusive: the host's 0xFB4 reservation decides which process
 * owns the pass-through, and nothing below writes the FIFO or FBTM before
 * a reservation is won. Every view belongs to this handle, so closing it
 * (process exit included) undoes them all. */
static HANDLE map_device = INVALID_HANDLE_VALUE;
static volatile uint32_t *ptm;
static uint32_t *fifo;
static uint32_t *fbtm;
static void *mbufo;
static volatile LONG transport_lock;
static volatile LONG lease_token;
static DWORD lease_last_tick;

static void lock_transport(void)
{
    DWORD now;
    LONG token;

    while (InterlockedCompareExchange(&transport_lock, 1, 0) != 0) {
        Sleep(0);
    }
    token = InterlockedCompareExchange(&lease_token, 0, 0);
    now = GetTickCount();
    if (ptm && token > 0 && (DWORD)(now - lease_last_tick) >= 500U) {
        ptm[0xfb0U >> 2] = (uint16_t)token;
        lease_last_tick = now;
    }
}

static void unlock_transport(void)
{
    InterlockedExchange(&transport_lock, 0);
}

static BOOL mmio_write_read(uint32_t offset, uint32_t value,
                            uint32_t *result)
{
    DWORD_PTR process_mask;
    DWORD_PTR system_mask;
    DWORD_PTR pin_mask;
    DWORD_PTR previous;

    if (!result ||
        !GetProcessAffinityMask(GetCurrentProcess(), &process_mask,
                                &system_mask) || !process_mask) {
        if (result) *result = 0;
        return FALSE;
    }
    pin_mask = process_mask & (0 - process_mask);
    previous = SetThreadAffinityMask(GetCurrentThread(), pin_mask);
    if (!previous) {
        *result = 0;
        return FALSE;
    }

    ptm[offset >> 2] = value;
    *result = ptm[offset >> 2];
    SetThreadAffinityMask(GetCurrentThread(), previous);
    return TRUE;
}

static BOOL map_window(ULONG window, ULONG size, void **result)
{
    QXPMAP_WINDOW_REQUEST request;
    QXPMAP_MAPPING mapping;
    DWORD returned = 0;

    ZeroMemory(&request, sizeof(request));
    ZeroMemory(&mapping, sizeof(mapping));
    request.Window = window;
    if (!DeviceIoControl(map_device, IOCTL_QXPMAP_MAP_WINDOW, &request,
                         sizeof(request), &mapping, sizeof(mapping),
                         &returned, NULL) ||
        returned != sizeof(mapping) || mapping.Window != window ||
        mapping.Size != size || !mapping.Address ||
        mapping.Address > (ULONGLONG)UINTPTR_MAX - size) {
        return FALSE;
    }
    *result = (void *)(uintptr_t)mapping.Address;
    return TRUE;
}

BOOL qxp_transport_open(void)
{
    void *registers = NULL;
    void *fifo_view = NULL;
    void *fbtm_view = NULL;
    void *mbufo_view = NULL;

    if (ptm) {
        return TRUE;
    }
    map_device = CreateFileA(QXPMAP_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (map_device == INVALID_HANDLE_VALUE ||
        !map_window(QXPMAP_WINDOW_MESA_REGS, PAGE_SIZE_, &registers) ||
        !map_window(QXPMAP_WINDOW_MESA_FIFO, MGLSHM_SIZE, &fifo_view) ||
        !map_window(QXPMAP_WINDOW_MESA_FBTM, MGLFBT_SIZE, &fbtm_view) ||
        !map_window(QXPMAP_WINDOW_MESA_MBUFO, MBUFO_SIZE, &mbufo_view)) {
        if (map_device != INVALID_HANDLE_VALUE) {
            CloseHandle(map_device);
            map_device = INVALID_HANDLE_VALUE;
        }
        return FALSE;
    }
    fifo = fifo_view;
    fbtm = fbtm_view;
    mbufo = mbufo_view;
    ptm = registers;
    return TRUE;
}

uint32_t qxp_transport_probe(void)
{
    return ptm ? ptm[0xfb4U >> 2] : 0;
}

BOOL qxp_transport_compatible(void)
{
    uint32_t probe = qxp_transport_probe();

    return (probe >> 24) == MESA_PROBE_TAG &&
           ((probe >> 20) & 0xfU) == MESA_PROBE_VER;
}

BOOL qxp_transport_reserve(uint16_t token)
{
    uint32_t probe;
    uint32_t state;

    if (!token) {
        token = 1;
    }
    probe = qxp_transport_probe();
    if ((probe >> 24) != MESA_PROBE_TAG ||
        ((probe >> 20) & 0xfU) != MESA_PROBE_VER) {
        return FALSE;
    }
    state = (probe >> 16) & 0xfU;
    if (state == QXP_PROBE_OWNED && (uint16_t)probe == token) {
        return TRUE;
    }
    if (state != QXP_PROBE_FREE &&
        !(state == QXP_PROBE_RESERVED && (uint16_t)probe == token)) {
        return FALSE;
    }
    ptm[0xfb4U >> 2] = token;
    probe = qxp_transport_probe();
    return ((probe >> 16) & 0xfU) == QXP_PROBE_RESERVED &&
           (uint16_t)probe == token;
}

BOOL qxp_transport_bind(int format, const PIXELFORMATDESCRIPTOR *pfd,
                        uint16_t token)
{
    static const uint8_t signature[8] = {
        'Q', 'X', 'P', 'I', 'C', 'D', 0, MESA_PROBE_VER
    };
    uint32_t *scratch;
    uint32_t result;

    if (!ptm || !fifo || !fbtm || !pfd) {
        return FALSE;
    }
    ZeroMemory((uint8_t *)fbtm + MGLFBT_SIZE - 16, 16);
    memcpy((uint8_t *)fbtm + MGLFBT_SIZE - 16, signature, sizeof(signature));
    if (!mmio_write_read(0xfbcU, 0xa0320U, &result) ||
        result != 0x320a0U) {
        return FALSE;
    }

    scratch = (uint32_t *)((uint8_t *)fifo + MGLSHM_SIZE - PAGE_SIZE_);
    ZeroMemory(scratch, 13 * sizeof(*scratch));
    scratch[0] = (uint32_t)format;
    scratch[1] = (uint32_t)(uintptr_t)ptm;
    memcpy(&scratch[2], pfd, sizeof(*pfd));
    scratch[12] = token;
    if (!mmio_write_read(0xfe4U, MESAGL_MAGIC, &result) ||
        result != MESAGL_MAGIC) {
        return FALSE;
    }
    lease_last_tick = GetTickCount();
    InterlockedExchange(&lease_token, token);
    return TRUE;
}

void qxp_transport_heartbeat(uint16_t token)
{
    if (ptm && token) {
        lock_transport();
        ptm[0xfb0U >> 2] = token;
        lease_last_tick = GetTickCount();
        unlock_transport();
    }
}

void qxp_transport_release(void)
{
    if (ptm) {
        InterlockedExchange(&lease_token, 0);
        lock_transport();
        ptm[0xff4U >> 2] = MESAGL_MAGIC;
        ptm[0xfbcU >> 2] = 0xd0320U;
        unlock_transport();
    }
}

BOOL qxp_transport_create_context(uint32_t hdc)
{
    uint32_t *scratch;

    if (!ptm || !fifo) return FALSE;
    lock_transport();
    scratch = (uint32_t *)((uint8_t *)fifo + MGLSHM_SIZE - PAGE_SIZE_);
    scratch[0] = hdc;
    ptm[0xffcU >> 2] = MESAGL_MAGIC;
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_make_current(uint32_t context)
{
    static const char build[] = "QXPICD/1 vertical-slice";
    uint32_t *scratch;

    if (!ptm || !fifo || !context) return FALSE;
    lock_transport();
    scratch = (uint32_t *)((uint8_t *)fifo + MGLSHM_SIZE - PAGE_SIZE_);
    ZeroMemory(scratch, 64);
    scratch[0] = MESAGL_MAGIC;
    memcpy(&scratch[1], build, sizeof(build));
    ptm[0xff8U >> 2] = MESAGL_MAGIC;
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_release_context(void)
{
    uint32_t *scratch;

    if (!ptm || !fifo) return FALSE;
    lock_transport();
    scratch = (uint32_t *)((uint8_t *)fifo + MGLSHM_SIZE - PAGE_SIZE_);
    ZeroMemory(scratch, 64);
    ptm[0xff8U >> 2] = MESAGL_MAGIC;
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_submit(uint32_t function, const uint32_t *arguments,
                          unsigned count)
{
    uint32_t index;
    unsigned i;
    BOOL result = FALSE;

    if (!fifo || count > 16) return FALSE;
    lock_transport();
    index = fifo[0];
    if (index >= FIRST_FIFO && index + count + 1U < MAX_FIFO &&
        fifo[MAX_FIFO] < MAX_DATA) {
        fifo[index++] = function;
        for (i = 0; i < count; ++i) fifo[index++] = arguments[i];
        fifo[0] = index;
        result = TRUE;
    }
    unlock_transport();
    return result;
}

BOOL qxp_transport_submit_input(uint32_t function,
                                const uint32_t *arguments,
                                unsigned count, const void *input,
                                unsigned input_bytes)
{
    uint32_t *data;
    uint32_t fifo_index;
    uint32_t data_index;
    uint32_t words;
    uint32_t next;
    unsigned attempt;
    unsigned i;
    BOOL result = FALSE;

    if (!ptm || !fifo || !input || !input_bytes || count > 16 ||
        input_bytes > UINT_MAX - 3U) return FALSE;
    words = (input_bytes + 3U) >> 2;
    lock_transport();
    data = &fifo[MAX_FIFO];
    for (attempt = 0; attempt < 2; ++attempt) {
        fifo_index = fifo[0];
        data_index = data[0];
        if (data_index <= UINT_MAX - words) {
            next = data_index + words;
            if (next < MAX_DATA && (next & 1U)) ++next;
            if (next < MAX_DATA && fifo_index >= FIRST_FIFO &&
                fifo_index + count + 1U < MAX_FIFO && data_index >= 2U) {
                ZeroMemory(&data[data_index],
                           (next - data_index) * sizeof(uint32_t));
                memcpy(&data[data_index], input, input_bytes);
                data[0] = next;
                fifo[fifo_index++] = function;
                for (i = 0; i < count; ++i)
                    fifo[fifo_index++] = arguments[i];
                fifo[0] = fifo_index;
                result = TRUE;
                break;
            }
        }
        if (attempt ||
            (fifo[0] == FIRST_FIFO && data[0] == (ALIGNED_1 >> 2))) {
            break;
        }
        /* Preserve API order and reclaim both streams together. The direct
         * glFlush doorbell first replays every queued command and its matching
         * input payload, then resets the FIFO/data cursors. */
        ptm[0xfc0U >> 2] = FEnum_glFlush;
    }
    unlock_transport();
    return result;
}

uint32_t qxp_transport_call_return(uint32_t function,
                                   const uint32_t *arguments,
                                   unsigned count)
{
    unsigned i;
    uint32_t result = 0;

    if (!ptm || !fifo || count > 16) return 0;
    lock_transport();
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    if (!mmio_write_read(0xfc0U, function, &result)) result = 0;
    unlock_transport();
    return result;
}

uint32_t qxp_transport_call_return_output(uint32_t function,
                                          const uint32_t *arguments,
                                          unsigned count, void *output,
                                          unsigned output_bytes)
{
    uint8_t *shared_output;
    unsigned i;
    uint32_t result;

    if (!ptm || !fifo || count > 16 ||
        (output_bytes && (!output || output_bytes > 3U * PAGE_SIZE_))) return 0;
    lock_transport();
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    if (!mmio_write_read(0xfc0U, function, &result)) result = 0;
    if (output_bytes) {
        shared_output = (uint8_t *)fifo + MGLSHM_SIZE - 3U * PAGE_SIZE_;
        memcpy(output, shared_output, output_bytes);
    }
    unlock_transport();
    return result;
}

BOOL qxp_transport_call_input(uint32_t function, const uint32_t *arguments,
                              unsigned count, const void *input,
                              unsigned input_bytes)
{
    uint32_t *data;
    uint32_t index;
    uint32_t words;
    uint32_t next;
    unsigned i;
    BOOL result = FALSE;

    if (!ptm || !fifo || !input || !input_bytes || count > 16) return FALSE;
    lock_transport();
    data = &fifo[MAX_FIFO];
    index = data[0];
    words = (input_bytes + 3U) >> 2;
    next = index + words;
    if (next & 1U) ++next;
    if (index >= 2U && next < MAX_DATA) {
        ZeroMemory(&data[index], (next - index) * sizeof(uint32_t));
        memcpy(&data[index], input, input_bytes);
        data[0] = next;
        for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
        ptm[0xfc0U >> 2] = function;
        result = TRUE;
    }
    unlock_transport();
    return result;
}

uint32_t qxp_transport_call_input_output_return(
    uint32_t function, const uint32_t *arguments, unsigned count,
    const void *input, unsigned input_bytes, void *output,
    unsigned output_bytes)
{
    uint8_t *shared_output;
    uint32_t *data;
    uint32_t index, words, next, result = 0;
    unsigned i;

    if (!ptm || !fifo || !input || !input_bytes || !output || !output_bytes ||
        output_bytes > 3U * PAGE_SIZE_ || count > 16) return 0;
    lock_transport();
    data = &fifo[MAX_FIFO];
    index = data[0];
    words = (input_bytes + 3U) >> 2;
    next = index + words;
    if (next & 1U) ++next;
    if (index >= 2U && next < MAX_DATA) {
        ZeroMemory(&data[index], (next - index) * sizeof(uint32_t));
        memcpy(&data[index], input, input_bytes);
        data[0] = next;
        shared_output = (uint8_t *)fifo + MGLSHM_SIZE - 3U * PAGE_SIZE_;
        ZeroMemory(shared_output, output_bytes);
        for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
        if (!mmio_write_read(0xfc0U, function, &result)) result = 0;
        memcpy(output, shared_output, output_bytes);
    }
    unlock_transport();
    return result;
}

BOOL qxp_transport_call_output(uint32_t function, const uint32_t *arguments,
                               unsigned count, void *output,
                               unsigned output_bytes)
{
    uint8_t *shared_output;
    unsigned i;

    if (!ptm || !fifo || !output || !output_bytes ||
        output_bytes > 3U * PAGE_SIZE_ || count > 16) return FALSE;
    lock_transport();
    shared_output = (uint8_t *)fifo + MGLSHM_SIZE - 3U * PAGE_SIZE_;
    ZeroMemory(shared_output, output_bytes);
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    ptm[0xfc0U >> 2] = function;
    memcpy(output, shared_output, output_bytes);
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_call_counted_output(uint32_t function,
                                       const uint32_t *arguments,
                                       unsigned count, void *output,
                                       unsigned element_size,
                                       unsigned maximum_elements)
{
    uint8_t *shared_output;
    uint32_t returned;
    unsigned i;

    if (!ptm || !fifo || !output || !element_size || !maximum_elements ||
        element_size * maximum_elements > 3U * PAGE_SIZE_ - ALIGNED_1 ||
        count > 16) return FALSE;
    lock_transport();
    shared_output = (uint8_t *)fifo + MGLSHM_SIZE - 3U * PAGE_SIZE_;
    ZeroMemory(shared_output, ALIGNED_1 + element_size * maximum_elements);
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    ptm[0xfc0U >> 2] = function;
    returned = *(uint32_t *)shared_output;
    if (returned > maximum_elements) returned = maximum_elements;
    memcpy(output, shared_output + ALIGNED_1, returned * element_size);
    unlock_transport();
    return returned != 0;
}

BOOL qxp_transport_call_fbtm(uint32_t function, const uint32_t *arguments,
                             unsigned count, const void *input,
                             unsigned input_bytes)
{
    uint32_t aligned;
    uint8_t *shared_input;
    unsigned i;

    if (!ptm || !fifo || !fbtm || count > 16 ||
        (input_bytes && !input) || input_bytes > MGLFBT_SIZE) return FALSE;
    aligned = (input_bytes + 7U) & ~7U;
    lock_transport();
    if (input_bytes) {
        shared_input = (uint8_t *)fbtm + MGLFBT_SIZE - aligned;
        ZeroMemory(shared_input, aligned);
        memcpy(shared_input, input, input_bytes);
    }
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    ptm[0xfc0U >> 2] = function;
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_call_fbtm_output(uint32_t function,
                                    const uint32_t *arguments,
                                    unsigned count, void *output,
                                    unsigned output_bytes,
                                    unsigned output_offset)
{
    unsigned i;

    if (!ptm || !fifo || !fbtm || !output || !output_bytes || count > 16 ||
        output_offset > MGLFBT_SIZE ||
        output_bytes > MGLFBT_SIZE - output_offset) return FALSE;
    lock_transport();
    ZeroMemory((uint8_t *)fbtm + output_offset, output_bytes);
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    ptm[0xfc0U >> 2] = function;
    memcpy(output, (uint8_t *)fbtm + output_offset, output_bytes);
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_call_fbtm_counted_output(uint32_t function,
                                            const uint32_t *arguments,
                                            unsigned count, void *output)
{
    uint32_t returned;
    unsigned i;

    if (!ptm || !fifo || !fbtm || !output || count > 16) return FALSE;
    lock_transport();
    *(uint32_t *)fbtm = 0;
    for (i = 0; i < count; ++i) fifo[2 + i] = arguments[i];
    ptm[0xfc0U >> 2] = function;
    returned = *(uint32_t *)fbtm;
    if (!returned || returned > MGLFBT_SIZE - ALIGNED_1) {
        unlock_transport();
        return FALSE;
    }
    memcpy(output, (uint8_t *)fbtm + ALIGNED_1, returned);
    unlock_transport();
    return TRUE;
}

BOOL qxp_transport_get_string(uint32_t name, char *destination,
                              unsigned capacity)
{
    const char *output;

    if (!ptm || !fifo || !destination || capacity < 2) return FALSE;
    lock_transport();
    fifo[2] = name;
    ptm[0xfc0U >> 2] = FEnum_glGetString;
    output = (const char *)((uint8_t *)fifo + MGLSHM_SIZE - 3U * PAGE_SIZE_);
    strncpy(destination, output, capacity - 1U);
    destination[capacity - 1U] = 0;
    unlock_transport();
    return destination[0] != 0;
}

BOOL qxp_transport_swap(void)
{
    uint32_t *result;
    BOOL success;

    if (!ptm || !fifo) return FALSE;
    lock_transport();
    result = (uint32_t *)((uint8_t *)fifo + MGLSHM_SIZE - ALIGNED_1);
    result[0] = 0;
    result[1] = 0;
    ptm[0xff0U >> 2] = MESAGL_MAGIC;
    success = (result[0] & 1U) != 0;
    unlock_transport();
    return success;
}
