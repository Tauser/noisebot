import { voiceStateLabel } from "../lib/voice";

export function VoiceStage({
  detail,
  label,
  state,
}: {
  detail: string;
  label: string;
  state: "ok" | "warn" | "error" | "idle";
}) {
  const styles = {
    ok:   "bg-emerald-400/10 text-emerald-300",
    warn: "bg-amber-400/10 text-amber-300",
    error:"bg-rose-400/10 text-rose-300",
    idle: "bg-black/[0.15] text-slate-300",
  };
  return (
    <article className={`rounded-xl p-3 ${styles[state]}`}>
      <span className="text-xs font-bold uppercase opacity-75">{label}</span>
      <strong className="mt-1 block text-sm">{voiceStateLabel(state)}</strong>
      <p className="mt-1 text-sm opacity-85">{detail}</p>
    </article>
  );
}
