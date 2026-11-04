/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d9_multiproc - multi-process oracle for QD3D channel slots.
 *
 * Every Direct3D 9 process gets a channel slot of its own, and a process
 * that finds every slot busy uses the stock runtime (qd3d_proto.h,
 * Transports). Run it through run-loopback.ps1 (QD3D_TRANSPORT=loopback,
 * %TEMP% in the run directory, so the proxy's qd3d.log lands there);
 * run-multiproc.ps1 runs both modes and checks the executor's side:
 *
 *   --slots 2  launcher: this process holds an IDirect3D9 without a device
 *              (as a game launcher's hardware detection does) while a child
 *              connects, renders and checks its pixels; then both render
 *              at once. crashed owner: a child creates a device, a texture
 *              and a vertex buffer and terminates itself while holding its
 *              slot; the next child must get that slot and render.
 *   --slots 1  busy: while this process holds the only slot, a child gets
 *              the stock runtime and %TEMP%\qd3d.log says why (as it
 *              records a failed CreateDevice's HRESULT). unload:
 *              FreeLibrary of the proxy gives the slot back at once (a
 *              child connects), and loaded again it connects again.
 *
 * Children are this executable with --child ROLE (internal). Every pixel
 * check is exact. Output: "INFO ..." lines, one "PASS <scenario>" or
 * "FAIL <scenario>: <first failure>" line per scenario of every process,
 * and the parent's final RESULT line. Exit code: 0 all scenarios passed,
 * 1 one failed, 2 setup failed, 3 watchdog timeout.
 */
#include <windows.h>
#include <d3d9.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../proto/qd3d_proto.h"

#define BB 64
#define PARENT_WATCHDOG_MS 100000u
#define CHILD_WATCHDOG_MS 60000u
#define CHILD_WAIT_MS 45000u
#define CLAIM_WAIT_MS 5000u
#define CONCURRENT_ROUNDS 4
/* Exit code of the child that terminates itself while holding its slot. */
#define CRASH_EXIT 42

/* Colours per role: whoever reads another process's pixels fails. */
#define C_PARENT_A D3DCOLOR_XRGB(0x20, 0x30, 0x40)
#define C_PARENT_B D3DCOLOR_XRGB(0xc0, 0xa0, 0x80)
#define C_RENDER_A D3DCOLOR_XRGB(0x10, 0x40, 0x80)
#define C_RENDER_B D3DCOLOR_XRGB(0x80, 0xf0, 0x40)
#define C_CRASH_A D3DCOLOR_XRGB(0x40, 0x10, 0x10)
#define C_CRASH_B D3DCOLOR_XRGB(0x10, 0xf0, 0xf0)

struct scene {
	const char *name;
	unsigned failures;
	char first[512];
};

struct vtx {
	float x, y, z, rhw;
	D3DCOLOR color;
};
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static HWND g_hwnd;
/* d3d9.dll (the proxy beside the executable), loaded at run time as game
 * launchers often do, so the unload scenario can unload it. */
static HMODULE g_d3d9;
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static char g_exe[MAX_PATH];
static const char *g_base = g_exe; /* file name part of g_exe */
static HANDLE g_finished;
static DWORD g_watchdog_ms;
static char *g_log; /* %TEMP%\qd3d.log, see log_load */

static void fail(struct scene *s, const char *fmt, ...)
{
	va_list ap;

	if (s->failures++ != 0)
		return;
	va_start(ap, fmt);
	vsnprintf(s->first, sizeof(s->first), fmt, ap);
	va_end(ap);
}

/* Every Direct3D call here must return exactly D3D_OK. */
#define TRY(s, call)                                                      \
	do {                                                              \
		HRESULT try_hr_ = (call);                                 \
		if (try_hr_ != D3D_OK) {                                  \
			fail((s), "%s returned 0x%08lx (line %d)", #call, \
			     (unsigned long)try_hr_, __LINE__);           \
			goto out;                                         \
		}                                                         \
	} while (0)

static int report(const struct scene *s)
{
	if (s->failures == 0)
		printf("PASS %s\n", s->name);
	else if (s->failures == 1)
		printf("FAIL %s: %s\n", s->name, s->first);
	else
		printf("FAIL %s: %s (+%u more)\n", s->name, s->first,
		       s->failures - 1);
	return s->failures == 0;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
	if (WaitForSingleObject(g_finished, g_watchdog_ms) == WAIT_TIMEOUT) {
		printf("FAIL watchdog: pid %lu not finished within %lu ms\n",
		       (unsigned long)GetCurrentProcessId(),
		       (unsigned long)g_watchdog_ms);
		fflush(stdout);
		/* Skip DLL detach: a hung proxy may hold its channel lock. */
		TerminateProcess(GetCurrentProcess(), 3);
	}
	return 0;
}

static void pump_messages(void)
{
	MSG msg;

	while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}
}

/* ------------------------------------------------------------------ */
/* Direct3D                                                            */

static int make_window(void)
{
	static const char cls[] = "qd3d9_multiproc";
	WNDCLASSA wc;
	RECT rc = { 0, 0, BB, BB };
	DWORD style = WS_OVERLAPPED | WS_CAPTION;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	wc.lpszClassName = cls;
	if (!RegisterClassA(&wc))
		return 0;
	AdjustWindowRect(&rc, style, FALSE);
	g_hwnd = CreateWindowA(cls, "qd3d9_multiproc", style, CW_USEDEFAULT,
			       CW_USEDEFAULT, rc.right - rc.left,
			       rc.bottom - rc.top, NULL, NULL, wc.hInstance,
			       NULL);
	if (g_hwnd == NULL)
		return 0;
	ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
	pump_messages();
	return 1;
}

/* Direct3DCreate9 of d3d9.dll, loaded on first use (the application
 * directory comes first in the search: the proxy). */
static IDirect3D9 *d3d_create(void)
{
	typedef IDirect3D9 *(WINAPI * create9_fn)(UINT);
	create9_fn fn = NULL;

	if (!g_d3d9)
		g_d3d9 = LoadLibraryA("d3d9.dll");
	if (g_d3d9)
		fn = (create9_fn)(void (*)(void))GetProcAddress(
			g_d3d9, "Direct3DCreate9");
	return fn ? fn(D3D_SDK_VERSION) : NULL;
}

/* 1 when d3d answers IID_QD3D_REMOTE: a QD3D session, not the stock
 * runtime. */
static int is_qd3d(IDirect3D9 *d3d)
{
	void *q = NULL;

	if (FAILED(IDirect3D9_QueryInterface(d3d, &IID_QD3D_REMOTE, &q)) ||
	    q == NULL)
		return 0;
	IDirect3D9_Release((IDirect3D9 *)q);
	return 1;
}

/* Module implementing an interface (its vtable), or NULL. */
static HMODULE module_of(void *iface)
{
	HMODULE m = NULL;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR) * (void **)iface, &m))
		return NULL;
	return m;
}

static void print_module(const char *what, void *iface)
{
	char path[MAX_PATH];
	HMODULE m = iface ? module_of(iface) : NULL;

	if (m && GetModuleFileNameA(m, path, sizeof(path)))
		printf("INFO %s: %s\n", what, path);
}

static HRESULT create_device(void)
{
	static const DWORD vp[2] = { D3DCREATE_HARDWARE_VERTEXPROCESSING,
				     D3DCREATE_SOFTWARE_VERTEXPROCESSING };
	D3DPRESENT_PARAMETERS pp;
	HRESULT hr = D3DERR_NOTAVAILABLE;
	int i;

	for (i = 0; i < 2 && hr != D3D_OK; i++) {
		memset(&pp, 0, sizeof(pp));
		pp.BackBufferWidth = BB;
		pp.BackBufferHeight = BB;
		pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.BackBufferCount = 1;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = g_hwnd;
		pp.Windowed = TRUE;
		pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
		hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
					     D3DDEVTYPE_HAL, g_hwnd, vp[i], &pp,
					     &g_dev);
	}
	if (hr != D3D_OK)
		g_dev = NULL;
	return hr;
}

static void release_d3d(void)
{
	if (g_dev) {
		IDirect3DDevice9_Release(g_dev);
		g_dev = NULL;
	}
	if (g_d3d) {
		IDirect3D9_Release(g_d3d);
		g_d3d = NULL;
	}
}

static void release_all(void)
{
	release_d3d();
	if (g_hwnd) {
		DestroyWindow(g_hwnd);
		g_hwnd = NULL;
	}
	pump_messages();
}

static int expect_px(struct scene *s, const D3DLOCKED_RECT *lr, int x, int y,
		     D3DCOLOR want)
{
	DWORD got = ((const DWORD *)((const BYTE *)lr->pBits +
				     (size_t)y * (size_t)lr->Pitch))[x];

	if ((got & 0x00ffffffu) == (want & 0x00ffffffu))
		return 1;
	fail(s, "pid %lu: expected %06lx got %06lx at %d,%d",
	     (unsigned long)GetCurrentProcessId(),
	     (unsigned long)(want & 0x00ffffffu),
	     (unsigned long)(got & 0x00ffffffu), x, y);
	return 0;
}

/* Clears the back buffer to bg, draws an fg square over [16,48)^2, reads
 * the render target back and checks it exactly inside and outside the
 * square, then presents. */
static int render_check(struct scene *s, D3DCOLOR bg, D3DCOLOR fg)
{
	const struct vtx v[4] = {
		{ 16.0f, 16.0f, 0.5f, 1.0f, fg },
		{ 48.0f, 16.0f, 0.5f, 1.0f, fg },
		{ 16.0f, 48.0f, 0.5f, 1.0f, fg },
		{ 48.0f, 48.0f, 0.5f, 1.0f, fg },
	};
	IDirect3DSurface9 *rt = NULL, *sys = NULL;
	D3DLOCKED_RECT lr;
	unsigned before = s->failures;

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, bg,
				      1.0f, 0));
	TRY(s, IDirect3DDevice9_SetFVF(g_dev, FVF));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE,
					       D3DCULL_NONE));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
					       D3DZB_FALSE));
	TRY(s, IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP,
						     D3DTOP_SELECTARG1));
	TRY(s, IDirect3DDevice9_SetTextureStageState(
		       g_dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
	TRY(s, IDirect3DDevice9_BeginScene(g_dev));
	TRY(s, IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2,
						v, sizeof(v[0])));
	TRY(s, IDirect3DDevice9_EndScene(g_dev));
	TRY(s, IDirect3DDevice9_GetRenderTarget(g_dev, 0, &rt));
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(
		       g_dev, BB, BB, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys,
		       NULL));
	TRY(s, IDirect3DDevice9_GetRenderTargetData(g_dev, rt, sys));
	TRY(s, IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
	if (expect_px(s, &lr, 4, 4, bg) && expect_px(s, &lr, 32, 32, fg))
		expect_px(s, &lr, 60, 60, bg);
	IDirect3DSurface9_UnlockRect(sys);
	TRY(s, IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL));
out:
	if (sys)
		IDirect3DSurface9_Release(sys);
	if (rt)
		IDirect3DSurface9_Release(rt);
	pump_messages();
	return s->failures == before;
}

/* Window, IDirect3D9 with a QD3D session, optionally a device. */
static int connect_qd3d(struct scene *s, int with_device)
{
	HRESULT hr;

	if (!make_window()) {
		fail(s, "window error %lu", (unsigned long)GetLastError());
		return 0;
	}
	g_d3d = d3d_create();
	if (g_d3d == NULL) {
		fail(s, "Direct3DCreate9 returned NULL");
		return 0;
	}
	if (!is_qd3d(g_d3d)) {
		fail(s, "pid %lu got no QD3D session (stock runtime)",
		     (unsigned long)GetCurrentProcessId());
		return 0;
	}
	if (!with_device)
		return 1;
	hr = create_device();
	if (hr != D3D_OK) {
		fail(s, "CreateDevice returned 0x%08lx", (unsigned long)hr);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Children                                                            */

/* Renders; with ready/go event names (not "-"), signals ready, waits for
 * go and renders CONCURRENT_ROUNDS more times while the parent does. */
static int child_render(const char *ready_name, const char *go_name)
{
	struct scene s = { .name = "child_render" };
	HANDLE ready = NULL, go = NULL;
	int i;

	if (!connect_qd3d(&s, 1))
		goto out;
	printf("INFO render child pid %lu: QD3D session, device created\n",
	       (unsigned long)GetCurrentProcessId());
	if (!render_check(&s, C_RENDER_A, C_RENDER_B))
		goto out;
	if (strcmp(ready_name, "-") != 0) {
		ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name);
		go = OpenEventA(SYNCHRONIZE, FALSE, go_name);
		if (!ready || !go) {
			fail(&s, "cannot open %s/%s (error %lu)", ready_name,
			     go_name, (unsigned long)GetLastError());
			goto out;
		}
		SetEvent(ready);
		if (WaitForSingleObject(go, CHILD_WAIT_MS) != WAIT_OBJECT_0) {
			fail(&s, "parent never said go");
			goto out;
		}
		for (i = 0; i < CONCURRENT_ROUNDS; i++)
			if (!render_check(&s, i & 1 ? C_RENDER_A : C_RENDER_B,
					  i & 1 ? C_RENDER_B : C_RENDER_A))
				goto out;
	}
out:
	if (ready)
		CloseHandle(ready);
	if (go)
		CloseHandle(go);
	release_all();
	return report(&s) ? 0 : 1;
}

/* Holds a device, a texture and a vertex buffer, renders, and terminates
 * itself with all of them alive: no DLL detach, no GOODBYE. */
static int child_crash(void)
{
	struct scene s = { .name = "child_crash" };
	IDirect3DTexture9 *tex = NULL;
	IDirect3DVertexBuffer9 *vb = NULL;

	if (!connect_qd3d(&s, 1))
		goto out;
	TRY(&s, IDirect3DDevice9_CreateTexture(g_dev, 16, 16, 1, 0,
					       D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
					       &tex, NULL));
	TRY(&s, IDirect3DDevice9_CreateVertexBuffer(
			g_dev, 4 * sizeof(struct vtx), D3DUSAGE_WRITEONLY, FVF,
			D3DPOOL_MANAGED, &vb, NULL));
	if (!render_check(&s, C_CRASH_A, C_CRASH_B))
		goto out;
	printf("INFO crash child pid %lu: terminating with its device, a texture and a vertex buffer alive\n",
	       (unsigned long)GetCurrentProcessId());
	fflush(stdout);
	TerminateProcess(GetCurrentProcess(), CRASH_EXIT);
out:
	if (vb)
		IDirect3DVertexBuffer9_Release(vb);
	if (tex)
		IDirect3DTexture9_Release(tex);
	release_all();
	return report(&s) ? 0 : 1;
}

/* The only slot is busy: Direct3DCreate9 must hand out the stock
 * runtime's object. */
static int child_stock(void)
{
	struct scene s = { .name = "child_stock" };
	char dir[MAX_PATH + 16], *p;
	HMODULE proxy, impl;
	UINT adapters;

	g_d3d = d3d_create();
	if (g_d3d == NULL) {
		fail(&s, "Direct3DCreate9 returned NULL");
		goto out;
	}
	if (is_qd3d(g_d3d)) {
		fail(&s, "got a QD3D session although the only slot is busy");
		goto out;
	}
	/* The proxy beside the executable stays loaded and forwards. */
	strcpy(dir, g_exe);
	p = strrchr(dir, '\\');
	strcpy(p ? p + 1 : dir, "d3d9.dll");
	proxy = GetModuleHandleA(dir);
	impl = module_of(g_d3d);
	print_module("stock child IDirect3D9 from", g_d3d);
	if (proxy == NULL || impl == NULL || impl == proxy) {
		fail(&s, "IDirect3D9 not from the stock runtime (proxy %p, implementation %p)",
		     (void *)proxy, (void *)impl);
		goto out;
	}
	adapters = IDirect3D9_GetAdapterCount(g_d3d);
	if (adapters < 1)
		fail(&s, "stock GetAdapterCount returned %u", adapters);
out:
	release_all();
	return report(&s) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Parent                                                              */

static HANDLE spawn(const char *args, DWORD *pid)
{
	char cmd[MAX_PATH + 512];
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	HANDLE std[3];
	int i;

	std[0] = GetStdHandle(STD_INPUT_HANDLE);
	std[1] = GetStdHandle(STD_OUTPUT_HANDLE);
	std[2] = GetStdHandle(STD_ERROR_HANDLE);
	for (i = 0; i < 3; i++)
		if (std[i] && std[i] != INVALID_HANDLE_VALUE)
			SetHandleInformation(std[i], HANDLE_FLAG_INHERIT,
					     HANDLE_FLAG_INHERIT);
	snprintf(cmd, sizeof(cmd), "\"%s\" --child %s", g_exe, args);
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = std[0];
	si.hStdOutput = std[1];
	si.hStdError = std[2];
	if (!CreateProcessA(g_exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
			    &pi))
		return NULL;
	CloseHandle(pi.hThread);
	*pid = pi.dwProcessId;
	return pi.hProcess;
}

/* Waits for a child's exit; returns its exit code, or (DWORD)-1 when it
 * had to be terminated. Closes the handle. */
static DWORD reap(HANDLE child, DWORD ms)
{
	DWORD code = (DWORD)-1;

	if (WaitForSingleObject(child, ms) != WAIT_OBJECT_0)
		TerminateProcess(child, 99);
	else if (!GetExitCodeProcess(child, &code))
		code = (DWORD)-1;
	WaitForSingleObject(child, INFINITE);
	CloseHandle(child);
	return code;
}

static void log_load(void)
{
	char path[MAX_PATH + 16];
	DWORD n = GetTempPathA(MAX_PATH, path);
	FILE *f;
	long len;

	free(g_log);
	g_log = NULL;
	if (n == 0 || n >= MAX_PATH)
		return;
	strcpy(path + n, "qd3d.log");
	f = fopen(path, "rb");
	if (!f)
		return;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 &&
	    fseek(f, 0, SEEK_SET) == 0 &&
	    (g_log = (char *)malloc((size_t)len + 1u)) != NULL)
		g_log[fread(g_log, 1, (size_t)len, f)] = 0;
	fclose(f);
}

/* The rest of the first qd3d.log line of process pid whose message starts
 * with msg, or NULL. */
static const char *log_find(DWORD pid, const char *msg)
{
	char key[MAX_PATH + 128];
	const char *p;
	int n = snprintf(key, sizeof(key), " %lu %s: %s", (unsigned long)pid,
			 g_base, msg);

	if (!g_log || n <= 0 || (size_t)n >= sizeof(key))
		return NULL;
	p = strstr(g_log, key);
	return p ? p + n : NULL;
}

/* The slot process pid connected to according to qd3d.log, or -1. */
static long log_slot(DWORD pid)
{
	const char *p = log_find(pid, "connected (loopback) slot ");

	return p ? strtol(p, NULL, 10) : -1;
}

/* The kernel closes a dead process's handles, its slot claim among them,
 * as it tears the process down; its process object may be signalled
 * before that ends. Waits until one of the n claims is free; returns the
 * milliseconds that took, or -1. */
static long wait_free_slot(unsigned n)
{
	char name[512], base[256];
	DWORD start = GetTickCount();
	unsigned i;

	if (!GetEnvironmentVariableA("QD3D_LOOPBACK_NAME", base,
				     sizeof(base)))
		strcpy(base, QD3D_LOOPBACK_MAPPING);
	for (;;) {
		for (i = 0; i < n; i++) {
			HANDLE e;

			snprintf(name, sizeof(name), "%s%s%u", base,
				 QD3D_LOOPBACK_SLOT_CLAIM, i);
			e = OpenEventA(SYNCHRONIZE, FALSE, name);
			if (!e)
				return (long)(GetTickCount() - start);
			CloseHandle(e);
		}
		if (GetTickCount() - start >= CLAIM_WAIT_MS)
			return -1;
		Sleep(10);
	}
}

/* A launcher holds an IDirect3D9 (session, no device) and starts the game,
 * which must get a slot of its own; then both render at once. */
static void scenario_launcher(struct scene *s)
{
	char ready_name[96], go_name[96], args[256];
	HANDLE ready, go, child = NULL;
	DWORD pid = 0, code, r;
	HRESULT hr;
	int i;

	snprintf(ready_name, sizeof(ready_name),
		 "Local\\qd3d9_multiproc_%lu_ready",
		 (unsigned long)GetCurrentProcessId());
	snprintf(go_name, sizeof(go_name), "Local\\qd3d9_multiproc_%lu_go",
		 (unsigned long)GetCurrentProcessId());
	ready = CreateEventA(NULL, TRUE, FALSE, ready_name);
	go = CreateEventA(NULL, TRUE, FALSE, go_name);
	if (!ready || !go) {
		fail(s, "CreateEvent error %lu", (unsigned long)GetLastError());
		goto out;
	}
	snprintf(args, sizeof(args), "render %s %s", ready_name, go_name);
	child = spawn(args, &pid);
	if (!child) {
		fail(s, "CreateProcess error %lu",
		     (unsigned long)GetLastError());
		goto out;
	}
	printf("INFO launcher: game child pid %lu\n", (unsigned long)pid);
	{
		HANDLE w[2] = { ready, child };

		r = WaitForMultipleObjects(2, w, FALSE, CHILD_WAIT_MS);
	}
	if (r != WAIT_OBJECT_0) {
		fail(s, "child pid %lu %s before rendering", (unsigned long)pid,
		     r == WAIT_OBJECT_0 + 1 ? "exited" : "timed out");
		goto out;
	}
	/* The child renders on its slot; now this process's device. */
	hr = create_device();
	if (hr != D3D_OK) {
		fail(s, "parent CreateDevice returned 0x%08lx",
		     (unsigned long)hr);
		goto out;
	}
	if (!render_check(s, C_PARENT_A, C_PARENT_B))
		goto out;
	SetEvent(go);
	for (i = 0; i < CONCURRENT_ROUNDS; i++)
		if (!render_check(s, i & 1 ? C_PARENT_B : C_PARENT_A,
				  i & 1 ? C_PARENT_A : C_PARENT_B))
			goto out;
	code = reap(child, CHILD_WAIT_MS);
	child = NULL;
	if (code != 0) {
		fail(s, "child pid %lu exit code %ld", (unsigned long)pid,
		     (long)code);
		goto out;
	}
	/* After the child's GOODBYE this session still renders. */
	render_check(s, C_PARENT_B, C_PARENT_A);
out:
	if (go)
		SetEvent(go);
	if (child)
		reap(child, CHILD_WAIT_MS);
	if (ready)
		CloseHandle(ready);
	if (go)
		CloseHandle(go);
}

/* A child dies holding its slot; the next child must get that slot and
 * render, and this process's session must not notice. */
static void scenario_crashed_owner(struct scene *s)
{
	HANDLE child;
	DWORD dead = 0, reuse = 0, code;
	long parent_slot, dead_slot, reuse_slot, waited;
	HRESULT hr;

	if (!g_dev && (hr = create_device()) != D3D_OK) {
		fail(s, "parent CreateDevice returned 0x%08lx",
		     (unsigned long)hr);
		return;
	}
	child = spawn("crash", &dead);
	if (!child) {
		fail(s, "CreateProcess error %lu",
		     (unsigned long)GetLastError());
		return;
	}
	code = reap(child, CHILD_WAIT_MS);
	if (code != CRASH_EXIT) {
		fail(s, "crash child pid %lu exit code %ld, expected %d",
		     (unsigned long)dead, (long)code, CRASH_EXIT);
		return;
	}
	waited = wait_free_slot(2);
	if (waited < 0) {
		fail(s, "no slot free %u ms after the crash child's end",
		     CLAIM_WAIT_MS);
		return;
	}
	printf("INFO crashed-owner: a slot was free %ld ms after the crash child's exit\n",
	       waited);
	child = spawn("render - -", &reuse);
	if (!child) {
		fail(s, "CreateProcess error %lu",
		     (unsigned long)GetLastError());
		return;
	}
	code = reap(child, CHILD_WAIT_MS);
	printf("INFO crashed-owner: dead pid %lu, reuse pid %lu\n",
	       (unsigned long)dead, (unsigned long)reuse);
	if (code != 0) {
		fail(s, "reuse child pid %lu exit code %ld",
		     (unsigned long)reuse, (long)code);
		return;
	}
	log_load();
	parent_slot = log_slot(GetCurrentProcessId());
	dead_slot = log_slot(dead);
	reuse_slot = log_slot(reuse);
	printf("INFO crashed-owner: slots parent %ld, dead %ld, reuse %ld\n",
	       parent_slot, dead_slot, reuse_slot);
	if (parent_slot < 0 || dead_slot < 0 || dead_slot == parent_slot ||
	    reuse_slot != dead_slot)
		fail(s, "qd3d.log slots: parent %ld, dead %ld, reuse %ld (reuse must take the dead child's)",
		     parent_slot, dead_slot, reuse_slot);
	/* The crash in another slot left this session alone. */
	render_check(s, C_PARENT_A, C_PARENT_B);
}

/* The only slot is this process's: a second process gets the stock
 * runtime, and qd3d.log says why. */
static void scenario_busy(struct scene *s)
{
	HANDLE child;
	DWORD pid = 0, code;
	HRESULT hr;

	child = spawn("stock", &pid);
	if (!child) {
		fail(s, "CreateProcess error %lu",
		     (unsigned long)GetLastError());
		return;
	}
	code = reap(child, CHILD_WAIT_MS);
	if (code != 0)
		fail(s, "stock child pid %lu exit code %ld",
		     (unsigned long)pid, (long)code);
	log_load();
	if (log_slot(GetCurrentProcessId()) != 0)
		fail(s, "qd3d.log has no \"connected (loopback) slot 0\" line of pid %lu",
		     (unsigned long)GetCurrentProcessId());
	if (!log_find(pid, "not connected: loopback: all 1 slots busy"))
		fail(s, "qd3d.log has no \"all 1 slots busy\" line of pid %lu",
		     (unsigned long)pid);
	if (!log_find(pid, "Direct3DCreate9: using the stock runtime (not connected)"))
		fail(s, "qd3d.log has no stock-runtime line of pid %lu",
		     (unsigned long)pid);
	/* A failed CreateDevice reaches the log with its HRESULT (no
	 * presentation parameters: D3DERR_INVALIDCALL, as natively). */
	hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
				     g_hwnd,
				     D3DCREATE_SOFTWARE_VERTEXPROCESSING, NULL,
				     &g_dev);
	if (hr != D3DERR_INVALIDCALL || g_dev != NULL)
		fail(s, "CreateDevice without parameters returned 0x%08lx",
		     (unsigned long)hr);
	g_dev = NULL;
	log_load();
	if (!log_find(GetCurrentProcessId(), "CreateDevice failed (hr 0x8876086c)"))
		fail(s, "qd3d.log has no CreateDevice failure line of pid %lu",
		     (unsigned long)GetCurrentProcessId());
	/* The owner's session is unaffected. */
	hr = create_device();
	if (hr != D3D_OK) {
		fail(s, "parent CreateDevice returned 0x%08lx",
		     (unsigned long)hr);
		return;
	}
	render_check(s, C_PARENT_A, C_PARENT_B);
}

/* FreeLibrary of the proxy sends GOODBYE, unmaps the channel and gives the
 * slot back: with the only slot this process's, a child connects right
 * after the unload; loaded again, this process connects again. */
static void scenario_unload(struct scene *s)
{
	char proxy[MAX_PATH + 16], *p;
	HANDLE child;
	DWORD pid = 0, code;
	HRESULT hr;

	release_d3d();
	strcpy(proxy, g_exe);
	p = strrchr(proxy, '\\');
	strcpy(p ? p + 1 : proxy, "d3d9.dll");
	if (!g_d3d9 || !FreeLibrary(g_d3d9)) {
		fail(s, "FreeLibrary(d3d9.dll) error %lu",
		     (unsigned long)GetLastError());
		return;
	}
	g_d3d9 = NULL;
	if (GetModuleHandleA(proxy)) {
		fail(s, "%s still loaded after FreeLibrary", proxy);
		return;
	}
	child = spawn("render - -", &pid);
	if (!child) {
		fail(s, "CreateProcess error %lu",
		     (unsigned long)GetLastError());
		return;
	}
	code = reap(child, CHILD_WAIT_MS);
	if (code != 0) {
		fail(s, "child pid %lu after the unload: exit code %ld",
		     (unsigned long)pid, (long)code);
		return;
	}
	if (wait_free_slot(1) < 0) {
		fail(s, "the slot stayed busy after child pid %lu",
		     (unsigned long)pid);
		return;
	}
	g_d3d = d3d_create();
	if (g_d3d == NULL || !is_qd3d(g_d3d)) {
		fail(s, "the reloaded proxy got no QD3D session");
		return;
	}
	hr = create_device();
	if (hr != D3D_OK) {
		fail(s, "parent CreateDevice returned 0x%08lx",
		     (unsigned long)hr);
		return;
	}
	render_check(s, C_PARENT_B, C_PARENT_A);
}

static int parent(unsigned slots)
{
	struct scene setup = { .name = "setup" };
	struct scene sc[2] = { { .name = "launcher" },
			       { .name = "crashed_owner" } };
	unsigned n, passed = 0;

	if (!connect_qd3d(&setup, 0)) {
		print_module("d3d9", g_d3d);
		report(&setup);
		release_all();
		return 2;
	}
	print_module("d3d9", g_d3d);
	printf("INFO parent pid %lu: QD3D session without a device\n",
	       (unsigned long)GetCurrentProcessId());
	if (slots == 2) {
		scenario_launcher(&sc[0]);
		passed += report(&sc[0]);
		scenario_crashed_owner(&sc[1]);
		passed += report(&sc[1]);
		n = 2;
	} else {
		sc[0].name = "busy";
		scenario_busy(&sc[0]);
		passed += report(&sc[0]);
		sc[1].name = "unload";
		scenario_unload(&sc[1]);
		passed += report(&sc[1]);
		n = 2;
	}
	release_all();
	printf("RESULT %s %u/%u scenarios passed\n",
	       passed == n ? "PASS" : "FAIL", passed, n);
	return passed == n ? 0 : 1;
}

int main(int argc, char **argv)
{
	HANDLE wd;
	const char *p;
	int child = argc >= 3 && !strcmp(argv[1], "--child");
	int rc;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!GetModuleFileNameA(NULL, g_exe, sizeof(g_exe))) {
		printf("FAIL setup: GetModuleFileName error %lu\n",
		       (unsigned long)GetLastError());
		return 2;
	}
	for (p = g_exe; *p; p++)
		if (*p == '\\' || *p == '/')
			g_base = p + 1;
	g_watchdog_ms = child ? CHILD_WATCHDOG_MS : PARENT_WATCHDOG_MS;
	g_finished = CreateEventA(NULL, TRUE, FALSE, NULL);
	wd = g_finished ? CreateThread(NULL, 0, watchdog, NULL, 0, NULL) : NULL;
	if (wd == NULL) {
		printf("FAIL setup: watchdog error %lu\n",
		       (unsigned long)GetLastError());
		return 2;
	}
	if (child && !strcmp(argv[2], "render") && argc == 5)
		rc = child_render(argv[3], argv[4]);
	else if (child && !strcmp(argv[2], "crash") && argc == 3)
		rc = child_crash();
	else if (child && !strcmp(argv[2], "stock") && argc == 3)
		rc = child_stock();
	else if (argc == 3 && !strcmp(argv[1], "--slots") &&
		 (!strcmp(argv[2], "1") || !strcmp(argv[2], "2")))
		rc = parent((unsigned)atoi(argv[2]));
	else {
		printf("usage: qd3d9_multiproc --slots 1|2\n");
		rc = 2;
	}
	SetEvent(g_finished);
	WaitForSingleObject(wd, INFINITE);
	CloseHandle(wd);
	CloseHandle(g_finished);
	free(g_log);
	return rc;
}
