import { useEffect, useState } from "react";
import type { DevData, DashboardSnapshot } from "../api";
import { fetchSandboxStatus, setSandboxMode } from "../api";
import { cardClass } from "../lib/classes";
import { ServiceTile } from "../components/ServiceTile";
import { InfoRow } from "../components/InfoRow";
import { ErrorLog } from "../components/ErrorLog";

function SandboxToggle({ opsToken }: { opsToken?: string }) {
  const [enabled, setEnabled] = useState(false);
  const [loading, setLoading] = useState(false);
  const [status, setStatus] = useState("");

  useEffect(() => {
    fetchSandboxStatus().then(setEnabled);
  }, []);

  async function toggle() {
    if (!opsToken?.trim()) {
      setStatus("token obrigatório — acesse Dev > Sistema");
      return;
    }
    setLoading(true);
    setStatus("");
    try {
      const next = !enabled;
      await setSandboxMode(next, opsToken);
      setEnabled(next);
      setStatus(next ? "sandbox ativado" : "sandbox desativado");
    } catch (e) {
      setStatus(e instanceof Error ? e.message : "erro");
    } finally {
      setLoading(false);
    }
  }

  return (
    <div className="flex items-start justify-between gap-4">
      <div>
        <p className="text-sm font-medium text-slate-200">Tool Sandbox</p>
        <p className="mt-0.5 text-xs text-slate-400">
          Tools são validadas normalmente, mas o executor não é chamado.
          O segundo passo da LLM recebe "seria executado" como contexto.
        </p>
        {status && (
          <p className="mt-1 text-xs text-amber-400">{status}</p>
        )}
      </div>
      <button
        type="button"
        onClick={() => void toggle()}
        disabled={loading}
        className={
          enabled
            ? "shrink-0 rounded-full px-4 py-1.5 text-xs font-semibold bg-amber-600 text-white hover:bg-amber-700 disabled:opacity-50 transition"
            : "shrink-0 rounded-full px-4 py-1.5 text-xs font-semibold bg-slate-700 text-slate-300 hover:bg-slate-600 disabled:opacity-50 transition"
        }
      >
        {loading ? "…" : enabled ? "Ativado" : "Desativado"}
      </button>
    </div>
  );
}

export function DevIntegrationsView({
  devData,
  snapshot,
  opsToken,
}: {
  devData: DevData;
  snapshot: DashboardSnapshot;
  opsToken?: string;
}) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_300px]">
      <section className={cardClass}>
        <h2 className="mb-3 text-lg font-semibold">Serviços</h2>
        <div className="grid gap-3 md:grid-cols-3">
          <ServiceTile label="STT" value={snapshot.robot.sttStatus} />
          <ServiceTile label="TTS" value={snapshot.robot.ttsStatus} />
          <ServiceTile label="LLM" value={snapshot.robot.llmStatus} />
        </div>
      </section>

      <section className={cardClass}>
        <h2 className="mb-3 text-lg font-semibold">Diagnóstico</h2>
        <InfoRow label="Último erro" value={snapshot.robot.lastError || "--"} />
        <InfoRow label="Rota" value={snapshot.robot.lastRoute || "--"} />
        <InfoRow label="Turno" value={String(snapshot.robot.lastTurnId || 0)} />
        <InfoRow
          label="Pipeline"
          value={devData.config.pipeline_mode || snapshot.robot.mode || "--"}
        />
        <InfoRow
          label="Modelo"
          value={devData.config.llm?.model || snapshot.robot.model || "--"}
        />
      </section>

      <section className={`${cardClass} lg:col-span-2`}>
        <h2 className="mb-3 text-lg font-semibold">Tool Sandbox</h2>
        <SandboxToggle opsToken={opsToken} />
      </section>

      <section className={`${cardClass} lg:col-span-2`}>
        <h2 className="mb-3 text-lg font-semibold">Erros recentes</h2>
        <ErrorLog errors={devData.errors} />
      </section>
    </div>
  );
}
