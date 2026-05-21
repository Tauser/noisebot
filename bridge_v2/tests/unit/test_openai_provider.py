"""Tests for bridgev2.llm.openai_provider (mock-based, sem chamadas reais)."""
from __future__ import annotations

import asyncio
import os
import pytest
import pytest_asyncio

from bridgev2.llm.circuit_breaker import CircuitBreaker, CircuitOpenError, CircuitState
from bridgev2.llm.openai_provider import OpenAIStreamingProvider


# ── Fixtures / Helpers ────────────────────────────────────────────────────────

def _make_provider(**kwargs) -> OpenAIStreamingProvider:
    defaults = dict(model="gpt-4o-mini", failure_threshold=3, reset_timeout=30.0)
    defaults.update(kwargs)
    return OpenAIStreamingProvider(**defaults)


async def _collect_stream(stream) -> list[str]:
    tokens = []
    async for token in stream:
        tokens.append(token)
    return tokens


# ── Testes de construção e propriedades ──────────────────────────────────────

class TestProviderConstruction:
    def test_default_model(self):
        p = _make_provider()
        assert p._model == "gpt-4o-mini"

    def test_custom_model(self):
        p = OpenAIStreamingProvider(model="gpt-4o")
        assert p._model == "gpt-4o"

    def test_circuit_breaker_property(self):
        p = _make_provider()
        assert isinstance(p.circuit_breaker, CircuitBreaker)

    def test_provider_name(self):
        p = _make_provider()
        assert p._provider_name == "openai"

    def test_generate_stream_returns_async_iterator(self):
        p = _make_provider()
        stream = p.generate_stream("oi", {})
        # Must be an async iterable
        assert hasattr(stream, "__aiter__")


# ── Circuit breaker integrado ─────────────────────────────────────────────────

class TestCircuitBreakerIntegration:
    @pytest.mark.asyncio
    async def test_circuit_open_raises_on_stream(self):
        """Quando circuit está OPEN, generate_stream lança CircuitOpenError na iteração."""
        p = _make_provider(failure_threshold=1)
        # Força abertura do circuito
        p.circuit_breaker.record_failure()
        assert p.circuit_breaker.is_open

        stream = p.generate_stream("oi", {})
        with pytest.raises(CircuitOpenError):
            async for _ in stream:
                pass

    @pytest.mark.asyncio
    async def test_circuit_open_raises_on_complete(self):
        """generate_complete também respeita o circuit breaker."""
        p = _make_provider(failure_threshold=1)
        p.circuit_breaker.record_failure()

        with pytest.raises(CircuitOpenError):
            await p.generate_complete("oi", {})

    def test_record_failure_increments(self):
        p = _make_provider(failure_threshold=5)
        p.circuit_breaker.record_failure()
        p.circuit_breaker.record_failure()
        assert p.circuit_breaker.failure_count == 2

    def test_record_success_resets(self):
        p = _make_provider()
        p.circuit_breaker.record_failure()
        p.circuit_breaker.record_success()
        assert p.circuit_breaker.failure_count == 0
        assert p.circuit_breaker.is_closed

    def test_threshold_reached_opens_circuit(self):
        p = _make_provider(failure_threshold=3)
        for _ in range(3):
            p.circuit_breaker.record_failure()
        assert p.circuit_breaker.is_open


# ── Testes de _get_client (sem OPENAI_API_KEY) ────────────────────────────────

class TestMissingApiKey:
    @pytest.mark.asyncio
    async def test_missing_api_key_raises_on_stream(self, monkeypatch):
        """generate_stream deve falhar graciosamente se OPENAI_API_KEY ausente."""
        monkeypatch.delenv("OPENAI_API_KEY", raising=False)
        p = _make_provider()
        stream = p.generate_stream("oi", {})
        with pytest.raises((ValueError, RuntimeError)):
            async for _ in stream:
                pass

    @pytest.mark.asyncio
    async def test_missing_api_key_does_not_affect_closed_circuit(self, monkeypatch):
        """Chave ausente é erro de configuração — não dispara circuit breaker."""
        monkeypatch.delenv("OPENAI_API_KEY", raising=False)
        p = _make_provider(failure_threshold=5)
        stream = p.generate_stream("oi", {})
        try:
            async for _ in stream:
                pass
        except (ValueError, RuntimeError):
            pass
        # Circuito permanece CLOSED — chave ausente é misconfiguration, não falha transiente
        assert p.circuit_breaker.is_closed
