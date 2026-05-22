"""Integration tests — Fase 9.5: ops HTTP API (test_dashboard_api).

Verifica todos os 9 endpoints contra um servidor aiohttp real.
Usa aiohttp.test_utils.TestClient sem depender de pytest-aiohttp.
"""
from __future__ import annotations

import os
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from bridgev2.config import load_config
from bridgev2.ops.config_controller import ConfigController
from bridgev2.ops.http_api import OpsHttpServer
from bridgev2.ops.status_store import StatusStore
from bridgev2.metrics.registry import MetricsRegistry
from bridgev2.runtime.bus import EventBus
from bridgev2.runtime.events import AudioChunkIn, FinalTranscript, VoiceActivityEnd, VoiceActivityStart

TOKEN = "test-token-deadbeef1234"


def _make_ops(store: StatusStore | None = None, token: str = TOKEN) -> OpsHttpServer:
    if store is None:
        store = StatusStore()

    config = MagicMock()
    config.llm.provider.value = "openai"
    config.llm.model = "gpt-4o-mini"
    config.pipeline_mode.value = "normal"
    config.ops.port = 18765
    config.safe_dict.return_value = {
        "llm": {"provider": "openai", "model": "gpt-4o-mini"},
        "pipeline_mode": "normal",
    }

    orch = MagicMock()
    orch.metrics = MetricsRegistry(window=100)
    orch.set_llm_provider = MagicMock()

    fake_app = MagicMock()
    fake_app._config = config
    fake_app._bus = EventBus(default_maxsize=64)
    fake_app._orchestrator = orch
    fake_app.shutdown = AsyncMock()
    fake_app._build_llm_provider = MagicMock(return_value=None)

    with patch("bridgev2.ops.http_api.load_or_create_token", return_value=token):
        ops = OpsHttpServer(app=fake_app, store=store, host="127.0.0.1", port=18765)
    ops._token = token
    return ops


async def _get(web_app, path: str, **kwargs):
    client = TestClient(TestServer(web_app))
    await client.start_server()
    try:
        return await client.get(path, **kwargs), client
    finally:
        pass  # caller closes


async def _do(method: str, web_app, path: str, **kwargs):
    client = TestClient(TestServer(web_app))
    await client.start_server()
    try:
        resp = await getattr(client, method)(path, **kwargs)
        data = await resp.json()
        return resp, data
    finally:
        await client.close()


# ---------------------------------------------------------------------------
# GET /health
# ---------------------------------------------------------------------------

class TestGetHealth:
    @pytest.mark.asyncio
    async def test_health_returns_200_when_healthy(self):
        ops = _make_ops()
        with patch("bridgev2.ops.http_api.is_healthy", return_value=True):
            resp, data = await _do("get", ops._web_app, "/health")
        assert resp.status == 200
        assert data["status"] == "ok"
        assert "uptime_s" in data

    @pytest.mark.asyncio
    async def test_health_returns_503_when_degraded(self):
        ops = _make_ops()
        with patch("bridgev2.ops.http_api.is_healthy", return_value=False):
            resp, data = await _do("get", ops._web_app, "/health")
        assert resp.status == 503
        assert data["status"] == "degraded"


# ---------------------------------------------------------------------------
# GET /ai/status
# ---------------------------------------------------------------------------

class TestGetAiStatus:
    @pytest.mark.asyncio
    async def test_status_has_all_required_fields(self):
        ops = _make_ops()
        resp, data = await _do("get", ops._web_app, "/ai/status")
        assert resp.status == 200
        required = (
            "connected", "pipeline", "mode", "provider", "model",
            "api_key_configured", "stt_status", "llm_status", "tts_status",
            "last_turn_id", "last_outcome", "last_transcript", "last_reply",
            "last_route", "updated_at",
        )
        for field in required:
            assert field in data, f"campo ausente: {field}"

    @pytest.mark.asyncio
    async def test_api_key_never_exposed(self):
        ops = _make_ops()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-secret-do-not-leak"}):
            resp, _ = await _do("get", ops._web_app, "/ai/status")
        # Verifica o texto bruto da resposta
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-secret-do-not-leak"}):
            r = await client.get("/ai/status")
            body = await r.text()
        await client.close()
        assert "sk-secret-do-not-leak" not in body

    @pytest.mark.asyncio
    async def test_api_key_configured_true_when_present(self):
        ops = _make_ops()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-present"}):
            resp, data = await _do("get", ops._web_app, "/ai/status")
        assert data["api_key_configured"] is True

    @pytest.mark.asyncio
    async def test_api_key_configured_false_when_absent(self):
        ops = _make_ops()
        env_without_key = {k: v for k, v in os.environ.items() if k != "OPENAI_API_KEY"}
        with patch.dict(os.environ, env_without_key, clear=True):
            resp, data = await _do("get", ops._web_app, "/ai/status")
        assert data["api_key_configured"] is False

    @pytest.mark.asyncio
    async def test_connected_reflects_store(self):
        store = StatusStore()
        store.firmware_connected = True
        ops = _make_ops(store)
        _, data = await _do("get", ops._web_app, "/ai/status")
        assert data["connected"] is True

    @pytest.mark.asyncio
    async def test_pipeline_is_v2(self):
        ops = _make_ops()
        _, data = await _do("get", ops._web_app, "/ai/status")
        assert data["pipeline"] == "v2"

    @pytest.mark.asyncio
    async def test_status_includes_last_turn_texts(self):
        store = StatusStore()
        store.record_turn(
            7,
            "llm",
            transcript="qual é a cor do céu?",
            reply="Azul, na maior parte do dia.",
            route="llm",
        )
        ops = _make_ops(store)
        _, data = await _do("get", ops._web_app, "/ai/status")
        assert data["last_turn_id"] == 7
        assert data["last_transcript"] == "qual é a cor do céu?"
        assert data["last_reply"] == "Azul, na maior parte do dia."
        assert data["last_route"] == "llm"

    @pytest.mark.asyncio
    async def test_status_redacts_secrets_from_last_turn_texts(self):
        store = StatusStore()
        store.record_turn(
            8,
            "llm",
            transcript="minha chave é sk-abcdef1234567890",
            reply="Gemini key AIzaSy123456789012345678901234567890",
            route="llm",
        )
        ops = _make_ops(store)
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/ai/status")
        body = await r.text()
        await client.close()
        assert "sk-abcdef1234567890" not in body
        assert "AIzaSy123456789012345678901234567890" not in body
        assert "<redacted>" in body


# ---------------------------------------------------------------------------
# GET /ai/metrics
# ---------------------------------------------------------------------------

class TestGetAiMetrics:
    @pytest.mark.asyncio
    async def test_metrics_returns_200(self):
        ops = _make_ops()
        resp, data = await _do("get", ops._web_app, "/ai/metrics")
        assert resp.status == 200
        assert "latency_ms" in data
        assert "turns" in data

    @pytest.mark.asyncio
    async def test_metrics_includes_recorded_latency(self):
        ops = _make_ops()
        ops._app._orchestrator.metrics.record("stt_ms", 400.0)
        ops._metrics_api._registry = ops._app._orchestrator.metrics
        _, data = await _do("get", ops._web_app, "/ai/metrics")
        assert "stt" in data["latency_ms"]
        assert data["latency_ms"]["stt"]["p50"] is not None


# ---------------------------------------------------------------------------
# GET /ai/errors
# ---------------------------------------------------------------------------

class TestGetAiErrors:
    @pytest.mark.asyncio
    async def test_errors_empty_by_default(self):
        ops = _make_ops()
        _, data = await _do("get", ops._web_app, "/ai/errors")
        assert data["errors"] == []
        assert data["total"] == 0

    @pytest.mark.asyncio
    async def test_errors_shows_recorded(self):
        store = StatusStore()
        store.record_error("llm_failure", turn_id=5, provider="openai", message="timeout")
        ops = _make_ops(store)
        _, data = await _do("get", ops._web_app, "/ai/errors")
        assert data["total"] == 1
        assert data["errors"][0]["kind"] == "llm_failure"

    @pytest.mark.asyncio
    async def test_errors_no_api_key_in_message(self):
        store = StatusStore()
        store.record_error("auth", turn_id=1, message="bad key sk-shouldnotappear123456")
        ops = _make_ops(store)
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/ai/errors")
        body = await r.text()
        await client.close()
        assert "sk-shouldnotappear123456" not in body
        assert "<redacted>" in body


# ---------------------------------------------------------------------------
# GET /ai/config
# ---------------------------------------------------------------------------

class TestGetAiConfig:
    @pytest.mark.asyncio
    async def test_config_returns_200(self):
        ops = _make_ops()
        resp, _ = await _do("get", ops._web_app, "/ai/config")
        assert resp.status == 200

    @pytest.mark.asyncio
    async def test_config_no_api_key(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-should-not-be-here"}):
            r = await client.get("/ai/config")
            body = await r.text()
        await client.close()
        assert "sk-should-not-be-here" not in body


# ---------------------------------------------------------------------------
# POST /ai/config
# ---------------------------------------------------------------------------

class TestPostAiConfig:
    @pytest.mark.asyncio
    async def test_no_token_returns_401(self):
        ops = _make_ops()
        resp, _ = await _do("post", ops._web_app, "/ai/config", json={"model": "gpt-4o"})
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_wrong_token_returns_401(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post", ops._web_app, "/ai/config",
            json={"model": "gpt-4o"},
            headers={"Authorization": "Bearer bad-token"},
        )
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_api_key_in_body_rejected(self):
        ops = _make_ops()
        resp, data = await _do(
            "post", ops._web_app, "/ai/config",
            json={"api_key": "sk-secret"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 422
        assert "segredos" in data["error"].lower() or "api_key" in data["error"]

    @pytest.mark.asyncio
    async def test_unknown_provider_rejected(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post", ops._web_app, "/ai/config",
            json={"provider": "FAKE"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 422

    @pytest.mark.asyncio
    async def test_unknown_fields_rejected(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post", ops._web_app, "/ai/config",
            json={"not_allowed": "value"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 422

    @pytest.mark.asyncio
    async def test_valid_config_accepted(self):
        ops = _make_ops()
        resp, data = await _do(
            "post", ops._web_app, "/ai/config",
            json={"provider": "openai", "model": "gpt-4o-mini"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 200
        assert data["status"] == "ok"

    def test_runtime_config_updates_application_snapshot(self, monkeypatch):
        monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "openai")
        monkeypatch.setenv("NOISEBOT_LLM_MODEL", "gpt-4o-mini")
        monkeypatch.setenv("NOISEBOT_LLM_MAX_OUTPUT_TOKENS", "140")
        monkeypatch.setenv("NOISEBOT_PIPELINE_MODE", "normal")

        fake_app = MagicMock()
        fake_app._config = load_config()
        fake_app._orchestrator.set_llm_provider = MagicMock()
        built_with = []

        def build_llm_provider():
            cfg = fake_app._config
            built_with.append((
                cfg.llm.provider.value,
                cfg.llm.model,
                cfg.llm.max_output_tokens,
                cfg.pipeline_mode.value,
            ))
            return "new-provider"

        fake_app._build_llm_provider.side_effect = build_llm_provider

        changes = ConfigController(fake_app).apply({
            "provider": "gemini",
            "model": "gemini-2.5-flash",
            "max_tokens": 77,
            "mode": "local_only",
        })

        assert changes["provider"]["new"] == "gemini"
        assert fake_app._config.llm.provider.value == "gemini"
        assert fake_app._config.llm.model == "gemini-2.5-flash"
        assert fake_app._config.llm.max_output_tokens == 77
        assert fake_app._config.pipeline_mode.value == "local_only"
        assert built_with[-1] == ("gemini", "gemini-2.5-flash", 77, "local_only")
        fake_app._orchestrator.set_llm_provider.assert_called_once_with("new-provider")


# ---------------------------------------------------------------------------
# POST /ai/mode
# ---------------------------------------------------------------------------

class TestPostAiMode:
    @pytest.mark.asyncio
    async def test_no_token_returns_401(self):
        ops = _make_ops()
        resp, _ = await _do("post", ops._web_app, "/ai/mode", json={"mode": "local_only"})
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_invalid_mode_rejected(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post", ops._web_app, "/ai/mode",
            json={"mode": "INVALID"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 422

    @pytest.mark.asyncio
    async def test_valid_mode_accepted(self):
        ops = _make_ops()
        resp, data = await _do(
            "post", ops._web_app, "/ai/mode",
            json={"mode": "local_only"},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 200


# ---------------------------------------------------------------------------
# POST /ai/metrics/reset
# ---------------------------------------------------------------------------

class TestPostMetricsReset:
    @pytest.mark.asyncio
    async def test_no_token_returns_401(self):
        ops = _make_ops()
        resp, _ = await _do("post", ops._web_app, "/ai/metrics/reset")
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_reset_clears_turn_counters(self):
        store = StatusStore()
        store.turn_counters["total"] = 42
        store.turn_counters["llm"] = 30
        ops = _make_ops(store)
        resp, data = await _do(
            "post", ops._web_app, "/ai/metrics/reset",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 200
        assert store.turn_counters["total"] == 0
        assert store.turn_counters["llm"] == 0


# ---------------------------------------------------------------------------
# GET / — Dashboard HTML
# ---------------------------------------------------------------------------

class TestGetDashboard:
    @pytest.mark.asyncio
    async def test_root_returns_200(self):
        ops = _make_ops()
        resp, _ = await _do("get", ops._web_app, "/health")  # warm-up
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        assert r.status == 200
        await client.close()

    @pytest.mark.asyncio
    async def test_root_content_type_html(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        assert "text/html" in r.content_type
        await client.close()

    @pytest.mark.asyncio
    async def test_dashboard_contains_title(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "Bridge v2" in body

    @pytest.mark.asyncio
    async def test_dashboard_has_status_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "status-grid" in body or "Status" in body

    @pytest.mark.asyncio
    async def test_dashboard_has_metrics_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "Métricas" in body or "metrics" in body.lower()

    @pytest.mark.asyncio
    async def test_dashboard_has_config_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "Configuração" in body or "cfg-provider" in body

    @pytest.mark.asyncio
    async def test_dashboard_has_token_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "token" in body.lower()

    @pytest.mark.asyncio
    async def test_dashboard_never_shows_api_key_value(self):
        """O HTML do dashboard nunca deve referenciar um valor de API key."""
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-should-not-appear-in-html"}):
            r = await client.get("/")
            body = await r.text()
        await client.close()
        assert "sk-should-not-appear-in-html" not in body

    @pytest.mark.asyncio
    async def test_dashboard_has_restart_button(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "restart" in body.lower() or "reinício" in body.lower()

    @pytest.mark.asyncio
    async def test_dashboard_has_errors_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "Erros" in body or "errors" in body.lower()

    @pytest.mark.asyncio
    async def test_dashboard_has_tests_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "Testes" in body or "tests" in body.lower()
        assert "/debug/transcript" in body
        assert "/debug/voice-turn" in body

    @pytest.mark.asyncio
    async def test_dashboard_has_last_turn_detail_section(self):
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "turn-detail" in body
        assert "Resposta da IA" in body

    @pytest.mark.asyncio
    async def test_dashboard_warns_about_api_keys(self):
        """Dashboard deve ter aviso claro sobre não usar API keys aqui."""
        ops = _make_ops()
        client = TestClient(TestServer(ops._web_app))
        await client.start_server()
        r = await client.get("/")
        body = await r.text()
        await client.close()
        assert "variáveis de ambiente" in body or "env" in body.lower()


# ---------------------------------------------------------------------------
# POST /debug/* — Testes manuais via dashboard
# ---------------------------------------------------------------------------

class TestDebugEndpoints:
    @pytest.mark.asyncio
    async def test_debug_transcript_requires_token(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post",
            ops._web_app,
            "/debug/transcript",
            json={"text": "que horas são"},
        )
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_debug_transcript_publishes_final_transcript(self):
        ops = _make_ops()
        q = ops._app._bus.subscribe(FinalTranscript)
        resp, data = await _do(
            "post",
            ops._web_app,
            "/debug/transcript",
            json={"text": "que horas são", "turn_id": 1234},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 200
        assert data["status"] == "ok"
        evt = await q.get()
        assert isinstance(evt, FinalTranscript)
        assert evt.turn_id == 1234
        assert evt.text == "que horas são"

    @pytest.mark.asyncio
    async def test_debug_transcript_rejects_empty_text(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post",
            ops._web_app,
            "/debug/transcript",
            json={"text": ""},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 400

    @pytest.mark.asyncio
    async def test_debug_voice_turn_requires_token(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post",
            ops._web_app,
            "/debug/voice-turn",
            json={"chunks": 2},
        )
        assert resp.status == 401

    @pytest.mark.asyncio
    async def test_debug_voice_turn_publishes_voice_events(self):
        ops = _make_ops()
        q = ops._app._bus.subscribe(VoiceActivityStart, AudioChunkIn, VoiceActivityEnd)
        resp, data = await _do(
            "post",
            ops._web_app,
            "/debug/voice-turn",
            json={"chunks": 3},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 200
        assert data["status"] == "ok"
        events = [await q.get() for _ in range(5)]
        assert isinstance(events[0], VoiceActivityStart)
        assert all(isinstance(evt, AudioChunkIn) for evt in events[1:4])
        assert isinstance(events[4], VoiceActivityEnd)

    @pytest.mark.asyncio
    async def test_debug_voice_turn_rejects_invalid_chunk_count(self):
        ops = _make_ops()
        resp, _ = await _do(
            "post",
            ops._web_app,
            "/debug/voice-turn",
            json={"chunks": 999},
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        assert resp.status == 422
