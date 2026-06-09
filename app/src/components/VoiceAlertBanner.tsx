import type { DevData } from "../api";

export function VoiceAlertBanner({
  alert,
}: {
  alert: NonNullable<DevData["metrics"]["voice_alert"]>;
}) {
  const style =
    alert.level === "error"
      ? "border-rose-400/20 bg-rose-400/10 text-rose-300"
      : "border-amber-400/20 bg-amber-400/10 text-amber-300";
  return (
    <div className={`mb-4 rounded-lg border p-3 ${style}`}>
      <strong className="block text-sm">{alert.title}</strong>
      <span className="text-sm">{alert.detail || "sem detalhe"}</span>
    </div>
  );
}
