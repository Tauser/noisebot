import type { ReactNode } from "react";

export function LabeledField({ children, label }: { children: ReactNode; label: string }) {
  return (
    <label className="grid gap-1.5 text-sm font-semibold text-slate-300">
      <span>{label}</span>
      {children}
    </label>
  );
}
