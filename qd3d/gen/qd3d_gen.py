#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the QD3D Direct3D 9 remoting layer from apitrace's D3D9 spec.

Writes these checked-in files (never edit them by hand):
  qd3d/proto/qd3d_ops.h    method opcodes, interface type tags, op info
  qd3d/guest/qd3d9_gen.h   guest prototypes (generated + hand-written)
  qd3d/guest/qd3d9_gen.c   guest marshalling methods, vtables, layout asserts
  qd3d/host/qd3dx_gen.h    host override prototypes
  qd3d/host/qd3dx_gen.c    host decode/validate/execute and dispatch table

Usage: python3 qd3d/gen/qd3d_gen.py [--check]
  --check  fail (exit 1) if any checked-in file differs from the output

The wire encoding is documented in qd3d/proto/qd3d_proto.h. Every method is
in exactly one category:
  generic      encoded by this generator on both sides
  GUEST_LOCAL  answered by the guest proxy; never sent
  EXT          sent as a hand-specified extension operation
GUEST_OVERRIDE and HOST_OVERRIDE are generic methods whose vtable entry
(guest) or native call (host) is hand-written; the wire format stays
generated.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QD3D = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(QD3D, 'third_party', 'apitrace'))

from specs import stdapi  # noqa: E402
from specs.winapi import IUnknown  # noqa: E402
from specs import d3d9  # noqa: E402

DECLARING = [
    IUnknown,
    d3d9.IDirect3D9,
    d3d9.IDirect3DDevice9,
    d3d9.IDirect3DStateBlock9,
    d3d9.IDirect3DSwapChain9,
    d3d9.IDirect3DResource9,
    d3d9.IDirect3DVertexDeclaration9,
    d3d9.IDirect3DVertexShader9,
    d3d9.IDirect3DPixelShader9,
    d3d9.IDirect3DBaseTexture9,
    d3d9.IDirect3DTexture9,
    d3d9.IDirect3DVolumeTexture9,
    d3d9.IDirect3DCubeTexture9,
    d3d9.IDirect3DVertexBuffer9,
    d3d9.IDirect3DIndexBuffer9,
    d3d9.IDirect3DSurface9,
    d3d9.IDirect3DVolume9,
    d3d9.IDirect3DQuery9,
]
ABSTRACT = {'IUnknown', 'IDirect3DResource9', 'IDirect3DBaseTexture9'}
CONCRETE = [i for i in DECLARING if i.name not in ABSTRACT]


def keyset(text):
    return set(text.split())


# Answered entirely inside the guest proxy.
GUEST_LOCAL = keyset('''
IUnknown::QueryInterface IUnknown::AddRef IUnknown::Release
IDirect3D9::RegisterSoftwareDevice IDirect3D9::GetAdapterCount
IDirect3D9::GetAdapterModeCount IDirect3D9::EnumAdapterModes
IDirect3D9::GetAdapterDisplayMode IDirect3D9::GetAdapterMonitor
IDirect3DDevice9::TestCooperativeLevel IDirect3DDevice9::GetDirect3D
IDirect3DDevice9::GetDisplayMode IDirect3DDevice9::GetCreationParameters
IDirect3DDevice9::SetCursorProperties IDirect3DDevice9::SetCursorPosition
IDirect3DDevice9::ShowCursor
IDirect3DDevice9::GetRasterStatus IDirect3DDevice9::DrawRectPatch
IDirect3DDevice9::DrawTriPatch IDirect3DDevice9::DeletePatch
IDirect3DStateBlock9::GetDevice
IDirect3DSwapChain9::GetRasterStatus IDirect3DSwapChain9::GetDisplayMode
IDirect3DSwapChain9::GetDevice IDirect3DSwapChain9::GetPresentParameters
IDirect3DResource9::GetDevice IDirect3DResource9::SetPrivateData
IDirect3DResource9::GetPrivateData IDirect3DResource9::FreePrivateData
IDirect3DResource9::SetPriority IDirect3DResource9::GetPriority
IDirect3DResource9::GetType
IDirect3DVertexDeclaration9::GetDevice IDirect3DVertexDeclaration9::GetDeclaration
IDirect3DVertexShader9::GetDevice IDirect3DVertexShader9::GetFunction
IDirect3DPixelShader9::GetDevice IDirect3DPixelShader9::GetFunction
IDirect3DTexture9::LockRect IDirect3DTexture9::UnlockRect
IDirect3DVolumeTexture9::LockBox IDirect3DVolumeTexture9::UnlockBox
IDirect3DCubeTexture9::LockRect IDirect3DCubeTexture9::UnlockRect
IDirect3DVertexBuffer9::Lock IDirect3DVertexBuffer9::Unlock
IDirect3DIndexBuffer9::Lock IDirect3DIndexBuffer9::Unlock
IDirect3DSurface9::GetContainer IDirect3DSurface9::LockRect
IDirect3DSurface9::UnlockRect IDirect3DSurface9::GetDC IDirect3DSurface9::ReleaseDC
IDirect3DVolume9::GetDevice IDirect3DVolume9::SetPrivateData
IDirect3DVolume9::GetPrivateData IDirect3DVolume9::FreePrivateData
IDirect3DVolume9::GetContainer IDirect3DVolume9::LockBox IDirect3DVolume9::UnlockBox
IDirect3DQuery9::GetDevice IDirect3DQuery9::GetType IDirect3DQuery9::GetDataSize
''')

# Sent as QD3D_OP_* extension operations (see qd3d_proto.h).
EXT = keyset('''
IDirect3D9::CreateDevice IDirect3DDevice9::Reset IDirect3DDevice9::Present
IDirect3DSwapChain9::Present IDirect3DQuery9::GetData
IDirect3DDevice9::CreateAdditionalSwapChain
''')

# Generic wire format; the guest vtable points at a hand-written qd3d9o_*
# wrapper that calls the generated qd3d9g_* function.
GUEST_OVERRIDE = keyset('''
IDirect3DDevice9::CreateTexture IDirect3DDevice9::CreateVolumeTexture
IDirect3DDevice9::CreateCubeTexture IDirect3DDevice9::CreateVertexBuffer
IDirect3DDevice9::CreateIndexBuffer IDirect3DDevice9::CreateRenderTarget
IDirect3DDevice9::CreateDepthStencilSurface IDirect3DDevice9::CreateOffscreenPlainSurface
IDirect3DDevice9::UpdateSurface IDirect3DDevice9::UpdateTexture
IDirect3DDevice9::GetRenderTargetData IDirect3DDevice9::GetFrontBufferData
IDirect3DDevice9::StretchRect IDirect3DDevice9::ColorFill
IDirect3DDevice9::ProcessVertices IDirect3DDevice9::CreateVertexDeclaration
IDirect3DDevice9::CreateVertexShader IDirect3DDevice9::CreatePixelShader
IDirect3DDevice9::CreateQuery
IDirect3DSwapChain9::GetFrontBufferData
IDirect3DTexture9::GetSurfaceLevel IDirect3DVolumeTexture9::GetVolumeLevel
IDirect3DCubeTexture9::GetCubeMapSurface
IDirect3DTexture9::GetLevelDesc IDirect3DCubeTexture9::GetLevelDesc
IDirect3DVolumeTexture9::GetLevelDesc IDirect3DSurface9::GetDesc IDirect3DVolume9::GetDesc
IDirect3DVertexBuffer9::GetDesc IDirect3DIndexBuffer9::GetDesc
IDirect3DDevice9::SetTexture IDirect3DDevice9::SetStreamSource IDirect3DDevice9::SetIndices
IDirect3DDevice9::SetVertexDeclaration IDirect3DDevice9::SetVertexShader
IDirect3DDevice9::SetPixelShader IDirect3DDevice9::SetRenderTarget
IDirect3DDevice9::SetDepthStencilSurface IDirect3DDevice9::GetSwapChain
IDirect3DDevice9::GetBackBuffer IDirect3DSwapChain9::GetBackBuffer
IDirect3DDevice9::GetRenderTarget IDirect3DDevice9::GetDepthStencilSurface
IDirect3DDevice9::BeginStateBlock IDirect3DDevice9::EndStateBlock
IDirect3DDevice9::CreateStateBlock IDirect3DStateBlock9::Capture
IDirect3DStateBlock9::Apply IDirect3DDevice9::DrawPrimitiveUP
IDirect3DDevice9::DrawIndexedPrimitiveUP
IDirect3DDevice9::DrawPrimitive IDirect3DDevice9::DrawIndexedPrimitive
IDirect3DResource9::PreLoad
IDirect3DDevice9::SetVertexShaderConstantF IDirect3DDevice9::SetVertexShaderConstantI
IDirect3DDevice9::SetVertexShaderConstantB IDirect3DDevice9::SetPixelShaderConstantF
IDirect3DDevice9::SetPixelShaderConstantI IDirect3DDevice9::SetPixelShaderConstantB
IDirect3DTexture9::AddDirtyRect IDirect3DCubeTexture9::AddDirtyRect
IDirect3DVolumeTexture9::AddDirtyBox IDirect3DDevice9::EvictManagedResources
IDirect3DDevice9::BeginScene IDirect3DDevice9::EndScene IDirect3DDevice9::Clear
IDirect3DDevice9::SetFVF IDirect3DDevice9::SetStreamSourceFreq
IDirect3DBaseTexture9::SetAutoGenFilterType IDirect3DBaseTexture9::SetLOD
IDirect3DBaseTexture9::GetLOD IDirect3DDevice9::GetVertexDeclaration
''')

# Generic wire format; the host calls a hand-written qd3dxo_* function with
# the native method's signature instead of the native method.
HOST_OVERRIDE = keyset('''
IDirect3D9::GetAdapterIdentifier IDirect3D9::GetDeviceCaps
IDirect3DDevice9::GetDeviceCaps IDirect3DDevice9::GetAvailableTextureMem
IDirect3DDevice9::CreateTexture IDirect3DDevice9::CreateVolumeTexture
IDirect3DDevice9::CreateCubeTexture IDirect3DDevice9::CreateVertexBuffer
IDirect3DDevice9::CreateIndexBuffer IDirect3DDevice9::ValidateDevice
IDirect3DDevice9::GetFrontBufferData IDirect3DSwapChain9::GetFrontBufferData
IDirect3DDevice9::SetTexture IDirect3DDevice9::GetTexture
IDirect3DDevice9::SetLight IDirect3DDevice9::GetLight IDirect3DDevice9::LightEnable
IDirect3DDevice9::GetLightEnable IDirect3DDevice9::SetPaletteEntries
IDirect3DDevice9::GetPaletteEntries IDirect3DDevice9::SetCurrentTexturePalette
IDirect3DDevice9::SetStreamSource IDirect3DDevice9::GetStreamSource
IDirect3DDevice9::SetStreamSourceFreq IDirect3DDevice9::GetStreamSourceFreq
IDirect3DDevice9::GetRenderTarget IDirect3DDevice9::SetSamplerState
IDirect3DDevice9::GetSamplerState IDirect3DDevice9::SetTextureStageState
IDirect3DDevice9::GetTextureStageState IDirect3DDevice9::DrawIndexedPrimitiveUP
IDirect3DDevice9::DrawPrimitive IDirect3DDevice9::DrawIndexedPrimitive
IDirect3DDevice9::DrawPrimitiveUP IDirect3DBaseTexture9::SetLOD
IDirect3DStateBlock9::Apply IDirect3DDevice9::BeginStateBlock
IDirect3DDevice9::EndStateBlock
''')

# Data outputs the native runtime writes even when the call fails
# (CheckDeviceMultiSampleType reports 1 quality level with NOTAVAILABLE).
OUTDATA_ALWAYS = keyset('''
IDirect3D9::CheckDeviceMultiSampleType
''')

# HRESULT-only methods whose result is the answer the caller asked for.
# BeginStateBlock fails while a recording is open; games rely on that
# (F.E.A.R. nests recordings and skips the inner one when Begin fails).
SYNC_HRESULT = keyset('''
IDirect3D9::CheckDeviceType IDirect3D9::CheckDeviceFormat
IDirect3D9::CheckDepthStencilMatch IDirect3D9::CheckDeviceFormatConversion
IDirect3DDevice9::BeginStateBlock
''')

# Out-parameters follow the native runtime (measured on the stock 32-bit
# runtime): a failed call leaves data outputs as the caller set them, getters
# return NULL objects, and creating methods set *pp = NULL, except these,
# which leave it untouched (an unsupported query type, EndStateBlock without
# BeginStateBlock).
NEWOBJ_KEEP = keyset('''
IDirect3DDevice9::CreateQuery IDirect3DDevice9::EndStateBlock
''')

U32, OBJ, NEWOBJ, OUTOBJ, DATA, OUTDATA, IGNORED = (
    'U32', 'OBJ', 'NEWOBJ', 'OUTOBJ', 'DATA', 'OUTDATA', 'IGNORED')

POINTER_LIKE = (stdapi.Pointer, stdapi.ObjPointer, stdapi.LinearPointer,
                stdapi.IntPointer, stdapi.Handle, stdapi.Opaque, stdapi.Blob,
                stdapi.String, stdapi.Reference, stdapi.Interface)
UNSAFE_STRUCTS = {'RGNDATA', 'D3DPRESENT_PARAMETERS', 'D3DLOCKED_RECT', 'D3DLOCKED_BOX',
                  'D3DDEVICE_CREATION_PARAMETERS'}
# apitrace models these inline char arrays as String without a length; the
# SDK declarations hold only inline arrays, DWORDs, a LARGE_INTEGER and a
# GUID, so the field layout is identical for x86 and x64.
SAFE_STRUCTS = {'D3DADAPTER_IDENTIFIER9'}
SCALAR32_LITERALS = {'int', 'unsigned int', 'long', 'unsigned long', 'float',
                     'int32_t', 'uint32_t'}

# sizeof() of every type that travels as DATA/OUTDATA, measured with
# i686-w64-mingw32-gcc and x86_64-w64-mingw32-gcc 13 (guest, host). The
# generated code asserts these at compile time on each side. Where they
# differ only by tail padding (LARGE_INTEGER is 4-aligned on i686), the wire
# carries the guest size and the host copies that prefix.
WIRE_LAYOUT = {
    'BOOL': (4, 4), 'DWORD': (4, 4), 'UINT': (4, 4), 'float': (4, 4), 'int': (4, 4),
    'D3DADAPTER_IDENTIFIER9': (1100, 1104), 'D3DBOX': (24, 24), 'D3DCAPS9': (304, 304),
    'D3DCLIPSTATUS9': (8, 8), 'D3DGAMMARAMP': (1536, 1536), 'D3DINDEXBUFFER_DESC': (20, 20),
    'D3DLIGHT9': (104, 104), 'D3DMATERIAL9': (68, 68), 'D3DMATRIX': (64, 64),
    'D3DRECT': (16, 16), 'D3DSURFACE_DESC': (32, 32), 'D3DVERTEXBUFFER_DESC': (24, 24),
    'D3DVERTEXELEMENT9': (8, 8), 'D3DVIEWPORT9': (24, 24), 'D3DVOLUME_DESC': (28, 28),
    'PALETTEENTRY': (4, 4), 'POINT': (8, 8), 'RECT': (16, 16),
}


def wire_size(ctype):
    """C expression for the on-wire byte size of one element of ctype."""
    base = ctype.replace('const ', '').strip()
    if base not in WIRE_LAYOUT:
        raise GenError('%s travels as data but has no measured WIRE_LAYOUT entry' % base)
    guest, host = WIRE_LAYOUT[base]
    return '%du' % guest if guest != host else 'sizeof(%s)' % ctype


class GenError(Exception):
    pass


def strip(t):
    while isinstance(t, (stdapi.Alias, stdapi.Const)):
        t = t.type
    return t


def cexpr(t):
    if t is stdapi.Void or isinstance(strip(t), stdapi._Void) and t.expr == 'void':
        return 'void'
    return t.expr if t.expr is not None else 'void *'


def is_scalar32(t):
    s = strip(t)
    if isinstance(s, stdapi.Literal):
        return s.expr in SCALAR32_LITERALS
    if isinstance(s, (stdapi.Enum, stdapi.Bitmask)):
        return True
    if isinstance(s, stdapi.Polymorphic):
        return s.defaultType is not None and is_scalar32(s.defaultType)
    return False


def is_float(t):
    s = strip(t)
    return isinstance(s, stdapi.Literal) and s.expr == 'float'


def wire_safe(t, depth=0):
    """True for types whose bytes mean the same thing in an x86 guest and
    an x64 host: no pointers, handles, or pointer-sized integers."""
    s = strip(t)
    if depth > 16:
        raise GenError('type nesting too deep')
    if isinstance(s, POINTER_LIKE):
        return False
    if isinstance(s, stdapi.Literal):
        return s.expr not in ('size_t', 'intptr_t', 'uintptr_t')
    if isinstance(s, (stdapi.Enum, stdapi.Bitmask)):
        return True
    if isinstance(s, stdapi.Polymorphic):
        return s.defaultType is not None and wire_safe(s.defaultType, depth + 1)
    if isinstance(s, stdapi.Array):
        return isinstance(s.length, int) and wire_safe(s.type, depth + 1)
    if isinstance(s, stdapi.Struct):
        if s.name in UNSAFE_STRUCTS:
            return False
        if s.name in SAFE_STRUCTS:
            return True
        return all(wire_safe(mt, depth + 1) for mt, _ in s.members)
    return False


def is_newobj(key):
    method = key.split('::')[1]
    return method.startswith('Create') or key == 'IDirect3DDevice9::EndStateBlock'


class Arg:
    def __init__(self, a, kind, **kw):
        self.a = a
        self.name = a.name
        self.kind = kind
        self.ctype = cexpr(a.type)
        self.__dict__.update(kw)


def classify(key, a):
    s = strip(a.type)
    where = '%s(%s)' % (key, a.name)
    if a.output and not a.input:
        if isinstance(s, stdapi.Pointer):
            pt = strip(s.type)
            if isinstance(pt, stdapi.ObjPointer):
                if not isinstance(pt.type, stdapi.Interface):
                    raise GenError('%s: untyped object output' % where)
                kind = NEWOBJ if is_newobj(key) else OUTOBJ
                return Arg(a, kind, iface=pt.type.name, pointee=cexpr(s.type))
            if is_scalar32(pt) or wire_safe(pt):
                return Arg(a, OUTDATA, pointee=cexpr(s.type), elem=None)
        if isinstance(s, stdapi.Array) and wire_safe(s.type):
            return Arg(a, OUTDATA, pointee=None, elem=cexpr(s.type), length=s.length)
        raise GenError('%s: unsupported output %s' % (where, a.type.expr))
    if a.input and a.output:
        if isinstance(s, stdapi.Pointer):
            pt = strip(s.type)
            if isinstance(pt, stdapi.Polymorphic) and pt.defaultType is not None:
                pt = strip(pt.defaultType)
            if isinstance(pt, stdapi.IntPointer):
                return Arg(a, IGNORED)
        raise GenError('%s: unsupported in/out %s' % (where, a.type.expr))
    if is_scalar32(s):
        return Arg(a, U32, float=is_float(s))
    if isinstance(s, stdapi.ObjPointer):
        if not isinstance(s.type, stdapi.Interface):
            raise GenError('%s: untyped object input' % where)
        return Arg(a, OBJ, iface=s.type.name)
    if isinstance(s, stdapi.Pointer):
        pt = strip(s.type)
        if is_scalar32(pt) or wire_safe(pt):
            return Arg(a, DATA, form='single', pointee=cexpr(s.type))
    if isinstance(s, stdapi.Array) and wire_safe(s.type):
        return Arg(a, DATA, form='array', elem=cexpr(s.type), length=s.length)
    if isinstance(s, stdapi.Blob):
        return Arg(a, DATA, form='blob', size=s.size)
    raise GenError('%s: unsupported input %s' % (where, a.type.expr))


def ret_kind(m):
    if cexpr(m.type) == 'void':
        return 'void'
    if cexpr(m.type) == 'HRESULT':
        return 'hresult'
    if is_float(m.type):
        return 'float'
    if is_scalar32(m.type):
        return 'u32'
    raise GenError('unsupported return type %s of %s' % (m.type.expr, m.name))


IDENT = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')
CALL = re.compile(r'^(_\w+)\((.*)\)$')


def widen(expr, names):
    """Arithmetic size expression with every argument widened to 64 bits."""
    if isinstance(expr, int):
        return '%du' % expr
    return '(' + IDENT.sub(lambda mo: '(uint64_t)(%s)' % mo.group(0)
                           if mo.group(0) in names else mo.group(0), expr) + ')'


GUEST_FUNCS = {
    '_shaderSize': 'qd3d_shader_size(%s)',
    '_declCount': 'qd3d_decl_count(%s)',
    '_vertexDataSize': 'qd3d_vertex_data_size(%s)',
    '_indexDataSize': 'qd3d_index_data_size(%s)',
}


def guest_bytes(arg, names):
    """Guest C expression for the byte length of a DATA/OUTDATA item."""
    if arg.kind == DATA and arg.form == 'single':
        return '(uint64_t)%s' % wire_size(arg.pointee)
    if arg.kind == OUTDATA and arg.pointee is not None:
        return '(uint64_t)%s' % wire_size(arg.pointee)
    length = arg.length if arg.kind == OUTDATA or arg.form == 'array' else arg.size
    mo = CALL.match(length) if isinstance(length, str) else None
    count = (GUEST_FUNCS[mo.group(1)] % mo.group(2)) if mo else widen(length, names)
    if arg.kind == DATA and arg.form == 'blob':
        return '(uint64_t)(%s)' % count
    return '(uint64_t)(%s) * %s' % (count, wire_size(arg.elem))


def host_check(arg, names, args_by_name):
    """Host C condition that the received DATA length n_<name> is exact."""
    n = 'n_' + arg.name
    p = 'p_' + arg.name
    if arg.form == 'single':
        return '%s == %s' % (n, wire_size(arg.pointee))
    length = arg.length if arg.form == 'array' else arg.size
    mo = CALL.match(length) if isinstance(length, str) else None
    if mo:
        fn, inner = mo.group(1), mo.group(2)
        if fn == '_declCount':
            e = 'sizeof(D3DVERTEXELEMENT9)'
            return ('%s != 0u && %s %% %s == 0 && (uint64_t)qd3d_decl_count_bounded((const D3DVERTEXELEMENT9 *)%s, '
                    '%s / %s) * %s == %s' % (n, n, e, p, n, e, e, n))
        if fn == '_shaderSize':
            return '%s != 0u && qd3d_shader_size_bounded(%s, %s) == %s' % (n, p, n, n)
        if fn in ('_vertexDataSize', '_indexDataSize'):
            helper = GUEST_FUNCS[fn].split('(')[0]
            return '(uint64_t)%s == %s(%s)' % (n, helper, inner)
        raise GenError('no host check for %s' % fn)
    if arg.form == 'blob':
        return '(uint64_t)%s == %s' % (n, widen(length, names))
    return '(uint64_t)%s == %s * %s' % (n, widen(length, names), wire_size(arg.elem))


class Method:
    def __init__(self, decl, m, index):
        self.decl = decl
        self.m = m
        self.name = m.name
        self.key = '%s::%s' % (decl.name, m.name)
        self.op = 'QD3D_OP_%s_%s' % (decl.name, m.name)
        self.index = index
        cats = [self.key in GUEST_LOCAL, self.key in EXT]
        if sum(cats) > 1:
            raise GenError('%s is in more than one category' % self.key)
        self.local = self.key in GUEST_LOCAL
        self.ext = self.key in EXT
        self.generic = not (self.local or self.ext)
        if not self.generic and (self.key in GUEST_OVERRIDE or self.key in HOST_OVERRIDE):
            raise GenError('%s: overrides apply to generic methods only' % self.key)
        self.guest_override = self.key in GUEST_OVERRIDE
        self.host_override = self.key in HOST_OVERRIDE
        self.rk = ret_kind(m) if self.generic else None
        self.rtype = cexpr(m.type)
        self.args = [classify(self.key, a) for a in m.args] if self.generic else None
        if self.generic:
            outs = [a for a in self.args if a.kind == OUTDATA and a.pointee is None]
            if len(outs) > 1:
                raise GenError('%s: more than one output array' % self.key)
            self.sync = (any(a.kind in (NEWOBJ, OUTOBJ, OUTDATA) for a in self.args)
                         or self.rk not in ('hresult', 'void') or self.key in SYNC_HRESULT)
            if any(a.kind == IGNORED for a in self.args) and self.rk != 'hresult':
                raise GenError('%s: ignored argument needs an HRESULT method' % self.key)
        else:
            self.sync = None

    def params(self, this_type=None):
        this = '%s *This' % (this_type or self.decl.name)
        return ', '.join([this] + ['%s %s' % (cexpr(a.type), a.name) for a in self.m.args])

    def arg_names(self):
        return [a.name for a in self.m.args]

    def guest_impl(self):
        if self.local or self.ext or self.guest_override:
            return 'qd3d9o_%s_%s' % (self.decl.name, self.name)
        return 'qd3d9g_%s_%s' % (self.decl.name, self.name)


def collect():
    methods = []
    index = 0
    seen = set()
    for decl in DECLARING:
        for m in decl.methods:
            meth = Method(decl, m, index)
            methods.append(meth)
            seen.add(meth.key)
            index += 1
    for s, label in ((GUEST_LOCAL, 'GUEST_LOCAL'), (EXT, 'EXT'), (GUEST_OVERRIDE, 'GUEST_OVERRIDE'),
                     (HOST_OVERRIDE, 'HOST_OVERRIDE'), (SYNC_HRESULT, 'SYNC_HRESULT'),
                     (NEWOBJ_KEEP, 'NEWOBJ_KEEP'), (OUTDATA_ALWAYS, 'OUTDATA_ALWAYS')):
        stale = s - seen
        if stale:
            raise GenError('%s names unknown methods: %s' % (label, ', '.join(sorted(stale))))
    return methods


HEADER = '/* SPDX-License-Identifier: GPL-2.0-only */\n/* Generated by qd3d/gen/qd3d_gen.py from apitrace specs. Do not edit. */\n'


def gen_ops(methods):
    o = [HEADER, '#ifndef QD3D_OPS_H\n#define QD3D_OPS_H\n\n#include <stdint.h>\n\n']
    o.append('/* Interface type tags. Abstract tags only appear as declared types. */\n')
    o.append('enum qd3d_type {\n    QD3D_T_NONE = 0,\n')
    for i, decl in enumerate(DECLARING):
        o.append('    QD3D_T_%s = %d,\n' % (decl.name, i + 1))
    o.append('    QD3D_T_COUNT = %d\n};\n\n' % (len(DECLARING) + 1))
    o.append('/* Base interface of each tag (QD3D_T_NONE for IUnknown). */\n')
    o.append('static const uint8_t qd3d_type_base[QD3D_T_COUNT] = {\n    0,\n')
    for decl in DECLARING:
        base = 'QD3D_T_%s' % decl.base.name if decl.base is not None else 'QD3D_T_NONE'
        o.append('    %s, /* %s */\n' % (base, decl.name))
    o.append('};\n\n')
    o.append('static const uint8_t qd3d_type_concrete[QD3D_T_COUNT] = {\n    0,\n')
    for decl in DECLARING:
        o.append('    %d, /* %s */\n' % (0 if decl.name in ABSTRACT else 1, decl.name))
    o.append('};\n\n')
    o.append('static inline int qd3d_type_isa(uint32_t type, uint32_t base)\n{\n'
             '    while (type != 0u && type < (uint32_t)QD3D_T_COUNT) {\n'
             '        if (type == base)\n            return 1;\n'
             '        type = qd3d_type_base[type];\n    }\n    return 0;\n}\n\n')
    o.append('/* Method opcodes, one per declaring interface and method. */\n')
    for meth in methods:
        o.append('#define %s (QD3D_OP_METHOD_BASE + %du)\n' % (meth.op, meth.index))
    o.append('#define QD3D_OP_METHOD_COUNT %du\n\n' % len(methods))
    o.append('#define QD3D_OPF_SYNC  1u /* generic and synchronous */\n'
             '#define QD3D_OPF_LOCAL 2u /* answered by the guest, never sent */\n'
             '#define QD3D_OPF_EXT   4u /* sent as an extension operation */\n\n')
    o.append('struct qd3d_op_info {\n    const char *name;\n    uint8_t decl_type;\n    uint8_t flags;\n};\n\n')
    o.append('#ifdef QD3D_DEFINE_OP_INFO\n'
             'const struct qd3d_op_info qd3d_op_info[QD3D_OP_METHOD_COUNT] = {\n')
    for meth in methods:
        flags = []
        if meth.generic and meth.sync:
            flags.append('QD3D_OPF_SYNC')
        if meth.local:
            flags.append('QD3D_OPF_LOCAL')
        if meth.ext:
            flags.append('QD3D_OPF_EXT')
        o.append('    { "%s", QD3D_T_%s, %s },\n' % (meth.key, meth.decl.name, ' | '.join(flags) or '0'))
    o.append('};\n#else\nextern const struct qd3d_op_info qd3d_op_info[QD3D_OP_METHOD_COUNT];\n#endif\n\n')
    o.append('#endif /* QD3D_OPS_H */\n')
    return ''.join(o)


def gen_guest_h(methods):
    o = [HEADER, '#ifndef QD3D9_GEN_H\n#define QD3D9_GEN_H\n\n#include "qd3d9_rt.h"\n\n']
    o.append('/* Generated marshalling methods (vtable entries or override helpers). */\n')
    for meth in methods:
        if meth.generic:
            o.append('%s STDMETHODCALLTYPE qd3d9g_%s_%s(%s);\n'
                     % (meth.rtype, meth.decl.name, meth.name, meth.params()))
    o.append('\n/* Hand-written: guest-local, extension, and override methods. */\n')
    for meth in methods:
        if meth.local or meth.ext or meth.guest_override:
            tag = 'local' if meth.local else 'ext' if meth.ext else 'override'
            o.append('%s STDMETHODCALLTYPE qd3d9o_%s_%s(%s); /* %s */\n'
                     % (meth.rtype, meth.decl.name, meth.name, meth.params(), tag))
    o.append('\n#endif /* QD3D9_GEN_H */\n')
    return ''.join(o)


def guest_method(meth):
    names = meth.arg_names()
    L = ['%s STDMETHODCALLTYPE qd3d9g_%s_%s(%s)' % (meth.rtype, meth.decl.name, meth.name, meth.params()), '{']
    L.append('    struct qd3d_call k;')
    if meth.sync:
        L.append('    uint32_t r_;')
    fixed = 0
    pads = []
    for a in meth.args:
        fixed += 4
        if a.kind == DATA:
            L.append('    const uint64_t n_%s = %s ? %s : 0u;' % (a.name, a.name, guest_bytes(a, names)))
            pads.append('QD3D_PAD4(n_%s)' % a.name)
            length = a.length if a.form == 'array' else getattr(a, 'size', None)
            mo = CALL.match(length) if isinstance(length, str) else None
            if mo and mo.group(1) in ('_shaderSize', '_declCount'):
                # Terminator-scanned data without a terminator: fail like
                # the native runtime instead of sending it.
                L.append('    if (%s && n_%s == 0u)' % (a.name, a.name))
                L.append('        return D3DERR_INVALIDCALL;')
    for a in meth.args:
        if a.kind == NEWOBJ:
            L.append('    struct qd3d_obj *o_%s = NULL;' % a.name)
    for a in meth.args:
        if a.kind == NEWOBJ:
            L.append('    if (%s) {' % a.name)
            L.append('        o_%s = qd3d_obj_new(qd3d_obj_device(This), QD3D_T_%s);' % (a.name, a.iface))
            L.append('        if (!o_%s) {' % a.name)
            L.append('            *%s = NULL;' % a.name)
            for b in meth.args:
                if b.kind == NEWOBJ and b is not a:
                    L.append('            if (o_%s)' % b.name)
                    L.append('                qd3d_obj_discard(o_%s);' % b.name)
            L.append('            return E_OUTOFMEMORY;')
            L.append('        }')
            L.append('    }')
    payload = ' + '.join(['%du' % fixed] + pads)
    L.append('    qd3d_call_begin(&k, %s, qd3d_obj_id(This), %d, %s);'
             % (meth.op, 1 if meth.sync else 0, payload))
    for a in meth.args:
        if a.kind == U32:
            if a.float:
                L.append('    qd3d_put_u32(&k, qd3d_f2u(%s));' % a.name)
            else:
                L.append('    qd3d_put_u32(&k, (uint32_t)%s);' % a.name)
        elif a.kind == OBJ:
            L.append('    qd3d_put_u32(&k, qd3d_obj_id(%s));' % a.name)
        elif a.kind == NEWOBJ:
            L.append('    qd3d_put_u32(&k, o_%s ? o_%s->id : 0u);' % (a.name, a.name))
        elif a.kind in (OUTOBJ, OUTDATA, IGNORED):
            L.append('    qd3d_put_u32(&k, %s != NULL);' % a.name)
        elif a.kind == DATA:
            L.append('    qd3d_put_data(&k, %s, n_%s);' % (a.name, a.name))
    if meth.sync:
        L.append('    r_ = qd3d_call_end(&k);')
        for a in meth.args:
            if a.kind == OUTOBJ:
                L.append('    if (%s) {' % a.name)
                L.append('        uint32_t id_ = 0, t_ = 0;')
                L.append('        qd3d_get_u32(&k, &id_);')
                L.append('        qd3d_get_u32(&k, &t_);')
                L.append('        *%s = (%s)qd3d_obj_from_id(qd3d_obj_device(This), id_, t_, QD3D_T_%s);'
                         % (a.name, a.pointee, a.iface))
                L.append('    }')
            elif a.kind == OUTDATA:
                if meth.rk == 'hresult' and meth.key not in OUTDATA_ALWAYS:
                    # A failed call leaves the caller's buffer alone; the
                    # item is still consumed so later items stay aligned.
                    L.append('    if (%s) {' % a.name)
                    L.append('        if (SUCCEEDED((HRESULT)r_))')
                    L.append('            qd3d_get_data(&k, %s, %s);' % (a.name, guest_bytes(a, names)))
                    L.append('        else')
                    L.append('            qd3d_skip_data(&k);')
                    L.append('    }')
                else:
                    L.append('    if (%s)' % a.name)
                    L.append('        qd3d_get_data(&k, %s, %s);' % (a.name, guest_bytes(a, names)))
        L.append('    qd3d_call_done(&k);')
        for a in meth.args:
            if a.kind == NEWOBJ:
                L.append('    if (o_%s) {' % a.name)
                L.append('        if (SUCCEEDED((HRESULT)r_)) {')
                L.append('            *%s = (%s)o_%s;' % (a.name, a.pointee, a.name))
                L.append('        } else {')
                L.append('            qd3d_obj_discard(o_%s);' % a.name)
                if meth.key not in NEWOBJ_KEEP:
                    L.append('            *%s = NULL;' % a.name)
                L.append('        }')
                L.append('    }')
        if meth.rk == 'hresult':
            L.append('    return (HRESULT)r_;')
        elif meth.rk == 'float':
            L.append('    return qd3d_u2f(r_);')
        elif meth.rk == 'u32':
            L.append('    return (%s)r_;' % meth.rtype)
        else:
            L.append('    (void)r_;')
    else:
        L.append('    (void)qd3d_call_end(&k);')
        L.append('    qd3d_call_done(&k);')
        if meth.rk == 'hresult':
            L.append('    return D3D_OK;')
    L.append('}')
    return '\n'.join(L) + '\n'


# The wire prefix of a type whose sizes differ must end exactly at the guest
# size, so the host copies whole fields and never padding-dependent bytes.
WIRE_PREFIX = {
    'D3DADAPTER_IDENTIFIER9': 'offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof(DWORD)',
}


def wire_asserts(side):
    """Compile-time checks that this compiler lays out data as measured."""
    o = ['/* Data layouts must match the measured wire sizes (WIRE_LAYOUT). */\n']
    for name in sorted(WIRE_LAYOUT):
        size = WIRE_LAYOUT[name][side]
        o.append('_Static_assert(sizeof(%s) == %du, "%s size");\n' % (name, size, name))
    for name, expr in sorted(WIRE_PREFIX.items()):
        o.append('_Static_assert(%s == %du, "%s wire prefix");\n' % (expr, WIRE_LAYOUT[name][0], name))
    return ''.join(o) + '\n'


def gen_guest_c(methods):
    o = [HEADER, '#include <stddef.h>\n\n#include "qd3d9_gen.h"\n\n']
    o.append(wire_asserts(0))
    o.append('/* Vtable layout must match the SDK interface declarations exactly. */\n')
    for iface in CONCRETE:
        entries = list(iface.iterBaseMethods())
        for idx, (_decl, m) in enumerate(entries):
            o.append('_Static_assert(offsetof(%sVtbl, %s) == %d * sizeof(void *), "%s::%s slot");\n'
                     % (iface.name, m.name, idx, iface.name, m.name))
        o.append('_Static_assert(sizeof(%sVtbl) == %d * sizeof(void *), "%s vtable size");\n\n'
                 % (iface.name, len(entries), iface.name))
    for meth in methods:
        if meth.generic:
            o.append(guest_method(meth))
            o.append('\n')
    by_key = {meth.key: meth for meth in methods}
    for iface in CONCRETE:
        o.append('static const void *const qd3d9_vtbl_%s[] = {\n' % iface.name)
        for decl, m in iface.iterBaseMethods():
            o.append('    (const void *)%s,\n' % by_key['%s::%s' % (decl.name, m.name)].guest_impl())
        o.append('};\n\n')
    o.append('const void *const *const qd3d9_vtables[QD3D_T_COUNT] = {\n')
    for iface in CONCRETE:
        o.append('    [QD3D_T_%s] = qd3d9_vtbl_%s,\n' % (iface.name, iface.name))
    o.append('};\n')
    return ''.join(o)


def host_method(meth):
    names = meth.arg_names()
    d = meth.decl.name
    L = ['static uint32_t qd3dxg_%s_%s(struct qd3dx *ex, const struct qd3d_cmd *c,' % (d, meth.name),
         '        struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)', '{']
    L.append('    %s *This = (%s *)qd3dx_obj(ex, c->obj, QD3D_T_%s);' % (d, d, d))
    for a in meth.args:
        if a.kind == U32:
            L.append('    %s %s;' % (a.ctype, a.name))
        elif a.kind == OBJ:
            L.append('    uint32_t id_%s;' % a.name)
            L.append('    %s %s = NULL;' % (a.ctype, a.name))
        elif a.kind == DATA:
            L.append('    uint32_t n_%s;' % a.name)
            L.append('    const void *p_%s;' % a.name)
        elif a.kind == NEWOBJ:
            L.append('    uint32_t id_%s;' % a.name)
            L.append('    %s o_%s = NULL;' % (a.pointee, a.name))
        elif a.kind == OUTOBJ:
            L.append('    uint32_t pr_%s;' % a.name)
            L.append('    %s o_%s = NULL;' % (a.pointee, a.name))
        elif a.kind == OUTDATA:
            L.append('    uint32_t pr_%s;' % a.name)
            if a.pointee is not None:
                L.append('    %s v_%s;' % (a.pointee, a.name))
            else:
                L.append('    %s *a_%s = NULL;' % (a.elem, a.name))
                L.append('    uint64_t nb_%s;' % a.name)
        elif a.kind == IGNORED:
            L.append('    uint32_t pr_%s;' % a.name)
    if meth.rk != 'void':
        L.append('    %s r_;' % meth.rtype)
    L.append('')
    L.append('    if (!This)')
    L.append('        return QD3D_FAULT_OBJECT;')
    for a in meth.args:
        if a.kind == U32:
            if a.float:
                L.append('    %s = qd3d_u2f(qd3d_rd_u32(rd));' % a.name)
            else:
                L.append('    %s = (%s)qd3d_rd_u32(rd);' % (a.name, a.ctype))
        elif a.kind in (OBJ, NEWOBJ):
            L.append('    id_%s = qd3d_rd_u32(rd);' % a.name)
        elif a.kind == DATA:
            L.append('    p_%s = qd3d_rd_data(rd, &n_%s);' % (a.name, a.name))
        else:
            L.append('    pr_%s = qd3d_rd_u32(rd);' % a.name)
    L.append('    if (!qd3d_rd_end(rd))')
    L.append('        return QD3D_FAULT_DECODE;')
    args_by_name = {a.name: a for a in meth.args}
    for a in meth.args:
        if a.kind == OBJ:
            L.append('    if (id_%s) {' % a.name)
            L.append('        %s = (%s)qd3dx_obj(ex, id_%s, QD3D_T_%s);' % (a.name, a.ctype, a.name, a.iface))
            L.append('        if (!%s)' % a.name)
            L.append('            return QD3D_FAULT_OBJECT;')
            L.append('    }')
        elif a.kind == DATA:
            L.append('    if (p_%s && !(%s))' % (a.name, host_check(a, names, args_by_name)))
            L.append('        return QD3D_FAULT_SIZE;')
        elif a.kind == NEWOBJ:
            L.append('    if (id_%s && !qd3dx_id_is_fresh(ex, id_%s))' % (a.name, a.name))
            L.append('        return QD3D_FAULT_ID;')
        elif a.kind == OUTDATA:
            if a.pointee is not None:
                L.append('    memset(&v_%s, 0, sizeof v_%s);' % (a.name, a.name))
            else:
                mo = CALL.match(a.length) if isinstance(a.length, str) else None
                if mo:
                    raise GenError('%s: output array length must be arithmetic' % meth.key)
                L.append('    nb_%s = %s * %s;' % (a.name, widen(a.length, names), wire_size(a.elem)))
                L.append('    if (pr_%s) {' % a.name)
                L.append('        a_%s = (%s *)qd3dx_scratch(ex, nb_%s);' % (a.name, a.elem, a.name))
                L.append('        if (!a_%s)' % a.name)
                L.append('            return QD3D_FAULT_SIZE;')
                L.append('        memset(a_%s, 0, (size_t)nb_%s);' % (a.name, a.name))
                L.append('    }')
    for a in meth.args:
        if a.kind == IGNORED:
            L.append('    if (pr_%s) {' % a.name)
            L.append('        *ret = (uint32_t)D3DERR_INVALIDCALL;')
            L.append('        return QD3D_FAULT_NONE;')
            L.append('    }')
    call_args = []
    for a in meth.args:
        if a.kind in (U32, OBJ):
            call_args.append(a.name)
        elif a.kind == DATA:
            call_args.append('(%s)p_%s' % (a.ctype, a.name))
        elif a.kind == NEWOBJ:
            call_args.append('id_%s ? &o_%s : NULL' % (a.name, a.name))
        elif a.kind == OUTOBJ:
            call_args.append('pr_%s ? &o_%s : NULL' % (a.name, a.name))
        elif a.kind == OUTDATA:
            if a.pointee is not None:
                call_args.append('pr_%s ? &v_%s : NULL' % (a.name, a.name))
            else:
                call_args.append('a_%s' % a.name)
        elif a.kind == IGNORED:
            call_args.append('NULL')
    if meth.host_override:
        call = 'qd3dxo_%s_%s(%s)' % (d, meth.name, ', '.join(['ex', 'This'] + call_args))
    else:
        call = 'This->lpVtbl->%s(%s)' % (meth.name, ', '.join(['This'] + call_args))
    L.append('    %s%s;' % ('' if meth.rk == 'void' else 'r_ = ', call))
    for a in meth.args:
        if a.kind == NEWOBJ:
            L.append('    if (o_%s) {' % a.name)
            L.append('        if (FAILED(r_) || !qd3dx_bind(ex, id_%s, (IUnknown *)o_%s, QD3D_T_%s)) {'
                     % (a.name, a.name, a.iface))
            L.append('            o_%s->lpVtbl->Release(o_%s);' % (a.name, a.name))
            L.append('            if (SUCCEEDED(r_))')
            L.append('                r_ = E_OUTOFMEMORY;')
            L.append('        }')
            L.append('    }')
        elif a.kind == OUTOBJ:
            L.append('    if (pr_%s) {' % a.name)
            L.append('        uint32_t t_ = 0;')
            L.append('        uint32_t id_ = qd3dx_out_id(ex, (IUnknown *)o_%s, QD3D_T_%s, &t_);'
                     % (a.name, a.iface))
            L.append('        qd3d_wr_u32(rp, id_);')
            L.append('        qd3d_wr_u32(rp, t_);')
            L.append('    }')
        elif a.kind == OUTDATA:
            L.append('    if (pr_%s)' % a.name)
            if a.pointee is not None:
                L.append('        qd3d_wr_data(rp, &v_%s, %s);' % (a.name, wire_size(a.pointee)))
            else:
                L.append('        qd3d_wr_data(rp, a_%s, nb_%s);' % (a.name, a.name))
    if meth.rk == 'void':
        L.append('    *ret = 0u;')
    elif meth.rk == 'float':
        L.append('    *ret = qd3d_f2u(r_);')
    else:
        L.append('    *ret = (uint32_t)r_;')
    L.append('    return rp->err ? QD3D_FAULT_REPLY : QD3D_FAULT_NONE;')
    L.append('}')
    return '\n'.join(L) + '\n'


def gen_host_h(methods):
    o = [HEADER, '#ifndef QD3DX_GEN_H\n#define QD3DX_GEN_H\n\n#include "qd3dx_rt.h"\n\n']
    o.append('/* Hand-written replacements for the native call of generic methods. */\n')
    for meth in methods:
        if meth.host_override:
            o.append('%s qd3dxo_%s_%s(struct qd3dx *ex, %s);\n'
                     % (meth.rtype, meth.decl.name, meth.name, meth.params()))
    o.append('\n#endif /* QD3DX_GEN_H */\n')
    return ''.join(o)


def gen_host_c(methods):
    o = [HEADER, '#include <stddef.h>\n\n#include "qd3dx_gen.h"\n\n']
    o.append(wire_asserts(1))
    for meth in methods:
        if meth.generic:
            o.append(host_method(meth))
            o.append('\n')
    o.append('const qd3dx_method_fn qd3dx_methods[QD3D_OP_METHOD_COUNT] = {\n')
    for meth in methods:
        if meth.generic:
            o.append('    [%s - QD3D_OP_METHOD_BASE] = qd3dxg_%s_%s,\n' % (meth.op, meth.decl.name, meth.name))
    o.append('};\n')
    return ''.join(o)


def main(argv):
    check = '--check' in argv
    methods = collect()
    outputs = {
        os.path.join(QD3D, 'proto', 'qd3d_ops.h'): gen_ops(methods),
        os.path.join(QD3D, 'guest', 'qd3d9_gen.h'): gen_guest_h(methods),
        os.path.join(QD3D, 'guest', 'qd3d9_gen.c'): gen_guest_c(methods),
        os.path.join(QD3D, 'host', 'qd3dx_gen.h'): gen_host_h(methods),
        os.path.join(QD3D, 'host', 'qd3dx_gen.c'): gen_host_c(methods),
    }
    stale = []
    for path, text in outputs.items():
        old = None
        if os.path.exists(path):
            with open(path, 'r', newline='') as f:
                old = f.read()
        if old != text:
            stale.append(path)
            if not check:
                with open(path, 'w', newline='\n') as f:
                    f.write(text)
    generic = sum(1 for m in methods if m.generic)
    sync = sum(1 for m in methods if m.generic and m.sync)
    print('qd3d_gen: %d methods: %d generic (%d sync, %d async), %d guest-local, %d ext'
          % (len(methods), generic, sync, generic - sync,
             sum(1 for m in methods if m.local), sum(1 for m in methods if m.ext)))
    if check and stale:
        for path in stale:
            print('stale: %s' % os.path.relpath(path, os.path.dirname(QD3D)))
        return 1
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except GenError as e:
        print('qd3d_gen: error: %s' % e, file=sys.stderr)
        sys.exit(2)
