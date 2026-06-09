import type { LucideIcon } from "lucide-react";

export type StatTileTone = "blue" | "violet" | "emerald" | "rose";

const toneMap: Record<StatTileTone, { bg: string; shadow: string; text: string }> = {
  blue:    { bg: "bg-blue-500",    shadow: "shadow-blue-500/35",    text: "text-blue-400" },
  violet:  { bg: "bg-violet-500",  shadow: "shadow-violet-500/35",  text: "text-violet-400" },
  emerald: { bg: "bg-emerald-500", shadow: "shadow-emerald-500/35", text: "text-emerald-400" },
  rose:    { bg: "bg-rose-500",    shadow: "shadow-rose-500/35",    text: "text-rose-400" },
};

export function StatTile({
  icon: Icon,
  label,
  meta,
  tone,
  value,
}: {
  icon: LucideIcon;
  label: string;
  meta?: { label: string; value: string }[];
  tone: StatTileTone;
  value: string;
}) {
  const t = toneMap[tone];
  return (
    <article className="rounded-xl bg-black/[0.18]">
      {/* Top section: icon + label + value */}
      <div className="flex items-start gap-4 px-5 pt-5 pb-4">
        <span
          className={`inline-flex h-16 w-16 shrink-0 items-center justify-center rounded-2xl ${t.bg} shadow-lg ${t.shadow}`}
        >
          <Icon size={30} className="text-white" />
        </span>
        <div className="min-w-0 flex-1 pt-0.5">
          <p className="mb-2 text-[10px] font-bold uppercase tracking-[0.14em] text-slate-400">
            {label}
          </p>
          <p className="text-4xl font-bold leading-none text-white">{value}</p>
        </div>
      </div>

      {/* Bottom section: sub-stats */}
      {meta && meta.length > 0 && (
        <div className="border-t border-white/[0.05] px-5 py-3">
          <div className="grid grid-cols-2 gap-3">
            {meta.slice(0, 2).map((m) => (
              <div key={m.label} className="min-w-0">
                <p className="text-[11px] text-slate-500">{m.label}</p>
                <p className={`mt-0.5 truncate text-sm font-semibold ${t.text}`}>
                  {m.value || "--"}
                </p>
              </div>
            ))}
          </div>
        </div>
      )}
    </article>
  );
}
