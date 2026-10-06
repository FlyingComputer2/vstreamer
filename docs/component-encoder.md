# Encoder

Encoders are `component_coder`s: push NV12 frames with `input`, pull H.264 access units with
`output`. Header: [component_coder.hpp](../src/core/component_coder.hpp). Keys:
[vstreamer.md § Keys](vstreamer.md#keys).

```cpp
class component_coder : public component, public component_input, public component_output
{
public:
    virtual std::string  name() const = 0;
    virtual media_kind_e input_kind() const = 0;    // NV12
    virtual media_kind_e output_kind() const = 0;   // H264
    virtual int  open() = 0;
    virtual void close() = 0;
    virtual int  input(uint8_t port, const data_packet &in) = 0;
    virtual int  output(uint8_t port, data_packet &out, int timeout_ms) = 0;
};
```

Rules:

- `input` takes one packed NV12 frame of the configured `size`; a wrong kind or size returns
  `-EINVAL` (never a crash), and `-EBADF` if the encoder is not open.
- `output` returns one Annex-B access unit per call (`frame_data`, `key` set on IDR) carrying the
  input's `pts` and `capture_mono_ns`; `-EAGAIN` when none is ready.
- `configure` may run on another thread while encoding. `size` / `fps` changes set a reopen
  request handled on the encode path; `qp`, `gop`, `cbr`, `rc` apply live on MPP. `idr` forces
  an IDR on the next frame.
- MPP encoders support `cancel_pending_io()` to wake blocked calls at shutdown.

## Implementations

| Class | Backend | Rate control | Notes |
|-------|---------|--------------|-------|
| `h264_encoder_mpp` | Rockchip MPP (RK3588) | `cbr` (default target 20 Mb/s) or `fixqp` | cumulative `latency_ms` query; super-frame ratios |
| `h264_encoder_cedar` | libavcodec `h264_cedrus` (`/dev/cedar_dev`) | fixed QP 2..47 | width multiple of 32; QP/GOP reopen (PPS written at open); does not pass `capture_mono_ns` through to AUs |
| `h264_encoder_intel` | libavcodec `h264_vaapi` | fixed QP 0..52 | `device` = VA render node; cumulative `latency_ms` query when input carries `capture_mono_ns` |

The bench app picks the encoder at compile time (`h264_encoder_t` in
`stream_sdl/encoder_types.hpp`: MPP, else Cedar, else Intel); the factory name `h264_encoder`
follows the same order.
