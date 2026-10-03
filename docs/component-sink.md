# Sink

A sink consumes packets; it has no output pad. Header:
[component_sink.hpp](../src/core/component_sink.hpp). Keys of each implementation:
[vstreamer.md § Keys](vstreamer.md#keys).

```cpp
class component_sink : public component, public component_input
{
public:
    virtual std::string   name() const = 0;
    virtual media_kind_e  input_kind() const = 0;
    virtual uint8_t       input_pad_count() const;               // 1
    virtual packet_kind_e input_packet_kind(uint8_t port) const;  // FRAME on port 0

    virtual int  open() = 0;
    virtual void close() = 0;

    /* `in` is immutable; keep a reference (not a copy) if needed after return. */
    virtual int input(uint8_t port, const data_packet &in) = 0;

    /* Send gate. on=false: off. timeout_ms <= 0: on, no deadline.
     * > 0: on until now + timeout (renew by calling again).
     * Default implementation: always enabled, returns 0. */
    virtual int  set_enabled(bool on, int timeout_ms);
    virtual bool enabled() const;
};
```

Rules:

- `input` runs on the producer's thread and must not block for more than a short enqueue.
- A packet whose kind does not match `input_packet_kind()` / `input_kind()` → `-EINVAL`.
- A gated-off sink returns 0 from `input` and drops the packet.
- Unknown key → `-ENOTSUP`, bad value → `-EINVAL`.

## Implementations

| Class | Input | Gate | Notes |
|-------|-------|------|-------|
| `stream_sender` | `SOCK` | yes, **off after construction** — call `set_enabled(true, …)` | UDP egress, RS FEC, pacing, bounded queue, send thread |
| `mkv_sink` | `MJPEG` frames | no | Matroska record; `output` = path, empty = stop |
| `sdl_sink` | `NV12` frames | no | preview window; `video_driver` = `auto` / `kmsdrm` / SDL driver name |

`stream_sender` keeps its socket open while gated off. The gate is the hook for the planned
operator `stream_request` deadman ([vstreamer.md § Roadmap](vstreamer.md#roadmap--target-design)).

`sdl_sink` creates its window and GL context on the thread that calls `open()` and must present
from that same thread; `stream_sdl` therefore opens it on its present thread (except kmsdrm,
which it opens up front).
