import { Activity, Brain, Mic, Volume2 } from "lucide-react";
import type { AppData, DashboardSnapshot } from "../api";
import { InfoRow } from "../components/InfoRow";
import { SocialPresenceCard } from "../components/SocialPresenceCard";
import { Sparkline } from "../components/Sparkline";
import { StatTile } from "../components/StatTile";
import { TurnBubble } from "../components/TurnBubble";
import { lastReplyText } from "../lib/format";

/* Placeholder sparkline data — gives the card visual life even with no time-series API */
const FAKE_TURNS  = [2, 5, 3, 8, 6, 9, 4, 7, 11, 8, 13, 10];
const FAKE_BUDGET = [40, 55, 48, 62, 58, 70, 65, 75, 68, 80, 72, 85];

export function UserHomeView({
  appData,
  snapshot,
}: {
  appData: AppData;
  snapshot: DashboardSnapshot;
}) {
  const r = snapshot.robot;

  return (
    <div className="grid gap-5">
      {/* ── KPI cards ── */}
      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        <StatTile
          icon={Activity}
          label="Robô"
          tone="blue"
          value={r.firmwareOnline ? "online" : "offline"}
          meta={[
            { label: "Humor",  value: r.mood },
            { label: "Estado", value: r.state },
          ]}
        />
        <StatTile
          icon={Mic}
          label="Reconhecimento de fala"
          tone="violet"
          value={r.sttStatus || "--"}
          meta={[
            { label: "Turno", value: String(r.lastTurnId || 0) },
            { label: "Rota",  value: r.lastRoute || "--" },
          ]}
        />
        <StatTile
          icon={Brain}
          label="Modelo de linguagem"
          tone="emerald"
          value={r.llmStatus || "--"}
          meta={[
            { label: "Modelo",    value: r.model    || "--" },
            { label: "Provedor",  value: r.provider || "--" },
          ]}
        />
        <StatTile
          icon={Volume2}
          label="Síntese de voz"
          tone="rose"
          value={r.ttsStatus || "--"}
          meta={[
            { label: "Energia", value: r.batteryLabel || "--" },
            { label: "Modo",    value: r.mode         || "--" },
          ]}
        />
      </div>

      {/* ── Linha inferior ── */}
      <div className="grid gap-4 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)_minmax(0,1fr)]">
        {/* Última interação — card largo */}
        <section className="flex flex-col rounded-xl bg-black/[0.18]">
          {/* Card header */}
          <div className="flex items-center justify-between gap-3 border-b border-white/[0.05] px-5 py-4">
            <h2 className="text-sm font-semibold text-white">Última interação</h2>
            <div className="flex items-center gap-2">
              {r.lastTurnId > 0 && (
                <span className="rounded-full border border-white/10 bg-white/5 px-2.5 py-0.5 text-xs font-semibold text-slate-400">
                  turno #{r.lastTurnId}
                </span>
              )}
            </div>
          </div>

          {/* Turn bubbles */}
          <div className="grid flex-1 gap-3 p-5 lg:grid-cols-2">
            <TurnBubble
              label="Ouvi"
              text={r.lastTranscript || "Sem transcrição recente."}
            />
            <TurnBubble label="Respondi" text={lastReplyText(snapshot)} />
          </div>

          {/* Info rows */}
          <div className="grid gap-3 border-t border-white/[0.05] px-5 pt-4 pb-3 sm:grid-cols-3">
            <InfoRow label="Provedor" value={r.provider || "--"} />
            <InfoRow label="Modelo"   value={r.model    || "--"} />
            <InfoRow label="Modo"     value={r.mode     || "--"} />
          </div>

          {/* Sparkline footer */}
          <div className="border-t border-white/[0.05] px-5 pt-3 pb-4">
            <p className="mb-2 text-[10px] font-semibold uppercase tracking-widest text-slate-500">
              Atividade de turnos
            </p>
            <Sparkline data={FAKE_TURNS} color="#6366f1" fill />
          </div>
        </section>

        {/* Rotina — card lateral */}
        <section className="flex flex-col rounded-xl bg-black/[0.18]">
          {/* Card header */}
          <div className="flex items-center justify-between gap-3 border-b border-white/[0.05] px-5 py-4">
            <h2 className="text-sm font-semibold text-white">Rotina</h2>
            {appData.routine.summary.next && (
              <span className="rounded-full border border-white/10 bg-emerald-500/15 px-2.5 py-0.5 text-xs font-semibold text-emerald-400">
                ativo
              </span>
            )}
          </div>

          {/* Counters */}
          <div className="grid grid-cols-3 gap-3 border-b border-white/[0.05] px-5 py-4">
            {[
              { label: "Timers",    value: appData.routine.summary.timers },
              { label: "Alarmes",   value: appData.routine.summary.alarms },
              { label: "Lembretes", value: appData.routine.summary.reminders },
            ].map(({ label, value }) => (
              <div key={label} className="text-center">
                <p className="text-3xl font-bold text-white">{value}</p>
                <p className="mt-0.5 text-[10px] text-slate-500">{label}</p>
              </div>
            ))}
          </div>

          {/* Next event */}
          <div className="border-b border-white/[0.05] px-5 py-3">
            <p className="text-[10px] text-slate-500 mb-1">Próximo evento</p>
            <p className="text-sm font-semibold text-slate-100 truncate">
              {appData.routine.summary.next || "--"}
            </p>
          </div>

          {/* Items list */}
          <div className="flex-1 px-5 py-3">
            {appData.routine.items.length > 0 ? (
              <div className="grid gap-1.5">
                {appData.routine.items.slice(0, 5).map((item) => (
                  <div key={item.id} className="flex items-center justify-between gap-2">
                    <span className="truncate text-xs text-slate-300">{item.title}</span>
                    <span
                      className={`shrink-0 rounded-full px-2 py-0.5 text-[10px] font-bold uppercase ${
                        item.enabled
                          ? "bg-emerald-500/15 text-emerald-400"
                          : "bg-white/5 text-slate-500"
                      }`}
                    >
                      {item.kind}
                    </span>
                  </div>
                ))}
              </div>
            ) : (
              <p className="text-xs text-slate-500">Sem itens na rotina.</p>
            )}
          </div>

          {/* Sparkline footer */}
          <div className="border-t border-white/[0.05] px-5 pt-3 pb-4">
            <p className="mb-2 text-[10px] font-semibold uppercase tracking-widest text-slate-500">
              Execuções recentes
            </p>
            <Sparkline data={FAKE_BUDGET} color="#10b981" fill />
          </div>
        </section>

        {/* Presença social — card lateral */}
        <SocialPresenceCard presence={snapshot.social_presence} />
      </div>
    </div>
  );
}
