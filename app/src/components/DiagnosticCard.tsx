import type { ReactNode } from "react";
import type { LucideIcon } from "lucide-react";
import { cardClass } from "../lib/classes";

export function DiagnosticCard({
  children,
  defaultOpen = false,
  icon: Icon,
  title,
  wide = false,
}: {
  children: ReactNode;
  defaultOpen?: boolean;
  icon: LucideIcon;
  title: string;
  wide?: boolean;
}) {
  return (
    <details
      className={`${cardClass} group ${wide ? "xl:col-span-2" : ""}`}
      open={defaultOpen}
    >
      <summary className="flex cursor-pointer list-none items-center justify-between gap-3 [&::-webkit-details-marker]:hidden">
        <span className="flex items-center gap-2 text-lg font-semibold text-slate-100">
          <Icon size={18} className="text-slate-400" />
          {title}
        </span>
        <span className="rounded-full bg-black/[0.20] px-3 py-1 text-xs font-bold text-slate-400 group-open:hidden">
          abrir
        </span>
        <span className="hidden rounded-full bg-black/[0.20] px-3 py-1 text-xs font-bold text-slate-400 group-open:inline-flex">
          fechar
        </span>
      </summary>
      <div className="mt-4">{children}</div>
    </details>
  );
}
