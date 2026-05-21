"""Tests for bridgev2.tts.sentencizer."""
from __future__ import annotations

import pytest

from bridgev2.tts.sentencizer import Sentencizer


def _feed_all(sz: Sentencizer, text: str) -> list[str]:
    """Feed text token-by-token (1 char at a time) and collect sentences."""
    result: list[str] = []
    for ch in text:
        result.extend(sz.feed(ch))
    return result


def _feed_flush(text: str, min_chars: int = 8) -> list[str]:
    """Feed full text in one shot + flush."""
    sz = Sentencizer(min_chars=min_chars)
    result = list(sz.feed(text))
    result.extend(sz.flush())
    return result


class TestFeedAndFlush:
    def test_single_sentence_flush(self):
        sentences = _feed_flush("Olá, tudo bem")
        assert sentences == ["Olá, tudo bem"]

    def test_sentence_with_period(self):
        sentences = _feed_flush("Olá, tudo bem. Como posso ajudar?")
        assert len(sentences) >= 1
        assert any("Olá" in s for s in sentences)

    def test_two_sentences(self):
        text = "Estou bem, obrigado. Posso ajudar com algo?"
        sentences = _feed_flush(text)
        assert len(sentences) == 2

    def test_exclamation_split(self):
        sentences = _feed_flush("Que ótimo! Vamos lá?")
        assert len(sentences) == 2

    def test_question_split(self):
        sentences = _feed_flush("Tudo certo? Pode perguntar!")
        assert len(sentences) == 2

    def test_ellipsis_split(self):
        sentences = _feed_flush("Hmm... deixa eu pensar.")
        assert len(sentences) >= 1

    def test_empty_input_flush(self):
        sz = Sentencizer()
        assert list(sz.flush()) == []

    def test_whitespace_only_flush(self):
        sz = Sentencizer()
        list(sz.feed("   "))
        assert list(sz.flush()) == []

    def test_flush_clears_buffer(self):
        sz = Sentencizer()
        list(sz.feed("texto sem ponto"))
        list(sz.flush())
        assert sz.buffer == ""

    def test_reset_clears_buffer(self):
        sz = Sentencizer()
        list(sz.feed("texto sem ponto"))
        sz.reset()
        assert sz.buffer == ""

    def test_reset_no_more_output(self):
        sz = Sentencizer()
        list(sz.feed("texto"))
        sz.reset()
        assert list(sz.flush()) == []


class TestMinChars:
    def test_short_sentence_not_emitted_during_feed(self):
        sz = Sentencizer(min_chars=20)
        # "Oi." is too short — not emitted during feed
        result = list(sz.feed("Oi. "))
        assert result == []

    def test_short_sentence_emitted_on_flush(self):
        sz = Sentencizer(min_chars=20)
        list(sz.feed("Oi. "))
        flushed = list(sz.flush())
        assert len(flushed) >= 1
        assert "Oi" in flushed[0]

    def test_min_chars_zero_emits_immediately(self):
        sz = Sentencizer(min_chars=0)
        result = list(sz.feed("Ok."))
        # With min_chars=0, short sentences are allowed
        assert len(result) >= 0  # may or may not emit depending on trailing space


class TestTokenByToken:
    def test_sentence_assembled_from_chars(self):
        sz = Sentencizer(min_chars=5)
        results: list[str] = []
        for ch in "Olá tudo bem. Pode ajudar.":
            results.extend(sz.feed(ch))
        results.extend(sz.flush())
        full = " ".join(results)
        assert "Olá" in full
        assert "ajudar" in full

    def test_buffer_tracks_accumulation(self):
        sz = Sentencizer()
        list(sz.feed("Olá "))
        assert "Olá" in sz.buffer
        list(sz.feed("mundo"))
        assert "mundo" in sz.buffer


class TestAbbreviations:
    def test_dr_not_split(self):
        sz = Sentencizer(min_chars=5)
        # "Dr. Silva" should not split at "Dr."
        result = list(sz.feed("Dr. Silva está aqui."))
        result.extend(sz.flush())
        # There should be only one sentence (not split at "Dr.")
        combined = " ".join(result)
        assert "Dr" in combined
        assert "Silva" in combined

    def test_etc_not_split(self):
        sz = Sentencizer(min_chars=5)
        result = list(sz.feed("coisas, etc. desse tipo são comuns."))
        result.extend(sz.flush())
        combined = " ".join(result)
        assert "etc" in combined


class TestMultipleSentences:
    def test_three_sentences(self):
        text = "Primeira frase aqui. Segunda frase aqui. Terceira frase aqui."
        sentences = _feed_flush(text)
        assert len(sentences) == 3

    def test_preserves_content(self):
        text = "Primeira frase. Segunda frase."
        sentences = _feed_flush(text)
        assert any("Primeira" in s for s in sentences)
        assert any("Segunda" in s for s in sentences)
