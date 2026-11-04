/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: DXT1-5 blocks to and from A8R8G8B8, for blits between compressed
 * and other surfaces, stretched or not block-aligned (the stock runtime
 * decodes and encodes those on the CPU, Wine's test_compressed_surface_
 * stretch and test_surface_format_conversion_alpha).
 *
 * The encoder takes the two most distant colors of a block as endpoints
 * (exact for blocks of one or two colors, as those tests use), the larger
 * 5:6:5 value first so opaque DXT1 blocks use the four-color mode, and the
 * nearest palette entry per pixel; DXT1 blocks with transparent pixels use
 * the three-color mode.
 */
#include "qdd_priv.h"

static DWORD qdd_565_to_argb(WORD c)
{
    DWORD r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xff000000u | ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
}

static WORD qdd_argb_to_565(DWORD c)
{
    DWORD r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    return (WORD)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

static DWORD qdd_mix(DWORD a, DWORD b, UINT wa, UINT wb, UINT div)
{
    DWORD out = 0xff000000u;
    for (UINT s = 0; s < 24; s += 8)
        out |= ((((a >> s) & 0xff) * wa + ((b >> s) & 0xff) * wb) / div) << s;
    return out;
}

/* The four colors of a color block; four-color mode unless three (DXT1 with
 * c0 <= c1: the third is the mean, the fourth transparent black). */
static void qdd_color_palette(WORD c0, WORD c1, bool four, DWORD pal[4])
{
    pal[0] = qdd_565_to_argb(c0);
    pal[1] = qdd_565_to_argb(c1);
    if (four) {
        pal[2] = qdd_mix(pal[0], pal[1], 2, 1, 3);
        pal[3] = qdd_mix(pal[0], pal[1], 1, 2, 3);
    } else {
        pal[2] = qdd_mix(pal[0], pal[1], 1, 1, 2);
        pal[3] = 0;
    }
}

static UINT qdd_block_kind(const QddFormat *f)
{
    switch (f->fourcc) {
    case MAKEFOURCC('D', 'X', 'T', '1'): return 1;
    case MAKEFOURCC('D', 'X', 'T', '2'):
    case MAKEFOURCC('D', 'X', 'T', '3'): return 3;
    default: return 5;
    }
}

void qdd_dxt_decode(const QddFormat *f, const BYTE *blk, DWORD out[16])
{
    UINT kind = qdd_block_kind(f);
    const BYTE *cb = kind == 1 ? blk : blk + 8;
    WORD c0 = (WORD)(cb[0] | cb[1] << 8), c1 = (WORD)(cb[2] | cb[3] << 8);
    DWORD idx = (DWORD)cb[4] | (DWORD)cb[5] << 8 | (DWORD)cb[6] << 16 | (DWORD)cb[7] << 24;
    DWORD pal[4];
    qdd_color_palette(c0, c1, kind != 1 || c0 > c1, pal);
    for (UINT i = 0; i < 16; ++i)
        out[i] = pal[(idx >> (2 * i)) & 3];
    if (kind == 3) {
        for (UINT i = 0; i < 16; ++i) {
            DWORD a = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            out[i] = (out[i] & 0xffffff) | (a * 17) << 24;
        }
    } else if (kind == 5) {
        DWORD a0 = blk[0], a1 = blk[1], al[8];
        al[0] = a0;
        al[1] = a1;
        if (a0 > a1) {
            for (UINT i = 1; i < 7; ++i)
                al[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        } else {
            for (UINT i = 1; i < 5; ++i)
                al[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            al[6] = 0;
            al[7] = 255;
        }
        ULONGLONG bits = 0;
        for (UINT i = 0; i < 6; ++i)
            bits |= (ULONGLONG)blk[2 + i] << (8 * i);
        for (UINT i = 0; i < 16; ++i)
            out[i] = (out[i] & 0xffffff) | al[(bits >> (3 * i)) & 7] << 24;
    }
}

static LONG qdd_dist(DWORD a, DWORD b)
{
    LONG d = 0;
    for (UINT s = 0; s < 24; s += 8) {
        LONG c = (LONG)((a >> s) & 0xff) - (LONG)((b >> s) & 0xff);
        d += c * c;
    }
    return d;
}

static void qdd_encode_color(const DWORD in[16], bool dxt1, BYTE *cb)
{
    bool transparent = false;
    if (dxt1)
        for (UINT i = 0; i < 16; ++i)
            transparent |= (in[i] >> 24) < 128;
    /* Endpoints: the two most distant (opaque) colors. */
    UINT e0 = 16, e1 = 16;
    LONG best = -1;
    for (UINT i = 0; i < 16; ++i) {
        if (transparent && (in[i] >> 24) < 128)
            continue;
        for (UINT j = i; j < 16; ++j) {
            if (transparent && (in[j] >> 24) < 128)
                continue;
            LONG d = qdd_dist(in[i], in[j]);
            if (d > best) {
                best = d;
                e0 = i;
                e1 = j;
            }
        }
    }
    WORD c0 = e0 < 16 ? qdd_argb_to_565(in[e0]) : 0, c1 = e1 < 16 ? qdd_argb_to_565(in[e1]) : 0;
    /* Four-color mode needs c0 > c1, three-color mode c0 <= c1. */
    if (transparent ? c0 > c1 : c0 < c1) {
        WORD t = c0;
        c0 = c1;
        c1 = t;
    }
    bool four = !dxt1 || c0 > c1;
    DWORD pal[4];
    qdd_color_palette(c0, c1, four, pal);
    DWORD idx = 0;
    for (UINT i = 0; i < 16; ++i) {
        UINT code = 0;
        if (transparent && (in[i] >> 24) < 128) {
            code = 3;
        } else {
            LONG bd = 0x7fffffff;
            for (UINT c = 0; c < (four ? 4u : 3u); ++c) {
                LONG d = qdd_dist(in[i], pal[c]);
                if (d < bd) {
                    bd = d;
                    code = c;
                }
            }
        }
        idx |= (DWORD)code << (2 * i);
    }
    cb[0] = (BYTE)c0;
    cb[1] = (BYTE)(c0 >> 8);
    cb[2] = (BYTE)c1;
    cb[3] = (BYTE)(c1 >> 8);
    cb[4] = (BYTE)idx;
    cb[5] = (BYTE)(idx >> 8);
    cb[6] = (BYTE)(idx >> 16);
    cb[7] = (BYTE)(idx >> 24);
}

void qdd_dxt_encode(const QddFormat *f, const DWORD in[16], BYTE *blk)
{
    UINT kind = qdd_block_kind(f);
    if (kind == 1) {
        qdd_encode_color(in, true, blk);
        return;
    }
    qdd_encode_color(in, false, blk + 8);
    if (kind == 3) {
        memset(blk, 0, 8);
        for (UINT i = 0; i < 16; ++i)
            blk[i / 2] |= (BYTE)((((in[i] >> 24) + 8) / 17) << ((i & 1) * 4));
        return;
    }
    DWORD a0 = 0, a1 = 255;
    for (UINT i = 0; i < 16; ++i) {
        DWORD a = in[i] >> 24;
        if (a > a0)
            a0 = a;
        if (a < a1)
            a1 = a;
    }
    blk[0] = (BYTE)a0;
    blk[1] = (BYTE)a1;
    DWORD al[8];
    al[0] = a0;
    al[1] = a1;
    for (UINT i = 1; i < 7; ++i)
        al[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    ULONGLONG bits = 0;
    for (UINT i = 0; i < 16 && a0 != a1; ++i) {
        DWORD a = in[i] >> 24;
        UINT code = 0;
        LONG bd = 0x7fffffff;
        for (UINT c = 0; c < 8; ++c) {
            LONG d = (LONG)a - (LONG)al[c];
            if (d < 0)
                d = -d;
            if (d < bd) {
                bd = d;
                code = c;
            }
        }
        bits |= (ULONGLONG)code << (3 * i);
    }
    for (UINT i = 0; i < 6; ++i)
        blk[2 + i] = (BYTE)(bits >> (8 * i));
}

/* Decodes the blocks covering [x0, x1) x [y0, y1) (block-aligned) of a
 * block surface's memory into rows of A8R8G8B8 (pitch (x1 - x0) * 4). */
void qdd_dxt_decode_rect(const QddFormat *f, const BYTE *bits, LONG pitch, LONG x0, LONG y0, LONG x1, LONG y1,
                         DWORD *out)
{
    LONG w = x1 - x0;
    for (LONG by = y0; by < y1; by += 4) {
        const BYTE *row = bits + (size_t)(by / 4) * pitch;
        for (LONG bx = x0; bx < x1; bx += 4) {
            DWORD px[16];
            qdd_dxt_decode(f, row + (size_t)(bx / 4) * f->block_bytes, px);
            for (UINT y = 0; y < 4; ++y)
                memcpy(out + (size_t)(by - y0 + y) * w + (bx - x0), px + y * 4, 16);
        }
    }
}

void qdd_dxt_encode_rect(const QddFormat *f, BYTE *bits, LONG pitch, LONG x0, LONG y0, LONG x1, LONG y1,
                         const DWORD *in)
{
    LONG w = x1 - x0;
    for (LONG by = y0; by < y1; by += 4) {
        BYTE *row = bits + (size_t)(by / 4) * pitch;
        for (LONG bx = x0; bx < x1; bx += 4) {
            DWORD px[16];
            for (UINT y = 0; y < 4; ++y)
                memcpy(px + y * 4, in + (size_t)(by - y0 + y) * w + (bx - x0), 16);
            qdd_dxt_encode(f, px, row + (size_t)(bx / 4) * f->block_bytes);
        }
    }
}
