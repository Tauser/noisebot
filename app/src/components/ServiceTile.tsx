export function ServiceTile({ label, value }: { label: string; value: string }) {
  const ok = value === "ok" || value === "ready" || value === "enabled";
  return (
    <article className="rounded-xl bg-black/[0.18] p-4 transition hover:bg-black/[0.25]">
      <span
        className={
          ok
            ? "pulse-dot relative mb-3 inline-flex h-2.5 w-2.5 rounded-full bg-emerald-500 text-emerald-500"
            : "mb-3 inline-flex h-2.5 w-2.5 rounded-full bg-amber-400"
        }
        aria-hidden
      />
      <strong className="block text-sm font-semibold text-white">{label}</strong>
      <span className="text-xs text-slate-400">{value || "--"}</span>
    </article>
  );
}
