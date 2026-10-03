# Packet model

Pipeline wires carry `data_packet` (`core/data_packet.hpp`): owned
`packet_body` subclasses (`frame_data`, `audio_data`, `sock_data`).

Receiver RX/gap counters are not carried in `data_packet`; the bench app publishes them as
`stream_sender.peer_*` metrics in-process. See [pipeline-flow.md](pipeline-flow.md).

### RTP capture timestamp extension

`rtp_h264_pay` stamps a one-byte RFC 5285 extension (id **1**, 8 bytes) with capture time as
**nanoseconds since the packer epoch** (`steady_mono_ns()` at `open()`). Query
`capture_epoch_ns` on the payloader; configure the same value on `rtp_h264_depay` via
`capture_epoch_ns` so `frame_data.capture_mono_ns = epoch + rel` on output.

That absolute mapping is only meaningful **in-process** (e.g. `stream_sdl` wires pay → depay).
Cross-host receivers should treat `capture_mono_ns` as 0 unless a future side channel defines the
epoch.

## Forward datagram (after FEC + RTP)

Bench path: `stream_sender` prepends `stream_header_s` (2 B, `stream_sequence`) before each FEC
shard on the wire. Systematic shards add a 2 B big-endian payload length before the RTP datagram.

```text
| stream_header (2) | FEC shard hdr (4) | [len prefix (2)] | RTP hdr (12) | RTP ext (16) | H.264 |
```

| Region | Size | Notes |
|--------|------|--------|
| `stream_header_s` | 2 | `stream_sequence` (BE); gap detection on raw UDP |
| FEC shard header | 4 | `block_id`, `shard_index`, `k`, `n` (see `rs_block_erasure`) |
| Length prefix | 2 | Systematic shards only; BE byte count of following RTP datagram |
| RTP fixed header | 12 | PT 96, marker on AU boundary; SSRC from payloader |
| RTP extension | 16 | RFC 5285: id 1, 8 B relative capture ns (BE) |
| Payload | var | Single NAL, STAP-A, or FU-A |

`stream_receiver` strips FEC and length, then passes **`sock_data`** to `rtp_h264_depay` with
`sock_data.seq` set to the post-FEC app sequence (in-process gap metric input; not on wire).

Peer loss metrics on `stream_sender` (`peer_loss_*`, `peer_*_gap_count`) are updated from the
in-process receiver counters in `stream_sdl` — not from reverse UDP telemetry.

## Stream path

```text
TX: … → rtp_h264_pay → stream_sender.0

RX: stream_receiver.0 → rtp_h264_depay → h264_decoder → sdl_sink
```
