# Direct ISP source contract checks

Build and run on a host, without an ESP32-P4:

```sh
cmake -S tests/direct_isp -B /tmp/direct-isp-tests \
  -DBFC_INCLUDE_DIR="$PWD/build/_deps/bfc-src/src"
cmake --build /tmp/direct-isp-tests
ctest --test-dir /tmp/direct-isp-tests --output-on-failure
```

Use another BFC checkout's `src` directory if the main build has not fetched it.
The executable compiles the real `esp32_h264_isp_ng_src`, existing camera node, component
factory, caps, and wakeup code. Deterministic device/encoder doubles verify buffer address identity, delayed
buffer return while encoding, raw-frame drops under backpressure, configuration
routing, close during encoding, reopening, and error recovery/propagation. Encoder
doubles hold the production encode mutex across a paused encode; concurrent stats,
forwarded queries, bitrate changes, and stats resets must leave empty `output()`
reads free to return `-EAGAIN` before encoding resumes. It also
checks that the existing camera node retains a buffer until the last shared PDU
reference is released.

These checks do not emulate DMA, the camera driver, sensor timing, or esp_h264.
Cross-compile the production sources with ESP-IDF, then run live TX on hardware to
check frame integrity, capture/encode rate, latency, and shutdown. `buffers` controls
the driver ring, which still buffers completed frames even without a software raw
queue. Compare with the explicit `esp32_h264_isp_src` factory name if needed.
