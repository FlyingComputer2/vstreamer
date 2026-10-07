# Component sink

Sinks implement `component_sink` → `component_input`:

```cpp
virtual int input(component_pdu &&in) = 0;
virtual int set_enabled(bool on, int timeout_ms) = 0;
```

Send a matching **caps** PDU before the first media PDU when the sink requires geometry or
codec parameters (`CAPS_VIDEO_RAW`, `CAPS_VIDEO_CODED`, …).

## Errors

- Wrong `sdu_type` or port → `-EINVAL`.
- Queue full (where applicable) → `-EAGAIN`.

## Built-in sinks

| Component | Input SDUs | Notes |
|-----------|------------|--------|
| `sdl_sink` | `CAPS_VIDEO_RAW`, `NV12` | Present NV12 |
| `mkv_sink` | `CAPS_VIDEO_CODED`, `MJPEG` | File recording |
| `stream_sender` | `STREAM_DGRAM` | UDP egress + FEC |
