# vstreamer

Composable video pipeline framework (sources, codecs, sinks). See [docs/vstreamer.md](docs/vstreamer.md) for architecture and plugin contracts.

## Repository layout

```
vstreamer/
├── CMakeLists.txt
├── docs/
├── scripts/
└── src/
    ├── components/   # plugins (V4L2, stream_*, rtp pay/depay, MKV, …)
    ├── core/         # packets, frames, factory, component interfaces
    ├── apps/           # uvc_stream_sender, sdl_stream_receiver, stream_sdl_test, apps_common
    └── test_app/       # rs_fec_test/, noise_fft_bench/, …
```

## Build

Reference configurations (Rockchip bench host; Cedar and Intel encoders build through libav):

```bash
cmake -S . -B out/full  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DENABLE_H264_ENCODER_MPP=ON -DENABLE_SDL_SINK=ON \
      -DENABLE_TEST_STREAM_SDL=ON -DVSTREAMER_BUILD_TESTS=ON
cmake -S . -B out/rover -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_CEDAR=ON
cmake -S . -B out/intel -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_INTEL=ON
cmake -S . -B out/gs    -DENABLE_NOISE_SOURCE=OFF -DENABLE_V4L2_SOURCE=OFF \
      -DENABLE_JPEG_DECODER_MULTICORE=OFF
cmake --build out/full -j$(nproc)
ctest --test-dir out/full --output-on-failure            # add -L hw for MPP hardware tests
```

Each component has an `ENABLE_*` option. The MPP decoder defaults ON and the MPP encoder OFF;
either is switched OFF with a warning when `rockchip_mpp` is not found. `VSTREAMER_BUILD_TESTS`
builds the GoogleTest suite; `ENABLE_TEST_STREAM_SDL` builds the loopback bench
(`stream_sdl_test --display kmsdrm --source /dev/video0` for UVC on DRM/KMS).
Full option table and component keys: [docs/vstreamer.md](docs/vstreamer.md#build).

### Split sender / receiver (one host)

With `out/full` (TX + RX components, `ENABLE_APP_UVC_STREAM_SENDER` / `ENABLE_APP_SDL_STREAM_RECEIVER` default ON):

```bash
# Terminal 1 — receive on UDP :5001, console :5091
out/full/src/apps/sdl_stream_receiver/sdl_stream_receiver --listen 0.0.0.0:5001

# Terminal 2 — send to peer, console :5090 (noise fallback if camera missing)
out/full/src/apps/uvc_stream_sender/uvc_stream_sender --peer 127.0.0.1:5001 --device /dev/video0
```

`stream_sdl_test` is the single-process loopback bench with channel emulator and self-test
(metric names remain `stream_sdl.*` for console compatibility).
