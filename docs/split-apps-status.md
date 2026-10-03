# split-apps status (after merge to `main`)

Executable spec (local, not in git): `aidocs/split-streamer.md`. This file tracks what is
**done** vs **open** on the `split-apps` workstream.

## Shipped on `split-apps`

- `uvc_stream_sender`, `sdl_stream_receiver`, `apps_common` (TX/RX halves, `source_selector`,
  `pipeline_controller`, `app_console`, `tx_console`, `tx_stages` / `rx_stages`, `tx_metrics` /
  `rx_metrics`).
- Loopback bench: **`stream_sdl_test`** binary under `src/apps/stream_sdl_test/` (CMake target
  `stream_sdl_test`). Console **metric names** remain `stream_sdl.*` (D6).
- `tests/split_apps_test` (hw); `ctest out/full` 102/102 on bench host.
- All three production/bench apps use `pipeline_controller` with `bind_legacy_run(&g_run)`.

## Open items (backlog)

| ID | Area | What remains |
|----|------|----------------|
| O1 | **D9 state** | `g_tx` / `g_rx` in `pipeline_state`; `g_run` legacy-synced, not owned per `main`. |
| O2 | **Metrics** | Slim `update_pipeline_metrics`; more logic in `tx_metrics` / `rx_metrics` vs bench-only. |
| O3 | **Linking** | Split apps still link `vstreamer_bench_pipeline` for metrics registry. |
| O4 | **§3 gate** | Four-config rebuild + warning table; full console baseline; FEC/link manual smoke. |
| O5 | **SA-T5/T6** | Manual two-host and camera/noise verification per spec. |
| O6 | **SA-T0** | Move `rs_fec_test`, `rs_block_id_pace_test`, `noise_fft_bench` to `src/apps/`. |
| O7 | **TSan** | `SplitAppsTest` skipped under TSan (`noise_source` OMP); fix or accept policy. |
| O8 | **Docs** | Stale `camera_noise_mux` references; SA-T8 two-host/winject; `implementation-plan` gs. |
| O9 | **Out of scope** | `--self-test` QP behavior; automated `cbr_controller.py` gate; packaging. |

## Out of scope (unchanged)

- `stream_sdl_test --self-test` (QP under loss).
- Automated `scripts/cbr_controller.py` in CI for split-apps (manual runs only).
