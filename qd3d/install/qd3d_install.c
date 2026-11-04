/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qd3d_install: installs the QD3D Direct3D 9 proxy (d3d9.dll), Direct3D 8
 * front end (d3d8.dll) and DirectDraw/Direct3D 1-7 front end (ddraw.dll)
 * system-wide on Windows XP SP3 (x86), and reports the per-game wrapper DLLs
 * that would shadow them. README.md documents usage, the Windows File
 * Protection (WFP) strategy and rollback.
 *
 * Invariants:
 *  - A file is QD3D when it is an i386 PE DLL that contains the ASCII marker
 *    "QD3D" (the QD3D_MARKER RCDATA resource of current builds) and is not a
 *    Microsoft DLL. A Microsoft DLL names Microsoft as the company and a
 *    Microsoft product; Wine's DLLs name Microsoft Corporation as the company
 *    but Wine as the product, so they are foreign.
 *  - The stock DLL is kept as qd3d9_orig.dll / qd3d8_orig.dll /
 *    qddraw_orig.dll beside itself: in System32 (the front ends' fallback)
 *    and in each cache WFP restores from. Only a Microsoft DLL becomes a
 *    saved original; a saved original is never overwritten, and a stock file
 *    is removed only when a byte-identical saved original exists.
 *  - File changes are journaled until commit; the qxpmap service snapshot is
 *    restored around file rollback. DeleteService is the last fallible step.
 *    Restart-time replacements use new files that rollback deletes. A queued
 *    driver deletion is withdrawn before rollback restores the service.
 *
 * No C runtime: the imports are exactly the Win32 calls made here
 * ("make check-imports").
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <winver.h>

#define QP 520                 /* path buffer, WCHARs */
#define MAX_FILE (64u << 20)   /* larger files are never QD3D DLLs */
#define SCAN_DEPTH 12

enum { RC_OK = 0, RC_USAGE = 1, RC_FAILED = 2, RC_RESTART = 3, RC_REFUSED = 4 };

void qd3dinst_start(void) __attribute__((noreturn));

/* ------------------------------------------------------------------------ */
/* Output                                                                    */

static HANDLE g_out;
static int g_console;
static WCHAR g_ob[512];
static unsigned g_on;

static void out_flush(void)
{
    DWORD w;

    if (!g_on)
        return;
    if (g_console) {
        WriteConsoleW(g_out, g_ob, g_on, &w, NULL);
    } else {
        char a[sizeof g_ob]; /* two bytes per character covers every OEM code page */
        int n = WideCharToMultiByte(CP_OEMCP, 0, g_ob, (int)g_on, a, (int)sizeof a, NULL, NULL);

        if (n > 0)
            WriteFile(g_out, a, (DWORD)n, &w, NULL);
    }
    g_on = 0;
}

static void out_c(WCHAR c)
{
    if (g_on + 2 > ARRAYSIZE(g_ob))
        out_flush();
    if (c == L'\n' && !g_console)
        g_ob[g_on++] = L'\r';
    g_ob[g_on++] = c;
}

static void out_num(unsigned v, unsigned base)
{
    WCHAR t[12];
    int n = 0;

    do {
        unsigned d = v % base;

        t[n++] = (WCHAR)(d < 10 ? L'0' + d : L'a' + d - 10);
        v /= base;
    } while (v);
    while (n)
        out_c(t[--n]);
}

/* %s (const WCHAR *), %u, %x (unsigned), %% */
static void out_v(const WCHAR *f, va_list ap)
{
    const WCHAR *s;

    for (; *f; f++) {
        if (*f != L'%') {
            out_c(*f);
            continue;
        }
        switch (*++f) {
        case L's':
            for (s = va_arg(ap, const WCHAR *); s && *s; s++)
                out_c(*s);
            break;
        case L'u':
            out_num(va_arg(ap, unsigned), 10);
            break;
        case L'x':
            out_num(va_arg(ap, unsigned), 16);
            break;
        default:
            out_c(L'%');
            if (!*f)
                return;
            if (*f != L'%')
                out_c(*f);
            break;
        }
    }
}

static void outf(const WCHAR *f, ...)
{
    va_list ap;

    va_start(ap, f);
    out_v(f, ap);
    va_end(ap);
}

static void out_err(DWORD e)
{
    WCHAR m[200];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, m,
                             ARRAYSIZE(m), NULL);

    while (n && (m[n - 1] == L'\r' || m[n - 1] == L'\n' || m[n - 1] == L' ' || m[n - 1] == L'.'))
        n--;
    m[n] = 0;
    outf(L"error %u%s%s", (unsigned)e, n ? L": " : L"", m);
}

/* ------------------------------------------------------------------------ */
/* Memory and strings                                                        */

static HANDLE g_heap;

static void *xalloc(size_t n)
{
    void *p = HeapAlloc(g_heap, 0, n ? n : 1);

    if (!p) {
        outf(L"out of memory\n");
        out_flush();
        ExitProcess(RC_FAILED);
    }
    return p;
}

static void xfree(void *p)
{
    if (p)
        HeapFree(g_heap, 0, p);
}

static size_t wlen(const WCHAR *s)
{
    const WCHAR *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

/* Bounded copy and append; 0 (and an empty or unchanged result) when too long. */
static int wcpy(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t n = wlen(s);

    if (n >= cap) {
        if (cap)
            d[0] = 0;
        return 0;
    }
    memcpy(d, s, (n + 1) * sizeof *d);
    return 1;
}

static int wcat(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t a = wlen(d);

    return a < cap && wcpy(d + a, cap - a, s);
}

static int wcat_u(WCHAR *d, size_t cap, unsigned v)
{
    WCHAR t[12];
    int n = 0, i = 0;
    WCHAR r[12];

    do {
        t[n++] = (WCHAR)(L'0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        r[i++] = t[--n];
    r[i] = 0;
    return wcat(d, cap, r);
}

/* dir\name */
static int wjoin(WCHAR *d, size_t cap, const WCHAR *dir, const WCHAR *name)
{
    size_t n;

    if (!wcpy(d, cap, dir))
        return 0;
    n = wlen(d);
    if (n && d[n - 1] != L'\\' && !wcat(d, cap, L"\\"))
        return 0;
    return wcat(d, cap, name);
}

static WCHAR lc(WCHAR c)
{
    return c >= L'A' && c <= L'Z' ? (WCHAR)(c + (L'a' - L'A')) : c;
}

/* Path names compare case-insensitively in ASCII and exactly otherwise. */
static int wieq(const WCHAR *a, const WCHAR *b)
{
    while (*a && lc(*a) == lc(*b))
        a++, b++;
    return lc(*a) == lc(*b);
}

static int wistarts(const WCHAR *s, const WCHAR *prefix)
{
    while (*prefix && lc(*s) == lc(*prefix))
        s++, prefix++;
    return !*prefix;
}

static void trim_slash(WCHAR *p)
{
    size_t n = wlen(p);

    while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/'))
        p[--n] = 0;
}

/* Directory part of path, in place. */
static void dir_of(WCHAR *path)
{
    size_t n = wlen(path);

    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/')
        n--;
    if (n > 1)
        n--;
    path[n] = 0;
    trim_slash(path);
}

static int full_path(const WCHAR *in, WCHAR *out)
{
    DWORD n = GetFullPathNameW(in, QP, out, NULL);

    if (!n || n >= QP)
        return 0;
    trim_slash(out);
    return 1;
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int fixed_drive(const WCHAR *p)
{
    WCHAR r[4];

    if (!p[0] || p[1] != L':')
        return 0;
    r[0] = p[0];
    r[1] = L':';
    r[2] = L'\\';
    r[3] = 0;
    return GetDriveTypeW(r) == DRIVE_FIXED;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */

struct slot {
    const WCHAR *name; /* the System32 DLL */
    const WCHAR *orig; /* its stock copy */
    const char *entry; /* export the QD3D replacement must have */
    const WCHAR *what;
};

static const struct slot k_slots[] = {
    {L"d3d9.dll", L"qd3d9_orig.dll", "Direct3DCreate9", L"Direct3D 9 proxy"},
    {L"d3d8.dll", L"qd3d8_orig.dll", "Direct3DCreate8", L"Direct3D 8 front end"},
    {L"ddraw.dll", L"qddraw_orig.dll", "DirectDrawCreate", L"DirectDraw front end"},
};
#define NSLOT ARRAYSIZE(k_slots)

/* Per-game DLLs a game loads instead of the system-wide ones. */
static const WCHAR *const k_wrappers[] = {
    L"d3d8.dll",  L"d3d9.dll",   L"ddraw.dll",      L"opengl32.dll", L"wined3d.dll",
    L"libwine.dll", L"dxgi.dll", L"d3d10.dll",      L"d3d10_1.dll",  L"d3d10core.dll",
    L"d3d11.dll", L"d3dim.dll",  L"d3dim700.dll",   L"d3dimm.dll",
};

/* Top-level directories of a drive that are not game roots. */
static const WCHAR *const k_skip_top[] = {
    L"WINDOWS",       L"WINNT",        L"Documents and Settings", L"Users",
    L"RECYCLER",      L"$Recycle.Bin", L"System Volume Information", L"Recovery",
    L"Config.Msi",    L"MSOCache",     L"ProgramData",             L"Windows.old",
    L"$WINDOWS.~BT",  L"$WINDOWS.~WS", L"$WinREAgent",             L"PerfLogs",
};

#define BAK_SUFFIX L".qd3d-bak"

/* A directory WFP restores protected files from. */
struct loc {
    WCHAR dir[QP];
    const WCHAR *label;
};

static struct loc g_loc[3];
static unsigned g_nloc;

static struct {
    int root_mode, xp, wfp_deny, move, restore, restart, scm_busy, scm_pending, rollback_failed;
    unsigned fail_after, ops, nscan;
    const WCHAR *scm_fail;
    WCHAR root[QP], windir[QP], sys[QP], from[QP], self_dir[QP];
    WCHAR sp_reg[QP], srcpath[QP], pendfile[QP], drivers[QP], driver[QP], scmfile[QP];
    WCHAR scan[8][QP];
} C;

/* ------------------------------------------------------------------------ */
/* Files                                                                     */

/* K_OTHER: a Microsoft DLL when microsoft is set, else a foreign one. */
enum kind { K_ABSENT, K_DIR, K_UNREADABLE, K_QD3D, K_OTHER };

struct finfo {
    enum kind kind;
    DWORD err, attrs, size;
    BYTE *data;
    int microsoft, has_file_version;
    DWORD file_version_ms, file_version_ls;
    WCHAR company[64], product[96], desc[96], version[48], original[48];
};

static int read_all(const WCHAR *path, BYTE **data, DWORD *size, DWORD *err)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    DWORD total = 0, got;
    BYTE *buf;

    *data = NULL;
    *size = 0;
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return 0;
    }
    if (!GetFileSizeEx(h, &sz)) {
        *err = GetLastError();
        CloseHandle(h);
        return 0;
    }
    if (sz.QuadPart > MAX_FILE) {
        *err = ERROR_FILE_TOO_LARGE;
        CloseHandle(h);
        return 0;
    }
    buf = xalloc((size_t)sz.QuadPart);
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, buf + total, (DWORD)sz.QuadPart - total, &got, NULL) || !got) {
            *err = got ? GetLastError() : ERROR_HANDLE_EOF;
            if (!*err)
                *err = ERROR_READ_FAULT;
            xfree(buf);
            CloseHandle(h);
            return 0;
        }
        total += got;
    }
    CloseHandle(h);
    *data = buf;
    *size = total;
    return 1;
}

static DWORD rd32(const BYTE *p)
{
    return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
}

static WORD rd16(const BYTE *p)
{
    return (WORD)(p[0] | p[1] << 8);
}

struct pe {
    const BYTE *d;
    DWORD n, sec, exp_rva;
    WORD nsec;
};

/* An i386 PE32 DLL, or a native kernel image, with consistent headers. */
static int pe_open(const BYTE *d, DWORD n, struct pe *pe, int native)
{
    DWORD nt, opt;
    WORD osz;

    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z')
        return 0;
    nt = rd32(d + 0x3c);
    if (nt > n || n - nt < 4 + sizeof(IMAGE_FILE_HEADER) || memcmp(d + nt, "PE\0\0", 4))
        return 0;
    if (rd16(d + nt + 4 + offsetof(IMAGE_FILE_HEADER, Machine)) != IMAGE_FILE_MACHINE_I386 ||
        !(rd16(d + nt + 4 + offsetof(IMAGE_FILE_HEADER, Characteristics)) &
          (native ? IMAGE_FILE_EXECUTABLE_IMAGE : IMAGE_FILE_DLL)))
        return 0;
    pe->nsec = rd16(d + nt + 4 + offsetof(IMAGE_FILE_HEADER, NumberOfSections));
    osz = rd16(d + nt + 4 + offsetof(IMAGE_FILE_HEADER, SizeOfOptionalHeader));
    opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    if (osz < offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) + sizeof(IMAGE_DATA_DIRECTORY) || n - opt < osz ||
        rd16(d + opt) != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 0;
    if (native && rd16(d + opt + offsetof(IMAGE_OPTIONAL_HEADER32, Subsystem)) != IMAGE_SUBSYSTEM_NATIVE)
        return 0;
    pe->exp_rva = rd32(d + opt + offsetof(IMAGE_OPTIONAL_HEADER32, NumberOfRvaAndSizes))
                      ? rd32(d + opt + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory))
                      : 0;
    pe->sec = opt + osz;
    if ((n - pe->sec) / sizeof(IMAGE_SECTION_HEADER) < pe->nsec)
        return 0;
    pe->d = d;
    pe->n = n;
    return 1;
}

/* File offset of [rva, rva + len) when the file holds all of it. */
static int pe_map(const struct pe *pe, DWORD rva, DWORD len, DWORD *off)
{
    WORD i;

    for (i = 0; i < pe->nsec; i++) {
        const BYTE *s = pe->d + pe->sec + i * sizeof(IMAGE_SECTION_HEADER);
        DWORD va = rd32(s + offsetof(IMAGE_SECTION_HEADER, VirtualAddress));
        DWORD raw = rd32(s + offsetof(IMAGE_SECTION_HEADER, SizeOfRawData));
        DWORD ptr = rd32(s + offsetof(IMAGE_SECTION_HEADER, PointerToRawData));

        if (rva >= va && rva - va < raw && len <= raw - (rva - va)) {
            DWORD o = ptr + (rva - va);

            if (o < ptr || o > pe->n || len > pe->n - o)
                return 0;
            *off = o;
            return 1;
        }
    }
    return 0;
}

static int pe_exports(const BYTE *d, DWORD n, const char *name)
{
    struct pe pe;
    DWORD eo, no, cnt, i, len = 0;

    if (!d || !pe_open(d, n, &pe, 0) || !pe.exp_rva || !pe_map(&pe, pe.exp_rva, sizeof(IMAGE_EXPORT_DIRECTORY), &eo))
        return 0;
    cnt = rd32(d + eo + offsetof(IMAGE_EXPORT_DIRECTORY, NumberOfNames));
    if (cnt > 0x10000 || !pe_map(&pe, rd32(d + eo + offsetof(IMAGE_EXPORT_DIRECTORY, AddressOfNames)), cnt * 4, &no))
        return 0;
    while (name[len])
        len++;
    for (i = 0; i < cnt; i++) {
        DWORD so;

        if (pe_map(&pe, rd32(d + no + 4 * i), len + 1, &so) && !memcmp(d + so, name, len + 1))
            return 1;
    }
    return 0;
}

static int has_marker(const BYTE *d, DWORD n)
{
    DWORD i;

    for (i = 0; i + 4 <= n; i++)
        if (d[i] == 'Q' && d[i + 1] == 'D' && d[i + 2] == '3' && d[i + 3] == 'D')
            return 1;
    return 0;
}

static void hex4(WCHAR *p, unsigned v)
{
    int i;

    for (i = 3; i >= 0; i--, v >>= 4)
        p[i] = (WCHAR)((v & 15) < 10 ? L'0' + (v & 15) : L'a' + (v & 15) - 10);
}

static int ver_string(const void *vi, unsigned lang, unsigned cp, const WCHAR *key, WCHAR *out, size_t cap)
{
    WCHAR q[64];
    LPVOID v;
    UINT n;
    size_t i;

    out[0] = 0;
    wcpy(q, ARRAYSIZE(q), L"\\StringFileInfo\\00000000\\");
    hex4(q + 16, lang);
    hex4(q + 20, cp);
    if (!wcat(q, ARRAYSIZE(q), key) || !VerQueryValueW(vi, q, &v, &n) || !v)
        return 0;
    for (i = 0; i + 1 < cap && i < n && ((const WCHAR *)v)[i]; i++)
        out[i] = ((const WCHAR *)v)[i];
    out[i] = 0;
    return 1;
}

static int wcontains(const WCHAR *s, const WCHAR *sub)
{
    for (; *s; s++)
        if (wistarts(s, sub))
            return 1;
    return 0;
}

static void ver_info(const WCHAR *path, struct finfo *f)
{
    static const WCHAR *const keys[] = {L"CompanyName", L"ProductName", L"FileDescription", L"FileVersion",
                                       L"OriginalFilename"};
    WCHAR *const outs[] = {f->company, f->product, f->desc, f->version, f->original};
    const size_t caps[] = {ARRAYSIZE(f->company), ARRAYSIZE(f->product), ARRAYSIZE(f->desc),
                           ARRAYSIZE(f->version), ARRAYSIZE(f->original)};
    /* The declared translation first, then the usual US English ones. */
    unsigned lang[4][2] = {{0x0409, 0x04b0}, {0x0409, 0x04b0}, {0x0409, 0x04e4}, {0x0000, 0x04b0}};
    DWORD dummy, n = GetFileVersionInfoSizeW(path, &dummy);
    void *vi;
    LPVOID tr;
    UINT trn;
    unsigned k, i;

    if (!n || n > (1u << 20))
        return;
    vi = xalloc(n);
    if (GetFileVersionInfoW(path, 0, n, vi)) {
        if (VerQueryValueW(vi, L"\\", &tr, &trn) && tr && trn >= sizeof(VS_FIXEDFILEINFO)) {
            const VS_FIXEDFILEINFO *fixed = tr;

            if (fixed->dwSignature == VS_FFI_SIGNATURE) {
                f->has_file_version = 1;
                f->file_version_ms = fixed->dwFileVersionMS;
                f->file_version_ls = fixed->dwFileVersionLS;
            }
        }
        if (VerQueryValueW(vi, L"\\VarFileInfo\\Translation", &tr, &trn) && tr && trn >= 4) {
            lang[0][0] = ((const WORD *)tr)[0];
            lang[0][1] = ((const WORD *)tr)[1];
        }
        for (k = 0; k < ARRAYSIZE(keys); k++)
            for (i = 0; !outs[k][0] && i < ARRAYSIZE(lang); i++)
                ver_string(vi, lang[i][0], lang[i][1], keys[k], outs[k], caps[k]);
    }
    xfree(vi);
    /* Windows names its product "Microsoft(R) Windows(R) Operating System"
     * (localized builds translate the words around the name). Wine's DLLs,
     * and so WineD3D's, give "Microsoft Corporation" as the company and
     * "Wine" as the product. */
    f->microsoft = wistarts(f->company, L"Microsoft") && wcontains(f->product, L"Microsoft");
}

static void probe(const WCHAR *path, struct finfo *f)
{
    struct pe pe;

    memset(f, 0, sizeof *f);
    f->attrs = GetFileAttributesW(path);
    if (f->attrs == INVALID_FILE_ATTRIBUTES) {
        f->err = GetLastError();
        f->kind = f->err == ERROR_FILE_NOT_FOUND || f->err == ERROR_PATH_NOT_FOUND ? K_ABSENT : K_UNREADABLE;
        return;
    }
    if (f->attrs & FILE_ATTRIBUTE_DIRECTORY) {
        f->kind = K_DIR;
        return;
    }
    if (!read_all(path, &f->data, &f->size, &f->err)) {
        f->kind = K_UNREADABLE;
        return;
    }
    ver_info(path, f);
    f->kind = !f->microsoft && pe_open(f->data, f->size, &pe, 0) && has_marker(f->data, f->size) ? K_QD3D : K_OTHER;
}

static void unprobe(struct finfo *f)
{
    xfree(f->data);
    f->data = NULL;
}

static int same(const struct finfo *a, const struct finfo *b)
{
    return a->data && b->data && a->size == b->size && !memcmp(a->data, b->data, a->size);
}

static int driver_image(const struct finfo *f)
{
    struct pe pe;

    return f->data && pe_open(f->data, f->size, &pe, 1) && wieq(f->original, L"qxpmap.sys") &&
           (wieq(f->product, L"XPDriver") || (C.root_mode && wieq(f->product, L"XPDriver test fixture")));
}

/* "QD3D: QD3D Direct3D 8 front end 0.1.0.0, 270937 bytes",
 * "Microsoft: Microsoft DirectDraw 5.03.2600.5512 (xpsp...), 279552 bytes",
 * "foreign: Microsoft Corporation, Wine, Wine DirectDraw 5.3.1.904, ..." */
static void out_finfo(const struct finfo *f)
{
    int foreign = f->kind == K_OTHER && !f->microsoft, any = 0;

    switch (f->kind) {
    case K_ABSENT:
        outf(L"absent");
        return;
    case K_DIR:
        outf(L"a directory");
        return;
    case K_UNREADABLE:
        outf(L"unreadable, ");
        out_err(f->err);
        return;
    case K_QD3D:
        outf(L"QD3D");
        break;
    case K_OTHER:
        outf(foreign ? L"foreign" : L"Microsoft");
        break;
    }
    /* Who a foreign file says it comes from. */
    if (foreign && f->company[0])
        outf(L"%s%s", any++ ? L", " : L": ", f->company);
    if (foreign && f->product[0])
        outf(L"%s%s", any++ ? L", " : L": ", f->product);
    if (f->desc[0] || f->version[0])
        outf(L"%s%s%s%s", any++ ? L", " : L": ", f->desc, f->desc[0] && f->version[0] ? L" " : L"", f->version);
    if (foreign && !any)
        outf(L": no version information");
    outf(L", %u bytes", (unsigned)f->size);
}

/* ------------------------------------------------------------------------ */
/* System paths                                                              */

static int reg_path(const WCHAR *key, const WCHAR *value, WCHAR *out)
{
    WCHAR raw[QP];
    DWORD type, size = sizeof raw - sizeof raw[0], n;
    HKEY k;
    int ok = 0;

    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, value, NULL, &type, (BYTE *)raw, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        raw[size / sizeof raw[0]] = 0;
        n = ExpandEnvironmentStringsW(raw, out, QP);
        ok = n && n <= QP && out[0];
    }
    RegCloseKey(k);
    if (!ok)
        out[0] = 0;
    trim_slash(out);
    return ok;
}

static void add_loc(const WCHAR *dir, const WCHAR *label)
{
    unsigned i;

    if (!dir[0] || g_nloc == ARRAYSIZE(g_loc) || wieq(dir, C.sys) || !dir_exists(dir) ||
        (!C.root_mode && !fixed_drive(dir)))
        return;
    for (i = 0; i < g_nloc; i++)
        if (wieq(g_loc[i].dir, dir))
            return;
    wcpy(g_loc[g_nloc].dir, QP, dir);
    g_loc[g_nloc++].label = label;
}

static int setup_paths(void)
{
    static const WCHAR setup_key[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Setup";
    WCHAR p[QP];
    UINT n;

    if (C.root_mode) {
        if (!wjoin(C.windir, QP, C.root, L"WINDOWS") || !wjoin(C.sys, QP, C.windir, L"system32") ||
            !wjoin(C.pendfile, QP, C.root, L"PendingFileRenameOperations.bin") || !dir_exists(C.sys)) {
            outf(L"%s\\WINDOWS\\system32 does not exist\n", C.root);
            return 0;
        }
        if (wjoin(p, QP, C.sys, L"dllcache"))
            add_loc(p, L"dllcache");
        if (wjoin(p, QP, C.windir, L"ServicePackFiles\\i386"))
            add_loc(p, L"ServicePackFiles");
        return 1;
    }
    n = GetSystemDirectoryW(C.sys, QP);
    if (!n || n >= QP)
        return 0;
    n = GetSystemWindowsDirectoryW(C.windir, QP);
    if (!n || n >= QP)
        return 0;
    trim_slash(C.sys);
    trim_slash(C.windir);
    /* KB 222193: the policy value takes precedence over the Winlogon one. */
    if (!reg_path(L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\Windows File Protection", L"SFCDllCacheDir", p) &&
        !reg_path(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"SFCDllCacheDir", p))
        wjoin(p, QP, C.sys, L"dllcache");
    add_loc(p, L"dllcache");
    if (reg_path(setup_key, L"ServicePackSourcePath", C.sp_reg) && wjoin(p, QP, C.sp_reg, L"i386"))
        add_loc(p, L"ServicePackFiles");
    if (wjoin(p, QP, C.windir, L"ServicePackFiles\\i386"))
        add_loc(p, L"ServicePackFiles");
    reg_path(setup_key, L"SourcePath", C.srcpath);
    return 1;
}

static int is_xp_x86(void)
{
    typedef BOOL(WINAPI * iswow64_fn)(HANDLE, PBOOL);
    OSVERSIONINFOW v;
    iswow64_fn iswow64;
    BOOL wow = FALSE;

    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (!GetVersionExW(&v))
        return 0;
    iswow64 = (iswow64_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (iswow64 && !iswow64(GetCurrentProcess(), &wow))
        wow = FALSE;
    return v.dwPlatformId == VER_PLATFORM_WIN32_NT && v.dwMajorVersion == 5 && v.dwMinorVersion == 1 && !wow;
}

/* ------------------------------------------------------------------------ */
/* Windows File Protection                                                   */

typedef BOOL(WINAPI *sfc_protected_fn)(HANDLE, LPCWSTR);
typedef DWORD(WINAPI *sfc_exception_fn)(DWORD, PWCHAR, DWORD);
static sfc_protected_fn g_sfc_protected;
static sfc_exception_fn g_sfc_exception;

static void wfp_init(void)
{
    HMODULE m;

    if (C.root_mode || !C.xp)
        return;
    m = LoadLibraryW(L"sfc_os.dll");
    if (m) {
        g_sfc_protected = (sfc_protected_fn)(void (*)(void))GetProcAddress(m, "SfcIsFileProtected");
        /* SfcFileException: unnamed export 5 of the Windows XP sfc_os.dll. */
        g_sfc_exception = (sfc_exception_fn)(void (*)(void))GetProcAddress(m, MAKEINTRESOURCEA(5));
    }
    if (!g_sfc_protected && (m = LoadLibraryW(L"sfc.dll")) != NULL)
        g_sfc_protected = (sfc_protected_fn)(void (*)(void))GetProcAddress(m, "SfcIsFileProtected");
}

/* 1: protected, 0: not protected, -1: unknown. Only asked for System32 targets. */
static int wfp_protected(const WCHAR *path)
{
    if (C.root_mode)
        return 1; /* the fake System32 DLLs stand for protected files */
    if (!g_sfc_protected)
        return -1;
    return g_sfc_protected(NULL, path) ? 1 : 0;
}

/* Asks WFP to leave path alone for about a minute; 1 when it may change now. */
static int wfp_suspend(const WCHAR *path)
{
    WCHAR p[QP];
    DWORD r;

    if (C.root_mode) {
        outf(L"    WFP exception for %s (simulated%s)\n", path, C.wfp_deny ? L", refused" : L"");
        return !C.wfp_deny;
    }
    if (!g_sfc_exception) {
        outf(L"    WFP exception for %s: sfc_os.dll ordinal 5 not available\n", path);
        return 0;
    }
    wcpy(p, QP, path);
    r = g_sfc_exception(0, p, (DWORD)-1);
    if (r)
        outf(L"    WFP exception for %s failed (%u)\n", path, (unsigned)r);
    else
        outf(L"    WFP exception for %s\n", path);
    return r == 0;
}

/* ------------------------------------------------------------------------ */
/* Restart-time renames (PendingFileRenameOperations)                       */

struct pend {
    WCHAR *src, *dst; /* dst empty: delete */
    int replace;
};

static struct pend *g_pend;
static unsigned g_npend, g_cpend;

static WCHAR *wdup(const WCHAR *s)
{
    size_t n = wlen(s) + 1;
    WCHAR *d = xalloc(n * sizeof *d);

    memcpy(d, s, n * sizeof *d);
    return d;
}

static const WCHAR *strip_nt(const WCHAR *s)
{
    return s[0] == L'\\' && s[1] == L'?' && s[2] == L'?' && s[3] == L'\\' ? s + 4 : s;
}

static void pend_add(const WCHAR *src, const WCHAR *dst)
{
    int replace = dst[0] == L'!';

    if (!src[0])
        return;
    if (g_npend == g_cpend) {
        struct pend *n;

        g_cpend = g_cpend ? g_cpend * 2 : 8;
        n = xalloc(g_cpend * sizeof *n);
        if (g_npend)
            memcpy(n, g_pend, g_npend * sizeof *n);
        xfree(g_pend);
        g_pend = n;
    }
    g_pend[g_npend].src = wdup(strip_nt(src));
    g_pend[g_npend].dst = wdup(strip_nt(dst + replace));
    g_pend[g_npend++].replace = replace;
}

/* REG_MULTI_SZ-like pairs; deletions make empty strings, so walk the size. */
static void pend_parse(const BYTE *raw, DWORD bytes)
{
    const WCHAR *w = (const WCHAR *)raw, *first = NULL;
    size_t n = bytes / sizeof(WCHAR), i = 0;

    while (i < n) {
        size_t j = i;

        while (j < n && w[j])
            j++;
        if (j == n)
            break;
        if (!first) {
            first = w + i;
        } else {
            pend_add(first, w + i);
            first = NULL;
        }
        i = j + 1;
    }
}

static void pend_load(void)
{
    BYTE *raw = NULL;
    DWORD n = 0, type, err;
    HKEY k;

    if (C.root_mode) {
        if (read_all(C.pendfile, &raw, &n, &err))
            pend_parse(raw, n);
        xfree(raw);
        return;
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE,
                      &k) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, NULL, &n) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && n) {
        raw = xalloc(n);
        if (RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, raw, &n) == ERROR_SUCCESS)
            pend_parse(raw, n);
        xfree(raw);
    }
    RegCloseKey(k);
}

static int pend_mentions(const WCHAR *p)
{
    unsigned i;

    for (i = 0; i < g_npend; i++)
        if (wieq(g_pend[i].src, p) || wieq(g_pend[i].dst, p))
            return 1;
    return 0;
}

/* The files this program manages: System32 DLLs, saved originals, cache copies. */
static int managed(const WCHAR *p)
{
    WCHAR m[QP];
    unsigned i, l;

    if (wieq(C.driver, p))
        return 1;
    for (i = 0; i < NSLOT; i++) {
        if ((wjoin(m, QP, C.sys, k_slots[i].name) && wieq(m, p)) ||
            (wjoin(m, QP, C.sys, k_slots[i].orig) && wieq(m, p)))
            return 1;
        for (l = 0; l < g_nloc; l++)
            if ((wjoin(m, QP, g_loc[l].dir, k_slots[i].name) && wieq(m, p)) ||
                (wjoin(m, QP, g_loc[l].dir, k_slots[i].orig) && wieq(m, p)))
                return 1;
    }
    return 0;
}

/* A managed file or one of its temporary names (NAME.qd3d-new, NAME.qd3d-old...). */
static int ours(const WCHAR *p)
{
    WCHAR base[QP];
    const WCHAR *s;

    if (!p[0])
        return 0;
    if (managed(p))
        return 1;
    for (s = p; *s && !wistarts(s, L".qd3d-"); s++)
        ;
    if (!*s || (size_t)(s - p) >= QP)
        return 0;
    memcpy(base, p, (size_t)(s - p) * sizeof *base);
    base[s - p] = 0;
    return managed(base);
}

/* A pending operation that would change a managed file at the next restart. */
static const struct pend *pend_blocking(void)
{
    unsigned i;

    for (i = 0; i < g_npend; i++)
        if (managed(g_pend[i].src) || (g_pend[i].dst[0] && managed(g_pend[i].dst)))
            return &g_pend[i];
    return NULL;
}

static void out_pend(const struct pend *p)
{
    if (p->dst[0])
        outf(L"%s replaces %s", p->src, p->dst);
    else
        outf(L"%s is deleted", p->src);
}

/* dst NULL: delete src at the next restart; otherwise src replaces dst. */
static int schedule(const WCHAR *src, const WCHAR *dst)
{
    WCHAR rec[2 * QP + 16];
    size_t n;
    HANDLE h;
    DWORD w;
    BOOL ok;

    if (!C.root_mode)
        return MoveFileExW(src, dst, MOVEFILE_DELAY_UNTIL_REBOOT | (dst ? MOVEFILE_REPLACE_EXISTING : 0));
    /* Test root: the same record MoveFileEx writes, in a file of the root. */
    if (!wcpy(rec, ARRAYSIZE(rec), L"\\??\\") || !wcat(rec, ARRAYSIZE(rec), src))
        return 0;
    n = wlen(rec) + 1;
    if (dst && (!wcpy(rec + n, ARRAYSIZE(rec) - n, L"!\\??\\") || !wcat(rec + n, ARRAYSIZE(rec) - n, dst)))
        return 0;
    if (!dst)
        rec[n] = 0;
    n += wlen(rec + n) + 1;
    h = CreateFileW(C.pendfile, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, rec, (DWORD)(n * sizeof rec[0]), &w, NULL) && w == n * sizeof rec[0];
    CloseHandle(h);
    if (ok)
        pend_parse((const BYTE *)rec, (DWORD)(n * sizeof rec[0]));
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Journaled changes                                                         */

enum { J_RENAMED, J_CREATED };

struct jent {
    int op;
    WCHAR a[QP], b[QP]; /* J_RENAMED: a was renamed to b; J_CREATED: a is new */
};

/* Per DLL at most 11 journaled changes (new file, stray saved copy, recovered
 * original, two per WFP cache, two at the target) and 8 files to delete
 * (stray saved copy, two per WFP cache, the old target). */
#define JMAX (NSLOT * 16 + 8)
static struct jent g_j[JMAX];
static unsigned g_nj;

struct later {
    WCHAR src[QP], dst[QP];
};
static struct later g_later[NSLOT + 1];
static unsigned g_nlater;
static WCHAR g_trash[NSLOT * 12 + 1][QP];
static unsigned g_ntrash;
/* Saved originals whose verified copy replaces the target at the next
 * restart: deleted at the restart, after the replacement. */
static WCHAR g_bootdel[NSLOT][QP];
static unsigned g_nbootdel;

static int injected(void)
{
    C.ops++;
    if (C.fail_after && C.ops == C.fail_after) {
        outf(L"    [test] injected failure at step %u\n", C.ops);
        SetLastError(ERROR_GEN_FAILURE);
        return 1;
    }
    return 0;
}

/* All SCM access is confined to svc_call. --root selects the persisted fake
 * before opening any SCM handle, including for queries and rollback. */
enum svc_op { S_READ, S_CREATE, S_CONFIG, S_START, S_STOP, S_DELETE };
static const WCHAR *const k_svc_ops[] = {L"query", L"create", L"config", L"start", L"stop", L"delete"};
static const WCHAR k_driver_image[] = L"\\SystemRoot\\system32\\drivers\\qxpmap.sys";

static int driver_service_image(const WCHAR *image)
{
    return wieq(image, k_driver_image) ||
           wieq(image, L"system32\\drivers\\qxpmap.sys") ||
           wieq(image, L"%SystemRoot%\\system32\\drivers\\qxpmap.sys") ||
           wieq(strip_nt(image), C.driver);
}

/* --root represents this registry key by a directory, never the host hive. */
static int driver_opengl_user(int *present)
{
    static const WCHAR key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\qxpicd";
    HKEY k;
    LONG e;

    *present = 0;
    if (C.root_mode) {
        WCHAR path[QP];
        DWORD attrs;

        if (!wjoin(path, QP, C.root, L"registry\\HKLM\\") || !wcat(path, QP, key)) {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return 0;
        }
        attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                SetLastError(ERROR_INVALID_DATA);
                return 0;
            }
            *present = 1;
            return 1;
        }
        e = (LONG)GetLastError();
    } else {
        e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &k);
        if (e == ERROR_SUCCESS) {
            RegCloseKey(k);
            *present = 1;
            return 1;
        }
    }
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
        return 1;
    SetLastError((DWORD)e);
    return 0;
}

struct svc_state {
    DWORD magic, present, state, type, start, error, pending;
    WCHAR image[QP];
};

static struct {
    struct svc_state before, now;
    struct finfo source, target;
    WCHAR src[QP];
    int active, changed, started, uninstall, deferred, delete_queued;
} D;

static int svc_fake(enum svc_op op, struct svc_state *s, int restoring)
{
    HANDLE h;
    BYTE *raw = NULL;
    DWORD n = 0, e = 0, w;
    BOOL ok;

    outf(L"    SCM (simulated): %s qxpmap\n", k_svc_ops[op]);
    if (!restoring && C.scm_fail && wieq(C.scm_fail, k_svc_ops[op])) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (op == S_READ) {
        memset(s, 0, sizeof *s);
        if (!read_all(C.scmfile, &raw, &n, &e)) {
            if (e == ERROR_FILE_NOT_FOUND)
                return 1;
            SetLastError(e);
            return 0;
        }
        ok = n == sizeof *s;
        if (ok) {
            memcpy(s, raw, sizeof *s);
            ok = s->magic == 0x31534351 && s->image[QP - 1] == 0;
        }
        xfree(raw);
        if (!ok)
            SetLastError(ERROR_INVALID_DATA);
        return ok;
    }
    if (op == S_CREATE) {
        s->magic = 0x31534351;
        s->present = 1;
        s->pending = 0;
        s->state = SERVICE_STOPPED;
    } else if (op == S_STOP) {
        if (!restoring && C.scm_busy && s->state != SERVICE_STOPPED) {
            SetLastError(ERROR_BUSY);
            return 0;
        }
        s->state = SERVICE_STOPPED;
    } else if (op == S_START) {
        if (GetFileAttributesW(C.driver) == INVALID_FILE_ATTRIBUTES) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return 0;
        }
        s->state = SERVICE_RUNNING;
    } else if (op == S_DELETE) {
        s->pending = s->state != SERVICE_STOPPED || (!restoring && C.scm_pending);
        s->present = s->pending;
    }
    if (!s->present)
        return DeleteFileW(C.scmfile) || GetLastError() == ERROR_FILE_NOT_FOUND;
    h = CreateFileW(C.scmfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, s, sizeof *s, &w, NULL) && w == sizeof *s;
    e = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok)
        SetLastError(e ? e : ERROR_WRITE_FAULT);
    return ok;
}

static int svc_call(enum svc_op op, struct svc_state *s, int restoring)
{
    SC_HANDLE manager, service;
    SERVICE_STATUS status;
    union {
        QUERY_SERVICE_CONFIGW config;
        BYTE bytes[8192];
    } config;
    DWORD access, e = ERROR_SUCCESS, needed, wait;
    BOOL ok = FALSE;

    if (C.root_mode)
        return svc_fake(op, s, restoring);
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | (op == S_CREATE ? SC_MANAGER_CREATE_SERVICE : 0));
    if (!manager)
        return 0;
    access = SERVICE_QUERY_STATUS;
    if (op == S_READ)
        access |= SERVICE_QUERY_CONFIG;
    else if (op == S_CONFIG)
        access |= SERVICE_CHANGE_CONFIG;
    else if (op == S_START)
        access |= SERVICE_START;
    else if (op == S_STOP)
        access |= SERVICE_STOP;
    else if (op == S_DELETE)
        access |= DELETE;
    if (op == S_CREATE) {
        service = CreateServiceW(manager, L"qxpmap", L"XPDriver guest memory-window driver", access,
                                 SERVICE_KERNEL_DRIVER, s->start, SERVICE_ERROR_NORMAL, k_driver_image,
                                 NULL, NULL, NULL, NULL, NULL);
    } else {
        service = OpenServiceW(manager, L"qxpmap", access);
    }
    if (!service) {
        e = GetLastError();
        if (op == S_READ && (e == ERROR_SERVICE_DOES_NOT_EXIST || e == ERROR_SERVICE_MARKED_FOR_DELETE)) {
            memset(s, 0, sizeof *s);
            s->present = s->pending = e == ERROR_SERVICE_MARKED_FOR_DELETE;
            ok = TRUE;
        } else if (op == S_DELETE && (e == ERROR_SERVICE_DOES_NOT_EXIST || e == ERROR_SERVICE_MARKED_FOR_DELETE)) {
            s->present = s->pending = e == ERROR_SERVICE_MARKED_FOR_DELETE;
            ok = TRUE;
        }
        goto done;
    }
    if (op == S_READ) {
        if (!QueryServiceConfigW(service, &config.config, sizeof config, &needed) ||
            !QueryServiceStatus(service, &status))
            goto service_done;
        memset(s, 0, sizeof *s);
        s->present = 1;
        s->state = status.dwCurrentState;
        s->type = config.config.dwServiceType;
        s->start = config.config.dwStartType;
        s->error = config.config.dwErrorControl;
        ok = wcpy(s->image, QP, config.config.lpBinaryPathName);
    } else if (op == S_CONFIG) {
        ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, s->start, SERVICE_NO_CHANGE, NULL,
                                  NULL, NULL, NULL, NULL, NULL, NULL);
    } else if (op == S_CREATE) {
        s->present = 1;
        s->state = SERVICE_STOPPED;
        ok = TRUE;
    } else if (op == S_DELETE) {
        ok = DeleteService(service);
        if (!ok && GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE)
            ok = TRUE;
        if (ok) {
            /* Close our handle before checking whether another handle or a
             * running driver keeps the service marked for deletion. */
            CloseServiceHandle(service);
            service = OpenServiceW(manager, L"qxpmap", SERVICE_QUERY_STATUS);
            e = service ? ERROR_SERVICE_MARKED_FOR_DELETE : GetLastError();
            s->present = s->pending = e != ERROR_SERVICE_DOES_NOT_EXIST;
            if (service)
                CloseServiceHandle(service);
            goto done;
        }
    } else {
        if (op == S_START) {
            ok = StartServiceW(service, 0, NULL);
            if (!ok && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                ok = TRUE;
        } else {
            ok = ControlService(service, SERVICE_CONTROL_STOP, &status);
            if (!ok && GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
                ok = TRUE;
        }
        if (ok) {
            DWORD desired = op == S_START ? SERVICE_RUNNING : SERVICE_STOPPED;
            for (wait = 0; wait < 100; wait++) {
                if (!QueryServiceStatus(service, &status)) {
                    ok = FALSE;
                    break;
                }
                s->state = status.dwCurrentState;
                if (s->state == desired)
                    break;
                if (op == S_START && s->state == SERVICE_STOPPED) {
                    SetLastError(status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
                    ok = FALSE;
                    break;
                }
                Sleep(100);
            }
            if (ok && s->state != desired) {
                SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
                ok = FALSE;
            }
        }
    }
service_done:
    e = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
done:
    CloseServiceHandle(manager);
    if (!ok)
        SetLastError(e);
    return ok;
}

static int svc_change(enum svc_op op)
{
    if (injected())
        return 0;
    D.changed = 1;
    return svc_call(op, &D.now, 0);
}

static int driver_rollback_begin(void);
static int driver_rollback_end(void);
static int driver_commit(void);

static int is_target(const WCHAR *p)
{
    WCHAR t[QP];
    unsigned i;

    for (i = 0; i < NSLOT; i++)
        if (wjoin(t, QP, C.sys, k_slots[i].name) && wieq(t, p))
            return 1;
    return 0;
}

static int journal_full(void)
{
    if (g_nj < JMAX)
        return 0;
    SetLastError(ERROR_BUFFER_OVERFLOW);
    return 1;
}

static int f_rename(const WCHAR *a, const WCHAR *b)
{
    if (journal_full() || injected() || !MoveFileExW(a, b, 0))
        return 0;
    g_j[g_nj].op = J_RENAMED;
    wcpy(g_j[g_nj].a, QP, a);
    wcpy(g_j[g_nj].b, QP, b);
    g_nj++;
    return 1;
}

/* Copies src to the new file dst, then checks that dst holds want's bytes. */
static int f_copy(const WCHAR *src, const WCHAR *dst, const struct finfo *want, int writable)
{
    BYTE *d;
    DWORD n, e, a;
    int ok;

    if (journal_full() || injected() || !CopyFileW(src, dst, TRUE))
        return 0;
    g_j[g_nj].op = J_CREATED;
    wcpy(g_j[g_nj].a, QP, dst);
    g_j[g_nj].b[0] = 0;
    g_nj++;
    a = GetFileAttributesW(dst);
    if (writable && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(dst, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!read_all(dst, &d, &n, &e)) {
        SetLastError(e);
        return 0;
    }
    ok = n == want->size && !memcmp(d, want->data, n);
    xfree(d);
    if (!ok)
        SetLastError(ERROR_CRC);
    return ok;
}

static int trash(const WCHAR *p)
{
    if (g_ntrash == ARRAYSIZE(g_trash)) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    wcpy(g_trash[g_ntrash++], QP, p);
    return 1;
}

/* A free name "path.qd3d-TAG[N]" that no pending operation mentions. */
static int temp_name(WCHAR *out, const WCHAR *path, const WCHAR *tag)
{
    unsigned i;

    for (i = 0; i < 100; i++) {
        if (!wcpy(out, QP, path) || !wcat(out, QP, L".qd3d-") || !wcat(out, QP, tag) || (i && !wcat_u(out, QP, i)))
            break;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND &&
            !pend_mentions(out))
            return 1;
    }
    SetLastError(ERROR_FILE_EXISTS);
    return 0;
}

/* Moves path to a temporary name that the commit deletes. */
static int f_discard(const WCHAR *path)
{
    WCHAR tmp[QP];

    return temp_name(tmp, path, L"old") && f_rename(path, tmp) && trash(tmp);
}

static void rollback(void)
{
    WCHAR suspended[NSLOT][QP];
    unsigned i, n = 0, k;
    int bad = !driver_rollback_begin();

    if (g_nj)
        outf(L"  rolling back %u change%s\n", g_nj, g_nj == 1 ? L"" : L"s");
    for (i = g_nj; i-- > 0;) {
        struct jent *j = &g_j[i];
        const WCHAR *t = is_target(j->a) ? j->a : is_target(j->b) ? j->b : NULL;
        BOOL ok;

        /* Each System32 target once: the exception lasts about a minute. */
        for (k = 0; t && k < n; k++)
            if (wieq(suspended[k], t))
                t = NULL;
        if (t && n < NSLOT) {
            wfp_suspend(t);
            wcpy(suspended[n++], QP, t);
        }
        if (j->op == J_RENAMED) {
            ok = MoveFileExW(j->b, j->a, 0);
        } else {
            SetFileAttributesW(j->a, FILE_ATTRIBUTE_NORMAL);
            ok = DeleteFileW(j->a);
        }
        if (!ok) {
            DWORD e = GetLastError();

            bad = 1;
            if (j->op == J_RENAMED)
                outf(L"  ROLLBACK FAILED: rename %s back to %s: ", j->b, j->a);
            else
                outf(L"  ROLLBACK FAILED: delete %s: ", j->a);
            out_err(e);
            outf(L"\n");
        }
    }
    if (!driver_rollback_end())
        bad = 1;
    g_nj = g_nlater = g_ntrash = g_nbootdel = 0;
    C.rollback_failed = bad;
    if (bad)
        outf(L"  Some changes could not be undone; see \"Manual recovery\" in README.md.\n");
}

static int fail(const WCHAR *what, const WCHAR *path)
{
    DWORD e = GetLastError();

    outf(L"  cannot %s %s: ", what, path);
    out_err(e);
    if (e == ERROR_ACCESS_DENIED)
        outf(L" (run as an administrator)");
    outf(L"\n");
    return 0;
}

/* Only a deferred uninstall queues deletion of an existing managed file.
 * If DeleteService then fails, remove exactly our deletion pair, retaining
 * every unrelated current pending operation rather than restoring an old
 * registry snapshot. All ordinary replacements retain the existing
 * new-file/harmless-after-rollback convention. */
static int driver_unqueue_delete(void)
{
    static const WCHAR key[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    BYTE *raw = NULL;
    DWORD bytes = 0, e = 0, type = REG_MULTI_SZ, written;
    WCHAR source[QP + 4], *p;
    size_t n, i, first, end;
    HANDLE h;
    HKEY k = NULL;
    BOOL ok = FALSE;

    if (!wcpy(source, ARRAYSIZE(source), L"\\??\\") || !wcat(source, ARRAYSIZE(source), C.driver))
        return 0;
    if (C.root_mode) {
        if (!read_all(C.pendfile, &raw, &bytes, &e))
            goto done;
    } else {
        e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
        if (e)
            goto done;
        e = RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, NULL, &bytes);
        if (e)
            goto done;
        raw = xalloc(bytes);
        e = RegQueryValueExW(k, L"PendingFileRenameOperations", NULL, &type, raw, &bytes);
        if (e)
            goto done;
    }
    e = ERROR_INVALID_DATA;
    if (type != REG_MULTI_SZ || bytes % sizeof(WCHAR))
        goto done;
    p = (WCHAR *)raw;
    n = bytes / sizeof *p;
    for (i = 0; i < n;) {
        first = i;
        while (i < n && p[i])
            i++;
        if (i == n || i + 1 == n)
            break;
        end = ++i;
        while (i < n && p[i])
            i++;
        if (i == n)
            break;
        i++;
        if (p[end] || !wieq(p + first, source))
            continue;
        memmove(p + first, p + i, (n - i) * sizeof *p);
        bytes -= (DWORD)((i - first) * sizeof *p);
        if (C.root_mode) {
            if (!bytes) {
                ok = DeleteFileW(C.pendfile);
            } else {
                h = CreateFileW(C.pendfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    ok = WriteFile(h, raw, bytes, &written, NULL) && written == bytes;
                    e = ok ? ERROR_SUCCESS : GetLastError();
                    CloseHandle(h);
                }
            }
            if (!ok && e == ERROR_INVALID_DATA)
                e = GetLastError();
        } else {
            e = bytes ? RegSetValueExW(k, L"PendingFileRenameOperations", 0, REG_MULTI_SZ, raw, bytes)
                      : RegDeleteValueW(k, L"PendingFileRenameOperations");
            ok = e == ERROR_SUCCESS;
        }
        goto done;
    }
    /* A concurrently removed pair is already harmless. */
    ok = TRUE;
done:
    xfree(raw);
    if (k)
        RegCloseKey(k);
    if (!ok)
        SetLastError(e ? e : ERROR_WRITE_FAULT);
    return ok;
}

static int driver_rollback_begin(void)
{
    struct svc_state current;
    int ok = 1;

    if (!D.active)
        return 1;
    if (D.delete_queued && !driver_unqueue_delete()) {
        fail(L"ROLLBACK: withdraw the restart-time deletion of", C.driver);
        ok = 0;
    }
    if (D.started) {
        if (!svc_call(S_READ, &current, 1) ||
            (current.present && current.state != SERVICE_STOPPED && !svc_call(S_STOP, &current, 1))) {
            fail(L"ROLLBACK: stop", L"qxpmap");
            ok = 0;
        }
    }
    return ok;
}

static int driver_rollback_end(void)
{
    struct svc_state current;

    if (!D.active || !D.changed)
        return 1;
    if (!svc_call(S_READ, &current, 1))
        return fail(L"ROLLBACK: query", L"qxpmap");
    if (!D.before.present) {
        if (current.present && !svc_call(S_DELETE, &current, 1))
            return fail(L"ROLLBACK: delete", L"qxpmap");
    } else {
        if (!current.present || current.pending) {
            SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE);
            return fail(L"ROLLBACK: restore", L"qxpmap");
        }
        if (current.start != D.before.start) {
            current.start = D.before.start;
            if (!svc_call(S_CONFIG, &current, 1))
                return fail(L"ROLLBACK: restore the start type of", L"qxpmap");
        }
        if (current.state != D.before.state) {
            if (!svc_call(D.before.state == SERVICE_RUNNING ? S_START : S_STOP, &current, 1))
                return fail(L"ROLLBACK: restore the running state of", L"qxpmap");
        }
    }
    D.active = 0;
    return 1;
}

static int driver_plan(int uninstall)
{
    int shared;

    memset(&D, 0, sizeof D);
    D.uninstall = uninstall;
    if (!svc_call(S_READ, &D.before, 0))
        return fail(L"query the service", L"qxpmap");
    D.now = D.before;
    if (D.before.pending && !uninstall) {
        outf(L"  qxpmap: service deletion is pending; restart Windows first.\n");
        return 0;
    }
    if (D.before.present && !D.before.pending &&
        (D.before.type != SERVICE_KERNEL_DRIVER || !driver_service_image(D.before.image))) {
        outf(L"  qxpmap: conflicting service type or image path; nothing will be changed.\n");
        return 0;
    }
    if (uninstall) {
        if (!driver_opengl_user(&shared))
            return fail(L"query the OpenGL registration for", L"qxpicd");
        if (shared) {
            outf(L"qxpmap: kept because the XPDriver OpenGL driver (qxpicd) uses it.\n");
            return 1;
        }
    }
    if (D.before.present && !D.before.pending &&
        D.before.state != SERVICE_RUNNING && D.before.state != SERVICE_STOPPED) {
        outf(L"  qxpmap: service is transitioning; wait for it to settle and retry.\n");
        return 0;
    }
    probe(C.driver, &D.target);
    if (D.target.kind != K_ABSENT && !driver_image(&D.target)) {
        outf(L"  %s is not a readable XPDriver qxpmap kernel image; refusing to replace or remove it.\n", C.driver);
        return 0;
    }
    if (!uninstall) {
        if (!wjoin(D.src, QP, C.from, L"qxpmap.sys"))
            return 0;
        probe(D.src, &D.source);
        if (!driver_image(&D.source)) {
            outf(L"  %s is not the XPDriver qxpmap native i386 driver (", D.src);
            out_finfo(&D.source);
            outf(L")\n");
            return 0;
        }
        if (!D.source.has_file_version) {
            outf(L"  %s has no readable FILEVERSION; refusing an update without version ordering.\n", D.src);
            return 0;
        }
        if (!dir_exists(C.drivers)) {
            outf(L"  %s is not a directory.\n", C.drivers);
            return 0;
        }
    }
    D.active = 1;
    return 1;
}

static int driver_busy(DWORD e)
{
    return e == ERROR_BUSY || e == ERROR_SHARING_VIOLATION || e == ERROR_INVALID_SERVICE_CONTROL ||
           e == ERROR_SERVICE_CANNOT_ACCEPT_CTRL || e == ERROR_DEPENDENT_SERVICES_RUNNING ||
           e == ERROR_SERVICE_REQUEST_TIMEOUT;
}

static int driver_stop(void)
{
    if (!D.now.present || D.now.state == SERVICE_STOPPED)
        return 1;
    if (D.now.pending) {
        D.deferred = 1;
        return 1;
    }
    if (svc_change(S_STOP))
        return 1;
    if (!driver_busy(GetLastError()))
        return fail(L"stop the service", L"qxpmap");
    D.deferred = 1;
    outf(L"  qxpmap: clients prevent stopping the driver; change deferred until restart.\n");
    return 1;
}

static int driver_stage(void)
{
    WCHAR next[QP];
    DWORD start;
    int newer, replace;

    if (!D.active)
        return 1;

    if (D.uninstall) {
        if (!driver_stop())
            return 0;
        if (D.target.kind != K_ABSENT) {
            if (!D.deferred && !f_discard(C.driver)) {
                if (GetLastError() != ERROR_SHARING_VIOLATION)
                    return fail(L"remove the driver file", C.driver);
                D.deferred = 1;
            }
        }
        if (D.deferred && D.now.present && !D.now.pending && D.now.start != SERVICE_DISABLED) {
            D.now.start = SERVICE_DISABLED;
            if (!svc_change(S_CONFIG))
                return fail(L"disable the driver for removal", L"qxpmap");
        }
        return 1;
    }
    newer = D.target.has_file_version &&
            (D.target.file_version_ms > D.source.file_version_ms ||
             (D.target.file_version_ms == D.source.file_version_ms &&
              D.target.file_version_ls > D.source.file_version_ls));
    if (D.target.kind != K_ABSENT && !D.target.has_file_version)
        outf(L"qxpmap.sys: installed FILEVERSION unreadable; using byte comparison\n");
    replace = !newer && !same(&D.target, &D.source);
    if (replace) {
        if (!temp_name(next, C.driver, L"new") || !f_copy(D.src, next, &D.source, 1))
            return fail(L"copy the new driver next to", C.driver);
        if (!driver_stop())
            return 0;
        if (!D.deferred && D.target.kind != K_ABSENT && !f_discard(C.driver)) {
            if (GetLastError() != ERROR_SHARING_VIOLATION)
                return fail(L"move aside the driver", C.driver);
            D.deferred = 1;
        }
        if (D.deferred) {
            if (g_nlater == ARRAYSIZE(g_later)) {
                SetLastError(ERROR_BUFFER_OVERFLOW);
                return fail(L"track the driver replacement", C.driver);
            }
            wcpy(g_later[g_nlater].src, QP, next);
            wcpy(g_later[g_nlater++].dst, QP, C.driver);
        } else if (!f_rename(next, C.driver)) {
            return fail(L"place the driver", C.driver);
        }
    }
    /* SYSTEM_START runs before smss processes file renames. A deferred
     * replacement instead starts through SCM after smss, then the next
     * successful driver/install command normalizes it to SYSTEM_START. */
    start = D.deferred ? SERVICE_AUTO_START : SERVICE_SYSTEM_START;
    if (!D.now.present) {
        D.now.type = SERVICE_KERNEL_DRIVER;
        D.now.start = start;
        D.now.error = SERVICE_ERROR_NORMAL;
        wcpy(D.now.image, QP, k_driver_image);
        if (!svc_change(S_CREATE))
            return fail(L"create the kernel service", L"qxpmap");
    } else if (D.now.start != start) {
        D.now.start = start;
        if (!svc_change(S_CONFIG))
            return fail(L"set the start type of", L"qxpmap");
    }
    if (newer)
        outf(L"qxpmap.sys: newer version %u.%u.%u.%u already installed\n",
             (unsigned)HIWORD(D.target.file_version_ms), (unsigned)LOWORD(D.target.file_version_ms),
             (unsigned)HIWORD(D.target.file_version_ls), (unsigned)LOWORD(D.target.file_version_ls));
    else
        outf(L"qxpmap.sys: %s%s\n", replace ? L"installed/updated" : L"up to date",
             D.deferred ? L" (at the next restart; temporary AUTO_START)" : L"");
    return 1;
}

static int driver_commit(void)
{
    if (!D.active)
        return 1;
    if (D.uninstall) {
        if (D.deferred && D.target.kind != K_ABSENT) {
            if (injected() || !schedule(C.driver, NULL))
                return fail(L"schedule the driver deletion", C.driver);
            D.delete_queued = 1;
        }
        /* No fallible transactional step may follow successful deletion.
         * A marked-for-deletion service is a successful uninstall, not an
         * excuse to recreate it under the same name. */
        if (D.now.present && !D.now.pending && !svc_change(S_DELETE))
            return fail(L"delete the service", L"qxpmap");
        if (D.deferred || D.now.pending)
            C.restart = 1;
        outf(L"qxpmap: removed%s\n", C.restart ? L" (pending restart)" : L"");
    } else if (!D.deferred && D.now.state != SERVICE_RUNNING) {
        D.started = 1;
        if (!svc_change(S_START))
            return fail(L"start the driver", L"qxpmap");
    }
    return 1;
}

/* Schedules restart-time operations, then deletes the discarded files. A
 * scheduled operation cannot be withdrawn, so every replacement takes a new
 * file that a rollback deletes: one scheduled before a later failure has no
 * effect. A saved original whose copy is that new file is deleted at the
 * restart after the replacement, scheduled once nothing can fail any more. */
static int commit(void)
{
    unsigned i, k;

    for (i = 0; i < g_nlater; i++) {
        if (injected() || !schedule(g_later[i].src, g_later[i].dst)) {
            fail(L"schedule a restart-time replacement of", g_later[i].dst);
            rollback();
            for (k = 0; k < i; k++) {
                if (GetFileAttributesW(g_later[k].src) == INVALID_FILE_ATTRIBUTES)
                    outf(L"  the scheduled replacement of %s has no effect: %s is gone\n", g_later[k].dst,
                         g_later[k].src);
                else
                    outf(L"  still scheduled: at the next restart %s replaces %s\n", g_later[k].src,
                         g_later[k].dst);
            }
            return 0;
        }
        outf(L"  at the next restart %s replaces %s\n", g_later[i].src, g_later[i].dst);
        C.restart = 1;
    }
    if (!driver_commit()) {
        rollback();
        return 0;
    }
    g_nj = 0;
    for (i = 0; i < g_nbootdel; i++) {
        if (schedule(g_bootdel[i], NULL))
            outf(L"  at the next restart %s is deleted\n", g_bootdel[i]);
        else
            outf(L"  warning: cannot schedule the deletion of %s; delete it after the restart\n", g_bootdel[i]);
    }
    for (i = 0; i < g_ntrash; i++) {
        DWORD e;

        SetFileAttributesW(g_trash[i], FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(g_trash[i]))
            continue;
        e = GetLastError();
        if (schedule(g_trash[i], NULL)) {
            outf(L"  %s is in use; it is deleted at the next restart\n", g_trash[i]);
        } else {
            outf(L"  warning: cannot delete %s: ", g_trash[i]);
            out_err(e);
            outf(L"\n");
        }
    }
    g_nlater = g_ntrash = g_nbootdel = 0;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Per-DLL state and plans                                                   */

enum { A_NONE, A_FRESH, A_REPLACE, A_PLACE };     /* install */
enum { U_NONE, U_RESTORE, U_COPY };               /* uninstall */

struct sl {
    const struct slot *s;
    WCHAR t[QP], b[QP], src[QP], copy[QP];
    struct finfo ft, fb, fs, fc; /* target, saved original, source, stock copy */
    int act, junk_b, prot, later;
};

static struct sl g_sl[NSLOT];

static void load_slot(struct sl *x, unsigned i, int with_source)
{
    memset(x, 0, sizeof *x);
    x->s = &k_slots[i];
    wjoin(x->t, QP, C.sys, x->s->name);
    wjoin(x->b, QP, C.sys, x->s->orig);
    probe(x->t, &x->ft);
    probe(x->b, &x->fb);
    if (wjoin(x->src, QP, C.from, x->s->name) && with_source)
        probe(x->src, &x->fs);
}

static void unload_slot(struct sl *x)
{
    unprobe(&x->ft);
    unprobe(&x->fb);
    unprobe(&x->fs);
    unprobe(&x->fc);
}

static int later(struct sl *x, const WCHAR *src)
{
    if (g_nlater == ARRAYSIZE(g_later)) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return fail(L"schedule", x->t);
    }
    wcpy(g_later[g_nlater].src, QP, src);
    wcpy(g_later[g_nlater++].dst, QP, x->t);
    x->later = 1;
    return 1;
}

/* A stock (Microsoft) copy of the DLL in the WFP caches, for a missing saved
 * original. */
static int find_stock_copy(struct sl *x)
{
    unsigned i, k;

    for (i = 0; i < g_nloc; i++)
        for (k = 0; k < 2; k++) {
            if (!wjoin(x->copy, QP, g_loc[i].dir, k ? x->s->name : x->s->orig))
                continue;
            probe(x->copy, &x->fc);
            if (x->fc.kind == K_OTHER && x->fc.microsoft)
                return 1;
            unprobe(&x->fc);
        }
    x->copy[0] = 0;
    return 0;
}

static int check_readable(const struct sl *x)
{
    const WCHAR *p = x->ft.kind == K_DIR || x->ft.kind == K_UNREADABLE ? x->t : x->b;
    const struct finfo *f = p == x->t ? &x->ft : &x->fb;

    if (f->kind != K_DIR && f->kind != K_UNREADABLE)
        return 1;
    outf(L"  %s: cannot use %s: ", x->s->name, p);
    if (f->kind == K_DIR)
        outf(L"it is a directory");
    else
        out_err(f->err);
    if (f->err == ERROR_SHARING_VIOLATION)
        outf(L" (close the program using it)");
    outf(L"\n");
    return 0;
}

static void out_conflict(const struct sl *x, const WCHAR *verb)
{
    outf(L"  %s: %s is not QD3D and differs from the saved original %s.\n"
         L"    Keep the stock DLL you want as %s, delete or move the other file, and run %s again.\n",
         x->s->name, x->t, x->b, x->b, verb);
}

static void out_no_original(const struct sl *x)
{
    outf(L"  %s: %s is %s, and no stock copy is saved as %s or found in the WFP caches.\n"
         L"    Put the stock Windows XP SP3 %s at %s and run this again.\n",
         x->s->name, x->t, x->ft.kind == K_ABSENT ? L"missing" : L"QD3D", x->b, x->s->name, x->b);
}

static int plan_install(struct sl *x)
{
    if (!check_readable(x))
        return 0;
    if (x->fb.kind == K_OTHER) {
        if (!x->fb.microsoft)
            outf(L"  warning: the saved original %s is not a Microsoft DLL; the proxy falls back to it\n", x->b);
        if (x->ft.kind == K_QD3D)
            x->act = same(&x->ft, &x->fs) ? A_NONE : A_REPLACE;
        else if (x->ft.kind == K_ABSENT)
            x->act = A_PLACE;
        else if (same(&x->ft, &x->fb))
            x->act = A_REPLACE; /* a second copy of the saved original, e.g. restored by WFP */
        else
            return out_conflict(x, L"install"), 0;
        return 1;
    }
    /* No saved original: the stock file in place becomes it, or a cache copy.
     * Only a Microsoft DLL may become the fallback, never another wrapper. */
    x->junk_b = x->fb.kind == K_QD3D;
    if (x->ft.kind == K_OTHER) {
        if (!x->ft.microsoft) {
            outf(L"  %s: %s is neither QD3D nor a Microsoft DLL (", x->s->name, x->t);
            out_finfo(&x->ft);
            outf(L").\n    It would become the stock fallback; put the Windows XP SP3 %s back first.\n",
                 x->s->name);
            return 0;
        }
        x->act = A_FRESH;
        return 1;
    }
    if (!find_stock_copy(x))
        return out_no_original(x), 0;
    x->act = x->ft.kind == K_ABSENT ? A_PLACE : same(&x->ft, &x->fs) ? A_NONE : A_REPLACE;
    return 1;
}

static int plan_uninstall(struct sl *x)
{
    if (!check_readable(x))
        return 0;
    if (x->fb.kind == K_OTHER) {
        if (x->ft.kind == K_OTHER && !same(&x->ft, &x->fb))
            return out_conflict(x, L"uninstall"), 0;
        x->act = U_RESTORE;
        return 1;
    }
    x->junk_b = x->fb.kind == K_QD3D;
    if (x->ft.kind == K_OTHER) {
        x->act = U_NONE;
        return 1;
    }
    if (!find_stock_copy(x))
        return out_no_original(x), 0;
    x->act = U_COPY;
    return 1;
}

/* Install: the stock copies in the WFP caches move aside, renamed to the
 * saved-original name (qd3d9_orig.dll...). */
static int caches_install(struct sl *x)
{
    unsigned i;

    for (i = 0; i < g_nloc; i++) {
        WCHAR f[QP], k[QP];
        struct finfo ff, fk;
        int ok = 1;
        DWORD e = 0;

        if (!wjoin(f, QP, g_loc[i].dir, x->s->name) || !wjoin(k, QP, g_loc[i].dir, x->s->orig))
            continue;
        probe(f, &ff);
        probe(k, &fk);
        if (ff.kind == K_OTHER) {
            if (fk.kind == K_QD3D && (ok = f_discard(k)) != 0)
                fk.kind = K_ABSENT;
            if (!ok) {
                e = GetLastError();
            } else if (fk.kind == K_ABSENT) {
                if ((ok = f_rename(f, k)) != 0)
                    outf(L"    %s: kept the stock %s as %s\n", g_loc[i].label, x->s->name, x->s->orig);
                else
                    e = GetLastError();
            } else if (fk.kind == K_OTHER && same(&ff, &fk)) {
                if ((ok = f_discard(f)) != 0)
                    outf(L"    %s: removing a second stock copy of %s\n", g_loc[i].label, x->s->name);
                else
                    e = GetLastError();
            } else {
                outf(L"    warning: %s is not QD3D and %s is not its copy; both left in place, so WFP "
                     L"may restore the stock DLL\n", f, k);
            }
        } else if (ff.kind == K_UNREADABLE) {
            outf(L"    warning: cannot read %s: ", f);
            out_err(ff.err);
            outf(L"\n");
        }
        unprobe(&ff);
        unprobe(&fk);
        if (!ok) {
            SetLastError(e);
            return fail(L"move aside", f);
        }
    }
    return 1;
}

/* Uninstall: the saved cache copies return; QD3D files in the caches go. */
static int caches_uninstall(struct sl *x)
{
    unsigned i;

    for (i = 0; i < g_nloc; i++) {
        WCHAR f[QP], k[QP];
        struct finfo ff, fk;
        int ok = 1;
        DWORD e = 0;

        if (!wjoin(f, QP, g_loc[i].dir, x->s->name) || !wjoin(k, QP, g_loc[i].dir, x->s->orig))
            continue;
        probe(f, &ff);
        probe(k, &fk);
        if (fk.kind == K_OTHER) {
            if (ff.kind == K_QD3D || (ff.kind == K_OTHER && same(&ff, &fk))) {
                if ((ok = f_discard(f)) != 0)
                    ff.kind = K_ABSENT;
            }
            if (ok && ff.kind == K_ABSENT) {
                if ((ok = f_rename(k, f)) != 0)
                    outf(L"    %s: restored the stock %s\n", g_loc[i].label, x->s->name);
            } else if (ok) {
                outf(L"    warning: %s is not the saved %s; both left in place\n", f, k);
            }
        } else {
            if (fk.kind == K_QD3D)
                ok = f_discard(k);
            if (ok && ff.kind == K_QD3D)
                ok = f_discard(f);
        }
        if (!ok)
            e = GetLastError();
        unprobe(&ff);
        unprobe(&fk);
        if (!ok) {
            SetLastError(e);
            return fail(L"restore", f);
        }
    }
    return 1;
}

/* Replaces the target at the next restart. A scheduled replacement cannot be
 * withdrawn, so it always takes a new file that a rollback deletes: a saved
 * original is copied, and itself deleted at the restart once its copy has
 * replaced the target. */
static int defer(struct sl *x, const WCHAR *src)
{
    WCHAR tnew[QP];

    if (!wieq(src, x->b))
        return later(x, src);
    if (g_nbootdel == ARRAYSIZE(g_bootdel)) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return fail(L"schedule", x->t);
    }
    if (!temp_name(tnew, x->t, L"new") || !f_copy(x->b, tnew, &x->fb, 0))
        return fail(L"copy the saved original next to", x->t);
    if (!later(x, tnew))
        return 0;
    wcpy(g_bootdel[g_nbootdel++], QP, x->b);
    return 1;
}

/* Puts the file src (a new or saved file) at the target: now when WFP and the
 * other programs let us rename the target, otherwise at the next restart. */
static int put_target(struct sl *x, const WCHAR *src)
{
    WCHAR old[QP];

    if (x->prot && !wfp_suspend(x->t))
        return defer(x, src);
    if (x->ft.kind == K_ABSENT)
        return f_rename(src, x->t) || fail(L"rename to", x->t);
    if (!temp_name(old, x->t, L"old"))
        return fail(L"find a temporary name for", x->t);
    if (f_rename(x->t, old)) {
        if (!f_rename(src, x->t))
            return fail(L"rename to", x->t);
        return trash(old) || fail(L"track", old);
    }
    if (GetLastError() != ERROR_SHARING_VIOLATION)
        return fail(L"rename", x->t);
    outf(L"    %s is locked by a running program\n", x->t);
    return defer(x, src);
}

static int do_install(struct sl *x)
{
    WCHAR tnew[QP];

    /* The new file first, beside the target, so the target is never partial. */
    if (x->act != A_NONE && (!temp_name(tnew, x->t, L"new") || !f_copy(x->src, tnew, &x->fs, 1)))
        return fail(L"copy the new file next to", x->t);
    if (x->junk_b) {
        if (!f_discard(x->b))
            return fail(L"move aside", x->b);
        outf(L"    removing the QD3D file found at %s\n", x->b);
    }
    if (x->copy[0]) {
        if (!f_copy(x->copy, x->b, &x->fc, 0))
            return fail(L"save a stock copy as", x->b);
        outf(L"    saved %s as %s\n", x->copy, x->b);
    }
    if (!caches_install(x))
        return 0;
    if (x->act == A_NONE)
        return 1;
    if (x->act != A_FRESH)
        return put_target(x, tnew);
    /* The stock target becomes the saved original: by rename when nothing
     * holds it open without FILE_SHARE_DELETE, otherwise as a checked copy. */
    if (!x->prot || wfp_suspend(x->t)) {
        if (f_rename(x->t, x->b)) {
            outf(L"    kept the stock %s as %s\n", x->s->name, x->b);
            return f_rename(tnew, x->t) || fail(L"rename to", x->t);
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION)
            return fail(L"rename", x->t);
        outf(L"    %s is locked by a running program\n", x->t);
    }
    if (!f_copy(x->t, x->b, &x->ft, 0))
        return fail(L"save a copy of the stock DLL as", x->b);
    outf(L"    saved a copy of the stock %s as %s\n", x->s->name, x->b);
    return later(x, tnew);
}

static int do_uninstall(struct sl *x)
{
    WCHAR tnew[QP];

    if (x->act == U_COPY && (!temp_name(tnew, x->t, L"new") || !f_copy(x->copy, tnew, &x->fc, 0)))
        return fail(L"copy the stock file next to", x->t);
    if (!caches_uninstall(x))
        return 0;
    if (x->junk_b) {
        if (!f_discard(x->b))
            return fail(L"remove", x->b);
        outf(L"    removing the QD3D file found at %s\n", x->b);
    }
    if (x->act == U_NONE)
        return 1;
    return put_target(x, x->act == U_COPY ? tnew : x->b);
}

/* ------------------------------------------------------------------------ */
/* Per-game wrapper DLLs                                                     */

struct match {
    WCHAR *path;
    int backup; /* NAME.qd3d-bak */
};

static struct match *g_m;
static unsigned g_nm, g_cm;

static int wrapper_name(const WCHAR *name)
{
    unsigned i;

    for (i = 0; i < ARRAYSIZE(k_wrappers); i++)
        if (wieq(name, k_wrappers[i]))
            return 1;
    return 0;
}

/* NAME.qd3d-bak for a wrapper NAME; orig receives NAME. */
static int wrapper_backup(const WCHAR *name, WCHAR *orig)
{
    size_t n = wlen(name), s = wlen(BAK_SUFFIX);

    if (n <= s || n - s >= MAX_PATH || !wieq(name + n - s, BAK_SUFFIX))
        return 0;
    memcpy(orig, name, (n - s) * sizeof *orig);
    orig[n - s] = 0;
    return wrapper_name(orig);
}

static void add_match(const WCHAR *path, int backup)
{
    unsigned i;

    for (i = 0; i < g_nm; i++)
        if (wieq(g_m[i].path, path))
            return;
    if (g_nm == g_cm) {
        struct match *n;

        g_cm = g_cm ? g_cm * 2 : 16;
        n = xalloc(g_cm * sizeof *n);
        if (g_nm)
            memcpy(n, g_m, g_nm * sizeof *n);
        xfree(g_m);
        g_m = n;
    }
    g_m[g_nm].path = wdup(path);
    g_m[g_nm++].backup = backup;
}

static int excluded_dir(const WCHAR *dir)
{
    return wieq(dir, C.from) || wieq(dir, C.self_dir) || wieq(dir, C.windir) || wieq(dir, C.sys);
}

/* path: a QP buffer holding a directory; restored on return. */
static void scan_tree(WCHAR *path, unsigned depth)
{
    WIN32_FIND_DATAW fd;
    WCHAR orig[MAX_PATH];
    size_t n = wlen(path);
    HANDLE h;

    if (depth > SCAN_DEPTH || excluded_dir(path) || !wcat(path, QP, L"\\*")) {
        path[n] = 0;
        return;
    }
    h = FindFirstFileW(path, &fd);
    path[n] = 0;
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const WCHAR *e = fd.cFileName;

        if (e[0] == L'.' && (!e[1] || (e[1] == L'.' && !e[2])))
            continue;
        if (n + 1 + wlen(e) >= MAX_PATH) /* Windows XP cannot load programs from there */
            continue;
        path[n] = L'\\';
        wcpy(path + n + 1, QP - n - 1, e);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                scan_tree(path, depth + 1);
        } else if (wrapper_name(e)) {
            add_match(path, 0);
        } else if (wrapper_backup(e, orig)) {
            add_match(path, 1);
        }
        path[n] = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The game directories of a drive: every top-level directory but the system's. */
static void scan_top(const WCHAR *drive)
{
    WIN32_FIND_DATAW fd;
    WCHAR path[QP];
    HANDLE h;
    unsigned i;

    if (!wjoin(path, QP, drive, L"*"))
        return;
    h = FindFirstFileW(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const WCHAR *e = fd.cFileName;
        int skip = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                   (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                   (e[0] == L'.' && (!e[1] || (e[1] == L'.' && !e[2])));

        for (i = 0; !skip && i < ARRAYSIZE(k_skip_top); i++)
            skip = wieq(e, k_skip_top[i]);
        if (!skip && wjoin(path, QP, drive, e))
            scan_tree(path, 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* NAME.qd3d-bak -> NAME */
static void bak_target(const WCHAR *bak, WCHAR *dst)
{
    size_t n = wlen(bak) - wlen(BAK_SUFFIX);

    memcpy(dst, bak, n * sizeof *dst);
    dst[n] = 0;
}

/* Lists (and with --move/--restore-leftovers renames) wrapper DLLs. */
static void leftovers(void)
{
    WCHAR path[QP], dst[QP];
    unsigned i, found = 0, moved = 0, restored = 0, backups = 0;

    if (C.root_mode) {
        scan_top(C.root);
    } else {
        WCHAR drives[256];
        DWORD n = GetLogicalDriveStringsW(ARRAYSIZE(drives) - 1, drives);
        const WCHAR *d;

        for (d = drives; n && n < ARRAYSIZE(drives) && *d; d += wlen(d) + 1)
            if (GetDriveTypeW(d) == DRIVE_FIXED)
                scan_top(d);
    }
    for (i = 0; i < C.nscan; i++)
        if (wcpy(path, QP, C.scan[i]))
            scan_tree(path, 0);

    for (i = 0; i < g_nm; i++)
        if (!g_m[i].backup)
            found++;
    outf(L"\nPer-game DLLs that a game loads instead of the system-wide ones: %u\n", found);
    for (i = 0; i < g_nm; i++) {
        struct finfo f;

        if (g_m[i].backup) {
            backups++;
            continue;
        }
        probe(g_m[i].path, &f);
        outf(L"  %s [", g_m[i].path);
        out_finfo(&f);
        outf(L"]\n");
        unprobe(&f);
        if (!C.move)
            continue;
        if (!wcpy(dst, QP, g_m[i].path) || !wcat(dst, QP, BAK_SUFFIX))
            continue;
        if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
            outf(L"    not moved: %s exists\n", dst);
        } else if (MoveFileExW(g_m[i].path, dst, 0)) {
            moved++;
            outf(L"    moved to %s\n", dst);
        } else {
            outf(L"    not moved: ");
            out_err(GetLastError());
            outf(L"\n");
        }
    }
    if (backups && !C.restore)
        outf(L"Moved aside earlier (*%s): %u; --restore-leftovers renames them back.\n", BAK_SUFFIX, backups);
    for (i = 0; C.restore && i < g_nm; i++) {
        if (!g_m[i].backup)
            continue;
        bak_target(g_m[i].path, dst);
        if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
            outf(L"  not restored: %s exists\n", dst);
        } else if (MoveFileExW(g_m[i].path, dst, 0)) {
            restored++;
            outf(L"  restored %s\n", dst);
        } else {
            outf(L"  cannot restore %s: ", dst);
            out_err(GetLastError());
            outf(L"\n");
        }
    }
    if (found && !C.move)
        outf(L"Games with these files bypass QD3D. \"scan --move-leftovers\" renames them to NAME%s.\n",
             BAK_SUFFIX);
    outf(L"qd3d: leftovers=%u moved=%u restored=%u\n", found, moved, restored);
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */

static int finish(const WCHAR *cmd, int rc)
{
    static const WCHAR *const names[] = {L"ok", L"usage", L"failed", L"ok", L"refused"};

    if (rc == RC_REFUSED)
        outf(L"Nothing was changed.\n");
    else if (rc == RC_FAILED)
        outf(C.rollback_failed ? L"Rollback is incomplete; see the diagnostics above.\n" :
                                L"The changes were rolled back.\n");
    else if (rc == RC_RESTART)
        outf(L"Restart Windows to finish.\n");
    outf(L"qd3d: %s %s restart-required=%s\n", cmd, names[rc], rc == RC_RESTART ? L"yes" : L"no");
    return rc;
}

static int pending_refuses(void)
{
    const struct pend *p = pend_blocking();

    if (!p)
        return 0;
    outf(L"A restart is pending to finish an earlier change (");
    out_pend(p);
    outf(L").\nRestart Windows, then run this again.\n");
    return 1;
}

static int cmd_install(void)
{
    unsigned i;
    int ok = 1, changed = 0;

    outf(L"Installing QD3D from %s\n", C.from);
    for (i = 0; i < NSLOT; i++) {
        struct sl *x = &g_sl[i];

        load_slot(x, i, 1);
        if (x->fs.kind != K_QD3D || !pe_exports(x->fs.data, x->fs.size, x->s->entry)) {
            outf(L"  %s is not the QD3D %s (", x->src, x->s->what);
            out_finfo(&x->fs);
            outf(L")\n");
            ok = 0;
        }
    }
    if (!driver_plan(0))
        ok = 0;
    if (!ok || pending_refuses())
        return finish(L"install", RC_REFUSED);
    for (i = 0; i < NSLOT; i++) {
        g_sl[i].prot = wfp_protected(g_sl[i].t) != 0;
        if (!plan_install(&g_sl[i]))
            ok = 0;
    }
    if (!ok)
        return finish(L"install", RC_REFUSED);
    for (i = 0; i < NSLOT; i++) {
        outf(L"  %s\n", g_sl[i].s->name);
        if (!do_install(&g_sl[i])) {
            rollback();
            return finish(L"install", RC_FAILED);
        }
    }
    if (!driver_stage()) {
        rollback();
        return finish(L"install", RC_FAILED);
    }
    if (!commit())
        return finish(L"install", RC_FAILED);
    for (i = 0; i < NSLOT; i++) {
        struct sl *x = &g_sl[i];
        static const WCHAR *const what[] = {L"up to date", L"installed, stock DLL saved as", L"updated",
                                            L"installed"};

        outf(L"%s: %s%s%s%s\n", x->s->name, what[x->act], x->act == A_FRESH ? L" " : L"",
             x->act == A_FRESH ? x->s->orig : L"", x->later ? L" (at the next restart)" : L"");
        changed |= x->act != A_NONE;
    }
    if (changed)
        outf(L"Programs already running keep the DLLs they loaded until they exit.\n");
    leftovers();
    for (i = 0; i < NSLOT; i++)
        unload_slot(&g_sl[i]);
    return finish(L"install", C.restart ? RC_RESTART : RC_OK);
}

static int cmd_uninstall(void)
{
    unsigned i;
    int ok = 1;

    outf(L"Removing QD3D\n");
    if (pending_refuses())
        return finish(L"uninstall", RC_REFUSED);
    for (i = 0; i < NSLOT; i++) {
        load_slot(&g_sl[i], i, 0);
        g_sl[i].prot = wfp_protected(g_sl[i].t) != 0;
        if (!plan_uninstall(&g_sl[i]))
            ok = 0;
    }
    if (!driver_plan(1))
        ok = 0;
    if (!ok)
        return finish(L"uninstall", RC_REFUSED);
    for (i = 0; i < NSLOT; i++) {
        outf(L"  %s\n", g_sl[i].s->name);
        if (!do_uninstall(&g_sl[i])) {
            rollback();
            return finish(L"uninstall", RC_FAILED);
        }
    }
    if (!driver_stage()) {
        rollback();
        return finish(L"uninstall", RC_FAILED);
    }
    if (!commit())
        return finish(L"uninstall", RC_FAILED);
    for (i = 0; i < NSLOT; i++) {
        struct sl *x = &g_sl[i];

        if (x->act == U_NONE)
            outf(L"%s: not installed\n", x->s->name);
        else
            outf(L"%s: stock DLL restored%s\n", x->s->name, x->later ? L" at the next restart" : L"");
        unload_slot(x);
    }
    return finish(L"uninstall", C.restart ? RC_RESTART : RC_OK);
}

static int cmd_driver(void)
{
    if (pending_refuses() || !driver_plan(0))
        return finish(L"driver", RC_REFUSED);
    if (!driver_stage()) {
        rollback();
        return finish(L"driver", RC_FAILED);
    }
    if (!commit())
        return finish(L"driver", RC_FAILED);
    return finish(L"driver", C.restart ? RC_RESTART : RC_OK);
}

/* 1: installed, 0: not installed, -1: anything else. */
static int out_slot_state(const struct sl *x)
{
    const WCHAR *n = x->s->name;

    if (x->ft.kind == K_QD3D && x->fb.kind == K_OTHER) {
        outf(L"%s: installed", n);
        if (x->fs.kind == K_QD3D)
            outf(same(&x->ft, &x->fs) ? L", same as %s" : L", differs from %s", x->src);
        outf(L"\n");
        return 1;
    }
    if (x->ft.kind == K_OTHER && x->fb.kind == K_ABSENT) {
        outf(L"%s: not installed\n", n);
        return 0;
    }
    if (x->ft.kind == K_OTHER && x->fb.kind == K_OTHER)
        outf(same(&x->ft, &x->fb) ? L"%s: the stock DLL is back in place (WFP?) and the saved original is "
                                    L"kept; run install or uninstall\n"
                                  : L"%s: conflict: the DLL in place is not QD3D and differs from the saved "
                                    L"original\n",
             n);
    else if (x->ft.kind == K_QD3D)
        outf(L"%s: QD3D without a saved stock original\n", n);
    else if (x->ft.kind == K_ABSENT)
        outf(L"%s: missing\n", n);
    else
        outf(L"%s: unexpected state\n", n);
    return -1;
}

static void out_file(const WCHAR *path)
{
    struct finfo f;

    probe(path, &f);
    outf(L"  %s: ", path);
    out_finfo(&f);
    outf(L"\n");
    unprobe(&f);
}

/* Files left by an interrupted run: NAME.qd3d-new*, NAME.qd3d-old*. */
static void out_strays(const WCHAR *dir)
{
    WIN32_FIND_DATAW fd;
    WCHAR pat[QP];
    HANDLE h;

    if (!wjoin(pat, QP, dir, L"*.qd3d-*"))
        return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wjoin(pat, QP, dir, fd.cFileName))
            outf(L"  temporary file %s%s\n", pat, pend_mentions(pat) ? L" (pending restart)" : L"");
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int driver_status(int *pending)
{
    struct svc_state s;
    struct finfo f;
    int state = -1, queried = svc_call(S_READ, &s, 0);

    if (!queried) {
        fail(L"query the service", L"qxpmap");
    } else {
        const WCHAR *name = !s.present ? L"absent" : s.pending ? L"delete-pending" :
                            s.state == SERVICE_RUNNING ? L"running" :
                            s.state == SERVICE_STOPPED ? L"stopped" :
                            s.state == SERVICE_START_PENDING ? L"start-pending" :
                            s.state == SERVICE_STOP_PENDING ? L"stop-pending" : L"other";
        const WCHAR *start = !s.present || s.pending ? L"unknown" :
                             s.start == SERVICE_SYSTEM_START ? L"SYSTEM_START" :
                             s.start == SERVICE_AUTO_START ? L"AUTO_START" :
                             s.start == SERVICE_DEMAND_START ? L"DEMAND_START" :
                             s.start == SERVICE_DISABLED ? L"DISABLED" : L"BOOT_START";
        outf(L"qxpmap: service=%s start=%s\n", name, start);
        if (s.pending)
            *pending = 1;
        if (s.present && !s.pending && s.start == SERVICE_AUTO_START)
            outf(L"  replacement-boot policy; run driver or install to restore SYSTEM_START.\n");
    }
    probe(C.driver, &f);
    outf(L"  %s: file-version=%s, %u bytes%s\n", C.driver,
         f.kind == K_ABSENT ? L"absent" : f.version[0] ? f.version : L"unavailable",
         (unsigned)f.size, driver_image(&f) ? L" (XPDriver native i386)" : L"");
    if (queried) {
        if (!s.present && f.kind == K_ABSENT)
            state = 0;
        else if (s.present && !s.pending && s.type == SERVICE_KERNEL_DRIVER &&
                 driver_service_image(s.image) &&
                 s.state == SERVICE_RUNNING && driver_image(&f))
            state = 1;
    }
    unprobe(&f);
    return state;
}

static int cmd_status(void)
{
    unsigned i, l, installed = 0, stock = 0;
    const struct pend *p = pend_blocking();
    const WCHAR *state;
    int restart_pending = p != NULL, ds = driver_status(&restart_pending);

    for (i = 0; i < NSLOT; i++) {
        struct sl *x = &g_sl[i];
        int prot, st;

        load_slot(x, i, 1);
        st = out_slot_state(x);
        installed += st == 1;
        stock += st == 0;
        out_file(x->t);
        out_file(x->b);
        for (l = 0; l < g_nloc; l++) {
            WCHAR f[QP];

            if (wjoin(f, QP, g_loc[l].dir, x->s->name))
                out_file(f);
            if (wjoin(f, QP, g_loc[l].dir, x->s->orig))
                out_file(f);
        }
        prot = wfp_protected(x->t);
        outf(L"  WFP: %s\n", C.root_mode ? L"protected (simulated)"
                             : prot > 0  ? L"protected"
                             : prot == 0 ? L"not protected"
                                         : L"unknown");
        unload_slot(x);
    }
    outf(L"\nWFP caches handled:%s\n", g_nloc ? L"" : L" none");
    for (l = 0; l < g_nloc; l++)
        outf(L"  %s (%s)\n", g_loc[l].dir, g_loc[l].label);
    if (C.sp_reg[0])
        outf(L"ServicePackSourcePath: %s\n", C.sp_reg);
    if (C.srcpath[0])
        outf(L"SourcePath (installation source, never changed): %s\n", C.srcpath);
    out_strays(C.sys);
    out_strays(C.drivers);
    for (l = 0; l < g_nloc; l++)
        out_strays(g_loc[l].dir);
    for (i = 0; i < g_npend; i++)
        if (ours(g_pend[i].src) || ours(g_pend[i].dst)) {
            outf(L"  at the next restart: ");
            out_pend(&g_pend[i]);
            outf(L"\n");
        }
    state = installed == NSLOT && ds == 1 ? L"installed" :
            stock == NSLOT && ds == 0 ? L"not-installed" :
            installed + stock == NSLOT && ds >= 0 ? L"partial" : L"inconsistent";
    outf(L"qd3d: state=%s restart-pending=%s\n", state, restart_pending ? L"yes" : L"no");
    return RC_OK;
}

static int cmd_scan(void)
{
    leftovers();
    return RC_OK;
}

/* ------------------------------------------------------------------------ */
/* Command line                                                              */

static WCHAR **split_args(const WCHAR *p, int *argc)
{
    size_t n = wlen(p);
    WCHAR **v = xalloc((n / 2 + 3) * sizeof *v);
    WCHAR *o = xalloc((n + 2) * sizeof *o);
    int c = 0;

    /* Program name: up to the closing quote or a blank, no escapes. */
    v[c++] = o;
    if (*p == L'"') {
        for (p++; *p && *p != L'"';)
            *o++ = *p++;
        if (*p)
            p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            *o++ = *p++;
    }
    *o++ = 0;
    for (;;) {
        int q = 0;

        while (*p == L' ' || *p == L'\t')
            p++;
        if (!*p)
            break;
        v[c++] = o;
        while (*p && (q || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'\\') {
                size_t bs = 0;

                while (*p == L'\\')
                    bs++, p++;
                if (*p == L'"') {
                    for (; bs >= 2; bs -= 2)
                        *o++ = L'\\';
                    if (bs)
                        *o++ = *p++;
                } else {
                    for (; bs; bs--)
                        *o++ = L'\\';
                }
            } else if (*p == L'"') {
                q = !q;
                p++;
            } else {
                *o++ = *p++;
            }
        }
        *o++ = 0;
    }
    v[c] = NULL;
    *argc = c;
    return v;
}

static void usage(void)
{
    outf(L"usage: qd3d_install [status|install|driver|uninstall|scan] [options]\n"
         L"  status     show what is installed (default)\n"
         L"  install    install or update the QD3D d3d9.dll, d3d8.dll and ddraw.dll in System32;\n"
         L"             the stock DLLs stay as qd3d9_orig.dll, qd3d8_orig.dll and qddraw_orig.dll\n"
         L"             also installs/updates and starts the qxpmap kernel driver\n"
         L"  driver     install/update qxpmap.sys only (per-game testing; DLLs unchanged)\n"
         L"  uninstall  restore the stock DLLs and remove QD3D\n"
         L"  scan       list per-game wrapper DLLs (d3d8, d3d9, ddraw, opengl32, wined3d, dxgi...)\n"
         L"             that games load instead of the system-wide DLLs\n"
         L"options:\n"
         L"  --from DIR           QD3D DLLs and qxpmap.sys (default: this program's directory)\n"
         L"  --scan-dir DIR       also scan DIR (repeatable)\n"
         L"  --move-leftovers     scan: rename each wrapper DLL found to NAME.qd3d-bak\n"
         L"  --restore-leftovers  scan: rename NAME.qd3d-bak files back\n"
         L"  --root DIR           test mode: DIR stands for the system drive (DIR\\WINDOWS\\system32...);\n"
         L"                       SCM, WFP and restart-time renames are simulated\n"
         L"  --fail-after N       test mode: fail the Nth change to exercise rollback\n"
         L"  --sim-wfp-deny       test mode: WFP refuses the exception\n"
         L"  --sim-scm-busy       test mode: clients prevent stopping qxpmap\n"
         L"  --sim-scm-delete-pending  test mode: a service handle delays deletion\n"
         L"  --sim-scm-fail OP    test mode: fail query/create/config/start/stop/delete\n"
         L"exit codes: 0 done, 1 usage, 2 failed and rolled back, 3 done at the next restart,\n"
         L"            4 refused (nothing changed)\n");
}

static unsigned parse_uint(const WCHAR *s)
{
    unsigned v = 0;

    if (!*s)
        return 0;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9' || v > 100000)
            return 0;
        v = v * 10 + (unsigned)(*s - L'0');
    }
    return v;
}

static int run(void)
{
    static const WCHAR *const cmds[] = {L"status", L"install", L"driver", L"uninstall", L"scan"};
    int argc, i;
    WCHAR **argv = split_args(GetCommandLineW(), &argc);
    const WCHAR *cmd = NULL, *from = NULL, *root = NULL;
    unsigned k;
    DWORD n;
    int mutating;

    for (i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        int known = 0;

        for (k = 0; k < ARRAYSIZE(cmds); k++)
            if (wieq(a, cmds[k]) && !cmd) {
                cmd = cmds[k];
                known = 1;
            }
        if (known)
            continue;
        if (wieq(a, L"--from") && i + 1 < argc) {
            from = argv[++i];
        } else if (wieq(a, L"--root") && i + 1 < argc) {
            root = argv[++i];
        } else if (wieq(a, L"--scan-dir") && i + 1 < argc && C.nscan < ARRAYSIZE(C.scan)) {
            if (!full_path(argv[++i], C.scan[C.nscan]) || !dir_exists(C.scan[C.nscan])) {
                outf(L"--scan-dir %s: not a directory\n", argv[i]);
                return RC_USAGE;
            }
            C.nscan++;
        } else if (wieq(a, L"--move-leftovers")) {
            C.move = 1;
        } else if (wieq(a, L"--restore-leftovers")) {
            C.restore = 1;
        } else if (wieq(a, L"--fail-after") && i + 1 < argc) {
            C.fail_after = parse_uint(argv[++i]);
            if (!C.fail_after) {
                outf(L"--fail-after needs a number from 1\n");
                return RC_USAGE;
            }
        } else if (wieq(a, L"--sim-wfp-deny")) {
            C.wfp_deny = 1;
        } else if (wieq(a, L"--sim-scm-busy")) {
            C.scm_busy = 1;
        } else if (wieq(a, L"--sim-scm-delete-pending")) {
            C.scm_pending = 1;
        } else if (wieq(a, L"--sim-scm-fail") && i + 1 < argc) {
            C.scm_fail = argv[++i];
            for (k = 0; k < ARRAYSIZE(k_svc_ops); k++)
                if (wieq(C.scm_fail, k_svc_ops[k]))
                    break;
            if (k == ARRAYSIZE(k_svc_ops)) {
                outf(L"--sim-scm-fail needs query/create/config/start/stop/delete\n");
                return RC_USAGE;
            }
        } else if (wieq(a, L"--help") || wieq(a, L"-h") || wieq(a, L"/?")) {
            usage();
            return RC_OK;
        } else {
            outf(L"unknown argument: %s\n", a);
            usage();
            return RC_USAGE;
        }
    }
    if (!cmd)
        cmd = L"status";
    if ((C.fail_after || C.wfp_deny || C.scm_busy || C.scm_pending || C.scm_fail) && !root) {
        outf(L"--fail-after and simulation options need --root\n");
        return RC_USAGE;
    }
    if ((C.move || C.restore) && (!wieq(cmd, L"scan") || (C.move && C.restore))) {
        outf(L"--move-leftovers or --restore-leftovers (one of them) go with scan\n");
        return RC_USAGE;
    }
    n = GetModuleFileNameW(NULL, C.self_dir, QP);
    if (!n || n >= QP)
        return RC_FAILED;
    dir_of(C.self_dir);
    if (!full_path(from ? from : C.self_dir, C.from)) {
        outf(L"bad --from directory\n");
        return RC_USAGE;
    }
    if (root) {
        C.root_mode = 1;
        if (!full_path(root, C.root) || !dir_exists(C.root)) {
            outf(L"--root %s: not a directory\n", root);
            return RC_USAGE;
        }
    } else {
        C.xp = is_xp_x86();
    }
    if (!setup_paths() || !wjoin(C.drivers, QP, C.sys, L"drivers") ||
        !wjoin(C.driver, QP, C.drivers, L"qxpmap.sys") ||
        (C.root_mode && !wjoin(C.scmfile, QP, C.root, L"qxpmap-scm.bin")))
        return RC_FAILED;
    wfp_init();
    pend_load();
    outf(L"QD3D system-wide installer: %s%s\n", C.root_mode ? L"test root " : L"", C.root_mode ? C.root : C.sys);

    mutating = wieq(cmd, L"install") || wieq(cmd, L"driver") || wieq(cmd, L"uninstall") || C.move || C.restore;
    if (mutating && !C.root_mode && !C.xp) {
        outf(L"This changes Windows XP (x86) system files and refuses to run on this system.\n"
             L"Use --root DIR to exercise it against a test directory.\n");
        return finish(cmd, RC_REFUSED);
    }
    if (wieq(cmd, L"install"))
        return cmd_install();
    if (wieq(cmd, L"driver"))
        return cmd_driver();
    if (wieq(cmd, L"uninstall"))
        return cmd_uninstall();
    if (wieq(cmd, L"scan"))
        return cmd_scan();
    return cmd_status();
}

void qd3dinst_start(void)
{
    DWORD mode;
    int rc;

    g_heap = GetProcessHeap();
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_console = GetConsoleMode(g_out, &mode) != 0;
    rc = run();
    out_flush();
    ExitProcess((UINT)rc);
}
