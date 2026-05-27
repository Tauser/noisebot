import struct

from noisebot_server.internal.agent.vad import BargeInMonitor


def _pcm(level: int, samples: int = 256) -> bytes:
    return struct.pack(f"<{samples}h", *([level] * samples))


def test_barge_monitor_learns_tts_leakage_without_triggering() -> None:
    monitor = BargeInMonitor(threshold_rms=1200.0, sustain_chunks=4)

    for _ in range(20):
        assert monitor.feed(_pcm(1800)) is False

    assert monitor.above_count == 0
    assert monitor.baseline is not None


def test_barge_monitor_triggers_on_sustained_energy_above_tts_floor() -> None:
    monitor = BargeInMonitor(threshold_rms=1200.0, sustain_chunks=4)

    for _ in range(10):
        assert monitor.feed(_pcm(1600)) is False

    assert monitor.feed(_pcm(3600)) is False
    assert monitor.feed(_pcm(3600)) is False
    assert monitor.feed(_pcm(3600)) is False
    assert monitor.feed(_pcm(3600)) is True


def test_barge_monitor_observes_playback_grace_without_triggering() -> None:
    monitor = BargeInMonitor(threshold_rms=1200.0, sustain_chunks=3)

    for _ in range(6):
        assert monitor.feed(_pcm(4200), allow_trigger=False) is False

    assert monitor.above_count == 0
    assert monitor.baseline is not None
