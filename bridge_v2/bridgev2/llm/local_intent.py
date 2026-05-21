"""bridgev2.llm.local_intent -- LocalIntentProvider: intents PT-BR determinísticos.

Portado de noisebot_bridge/intent_router.py para bridgev2 standalone.
Sem dependencia do bridge v1. Latencia < 5 ms. Nenhuma chamada de rede.
"""
from __future__ import annotations

import re
import unicodedata
from datetime import datetime

from ..runtime.events import IntentResolved


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


# -- Replies de tempo --------------------------------------------------------

def _time_reply(now: datetime) -> str:
    hour = now.hour
    minute = now.minute
    date_suffix = f" Sao Paulo, {now:%d/%m/%Y}."
    if hour == 0 and minute == 0:
        return f"Agora e meia-noite.{date_suffix}"
    verb = "e" if hour == 1 else "sao"
    hour_unit = "hora" if hour == 1 else "horas"
    if minute == 0:
        return f"Agora {verb} {hour} {hour_unit}.{date_suffix}"
    minute_unit = "minuto" if minute == 1 else "minutos"
    return f"Agora {verb} {hour} {hour_unit} e {minute:02d} {minute_unit}.{date_suffix}"


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

        # -- Status do sistema -------------------------------------------------
        if _has(norm, "como voce esta", "como vc esta", "tudo bem", "tudo certo",
                "como esta", "seu status", "status do sistema"):
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

        # -- Humor/sentimento do robo ------------------------------------------
        if _has(norm, "como voce se sente", "esta feliz", "esta triste",
                "seu humor", "esta bem", "se sente"):
            return IntentResolved(
                turn_id=turn_id,
                intent_name="local_mood",
                reply_text="Estou bem, obrigado por perguntar!",
                expression_id=_EXPR_HAPPY,
                action_id=_ACTION_NOD,
                emot_event_id=_EMOT_HAPPY,
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
