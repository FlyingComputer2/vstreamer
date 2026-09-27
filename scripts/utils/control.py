from __future__ import annotations

import math
from collections import deque


class FirBoxcar:
    """Uniform FIR: sum of last up-to-N samples divided by N (implicit zero pad → ramps from 0)."""

    def __init__(self, length: int) -> None:
        self._n = max(1, int(length))
        self._buf: deque[float] = deque(maxlen=self._n)
        self._sum = 0.0
        self._last_out = 0.0

    @property
    def value(self) -> float:
        return self._last_out

    def __call__(self, x: float) -> float:
        if len(self._buf) == self._n:
            self._sum -= self._buf[0]
        self._buf.append(x)
        self._sum += x
        self._last_out = self._sum / self._n
        return self._last_out


def _fir_lowpass_taps(numtaps: int, cutoff: float, sample_rate: float) -> list[float]:
    """Hamming-windowed sinc lowpass; unity gain at DC."""
    n = max(3, int(numtaps) | 1)
    fc = max(cutoff, 1e-9)
    fs = max(sample_rate, 1e-9)
    fn = fc / fs
    mid = (n - 1) / 2.0
    taps: list[float] = []
    for i in range(n):
        t = 2.0 * fn * (i - mid)
        if abs(t) < 1e-12:
            ideal = 2.0 * fn
        else:
            ideal = math.sin(math.pi * t) / (math.pi * t) * (2.0 * fn)
        w = 0.54 - 0.46 * math.cos(2.0 * math.pi * i / (n - 1))
        taps.append(ideal * w)
    scale = sum(taps)
    if abs(scale) < 1e-12:
        return [1.0 / n] * n
    return [t / scale for t in taps]


class LPF:
    """Causal FIR lowpass (Hamming window). `order` = tap count (odd); if <= 1, length ~ 3 RC time constants."""

    def __init__(
        self,
        sample_rate: float,
        cutoff: float,
        rolloff: float = 1.0,
        order: int = 1,
        type: str = "lowpass",
    ) -> None:
        del rolloff, type
        if order > 1:
            numtaps = max(3, int(order) | 1)
        else:
            rc = 1.0 / (2.0 * math.pi * max(cutoff, 1e-9))
            numtaps = int(round(3.0 * rc * sample_rate)) | 1
            numtaps = max(3, min(numtaps, 401))
        self._taps = _fir_lowpass_taps(numtaps, cutoff, sample_rate)
        self._buf: deque[float] = deque(maxlen=len(self._taps))
        self._last_out = 0.0

    @property
    def num_taps(self) -> int:
        return len(self._taps)

    @property
    def group_delay_samples(self) -> float:
        return (len(self._taps) - 1) / 2.0

    def __call__(self, x: float) -> float:
        self._buf.append(x)
        n = len(self._buf)
        nt = len(self._taps)
        hist = self._buf
        y = 0.0
        base = nt - n
        for i in range(n):
            y += self._taps[base + i] * hist[i]
        self._last_out = y
        return y

    @property
    def value(self) -> float:
        return self._last_out


class Delay:
    """Return the sample from n loop steps ago; warm-up passes x through (so s - delay(s) is 0)."""

    def __init__(self, n: int) -> None:
        self._n = max(1, int(n))
        self._hist: deque[float] = deque(maxlen=self._n)

    def __call__(self, x: float | None) -> float:
        if x is None:
            return 0.0
        if len(self._hist) < self._n:
            out = x
        else:
            out = self._hist[0]
        self._hist.append(x)
        return out


class CounterDelta:
    """Per-loop delta of a monotonic counter; unchanged telemetry reads yield changed=False."""

    def __init__(self) -> None:
        self._prev: float | None = None

    def __call__(self, x: float | None) -> tuple[float, bool]:
        if x is None:
            return 0.0, False
        if self._prev is None:
            self._prev = x
            return 0.0, False
        if x == self._prev:
            return 0.0, False
        delta = x - self._prev
        self._prev = x
        return delta, True


class Clamp:
    def __init__(self, min: float, max: float) -> None:
        self._min = min
        self._max = max

    def set_max(self, max: float) -> None:
        self._max = max

    def __call__(self, x: float) -> float:
        return max(self._min, min(self._max, x))


class Integrator:
    """Euler integrate u (per-second units) with fixed dt; optional output clamp."""

    def __init__(self, dt: float, output_clamp: Clamp | None = None) -> None:
        self._dt = dt
        self._clamp = output_clamp
        self._state = 0.0

    @property
    def value(self) -> float:
        return self._state

    def reset(self, value: float = 0.0) -> None:
        self._state = value
        if self._clamp is not None:
            self._state = self._clamp(self._state)

    def set_max(self, max: float) -> None:
        if self._clamp is not None:
            self._clamp.set_max(max)
            self._state = self._clamp(self._state)

    def __call__(self, u: float) -> float:
        self._state += u * self._dt
        if self._clamp is not None:
            self._state = self._clamp(self._state)
        return self._state


class FecMap:
    def __init__(self, k: float, gain: float) -> None:
        self._k = k
        self._gain = gain

    def __call__(self, loss_rate: float) -> float:
        p = min(max(loss_rate, 0.0), 0.999999)
        return self._gain * self._k / (1.0 - p)
