/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d9_smoke - end-to-end Direct3D 9 oracle for QD3D remoting.
 *
 * Renders deterministic scenes on one windowed HAL device and verifies the
 * pixels itself through GetRenderTarget/GetBackBuffer + GetRenderTargetData
 * readback. The same executable must pass against the stock runtime and
 * against the QD3D proxy d3d9.dll placed beside it.
 *
 * Output: "INFO ..." lines, then one "PASS <scene>" or
 * "FAIL <scene>: <first mismatch>" line per scene and a final RESULT line.
 * Exit code: 0 all scenes passed, 1 a scene failed, 2 setup failed,
 * 3 watchdog timeout.
 */
#include <windows.h>
#include <d3d9.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd3d9_smoke_shaders.h"
#include "../proto/qd3d_proto.h" /* IID_QD3D_REMOTE */

#define BB_W 256
#define BB_H 256
#define RESET_W 128
#define RESET_H 128
#define WATCHDOG_MS 120000u
#define QUERY_TIMEOUT_MS 5000u
#define PRESENT_BURST 8

#define C_BLACK D3DCOLOR_XRGB(0x00, 0x00, 0x00)
#define C_WHITE D3DCOLOR_XRGB(0xff, 0xff, 0xff)

struct scene {
	const char *name;
	unsigned failures;
	char first[512];
};

struct image {
	UINT w, h;
	DWORD *px;
};

/* Pre-transformed, flat colour. */
struct vtx_c {
	float x, y, z, rhw;
	D3DCOLOR color;
};
#define FVF_C (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* Pre-transformed, one texture coordinate set. */
struct vtx_t {
	float x, y, z, rhw;
	float u, v;
};
#define FVF_T (D3DFVF_XYZRHW | D3DFVF_TEX1)

/* Untransformed, fixed-function T&L. */
struct vtx_xyz {
	float x, y, z;
	D3DCOLOR color;
};
#define FVF_XYZ (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static HWND g_hwnd;
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DPRESENT_PARAMETERS g_pp;
static int g_in_scene;
static unsigned g_readback_toggle;
static unsigned g_presents;
static unsigned g_present_failures;
static HRESULT g_first_present_hr = D3D_OK;
static HANDLE g_finished;
static char g_setup_error[256];
/* x87 control word right after CreateDevice (setup unmasks invalid operation
 * and zero divide first, as Need for Speed: Underground 2 runs). */
static unsigned short g_fpu_cw;

static unsigned short fpu_cw_get(void)
{
	unsigned short cw;

	__asm__ __volatile__("fnstcw %0" : "=m"(cw));
	return cw;
}

static void fpu_cw_set(unsigned short cw)
{
	__asm__ __volatile__("fldcw %0" : : "m"(cw));
}

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

#define RELEASE(type, p)                          \
	do {                                      \
		if (p) {                          \
			type##_Release(p);        \
			(p) = NULL;               \
		}                                 \
	} while (0)

static HRESULT begin_scene(void)
{
	HRESULT hr = IDirect3DDevice9_BeginScene(g_dev);

	if (hr == D3D_OK)
		g_in_scene = 1;
	return hr;
}

static HRESULT end_scene(void)
{
	g_in_scene = 0;
	return IDirect3DDevice9_EndScene(g_dev);
}

/* ------------------------------------------------------------------ */
/* Readback and pixel checks                                           */

static void image_free(struct image *img)
{
	free(img->px);
	img->px = NULL;
	img->w = 0;
	img->h = 0;
}

static int read_surface(struct scene *s, IDirect3DSurface9 *src,
			struct image *img)
{
	D3DSURFACE_DESC desc;
	IDirect3DSurface9 *sys = NULL;
	D3DLOCKED_RECT lr;
	int locked = 0;
	int ok = 0;
	UINT y;

	image_free(img);
	TRY(s, IDirect3DSurface9_GetDesc(src, &desc));
	if ((desc.Format != D3DFMT_X8R8G8B8 &&
	     desc.Format != D3DFMT_A8R8G8B8) ||
	    desc.Width == 0 || desc.Height == 0 || desc.Width > 4096 ||
	    desc.Height > 4096) {
		fail(s, "unexpected render target %ux%u format %u",
		     desc.Width, desc.Height, (unsigned)desc.Format);
		goto out;
	}
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(
		       g_dev, desc.Width, desc.Height, desc.Format,
		       D3DPOOL_SYSTEMMEM, &sys, NULL));
	TRY(s, IDirect3DDevice9_GetRenderTargetData(g_dev, src, sys));
	TRY(s, IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
	locked = 1;
	if (lr.pBits == NULL || lr.Pitch < (INT)(desc.Width * 4)) {
		fail(s, "readback lock pitch %d for width %u", lr.Pitch,
		     desc.Width);
		goto out;
	}
	img->px = malloc((size_t)desc.Width * desc.Height * sizeof(DWORD));
	if (img->px == NULL) {
		fail(s, "out of memory for %ux%u readback", desc.Width,
		     desc.Height);
		goto out;
	}
	for (y = 0; y < desc.Height; y++)
		memcpy(img->px + (size_t)y * desc.Width,
		       (const BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
		       (size_t)desc.Width * sizeof(DWORD));
	img->w = desc.Width;
	img->h = desc.Height;
	ok = 1;
out:
	if (locked)
		IDirect3DSurface9_UnlockRect(sys);
	RELEASE(IDirect3DSurface9, sys);
	return ok;
}

/* Alternates GetRenderTarget(0) and GetBackBuffer(0, 0) as the source. */
static int read_back_buffer(struct scene *s, struct image *img)
{
	IDirect3DSurface9 *surf = NULL;
	int ok = 0;

	if (g_readback_toggle++ & 1)
		TRY(s, IDirect3DDevice9_GetBackBuffer(
			       g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &surf));
	else
		TRY(s, IDirect3DDevice9_GetRenderTarget(g_dev, 0, &surf));
	if (surf == NULL) {
		fail(s, "back buffer getter returned NULL");
		goto out;
	}
	if (!read_surface(s, surf, img))
		goto out;
	if (img->w != g_pp.BackBufferWidth || img->h != g_pp.BackBufferHeight) {
		fail(s, "back buffer is %ux%u, expected %ux%u", img->w, img->h,
		     g_pp.BackBufferWidth, g_pp.BackBufferHeight);
		goto out;
	}
	ok = 1;
out:
	RELEASE(IDirect3DSurface9, surf);
	return ok;
}

static int channels_close(DWORD a, DWORD b, int tol)
{
	int shift;

	for (shift = 0; shift < 24; shift += 8) {
		int d = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);

		if (d > tol || d < -tol)
			return 0;
	}
	return 1;
}

/* Compares RGB only: the back buffer is X8R8G8B8. */
static int expect_px(struct scene *s, const struct image *img, int x, int y,
		     D3DCOLOR want, int tol)
{
	DWORD got;

	if (x < 0 || y < 0 || (UINT)x >= img->w || (UINT)y >= img->h) {
		fail(s, "sample %d,%d outside %ux%u image", x, y, img->w,
		     img->h);
		return 0;
	}
	got = img->px[(size_t)y * img->w + (size_t)x] & 0x00ffffffu;
	want &= 0x00ffffffu;
	if (channels_close(got, want, tol))
		return 1;
	fail(s, "expected %06lx (+-%d) got %06lx at %d,%d",
	     (unsigned long)want, tol, (unsigned long)got, x, y);
	return 0;
}

/* Every pixel of [x0,x1) x [y0,y1); stops at the first mismatch. */
static int expect_area(struct scene *s, const struct image *img, int x0,
		       int y0, int x1, int y1, D3DCOLOR want, int tol)
{
	int x, y;

	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			if (!expect_px(s, img, x, y, want, tol))
				return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* State and primitive helpers                                         */

/* Keeps the first non-D3D_OK result of a sequence of state calls. */
#define KEEP_FIRST(hr, call)                    \
	do {                                    \
		HRESULT keep_hr_ = (call);      \
		if ((hr) == D3D_OK)             \
			(hr) = keep_hr_;        \
	} while (0)

static HRESULT stage_diffuse(void)
{
	IDirect3DDevice9 *d = g_dev;
	HRESULT hr = D3D_OK;

	KEEP_FIRST(hr, IDirect3DDevice9_SetTexture(d, 0, NULL));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
	return hr;
}

static HRESULT stage_texture(IDirect3DBaseTexture9 *tex, DWORD address)
{
	IDirect3DDevice9 *d = g_dev;
	HRESULT hr = D3D_OK;

	KEEP_FIRST(hr, IDirect3DDevice9_SetTexture(d, 0, tex));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
	KEEP_FIRST(hr, IDirect3DDevice9_SetTextureStageState(
			       d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
	KEEP_FIRST(hr, IDirect3DDevice9_SetSamplerState(
			       d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
	KEEP_FIRST(hr, IDirect3DDevice9_SetSamplerState(
			       d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
	KEEP_FIRST(hr, IDirect3DDevice9_SetSamplerState(
			       d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
	KEEP_FIRST(hr, IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU,
							address));
	KEEP_FIRST(hr, IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV,
							address));
	return hr;
}

/* Known state at the start of every scene (Reset restores defaults). */
static void base_state(struct scene *s)
{
	static const struct {
		D3DRENDERSTATETYPE state;
		DWORD value;
	} rs[] = {
		{ D3DRS_LIGHTING, FALSE },
		{ D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_ZENABLE, D3DZB_FALSE },
		{ D3DRS_ZWRITEENABLE, TRUE },
		{ D3DRS_ZFUNC, D3DCMP_LESSEQUAL },
		{ D3DRS_ALPHABLENDENABLE, FALSE },
		{ D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_FOGENABLE, FALSE },
		{ D3DRS_SPECULARENABLE, FALSE },
		{ D3DRS_STENCILENABLE, FALSE },
		{ D3DRS_SCISSORTESTENABLE, FALSE },
		{ D3DRS_COLORWRITEENABLE, 0xf },
	};
	size_t i;

	for (i = 0; i < sizeof(rs) / sizeof(rs[0]); i++)
		TRY(s, IDirect3DDevice9_SetRenderState(g_dev, rs[i].state,
						       rs[i].value));
	TRY(s, IDirect3DDevice9_SetVertexShader(g_dev, NULL));
	TRY(s, IDirect3DDevice9_SetPixelShader(g_dev, NULL));
	TRY(s, stage_diffuse());
out:
	return;
}

/*
 * Screen rectangle covering exactly the pixels [x0,x1) x [y0,y1): Direct3D 9
 * pixel centres sit on integer coordinates, so edges go half a pixel before.
 */
static void rect_c(struct vtx_c v[4], int x0, int y0, int x1, int y1, float z,
		   D3DCOLOR color)
{
	float l = (float)x0 - 0.5f, t = (float)y0 - 0.5f;
	float r = (float)x1 - 0.5f, b = (float)y1 - 0.5f;

	v[0] = (struct vtx_c){ l, t, z, 1.0f, color };
	v[1] = (struct vtx_c){ r, t, z, 1.0f, color };
	v[2] = (struct vtx_c){ l, b, z, 1.0f, color };
	v[3] = (struct vtx_c){ r, b, z, 1.0f, color };
}

static HRESULT draw_rect_c(int x0, int y0, int x1, int y1, float z,
			   D3DCOLOR color)
{
	struct vtx_c v[4];
	HRESULT hr;

	rect_c(v, x0, y0, x1, y1, z, color);
	hr = IDirect3DDevice9_SetFVF(g_dev, FVF_C);
	if (hr != D3D_OK)
		return hr;
	return IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2,
						v, sizeof(v[0]));
}

static HRESULT draw_rect_t(int x0, int y0, int x1, int y1, float u0, float v0,
			   float u1, float v1)
{
	float l = (float)x0 - 0.5f, t = (float)y0 - 0.5f;
	float r = (float)x1 - 0.5f, b = (float)y1 - 0.5f;
	struct vtx_t v[4] = {
		{ l, t, 0.5f, 1.0f, u0, v0 },
		{ r, t, 0.5f, 1.0f, u1, v0 },
		{ l, b, 0.5f, 1.0f, u0, v1 },
		{ r, b, 0.5f, 1.0f, u1, v1 },
	};
	HRESULT hr;

	hr = IDirect3DDevice9_SetFVF(g_dev, FVF_T);
	if (hr != D3D_OK)
		return hr;
	return IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2,
						v, sizeof(v[0]));
}

/* Copies [rc] (whole level when NULL) of a tw x th texel image to level 0. */
static int upload_argb(struct scene *s, IDirect3DTexture9 *tex,
		       const DWORD *texels, UINT tw, UINT th, const RECT *rc)
{
	RECT full = { 0, 0, (LONG)tw, (LONG)th };
	const RECT *r = rc ? rc : &full;
	D3DLOCKED_RECT lr;
	size_t row_bytes;
	LONG y;
	int ok = 0;

	if (r->left < 0 || r->top < 0 || r->right > (LONG)tw ||
	    r->bottom > (LONG)th || r->left >= r->right ||
	    r->top >= r->bottom) {
		fail(s, "bad upload rect");
		return 0;
	}
	row_bytes = (size_t)(r->right - r->left) * sizeof(DWORD);
	TRY(s, IDirect3DTexture9_LockRect(tex, 0, &lr, rc, 0));
	if (lr.pBits == NULL || lr.Pitch < (INT)row_bytes) {
		fail(s, "texture lock pitch %d for %u row bytes", lr.Pitch,
		     (unsigned)row_bytes);
	} else {
		for (y = r->top; y < r->bottom; y++)
			memcpy((BYTE *)lr.pBits +
				       (size_t)(y - r->top) * (size_t)lr.Pitch,
			       texels + (size_t)y * tw + (size_t)r->left,
			       row_bytes);
		ok = 1;
	}
	TRY(s, IDirect3DTexture9_UnlockRect(tex, 0));
	return ok;
out:
	return 0;
}

/* Same for DXT1: blocks is bw x bh blocks of 8 bytes; rc is in texels. */
static int upload_dxt1(struct scene *s, IDirect3DTexture9 *tex,
		       const BYTE *blocks, UINT bw, UINT bh, const RECT *rc)
{
	RECT full = { 0, 0, (LONG)bw * 4, (LONG)bh * 4 };
	const RECT *r = rc ? rc : &full;
	D3DLOCKED_RECT lr;
	LONG bx0, by0, bx1, by1, by;
	size_t row_bytes;
	int ok = 0;

	if (r->left < 0 || r->top < 0 || r->right > (LONG)bw * 4 ||
	    r->bottom > (LONG)bh * 4 || r->left >= r->right ||
	    r->top >= r->bottom || (r->left & 3) || (r->top & 3) ||
	    (r->right & 3) || (r->bottom & 3)) {
		fail(s, "bad DXT1 upload rect");
		return 0;
	}
	bx0 = r->left / 4;
	by0 = r->top / 4;
	bx1 = r->right / 4;
	by1 = r->bottom / 4;
	row_bytes = (size_t)(bx1 - bx0) * 8;
	TRY(s, IDirect3DTexture9_LockRect(tex, 0, &lr, rc, 0));
	if (lr.pBits == NULL || lr.Pitch < (INT)row_bytes) {
		fail(s, "DXT1 lock pitch %d for %u block-row bytes", lr.Pitch,
		     (unsigned)row_bytes);
	} else {
		for (by = by0; by < by1; by++)
			memcpy((BYTE *)lr.pBits +
				       (size_t)(by - by0) * (size_t)lr.Pitch,
			       blocks + ((size_t)by * bw + (size_t)bx0) * 8,
			       row_bytes);
		ok = 1;
	}
	TRY(s, IDirect3DTexture9_UnlockRect(tex, 0));
	return ok;
out:
	return 0;
}

static void dxt1_block(BYTE *p, WORD c0, WORD c1, DWORD indices)
{
	p[0] = (BYTE)(c0 & 0xff);
	p[1] = (BYTE)(c0 >> 8);
	p[2] = (BYTE)(c1 & 0xff);
	p[3] = (BYTE)(c1 >> 8);
	p[4] = (BYTE)(indices & 0xff);
	p[5] = (BYTE)((indices >> 8) & 0xff);
	p[6] = (BYTE)((indices >> 16) & 0xff);
	p[7] = (BYTE)(indices >> 24);
}

static D3DCOLOR rgb565_to_xrgb(WORD c)
{
	unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;

	return D3DCOLOR_XRGB((r << 3) | (r >> 2), (g << 2) | (g >> 4),
			     (b << 3) | (b >> 2));
}

/*
 * Writes one 4-colour-mode block (c0 > c1, indices 0 and 1 only, so the
 * decoded texels are exact) and records its decoded texels in exp[8][8].
 */
static void dxt1_put(BYTE *blocks, UINT bw, D3DCOLOR exp[8][8], UINT bx,
		     UINT by, WORD c0, WORD c1, DWORD indices)
{
	UINT x, y;

	dxt1_block(blocks + ((size_t)by * bw + bx) * 8, c0, c1, indices);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			exp[by * 4 + y][bx * 4 + x] =
				((indices >> (2 * (4 * y + x))) & 3) ?
					rgb565_to_xrgb(c1) :
					rgb565_to_xrgb(c0);
}

static DWORD dxt1_indices(int (*one)(UINT x, UINT y))
{
	DWORD idx = 0;
	UINT x, y;

	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			if (one(x, y))
				idx |= 1u << (2 * (4 * y + x));
	return idx;
}

static int dxt1_checker(UINT x, UINT y)
{
	return (x ^ y) & 1;
}

static int dxt1_left_half(UINT x, UINT y)
{
	return x < 2;
}

static void ortho_pixels(D3DMATRIX *m, float w, float h)
{
	memset(m, 0, sizeof(*m));
	m->_11 = 2.0f / w;
	m->_22 = -2.0f / h;
	m->_33 = 1.0f;
	m->_41 = -1.0f;
	m->_42 = 1.0f;
	m->_44 = 1.0f;
}

static void identity(D3DMATRIX *m)
{
	memset(m, 0, sizeof(*m));
	m->_11 = m->_22 = m->_33 = m->_44 = 1.0f;
}

static int wait_query(struct scene *s, IDirect3DQuery9 *q, void *data,
		      DWORD size, const char *what)
{
	DWORD start = GetTickCount();
	unsigned long polls = 0;

	for (;;) {
		HRESULT hr = IDirect3DQuery9_GetData(q, data, size,
						     D3DGETDATA_FLUSH);

		polls++;
		if (hr == S_OK)
			return 1;
		if (hr != S_FALSE) {
			fail(s, "%s GetData returned 0x%08lx after %lu polls",
			     what, (unsigned long)hr, polls);
			return 0;
		}
		if (GetTickCount() - start > QUERY_TIMEOUT_MS) {
			fail(s, "%s incomplete after %u ms (%lu polls)", what,
			     QUERY_TIMEOUT_MS, polls);
			return 0;
		}
		SwitchToThread();
	}
}

/* ------------------------------------------------------------------ */
/* Scenes                                                              */

#define C_CLEAR D3DCOLOR_XRGB(0x33, 0x66, 0x99)
#define C_CLEAR_ONE D3DCOLOR_XRGB(0xc0, 0x80, 0x40)
#define C_CLEAR_TWO D3DCOLOR_XRGB(0x10, 0xe0, 0xa0)

static int in_d3drect(const D3DRECT *r, int x, int y)
{
	return x >= r->x1 && x < r->x2 && y >= r->y1 && y < r->y2;
}

static void scene_clear(struct scene *s)
{
	static const D3DRECT one = { 64, 64, 192, 128 };
	static const D3DRECT two[2] = { { 0, 200, 32, 232 },
					{ 224, 0, 256, 16 } };
	struct image img = { 0 };
	int x, y;

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL,
				      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
					      D3DCLEAR_STENCIL,
				      C_CLEAR, 1.0f, 0));
	TRY(s, IDirect3DDevice9_Clear(g_dev, 1, &one, D3DCLEAR_TARGET,
				      C_CLEAR_ONE, 1.0f, 0));
	TRY(s, IDirect3DDevice9_Clear(g_dev, 2, two, D3DCLEAR_TARGET,
				      C_CLEAR_TWO, 1.0f, 0));
	if (!read_back_buffer(s, &img))
		goto out;
	for (y = 0; y < (int)img.h; y++)
		for (x = 0; x < (int)img.w; x++) {
			D3DCOLOR want = C_CLEAR;

			if (in_d3drect(&one, x, y))
				want = C_CLEAR_ONE;
			else if (in_d3drect(&two[0], x, y) ||
				 in_d3drect(&two[1], x, y))
				want = C_CLEAR_TWO;
			if (!expect_px(s, &img, x, y, want, 0))
				goto out;
		}
out:
	image_free(&img);
}

#define C_TRI D3DCOLOR_XRGB(0xe0, 0x30, 0x10)

static void scene_up_tri(struct scene *s)
{
	const struct vtx_c tri[3] = {
		{ 32.0f, 32.0f, 0.5f, 1.0f, C_TRI },
		{ 224.0f, 32.0f, 0.5f, 1.0f, C_TRI },
		{ 32.0f, 224.0f, 0.5f, 1.0f, C_TRI },
	};
	struct image img = { 0 };
	int x, y;

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DDevice9_SetFVF(g_dev, FVF_C));
	TRY(s, IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 1,
						tri, sizeof(tri[0])));
	TRY(s, end_scene());
	if (!read_back_buffer(s, &img))
		goto out;
	/* Edges are x = 32, y = 32 and x + y = 256; skip a band around them. */
	for (y = 0; y < (int)img.h; y++)
		for (x = 0; x < (int)img.w; x++) {
			D3DCOLOR want;

			if (x >= 36 && y >= 36 && x + y <= 248)
				want = C_TRI;
			else if (x < 28 || y < 28 || x + y >= 264)
				want = C_BLACK;
			else
				continue;
			if (!expect_px(s, &img, x, y, want, 0))
				goto out;
		}
out:
	image_free(&img);
}

#define C_VB_BG D3DCOLOR_XRGB(0x10, 0x20, 0x30)
#define C_VB0 D3DCOLOR_XRGB(0x20, 0xc0, 0x40)
#define C_VB1 D3DCOLOR_XRGB(0x40, 0x60, 0xe0)

static void quad_xyz(struct vtx_xyz *v, float x0, float y0, float x1, float y1,
		     D3DCOLOR color)
{
	v[0] = (struct vtx_xyz){ x0, y0, 0.5f, color };
	v[1] = (struct vtx_xyz){ x1, y0, 0.5f, color };
	v[2] = (struct vtx_xyz){ x0, y1, 0.5f, color };
	v[3] = (struct vtx_xyz){ x1, y1, 0.5f, color };
}

static void scene_vb_ib(struct scene *s)
{
	static const WORD indices[6] = { 0, 1, 2, 2, 1, 3 };
	IDirect3DVertexBuffer9 *vb = NULL;
	IDirect3DIndexBuffer9 *ib = NULL;
	struct image img = { 0 };
	D3DMATRIX ident, proj;
	void *p;

	TRY(s, IDirect3DDevice9_CreateVertexBuffer(
		       g_dev, 8 * sizeof(struct vtx_xyz), D3DUSAGE_WRITEONLY,
		       FVF_XYZ, D3DPOOL_MANAGED, &vb, NULL));
	TRY(s, IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0));
	/* Two quads in pixel units; the second is reached with BaseVertexIndex 4. */
	quad_xyz((struct vtx_xyz *)p, 32.0f, 32.0f, 112.0f, 224.0f, C_VB0);
	quad_xyz((struct vtx_xyz *)p + 4, 144.0f, 32.0f, 224.0f, 224.0f, C_VB1);
	TRY(s, IDirect3DVertexBuffer9_Unlock(vb));

	TRY(s, IDirect3DDevice9_CreateIndexBuffer(
		       g_dev, sizeof(indices), D3DUSAGE_WRITEONLY,
		       D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL));
	TRY(s, IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0));
	memcpy(p, indices, sizeof(indices));
	TRY(s, IDirect3DIndexBuffer9_Unlock(ib));

	identity(&ident);
	ortho_pixels(&proj, (float)BB_W, (float)BB_H);
	TRY(s, IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, &ident));
	TRY(s, IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, &ident));
	TRY(s, IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, &proj));

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_VB_BG,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DDevice9_SetFVF(g_dev, FVF_XYZ));
	TRY(s, IDirect3DDevice9_SetStreamSource(g_dev, 0, vb, 0,
						sizeof(struct vtx_xyz)));
	TRY(s, IDirect3DDevice9_SetIndices(g_dev, ib));
	TRY(s, IDirect3DDevice9_DrawIndexedPrimitive(
		       g_dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
	TRY(s, IDirect3DDevice9_DrawIndexedPrimitive(
		       g_dev, D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2));
	TRY(s, end_scene());
	if (!read_back_buffer(s, &img))
		goto out;
	expect_area(s, &img, 36, 36, 108, 220, C_VB0, 0);
	expect_area(s, &img, 148, 36, 220, 220, C_VB1, 0);
	expect_area(s, &img, 0, 0, 256, 28, C_VB_BG, 0);
	expect_area(s, &img, 0, 228, 256, 256, C_VB_BG, 0);
	expect_area(s, &img, 116, 36, 140, 220, C_VB_BG, 0);
	expect_area(s, &img, 0, 36, 28, 220, C_VB_BG, 0);
	expect_area(s, &img, 228, 36, 256, 220, C_VB_BG, 0);
out:
	if (g_dev) {
		IDirect3DDevice9_SetStreamSource(g_dev, 0, NULL, 0, 0);
		IDirect3DDevice9_SetIndices(g_dev, NULL);
	}
	RELEASE(IDirect3DIndexBuffer9, ib);
	RELEASE(IDirect3DVertexBuffer9, vb);
	image_free(&img);
}

static D3DCOLOR tex1_texel(int x, int y)
{
	return D3DCOLOR_ARGB(0xff, 0x20 + x * 0x40, 0x20 + y * 0x40,
			     ((x ^ y) & 1) ? 0xf0 : 0x10);
}

#define C_T2_BASE D3DCOLOR_ARGB(0xff, 0x30, 0x50, 0x70)
#define C_T2_SUB1 D3DCOLOR_ARGB(0xff, 0xe0, 0x20, 0x90)
#define C_T2_SUB2 D3DCOLOR_ARGB(0xff, 0x08, 0xf8, 0x18)

static void fill_rect_texels(DWORD *texels, UINT tw, const RECT *rc,
			     D3DCOLOR color)
{
	LONG x, y;

	for (y = rc->top; y < rc->bottom; y++)
		for (x = rc->left; x < rc->right; x++)
			texels[(size_t)y * tw + (size_t)x] = color;
}

static void scene_texture(struct scene *s)
{
	static const RECT t2_full = { 0, 0, 8, 8 };
	static const RECT t2_sub1 = { 2, 1, 7, 5 };
	static const RECT t2_sub2 = { 0, 6, 3, 8 };
	static const RECT t3_sub = { 4, 4, 8, 8 };
	IDirect3DTexture9 *t1 = NULL, *t2 = NULL, *t3 = NULL;
	DWORD texels1[4 * 4], texels2[8 * 8];
	BYTE blocks3[2 * 2 * 8];
	D3DCOLOR exp3[8][8];
	D3DDISPLAYMODE mode;
	struct image img = { 0 };
	int x, y;

	TRY(s, IDirect3D9_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT,
						&mode));
	TRY(s, IDirect3D9_CheckDeviceFormat(g_d3d, D3DADAPTER_DEFAULT,
					    D3DDEVTYPE_HAL, mode.Format, 0,
					    D3DRTYPE_TEXTURE, D3DFMT_DXT1));

	/* t1: 4x4 checker with unique per-texel red/green, one full lock. */
	TRY(s, IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0,
					      D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
					      &t1, NULL));
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			texels1[y * 4 + x] = tex1_texel(x, y);
	if (!upload_argb(s, t1, texels1, 4, 4, NULL))
		goto out;

	/* t2: 8x8, full lock then two sub-rectangle locks. */
	TRY(s, IDirect3DDevice9_CreateTexture(g_dev, 8, 8, 1, 0,
					      D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
					      &t2, NULL));
	fill_rect_texels(texels2, 8, &t2_full, C_T2_BASE);
	if (!upload_argb(s, t2, texels2, 8, 8, NULL))
		goto out;
	fill_rect_texels(texels2, 8, &t2_sub1, C_T2_SUB1);
	if (!upload_argb(s, t2, texels2, 8, 8, &t2_sub1))
		goto out;
	fill_rect_texels(texels2, 8, &t2_sub2, C_T2_SUB2);
	if (!upload_argb(s, t2, texels2, 8, 8, &t2_sub2))
		goto out;

	/* t3: 8x8 DXT1 (2x2 blocks), full lock then one block sub-rect. */
	TRY(s, IDirect3DDevice9_CreateTexture(g_dev, 8, 8, 1, 0, D3DFMT_DXT1,
					      D3DPOOL_MANAGED, &t3, NULL));
	dxt1_put(blocks3, 2, exp3, 0, 0, 0xf800, 0x0000, 0);
	dxt1_put(blocks3, 2, exp3, 1, 0, 0x07e0, 0x0000,
		 dxt1_indices(dxt1_checker));
	dxt1_put(blocks3, 2, exp3, 0, 1, 0x001f, 0x0000, 0);
	dxt1_put(blocks3, 2, exp3, 1, 1, 0xffff, 0x0000, 0);
	if (!upload_dxt1(s, t3, blocks3, 2, 2, NULL))
		goto out;
	dxt1_put(blocks3, 2, exp3, 1, 1, 0xffe0, 0x0000,
		 dxt1_indices(dxt1_left_half));
	if (!upload_dxt1(s, t3, blocks3, 2, 2, &t3_sub))
		goto out;

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	/* Top-left: t1, 32x32 pixels per texel. */
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)t1, D3DTADDRESS_CLAMP));
	TRY(s, draw_rect_t(0, 0, 128, 128, 0.0f, 0.0f, 1.0f, 1.0f));
	/* Top-right: t2, 16x16 pixels per texel. */
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)t2, D3DTADDRESS_CLAMP));
	TRY(s, draw_rect_t(128, 0, 256, 128, 0.0f, 0.0f, 1.0f, 1.0f));
	/* Bottom-left: t3 (DXT1), 16x16 pixels per texel. */
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)t3, D3DTADDRESS_CLAMP));
	TRY(s, draw_rect_t(0, 128, 128, 256, 0.0f, 0.0f, 1.0f, 1.0f));
	/* Bottom-right: t1 repeated twice with WRAP, 16x16 pixels per texel. */
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)t1, D3DTADDRESS_WRAP));
	TRY(s, draw_rect_t(128, 128, 256, 256, 0.0f, 0.0f, 2.0f, 2.0f));
	TRY(s, end_scene());
	TRY(s, stage_diffuse());
	if (!read_back_buffer(s, &img))
		goto out;
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			expect_area(s, &img, 32 * x + 6, 32 * y + 6,
				    32 * x + 26, 32 * y + 26, tex1_texel(x, y),
				    0);
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++) {
			expect_area(s, &img, 128 + 16 * x + 4, 16 * y + 4,
				    128 + 16 * x + 12, 16 * y + 12,
				    texels2[y * 8 + x], 0);
			expect_area(s, &img, 16 * x + 4, 128 + 16 * y + 4,
				    16 * x + 12, 128 + 16 * y + 12, exp3[y][x],
				    0);
			expect_area(s, &img, 128 + 16 * x + 4,
				    128 + 16 * y + 4, 128 + 16 * x + 12,
				    128 + 16 * y + 12, tex1_texel(x & 3, y & 3),
				    0);
		}
out:
	if (g_dev)
		IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
	RELEASE(IDirect3DTexture9, t3);
	RELEASE(IDirect3DTexture9, t2);
	RELEASE(IDirect3DTexture9, t1);
	image_free(&img);
}

#define C_DYN_A D3DCOLOR_XRGB(0xff, 0x80, 0x00)
#define C_DYN_B D3DCOLOR_XRGB(0x00, 0xff, 0x80)
#define C_DYN_C D3DCOLOR_XRGB(0x80, 0x00, 0xff)
#define C_DYN_D D3DCOLOR_XRGB(0xff, 0xff, 0x00)

/* Locks four vertices at vertex offset `first` with `flags`, writes a rect. */
static int dyn_write(struct scene *s, IDirect3DVertexBuffer9 *vb, UINT first,
		     DWORD flags, int x0, int y0, int x1, int y1,
		     D3DCOLOR color)
{
	void *p = NULL;

	TRY(s, IDirect3DVertexBuffer9_Lock(vb, first * sizeof(struct vtx_c),
					   4 * sizeof(struct vtx_c), &p,
					   flags));
	if (p == NULL) {
		IDirect3DVertexBuffer9_Unlock(vb);
		fail(s, "dynamic VB lock returned NULL");
		return 0;
	}
	rect_c((struct vtx_c *)p, x0, y0, x1, y1, 0.5f, color);
	TRY(s, IDirect3DVertexBuffer9_Unlock(vb));
	return 1;
out:
	return 0;
}

static void scene_dynamic_vb(struct scene *s)
{
	IDirect3DVertexBuffer9 *vb = NULL;
	struct image img = { 0 };

	TRY(s, IDirect3DDevice9_CreateVertexBuffer(
		       g_dev, 16 * sizeof(struct vtx_c),
		       D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF_C,
		       D3DPOOL_DEFAULT, &vb, NULL));
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DDevice9_SetFVF(g_dev, FVF_C));
	TRY(s, IDirect3DDevice9_SetStreamSource(g_dev, 0, vb, 0,
						sizeof(struct vtx_c)));
	/* A: DISCARD at 0; B: NOOVERWRITE append at 4. */
	if (!dyn_write(s, vb, 0, D3DLOCK_DISCARD, 16, 16, 112, 112, C_DYN_A))
		goto out;
	TRY(s, IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLESTRIP, 0, 2));
	if (!dyn_write(s, vb, 4, D3DLOCK_NOOVERWRITE, 144, 16, 240, 112,
		       C_DYN_B))
		goto out;
	TRY(s, IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLESTRIP, 4, 2));
	/* C: DISCARD again (A is already drawn); D: NOOVERWRITE append. */
	if (!dyn_write(s, vb, 0, D3DLOCK_DISCARD, 16, 144, 112, 240, C_DYN_C))
		goto out;
	TRY(s, IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLESTRIP, 0, 2));
	if (!dyn_write(s, vb, 4, D3DLOCK_NOOVERWRITE, 144, 144, 240, 240,
		       C_DYN_D))
		goto out;
	TRY(s, IDirect3DDevice9_DrawPrimitive(g_dev, D3DPT_TRIANGLESTRIP, 4, 2));
	TRY(s, end_scene());
	if (!read_back_buffer(s, &img))
		goto out;
	expect_area(s, &img, 16, 16, 112, 112, C_DYN_A, 0);
	expect_area(s, &img, 144, 16, 240, 112, C_DYN_B, 0);
	expect_area(s, &img, 16, 144, 112, 240, C_DYN_C, 0);
	expect_area(s, &img, 144, 144, 240, 240, C_DYN_D, 0);
	expect_area(s, &img, 112, 0, 144, 256, C_BLACK, 0);
	expect_area(s, &img, 0, 112, 256, 144, C_BLACK, 0);
	expect_area(s, &img, 0, 0, 256, 16, C_BLACK, 0);
	expect_area(s, &img, 0, 240, 256, 256, C_BLACK, 0);
out:
	if (g_dev)
		IDirect3DDevice9_SetStreamSource(g_dev, 0, NULL, 0, 0);
	RELEASE(IDirect3DVertexBuffer9, vb);
	image_free(&img);
}

struct shader_quad {
	int x0, y0, x1, y1;
	float vs_color[4]; /* vertex shader c4 -> oD0 */
	float ps_color[4]; /* pixel shader c0, added to v0 */
	int ps11;	   /* ps_1_1 instead of ps_2_0 */
	D3DCOLOR want;
};

/*
 * c0..c3 map the unit square (v0.xy in [0,1]) to the pixel rectangle in clip
 * space for a BB_W x BB_H viewport; c4 is the colour.
 */
static void shader_vs_consts(float c[20], const struct shader_quad *q)
{
	memset(c, 0, 20 * sizeof(float));
	c[0] = (float)(q->x1 - q->x0) / (BB_W / 2.0f);
	c[3] = (float)q->x0 / (BB_W / 2.0f) - 1.0f;
	c[5] = -(float)(q->y1 - q->y0) / (BB_H / 2.0f);
	c[7] = 1.0f - (float)q->y0 / (BB_H / 2.0f);
	c[11] = 0.5f;
	c[15] = 1.0f;
	memcpy(&c[16], q->vs_color, 4 * sizeof(float));
}

static void scene_shaders(struct scene *s)
{
	static const D3DVERTEXELEMENT9 decl_elems[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
		  D3DDECLUSAGE_POSITION, 0 },
		D3DDECL_END(),
	};
	static const float unit_quad[4][3] = {
		{ 0.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f },
	};
	static const struct shader_quad quads[4] = {
		{ 16, 16, 112, 112, { 1.0f, 0.0f, 0.0f, 1.0f },
		  { 0.0f, 128.0f / 255.0f, 0.0f, 0.0f }, 0,
		  D3DCOLOR_XRGB(0xff, 0x80, 0x00) },
		{ 144, 16, 240, 112, { 0.0f, 0.0f, 1.0f, 1.0f },
		  { 64.0f / 255.0f, 0.0f, 0.0f, 0.0f }, 0,
		  D3DCOLOR_XRGB(0x40, 0x00, 0xff) },
		{ 16, 144, 112, 240, { 0.0f, 1.0f, 0.0f, 1.0f },
		  { 192.0f / 255.0f, 0.0f, 32.0f / 255.0f, 0.0f }, 1,
		  D3DCOLOR_XRGB(0xc0, 0xff, 0x20) },
		{ 144, 144, 240, 240, { 1.0f, 1.0f, 1.0f, 1.0f },
		  { -1.0f, -1.0f, 0.0f, 0.0f }, 1,
		  D3DCOLOR_XRGB(0x00, 0x00, 0xff) },
	};
	IDirect3DVertexDeclaration9 *decl = NULL;
	IDirect3DVertexShader9 *vs = NULL;
	IDirect3DPixelShader9 *ps20 = NULL, *ps11 = NULL;
	struct image img = { 0 };
	D3DCAPS9 caps;
	float c[20];
	size_t i;

	TRY(s, IDirect3DDevice9_GetDeviceCaps(g_dev, &caps));
	if (caps.VertexShaderVersion < D3DVS_VERSION(1, 1) ||
	    caps.PixelShaderVersion < D3DPS_VERSION(2, 0)) {
		fail(s, "caps report vs %lx ps %lx, need vs_1_1 and ps_2_0",
		     (unsigned long)caps.VertexShaderVersion,
		     (unsigned long)caps.PixelShaderVersion);
		goto out;
	}
	TRY(s, IDirect3DDevice9_CreateVertexDeclaration(g_dev, decl_elems,
							&decl));
	TRY(s, IDirect3DDevice9_CreateVertexShader(
		       g_dev, smoke_vs11_xform_color, &vs));
	TRY(s, IDirect3DDevice9_CreatePixelShader(g_dev, smoke_ps20_add,
						  &ps20));
	TRY(s, IDirect3DDevice9_CreatePixelShader(g_dev, smoke_ps11_add,
						  &ps11));

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DDevice9_SetVertexDeclaration(g_dev, decl));
	TRY(s, IDirect3DDevice9_SetVertexShader(g_dev, vs));
	for (i = 0; i < sizeof(quads) / sizeof(quads[0]); i++) {
		shader_vs_consts(c, &quads[i]);
		TRY(s, IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, c,
								 5));
		TRY(s, IDirect3DDevice9_SetPixelShaderConstantF(
			       g_dev, 0, quads[i].ps_color, 1));
		TRY(s, IDirect3DDevice9_SetPixelShader(
			       g_dev, quads[i].ps11 ? ps11 : ps20));
		TRY(s, IDirect3DDevice9_DrawPrimitiveUP(
			       g_dev, D3DPT_TRIANGLESTRIP, 2, unit_quad,
			       sizeof(unit_quad[0])));
	}
	TRY(s, end_scene());
	TRY(s, IDirect3DDevice9_SetVertexShader(g_dev, NULL));
	TRY(s, IDirect3DDevice9_SetPixelShader(g_dev, NULL));
	if (!read_back_buffer(s, &img))
		goto out;
	for (i = 0; i < sizeof(quads) / sizeof(quads[0]); i++)
		expect_area(s, &img, quads[i].x0 + 4, quads[i].y0 + 4,
			    quads[i].x1 - 4, quads[i].y1 - 4, quads[i].want,
			    0);
	expect_area(s, &img, 120, 0, 136, 256, C_BLACK, 0);
	expect_area(s, &img, 0, 120, 256, 136, C_BLACK, 0);
out:
	if (g_dev) {
		IDirect3DDevice9_SetVertexShader(g_dev, NULL);
		IDirect3DDevice9_SetPixelShader(g_dev, NULL);
		IDirect3DDevice9_SetFVF(g_dev, FVF_C);
	}
	RELEASE(IDirect3DPixelShader9, ps11);
	RELEASE(IDirect3DPixelShader9, ps20);
	RELEASE(IDirect3DVertexShader9, vs);
	RELEASE(IDirect3DVertexDeclaration9, decl);
	image_free(&img);
}

#define RTT_SIZE 64
#define C_RTT_CLEAR D3DCOLOR_XRGB(0x20, 0x40, 0x80)
#define C_RTT_QUAD D3DCOLOR_XRGB(0xf0, 0xc0, 0x10)
#define C_RTT_CORNER D3DCOLOR_XRGB(0x10, 0xf0, 0xf0)

static D3DCOLOR rtt_texel(int x, int y)
{
	if (x >= 16 && x < 48 && y >= 16 && y < 48)
		return C_RTT_QUAD;
	if (x < 8 && y < 8)
		return C_RTT_CORNER;
	return C_RTT_CLEAR;
}

static void scene_rtt(struct scene *s)
{
	IDirect3DTexture9 *tex = NULL;
	IDirect3DSurface9 *rt = NULL, *bb = NULL;
	struct image img = { 0 };
	int x, y;

	TRY(s, IDirect3DDevice9_CreateTexture(
		       g_dev, RTT_SIZE, RTT_SIZE, 1, D3DUSAGE_RENDERTARGET,
		       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL));
	TRY(s, IDirect3DTexture9_GetSurfaceLevel(tex, 0, &rt));
	TRY(s, IDirect3DDevice9_GetRenderTarget(g_dev, 0, &bb));

	/* Pass 1: render into the texture. */
	TRY(s, IDirect3DDevice9_SetRenderTarget(g_dev, 0, rt));
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET,
				      C_RTT_CLEAR, 1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, draw_rect_c(16, 16, 48, 48, 0.5f, C_RTT_QUAD));
	TRY(s, draw_rect_c(0, 0, 8, 8, 0.5f, C_RTT_CORNER));
	TRY(s, end_scene());
	TRY(s, IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb));

	/* The texture itself, read back directly. */
	if (!read_surface(s, rt, &img))
		goto out;
	if (img.w != RTT_SIZE || img.h != RTT_SIZE) {
		fail(s, "render-target texture is %ux%u", img.w, img.h);
		goto out;
	}
	for (y = 0; y < RTT_SIZE; y++)
		for (x = 0; x < RTT_SIZE; x++)
			if (!expect_px(s, &img, x, y, rtt_texel(x, y), 0))
				goto out;

	/* Pass 2: draw it to the back buffer, 2x2 pixels per texel. */
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)tex, D3DTADDRESS_CLAMP));
	TRY(s, draw_rect_t(0, 0, 2 * RTT_SIZE, 2 * RTT_SIZE, 0.0f, 0.0f, 1.0f,
			   1.0f));
	TRY(s, end_scene());
	TRY(s, stage_diffuse());
	if (!read_back_buffer(s, &img))
		goto out;
	for (y = 0; y < 2 * RTT_SIZE; y++)
		for (x = 0; x < 2 * RTT_SIZE; x++)
			if (!expect_px(s, &img, x, y, rtt_texel(x / 2, y / 2),
				       0))
				goto out;
	expect_area(s, &img, 2 * RTT_SIZE, 0, BB_W, BB_H, C_BLACK, 0);
	expect_area(s, &img, 0, 2 * RTT_SIZE, 2 * RTT_SIZE, BB_H, C_BLACK, 0);
out:
	if (g_dev) {
		IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
		if (bb)
			IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb);
	}
	RELEASE(IDirect3DSurface9, bb);
	RELEASE(IDirect3DSurface9, rt);
	RELEASE(IDirect3DTexture9, tex);
	image_free(&img);
}

#define C_NEAR D3DCOLOR_XRGB(0x00, 0x00, 0xff)
#define C_FAR D3DCOLOR_XRGB(0xff, 0x00, 0x00)
#define C_BLEND D3DCOLOR_ARGB(0x80, 0xff, 0xff, 0xff)

static void scene_depth_blend(struct scene *s)
{
	struct image img = { 0 };

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL,
				      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
				      C_BLACK, 1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
					       D3DZB_TRUE));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZWRITEENABLE,
					       TRUE));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZFUNC,
					       D3DCMP_LESSEQUAL));
	/* Near first, far second: the overlap must stay near. */
	TRY(s, draw_rect_c(32, 32, 160, 160, 0.25f, C_NEAR));
	TRY(s, draw_rect_c(96, 96, 224, 224, 0.75f, C_FAR));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
					       D3DZB_FALSE));
	/* 50% white over black and over the far (red) quad. */
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE,
					       TRUE));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SRCBLEND,
					       D3DBLEND_SRCALPHA));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DESTBLEND,
					       D3DBLEND_INVSRCALPHA));
	TRY(s, draw_rect_c(16, 176, 240, 240, 0.5f, C_BLEND));
	TRY(s, IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE,
					       FALSE));
	TRY(s, end_scene());
	if (!read_back_buffer(s, &img))
		goto out;
	/* Depth: exact. */
	expect_area(s, &img, 36, 36, 92, 92, C_NEAR, 0);
	expect_area(s, &img, 100, 100, 156, 156, C_NEAR, 0);
	expect_area(s, &img, 164, 100, 220, 172, C_FAR, 0);
	expect_area(s, &img, 100, 164, 156, 172, C_FAR, 0);
	expect_area(s, &img, 168, 16, 256, 88, C_BLACK, 0);
	/*
	 * Blend: src alpha 128/255 gives 255 * 128/255 = 128 over black and
	 * 128 + 255 * 127/255 = 255 over red; blender rounding is
	 * implementation-defined, so +-1 per channel.
	 */
	expect_area(s, &img, 20, 180, 92, 236, D3DCOLOR_XRGB(0x80, 0x80, 0x80),
		    1);
	expect_area(s, &img, 100, 180, 220, 220,
		    D3DCOLOR_XRGB(0xff, 0x80, 0x80), 1);
	expect_area(s, &img, 100, 228, 236, 236,
		    D3DCOLOR_XRGB(0x80, 0x80, 0x80), 1);
	expect_area(s, &img, 0, 244, 256, 256, C_BLACK, 0);
out:
	if (g_dev) {
		IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE,
						FALSE);
		IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE,
						D3DZB_FALSE);
	}
	image_free(&img);
}

static void scene_query(struct scene *s)
{
	IDirect3DQuery9 *ev = NULL, *occ = NULL;
	struct image img = { 0 };
	BOOL done = FALSE;
	DWORD samples = 0;

	/* Probe form (NULL out) reports support without creating a query. */
	TRY(s, IDirect3DDevice9_CreateQuery(g_dev, D3DQUERYTYPE_EVENT, NULL));
	TRY(s, IDirect3DDevice9_CreateQuery(g_dev, D3DQUERYTYPE_EVENT, &ev));
	TRY(s, IDirect3DDevice9_CreateQuery(g_dev, D3DQUERYTYPE_OCCLUSION,
					    &occ));
	if (IDirect3DQuery9_GetType(ev) != D3DQUERYTYPE_EVENT ||
	    IDirect3DQuery9_GetDataSize(ev) != sizeof(BOOL) ||
	    IDirect3DQuery9_GetType(occ) != D3DQUERYTYPE_OCCLUSION ||
	    IDirect3DQuery9_GetDataSize(occ) != sizeof(DWORD)) {
		fail(s, "query type/size: event %u/%lu occlusion %u/%lu",
		     (unsigned)IDirect3DQuery9_GetType(ev),
		     (unsigned long)IDirect3DQuery9_GetDataSize(ev),
		     (unsigned)IDirect3DQuery9_GetType(occ),
		     (unsigned long)IDirect3DQuery9_GetDataSize(occ));
		goto out;
	}

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK,
				      1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, IDirect3DQuery9_Issue(occ, D3DISSUE_BEGIN));
	TRY(s, draw_rect_c(16, 16, 48, 48, 0.5f, C_WHITE));
	TRY(s, IDirect3DQuery9_Issue(occ, D3DISSUE_END));
	TRY(s, end_scene());
	TRY(s, IDirect3DQuery9_Issue(ev, D3DISSUE_END));

	if (!wait_query(s, ev, &done, sizeof(done), "event query"))
		goto out;
	if (done != TRUE) {
		fail(s, "event query data %ld, expected TRUE", (long)done);
		goto out;
	}
	if (!wait_query(s, occ, &samples, sizeof(samples), "occlusion query"))
		goto out;
	if (samples != 32 * 32) {
		fail(s, "occlusion query counted %lu samples, expected %u",
		     (unsigned long)samples, 32 * 32);
		goto out;
	}
	if (!read_back_buffer(s, &img))
		goto out;
	expect_area(s, &img, 16, 16, 48, 48, C_WHITE, 0);
	expect_area(s, &img, 48, 0, 256, 256, C_BLACK, 0);
out:
	RELEASE(IDirect3DQuery9, occ);
	RELEASE(IDirect3DQuery9, ev);
	image_free(&img);
}

#define C_RESET D3DCOLOR_XRGB(0x5a, 0x3c, 0x1e)

static void scene_reset(struct scene *s)
{
	static const DWORD mt_texels[4] = {
		D3DCOLOR_ARGB(0xff, 0x11, 0xaa, 0x33),
		D3DCOLOR_ARGB(0xff, 0xcc, 0x22, 0x88),
		D3DCOLOR_ARGB(0xff, 0x33, 0x55, 0xee),
		D3DCOLOR_ARGB(0xff, 0xee, 0xdd, 0x44),
	};
	IDirect3DTexture9 *mt = NULL;
	IDirect3DSurface9 *surf = NULL;
	D3DPRESENT_PARAMETERS pp;
	D3DSURFACE_DESC desc;
	D3DVIEWPORT9 vp;
	struct image img = { 0 };
	int x, y;

	/* A managed texture must survive Reset with its contents. */
	TRY(s, IDirect3DDevice9_CreateTexture(g_dev, 2, 2, 1, 0,
					      D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
					      &mt, NULL));
	if (!upload_argb(s, mt, mt_texels, 2, 2, NULL))
		goto out;

	pp = g_pp;
	pp.BackBufferWidth = RESET_W;
	pp.BackBufferHeight = RESET_H;
	TRY(s, IDirect3DDevice9_Reset(g_dev, &pp));
	g_pp = pp;
	base_state(s);
	if (s->failures)
		goto out;

	TRY(s, IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0,
					      D3DBACKBUFFER_TYPE_MONO, &surf));
	TRY(s, IDirect3DSurface9_GetDesc(surf, &desc));
	RELEASE(IDirect3DSurface9, surf);
	if (desc.Width != RESET_W || desc.Height != RESET_H ||
	    desc.Format != D3DFMT_X8R8G8B8) {
		fail(s, "back buffer after Reset is %ux%u format %u",
		     desc.Width, desc.Height, (unsigned)desc.Format);
		goto out;
	}
	TRY(s, IDirect3DDevice9_GetDepthStencilSurface(g_dev, &surf));
	TRY(s, IDirect3DSurface9_GetDesc(surf, &desc));
	RELEASE(IDirect3DSurface9, surf);
	if (desc.Width != RESET_W || desc.Height != RESET_H ||
	    desc.Format != D3DFMT_D24S8) {
		fail(s, "depth-stencil after Reset is %ux%u format %u",
		     desc.Width, desc.Height, (unsigned)desc.Format);
		goto out;
	}
	TRY(s, IDirect3DDevice9_GetViewport(g_dev, &vp));
	if (vp.X != 0 || vp.Y != 0 || vp.Width != RESET_W ||
	    vp.Height != RESET_H || vp.MinZ != 0.0f || vp.MaxZ != 1.0f) {
		fail(s, "viewport after Reset is %lu,%lu %lux%lu",
		     (unsigned long)vp.X, (unsigned long)vp.Y,
		     (unsigned long)vp.Width, (unsigned long)vp.Height);
		goto out;
	}

	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL,
				      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
					      D3DCLEAR_STENCIL,
				      C_RESET, 1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)mt, D3DTADDRESS_CLAMP));
	TRY(s, draw_rect_t(64, 64, 128, 128, 0.0f, 0.0f, 1.0f, 1.0f));
	TRY(s, end_scene());
	TRY(s, stage_diffuse());
	if (!read_back_buffer(s, &img))
		goto out;
	expect_area(s, &img, 0, 0, RESET_W, 64, C_RESET, 0);
	expect_area(s, &img, 0, 64, 64, RESET_H, C_RESET, 0);
	for (y = 0; y < 2; y++)
		for (x = 0; x < 2; x++)
			expect_area(s, &img, 64 + 32 * x + 4, 64 + 32 * y + 4,
				    64 + 32 * x + 28, 64 + 32 * y + 28,
				    mt_texels[y * 2 + x], 0);
out:
	if (g_dev)
		IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
	RELEASE(IDirect3DSurface9, surf);
	RELEASE(IDirect3DTexture9, mt);
	image_free(&img);
}

static void note_present(HRESULT hr)
{
	g_presents++;
	if (hr == D3D_OK)
		return;
	if (g_present_failures++ == 0)
		g_first_present_hr = hr;
}

#define C_PRESENT D3DCOLOR_XRGB(0x7f, 0x1f, 0x5f)

/*
 * Present is also called after every other scene; this scene adds a burst
 * through the device and the implicit swap chain, then checks the device
 * still renders and that every Present so far returned D3D_OK.
 */
static void scene_present(struct scene *s)
{
	IDirect3DSwapChain9 *sc = NULL;
	IDirect3DSurface9 *surf = NULL;
	D3DPRESENT_PARAMETERS pp;
	struct image img = { 0 };
	int i;

	TRY(s, IDirect3DDevice9_TestCooperativeLevel(g_dev));
	TRY(s, IDirect3DDevice9_GetSwapChain(g_dev, 0, &sc));
	TRY(s, IDirect3DSwapChain9_GetPresentParameters(sc, &pp));
	if (pp.BackBufferWidth != g_pp.BackBufferWidth ||
	    pp.BackBufferHeight != g_pp.BackBufferHeight) {
		fail(s, "swap chain reports %ux%u, expected %ux%u",
		     pp.BackBufferWidth, pp.BackBufferHeight,
		     g_pp.BackBufferWidth, g_pp.BackBufferHeight);
		goto out;
	}
	for (i = 0; i < PRESENT_BURST; i++) {
		TRY(s, IDirect3DDevice9_Clear(
			       g_dev, 0, NULL, D3DCLEAR_TARGET,
			       D3DCOLOR_XRGB(0x20 * i, 0xff - 0x20 * i, 0x80),
			       1.0f, 0));
		if (i & 1)
			note_present(IDirect3DSwapChain9_Present(sc, NULL, NULL,
								 NULL, NULL,
								 0));
		else
			note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL,
							      NULL, NULL));
	}
	if (g_present_failures) {
		fail(s, "%u of %u Present calls failed, first 0x%08lx",
		     g_present_failures, g_presents,
		     (unsigned long)g_first_present_hr);
		goto out;
	}
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET,
				      C_PRESENT, 1.0f, 0));
	/* Read through the swap chain's own back-buffer getter. */
	TRY(s, IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO,
						 &surf));
	if (!read_surface(s, surf, &img))
		goto out;
	if (img.w != g_pp.BackBufferWidth || img.h != g_pp.BackBufferHeight) {
		fail(s, "swap chain back buffer is %ux%u", img.w, img.h);
		goto out;
	}
	expect_area(s, &img, 0, 0, (int)img.w, (int)img.h, C_PRESENT, 0);
out:
	RELEASE(IDirect3DSurface9, surf);
	RELEASE(IDirect3DSwapChain9, sc);
	image_free(&img);
}

/* ------------------------------------------------------------------ */
/* Setup, driver                                                       */

static DWORD WINAPI watchdog(LPVOID arg)
{
	if (WaitForSingleObject(g_finished, WATCHDOG_MS) == WAIT_TIMEOUT) {
		printf("FAIL watchdog: not finished within %u ms\n",
		       WATCHDOG_MS);
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

static void print_module(void)
{
	char path[MAX_PATH];
	HMODULE m = GetModuleHandleA("d3d9.dll");

	if (m && GetModuleFileNameA(m, path, sizeof(path)))
		printf("INFO d3d9: %s\n", path);
}

/* The cursor scene's window shows the device cursor or a Windows cursor
 * over its client area the way the DirectX documentation has applications
 * do it: WM_SETCURSOR shows it instead of the class cursor. */
static int g_cursor_d3d;
static HCURSOR g_cursor_win;

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
		if (g_cursor_d3d && g_dev) {
			IDirect3DDevice9_ShowCursor(g_dev, TRUE);
			return TRUE;
		}
		if (g_cursor_win) {
			SetCursor(g_cursor_win);
			return TRUE;
		}
	}
	return DefWindowProcA(w, m, wp, lp);
}

static int setup(void)
{
	static const char cls[] = "qd3d9_smoke";
	WNDCLASSA wc;
	RECT rc = { 0, 0, BB_W, BB_H };
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
	D3DADAPTER_IDENTIFIER9 id;
	HRESULT hr;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	wc.lpszClassName = cls;
	if (!RegisterClassA(&wc)) {
		snprintf(g_setup_error, sizeof(g_setup_error),
			 "RegisterClass error %lu",
			 (unsigned long)GetLastError());
		return 0;
	}
	AdjustWindowRect(&rc, style, FALSE);
	g_hwnd = CreateWindowA(cls, "qd3d9_smoke", style, CW_USEDEFAULT,
			       CW_USEDEFAULT, rc.right - rc.left,
			       rc.bottom - rc.top, NULL, NULL, wc.hInstance,
			       NULL);
	if (g_hwnd == NULL) {
		snprintf(g_setup_error, sizeof(g_setup_error),
			 "CreateWindow error %lu",
			 (unsigned long)GetLastError());
		return 0;
	}
	ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
	pump_messages();

	g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
	print_module();
	if (g_d3d == NULL) {
		snprintf(g_setup_error, sizeof(g_setup_error),
			 "Direct3DCreate9 returned NULL");
		return 0;
	}
	if (IDirect3D9_GetAdapterIdentifier(g_d3d, D3DADAPTER_DEFAULT, 0,
					    &id) == D3D_OK) {
		id.Description[sizeof(id.Description) - 1] = '\0';
		printf("INFO adapter: %s\n", id.Description);
	}

	memset(&g_pp, 0, sizeof(g_pp));
	g_pp.BackBufferWidth = BB_W;
	g_pp.BackBufferHeight = BB_H;
	g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	g_pp.BackBufferCount = 1;
	g_pp.MultiSampleType = D3DMULTISAMPLE_NONE;
	g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	g_pp.hDeviceWindow = g_hwnd;
	g_pp.Windowed = TRUE;
	g_pp.EnableAutoDepthStencil = TRUE;
	g_pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	/* Default control word with invalid operation and zero divide
	 * unmasked, 53-bit precision: CreateDevice must mask and narrow it. */
	fpu_cw_set(0x027a);
	hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
				     g_hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
				     &g_pp, &g_dev);
	g_fpu_cw = fpu_cw_get();
	fpu_cw_set(g_fpu_cw | 0x3f); /* later scenes never trap */
	if (hr == D3D_OK) {
		printf("INFO vertex processing: hardware\n");
		return 1;
	}
	printf("INFO hardware vertex processing failed 0x%08lx, trying software\n",
	       (unsigned long)hr);
	g_pp.BackBufferWidth = BB_W;
	g_pp.BackBufferHeight = BB_H;
	hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
				     g_hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
				     &g_pp, &g_dev);
	if (hr == D3D_OK) {
		printf("INFO vertex processing: software\n");
		return 1;
	}
	g_dev = NULL;
	snprintf(g_setup_error, sizeof(g_setup_error),
		 "CreateDevice returned 0x%08lx", (unsigned long)hr);
	return 0;
}

static void teardown(void)
{
	if (g_dev) {
		ULONG left = IDirect3DDevice9_Release(g_dev);

		g_dev = NULL;
		printf("INFO device references after release: %lu\n",
		       (unsigned long)left);
	}
	RELEASE(IDirect3D9, g_d3d);
	if (g_hwnd) {
		DestroyWindow(g_hwnd);
		g_hwnd = NULL;
	}
	pump_messages();
}

/* CreateDevice without D3DCREATE_FPU_PRESERVE masks every x87 exception and
 * selects single precision, round-to-nearest (g_fpu_cw, see setup). */
static void scene_fpu_setup(struct scene *s)
{
	if ((g_fpu_cw & 0x3f) != 0x3f || (g_fpu_cw & 0xf00) != 0)
		fail(s, "x87 control word %04x after CreateDevice, expected masks 3f, single precision, nearest",
		     (unsigned)g_fpu_cw);
}

/* Private data names an object without touching a released pointer. */
static const GUID g_tag = { 0x6f1d2c7a, 0x41b3, 0x4e88, { 0x9a, 0x0e, 0x52, 0xd4, 0x17, 0x8c, 0x33, 0x01 } };

static void tag_set(void *res, DWORD v)
{
	IDirect3DResource9_SetPrivateData((IDirect3DResource9 *)res, &g_tag, &v, sizeof(v), 0);
}

static DWORD tag_get(void *res)
{
	DWORD v = 0, n = sizeof(v);

	if (FAILED(IDirect3DResource9_GetPrivateData((IDirect3DResource9 *)res, &g_tag, &v, &n)))
		return 0;
	return v;
}

static ULONG dev_refs(void)
{
	IDirect3DDevice9_AddRef(g_dev);
	return IDirect3DDevice9_Release(g_dev);
}

/* Native lifetime rules games rely on (Need for Speed: Underground 2 releases
 * bound objects twice when its video options change): an object released
 * while bound stays alive and Get* returns it, without the device reference
 * a live resource holds; a level surface lives as long as its texture; the
 * device owns its automatic depth stencil. */
static void scene_bound_lifetime(struct scene *s)
{
	IDirect3DTexture9 *tex = NULL;
	IDirect3DBaseTexture9 *bt = NULL;
	IDirect3DSurface9 *lvl = NULL, *ds = NULL;
	ULONG before, r;

	before = dev_refs();
	if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8,
						  D3DPOOL_MANAGED, &tex, NULL))) {
		fail(s, "CreateTexture failed");
		return;
	}
	tag_set(tex, 0x7e57u);
	IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)tex);
	r = IDirect3DTexture9_Release(tex);
	if (r != 0)
		fail(s, "Release of the bound texture returned %lu, expected 0", (unsigned long)r);
	if (dev_refs() != before)
		fail(s, "device count %lu with the released texture bound, expected %lu",
		     (unsigned long)dev_refs(), (unsigned long)before);
	if (FAILED(IDirect3DDevice9_GetTexture(g_dev, 0, &bt)) || !bt) {
		fail(s, "GetTexture after release returned nothing");
	} else {
		if (tag_get(bt) != 0x7e57u)
			fail(s, "GetTexture returned another object than the released bound texture");
		if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)bt, 0, &lvl))) {
			tag_set(lvl, 0x1e7e1u);
			IDirect3DSurface9_Release(lvl);
			lvl = NULL;
			if (FAILED(IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)bt, 0, &lvl)) ||
			    tag_get(lvl) != 0x1e7e1u)
				fail(s, "GetSurfaceLevel returned another surface after its release");
			if (lvl)
				IDirect3DSurface9_Release(lvl);
		}
		IDirect3DBaseTexture9_Release(bt);
	}
	IDirect3DDevice9_SetTexture(g_dev, 0, NULL);

	if (SUCCEEDED(IDirect3DDevice9_GetDepthStencilSurface(g_dev, &ds)) && ds) {
		tag_set(ds, 0xd5u);
		IDirect3DSurface9_Release(ds);
		ds = NULL;
		if (FAILED(IDirect3DDevice9_GetDepthStencilSurface(g_dev, &ds)) || !ds || tag_get(ds) != 0xd5u)
			fail(s, "GetDepthStencilSurface returned another surface after its release");
		if (ds)
			IDirect3DSurface9_Release(ds);
	}
}

/* ------------------------------------------------------------------ */
/* Cursor, managed LOD, additional swap chains, front buffer           */

#define C_CURSOR_BG D3DCOLOR_XRGB(0x20, 0x40, 0x60)
#define C_CURSOR_BG2 D3DCOLOR_XRGB(0x60, 0x30, 0x10)
#define C_CURSOR D3DCOLOR_ARGB(0xff, 0xff, 0x00, 0xff)
#define CURSOR_HX 4
#define CURSOR_HY 6
/* Client point the window is moved under the mouse with. */
#define CURSOR_AT_X 100
#define CURSOR_AT_Y 90

/* 1 when the IDirect3D9 is a QD3D session (else the stock runtime). */
static int qd3d_session(void)
{
	IUnknown *u = NULL;

	if (FAILED(IDirect3D9_QueryInterface(g_d3d, &IID_QD3D_REMOTE, (void **)&u)) || !u)
		return 0;
	u->lpVtbl->Release(u);
	return 1;
}

/* Pumps messages until the shown cursor has hot spot (hx, hy) and width w
 * (GetCursorInfo; the window under the mouse decides it). */
static int wait_cursor(UINT hx, UINT hy, LONG w, DWORD ms)
{
	DWORD t0 = GetTickCount();

	do {
		CURSORINFO ci;
		ICONINFO ii;
		BITMAP bm;

		pump_messages();
		memset(&ci, 0, sizeof(ci));
		ci.cbSize = sizeof(ci);
		if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor &&
		    GetIconInfo(ci.hCursor, &ii)) {
			int ok = ii.xHotspot == hx && ii.yHotspot == hy &&
				 GetObjectA(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth == w;

			DeleteObject(ii.hbmMask);
			if (ii.hbmColor)
				DeleteObject(ii.hbmColor);
			if (ok)
				return 1;
		}
		Sleep(10);
	} while (GetTickCount() - t0 < ms);
	return 0;
}

/* A complete 32-bit BMP the loopback harness wrote, top-down into img: 1,
 * else 0 (absent or still being written). */
static int read_bmp(const char *path, struct image *img)
{
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	FILE *f;
	long size = 0;
	LONG y;
	int ok;

	image_free(img);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	ok = fread(&fh, sizeof(fh), 1, f) == 1 && fread(&ih, sizeof(ih), 1, f) == 1 && fh.bfType == 0x4d42 &&
	     ih.biBitCount == 32 && ih.biWidth > 0 && ih.biHeight > 0 && ih.biWidth <= 4096 && ih.biHeight <= 4096 &&
	     fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 &&
	     (unsigned long)size >= fh.bfOffBits + (unsigned long)ih.biWidth * ih.biHeight * 4u &&
	     (img->px = malloc((size_t)ih.biWidth * ih.biHeight * 4u)) != NULL;
	if (ok) {
		img->w = (UINT)ih.biWidth;
		img->h = (UINT)ih.biHeight;
		for (y = 0; y < ih.biHeight && ok; y++) /* bottom-up */
			ok = fseek(f, (long)(fh.bfOffBits + (DWORD)y * img->w * 4u), SEEK_SET) == 0 &&
			     fread(img->px + (size_t)(ih.biHeight - 1 - y) * img->w, 4, img->w, f) == img->w;
	}
	fclose(f);
	if (!ok)
		image_free(img);
	return ok;
}

/* Frame n as the loopback harness dumped it (run-loopback.ps1 names the
 * directory in QD3D_FRAME_DUMPS): 1, 0 without dumps, -1 (failed) when the
 * file does not appear complete within 5 s. */
static int read_dump(struct scene *s, unsigned n, struct image *img)
{
	char dir[MAX_PATH], path[MAX_PATH + 32];
	DWORD t0 = GetTickCount();

	image_free(img);
	if (!GetEnvironmentVariableA("QD3D_FRAME_DUMPS", dir, sizeof(dir)) || !dir[0])
		return 0;
	snprintf(path, sizeof(path), "%s\\frame_%06u.bmp", dir, n);
	while (!read_bmp(path, img)) {
		if (GetTickCount() - t0 > 5000u) {
			fail(s, "frame dump %s missing or incomplete", path);
			return -1;
		}
		Sleep(20);
	}
	return 1;
}

typedef BOOL(WINAPI *cycle_time_fn)(HANDLE, PULONG64);

/* CPU load of the process's other threads (the proxy's cursor poller; the
 * watchdog only waits) over about ms while this thread sleeps or, with co,
 * moves the mouse between points of the client area at co every 10 ms:
 * their cycles (QueryProcessCycleTime minus this thread's
 * QueryThreadCycleTime, Windows Vista and later) in percent of one core,
 * calibrated by a 20 ms busy spin of this thread. -1 without cycle
 * counters (Windows XP). */
static double other_threads_load(DWORD ms, const POINT *co)
{
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	cycle_time_fn process = (cycle_time_fn)(void (*)(void))GetProcAddress(k32, "QueryProcessCycleTime");
	cycle_time_fn thread = (cycle_time_fn)(void (*)(void))GetProcAddress(k32, "QueryThreadCycleTime");
	ULONG64 c0 = 0, c1 = 0, p0 = 0, p1 = 0, t0 = 0, t1 = 0;
	LARGE_INTEGER f, a, b;
	double core;
	DWORD i;

	if (!process || !thread || !QueryPerformanceFrequency(&f))
		return -1.0;
	QueryPerformanceCounter(&a);
	thread(GetCurrentThread(), &c0);
	do
		QueryPerformanceCounter(&b);
	while ((b.QuadPart - a.QuadPart) * 50 < f.QuadPart);
	thread(GetCurrentThread(), &c1);
	core = (double)(c1 - c0) * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart);
	process(GetCurrentProcess(), &p0);
	thread(GetCurrentThread(), &t0);
	QueryPerformanceCounter(&a);
	for (i = 0; i < ms / 10u; i++) {
		if (co)
			SetCursorPos(co->x + 40 + (int)(i % 2u) * 100, co->y + 40 + (int)(i % 3u) * 60);
		Sleep(10);
	}
	process(GetCurrentProcess(), &p1);
	thread(GetCurrentThread(), &t1);
	QueryPerformanceCounter(&b);
	core = 100.0 * ((double)(p1 - p0) - (double)(t1 - t0)) * (double)f.QuadPart /
	       (core * (double)(b.QuadPart - a.QuadPart));
	return core > 0.0 ? core : 0.0; /* reading order makes idle slightly negative */
}

/*
 * The device cursor is a Windows cursor, as natively: hidden without a
 * bitmap, the bitmap the Windows cursor once shown (alpha 255 opaque,
 * anything else transparent), no cursor after ShowCursor(FALSE), sizes
 * powers of two. Through QD3D the cursor the guest shows is drawn over the
 * published frames (loopback: the harness's frame dumps) where the guest's
 * screen puts it, and never into the back buffer or front buffer. This is
 * the first scene, so its frames are the run's first two: the device
 * cursor, then a monochrome Windows cursor (black, white, inverting and
 * transparent rows). The window moves under the mouse; the mouse does not
 * move (SetCursorPosition onto its own position leaves it).
 */
static void scene_cursor(struct scene *s)
{
	IDirect3DSurface9 *surf = NULL, *bad = NULL, *front = NULL;
	D3DDISPLAYMODE dm;
	D3DLOCKED_RECT lr;
	struct image img = { 0 };
	BYTE and_bits[32 * 4], xor_bits[32 * 4];
	unsigned first = g_presents + 1u;
	HCURSOR mono = NULL;
	CURSORINFO ci;
	POINT p, now, co = { 0, 0 };
	RECT wr;
	int observed, mono_shown = 0, x, y, locked = 0, dumped;
	double load;

	/* Measured before any present: no cursor poller thread exists yet. */
	load = other_threads_load(500, NULL);
	if (load >= 0.0)
		printf("INFO cursor: Measured: other threads %.3f%% of one core before the first present (no cursor "
		       "poller)\n",
		       load);
	if (IDirect3DDevice9_ShowCursor(g_dev, TRUE) != FALSE || IDirect3DDevice9_ShowCursor(g_dev, TRUE) != FALSE) {
		fail(s, "ShowCursor without a cursor bitmap reported a visible cursor");
		goto out;
	}
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf,
							    NULL));
	TRY(s, IDirect3DSurface9_LockRect(surf, &lr, NULL, 0));
	for (y = 0; y < 32; y++) {
		DWORD *row = (DWORD *)((BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch);

		for (x = 0; x < 32; x++)
			row[x] = x >= 12 && x < 20 && y >= 12 && y < 20 ? 0x00000000u
				 : x < 4 && y >= 28			    ? 0x80ff00ffu
									    : C_CURSOR;
	}
	TRY(s, IDirect3DSurface9_UnlockRect(surf));
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 48, 48, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &bad,
							    NULL));
	if (IDirect3DDevice9_SetCursorProperties(g_dev, 0, 0, bad) != D3DERR_INVALIDCALL ||
	    IDirect3DDevice9_SetCursorProperties(g_dev, 32, 0, surf) != D3DERR_INVALIDCALL) {
		fail(s, "SetCursorProperties accepted a 48x48 bitmap or a hot spot outside");
		goto out;
	}
	TRY(s, IDirect3DDevice9_SetCursorProperties(g_dev, CURSOR_HX, CURSOR_HY, surf));
	if (IDirect3DDevice9_ShowCursor(g_dev, TRUE) != FALSE || IDirect3DDevice9_ShowCursor(g_dev, TRUE) != TRUE) {
		fail(s, "ShowCursor(TRUE) with a bitmap did not show the cursor");
		goto out;
	}

	/* The window goes under the mouse, on top. */
	GetCursorPos(&p);
	GetWindowRect(g_hwnd, &wr);
	ClientToScreen(g_hwnd, &co);
	SetWindowPos(g_hwnd, HWND_TOPMOST, wr.left + p.x - CURSOR_AT_X - co.x, wr.top + p.y - CURSOR_AT_Y - co.y, 0, 0,
		     SWP_NOSIZE | SWP_NOACTIVATE);
	co.x = co.y = 0;
	ClientToScreen(g_hwnd, &co);
	g_cursor_d3d = 1;
	observed = wait_cursor(CURSOR_HX, CURSOR_HY, 32, 2000);
	GetCursorPos(&p);
	IDirect3DDevice9_SetCursorPosition(g_dev, p.x, p.y, 0);
	GetCursorPos(&now);
	if (now.x != p.x || now.y != p.y)
		fail(s, "SetCursorPosition onto the cursor's position moved it");
	if (observed && (p.x - co.x != CURSOR_AT_X || p.y - CURSOR_AT_Y != co.y)) {
		printf("INFO cursor: the mouse moved during the scene\n");
		observed = 0;
	}
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_CURSOR_BG, 1.0f, 0));
	note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL));
	/* Published before the cursor changes: a frame in flight would take a
	 * newer cursor along (the cursor follows the mouse between presents),
	 * as it does when the mouse moves meanwhile. */
	if (observed) {
		(void)read_dump(s, first, &img);
		GetCursorPos(&now);
		if (now.x != p.x || now.y != p.y) {
			printf("INFO cursor: the mouse moved during the scene\n");
			observed = 0;
		}
	}
	if (IDirect3DDevice9_ShowCursor(g_dev, FALSE) != TRUE)
		fail(s, "ShowCursor(FALSE) did not report the visible cursor");
	g_cursor_d3d = 0;
	memset(&ci, 0, sizeof(ci));
	ci.cbSize = sizeof(ci);
	if (observed && GetCursorInfo(&ci) && ci.hCursor && (ci.flags & CURSOR_SHOWING))
		fail(s, "a cursor (%p) shows after ShowCursor(FALSE)", (void *)ci.hCursor);

	/* A monochrome Windows cursor: rows 0-7 black, 8-15 white, 16-23
	 * inverting, 24-31 transparent; hot spot (1, 2). */
	for (y = 0; y < 32; y++) {
		memset(and_bits + y * 4, y >= 16 ? 0xff : 0x00, 4);
		memset(xor_bits + y * 4, y >= 8 && y < 24 ? 0xff : 0x00, 4);
	}
	mono = CreateCursor(GetModuleHandleA(NULL), 1, 2, 32, 32, and_bits, xor_bits);
	if (mono) {
		g_cursor_win = mono;
		SetCursor(mono);
		mono_shown = observed && wait_cursor(1, 2, 32, 2000);
	}
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_CURSOR_BG2, 1.0f, 0));
	note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL));
	if (mono_shown) {
		(void)read_dump(s, first + 1u, &img);
		GetCursorPos(&now);
		if (now.x != p.x || now.y != p.y) {
			printf("INFO cursor: the mouse moved during the scene\n");
			mono_shown = 0;
		}
	}
	g_cursor_win = NULL;
	SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_ARROW));

	if (!observed) {
		printf("INFO cursor: the cursor over the window could not be observed; composition not checked\n");
		goto out;
	}
	if (!qd3d_session())
		goto out;
	/* Not in the front buffer: the frame at its client area, no cursor. */
	TRY(s, IDirect3DDevice9_GetDisplayMode(g_dev, 0, &dm));
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, dm.Width, dm.Height, D3DFMT_A8R8G8B8,
							    D3DPOOL_SYSTEMMEM, &front, NULL));
	TRY(s, IDirect3DDevice9_GetFrontBufferData(g_dev, 0, front));
	TRY(s, IDirect3DSurface9_LockRect(front, &lr, NULL, D3DLOCK_READONLY));
	locked = 1;
	x = co.x + CURSOR_AT_X + 4;
	y = co.y + CURSOR_AT_Y + 2;
	if (x >= 0 && y >= 0 && (UINT)x < dm.Width && (UINT)y < dm.Height &&
	    (((const DWORD *)((const BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch))[x] & 0x00ffffffu) !=
		    (C_CURSOR_BG2 & 0x00ffffffu))
		fail(s, "front buffer under the cursor is %06lx, expected the frame's %06lx",
		     (unsigned long)(((const DWORD *)((const BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch))[x] &
				     0x00ffffffu),
		     (unsigned long)(C_CURSOR_BG2 & 0x00ffffffu));
	/* Over the published frames: the device cursor's top-left at the
	 * client point minus its hot spot, then the monochrome one. */
	dumped = read_dump(s, first, &img);
	if (dumped > 0) {
		x = CURSOR_AT_X - CURSOR_HX;
		y = CURSOR_AT_Y - CURSOR_HY;
		expect_px(s, &img, x, y, C_CURSOR, 0);
		expect_px(s, &img, x + 31, y + 31, C_CURSOR, 0);
		expect_px(s, &img, x + 15, y + 15, C_CURSOR_BG, 0);
		expect_px(s, &img, x + 1, y + 30, C_CURSOR_BG, 0);
		expect_px(s, &img, x - 1, y, C_CURSOR_BG, 0);
		expect_px(s, &img, x, y - 1, C_CURSOR_BG, 0);
		expect_px(s, &img, x + 32, y + 31, C_CURSOR_BG, 0);
		printf("INFO cursor: device cursor checked in frame dump %u\n", first);
	}
	if (dumped > 0 && mono_shown && read_dump(s, first + 1u, &img) > 0) {
		x = CURSOR_AT_X - 1;
		y = CURSOR_AT_Y - 2;
		expect_px(s, &img, x, y, D3DCOLOR_XRGB(0, 0, 0), 0);
		expect_px(s, &img, x + 31, y + 8, D3DCOLOR_XRGB(0xff, 0xff, 0xff), 0);
		expect_px(s, &img, x + 5, y + 16, C_CURSOR_BG2 ^ 0x00ffffffu, 0);
		expect_px(s, &img, x + 5, y + 24, C_CURSOR_BG2, 0);
		expect_px(s, &img, x - 1, y, C_CURSOR_BG2, 0);
		printf("INFO cursor: monochrome Windows cursor checked in frame dump %u\n", first + 1u);
	}
out:
	if (locked)
		IDirect3DSurface9_UnlockRect(front);
	g_cursor_d3d = 0;
	g_cursor_win = NULL;
	if (g_hwnd)
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (mono)
		DestroyCursor(mono);
	RELEASE(IDirect3DSurface9, front);
	RELEASE(IDirect3DSurface9, bad);
	RELEASE(IDirect3DSurface9, surf);
	image_free(&img);
}

/* Native managed-texture LOD (Wine's maxmip_test on the stock runtime): the
 * most detailed level sampled is max(MAXMIPLEVEL, LOD) with mip filtering,
 * the LOD level without; SetLOD clamps to the last level and answers the
 * previous LOD; sampler states read back as set. */
static void scene_setlod(struct scene *s)
{
	static const D3DCOLOR colors[3] = {
		D3DCOLOR_XRGB(0xff, 0x00, 0x00), D3DCOLOR_XRGB(0x00, 0xff, 0x00), D3DCOLOR_XRGB(0x00, 0x00, 0xff)
	};
	static const struct {
		DWORD lod, maxmip, filter, want_old;
		int level;
	} steps[] = {
		{ 0, 0, D3DTEXF_POINT, 0, 0 }, { 1, 0, D3DTEXF_POINT, 0, 1 }, { 1, 2, D3DTEXF_POINT, 1, 2 },
		{ 1, 2, D3DTEXF_NONE, 1, 1 },  { 7, 0, D3DTEXF_POINT, 1, 2 }, { 0, 1, D3DTEXF_NONE, 2, 0 },
	};
	IDirect3DTexture9 *tex = NULL;
	struct image img = { 0 };
	D3DLOCKED_RECT lr;
	DWORD v, old;
	UINT l, i, n;

	TRY(s, IDirect3DDevice9_CreateTexture(g_dev, 32, 32, 3, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex, NULL));
	for (l = 0; l < 3; l++) {
		TRY(s, IDirect3DTexture9_LockRect(tex, l, &lr, NULL, 0));
		for (n = 0; n < (32u >> l); n++)
			for (i = 0; i < (32u >> l); i++)
				((DWORD *)((BYTE *)lr.pBits + (size_t)n * (size_t)lr.Pitch))[i] = colors[l];
		TRY(s, IDirect3DTexture9_UnlockRect(tex, l));
	}
	TRY(s, stage_texture((IDirect3DBaseTexture9 *)tex, D3DTADDRESS_CLAMP));
	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		old = IDirect3DTexture9_SetLOD(tex, steps[i].lod);
		if (old != steps[i].want_old) {
			fail(s, "step %u: SetLOD(%lu) answered %lu, expected %lu", i, (unsigned long)steps[i].lod,
			     (unsigned long)old, (unsigned long)steps[i].want_old);
			goto out;
		}
		TRY(s, IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MIPFILTER, steps[i].filter));
		TRY(s, IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAXMIPLEVEL, steps[i].maxmip));
		TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_BLACK, 1.0f, 0));
		TRY(s, begin_scene());
		/* 64x64 pixels from a 32x32 level 0: magnified, so the most
		 * detailed level allowed is sampled. */
		TRY(s, draw_rect_t(32, 32, 96, 96, 0.0f, 0.0f, 1.0f, 1.0f));
		TRY(s, end_scene());
		if (!read_back_buffer(s, &img))
			goto out;
		if (!expect_px(s, &img, 64, 64, colors[steps[i].level], 0)) {
			fail(s, "step %u: LOD %lu, MAXMIPLEVEL %lu, MIPFILTER %lu sampled the wrong level", i,
			     (unsigned long)steps[i].lod, (unsigned long)steps[i].maxmip, (unsigned long)steps[i].filter);
			goto out;
		}
		if (IDirect3DDevice9_GetSamplerState(g_dev, 0, D3DSAMP_MAXMIPLEVEL, &v) != D3D_OK ||
		    v != steps[i].maxmip) {
			fail(s, "step %u: MAXMIPLEVEL reads back %lu", i, (unsigned long)v);
			goto out;
		}
		if (IDirect3DTexture9_GetLOD(tex) != (steps[i].lod > 2 ? 2u : steps[i].lod)) {
			fail(s, "step %u: GetLOD answered %lu", i, (unsigned long)IDirect3DTexture9_GetLOD(tex));
			goto out;
		}
	}
out:
	if (g_dev) {
		IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
		IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
	}
	RELEASE(IDirect3DTexture9, tex);
	image_free(&img);
}

#define C_SC_BG D3DCOLOR_XRGB(0x10, 0x80, 0x30)
#define C_SC_RECT D3DCOLOR_XRGB(0xe0, 0x20, 0x90)
/* IID_IDirect3DSwapChain9 (d3d9.h declares it; nothing here defines it). */
static const GUID g_iid_swapchain9 = { 0x794950f2, 0xadfc, 0x458a, { 0x90, 0x5e, 0x10, 0xa1, 0x0b, 0x0b, 0x50, 0x3b } };

/* An additional swap chain (native rules, Wine's test_swapchain on the
 * stock runtime): defaults written back, the device window for a NULL one,
 * a device reference, back buffers with the swap chain as container,
 * renders and presents; GetSwapChain and GetNumberOfSwapChains know only
 * the implicit one; no fullscreen swap chain. */
static void scene_swapchain(struct scene *s)
{
	IDirect3DSwapChain9 *sc = NULL, *x = NULL, *cont = NULL;
	IDirect3DSurface9 *bb = NULL, *rt = NULL, *keep = (IDirect3DSurface9 *)(UINT_PTR)0x1234;
	D3DPRESENT_PARAMETERS pp, got;
	struct image img = { 0 };
	ULONG before = dev_refs();
	HRESULT hr;

	memset(&pp, 0, sizeof(pp));
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 48;
	TRY(s, IDirect3DDevice9_CreateAdditionalSwapChain(g_dev, &pp, &sc));
	if (pp.BackBufferCount != 1 || pp.hDeviceWindow != NULL) {
		fail(s, "CreateAdditionalSwapChain wrote back count %u window %p", pp.BackBufferCount,
		     (void *)pp.hDeviceWindow);
		goto out;
	}
	if (dev_refs() != before + 1) {
		fail(s, "device count %lu with a swap chain, expected %lu", (unsigned long)dev_refs(),
		     (unsigned long)before + 1);
		goto out;
	}
	TRY(s, IDirect3DSwapChain9_GetPresentParameters(sc, &got));
	if (got.BackBufferWidth != 64 || got.BackBufferHeight != 48 || got.BackBufferCount != 1 ||
	    got.hDeviceWindow != g_hwnd || !got.Windowed) {
		fail(s, "swap chain parameters %ux%u count %u window %p", got.BackBufferWidth, got.BackBufferHeight,
		     got.BackBufferCount, (void *)got.hDeviceWindow);
		goto out;
	}
	hr = IDirect3DDevice9_GetSwapChain(g_dev, 1, &x);
	if (hr != D3DERR_INVALIDCALL || x || IDirect3DDevice9_GetNumberOfSwapChains(g_dev) != 1) {
		fail(s, "GetSwapChain(1) 0x%08lx or GetNumberOfSwapChains %u counts the additional swap chain",
		     (unsigned long)hr, IDirect3DDevice9_GetNumberOfSwapChains(g_dev));
		goto out;
	}
	if (IDirect3DSwapChain9_GetBackBuffer(sc, 1, D3DBACKBUFFER_TYPE_MONO, &keep) != D3DERR_INVALIDCALL ||
	    keep != (IDirect3DSurface9 *)(UINT_PTR)0x1234) {
		fail(s, "GetBackBuffer(1) of a one-buffer swap chain succeeded or changed the pointer");
		goto out;
	}
	TRY(s, IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
	TRY(s, IDirect3DSurface9_GetContainer(bb, &g_iid_swapchain9, (void **)&cont));
	if (cont != sc) {
		fail(s, "back buffer container %p, expected the swap chain %p", (void *)cont, (void *)sc);
		goto out;
	}
	RELEASE(IDirect3DSwapChain9, cont);

	TRY(s, IDirect3DDevice9_GetRenderTarget(g_dev, 0, &rt));
	TRY(s, IDirect3DDevice9_SetRenderTarget(g_dev, 0, bb));
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_SC_BG, 1.0f, 0));
	TRY(s, begin_scene());
	TRY(s, draw_rect_c(8, 8, 24, 20, 0.5f, C_SC_RECT));
	TRY(s, end_scene());
	TRY(s, IDirect3DDevice9_SetRenderTarget(g_dev, 0, rt));
	if (!read_surface(s, bb, &img))
		goto out;
	if (img.w != 64 || img.h != 48) {
		fail(s, "swap chain back buffer is %ux%u", img.w, img.h);
		goto out;
	}
	expect_px(s, &img, 40, 30, C_SC_BG, 0);
	expect_px(s, &img, 16, 14, C_SC_RECT, 0);
	TRY(s, IDirect3DSwapChain9_Present(sc, NULL, NULL, NULL, NULL, 0));

	pp.Windowed = FALSE;
	pp.hDeviceWindow = g_hwnd;
	if (IDirect3DDevice9_CreateAdditionalSwapChain(g_dev, &pp, &x) != D3DERR_INVALIDCALL || x) {
		fail(s, "a fullscreen additional swap chain was created");
		goto out;
	}
out:
	RELEASE(IDirect3DSurface9, rt);
	RELEASE(IDirect3DSwapChain9, cont);
	RELEASE(IDirect3DSwapChain9, x);
	RELEASE(IDirect3DSurface9, bb);
	RELEASE(IDirect3DSwapChain9, sc);
	if (!s->failures && dev_refs() != before)
		fail(s, "device count %lu after the swap chain went, expected %lu", (unsigned long)dev_refs(),
		     (unsigned long)before);
	image_free(&img);
}

#define C_FB_LEFT D3DCOLOR_XRGB(0xc0, 0x40, 0x10)
#define C_FB_RIGHT D3DCOLOR_XRGB(0x10, 0x60, 0xd0)

/* The front buffer of a windowed device is the screen with the window's
 * last presented image at its client area (natively the desktop), not a
 * black surface: two halves read back at their screen positions. */
static void scene_frontbuffer(struct scene *s)
{
	IDirect3DSurface9 *front = NULL;
	D3DDISPLAYMODE dm;
	D3DLOCKED_RECT lr;
	D3DRECT right = { (LONG)g_pp.BackBufferWidth / 2, 0, (LONG)g_pp.BackBufferWidth, (LONG)g_pp.BackBufferHeight };
	RECT cr;
	POINT pt[2];
	D3DCOLOR want[2] = { C_FB_LEFT, C_FB_RIGHT };
	int i, locked = 0;

	/* On top, so the stock runtime's front buffer (the desktop) shows it. */
	SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	for (i = 0; i < 3; i++) {
		TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_FB_LEFT, 1.0f, 0));
		TRY(s, IDirect3DDevice9_Clear(g_dev, 1, &right, D3DCLEAR_TARGET, C_FB_RIGHT, 1.0f, 0));
		note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL));
		pump_messages();
		Sleep(40);
	}
	GetClientRect(g_hwnd, &cr);
	pt[0].x = cr.right / 4;
	pt[0].y = cr.bottom / 2;
	pt[1].x = cr.right * 3 / 4;
	pt[1].y = cr.bottom / 2;
	ClientToScreen(g_hwnd, &pt[0]);
	ClientToScreen(g_hwnd, &pt[1]);
	TRY(s, IDirect3DDevice9_GetDisplayMode(g_dev, 0, &dm));
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, dm.Width, dm.Height, D3DFMT_A8R8G8B8,
							    D3DPOOL_SYSTEMMEM, &front, NULL));
	TRY(s, IDirect3DDevice9_GetFrontBufferData(g_dev, 0, front));
	TRY(s, IDirect3DSurface9_LockRect(front, &lr, NULL, D3DLOCK_READONLY));
	locked = 1;
	for (i = 0; i < 2; i++) {
		DWORD got;

		if (pt[i].x < 0 || pt[i].y < 0 || (UINT)pt[i].x >= dm.Width || (UINT)pt[i].y >= dm.Height) {
			printf("INFO frontbuffer: probe %ld,%ld off the screen\n", pt[i].x, pt[i].y);
			continue;
		}
		got = ((const DWORD *)((const BYTE *)lr.pBits + (size_t)pt[i].y * (size_t)lr.Pitch))[pt[i].x];
		if (!channels_close(got & 0x00ffffffu, want[i] & 0x00ffffffu, 2))
			fail(s, "front buffer at %ld,%ld is %06lx, expected %06lx", pt[i].x, pt[i].y,
			     (unsigned long)(got & 0x00ffffffu), (unsigned long)(want[i] & 0x00ffffffu));
	}
out:
	if (locked)
		IDirect3DSurface9_UnlockRect(front);
	RELEASE(IDirect3DSurface9, front);
	if (g_hwnd)
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

#define C_FOLLOW_BG D3DCOLOR_XRGB(0x30, 0x50, 0x20)

/*
 * Between presents the cursor follows the mouse, like a hardware cursor
 * plane: the proxy's cursor poller reports where the cursor went and the
 * host publishes the last frame again with the cursor there. One present
 * with the device cursor at a first point, then SetCursorPos to two more
 * points inside the window: the loopback harness's dumps of the
 * re-published frame (frame_N_cK.bmp) show the cursor at each point and the
 * frame clean where it was, and no further frame was presented. Third
 * scene: its frame N is the run's seventh, within the harness's first eight
 * dumped frames. Also reports (Measured) the poller's CPU load with a still
 * and with a moving mouse.
 */
static void scene_cursor_follow(struct scene *s)
{
	static const POINT at[3] = { { 40, 40 }, { 150, 60 }, { 90, 170 } };
	IDirect3DSurface9 *surf = NULL;
	D3DLOCKED_RECT lr;
	struct image img = { 0 };
	char dir[MAX_PATH], path[MAX_PATH + 32];
	POINT saved = { 0, 0 }, co = { 0, 0 }, now, got[3];
	unsigned n, k = 0;
	double still, moving;
	DWORD t0;
	int i, x, y, found, moved = 0;

	if (!qd3d_session() || !GetEnvironmentVariableA("QD3D_FRAME_DUMPS", dir, sizeof(dir)) || !dir[0]) {
		printf("INFO cursor_follow: checked through QD3D with frame dumps (run-loopback.ps1) only\n");
		return;
	}
	TRY(s, IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surf,
							    NULL));
	TRY(s, IDirect3DSurface9_LockRect(surf, &lr, NULL, 0));
	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++)
			((DWORD *)((BYTE *)lr.pBits + (size_t)y * (size_t)lr.Pitch))[x] = C_CURSOR;
	TRY(s, IDirect3DSurface9_UnlockRect(surf));
	/* Hot spot (2, 3): no system cursor has it (wait_cursor tells them apart). */
	TRY(s, IDirect3DDevice9_SetCursorProperties(g_dev, 2, 3, surf));
	GetCursorPos(&saved);
	moved = 1;
	SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	ClientToScreen(g_hwnd, &co);
	g_cursor_d3d = 1;
	SetCursorPos(co.x + at[0].x, co.y + at[0].y);
	IDirect3DDevice9_ShowCursor(g_dev, TRUE);
	if (!wait_cursor(2, 3, 32, 2000)) {
		printf("INFO cursor_follow: the cursor over the window could not be observed; not checked\n");
		goto out;
	}
	/* Where the mouse went (display scaling may round a requested point),
	 * in client coordinates: the frame's pixels. */
	GetCursorPos(&got[0]);
	got[0].x -= co.x;
	got[0].y -= co.y;
	if (got[0].x < 3 || got[0].y < 3 || got[0].x > 200 || got[0].y > 200) {
		printf("INFO cursor_follow: the mouse is not where it was put; not checked\n");
		goto out;
	}
	TRY(s, IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, C_FOLLOW_BG, 1.0f, 0));
	note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL));
	n = g_presents;
	if (read_dump(s, n, &img) <= 0)
		goto out;
	expect_px(s, &img, got[0].x, got[0].y, C_CURSOR, 0);
	expect_px(s, &img, got[0].x + 29, got[0].y + 28, C_CURSOR, 0);
	expect_px(s, &img, got[0].x - 3, got[0].y, C_FOLLOW_BG, 0);
	for (i = 1; i < 3 && !s->failures; i++) {
		SetCursorPos(co.x + at[i].x, co.y + at[i].y);
		GetCursorPos(&got[i]);
		got[i].x -= co.x;
		got[i].y -= co.y;
		if (got[i].x < 3 || got[i].y < 3 || got[i].x > 200 || got[i].y > 200) {
			printf("INFO cursor_follow: the mouse is not where it was put; not checked further\n");
			goto out;
		}
		for (found = 0, t0 = GetTickCount(); !found && GetTickCount() - t0 < 3000u;) {
			pump_messages();
			snprintf(path, sizeof(path), "%s\\frame_%06u_c%03u.bmp", dir, n, k + 1u);
			if (!read_bmp(path, &img)) {
				Sleep(5);
				continue;
			}
			k++;
			found = (img.px[(size_t)got[i].y * img.w + (size_t)got[i].x] & 0x00ffffffu) ==
					(C_CURSOR & 0x00ffffffu) &&
				(img.px[(size_t)got[i - 1].y * img.w + (size_t)got[i - 1].x] & 0x00ffffffu) ==
					(C_FOLLOW_BG & 0x00ffffffu);
		}
		GetCursorPos(&now);
		if (now.x - co.x != got[i].x || now.y - co.y != got[i].y) {
			printf("INFO cursor_follow: the mouse moved during the scene; not checked further\n");
			goto out;
		}
		if (!found) {
			fail(s, "no repeat of frame %u shows the cursor at %ld,%ld (%u read)", n, got[i].x, got[i].y, k);
			goto out;
		}
		expect_px(s, &img, got[i].x + 29, got[i].y + 28, C_CURSOR, 0);
		expect_px(s, &img, got[i - 1].x + 29, got[i - 1].y + 28, C_FOLLOW_BG, 0);
	}
	snprintf(path, sizeof(path), "%s\\frame_%06u.bmp", dir, n + 1u);
	if (g_presents != n || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
		fail(s, "a frame was presented while only the cursor moved");
	if (!s->failures)
		printf("INFO cursor_follow: cursor at 3 points over frame %u, %u repeats read, no present\n", n, k);
	still = other_threads_load(1000, NULL);
	moving = other_threads_load(1000, &co);
	if (still >= 0.0)
		printf("INFO cursor_follow: Measured: cursor poller %.3f%% of one core with a still mouse, %.3f%% with "
		       "the mouse moving every 10 ms\n",
		       still, moving);
out:
	g_cursor_d3d = 0;
	if (g_dev)
		IDirect3DDevice9_ShowCursor(g_dev, FALSE);
	if (moved)
		SetCursorPos(saved.x, saved.y);
	if (g_hwnd)
		SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	RELEASE(IDirect3DSurface9, surf);
	image_free(&img);
}

#define C_HB_1 D3DCOLOR_XRGB(0x90, 0x10, 0x40)
#define C_HB_2 D3DCOLOR_XRGB(0x10, 0x90, 0x70)

/*
 * A second device on a window of its own presents two frames and is
 * released. Through QD3D the frontend shows both frames and then learns
 * that their source is gone, before anything later (the VM returns to the
 * guest desktop at once; the loopback harness appends "slot S after frame
 * N" to QD3D_FRAME_DUMPS\source_released.txt). Second scene: the frame
 * numbers follow the cursor scene's frames and its closing present.
 */
static void scene_handback(struct scene *s)
{
	static const char cls[] = "qd3d9_smoke_handback";
	IDirect3DDevice9 *dev = NULL;
	D3DPRESENT_PARAMETERS pp;
	struct image img = { 0 };
	char dir[MAX_PATH], path[MAX_PATH + 32], line[64], want[64];
	unsigned last;
	WNDCLASSA wc;
	HWND w;
	DWORD t0;
	int found = 0;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = cls;
	RegisterClassA(&wc);
	w = CreateWindowA(cls, "qd3d9_smoke handback", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64, NULL, NULL,
			  wc.hInstance, NULL);
	if (!w) {
		fail(s, "CreateWindow error %lu", (unsigned long)GetLastError());
		return;
	}
	memset(&pp, 0, sizeof(pp));
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = w;
	pp.Windowed = TRUE;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	TRY(s, IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w,
				       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev));
	TRY(s, IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, C_HB_1, 1.0f, 0));
	note_present(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));
	TRY(s, IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, C_HB_2, 1.0f, 0));
	note_present(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));
	last = g_presents;
	if (IDirect3DDevice9_Release(dev) != 0)
		fail(s, "the second device kept references");
	dev = NULL;
	if (s->failures || !qd3d_session() ||
	    !GetEnvironmentVariableA("QD3D_FRAME_DUMPS", dir, sizeof(dir)) || !dir[0])
		goto out;
	snprintf(path, sizeof(path), "%s\\source_released.txt", dir);
	snprintf(want, sizeof(want), "slot 0 after frame %u", last);
	for (t0 = GetTickCount(); !found && GetTickCount() - t0 < 5000u; Sleep(20)) {
		FILE *f = fopen(path, "r");

		while (f && !found && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			found = !strcmp(line, want);
		}
		if (f)
			fclose(f);
	}
	if (!found) {
		fail(s, "no \"%s\" in %s", want, path);
		goto out;
	}
	if (read_dump(s, last - 1u, &img) > 0)
		expect_px(s, &img, 32, 32, C_HB_1, 0);
	if (read_dump(s, last, &img) > 0)
		expect_px(s, &img, 32, 32, C_HB_2, 0);
	printf("INFO handback: device release reported after its frames %u and %u\n", last - 1u, last);
out:
	RELEASE(IDirect3DDevice9, dev);
	DestroyWindow(w);
	UnregisterClassA(cls, wc.hInstance);
	image_free(&img);
}

static const struct {
	const char *name;
	void (*run)(struct scene *s);
} scenes[] = {
	{ "cursor", scene_cursor }, /* first: its frames are the run's first */
	{ "handback", scene_handback }, /* second: its frames follow the cursor scene's */
	{ "cursor_follow", scene_cursor_follow }, /* third: its frame is the run's seventh */
	{ "clear", scene_clear },
	{ "up_tri", scene_up_tri },
	{ "vb_ib", scene_vb_ib },
	{ "texture", scene_texture },
	{ "dynamic_vb", scene_dynamic_vb },
	{ "shaders", scene_shaders },
	{ "rtt", scene_rtt },
	{ "depth_blend", scene_depth_blend },
	{ "query", scene_query },
	{ "reset", scene_reset },
	{ "present", scene_present },
	{ "fpu_setup", scene_fpu_setup },
	{ "bound_lifetime", scene_bound_lifetime },
	{ "setlod", scene_setlod },
	{ "swapchain", scene_swapchain },
	{ "frontbuffer", scene_frontbuffer },
};

int main(void)
{
	HANDLE wd;
	unsigned passed = 0;
	size_t i;
	const size_t count = sizeof(scenes) / sizeof(scenes[0]);

	setvbuf(stdout, NULL, _IONBF, 0);
	g_finished = CreateEventA(NULL, TRUE, FALSE, NULL);
	wd = g_finished ? CreateThread(NULL, 0, watchdog, NULL, 0, NULL) : NULL;
	if (wd == NULL) {
		printf("FAIL setup: watchdog error %lu\n",
		       (unsigned long)GetLastError());
		return 2;
	}
	if (!setup()) {
		printf("FAIL setup: %s\n", g_setup_error);
		teardown();
		SetEvent(g_finished);
		WaitForSingleObject(wd, INFINITE);
		return 2;
	}
	for (i = 0; i < count; i++) {
		struct scene s = { .name = scenes[i].name };

		base_state(&s);
		if (s.failures == 0)
			scenes[i].run(&s);
		if (g_in_scene)
			end_scene();
		if (s.failures == 0) {
			printf("PASS %s\n", s.name);
			passed++;
		} else if (s.failures == 1) {
			printf("FAIL %s: %s\n", s.name, s.first);
		} else {
			printf("FAIL %s: %s (+%u more)\n", s.name, s.first,
			       s.failures - 1);
		}
		if (i + 1 < count)
			note_present(IDirect3DDevice9_Present(g_dev, NULL, NULL,
							      NULL, NULL));
		pump_messages();
	}
	teardown();
	SetEvent(g_finished);
	WaitForSingleObject(wd, INFINITE);
	CloseHandle(wd);
	CloseHandle(g_finished);
	printf("RESULT %s %u/%u scenes passed, %u presents\n",
	       passed == count ? "PASS" : "FAIL", passed, (unsigned)count,
	       g_presents);
	return passed == count ? 0 : 1;
}
