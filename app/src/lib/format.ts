import type { AiMetrics, DashboardSnapshot, RoutineItem } from "../api";

export function formatLatency(
  value: AiMetrics["latency_ms"][string] | undefined,
): string {
  if (!value || value.count <= 0) return "--";
  const p50 = value.p50 === null ? "--" : `${value.p50} ms`;
  const p95 = value.p95 === null ? "--" : `${value.p95} ms`;
  return `p50 ${p50} / p95 ${p95}`;
}

export function asRecord(value: unknown): Record<string, unknown> {
  if (value && typeof value === "object" && !Array.isArray(value)) {
    return value as Record<string, unknown>;
  }
  return {};
}

export function readNumber(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim() !== "") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
}

export function textValue(value: unknown): string {
  if (value === null || value === undefined || value === "") return "--";
  return String(value);
}

export function numberValue(value: unknown, suffix: string): string {
  const number = readNumber(value);
  if (number === null) return "--";
  return `${number}${suffix}`;
}

export function ratioLabel(value: unknown): string {
  const number = readNumber(value);
  if (number === null) return "--";
  return `${Math.round(Math.max(0, Math.min(1, number)) * 100)}%`;
}

export function boolValue(value: unknown): string {
  if (typeof value !== "boolean") return "--";
  return value ? "sim" : "não";
}

export function bytesValue(value: unknown): string {
  const bytes = readNumber(value);
  if (bytes === null) return "--";
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${bytes} B`;
}

export function formatSeconds(value: number | null): string {
  if (value === null) return "--";
  if (value >= 3600) return `${(value / 3600).toFixed(1)} h`;
  if (value >= 60) return `${Math.round(value / 60)} min`;
  return `${value} s`;
}

export function formatTime(ts: number): string {
  if (!Number.isFinite(ts) || ts <= 0) return "--";
  return new Date(ts * 1000).toLocaleTimeString("pt-BR", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

export function kindLabel(kind: RoutineItem["kind"]): string {
  if (kind === "timer") return "Timer";
  if (kind === "alarm") return "Alarme";
  return "Lembrete";
}

export function clampNumber(value: string, min: number, max: number): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return min;
  return Math.max(min, Math.min(max, Math.round(parsed)));
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : "erro inesperado";
}

export function lastReplyText(snapshot: DashboardSnapshot): string {
  if (snapshot.robot.lastReply) return snapshot.robot.lastReply;
  if (snapshot.robot.lastTurnId > 0) {
    return "Sem resposta registrada. Talvez eu não tenha entendido.";
  }
  return "Sem resposta recente.";
}

export function logLevelClass(level: string): string {
  const normalized = level.toUpperCase();
  if (normalized === "ERROR" || normalized === "CRITICAL") return "text-rose-300";
  if (normalized === "WARNING" || normalized === "WARN") return "text-amber-300";
  if (normalized === "DEBUG") return "text-sky-300";
  return "text-emerald-300";
}
