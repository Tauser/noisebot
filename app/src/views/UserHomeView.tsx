import {
  Activity, Brain, Clock3, Mic, Volume2,
} from "lucide-react";
import type {
  AiMetrics, AppData, DashboardSnapshot, FirmwareDiagnostics, VoiceSessionSummary,
} from "../api";
import { HardwareArcGauge } from "../components/HardwareArcGauge";
import { SocialPresenceCard } from "../components/SocialPresenceCard";
import { StatTile } from "../components/StatTile";
import { asRecord, readNumber } from "../lib/format";

export function UserHomeView({
  appData,
  diagnostics,
  metrics,
  snapshot,
}: {
  appData: AppData;
  diagnostics: FirmwareDiagnostics;
  metrics: AiMetrics;
  snapshot: DashboardSnapshot;
}) {
  const r = snapshot.robot;
  const totalInput = metrics.tokens.input ?? 0;
  const totalOutput = metrics.tokens.output ?? 0;
  const totalTokens = totalInput + totalOutput;
  const lastLlmSession = metrics.recent_voice_sessions.find(hasLlmMetrics)
    ?? (hasLlmMetrics(metrics.last_voice_session) ? metrics.last_voice_session : null);
  const lastInput = lastLlmSession?.input_tokens ?? 0;
  const lastOutput = lastLlmSession?.output_tokens ?? 0;
  const lastTokens = lastInput + lastOutput;
  const hardware = readHardwareSnapshot(diagnostics);

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
            { label: "Modelo", value: r.model || "--" },
            { label: "Tokens", value: totalTokens > 0 ? formatInteger(totalTokens) : "--" },
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

      <section className="overflow-hidden rounded-xl bg-black/[0.18]">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b border-white/[0.05] px-5 py-4">
          <div className="flex items-center gap-3">
            <span className="flex h-9 w-9 items-center justify-center rounded-lg bg-emerald-500/10 text-emerald-400">
              <Brain size={18} />
            </span>
            <div>
              <h2 className="text-sm font-semibold text-white">
                {r.model || "Modelo local"}
              </h2>
              <p className="mt-0.5 text-xs text-slate-500">
                {r.provider || "local"} · {r.llmStatus || "status desconhecido"}
              </p>
            </div>
          </div>
          <span className="text-xs text-slate-500">
            desde o início do server ou último reset
          </span>
        </div>

        <div className="grid gap-5 px-5 py-5 lg:grid-cols-[minmax(0,1fr)_minmax(0,1.35fr)]">
          <div>
            <p className="text-[10px] font-bold uppercase tracking-[0.12em] text-slate-500">
              Tokens processados
            </p>
            <p className="mt-1 text-3xl font-bold text-white">
              {totalTokens > 0 ? formatInteger(totalTokens) : "--"}
            </p>
            <div className="mt-3 flex flex-wrap gap-2">
              <MetricPill label="Entrada" value={formatOptionalInteger(totalInput)} />
              <MetricPill label="Saída" value={formatOptionalInteger(totalOutput)} />
              <MetricPill label="Turnos LLM" value={formatInteger(metrics.turns.llm ?? 0)} />
            </div>
          </div>

          <div className="border-t border-white/[0.06] pt-4 lg:border-l lg:border-t-0 lg:pl-5 lg:pt-0">
            <div className="flex items-center justify-between gap-3">
              <p className="text-[10px] font-bold uppercase tracking-[0.12em] text-slate-500">
                Último turno
              </p>
              {lastLlmSession?.turn_id && (
                <span className="text-[10px] text-slate-600">
                  #{lastLlmSession.turn_id}
                </span>
              )}
            </div>
            <div className="mt-2 flex flex-wrap items-baseline gap-x-3 gap-y-1">
              <strong className="text-2xl text-white">
                {lastTokens > 0 ? `${formatInteger(lastTokens)} tokens` : "--"}
              </strong>
              <span className="text-xs text-slate-500">
                {lastTokens > 0
                  ? `${formatInteger(lastInput)} entrada · ${formatInteger(lastOutput)} saída`
                  : "uso não reportado"}
              </span>
            </div>
            <div className="mt-3 flex flex-wrap gap-2">
              <MetricPill
                icon={Clock3}
                label="Resposta"
                value={formatDuration(lastLlmSession?.llm_total_ms)}
              />
              <MetricPill
                label="Primeiro token"
                value={formatDuration(lastLlmSession?.llm_first_token_ms)}
              />
              <MetricPill
                label="Rota"
                value={lastLlmSession?.route || r.lastRoute || "--"}
              />
            </div>
          </div>
        </div>
      </section>

      {/* ── Linha inferior ── */}
      <div className="grid gap-4 xl:grid-cols-[minmax(0,2fr)_minmax(0,1fr)_minmax(0,1fr)]">
        {/* Hardware em tempo real — card principal */}
        <section className="overflow-hidden rounded-xl bg-black/[0.18]">
          <div className="flex flex-wrap items-center justify-between gap-3 border-b border-white/[0.05] px-5 py-4">
            <div>
              <h2 className="text-sm font-semibold text-white">Consumo do hardware</h2>
              <p className="mt-0.5 text-xs text-slate-500">
                Leituras reais do ESP32-S3 a cada 5 segundos
              </p>
            </div>
            <div className="flex flex-wrap gap-2 text-xs">
              <HardwareBadge
                label="Saúde"
                value={hardware.health == null ? "--" : `${Math.round(hardware.health)}%`}
              />
              <HardwareBadge
                label="WiFi"
                value={hardware.rssi == null ? "--" : `${Math.round(hardware.rssi)} dBm`}
              />
              <HardwareBadge
                label="Janela"
                value="tempo real"
              />
            </div>
          </div>
          <div className="grid gap-3 p-4 sm:grid-cols-2">
            <HardwareArcGauge
              color="#8b5cf6"
              detail="Percentual livre da PSRAM física"
              label="PSRAM livre"
              maximum={hardware.psramTotal}
              value={hardware.psramFree}
              valueLabel={formatNullableBytes(hardware.psramFree)}
            />
            <HardwareArcGauge
              color="#f59e0b"
              detail="Escala de renderização de 0 a 60 FPS"
              label="Renderização"
              maximum={60}
              value={hardware.fps}
              valueLabel={hardware.fps == null ? "--" : `${hardware.fps.toFixed(1)} fps`}
            />
            <HardwareArcGauge
              color="#10b981"
              detail="Percentual livre do cartão montado"
              label="SD livre"
              maximum={hardware.sdTotal}
              value={hardware.sdFree}
              valueLabel={formatNullableBytes(hardware.sdFree)}
            />
            <HardwareArcGauge
              color="#38bdf8"
              detail="Carga agregada dos dois núcleos"
              label="Uso de CPU"
              maximum={100}
              value={hardware.cpu}
              valueLabel={hardware.cpu == null ? "--" : `${hardware.cpu.toFixed(1)}%`}
            />
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

        </section>

        {/* Presença social — card lateral */}
        <SocialPresenceCard presence={snapshot.social_presence} />
      </div>
    </div>
  );
}

function MetricPill({
  icon: Icon,
  label,
  value,
}: {
  icon?: typeof Brain;
  label: string;
  value: string;
}) {
  return (
    <span className="inline-flex items-center gap-1.5 rounded-md border border-white/[0.07] bg-white/[0.03] px-2.5 py-1.5 text-xs">
      {Icon && <Icon size={12} className="text-emerald-400" />}
      <span className="text-slate-500">{label}</span>
      <strong className="font-semibold text-slate-200">{value}</strong>
    </span>
  );
}

function HardwareBadge({ label, value }: { label: string; value: string }) {
  return (
    <span className="rounded-md border border-white/[0.07] bg-white/[0.03] px-2.5 py-1.5">
      <span className="text-slate-500">{label} </span>
      <strong className="text-slate-200">{value}</strong>
    </span>
  );
}

type HardwareSnapshot = {
  psramFree: number | null;
  psramTotal: number | null;
  fps: number | null;
  sdFree: number | null;
  sdTotal: number | null;
  cpu: number | null;
  health: number | null;
  rssi: number | null;
};

function readHardwareSnapshot(diagnostics: FirmwareDiagnostics): HardwareSnapshot {
  const diag = asRecord(diagnostics.diag);
  const health = asRecord(diagnostics.health);
  const wifi = asRecord(diagnostics.wifi);
  const storage = asRecord(health.storage);
  return {
    psramFree: readNumber(health.heap_psram_free)
      ?? readNumber(asRecord(diag.memory).psram_free),
    psramTotal: readNumber(health.heap_psram_total),
    fps: readNumber(diag.fps),
    sdFree: readNumber(storage.sd_free_bytes),
    sdTotal: readNumber(storage.sd_total_bytes),
    cpu: readNumber(health.cpu_percent) ?? readNumber(diag.cpu_percent),
    health: readNumber(health.health) ?? readNumber(diag.health),
    rssi: readNumber(wifi.rssi),
  };
}

function hasLlmMetrics(session: VoiceSessionSummary | null | undefined): boolean {
  if (!session) return false;
  return (
    session.llm_total_ms != null
    || session.input_tokens != null
    || session.output_tokens != null
  );
}

function formatInteger(value: number): string {
  return new Intl.NumberFormat("pt-BR", { maximumFractionDigits: 0 }).format(value);
}

function formatOptionalInteger(value: number): string {
  return value > 0 ? formatInteger(value) : "--";
}

function formatBytes(value: number): string {
  if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MB`;
  if (value >= 1024) return `${Math.round(value / 1024)} KB`;
  return `${Math.round(value)} B`;
}

function formatNullableBytes(value: number | null): string {
  return value == null ? "--" : formatBytes(value);
}

function formatDuration(value: number | null | undefined): string {
  if (value == null) return "--";
  const seconds = value / 1000;
  return `${new Intl.NumberFormat("pt-BR", {
    minimumFractionDigits: seconds < 10 ? 2 : 1,
    maximumFractionDigits: seconds < 10 ? 2 : 1,
  }).format(seconds)} s`;
}
