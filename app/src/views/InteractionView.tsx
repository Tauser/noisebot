import { useState } from "react";
import { SendHorizontal } from "lucide-react";
import type { DashboardSnapshot } from "../api";
import { cardClass, primaryButtonClass, inputClass } from "../lib/classes";
import { lastReplyText } from "../lib/format";
import { TurnBubble } from "../components/TurnBubble";

/* ─── Command Reference data ────────────────────────────────────── */

const LOCAL_INTENTS = [
  { cat: "Controle",    examples: ["pare", "cancela", "silêncio", "fica quieto"] },
  { cat: "Alertas",     examples: ["silencia o alarme", "para de tocar", "desliga o alarme"] },
  { cat: "Timers",      examples: ["timer de 5 minutos", "timer chamado X de 10 min", "cancela o timer"] },
  { cat: "Lembretes",   examples: ["me lembre de Y daqui a 10 minutos", "cancela o lembrete"] },
  { cat: "Alarmes",     examples: ["alarme às 7h", "alarme às 8 e meia", "quais alarmes ativos"] },
  { cat: "Hora / Data", examples: ["que horas são", "que dia é hoje"] },
  { cat: "Clima",       examples: ["temperatura agora", "como está o tempo hoje"] },
  { cat: "Volume",      examples: ["volume 50%", "mais alto", "fala mais baixo"] },
  { cat: "LEDs",        examples: ["luz azul", "led vermelho", "brilho do led 80%", "luz normal"] },
  { cat: "Visão",       examples: ["o que você está vendo", "está me vendo", "como está a luz", "tem movimento"] },
  { cat: "Expressões",  examples: ["fique feliz", "fique curioso", "fique focado"] },
  { cat: "Olhar",       examples: ["olha pra cima", "olha pra baixo", "olha pra esquerda", "olha pra direita"] },
  { cat: "Humor",       examples: ["como você está", "tudo bem", "está feliz"] },
  { cat: "Status",      examples: ["seu status", "barra de status", "está conectado"] },
  { cat: "Curiosidade", examples: ["me conte uma curiosidade", "fale uma curiosidade"] },
  { cat: "Saudações",   examples: ["oi", "bom dia", "boa tarde", "tchau", "até logo"] },
];

const TOOLS = [
  { name: "set_expression",          desc: "Expressão facial imediata.",                    args: "expression_id*: neutral · happy · curious · sleepy · focused · suspicious · surprised · sad · alarmed · angry" },
  { name: "set_led",                 desc: "Cor e brilho dos LEDs WS2812.",                args: "color* (#RRGGBB ou nome: red, green, blue, white, yellow, off) · brightness 0–100" },
  { name: "create_timer",            desc: "Cria temporizador com aviso ao expirar.",       args: "duration_s* 1–86400 · label (opcional)" },
  { name: "create_reminder",         desc: "Lembrete falado no horário definido.",          args: 'text* · trigger_iso* ex: "2026-06-10T15:30:00"' },
  { name: "analyze_vision",          desc: "Captura e analisa o que a câmera vê agora.",   args: "sem argumentos" },
  { name: "show_message",            desc: "Exibe texto curto no display via scroll.",      args: "text* (máx 80 chars)" },
  { name: "list_agenda",             desc: "Lista timers, lembretes e alarmes ativos.",     args: "sem argumentos" },
  { name: "remember_fact",           desc: "Memoriza fato persistente sobre o usuário.",    args: "text* · chamar request_confirmation antes" },
  { name: "forget_fact",             desc: "Remove fato memorizado (irreversível).",        args: "fact_id* · chamar request_confirmation antes" },
  { name: "recall_user_preferences", desc: "Recupera todos os fatos memorizados.",          args: "sem argumentos" },
  { name: "web_search",              desc: "Pesquisa atual na web.",                        args: "query* · mode: auto | general | factual | news | technical · max_results 1–8" },
  { name: "request_confirmation",    desc: "Confirmação explícita antes de ação sensível.", args: "question* · action_description" },
];

function CommandReference() {
  const [open, setOpen] = useState(false);
  return (
    <section className={cardClass}>
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        className="flex w-full items-center justify-between text-left"
      >
        <h2 className="text-lg font-semibold text-white">Referência de Comandos</h2>
        <span className="text-slate-400 text-sm">{open ? "▲" : "▼"}</span>
      </button>

      {open && (
        <div className="mt-4 grid gap-6 sm:grid-cols-2">
          {/* Intents Locais */}
          <div>
            <p className="mb-3 text-[10px] font-bold uppercase tracking-[0.14em] text-slate-500">
              Intents Locais (sem LLM)
            </p>
            <div className="flex flex-col gap-1.5">
              {LOCAL_INTENTS.map(({ cat, examples }) => (
                <div
                  key={cat}
                  className="grid grid-cols-[100px_1fr] gap-2 rounded border border-slate-700 bg-slate-800/60 px-3 py-2"
                >
                  <span className="text-[10px] font-bold uppercase tracking-wide text-blue-400 self-start pt-0.5">
                    {cat}
                  </span>
                  <span className="text-[11px] text-slate-300 leading-relaxed">
                    {examples.map((ex, i) => (
                      <span key={ex}>
                        {i > 0 && <span className="text-slate-600"> · </span>}
                        <span className="font-mono">"{ex}"</span>
                      </span>
                    ))}
                  </span>
                </div>
              ))}
            </div>
          </div>

          {/* Function Calling */}
          <div>
            <p className="mb-3 text-[10px] font-bold uppercase tracking-[0.14em] text-slate-500">
              Function Calling (LLM → robô)
            </p>
            <div className="flex flex-col gap-1.5">
              {TOOLS.map(({ name, desc, args }) => (
                <div
                  key={name}
                  className="rounded border border-slate-700 bg-slate-800/60 px-3 py-2"
                >
                  <span className="font-mono text-[12px] font-bold text-sky-300">{name}</span>
                  <p className="mt-0.5 text-[12px] text-slate-300">{desc}</p>
                  <p className="mt-0.5 text-[11px] text-slate-500">{args}</p>
                </div>
              ))}
            </div>
          </div>
        </div>
      )}
    </section>
  );
}

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

      <CommandReference />
    </div>
  );
}
