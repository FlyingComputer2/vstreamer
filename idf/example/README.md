# ESP32-P4 vstreamer IDF example

Smoke tests and **live camera TX** over Ethernet (no winject/radio).

## P4 (Waveshare ESP32-P4-WIFI6-DEV-KIT + OV5647)

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd idf/example
idf.py set-target esp32p4 build
./apply-patches.sh && idf.py build flash monitor
```

With `CONFIG_VSTREAMER_IDF_LIVE_TX=y` (default in `sdkconfig.defaults`):

- DHCP on Ethernet (IP101 PHY)
- `csi_camera` → `h264_encoder` → `rtp_h264_pay` → `stream_sender`
- Destination: `CONFIG_VSTREAMER_RX_PEER` (default `192.168.253.106:21090`)
- `fec=none` for direct LAN
- Streams until reset (`CONFIG_VSTREAMER_LIVE_TX_SECONDS=0`)

UART shows `P4_IP ip=…` and `live TX streaming … until reset`.

## GS / OPI5 receiver (direct UDP)

Build the GS binary once on the Pi:

```bash
cmake -S . -B out/gs -DENABLE_NOISE_SOURCE=OFF -DENABLE_V4L2_SOURCE=OFF \
  -DENABLE_JPEG_DECODER_MULTICORE=OFF -DENABLE_SDL_SINK=ON
cmake --build out/gs -j"$(nproc)"
```

Start HDMI receiver (stops getty on tty1):

```bash
./host/start-gs-receiver.sh
```

Check metrics:

```bash
printf 'metrics\n' | nc -u -w1 127.0.0.1 5091 | rg 'udp_packet|h264_decoder.out_fps|sdl_sink.render'
```

Expect non-zero `h264_decoder.in_packets` and `sdl_sink.render_fps` when video is healthy.

## 1080p30 expectations (P4)

The OV5647 mode is **1080p30** capture. UART `live TX` lines report:

- `cam frames` — sensor/ISP delivery rate (~30/s when healthy)
- `enc frames` / `encode_ms` — **esp_h264** HW time per frame

On ESP32-P4 @ 400 MHz, synthetic `O_UYY_E_VYY` encode (CPU-filled **internal** buffer) is
~**33.6 ms**/frame — just under the 33.3 ms budget for 30 fps. **Live CSI** mmap buffers live
in **SPIRAM** (`CSI_MEM_CAPS`); the same encoder reports ~**35.6–36 ms**/frame (~**26 fps**
TX) even with correct post-capture `esp_cache_msync` (M2C, cache-line aligned). The gap is
not fixed by skipping `esp_h264` input writeback alone — HW encode from SPIRAM YUV is slower
than from internal RAM. The IDF example patches `esp_h264` HW encode to **invalidate** (not
C2M writeback) PSRAM YUV produced by the ISP before the H.264 DMA read.

**Target for true 1080p30 TX:** UART `encode_ms` ≤ **33 ms** steady. Remaining work is
likely an **esp_video / esp_h264** path that feeds the H.264 DMA from internal or
driver-coherent buffers (e.g. V4L2 HW H.264 M2M, or Espressif-supported zero-copy CSI→enc).

`vs_cap_pull` must **not** poll `output()` in a tight loop: use `esp32p4_csi_camera_src::wait_output()`
so the thread sleeps on `out_cv_` until `vs_csi_cam` delivers a frame (replaces ~1 kHz
`vTaskDelay(1)` polling and avoids `taskYIELD()` busy-wait).
