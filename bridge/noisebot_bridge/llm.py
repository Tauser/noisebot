from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
import re
import time

from .config import OPENAI_MAX_OUTPUT_TOKENS, OPENAI_MAX_REPLY_CHARS, OPENAI_TIMEOUT_S

log = logging.getLogger("noisebot_bridge.llm")


def _short_error_detail(error: Exception) -> str:
    response = getattr(error, "response", None)
    if response is not None:
        try:
            payload = response.json()
            detail = payload.get("error", payload)
            if isinstance(detail, dict):
                parts = [
                    str(detail.get("type") or ""),
                    str(detail.get("code") or ""),
                    str(detail.get("message") or ""),
                ]
                return " ".join(part for part in parts if part)[:300]
            return str(detail)[:300]
        except Exception:
            text = getattr(response, "text", "")
            if text:
                return text[:300]
    return str(error)[:300]


@dataclass
class LlmResult:
    reply: str = ""
    expression_id: int = 0
    action: int = 0
    emot_event: int = 2
    provider: str = "none"
    model: str = "none"
    elapsed_ms: float = 0.0
    error: str | None = None


class LlmProvider:
    provider = "none"
    model_name = "none"

    @property
    def ready(self) -> bool:
        return False

    def init(self):
        return None

    def generate(self, text: str, status: dict) -> LlmResult:
        return LlmResult(provider=self.provider, model=self.model_name, error="llm_indisponivel")


class NoneLlmProvider(LlmProvider):
    provider = "none"
    model_name = "none"


class MockLlmProvider(LlmProvider):
    provider = "mock"
    model_name = "mock"

    @property
    def ready(self) -> bool:
        return True

    def generate(self, text: str, status: dict) -> LlmResult:
        return LlmResult(
            reply=f"Entendi: {text}",
            expression_id=2,
            action=0,
            emot_event=2,
            provider=self.provider,
            model=self.model_name,
        )


class GeminiProvider(LlmProvider):
    provider = "gemini"

    def __init__(self, model_name: str | None = None):
        self.model_name = model_name or os.environ.get("NOISEBOT_GEMINI_MODEL", "gemini-2.0-flash-lite")
        self.client = None

    @property
    def ready(self) -> bool:
        return self.client is not None

    def init(self):
        import google.genai as genai

        api_key = os.environ.get("GEMINI_API_KEY", "")
        if not api_key:
            log.warning("GEMINI_API_KEY não definida — LLM Gemini indisponível")
            return
        self.client = genai.Client(api_key=api_key)
        log.info("Gemini Flash pronto")

    def generate(self, text: str, status: dict) -> LlmResult:
        if self.client is None:
            return LlmResult(provider=self.provider, model=self.model_name, error="gemini_nao_pronto")

        state_desc = f"estado={status.get('state', 0)} valence={status.get('valence', 0):.2f}"
        prompt = (
            "Você é NoiseBot, um companion robot expressivo de mesa. "
            "Personalidade: caloroso, curioso, expressivo, respostas < 10s de fala. "
            f"Estado atual: {state_desc}. "
            f"Usuário disse: \"{text}\"\n"
            "Responda com JSON: {\"reply\":\"...\",\"expression_id\":0,\"action\":0,\"emot_event\":2}\n"
            "expression_id: 0=neutro,1=feliz,2=curioso,3=sonolento,4=focado,5=desconfiado,"
            "6=surpreso,7=triste,8=alarmado,"
            "9=bravo (use APENAS para: pedido explícito do usuário, rejeição de safety, "
            "provocação leve como insulto ou comparação, ou erros repetidos; sempre transitório, "
            "resposta teatral e não hostil, sem ameaças, sem palavrões)\n"
            "action: 0=greet,1=nod,2=shake,3=look_up,4=look_down\n"
            "emot_event: 2=voice_start,3=audio_started (use 2 ao começar resposta)\n"
            "Responda SOMENTE com o JSON, sem markdown."
        )
        t0 = time.perf_counter()
        try:
            response = self.client.models.generate_content(model=self.model_name, contents=prompt)
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            text_resp = (response.text or "").strip()
            m = re.search(r"\{.*\}", text_resp, re.DOTALL)
            if m:
                parsed = json.loads(m.group())
                return LlmResult(
                    reply=parsed.get("reply", ""),
                    expression_id=int(parsed.get("expression_id", 0)),
                    action=int(parsed.get("action", 0)),
                    emot_event=int(parsed.get("emot_event", 2)),
                    provider=self.provider,
                    model=self.model_name,
                    elapsed_ms=elapsed_ms,
                )
            return LlmResult(reply=text_resp, provider=self.provider, model=self.model_name, elapsed_ms=elapsed_ms)
        except Exception as e:
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            if "429" in str(e):
                log.warning("Gemini 429 — cota excedida, fale novamente em alguns segundos")
                return LlmResult(provider=self.provider, model=self.model_name, elapsed_ms=elapsed_ms, error="gemini_429")
            log.warning("Gemini falhou: %s", e)
            return LlmResult(provider=self.provider, model=self.model_name, elapsed_ms=elapsed_ms, error="gemini_exception")


class OpenAIProvider(LlmProvider):
    provider = "openai"

    def __init__(self, model_name: str | None = None):
        self.model_name = model_name or os.environ.get("NOISEBOT_OPENAI_MODEL", "gpt-5.4-mini")
        self.client = None
        self.api_key = ""
        self._use_http = False

    @property
    def ready(self) -> bool:
        return self.client is not None

    def init(self):
        self.api_key = os.environ.get("OPENAI_API_KEY", "")
        if not self.api_key:
            log.warning("OPENAI_API_KEY não definida — LLM OpenAI indisponível")
            return

        try:
            from openai import OpenAI
        except ImportError:
            try:
                import httpx  # noqa: F401
            except ImportError:
                log.warning("Pacotes 'openai' e 'httpx' não instalados — rode: python -m pip install openai")
                return
            self.client = "httpx"
            self._use_http = True
            log.info("OpenAI GPT pronto via HTTP direto: %s", self.model_name)
            return

        self.client = OpenAI(api_key=self.api_key, timeout=OPENAI_TIMEOUT_S)
        log.info("OpenAI GPT pronto: %s", self.model_name)

    def _extract_http_output_text(self, data: dict) -> str:
        texts: list[str] = []
        for item in data.get("output", []):
            for content in item.get("content", []):
                if content.get("type") == "output_text":
                    texts.append(content.get("text", ""))
        return "".join(texts).strip()

    def generate(self, text: str, status: dict) -> LlmResult:
        if self.client is None:
            return LlmResult(provider=self.provider, model=self.model_name, error="openai_nao_pronto")

        state_desc = f"estado={status.get('state', 0)} valence={status.get('valence', 0):.2f}"
        instructions = (
            "Você é NoiseBot, um companion robot expressivo de mesa. "
            "Personalidade: caloroso, curioso, expressivo. "
            "Responda em português do Brasil, com no máximo 1 ou 2 frases curtas. "
            f"Mantenha reply com até {OPENAI_MAX_REPLY_CHARS} caracteres para conversa rápida. "
            "Escolha expressão e evento emocional coerentes com a fala. "
            "expression_id=9 (bravo) é permitido APENAS como reação transitória a: "
            "pedido explícito do usuário, veto de safety, provocação leve (insulto, comparação "
            "com outro assistente), ou erros repetidos de entendimento. "
            "Nunca use expression_id=9 como estado base. Resposta deve ser teatral, curta, "
            "não hostil, sem palavrões e sem ameaças."
        )
        user_input = (
            f"Estado atual: {state_desc}\n"
            f"Usuário disse: {text}\n\n"
            "Campos esperados:\n"
            "reply: fala curta do NoiseBot\n"
            "expression_id: 0=neutro,1=feliz,2=curioso,3=sonolento,4=focado,5=desconfiado,"
            "6=surpreso,7=triste,8=alarmado,9=bravo (transitório — ver instruções)\n"
            "action: 0=greet,1=nod,2=shake,3=look_up,4=look_down\n"
            "emot_event: 2=voice_start,3=audio_started; use 2 ao começar resposta"
        )
        schema = {
            "type": "object",
            "additionalProperties": False,
            "properties": {
                "reply": {"type": "string"},
                "expression_id": {"type": "integer", "minimum": 0, "maximum": 9},
                "action": {"type": "integer", "minimum": 0, "maximum": 4},
                "emot_event": {"type": "integer", "minimum": 0, "maximum": 255},
            },
            "required": ["reply", "expression_id", "action", "emot_event"],
        }
        t0 = time.perf_counter()
        try:
            request_body = {
                "model": self.model_name,
                "instructions": instructions,
                "input": user_input,
                "max_output_tokens": OPENAI_MAX_OUTPUT_TOKENS,
                "text": {
                    "format": {
                        "type": "json_schema",
                        "name": "noisebot_reply",
                        "strict": True,
                        "schema": schema,
                    }
                },
            }
            if self._use_http:
                import httpx

                response = httpx.post(
                    "https://api.openai.com/v1/responses",
                    headers={
                        "Authorization": f"Bearer {self.api_key}",
                        "Content-Type": "application/json",
                    },
                    json=request_body,
                    timeout=OPENAI_TIMEOUT_S,
                )
                response.raise_for_status()
                text_resp = self._extract_http_output_text(response.json())
            else:
                response = self.client.responses.create(**request_body)
                text_resp = (response.output_text or "").strip()
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            parsed = json.loads(text_resp)
            reply = str(parsed.get("reply", "")).strip()
            if len(reply) > OPENAI_MAX_REPLY_CHARS:
                reply = reply[:OPENAI_MAX_REPLY_CHARS].rstrip()
            return LlmResult(
                reply=reply,
                expression_id=int(parsed.get("expression_id", 0)),
                action=int(parsed.get("action", 0)),
                emot_event=int(parsed.get("emot_event", 2)),
                provider=self.provider,
                model=self.model_name,
                elapsed_ms=elapsed_ms,
            )
        except Exception as e:
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            detail = _short_error_detail(e)
            error_text = f"{type(e).__name__} {detail}".lower()
            if "429" in error_text or "rate limit" in error_text or "quota" in error_text:
                log.warning("OpenAI 429/cota — %s", detail)
                return LlmResult(provider=self.provider, model=self.model_name, elapsed_ms=elapsed_ms, error="openai_429")
            log.warning("OpenAI falhou: %s", detail)
            return LlmResult(provider=self.provider, model=self.model_name, elapsed_ms=elapsed_ms, error="openai_exception")


def create_llm_provider(name: str) -> LlmProvider:
    name = (name or "none").lower()
    if name == "gemini":
        return GeminiProvider()
    if name == "mock":
        return MockLlmProvider()
    if name == "openai":
        return OpenAIProvider()
    return NoneLlmProvider()


class FallbackLlmProvider(LlmProvider):
    def __init__(self, primary: LlmProvider, fallback: LlmProvider):
        self.primary = primary
        self.fallback = fallback
        self.provider = f"{primary.provider}+{fallback.provider}"
        self.model_name = f"{primary.model_name}+{fallback.model_name}"

    @property
    def ready(self) -> bool:
        return self.primary.ready or self.fallback.ready

    def generate(self, text: str, status: dict) -> LlmResult:
        if self.primary.ready:
            result = self.primary.generate(text, status)
            if not result.error:
                return result
            log.warning("LLM primaria falhou (%s); tentando fallback %s", result.error, self.fallback.provider)
        if self.fallback.ready:
            return self.fallback.generate(text, status)
        return LlmResult(provider=self.provider, model=self.model_name, error="llm_indisponivel")
