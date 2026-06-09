import { SendHorizontal } from "lucide-react";
import type { DashboardSnapshot } from "../api";
import { cardClass, primaryButtonClass, inputClass } from "../lib/classes";
import { lastReplyText } from "../lib/format";
import { TurnBubble } from "../components/TurnBubble";

export function InteractionView({
  commandStatus,
  commandText,
  onCommandChange,
  onCommandSubmit,
  snapshot,
}: {
  commandStatus: string;
  commandText: string;
  onCommandChange: (value: string) => void;
  onCommandSubmit: () => void;
  snapshot: DashboardSnapshot;
}) {
  return (
    <div className="grid gap-4">
      <section className={cardClass}>
        <h2 className="mb-4 text-lg font-semibold text-white">Última conversa</h2>
        <div className="grid gap-3 sm:grid-cols-2">
          <TurnBubble
            label="Última fala"
            text={snapshot.robot.lastTranscript || "Sem transcrição recente."}
          />
          <TurnBubble label="Última resposta" text={lastReplyText(snapshot)} />
        </div>
      </section>

      <section className={cardClass}>
        <h2 className="mb-4 text-lg font-semibold text-white">Enviar comando</h2>
        <form
          className="grid gap-3 sm:grid-cols-[minmax(0,1fr)_auto]"
          onSubmit={(e) => {
            e.preventDefault();
            void onCommandSubmit();
          }}
        >
          <input
            className={inputClass}
            onChange={(e) => onCommandChange(e.target.value)}
            placeholder="Digite algo para o NoiseBot responder…"
            value={commandText}
          />
          <button className={primaryButtonClass} type="submit">
            <SendHorizontal size={17} />
            Enviar
          </button>
        </form>
        {commandStatus !== "pronto" && (
          <p className="mt-3 text-sm text-slate-400">{commandStatus}</p>
        )}
      </section>
    </div>
  );
}
