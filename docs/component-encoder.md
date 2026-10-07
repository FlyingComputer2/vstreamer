# Component encoder (coder)

Encoders implement `component_coder` (`component_input` + `component_output`):

```cpp
virtual int input(component_pdu &&in) = 0;
virtual int output(component_pdu &out) = 0;
```

## Caps and media

Send `CAPS_VIDEO_RAW` (width, height, stride, fps) before the first `NV12` frame. Encoders emit
`CAPS_VIDEO_CODED` when parameters change, then `H264_AU` (or `MJPEG`) access units. `ts_us` on
input frames is propagated to coded output where the backend supports it; `-EAGAIN` on `output`
when no AU is ready.

## Built-in encoders

| Component | In | Out |
|-----------|----|-----|
| `h264_encoder_mpp` | `NV12` | `H264_AU` |
| `h264_encoder_intel` | `NV12` | `H264_AU` |
| `h264_encoder_cedar` | `NV12` | `H264_AU` |
| `jpeg_encoder` | `NV12` | `MJPEG` |
