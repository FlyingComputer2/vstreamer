# Component sink

Sinks implement `component_sink` → `component_input`:

```cpp
virtual int input(component_pdu &&in) = 0;
```

Send a matching **caps** PDU before the first media PDU when the sink requires geometry or
codec parameters (`CAPS_VIDEO_RAW`, `CAPS_VIDEO_CODED`, …).

The controller decides whether to deliver data to a sink; sinks process the data they receive.

## Errors

- Wrong `sdu_type` or port → `-EINVAL`.
- Queue full (where applicable) → `-EAGAIN`.

## Built-in sinks

| Component | Input SDUs | Notes |
|-----------|------------|--------|
| `sdl_sink` | `CAPS_VIDEO_RAW`, `NV12` | Present NV12 |
| `mkv_sink` | `CAPS_VIDEO_CODED`, `MJPEG`, `H264_AU` | File recording |
| `stream_sender` | `STREAM_DGRAM` | UDP egress + FEC |

For `sdl_sink`, send caps or configure `width` and `height` as fallback dimensions.
Configuration stores dimensions only; `input()` creates and uses SDL rendering resources on
the rendering thread. `query("render_thread")` returns that thread's ID, or an empty value when
unbound. `query("stats")` reports presented frames.
