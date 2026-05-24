"""bridgev2.tts.piper_server — Piper TTS local (Fase 6).

O Piper CLI no Windows escreve mensagens de caminho/log no stdout quando usado
sem ``--output_raw``. Para manter o transporte limpo, cada frase é sintetizada
em subprocesso curto com stdout PCM bruto. O cache LRU evita repetir sínteses
para frases comuns.
"""
from __future__ import annotations

import asyncio
import json
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
    """TTS local via Piper CLI com saída PCM bruta.

    Cada chamada a synthesize_stream() sintetiza uma frase por vez, aplica
    cache LRU e entrega chunks de 512 bytes (256 amostras int16) ao firmware.

    Thread-safety: não é thread-safe. Projetado para rodar no event loop asyncio.
    A trava _lock serializa sínteses para evitar subprocessos concorrentes.
    """

    def __init__(
        self,
        executable: str = "piper",
        model: str = "",
        cache_size: int = 64,
        disk_cache_dir: Path | None = None,
        sample_rate: int = 16000,
        target_peak: int = 8000,
    ) -> None:
        self._executable = executable
        self._model = model
        self._sample_rate = sample_rate
        self._source_sample_rate = _load_model_sample_rate(model) or sample_rate
        self._target_peak = max(0, min(32767, target_peak))
        self._cache = PhrasePcmCache(maxsize=cache_size, disk_dir=disk_cache_dir)
        self._proc: asyncio.subprocess.Process | None = None
        self._lock = asyncio.Lock()

    # -- TTSProvider interface ------------------------------------------------

    async def initialize(self) -> None:
        """Valida o Piper e o modelo. A síntese abre subprocessos por frase."""
        if not self._model:
            log.warning("PiperServerTTS: NOISEBOT_PIPER_MODEL não configurado — TTS desabilitado.")
            return
        try:
            proc = await asyncio.create_subprocess_exec(
                self._executable,
                "--help",
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
            await proc.wait()
            if not Path(self._model).exists():
                raise RuntimeError(f"modelo não encontrado: {self._model}")
        except Exception as exc:
            log.error("PiperServerTTS: falha ao iniciar piper (%s). TTS desabilitado.", exc)
            raise RuntimeError("PiperServerTTS: falha ao iniciar piper") from exc

        log.info(
            "PiperServerTTS: pronto. modelo=%s sample_rate=%d→%d",
            self._model, self._source_sample_rate, self._sample_rate,
        )

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
        """Encerra processo pendente, se existir."""
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
            return _normalize_pcm16_peak(cached, self._target_peak)

        if not self._model:
            return b""

        async with self._lock:
            try:
                pcm = await self._run_piper_raw(text)
                if self._source_sample_rate != self._sample_rate:
                    pcm = _resample_pcm16_linear(
                        pcm,
                        src_rate=self._source_sample_rate,
                        dst_rate=self._sample_rate,
                    )
                pcm = _normalize_pcm16_peak(pcm, self._target_peak)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                log.error("PiperServerTTS: erro na síntese de %r: %s", text[:40], exc)
                return b""

        self._cache.put(text, pcm)
        log.debug("TTS sintetizado: %r → %d bytes PCM", text[:40], len(pcm))
        return pcm

    async def _run_piper_raw(self, text: str) -> bytes:
        proc = await asyncio.create_subprocess_exec(
            self._executable,
            "--model", self._model,
            "--output_raw",
            "--quiet",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        self._proc = proc
        stdout, _ = await proc.communicate(text.encode("utf-8") + b"\n")
        self._proc = None
        if proc.returncode not in (0, None):
            raise RuntimeError(f"piper saiu com código {proc.returncode}")
        if not stdout:
            raise RuntimeError("piper não gerou áudio")
        return stdout

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


def _load_model_sample_rate(model: str) -> int | None:
    if not model:
        return None
    cfg = Path(f"{model}.json")
    if not cfg.exists():
        return None
    try:
        data = json.loads(cfg.read_text(encoding="utf-8"))
        rate = data.get("audio", {}).get("sample_rate")
        return int(rate) if rate else None
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return None


def _resample_pcm16_linear(pcm: bytes, *, src_rate: int, dst_rate: int) -> bytes:
    if src_rate <= 0 or dst_rate <= 0 or src_rate == dst_rate or len(pcm) < 4:
        return pcm
    samples = memoryview(pcm).cast("h")
    src_len = len(samples)
    dst_len = max(1, int(src_len * dst_rate / src_rate))
    out = bytearray(dst_len * 2)
    for i in range(dst_len):
        src_pos = i * src_rate / dst_rate
        j = int(src_pos)
        if j >= src_len - 1:
            val = samples[src_len - 1]
        else:
            frac = src_pos - j
            val = int(samples[j] * (1.0 - frac) + samples[j + 1] * frac)
        struct.pack_into("<h", out, i * 2, max(-32768, min(32767, val)))
    return bytes(out)


def _normalize_pcm16_peak(pcm: bytes, target_peak: int) -> bytes:
    if target_peak <= 0 or len(pcm) < 2:
        return pcm
    samples = memoryview(pcm).cast("h")
    peak = max((abs(int(s)) for s in samples), default=0)
    if peak <= 0 or peak == target_peak:
        return pcm
    gain = target_peak / peak
    out = bytearray(len(pcm))
    for i, sample in enumerate(samples):
        val = int(int(sample) * gain)
        struct.pack_into("<h", out, i * 2, max(-32768, min(32767, val)))
    return bytes(out)
