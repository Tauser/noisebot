"""bridgev2.stt.whisper_local — WhisperLocalSTT: faster-whisper em thread pool.

Executa a transcrição final em run_in_executor para não bloquear o event loop.
Portado do legacy WhisperStt com rejeições de qualidade alinhadas ao AudioConfig.

Rejeições de qualidade (mesmos critérios do audio_hal / AudioConfig):
  - RMS < min_transcribe_rms → LOW_RMS
  - no_speech_prob > max_no_speech_prob → NO_SPEECH
  - avg_logprob < min_avg_logprob → LOW_LOGPROB
  - compression_ratio > max_compression_ratio → HIGH_COMPRESSION
  - texto vazio após strip → EMPTY
"""
from __future__ import annotations

import asyncio
import logging
import struct
from concurrent.futures import ThreadPoolExecutor
from typing import Any

from .base import STTProvider
from ..runtime.events import FinalTranscript, PartialTranscript, TranscriptQuality

log = logging.getLogger(__name__)

# Limiar padrão de qualidade (sobrescritos pelo AudioConfig quando injetado)
_DEFAULT_MAX_NO_SPEECH = 0.75
_DEFAULT_MIN_LOGPROB = -1.10
_DEFAULT_MAX_COMPRESSION = 2.60
_DEFAULT_MIN_RMS = 140.0


def _compute_rms(pcm: bytes) -> float:
    """RMS do PCM int16 LE."""
    if not pcm:
        return 0.0
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack(f"<{n}h", pcm[:n * 2])
    return (sum(s * s for s in samples) / n) ** 0.5


class WhisperLocalSTT(STTProvider):
    """STT via faster-whisper executado em thread pool separado.

    Parâmetros
    ----------
    model : str
        Nome do modelo Whisper (tiny/base/small/medium/large-v3).
    device : str
        "cpu" ou "cuda".
    compute_type : str
        "int8", "float16", "float32" etc.
    language : str | None
        Forçar idioma (None = auto-detect). Usar "pt" para PT-BR.
    max_no_speech_prob : float
    min_avg_logprob : float
    max_compression_ratio : float
    min_rms : float
        Limiares de rejeição de qualidade. Padrões alinhados ao AudioConfig.
    """

    def __init__(
        self,
        model: str = "small",
        device: str = "cpu",
        compute_type: str = "int8",
        language: str | None = "pt",
        max_no_speech_prob: float = _DEFAULT_MAX_NO_SPEECH,
        min_avg_logprob: float = _DEFAULT_MIN_LOGPROB,
        max_compression_ratio: float = _DEFAULT_MAX_COMPRESSION,
        min_rms: float = _DEFAULT_MIN_RMS,
        beam_size: int = 5,
    ) -> None:
        self._model_name = model
        self._device = device
        self._compute_type = compute_type
        self._language = language
        self._max_no_speech_prob = max_no_speech_prob
        self._min_avg_logprob = min_avg_logprob
        self._max_compression_ratio = max_compression_ratio
        self._min_rms = min_rms
        self._beam_size = beam_size

        self._model: Any = None          # WhisperModel (faster-whisper)
        self._executor: ThreadPoolExecutor | None = None
        self._partial_text: str = ""

    # ── STTProvider ────────────────────────────────────────────────────────

    async def initialize(self) -> None:
        """Carrega o modelo Whisper em executor dedicado (bloqueia uma vez)."""
        try:
            from faster_whisper import WhisperModel  # type: ignore[import]
        except ImportError:
            raise RuntimeError(
                "faster-whisper não instalado. "
                "Instale com: pip install bridgev2[stt]"
            )

        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="nb_stt")
        loop = asyncio.get_running_loop()

        log.info(
            "STT: carregando modelo %s device=%s compute=%s",
            self._model_name, self._device, self._compute_type,
        )

        def _load():
            return WhisperModel(
                self._model_name,
                device=self._device,
                compute_type=self._compute_type,
            )

        self._model = await loop.run_in_executor(self._executor, _load)
        log.info("STT: modelo %s carregado.", self._model_name)

    def feed(self, pcm: bytes) -> None:
        """Chunk de áudio para transcrição parcial (Fase 4: no-op; streaming na Fase 5)."""
        pass  # Fase 5 implementará transcrição parcial via streaming

    async def partial(self, turn_id: int) -> PartialTranscript:
        """Estimativa parcial — vazia até implementação de streaming (Fase 5)."""
        return PartialTranscript(turn_id=turn_id, text=self._partial_text, stable=False)

    async def finalize(self, full_pcm: bytes, turn_id: int) -> FinalTranscript:
        """Transcrição final do áudio completo do turno.

        Executa faster-whisper em thread pool. Aplica rejeições de qualidade.
        """
        if self._model is None:
            raise RuntimeError("WhisperLocalSTT.initialize() não foi chamado")

        # Rejeição por RMS antes mesmo de chamar o modelo
        rms = _compute_rms(full_pcm)
        if rms < self._min_rms:
            log.debug("STT: RMS=%.1f < %.1f — rejeição LOW_RMS", rms, self._min_rms)
            return FinalTranscript(
                turn_id=turn_id,
                text="",
                quality=TranscriptQuality.LOW_RMS,
            )

        loop = asyncio.get_running_loop()
        try:
            text, quality, no_speech_prob, avg_logprob, compression_ratio = \
                await loop.run_in_executor(
                    self._executor,
                    self._transcribe_sync,
                    full_pcm,
                )
        except Exception:
            log.exception("STT: erro durante transcrição turn_id=%d", turn_id)
            return FinalTranscript(
                turn_id=turn_id,
                text="",
                quality=TranscriptQuality.NO_SPEECH,
            )

        log.info(
            "STT turn_id=%d quality=%s text=%r no_speech=%.2f logprob=%.2f comp=%.2f",
            turn_id, quality.name, text[:60], no_speech_prob, avg_logprob, compression_ratio,
        )

        return FinalTranscript(
            turn_id=turn_id,
            text=text,
            quality=quality,
            no_speech_prob=no_speech_prob,
            avg_logprob=avg_logprob,
            compression_ratio=compression_ratio,
        )

    async def reset(self) -> None:
        """Limpa estado parcial para novo turno."""
        self._partial_text = ""

    # ── Execução síncrona (thread pool) ───────────────────────────────────

    def _transcribe_sync(
        self,
        full_pcm: bytes,
    ) -> tuple[str, TranscriptQuality, float, float, float]:
        """Executa faster-whisper de forma síncrona no thread pool.

        Retorna (text, quality, no_speech_prob, avg_logprob, compression_ratio).
        """
        import numpy as np  # type: ignore[import]

        # PCM int16 LE → float32 normalizado [-1, 1]
        n = len(full_pcm) // 2
        samples = np.frombuffer(full_pcm[:n * 2], dtype=np.int16).astype(np.float32) / 32768.0

        segments, info = self._model.transcribe(
            samples,
            language=self._language,
            beam_size=self._beam_size,
            vad_filter=False,  # VAD já foi feito no firmware
        )

        # Coleta todos os segmentos
        texts: list[str] = []
        no_speech_probs: list[float] = []
        avg_logprobs: list[float] = []
        compression_ratios: list[float] = []

        for seg in segments:
            texts.append(seg.text.strip())
            no_speech_probs.append(seg.no_speech_prob)
            avg_logprobs.append(seg.avg_logprob)
            compression_ratios.append(seg.compression_ratio)

        text = " ".join(t for t in texts if t).strip()

        # Médias das métricas por segmento
        no_speech_prob = (
            sum(no_speech_probs) / len(no_speech_probs) if no_speech_probs else 1.0
        )
        avg_logprob = (
            sum(avg_logprobs) / len(avg_logprobs) if avg_logprobs else -2.0
        )
        compression_ratio = (
            max(compression_ratios) if compression_ratios else 0.0
        )

        # Rejeições de qualidade
        if not text:
            return text, TranscriptQuality.EMPTY, no_speech_prob, avg_logprob, compression_ratio

        if no_speech_prob > self._max_no_speech_prob:
            return text, TranscriptQuality.NO_SPEECH, no_speech_prob, avg_logprob, compression_ratio

        if avg_logprob < self._min_avg_logprob:
            return text, TranscriptQuality.LOW_LOGPROB, no_speech_prob, avg_logprob, compression_ratio

        if compression_ratio > self._max_compression_ratio:
            return text, TranscriptQuality.HIGH_COMPRESSION, no_speech_prob, avg_logprob, compression_ratio

        return text, TranscriptQuality.GOOD, no_speech_prob, avg_logprob, compression_ratio

    # ── Ciclo de vida ──────────────────────────────────────────────────────

    async def close(self) -> None:
        """Encerra o thread pool do STT."""
        if self._executor is not None:
            self._executor.shutdown(wait=False)
            self._executor = None
