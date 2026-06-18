import { useEffect, useRef, useState } from "react";
import {
  AppData, DashboardSnapshot, DevData, DevicePersona, InteractionResponseMode,
  LlmTurnDebug, RoutineItem,
  createAgendaItem, defaultAppData, defaultDevicePersona,
  deleteAgendaItem, fetchLlmDebug, loadAppData, loadDevData, loadDevicePersona,
  loadSnapshot, resetMetrics, restartServer, saveBasicSettings,
  saveDevicePersona, sendInteraction, setFollowupEnabled,
  startAudioProcessorBridge, startAudioProcessorShadow,
  stopAudioProcessorBridge, stopAudioProcessorShadow, updateAgendaItem,
} from "../api";
import { errorMessage } from "../lib/format";

export type AppMode = "user" | "dev";
export type UserSection = "home" | "profile" | "interaction" | "routine" | "basics";
export type DevSection = "telemetry" | "integrations" | "console" | "llm";
export type PendingCommand = {
  turnId: number;
  text: string;
  startedAt: number;
  attachmentName?: string;
  responseMode?: InteractionResponseMode;
};

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
  routine: { next: "Carregando rotina", timers: 0, alarms: 0, reminders: 0 },
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
    turns: { total: 0, local_intent: 0, llm: 0, fallback: 0, failed: 0, interrupted: 0 },
    tokens: { input: null, output: null },
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
  vision: { available: false, source: "unconfigured" },
  diagnostics: { available: false, source: "unavailable", errors: {} },
};

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

function editableContext(
  mode: AppMode,
  userSection: UserSection,
  devSection: DevSection,
): boolean {
  if (mode === "user")
    return userSection === "profile" || userSection === "routine" || userSection === "basics";
  return devSection === "console";
}

async function safeLoadDevData(): Promise<DevData> {
  try { return await loadDevData(); }
  catch { return defaultDevData; }
}

async function safeLoadDevicePersona(): Promise<DevicePersona> {
  try { return await loadDevicePersona(); }
  catch { return defaultDevicePersona; }
}

export function useAppState() {
  const [mode, setMode] = useState<AppMode>("user");
  const [userSection, setUserSection] = useState<UserSection>("home");
  const [devSection, setDevSection] = useState<DevSection>("telemetry");
  const contextRef = useRef({ mode: "user" as AppMode, userSection: "home" as UserSection, devSection: "telemetry" as DevSection });

  const [snapshot, setSnapshot] = useState<DashboardSnapshot>(initialSnapshot);
  const [appData, setAppData] = useState<AppData>(defaultAppData);
  const [devicePersona, setDevicePersona] = useState<DevicePersona>(defaultDevicePersona);
  const [profileDraft, setProfileDraft] = useState(defaultDevicePersona.user);
  const [devData, setDevData] = useState<DevData>(defaultDevData);
  const [llmDebug, setLlmDebug] = useState<LlmTurnDebug[]>([]);
  const [volume, setVolume] = useState(defaultAppData.settings.volume);
  const [leds, setLeds] = useState(defaultAppData.settings.led_brightness);
  const [followupOverride, setFollowupOverride] = useState<boolean | null>(null);
  const [opsToken, setOpsToken] = useState(() => localStorage.getItem("noisebot_ops_token") ?? "");

  const [commandText, setCommandText] = useState("");
  const [commandStatus, setCommandStatus] = useState("pronto");
  const [pendingCommand, setPendingCommand] = useState<PendingCommand | null>(null);
  const [routineStatus, setRoutineStatus] = useState("pronto");
  const [volumeStatus, setVolumeStatus] = useState("pronto");
  const [ledsStatus, setLedsStatus] = useState("pronto");
  const [profileStatus, setProfileStatus] = useState("pronto");
  const [followupStatus, setFollowupStatus] = useState("pronto");
  const [silenceModeStatus, setSilenceModeStatus] = useState("pronto");
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
      const [nextSnapshot, nextData, nextDevData, nextLlmDebug] = await Promise.all([
        loadSnapshot(), loadAppData(), safeLoadDevData(), fetchLlmDebug(),
      ]);
      const nextPersona = await safeLoadDevicePersona();
      setSnapshot(withRoutine(nextSnapshot, nextData));
      setDevData(nextDevData);
      setLlmDebug(nextLlmDebug);
      setDevicePersona(nextPersona);
      const ctx = contextRef.current;
      applyAppData(nextData, !editableContext(ctx.mode, ctx.userSection, ctx.devSection));
      if (ctx.mode !== "user" || ctx.userSection !== "profile") {
        setProfileDraft(nextPersona.user);
      }
    } finally {
      setRefreshing(false);
    }
  };

  useEffect(() => {
    let cancelled = false;
    const refresh = async () => {
      const [nextSnapshot, nextData, nextDevData, nextPersona, nextLlmDebug] = await Promise.all([
        loadSnapshot(), loadAppData(), safeLoadDevData(), safeLoadDevicePersona(),
        fetchLlmDebug(),
      ]);
      if (!cancelled) {
        setSnapshot(withRoutine(nextSnapshot, nextData));
        setDevData(nextDevData);
        setLlmDebug(nextLlmDebug);
        setDevicePersona(nextPersona);
        const ctx = contextRef.current;
        applyAppData(nextData, !editableContext(ctx.mode, ctx.userSection, ctx.devSection));
        if (ctx.mode !== "user" || ctx.userSection !== "profile") {
          setProfileDraft(nextPersona.user);
        }
      }
    };
    void refresh();
    const interval = window.setInterval(() => void refresh(), 5000);
    return () => { cancelled = true; window.clearInterval(interval); };
  }, []);

  useEffect(() => {
    if (!pendingCommand) return;
    const completed = devData.metrics.recent_voice_sessions.find(
      (session) => session.turn_id === pendingCommand.turnId,
    );
    if (!completed) return;

    setPendingCommand(null);
    if (completed.reply) {
      setCommandStatus("pronto");
      return;
    }
    const reason = completed.error_reason
      || completed.discard_reason
      || completed.outcome
      || "sem resposta";
    setCommandStatus(`O turno terminou sem resposta: ${reason}. Tente novamente.`);
  }, [devData.metrics.recent_voice_sessions, pendingCommand]);

  useEffect(() => {
    if (!pendingCommand) return;
    const elapsed = Date.now() - pendingCommand.startedAt;
    const remaining = Math.max(0, 45_000 - elapsed);
    const timer = window.setTimeout(() => {
      setCommandStatus(
        "A resposta demorou demais e o turno não foi concluído. Verifique a conexão e tente novamente.",
      );
    }, remaining);
    return () => window.clearTimeout(timer);
  }, [pendingCommand]);

  const requireToken = (
    target: "command" | "routine" | "volume" | "leds" | "profile" | "followup" | "silence" | "dev",
  ): string => {
    const token = opsToken.trim();
    if (!token) {
      setMode("dev");
      setDevSection("console");
      const msg = "token local obrigatório para executar esta ação";
      if (target === "command") setCommandStatus(msg);
      else if (target === "routine") setRoutineStatus(msg);
      else if (target === "volume") setVolumeStatus(msg);
      else if (target === "leds") setLedsStatus(msg);
      else if (target === "profile") setProfileStatus(msg);
      else if (target === "followup") setFollowupStatus(msg);
      else if (target === "silence") setSilenceModeStatus(msg);
      else setDevStatus(msg);
      return "";
    }
    return token;
  };

  const setAppMode = (nextMode: AppMode) => {
    setMode(nextMode);
    if (nextMode === "user") setUserSection("home");
    else setDevSection("telemetry");
  };

  const saveOpsToken = (value: string) => {
    setOpsToken(value);
    if (value.trim()) localStorage.setItem("noisebot_ops_token", value.trim());
    else localStorage.removeItem("noisebot_ops_token");
  };

  // --- Routine ---

  const updateRoutineState = (routine: AppData["routine"]) => {
    const nextData = { ...appData, routine };
    setAppData(nextData);
    setSnapshot(withRoutine(snapshot, nextData));
  };

  const createTimer = async (title: string, durationMin: number) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando timer");
    try {
      updateRoutineState(await createAgendaItem("timer", { title, duration_min: durationMin }, token));
      setRoutineStatus("timer criado");
    } catch (e) { setRoutineStatus(errorMessage(e)); }
  };

  const createAlarm = async (title: string, time: string, repeat: string) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando alarme");
    try {
      updateRoutineState(await createAgendaItem("alarm", { title, time, repeat }, token));
      setRoutineStatus("alarme criado");
    } catch (e) { setRoutineStatus(errorMessage(e)); }
  };

  const createReminder = async (title: string, durationMin: number) => {
    const token = requireToken("routine");
    if (!token) return;
    setRoutineStatus("criando lembrete");
    try {
      updateRoutineState(await createAgendaItem("reminder", { title, duration_min: durationMin }, token));
      setRoutineStatus("lembrete criado");
    } catch (e) { setRoutineStatus(errorMessage(e)); }
  };

  const toggleRoutine = async (item: RoutineItem) => {
    const token = requireToken("routine");
    if (!token) return;
    try {
      updateRoutineState(await updateAgendaItem(item.id, { enabled: !item.enabled }, token));
      setRoutineStatus("rotina atualizada");
    } catch (e) { setRoutineStatus(errorMessage(e)); }
  };

  const removeRoutine = async (item: RoutineItem) => {
    const token = requireToken("routine");
    if (!token) return;
    try {
      updateRoutineState(await deleteAgendaItem(item.id, token));
      setRoutineStatus("item removido");
    } catch (e) { setRoutineStatus(errorMessage(e)); }
  };

  // --- Settings ---

  const saveVolume = async () => {
    const token = requireToken("volume");
    if (!token) return;
    setVolumeStatus("salvando volume");
    try {
      const result = await saveBasicSettings({ volume }, token);
      const saved = result.settings;
      setAppData((prev: AppData) => ({ ...prev, settings: saved }));
      setVolume(saved.volume);
      setVolumeStatus(`volume alterado para ${saved.volume}%`);
    } catch (e) { setVolumeStatus(errorMessage(e)); }
  };

  const saveLeds = async () => {
    const token = requireToken("leds");
    if (!token) return;
    setLedsStatus("salvando brilho dos LEDs");
    try {
      const result = await saveBasicSettings({ led_brightness: leds }, token);
      const saved = result.settings;
      setAppData((prev: AppData) => ({ ...prev, settings: saved }));
      setLeds(saved.led_brightness);
      setLedsStatus(`brilho dos LEDs alterado para ${saved.led_brightness}%`);
    } catch (e) { setLedsStatus(errorMessage(e)); }
  };

  const toggleFollowup = async (enabled: boolean) => {
    const token = requireToken("followup");
    if (!token) return;
    setFollowupOverride(enabled);
    setFollowupStatus(enabled ? "ativando escuta contínua" : "desativando escuta contínua");
    try {
      await setFollowupEnabled(enabled, token);
      setDevData(await safeLoadDevData());
      setFollowupOverride(null);
      setFollowupStatus(
        enabled
          ? "escuta contínua ativada após perguntas"
          : "escuta contínua desativada — só ouve com a palavra de ativação",
      );
    } catch (e) {
      setFollowupOverride(null);
      setFollowupStatus(errorMessage(e));
    }
  };

  const toggleSilenceMode = async (enabled: boolean) => {
    const token = requireToken("silence");
    if (!token) return;
    setSilenceModeStatus(enabled ? "ativando modo silêncio" : "desativando modo silêncio");
    try {
      const result = await saveBasicSettings({ silent_mode: enabled }, token);
      const saved = result.settings;
      setAppData((prev: AppData) => ({ ...prev, settings: saved }));
      setDevData(await safeLoadDevData());
      const applied = result.applied.silent_mode;
      if (applied === "firmware") {
        setSilenceModeStatus(enabled ? "modo silêncio ativo no robô" : "modo silêncio desativado no robô");
      } else {
        setSilenceModeStatus(
          enabled
            ? "salvo no dashboard, mas o robô não confirmou"
            : "desativado no dashboard, mas o robô não confirmou",
        );
      }
    } catch (e) {
      setSilenceModeStatus(errorMessage(e));
    }
  };

  // --- Profile ---

  const saveUserProfile = async () => {
    const token = requireToken("profile");
    if (!token) return;
    setProfileStatus("salvando perfil");
    try {
      await saveDevicePersona(profileDraft, token);
      const nextPersona = await safeLoadDevicePersona();
      setDevicePersona(nextPersona);
      setProfileDraft(nextPersona.user);
      setProfileStatus("perfil salvo no robô");
    } catch (e) { setProfileStatus(errorMessage(e)); }
  };

  // --- Interaction ---

  const submitCommand = async (
    image?: File | null,
    responseMode: InteractionResponseMode = "dashboard",
  ) => {
    const text = commandText.trim();
    const token = requireToken("command");
    if ((!text && !image) || !token) return;
    const prompt = text || "Analise esta imagem.";
    setCommandStatus(image ? "analisando imagem" : "enviando");
    try {
      const result = await sendInteraction(prompt, token, responseMode, image);
      setCommandText("");
      setPendingCommand({
        turnId: result.turn_id,
        text: prompt,
        startedAt: Date.now(),
        attachmentName: result.attachment?.name,
        responseMode,
      });
      setCommandStatus("preparando");
      window.setTimeout(() => void refreshAll(), 800);
    } catch (e) { setCommandStatus(errorMessage(e)); }
  };

  // --- Dev ---

  const handleResetMetrics = async () => {
    const token = requireToken("dev");
    if (!token) return;
    setDevStatus("zerando métricas");
    try {
      await resetMetrics(token);
      setDevData(await safeLoadDevData());
      setDevStatus("métricas zeradas");
    } catch (e) { setDevStatus(errorMessage(e)); }
  };

  const handleRestartServer = async () => {
    const token = requireToken("dev");
    if (!token) return;
    setDevStatus("reinício solicitado");
    try {
      await restartServer(token);
      setDevStatus("server reiniciando");
    } catch (e) { setDevStatus(errorMessage(e)); }
  };

  const handleAudioProcessorAction = async (
    action: "shadow_start" | "shadow_stop" | "bridge_start" | "bridge_stop",
  ) => {
    const token = requireToken("dev");
    if (!token) return;
    const labels = {
      shadow_start: "ligando shadow AFE",
      shadow_stop: "desligando shadow AFE",
      bridge_start: "ligando AFE no bridge",
      bridge_stop: "desligando AFE no bridge",
    };
    setDevStatus(labels[action]);
    try {
      if (action === "shadow_start") await startAudioProcessorShadow(token);
      if (action === "shadow_stop") await stopAudioProcessorShadow(token);
      if (action === "bridge_start") await startAudioProcessorBridge(token);
      if (action === "bridge_stop") await stopAudioProcessorBridge(token);
      setDevData(await safeLoadDevData());
      setDevStatus("AFE atualizado");
    } catch (e) { setDevStatus(errorMessage(e)); }
  };

  return {
    // nav state
    mode, userSection, devSection,
    setAppMode, setUserSection, setDevSection,
    // data
    snapshot, appData, devicePersona, profileDraft, devData, llmDebug,
    // controls
    volume, leds, opsToken, now,
    // setters
    setProfileDraft, setCommandText, setVolume, setLeds,
    saveOpsToken,
    // status
    commandText, commandStatus, pendingCommand,
    routineStatus,
    volumeStatus, ledsStatus,
    profileStatus, followupStatus, silenceModeStatus, devStatus,
    refreshing,
    // computed
    followupEnabled: followupOverride ?? devData.config.conversation?.followup_enabled ?? false,
    followupWindowMs: devData.config.conversation?.followup_window_ms,
    // actions
    refreshAll,
    createTimer, createAlarm, createReminder, toggleRoutine, removeRoutine,
    saveVolume, saveLeds, toggleFollowup, toggleSilenceMode,
    saveUserProfile,
    submitCommand,
    handleResetMetrics, handleRestartServer, handleAudioProcessorAction,
  };
}
