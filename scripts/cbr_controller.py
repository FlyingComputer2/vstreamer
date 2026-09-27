#!/usr/bin/env python3
from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from utils import (  # noqa: E402
    Args,
    Clamp,
    Console,
    Delay,
    FecMap,
    LPF,
    Metrics,
)
from utils.plot import PlotCsv, PlotFanout, PlotWS  # noqa: E402
from utils.tune_console import TuneConsole  # noqa: E402

_TUNE_HELP = """\
commands (UDP text line):
  show | status
  record <file.csv>   start CSV log (plot series, one row per loop)
  stop                stop CSV log
  help"""


def _make_tune_handler(plot_csv: PlotCsv) -> Callable[[str], str]:
    def handle(line: str) -> str:
        parts = line.strip().split()
        if not parts:
            return "ok"
        cmd = parts[0].lower().replace("_", "-")
        if cmd in ("help", "?"):
            return _TUNE_HELP
        if cmd in ("show", "status"):
            rec = plot_csv.path or "-"
            return f"record={rec}"
        if cmd == "record":
            if len(parts) < 2:
                return "usage: record <file.csv>"
            return plot_csv.start(" ".join(parts[1:]))
        if cmd == "stop":
            return plot_csv.stop()
        return f"unknown command: {line}"

    return handle


def _stop_other_controllers() -> None:
    """Only one local controller should publish to PlotJuggler."""
    me = os.getpid()
    try:
        out = subprocess.check_output(
            ["pgrep", "-f", "scripts/cbr_controller.py"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except subprocess.CalledProcessError:
        return
    for token in out.split():
        pid = int(token)
        if pid != me:
            os.kill(pid, signal.SIGTERM)


def main() -> int:
    a = Args(sys.argv[1:])
    a.default("plot-url", "")
    a.default("host", "127.0.0.1")
    a.default("port", 5090)
    a.default("timeout", 2.5)
    a.default("udp-loss-filter-cutoff", 0.01)
    a.default("fec-loss-filter-cutoff", 0.01)
    a.default("encode-rate-filter-cutoff", 5)
    a.default("encode-rate-filter-cutoff-long", 0.01)
    a.default("k", 6)
    a.default("n-gain", 1.2)
    a.default("n-max", 15)
    a.default("cbr-min", 100)
    a.default("cbr-max", 20000)
    a.default("loop-rate", 10.0)
    a.default("tune-host", "127.0.0.1")
    a.default("tune-port", 5092)
    a.default("ramp-rate-kbpsps", 50.0)
    a.default("fall-rate-pct", 5.0)
    a.default("rate-stable-eps-kbps", 300.0)
    a.default("rate-stable-time-s", 5.0)
    a.default("probe-interval-s", 30.0)
    a.default("probe-duration-s", 5.0)

    _stop_other_controllers()

    loop_rate = a("loop-rate")
    dt = 1.0 / loop_rate

    plot_csv = PlotCsv()
    plot = PlotFanout(PlotWS(a("plot-url")), plot_csv)
    metric = Metrics(a("host"), a("port"), a("timeout"))
    console = Console(a("host"), a("port"), a("timeout"))
    lpf_udp_loss = LPF(loop_rate, a("udp-loss-filter-cutoff"))
    lpf_fec_loss = LPF(loop_rate, a("fec-loss-filter-cutoff"))
    lpf_encode_rate = LPF(loop_rate, a("encode-rate-filter-cutoff"))
    lpf_encode_rate_long = LPF(loop_rate, a("encode-rate-filter-cutoff-long"))
    fec_map = FecMap(a("k"), a("n-gain"))
    fec_clamp = Clamp(a("k"), a("n-max"))
    cbr_clamp = Clamp(a("cbr-min"), a("cbr-max"))
    tune = TuneConsole(a("tune-host"), a("tune-port"), _make_tune_handler(plot_csv))
    d_udp_recv = Delay(1)
    d_fec_recv = Delay(1)
    d_udp_gap = Delay(1)
    d_fec_gap = Delay(1)
    d_encode = Delay(1)
    loss_min_total = 8
    ramp_rate_kbpsps = float(a("ramp-rate-kbpsps"))
    fall_rate_pct = float(a("fall-rate-pct"))
    cbr_max_abs = float(a("cbr-max"))
    rate_stable_eps = float(a("rate-stable-eps-kbps"))
    rate_stable_time_s = float(a("rate-stable-time-s"))
    probe_interval_s = float(a("probe-interval-s"))
    probe_duration_s = float(a("probe-duration-s"))
    console.line(f"set_fec_k {a('k')}")
    metric.refresh()
    s_cbr0 = metric("h264_encoder.cbr_kbps")
    cbr_kbps = float(s_cbr0) if s_cbr0 is not None else float(a("cbr-min"))
    cbr_kbps = cbr_clamp(cbr_kbps)

    last_metrics_t: float | None = metric.timestamp
    rate_stable = False
    rate_stable_accum_s = 0.0
    next_probe_at: float | None = None
    probing_until: float | None = None

    try:
        while True:
            tune.poll()

            loop_start = time.monotonic()
            if not metric.refresh() or metric.timestamp is None:
                time.sleep(max(0.0, dt - (time.monotonic() - loop_start)))
                continue

            if last_metrics_t is None:
                last_metrics_t = metric.timestamp
                time.sleep(max(0.0, dt - (time.monotonic() - loop_start)))
                continue

            metrics_dt = metric.timestamp - last_metrics_t
            last_metrics_t = metric.timestamp
            plot.set_time(time.time())

            # metrics
            s_udp_recv      = metric("stream_sender.peer_udp_packet_received")
            s_peer_loss_udp = metric("stream_sender.peer_loss_udp_pct")
            s_fec_recv = metric("stream_sender.peer_fec_packet_received")
            s_udp_gap  = metric("stream_sender.peer_udp_gap_count")
            s_fec_gap  = metric("stream_sender.peer_fec_gap_count")
            s_encoded  = metric("h264_encoder.out_bytes")
            s_cbr      = metric("h264_encoder.cbr_kbps")

            # deltas (counters refreshed from atomics on each metrics UDP poll)
            udp_received_delta_ = (
                s_udp_recv - d_udp_recv(s_udp_recv) if s_udp_recv is not None else 0.0
            )
            fec_received_delta_ = (
                s_fec_recv - d_fec_recv(s_fec_recv) if s_fec_recv is not None else 0.0
            )
            udp_gap_delta_ = s_udp_gap - d_udp_gap(s_udp_gap) if s_udp_gap is not None else 0.0
            fec_gap_delta_ = s_fec_gap - d_fec_gap(s_fec_gap) if s_fec_gap is not None else 0.0
            encoded_delta_ = (
                s_encoded - d_encode(s_encoded) if s_encoded is not None else 0.0
            )
            encoded_rate_raw_ = (
                encoded_delta_ / metrics_dt * 8.0 / 1000.0 if metrics_dt > 0.0 else 0.0
            )

            # losses (wire UDP: gaps / (gaps + received); matches stream_sdl interval_loss_pct)
            udp_total_ = udp_gap_delta_ + udp_received_delta_
            fec_total_ = fec_gap_delta_ + fec_received_delta_
            if s_udp_recv is not None:
                loss_udp_raw_ = (
                    udp_gap_delta_ / udp_total_
                    if udp_total_ >= loss_min_total
                    else 0.0
                )
            elif s_peer_loss_udp is not None:
                loss_udp_raw_ = s_peer_loss_udp / 100.0
            else:
                loss_udp_raw_ = 0.0
            loss_fec_raw_ = (
                fec_gap_delta_ / fec_total_ if fec_total_ >= loss_min_total else 0.0
            )

            loss_udp_ = lpf_udp_loss(loss_udp_raw_)
            loss_fec_ = lpf_fec_loss(loss_fec_raw_)
            encoded_rate_ = lpf_encode_rate(encoded_rate_raw_)
            encoded_rate_long_ = lpf_encode_rate_long(encoded_rate_raw_)

            plot("s_cbr", s_cbr)
            plot("metrics_dt", metrics_dt)

            plot("loss_udp_raw", loss_udp_raw_)
            plot("loss_fec_raw", loss_fec_raw_)
            plot("encoded_rate_raw", encoded_rate_raw_)

            plot("loss_udp", loss_udp_)
            plot("loss_fec", loss_fec_)
            plot("encoded_rate", encoded_rate_)
            plot("encoded_rate_long", encoded_rate_long_)

            plot("fec_gap_delta", fec_gap_delta_)
            plot("udp_gap_delta", udp_gap_delta_)

            # calculate FEC N
            fec_n_ = fec_clamp(fec_map(loss_udp_))
            plot("fec_n", fec_n_)
            console.line(f"set_fec_n {fec_n_}")

            # AIMD on FEC gaps (fec_gap_delta_ > 0 => loss this interval)
            is_loss_ = fec_gap_delta_ > 0.0
            plot("is_loss", float(is_loss_))

            if is_loss_:
                cbr_kbps *= 1.0 - fall_rate_pct / 100.0
            else:
                cbr_kbps += ramp_rate_kbpsps * metrics_dt
            cbr_kbps = cbr_clamp(cbr_kbps)
            plot("cbr", cbr_kbps)
            console("set_encode_cbr", cbr_kbps)

            plot.flush()
            time.sleep(max(0.0, dt - (time.monotonic() - loop_start)))
    finally:
        tune.close()
        plot.close()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
