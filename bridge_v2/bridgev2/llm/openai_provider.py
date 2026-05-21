"""bridgev2.llm.openai_provider — OpenAI streaming + batch LLM provider.

A chave de API vem EXCLUSIVAMENTE de os.environ["OPENAI_API_KEY"].
Nunca persistir, nunca logar a chave.

Requer: pip install openai>=1.0
"""
from __future__ import annotations

import logging
import os
from typing import AsyncIterator

from .base import StreamingLLMProvider
from .circuit_breaker import CircuitBreaker, CircuitOpenError
from .prompt import build_messages, parse_llm_json
from ..runtime.events import LlmReplyComplete

log = logging.getLogger(__name__)

_DEFAULT_MODEL = "gpt-4o-mini"
_DEFAULT_TEMPERATURE = 0.7
_DEFAULT_MAX_TOKENS = 256


class OpenAIStreamingProvider(StreamingLLMProvider):
    """OpenAI streaming provider com circuit breaker integrado.

    Suporta streaming de tokens (generate_stream) e fallback batch
    (generate_complete). A seleção é feita pelo Orchestrator.

    Parâmetros
    ----------
    model:
        Modelo OpenAI. Default: gpt-4o-mini.
    temperature:
        Temperatura de sampling. Default: 0.7.
    max_tokens:
        Máximo de tokens de saída. Default: 256.
    failure_threshold:
        Falhas consecutivas para abrir o circuit breaker. Default: 3.
    reset_timeout:
        Segundos em OPEN antes de tentar HALF_OPEN. Default: 30.
    """

    _provider_name = "openai"

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
            provider=f"openai/{model}",
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
        """Retorna AsyncIterator de tokens brutos de texto.

        O circuit check (allow_request) acontece na primeira iteração.
        Pode lançar CircuitOpenError ou exceções do cliente openai.
        """
        return self._do_stream(text, context)

    # -- Batch fallback -------------------------------------------------------

    async def generate_complete(
        self, text: str, context: dict, turn_id: int = 0
    ) -> LlmReplyComplete:
        """Fallback batch — retorna resposta completa sem streaming."""
        self._cb.allow_request()
        client = _get_client()
        messages = build_messages(text, context)
        try:
            response = await client.chat.completions.create(
                model=self._model,
                messages=messages,
                temperature=self._temperature,
                max_tokens=self._max_tokens,
                stream=False,
            )
            raw = response.choices[0].message.content or ""
            usage = response.usage
            parsed = parse_llm_json(raw)
            self._cb.record_success()
            return LlmReplyComplete(
                turn_id=turn_id,
                reply=parsed["reply"],
                expression_id=parsed.get("expression_id"),
                action_id=parsed.get("action"),
                emot_event_id=parsed.get("emot_event"),
                input_tokens=usage.prompt_tokens if usage else 0,
                output_tokens=usage.completion_tokens if usage else 0,
                provider=self._provider_name,
                model=self._model,
            )
        except Exception:
            self._cb.record_failure()
            raise

    # -- Async generator interno ----------------------------------------------

    async def _do_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        """Async generator que emite tokens brutos do stream SSE da OpenAI."""
        self._cb.allow_request()  # CircuitOpenError se OPEN
        client = _get_client()
        messages = build_messages(text, context)
        try:
            stream = await client.chat.completions.create(
                model=self._model,
                messages=messages,
                temperature=self._temperature,
                max_tokens=self._max_tokens,
                stream=True,
            )
            async for chunk in stream:
                if chunk.choices:
                    delta = chunk.choices[0].delta
                    if delta and delta.content:
                        yield delta.content
            self._cb.record_success()
        except Exception:
            self._cb.record_failure()
            raise


def _get_client():
    """Cria AsyncOpenAI com API key do ambiente.

    Lança RuntimeError se openai não estiver instalado.
    Lança ValueError se OPENAI_API_KEY não estiver definida.
    """
    try:
        from openai import AsyncOpenAI  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError(
            "openai não instalado: pip install openai"
        ) from exc
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key:
        raise ValueError("OPENAI_API_KEY não definida no ambiente")
    return AsyncOpenAI(api_key=api_key)
