"""Local deterministic PT-BR intents for the NoiseBot server.

Intents locais sao deterministicos; clima usa consulta HTTP curta quando
solicitado explicitamente.
"""
from __future__ import annotations

import re
import unicodedata
from datetime import datetime

from .runtime import IntentResolved
from ..vision import VisionAnalysis, VisionClient, VisionError, VisionObservation
from .weather import fetch_weather_now, format_weather_reply


# -- Normalizacao de texto ---------------------------------------------------

def _normalize(text: str) -> str:
    normalized = unicodedata.normalize("NFD", text or "")
    without_accents = "".join(ch for ch in normalized if unicodedata.category(ch) != "Mn")
    lowered = without_accents.lower()
    lowered = re.sub(r"[^a-z0-9% ]+", " ", lowered)
    lowered = re.sub(r"\s+", " ", lowered).strip()
    return lowered


def _has(text: str, *terms: str) -> bool:
    return any(t in text for t in terms)


def _word_count(text: str) -> int:
    return len(text.split())


# -- Agenda helpers ---------------------------------------------------------

_UNIT_SECONDS: dict[str, int] = {
    "segundo": 1,
    "segundos": 1,
    "minuto": 60,
    "minutos": 60,
    "hora": 3600,
    "horas": 3600,
}

_DEFAULT_VOLUME_PERCENT = 80

_TIMER_TERMS = ("timer", "timing", "temporizador", "contador", "contagem")
_CANCEL_TERMS = (
    "cancela",
    "cancelar",
    "cancele",
    "para",
    "pare",
    "parar",
    "desliga",
    "desligar",
    "encerra",
    "encerrar",
    "remove",
    "remover",
)

_STOP_ONLY_PHRASES = {
    "pare",
    "para",
    "parar",
    "pare agora",
    "para agora",
    "para de falar",
    "pode parar",
    "cancela",
    "cancelar",
    "cancele",
    "cancele isso",
    "cancela isso",
    "chega disso",
    "corta",
    "corta isso",
    "encerra",
    "fica quieto",
    "nao quero mais",
    "silencio",
}


def _parse_duration_s(text: str) -> int | None:
    match = re.search(r"\b(\d+)\s*(segundos?|minutos?|horas?)\b", text)
    if not match:
        return None
    return int(match.group(1)) * _UNIT_SECONDS[match.group(2)]


def _parse_percent(text: str) -> int | None:
    match = re.search(r"\b(\d{1,3})\s*(?:%|por cento)?\b", text)
    if not match:
        return None
    return max(0, min(100, int(match.group(1))))


def _status_volume(status: dict) -> int:
    try:
        return max(0, min(100, int(status.get("volume", _DEFAULT_VOLUME_PERCENT))))
    except (TypeError, ValueError):
        return _DEFAULT_VOLUME_PERCENT


def _agenda_command(action: str, **payload: object) -> dict[str, object]:
    return {
        "event": "AGENDA_COMMAND",
        "action": action,
        **payload,
    }


def _volume_command(percent: int) -> dict[str, object]:
    return {
        "event": "VOLUME_COMMAND",
        "percent": max(0, min(100, int(percent))),
    }


def _settings_command(**payload: object) -> dict[str, object]:
    return {
        "event": "SETTINGS_COMMAND",
        **payload,
    }


def _led_command(action: str, **payload: object) -> dict[str, object]:
    return {
        "event": "LED_COMMAND",
        "action": action,
        **payload,
    }


def _alert_command(action: str) -> dict[str, object]:
    return {
        "event": "ALERT_COMMAND",
        "action": action,
    }


def _status_command() -> dict[str, object]:
    return {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }


def _is_status_display_command(text: str) -> bool:
    if "barra de status" in text:
        return True
    return re.search(
        r"\b(?:mostrar|mostra|mostre|exibir|exibe|exiba)"
        r"(?:\s+(?:o|a|ai|aqui|pra|para|mim|me|por|favor)){0,5}"
        r"\s+status\b",
        text,
    ) is not None


def _extract_timer_label(text: str) -> str:
    match = re.search(r"\bchamado\s+(.+?)\s+(?:de|por|para)\s+\d+\s*(?:segundos?|minutos?|horas?)\b", text)
    if match:
        return match.group(1).strip()[:48] or "timer"
    return "timer"


def _extract_cancel_label(text: str, kind: str) -> str | None:
    verbs = r"(?:cancela|cancelar|cancele|para|pare|parar|desliga|desligar|encerra|encerrar|remove|remover)"
    patterns = [
        rf"\b{verbs}\s+(?:o\s+|a\s+)?{kind}\s+(?:do|da|de)\s+(.+)$",
        rf"\b{verbs}\s+(?:o\s+|a\s+)?{kind}\s+chamado\s+(.+)$",
    ]
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return match.group(1).strip()[:48] or None
    return None


def _extract_timer_cancel_label(text: str) -> str | None:
    for term in _TIMER_TERMS:
        label = _extract_cancel_label(text, term)
        if label:
            return label
    return None


def _extract_reminder_label(text: str) -> str:
    match = re.search(r"\bme\s+lembre\s+de\s+(.+?)\s+daqui\s+a\s+\d+\s*(?:segundos?|minutos?|horas?)\b", text)
    if match:
        return match.group(1).strip()[:48] or "lembrete"
    return "lembrete"


def _parse_alarm_time(text: str) -> tuple[int, int] | None:
    match = re.search(r"\b(?:as|às)?\s*(\d{1,2})\s+e\s+meia\b", text)
    if match:
        hour = int(match.group(1))
        return (hour, 30) if 0 <= hour <= 23 else None

    match = re.search(r"\b(?:as|às)?\s*(\d{1,2})\s*(?:h|horas?)?\s+(\d{1,2})\b", text)
    if match:
        hour = int(match.group(1))
        minute = int(match.group(2))
        return (hour, minute) if 0 <= hour <= 23 and 0 <= minute <= 59 else None

    match = re.search(r"\b(?:as|às)?\s*(\d{1,2})\s*(?:h|horas?)\b", text)
    if match:
        hour = int(match.group(1))
        return (hour, 0) if 0 <= hour <= 23 else None

    return None


def _parse_weekdays_mask(text: str) -> int:
    if "segunda a sexta" in text or "segunda ate sexta" in text:
        return 0x3E
    if "segunda a sabado" in text or "segunda ate sabado" in text:
        return 0x7E
    days = {
        "domingo": 0,
        "segunda": 1,
        "terca": 2,
        "quarta": 3,
        "quinta": 4,
        "sexta": 5,
        "sabado": 6,
    }
    mask = 0
    for name, bit in days.items():
        if name in text:
            mask |= 1 << bit
    return mask


def _parse_led_color(text: str) -> tuple[str, int, int, int] | None:
    colors: list[tuple[str, tuple[str, ...], tuple[int, int, int]]] = [
        ("azul", ("azul", "azuis"), (40, 120, 255)),
        ("verde", ("verde", "verdes"), (40, 220, 90)),
        ("vermelho", ("vermelho", "vermelha", "vermelhos", "vermelhas"), (255, 0, 0)),
        ("branco", ("branco", "branca", "brancos", "brancas"), (255, 255, 220)),
        ("amarelo", ("amarelo", "amarela", "amarelos", "amarelas"), (255, 200, 40)),
        ("roxo", ("roxo", "roxa", "roxos", "roxas", "purpura"), (120, 55, 255)),
        ("rosa", ("rosa", "magenta"), (255, 80, 180)),
        ("ciano", ("ciano", "cyan"), (35, 220, 240)),
        ("laranja", ("laranja",), (255, 80, 0)),
    ]
    for name, terms, rgb in colors:
        if any(term in text for term in terms):
            return name, rgb[0], rgb[1], rgb[2]
    return None


# -- Replies de tempo --------------------------------------------------------

def _time_reply(now: datetime) -> str:
    hour = now.hour
    minute = now.minute
    date_suffix = f" Brasilia, {now:%d/%m/%Y}."
    if hour == 0 and minute == 0:
        return f"Agora e meia-noite.{date_suffix}"
    verb = "e" if hour == 1 else "sao"
    hour_unit = "hora" if hour == 1 else "horas"
    if minute == 0:
        return f"Agora {verb} {hour} {hour_unit}.{date_suffix}"
    minute_unit = "minuto" if minute == 1 else "minutos"
    return f"Agora {verb} {hour} {hour_unit} e {minute:02d} {minute_unit}.{date_suffix}"


def _date_reply(now: datetime) -> str:
    weekdays = (
        "segunda-feira",
        "terca-feira",
        "quarta-feira",
        "quinta-feira",
        "sexta-feira",
        "sabado",
        "domingo",
    )
    months = (
        "janeiro",
        "fevereiro",
        "marco",
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
    return f"Hoje e {weekdays[now.weekday()]}, {now.day} de {months[now.month - 1]} de {now.year}."


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
            "Estou um pouco atravessado agora, mas ainda aqui com voce.",
            7,
            _ACTION_NONE,
            _EMOT_NEUTRAL,
        )
    if activation >= 0.70:
        return (
            "Estou bem acordado e meio eletrico. Pronto para alguma coisa interessante.",
            _EXPR_ATTENTIVE,
            _ACTION_NOD,
            _EMOT_CURIOUS,
        )
    if activation <= 0.20:
        return (
            "Estou calmo, num ritmo mais quietinho. Mas estou prestando atencao.",
            3,
            _ACTION_NONE,
            _EMOT_NEUTRAL,
        )
    if attention <= 0.25:
        return (
            "Estou bem, so um pouco disperso. Sua voz me trouxe de volta.",
            _EXPR_CURIOUS,
            _ACTION_NONE,
            _EMOT_CURIOUS,
        )
    if valence >= 0.45:
        return (
            "Estou bem. Leve, curioso, e feliz por voce ter perguntado.",
            _EXPR_HAPPY,
            _ACTION_NOD,
            _EMOT_HAPPY,
        )
    return (
        "Estou bem. Meio atento, meio curioso, e feliz por voce ter perguntado.",
        _EXPR_HAPPY,
        _ACTION_NOD,
        _EMOT_HAPPY,
    )


def _curiosity_reply(turn_id: int) -> str:
    index = max(0, turn_id) % len(_CURIOSITY_REPLIES)
    return _CURIOSITY_REPLIES[index]


def _vision_unavailable_reply() -> str:
    return "Ainda nao consegui acessar minha camera agora."


def _scene_label(scene: str) -> str:
    labels = {
        "dark": "escura",
        "dim": "com pouca luz",
        "normal": "com iluminacao normal",
        "bright": "bem clara",
        "flat": "com pouco contraste",
    }
    return labels.get(scene, "indefinida")


def _vision_scene_reply(obs: VisionObservation) -> str:
    if not obs.valid:
        return "Consegui consultar a camera, mas a observacao ainda nao ficou valida."
    return (
        f"Estou vendo uma cena {_scene_label(obs.scene)}. "
        f"A imagem veio em {obs.width} por {obs.height}, com contraste {obs.contrast}."
    )


def _vision_light_reply(obs: VisionObservation) -> str:
    if not obs.valid:
        return "A camera respondeu, mas nao tenho leitura confiavel de luz ainda."
    if obs.luma_avg < 45:
        level = "esta bem escuro"
    elif obs.luma_avg < 90:
        level = "esta com pouca luz"
    elif obs.luma_avg > 210:
        level = "esta bem claro"
    else:
        level = "esta com luz normal"
    return f"Pela camera, {level}. O brilho medio esta em {obs.luma_avg} de 255."


def _vision_motion_reply(obs: VisionObservation) -> str:
    if not obs.valid:
        return "A camera respondeu, mas ainda nao tenho movimento confiavel."
    if obs.motion_score >= 24:
        return f"Percebi movimento forte agora. O score ficou em {obs.motion_score}."
    if obs.motion_score >= 10:
        return f"Percebi algum movimento na cena. O score ficou em {obs.motion_score}."
    return f"Nao percebi muito movimento agora. O score ficou em {obs.motion_score}."


def _vision_person_reply(obs: VisionObservation) -> str:
    if not obs.valid:
        return "Minha camera respondeu, mas ainda nao tenho leitura visual confiavel."
    return (
        "Minha camera esta funcionando, mas eu ainda nao tenho deteccao de pessoa ligada. "
        "Consigo medir luz, contraste e movimento por enquanto."
    )


def _face_position_label(analysis: VisionAnalysis) -> str:
    x = analysis.face_center_norm_x
    if x is None:
        return "na imagem"
    if x < -0.33:
        return "mais para a esquerda da imagem"
    if x > 0.33:
        return "mais para a direita da imagem"
    return "perto do centro da imagem"


def _vision_person_analysis_reply(analysis: VisionAnalysis) -> str:
    obs = analysis.observation
    if not obs.valid:
        return "Minha camera respondeu, mas ainda nao tenho leitura visual confiavel."
    if not analysis.detector_available:
        return (
            "Minha camera esta funcionando, mas o detector de rosto do bridge nao esta disponivel. "
            "Consigo medir luz, contraste e movimento por enquanto."
        )
    if analysis.face_detected:
        rosto = "um rosto" if analysis.face_count == 1 else f"{analysis.face_count} rostos"
        return f"Sim. Detectei {rosto} {_face_position_label(analysis)}."
    return "Estou vendo a cena, mas nao detectei rosto agora."


# -- Mapeamentos de intents --------------------------------------------------

# expression_id firmware: 0=NEUTRAL, 1=HAPPY, 2=CURIOUS, 3=SLEEPY, 4=FOCUSED
# action_id:     0=none, 1=nod, 2=shake, 3=look_up, 4=look_down
# emot_event_id: 2=neutral, 3=happy, 4=curious

_EXPR_NEUTRAL    = 0
_EXPR_ATTENTIVE  = 2
_EXPR_HAPPY      = 1
_EXPR_CURIOUS    = 2
_EXPR_FOCUSED    = 4
_EMOT_NEUTRAL    = 2
_EMOT_HAPPY      = 3
_EMOT_CURIOUS    = 4
_ACTION_NOD      = 1
_ACTION_NONE     = 0

_CURIOSITY_REPLIES = (
    "Curiosidade: polvos tem tres coracoes e sangue azulado.",
    "Curiosidade: em Venus, um dia dura mais que um ano.",
    "Curiosidade: bananas sao classificadas como bagas pela botanica.",
    "Curiosidade: o mel quase nao estraga quando fica bem fechado.",
)

_STOP_AFTER_BARGE_MISHEARS = frozenset({
    "chau",
    "tchau",
    "vale",
    "valeu",
    "xau",
})

_STOP_AFTER_BARGE_TERMS = (
    "adeus",
    "bye",
    "chega",
    "pronto",
    "tchau",
)


class LocalIntentProvider:
    """Provider de intents locais PT-BR, determinístico, sem I/O.

    match() retorna IntentResolved com intent_name=None se nao houver intent local.
    """

    def __init__(self, vision_client: VisionClient | None = None) -> None:
        self._vision = vision_client

    def match(
        self,
        text: str,
        turn_id: int,
        context: dict | None = None,
        now: datetime | None = None,
    ) -> IntentResolved:
        """Tenta casar o texto com um intent local.

        Retorna IntentResolved com intent_name=None se nenhum intent foi encontrado.
        """
        norm = _normalize(text)
        if not norm:
            return IntentResolved(turn_id=turn_id, intent_name=None)

        now = now or datetime.now()
        context = context or {}
        status = context.get("status", {})

        # -- Controle direto --------------------------------------------------
        # Um "ww -> pare" depois de barge-in e comando de controle, nao prompt LLM.
        recent_barge_in = bool(context.get("recent_barge_in"))
        direct_stop = norm in _STOP_ONLY_PHRASES
        post_barge_stop = (
            recent_barge_in
            and (
                norm in _STOP_AFTER_BARGE_MISHEARS
                or _has(norm, *_STOP_AFTER_BARGE_TERMS)
            )
        )
        if direct_stop or post_barge_stop:
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_stop",
                reply_text="Pronto, parei.",
                expression_id=_EXPR_NEUTRAL,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                resolution_reason="direct_stop" if direct_stop else "post_barge_stop",
            )

        # -- Alertas locais ---------------------------------------------------
        if _has(norm, "silencia", "silenciar", "para de tocar",
                "pare de tocar", "para o alarme", "desliga o alarme",
                "desligar alarme", "cala o alarme"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_alert_silence",
                reply_text="Pronto, silenciei.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_alert_command("silence"),
            )

        # -- Agenda: timers ---------------------------------------------------
        has_timer_ref = _has(norm, *_TIMER_TERMS)

        if has_timer_ref and _has(norm, *_CANCEL_TERMS):
            label = _extract_timer_cancel_label(norm)
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_timer_cancel",
                reply_text="Timer cancelado." if label else "Vou cancelar o timer.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_agenda_command("timer_cancel", label=label or ""),
            )

        if has_timer_ref and _has(norm, "quanto falta", "tempo falta"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_timer_status",
                reply_text="Os timers ativos aparecem no dashboard do robo.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if has_timer_ref:
            duration_s = _parse_duration_s(norm)
            if duration_s is not None:
                label = _extract_timer_label(norm)
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_timer_create",
                    reply_text=f"Timer {label} iniciado.",
                    expression_id=_EXPR_ATTENTIVE,
                    action_id=_ACTION_NOD,
                    emot_event_id=_EMOT_NEUTRAL,
                    device_command=_agenda_command(
                        "timer_create",
                        duration_ms=duration_s * 1000,
                        label=label,
                    ),
                )

        # -- Agenda: lembretes ------------------------------------------------
        if _has(norm, "lembrete", "lembre") and _has(norm, "cancela", "cancelar"):
            label = _extract_cancel_label(norm, "lembrete") or _extract_cancel_label(norm, "lembrete de")
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_reminder_cancel",
                reply_text="Lembrete cancelado.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_agenda_command("reminder_cancel", label=label or ""),
            )

        if _has(norm, "me lembre", "lembre de"):
            delay_s = _parse_duration_s(norm)
            if delay_s is not None:
                label = _extract_reminder_label(norm)
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_reminder_create",
                    reply_text=f"Combinado, vou lembrar: {label}.",
                    expression_id=_EXPR_ATTENTIVE,
                    action_id=_ACTION_NOD,
                    emot_event_id=_EMOT_NEUTRAL,
                    device_command=_agenda_command(
                        "reminder_create",
                        delay_ms=delay_s * 1000,
                        label=label,
                    ),
                )

        # -- Agenda: alarmes --------------------------------------------------
        if _has(norm, "alarme") and _has(norm, "quais", "ativos"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_alarm_list",
                reply_text="Os alarmes ativos estao no dashboard do robo.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if _has(norm, "alarme") and _has(norm, "desativa", "desativar", "pausa", "pausar"):
            label = _extract_cancel_label(norm, "alarme")
            if label is None and "manha" in norm:
                label = "manha"
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_alarm_disable",
                reply_text="Alarme desativado.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_agenda_command("alarm_set_enabled", label=label or "", enabled=False),
            )

        if _has(norm, "alarme"):
            alarm_time = _parse_alarm_time(norm)
            if alarm_time is not None:
                hour, minute = alarm_time
                label = "manha" if "manha" in norm else "alarme"
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_alarm_create",
                    reply_text=f"Alarme criado para {hour:02d}:{minute:02d}.",
                    expression_id=_EXPR_ATTENTIVE,
                    action_id=_ACTION_NOD,
                    emot_event_id=_EMOT_NEUTRAL,
                    device_command=_agenda_command(
                        "alarm_create",
                        hour=hour,
                        minute=minute,
                        weekdays_mask=_parse_weekdays_mask(norm),
                        label=label,
                        enabled=True,
                    ),
                )

        if _has(norm, "agenda", "agendar", "agende", "lembrete", "lembre", "timer", "alarme"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_agenda_incomplete",
                reply_text=(
                    "Ainda preciso de tempo e tipo. Pode dizer, por exemplo: "
                    "timer de 5 minutos, ou me lembre de beber agua daqui a 10 minutos."
                ),
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        # -- Tempo atual -------------------------------------------------------
        if _has(norm, "que horas", "horas sao", "hora e", "hora esta",
                "que hora", "me diz a hora", "diz a hora", "horas agora"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_time",
                reply_text=_time_reply(now),
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if _has(norm, "que dia e hoje", "qual dia e hoje", "data de hoje",
                "qual a data", "dia de hoje", "hoje e que dia"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_date",
                reply_text=_date_reply(now),
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        wants_led_brightness = _has(
            norm,
            "brilho do led",
            "brilho dos leds",
            "led mais forte",
            "led mais fraco",
            "luz mais forte",
            "luzes mais fortes",
            "luz mais fraca",
            "luzes mais fracas",
        )
        wants_generic_brightness = _has(
            norm,
            "aumenta o brilho",
            "aumente o brilho",
            "diminui o brilho",
            "diminua o brilho",
            "abaixa o brilho",
            "baixa o brilho",
            "brilho mais forte",
            "brilho mais fraco",
        )

        if _has(norm, "brilho da tela", "brilho tela", "tela mais clara", "tela mais escura",
                "brilho do display", "display mais claro", "display mais escuro"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_display_brightness_unsupported",
                reply_text="Essa tela nao tem backlight ajustavel; posso ajustar o brilho dos LEDs.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if wants_led_brightness:
            percent = _parse_percent(norm)
            if percent is None:
                percent = 90 if _has(norm, "forte", "fortes", "aumenta", "aumentar", "aumente") else 25
            brightness = max(0, min(255, int(round(percent * 2.55))))
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_led_brightness",
                reply_text=f"Brilho dos LEDs em {percent} por cento.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_settings_command(led_brightness=brightness),
            )

        if wants_generic_brightness:
            percent = _parse_percent(norm)
            if percent is None:
                percent = 90 if _has(norm, "forte", "fortes", "aumenta", "aumentar", "aumente") else 25
            brightness = max(0, min(255, int(round(percent * 2.55))))
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_led_brightness",
                reply_text=f"Brilho dos LEDs em {percent} por cento.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_settings_command(led_brightness=brightness),
            )

        if _has(norm, "luz normal", "led normal", "cor normal", "volta a luz", "volte a luz"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_light_reset",
                reply_text="Luzes de volta ao modo normal.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_led_command("reset"),
            )

        if _has(norm, "acende a luz", "acenda a luz", "mude a luz", "mudar a luz",
                "mude a cor", "mudar a cor", "troque a cor", "trocar a cor",
                "deixe a luz", "deixa a luz", "deixe os leds", "deixa os leds",
                "luz ", "luzes", "led ", "leds", "cor do led", "cor dos leds"):
            color = _parse_led_color(norm)
            if color is not None:
                name, red, green, blue = color
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_light_color",
                    reply_text=f"Luzes em {name}.",
                    expression_id=_EXPR_ATTENTIVE,
                    action_id=_ACTION_NONE,
                    emot_event_id=_EMOT_NEUTRAL,
                    device_command=_led_command("color", r=red, g=green, b=blue),
                )
            if _has(norm, "acende a luz", "acenda a luz"):
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_light_color",
                    reply_text="Luzes acesas.",
                    expression_id=_EXPR_ATTENTIVE,
                    action_id=_ACTION_NONE,
                    emot_event_id=_EMOT_NEUTRAL,
                    device_command=_led_command("color", r=255, g=255, b=220),
                )
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_light_incomplete",
                reply_text="Qual cor voce quer nos LEDs?",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        # -- Teste de bridge ---------------------------------------------------
        if _has(norm, "teste do bridge", "bridge teste", "test bridge",
                "bridge funcionando", "bridge ok"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_bridge_test",
                reply_text="Bridge: ouvindo.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if _has(norm, "temperatura", "clima", "tempo") and _has(
            norm, "atual", "agora", "hoje", "esta", "como", "qual"
        ):
            weather = fetch_weather_now()
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_weather",
                reply_text=format_weather_reply(weather),
                expression_id=_EXPR_CURIOUS,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_CURIOUS,
            )

        # -- Visao local -------------------------------------------------------
        if _has(norm, "o que voce esta vendo", "o que vc esta vendo",
                "o que esta vendo", "o que voce ve", "o que vc ve",
                "descreve a cena", "descreva a cena"):
            return self._match_vision(
                turn_id=turn_id,
                intent_name="local_vision_scene",
                formatter=_vision_scene_reply,
            )

        if _has(norm, "voce esta me vendo", "vc esta me vendo",
                "consegue me ver", "esta me vendo", "me ve"):
            return self._match_vision(
                turn_id=turn_id,
                intent_name="local_vision_person",
                formatter=_vision_person_reply,
                analyzer_formatter=_vision_person_analysis_reply,
            )

        if _has(norm, "como esta a luz", "como esta iluminacao",
                "como esta a iluminacao", "esta claro", "esta escuro",
                "tem luz", "luz ambiente"):
            return self._match_vision(
                turn_id=turn_id,
                intent_name="local_vision_light",
                formatter=_vision_light_reply,
            )

        if _has(norm, "tem movimento", "viu movimento", "percebe movimento",
                "alguma coisa mexeu", "algo se mexeu"):
            return self._match_vision(
                turn_id=turn_id,
                intent_name="local_vision_motion",
                formatter=_vision_motion_reply,
            )

        # -- Humor/sentimento do robo ------------------------------------------
        if _has(norm, "como voce esta", "como vc esta", "tudo bem", "tudo certo",
                "como esta", "como voce se sente", "esta feliz", "esta triste",
                "seu humor", "esta bem", "se sente"):
            reply_text, expression_id, action_id, emot_event_id = _mood_reply(status)
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_mood",
                reply_text=reply_text,
                expression_id=expression_id,
                action_id=action_id,
                emot_event_id=emot_event_id,
            )

        # -- Status do sistema -------------------------------------------------
        if _is_status_display_command(norm) or _has(
            norm,
            "seu status",
            "status do sistema",
            "diagnostico",
            "diagnostico do sistema",
        ):
            health = status.get("health")
            attention = status.get("attention")
            details = []
            if health is not None:
                details.append(f"saude {int(health)}%")
            if attention is not None:
                details.append(f"atencao {int(float(attention) * 100.0)}%")
            suffix = ", ".join(details) if details else "operacional"
            visual_only = _is_status_display_command(norm)
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_status",
                reply_text=None if visual_only else f"Status: {suffix}.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_status_command(),
            )

        # -- Status de rede ----------------------------------------------------
        if _has(norm, "esta conectado", "tem conexao", "tem internet",
                "status da rede", "rede ok", "network"):
            ip = status.get("ip")
            detail = f"ip {ip}" if ip else "ip indisponivel"
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_network_status",
                reply_text=f"Rede: bridge conectado, {detail}.",
                expression_id=_EXPR_NEUTRAL,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_status_command(),
            )

        # -- Curiosidade local ------------------------------------------------
        if _has(norm, "curiosidade") and _has(
            norm,
            "me conte",
            "conte",
            "me diga",
            "diga",
            "me fale",
            "fale",
            "outra",
            "uma",
        ):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_curiosity_fact",
                reply_text=_curiosity_reply(turn_id),
                expression_id=_EXPR_CURIOUS,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_CURIOUS,
            )

        # -- Expressao direta --------------------------------------------------
        has_expression_trigger = _has(norm, "fique", "fica", "expressao", "rosto", "cara", "modo")
        if has_expression_trigger or _word_count(norm) <= 3:
            if _has(norm, "feliz", "alegre", "sorria", "sorriso"):
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_expression_happy",
                    reply_text="Pronto, feliz.",
                    expression_id=_EXPR_HAPPY,
                    action_id=_ACTION_NONE,
                    emot_event_id=_EMOT_HAPPY,
                )
            if _has(norm, "curioso", "curiosa", "curiosidade"):
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_expression_curious",
                    reply_text="Pronto, curioso.",
                    expression_id=_EXPR_CURIOUS,
                    action_id=_ACTION_NONE,
                    emot_event_id=_EMOT_CURIOUS,
                )
            if _has(norm, "focado", "focada", "foco"):
                return IntentResolved(
                    turn_id=turn_id,
                    intent_name="local_expression_focused",
                    reply_text="Pronto, focado.",
                    expression_id=_EXPR_FOCUSED,
                    action_id=_ACTION_NONE,
                    emot_event_id=_EMOT_NEUTRAL,
                )

        # -- Saudacoes ---------------------------------------------------------
        if _has(norm, "ola", "oi ", "oi!", "oi.", "bom dia", "boa tarde",
                "boa noite", "hey ", "hello", "hi "):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_greeting",
                reply_text="Ola! Como posso ajudar?",
                expression_id=_EXPR_HAPPY,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_HAPPY,
            )

        # -- Despedida ---------------------------------------------------------
        if _has(norm, "tchau", "ate logo", "ate mais", "bye", "adeus",
                "encerrar", "pode parar", "para de ouvir"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_farewell",
                reply_text="Ate logo!",
                expression_id=_EXPR_HAPPY,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_HAPPY,
            )

        # -- Olhar em direcoes -------------------------------------------------
        if _has(norm, "olha pra cima", "olha para cima", "olhe para cima", "olha acima"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_look_up",
                reply_text=None,
                expression_id=_EXPR_CURIOUS,
                action_id=3,  # look_up
                emot_event_id=_EMOT_CURIOUS,
            )

        if _has(norm, "olha pra baixo", "olha para baixo", "olhe para baixo", "olha abaixo"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_look_down",
                reply_text=None,
                expression_id=_EXPR_NEUTRAL,
                action_id=4,  # look_down
                emot_event_id=_EMOT_NEUTRAL,
            )

        if _has(norm, "olha pra esquerda", "olha para esquerda", "esquerda"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_look_left",
                reply_text=None,
                expression_id=_EXPR_CURIOUS,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_CURIOUS,
            )

        if _has(norm, "olha pra direita", "olha para direita", "direita"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_look_right",
                reply_text=None,
                expression_id=_EXPR_CURIOUS,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_CURIOUS,
            )

        # -- Volume ------------------------------------------------------------
        if _has(norm, "volume", "som", "audio") or _has(norm, "mais alto", "mais baixo", "fala mais alto", "fala mais baixo"):
            explicit_percent = _parse_percent(norm)
            current = _status_volume(status)
            if explicit_percent is not None:
                percent = explicit_percent
            elif _has(norm, "aumenta", "aumentar", "aumente", "alto", "mais alto"):
                percent = min(100, current + 10)
            elif _has(norm, "diminui", "diminuir", "diminua", "baixa", "baixar", "baixo", "mais baixo", "silencio", "fica quieto"):
                percent = max(0, current - 10)
            else:
                percent = current
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_volume_set",
                reply_text=f"Volume em {percent} por cento.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
                device_command=_volume_command(percent),
            )

        # -- Sem intent local --------------------------------------------------
        return IntentResolved(turn_id=turn_id, intent_name=None)

    def _match_vision(
        self,
        turn_id: int,
        intent_name: str,
        formatter,
        analyzer_formatter=None,
    ) -> IntentResolved:
        reply = _vision_unavailable_reply()
        if self._vision is not None:
            try:
                if analyzer_formatter is not None and hasattr(self._vision, "analyze"):
                    reply = analyzer_formatter(self._vision.analyze())
                else:
                    reply = formatter(self._vision.observe())
            except VisionError:
                reply = _vision_unavailable_reply()
        return IntentResolved(
            turn_id=turn_id,
            intent_name=intent_name,
            reply_text=reply,
            expression_id=_EXPR_CURIOUS,
            action_id=_ACTION_NONE,
            emot_event_id=_EMOT_CURIOUS,
        )
