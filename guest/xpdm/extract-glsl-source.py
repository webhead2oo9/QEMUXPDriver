#!/usr/bin/env python3
"""Extract and bound-check a miniport production shader C string."""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path


DEFAULT_SYMBOL = "qxp_d3d_scalable_point_program"
MAX_INTERNAL_SOURCE_BYTES = 16384
MAX_ARBVP_SOURCE_BYTES = 128 * 1024


def main() -> int:
    if len(sys.argv) not in (3, 4):
        raise SystemExit(
            "usage: extract-glsl-source.py qxpmp.c output [symbol]"
        )
    source_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    symbol = sys.argv[3] if len(sys.argv) == 4 else DEFAULT_SYMBOL
    text = source_path.read_text(encoding="utf-8")
    match = re.search(
        rf"static\s+const\s+UCHAR\s+{re.escape(symbol)}\[\]\s*=\s*"
        r"(?P<body>(?:\s*\"(?:\\.|[^\"\\])*\")+)\s*;",
        text,
        re.DOTALL,
    )
    if not match:
        raise SystemExit(f"could not find a single literal definition of {symbol}")
    literals = re.findall(r'\"(?:\\.|[^\"\\])*\"', match.group("body"))
    if not literals:
        raise SystemExit(f"{symbol} has no string literals")
    decoded = "".join(ast.literal_eval(literal) for literal in literals)
    encoded = decoded.encode("ascii")
    glsl = symbol == DEFAULT_SYMBOL
    if glsl:
        if not decoded.startswith("#version 110\n") or not decoded.endswith("}\n"):
            raise SystemExit(f"{symbol} is not a complete GLSL 1.10 source")
        source_limit = MAX_INTERNAL_SOURCE_BYTES
    else:
        if not decoded.startswith("!!ARBvp1.0\n") or not decoded.endswith("END\n"):
            raise SystemExit(f"{symbol} is not a complete ARBvp1.0 source")
        source_limit = MAX_ARBVP_SOURCE_BYTES
    source_payload_bytes = len(encoded) + 1
    fifo_bytes = (source_payload_bytes + 7) & ~7
    fifo_bytes += 8
    if source_payload_bytes > source_limit:
        raise SystemExit(
            f"{symbol} needs {source_payload_bytes} source payload bytes; "
            f"limit is {source_limit}"
        )
    output_path.write_bytes(encoded)
    print(
        f"{symbol}: {len(encoded)} source bytes, "
        f"{source_payload_bytes}/{source_limit} source payload bytes, "
        f"{fifo_bytes} FIFO bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
