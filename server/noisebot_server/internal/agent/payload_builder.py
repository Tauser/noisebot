"""Multi-turn context payload builder for LLM prompts.

Replaces the ad-hoc context dict construction in the orchestrator.
All float telemetry (VAA) is translated to natural language via mood.py.
Empty optional sections are omitted to keep the payload compact.
"""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from .mood import describe_mood


def build_turn_payload(
    *,
    text: str,
    turn_id: int,
    last_status: dict[str, Any] | None = None,
    user_profile: dict[str, Any] | None = None,
    recent_user_texts: list[str] | None = None,
    recent_robot_replies: list[str] | None = None,
    pipeline_mode: str = "normal",
    firmware_online: bool = False,
    tts_available: bool = False,
    vision_available: bool = False,
    servos_enabled: bool = False,
    allowed_tools: list[str] | None = None,
    vision_snapshot: dict[str, Any] | None = None,
    memory_facts: list[dict[str, Any]] | None = None,
    presence_state: str | None = None,
    companionship_s: int = 0,
) -> dict[str, Any]:
    """Build a compact, structured context payload for the LLM.

    - VAA floats are translated to natural language (never exposed raw).
    - Absent/empty optional fields are omitted entirely.
    - Conversation history is bounded to the last 3 entries each.
    - Tools list contains only names of tools allowed in the current state.
    - vision_snapshot: lightweight dict from get_lightweight_snapshot();
      bytes values are stripped to guarantee no raw frames leak.
    """
    status = last_status or {}

    mood = describe_mood(
        valence=_safe_float(status.get("valence")),
        activation=_safe_float(status.get("activation")),
        attention=_safe_float(status.get("attention")),
        trust=_safe_float(status.get("trust")),
    )

    turn: dict[str, Any] = {
        "id": turn_id,
        "user_text": text[:300],
        "timestamp_iso": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }

    user: dict[str, str] = {}
    if isinstance(user_profile, dict):
        for key in (
            "display_name", "relationship", "language",
            "robot_nickname", "persona_mode", "interaction_style",
        ):
            val = user_profile.get(key, "")
            if val:
                user[key] = str(val)[:48]

    robot_state = str(status.get("state", "")) or "unknown"
    robot: dict[str, Any] = {
        "state": robot_state,
        "firmware_online": firmware_online,
        "pipeline_mode": pipeline_mode,
    }

    hardware: dict[str, bool] = {}
    if vision_available:
        hardware["vision_available"] = True
    if tts_available:
        hardware["tts_available"] = True
    if servos_enabled:
        hardware["servos_enabled"] = True

    conversation: dict[str, Any] = {}
    if recent_user_texts:
        conversation["recent_user"] = [str(t)[:200] for t in recent_user_texts[-3:]]
    if recent_robot_replies:
        conversation["recent_robot"] = [str(r)[:200] for r in recent_robot_replies[-3:]]

    tools = list(allowed_tools) if allowed_tools else []

    payload: dict[str, Any] = {
        "turn": turn,
        "robot": robot,
        "mood": mood,
    }
    if user:
        payload["user"] = user
    if hardware:
        payload["hardware"] = hardware
    if conversation:
        payload["conversation"] = conversation
    if tools:
        payload["tools"] = tools

    if isinstance(vision_snapshot, dict):
        payload["vision"] = {
            k: v for k, v in vision_snapshot.items()
            if not isinstance(v, (bytes, bytearray))
        }

    if memory_facts:
        payload["memory"] = [
            {"id": f.get("id", ""), "text": str(f.get("text", ""))[:120]}
            for f in memory_facts[:10]  # max 10 fatos no contexto
        ]

    presence_ctx = _build_presence_context(presence_state, companionship_s)
    if presence_ctx:
        payload["presence"] = presence_ctx

    return payload


def _build_presence_context(state: str | None, companionship_s: int) -> dict[str, str] | None:
    """Constrói bloco de contexto de presença para o LLM.

    Discreto: descreve o que o robô percebe, nunca gera fala espontânea.
    Fonte: espelho do estado do firmware (não é fonte autoritativa).
    """
    if not state or state in ("NO_ONE", "ALONE_SETTLED"):
        return None

    if state in ("PRESENT", "MAYBE_SOMEONE"):
        ctx = "O usuário parece estar fisicamente presente diante do robô."
    elif state == "ENGAGED":
        mins = max(1, companionship_s // 60)
        ctx = f"O usuário está em companhia silenciosa há aproximadamente {mins} minuto(s)."
    elif state == "LEFT_RECENTLY":
        ctx = "O usuário estava presente há pouco e pode ter se afastado momentaneamente."
    elif state == "AWAY":
        ctx = "O usuário se afastou há algum tempo; não há presença visual confirmada."
    else:
        return None

    return {"note": ctx, "state": state, "companionship_s": str(companionship_s)}


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


__all__ = ["build_turn_payload"]
