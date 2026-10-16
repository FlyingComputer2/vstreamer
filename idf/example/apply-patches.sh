#!/usr/bin/env bash
# Apply local patches to managed_components (not tracked in git).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
H264_SRC="$ROOT/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c"
COHERENCY_PATCH="$ROOT/patches/esp_h264_psram_dqbuf_coherency.patch"
LEGACY_COHERENCY_PATCH="$ROOT/patches/esp_h264_psram_invalidate.patch"
DB_BYPASS_PATCH="$ROOT/patches/esp_h264_db_bypass.patch"
CACHE_PATCH="$ROOT/patches/esp_h264_cache_aligned_invalidate.patch"
CACHE_SRC="$ROOT/managed_components/espressif__esp_h264/port/src/esp_h264_cache.c"
if [[ ! -f "$H264_SRC" ]]; then
  echo "apply-patches: run idf.py build once so managed_components exist" >&2
  exit 1
fi
if grep -q 'CSI DQBUF' "$H264_SRC"; then
  echo "apply-patches: esp_h264 PSRAM DQBUF coherency patch already applied"
elif grep -q 'esp_ptr_external_ram' "$H264_SRC"; then
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$COHERENCY_PATCH"
  echo "apply-patches: migrated esp_h264 to DQBUF-overlap PSRAM coherency"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$LEGACY_COHERENCY_PATCH" || true
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$COHERENCY_PATCH"
  echo "apply-patches: applied esp_h264 DQBUF-overlap PSRAM coherency patch"
fi
if grep -q 'db_bypass = 1' "$H264_SRC"; then
  echo "apply-patches: esp_h264 db_bypass patch already applied"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$DB_BYPASS_PATCH"
  echo "apply-patches: applied esp_h264 db_bypass patch"
fi
if grep -q 'align = 64U' "$CACHE_SRC"; then
  echo "apply-patches: esp_h264 aligned invalidate patch already applied"
else
  patch -p1 -d "$ROOT/managed_components/espressif__esp_h264" -i "$CACHE_PATCH"
  echo "apply-patches: applied esp_h264 aligned invalidate patch"
fi
