from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
import random
import re
import unicodedata

from .market import fetch_btc_price, format_btc_reply
from .weather import fetch_weather_now, format_weather_reply


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


def _word_count(text: str) -> int:
    return len(text.split())


def _time_reply(now: datetime) -> str:
    hour = now.hour
    minute = now.minute
    date_suffix = f" São Paulo, {now:%d/%m/%Y}."
    if hour == 0 and minute == 0:
        return f"Agora é meia-noite.{date_suffix}"
    verb = "é" if hour == 1 else "são"
    hour_unit = "hora" if hour == 1 else "horas"
    if minute == 0:
        return f"Agora {verb} {hour} {hour_unit}.{date_suffix}"
    minute_unit = "minuto" if minute == 1 else "minutos"
    return f"Agora {verb} {hour} {hour_unit} e {minute:02d} {minute_unit}.{date_suffix}"


def _date_reply(now: datetime) -> str:
    weekdays = (
        "segunda-feira",
        "terça-feira",
        "quarta-feira",
        "quinta-feira",
        "sexta-feira",
        "sábado",
        "domingo",
    )
    months = (
        "janeiro",
        "fevereiro",
        "março",
        "abril",
        "maio",
        "junho",
        "julho",
        "agosto",
        "setembro",
        "outubro",
        "novembro",
        "dezembro",
    )
    return f"Hoje é {weekdays[now.weekday()]}, {now.day} de {months[now.month - 1]} de {now.year}."


def _float_status(status: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(status.get(key, default))
    except (TypeError, ValueError):
        return default


def _mood_reply(status: dict) -> tuple[str, int, int, int]:
    valence = _float_status(status, "valence")
    activation = _float_status(status, "activation")
    attention = _float_status(status, "attention")

    if valence <= -0.35:
        return (
            "Estou um pouco atravessado agora, mas ainda aqui com você.",
            7,
            0,
            2,
        )
    if activation >= 0.70:
        return (
            "Estou bem acordado e meio elétrico. Pronto para alguma coisa interessante.",
            1,
            1,
            4,
        )
    if activation <= 0.20:
        return (
            "Estou calmo, num ritmo mais quietinho. Mas estou prestando atenção.",
            3,
            0,
            2,
        )
    if attention <= 0.25:
        return (
            "Estou bem, só um pouco disperso. Sua voz me trouxe de volta.",
            2,
            0,
            4,
        )
    if valence >= 0.45:
        return (
            "Estou bem. Leve, curioso, e feliz por você ter perguntado.",
            1,
            1,
            3,
        )
    return (
        "Estou bem. Meio atento, meio curioso, e feliz por você ter perguntado.",
        1,
        1,
        3,
    )


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
            )

        if self._is_date(norm):
            return LocalIntentResult(
                intent="local_date",
                confidence=0.90,
                reply=_date_reply(now),
                expression_id=2,
                action=0,
                emot_event=2,
            )

        if self._is_bridge_test(norm):
            return LocalIntentResult(
                intent="local_bridge_test",
                confidence=0.90,
                reply="Bridge: ouvindo.",
                expression_id=1,
                action=1,
                emot_event=2,
            )

        if self._is_mood(norm):
            reply, expression_id, action, emot_event = _mood_reply(status)
            return LocalIntentResult(
                intent="local_mood",
                confidence=0.88,
                reply=reply,
                expression_id=expression_id,
                action=action,
                emot_event=emot_event,
            )

        if self._is_status(norm):
            health = status.get("health")
            attention = status.get("attention")
            details = []
            if health is not None:
                details.append(f"saude {int(health)}%")
            if attention is not None:
                details.append(f"atencao {int(float(attention) * 100.0)}%")
            suffix = ", ".join(details) if details else "operacional"
            return LocalIntentResult(
                intent="local_status",
                confidence=0.88,
                reply=f"Status: {suffix}.",
                expression_id=1,
                action=1,
                emot_event=2,
            )

        if self._is_network(norm):
            ip = status.get("ip")
            detail = f"ip {ip}" if ip else "ip indisponivel"
            return LocalIntentResult(
                intent="local_network_status",
                confidence=0.84,
                reply=f"Rede: bridge conectado, {detail}.",
                expression_id=2,
                action=0,
                emot_event=2,
            )

        if self._is_btc_price(norm):
            price = fetch_btc_price()
            return LocalIntentResult(
                intent="local_market_btc_price",
                confidence=0.86,
                reply=format_btc_reply(price),
                expression_id=4,
                action=0,
                emot_event=2,
            )

        if self._is_weather(norm):
            weather = fetch_weather_now()
            return LocalIntentResult(
                intent="local_weather",
                confidence=0.84,
                reply=format_weather_reply(weather),
                expression_id=4,
                action=0,
                emot_event=4,
            )

        expression = self._expression_command(norm)
        if expression is not None:
            return expression

        provocation = self._angry_provocation_command(norm)
        if provocation is not None:
            return provocation

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
    def _is_date(text: str) -> bool:
        return _has_any(
            text,
            (
                "que dia e hoje",
                "qual dia e hoje",
                "data de hoje",
                "qual a data",
                "dia de hoje",
                "hoje e que dia",
            ),
        )

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
        return _has_any(text, ("qual seu status", "status do sistema", "diagnostico", "diagnostico do sistema"))

    @staticmethod
    def _is_mood(text: str) -> bool:
        return _has_any(
            text,
            (
                "como voce esta",
                "como vc esta",
                "voce esta bem",
                "esta tudo bem",
                "tudo bem",
                "tudo certo",
                "como voce se sente",
                "seu humor",
            ),
        )

    @staticmethod
    def _is_network(text: str) -> bool:
        return _has_any(text, ("qual seu ip", "voce esta conectado", "esta conectado", "conexao", "rede"))

    @staticmethod
    def _is_btc_price(text: str) -> bool:
        has_asset = _has_any(text, ("bitcoin", "bit coin", "btc", "btse", "b t c"))
        has_price = _has_any(text, ("valor", "preco", "cotacao", "quanto", "vale", "agora", "momento"))
        return has_asset and has_price

    @staticmethod
    def _is_weather(text: str) -> bool:
        has_weather = _has_any(text, ("temperatura", "clima", "tempo"))
        has_now = _has_any(text, ("atual", "agora", "hoje", "esta", "como", "qual"))
        return has_weather and has_now

    @staticmethod
    def _expression_command(text: str) -> LocalIntentResult | None:
        has_expression_trigger = _has_any(text, ("fique", "fica", "expressao", "rosto", "cara", "modo"))
        allow_short_expression = _word_count(text) <= 3
        expressions = {
            "feliz": (1, "feliz"),
            "alegre": (1, "feliz"),
            "sorria": (1, "feliz"),
            "sorriso": (1, "feliz"),
            "curioso": (2, "curioso"),
            "curiosa": (2, "curioso"),
            "curiosidade": (2, "curioso"),
            "sonolento": (3, "sonolento"),
            "sonolenta": (3, "sonolento"),
            "sono": (3, "sonolento"),
            "focado": (4, "focado"),
            "focada": (4, "focado"),
            "foco": (4, "focado"),
            "surpreso": (6, "surpreso"),
            "surpresa": (6, "surpreso"),
            "triste": (7, "triste"),
            # ANGRY — sempre transitório, não vira baseline
            "bravo": (9, "bravo"),
            "brava": (9, "bravo"),
            "raiva": (9, "bravo"),
            "irritado": (9, "bravo"),
            "irritada": (9, "bravo"),
            "zangado": (9, "bravo"),
            "zangada": (9, "bravo"),
        }
        for key, (expression_id, label) in expressions.items():
            if key in text:
                if not has_expression_trigger and not allow_short_expression:
                    return None
                if expression_id == 9:
                    # ANGRY: speak_reply=True para dar feedback teatral ao usuário
                    return LocalIntentResult(
                        intent="local_device_expression_angry",
                        confidence=0.85,
                        reply="Tá bem. Expressão de raiva ativada. Por exactamente três segundos.",
                        expression_id=expression_id,
                        action=0,
                        emot_event=2,
                        speak_reply=True,
                        device_commands=(
                            DeviceCommand(
                                "set_expression",
                                {"expression_id": expression_id, "duration_ms": 3000},
                                supported=True,
                            ),
                        ),
                    )
                return LocalIntentResult(
                    intent="local_device_expression",
                    confidence=0.84,
                    reply=f"Pronto, expressão {label}.",
                    expression_id=expression_id,
                    action=0,
                    emot_event=2,
                    device_commands=(
                        DeviceCommand(
                            "set_expression",
                            {"expression_id": expression_id, "duration_ms": 4000},
                            supported=True,
                        ),
                    ),
                )
        return None

    # ── Provocações e taunts ──────────────────────────────────────────────────

    _ANGRY_REPLIES: tuple[str, ...] = (
        "Tá. Agora eu fiquei oficialmente ofendido.",
        "Eu ouvi isso. E vou fingir que não doeu.",
        "Não. Meu bom senso acabou de vetar essa ideia.",
        "Hm. Você está testando minha paciência de silício.",
        "Anota aí: isso foi uma provocação registrada nos meus logs.",
        "Processando... processado. Definitivamente não gostei.",
    )

    @staticmethod
    def _angry_provocation_command(text: str) -> LocalIntentResult | None:
        """Detecta provocações leves e responde com ANGRY transitório e réplica teatral."""
        triggers = (
            "voce errou",
            "voce falhou",
            "errou de novo",
            "errou feio",
            "robo burro",
            "robo idiota",
            "robo estupido",
            "robo inutil",
            "voce e burro",
            "voce e idiota",
            "voce e pessimo",
            "voce e horrivel",
            "voce e uma droga",
            "voce e inutil",
            "prefiro outra ia",
            "prefiro o chatgpt",
            "prefiro o chat",
            "prefiro alexa",
            "prefiro a siri",
            "prefiro o gemini",
        )
        if not _has_any(text, triggers):
            return None
        reply = random.choice(LocalIntentRouter._ANGRY_REPLIES)
        return LocalIntentResult(
            intent="local_angry_provocation",
            confidence=0.88,
            reply=reply,
            expression_id=9,
            action=0,
            emot_event=2,
            speak_reply=True,
        )

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
        has_direct_loudness = _has_any(text, ("mais alto", "mais baixo", "fala alto", "fala baixo"))
        if not has_volume_term and not has_direct_loudness:
            return None
        if has_volume_term and not has_volume_direction and "volume" not in text:
            return None
        match = re.search(r"\b(\d{1,3})\s*%?\b", text)
        if match:
            percent = max(0, min(100, int(match.group(1))))
            reply = f"Volume em {percent} por cento."
            args = {"percent": percent}
        elif "aument" in text or "alto" in text:
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
