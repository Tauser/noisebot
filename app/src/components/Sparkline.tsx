/** Tiny SVG sparkline – no external deps */
export function Sparkline({
  color = "#4f6ef5",
  data,
  fill = true,
}: {
  color?: string;
  data: number[];
  fill?: boolean;
}) {
  if (data.length < 2) return null;
  const W = 200;
  const H = 44;
  const min = Math.min(...data);
  const max = Math.max(...data);
  const range = max - min || 1;
  const toX = (i: number) => (i / (data.length - 1)) * W;
  const toY = (v: number) => H - ((v - min) / range) * (H - 8) - 4;
  const pts = data.map((v, i): [number, number] => [toX(i), toY(v)]);
  const line = pts
    .map(([x, y], i) => `${i === 0 ? "M" : "L"}${x.toFixed(1)},${y.toFixed(1)}`)
    .join(" ");
  const area = fill
    ? `${line} L${pts[pts.length - 1][0].toFixed(1)},${H} L${pts[0][0].toFixed(1)},${H} Z`
    : "";

  return (
    <svg
      viewBox={`0 0 ${W} ${H}`}
      className="w-full"
      preserveAspectRatio="none"
      style={{ height: H }}
    >
      {fill && (
        <path d={area} fill={color} fillOpacity={0.12} />
      )}
      <path
        d={line}
        fill="none"
        stroke={color}
        strokeWidth="1.8"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}
