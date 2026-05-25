"""Speech-to-text providers for the NoiseBot server."""

from __future__ import annotations

import asyncio
import logging
import struct
from abc import ABC, abstractmethod
from concurrent.futures import ThreadPoolExecutor
from typing import Any

from .runtime import FinalTranscript, PartialTranscript, TranscriptQuality

log = logging.getLogger(__name__)

_DEFAULT_MAX_NO_SPEECH = 0.75
_DEFAULT_MIN_LOGPROB = -1.10
_DEFAULT_MAX_COMPRESSION = 2.60
_DEFAULT_MIN_RMS = 140.0


class STTProvider(ABC):
    @abstractmethod
    async def initialize(self) -> None:
        ...

    @abstractmethod
    def feed(self, pcm: bytes) -> None:
        ...

    @abstractmethod
    async def partial(self, turn_id: int) -> PartialTranscript:
        ...

    @abstractmethod
    async def finalize(self, full_pcm: bytes, turn_id: int) -> FinalTranscript:
        ...

    @abstractmethod
    async def reset(self) -> None:
        ...


def _compute_rms(pcm: bytes) -> float:
    if not pcm:
        return 0.0
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack(f"<{n}h", pcm[: n * 2])
    return (sum(sample * sample for sample in samples) / n) ** 0.5


class WhisperLocalSTT(STTProvider):
    """Local faster-whisper STT running in a dedicated thread pool."""

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
        self._model: Any = None
        self._executor: ThreadPoolExecutor | None = None
        self._partial_text = ""
        self._initialized = False

    async def initialize(self) -> None:
        if self._initialized:
            return
        try:
            from faster_whisper import WhisperModel  # type: ignore[import]
        except ImportError as exc:
            raise RuntimeError(
                "faster-whisper nao instalado. Instale com: pip install noisebot-server[stt]"
            ) from exc

        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="nb_stt")
        loop = asyncio.get_running_loop()

        log.info(
            "STT: carregando modelo %s device=%s compute=%s",
            self._model_name,
            self._device,
            self._compute_type,
        )

        def _load():
            return WhisperModel(
                self._model_name,
                device=self._device,
                compute_type=self._compute_type,
            )

        self._model = await loop.run_in_executor(self._executor, _load)
        self._initialized = True
        log.info("STT: modelo %s carregado.", self._model_name)

    def feed(self, pcm: bytes) -> None:
        pass

    async def partial(self, turn_id: int) -> PartialTranscript:
        return PartialTranscript(turn_id=turn_id, text=self._partial_text, stable=False)

    async def finalize(self, full_pcm: bytes, turn_id: int) -> FinalTranscript:
        if self._model is None:
            raise RuntimeError("WhisperLocalSTT.initialize() nao foi chamado")

        rms = _compute_rms(full_pcm)
        if rms < self._min_rms:
            log.debug("STT: RMS=%.1f < %.1f -- LOW_RMS", rms, self._min_rms)
            return FinalTranscript(
                turn_id=turn_id,
                text="",
                quality=TranscriptQuality.LOW_RMS,
            )

        loop = asyncio.get_running_loop()
        try:
            text, quality, no_speech_prob, avg_logprob, compression_ratio = (
                await loop.run_in_executor(self._executor, self._transcribe_sync, full_pcm)
            )
        except Exception:
            log.exception("STT: erro durante transcricao turn_id=%d", turn_id)
            return FinalTranscript(
                turn_id=turn_id,
                text="",
                quality=TranscriptQuality.NO_SPEECH,
            )

        log.info(
            "STT turn_id=%d quality=%s text=%r no_speech=%.2f logprob=%.2f comp=%.2f",
            turn_id,
            quality.name,
            text[:60],
            no_speech_prob,
            avg_logprob,
            compression_ratio,
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
        self._partial_text = ""

    def _transcribe_sync(
        self,
        full_pcm: bytes,
    ) -> tuple[str, TranscriptQuality, float, float, float]:
        import numpy as np  # type: ignore[import]

        n = len(full_pcm) // 2
        samples = np.frombuffer(full_pcm[: n * 2], dtype=np.int16).astype(np.float32)
        samples = samples / 32768.0

        segments, _info = self._model.transcribe(
            samples,
            language=self._language,
            beam_size=self._beam_size,
            vad_filter=False,
        )

        texts: list[str] = []
        no_speech_probs: list[float] = []
        avg_logprobs: list[float] = []
        compression_ratios: list[float] = []

        for segment in segments:
            texts.append(segment.text.strip())
            no_speech_probs.append(segment.no_speech_prob)
            avg_logprobs.append(segment.avg_logprob)
            compression_ratios.append(segment.compression_ratio)

        text = " ".join(item for item in texts if item).strip()
        no_speech_prob = (
            sum(no_speech_probs) / len(no_speech_probs) if no_speech_probs else 1.0
        )
        avg_logprob = sum(avg_logprobs) / len(avg_logprobs) if avg_logprobs else -2.0
        compression_ratio = max(compression_ratios) if compression_ratios else 0.0

        if not text:
            return text, TranscriptQuality.EMPTY, no_speech_prob, avg_logprob, compression_ratio
        if no_speech_prob > self._max_no_speech_prob:
            return text, TranscriptQuality.NO_SPEECH, no_speech_prob, avg_logprob, compression_ratio
        if avg_logprob < self._min_avg_logprob:
            return text, TranscriptQuality.LOW_LOGPROB, no_speech_prob, avg_logprob, compression_ratio
        if compression_ratio > self._max_compression_ratio:
            return (
                text,
                TranscriptQuality.HIGH_COMPRESSION,
                no_speech_prob,
                avg_logprob,
                compression_ratio,
            )
        return text, TranscriptQuality.GOOD, no_speech_prob, avg_logprob, compression_ratio

    async def close(self) -> None:
        if self._executor is not None:
            self._executor.shutdown(wait=False)
            self._executor = None
        self._model = None
        self._initialized = False


__all__ = ["STTProvider", "WhisperLocalSTT"]
