# Source

A source produces packets; it has no input pad. Header:
[component_source.hpp](../src/core/component_source.hpp). Keys of each implementation:
[vstreamer.md § Keys](vstreamer.md#keys).

```cpp
class component_source : public component, public component_output
{
public:
    virtual std::string  name() const = 0;
    virtual media_kind_e output_kind() const = 0;
    virtual uint8_t       output_pad_count() const;             // 1
    virtual packet_kind_e output_packet_kind(uint8_t port) const;  // FRAME on port 0

    virtual int  open() = 0;
    virtual void close() = 0;

    /* timeout_ms < 0 blocks, 0 polls, > 0 waits.
     * 0 = `out` filled; -EAGAIN = nothing this wait; other negative = error. */
    virtual int output(uint8_t port, data_packet &out, int timeout_ms) = 0;

    /* from component */
    virtual int configure(std::string_view key, std::string_view value) = 0;
    virtual int query(std::string_view key, std::string *value) const = 0;
};
```

Rules:

- `configure` before `open`. Keys that cannot change while open return `-EBUSY`; others apply
  live or on the next frame. To apply a device or size change, `close` then `open`.
  `close` is idempotent.
- `output` runs on the app's source thread; `configure` / `query` may run concurrently on another
  thread (the implementation locks).
- The returned packet owns its bytes (never a pointer into driver memory: mmap capture copies
  before `QBUF`) and is immutable from then on.
- Unknown key → `-ENOTSUP`, bad value → `-EINVAL`.

## Implementations

| Class | `output_kind` | Notes |
|-------|---------------|-------|
| `v4l2_source` | `MJPEG` | UVC mmap capture; live V4L2 controls via `ctrl.<id>` / `v4l2-ctl/<name>` (stashed if set before `open`) |
| `noise_source` | `NV12` | synthetic snow, bandwidth-shaped IFFT; optional pregenerated loop |
| `stream_receiver` | — (`SOCK`) | UDP ingress + FEC; one `sock_data` per original datagram |

`v4l2_source` reports `state` = `capturing` / `waiting_device` and does not fall back to noise
itself. Camera-with-noise-fallback is pipeline-creator policy: `stream_sdl` wraps both in
`camera_noise_mux_source`, which switches to noise on `-ENODEV` and back when capture returns.
Use metric **`source.state`** for source status (not `stream_sdl.status`, which is always
`running`). `noise_source` reports `state` = `running` or `pregeneration_i/N` while building its
pregenerated pool.

A source does not encode, packetize for the air, or talk to the radio.
