#!/usr/bin/env python3
"""Build the deterministic XP acceptance ISO without a Unix cdrtools install."""

import filecmp
import os
import re
import sys
import time
from pathlib import Path
from unittest.mock import patch


WORKSPACE = Path(__file__).resolve().parents[2]
FIXED_ISO_EPOCH = 946684800
sys.path.insert(0, str(WORKSPACE / ".tools" / "pycdlib"))

try:
    import pycdlib
except ImportError as error:
    raise SystemExit(
        "pycdlib 1.14.0 is required; run: "
        "python -m pip install --target .tools/pycdlib pycdlib==1.14.0"
    ) from error


def iso_component(name):
    component = re.sub(r"[^A-Z0-9_.]", "_", name.upper())
    if len(component) > 31 or not re.fullmatch(r"[A-Z0-9_.]+", component):
        raise SystemExit(f"unsupported ISO-9660 component: {name}")
    return component


def iso_path(relative, versioned=False):
    components = [iso_component(part) for part in relative.parts]
    result = "/" + "/".join(components)
    return result + (";1" if versioned else "")


def fixed_utc_localtime(value=None):
    """Make pycdlib's local ISO timestamps host-timezone independent."""
    return time.gmtime(FIXED_ISO_EPOCH if value is None else value)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_iso.py SOURCE_DIRECTORY OUTPUT_ISO")
    source = Path(sys.argv[1]).resolve()
    destination = Path(sys.argv[2]).resolve()
    if not source.is_dir():
        raise SystemExit(f"ISO source directory does not exist: {source}")

    directories = sorted(
        (path for path in source.rglob("*") if path.is_dir()),
        key=lambda path: (len(path.relative_to(source).parts),
                          path.relative_to(source).as_posix().lower()),
    )
    files = sorted(
        (path for path in source.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(source).as_posix().lower(),
    )
    image = pycdlib.PyCdlib()
    with patch("time.time", return_value=FIXED_ISO_EPOCH), \
            patch("time.localtime", side_effect=fixed_utc_localtime):
        image.new(interchange_level=3, joliet=3,
                  vol_ident="XPDRIVER_ACCEPT")
        try:
            for directory in directories:
                relative = directory.relative_to(source)
                image.add_directory(
                    iso_path=iso_path(relative),
                    joliet_path="/" + relative.as_posix(),
                )
            for path in files:
                relative = path.relative_to(source)
                image.add_file(
                    str(path),
                    iso_path=iso_path(relative, versioned=True),
                    joliet_path="/" + relative.as_posix(),
                )
            temporary = destination.with_suffix(destination.suffix + ".tmp")
            temporary.unlink(missing_ok=True)
            image.write(str(temporary))
            try:
                os.replace(temporary, destination)
            except PermissionError as error:
                # Windows denies replacement while retro_host has the ISO
                # mounted. Reusing it is safe only when the deterministic
                # image is byte-identical; changed contents remain a failure.
                if (not destination.is_file() or not filecmp.cmp(
                        temporary, destination, shallow=False)):
                    raise SystemExit(
                        f"cannot replace locked, changed ISO: {destination}"
                    ) from error
                temporary.unlink()
                print(f"{destination} (unchanged locked image reused)")
                return
        finally:
            image.close()
    print(destination)


if __name__ == "__main__":
    main()
