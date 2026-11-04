/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The parts of d3d9on12.h that Wine's d3d9 device tests use. The mingw-w64
 * headers do not provide this header; the declarations follow Wine's
 * include/d3d9on12.idl (wine-11.0), from which this file is derived.
 *
 * Copyright (C) 2024 Mohamad Al-Jaf (include/d3d9on12.idl)
 */
#ifndef QD3D_CONFORMANCE_D3D9ON12_H
#define QD3D_CONFORMANCE_D3D9ON12_H

#include <d3d9.h>
#include <d3d12.h>

#define MAX_D3D9ON12_QUEUES 2

typedef struct D3D9ON12_ARGS
{
    BOOL Enable9On12;
    IUnknown *pD3D12Device;
    IUnknown *ppD3D12Queues[MAX_D3D9ON12_QUEUES];
    UINT NumQueues;
    UINT NodeMask;
} D3D9ON12_ARGS;

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9On12)(UINT sdk_version, D3D9ON12_ARGS *d3d9on12_args,
        UINT d3d9on12_args_count);

DEFINE_GUID(IID_IDirect3DDevice9On12, 0xe7fda234, 0xb589, 0x4049, 0x94, 0x0d, 0x88, 0x78, 0x97, 0x75, 0x31, 0xc8);

typedef struct IDirect3DDevice9On12 IDirect3DDevice9On12, *LPDIRECT3DDEVICE9ON12, *PDIRECT3DDEVICE9ON12;

typedef struct IDirect3DDevice9On12Vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDirect3DDevice9On12 *iface, REFIID riid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDirect3DDevice9On12 *iface);
    ULONG (STDMETHODCALLTYPE *Release)(IDirect3DDevice9On12 *iface);
    HRESULT (STDMETHODCALLTYPE *GetD3D12Device)(IDirect3DDevice9On12 *iface, REFIID riid, void **out);
    HRESULT (STDMETHODCALLTYPE *UnwrapUnderlyingResource)(IDirect3DDevice9On12 *iface,
            IDirect3DResource9 *resource, ID3D12CommandQueue *queue, REFIID riid, void **out);
    HRESULT (STDMETHODCALLTYPE *ReturnUnderlyingResource)(IDirect3DDevice9On12 *iface,
            IDirect3DResource9 *resource, UINT fence_count, UINT64 *signal_values, ID3D12Fence **fences);
} IDirect3DDevice9On12Vtbl;

struct IDirect3DDevice9On12
{
    const IDirect3DDevice9On12Vtbl *lpVtbl;
};

#define IDirect3DDevice9On12_QueryInterface(p, a, b) ((p)->lpVtbl->QueryInterface(p, a, b))
#define IDirect3DDevice9On12_AddRef(p) ((p)->lpVtbl->AddRef(p))
#define IDirect3DDevice9On12_Release(p) ((p)->lpVtbl->Release(p))
#define IDirect3DDevice9On12_GetD3D12Device(p, a, b) ((p)->lpVtbl->GetD3D12Device(p, a, b))
#define IDirect3DDevice9On12_UnwrapUnderlyingResource(p, a, b, c, d) \
        ((p)->lpVtbl->UnwrapUnderlyingResource(p, a, b, c, d))
#define IDirect3DDevice9On12_ReturnUnderlyingResource(p, a, b, c, d) \
        ((p)->lpVtbl->ReturnUnderlyingResource(p, a, b, c, d))

#endif
