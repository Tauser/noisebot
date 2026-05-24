"""bridgev2.audio.playback — Output Scheduler: pacing de SAY para firmware (Fase 6).

A fila SAY do firmware tem apenas 4 chunks (~64 ms). Enviar mais rápido
que o firmware consome resulta em overflow silencioso. O scheduler monitora
quantos chunks o firmware já consumiu desde o início da reprodução e espera
quando a fila virtual está cheia.

Cancelável via asyncio.Task.cancel() para barge-in.
"""
from __future__ import annotations

import asyncio
import inspect
import logging
import time
from typing import Any, AsyncIterator, Callable

log = logging.getLogger(__name__)

CHUNK_SAMPLES = 256                             # int16 amostras por chunk
SAMPLE_RATE = 16_000                            # Hz
CHUNK_DURATION_S = CHUNK_SAMPLES / SAMPLE_RATE  # 16 ms por chunk
FIRMWARE_SAY_QUEUE = 4                          # chunks máximos na fila SAY


class OutputScheduler:
    """Envia chunks PCM para o firmware respeitando a fila de 4 chunks.

    Estratégia: rastreia quantos chunks o firmware já deve ter reproduzido
    (tempo decorrido / duração por chunk) e aguarda quando o buffer virtual
    ficaria cheio. Isso mantém a fila entre 1–4 chunks sem underrun.

    Uso típico:
        scheduler = OutputScheduler()
        await scheduler.run(turn_id, pcm_iter, adapter, on_first_audio=cb)
    """

    def __init__(self) -> None:
        self._chunks_sent = 0
        self._t_first: float | None = None

    async def run(
        self,
        turn_id: int,
        pcm_iter: AsyncIterator[bytes],
        adapter: Any,
        on_first_audio: Callable[[int], None] | None = None,
        on_audio_progress: Callable[[int], None] | None = None,
    ) -> None:
        """Drena pcm_iter, enviando chunks ao firmware com pacing baseado em tempo.

        Args:
            turn_id:        identificador do turno.
            pcm_iter:       iterador async de chunks PCM (int16 LE, ≤512 bytes).
            adapter:        FirmwareAdapter ou None (modo headless).
            on_first_audio: callback chamado com turn_id ao enviar o 1º chunk.
            on_audio_progress: callback chamado a cada chunk enviado.
        """
        self._chunks_sent = 0
        self._t_first = None

        async for chunk in pcm_iter:
            if not chunk:
                continue

            if self._t_first is None:
                self._t_first = time.monotonic()
                await _maybe_call_adapter(adapter, "send_say_begin", turn_id)
                if on_first_audio is not None:
                    on_first_audio(turn_id)

            # Pacing: quantos chunks o firmware já reproduziu?
            elapsed = time.monotonic() - self._t_first
            chunks_played = int(elapsed / CHUNK_DURATION_S)
            buffer_fill = self._chunks_sent - chunks_played

            if buffer_fill >= FIRMWARE_SAY_QUEUE:
                # Fila cheia — aguarda pelo menos 1 slot livre
                sleep_s = (buffer_fill - FIRMWARE_SAY_QUEUE + 1) * CHUNK_DURATION_S
                await asyncio.sleep(sleep_s)

            if adapter is not None:
                try:
                    await adapter.send_say(chunk)
                except Exception:
                    log.exception(
                        "OutputScheduler: erro ao enviar SAY turn_id=%d", turn_id
                    )

            self._chunks_sent += 1
            if on_audio_progress is not None:
                on_audio_progress(turn_id)
            # Cede o event loop entre chunks para não bloquear outros coroutines
            await asyncio.sleep(0)

        if self._chunks_sent:
            await _maybe_call_adapter(adapter, "send_say_end", turn_id)

        if self._chunks_sent:
            log.debug(
                "OutputScheduler: turn_id=%d %d chunks (%.1f s)",
                turn_id,
                self._chunks_sent,
                self._chunks_sent * CHUNK_DURATION_S,
            )


async def _maybe_call_adapter(adapter: Any, method_name: str, *args: Any) -> None:
    """Chama método opcional do adapter, aceitando adapters mockados nos testes."""
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
