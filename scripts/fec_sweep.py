#!/usr/bin/env python3
"""FEC performance sweep: sender FEC k/n x channel_sim impairment -> residual loss at the receiver.

The sender is anything that speaks the vstreamer mplane (the Linux vstreamer app or the ESP32-P4
live TX example); channel_sim sits between it and the receiver; the receiver is
sdl_stream_receiver (its UDP console `metrics`). For each point the script sets the sender's FEC,
the channel's loss, waits --settle seconds, zeroes the channel counters, and measures --duration
seconds.

  python3 scripts/fec_sweep.py --sender 192.168.253.28:5090 \\
      --fec none,10/12,8/12 --loss 0,1,5 --ge 1:25,2:20 --duration 20 --csv out.csv

Columns: chan% is the channel's forward drop rate (all models); resid% is RTP packets missing
after FEC (sequence gaps at the depacketizer) over RTP packets expected; fps is frames presented;
idr is depacketizer resyncs (waits for an IDR); cpu is the sender's busiest task when it answers
`get sys stats` (P4 only).
"""

from __future__ import annotations

import argparse
import csv
import re
import socket
import sys
import time
from dataclasses import dataclass


def ask(addr: tuple[str, int], line: str, timeout: float = 1.0) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto((line + "\n").encode(), addr)
        data, _ = s.recvfrom(65536)
    return data.decode(errors="replace")


def ask_ok(addr: tuple[str, int], line: str) -> str:
    for attempt in range(3):
        try:
            reply = ask(addr, line).strip()
        except socket.timeout:
            if attempt == 2:
                raise
            continue
        if not reply.startswith("ok"):
            raise RuntimeError(f"{addr[0]}:{addr[1]} '{line}': {reply}")
        return reply[2:].strip()
    raise AssertionError


def host_port(s: str) -> tuple[str, int]:
    host, _, port = s.rpartition(":")
    return (host or "127.0.0.1", int(port))


def kv(s: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=(\S+)", s))


def receiver_metrics(addr: tuple[str, int]) -> dict[str, float]:
    out: dict[str, float] = {}
    for line in ask(addr, "metrics", timeout=2.0).splitlines():
        name, sep, value = line.partition(" = ")
        if not sep:
            continue
        try:
            out[name.strip()] = float(value)
        except ValueError:
            pass
    return out


@dataclass
class channel_point:
    label: str
    loss: float = 0.0
    ge: tuple[float, float, float] | None = None  # p_gb, p_bg, loss_bad


@dataclass
class fec_point:
    label: str
    k: int = 0
    n: int = 0


@dataclass
class result:
    fec: str
    channel: str
    chan_pct: float
    resid_pct: float
    fps: float
    recovered: int
    failures: int
    idr: int
    cpu: str


def parse_fec(spec: str) -> list[fec_point]:
    pts = []
    for item in spec.split(","):
        item = item.strip()
        if item == "none":
            pts.append(fec_point("none"))
            continue
        k, n = (int(x) for x in item.split("/"))
        if not 1 <= k <= n <= 31:
            raise SystemExit(f"bad fec {item}: need 1 <= k <= n <= 31")
        pts.append(fec_point(item, k, n))
    return pts


def parse_channels(loss: str, ge: str) -> list[channel_point]:
    pts = [channel_point(f"loss {float(x):g}%", loss=float(x)) for x in loss.split(",") if x]
    for item in (x for x in ge.split(",") if x):
        vals = [float(v) for v in item.split(":")]
        p_gb, p_bg = vals[0], vals[1]
        loss_bad = vals[2] if len(vals) > 2 else 100.0
        bad = p_gb / (p_gb + p_bg) * loss_bad
        pts.append(channel_point(f"ge {p_gb:g}/{p_bg:g}/{loss_bad:g} (~{bad:.1f}%, burst {100 / p_bg:.1f})",
                                 ge=(p_gb, p_bg, loss_bad)))
    return pts


def set_fec(sender: tuple[str, int], node: str, f: fec_point) -> None:
    if f.k == 0:
        ask_ok(sender, f"set {node} fec none")
        return
    # Keep k <= n at every step without passing through a high-overhead k/n (k = 1 with a large
    # n multiplies the sender's output): raise n before k, lower k before n.
    n0 = int(ask_ok(sender, f"get {node} fec_n"))
    order = (("fec_n", f.n), ("fec_k", f.k)) if f.n >= n0 else (("fec_k", f.k), ("fec_n", f.n))
    for key, value in order:
        ask_ok(sender, f"set {node} {key} {value}")


def set_channel(chan: tuple[str, int], c: channel_point) -> None:
    ask_ok(chan, f"set link loss {c.loss:g}")
    p_gb, p_bg, loss_bad = c.ge if c.ge else (0.0, 100.0, 100.0)
    ask_ok(chan, f"set link ge_p_bg {p_bg:g}")
    ask_ok(chan, f"set link ge_loss_bad {loss_bad:g}")
    ask_ok(chan, f"set link ge_p_gb {p_gb:g}")


def busiest_task(sys_stats: str) -> str:
    tasks = [(name, float(pct)) for name, pct in re.findall(r" (\w+)=([\d.]+)%", sys_stats)
             if not name.startswith("IDLE")]
    idle = re.findall(r"(IDLE\d)=([\d.]+)%", sys_stats)
    top = max(tasks, key=lambda t: t[1]) if tasks else None
    s = f"{top[0]} {top[1]:.0f}%" if top else ""
    if idle:
        s += " idle " + "/".join(f"{float(p):.0f}" for _, p in sorted(idle))
    return s


def measure(args, sender, chan, rcv, f: fec_point, c: channel_point) -> result:
    sys_ok = True
    try:
        ask_ok(sender, "get sys stats")  # opens the CPU window
    except (RuntimeError, socket.timeout):
        sys_ok = False
    m0 = receiver_metrics(rcv)
    ask_ok(chan, "set link stats reset")
    t0 = time.monotonic()
    time.sleep(args.duration)
    m1 = receiver_metrics(rcv)
    dt = time.monotonic() - t0
    st = kv(ask_ok(chan, "get link stats"))
    cpu = busiest_task(ask_ok(sender, "get sys stats")) if sys_ok else ""

    def d(name: str) -> float:
        return m1.get(name, 0.0) - m0.get(name, 0.0)

    fwd_in = int(st["fwd_in"])
    fwd_drop = sum(int(st[k]) for k in ("fwd_drop_rate", "fwd_drop_loss", "fwd_drop_burst",
                                         "fwd_drop_queue"))
    lost = d("rtp_h264_depay.rtp_lost")
    got = d("rtp_h264_depay.rtp_received")
    return result(
        fec=f.label,
        channel=c.label,
        chan_pct=100.0 * fwd_drop / fwd_in if fwd_in else 0.0,
        resid_pct=100.0 * lost / (lost + got) if lost + got > 0 else 0.0,
        fps=d("sdl_sink.in_frames") / dt,
        recovered=int(d("stream_receiver.fec_recovered")),
        failures=int(d("stream_receiver.fec_failures")),
        idr=int(d("rtp_h264_depay.need_idr")),
        cpu=cpu,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sender", required=True, help="sender mplane host:port")
    ap.add_argument("--sender-node", default="snd", help="stream_sender node name (default snd)")
    ap.add_argument("--channel", default="127.0.0.1:5095", help="channel_sim mplane")
    ap.add_argument("--receiver", default="127.0.0.1:5091", help="receiver console")
    ap.add_argument("--fec", default="none,10/12,8/12", help="none or k/n, comma separated")
    ap.add_argument("--loss", default="0,1,2,5", help="constant loss %%, comma separated")
    ap.add_argument("--ge", default="1:25,2:20",
                    help="Gilbert-Elliott p_gb:p_bg[:loss_bad] (%%), comma separated")
    ap.add_argument("--duration", type=float, default=20.0, help="seconds per point")
    ap.add_argument("--settle", type=float, default=3.0, help="seconds after a change")
    ap.add_argument("--csv", help="also write results here")
    args = ap.parse_args()

    sender, chan, rcv = host_port(args.sender), host_port(args.channel), host_port(args.receiver)
    fecs = parse_fec(args.fec)
    chans = parse_channels(args.loss, args.ge)

    hdr = f"{'fec':>6} {'channel':<36} {'chan%':>6} {'resid%':>7} {'fps':>5} {'recov':>6} {'fail':>5} {'idr':>4}  cpu"
    print(hdr)
    results: list[result] = []
    try:
        for f in fecs:
            set_fec(sender, args.sender_node, f)
            for c in chans:
                set_channel(chan, c)
                time.sleep(args.settle)
                r = measure(args, sender, chan, rcv, f, c)
                results.append(r)
                print(f"{r.fec:>6} {r.channel:<36} {r.chan_pct:6.2f} {r.resid_pct:7.3f} {r.fps:5.1f} "
                      f"{r.recovered:6d} {r.failures:5d} {r.idr:4d}  {r.cpu}", flush=True)
    finally:
        set_channel(chan, channel_point("clean"))

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["fec", "channel", "chan_pct", "resid_pct", "fps", "recovered", "failures",
                        "idr", "cpu"])
            for r in results:
                w.writerow([r.fec, r.channel, f"{r.chan_pct:.3f}", f"{r.resid_pct:.4f}",
                            f"{r.fps:.2f}", r.recovered, r.failures, r.idr, r.cpu])
    return 0


if __name__ == "__main__":
    sys.exit(main())
