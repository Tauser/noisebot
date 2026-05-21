"""bridgev2.metrics.registry — Agregação p50/p95 por janela móvel deslizante.

MetricsRegistry mantém uma janela circular de N amostras por métrica e
expõe percentis (p50, p95) para exibição no dashboard de operação (Fase 9.5).

Uso típico:
    registry = MetricsRegistry(window=100)
    registry.record("stt_ms", 420.5)
    registry.record("stt_ms", 380.2)
    p50 = registry.percentile("stt_ms", 50)
    snap = registry.snapshot()   # dict com p50/p95 de todas as métricas
"""
from __future__ import annotations

import bisect
import math
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Sequence


@dataclass
class _Series:
    """Janela circular de amostras com cálculo de percentil O(n log n)."""
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
        """Percentil p (0–100) da janela atual. None se vazia."""
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

    def count(self) -> int:
        return self.total_count

    def to_dict(self) -> dict:
        p50 = self.percentile(50)
        p95 = self.percentile(95)
        return {
            "p50_ms": round(p50, 1) if p50 is not None else None,
            "p95_ms": round(p95, 1) if p95 is not None else None,
            "mean_ms": round(self.mean(), 1) if self.mean() is not None else None,
            "count": self.total_count,
            "window": min(len(self.samples), self.window),
        }


class MetricsRegistry:
    """Registro de métricas de latência com janela deslizante.

    Thread-safe para leituras. Escritas devem ocorrer no event loop
    (sem lock — estrutura não é thread-safe para escritas concorrentes).
    """

    def __init__(self, window: int = 100) -> None:
        """
        Parâmetros
        ----------
        window : int
            Número máximo de amostras mantidas por métrica (padrão 100 turnos).
        """
        self._window = window
        self._series: dict[str, _Series] = {}

    def record(self, key: str, value_ms: float) -> None:
        """Registra uma amostra para a métrica `key` em milissegundos."""
        if key not in self._series:
            self._series[key] = _Series(window=self._window)
        self._series[key].push(value_ms)

    def record_many(self, values: dict[str, float]) -> None:
        """Registra múltiplas métricas de uma vez (uso conveniente após turno)."""
        for key, value in values.items():
            if value is not None:
                self.record(key, value)

    def percentile(self, key: str, p: float) -> float | None:
        """Percentil p (0–100) para a métrica `key`. None se sem dados."""
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

    def snapshot(self) -> dict[str, dict]:
        """Retorna snapshot de todas as métricas com p50, p95, mean e count."""
        return {key: series.to_dict() for key, series in self._series.items()}

    def snapshot_flat(self) -> dict[str, float | None]:
        """Snapshot plano: key_p50, key_p95 para cada métrica. Útil para logs."""
        result: dict[str, float | None] = {}
        for key, series in self._series.items():
            result[f"{key}_p50"] = series.percentile(50)
            result[f"{key}_p95"] = series.percentile(95)
        return result

    def clear(self, key: str | None = None) -> None:
        """Limpa uma métrica específica ou todas."""
        if key is not None:
            self._series.pop(key, None)
        else:
            self._series.clear()
