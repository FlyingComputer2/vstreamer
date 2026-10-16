# ESP32-P4 vstreamer IDF example

Smoke tests and **live camera TX** over Ethernet (no winject/radio).

**Work log (1080p30 bench, coherency, branches):** [docs/esp32p4-1080p30-live-tx.md](docs/esp32p4-1080p30-live-tx.md)

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
than from internal RAM. The IDF patch uses **PSRAM invalidate** (not C2M writeback) in
`esp_h264_enc_process` immediately before the H.264 DMA read; internal RAM still writebacks.
`vs_isp_enc` pulls YUV directly from `esp32p4_csi_camera_src` (no intermediate raw queue).
PSRAM **M2C invalidate runs at V4L2 DQBUF** so it can overlap `esp_h264` on the prior frame;
`esp_h264_enc_process` skips PSRAM cache ops (`esp_h264_psram_dqbuf_coherency.patch`). Optional
`esp_h264_db_bypass.patch` disables the HW deblocking filter for a small encode-time win.

**Target for true 1080p30 TX:** UART `encode_ms` ≤ **33 ms** steady and `live TX` ≥ **30 fps**.

With `h264_isp_src` + PSRAM invalidate patch (see `apply-patches.sh`), bench UART typically
shows **~28.8–29.0 fps** TX, **`encode_ms` ~33.3–33.5** (`avg`), **`max` ~34.5** (IDR),
**cam `dropped=0`**, **`dq_ms` ~34.7** (DQBUF interval), **`wall_ms` ~34.7**,
**`push_ms` ≈ `encode_ms`**, **`wait_ms` ~1.1**. Synthetic heap PSRAM YUV at boot is often **~32.9 ms** avg (under budget); a one-shot
**CSI mmap** encode bench measured **~33.3 ms** avg (same as live) — the gap vs synthetic
is scene/complexity, not mmap vs heap alone.
Sustained **30 fps** needs end-to-end **`wall_ms` ≤ 33.33 ms** (UART `dq_ms` tracks this).
Live runs show **`encode_ms` ~33.3** plus **`wait_ms` ~1.2** and scheduling overhead →
**`wall_ms` ~34.7** (~**28.9 fps**) even when synthetic PSRAM heap encode is **~32.9 ms**.
QP/bitrate, HW deblock bypass, MB-decimate score, ISP-pipeline off, and a 2-deep camera
`ready_q_` did not close that gap on bench. Remaining levers: **~1 ms+ faster live
`esp_h264_enc_process`** (Espressif CSI→H.264 / coherent or M2M path) or lower non-HW
pipeline overhead; internal 1080p YUV alloc still fails on this board’s heap layout.

**GS check (direct LAN to `192.168.253.106:21090`):** `printf 'metrics\n' | nc -u -w1
127.0.0.1 5091` on the receiver host — `h264_decoder.depay_au_pps` has measured **~30** while
UART `live TX` fps stays **~28.8** (cumulative AU counter vs receiver’s short-window rate).
`peer{rcv=0}` on UART is expected when `telemetry=off`. With `telemetry=on`,
bench has shown `peer{rcv=100 lost=0}` to GS on direct LAN while UART `live TX`
stays ~**28.9 fps** (encode-limited, not wire loss). High `rtp_h264_depay.loss` on a busy
bench may still need receiver tuning; `render_fps=0` is a separate SDL/KMS issue.

`esp32_h264_isp_src::set_live_tx_hooks()` can run pay/send on the encode thread (legacy style);
bench showed it **hurts** throughput vs a separate `vs_isp_tx` thread — keep the default split.

`vs_cap_pull` must **not** poll `output()` in a tight loop: use `esp32p4_csi_camera_src::wait_output()`
so the thread sleeps on `out_cv_` until `vs_csi_cam` delivers a frame (replaces ~1 kHz
`vTaskDelay(1)` polling and avoids `taskYIELD()` busy-wait).
