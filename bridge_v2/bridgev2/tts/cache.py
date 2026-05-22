"""bridgev2.tts.cache — Cache LRU de PCM por frase (RAM + disco, Fase 6)."""
from __future__ import annotations

import hashlib
import logging
from collections import OrderedDict
from pathlib import Path

log = logging.getLogger(__name__)


def _phrase_key(text: str) -> str:
    """Chave canônica: texto normalizado → SHA-1 de 16 hex chars."""
    norm = " ".join(text.lower().split())
    return hashlib.sha1(norm.encode()).hexdigest()[:16]


class PhrasePcmCache:
    """Cache LRU de PCM bruto (int16 LE, 16 kHz mono) indexado por frase.

    Args:
        maxsize:  número máximo de frases em RAM (LRU eviction).
        disk_dir: diretório para persistência em disco (opcional).
    """

    def __init__(self, maxsize: int = 64, disk_dir: Path | None = None) -> None:
        self._maxsize = maxsize
        self._ram: OrderedDict[str, bytes] = OrderedDict()
        self._disk = disk_dir
        if disk_dir:
            disk_dir.mkdir(parents=True, exist_ok=True)

    # -- API pública ----------------------------------------------------------

    def get(self, text: str) -> bytes | None:
        """Retorna PCM cacheado ou None (miss). Atualiza ordem LRU."""
        key = _phrase_key(text)
        if key in self._ram:
            self._ram.move_to_end(key)
            return self._ram[key]
        if self._disk:
            p = self._disk / f"{key}.pcm"
            if p.exists():
                try:
                    pcm = p.read_bytes()
                    self._put_ram(key, pcm)
                    return pcm
                except OSError as exc:
                    log.warning("cache: erro ao ler disco: %s", exc)
        return None

    def put(self, text: str, pcm: bytes) -> None:
        """Armazena PCM no cache (RAM + disco se configurado)."""
        key = _phrase_key(text)
        self._put_ram(key, pcm)
        if self._disk:
            try:
                (self._disk / f"{key}.pcm").write_bytes(pcm)
            except OSError as exc:
                log.warning("cache: erro ao gravar disco: %s", exc)

    @property
    def size(self) -> int:
        """Número de entradas atualmente em RAM."""
        return len(self._ram)

    def clear(self) -> None:
        """Remove todas as entradas da RAM (não afeta disco)."""
        self._ram.clear()

    # -- Internos -------------------------------------------------------------

    def _put_ram(self, key: str, pcm: bytes) -> None:
        self._ram[key] = pcm
        self._ram.move_to_end(key)
        while len(self._ram) > self._maxsize:
            self._ram.popitem(last=False)
