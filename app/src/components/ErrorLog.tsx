import type { DevData } from "../api";
import { formatTime } from "../lib/format";

export function ErrorLog({ errors }: { errors: DevData["errors"] }) {
  if (errors.length === 0) {
    return (
      <p className="rounded-lg border border-dashed border-white/10 p-4 text-sm text-slate-400">
        Nenhum erro recente registrado.
      </p>
    );
  }
  return (
    <div className="grid gap-2">
      {errors.map((error) => (
        <article
          className="rounded-lg border border-white/10 bg-white/[0.03] p-3"
          key={`${error.ts}-${error.turn_id}-${error.kind}`}
        >
          <div className="flex flex-wrap items-center justify-between gap-2">
            <strong className="text-sm text-slate-100">{error.kind}</strong>
            <span className="text-xs font-semibold text-slate-400">
              {formatTime(error.ts)}
            </span>
          </div>
          <p className="mt-1 text-sm text-slate-300">
            {error.message || "sem mensagem"}
          </p>
          <p className="mt-1 text-xs text-slate-400">
            turn={error.turn_id || 0} provider={error.provider || "--"} model=
            {error.model || "--"}
          </p>
        </article>
      ))}
    </div>
  );
}
