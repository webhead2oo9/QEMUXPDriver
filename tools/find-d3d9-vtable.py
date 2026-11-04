#!/usr/bin/env python3
"""Locate COM vtables in a PE by scanning for runs of pointers to function entries.

Used to find the IDirect3D9 vtable in stock XP d3d9.dll so that
IDirect3D9::GetDeviceCaps (slot 14) can be identified by address rather than
guessed from caller heuristics.

A plain "run of code pointers" scan is far too noisy -- jump tables dominate the
results. The discriminator used here is that a COM vtable slot points at a
*function entry point*, which in XP system binaries carries the hotpatch
prologue `mov edi,edi; push ebp; mov ebp,esp` (8b ff 55 8b ec), or at minimum a
standard frame setup (55 8b ec). Jump-table targets sit mid-function and fail
this test.

IDirect3D9 slot map (17 methods):
  0 QueryInterface         6 GetAdapterModeCount    12 CheckDepthStencilMatch
  1 AddRef                 7 EnumAdapterModes       13 CheckDeviceFormatConversion
  2 Release                8 GetAdapterDisplayMode  14 GetDeviceCaps
  3 RegisterSoftwareDevice 9 CheckDeviceType        15 GetAdapterMonitor
  4 GetAdapterCount       10 CheckDeviceFormat      16 CreateDevice
  5 GetAdapterIdentifier  11 CheckDeviceMultiSampleType
"""
import struct
import sys

MIN_SLOTS = 17
HOTPATCH = b"\x8b\xff\x55\x8b\xec"
FRAME = b"\x55\x8b\xec"

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d3d9.dll"
data = open(path, "rb").read()

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
coff = e_lfanew + 4
nsec, = struct.unpack_from("<H", data, coff + 2)
opt_size, = struct.unpack_from("<H", data, coff + 16)
opt = coff + 20
image_base, = struct.unpack_from("<I", data, opt + 28)

sections = []
for i in range(nsec):
    o = opt + opt_size + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode("latin1")
    vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
    sections.append((name, vaddr, vsize, rawptr, rawsize))

text = [s for s in sections if s[0] == ".text"][0]
tlo, thi = image_base + text[1], image_base + text[1] + text[2]


def va_to_off(va):
    for _, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= va - image_base < vaddr + vsize:
            off = rawptr + (va - image_base - vaddr)
            return off if off < len(data) else None
    return None


def is_entry(va):
    """True if va looks like a function entry point."""
    if not (tlo <= va < thi):
        return False
    off = va_to_off(va)
    if off is None:
        return False
    return data[off:off + 5] == HOTPATCH or data[off:off + 3] == FRAME


candidates = []
for name, vaddr, vsize, rawptr, rawsize in sections:
    if rawsize == 0:
        continue
    blob = data[rawptr:rawptr + rawsize]
    n = len(blob) // 4
    vals = struct.unpack_from("<%dI" % n, blob, 0)
    run = 0
    for i in range(n):
        if is_entry(vals[i]):
            run += 1
        else:
            if run >= MIN_SLOTS:
                candidates.append((name, image_base + vaddr + (i - run) * 4,
                                   vals[i - run:i]))
            run = 0
    if run >= MIN_SLOTS:
        candidates.append((name, image_base + vaddr + (n - run) * 4, vals[n - run:n]))

print("vtable candidates (>= %d consecutive function entries): %d\n"
      % (MIN_SLOTS, len(candidates)))
for name, va, slots in candidates:
    print("=== %s @ 0x%08x (%d slots) ===" % (name, va, len(slots)))
    for i, s in enumerate(slots[:17]):
        tag = {10: "  <-- CheckDeviceFormat?", 14: "  <-- GetDeviceCaps?"}.get(i, "")
        print("   [%2d] 0x%08x%s" % (i, s, tag))
    print()
