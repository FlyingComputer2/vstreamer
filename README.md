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
    └── test_app/       # one subdirectory per app target (stream_sdl/, rs_fec_test/, …)
```

## Build

Reproducible bench configs (see `aidocs/implementation-plan.md` §1.1):

```bash
cmake -S . -B out/full  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DENABLE_H264_ENCODER_MPP=ON -DENABLE_SDL_SINK=ON \
      -DENABLE_TEST_STREAM_SDL=ON \
      -DVSTREAMER_BUILD_TESTS=ON
cmake -S . -B out/rover -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_CEDAR=ON
cmake -S . -B out/intel -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_INTEL=ON
cmake -S . -B out/gs    -DENABLE_NOISE_SOURCE=OFF -DENABLE_V4L2_SOURCE=OFF \
      -DENABLE_JPEG_DECODER_MULTICORE=OFF
cmake --build out/full -j$(nproc)
ctest --test-dir out/full --output-on-failure
```

CMake options include `ENABLE_*` component toggles, `VSTREAMER_BUILD_TESTS`, and test-app
target `ENABLE_TEST_STREAM_SDL` (loopback bench; UVC+kmsdrm: `stream_sdl --display kmsdrm --source /dev/video0`).
MPP encoder/decoder are
auto-enabled on Rockchip when headers are found. Full table: [docs/vstreamer.md](docs/vstreamer.md).
