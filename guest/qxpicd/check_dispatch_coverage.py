#!/usr/bin/env python3
"""Report QXPICD's non-no-op OpenGL 1.1 dispatch-table coverage."""

import argparse
import re
from pathlib import Path

from generate_scalar_stubs import GLFUNCS, balanced_calls, split_top


HERE = Path(__file__).resolve().parent
SOURCES = (HERE / "generated_gl_scalar.c", HERE / "qxpicd.c")
INSTALL = re.compile(r"entries\[(\d+)\]\s*=\s*\(PROC\)")


def all_entries():
    text = GLFUNCS.read_text(encoding="utf-8")
    entries = {}
    for macro, fields_expected in (("USE_GL_FUNC", 5),
                                   ("USE_GL_FUNC_RET", 6)):
        for call in balanced_calls(text, macro):
            fields = split_top(call)
            if len(fields) != fields_expected:
                continue
            try:
                offset = int(fields[-2])
            except ValueError:  # macro definitions in the header preamble
                continue
            entries[offset] = fields[0]
    return entries


def implemented_offsets():
    result = set()
    for path in SOURCES:
        for match in INSTALL.finditer(path.read_text(encoding="utf-8")):
            result.add(int(match.group(1)))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minimum", type=int, default=0)
    args = parser.parse_args()
    entries = all_entries()
    implemented = implemented_offsets()
    missing = [(offset, entries[offset]) for offset in sorted(entries)
               if offset not in implemented]
    percent = 100.0 * len(implemented) / len(entries)
    print(f"implemented {len(implemented)}/{len(entries)} ({percent:.1f}%)")
    print(f"remaining {len(missing)}: " +
          ", ".join(f"{offset}:{name}" for offset, name in missing))
    if len(implemented) < args.minimum:
        raise SystemExit(
            f"coverage regression: {len(implemented)} < {args.minimum}")


if __name__ == "__main__":
    main()
