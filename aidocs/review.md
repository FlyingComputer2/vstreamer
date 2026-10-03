# vstreamer — code review

Date: 2026-10-03 · Commit reviewed: `82fa2de` (main) plus untracked working-tree files

> **Implementation:** see [`implementation-plan.md`](implementation-plan.md) for the phased, test-gated spec derived from the decisions below.

## Scope

| Area | Depth |
|------|-------|
| `src/core/*` (packets, pools, frame, metrics, RS FEC, RTP H.264, pix convert, factory) | Full read |
| `stream_sender`, `stream_receiver`, `rtp_h264_pay`, `rtp_h264_depay` | Full read |
| `h264_encoder_mpp`, `h264_decoder_mpp` | Full read |
| `jpeg_decoder_multicore`, `v4l2_source`, `mkv_sink` | Hot paths only (I/O, threading, muxing) |
| `test_app/stream_sdl.cpp`, `channel_controller.cpp` | Pipeline threads, console, shutdown |
| `noise_source`, `noise_fft2`, `h264_encoder_cedar`, `h264_encoder_intel`, SDL sinks/presenter | Not reviewed in depth |
| `scripts/` (Python) | `docs_server.py` security pass only |
| Build (`CMakeLists.txt`, `cmake/`), docs, repo hygiene | Full read |

**Verified by running code:** C1 (`open()` hangs) and C3 (FEC block-id wraparound) were reproduced
with small test programs built against the tree. The other findings come from reading the code.

Severity: **Critical** = hang, memory corruption or a broken link in normal use · **High** = wrong
output or a likely race under normal use · **Medium** = edge cases, latency, robustness ·
**Low** = cleanup, style, docs.

---

## Summary — top items to fix first

1. **C1** `stream_sender::open()` deadlocks when the host isn't a dotted IPv4 address (e.g. `localhost:5000`). Reproduced.
2. **C2** If a runtime FEC k/n change fails, the sender silently turns FEC off and the link goes dead. It also corrupts RTP headers.
3. **C3** FEC emit ordering uses 16-bit signed distance on an 8-bit block id, which breaks around the 255→0 wrap. Reproduced.
4. **C4** Stack buffer overflow in `configure()` for numeric keys (`char buf[32]` + unchecked `memcpy`), in 8 places.
5. **C5** `h264_encoder_mpp` takes its two locks in opposite orders (ABBA deadlock), and the slot deques have data races.
6. **C6** `stream_sender` "no FEC" mode is incompatible with `stream_receiver`, which always expects stream + FEC headers.
7. **H1** Pool destructors free memory that outstanding buffers still point at (use-after-free on late release).
8. **H2** `third_party/pffft` is untracked, but CMake needs it with default options, so a fresh clone fails to configure.

---

## Critical

### C1 — `stream_sender::open()` self-deadlocks on non-numeric host
- **Where:** `src/components/stream_sender.cpp:323-327`
- **What:** `open()` holds `mu` (`lock_guard` at the top). When `inet_pton` fails it calls `close()`, which takes `mu` again (`stream_sender.cpp:367`). `std::mutex` is not recursive, so this is undefined behavior; in practice the process hangs forever.
- **Repro:** `configure("stream","localhost:5000"); open();` → hangs (confirmed: `timeout 5` killed it, exit 124).
- **Fix:** On this error path, close `send_fd` inline (as the FEC-init error path a few lines below already does), or move the socket setup into a helper that runs without the lock. Also consider `getaddrinfo` so hostnames work.

### C2 — A failed FEC reconfigure disables FEC and corrupts egress
- **Where:** `src/core/rs_block_erasure.cpp:85` (`init()` sets `enabled_ = false` before validating), `src/components/stream_sender.cpp:520-529` (`reinit_fec_if_active`), `stream_sender.cpp:610-668` (`fec_k` / `fec_n` handlers)
- **What:** `fec_k` accepts 1..254 and `fec_n` accepts 1..255, but `rs_block_erasure::init()` only accepts values ≤ 15. A value like `set_fec_n 20` gets as far as `reinit_fec_if_active()`. `fec.init()` returns false, but it has already cleared `enabled_`. The handler rolls back `fec_n` but never re-inits, so:
  - `input()` now takes the non-FEC path (`fec.enabled()` is false) while `fec_block` stays true.
  - The send thread still stamps a 2-byte `stream_sequence` (`stream_sender.cpp:269-277`) over the first 2 bytes of every raw RTP datagram ≥ 6 bytes, which corrupts the RTP header.
  - The receiver drops everything. The console replies `err ...`, but the stream is already dead.
- **Reachable from:** `channel_controller` console `set_fec_k` / `set_fec_n` (which itself advertises 1..254 / 1..255).
- **Fix:** Validate against `rs_block_erasure::k_header_k_n_max` in `stream_sender::configure` and in the console. Make `init()` validate before touching state, so a failure leaves the previous config running. Never stamp the stream header on packets that didn't reserve room for it.

### C3 — FEC in-order emit queue breaks at block-id wraparound (8-bit id, 16-bit distance)
- **Where:** `src/core/rs_block_erasure.cpp:156` (`emit_distance`), used by `queue_decoded_block`, `skip_emit_block`, `prune_emit_behind` and `advance_emit_past_hole`
- **What:** Wire block ids are 0..255, but the distance is `int16_t(uint16_t(a - b))`, which gives values from −255 to +255 instead of −128 to +127. Near the wrap:
  - A block that is *behind* `emit_next` across the wrap (e.g. id 254 when `emit_next` = 3) looks like +251, i.e. "far future". It is queued and released only after the `emit_hold_ms` timeout (60 ms), and then out of order.
  - A block that is *ahead* across the wrap (e.g. id 0 when `emit_next` = 254) looks like −254 and triggers the "peer restarted" resync. That skips the reorder window and flushes the queue early.
  - `advance_emit_past_hole` picks the "nearest" block using the same broken metric.
- **Repro (confirmed):** k=n=1; deliver ids 250..253, 255, 0, 1, 2, then 254 late. Ids 255..2 are emitted immediately via the resync path. Late 254 is held 61 ms and emitted last. Away from the wrap, the same late block is delivered immediately (the `d < 0` path).
- **Impact:** Every 256 blocks (a few seconds at typical rates), reordering or loss near the wrap adds ~60 ms of head-of-line delay and out-of-order RTP to the depayloader.
- **Fix:** `return static_cast<int8_t>(static_cast<uint8_t>(block_id - emit_next));` and set the "restart" threshold to match (with an 8-bit ring, a real restart can't be told apart from distance anyway; use `done`/timeouts instead).

### C4 — Stack buffer overflow in numeric `configure()` handlers
- **Where:** `stream_sender.cpp:577, 590, 627, 656, 685`; `rtp_h264_pay.cpp:150, 163`; `rtp_h264_depay.cpp:146`
- **What:** `char buf[32]; std::memcpy(buf, value->data(), value->size()); buf[value->size()] = '\0';` with no length check. Any value of 32 bytes or more overflows the stack.
- **Impact:** Today the only remote path (the channel console) re-formats the number first, but `configure()` is the public plugin interface and the docs plan a `key = value` config loader and a UDP console that call it directly.
- **Fix:** Add one shared helper, e.g. `int key_parse_i64(std::string_view, int64_t*)` in `core/key_util.hpp`, that bounds-checks (or uses `std::from_chars`), and use it everywhere. `h264_decoder_mpp` / `h264_encoder_mpp` already do this safely with `std::string tmp(v)`.

### C5 — `h264_encoder_mpp`: lock-order inversion and unsynchronized state
- **Where:** `src/components/h264_encoder_mpp.cpp`
  - `drain_packets_locked` holds `mpp_api_mu` (`:702`) → `ingest_enc_packet` takes `mu` (`:860`).
  - `configure("qp"|"gop"|"cbr"|"rc")` holds `mu` → `apply_rc_cfg_locked` takes `mpp_api_mu` (`:442`). `encoder_close_locked` (under `mu`, from `close()` or a reopen in `input()`) also takes `mpp_api_mu` (`:645`).
  - → **ABBA deadlock** between the output/drain thread and a reconfigure or close.
- **Also:** `input()` drops `mu` and calls `put_nv12_frame_unlocked`, which reads and writes `enc_free_slots` (`:996-1007`, partly outside any lock), `live_*`, `ctx` and `md_info`. Meanwhile `release_enc_slot_after_eoi` (under `mu`) and `encoder_close_locked` change the same deques and free the buffers. These are data races, with use-after-free if `close()` or a reopen overlaps an in-flight `input()`. `enc_au_accum`, `enc_au_pts` and `enc_au_key` are also changed under `mpp_api_mu` only, while `encoder_close_locked` clears them under `mu`.
- **Mitigation today:** `stream_sdl` sends console changes through atomics applied on the encode thread (`apply_pending_console_encoder_cfg`), which hides the problem in the bench app but not in the component.
- **Fix:** Pick one order (e.g. `mu` → `mpp_api_mu`) and never take `mu` while holding `mpp_api_mu`. Collect finished AUs in a local list and push them to `out_q` after releasing `mpp_api_mu`. Protect the slot deques with one lock.

### C6 — "FEC none" mode on the sender cannot be received
- **Where:** `stream_sender.cpp:426-450` (non-FEC path sends the raw datagram with no `stream_header_s` and no FEC shard header); `stream_receiver.cpp:156-177` (`ingest_datagram` always parses a stream header + FEC shard header)
- **What:** `set_fec none` (console) or `fec=none` makes the receiver read the first RTP bytes as `stream_sequence` and FEC header. Most datagrams fail the reserved-bit check and get counted as decode failures. A few may decode as garbage shards.
- **Fix:** Always send the stream header and use the documented `n == k` "no parity" framing (`rs_block_erasure.hpp:14`) for "none". Alternatively, add a header flag and make the receiver handle both.

---

## High

### H1 — `packet_pool` / `frame_pool` destructors: use-after-free on late release
- **Where:** `src/core/packet_pool.cpp:40-50`, `src/core/frame_pool.cpp:39-50`
- **What:** The destructor sets `hdr->pool = nullptr` and then frees every block (`blocks.clear()`). A buffer that is still checked out will later call `release()` → `hdr_of(data)`, which reads the header from freed memory. Nulling the pointer does nothing once the memory is gone.
- **Where it can bite:** `stream_sender` / `stream_receiver` own their pools as members, and their `data_packet`s (with `&packet_pool::release` deleters) live in queues owned by the app (`rx_au_queue`, `pipeline_queue`) or in other components. Destruction order in `main` decides whether this happens.
- **Fix:** Keep block storage alive until every block has been returned (refcount or `shared_ptr` control block), or assert `free_list.size() == pool_depth` in the destructor so leaks show up in debug builds.

### H2 — Build: a fresh clone can't configure with default options
- `third_party/pffft` is **untracked** (`git ls-files third_party` → 0 files), but `CMakeLists.txt` runs `add_subdirectory(third_party/pffft)` when `ENABLE_NOISE_SOURCE=ON` (the default). Add it as a git submodule or FetchContent (as ISA-L already is) with a pinned commit.
- `ENABLE_H264_DECODER_MPP` defaults ON and requires `rockchip_mpp`, so the README's plain `cmake -B build` fails on any non-Rockchip host. Either default it OFF or use `pkg_check_modules(... )` without `REQUIRED` and auto-disable it.
- The ISA-L FetchContent pulls from GitHub at configure time (no offline or vendored option).

### H3 — `stream_receiver::output(timeout < 0)` can block forever after `close()`
- **Where:** `src/components/stream_receiver.cpp:393`
- **What:** The wait predicate is `!payload_queue.empty()` only. `close()` clears the queue and notifies, so the waiter wakes, sees an empty queue and goes back to sleep. Add a `stopping`/`opened` check to the predicate (as `h264_encoder_mpp::output` does).

### H4 — Decoder thread reorders AUs on `-EAGAIN`
- **Where:** `src/test_app/stream_sdl.cpp:2259`
- **What:** When `feed_decoder_au` returns `-EAGAIN`, the AU is pushed back to the *tail* of `rx_au_queue`, behind newer AUs, and the queue may evict the oldest entry. H.264 NALs then reach the decoder out of order, causing corruption until the next IDR. Hold the AU locally and retry it first (as `encode_stage_main` does with `holding`).

### H5 — RTP depayloader: FU-A loss isn't detected; one NAL = one "AU"
- **Where:** `src/core/rtp_h264.cpp:330-433`; `src/components/rtp_h264_depay.cpp:86, 119`
- A lost middle FU-A fragment isn't detected: `fu_active` stays true and fragments are joined across the gap, producing a corrupt NAL. Drop the in-progress FU when the RTP sequence has a gap, or when an FU arrives without the start bit and the sequence isn't contiguous.
- Each NAL is emitted as its own `frame_data` with `key = true` always. The RTP marker bit is ignored, so "AU" is a misnomer and `key` is meaningless downstream.
- `rtp_h264_depay` has a single `au_buf` slot: if `input()` produces a NAL before the previous one was pulled with `output()`, the earlier one is overwritten silently. That works in `stream_sdl` only because it drains after every input.
- Loss stats: a reordered or duplicate packet makes `seq - next` wrap to ~65535 and inflates `expected` (`rtp_h264.cpp:349`). Use a signed 16-bit difference and ignore backward jumps.
- `rtp_datagram_payload_offset` ignores the CSRC count and padding bit (fine for our own sender, wrong for generic RTP).

### H6 — `mkv_sink`: wrong timestamps; file overwritten on resize
- **Where:** `src/components/mkv_sink.cpp:134, 227-228`
- **What:** `st->time_base` is set to `1/fps` before `avformat_write_header`. Matroska rewrites it to 1/1000, but packets are written with frame-count PTS and never rescaled, so recordings play back about `1000/fps`× too fast. Use `av_packet_rescale_ts(pkt, {1,fps}, st->time_base)` after the header is written. PTS also comes from a local counter and ignores the frame's PTS, so dropped frames shorten the timeline.
- A resolution change calls `stop_locked()` + `start_locked()` with the same `output_path`, which truncates the earlier recording.

### H7 — MPP decoder outputs NV21 / NV61 as NV12 (swapped chroma)
- **Where:** `src/components/h264_decoder_mpp.cpp:286-327`
- **What:** `MPP_FMT_YUV420SP_VU` and `MPP_FMT_YUV422SP_VU` are accepted, but `pack_yuv420sp_to_nv12` / `pack_yuv422sp_to_nv12` copy chroma without swapping → wrong colors. Either swap (as `copy_nv12_planes_to_packed(..., swap_chroma=true)` does) or reject them.

### H8 — Wall clock used for pacing, deadlines and rate windows
- **Where:** `stream_sender.cpp:30`, `stream_receiver.cpp:31` (`CLOCK_REALTIME`)
- **What:** The token-bucket pacer, the `set_enabled` deadman (`stream_sender.cpp:478`) and the kbps windows all use wall time. An NTP step or manual clock change can stall sending (negative `dt`), release a burst, or expire or extend the operator gate. Use `CLOCK_MONOTONIC` / `steady_clock` (`core/time_util.hpp` already has `steady_mono_ns`).

### H9 — `query()` returns `string_view` into a shared mutable `query_buf`
- **Where:** every component (`mutable std::string query_buf;`)
- **What:** The returned view points into a member that the next `query()` call overwrites, from any thread. `stream_sdl` queries the same components from its metrics/console thread and from pipeline threads, so the view can dangle or be torn mid-read. This is a contract-level race, not a single bug. Fix it at the interface: return `std::string`, or let the caller pass the buffer in.

---

## Medium

### FEC (`rs_block_erasure`)
- **M1** `fail_missing_shards_` is never incremented and `note_rx_block_loss()` is an empty function (`rs_block_erasure.cpp:771`), so `take_fail_missing_shards()` always returns 0. Implement it or remove it.
- **M2** The hold windows don't match: `rx_hold_ms` = max(10×timeout, 250 ms), but `emit_hold_ms` = max(3×timeout, 60 ms). After one unrecoverable loss, the emit queue skips the hole at 60 ms, and the incomplete block's surviving systematic packets are released at 250 ms via `finish_block_with_available` — *after* later blocks, i.e. out of order and 250 ms late. For live video it's usually better to release surviving systematic shards right away (in order) and drop the block once emit has passed it.
- **M3** `finish_block_with_available` writes straight to `out`, bypassing the in-order emit queue (another path for reordering).
- **M4** `decode_fail_` mixes header-parse failures, k/n mismatches, evictions and RS failures, so the "fec_failures" metric can't tell link loss from protocol errors. Split the counters.
- **M5** With an 8-bit id, `k_block_max = 256` and `rx_hold` = 250 ms, a stale partial block can merge with a new block that reuses the same id once the ring wraps in under 250 ms (≈ >1000 blocks/s, e.g. k=1 at high rate). The k/n/sdu_n mismatch check only catches some of these cases.
- **M6** Per-shard allocation churn: `frags_decode = buf->frags` copies the whole map on every shard after `sdu_n` (`:1039`), `decode_block` copies again into `padded`, and the Cauchy matrix is rebuilt per block (`:557`). Cache the matrix and tables per (k,n) and decode in place.
- **M7** `count_app_packets_in_frags` (`:746`) is an unused non-static function at global scope (external linkage, ODR risk). Make it `static` or delete it. The same applies to `rtp_datagram_payload_offset` in `rtp_h264.cpp:278` (non-static, no header declaration).

### Sender / receiver
- **M8** `stream_sender` queue + pool: the pool depth equals the queue depth (4096), and the buffer is acquired *before* the eviction check. When the queue is full, `acquire()` fails first, so the "evict oldest" branch never runs and the *newest* packet is dropped instead. 4096 × ~1.4 KB is also several seconds of buffering at low bitrates, which is bufferbloat for live video. Use a smaller time-based bound.
- **M9** Data races in `stream_sender`: `mtu` is written in `configure` without `mu`; `pace_bucket_bytes` / `pace_last_sec` are reset in `configure("max_kbps")` while the send thread uses them.
- **M10** The send thread wakes every 5 ms whenever FEC is enabled (`wait_ms = 5`), even when idle. Wait until `fec` deadline − now instead.
- **M11** `stream_receiver::stop_recv_thread` closes `recv_fd` while the receive thread may still be inside `poll`/`recv` on its copied fd. The fd number can be reused by another `open()` in the meantime. Do shutdown → join → close. `channel_controller::stop_relay` / `stop_console` have the same issue: `teardown_direction` closes fds and clears `ingress_queue` before joining the relay thread (`channel_controller.cpp:1053`), and `console_fd` is a plain `int` read by the console thread.
- **M12** The receive buffer is 2048 bytes and `MSG_TRUNC` isn't checked, so oversized datagrams are silently truncated (`stream_receiver.cpp:216`).
- **M13** Reverse telemetry is in-process only: `stream_sdl` copies receiver counters straight into `stream_sender::set_receiver_counters` (`stream_sdl.cpp:1941, 2032`). The rate controller and FEC-gap AIMD will not work across two hosts until a telemetry datagram exists on the reverse path. Worth stating explicitly in the docs.

### RTP packetizer
- **M14** SPS+PPS are sent before the first slice of **every** AU (`rtp_h264.cpp:224`), not only IDRs. That's +2 datagrams per frame, which is expensive on a packet-count-limited, FEC-blocked radio link. When the encoder already emits SPS/PPS in the IDR AU (`MPP_ENC_HEADER_MODE_EACH_IDR`), they are sent twice.
- **M15** NALs past the 64th in an AU are silently dropped (`nal_ptr[64]`).
- **M16** `rtp_h264_pay::input()` clears `pending` on each new AU, which silently drops datagrams not yet pulled. `mtu` / `fps` changes after `open()` have no effect until reopen. `mtu` accepts up to 1500, but with FEC the shard limit is `max_original()` = 1468, so large MTUs are rejected as "oversized" downstream.
- **M17** The capture-timestamp header extension carries a host-endian `steady_clock` value, which is meaningless across machines. Document it as loopback-only, or switch to an NTP-style or relative timestamp in network byte order.

### Codecs
- **M18** `h264_encoder_mpp` only requests an IDR on the first frame (`:1074`). There is no "force keyframe" key, so after unrecoverable loss the receiver stays corrupt until the next GOP (up to 255 frames). Add `configure("idr", ...)` and wire it to receiver feedback.
- **M19** `rc:super_i_thd` = average frame bytes at ≥1080p (`:364-374`), with re-encode up to 8 times. Nearly every I-frame exceeds that, so I-frames get re-encoded repeatedly at worse QP. Check that this is intended.
- **M20** `h264_level_for_size` returns 4.0 for everything above 720p (`:71`). 1080p60 needs 4.2 and 4K needs 5.1.
- **M21** `drain_packets_locked(timeout > 0)` keeps polling until the deadline even after receiving packets, so `output(timeout_ms)` always takes the full timeout. That's latent today (callers pass 0).
- **M22** `h264_decoder_mpp`: `output(timeout)` holds `mu` while polling, which blocks `input()`. `pending_capture_mono_ns` is one value that gets overwritten by the latest input, so the latency metric is attributed to the wrong frame when the decoder pipelines. Output size comes from the configured `size`, not the stream; a mismatch crops or green-pads silently. **Fixed in P7** (`71abb9b`) except: `drain_mpp_to_ready` was still invoked under `mu` in `input()`/`output()`, deadlocking the decode thread (`dec_in=0` in §1.4); drain must run with `mu` released (fixed in follow-up commit after `71abb9b`).
- **M23** `encoder_open_locked` overwrites `ret` across six `mpp_enc_cfg_set_s32` calls and checks only the last one (`:498-503`). `put_nv12_frame_unlocked` leaks a slot on the `-EIO` path (`:1021-1024`).
- **M24** `jpeg_decoder_multicore`: one `worker_cpu` value pins **all** workers to the same core. `stream_sdl` also calls `input()` then `output(-1)` one frame at a time, so the "multicore" decoder never has more than one job in flight.

### App / tooling
- **M25** `submit_nv12_to_encoder` (`stream_sdl.cpp:1310-1360`) spins up to 512 times with no sleep. Its periodic `drain_encoder` sends newer AUs before the `enc_pending` ones already pulled → possible AU reordering on the TX side.
- **M26** The channel console (`channel_controller.cpp:1034`) and the relay bind `INADDR_ANY` with no authentication, so anyone on the LAN can change loss, FEC and encoder settings. Default to `127.0.0.1` with an opt-in flag.
- **M27** `scripts/docs_server.py` has `PUT /api/doc/<path>` with no auth or CSRF/Origin check. Any web page the user visits can overwrite or create `.md` files under `docs/` via `fetch("http://127.0.0.1:<port>/api/doc/...", {method:"PUT"})`. Check the `Origin`/`Host` header or require a token. (Path traversal is handled correctly by `resolve_doc_path`.)
- **M28** `v4l2_source` stamps `capture_mono_ns` after `DQBUF` + memcpy instead of using `buf.timestamp`, which under-reports camera→glass latency. The live path holds `mu` for the whole `select()` (up to 50 ms), which blocks `configure` / `query`.

---

## Low / cleanup

- **L1** `frame_pool.{hpp,cpp}` isn't in `VSTREAMER_SOURCES` and isn't used. Decoders call `malloc` + `memset` for a full NV12 frame each time (`h264_decoder_mpp.cpp:312-317`, `jpeg_decoder_multicore.cpp:143-148`). Either wire the pool in (it removes per-frame allocation and the redundant memset) or delete it. `packet_pool` and `frame_pool` are copies of each other; make one template.
- **L2** `core/frame.hpp:21` calls `free()` without `#include <cstdlib>`.
- **L3** Dead code in `h264_encoder_mpp`: `push_mpp_packet_to_out{,_locked}` are never called. `drain_packets_unlocked` just forwards to `drain_packets_locked`, and its name contradicts what it does. `stream_sdl`'s `pull_encoded_aus{,_locked,_unlocked}` is a chain of three trivial wrappers.
- **L4** Coding-guideline violations (`.cursor/skills/coding-guidelines`: no trailing `_`): `rs_block_erasure` uses `enabled_`, `k_`, `n_`, `recovered_` and others.
- **L5** `metrics` keeps both `order` and `entries` (redundant; `to_string` is O(n²)). `get_metric(name, metric &metric)` gives the parameter the same name as the type, and the "seed metric" argument is an odd API; a default value would do.
- **L6** `key_parse_i64` uses base 0, so `"010"` parses as octal 8. Use base 10 for user-facing keys.
- **L7** `component::configure(std::string_view key, std::string_view *value)` takes a non-const pointer for an input value, and `int64_t` key/value overloads exist that every component rejects. Simplify the interface.
- **L8** Static `std::atomic` log-throttle counters in `h264_decoder_mpp` are shared across instances.
- **L9** `pix_convert` pads missing rows but not missing columns. When the source is narrower than the destination, the right edge stays zero-filled (green in NV12).
- **L10** `CMakeLists.txt` repeats `target_include_directories` / `-Wall -Wextra -Wpedantic` for every executable. A small `vstreamer_add_app()` function or an INTERFACE target would cover it. `noise_fft_bench` links `vstreamer_source` before that target is defined (works, but reads oddly).
- **L11** No CTest: `rs_fec_test` and `rs_block_id_pace_test` exist but aren't registered with `add_test`. Add `enable_testing()`. Good candidates for regression tests: C3 (wraparound), C2 (bad k/n rollback), C1 (open with hostname), FU-A loss (H5).
- **L12** Repo hygiene: `logs/`, `noise.log`, `newmetrics.txt` and `.cursor/rules/` are untracked. Add `logs/` and `*.log` to `.gitignore`, and decide whether `.cursor/rules` should be committed. `scripts/lpf_impulse_0p1.png` is committed next to the scripts; consider moving it to `docs/`.

---

## Documentation

- **D1** `docs/vstreamer.md` describes a design that doesn't match the code: `source`/`sink`/`encoder`/`decoder` interfaces with `fetch`/`write`/`EmitFn`, classes `RtpSink`, `RtpSource`, `Mp4Sink`, `CedrusEncoder`, `DisplaySink`, a feedback plugin, a core config loader and console. The code has `component_source` / `component_coder` / `component_sink` with `input`/`output` ports, `stream_sender` / `stream_receiver` / `rtp_h264_pay` / `rtp_h264_depay` / `mkv_sink` / `sdl_sink`, no feedback plugin, and no config loader. Mark the doc as "target design", or rewrite it from the current headers.
- **D2** The build table in `docs/vstreamer.md` omits `ENABLE_H264_ENCODER_MPP`, `ENABLE_MKV_SINK`, `ENABLE_SDL_SINK`, `ENABLE_STREAM_*`, `ENABLE_RTP_*` and the test-app options.
- **D3** The `stream_sdl.cpp` header comment says `VSTREAMER_PIPE_QUEUE_DEPTH (default 2)`, but `k_default_pipe_queue_depth = 8` (`stream_sdl.cpp:122`).
- **D4** The wire format (stream header → 4-byte FEC shard header → 2-byte length prefix → 2-byte FEC payload seq → RTP) is spread across four headers. One diagram in `docs/packet-model.md` would make the C2/C6 class of bugs much easier to spot.

---

## Suggested order of work

1. C1, C4, H3: small, mechanical fixes.
2. C2 + C6 together: decide the "no FEC" wire format, then validate k/n once.
3. C3 plus regression tests (L11).
4. C5: restructure encoder locking.
5. H1, H2: pool lifetime and build reproducibility.
6. H4, H5, M2/M3: end-to-end ordering under loss. These are what the receiver actually sees on a bad link.
7. Everything else as time allows.

---

## Decisions (2026-10-03)

Status for every item: **Fix** = do as described; notes say what was chosen when there was a choice.
Nothing was deferred or dropped.

**Status column:** tracks `review-fixes` per [`implementation-plan.md`](implementation-plan.md).
Bulk land: `71abb9b` (P0–P8, P6.5, P7). Post-land fixes list their own commit when pushed.

### Critical

| ID | Decision | Status |
|----|----------|--------|
| C1 | Fix the re-lock on the `open()` error path, and resolve hosts with `getaddrinfo` (IPv4 only for now) so names like `localhost:5000` work. | done |
| C2 | Full fix: validate k,n ≤ `k_header_k_n_max` in `stream_sender` and the channel console; make `rs_block_erasure::init()` validate before changing any state, so a failure keeps the old config running; stamp the stream header only on packets that reserved room for it. | done |
| C3 | Use 8-bit wrap math in `emit_distance`; remove the "peer restarted" heuristic (a block more than half the ring away counts as late or stale; `emit_hold` / `done` expiry resyncs the queue after a quiet gap); add a wraparound regression test. | done |
| C4 | Add a shared bounds-safe `key_parse_i64(std::string_view, int64_t*)` (`std::from_chars`, base 10; covers L6) in `core/key_util.hpp` and use it at all 8 sites. | done |
| C5 | Restructure: one lock order `mu` → `mpp_api_mu`, never take `mu` while holding `mpp_api_mu`; collect AUs locally and push them to `out_q` after releasing; protect the slot deques with one lock. | done (P7) `71abb9b` |
| C6 | "none" = the `n == k` framing: always send the stream header + FEC shard header. One wire format; receiver unchanged. | done |

### High

| ID | Decision | Status |
|----|----------|--------|
| H1 | Port `shared_sized_buffer` (from `winject-l3/src/bfcext/shared_sized_buffer.hpp`, which has a test in `winject-l3/src/test/SharedSizedBufferTest.cpp`) into vstreamer core **without** the `bfc` dependency. Back its storage with a pool whose state is owned by a `shared_ptr`, so a buffer released after the pool is gone is safe. Replace `buffer_block` / `packet_pool` / `frame_pool` with it (covers L1). Side benefits to use: zero-copy `subview()` for header stripping (receiver/depay), and copyable packets so one packet can go to several sinks without copying. | done |
| H2 | Fetch pffft at a pinned commit with FetchContent (like ISA-L); build the MPP decoder only when `rockchip_mpp` is found (no hard failure). No offline ISA-L option. | done |
| H3 | Fix: add a stop/opened flag to the wait predicate; return `-EBADF` after close. | done |
| H4 | Keep the AU locally and retry it first (same `holding` pattern as the encode thread). | done (P8) |
| H5 | Full depayloader rework: drop the FU-A on a sequence gap; build real AUs on the marker bit with a correct key flag; output queue instead of a single slot; signed sequence difference for the loss stat; honor CSRC count and padding. | done (P6) |
| H6 | Rescale with `av_packet_rescale_ts`; take PTS from the frame; on a resize, start a new numbered file instead of overwriting. | done (P8) |
| H7 | Swap chroma for the `_VU` formats in the 420SP/422SP packers. | done (P7) `71abb9b` |
| H8 | Use the monotonic clock (`time_util` helpers) for pacing, the send-gate deadline and rate windows. | done |
| H9 | Change the interface to `query(std::string_view key, std::string *out)`; remove `query_buf` everywhere (do together with L7). | done |

### Medium

| ID | Decision | Status |
|----|----------|--------|
| M1 | Implement the missing-shards counter on eviction and expiry. | done `71abb9b` |
| M2 | Release surviving systematic shards in order right away; drop the block once emit has passed it. | done `71abb9b` |
| M3 | Send partial-block payloads through the in-order emit queue. | done `71abb9b` |
| M4 | Separate counters: parse errors, k/n mismatches, evictions, RS failures. | done `71abb9b` |
| M5 | Evict or expire partial blocks once the id ring has moved past them, not only by time. | done `71abb9b` |
| M6 | Cache the Cauchy matrix and tables per (k,n); decode in place without copying the shard map. | done `71abb9b` |
| M7 | Delete `count_app_packets_in_frags`; make `rtp_datagram_payload_offset` static. | done `71abb9b` |
| M8 | Evict before acquiring a buffer (drop oldest); bound the queue by time or bytes (~100 ms) instead of 4096 packets. | done `71abb9b` |
| M9 | Guard `mtu` with `mu`; reset the pacer through a flag that the send thread consumes. | done `71abb9b` |
| M10 | Wait until the FEC flush deadline instead of polling every 5 ms. | done `71abb9b` |
| M11 | Teardown order shutdown → join → close in `stream_receiver` and `channel_controller`; make `console_fd` atomic. | done `71abb9b` |
| M12 | Size the receive buffer to the largest datagram and count `MSG_TRUNC` drops. | done `71abb9b` |
| M13 | This round: document that telemetry is loopback-only. The reverse-path telemetry datagram (receiver → sender) is a separate follow-up, not in this round. | done `71abb9b` |
| M14 | Insert cached SPS/PPS only before IDR AUs that don't already carry them. | done `71abb9b` |
| M15 | Replace the fixed 64-entry NAL array with a vector. | done `71abb9b` |
| M16 | Don't drop datagrams that haven't been pulled yet; apply `mtu`/`fps` live; `mtu` validated against RTP limits only; pipeline creator sizes it from `stream_sender` `max_input` (superseded by N1). | done `71abb9b` |
| M17 | Switch the capture extension to a relative (sender-local) timestamp in network byte order. | done `71abb9b` |
| M18 | Add `configure("idr")` and expose it on the console; wire it to receiver loss feedback later. MPP + Cedar + Intel; Cedar/Intel compile-checked only, **untested on HW** (bench is RK3588 MPP). | done (P7) `71abb9b` |
| M19 | Make the I-frame super-frame threshold a configure key, with a default well above the average frame (~6×). CBR mode only (2026-10-04). | done (P7) `71abb9b` |
| M20 | Choose the H.264 level from resolution × fps per Annex A. | done (P7) `71abb9b` |
| M21 | Return as soon as at least one packet has been drained. | done (P7) `71abb9b` |
| M22 | Poll without holding `mu`; match capture timestamps to frames via PTS; add an explicit scale/crop mode key for the output size. Remove `stream_sdl`'s `g_mpp_hw_mu` afterwards (2026-10-04). | done `71abb9b` (incl. drain-under-mu deadlock fix; P10-T4 regression test) |
| M23 | Check every cfg set call; return the slot on `-EIO`. | done `71abb9b` |
| M24 | Per-worker CPU list (`worker_cpus`); `VSTREAMER_CPU_MAP` in `stream_sdl` with JPEG workers on the big cores 4–7 of the Orange Pi 5 by default; let the app keep several jobs in flight (2026-10-04). | done (P7) `71abb9b` |
| M25 | Back off on `-EAGAIN`; forward AUs strictly in the order they were pulled. | done (P8) |
| M26 | Bind the console and relay to 127.0.0.1 by default, with an opt-in flag for other addresses. | done (P8) |
| M27 | `docs_server.py`: require a token for PUT. | done (P8) |
| M28 | Use `buf.timestamp` (converted to the steady clock); don't hold `mu` across `select`. | done (P8) |

### Low / docs

| ID | Decision | Status |
|----|----------|--------|
| L1 | Replaced by the H1 buffer/pool work (remove the duplicate pools). | done `71abb9b` |
| L2 | Add `<cstdlib>` to `frame.hpp`. | obsolete (frame.hpp no longer allocates; H1) |
| L3 | Delete dead encoder/app wrappers. | done `71abb9b` |
| L4 | Rename `rs_block_erasure` members to follow the coding guidelines. | done `71abb9b` |
| L5 | Use one container in `metrics`; replace the seed parameter with a default value. | done (P8) |
| L6 | Parse as base 10 (part of the C4 helper). | done `71abb9b` |
| L7 | Simplify the component interface: const value input, drop the unused `int64_t` overloads (together with H9). | done `71abb9b` |
| L8 | Per-instance log throttles. | done `71abb9b` |
| L9 | Pad columns in `pix_convert`. | done (P7) `71abb9b` |
| L10 | `vstreamer_add_app()` CMake helper / INTERFACE target for flags and includes. | done `71abb9b` |
| L11 | `enable_testing()`; register `rs_fec_test`, `rs_block_id_pace_test` and new regression tests (C1, C2, C3, H5). | done `71abb9b` |
| L12 | Add `logs/` and `*.log` to `.gitignore`; commit `.cursor/rules/`; delete `newmetrics.txt`. | done `71abb9b` |
| D1 | (Confirmed) Rewrite `docs/vstreamer.md` from the current `component_*` interfaces, and move what isn't built yet into a clearly marked target-design / roadmap section with a status table. | partial (P9 banner + status table) |
| D2 | Complete the build option table. | partial (README §1.1; full table still in CMake) |
| D3 | Fix the queue-default comment in `stream_sdl.cpp`. | done (P9) |
| D4 | Add a full datagram layout diagram to `docs/packet-model.md`. | done (P9) |

### Node independence (decided 2026-10-04)

Principle: every component is independent; only the pipeline creator knows the whole graph.
Implemented as phase P6.5 in the plan.

| ID | Finding | Decision | Status |
|----|---------|----------|--------|
| N1 | `rtp_h264_pay` includes `rs_block_erasure` and clamps `mtu` to the FEC limit (introduced by plan M16). | Validate RTP limits only; the app sizes `mtu` from `stream_sender` `max_input`. | done (P6.5) `71abb9b` |
| N2 | `rs_block_erasure` and `stream_sender`/`stream_receiver` hard-code the winject 1476-byte cap (`stream_air_limits.hpp`). | Delete the header; FEC takes `max_shard_bytes` in `init()`; sender/receiver key `max_datagram` (default 1472); `stream_sdl` sets 1476 for winject. | done (P6.5) `71abb9b` |
| N3 | `sock_data::fec_seq` names a transport detail in a generic packet type. | Rename to `seq` (producer-defined). | done (P6.5) `71abb9b` |
| N4 | Sender/receiver headers name their neighbor components. | Describe pads by packet kind only. | done (P6.5) `71abb9b` |
| N5 | `stream_sender` stores receiver counters (`set_receiver_counters`, `peer_*`). | Move peer telemetry to the app; keep publishing the same metric names. | done (P6.5) `71abb9b` |
| N6 | `v4l2_source` embeds `noise_source` as a fallback. | Fallback policy moves to the app source stage; v4l2 returns `-ENODEV` while the device is down; drop the CMake V4L2→NOISE requirement. | done (P6.5) `71abb9b` |

### Suggested implementation batches

1. **Safety quick wins:** C1, C4 (+L6), H3, H8, L2, L3, M7, M23.
2. **FEC / wire format:** C2, C6, C3 (+test), M1–M6, M8–M10, L4, L11 setup.
3. **Buffer model:** H1 (port `shared_sized_buffer` + pooled storage; L1), then use zero-copy in the receiver/depay.
4. **Interface:** H9 + L7 (`query` → `std::string`), L5, L8.
5. **RTP:** H5, M14–M17.
6. **Codecs:** C5, H7, M18–M22, M24, L9.
7. **App/tooling:** H4, M11, M12, M25–M28, H6.
8. **Build/docs:** H2, L10, L12, D1–D4, M13 doc (telemetry wire design as a separate follow-up).
