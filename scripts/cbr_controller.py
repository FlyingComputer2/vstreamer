#!/usr/bin/env python3
from __future__ import annotations

import os
import signal
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
    GapLoss,
    LossRateControl,
    telemetry_stale,
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


def _is_controller_argv(argv: list[str]) -> bool:
    """True only for `python[3] [-flags] .../cbr_controller.py ...`, not for a shell or wrapper
    whose command line merely mentions the script."""
    if not argv or not os.path.basename(argv[0]).startswith("python"):
        return False
    for arg in argv[1:]:
        if arg.startswith("-"):
            continue
        return os.path.basename(arg) == "cbr_controller.py"
    return False


def _stop_other_controllers() -> None:
    """Only one local controller should publish to PlotJuggler."""
    me = os.getpid()
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) == me:
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as f:
                argv = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
        except OSError:
            continue
        if _is_controller_argv(argv):
            try:
                os.kill(int(entry), signal.SIGTERM)
            except OSError:
                pass


def main() -> int:
    a = Args(sys.argv[1:])
    a.default("plot-url", "")
    a.default("host", "127.0.0.1")
    a.default("port", 5090)
    a.default("timeout", 2.5)
    a.default("udp-loss-filter-cutoff", 0.1)
    a.default("udp-loss-long-filter-cutoff", 0.005)
    a.default("fec-loss-filter-cutoff", 0.1)
    a.default("encode-rate-filter-cutoff", 5)
    a.default("encode-rate-filter-cutoff-long", 0.1)
    a.default("k", 6)
    a.default("n-gain", 1.2)
    a.default("n-max", 24)
    a.default("cbr-min", 100)
    a.default("cbr-max", 20000)
    a.default("loop-rate", 10.0)
    a.default("tune-host", "127.0.0.1")
    a.default("tune-port", 5092)
    a.default("ramp-rate-kbpsps", 50.0)
    a.default("fall-rate-pct", 5.0)
    a.default("ramp-slow-kbpsps", 100.0)
    a.default("hold-s", 1.0)
    a.default("near-loss-margin-pct", 10.0)
    a.default("fec-gap-min", 1)
    a.default("telemetry-ms", 100)

    _stop_other_controllers()

    loop_rate = a("loop-rate")
    dt = 1.0 / loop_rate

    plot_csv = PlotCsv()
    plot = PlotFanout(PlotWS(a("plot-url")), plot_csv)
    metric = Metrics(a("host"), a("port"), a("timeout"))
    console = Console(a("host"), a("port"), a("timeout"))
    udp_loss = GapLoss(loop_rate, a("udp-loss-filter-cutoff"))
    udp_loss_long = GapLoss(loop_rate, a("udp-loss-long-filter-cutoff"))
    fec_loss = GapLoss(loop_rate, a("fec-loss-filter-cutoff"))
    lpf_encode_rate = LPF(loop_rate, a("encode-rate-filter-cutoff"))
    lpf_encode_rate_long = LPF(loop_rate, a("encode-rate-filter-cutoff-long"))
    fec_map = FecMap(a("k"), a("n-gain"))
    fec_clamp = Clamp(a("k"), a("n-max"))
    cbr_clamp = Clamp(a("cbr-min"), a("cbr-max"))
    tune = TuneConsole(a("tune-host"), a("tune-port"), _make_tune_handler(plot_csv))
    d_encode = Delay(1)
    console.line(f"set_fec_k {a('k')}")
    metric.refresh()
    s_cbr0 = metric("h264_encoder.cbr_kbps")
    cbr0 = float(s_cbr0) if s_cbr0 is not None else float(a("cbr-min"))
    telemetry_ms = float(a("telemetry-ms"))
    stale_hold = False
    peer_session_prev: float | None = None

    rate_ctl = LossRateControl(
        cbr0,
        cbr_clamp,
        ramp_kbpsps=float(a("ramp-rate-kbpsps")),
        ramp_slow_kbpsps=float(a("ramp-slow-kbpsps")),
        fall_rate_pct=float(a("fall-rate-pct")),
        hold_s=float(a("hold-s")),
        near_loss_margin_pct=float(a("near-loss-margin-pct")),
        gap_min=float(a("fec-gap-min")),
    )

    last_metrics_t: float | None = metric.timestamp

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
            # @note you are not allowed to add or remove any metrics
            s_udp_recv = metric("stream_sender.peer_udp_packet_received")
            s_fec_recv = metric("stream_sender.peer_fec_packet_received")
            s_udp_gap  = metric("stream_sender.peer_udp_gap_count")
            s_fec_gap  = metric("stream_sender.peer_fec_gap_count")
            s_report_age = metric("stream_sender.peer_report_age_ms")
            s_peer_session = metric("stream_sender.peer_session")
            s_encoded  = metric("h264_encoder.out_bytes")
            s_cbr      = metric("h264_encoder.cbr_kbps")

            if s_peer_session is not None and peer_session_prev is not None:
                if s_peer_session != peer_session_prev:
                    udp_loss.reset()
                    fec_loss.reset()
            if s_peer_session is not None:
                peer_session_prev = s_peer_session

            stale = telemetry_stale(s_report_age, telemetry_ms)
            if stale and not stale_hold:
                print(f"stale telemetry (age={s_report_age} ms), holding", flush=True)
                stale_hold = True
            if not stale and stale_hold:
                print("telemetry recovered", flush=True)
                stale_hold = False

            encoded_delta_ = s_encoded - d_encode(s_encoded) if s_encoded is not None else 0.0
            encoded_rate_raw_ = encoded_delta_ / metrics_dt * 8.0 / 1000.0 if metrics_dt > 0.0 else 0.0

            udp_loss.step(metrics_dt, s_udp_recv, s_udp_gap)
            udp_loss_long.step(metrics_dt, s_udp_recv, s_udp_gap)

            fec_loss.step(metrics_dt, s_fec_recv, s_fec_gap)
            udp_gap_delta_ = udp_loss.gap_delta
            fec_gap_delta_ = fec_loss.gap_delta
            loss_udp_raw_ = udp_loss.loss_raw
            loss_fec_raw_ = fec_loss.loss_raw
            loss_udp_ = udp_loss.loss
            loss_udp_long_ = udp_loss_long.loss
            loss_fec_ = fec_loss.loss
            encoded_rate_ = lpf_encode_rate(encoded_rate_raw_)
            encoded_rate_long_ = lpf_encode_rate_long(encoded_rate_raw_)

            plot("s_cbr", s_cbr)
            plot("metrics_dt", metrics_dt)

            plot("loss_udp_raw", loss_udp_raw_)
            plot("loss_fec_raw", loss_fec_raw_)
            plot("encoded_rate_raw", encoded_rate_raw_)

            plot("loss_udp", loss_udp_)
            plot("loss_udp_long", loss_udp_long_)
            plot("loss_fec", loss_fec_)
            plot("encoded_rate", encoded_rate_)
            plot("encoded_rate_long", encoded_rate_long_)

            plot("fec_gap_delta", fec_gap_delta_)
            plot("udp_gap_delta", udp_gap_delta_)

            # calculate FEC N
            fec_n_ = fec_clamp(fec_map(udp_loss_long.loss))
            plot("fec_n", fec_n_)
            console.line(f"set_fec_n {int(round(fec_n_))}")

            # calculate CBR (receiver counter reset on peer restart gives a negative delta)
            cbr_kbps = rate_ctl.step(
                max(0.0, fec_gap_delta_),
                metrics_dt,
                time.monotonic(),
                allow_increase=not stale,
            )
            plot("loss_event", float(rate_ctl.loss_event))
            plot("is_holding", float(rate_ctl.is_holding))
            plot("cbr_loss", rate_ctl.cbr_loss if rate_ctl.cbr_loss is not None else 0.0)
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
