"""Testes unitários: MetricsRegistry — sliding window e percentis.

Cobre:
  - record() e count() acumulam corretamente
  - percentile(50) e percentile(95) com valores conhecidos
  - p50() / p95() atalhos
  - janela deslizante: amostras antigas são descartadas
  - record_many() registra múltiplas métricas
  - snapshot() e snapshot_flat() retornam estruturas corretas
  - clear() limpa métrica específica ou todas
  - métricas inexistentes retornam None
"""
from __future__ import annotations

import pytest

from bridgev2.metrics.registry import MetricsRegistry


# ── Helpers ────────────────────────────────────────────────────────────────────

def _registry(window: int = 100) -> MetricsRegistry:
    return MetricsRegistry(window=window)


# ── Básico ─────────────────────────────────────────────────────────────────────

class TestBasic:
    def test_empty_percentile_returns_none(self):
        r = _registry()
        assert r.percentile("stt_ms", 50) is None
        assert r.p50("stt_ms") is None
        assert r.p95("stt_ms") is None

    def test_count_zero_before_record(self):
        r = _registry()
        assert r.count("stt_ms") == 0

    def test_single_sample(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        assert r.count("stt_ms") == 1
        assert r.p50("stt_ms") == pytest.approx(400.0)
        assert r.p95("stt_ms") == pytest.approx(400.0)

    def test_two_samples(self):
        r = _registry()
        r.record("stt_ms", 100.0)
        r.record("stt_ms", 200.0)
        p50 = r.p50("stt_ms")
        assert p50 is not None
        assert 100.0 <= p50 <= 200.0


# ── Percentis com valores conhecidos ──────────────────────────────────────────

class TestPercentiles:
    def test_p50_median(self):
        r = _registry()
        for v in [10.0, 20.0, 30.0, 40.0, 50.0]:
            r.record("x", v)
        p50 = r.p50("x")
        assert p50 == pytest.approx(30.0)

    def test_p95_high_end(self):
        r = _registry()
        for v in range(1, 101):   # 1..100
            r.record("x", float(v))
        p95 = r.p95("x")
        assert p95 is not None
        assert p95 >= 94.0  # p95 de 1..100 ≈ 95.05

    def test_identical_samples(self):
        r = _registry()
        for _ in range(50):
            r.record("x", 300.0)
        assert r.p50("x") == pytest.approx(300.0)
        assert r.p95("x") == pytest.approx(300.0)

    def test_p50_two_elements(self):
        r = _registry()
        r.record("x", 100.0)
        r.record("x", 200.0)
        # interpolação linear: idx = 0.5 * 1 = 0.5 → entre 100 e 200 = 150
        assert r.p50("x") == pytest.approx(150.0)


# ── Janela deslizante ─────────────────────────────────────────────────────────

class TestSlidingWindow:
    def test_window_discards_old_samples(self):
        r = _registry(window=5)
        for v in [1.0, 2.0, 3.0, 4.0, 5.0]:
            r.record("x", v)
        # Adiciona 1000 — expulsa o 1.0
        r.record("x", 1000.0)
        # Agora a janela tem [2, 3, 4, 5, 1000]
        p95 = r.p95("x")
        assert p95 is not None
        assert p95 > 500.0  # dominado pelo 1000

    def test_count_continua_acumulando_alem_da_janela(self):
        r = _registry(window=3)
        for _ in range(10):
            r.record("x", 1.0)
        assert r.count("x") == 10  # total acumulado

    def test_window_size_one(self):
        r = _registry(window=1)
        r.record("x", 100.0)
        r.record("x", 200.0)
        # Janela de 1: apenas o último valor
        assert r.p50("x") == pytest.approx(200.0)


# ── record_many ────────────────────────────────────────────────────────────────

class TestRecordMany:
    def test_record_many_multiple_keys(self):
        r = _registry()
        r.record_many({"stt_ms": 400.0, "llm_ms": 1200.0, "tts_ms": 300.0})
        assert r.count("stt_ms") == 1
        assert r.count("llm_ms") == 1
        assert r.count("tts_ms") == 1

    def test_record_many_skips_none(self):
        r = _registry()
        r.record_many({"stt_ms": 400.0, "llm_ms": None})  # type: ignore
        assert r.count("stt_ms") == 1
        assert r.count("llm_ms") == 0


# ── snapshot ──────────────────────────────────────────────────────────────────

class TestSnapshot:
    def test_snapshot_keys(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        r.record("llm_ms", 1000.0)
        snap = r.snapshot()
        assert "stt_ms" in snap
        assert "llm_ms" in snap

    def test_snapshot_has_p50_p95(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        snap = r.snapshot()
        assert "p50_ms" in snap["stt_ms"]
        assert "p95_ms" in snap["stt_ms"]
        assert "count" in snap["stt_ms"]

    def test_snapshot_flat_keys(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        flat = r.snapshot_flat()
        assert "stt_ms_p50" in flat
        assert "stt_ms_p95" in flat

    def test_snapshot_empty(self):
        r = _registry()
        assert r.snapshot() == {}
        assert r.snapshot_flat() == {}


# ── clear ─────────────────────────────────────────────────────────────────────

class TestClear:
    def test_clear_specific_key(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        r.record("llm_ms", 1000.0)
        r.clear("stt_ms")
        assert r.count("stt_ms") == 0
        assert r.count("llm_ms") == 1

    def test_clear_all(self):
        r = _registry()
        r.record("stt_ms", 400.0)
        r.record("llm_ms", 1000.0)
        r.clear()
        assert r.count("stt_ms") == 0
        assert r.count("llm_ms") == 0
        assert r.keys() == []

    def test_clear_nonexistent_key_is_noop(self):
        r = _registry()
        r.clear("nenhuma_metrica")  # não lança exceção
        assert r.count("nenhuma_metrica") == 0


# ── keys ──────────────────────────────────────────────────────────────────────

class TestKeys:
    def test_keys_empty(self):
        r = _registry()
        assert r.keys() == []

    def test_keys_after_record(self):
        r = _registry()
        r.record("stt_ms", 1.0)
        r.record("llm_ms", 2.0)
        assert set(r.keys()) == {"stt_ms", "llm_ms"}
