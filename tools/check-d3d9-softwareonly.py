#!/usr/bin/env python3
"""Check HKLM\\Software\\Microsoft\\Direct3D\\Drivers -> SoftwareOnly in an offline SOFTWARE hive.

d3d9.dll (XP SP3) reads this REG_DWORD in Direct3DCreate9. If it exists and is
nonzero, [pD3D9+0x4684] is set to 1, and GetDeviceCaps(D3DDEVTYPE_HAL) returns
D3DERR_NOTAVAILABLE (0x8876086a) regardless of the driver's DDI version.
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hive/WINDOWS/system32/config/software"
with open(path, "rb") as f:
    data = f.read()
print("hive bytes: %d" % len(data))

needle = b"SoftwareOnly"
hits = []
start = 0
while True:
    i = data.find(needle, start)
    if i < 0:
        break
    hits.append(i)
    start = i + 1
print("occurrences of %r: %d" % (needle.decode(), len(hits)))

if not hits:
    print("RESULT: value name absent from hive -> key does not exist -> flag stays 0")
    sys.exit(0)

for off in hits:
    vk = off - 20
    if vk < 0 or data[vk:vk + 2] != b"vk":
        print("  offset %d: not preceded by a vk record (incidental string)" % off)
        continue
    name_len, data_len, data_off, vtype, flags = struct.unpack_from("<HIIIH", data, vk + 2)
    inline = bool(data_len & 0x80000000)
    real_len = data_len & 0x7FFFFFFF
    if inline:
        value = data_off & 0xFFFFFFFF
    else:
        # hive bins start at 0x1000; cell data begins 4 bytes into the cell
        cell = 0x1000 + data_off + 4
        value = struct.unpack_from("<I", data, cell)[0] if real_len >= 4 else None
    print("  vk @0x%x name_len=%d type=%d inline=%s len=%d VALUE=%s"
          % (vk, name_len, vtype, inline, real_len,
             hex(value) if value is not None else "?"))
    print("  RESULT: SoftwareOnly = %s -> HAL %s"
          % (hex(value), "BLOCKED" if value else "allowed"))
