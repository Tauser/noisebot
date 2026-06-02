"""Paced SAY output scheduler for firmware playback."""

from __future__ import annotations

import asyncio
import inspect
import logging
import os
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from typing import Any

log = logging.getLogger(__name__)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default

CHUNK_SAMPLES = 256
SAMPLE_RATE = 16_000
CHUNK_BYTES = CHUNK_SAMPLES * 2
CHUNK_DURATION_S = CHUNK_SAMPLES / SAMPLE_RATE
FIRMWARE_SAY_QUEUE = max(0, min(16, _env_int("NOISEBOT_TTS_QUEUE_TARGET", 0)))
SAY_SEND_INTERVAL_S = max(
    CHUNK_DURATION_S,
    min(0.040, _env_float("NOISEBOT_TTS_SEND_INTERVAL_MS", 16.0) / 1000.0),
)
SAY_STARTUP_CHUNKS = max(0, min(256, _env_int("NOISEBOT_TTS_STARTUP_CHUNKS", 0)))
SAY_STARTUP_INTERVAL_S = max(
    SAY_SEND_INTERVAL_S,
    min(0.080, _env_float("NOISEBOT_TTS_STARTUP_INTERVAL_MS", 24.0) / 1000.0),
)


@dataclass(frozen=True)
class PlaybackStats:
    """Observed SAY output for one spoken turn."""

    chunks_sent: int = 0
    pcm_bytes_in: int = 0
    pcm_bytes_sent: int = 0
    padding_bytes: int = 0
    say_begin_sent: bool = False
    say_end_sent: bool = False


class OutputScheduler:
    """Send PCM chunks to firmware while respecting its SAY queue."""

    def __init__(self) -> None:
        self._chunks_sent = 0
        self._t_first: float | None = None
        self._next_send_at: float | None = None

    async def run(
        self,
        turn_id: int,
        pcm_iter: AsyncIterator[bytes],
        adapter: Any,
        on_first_audio: Callable[[int], Any] | None = None,
        on_audio_progress: Callable[[int], None] | None = None,
    ) -> PlaybackStats:
        self._chunks_sent = 0
        self._t_first = None
        self._next_send_at = None
        pcm_bytes_in = 0
        pcm_bytes_sent = 0
        padding_bytes = 0
        say_begin_sent = False

        try:
            pending = bytearray()
            async for chunk in pcm_iter:
                if not chunk:
                    continue
                pcm_bytes_in += len(chunk)
                pending.extend(chunk)

                while len(pending) >= CHUNK_BYTES:
                    say_chunk = bytes(pending[:CHUNK_BYTES])
                    del pending[:CHUNK_BYTES]

                    chunk_begin_sent = await self._send_chunk(
                        turn_id,
                        say_chunk,
                        adapter,
                        on_first_audio,
                        on_audio_progress,
                    )
                    say_begin_sent = say_begin_sent or chunk_begin_sent
                    pcm_bytes_sent += len(say_chunk)

                await asyncio.sleep(0)

            if pending:
                pad = CHUNK_BYTES - len(pending)
                pending.extend(b"\x00" * pad)
                chunk_begin_sent = await self._send_chunk(
                    turn_id,
                    bytes(pending),
                    adapter,
                    on_first_audio,
                    on_audio_progress,
                )
                say_begin_sent = say_begin_sent or chunk_begin_sent
                pcm_bytes_sent += CHUNK_BYTES
                padding_bytes += pad
        finally:
            close_iter = getattr(pcm_iter, "aclose", None)
            if close_iter is not None:
                result = close_iter()
                if inspect.isawaitable(result):
                    await result

        say_end_sent = False
        if self._chunks_sent:
            say_end_sent = await _maybe_call_adapter(adapter, "send_say_end", turn_id)
            log.debug(
                "OutputScheduler: turn_id=%d %d chunks (%.1f s)",
                turn_id,
                self._chunks_sent,
                self._chunks_sent * CHUNK_DURATION_S,
            )
        return PlaybackStats(
            chunks_sent=self._chunks_sent,
            pcm_bytes_in=pcm_bytes_in,
            pcm_bytes_sent=pcm_bytes_sent,
            padding_bytes=padding_bytes,
            say_begin_sent=say_begin_sent,
            say_end_sent=say_end_sent,
        )

    async def _send_chunk(
        self,
        turn_id: int,
        chunk: bytes,
        adapter: Any,
        on_first_audio: Callable[[int], Any] | None,
        on_audio_progress: Callable[[int], None] | None,
    ) -> bool:
        if len(chunk) != CHUNK_BYTES:
            raise ValueError(f"SAY chunk invalido: {len(chunk)} bytes")

        say_begin_sent = False
        if self._t_first is None:
            self._t_first = time.monotonic()
            say_begin_sent = await _maybe_call_adapter(adapter, "send_say_begin", turn_id)
            if on_first_audio is not None:
                result = on_first_audio(turn_id)
                if inspect.isawaitable(result):
                    await result

        await self._pace_chunk()

        if adapter is not None:
            try:
                await adapter.send_say(chunk)
            except Exception as exc:
                if isinstance(exc, ConnectionError) and str(exc) == "SPEECH_CANCEL":
                    log.info("OutputScheduler: SAY cancelado por SPEECH_CANCEL turn_id=%d", turn_id)
                    raise asyncio.CancelledError("speech_cancel") from exc
                log.exception(
                    "OutputScheduler: erro ao enviar SAY turn_id=%d",
                    turn_id,
                )
                raise ConnectionError("falha ao enviar audio ao firmware") from exc

        self._chunks_sent += 1
        if on_audio_progress is not None:
            on_audio_progress(turn_id)
        return say_begin_sent

    async def _pace_chunk(self) -> None:
        now = time.monotonic()
        interval_s = self._chunk_interval_s()
        if self._chunks_sent < FIRMWARE_SAY_QUEUE:
            self._next_send_at = now + interval_s
            return

        if self._next_send_at is None or self._next_send_at < now:
            self._next_send_at = now

        sleep_s = self._next_send_at - now
        if sleep_s > 0:
            await asyncio.sleep(sleep_s)

        self._next_send_at += interval_s

    def _chunk_interval_s(self) -> float:
        if self._chunks_sent < SAY_STARTUP_CHUNKS:
            return SAY_STARTUP_INTERVAL_S
        return SAY_SEND_INTERVAL_S


async def _maybe_call_adapter(adapter: Any, method_name: str, *args: Any) -> bool:
    if adapter is None:
        return False
    method = getattr(adapter, method_name, None)
    if method is None:
        return False
    try:
        result = method(*args)
        if inspect.isawaitable(result):
            await result
        return True
    except Exception:
        log.exception("OutputScheduler: erro em %s", method_name)
        return False


__all__ = [
    "CHUNK_DURATION_S",
    "CHUNK_BYTES",
    "CHUNK_SAMPLES",
    "FIRMWARE_SAY_QUEUE",
    "OutputScheduler",
    "PlaybackStats",
    "SAY_SEND_INTERVAL_S",
    "SAY_STARTUP_CHUNKS",
    "SAY_STARTUP_INTERVAL_S",
    "SAMPLE_RATE",
]
