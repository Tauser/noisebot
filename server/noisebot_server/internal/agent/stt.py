"""Speech-to-text providers for the NoiseBot server."""

from __future__ import annotations

import asyncio
import logging
import os
import re
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
_DEFAULT_MIN_PEAK = 1600
_REPEATED_TOKEN_MIN_WORDS = 10


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


def _compute_peak(pcm: bytes) -> int:
    if not pcm:
        return 0
    n = len(pcm) // 2
    if n == 0:
        return 0
    samples = struct.unpack(f"<{n}h", pcm[: n * 2])
    return max((abs(sample) for sample in samples), default=0)


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
        min_peak: int = _DEFAULT_MIN_PEAK,
        beam_size: int = 5,
        denoise_enabled: bool = False,
        vad_filter_enabled: bool = False,
        noise_gate_mult: float = 1.8,
        noise_gate_floor: float = 0.003,
    ) -> None:
        self._model_name = model
        self._device = device
        self._compute_type = compute_type
        self._language = language
        self._max_no_speech_prob = max_no_speech_prob
        self._min_avg_logprob = min_avg_logprob
        self._max_compression_ratio = max_compression_ratio
        self._min_rms = min_rms
        self._min_peak = min_peak
        self._beam_size = beam_size
        self._denoise_enabled = denoise_enabled
        self._vad_filter_enabled = vad_filter_enabled
        self._noise_gate_mult = max(1.0, noise_gate_mult)
        self._noise_gate_floor = max(0.0, noise_gate_floor)
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
        peak = _compute_peak(full_pcm)
        if rms < self._min_rms:
            log.debug("STT: RMS=%.1f < %.1f -- LOW_RMS", rms, self._min_rms)
            return FinalTranscript(
                turn_id=turn_id,
                text="",
                quality=TranscriptQuality.LOW_RMS,
            )
        if peak < self._min_peak:
            log.debug("STT: peak=%d < %d -- LOW_RMS", peak, self._min_peak)
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
        if self._denoise_enabled:
            samples = _preprocess_speech_samples(
                samples,
                noise_gate_mult=self._noise_gate_mult,
                noise_gate_floor=self._noise_gate_floor,
            )

        segments, _info = self._model.transcribe(
            samples,
            language=self._language,
            beam_size=self._beam_size,
            condition_on_previous_text=False,
            compression_ratio_threshold=self._max_compression_ratio,
            log_prob_threshold=self._min_avg_logprob,
            no_speech_threshold=self._max_no_speech_prob,
            temperature=0.0,
            vad_filter=self._vad_filter_enabled,
            vad_parameters=(
                {
                    "min_silence_duration_ms": 500,
                    "speech_pad_ms": 200,
                }
                if self._vad_filter_enabled
                else None
            ),
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
        if _looks_like_repetition_loop(text):
            return text, TranscriptQuality.HIGH_COMPRESSION, no_speech_prob, avg_logprob, compression_ratio
        return text, TranscriptQuality.GOOD, no_speech_prob, avg_logprob, compression_ratio

    async def close(self) -> None:
        if self._executor is not None:
            self._executor.shutdown(wait=False)
            self._executor = None
        self._model = None
        self._initialized = False


def _preprocess_speech_samples(
    samples,
    *,
    noise_gate_mult: float,
    noise_gate_floor: float,
):
    """Lightweight speech cleanup before Whisper.

    It removes DC offset, applies a gentle high-pass filter and attenuates
    frames whose RMS looks like room noise. This is not a neural denoiser; it is
    a cheap guardrail against fan/room hiss becoming hallucinated text.
    """
    import numpy as np  # type: ignore[import]

    if samples.size < 320:
        return samples

    cleaned = samples - np.mean(samples)

    prev_x = 0.0
    prev_y = 0.0
    high_passed = np.empty_like(cleaned)
    for i, value in enumerate(cleaned):
        y = float(value) - prev_x + 0.995 * prev_y
        high_passed[i] = y
        prev_x = float(value)
        prev_y = y

    frame = 320  # 20 ms at 16 kHz.
    frame_count = high_passed.size // frame
    if frame_count <= 0:
        return high_passed

    trimmed = high_passed[: frame_count * frame].reshape(frame_count, frame)
    rms = np.sqrt(np.mean(trimmed * trimmed, axis=1))
    noise_rms = float(np.percentile(rms, 20))
    threshold = max(noise_gate_floor, noise_rms * noise_gate_mult)

    gains = np.where(rms >= threshold, 1.0, 0.18).astype(np.float32)
    smoothed = np.convolve(gains, np.array([0.25, 0.5, 0.25], dtype=np.float32), mode="same")
    gated = high_passed.copy()
    gated[: frame_count * frame] *= np.repeat(smoothed, frame)
    return np.clip(gated, -1.0, 1.0)


def _looks_like_repetition_loop(text: str) -> bool:
    words = re.findall(r"[\wÀ-ÿ]+", text.casefold())
    if len(words) < _REPEATED_TOKEN_MIN_WORDS:
        return False

    unique_ratio = len(set(words)) / len(words)
    if unique_ratio <= 0.35:
        return True

    for size in (2, 3, 4):
        if len(words) < size * 4:
            continue
        repeated = 0
        total = len(words) - size
        for i in range(total):
            if words[i : i + size] == words[i + size : i + size * 2]:
                repeated += 1
        if total > 0 and repeated / total >= 0.35:
            return True
    return False


def _env_bool(key: str, default: bool) -> bool:
    raw = os.environ.get(key)
    if raw is None:
        return default
    return raw.strip().lower() not in {"0", "false", "off", "no", "nao", "não"}


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


__all__ = ["STTProvider", "WhisperLocalSTT"]
