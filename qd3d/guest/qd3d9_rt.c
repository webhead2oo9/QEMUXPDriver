/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest runtime: transport selection, the command ring writer, synchronous
 * replies, and the id -> proxy object table. Every call is serialised under
 * one recursive process-wide lock (g.cs).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd3d9_priv.h"

#include <winioctl.h>
#include "../../guest/qxpmap/qxpmap_ioctl.h"

_Static_assert(sizeof(QXPMAP_INFO) == 32, "QXPMAP_INFO layout");
_Static_assert(sizeof(QXPMAP_QD3D_SLOT) == 24, "QXPMAP_QD3D_SLOT layout");

#define QD3D_DEFAULT_TIMEOUT_MS 60000u
#define QD3D_FAULT_RECOVER_MS   2000u
/* Kernel object names: a loopback NAME plus the longest suffix. */
#define QD3D_NAME_MAX           200u
#define QD3D_NAME_SUFFIX_MAX    (sizeof(QD3D_LOOPBACK_SLOT_CLAIM) + 10u)

/* Persistent event log, %TEMP%\QD3D_LOG_FILE, emptied when it has grown
 * past QD3D_LOG_LIMIT bytes. */
#define QD3D_LOG_FILE           "qd3d.log"
#define QD3D_LOG_LIMIT          (1024u * 1024u)

static struct {
    CRITICAL_SECTION cs;
    volatile LONG init;
    int tried;
    int dead;
    enum qd3d_transport transport;

    uint8_t *base;
    uint32_t size;
    volatile struct qd3d_ctrl *ctrl;
    uint8_t *ring;
    uint32_t ring_size;
    const uint8_t *reply;
    uint32_t reply_size;
    uint32_t head;           /* our copy of ctrl->head */
    uint32_t max_cmd;
    uint32_t max_reply;
    uint32_t seq;
    uint32_t timeout_ms;

    /* The claimed slot (loopback and VM). */
    HANDLE claim;            /* loopback slot's claim event */
    HANDLE vm_driver;        /* QXPMAP handle owns the VM slot and its views */
    uint32_t slot;
    void *view;              /* the slot's channel, mapped into this process */
    uint32_t view_size;
    HANDLE kick_event;       /* loopback doorbell */
    volatile uint32_t *vm_mmio; /* VM register page, mapped into this process */
    char why[200];           /* why the connection attempt failed */
    void (*mem_kick)(void *ctx);
    void *mem_ctx;

    /* Current call (valid while the lock is held between begin and done). */
    uint32_t cur_advance;
    int cur_overflow;
    uint32_t new_holds;      /* extra lock holds taken by qd3d_obj_new */

    /* Object table: open addressing, linear probing. */
    struct qd3d_obj **tab;
    uint32_t tab_cap;
    uint32_t tab_count;
    uint32_t next_id;
    int log_stderr;
} g;

/* ------------------------------------------------------------------------ */
/* Lock and diagnostics                                                     */
/* ------------------------------------------------------------------------ */

void qd3d_rt_init(void)
{
    char buf[32];

    if (InterlockedCompareExchange(&g.init, 1, 0) == 0) {
        InitializeCriticalSection(&g.cs);
        g.next_id = 1;
        g.timeout_ms = QD3D_DEFAULT_TIMEOUT_MS;
        if (GetEnvironmentVariableA("QD3D_LOG", buf, sizeof(buf)) > 0 &&
            lstrcmpiA(buf, "stderr") == 0)
            g.log_stderr = 1;
        InterlockedExchange(&g.init, 2);
    } else {
        while (g.init != 2)
            SwitchToThread();
    }
}

void qd3d_rt_lock(void)
{
    EnterCriticalSection(&g.cs);
}

void qd3d_rt_unlock(void)
{
    LeaveCriticalSection(&g.cs);
}

int qd3d_rt_trylock(void)
{
    return TryEnterCriticalSection(&g.cs) ? 1 : 0;
}

void qd3d_rt_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    memcpy(line, "qd3d9: ", 7);
    va_start(ap, fmt);
    n = _vsnprintf(line + 7, sizeof(line) - 9, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(line) - 9)
        n = (int)sizeof(line) - 9;
    line[7 + n] = '\n';
    line[8 + n] = 0;
    OutputDebugStringA(line);
    if (g.log_stderr) {
        DWORD wr;
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE)
            WriteFile(h, line, (DWORD)(8 + n), &wr, NULL);
    }
}

/* _vsnprintf into buf[0, size), always terminated; returns the length. */
static int vformat(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n = _vsnprintf(buf, size - 1u, fmt, ap);

    if (n < 0 || (size_t)n > size - 1u)
        n = (int)(size - 1u);
    buf[n] = 0;
    return n;
}

static int format(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vformat(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Appends "YYYY-MM-DD hh:mm:ss pid exe: msg" to %TEMP%\qd3d.log, emptying
 * the file first when it has grown past QD3D_LOG_LIMIT. Every line is one
 * append-only write, so lines of concurrent processes do not mix. Failures
 * are ignored. */
static void log_file(const char *msg)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    char path[MAX_PATH + sizeof(QD3D_LOG_FILE) + 1u], exe[MAX_PATH], line[640];
    const char *base = exe, *p;
    SYSTEMTIME t;
    DWORD n, hi = 0, wr;
    HANDLE f;
    int len;

    n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH)
        return;
    if (path[n - 1u] != '\\')
        path[n++] = '\\';
    memcpy(path + n, QD3D_LOG_FILE, sizeof(QD3D_LOG_FILE));
    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n == 0 || n >= sizeof(exe))
        memcpy(exe, "?", 2);
    for (p = exe; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    GetLocalTime(&t);
    len = format(line, sizeof(line) - 2u, "%04u-%02u-%02u %02u:%02u:%02u %lu %s: %s", (unsigned)t.wYear,
                 (unsigned)t.wMonth, (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute,
                 (unsigned)t.wSecond, (unsigned long)GetCurrentProcessId(), base, msg);
    line[len++] = '\r';
    line[len++] = '\n';
    f = CreateFileA(path, FILE_APPEND_DATA, share, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSize(f, &hi) > QD3D_LOG_LIMIT || hi) {
        CloseHandle(f);
        f = CreateFileA(path, GENERIC_WRITE, share, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE)
            return;
    }
    WriteFile(f, line, (DWORD)len, &wr, NULL);
    CloseHandle(f);
}

void qd3d_rt_event(const char *fmt, ...)
{
    char msg[400];
    va_list ap;

    va_start(ap, fmt);
    vformat(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    qd3d_rt_log("%s", msg);
    log_file(msg);
}

/* Why the connection attempt failed, for the "not connected" event. */
static void set_why(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_why(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vformat(g.why, sizeof(g.why), fmt, ap);
    va_end(ap);
}

void qd3d_rt_set_timeout(uint32_t ms)
{
    g.timeout_ms = ms;
}

/* ------------------------------------------------------------------------ */
/* Channel validation and transports                                        */
/* ------------------------------------------------------------------------ */

static int is_pow2(uint32_t v)
{
    return v && !(v & (v - 1u));
}

/* Validates the control block of a channel mapped at base[0, size) and
 * binds it. Returns 1 on success. */
static int channel_bind(uint8_t *base, uint32_t size)
{
    volatile struct qd3d_ctrl *c = (volatile struct qd3d_ctrl *)base;
    uint64_t ring_end, reply_end;
    uint32_t head, tail;

    if (size < QD3D_CTRL_SIZE)
        return 0;
    if (c->magic != QD3D_MAGIC)
        return 0;
    MemoryBarrier();
    if (c->abi != QD3D_ABI_VERSION) {
        qd3d_rt_log("channel abi %u, expected %u", (unsigned)c->abi, QD3D_ABI_VERSION);
        return 0;
    }
    if (c->total_size < QD3D_CTRL_SIZE || c->total_size > size ||
        (c->ring_offset % 4096u) || c->ring_offset < QD3D_CTRL_SIZE ||
        !is_pow2(c->ring_size) || c->ring_size < 65536u ||
        (c->reply_offset % 4096u) || c->reply_offset < QD3D_CTRL_SIZE ||
        c->reply_size < 65536u) {
        qd3d_rt_log("channel geometry invalid");
        return 0;
    }
    ring_end = (uint64_t)c->ring_offset + c->ring_size;
    reply_end = (uint64_t)c->reply_offset + c->reply_size;
    if (ring_end > c->total_size || reply_end > c->total_size ||
        !(ring_end <= c->reply_offset || reply_end <= c->ring_offset)) {
        qd3d_rt_log("channel regions invalid");
        return 0;
    }
    head = c->head;
    tail = c->tail;
    if ((head % QD3D_CMD_ALIGN) || (tail % QD3D_CMD_ALIGN) || head - tail > c->ring_size) {
        qd3d_rt_log("channel cursors invalid (head %u tail %u)", (unsigned)head, (unsigned)tail);
        return 0;
    }
    g.base = base;
    g.size = size;
    g.ctrl = c;
    g.ring = base + c->ring_offset;
    g.ring_size = c->ring_size;
    g.reply = base + c->reply_offset;
    g.reply_size = c->reply_size;
    g.head = head;
    g.seq = c->sync_req; /* after any sync command a previous owner left */
    g.max_cmd = c->ring_size / 2u;
    g.max_reply = c->reply_size - (uint32_t)sizeof(struct qd3d_reply);
    g.dead = 0;
    return 1;
}

/* Waits for the host to be READY: immediately if it is, up to
 * QD3D_FAULT_RECOVER_MS while it reports FAULT (the VM device resets a
 * faulted session after about a second). */
static int host_ready(volatile struct qd3d_ctrl *c)
{
    DWORD start = GetTickCount();

    for (;;) {
        uint32_t s = c->host_state;
        if (s == QD3D_HOST_READY)
            return 1;
        if (s != QD3D_HOST_FAULT || GetTickCount() - start > QD3D_FAULT_RECOVER_MS)
            return 0;
        Sleep(10);
    }
}

static void unbind(void)
{
    g.base = NULL;
    g.ctrl = NULL;
    g.ring = NULL;
    g.reply = NULL;
}

/* ------------------------------------------------------------------------ */
/* Channel slots                                                            */
/* ------------------------------------------------------------------------ */

/* Writes v in decimal at p, terminated. */
static void put_dec(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
}

/* Maps the channel of claimed slot i to g.view/g.view_size for one
 * transport: 1 mapped, 0 the transport has no slot i, -1 failed (g.why). */
typedef int (*slot_map_fn)(void *ctx, uint32_t slot);

/*
 * Claims the lowest free slot below count and maps its channel (qd3d_proto.h,
 * Transports). Slot i is this process's when CreateEvent creates the claim
 * <prefix><i>. An existing claim belongs to a live owner
 * (ERROR_ALREADY_EXISTS); one this process may not open (an owner in another
 * logon session) fails outright: either way the slot is busy. The claim has
 * no security attributes, so it is not inheritable (a child would keep the
 * slot), and stays open while the process uses the slot; the kernel closes
 * it when the process ends however it ends. Handles are process-wide, so no
 * thread exit gives the slot up (a mutex would be abandoned with its
 * owning thread). VM claims are instead owned by qxpmap.sys.
 */
static int claim_slot(const char *transport, const char *prefix, uint32_t count, slot_map_fn map, void *ctx)
{
    char name[QD3D_NAME_MAX + QD3D_NAME_SUFFIX_MAX];
    size_t n = strlen(prefix);
    uint32_t i;

    memcpy(name, prefix, n);
    for (i = 0; i < count; i++) {
        HANDLE e;
        int r;

        put_dec(name + n, i);
        e = CreateEventA(NULL, TRUE, FALSE, name);
        if (!e)
            continue;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(e);
            continue;
        }
        r = map(ctx, i);
        if (r > 0) {
            g.claim = e;
            g.slot = i;
            return 1;
        }
        CloseHandle(e);
        if (r < 0)
            return 0;
        count = i; /* slots 0 .. i-1 exist and are busy */
        break;
    }
    set_why("%s: all %u slots busy", transport, (unsigned)count);
    return 0;
}

/* Binds the claimed slot's channel once its host is READY (a slot that a
 * dead owner left FAULTed recovers within QD3D_FAULT_RECOVER_MS). */
static int bind_slot(const char *transport)
{
    volatile struct qd3d_ctrl *c = (volatile struct qd3d_ctrl *)g.view;

    if (!host_ready(c)) {
        set_why("%s: slot %u not ready (host_state %u)", transport, (unsigned)g.slot, (unsigned)c->host_state);
        return 0;
    }
    if (!channel_bind((uint8_t *)g.view, g.view_size)) {
        set_why("%s: slot %u holds no valid channel", transport, (unsigned)g.slot);
        return 0;
    }
    return 1;
}

/* Gives the slot back. VM views belong to the driver, never UnmapViewOfFile. */
static void release_slot(void)
{
    DWORD got;

    unbind();
    if (g.vm_driver) {
        (void)DeviceIoControl(g.vm_driver, IOCTL_QXPMAP_QD3D_RELEASE,
                              NULL, 0, NULL, 0, &got, NULL);
        CloseHandle(g.vm_driver);
    } else if (g.view) {
        UnmapViewOfFile(g.view);
    }
    if (g.kick_event)
        CloseHandle(g.kick_event);
    if (g.claim)
        CloseHandle(g.claim);
    g.view = NULL;
    g.vm_mmio = NULL;
    g.vm_driver = NULL;
    g.kick_event = NULL;
    g.claim = NULL;
    g.transport = QD3D_TRANSPORT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Transports                                                               */
/* ------------------------------------------------------------------------ */

struct loopback_name {
    char base[QD3D_NAME_MAX]; /* NAME */
    size_t len;
};

static int loopback_map(void *ctx, uint32_t slot)
{
    const struct loopback_name *l = (const struct loopback_name *)ctx;
    char name[QD3D_NAME_MAX + QD3D_NAME_SUFFIX_MAX];
    MEMORY_BASIC_INFORMATION mbi;
    HANDLE map;
    DWORD err;

    memcpy(name, l->base, l->len + 1u);
    if (slot) {
        name[l->len] = '_';
        put_dec(name + l->len + 1u, slot);
    }
    map = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!map) {
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            return 0;
        set_why("loopback: cannot open %s (error %lu)", name, (unsigned long)err);
        return -1;
    }
    g.view = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    err = GetLastError();
    CloseHandle(map); /* the view keeps the section */
    if (!g.view || !VirtualQuery(g.view, &mbi, sizeof(mbi))) {
        set_why("loopback: cannot map %s (error %lu)", name, (unsigned long)err);
        if (g.view)
            UnmapViewOfFile(g.view);
        g.view = NULL;
        return -1;
    }
    g.view_size = (uint32_t)mbi.RegionSize;
    return 1;
}

/* 1 connected, 0 failed (g.why), -1 no harness runs. QD3D_LOOPBACK_NAME
 * selects another NAME, so several harnesses can run side by side. */
static int connect_loopback(void)
{
    struct loopback_name l;
    char name[QD3D_NAME_MAX + QD3D_NAME_SUFFIX_MAX];
    DWORD n;

    n = GetEnvironmentVariableA("QD3D_LOOPBACK_NAME", l.base, sizeof(l.base));
    if (n == 0 || n >= sizeof(l.base)) {
        n = sizeof(QD3D_LOOPBACK_MAPPING) - 1u;
        memcpy(l.base, QD3D_LOOPBACK_MAPPING, n + 1u);
    }
    l.len = n;
    memcpy(name, l.base, n);
    memcpy(name + n, "_KICK", sizeof("_KICK"));
    g.kick_event = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (!g.kick_event) {
        set_why("loopback: no harness on %s (error %lu)", l.base, (unsigned long)GetLastError());
        return -1;
    }
    memcpy(name + n, QD3D_LOOPBACK_SLOT_CLAIM, sizeof(QD3D_LOOPBACK_SLOT_CLAIM));
    if (!claim_slot("loopback", name, QD3D_VM_SLOTS, loopback_map, &l) || !bind_slot("loopback")) {
        release_slot();
        return 0;
    }
    g.transport = QD3D_TRANSPORT_LOOPBACK;
    return 1;
}

/* The driver's non-inheritable handle owns the slot until explicit release
 * or process cleanup. No physical addresses or global named claims travel
 * from user mode to the driver. */
static int connect_vm(void)
{
    QXPMAP_INFO info;
    QXPMAP_QD3D_SLOT slot;
    DWORD got = 0, err;

    g.vm_driver = CreateFileA(QXPMAP_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g.vm_driver == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        g.vm_driver = NULL;
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            set_why("QD3D mapping driver not installed (\\\\.\\QXPMAP error %lu)", (unsigned long)err);
        else
            set_why("cannot open QD3D mapping driver (\\\\.\\QXPMAP error %lu)", (unsigned long)err);
        return 0;
    }
    memset(&info, 0, sizeof(info));
    if (!DeviceIoControl(g.vm_driver, IOCTL_QXPMAP_QUERY, NULL, 0,
                         &info, sizeof(info), &got, NULL)) {
        set_why("QD3D mapping driver query failed (error %lu)", (unsigned long)GetLastError());
        goto fail;
    }
    if (got != sizeof(info) || info.Version != QXPMAP_VERSION ||
        info.Qd3dSlots > QD3D_VM_SLOTS ||
        (info.Qd3dSlots && info.Qd3dSlotSize != QD3D_VM_CHANNEL_SIZE) ||
        (info.Qd3dAbi && info.Qd3dAbi != QD3D_ABI_VERSION)) {
        set_why("incompatible QD3D mapping driver or device");
        goto fail;
    }
    if (!info.Qd3dSlots) {
        set_why("no QD3D device");
        goto fail;
    }
    memset(&slot, 0, sizeof(slot));
    if (!DeviceIoControl(g.vm_driver, IOCTL_QXPMAP_QD3D_CLAIM, NULL, 0,
                         &slot, sizeof(slot), &got, NULL)) {
        err = GetLastError();
        if (err == ERROR_BUSY)
            set_why("all QD3D slots busy");
        else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_DEV_NOT_EXIST ||
                 err == ERROR_DEVICE_NOT_CONNECTED)
            set_why("no QD3D device");
        else
            set_why("cannot claim a QD3D slot (error %lu)", (unsigned long)err);
        goto fail;
    }
    if (got != sizeof(slot) || slot.Slot >= info.Qd3dSlots ||
        slot.ChannelSize != QD3D_VM_CHANNEL_SIZE ||
        !slot.Registers || slot.Registers > UINTPTR_MAX ||
        !slot.Channel || slot.Channel > UINTPTR_MAX) {
        set_why("QD3D mapping driver returned an invalid slot");
        goto fail;
    }
    g.slot = slot.Slot;
    g.vm_mmio = (volatile uint32_t *)(uintptr_t)slot.Registers;
    g.view = (void *)(uintptr_t)slot.Channel;
    g.view_size = slot.ChannelSize;
    if (!bind_slot("vm"))
        goto fail;
    g.transport = QD3D_TRANSPORT_VM;
    return 1;

fail:
    release_slot();
    return 0;
}

int qd3d_rt_connect(void)
{
    char sel[32];
    char tmo[32];
    DWORD n;
    int r = 0;

    qd3d_rt_lock();
    if (g.tried) {
        r = qd3d_rt_alive();
        qd3d_rt_unlock();
        return r;
    }
    g.tried = 1;
    n = GetEnvironmentVariableA("QD3D_TIMEOUT_MS", tmo, sizeof(tmo));
    if (n > 0 && n < sizeof(tmo))
        g.timeout_ms = (uint32_t)strtoul(tmo, NULL, 10);
    n = GetEnvironmentVariableA("QD3D_TRANSPORT", sel, sizeof(sel));
    if (n == 0 || n >= sizeof(sel)) {
        /* A harness on this machine (development) wins over the VM. */
        r = connect_loopback();
        if (r < 0)
            r = connect_vm();
    } else if (lstrcmpiA(sel, "loopback") == 0) {
        r = connect_loopback();
    } else if (lstrcmpiA(sel, "vm") == 0) {
        r = connect_vm();
    } else if (lstrcmpiA(sel, "none") == 0) {
        set_why("QD3D_TRANSPORT=none");
    } else {
        set_why("unknown QD3D_TRANSPORT '%s'", sel);
    }
    if (r > 0) {
        /* Which d3d9.dll connected: a process with two proxies (a game's own
         * copy beside a system-wide install) shows up here. */
        char self[MAX_PATH];
        MEMORY_BASIC_INFORMATION mbi;

        if (!VirtualQuery(&g, &mbi, sizeof(mbi)) ||
            !GetModuleFileNameA((HMODULE)mbi.AllocationBase, self, sizeof(self)))
            self[0] = 0;
        self[sizeof(self) - 1] = 0;
        qd3d_rt_event("connected (%s) slot %u: ring %u, reply %u, %s",
                      g.transport == QD3D_TRANSPORT_VM ? "vm" : "loopback", (unsigned)g.slot,
                      (unsigned)g.ring_size, (unsigned)g.reply_size, self);
    } else {
        qd3d_rt_event("not connected: %s", g.why);
    }
    qd3d_rt_unlock();
    return r > 0;
}

void qd3d_rt_disconnect(void)
{
    if (!qd3d_rt_trylock())
        return;
    if (g.transport == QD3D_TRANSPORT_LOOPBACK || g.transport == QD3D_TRANSPORT_VM)
        release_slot();
    qd3d_rt_unlock();
}

int qd3d_rt_attach_memory(void *base, uint32_t size, void (*kick)(void *ctx), void *ctx)
{
    int ok;

    qd3d_rt_lock();
    g.tried = 1;
    g.mem_kick = kick;
    g.mem_ctx = ctx;
    ok = channel_bind((uint8_t *)base, size) && g.ctrl->host_state == QD3D_HOST_READY;
    if (ok)
        g.transport = QD3D_TRANSPORT_MEMORY;
    else
        unbind();
    qd3d_rt_unlock();
    return ok;
}

void qd3d_rt_detach_memory(void)
{
    qd3d_rt_lock();
    unbind();
    g.transport = QD3D_TRANSPORT_NONE;
    g.dead = 0;
    g.mem_kick = NULL;
    qd3d_rt_unlock();
}

int qd3d_rt_alive(void)
{
    return g.ctrl != NULL && !g.dead;
}

enum qd3d_transport qd3d_rt_transport(void)
{
    return g.ctrl ? g.transport : QD3D_TRANSPORT_NONE;
}

void qd3d_rt_set_limits(uint32_t max_cmd, uint32_t max_reply)
{
    qd3d_rt_lock();
    if (g.ctrl) {
        g.max_cmd = g.ring_size / 2u;
        if (max_cmd < g.max_cmd)
            g.max_cmd = max_cmd & ~(QD3D_CMD_ALIGN - 1u);
        g.max_reply = g.reply_size - (uint32_t)sizeof(struct qd3d_reply);
        if (max_reply < g.max_reply)
            g.max_reply = max_reply;
    }
    qd3d_rt_unlock();
}

uint32_t qd3d_rt_max_upload(void)
{
    uint32_t m = g.ring_size / 4u;
    uint32_t c = g.max_cmd > 16u + sizeof(struct qd3d_region) ?
                 g.max_cmd - 16u - (uint32_t)sizeof(struct qd3d_region) : 0u;

    return m < c ? m : c;
}

uint32_t qd3d_rt_max_readback(void)
{
    return g.max_reply;
}

/* ------------------------------------------------------------------------ */
/* Waiting                                                                  */
/* ------------------------------------------------------------------------ */

static void kick(void)
{
    switch (g.transport) {
    case QD3D_TRANSPORT_LOOPBACK:
        SetEvent(g.kick_event);
        break;
    case QD3D_TRANSPORT_VM:
        g.vm_mmio[QD3D_VM_REG_KICK / 4u] = 1u;
        break;
    case QD3D_TRANSPORT_MEMORY:
        if (g.mem_kick)
            g.mem_kick(g.mem_ctx);
        break;
    default:
        break;
    }
}

static void mark_dead(const char *why)
{
    if (!g.dead && g.ctrl)
        qd3d_rt_event("channel dead: %s (slot %u, host_state %u fault_op %u fault_code %u)", why,
                      (unsigned)g.slot, (unsigned)g.ctrl->host_state, (unsigned)g.ctrl->fault_op,
                      (unsigned)g.ctrl->fault_code);
    g.dead = 1;
}

/* Polls pred until it holds. Spins with pause/yield first and only sleeps
 * once a wait has lasted longer than a frame's worth of yields. Returns 0
 * (channel dead) if the host leaves READY or makes no progress for
 * g.timeout_ms. */
static int wait_for(int (*pred)(void *arg), void *arg)
{
    volatile struct qd3d_ctrl *c = g.ctrl;
    uint32_t spins = 0;
    uint32_t t = c->tail, d = c->sync_done, f = c->frames_done;
    DWORD start = 0, progress = 0;

    for (;;) {
        if (pred(arg))
            return 1;
        if (c->host_state != QD3D_HOST_READY) {
            mark_dead("host left READY");
            return 0;
        }
        if (spins < 64u) {
            YieldProcessor();
        } else if (spins < 256u) {
            SwitchToThread();
        } else {
            DWORD now = GetTickCount();
            if (!start)
                start = progress = now;
            if (c->tail != t || c->sync_done != d || c->frames_done != f) {
                t = c->tail;
                d = c->sync_done;
                f = c->frames_done;
                progress = now;
            }
            if (g.timeout_ms && now - progress > g.timeout_ms) {
                mark_dead("host made no progress");
                return 0;
            }
            if (c->host_idle == 1u && c->tail != g.head)
                kick();
            if (now - start < 20u)
                Sleep(0);
            else
                Sleep(1);
        }
        spins++;
    }
}

struct space_req {
    uint32_t need_stride;
    uint32_t pad;
};

static int space_ok(void *arg)
{
    struct space_req *r = (struct space_req *)arg;
    uint32_t used = g.head - g.ctrl->tail;
    uint32_t off = g.head & (g.ring_size - 1u);
    uint32_t to_end = g.ring_size - off;

    if (used > g.ring_size)
        return 1; /* corrupt; caller re-checks */
    r->pad = to_end < r->need_stride ? to_end : 0u;
    return g.ring_size - used >= r->pad + r->need_stride;
}

static int sync_ok(void *arg)
{
    return g.ctrl->sync_done == *(uint32_t *)arg;
}

/* ------------------------------------------------------------------------ */
/* Call builder                                                             */
/* ------------------------------------------------------------------------ */

void qd3d_call_begin(struct qd3d_call *k, uint32_t op, uint32_t obj, int sync,
                     uint64_t payload_bytes)
{
    uint64_t size = 16u + payload_bytes;
    struct space_req r;
    struct qd3d_cmd *cmd;

    memset(k, 0, sizeof(*k));
    k->op = op;
    k->seq = sync ? 1u : 0u; /* placeholder so a dead call knows it is sync */
    qd3d_rt_lock();
    g.cur_advance = 0;
    g.cur_overflow = 0;
    if (!qd3d_rt_alive()) {
        k->dead = 1;
        return;
    }
    if (g.ctrl->host_state != QD3D_HOST_READY) {
        mark_dead("host not READY");
        k->dead = 1;
        return;
    }
    if (size > g.max_cmd) {
        qd3d_rt_log("op %u: command of %llu bytes exceeds max %u; dropped", (unsigned)op,
                    (unsigned long long)size, (unsigned)g.max_cmd);
        k->dead = 1;
        return;
    }
    r.need_stride = QD3D_CMD_STRIDE((uint32_t)size);
    r.pad = 0;
    if (!wait_for(space_ok, &r)) {
        k->dead = 1;
        return;
    }
    if (g.head - g.ctrl->tail > g.ring_size) {
        mark_dead("tail ahead of head");
        k->dead = 1;
        return;
    }
    if (r.pad) {
        struct qd3d_cmd *pad = (struct qd3d_cmd *)(g.ring + (g.head & (g.ring_size - 1u)));
        pad->size = r.pad;
        pad->op = QD3D_OP_PAD;
        pad->obj = 0;
        pad->seq = 0;
    }
    cmd = (struct qd3d_cmd *)(g.ring + ((g.head + r.pad) & (g.ring_size - 1u)));
    if (sync) {
        if (++g.seq == 0u)
            g.seq = 1u;
        k->seq = g.seq;
    }
    cmd->size = (uint32_t)size;
    cmd->op = op;
    cmd->obj = obj;
    cmd->seq = k->seq;
    k->dst = (uint8_t *)(cmd + 1);
    k->left = (uint32_t)payload_bytes;
    g.cur_advance = r.pad + r.need_stride;
}

void qd3d_put_u32(struct qd3d_call *k, uint32_t v)
{
    if (k->dead)
        return;
    if (k->left < 4u) {
        g.cur_overflow = 1;
        return;
    }
    memcpy(k->dst, &v, 4);
    k->dst += 4;
    k->left -= 4u;
}

void qd3d_put_data(struct qd3d_call *k, const void *p, uint64_t nbytes)
{
    uint64_t padded = QD3D_PAD4(nbytes);
    uint32_t n32;

    if (!p) {
        qd3d_put_u32(k, QD3D_DATA_NULL);
        return;
    }
    if (k->dead)
        return;
    if (nbytes >= QD3D_DATA_NULL || 4u + padded > k->left) {
        g.cur_overflow = 1;
        return;
    }
    n32 = (uint32_t)nbytes;
    memcpy(k->dst, &n32, 4);
    memcpy(k->dst + 4, p, (size_t)nbytes);
    memset(k->dst + 4 + nbytes, 0, (size_t)(padded - nbytes));
    k->dst += 4u + (uint32_t)padded;
    k->left -= 4u + (uint32_t)padded;
}

void qd3d_put_raw(struct qd3d_call *k, const void *p, uint64_t nbytes)
{
    if (k->dead)
        return;
    if (nbytes > k->left) {
        g.cur_overflow = 1;
        return;
    }
    memcpy(k->dst, p, (size_t)nbytes);
    k->dst += (uint32_t)nbytes;
    k->left -= (uint32_t)nbytes;
}

uint32_t qd3d_call_end(struct qd3d_call *k)
{
    volatile struct qd3d_ctrl *c = g.ctrl;
    const struct qd3d_reply *rep;
    uint32_t seq = k->seq;

    if (k->dead)
        return seq ? (uint32_t)D3DERR_DRIVERINTERNALERROR : 0u;
    if (k->left || g.cur_overflow) {
        qd3d_rt_log("op %u: payload encoding mismatch (%u bytes unwritten%s)", (unsigned)k->op,
                    (unsigned)k->left, g.cur_overflow ? ", overflow" : "");
        memset(k->dst, 0, k->left);
        k->dst += k->left;
        k->left = 0;
    }
    if (seq)
        c->sync_req = seq;
    MemoryBarrier();
    g.head += g.cur_advance;
    c->head = g.head;
    MemoryBarrier();
    if (c->host_idle == 1u)
        kick();
    if (!seq)
        return 0u;
    if (!wait_for(sync_ok, &seq)) {
        k->dead = 1;
        return (uint32_t)D3DERR_DRIVERINTERNALERROR;
    }
    MemoryBarrier();
    rep = (const struct qd3d_reply *)g.reply;
    if (rep->seq != seq || rep->size > g.reply_size - (uint32_t)sizeof(*rep)) {
        mark_dead("malformed reply");
        k->dead = 1;
        return (uint32_t)D3DERR_DRIVERINTERNALERROR;
    }
    k->rp = g.reply + sizeof(*rep);
    k->rleft = rep->size;
    return rep->ret;
}

int qd3d_get_u32(struct qd3d_call *k, uint32_t *v)
{
    if (k->dead || !k->rp || k->rleft < 4u) {
        *v = 0;
        return 0;
    }
    memcpy(v, k->rp, 4);
    k->rp += 4;
    k->rleft -= 4u;
    return 1;
}

int qd3d_get_data(struct qd3d_call *k, void *dst, uint64_t nbytes)
{
    uint32_t n;
    uint64_t padded;

    if (!qd3d_get_u32(k, &n) || n == QD3D_DATA_NULL) {
        memset(dst, 0, (size_t)nbytes);
        return 0;
    }
    padded = QD3D_PAD4(n);
    if (padded > k->rleft) {
        k->rleft = 0;
        memset(dst, 0, (size_t)nbytes);
        return 0;
    }
    if (n != nbytes) {
        k->rp += padded;
        k->rleft -= (uint32_t)padded;
        memset(dst, 0, (size_t)nbytes);
        return 0;
    }
    memcpy(dst, k->rp, (size_t)nbytes);
    k->rp += padded;
    k->rleft -= (uint32_t)padded;
    return 1;
}

void qd3d_skip_data(struct qd3d_call *k)
{
    uint32_t n;
    uint64_t padded;

    if (!qd3d_get_u32(k, &n) || n == QD3D_DATA_NULL)
        return;
    padded = QD3D_PAD4(n);
    if (padded > k->rleft) {
        k->rleft = 0;
        return;
    }
    k->rp += padded;
    k->rleft -= (uint32_t)padded;
}

int qd3d_get_raw(struct qd3d_call *k, void *dst, uint64_t nbytes)
{
    if (k->dead || !k->rp || nbytes > k->rleft) {
        memset(dst, 0, (size_t)nbytes);
        return 0;
    }
    memcpy(dst, k->rp, (size_t)nbytes);
    k->rp += (uint32_t)nbytes;
    k->rleft -= (uint32_t)nbytes;
    return 1;
}

void qd3d_call_done(struct qd3d_call *k)
{
    uint32_t holds = g.new_holds;

    (void)k;
    g.new_holds = 0;
    g.cur_advance = 0;
    LeaveCriticalSection(&g.cs);
    while (holds--)
        LeaveCriticalSection(&g.cs);
}

/* ------------------------------------------------------------------------ */
/* Session helpers                                                          */
/* ------------------------------------------------------------------------ */

void qd3d_rt_hello_mark(const struct qd3d_call *k)
{
    if (k->dead)
        return;
    g.ctrl->guest_pid = GetCurrentProcessId();
    g.ctrl->session = g.ctrl->session + 1u;
}

void qd3d_rt_frame_mark(const struct qd3d_call *k)
{
    if (k->dead)
        return;
    g.ctrl->frames_submitted = g.ctrl->frames_submitted + 1u;
}

void qd3d_rt_heartbeat(void)
{
    volatile struct qd3d_ctrl *c = g.ctrl;

    if (c && !g.dead)
        c->heartbeat = c->heartbeat + 1u;
}

uint32_t qd3d_rt_frames_ahead(void)
{
    if (!qd3d_rt_alive())
        return 0;
    return g.ctrl->frames_submitted - g.ctrl->frames_done;
}

static int frames_ok(void *arg)
{
    return g.ctrl->frames_submitted - g.ctrl->frames_done <= *(uint32_t *)arg;
}

void qd3d_rt_frame_pace(uint32_t max_ahead)
{
    qd3d_rt_lock();
    if (qd3d_rt_alive())
        (void)wait_for(frames_ok, &max_ahead);
    qd3d_rt_unlock();
}

/* ------------------------------------------------------------------------ */
/* Object table                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t id_hash(uint32_t id)
{
    return id * 2654435761u;
}

static struct qd3d_obj **tab_slot(uint32_t id)
{
    uint32_t mask = g.tab_cap - 1u;
    uint32_t i;

    if (!g.tab)
        return NULL;
    for (i = id_hash(id) & mask; g.tab[i]; i = (i + 1u) & mask)
        if (g.tab[i]->id == id)
            return &g.tab[i];
    return NULL;
}

static int tab_put_raw(struct qd3d_obj **tab, uint32_t cap, struct qd3d_obj *o)
{
    uint32_t mask = cap - 1u;
    uint32_t i;

    for (i = id_hash(o->id) & mask; tab[i]; i = (i + 1u) & mask)
        ;
    tab[i] = o;
    return 1;
}

static int tab_insert(struct qd3d_obj *o)
{
    if ((g.tab_count + 1u) * 2u > g.tab_cap) {
        uint32_t cap = g.tab_cap ? g.tab_cap * 2u : 256u;
        struct qd3d_obj **t;
        uint32_t i;

        if (cap < g.tab_cap)
            return 0;
        t = (struct qd3d_obj **)calloc(cap, sizeof(*t));
        if (!t)
            return 0;
        for (i = 0; i < g.tab_cap; i++)
            if (g.tab[i])
                tab_put_raw(t, cap, g.tab[i]);
        free(g.tab);
        g.tab = t;
        g.tab_cap = cap;
    }
    tab_put_raw(g.tab, g.tab_cap, o);
    g.tab_count++;
    return 1;
}

static void tab_remove(struct qd3d_obj *o)
{
    struct qd3d_obj **s = tab_slot(o->id);
    uint32_t mask, i, j;

    if (!s || *s != o)
        return;
    mask = g.tab_cap - 1u;
    i = (uint32_t)(s - g.tab);
    g.tab[i] = NULL;
    g.tab_count--;
    /* Backward-shift deletion keeps probe chains intact. */
    for (j = (i + 1u) & mask; g.tab[j]; j = (j + 1u) & mask) {
        uint32_t h = id_hash(g.tab[j]->id) & mask;
        int move = (i <= j) ? (h <= i || h > j) : (h <= i && h > j);
        if (move) {
            g.tab[i] = g.tab[j];
            g.tab[j] = NULL;
            i = j;
        }
    }
}

static int type_ok(uint32_t type)
{
    return type < (uint32_t)QD3D_T_COUNT && qd3d_type_concrete[type] && qd3d9_vtables[type];
}

void qd3d_obj_foreach(void (*fn)(struct qd3d_obj *o, void *ctx), void *ctx)
{
    uint32_t i;

    qd3d_rt_lock();
    for (i = 0; i < g.tab_cap; i++)
        if (g.tab[i])
            fn(g.tab[i], ctx);
    qd3d_rt_unlock();
}

uint32_t qd3d_obj_id(const void *iface)
{
    return iface ? qd3d_obj(iface)->id : 0u;
}

struct qd3d_obj *qd3d_obj_device(const void *iface)
{
    return iface ? qd3d_obj(iface)->device : NULL;
}

static void obj_revive(struct qd3d_obj *o);

ULONG qd3d_obj_addref(struct qd3d_obj *o)
{
    LONG r;

    /* Native levels share their texture's count. */
    if (o->owned_level && o->parent)
        return qd3d_obj_addref(o->parent);
    r = InterlockedIncrement(&o->refs);
    /* 0 -> 1: a held proxy the application had released comes back. */
    if (r == 1)
        obj_revive(o);
    return (ULONG)r;
}

static struct qd3d_obj *obj_alloc(struct qd3d_obj *device, uint32_t id, uint32_t type)
{
    struct qd3d_obj *o = (struct qd3d_obj *)calloc(1, sizeof(*o));

    if (!o)
        return NULL;
    o->vtbl = qd3d9_vtables[type];
    o->refs = 1;
    o->id = id;
    o->type = type;
    if (type == QD3D_T_IDirect3DDevice9) {
        o->device = o;
    } else {
        o->device = device;
        o->parent = device;
    }
    if (!tab_insert(o)) {
        free(o);
        return NULL;
    }
    if (o->parent)
        qd3d_obj_addref(o->parent);
    return o;
}

struct qd3d_obj *qd3d_obj_new(struct qd3d_obj *device, uint32_t type)
{
    struct qd3d_obj *o;

    if (!type_ok(type))
        return NULL;
    qd3d_rt_lock();
    if (g.next_id > 0x7fffffffu) {
        qd3d_rt_unlock();
        return NULL;
    }
    o = obj_alloc(device, g.next_id, type);
    if (!o) {
        qd3d_rt_unlock();
        return NULL;
    }
    g.next_id++;
    /* Keep the lock until the creating call's qd3d_call_done so guest ids
     * reach the host in allocation order (the host requires each NEWOBJ id
     * to exceed every id bound before it). */
    g.new_holds++;
    return o;
}

uint32_t qd3d_rt_new_id(void)
{
    uint32_t id = 0;

    qd3d_rt_lock();
    if (g.next_id <= 0x7fffffffu)
        id = g.next_id++;
    qd3d_rt_unlock();
    return id;
}

struct qd3d_obj *qd3d_obj_new_local(uint32_t type, uint32_t id)
{
    struct qd3d_obj *o;

    if (!type_ok(type) || !id)
        return NULL;
    o = (struct qd3d_obj *)calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->vtbl = qd3d9_vtables[type];
    o->refs = 1;
    o->id = id;
    o->type = type;
    o->local = 1;
    if (type == QD3D_T_IDirect3DDevice9)
        o->device = o;
    return o;
}

struct qd3d_obj *qd3d_obj_from_id(struct qd3d_obj *device, uint32_t id,
                                  uint32_t type, uint32_t declared_type)
{
    struct qd3d_obj **s;
    struct qd3d_obj *o;

    if (!id)
        return NULL;
    if (!type_ok(type) || !qd3d_type_isa(type, declared_type)) {
        qd3d_rt_log("reply object %#x has invalid type %u (declared %u)", (unsigned)id,
                    (unsigned)type, (unsigned)declared_type);
        return NULL;
    }
    qd3d_rt_lock();
    s = tab_slot(id);
    if (s) {
        o = *s;
        if (o->type != type) {
            qd3d_rt_log("reply object %#x type %u, proxy type %u", (unsigned)id,
                        (unsigned)type, (unsigned)o->type);
            o = NULL;
        } else {
            qd3d_obj_addref(o);
        }
    } else {
        o = obj_alloc(device, id, type);
    }
    qd3d_rt_unlock();
    return o;
}

void qd3d_obj_set_parent(struct qd3d_obj *o, struct qd3d_obj *p)
{
    struct qd3d_obj *old;

    qd3d_rt_lock();
    old = o->parent;
    if (old == p) {
        qd3d_rt_unlock();
        return;
    }
    if (p)
        qd3d_obj_addref(p);
    o->parent = p;
    qd3d_rt_unlock();
    if (old)
        qd3d_obj_release(old);
}

void qd3d_obj_discard(struct qd3d_obj *o)
{
    struct qd3d_obj *parent;

    if (!o)
        return;
    qd3d_rt_lock();
    tab_remove(o);
    if (o->ext_free)
        o->ext_free(o);
    parent = o->parent;
    qd3d_rt_unlock();
    if (parent)
        qd3d_obj_release(parent);
    free(o);
}

/* Lock held on entry, released on return. Holds the object gives (ext_free,
 * device teardown) go before its own RELEASE, so the host releases children
 * before their device. */
static void obj_destroy(struct qd3d_obj *o)
{
    struct qd3d_obj *parent = o->parent;
    int parent_hold = o->parent_hold;
    struct qd3d_call k;

    o->dying = 1;
    if (!o->local)
        tab_remove(o);
    if (o->ext_free)
        o->ext_free(o);
    if (!o->local) {
        qd3d_call_begin(&k, QD3D_OP_RELEASE, o->id, 0, 0u);
        (void)qd3d_call_end(&k);
        qd3d_call_done(&k);
    }
    qd3d_rt_unlock();
    if (parent) {
        if (parent_hold)
            qd3d_obj_unhold(parent);
        else if (!o->owned_level)
            qd3d_obj_release(parent);
    }
    free(o);
}

/* refs reached 0 while something still holds o (lock held): the parent's
 * public reference goes, as the native runtime drops the device reference
 * of a released resource that is still bound. Returns the parent whose
 * public reference the caller releases after unlocking, or NULL. */
static struct qd3d_obj *obj_go_dormant(struct qd3d_obj *o)
{
    struct qd3d_obj *parent = o->parent;

    if (!parent || o->parent_hold)
        return NULL;
    parent->holds++;
    o->parent_hold = 1;
    return parent;
}

static void obj_revive(struct qd3d_obj *o)
{
    struct qd3d_obj *parent;
    int was_hold;

    qd3d_rt_lock();
    if (!o->parent && o->device && o->device != o) {
        /* The container died while o was held elsewhere: reference the
         * device, which holds o and so is alive. */
        o->parent = o->device;
        o->owned_level = 0;
        o->parent_hold = 0;
        qd3d_obj_addref(o->parent);
        qd3d_rt_unlock();
        return;
    }
    parent = o->parent;
    was_hold = o->parent_hold;
    o->parent_hold = 0;
    if (parent && was_hold)
        qd3d_obj_addref(parent);
    qd3d_rt_unlock();
    if (parent && was_hold)
        qd3d_obj_unhold(parent);
}

void qd3d_obj_hold(struct qd3d_obj *o)
{
    qd3d_rt_lock();
    o->holds++;
    qd3d_rt_unlock();
}

void qd3d_obj_unhold(struct qd3d_obj *o)
{
    qd3d_rt_lock();
    if (--o->holds == 0 && o->refs == 0 && !o->dying) {
        obj_destroy(o);
        return;
    }
    qd3d_rt_unlock();
}

ULONG qd3d_obj_release(struct qd3d_obj *o)
{
    if (o->owned_level && o->parent)
        return qd3d_obj_release(o->parent);
    for (;;) {
        LONG r = o->refs;
        struct qd3d_obj *parent;

        if (r <= 0) {
            qd3d_rt_log("Release of object %#x with refcount %ld", (unsigned)o->id, (long)r);
            return 0;
        }
        if (r > 1) {
            if (InterlockedCompareExchange(&o->refs, r - 1, r) == r)
                return (ULONG)(r - 1);
            continue;
        }
        /* The 1 -> 0 transition happens under the lock so that
         * qd3d_obj_from_id never resurrects a dying proxy. */
        qd3d_rt_lock();
        r = InterlockedDecrement(&o->refs);
        if (r != 0) {
            qd3d_rt_unlock();
            return (ULONG)r;
        }
        if (o->type == QD3D_T_IDirect3DDevice9) {
            /* A released device lets go of everything it keeps alive. */
            o->dying = 1;
            qd3d_dev_teardown(o);
            o->dying = 0;
        }
        if (o->holds) {
            parent = obj_go_dormant(o);
            qd3d_rt_unlock();
            if (parent)
                qd3d_obj_release(parent);
            return 0;
        }
        obj_destroy(o);
        return 0;
    }
}
