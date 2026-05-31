"""noisebot_server.internal.ops.metrics — formata métricas para o dashboard."""
from __future__ import annotations

from ..agent.metrics import MetricsRegistry
from .status import StatusStore


class MetricsApi:
    """Serializa MetricsRegistry + StatusStore para o endpoint GET /ai/metrics."""

    def __init__(self, registry: MetricsRegistry, store: StatusStore) -> None:
        self._registry = registry
        self._store = store

    def get_metrics(self) -> dict:
        snap = self._registry.snapshot()

        def _lat(key: str) -> dict | None:
            series = snap.get(key)
            if series is None:
                return None
            p50 = series.get("p50_ms")
            p95 = series.get("p95_ms")
            if p50 is None and p95 is None:
                return None
            return {
                "p50": _round(p50),
                "p95": _round(p95),
                "count": series.get("count", 0),
            }

        latency_ms = {}
        for key, label in [
            ("stt_ms",                   "stt"),
            ("llm_first_token_ms",       "llm_first_token"),
            ("llm_total_ms",             "llm_total"),
            ("tts_first_audio_ms",       "tts_first_audio"),
            ("first_audio_out_ms",       "first_audio_out"),
            ("first_robot_reaction_ms",  "first_robot_reaction"),
            ("interruption_cancel_ms",   "interruption_cancel"),
        ]:
            entry = _lat(key)
            if entry is not None:
                latency_ms[label] = entry

        # Tokens e custo: null por enquanto (providers não fornecem ainda)
        # Será preenchido quando LlmReplyComplete incluir usage.
        token_series = snap.get("input_tokens")
        input_total = _total_from_snapshot(token_series)
        output_series = snap.get("output_tokens")
        output_total = _total_from_snapshot(output_series)

        last_voice_session = self._store.last_voice_session.copy()
        voice_alert = _voice_alert(last_voice_session)

        return {
            "latency_ms": latency_ms,
            "turns": self._store.turn_counters.copy(),
            "tokens": {
                "input": input_total,
                "output": output_total,
            },
            "last_voice_session": last_voice_session,
            "recent_voice_sessions": self._store.recent_voice_sessions,
            "voice_alert": voice_alert,
            "voice_diagnosis": _voice_diagnosis(last_voice_session, voice_alert),
            "estimated_cost": None,  # preenchido quando providers fornecerem uso
        }

    def reset(self) -> None:
        """Zera as métricas agregadas (não apaga logs estruturados)."""
        self._registry.clear()
        for k in self._store.turn_counters:
            self._store.turn_counters[k] = 0
        self._store.clear_voice_sessions()


def _round(v: float | None) -> float | None:
    if v is None:
        return None
    return round(v, 1)


def _total_from_snapshot(series: dict | None) -> int | None:
    if not series:
        return None
    mean = series.get("mean_ms")
    count = series.get("count", 0)
    if mean is None or not count:
        return None
    return int(mean * count)


def _voice_alert(session: dict) -> dict | None:
    if not session:
        return None
    outcome = str(session.get("outcome") or "")
    discard = session.get("discard_reason")
    stage = session.get("error_stage")
    reason = session.get("error_reason") or discard or stage
    if outcome == "failed" or stage:
        return {
            "level": "error",
            "title": "Turno de voz falhou",
            "detail": str(reason or "erro não detalhado"),
        }
    if outcome in {"audio_rejected", "stt_rejected", "cancelled"} or discard:
        return {
            "level": "warn",
            "title": "Turno de voz descartado",
            "detail": str(reason or outcome or "sem motivo detalhado"),
        }
    quality = session.get("transcript_quality")
    if quality and str(quality).lower() not in {"ok", "good"}:
        return {
            "level": "warn",
            "title": "Transcrição com baixa confiança",
            "detail": str(quality),
        }
    if session.get("tts_completed") is False:
        return {
            "level": "warn",
            "title": "Fala possivelmente incompleta",
            "detail": "TTS/playback não confirmou SAY_END",
        }
    return None


def _voice_diagnosis(session: dict, alert: dict | None) -> dict | None:
    if not session:
        return None
    reason = str(
        session.get("error_reason")
        or session.get("discard_reason")
        or session.get("transcript_quality")
        or session.get("outcome")
        or ""
    )
    lower_reason = reason.lower()
    title = alert["title"] if alert else "Turno de voz concluído"
    detail = alert["detail"] if alert else "sem alerta"
    next_check = "Comparar duração, qualidade STT e latência até o primeiro áudio."

    if "audio_curto" in lower_reason:
        detail = "áudio útil abaixo do mínimo para STT"
        next_check = "Verificar wake sem fala, limiar de VAD e distância do microfone."
    elif "audio_longo" in lower_reason:
        detail = "fala excedeu o teto de 10 s antes do STT"
        next_check = "Confirmar encerramento por silêncio e evitar sessão presa aberta."
    elif "backpressure" in lower_reason:
        detail = "bridge congestionado descartou áudio antes do STT"
        next_check = "Verificar fila TCP, carga do server e pacing dos chunks."
    elif "chunk_invalido" in lower_reason or "invalid_chunk" in lower_reason:
        detail = "chunk fora do contrato PCM16/256 samples"
        next_check = "Conferir contrato HELLO e tamanho dos frames no firmware."
    elif "stt" in lower_reason or lower_reason in {"empty", "low_confidence", "bad"}:
        detail = "STT rejeitou ou degradou a transcrição"
        next_check = "Comparar RMS, peak, clipping e amostra enviada ao STT."
    elif "barge" in lower_reason or session.get("outcome") == "interrupted":
        detail = "turno interrompido por barge-in"
        next_check = "Confirmar que SPEECH_CANCEL drenou áudio antigo e abriu turno limpo."
    elif session.get("tts_completed") is False:
        detail = "TTS/playback não confirmou envio completo de fala"
        next_check = "Checar chunks SAY, SAY_BEGIN/SAY_END e cancelamentos durante playback."
    elif session.get("text_scroll_truncated") is True:
        pages = session.get("text_scroll_pages")
        pages_sent = session.get("text_scroll_pages_sent")
        if isinstance(pages, int) and isinstance(pages_sent, int) and pages_sent >= pages > 1:
            detail = "texto visual longo foi paginado em TEXT_SCROLL; áudio pode estar completo"
            next_check = "Confirmar no display se as páginas apareceram durante a fala."
        else:
            detail = "texto visual foi truncado pelo limite de TEXT_SCROLL; áudio pode estar completo"
            next_check = "Comparar reply_chars com tts_completed e duração esperada de fala."
    elif session.get("tts_completed") is True:
        detail = "TTS/playback confirmou SAY_END para o turno"
        next_check = "Se o áudio pareceu cortar, investigar fila/playback no firmware ou alto-falante."
    elif session.get("outcome") == "failed" or session.get("error_stage"):
        detail = detail or "erro no pipeline de voz"
        next_check = "Abrir eventos de sessão e checar o estágio indicado."

    return {
        "title": title,
        "detail": detail,
        "next_check": next_check,
    }
