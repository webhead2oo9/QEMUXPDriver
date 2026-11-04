/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shader bytecode for qd3d9_smoke.
 *
 * Produced by hand-assembling the Direct3D 9 shader token format (no D3DX,
 * no HLSL compiler):
 *   - version token: 0xFFFE0000 (vertex) / 0xFFFF0000 (pixel) | major << 8
 *     | minor;
 *   - instruction token: opcode in bits 0-15 (mov 0x01, add 0x02, dp4 0x09,
 *     dcl 0x1F), instruction length in bits 24-27 for shader model 2.0 and
 *     later (zero for 1.x);
 *   - parameter token: bit 31 set, register number in bits 0-10, register
 *     type in bits 28-30 plus bits 11-12 (temp 0, input 1, const 2,
 *     rastout 4, attrout 5, colorout 8), destination write mask in bits
 *     16-19, source swizzle in bits 16-23 (0xE4 = .xyzw);
 *   - dcl usage token: bit 31 set, usage in bits 0-4 (position 0);
 *   - end token 0x0000FFFF.
 * Each array's disassembly was cross-checked once on the development host
 * with D3DDisassemble from the system d3dcompiler_47.dll; it matches the
 * listing in the comment above the array. The Direct3D 9 runtime validates
 * the tokens again in CreateVertexShader/CreatePixelShader.
 */
#ifndef QD3D9_SMOKE_SHADERS_H
#define QD3D9_SMOKE_SHADERS_H

#include <windows.h>

/*
 * vs_1_1
 * dcl_position v0
 * dp4 oPos.x, v0, c0
 * dp4 oPos.y, v0, c1
 * dp4 oPos.z, v0, c2
 * dp4 oPos.w, v0, c3
 * mov oD0, c4
 */
static const DWORD smoke_vs11_xform_color[] = {
	0xfffe0101,
	0x0000001f, 0x80000000, 0x900f0000,
	0x00000009, 0xc0010000, 0x90e40000, 0xa0e40000,
	0x00000009, 0xc0020000, 0x90e40000, 0xa0e40001,
	0x00000009, 0xc0040000, 0x90e40000, 0xa0e40002,
	0x00000009, 0xc0080000, 0x90e40000, 0xa0e40003,
	0x00000001, 0xd00f0000, 0xa0e40004,
	0x0000ffff,
};

/*
 * ps_2_0
 * dcl v0
 * add r0, v0, c0
 * mov oC0, r0
 */
static const DWORD smoke_ps20_add[] = {
	0xffff0200,
	0x0200001f, 0x80000000, 0x900f0000,
	0x03000002, 0x800f0000, 0x90e40000, 0xa0e40000,
	0x02000001, 0x800f0800, 0x80e40000,
	0x0000ffff,
};

/*
 * ps_1_1
 * add r0, v0, c0
 */
static const DWORD smoke_ps11_add[] = {
	0xffff0101,
	0x00000002, 0x800f0000, 0x90e40000, 0xa0e40000,
	0x0000ffff,
};

#endif
