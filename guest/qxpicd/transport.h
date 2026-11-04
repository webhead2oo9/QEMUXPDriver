#ifndef QXPICD_TRANSPORT_H
#define QXPICD_TRANSPORT_H

#include <windows.h>
#include <stdint.h>

enum {
    QXP_PROBE_FREE = 0,
    QXP_PROBE_RESERVED = 1,
    QXP_PROBE_OWNED = 2
};

BOOL qxp_transport_open(void);
BOOL qxp_transport_compatible(void);
uint32_t qxp_transport_probe(void);
BOOL qxp_transport_reserve(uint16_t token);
BOOL qxp_transport_bind(int format, const PIXELFORMATDESCRIPTOR *pfd,
                        uint16_t token);
void qxp_transport_heartbeat(uint16_t token);
void qxp_transport_release(void);
BOOL qxp_transport_create_context(uint32_t hdc);
BOOL qxp_transport_make_current(uint32_t context);
BOOL qxp_transport_release_context(void);
BOOL qxp_transport_submit(uint32_t function, const uint32_t *arguments,
                          unsigned count);
BOOL qxp_transport_submit_input(uint32_t function,
                                const uint32_t *arguments,
                                unsigned count, const void *input,
                                unsigned input_bytes);
uint32_t qxp_transport_call_return(uint32_t function,
                                   const uint32_t *arguments,
                                   unsigned count);
uint32_t qxp_transport_call_return_output(uint32_t function,
                                          const uint32_t *arguments,
                                          unsigned count, void *output,
                                          unsigned output_bytes);
BOOL qxp_transport_call_input(uint32_t function, const uint32_t *arguments,
                              unsigned count, const void *input,
                              unsigned input_bytes);
uint32_t qxp_transport_call_input_output_return(
    uint32_t function, const uint32_t *arguments, unsigned count,
    const void *input, unsigned input_bytes, void *output,
    unsigned output_bytes);
BOOL qxp_transport_call_output(uint32_t function, const uint32_t *arguments,
                               unsigned count, void *output,
                               unsigned output_bytes);
BOOL qxp_transport_call_counted_output(uint32_t function,
                                       const uint32_t *arguments,
                                       unsigned count, void *output,
                                       unsigned element_size,
                                       unsigned maximum_elements);
BOOL qxp_transport_call_fbtm(uint32_t function, const uint32_t *arguments,
                             unsigned count, const void *input,
                             unsigned input_bytes);
BOOL qxp_transport_call_fbtm_output(uint32_t function,
                                    const uint32_t *arguments,
                                    unsigned count, void *output,
                                    unsigned output_bytes,
                                    unsigned output_offset);
BOOL qxp_transport_call_fbtm_counted_output(uint32_t function,
                                            const uint32_t *arguments,
                                            unsigned count, void *output);
BOOL qxp_transport_get_string(uint32_t name, char *destination,
                              unsigned capacity);
BOOL qxp_transport_swap(void);

#endif
