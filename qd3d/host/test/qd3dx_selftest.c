/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * QD3D host executor self-test. Drives an in-process executor on a heap
 * channel with commands hand-encoded per qd3d_proto.h:
 *   (a) framing/decode/object/id faults produce the right fault_code, leave
 *       the ring untouched, and qd3dx_reset recovers the channel;
 *   (b) a real path on the host GPU: HELLO, CREATE_DEVICE, Clear, PRESENT
 *       (frame hook pixels), getters returning host ids, UPLOAD/READBACK in
 *       several pools, GetFrontBufferData, QUERY_DATA, RESET, RELEASE and
 *       GOODBYE leaving no live objects;
 *   (c) presented frames reach the frame hook in present order, at most one
 *       frame behind, the last one when the executor goes idle, and before
 *       Reset, GetFrontBufferData, device release and GOODBYE;
 *   (d) FOURCC levels travel in the layout their native lock exposes (ATI1N/
 *       ATI2N full mip chains in every pool, YV12/NV12 planes) and regions
 *       outside it fault without touching native memory; every process heap
 *       still validates afterwards;
 *   (e) host ids stay unique across sessions and wrap past 0x7fffffff
 *       around live ids;
 *   (f) a SYSTEMMEM texture bound to a stage is sampled, GetTexture answers
 *       it, and the allocation after the draw succeeds;
 *   (g) out-of-range indices the native runtime crashes on are answered
 *       without it, and RESET restores the native default device state;
 *   (l) between presents the cursor re-publishes the last frame (never
 *       read back, rate-limited, nothing when it stays off the frame), a
 *       frame in flight takes it along, a PRESENT supersedes it;
 *   (m) frames of minimized or hidden windows are not published, the
 *       embedder hears source_hidden for the source shown and gets its
 *       frame again when the window shows;
 *   (n) qd3dx_alive and the display policy (synthetic times);
 *   (o) cross-thread slot release drops in-flight frames and stale packets,
 *       ends only its displayed source, is harmless idle or repeated, and
 *       preserves a new claimant's HELLO across pending/coalesced releases.
 * Exit status 0 when every check passes.
 *
 * --heap-check runs the same test as a debuggee: a process created under a
 * debugger gets ntdll's debug heap (tail and free checking), so a write past
 * any heap block, including the system-memory levels the native runtime
 * allocates, makes the heap manager report it when the block is validated
 * or freed. That needs no administrator rights, unlike page heap (an Image
 * File Execution Options key under HKLM).
 */
#include <stdio.h>
#include <stdlib.h>

#include "qd3dx_priv.h"

/* ------------------------------------------------------------------------ */
/* Harness                                                                  */
/* ------------------------------------------------------------------------ */

static int g_failures;
static int g_checks;
static int g_verbose_log;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define FRAME_LOG 64
#define FRAME_PIXELS (128u * 128u)

struct frame_rec {
    uint32_t count;
    uint32_t width, height;
    uint32_t first;     /* first pixel, BGRX as little-endian u32 */
    int uniform;        /* all pixels equal first (X byte ignored) */
    /* first pixel (X byte cleared) and size of the first FRAME_LOG frames */
    uint32_t log_first[FRAME_LOG], log_w[FRAME_LOG], log_h[FRAME_LOG];
    /* every pixel (X byte cleared) of the last frame up to FRAME_PIXELS */
    uint32_t px[FRAME_PIXELS];
    uint32_t repeat;    /* repeat kind of the last frame */
};

struct chan {
    uint8_t *base;
    struct qd3d_ctrl *ctrl;
    uint8_t *ring;
    uint32_t ring_size;
    uint8_t *reply;
    struct qd3dx *ex;
    uint32_t seq;
    uint32_t next_id;
    /* Guest screen rectangle present() reports: the last created device's
     * back buffer at the origin, as a fullscreen guest would. */
    int32_t screen[4];
    struct frame_rec frame;
    /* last reply */
    uint32_t ret;
    const uint8_t *rp;
    uint32_t rsize;
    /* Frame hook and source_released calls in order: a frame's first pixel
     * (X byte cleared) or EV_RELEASED. */
    uint32_t ev[64];
    uint32_t nev;
};

#define EV_RELEASED 0xffffffffu
#define EV_HIDDEN 0xfffffffeu

static void ev_add(struct chan *ch, uint32_t v)
{
    if (ch->nev < sizeof ch->ev / sizeof ch->ev[0])
        ch->ev[ch->nev] = v;
    ch->nev++;
}

static void on_source_released(void *user)
{
    ev_add(user, EV_RELEASED);
}

static void on_source_hidden(void *user)
{
    ev_add(user, EV_HIDDEN);
}

static void on_frame(void *user, const struct qd3dx_frame *f)
{
    struct chan *ch = user;
    const uint8_t *p = f->pixels;
    uint32_t x, y, first;

    memcpy(&first, p, 4);
    ev_add(ch, first & 0x00ffffffu);
    ch->frame.count++;
    ch->frame.repeat = f->repeat;
    ch->frame.width = f->width;
    ch->frame.height = f->height;
    ch->frame.first = first;
    ch->frame.uniform = 1;
    for (y = 0; y < f->height && ch->frame.uniform; y++) {
        for (x = 0; x < f->width; x++) {
            uint32_t v;

            memcpy(&v, p + (size_t)y * f->pitch + x * 4u, 4);
            if ((v & 0x00ffffffu) != (first & 0x00ffffffu)) {
                ch->frame.uniform = 0;
                break;
            }
        }
    }
    if (ch->frame.count <= FRAME_LOG) {
        ch->frame.log_first[ch->frame.count - 1u] = first & 0x00ffffffu;
        ch->frame.log_w[ch->frame.count - 1u] = f->width;
        ch->frame.log_h[ch->frame.count - 1u] = f->height;
    }
    if ((uint64_t)f->width * f->height <= FRAME_PIXELS) {
        for (y = 0; y < f->height; y++) {
            for (x = 0; x < f->width; x++) {
                memcpy(&ch->frame.px[y * f->width + x], p + (size_t)y * f->pitch + x * 4u, 4);
                ch->frame.px[y * f->width + x] &= 0x00ffffffu;
            }
        }
    }
}

/* Pixel (x, y) of the last frame the hook saw (X byte cleared). */
static uint32_t frame_px(const struct chan *ch, uint32_t x, uint32_t y)
{
    if (x >= ch->frame.width || y >= ch->frame.height || (uint64_t)ch->frame.width * ch->frame.height > FRAME_PIXELS)
        return 0xdeadbeefu;
    return ch->frame.px[y * ch->frame.width + x];
}

static void on_log(void *user, const char *line)
{
    if (g_verbose_log || strstr(line, "FAULT"))
        printf("  log: %s\n", line);
}

static int chan_open(struct chan *ch)
{
    struct qd3dx_hooks hooks;

    memset(ch, 0, sizeof *ch);
    ch->base = VirtualAlloc(NULL, QD3D_DEFAULT_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ch->base)
        return 0;
    memset(&hooks, 0, sizeof hooks);
    hooks.user = ch;
    hooks.frame = on_frame;
    hooks.log = on_log;
    hooks.source_released = on_source_released;
    hooks.source_hidden = on_source_hidden;
    ch->ex = qd3dx_create(ch->base, QD3D_DEFAULT_SIZE, &hooks);
    if (!ch->ex)
        return 0;
    ch->ctrl = (struct qd3d_ctrl *)ch->base;
    ch->ring = ch->base + ch->ctrl->ring_offset;
    ch->ring_size = ch->ctrl->ring_size;
    ch->reply = ch->base + ch->ctrl->reply_offset;
    ch->next_id = 1;
    return 1;
}

static void chan_close(struct chan *ch)
{
    qd3dx_destroy(ch->ex);
    CHECK(ch->ctrl->host_state == QD3D_HOST_DOWN, "destroy leaves host_state %u", ch->ctrl->host_state);
    VirtualFree(ch->base, 0, MEM_RELEASE);
}

/* Payload builder. */
struct pl {
    uint8_t b[4096];
    uint32_t n;
};

static void pu(struct pl *p, uint32_t v)
{
    memcpy(p->b + p->n, &v, 4);
    p->n += 4;
}

static void pdata(struct pl *p, const void *d, uint32_t n)
{
    if (!d) {
        pu(p, QD3D_DATA_NULL);
        return;
    }
    pu(p, n);
    memcpy(p->b + p->n, d, n);
    memset(p->b + p->n + n, 0, (size_t)(QD3D_PAD4(n) - n));
    p->n += (uint32_t)QD3D_PAD4(n);
}

static void praw(struct pl *p, const void *d, uint32_t n)
{
    memcpy(p->b + p->n, d, n);
    p->n += n;
}

/* Writes one command (header size field may lie: hdr_size) and publishes
 * stride(real) bytes. Inserts a PAD when the command would straddle. */
static void put_cmd(struct chan *ch, uint32_t op, uint32_t obj, uint32_t seq, const void *payload,
                    uint32_t len, uint32_t hdr_size)
{
    struct qd3d_cmd c;
    uint32_t head = ch->ctrl->head, off = head & (ch->ring_size - 1u);
    uint32_t stride = QD3D_CMD_STRIDE(16u + len);

    if (ch->ring_size - off < stride) {
        c.size = ch->ring_size - off;
        c.op = QD3D_OP_PAD;
        c.obj = 0;
        c.seq = 0;
        memcpy(ch->ring + off, &c, sizeof c);
        head += c.size;
        off = 0;
    }
    c.size = hdr_size;
    c.op = op;
    c.obj = obj;
    c.seq = seq;
    memcpy(ch->ring + off, &c, sizeof c);
    if (len)
        memcpy(ch->ring + off + 16u, payload, len);
    MemoryBarrier();
    ch->ctrl->head = head + stride;
}

/* Runs the executor until the ring drains or faults. Returns 0 on success,
 * the fault code otherwise. */
static uint32_t run(struct chan *ch)
{
    int guard = 0;

    while (ch->ctrl->tail != ch->ctrl->head && guard++ < 100000) {
        if (qd3dx_poll(ch->ex, 64) < 0)
            return ch->ctrl->fault_code ? ch->ctrl->fault_code : 0xffffffffu;
    }
    return 0;
}

/* The embedder going idle (qd3dx.h): qd3dx_prepare_wait publishes the frame
 * still in flight. Returns 1 when the executor may sleep (empty ring). */
static int idle(struct chan *ch)
{
    int r = qd3dx_prepare_wait(ch->ex);

    qd3dx_wake(ch->ex);
    return r == 0;
}

/* Sends a well-formed command; for sync ones captures the reply. */
static uint32_t call(struct chan *ch, uint32_t op, uint32_t obj, int sync, const struct pl *p)
{
    uint32_t seq = sync ? ++ch->seq : 0u;
    uint32_t len = p ? p->n : 0u, f;
    struct qd3d_reply r;

    put_cmd(ch, op, obj, seq, p ? p->b : NULL, len, 16u + len);
    f = run(ch);
    ch->ret = 0xdeadbeefu;
    ch->rp = NULL;
    ch->rsize = 0;
    if (f || !sync)
        return f;
    if (ch->ctrl->sync_done != seq)
        return 0xfffffffeu;
    memcpy(&r, ch->reply, sizeof r);
    if (r.seq != seq)
        return 0xfffffffdu;
    ch->ret = r.ret;
    ch->rp = ch->reply + sizeof r;
    ch->rsize = r.size;
    return 0;
}

static uint32_t reply_u32(const struct chan *ch, uint32_t index)
{
    uint32_t v = 0;

    if ((index + 1u) * 4u <= ch->rsize)
        memcpy(&v, ch->rp + index * 4u, 4);
    return v;
}

static uint32_t new_id(struct chan *ch)
{
    return ch->next_id++;
}

/* ------------------------------------------------------------------------ */
/* Session helpers                                                          */
/* ------------------------------------------------------------------------ */

struct session {
    uint32_t d3d, dev;
};

static int hello(struct chan *ch, struct session *s)
{
    struct pl p = { .n = 0 };
    struct qd3d_hello h;

    s->d3d = new_id(ch);
    h.abi = QD3D_ABI_VERSION;
    h.pid = GetCurrentProcessId();
    h.sdk_version = D3D_SDK_VERSION;
    h.d3d_id = s->d3d;
    praw(&p, &h, sizeof h);
    if (call(ch, QD3D_OP_HELLO, 0, 1, &p))
        return 0;
    return ch->ret == (uint32_t)S_OK && ch->rsize == sizeof(struct qd3d_hello_reply);
}

static struct qd3d_wire_pp make_pp(uint32_t w, uint32_t h)
{
    struct qd3d_wire_pp pp;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = 0x1234;
    pp.Windowed = 1;
    pp.EnableAutoDepthStencil = 1;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    return pp;
}

static int create_device_pp(struct chan *ch, struct session *s, const struct qd3d_wire_pp *pp)
{
    struct pl p = { .n = 0 };
    struct qd3d_create_device cd;

    s->dev = new_id(ch);
    ch->screen[0] = ch->screen[1] = 0;
    ch->screen[2] = (int32_t)pp->BackBufferWidth;
    ch->screen[3] = (int32_t)pp->BackBufferHeight;
    memset(&cd, 0, sizeof cd);
    cd.device_id = s->dev;
    cd.adapter = 0;
    cd.device_type = D3DDEVTYPE_HAL;
    cd.behavior_flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE;
    cd.pp = *pp;
    praw(&p, &cd, sizeof cd);
    if (call(ch, QD3D_OP_CREATE_DEVICE, s->d3d, 1, &p))
        return 0;
    return ch->ret == (uint32_t)S_OK && ch->rsize == sizeof(struct qd3d_wire_pp);
}

static int create_device(struct chan *ch, struct session *s, uint32_t w, uint32_t h)
{
    struct qd3d_wire_pp pp = make_pp(w, h);

    return create_device_pp(ch, s, &pp);
}

static uint32_t clear(struct chan *ch, uint32_t dev, D3DCOLOR color)
{
    struct pl p = { .n = 0 };

    pu(&p, 0);                 /* Count */
    pdata(&p, NULL, 0);        /* pRects */
    pu(&p, D3DCLEAR_TARGET);
    pu(&p, color);
    pu(&p, qd3d_f2u(1.0f));
    pu(&p, 0);
    return call(ch, QD3D_OP_IDirect3DDevice9_Clear, dev, 0, &p);
}

static uint32_t present(struct chan *ch, uint32_t obj)
{
    struct pl p = { .n = 0 };
    struct qd3d_present pr;

    memset(&pr, 0, sizeof pr);
    memcpy(pr.screen, ch->screen, sizeof pr.screen);
    praw(&p, &pr, sizeof pr);
    ch->ctrl->frames_submitted++;
    return call(ch, QD3D_OP_PRESENT, obj, 0, &p);
}

static struct qd3d_region region(uint32_t kind, uint32_t level, uint32_t face)
{
    struct qd3d_region r;

    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.level = level;
    r.face = face;
    r.d = 1;
    r.h = 1;
    return r;
}

static uint32_t upload(struct chan *ch, uint32_t obj, const struct qd3d_region *r, const void *data)
{
    static uint8_t buf[sizeof(struct qd3d_region) + (1u << 20)];
    uint32_t len = (uint32_t)sizeof *r + r->data_size;

    if (r->data_size > sizeof buf - sizeof *r)
        return 0xfffffff2u;
    memcpy(buf, r, sizeof *r);
    memcpy(buf + sizeof *r, data, r->data_size);
    put_cmd(ch, QD3D_OP_UPLOAD, obj, 0, buf, len, 16u + len);
    return run(ch);
}

/* READBACK; returns 0 and the bytes in out when ret == S_OK. */
static uint32_t readback(struct chan *ch, uint32_t obj, const struct qd3d_region *r, void *out)
{
    struct pl p = { .n = 0 };
    uint32_t f;

    praw(&p, r, sizeof *r);
    f = call(ch, QD3D_OP_READBACK, obj, 1, &p);
    if (f)
        return f;
    if (ch->ret != (uint32_t)S_OK)
        return 0xfffffff0u;
    if (ch->rsize != r->data_size)
        return 0xfffffff1u;
    memcpy(out, ch->rp, r->data_size);
    return 0;
}

static uint32_t create_simple(struct chan *ch, uint32_t op, uint32_t dev, const uint32_t *args, int nargs,
                              uint32_t *id, int shared)
{
    struct pl p = { .n = 0 };
    int i;

    for (i = 0; i < nargs; i++)
        pu(&p, args[i]);
    *id = new_id(ch);
    pu(&p, *id);
    if (shared)
        pu(&p, 0); /* pSharedHandle absent */
    return call(ch, op, dev, 1, &p);
}

/* ------------------------------------------------------------------------ */
/* (a) Fault tests                                                          */
/* ------------------------------------------------------------------------ */

static void expect_fault(struct chan *ch, const char *name, uint32_t got, uint32_t want, uint32_t tail_before)
{
    CHECK(got == want, "%s: fault %u, expected %u", name, got, want);
    CHECK(ch->ctrl->host_state == QD3D_HOST_FAULT, "%s: host_state %u", name, ch->ctrl->host_state);
    CHECK(ch->ctrl->fault_code == want, "%s: ctrl fault_code %u", name, ch->ctrl->fault_code);
    CHECK(ch->ctrl->tail == tail_before, "%s: ring advanced past the faulting command", name);
    CHECK(qd3dx_poll(ch->ex, 16) == -1, "%s: poll after fault must return -1", name);
    qd3dx_reset(ch->ex);
    CHECK(ch->ctrl->host_state == QD3D_HOST_READY && ch->ctrl->tail == ch->ctrl->head &&
          ch->ctrl->fault_code == 0 && qd3dx_live_objects(ch->ex) == 0,
          "%s: reset did not recover the channel", name);
    printf("  ok  fault %-34s -> %u\n", name, want);
}

/* Sends a raw command and expects a fault. */
static void fault_raw(struct chan *ch, const char *name, uint32_t op, uint32_t obj, uint32_t seq,
                      const struct pl *p, uint32_t hdr_size, uint32_t want)
{
    uint32_t tail = ch->ctrl->tail;
    uint32_t len = p ? p->n : 0u;

    put_cmd(ch, op, obj, seq, p ? p->b : NULL, len, hdr_size);
    expect_fault(ch, name, run(ch), want, tail);
}

static void clear_payload(struct pl *p, uint32_t count, const D3DRECT *rects, uint32_t nrects)
{
    p->n = 0;
    pu(p, count);
    pdata(p, rects, nrects * (uint32_t)sizeof *rects);
    pu(p, D3DCLEAR_TARGET);
    pu(p, 0);
    pu(p, qd3d_f2u(1.0f));
    pu(p, 0);
}

static void test_faults(void)
{
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    D3DRECT rc = { 0, 0, 4, 4 };
    uint32_t tail, id, args[4];
    struct qd3d_region r;
    uint8_t data[64];

    printf("[faults]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(ch.ctrl->magic == QD3D_MAGIC && ch.ctrl->abi == QD3D_ABI_VERSION &&
          ch.ctrl->host_state == QD3D_HOST_READY && ch.ctrl->ring_offset == QD3D_CTRL_SIZE &&
          ch.ctrl->ring_size == QD3D_DEFAULT_RING && ch.ctrl->reply_size == QD3D_DEFAULT_REPLY &&
          ch.ctrl->total_size == QD3D_DEFAULT_SIZE,
          "channel geometry");
    CHECK(qd3dx_poll(ch.ex, 16) == 0, "empty ring polls 0");
    CHECK(qd3dx_prepare_wait(ch.ex) == 0 && ch.ctrl->host_idle == 1, "idle when empty");
    qd3dx_wake(ch.ex);
    CHECK(ch.ctrl->host_idle == 0, "wake clears host_idle");

    /* Framing without any session. */
    clear_payload(&p, 0, NULL, 0);
    fault_raw(&ch, "size below header", QD3D_OP_IDirect3DDevice9_Clear, 1, 0, NULL, 8, QD3D_FAULT_FRAMING);
    fault_raw(&ch, "size beyond published bytes", QD3D_OP_IDirect3DDevice9_Clear, 1, 0, NULL, 256,
              QD3D_FAULT_FRAMING);
    fault_raw(&ch, "size beyond ring_size/2", QD3D_OP_IDirect3DDevice9_Clear, 1, 0, NULL,
              QD3D_DEFAULT_RING / 2u + 16u, QD3D_FAULT_FRAMING);
    fault_raw(&ch, "PAD not reaching ring end", QD3D_OP_PAD, 0, 0, NULL, 16, QD3D_FAULT_FRAMING);
    fault_raw(&ch, "unknown extension op", 30, 0, 0, NULL, 16, QD3D_FAULT_UNEXPECTED);
    fault_raw(&ch, "op beyond method range", QD3D_OP_METHOD_BASE + QD3D_OP_METHOD_COUNT, 0, 0, NULL, 16,
              QD3D_FAULT_FRAMING);
    fault_raw(&ch, "guest-local method (AddRef)", QD3D_OP_IUnknown_AddRef, 1, 0, NULL, 16,
              QD3D_FAULT_UNEXPECTED);
    fault_raw(&ch, "guest-local method (LockRect)", QD3D_OP_IDirect3DSurface9_LockRect, 1, 1, NULL, 16,
              QD3D_FAULT_UNEXPECTED);
    fault_raw(&ch, "extension method via generic op", QD3D_OP_IDirect3DDevice9_Present, 1, 0, NULL, 16,
              QD3D_FAULT_UNEXPECTED);
    fault_raw(&ch, "bad id (Clear on 999)", QD3D_OP_IDirect3DDevice9_Clear, 999, 0, &p, 16u + p.n,
              QD3D_FAULT_OBJECT);
    fault_raw(&ch, "RELEASE of unknown id", QD3D_OP_RELEASE, 77, 0, NULL, 16, QD3D_FAULT_OBJECT);
    fault_raw(&ch, "sync HELLO without seq", QD3D_OP_HELLO, 0, 0, NULL, 16, QD3D_FAULT_FRAMING);
    {
        struct pl hp = { .n = 0 };
        struct qd3d_hello h = { QD3D_ABI_VERSION, 1, D3D_SDK_VERSION, QD3D_ID_HOST_BIT | 5u };

        praw(&hp, &h, sizeof h);
        fault_raw(&ch, "HELLO with host-range id", QD3D_OP_HELLO, 0, 1, &hp, 16u + hp.n, QD3D_FAULT_ID);
        hp.n -= 4;
        fault_raw(&ch, "HELLO short payload", QD3D_OP_HELLO, 0, 1, &hp, 16u + hp.n, QD3D_FAULT_DECODE);
    }

    /* Faults that need live objects. */
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    fault_raw(&ch, "wrong object type (Clear on D3D9)", QD3D_OP_IDirect3DDevice9_Clear, s.d3d, 0, &p,
              16u + p.n, QD3D_FAULT_OBJECT);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    clear_payload(&p, 0, NULL, 0);
    pu(&p, 0); /* trailing junk */
    fault_raw(&ch, "trailing payload bytes", QD3D_OP_IDirect3DDevice9_Clear, s.dev, 0, &p, 16u + p.n,
              QD3D_FAULT_DECODE);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    clear_payload(&p, 2, &rc, 1);
    fault_raw(&ch, "rect count disagrees with DATA", QD3D_OP_IDirect3DDevice9_Clear, s.dev, 0, &p, 16u + p.n,
              QD3D_FAULT_SIZE);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    p.n = 0;
    pu(&p, 64);
    pu(&p, 0);
    pu(&p, 0);
    pu(&p, D3DPOOL_SYSTEMMEM);
    pu(&p, s.dev); /* NEWOBJ id not above the session maximum */
    pu(&p, 0);
    fault_raw(&ch, "stale NEWOBJ id", QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, 1, &p, 16u + p.n,
              QD3D_FAULT_ID);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    p.n = 4u * 4u + 4u * 14u;
    memset(p.b, 0, p.n);
    memcpy(p.b, &s.d3d, 4); /* device_id == the IDirect3D9 id: reused */
    fault_raw(&ch, "CREATE_DEVICE reused id", QD3D_OP_CREATE_DEVICE, s.d3d, 1, &p, 16u + p.n, QD3D_FAULT_ID);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    args[0] = 64; args[1] = 0; args[2] = 0; args[3] = D3DPOOL_SYSTEMMEM;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &id, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "fixture vertex buffer");
    memset(data, 0x5a, sizeof data);
    r = region(QD3D_UPLOAD_BUFFER, 0, 0);
    r.x = 0;
    r.w = 64;
    r.data_size = 64;
    {
        uint8_t buf[sizeof r + 64];

        memcpy(buf, &r, sizeof r);
        memcpy(buf + sizeof r, data, 60);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_UPLOAD, id, 0, buf, sizeof r + 60, 16u + (uint32_t)sizeof r + 60u);
        expect_fault(&ch, "UPLOAD data_size vs payload", run(&ch), QD3D_FAULT_SIZE, tail);
    }

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &id, 1) == 0,
          "fixture vertex buffer");
    r.x = 32;
    r.w = 64;
    r.data_size = 64;
    tail = ch.ctrl->tail;
    expect_fault(&ch, "UPLOAD past buffer end", upload(&ch, id, &r, data), QD3D_FAULT_SIZE, tail);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &id, 1) == 0,
          "fixture vertex buffer");
    r.x = 0;
    r.w = 16;
    r.data_size = 16;
    r.lock_flags = QD3D_UPLOAD_CONTINUE;
    tail = ch.ctrl->tail;
    expect_fault(&ch, "UPLOAD CONTINUE without first chunk", upload(&ch, id, &r, data), QD3D_FAULT_DECODE,
                 tail);

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "fault fixture setup");
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 4;
    r.h = 4;
    r.row_pitch = 16;
    r.data_size = 64;
    tail = ch.ctrl->tail;
    expect_fault(&ch, "UPLOAD to a device id", upload(&ch, s.dev, &r, data), QD3D_FAULT_OBJECT, tail);

    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (b) GPU path                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t get_back_buffer(struct chan *ch, uint32_t dev, uint32_t *type)
{
    struct pl p = { .n = 0 };

    pu(&p, 0);
    pu(&p, 0);
    pu(&p, D3DBACKBUFFER_TYPE_MONO);
    pu(&p, 1);
    if (call(ch, QD3D_OP_IDirect3DDevice9_GetBackBuffer, dev, 1, &p) || ch->ret != (uint32_t)S_OK ||
        ch->rsize != 8)
        return 0;
    *type = reply_u32(ch, 1);
    return reply_u32(ch, 0);
}

static void release(struct chan *ch, uint32_t id)
{
    CHECK(call(ch, QD3D_OP_RELEASE, id, 0, NULL) == 0, "RELEASE 0x%08x", id);
}

static void test_gpu(void)
{
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    uint32_t bb, bb2, type, lvl, rt, tex, vb, dvb, vtex, dxt, plain, query, mem, i, f;
    uint32_t args[9];
    struct qd3d_region r;
    static uint8_t data[65536], back[65536];
    D3DADAPTER_IDENTIFIER9 ident;
    const uint32_t color = 0x00336699u; /* X8R8G8B8: R=0x33 G=0x66 B=0x99 */

    printf("[gpu]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    g_verbose_log = 1;
    CHECK(hello(&ch, &s), "HELLO ret 0x%08x size %u", ch.ret, ch.rsize);
    if (ch.ret != (uint32_t)S_OK) {
        chan_close(&ch);
        return;
    }
    CHECK(reply_u32(&ch, 0) == QD3D_ABI_VERSION && reply_u32(&ch, 2) == QD3D_DEFAULT_RING / 2u &&
          reply_u32(&ch, 3) == QD3D_DEFAULT_REPLY - 16u, "hello reply fields");
    CHECK(qd3dx_session_pid(ch.ex) == GetCurrentProcessId(), "session pid");

    /* GetAdapterIdentifier: generic sync method with an OUTDATA reply. The
     * wire carries the 1100-byte i686 layout (the x64 struct only adds tail
     * padding), so the reply item ends at WHQLLevel. */
    p.n = 0;
    pu(&p, 0);
    pu(&p, 0);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3D9_GetAdapterIdentifier, s.d3d, 1, &p) == 0 && ch.ret == (uint32_t)S_OK &&
          reply_u32(&ch, 0) == offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof(DWORD),
          "GetAdapterIdentifier");
    memset(&ident, 0, sizeof ident);
    if (ch.rsize >= 4u + offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof(DWORD))
        memcpy(&ident, ch.rp + 4, offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof(DWORD));
    ident.Description[sizeof ident.Description - 1] = 0;
    printf("  adapter: %s (vendor 0x%04lx device 0x%04lx)\n", ident.Description, (unsigned long)ident.VendorId,
           (unsigned long)ident.DeviceId);

    CHECK(create_device(&ch, &s, 64, 64), "CREATE_DEVICE ret 0x%08x", ch.ret);
    {
        struct qd3d_wire_pp acc;

        memcpy(&acc, ch.rp, sizeof acc);
        CHECK(acc.BackBufferWidth == 64 && acc.BackBufferHeight == 64 && acc.BackBufferFormat == D3DFMT_X8R8G8B8 &&
              acc.BackBufferCount == 1 && acc.hDeviceWindow == 0x1234 && acc.Windowed == 1 &&
              acc.PresentationInterval == D3DPRESENT_INTERVAL_ONE, "accepted present parameters");
    }

    /* Texture memory clamp. */
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetAvailableTextureMem, s.dev, 1, NULL) == 0, "GetAvailableTextureMem");
    mem = ch.ret;
    CHECK(mem > 0 && mem <= 512u * 1024u * 1024u, "texture memory %u not clamped", mem);

    /* ValidateDevice: one pass for the default state and for a stage that
     * selects an unbound texture, whatever the driver says natively (current
     * drivers answer E_FAIL for every state; D3D8-era games abort on it). */
    p.n = 0;
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_ValidateDevice, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)D3D_OK &&
              reply_u32(&ch, 0) == 4u && reply_u32(&ch, 1) >= 1u,
          "ValidateDevice default state ret 0x%08x passes %u", ch.ret, reply_u32(&ch, 1));
    {
        static const uint32_t tss[][2] = {
            { D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { D3DTSS_COLORARG1, D3DTA_TEXTURE },
            { D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { D3DTSS_ALPHAARG1, D3DTA_TEXTURE },
        };

        for (i = 0; i < sizeof tss / sizeof tss[0]; i++) {
            p.n = 0;
            pu(&p, 0);
            pu(&p, tss[i][0]);
            pu(&p, tss[i][1]);
            CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetTextureStageState, s.dev, 0, &p) == 0,
                  "SetTextureStageState %u", tss[i][0]);
        }
    }
    p.n = 0;
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_ValidateDevice, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)D3D_OK &&
              reply_u32(&ch, 0) == 4u && reply_u32(&ch, 1) >= 1u,
          "ValidateDevice SELECTARG1(TEXTURE) unbound ret 0x%08x passes %u", ch.ret, reply_u32(&ch, 1));

    /* Clear + PRESENT -> frame hook sees the colour once the executor goes
     * idle. */
    CHECK(clear(&ch, s.dev, color) == 0, "Clear");
    CHECK(present(&ch, s.dev) == 0 && idle(&ch), "PRESENT");
    CHECK(ch.frame.count == 1 && ch.frame.width == 64 && ch.frame.height == 64 && ch.frame.uniform &&
          (ch.frame.first & 0x00ffffffu) == color,
          "frame: count %u %ux%u uniform %d first 0x%08x", ch.frame.count, ch.frame.width, ch.frame.height,
          ch.frame.uniform, ch.frame.first);
    CHECK(ch.ctrl->frames_done == 1, "frames_done %u", ch.ctrl->frames_done);
    printf("  frame %ux%u first pixel 0x%08x uniform %d\n", ch.frame.width, ch.frame.height, ch.frame.first,
           ch.frame.uniform);

    /* GetBackBuffer -> host id of type surface; second call returns the same id. */
    bb = get_back_buffer(&ch, s.dev, &type);
    CHECK(QD3D_ID_IS_HOST(bb) && type == QD3D_T_IDirect3DSurface9, "GetBackBuffer id 0x%08x type %u", bb, type);
    bb2 = get_back_buffer(&ch, s.dev, &type);
    CHECK(bb2 == bb, "second GetBackBuffer id 0x%08x != 0x%08x", bb2, bb);

    /* READBACK of the (non-lockable) back buffer. */
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 64;
    r.h = 64;
    r.row_pitch = 256;
    r.data_size = 256u * 64u;
    f = readback(&ch, bb, &r, back);
    CHECK(f == 0, "READBACK back buffer: %u ret 0x%08x", f, ch.ret);
    if (f == 0) {
        uint32_t px;

        memcpy(&px, back + 256u * 37u + 4u * 21u, 4);
        CHECK((px & 0x00ffffffu) == color, "back buffer pixel 0x%08x", px);
    }

    /* Managed texture: two UPLOAD chunks into the staging copy, then
     * StretchRect of the default-pool level into a render target. */
    args[0] = 8; args[1] = 8; args[2] = 1; args[3] = 0; args[4] = D3DFMT_A8R8G8B8; args[5] = D3DPOOL_MANAGED;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateTexture, s.dev, args, 6, &tex, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateTexture MANAGED ret 0x%08x", ch.ret);
    for (i = 0; i < 256; i++)
        data[i] = (uint8_t)(i * 7u + 3u);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 8;
    r.h = 4;
    r.row_pitch = 32;
    r.data_size = 128;
    CHECK(upload(&ch, tex, &r, data) == 0, "UPLOAD managed chunk 1");
    r.y = 4;
    r.lock_flags = QD3D_UPLOAD_CONTINUE;
    CHECK(upload(&ch, tex, &r, data + 128) == 0, "UPLOAD managed chunk 2");
    p.n = 0;
    pu(&p, 0);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DTexture9_GetSurfaceLevel, tex, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "GetSurfaceLevel");
    lvl = reply_u32(&ch, 0);
    CHECK(QD3D_ID_IS_HOST(lvl) && reply_u32(&ch, 1) == QD3D_T_IDirect3DSurface9, "level id 0x%08x", lvl);
    args[0] = 8; args[1] = 8; args[2] = D3DFMT_A8R8G8B8; args[3] = D3DMULTISAMPLE_NONE; args[4] = 0; args[5] = 0;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateRenderTarget, s.dev, args, 6, &rt, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateRenderTarget");
    p.n = 0;
    pu(&p, lvl);
    pdata(&p, NULL, 0);
    pu(&p, rt);
    pdata(&p, NULL, 0);
    pu(&p, D3DTEXF_POINT);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_StretchRect, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "StretchRect texture level -> RT ret 0x%08x", ch.ret);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 8;
    r.h = 8;
    r.row_pitch = 40; /* padded pitch on purpose */
    r.data_size = 320;
    f = readback(&ch, rt, &r, back);
    CHECK(f == 0, "READBACK render target: %u", f);
    for (i = 0; f == 0 && i < 8; i++)
        CHECK(memcmp(back + i * 40u, data + i * 32u, 32) == 0, "managed texture row %u differs on the GPU", i);
    /* Level surface READBACK reads the staging copy. */
    r.row_pitch = 32;
    r.data_size = 256;
    CHECK(readback(&ch, lvl, &r, back) == 0 && memcmp(back, data, 256) == 0, "READBACK managed level");

    /* SYSTEMMEM vertex buffer. */
    args[0] = 64; args[1] = 0; args[2] = 0; args[3] = D3DPOOL_SYSTEMMEM;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &vb, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateVertexBuffer SYSTEMMEM");
    r = region(QD3D_UPLOAD_BUFFER, 0, 0);
    r.x = 8;
    r.w = 32;
    r.data_size = 32;
    CHECK(upload(&ch, vb, &r, data + 1000) == 0, "UPLOAD vb");
    CHECK(readback(&ch, vb, &r, back) == 0 && memcmp(back, data + 1000, 32) == 0, "READBACK vb");

    /* DEFAULT dynamic vertex buffer with DISCARD then a continuation. */
    args[0] = 64; args[1] = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY; args[2] = 0; args[3] = D3DPOOL_DEFAULT;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &dvb, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateVertexBuffer DYNAMIC");
    r.x = 0;
    r.w = 32;
    r.data_size = 32;
    r.lock_flags = D3DLOCK_DISCARD;
    CHECK(upload(&ch, dvb, &r, data + 2000) == 0, "UPLOAD dynamic vb");
    r.x = 32;
    r.lock_flags = QD3D_UPLOAD_CONTINUE;
    CHECK(upload(&ch, dvb, &r, data + 2032) == 0, "UPLOAD dynamic vb continue");
    r.x = 0;
    r.w = 64;
    r.data_size = 64;
    r.lock_flags = 0;
    CHECK(readback(&ch, dvb, &r, back) == 0 && memcmp(back, data + 2000, 64) == 0, "READBACK dynamic vb");

    /* SYSTEMMEM volume texture. */
    args[0] = 4; args[1] = 4; args[2] = 2; args[3] = 1; args[4] = 0; args[5] = D3DFMT_A8R8G8B8;
    args[6] = D3DPOOL_SYSTEMMEM;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVolumeTexture, s.dev, args, 7, &vtex, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateVolumeTexture");
    r = region(QD3D_UPLOAD_VOLUME, 0, 0);
    r.w = 4;
    r.h = 4;
    r.d = 2;
    r.row_pitch = 16;
    r.slice_pitch = 64;
    r.data_size = 128;
    CHECK(upload(&ch, vtex, &r, data + 3000) == 0, "UPLOAD volume");
    CHECK(readback(&ch, vtex, &r, back) == 0 && memcmp(back, data + 3000, 128) == 0, "READBACK volume");

    /* Managed DXT1 texture with a full chain; level 2 is 2x2 (one block,
     * region rounded up to 4x4). */
    args[0] = 8; args[1] = 8; args[2] = 0; args[3] = 0; args[4] = D3DFMT_DXT1; args[5] = D3DPOOL_MANAGED;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateTexture, s.dev, args, 6, &dxt, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateTexture DXT1 MANAGED ret 0x%08x", ch.ret);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 8;
    r.h = 8;
    r.row_pitch = 16;
    r.data_size = 32;
    CHECK(upload(&ch, dxt, &r, data + 4000) == 0, "UPLOAD dxt level 0");
    CHECK(readback(&ch, dxt, &r, back) == 0 && memcmp(back, data + 4000, 32) == 0, "READBACK dxt level 0");
    r = region(QD3D_UPLOAD_SURFACE, 2, 0);
    r.w = 4;
    r.h = 4;
    r.row_pitch = 8;
    r.data_size = 8;
    CHECK(upload(&ch, dxt, &r, data + 4100) == 0, "UPLOAD dxt level 2 (rounded)");
    CHECK(readback(&ch, dxt, &r, back) == 0 && memcmp(back, data + 4100, 8) == 0, "READBACK dxt level 2");

    /* GetFrontBufferData returns the last presented frame. */
    args[0] = 64; args[1] = 64; args[2] = D3DFMT_A8R8G8B8; args[3] = D3DPOOL_SYSTEMMEM;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, s.dev, args, 4, &plain, 1) == 0 &&
          ch.ret == (uint32_t)S_OK, "CreateOffscreenPlainSurface");
    p.n = 0;
    pu(&p, 0);
    pu(&p, plain);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetFrontBufferData, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "GetFrontBufferData ret 0x%08x", ch.ret);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 64;
    r.h = 64;
    r.row_pitch = 256;
    r.data_size = 256u * 64u;
    f = readback(&ch, plain, &r, back);
    CHECK(f == 0, "READBACK front buffer copy");
    if (f == 0) {
        uint32_t px;

        memcpy(&px, back + 256u * 63u + 4u * 63u, 4);
        CHECK(px == (0xff000000u | color), "front buffer pixel 0x%08x", px);
    }

    /* Event query through QUERY_DATA. */
    p.n = 0;
    pu(&p, D3DQUERYTYPE_EVENT);
    query = new_id(&ch);
    pu(&p, query);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_CreateQuery, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "CreateQuery EVENT");
    p.n = 0;
    pu(&p, D3DISSUE_END);
    CHECK(call(&ch, QD3D_OP_IDirect3DQuery9_Issue, query, 0, &p) == 0, "Issue");
    for (i = 0; i < 10000; i++) {
        struct qd3d_query_data qd = { 4, D3DGETDATA_FLUSH };

        p.n = 0;
        praw(&p, &qd, sizeof qd);
        if (call(&ch, QD3D_OP_QUERY_DATA, query, 1, &p) || ch.ret != (uint32_t)S_FALSE)
            break;
        CHECK(ch.rsize == 0, "S_FALSE reply carries no payload");
    }
    CHECK(ch.ret == (uint32_t)S_OK && ch.rsize == 4 && reply_u32(&ch, 0) == TRUE,
          "QUERY_DATA ret 0x%08x size %u after %u polls", ch.ret, ch.rsize, i);

    /* RESET to 32x32 after releasing the implicit back buffer id. */
    {
        uint32_t live = qd3dx_live_objects(ch.ex);

        release(&ch, bb);
        CHECK(qd3dx_live_objects(ch.ex) == live - 1u, "RELEASE drops one id");
    }
    {
        struct qd3d_wire_pp pp = make_pp(32, 32);

        p.n = 0;
        praw(&p, &pp, sizeof pp);
        CHECK(call(&ch, QD3D_OP_RESET, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK && ch.rsize == sizeof pp,
              "RESET ret 0x%08x", ch.ret);
    }
    CHECK(clear(&ch, s.dev, 0x00102030u) == 0 && present(&ch, s.dev) == 0 && idle(&ch), "Clear+PRESENT after RESET");
    CHECK(ch.frame.count == 2 && ch.frame.width == 32 && ch.frame.height == 32 && ch.frame.uniform &&
          (ch.frame.first & 0x00ffffffu) == 0x00102030u, "frame after RESET %ux%u 0x%08x", ch.frame.width,
          ch.frame.height, ch.frame.first);
    bb = get_back_buffer(&ch, s.dev, &type);
    CHECK(QD3D_ID_IS_HOST(bb) && bb != bb2, "new back buffer id after RESET 0x%08x", bb);

    /* RELEASE some ids explicitly (children first, as the guest would). */
    release(&ch, bb);
    release(&ch, lvl);
    release(&ch, rt);
    release(&ch, vb);
    printf("  live objects before GOODBYE: %u\n", qd3dx_live_objects(ch.ex));
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0, "GOODBYE");
    CHECK(qd3dx_live_objects(ch.ex) == 0 && ch.ex->devices == NULL && qd3dx_session_pid(ch.ex) == 0,
          "GOODBYE left %u objects", qd3dx_live_objects(ch.ex));

    /* A second session on the same channel, ended by RELEASE alone. */
    ch.next_id = 1;
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "second session");
    CHECK(clear(&ch, s.dev, 0x00ff0000u) == 0 && present(&ch, s.dev) == 0 && idle(&ch) && ch.frame.count == 3 &&
          (ch.frame.first & 0x00ffffffu) == 0x00ff0000u, "second session frame");

    /* XP-era back buffers: 16-bit and 4x MSAA devices present through the
     * X8R8G8B8 resolve. */
    {
        static const struct { uint32_t fmt, ms; const char *name; } cases[] = {
            { D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, "R5G6B5" },
            { D3DFMT_X1R5G5B5, D3DMULTISAMPLE_NONE, "X1R5G5B5" },
            { D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, "A8R8G8B8 4x MSAA" },
        };
        size_t k;

        for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            struct session s2 = s;
            struct qd3d_wire_pp pp = make_pp(48, 40);
            uint32_t frames = ch.frame.count;

            pp.BackBufferFormat = cases[k].fmt;
            pp.MultiSampleType = cases[k].ms;
            CHECK(create_device_pp(&ch, &s2, &pp), "%s device ret 0x%08x", cases[k].name, ch.ret);
            CHECK(clear(&ch, s2.dev, 0x0000ff00u) == 0 && present(&ch, s2.dev) == 0 && idle(&ch) &&
                  ch.frame.count == frames + 1u && ch.frame.width == 48 && ch.frame.height == 40 &&
                  ch.frame.uniform && (ch.frame.first & 0x00ffffffu) == 0x0000ff00u,
                  "%s frame %ux%u 0x%08x", cases[k].name, ch.frame.width, ch.frame.height, ch.frame.first);
            printf("  ok  %s back buffer presents 0x%08x\n", cases[k].name, ch.frame.first & 0x00ffffffu);
            release(&ch, s2.dev);
        }
    }
    release(&ch, s.dev);
    release(&ch, s.d3d);
    CHECK(qd3dx_live_objects(ch.ex) == 0 && ch.ex->devices == NULL, "RELEASE of every id leaves no objects");
    CHECK(ch.ctrl->host_state == QD3D_HOST_READY, "host_state READY at end");
    g_verbose_log = 0;
    chan_close(&ch);
}

/* GetFrontBufferData into a new w x h system-memory surface; *px receives
 * its centre pixel. */
static int front_buffer_pixel(struct chan *ch, uint32_t dev, uint32_t w, uint32_t h, uint32_t *px)
{
    static uint8_t back[64u * 64u * 4u];
    struct pl p = { .n = 0 };
    struct qd3d_region r;
    uint32_t args[4], plain, ok;

    if (w > 64u || h > 64u)
        return 0;
    args[0] = w; args[1] = h; args[2] = D3DFMT_A8R8G8B8; args[3] = D3DPOOL_SYSTEMMEM;
    if (create_simple(ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, dev, args, 4, &plain, 1) ||
        ch->ret != (uint32_t)S_OK)
        return 0;
    pu(&p, 0);
    pu(&p, plain);
    ok = call(ch, QD3D_OP_IDirect3DDevice9_GetFrontBufferData, dev, 1, &p) == 0 && ch->ret == (uint32_t)S_OK;
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = w;
    r.h = h;
    r.row_pitch = w * 4u;
    r.data_size = w * h * 4u;
    ok = ok && readback(ch, plain, &r, back) == 0;
    release(ch, plain);
    if (ok)
        memcpy(px, back + (size_t)(h / 2u) * w * 4u + (w / 2u) * 4u, 4);
    return (int)ok;
}

/* Frames reach the hook in present order, at most one frame after their
 * PRESENT executed, the last one when the executor goes idle; GetFrontBufferData,
 * Reset, device release and GOODBYE with a frame in flight lose nothing. */
static void test_present(void)
{
    static const uint32_t colors[8] = {
        0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffff00u, 0x0000ffffu, 0x00ff00ffu, 0x00808080u, 0x00404040u,
    };
    struct chan ch;
    struct session s, s2;
    struct pl p = { .n = 0 };
    uint32_t i, n, px = 0;

    printf("[present]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 64, 64), "present session");

    /* A burst: never more than one frame outstanding after a PRESENT has
     * executed; the last frame appears once the executor goes idle. */
    for (i = 0; i < 8u; i++) {
        CHECK(clear(&ch, s.dev, colors[i]) == 0 && present(&ch, s.dev) == 0, "burst PRESENT %u", i);
        CHECK(ch.frame.count >= i && ch.ctrl->frames_submitted - ch.ctrl->frames_done <= 1u,
              "after PRESENT %u: %u frames published, frames_done %u of %u", i, ch.frame.count,
              ch.ctrl->frames_done, ch.ctrl->frames_submitted);
    }
    CHECK(idle(&ch), "idle after the burst");
    CHECK(ch.frame.count == 8u && ch.ctrl->frames_done == ch.ctrl->frames_submitted,
          "idle: %u of 8 frames published, frames_done %u of %u", ch.frame.count, ch.ctrl->frames_done,
          ch.ctrl->frames_submitted);
    for (i = 0; i < 8u && i < ch.frame.count; i++)
        CHECK(ch.frame.log_first[i] == colors[i] && ch.frame.log_w[i] == 64u && ch.frame.log_h[i] == 64u,
              "frame %u: 0x%06x %ux%u, expected 0x%06x 64x64", i, ch.frame.log_first[i], ch.frame.log_w[i],
              ch.frame.log_h[i], colors[i]);
    printf("  ok  8 frames in present order, the last one after idle\n");

    /* GetFrontBufferData returns the frame just presented, even while its
     * readback is in flight. */
    CHECK(clear(&ch, s.dev, 0x00123456u) == 0 && present(&ch, s.dev) == 0, "PRESENT before GetFrontBufferData");
    CHECK(front_buffer_pixel(&ch, s.dev, 64, 64, &px) && px == 0xff123456u, "front buffer 0x%08x", px);

    /* Reset with a frame in flight: the frame reaches the hook with its
     * own size; the device then presents at the new size. */
    CHECK(clear(&ch, s.dev, 0x00654321u) == 0 && present(&ch, s.dev) == 0, "PRESENT before RESET");
    {
        struct qd3d_wire_pp pp = make_pp(32, 32);

        praw(&p, &pp, sizeof pp);
        CHECK(call(&ch, QD3D_OP_RESET, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
              "RESET with a frame in flight ret 0x%08x", ch.ret);
    }
    ch.screen[2] = ch.screen[3] = 32;
    CHECK(idle(&ch) && ch.frame.width == 64u && ch.frame.height == 64u && ch.frame.uniform &&
              (ch.frame.first & 0x00ffffffu) == 0x00654321u,
          "frame presented before RESET: %ux%u 0x%08x", ch.frame.width, ch.frame.height, ch.frame.first);
    CHECK(clear(&ch, s.dev, 0x00abcdefu) == 0 && present(&ch, s.dev) == 0 && idle(&ch) && ch.frame.width == 32u &&
              ch.frame.height == 32u && ch.frame.uniform && (ch.frame.first & 0x00ffffffu) == 0x00abcdefu,
          "frame after RESET: %ux%u 0x%08x", ch.frame.width, ch.frame.height, ch.frame.first);
    CHECK(front_buffer_pixel(&ch, s.dev, 32, 32, &px) && px == 0xffabcdefu, "front buffer after RESET 0x%08x", px);
    printf("  ok  GetFrontBufferData and RESET with a frame in flight\n");

    /* Frames of two devices reach the hook in present order. */
    s2 = s;
    CHECK(create_device(&ch, &s2, 48, 40), "second device ret 0x%08x", ch.ret);
    n = ch.frame.count;
    CHECK(clear(&ch, s.dev, 0x00111111u) == 0 && present(&ch, s.dev) == 0 && clear(&ch, s2.dev, 0x00222222u) == 0 &&
              present(&ch, s2.dev) == 0 && idle(&ch),
          "PRESENT on two devices");
    CHECK(ch.frame.count == n + 2u && n + 2u <= FRAME_LOG && ch.frame.log_first[n] == 0x00111111u &&
              ch.frame.log_w[n] == 32u && ch.frame.log_first[n + 1u] == 0x00222222u && ch.frame.log_w[n + 1u] == 48u,
          "two devices: %u new frames", ch.frame.count - n);

    /* Releasing a device or ending the session with a frame in flight
     * still shows that frame. */
    CHECK(clear(&ch, s2.dev, 0x00333333u) == 0 && present(&ch, s2.dev) == 0, "PRESENT before device release");
    release(&ch, s2.dev);
    CHECK(idle(&ch) && ch.frame.width == 48u && (ch.frame.first & 0x00ffffffu) == 0x00333333u,
          "frame of a released device: %ux%u 0x%08x", ch.frame.width, ch.frame.height, ch.frame.first);
    CHECK(clear(&ch, s.dev, 0x00444444u) == 0 && present(&ch, s.dev) == 0, "PRESENT before GOODBYE");
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && idle(&ch), "GOODBYE");
    CHECK(ch.frame.width == 32u && (ch.frame.first & 0x00ffffffu) == 0x00444444u,
          "frame before GOODBYE: %ux%u 0x%08x", ch.frame.width, ch.frame.height, ch.frame.first);
    CHECK(ch.frame.count == ch.ctrl->frames_submitted && ch.ctrl->frames_done == ch.ctrl->frames_submitted,
          "%u frames published of %u presented, frames_done %u", ch.frame.count, ch.ctrl->frames_submitted,
          ch.ctrl->frames_done);
    CHECK(qd3dx_live_objects(ch.ex) == 0 && ch.ex->devices == NULL, "GOODBYE left %u objects",
          qd3dx_live_objects(ch.ex));
    printf("  ok  two devices in order; device release and GOODBYE publish the frame in flight\n");

    /* An abandoned session (qd3dx_reset) and executor teardown with a
     * frame in flight. */
    ch.next_id = 1;
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16) && clear(&ch, s.dev, 0x00555555u) == 0 &&
              present(&ch, s.dev) == 0,
          "session abandoned with a frame in flight");
    qd3dx_reset(ch.ex);
    CHECK(qd3dx_live_objects(ch.ex) == 0 && ch.ex->devices == NULL && ch.ctrl->host_state == QD3D_HOST_READY &&
              ch.ctrl->frames_done == ch.ctrl->frames_submitted,
          "qd3dx_reset with a frame in flight");
    ch.next_id = 1;
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16) && clear(&ch, s.dev, 0x00666666u) == 0 &&
              present(&ch, s.dev) == 0,
          "teardown with a frame in flight");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (d) FOURCC layouts                                                       */
/* ------------------------------------------------------------------------ */

#define FMT_ATI1 ((uint32_t)MAKEFOURCC('A', 'T', 'I', '1'))
#define FMT_ATI2 ((uint32_t)MAKEFOURCC('A', 'T', 'I', '2'))
#define FMT_YV12 ((uint32_t)MAKEFOURCC('Y', 'V', '1', '2'))
#define FMT_NV12 ((uint32_t)MAKEFOURCC('N', 'V', '1', '2'))

/* Wire bytes sent and read back. */
static uint8_t g_sent[1u << 20], g_back[1u << 20];

/* HeapValidate on every heap of the process; under --heap-check that
 * includes the fill after each busy block. */
static int heaps_valid(void)
{
    HANDLE heaps[256];
    DWORD n = GetProcessHeaps(256, heaps), i;

    for (i = 0; i < n && i < 256u; i++)
        if (!HeapValidate(heaps[i], 0, NULL))
            return 0;
    return 1;
}

/* Layout a level travels in: blocks of bw x bh texels and bpb bytes. */
struct view {
    uint32_t bw, bh, bpb;
};

static void fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 131u + (i >> 9) + seed * 29u);
}

/* A fresh session when the last one faulted (or never started). */
static int session_ready(struct chan *ch, struct session *s)
{
    if (ch->ctrl->host_state == QD3D_HOST_READY && qd3dx_session_pid(ch->ex) != 0u)
        return 1;
    qd3dx_reset(ch->ex);
    return hello(ch, s) && create_device(ch, s, 16, 16);
}

/* UPLOAD of a whole w x h level in view v with a padded row pitch, READBACK,
 * and a comparison of every row. */
static int level_roundtrip(struct chan *ch, uint32_t obj, uint32_t level, uint32_t face, uint32_t w, uint32_t h,
                           struct view v, uint32_t seed)
{
    struct qd3d_region r = region(QD3D_UPLOAD_SURFACE, level, face);
    uint32_t cols = (w + v.bw - 1u) / v.bw, rows = (h + v.bh - 1u) / v.bh, rb = cols * v.bpb, i;

    r.w = cols * v.bw;
    r.h = rows * v.bh;
    r.row_pitch = rb + 6u;
    r.data_size = r.row_pitch * rows;
    if (r.data_size > sizeof g_sent)
        return 0;
    fill(g_sent, r.data_size, seed);
    if (upload(ch, obj, &r, g_sent) != 0)
        return 0;
    memset(g_back, 0xcc, r.data_size);
    if (readback(ch, obj, &r, g_back) != 0)
        return 0;
    for (i = 0; i < rows; i++)
        if (memcmp(g_sent + (size_t)i * r.row_pitch, g_back + (size_t)i * r.row_pitch, rb) != 0)
            return 0;
    return 1;
}

/* Planar YV12/NV12 surface (even size) in one region: luma rows, then the
 * chroma (YV12: V and U planes at half pitch, NV12: interleaved UV rows). */
static int planar_roundtrip(struct chan *ch, uint32_t surf, uint32_t fmt, uint32_t w, uint32_t h, uint32_t seed)
{
    struct qd3d_region r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    uint32_t crows = h / 2u, rp = w + 10u, i;
    size_t c = (size_t)rp * h;

    r.w = w;
    r.h = h;
    r.row_pitch = rp;
    r.data_size = rp * h + rp * crows;
    fill(g_sent, r.data_size, seed);
    if (upload(ch, surf, &r, g_sent) != 0)
        return 0;
    memset(g_back, 0xcc, r.data_size);
    if (readback(ch, surf, &r, g_back) != 0)
        return 0;
    for (i = 0; i < h; i++)
        if (memcmp(g_sent + (size_t)i * rp, g_back + (size_t)i * rp, w) != 0)
            return 0;
    for (i = 0; i < crows; i++) {
        if (fmt == FMT_YV12) {
            size_t v = c + (size_t)i * (rp / 2u), u = v + (size_t)(rp / 2u) * crows;

            if (memcmp(g_sent + v, g_back + v, w / 2u) != 0 || memcmp(g_sent + u, g_back + u, w / 2u) != 0)
                return 0;
        } else if (memcmp(g_sent + c + (size_t)i * rp, g_back + c + (size_t)i * rp, w) != 0) {
            return 0;
        }
    }
    return 1;
}

static uint32_t create_tex(struct chan *ch, uint32_t dev, int cube, uint32_t size, uint32_t levels, uint32_t usage,
                           uint32_t fmt, uint32_t pool, uint32_t *id)
{
    uint32_t args[6], f;

    if (cube) {
        args[0] = size; args[1] = levels; args[2] = usage; args[3] = fmt; args[4] = pool;
        f = create_simple(ch, QD3D_OP_IDirect3DDevice9_CreateCubeTexture, dev, args, 5, id, 1);
    } else {
        args[0] = size; args[1] = size; args[2] = levels; args[3] = usage; args[4] = fmt; args[5] = pool;
        f = create_simple(ch, QD3D_OP_IDirect3DDevice9_CreateTexture, dev, args, 6, id, 1);
    }
    return f ? f : ch->ret;
}

static void test_fourcc(void)
{
    /* 257x257 full chains (9 levels) in the layout of each pool's native
     * lock: runtime memory one byte per texel, driver memory 4x4 blocks. */
    static const struct {
        uint32_t fmt, pool, usage;
        int cube;
        struct view v;
        const char *name;
    } chains[] = {
        { FMT_ATI2, D3DPOOL_MANAGED, 0, 0, { 1, 1, 1 }, "ATI2 MANAGED 2D" },
        { FMT_ATI2, D3DPOOL_MANAGED, 0, 1, { 1, 1, 1 }, "ATI2 MANAGED cube" },
        { FMT_ATI2, D3DPOOL_SCRATCH, 0, 1, { 1, 1, 1 }, "ATI2 SCRATCH cube" },
        { FMT_ATI2, D3DPOOL_SYSTEMMEM, 0, 1, { 1, 1, 1 }, "ATI2 SYSTEMMEM cube" },
        { FMT_ATI1, D3DPOOL_MANAGED, 0, 0, { 1, 1, 1 }, "ATI1 MANAGED 2D" },
        { FMT_ATI1, D3DPOOL_SCRATCH, 0, 1, { 1, 1, 1 }, "ATI1 SCRATCH cube" },
        { FMT_ATI2, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, 0, { 4, 4, 16 }, "ATI2 DEFAULT|DYNAMIC 2D" },
        { FMT_ATI1, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, 1, { 4, 4, 8 }, "ATI1 DEFAULT|DYNAMIC cube" },
    };
    static const struct {
        uint32_t fmt, w, h;
        const char *name;
    } planar[] = {
        { FMT_YV12, 64, 64, "YV12 64x64" }, { FMT_YV12, 66, 34, "YV12 66x34" }, { FMT_YV12, 5, 3, "YV12 5x3" },
        { FMT_NV12, 64, 64, "NV12 64x64" }, { FMT_NV12, 66, 34, "NV12 66x34" }, { FMT_NV12, 7, 6, "NV12 7x6" },
    };
    static const struct view texel = { 1, 1, 1 };
    struct chan ch;
    struct session s;
    struct qd3d_region r;
    uint32_t tex, surf, f, l, k, faces, ok, tail, fault, args[4];
    size_t c;

    printf("[fourcc]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    for (c = 0; c < sizeof chains / sizeof chains[0]; c++) {
        if (!session_ready(&ch, &s)) {
            CHECK(0, "%s: session", chains[c].name);
            continue;
        }
        f = create_tex(&ch, s.dev, chains[c].cube, 257, 0, chains[c].usage, chains[c].fmt, chains[c].pool, &tex);
        CHECK(f == (uint32_t)S_OK, "%s: create 0x%08x", chains[c].name, f);
        if (f != (uint32_t)S_OK)
            continue;
        faces = chains[c].cube ? 6u : 1u;
        ok = 1;
        for (f = 0; f < faces && ok; f++) {
            for (l = 0; l < 9u && ok; l++) {
                uint32_t d = 257u >> l;

                ok = level_roundtrip(&ch, tex, l, f, d, d, chains[c].v, (uint32_t)c * 64u + f * 9u + l);
                CHECK(ok, "%s: face %u level %u (%ux%u) round trip, host_state %u", chains[c].name, f, l, d, d,
                      ch.ctrl->host_state);
            }
        }
        if (ch.ctrl->host_state == QD3D_HOST_READY)
            release(&ch, tex);
        CHECK(heaps_valid(), "%s: process heaps validate", chains[c].name);
        if (ok)
            printf("  ok  %s: 9 levels x %u face(s) round trip\n", chains[c].name, faces);
    }

    /* YV12/NV12: luma and chroma planes in driver memory (DEFAULT) at even
     * sizes; odd sizes, which have no defined layout there, are declined
     * (UPLOAD skipped, READBACK D3DERR_INVALIDCALL). Runtime memory holds the
     * luma plane alone, as the native level has no chroma there. */
    for (k = 0; k < sizeof planar / sizeof planar[0]; k++) {
        uint32_t w = planar[k].w, h = planar[k].h;

        if (!session_ready(&ch, &s)) {
            CHECK(0, "%s: session", planar[k].name);
            continue;
        }
        args[0] = w; args[1] = h; args[2] = planar[k].fmt; args[3] = D3DPOOL_DEFAULT;
        f = create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, s.dev, args, 4, &surf, 1);
        CHECK(f == 0 && ch.ret == (uint32_t)S_OK, "%s DEFAULT: create 0x%08x", planar[k].name, ch.ret);
        if (f == 0 && ch.ret == (uint32_t)S_OK && !((w | h) & 1u)) {
            ok = planar_roundtrip(&ch, surf, planar[k].fmt, w, h, k);
            CHECK(ok, "%s DEFAULT: planar round trip, host_state %u", planar[k].name, ch.ctrl->host_state);
            if (ch.ctrl->host_state == QD3D_HOST_READY)
                release(&ch, surf);
            if (ok)
                printf("  ok  %s DEFAULT: luma and chroma planes round trip\n", planar[k].name);
        } else if (f == 0 && ch.ret == (uint32_t)S_OK) {
            ok = (uint32_t)g_failures;
            r = region(QD3D_UPLOAD_SURFACE, 0, 0);
            r.w = w;
            r.h = h;
            r.row_pitch = (w + 11u) & ~1u;
            r.data_size = r.row_pitch * (h + (h + 1u) / 2u);
            fill(g_sent, r.data_size, k);
            f = upload(&ch, surf, &r, g_sent);
            CHECK(f == 0 && ch.ctrl->host_state == QD3D_HOST_READY, "%s DEFAULT: UPLOAD fault %u", planar[k].name, f);
            f = readback(&ch, surf, &r, g_back);
            CHECK(f == 0xfffffff0u && ch.ret == (uint32_t)D3DERR_INVALIDCALL,
                  "%s DEFAULT: READBACK %u ret 0x%08x", planar[k].name, f, ch.ret);
            if (ch.ctrl->host_state == QD3D_HOST_READY)
                release(&ch, surf);
            if ((uint32_t)g_failures == ok)
                printf("  ok  %s DEFAULT: odd planar transfers declined\n", planar[k].name);
        }
        if (!session_ready(&ch, &s))
            continue;
        args[3] = D3DPOOL_SYSTEMMEM;
        f = create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, s.dev, args, 4, &surf, 1);
        CHECK(f == 0 && ch.ret == (uint32_t)S_OK, "%s SYSTEMMEM: create 0x%08x", planar[k].name, ch.ret);
        if (f == 0 && ch.ret == (uint32_t)S_OK) {
            ok = level_roundtrip(&ch, surf, 0, 0, planar[k].w, planar[k].h, texel, k);
            CHECK(ok, "%s SYSTEMMEM: luma round trip, host_state %u", planar[k].name, ch.ctrl->host_state);
            if (ch.ctrl->host_state == QD3D_HOST_READY)
                release(&ch, surf);
        }
    }
    CHECK(heaps_valid(), "planar: process heaps validate");

    /* A region outside the native lock's layout faults before any native
     * memory is touched. The 4x4 view once used for runtime-memory ATI2
     * levels wrote 16 bytes into the 4-byte 1x1 level, past the runtime's
     * allocation about every other time, so this runs on eight textures. */
    ok = (uint32_t)g_failures;
    for (k = 0; k < 8u; k++) {
        qd3dx_reset(ch.ex);
        if (!session_ready(&ch, &s) ||
            create_tex(&ch, s.dev, 0, 8, 0, 0, FMT_ATI2, (k & 1u) ? D3DPOOL_SCRATCH : D3DPOOL_MANAGED, &tex) !=
                (uint32_t)S_OK) {
            CHECK(0, "ATI2 8x8 fixture %u", k);
            continue;
        }
        r = region(QD3D_UPLOAD_SURFACE, 3, 0);
        r.w = 4;
        r.h = 4;
        r.row_pitch = 16;
        r.data_size = 16;
        fill(g_sent, 16, k);
        tail = ch.ctrl->tail;
        fault = upload(&ch, tex, &r, g_sent);
        CHECK(fault == QD3D_FAULT_SIZE && ch.ctrl->tail == tail,
              "4x4 block UPLOAD into the 1x1 level of a %s ATI2 texture: fault %u", (k & 1u) ? "SCRATCH" : "MANAGED",
              fault);
    }
    CHECK(heaps_valid(), "process heaps validate after rejected ATI2 block uploads");
    if ((uint32_t)g_failures == ok)
        printf("  ok  4x4 block UPLOADs into 1x1 runtime-memory ATI2 levels fault, heaps intact\n");

    qd3dx_reset(ch.ex);
    CHECK(session_ready(&ch, &s) && create_tex(&ch, s.dev, 0, 8, 0, 0, FMT_ATI2, D3DPOOL_MANAGED, &tex) == (uint32_t)S_OK,
          "ATI2 READBACK fixture");
    r = region(QD3D_UPLOAD_SURFACE, 3, 0);
    r.w = 4;
    r.h = 4;
    r.row_pitch = 16;
    r.data_size = 16;
    tail = ch.ctrl->tail;
    expect_fault(&ch, "ATI2 4x4 READBACK of a 1x1 level", readback(&ch, tex, &r, g_back), QD3D_FAULT_SIZE, tail);

    CHECK(session_ready(&ch, &s) &&
              create_tex(&ch, s.dev, 0, 8, 1, D3DUSAGE_DYNAMIC, FMT_ATI2, D3DPOOL_DEFAULT, &tex) == (uint32_t)S_OK,
          "ATI2 DEFAULT fixture");
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 8;
    r.h = 8;
    r.row_pitch = 8;
    r.data_size = 64;
    tail = ch.ctrl->tail;
    expect_fault(&ch, "ATI2 texel UPLOAD into driver memory", upload(&ch, tex, &r, g_sent), QD3D_FAULT_SIZE, tail);

    {
        static const struct {
            uint32_t w, h, rw, rp, data;
            const char *name;
        } bad[] = {
            { 64, 64, 32, 64, 64u * 64u + 64u * 32u, "YV12 partial planar region" },
            { 64, 64, 64, 65, 65u * 64u + 65u * 32u, "YV12 planar region with odd pitch" },
            { 64, 64, 64, 64, 64u * 64u + 64u * 31u, "YV12 planar data one chroma row short" },
        };

        for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
            args[0] = bad[k].w; args[1] = bad[k].h; args[2] = FMT_YV12; args[3] = D3DPOOL_DEFAULT;
            CHECK(session_ready(&ch, &s) &&
                      create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, s.dev, args, 4, &surf,
                                    1) == 0 &&
                      ch.ret == (uint32_t)S_OK,
                  "%s: fixture", bad[k].name);
            r = region(QD3D_UPLOAD_SURFACE, 0, 0);
            r.w = bad[k].rw;
            r.h = bad[k].h;
            r.row_pitch = bad[k].rp;
            r.data_size = bad[k].data;
            tail = ch.ctrl->tail;
            expect_fault(&ch, bad[k].name, upload(&ch, surf, &r, g_sent), QD3D_FAULT_SIZE, tail);
        }
    }
    CHECK(heaps_valid(), "process heaps validate after rejected regions");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (e) Host ids                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t surface_level(struct chan *ch, uint32_t tex, uint32_t level)
{
    struct pl p = { .n = 0 };

    pu(&p, level);
    pu(&p, 1);
    if (call(ch, QD3D_OP_IDirect3DTexture9_GetSurfaceLevel, tex, 1, &p) || ch->ret != (uint32_t)S_OK)
        return 0;
    return reply_u32(ch, 0);
}

static void test_host_ids(void)
{
    static const char *const how[] = { "after GOODBYE", "after qd3dx_reset", "HELLO over a live session" };
    struct chan ch;
    struct session s;
    uint32_t type, prev, bb, tex, l0, l1, l2, k;
    int failures = g_failures;

    printf("[host ids]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    /* Proxies of a superseded session may still name its host ids, so no
     * later session gets them again, however it starts and whichever guest
     * ids it reuses. */
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "first session");
    prev = get_back_buffer(&ch, s.dev, &type);
    CHECK(QD3D_ID_IS_HOST(prev), "first back buffer 0x%08x", prev);
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0, "GOODBYE");
    for (k = 0; k < 3u; k++) {
        if (k == 1u)
            qd3dx_reset(ch.ex);
        ch.next_id = 1;
        CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "session %s", how[k]);
        bb = get_back_buffer(&ch, s.dev, &type);
        CHECK(QD3D_ID_IS_HOST(bb) && (bb & ~QD3D_ID_HOST_BIT) > (prev & ~QD3D_ID_HOST_BIT),
              "back buffer %s: 0x%08x, previous session 0x%08x", how[k], bb, prev);
        prev = bb;
    }
    if (g_failures == failures)
        printf("  ok  host ids keep increasing across sessions (last 0x%08x)\n", prev);

    /* The counter wraps past 0x7fffffff and skips ids still live. */
    CHECK(create_tex(&ch, s.dev, 0, 8, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex) == (uint32_t)S_OK,
          "wrap texture");
    ch.ex->next_host_id = 0x7fffffffu;
    l0 = surface_level(&ch, tex, 0);
    l1 = surface_level(&ch, tex, 1);
    CHECK(l0 == 0xffffffffu && l1 == (QD3D_ID_HOST_BIT | 1u), "ids across the wrap: 0x%08x 0x%08x", l0, l1);
    ch.ex->next_host_id = prev & ~QD3D_ID_HOST_BIT; /* the live back buffer's number */
    l2 = surface_level(&ch, tex, 2);
    CHECK(QD3D_ID_IS_HOST(l2) && l2 != prev && l2 != l0 && l2 != l1, "id after a live one: 0x%08x (live 0x%08x)", l2,
          prev);
    if (g_failures == failures)
        printf("  ok  wrap past 0x7fffffff: 0x%08x 0x%08x, live 0x%08x skipped for 0x%08x\n", l0, l1, prev, l2);
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "host ids GOODBYE");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (f) SYSTEMMEM textures                                                   */
/* ------------------------------------------------------------------------ */

static uint32_t get_texture(struct chan *ch, uint32_t dev, uint32_t stage)
{
    struct pl p = { .n = 0 };

    pu(&p, stage);
    pu(&p, 1);
    if (call(ch, QD3D_OP_IDirect3DDevice9_GetTexture, dev, 1, &p) || ch->ret != (uint32_t)S_OK)
        return 0xffffffffu;
    return reply_u32(ch, 0);
}

/* A SYSTEMMEM texture bound to a stage is sampled (natively 9Ex drops such
 * a draw and fails the next allocation), and GetTexture answers it until
 * RESET unbinds it as native D3D9 does. */
static void test_sysmem_texture(void)
{
    static const struct { float x, y, z, rhw, u, v; } quad[] = {
        { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }, { 16.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f },
        { 0.0f, 16.0f, 0.0f, 1.0f, 0.0f, 1.0f }, { 16.0f, 16.0f, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    const uint32_t color = 0x0020c040u;
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    struct qd3d_region r;
    uint32_t tex, other, bb, type, i, px = 0;
    int failures = g_failures;
    static uint8_t texels[64], back[16u * 16u * 4u];

    printf("[sysmem texture]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "sysmem texture session");
    CHECK(create_tex(&ch, s.dev, 0, 4, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &tex) == (uint32_t)S_OK,
          "SYSTEMMEM texture");
    for (i = 0; i < 16u; i++)
        memcpy(texels + i * 4u, &color, 4);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 4;
    r.h = 4;
    r.row_pitch = 16;
    r.data_size = 64;
    CHECK(upload(&ch, tex, &r, texels) == 0, "UPLOAD SYSTEMMEM texture");
    pu(&p, 0);
    pu(&p, tex);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetTexture, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "SetTexture(SYSTEMMEM) ret 0x%08x", ch.ret);
    p.n = 0;
    pu(&p, D3DFVF_XYZRHW | D3DFVF_TEX1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetFVF, s.dev, 0, &p) == 0, "SetFVF");
    CHECK(clear(&ch, s.dev, 0x00ffffffu) == 0 && call(&ch, QD3D_OP_IDirect3DDevice9_BeginScene, s.dev, 1, NULL) == 0,
          "Clear, BeginScene");
    p.n = 0;
    pu(&p, D3DPT_TRIANGLESTRIP);
    pu(&p, 2);
    pdata(&p, quad, (uint32_t)sizeof quad);
    pu(&p, (uint32_t)sizeof quad[0]);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_DrawPrimitiveUP, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "DrawPrimitiveUP with a SYSTEMMEM texture ret 0x%08x", ch.ret);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_EndScene, s.dev, 1, NULL) == 0, "EndScene");
    bb = get_back_buffer(&ch, s.dev, &type);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 16;
    r.h = 16;
    r.row_pitch = 64;
    r.data_size = sizeof back;
    CHECK(readback(&ch, bb, &r, back) == 0, "READBACK back buffer");
    memcpy(&px, back + 64u * 8u + 4u * 8u, 4);
    CHECK((px & 0x00ffffffu) == color, "pixel sampled from the SYSTEMMEM texture 0x%08x", px);
    CHECK(get_texture(&ch, s.dev, 0) == tex, "GetTexture answers the SYSTEMMEM texture");
    CHECK(create_tex(&ch, s.dev, 0, 4, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &other) == (uint32_t)S_OK,
          "the next allocation succeeds");
    p.n = 0;
    pu(&p, 0);
    pu(&p, other);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetTexture, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK &&
              get_texture(&ch, s.dev, 0) == other,
          "rebinding a DEFAULT texture");
    p.n = 0;
    pu(&p, 0);
    pu(&p, tex);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetTexture, s.dev, 1, &p) == 0 && get_texture(&ch, s.dev, 0) == tex,
          "rebinding the SYSTEMMEM texture");
    {
        struct qd3d_wire_pp pp = make_pp(16, 16);

        release(&ch, bb);
        p.n = 0;
        praw(&p, &pp, sizeof pp);
        CHECK(call(&ch, QD3D_OP_RESET, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK, "RESET ret 0x%08x", ch.ret);
    }
    CHECK(get_texture(&ch, s.dev, 0) == 0u, "RESET unbinds the SYSTEMMEM texture");
    if (g_failures == failures)
        printf("  ok  SYSTEMMEM texture sampled (0x%06x), GetTexture answers it, next allocation succeeds\n",
               px & 0x00ffffffu);
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "sysmem texture GOODBYE");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (h) Cursor over published frames, front buffer placement                 */
/* ------------------------------------------------------------------------ */

static uint32_t present_ex(struct chan *ch, uint32_t obj, const int32_t *src, const int32_t *screen, uint32_t cursor,
                           int32_t cx, int32_t cy)
{
    struct pl p = { .n = 0 };
    struct qd3d_present pr;

    memset(&pr, 0, sizeof pr);
    if (src) {
        pr.has_src = 1;
        memcpy(pr.src, src, sizeof pr.src);
    }
    memcpy(pr.screen, screen, sizeof pr.screen);
    pr.cursor = cursor;
    pr.cursor_pos[0] = cx;
    pr.cursor_pos[1] = cy;
    praw(&p, &pr, sizeof pr);
    ch->ctrl->frames_submitted++;
    return call(ch, QD3D_OP_PRESENT, obj, 0, &p);
}

static void cursor_payload(struct pl *p, uint32_t w, uint32_t h, uint32_t hx, uint32_t hy, const uint32_t *px)
{
    struct qd3d_cursor c = { w, h, hx, hy };

    p->n = 0;
    praw(p, &c, sizeof c);
    if (px)
        praw(p, px, w * h * 4u);
}

/* GetFrontBufferData into a new w x h A8R8G8B8 system-memory surface, read
 * back into out (w * h pixels). */
static int front_read(struct chan *ch, uint32_t dev, uint32_t w, uint32_t h, uint32_t *out)
{
    struct qd3d_region r;
    struct pl p = { .n = 0 };
    uint32_t args[4] = { w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM }, plain;
    int ok;

    if (create_simple(ch, QD3D_OP_IDirect3DDevice9_CreateOffscreenPlainSurface, dev, args, 4, &plain, 1) ||
        ch->ret != (uint32_t)S_OK)
        return 0;
    pu(&p, 0);
    pu(&p, plain);
    ok = call(ch, QD3D_OP_IDirect3DDevice9_GetFrontBufferData, dev, 1, &p) == 0 && ch->ret == (uint32_t)S_OK;
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = w;
    r.h = h;
    r.row_pitch = w * 4u;
    r.data_size = w * h * 4u;
    ok = ok && readback(ch, plain, &r, out) == 0;
    release(ch, plain);
    return ok;
}

/* The session's cursor image is drawn over published frames at the frame
 * pixel the guest screen rectangle maps its position to, with opaque,
 * transparent, inverting and blended pixels, clipped at the frame edge,
 * scaled positions, and never into the front buffer, which places the frame
 * at its screen rectangle. Malformed CURSOR commands fault. */
static void test_cursor(void)
{
    static uint32_t img[8 * 8], front[256 * 300];
    static const int32_t screen[4] = { 100, 200, 164, 264 }, twice[4] = { 100, 200, 228, 328 };
    const uint32_t bg = 0x00102030u;
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    uint32_t i, tail;
    int failures = g_failures;

    printf("[cursor]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    for (i = 0; i < 64u; i++)
        img[i] = 0xff0000ffu; /* opaque blue */
    img[0] = img[1] = 0xffff0000u;      /* opaque red */
    img[2] = img[3] = 0x00000000u;      /* transparent */
    img[4] = img[5] = 0x00ffffffu;      /* inverting */
    img[6] = img[7] = 0x8000ff00u;      /* half-transparent green */

    CHECK(hello(&ch, &s) && create_device(&ch, &s, 64, 64), "cursor session");
    cursor_payload(&p, 8, 8, 2, 3, img);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0, "CURSOR 8x8");
    CHECK(clear(&ch, s.dev, bg) == 0 && present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0 && idle(&ch),
          "PRESENT with the cursor");
    /* screen (110, 220) is frame (10, 20); hot spot (2, 3): top-left (8, 17). */
    CHECK(frame_px(&ch, 8, 17) == 0xff0000u && frame_px(&ch, 9, 17) == 0xff0000u, "opaque cursor pixel 0x%06x",
          frame_px(&ch, 8, 17));
    CHECK(frame_px(&ch, 10, 17) == bg && frame_px(&ch, 11, 17) == bg, "transparent cursor pixel 0x%06x",
          frame_px(&ch, 10, 17));
    CHECK(frame_px(&ch, 12, 17) == (bg ^ 0x00ffffffu), "inverting cursor pixel 0x%06x", frame_px(&ch, 12, 17));
    CHECK(frame_px(&ch, 14, 17) == 0x089018u, "blended cursor pixel 0x%06x", frame_px(&ch, 14, 17));
    CHECK(frame_px(&ch, 8, 18) == 0x0000ffu && frame_px(&ch, 15, 24) == 0x0000ffu, "cursor body 0x%06x 0x%06x",
          frame_px(&ch, 8, 18), frame_px(&ch, 15, 24));
    CHECK(frame_px(&ch, 7, 17) == bg && frame_px(&ch, 16, 17) == bg && frame_px(&ch, 8, 16) == bg &&
              frame_px(&ch, 8, 25) == bg,
          "pixels around the cursor keep the frame");

    /* The front buffer: the frame at its screen rectangle, no cursor. */
    CHECK(front_read(&ch, s.dev, 256, 300, front), "GetFrontBufferData 256x300");
    CHECK(front[(200 + 17) * 256 + 100 + 8] == (0xff000000u | bg) && front[200 * 256 + 100] == (0xff000000u | bg) &&
              front[263 * 256 + 163] == (0xff000000u | bg),
          "front buffer holds the frame at its screen rectangle without the cursor 0x%08x",
          front[(200 + 17) * 256 + 100 + 8]);
    CHECK(front[0] == 0xff000000u && front[199 * 256 + 100] == 0xff000000u && front[200 * 256 + 99] == 0xff000000u &&
              front[264 * 256 + 100] == 0xff000000u && front[200 * 256 + 164] == 0xff000000u,
          "front buffer is opaque black outside the frame");

    /* Clipped at the frame's top-left corner. */
    CHECK(present_ex(&ch, s.dev, NULL, screen, 1, 100, 200) == 0 && idle(&ch), "PRESENT, cursor at the corner");
    CHECK(frame_px(&ch, 0, 0) == 0x0000ffu && frame_px(&ch, 5, 4) == 0x0000ffu && frame_px(&ch, 6, 0) == bg &&
              frame_px(&ch, 0, 5) == bg,
          "clipped cursor 0x%06x 0x%06x", frame_px(&ch, 0, 0), frame_px(&ch, 6, 0));
    /* A screen rectangle twice the frame's size halves the position. */
    CHECK(present_ex(&ch, s.dev, NULL, twice, 1, 164, 264) == 0 && idle(&ch), "PRESENT, scaled screen");
    CHECK(frame_px(&ch, 30, 29) == 0xff0000u && frame_px(&ch, 29, 29) == bg, "scaled cursor position 0x%06x",
          frame_px(&ch, 30, 29));
    /* Off the frame, not requested, or no image: nothing drawn. */
    CHECK(present_ex(&ch, s.dev, NULL, screen, 1, 50, 50) == 0 && idle(&ch) && ch.frame.uniform &&
              (ch.frame.first & 0x00ffffffu) == bg,
          "cursor off the frame");
    CHECK(present_ex(&ch, s.dev, NULL, screen, 0, 110, 220) == 0 && idle(&ch) && ch.frame.uniform,
          "frame without the cursor");
    cursor_payload(&p, 0, 0, 0, 0, NULL);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0 && present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0 &&
              idle(&ch) && ch.frame.uniform,
          "cleared cursor image");
    /* A cursor image replaced while its frame is in flight: the frame keeps
     * the image it was presented with. */
    cursor_payload(&p, 8, 8, 2, 3, img);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0 && present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0,
          "PRESENT with a cursor, then a new image");
    for (i = 0; i < 64u; i++)
        img[i] = 0xff00ff00u;
    cursor_payload(&p, 8, 8, 0, 0, img);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0 && idle(&ch) && frame_px(&ch, 8, 17) == 0xff0000u,
          "frame in flight keeps its cursor image 0x%06x", frame_px(&ch, 8, 17));
    CHECK(present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0 && idle(&ch) && frame_px(&ch, 10, 20) == 0x00ff00u,
          "the next frame draws the new image 0x%06x", frame_px(&ch, 10, 20));
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "cursor GOODBYE");
    if (g_failures == failures)
        printf("  ok  cursor drawn over published frames (opaque, transparent, inverting, blended, clipped, scaled), "
               "front buffer at the screen rectangle without it\n");

    /* Malformed CURSOR and PRESENT commands. */
    CHECK(session_ready(&ch, &s), "cursor fault session");
    cursor_payload(&p, QD3D_CURSOR_MAX + 1u, 1, 0, 0, NULL);
    fault_raw(&ch, "CURSOR wider than the maximum", QD3D_OP_CURSOR, 0, 0, &p, 16u + p.n, QD3D_FAULT_DECODE);
    CHECK(session_ready(&ch, &s), "cursor fault session");
    cursor_payload(&p, 8, 8, 8, 0, img);
    fault_raw(&ch, "CURSOR hot spot outside", QD3D_OP_CURSOR, 0, 0, &p, 16u + p.n, QD3D_FAULT_DECODE);
    CHECK(session_ready(&ch, &s), "cursor fault session");
    cursor_payload(&p, 0, 0, 1, 0, NULL);
    fault_raw(&ch, "CURSOR empty with a hot spot", QD3D_OP_CURSOR, 0, 0, &p, 16u + p.n, QD3D_FAULT_DECODE);
    CHECK(session_ready(&ch, &s), "cursor fault session");
    cursor_payload(&p, 8, 8, 0, 0, img);
    p.n -= 4u;
    fault_raw(&ch, "CURSOR one pixel short", QD3D_OP_CURSOR, 0, 0, &p, 16u + p.n, QD3D_FAULT_SIZE);
    CHECK(session_ready(&ch, &s), "cursor fault session");
    cursor_payload(&p, 8, 8, 0, 0, img);
    fault_raw(&ch, "CURSOR naming an object", QD3D_OP_CURSOR, s.dev, 0, &p, 16u + p.n, QD3D_FAULT_DECODE);
    CHECK(session_ready(&ch, &s), "cursor fault session");
    {
        struct qd3d_present pr;

        memset(&pr, 0, sizeof pr);
        pr.cursor = 2;
        p.n = 0;
        praw(&p, &pr, sizeof pr);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_PRESENT, s.dev, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "PRESENT cursor flag 2", run(&ch), QD3D_FAULT_DECODE, tail);
    }
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (i) Additional swap chains, source rectangles, the display policy        */
/* ------------------------------------------------------------------------ */

static uint32_t create_swapchain(struct chan *ch, uint32_t dev, uint32_t w, uint32_t h, uint32_t *id)
{
    struct pl p = { .n = 0 };
    struct qd3d_create_swapchain cs;

    *id = new_id(ch);
    memset(&cs, 0, sizeof cs);
    cs.swapchain_id = *id;
    cs.pp = make_pp(w, h);
    cs.pp.EnableAutoDepthStencil = 0;
    praw(&p, &cs, sizeof cs);
    return call(ch, QD3D_OP_CREATE_SWAPCHAIN, dev, 1, &p);
}

static uint32_t color_fill(struct chan *ch, uint32_t dev, uint32_t surface, D3DCOLOR color)
{
    struct pl p = { .n = 0 };

    pu(&p, surface);
    pdata(&p, NULL, 0);
    pu(&p, color);
    return call(ch, QD3D_OP_IDirect3DDevice9_ColorFill, dev, 1, &p);
}

/* A swap chain the guest creates renders, presents and answers its back
 * buffer; of the device's swap chains the largest presenting one is shown
 * (the smaller is not read back while the larger presents, and is shown
 * once the larger stopped presenting for the hold); a source rectangle
 * publishes that part of the back buffer. */
static void test_swapchain(void)
{
    static const int32_t src[4] = { 8, 8, 24, 24 };
    static uint32_t back[32 * 32];
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    struct qd3d_wire_pp acc;
    struct qd3d_region r;
    uint32_t sc, bb, type, frames, tail;
    int failures = g_failures;

    printf("[swap chains]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 64, 64), "swap chain session");
    CHECK(create_swapchain(&ch, s.dev, 32, 32, &sc) == 0 && ch.ret == (uint32_t)S_OK && ch.rsize == sizeof acc,
          "CREATE_SWAPCHAIN ret 0x%08x", ch.ret);
    memcpy(&acc, ch.rp, sizeof acc);
    CHECK(acc.BackBufferWidth == 32u && acc.BackBufferHeight == 32u && acc.BackBufferCount == 1u,
          "accepted swap chain parameters %ux%u count %u", acc.BackBufferWidth, acc.BackBufferHeight,
          acc.BackBufferCount);
    pu(&p, 0);
    pu(&p, D3DBACKBUFFER_TYPE_MONO);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DSwapChain9_GetBackBuffer, sc, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "swap chain GetBackBuffer ret 0x%08x", ch.ret);
    bb = reply_u32(&ch, 0);
    type = reply_u32(&ch, 1);
    CHECK(QD3D_ID_IS_HOST(bb) && type == QD3D_T_IDirect3DSurface9, "swap chain back buffer 0x%08x type %u", bb, type);
    CHECK(color_fill(&ch, s.dev, bb, 0x00a0b0c0u) == 0 && ch.ret == (uint32_t)S_OK, "ColorFill the swap chain");
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 32;
    r.h = 32;
    r.row_pitch = 128;
    r.data_size = sizeof back;
    CHECK(readback(&ch, bb, &r, back) == 0 && (back[16 * 32 + 16] & 0x00ffffffu) == 0x00a0b0c0u,
          "swap chain back buffer content 0x%08x", back[16 * 32 + 16]);
    p.n = 0;
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetNumberOfSwapChains, s.dev, 1, &p) == 0 && ch.ret == 1u,
          "GetNumberOfSwapChains counts the implicit swap chain only (%u)", ch.ret);
    p.n = 0;
    pu(&p, 1);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetSwapChain, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)D3DERR_INVALIDCALL,
          "GetSwapChain(1) does not answer the additional swap chain (0x%08x)", ch.ret);

    /* The larger implicit swap chain presents: the smaller one is not shown. */
    CHECK(clear(&ch, s.dev, 0x00111111u) == 0 && present(&ch, s.dev) == 0 && idle(&ch) && ch.frame.width == 64u,
          "implicit swap chain shown");
    frames = ch.frame.count;
    CHECK(present(&ch, sc) == 0 && idle(&ch) && ch.frame.count == frames &&
              ch.ctrl->frames_done == ch.ctrl->frames_submitted,
          "smaller swap chain not shown while the larger presents (frames %u, done %u of %u)", ch.frame.count,
          ch.ctrl->frames_done, ch.ctrl->frames_submitted);
    Sleep(400);
    CHECK(present(&ch, sc) == 0 && idle(&ch) && ch.frame.count == frames + 1u && ch.frame.width == 32u &&
              ch.frame.uniform && (ch.frame.first & 0x00ffffffu) == 0x00a0b0c0u,
          "swap chain shown once the larger stopped presenting: %ux%u 0x%08x", ch.frame.width, ch.frame.height,
          ch.frame.first);
    CHECK(present(&ch, s.dev) == 0 && idle(&ch) && ch.frame.count == frames + 2u && ch.frame.width == 64u,
          "the larger swap chain is shown again at once");
    /* A source rectangle publishes that part of the back buffer. */
    CHECK(present_ex(&ch, s.dev, src, ch.screen, 0, 0, 0) == 0 && idle(&ch) && ch.frame.width == 16u &&
              ch.frame.height == 16u && (ch.frame.first & 0x00ffffffu) == 0x00111111u,
          "source rectangle frame %ux%u", ch.frame.width, ch.frame.height);
    release(&ch, bb);
    release(&ch, sc);
    CHECK(present(&ch, s.dev) == 0 && idle(&ch), "PRESENT after the swap chain went");

    /* Malformed CREATE_SWAPCHAIN commands. */
    {
        struct qd3d_create_swapchain cs;

        memset(&cs, 0, sizeof cs);
        cs.swapchain_id = new_id(&ch);
        cs.pp = make_pp(16, 16);
        p.n = 0;
        praw(&p, &cs, sizeof cs);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CREATE_SWAPCHAIN, s.d3d, ++ch.seq, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CREATE_SWAPCHAIN on IDirect3D9", run(&ch), QD3D_FAULT_OBJECT, tail);
        CHECK(session_ready(&ch, &s), "swap chain fault session");
        cs.swapchain_id = s.dev; /* bound already */
        p.n = 0;
        praw(&p, &cs, sizeof cs);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CREATE_SWAPCHAIN, s.dev, ++ch.seq, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CREATE_SWAPCHAIN reusing an id", run(&ch), QD3D_FAULT_ID, tail);
        CHECK(session_ready(&ch, &s), "swap chain fault session");
        p.n -= 4u;
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CREATE_SWAPCHAIN, s.dev, ++ch.seq, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CREATE_SWAPCHAIN short", run(&ch), QD3D_FAULT_DECODE, tail);
    }
    if (g_failures == failures)
        printf("  ok  swap chain renders, presents, answers its back buffer; largest presenting swap chain shown; "
               "source rectangle published\n");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (j) Managed texture LOD                                                  */
/* ------------------------------------------------------------------------ */

static uint32_t sampler(struct chan *ch, uint32_t dev, uint32_t type, uint32_t value)
{
    struct pl p = { .n = 0 };

    pu(&p, 0);
    pu(&p, type);
    pu(&p, value);
    return call(ch, QD3D_OP_IDirect3DDevice9_SetSamplerState, dev, 1, &p);
}

static uint32_t get_sampler(struct chan *ch, uint32_t dev, uint32_t type)
{
    struct pl p = { .n = 0 };

    pu(&p, 0);
    pu(&p, type);
    pu(&p, 1);
    if (call(ch, QD3D_OP_IDirect3DDevice9_GetSamplerState, dev, 1, &p) || ch->ret != (uint32_t)S_OK)
        return 0xffffffffu;
    return reply_u32(ch, 1); /* DATA item: length, value */
}

static uint32_t set_lod(struct chan *ch, uint32_t tex, uint32_t lod)
{
    struct pl p = { .n = 0 };

    pu(&p, lod);
    if (call(ch, QD3D_OP_IDirect3DBaseTexture9_SetLOD, tex, 1, &p))
        return 0xffffffffu;
    return ch->ret;
}

static uint32_t set_texture(struct chan *ch, uint32_t dev, uint32_t tex)
{
    struct pl p = { .n = 0 };

    pu(&p, 0);
    pu(&p, tex);
    return call(ch, QD3D_OP_IDirect3DDevice9_SetTexture, dev, 1, &p);
}

/* Draws a 16x16 quad sampling stage 0 (magnified, so the most detailed
 * level allowed is sampled) and returns its centre pixel. */
static uint32_t lod_draw(struct chan *ch, uint32_t dev)
{
    static const struct { float x, y, z, rhw, u, v; } quad[] = {
        { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }, { 16.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f },
        { 0.0f, 16.0f, 0.0f, 1.0f, 0.0f, 1.0f }, { 16.0f, 16.0f, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    static uint8_t back[16u * 16u * 4u];
    struct pl p = { .n = 0 };
    struct qd3d_region r;
    uint32_t bb, type, px = 0xdeadbeefu;

    if (clear(ch, dev, 0x00000000u) || call(ch, QD3D_OP_IDirect3DDevice9_BeginScene, dev, 1, NULL))
        return px;
    pu(&p, D3DPT_TRIANGLESTRIP);
    pu(&p, 2);
    pdata(&p, quad, (uint32_t)sizeof quad);
    pu(&p, (uint32_t)sizeof quad[0]);
    if (call(ch, QD3D_OP_IDirect3DDevice9_DrawPrimitiveUP, dev, 1, &p) || ch->ret != (uint32_t)S_OK ||
        call(ch, QD3D_OP_IDirect3DDevice9_EndScene, dev, 1, NULL))
        return px;
    bb = get_back_buffer(ch, dev, &type);
    r = region(QD3D_UPLOAD_SURFACE, 0, 0);
    r.w = 16;
    r.h = 16;
    r.row_pitch = 64;
    r.data_size = sizeof back;
    if (readback(ch, bb, &r, back) == 0)
        memcpy(&px, back + 64u * 8u + 4u * 8u, 4);
    release(ch, bb);
    return px & 0x00ffffffu;
}

/* A managed texture's LOD selects its most detailed level as natively:
 * max(MAXMIPLEVEL, LOD) with mip filtering, the LOD level without; the
 * guest's sampler states stay what it set (getters, state blocks); a
 * DEFAULT texture's SetLOD does nothing. */
static void test_lod(void)
{
    static const uint32_t colors[4] = { 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu };
    static uint32_t texels[8 * 8];
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    struct qd3d_region r;
    uint32_t tex, plain, sb, l, i, px;
    int failures = g_failures;

    printf("[managed LOD]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "LOD session");
    CHECK(create_tex(&ch, s.dev, 0, 8, 4, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex) == (uint32_t)S_OK,
          "MANAGED texture 8x8, 4 levels");
    for (l = 0; l < 4u; l++) {
        uint32_t n = 8u >> l;

        for (i = 0; i < n * n; i++)
            texels[i] = colors[l];
        r = region(QD3D_UPLOAD_SURFACE, l, 0);
        r.w = n;
        r.h = n;
        r.row_pitch = n * 4u;
        r.data_size = n * n * 4u;
        CHECK(upload(&ch, tex, &r, texels) == 0, "UPLOAD level %u", l);
    }
    CHECK(create_tex(&ch, s.dev, 0, 8, 4, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &plain) ==
              (uint32_t)S_OK,
          "DEFAULT texture");
    l = surface_level(&ch, plain, 0);
    CHECK(color_fill(&ch, s.dev, l, colors[0]) == 0 && ch.ret == (uint32_t)S_OK, "ColorFill DEFAULT level 0");
    release(&ch, l);
    p.n = 0;
    pu(&p, D3DFVF_XYZRHW | D3DFVF_TEX1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetFVF, s.dev, 0, &p) == 0 && set_texture(&ch, s.dev, tex) == 0 &&
              sampler(&ch, s.dev, D3DSAMP_MIPFILTER, D3DTEXF_POINT) == 0,
          "LOD draw state");
    CHECK((px = lod_draw(&ch, s.dev)) == colors[0], "no LOD: level 0 (0x%06x)", px);
    CHECK(set_lod(&ch, tex, 1) == 0u && (px = lod_draw(&ch, s.dev)) == colors[1], "LOD 1: level 1 (0x%06x)", px);
    CHECK(set_lod(&ch, tex, 2) == 1u && (px = lod_draw(&ch, s.dev)) == colors[2], "LOD 2: level 2 (0x%06x)", px);
    CHECK(sampler(&ch, s.dev, D3DSAMP_MAXMIPLEVEL, 3) == 0 && (px = lod_draw(&ch, s.dev)) == colors[3],
          "MAXMIPLEVEL 3 over LOD 2: level 3 (0x%06x)", px);
    CHECK(sampler(&ch, s.dev, D3DSAMP_MIPFILTER, D3DTEXF_NONE) == 0 && set_lod(&ch, tex, 1) == 2u &&
              (px = lod_draw(&ch, s.dev)) == colors[1],
          "MIPFILTER NONE, MAXMIPLEVEL 3, LOD 1: level 1 (0x%06x)", px);
    CHECK(get_sampler(&ch, s.dev, D3DSAMP_MAXMIPLEVEL) == 3u && get_sampler(&ch, s.dev, D3DSAMP_MIPFILTER) == D3DTEXF_NONE &&
              get_sampler(&ch, s.dev, D3DSAMP_MIPMAPLODBIAS) == 0u,
          "sampler states stay the guest's after the draw");
    /* A state block captures the guest's values, not the draw's. */
    {
        uint32_t args[1] = { D3DSBT_ALL };

        CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateStateBlock, s.dev, args, 1, &sb, 0) == 0 &&
                  ch.ret == (uint32_t)S_OK,
              "CreateStateBlock ret 0x%08x", ch.ret);
        CHECK(sampler(&ch, s.dev, D3DSAMP_MAXMIPLEVEL, 0) == 0 &&
                  call(&ch, QD3D_OP_IDirect3DStateBlock9_Apply, sb, 1, NULL) == 0 &&
                  get_sampler(&ch, s.dev, D3DSAMP_MAXMIPLEVEL) == 3u,
              "state block restores MAXMIPLEVEL 3");
        CHECK((px = lod_draw(&ch, s.dev)) == colors[1], "after Apply: level 1 (0x%06x)", px);
        release(&ch, sb);
    }
    CHECK(sampler(&ch, s.dev, D3DSAMP_MIPFILTER, D3DTEXF_POINT) == 0 &&
              sampler(&ch, s.dev, D3DSAMP_MAXMIPLEVEL, 0) == 0 && set_lod(&ch, tex, 9) == 1u &&
              (px = lod_draw(&ch, s.dev)) == colors[3],
          "LOD 9 clamps to the last level (0x%06x)", px);
    CHECK(set_lod(&ch, tex, 0) == 3u && (px = lod_draw(&ch, s.dev)) == colors[0], "LOD 0 again: level 0 (0x%06x)",
          px);
    CHECK(set_lod(&ch, tex, 2) == 0u && set_texture(&ch, s.dev, plain) == 0 && set_lod(&ch, plain, 2) == 0u &&
              (px = lod_draw(&ch, s.dev)) == colors[0],
          "DEFAULT texture: SetLOD answers 0 and changes nothing (0x%06x)", px);
    CHECK(set_texture(&ch, s.dev, tex) == 0 && (px = lod_draw(&ch, s.dev)) == colors[2],
          "rebinding the managed texture keeps its LOD (0x%06x)", px);
    {
        struct qd3d_wire_pp pp = make_pp(16, 16);

        p.n = 0;
        praw(&p, &pp, sizeof pp);
        CHECK(call(&ch, QD3D_OP_RESET, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK, "LOD RESET ret 0x%08x", ch.ret);
        p.n = 0;
        pu(&p, D3DFVF_XYZRHW | D3DFVF_TEX1);
        CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_SetFVF, s.dev, 0, &p) == 0 && set_texture(&ch, s.dev, tex) == 0 &&
                  sampler(&ch, s.dev, D3DSAMP_MIPFILTER, D3DTEXF_POINT) == 0 &&
                  (px = lod_draw(&ch, s.dev)) == colors[2],
              "LOD survives RESET (0x%06x)", px);
    }
    if (g_failures == failures)
        printf("  ok  managed LOD: max(MAXMIPLEVEL, LOD) with mip filtering, LOD level without, guest states kept\n");
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "LOD GOODBYE");
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (k) Released frame sources                                               */
/* ------------------------------------------------------------------------ */

/* The events since mark are exactly want[0..n), in order. */
static int ev_since(const struct chan *ch, uint32_t mark, const uint32_t *want, uint32_t n)
{
    uint32_t i;

    if (ch->nev != mark + n || ch->nev > sizeof ch->ev / sizeof ch->ev[0])
        return 0;
    for (i = 0; i < n; i++)
        if (ch->ev[mark + i] != want[i])
            return 0;
    return 1;
}

/* The embedder learns when the device or additional swap chain of the last
 * published frame is released, after every frame presented before reached
 * the frame hook; releasing anything else (a device whose frames are not the
 * last published, a surface, the implicit swap chain's id as Reset does, a
 * RESET) says nothing; GOODBYE, the next HELLO and qd3dx_reset end the
 * source shown too; qd3dx_destroy calls no hook. */
static void test_source_released(void)
{
    static const uint32_t rel[] = { EV_RELEASED };
    struct chan ch;
    struct session s, a, b;
    struct pl p = { .n = 0 };
    uint32_t mark, sc, bb, sc0;
    int failures = g_failures;

    printf("[source released]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "release session");

    /* The frame still in flight when its device goes reaches the hook first. */
    mark = ch.nev;
    CHECK(clear(&ch, s.dev, 0x00a00000u) == 0 && present(&ch, s.dev) == 0, "PRESENT before the release");
    release(&ch, s.dev);
    {
        const uint32_t want[] = { 0x00a00000u, EV_RELEASED };

        CHECK(ev_since(&ch, mark, want, 2), "device release: frame, then release (%u events)", ch.nev - mark);
    }

    /* Two devices: releasing the one whose frame is not the last published
     * publishes the other's frame in flight and says nothing. */
    a = s;
    b = s;
    CHECK(create_device(&ch, &a, 16, 16) && create_device(&ch, &b, 16, 16), "two devices");
    CHECK(clear(&ch, a.dev, 0x0000a000u) == 0 && present(&ch, a.dev) == 0 && idle(&ch), "device A shown");
    mark = ch.nev;
    CHECK(clear(&ch, b.dev, 0x000000a0u) == 0 && present(&ch, b.dev) == 0, "device B presents");
    release(&ch, a.dev);
    {
        const uint32_t want[] = { 0x000000a0u };

        CHECK(ev_since(&ch, mark, want, 1), "releasing device A: B's frame only (%u events)", ch.nev - mark);
    }
    mark = ch.nev;
    release(&ch, b.dev);
    CHECK(ev_since(&ch, mark, rel, 1), "releasing device B: release (%u events)", ch.nev - mark);

    /* An additional swap chain shown: its release, after its frame. */
    CHECK(create_device(&ch, &s, 16, 16), "swap chain device");
    CHECK(create_swapchain(&ch, s.dev, 32, 32, &sc) == 0 && ch.ret == (uint32_t)S_OK, "CREATE_SWAPCHAIN 0x%08x",
          ch.ret);
    pu(&p, 0);
    pu(&p, D3DBACKBUFFER_TYPE_MONO);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DSwapChain9_GetBackBuffer, sc, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "swap chain GetBackBuffer 0x%08x", ch.ret);
    bb = reply_u32(&ch, 0);
    CHECK(color_fill(&ch, s.dev, bb, 0x00a0a000u) == 0 && ch.ret == (uint32_t)S_OK, "ColorFill swap chain");
    release(&ch, bb);
    CHECK(clear(&ch, s.dev, 0x00102030u) == 0 && present(&ch, s.dev) == 0 && idle(&ch), "implicit swap chain shown");
    mark = ch.nev;
    CHECK(present(&ch, sc) == 0, "the larger swap chain presents");
    release(&ch, sc);
    {
        const uint32_t want[] = { 0x00a0a000u, EV_RELEASED };

        CHECK(ev_since(&ch, mark, want, 2), "swap chain release: its frame, then release (%u events)", ch.nev - mark);
    }
    /* The display policy forgot it: the implicit swap chain shows at once. */
    mark = ch.nev;
    CHECK(clear(&ch, s.dev, 0x00203040u) == 0 && present(&ch, s.dev) == 0 && idle(&ch), "implicit swap chain again");
    {
        const uint32_t want[] = { 0x00203040u };

        CHECK(ev_since(&ch, mark, want, 1), "implicit swap chain shown after the release (%u events)", ch.nev - mark);
    }
    /* The implicit swap chain's own id and RESET are no source. */
    p.n = 0;
    pu(&p, 0);
    pu(&p, 1);
    CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetSwapChain, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
          "GetSwapChain(0) 0x%08x", ch.ret);
    sc0 = reply_u32(&ch, 0);
    mark = ch.nev;
    release(&ch, sc0);
    {
        struct qd3d_wire_pp pp = make_pp(16, 16);

        p.n = 0;
        praw(&p, &pp, sizeof pp);
        CHECK(call(&ch, QD3D_OP_RESET, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK, "RESET 0x%08x", ch.ret);
    }
    CHECK(ch.nev == mark, "implicit swap chain id and RESET release no source (%u events)", ch.nev - mark);

    /* GOODBYE, the next HELLO and qd3dx_reset end the source shown. */
    CHECK(clear(&ch, s.dev, 0x00304050u) == 0 && present(&ch, s.dev) == 0, "PRESENT before GOODBYE");
    mark = ch.nev;
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0, "GOODBYE");
    {
        const uint32_t want[] = { 0x00304050u, EV_RELEASED };

        CHECK(ev_since(&ch, mark, want, 2), "GOODBYE: frame, then release (%u events)", ch.nev - mark);
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16) && clear(&ch, s.dev, 0x00405060u) == 0 &&
              present(&ch, s.dev) == 0 && idle(&ch),
          "a session that ends without GOODBYE");
    mark = ch.nev;
    CHECK(hello(&ch, &s) && ev_since(&ch, mark, rel, 1), "the next HELLO ends its source (%u events)",
          ch.nev - mark);
    CHECK(create_device(&ch, &s, 16, 16) && clear(&ch, s.dev, 0x00506070u) == 0 && present(&ch, s.dev) == 0 &&
              idle(&ch),
          "a session reset by the embedder");
    mark = ch.nev;
    qd3dx_reset(ch.ex);
    CHECK(ev_since(&ch, mark, rel, 1), "qd3dx_reset ends its source (%u events)", ch.nev - mark);
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16) && clear(&ch, s.dev, 0x00607080u) == 0 &&
              present(&ch, s.dev) == 0 && idle(&ch),
          "a session the executor ends");
    mark = ch.nev;
    chan_close(&ch);
    CHECK(ch.nev == mark, "qd3dx_destroy calls no hook (%u events)", ch.nev - mark);
    if (g_failures == failures)
        printf("  ok  released device or swap chain of the last published frame reported after its frames; "
               "other releases, RESET and destroy silent\n");
}

/* ------------------------------------------------------------------------ */
/* (l) Cursor between presents                                              */
/* ------------------------------------------------------------------------ */

static uint32_t cursor_pos(struct chan *ch, uint32_t visible, int32_t x, int32_t y)
{
    struct pl p = { .n = 0 };
    struct qd3d_cursor_pos cp = { visible, { x, y } };

    praw(&p, &cp, sizeof cp);
    return call(ch, QD3D_OP_CURSOR_POS, 0, 0, &p);
}

/* Lets the executor go idle (qd3dx_prepare_wait) until the frame hook saw n
 * frames in all or ms passed; 1 when it did. */
static int frames_by(struct chan *ch, uint32_t n, DWORD ms)
{
    DWORD t0 = GetTickCount();

    while (ch->frame.count < n && GetTickCount() - t0 < ms) {
        idle(ch);
        Sleep(1);
    }
    return ch->frame.count >= n;
}

/* Between presents the cursor moves like a hardware cursor plane: a
 * CURSOR_POS publishes the last frame again with the cursor at its new
 * position (QD3DX_REPEAT_CURSOR, frames_done unchanged), a burst faster
 * than the re-publish interval ends with its last position, a cursor off
 * the frame takes it out once and then re-publishes nothing, a new image
 * shows with the CURSOR_POS after it, hiding it shows the clean frame; a
 * frame in flight takes the new position along instead; a PRESENT
 * supersedes a pending update; nothing comes after the source's release;
 * malformed updates fault. */
static void test_cursor_follow(void)
{
    static uint32_t img[8 * 8];
    static const int32_t screen[4] = { 100, 200, 164, 264 };
    const uint32_t bg = 0x00203040u;
    struct chan ch;
    struct session s;
    struct pl p = { .n = 0 };
    uint32_t i, n, done, tail;
    int failures = g_failures;

    printf("[cursor follow]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    for (i = 0; i < 64u; i++)
        img[i] = 0xffff0000u;
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 64, 64), "cursor follow session");
    cursor_payload(&p, 8, 8, 0, 0, img);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0, "CURSOR 8x8");
    CHECK(clear(&ch, s.dev, bg) == 0 && present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0 && idle(&ch) &&
              ch.frame.repeat == QD3DX_REPEAT_NONE && frame_px(&ch, 10, 20) == 0xff0000u,
          "presented frame with the cursor at (10, 20)");
    n = ch.frame.count;
    done = ch.ctrl->frames_done;
    Sleep(10);
    CHECK(cursor_pos(&ch, 1, 130, 240) == 0 && frames_by(&ch, n + 1u, 100) &&
              ch.frame.repeat == QD3DX_REPEAT_CURSOR && frame_px(&ch, 30, 40) == 0xff0000u &&
              frame_px(&ch, 29, 40) == bg && frame_px(&ch, 10, 20) == bg,
          "re-published with the cursor at (30, 40): %u frames, repeat %u, 0x%06x", ch.frame.count - n,
          ch.frame.repeat, frame_px(&ch, 30, 40));
    CHECK(ch.ctrl->frames_done == done, "a re-publication is no presented frame (frames_done %u, was %u)",
          ch.ctrl->frames_done, done);

    /* A burst faster than the interval: its last position shows. */
    n = ch.frame.count;
    Sleep(10);
    for (i = 0; i < 10u; i++)
        CHECK(cursor_pos(&ch, 1, 101 + (int32_t)i * 5, 201) == 0, "CURSOR_POS %u of the burst", i);
    frames_by(&ch, n + 10u, 60);
    CHECK(ch.frame.count - n >= 1u && ch.frame.count - n <= 3u && frame_px(&ch, 46, 1) == 0xff0000u &&
              frame_px(&ch, 45, 1) == bg,
          "10 updates in a burst: %u re-publications, the last position shown (0x%06x)", ch.frame.count - n,
          frame_px(&ch, 46, 1));

    /* Off the frame: once without the cursor, then nothing. */
    n = ch.frame.count;
    Sleep(10);
    CHECK(cursor_pos(&ch, 1, 10, 10) == 0 && frames_by(&ch, n + 1u, 100) && ch.frame.uniform &&
              (ch.frame.first & 0x00ffffffu) == bg,
          "cursor moved off the frame: the clean frame re-published");
    n = ch.frame.count;
    Sleep(10);
    CHECK(cursor_pos(&ch, 1, 20, 20) == 0 && !frames_by(&ch, n + 1u, 30),
          "a cursor moving off the frame re-publishes nothing (%u frames)", ch.frame.count - n);

    /* A new image between presents shows with the CURSOR_POS after it. */
    for (i = 0; i < 64u; i++)
        img[i] = 0xff00ff00u;
    cursor_payload(&p, 8, 8, 0, 0, img);
    Sleep(10);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0 && ch.frame.count == n && cursor_pos(&ch, 1, 110, 220) == 0 &&
              frames_by(&ch, n + 1u, 100) && frame_px(&ch, 10, 20) == 0x00ff00u,
          "a new image between presents (CURSOR, then CURSOR_POS): 0x%06x", frame_px(&ch, 10, 20));
    for (i = 0; i < 64u; i++)
        img[i] = 0xffff0000u;
    cursor_payload(&p, 8, 8, 0, 0, img);
    CHECK(call(&ch, QD3D_OP_CURSOR, 0, 0, &p) == 0, "CURSOR red again");

    /* Hidden: the clean frame again. */
    n = ch.frame.count;
    Sleep(10);
    CHECK(cursor_pos(&ch, 0, 0, 0) == 0 && frames_by(&ch, n + 1u, 100) && ch.frame.repeat == QD3DX_REPEAT_CURSOR &&
              ch.frame.uniform && (ch.frame.first & 0x00ffffffu) == bg,
          "hidden cursor: the clean frame re-published");

    /* A frame in flight takes the new position along. */
    n = ch.frame.count;
    CHECK(clear(&ch, s.dev, 0x00405060u) == 0 && present_ex(&ch, s.dev, NULL, screen, 1, 110, 220) == 0 &&
              cursor_pos(&ch, 1, 140, 250) == 0 && idle(&ch) && ch.frame.count == n + 1u &&
              ch.frame.repeat == QD3DX_REPEAT_NONE && frame_px(&ch, 40, 50) == 0xff0000u &&
              frame_px(&ch, 10, 20) == 0x405060u,
          "the frame in flight is published with the newer position (%u frames)", ch.frame.count - n);

    /* A PRESENT supersedes a pending update: nothing older comes after it. */
    CHECK(cursor_pos(&ch, 1, 120, 230) == 0 && clear(&ch, s.dev, 0x00506070u) == 0 &&
              present_ex(&ch, s.dev, NULL, screen, 1, 150, 210) == 0 && idle(&ch),
          "CURSOR_POS, then PRESENT");
    n = ch.frame.count;
    Sleep(20);
    CHECK(idle(&ch) && ch.frame.count == n && ch.frame.repeat == QD3DX_REPEAT_NONE &&
              frame_px(&ch, 50, 10) == 0xff0000u && frame_px(&ch, 20, 30) == 0x506070u,
          "the presented frame stays last with the PRESENT's cursor (%u frames after it)", ch.frame.count - n);

    /* Nothing after the source's release. */
    release(&ch, s.dev);
    n = ch.frame.count;
    Sleep(10);
    CHECK(cursor_pos(&ch, 1, 111, 211) == 0 && !frames_by(&ch, n + 1u, 30),
          "nothing re-published after the device's release");
    if (g_failures == failures)
        printf("  ok  cursor re-published between presents (moved, burst, off the frame, new image, hidden), "
               "frame in flight updated, PRESENT and release supersede it\n");

    /* Malformed CURSOR_POS commands. */
    {
        struct qd3d_cursor_pos cp = { 2u, { 0, 0 } };

        p.n = 0;
        praw(&p, &cp, sizeof cp);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CURSOR_POS, 0, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CURSOR_POS visible 2", run(&ch), QD3D_FAULT_DECODE, tail);
        CHECK(session_ready(&ch, &s), "cursor follow fault session");
        cp.visible = 1;
        p.n = 0;
        praw(&p, &cp, sizeof cp);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CURSOR_POS, 1, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CURSOR_POS obj 1", run(&ch), QD3D_FAULT_DECODE, tail);
        CHECK(session_ready(&ch, &s), "cursor follow fault session 2");
        p.n -= 4u;
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_CURSOR_POS, 0, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "CURSOR_POS short", run(&ch), QD3D_FAULT_DECODE, tail);
    }
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (m) Minimized and hidden windows                                         */
/* ------------------------------------------------------------------------ */

static uint32_t source_shown(struct chan *ch, uint32_t obj, uint32_t shown)
{
    struct pl p = { .n = 0 };
    struct qd3d_source_shown ss = { shown };

    praw(&p, &ss, sizeof ss);
    return call(ch, QD3D_OP_SOURCE_SHOWN, obj, 0, &p);
}

/* A PRESENT whose window is minimized or hidden. */
static uint32_t present_hidden(struct chan *ch, uint32_t obj, const int32_t *screen)
{
    struct pl p = { .n = 0 };
    struct qd3d_present pr;

    memset(&pr, 0, sizeof pr);
    memcpy(pr.screen, screen, sizeof pr.screen);
    pr.hidden = 1;
    praw(&p, &pr, sizeof pr);
    ch->ctrl->frames_submitted++;
    return call(ch, QD3D_OP_PRESENT, obj, 0, &p);
}

/* A frame source's window minimized or hidden: frames presented to it are
 * not published (frames_done counts them) and, for the source of the last
 * published frame, the embedder hears source_hidden once, after what was
 * presented before; nothing of it is published (cursor, qd3dx_show_again)
 * until it shows: SOURCE_SHOWN(1) re-publishes its frame
 * (QD3DX_REPEAT_SHOWN), a visible PRESENT publishes a new one; another
 * source's window says nothing; an additional swap chain shown answers for
 * its own window; malformed SOURCE_SHOWN faults. */
static void test_source_shown(void)
{
    static const uint32_t hid[] = { EV_HIDDEN };
    static const int32_t screen[4] = { 0, 0, 16, 16 };
    struct chan ch;
    struct session s, b;
    struct pl p = { .n = 0 };
    uint32_t mark, sc = 0, sc0, tail;
    int failures = g_failures;

    printf("[hidden windows]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "hidden session");

    /* A hidden PRESENT of the source shown: the frame in flight first. */
    mark = ch.nev;
    CHECK(clear(&ch, s.dev, 0x00a00000u) == 0 && present_ex(&ch, s.dev, NULL, screen, 0, 0, 0) == 0 &&
              clear(&ch, s.dev, 0x0000a000u) == 0 && present_hidden(&ch, s.dev, screen) == 0 && idle(&ch),
          "visible PRESENT, then hidden PRESENT");
    {
        const uint32_t want[] = { 0x00a00000u, EV_HIDDEN };

        CHECK(ev_since(&ch, mark, want, 2), "hidden PRESENT: the frame before, then source_hidden (%u events)",
              ch.nev - mark);
    }
    CHECK(ch.ctrl->frames_done == ch.ctrl->frames_submitted, "hidden frames count as done (%u of %u)",
          ch.ctrl->frames_done, ch.ctrl->frames_submitted);

    /* While hidden nothing is published. */
    mark = ch.nev;
    Sleep(10);
    CHECK(present_hidden(&ch, s.dev, screen) == 0 && cursor_pos(&ch, 1, 5, 5) == 0 && idle(&ch),
          "hidden: PRESENT, CURSOR_POS");
    qd3dx_show_again(ch.ex);
    CHECK(ch.nev == mark && ch.ctrl->frames_done == ch.ctrl->frames_submitted,
          "nothing published while hidden (%u events)", ch.nev - mark);

    /* Shown again between presents: the last frame again. */
    mark = ch.nev;
    CHECK(source_shown(&ch, s.dev, 1) == 0 && ch.nev == mark + 1u && ch.frame.repeat == QD3DX_REPEAT_SHOWN &&
              ch.frame.uniform && (ch.frame.first & 0x00ffffffu) == 0xa00000u,
          "shown again: the last published frame again (%u events, repeat %u)", ch.nev - mark, ch.frame.repeat);
    mark = ch.nev;
    CHECK(source_shown(&ch, s.dev, 1) == 0 && ch.nev == mark, "shown while shown: nothing");

    /* Minimized between presents: source_hidden once; a visible PRESENT
     * shows again. */
    mark = ch.nev;
    CHECK(source_shown(&ch, s.dev, 0) == 0 && source_shown(&ch, s.dev, 0) == 0 && ev_since(&ch, mark, hid, 1),
          "minimized between presents: source_hidden once (%u events)", ch.nev - mark);
    mark = ch.nev;
    CHECK(clear(&ch, s.dev, 0x000000a0u) == 0 && present_ex(&ch, s.dev, NULL, screen, 0, 0, 0) == 0 && idle(&ch) &&
              ch.nev == mark + 1u && ch.frame.repeat == QD3DX_REPEAT_NONE &&
              (ch.frame.first & 0x00ffffffu) == 0x0000a0u,
          "a visible PRESENT after hiding is published (%u events)", ch.nev - mark);
    mark = ch.nev;
    qd3dx_show_again(ch.ex);
    CHECK(ch.nev == mark + 1u && ch.frame.repeat == QD3DX_REPEAT_SHOWN && (ch.frame.first & 0x00ffffffu) == 0x0000a0u,
          "qd3dx_show_again re-publishes the frame shown (%u events)", ch.nev - mark);

    /* Another source's window says nothing. */
    b = s;
    CHECK(create_device(&ch, &b, 16, 16), "second device");
    mark = ch.nev;
    CHECK(source_shown(&ch, b.dev, 0) == 0 && present_hidden(&ch, b.dev, screen) == 0 && idle(&ch) && ch.nev == mark,
          "another device's window minimized: nothing (%u events)", ch.nev - mark);
    release(&ch, b.dev);

    /* An additional swap chain shown answers for its own window. */
    CHECK(create_swapchain(&ch, s.dev, 32, 32, &sc) == 0 && ch.ret == (uint32_t)S_OK, "CREATE_SWAPCHAIN 0x%08x",
          ch.ret);
    CHECK(present_ex(&ch, sc, NULL, screen, 0, 0, 0) == 0 && idle(&ch) && ch.frame.width == 32u,
          "the larger swap chain is shown");
    mark = ch.nev;
    CHECK(source_shown(&ch, s.dev, 0) == 0 && ch.nev == mark, "the device's window minimized: not the source shown");
    CHECK(source_shown(&ch, sc, 0) == 0 && ev_since(&ch, mark, hid, 1), "the swap chain's window minimized");
    release(&ch, sc);
    if (g_failures == failures)
        printf("  ok  hidden frames not published, source_hidden once for the source shown, shown again or "
               "presented brings frames back, other sources silent\n");

    /* Malformed SOURCE_SHOWN: shown 2, the implicit swap chain's host id,
     * an unknown id. */
    {
        struct qd3d_source_shown ss = { 2u };

        p.n = 0;
        praw(&p, &ss, sizeof ss);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_SOURCE_SHOWN, s.dev, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "SOURCE_SHOWN shown 2", run(&ch), QD3D_FAULT_DECODE, tail);
        CHECK(session_ready(&ch, &s) && create_device(&ch, &s, 16, 16), "hidden fault session");
        p.n = 0;
        pu(&p, 0);
        pu(&p, 1);
        CHECK(call(&ch, QD3D_OP_IDirect3DDevice9_GetSwapChain, s.dev, 1, &p) == 0 && ch.ret == (uint32_t)S_OK,
              "GetSwapChain(0) 0x%08x", ch.ret);
        sc0 = reply_u32(&ch, 0);
        ss.shown = 0;
        p.n = 0;
        praw(&p, &ss, sizeof ss);
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_SOURCE_SHOWN, sc0, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "SOURCE_SHOWN implicit swap chain", run(&ch), QD3D_FAULT_OBJECT, tail);
        CHECK(session_ready(&ch, &s), "hidden fault session 2");
        tail = ch.ctrl->tail;
        put_cmd(&ch, QD3D_OP_SOURCE_SHOWN, 0x777u, 0, p.b, p.n, 16u + p.n);
        expect_fault(&ch, "SOURCE_SHOWN unknown id", run(&ch), QD3D_FAULT_OBJECT, tail);
    }
    chan_close(&ch);
}

/* ------------------------------------------------------------------------ */
/* (n) Liveness and the display policy                                      */
/* ------------------------------------------------------------------------ */

/* qd3dx_alive answers commands executed and heartbeat moves since its last
 * answer. The display policy (synthetic times) shows presented frames and
 * frames shown again of any slot and cursor repeats only of the slot shown;
 * it returns to the desktop for a released or hidden source of that slot
 * only, after QD3DX_DISPLAY_IDLE_US of its silence and never while it
 * beats, and asks once for the frames of a silent slot that lives again. */
static void test_alive_display(void)
{
    const uint64_t idle_us = QD3DX_DISPLAY_IDLE_US;
    struct qd3dx_display d;
    struct chan ch;
    struct session s;
    uint64_t t;
    int failures = g_failures;

    printf("[liveness, display policy]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    (void)qd3dx_alive(ch.ex);
    CHECK(!qd3dx_alive(ch.ex), "no sign of life without commands or heartbeat");
    ch.ctrl->heartbeat++;
    CHECK(qd3dx_alive(ch.ex) && !qd3dx_alive(ch.ex), "a heartbeat is one sign of life");
    CHECK(hello(&ch, &s) && qd3dx_alive(ch.ex) && !qd3dx_alive(ch.ex), "commands are signs of life");
    chan_close(&ch);

    qd3dx_display_init(&d);
    CHECK(qd3dx_display_frame(&d, 1, QD3DX_REPEAT_CURSOR, 0) == QD3DX_DISPLAY_DROP,
          "a cursor repeat turns nothing on");
    CHECK(qd3dx_display_frame(&d, 1, QD3DX_REPEAT_NONE, 0) == QD3DX_DISPLAY_ON &&
              qd3dx_display_frame(&d, 1, QD3DX_REPEAT_CURSOR, 1000) == QD3DX_DISPLAY_SHOW,
          "a presented frame turns 3D on; its slot's cursor repeats show");
    CHECK(qd3dx_display_frame(&d, 0, QD3DX_REPEAT_CURSOR, 2000) == QD3DX_DISPLAY_DROP,
          "another slot's cursor repeat does not show");
    /* A static screen whose guest beats stays up far past the idle time. */
    for (t = 1000; t < 10u * idle_us; t += idle_us / 2u) {
        qd3dx_display_alive(&d, 1, t);
        CHECK(!qd3dx_display_idle(&d, t + 1000u), "a beating slot went idle at %llu us", (unsigned long long)t);
    }
    /* Another slot's life keeps nothing up; silence ends 3D. */
    t = 10u * idle_us;
    qd3dx_display_alive(&d, 1, t);
    qd3dx_display_alive(&d, 0, t + idle_us);
    CHECK(!qd3dx_display_idle(&d, t + idle_us) && qd3dx_display_idle(&d, t + idle_us + 1u) && !d.on,
          "silence for the idle time: desktop");
    CHECK(qd3dx_display_frame(&d, 1, QD3DX_REPEAT_CURSOR, t + idle_us + 2u) == QD3DX_DISPLAY_DROP,
          "no cursor repeat shows after it");
    CHECK(qd3dx_display_alive(&d, 1, t + idle_us + 3u) && !qd3dx_display_alive(&d, 1, t + idle_us + 4u),
          "the silent slot lives again: its frames asked for once");
    CHECK(qd3dx_display_frame(&d, 1, QD3DX_REPEAT_SHOWN, t + idle_us + 5u) == QD3DX_DISPLAY_ON,
          "its frame shown again turns 3D on");
    /* Released or hidden sources of the slot shown end 3D at once. */
    CHECK(!qd3dx_display_gone(&d, 0) && d.on, "another slot's source gone: nothing");
    CHECK(qd3dx_display_gone(&d, 1) && !d.on && !qd3dx_display_gone(&d, 1), "the shown slot's source gone: desktop");
    CHECK(!qd3dx_display_alive(&d, 1, t + 2u * idle_us), "life after a source gone asks for nothing");
    CHECK(!qd3dx_display_idle(&d, t + 10u * idle_us), "no idle transition while the desktop shows");
    CHECK(qd3dx_display_frame(&d, 0, QD3DX_REPEAT_NONE, t + 10u * idle_us) == QD3DX_DISPLAY_ON &&
              qd3dx_display_off(&d) && !qd3dx_display_off(&d),
          "off: desktop once");
    if (g_failures == failures)
        printf("  ok  liveness from commands and heartbeat; display policy: frames, cursor repeats, gone, idle "
               "after %u ms of silence only, asked again once\n",
               QD3DX_DISPLAY_IDLE_US / 1000u);
}

/* ------------------------------------------------------------------------ */
/* (o) Slot ownership release                                               */
/* ------------------------------------------------------------------------ */

struct release_slot {
    struct chan ch;
    struct qd3dx_display *display;
    unsigned index;
    int release_on_frame;
    uint32_t release_head, successor_seq;
};

/* Append a claimant's synchronous HELLO without running the executor. */
static uint32_t release_hello(struct chan *ch, uint32_t pid)
{
    struct qd3d_hello h = { QD3D_ABI_VERSION, pid, D3D_SDK_VERSION, new_id(ch) };
    uint32_t seq = ++ch->seq;

    put_cmd(ch, QD3D_OP_HELLO, 0, seq, &h, sizeof h, 16u + sizeof h);
    return seq;
}

static void release_slot_frame(void *user, const struct qd3dx_frame *f)
{
    struct release_slot *s = user;

    on_frame(&s->ch, f);
    qd3dx_display_frame(s->display, s->index, f->repeat, 0);
    if (s->release_on_frame) {
        /* A deterministic handoff while an old command is in progress:
         * its callback may finish, but later old packets must not run. */
        s->release_on_frame = 0;
        s->release_head = s->ch.ctrl->head;
        qd3dx_request_release(s->ch.ex);
        s->successor_seq = release_hello(&s->ch, 0x12345678u);
    }
}

static void release_slot_gone(void *user)
{
    struct release_slot *s = user;

    on_source_released(&s->ch);
    qd3dx_display_gone(s->display, s->index);
}

static int release_slot_open(struct release_slot *s, struct qd3dx_display *d, unsigned index)
{
    memset(s, 0, sizeof *s);
    if (!chan_open(&s->ch))
        return 0;
    s->display = d;
    s->index = index;
    s->ch.ex->hooks.user = s;
    s->ch.ex->hooks.frame = release_slot_frame;
    s->ch.ex->hooks.source_released = release_slot_gone;
    s->ch.ex->hooks.source_hidden = NULL;
    return 1;
}

static DWORD WINAPI release_thread(void *arg)
{
    qd3dx_request_release(arg);
    return 0;
}

/* The vCPU counterpart has no executor affinity and must call no hooks. */
static void release_elsewhere(struct qd3dx *ex)
{
    HANDLE t = CreateThread(NULL, 0, release_thread, ex, 0, NULL);

    CHECK(t != NULL, "create release request thread");
    if (!t)
        return;
    CHECK(WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0, "release request thread returned");
    CloseHandle(t);
}

static void test_slot_release(void)
{
    static const uint32_t rel[] = { EV_RELEASED };
    struct release_slot a, b;
    struct qd3dx_display d;
    struct session sa, sb;
    struct qd3d_present pr;
    uint32_t mark, frames, cut, seq;
    int failures = g_failures;

    printf("[slot release]\n");
    qd3dx_display_init(&d);
    if (!release_slot_open(&a, &d, 0)) {
        CHECK(0, "cannot open release slot A");
        return;
    }
    if (!release_slot_open(&b, &d, 1)) {
        CHECK(0, "cannot open release slot B");
        chan_close(&a.ch);
        return;
    }

    /* Head zero must still carry a release request; no session or source
     * has ever existed here. Both the poll and wait paths service it. */
    release_elsewhere(a.ch.ex);
    CHECK(a.ch.ex->release_request != 0 && a.ch.nev == 0, "idle release is pending, not executed off-thread");
    CHECK(qd3dx_poll(a.ch.ex, 0) == 0 && idle(&a.ch) && !a.ch.ex->release_request &&
              a.ch.ctrl->host_state == QD3D_HOST_READY && a.ch.ctrl->tail == 0 && a.ch.nev == 0,
          "release of a never-used idle slot");
    CHECK(hello(&a.ch, &sa) && create_device(&a.ch, &sa, 16, 16) &&
              clear(&a.ch, sa.dev, 0x00112233u) == 0 && present(&a.ch, sa.dev) == 0 && idle(&a.ch),
          "slot A shown");
    CHECK(hello(&b.ch, &sb) && create_device(&b.ch, &sb, 16, 16) &&
              clear(&b.ch, sb.dev, 0x00445566u) == 0 && present(&b.ch, sb.dev) == 0 && idle(&b.ch) &&
              d.on && d.slot == 1,
          "slot B replaces slot A on the display");

    /* Release a nonshown slot mid-frame, with queued stale HELLO and a
     * malformed packet that would resurrect/fault it if either executed. */
    CHECK(clear(&a.ch, sa.dev, 0x00778899u) == 0 && present(&a.ch, sa.dev) == 0, "slot A mid-frame");
    release_hello(&a.ch, 0xbad00001u);
    put_cmd(&a.ch, 0xffffffffu, 0, 0, NULL, 0, 16);
    a.ch.ctrl->frames_submitted++; /* old producer died before publishing PRESENT */
    cut = a.ch.ctrl->head;
    mark = a.ch.nev;
    frames = a.ch.frame.count;
    release_elsewhere(a.ch.ex);
    CHECK(a.ch.nev == mark && a.ch.ex->inflight_dev && qd3dx_live_objects(a.ch.ex) != 0 &&
              qd3dx_session_pid(a.ch.ex) == GetCurrentProcessId(),
          "request thread did not release objects, publish frames or call hooks");
    CHECK(qd3dx_poll(a.ch.ex, 0) == 0 && a.ch.ctrl->tail == cut &&
              a.ch.ctrl->host_state == QD3D_HOST_READY && qd3dx_live_objects(a.ch.ex) == 0 &&
              !a.ch.ex->devices && !a.ch.ex->inflight_dev && !qd3dx_session_pid(a.ch.ex) &&
              a.ch.ctrl->frames_done == a.ch.ctrl->frames_submitted && !qd3dx_alive(a.ch.ex),
          "nonshown slot release drops objects, frame, stale packets and liveness");
    CHECK(a.ch.frame.count == frames && ev_since(&a.ch, mark, rel, 1) && d.on && d.slot == 1 &&
              qd3dx_live_objects(b.ch.ex) != 0,
          "nonshown release leaves the other slot and its display intact");
    mark = a.ch.nev;
    qd3dx_request_release(a.ch.ex);
    qd3dx_request_release(a.ch.ex);
    CHECK(idle(&a.ch) && a.ch.nev == mark && d.on && d.slot == 1, "double release of an idle slot");

    CHECK(clear(&b.ch, sb.dev, 0x00aabbccu) == 0 && present(&b.ch, sb.dev) == 0, "shown slot B mid-frame");
    mark = b.ch.nev;
    frames = b.ch.frame.count;
    release_elsewhere(b.ch.ex);
    CHECK(d.on && b.ch.nev == mark, "shown release is deferred to executor");
    CHECK(idle(&b.ch) && !d.on && ev_since(&b.ch, mark, rel, 1) && b.ch.frame.count == frames &&
              !qd3dx_live_objects(b.ch.ex) && !b.ch.ex->devices && !b.ch.ex->inflight_dev &&
              b.ch.ctrl->frames_done == b.ch.ctrl->frames_submitted,
          "prepare_wait releases the shown source, drops its frame and returns desktop");
    mark = b.ch.nev;
    qd3dx_request_release(b.ch.ex);
    qd3dx_request_release(b.ch.ex);
    CHECK(qd3dx_poll(b.ch.ex, 0) == 0 && idle(&b.ch) && b.ch.nev == mark && !d.on,
          "double release does not repeat the display callback");

    /* A driver can free ownership as soon as request_release returns,
     * without waiting for the executor. Only packets before the cut die. */
    release_hello(&a.ch, 0xbad00002u);
    qd3dx_request_release(a.ch.ex);
    cut = a.ch.ctrl->head;
    seq = release_hello(&a.ch, 0x10000001u);
    CHECK(qd3dx_poll(a.ch.ex, 0) == 0 && a.ch.ctrl->tail == cut &&
              a.ch.ctrl->head != cut && !qd3dx_live_objects(a.ch.ex),
          "release preserves a later claimant's queued HELLO");
    CHECK(run(&a.ch) == 0 && a.ch.ctrl->sync_done == seq && qd3dx_session_pid(a.ch.ex) == 0x10000001u &&
              qd3dx_live_objects(a.ch.ex) == 1,
          "only the later claimant starts a session");
    release_hello(&a.ch, 0xbad00003u);
    qd3dx_request_release(a.ch.ex);
    release_hello(&a.ch, 0xbad00004u);
    qd3dx_request_release(a.ch.ex);
    cut = a.ch.ctrl->head;
    seq = release_hello(&a.ch, 0x10000002u);
    CHECK(qd3dx_poll(a.ch.ex, 0) == 0 && a.ch.ctrl->tail == cut && !qd3dx_live_objects(a.ch.ex) &&
              run(&a.ch) == 0 && a.ch.ctrl->sync_done == seq && qd3dx_session_pid(a.ch.ex) == 0x10000002u &&
              qd3dx_live_objects(a.ch.ex) == 1,
          "coalesced releases use the latest cut, never an earlier stale HELLO");

    put_cmd(&a.ch, 0xffffffffu, 0, 0, NULL, 0, 16);
    CHECK(qd3dx_poll(a.ch.ex, 1) == -1, "fault before timed recovery races release");
    qd3dx_request_release(a.ch.ex);
    cut = a.ch.ctrl->head;
    seq = release_hello(&a.ch, 0x10000004u);
    qd3dx_reset(a.ch.ex);
    CHECK(a.ch.ctrl->tail == cut && a.ch.ctrl->host_state == QD3D_HOST_READY &&
              run(&a.ch) == 0 && a.ch.ctrl->sync_done == seq && qd3dx_session_pid(a.ch.ex) == 0x10000004u,
          "fault recovery honors a pending release instead of swallowing its successor");

    put_cmd(&a.ch, 0xffffffffu, 0, 0, NULL, 0, 16);
    CHECK(qd3dx_poll(a.ch.ex, 1) == -1, "fault before owner release");
    qd3dx_request_release(a.ch.ex);
    cut = a.ch.ctrl->head;
    seq = release_hello(&a.ch, 0x10000003u);
    CHECK(qd3dx_poll(a.ch.ex, 0) == 0 && a.ch.ctrl->tail == cut &&
              a.ch.ctrl->host_state == QD3D_HOST_READY && !a.ch.ctrl->fault_code &&
              run(&a.ch) == 0 && a.ch.ctrl->sync_done == seq && qd3dx_session_pid(a.ch.ex) == 0x10000003u,
          "release clears a fault immediately without losing its successor");

    /* The first frame's hook requests release while a second PRESENT
     * executes and appends a new claim before that command retires. */
    CHECK(hello(&b.ch, &sb) && create_device(&b.ch, &sb, 16, 16) &&
              clear(&b.ch, sb.dev, 0x00010203u) == 0 && present(&b.ch, sb.dev) == 0 &&
              clear(&b.ch, sb.dev, 0x00040506u) == 0,
          "session for an in-command release");
    memset(&pr, 0, sizeof pr);
    memcpy(pr.screen, b.ch.screen, sizeof pr.screen);
    b.ch.ctrl->frames_submitted++;
    put_cmd(&b.ch, QD3D_OP_PRESENT, sb.dev, 0, &pr, sizeof pr, 16u + sizeof pr);
    release_hello(&b.ch, 0xbad00005u);
    b.release_on_frame = 1;
    mark = b.ch.nev;
    frames = b.ch.frame.count;
    CHECK(qd3dx_poll(b.ch.ex, 1) == 1 && !b.release_on_frame && b.ch.ctrl->tail == b.release_head &&
              b.ch.ctrl->head != b.release_head && !qd3dx_live_objects(b.ch.ex) && !b.ch.ex->inflight_dev &&
              b.ch.ctrl->frames_done == b.ch.ctrl->frames_submitted,
          "release during a command is applied before the budget-limited poll returns");
    {
        const uint32_t want[] = { 0x00010203u, EV_RELEASED };

        CHECK(ev_since(&b.ch, mark, want, 2) && b.ch.frame.count == frames + 1 && !d.on,
              "a begun hook finishes, the later in-flight frame drops, then desktop");
    }
    CHECK(run(&b.ch) == 0 && b.ch.ctrl->sync_done == b.successor_seq &&
              qd3dx_session_pid(b.ch.ex) == 0x12345678u && qd3dx_live_objects(b.ch.ex) == 1,
          "handoff during a command preserves exactly the successor HELLO");
    frames = b.ch.frame.count;
    qd3dx_show_again(b.ch.ex);
    CHECK(idle(&b.ch) && b.ch.frame.count == frames && !d.on, "released frames cannot show again");

    chan_close(&b.ch);
    chan_close(&a.ch);
    CHECK(heaps_valid(), "heaps valid after slot releases");
    if (g_failures == failures)
        printf("  ok  idle, nonshown, mid-frame, repeated, coalesced and in-command slot releases; "
               "stale packets discarded and successor HELLO preserved\n");
}

/* ------------------------------------------------------------------------ */
/* (g) Index arguments and Reset defaults                                   */
/* ------------------------------------------------------------------------ */

/* A command answered with a reply; ret, or 0xffffffff when it faulted. */
static uint32_t sync_ret(struct chan *ch, uint32_t op, uint32_t obj, const struct pl *p)
{
    return call(ch, op, obj, 1, p) ? 0xffffffffu : ch->ret;
}

static void set3(struct pl *p, uint32_t a, uint32_t b, uint32_t c)
{
    p->n = 0;
    pu(p, a);
    pu(p, b);
    pu(p, c);
}

/* Out-of-range indices the native runtime crashes, hangs or corrupts its
 * heap on are answered without reaching it. */
static void test_index_args(struct chan *ch, const struct session *s, uint32_t vb, uint32_t tex)
{
    static const uint16_t bad[3] = { 0, 60000, 2 }, good[3] = { 0, 1, 2 };
    static const float tri[3][4] = { { 0, 0, 0, 1 }, { 8, 0, 0, 1 }, { 0, 8, 0, 1 } };
    static PALETTEENTRY pal[256];
    const uint32_t inv = (uint32_t)D3DERR_INVALIDCALL;
    struct pl p = { .n = 0 };
    D3DLIGHT9 light;
    int failures = g_failures;

    memset(&light, 0, sizeof light);
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Direction.z = 1.0f;
    pu(&p, 0xffffffffu);
    pdata(&p, &light, sizeof light);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetLight, s->dev, &p) == inv, "SetLight(0xffffffff)");
    p.n = 0;
    pu(&p, 0x10000000u);
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_LightEnable, s->dev, &p) == inv, "LightEnable(0x10000000)");
    p.n = 0;
    pu(&p, 0xffffffffu);
    pdata(&p, pal, sizeof pal);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetPaletteEntries, s->dev, &p) == inv, "SetPaletteEntries(0xffffffff)");
    p.n = 0;
    pu(&p, 7);
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetPaletteEntries, s->dev, &p) == inv, "GetPaletteEntries(unset)");
    p.n = 0;
    pu(&p, 7);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetCurrentTexturePalette, s->dev, &p) == inv,
          "SetCurrentTexturePalette(unset)");
    p.n = 0;
    pu(&p, 7);
    pdata(&p, pal, sizeof pal);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetPaletteEntries, s->dev, &p) == (uint32_t)D3D_OK,
          "SetPaletteEntries(7)");
    p.n = 0;
    pu(&p, 7);
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetPaletteEntries, s->dev, &p) == (uint32_t)D3D_OK,
          "GetPaletteEntries(7)");
    p.n = 0;
    pu(&p, 7);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetCurrentTexturePalette, s->dev, &p) == (uint32_t)D3D_OK,
          "SetCurrentTexturePalette(7)");
    p.n = 0;
    pu(&p, 16);
    pu(&p, vb);
    pu(&p, 0);
    pu(&p, 16);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetStreamSource, s->dev, &p) == inv, "SetStreamSource(16)");
    p.n = 0;
    pu(&p, 0x21);
    pu(&p, 1);
    pu(&p, 1);
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetStreamSource, s->dev, &p) == inv, "GetStreamSource(0x21)");
    set3(&p, 16, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetStreamSourceFreq, s->dev, &p) == inv, "SetStreamSourceFreq(16)");
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetStreamSourceFreq, s->dev, &p) == inv, "GetStreamSourceFreq(16)");
    set3(&p, 0x10000, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetRenderTarget, s->dev, &p) == inv, "GetRenderTarget(0x10000)");
    set3(&p, 0, 0x10000, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetSamplerState, s->dev, &p) == (uint32_t)D3D_OK,
          "SetSamplerState(type 0x10000)");
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetSamplerState, s->dev, &p) == inv, "GetSamplerState(type 0x10000)");
    set3(&p, 0xffffffffu, D3DSAMP_MINFILTER, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetSamplerState, s->dev, &p) == (uint32_t)D3D_OK,
          "SetSamplerState(sampler 0xffffffff)");
    set3(&p, 0xffffffffu, D3DTSS_COLOROP, 2);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetTextureStageState, s->dev, &p) == (uint32_t)D3D_OK,
          "SetTextureStageState(stage 0xffffffff)");
    set3(&p, 0, 0x10000, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetTextureStageState, s->dev, &p) == inv,
          "GetTextureStageState(type 0x10000)");
    set3(&p, 0x10000, tex, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetTexture, s->dev, &p) == (uint32_t)D3D_OK, "SetTexture(0x10000)");
    set3(&p, 0x10000, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetTexture, s->dev, &p) == inv, "GetTexture(0x10000)");
    p.n = 0;
    pu(&p, D3DFVF_XYZRHW);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetFVF, s->dev, 0, &p) == 0 &&
              call(ch, QD3D_OP_IDirect3DDevice9_BeginScene, s->dev, 1, NULL) == 0,
          "SetFVF, BeginScene");
    p.n = 0;
    pu(&p, D3DPT_TRIANGLELIST);
    pu(&p, 0);
    pu(&p, 3);
    pu(&p, 1);
    pdata(&p, bad, sizeof bad);
    pu(&p, D3DFMT_INDEX16);
    pdata(&p, tri, sizeof tri);
    pu(&p, (uint32_t)sizeof tri[0]);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_DrawIndexedPrimitiveUP, s->dev, &p) == inv,
          "DrawIndexedPrimitiveUP with an index past the vertex data");
    memcpy(p.b + 20, good, sizeof good);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_DrawIndexedPrimitiveUP, s->dev, &p) == (uint32_t)D3D_OK,
          "DrawIndexedPrimitiveUP with valid indices");
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_EndScene, s->dev, 1, NULL) == 0, "EndScene");
    CHECK(clear(ch, s->dev, 0x00203040u) == 0 && present(ch, s->dev) == 0 && idle(ch) &&
              (ch->frame.first & 0x00ffffffu) == 0x00203040u,
          "device presents after the rejected calls");
    CHECK(heaps_valid(), "process heaps validate after the rejected calls");
    if (g_failures == failures)
        printf("  ok  out-of-range lights, palettes, streams, render targets, sampler/stage states, textures and "
               "UP indices answered without the runtime\n");
}

/* RESET leaves the device as native D3D9 does: default states, nothing
 * bound, lights off, viewport over the new back buffer. */
static void test_reset_defaults(struct chan *ch, const struct session *s, uint32_t vb, uint32_t tex)
{
    static const float plane[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    D3DVIEWPORT9 vp = { 1, 2, 10, 8, 0.25f, 0.75f };
    struct qd3d_wire_pp pp = make_pp(48, 40);
    struct pl p = { .n = 0 };
    D3DLIGHT9 light;
    D3DMATRIX m;
    float f[4];
    int failures = g_failures;

    memset(&light, 0, sizeof light);
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Direction.z = 1.0f;
    memset(&m, 0, sizeof m);
    m._11 = 2.0f;
    p.n = 0;
    pu(&p, 3);
    pdata(&p, &light, sizeof light);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_SetLight, s->dev, &p) == (uint32_t)D3D_OK, "SetLight(3)");
    set3(&p, 3, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_LightEnable, s->dev, &p) == (uint32_t)D3D_OK, "LightEnable(3)");
    set3(&p, D3DRS_LIGHTING, FALSE, 0);
    p.n = 8;
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetRenderState, s->dev, 0, &p) == 0, "SetRenderState");
    set3(&p, 0, D3DTSS_COLOROP, D3DTOP_ADD);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetTextureStageState, s->dev, 0, &p) == 0, "SetTextureStageState");
    set3(&p, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetSamplerState, s->dev, 0, &p) == 0, "SetSamplerState");
    p.n = 0;
    pu(&p, D3DTS_WORLD);
    pdata(&p, &m, sizeof m);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetTransform, s->dev, 0, &p) == 0, "SetTransform");
    p.n = 0;
    pdata(&p, &vp, sizeof vp);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetViewport, s->dev, 0, &p) == 0, "SetViewport");
    p.n = 0;
    pu(&p, 0);
    pdata(&p, plane, sizeof plane);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetClipPlane, s->dev, 0, &p) == 0, "SetClipPlane");
    set3(&p, 0, tex, 0);
    p.n = 8;
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetTexture, s->dev, 0, &p) == 0, "SetTexture");
    p.n = 0;
    pu(&p, 0);
    pu(&p, vb);
    pu(&p, 0);
    pu(&p, 16);
    CHECK(call(ch, QD3D_OP_IDirect3DDevice9_SetStreamSource, s->dev, 0, &p) == 0, "SetStreamSource");
    p.n = 0;
    praw(&p, &pp, sizeof pp);
    CHECK(sync_ret(ch, QD3D_OP_RESET, s->dev, &p) == (uint32_t)S_OK, "RESET ret 0x%08x", ch->ret);

    set3(&p, D3DRS_LIGHTING, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetRenderState, s->dev, &p) == 0u && reply_u32(ch, 1) == TRUE,
          "LIGHTING after RESET %u", reply_u32(ch, 1));
    set3(&p, 0, D3DTSS_COLOROP, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetTextureStageState, s->dev, &p) == 0u &&
              reply_u32(ch, 1) == D3DTOP_MODULATE,
          "COLOROP after RESET %u", reply_u32(ch, 1));
    set3(&p, 0, D3DSAMP_MINFILTER, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetSamplerState, s->dev, &p) == 0u && reply_u32(ch, 1) == D3DTEXF_POINT,
          "MINFILTER after RESET %u", reply_u32(ch, 1));
    set3(&p, D3DTS_WORLD, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetTransform, s->dev, &p) == 0u && ch->rsize >= 8u,
          "GetTransform after RESET");
    memcpy(f, ch->rp + 4, 4);
    CHECK(f[0] == 1.0f, "WORLD._11 after RESET %g", (double)f[0]);
    p.n = 0;
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetViewport, s->dev, &p) == 0u && reply_u32(ch, 1) == 0u &&
              reply_u32(ch, 2) == 0u && reply_u32(ch, 3) == 48u && reply_u32(ch, 4) == 40u,
          "viewport after RESET %u,%u %ux%u", reply_u32(ch, 1), reply_u32(ch, 2), reply_u32(ch, 3), reply_u32(ch, 4));
    set3(&p, 0, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetClipPlane, s->dev, &p) == 0u && ch->rsize >= 20u, "GetClipPlane");
    memcpy(f, ch->rp + 4, sizeof f);
    CHECK(f[0] == 0.0f && f[3] == 0.0f, "clip plane after RESET %g %g", (double)f[0], (double)f[3]);
    CHECK(get_texture(ch, s->dev, 0) == 0u, "texture after RESET");
    p.n = 0;
    pu(&p, 0);
    pu(&p, 1);
    pu(&p, 1);
    pu(&p, 1);
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetStreamSource, s->dev, &p) == 0u && reply_u32(ch, 0) == 0u,
          "stream source after RESET 0x%08x", reply_u32(ch, 0));
    set3(&p, 3, 1, 0);
    p.n = 8;
    CHECK(sync_ret(ch, QD3D_OP_IDirect3DDevice9_GetLightEnable, s->dev, &p) == 0u && reply_u32(ch, 1) == FALSE,
          "light 3 after RESET %u", reply_u32(ch, 1));
    if (g_failures == failures)
        printf("  ok  RESET restores default states, unbinds, turns lights off, viewport 48x40\n");
}

static void test_device_state(void)
{
    struct chan ch;
    struct session s;
    uint32_t vb, tex, args[4];

    printf("[device state]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "device state session");
    args[0] = 64; args[1] = 0; args[2] = 0; args[3] = D3DPOOL_SYSTEMMEM;
    CHECK(create_simple(&ch, QD3D_OP_IDirect3DDevice9_CreateVertexBuffer, s.dev, args, 4, &vb, 1) == 0 &&
              ch.ret == (uint32_t)S_OK,
          "vertex buffer");
    CHECK(create_tex(&ch, s.dev, 0, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex) == (uint32_t)S_OK, "texture");
    test_index_args(&ch, &s, vb, tex);
    test_reset_defaults(&ch, &s, vb, tex);
    {
        /* GetAvailableTextureMem moves with DEFAULT allocations (Wine
         * test_vidmem_accounting: >= 2 MiB per 1024x1024 X8R8G8B8 RT). */
        uint32_t m0, m1, m2, rt;

        m0 = sync_ret(&ch, QD3D_OP_IDirect3DDevice9_GetAvailableTextureMem, s.dev, NULL);
        CHECK(create_tex(&ch, s.dev, 0, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rt) ==
                  (uint32_t)S_OK,
              "1024x1024 render-target texture");
        m1 = sync_ret(&ch, QD3D_OP_IDirect3DDevice9_GetAvailableTextureMem, s.dev, NULL);
        release(&ch, rt);
        m2 = sync_ret(&ch, QD3D_OP_IDirect3DDevice9_GetAvailableTextureMem, s.dev, NULL);
        CHECK(m0 > m1 && m0 - m1 >= 4u * 1024u * 1024u && m2 == m0, "available texture memory %u -> %u -> %u", m0, m1,
              m2);
        if (m0 > m1 && m2 == m0)
            printf("  ok  GetAvailableTextureMem %u -> %u with a 4 MiB render target, back after release\n", m0, m1);
    }
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "device state GOODBYE");
    chan_close(&ch);
}

/* Ring wrap: batches of variable-size async commands push the cursor around
 * the ring three times, so PAD commands and wrapped strides are exercised. */
static void test_wrap(void)
{
    struct chan ch;
    struct session s;
    static uint8_t cmd[4096];
    float consts[4 * 200];
    uint32_t i = 0, start, guard = 0, pads = 0;

    printf("[wrap]\n");
    if (!chan_open(&ch)) {
        CHECK(0, "cannot open channel");
        return;
    }
    g_verbose_log = 0;
    CHECK(hello(&ch, &s) && create_device(&ch, &s, 16, 16), "wrap session");
    for (i = 0; i < 4u * 200u; i++)
        consts[i] = (float)i;
    start = ch.ctrl->head;
    while (ch.ctrl->head - start < 3u * QD3D_DEFAULT_RING && guard++ < 1000000u) {
        uint32_t b;

        for (b = 0; b < 64u; b++, i++) {
            uint32_t count = 1u + (i * 37u) % 200u, n = 0, v;
            uint32_t before = ch.ctrl->head, len;

            v = 0;
            memcpy(cmd + n, &v, 4); n += 4;           /* StartRegister */
            v = count * 16u;
            memcpy(cmd + n, &v, 4); n += 4;           /* DATA count */
            memcpy(cmd + n, consts, count * 16u); n += count * 16u;
            memcpy(cmd + n, &count, 4); n += 4;       /* Vector4fCount */
            len = n;
            put_cmd(&ch, QD3D_OP_IDirect3DDevice9_SetVertexShaderConstantF, s.dev, 0, cmd, len, 16u + len);
            if (ch.ctrl->head - before != QD3D_CMD_STRIDE(16u + len))
                pads++;
        }
        if (run(&ch))
            break;
    }
    CHECK(ch.ctrl->head - start >= 3u * QD3D_DEFAULT_RING && ch.ctrl->tail == ch.ctrl->head &&
          ch.ctrl->host_state == QD3D_HOST_READY && pads >= 3u,
          "ring wrap: %u bytes, %u PADs, state %u", ch.ctrl->head - start, pads, ch.ctrl->host_state);
    printf("  ok  %u bytes through the ring, %u PAD commands\n", ch.ctrl->head - start, pads);
    CHECK(call(&ch, QD3D_OP_GOODBYE, 0, 0, NULL) == 0 && qd3dx_live_objects(ch.ex) == 0, "wrap GOODBYE");
    chan_close(&ch);
}

/* Reports a crash with addresses relative to the image base (for
 * addr2line), including the return address when the faulting call came
 * from another module such as d3d9.dll. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t ret = *(uintptr_t *)(uintptr_t)ep->ContextRecord->Rsp;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ret, &mod))
        GetModuleFileNameA(mod, name, sizeof name);
    printf("qd3dx_selftest: exception 0x%08lx at %p (image base %p, rva 0x%llx); [rsp] %s+0x%llx\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, (void *)addr, (void *)base,
           (unsigned long long)(addr - base), name, (unsigned long long)(ret - (uintptr_t)mod));
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --heap-check (see the top of the file): runs this program again as a
 * debuggee and fails when it fails or crashes, or when the heap manager
 * reports a damaged block ("HEAP[...]" debug output and a breakpoint). */
#define QD3DX_STATUS_HEAP_CORRUPTION 0xc0000374u

static int heap_check(void)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 8], text[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT ev;
    DWORD exit_code = 1;
    int loader_break = 1, reports = 0, crashed = 0, done = 0;

    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return 2;
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        printf("qd3dx_selftest: cannot start %s as a debuggee (error %lu)\n", exe, GetLastError());
        return 2;
    }
    while (!done && WaitForDebugEvent(&ev, INFINITE)) {
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile)
                CloseHandle(ev.u.LoadDll.hFile);
            break;
        case OUTPUT_DEBUG_STRING_EVENT: {
            SIZE_T chars = ev.u.DebugString.nDebugStringLength, got = 0;
            size_t len;

            text[0] = 0;
            if (ev.u.DebugString.fUnicode) {
                WCHAR w[512];

                if (chars > 511u)
                    chars = 511u;
                if (ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, w, chars * sizeof(WCHAR),
                                      &got)) {
                    w[got / sizeof(WCHAR)] = 0;
                    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, text, (int)sizeof text, NULL, NULL))
                        text[0] = 0;
                }
            } else {
                if (chars > sizeof text - 1u)
                    chars = sizeof text - 1u;
                if (ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, text, chars, &got))
                    text[got] = 0;
            }
            text[sizeof text - 1u] = 0;
            if (strstr(text, "HEAP["))
                reports++;
            len = strlen(text);
            printf("  debuggee: %s%s", text, len && text[len - 1u] == '\n' ? "" : "\n");
            break;
        }
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord;

            if (er->ExceptionCode == EXCEPTION_BREAKPOINT && loader_break) {
                loader_break = 0; /* the loader's initial breakpoint */
            } else if (er->ExceptionCode == EXCEPTION_BREAKPOINT ||
                       er->ExceptionCode == QD3DX_STATUS_HEAP_CORRUPTION) {
                reports++;
                printf("  debuggee: heap manager exception 0x%08lx at %p\n", (unsigned long)er->ExceptionCode,
                       er->ExceptionAddress);
                if (er->ExceptionCode != EXCEPTION_BREAKPOINT)
                    cont = DBG_EXCEPTION_NOT_HANDLED;
            } else if (ev.u.Exception.dwFirstChance) {
                cont = DBG_EXCEPTION_NOT_HANDLED;
            } else {
                crashed = 1;
                printf("  debuggee: unhandled exception 0x%08lx at %p\n", (unsigned long)er->ExceptionCode,
                       er->ExceptionAddress);
                TerminateProcess(pi.hProcess, 3);
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_code = ev.u.ExitProcess.dwExitCode;
            done = 1;
            break;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("qd3dx_selftest --heap-check: debuggee exit 0x%lx, %d heap report(s)%s\n", (unsigned long)exit_code,
           reports, crashed ? ", crashed" : "");
    return exit_code == 0 && reports == 0 && !crashed ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--heap-check"))
        return heap_check();
    if (argc > 1) {
        printf("usage: qd3dx_selftest [--heap-check]\n");
        return 2;
    }
    SetUnhandledExceptionFilter(on_crash);
    test_faults();
    test_gpu();
    test_present();
    test_fourcc();
    test_host_ids();
    test_sysmem_texture();
    test_cursor();
    test_swapchain();
    test_lod();
    test_source_released();
    test_cursor_follow();
    test_source_shown();
    test_alive_display();
    test_slot_release();
    test_device_state();
    test_wrap();
    printf("qd3dx_selftest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
