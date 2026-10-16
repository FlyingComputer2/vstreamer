#!/usr/bin/env bash
# OPI5 / GS: direct LAN receive from ESP32-P4 vstreamer-idf-example (no winject).
# Uses tty1 + KD_GRAPHICS like gs-video-receiver.service.
set -euo pipefail

BIN="${SDL_STREAM_RECEIVER:-/home/ubuntu/development/vstreamer/out/gs/src/apps/sdl_stream_receiver/sdl_stream_receiver}"
LISTEN="${VSTREAMER_P4_RX_LISTEN:-0.0.0.0:21090}"
UNIT="${VSTREAMER_P4_RX_UNIT:-vstreamer-p4-e2e}"

sudo systemctl stop "getty@tty1.service" 2>/dev/null || true
sudo systemctl stop "$UNIT.service" 2>/dev/null || true

sudo systemd-run --unit="$UNIT" --collect \
  -p Type=simple \
  -p User=ubuntu \
  -p SupplementaryGroups=video \
  -p SupplementaryGroups=render \
  -p TTYPath=/dev/tty1 \
  -p StandardInput=tty \
  -p TTYReset=yes \
  -p TTYVHangup=yes \
  -p Restart=always \
  -p RestartSec=2 \
  "$BIN" --listen "$LISTEN" --display kmsdrm

echo "started $UNIT on HDMI (tty1), listen $LISTEN"
echo "console: printf 'metrics\\n' | nc -u -w1 127.0.0.1 5091"
echo "stop: sudo systemctl stop $UNIT; sudo systemctl start getty@tty1"
