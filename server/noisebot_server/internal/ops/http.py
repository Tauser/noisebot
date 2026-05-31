"""noisebot_server.internal.ops.http — servidor HTTP local-only async.

Endpoints operacionais:
  GET  /            Dashboard HTML (painel de operação)
  GET  /health
  GET  /ai/status
  GET  /ai/metrics
  GET  /ai/errors
  GET  /ai/config
  GET  /api/logs
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
from pathlib import Path
from typing import Any

from aiohttp import web

from .app_state import AppStateStore
from .config import ConfigController
from .metrics import MetricsApi
from .schemas import (
    ai_status_response,
    error_response,
    health_response,
    ok_response,
)
from .dashboard import get_dashboard_html
from .firmware_agenda import FirmwareAgendaClient, FirmwareAgendaError
from .firmware_diag import FirmwareDiagClient, FirmwareDiagError
from .log_buffer import install_recent_log_handler
from .security import check_token, load_or_create_token
from .status import StatusStore
from ..vision import VisionClient, VisionError
from ..agent.runtime import (
    AudioChunkIn,
    FinalTranscript,
    VoiceActivityEnd,
    VoiceActivityStart,
    new_turn_id,
)
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
        self._app_state = AppStateStore()
        self._log_buffer = install_recent_log_handler()
        self._agenda_client = FirmwareAgendaClient.from_config(app._config)
        self._firmware_diag_client = FirmwareDiagClient.from_config(app._config)
        self._vision_client = VisionClient.from_config(app._config)
        self._app_dist = _find_app_dist()
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
        wa.router.add_post("/debug/transcript", self._post_debug_transcript)
        wa.router.add_post("/debug/voice-turn", self._post_debug_voice_turn)
        wa.router.add_get("/api/app/state", self._get_app_state)
        wa.router.add_get("/api/logs", self._get_logs)
        wa.router.add_get("/api/agenda/items", self._get_agenda_items)
        wa.router.add_post("/api/agenda/timers", self._post_agenda_timer)
        wa.router.add_post("/api/agenda/alarms", self._post_agenda_alarm)
        wa.router.add_post("/api/agenda/reminders", self._post_agenda_reminder)
        wa.router.add_patch("/api/agenda/items/{item_id}", self._patch_agenda_item)
        wa.router.add_delete("/api/agenda/items/{item_id}", self._delete_agenda_item)
        wa.router.add_get("/api/settings/basic", self._get_basic_settings)
        wa.router.add_put("/api/settings/basic", self._put_basic_settings)
        wa.router.add_get("/api/settings/advanced", self._get_advanced_settings)
        wa.router.add_put("/api/settings/advanced", self._put_advanced_settings)
        wa.router.add_get("/api/profile", self._get_profile)
        wa.router.add_put("/api/profile", self._put_profile)
        wa.router.add_post("/api/profile/test-voice", self._post_profile_test_voice)
        wa.router.add_get("/api/device/status", self._get_device_status)
        wa.router.add_get("/api/device/diagnostics", self._get_device_diagnostics)
        wa.router.add_get("/api/device/audio/files", self._get_device_audio_files)
        wa.router.add_get("/api/device/audio/files/{name}", self._get_device_audio_file)
        wa.router.add_get("/api/device/audio/processor", self._get_device_audio_processor)
        wa.router.add_post("/api/device/audio/processor/probe", self._post_device_audio_processor_probe)
        wa.router.add_post("/api/device/audio/processor/shadow/start", self._post_device_audio_processor_shadow_start)
        wa.router.add_post("/api/device/audio/processor/shadow/stop", self._post_device_audio_processor_shadow_stop)
        wa.router.add_post("/api/device/audio/processor/bridge/start", self._post_device_audio_processor_bridge_start)
        wa.router.add_post("/api/device/audio/processor/bridge/stop", self._post_device_audio_processor_bridge_stop)
        wa.router.add_get("/api/device/audio/opus/worker", self._get_device_audio_opus_worker)
        wa.router.add_post("/api/device/audio/opus/worker/probe", self._post_device_audio_opus_worker_probe)
        wa.router.add_post("/api/device/audio/opus/worker/start", self._post_device_audio_opus_worker_start)
        wa.router.add_post("/api/device/audio/opus/worker/stop", self._post_device_audio_opus_worker_stop)
        wa.router.add_post("/api/device/audio/opus/worker/encode-test", self._post_device_audio_opus_worker_encode_test)
        wa.router.add_post("/api/device/audio/opus/worker/drain-packets", self._post_device_audio_opus_worker_drain_packets)
        wa.router.add_post("/api/device/audio/opus/transport/enable", self._post_device_audio_opus_transport_enable)
        wa.router.add_post("/api/device/audio/opus/transport/disable", self._post_device_audio_opus_transport_disable)
        wa.router.add_get("/api/device/audio/capture-v2", self._get_device_audio_capture_v2)
        wa.router.add_post("/api/device/audio/capture-v2/replay", self._post_device_audio_capture_v2_replay)
        wa.router.add_post("/api/device/audio/capture-v2/cancel", self._post_device_audio_capture_v2_cancel)
        wa.router.add_get("/api/device/audio/codec-v2", self._get_device_audio_codec_v2)
        wa.router.add_post("/api/device/audio/codec-v2/encode-test", self._post_device_audio_codec_v2_encode_test)
        wa.router.add_post("/api/device/audio/codec-v2/drain", self._post_device_audio_codec_v2_drain)
        wa.router.add_post("/api/device/audio/codec-v2/egress/drain", self._post_device_audio_codec_v2_egress_drain)
        wa.router.add_post("/api/device/audio/codec-v2/reset", self._post_device_audio_codec_v2_reset)
        wa.router.add_post("/api/device/audio/codec-v2/opus-encode-test", self._post_device_audio_codec_v2_opus_encode_test)
        wa.router.add_post("/api/device/audio/codec-v2/worker/start", self._post_device_audio_codec_v2_worker_start)
        wa.router.add_post("/api/device/audio/codec-v2/worker/stop", self._post_device_audio_codec_v2_worker_stop)
        wa.router.add_post("/api/device/audio/codec-v2/worker/stress-test", self._post_device_audio_codec_v2_worker_stress_test)
        wa.router.add_post("/api/device/audio/codec-v2/worker/feed-test", self._post_device_audio_codec_v2_worker_feed_test)
        wa.router.add_post("/api/device/audio/codec-v2/bridge-handoff-test", self._post_device_audio_codec_v2_bridge_handoff_test)
        wa.router.add_post("/api/device/audio/codec-v2/overflow-test", self._post_device_audio_codec_v2_overflow_test)
        wa.router.add_get("/api/vision/status", self._get_vision_status)
        wa.router.add_get("/api/vision/observe", self._get_vision_observe)
        wa.router.add_get("/api/vision/analyze", self._get_vision_analyze)
        wa.router.add_get("/api/vision/snapshot", self._get_vision_snapshot)
        wa.router.add_get("/api/vision/stream.mjpg", self._get_vision_stream)
        if self._app_dist is not None and (self._app_dist / "assets").exists():
            wa.router.add_static("/assets", self._app_dist / "assets")
        wa.router.add_get("/{tail:.*}", self._get_spa_fallback)
        return wa

    # -- Lifecycle -------------------------------------------------------------

    async def start(self) -> None:
        self._runner = web.AppRunner(self._web_app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self._host, self._port)
        await site.start()
        log.info(
            "Ops API: http://%s:%d (token em ~/.noisebot-server/ops_token)",
            self._host,
            self._port,
        )

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
        if self._app_dist is not None:
            return web.FileResponse(self._app_dist / "index.html")
        return web.Response(
            text=get_dashboard_html(),
            content_type="text/html",
            charset="utf-8",
        )

    async def _get_spa_fallback(self, request: web.Request) -> web.StreamResponse:
        if request.path.startswith(("/api/", "/ai/", "/debug/")):
            raise web.HTTPNotFound()
        return await self._get_dashboard(request)

    async def _get_health(self, request: web.Request) -> web.Response:
        healthy = is_healthy()
        uptime = time.monotonic() - self._t_start
        return _json(health_response(healthy, uptime), status=200 if healthy else 503)

    async def _get_ai_status(self, request: web.Request) -> web.Response:
        config = self._app._config
        store = self._store

        # api_key_configured: verifica apenas presença, nunca o valor
        provider = config.llm.provider.value
        if provider == "openai":
            api_key = bool(os.environ.get("OPENAI_API_KEY"))
        elif provider == "gemini":
            api_key = bool(os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY"))
        else:
            api_key = provider == "ollama"

        supervisor = getattr(self._app, "_supervisor", None)
        adapter = self._get_adapter()
        live_connected = bool(
            supervisor is not None and getattr(supervisor, "is_connected", False)
        )
        store.firmware_connected = live_connected
        capabilities = {}
        if adapter is not None:
            capabilities = getattr(adapter, "peer_capabilities", {}) or {}

        return _json(ai_status_response(
            connected=live_connected,
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
            last_transcript=store.last_transcript,
            last_reply=store.last_reply,
            last_route=store.last_route,
            firmware_capabilities=capabilities,
        ))

    async def _get_ai_metrics(self, request: web.Request) -> web.Response:
        return _json(self._metrics_api.get_metrics())

    async def _get_ai_errors(self, request: web.Request) -> web.Response:
        limit = int(request.rel_url.query.get("limit", "20"))
        errors = self._store.recent_errors[:limit]
        return _json({"errors": errors, "total": len(errors)})

    async def _get_logs(self, request: web.Request) -> web.Response:
        limit = _int_query(request, "limit", 80, min_value=1, max_value=300)
        return _json({
            "logs": self._log_buffer.recent(limit),
            "total": self._log_buffer.count,
        })

    async def _get_ai_config(self, request: web.Request) -> web.Response:
        return _json(self._ctrl.current_config_safe())

    async def _get_app_state(self, request: web.Request) -> web.Response:
        await self._sync_firmware_agenda()
        return _json(self._app_state.snapshot())

    async def _get_agenda_items(self, request: web.Request) -> web.Response:
        await self._sync_firmware_agenda()
        return _json(self._app_state.list_agenda())

    async def _get_basic_settings(self, request: web.Request) -> web.Response:
        return _json({"settings": self._app_state.get_basic_settings()})

    async def _get_advanced_settings(self, request: web.Request) -> web.Response:
        return _json({"advanced": self._app_state.get_advanced_settings()})

    async def _get_profile(self, request: web.Request) -> web.Response:
        return _json({"profile": self._app_state.get_profile()})

    async def _get_device_status(self, request: web.Request) -> web.Response:
        config = self._app._config
        supervisor = getattr(self._app, "_supervisor", None)
        adapter = self._get_adapter()
        return _json({
            "server_online": True,
            "firmware_online": bool(adapter is not None and getattr(adapter, "is_connected", False)),
            "transport_host": getattr(config.transport, "host", ""),
            "transport_port": getattr(config.transport, "port", 0),
            "ops_port": config.ops.port,
            "dry_run": bool(config.dry_run),
            "features": [] if adapter is None else getattr(adapter, "peer_features", []),
            "supervisor": "active" if supervisor is not None else "disabled",
            "diagnostics_available": self._firmware_diag_client is not None,
        })

    async def _get_device_diagnostics(self, request: web.Request) -> web.Response:
        if self._firmware_diag_client is None:
            return _json({
                "available": False,
                "source": "unconfigured",
                "errors": {
                    "config": "NOISEBOT_ROBOT_HTTP_URL ou NOISEBOT_HOST nao configurado",
                },
            })
        try:
            payload = await asyncio.to_thread(self._firmware_diag_client.collect)
        except FirmwareDiagError as exc:
            return _json({
                "available": False,
                "source": "firmware_http",
                "base_url": self._firmware_diag_client.base_url.rstrip("/"),
                "errors": {"request": str(exc)},
            })
        return _json({"source": "firmware_http", **payload})

    async def _get_device_audio_files(self, request: web.Request) -> web.Response:
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(self._firmware_diag_client.list_audio_files)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        return _json({"source": "firmware_http", **payload})

    async def _get_device_audio_file(self, request: web.Request) -> web.Response:
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        name = request.match_info.get("name", "")
        try:
            payload = await asyncio.to_thread(self._firmware_diag_client.download_audio_file, name)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=404)
        headers = {"Content-Disposition": f'attachment; filename="{name}"'}
        return web.Response(body=payload, content_type="audio/wav", headers=headers)

    async def _get_device_audio_processor(self, request: web.Request) -> web.Response:
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(self._firmware_diag_client.audio_processor_status)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        return _json({"source": "firmware_http", **payload})

    async def _post_device_audio_processor_probe(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(self._firmware_diag_client.audio_processor_probe)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _post_device_audio_processor_shadow_start(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(
                self._firmware_diag_client.audio_processor_shadow_start
            )
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _post_device_audio_processor_shadow_stop(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(
                self._firmware_diag_client.audio_processor_shadow_stop
            )
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _post_device_audio_processor_bridge_start(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(
                self._firmware_diag_client.audio_processor_bridge_start
            )
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _post_device_audio_processor_bridge_stop(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(
                self._firmware_diag_client.audio_processor_bridge_stop
            )
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _get_device_audio_opus_worker(self, request: web.Request) -> web.Response:
        return await self._proxy_firmware_diag_get(
            self._firmware_diag_client.audio_opus_worker_status
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_worker_probe(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_worker_probe
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_worker_start(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_worker_start
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_worker_stop(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_worker_stop
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_worker_encode_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_worker_encode_test
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_worker_drain_packets(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_worker_drain_packets
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_transport_enable(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_transport_enable
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_opus_transport_disable(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_opus_transport_disable
            if self._firmware_diag_client is not None else None
        )

    async def _get_device_audio_capture_v2(self, request: web.Request) -> web.Response:
        return await self._proxy_firmware_diag_get(
            self._firmware_diag_client.audio_capture_v2_status
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_capture_v2_replay(self, request: web.Request) -> web.Response:
        self._require_token(request)
        if self._firmware_diag_client is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            body = await request.json()
        except json.JSONDecodeError:
            body = {}
        if not isinstance(body, dict):
            return _json(error_response("corpo JSON invalido"), status=400)
        try:
            payload = await asyncio.to_thread(
                self._firmware_diag_client.audio_capture_v2_replay,
                body,
            )
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _post_device_audio_capture_v2_cancel(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_capture_v2_cancel
            if self._firmware_diag_client is not None else None
        )

    async def _get_device_audio_codec_v2(self, request: web.Request) -> web.Response:
        return await self._proxy_firmware_diag_get(
            self._firmware_diag_client.audio_codec_v2_status
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_encode_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_encode_test
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_drain(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_drain
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_egress_drain(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_egress_drain
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_reset(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_reset
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_opus_encode_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_opus_encode_test
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_worker_start(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_worker_start
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_worker_stop(self, request: web.Request) -> web.Response:
        self._require_token(request)
        return await self._proxy_firmware_diag_post(
            self._firmware_diag_client.audio_codec_v2_worker_stop
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_worker_stress_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        packets = int(data.get("packets", 10)) if data is not None else 10
        return await self._proxy_firmware_diag_post(
            (lambda: self._firmware_diag_client.audio_codec_v2_worker_stress_test(packets))
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_worker_feed_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        frames = int(data.get("frames", 10)) if data is not None else 10
        return await self._proxy_firmware_diag_post(
            (lambda: self._firmware_diag_client.audio_codec_v2_worker_feed_test(frames))
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_bridge_handoff_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        frames = int(data.get("frames", 10)) if data is not None else 10
        return await self._proxy_firmware_diag_post(
            (lambda: self._firmware_diag_client.audio_codec_v2_bridge_handoff_test(frames))
            if self._firmware_diag_client is not None else None
        )

    async def _post_device_audio_codec_v2_overflow_test(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        packets = int(data.get("packets", 45)) if data is not None else 45
        return await self._proxy_firmware_diag_post(
            (lambda: self._firmware_diag_client.audio_codec_v2_overflow_test(packets))
            if self._firmware_diag_client is not None else None
        )

    async def _proxy_firmware_diag_get(self, fn) -> web.Response:
        if fn is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(fn)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        return _json({"source": "firmware_http", **payload})

    async def _proxy_firmware_diag_post(self, fn) -> web.Response:
        if fn is None:
            return _json(error_response("firmware HTTP não configurado"), status=503)
        try:
            payload = await asyncio.to_thread(fn)
        except FirmwareDiagError as exc:
            return _json(error_response(str(exc)), status=503)
        status = 200 if payload.get("ok", False) else 500
        return _json({"source": "firmware_http", **payload}, status=status)

    async def _get_vision_status(self, request: web.Request) -> web.Response:
        return _json({
            "available": self._vision_client is not None,
            "source": "firmware_http" if self._vision_client is not None else "unconfigured",
        })

    async def _get_vision_observe(self, request: web.Request) -> web.Response:
        if self._vision_client is None:
            return _json(error_response("visão não configurada"), status=503)
        try:
            observation = await asyncio.to_thread(self._vision_client.observe)
        except VisionError as exc:
            return _json(error_response(str(exc)), status=503)
        return _json(ok_response("observação capturada", observation=_vision_observation_dict(observation)))

    async def _get_vision_analyze(self, request: web.Request) -> web.Response:
        if self._vision_client is None:
            return _json(error_response("visão não configurada"), status=503)
        try:
            analysis = await asyncio.to_thread(self._vision_client.analyze)
        except VisionError as exc:
            return _json(error_response(str(exc)), status=503)
        return _json(ok_response("análise visual concluída", analysis=_vision_analysis_dict(analysis)))

    async def _get_vision_snapshot(self, request: web.Request) -> web.Response:
        if self._vision_client is None:
            return _json(error_response("visão não configurada"), status=503)
        try:
            jpeg = await asyncio.to_thread(self._vision_client.snapshot)
        except VisionError as exc:
            return _json(error_response(str(exc)), status=503)
        return web.Response(
            body=jpeg,
            content_type="image/jpeg",
            headers={"Cache-Control": "no-store"},
        )

    async def _get_vision_stream(self, request: web.Request) -> web.StreamResponse:
        if self._vision_client is None:
            return _json(error_response("visão não configurada"), status=503)

        boundary = "noisebot-frame"
        response = web.StreamResponse(
            status=200,
            headers={
                "Cache-Control": "no-store, no-cache, must-revalidate",
                "Connection": "close",
                "Content-Type": f"multipart/x-mixed-replace; boundary={boundary}",
                "Pragma": "no-cache",
            },
        )
        await response.prepare(request)

        frame_delay_s = _env_float("NOISEBOT_VISION_STREAM_DELAY_S", 0.05)
        while True:
            try:
                jpeg = await asyncio.to_thread(self._vision_client.snapshot)
                await response.write(
                    (
                        f"--{boundary}\r\n"
                        "Content-Type: image/jpeg\r\n"
                        f"Content-Length: {len(jpeg)}\r\n\r\n"
                    ).encode("ascii")
                )
                await response.write(jpeg)
                await response.write(b"\r\n")
                await asyncio.sleep(frame_delay_s)
            except (asyncio.CancelledError, ConnectionResetError, BrokenPipeError):
                break
            except VisionError as exc:
                log.debug("Ops API: stream de visao interrompido: %s", exc)
                await asyncio.sleep(0.5)
        return response

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

    async def _post_debug_transcript(self, request: web.Request) -> web.Response:
        self._require_token(request)
        try:
            data = await request.json()
        except Exception:
            return _json(error_response("body JSON inválido"), status=400)

        text = str(data.get("text", "")).strip() if isinstance(data, dict) else ""
        if not text:
            return _json(error_response("text obrigatório"), status=400)
        if len(text) > 500:
            return _json(error_response("text deve ter no máximo 500 caracteres"), status=422)

        turn_id = _safe_turn_id(data.get("turn_id")) if isinstance(data, dict) else 0
        if turn_id <= 0:
            turn_id = new_turn_id()

        bus = getattr(self._app, "_bus", None)
        if bus is None:
            return _json(error_response("event bus indisponível"), status=503)

        await bus.publish(FinalTranscript(turn_id=turn_id, text=text))
        log.info("Ops debug: transcript injetado turn_id=%d chars=%d", turn_id, len(text))
        return _json(ok_response("transcript injetado", turn_id=turn_id, text=text))

    async def _post_debug_voice_turn(self, request: web.Request) -> web.Response:
        self._require_token(request)
        try:
            data = await request.json()
        except Exception:
            data = {}

        chunks = 40
        if isinstance(data, dict) and data.get("chunks") is not None:
            try:
                chunks = int(data.get("chunks"))
            except (TypeError, ValueError):
                return _json(error_response("chunks deve ser inteiro"), status=400)
        if chunks < 1 or chunks > 300:
            return _json(error_response("chunks deve estar entre 1 e 300"), status=422)

        bus = getattr(self._app, "_bus", None)
        if bus is None:
            return _json(error_response("event bus indisponível"), status=503)

        await bus.publish(VoiceActivityStart())
        silence = bytes(512)
        for seq in range(1, chunks + 1):
            await bus.publish(AudioChunkIn(pcm=silence, seq=seq))
        await bus.publish(VoiceActivityEnd())
        log.info("Ops debug: voice-turn sintético injetado chunks=%d", chunks)
        return _json(ok_response("voice-turn sintético injetado", chunks=chunks))

    async def _post_agenda_timer(self, request: web.Request) -> web.Response:
        return await self._create_agenda_item(request, "timer")

    async def _post_agenda_alarm(self, request: web.Request) -> web.Response:
        return await self._create_agenda_item(request, "alarm")

    async def _post_agenda_reminder(self, request: web.Request) -> web.Response:
        return await self._create_agenda_item(request, "reminder")

    async def _create_agenda_item(self, request: web.Request, kind: str) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        if data is None:
            return _json(error_response("body JSON inválido"), status=400)
        try:
            item = self._app_state.create_agenda_item(kind, data)
        except ValueError as exc:
            return _json(error_response(str(exc)), status=422)
        applied = await self._apply_agenda_item(item, "create")
        if applied == "firmware":
            await self._sync_firmware_agenda()
        return _json(
            ok_response(
                "item criado",
                item=item,
                agenda=self._app_state.list_agenda(),
                applied=applied,
            )
        )

    async def _patch_agenda_item(self, request: web.Request) -> web.Response:
        self._require_token(request)
        item_id = request.match_info.get("item_id", "")
        data = await _read_json_object(request)
        if data is None:
            return _json(error_response("body JSON inválido"), status=400)
        item = self._app_state.update_agenda_item(item_id, data)
        if item is None:
            return _json(error_response("item não encontrado", code=404), status=404)
        if item.get("kind") == "alarm" and any(key in data for key in ("title", "time", "repeat")):
            action = "recreate"
        elif item.get("kind") == "alarm" and "enabled" in data:
            action = "set_enabled"
        else:
            action = "enable" if item.get("enabled", True) else "cancel"
        applied = await self._apply_agenda_item(item, action)
        if applied == "firmware":
            await self._sync_firmware_agenda()
        return _json(
            ok_response(
                "item atualizado",
                item=item,
                agenda=self._app_state.list_agenda(),
                applied=applied,
            )
        )

    async def _delete_agenda_item(self, request: web.Request) -> web.Response:
        self._require_token(request)
        item_id = request.match_info.get("item_id", "")
        item = self._app_state.get_agenda_item(item_id)
        if not self._app_state.delete_agenda_item(item_id):
            return _json(error_response("item não encontrado", code=404), status=404)
        applied = await self._apply_agenda_item(item, "cancel") if item is not None else "saved_only"
        if applied == "firmware":
            await self._sync_firmware_agenda()
        return _json(ok_response("item removido", agenda=self._app_state.list_agenda(), applied=applied))

    async def _put_basic_settings(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        if data is None:
            return _json(error_response("body JSON inválido"), status=400)

        settings = self._app_state.update_basic_settings(data)
        applied = await self._apply_available_basic_settings(data, settings)
        return _json(ok_response("ajustes salvos", settings=settings, applied=applied))

    async def _put_advanced_settings(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        if data is None:
            return _json(error_response("body JSON inválido"), status=400)
        advanced = self._app_state.update_advanced_settings(data)
        applied = await self._apply_advanced_settings(data, advanced)
        return _json(ok_response("configurações salvas", advanced=advanced, applied=applied))

    async def _put_profile(self, request: web.Request) -> web.Response:
        self._require_token(request)
        data = await _read_json_object(request)
        if data is None:
            return _json(error_response("body JSON inválido"), status=400)
        profile = self._app_state.update_profile(data)
        applied = await self._apply_profile(profile)
        return _json(ok_response("perfil salvo", profile=profile, applied=applied))

    async def _post_profile_test_voice(self, request: web.Request) -> web.Response:
        self._require_token(request)
        profile = self._app_state.get_profile()
        name = profile.get("assistant_name", "NoiseBot")
        text = f"Olá, eu sou {name}. Minha voz está funcionando."
        spoken = await self._speak_text(text)
        status = "firmware" if spoken else "unavailable"
        return _json(ok_response("teste de voz solicitado", spoken=spoken, applied=status))

    # -- Internos --------------------------------------------------------------

    async def _apply_available_basic_settings(
        self,
        request_data: dict[str, Any],
        settings: dict[str, bool | int],
    ) -> dict[str, str]:
        applied: dict[str, str] = {}
        get_adapter = getattr(self._app, "_get_adapter", None)
        adapter = get_adapter() if callable(get_adapter) else None
        connected = adapter is not None and getattr(adapter, "is_connected", False)

        if "volume" in request_data:
            if connected:
                try:
                    await adapter.send_volume(int(settings["volume"]))
                    applied["volume"] = "firmware"
                except Exception as exc:
                    log.warning("Ops API: volume salvo, envio ao firmware falhou: %s", exc)
                    applied["volume"] = "saved_only"
            else:
                applied["volume"] = "saved_only"

        if "led_brightness" in request_data:
            if connected:
                brightness = _percent_to_u8(int(settings["led_brightness"]))
                try:
                    await adapter.send_session(
                        {
                            "event": "SETTINGS_COMMAND",
                            "led_brightness": brightness,
                        }
                    )
                    applied["led_brightness"] = "firmware"
                except Exception as exc:
                    log.warning("Ops API: brilho LED salvo, envio ao firmware falhou: %s", exc)
                    applied["led_brightness"] = "saved_only"
            else:
                applied["led_brightness"] = "saved_only"

        if "display_brightness" in request_data:
            if connected:
                brightness = _percent_to_u8(int(settings["display_brightness"]))
                try:
                    await adapter.send_session(
                        {
                            "event": "SETTINGS_COMMAND",
                            "display_brightness": brightness,
                        }
                    )
                    applied["display_brightness"] = "firmware"
                except Exception as exc:
                    log.warning("Ops API: brilho tela salvo, envio ao firmware falhou: %s", exc)
                    applied["display_brightness"] = "saved_only"
            else:
                applied["display_brightness"] = "saved_only"

        for key in ("display_brightness", "led_brightness"):
            if key in request_data and key not in applied:
                applied[key] = "saved_only"
        return applied

    async def _apply_advanced_settings(
        self,
        request_data: dict[str, Any],
        advanced: dict[str, bool | int | str],
    ) -> dict[str, str]:
        applied: dict[str, str] = {}
        adapter = self._get_adapter()
        if adapter is None or not getattr(adapter, "is_connected", False):
            return {key: "saved_only" for key in request_data}
        payload_keys = {
            "wifi_enabled",
            "logs_enabled",
            "log_level",
            "servos_enabled",
            "timezone",
            "location",
            "device_name",
        }
        payload = {
            "event": "APP_ADVANCED_SETTINGS",
            **{key: advanced[key] for key in payload_keys if key in request_data and key in advanced},
        }
        if len(payload) <= 1:
            return {key: "saved_only" for key in request_data}
        try:
            await adapter.send_session(payload)
        except Exception as exc:
            log.warning("Ops API: configurações salvas, envio ao firmware falhou: %s", exc)
            return {key: "saved_only" for key in request_data}
        for key in request_data:
            applied[key] = "firmware" if key in payload_keys else "saved_only"
        return applied

    async def _apply_profile(self, profile: dict[str, str]) -> str:
        adapter = self._get_adapter()
        if adapter is None or not getattr(adapter, "is_connected", False):
            return "saved_only"
        try:
            await adapter.send_session({"event": "APP_PROFILE", **profile})
        except Exception as exc:
            log.warning("Ops API: perfil salvo, envio ao firmware falhou: %s", exc)
            return "saved_only"
        return "firmware"

    async def _apply_agenda_item(self, item: dict[str, Any], action: str) -> str:
        payload = _agenda_session_payload(item, action)
        if payload is None:
            return "saved_only"
        payloads = payload if isinstance(payload, list) else [payload]

        get_adapter = getattr(self._app, "_get_adapter", None)
        adapter = get_adapter() if callable(get_adapter) else None
        if adapter is None or not getattr(adapter, "is_connected", False):
            return "saved_only"

        try:
            for current in payloads:
                await adapter.send_session(current)
        except Exception as exc:
            log.warning("Ops API: agenda salva, envio ao firmware falhou: %s", exc)
            return "saved_only"
        return "firmware"

    def _get_adapter(self) -> Any | None:
        get_adapter = getattr(self._app, "_get_adapter", None)
        return get_adapter() if callable(get_adapter) else None

    async def _speak_text(self, text: str) -> bool:
        adapter = self._get_adapter()
        tts = getattr(self._app, "_tts_provider", None)
        if adapter is None or not getattr(adapter, "is_connected", False) or tts is None:
            return False

        async def _sentences():
            yield text

        turn_id = int(time.time() * 1000) & 0x7FFFFFFF
        sent = False
        try:
            await adapter.send_say_begin(turn_id)
            async for pcm in tts.synthesize_stream(_sentences()):
                await adapter.send_say(pcm)
                sent = True
            await adapter.send_say_end(turn_id)
        except Exception as exc:
            log.warning("Ops API: teste de voz falhou: %s", exc)
            return False
        return sent

    async def _sync_firmware_agenda(self) -> None:
        if self._agenda_client is None:
            return
        try:
            payload = await asyncio.to_thread(self._agenda_client.fetch)
        except FirmwareAgendaError as exc:
            log.debug("Ops API: agenda do firmware indisponivel: %s", exc)
            return
        imported = await asyncio.to_thread(self._app_state.import_firmware_agenda, payload)
        if imported:
            log.debug("Ops API: agenda sincronizada do firmware itens=%d", imported)

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


def _int_query(
    request: web.Request,
    key: str,
    default: int,
    *,
    min_value: int,
    max_value: int,
) -> int:
    try:
        value = int(request.rel_url.query.get(key, str(default)))
    except (TypeError, ValueError):
        value = default
    return max(min_value, min(max_value, value))


async def _read_json_object(request: web.Request) -> dict[str, Any] | None:
    try:
        data = await request.json()
    except Exception:
        return None
    return data if isinstance(data, dict) else None


def _safe_turn_id(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _agenda_session_payload(
    item: dict[str, Any],
    action: str,
) -> dict[str, Any] | list[dict[str, Any]] | None:
    kind = item.get("kind")
    title = str(item.get("title", ""))
    firmware_id = item.get("firmware_id")
    if kind == "timer":
        if action == "create" or action == "enable":
            duration_min = int(item.get("duration_min", 5))
            return {
                "event": "AGENDA_COMMAND",
                "action": "timer_create",
                "duration_ms": duration_min * 60 * 1000,
                "label": title,
            }
        payload = {"event": "AGENDA_COMMAND", "action": "timer_cancel", "label": title}
        _attach_firmware_id(payload, firmware_id)
        return payload

    if kind == "alarm":
        if action == "cancel":
            payload = {"event": "AGENDA_COMMAND", "action": "alarm_cancel", "label": title}
            _attach_firmware_id(payload, firmware_id)
            return payload
        if action == "set_enabled":
            payload = {
                "event": "AGENDA_COMMAND",
                "action": "alarm_set_enabled",
                "label": title,
                "enabled": bool(item.get("enabled", True)),
            }
            _attach_firmware_id(payload, firmware_id)
            return payload
        if action == "recreate":
            cancel = {"event": "AGENDA_COMMAND", "action": "alarm_cancel", "label": title}
            _attach_firmware_id(cancel, firmware_id)
            create = _agenda_session_payload(item, "create")
            return [cancel, create] if create is not None else [cancel]
        hour, minute = _parse_hhmm(str(item.get("time", "07:30")))
        return {
            "event": "AGENDA_COMMAND",
            "action": "alarm_create",
            "hour": hour,
            "minute": minute,
            "weekdays_mask": int(item.get("weekdays_mask", 0)),
            "label": title,
            "enabled": bool(item.get("enabled", True)),
        }

    if kind == "reminder":
        if action == "cancel":
            payload = {"event": "AGENDA_COMMAND", "action": "reminder_cancel", "label": title}
            _attach_firmware_id(payload, firmware_id)
            return payload
        return {
            "event": "AGENDA_COMMAND",
            "action": "reminder_create",
            "delay_ms": int(item.get("duration_min", 5)) * 60 * 1000,
            "label": title,
        }
    return None


def _attach_firmware_id(payload: dict[str, Any], firmware_id: Any) -> None:
    try:
        value = int(firmware_id)
    except (TypeError, ValueError):
        return
    payload["id"] = value


def _parse_hhmm(value: str) -> tuple[int, int]:
    try:
        hour_text, minute_text = value.split(":", 1)
        hour = max(0, min(23, int(hour_text)))
        minute = max(0, min(59, int(minute_text)))
    except (ValueError, TypeError):
        return 7, 30
    return hour, minute


def _percent_to_u8(percent: int) -> int:
    clamped = max(0, min(100, percent))
    return round((clamped * 255) / 100)


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def _vision_observation_dict(observation) -> dict[str, Any]:
    return {
        "valid": observation.valid,
        "scene": observation.scene,
        "timestamp_ms": observation.timestamp_ms,
        "width": observation.width,
        "height": observation.height,
        "jpeg_bytes": observation.jpeg_bytes,
        "capture_ms": observation.capture_ms,
        "luma_avg": observation.luma_avg,
        "luma_min": observation.luma_min,
        "luma_max": observation.luma_max,
        "contrast": observation.contrast,
        "motion_score": observation.motion_score,
    }


def _vision_analysis_dict(analysis) -> dict[str, Any]:
    primary = analysis.primary_face
    return {
        "observation": _vision_observation_dict(analysis.observation),
        "detector": analysis.detector,
        "detector_available": analysis.detector_available,
        "face_detected": analysis.face_detected,
        "face_count": analysis.face_count,
        "face_center_norm_x": analysis.face_center_norm_x,
        "face_center_norm_y": analysis.face_center_norm_y,
        "primary_face": None if primary is None else {
            "x": primary.x,
            "y": primary.y,
            "width": primary.width,
            "height": primary.height,
        },
        "error": analysis.error,
    }


def _find_app_dist() -> Path | None:
    configured = os.environ.get("NOISEBOT_APP_DIST", "").strip()
    candidates = [Path(configured)] if configured else []
    candidates.append(Path(__file__).resolve().parents[4] / "app" / "dist")
    for candidate in candidates:
        if not str(candidate):
            continue
        resolved = candidate.resolve()
        if (resolved / "index.html").exists():
            return resolved
    return None
