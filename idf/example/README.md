# ESP32-P4 vstreamer IDF example

Smoke tests and **live camera TX** over Ethernet (no winject/radio).

**Work log (1080p30 bench):** [docs/esp32p4-1080p30-live-tx.md](docs/esp32p4-1080p30-live-tx.md)

## P4 (Waveshare ESP32-P4-WIFI6-DEV-KIT + OV5647)

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd idf/example
idf.py set-target esp32p4 build flash monitor
```

That build runs the smoke tests (loopback pipeline, encoder, camera → encoder). For live TX,
layer `sdkconfig.live_tx` on top (delete `sdkconfig` first when switching):

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.live_tx" build flash monitor
```

With `CONFIG_VSTREAMER_IDF_LIVE_TX=y`:

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

UART `live TX` lines report the camera (`cam frames`, `dropped`, `dq_ms` = DQBUF interval) and
the encoder (`encode_ms`, `wall_ms`, `wait_ms`). Healthy 1080p30 on the bench:

- `dq_ms` 33.30 (30.0 fps): `esp32p4_csi_camera_src` rewrites the OV5647 VTS for a 24 MHz
  xclk; the stock table runs at 28.8 fps (`dq_ms` 34.7).
- `encode_ms` ~33.3 avg: esp_h264's HW time at 1080p, right at the 33.33 ms budget, so ~0.5–1%
  of camera frames drop and TX reports ~29.7 fps.
- Bitrate tracks `bitrate` (CBR; QP 10–51 are only rate-control clamps).

Unmodified `espressif/esp_h264`; see the work log for what did and did not help.
