import { CheckCircle2, MapPin, MessageSquare, MicOff, SlidersHorizontal, Volume, Volume1, Volume2, VolumeX } from "lucide-react";
import type { AppData } from "../api";
import { cardClass, primaryButtonClass } from "../lib/classes";
import { ToggleRow } from "../components/ToggleRow";
import { InfoRow } from "../components/InfoRow";

export function BasicSettingsView({
  appData,
  followupEnabled,
  followupStatus,
  followupWindowMs,
  leds,
  ledsStatus,
  onLedsChange,
  onSaveLeds,
  onSaveVolume,
  onToggleSilenceMode,
  onToggleFollowup,
  onVolumeChange,
  silenceModeEnabled,
  silenceModeStatus,
  volume,
  volumeStatus,
}: {
  appData: AppData;
  followupEnabled: boolean;
  followupStatus: string;
  followupWindowMs?: number;
  leds: number;
  ledsStatus: string;
  onLedsChange: (value: number) => void;
  onSaveLeds: () => void;
  onSaveVolume: () => void;
  onToggleSilenceMode: (enabled: boolean) => void;
  onToggleFollowup: (enabled: boolean) => void;
  onVolumeChange: (value: number) => void;
  silenceModeEnabled: boolean;
  silenceModeStatus: string;
  volume: number;
  volumeStatus: string;
}) {
  const VolumeIcon =
    volume === 0 ? VolumeX : volume <= 33 ? Volume : volume <= 66 ? Volume1 : Volume2;

  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_260px]">

      {/* ── Configurações principais ── */}
      <section className={cardClass}>

        {/* Volume */}
        <div className="pb-5">
          <div className="mb-3 flex items-center justify-between gap-3">
            <span className="flex items-center gap-2 text-sm font-semibold text-slate-200">
              <VolumeIcon size={16} className="text-slate-500" />
              Volume
              <strong className="ml-2 text-xl font-bold text-white">{volume}%</strong>
            </span>
            <button className={primaryButtonClass} onClick={onSaveVolume} type="button">
              <CheckCircle2 size={14} />
              Salvar
            </button>
          </div>
          <input
            className="w-full accent-blue-500"
            max="100" min="0"
            onChange={(e) => onVolumeChange(Number(e.target.value))}
            type="range"
            value={volume}
          />
          {volumeStatus !== "pronto" && (
            <p className="mt-2 text-xs text-slate-400">{volumeStatus}</p>
          )}
        </div>

        {/* LEDs */}
        <div className="border-t border-white/[0.05] py-5">
          <div className="mb-3 flex items-center justify-between gap-3">
            <span className="flex items-center gap-2 text-sm font-semibold text-slate-200">
              <SlidersHorizontal size={16} className="text-slate-500" />
              LEDs
              <strong className="ml-2 text-xl font-bold text-white">{leds}%</strong>
            </span>
            <button className={primaryButtonClass} onClick={onSaveLeds} type="button">
              <CheckCircle2 size={14} />
              Salvar
            </button>
          </div>
          <input
            className="w-full accent-blue-500"
            max="100" min="0"
            onChange={(e) => onLedsChange(Number(e.target.value))}
            type="range"
            value={leds}
          />
          {ledsStatus !== "pronto" && (
            <p className="mt-2 text-xs text-slate-400">{ledsStatus}</p>
          )}
        </div>

        {/* Conversa */}
        <div className="border-t border-white/[0.05] pt-5">
          <p className="mb-3 flex items-center gap-2 text-sm font-semibold text-slate-200">
            <MessageSquare size={16} className="text-slate-500" />
            Conversa
          </p>
          <div className="pb-4">
            <ToggleRow
              description={
                silenceModeEnabled
                  ? "Microfone de conversa desligado; wake word e follow-up ficam suspensos."
                  : "O robô pode ouvir pela palavra de ativação e pela escuta de continuação."
              }
              enabled={silenceModeEnabled}
              label={
                <span className="flex items-center gap-2">
                  <MicOff size={15} className="text-slate-500" />
                  Modo silêncio
                </span>
              }
              onChange={onToggleSilenceMode}
            />
            {silenceModeStatus !== "pronto" && (
              <p className="mt-2 text-xs text-slate-400">{silenceModeStatus}</p>
            )}
          </div>
          <ToggleRow
            description={
              followupEnabled
                ? `Microfone fica aberto ${Math.round((followupWindowMs ?? 8000) / 1000)}s após uma pergunta.`
                : "O robô só ouve após a palavra de ativação."
            }
            enabled={followupEnabled}
            label="Continuar ouvindo após perguntas"
            onChange={onToggleFollowup}
          />
          {followupStatus !== "pronto" && (
            <p className="mt-2 text-xs text-slate-400">{followupStatus}</p>
          )}
        </div>
      </section>

      {/* ── Localização ── */}
      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <p className="mb-3 flex items-center gap-2 text-sm font-semibold text-slate-200">
            <MapPin size={15} className="text-slate-500" />
            Localização
          </p>
          <InfoRow label="Cidade" value={appData.advanced.location || "Não definida"} />
          <InfoRow label="Fuso"   value={appData.advanced.timezone  || "Não definido"} />
        </section>
      </aside>

    </div>
  );
}
