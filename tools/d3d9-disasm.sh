#!/bin/bash
# d3d9-disasm.sh : disassemble the guest d3d9.dll and print a region.
#
#   tools/extract-guest-d3d9.sh      once, to pull the DLL out of the image
#   wsl bash tools/d3d9-disasm.sh <hex-address> [instruction-count]
#
# Addresses are virtual, image base 0x4fdd0000. Run under WSL: the disassembly
# lives in WSL's /tmp, which is NOT the same directory as git-bash's /tmp.
ASM=/tmp/d3d9.asm
DLL=/tmp/d3d9.dll

if [ ! -f "$DLL" ]; then
  echo "missing $DLL - run tools/extract-guest-d3d9.sh first" >&2
  exit 2
fi
if [ ! -f "$ASM" ]; then
  echo "=== disassembling (about 576k lines) ===" >&2
  objdump -d -M intel "$DLL" > "$ASM" 2>/dev/null
fi

ADDR="$1"
COUNT="${2:-30}"
if [ -z "$ADDR" ]; then echo "usage: d3d9-disasm.sh <hex-address> [count]" >&2; exit 2; fi

N=$(grep -n "^${ADDR}:" "$ASM" | head -1 | cut -d: -f1)
if [ -z "$N" ]; then
  echo "address ${ADDR} is not an instruction boundary" >&2
  exit 1
fi
sed -n "${N},$((N+COUNT))p" "$ASM" | sed 's/\t/ /g' | cut -c1-105
