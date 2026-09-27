from __future__ import annotations

import csv
import sys
import time
from pathlib import Path

PLOT_PREFIX = "cbr/"


def _normalize_plot_url(url: str) -> str:
    url = url.strip()
    if not url:
        return ""
    if url.startswith("http://"):
        return "ws://" + url[len("http://") :]
    if url.startswith("https://"):
        return "wss://" + url[len("https://") :]
    return url


class PlotWS:
    def __init__(self, url: str) -> None:
        self._enabled = False
        self._ts: float | None = None
        self._pending: dict[str, float] = {}
        ws_url = _normalize_plot_url(url)
        if not ws_url:
            return
        try:
            from utils.plotjuggler import start_client  # noqa: WPS433

            start_client(ws_url)
            self._enabled = True
        except ImportError:
            print("plot-url disabled: pip install websockets", file=sys.stderr)
        except (RuntimeError, ValueError) as exc:
            print(f"plot-url disabled: {exc}", file=sys.stderr)

    def set_time(self, ts: float) -> None:
        self._ts = ts

    def __call__(self, name: str, *args: float) -> None:
        if not self._enabled:
            return
        key = f"{PLOT_PREFIX}{name}"
        if len(args) >= 2:
            self._ts = args[0]
            self._pending[key] = args[1]
        elif len(args) == 1:
            self._pending[key] = args[0]

    def flush(self) -> None:
        if not self._enabled or not self._pending:
            return
        from utils.plotjuggler import push_tick  # noqa: WPS433

        ts = self._ts if self._ts is not None else time.time()
        push_tick(ts, dict(self._pending))
        self._pending.clear()
        self._ts = None


class PlotCsv:
    """One CSV row per flush (same series as PlotWS, without cbr/ prefix)."""

    def __init__(self) -> None:
        self._path = ""
        self._enabled = False
        self._pending: dict[str, float] = {}
        self._ts: float | None = None
        self._fp = None
        self._writer: csv.DictWriter | None = None
        self._fieldnames: list[str] = []

    @property
    def path(self) -> str:
        return self._path if self._enabled else ""

    def start(self, path: str) -> str:
        path = path.strip()
        if not path:
            return "err empty path"
        self.stop()
        out = Path(path)
        if not out.is_absolute():
            out = Path.cwd() / out
        out.parent.mkdir(parents=True, exist_ok=True)
        self._path = str(out)
        self._enabled = True
        self._pending.clear()
        self._ts = None
        self._writer = None
        self._fieldnames = []
        self._fp = out.open("w", newline="", encoding="utf-8")
        return f"ok recording {self._path}"

    def stop(self) -> str:
        if not self._enabled:
            return "ok not recording"
        saved = self._path
        self._enabled = False
        self._pending.clear()
        self._ts = None
        self._writer = None
        self._fieldnames = []
        self._path = ""
        if self._fp is not None:
            self._fp.close()
            self._fp = None
        return f"ok stopped {saved}"

    def set_time(self, ts: float) -> None:
        self._ts = ts

    def __call__(self, name: str, *args: float) -> None:
        if not self._enabled:
            return
        if len(args) >= 2:
            self._ts = args[0]
            self._pending[name] = args[1]
        elif len(args) == 1:
            self._pending[name] = args[0]

    def flush(self) -> None:
        if not self._enabled or not self._pending or self._fp is None:
            return
        ts = self._ts if self._ts is not None else time.time()
        row_keys = sorted(self._pending.keys())
        if self._writer is None:
            self._fieldnames = ["timestamp", *row_keys]
            self._writer = csv.DictWriter(
                self._fp, fieldnames=self._fieldnames, extrasaction="ignore"
            )
            if self._fp.tell() == 0:
                self._writer.writeheader()
        row = {"timestamp": ts, **self._pending}
        self._writer.writerow(row)
        self._fp.flush()
        self._pending.clear()
        self._ts = None

    def close(self) -> None:
        if self._fp is not None:
            self._fp.close()
            self._fp = None


class PlotFanout:
    def __init__(self, *sinks: PlotWS | PlotCsv) -> None:
        self._sinks = sinks

    def set_time(self, ts: float) -> None:
        for sink in self._sinks:
            sink.set_time(ts)

    def __call__(self, name: str, *args: float) -> None:
        for sink in self._sinks:
            sink(name, *args)

    def flush(self) -> None:
        for sink in self._sinks:
            sink.flush()

    def close(self) -> None:
        for sink in self._sinks:
            close = getattr(sink, "close", None)
            if callable(close):
                close()
