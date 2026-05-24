"""Tests for bridgev2.tts.piper_server — PiperServerTTS."""
from __future__ import annotations

import asyncio
import struct
import wave
import io
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from bridgev2.tts.piper_server import PiperServerTTS, CHUNK_BYTES, _read_exact


def _make_wav_bytes(num_samples: int = 512, sample_rate: int = 16000) -> bytes:
    """Gera bytes de um WAV mínimo com PCM de silêncio."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # int16
        wf.setframerate(sample_rate)
        wf.writeframes(b"\x00\x00" * num_samples)
    return buf.getvalue()


class FakeStreamReader:
    """Simula asyncio.StreamReader com bytes pré-definidos."""

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._pos = 0

    async def read(self, n: int) -> bytes:
        chunk = self._data[self._pos:self._pos + n]
        self._pos += len(chunk)
        return chunk


class TestReadExact:
    @pytest.mark.asyncio
    async def test_reads_exact_bytes(self):
        reader = FakeStreamReader(b"ABCDEFGH")
        data = await _read_exact(reader, 4)
        assert data == b"ABCD"

    @pytest.mark.asyncio
    async def test_reads_in_multiple_chunks(self):
        """Simula stream que entrega 2 bytes por vez."""
        class SlowReader:
            def __init__(self, data):
                self._data = data
                self._pos = 0
            async def read(self, n):
                chunk = self._data[self._pos:self._pos + min(n, 2)]
                self._pos += len(chunk)
                return chunk

        reader = SlowReader(b"ABCDEFGH")
        data = await _read_exact(reader, 6)
        assert data == b"ABCDEF"

    @pytest.mark.asyncio
    async def test_raises_on_eof(self):
        reader = FakeStreamReader(b"AB")
        with pytest.raises(EOFError):
            await _read_exact(reader, 10)


class TestPiperServerTTSNoProcess:
    """Testes sem processo real — verifica comportamento com modelo ausente."""

    @pytest.mark.asyncio
    async def test_initialize_without_model_logs_warning(self, caplog):
        import logging
        tts = PiperServerTTS(model="")
        with caplog.at_level(logging.WARNING):
            await tts.initialize()
        assert any("NOISEBOT_PIPER_MODEL" in r.message for r in caplog.records)

    @pytest.mark.asyncio
    async def test_synthesize_empty_sentence_yields_nothing(self):
        tts = PiperServerTTS(model="fake.onnx")
        chunks = []
        async def _empty():
            return
            yield  # faz ser async generator

        async for chunk in tts.synthesize_stream(_empty()):
            chunks.append(chunk)
        assert chunks == []

    @pytest.mark.asyncio
    async def test_synthesize_whitespace_only_yields_nothing(self):
        tts = PiperServerTTS(model="fake.onnx")

        async def _ws():
            yield "   "

        chunks = []
        async for chunk in tts.synthesize_stream(_ws()):
            chunks.append(chunk)
        assert chunks == []

    @pytest.mark.asyncio
    async def test_shutdown_when_not_started_is_safe(self):
        tts = PiperServerTTS(model="")
        await tts.shutdown()  # não deve levantar

    @pytest.mark.asyncio
    async def test_initialize_raises_when_process_fails(self):
        tts = PiperServerTTS(executable="piper-inexistente", model="fake.onnx")
        with patch(
            "asyncio.create_subprocess_exec",
            side_effect=FileNotFoundError("piper"),
        ):
            with pytest.raises(RuntimeError, match="falha ao iniciar piper"):
                await tts.initialize()


class TestPiperServerTTSWithMockProcess:
    """Testa o caminho de síntese com processo piper mockado."""

    def _make_mock_process(self, wav_data: bytes):
        """Cria um mock de asyncio.subprocess.Process com stdout simulado."""
        mock_proc = MagicMock()
        mock_proc.returncode = None
        mock_proc.pid = 12345

        reader = FakeStreamReader(wav_data)
        mock_proc.stdout = reader
        mock_proc.stdin = AsyncMock()
        mock_proc.stdin.write = MagicMock()
        mock_proc.stdin.drain = AsyncMock()
        return mock_proc

    @pytest.mark.asyncio
    async def test_read_wav_pcm_returns_pcm_bytes(self):
        wav = _make_wav_bytes(num_samples=256)
        tts = PiperServerTTS(model="m.onnx")
        mock_proc = self._make_mock_process(wav)
        pcm = await tts._read_wav_pcm(mock_proc)
        assert len(pcm) == 256 * 2  # 256 int16 = 512 bytes

    @pytest.mark.asyncio
    async def test_read_wav_pcm_raises_on_bad_header(self):
        bad_data = b"NOTARIFFHEADER!!" + b"\x00" * 100
        tts = PiperServerTTS(model="m.onnx")
        mock_proc = self._make_mock_process(bad_data)
        with pytest.raises(ValueError, match="WAV inválido"):
            await tts._read_wav_pcm(mock_proc)

    @pytest.mark.asyncio
    async def test_chunks_are_at_most_chunk_bytes(self):
        tts = PiperServerTTS(model="m.onnx")
        raw_pcm = b"\x00\x00" * 1024

        async def _fake_run_piper_raw(text: str) -> bytes:
            assert text == "texto de teste"
            return raw_pcm

        tts._run_piper_raw = _fake_run_piper_raw

        async def _sentences():
            yield "texto de teste"

        chunks = []
        async for chunk in tts.synthesize_stream(_sentences()):
            chunks.append(chunk)
            assert len(chunk) <= CHUNK_BYTES

        total = sum(len(c) for c in chunks)
        assert total == 1024 * 2  # 1024 int16

    @pytest.mark.asyncio
    async def test_cache_hit_skips_process(self):
        wav = _make_wav_bytes(num_samples=128)
        tts = PiperServerTTS(model="m.onnx", cache_size=4)
        mock_proc = self._make_mock_process(wav)

        # Pré-popula cache
        tts._cache.put("frase cacheada", b"\xAB\xCD" * 64)

        async def _fake_ensure():
            return mock_proc
        tts._ensure_process = _fake_ensure

        async def _sentences():
            yield "frase cacheada"

        chunks = []
        async for chunk in tts.synthesize_stream(_sentences()):
            chunks.append(chunk)

        # O stdin do mock NÃO deve ter sido chamado (cache hit)
        mock_proc.stdin.write.assert_not_called()
        assert len(chunks) > 0

    @pytest.mark.asyncio
    async def test_synthesize_caches_result(self):
        tts = PiperServerTTS(model="m.onnx", cache_size=4)
        raw_pcm = b"\x00\x00" * 256

        async def _fake_run_piper_raw(text: str) -> bytes:
            assert text == "nova frase"
            return raw_pcm

        tts._run_piper_raw = _fake_run_piper_raw

        result = await tts._synthesize_sentence("nova frase")
        assert tts._cache.get("nova frase") == result
