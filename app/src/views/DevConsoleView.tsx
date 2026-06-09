import type { DevData, DashboardSnapshot } from "../api";
import { cardClass, inputClass, secondaryButtonClass, primaryButtonClass } from "../lib/classes";
import { asRecord, boolValue, numberValue, formatTime, logLevelClass } from "../lib/format";
import { Metric } from "../components/Metric";

export function DevConsoleView({
  devData,
  onAudioProcessorAction,
  onResetMetrics,
  onRestartServer,
  opsToken,
  onOpsTokenChange,
  snapshot,
  status,
}: {
  devData: DevData;
  onAudioProcessorAction: (
    action: "shadow_start" | "shadow_stop" | "bridge_start" | "bridge_stop",
  ) => void;
  onResetMetrics: () => void;
  onRestartServer: () => void;
  opsToken: string;
  onOpsTokenChange: (value: string) => void;
  snapshot: DashboardSnapshot;
  status: string;
}) {
  const audioProcessor = asRecord(devData.diagnostics.audio_processor);

  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_300px]">
      {/* Logs */}
      <section className={cardClass}>
        <div className="mb-3 flex items-center justify-between gap-3">
          <h2 className="text-lg font-semibold">Logs do server</h2>
          <span className="rounded-full bg-white/5 px-3 py-1 text-xs font-semibold text-slate-300">
            {devData.logs.length} linhas
          </span>
        </div>
        <div className="min-h-72 overflow-auto rounded-xl bg-black/40 p-4 font-mono text-sm text-slate-300">
          <p className="text-slate-400">
            {">"} firmware:{" "}
            {snapshot.robot.firmwareOnline ? "online" : "offline"} | turnos:{" "}
            {devData.metrics.turns.total ?? 0}
          </p>
          {snapshot.robot.lastError && (
            <p className="mt-2 text-rose-300">
              {">"} último erro: {snapshot.robot.lastError}
            </p>
          )}
          {devData.logs.length === 0 && (
            <p className="mt-3 text-slate-400">
              {">"} sem logs recentes capturados pelo server
            </p>
          )}
          {devData.logs.map((entry, index) => (
            <p
              className="mt-2 wrap-break-word"
              key={`${entry.ts}-${entry.level}-${index}`}
            >
              <span className="text-slate-400">{formatTime(entry.ts)}</span>{" "}
              <span className={logLevelClass(entry.level)}>
                {entry.level.padEnd(7, " ")}
              </span>{" "}
              <span className="text-slate-400">{entry.logger}</span>{" "}
              {entry.message}
            </p>
          ))}
        </div>
        {devData.errors.length > 0 && (
          <div className="mt-3 rounded-lg bg-amber-400/10 p-3 text-sm text-amber-200">
            <strong className="block">Erros estruturados</strong>
            {devData.errors.slice(0, 3).map((error) => (
              <p
                className="mt-1"
                key={`${error.ts}-${error.turn_id}-${error.kind}`}
              >
                [{formatTime(error.ts)}] {error.kind} turn={error.turn_id}{" "}
                {error.message}
              </p>
            ))}
          </div>
        )}
      </section>

      <aside className="grid content-start gap-4">
        {/* Token */}
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Token local</h2>
          {!opsToken.trim() && (
            <p className="mb-3 rounded-lg bg-amber-400/10 p-3 text-sm text-amber-200">
              Sem token salvo. Cole o mesmo token usado no localhost.
            </p>
          )}
          <label className="grid gap-2 text-sm font-semibold text-slate-300">
            Ops token
            <input
              className={inputClass}
              onChange={(e) => onOpsTokenChange(e.target.value)}
              placeholder="cole o token local"
              type="password"
              value={opsToken}
            />
          </label>
        </section>

        {/* AFE de voz */}
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">AFE de voz</h2>
          <div className="grid gap-2">
            <Metric label="Shadow" value={boolValue(audioProcessor.shadow_active)} />
            <Metric label="Bridge AFE" value={boolValue(audioProcessor.processed_bridge_enabled)} />
            <Metric label="Captura AFE" value={boolValue(audioProcessor.processed_capture_active)} />
            <Metric label="Chunks AFE" value={numberValue(audioProcessor.processed_bridge_chunks, "")} />
            <Metric label="Fallbacks" value={numberValue(audioProcessor.processed_bridge_fallbacks, "")} />
            <Metric label="Buffer" value={numberValue(audioProcessor.processed_buffer_level, " samples")} />
            <Metric label="Overruns" value={numberValue(audioProcessor.processed_output_overruns, "")} />
          </div>
          <div className="mt-3 grid grid-cols-2 gap-2">
            <button
              className={secondaryButtonClass}
              onClick={() => onAudioProcessorAction("shadow_start")}
              type="button"
            >
              Shadow on
            </button>
            <button
              className={secondaryButtonClass}
              onClick={() => onAudioProcessorAction("shadow_stop")}
              type="button"
            >
              Shadow off
            </button>
            <button
              className={primaryButtonClass}
              onClick={() => onAudioProcessorAction("bridge_start")}
              type="button"
            >
              Bridge AFE
            </button>
            <button
              className={secondaryButtonClass}
              onClick={() => onAudioProcessorAction("bridge_stop")}
              type="button"
            >
              Bridge raw
            </button>
          </div>
        </section>

        {/* Manutenção */}
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Manutenção</h2>
          <div className="grid gap-2">
            <button
              className={secondaryButtonClass}
              onClick={onResetMetrics}
              type="button"
            >
              Zerar métricas
            </button>
            <button
              className={secondaryButtonClass}
              onClick={onRestartServer}
              type="button"
            >
              Reiniciar server
            </button>
          </div>
          {status !== "pronto" && (
            <p className="mt-3 text-sm text-slate-400">{status}</p>
          )}
        </section>
      </aside>
    </div>
  );
}
