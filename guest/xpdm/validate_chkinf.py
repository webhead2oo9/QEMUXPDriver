#!/usr/bin/env python3
"""Fail when the legacy WDK ChkINF HTML summary reports diagnostics."""

import html
import re
import sys
from pathlib import Path


if len(sys.argv) != 2:
    raise SystemExit("usage: validate_chkinf.py summary.htm")

path = Path(sys.argv[1])
plain = html.unescape(re.sub(r"<[^>]+>", " ", path.read_text(errors="replace")))
plain = re.sub(r"\s+", " ", plain)
errors = re.search(r"Errors\s*:\s*(\d+)", plain, re.I)
warnings = re.search(r"Warnings\s*:\s*(\d+)", plain, re.I)
if not errors or not warnings:
    raise SystemExit(f"could not parse ChkINF summary: {path}")
if int(errors.group(1)) or int(warnings.group(1)):
    raise SystemExit(
        f"ChkINF reported {errors.group(1)} error(s), "
        f"{warnings.group(1)} warning(s): {path}"
    )
print("ChkINF: 0 errors, 0 warnings")
