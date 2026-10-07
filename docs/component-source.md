# Component source

Sources implement `component_source` → `component_output`:

```cpp
virtual int output(component_pdu &out) = 0;
```

`output()` returns `0` with a filled PDU, or `-EAGAIN` when no sample is ready.

## Port caps

Override `output_ports()` to list accepted `sdu_type_e` values per port (usually port `0`).

## Built-in sources

| Component | Typical output SDU | Notes |
|-----------|-------------------|--------|
| `v4l2_source` | `CAPS_VIDEO_RAW`, `NV12` / `MJPEG` | UVC capture |
| `noise_source` | `CAPS_VIDEO_RAW`, `NV12` | Test pattern |
| `stream_receiver` | `STREAM_DGRAM` | UDP ingress + FEC; one datagram per PDU |
