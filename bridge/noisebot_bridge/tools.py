from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True)
class ToolArgSpec:
    kind: type
    required: bool = True
    enum: tuple[Any, ...] = ()
    minimum: int | None = None
    maximum: int | None = None
    max_len: int | None = None


@dataclass(frozen=True)
class ToolSpec:
    name: str
    command_name: str | None
    description: str
    args: dict[str, ToolArgSpec] = field(default_factory=dict)
    local_only: bool = True
    requires_motion_safety: bool = False
    supported: bool = True


@dataclass(frozen=True)
class ToolValidationResult:
    ok: bool
    tool_name: str
    command_name: str | None = None
    args: dict[str, Any] = field(default_factory=dict)
    reason: str | None = None
    spec: ToolSpec | None = None


@dataclass(frozen=True)
class ToolExecutionResult:
    tool_name: str
    ok: bool
    payload: dict[str, Any] = field(default_factory=dict)
    error: str | None = None


TOOL_ALIASES = {
    "look": "noisebot.robot.set_gaze",
    "set_expression": "noisebot.robot.set_expression",
    "play_action": "noisebot.robot.play_action",
    "emit_emotion_event": "noisebot.robot.emit_emotion_event",
    "scroll_text": "noisebot.robot.show_text",
    "set_led_color": "noisebot.robot.set_led_mood",
}


TOOL_CATALOG = {
    "noisebot.robot.get_status": ToolSpec(
        name="noisebot.robot.get_status",
        command_name=None,
        description="Consulta o estado local conhecido pelo bridge.",
    ),
    "noisebot.robot.set_gaze": ToolSpec(
        name="noisebot.robot.set_gaze",
        command_name="look",
        description="Move apenas o olhar/render gaze para uma direcao permitida.",
        args={
            "direction": ToolArgSpec(
                str,
                enum=("esquerda", "direita", "cima", "baixo", "centro", "center"),
            ),
        },
    ),
    "noisebot.robot.set_expression": ToolSpec(
        name="noisebot.robot.set_expression",
        command_name="set_expression",
        description="Aplica uma expressao por tempo limitado.",
        args={
            "expression_id": ToolArgSpec(int, minimum=0, maximum=7),
            "duration_ms": ToolArgSpec(int, required=False, minimum=0, maximum=10000),
        },
    ),
    "noisebot.robot.play_action": ToolSpec(
        name="noisebot.robot.play_action",
        command_name="play_action",
        description="Executa uma acao ja conhecida pelo firmware.",
        args={"action_id": ToolArgSpec(int, minimum=0, maximum=10)},
        requires_motion_safety=True,
    ),
    "noisebot.robot.emit_emotion_event": ToolSpec(
        name="noisebot.robot.emit_emotion_event",
        command_name="emit_emotion_event",
        description="Publica um evento emocional permitido no firmware.",
        args={"event_id": ToolArgSpec(int, minimum=0, maximum=15)},
    ),
    "noisebot.robot.show_text": ToolSpec(
        name="noisebot.robot.show_text",
        command_name="scroll_text",
        description="Exibe texto curto no display do robo.",
        args={"text": ToolArgSpec(str, max_len=160)},
    ),
    "noisebot.robot.set_led_mood": ToolSpec(
        name="noisebot.robot.set_led_mood",
        command_name=None,
        description="Futura tool para humor visual por LED.",
        args={"mood": ToolArgSpec(str, required=False, max_len=24)},
        supported=False,
    ),
    "noisebot.robot.create_reminder": ToolSpec(
        name="noisebot.robot.create_reminder",
        command_name=None,
        description="Cria um lembrete local no bridge.",
        args={
            "text": ToolArgSpec(str, max_len=120),
            "due_at_epoch": ToolArgSpec(int, minimum=0, maximum=4102444800),
        },
    ),
    "noisebot.robot.stop_reminder": ToolSpec(
        name="noisebot.robot.stop_reminder",
        command_name=None,
        description="Cancela um lembrete local pelo id.",
        args={"reminder_id": ToolArgSpec(int, minimum=1, maximum=100000)},
    ),
    "noisebot.robot.get_reminders": ToolSpec(
        name="noisebot.robot.get_reminders",
        command_name=None,
        description="Lista lembretes locais ativos.",
    ),
}


def canonical_tool_name(name: str) -> str:
    return TOOL_ALIASES.get(name, name)


def validate_tool_call(name: str, args: dict[str, Any] | None) -> ToolValidationResult:
    tool_name = canonical_tool_name(name)
    spec = TOOL_CATALOG.get(tool_name)
    if spec is None:
        return ToolValidationResult(False, tool_name, reason="unknown_tool")
    if not spec.supported:
        return ToolValidationResult(False, tool_name, spec=spec, reason="unsupported_tool")

    args = args or {}
    normalized: dict[str, Any] = {}
    for arg_name, arg_spec in spec.args.items():
        if arg_name not in args:
            if arg_spec.required:
                return ToolValidationResult(False, tool_name, spec=spec, reason=f"missing_arg:{arg_name}")
            continue

        value = args[arg_name]
        if arg_spec.kind is int:
            if isinstance(value, bool):
                return ToolValidationResult(False, tool_name, spec=spec, reason=f"invalid_type:{arg_name}")
            try:
                value = int(value)
            except (TypeError, ValueError):
                return ToolValidationResult(False, tool_name, spec=spec, reason=f"invalid_type:{arg_name}")
        elif not isinstance(value, arg_spec.kind):
            return ToolValidationResult(False, tool_name, spec=spec, reason=f"invalid_type:{arg_name}")

        if arg_spec.enum and value not in arg_spec.enum:
            return ToolValidationResult(False, tool_name, spec=spec, reason=f"invalid_enum:{arg_name}")
        if arg_spec.minimum is not None and value < arg_spec.minimum:
            return ToolValidationResult(False, tool_name, spec=spec, reason=f"below_min:{arg_name}")
        if arg_spec.maximum is not None and value > arg_spec.maximum:
            return ToolValidationResult(False, tool_name, spec=spec, reason=f"above_max:{arg_name}")
        if arg_spec.max_len is not None and len(value) > arg_spec.max_len:
            return ToolValidationResult(False, tool_name, spec=spec, reason=f"too_long:{arg_name}")
        normalized[arg_name] = value

    extra_args = sorted(set(args) - set(spec.args))
    if extra_args:
        return ToolValidationResult(False, tool_name, spec=spec, reason=f"unknown_arg:{extra_args[0]}")

    return ToolValidationResult(True, tool_name, spec.command_name, normalized, spec=spec)


class RobotToolRuntime:
    def __init__(self):
        self._reminders: dict[int, dict[str, Any]] = {}
        self._next_reminder_id = 1
        self.status: dict[str, Any] = {}

    def update_status(self, status: dict[str, Any] | None):
        self.status = dict(status or {})

    def execute(self, name: str, args: dict[str, Any] | None = None) -> ToolExecutionResult:
        validation = validate_tool_call(name, args)
        if not validation.ok:
            return ToolExecutionResult(validation.tool_name, ok=False, error=validation.reason)
        if validation.command_name is not None:
            return ToolExecutionResult(validation.tool_name, ok=False, error="firmware_tool")

        if validation.tool_name == "noisebot.robot.get_status":
            return ToolExecutionResult(validation.tool_name, ok=True, payload={"status": dict(self.status)})
        if validation.tool_name == "noisebot.robot.create_reminder":
            reminder_id = self._next_reminder_id
            self._next_reminder_id += 1
            reminder = {
                "id": reminder_id,
                "text": validation.args["text"],
                "due_at_epoch": validation.args["due_at_epoch"],
            }
            self._reminders[reminder_id] = reminder
            return ToolExecutionResult(validation.tool_name, ok=True, payload={"reminder": dict(reminder)})
        if validation.tool_name == "noisebot.robot.stop_reminder":
            reminder_id = validation.args["reminder_id"]
            removed = self._reminders.pop(reminder_id, None)
            if removed is None:
                return ToolExecutionResult(validation.tool_name, ok=False, error="reminder_not_found")
            return ToolExecutionResult(validation.tool_name, ok=True, payload={"reminder": dict(removed)})
        if validation.tool_name == "noisebot.robot.get_reminders":
            reminders = [dict(item) for item in sorted(self._reminders.values(), key=lambda item: item["id"])]
            return ToolExecutionResult(validation.tool_name, ok=True, payload={"reminders": reminders})

        return ToolExecutionResult(validation.tool_name, ok=False, error="local_tool_not_implemented")
