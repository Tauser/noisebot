export function StatusPill({ label, ok }: { label: string; ok: boolean }) {
  return (
    <span
      className={
        ok
          ? "inline-flex items-center gap-1.5 rounded-full border border-emerald-500/25 bg-emerald-500/10 px-3 py-1 text-xs font-semibold text-emerald-400"
          : "inline-flex items-center gap-1.5 rounded-full border border-white/10 bg-white/5 px-3 py-1 text-xs font-semibold text-slate-400"
      }
    >
      <span
        className={
          ok
            ? "pulse-dot relative inline-flex h-1.5 w-1.5 rounded-full bg-emerald-500 text-emerald-500"
            : "inline-flex h-1.5 w-1.5 rounded-full bg-slate-500"
        }
        aria-hidden
      />
      {label}
    </span>
  );
}
