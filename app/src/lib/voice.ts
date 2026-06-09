import type { VoiceSessionSummary } from "../api";
import { readNumber, textValue, numberValue } from "./format";

export function summarizeVoiceSession(session: VoiceSessionSummary) {
  const outcome = session.outcome || "";
  const discard = session.discard_reason || "";
  const quality = (session.transcript_quality || "").toLowerCase();
  if (!session.turn_id) {
    return {
      label: "sem turno recente",
      className: "rounded-full border border-white/10 bg-white/5 px-3 py-1 text-sm font-bold text-slate-400",
    };
  }
  if (outcome === "failed" || session.error_stage) {
    return {
      label: `falhou: ${session.error_stage || session.error_reason || "erro"}`,
      className: "rounded-full border border-rose-400/20 bg-rose-400/10 px-3 py-1 text-sm font-bold text-rose-300",
    };
  }
  if (outcome === "audio_rejected" || discard === "audio_curto" || discard === "audio_longo") {
    return {
      label: `não ouvi direito: ${discard || outcome}`,
      className: "rounded-full border border-amber-400/20 bg-amber-400/10 px-3 py-1 text-sm font-bold text-amber-300",
    };
  }
  if (
    outcome === "stt_rejected" ||
    discard.startsWith("stt_") ||
    (quality && quality !== "good" && quality !== "ok")
  ) {
    return {
      label: "não entendeu e pediu repetição",
      className: "rounded-full border border-amber-400/20 bg-amber-400/10 px-3 py-1 text-sm font-bold text-amber-300",
    };
  }
  if (outcome === "interrupted" || outcome === "cancelled") {
    return {
      label: "interrompido",
      className: "rounded-full border border-sky-400/20 bg-sky-400/10 px-3 py-1 text-sm font-bold text-sky-300",
    };
  }
  if (session.reply || session.reply_chars) {
    return {
      label: "respondeu",
      className: "rounded-full border border-emerald-400/20 bg-emerald-400/10 px-3 py-1 text-sm font-bold text-emerald-300",
    };
  }
  return {
    label: outcome || "sem resposta",
    className: "rounded-full border border-white/10 bg-white/5 px-3 py-1 text-sm font-bold text-slate-400",
  };
}

export function voiceStageState(
  session: VoiceSessionSummary,
  stage: "audio" | "stt" | "decision" | "reply",
): "ok" | "warn" | "error" | "idle" {
  const outcome = session.outcome || "";
  const discard = session.discard_reason || "";
  const quality = (session.transcript_quality || "").toLowerCase();
  if (!session.turn_id) return "idle";
  if (session.error_stage) {
    if (stage === "reply" || session.error_stage === stage) return "error";
  }
  if (stage === "audio") {
    if (
      outcome === "audio_rejected" ||
      discard === "audio_curto" ||
      discard === "audio_longo"
    )
      return "warn";
    return session.total_samples ? "ok" : "idle";
  }
  if (stage === "stt") {
    if (
      outcome === "stt_rejected" ||
      discard.startsWith("stt_") ||
      (quality && quality !== "good" && quality !== "ok")
    )
      return "warn";
    return session.transcript_quality || session.transcript ? "ok" : "idle";
  }
  if (stage === "decision") {
    if (outcome === "failed") return "error";
    return session.intent_name || outcome ? "ok" : "idle";
  }
  if (outcome === "failed") return "error";
  if (session.reply || session.reply_chars) return "ok";
  if (outcome === "stt_rejected" || discard.startsWith("stt_")) return "warn";
  return "idle";
}

export function voiceStageDetail(
  session: VoiceSessionSummary,
  stage: "audio" | "stt" | "decision" | "reply",
): string {
  if (!session.turn_id) return "aguardando uso";
  if (stage === "audio") {
    return `${numberValue(session.duration_ms, " ms")} · ${numberValue(session.chunk_count, " chunks")}`;
  }
  if (stage === "stt") {
    return `${textValue(session.transcript_quality)} · ${numberValue(session.stt_ms, " ms")}`;
  }
  if (stage === "decision") {
    return `${textValue(session.intent_name || session.outcome)} · ${numberValue(session.end_of_turn_ms, " ms")}`;
  }
  if (session.reply || session.reply_chars) {
    return `${numberValue(session.first_audio_after_voice_end_ms, " ms")} até 1º áudio`;
  }
  return textValue(session.discard_reason || session.error_reason || "sem fala enviada");
}

export function voiceStateLabel(state: "ok" | "warn" | "error" | "idle"): string {
  if (state === "ok") return "ok";
  if (state === "warn") return "atenção";
  if (state === "error") return "erro";
  return "sem dado";
}

export function voiceLatencyBottleneck(session: VoiceSessionSummary) {
  const firstAudio = readNumber(session.first_audio_after_voice_end_ms);
  const stt = readNumber(session.stt_ms);
  const voiceEndToStt = readNumber(session.voice_end_to_stt_start_ms) ?? 0;
  const ttsAudio = readNumber(session.tts_first_audio_ms);
  const postSttToAudio =
    firstAudio !== null && stt !== null
      ? Math.max(0, firstAudio - voiceEndToStt - stt)
      : null;
  const candidates = [
    { key: "STT", value: stt },
    { key: "decisão e TTS", value: postSttToAudio },
    { key: "TTS", value: ttsAudio },
  ].filter((item): item is { key: string; value: number } => item.value !== null);
  if (candidates.length === 0) {
    return {
      label: "sem dados suficientes",
      detail: "faça um teste de voz para medir o ciclo.",
    };
  }
  const highest = candidates.reduce((best, item) =>
    item.value > best.value ? item : best,
  );
  if (highest.value < 1500) {
    return {
      label: "ciclo saudável",
      detail: `${highest.key} foi o maior trecho medido: ${highest.value} ms.`,
    };
  }
  const speechTotal = readNumber(session.speech_total_ms);
  const firstAudioNote =
    firstAudio !== null ? ` Tempo total até 1º áudio: ${firstAudio} ms.` : "";
  const speechNote =
    speechTotal !== null ? ` Fala total: ${speechTotal} ms, apenas informativo.` : "";
  return {
    label: highest.key,
    detail: `maior atraso medido no último turno: ${highest.value} ms.${firstAudioNote}${speechNote}`,
  };
}

export function voiceOutcomeClass(outcome: string | undefined): string {
  if (outcome === "failed")
    return "rounded-full border border-rose-400/20 bg-rose-400/10 px-2 py-1 text-xs font-bold text-rose-300";
  if (
    outcome === "audio_rejected" ||
    outcome === "stt_rejected" ||
    outcome === "cancelled"
  ) {
    return "rounded-full border border-amber-400/20 bg-amber-400/10 px-2 py-1 text-xs font-bold text-amber-300";
  }
  return "rounded-full border border-emerald-400/20 bg-emerald-400/10 px-2 py-1 text-xs font-bold text-emerald-300";
}
