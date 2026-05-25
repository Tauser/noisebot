from __future__ import annotations

from dataclasses import dataclass
import logging
import struct

from .intent_router import DeviceCommand
from .protocol import (
    MSG_ACTION,
    MSG_EMOT_EVENT,
    MSG_EXPR,
    MSG_GAZE,
    MSG_TEXT_SCROLL,
    MSG_VOLUME,
)
from .tools import validate_tool_call

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
            log.info("tool_rejected name=%s reason=unsupported_intent_command args=%r", command.name, command.args)
            return DeviceCommandResult(command.name, supported=False, executed=False)

        validation = validate_tool_call(command.name, command.args)
        if not validation.ok:
            log.info(
                "tool_rejected name=%s reason=%s args=%r",
                validation.tool_name,
                validation.reason,
                command.args,
            )
            return DeviceCommandResult(
                validation.tool_name,
                supported=validation.reason != "unknown_tool",
                executed=False,
                error=validation.reason,
            )

        tool_name = validation.tool_name
        command_name = validation.command_name
        args = validation.args
        log.info("tool_call name=%s args=%r", tool_name, args)

        try:
            if command_name == "look":
                self._look(str(args["direction"]))
            elif command_name == "set_expression":
                self._set_expression(
                    int(args["expression_id"]),
                    int(args.get("duration_ms", 4000)),
                )
            elif command_name == "play_action":
                self._play_action(int(args["action_id"]))
            elif command_name == "emit_emotion_event":
                self._emit_emotion_event(int(args["event_id"]))
            elif command_name == "scroll_text":
                self._scroll_text(str(args["text"]))
            elif command_name == "set_volume":
                self._set_volume(int(args["percent"]))
            else:
                log.info("tool_rejected name=%s reason=no_firmware_command args=%r", tool_name, args)
                return DeviceCommandResult(tool_name, supported=True, executed=False, error="no_firmware_command")
        except Exception as e:
            log.warning("tool_result name=%s status=error error=%s", tool_name, e)
            return DeviceCommandResult(tool_name, supported=True, executed=False, error=str(e))

        log.info("tool_result name=%s status=ok", tool_name)
        return DeviceCommandResult(tool_name, supported=True, executed=True)

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
        self.send_msg(MSG_EXPR, struct.pack("<BI", expression_id, duration_ms))

    def _play_action(self, action_id: int):
        self.send_msg(MSG_ACTION, struct.pack("<I", action_id))

    def _emit_emotion_event(self, event_id: int):
        self.send_msg(MSG_EMOT_EVENT, struct.pack("<I", event_id))

    def _scroll_text(self, text: str):
        raw = text.encode("utf-8")[:128]
        payload = raw.decode("utf-8", "ignore").encode("utf-8")
        if payload:
            self.send_msg(MSG_TEXT_SCROLL, payload)

    def _set_volume(self, percent: int):
        self.send_msg(MSG_VOLUME, struct.pack("<B", percent))
        self._scroll_text(f"Volume {percent}%")
