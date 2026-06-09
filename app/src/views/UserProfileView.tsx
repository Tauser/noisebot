import { CheckCircle2 } from "lucide-react";
import type { DevicePersona } from "../api";
import { cardClass, primaryButtonClass, inputClass } from "../lib/classes";
import { ratioLabel } from "../lib/format";
import { InfoRow } from "../components/InfoRow";
import { LabeledField } from "../components/LabeledField";

export function UserProfileView({
  onChange,
  onSave,
  persona,
  profile,
  status,
}: {
  onChange: (value: DevicePersona["user"]) => void;
  onSave: () => void;
  persona: DevicePersona;
  profile: DevicePersona["user"];
  status: string;
}) {
  const update = (key: keyof DevicePersona["user"], value: string) => {
    onChange({ ...profile, [key]: value });
  };

  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_300px]">
      <section className={cardClass}>
        <div className="mb-4 flex flex-wrap items-center justify-between gap-3">
          <div>
            <h2 className="text-lg font-semibold">Perfil do usuário</h2>
            <p className="text-sm text-slate-400">
              Identidade local usada pelo robô para contexto de conversa.
            </p>
          </div>
          <button className={primaryButtonClass} onClick={onSave} type="button">
            <CheckCircle2 size={17} />
            Salvar perfil
          </button>
        </div>

        <div className="grid gap-4 md:grid-cols-2">
          <LabeledField label="Nome do usuário">
            <input
              className={inputClass}
              maxLength={31}
              onChange={(e) => update("display_name", e.target.value)}
              value={profile.display_name}
            />
          </LabeledField>
          <LabeledField label="Identificador local">
            <input
              className={inputClass}
              maxLength={15}
              onChange={(e) => update("id", e.target.value)}
              value={profile.id}
            />
          </LabeledField>
          <LabeledField label="Relação">
            <select
              className={inputClass}
              onChange={(e) => update("relationship", e.target.value)}
              value={profile.relationship}
            >
              <option value="owner">Dono</option>
              <option value="friend">Amigo</option>
              <option value="family">Família</option>
              <option value="guest">Convidado</option>
            </select>
          </LabeledField>
          <LabeledField label="Idioma">
            <select
              className={inputClass}
              onChange={(e) => update("language", e.target.value)}
              value={profile.language}
            >
              <option value="pt-BR">Português</option>
              <option value="en-US">English</option>
            </select>
          </LabeledField>
          <LabeledField label="Nome do robô">
            <input
              className={inputClass}
              maxLength={23}
              onChange={(e) => update("robot_nickname", e.target.value)}
              value={profile.robot_nickname}
            />
          </LabeledField>
          <LabeledField label="Modo de persona">
            <select
              className={inputClass}
              onChange={(e) => update("persona_mode", e.target.value)}
              value={profile.persona_mode}
            >
              <option value="companion">Companheiro</option>
              <option value="focus_assistant">Foco</option>
              <option value="playful">Brincalhão</option>
              <option value="quiet_company">Companhia quieta</option>
            </select>
          </LabeledField>
          <LabeledField label="Estilo de interação">
            <select
              className={inputClass}
              onChange={(e) => update("interaction_style", e.target.value)}
              value={profile.interaction_style}
            >
              <option value="direct_warm">Direto e caloroso</option>
              <option value="brief">Breve</option>
              <option value="curious">Curioso</option>
              <option value="calm">Calmo</option>
            </select>
          </LabeledField>
        </div>

        {status !== "pronto" && (
          <p className="mt-3 text-sm text-slate-400">{status}</p>
        )}
      </section>

      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Persona atual</h2>
          <InfoRow label="Calor" value={ratioLabel(persona.warmth)} />
          <InfoRow label="Energia" value={ratioLabel(persona.energy)} />
          <InfoRow label="Curiosidade" value={ratioLabel(persona.curiosity)} />
          <InfoRow label="Confiança" value={ratioLabel(persona.trust)} />
        </section>
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Contexto</h2>
          <InfoRow label="Origem" value={persona.source ?? "firmware"} />
          <InfoRow label="Usuário" value={profile.display_name || "--"} />
          <InfoRow label="Robô" value={profile.robot_nickname || "--"} />
        </section>
      </aside>
    </div>
  );
}
