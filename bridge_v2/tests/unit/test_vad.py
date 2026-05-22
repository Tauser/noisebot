"""Tests for bridgev2.audio.vad — BargeInMonitor."""
from __future__ import annotations

import math
import struct

import pytest

from bridgev2.audio.vad import BargeInMonitor, _compute_rms, DEFAULT_THRESHOLD_RMS


def _make_chunk(rms_target: float, num_samples: int = 256) -> bytes:
    """Gera chunk PCM com RMS aproximado de rms_target."""
    value = int(rms_target)
    value = max(-32768, min(32767, value))
    return struct.pack(f"<{num_samples}h", *([value] * num_samples))


def _silence(num_samples: int = 256) -> bytes:
    return bytes(num_samples * 2)


def _loud(rms: float = 2000.0, num_samples: int = 256) -> bytes:
    return _make_chunk(rms, num_samples)


class TestComputeRms:
    def test_silence_is_zero(self):
        assert _compute_rms(_silence()) == 0.0

    def test_full_scale_positive(self):
        chunk = struct.pack("<4h", 32767, 32767, 32767, 32767)
        assert _compute_rms(chunk) == pytest.approx(32767.0)

    def test_alternating_sign(self):
        chunk = struct.pack("<4h", 100, -100, 100, -100)
        assert _compute_rms(chunk) == pytest.approx(100.0)

    def test_empty_returns_zero(self):
        assert _compute_rms(b"") == 0.0

    def test_odd_byte_ignored(self):
        # Número ímpar de bytes: byte extra ignorado
        chunk = struct.pack("<2h", 100, 100) + b"\x00"
        rms = _compute_rms(chunk)
        assert rms == pytest.approx(100.0)


class TestBargeInMonitorBasic:
    def test_silence_never_triggers(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=3)
        for _ in range(50):
            assert not monitor.feed(_silence())

    def test_single_loud_chunk_does_not_trigger(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=5)
        assert not monitor.feed(_loud(2000))

    def test_sustained_loud_triggers(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=5)
        for i in range(5):
            result = monitor.feed(_loud(2000))
        assert result  # 5 consecutive → triggered

    def test_triggers_exactly_at_sustain_count(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=3)
        assert not monitor.feed(_loud(2000))  # 1
        assert not monitor.feed(_loud(2000))  # 2
        assert monitor.feed(_loud(2000))      # 3 → trigger


class TestBargeInMonitorReset:
    def test_silence_breaks_streak(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=5)
        monitor.feed(_loud(2000))  # count = 1
        monitor.feed(_loud(2000))  # count = 2
        monitor.feed(_silence())   # count = 0
        monitor.feed(_loud(2000))  # count = 1
        monitor.feed(_loud(2000))  # count = 2
        assert not monitor.feed(_loud(2000))   # count = 3 — still below 5

    def test_reset_clears_count(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=3)
        monitor.feed(_loud(2000))
        monitor.feed(_loud(2000))
        monitor.reset()
        assert monitor.above_count == 0

    def test_reset_prevents_false_trigger(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=3)
        monitor.feed(_loud(2000))
        monitor.feed(_loud(2000))
        monitor.reset()
        # Dois chunks pós-reset não devem acionar
        assert not monitor.feed(_loud(2000))
        assert not monitor.feed(_loud(2000))


class TestBargeInMonitorProperties:
    def test_above_count_increments(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=10)
        monitor.feed(_loud(2000))
        assert monitor.above_count == 1
        monitor.feed(_loud(2000))
        assert monitor.above_count == 2

    def test_above_count_resets_on_silence(self):
        monitor = BargeInMonitor(threshold_rms=200, sustain_chunks=10)
        monitor.feed(_loud(2000))
        monitor.feed(_loud(2000))
        monitor.feed(_silence())
        assert monitor.above_count == 0

    def test_threshold_boundary(self):
        monitor = BargeInMonitor(threshold_rms=500.0, sustain_chunks=1)
        # Chunk com RMS = 499 → não deve acionar
        assert not monitor.feed(_make_chunk(499, 256))
        monitor.reset()
        # Chunk com RMS ≥ 500 → deve acionar
        assert monitor.feed(_make_chunk(500, 256))


class TestBargeInMonitorDefaults:
    def test_default_threshold_ignores_silence(self):
        monitor = BargeInMonitor()
        for _ in range(20):
            assert not monitor.feed(_silence())

    def test_default_sustain_requires_multiple_chunks(self):
        monitor = BargeInMonitor()
        # Um único chunk acima do threshold não deve acionar
        assert not monitor.feed(_loud(DEFAULT_THRESHOLD_RMS + 100))
