import type { VoiceSessionSummary } from "../api";
import { numberValue, textValue } from "../lib/format";
import { voiceOutcomeClass } from "../lib/voice";
import { InfoRow } from "./InfoRow";

export function VoiceSessionHistory({
  sessions,
}: {
  sessions: VoiceSessionSummary[];
}) {
  if (sessions.length === 0) {
    return (
      <p className="rounded-lg border border-dashed border-white/10 p-4 text-sm text-slate-400">
        Nenhuma sessão de voz registrada ainda.
      </p>
    );
  }
  return (
    <div className="grid gap-2">
      {sessions.slice(0, 8).map((session, index) => (
        <article
          className="rounded-lg border border-white/10 bg-white/[0.03] p-3"
          key={`${session.turn_id ?? "turn"}-${index}`}
        >
          <div className="flex flex-wrap items-center justify-between gap-2">
            <strong className="text-sm text-slate-100">
              Turno {session.turn_id ?? "--"}
            </strong>
            <span className={voiceOutcomeClass(session.outcome)}>
              {session.outcome || "--"}
            </span>
          </div>
          {(session.transcript || session.reply) && (
            <div className="mt-2 grid gap-2 text-sm text-slate-300">
              {session.transcript && (
                <p>
                  <strong className="text-slate-100">Ouvi:</strong>{" "}
                  {session.transcript}
                </p>
              )}
              {session.reply && (
                <p>
                  <strong className="text-slate-100">Respondi:</strong>{" "}
                  {session.reply}
                </p>
              )}
            </div>
          )}
          <div className="mt-2 grid gap-2 text-sm text-slate-300 md:grid-cols-2">
            <InfoRow label="Duração" value={numberValue(session.duration_ms, " ms")} />
            <InfoRow label="Chunks" value={numberValue(session.chunk_count, "")} />
            <InfoRow label="Qualidade" value={textValue(session.transcript_quality)} />
            <InfoRow label="Descarte" value={textValue(session.discard_reason)} />
          </div>
        </article>
      ))}
    </div>
  );
}
