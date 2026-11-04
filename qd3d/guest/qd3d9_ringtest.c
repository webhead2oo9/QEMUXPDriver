/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Framing self-test for the guest runtime. A fake host thread consumes an
 * in-process heap channel and checks every command: exact sizes, 16-byte
 * strides, PAD placement at the ring end, DATA padding, UPLOAD regions and
 * chunk sequencing. It answers synchronous calls so reply parsing and the
 * real proxy paths (HELLO, CreateDevice, locks, READBACK, Release) run
 * end to end. Finally the host faults, the dead-channel path is checked,
 * and so is the persistent event log the death leaves (%TEMP%\qd3d.log,
 * with %TEMP% pointed at a test directory).
 *
 * Runs on the Windows host (32-bit, WoW64) or inside XP. Exit code 0 = pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd3d9_priv.h"

IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion);

#define RING_SIZE   (64u * 1024u)
#define REPLY_SIZE  (64u * 1024u)
#define CHAN_SIZE   (QD3D_CTRL_SIZE + RING_SIZE + REPLY_SIZE)

#define OP_T_U32S   0x7001u /* n u32 values v[i] = i */
#define OP_T_DATA   0x7002u /* u32 len, DATA(len bytes of pattern) */
#define OP_T_SYNC   0x7003u /* u32 a -> ret a+1 and a fixed reply */
#define OP_T_BLOB   0x7004u /* raw bytes, byte i = (i + size) & 0xff */

struct rec {
    uint32_t op, obj, seq, size, off;
    uint32_t pad_before;          /* PAD bytes skipped right before */
    struct qd3d_region rg;        /* UPLOAD/READBACK */
    uint8_t *data;                /* UPLOAD payload copy */
};

#define MAX_RECS 8192

static struct {
    uint8_t *base;
    volatile struct qd3d_ctrl *c;
    uint8_t *ring;
    uint8_t *reply;
    HANDLE kick;
    HANDLE thread;
    volatile LONG stop;
    volatile LONG fail;
    char why[256];
    struct rec recs[MAX_RECS];
    volatile LONG nrec;
    uint32_t pending_pad;
    volatile LONG lag;
    uint32_t pads;
} fh;

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
            failures++;                                               \
        }                                                             \
    } while (0)

static void host_fail(const char *msg, uint32_t a, uint32_t b)
{
    if (InterlockedCompareExchange(&fh.fail, 1, 0) == 0)
        _snprintf(fh.why, sizeof(fh.why) - 1, "%s (%u, %u)", msg, (unsigned)a, (unsigned)b);
    fh.c->host_state = QD3D_HOST_FAULT;
}

/* Reply writer. */
static uint8_t *rp_cur;

static void rp_begin(void)
{
    rp_cur = fh.reply + sizeof(struct qd3d_reply);
}

static void rp_u32(uint32_t v)
{
    memcpy(rp_cur, &v, 4);
    rp_cur += 4;
}

static void rp_raw(const void *p, uint32_t n)
{
    memcpy(rp_cur, p, n);
    rp_cur += n;
}

static void rp_data(const void *p, uint32_t n)
{
    rp_u32(n);
    memcpy(rp_cur, p, n);
    memset(rp_cur + n, 0, (size_t)(QD3D_PAD4(n) - n));
    rp_cur += QD3D_PAD4(n);
}

static void rp_end(uint32_t seq, uint32_t ret)
{
    struct qd3d_reply *r = (struct qd3d_reply *)fh.reply;

    r->seq = seq;
    r->ret = ret;
    r->size = (uint32_t)(rp_cur - fh.reply - sizeof(*r));
    r->reserved = 0;
}

static uint8_t readback_byte(uint32_t i)
{
    return (uint8_t)(i * 13u + 5u);
}

/* IID_IDirect3DTexture9 (public Direct3D 9 headers). */
static const GUID iid_texture9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};

/* Validates one command; returns 0 on a framing violation. */
static int host_command(const struct qd3d_cmd *cmd, uint32_t off)
{
    const uint8_t *p = (const uint8_t *)(cmd + 1);
    uint32_t n = cmd->size - 16u, i;
    struct rec *r;
    LONG idx = fh.nrec;

    if (idx >= MAX_RECS) {
        host_fail("record overflow", 0, 0);
        return 0;
    }
    r = &fh.recs[idx];
    memset(r, 0, sizeof(*r));
    r->op = cmd->op;
    r->obj = cmd->obj;
    r->seq = cmd->seq;
    r->size = cmd->size;
    r->off = off;
    r->pad_before = fh.pending_pad;
    if (fh.pending_pad && QD3D_CMD_STRIDE(cmd->size) <= fh.pending_pad) {
        host_fail("PAD written although the command fit", fh.pending_pad, cmd->size);
        return 0;
    }
    fh.pending_pad = 0;

    switch (cmd->op) {
    case OP_T_U32S:
        if (n % 4u)
            return host_fail("u32 payload not multiple of 4", n, 0), 0;
        for (i = 0; i < n / 4u; i++) {
            uint32_t v;
            memcpy(&v, p + 4u * i, 4);
            if (v != i)
                return host_fail("u32 value", v, i), 0;
        }
        break;
    case OP_T_DATA: {
        uint32_t len, cnt;
        if (n < 8u)
            return host_fail("DATA cmd short", n, 0), 0;
        memcpy(&len, p, 4);
        memcpy(&cnt, p + 4, 4);
        if (len == QD3D_DATA_NULL) {
            if (cnt != QD3D_DATA_NULL || n != 8u)
                return host_fail("DATA NULL encoding", cnt, n), 0;
            break;
        }
        if (cnt != len || n != 8u + QD3D_PAD4(len))
            return host_fail("DATA size", cnt, n), 0;
        for (i = 0; i < len; i++)
            if (p[8u + i] != (uint8_t)(i * 3u + 1u))
                return host_fail("DATA byte", i, p[8u + i]), 0;
        for (; i < QD3D_PAD4(len); i++)
            if (p[8u + i] != 0)
                return host_fail("DATA padding not zero", len, i), 0;
        break;
    }
    case OP_T_BLOB:
        for (i = 0; i < n; i++)
            if (p[i] != (uint8_t)(i + cmd->size))
                return host_fail("blob byte", i, cmd->size), 0;
        break;
    case OP_T_SYNC: {
        uint32_t a;
        static const uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        if (n != 4u || !cmd->seq)
            return host_fail("sync cmd", n, cmd->seq), 0;
        memcpy(&a, p, 4);
        rp_begin();
        rp_u32(0x11223344u);
        rp_data("hello", 5);
        rp_u32(QD3D_DATA_NULL);
        rp_data(eight, 8);
        rp_u32(0x55u);
        rp_end(cmd->seq, a + 1u);
        break;
    }
    case QD3D_OP_HELLO: {
        struct qd3d_hello h;
        struct qd3d_hello_reply hr;
        if (n != sizeof(h) || !cmd->seq)
            return host_fail("HELLO size", n, cmd->seq), 0;
        memcpy(&h, p, sizeof(h));
        if (h.abi != QD3D_ABI_VERSION || h.d3d_id == 0 || fh.c->guest_pid != h.pid)
            return host_fail("HELLO fields", h.abi, h.d3d_id), 0;
        hr.abi = QD3D_ABI_VERSION;
        hr.flags = 0;
        hr.max_cmd = RING_SIZE / 2u;
        hr.max_reply = REPLY_SIZE - (uint32_t)sizeof(struct qd3d_reply);
        rp_begin();
        rp_raw(&hr, sizeof(hr));
        rp_end(cmd->seq, D3D_OK);
        break;
    }
    case QD3D_OP_CREATE_DEVICE: {
        struct qd3d_create_device cd;
        if (n != sizeof(cd) || !cmd->seq)
            return host_fail("CREATE_DEVICE size", n, cmd->seq), 0;
        memcpy(&cd, p, sizeof(cd));
        rp_begin();
        rp_raw(&cd.pp, sizeof(cd.pp));
        rp_end(cmd->seq, D3D_OK);
        break;
    }
    case QD3D_OP_UPLOAD:
    case QD3D_OP_READBACK: {
        struct qd3d_region *g = &r->rg;
        if (n < sizeof(*g))
            return host_fail("region short", n, 0), 0;
        memcpy(g, p, sizeof(*g));
        if (cmd->op == QD3D_OP_UPLOAD) {
            if (cmd->seq || n != sizeof(*g) + g->data_size)
                return host_fail("UPLOAD size", n, g->data_size), 0;
            if (g->lock_flags & ~(uint32_t)(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | QD3D_UPLOAD_CONTINUE))
                return host_fail("UPLOAD flags", g->lock_flags, 0), 0;
            r->data = (uint8_t *)malloc(g->data_size ? g->data_size : 1u);
            if (r->data)
                memcpy(r->data, p + sizeof(*g), g->data_size);
        } else {
            if (!cmd->seq || n != sizeof(*g) || g->data_size > REPLY_SIZE - 16u)
                return host_fail("READBACK size", n, g->data_size), 0;
            rp_begin();
            for (i = 0; i < g->data_size; i++)
                *rp_cur++ = readback_byte(i);
            rp_end(cmd->seq, D3D_OK);
        }
        if (g->kind == QD3D_UPLOAD_SURFACE) {
            if (!g->row_pitch)
                return host_fail("surface row_pitch 0", 0, 0), 0;
            if (g->d != 1u || g->slice_pitch != 0u || !g->h || g->data_size % g->row_pitch)
                return host_fail("surface region", g->d, g->data_size), 0;
        } else if (g->kind == QD3D_UPLOAD_BUFFER) {
            if (g->data_size != g->w)
                return host_fail("buffer region", g->data_size, g->w), 0;
        }
        break;
    }
    case QD3D_OP_IDirect3DTexture9_GetSurfaceLevel: {
        uint32_t present;
        if (n != 8u || !cmd->seq)
            return host_fail("GetSurfaceLevel size", n, cmd->seq), 0;
        memcpy(&present, p + 4, 4);
        rp_begin();
        if (present) {
            rp_u32(QD3D_ID_HOST_BIT | 1u);
            rp_u32(QD3D_T_IDirect3DSurface9);
        }
        rp_end(cmd->seq, D3D_OK);
        break;
    }
    default:
        if (cmd->seq) {
            rp_begin();
            rp_end(cmd->seq, D3D_OK);
        }
        break;
    }
    InterlockedIncrement(&fh.nrec);
    return 1;
}

static DWORD WINAPI host_thread(void *arg)
{
    volatile struct qd3d_ctrl *c = fh.c;
    uint32_t tail = c->tail;
    uint32_t mask = RING_SIZE - 1u;
    uint32_t count = 0;

    (void)arg;
    while (!fh.stop) {
        uint32_t head = c->head, off, stride;
        const struct qd3d_cmd *cmd;

        MemoryBarrier();
        if (head == tail || c->host_state != QD3D_HOST_READY) {
            c->host_idle = 1;
            MemoryBarrier();
            if (c->head != tail && c->host_state == QD3D_HOST_READY) {
                c->host_idle = 0;
                continue;
            }
            WaitForSingleObject(fh.kick, 1);
            c->host_idle = 0;
            continue;
        }
        if (head - tail > RING_SIZE || (head & 15u)) {
            host_fail("head invalid", head, tail);
            continue;
        }
        off = tail & mask;
        cmd = (const struct qd3d_cmd *)(fh.ring + off);
        if (cmd->size < 16u) {
            host_fail("command size < 16", cmd->size, off);
            continue;
        }
        stride = QD3D_CMD_STRIDE(cmd->size);
        if (off + stride > RING_SIZE) {
            host_fail("command straddles the ring end", off, cmd->size);
            continue;
        }
        if (stride > head - tail) {
            host_fail("command beyond head", stride, head - tail);
            continue;
        }
        if (cmd->op == QD3D_OP_PAD) {
            if (cmd->size != RING_SIZE - off || cmd->seq || cmd->obj) {
                host_fail("PAD size is not the space to the ring end", cmd->size, off);
                continue;
            }
            fh.pending_pad = cmd->size;
            fh.pads++;
        } else if (cmd->op != QD3D_OP_PAD && stride > RING_SIZE / 2u) {
            host_fail("stride above ring_size/2", stride, 0);
            continue;
        } else {
            if (!host_command(cmd, off))
                continue;
            if (cmd->seq) {
                MemoryBarrier();
                c->sync_done = cmd->seq;
            }
        }
        tail += stride;
        MemoryBarrier();
        c->tail = tail;
        if (fh.lag && (++count % 7u) == 0u)
            Sleep(1);
    }
    return 0;
}

static void kick_cb(void *ctx)
{
    (void)ctx;
    SetEvent(fh.kick);
}

static void channel_start(void)
{
    volatile struct qd3d_ctrl *c;
    DWORD tid;

    memset((void *)fh.recs, 0, sizeof(fh.recs));
    fh.nrec = 0;
    fh.fail = 0;
    fh.stop = 0;
    fh.pads = 0;
    fh.pending_pad = 0;
    fh.base = (uint8_t *)VirtualAlloc(NULL, CHAN_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fh.base) {
        printf("FAIL: VirtualAlloc\n");
        exit(1);
    }
    c = (volatile struct qd3d_ctrl *)fh.base;
    c->abi = QD3D_ABI_VERSION;
    c->total_size = CHAN_SIZE;
    c->ring_offset = QD3D_CTRL_SIZE;
    c->ring_size = RING_SIZE;
    c->reply_offset = QD3D_CTRL_SIZE + RING_SIZE;
    c->reply_size = REPLY_SIZE;
    c->host_state = QD3D_HOST_READY;
    MemoryBarrier();
    c->magic = QD3D_MAGIC;
    fh.c = c;
    fh.ring = fh.base + QD3D_CTRL_SIZE;
    fh.reply = fh.base + QD3D_CTRL_SIZE + RING_SIZE;
    fh.kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    fh.thread = CreateThread(NULL, 0, host_thread, NULL, 0, &tid);
    if (!fh.kick || !fh.thread) {
        printf("FAIL: host thread\n");
        exit(1);
    }
    if (!qd3d_rt_attach_memory(fh.base, CHAN_SIZE, kick_cb, NULL)) {
        printf("FAIL: channel did not validate\n");
        exit(1);
    }
}

/* Waits until the host consumed everything published. */
static void drain(void)
{
    DWORD start = GetTickCount();

    while (fh.c->tail != fh.c->head && !fh.fail && GetTickCount() - start < 10000)
        Sleep(1);
    CHECK(fh.c->tail == fh.c->head, "ring not drained");
}

/* Drains and returns the index of the next host record. */
static LONG mark(void)
{
    drain();
    return fh.nrec;
}

static void channel_stop(void)
{
    LONG i;

    drain();
    fh.stop = 1;
    SetEvent(fh.kick);
    WaitForSingleObject(fh.thread, INFINITE);
    CloseHandle(fh.thread);
    CloseHandle(fh.kick);
    qd3d_rt_detach_memory();
    CHECK(!fh.fail, "host: %s", fh.why);
    for (i = 0; i < fh.nrec; i++)
        free(fh.recs[i].data);
    VirtualFree(fh.base, 0, MEM_RELEASE);
}

static void send_u32s(uint32_t n)
{
    struct qd3d_call k;
    uint32_t i;

    qd3d_call_begin(&k, OP_T_U32S, 7, 0, 4ull * n);
    for (i = 0; i < n; i++)
        qd3d_put_u32(&k, i);
    qd3d_call_end(&k);
    qd3d_call_done(&k);
}

static void send_data(const uint8_t *p, uint32_t len)
{
    struct qd3d_call k;

    qd3d_call_begin(&k, OP_T_DATA, 0, 0, 4u + 4u + (p ? QD3D_PAD4(len) : 0u));
    qd3d_put_u32(&k, p ? len : QD3D_DATA_NULL);
    qd3d_put_data(&k, p, len);
    qd3d_call_end(&k);
    qd3d_call_done(&k);
}

/* A raw command whose total size is exactly `size` bytes. */
static void send_blob(uint32_t size)
{
    static uint8_t buf[RING_SIZE];
    struct qd3d_call k;
    uint32_t i;

    for (i = 0; i < size - 16u; i++)
        buf[i] = (uint8_t)(i + size);
    qd3d_call_begin(&k, OP_T_BLOB, 0, 0, size - 16u);
    qd3d_put_raw(&k, buf, size - 16u);
    qd3d_call_end(&k);
    qd3d_call_done(&k);
}

static uint32_t ring_left(void)
{
    return RING_SIZE - (fh.c->head & (RING_SIZE - 1u));
}

/* ------------------------------------------------------------------------ */

static void test_framing(void)
{
    static uint8_t pat[64];
    uint32_t i, left, pads;
    LONG first;

    printf("framing: sizes, strides, DATA padding\n");
    channel_start();
    for (i = 0; i < sizeof(pat); i++)
        pat[i] = (uint8_t)(i * 3u + 1u);
    send_u32s(0);
    send_u32s(1);
    send_u32s(3);
    send_u32s(4);
    for (i = 0; i <= 9; i++)
        send_data(pat, i);
    send_data(NULL, 0);
    drain();
    CHECK(fh.nrec == 15, "expected 15 records, got %ld", (long)fh.nrec);
    CHECK(fh.recs[0].size == 16 && fh.recs[1].size == 20 && fh.recs[2].size == 28 &&
          fh.recs[3].size == 32, "u32 command sizes");
    CHECK(fh.recs[1].off == 16 && fh.recs[2].off == 48 && fh.recs[3].off == 80, "strides");

    printf("framing: PAD at the ring end\n");
    while (ring_left() > 1024u)
        send_blob(1000);
    left = ring_left();
    first = mark();
    pads = fh.pads;
    send_blob(left + 16u); /* stride exceeds the space left: PAD first */
    drain();
    CHECK(fh.pads == pads + 1, "PAD count %u -> %u", (unsigned)pads, (unsigned)fh.pads);
    CHECK(fh.recs[first].pad_before == left && fh.recs[first].off == 0,
          "command after PAD at offset %u (pad %u, expected %u)", (unsigned)fh.recs[first].off,
          (unsigned)fh.recs[first].pad_before, (unsigned)left);

    printf("framing: command filling the remaining space exactly\n");
    while (ring_left() > RING_SIZE / 2u - 64u)
        send_blob(2000);
    left = ring_left();
    first = mark();
    pads = fh.pads;
    send_blob(left - 8u); /* stride == left exactly */
    send_u32s(2);
    drain();
    CHECK(fh.pads == pads, "no PAD expected for an exact fill");
    CHECK(fh.recs[first].pad_before == 0 && QD3D_CMD_STRIDE(fh.recs[first].size) == left,
          "exact fill stride");
    CHECK(fh.recs[first + 1].off == 0, "command after exact fill at offset %u",
          (unsigned)fh.recs[first + 1].off);

    printf("framing: wrap stress with a lagging host\n");
    fh.lag = 1;
    srand(1234);
    first = mark();
    for (i = 0; i < 3000; i++) {
        uint32_t sz = 16u + (uint32_t)rand() % (RING_SIZE / 2u - 16u);
        if (i % 5u == 0)
            sz = 16u + (uint32_t)rand() % 64u;
        send_blob(sz);
    }
    drain();
    fh.lag = 0;
    CHECK(fh.nrec - first == 3000, "stress records %ld", (long)(fh.nrec - first));
    CHECK(fh.pads > 10, "stress produced %u PADs", (unsigned)fh.pads);

    printf("framing: oversize command is dropped\n");
    {
        struct qd3d_call k;
        uint32_t r;
        first = mark();
        qd3d_call_begin(&k, OP_T_SYNC, 0, 1, RING_SIZE);
        r = qd3d_call_end(&k);
        qd3d_call_done(&k);
        CHECK(r == (uint32_t)D3DERR_DRIVERINTERNALERROR, "oversize sync ret %#x", (unsigned)r);
        CHECK(qd3d_rt_alive(), "oversize command must not kill the channel");
        drain();
        CHECK(fh.nrec == first, "oversize command reached the host");
    }
    channel_stop();
}

static void test_sync(void)
{
    struct qd3d_call k;
    uint32_t ret, v = 0, tail = 0;
    char five[5];
    uint8_t four[4] = {9, 9, 9, 9};
    uint8_t nul[4] = {9, 9, 9, 9};
    int i;

    printf("sync: reply parsing\n");
    channel_start();
    for (i = 0; i < 100; i++) {
        qd3d_call_begin(&k, OP_T_SYNC, 3, 1, 4u);
        qd3d_put_u32(&k, (uint32_t)i);
        ret = qd3d_call_end(&k);
        CHECK(ret == (uint32_t)i + 1u, "sync ret %u", (unsigned)ret);
        CHECK(qd3d_get_u32(&k, &v) && v == 0x11223344u, "reply u32");
        CHECK(qd3d_get_data(&k, five, 5) && memcmp(five, "hello", 5) == 0, "reply DATA");
        CHECK(!qd3d_get_data(&k, nul, 4) && !memcmp(nul, "\0\0\0\0", 4), "reply DATA NULL zero-fills");
        CHECK(!qd3d_get_data(&k, four, 4) && !memcmp(four, "\0\0\0\0", 4), "size-mismatched DATA zero-fills");
        CHECK(qd3d_get_u32(&k, &tail) && tail == 0x55u, "cursor after mismatched DATA");
        CHECK(!qd3d_get_u32(&k, &v) && v == 0, "read past reply end");
        qd3d_call_done(&k);
        if (i % 10 == 0)
            send_u32s(5);
    }
    channel_stop();
}

static struct rec *find_rec(LONG from, uint32_t op, LONG *at)
{
    LONG i;

    for (i = from; i < fh.nrec; i++) {
        if (fh.recs[i].op == op) {
            if (at)
                *at = i;
            return &fh.recs[i];
        }
    }
    return NULL;
}

static int count_recs(LONG from, uint32_t op)
{
    LONG i;
    int n = 0;

    for (i = from; i < fh.nrec; i++)
        n += fh.recs[i].op == op;
    return n;
}

static void test_proxy(void)
{
    static const GUID iid_remote = QD3D_IID_REMOTE_INIT;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D9 *d3d, *d3d2, *q = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DTexture9 *tex = NULL, *small = NULL, *mip = NULL;
    IDirect3DSurface9 *surf = NULL, *lvl = NULL;
    D3DLOCKED_RECT lr;
    uint8_t *p;
    HRESULT hr;
    LONG at, i, first;
    uint32_t x, y, vb_id, dev_id, d3d_id, expect_x, lvl_id = 0, tex_id = 0, mip_id = 0;
    RECT rc = {4, 4, 10, 10};

    printf("proxy: HELLO, CreateDevice, buffer upload chunks\n");
    channel_start();
    first = mark();
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(d3d != NULL, "Direct3DCreate9");
    if (!d3d) {
        channel_stop();
        return;
    }
    d3d2 = Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(d3d2 && d3d2 != d3d, "every Direct3DCreate9 returns a new object");
    CHECK(d3d2 && qd3d_obj_id(d3d2) == qd3d_obj_id(d3d), "one session IDirect3D9 on the wire");
    hr = d3d->lpVtbl->QueryInterface(d3d, &iid_remote, (void **)&q);
    CHECK(hr == S_OK && q == d3d, "IID_QD3D_REMOTE answered by the proxy");
    if (q)
        q->lpVtbl->Release(q);
    if (d3d2)
        CHECK(d3d2->lpVtbl->Release(d3d2) == 0, "second IDirect3D9 released");
    drain();
    CHECK(count_recs(first, QD3D_OP_HELLO) == 1 && count_recs(first, QD3D_OP_RELEASE) == 0,
          "one HELLO per process, IDirect3D9 proxies send no RELEASE");
    d3d_id = qd3d_obj_id(d3d);
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 32;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    CHECK(SUCCEEDED(hr) && dev, "CreateDevice hr %#lx", (unsigned long)hr);
    CHECK(pp.BackBufferCount == 1, "BackBufferCount resolved to 1");
    if (!dev) {
        d3d->lpVtbl->Release(d3d);
        channel_stop();
        return;
    }
    dev_id = qd3d_obj_id(dev);

    /* Managed resources start dirty and upload at first use (PreLoad
     * here), as native ones do; unlocking uploads nothing. */
    hr = dev->lpVtbl->CreateVertexBuffer(dev, 40000, 0, 0, D3DPOOL_MANAGED, &vb, NULL);
    CHECK(SUCCEEDED(hr) && vb, "CreateVertexBuffer");
    vb_id = qd3d_obj_id(vb);
    CHECK(vb_id > dev_id && dev_id > d3d_id, "monotonic guest ids");
    first = mark();
    hr = vb->lpVtbl->Lock(vb, 0, 16, (void **)&p, 0);
    CHECK(SUCCEEDED(hr) && ((uintptr_t)p & 15u) == 0, "VB Lock, 16-byte aligned");
    for (x = 0; SUCCEEDED(hr) && x < 40000; x++)
        p[x] = (uint8_t)(x * 7u); /* beyond the locked range too */
    hr = vb->lpVtbl->Unlock(vb);
    CHECK(SUCCEEDED(hr), "VB Unlock");
    drain();
    CHECK(!find_rec(first, QD3D_OP_UPLOAD, NULL), "managed unlock uploads nothing");
    vb->lpVtbl->PreLoad(vb);
    drain();
    expect_x = 0;
    for (i = first, at = 0; i < fh.nrec; i++) {
        struct rec *r = &fh.recs[i];
        if (r->op != QD3D_OP_UPLOAD)
            continue;
        CHECK(r->obj == vb_id && r->rg.kind == QD3D_UPLOAD_BUFFER, "VB upload target");
        CHECK(r->rg.x == expect_x, "VB chunk x %u expected %u", (unsigned)r->rg.x, (unsigned)expect_x);
        CHECK(r->rg.lock_flags == (at ? QD3D_UPLOAD_CONTINUE : 0u), "VB chunk flags %#x", (unsigned)r->rg.lock_flags);
        CHECK(r->rg.w <= RING_SIZE / 4u, "VB chunk size %u", (unsigned)r->rg.w);
        for (x = 0; r->data && x < r->rg.w; x++)
            if (r->data[x] != (uint8_t)((r->rg.x + x) * 7u)) {
                CHECK(0, "VB data at %u", (unsigned)(r->rg.x + x));
                break;
            }
        expect_x += r->rg.w;
        at++;
    }
    CHECK(expect_x == 40000 && at == 3, "VB upload covered %u bytes in %ld chunks", (unsigned)expect_x, (long)at);
    first = mark();
    vb->lpVtbl->PreLoad(vb);
    drain();
    CHECK(!find_rec(first, QD3D_OP_UPLOAD, NULL), "second PreLoad uploads nothing");

    printf("proxy: managed mip chain in one block, uploaded at first use\n");
    hr = dev->lpVtbl->CreateTexture(dev, 13, 5, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &mip, NULL);
    CHECK(SUCCEEDED(hr) && mip, "CreateTexture managed");
    if (mip) {
        static const int32_t offs[4] = {0, 260, 308, 320}; /* measured, stock runtime */
        static const INT pitches[4] = {52, 24, 12, 4};
        uint8_t *base = NULL;
        uint32_t lvls = 0;

        mip_id = qd3d_obj_id(mip);
        first = mark();
        for (x = 0; x < 4; x++) {
            hr = mip->lpVtbl->LockRect(mip, x, &lr, NULL, 0);
            CHECK(SUCCEEDED(hr), "mip lock %u", (unsigned)x);
            if (FAILED(hr))
                continue;
            if (!x)
                base = (uint8_t *)lr.pBits;
            CHECK(((uintptr_t)base & 31u) == 0, "block 32-byte aligned");
            CHECK((uint8_t *)lr.pBits - base == offs[x] && lr.Pitch == pitches[x], "level %u at %ld pitch %d",
                  (unsigned)x, (long)((uint8_t *)lr.pBits - base), lr.Pitch);
            mip->lpVtbl->UnlockRect(mip, x);
        }
        drain();
        CHECK(!find_rec(first, QD3D_OP_UPLOAD, NULL), "managed texture unlock uploads nothing");
        mip->lpVtbl->PreLoad(mip);
        drain();
        for (i = first; i < fh.nrec; i++)
            if (fh.recs[i].op == QD3D_OP_UPLOAD && fh.recs[i].obj == mip_id)
                lvls |= 1u << fh.recs[i].rg.level;
        CHECK(lvls == 15u, "PreLoad uploads every level (mask %#x)", (unsigned)lvls);
    }

    printf("proxy: DXT1 rect rounding, level surface addressing\n");
    /* Default pool: natively only there are block-format rectangles checked. */
    hr = dev->lpVtbl->CreateTexture(dev, 64, 64, 0, D3DUSAGE_DYNAMIC, D3DFMT_DXT1, D3DPOOL_DEFAULT, &tex, NULL);
    CHECK(SUCCEEDED(hr) && tex, "CreateTexture DXT1");
    first = mark();
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, &rc, 0);
    CHECK(hr == D3DERR_INVALIDCALL, "unaligned DXT rect rejected");
    rc.left = 4;
    rc.top = 8;
    rc.right = 12;
    rc.bottom = 16;
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, &rc, 0);
    CHECK(SUCCEEDED(hr) && lr.Pitch == 16 * 8, "DXT lock pitch %d", lr.Pitch);
    if (SUCCEEDED(hr))
        tex->lpVtbl->UnlockRect(tex, 0);
    hr = tex->lpVtbl->LockRect(tex, 5, &lr, NULL, 0); /* 2x2 level */
    CHECK(SUCCEEDED(hr) && lr.Pitch == 8, "DXT 2x2 level pitch %d", lr.Pitch);
    if (SUCCEEDED(hr))
        tex->lpVtbl->UnlockRect(tex, 5);
    drain();
    {
        struct rec *u1 = find_rec(first, QD3D_OP_UPLOAD, &at);
        struct rec *u2 = u1 ? find_rec(at + 1, QD3D_OP_UPLOAD, NULL) : NULL;
        CHECK(u1 && u1->rg.x == 4 && u1->rg.y == 8 && u1->rg.w == 8 && u1->rg.h == 8 &&
              u1->rg.row_pitch == 16 && u1->rg.data_size == 32 && u1->rg.level == 0,
              "DXT rect region");
        CHECK(u2 && u2->rg.w == 4 && u2->rg.h == 4 && u2->rg.level == 5 && u2->rg.data_size == 8,
              "DXT 2x2 level region rounded to one block");
    }
    hr = tex->lpVtbl->GetSurfaceLevel(tex, 1, &lvl);
    CHECK(SUCCEEDED(hr) && lvl, "GetSurfaceLevel");
    if (lvl) {
        IDirect3DTexture9 *ct = NULL;
        first = mark();
        hr = lvl->lpVtbl->LockRect(lvl, &lr, NULL, 0);
        CHECK(SUCCEEDED(hr), "level surface lock");
        if (SUCCEEDED(hr))
            lvl->lpVtbl->UnlockRect(lvl);
        drain();
        {
            struct rec *u = find_rec(first, QD3D_OP_UPLOAD, NULL);
            CHECK(u && u->obj == qd3d_obj_id(tex) && u->rg.level == 1 && u->rg.w == 32 && u->rg.h == 32,
                  "level surface uploads through its texture");
        }
        hr = lvl->lpVtbl->GetContainer(lvl, &iid_texture9, (void **)&ct);
        CHECK(SUCCEEDED(hr) && ct == tex, "GetContainer");
        if (ct)
            ct->lpVtbl->Release(ct);
        lvl_id = qd3d_obj_id(lvl);
        lvl->lpVtbl->Release(lvl);
    }
    hr = dev->lpVtbl->CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &small, NULL);
    if (SUCCEEDED(hr) && small) {
        hr = small->lpVtbl->LockRect(small, 0, &lr, NULL, 0);
        CHECK(hr == D3DERR_INVALIDCALL, "lock of non-dynamic DEFAULT texture rejected");
        small->lpVtbl->Release(small);
    }

    printf("proxy: surface row-band upload, GPU-written READBACK\n");
    /* Default pool: natively ColorFill rejects system-memory surfaces. */
    hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, 256, 256, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &surf, NULL);
    CHECK(SUCCEEDED(hr) && surf, "CreateOffscreenPlainSurface");
    if (surf) {
        uint32_t ey = 0, chunks = 0;
        first = mark();
        hr = surf->lpVtbl->LockRect(surf, &lr, NULL, 0);
        CHECK(SUCCEEDED(hr) && lr.Pitch == 1024, "surface lock");
        for (y = 0; SUCCEEDED(hr) && y < 256; y++)
            memset((uint8_t *)lr.pBits + y * (uint32_t)lr.Pitch, (int)y, 1024);
        if (SUCCEEDED(hr))
            surf->lpVtbl->UnlockRect(surf);
        drain();
        for (i = first; i < fh.nrec; i++) {
            struct rec *r = &fh.recs[i];
            if (r->op != QD3D_OP_UPLOAD)
                continue;
            CHECK(r->rg.y == ey && r->rg.x == 0 && r->rg.w == 256 && r->rg.row_pitch == 1024,
                  "row band y %u expected %u", (unsigned)r->rg.y, (unsigned)ey);
            CHECK(r->rg.lock_flags == (chunks ? QD3D_UPLOAD_CONTINUE : 0u), "band flags");
            CHECK(r->rg.data_size == r->rg.h * 1024u && r->rg.data_size <= RING_SIZE / 4u, "band size");
            CHECK(r->data && r->data[0] == (uint8_t)r->rg.y, "band contents");
            ey += r->rg.h;
            chunks++;
        }
        CHECK(ey == 256 && chunks == 16, "surface bands cover %u rows in %u chunks", (unsigned)ey, (unsigned)chunks);
        {
            RECT part = {8, 0, 200, 256};
            first = mark();
            hr = surf->lpVtbl->LockRect(surf, &lr, &part, 0);
            CHECK(SUCCEEDED(hr), "partial surface lock");
            if (SUCCEEDED(hr))
                surf->lpVtbl->UnlockRect(surf);
            drain();
            ey = 0;
            for (i = first; i < fh.nrec; i++) {
                struct rec *r = &fh.recs[i];
                if (r->op != QD3D_OP_UPLOAD)
                    continue;
                CHECK(r->rg.x == 0 && r->rg.w == 256 && r->rg.y == ey, "multi-chunk partial lock uses full-width bands");
                ey += r->rg.h;
            }
            CHECK(ey == 256, "partial lock bands cover %u rows", (unsigned)ey);
        }

        hr = dev->lpVtbl->ColorFill(dev, surf, NULL, 0);
        CHECK(SUCCEEDED(hr), "ColorFill");
        first = mark();
        hr = surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY);
        CHECK(SUCCEEDED(hr), "readback lock");
        if (SUCCEEDED(hr)) {
            uint32_t bytes = 0, ok = 1, ry = 0;
            for (i = first; i < fh.nrec; i++) {
                struct rec *r = &fh.recs[i];
                if (r->op != QD3D_OP_READBACK)
                    continue;
                CHECK(r->rg.y == ry && r->rg.data_size <= REPLY_SIZE - 16u, "readback band");
                for (x = 0; x < r->rg.data_size && ok; x++)
                    ok = ((uint8_t *)lr.pBits)[r->rg.y * 1024u + x] == readback_byte(x);
                ry += r->rg.h;
                bytes += r->rg.data_size;
            }
            CHECK(bytes == 256u * 1024u && ok, "readback filled the surface (%u bytes, ok %u)", (unsigned)bytes, (unsigned)ok);
            first = mark();
            surf->lpVtbl->UnlockRect(surf);
            drain();
            CHECK(!find_rec(first, QD3D_OP_UPLOAD, NULL), "READONLY unlock uploads nothing");
            first = mark();
            hr = surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY);
            if (SUCCEEDED(hr))
                surf->lpVtbl->UnlockRect(surf);
            CHECK(!find_rec(first, QD3D_OP_READBACK, NULL), "second lock does not read back again");
        }
    }

    /* A level surface lives as long as its texture (native lifetime), so its
     * RELEASE goes out with the texture's, before it; children always reach
     * the host before their parents. The IDirect3D9 proxy names the session
     * object, which GOODBYE releases. */
    printf("proxy: Release order\n");
    if (mip)
        mip->lpVtbl->Release(mip);
    first = mark();
    tex_id = tex ? qd3d_obj_id(tex) : 0u;
    if (surf)
        surf->lpVtbl->Release(surf);
    if (tex)
        tex->lpVtbl->Release(tex);
    vb->lpVtbl->Release(vb);
    CHECK(dev->lpVtbl->Release(dev) == 0, "device released");
    CHECK(d3d->lpVtbl->Release(d3d) == 0, "d3d released");
    drain();
    {
        uint32_t order[8], n = 0;
        for (i = first; i < fh.nrec && n < 8; i++)
            if (fh.recs[i].op == QD3D_OP_RELEASE)
                order[n++] = fh.recs[i].obj;
        CHECK(n == 5 && order[1] == lvl_id && order[2] == tex_id && order[3] == vb_id && order[4] == dev_id,
              "RELEASE order (%u releases)", (unsigned)n);
    }
    channel_stop();
}

static void test_dead(void)
{
    struct qd3d_call k;
    uint32_t r, v = 7;
    uint8_t buf[4] = {1, 1, 1, 1};

    printf("dead channel: host fault\n");
    channel_start();
    send_u32s(2);
    drain();
    fh.c->host_state = QD3D_HOST_FAULT;
    qd3d_call_begin(&k, OP_T_SYNC, 0, 1, 4u);
    qd3d_put_u32(&k, 1);
    r = qd3d_call_end(&k);
    CHECK(r == (uint32_t)D3DERR_DRIVERINTERNALERROR, "sync on faulted channel returns %#x", (unsigned)r);
    CHECK(!qd3d_get_u32(&k, &v) && v == 0, "get_u32 zero-fills");
    CHECK(!qd3d_get_data(&k, buf, 4) && !memcmp(buf, "\0\0\0\0", 4), "get_data zero-fills");
    qd3d_call_done(&k);
    CHECK(!qd3d_rt_alive(), "channel marked dead");
    qd3d_call_begin(&k, OP_T_U32S, 0, 0, 4u);
    qd3d_put_u32(&k, 0);
    r = qd3d_call_end(&k);
    qd3d_call_done(&k);
    CHECK(r == 0 && fh.c->head == fh.c->tail, "async call on dead channel is discarded");
    fh.c->host_state = QD3D_HOST_READY;
    fh.fail = 0;
    channel_stop();
}

/* ------------------------------------------------------------------------ */
/* Persistent event log                                                     */
/* ------------------------------------------------------------------------ */
/* %TEMP% points at a directory of the test's own (log_setup, before any
 * event), so the user's own qd3d.log stays untouched. */
static char g_logdir[MAX_PATH + 32], g_logfile[MAX_PATH + 48];

static int log_setup(void)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);

    if (n == 0 || n >= sizeof(tmp))
        return 0;
    snprintf(g_logdir, sizeof(g_logdir), "%sqd3d9_ringtest_%lu", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(g_logfile, sizeof(g_logfile), "%s\\qd3d.log", g_logdir);
    if (!CreateDirectoryA(g_logdir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    DeleteFileA(g_logfile);
    return SetEnvironmentVariableA("TMP", g_logdir) && SetEnvironmentVariableA("TEMP", g_logdir);
}

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;

    *len = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = (char *)malloc((size_t)*len + 1u)) != NULL) {
        *len = (long)fread(buf, 1, (size_t)*len, f);
        buf[*len] = 0;
    }
    fclose(f);
    return buf;
}

static void fill_file(const char *path, long len)
{
    FILE *f = fopen(path, "wb");
    static char z[4096];

    memset(z, 'Z', sizeof(z));
    while (f && len > 0) {
        long n = len < (long)sizeof(z) ? len : (long)sizeof(z);
        fwrite(z, 1, (size_t)n, f);
        len -= n;
    }
    if (f)
        fclose(f);
}

/* test_dead's channel death is a line "YYYY-MM-DD hh:mm:ss pid exe: ...";
 * past 1 MiB the next event empties the file first, at 1 MiB it appends. */
static void test_log(void)
{
    char exe[MAX_PATH], want[MAX_PATH + 64];
    const char *base = exe, *p;
    unsigned y, mo, d, h, mi, s;
    unsigned long pid = 0;
    long n;
    int at = 0;
    char *text;

    printf("event log: %%TEMP%%\\qd3d.log\n");
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    for (p = exe; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    snprintf(want, sizeof(want), "%s: channel dead: host not READY", base);
    text = slurp(g_logfile, &n);
    CHECK(text != NULL, "channel death left no %s", g_logfile);
    if (text) {
        CHECK(sscanf(text, "%4u-%2u-%2u %2u:%2u:%2u %lu %n", &y, &mo, &d, &h, &mi, &s, &pid, &at) == 7 && at > 0 &&
                  y >= 2000 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && s < 62,
              "log line format: %.100s", text);
        CHECK(pid == GetCurrentProcessId(), "log line pid %lu", pid);
        CHECK(at > 0 && !strncmp(text + at, want, strlen(want)), "log line message: %.100s", text + at);
        CHECK(n >= 2 && text[n - 2] == '\r' && text[n - 1] == '\n' && !strchr(text, '\n')[1],
              "one CRLF-terminated line (%ld bytes)", n);
        free(text);
    }

    fill_file(g_logfile, 1024L * 1024L + 1L);
    qd3d_rt_event("log probe %u", 1u);
    text = slurp(g_logfile, &n);
    CHECK(text && n < 512 && !strstr(text, "ZZ") && strstr(text, ": log probe 1\r\n"),
          "a log past 1 MiB starts afresh (%ld bytes)", n);
    free(text);

    fill_file(g_logfile, 1024L * 1024L);
    qd3d_rt_event("log probe %u", 2u);
    text = slurp(g_logfile, &n);
    CHECK(text && n > 1024L * 1024L && n < 1024L * 1024L + 512L && text[0] == 'Z' &&
              !strcmp(text + n - strlen(": log probe 2\r\n"), ": log probe 2\r\n"),
          "a log of 1 MiB is appended to (%ld bytes)", n);
    free(text);
    DeleteFileA(g_logfile);
    RemoveDirectoryA(g_logdir);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    qd3d_rt_init();
    qd3d_rt_set_timeout(20000);
    if (!log_setup()) {
        printf("FAIL: cannot point %%TEMP%% at a test directory (error %lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    test_framing();
    test_sync();
    test_proxy();
    test_dead();
    test_log();
    if (failures) {
        printf("qd3d9_ringtest: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("qd3d9_ringtest: all checks passed\n");
    return 0;
}
