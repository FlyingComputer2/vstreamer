# Packet model

Pipeline stages exchange **`component_pdu`** (`core/component_pdu.hpp`): a typed SDU in
`bfc::shared_sized_buffer`, plus `sdu_type`, timestamps, flags, and port index. Caps PDUs
(`CAPS_VIDEO_RAW`, `CAPS_VIDEO_CODED`, …) describe geometry and codec parameters via
`sdu_caps.hpp`.

When a component accepts input caps (`CAPS_VIDEO_*`), it emits matching output caps on its
output port even if the geometry is unchanged from its last output caps. Downstream stages use
that to resync after source switches or other paths that changed size without this component
seeing new input caps.

Receiver RX/gap counters are not carried in PDUs; they are sent on the reverse UDP
link-report path and surfaced as `stream_sender.peer_*` metrics (see [pipeline-flow.md](pipeline-flow.md)).

### RTP capture timestamp extension

`rtp_h264_pay` stamps a one-byte RFC 5285 extension (id **2**, 8 bytes big-endian) with the
capture instant as **CLOCK_REALTIME** nanoseconds since the Unix epoch. `rtp_h264_depay` maps
that value to local monotonic time and stores it in each output AU’s `ts_us` (microseconds) for
latency accounting downstream.

**Clock requirements:** sender and receiver hosts must be time-synchronized (chrony, NTP, or
PTP). Residual clock offset adds directly to reported end-to-end latency; the stack does not
estimate or correct offset in-band.

## Forward datagram (wire v2)

`stream_sender` prepends a 4-byte `stream_header` (version 2) on every UDP datagram: stream media
(FEC shard or raw SDU) and reverse telemetry each carry their own header. Systematic FEC shards
add a 2-byte big-endian `orig_len` before the RTP datagram inside the shard body.

```text
| stream_header (4) | FEC shard hdr (5) | [orig_len (2)] | RTP hdr (12) | RTP ext (16) | H.264 |
```

| Region | Size | Notes |
|--------|------|--------|
| `stream_header` | 4 | `sequence_number` (BE), flags: version=2, `is_fec`, `is_stream_data`, `ext_len` |
| FEC shard header | 5 | `sdu_base`, `k`/`n`/`idx`, `sdu_n` (see `rs_block_erasure`) |
| `orig_len` | 2 | Systematic shards: BE byte count of following app payload; parity shards carry parity bytes (not a length) |
| RTP fixed header | 12 | PT 96, marker on AU boundary; SSRC from payloader |
| RTP extension | 16 | RFC 5285: id 2, 8 B CLOCK_REALTIME capture ns (BE) |
| Payload | var | Single NAL, STAP-A, or FU-A |

`fec none` sends raw SDUs (`is_fec=0`) with only the 4-byte stream header prefix.

`stream_receiver` strips the stream header and FEC, then emits **`STREAM_DGRAM`** PDUs.
`rtp_h264_depay` accepts `RTP` or `STREAM_DGRAM` and outputs `H264_AU` (and `CAPS_VIDEO_CODED`
when SPS dimensions change).

The depayloader orders RTP using the sequence inside the RTP header, not a separate sock sequence.

Peer loss metrics on `stream_sender` (`peer_loss_*`, `peer_*_gap_count`) are fed from
**reverse UDP link reports** (`core/stream_telemetry.hpp`): `stream_receiver` sends cumulative
counters to the source address of the last valid media datagram; `stream_sender` receives them
on its bound media socket. Loss % is derived in the app from report deltas (`stream_sdl`
re-baselines on a new `session_id` or a counter decrease).

v1 and v2 wire layouts do not interoperate; upgrade both ends together. Winject radio forwards
bytes unchanged.

### Reverse path: versioned link report payload (64 bytes, big-endian)

The 4-byte `stream_header` on the reverse datagram has `is_stream_data=0`, `is_fec=0`; `report_seq`
is the header `sequence_number`. The complete UDP payload is 68 bytes. Payload fields:

| Off | Size | Field |
|----:|-----:|-------|
| 0 | 2 | magic bytes `0x56 0x4c` (`VL`) |
| 2 | 1 | report version, `2` |
| 3 | 1 | reserved, must be zero |
| 4 | 4 | `session_id` (nonzero, new each receiver `open()`) |
| 8 | 8 | `timestamp_us` (`CLOCK_MONOTONIC`, deltas only) |
| 16 | 8 | `udp_packet_received` |
| 24 | 8 | `udp_gap_count` |
| 32 | 8 | `fec_packet_received` |
| 40 | 8 | `fec_gap_count` |
| 48 | 8 | `udp_bytes_received` |
| 56 | 8 | `fec_bytes_received` |

All six counters and the sampling timestamp are taken under the receiver's state lock.
UDP bytes include the stream/FEC headers and parity; FEC bytes count reconstructed payloads.
Telemetry itself is excluded from media totals.

New decoders also accept the legacy 44-byte payload (48 bytes with stream header): it has no
version prefix and contains the first six fields from `session_id` through `fec_gap_count`
at offsets 0, 4, 12, 20, 28 and 36. Such reports explicitly lack byte totals, so
`query("peer_byt_recv")` returns `-ENODATA`; zeros are never substituted for unavailable data.
Old decoders reject the new payload length, so upgrade both ends for current telemetry.
Unrecognized versions, nonzero reserved bits, other lengths and zero session IDs are rejected.

## Stream path

```text
TX: … → rtp_h264_pay → stream_sender

RX: stream_receiver → rtp_h264_depay → h264_decoder → sdl_sink
```
