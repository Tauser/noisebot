"""Tests for bridgev2.audio.playback — OutputScheduler."""
from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock, MagicMock

import pytest

from bridgev2.audio.playback import (
    OutputScheduler,
    CHUNK_DURATION_S,
    FIRMWARE_SAY_QUEUE,
)

CHUNK_BYTES = 512  # 256 int16 amostras × 2 bytes


async def _aiter(items):
    for item in items:
        yield item


class TestOutputScheduler:
    @pytest.mark.asyncio
    async def test_sends_all_chunks_to_adapter(self):
        adapter = MagicMock()
        adapter.send_say = AsyncMock()

        chunks = [b"\x00\x01" * 256, b"\x02\x03" * 256, b"\x04\x05" * 256]
        scheduler = OutputScheduler()
        await scheduler.run(
            turn_id=1,
            pcm_iter=_aiter(chunks),
            adapter=adapter,
        )

        assert adapter.send_say.call_count == 3
        calls = [c.args[0] for c in adapter.send_say.call_args_list]
        assert calls == chunks

    @pytest.mark.asyncio
    async def test_no_adapter_does_not_raise(self):
        chunks = [b"\x00" * CHUNK_BYTES] * 5
        scheduler = OutputScheduler()
        await scheduler.run(
            turn_id=1,
            pcm_iter=_aiter(chunks),
            adapter=None,
        )

    @pytest.mark.asyncio
    async def test_empty_iter_sends_nothing(self):
        adapter = MagicMock()
        adapter.send_say = AsyncMock()
        scheduler = OutputScheduler()
        await scheduler.run(turn_id=1, pcm_iter=_aiter([]), adapter=adapter)
        adapter.send_say.assert_not_called()

    @pytest.mark.asyncio
    async def test_skips_empty_chunks(self):
        adapter = MagicMock()
        adapter.send_say = AsyncMock()
        chunks = [b"", b"\x01\x02" * 8, b"", b"\x03\x04" * 8]
        scheduler = OutputScheduler()
        await scheduler.run(turn_id=1, pcm_iter=_aiter(chunks), adapter=adapter)
        assert adapter.send_say.call_count == 2  # apenas os não-vazios

    @pytest.mark.asyncio
    async def test_on_first_audio_called_once(self):
        calls = []
        adapter = MagicMock()
        adapter.send_say = AsyncMock()

        def cb(turn_id):
            calls.append(turn_id)

        chunks = [b"\x00" * CHUNK_BYTES] * 3
        scheduler = OutputScheduler()
        await scheduler.run(
            turn_id=42,
            pcm_iter=_aiter(chunks),
            adapter=adapter,
            on_first_audio=cb,
        )

        assert calls == [42]  # chamado apenas uma vez com turn_id correto

    @pytest.mark.asyncio
    async def test_on_first_audio_not_called_on_empty(self):
        calls = []
        scheduler = OutputScheduler()
        await scheduler.run(
            turn_id=1,
            pcm_iter=_aiter([]),
            adapter=None,
            on_first_audio=lambda t: calls.append(t),
        )
        assert calls == []

    @pytest.mark.asyncio
    async def test_cancellable(self):
        """Task cancelada no meio não vaza."""
        adapter = MagicMock()
        adapter.send_say = AsyncMock()

        async def _slow_iter():
            for _ in range(100):
                yield b"\x00" * CHUNK_BYTES
                await asyncio.sleep(0)  # yield control

        scheduler = OutputScheduler()
        task = asyncio.create_task(
            scheduler.run(turn_id=1, pcm_iter=_slow_iter(), adapter=adapter)
        )
        await asyncio.sleep(0)  # deixa iniciar
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

        # Deve ter enviado alguns chunks mas não todos
        assert adapter.send_say.call_count < 100

    @pytest.mark.asyncio
    async def test_adapter_error_does_not_crash(self):
        adapter = MagicMock()
        adapter.send_say = AsyncMock(side_effect=OSError("send falhou"))

        chunks = [b"\x00" * CHUNK_BYTES] * 3
        scheduler = OutputScheduler()
        # Não deve levantar mesmo com erro no adapter
        await scheduler.run(turn_id=1, pcm_iter=_aiter(chunks), adapter=adapter)
