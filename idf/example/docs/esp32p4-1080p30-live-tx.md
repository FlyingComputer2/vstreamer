# ESP32-P4 live TX — 1080p30 work log

**Status (2026-10-16):** camera at 30.0 fps; live TX ~29.7 fps at 1080p, 4 Mbps CBR. The
remaining gap is esp_h264's hardware encode time (~33.3 ms/frame against a 33.33 ms budget),
which drops ~0.5–1% of camera frames.

## Bench

- Waveshare ESP32-P4-WIFI6-DEV-KIT + OV5647 (MIPI-CSI, 24 MHz xclk), IDF 5.5.5, `/dev/ttyACM0`
- CPU 400 MHz, PSRAM 250 MHz
- Direct LAN UDP to the GS at `192.168.253.106:21090` (`CONFIG_VSTREAMER_RX_PEER`), `fec=none`
- Pipeline: `esp32_h264_isp_src` = `esp32p4_csi_camera_src` (`vs_csi_cam`, core 0) feeding
  `h264_encoder_esp32p4` (`vs_isp_enc`, core 1); main thread pays and sends.

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd idf/example
idf.py set-target esp32p4 build flash monitor
```

## Root cause of the 28.8 fps plateau: the sensor, not the encoder

esp_cam_sensor's `MIPI_2lane_24Minput_RAW10_1920x1080_30fps` table copies the Linux driver's
PLL (0x3035 = 0x21, 0x3036 = 0x62). Its 81.6667 MHz pixel clock assumes a 25 MHz xclk; at
24 MHz the clock is 78.4 MHz, and HTS 2271 × VTS 1199 gives 28.79 fps (34.73 ms). The bench
measured `dq_ms` 34.72 and `wait_ms` ~1.2 (the encoder waiting on the camera), with no drops.

`esp32p4_csi_camera_src` now recomputes VTS for the configured `fps` at 24 MHz (VTS 1150 →
30.02 fps) and loads the table through `VIDIOC_S_SENSOR_FMT`. Measured `dq_ms` 33.30.

## Measurements after the sensor fix (60–75 s runs)

| Change | encode_ms avg | TX fps | cam dropped |
|--------|---------------|--------|-------------|
| Sensor fix, earlier esp_h264 patches, QP 40–51 | 33.4 | 29.8 | 0.6% |
| Invalidate only coded bytes after encode | 33.3 | 29.9 | 0.2% |
| HW deblocking back on (no `db_bypass`) | 33.3 | 29.9 | 0% |
| QP 20–40 (RC capped: 14 Mbps instead of 4) | 33.8 | 28.9 | 3% |
| QP 25–51 | 33.3 | 29.7 | 0.5% |
| No cache invalidate at DQBUF | 33.3 | 29.7 | 0.5% |
| Unmodified esp_h264 (no patches) | 33.3 | 29.8 | 0.3% |
| + IDLE task WDT on, default 5 s timeout | 33.3 | 29.8 | 0.4% |
| + QP 10–51 (final) | 33.3 | 29.7 | 0.7% |

So none of the esp_h264 patches, the DQBUF cache sync, or the deblocking bypass changed
encode time, and they were removed. Encode time only moves with the bitstream size.

## Rate control

esp_h264 has one RC mode: with `qp_min < qp_max` it targets `bitrate` (CBR) and picks a QP
per frame inside the clamps; `qp_min == qp_max` is constant QP. Live TX uses 4 Mbps with QP
10–51, measured 4.05–4.3 Mbps.

## What did not help (earlier attempts)

- esp_h264 patches: skipping the PSRAM invalidate in `enc_process`, aligned invalidate,
  `db_bypass`, MB-decimate score 255
- Cache invalidate at DQBUF to overlap with the previous encode
- ISP pipeline controller off; swapping encode/capture cores
- 8 V4L2 buffers (`REQBUFS` fails at 1080p; 6 is the maximum)
- esp_video's V4L2 H.264 M2M device (same `esp_h264_enc_process()`)
- Copying the frame to internal RAM (a 1080p frame does not fit the largest internal block)

## Open

- HW encode headroom: 1080p30 sits at esp_h264's limit. Options: accept ~0.5–1% drops, crop
  to 1920×1056 (3% fewer macroblocks), or run 25 fps.
- The UART `enc frames` counter runs ahead of `cam frames` between samples; TX fps and
  `dq_ms` agree, so treat `enc` fps as unreliable until that is explained.
- GS: the long-running `sdl_stream_receiver` showed decoder `out_fps` 0 and ~17% depay loss
  while receiving this stream; not investigated.
