/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: QDDRAW_TRACE=1 statistics, written to the log (QDDRAW_LOG) once per
 * second: every DirectDraw method called in that second (calls, total and
 * longest time, the first failures), transfers between the system-memory
 * and Direct3D 9 copies, presents by the method that caused them, vertical
 * blank waits and time spent waiting for the front end's lock.
 *
 * Methods are timed without touching their code: with tracing on, the first
 * object of each interface points every slot of its vtable (writable, see
 * DllMain) at a generated thunk "push slot; jmp qdd_trace_stub".  The stub
 * records the entry on a per-thread stack, replaces the return address and
 * jumps to the original method; the method returns into the stub, which
 * records the time and HRESULT and returns to the caller.  With tracing off
 * nothing is patched; the counters cost a predictable branch.
 */
#include "qdd_priv.h"
#include <stdio.h>

bool g_qdd_trace;

/* ------------------------------------------------------------------------ */
/* Method names per interface (vtable order, IUnknown included)              */

static const char *const g_unk[] = {"QueryInterface", "AddRef", "Release"};
static const char *const g_draw[] = {
    "Compact", "CreateClipper", "CreatePalette", "CreateSurface", "DuplicateSurface", "EnumDisplayModes",
    "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface",
    "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem",                                                                   /* 2 */
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier", /* 4 */
    "StartModeTest", "EvaluateMode",                                                        /* 7 */
};
static const char *const g_surf[] = {
    "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface",
    "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps",
    "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette", "GetPixelFormat",
    "GetSurfaceDesc", "Initialize", "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey",
    "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
    "GetDDInterface", "PageLock", "PageUnlock",                                                      /* 2 */
    "SetSurfaceDesc",                                                                                /* 3 */
    "SetPrivateData", "GetPrivateData", "FreePrivateData", "GetUniquenessValue", "ChangeUniquenessValue", /* 4 */
    "SetPriority", "GetPriority", "SetLOD", "GetLOD",                                                 /* 7 */
};
static const char *const g_pal[] = {"GetCaps", "GetEntries", "Initialize", "SetEntries"};
static const char *const g_clip[] = {"GetClipList", "GetHWnd", "Initialize", "IsClipListChanged", "SetClipList",
                                     "SetHWnd"};
static const char *const g_gamma[] = {"GetGammaRamp", "SetGammaRamp"};

/* Methods after IUnknown in interface kind/version. */
static UINT qdd_trace_methods(int kind, int v, const char *const **names, const char **iface)
{
    switch (kind) {
    case QDD_TK_DRAW:
        *names = g_draw;
        *iface = v == 1 ? "DDraw1" : v == 2 ? "DDraw2" : v == 4 ? "DDraw4" : "DDraw7";
        return v == 1 ? 20 : v == 2 ? 21 : v == 4 ? 25 : 27;
    case QDD_TK_SURF:
        *names = g_surf;
        *iface = v == 1 ? "Surf1" : v == 2 ? "Surf2" : v == 3 ? "Surf3" : v == 4 ? "Surf4" : "Surf7";
        return v == 1 ? 33 : v == 2 ? 36 : v == 3 ? 37 : v == 4 ? 42 : 46;
    case QDD_TK_PAL:
        *names = g_pal;
        *iface = "Palette";
        return 4;
    case QDD_TK_CLIP:
        *names = g_clip;
        *iface = "Clipper";
        return 6;
    default:
        *names = g_gamma;
        *iface = "Gamma";
        return 2;
    }
}

/* ------------------------------------------------------------------------ */
/* State                                                                     */

#define QDD_TRACE_SLOTS 512
#define QDD_TRACE_DEPTH 128

struct QddTraceSlot {
    const char *iface, *name;
    void *orig;
    bool hresult;
    LONG calls;
    LONGLONG ticks, max;
    LONG presents;
    LONGLONG present_ticks;
    LONG fails;
};

struct QddTraceFrame {
    DWORD id;
    void *ret;
    LONGLONG t0;
};

struct QddTraceThread {
    UINT depth;
    bool presenter;
    QddTraceFrame f[QDD_TRACE_DEPTH];
};

static QddTraceSlot g_slots[QDD_TRACE_SLOTS];
static UINT g_nslots;
static BYTE *g_thunks;
static void **g_patched[32];
static UINT g_npatched;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static CRITICAL_SECTION g_tcs;
static LARGE_INTEGER g_freq;
static LONGLONG g_t_start, g_t_report;

static struct {
    LONG xfer[4];
    LONGLONG xfer_bytes[4], xfer_ticks[4];
    LONG presents_presenter, presents_other;
    LONGLONG present_ticks_presenter, present_ticks_other;
    LONG vblank_app, vblank_presenter;
    LONGLONG vblank_app_ticks, vblank_presenter_ticks;
    LONG lock_waits;
    LONGLONG lock_wait_ticks, lock_wait_max;
} g_st;

LONGLONG qdd_trace_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double qdd_ms(LONGLONG ticks)
{
    return (double)ticks * 1000.0 / (double)g_freq.QuadPart;
}

static QddTraceThread *qdd_trace_thread(void)
{
    QddTraceThread *t = (QddTraceThread *)TlsGetValue(g_tls);
    if (!t) {
        t = (QddTraceThread *)calloc(1, sizeof(*t));
        TlsSetValue(g_tls, t);
    }
    return t;
}

/* ------------------------------------------------------------------------ */
/* The stub                                                                  */

extern "C" void *qdd_trace_enter(DWORD id, void *ret);
extern "C" void *qdd_trace_leave(DWORD result);
extern "C" void qdd_trace_stub(void);

/* [esp] = slot (pushed by the thunk), [esp+4] = caller's return address,
 * then the method's arguments.  COM methods are stdcall with no register
 * arguments, so eax/ecx/edx are free on entry. */
__asm__(".text\n"
        ".globl _qdd_trace_stub\n"
        "_qdd_trace_stub:\n"
        "    movl (%esp), %eax\n"
        "    movl 4(%esp), %ecx\n"
        "    pushl %ecx\n"
        "    pushl %eax\n"
        "    call _qdd_trace_enter\n"
        "    addl $12, %esp\n"
        "    movl $_qdd_trace_back, (%esp)\n"
        "    jmp *%eax\n"
        "_qdd_trace_back:\n"
        "    pushl %eax\n"
        "    pushl %eax\n"
        "    call _qdd_trace_leave\n"
        "    addl $4, %esp\n"
        "    movl %eax, %ecx\n"
        "    popl %eax\n"
        "    jmp *%ecx\n");

extern "C" void *qdd_trace_enter(DWORD id, void *ret)
{
    DWORD err = GetLastError();
    QddTraceThread *t = qdd_trace_thread();
    QddTraceFrame *f = &t->f[t->depth < QDD_TRACE_DEPTH ? t->depth : QDD_TRACE_DEPTH - 1];
    ++t->depth;
    f->id = id;
    f->ret = ret;
    f->t0 = qdd_trace_now();
    SetLastError(err);
    return g_slots[id].orig;
}

static void qdd_trace_report(LONGLONG now);

extern "C" void *qdd_trace_leave(DWORD result)
{
    DWORD err = GetLastError();
    LONGLONG now = qdd_trace_now();
    QddTraceThread *t = (QddTraceThread *)TlsGetValue(g_tls);
    --t->depth;
    QddTraceFrame *f = &t->f[t->depth < QDD_TRACE_DEPTH ? t->depth : QDD_TRACE_DEPTH - 1];
    void *ret = f->ret;
    QddTraceSlot *s = &g_slots[f->id];
    LONGLONG dt = now - f->t0;
    EnterCriticalSection(&g_tcs);
    ++s->calls;
    s->ticks += dt;
    if (dt > s->max)
        s->max = dt;
    if (s->hresult && FAILED((HRESULT)result) && s->fails < 3) {
        ++s->fails;
        qdd_log("qddraw trace: %s.%s failed 0x%08lx", s->iface, s->name, (unsigned long)result);
    }
    if (now - g_t_report >= g_freq.QuadPart)
        qdd_trace_report(now);
    LeaveCriticalSection(&g_tcs);
    SetLastError(err);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */

void qdd_trace_init(void)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA("QDDRAW_TRACE", v, sizeof(v));
    if (!(n > 0 && n < sizeof(v) && v[0] != '0'))
        return;
    g_tls = TlsAlloc();
    g_thunks = (BYTE *)VirtualAlloc(NULL, QDD_TRACE_SLOTS * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_tls == TLS_OUT_OF_INDEXES || !g_thunks)
        return;
    InitializeCriticalSection(&g_tcs);
    QueryPerformanceFrequency(&g_freq);
    g_t_start = g_t_report = qdd_trace_now();
    g_qdd_trace = true;
    qdd_log("qddraw trace: on (QDDRAW_TRACE)");
}

void qdd_trace_vtbl(void *iface, int kind, int version)
{
    if (!g_qdd_trace || !iface)
        return;
    void **vt = *(void ***)iface;
    EnterCriticalSection(&g_tcs);
    bool done = false;
    for (UINT i = 0; i < g_npatched; ++i)
        done |= g_patched[i] == vt;
    const char *const *names;
    const char *name;
    UINT n = qdd_trace_methods(kind, version, &names, &name) + 3;
    if (!done && g_npatched < 32 && g_nslots + n <= QDD_TRACE_SLOTS) {
        g_patched[g_npatched++] = vt;
        for (UINT i = 0; i < n; ++i) {
            UINT id = g_nslots++;
            QddTraceSlot *s = &g_slots[id];
            s->iface = name;
            s->name = i < 3 ? g_unk[i] : names[i - 3];
            s->orig = vt[i];
            s->hresult = i != 1 && i != 2;
            BYTE *th = g_thunks + id * 10;
            th[0] = 0x68; /* push imm32 */
            *(DWORD *)(th + 1) = id;
            th[5] = 0xe9; /* jmp rel32 */
            *(LONG *)(th + 6) = (LONG)((BYTE *)(void *)qdd_trace_stub - (th + 10));
            vt[i] = th;
        }
        FlushInstructionCache(GetCurrentProcess(), g_thunks, QDD_TRACE_SLOTS * 10);
    }
    LeaveCriticalSection(&g_tcs);
}

/* ------------------------------------------------------------------------ */
/* Counters                                                                  */

void qdd_trace_xfer(int kind, DWORD bytes, LONGLONG t0)
{
    LONGLONG dt = qdd_trace_now() - t0;
    EnterCriticalSection(&g_tcs);
    ++g_st.xfer[kind];
    g_st.xfer_bytes[kind] += bytes;
    g_st.xfer_ticks[kind] += dt;
    LeaveCriticalSection(&g_tcs);
}

void qdd_trace_mark_presenter(void)
{
    if (g_qdd_trace)
        qdd_trace_thread()->presenter = true;
}

void qdd_trace_present(LONGLONG t0)
{
    LONGLONG dt = qdd_trace_now() - t0;
    QddTraceThread *t = qdd_trace_thread();
    EnterCriticalSection(&g_tcs);
    if (t->depth) {
        QddTraceSlot *s = &g_slots[t->f[0].id]; /* the application's call */
        ++s->presents;
        s->present_ticks += dt;
    } else if (t->presenter) {
        ++g_st.presents_presenter;
        g_st.present_ticks_presenter += dt;
    } else {
        ++g_st.presents_other;
        g_st.present_ticks_other += dt;
    }
    LeaveCriticalSection(&g_tcs);
}

void qdd_trace_vblank(LONGLONG t0)
{
    LONGLONG dt = qdd_trace_now() - t0;
    QddTraceThread *t = qdd_trace_thread();
    EnterCriticalSection(&g_tcs);
    if (t->presenter) {
        ++g_st.vblank_presenter;
        g_st.vblank_presenter_ticks += dt;
    } else {
        ++g_st.vblank_app;
        g_st.vblank_app_ticks += dt;
    }
    LeaveCriticalSection(&g_tcs);
}

void qdd_trace_lock(CRITICAL_SECTION *cs)
{
    if (TryEnterCriticalSection(cs))
        return;
    LONGLONG t0 = qdd_trace_now();
    EnterCriticalSection(cs);
    LONGLONG dt = qdd_trace_now() - t0;
    EnterCriticalSection(&g_tcs);
    ++g_st.lock_waits;
    g_st.lock_wait_ticks += dt;
    if (dt > g_st.lock_wait_max)
        g_st.lock_wait_max = dt;
    LeaveCriticalSection(&g_tcs);
}

/* Called with g_tcs held, once a second. */
static void qdd_trace_report(LONGLONG now)
{
    double t = qdd_ms(now - g_t_start) / 1000.0;
    char line[480];
    int n = 0;
    for (UINT i = 0; i < g_nslots; ++i) {
        QddTraceSlot *s = &g_slots[i];
        if (!s->calls && !s->presents)
            continue;
        char item[160];
        int k = _snprintf(item, sizeof(item), " %s.%s %ld %.1fms max %.2f", s->iface, s->name, s->calls,
                          qdd_ms(s->ticks), qdd_ms(s->max));
        if (k > 0 && s->presents)
            k += _snprintf(item + k, sizeof(item) - k, " presents %ld %.1fms", s->presents, qdd_ms(s->present_ticks));
        if (k < 0 || k >= (int)sizeof(item))
            k = (int)sizeof(item) - 1;
        item[k] = 0;
        if (n + k >= (int)sizeof(line) - 1) {
            qdd_log("qddraw trace %.1fs calls:%s", t, line);
            n = 0;
        }
        memcpy(line + n, item, (size_t)k + 1);
        n += k;
        s->calls = s->presents = 0;
        s->ticks = s->max = s->present_ticks = 0;
    }
    if (n)
        qdd_log("qddraw trace %.1fs calls:%s", t, line);
    static const char *const xn[4] = {"upload", "readback", "depth-upload", "depth-readback"};
    n = _snprintf(line, sizeof(line), "qddraw trace %.1fs:", t);
    for (int i = 0; i < 4; ++i)
        if (g_st.xfer[i] && n > 0 && n < (int)sizeof(line))
            n += _snprintf(line + n, sizeof(line) - n, " %s %ld %.0fKiB %.1fms", xn[i], g_st.xfer[i],
                           (double)g_st.xfer_bytes[i] / 1024.0, qdd_ms(g_st.xfer_ticks[i]));
    if (n > 0 && n < (int)sizeof(line))
        _snprintf(line + n, sizeof(line) - n,
                  " presenter-presents %ld %.1fms other-presents %ld %.1fms vblank-waits app %ld %.1fms presenter %ld"
                  " %.1fms lock-waits %ld %.2fms max %.2f",
                  g_st.presents_presenter, qdd_ms(g_st.present_ticks_presenter), g_st.presents_other,
                  qdd_ms(g_st.present_ticks_other), g_st.vblank_app, qdd_ms(g_st.vblank_app_ticks),
                  g_st.vblank_presenter, qdd_ms(g_st.vblank_presenter_ticks), g_st.lock_waits,
                  qdd_ms(g_st.lock_wait_ticks), qdd_ms(g_st.lock_wait_max));
    line[sizeof(line) - 1] = 0;
    qdd_log("%s", line);
    memset(&g_st, 0, sizeof(g_st));
    g_t_report = now;
}
