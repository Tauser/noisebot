"""bridgev2.llm.local_intent -- LocalIntentProvider: intents PT-BR determinísticos.

Portado de noisebot_bridge/intent_router.py para bridgev2 standalone.
Sem dependencia do bridge v1. Intents locais sao determinísticos; clima usa
consulta HTTP curta quando solicitado explicitamente.
"""
from __future__ import annotations

import re
import unicodedata
from datetime import datetime

from ..runtime.events import IntentResolved
from ..vision import VisionClient, VisionError, VisionObservation
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


def _parse_duration_s(text: str) -> int | None:
    match = re.search(r"\b(\d+)\s*(segundos?|minutos?|horas?)\b", text)
    if not match:
        return None
    return int(match.group(1)) * _UNIT_SECONDS[match.group(2)]


def _agenda_command(action: str, **payload: object) -> dict[str, object]:
    return {
        "event": "AGENDA_COMMAND",
        "action": action,
        **payload,
    }


def _alert_command(action: str) -> dict[str, object]:
    return {
        "event": "ALERT_COMMAND",
        "action": action,
    }


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


# -- Mapeamentos de intents --------------------------------------------------

# expression_id: 1=ATTENTIVE, 2=NEUTRAL, 3=HAPPY, 4=CURIOUS, 5=FOCUSED
# action_id:     0=none, 1=nod, 2=shake, 3=look_up, 4=look_down
# emot_event_id: 2=neutral, 3=happy, 4=curious

_EXPR_NEUTRAL    = 2
_EXPR_ATTENTIVE  = 1
_EXPR_HAPPY      = 3
_EXPR_CURIOUS    = 4
_EMOT_NEUTRAL    = 2
_EMOT_HAPPY      = 3
_EMOT_CURIOUS    = 4
_ACTION_NOD      = 1
_ACTION_NONE     = 0


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
        if _has(norm, "seu status", "status do sistema", "diagnostico", "diagnostico do sistema"):
            health = status.get("health")
            attention = status.get("attention")
            details = []
            if health is not None:
                details.append(f"saude {int(health)}%")
            if attention is not None:
                details.append(f"atencao {int(float(attention) * 100.0)}%")
            suffix = ", ".join(details) if details else "operacional"
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_status",
                reply_text=f"Status: {suffix}.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_NEUTRAL,
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
            )

        # -- Preco do Bitcoin --------------------------------------------------
        if _has(norm, "bitcoin", "btc", "preco do bitcoin", "valor do bitcoin",
                "quanto vale", "criptomoeda"):
            # Sem fetch de rede -- reply placeholder
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_market_btc_price",
                reply_text="Preco do Bitcoin indisponivel no momento.",
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
                    expression_id=5,
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
        if _has(norm, "aumenta o volume", "mais alto", "fala mais alto", "volume alto"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_volume_up",
                reply_text="Volume aumentado.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        if _has(norm, "diminui o volume", "mais baixo", "fala mais baixo", "volume baixo",
                "silencio", "fica quieto"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_volume_down",
                reply_text="Volume reduzido.",
                expression_id=_EXPR_ATTENTIVE,
                action_id=_ACTION_NONE,
                emot_event_id=_EMOT_NEUTRAL,
            )

        # -- Sem intent local --------------------------------------------------
        return IntentResolved(turn_id=turn_id, intent_name=None)

    def _match_vision(self, turn_id: int, intent_name: str, formatter) -> IntentResolved:
        reply = _vision_unavailable_reply()
        if self._vision is not None:
            try:
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
