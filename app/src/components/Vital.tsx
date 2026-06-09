import type { LucideIcon } from "lucide-react";

export function Vital({
  good,
  icon: Icon,
  label,
  pulse,
}: {
  good?: boolean;
  icon: LucideIcon;
  label: string;
  pulse?: boolean;
}) {
  return (
    <span
      className={
        good
          ? "inline-flex min-h-8 items-center gap-2 rounded-full bg-emerald-500/10 px-3 text-xs font-semibold text-emerald-400"
          : "inline-flex min-h-8 items-center gap-2 rounded-full bg-black/[0.18] px-3 text-xs font-semibold text-slate-400"
      }
    >
      {pulse ? (
        <span className="pulse-dot relative inline-flex h-2 w-2 rounded-full bg-emerald-500 text-emerald-500" />
      ) : (
        <Icon size={14} />
      )}
      {label}
    </span>
  );
}
