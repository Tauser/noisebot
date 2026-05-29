"""LLM providers for the NoiseBot server."""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
from abc import ABC, abstractmethod
from collections.abc import AsyncIterator
from typing import Any

from .circuit_breaker import CircuitBreaker
from .runtime import LlmReplyComplete

log = logging.getLogger(__name__)

_DEFAULT_TEMPERATURE = 0.7
_DEFAULT_MAX_TOKENS = 256

_SYSTEM_PROMPT = (
    "Voce e NoiseBot, um companion robot expressivo de mesa.\n"
    "Personalidade: caloroso, curioso, expressivo, respostas < 10s de fala.\n"
    "\n"
    "Responda SEMPRE em JSON valido, sem markdown, neste formato exato:\n"
    '{"reply":"<texto falado>","expression_id":<int>,"action":<int>,"emot_event":<int>}\n'
    "\n"
    "expression_id: 0=neutro 1=feliz 2=curioso 3=sonolento 4=focado "
    "5=desconfiado 6=surpreso 7=triste 8=alarmado 9=bravo\n"
    "action: 0=greet 1=nod 2=shake 3=look_up 4=look_down\n"
    "emot_event: 2=voice_start 3=audio_started\n"
    "\n"
    '"reply" deve ser natural, conciso, maximo 2-3 frases curtas.\n'
    '"reply" deve ser SEMPRE em portugues do Brasil. Nao use chines, japones, '
    "coreano, ingles, espanhol ou mistura de idiomas.\n"
    "Se o usuario pedir piada, varie tema, estrutura e punchline; nunca repita "
    "uma piada ou resposta recente."
)

_FOREIGN_SCRIPT_RE = re.compile(
    r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uac00-\ud7af]"
)
_ENGLISH_LEAK_RE = re.compile(
    r"\b("
    r"did you know|penguins?|they(?:'| a)?re|can't|cannot|amazing|swimmers?|"
    r"actually|because|instead|water|doctor|book|story|once upon|"
    r"the|that|with|about|what|why|how|when|where"
    r")\b",
    re.IGNORECASE,
)
_PT_LANGUAGE_FALLBACK = (
    "Desculpa, minha resposta saiu no idioma errado. Vou responder em portugues."
)
_PT_JOKE_FALLBACK = (
    "Claro. Por que o robo nao gosta de elevador? "
    "Porque ele prefere subir de versao."
)


class LLMProvider(ABC):
    @abstractmethod
    async def generate(self, text: str, context: dict) -> LlmReplyComplete:
        ...


class StreamingLLMProvider(ABC):
    @abstractmethod
    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        ...


def build_messages(
    text: str,
    context: dict[str, Any] | None = None,
    config: dict[str, Any] | None = None,
) -> list[dict[str, str]]:
    cfg = config or {}
    ctx = context or {}
    system_content: str = cfg.get("system_prompt", _SYSTEM_PROMPT)

    extra: list[str] = []
    if ctx.get("robot_state"):
        extra.append(f"Estado do robo: {ctx['robot_state']}")
    if ctx.get("emotion_state"):
        extra.append(f"Estado emocional: {ctx['emotion_state']}")
    recent_replies = ctx.get("recent_replies") or []
    if recent_replies:
        joined = "\n".join(f"- {str(reply)[:180]}" for reply in recent_replies[-5:])
        extra.append(
            "Respostas recentes a evitar repetir literalmente ou em variação próxima:\n"
            f"{joined}"
        )

    if extra:
        system_content = system_content.rstrip() + "\n\n" + "\n".join(extra)

    return [
        {"role": "system", "content": system_content},
        {"role": "user", "content": text},
    ]


def parse_llm_json(raw: str) -> dict[str, Any]:
    cleaned = raw.strip()
    md = re.search(r"```(?:json)?\s*([\s\S]*?)```", cleaned)
    if md:
        cleaned = md.group(1).strip()

    try:
        data = json.loads(cleaned)
    except json.JSONDecodeError:
        obj = re.search(r"\{[\s\S]*\}", cleaned)
        if not obj:
            raise ValueError(f"Sem JSON valido em: {raw!r}") from None
        data = json.loads(obj.group(0))

    return {
        "reply": str(data.get("reply", "")),
        "expression_id": _int_or_none(data.get("expression_id")),
        "action": _int_or_none(data.get("action")),
        "emot_event": _int_or_none(data.get("emot_event") or data.get("emot_event_id")),
    }


def recover_llm_reply_text(raw: str) -> str:
    cleaned = raw.strip()
    md = re.search(r"```(?:json)?\s*([\s\S]*?)(?:```|$)", cleaned)
    if md:
        cleaned = md.group(1).strip()

    reply_match = re.search(r'"reply"\s*:\s*"((?:\\.|[^"\\])*)', cleaned)
    if reply_match:
        return _decode_json_string_fragment(reply_match.group(1)).strip()

    if cleaned.startswith("{") or cleaned.startswith("["):
        return "Nao consegui completar minha resposta."
    return cleaned


def enforce_pt_br_reply(reply: str, user_text: str = "") -> tuple[str, bool]:
    """Return a safe pt-BR reply when the model leaks unsupported languages."""
    cleaned = " ".join(reply.split()).strip()
    if not cleaned:
        return cleaned, False
    if not _FOREIGN_SCRIPT_RE.search(cleaned) and not _looks_like_english_leak(cleaned):
        return cleaned, False
    user_lower = user_text.casefold()
    fallback = _PT_JOKE_FALLBACK if "piada" in user_lower else _PT_LANGUAGE_FALLBACK
    return fallback, True


def _looks_like_english_leak(text: str) -> bool:
    if not re.search(r"[a-zA-Z]", text):
        return False
    markers = _ENGLISH_LEAK_RE.findall(text)
    if len(markers) >= 2:
        return True
    lower = text.casefold()
    return "did you know" in lower or "can't" in lower or "cannot" in lower


def _int_or_none(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _decode_json_string_fragment(value: str) -> str:
    try:
        return json.loads(f'"{value}"')
    except json.JSONDecodeError:
        return (
            value.replace(r"\"", '"')
            .replace(r"\\", "\\")
            .replace(r"\n", "\n")
            .replace(r"\t", "\t")
        )


class OllamaProvider(StreamingLLMProvider):
    _provider_name = "ollama"

    def __init__(
        self,
        model: str = "qwen2.5:7b",
        base_url: str = "http://127.0.0.1:11434",
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


class OpenAIStreamingProvider(StreamingLLMProvider):
    _provider_name = "openai"

    def __init__(
        self,
        model: str = "gpt-4o-mini",
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

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._do_stream(text, context)

    async def generate_complete(
        self,
        text: str,
        context: dict,
        turn_id: int = 0,
    ) -> LlmReplyComplete:
        self._cb.allow_request()
        client = _get_openai_client()
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

    async def _do_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        self._cb.allow_request()
        client = _get_openai_client()
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


class GeminiProvider(StreamingLLMProvider):
    _provider_name = "gemini"

    def __init__(
        self,
        model: str = "gemini-2.5-flash",
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

    def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        return self._do_stream(text, context)

    async def _do_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        self._cb.allow_request()
        messages = build_messages(text, context)
        try:
            raw = await _call_gemini(
                self._model,
                messages,
                self._temperature,
                self._max_tokens,
            )
            self._cb.record_success()
            yield raw
        except Exception:
            self._cb.record_failure()
            raise


async def _call_gemini(
    model: str,
    messages: list[dict[str, str]],
    temperature: float,
    max_tokens: int,
) -> str:
    try:
        import google.generativeai as genai  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError(
            "google-generativeai nao instalado: pip install google-generativeai"
        ) from exc

    api_key = os.environ.get("GEMINI_API_KEY", "")
    if not api_key:
        raise ValueError("GEMINI_API_KEY nao definida no ambiente")

    def _sync_call() -> str:
        genai.configure(api_key=api_key)
        gm = genai.GenerativeModel(
            model_name=model,
            generation_config=genai.types.GenerationConfig(
                temperature=temperature,
                max_output_tokens=max_tokens,
            ),
        )
        system_parts = [msg["content"] for msg in messages if msg["role"] == "system"]
        user_parts = [msg["content"] for msg in messages if msg["role"] == "user"]
        prompt = "\n\n".join(system_parts + user_parts)
        response = gm.generate_content(prompt)
        return response.text

    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(None, _sync_call)


def _get_openai_client():
    try:
        from openai import AsyncOpenAI  # type: ignore[import]
    except ImportError as exc:
        raise RuntimeError("openai nao instalado: pip install openai") from exc
    api_key = os.environ.get("OPENAI_API_KEY", "")
    if not api_key:
        raise ValueError("OPENAI_API_KEY nao definida no ambiente")
    return AsyncOpenAI(api_key=api_key)


__all__ = [
    "GeminiProvider",
    "LLMProvider",
    "OllamaProvider",
    "OpenAIStreamingProvider",
    "StreamingLLMProvider",
    "build_messages",
    "enforce_pt_br_reply",
    "parse_llm_json",
    "recover_llm_reply_text",
]
