from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator

import pytest

from noisebot_server.internal.agent import playback as playback_module
from noisebot_server.internal.agent.playback import (
    CHUNK_BYTES,
    CHUNK_DURATION_S,
    FIRMWARE_SAY_QUEUE,
    OutputScheduler,
    SAY_SEND_INTERVAL_S,
    SAY_STARTUP_CHUNKS,
    SAY_STARTUP_INTERVAL_S,
)


class AdapterProbe:
    def __init__(self) -> None:
        self.begin: list[int] = []
        self.end: list[int] = []
        self.chunks: list[bytes] = []

    async def send_say_begin(self, turn_id: int) -> None:
        self.begin.append(turn_id)

    async def send_say(self, pcm: bytes) -> None:
        self.chunks.append(pcm)

    async def send_say_end(self, turn_id: int) -> None:
        self.end.append(turn_id)


class SpeechCancelAdapter(AdapterProbe):
    async def send_say(self, pcm: bytes) -> None:
        raise ConnectionError("SPEECH_CANCEL")


def test_output_scheduler_default_prebuffer_leaves_firmware_queue_headroom() -> None:
    assert FIRMWARE_SAY_QUEUE == 0


def test_output_scheduler_default_send_interval_keeps_hardware_headroom() -> None:
    assert SAY_SEND_INTERVAL_S == pytest.approx(0.024)
    assert SAY_SEND_INTERVAL_S > CHUNK_DURATION_S


def test_output_scheduler_default_startup_ramp_is_disabled_for_responsiveness() -> None:
    assert SAY_STARTUP_CHUNKS == 0
    assert SAY_STARTUP_INTERVAL_S == pytest.approx(0.024)


async def _iter_chunks(*chunks: bytes) -> AsyncIterator[bytes]:
    for chunk in chunks:
        yield chunk


@pytest.mark.asyncio
async def test_output_scheduler_rechunks_oversized_tts_frames() -> None:
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = bytes(range(256)) * 3  # 768 bytes; not aligned to firmware chunks.

    stats = await scheduler.run(7, _iter_chunks(source[:532], source[532:]), adapter)

    assert adapter.begin == [7]
    assert adapter.end == [7]
    assert [len(chunk) for chunk in adapter.chunks] == [CHUNK_BYTES, CHUNK_BYTES]
    assert adapter.chunks[0] == source[:CHUNK_BYTES]
    assert adapter.chunks[1].startswith(source[CHUNK_BYTES:])
    assert adapter.chunks[1][len(source) - CHUNK_BYTES :] == b"\x00" * (CHUNK_BYTES * 2 - len(source))
    assert stats.chunks_sent == 2
    assert stats.pcm_bytes_in == len(source)
    assert stats.pcm_bytes_sent == CHUNK_BYTES * 2
    assert stats.padding_bytes == CHUNK_BYTES * 2 - len(source)
    assert stats.say_begin_sent is True
    assert stats.say_end_sent is True


@pytest.mark.asyncio
async def test_output_scheduler_sends_exact_chunk_without_padding() -> None:
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = b"\x11\x22" * (CHUNK_BYTES // 2)

    stats = await scheduler.run(8, _iter_chunks(source), adapter)

    assert [len(chunk) for chunk in adapter.chunks] == [CHUNK_BYTES]
    assert adapter.chunks[0] == source
    assert stats.chunks_sent == 1
    assert stats.pcm_bytes_in == CHUNK_BYTES
    assert stats.pcm_bytes_sent == CHUNK_BYTES
    assert stats.padding_bytes == 0
    assert stats.say_end_sent is True


@pytest.mark.asyncio
async def test_output_scheduler_does_not_catch_up_with_bursts(monkeypatch) -> None:
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = b"\x55" * CHUNK_BYTES * (FIRMWARE_SAY_QUEUE + 3)
    sleeps: list[float] = []
    now = 1000.0

    def monotonic() -> float:
        return now

    async def fake_sleep(delay: float) -> None:
        nonlocal now
        sleeps.append(delay)
        if delay > 0:
            now += delay

    monkeypatch.setattr(playback_module.time, "monotonic", monotonic)
    monkeypatch.setattr(playback_module.asyncio, "sleep", fake_sleep)

    stats = await scheduler.run(10, _iter_chunks(source), adapter)

    paced_sleeps = [delay for delay in sleeps if delay > 0]
    assert len(adapter.chunks) == FIRMWARE_SAY_QUEUE + 3
    assert stats.chunks_sent == FIRMWARE_SAY_QUEUE + 3
    expected_paced = len(adapter.chunks) - max(FIRMWARE_SAY_QUEUE, 1)
    assert len(paced_sleeps) == expected_paced
    assert all(delay == pytest.approx(SAY_SEND_INTERVAL_S) for delay in paced_sleeps)


@pytest.mark.asyncio
async def test_output_scheduler_returns_to_nominal_pacing_after_startup(monkeypatch) -> None:
    monkeypatch.setattr(playback_module, "SAY_STARTUP_CHUNKS", 4)
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = b"\x55" * CHUNK_BYTES * (playback_module.SAY_STARTUP_CHUNKS + 3)
    sleeps: list[float] = []
    now = 1000.0

    def monotonic() -> float:
        return now

    async def fake_sleep(delay: float) -> None:
        nonlocal now
        sleeps.append(delay)
        if delay > 0:
            now += delay

    monkeypatch.setattr(playback_module.time, "monotonic", monotonic)
    monkeypatch.setattr(playback_module.asyncio, "sleep", fake_sleep)

    stats = await scheduler.run(11, _iter_chunks(source), adapter)

    paced_sleeps = [delay for delay in sleeps if delay > 0]
    assert len(adapter.chunks) == playback_module.SAY_STARTUP_CHUNKS + 3
    assert stats.chunks_sent == playback_module.SAY_STARTUP_CHUNKS + 3
    assert any(delay == pytest.approx(SAY_STARTUP_INTERVAL_S) for delay in paced_sleeps)
    assert any(delay == pytest.approx(SAY_SEND_INTERVAL_S) for delay in paced_sleeps)


@pytest.mark.asyncio
async def test_output_scheduler_treats_speech_cancel_as_cancellation() -> None:
    adapter = SpeechCancelAdapter()
    scheduler = OutputScheduler()
    source = b"\x33\x44" * (CHUNK_BYTES // 2)

    with pytest.raises(asyncio.CancelledError):
        await scheduler.run(9, _iter_chunks(source), adapter)

    assert adapter.begin == [9]
    assert adapter.end == []
