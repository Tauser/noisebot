import { CheckCircle2, Camera, Trash2, UserRound, VideoOff } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import type { DevicePersona, EnrolledFace } from "../api";
import { listFaces, enrollFace, deleteFace, visionSnapshotUrl } from "../api";
import { cardClass, primaryButtonClass, secondaryButtonClass, inputClass, mutedTextClass } from "../lib/classes";
import { ratioLabel } from "../lib/format";
import { InfoRow } from "../components/InfoRow";
import { LabeledField } from "../components/LabeledField";

function FaceEnrollmentCard({
  token,
  profile,
}: {
  token: string;
  profile: DevicePersona["user"];
}) {
  const [faces, setFaces] = useState<EnrolledFace[]>([]);
  const [status, setStatus] = useState("");
  const [busy, setBusy] = useState(false);
  const [previewing, setPreviewing] = useState(false);
  const [imgUrl, setImgUrl] = useState<string | null>(null);
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const load = () => listFaces().then(setFaces).catch(() => {});
  useEffect(() => { load(); }, []);

  const startPreview = () => {
    setPreviewing(true);
    setImgUrl(visionSnapshotUrl());
    intervalRef.current = setInterval(() => setImgUrl(visionSnapshotUrl()), 2000);
  };

  const stopPreview = () => {
    setPreviewing(false);
    if (intervalRef.current) { clearInterval(intervalRef.current); intervalRef.current = null; }
    setImgUrl(null);
    setStatus("");
  };

  useEffect(() => () => {
    if (intervalRef.current) clearInterval(intervalRef.current);
  }, []);

  const enroll = async () => {
    setBusy(true);
    setStatus("Capturando…");
    try {
      await enrollFace(profile.id, profile.display_name || profile.id, token);
      setStatus("✓ Rosto cadastrado.");
      stopPreview();
      load();
    } catch (e) {
      setStatus(`✗ ${(e as Error).message}`);
    } finally {
      setBusy(false);
    }
  };

  const remove = async (userId: string, displayName: string) => {
    if (!confirm(`Remover "${displayName}" do reconhecimento facial?`)) return;
    try {
      await deleteFace(userId, token);
      load();
    } catch (e) {
      setStatus(`✗ ${(e as Error).message}`);
    }
  };

  return (
    <section className={cardClass}>
      <h2 className="mb-1 text-lg font-semibold">Reconhecimento Facial</h2>
      <p className={`${mutedTextClass} mb-4`}>
        Cadastre seu rosto para o robô te reconhecer pela câmera.
      </p>

      {/* Lista de cadastrados */}
      {faces.length === 0 ? (
        <p className={`${mutedTextClass} mb-4`}>Nenhum rosto cadastrado.</p>
      ) : (
        <ul className="mb-4 flex flex-col gap-2">
          {faces.map((f) => (
            <li
              key={f.user_id}
              className="flex items-center gap-3 rounded-lg bg-black/20 px-3 py-2"
            >
              <UserRound size={20} className="shrink-0 text-slate-400" />
              <div className="min-w-0 flex-1">
                <div className="truncate font-medium">{f.display_name || f.user_id}</div>
                <div className="truncate text-xs text-slate-500">
                  {f.user_id}
                  {f.enrolled_at
                    ? ` · ${new Date(f.enrolled_at * 1000).toLocaleDateString("pt-BR")}`
                    : ""}
                </div>
              </div>
              <button
                className="shrink-0 text-slate-500 transition hover:text-red-400"
                onClick={() => remove(f.user_id, f.display_name || f.user_id)}
                type="button"
              >
                <Trash2 size={16} />
              </button>
            </li>
          ))}
        </ul>
      )}

      {/* Preview da câmera */}
      {previewing && (
        <div className="mb-4">
          {imgUrl ? (
            <img
              src={imgUrl}
              alt="câmera"
              className="w-full max-w-xs rounded-lg border border-slate-700 object-contain"
              onError={() => setStatus("✗ Câmera indisponível.")}
            />
          ) : (
            <div className="flex h-40 w-full max-w-xs items-center justify-center rounded-lg bg-black/30 text-slate-500">
              <VideoOff size={32} />
            </div>
          )}
          <p className={`mt-1 text-xs ${mutedTextClass}`}>
            Posicione seu rosto e clique em Capturar.
          </p>
        </div>
      )}

      {/* Ações */}
      <div className="flex flex-wrap items-center gap-3">
        {!previewing ? (
          <button className={primaryButtonClass} onClick={startPreview} type="button">
            <Camera size={16} />
            Abrir câmera
          </button>
        ) : (
          <>
            <button
              className={primaryButtonClass}
              disabled={busy}
              onClick={enroll}
              type="button"
            >
              <Camera size={16} />
              Capturar e Salvar
            </button>
            <button className={secondaryButtonClass} onClick={stopPreview} type="button">
              Cancelar
            </button>
          </>
        )}
        {status && <span className="text-sm text-slate-400">{status}</span>}
      </div>

      {previewing && (
        <p className={`mt-2 text-xs ${mutedTextClass}`}>
          Cadastrando como <strong className="text-slate-300">{profile.display_name || profile.id}</strong> ({profile.id})
        </p>
      )}
    </section>
  );
}

export function UserProfileView({
  onChange,
  onSave,
  persona,
  profile,
  status,
  token,
}: {
  onChange: (value: DevicePersona["user"]) => void;
  onSave: () => void;
  persona: DevicePersona;
  profile: DevicePersona["user"];
  status: string;
  token: string;
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

      <div className="lg:col-span-2">
        <FaceEnrollmentCard token={token} profile={profile} />
      </div>
    </div>
  );
}
