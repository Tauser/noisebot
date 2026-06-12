import { Users } from "lucide-react";
import type { SocialPresence, SocialPresenceState } from "../api";

function statePill(state: SocialPresenceState): { label: string; cls: string } {
  switch (state) {
    case "PRESENT":
      return { label: "presente", cls: "bg-emerald-500/15 text-emerald-400 border-emerald-500/20" };
    case "ENGAGED":
      return { label: "engajado", cls: "bg-emerald-500/25 text-emerald-300 border-emerald-500/30" };
    case "MAYBE_SOMEONE":
      return { label: "talvez alguém", cls: "bg-amber-500/15 text-amber-400 border-amber-500/20" };
    case "LEFT_RECENTLY":
      return { label: "saiu há pouco", cls: "bg-amber-500/15 text-amber-400 border-amber-500/20" };
    case "AWAY":
      return { label: "ausente", cls: "bg-orange-500/15 text-orange-400 border-orange-500/20" };
    case "ALONE_SETTLED":
      return { label: "sozinho", cls: "bg-amber-500/15 text-amber-400 border-amber-500/20" };
    default:
      return { label: "ninguém", cls: "bg-white/5 text-slate-500 border-white/10" };
  }
}

function formatCompanionship(seconds: number): string {
  if (seconds < 60) return `${seconds}s`;
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${m}min`;
  const h = Math.floor(m / 60);
  const rem = m % 60;
  return rem > 0 ? `${h}h ${rem}min` : `${h}h`;
}

export function SocialPresenceCard({ presence }: { presence: SocialPresence | null | undefined }) {
  const state = presence?.state ?? "NO_ONE";
  const pill = statePill(state);
  const companionship = presence?.companionship_today_s ?? 0;

  return (
    <section className="flex flex-col rounded-xl bg-black/[0.18]">
      <div className="flex items-center justify-between gap-3 border-b border-white/[0.05] px-5 py-4">
        <div className="flex items-center gap-2">
          <Users size={14} className="text-slate-400" />
          <h2 className="text-sm font-semibold text-white">Presença social</h2>
        </div>
        <span className={`rounded-full border px-2.5 py-0.5 text-xs font-semibold ${pill.cls}`}>
          {pill.label}
        </span>
      </div>

      <div className="flex-1 px-5 py-4">
        <p className="text-[10px] text-slate-500 mb-1">Companhia hoje</p>
        <p className="text-3xl font-bold text-white">{formatCompanionship(companionship)}</p>
      </div>
    </section>
  );
}
