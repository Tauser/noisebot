import { useEffect, useMemo, useRef, useState } from "react";
import {
  Bot, CircleAlert, ExternalLink, FileText, Globe, Image as ImageIcon, Paperclip,
  SendHorizontal, UserRound, Volume2, X,
} from "lucide-react";
import type {
  DashboardSnapshot, InteractionResponseMode, LlmTurnDebug, SearchMeta,
  SearchSource, VoiceSessionSummary,
} from "../api";
import { loadInteractionAttachment } from "../api";
import type { PendingCommand } from "../hooks/useAppState";
import { cardClass, inputClass } from "../lib/classes";
import { lastReplyText } from "../lib/format";

type TurnSearch = { sources: SearchSource[]; search: SearchMeta | null };

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
  llmDebug,
  onCommandChange,
  onCommandSubmit,
  opsToken,
  snapshot,
}: {
  commandStatus: string;
  commandText: string;
  pendingCommand: PendingCommand | null;
  sessions: VoiceSessionSummary[];
  llmDebug?: LlmTurnDebug[];
  onCommandChange: (value: string) => void;
  onCommandSubmit: (
    attachment?: File | null,
    responseMode?: InteractionResponseMode,
  ) => void | Promise<void>;
  opsToken: string;
  snapshot: DashboardSnapshot;
}) {
  const [attachment, setAttachment] = useState<File | null>(null);
  const [attachmentError, setAttachmentError] = useState("");
  const [responseMode, setResponseMode] = useState<InteractionResponseMode>("dashboard");
  const fileInputRef = useRef<HTMLInputElement>(null);
  const imagePreviewUrl = useMemo(
    () => attachment?.type.startsWith("image/")
      ? URL.createObjectURL(attachment)
      : "",
    [attachment],
  );
  useEffect(() => () => {
    if (imagePreviewUrl) URL.revokeObjectURL(imagePreviewUrl);
  }, [imagePreviewUrl]);
  useEffect(() => {
    if (commandStatus === "preparando") {
      setAttachment(null);
      setAttachmentError("");
    }
  }, [commandStatus]);

  // Mapa turn_id -> dados de busca web (fontes + métricas), vindos de /ai/llm/debug.
  const searchByTurn = useMemo(() => {
    const map = new Map<number, TurnSearch>();
    for (const turn of llmDebug ?? []) {
      const sources = turn.sources ?? [];
      const search = turn.search ?? null;
      if (sources.length > 0 || search) {
        map.set(turn.turn_id, { sources, search });
      }
    }
    return map;
  }, [llmDebug]);

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
        attachment_name: pendingCommand.attachmentName,
        attachment_type: pendingCommand.attachmentType,
        response_mode: pendingCommand.responseMode,
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

  const isSending = (
    commandStatus === "enviando"
    || commandStatus === "analisando imagem"
    || commandStatus === "lendo documento"
    || commandStatus === "preparando"
  );
  const hasError = commandStatus !== "pronto" && !isSending;
  const selectAttachment = (file?: File) => {
    if (!file) return;
    const extension = file.name.split(".").pop()?.toLowerCase() ?? "";
    const isImage = ["image/jpeg", "image/png", "image/webp"].includes(file.type);
    const isDocument = ["pdf", "docx", "txt"].includes(extension);
    if (!isImage && !isDocument) {
      setAttachmentError("Use JPEG, PNG, WebP, PDF, DOCX ou TXT.");
      return;
    }
    const maxBytes = isImage ? 5_000_000 : 10_000_000;
    if (file.size > maxBytes) {
      setAttachmentError(
        isImage
          ? "A imagem deve ter no máximo 5 MB."
          : "O documento deve ter no máximo 10 MB.",
      );
      return;
    }
    setAttachment(file);
    setAttachmentError("");
  };

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
                      attachmentName={session.attachment_name}
                      attachmentType={session.attachment_type}
                      responseMode={session.response_mode}
                      turnId={session.turn_id}
                      opsToken={opsToken}
                    />
                  )}
                  {session.reply && (
                    <ChatMessage
                      icon="robot"
                      label={snapshot.robot.name}
                      meta={session.intent_name || session.route || session.outcome}
                      metrics={formatTurnMetrics(session)}
                      text={session.reply}
                      search={session.turn_id != null ? searchByTurn.get(session.turn_id) : undefined}
                    />
                  )}
                </div>
              ))}
              {isSending && (
                <div className="flex items-center gap-2 pl-11 text-xs text-slate-400">
                  <span className="h-1.5 w-1.5 animate-pulse rounded-full bg-blue-400" />
                  {commandStatus === "enviando"
                    ? "Enviando sua mensagem"
                    : commandStatus === "analisando imagem"
                      ? "Analisando a imagem no servidor local"
                    : commandStatus === "lendo documento"
                      ? "Extraindo o documento no servidor local"
                    : "Aguarde, estou preparando sua resposta"}
                </div>
              )}
            </div>
          )}
        </div>

        <footer className="border-t border-white/10 bg-black/[0.08] p-3 sm:p-4">
          {attachment && (
            <div className="mx-auto mb-2 flex max-w-3xl items-center gap-3 rounded-lg border border-white/10 bg-black/[0.14] p-2">
              {imagePreviewUrl ? (
                <img
                  alt="Pré-visualização do anexo"
                  className="h-14 w-14 rounded-md object-cover"
                  src={imagePreviewUrl}
                />
              ) : (
                <span className="flex h-14 w-14 items-center justify-center rounded-md bg-blue-500/10 text-blue-200">
                  <FileText size={24} />
                </span>
              )}
              <div className="min-w-0 flex-1">
                <p className="truncate text-xs font-semibold text-slate-200">{attachment.name}</p>
                <p className="text-[11px] text-slate-500">
                  {(attachment.size / 1024).toFixed(0)} KB · processado somente no servidor
                </p>
              </div>
              <button
                aria-label="Remover anexo"
                className="flex h-8 w-8 items-center justify-center rounded-md text-slate-400 hover:bg-white/5 hover:text-white"
                onClick={() => setAttachment(null)}
                type="button"
              >
                <X size={15} />
              </button>
            </div>
          )}
          <form
            className="mx-auto grid max-w-3xl gap-2"
            onSubmit={(e) => {
              e.preventDefault();
              void onCommandSubmit(attachment, responseMode);
            }}
          >
            <div className="flex items-center gap-2">
              <input
                accept="image/jpeg,image/png,image/webp,.pdf,.docx,.txt"
                className="hidden"
                onChange={(e) => {
                  selectAttachment(e.target.files?.[0]);
                  e.target.value = "";
                }}
                ref={fileInputRef}
                type="file"
              />
              <button
                aria-label="Anexar arquivo"
                className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg border border-white/10 bg-black/[0.12] text-slate-400 transition hover:text-white"
                onClick={() => fileInputRef.current?.click()}
                title="Anexar imagem ou documento"
                type="button"
              >
                <Paperclip size={17} />
              </button>
              <input
                aria-label="Mensagem para o NoiseBot"
                className={`${inputClass} min-w-0 flex-1`}
                onChange={(e) => onCommandChange(e.target.value)}
                placeholder={
                  attachment
                    ? "O que deseja saber sobre este anexo?"
                    : `Mensagem para ${snapshot.robot.name}`
                }
                value={commandText}
              />
              <button
                aria-label="Enviar mensagem"
                className="flex h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-blue-600 text-white transition hover:bg-blue-500 disabled:cursor-not-allowed disabled:opacity-40"
                disabled={(!commandText.trim() && !attachment) || isSending}
                title="Enviar mensagem"
                type="submit"
              >
                <SendHorizontal size={18} />
              </button>
            </div>
            <label className="flex w-fit items-center gap-2 text-[11px] text-slate-400">
              {responseMode === "dashboard" ? <ImageIcon size={13} /> : <Volume2 size={13} />}
              Resposta
              <select
                className="rounded-md border border-white/10 bg-[#182036] px-2 py-1 text-[11px] text-slate-200 outline-none"
                onChange={(e) => setResponseMode(e.target.value as InteractionResponseMode)}
                value={responseMode}
              >
                <option value="dashboard">somente no dashboard</option>
                <option value="robot">dashboard e robô</option>
              </select>
            </label>
          </form>
          {attachmentError && (
            <p className="mx-auto mt-2 flex max-w-3xl items-center gap-2 text-xs text-amber-300">
              <CircleAlert size={14} />
              {attachmentError}
            </p>
          )}
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
  search,
  attachmentName,
  attachmentType,
  responseMode,
  turnId,
  opsToken,
}: {
  icon: "user" | "robot";
  label: string;
  meta?: string | null;
  metrics?: string | null;
  text: string;
  search?: TurnSearch;
  attachmentName?: string | null;
  attachmentType?: string | null;
  responseMode?: string | null;
  turnId?: number;
  opsToken?: string;
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
          <div className="inline-grid gap-1.5 text-left">
            {attachmentName && attachmentType?.startsWith("image/") && turnId && opsToken && (
              <InteractionImage
                attachmentName={attachmentName}
                opsToken={opsToken}
                turnId={turnId}
              />
            )}
            {attachmentName && !attachmentType?.startsWith("image/") && turnId && opsToken ? (
              <InteractionDocument
                attachmentName={attachmentName}
                opsToken={opsToken}
                turnId={turnId}
              />
            ) : attachmentName && (
              <span className="inline-flex items-center gap-1.5 rounded-md bg-blue-500/20 px-2 py-1 text-[11px] text-blue-100">
                <ImageIcon size={12} />
                {attachmentName}
              </span>
            )}
            <p className="whitespace-pre-wrap rounded-lg bg-blue-600 px-3 py-2 text-sm leading-6 text-white">
              {text}
            </p>
            {responseMode === "dashboard" && (
              <span className="text-right text-[10px] text-slate-500">resposta silenciosa</span>
            )}
          </div>
        ) : (
          <>
            <p className="w-fit whitespace-pre-wrap rounded-lg bg-black/[0.18] px-3 py-2 text-left text-sm leading-6 text-slate-200">
              {text}
            </p>
            {search && (search.sources.length > 0 || search.search) && (
              <SearchSources search={search} />
            )}
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

function InteractionImage({
  attachmentName,
  opsToken,
  turnId,
}: {
  attachmentName: string;
  opsToken: string;
  turnId: number;
}) {
  const [url, setUrl] = useState("");
  const [unavailable, setUnavailable] = useState(false);

  useEffect(() => {
    let active = true;
    let objectUrl = "";
    setUnavailable(false);
    void loadInteractionAttachment(turnId, opsToken)
      .then((blob) => {
        if (!active) return;
        objectUrl = URL.createObjectURL(blob);
        setUrl(objectUrl);
      })
      .catch(() => {
        if (active) setUnavailable(true);
      });
    return () => {
      active = false;
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [opsToken, turnId]);

  if (unavailable) return null;
  if (!url) {
    return <span className="h-36 w-56 animate-pulse rounded-lg bg-white/[0.06]" />;
  }
  return (
    <a href={url} rel="noreferrer" target="_blank" title={`Abrir ${attachmentName}`}>
      <img
        alt={attachmentName}
        className="max-h-72 w-full max-w-sm rounded-lg border border-white/10 bg-black/20 object-contain"
        src={url}
      />
    </a>
  );
}

function InteractionDocument({
  attachmentName,
  opsToken,
  turnId,
}: {
  attachmentName: string;
  opsToken: string;
  turnId: number;
}) {
  const [url, setUrl] = useState("");

  useEffect(() => {
    let active = true;
    let objectUrl = "";
    void loadInteractionAttachment(turnId, opsToken)
      .then((blob) => {
        if (!active) return;
        objectUrl = URL.createObjectURL(blob);
        setUrl(objectUrl);
      })
      .catch(() => undefined);
    return () => {
      active = false;
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [opsToken, turnId]);

  const className = "inline-flex items-center gap-1.5 rounded-md bg-blue-500/20 px-2 py-1 text-[11px] text-blue-100";
  if (!url) {
    return (
      <span className={className}>
        <FileText size={12} />
        {attachmentName}
      </span>
    );
  }
  return (
    <a
      className={`${className} hover:bg-blue-500/30`}
      download={attachmentName}
      href={url}
      title={`Abrir ${attachmentName}`}
    >
      <FileText size={12} />
      {attachmentName}
    </a>
  );
}

function SearchSources({ search }: { search: TurnSearch }) {
  const { sources, search: meta } = search;
  return (
    <div className="mt-2 rounded-lg border border-white/10 bg-black/[0.12] px-3 py-2">
      <div className="mb-1.5 flex items-center gap-1.5 text-[10px] font-bold uppercase tracking-[0.12em] text-slate-400">
        <Globe size={12} className="text-blue-300" />
        Pesquisa na web
        {formatSearchMeta(meta) && (
          <span className="font-normal normal-case tracking-normal text-slate-500">
            · {formatSearchMeta(meta)}
          </span>
        )}
      </div>
      {sources.length > 0 && (
        <ol className="grid gap-1.5">
          {sources.map((src, i) => (
            <li key={src.url || i} className="grid grid-cols-[1.1rem_1fr] gap-1.5 text-xs">
              <span className="pt-0.5 text-right font-mono text-[10px] text-slate-500">
                {i + 1}.
              </span>
              <div className="min-w-0">
                <a
                  href={src.url}
                  target="_blank"
                  rel="noopener noreferrer"
                  className="inline-flex items-center gap-1 font-medium text-sky-300 hover:text-sky-200 hover:underline"
                  title={src.url}
                >
                  <span className="truncate">{src.title || src.source || src.url}</span>
                  <ExternalLink size={11} className="shrink-0 opacity-70" />
                </a>
                <div className="flex flex-wrap items-center gap-x-2 text-[10px] text-slate-500">
                  {src.source && <span>{src.source}</span>}
                  {src.published && <span>· {src.published}</span>}
                  {src.score != null && <span>· relevância {Math.round(src.score * 100)}%</span>}
                </div>
                {src.snippet && (
                  <p className="mt-0.5 line-clamp-2 text-[11px] leading-snug text-slate-400">
                    {src.snippet}
                  </p>
                )}
              </div>
            </li>
          ))}
        </ol>
      )}
    </div>
  );
}

function formatSearchMeta(meta: SearchMeta | null): string | null {
  if (!meta) return null;
  const parts: string[] = [];
  if (meta.mode && meta.mode !== "auto") parts.push(meta.mode);
  if (meta.depth) parts.push(meta.depth);
  if (meta.result_count != null) {
    parts.push(`${meta.result_count} resultado${meta.result_count === 1 ? "" : "s"}`);
  }
  if (meta.cached) parts.push("cache");
  return parts.length > 0 ? parts.join(" · ") : null;
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
