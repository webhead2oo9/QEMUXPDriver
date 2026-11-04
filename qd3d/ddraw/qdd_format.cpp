/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: pixel formats, layout and conversions.
 *
 * Matching follows the native runtime as Wine's tests document it: palette
 * indexed by flag; 8-bit RGB is R3G3B2 and 24-bit RGB is R8G8B8 whatever
 * the masks; 16/32-bit RGB by masks; z by depth (and masks for 32 bits);
 * FOURCC by code.  Pitches are aligned to 8 bytes (measured on the stock
 * 32-bit runtime: a 63-texel X8R8G8B8 row has pitch 0x100).
 */
#include "qdd_priv.h"

#define PF(flags, bits, r, g, b, a) \
    {sizeof(DDPIXELFORMAT), (flags), 0, {(bits)}, {(r)}, {(g)}, {(b)}, {(a)}}
#define PF_FOURCC(cc) \
    {sizeof(DDPIXELFORMAT), DDPF_FOURCC, (cc), {0}, {0}, {0}, {0}, {0}}

/* Indexes into g_formats. */
enum {
    F_P1, F_P2, F_P4, F_P8,
    F_R3G3B2, F_A8R3G3B2, F_X4R4G4B4, F_A4R4G4B4, F_R5G6B5, F_X1R5G5B5, F_A1R5G5B5,
    F_R8G8B8, F_X8R8G8B8, F_A8R8G8B8, F_X8B8G8R8, F_A8B8G8R8,
    F_A8, F_L8, F_A8L8, F_A4L4,
    F_V8U8, F_L6V5U5, F_X8L8V8U8,
    F_D16, F_D15S1, F_D24X8, F_D24S8, F_D24X4S4, F_D32, F_D24, F_D24X8H, F_D24S8H, F_D24X4S4H,
    F_DXT1, F_DXT2, F_DXT3, F_DXT4, F_DXT5, F_YUY2, F_UYVY, F_YV12,
    F_COUNT
};

static const QddFormat g_formats[F_COUNT] = {
    /* F_P1 */ {QFC_PAL, 1, 0, 0, 0, 0, 0, 0, D3DFMT_UNKNOWN, PF(DDPF_RGB | DDPF_PALETTEINDEXED1, 1, 0, 0, 0, 0)},
    /* F_P2 */ {QFC_PAL, 2, 0, 0, 0, 0, 0, 0, D3DFMT_UNKNOWN, PF(DDPF_RGB | DDPF_PALETTEINDEXED2, 2, 0, 0, 0, 0)},
    /* F_P4 */ {QFC_PAL, 4, 0, 0, 0, 0, 0, 0, D3DFMT_UNKNOWN, PF(DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0, 0, 0)},
    /* F_P8 */ {QFC_PAL, 8, 0, 0, 0, 0, 0, 0, D3DFMT_P8, PF(DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0)},
    /* F_R3G3B2 */ {QFC_RGB, 8, 0, 0, 0xe0, 0x1c, 0x03, 0, D3DFMT_R3G3B2, PF(DDPF_RGB, 8, 0xe0, 0x1c, 0x03, 0)},
    /* F_A8R3G3B2 */ {QFC_RGB, 16, 0, 0, 0xe0, 0x1c, 0x03, 0xff00, D3DFMT_A8R3G3B2,
                      PF(DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0xe0, 0x1c, 0x03, 0xff00)},
    /* F_X4R4G4B4 */ {QFC_RGB, 16, 0, 0, 0x0f00, 0x00f0, 0x000f, 0, D3DFMT_X4R4G4B4,
                      PF(DDPF_RGB, 16, 0x0f00, 0x00f0, 0x000f, 0)},
    /* F_A4R4G4B4 */ {QFC_RGB, 16, 0, 0, 0x0f00, 0x00f0, 0x000f, 0xf000, D3DFMT_A4R4G4B4,
                      PF(DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000)},
    /* F_R5G6B5 */ {QFC_RGB, 16, 0, 0, 0xf800, 0x07e0, 0x001f, 0, D3DFMT_R5G6B5,
                    PF(DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0)},
    /* F_X1R5G5B5 */ {QFC_RGB, 16, 0, 0, 0x7c00, 0x03e0, 0x001f, 0, D3DFMT_X1R5G5B5,
                      PF(DDPF_RGB, 16, 0x7c00, 0x03e0, 0x001f, 0)},
    /* F_A1R5G5B5 */ {QFC_RGB, 16, 0, 0, 0x7c00, 0x03e0, 0x001f, 0x8000, D3DFMT_A1R5G5B5,
                      PF(DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000)},
    /* F_R8G8B8 */ {QFC_RGB, 24, 0, 0, 0xff0000, 0xff00, 0xff, 0, D3DFMT_R8G8B8,
                    PF(DDPF_RGB, 24, 0xff0000, 0xff00, 0xff, 0)},
    /* F_X8R8G8B8 */ {QFC_RGB, 32, 0, 0, 0xff0000, 0xff00, 0xff, 0, D3DFMT_X8R8G8B8,
                      PF(DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0)},
    /* F_A8R8G8B8 */ {QFC_RGB, 32, 0, 0, 0xff0000, 0xff00, 0xff, 0xff000000, D3DFMT_A8R8G8B8,
                      PF(DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xff0000, 0xff00, 0xff, 0xff000000)},
    /* F_X8B8G8R8 */ {QFC_RGB, 32, 0, 0, 0xff, 0xff00, 0xff0000, 0, D3DFMT_X8B8G8R8,
                      PF(DDPF_RGB, 32, 0xff, 0xff00, 0xff0000, 0)},
    /* F_A8B8G8R8 */ {QFC_RGB, 32, 0, 0, 0xff, 0xff00, 0xff0000, 0xff000000, D3DFMT_A8B8G8R8,
                      PF(DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xff, 0xff00, 0xff0000, 0xff000000)},
    /* F_A8 */ {QFC_ALPHA, 8, 0, 0, 0, 0, 0, 0xff, D3DFMT_A8, PF(DDPF_ALPHA, 8, 0, 0, 0, 0)},
    /* F_L8 */ {QFC_LUM, 8, 0, 0, 0xff, 0, 0, 0, D3DFMT_L8, PF(DDPF_LUMINANCE, 8, 0xff, 0, 0, 0)},
    /* F_A8L8 */ {QFC_LUM, 16, 0, 0, 0x00ff, 0, 0, 0xff00, D3DFMT_A8L8,
                  PF(DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0x00ff, 0, 0, 0xff00)},
    /* F_A4L4 */ {QFC_LUM, 8, 0, 0, 0x0f, 0, 0, 0xf0, D3DFMT_A4L4,
                  PF(DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 8, 0x0f, 0, 0, 0xf0)},
    /* F_V8U8 */ {QFC_BUMP, 16, 0, 0, 0x00ff, 0xff00, 0, 0, D3DFMT_V8U8, PF(DDPF_BUMPDUDV, 16, 0x00ff, 0xff00, 0, 0)},
    /* F_L6V5U5 */ {QFC_BUMP, 16, 0, 0, 0x001f, 0x03e0, 0xfc00, 0, D3DFMT_L6V5U5,
                    PF(DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 16, 0x001f, 0x03e0, 0xfc00, 0)},
    /* F_X8L8V8U8 */ {QFC_BUMP, 32, 0, 0, 0x0000ff, 0x00ff00, 0xff0000, 0, D3DFMT_X8L8V8U8,
                      PF(DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 32, 0x0000ff, 0x00ff00, 0xff0000, 0)},
    /* F_D16 */ {QFC_DEPTH, 16, 0, 0, 0xffff, 0, 0, 0, D3DFMT_D16, PF(DDPF_ZBUFFER, 16, 0, 0xffff, 0, 0)},
    /* F_D15S1 */ {QFC_DEPTH, 16, 0, 0, 0x7fff, 0, 0, 0x8000, D3DFMT_D15S1,
                   PF(DDPF_ZBUFFER | DDPF_STENCILBUFFER, 16, 1, 0x7fff, 0x8000, 0)},
    /* F_D24X8 */ {QFC_DEPTH, 32, 0, 0, 0x00ffffff, 0, 0, 0, D3DFMT_D24X8, PF(DDPF_ZBUFFER, 32, 0, 0x00ffffff, 0, 0)},
    /* F_D24S8 */ {QFC_DEPTH, 32, 0, 0, 0x00ffffff, 0, 0, 0xff000000, D3DFMT_D24S8,
                   PF(DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 8, 0x00ffffff, 0xff000000, 0)},
    /* F_D24X4S4 */ {QFC_DEPTH, 32, 0, 0, 0x00ffffff, 0, 0, 0x0f000000, D3DFMT_D24X4S4,
                     PF(DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 4, 0x00ffffff, 0x0f000000, 0)},
    /* F_D32 */ {QFC_DEPTH, 32, 0, 0, 0xffffffff, 0, 0, 0, D3DFMT_D32, PF(DDPF_ZBUFFER, 32, 0, 0xffffffff, 0, 0)},
    /* The same layouts with the depth in the high bits, and the 24-bit
     * descriptor (stored in 32 bits): the descriptor keeps what the
     * application asked for, and fills and transfers use its masks. */
    /* F_D24 */ {QFC_DEPTH, 32, 0, 0, 0x00ffffff, 0, 0, 0, D3DFMT_D24X8, PF(DDPF_ZBUFFER, 24, 0, 0x00ffffff, 0, 0)},
    /* F_D24X8H */ {QFC_DEPTH, 32, 0, 0, 0xffffff00, 0, 0, 0, D3DFMT_D24X8, PF(DDPF_ZBUFFER, 32, 0, 0xffffff00, 0, 0)},
    /* F_D24S8H */ {QFC_DEPTH, 32, 0, 0, 0xffffff00, 0, 0, 0x000000ff, D3DFMT_D24S8,
                    PF(DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 8, 0xffffff00, 0x000000ff, 0)},
    /* F_D24X4S4H */ {QFC_DEPTH, 32, 0, 0, 0xffffff00, 0, 0, 0x0000000f, D3DFMT_D24X4S4,
                      PF(DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 4, 0xffffff00, 0x0000000f, 0)},
    /* F_DXT1 */ {QFC_FOURCC_BLOCK, 0, 8, MAKEFOURCC('D', 'X', 'T', '1'), 0, 0, 0, 0, D3DFMT_DXT1,
                  PF_FOURCC(MAKEFOURCC('D', 'X', 'T', '1'))},
    /* F_DXT2 */ {QFC_FOURCC_BLOCK, 0, 16, MAKEFOURCC('D', 'X', 'T', '2'), 0, 0, 0, 0, D3DFMT_DXT2,
                  PF_FOURCC(MAKEFOURCC('D', 'X', 'T', '2'))},
    /* F_DXT3 */ {QFC_FOURCC_BLOCK, 0, 16, MAKEFOURCC('D', 'X', 'T', '3'), 0, 0, 0, 0, D3DFMT_DXT3,
                  PF_FOURCC(MAKEFOURCC('D', 'X', 'T', '3'))},
    /* F_DXT4 */ {QFC_FOURCC_BLOCK, 0, 16, MAKEFOURCC('D', 'X', 'T', '4'), 0, 0, 0, 0, D3DFMT_DXT4,
                  PF_FOURCC(MAKEFOURCC('D', 'X', 'T', '4'))},
    /* F_DXT5 */ {QFC_FOURCC_BLOCK, 0, 16, MAKEFOURCC('D', 'X', 'T', '5'), 0, 0, 0, 0, D3DFMT_DXT5,
                  PF_FOURCC(MAKEFOURCC('D', 'X', 'T', '5'))},
    /* F_YUY2 */ {QFC_FOURCC_YUV, 16, 0, MAKEFOURCC('Y', 'U', 'Y', '2'), 0, 0, 0, 0, D3DFMT_YUY2,
                  PF_FOURCC(MAKEFOURCC('Y', 'U', 'Y', '2'))},
    /* F_UYVY */ {QFC_FOURCC_YUV, 16, 0, MAKEFOURCC('U', 'Y', 'V', 'Y'), 0, 0, 0, 0, D3DFMT_UYVY,
                  PF_FOURCC(MAKEFOURCC('U', 'Y', 'V', 'Y'))},
    /* F_YV12 */ {QFC_FOURCC_PLANAR, 8, 0, MAKEFOURCC('Y', 'V', '1', '2'), 0, 0, 0, 0,
                  (D3DFORMAT)MAKEFOURCC('Y', 'V', '1', '2'), PF_FOURCC(MAKEFOURCC('Y', 'V', '1', '2'))},
};

const QddFormat *qdd_format_from_ddpf(const DDPIXELFORMAT *pf)
{
    DWORD fl = pf->dwFlags;
    if (fl & DDPF_PALETTEINDEXED8)
        return &g_formats[F_P8];
    if (fl & DDPF_PALETTEINDEXED4)
        return &g_formats[F_P4];
    if (fl & DDPF_PALETTEINDEXED2)
        return &g_formats[F_P2];
    if (fl & DDPF_PALETTEINDEXED1)
        return &g_formats[F_P1];
    if (fl & DDPF_RGB) {
        bool alpha = (fl & DDPF_ALPHAPIXELS) != 0;
        DWORD r = pf->dwRBitMask, g = pf->dwGBitMask, b = pf->dwBBitMask, a = pf->dwRGBAlphaBitMask;
        switch (pf->dwRGBBitCount) {
        case 8:
            return &g_formats[F_R3G3B2];
        case 16:
            if (r == 0xf800 && g == 0x07e0 && b == 0x001f)
                return &g_formats[F_R5G6B5];
            if (r == 0x7c00 && g == 0x03e0 && b == 0x001f)
                return &g_formats[alpha && a == 0x8000 ? F_A1R5G5B5 : F_X1R5G5B5];
            if (r == 0x0f00 && g == 0x00f0 && b == 0x000f)
                return &g_formats[alpha && a == 0xf000 ? F_A4R4G4B4 : F_X4R4G4B4];
            if (alpha && a == 0xff00 && r == 0x00e0 && g == 0x001c && b == 0x0003)
                return &g_formats[F_A8R3G3B2];
            return NULL;
        case 24:
            return &g_formats[F_R8G8B8];
        case 32:
            if (r == 0x00ff0000 && g == 0x0000ff00 && b == 0x000000ff)
                return &g_formats[alpha && a == 0xff000000 ? F_A8R8G8B8 : F_X8R8G8B8];
            if (r == 0x000000ff && g == 0x0000ff00 && b == 0x00ff0000)
                return &g_formats[alpha && a == 0xff000000 ? F_A8B8G8R8 : F_X8B8G8R8];
            return NULL;
        default:
            return NULL;
        }
    }
    if (fl & DDPF_ALPHA)
        return pf->dwAlphaBitDepth == 8 ? &g_formats[F_A8] : NULL;
    if (fl & DDPF_LUMINANCE) {
        DWORD lm = pf->dwLuminanceBitMask, am = pf->dwLuminanceAlphaBitMask;
        if (fl & DDPF_ALPHAPIXELS) {
            if ((lm == 0x00ff && am == 0xff00) || (!lm && pf->dwLuminanceBitCount == 8))
                return &g_formats[F_A8L8];
            if ((lm == 0x0f && am == 0xf0) || (!lm && pf->dwLuminanceBitCount == 4))
                return &g_formats[F_A4L4];
            return NULL;
        }
        return pf->dwLuminanceBitCount == 8 ? &g_formats[F_L8] : NULL;
    }
    if (fl & DDPF_ZBUFFER) {
        DWORD zm = pf->dwZBitMask, sm = pf->dwStencilBitMask;
        bool high = zm == 0xffffff00;
        if (fl & DDPF_STENCILBUFFER) {
            if (pf->dwZBufferBitDepth == 16 && pf->dwStencilBitDepth == 1)
                return &g_formats[F_D15S1];
            if (pf->dwZBufferBitDepth == 32 && pf->dwStencilBitDepth == 8)
                return &g_formats[high || sm == 0x000000ff ? F_D24S8H : F_D24S8];
            if (pf->dwZBufferBitDepth == 32 && pf->dwStencilBitDepth == 4)
                return &g_formats[high || sm == 0x0000000f ? F_D24X4S4H : F_D24X4S4];
            return NULL;
        }
        switch (pf->dwZBufferBitDepth) {
        case 16:
            return &g_formats[F_D16];
        case 24:
            return &g_formats[F_D24];
        case 32:
            if (zm == 0x00ffffff || !zm)
                return &g_formats[F_D24X8];
            if (high)
                return &g_formats[F_D24X8H];
            if (zm == 0xffffffff)
                return &g_formats[F_D32];
            return NULL;
        default:
            return NULL;
        }
    }
    if (fl & DDPF_FOURCC) {
        for (UINT i = F_DXT1; i <= F_YV12; ++i)
            if (g_formats[i].fourcc == pf->dwFourCC)
                return &g_formats[i];
        return NULL;
    }
    if (fl & DDPF_BUMPDUDV) {
        DWORD u = pf->dwBumpDuBitMask, v = pf->dwBumpDvBitMask, l = pf->dwBumpLuminanceBitMask;
        if (pf->dwBumpBitCount == 16 && u == 0x00ff && v == 0xff00 && !l)
            return &g_formats[F_V8U8];
        if (pf->dwBumpBitCount == 16 && u == 0x001f && v == 0x03e0 && l == 0xfc00)
            return &g_formats[F_L6V5U5];
        if (pf->dwBumpBitCount == 32 && u == 0x00ff && v == 0xff00 && l == 0x00ff0000)
            return &g_formats[F_X8L8V8U8];
    }
    return NULL;
}

const QddFormat *qdd_format_for_depth_bits(DWORD bits)
{
    switch (bits) {
    case 16: return &g_formats[F_D16];
    case 24: return &g_formats[F_D24];
    case 32: return &g_formats[F_D32];
    default: return NULL;
    }
}

const QddFormat *qdd_format_for_bpp(DWORD bpp)
{
    switch (bpp) {
    case 8: return &g_formats[F_P8];
    case 16: return &g_formats[F_R5G6B5];
    case 24: return &g_formats[F_R8G8B8];
    case 32: return &g_formats[F_X8R8G8B8];
    default: return NULL;
    }
}

const QddFormat *qdd_format_d3d(D3DFORMAT f)
{
    for (UINT i = 0; i < F_COUNT; ++i)
        if (g_formats[i].d3d == f && g_formats[i].d3d != D3DFMT_UNKNOWN)
            return &g_formats[i];
    return NULL;
}

DWORD qdd_format_pitch(const QddFormat *f, DWORD w)
{
    DWORD bytes;
    if (f->cls == QFC_FOURCC_BLOCK)
        bytes = ((w + 3) / 4) * f->block_bytes;
    else if (f->cls == QFC_FOURCC_YUV)
        bytes = ((w + 1) & ~1u) * 2;
    else
        bytes = (w * f->bpp + 7) / 8;
    return (bytes + 7) & ~7u;
}

DWORD qdd_format_rows(const QddFormat *f, DWORD h)
{
    if (f->cls == QFC_FOURCC_BLOCK)
        return (h + 3) / 4;
    if (f->cls == QFC_FOURCC_PLANAR)
        return h + (h + 1) / 2; /* the V and U planes: half the pitch, half the rows each */
    return h;
}

/* ------------------------------------------------------------------------ */
/* Pixel conversion                                                          */

static inline UINT qdd_shift(DWORD mask)
{
    UINT s = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++s;
    }
    return s;
}

static inline UINT qdd_bits(DWORD mask)
{
    UINT n = 0;
    while (mask) {
        n += mask & 1;
        mask >>= 1;
    }
    return n;
}

/* Channel value under mask, widened to 8 bits by bit replication. */
static inline DWORD qdd_chan8(DWORD px, DWORD mask)
{
    if (!mask)
        return 0;
    UINT bits = qdd_bits(mask);
    DWORD v = (px & mask) >> qdd_shift(mask);
    if (bits >= 8)
        return v >> (bits - 8);
    DWORD out = 0;
    int have = 0;
    while (have < 8) {
        out = (out << bits) | v;
        have += (int)bits;
    }
    return out >> (have - 8);
}

static inline DWORD qdd_pack8(DWORD v8, DWORD mask)
{
    if (!mask)
        return 0;
    UINT bits = qdd_bits(mask);
    DWORD v = bits >= 8 ? v8 << (bits - 8) : v8 >> (8 - bits);
    return (v << qdd_shift(mask)) & mask;
}

DWORD qdd_pixel_to_argb(const QddFormat *f, DWORD px, const PALETTEENTRY *pal)
{
    switch (f->cls) {
    case QFC_PAL: {
        if (!pal)
            return 0xff000000u;
        const PALETTEENTRY *e = &pal[px & 0xff];
        return 0xff000000u | ((DWORD)e->peRed << 16) | ((DWORD)e->peGreen << 8) | e->peBlue;
    }
    case QFC_RGB: {
        DWORD a = f->mask_a ? qdd_chan8(px, f->mask_a) : 0xff;
        return (a << 24) | (qdd_chan8(px, f->mask_r) << 16) | (qdd_chan8(px, f->mask_g) << 8) |
               qdd_chan8(px, f->mask_b);
    }
    case QFC_LUM: {
        DWORD l = qdd_chan8(px, f->mask_r);
        DWORD a = f->mask_a ? qdd_chan8(px, f->mask_a) : 0xff;
        return (a << 24) | (l << 16) | (l << 8) | l;
    }
    case QFC_ALPHA:
        return (px & 0xff) << 24;
    default:
        return px;
    }
}

DWORD qdd_argb_to_pixel(const QddFormat *f, DWORD argb)
{
    DWORD a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    switch (f->cls) {
    case QFC_RGB:
        return qdd_pack8(r, f->mask_r) | qdd_pack8(g, f->mask_g) | qdd_pack8(b, f->mask_b) | qdd_pack8(a, f->mask_a);
    case QFC_LUM: {
        DWORD l = (r * 77 + g * 151 + b * 28) >> 8;
        return qdd_pack8(l, f->mask_r) | qdd_pack8(a, f->mask_a);
    }
    case QFC_ALPHA:
        return a;
    default:
        return argb;
    }
}

static inline BYTE qdd_clamp8(int v)
{
    return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601 studio range, as YUV overlay hardware converts. */
DWORD qdd_yuv_to_argb(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    BYTE r = qdd_clamp8((c + 409 * e + 128) >> 8);
    BYTE g = qdd_clamp8((c - 100 * d - 208 * e + 128) >> 8);
    BYTE b = qdd_clamp8((c + 516 * d + 128) >> 8);
    return 0xff000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
}

bool qdd_convert_row(const QddFormat *df, BYTE *dst, const QddFormat *sf, const BYTE *src, UINT n,
                     const PALETTEENTRY *pal)
{
    UINT db = qdd_format_bytes(df);
    if (df == sf && db) {
        memcpy(dst, src, (size_t)n * db);
        return true;
    }
    if (!db)
        return false;
    if (sf->cls == QFC_FOURCC_YUV) {
        bool yuy2 = sf->fourcc == MAKEFOURCC('Y', 'U', 'Y', '2');
        for (UINT x = 0; x < n; ++x) {
            const BYTE *p = src + (x & ~1u) * 2;
            int y, u, v;
            if (yuy2) {
                y = p[(x & 1) ? 2 : 0];
                u = p[1];
                v = p[3];
            } else {
                y = p[(x & 1) ? 3 : 1];
                u = p[0];
                v = p[2];
            }
            qdd_write_px(dst + (size_t)x * db, db, qdd_argb_to_pixel(df, qdd_yuv_to_argb(y, u, v)));
        }
        return true;
    }
    UINT sb = qdd_format_bytes(sf);
    if (!sb)
        return false;
    if (df->cls == QFC_DEPTH || sf->cls == QFC_DEPTH || df->cls == QFC_BUMP || sf->cls == QFC_BUMP ||
        df->cls == QFC_PAL)
        return false;
    /* Fast path for P8 presentation uploads. */
    if (sb == 1 && sf->cls == QFC_PAL && db == 4 && pal) {
        DWORD lut[256];
        for (UINT i = 0; i < 256; ++i)
            lut[i] = qdd_argb_to_pixel(df, qdd_pixel_to_argb(sf, i, pal));
        DWORD *d = (DWORD *)dst;
        for (UINT x = 0; x < n; ++x)
            d[x] = lut[src[x]];
        return true;
    }
    for (UINT x = 0; x < n; ++x) {
        DWORD px = qdd_read_px(src + (size_t)x * sb, sb);
        qdd_write_px(dst + (size_t)x * db, db, qdd_argb_to_pixel(df, qdd_pixel_to_argb(sf, px, pal)));
    }
    return true;
}
