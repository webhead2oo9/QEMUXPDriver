#!/bin/bash
# extract-d3d9.sh : pull WINDOWS/system32/d3d9.dll out of the guest image.
# Read-only with respect to the image. No VM may be running against it.
set -e
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
IMG=qemu/winxp/xpdm-games-pop-257-20260719.qcow2
RAW=/tmp/xp.raw
PART=/tmp/xppart

if [ ! -f "$RAW" ]; then
  echo "=== converting to raw (follows backing chain, 12 GiB) ==="
  qemu-img convert -O raw "$IMG" "$RAW"
fi
ls -la "$RAW"

echo "=== partitions ==="
7z l "$RAW" 2>/dev/null | grep -Ei "ntfs|Name|----" | head -8

mkdir -p "$PART"
if [ ! -f /tmp/d3d9.dll ]; then
  echo "=== extracting the NTFS volume ==="
  7z x -y -o"$PART" "$RAW" >/dev/null 2>&1 || true
  ls -la "$PART" | head

  VOL=$(ls "$PART" | head -1)
  echo "=== extracting d3d9.dll from $VOL ==="
  7z e -y -o/tmp "$PART/$VOL" "WINDOWS/system32/d3d9.dll" -r >/dev/null 2>&1 || true
fi

ls -la /tmp/d3d9.dll
md5sum /tmp/d3d9.dll
