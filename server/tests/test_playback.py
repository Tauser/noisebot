from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator

import pytest

from noisebot_server.internal.agent.playback import CHUNK_BYTES, OutputScheduler


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


async def _iter_chunks(*chunks: bytes) -> AsyncIterator[bytes]:
    for chunk in chunks:
        yield chunk


@pytest.mark.asyncio
async def test_output_scheduler_rechunks_oversized_tts_frames() -> None:
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = bytes(range(256)) * 3  # 768 bytes; not aligned to firmware chunks.

    await scheduler.run(7, _iter_chunks(source[:532], source[532:]), adapter)

    assert adapter.begin == [7]
    assert adapter.end == [7]
    assert [len(chunk) for chunk in adapter.chunks] == [CHUNK_BYTES, CHUNK_BYTES]
    assert adapter.chunks[0] == source[:CHUNK_BYTES]
    assert adapter.chunks[1].startswith(source[CHUNK_BYTES:])
    assert adapter.chunks[1][len(source) - CHUNK_BYTES :] == b"\x00" * (CHUNK_BYTES * 2 - len(source))


@pytest.mark.asyncio
async def test_output_scheduler_sends_exact_chunk_without_padding() -> None:
    adapter = AdapterProbe()
    scheduler = OutputScheduler()
    source = b"\x11\x22" * (CHUNK_BYTES // 2)

    await scheduler.run(8, _iter_chunks(source), adapter)

    assert [len(chunk) for chunk in adapter.chunks] == [CHUNK_BYTES]
    assert adapter.chunks[0] == source


@pytest.mark.asyncio
async def test_output_scheduler_treats_speech_cancel_as_cancellation() -> None:
    adapter = SpeechCancelAdapter()
    scheduler = OutputScheduler()
    source = b"\x33\x44" * (CHUNK_BYTES // 2)

    with pytest.raises(asyncio.CancelledError):
        await scheduler.run(9, _iter_chunks(source), adapter)

    assert adapter.begin == [9]
    assert adapter.end == []
