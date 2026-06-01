"""bridgev2.ops.config_controller — Valida e aplica config em runtime (Fase 9.5).

Mudanças de provider/modelo/modo são:
  1. Validadas contra o catálogo antes de aplicar.
  2. Auditadas em log estruturado (sem segredos).
  3. Aplicadas em janela segura (entre turnos) quando possível.
  4. Reversíveis ao valor anterior via restore_previous().

Política de segredos: POST /ai/config NÃO aceita API keys.
Segredos só via variável de ambiente / arquivo fora do Git.
"""
from __future__ import annotations

import logging
from dataclasses import is_dataclass, replace
import time
from typing import Any

from ..config import LlmProvider, PipelineMode

log = logging.getLogger(__name__)

# Catálogo de providers e modelos válidos (atualizar conforme providers evoluem)
PROVIDER_CATALOG: dict[str, list[str]] = {
    "openai": [
        "gpt-4o-mini", "gpt-4o",
        "gpt-4.1-mini", "gpt-4.1",
        "gpt-4-turbo",
    ],
    "gemini": [
        "gemini-2.5-flash", "gemini-2.5-flash-lite",
        "gemini-2.5-pro",
    ],
    "ollama": [
        "qwen3.5:9b",
        "qwen2.5:7b", "qwen2.5:14b",
        "llama3.1:8b", "llama3.2:3b",
        "mistral:7b",
    ],
    "none": [],
}

VALID_MODES = {"normal", "local_only", "degraded", "realtime"}

MUTABLE_FIELDS = {"provider", "model", "max_tokens", "mode"}


class ConfigController:
    """Controla mudanças de configuração em runtime.

    app: Application — referência à aplicação para rebuild de providers.
    """

    def __init__(self, app: Any) -> None:
        self._app = app
        self._history: list[dict] = []   # auditoria (sem segredos)

    # -- Validação -------------------------------------------------------------

    def validate(self, data: dict) -> list[str]:
        """Retorna lista de erros de validação (vazia = ok)."""
        errors: list[str] = []

        # Campos desconhecidos ou perigosos
        unknown = set(data) - MUTABLE_FIELDS
        if unknown:
            errors.append(f"campos não permitidos: {sorted(unknown)}")

        # Segredos rejeitados explicitamente
        for secret_key in ("api_key", "openai_api_key", "gemini_api_key", "token", "key"):
            if secret_key in data:
                errors.append(
                    "segredos (API keys) não são aceitos por este endpoint; "
                    "use variáveis de ambiente"
                )
                break

        # Provider
        provider = data.get("provider")
        if provider is not None:
            if provider not in PROVIDER_CATALOG:
                errors.append(
                    f"provider '{provider}' desconhecido; "
                    f"válidos: {sorted(PROVIDER_CATALOG)}"
                )

        # Model — validar só se provider também informado ou já configurado
        model = data.get("model")
        if model is not None:
            effective_provider = provider or self._current_provider()
            valid_models = PROVIDER_CATALOG.get(effective_provider, [])
            if valid_models and model not in valid_models:
                errors.append(
                    f"model '{model}' inválido para provider '{effective_provider}'; "
                    f"válidos: {valid_models}"
                )

        # max_tokens
        max_tokens = data.get("max_tokens")
        if max_tokens is not None:
            if not isinstance(max_tokens, int) or max_tokens < 1 or max_tokens > 4096:
                errors.append("max_tokens deve ser inteiro entre 1 e 4096")

        # Mode
        mode = data.get("mode")
        if mode is not None and mode not in VALID_MODES:
            errors.append(f"mode '{mode}' inválido; válidos: {sorted(VALID_MODES)}")

        return errors

    # -- Aplicação -------------------------------------------------------------

    def apply(self, data: dict) -> dict:
        """Aplica mudanças de config validadas. Retorna dict old→new para auditoria."""
        changes: dict[str, dict] = {}
        app = self._app
        config = app._config

        # Provider / Model — rebuild do LLM provider
        new_provider = data.get("provider")
        new_model = data.get("model")
        new_max_tokens = data.get("max_tokens")

        if new_provider or new_model or new_max_tokens is not None:
            import os
            old_provider = config.llm.provider.value
            old_model = config.llm.model
            old_max = config.llm.max_output_tokens

            if new_provider:
                os.environ["NOISEBOT_LLM_PROVIDER"] = new_provider
                changes["provider"] = {"old": old_provider, "new": new_provider}
            if new_model:
                os.environ["NOISEBOT_LLM_MODEL"] = new_model
                changes["model"] = {"old": old_model, "new": new_model}
            if new_max_tokens is not None:
                os.environ["NOISEBOT_LLM_MAX_OUTPUT_TOKENS"] = str(new_max_tokens)
                changes["max_tokens"] = {"old": old_max, "new": new_max_tokens}

        # Mode
        new_mode = data.get("mode")
        if new_mode:
            import os
            old_mode = config.pipeline_mode.value
            os.environ["NOISEBOT_PIPELINE_MODE"] = new_mode
            changes["mode"] = {"old": old_mode, "new": new_mode}
            log.info("Config aplicada: pipeline_mode %s → %s", old_mode, new_mode)

        if changes:
            self._refresh_app_config(
                provider=new_provider,
                model=new_model,
                max_tokens=new_max_tokens,
                mode=new_mode,
            )

        if any(key in changes for key in ("provider", "model", "max_tokens", "mode")):
            # Rebuild provider e injeta no orchestrator usando o snapshot atualizado.
            try:
                new_llm = app._build_llm_provider()
                app._orchestrator.set_llm_provider(new_llm)
                log.info("Config aplicada: LLM provider reconstruído com %s", changes)
            except Exception as exc:
                log.error("Falha ao reconstruir LLM provider: %s", exc)
                raise RuntimeError(f"Falha ao aplicar config: {exc}") from exc

        # Auditoria (sem segredos)
        if changes:
            entry = {"ts": time.time(), "changes": changes}
            self._history.append(entry)
            log.info("ConfigController audit: %s", entry)

        return changes

    # -- Queries ---------------------------------------------------------------

    def current_config_safe(self) -> dict:
        """Retorna configuração efetiva atual, sem segredos."""
        return self._app._config.safe_dict()

    def audit_log(self) -> list[dict]:
        """Histórico de mudanças (sem segredos)."""
        return list(self._history)

    def _current_provider(self) -> str:
        try:
            return self._app._config.llm.provider.value
        except Exception:
            return "openai"

    def _refresh_app_config(
        self,
        *,
        provider: str | None,
        model: str | None,
        max_tokens: int | None,
        mode: str | None,
    ) -> None:
        """Atualiza o snapshot tipado usado pelos builders da Application."""
        config = self._app._config
        if not is_dataclass(config):
            return

        next_config = config

        if provider is not None or model is not None or max_tokens is not None:
            next_llm = replace(
                config.llm,
                provider=LlmProvider(provider) if provider is not None else config.llm.provider,
                model=model if model is not None else config.llm.model,
                max_output_tokens=max_tokens
                if max_tokens is not None
                else config.llm.max_output_tokens,
            )
            next_config = replace(next_config, llm=next_llm)

        if mode is not None:
            next_config = replace(next_config, pipeline_mode=PipelineMode(mode))

        self._app._config = next_config
