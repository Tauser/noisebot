"""Robot output commands emitted by the server orchestrator."""

from __future__ import annotations

import logging
from typing import Any

from .runtime import EventBus, IntentResolved, RobotCommand

log = logging.getLogger(__name__)

_GAZE_MAP: dict[str, tuple[float, float]] = {
    "local_look_left": (-0.75, 0.0),
    "local_look_right": (0.75, 0.0),
    "local_look_up": (0.0, -0.55),
    "local_look_down": (0.0, 0.55),
}


class RobotOutputProvider:
    """Translate decisions into firmware commands."""

    def __init__(self, bus: EventBus) -> None:
        self._bus = bus

    async def emit_for_intent(
        self,
        intent: IntentResolved,
        adapter: Any,
        include_reply_text: bool = True,
    ) -> None:
        if not intent.has_intent:
            return

        if intent.expression_id is not None:
            await self._emit(
                adapter,
                "expr",
                {"expression_id": intent.expression_id, "duration_ms": 3000},
                intent.turn_id,
            )

        if intent.emot_event_id is not None:
            await self._emit(
                adapter,
                "emot",
                {"event_id": intent.emot_event_id},
                intent.turn_id,
            )

        if intent.action_id is not None and intent.action_id != 0:
            await self._emit(
                adapter,
                "action",
                {"action_id": intent.action_id},
                intent.turn_id,
            )

        gaze = _GAZE_MAP.get(intent.intent_name or "")
        if gaze:
            await self._emit(
                adapter,
                "gaze",
                {"x": gaze[0], "y": gaze[1]},
                intent.turn_id,
            )

        if intent.device_command:
            if intent.device_command.get("event") == "VOLUME_COMMAND":
                await self._emit(
                    adapter,
                    "volume",
                    {"percent": intent.device_command.get("percent", 50)},
                    intent.turn_id,
                )
            else:
                await self._emit(adapter, "session", intent.device_command, intent.turn_id)

        if include_reply_text and intent.reply_text:
            await self._emit(
                adapter,
                "text",
                {"text": intent.reply_text[:128]},
                intent.turn_id,
            )

    async def reset_baseline(self, adapter: Any, turn_id: int = 0) -> None:
        await self._emit(
            adapter,
            "expr",
            {"expression_id": 2, "duration_ms": 500},
            turn_id,
        )
        await self._emit(adapter, "gaze", {"x": 0.0, "y": 0.0}, turn_id)

    async def _emit(
        self,
        adapter: Any,
        kind: str,
        payload: dict[str, Any],
        turn_id: int,
    ) -> None:
        cmd = RobotCommand(kind=kind, payload=payload, turn_id=turn_id)
        await self._bus.publish(cmd)

        if adapter is None:
            return

        try:
            if kind == "expr":
                await adapter.send_expr(
                    payload["expression_id"],
                    payload.get("duration_ms", 2000),
                )
            elif kind == "action":
                await adapter.send_action(payload["action_id"])
            elif kind == "emot":
                await adapter.send_emot_event(payload["event_id"])
            elif kind == "gaze":
                await adapter.send_gaze(payload["x"], payload["y"])
            elif kind == "text":
                await adapter.send_text_scroll(payload["text"])
            elif kind == "volume":
                await adapter.send_volume(payload.get("percent", 50))
            elif kind == "session":
                await adapter.send_session(payload)
        except Exception:
            log.exception("RobotOutputProvider: erro ao enviar %s turn_id=%d", kind, turn_id)


__all__ = ["RobotOutputProvider"]
