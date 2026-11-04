/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal interfaces shared by the hand-written parts of the guest d3d9
 * proxy (runtime, Direct3D front end, resources). Not part of the wire
 * contract.
 */
#ifndef QD3D9_PRIV_H
#define QD3D9_PRIV_H

#include "qd3d9_gen.h"

enum qd3d_transport {
    QD3D_TRANSPORT_NONE = 0,
    QD3D_TRANSPORT_LOOPBACK,
    QD3D_TRANSPORT_VM,
    QD3D_TRANSPORT_MEMORY      /* in-process channel (self-test) */
};

/* ---- runtime (qd3d9_rt.c) ---------------------------------------------- */

/* Once per process before anything else (DllMain or the self-test). */
void qd3d_rt_init(void);
/* Selects a transport, claims a channel slot and maps it on first use
 * (qd3d_proto.h, Transports). Returns 1 when a validated channel is mapped
 * and not dead. The outcome is a qd3d_rt_event. */
int qd3d_rt_connect(void);
/* Gives the slot back and unmaps the channel (DLL unload; at process exit
 * the kernel does both). */
void qd3d_rt_disconnect(void);
/* 1 while a mapped channel is usable. */
int qd3d_rt_alive(void);
enum qd3d_transport qd3d_rt_transport(void);
/* The process-wide connection lock (recursive). */
void qd3d_rt_lock(void);
void qd3d_rt_unlock(void);
/* Non-blocking acquire (1 when taken). */
int qd3d_rt_trylock(void);
/* Limits from the HELLO reply, clamped to the channel geometry. */
void qd3d_rt_set_limits(uint32_t max_cmd, uint32_t max_reply);
/* Largest raw payload of one UPLOAD chunk (after its qd3d_region). */
uint32_t qd3d_rt_max_upload(void);
/* Largest READBACK data_size. */
uint32_t qd3d_rt_max_readback(void);
/* Writes guest_pid and increments session; call inside the HELLO call
 * (between qd3d_call_begin and qd3d_call_end). */
void qd3d_rt_hello_mark(const struct qd3d_call *k);
/* Increments frames_submitted; call between begin and end of PRESENT. */
void qd3d_rt_frame_mark(const struct qd3d_call *k);
/* Advances ctrl->heartbeat (qd3d_proto.h), without the lock: the cursor
 * poller's sign of life while a device that presented lives. */
void qd3d_rt_heartbeat(void);
/* frames_submitted - frames_done (0 without a channel). */
uint32_t qd3d_rt_frames_ahead(void);
/* Waits while more than max_ahead frames are outstanding. */
void qd3d_rt_frame_pace(uint32_t max_ahead);
/* Attaches an in-process channel already initialised by a (fake) host.
 * kick is called for every doorbell. Returns 1 if the channel validated. */
int qd3d_rt_attach_memory(void *base, uint32_t size, void (*kick)(void *ctx), void *ctx);
/* Forgets the current channel (self-test only; nothing is unmapped). */
void qd3d_rt_detach_memory(void);
/* Overrides the no-progress watchdog (milliseconds, 0 = never). */
void qd3d_rt_set_timeout(uint32_t ms);
/* Diagnostics through OutputDebugString (and stderr when QD3D_LOG=stderr). */
void qd3d_rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Rare events (connection, stock fallback, channel death, CreateDevice and
 * Reset failures; never per call or frame): qd3d_rt_log plus one line
 * "YYYY-MM-DD hh:mm:ss pid exe: message" in %TEMP%\qd3d.log, which outlives
 * the process for field diagnosis (emptied once past 1 MiB). */
void qd3d_rt_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- objects (qd3d9_rt.c) ---------------------------------------------- */

static inline struct qd3d_obj *qd3d_obj(const void *iface)
{
    return (struct qd3d_obj *)iface;
}

ULONG qd3d_obj_addref(struct qd3d_obj *o);
ULONG qd3d_obj_release(struct qd3d_obj *o);
/* Replaces o->parent by p (referenced), releasing the previous parent. */
void qd3d_obj_set_parent(struct qd3d_obj *o, struct qd3d_obj *p);
/* Calls fn for every proxy in the id table, lock held (fn must not destroy
 * proxies). */
void qd3d_obj_foreach(void (*fn)(struct qd3d_obj *o, void *ctx), void *ctx);
/* Called with the lock held when a device's public count reaches zero:
 * drops everything the device keeps alive (bindings, the state block being
 * recorded, implicit surfaces), as the native runtime does. */
void qd3d_dev_teardown(struct qd3d_obj *dev);

/* ---- front end (qd3d9_main.c) ------------------------------------------ */

HRESULT qd3d_query_interface(struct qd3d_obj *o, REFIID riid, void **ppv);
int qd3d_iid_equal(REFIID a, REFIID b);
/* Current guest display mode from GDI. */
HRESULT qd3d_current_mode(D3DDISPLAYMODE *m);
/* A resource of dev received guest writes the host has not seen yet (the
 * deferred upload of a managed resource): the next draw flushes what is
 * bound. Called with the runtime lock held or not. */
void qd3d_dev_note_flush(struct qd3d_obj *dev);
/* Flushes the bound resources of dev when qd3d_dev_note_flush asked for it
 * (before anything that reads them on the host). */
void qd3d_dev_flush(struct qd3d_obj *dev);
/* 1 between a successful BeginScene and EndScene. */
int qd3d_dev_in_scene(struct qd3d_obj *dev);
/* The device's caps, or NULL when they could not be read. */
const D3DCAPS9 *qd3d_dev_caps(struct qd3d_obj *dev);
/* Surface validation data for the device-level checks (lock held or not):
 * 0 when the description is unknown. */
struct qd3d_surf_info {
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD usage;
    UINT w, h;
    D3DMULTISAMPLE_TYPE ms;
    int level;                   /* a level of a texture */
    int locked;
};
int qd3d_res_surf_info(const void *surface, struct qd3d_surf_info *si);

/* ---- resources (qd3d9_res.c) ------------------------------------------- */

/* Flags every sub-resource of a surface/volume/texture/buffer proxy as
 * written by the GPU (read back on the next lock). NULL is ignored. */
void qd3d_res_mark_gpu(const void *iface);
/* Uploads what a resource's host copy lacks: the dirty region of a managed
 * resource (native managed resources upload at first use, not at unlock)
 * and the writes so far of a buffer drawn from while mapped. Level surfaces
 * flush their container. NULL is ignored. */
void qd3d_res_flush(const void *iface);
/* 1 when qd3d_res_flush has something to upload (cheap; no lock). */
int qd3d_res_has_pending(const void *iface);
/* Records that surface is a back buffer of the implicit swap chain index
 * (its container for GetContainer); lockable 0 makes locks and GetDC fail
 * as natively without D3DPRESENTFLAG_LOCKABLE_BACKBUFFER, -1 leaves that
 * unknown. */
void qd3d_res_set_swapchain(const void *surface, uint32_t index, int lockable);
/* Records that surface is a back buffer of the additional swap chain sc,
 * which then answers its GetContainer (not referenced: the swap chain holds
 * the surface and calls this with NULL before it goes). */
void qd3d_res_set_sc_container(const void *surface, struct qd3d_obj *sc);

#endif /* QD3D9_PRIV_H */
