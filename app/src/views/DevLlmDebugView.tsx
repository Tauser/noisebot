import { useEffect, useState } from "react";
import { RefreshCw, ChevronDown, ChevronRight, CheckCircle2, XCircle, AlertTriangle, Clock } from "lucide-react";
import { fetchLlmDebug, type LlmTurnDebug, type LlmToolDebug } from "../api";

/* ─── helpers ─────────────────────────────────────────────────── */

function fmtTime(unix: number): string {
  return new Date(unix * 1000).toLocaleTimeString("pt-BR", {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  });
}

function fmtMs(v: number | undefined): string {
  if (v == null) return "—";
  return v >= 1000 ? `${(v / 1000).toFixed(1)}s` : `${Math.round(v)}ms`;
}

/* ─── Badge ───────────────────────────────────────────────────── */

type BadgeVariant = "ok" | "warn" | "err" | "info" | "neutral";

function Badge({ label, variant }: { label: string; variant: BadgeVariant }) {
  const cls: Record<BadgeVariant, string> = {
    ok:      "bg-emerald-900/60 text-emerald-300 border-emerald-700",
    warn:    "bg-amber-900/60   text-amber-300   border-amber-700",
    err:     "bg-red-900/60     text-red-300     border-red-700",
    info:    "bg-sky-900/60     text-sky-300     border-sky-700",
    neutral: "bg-slate-800      text-slate-300   border-slate-600",
  };
  return (
    <span className={`inline-block rounded border px-1.5 py-0.5 text-[10px] font-medium ${cls[variant]}`}>
      {label}
    </span>
  );
}

/* ─── Raw collapsible ─────────────────────────────────────────── */

function Raw({ label, value }: { label: string; value: string }) {
  const [open, setOpen] = useState(false);
  if (!value) return null;
  return (
    <div className="mt-1">
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        className="flex items-center gap-1 text-[11px] text-slate-400 hover:text-slate-200"
      >
        {open ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        {label}
      </button>
      {open && (
        <pre className="mt-1 max-h-40 overflow-auto rounded bg-slate-950 p-2 text-[10px] text-slate-300 whitespace-pre-wrap break-all">
          {value}
        </pre>
      )}
    </div>
  );
}

/* ─── ToolBlock ───────────────────────────────────────────────── */

function ToolBlock({ tool }: { tool: LlmToolDebug }) {
  const outcomeVariant: BadgeVariant =
    tool.outcome === "success" ? "ok" :
    tool.outcome === "confirmation_required" ? "warn" : "err";

  return (
    <div className="rounded border border-slate-700 bg-slate-800/50 p-2 text-xs">
      <div className="flex flex-wrap items-center gap-2">
        <span className="font-mono text-sky-300">{tool.name}</span>
        <Badge label={tool.outcome} variant={outcomeVariant} />
        {tool.sandboxed && <Badge label="sandbox" variant="warn" />}
        {tool.veto_reason && (
          <span className="italic text-red-400">{tool.veto_reason}</span>
        )}
      </div>
      {Object.keys(tool.arguments).length > 0 && (
        <div className="mt-1 text-slate-400">
          args:{" "}
          {Object.entries(tool.arguments).map(([k, v]) => (
            <span key={k} className="mr-2">
              <span className="text-slate-300">{k}</span>=<span className="text-amber-300">{v}</span>
            </span>
          ))}
        </div>
      )}
      {tool.result_summary && (
        <div className="mt-1 text-emerald-400">resultado: {tool.result_summary}</div>
      )}
    </div>
  );
}

/* ─── TurnCard ────────────────────────────────────────────────── */

function TurnCard({ turn }: { turn: LlmTurnDebug }) {
  const [open, setOpen] = useState(false);

  return (
    <div className="rounded-lg border border-slate-700 bg-slate-800/60">
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        className="flex w-full items-start gap-3 p-3 text-left"
      >
        <span className="mt-0.5 shrink-0 text-slate-500">
          {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        </span>
        <div className="min-w-0 flex-1">
          <div className="flex flex-wrap items-center gap-2">
            <span className="font-mono text-[11px] text-slate-500">#{turn.turn_id}</span>
            <span className="text-[11px] text-slate-500">{fmtTime(turn.ts)}</span>
            <Badge label={turn.turn_payload_summary.robot_state || "?"} variant="neutral" />
            {turn.tool && (
              <Badge
                label={`tool: ${turn.tool.name}`}
                variant={turn.tool.outcome === "success" ? "ok" : "err"}
              />
            )}
            {turn.step2 && <Badge label="2-step" variant="info" />}
            {turn.retry_count > 0 && <Badge label={`retry ×${turn.retry_count}`} variant="warn" />}
            <span className="ml-auto flex items-center gap-0.5 text-[11px] text-slate-400">
              <Clock size={11} />
              {fmtMs(turn.total_latency_ms)}
            </span>
          </div>
          <p className="mt-1 truncate text-sm text-slate-200">{turn.transcript || "—"}</p>
          {!open && turn.final_reply && (
            <p className="mt-0.5 truncate text-xs italic text-slate-400">{turn.final_reply}</p>
          )}
        </div>
      </button>

      {open && (
        <div className="space-y-3 border-t border-slate-700 px-4 pb-4 pt-3 text-xs">

          {/* Context summary */}
          <div>
            <p className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">Contexto</p>
            <div className="flex flex-wrap gap-2 text-slate-300">
              <span>humor: <span className="text-slate-100">{turn.turn_payload_summary.mood || "—"}</span></span>
              {turn.turn_payload_summary.vision_present && <Badge label="visão" variant="info" />}
              {turn.turn_payload_summary.tools_available.length > 0 && (
                <span className="text-slate-400">
                  tools: {turn.turn_payload_summary.tools_available.join(", ")}
                </span>
              )}
            </div>
          </div>

          {/* Step 1 */}
          <div>
            <p className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">
              Passo 1 — {fmtMs(turn.step1?.latency_ms)}
            </p>
            <div className="space-y-1">
              <div className="flex flex-wrap gap-2">
                {turn.step1?.expression_id && (
                  <Badge label={turn.step1.expression_id} variant="neutral" />
                )}
                {turn.step1?.has_tool_call && (
                  <Badge label={`→ ${turn.step1.tool_name ?? "tool"}`} variant="info" />
                )}
              </div>
              {turn.step1?.reply && <p className="text-slate-200">{turn.step1.reply}</p>}
              <Raw label="resposta bruta" value={turn.step1?.raw_response ?? ""} />
            </div>
          </div>

          {/* Tool */}
          {turn.tool && (
            <div>
              <p className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">Tool</p>
              <ToolBlock tool={turn.tool} />
            </div>
          )}

          {/* Step 2 */}
          {turn.step2 && (
            <div>
              <p className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">
                Passo 2 — {fmtMs(turn.step2.latency_ms)}
              </p>
              <div className="space-y-1">
                {turn.step2.expression_id && (
                  <Badge label={turn.step2.expression_id} variant="neutral" />
                )}
                <p className="text-slate-200">{turn.step2.reply}</p>
                <Raw label="resposta bruta" value={turn.step2.raw_response} />
              </div>
            </div>
          )}

          {/* Final reply */}
          <div className="rounded border border-slate-600 bg-slate-900/50 p-2">
            <p className="mb-1 text-[10px] font-semibold uppercase tracking-wide text-slate-400">Resposta Final</p>
            <p className="text-slate-100">{turn.final_reply || "—"}</p>
            <div className="mt-1 flex gap-2 text-[10px] text-slate-500">
              <span>expr: {turn.final_expression_id ?? "null"}</span>
              <span>·</span>
              <span>retry: {turn.retry_count}</span>
              <span>·</span>
              <span>total: {fmtMs(turn.total_latency_ms)}</span>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

/* ─── Main view ───────────────────────────────────────────────── */

export function DevLlmDebugView() {
  const [turns, setTurns] = useState<LlmTurnDebug[]>([]);
  const [loading, setLoading] = useState(false);
  const [lastFetch, setLastFetch] = useState<Date | null>(null);
  const [autoRefresh, setAutoRefresh] = useState(true);

  async function refresh() {
    setLoading(true);
    try {
      const data = await fetchLlmDebug();
      setTurns(data);
      setLastFetch(new Date());
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    void refresh();
  }, []);

  useEffect(() => {
    if (!autoRefresh) return;
    const id = setInterval(() => void refresh(), 3000);
    return () => clearInterval(id);
  }, [autoRefresh]);

  return (
    <div className="flex flex-col gap-4">
      {/* Toolbar */}
      <div className="flex items-center justify-between">
        <div>
          <h2 className="text-base font-semibold text-slate-100">LLM Debug</h2>
          <p className="text-xs text-slate-500">
            Últimos {turns.length} turnos via LLM
            {lastFetch && ` · atualizado ${lastFetch.toLocaleTimeString("pt-BR")}`}
          </p>
        </div>
        <div className="flex items-center gap-2">
          <label className="flex cursor-pointer items-center gap-1.5 text-xs text-slate-400">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={e => setAutoRefresh(e.target.checked)}
              className="accent-sky-500"
            />
            auto
          </label>
          <button
            type="button"
            onClick={() => void refresh()}
            disabled={loading}
            className="flex items-center gap-1.5 rounded border border-slate-600 bg-slate-800 px-3 py-1.5 text-xs text-slate-300 hover:bg-slate-700 disabled:opacity-50"
          >
            <RefreshCw size={12} className={loading ? "animate-spin" : ""} />
            Atualizar
          </button>
        </div>
      </div>

      {/* Legend */}
      <div className="flex flex-wrap gap-3 text-[11px] text-slate-500">
        <span className="flex items-center gap-1">
          <CheckCircle2 size={11} className="text-emerald-400" /> tool executada
        </span>
        <span className="flex items-center gap-1">
          <XCircle size={11} className="text-red-400" /> vetada/erro
        </span>
        <span className="flex items-center gap-1">
          <AlertTriangle size={11} className="text-amber-400" /> retry usado
        </span>
        <span className="flex items-center gap-1">
          <Badge label="2-step" variant="info" /> segundo passo LLM
        </span>
      </div>

      {/* Turn list */}
      <div className="space-y-2">
        {turns.length === 0 && !loading && (
          <div className="rounded-lg border border-dashed border-slate-700 p-8 text-center text-sm text-slate-500">
            Nenhum turno LLM registrado ainda.
            <br />
            Fale com o robô para ver os dados aparecerem aqui.
          </div>
        )}
        {turns.map(t => (
          <TurnCard key={`${t.turn_id}-${t.ts}`} turn={t} />
        ))}
      </div>
    </div>
  );
}
