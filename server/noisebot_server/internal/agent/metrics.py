"""Latency metrics registry for the server runtime."""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field


@dataclass
class _Series:
    window: int
    samples: deque[float] = field(default_factory=deque)
    total_count: int = 0
    total_sum: float = 0.0
    last_updated: float = field(default_factory=time.monotonic)

    def push(self, value: float) -> None:
        if len(self.samples) >= self.window:
            self.samples.popleft()
        self.samples.append(value)
        self.total_count += 1
        self.total_sum += value
        self.last_updated = time.monotonic()

    def percentile(self, p: float) -> float | None:
        if not self.samples:
            return None
        sorted_samples = sorted(self.samples)
        n = len(sorted_samples)
        idx = (p / 100.0) * (n - 1)
        lo = int(idx)
        hi = min(lo + 1, n - 1)
        frac = idx - lo
        return sorted_samples[lo] * (1 - frac) + sorted_samples[hi] * frac

    def mean(self) -> float | None:
        if not self.samples:
            return None
        return sum(self.samples) / len(self.samples)

    def to_dict(self) -> dict[str, float | int | None]:
        p50 = self.percentile(50)
        p95 = self.percentile(95)
        mean = self.mean()
        return {
            "p50_ms": round(p50, 1) if p50 is not None else None,
            "p95_ms": round(p95, 1) if p95 is not None else None,
            "mean_ms": round(mean, 1) if mean is not None else None,
            "total": round(self.total_sum, 1),
            "count": self.total_count,
            "window": min(len(self.samples), self.window),
        }


class MetricsRegistry:
    """Sliding-window latency metrics."""

    def __init__(self, window: int = 100) -> None:
        self._window = window
        self._series: dict[str, _Series] = {}

    def record(self, key: str, value_ms: float) -> None:
        if key not in self._series:
            self._series[key] = _Series(window=self._window)
        self._series[key].push(value_ms)

    def record_many(self, values: dict[str, float]) -> None:
        for key, value in values.items():
            if value is not None:
                self.record(key, value)

    def percentile(self, key: str, p: float) -> float | None:
        series = self._series.get(key)
        if series is None:
            return None
        return series.percentile(p)

    def p50(self, key: str) -> float | None:
        return self.percentile(key, 50)

    def p95(self, key: str) -> float | None:
        return self.percentile(key, 95)

    def count(self, key: str) -> int:
        series = self._series.get(key)
        return series.total_count if series else 0

    def keys(self) -> list[str]:
        return list(self._series.keys())

    def snapshot(self) -> dict[str, dict[str, float | int | None]]:
        return {key: series.to_dict() for key, series in self._series.items()}

    def snapshot_flat(self) -> dict[str, float | None]:
        result: dict[str, float | None] = {}
        for key, series in self._series.items():
            result[f"{key}_p50"] = series.percentile(50)
            result[f"{key}_p95"] = series.percentile(95)
        return result

    def clear(self, key: str | None = None) -> None:
        if key is not None:
            self._series.pop(key, None)
        else:
            self._series.clear()


__all__ = ["MetricsRegistry"]
