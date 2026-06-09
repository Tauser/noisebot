export function TurnBubble({ label, text }: { label: string; text: string }) {
  return (
    <article className="rounded-xl bg-black/[0.15] p-3">
      <span className="text-xs font-bold uppercase tracking-wide text-slate-500">{label}</span>
      <p className="mt-1 text-sm text-slate-200">{text}</p>
    </article>
  );
}
