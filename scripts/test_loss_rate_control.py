#!/usr/bin/env python3
"""Offline check of LossRateControl against a simulated capacity-limited link.

  python3 scripts/test_loss_rate_control.py

The link drops blocks whenever the wire rate (cbr * n/k) exceeds capacity; the
residual gap count reaches the controller after a reverse-telemetry delay.
"""

from __future__ import annotations

import sys
from collections import deque
from collections.abc import Callable
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from utils import Clamp, LossRateControl, telemetry_stale  # noqa: E402

LOOP_RATE = 100.0
DT = 1.0 / LOOP_RATE
TELEMETRY_DELAY_S = 0.2
FEC_RATIO = 8.0 / 6.0
BLOCKS_PER_S_PER_KBPS = 0.02

Step = Callable[[float, float, float], float]


def _make_ctl(ramp: float, fall: float, cbr0: float = 1000.0) -> LossRateControl:
    return LossRateControl(
        cbr0,
        Clamp(100.0, 20000.0),
        ramp_kbpsps=ramp,
        ramp_slow_kbpsps=100.0,
        fall_rate_pct=fall,
        hold_s=1.0,
        near_loss_margin_pct=10.0,
        gap_min=1,
    )


def _simulate(step: Step, cbr0: float, capacity: Callable[[float], float], duration_s: float) -> list[float]:
    """Return the cbr trace; capacity(t) is in encoded (pre-FEC) kbps."""
    delay = deque([0.0] * int(round(TELEMETRY_DELAY_S * LOOP_RATE)))
    cbr = cbr0
    lost_blocks = 0.0
    trace: list[float] = []
    t = 0.0
    while t < duration_s:
        excess_kbps = max(0.0, (cbr - capacity(t)) * FEC_RATIO)
        lost_blocks += excess_kbps * BLOCKS_PER_S_PER_KBPS * DT
        whole = float(int(lost_blocks))
        lost_blocks -= whole
        delay.append(whole)
        gaps = delay.popleft()
        cbr = step(gaps, DT, t)
        trace.append(cbr)
        t += DT
    return trace


def _mean(xs: list[float]) -> float:
    return sum(xs) / len(xs)


def _window(trace: list[float], t0: float, t1: float) -> list[float]:
    return trace[int(t0 * LOOP_RATE) : int(t1 * LOOP_RATE)]


def check_steady_state(
    step: Step, cbr0: float, cap: float, label: str, warmup_s: float = 60.0
) -> list[str]:
    trace = _simulate(step, cbr0, lambda _t: cap, warmup_s + 60.0)
    m = _mean(_window(trace, warmup_s, warmup_s + 60.0))
    peak = max(_window(trace, warmup_s, warmup_s + 60.0))
    ok = 0.8 * cap <= m <= cap and peak <= 1.1 * cap
    print(f"[{'ok' if ok else 'FAIL'}] {label}: steady mean={m:.0f} peak={peak:.0f} cap={cap:.0f}")
    return [] if ok else [label]


def check_single_cut(ramp: float, fall: float, label: str) -> list[str]:
    ctl = _make_ctl(ramp, fall, cbr0=5000.0)
    ctl._ramp = 0.0
    ctl._ramp_slow = 0.0
    cuts = 0
    t = 0.0
    burst_len = int(0.5 * LOOP_RATE)
    for i in range(int(3.0 * LOOP_RATE)):
        ctl.step(3.0 if 10 <= i < 10 + burst_len else 0.0, DT, t)
        cuts += int(ctl.loss_event)
        t += DT
    ok = cuts == 1 and abs(ctl.cbr - 5000.0 * (1 - fall / 100.0)) < 1e-6
    print(f"[{'ok' if ok else 'FAIL'}] {label}: 0.5 s burst -> cuts={cuts} cbr={ctl.cbr:.0f}")
    return [] if ok else [label]


def check_recovery(ramp: float, fall: float, label: str) -> list[str]:
    ctl = _make_ctl(ramp, fall)
    trace = _simulate(ctl.step, 1000.0, lambda t: 4000.0 if t < 60.0 else 8000.0, 180.0)
    m = _mean(_window(trace, 150.0, 180.0))
    ok = 0.8 * 8000.0 <= m <= 8000.0
    print(f"[{'ok' if ok else 'FAIL'}] {label}: capacity 4000->8000, mean after={m:.0f}")
    return [] if ok else [label]


def check_stale_age_flags() -> list[str]:
    ok_neg = telemetry_stale(-1.0, 100.0)
    ok_old = telemetry_stale(1000.0, 100.0)
    ok_missing = telemetry_stale(None, 100.0)
    ok_fresh = not telemetry_stale(50.0, 100.0)
    ok_edge = not telemetry_stale(300.0, 100.0) and telemetry_stale(301.0, 100.0)
    ok = ok_neg and ok_old and ok_missing and ok_fresh and ok_edge
    print(f"[{'ok' if ok else 'FAIL'}] stale age flags (-1, 1000, missing, 50, 300/301 ms)")
    return [] if ok else ["stale-age-flags"]


def check_stale_telemetry_holds_increase() -> list[str]:
    ctl = _make_ctl(50.0, 5.0)
    before = ctl.cbr
    ctl.step(0.0, 1.0, 0.0, allow_increase=False)
    held = abs(ctl.cbr - before) < 1e-6
    ctl.step(0.0, 1.0, 1.0, allow_increase=True)
    increased = ctl.cbr > before
    ok = held and increased
    print(
        f"[{'ok' if ok else 'FAIL'}] stale telemetry blocks increase "
        f"(held={held} increased={increased})"
    )
    return [] if ok else ["stale-hold"]


def check_restart_reset() -> list[str]:
    ctl = _make_ctl(50.0, 5.0, cbr0=5000.0)
    ctl.step(-12345.0, DT, 0.0)
    ok = not ctl.loss_event
    print(f"[{'ok' if ok else 'FAIL'}] negative gap delta (peer restart) is not a loss event")
    return [] if ok else ["restart"]


def main() -> int:
    failures: list[str] = []
    for ramp, fall in ((50.0, 5.0), (3000.0, 33.0)):
        tag = f"ramp={ramp:g} fall={fall:g}%"
        failures += check_single_cut(ramp, fall, f"single cut {tag}")
        for cap in (2000.0, 5000.0, 12000.0):
            cbr0 = 0.7 * cap
            ctl = _make_ctl(ramp, fall, cbr0=cbr0)
            warmup_s = (cap - cbr0) / ramp + 60.0
            failures += check_steady_state(ctl.step, cbr0, cap, f"steady {tag} cap={cap:g}", warmup_s)
        failures += check_recovery(ramp, fall, f"recovery {tag}")
    failures += check_restart_reset()
    failures += check_stale_age_flags()
    failures += check_stale_telemetry_holds_increase()

    # The previous controller never reacted to loss: a pure ramp must fail the steady-state check.
    def loss_blind(_gaps: float, dt: float, _now: float, s={"cbr": 1000.0}) -> float:
        s["cbr"] = min(20000.0, s["cbr"] + 3000.0 * dt)
        return s["cbr"]

    if not check_steady_state(loss_blind, 1000.0, 5000.0, "loss-blind ramp (expected FAIL)"):
        failures.append("test cannot fail: loss-blind controller passed")

    print("PASS" if not failures else f"FAILED: {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
