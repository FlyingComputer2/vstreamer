# vstreamer — review fix implementation plan

Source of findings and decisions: [`aidocs/review.md`](review.md). This file is the **executable**
spec: every task says what to change, which design choices are already made, and how to prove it
is done. If something here conflicts with `review.md`, **this file wins**.

Read first: `.cursor/rules/architecture-notes.mdc`, `.cursor/rules/debugging-discipline.mdc`,
`.cursor/skills/coding-guidelines/SKILL.md`.

---

## 0. Ground rules (apply to every task)

1. **Branch:** work on `review-fixes` (create from `main`). One commit per task ID (or per tightly
   coupled group listed together, e.g. `H9+L7`). The commit subject starts with the IDs:
   `C1: stream_sender open() no self-deadlock; resolve hostnames`.
2. **Phases are ordered.** Finish a phase (all its tasks + its gate) before starting the next.
   Within a phase, follow the listed order.
3. **Style:** `.clang-format` (Google + Allman, 4 spaces). No trailing `_` on members (coding
   guidelines). `snake_case`. Match surrounding comment density.
4. **Do not rename existing metric names or console commands.** Scripts depend on
   `h264_encoder.cbr_kbps`, `h264_encoder.out_bytes`, `stream_sender.peer_fec_gap_count`,
   `stream_sender.peer_fec_packet_received`, `stream_sender.peer_udp_gap_count`,
   `stream_sender.peer_udp_packet_received`, and the console verbs listed in
   `src/test_app/stream_sdl.cpp` header. Adding new metrics/commands is fine.
5. **Wire format** may only change where a task says so (C6 framing, M17 RTP extension).
6. **No new compiler warnings.** Baseline warning counts (fresh build, 2026-10-03):
   full = 10, rover = 2, intel = 2, gs = 0. A phase may reduce them, never increase.
7. **Tests must be able to fail.** For every bug-fix task with a "Test" line, write the test
   first, run it against the unfixed code, confirm it fails (or hangs → use a timeout), then fix.
   Note in the commit message: `test fails before fix: yes`.
8. **Thread-touching tasks** (C5, H1, H3, H9, M8–M11, M22, M24): also run the unit tests under
   TSan (see §1.3) and fix any report in code you touched.
9. If a task turns out to need a design decision **not** written here, stop and write the question
   at the bottom of this file under "Open questions" instead of guessing.
10. **Node independence (decided 2026-10-04).** Every component is independent. A component
    validates only its **own** limits and formats; it never includes another component's header,
    never uses another component's constants or limits, and never assumes which node sits
    upstream or downstream (not even in comments). Link-specific values (radio MPDU size, winject
    caps, …) live only in the **pipeline creator** (`stream_sdl`, configs). Components *expose*
    their limits through `query()` (e.g. `max_input`) so the pipeline creator can configure
    neighbors consistently. Two ends of the same wire protocol (`stream_sender`/`stream_receiver`,
    `rtp_h264_pay`/`rtp_h264_depay`) may share that protocol's header in `core/`, nothing more.

---

## 1. Build & verify commands

All four configurations compile on this Rockchip host (Cedar and Intel encoders go through libav).
Build dirs live outside the repo-tracked `build*` names to avoid stale caches.

### 1.1 Configure (once, or after CMake changes)

```bash
cd ~/development/vstreamer
cmake -S . -B out/full  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g" \
      -DENABLE_H264_ENCODER_MPP=ON -DENABLE_SDL_SINK=ON \
      -DENABLE_TEST_STREAM_SDL=ON -DENABLE_TEST_UVC_JPEGDEC_KMSDRM=ON \
      -DVSTREAMER_BUILD_TESTS=ON          # option added in P0-T2
cmake -S . -B out/rover -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_CEDAR=ON
cmake -S . -B out/intel -DENABLE_H264_DECODER_MPP=OFF -DENABLE_H264_ENCODER_INTEL=ON
cmake -S . -B out/gs    -DENABLE_NOISE_SOURCE=OFF -DENABLE_V4L2_SOURCE=OFF \
      -DENABLE_JPEG_DECODER_MULTICORE=OFF
```

Add `out/` to `.gitignore` (P0-T4).

### 1.2 Phase gate (run at the end of every phase; all must pass)

```bash
for b in full rover intel gs; do cmake --build out/$b -j8 2>&1 | tee out/$b.log | tail -1; done
grep -c 'warning:' out/*.log              # must be <= baseline (rule 6)
ctest --test-dir out/full --output-on-failure --timeout 120
```

Plus, for phases 4–6, the loopback integration test (P0-T3) must pass, including its
runtime-reconfigure and restart cases.

### 1.3 TSan run (rule 8)

```bash
cmake -S . -B out/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
      -DENABLE_H264_DECODER_MPP=OFF -DVSTREAMER_BUILD_TESTS=ON
cmake --build out/tsan -j8 && ctest --test-dir out/tsan --output-on-failure --timeout 300
```

Hardware MPP tests (label `hw`, used from P7; suppressions in `tests/tsan_mpp.supp`, see P7 decisions):

```bash
cmake -S . -B out/tsan-hw -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
      -DENABLE_H264_ENCODER_MPP=ON -DENABLE_H264_DECODER_MPP=OFF -DVSTREAMER_BUILD_TESTS=ON
cmake --build out/tsan-hw -j8 && ctest --test-dir out/tsan-hw -L hw --output-on-failure --timeout 300
```

### 1.4 Manual smoke (end of phases 3, 5, 6, 7 — needs the display; report if not possible)

```bash
out/full/stream_sdl --diag          # frames presented, no errors, Ctrl-C exits cleanly < 2 s
# in another shell:
printf 'set_fec_n 12\n' | nc -u -w1 127.0.0.1 5090; printf 'set_fec none\n' | nc -u -w1 127.0.0.1 5090
printf 'set_constant_loss 10\n' | nc -u -w1 127.0.0.1 5090   # picture keeps updating
```

---

## 2. Phase overview

| Phase | Tasks | Why this order |
|-------|-------|----------------|
| P0 Infra | H2, L10, L11 (GoogleTest + loopback test), L12 | Tests and reproducible builds first |
| P1 Safety quick wins | C1, C4+L6, H3, H8, L2, L3, M7, M23 | Small, independent, high value |
| P2 Component interface | H9+L7, L8 | Touches every component — do before deeper per-component work |
| P3 Buffer model | H1+L1 | Touches every component — do before FEC/RTP/codec work |
| P4 FEC & wire | C2, C6, C3, M1–M6, L4 | Depends on P0 tests, P3 buffers |
| P5 Sender/receiver | M8–M12, M13 (doc) | |
| P6 RTP | H5, M14–M17 | |
| P6.5 Node independence | P6.5-T1…T6 (rule 10) | Undo cross-node coupling before more code builds on it |
| P7 Codecs | C5, H7, M18–M22, M24, L9 | |
| P8 App & tooling | H4, H6, M25–M28, L5 | |
| P9 Docs | D1–D4 | Last, so docs describe the final code |

**Execution status (2026-10-04):** P0–P6, **P6.5**, **P7**, and P8 landed on `review-fixes`
(`aef51a9`). **P9** partial (D3, D4 done; D1 banner + roadmap table; D2 README only).
**Remaining:** finish P9 (D1, D2, P9-T5 commit hashes in `review.md`).

**P7 gate (2026-10-04, bench RK3588):** §1.2 builds + `ctest` full (63/63) + §1.3 TSan (62/62) +
`out/tsan-hw -L hw` passed. Warning budget (rule 6): **rover/intel/gs** exceeded baseline on
incremental rebuild (four known warnings: `pix_convert`, `v4l2_source`, `rs_block_erasure`,
`noise_fft_bench`). §1.4 smoke initially showed `dec_in=0` (decode thread deadlock); fixed by
**not** calling `drain_mpp_to_ready` while holding `mu` (see `review.md` M22; fixed in follow-up commit).
After fix: `present_ok` and `dec_in` advance under `--diag`.

---

## P0 — Infrastructure

### P0-T1 · H2 — reproducible configure
- `CMakeLists.txt`: replace `add_subdirectory(third_party/pffft …)` with FetchContent:
  ```cmake
  include(FetchContent)
  FetchContent_Declare(pffft
      GIT_REPOSITORY https://github.com/marton78/pffft.git
      GIT_TAG        e1dbebc9fbf74247d12f094accbbc470aaee8715)   # commit currently in third_party/
  ```
  Keep the existing `PFFFT_*` cache settings before `FetchContent_MakeAvailable(pffft)`; keep it
  out of `ALL` (use `FetchContent_GetProperties`/`FetchContent_Populate` + `add_subdirectory(... EXCLUDE_FROM_ALL)`
  as `cmake/isal_ec.cmake` does). Target stays `PFFFT::PFFFT`.
- Do **not** delete the local `third_party/` directory (untracked, user's checkout); just stop using it.
- MPP auto-detect: change `ENABLE_H264_DECODER_MPP` / `ENABLE_H264_ENCODER_MPP` handling to:
  `pkg_check_modules(MPP IMPORTED_TARGET rockchip_mpp)` (no `REQUIRED`). If a MPP option is ON and
  `MPP_FOUND` is false → `message(WARNING …)` and force the option OFF
  (`set(ENABLE_H264_DECODER_MPP OFF CACHE BOOL "" FORCE)`) **before** `configure_file` and the
  source list. The test-app `FATAL_ERROR` checks stay (they then report the missing decoder).
  Note: option evaluation currently happens before `find_package(PkgConfig)`; move the
  `pkg_check_modules` for MPP above the source-list block.
- **Accept:** `git clone` into `/tmp/x`, then `cmake -S /tmp/x -B /tmp/x/b` with no options
  configures and builds (on this host MPP is found; also verify with
  `PKG_CONFIG_PATH=/nonexistent PKG_CONFIG_LIBDIR=/nonexistent cmake …` that it warns and builds
  without MPP).

### P0-T2 · L10 + L11 — CMake helper and GoogleTest
- Add `function(vstreamer_add_app name)` (sources as ARGN) that does `add_executable`,
  include dirs (`src`, `${CMAKE_CURRENT_BINARY_DIR}/generated`), links `vstreamer_source`,
  `-Wall -Wextra -Wpedantic`. Use it for `noise_fft_bench`, `rs_fec_test`,
  `rs_block_id_pace_test`, `stream_sdl`, `uvc_jpegdec_kmsdrm` (the latter two also link
  `${VSTREAMER_LSAN_HOOKS}`). Move `noise_fft_bench` below the `vstreamer_source` definition.
- `option(VSTREAMER_BUILD_TESTS "Build unit/integration tests" OFF)`. When ON: `enable_testing()`,
  fetch GoogleTest exactly as winject-l3 does (`~/development/winject-l3/src/test/CMakeLists.txt`:
  FetchContent, `GIT_TAG v1.14.0`, `INSTALL_GTEST OFF`), create `tests/` with `tests/CMakeLists.txt`, one
  executable `vstreamer_tests` linking `GTest::gtest_main vstreamer_source`, and
  `gtest_discover_tests(vstreamer_tests)`.
- Register the existing executables as CTest tests: `add_test(NAME rs_fec_test COMMAND rs_fec_test)`,
  same for `rs_block_id_pace_test` (when FEC is enabled).
- Test files go in `tests/` named `<unit>_test.cpp`. Every later task's "Test" line refers to a
  `TEST(...)` in this executable unless it says otherwise.
- **Accept:** `ctest --test-dir out/full` runs ≥ 3 tests and passes.

### P0-T3 · Loopback integration test harness
`tests/loopback_test.cpp` — headless pipeline check required by
`.cursor/rules/architecture-notes.mdc`:
- Wire `stream_sender` (configure `stream=127.0.0.1:<ephemeral>`) → `stream_receiver`
  (`listen=127.0.0.1:<same>`), pick the port by binding a temp socket to port 0 and reading it back.
- Feed synthetic SOCK packets (≤ 1200 B, payload = 4-byte big-endian counter + filler) into
  `sender.input()`; read with `receiver.output(…, 50)`; strip the 2-byte FEC payload header
  (`k_fec_stream_header_len`) and check the counters.
- Provide a helper `run_link(n_packets, setup_fn, mid_fn)` so later tasks add cases.
- Initial cases (must pass on current code): FEC k=6 n=8, 2000 packets, all delivered **in order**,
  no duplicates.
- Later tasks add: runtime `fec_n` change mid-stream (C2), `fec none` (C6), sender restart
  (`close()`+`open()` = new random block id / sequence; C3), drop-every-Nth via a small UDP relay
  thread inside the test (C3/M2).
- **Accept:** test passes, runs < 10 s.

### P0-T4 · L12 — repo hygiene
- `.gitignore`: add `out/`, `logs/`, `*.log`.
- `git add .cursor/rules/` (commit them as-is).
- `git rm`/delete `newmetrics.txt` (untracked → just delete the file).
- **Accept:** `git status --short` shows nothing unexpected after a full build.

**P0 gate:** §1.2.

---

## P1 — Safety quick wins

### P1-T1 · C1 — `stream_sender::open()` deadlock + hostnames
- In `open()` never call `close()` while holding `mu`. Restructure: do parsing + resolution +
  socket creation into locals first; on any failure close the local fd and return; only then
  publish into members under `mu`.
- Resolve with `getaddrinfo(host, port_str, {AF_INET, SOCK_DGRAM}, …)`; use the first result.
  IPv4 only (decision). Keep `parse_host_port` for splitting.
- Apply the same pattern to `stream_receiver::open()` (listen host may be a name, `""`/`0`/`0.0.0.0` = any).
- **Test:** `stream_sender_test.OpenWithHostnameDoesNotHang` — configure `localhost:<port>`,
  call `open()` on a `std::async` and `wait_for(2s)`; must return 0. Also `bogus.invalid:5000`
  → returns a negative errno within 2 s (no hang).

### P1-T2 · C4 + L6 — safe numeric parsing
- `core/key_util.hpp`: add
  ```cpp
  /* Decimal integer, whole string, no leading '+', optional '-'. */
  int key_parse_i64(std::string_view s, int64_t *out);
  ```
  implemented with `std::from_chars` (base 10). Keep the existing `const char*` overload but make
  it forward to the new one with base 10 (L6: no octal/hex surprises). Grep for callers that
  intentionally pass hex (`0x…`) — if any exist (V4L2 control ids), give them an explicit
  `key_parse_i64_auto` (base 0) instead.
- Replace all 8 `char buf[32]; memcpy(...)` sites (`stream_sender.cpp`, `rtp_h264_pay.cpp`,
  `rtp_h264_depay.cpp`) and the `std::string tmp(v)` idiom in the MPP coder/decoder with the new
  overload.
- **Console leniency stays:** `channel_controller` keeps `strtol`/`strtod` parsing for console
  input (scripts may send `set_fec_n 7.0`). Also fix `scripts/utils/console.py`/`cbr_controller.py`
  to send `int(round(x))` for `set_fec_n` (line 202 currently sends the raw clamp result).
- **Test:** `key_util_test` — `"12"`→12, `"-3"`→-3, `"010"`→10, `"0x10"`→error, `""`→error,
  `"12a"`→error, a 100-char digit string → error (overflow), no crash. Plus
  `stream_sender_test.ConfigureLongValueRejected` with a 200-char value under ASan.

### P1-T3 · H3 — receiver `output()` wake on close
- Add `bool stopping` (guarded by `q_mu`) set in `close()` before notify; predicate
  `stopping || !payload_queue.empty()`; if woken with stopping and empty → `-EBADF`.
  Reset `stopping=false` in `open()`.
- **Test:** `stream_receiver_test.OutputUnblocksOnClose` — thread calls `output(…, -1)`; main
  calls `close()` after 100 ms; thread must return `-EBADF` within 1 s.

### P1-T4 · H8 — monotonic clock
- `stream_sender.cpp` / `stream_receiver.cpp`: `now_sec()` uses `CLOCK_MONOTONIC`
  (or `steady_clock`). Check every use: pacer, `set_enabled` deadline, kbps windows — all are
  relative, so no semantic change. Grep the rest of `src/` for `CLOCK_REALTIME`/`system_clock`
  used for intervals and fix those too (leave real wall-clock timestamps in logs alone).
- **Test:** none automatable beyond existing; mention in commit.

### P1-T5 · L2 — `core/frame.hpp`: `#include <cstdlib>` (and use `std::free`).

### P1-T6 · L3 — dead code
- `h264_encoder_mpp`: delete `push_mpp_packet_to_out`, `push_mpp_packet_to_out_locked`;
  rename `drain_packets_unlocked` away (call `drain_packets_locked` directly — C5 will restructure).
- `stream_sdl.cpp`: collapse `pull_encoded_aus_locked`/`_unlocked`/`pull_encoded_aus` into one
  function `pull_encoded_aus`.

### P1-T7 · M7 — delete `count_app_packets_in_frags` (`rs_block_erasure.cpp`); make
`rtp_datagram_payload_offset` `static` inside the anonymous namespace (`rtp_h264.cpp`).

### P1-T8 · M23 — MPP encoder error paths
- `encoder_open_locked`: check every `mpp_enc_cfg_set_s32` (use the existing `cfg_set_s32` helper).
- `put_nv12_frame_unlocked`: on `dst == nullptr` return the slot to `enc_free_slots` before
  returning `-EIO`.

**P1 gate:** §1.2.

---

## P2 — Component interface (H9 + L7, L8)

### P2-T1 · H9 + L7 — new `component` contract
Replace `src/core/component.hpp` with:
```cpp
class component
{
public:
    virtual ~component() = default;

    /* 0 on success, -ENOTSUP unknown key, -EINVAL bad value, other -errno. */
    virtual int configure(std::string_view key, std::string_view value) = 0;
    /* Fills *value (overwritten). 0 / -ENOTSUP / -EINVAL. Thread-safe. */
    virtual int query(std::string_view key, std::string *value) const = 0;
};
```
- Remove both `uint64_t` overloads from every component (no callers exist).
- `v4l2_source`: keep raw V4L2 control access as string keys `ctrl.<id>` where `<id>` is decimal
  or `0x…` hex (use `key_parse_i64_auto` from P1-T2); value is decimal. Implement via the existing
  `set_ctrl_locked` / `get_ctrl_locked`.
- Delete every `query_buf` member; build the string locally.
- Unknown keys return `-ENOTSUP` everywhere (several components currently return `-EINVAL`
  for unknown keys — normalize; check `stream_sdl`/`channel_controller` don't depend on the
  difference).
- Update all call sites: `stream_sdl.cpp` (`cfg_str`, `query_u64`, `query_rate_kbps`,
  `query_component_latency_ms`, `log_bench_diag`, metrics code, self-test), `channel_controller.cpp`,
  `v4l2_source.cpp` (embedded noise), `noise_fft_bench.cpp`, tests.
- **Accept:** `grep -rn "query_buf\|string_view \*value\|uint64_t key" src` → no hits;
  all four configs build; ctest passes; TSan clean.

### P2-T2 · L8 — log throttles
- Replace function-local `static std::atomic<unsigned>` throttle counters (e.g.
  `h264_decoder_mpp.cpp` `n_errinfo`, `n_fbc`, `n_pix`; grep `static std::atomic` in
  `src/components`) with per-instance members.

**P2 gate:** §1.2 + §1.3 + §1.4 smoke.

---

## P3 — Buffer model (H1 + L1)

### Design (decided)

Reference implementation: `~/development/winject-l3/src/bfcext/shared_sized_buffer.hpp` and its
test `~/development/winject-l3/src/test/SharedSizedBufferTest.cpp`. Port it **without** `bfc`.

**`src/core/shared_sized_buffer.hpp`** (namespace `vstreamer`), API = winject's, adapted:
```cpp
class shared_sized_buffer
{
public:
    struct storage
    {
        std::byte *bytes = nullptr;
        size_t     capacity = 0;
        std::function<void(std::byte *)> release;   // called once in ~storage
        ~storage();                                  // if (release) release(bytes)
    };

    shared_sized_buffer() = default;                 // copyable + movable (default ops)
    static shared_sized_buffer copy_from(const void *data, size_t len);   // heap
    static shared_sized_buffer allocate(size_t capacity);                 // heap, size 0
    /* Wrap foreign memory (e.g. malloc'd, libav). Takes ownership via release. */
    static shared_sized_buffer adopt(std::byte *p, size_t capacity, size_t size,
                                     std::function<void(std::byte *)> release);

    size_t size() const noexcept; size_t offset() const noexcept; size_t capacity() const noexcept;
    bool empty() const noexcept;   std::byte *data() const noexcept;
    uint8_t *u8() const noexcept;  // reinterpret_cast helper; most call sites use uint8_t
    void clear() noexcept; void reserve(size_t); void resize(size_t);
    shared_sized_buffer subview(size_t rel_offset, size_t len) const noexcept;
    long use_count() const noexcept;   // storage refcount (0 if empty)
private:
    std::shared_ptr<storage> store; size_t off = 0; size_t len = 0;
};
```
- Member names follow vstreamer guidelines (no trailing `_`).
- Moved-from objects must be `empty()` (winject test relies on it; make the move ops zero `len`/`off`).

**`src/core/buffer_pool.hpp/.cpp`** — replaces `packet_pool` and `frame_pool`:
```cpp
class buffer_pool
{
public:
    buffer_pool(size_t block_bytes, size_t depth);
    /* Pooled block if available and need <= block_bytes, else heap fallback (counted).
     * Returned buffer has size == need. Never returns empty for need > 0 unless OOM. */
    shared_sized_buffer acquire(size_t need);
    size_t available() const; uint64_t misses() const;   // heap fallbacks
private:
    struct state;                       // mutex + free list of std::unique_ptr<std::byte[]>
    std::shared_ptr<state> st;          // captured by each storage::release
};
```
- `storage::release` for pooled blocks captures `std::shared_ptr<state>`; it pushes the block
  back if `state` is still `accepting` (set false in `~buffer_pool`), else deletes it.
  **A buffer outliving its pool is therefore safe** (H1 fixed by construction).
- Heap fallback instead of failing: pool exhaustion no longer causes drops; queue bounds (M8)
  are the backpressure. Expose `misses()` as a metric (`<component>.pool_misses`).

**Packet model changes**
- `packet_types.hpp`: delete `buffer_block`; `frame_data::buf`, `audio_data::buf`,
  `sock_data::buf` become `shared_sized_buffer`.
- `data_packet`: body becomes `std::shared_ptr<packet_body>`; **copyable** (shallow).
  Rule (write it as a comment on `data_packet`): *a packet's body and bytes are immutable once it
  has been passed to `input()` or returned from `output()`. Code that needs to modify bytes must
  own the only reference (`buf.use_count() == 1`) or copy.* `cast<T>` keeps working.
- `frame` (component-internal): replace `data`/`size`/`data_deleter` with a
  `shared_sized_buffer payload`; `frame::reset(kind, w, h, pts, key, shared_sized_buffer, capture_ns)`.
  `adopt_frame` / `move_to_frame` become trivial moves.
- Delete `packet_pool.*`, `frame_pool.*`, `default_data_deleter`.

### Tasks
- **P3-T1** Add `shared_sized_buffer` + `buffer_pool` + tests (port the 3 winject tests; add
  `PoolBufferOutlivesPool` — acquire, destroy pool, then drop buffer under ASan: no error;
  `PoolRecycles` — acquire/release depth+1 times, `misses()==1`; `SubviewKeepsStorageAlive`).
  The "outlives pool" test must fail (ASan) on the old `packet_pool` first — write it against
  `packet_pool` in a throwaway commit or just run it locally and note the result.
- **P3-T2** Switch `packet_types` / `data_packet` / `frame` and migrate **all** components and
  `stream_sdl` in one commit (it won't compile half-way). Mechanical mapping:
  `x.buf.data` → `x.buf.u8()`, `x.buf.size` → `x.buf.size()`,
  `buf.reset(p, n, &packet_pool::release)` → `buf = pool.acquire(n)` then write into `u8()`,
  `malloc`+deleter patterns → `shared_sized_buffer::allocate(n)` + `resize(n)` (or `adopt` when
  the memory comes from a library and is freed by it).
  Pools per component (keep current depths/block sizes): `stream_sender` 1500 B × 1024,
  `stream_receiver` 1500 B × 1024, `rtp_h264_pay` 1500 B × 64. Decoders (MPP, JPEG): add a
  `buffer_pool` sized to one NV12 frame (`w*h*3/2`) × 8, recreated when the output size changes;
  this replaces `malloc` + `memset` per frame (L1). Drop the `memset` where the packer writes
  every byte (it does after L9 pads columns; until P7 keep it).
- **P3-T3** Zero-copy where it is now trivial (no behavior change):
  `rtp_h264_depay` strips the 2-byte FEC payload header with `subview(2, size-2)` instead of
  pointer math. (Receiver/FEC zero-copy is done in P4-T6.)
- **Accept:** `grep -rn "packet_pool\|frame_pool\|buffer_block\|default_data_deleter" src` → none;
  all configs build; ctest + loopback pass; TSan clean; §1.4 smoke.

**P3 gate:** §1.2 + §1.3 + §1.4.

---

## P4 — FEC and wire format

### Design: emit/ordering rules (C3, M2, M3, M5) — implement exactly this

Definitions: block ids are 8-bit on the wire. `dist(a, b) = int8_t(uint8_t(a - b))` (range
−128..127). `emit_next` = id of the next block whose payloads may be released. Every RX block
tracks `released` = number of leading systematic indices already emitted (0..sdu_n).

1. **Distance:** all comparisons use `dist`. No 16-bit arithmetic on block ids anywhere in RX.
2. **New shard for block B:**
   - `dist(B, emit_next) < 0` and B is in `done` → duplicate, drop.
   - `dist(B, emit_next) < 0` and B not done → **late block**: buffer as usual; when it completes
     (decodes or all systematic present) emit its not-yet-released payloads immediately, count
     `late_blocks`.
   - otherwise → normal assembly.
3. **Head-of-line streaming (M2):** for the block with id `emit_next`, whenever systematic
   indices `released, released+1, …` are present (received or recovered), emit them immediately
   in index order and advance `released`. A missing index stops the prefix.
4. **Block completion:** when the head block becomes decodable, recover, emit the remaining
   indices in order, mark done, `emit_next++`, then repeat rule 3 for the new head (blocks
   already complete in the queue drain immediately).
5. **Giving up on the head block:** if the head block has a gap and either (a) `emit_hold_ms`
   has elapsed since a *later* block first had data, or (b) the head block can no longer be
   completed (all n shards accounted for or evicted by rule 7), then emit its remaining present
   systematic payloads in index order (M3: through this same path — no direct writes to `out`),
   count missing ones in `fail_lost_app_pkts`, mark done, `emit_next++`.
   A head id for which **no shard was ever seen** is skipped under (a) as well.
6. **Resync (C3, timeout-based):** there is no "peer restarted" heuristic. If no payload has
   been emitted for `rx_hold_ms` and a shard arrives for a block B with `dist(B, emit_next) < 0`,
   set `emit_next = B` (rebase), clear queue state for ids "behind" B. The first shard ever seen
   also sets `emit_next`.
7. **Ring eviction (M5):** track `newest` = id with the largest `dist` seen. Any RX block with
   `dist(id, newest) < -64` is evicted (as in rule 5b) even if `rx_hold_ms` hasn't expired;
   `done` entries with `dist(id, newest) < -64` are dropped too.
8. All emitted payloads leave `rs_block_erasure` through one function in strictly the order
   above. `push_air` / `poll_rx` append to `out`; nothing else writes to `out`.

### Tasks
- **P4-T1 · C2** — `rs_block_erasure::init(k, n, timeout)`: validate first, return false
  **without changing any state** on invalid input. Valid: `1 ≤ k ≤ n ≤ 15`, `timeout ≥ 0`.
  `stream_sender::configure("fec_k"/"fec_n")`: validate against
  `rs_block_erasure::k_header_k_n_max` and `k ≤ n` before storing; on a failed reinit keep the old
  k/n **and** a working encoder (because init no longer mutates on failure). Console
  `set_fec_k`/`set_fec_n` error text: `err bad k (1..15)` / `err bad n (k..15)`.
  Remove header stamping on packets that did not reserve header room (after C6 every queued
  packet reserves it; assert `size >= k_stream_header_len + k_header_len`).
  **Test (loopback):** start k=6 n=8, after 500 packets send `fec_n=20` (rejected, -EINVAL),
  then `fec_n=10` (accepted); all 2000 packets delivered in order.
- **P4-T2 · C6** — FEC mode `none` = `k = n = 1` with the normal framing (stream header + shard
  header). Replace `bool fec_block` with `enum class fec_mode_e { none, block }`; effective
  `(k, n) = none ? (1, 1) : (fec_k, fec_n)`. `configure("fec","none")` switches to none;
  `"block"`/`"RS_BLOCK_ERASURE"` and any `fec_k`/`fec_n` set switch back to block. Delete the
  non-FEC send path in `input()`. `query("fec_k"/"fec_n")` keep returning the **configured** block
  values; add `query("fec_mode")`. Timeout for k=1 is irrelevant (every packet flushes).
  **Test (loopback):** `fec none` from the start → 2000 packets in order; switch block→none→block
  mid-stream → no loss, in order.
- **P4-T3 · C3 + M2 + M3 + M5** — implement the rules above. Remove `emit_skipped`'s 16-bit
  logic, the restart branch in `queue_decoded_block`, and `finish_block_with_available`'s direct
  writes.
  **Tests (`rs_block_erasure_test`, unit, no sockets):**
  - `WrapLateBlockDeliveredImmediately` — k=n=1, ids 250..253,255,0,1,2 then 254: 254 must be
    emitted on arrival (not after `emit_hold_ms`); with the old code it is emitted after ~60 ms
    (this reproduction already exists in review.md C3 — use it as the failing test).
  - `WrapInOrderNoLoss` — 1000 blocks across several wraps, random reorder within a window of 3
    blocks: output order == input order, no gaps.
  - `HeadOfLineStreaming` — k=4 n=6, drop systematic index 2 of a block and **all** parity:
    indices 0,1 emitted on arrival; 3 emitted at give-up; nothing from the next block before 3.
  - `RecoveredStillInOrder` — drop index 1, keep parity: output order 0,1,2,3.
  - `PeerRestartResync` — after 300 blocks, jump the id by −50 (simulated restart) with a 300 ms
    gap: delivery resumes within one block, nothing stuck.
  - `RingEvictsStalePartial` — partial block id X, then 70 newer blocks quickly (< rx_hold):
    X evicted, later reuse of id X is assembled fresh.
  **Loopback:** sender restart mid-stream (close/open) → delivery resumes < 300 ms, no duplicates.
- **P4-T4 · M1 + M4** — counters in `rs_block_erasure` (lifetime + `take_*` interval variants as
  today): `hdr_errors`, `kn_mismatch`, `evicted_blocks`, `rs_failures`, `missing_shards`
  (on eviction/give-up: `(sdu_n + (n - k)) - shards_received`), `late_blocks`.
  Keep `decode_fail()` = `evicted_blocks + rs_failures` so `stream_receiver`'s `fec_failures`
  metric keeps its meaning; expose the others as new receiver query keys/metrics
  (`stream_receiver.fec_hdr_errors`, …). Delete `note_rx_block_loss`.
- **P4-T5 · M6** — cache `encode_matrix` and decode inputs per `(k, n)` in a small
  `std::array<…, 16*16>` / map; build lazily. In `push_air` stop copying `frags` into
  `frags_decode`; `decode_block` takes pointers/views and only allocates outputs for missing rows.
  **Accept:** existing `rs_fec_test` + new tests pass; add a micro-benchmark line to
  `rs_block_id_pace_test` output or note before/after timing in the commit message.
- **P4-T6 · zero-copy RX** — store received shards as `shared_sized_buffer` (from the
  receiver's pool); emitted systematic payloads that were received intact are
  `subview(k_header_len + k_len_prefix, len)` of the datagram (no copy); recovered ones are new
  buffers. `stream_receiver` no longer prepends the 2-byte FEC payload sequence (that would force
  a copy): it outputs the payload only and carries its output sequence number in `sock_data`
  (generic `uint16_t seq` field; meaning defined by the producer — see P6.5-T3). `rtp_h264_depay`
  stops stripping 2 bytes. Remove `fec_stream_header.hpp` if no longer used.
- **P4-T7 · L4** — rename `rs_block_erasure` members to drop trailing `_` (accessor clashes:
  e.g. storage `enabled_` → `active` with accessor `enabled()`; `k_`/`n_` → `cfg_k`/`cfg_n`).

**P4 gate:** §1.2 + loopback (all cases) + §1.3.

---

## P5 — Sender / receiver

- **P5-T1 · M8** — `stream_sender` queue: check bound and evict oldest **before** acquiring a
  buffer. Bound by bytes: `limit = max(32 KiB, rate_Bps × queue_ms / 1000)` where
  `rate_Bps = max_wire_kbps×125` if pacing is on, else the measured ingress rate; plus a hard
  cap of 1024 packets. New key `queue_ms` (default 100, range 10..2000). Count evictions in
  `dropped` as today. **Test:** with `max_kbps=1000`, push 5 MB fast: queue never exceeds the
  limit; newest packets are the ones that get sent.
- **P5-T2 · M9** — `mtu` guarded by `mu`; `configure("max_kbps")` sets an atomic
  `pace_reset` flag; the send thread resets `pace_bucket_bytes`/`pace_last_sec` when it sees it.
  TSan must be clean on a test that hammers `configure("max_kbps")` while sending.
- **P5-T3 · M10** — send thread waits until `min(next FEC flush deadline, 50 ms)` instead of a
  fixed 5 ms; `rs_block_erasure` gets `bool next_deadline(time_point *out) const`.
- **P5-T4 · M11** — teardown order shutdown → join → close in `stream_receiver::stop_recv_thread`,
  `channel_controller::stop_relay` (join relay thread before `teardown_direction`) and
  `stop_console`; `console_fd` / direction fds read by threads become `std::atomic<int>` or are
  only closed after join. **Test:** open/close `stream_receiver` 200× in a loop under TSan.
- **P5-T5 · M12** — receive buffer `uint8_t buf[65536]` (max UDP), use `recvmsg`/`MSG_TRUNC`
  to detect truncation; count `rx_truncated`.
- **P5-T6 · M13 (doc only)** — add a "Telemetry" note to `docs/pipeline-flow.md` and
  `.cursor/rules/architecture-notes.mdc`: receiver counters reach `stream_sender` only in-process
  (`stream_sdl` `sync_peer_link_metrics_live`); a cross-host telemetry datagram is a planned
  follow-up (not in this plan). Fix the architecture-notes bullet that says they are "sent back
  over reverse telemetry".

**P5 gate:** §1.2 + loopback + §1.3 + §1.4.

---

## P6 — RTP

### Design: depayloader output contract (H5)
- Output = **one `frame_data` per access unit**: all NALs with the same RTP timestamp,
  Annex-B (4-byte start codes), in arrival order. An AU is complete on the marker bit; if a packet
  with a new timestamp arrives first, the previous AU is completed without marker.
- `key = true` iff the AU contains an IDR slice (NAL type 5).
- Loss handling: track RTP sequence with signed 16-bit diff. On a gap inside an FU-A, drop that
  NAL (count `nal_dropped`); deliver the remaining complete NALs of the AU. Set a sticky
  `need_idr` counter (query key `need_idr`, incremented per damaged AU) — M18 will consume it later.
- Reordered/duplicate packets (diff ≤ 0): drop, count `rtp_reordered`; do not touch loss stats.
- Parse CSRC count (`CC` bits) and padding (`P` bit, last byte = pad length) correctly; support
  STAP-A (type 24) input.
- Output queue: `std::deque<frame_data-packet>`, depth 8, drop-oldest with counter `au_dropped`.
- `pts` = RTP ts × fps / 90000 as today; capture timestamp per M17.

### Tasks
- **P6-T1 · H5** — implement the contract in `rtp_h264_depacketizer` + `rtp_h264_depay`.
  **Tests (`rtp_h264_test`):** pack→depack round-trip of an AU with SPS+PPS+IDR (+ a NAL larger than
  MTU) → one frame, `key=true`, bytes identical; drop one middle FU fragment → that NAL missing,
  others present, `need_idr` incremented; reorder two packets → no loss-stat inflation; CSRC=2
  packet parsed; padded packet parsed; STAP-A with 2 NALs → both NALs present.
  Update `stream_sdl` RX thread for one-AU-per-output (it already loops `output()`).
- **P6-T2 · M14** — packer inserts cached SPS/PPS only before an AU that contains an IDR and does
  not already contain SPS/PPS. **Test:** non-IDR AU → no SPS/PPS datagrams; IDR AU without
  parameter sets → inserted once; IDR AU with them → not duplicated.
- **P6-T3 · M15** — `std::vector` of NAL spans instead of `nal_ptr[64]`. **Test:** AU with 100 NALs.
- **P6-T4 · M16** — `rtp_h264_pay`: keep un-pulled datagrams (append; cap 512 datagrams,
  drop-oldest with counter); `mtu`/`fps` changes rebuild the packer config immediately. `mtu` is
  validated against **RTP limits only** (rule 10; superseded clamp removed in P6.5-T1).
- **P6-T5 · M17** — capture extension becomes **relative**: one-byte header ext, id 1, 8 bytes =
  `uint64` big-endian nanoseconds since the packer's epoch (`steady_mono_ns()` at `open()`).
  `rtp_h264_pay` exposes `query("capture_epoch_ns")`. `rtp_h264_depay` gets
  `configure("capture_epoch_ns", …)`; when set, output `capture_mono_ns = epoch + rel`, else 0.
  `stream_sdl` (same process) passes the sender epoch to the depay after opening both.
  Document in `docs/packet-model.md` that the absolute mapping is only valid in-process.
  **Test:** round-trip with epoch set → `capture_mono_ns` equal to input; without → 0.

**P6 gate:** §1.2 + loopback + §1.4 (stage latency lines still print sane values with `--diag`).

---

## P6.5 — Node independence (rework of P3–P6 code; decided 2026-10-04)

Applies §0 rule 10 to code that already exists in the working tree. Do this phase **before P7**.

- **P6.5-T1 · `rtp_h264_pay` knows nothing about FEC.** Remove `#include "core/rs_block_erasure.hpp"`
  and the `max_original()` clamp. Valid `mtu` = `[12 + 16 (capture ext) + 2 + 1, 65507]`
  (RTP header + extension + FU-A header + 1 byte). Keep the pool block size ≥ `mtu`
  (recreate the pool when `mtu` changes). **Test:** `mtu=9000` accepted; `mtu=20` rejected;
  `grep rs_block_erasure src/components/rtp_h264_*` → nothing.
- **P6.5-T2 · No radio constants in nodes or the FEC codec.**
  - Delete `core/stream_air_limits.hpp`.
  - `rs_block_erasure`: `init(k, n, timeout_ms, max_shard_bytes)`; `max_original()` becomes a
    non-static member = `max_shard_bytes - k_header_len - k_len_prefix`. RX side accepts any
    shard up to the size of the datagram it was given (no global cap).
  - `stream_sender`: new key `max_datagram` (bytes of UDP payload on the wire, including
    `stream_header_s`; default **1472** = 1500 Ethernet MTU − IP/UDP headers; range 64..65507).
    FEC is initialized with `max_shard_bytes = max_datagram - k_stream_header_len`. New query
    `max_input` = largest `input()` payload accepted with the current FEC mode
    (= `fec.max_original()`), so the pipeline creator can size upstream nodes.
  - `stream_receiver`: same `max_datagram` key (default 1472); datagrams larger than it are
    counted (`rx_oversize`) and dropped; the receive buffer stays 64 KiB for truncation detection.
  - `stream_sdl` (pipeline creator): sets `max_datagram=1476` on sender and receiver (winject
    cap, comment says why), then sets `rtp_h264_pay` `mtu` from `sender.query("max_input")`
    (or keeps its configured mtu if smaller). `rs_fec_test` / unit tests pass explicit sizes.
  - **Test:** sender `max_datagram=600`, payloader `mtu` from `max_input` → loopback 2000 packets
    in order, none `fec_oversized`; payload larger than `max_input` → counted oversized, not sent.
- **P6.5-T3 · Neutral packet fields.** Rename `sock_data::fec_seq` → `seq` with comment
  "producer-defined sequence number (0 if unused)". Update producers/consumers.
- **P6.5-T4 · Neighbor-free comments.** `stream_sender.hpp` / `stream_receiver.hpp` (and any other
  component header) describe their pads by packet kind only ("Pad 0: SOCK in → UDP egress"),
  never by naming another component. `grep -n "rtp_h264\|stream_sender\|stream_receiver" src/components/*.hpp`
  must only hit each file's own class.
- **P6.5-T5 · Peer telemetry moves to the app.** Remove `set_receiver_counters`, `peer`,
  `peer_*`/`telemetry` query keys from `stream_sender`, and delete `core/stream_telemetry.hpp`
  (move the struct into `stream_sdl.cpp` or a `test_app/` header). `stream_sdl` keeps publishing
  the **same metric names** (`stream_sender.peer_udp_packet_received`, `…peer_fec_packet_received`,
  `…peer_udp_gap_count`, `…peer_fec_gap_count`, `…peer_loss_udp_pct`, `…peer_loss_fec_pct`)
  from its own state — `scripts/cbr_controller.py` depends on them. `stream_receiver` keeps its
  own counters (`link_counters_snapshot` returns a receiver-local struct defined in its header).
  Update `.cursor/rules/architecture-notes.mdc` (loss metrics are assembled by the app).
  **Test:** run `cbr_controller.py` against `stream_sdl` for 30 s (or the metrics query by name
  via the console `get_metric`): all six metric names resolve.
- **P6.5-T6 · Camera fallback moves to the app.**
  - `v4l2_source`: remove the embedded `noise_source`, its passthrough keys (`noise-*`,
    `pregenerate-*`, …) and `state` values that describe noise. It only captures: `open()`
    succeeds even if the device is absent (it keeps retrying internally, as today);
    `output()` returns `-ENODEV` while the device is down; `query("state")` = `capturing` |
    `waiting_device`.
  - CMake: delete the rule "ENABLE_V4L2_SOURCE requires ENABLE_NOISE_SOURCE".
  - `stream_sdl` source stage (pipeline creator) owns the policy: primary `v4l2_source`; on
    `-ENODEV` it pulls from a `noise_source` configured with the same `size`/`fps`/format (NV12)
    and retries the camera on every loop (cheap: v4l2 returns immediately); switches back as soon
    as a camera frame arrives. Log the transitions once each. The `source.state` metric
    (`camera` | `noise_fallback`) and the `source.noise_*` metrics are published by the app.
  - `uvc_jpegdec_kmsdrm` gets the same behavior (shared code).
  - **Test (manual, §1.4 with `--source /dev/video0` if a camera is attached; else document
    skipped):** unplug → noise within 1 s; replug → camera within 2 s. Unit test the selector
    logic with two fake sources.

**P6.5 gate:** §1.2 + loopback + §1.4. Also:
`grep -rn "stream_air_limits\|rs_block_erasure" src/components/rtp_h264_* src/components/v4l2_* src/core/rtp_h264.*` → nothing;
`grep -rn "noise" src/components/v4l2_source.*` → nothing.

---

## P7 — Codecs

**Phase order (decided):** Do **P7-T1 (C5)** before **P7-T4–T9** (M18–M24). **P7-T2 (H7)** and
**P7-T3 (L9)** may run in parallel with C5 (no MPP, no encoder locks). **P7-T8 (M22)** follows the
same lock-order pattern as C5 (`mu` → `mpp_io_mu`); implement after C5 lands.

**Scope:** C5 applies to **`h264_encoder_mpp` only**. Cedar/Intel encoders are not unified in this
task. **P8-T2 (M25)** (`submit_nv12_to_encoder` ordering) does **not** remove the need for C5: ABBA
and slot races live inside the encoder while the console calls `configure` during encode.

**P7 decisions (2026-10-04)** — these override anything below that disagrees:

| Topic | Decision |
|-------|----------|
| Host | Bench host is an **Orange Pi 5 (RK3588S)**: CPUs 0–3 little A55 (capacity 414, 1.8 GHz), 4–7 big A76 (≈1010, 2.3 GHz). Rover is Orange Pi PC (H3, 4 equal cores, CPU0 reserved for `winject-manager`). |
| Thread map (M24) | One env var `VSTREAMER_CPU_MAP` in `stream_sdl`, entries separated by `;`, each `stage=cpulist` (`cpulist` = comma list and/or ranges, e.g. `4-7` or `4,5`). Stages: `source`, `jpeg` (submit thread), `jpeg_workers`, `encode`, `rx`. Unlisted stages keep the default; `-1` = don't pin. **Defaults (Orange Pi 5):** `source=0;jpeg=1;encode=2;rx=3;jpeg_workers=4-7` — compute-heavy JPEG workers on big cores, the MPP-hardware stages stay on little cores. `jpeg_decoder_multicore` `workers` defaults to the number of CPUs in `jpeg_workers`. Remove `VSTREAMER_JPEG_CPUS` (superseded). Present thread stays unpinned. Document the rover H3 layout as an example map in `docs/vstreamer.md`: `source=1;jpeg=1;jpeg_workers=1,2;encode=3;rx=-1`. |
| C5 test | Real MPP hardware test. `GTEST_SKIP()` if `mpp_create`/`mpp_init` fails. ctest label `hw`. Always run under ASan with a 20 s watchdog. TSan: separate config `out/tsan-hw` (`-fsanitize=thread`, `-DENABLE_H264_ENCODER_MPP=ON -DENABLE_H264_DECODER_MPP=OFF -DVSTREAMER_BUILD_TESTS=ON`) with suppressions file `tests/tsan_mpp.supp` (`called_from_lib:librockchip_mpp.so`, `race:librockchip_mpp.so`, `deadlock:librockchip_mpp.so`) set via the test's ctest `ENVIRONMENT` property `TSAN_OPTIONS=suppressions=…:halt_on_error=1`. Any report whose stack contains vstreamer frames must be fixed, not suppressed. |
| M18 scope | Implement `idr` for MPP, Cedar and Intel. Cedar/Intel are **compile-checked only** (no hardware here); mark them "done (untested on HW)" in `review.md`. |
| `g_mpp_hw_mu` | Remove from `stream_sdl.cpp` (both call sites) as part of P7-T8, once `h264_decoder_mpp` is internally thread-safe. |
| M19 mode | `super_i_ratio` / `super_p_ratio` only apply in CBR mode (`rc=cbr`); in fixqp mode super-frame re-encode stays disabled. |

- **P7-T1 · C5** — `h264_encoder_mpp` locking:
  - **Problem (review):** `drain_packets_locked` holds `mpp_api_mu` then `ingest_enc_packet` takes
    `mu` at EOI; `configure` / `encoder_close_locked` hold `mu` then `mpp_api_mu` → ABBA. Slot
    deques and `enc_au_*` are updated outside a single critical section (e.g. `put_nv12` pops
    `enc_free_slots` then takes `mpp_api_mu` later).
  - Lock order **`mu` → `mpp_api_mu` only**. Code holding `mpp_api_mu` must **never** take `mu`.
  - **`mu` owns:** `opened`, `reopen_req`, `out_q`, `cv`, and anything `query()` / `output()` read
    without touching MPP. **`mpp_api_mu` owns:** slot deques, `enc_slots`, `enc_au_*`, MPP
    `encode_*` calls, and `live_*` used on the encode path.
  - Split “assemble AU” vs “enqueue”: `ingest_enc_packet` (and `drain_enc_packets_nonblock`) build
    completed AUs in a **local** `std::vector<frame>` while holding only `mpp_api_mu`; callers push
    into `out_q` **after** releasing `mpp_api_mu` (take `mu`, then `notify` on `cv`). Do not call
    today’s `flush_enc_au_to_out_locked` from inside `mpp_api_mu`.
  - `input()` holds `mu` only for `opened` / `reopen_if_needed_locked`, then releases `mu` before
    the MPP section; that section holds `mpp_api_mu` for the full slot pop → put_frame → push
    pending sequence.
  - `close()` / reopen: `cancel_io`, then `mu`, then `mpp_api_mu`, then free (same order as today’s
    `encoder_close_locked` intent).
  - Write the lock order as a comment next to the mutex declarations in `h264_encoder_mpp.hpp`.
  - **P7-T7 (M21):** Prefer implementing in the **same change** as C5 (one drain refactor). If
    split, M21 must not reintroduce `mpp_api_mu` → `mu`.
  **Test:** `h264_encoder_mpp_test` (`ENABLE_H264_ENCODER_MPP`, full config only): 3 threads — input
  loop, output loop, configure loop (`qp`, `gop`, `cbr`, `rc`) — for 5 s, then `close()`; must
  finish (watchdog 20 s). Run setup (skip, label, ASan, TSan + suppressions) per the decisions table.
- **P7-T2 · H7** — add `bool swap_chroma` param to `pack_yuv420sp_to_nv12` /
  `pack_yuv422sp_to_nv12`; pass true for `_VU` formats. **Test:** `pix_convert_test` with a
  2×2 NV21 input → NV12 output bytes.
- **P7-T3 · L9** — `pix_convert`: when `src_w < dst_w`, replicate the last column (luma and UV
  pairs) to the right edge, like rows. Then remove the per-frame `memset` in decoders.
  **Test:** 4×2 source into 8×4 destination → no zero bytes.
- **P7-T4 · M18** — encoder key `idr` (value ignored) → next frame gets `KEY_INPUT_IDR_REQ`
  (MPP). Cedar and Intel (both libav): set `pict_type = AV_PICTURE_TYPE_I` on the next
  `AVFrame` (plus `AV_FRAME_FLAG_KEY` if the installed libavutil defines it; guard with `#ifdef`). Console verb `force_idr` in `channel_controller` → atomic
  pending flag applied on the encode thread (same pattern as `g_pending_console_qp`).
  Wiring to depay `need_idr` is a follow-up (not in this plan).
- **P7-T5 · M19** — keys `super_i_ratio` (default 6.0) and `super_p_ratio` (default 1.5);
  `0` disables super-frame re-encode. Threshold = ratio × average frame bytes. Applied via
  `apply_rc_cfg_locked`. Remove the `large_frame` special case.
- **P7-T6 · M20** — `h264_level_for_size(w, h, fps, kbps)` per H.264 Annex A, High profile
  (MaxBR × 1.25): choose the smallest level with `MaxFS ≥ ceil(w/16)×ceil(h/16)`,
  `MaxMBPS ≥ MaxFS_used × fps`, `MaxBR×1.25 ≥ kbps` (kbps = 0 → ignore).

  | level | MaxMBPS | MaxFS | MaxBR (kbps) |
  |------:|--------:|------:|-------------:|
  | 1.0 | 1485 | 99 | 64 |
  | 1.1 | 3000 | 396 | 192 |
  | 1.2 | 6000 | 396 | 384 |
  | 1.3 | 11880 | 396 | 768 |
  | 2.0 | 11880 | 396 | 2000 |
  | 2.1 | 19800 | 792 | 4000 |
  | 2.2 | 20250 | 1620 | 4000 |
  | 3.0 | 40500 | 1620 | 10000 |
  | 3.1 | 108000 | 3600 | 14000 |
  | 3.2 | 216000 | 5120 | 20000 |
  | 4.0 | 245760 | 8192 | 20000 |
  | 4.1 | 245760 | 8192 | 50000 |
  | 4.2 | 522240 | 8704 | 50000 |
  | 5.0 | 589824 | 22080 | 135000 |
  | 5.1 | 983040 | 36864 | 240000 |
  | 5.2 | 2073600 | 36864 | 240000 |

  **Test:** 416×240@30 kbps=0 → 1.3, 416×240@30 kbps=2000 → 2.0, 1280×720@30 → 3.1, 1280×720@60 → 3.2,
  1920×1080@30 → 4.0, 1920×1080@60 → 4.2, 3840×2160@30 → 5.1. (Make the function a free function
  in a small header so it is unit-testable without MPP.)
- **P7-T7 · M21** — `drain_packets_locked(timeout)`: return as soon as ≥ 1 packet was ingested
  and the next poll is empty.
- **P7-T8 · M22** — `h264_decoder_mpp`:
  - `output(timeout)`: poll MPP without holding `mu` (copy ctx/mpi under `mu`, use a separate
    `mpp_io_mu` for MPP calls, same lock-order rule as C5: `mu` → `mpp_io_mu`).
  - Capture timestamps: map `pts → capture_mono_ns` in a small ring (64 entries) filled on
    `input()`, looked up on output by `mpp_frame_get_pts`.
  - Key `output_size_mode` = `config` (default, current behavior: crop/pad to `size`) |
    `stream` (output dims = decoded `src_w×src_h`, `size` ignored; output buffer pool resized on change).
  - Remove `g_mpp_hw_mu` from `stream_sdl.cpp` (decision table).
- **P7-T9 · M24** — `jpeg_decoder_multicore`: key `worker_cpus` = cpulist (comma list and/or
  ranges, e.g. `4-7`); worker *i* pins to `list[i % list.size()]`; keep `worker_cpu` as alias for a
  single CPU. `stream_sdl`: implement `VSTREAMER_CPU_MAP` per the decisions table (parser in a small
  free function with a unit test: defaults, override one stage, `-1`, ranges, malformed input →
  warning + defaults); replace the `k_cpu_*` constants with the parsed map; pass `jpeg_workers` to
  the decoder as `worker_cpus` and its size as `workers`. `jpeg_stage_main`: keep up to `workers`
  jobs in flight — submit while `input()` returns 0, drain with `output(…, 0)`, block with
  `output(…, 50)` only when the job queue is full. **Accept:** with `--source /dev/video0` (if a
  camera is attached) or the MJPEG noise path, `top -H` shows JPEG workers on CPUs 4–7; JPEG
  stage fps ≥ before (note numbers in the commit message).

**P7 gate:** §1.2 + §1.3 (tests that don't need MPP) + `ctest --test-dir out/tsan-hw -L hw` + §1.4.
After land, verify §1.4 decode→present path; if `dec_in` stays 0, check `h264_decoder_mpp` does not
drain while holding `mu` (re-entrant lock on `fetch_one_mpp_frame`).

---

## P8 — App and tooling

- **P8-T1 · H4** — `decode_thread_main`: on `-EAGAIN` keep the AU in a local `holding` slot and
  retry it before popping a new one (mirror `encode_stage_main`). Never re-push to `rx_au_queue`.
- **P8-T2 · M25** — `submit_nv12_to_encoder`: on `-EAGAIN` pull AUs, **forward them immediately
  in pull order**, then sleep 200 µs before retrying; remove the separate `enc_pending` batch and
  the `attempt & 7` drain. Cap attempts by time (100 ms), not count.
- **P8-T3 · H6** — `mkv_sink` **(review ID H6; phase P8, not P7 — do not confuse with P7-T2 H7
  chroma / `pix_convert`):**
  - After `avformat_write_header`, rescale: `av_packet_rescale_ts(pkt, AVRational{1, fps}, st->time_base)`.
  - PTS from `frame_data::pts` (frame index at `fps`); enforce strictly increasing mux PTS
    (`mux_pts = max(relative_pts, last_mux_pts + 1)`).
  - **Per-segment timeline:** on each new segment file, set `segment_pts_base` from the first frame’s
    `f.pts` so a continuous source counter (e.g. 30…59 after resize) maps to 0… in the new file.
  - `configure("output")` stores the **template** path; written files are `<stem>-NNN<ext>` only —
    never reopen the template path. `query("output")` → template; `query("segment")` → active file.
  - On resize (or `configure("fps")` while recording), `stop_locked()` then next `start_locked()`
    increments segment index and opens the next numbered file.
  - **Dropped frames:** timeline follows source `pts` gaps; monotonic clamp handles equal/backward
    PTS only (no synthetic fill for missing indices).
  - **Orthogonal to P7 C5:** affects record/MJPEG mux only, not the live `stream_sdl` glass path.
  **Test:** `mkv_sink_test` (needs libavformat; full + rover configs): 60 MJPEG frames at 30 fps,
  resize at frame 30 → `clip-001.mkv` and `clip-002.mkv` exist, template path not used; each
  segment ≈ 1.0 s ± 0.15 s via libavformat packet span (30 frames per segment). Optional follow-up
  smoke: loopback record with real noise MJPEG (not required for P8 gate).
- **P8-T4 · M26** — `channel_controller`: new `--chan-bind ADDR` option in `stream_sdl`
  (default `127.0.0.1`) used for the console **and** relay ingress sockets.
  Update `print_usage`.
- **P8-T5 · M27** — `scripts/docs_server.py`: token required for `PUT /api/doc/…`.
  Token = `--token` arg, else env `VSTREAMER_DOCS_TOKEN`, else `secrets.token_urlsafe(24)`
  generated at startup and printed once. The rendered page embeds it as a JS constant; the editor
  sends header `X-Docs-Token`; server compares with `hmac.compare_digest`, else `403`.
  (Custom header also forces a CORS preflight, which Flask does not grant.)
  **Test:** `scripts/test_docs_server.py` with Flask test client: PUT without/with wrong token →
  403; with token → 200; GET still works without token.
- **P8-T6 · M28** — `v4l2_source`: `capture_mono_ns` from `buf.timestamp`: if
  `(buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`, convert timeval → ns (same clock as
  `steady_clock` on Linux = `CLOCK_MONOTONIC`); else fall back to `steady_mono_ns()`. Release `mu`
  during `select()` (copy fd under lock; re-lock for DQBUF/QBUF; handle close-during-select via
  the existing `interrupt_shutdown`).
- **P8-T7 · L5** — `metrics`: single `std::vector<std::pair<std::string, std::shared_ptr<metric>>>`
  (insertion order) + `std::unordered_map<std::string, size_t>` index; `get_metric(const
  std::string &name)` creates with default value `uint64_t{0}`; delete `g_metric_seed` and the
  seed parameter; update all call sites in `stream_sdl.cpp`.

**P8 gate:** §1.2 + §1.4 (incl. `--self-test`).

---

## P9 — Documentation

- **P9-T1 · D1** — rewrite `docs/vstreamer.md` from the code as it is after P8:
  component model (`component`, `component_source`/`_coder`/`_sink`, ports, `data_packet` +
  `shared_sized_buffer` immutability rule, `configure`/`query` contract), the component list with
  their keys (generate the key tables by reading each `configure`/`query`), threading/lock-order
  notes, the `stream_sdl` bench pipeline. Move everything not implemented (feedback plugin,
  config loader, core console, `Mp4Sink`, rover deployment, winject integration) into a final
  **"Roadmap / target design"** section with a status table (`planned` / `bench-only` /
  `not started`). Fix other `docs/component-*.md` files where they describe removed APIs
  (`fetch`, `write`, `EmitFn`).
- **P9-T2 · D2** — complete the build option table (all `ENABLE_*`, `VSTREAMER_BUILD_TESTS`,
  test-app options, MPP auto-detect behavior); update `README.md` build section with the §1.1
  commands.
- **P9-T3 · D3** — `stream_sdl.cpp` header: queue default comment matches `k_default_pipe_queue_depth`;
  list new options (`--chan-bind`, env `VSTREAMER_CPU_MAP`) and console verbs (`force_idr`).
- **P9-T4 · D4** — `docs/packet-model.md`: one diagram of the forward datagram **as configured by
  `stream_sdl`** (the bench pipeline creator), labelled per layer so it is clear which node owns
  which bytes: transport (`stream_sender`/`stream_receiver`: `stream_header_s (2) | FEC shard hdr (4)
  | len prefix (2, systematic only)`) wrapping an opaque payload, which in this pipeline is RTP
  (`rtp_h264_pay`/`depay`: `RTP hdr (12) | RTP ext (relative capture ts, 16) | H.264 payload`).
  State that the layers are independent (rule 10) and that sizes are set by the pipeline creator
  (`max_datagram`, `max_input` → `mtu`). Add the generic `sock_data.seq` note and the
  app-owned telemetry note (M13, P6.5-T5).
- **P9-T5** — update `aidocs/review.md`: add a "Status" column (done + commit hash) to the
  Decisions tables.

**P9 gate:** §1.2; docs links resolve (`scripts/docs_server.py` renders every page without error).

---

## Follow-ups explicitly out of scope
- Cross-host reverse telemetry datagram (M13 wire part).
- Wiring depay `need_idr` → encoder `idr` across the link (M18 feedback part).
- IPv6 for `stream_sender`/`stream_receiver` (C1 is IPv4-only).
- Offline/vendored ISA-L.
- **MKV loopback record smoke** (noise → `mkv_sink` on disk): optional; unit test is the P8-T3 gate.

## Recorded decisions (2026-10)

Cross-cutting only. P7 ordering and scope live in the P7 section; node independence in §0 rule 10 and P6.5.

| Topic | Decision |
|-------|----------|
| **H6 vs H7 naming** | Review **H6** = `mkv_sink` → **P8-T3**. Review **H7** = chroma swap in `pix_convert` → **P7-T2**. |
| **H6 gate** | `mkv_sink_test` per segment duration; full 60-frame single-file ≈2 s only when no mid-stream resize. |
| **P9 D1** | P7 closed (`aef51a9`); finish full `docs/vstreamer.md` rewrite + component key tables (P9-T1). |

## Open questions
_(Composer: append here instead of guessing; leave the task unimplemented.)_

- None. P6.5 not started; no open design questions for P6.5 or P7.
