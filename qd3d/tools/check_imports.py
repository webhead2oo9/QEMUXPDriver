#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""List PE images' imports and fail on anything a Windows XP SP3 x86 system
cannot satisfy.

Usage: check_imports.py [--objdump OBJDUMP] [--exports LIST] IMAGE...

LIST (default: xp_sp3_exports.txt beside this script) holds every named
export of the XP SP3 system DLLs a guest binary may use, generated from the
DLLs themselves by gen_xp_exports.py.  An import passes only if its DLL is
on the list and that DLL exports the function by name on XP SP3; imports by
ordinal and imports from any other DLL fail.
"""
import argparse
import os
import re
import subprocess
import sys


def read_imports(objdump, path):
    out = subprocess.run([objdump, "-p", path], check=True, capture_output=True, text=True).stdout
    imports = []
    dll = None
    in_imports = False
    for line in out.splitlines():
        if line.startswith("The Import Tables"):
            in_imports = True
            continue
        if line.startswith("The Export Tables") or line.startswith("PE File Base Relocations"):
            in_imports = False
        if not in_imports:
            continue
        m = re.match(r"\s+DLL Name:\s+(\S+)", line)
        if m:
            dll = m.group(1).lower()
            continue
        m = re.match(r"\s+[0-9a-f]{8}\s+(<none>|\d+)\s+([0-9a-f]{4})\s+(\S+)", line)
        if m and dll:
            imports.append((dll, m.group(3)))
            continue
        m = re.match(r"\s+[0-9a-f]{8}\s+(\d+)\s*$", line)
        if m and dll:
            imports.append((dll, "#" + m.group(1)))
    return imports


def read_exports(path):
    exported = set()
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2:
                sys.exit(f"{path}:{n}: expected 'dll function'")
            exported.add((parts[0].lower(), parts[1]))
    return exported


def check(objdump, image, exported, dlls):
    imports = read_imports(objdump, image)
    if not imports:
        print(f"{image}: no imports parsed", file=sys.stderr)
        return False
    bad = 0
    current = None
    for dll, func in imports:
        if dll != current:
            print(dll)
            current = dll
        if dll not in dlls:
            status = "DLL NOT ON THE XP SP3 LIST"
        elif (dll, func) not in exported:
            status = "NOT EXPORTED BY XP SP3"
        else:
            status = "ok"
        print(f"    {func:32s} {status}")
        bad += status != "ok"
    if bad:
        print(f"check-imports: {image}: FAIL ({bad} imports XP SP3 cannot satisfy)", file=sys.stderr)
        return False
    print(f"check-imports: OK ({len(imports)} imports from {len({d for d, _ in imports})} DLLs)")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="i686-w64-mingw32-objdump")
    ap.add_argument("--exports", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "xp_sp3_exports.txt"))
    ap.add_argument("image", nargs="+")
    args = ap.parse_args()

    exported = read_exports(args.exports)
    dlls = {d for d, _ in exported}
    ok = True
    for image in args.image:
        ok = check(args.objdump, image, exported, dlls) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
