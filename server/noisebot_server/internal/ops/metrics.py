"""noisebot_server.internal.ops.metrics — formata métricas para o dashboard."""
from __future__ import annotations

from ..agent.metrics import MetricsRegistry
from .status import StatusStore


class MetricsApi:
    """Serializa MetricsRegistry + StatusStore para o endpoint GET /ai/metrics."""

    def __init__(self, registry: MetricsRegistry, store: StatusStore) -> None:
        self._registry = registry
        self._store = store

    def get_metrics(self) -> dict:
        snap = self._registry.snapshot()

        def _lat(key: str) -> dict | None:
            series = snap.get(key)
            if series is None:
                return None
            p50 = series.get("p50_ms")
            p95 = series.get("p95_ms")
            if p50 is None and p95 is None:
                return None
            return {
                "p50": _round(p50),
                "p95": _round(p95),
                "count": series.get("count", 0),
            }

        latency_ms = {}
        for key, label in [
            ("stt_ms",                   "stt"),
            ("llm_first_token_ms",       "llm_first_token"),
            ("llm_total_ms",             "llm_total"),
            ("tts_first_audio_ms",       "tts_first_audio"),
            ("first_audio_out_ms",       "first_audio_out"),
            ("first_robot_reaction_ms",  "first_robot_reaction"),
            ("interruption_cancel_ms",   "interruption_cancel"),
        ]:
            entry = _lat(key)
            if entry is not None:
                latency_ms[label] = entry

        # Tokens e custo: null por enquanto (providers não fornecem ainda)
        # Será preenchido quando LlmReplyComplete incluir usage.
        token_series = snap.get("input_tokens")
        input_total = _total_from_snapshot(token_series)
        output_series = snap.get("output_tokens")
        output_total = _total_from_snapshot(output_series)

        return {
            "latency_ms": latency_ms,
            "turns": self._store.turn_counters.copy(),
            "tokens": {
                "input": input_total,
                "output": output_total,
            },
            "estimated_cost": None,  # preenchido quando providers fornecerem uso
        }

    def reset(self) -> None:
        """Zera as métricas agregadas (não apaga logs estruturados)."""
        self._registry.clear()
        for k in self._store.turn_counters:
            self._store.turn_counters[k] = 0


def _round(v: float | None) -> float | None:
    if v is None:
        return None
    return round(v, 1)


def _total_from_snapshot(series: dict | None) -> int | None:
    if not series:
        return None
    mean = series.get("mean_ms")
    count = series.get("count", 0)
    if mean is None or not count:
        return None
    return int(mean * count)
