import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  Activity,
  BatteryCharging,
  Bell,
  Brain,
  CalendarDays,
  Camera,
  CheckCircle2,
  Clock3,
  CloudSun,
  Cpu,
  Database,
  HardDrive,
  Home,
  MapPin,
  MessageSquare,
  Mic,
  MicOff,
  Pause,
  PlugZap,
  RefreshCw,
  SendHorizontal,
  Settings,
  Shield,
  SlidersHorizontal,
  Terminal,
  Thermometer,
  Timer,
  Trash2,
  Volume2,
  Wifi,
  Wrench,
} from "lucide-react";
import {
  AppData,
  BasicSettings,
  DashboardSnapshot,
  DevData,
  VisionAnalysis,
  VoiceSessionSummary,
  RoutineItem,
  analyzeVision,
  createAgendaItem,
  defaultAppData,
  deleteAgendaItem,
  loadDevData,
  loadAppData,
  loadSnapshot,
  observeVision,
  resetMetrics,
  restartServer,
  saveBasicSettings,
  sendDebugTranscript,
  updateAgendaItem,
  visionSnapshotUrl,
} from "./api";

type AppMode = "user" | "dev";
type UserSection = "home" | "interaction" | "routine" | "basics";
type DevSection = "telemetry" | "integrations" | "sensors" | "console";

type NavItem<T extends string> = {
  id: T;
  label: string;
  icon: typeof Home;
};

const userNav: NavItem<UserSection>[] = [
  { id: "home", label: "Início", icon: Home },
  { id: "interaction", label: "Interação", icon: MessageSquare },
  { id: "routine", label: "Rotinas", icon: CalendarDays },
  { id: "basics", label: "Ajustes", icon: SlidersHorizontal },
];

const devNav: NavItem<DevSection>[] = [
  { id: "telemetry", label: "Telemetria", icon: Activity },
  { id: "integrations", label: "Integrações", icon: PlugZap },
  { id: "sensors", label: "Sensores", icon: Camera },
  { id: "console", label: "Sistema", icon: Terminal },
];

const initialSnapshot: DashboardSnapshot = {
  robot: {
    name: "NoiseBot",
    state: "resting",
    mood: "iniciando",
    batteryLabel: "energia externa",
    serverOnline: false,
    firmwareOnline: false,
    sttStatus: "desconhecido",
    llmStatus: "desconhecido",
    ttsStatus: "desconhecido",
    mode: "local",
    provider: "local",
    model: "",
    lastError: "",
    lastTranscript: "",
    lastReply: "",
    lastRoute: "",
    lastTurnId: 0,
    lastUpdatedAt: "",
  },
  routine: {
    next: "Carregando rotina",
    timers: 0,
    alarms: 0,
    reminders: 0,
  },
  vision: {
    mode: "idle",
    lastObservation: "Aguardando captura",
    light: "normal",
    motion: "sem leitura",
    frameUrl: null,
  },
};

const defaultDevData: DevData = {
  metrics: {
    latency_ms: {},
    turns: {
      total: 0,
      local_intent: 0,
      llm: 0,
      fallback: 0,
      failed: 0,
      interrupted: 0,
    },
    tokens: {
      input: null,
      output: null,
    },
    last_voice_session: {},
    recent_voice_sessions: [],
    voice_alert: null,
    estimated_cost: null,
  },
  errors: [],
  logs: [],
  config: {},
  device: {
    server_online: false,
    firmware_online: false,
    transport_host: "",
    transport_port: 0,
    ops_port: 0,
    dry_run: false,
    features: [],
    supervisor: "unknown",
  },
  vision: {
    available: false,
    source: "unconfigured",
  },
  diagnostics: {
    available: false,
    source: "unavailable",
    errors: {},
  },
};

const cardClass = "rounded-xl border border-slate-200 bg-white p-4 shadow-sm";
const primaryButtonClass = "inline-flex min-h-10 items-center justify-center gap-2 rounded-lg bg-slate-900 px-4 font-semibold text-white transition hover:bg-slate-700";
const secondaryButtonClass = "inline-flex min-h-10 items-center justify-center gap-2 rounded-lg border border-slate-200 bg-white px-4 font-semibold text-slate-700 transition hover:bg-slate-50";
const inputClass = "min-h-10 w-full rounded-lg border border-slate-300 bg-white px-3 text-slate-900 outline-none";

function editableContext(mode: AppMode, userSection: UserSection, devSection: DevSection) {
  if (mode === "user") return userSection === "routine" || userSection === "basics";
  return devSection === "console";
}

async function safeLoadDevData() {
  try {
    return await loadDevData();
  } catch {
    return defaultDevData;
  }
}

export function App() {
  const [mode, setMode] = useState<AppMode>("user");
  const [userSection, setUserSection] = useState<UserSection>("home");
  const [devSection, setDevSection] = useState<DevSection>("telemetry");
  const contextRef = useRef({ mode: "user" as AppMode, userSection: "home" as UserSection, devSection: "telemetry" as DevSection });
  const [snapshot, setSnapshot] = useState<DashboardSnapshot>(initialSnapshot);
  const [appData, setAppData] = useState<AppData>(defaultAppData);
  const [devData, setDevData] = useState<DevData>(defaultDevData);
  const [volume, setVolume] = useState(defaultAppData.settings.volume);
  const [leds, setLeds] = useState(defaultAppData.settings.led_brightness);
  const [opsToken, setOpsToken] = useState(() => localStorage.getItem("noisebot_ops_token") ?? "");
  const [commandText, setCommandText] = useState("");
  const [commandStatus, setCommandStatus] = useState("pronto");
  const [routineStatus, setRoutineStatus] = useState("pronto");
  const [settingsStatus, setSettingsStatus] = useState("pronto");
  const [devStatus, setDevStatus] = useState("pronto");
  const [refreshing, setRefreshing] = useState(false);
  const [now, setNow] = useState(() => new Date());

  useEffect(() => {
    contextRef.current = { mode, userSection, devSection };
  }, [mode, userSection, devSection]);

  useEffect(() => {
    const timer = window.setInterval(() => setNow(new Date()), 30_000);
    return () => window.clearInterval(timer);
  }, []);

  const applyAppData = (data: AppData, syncControls = true) => {
    setAppData(data);
    if (syncControls) {
      setVolume(data.settings.volume);
      setLeds(data.settings.led_brightness);
    }
  };

  const refreshAll = async () => {
    setRefreshing(true);
    try {
      const [nextSnapshot, nextData, nextDevData] = await Promise.all([loadSnapshot(), loadAppData(), safeLoadDevData()]);
      setSnapshot(withRoutine(nextSnapshot, nextData));
      setDevData(nextDevData);
      const ctx = contextRef.current;
      applyAppData(nextData, !editableContext(ctx.mode, ctx.userSection, ctx.devSection));
    } finally {
      setRefreshing(false);
    }
  };

  useEffect(() => {
    let cancelled = false;
    const refresh = async () => {
      const [nextSnapshot, nextData, nextDevData] = await Promise.all([loadSnapshot(), loadAppData(), safeLoadDevData()]);
      if (!cancelled) {
        setSnapshot(withRoutine(nextSnapshot, nextData));
        setDevData(nextDevData);
        const ctx = contextRef.current;
        applyAppData(nextData, !editableContext(ctx.mode, ctx.userSection, ctx.devSection));
      }
    };
    void refresh();
    const interval = window.setInterval(() => void refresh(), 5000);
    return () => {
      cancelled = true;
      window.clearInterval(interval);
    };
  }, []);

  const setAppMode = (nextMode: AppMode) => {
    setMode(nextMode);
    if (nextMode === "user") {
      setUserSection("home");
    } else {
      setDevSection("telemetry");
    }
  };

  const saveOpsToken = (value: string) => {
    setOpsToken(value);
    if (value.trim()) {
      localStorage.setItem("noisebot_ops_token", value.trim());
    } else {
      localStorage.removeItem("noisebot_ops_token");
    }
  };

  const requireToken = (target: "command" | "routine" | "settings") => {
    const token = opsToken.trim();
    if (!token) {
      setMode("dev");
      setDevSection("console");
      const message = "token local obrigatório para executar esta ação";
      if (target === "command") {
        setCommandStatus(message);
      } else if (target === "routine") {
        setRoutineStatus(message);
      } else {
        setSettingsStatus(message);
      }
      return "";
    }
    return token;
  };

  const createTimer = async (title: string, durationMin: number) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando timer");
    try {
      const routine = await createAgendaItem("timer", { title, duration_min: durationMin }, token);
      updateRoutine(routine);
      setRoutineStatus("timer criado");
    } catch (error) {
      setRoutineStatus(errorMessage(error));
    }
  };

  const createAlarm = async (title: string, time: string, repeat: string) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando alarme");
    try {
      const routine = await createAgendaItem("alarm", { title, time, repeat }, token);
      updateRoutine(routine);
      setRoutineStatus("alarme criado");
    } catch (error) {
      setRoutineStatus(errorMessage(error));
    }
  };

  const createReminder = async (title: string, durationMin: number) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando lembrete");
    try {
      const routine = await createAgendaItem("reminder", { title, duration_min: durationMin }, token);
      updateRoutine(routine);
      setRoutineStatus("lembrete criado");
    } catch (error) {
      setRoutineStatus(errorMessage(error));
    }
  };

  const toggleRoutine = async (item: RoutineItem) => {
    const token = requireToken("routine");
    if (!token) return;
    try {
      const routine = await updateAgendaItem(item.id, { enabled: !item.enabled }, token);
      updateRoutine(routine);
      setRoutineStatus("rotina atualizada");
    } catch (error) {
      setRoutineStatus(errorMessage(error));
    }
  };

  const removeRoutine = async (item: RoutineItem) => {
    const token = requireToken("routine");
    if (!token) return;
    try {
      const routine = await deleteAgendaItem(item.id, token);
      updateRoutine(routine);
      setRoutineStatus("item removido");
    } catch (error) {
      setRoutineStatus(errorMessage(error));
    }
  };

  const updateRoutine = (routine: AppData["routine"]) => {
    const nextData = { ...appData, routine };
    setAppData(nextData);
    setSnapshot(withRoutine(snapshot, nextData));
  };

  const saveSettings = async () => {
    const token = requireToken("settings");
    if (!token) return;
    const settings: BasicSettings = {
      ...appData.settings,
      volume,
      led_brightness: leds,
    };
    setSettingsStatus("salvando");
    try {
      const saved = await saveBasicSettings(settings, token);
      applyAppData({ ...appData, settings: saved }, true);
      setSettingsStatus("ajustes salvos");
    } catch (error) {
      setSettingsStatus(errorMessage(error));
    }
  };

  const submitCommand = async () => {
    const text = commandText.trim();
    const token = requireToken("command");
    if (!text || !token) return;
    setCommandStatus("enviando");
    try {
      await sendDebugTranscript(text, token);
      setCommandText("");
      setCommandStatus("enviado");
      window.setTimeout(() => void refreshAll(), 800);
    } catch (error) {
      setCommandStatus(errorMessage(error));
    }
  };

  const handleResetMetrics = async () => {
    const token = opsToken.trim();
    if (!token) {
      setDevStatus("token local obrigatório para executar esta ação");
      setMode("dev");
      setDevSection("console");
      return;
    }
    setDevStatus("zerando métricas");
    try {
      await resetMetrics(token);
      setDevData(await safeLoadDevData());
      setDevStatus("métricas zeradas");
    } catch (error) {
      setDevStatus(errorMessage(error));
    }
  };

  const handleRestartServer = async () => {
    const token = opsToken.trim();
    if (!token) {
      setDevStatus("token local obrigatório para executar esta ação");
      setMode("dev");
      setDevSection("console");
      return;
    }
    setDevStatus("reinício solicitado");
    try {
      await restartServer(token);
      setDevStatus("server reiniciando");
    } catch (error) {
      setDevStatus(errorMessage(error));
    }
  };

  const title = useMemo(() => {
    const list = mode === "user" ? userNav : devNav;
    const active = mode === "user" ? userSection : devSection;
    return list.find((item) => item.id === active)?.label ?? "NoiseBot";
  }, [mode, userSection, devSection]);

  const currentTime = now.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit" });

  return (
    <main className="grid min-h-screen grid-cols-[240px_minmax(0,1fr)] bg-slate-100 max-lg:grid-cols-1">
      <aside className="border-r border-slate-200 bg-slate-950 px-4 py-5 text-white max-lg:border-r-0 max-lg:border-b">
        <div className="mb-5 grid gap-1 px-2">
          <strong className="text-lg">NoiseBot</strong>
          <span className="text-sm text-slate-400">{mode === "user" ? "Companheiro de mesa" : "Centro de comando"}</span>
        </div>

        <div className="mb-5 grid grid-cols-2 rounded-xl bg-slate-900 p-1 text-sm font-semibold">
          <button className={mode === "user" ? "rounded-lg bg-white px-3 py-2 text-slate-950" : "rounded-lg px-3 py-2 text-slate-400"} onClick={() => setAppMode("user")} type="button">
            User
          </button>
          <button className={mode === "dev" ? "rounded-lg bg-white px-3 py-2 text-slate-950" : "rounded-lg px-3 py-2 text-slate-400"} onClick={() => setAppMode("dev")} type="button">
            Dev
          </button>
        </div>

        <nav className="grid gap-1" aria-label="Navegação principal">
          {(mode === "user" ? userNav : devNav).map((item) => {
            const Icon = item.icon;
            const active = mode === "user" ? userSection === item.id : devSection === item.id;
            return (
              <button
                className={active ? "flex min-h-11 items-center gap-3 rounded-lg bg-slate-800 px-3 text-white" : "flex min-h-11 items-center gap-3 rounded-lg px-3 text-slate-300 hover:bg-slate-900 hover:text-white"}
                key={item.id}
                onClick={() => {
                  if (mode === "user") setUserSection(item.id as UserSection);
                  else setDevSection(item.id as DevSection);
                }}
                type="button"
              >
                <Icon size={18} />
                {item.label}
              </button>
            );
          })}
        </nav>
      </aside>

      <section className="grid min-w-0 grid-rows-[auto_1fr]">
        <header className="border-b border-slate-200 bg-white px-6 py-4">
          <div className="mx-auto flex max-w-7xl flex-wrap items-center justify-between gap-4">
            <div>
              <h1 className="text-2xl font-semibold tracking-tight text-slate-950">{title}</h1>
              <p className="text-sm text-slate-500">{snapshot.robot.mood}</p>
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <Vital icon={BatteryCharging} label={snapshot.robot.batteryLabel || "energia"} />
              <Vital icon={Wifi} label={snapshot.robot.firmwareOnline ? "online" : "offline"} good={snapshot.robot.firmwareOnline} />
              <Vital icon={snapshot.robot.state === "listening" ? Mic : MicOff} label={snapshot.robot.state === "listening" ? "ouvindo" : "mic"} />
              <Vital icon={CloudSun} label={`${currentTime} · ${appData.advanced.location || "local"}`} />
              <button className={refreshing ? "inline-flex h-10 w-10 items-center justify-center rounded-lg border border-slate-200 bg-white text-slate-600 [&_svg]:animate-spin" : "inline-flex h-10 w-10 items-center justify-center rounded-lg border border-slate-200 bg-white text-slate-600"} onClick={() => void refreshAll()} type="button">
                <RefreshCw size={18} />
              </button>
            </div>
          </div>
        </header>

        <div className="min-w-0 px-6 py-5">
          <div className="mx-auto max-w-7xl">
            {mode === "user" && userSection === "home" && (
              <UserHomeView appData={appData} onNavigate={setUserSection} snapshot={snapshot} />
            )}
            {mode === "user" && userSection === "interaction" && (
              <InteractionView
                commandStatus={commandStatus}
                commandText={commandText}
                onCommandChange={setCommandText}
                onCommandSubmit={submitCommand}
                snapshot={snapshot}
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
                leds={leds}
                onLedsChange={setLeds}
                onSave={saveSettings}
                onVolumeChange={setVolume}
                status={settingsStatus}
                volume={volume}
              />
            )}
            {mode === "dev" && devSection === "telemetry" && <DevTelemetryView devData={devData} snapshot={snapshot} />}
            {mode === "dev" && devSection === "integrations" && <DevIntegrationsView devData={devData} snapshot={snapshot} />}
            {mode === "dev" && devSection === "sensors" && <DevSensorsView devData={devData} snapshot={snapshot} />}
            {mode === "dev" && devSection === "console" && (
              <DevConsoleView
                devData={devData}
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
      </section>
    </main>
  );
}

function UserHomeView({
  appData,
  onNavigate,
  snapshot,
}: {
  appData: AppData;
  onNavigate: (section: UserSection) => void;
  snapshot: DashboardSnapshot;
}) {
  return (
    <div className="grid gap-4">
      <section className={cardClass}>
        <div className="mb-4 flex items-start justify-between gap-4">
          <div>
            <h2 className="text-lg font-semibold">Resumo do dia</h2>
            <p className="text-sm text-slate-500">{appData.routine.summary.next}</p>
          </div>
          <StatusPill ok={snapshot.robot.firmwareOnline} label={snapshot.robot.firmwareOnline ? "robô online" : "robô offline"} />
        </div>
        <div className="grid gap-3 md:grid-cols-4">
          <Metric label="Próximo" value={snapshot.routine.next} />
          <Metric label="Timers" value={String(snapshot.routine.timers)} />
          <Metric label="Alarmes" value={String(snapshot.routine.alarms)} />
          <Metric label="Lembretes" value={String(snapshot.routine.reminders)} />
        </div>
      </section>

      <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_320px]">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Widgets de mesa</h2>
          <div className="grid gap-3 md:grid-cols-2">
            <PlannedFeature icon={Timer} title="Pomodoro visual" description="Timer com estado visual no robô." />
            <PlannedFeature icon={Clock3} title="Acompanhamento do dia" description="Relógio, clima e foco no display." />
          </div>
        </section>

        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Modos de foco</h2>
          <div className="grid gap-2">
            <DisabledButton label="Modo silencioso" />
            <DisabledButton label="Não perturbe" />
            <button className={secondaryButtonClass} onClick={() => onNavigate("routine")} type="button">
              Abrir rotinas
            </button>
          </div>
        </section>
      </div>
    </div>
  );
}

function InteractionView({
  commandStatus,
  commandText,
  onCommandChange,
  onCommandSubmit,
  snapshot,
}: {
  commandStatus: string;
  commandText: string;
  onCommandChange: (value: string) => void;
  onCommandSubmit: () => void;
  snapshot: DashboardSnapshot;
}) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
      <section className={cardClass}>
        <h2 className="mb-3 text-lg font-semibold">Chat e comandos</h2>
        <div className="mb-4 grid gap-3">
          <TurnBubble label="Última fala" text={snapshot.robot.lastTranscript || "Sem transcrição recente."} />
          <TurnBubble label="Última resposta" text={lastReplyText(snapshot)} />
        </div>
        <form
          className="grid gap-3 md:grid-cols-[minmax(0,1fr)_auto]"
          onSubmit={(event) => {
            event.preventDefault();
            void onCommandSubmit();
          }}
        >
          <input className={inputClass} onChange={(event) => onCommandChange(event.target.value)} placeholder="Digite algo para o NoiseBot falar" value={commandText} />
          <button className={primaryButtonClass} type="submit">
            <SendHorizontal size={17} />
            Enviar
          </button>
        </form>
        <p className="mt-3 text-sm text-slate-500">{commandStatus}</p>
      </section>

      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Expressão visual</h2>
          <PlannedFeature icon={Shield} title="Galeria de olhos" description="Pixel art, reações e estilos visuais." />
        </section>
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Personalidade</h2>
          <PlannedFeature icon={Brain} title="Perfis de LLM" description="Assistente sério, companheiro irônico e outros estilos." />
        </section>
      </aside>
    </div>
  );
}

function RoutineView({
  items,
  onCreateAlarm,
  onCreateReminder,
  onCreateTimer,
  onRemove,
  onToggle,
  status,
  summary,
}: {
  items: RoutineItem[];
  onCreateAlarm: (title: string, time: string, repeat: string) => void;
  onCreateReminder: (title: string, durationMin: number) => void;
  onCreateTimer: (title: string, durationMin: number) => void;
  onRemove: (item: RoutineItem) => void;
  onToggle: (item: RoutineItem) => void;
  status: string;
  summary: AppData["routine"]["summary"];
}) {
  const [timerTitle, setTimerTitle] = useState("Timer");
  const [timerMin, setTimerMin] = useState(10);
  const [alarmTitle, setAlarmTitle] = useState("Alarme");
  const [alarmTime, setAlarmTime] = useState("07:30");
  const [alarmRepeat, setAlarmRepeat] = useState("diário");
  const [reminderTitle, setReminderTitle] = useState("Lembrete");
  const [reminderMin, setReminderMin] = useState(15);

  return (
    <div className="grid gap-4">
      <section className={cardClass}>
        <div className="flex flex-wrap items-start justify-between gap-4">
          <div>
            <h2 className="text-lg font-semibold">Rotinas e produtividade</h2>
            <p className="text-sm text-slate-500">{summary.next}</p>
          </div>
          <span className="text-sm font-medium text-slate-500">{status}</span>
        </div>
      </section>

      <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Itens</h2>
          <div className="grid gap-2">
            {items.length === 0 ? (
              <p className="rounded-lg border border-dashed border-slate-300 p-4 text-sm text-slate-500">Nenhum item criado.</p>
            ) : (
              items.map((item) => (
                <article className="grid grid-cols-[auto_minmax(0,1fr)_auto] items-center gap-3 rounded-lg border border-slate-200 p-3" key={item.id}>
                  <button className={item.enabled ? "inline-flex h-9 w-9 items-center justify-center rounded-lg bg-emerald-50 text-emerald-700" : "inline-flex h-9 w-9 items-center justify-center rounded-lg bg-slate-100 text-slate-500"} onClick={() => onToggle(item)} type="button">
                    {item.enabled ? <CheckCircle2 size={16} /> : <Pause size={16} />}
                  </button>
                  <div className="min-w-0">
                    <strong className="block truncate">{item.title}</strong>
                    <span className="text-sm text-slate-500">{kindLabel(item.kind)} · {item.detail || item.status}</span>
                  </div>
                  <button className="inline-flex h-9 w-9 items-center justify-center rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50" onClick={() => onRemove(item)} type="button">
                    <Trash2 size={17} />
                  </button>
                </article>
              ))
            )}
          </div>
        </section>

        <aside className="grid content-start gap-4">
          <RoutineForm icon={Timer} title="Novo timer" onSubmit={() => onCreateTimer(timerTitle, timerMin)}>
            <input className={inputClass} onChange={(event) => setTimerTitle(event.target.value)} value={timerTitle} />
            <NumberInput onChange={setTimerMin} value={timerMin} />
          </RoutineForm>
          <RoutineForm icon={Bell} title="Novo alarme" onSubmit={() => onCreateAlarm(alarmTitle, alarmTime, alarmRepeat)}>
            <input className={inputClass} onChange={(event) => setAlarmTitle(event.target.value)} value={alarmTitle} />
            <input className={inputClass} onChange={(event) => setAlarmTime(event.target.value)} type="time" value={alarmTime} />
            <select className={inputClass} onChange={(event) => setAlarmRepeat(event.target.value)} value={alarmRepeat}>
              <option value="diário">Diário</option>
              <option value="dias úteis">Dias úteis</option>
              <option value="fim de semana">Fim de semana</option>
              <option value="uma vez">Uma vez</option>
            </select>
          </RoutineForm>
          <RoutineForm icon={Clock3} title="Novo lembrete" onSubmit={() => onCreateReminder(reminderTitle, reminderMin)}>
            <input className={inputClass} onChange={(event) => setReminderTitle(event.target.value)} value={reminderTitle} />
            <NumberInput onChange={setReminderMin} value={reminderMin} />
          </RoutineForm>
        </aside>
      </div>
    </div>
  );
}

function BasicSettingsView({
  appData,
  leds,
  onLedsChange,
  onSave,
  onVolumeChange,
  status,
  volume,
}: {
  appData: AppData;
  leds: number;
  onLedsChange: (value: number) => void;
  onSave: () => void;
  onVolumeChange: (value: number) => void;
  status: string;
  volume: number;
}) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
      <section className={cardClass}>
        <div className="mb-4 flex flex-wrap items-center justify-between gap-3">
          <div>
            <h2 className="text-lg font-semibold">Hardware</h2>
            <p className="text-sm text-slate-500">Controles aplicados pelo server.</p>
          </div>
          <button className={primaryButtonClass} onClick={onSave} type="button">Salvar</button>
        </div>
        <div className="grid gap-4 md:grid-cols-2">
          <ControlPanel icon={Volume2} label="Volume" onChange={onVolumeChange} value={volume} />
          <ControlPanel icon={SlidersHorizontal} label="LEDs" onChange={onLedsChange} value={leds} />
        </div>
        <p className="mt-3 text-sm text-slate-500">{status}</p>
      </section>

      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Localização</h2>
          <InfoRow label="Cidade" value={appData.advanced.location || "Não definida"} />
          <InfoRow label="Fuso" value={appData.advanced.timezone || "Não definido"} />
        </section>
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Permissões</h2>
          <PlannedFeature icon={Camera} title="Captura por contexto" description="Controle fino de quando a câmera pode ser usada." />
        </section>
      </aside>
    </div>
  );
}

function DevTelemetryView({ devData, snapshot }: { devData: DevData; snapshot: DashboardSnapshot }) {
  const totalTurns = devData.metrics.turns.total ?? 0;
  const sttLatency = formatLatency(devData.metrics.latency_ms.stt);
  const llmLatency = formatLatency(devData.metrics.latency_ms.llm_total);
  const ttsLatency = formatLatency(devData.metrics.latency_ms.tts_first_audio);
  const firmware = devData.diagnostics;
  const diag = asRecord(firmware.diag);
  const health = asRecord(firmware.health);
  const version = asRecord(firmware.version);
  const wifi = asRecord(firmware.wifi);
  const audio = asRecord(firmware.audio);
  const camera = asRecord(firmware.camera);
  const touch = asRecord(firmware.touch);
  const storage = asRecord(health.storage);
  const ltm = asRecord(firmware.ltm);
  const voice = devData.metrics.last_voice_session ?? {};
  const recentVoice = devData.metrics.recent_voice_sessions ?? [];
  const voiceAlert = devData.metrics.voice_alert;
  const voiceSummary = summarizeVoiceSession(voice);
  const latencyBottleneck = voiceLatencyBottleneck(voice);
  const diagErrors = Object.entries(firmware.errors ?? {});

  return (
    <div className="grid gap-4 xl:grid-cols-2">
      <section className={`${cardClass} xl:col-span-2`}>
        <div className="mb-4 flex flex-wrap items-start justify-between gap-3">
          <div>
            <h2 className="text-lg font-semibold">Ciclo de voz</h2>
            <p className="text-sm text-slate-500">Mostra o que aconteceu quando o robô ouviu, pensou e tentou responder.</p>
          </div>
          <span className={voiceSummary.className}>{voiceSummary.label}</span>
        </div>
        {voiceAlert && <VoiceAlertBanner alert={voiceAlert} />}
        <div className="grid gap-3 lg:grid-cols-4">
          <VoiceStage label="Áudio" state={voiceStageState(voice, "audio")} detail={voiceStageDetail(voice, "audio")} />
          <VoiceStage label="STT" state={voiceStageState(voice, "stt")} detail={voiceStageDetail(voice, "stt")} />
          <VoiceStage label="Decisão" state={voiceStageState(voice, "decision")} detail={voiceStageDetail(voice, "decision")} />
          <VoiceStage label="Resposta" state={voiceStageState(voice, "reply")} detail={voiceStageDetail(voice, "reply")} />
        </div>
        <div className="mt-4 grid gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_260px]">
          <TurnBubble label="Transcrição" text={voice.transcript || snapshot.robot.lastTranscript || "Sem transcrição recente."} />
          <TurnBubble label="Resposta" text={voice.reply || lastReplyText(snapshot)} />
          <article className="rounded-xl border border-slate-200 bg-slate-50 p-3">
            <span className="text-xs font-bold uppercase text-slate-500">Gargalo provável</span>
            <strong className="mt-1 block text-sm text-slate-900">{latencyBottleneck.label}</strong>
            <p className="mt-1 text-sm text-slate-600">{latencyBottleneck.detail}</p>
          </article>
        </div>
      </section>

      <DiagnosticCard defaultOpen icon={Cpu} title="Hardware e build">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Firmware" value={snapshot.robot.firmwareOnline ? "online" : "offline"} />
          <Metric label="Estado" value={snapshot.robot.state} />
          <Metric label="Projeto" value={textValue(version.project)} />
          <Metric label="Versão" value={textValue(version.version)} />
          <Metric label="ESP-IDF" value={textValue(version.idf_ver)} />
          <Metric label="Build" value={`${textValue(version.build_date)} ${textValue(version.build_time)}`.trim()} />
          <Metric label="Health score" value={numberValue(health.health, "")} />
          <Metric label="Uptime" value={formatSeconds(readNumber(health.uptime_s) ?? readNumber(diag.uptime_s))} />
          <Metric label="Tasks" value={numberValue(health.task_count, "")} />
          <Metric label="FPS render" value={numberValue(diag.fps, " fps")} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard defaultOpen icon={Database} title="Memória">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="PSRAM livre" value={bytesValue(health.heap_psram_free)} />
          <Metric label="PSRAM mínimo" value={bytesValue(health.heap_psram_min)} />
          <Metric label="Maior bloco PSRAM" value={bytesValue(health.heap_psram_largest)} />
          <Metric label="Interna livre" value={bytesValue(health.heap_internal_free)} />
          <Metric label="Interna mínima" value={bytesValue(health.heap_internal_min)} />
          <Metric label="Maior bloco interno" value={bytesValue(health.heap_internal_largest)} />
          <Metric label="DMA livre" value={bytesValue(health.heap_dma_free)} />
          <Metric label="Maior bloco DMA" value={bytesValue(health.heap_dma_largest)} />
          <Metric label="Heap total livre" value={bytesValue(health.heap_dram_free)} />
          <Metric label="Heap total mínimo" value={bytesValue(health.heap_dram_min)} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={HardDrive} title="Armazenamento">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="SD montado" value={boolValue(storage.sd_mounted)} />
          <Metric label="SD livre" value={bytesValue(storage.sd_free_bytes)} />
          <Metric label="Config" value={firmware.config ? "exposta" : "não exposta"} />
          <Metric label="LTM" value={firmware.ltm ? "exposta" : "não exposta"} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard defaultOpen icon={Wifi} title="Rede e bridge">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Server" value={snapshot.robot.serverOnline ? "online" : "offline"} />
          <Metric label="Transporte" value={`${devData.device.transport_host || "--"}:${devData.device.transport_port || "--"}`} />
          <Metric label="Ops port" value={String(devData.device.ops_port || "--")} />
          <Metric label="Dry run" value={devData.device.dry_run ? "sim" : "não"} />
          <Metric label="HTTP robô" value={firmware.base_url || "--"} />
          <Metric label="Latência diag" value={numberValue(firmware.latency_ms, " ms")} />
          <Metric label="WiFi" value={boolValue(wifi.connected)} />
          <Metric label="SSID" value={textValue(wifi.ssid)} />
          <Metric label="IP" value={textValue(wifi.ip)} />
          <Metric label="RSSI" value={numberValue(wifi.rssi, " dBm")} />
          <Metric label="Bridge conectado" value={boolValue(diag.bridge_connected)} />
          <Metric label="Protocolo" value={numberValue(diag.bridge_protocol_v, "")} />
          <Metric label="Último RX bridge" value={numberValue(diag.bridge_last_rx_ms, " ms")} />
          <Metric label="Supervisor" value={devData.device.supervisor || "--"} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={Camera} title="Câmera">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Suportada" value={boolValue(camera.supported)} />
          <Metric label="Ativa" value={boolValue(camera.active)} />
          <Metric label="Modo" value={textValue(camera.mode)} />
          <Metric label="Resolução" value={`${numberValue(camera.mode_width, "")} x ${numberValue(camera.mode_height, "")}`} />
          <Metric label="Último JPEG" value={bytesValue(camera.last_jpeg_bytes)} />
          <Metric label="Última captura" value={numberValue(camera.last_capture_ms, " ms")} />
          <Metric label="Capturas" value={numberValue(camera.capture_count, "")} />
          <Metric label="Falhas" value={numberValue(camera.fail_count, "")} />
          <Metric label="Erro câmera" value={textValue(camera.last_error_name)} />
          <Metric label="DMA câmera" value={bytesValue(camera.heap_dma_free)} />
          <Metric label="Bloco DMA câmera" value={bytesValue(camera.heap_dma_largest)} />
          <Metric label="Interna câmera" value={bytesValue(camera.heap_internal_free)} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={Mic} title="Áudio e wake">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Listening" value={boolValue(diag.audio_listening)} />
          <Metric label="RMS" value={numberValue(diag.audio_rms, "")} />
          <Metric label="Volume" value={numberValue(audio.volume, "%")} />
          <Metric label="BPM" value={numberValue(audio.bpm, "")} />
          <Metric label="BPM confiança" value={numberValue(audio.bpm_conf, "%")} />
          <Metric label="Freq. dominante" value={numberValue(audio.dominant_freq, " Hz")} />
          <Metric label="Wake ativo" value={boolValue(diag.wake_active)} />
          <Metric label="Wake threshold" value={numberValue(diag.wake_threshold, "")} />
          <Metric label="Detecções wake" value={numberValue(diag.wake_detections, "")} />
          <Metric label="Modelo wake" value={textValue(diag.wake_model)} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard defaultOpen icon={Mic} title="Última sessão de voz">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Turno" value={numberValue(voice.turn_id, "")} />
          <Metric label="Resultado" value={textValue(voice.outcome)} />
          <Metric label="Estado final" value={textValue(voice.state)} />
          <Metric label="Fim da voz" value={textValue(voice.voice_end_reason)} />
          <Metric label="Descarte" value={textValue(voice.discard_reason)} />
          <Metric label="Duração fala" value={numberValue(voice.duration_ms, " ms")} />
          <Metric label="Chunks" value={numberValue(voice.chunk_count, "")} />
          <Metric label="Samples" value={numberValue(voice.total_samples, "")} />
          <Metric label="STT qualidade" value={textValue(voice.transcript_quality)} />
          <Metric label="No speech" value={numberValue(voice.no_speech_prob, "")} />
          <Metric label="Logprob" value={numberValue(voice.avg_logprob, "")} />
          <Metric label="Compressão" value={numberValue(voice.compression_ratio, "")} />
          <Metric label="Intent" value={textValue(voice.intent_name)} />
          <Metric label="Resposta chars" value={numberValue(voice.reply_chars, "")} />
          <Metric label="VOICE_END → STT" value={numberValue(voice.voice_end_to_stt_start_ms, " ms")} />
          <Metric label="STT" value={numberValue(voice.stt_ms, " ms")} />
          <Metric label="Fim de turno" value={numberValue(voice.end_of_turn_ms, " ms")} />
          <Metric label="TTS até 1º áudio" value={numberValue(voice.tts_first_audio_ms, " ms")} />
          <Metric label="1º áudio" value={numberValue(voice.first_audio_out_ms, " ms")} />
          <Metric label="1º áudio pós-fim" value={numberValue(voice.first_audio_after_voice_end_ms, " ms")} />
          <Metric label="Fala total" value={numberValue(voice.speech_total_ms, " ms")} />
          <Metric label="Erro estágio" value={textValue(voice.error_stage)} />
          <Metric label="Erro motivo" value={textValue(voice.error_reason)} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={Clock3} title="Histórico de voz">
        <VoiceSessionHistory sessions={recentVoice} />
      </DiagnosticCard>

      <DiagnosticCard icon={Activity} title="Touch, uso e sensores">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Touch pressed" value={boolValue(touch.pressed)} />
          <Metric label="Touch state" value={textValue(touch.state)} />
          <Metric label="Touch raw" value={numberValue(touch.raw, "")} />
          <Metric label="Touch filtered" value={numberValue(touch.filtered, "")} />
          <Metric label="Touch baseline" value={numberValue(touch.baseline, "")} />
          <Metric label="Último touch" value={textValue(touch.last_event)} />
          <Metric label="Sessões" value={numberValue(ltm.sessions, "")} />
          <Metric label="Horas vivo" value={numberValue(ltm.hours_alive, " h")} />
          <Metric label="Toques" value={numberValue(ltm.touch_count ?? diag.touch_count, "")} />
          <Metric label="Temperatura" value={<span className="inline-flex items-center gap-1"><Thermometer size={14} /> não exposta</span>} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={Activity} title="Métricas de turnos">
        <div className="grid gap-3 md:grid-cols-3">
          <Metric label="Total" value={String(totalTurns)} />
          <Metric label="Intent local" value={String(devData.metrics.turns.local_intent ?? 0)} />
          <Metric label="LLM" value={String(devData.metrics.turns.llm ?? 0)} />
          <Metric label="Falhas" value={String(devData.metrics.turns.failed ?? 0)} />
          <Metric label="Interrompidos" value={String(devData.metrics.turns.interrupted ?? 0)} />
          <Metric label="Fallback" value={String(devData.metrics.turns.fallback ?? 0)} />
        </div>
      </DiagnosticCard>

      <DiagnosticCard icon={Clock3} title="Latência">
        <div className="grid gap-3 md:grid-cols-3">
          <Metric label="STT" value={sttLatency} />
          <Metric label="LLM total" value={llmLatency} />
          <Metric label="TTS áudio" value={ttsLatency} />
        </div>
      </DiagnosticCard>

      {diagErrors.length > 0 && (
        <DiagnosticCard icon={Terminal} title="Endpoints sem resposta" wide>
          <div className="grid gap-2 md:grid-cols-2">
            {diagErrors.map(([key, value]) => (
              <InfoRow key={key} label={key} value={value} />
            ))}
          </div>
        </DiagnosticCard>
      )}
    </div>
  );
}

function DevIntegrationsView({ devData, snapshot }: { devData: DevData; snapshot: DashboardSnapshot }) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
      <section className={cardClass}>
        <h2 className="mb-3 text-lg font-semibold">Serviços</h2>
        <div className="grid gap-3 md:grid-cols-3">
          <ServiceTile label="STT" value={snapshot.robot.sttStatus} />
          <ServiceTile label="TTS" value={snapshot.robot.ttsStatus} />
          <ServiceTile label="LLM" value={snapshot.robot.llmStatus} />
        </div>
      </section>
      <section className={cardClass}>
        <h2 className="mb-3 text-lg font-semibold">Diagnóstico</h2>
        <InfoRow label="Último erro" value={snapshot.robot.lastError || "--"} />
        <InfoRow label="Rota" value={snapshot.robot.lastRoute || "--"} />
        <InfoRow label="Turno" value={String(snapshot.robot.lastTurnId || 0)} />
        <InfoRow label="Pipeline" value={devData.config.pipeline_mode || snapshot.robot.mode || "--"} />
        <InfoRow label="Modelo" value={devData.config.llm?.model || snapshot.robot.model || "--"} />
      </section>
      <section className={`${cardClass} lg:col-span-2`}>
        <h2 className="mb-3 text-lg font-semibold">Erros recentes</h2>
        <ErrorLog errors={devData.errors} />
      </section>
    </div>
  );
}

function DevSensorsView({ devData, snapshot }: { devData: DevData; snapshot: DashboardSnapshot }) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
      <CameraPanel snapshot={snapshot} />
      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Áudio raw</h2>
          <Metric label="Status STT" value={snapshot.robot.sttStatus} />
          <p className="mt-3 text-sm text-slate-500">Espectro em tempo real ainda depende de endpoint dedicado do firmware/server.</p>
        </section>
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Visão futura</h2>
          <InfoRow label="Disponível" value={devData.vision.available ? "sim" : "não"} />
          <InfoRow label="Origem" value={devData.vision.source || "--"} />
        </section>
      </aside>
    </div>
  );
}

function CameraPanel({ snapshot }: { snapshot: DashboardSnapshot }) {
  const [frameUrl, setFrameUrl] = useState<string | null>(snapshot.vision.frameUrl);
  const [status, setStatus] = useState("pronto");
  const [analysis, setAnalysis] = useState<VisionAnalysis | null>(null);

  useEffect(() => {
    setFrameUrl(snapshot.vision.frameUrl);
  }, [snapshot.vision.frameUrl]);

  const capture = async () => {
    setStatus("capturando");
    try {
      await observeVision();
      setFrameUrl(visionSnapshotUrl());
      setStatus("captura atualizada");
    } catch (error) {
      setStatus(errorMessage(error));
    }
  };

  const runAnalysis = async () => {
    setStatus("analisando");
    try {
      const result = await analyzeVision();
      setAnalysis(result);
      setFrameUrl(visionSnapshotUrl());
      setStatus("análise concluída");
    } catch (error) {
      setStatus(errorMessage(error));
    }
  };

  return (
    <section className={cardClass}>
      <div className="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 className="text-lg font-semibold">Câmera raw</h2>
          <p className="text-sm text-slate-500">Captura manual estável por enquanto.</p>
        </div>
        <StatusPill ok={status === "captura atualizada"} label={status} />
      </div>
      <div className="flex aspect-4/3 items-center justify-center overflow-hidden rounded-lg bg-slate-950 text-slate-300">
        {frameUrl ? <img alt="Câmera do NoiseBot" className="h-full w-full object-contain" src={frameUrl} /> : <span>Sem imagem</span>}
      </div>
      <button className={`${primaryButtonClass} mt-4`} onClick={() => void capture()} type="button">
        <Camera size={17} />
        Capturar foto
      </button>
      <button className={`${secondaryButtonClass} mt-2`} onClick={() => void runAnalysis()} type="button">
        <Activity size={17} />
        Analisar cena
      </button>
      {analysis && (
        <div className="mt-4 grid gap-2 rounded-xl border border-slate-200 bg-slate-50 p-3">
          <InfoRow label="Cena" value={analysis.observation.scene} />
          <InfoRow label="Resolução" value={`${analysis.observation.width}x${analysis.observation.height}`} />
          <InfoRow label="JPEG" value={`${analysis.observation.jpeg_bytes} bytes`} />
          <InfoRow label="Captura" value={`${analysis.observation.capture_ms} ms`} />
          <InfoRow label="Luz média" value={String(analysis.observation.luma_avg)} />
          <InfoRow label="Movimento" value={String(analysis.observation.motion_score)} />
          <InfoRow label="Face" value={analysis.face_detected ? `${analysis.face_count}` : "não"} />
        </div>
      )}
    </section>
  );
}

function DevConsoleView({
  devData,
  onResetMetrics,
  onRestartServer,
  opsToken,
  onOpsTokenChange,
  snapshot,
  status,
}: {
  devData: DevData;
  onResetMetrics: () => void;
  onRestartServer: () => void;
  opsToken: string;
  onOpsTokenChange: (value: string) => void;
  snapshot: DashboardSnapshot;
  status: string;
}) {
  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_340px]">
      <section className={cardClass}>
        <div className="mb-3 flex items-center justify-between gap-3">
          <h2 className="text-lg font-semibold">Logs do server</h2>
          <span className="rounded-full bg-slate-100 px-3 py-1 text-xs font-semibold text-slate-600">
            {devData.logs.length} linhas
          </span>
        </div>
        <div className="min-h-72 overflow-auto rounded-xl bg-slate-950 p-4 font-mono text-sm text-slate-300">
          <p className="text-slate-500">{">"} firmware: {snapshot.robot.firmwareOnline ? "online" : "offline"} | turnos: {devData.metrics.turns.total ?? 0}</p>
          {snapshot.robot.lastError && <p className="mt-2 text-rose-300">{">"} último erro: {snapshot.robot.lastError}</p>}
          {devData.logs.length === 0 && <p className="mt-3 text-slate-500">{">"} sem logs recentes capturados pelo server</p>}
          {devData.logs.map((entry, index) => (
            <p className="mt-2 break-words" key={`${entry.ts}-${entry.level}-${index}`}>
              <span className="text-slate-500">{formatTime(entry.ts)}</span>{" "}
              <span className={logLevelClass(entry.level)}>{entry.level.padEnd(7, " ")}</span>{" "}
              <span className="text-slate-500">{entry.logger}</span>{" "}
              {entry.message}
            </p>
          ))}
        </div>
        {devData.errors.length > 0 && (
          <div className="mt-3 rounded-lg border border-amber-200 bg-amber-50 p-3 text-sm text-amber-900">
            <strong className="block">Erros estruturados</strong>
            {devData.errors.slice(0, 3).map((error) => (
              <p className="mt-1" key={`${error.ts}-${error.turn_id}-${error.kind}`}>
                [{formatTime(error.ts)}] {error.kind} turn={error.turn_id} {error.message}
              </p>
            ))}
          </div>
        )}
      </section>
      <aside className="grid content-start gap-4">
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Token local</h2>
          {!opsToken.trim() && (
            <p className="mb-3 rounded-lg bg-amber-50 p-3 text-sm text-amber-800">
              Este host ainda não tem token salvo. Cole o mesmo token usado no localhost.
            </p>
          )}
          <label className="grid gap-2 text-sm font-semibold text-slate-600">
            Ops token
            <input className={inputClass} onChange={(event) => onOpsTokenChange(event.target.value)} placeholder="cole o token local" type="password" value={opsToken} />
          </label>
        </section>
        <section className={cardClass}>
          <h2 className="mb-3 text-lg font-semibold">Manutenção</h2>
          <div className="grid gap-2">
            <button className={secondaryButtonClass} onClick={onResetMetrics} type="button">
              Zerar métricas
            </button>
            <button className="inline-flex min-h-10 cursor-not-allowed items-center justify-center rounded-lg border border-slate-200 bg-slate-100 px-4 font-semibold text-slate-400" disabled onClick={onRestartServer} type="button">
              Reiniciar server
            </button>
            <DisabledButton label="OTA firmware" />
            <DisabledButton label="Reiniciar robô" />
            <DisabledButton label="Exportar backup" />
          </div>
          <p className="mt-3 text-sm text-slate-500">{status}</p>
        </section>
      </aside>
    </div>
  );
}

function RoutineForm({
  children,
  icon: Icon,
  onSubmit,
  title,
}: {
  children: ReactNode;
  icon: typeof Timer;
  onSubmit: () => void;
  title: string;
}) {
  return (
    <section className={cardClass}>
      <h2 className="mb-3 flex items-center gap-2 text-base font-semibold"><Icon size={18} /> {title}</h2>
      <form
        className="grid gap-3"
        onSubmit={(event) => {
          event.preventDefault();
          onSubmit();
        }}
      >
        {children}
        <button className={primaryButtonClass} type="submit">Criar</button>
      </form>
    </section>
  );
}

function ControlPanel({
  icon: Icon,
  label,
  onChange,
  value,
}: {
  icon: typeof Volume2;
  label: string;
  onChange: (value: number) => void;
  value: number;
}) {
  return (
    <section className="rounded-xl border border-slate-200 bg-slate-50 p-4">
      <h3 className="mb-3 flex items-center gap-2 font-semibold"><Icon size={18} /> {label}</h3>
      <strong className="mb-4 block text-4xl leading-none">{value}%</strong>
      <input className="w-full accent-slate-900" max="100" min="0" onChange={(event) => onChange(Number(event.target.value))} type="range" value={value} />
    </section>
  );
}

function NumberInput({ onChange, value }: { onChange: (value: number) => void; value: number }) {
  return (
    <input
      className={inputClass}
      min="1"
      max="1440"
      onChange={(event) => onChange(clampNumber(event.target.value, 1, 1440))}
      type="number"
      value={value}
    />
  );
}

function Metric({ label, value }: { label: string; value: ReactNode }) {
  return (
    <article className="min-h-20 rounded-lg border border-slate-200 bg-slate-50 p-3">
      <span className="block text-sm text-slate-500">{label}</span>
      <strong className="mt-1 block break-words text-base text-slate-950">{value}</strong>
    </article>
  );
}

function DiagnosticCard({
  children,
  defaultOpen = false,
  icon: Icon,
  title,
  wide = false,
}: {
  children: ReactNode;
  defaultOpen?: boolean;
  icon: typeof Cpu;
  title: string;
  wide?: boolean;
}) {
  return (
    <details className={`${cardClass} group ${wide ? "xl:col-span-2" : ""}`} open={defaultOpen}>
      <summary className="flex cursor-pointer list-none items-center justify-between gap-3 [&::-webkit-details-marker]:hidden">
        <span className="flex items-center gap-2 text-lg font-semibold">
          <Icon size={18} />
          {title}
        </span>
        <span className="rounded-full bg-slate-100 px-3 py-1 text-xs font-bold text-slate-500 group-open:hidden">
          abrir
        </span>
        <span className="hidden rounded-full bg-slate-100 px-3 py-1 text-xs font-bold text-slate-500 group-open:inline-flex">
          fechar
        </span>
      </summary>
      <div className="mt-4">{children}</div>
    </details>
  );
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex min-h-9 items-center justify-between gap-3 border-b border-slate-100 py-2 last:border-0">
      <span className="text-sm text-slate-500">{label}</span>
      <strong className="break-words text-right text-sm">{value}</strong>
    </div>
  );
}

function Vital({ good, icon: Icon, label }: { good?: boolean; icon: typeof BatteryCharging; label: string }) {
  return (
    <span className={good ? "inline-flex min-h-10 items-center gap-2 rounded-lg bg-emerald-50 px-3 text-sm font-semibold text-emerald-700" : "inline-flex min-h-10 items-center gap-2 rounded-lg bg-slate-100 px-3 text-sm font-semibold text-slate-600"}>
      <Icon size={16} />
      {label}
    </span>
  );
}

function StatusPill({ label, ok }: { label: string; ok: boolean }) {
  return (
    <span className={ok ? "inline-flex rounded-full bg-emerald-50 px-3 py-1 text-sm font-bold text-emerald-700" : "inline-flex rounded-full bg-slate-100 px-3 py-1 text-sm font-bold text-slate-500"}>
      {label}
    </span>
  );
}

function TurnBubble({ label, text }: { label: string; text: string }) {
  return (
    <article className="rounded-xl border border-slate-200 bg-slate-50 p-3">
      <span className="text-xs font-bold uppercase text-slate-500">{label}</span>
      <p className="mt-1 text-sm text-slate-800">{text}</p>
    </article>
  );
}

function PlannedFeature({ description, icon: Icon, title }: { description: string; icon: typeof Timer; title: string }) {
  return (
    <article className="rounded-xl border border-dashed border-slate-300 bg-slate-50 p-4">
      <div className="mb-2 flex items-center gap-2 font-semibold text-slate-700">
        <Icon size={18} />
        {title}
      </div>
      <p className="text-sm text-slate-500">{description}</p>
      <span className="mt-3 inline-flex rounded-full bg-slate-200 px-2.5 py-1 text-xs font-bold text-slate-600">planejado</span>
    </article>
  );
}

function DisabledButton({ label }: { label: string }) {
  return (
    <button className="inline-flex min-h-10 cursor-not-allowed items-center justify-center rounded-lg border border-slate-200 bg-slate-100 px-4 font-semibold text-slate-400" disabled type="button">
      {label}
    </button>
  );
}

function ServiceTile({ label, value }: { label: string; value: string }) {
  const ok = value === "ok" || value === "ready" || value === "enabled";
  return (
    <article className="rounded-xl border border-slate-200 bg-slate-50 p-4">
      <span className={ok ? "mb-3 block h-2 w-2 rounded-full bg-emerald-500" : "mb-3 block h-2 w-2 rounded-full bg-amber-500"} />
      <strong className="block">{label}</strong>
      <span className="text-sm text-slate-500">{value || "--"}</span>
    </article>
  );
}

function VoiceStage({
  detail,
  label,
  state,
}: {
  detail: string;
  label: string;
  state: "ok" | "warn" | "error" | "idle";
}) {
  const styles = {
    ok: "border-emerald-200 bg-emerald-50 text-emerald-800",
    warn: "border-amber-200 bg-amber-50 text-amber-800",
    error: "border-red-200 bg-red-50 text-red-800",
    idle: "border-slate-200 bg-slate-50 text-slate-700",
  };
  return (
    <article className={`rounded-xl border p-3 ${styles[state]}`}>
      <span className="text-xs font-bold uppercase opacity-75">{label}</span>
      <strong className="mt-1 block text-sm">{voiceStateLabel(state)}</strong>
      <p className="mt-1 text-sm opacity-85">{detail}</p>
    </article>
  );
}

function VoiceAlertBanner({ alert }: { alert: NonNullable<DevData["metrics"]["voice_alert"]> }) {
  const style = alert.level === "error"
    ? "border-red-200 bg-red-50 text-red-800"
    : "border-amber-200 bg-amber-50 text-amber-800";
  return (
    <div className={`mb-4 rounded-lg border p-3 ${style}`}>
      <strong className="block text-sm">{alert.title}</strong>
      <span className="text-sm">{alert.detail || "sem detalhe"}</span>
    </div>
  );
}

function VoiceSessionHistory({ sessions }: { sessions: VoiceSessionSummary[] }) {
  if (sessions.length === 0) {
    return <p className="rounded-lg border border-dashed border-slate-300 p-4 text-sm text-slate-500">Nenhuma sessão de voz registrada ainda.</p>;
  }
  return (
    <div className="grid gap-2">
      {sessions.slice(0, 8).map((session, index) => (
        <article className="rounded-lg border border-slate-200 bg-slate-50 p-3" key={`${session.turn_id ?? "turn"}-${index}`}>
          <div className="flex flex-wrap items-center justify-between gap-2">
            <strong className="text-sm text-slate-900">Turno {session.turn_id ?? "--"}</strong>
            <span className={voiceOutcomeClass(session.outcome)}>{session.outcome || "--"}</span>
          </div>
          {(session.transcript || session.reply) && (
            <div className="mt-2 grid gap-2 text-sm text-slate-600">
              {session.transcript && <p><strong className="text-slate-800">Ouvi:</strong> {session.transcript}</p>}
              {session.reply && <p><strong className="text-slate-800">Respondi:</strong> {session.reply}</p>}
            </div>
          )}
          <div className="mt-2 grid gap-2 text-sm text-slate-600 md:grid-cols-2">
            <InfoRow label="Duração" value={numberValue(session.duration_ms, " ms")} />
            <InfoRow label="Chunks" value={numberValue(session.chunk_count, "")} />
            <InfoRow label="Qualidade" value={textValue(session.transcript_quality)} />
            <InfoRow label="Descarte" value={textValue(session.discard_reason)} />
          </div>
        </article>
      ))}
    </div>
  );
}

function summarizeVoiceSession(session: VoiceSessionSummary) {
  const outcome = session.outcome || "";
  const discard = session.discard_reason || "";
  const quality = (session.transcript_quality || "").toLowerCase();
  if (!session.turn_id) {
    return {
      label: "sem turno recente",
      className: "rounded-full bg-slate-100 px-3 py-1 text-sm font-bold text-slate-500",
    };
  }
  if (outcome === "failed" || session.error_stage) {
    return {
      label: `falhou: ${session.error_stage || session.error_reason || "erro"}`,
      className: "rounded-full bg-red-100 px-3 py-1 text-sm font-bold text-red-700",
    };
  }
  if (outcome === "audio_rejected" || discard === "audio_curto" || discard === "audio_longo") {
    return {
      label: `não ouvi direito: ${discard || outcome}`,
      className: "rounded-full bg-amber-100 px-3 py-1 text-sm font-bold text-amber-700",
    };
  }
  if (outcome === "stt_rejected" || discard.startsWith("stt_") || (quality && quality !== "good" && quality !== "ok")) {
    return {
      label: "não entendeu e pediu repetição",
      className: "rounded-full bg-amber-100 px-3 py-1 text-sm font-bold text-amber-700",
    };
  }
  if (outcome === "interrupted" || outcome === "cancelled") {
    return {
      label: "interrompido",
      className: "rounded-full bg-sky-100 px-3 py-1 text-sm font-bold text-sky-700",
    };
  }
  if (session.reply || session.reply_chars) {
    return {
      label: "respondeu",
      className: "rounded-full bg-emerald-100 px-3 py-1 text-sm font-bold text-emerald-700",
    };
  }
  return {
    label: outcome || "sem resposta",
    className: "rounded-full bg-slate-100 px-3 py-1 text-sm font-bold text-slate-500",
  };
}

function voiceStageState(
  session: VoiceSessionSummary,
  stage: "audio" | "stt" | "decision" | "reply",
): "ok" | "warn" | "error" | "idle" {
  const outcome = session.outcome || "";
  const discard = session.discard_reason || "";
  const quality = (session.transcript_quality || "").toLowerCase();
  if (!session.turn_id) return "idle";
  if (session.error_stage) {
    if (stage === "reply" || session.error_stage === stage) return "error";
  }
  if (stage === "audio") {
    if (outcome === "audio_rejected" || discard === "audio_curto" || discard === "audio_longo") return "warn";
    return session.total_samples ? "ok" : "idle";
  }
  if (stage === "stt") {
    if (outcome === "stt_rejected" || discard.startsWith("stt_") || (quality && quality !== "good" && quality !== "ok")) return "warn";
    return session.transcript_quality || session.transcript ? "ok" : "idle";
  }
  if (stage === "decision") {
    if (outcome === "failed") return "error";
    return session.intent_name || outcome ? "ok" : "idle";
  }
  if (outcome === "failed") return "error";
  if (session.reply || session.reply_chars) return "ok";
  if (outcome === "stt_rejected" || discard.startsWith("stt_")) return "warn";
  return "idle";
}

function voiceStageDetail(
  session: VoiceSessionSummary,
  stage: "audio" | "stt" | "decision" | "reply",
) {
  if (!session.turn_id) return "aguardando uso";
  if (stage === "audio") {
    return `${numberValue(session.duration_ms, " ms")} · ${numberValue(session.chunk_count, " chunks")}`;
  }
  if (stage === "stt") {
    return `${textValue(session.transcript_quality)} · ${numberValue(session.stt_ms, " ms")}`;
  }
  if (stage === "decision") {
    return `${textValue(session.intent_name || session.outcome)} · ${numberValue(session.end_of_turn_ms, " ms")}`;
  }
  if (session.reply || session.reply_chars) {
    return `${numberValue(session.first_audio_after_voice_end_ms, " ms")} até 1º áudio`;
  }
  return textValue(session.discard_reason || session.error_reason || "sem fala enviada");
}

function voiceStateLabel(state: "ok" | "warn" | "error" | "idle") {
  if (state === "ok") return "ok";
  if (state === "warn") return "atenção";
  if (state === "error") return "erro";
  return "sem dado";
}

function voiceLatencyBottleneck(session: VoiceSessionSummary) {
  const firstAudio = readNumber(session.first_audio_after_voice_end_ms);
  const stt = readNumber(session.stt_ms);
  const voiceEndToStt = readNumber(session.voice_end_to_stt_start_ms) ?? 0;
  const ttsAudio = readNumber(session.tts_first_audio_ms);
  const postSttToAudio = firstAudio !== null && stt !== null
    ? Math.max(0, firstAudio - voiceEndToStt - stt)
    : null;
  const candidates = [
    { key: "STT", value: stt },
    { key: "decisão e TTS", value: postSttToAudio },
    { key: "TTS", value: ttsAudio },
  ].filter((item): item is { key: string; value: number } => item.value !== null);
  if (candidates.length === 0) {
    return { label: "sem dados suficientes", detail: "faça um teste de voz para medir o ciclo." };
  }
  const highest = candidates.reduce((best, item) => (item.value > best.value ? item : best));
  if (highest.value < 1500) {
    return { label: "ciclo saudável", detail: `${highest.key} foi o maior trecho medido: ${highest.value} ms.` };
  }
  const speechTotal = readNumber(session.speech_total_ms);
  const firstAudioNote = firstAudio !== null ? ` Tempo total até 1º áudio: ${firstAudio} ms.` : "";
  const speechNote = speechTotal !== null ? ` Fala total: ${speechTotal} ms, apenas informativo.` : "";
  return { label: highest.key, detail: `maior atraso medido no último turno: ${highest.value} ms.${firstAudioNote}${speechNote}` };
}

function voiceOutcomeClass(outcome: string | undefined) {
  if (outcome === "failed") return "rounded-full bg-red-100 px-2 py-1 text-xs font-bold text-red-700";
  if (outcome === "audio_rejected" || outcome === "stt_rejected" || outcome === "cancelled") {
    return "rounded-full bg-amber-100 px-2 py-1 text-xs font-bold text-amber-700";
  }
  return "rounded-full bg-emerald-100 px-2 py-1 text-xs font-bold text-emerald-700";
}

function lastReplyText(snapshot: DashboardSnapshot) {
  if (snapshot.robot.lastReply) return snapshot.robot.lastReply;
  if (snapshot.robot.lastTurnId > 0) {
    return "Sem resposta registrada. Talvez eu não tenha entendido.";
  }
  return "Sem resposta recente.";
}

function logLevelClass(level: string) {
  const normalized = level.toUpperCase();
  if (normalized === "ERROR" || normalized === "CRITICAL") return "text-rose-300";
  if (normalized === "WARNING" || normalized === "WARN") return "text-amber-300";
  if (normalized === "DEBUG") return "text-sky-300";
  return "text-emerald-300";
}

function ErrorLog({ errors }: { errors: DevData["errors"] }) {
  if (errors.length === 0) {
    return <p className="rounded-lg border border-dashed border-slate-300 p-4 text-sm text-slate-500">Nenhum erro recente registrado.</p>;
  }
  return (
    <div className="grid gap-2">
      {errors.map((error) => (
        <article className="rounded-lg border border-slate-200 bg-slate-50 p-3" key={`${error.ts}-${error.turn_id}-${error.kind}`}>
          <div className="flex flex-wrap items-center justify-between gap-2">
            <strong className="text-sm text-slate-900">{error.kind}</strong>
            <span className="text-xs font-semibold text-slate-500">{formatTime(error.ts)}</span>
          </div>
          <p className="mt-1 text-sm text-slate-600">{error.message || "sem mensagem"}</p>
          <p className="mt-1 text-xs text-slate-500">
            turn={error.turn_id || 0} provider={error.provider || "--"} model={error.model || "--"}
          </p>
        </article>
      ))}
    </div>
  );
}

function withRoutine(snapshot: DashboardSnapshot, data: AppData): DashboardSnapshot {
  return {
    ...snapshot,
    routine: {
      next: data.routine.summary.next,
      timers: data.routine.summary.timers,
      alarms: data.routine.summary.alarms,
      reminders: data.routine.summary.reminders,
    },
  };
}

function formatLatency(value: { p50: number | null; p95: number | null; count: number } | undefined) {
  if (!value || value.count <= 0) return "--";
  const p50 = value.p50 === null ? "--" : `${value.p50} ms`;
  const p95 = value.p95 === null ? "--" : `${value.p95} ms`;
  return `p50 ${p50} / p95 ${p95}`;
}

function asRecord(value: unknown): Record<string, unknown> {
  if (value && typeof value === "object" && !Array.isArray(value)) {
    return value as Record<string, unknown>;
  }
  return {};
}

function readNumber(value: unknown): number | null {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim() !== "") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
}

function textValue(value: unknown) {
  if (value === null || value === undefined || value === "") return "--";
  return String(value);
}

function numberValue(value: unknown, suffix: string) {
  const number = readNumber(value);
  if (number === null) return "--";
  return `${number}${suffix}`;
}

function boolValue(value: unknown) {
  if (typeof value !== "boolean") return "--";
  return value ? "sim" : "não";
}

function bytesValue(value: unknown) {
  const bytes = readNumber(value);
  if (bytes === null) return "--";
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${bytes} B`;
}

function formatSeconds(value: number | null) {
  if (value === null) return "--";
  if (value >= 3600) return `${(value / 3600).toFixed(1)} h`;
  if (value >= 60) return `${Math.round(value / 60)} min`;
  return `${value} s`;
}

function formatTime(ts: number) {
  if (!Number.isFinite(ts) || ts <= 0) return "--";
  return new Date(ts * 1000).toLocaleTimeString("pt-BR", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function kindLabel(kind: RoutineItem["kind"]) {
  if (kind === "timer") return "Timer";
  if (kind === "alarm") return "Alarme";
  return "Lembrete";
}

function clampNumber(value: string, min: number, max: number) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return min;
  return Math.max(min, Math.min(max, Math.round(parsed)));
}

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "erro inesperado";
}
