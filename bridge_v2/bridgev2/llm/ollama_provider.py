"""bridgev2.llm.ollama_provider -- Ollama local LLM provider.

Usa a API local do Ollama em /api/chat. Nao requer API key.
"""
from __future__ import annotations

import json
import logging
from typing import AsyncIterator

from .base import StreamingLLMProvider
from .circuit_breaker import CircuitBreaker
from .prompt import build_messages

log = logging.getLogger(__name__)

_DEFAULT_MODEL = "gemma4:12b"
_DEFAULT_BASE_URL = "http://127.0.0.1:11434"
_DEFAULT_TEMPERATURE = 0.7
_DEFAULT_MAX_TOKENS = 256


class OllamaProvider(StreamingLLMProvider):
    """Provider local via Ollama.

    Implementa streaming real lendo NDJSON da API /api/chat. O Orchestrator
    acumula os tokens e parseia o JSON final igual aos providers remotos.
    """

    _provider_name = "ollama"

    def __init__(
        self,
        model: str = _DEFAULT_MODEL,
        base_url: str = _DEFAULT_BASE_URL,
        temperature: float = _DEFAULT_TEMPERATURE,
        max_tokens: int = _DEFAULT_MAX_TOKENS,
        think: bool = False,
        failure_threshold: int = 3,
        reset_timeout: float = 30.0,
    ) -> None:
        self._model = model
        self._base_url = base_url.rstrip("/")
        self._temperature = temperature
        self._max_tokens = max_tokens
        self._think = think
        self._cb = CircuitBreaker(
            provider=f"ollama/{model}",
            failure_threshold=failure_threshold,
            reset_timeout=reset_timeout,
        )

    @property
    def circuit_breaker(self) -> CircuitBreaker:
        return self._cb

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._do_stream(text, context)

    async def _do_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        self._cb.allow_request()
        try:
            import aiohttp
        except ImportError as exc:
            raise RuntimeError("aiohttp nao instalado: pip install aiohttp") from exc

        payload = {
            "model": self._model,
            "messages": build_messages(text, context),
            "stream": True,
            "think": self._think,
            "options": {
                "temperature": self._temperature,
                "num_predict": self._max_tokens,
            },
        }

        try:
            timeout = aiohttp.ClientTimeout(total=None, sock_connect=5, sock_read=60)
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.post(f"{self._base_url}/api/chat", json=payload) as resp:
                    if resp.status >= 400:
                        body = await resp.text()
                        raise RuntimeError(f"Ollama HTTP {resp.status}: {body[:240]}")

                    async for raw_line in resp.content:
                        line = raw_line.strip()
                        if not line:
                            continue
                        data = json.loads(line)
                        msg = data.get("message") or {}
                        token = msg.get("content") or ""
                        if token:
                            yield token
                        if data.get("done"):
                            break

            self._cb.record_success()
        except Exception:
            self._cb.record_failure()
            raise
