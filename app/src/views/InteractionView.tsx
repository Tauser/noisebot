import { useEffect, useMemo, useRef, useState } from "react";
import {
  Bot, CircleAlert, SendHorizontal, UserRound,
} from "lucide-react";
import type { DashboardSnapshot, VoiceSessionSummary } from "../api";
import type { PendingCommand } from "../hooks/useAppState";
import { cardClass, inputClass } from "../lib/classes";
import { lastReplyText } from "../lib/format";

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
  pendingCommand,
  sessions,
  onCommandChange,
  onCommandSubmit,
  snapshot,
}: {
  commandStatus: string;
  commandText: string;
  pendingCommand: PendingCommand | null;
  sessions: VoiceSessionSummary[];
  onCommandChange: (value: string) => void;
  onCommandSubmit: () => void;
  snapshot: DashboardSnapshot;
}) {
  const conversation = useMemo(() => {
    const complete = sessions
      .filter((session) => session.transcript || session.reply)
      .slice(0, 12)
      .reverse();

    if (
      pendingCommand
      && !complete.some((session) => session.turn_id === pendingCommand.turnId)
    ) {
      complete.push({
        turn_id: pendingCommand.turnId,
        transcript: pendingCommand.text,
        outcome: "processing",
      });
    }

    if (complete.length > 0) return complete;
    if (!snapshot.robot.lastTranscript && !snapshot.robot.lastReply) return [];

    return [{
      turn_id: snapshot.robot.lastTurnId,
      transcript: snapshot.robot.lastTranscript,
      reply: lastReplyText(snapshot),
      route: snapshot.robot.lastRoute,
    }];
  }, [pendingCommand, sessions, snapshot]);

  const feedRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    feedRef.current?.scrollTo({
      top: feedRef.current.scrollHeight,
      behavior: "smooth",
    });
  }, [conversation.length, commandStatus]);

  const isSending = commandStatus === "enviando" || commandStatus === "preparando";
  const hasError = commandStatus !== "pronto" && !isSending;

  return (
    <div className="grid gap-4">
      <section className="overflow-hidden rounded-lg border border-white/10 bg-[#202941]">
        <header className="flex min-h-16 items-center justify-between gap-4 border-b border-white/10 px-4 py-3 sm:px-5">
          <div className="flex min-w-0 items-center gap-3">
            <span className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full bg-blue-500/15 text-blue-300">
              <Bot size={19} />
            </span>
            <div className="min-w-0">
              <h2 className="truncate text-sm font-semibold text-white">
                Conversa com {snapshot.robot.name}
              </h2>
              <p className="truncate text-xs text-slate-400">
                {snapshot.robot.firmwareOnline
                  ? `${snapshot.robot.provider || "local"} · ${snapshot.robot.lastRoute || "pronto"}`
                  : "firmware offline"}
              </p>
            </div>
          </div>
          <span className="flex shrink-0 items-center gap-2 text-xs text-slate-400">
            <span
              className={
                snapshot.robot.firmwareOnline
                  ? "h-2 w-2 rounded-full bg-emerald-400"
                  : "h-2 w-2 rounded-full bg-slate-500"
              }
            />
            {snapshot.robot.firmwareOnline ? "Online" : "Offline"}
          </span>
        </header>

        <div
          className="h-[min(52vh,520px)] min-h-72 overflow-y-auto px-3 py-4 sm:h-[min(56vh,620px)] sm:min-h-80 sm:px-6 sm:py-5"
          ref={feedRef}
        >
          {conversation.length === 0 ? (
            <div className="flex h-full flex-col items-center justify-center text-center">
              <span className="mb-3 flex h-11 w-11 items-center justify-center rounded-full bg-white/[0.05] text-slate-400">
                <Bot size={21} />
              </span>
              <strong className="text-sm text-slate-200">A conversa começa aqui</strong>
              <p className="mt-1 max-w-sm text-sm text-slate-500">
                Fale com o NoiseBot ou envie uma mensagem abaixo.
              </p>
            </div>
          ) : (
            <div className="mx-auto grid max-w-3xl gap-5">
              {conversation.map((session, index) => (
                <div className="grid gap-3" key={`${session.turn_id ?? "turn"}-${index}`}>
                  {session.transcript && (
                    <ChatMessage
                      icon="user"
                      label="Você"
                      meta={session.turn_id ? `Turno ${session.turn_id}` : undefined}
                      text={session.transcript}
                    />
                  )}
                  {session.reply && (
                    <ChatMessage
                      icon="robot"
                      label={snapshot.robot.name}
                      meta={session.intent_name || session.route || session.outcome}
                      metrics={formatTurnMetrics(session)}
                      text={session.reply}
                    />
                  )}
                </div>
              ))}
              {isSending && (
                <div className="flex items-center gap-2 pl-11 text-xs text-slate-400">
                  <span className="h-1.5 w-1.5 animate-pulse rounded-full bg-blue-400" />
                  {commandStatus === "enviando"
                    ? "Enviando sua mensagem"
                    : "Aguarde, estou preparando sua resposta"}
                </div>
              )}
            </div>
          )}
        </div>

        <footer className="border-t border-white/10 bg-black/[0.08] p-3 sm:p-4">
          <form
            className="mx-auto flex max-w-3xl items-center gap-2"
            onSubmit={(e) => {
              e.preventDefault();
              void onCommandSubmit();
            }}
          >
            <input
              aria-label="Mensagem para o NoiseBot"
              className={`${inputClass} min-w-0 flex-1`}
              onChange={(e) => onCommandChange(e.target.value)}
              placeholder={`Mensagem para ${snapshot.robot.name}`}
              value={commandText}
            />
            <button
              aria-label="Enviar mensagem"
              className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-blue-600 text-white transition hover:bg-blue-500 disabled:cursor-not-allowed disabled:opacity-40"
              disabled={!commandText.trim() || commandStatus === "enviando"}
              title="Enviar mensagem"
              type="submit"
            >
              <SendHorizontal size={18} />
            </button>
          </form>
          {hasError && (
            <p className="mx-auto mt-2 flex max-w-3xl items-center gap-2 text-xs text-rose-300">
              <CircleAlert size={14} />
              {commandStatus}
            </p>
          )}
        </footer>
      </section>

      <CommandReference />
    </div>
  );
}

function ChatMessage({
  icon,
  label,
  meta,
  metrics,
  text,
}: {
  icon: "user" | "robot";
  label: string;
  meta?: string | null;
  metrics?: string | null;
  text: string;
}) {
  const isUser = icon === "user";
  const Icon = isUser ? UserRound : Bot;
  return (
    <article className={`flex gap-3 ${isUser ? "flex-row-reverse" : ""}`}>
      <span
        className={
          isUser
            ? "flex h-8 w-8 shrink-0 items-center justify-center rounded-full bg-sky-500/15 text-sky-300"
            : "flex h-8 w-8 shrink-0 items-center justify-center rounded-full bg-blue-500/15 text-blue-300"
        }
      >
        <Icon size={16} />
      </span>
      <div
        className={`min-w-0 max-w-[82%] ${isUser ? "text-right" : ""}`}
      >
        <div className={`mb-1 flex items-center gap-2 ${isUser ? "justify-end" : ""}`}>
          <strong className="text-xs text-slate-300">{label}</strong>
          {meta && <span className="text-[10px] text-slate-500">{meta}</span>}
        </div>
        {isUser ? (
          <p className="inline-block whitespace-pre-wrap rounded-lg bg-blue-600 px-3 py-2 text-left text-sm leading-6 text-white">
            {text}
          </p>
        ) : (
          <>
            <p className="w-fit whitespace-pre-wrap rounded-lg bg-black/[0.18] px-3 py-2 text-left text-sm leading-6 text-slate-200">
              {text}
            </p>
            {metrics && (
              <p className="mt-1.5 text-left text-[10px] text-slate-500">
                {metrics}
              </p>
            )}
          </>
        )}
      </div>
    </article>
  );
}

function formatTurnMetrics(session: VoiceSessionSummary): string | null {
  const metrics: string[] = [];
  if (session.llm_total_ms != null) {
    const seconds = session.llm_total_ms / 1000;
    metrics.push(`Resposta: ${seconds < 10 ? seconds.toFixed(2) : seconds.toFixed(1)} s`);
  }
  const input = session.input_tokens ?? 0;
  const output = session.output_tokens ?? 0;
  if (input > 0 || output > 0) {
    metrics.push(`Tokens: ${input + output} (${input} entrada · ${output} saída)`);
  }
  return metrics.length > 0 ? metrics.join(" · ") : null;
}
