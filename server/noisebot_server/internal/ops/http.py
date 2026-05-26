"""noisebot_server.internal.ops.http — servidor HTTP local-only async.

Endpoints operacionais:
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
        self._agenda_client = FirmwareAgendaClient.from_config(app._config)
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
        wa.router.add_get("/api/agenda/items", self._get_agenda_items)
        wa.router.add_post("/api/agenda/timers", self._post_agenda_timer)
        wa.router.add_post("/api/agenda/alarms", self._post_agenda_alarm)
        wa.router.add_post("/api/agenda/reminders", self._post_agenda_reminder)
        wa.router.add_patch("/api/agenda/items/{item_id}", self._patch_agenda_item)
        wa.router.add_delete("/api/agenda/items/{item_id}", self._delete_agenda_item)
        wa.router.add_get("/api/settings/basic", self._get_basic_settings)
        wa.router.add_put("/api/settings/basic", self._put_basic_settings)
        wa.router.add_get("/api/vision/status", self._get_vision_status)
        wa.router.add_get("/api/vision/observe", self._get_vision_observe)
        wa.router.add_get("/api/vision/analyze", self._get_vision_analyze)
        wa.router.add_get("/api/vision/snapshot", self._get_vision_snapshot)
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
        live_connected = bool(
            supervisor is not None and getattr(supervisor, "is_connected", False)
        )
        store.firmware_connected = live_connected

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
        ))

    async def _get_ai_metrics(self, request: web.Request) -> web.Response:
        return _json(self._metrics_api.get_metrics())

    async def _get_ai_errors(self, request: web.Request) -> web.Response:
        limit = int(request.rel_url.query.get("limit", "20"))
        errors = self._store.recent_errors[:limit]
        return _json({"errors": errors, "total": len(errors)})

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
