"""bridgev2.llm.gemini_provider — Google Gemini LLM provider.

A chave de API vem EXCLUSIVAMENTE de os.environ["GEMINI_API_KEY"].
Nunca persistir, nunca logar a chave.

Requer: pip install google-generativeai>=0.7
"""
from __future__ import annotations

import asyncio
import logging
import os
from typing import AsyncIterator

from .base import StreamingLLMProvider
from .circuit_breaker import CircuitBreaker
from .prompt import build_messages

log = logging.getLogger(__name__)

_DEFAULT_MODEL = "gemini-1.5-flash"
_DEFAULT_TEMPERATURE = 0.7
_DEFAULT_MAX_TOKENS = 256


class GeminiProvider(StreamingLLMProvider):
    """Google Gemini provider com circuit breaker integrado.

    Implementa StreamingLLMProvider por compatibilidade com o Orchestrator:
    internamente chama a API Gemini de forma batch (síncrona em executor)
    e emite a resposta completa como um único "token".

    Parâmetros
    ----------
    model:
        Modelo Gemini. Default: gemini-1.5-flash.
    temperature:
        Temperatura de sampling. Default: 0.7.
    max_tokens:
        Máximo de tokens de saída. Default: 256.
    failure_threshold / reset_timeout:
        Parâmetros do circuit breaker. Defaults: 3 / 30s.
    """

    _provider_name = "gemini"

    def __init__(
        self,
        model: str = _DEFAULT_MODEL,
        temperature: float = _DEFAULT_TEMPERATURE,
        max_tokens: int = _DEFAULT_MAX_TOKENS,
        failure_threshold: int = 3,
        reset_timeout: float = 30.0,
    ) -> None:
        self._model = model
        self._temperature = temperature
        self._max_tokens = max_tokens
        self._cb = CircuitBreaker(
            provider=f"gemini/{model}",
            failure_threshold=failure_threshold,
            reset_timeout=reset_timeout,
        )

    @property
    def circuit_breaker(self) -> CircuitBreaker:
        return self._cb

    # -- StreamingLLMProvider -------------------------------------------------

    def generate_stream(
        self, text: str, context: dict
    ) -> AsyncIterator[str]:
        """Retorna AsyncIterator. Gemini roda em executor e emite um único token."""
        return self._do_stream(text, context)

    # -- Async generator interno ----------------------------------------------

    async def _do_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        """Async generator: chama Gemini (batch em executor), emite resposta."""
        self._cb.allow_request()  # CircuitOpenError se OPEN
        messages = build_messages(text, context)
        try:
            raw = await _call_gemini(
                self._model, messages, self._temperature, self._max_tokens
            )
            self._cb.record_success()
            # Emite o JSON completo como um único "token" — o Orchestrator
            # acumula os tokens antes de parsear, então isso funciona igual
            # ao streaming token a token.
            yield raw
        except Exception:
            self._cb.record_failure()
            raise


async def _call_gemini(
    model: str,
    messages: list[dict],
    temperature: float,
    max_tokens: int,
) -> str:
    """Chama a API Gemini (síncrona) em executor e retorna o texto bruto."""
    try:
        import google.generativeai as genai  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError(
            "google-generativeai não instalado: "
            "pip install google-generativeai"
        ) from exc

    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        raise ValueError("GEMINI_API_KEY não definida no ambiente")

    def _sync_call() -> str:
        genai.configure(api_key=api_key)
        gm = genai.GenerativeModel(
            model_name=model,
            generation_config=genai.types.GenerationConfig(
                temperature=temperature,
                max_output_tokens=max_tokens,
            ),
        )
        # Transforma messages (formato OpenAI) em prompt único para Gemini
        system_parts = [m["content"] for m in messages if m["role"] == "system"]
        user_parts = [m["content"] for m in messages if m["role"] == "user"]
        prompt = "\n\n".join(system_parts + user_parts)
        response = gm.generate_content(prompt)
        return response.text

    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(None, _sync_call)
