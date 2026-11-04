/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * QD3D loopback harness: runs the host executor on named shared-memory
 * channels so 32-bit Direct3D 9 applications with the qd3d proxy d3d9.dll
 * beside them render on this machine's GPU without a VM.
 *
 *   qd3d_loopback [--dump DIR] [--dump-every N] [--dump-first N] [--frames N]
 *                 [--timeout S] [--quiet] [--no-view] [--name NAME] [--slots N]
 *
 * Like the VM device it offers channel slots, one Direct3D process each
 * (qd3d_proto.h, Transports): mappings NAME (slot 0) and NAME_<i>, one
 * doorbell NAME_KICK, one executor per slot, all on this thread.
 * Manual-reset NAME_RELEASE_<i> events exercise the VM release path:
 * stop the old producer, signal its event, wait for the harness to reset
 * it (boundary captured), then give up the claim. A killed loopback owner
 * has no kernel notifier; the next HELLO still ends its previous session,
 * and a faulted session is reset once its process is gone.
 * It runs the VM device's display policy (qd3dx.h): the view shows the
 * frames it shows and goes black where the VM returns to the guest
 * desktop; with --dump every transition is logged (DIR\display.txt).
 *
 * Exit status: 0 normal (or --frames reached), 1 when --frames was not
 * reached or a fault occurred, 2 on setup errors.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../proto/qd3d_proto.h"
#include "qd3dx.h"

/* Polling window after the last executed command before going idle. */
#define HOST_SPIN_US 200

struct harness;

struct slot {
    struct harness *h;
    uint32_t index;
    HANDLE map;
    void *base;
    struct qd3dx *ex;
    HANDLE release;             /* manual-reset NAME_RELEASE_<i>, capture ack */
    int n;                      /* last qd3dx_poll result */
    int faulted;
};

struct harness {
    /* options */
    const char *dump_dir;
    uint32_t dump_every;
    uint32_t dump_first;        /* frames 1..dump_first are always dumped */
    uint32_t frames_target;     /* 0 = none */
    double timeout_s;           /* 0 = none */
    int quiet;
    int no_view;                /* no window: measures the executor alone */
    uint32_t nslots;

    /* state */
    struct slot slot[QD3D_VM_SLOTS];
    HWND wnd;
    uint8_t *view;              /* latest frame shown, top-down BGRX */
    uint32_t view_w, view_h;
    uint32_t frames;            /* presented frames published */
    uint32_t repeats;           /* repeats of published frames */
    uint32_t repeat_k;          /* of the last presented frame */
    uint32_t faults;
    uint32_t dumped;
    int dump_failed;
    struct qd3dx_display disp;
    LARGE_INTEGER freq, t0;
};

static volatile LONG g_stop;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

/* With several slots every executor line names its slot at the end (lines
 * still start with "qd3dx: " for the scripts that match them). */
static void on_log(void *user, const char *line)
{
    struct slot *s = user;

    if (s->h->quiet && !strstr(line, "FAULT"))
        return;
    if (s->h->nslots > 1)
        printf("%s [slot %u]\n", line, s->index);
    else
        printf("%s\n", line);
    fflush(stdout);
}

static int write_bmp(const char *path, const struct qd3dx_frame *f)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    uint32_t row = f->width * 4u, y;
    FILE *fp = fopen(path, "wb");

    if (!fp)
        return 0;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + row * f->height;
    ih.biSize = sizeof ih;
    ih.biWidth = (LONG)f->width;
    ih.biHeight = (LONG)f->height; /* bottom-up */
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = row * f->height;
    fwrite(&fh, sizeof fh, 1, fp);
    fwrite(&ih, sizeof ih, 1, fp);
    for (y = f->height; y-- > 0;)
        fwrite((const uint8_t *)f->pixels + (size_t)y * f->pitch, row, 1, fp);
    return fclose(fp) == 0;
}

/* Microseconds since the harness started. */
static uint64_t now_us(const struct harness *h)
{
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart - h->t0.QuadPart) * 1000000u / (uint64_t)h->freq.QuadPart;
}

/* A display policy transition: printed, with --dump appended to
 * DIR\display.txt as "<ms since start> <what>" (on slot S frame N[ repeat
 * K], off released|hidden|idle slot S, again slot S); an "off" blanks the
 * view, as the VM shows the guest desktop then. */
static void display_log(struct harness *h, int off, const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!h->quiet)
        printf("qd3d_loopback: display: %s\n", line);
    fflush(stdout);
    if (h->dump_dir) {
        char path[MAX_PATH];
        FILE *fp;

        snprintf(path, sizeof path, "%s\\display.txt", h->dump_dir);
        fp = fopen(path, "a");
        if (fp) {
            fprintf(fp, "%llu %s\n", (unsigned long long)(now_us(h) / 1000u), line);
            fclose(fp);
        }
    }
    if (off && h->wnd && h->view) {
        free(h->view);
        h->view = NULL;
        h->view_w = h->view_h = 0;
        InvalidateRect(h->wnd, NULL, FALSE);
    }
}

/* Every frame of every slot goes to the dump: a repeat of the last
 * presented frame N (struct qd3dx_frame repeat) as frame_N_cK.bmp, the
 * K-th of N (at most 64), when frame N is dumped. The view shows the frames
 * the display policy shows; turning 3D output on and switching the slot
 * shown are logged. */
static void on_frame(void *user, const struct qd3dx_frame *f)
{
    struct slot *s = user;
    struct harness *h = s->h;
    int was_on = h->disp.on;
    unsigned was_slot = h->disp.slot;
    uint32_t y;
    int r;

    if (f->repeat) {
        h->repeats++;
        h->repeat_k++;
    } else {
        h->frames++;
        h->repeat_k = 0;
    }
    if (h->dump_dir && (h->frames <= h->dump_first || (h->frames - 1u) % h->dump_every == 0u) && h->repeat_k <= 64u) {
        char path[MAX_PATH];

        if (f->repeat)
            snprintf(path, sizeof path, "%s\\frame_%06u_c%03u.bmp", h->dump_dir, h->frames, h->repeat_k);
        else
            snprintf(path, sizeof path, "%s\\frame_%06u.bmp", h->dump_dir, h->frames);
        if (write_bmp(path, f)) {
            h->dumped++;
        } else if (!h->dump_failed) {
            h->dump_failed = 1;
            printf("qd3d_loopback: cannot write %s\n", path);
        }
    }
    r = qd3dx_display_frame(&h->disp, s->index, f->repeat, now_us(h));
    if (r != QD3DX_DISPLAY_DROP && (!was_on || was_slot != s->index)) {
        if (f->repeat)
            display_log(h, 0, "on slot %u frame %u repeat %u", s->index, h->frames, h->repeat_k);
        else
            display_log(h, 0, "on slot %u frame %u", s->index, h->frames);
    }
    if (!h->wnd || r == QD3DX_DISPLAY_DROP)
        return;
    if (h->view_w != f->width || h->view_h != f->height) {
        free(h->view);
        h->view = malloc((size_t)f->width * f->height * 4u);
        h->view_w = h->view ? f->width : 0u;
        h->view_h = h->view ? f->height : 0u;
    }
    if (!h->view)
        return;
    for (y = 0; y < f->height; y++)
        memcpy(h->view + (size_t)y * f->width * 4u, (const uint8_t *)f->pixels + (size_t)y * f->pitch,
               (size_t)f->width * 4u);
    InvalidateRect(h->wnd, NULL, FALSE);
}

/* The source of a slot's last published frame was released: with --dump
 * the event is appended to DIR\source_released.txt as "slot S after frame
 * N" (N in the frame dumps' numbering), so a program can check what
 * reached the sink before it. */
static void on_source_released(void *user)
{
    struct slot *s = user;
    struct harness *h = s->h;

    if (!h->quiet)
        printf("qd3d_loopback: slot %u: frame source released after frame %u\n", s->index, h->frames);
    fflush(stdout);
    if (h->dump_dir) {
        char path[MAX_PATH];
        FILE *fp;

        snprintf(path, sizeof path, "%s\\source_released.txt", h->dump_dir);
        fp = fopen(path, "a");
        if (fp) {
            fprintf(fp, "slot %u after frame %u\n", s->index, h->frames);
            fclose(fp);
        }
    }
    if (qd3dx_display_gone(&h->disp, s->index))
        display_log(h, 1, "off released slot %u", s->index);
}

/* The window of the source of a slot's last published frame was minimized
 * or hidden. */
static void on_source_hidden(void *user)
{
    struct slot *s = user;
    struct harness *h = s->h;

    if (!h->quiet)
        printf("qd3d_loopback: slot %u: frame source hidden after frame %u\n", s->index, h->frames);
    fflush(stdout);
    if (qd3dx_display_gone(&h->disp, s->index))
        display_log(h, 1, "off hidden slot %u", s->index);
}

static LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct harness *h = (struct harness *)GetWindowLongPtrA(wnd, GWLP_USERDATA);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(wnd, &ps);

        GetClientRect(wnd, &rc);
        if (h && h->view) {
            BITMAPINFO bi;

            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = (LONG)h->view_w;
            bi.bmiHeader.biHeight = -(LONG)h->view_h; /* top-down */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, (int)h->view_w, (int)h->view_h,
                          h->view, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        InterlockedExchange(&g_stop, 1);
        return 0;
    default:
        return DefWindowProcA(wnd, msg, wp, lp);
    }
}

static HWND create_view(struct harness *h)
{
    WNDCLASSA wc;
    HWND wnd;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "QD3DLoopbackView";
    if (!RegisterClassA(&wc))
        return NULL;
    wnd = CreateWindowExA(0, wc.lpszClassName, "qd3d loopback", WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);
    if (!wnd)
        return NULL;
    SetWindowLongPtrA(wnd, GWLP_USERDATA, (LONG_PTR)h);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    return wnd;
}

static void usage(void)
{
    printf("usage: qd3d_loopback [--dump DIR] [--dump-every N] [--dump-first N] [--frames N] [--timeout S]\n"
           "                     [--quiet] [--no-view] [--name NAME] [--slots N]\n"
           "  --dump DIR      write presented frames as BMP into DIR (frame_NNNNNN.bmp, counted\n"
           "                  from 1 over every slot; repeats of frame N, for the cursor\n"
           "                  between presents or a window shown again, frame_NNNNNN_cKKK.bmp)\n"
           "  --dump-every N  dump every Nth presented frame (default 1)\n"
           "  --dump-first N  also dump each of the first N frames\n"
           "                  (with --dump, released frame sources are logged in\n"
           "                  DIR\\source_released.txt, display transitions in\n"
           "                  DIR\\display.txt)\n"
           "  --frames N      exit with status 0 after N presented frames\n"
           "  --timeout S     exit after S seconds\n"
           "  --quiet         print only faults and the summary\n"
           "  --no-view       no window showing the frames (executor cost alone)\n"
           "  --name NAME     channel NAME and doorbell NAME_KICK instead of\n"
           "                  " QD3D_LOOPBACK_MAPPING " (guests: QD3D_LOOPBACK_NAME)\n"
           "  --slots N       N channel slots, one Direct3D process each (1-%u,\n"
           "                  default 1): channels NAME, NAME_1, ...\n", QD3D_VM_SLOTS);
}

static int parse_u32(const char *s, uint32_t *v)
{
    char *end;
    unsigned long n = strtoul(s, &end, 10);

    if (!*s || *end || n > 0xffffffffUL)
        return 0;
    *v = (uint32_t)n;
    return 1;
}

/* 1 while process pid runs. */
static int process_alive(uint32_t pid)
{
    HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    int alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;

    if (p)
        CloseHandle(p);
    return alive;
}

/* The manual-reset event is the loopback equivalent of a synchronous MMIO
 * write: resetting it acknowledges boundary capture before the producer
 * permits a new claim. Never reset before qd3dx_request_release. */
static int slot_release(struct slot *s)
{
    if (WaitForSingleObject(s->release, 0) == WAIT_OBJECT_0) {
        qd3dx_request_release(s->ex);
        ResetEvent(s->release);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct harness h;
    HANDLE kick;
    HANDLE waits[1 + QD3D_VM_SLOTS];
    LARGE_INTEGER now, last_watch, last_work;
    double elapsed;
    int i, rc;
    uint32_t k;
    char name[200] = QD3D_LOOPBACK_MAPPING, kick_name[sizeof name + 8], slot_name[sizeof name + 16];

    memset(&h, 0, sizeof h);
    h.dump_every = 1;
    h.nslots = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            h.dump_dir = argv[++i];
        } else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &h.dump_every) || h.dump_every == 0u) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--dump-first") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &h.dump_first)) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &h.frames_target) || h.frames_target == 0u) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            char *end;

            h.timeout_s = strtod(argv[++i], &end);
            if (*end || h.timeout_s <= 0.0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--quiet")) {
            h.quiet = 1;
        } else if (!strcmp(argv[i], "--no-view")) {
            h.no_view = 1;
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc && argv[i + 1][0] &&
                   strlen(argv[i + 1]) + sizeof "_KICK" <= sizeof name) {
            strcpy(name, argv[++i]);
        } else if (!strcmp(argv[i], "--slots") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &h.nslots) || h.nslots == 0u || h.nslots > QD3D_VM_SLOTS) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (h.dump_dir && !CreateDirectoryA(h.dump_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        printf("qd3d_loopback: cannot create %s (error %lu)\n", h.dump_dir, GetLastError());
        return 2;
    }

    /* Every channel before the doorbell: guests take a missing doorbell for
     * "no harness". */
    for (k = 0; k < h.nslots; k++) {
        struct slot *s = &h.slot[k];
        struct qd3dx_hooks hooks;

        if (k)
            snprintf(slot_name, sizeof slot_name, "%s_%u", name, k);
        else
            snprintf(slot_name, sizeof slot_name, "%s", name);
        s->h = &h;
        s->index = k;
        s->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, QD3D_DEFAULT_SIZE, slot_name);
        if (!s->map) {
            printf("qd3d_loopback: CreateFileMapping %s failed (error %lu)\n", slot_name, GetLastError());
            return 2;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            printf("qd3d_loopback: %s already exists (another harness running?)\n", slot_name);
            return 2;
        }
        s->base = MapViewOfFile(s->map, FILE_MAP_ALL_ACCESS, 0, 0, QD3D_DEFAULT_SIZE);
        if (!s->base) {
            printf("qd3d_loopback: cannot map %s (error %lu)\n", slot_name, GetLastError());
            return 2;
        }
        memset(&hooks, 0, sizeof hooks);
        hooks.user = s;
        hooks.frame = on_frame;
        hooks.log = on_log;
        hooks.source_released = on_source_released;
        hooks.source_hidden = on_source_hidden;
        s->ex = qd3dx_create(s->base, QD3D_DEFAULT_SIZE, &hooks);
        if (!s->ex) {
            printf("qd3d_loopback: qd3dx_create failed\n");
            return 2;
        }
        snprintf(slot_name, sizeof slot_name, "%s_RELEASE_%u", name, k);
        s->release = CreateEventA(NULL, TRUE, FALSE, slot_name);
        if (!s->release || GetLastError() == ERROR_ALREADY_EXISTS) {
            printf("qd3d_loopback: cannot create %s (error %lu)\n", slot_name, GetLastError());
            return 2;
        }
        waits[k + 1] = s->release;
    }
    snprintf(kick_name, sizeof kick_name, "%s_KICK", name);
    kick = CreateEventA(NULL, FALSE, FALSE, kick_name);
    if (!kick || GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("qd3d_loopback: cannot create %s (error %lu)\n", kick_name, GetLastError());
        return 2;
    }
    waits[0] = kick;
    if (!h.no_view)
        h.wnd = create_view(&h);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (!h.quiet && h.nslots == 1u)
        printf("qd3d_loopback: channel %s (%u bytes), doorbell %s; waiting for a guest\n",
               name, QD3D_DEFAULT_SIZE, kick_name);
    else if (!h.quiet)
        printf("qd3d_loopback: %u slots, channels %s to %s_%u (%u bytes each), doorbell %s; waiting for a guest\n",
               h.nslots, name, name, h.nslots - 1u, QD3D_DEFAULT_SIZE, kick_name);
    fflush(stdout);

    qd3dx_display_init(&h.disp);
    QueryPerformanceFrequency(&h.freq);
    QueryPerformanceCounter(&h.t0);
    last_watch = h.t0;
    last_work = h.t0;
    while (!g_stop) {
        MSG msg;
        int work = 0, idle = 0, busy = 0;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        for (k = 0; k < h.nslots; k++) {
            struct slot *s = &h.slot[k];

            slot_release(s);
            s->n = qd3dx_poll(s->ex, 256);
            if (s->n < 0 && !s->faulted) {
                s->faulted = 1; /* the log hook printed the fault */
                h.faults++;
            }
            if (s->n >= 0)
                s->faulted = 0;
            work |= s->n > 0;
            idle |= s->n == 0;
            /* The display policy: a slot whose frames the desktop replaced
             * for silence shows them again when it lives after all. */
            if (qd3dx_alive(s->ex) && qd3dx_display_alive(&h.disp, k, now_us(&h))) {
                display_log(&h, 0, "again slot %u", k);
                qd3dx_show_again(s->ex);
            }
        }
        if (qd3dx_display_idle(&h.disp, now_us(&h)))
            display_log(&h, 1, "off idle slot %u", h.disp.slot);
        if (h.frames_target && h.frames >= h.frames_target)
            break;
        QueryPerformanceCounter(&now);
        if (work)
            last_work = now;
        elapsed = (double)(now.QuadPart - h.t0.QuadPart) / (double)h.freq.QuadPart;
        if (h.timeout_s > 0.0 && elapsed >= h.timeout_s)
            break;

        /* Loopback has no kernel cleanup notifier on process exit. Recover
         * a faulted dead owner here; explicit release events above use the
         * VM's immediate path, and the next HELLO ends other leftovers. */
        if ((double)(now.QuadPart - last_watch.QuadPart) / (double)h.freq.QuadPart >= 0.25) {
            last_watch = now;
            for (k = 0; k < h.nslots; k++) {
                struct slot *s = &h.slot[k];
                uint32_t pid = qd3dx_session_pid(s->ex);

                if (!s->faulted || process_alive(pid))
                    continue;
                printf("qd3d_loopback: slot %u: guest pid %u is gone, resetting its faulted session\n", k, pid);
                fflush(stdout);
                qd3dx_reset(s->ex);
                s->faulted = 0;
            }
        }

        if (work)
            continue;
        /* Keep polling for a moment after the last command: while a guest
         * is rendering it then never sees host_idle and never pays for a
         * doorbell (an event signal per command when the host drains
         * faster than the guest produces). */
        if (idle && (now.QuadPart - last_work.QuadPart) * 1000000 < (LONGLONG)HOST_SPIN_US * h.freq.QuadPart) {
            YieldProcessor();
            continue;
        }
        /* Sleep only when no slot has work; kicks and releases both wake. */
        for (k = 0; k < h.nslots; k++) {
            busy |= slot_release(&h.slot[k]);
            if (h.slot[k].n == 0 && qd3dx_prepare_wait(h.slot[k].ex))
                busy = 1;
        }
        if (!busy)
            MsgWaitForMultipleObjects(h.nslots + 1, waits, FALSE, 2, QS_ALLINPUT);
        for (k = 0; k < h.nslots; k++)
            if (h.slot[k].n == 0)
                qd3dx_wake(h.slot[k].ex);
    }

    QueryPerformanceCounter(&now);
    elapsed = (double)(now.QuadPart - h.t0.QuadPart) / (double)h.freq.QuadPart;
    printf("qd3d_loopback: frames=%u repeats=%u faults=%u dumped=%u elapsed=%.2fs fps=%.1f\n", h.frames,
           h.repeats, h.faults, h.dumped, elapsed, elapsed > 0.0 ? h.frames / elapsed : 0.0);
    fflush(stdout);

    rc = 0;
    if (h.faults || (h.frames_target && h.frames < h.frames_target))
        rc = 1;
    for (k = 0; k < h.nslots; k++)
        qd3dx_destroy(h.slot[k].ex);
    if (h.wnd)
        DestroyWindow(h.wnd);
    free(h.view);
    for (k = 0; k < h.nslots; k++) {
        UnmapViewOfFile(h.slot[k].base);
        CloseHandle(h.slot[k].release);
        CloseHandle(h.slot[k].map);
    }
    CloseHandle(kick);
    return rc;
}
