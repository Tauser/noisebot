from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
import re
import unicodedata


@dataclass(frozen=True)
class DeviceCommand:
    name: str
    args: dict = field(default_factory=dict)
    supported: bool = False


@dataclass(frozen=True)
class LocalIntentResult:
    intent: str
    confidence: float
    reply: str
    expression_id: int = 2
    action: int = 0
    emot_event: int = 2
    speak_reply: bool = True
    device_commands: tuple[DeviceCommand, ...] = ()


def normalize_text(text: str) -> str:
    normalized = unicodedata.normalize("NFD", text or "")
    without_accents = "".join(ch for ch in normalized if unicodedata.category(ch) != "Mn")
    lowered = without_accents.lower()
    lowered = re.sub(r"[^a-z0-9% ]+", " ", lowered)
    lowered = re.sub(r"\s+", " ", lowered).strip()
    return lowered


def _has_any(text: str, terms: tuple[str, ...]) -> bool:
    return any(term in text for term in terms)


def _time_reply(now: datetime) -> str:
    hour = now.hour
    minute = now.minute
    if hour == 0 and minute == 0:
        return "Agora é meia-noite."
    verb = "é" if hour == 1 else "são"
    hour_unit = "hora" if hour == 1 else "horas"
    if minute == 0:
        return f"Agora {verb} {hour} {hour_unit}."
    minute_unit = "minuto" if minute == 1 else "minutos"
    return f"Agora {verb} {hour} {hour_unit} e {minute:02d} {minute_unit}."


class LocalIntentRouter:
    def route(self, text: str, status: dict | None = None, now: datetime | None = None) -> LocalIntentResult | None:
        status = status or {}
        now = now or datetime.now()
        norm = normalize_text(text)
        if not norm:
            return None

        if self._is_time(norm):
            return LocalIntentResult(
                intent="local_time",
                confidence=0.92,
                reply=_time_reply(now),
                expression_id=2,
                action=0,
                emot_event=2,
                speak_reply=False,
            )

        if self._is_bridge_test(norm):
            return LocalIntentResult(
                intent="local_bridge_test",
                confidence=0.90,
                reply="Estou te ouvindo pelo bridge.",
                expression_id=1,
                action=1,
                emot_event=2,
            )

        if self._is_status(norm):
            health = status.get("health")
            attention = status.get("attention")
            details = []
            if health is not None:
                details.append(f"saúde {int(health)} de 100")
            if attention is not None:
                details.append(f"atenção {float(attention):.2f}")
            suffix = f" Tenho {', '.join(details)}." if details else ""
            return LocalIntentResult(
                intent="local_status",
                confidence=0.88,
                reply=f"Estou operacional e atento.{suffix}",
                expression_id=1,
                action=1,
                emot_event=2,
            )

        if self._is_network(norm):
            return LocalIntentResult(
                intent="local_network_status",
                confidence=0.84,
                reply="Estou conectado ao bridge. Ainda não recebo o IP do robô no status.",
                expression_id=2,
                action=0,
                emot_event=2,
            )

        expression = self._expression_command(norm)
        if expression is not None:
            return expression

        action = self._action_command(norm)
        if action is not None:
            return action

        movement = self._movement_command(norm)
        if movement is not None:
            return movement

        light = self._light_command(norm)
        if light is not None:
            return light

        volume = self._volume_command(norm, status)
        if volume is not None:
            return volume

        sleep = self._sleep_command(norm)
        if sleep is not None:
            return sleep

        return None

    @staticmethod
    def _is_time(text: str) -> bool:
        if "hora" in text and _has_any(text, ("que", "qual", "agora", "atual", "sao", "e ")):
            return True
        if "horas sao" in text or "ora sao" in text or "e horas sao" in text:
            return True
        return False

    @staticmethod
    def _is_bridge_test(text: str) -> bool:
        return _has_any(
            text,
            (
                "teste o bridge",
                "testar bridge",
                "voce esta me ouvindo",
                "esta me ouvindo",
                "diga que esta ouvindo",
            ),
        )

    @staticmethod
    def _is_status(text: str) -> bool:
        return _has_any(text, ("qual seu status", "como voce esta", "voce esta bem", "esta tudo bem", "status"))

    @staticmethod
    def _is_network(text: str) -> bool:
        return _has_any(text, ("qual seu ip", "voce esta conectado", "esta conectado", "conexao", "rede"))

    @staticmethod
    def _expression_command(text: str) -> LocalIntentResult | None:
        if not _has_any(text, ("fique", "fica", "expressao", "rosto", "cara")):
            return None
        expressions = {
            "feliz": (1, "feliz"),
            "curioso": (2, "curioso"),
            "curiosa": (2, "curioso"),
            "sonolento": (3, "sonolento"),
            "sonolenta": (3, "sonolento"),
            "focado": (4, "focado"),
            "focada": (4, "focado"),
            "surpreso": (6, "surpreso"),
            "triste": (7, "triste"),
        }
        for key, (expression_id, label) in expressions.items():
            if key in text:
                return LocalIntentResult(
                    intent="local_device_expression",
                    confidence=0.84,
                    reply=f"Pronto, expressão {label}.",
                    expression_id=expression_id,
                    action=0,
                    emot_event=2,
                    speak_reply=False,
                    device_commands=(
                        DeviceCommand(
                            "set_expression",
                            {"expression_id": expression_id, "duration_ms": 4000},
                            supported=True,
                        ),
                    ),
                )
        return None

    @staticmethod
    def _action_command(text: str) -> LocalIntentResult | None:
        if _has_any(text, ("balance a cabeca", "balanca a cabeca", "mexa a cabeca", "cumprimente")):
            return LocalIntentRouter._head_action_result()
        if _has_any(text, ("balan", "balao", "balaum")) and _has_any(text, ("cabec", "acabec")):
            return LocalIntentRouter._head_action_result()
        return None

    @staticmethod
    def _head_action_result() -> LocalIntentResult:
        return LocalIntentResult(
            intent="local_device_action",
            confidence=0.82,
            reply="Claro.",
            expression_id=2,
            action=0,
            emot_event=2,
            speak_reply=False,
            device_commands=(DeviceCommand("play_action", {"action_id": 4}, supported=True),),
        )

    @staticmethod
    def _movement_command(text: str) -> LocalIntentResult | None:
        if not _has_any(text, ("olhe", "olha", "vire", "mexa", "cabeca")):
            return None
        directions = {
            "esquerda": "esquerda",
            "direita": "direita",
            "cima": "cima",
            "baixo": "baixo",
        }
        for key, direction in directions.items():
            if key in text:
                return LocalIntentResult(
                    intent="local_device_move",
                    confidence=0.82,
                    reply=f"Olhando para {direction}.",
                    expression_id=2,
                    action=0,
                    emot_event=2,
                    speak_reply=False,
                    device_commands=(DeviceCommand("look", {"direction": direction}, supported=True),),
                )
        return None

    @staticmethod
    def _light_command(text: str) -> LocalIntentResult | None:
        if not _has_any(text, ("luz", "led", "cor")):
            return None
        colors = ("azul", "vermelho", "verde", "branco", "roxo", "amarelo")
        for color in colors:
            if color in text:
                return LocalIntentResult(
                    intent="local_device_light",
                    confidence=0.80,
                    reply=f"Entendi a luz {color}, mas esse comando ainda não está conectado ao firmware.",
                    expression_id=2,
                    action=0,
                    emot_event=2,
                    device_commands=(DeviceCommand("set_led_color", {"color": color}, supported=False),),
                )
        return None

    @staticmethod
    def _volume_command(text: str, status: dict) -> LocalIntentResult | None:
        has_volume_term = _has_any(text, ("volume", "som", "audio"))
        has_volume_direction = _has_any(text, ("aument", "diminu", "baix"))
        if not has_volume_term or not has_volume_direction and "volume" not in text:
            return None
        match = re.search(r"\b(\d{1,3})\s*%?\b", text)
        if match:
            percent = max(0, min(100, int(match.group(1))))
            reply = f"Volume em {percent} por cento."
            args = {"percent": percent}
        elif "aument" in text:
            percent = min(100, int(status.get("volume", 80)) + 10)
            reply = f"Volume em {percent} por cento."
            args = {"percent": percent}
        elif "diminu" in text or "baix" in text:
            percent = max(0, int(status.get("volume", 80)) - 10)
            reply = f"Volume em {percent} por cento."
            args = {"percent": percent}
        else:
            reply = "Entendi volume, mas preciso de um valor ou direção."
            args = {}
            supported = False
            return LocalIntentResult(
                intent="local_device_volume",
                confidence=0.78,
                reply=reply,
                expression_id=2,
                action=0,
                emot_event=2,
                device_commands=(DeviceCommand("set_volume", args, supported=supported),),
            )
        return LocalIntentResult(
            intent="local_device_volume",
            confidence=0.78,
            reply=reply,
            expression_id=2,
            action=0,
            emot_event=2,
            speak_reply=False,
            device_commands=(DeviceCommand("set_volume", args, supported=True),),
        )

    @staticmethod
    def _sleep_command(text: str) -> LocalIntentResult | None:
        if _has_any(text, ("dorme", "vai dormir", "durma")):
            return LocalIntentResult(
                intent="local_sleep",
                confidence=0.76,
                reply="Entendi. Ainda vou ganhar o comando de sono pelo bridge.",
                expression_id=3,
                action=0,
                emot_event=2,
                device_commands=(DeviceCommand("sleep", {}, supported=False),),
            )
        if _has_any(text, ("acorda", "acorde")):
            return LocalIntentResult(
                intent="local_wake",
                confidence=0.76,
                reply="Estou acordado por aqui.",
                expression_id=1,
                action=1,
                emot_event=2,
            )
        return None
