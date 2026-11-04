#!/usr/bin/env python3
"""Validate that the XP co-installer is a compatible PE32 DLL."""

import struct
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"invalid XPDM co-installer: {message}")


if len(sys.argv) != 2:
    raise SystemExit("usage: validate_coinstaller.py qxpcoin2.dll")

path = Path(sys.argv[1])
data = path.read_bytes()
if len(data) < 0x40 or data[:2] != b"MZ":
    fail("missing DOS/PE header")

pe = struct.unpack_from("<I", data, 0x3C)[0]
if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
    fail("missing PE signature")

machine, section_count, _, _, _, optional_size, characteristics = struct.unpack_from(
    "<HHIIIHH", data, pe + 4
)
optional = pe + 24
if machine != 0x014C or characteristics & 0x2100 != 0x2100:
    fail("image is not an i386 DLL")
if optional + optional_size > len(data) or optional_size < 0xE0:
    fail("truncated PE32 optional header")
if struct.unpack_from("<H", data, optional)[0] != 0x010B:
    fail("optional header is not PE32")
if struct.unpack_from("<HH", data, optional + 48) != (5, 1):
    fail("subsystem version is not XP 5.1")
if struct.unpack_from("<H", data, optional + 68)[0] != 2:
    fail("subsystem is not Windows GUI")


def rva_offset(rva: int) -> int:
    sections = optional + optional_size
    for index in range(section_count):
        section = sections + index * 40
        if section + 40 > len(data):
            fail("truncated section table")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, section + 8
        )
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            offset = raw_pointer + rva - virtual_address
            if offset >= len(data):
                fail("RVA points beyond the image")
            return offset
    fail(f"RVA 0x{rva:x} is not backed by a section")
    return 0


export_rva, _, import_rva, _ = struct.unpack_from("<IIII", data, optional + 96)
if not export_rva:
    fail("missing export directory")
export = rva_offset(export_rva)
fields = struct.unpack_from("<IIHHIIIIIII", data, export)
name_count, names_rva = fields[7], fields[9]
names_offset = rva_offset(names_rva)
exports = set()
for index in range(name_count):
    name_rva = struct.unpack_from("<I", data, names_offset + index * 4)[0]
    start = rva_offset(name_rva)
    end = data.find(b"\0", start)
    if end < 0:
        fail("unterminated export name")
    exports.add(data[start:end].decode("ascii"))
if exports != {"QxpCoInstaller"}:
    fail(f"unexpected exports: {', '.join(sorted(exports))}")

imports = set()
if import_rva:
    descriptor = rva_offset(import_rva)
    while True:
        fields = struct.unpack_from("<IIIII", data, descriptor)
        if not any(fields):
            break
        start = rva_offset(fields[3])
        end = data.find(b"\0", start)
        if end < 0:
            fail("unterminated import DLL name")
        imports.add(data[start:end].decode("ascii").upper())
        descriptor += 20
allowed = {"ADVAPI32.DLL", "KERNEL32.DLL", "MSVCRT.DLL"}
if imports - allowed:
    fail(f"unexpected import DLLs: {', '.join(sorted(imports - allowed))}")

print(
    f"{path.name}: i386 PE32 XP 5.1 co-installer, "
    "QxpCoInstaller export present"
)
