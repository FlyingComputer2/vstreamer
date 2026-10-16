#!/usr/bin/env bash
# Apply local patches to managed_components (not tracked in git).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
H264_SRC="$ROOT/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c"
PATCH="$ROOT/patches/esp_h264_psram_invalidate.patch"
if [[ ! -f "$H264_SRC" ]]; then
  echo "apply-patches: run idf.py build once so managed_components exist" >&2
  exit 1
fi
if grep -q 'PSRAM YUV from CSI mmap' "$H264_SRC"; then
  echo "apply-patches: esp_h264 PSRAM coherency patch already applied"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$PATCH"
  echo "apply-patches: applied esp_h264 PSRAM coherency patch"
fi
