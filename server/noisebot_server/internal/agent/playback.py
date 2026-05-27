"""Paced SAY output scheduler for firmware playback."""

from __future__ import annotations

import asyncio
import inspect
import logging
import os
import time
from collections.abc import AsyncIterator, Callable
from typing import Any

log = logging.getLogger(__name__)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default

CHUNK_SAMPLES = 256
SAMPLE_RATE = 16_000
CHUNK_DURATION_S = CHUNK_SAMPLES / SAMPLE_RATE
FIRMWARE_SAY_QUEUE = max(4, min(16, _env_int("NOISEBOT_TTS_QUEUE_TARGET", 12)))


class OutputScheduler:
    """Send PCM chunks to firmware while respecting its SAY queue."""

    def __init__(self) -> None:
        self._chunks_sent = 0
        self._t_first: float | None = None

    async def run(
        self,
        turn_id: int,
        pcm_iter: AsyncIterator[bytes],
        adapter: Any,
        on_first_audio: Callable[[int], Any] | None = None,
        on_audio_progress: Callable[[int], None] | None = None,
    ) -> None:
        self._chunks_sent = 0
        self._t_first = None

        try:
            async for chunk in pcm_iter:
                if not chunk:
                    continue

                if self._t_first is None:
                    self._t_first = time.monotonic()
                    await _maybe_call_adapter(adapter, "send_say_begin", turn_id)
                    if on_first_audio is not None:
                        result = on_first_audio(turn_id)
                        if inspect.isawaitable(result):
                            await result

                elapsed = time.monotonic() - self._t_first
                chunks_played = int(elapsed / CHUNK_DURATION_S)
                buffer_fill = self._chunks_sent - chunks_played

                if buffer_fill >= FIRMWARE_SAY_QUEUE:
                    sleep_s = (buffer_fill - FIRMWARE_SAY_QUEUE + 1) * CHUNK_DURATION_S
                    await asyncio.sleep(sleep_s)

                if adapter is not None:
                    try:
                        await adapter.send_say(chunk)
                    except Exception as exc:
                        log.exception(
                            "OutputScheduler: erro ao enviar SAY turn_id=%d",
                            turn_id,
                        )
                        raise ConnectionError("falha ao enviar audio ao firmware") from exc

                self._chunks_sent += 1
                if on_audio_progress is not None:
                    on_audio_progress(turn_id)
                await asyncio.sleep(0)
        finally:
            close_iter = getattr(pcm_iter, "aclose", None)
            if close_iter is not None:
                result = close_iter()
                if inspect.isawaitable(result):
                    await result

        if self._chunks_sent:
            await _maybe_call_adapter(adapter, "send_say_end", turn_id)
            log.debug(
                "OutputScheduler: turn_id=%d %d chunks (%.1f s)",
                turn_id,
                self._chunks_sent,
                self._chunks_sent * CHUNK_DURATION_S,
            )


async def _maybe_call_adapter(adapter: Any, method_name: str, *args: Any) -> None:
    if adapter is None:
        return
    method = getattr(adapter, method_name, None)
    if method is None:
        return
    try:
        result = method(*args)
        if inspect.isawaitable(result):
            await result
    except Exception:
        log.exception("OutputScheduler: erro em %s", method_name)


__all__ = [
    "CHUNK_DURATION_S",
    "CHUNK_SAMPLES",
    "FIRMWARE_SAY_QUEUE",
    "OutputScheduler",
    "SAMPLE_RATE",
]
