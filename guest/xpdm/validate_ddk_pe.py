#!/usr/bin/env python3
"""Validate the ABI-relevant PE fields of the WDK XPDM probe."""

import struct
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"invalid XPDM probe: {message}")


allow_imports = False
kernel_driver = False
native_driver = False
arguments = sys.argv[1:]
while arguments and arguments[0].startswith("--"):
    option = arguments.pop(0)
    if option == "--allow-imports":
        allow_imports = True
    elif option == "--kernel":
        kernel_driver = True
        allow_imports = True
    elif option == "--native-driver":
        kernel_driver = True
        native_driver = True
        allow_imports = True
    else:
        raise SystemExit(f"unknown option: {option}")
if len(arguments) != 1:
    raise SystemExit(
        "usage: validate_ddk_pe.py [--allow-imports | --kernel | --native-driver] driver"
    )

path = Path(arguments[0])
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
if optional + optional_size > len(data) or optional_size < 0xE0:
    fail("truncated PE32 optional header")
if machine != 0x014C:
    fail(f"machine is 0x{machine:04x}, expected i386")
if kernel_driver:
    if characteristics & 0x0102 != 0x0102 or characteristics & 0x2000:
        fail("image is not a 32-bit kernel driver")
elif characteristics & 0x2100 != 0x2100:
    fail("image is not a 32-bit DLL")
if struct.unpack_from("<H", data, optional)[0] != 0x010B:
    fail("optional header is not PE32")

entry_point = struct.unpack_from("<I", data, optional + 16)[0]
subsystem_major, subsystem_minor = struct.unpack_from("<HH", data, optional + 48)
subsystem = struct.unpack_from("<H", data, optional + 68)[0]
directory_count = struct.unpack_from("<I", data, optional + 92)[0]
if entry_point == 0:
    fail("DrvEnableDriver image entry point is zero")
if subsystem != 1:
    fail(f"subsystem is {subsystem}, expected IMAGE_SUBSYSTEM_NATIVE")
if (subsystem_major, subsystem_minor) != (5, 1):
    fail(
        f"subsystem version is {subsystem_major}.{subsystem_minor}, expected XP 5.1"
    )
if directory_count < 2:
    fail("PE data-directory table is incomplete")

if native_driver:
    checksum_offset = optional + 64
    recorded_checksum = struct.unpack_from("<I", data, checksum_offset)[0]
    checksum = 0
    for index in range(0, len(data), 2):
        if checksum_offset <= index < checksum_offset + 4:
            continue
        word = data[index]
        if index + 1 < len(data):
            word |= data[index + 1] << 8
        checksum += word
        checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = ((checksum & 0xFFFF) + (checksum >> 16)) + len(data)
    if not recorded_checksum or recorded_checksum != checksum:
        fail(
            f"kernel checksum is 0x{recorded_checksum:x}, expected 0x{checksum:x}"
        )

export_rva, export_size, import_rva, import_size = struct.unpack_from(
    "<IIII", data, optional + 96
)
if export_rva or export_size:
    fail("GDI_DRIVER probe unexpectedly has an export directory")
if (import_rva or import_size) and not allow_imports:
    fail("probe has imports; the escape path must remain CRT-free")


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


import_names = []
if import_rva:
    descriptor = rva_offset(import_rva)
    while True:
        if descriptor + 20 > len(data):
            fail("truncated import table")
        fields = struct.unpack_from("<IIIII", data, descriptor)
        if not any(fields):
            break
        name_offset = rva_offset(fields[3])
        name_end = data.find(b"\0", name_offset)
        if name_end < 0:
            fail("unterminated import DLL name")
        import_names.append(data[name_offset:name_end].decode("ascii").upper())
        descriptor += 20
expected_imports = {"NTOSKRNL.EXE", "VIDEOPRT.SYS"} if kernel_driver else {
    "WIN32K.SYS"
}
if native_driver:
    expected_imports = {"NTOSKRNL.EXE", "HAL.DLL"}
if allow_imports and set(import_names) - expected_imports:
    fail(f"unexpected import DLLs: {', '.join(import_names)}")
if kernel_driver and not native_driver and "VIDEOPRT.SYS" not in import_names:
    fail("video miniport does not import VIDEOPRT.SYS")

import_description = ", ".join(import_names) if import_names else "no imports"
print(
    f"{path.name}: i386 PE32 native XP 5.1 "
    f"{'kernel driver' if native_driver else ('video miniport' if kernel_driver else 'GDI driver')}, "
    f"entry=0x{entry_point:x}, {import_description}, no exports"
)
