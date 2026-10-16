#!/usr/bin/env bash
# Apply local patches to managed_components (not tracked in git).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
H264_SRC="$ROOT/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c"
PATCH="$ROOT/patches/esp_h264_psram_invalidate.patch"
CACHE_PATCH="$ROOT/patches/esp_h264_cache_aligned_invalidate.patch"
CACHE_SRC="$ROOT/managed_components/espressif__esp_h264/port/src/esp_h264_cache.c"
if [[ ! -f "$H264_SRC" ]]; then
  echo "apply-patches: run idf.py build once so managed_components exist" >&2
  exit 1
fi
if grep -q 'esp_h264_cache_check_and_invalidate' "$H264_SRC"; then
  echo "apply-patches: esp_h264 PSRAM coherency patch already applied"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$PATCH"
  echo "apply-patches: applied esp_h264 PSRAM coherency patch"
fi
if grep -q 'align = 64U' "$CACHE_SRC"; then
  echo "apply-patches: esp_h264 aligned invalidate patch already applied"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$CACHE_PATCH"
  echo "apply-patches: applied esp_h264 aligned invalidate patch"
fi
