"""Text-to-speech providers for the NoiseBot server."""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import re
import struct
from abc import ABC, abstractmethod
from collections import OrderedDict
from collections.abc import AsyncIterator, Iterator
from pathlib import Path

log = logging.getLogger(__name__)

CHUNK_SAMPLES = 256
CHUNK_BYTES = CHUNK_SAMPLES * 2

_ABBREV_RE = re.compile(
    r"\b(?:Dr|Dra|Sr|Sra|Prof|Profa|etc|vs|nr|Av|Fig|Ref|Obs|ex|p\.ex)\.$",
    re.IGNORECASE,
)
_SPLIT_RE = re.compile(r"([^.!?…]*[.!?…]+(?:\s|$))", re.DOTALL)


class TTSProvider(ABC):
    @abstractmethod
    async def initialize(self) -> None:
        ...

    @abstractmethod
    async def synthesize_stream(self, sentences: AsyncIterator[str]) -> AsyncIterator[bytes]:
        ...

    @abstractmethod
    async def shutdown(self) -> None:
        ...


class Sentencizer:
    """Accumulate text tokens and emit complete sentences."""

    def __init__(self, min_chars: int = 8) -> None:
        self._buf = ""
        self._min_chars = min_chars

    @property
    def buffer(self) -> str:
        return self._buf

    def feed(self, token: str) -> Iterator[str]:
        self._buf += token
        yield from self._extract()

    def flush(self) -> Iterator[str]:
        tail = self._buf.strip()
        self._buf = ""
        if tail:
            yield tail

    def reset(self) -> None:
        self._buf = ""

    def _extract(self) -> Iterator[str]:
        while True:
            match = _SPLIT_RE.search(self._buf)
            if not match:
                break
            candidate = match.group(1).strip()
            if _ABBREV_RE.search(candidate):
                break
            if len(candidate) < self._min_chars:
                break
            yield candidate
            self._buf = self._buf[match.end() :]


def _phrase_key(text: str) -> str:
    norm = " ".join(text.lower().split())
    return hashlib.sha1(norm.encode()).hexdigest()[:16]


class PhrasePcmCache:
    """LRU PCM cache for synthesized phrases."""

    def __init__(
        self,
        maxsize: int = 64,
        disk_dir: Path | None = None,
        namespace: str = "",
    ) -> None:
        self._maxsize = maxsize
        self._ram: OrderedDict[str, bytes] = OrderedDict()
        self._disk = disk_dir
        self._namespace = namespace
        if disk_dir:
            disk_dir.mkdir(parents=True, exist_ok=True)

    def get(self, text: str) -> bytes | None:
        key = _phrase_key(f"{self._namespace}\n{text}")
        if key in self._ram:
            self._ram.move_to_end(key)
            return self._ram[key]
        if self._disk:
            path = self._disk / f"{key}.pcm"
            if path.exists():
                try:
                    pcm = path.read_bytes()
                    self._put_ram(key, pcm)
                    return pcm
                except OSError as exc:
                    log.warning("cache: erro ao ler disco: %s", exc)
        return None

    def put(self, text: str, pcm: bytes) -> None:
        key = _phrase_key(f"{self._namespace}\n{text}")
        self._put_ram(key, pcm)
        if self._disk:
            try:
                (self._disk / f"{key}.pcm").write_bytes(pcm)
            except OSError as exc:
                log.warning("cache: erro ao gravar disco: %s", exc)

    @property
    def size(self) -> int:
        return len(self._ram)

    def clear(self) -> None:
        self._ram.clear()

    def _put_ram(self, key: str, pcm: bytes) -> None:
        self._ram[key] = pcm
        self._ram.move_to_end(key)
        while len(self._ram) > self._maxsize:
            self._ram.popitem(last=False)


class PiperServerTTS(TTSProvider):
    """Local Piper CLI TTS with raw PCM output and phrase cache."""

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
        cache_namespace = (
            f"{Path(model).name}|{self._source_sample_rate}|"
            f"{self._sample_rate}|{self._target_peak}"
        )
        self._cache = PhrasePcmCache(
            maxsize=cache_size,
            disk_dir=disk_cache_dir,
            namespace=cache_namespace,
        )
        self._proc: asyncio.subprocess.Process | None = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        if not self._model:
            log.warning("PiperServerTTS: NOISEBOT_PIPER_MODEL nao configurado.")
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
                raise RuntimeError(f"modelo nao encontrado: {self._model}")
        except Exception as exc:
            log.error("PiperServerTTS: falha ao iniciar piper (%s).", exc)
            raise RuntimeError("PiperServerTTS: falha ao iniciar piper") from exc

        log.info(
            "PiperServerTTS: pronto. modelo=%s sample_rate=%d->%d",
            self._model,
            self._source_sample_rate,
            self._sample_rate,
        )

    async def synthesize_stream(self, sentences: AsyncIterator[str]) -> AsyncIterator[bytes]:
        async for sentence in sentences:
            sentence = sentence.strip()
            if not sentence:
                continue
            pcm = await self._synthesize_sentence(sentence)
            for i in range(0, len(pcm), CHUNK_BYTES):
                chunk = pcm[i : i + CHUNK_BYTES]
                if chunk:
                    yield chunk

    async def shutdown(self) -> None:
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

    async def _synthesize_sentence(self, text: str) -> bytes:
        cached = self._cache.get(text)
        if cached is not None:
            log.debug("TTS cache hit: %r (%d bytes)", text[:40], len(cached))
            return cached

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
                log.error("PiperServerTTS: erro na sintese de %r: %s", text[:40], exc)
                return b""

        self._cache.put(text, pcm)
        log.debug("TTS sintetizado: %r -> %d bytes PCM", text[:40], len(pcm))
        return pcm

    async def _run_piper_raw(self, text: str) -> bytes:
        proc = await asyncio.create_subprocess_exec(
            self._executable,
            "--model",
            self._model,
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
            raise RuntimeError(f"piper saiu com codigo {proc.returncode}")
        if not stdout:
            raise RuntimeError("piper nao gerou audio")
        return stdout


def _load_model_sample_rate(model: str) -> int | None:
    if not model:
        return None
    config = Path(f"{model}.json")
    if not config.exists():
        return None
    try:
        data = json.loads(config.read_text(encoding="utf-8"))
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
    peak = max((abs(int(sample)) for sample in samples), default=0)
    if peak <= 0 or peak == target_peak:
        return pcm
    gain = target_peak / peak
    out = bytearray(len(pcm))
    for i, sample in enumerate(samples):
        val = int(int(sample) * gain)
        struct.pack_into("<h", out, i * 2, max(-32768, min(32767, val)))
    return bytes(out)


__all__ = [
    "CHUNK_BYTES",
    "CHUNK_SAMPLES",
    "PhrasePcmCache",
    "PiperServerTTS",
    "Sentencizer",
    "TTSProvider",
]
