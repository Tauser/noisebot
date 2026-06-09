export function ToggleRow({
  description,
  enabled,
  label,
  onChange,
}: {
  description: string;
  enabled: boolean;
  label: string;
  onChange: (enabled: boolean) => void;
}) {
  return (
    <div className="flex items-start justify-between gap-4">
      <div className="min-w-0">
        <p className="font-medium text-slate-100">{label}</p>
        <p className="mt-1 text-sm text-slate-400">{description}</p>
      </div>

      {/* Segmented pill: Ativo / Inativo */}
      <div className="flex shrink-0 rounded-lg bg-black/[0.25] p-1 gap-1">
        <button
          type="button"
          onClick={() => onChange(true)}
          className={
            enabled
              ? "rounded-md px-3 py-1 text-xs font-semibold text-white bg-blue-600 shadow transition"
              : "rounded-md px-3 py-1 text-xs font-semibold text-slate-400 transition hover:text-white"
          }
        >
          Ativo
        </button>
        <button
          type="button"
          onClick={() => onChange(false)}
          className={
            !enabled
              ? "rounded-md px-3 py-1 text-xs font-semibold text-white bg-black/[0.35] shadow transition"
              : "rounded-md px-3 py-1 text-xs font-semibold text-slate-400 transition hover:text-white"
          }
        >
          Inativo
        </button>
      </div>
    </div>
  );
}
