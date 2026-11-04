/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d8: D3D8 shader declarations and bytecode on D3D9, without D3DX.
 *
 * Validation.  Shaders are checked with the rules of the native D3D8
 * validator, measured on the stock d3d8.dll, which applies them in
 * ValidateVertexShader/ValidatePixelShader and in CreateVertexShader/
 * CreatePixelShader alike: the version, the instruction set of that version
 * (vs.1.x has neither dcl nor def), register numbers, reads of registers
 * (and components) nothing wrote, vertex inputs the declaration does not
 * load, oPos.xy written by a vertex shader and r0 written by a pixel shader.
 * Messages follow the native wording.  Where a rule is not known for
 * certain the check is left out, so no shader the native runtime accepts
 * is refused.
 *
 * Vertex shaders.  A D3D8 vertex shader is a declaration token stream plus
 * an optional vs.1.0/vs.1.1 function.  The declaration becomes a D3D9 vertex
 * declaration in which input register vN carries the usage the D3D8 fixed
 * function pipeline assigns to register N (D3DVSDE_*), so the same
 * declaration serves fixed-function and programmable processing.  D3D9
 * requires vs_1_1 functions to declare their inputs, so for every register
 * present in the declaration we insert
 *
 *     dcl_<usage><index> vN   = 0x0000001F                       (D3DSIO_DCL)
 *                               0x80000000 | usage | index << 16 (usage token)
 *                               0x900F0000 | N                   (v register,
 *                                                                 full mask)
 *
 * directly after the version token.  Shader model 1 instruction tokens carry
 * no length field, so the DCL token is the bare opcode.  vs.1.0 is promoted
 * to vs.1.1 (a strict superset).  D3D8 scalar instructions (rcp, rsq, exp,
 * log, expp, logp) take the .w of a source without a replicate swizzle; D3D9
 * requires the replicate, so the swizzle becomes that component replicated.
 * D3DVSD_CONST payloads are stored and loaded into constant registers
 * whenever the shader is selected.
 *
 * Pixel shaders.  ps.1.1-1.4 bytecode is identical between D3D8 and D3D9;
 * ps.1.0 is promoted to ps.1.1.
 */
#include "qd3d8.h"
#include <stdarg.h>
#include <stdio.h>

#define Q8_SIO_COMMENT 0xfffeu
#define Q8_SIO_PHASE 0xfffdu
#define Q8_SIO_END 0x0000ffffu
#define Q8_SIO_DEF 0x51u

/* D3D8 register types. */
#define Q8_REG_TEMP 0u
#define Q8_REG_INPUT 1u
#define Q8_REG_CONST 2u
#define Q8_REG_ADDR 3u    /* vs a0; ps t# */
#define Q8_REG_RASTOUT 4u
#define Q8_REG_ATTROUT 5u
#define Q8_REG_TEXCRDOUT 6u

/* ------------------------------------------------------------------------ */
/* Messages                                                                  */

void q8_text_free(Q8Text *t)
{
    free(t->buf);
    t->buf = NULL;
    t->len = t->cap = 0;
}

void q8_text_add(Q8Text *t, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    if (!t)
        return;
    va_start(ap, fmt);
    int n = _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(line) - 1)
        n = (int)sizeof(line) - 1;
    line[n] = 0;
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        while (cap < t->len + (size_t)n + 1)
            cap *= 2;
        char *b = (char *)realloc(t->buf, cap);
        if (!b)
            return;
        t->buf = b;
        t->cap = cap;
    }
    memcpy(t->buf + t->len, line, (size_t)n + 1);
    t->len += (size_t)n;
}

/* ------------------------------------------------------------------------ */
/* Instruction sets                                                          */

/* Which components of a source an instruction reads. */
enum {
    Q8RD_NONE,   /* not tracked */
    Q8RD_COMP,   /* per written component, through the swizzle */
    Q8RD_DP3,    /* .xyz */
    Q8RD_DP4,    /* .xyzw */
    Q8RD_SCALAR, /* the .w selector */
    Q8RD_LIT,    /* .x .y .w */
    Q8RD_DST,    /* src0 .y .z, src1 .y .w */
    Q8RD_M4,     /* src0 .xyzw */
    Q8RD_M3,     /* src0 .xyz */
};

struct Q8OpInfo {
    WORD op;
    BYTE dst, src;
    BYTE vs;    /* bit 0: vs.1.0, bit 1: vs.1.1 */
    BYTE ps;    /* bits 0-4: ps.1.0-1.4 */
    BYTE reads;
};

/* ps.1.4 texld/texcrd (tex/texcoord) take a source as well. */
static const Q8OpInfo q8_ops[] = {
    {0x00, 0, 0, 3, 0x1f, Q8RD_NONE},   /* nop */
    {0x01, 1, 1, 3, 0x1f, Q8RD_COMP},   /* mov */
    {0x02, 1, 2, 3, 0x1f, Q8RD_COMP},   /* add */
    {0x03, 1, 2, 3, 0x1f, Q8RD_COMP},   /* sub */
    {0x04, 1, 3, 3, 0x1f, Q8RD_COMP},   /* mad */
    {0x05, 1, 2, 3, 0x1f, Q8RD_COMP},   /* mul */
    {0x06, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* rcp */
    {0x07, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* rsq */
    {0x08, 1, 2, 3, 0x1f, Q8RD_DP3},    /* dp3 */
    {0x09, 1, 2, 3, 0x1c, Q8RD_DP4},    /* dp4 */
    {0x0a, 1, 2, 3, 0x00, Q8RD_COMP},   /* min */
    {0x0b, 1, 2, 3, 0x00, Q8RD_COMP},   /* max */
    {0x0c, 1, 2, 3, 0x00, Q8RD_COMP},   /* slt */
    {0x0d, 1, 2, 3, 0x00, Q8RD_COMP},   /* sge */
    {0x0e, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* exp */
    {0x0f, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* log */
    {0x10, 1, 1, 3, 0x00, Q8RD_LIT},    /* lit */
    {0x11, 1, 2, 3, 0x00, Q8RD_DST},    /* dst */
    {0x12, 1, 3, 0, 0x1f, Q8RD_COMP},   /* lrp */
    {0x13, 1, 1, 3, 0x00, Q8RD_COMP},   /* frc */
    {0x14, 1, 2, 3, 0x00, Q8RD_M4},     /* m4x4 */
    {0x15, 1, 2, 3, 0x00, Q8RD_M4},     /* m4x3 */
    {0x16, 1, 2, 3, 0x00, Q8RD_M3},     /* m3x4 */
    {0x17, 1, 2, 3, 0x00, Q8RD_M3},     /* m3x3 */
    {0x18, 1, 2, 3, 0x00, Q8RD_M3},     /* m3x2 */
    {0x40, 1, 0, 0, 0x1f, Q8RD_NONE},   /* texcoord / texcrd */
    {0x41, 1, 0, 0, 0x1f, Q8RD_NONE},   /* texkill */
    {0x42, 1, 0, 0, 0x1f, Q8RD_NONE},   /* tex / texld */
    {0x43, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texbem */
    {0x44, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texbeml */
    {0x45, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texreg2ar */
    {0x46, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texreg2gb */
    {0x47, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texm3x2pad */
    {0x48, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texm3x2tex */
    {0x49, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texm3x3pad */
    {0x4a, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texm3x3tex */
    {0x4c, 1, 2, 0, 0x0f, Q8RD_NONE},   /* texm3x3spec */
    {0x4d, 1, 1, 0, 0x0f, Q8RD_NONE},   /* texm3x3vspec */
    {0x4e, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* expp */
    {0x4f, 1, 1, 3, 0x00, Q8RD_SCALAR}, /* logp */
    {0x50, 1, 3, 0, 0x1f, Q8RD_COMP},   /* cnd */
    {0x52, 1, 1, 0, 0x0c, Q8RD_NONE},   /* texreg2rgb */
    {0x53, 1, 1, 0, 0x0c, Q8RD_NONE},   /* texdp3tex */
    {0x54, 1, 1, 0, 0x08, Q8RD_NONE},   /* texm3x2depth */
    {0x55, 1, 1, 0, 0x0c, Q8RD_NONE},   /* texdp3 */
    {0x56, 1, 1, 0, 0x0c, Q8RD_NONE},   /* texm3x3 */
    {0x57, 1, 0, 0, 0x10, Q8RD_NONE},   /* texdepth */
    {0x58, 1, 3, 0, 0x1c, Q8RD_COMP},   /* cmp */
    {0x59, 1, 2, 0, 0x10, Q8RD_NONE},   /* bem */
};

static const Q8OpInfo *q8_op_find(DWORD op, bool pixel, UINT minor)
{
    for (UINT i = 0; i < sizeof(q8_ops) / sizeof(q8_ops[0]); ++i) {
        const Q8OpInfo *o = &q8_ops[i];
        if (o->op != op)
            continue;
        if (pixel ? (o->ps >> minor) & 1u : (o->vs >> minor) & 1u)
            return o;
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Checker                                                                   */

struct Q8Check {
    bool pixel;
    UINT minor;
    const Q8ShaderLimits *lim;
    Q8Text *msg;
    bool failed;
    UINT statement;
    BYTE temp[16];    /* written components of r# */
    BYTE tex[8];      /* ps.1.0-1.3: written components of t# */
    BYTE opos;        /* vs: written components of oPos */
};

static const char *const q8_param_names[] = {"first", "second", "third"};

/* The written mask in the native message format: "*x/0 *y/1 ...". */
static void q8_uninit(Q8Check *c, const char *reg, UINT n, UINT read, UINT written)
{
    UINT missing = read & ~written;
    const char *names = c->pixel ? "rgba" : "xyzw";
    char list[64];
    int len = 0;
    UINT count = 0;
    for (UINT i = 0; i < 4; ++i) {
        if (missing & (1u << i))
            ++count;
        len += _snprintf(list + len, sizeof(list) - (size_t)len, "%s%s%c/%u", i ? " " : "",
                         (missing & (1u << i)) ? "*" : "", names[i], i);
    }
    q8_text_add(c->msg, "(Statement %u) (Validation Error) Read of uninitialized component%s(*) in %s%u: %s\n",
                c->statement, count == 1 ? "" : "s", reg, n, list);
    c->failed = true;
}

/* Components of src index si read by an instruction writing mask. */
static UINT q8_read_mask(const Q8OpInfo *op, UINT si, DWORD src, UINT mask)
{
    UINT sel[4], comps = 0;
    for (UINT i = 0; i < 4; ++i)
        sel[i] = (src >> (16 + 2 * i)) & 3u;
    switch (op->reads) {
    case Q8RD_COMP:
        for (UINT i = 0; i < 4; ++i)
            if (mask & (1u << i))
                comps |= 1u << sel[i];
        return comps;
    case Q8RD_DP3:
        return (1u << sel[0]) | (1u << sel[1]) | (1u << sel[2]);
    case Q8RD_DP4:
        return (1u << sel[0]) | (1u << sel[1]) | (1u << sel[2]) | (1u << sel[3]);
    case Q8RD_SCALAR:
        return 1u << sel[3];
    case Q8RD_LIT:
        return (1u << sel[0]) | (1u << sel[1]) | (1u << sel[3]);
    case Q8RD_DST:
        return si == 0 ? (1u << sel[1]) | (1u << sel[2]) : (1u << sel[1]) | (1u << sel[3]);
    case Q8RD_M4:
        return si == 0 ? (1u << sel[0]) | (1u << sel[1]) | (1u << sel[2]) | (1u << sel[3]) : 0;
    case Q8RD_M3:
        return si == 0 ? (1u << sel[0]) | (1u << sel[1]) | (1u << sel[2]) : 0;
    default:
        return 0;
    }
}

/* Whether a register type may be a destination (dst) or a source. */
static bool q8_reg_allowed(const Q8Check *c, UINT type, bool dst)
{
    if (c->pixel) {
        switch (type) {
        case Q8_REG_TEMP: return true;
        case Q8_REG_INPUT: case Q8_REG_CONST: return !dst;
        case Q8_REG_ADDR: return !dst || c->minor < 4; /* t# */
        default: return false;
        }
    }
    switch (type) {
    case Q8_REG_TEMP: return true;
    case Q8_REG_INPUT: case Q8_REG_CONST: return !dst;
    case Q8_REG_ADDR: return dst && c->minor == 1;
    case Q8_REG_RASTOUT: case Q8_REG_ATTROUT: case Q8_REG_TEXCRDOUT: return dst;
    default: return false;
    }
}

/* Registers of an allowed type (numbers below the limit). */
static UINT q8_reg_limit(const Q8Check *c, UINT type)
{
    if (c->pixel) {
        bool v14 = c->minor == 4;
        switch (type) {
        case Q8_REG_TEMP: return v14 ? 6 : 2;
        case Q8_REG_INPUT: return 2;
        case Q8_REG_CONST: return 8;
        default: return v14 ? 6 : 4; /* t# */
        }
    }
    switch (type) {
    case Q8_REG_TEMP: return 12;
    case Q8_REG_INPUT: return 16;
    case Q8_REG_CONST: return c->lim->max_const == ~0u ? 0x1000u : c->lim->max_const;
    case Q8_REG_ADDR: return 1;
    case Q8_REG_RASTOUT: return 3;
    case Q8_REG_ATTROUT: return 2;
    default: return 8; /* oT# */
    }
}

static void q8_check_dst(Q8Check *c, const Q8OpInfo *op, DWORD tok)
{
    UINT type = (tok >> 28) & 7u, n = tok & 0xfffu, mask = (tok >> 16) & 0xfu;
    if (!q8_reg_allowed(c, type, true)) {
        q8_text_add(c->msg, "(Statement %u) (Validation Error) Invalid register type for dest param.\n", c->statement);
        c->failed = true;
        return;
    }
    UINT limit = q8_reg_limit(c, type);
    if (n >= limit) {
        q8_text_add(c->msg, "(Statement %u) (Validation Error) Invalid reg num: %u for dest param. Max allowed "
                    "for this type is %d.\n", c->statement, n, (int)limit - 1);
        c->failed = true;
        return;
    }
    if (type == Q8_REG_TEMP) {
        c->temp[n] |= (BYTE)mask;
    } else if (c->pixel && type == Q8_REG_ADDR) {
        /* Texture instructions load the whole register. */
        c->tex[n] |= op->op >= 0x40 && op->op <= 0x57 ? 0xf : (BYTE)mask;
    } else if (!c->pixel && type == Q8_REG_RASTOUT && n == 0) {
        c->opos |= (BYTE)mask;
    }
}

static void q8_check_src(Q8Check *c, const Q8OpInfo *op, UINT si, DWORD tok, UINT mask)
{
    UINT type = (tok >> 28) & 7u, n = tok & 0xfffu;
    const char *which = q8_param_names[si < 3 ? si : 2];
    if (!q8_reg_allowed(c, type, false)) {
        q8_text_add(c->msg, "(Statement %u) (Validation Error) Invalid register type for %s source param.\n",
                    c->statement, which);
        c->failed = true;
        return;
    }
    if (tok & 0x2000u) {
        /* Relative addressing: vs.1.1 constants only; the base is not
         * bounded (a0 adds to it). */
        if (c->pixel || c->minor != 1 || type != Q8_REG_CONST) {
            q8_text_add(c->msg, "(Statement %u) (Validation Error) Relative addressing is only allowed for "
                        "constant registers.\n", c->statement);
            c->failed = true;
        }
        return;
    }
    UINT limit = q8_reg_limit(c, type);
    if (n >= limit) {
        q8_text_add(c->msg, "(Statement %u) (Validation Error) Invalid reg num: %u for %s source param. Max "
                    "allowed for this type is %d.\n", c->statement, n, which, (int)limit - 1);
        c->failed = true;
        return;
    }
    UINT read = q8_read_mask(op, si, tok, mask);
    if (!read)
        return;
    if (type == Q8_REG_TEMP) {
        if (read & ~c->temp[n])
            q8_uninit(c, "r", n, read, c->temp[n]);
    } else if (type == Q8_REG_INPUT && !c->pixel && c->lim->inputs != ~0u) {
        if (!(c->lim->inputs & (1u << n)))
            q8_uninit(c, "v", n, read, 0);
    } else if (type == Q8_REG_ADDR && c->pixel && c->minor < 4) {
        if (read & ~c->tex[n])
            q8_uninit(c, "t", n, read, c->tex[n]);
    }
}

static HRESULT q8_check_shader(const DWORD *code, bool pixel, const Q8ShaderLimits *lim, Q8Text *msg, UINT *length)
{
    const char *kind = pixel ? "pixel" : "vertex";
    Q8Check c;
    memset(&c, 0, sizeof(c));
    c.pixel = pixel;
    c.lim = lim;
    c.msg = msg;
    c.statement = 1;
    if (length)
        *length = 0;

    DWORD ver = code[0];
    UINT major = (ver >> 8) & 0xffu, minor = ver & 0xffu;
    if ((ver >> 16) != (pixel ? 0xffffu : 0xfffeu)) {
        q8_text_add(msg, "(Global Validation Error) Version token: 0x%08lx indicates a %s shader.  %s shader "
                    "version token must be of the form 0x%s****.\n", (unsigned long)ver,
                    (ver >> 16) == 0xffffu ? "pixel" : (ver >> 16) == 0xfffeu ? "vertex" : "unknown",
                    pixel ? "Pixel" : "Vertex", pixel ? "ffff" : "fffe");
        return E_FAIL;
    }
    if (major != 1 || minor > (pixel ? 4u : 1u)) {
        q8_text_add(msg, "(Global Validation Error) Version Token: %u.%u is not a supported %s shader version. "
                    "Aborting %s shader validation.\n", major, minor, kind, kind);
        return E_FAIL;
    }
    if (lim->max_version != ~0u && ver > lim->max_version) {
        q8_text_add(msg, "(Global Validation Error) Version Token: %s shader version %u.%u is too high for "
                    "device.%s Maximum supported version is %u.%u. Aborting shader validation.\n",
                    pixel ? "Pixel" : "Vertex", major, minor, pixel ? "  " : " ",
                    (unsigned)((lim->max_version >> 8) & 0xffu), (unsigned)(lim->max_version & 0xffu));
        return E_FAIL;
    }
    c.minor = minor;

    UINT i = 1;
    while (i < QD3D8_MAX_VS_TOKENS) {
        DWORD tok = code[i];
        if (tok == Q8_SIO_END) {
            if (length)
                *length = i + 1;
            break;
        }
        DWORD opcode = tok & 0xffffu;
        if (opcode == Q8_SIO_COMMENT && !(tok & 0x80000000u)) {
            i += 1 + ((tok >> 16) & 0x7fffu);
            continue;
        }
        ++c.statement;
        if (opcode == Q8_SIO_PHASE && pixel && minor == 4 && !(tok & 0x80000000u)) {
            ++i;
            continue;
        }
        if (opcode == Q8_SIO_DEF && pixel && !(tok & 0x80000000u)) {
            /* Destination constant and four raw floats. */
            if (i + 5 >= QD3D8_MAX_VS_TOKENS || !(code[i + 1] & 0x80000000u) ||
                ((code[i + 1] >> 28) & 7u) != Q8_REG_CONST || (code[i + 1] & 0xfffu) >= 8) {
                q8_text_add(msg, "(Statement %u) (Validation Error) Invalid def instruction. Aborting %s shader "
                            "validation.\n", c.statement, kind);
                return E_FAIL;
            }
            i += 6;
            continue;
        }
        const Q8OpInfo *op = (tok & 0x80000000u) ? NULL : q8_op_find(opcode, pixel, minor);
        if (!op) {
            q8_text_add(msg, "(Statement %u) (Validation Error) Unrecognized instruction. Aborting %s shader "
                        "validation.\n", c.statement, kind);
            return E_FAIL;
        }
        UINT nsrc = op->src;
        if (pixel && minor == 4 && (opcode == 0x40 || opcode == 0x42))
            nsrc = 1; /* texcrd / texld */
        UINT nparams = 0;
        while (i + 1 + nparams < QD3D8_MAX_VS_TOKENS && (code[i + 1 + nparams] & 0x80000000u))
            ++nparams;
        if (nparams != op->dst + nsrc) {
            q8_text_add(msg, "(Statement %u) (Validation Error) Invalid number of parameters: %u, expected %u. "
                        "Aborting %s shader validation.\n", c.statement, nparams, op->dst + nsrc, kind);
            return E_FAIL;
        }
        UINT mask = 0xf;
        if (op->dst)
            mask = (code[i + 1] >> 16) & 0xfu;
        /* Sources are read before the destination is written. */
        for (UINT s = 0; s < nsrc; ++s)
            q8_check_src(&c, op, s, code[i + 1 + op->dst + s], mask);
        if (op->dst)
            q8_check_dst(&c, op, code[i + 1]);
        i += 1 + nparams;
    }
    if (i >= QD3D8_MAX_VS_TOKENS) {
        q8_text_add(msg, "(Global Validation Error) The %s shader has no end token. Aborting %s shader "
                    "validation.\n", kind, kind);
        return E_FAIL;
    }
    if (!pixel && (c.opos & 3u) != 3u) {
        q8_text_add(msg, "(Global Validation Error) Vertex shader must minimally write first two (x,y) "
                    "components of oPos output register.\n");
        c.failed = true;
    }
    if (pixel && c.temp[0] != 0xf) {
        char list[64];
        int len = 0;
        UINT count = 0;
        for (UINT k = 0; k < 4; ++k) {
            bool miss = !(c.temp[0] & (1u << k));
            count += miss;
            len += _snprintf(list + len, sizeof(list) - (size_t)len, "%s%s%c/%u", k ? " " : "", miss ? "*" : "",
                             "rgba"[k], k);
        }
        q8_text_add(msg, "(Global Validation Error) r0 must be written by shader. Uninitialized component%s(*): "
                    "%s\n", count == 1 ? "" : "s", list);
        c.failed = true;
    }
    return c.failed ? E_FAIL : D3D_OK;
}

HRESULT q8_check_vertex_shader(const DWORD *code, const Q8ShaderLimits *lim, Q8Text *msg, UINT *length)
{
    return q8_check_shader(code, false, lim, msg, length);
}

HRESULT q8_check_pixel_shader(const DWORD *code, const Q8ShaderLimits *lim, Q8Text *msg, UINT *length)
{
    return q8_check_shader(code, true, lim, msg, length);
}

/* ------------------------------------------------------------------------ */
/* Declarations                                                              */

/* D3DVSDE register -> D3D9 usage, usage index. */
static const BYTE q8_reg_usage[17][2] = {
    {D3DDECLUSAGE_POSITION, 0},     /* D3DVSDE_POSITION */
    {D3DDECLUSAGE_BLENDWEIGHT, 0},  /* D3DVSDE_BLENDWEIGHT */
    {D3DDECLUSAGE_BLENDINDICES, 0}, /* D3DVSDE_BLENDINDICES */
    {D3DDECLUSAGE_NORMAL, 0},       /* D3DVSDE_NORMAL */
    {D3DDECLUSAGE_PSIZE, 0},        /* D3DVSDE_PSIZE */
    {D3DDECLUSAGE_COLOR, 0},        /* D3DVSDE_DIFFUSE */
    {D3DDECLUSAGE_COLOR, 1},        /* D3DVSDE_SPECULAR */
    {D3DDECLUSAGE_TEXCOORD, 0},     /* D3DVSDE_TEXCOORD0 */
    {D3DDECLUSAGE_TEXCOORD, 1},
    {D3DDECLUSAGE_TEXCOORD, 2},
    {D3DDECLUSAGE_TEXCOORD, 3},
    {D3DDECLUSAGE_TEXCOORD, 4},
    {D3DDECLUSAGE_TEXCOORD, 5},
    {D3DDECLUSAGE_TEXCOORD, 6},
    {D3DDECLUSAGE_TEXCOORD, 7},     /* D3DVSDE_TEXCOORD7 */
    {D3DDECLUSAGE_POSITION, 1},     /* D3DVSDE_POSITION2 */
    {D3DDECLUSAGE_NORMAL, 1},       /* D3DVSDE_NORMAL2 */
};

/* D3DVSDT_* and D3DDECLTYPE_* share values 0-7; byte sizes: */
static const BYTE q8_vsdt_size[8] = {4, 8, 12, 16, 4, 4, 4, 8};

/* Data types fixed-function processing takes per register (bit per
 * D3DVSDT_*; measured on the stock d3d8.dll): position, normal, position2
 * and normal2 FLOAT3; blend weights and texture coordinates FLOAT1-4; blend
 * indices UBYTE4; point size FLOAT1; colours D3DCOLOR. */
static const BYTE q8_ff_types[17] = {
    0x04, 0x0f, 0x20, 0x04, 0x01, 0x10, 0x10, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x04, 0x04,
};

struct Q8DeclResult {
    UINT token_count;
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
    UINT nelems;
    DWORD regmask; /* registers 0-16 present */
    DWORD *consts;
    UINT nconsts;
};

/* fixed: a declaration without a function, which the native runtime holds
 * to the fixed-function layout rules (measured): a position, data types per
 * register, registers in increasing order within a stream, each stream
 * once, no skipped data, texture coordinates from set 0 without gaps. */
static HRESULT q8_parse_decl(const DWORD *decl, bool fixed, Q8DeclResult *r)
{
    UINT stream_offset[16];
    int stream = -1;
    UINT const_cap = 0;
    DWORD streams_seen = 0;
    int last_reg = -1;

    memset(stream_offset, 0, sizeof(stream_offset));
    r->nelems = 0;
    r->regmask = 0;
    r->consts = NULL;
    r->nconsts = 0;
    r->token_count = 0;

    for (UINT i = 0; i < QD3D8_MAX_DECL_TOKENS;) {
        DWORD tok = decl[i];
        if (tok == 0xffffffffu) {
            if (fixed) {
                DWORD tc = (r->regmask >> 7) & 0xffu;
                if (!(r->regmask & 1u) || (tc & (tc + 1u))) {
                    q8_log("qd3d8: fixed-function declaration without position or with texture coordinate gaps");
                    goto fail;
                }
            }
            r->token_count = i + 1;
            r->elems[r->nelems].Stream = 0xff;
            r->elems[r->nelems].Offset = 0;
            r->elems[r->nelems].Type = D3DDECLTYPE_UNUSED;
            r->elems[r->nelems].Method = 0;
            r->elems[r->nelems].Usage = 0;
            r->elems[r->nelems].UsageIndex = 0;
            return D3D_OK;
        }
        switch (tok >> D3DVSD_TOKENTYPESHIFT) {
        case d8::D3DVSD_TOKEN_NOP:
            ++i;
            break;
        case d8::D3DVSD_TOKEN_STREAM:
            if (tok & D3DVSD_STREAMTESSMASK) {
                q8_log("qd3d8: declaration uses tessellator stream (unsupported)");
                goto fail;
            }
            stream = (int)(tok & D3DVSD_STREAMNUMBERMASK);
            if (fixed && (streams_seen & (1u << stream)))
                goto fail;
            streams_seen |= 1u << stream;
            last_reg = -1;
            ++i;
            break;
        case d8::D3DVSD_TOKEN_STREAMDATA: {
            if (stream < 0)
                goto fail;
            DWORD cnt = (tok & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;
            if (tok & D3DVSD_DATALOADTYPEMASK) {
                /* D3DVSD_SKIP (DWORDs) or D3DVSD_SKIPBYTES (bit 27). */
                if (fixed)
                    goto fail;
                stream_offset[stream] += (tok & 0x08000000u) ? cnt : cnt * 4;
            } else {
                DWORD reg = tok & D3DVSD_VERTEXREGMASK;
                DWORD type = (tok & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
                if (reg > 16 || type > 7 || (r->regmask & (1u << reg)) || r->nelems >= MAXD3DDECLLENGTH)
                    goto fail;
                if (fixed && (!(q8_ff_types[reg] & (1u << type)) || (int)reg <= last_reg)) {
                    q8_log("qd3d8: fixed-function declaration: register %lu type %lu out of place",
                           (unsigned long)reg, (unsigned long)type);
                    goto fail;
                }
                last_reg = (int)reg;
                D3DVERTEXELEMENT9 *e = &r->elems[r->nelems++];
                e->Stream = (WORD)stream;
                e->Offset = (WORD)stream_offset[stream];
                e->Type = (BYTE)type;
                e->Method = D3DDECLMETHOD_DEFAULT;
                e->Usage = q8_reg_usage[reg][0];
                e->UsageIndex = q8_reg_usage[reg][1];
                r->regmask |= 1u << reg;
                stream_offset[stream] += q8_vsdt_size[type];
            }
            if (stream_offset[stream] > 0xffffu)
                goto fail;
            ++i;
            break;
        }
        case d8::D3DVSD_TOKEN_TESSELLATOR:
            q8_log("qd3d8: declaration uses tessellator tokens (unsupported)");
            goto fail;
        case d8::D3DVSD_TOKEN_CONSTMEM: {
            DWORD count = (tok & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT;
            DWORD addr = tok & D3DVSD_CONSTADDRESSMASK;
            UINT payload = count * 4;
            if (i + 1 + payload >= QD3D8_MAX_DECL_TOKENS)
                goto fail;
            if (r->nconsts + 2 + payload > const_cap) {
                UINT cap = (r->nconsts + 2 + payload) * 2;
                DWORD *n = (DWORD *)realloc(r->consts, cap * sizeof(DWORD));
                if (!n)
                    goto fail;
                r->consts = n;
                const_cap = cap;
            }
            r->consts[r->nconsts++] = addr;
            r->consts[r->nconsts++] = count;
            memcpy(r->consts + r->nconsts, decl + i + 1, payload * sizeof(DWORD));
            r->nconsts += payload;
            i += 1 + payload;
            break;
        }
        case d8::D3DVSD_TOKEN_EXT:
            i += 1 + ((tok & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT);
            break;
        default:
            goto fail;
        }
    }
fail:
    free(r->consts);
    r->consts = NULL;
    r->nconsts = 0;
    return D3DERR_INVALIDCALL;
}

bool q8_decl_inputs(const DWORD *decl, DWORD *regs)
{
    Q8DeclResult dr;
    if (FAILED(q8_parse_decl(decl, false, &dr)))
        return false;
    free(dr.consts);
    *regs = dr.regmask;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Shader objects                                                            */

static DWORD *q8_dup_tokens(const DWORD *src, UINT count)
{
    DWORD *p = (DWORD *)malloc(count * sizeof(DWORD));
    if (p)
        memcpy(p, src, count * sizeof(DWORD));
    return p;
}

static void q8_log_rejected(const char *kind, const Q8Text *msg)
{
    q8_log("qd3d8: %s shader rejected: %.400s", kind, msg->buf ? msg->buf : "");
}

/* A source parameter of a D3D8 scalar instruction without a replicate
 * swizzle selects its .w component. */
static DWORD q8_scalar_swizzle(DWORD src)
{
    DWORD w = (src >> 22) & 3u;
    DWORD rep = (w | (w << 2) | (w << 4) | (w << 6)) << 16;
    return (src & ~0x00ff0000u) | rep;
}

void q8_destroy_vertex_shader(Q8VertexShader *vs)
{
    if (!vs)
        return;
    if (vs->vs9)
        vs->vs9->Release();
    if (vs->decl9)
        vs->decl9->Release();
    free(vs->decl);
    free(vs->func);
    free(vs->consts);
    free(vs);
}

HRESULT q8_create_vertex_shader(IDirect3DDevice9 *dev, const DWORD *decl, const DWORD *func, UINT max_const,
                                Q8VertexShader **out)
{
    *out = NULL;
    if (!decl)
        return D3DERR_INVALIDCALL;

    Q8DeclResult dr;
    HRESULT hr = q8_parse_decl(decl, func == NULL, &dr);
    if (FAILED(hr))
        return hr;

    UINT len = 0;
    if (func) {
        Q8ShaderLimits lim = {~0u, max_const, dr.regmask & 0xffffu};
        Q8Text msg = {NULL, 0, 0};
        hr = q8_check_vertex_shader(func, &lim, &msg, &len);
        if (FAILED(hr)) {
            q8_log_rejected("vertex", &msg);
            q8_text_free(&msg);
            free(dr.consts);
            return D3DERR_INVALIDCALL;
        }
        q8_text_free(&msg);
    }

    Q8VertexShader *vs = (Q8VertexShader *)calloc(1, sizeof(*vs));
    if (!vs) {
        free(dr.consts);
        return E_OUTOFMEMORY;
    }
    vs->consts = dr.consts;
    vs->consts_count = dr.nconsts;
    vs->decl_count = dr.token_count;
    vs->decl = q8_dup_tokens(decl, dr.token_count);
    if (!vs->decl) {
        q8_destroy_vertex_shader(vs);
        return E_OUTOFMEMORY;
    }

    hr = dev->CreateVertexDeclaration(dr.elems, &vs->decl9);
    if (FAILED(hr)) {
        q8_log("qd3d8: CreateVertexDeclaration failed hr=0x%08lx", (unsigned long)hr);
        q8_destroy_vertex_shader(vs);
        return hr;
    }

    if (func) {
        vs->func_count = len;
        vs->func = q8_dup_tokens(func, len);
        if (!vs->func) {
            q8_destroy_vertex_shader(vs);
            return E_OUTOFMEMORY;
        }

        DWORD regs = dr.regmask & 0xffffu;
        UINT ndcl = 0;
        for (UINT r = 0; r < 16; ++r)
            if (regs & (1u << r))
                ++ndcl;
        DWORD *code = (DWORD *)malloc((len + ndcl * 3) * sizeof(DWORD));
        if (!code) {
            q8_destroy_vertex_shader(vs);
            return E_OUTOFMEMORY;
        }
        UINT o = 0;
        code[o++] = D3DVS_VERSION(1, 1);
        for (UINT r = 0; r < 16; ++r) {
            if (!(regs & (1u << r)))
                continue;
            code[o++] = D3DSIO_DCL;
            code[o++] = 0x80000000u | ((DWORD)q8_reg_usage[r][0] << D3DSP_DCL_USAGE_SHIFT) |
                        ((DWORD)q8_reg_usage[r][1] << D3DSP_DCL_USAGEINDEX_SHIFT);
            code[o++] = 0x80000000u | ((DWORD)D3DSPR_INPUT << D3DSP_REGTYPE_SHIFT) | D3DSP_WRITEMASK_ALL | r;
        }
        /* The validated function, with D3D9's replicate swizzle on scalar
         * instructions (one destination, one source). */
        for (UINT i = 1; i < len;) {
            DWORD tok = func[i];
            DWORD op = tok & 0xffffu;
            code[o++] = tok;
            ++i;
            if (op == Q8_SIO_COMMENT) {
                UINT n = (tok >> 16) & 0x7fffu;
                memcpy(code + o, func + i, n * sizeof(DWORD));
                o += n;
                i += n;
                continue;
            }
            bool scalar = op == 0x06 || op == 0x07 || op == 0x0e || op == 0x0f || op == 0x4e || op == 0x4f;
            for (UINT p = 0; i < len && (func[i] & 0x80000000u); ++p, ++i)
                code[o++] = scalar && p == 1 ? q8_scalar_swizzle(func[i]) : func[i];
        }
        hr = dev->CreateVertexShader(code, &vs->vs9);
        free(code);
        if (FAILED(hr)) {
            q8_log("qd3d8: CreateVertexShader (D3D9) failed hr=0x%08lx", (unsigned long)hr);
            q8_destroy_vertex_shader(vs);
            return hr;
        }
    }
    *out = vs;
    return D3D_OK;
}

void q8_destroy_pixel_shader(Q8PixelShader *ps)
{
    if (!ps)
        return;
    if (ps->ps9)
        ps->ps9->Release();
    free(ps->func);
    free(ps);
}

HRESULT q8_create_pixel_shader(IDirect3DDevice9 *dev, const DWORD *func, DWORD max_version, Q8PixelShader **out)
{
    *out = NULL;
    if (!func)
        return D3DERR_INVALIDCALL;
    Q8ShaderLimits lim = {max_version, ~0u, ~0u};
    Q8Text msg = {NULL, 0, 0};
    UINT len = 0;
    HRESULT hr = q8_check_pixel_shader(func, &lim, &msg, &len);
    if (FAILED(hr)) {
        q8_log_rejected("pixel", &msg);
        q8_text_free(&msg);
        return D3DERR_INVALIDCALL;
    }
    q8_text_free(&msg);
    Q8PixelShader *ps = (Q8PixelShader *)calloc(1, sizeof(*ps));
    if (!ps)
        return E_OUTOFMEMORY;
    ps->func_count = len;
    ps->func = q8_dup_tokens(func, len);
    DWORD *code = q8_dup_tokens(func, len);
    if (!ps->func || !code) {
        free(code);
        q8_destroy_pixel_shader(ps);
        return E_OUTOFMEMORY;
    }
    if (code[0] == D3DPS_VERSION(1, 0))
        code[0] = D3DPS_VERSION(1, 1);
    hr = dev->CreatePixelShader(code, &ps->ps9);
    free(code);
    if (FAILED(hr)) {
        q8_log("qd3d8: CreatePixelShader (D3D9) failed hr=0x%08lx", (unsigned long)hr);
        q8_destroy_pixel_shader(ps);
        return hr;
    }
    *out = ps;
    return D3D_OK;
}
