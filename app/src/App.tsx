import { useEffect, useMemo, useState } from "react";
import {
  Activity,
  Bell,
  Bot,
  CalendarDays,
  Camera,
  CheckCircle2,
  ChevronRight,
  Clock3,
  Cpu,
  Eye,
  Gauge,
  HardDrive,
  Home,
  Languages,
  ListChecks,
  Mic2,
  Monitor,
  Network,
  Pause,
  Play,
  Power,
  RefreshCw,
  Settings,
  SendHorizontal,
  SlidersHorizontal,
  Sparkles,
  ShieldCheck,
  SunMedium,
  Timer,
  UserRound,
  Volume2,
  Wifi,
} from "lucide-react";
import {
  AppData,
  BasicSettings,
  DashboardSnapshot,
  RoutineItem,
  VisionAnalysis,
  VisionObservation,
  analyzeVision,
  createAgendaItem,
  defaultAppData,
  deleteAgendaItem,
  loadAppData,
  loadSnapshot,
  observeVision,
  RobotState,
  saveBasicSettings,
  sendDebugTranscript,
  updateAgendaItem,
  visionSnapshotUrl,
} from "./api";

type SectionId = "home" | "routine" | "vision" | "basics" | "profile" | "settings";

type NavItem = {
  id: SectionId;
  label: string;
  description: string;
  icon: typeof Home;
};

const navItems: NavItem[] = [
  { id: "home", label: "Início", description: "Resumo vivo do robô", icon: Home },
  { id: "routine", label: "Rotina", description: "Timers, alarmes e agenda", icon: CalendarDays },
  { id: "vision", label: "Visão", description: "Câmera e monitoramento", icon: Camera },
  { id: "basics", label: "Ajustes", description: "Volume, LEDs e modos", icon: SlidersHorizontal },
  { id: "profile", label: "Perfil", description: "Nome, idioma e jeito", icon: UserRound },
  { id: "settings", label: "Configurações", description: "Rede, OTA e device", icon: Settings },
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
    lastObservation: "Aguardando primeira captura",
    light: "normal",
    motion: "sem leitura",
    frameUrl: null,
  },
};

const stateLabels: Record<RobotState, string> = {
  online: "online",
  offline: "offline",
  listening: "ouvindo",
  thinking: "pensando",
  speaking: "falando",
  resting: "descansando",
};

export function App() {
  const [active, setActive] = useState<SectionId>("home");
  const [snapshot, setSnapshot] = useState<DashboardSnapshot>(initialSnapshot);
  const [appData, setAppData] = useState<AppData>(defaultAppData);
  const [monitoring, setMonitoring] = useState(false);
  const [volume, setVolume] = useState(62);
  const [leds, setLeds] = useState(48);
  const [silentMode, setSilentMode] = useState(false);
  const [doNotDisturb, setDoNotDisturb] = useState(false);
  const [nightMode, setNightMode] = useState(false);
  const [reduceBrightnessAtNight, setReduceBrightnessAtNight] = useState(true);
  const [confirmLoudSounds, setConfirmLoudSounds] = useState(true);
  const [subtleLeds, setSubtleLeds] = useState(false);
  const [commandText, setCommandText] = useState("");
  const [commandStatus, setCommandStatus] = useState("pronto");
  const [routineStatus, setRoutineStatus] = useState("pronto");
  const [settingsStatus, setSettingsStatus] = useState("pronto");
  const [opsToken, setOpsToken] = useState(() => localStorage.getItem("noisebot_ops_token") ?? "");
  const [isRefreshing, setIsRefreshing] = useState(false);

  const applyAppData = (data: AppData) => {
    setAppData(data);
    setVolume(data.settings.volume);
    setLeds(data.settings.led_brightness);
    setSilentMode(data.settings.silent_mode);
    setDoNotDisturb(data.settings.do_not_disturb);
    setNightMode(data.settings.night_mode);
    setReduceBrightnessAtNight(data.settings.reduce_brightness_at_night);
    setConfirmLoudSounds(data.settings.confirm_loud_sounds);
    setSubtleLeds(data.settings.subtle_leds);
  };

  const refreshAll = async () => {
    setIsRefreshing(true);
    try {
      const [snapshotData, stateData] = await Promise.all([loadSnapshot(), loadAppData()]);
      setSnapshot(snapshotWithRoutine(snapshotData, stateData));
      applyAppData(stateData);
      return snapshotData;
    } finally {
      setIsRefreshing(false);
    }
  };

  useEffect(() => {
    let cancelled = false;
    const refresh = async () => {
      const [snapshotData, stateData] = await Promise.all([loadSnapshot(), loadAppData()]);
      if (!cancelled) {
        setSnapshot(snapshotWithRoutine(snapshotData, stateData));
        applyAppData(stateData);
      }
    };
    void refresh();
    const interval = window.setInterval(() => {
      void refresh();
    }, 4000);
    return () => {
      cancelled = true;
      window.clearInterval(interval);
    };
  }, []);

  const activeItem = useMemo(
    () => navItems.find((item) => item.id === active) ?? navItems[0],
    [active],
  );

  const saveOpsToken = (value: string) => {
    setOpsToken(value);
    localStorage.setItem("noisebot_ops_token", value);
  };

  const requireOpsToken = (target: "routine" | "settings") => {
    if (opsToken.trim()) {
      return opsToken.trim();
    }
    setActive("settings");
    if (target === "routine") {
      setRoutineStatus("configure o token primeiro");
    } else {
      setSettingsStatus("configure o token primeiro");
    }
    return "";
  };

  const createTimer = async (minutes: number, title?: string) => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("criando timer");
    try {
      const routine = await createAgendaItem(
        "timer",
        { title: title?.trim() || `Timer de ${minutes} min`, duration_min: minutes },
        token,
      );
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("timer criado");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao criar");
    }
  };

  const createAlarm = async (time = "07:30", title = "Alarme diário", repeat = "diário") => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("criando alarme");
    try {
      const routine = await createAgendaItem(
        "alarm",
        { title: title.trim() || "Alarme", time, repeat: repeat.trim() || "diário" },
        token,
      );
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("alarme criado");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao criar");
    }
  };

  const createReminder = async (minutes = 15, title = "Novo lembrete") => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("criando lembrete");
    try {
      const routine = await createAgendaItem(
        "reminder",
        { title: title.trim() || "Lembrete", duration_min: minutes },
        token,
      );
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("lembrete criado");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao criar");
    }
  };

  const toggleRoutineItem = async (item: RoutineItem) => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("atualizando");
    try {
      const routine = await updateAgendaItem(item.id, { enabled: !item.enabled }, token);
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("rotina atualizada");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao atualizar");
    }
  };

  const editRoutineItem = async (item: RoutineItem, payload: Record<string, unknown>) => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("salvando item");
    try {
      const routine = await updateAgendaItem(item.id, payload, token);
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("item atualizado");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao salvar");
    }
  };

  const removeRoutineItem = async (item: RoutineItem) => {
    const token = requireOpsToken("routine");
    if (!token) {
      return;
    }
    setRoutineStatus("removendo");
    try {
      const routine = await deleteAgendaItem(item.id, token);
      const nextData = { ...appData, routine };
      setAppData(nextData);
      setSnapshot(snapshotWithRoutine(snapshot, nextData));
      setRoutineStatus("item removido");
    } catch (error) {
      setRoutineStatus(error instanceof Error ? error.message : "falha ao remover");
    }
  };

  const currentSettings = (): BasicSettings => ({
    volume,
    display_brightness: appData.settings.display_brightness,
    led_brightness: leds,
    silent_mode: silentMode,
    do_not_disturb: doNotDisturb,
    night_mode: nightMode,
    reduce_brightness_at_night: reduceBrightnessAtNight,
    confirm_loud_sounds: confirmLoudSounds,
    subtle_leds: subtleLeds,
  });

  const saveSettings = async () => {
    const token = requireOpsToken("settings");
    if (!token) {
      return;
    }
    setSettingsStatus("salvando");
    try {
      const settings = await saveBasicSettings(currentSettings(), token);
      applyAppData({ ...appData, settings });
      setSettingsStatus("ajustes salvos");
    } catch (error) {
      setSettingsStatus(error instanceof Error ? error.message : "falha ao salvar");
    }
  };

  const submitCommand = async () => {
    const text = commandText.trim();
    if (!text) {
      return;
    }
    if (!opsToken.trim()) {
      setActive("settings");
      setCommandStatus("configure o token primeiro");
      return;
    }
    setCommandStatus("enviando");
    try {
      await sendDebugTranscript(text, opsToken.trim());
      setCommandText("");
      setCommandStatus("processando");
      window.setTimeout(() => {
      void refreshAll().then(() => setCommandStatus("atualizado"));
      }, 800);
    } catch (error) {
      setCommandStatus(error instanceof Error ? error.message : "falha ao enviar");
    }
  };

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <span className="brand-mark">
            <Bot size={22} />
          </span>
          <div>
            <strong>NoiseBot</strong>
            <span>Companion local</span>
          </div>
        </div>

        <nav className="nav-list" aria-label="Navegacao principal">
          {navItems.map((item) => {
            const Icon = item.icon;
            return (
              <button
                className={`nav-button ${active === item.id ? "active" : ""}`}
                key={item.id}
                onClick={() => setActive(item.id)}
                type="button"
              >
                <Icon size={18} />
                <span>
                  <strong>{item.label}</strong>
                  <em>{item.description}</em>
                </span>
              </button>
            );
          })}
        </nav>

        <div className="sidebar-status">
          <StatusDot ok={snapshot.robot.serverOnline} />
          <span>{snapshot.robot.serverOnline ? "server conectado" : "server offline"}</span>
        </div>
      </aside>

      <section className="workspace">
        <header className="topbar">
          <div>
            <p className="eyebrow">NoiseBot App</p>
            <h1>{activeItem.label}</h1>
            <span className="section-hint">{activeItem.description}</span>
          </div>
          <div className="topbar-actions">
            <form
              className="command-box"
              onSubmit={(event) => {
                event.preventDefault();
                void submitCommand();
              }}
            >
              <input
                aria-label="Comando rápido"
                onChange={(event) => setCommandText(event.target.value)}
                placeholder="Digite um comando rápido"
                value={commandText}
              />
              <button title="Enviar comando" type="submit">
                <SendHorizontal size={17} />
              </button>
            </form>
            <span className="command-status">{commandStatus}</span>
            <button
              className={`icon-button ${isRefreshing ? "spinning" : ""}`}
              onClick={() => void refreshAll()}
              title="Atualizar"
              type="button"
            >
              <RefreshCw size={18} />
            </button>
            <ConnectionPill
              firmwareOnline={snapshot.robot.firmwareOnline}
              serverOnline={snapshot.robot.serverOnline}
            />
          </div>
        </header>

        {active === "home" && <HomeView snapshot={snapshot} onNavigate={setActive} />}
        {active === "routine" && (
          <RoutineView
            items={appData.routine.items}
            onCreateAlarm={createAlarm}
            onCreateReminder={createReminder}
            onCreateTimer={createTimer}
            onRemoveItem={removeRoutineItem}
            onToggleItem={toggleRoutineItem}
            onUpdateItem={editRoutineItem}
            status={routineStatus}
            summary={appData.routine.summary}
          />
        )}
        {active === "vision" && (
          <VisionView
            monitoring={monitoring}
            setMonitoring={setMonitoring}
            snapshot={snapshot}
          />
        )}
        {active === "basics" && (
          <BasicsView
            leds={leds}
            setLeds={setLeds}
            setVolume={setVolume}
            settings={{
              confirmLoudSounds,
              doNotDisturb,
              nightMode,
              reduceBrightnessAtNight,
              silentMode,
              subtleLeds,
            }}
            settingsStatus={settingsStatus}
            onSave={saveSettings}
            setters={{
              setConfirmLoudSounds,
              setDoNotDisturb,
              setNightMode,
              setReduceBrightnessAtNight,
              setSilentMode,
              setSubtleLeds,
            }}
            volume={volume}
          />
        )}
        {active === "profile" && <ProfileView />}
        {active === "settings" && (
          <SettingsView
            opsToken={opsToken}
            setOpsToken={saveOpsToken}
            snapshot={snapshot}
          />
        )}
      </section>
    </main>
  );
}

function snapshotWithRoutine(snapshot: DashboardSnapshot, appData: AppData): DashboardSnapshot {
  return {
    ...snapshot,
    routine: {
      next: appData.routine.summary.next,
      timers: appData.routine.summary.timers,
      alarms: appData.routine.summary.alarms,
      reminders: appData.routine.summary.reminders,
    },
  };
}

function HomeView({
  snapshot,
  onNavigate,
}: {
  snapshot: DashboardSnapshot;
  onNavigate: (section: SectionId) => void;
}) {
  return (
    <div className="content-grid home-grid">
      <section className="hero-panel">
        <div className="hero-copy">
          <span className={`state-badge ${snapshot.robot.state}`}>
            {stateLabels[snapshot.robot.state]}
          </span>
          <h2>{snapshot.robot.name} está {snapshot.robot.mood}</h2>
          <p>
            Tudo que importa hoje fica aqui: rotina, câmera, voz e ajustes rápidos,
            sem cara de painel técnico.
          </p>
          <div className="status-lane">
            <span>
              <StatusDot ok={snapshot.robot.serverOnline} />
              Server
            </span>
            <span>
              <StatusDot ok={snapshot.robot.firmwareOnline} />
              Firmware
            </span>
            <span>
              <Power size={15} />
              {snapshot.robot.batteryLabel}
            </span>
          </div>
          <div className="quick-actions">
            <button onClick={() => onNavigate("routine")} type="button">
              <Timer size={18} />
              Novo timer
            </button>
            <button onClick={() => onNavigate("vision")} type="button">
              <Eye size={18} />
              Ver câmera
            </button>
            <button onClick={() => onNavigate("basics")} type="button">
              <Volume2 size={18} />
              Ajustes rápidos
            </button>
          </div>
        </div>

        <div className="robot-dock">
          <div className="robot-avatar">
            <div className="face-screen">
              <div className="eye left" />
              <div className="eye right" />
              <div className="mouth" />
            </div>
          </div>
          <div className="next-card">
            <span>Próximo</span>
            <strong>{snapshot.routine.next}</strong>
            <button onClick={() => onNavigate("routine")} type="button">
              Abrir rotina
              <ChevronRight size={16} />
            </button>
          </div>
        </div>
      </section>

      <StatPanel icon={Timer} label="Timers ativos" value={snapshot.routine.timers.toString()} tone="teal" />
      <StatPanel icon={Bell} label="Alarmes ligados" value={snapshot.routine.alarms.toString()} tone="amber" />
      <StatPanel icon={CalendarDays} label="Lembretes hoje" value={snapshot.routine.reminders.toString()} tone="coral" />

      <section className="panel span-2">
        <PanelTitle icon={Mic2} title="Último turno" action={`#${snapshot.robot.lastTurnId}`} />
        <div className="turn-card">
          <div>
            <span>Você</span>
            <strong>{snapshot.robot.lastTranscript || "Nenhuma fala recente"}</strong>
          </div>
          <div>
            <span>NoiseBot</span>
            <strong>{snapshot.robot.lastReply || "Aguardando interação"}</strong>
          </div>
          <em>{snapshot.robot.lastRoute || "sem rota"}</em>
        </div>
      </section>

      <section className="panel">
        <PanelTitle icon={Bot} title="Runtime" />
        <InfoRow label="Modo" value={snapshot.robot.mode} />
        <InfoRow label="LLM" value={snapshot.robot.provider} />
        <InfoRow label="STT" value={snapshot.robot.sttStatus} />
        <InfoRow label="TTS" value={snapshot.robot.ttsStatus} />
      </section>

      <section className="panel span-2">
        <PanelTitle icon={Camera} title="Visão rápida" action="ver" />
        <div className="vision-strip">
          <div className="camera-placeholder">
            <Camera size={30} />
            <span>câmera em espera</span>
          </div>
          <div className="compact-list">
            <InfoRow label="Cena" value={snapshot.vision.lastObservation} />
            <InfoRow label="Luz" value={snapshot.vision.light} />
            <InfoRow label="Movimento" value={snapshot.vision.motion} />
          </div>
        </div>
      </section>

      <section className="panel">
        <PanelTitle icon={Gauge} title="Saúde" />
        <div className="health-list">
          <HealthItem label="Server" ok={snapshot.robot.serverOnline} />
          <HealthItem label="Firmware" ok={snapshot.robot.firmwareOnline} />
          <HealthItem label="Audio" ok />
          <HealthItem label="Camera" ok />
        </div>
      </section>
    </div>
  );
}

function RoutineView({
  items,
  onCreateAlarm,
  onCreateReminder,
  onCreateTimer,
  onRemoveItem,
  onToggleItem,
  onUpdateItem,
  status,
  summary,
}: {
  items: RoutineItem[];
  onCreateAlarm: (time?: string, title?: string, repeat?: string) => void;
  onCreateReminder: (minutes?: number, title?: string) => void;
  onCreateTimer: (minutes: number, title?: string) => void;
  onRemoveItem: (item: RoutineItem) => void;
  onToggleItem: (item: RoutineItem) => void;
  onUpdateItem: (item: RoutineItem, payload: Record<string, unknown>) => void;
  status: string;
  summary: AppData["routine"]["summary"];
}) {
  const activeItem = items.find((item) => item.enabled) ?? null;
  const [timerTitle, setTimerTitle] = useState("Timer");
  const [timerMinutes, setTimerMinutes] = useState(10);
  const [alarmTitle, setAlarmTitle] = useState("Alarme");
  const [alarmTime, setAlarmTime] = useState("07:30");
  const [alarmRepeat, setAlarmRepeat] = useState("diário");
  const [reminderTitle, setReminderTitle] = useState("Lembrete");
  const [reminderMinutes, setReminderMinutes] = useState(15);
  const [editingId, setEditingId] = useState("");
  const [editTitle, setEditTitle] = useState("");
  const [editTime, setEditTime] = useState("07:30");
  const [editRepeat, setEditRepeat] = useState("diário");
  const [editDuration, setEditDuration] = useState(10);

  const startEdit = (item: RoutineItem) => {
    setEditingId(item.id);
    setEditTitle(item.title);
    setEditTime(item.time || "07:30");
    setEditRepeat(item.repeat || "diário");
    setEditDuration(item.duration_min || 10);
  };

  const saveEdit = (item: RoutineItem) => {
    const base = { title: editTitle.trim() || item.title };
    if (item.kind === "alarm") {
      onUpdateItem(item, { ...base, time: editTime, repeat: editRepeat });
    } else {
      onUpdateItem(item, { ...base, duration_min: editDuration });
    }
    setEditingId("");
  };

  return (
    <div className="content-grid routine-grid">
      <section className="section-hero routine-hero span-2">
        <div>
          <p className="eyebrow">Rotina</p>
          <h2>O dia do NoiseBot, sem complicação</h2>
          <span>Timers, alarmes e lembretes aparecem aqui como uma linha do tempo simples.</span>
        </div>
        <button className="hero-action" onClick={() => onCreateTimer(10)} type="button">
          <Timer size={18} />
          Criar timer
        </button>
      </section>

      <section className="panel routine-now">
        <PanelTitle icon={Clock3} title="Agora" />
        <strong>{summary.next}</strong>
        <span>{activeItem?.detail || "Nada pendente agora"}</span>
        <div className="routine-progress">
          <span />
        </div>
        <small className="panel-status">{status}</small>
      </section>

      <section className="panel span-2">
        <PanelTitle icon={CalendarDays} title="Rotina de hoje" action="novo" />
        <div className="timeline">
          {items.length === 0 && (
            <article className="timeline-item empty">
              <span className="timeline-icon">
                <CalendarDays size={18} />
              </span>
              <div>
                <strong>Nenhum item criado</strong>
                <span>Use os atalhos para adicionar timers e alarmes.</span>
              </div>
              <em>vazio</em>
            </article>
          )}
          {items.map((item) => {
            const Icon = routineIcon(item.kind);
            return (
              <article className={`timeline-item ${item.enabled ? "" : "muted"}`} key={item.id}>
                <span className="timeline-icon">
                  <Icon size={18} />
                </span>
                <div>
                  <strong>{item.title}</strong>
                  <span>{item.detail}</span>
                </div>
                <em>{item.status}</em>
                <div className="timeline-actions">
                  <button onClick={() => startEdit(item)} title="Editar" type="button">
                    <SlidersHorizontal size={15} />
                  </button>
                  <button onClick={() => onToggleItem(item)} title="Ligar ou desligar" type="button">
                    {item.enabled ? <Pause size={15} /> : <Play size={15} />}
                  </button>
                  <button onClick={() => onRemoveItem(item)} title="Remover" type="button">
                    ×
                  </button>
                </div>
                <small className="timeline-source">
                  {item.source === "firmware" ? "salvo no robô" : "salvo no server"}
                </small>
                {editingId === item.id && (
                  <form
                    className="timeline-edit"
                    onSubmit={(event) => {
                      event.preventDefault();
                      saveEdit(item);
                    }}
                  >
                    <label>
                      Nome
                      <input
                        maxLength={80}
                        onChange={(event) => setEditTitle(event.target.value)}
                        value={editTitle}
                      />
                    </label>
                    {item.kind === "alarm" ? (
                      <>
                        <label>
                          Hora
                          <input
                            onChange={(event) => setEditTime(event.target.value)}
                            type="time"
                            value={editTime}
                          />
                        </label>
                        <label>
                          Repetição
                          <select onChange={(event) => setEditRepeat(event.target.value)} value={editRepeat}>
                            <option value="diário">Diário</option>
                            <option value="dias úteis">Dias úteis</option>
                            <option value="fim de semana">Fim de semana</option>
                            <option value="uma vez">Uma vez</option>
                          </select>
                        </label>
                      </>
                    ) : (
                      <label>
                        Minutos
                        <input
                          min="1"
                          max="1440"
                          onChange={(event) => setEditDuration(clampNumber(event.target.value, 1, 1440))}
                          type="number"
                          value={editDuration}
                        />
                      </label>
                    )}
                    <button type="submit">Salvar</button>
                    <button onClick={() => setEditingId("")} type="button">Cancelar</button>
                  </form>
                )}
              </article>
            );
          })}
        </div>
      </section>

      <section className="panel">
        <PanelTitle icon={Clock3} title="Novo timer" />
        <p className="panel-copy">Crie com o nome e duração que quiser.</p>
        <form
          className="routine-form"
          onSubmit={(event) => {
            event.preventDefault();
            onCreateTimer(timerMinutes, timerTitle);
          }}
        >
          <label>
            Nome
            <input
              maxLength={80}
              onChange={(event) => setTimerTitle(event.target.value)}
              value={timerTitle}
            />
          </label>
          <label>
            Minutos
            <input
              min="1"
              max="1440"
              onChange={(event) => setTimerMinutes(clampNumber(event.target.value, 1, 1440))}
              type="number"
              value={timerMinutes}
            />
          </label>
          <button type="submit"><Timer size={16} />Criar timer</button>
        </form>
        <div className="timer-composer">
          <button onClick={() => onCreateTimer(5)} type="button"><Timer size={16} />5 min</button>
          <button onClick={() => onCreateTimer(10)} type="button"><Timer size={16} />10 min</button>
          <button onClick={() => onCreateTimer(25)} type="button"><Timer size={16} />25 min</button>
          <button onClick={() => onCreateTimer(45)} type="button"><SlidersHorizontal size={16} />45 min</button>
        </div>
      </section>

      <section className="panel">
        <PanelTitle icon={Bell} title="Alarmes" />
        <InfoRow label="Ligados" value={summary.alarms.toString()} />
        <InfoRow label="Timers" value={summary.timers.toString()} />
        <InfoRow label="Lembretes" value={summary.reminders.toString()} />
        <form
          className="routine-form compact-form"
          onSubmit={(event) => {
            event.preventDefault();
            onCreateAlarm(alarmTime, alarmTitle, alarmRepeat);
          }}
        >
          <label>
            Nome
            <input
              maxLength={80}
              onChange={(event) => setAlarmTitle(event.target.value)}
              value={alarmTitle}
            />
          </label>
          <label>
            Hora
            <input
              onChange={(event) => setAlarmTime(event.target.value)}
              type="time"
              value={alarmTime}
            />
          </label>
          <label>
            Repetição
            <select onChange={(event) => setAlarmRepeat(event.target.value)} value={alarmRepeat}>
              <option value="diário">Diário</option>
              <option value="dias úteis">Dias úteis</option>
              <option value="fim de semana">Fim de semana</option>
              <option value="uma vez">Uma vez</option>
            </select>
          </label>
          <button type="submit"><Bell size={16} />Criar alarme</button>
        </form>
        <div className="timer-composer compact-actions">
          <button onClick={() => onCreateAlarm()} type="button"><Bell size={16} />07:30</button>
        </div>
      </section>

      <section className="panel span-2">
        <PanelTitle icon={ListChecks} title="Novo lembrete" />
        <form
          className="routine-form reminder-form"
          onSubmit={(event) => {
            event.preventDefault();
            onCreateReminder(reminderMinutes, reminderTitle);
          }}
        >
          <label>
            O que lembrar
            <input
              maxLength={80}
              onChange={(event) => setReminderTitle(event.target.value)}
              value={reminderTitle}
            />
          </label>
          <label>
            Avisar em
            <input
              min="1"
              max="1440"
              onChange={(event) => setReminderMinutes(clampNumber(event.target.value, 1, 1440))}
              type="number"
              value={reminderMinutes}
            />
          </label>
          <button type="submit"><CalendarDays size={16} />Criar lembrete</button>
        </form>
      </section>
    </div>
  );
}

function routineIcon(kind: RoutineItem["kind"]) {
  if (kind === "timer") {
    return Timer;
  }
  if (kind === "alarm") {
    return Bell;
  }
  return CalendarDays;
}

function clampNumber(value: string, min: number, max: number) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return min;
  }
  return Math.max(min, Math.min(max, Math.round(parsed)));
}

function VisionView({
  monitoring,
  setMonitoring,
  snapshot,
}: {
  monitoring: boolean;
  setMonitoring: (value: boolean) => void;
  snapshot: DashboardSnapshot;
}) {
  const [frameUrl, setFrameUrl] = useState<string | null>(snapshot.vision.frameUrl);
  const [observation, setObservation] = useState<VisionObservation | null>(null);
  const [analysis, setAnalysis] = useState<VisionAnalysis | null>(null);
  const [visionStatus, setVisionStatus] = useState("pronto");

  useEffect(() => {
    setFrameUrl(snapshot.vision.frameUrl);
  }, [snapshot.vision.frameUrl]);

  const captureFrame = async () => {
    setVisionStatus("capturando");
    try {
      const nextObservation = await observeVision();
      setObservation(nextObservation);
      setFrameUrl(visionSnapshotUrl());
      setVisionStatus("captura atualizada");
    } catch (error) {
      setVisionStatus(error instanceof Error ? error.message : "falha na câmera");
    }
  };

  const describeScene = async () => {
    setVisionStatus("analisando");
    try {
      const nextAnalysis = await analyzeVision();
      setAnalysis(nextAnalysis);
      setObservation(nextAnalysis.observation);
      setFrameUrl(visionSnapshotUrl());
      setVisionStatus("análise atualizada");
    } catch (error) {
      setVisionStatus(error instanceof Error ? error.message : "falha na análise");
    }
  };

  useEffect(() => {
    if (!monitoring) {
      return undefined;
    }
    let cancelled = false;
    const tick = async () => {
      try {
        const nextObservation = await observeVision();
        if (!cancelled) {
          setObservation(nextObservation);
          setFrameUrl(visionSnapshotUrl());
          setVisionStatus("monitorando");
        }
      } catch (error) {
        if (!cancelled) {
          setVisionStatus(error instanceof Error ? error.message : "monitoramento falhou");
        }
      }
    };
    void tick();
    const interval = window.setInterval(() => {
      void tick();
    }, 3000);
    return () => {
      cancelled = true;
      window.clearInterval(interval);
    };
  }, [monitoring]);

  const observedScene = observation?.scene ?? snapshot.vision.lastObservation;
  const observedLight = observation ? `${Math.round(observation.luma_avg)} luma` : snapshot.vision.light;
  const observedMotion = observation ? `${observation.motion_score}` : snapshot.vision.motion;
  const captureLabel = observation
    ? `${Math.round(observation.jpeg_bytes / 1024)}KB / ${observation.capture_ms}ms`
    : "--";
  const personLabel = analysis
    ? analysis.face_detected
      ? `${analysis.face_count} rosto(s)`
      : "não detectada"
    : "sem análise";

  return (
    <div className="content-grid vision-grid">
      <section className="section-hero vision-hero span-2">
        <div>
          <p className="eyebrow">Visão</p>
          <h2>Câmera para entender e monitorar</h2>
          <span>Análise de cena para o robô e monitoramento seguro para o usuário.</span>
        </div>
        <button className="hero-action" onClick={() => void describeScene()} type="button">
          <Eye size={18} />
          Analisar agora
        </button>
      </section>

      <section className="panel vision-main span-2">
        <PanelTitle icon={Eye} title="Visão inteligente" action={visionStatus} />
        <div className="camera-stage">
          {frameUrl ? (
            <img alt="Ultima captura do NoiseBot" src={frameUrl} />
          ) : (
            <div className="camera-empty">
              <Camera size={42} />
              <strong>Sem captura recente</strong>
              <span>Use analisar ou monitoramento para buscar um frame.</span>
            </div>
          )}
        </div>
        <div className="compact-list compact-grid">
          <InfoRow label="Observação" value={observedScene} />
          <InfoRow label="Luminosidade" value={observedLight} />
          <InfoRow label="Movimento" value={observedMotion} />
          <InfoRow label="Captura" value={captureLabel} />
        </div>
        <div className="vision-signal-grid">
          <VisionSignal icon={UserRound} label="Pessoa" value={personLabel} />
          <VisionSignal icon={SunMedium} label="Luz" value={observedLight} />
          <VisionSignal icon={Activity} label="Movimento" value={observedMotion} />
          <VisionSignal icon={ShieldCheck} label="Privacidade" value="local" />
        </div>
        <div className="vision-actions">
          <button onClick={() => void captureFrame()} type="button"><Camera size={17} />Capturar frame</button>
          <button onClick={() => void describeScene()} type="button"><Eye size={17} />Descrever cena</button>
          <button type="button"><ShieldCheck size={17} />Privacidade</button>
        </div>
      </section>

      <section className="panel monitor-panel">
        <PanelTitle icon={Monitor} title="Monitoramento" />
        <div className={`monitor-orb ${monitoring ? "active" : ""}`}>
          {monitoring ? <Pause size={28} /> : <Play size={28} />}
        </div>
        <h3>{monitoring ? "Camera ativa" : "Camera em espera"}</h3>
        <p>Preview controlado pelo server com taxa segura para o firmware.</p>
        <div className="monitor-meta">
          <InfoRow label="FPS alvo" value="1-2" />
          <InfoRow label="Timeout" value="5 min" />
          <InfoRow label="Privacidade" value={monitoring ? "ativa" : "inativa"} />
        </div>
        <button
          className={monitoring ? "danger-action" : "primary-action"}
          onClick={() => setMonitoring(!monitoring)}
          type="button"
        >
          {monitoring ? "Parar monitoramento" : "Iniciar monitoramento"}
        </button>
      </section>
    </div>
  );
}

function BasicsView({
  volume,
  setVolume,
  leds,
  setLeds,
  onSave,
  settings,
  settingsStatus,
  setters,
}: {
  volume: number;
  setVolume: (value: number) => void;
  leds: number;
  setLeds: (value: number) => void;
  onSave: () => void;
  settings: {
    confirmLoudSounds: boolean;
    doNotDisturb: boolean;
    nightMode: boolean;
    reduceBrightnessAtNight: boolean;
    silentMode: boolean;
    subtleLeds: boolean;
  };
  settingsStatus: string;
  setters: {
    setConfirmLoudSounds: (value: boolean) => void;
    setDoNotDisturb: (value: boolean) => void;
    setNightMode: (value: boolean) => void;
    setReduceBrightnessAtNight: (value: boolean) => void;
    setSilentMode: (value: boolean) => void;
    setSubtleLeds: (value: boolean) => void;
  };
}) {
  return (
    <div className="content-grid basics-grid">
      <section className="section-hero basics-hero span-2">
        <div>
          <p className="eyebrow">Ajustes básicos</p>
          <h2>Controles que você mexe todo dia</h2>
          <span>Volume, LEDs e modos rápidos ficam sempre à mão.</span>
        </div>
        <button className="hero-action" onClick={onSave} type="button">
          <SlidersHorizontal size={18} />
          Salvar ajustes
        </button>
      </section>

      <section className="panel basics-preview">
        <PanelTitle icon={Sparkles} title="Ambiente" />
        <div className="ambient-preview">
          <span style={{ opacity: Math.max(0.18, leds / 100) }} />
          <strong>Iluminação</strong>
          <em>LEDs {leds}%</em>
        </div>
      </section>

      <ControlPanel icon={Volume2} label="Volume" value={volume} onChange={setVolume} />
      <ControlPanel icon={Sparkles} label="Brilho dos LEDs" value={leds} onChange={setLeds} />
      <section className="panel span-2">
        <PanelTitle icon={Mic2} title="Modos rápidos" />
        <div className="mode-grid">
          <button
            className={!settings.silentMode && !settings.doNotDisturb && !settings.nightMode ? "active" : ""}
            onClick={() => {
              setters.setSilentMode(false);
              setters.setDoNotDisturb(false);
              setters.setNightMode(false);
            }}
            type="button"
          >
            Normal
          </button>
          <button
            className={settings.silentMode ? "active" : ""}
            onClick={() => setters.setSilentMode(!settings.silentMode)}
            type="button"
          >
            Silencioso
          </button>
          <button
            className={settings.doNotDisturb ? "active" : ""}
            onClick={() => setters.setDoNotDisturb(!settings.doNotDisturb)}
            type="button"
          >
            Não perturbe
          </button>
          <button
            className={settings.nightMode ? "active" : ""}
            onClick={() => setters.setNightMode(!settings.nightMode)}
            type="button"
          >
            Noite
          </button>
        </div>
        <small className="panel-status">{settingsStatus}</small>
      </section>
      <section className="panel">
        <PanelTitle icon={SunMedium} title="Conforto" />
        <ToggleRow
          enabled={settings.reduceBrightnessAtNight}
          label="Reduzir brilho à noite"
          onToggle={() => setters.setReduceBrightnessAtNight(!settings.reduceBrightnessAtNight)}
        />
        <ToggleRow
          enabled={settings.confirmLoudSounds}
          label="Confirmar sons altos"
          onToggle={() => setters.setConfirmLoudSounds(!settings.confirmLoudSounds)}
        />
        <ToggleRow
          enabled={settings.subtleLeds}
          label="LEDs discretos"
          onToggle={() => setters.setSubtleLeds(!settings.subtleLeds)}
        />
      </section>
    </div>
  );
}

function ProfileView() {
  return (
    <div className="content-grid profile-grid">
      <section className="section-hero profile-hero span-2">
        <div>
          <p className="eyebrow">Perfil</p>
          <h2>A identidade do seu assistente</h2>
          <span>Nome, idioma, voz e estilo de resposta sem abrir configuração técnica.</span>
        </div>
        <button className="hero-action" type="button">
          <UserRound size={18} />
          Aplicar perfil
        </button>
      </section>

      <section className="panel profile-card">
        <div className="profile-face">
          <Bot size={42} />
        </div>
        <strong>NoiseBot</strong>
        <span>Português do Brasil</span>
        <em>calmo, curioso e direto</em>
      </section>

      <section className="panel span-2">
        <PanelTitle icon={UserRound} title="Perfil do assistente" />
        <div className="form-grid">
          <label>
            Nome do assistente
            <input defaultValue="NoiseBot" />
          </label>
          <label>
            Linguagem
            <select defaultValue="pt-BR">
              <option value="pt-BR">Português do Brasil</option>
              <option value="en-US">English</option>
            </select>
          </label>
          <label>
            Tom de resposta
            <select defaultValue="natural">
              <option value="natural">Natural</option>
              <option value="curto">Mais curto</option>
              <option value="expressivo">Mais expressivo</option>
            </select>
          </label>
        </div>
      </section>

      <section className="panel">
        <PanelTitle icon={Languages} title="Voz" />
        <InfoRow label="Modelo" value="Piper Faber" />
        <InfoRow label="Idioma" value="pt-BR" />
        <InfoRow label="Status" value="ativo" />
        <button className="secondary-action" type="button">Testar voz</button>
      </section>
    </div>
  );
}

function SettingsView({
  opsToken,
  setOpsToken,
  snapshot,
}: {
  opsToken: string;
  setOpsToken: (value: string) => void;
  snapshot: DashboardSnapshot;
}) {
  const settings = [
    { icon: Wifi, title: "WiFi", detail: "rede local configurada", status: "ativo", group: "Conectividade" },
    { icon: Network, title: "Bridge/Server", detail: "localhost:8765", status: "online", group: "Conectividade" },
    { icon: RefreshCw, title: "OTA", detail: "atualização segura", status: "planejado", group: "Manutenção" },
    { icon: HardDrive, title: "Logs", detail: "diagnóstico e exportação", status: "local", group: "Manutenção" },
    { icon: Gauge, title: "Servos", detail: "motion safety obrigatório", status: "protegido", group: "Hardware" },
    { icon: Clock3, title: "Hora e local", detail: "America/Sao_Paulo", status: "ok", group: "Sistema" },
    { icon: Cpu, title: "Device", detail: "ESP32-S3 N16R8", status: "conectado", group: "Hardware" },
  ];

  return (
    <div className="content-grid settings-grid">
      <section className="section-hero settings-hero">
        <div>
          <p className="eyebrow">Configurações</p>
          <h2>Área avançada, organizada por risco</h2>
          <span>Rede, atualização, logs, servos e device ficam separados dos ajustes diários.</span>
        </div>
      </section>

      <section className="settings-summary">
        <StatPanel icon={Wifi} label="Conectividade" value="2" tone="teal" />
        <StatPanel icon={HardDrive} label="Manutenção" value="2" tone="amber" />
        <StatPanel icon={Cpu} label="Hardware" value="2" tone="coral" />
      </section>

      <section className="panel span-2">
        <PanelTitle icon={ShieldCheck} title="Token local" />
        <p className="panel-copy">
          Necessário para ações que mudam o robô, como comando rápido e testes.
        </p>
        <label>
          Ops token
          <input
            onChange={(event) => setOpsToken(event.target.value)}
            placeholder="cole o token de ~/.noisebot-server/ops_token"
            type="password"
            value={opsToken}
          />
        </label>
      </section>

      <section className="panel">
        <PanelTitle icon={Bot} title="Runtime" />
        <InfoRow label="Modo" value={snapshot.robot.mode} />
        <InfoRow label="LLM" value={snapshot.robot.provider} />
        <InfoRow label="Modelo" value={snapshot.robot.model || "--"} />
        <InfoRow label="STT" value={snapshot.robot.sttStatus} />
        <InfoRow label="TTS" value={snapshot.robot.ttsStatus} />
        <InfoRow label="Erro" value={snapshot.robot.lastError || "--"} />
      </section>

      {settings.map((item) => {
        const Icon = item.icon;
        return (
          <article className="settings-row" key={item.title}>
            <span className="settings-icon">
              <Icon size={20} />
            </span>
            <div>
              <strong>{item.title}</strong>
              <span>{item.group} · {item.detail}</span>
            </div>
            <em>{item.status}</em>
            <ChevronRight size={18} />
          </article>
        );
      })}
    </div>
  );
}

function ControlPanel({
  icon: Icon,
  label,
  value,
  onChange,
}: {
  icon: typeof Volume2;
  label: string;
  value: number;
  onChange: (value: number) => void;
}) {
  return (
    <section className="panel control-panel">
      <PanelTitle icon={Icon} title={label} />
      <strong className="control-value">{value}%</strong>
      <input
        aria-label={label}
        max="100"
        min="0"
        onChange={(event) => onChange(Number(event.target.value))}
        type="range"
        value={value}
      />
    </section>
  );
}

function StatPanel({
  icon: Icon,
  label,
  value,
  tone,
}: {
  icon: typeof Timer;
  label: string;
  value: string;
  tone: "teal" | "amber" | "coral";
}) {
  return (
    <section className={`stat-panel ${tone}`}>
      <Icon size={22} />
      <span>{label}</span>
      <strong>{value}</strong>
    </section>
  );
}

function VisionSignal({
  icon: Icon,
  label,
  value,
}: {
  icon: typeof Camera;
  label: string;
  value: string;
}) {
  return (
    <article className="vision-signal">
      <Icon size={18} />
      <span>{label}</span>
      <strong>{value}</strong>
    </article>
  );
}

function PanelTitle({
  icon: Icon,
  title,
  action,
}: {
  icon: typeof Home;
  title: string;
  action?: string;
}) {
  return (
    <div className="panel-title">
      <span>
        <Icon size={18} />
        {title}
      </span>
      {action ? <button type="button">{action}</button> : null}
    </div>
  );
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="info-row">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function HealthItem({ label, ok }: { label: string; ok: boolean }) {
  return (
    <div className="health-item">
      {ok ? <CheckCircle2 size={18} /> : <Power size={18} />}
      <span>{label}</span>
      <strong>{ok ? "ok" : "offline"}</strong>
    </div>
  );
}

function ToggleRow({
  label,
  enabled,
  onToggle,
}: {
  label: string;
  enabled: boolean;
  onToggle?: () => void;
}) {
  return (
    <div className="toggle-row">
      <span>{label}</span>
      <button className={enabled ? "toggle enabled" : "toggle"} onClick={onToggle} type="button">
        <span />
      </button>
    </div>
  );
}

function StatusDot({ ok }: { ok: boolean }) {
  return <span className={ok ? "status-dot ok" : "status-dot"} />;
}

function ConnectionPill({
  serverOnline,
  firmwareOnline,
}: {
  serverOnline: boolean;
  firmwareOnline: boolean;
}) {
  return (
    <div className="connection-pill">
      <StatusDot ok={serverOnline && firmwareOnline} />
      <span>{serverOnline && firmwareOnline ? "conectado" : "verificar conexao"}</span>
    </div>
  );
}
