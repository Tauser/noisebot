"""Executor functions for Phase 4 tools.

Each executor receives (arguments, context) and returns a result dict.
The gateway (Phase 5) is responsible for:
  - calling the right executor
  - checking risk level and state policy
  - building ToolResult with audit log
  - deciding whether to execute, simulate, or block

Expected context keys:
  adapter       FirmwareAdapter | None — active firmware connection
  app_state     AppStateStore | None   — persistent app state
  vision_client VisionClient | None    — camera/vision access
  turn_id       int                    — current turn for logging

Executor contract:
  - Returns a dict on success (may be partial).
  - Raises nothing — errors are returned as {"error": "..."}.
  - Never accesses HAL directly. Only goes through adapter or app_state.
"""
from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timezone
from typing import Any

log = logging.getLogger(__name__)

_EXPRESSION_ID_MAP: dict[str, int] = {
    "neutral": 0, "happy": 1, "curious": 2, "sleepy": 3, "focused": 4,
    "suspicious": 5, "surprised": 6, "sad": 7, "alarmed": 8, "angry": 9,
}


def execute_set_expression(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    expression_id = str(arguments["expression_id"]).lower()
    adapter = context.get("adapter")
    turn_id = context.get("turn_id", 0)
    log.info("tool set_expression turn_id=%d expression=%s", turn_id, expression_id)

    expr_int = _EXPRESSION_ID_MAP.get(expression_id)
    if expr_int is None:
        return {"error": f"expressao desconhecida: '{expression_id}'"}

    if adapter is None:
        return {"expression_id": expression_id, "expression_int": expr_int, "sent": False}

    try:
        asyncio.get_running_loop().create_task(
            adapter.send_expr(expr_int),
            name=f"nb_tool_expr_{turn_id}",
        )
        return {"expression_id": expression_id, "expression_int": expr_int, "sent": True}
    except RuntimeError:
        return {"expression_id": expression_id, "expression_int": expr_int, "sent": False}


def execute_set_led(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    color = arguments["color"]
    brightness = int(arguments.get("brightness", 100))
    adapter = context.get("adapter")
    turn_id = context.get("turn_id", 0)
    log.info(
        "tool set_led turn_id=%d color=%s brightness=%d", turn_id, color, brightness
    )
    # send_led requires MSG_LED firmware support (not yet released)
    return {"color": color, "brightness": brightness, "sent": False, "note": "MSG_LED pendente"}


def execute_create_timer(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    duration_s = int(arguments["duration_s"])
    label = str(arguments.get("label", "Timer"))
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info(
        "tool create_timer turn_id=%d duration_s=%d label=%r", turn_id, duration_s, label
    )
    if app_state is not None:
        try:
            duration_min = max(1, round(duration_s / 60))
            item = app_state.create_agenda_item(
                "timer",
                {"title": label, "duration_min": duration_min},
            )
            return {
                "timer_id": item.get("id"),
                "duration_s": duration_s,
                "label": label,
            }
        except Exception as exc:
            log.warning("create_timer: app_state falhou: %s", exc)
            return {"error": str(exc)}
    return {"duration_s": duration_s, "label": label, "persisted": False}


def execute_create_reminder(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    text = str(arguments["text"])
    trigger_iso = str(arguments["trigger_iso"])
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info(
        "tool create_reminder turn_id=%d trigger=%s text=%r", turn_id, trigger_iso, text
    )
    if app_state is not None:
        try:
            delay_min = _iso_to_delay_min(trigger_iso)
            item = app_state.create_agenda_item(
                "reminder",
                {"title": text, "duration_min": delay_min},
            )
            return {
                "reminder_id": item.get("id"),
                "text": text,
                "trigger_iso": trigger_iso,
            }
        except Exception as exc:
            log.warning("create_reminder: app_state falhou: %s", exc)
            return {"error": str(exc)}
    return {"text": text, "trigger_iso": trigger_iso, "persisted": False}


def execute_show_message(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    text = str(arguments["text"])[:80]
    adapter = context.get("adapter")
    turn_id = context.get("turn_id", 0)
    log.info("tool show_message turn_id=%d len=%d", turn_id, len(text))

    if adapter is None:
        return {"text": text, "sent": False}

    try:
        asyncio.get_running_loop().create_task(
            adapter.send_text_scroll(text),
            name=f"nb_tool_msg_{turn_id}",
        )
        return {"text": text, "sent": True}
    except RuntimeError:
        return {"text": text, "sent": False}


def execute_list_agenda(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info("tool list_agenda turn_id=%d", turn_id)

    if app_state is None:
        return {"items": [], "summary": "agenda indisponível"}

    try:
        data = app_state.list_agenda()
        items = data.get("items", [])
        compact = [
            {k: v for k, v in item.items() if k in ("id", "kind", "title", "status", "due_at")}
            for item in items
        ]
        return {"items": compact, "count": len(compact), "summary": data.get("summary", "")}
    except Exception as exc:
        log.warning("list_agenda falhou: %s", exc)
        return {"error": str(exc)}


def execute_analyze_vision(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    import os

    from ...vision.analysis import describe_with_ollama_vision, describe_with_vision_api

    vision_client = context.get("vision_client")
    turn_id = context.get("turn_id", 0)
    log.info("tool analyze_vision turn_id=%d", turn_id)
    if vision_client is None:
        return {"error": "visao indisponivel"}
    try:
        from ...vision.analysis import analyze_jpeg
        observation = vision_client.observe()
        jpeg = vision_client.snapshot()
        vision_client.session_close()
        analysis = analyze_jpeg(jpeg, observation)
        obs = analysis.observation
        brightness = _luma_to_text(obs.luma_avg)
        motion = _motion_to_text(obs.motion_score)

        result: dict[str, Any] = {
            "scene": obs.scene,
            "face_detected": analysis.face_detected,
            "face_count": analysis.face_count,
            "brightness": brightness,
            "motion": motion,
        }

        description = None
        if os.environ.get("NOISEBOT_LLM_PROVIDER", "ollama").strip().lower() == "ollama":
            description = describe_with_ollama_vision(
                jpeg,
                base_url=os.environ.get("NOISEBOT_OLLAMA_BASE_URL", ""),
                model=os.environ.get("NOISEBOT_LLM_MODEL", "gemma4:12b"),
            )
        if not description:
            description = describe_with_vision_api(
                jpeg,
                api_key=os.environ.get("OPENAI_API_KEY", ""),
            )
        if description:
            result["description"] = description
            log.info("analyze_vision turn_id=%d description=%r", turn_id, description[:80])

        return result
    except Exception as exc:
        log.warning("analyze_vision falhou: %s", exc)
        return {"error": str(exc)}


def execute_remember_fact(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    text = str(arguments["text"])
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info("tool remember_fact turn_id=%d len=%d", turn_id, len(text))

    if app_state is None:
        return {"text": text, "persisted": False}

    try:
        fact = app_state.add_fact(text)
        return {"fact_id": fact["id"], "text": fact["text"], "persisted": True}
    except Exception as exc:
        log.warning("remember_fact falhou: %s", exc)
        return {"error": str(exc)}


def execute_forget_fact(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    fact_id = str(arguments["fact_id"])
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info("tool forget_fact turn_id=%d fact_id=%s", turn_id, fact_id)

    if app_state is None:
        return {"fact_id": fact_id, "removed": False}

    try:
        removed = app_state.remove_fact(fact_id)
        return {"fact_id": fact_id, "removed": removed}
    except Exception as exc:
        log.warning("forget_fact falhou: %s", exc)
        return {"error": str(exc)}


def execute_recall_user_preferences(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    app_state = context.get("app_state")
    turn_id = context.get("turn_id", 0)
    log.info("tool recall_user_preferences turn_id=%d", turn_id)

    if app_state is None:
        return {"facts": [], "count": 0}

    try:
        facts = app_state.list_facts()
        return {"facts": facts, "count": len(facts)}
    except Exception as exc:
        log.warning("recall_user_preferences falhou: %s", exc)
        return {"error": str(exc)}


def execute_request_confirmation(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    question = str(arguments["question"])
    action_description = str(arguments.get("action_description", ""))
    turn_id = context.get("turn_id", 0)
    log.info(
        "tool request_confirmation turn_id=%d question=%r", turn_id, question
    )
    # The gateway handles the confirmation flow — this executor just records intent.
    return {
        "question": question,
        "action_description": action_description,
        "pending": True,
    }


def execute_web_search(
    arguments: dict[str, Any],
    context: dict[str, Any],
) -> dict[str, Any]:
    from ..web_search import fetch_web_search, format_search_results_for_llm

    query = str(arguments["query"])
    max_results = int(arguments.get("max_results", 4))
    mode = str(arguments.get("mode", "auto"))
    turn_id = context.get("turn_id", 0)
    log.info(
        "tool web_search turn_id=%d mode=%s query=%r max=%d",
        turn_id, mode, query, max_results,
    )

    resp = fetch_web_search(query, max_results=max_results, mode=mode)
    summary = format_search_results_for_llm(resp)
    if resp.error and not resp.results:
        return {
            "query": query, "results": [], "error": resp.error,
            "source": resp.source, "provider": resp.provider,
            "mode": resp.mode, "summary": summary,
        }

    return {
        "query": resp.query,
        "source": resp.source,
        "provider": resp.provider,
        "mode": resp.mode,
        "cached": resp.cached,
        "answer": resp.answer,
        "results": [
            {
                "title": h.title, "snippet": h.snippet, "url": h.url,
                "published": h.published, "source": h.source, "score": h.score,
            }
            for h in resp.results
        ],
        "summary": summary,
    }


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _iso_to_delay_min(trigger_iso: str) -> int:
    """Convert an ISO 8601 datetime string to minutes from now (minimum 1)."""
    try:
        trigger = datetime.fromisoformat(trigger_iso)
        if trigger.tzinfo is None:
            trigger = trigger.replace(tzinfo=timezone.utc)
        now = datetime.now(timezone.utc)
        delta_s = (trigger - now).total_seconds()
        return max(1, round(delta_s / 60))
    except (ValueError, TypeError):
        return 1


def _luma_to_text(luma: int) -> str:
    if luma < 60:
        return "baixa"
    if luma < 150:
        return "média"
    return "alta"


def _motion_to_text(motion_score: int) -> str:
    if motion_score < 10:
        return "baixo"
    if motion_score < 50:
        return "moderado"
    return "alto"


EXECUTOR_MAP: dict[str, Any] = {
    "set_expression": execute_set_expression,
    "set_led": execute_set_led,
    "show_message": execute_show_message,
    "list_agenda": execute_list_agenda,
    "create_timer": execute_create_timer,
    "create_reminder": execute_create_reminder,
    "analyze_vision": execute_analyze_vision,
    "remember_fact": execute_remember_fact,
    "forget_fact": execute_forget_fact,
    "recall_user_preferences": execute_recall_user_preferences,
    "request_confirmation": execute_request_confirmation,
    "web_search": execute_web_search,
}

__all__ = ["EXECUTOR_MAP"]
