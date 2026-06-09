import type { ReactNode } from "react";

export function Metric({ label, value }: { label: string; value: ReactNode }) {
  return (
    <article className="rounded-lg bg-black/[0.15] p-3">
      <span className="block text-xs font-semibold uppercase tracking-wide text-slate-500">{label}</span>
      <strong className="mt-1 block wrap-break-word text-sm font-semibold text-slate-100">
        {value}
      </strong>
    </article>
  );
}
