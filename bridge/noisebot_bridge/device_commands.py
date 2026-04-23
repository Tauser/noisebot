from __future__ import annotations

from dataclasses import dataclass
import logging
import struct

from .intent_router import DeviceCommand
from .protocol import MSG_ACTION, MSG_EMOT_EVENT, MSG_EXPR, MSG_GAZE, MSG_TEXT_SCROLL

log = logging.getLogger("noisebot_bridge.device_commands")


@dataclass(frozen=True)
class DeviceCommandResult:
    name: str
    supported: bool
    executed: bool
    error: str | None = None


class DeviceCommandDispatcher:
    def __init__(self, send_msg):
        self.send_msg = send_msg

    def dispatch(self, command: DeviceCommand) -> DeviceCommandResult:
        if not command.supported:
            log.info("unsupported_device_command name=%s args=%r", command.name, command.args)
            return DeviceCommandResult(command.name, supported=False, executed=False)

        try:
            if command.name == "look":
                self._look(command.args.get("direction", "center"))
            elif command.name == "set_expression":
                self._set_expression(
                    int(command.args.get("expression_id", 2)),
                    int(command.args.get("duration_ms", 4000)),
                )
            elif command.name == "play_action":
                self._play_action(int(command.args.get("action_id", 0)))
            elif command.name == "emit_emotion_event":
                self._emit_emotion_event(int(command.args.get("event_id", 2)))
            elif command.name == "scroll_text":
                self._scroll_text(str(command.args.get("text", "")))
            else:
                log.info("unsupported_device_command name=%s args=%r", command.name, command.args)
                return DeviceCommandResult(command.name, supported=False, executed=False)
        except Exception as e:
            log.warning("device_command_failed name=%s error=%s", command.name, e)
            return DeviceCommandResult(command.name, supported=True, executed=False, error=str(e))

        log.info("device_command_executed name=%s args=%r", command.name, command.args)
        return DeviceCommandResult(command.name, supported=True, executed=True)

    def _look(self, direction: str):
        gaze = {
            "esquerda": (-0.75, 0.0),
            "direita": (0.75, 0.0),
            "cima": (0.0, -0.55),
            "baixo": (0.0, 0.55),
            "centro": (0.0, 0.0),
            "center": (0.0, 0.0),
        }
        x, y = gaze.get(direction, (0.0, 0.0))
        self.send_msg(MSG_GAZE, struct.pack("<ff", x, y))

    def _set_expression(self, expression_id: int, duration_ms: int):
        expression_id = max(0, min(255, expression_id))
        duration_ms = max(0, duration_ms)
        self.send_msg(MSG_EXPR, struct.pack("<BI", expression_id, duration_ms))

    def _play_action(self, action_id: int):
        self.send_msg(MSG_ACTION, struct.pack("<I", max(0, action_id)))

    def _emit_emotion_event(self, event_id: int):
        self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", max(0, event_id)))

    def _scroll_text(self, text: str):
        payload = text.encode("utf-8")[:160]
        if payload:
            self.send_msg(MSG_TEXT_SCROLL, payload)
