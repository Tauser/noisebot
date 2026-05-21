"""bridgev2.tts.sentencizer — Divide texto em frases para síntese incremental.

Separa o campo 'reply' do LLM em frases prontas para TTS.
Respeita abreviações comuns em PT-BR para evitar splits espúrios.
"""
from __future__ import annotations

import re
from typing import Iterator

# Abreviações comuns em PT-BR que terminam em '.' mas NÃO encerram frase
_ABBREV_RE = re.compile(
    r"\b(?:Dr|Dra|Sr|Sra|Prof|Profa|etc|vs|nr|Av|Fig|Ref|Obs|ex|p\.ex)\.$",
    re.IGNORECASE,
)

# Captura tudo até (e incluindo) pontuação de fim de frase + espaço/fim
_SPLIT_RE = re.compile(r"([^.!?…]*[.!?…]+(?:\s|$))", re.DOTALL)


class Sentencizer:
    """Acumula tokens de texto e emite frases completas prontas para TTS.

    Uso típico:
        sz = Sentencizer()
        for token in token_stream:
            for sentence in sz.feed(token):
                # sentence pronta para TTS
                ...
        for sentence in sz.flush():
            # texto restante (última frase sem pontuação final)
            ...
        sz.reset()  # em caso de barge-in
    """

    def __init__(self, min_chars: int = 8) -> None:
        """
        min_chars: comprimento mínimo de uma frase para ser emitida.
        Frases menores são acumuladas com o texto seguinte para evitar
        enviar fragmentos muito curtos ao TTS.
        """
        self._buf: str = ""
        self._min_chars = min_chars

    # -- Propriedades ---------------------------------------------------------

    @property
    def buffer(self) -> str:
        """Buffer interno atual (diagnóstico / teste)."""
        return self._buf

    # -- API pública ----------------------------------------------------------

    def feed(self, token: str) -> Iterator[str]:
        """Processa um token. Yields zero ou mais frases completas."""
        self._buf += token
        yield from self._extract()

    def flush(self) -> Iterator[str]:
        """Finaliza o stream. Emite qualquer texto restante como última frase."""
        tail = self._buf.strip()
        self._buf = ""
        if tail:
            yield tail

    def reset(self) -> None:
        """Descarta buffer atual (barge-in ou erro de turno)."""
        self._buf = ""

    # -- Internos -------------------------------------------------------------

    def _extract(self) -> Iterator[str]:
        """Extrai frases completas do buffer respeitando abreviações."""
        while True:
            m = _SPLIT_RE.search(self._buf)
            if not m:
                break
            candidate = m.group(1).strip()
            # Verifica abreviação — se o candidate termina em abreviação não divide
            if _ABBREV_RE.search(candidate):
                break
            if len(candidate) < self._min_chars:
                # Frase muito curta; acumula com próximos tokens
                break
            yield candidate
            self._buf = self._buf[m.end():]
