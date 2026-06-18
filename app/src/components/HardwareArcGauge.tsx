export function HardwareArcGauge({
  color,
  detail,
  label,
  maximum,
  value,
  valueLabel,
}: {
  color: string;
  detail: string;
  label: string;
  maximum: number | null;
  value: number | null;
  valueLabel: string;
}) {
  const percent = value == null || maximum == null || maximum <= 0
    ? null
    : Math.max(0, Math.min(100, (value / maximum) * 100));

  return (
    <article className="rounded-xl border border-white/[0.06] bg-[#181e31] p-4 text-center">
      <p className="text-[10px] font-bold uppercase tracking-[0.12em] text-slate-500">
        {label}
      </p>
      <div className="relative mx-auto mt-3 h-28 max-w-52">
        <svg
          aria-label={`${label}: ${valueLabel}`}
          className="h-full w-full overflow-visible"
          role="img"
          viewBox="0 0 200 112"
        >
          <path
            d="M20 100 A80 80 0 0 1 180 100"
            fill="none"
            pathLength="100"
            stroke="rgba(148,163,184,0.12)"
            strokeLinecap="round"
            strokeWidth="15"
          />
          {percent != null && (
            <path
              d="M20 100 A80 80 0 0 1 180 100"
              fill="none"
              pathLength="100"
              stroke={color}
              strokeDasharray={`${percent} 100`}
              strokeLinecap="round"
              strokeWidth="15"
              style={{ filter: `drop-shadow(0 0 7px ${color}55)` }}
            />
          )}
        </svg>
        <div className="absolute inset-x-0 bottom-0">
          <strong className="block text-2xl text-white">
            {percent == null ? "--" : `${Math.round(percent)}%`}
          </strong>
          <span className="text-xs font-semibold" style={{ color }}>
            {valueLabel}
          </span>
        </div>
      </div>
      <p className="mt-2 text-[10px] text-slate-500">{detail}</p>
    </article>
  );
}
