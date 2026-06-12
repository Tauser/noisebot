"""noisebot_server.internal.ops.schemas — schemas JSON da API de operação."""
from __future__ import annotations

from datetime import datetime, timezone


def _now_iso() -> str:
    return datetime.now(tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def health_response(healthy: bool, uptime_s: float) -> dict:
    return {
        "status": "ok" if healthy else "degraded",
        "uptime_s": round(uptime_s, 1),
        "updated_at": _now_iso(),
    }


def ai_status_response(
    *,
    connected: bool,
    pipeline: str,
    mode: str,
    provider: str,
    model: str,
    api_key_configured: bool,
    stt_status: str,
    llm_status: str,
    tts_status: str,
    last_error: dict | None,
    last_turn_id: int,
    last_outcome: str,
    last_transcript: str,
    last_reply: str,
    last_route: str,
    firmware_capabilities: dict | None = None,
    playback_config: dict | None = None,
    social_presence: dict | None = None,
) -> dict:
    caps = firmware_capabilities if isinstance(firmware_capabilities, dict) else {}
    scheduler = playback_config if isinstance(playback_config, dict) else {}
    audio = caps.get("audio", {})
    codecs = caps.get("codecs", {})
    codec_options = caps.get("codec_options", {})
    features = caps.get("features", [])
    return {
        "connected": connected,
        "pipeline": pipeline,
        "mode": mode,
        "provider": provider,
        "model": model,
        "api_key_configured": api_key_configured,
        "stt_status": stt_status,
        "llm_status": llm_status,
        "tts_status": tts_status,
        "last_error": last_error,
        "last_turn_id": last_turn_id,
        "last_outcome": last_outcome,
        "last_transcript": last_transcript,
        "last_reply": last_reply,
        "last_route": last_route,
        "audio": audio if isinstance(audio, dict) else {},
        "codecs": codecs if isinstance(codecs, dict) else {},
        "codec_options": codec_options if isinstance(codec_options, dict) else {},
        "features": features if isinstance(features, list) else [],
        "tts_output_scheduler": scheduler,
        "firmware": {
            "audio": audio if isinstance(audio, dict) else {},
            "codecs": codecs if isinstance(codecs, dict) else {},
            "codec_options": codec_options if isinstance(codec_options, dict) else {},
            "features": features if isinstance(features, list) else [],
        },
        "social_presence": social_presence if isinstance(social_presence, dict) else None,
        "updated_at": _now_iso(),
    }


def error_response(message: str, code: int = 400) -> dict:
    return {"error": message, "code": code}


def ok_response(message: str = "ok", **extra) -> dict:
    return {"status": "ok", "message": message, **extra}
