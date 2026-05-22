"""bridgev2.ops.http_api — Servidor HTTP local-only async (Fase 9.5).

Endpoints (§11.3 do BRIDGE_V2.md):
  GET  /            Dashboard HTML (painel de operação)
  GET  /health
  GET  /ai/status
  GET  /ai/metrics
  GET  /ai/errors
  GET  /ai/config
  POST /ai/config       (token obrigatório)
  POST /ai/mode         (token obrigatório)
  POST /ai/restart      (token obrigatório)
  POST /ai/metrics/reset (token obrigatório)

Falha isolada: exceções nos handlers retornam JSON de erro sem derrubar o pipeline.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from typing import Any

from aiohttp import web

from .config_controller import ConfigController
from .metrics_api import MetricsApi
from .schemas import (
    ai_status_response,
    error_response,
    health_response,
    ok_response,
)
from .dashboard import get_dashboard_html
from .security import check_token, load_or_create_token
from .status_store import StatusStore
from ..service.healthcheck import is_healthy

log = logging.getLogger(__name__)


class OpsHttpServer:
    """Servidor HTTP local-only para operação e dashboard do NoiseBot.

    Roda no event loop asyncio sem bloqueá-lo. Uma falha deste servidor
    NÃO afeta o pipeline de voz.

    Args:
        app:         Application — para acesso a config e orchestrator.
        store:       StatusStore — estado de runtime do orchestrator.
        host:        Interface de bind (padrão: 127.0.0.1).
        port:        Porta HTTP (padrão: 8765, configurável via NOISEBOT_OPS_PORT).
    """

    def __init__(
        self,
        app: Any,
        store: StatusStore,
        host: str = "127.0.0.1",
        port: int = 8765,
    ) -> None:
        self._app = app
        self._store = store
        self._host = host
        self._port = port
        self._token = load_or_create_token()
        self._ctrl = ConfigController(app)
        self._metrics_api = MetricsApi(app._orchestrator.metrics, store)
        self._t_start = time.monotonic()
        self._runner: web.AppRunner | None = None
        self._web_app = self._build_app()

    def _build_app(self) -> web.Application:
        wa = web.Application(middlewares=[self._error_middleware])
        wa.router.add_get("/",                self._get_dashboard)
        wa.router.add_get("/health",          self._get_health)
        wa.router.add_get("/ai/status",       self._get_ai_status)
        wa.router.add_get("/ai/metrics",      self._get_ai_metrics)
        wa.router.add_get("/ai/errors",       self._get_ai_errors)
        wa.router.add_get("/ai/config",       self._get_ai_config)
        wa.router.add_post("/ai/config",      self._post_ai_config)
        wa.router.add_post("/ai/mode",        self._post_ai_mode)
        wa.router.add_post("/ai/restart",     self._post_ai_restart)
        wa.router.add_post("/ai/metrics/reset", self._post_metrics_reset)
        return wa

    # -- Lifecycle -------------------------------------------------------------

    async def start(self) -> None:
        self._runner = web.AppRunner(self._web_app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self._host, self._port)
        await site.start()
        log.info("Ops API: http://%s:%d (token em ~/.bridgev2/ops_token)", self._host, self._port)

    async def stop(self) -> None:
        if self._runner:
            await self._runner.cleanup()
            log.info("Ops API: encerrado.")

    # -- Middleware ------------------------------------------------------------

    @web.middleware
    async def _error_middleware(self, request: web.Request, handler) -> web.Response:
        try:
            return await handler(request)
        except web.HTTPException:
            raise
        except Exception as exc:
            log.exception("Ops API: erro não tratado em %s %s", request.method, request.path)
            return _json(error_response(str(exc), code=500), status=500)

    # -- Auth helper -----------------------------------------------------------

    def _require_token(self, request: web.Request) -> None:
        if not check_token(request, self._token):
            raise web.HTTPUnauthorized(
                text=json.dumps(error_response("token inválido ou ausente", 401)),
                content_type="application/json",
            )

    # -- GET handlers ----------------------------------------------------------

    async def _get_dashboard(self, request: web.Request) -> web.Response:
        return web.Response(
            text=get_dashboard_html(),
            content_type="text/html",
            charset="utf-8",
        )

    async def _get_health(self, request: web.Request) -> web.Response:
        healthy = is_healthy()
        uptime = time.monotonic() - self._t_start
        return _json(health_response(healthy, uptime), status=200 if healthy else 503)

    async def _get_ai_status(self, request: web.Request) -> web.Response:
        config = self._app._config
        store = self._store

        # api_key_configured: verifica apenas presença, nunca o valor
        provider = config.llm.provider.value
        api_key = (
            bool(os.environ.get("OPENAI_API_KEY"))
            if provider == "openai"
            else bool(os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY"))
        )

        return _json(ai_status_response(
            connected=store.firmware_connected,
            pipeline="v2",
            mode=config.pipeline_mode.value,
            provider=provider,
            model=config.llm.model,
            api_key_configured=api_key,
            stt_status=store.stt_status,
            llm_status=store.llm_status,
            tts_status=store.tts_status,
            last_error=store.last_error,
            last_turn_id=store.last_turn_id,
            last_outcome=store.last_outcome,
        ))

    async def _get_ai_metrics(self, request: web.Request) -> web.Response:
        return _json(self._metrics_api.get_metrics())

    async def _get_ai_errors(self, request: web.Request) -> web.Response:
        limit = int(request.rel_url.query.get("limit", "20"))
        errors = self._store.recent_errors[:limit]
        return _json({"errors": errors, "total": len(errors)})

    async def _get_ai_config(self, request: web.Request) -> web.Response:
        return _json(self._ctrl.current_config_safe())

    # -- POST handlers ---------------------------------------------------------

    async def _post_ai_config(self, request: web.Request) -> web.Response:
        self._require_token(request)
        try:
            data = await request.json()
        except Exception:
            return _json(error_response("body JSON inválido"), status=400)

        if not isinstance(data, dict):
            return _json(error_response("body deve ser um objeto JSON"), status=400)

        errors = self._ctrl.validate(data)
        if errors:
            return _json(error_response("; ".join(errors)), status=422)

        try:
            changes = self._ctrl.apply(data)
        except RuntimeError as exc:
            return _json(error_response(str(exc)), status=500)

        return _json(ok_response("configuração aplicada", changes=changes))

    async def _post_ai_mode(self, request: web.Request) -> web.Response:
        self._require_token(request)
        try:
            data = await request.json()
        except Exception:
            return _json(error_response("body JSON inválido"), status=400)

        mode = data.get("mode", "")
        errors = self._ctrl.validate({"mode": mode})
        if errors:
            return _json(error_response("; ".join(errors)), status=422)

        changes = self._ctrl.apply({"mode": mode})
        return _json(ok_response(f"modo alterado para '{mode}'", changes=changes))

    async def _post_ai_restart(self, request: web.Request) -> web.Response:
        self._require_token(request)
        log.info("Ops API: reinício gracioso solicitado.")
        # Agendar shutdown gracioso no event loop (não bloqueia o handler)
        asyncio.get_event_loop().create_task(
            self._graceful_restart(),
            # name não disponível em Python < 3.11 sem verificação, usa simples
        )
        return _json(ok_response("reinício gracioso agendado"))

    async def _post_metrics_reset(self, request: web.Request) -> web.Response:
        self._require_token(request)
        self._metrics_api.reset()
        log.info("Ops API: métricas zeradas.")
        return _json(ok_response("métricas zeradas"))

    # -- Internos --------------------------------------------------------------

    async def _graceful_restart(self) -> None:
        """Aguarda 1 s (drena turno em andamento) e sinaliza shutdown."""
        await asyncio.sleep(1.0)
        await self._app.shutdown()


def _json(data: dict, status: int = 200) -> web.Response:
    return web.Response(
        text=json.dumps(data, ensure_ascii=False),
        status=status,
        content_type="application/json",
    )
