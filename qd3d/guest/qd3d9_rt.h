/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest runtime contract for the generated d3d9 proxy (qd3d9_gen.c) and the
 * hand-written overrides. Windows XP x86, MinGW, C99.
 *
 * Every Direct3D object the application sees is a struct qd3d_obj whose
 * first field is the vtable pointer, so the COM interface pointer and the
 * proxy pointer are the same value.
 */
#ifndef QD3D9_RT_H
#define QD3D9_RT_H

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>

#include "../proto/qd3d_proto.h"
#include "../proto/qd3d_ops.h"
#include "../proto/qd3d_sizes.h"

/*
 * Lifetime follows the native runtime: a proxy lives while the application
 * references it (refs) or while something holds it (holds): device bindings,
 * state blocks, a container owning its level surfaces, the device owning its
 * implicit surfaces. A proxy whose public count reaches zero but is still
 * held keeps its id and identity, so Get* returns the same pointer and stale
 * application pointers stay valid, as with native Direct3D 9. Its parent
 * reference then becomes a hold on its parent. A level owned by its
 * container has no count of its own: AddRef and Release act on the
 * container's, as native levels share their texture's count.
 */
struct qd3d_obj {
    const void *const *vtbl;   /* first: COM interface pointer == &obj */
    volatile LONG refs;        /* public COM reference count */
    uint32_t id;               /* wire id (guest- or host-assigned) */
    uint32_t type;             /* enum qd3d_type, always a concrete type */
    struct qd3d_obj *device;   /* owning device (itself for devices, NULL
                                  for IDirect3D9); not referenced */
    struct qd3d_obj *parent;   /* container or NULL: the texture of a level
                                  surface/volume, the device for objects
                                  created by a device, the IDirect3D9 for a
                                  device. Referenced while refs > 0 (not by
                                  an owned level). */
    LONG holds;                /* internal references, under the lock */
    uint8_t parent_hold;       /* refs == 0 and the parent reference is a hold */
    uint8_t owned_level;       /* level owned by its container (parent),
                                  sharing its count */
    uint8_t dying;             /* being destroyed or torn down */
    uint8_t local;             /* no wire object of its own: not in the id
                                  table, sends no RELEASE */
    void *ext;                 /* type-specific state owned by overrides */
    void (*ext_free)(struct qd3d_obj *o); /* releases ext, may be NULL */
};

/*
 * Call builder. qd3d_call_begin takes the process-wide connection lock and
 * reserves exactly 16 + payload bytes in the ring; qd3d_call_done releases
 * the lock. Every qd3d_call_begin is paired with exactly one qd3d_call_end
 * and one qd3d_call_done, in that order. Between end and done a synchronous
 * call reads its reply items with qd3d_get_*.
 *
 * If the channel is unavailable or faulted, the builder discards the
 * command; a synchronous call then returns D3DERR_DRIVERINTERNALERROR and
 * every qd3d_get_* zero-fills its destination and returns 0.
 */
struct qd3d_call {
    uint8_t *dst;              /* write cursor inside the reserved command */
    uint32_t left;             /* reserved payload bytes not yet written */
    uint32_t op;
    uint32_t seq;              /* nonzero for synchronous calls */
    int dead;                  /* nothing is sent and no reply is read */
    const uint8_t *rp;         /* reply cursor */
    uint32_t rleft;
};

void qd3d_call_begin(struct qd3d_call *k, uint32_t op, uint32_t obj, int sync,
                     uint64_t payload_bytes);
void qd3d_put_u32(struct qd3d_call *k, uint32_t v);
/* p == NULL encodes QD3D_DATA_NULL and must be accounted as 4 bytes. */
void qd3d_put_data(struct qd3d_call *k, const void *p, uint64_t nbytes);
/* Returns reply.ret for synchronous calls, 0 for asynchronous ones. */
uint32_t qd3d_call_end(struct qd3d_call *k);
int qd3d_get_u32(struct qd3d_call *k, uint32_t *v);
/* Copies a DATA reply item of exactly nbytes; otherwise zero-fills dst and
 * returns 0 (also for QD3D_DATA_NULL). */
int qd3d_get_data(struct qd3d_call *k, void *dst, uint64_t nbytes);
/* Consumes one DATA reply item without storing it (outputs of a failed
 * call stay as the application left them). */
void qd3d_skip_data(struct qd3d_call *k);
void qd3d_call_done(struct qd3d_call *k);
/* Raw (unprefixed) bytes for hand-written extension payloads: put_raw
 * appends nbytes to the reserved command; get_raw copies exactly nbytes from
 * the reply payload, otherwise zero-fills dst and returns 0. */
void qd3d_put_raw(struct qd3d_call *k, const void *p, uint64_t nbytes);
int qd3d_get_raw(struct qd3d_call *k, void *dst, uint64_t nbytes);

/* Object helpers. iface may be NULL where noted. */
uint32_t qd3d_obj_id(const void *iface);                  /* 0 for NULL */
/* Owning device of a proxy (itself for a device, NULL for IDirect3D9). */
struct qd3d_obj *qd3d_obj_device(const void *iface);
/* NEWOBJ: fresh guest id, refs = 1, parent = device (referenced). */
struct qd3d_obj *qd3d_obj_new(struct qd3d_obj *device, uint32_t type);
/* A fresh guest id for an object the guest names without a proxy of its
 * own (HELLO's IDirect3D9); 0 when the id space is exhausted. Lock held. */
uint32_t qd3d_rt_new_id(void);
/* A proxy (refs = 1, no parent) that names wire object id without owning
 * it: not in the id table, never sends RELEASE (local = 1). */
struct qd3d_obj *qd3d_obj_new_local(uint32_t type, uint32_t id);
/* OUTOBJ: for a reply (id, type), AddRef and return the existing proxy for
 * id, or create one (refs = 1) for an id the guest has not seen. NULL for
 * id 0 and when type is not a concrete type deriving from declared_type. */
struct qd3d_obj *qd3d_obj_from_id(struct qd3d_obj *device, uint32_t id,
                                  uint32_t type, uint32_t declared_type);
/* Free a NEWOBJ proxy whose creation failed; sends nothing. */
void qd3d_obj_discard(struct qd3d_obj *o);
/* Internal references (see struct qd3d_obj). unhold destroys the proxy once
 * neither the application nor anything else references it. */
void qd3d_obj_hold(struct qd3d_obj *o);
void qd3d_obj_unhold(struct qd3d_obj *o);

/* Vtables by concrete type (generated). */
extern const void *const *const qd3d9_vtables[QD3D_T_COUNT];

#endif /* QD3D9_RT_H */
