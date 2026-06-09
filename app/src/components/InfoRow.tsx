export function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex min-h-9 items-center justify-between gap-3 border-b border-white/[0.04] py-2 last:border-0">
      <span className="text-sm text-slate-400">{label}</span>
      <strong className="wrap-break-word text-right text-sm font-medium text-slate-100">{value}</strong>
    </div>
  );
}
