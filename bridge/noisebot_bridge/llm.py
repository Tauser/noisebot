from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
import re
import time

log = logging.getLogger("noisebot_bridge.llm")


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
            "expression_id: 0=neutro,1=feliz,2=curioso,3=sonolento,4=focado,5=desconfiado\n"
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


def create_llm_provider(name: str) -> LlmProvider:
    name = (name or "none").lower()
    if name == "gemini":
        return GeminiProvider()
    if name == "mock":
        return MockLlmProvider()
    if name == "openai":
        log.warning("Provider OpenAI ainda não implementado na 12.13 — usando none")
        return NoneLlmProvider()
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
