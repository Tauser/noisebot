import type { ChangeEvent } from "react";
import type { LucideIcon } from "lucide-react";

export function ControlPanel({
  icon: Icon,
  label,
  onChange,
  value,
}: {
  icon: LucideIcon;
  label: string;
  onChange: (value: number) => void;
  value: number;
}) {
  return (
    <div className="grid gap-3">
      <div className="flex items-center justify-between">
        <span className="flex items-center gap-2 text-sm font-medium text-slate-400">
          <Icon size={16} />
          {label}
        </span>
        <strong className="text-2xl font-bold text-white">{value}%</strong>
      </div>
      <input
        className="w-full accent-blue-500"
        max="100"
        min="0"
        onChange={(event: ChangeEvent<HTMLInputElement>) => onChange(Number(event.target.value))}
        type="range"
        value={value}
      />
    </div>
  );
}
