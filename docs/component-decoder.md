# Component decoder (coder)

Decoders implement `component_coder`:

```cpp
virtual int input(component_pdu &&in) = 0;
virtual int output(component_pdu &out) = 0;
```

## Caps and media

For H.264, send `CAPS_VIDEO_CODED` before each `H264_AU` (or rely on in-band SPS when the
decoder accepts it). Decoders emit `CAPS_VIDEO_RAW` when the raster size changes, then `NV12`
frames. `ts_us` on coded input is carried through to raw output when possible; `-EAGAIN` when no
frame is ready.

## Built-in decoders

| Component | In | Out |
|-----------|----|-----|
| `h264_decoder_mpp` | `H264_AU` | `NV12` |
| `h264_decoder_intel` | `H264_AU` | `NV12` |
| `jpeg_decoder` | `MJPEG` | `NV12` |
