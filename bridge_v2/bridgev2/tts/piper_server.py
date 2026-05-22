"""bridgev2.tts.piper_server — Processo Piper persistente (Fase 6).

Mantém o processo piper rodando de longa duração via stdin/stdout.
Elimina o custo de spawn por turno do bridge v1.

Protocolo de framing: piper sem --output_raw escreve WAV para stdout.
O cabeçalho WAV contém o tamanho exato dos dados PCM, tornando o stream
auto-descritivo — uma sentença → um WAV completo no stdout.

Escolha de design (Fase 5.5 spike):
  Opção adotada: (a) piper CLI em processo persistente via pipe stdin/stdout.
  Protocolo: cada linha de texto no stdin → um WAV completo no stdout.
  Vantagem: sem spawn por turno, framing limpo via WAV.
"""
from __future__ import annotations

import asyncio
import logging
import struct
from pathlib import Path
from typing import AsyncIterator

from .base import TTSProvider
from .cache import PhrasePcmCache

log = logging.getLogger(__name__)

CHUNK_SAMPLES = 256          # int16 por chunk SAY do firmware
CHUNK_BYTES = CHUNK_SAMPLES * 2   # 512 bytes = 16 ms @ 16 kHz


class PiperServerTTS(TTSProvider):
    """Processo Piper persistente de longa duração.

    Cada chamada a synthesize_stream() escreve frases no stdin do piper
    e lê as respostas WAV auto-descritivas do stdout.
    O resultado é dividido em chunks de 512 bytes (256 amostras int16).

    Thread-safety: não é thread-safe. Projetado para rodar no event loop asyncio.
    A trava _lock serializa sínteses (uma por vez no processo piper).
    """

    def __init__(
        self,
        executable: str = "piper",
        model: str = "",
        cache_size: int = 64,
        disk_cache_dir: Path | None = None,
        sample_rate: int = 16000,
    ) -> None:
        self._executable = executable
        self._model = model
        self._sample_rate = sample_rate
        self._cache = PhrasePcmCache(maxsize=cache_size, disk_dir=disk_cache_dir)
        self._proc: asyncio.subprocess.Process | None = None
        self._lock = asyncio.Lock()

    # -- TTSProvider interface ------------------------------------------------

    async def initialize(self) -> None:
        """Inicia o processo piper. Pode demorar se o modelo for grande."""
        if not self._model:
            log.warning("PiperServerTTS: NOISEBOT_PIPER_MODEL não configurado — TTS desabilitado.")
            return
        try:
            await self._ensure_process()
            log.info("PiperServerTTS: processo iniciado. modelo=%s", self._model)
        except Exception as exc:
            log.error("PiperServerTTS: falha ao iniciar piper (%s). TTS desabilitado.", exc)

    async def synthesize_stream(
        self, sentences: AsyncIterator[str]
    ) -> AsyncIterator[bytes]:
        """Sintetiza frases em streaming, yielding chunks PCM de 512 bytes.

        Cache-first: frases já sintetizadas não chamam o piper.
        Cada yield é um chunk de CHUNK_BYTES bytes (int16 LE, 16 kHz mono).
        """
        async for sentence in sentences:
            sentence = sentence.strip()
            if not sentence:
                continue
            pcm = await self._synthesize_sentence(sentence)
            for i in range(0, len(pcm), CHUNK_BYTES):
                chunk = pcm[i:i + CHUNK_BYTES]
                if chunk:
                    yield chunk

    async def shutdown(self) -> None:
        """Encerra o processo piper graciosamente."""
        proc = self._proc
        self._proc = None
        if proc is None:
            return
        try:
            if proc.stdin:
                proc.stdin.close()
                await proc.stdin.wait_closed()
            await asyncio.wait_for(proc.wait(), timeout=3.0)
        except (asyncio.TimeoutError, Exception):
            try:
                proc.kill()
            except Exception:
                pass
        log.info("PiperServerTTS: encerrado.")

    # -- Internos -------------------------------------------------------------

    async def _ensure_process(self) -> asyncio.subprocess.Process:
        """Garante que o processo piper está rodando; reinicia se morreu."""
        if self._proc is not None and self._proc.returncode is None:
            return self._proc
        if not self._model:
            raise RuntimeError("PiperServerTTS: modelo não configurado.")
        self._proc = await asyncio.create_subprocess_exec(
            self._executable,
            "--model", self._model,
            "--quiet",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        log.debug("PiperServerTTS: subprocess pid=%d iniciado", self._proc.pid)
        return self._proc

    async def _synthesize_sentence(self, text: str) -> bytes:
        """Sintetiza uma frase. Cache-first. Retorna PCM bruto (int16 LE)."""
        cached = self._cache.get(text)
        if cached is not None:
            log.debug("TTS cache hit: %r (%d bytes)", text[:40], len(cached))
            return cached

        if not self._model:
            return b""

        async with self._lock:
            try:
                proc = await self._ensure_process()
                assert proc.stdin is not None
                proc.stdin.write(text.encode("utf-8") + b"\n")
                await proc.stdin.drain()
                pcm = await self._read_wav_pcm(proc)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                log.error("PiperServerTTS: erro na síntese de %r: %s", text[:40], exc)
                self._proc = None   # força restart na próxima chamada
                return b""

        self._cache.put(text, pcm)
        log.debug("TTS sintetizado: %r → %d bytes PCM", text[:40], len(pcm))
        return pcm

    async def _read_wav_pcm(self, proc: asyncio.subprocess.Process) -> bytes:
        """Lê um frame WAV completo do stdout do piper. Retorna apenas PCM."""
        assert proc.stdout is not None
        stdout = proc.stdout

        # Header RIFF: 'RIFF'(4) + size(4 LE) + 'WAVE'(4)
        riff_hdr = await _read_exact(stdout, 12)
        if riff_hdr[:4] != b"RIFF" or riff_hdr[8:12] != b"WAVE":
            raise ValueError(f"WAV inválido: header={riff_hdr[:12]!r}")

        # Percorre sub-chunks até encontrar 'data'
        while True:
            chunk_hdr = await _read_exact(stdout, 8)
            chunk_id = chunk_hdr[:4]
            chunk_size = struct.unpack_from("<I", chunk_hdr, 4)[0]
            if chunk_id == b"data":
                return await _read_exact(stdout, chunk_size)
            # Chunk desconhecido: pula (WAV spec: chunks têm tamanho par)
            skip = chunk_size + (chunk_size & 1)
            if skip:
                await _read_exact(stdout, skip)


async def _read_exact(stream: asyncio.StreamReader, n: int) -> bytes:
    """Lê exatamente n bytes de um StreamReader asyncio."""
    data = b""
    while len(data) < n:
        chunk = await stream.read(n - len(data))
        if not chunk:
            raise EOFError(
                f"stdout do piper fechou inesperadamente "
                f"(lido {len(data)}/{n} bytes)"
            )
        data += chunk
    return data
