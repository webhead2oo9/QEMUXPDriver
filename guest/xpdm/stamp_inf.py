#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stamp the staged XPDM INF with the repository's authoritative DriverVer."""

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: stamp_inf.py STAGED_INF")
    path = Path(sys.argv[1])
    driver_ver = subprocess.check_output(
        [sys.executable, str(ROOT / "tools" / "version.py"), "--driverver"],
        text=True,
    ).strip()
    original = path.read_bytes()
    stamped, count = re.subn(
        rb"(?im)^[ \t]*DriverVer[ \t]*=[^\r\n]*",
        b"DriverVer = " + driver_ver.encode("ascii"),
        original,
    )
    if count != 1:
        raise SystemExit("staged INF must contain exactly one DriverVer line")
    if stamped != original:
        path.write_bytes(stamped)


if __name__ == "__main__":
    main()
