"""Tests for bridgev2.tts.cache — PhrasePcmCache."""
from __future__ import annotations

import tempfile
from pathlib import Path

import pytest

from bridgev2.tts.cache import PhrasePcmCache, _phrase_key


class TestPhraseKey:
    def test_normalization_lower(self):
        assert _phrase_key("Olá") == _phrase_key("olá")

    def test_normalization_whitespace(self):
        assert _phrase_key("  oi  tudo  ") == _phrase_key("oi tudo")

    def test_different_texts_differ(self):
        assert _phrase_key("abc") != _phrase_key("xyz")

    def test_key_length(self):
        assert len(_phrase_key("qualquer texto")) == 16


class TestRamCache:
    def test_miss_returns_none(self):
        cache = PhrasePcmCache(maxsize=4)
        assert cache.get("frase desconhecida") is None

    def test_put_then_get(self):
        cache = PhrasePcmCache(maxsize=4)
        pcm = b"\x00\x01" * 100
        cache.put("olá mundo", pcm)
        assert cache.get("olá mundo") == pcm

    def test_case_insensitive_hit(self):
        cache = PhrasePcmCache(maxsize=4)
        pcm = b"\xAB\xCD" * 50
        cache.put("Olá Mundo", pcm)
        assert cache.get("olá mundo") == pcm

    def test_size_increments(self):
        cache = PhrasePcmCache(maxsize=4)
        assert cache.size == 0
        cache.put("frase um", b"\x00" * 10)
        assert cache.size == 1
        cache.put("frase dois", b"\x01" * 10)
        assert cache.size == 2

    def test_lru_eviction(self):
        cache = PhrasePcmCache(maxsize=2)
        cache.put("A", b"\x01")
        cache.put("B", b"\x02")
        cache.put("C", b"\x03")  # deve ejetar "A"
        assert cache.size == 2
        assert cache.get("A") is None
        assert cache.get("B") == b"\x02"
        assert cache.get("C") == b"\x03"

    def test_get_refreshes_lru(self):
        cache = PhrasePcmCache(maxsize=2)
        cache.put("A", b"\x01")
        cache.put("B", b"\x02")
        cache.get("A")           # "A" vira o mais recente
        cache.put("C", b"\x03")  # deve ejetar "B"
        assert cache.get("A") == b"\x01"
        assert cache.get("B") is None

    def test_clear_empties_ram(self):
        cache = PhrasePcmCache(maxsize=4)
        cache.put("frase", b"\x00")
        cache.clear()
        assert cache.size == 0
        assert cache.get("frase") is None


class TestDiskCache:
    def test_disk_persists_across_instances(self, tmp_path: Path):
        pcm = b"\xFF\xFE" * 128
        cache1 = PhrasePcmCache(maxsize=2, disk_dir=tmp_path)
        cache1.put("frase persistida", pcm)

        # Nova instância com mesmo diretório
        cache2 = PhrasePcmCache(maxsize=2, disk_dir=tmp_path)
        result = cache2.get("frase persistida")
        assert result == pcm

    def test_disk_promotes_to_ram(self, tmp_path: Path):
        pcm = b"\xAA" * 64
        cache = PhrasePcmCache(maxsize=4, disk_dir=tmp_path)
        cache.put("texto", pcm)

        # Limpa RAM mas mantém disco
        cache.clear()
        assert cache.size == 0

        # get() deve ler do disco e promover para RAM
        result = cache.get("texto")
        assert result == pcm
        assert cache.size == 1  # promovido para RAM

    def test_no_disk_stays_ram_only(self):
        cache = PhrasePcmCache(maxsize=2)
        cache.put("frase", b"\x00" * 8)
        cache.clear()
        assert cache.get("frase") is None  # sem disco, perdeu
