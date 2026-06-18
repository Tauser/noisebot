import { useMemo } from "react";
import {
  Activity, BatteryCharging, BrainCircuit, CalendarDays, ChevronRight,
  CloudSun, Home, MessageSquare, Mic, MicOff,
  PlugZap, RefreshCw, SlidersHorizontal, Terminal, UserRound, Wifi,
} from "lucide-react";
import { useAppState, type DevSection, type UserSection } from "./hooks/useAppState";
import { iconButtonClass } from "./lib/classes";
import { Vital } from "./components/Vital";
import { UserHomeView } from "./views/UserHomeView";
import { InteractionView } from "./views/InteractionView";
import { UserProfileView } from "./views/UserProfileView";
import { RoutineView } from "./views/RoutineView";
import { BasicSettingsView } from "./views/BasicSettingsView";
import { DevTelemetryView } from "./views/DevTelemetryView";
import { DevIntegrationsView } from "./views/DevIntegrationsView";
import { DevConsoleView } from "./views/DevConsoleView";
import { DevLlmDebugView } from "./views/DevLlmDebugView";

/* ─── Types ─────────────────────────────────────────────────── */
type NavItem<T extends string> = { id: T; label: string; icon: typeof Home };

const userNav: NavItem<UserSection>[] = [
  { id: "home",        label: "Início",    icon: Home },
  { id: "profile",     label: "Perfil",    icon: UserRound },
  { id: "interaction", label: "Interação", icon: MessageSquare },
  { id: "routine",     label: "Rotinas",   icon: CalendarDays },
  { id: "basics",      label: "Ajustes",   icon: SlidersHorizontal },
];

const devNav: NavItem<DevSection>[] = [
  { id: "telemetry",    label: "Telemetria",  icon: Activity },
  { id: "integrations", label: "Integrações", icon: PlugZap },
  { id: "console",      label: "Sistema",     icon: Terminal },
  { id: "llm",          label: "LLM Debug",   icon: BrainCircuit },
];

/* ─── Sub-components ─────────────────────────────────────────── */
function NavSection({ children, title }: { children: React.ReactNode; title: string }) {
  return (
    <div className="mb-3">
      <p className="mb-1 px-3 text-[10px] font-bold uppercase tracking-[0.14em] text-slate-500">
        {title}
      </p>
      <div className="grid grid-cols-2 gap-1 sm:grid-cols-3 md:grid-cols-1 md:gap-0.5">
        {children}
      </div>
    </div>
  );
}

function NavBtn({
  active,
  icon: Icon,
  label,
  onClick,
}: {
  active: boolean;
  icon: typeof Home;
  label: string;
  onClick: () => void;
}) {
  return (
    <button
      className={
        active
          ? "flex min-h-10 w-full items-center gap-3 rounded-lg bg-black/[0.18] px-3 text-sm font-semibold text-white"
          : "flex min-h-10 w-full items-center gap-3 rounded-lg px-3 text-sm text-slate-400 transition hover:bg-black/[0.10] hover:text-white"
      }
      onClick={onClick}
      type="button"
    >
      <Icon size={16} className={active ? "text-blue-400" : "text-slate-500"} />
      <span className="flex-1 text-left">{label}</span>
      <ChevronRight
        size={13}
        className={active ? "hidden text-blue-400 md:block" : "hidden text-slate-600 md:block"}
      />
    </button>
  );
}

/* ─── App ────────────────────────────────────────────────────── */
export function App() {
  const {
    mode, userSection, devSection,
    setAppMode, setUserSection, setDevSection,
    snapshot, appData, devicePersona, profileDraft, devData, llmDebug,
    volume, leds, opsToken, now,
    setProfileDraft, setCommandText, setVolume, setLeds,
    saveOpsToken,
    commandText, commandStatus, pendingCommand,
    routineStatus, volumeStatus, ledsStatus,
    profileStatus, followupStatus, silenceModeStatus, devStatus,
    refreshing, followupEnabled, followupWindowMs,
    refreshAll,
    createTimer, createAlarm, createReminder, toggleRoutine, removeRoutine,
    saveVolume, saveLeds, toggleFollowup, toggleSilenceMode,
    saveUserProfile, submitCommand,
    handleResetMetrics, handleRestartServer, handleAudioProcessorAction,
  } = useAppState();

  const activeItem = useMemo(() => {
    const list = mode === "user" ? userNav : devNav;
    const active = mode === "user" ? userSection : devSection;
    return list.find((i) => i.id === active) ?? list[0];
  }, [mode, userSection, devSection]);

  const currentTime = now.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit" });
  const micEnabled = !appData.settings.silent_mode;
  const micListening = micEnabled && snapshot.robot.state === "listening";

  return (
    <div
      className="flex min-h-screen flex-col"
      style={{ background: "#1a2035" }}
    >
      {/* ═══════════════════════════════════════════════════════
          Full-width top header (spans sidebar + content)
      ═══════════════════════════════════════════════════════ */}
      <header
        className="flex min-h-14 shrink-0 flex-wrap items-center justify-between gap-2 px-3 py-2 sm:px-5"
        style={{ background: "#1a2035" }}
      >
        {/* Logo */}
        <div className="flex items-center gap-2.5">
          <span className="inline-flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-blue-500 text-[11px] font-bold text-white shadow-lg shadow-blue-500/30">
            NB
          </span>
          <strong className="text-sm font-bold tracking-wide text-white">NoiseBot</strong>
        </div>

        {/* Status pills + refresh */}
        <div className="flex flex-wrap items-center gap-2">
          <Vital
            icon={Wifi}
            label={snapshot.robot.firmwareOnline ? "online" : "offline"}
            good={snapshot.robot.firmwareOnline}
            pulse={snapshot.robot.firmwareOnline}
          />
          <Vital
            icon={micEnabled ? Mic : MicOff}
            label={micListening ? "ouvindo" : micEnabled ? "mic ativo" : "mic desligado"}
            good={micEnabled}
            pulse={micListening}
          />
          <Vital icon={BatteryCharging} label={snapshot.robot.batteryLabel || "energia"} />
          <Vital
            icon={CloudSun}
            label={`${currentTime} · ${appData.advanced.location || "local"}`}
          />
          <button
            className={refreshing ? `${iconButtonClass} [&_svg]:animate-spin` : iconButtonClass}
            onClick={() => void refreshAll()}
            title="Atualizar"
            type="button"
          >
            <RefreshCw size={16} />
          </button>
        </div>
      </header>

      {/* ═══════════════════════════════════════════════════════
          Body: sidebar + main content side-by-side
      ═══════════════════════════════════════════════════════ */}
      <div className="flex flex-1 min-h-0 flex-col md:flex-row">

        {/* ── Sidebar ─────────────────────────────────────── */}
        <aside
          className="flex w-full shrink-0 flex-col overflow-y-auto px-3 py-3 md:w-60 md:py-5"
          style={{ background: "#1a2035" }}
        >
          {/* Mode toggle: User ↔ Dev */}
          <div className="mb-3 grid grid-cols-2 rounded-lg bg-black/[0.20] p-1 md:mb-5">
            {(["user", "dev"] as const).map((m) => (
              <button
                key={m}
                className={
                  mode === m
                    ? "rounded-md bg-blue-600 py-2 text-xs font-bold text-white shadow-sm transition"
                    : "rounded-md py-2 text-xs font-semibold text-slate-400 transition hover:text-white"
                }
                onClick={() => setAppMode(m)}
                type="button"
              >
                {m === "user" ? "User" : "Dev"}
              </button>
            ))}
          </div>

          {/* Navigation */}
          {mode === "user" ? (
            <>
              <NavSection title="App">
                {userNav.slice(0, 3).map((item) => (
                  <NavBtn
                    key={item.id}
                    active={userSection === item.id}
                    icon={item.icon}
                    label={item.label}
                    onClick={() => setUserSection(item.id)}
                  />
                ))}
              </NavSection>
              <NavSection title="Configurações">
                {userNav.slice(3).map((item) => (
                  <NavBtn
                    key={item.id}
                    active={userSection === item.id}
                    icon={item.icon}
                    label={item.label}
                    onClick={() => setUserSection(item.id)}
                  />
                ))}
              </NavSection>
            </>
          ) : (
            <NavSection title="Dev">
              {devNav.map((item) => (
                <NavBtn
                  key={item.id}
                  active={devSection === item.id}
                  icon={item.icon}
                  label={item.label}
                  onClick={() => setDevSection(item.id)}
                />
              ))}
            </NavSection>
          )}

          {/* Robot status — sidebar footer */}
          <div className="mt-auto hidden pt-4 md:block">
            <div className="rounded-lg bg-black/[0.18] px-3 py-3">
              <div className="mb-1 flex items-center gap-2">
                <span
                  className={
                    snapshot.robot.firmwareOnline
                      ? "pulse-dot relative h-2 w-2 rounded-full bg-emerald-500 text-emerald-500"
                      : "h-2 w-2 rounded-full bg-slate-600"
                  }
                />
                <span className="text-xs font-semibold text-slate-300">
                  {snapshot.robot.firmwareOnline ? "Firmware online" : "Firmware offline"}
                </span>
              </div>
              <p className="text-xs text-slate-500">{snapshot.robot.mood}</p>
            </div>
          </div>
        </aside>

        {/* ── Main content ─────────────────────────────────── */}
        <main className="min-w-0 flex-1 overflow-visible md:overflow-auto">
          <div className="px-3 py-4 sm:px-6 sm:py-5">
            {/* Breadcrumb — "YOU ARE HERE" Flatlogic style */}
            <div className="mb-6">
              <p className="mb-1 flex flex-wrap items-center gap-1 text-[10px] font-bold uppercase tracking-[0.14em] text-slate-500">
                VOCÊ ESTÁ AQUI
                <span className="mx-1 text-slate-600">›</span>
                <span>NoiseBot</span>
                <span className="text-slate-600">›</span>
                <span>{mode === "user" ? "App" : "Dev"}</span>
                <span className="text-slate-600">›</span>
                <span className="text-slate-300">{activeItem.label}</span>
              </p>
              <h1 className="text-2xl font-bold text-white">{activeItem.label}</h1>
            </div>

            {/* ── Views ─────────────────────────────────────── */}
            <div className="max-w-7xl">
              {mode === "user" && userSection === "home" && (
                <UserHomeView
                  appData={appData}
                  diagnostics={devData.diagnostics}
                  metrics={devData.metrics}
                  snapshot={snapshot}
                />
              )}
              {mode === "user" && userSection === "interaction" && (
                <InteractionView
                  commandStatus={commandStatus}
                  commandText={commandText}
                  pendingCommand={pendingCommand}
                  sessions={devData.metrics.recent_voice_sessions}
                  llmDebug={llmDebug}
                  onCommandChange={setCommandText}
                  onCommandSubmit={submitCommand}
                  opsToken={opsToken}
                  snapshot={snapshot}
                />
              )}
              {mode === "user" && userSection === "profile" && (
                <UserProfileView
                  persona={devicePersona}
                  profile={profileDraft}
                  onChange={setProfileDraft}
                  onSave={saveUserProfile}
                  status={profileStatus}
                  token={opsToken}
                />
              )}
              {mode === "user" && userSection === "routine" && (
                <RoutineView
                  items={appData.routine.items}
                  onCreateAlarm={createAlarm}
                  onCreateReminder={createReminder}
                  onCreateTimer={createTimer}
                  onRemove={removeRoutine}
                  onToggle={toggleRoutine}
                  status={routineStatus}
                  summary={appData.routine.summary}
                />
              )}
              {mode === "user" && userSection === "basics" && (
                <BasicSettingsView
                  appData={appData}
                  followupEnabled={followupEnabled}
                  followupStatus={followupStatus}
                  followupWindowMs={followupWindowMs}
                  leds={leds}
                  ledsStatus={ledsStatus}
                  onLedsChange={setLeds}
                  onSaveLeds={saveLeds}
                  onSaveVolume={saveVolume}
                  onToggleSilenceMode={toggleSilenceMode}
                  onToggleFollowup={toggleFollowup}
                  onVolumeChange={setVolume}
                  silenceModeEnabled={appData.settings.silent_mode}
                  silenceModeStatus={silenceModeStatus}
                  volume={volume}
                  volumeStatus={volumeStatus}
                />
              )}
              {mode === "dev" && devSection === "telemetry" && (
                <DevTelemetryView devData={devData} snapshot={snapshot} />
              )}
              {mode === "dev" && devSection === "integrations" && (
                <DevIntegrationsView devData={devData} snapshot={snapshot} opsToken={opsToken} />
              )}
              {mode === "dev" && devSection === "llm" && (
                <DevLlmDebugView />
              )}
              {mode === "dev" && devSection === "console" && (
                <DevConsoleView
                  devData={devData}
                  onAudioProcessorAction={handleAudioProcessorAction}
                  onOpsTokenChange={saveOpsToken}
                  onResetMetrics={handleResetMetrics}
                  onRestartServer={handleRestartServer}
                  opsToken={opsToken}
                  snapshot={snapshot}
                  status={devStatus}
                />
              )}
            </div>
          </div>
        </main>
      </div>
    </div>
  );
}
