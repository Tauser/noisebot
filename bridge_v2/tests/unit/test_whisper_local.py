"""Testes unitários: WhisperLocalSTT — mock de faster-whisper, qualidade, rejeições.

Usa unittest.mock para substituir faster-whisper e numpy, permitindo testar
toda a lógica de rejeição de qualidade sem carregar o modelo real.

Cobre:
  - finalize() retorna FinalTranscript com quality GOOD para transcrição válida
  - Rejeição LOW_RMS: RMS do PCM abaixo do limiar → sem chamar o modelo
  - Rejeição NO_SPEECH: no_speech_prob > max_no_speech_prob
  - Rejeição LOW_LOGPROB: avg_logprob < min_avg_logprob
  - Rejeição HIGH_COMPRESSION: compression_ratio > max_compression_ratio
  - Rejeição EMPTY: texto vazio após transcrição
  - turn_id propagado corretamente
  - reset() limpa texto parcial
  - close() desliga executor sem erro
  - initialize() lança RuntimeError se faster-whisper não instalado
"""
from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from bridgev2.stt.whisper_local import WhisperLocalSTT, _compute_rms
from bridgev2.runtime.events import FinalTranscript, TranscriptQuality


# ── Helpers ────────────────────────────────────────────────────────────────────

def _make_pcm(n_samples: int = 4096, amplitude: int = 8000) -> bytes:
    """PCM int16 LE sintético com amplitude definida (RMS ≈ amplitude / √2)."""
    # onda quadrada: metade positiva, metade negativa
    half = n_samples // 2
    samples = [amplitude] * half + [-amplitude] * (n_samples - half)
    return struct.pack(f"<{n_samples}h", *samples)


def _silent_pcm(n_samples: int = 4096) -> bytes:
    return struct.pack(f"<{n_samples}h", *([0] * n_samples))


def _make_segment(
    text: str = "olá",
    no_speech_prob: float = 0.05,
    avg_logprob: float = -0.3,
    compression_ratio: float = 1.2,
) -> MagicMock:
    seg = MagicMock()
    seg.text = text
    seg.no_speech_prob = no_speech_prob
    seg.avg_logprob = avg_logprob
    seg.compression_ratio = compression_ratio
    return seg


def _mock_model(segments=None, info=None):
    """Cria um mock de WhisperModel que retorna os segmentos fornecidos."""
    if segments is None:
        segments = [_make_segment()]
    model = MagicMock()
    model.transcribe.return_value = (iter(segments), info or MagicMock())
    return model


async def _init_with_mock(stt: WhisperLocalSTT, model_mock) -> None:
    """Inicializa o STT injetando um modelo mock diretamente."""
    from concurrent.futures import ThreadPoolExecutor
    stt._executor = ThreadPoolExecutor(max_workers=1)
    stt._model = model_mock


# ── _compute_rms ──────────────────────────────────────────────────────────────

class TestComputeRms:
    def test_silent_pcm_zero_rms(self):
        assert _compute_rms(_silent_pcm()) == pytest.approx(0.0)

    def test_constant_amplitude(self):
        pcm = struct.pack("<4h", 1000, 1000, 1000, 1000)
        assert _compute_rms(pcm) == pytest.approx(1000.0)

    def test_empty_pcm_zero(self):
        assert _compute_rms(b"") == pytest.approx(0.0)

    def test_square_wave_amplitude(self):
        # onda quadrada: RMS = amplitude
        pcm = _make_pcm(n_samples=8, amplitude=5000)
        rms = _compute_rms(pcm)
        assert rms == pytest.approx(5000.0, rel=0.01)


# ── Inicialização ─────────────────────────────────────────────────────────────

class TestInitialize:
    async def test_initialize_raises_if_no_faster_whisper(self):
        """ImportError de faster-whisper → RuntimeError claro."""
        stt = WhisperLocalSTT(model="small")
        with patch.dict("sys.modules", {"faster_whisper": None}):
            with pytest.raises(RuntimeError, match="faster-whisper"):
                await stt.initialize()

    async def test_initialize_loads_model(self):
        """initialize() chama WhisperModel com os parâmetros corretos."""
        stt = WhisperLocalSTT(model="tiny", device="cpu", compute_type="int8")
        mock_model_cls = MagicMock(return_value=MagicMock())

        with patch.dict("sys.modules", {"faster_whisper": MagicMock(WhisperModel=mock_model_cls)}):
            await stt.initialize()

        mock_model_cls.assert_called_once_with("tiny", device="cpu", compute_type="int8")
        await stt.close()


# ── Rejeição por RMS ──────────────────────────────────────────────────────────

class TestRmsRejection:
    async def test_silent_pcm_rejected_low_rms(self):
        """PCM silencioso (RMS ≈ 0) → LOW_RMS sem chamar o modelo."""
        stt = WhisperLocalSTT(min_rms=100.0)
        model = _mock_model()
        await _init_with_mock(stt, model)

        ft = await stt.finalize(_silent_pcm(4096), turn_id=1)
        assert ft.quality == TranscriptQuality.LOW_RMS
        assert ft.text == ""
        model.transcribe.assert_not_called()
        await stt.close()

    async def test_loud_pcm_passes_rms(self):
        """PCM com amplitude alta passa a rejeição de RMS."""
        stt = WhisperLocalSTT(min_rms=100.0)
        model = _mock_model([_make_segment("tudo bem", no_speech_prob=0.02)])
        await _init_with_mock(stt, model)

        pcm = _make_pcm(n_samples=8000, amplitude=8000)
        ft = await stt.finalize(pcm, turn_id=2)
        assert ft.quality == TranscriptQuality.GOOD
        await stt.close()


# ── Rejeições de qualidade do modelo ─────────────────────────────────────────

class TestQualityRejections:
    async def _run(self, segments, **kwargs) -> FinalTranscript:
        stt = WhisperLocalSTT(**kwargs)
        model = _mock_model(segments)
        await _init_with_mock(stt, model)
        pcm = _make_pcm(n_samples=8000, amplitude=5000)
        ft = await stt.finalize(pcm, turn_id=1)
        await stt.close()
        return ft

    async def test_no_speech_rejection(self):
        seg = _make_segment("ruído", no_speech_prob=0.90)
        ft = await self._run([seg], max_no_speech_prob=0.75)
        assert ft.quality == TranscriptQuality.NO_SPEECH

    async def test_low_logprob_rejection(self):
        seg = _make_segment("mumbling", avg_logprob=-1.5)
        ft = await self._run([seg], min_avg_logprob=-1.10)
        assert ft.quality == TranscriptQuality.LOW_LOGPROB

    async def test_high_compression_rejection(self):
        seg = _make_segment("hallucination " * 20, compression_ratio=3.0)
        ft = await self._run([seg], max_compression_ratio=2.60)
        assert ft.quality == TranscriptQuality.HIGH_COMPRESSION

    async def test_empty_text_rejection(self):
        seg = _make_segment("   ")  # só espaço
        ft = await self._run([seg])
        assert ft.quality == TranscriptQuality.EMPTY

    async def test_good_transcript(self):
        seg = _make_segment("olá tudo bem", no_speech_prob=0.02, avg_logprob=-0.3)
        ft = await self._run([seg])
        assert ft.quality == TranscriptQuality.GOOD
        assert "olá" in ft.text or "tudo" in ft.text


# ── FinalTranscript campos ─────────────────────────────────────────────────────

class TestFinalTranscriptFields:
    async def test_turn_id_propagated(self):
        stt = WhisperLocalSTT()
        model = _mock_model([_make_segment("teste")])
        await _init_with_mock(stt, model)
        pcm = _make_pcm(n_samples=8000, amplitude=5000)

        ft = await stt.finalize(pcm, turn_id=42)
        assert ft.turn_id == 42
        await stt.close()

    async def test_metrics_populated(self):
        seg = _make_segment("ok", no_speech_prob=0.1, avg_logprob=-0.5, compression_ratio=1.3)
        stt = WhisperLocalSTT()
        model = _mock_model([seg])
        await _init_with_mock(stt, model)
        pcm = _make_pcm(n_samples=8000, amplitude=5000)

        ft = await stt.finalize(pcm, turn_id=1)
        assert ft.no_speech_prob == pytest.approx(0.1)
        assert ft.avg_logprob == pytest.approx(-0.5)
        assert ft.compression_ratio == pytest.approx(1.3)
        await stt.close()

    async def test_multiple_segments_concatenated(self):
        segs = [
            _make_segment("Olá"),
            _make_segment("como vai?"),
        ]
        stt = WhisperLocalSTT()
        model = _mock_model(segs)
        await _init_with_mock(stt, model)
        pcm = _make_pcm(n_samples=8000, amplitude=5000)

        ft = await stt.finalize(pcm, turn_id=1)
        assert "Olá" in ft.text
        assert "como vai?" in ft.text
        await stt.close()


# ── reset e feed ──────────────────────────────────────────────────────────────

class TestResetAndFeed:
    async def test_reset_clears_partial(self):
        stt = WhisperLocalSTT()
        stt._partial_text = "texto parcial"
        await stt.reset()
        assert stt._partial_text == ""

    async def test_feed_is_noop_fase4(self):
        """feed() é no-op em Fase 4 (sem crash)."""
        stt = WhisperLocalSTT()
        pcm = _make_pcm(256, amplitude=1000)
        stt.feed(pcm)  # não deve lançar exceção

    async def test_partial_returns_empty(self):
        stt = WhisperLocalSTT()
        result = await stt.partial(turn_id=5)
        from bridgev2.runtime.events import PartialTranscript
        assert isinstance(result, PartialTranscript)
        assert result.turn_id == 5


# ── Erro ao chamar sem initialize ─────────────────────────────────────────────

class TestUninitializedError:
    async def test_finalize_raises_if_not_initialized(self):
        stt = WhisperLocalSTT()
        with pytest.raises(RuntimeError, match="initialize"):
            await stt.finalize(_make_pcm(), turn_id=1)
