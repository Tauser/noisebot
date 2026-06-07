export type RobotState = "online" | "offline" | "listening" | "thinking" | "speaking" | "resting";

export type DashboardSnapshot = {
  robot: {
    name: string;
    state: RobotState;
    mood: string;
    batteryLabel: string;
    serverOnline: boolean;
    firmwareOnline: boolean;
    sttStatus: string;
    llmStatus: string;
    ttsStatus: string;
    mode: string;
    provider: string;
    model: string;
    lastError: string;
    lastTranscript: string;
    lastReply: string;
    lastRoute: string;
    lastTurnId: number;
    lastUpdatedAt: string;
  };
  routine: {
    next: string;
    timers: number;
    alarms: number;
    reminders: number;
  };
  vision: {
    mode: "idle" | "detecting" | "analyzing";
    lastObservation: string;
    light: string;
    motion: string;
    frameUrl: string | null;
  };
};

export type VisionObservation = {
  valid: boolean;
  scene: string;
  timestamp_ms: number;
  width: number;
  height: number;
  jpeg_bytes: number;
  capture_ms: number;
  luma_avg: number;
  luma_min: number;
  luma_max: number;
  contrast: number;
  motion_score: number;
};

export type VisionAnalysis = {
  observation: VisionObservation;
  detector: string;
  detector_available: boolean;
  face_detected: boolean;
  face_count: number;
  face_center_norm_x: number | null;
  face_center_norm_y: number | null;
  primary_face: { x: number; y: number; width: number; height: number } | null;
  error: string | null;
};

export type VoiceSessionSummary = {
  turn_id?: number;
  outcome?: string;
  state?: string;
  discard_reason?: string | null;
  voice_end_reason?: string | null;
  total_samples?: number;
  duration_ms?: number;
  chunk_count?: number;
  transcript_quality?: string | null;
  transcript?: string | null;
  no_speech_prob?: number | null;
  avg_logprob?: number | null;
  compression_ratio?: number | null;
  intent_name?: string | null;
  reply?: string | null;
  reply_chars?: number;
  voice_end_to_stt_start_ms?: number | null;
  stt_ms?: number | null;
  end_of_turn_ms?: number | null;
  tts_first_audio_ms?: number | null;
  first_audio_out_ms?: number | null;
  first_audio_after_voice_end_ms?: number | null;
  speech_total_ms?: number | null;
  error_stage?: string | null;
  error_reason?: string | null;
};

export type VoiceAlert = {
  level: "ok" | "warn" | "error";
  title: string;
  detail: string;
};

export type AiMetrics = {
  latency_ms: Record<string, { p50: number | null; p95: number | null; count: number }>;
  turns: Record<string, number>;
  last_voice_session: VoiceSessionSummary;
  recent_voice_sessions: VoiceSessionSummary[];
  voice_alert: VoiceAlert | null;
  tokens: {
    input: number | null;
    output: number | null;
  };
  estimated_cost: number | null;
};

export type AiErrorEntry = {
  ts: number;
  kind: string;
  provider: string;
  model: string;
  message: string;
  turn_id: number;
};

export type ServerLogEntry = {
  ts: number;
  level: string;
  logger: string;
  message: string;
};

export type AiConfig = {
  transport?: {
    type?: string;
    host?: string;
    port?: number;
    uart?: string;
  };
  llm?: {
    provider?: string;
    model?: string;
    temperature?: number;
  };
  ops?: {
    host?: string;
    port?: number;
    token_configured?: boolean;
  };
  log_level?: string;
  pipeline_mode?: string;
};

export type DeviceStatus = {
  server_online: boolean;
  firmware_online: boolean;
  transport_host: string;
  transport_port: number;
  ops_port: number;
  dry_run: boolean;
  features: string[];
  supervisor: string;
  diagnostics_available?: boolean;
};

export type VisionStatus = {
  available: boolean;
  source: string;
};

export type FirmwareDiagnostics = {
  available: boolean;
  source?: string;
  base_url?: string;
  latency_ms?: number | null;
  errors?: Record<string, string>;
  diag?: Record<string, unknown> | null;
  health?: Record<string, unknown> | null;
  version?: Record<string, unknown> | null;
  wifi?: Record<string, unknown> | null;
  audio?: Record<string, unknown> | null;
  audio_processor?: Record<string, unknown> | null;
  camera?: Record<string, unknown> | null;
  vision?: Record<string, unknown> | null;
  touch?: Record<string, unknown> | null;
  agenda?: Record<string, unknown> | unknown[] | null;
  config?: Record<string, unknown> | null;
  ltm?: Record<string, unknown> | null;
};

export type AudioSampleFile = {
  name: string;
  size: number;
  path?: string;
};

export type DevData = {
  metrics: AiMetrics;
  errors: AiErrorEntry[];
  logs: ServerLogEntry[];
  config: AiConfig;
  device: DeviceStatus;
  vision: VisionStatus;
  diagnostics: FirmwareDiagnostics;
};

export type RoutineKind = "timer" | "alarm" | "reminder";

export type RoutineItem = {
  id: string;
  kind: RoutineKind;
  title: string;
  detail: string;
  enabled: boolean;
  status: string;
  duration_min?: number;
  time?: string;
  repeat?: string;
  source?: "firmware" | "server";
  weekdays_mask?: number;
};

export type RoutineSummary = {
  next: string;
  timers: number;
  alarms: number;
  reminders: number;
};

export type BasicSettings = {
  volume: number;
  display_brightness: number;
  led_brightness: number;
  silent_mode: boolean;
  do_not_disturb: boolean;
  night_mode: boolean;
  reduce_brightness_at_night: boolean;
  confirm_loud_sounds: boolean;
  subtle_leds: boolean;
};

export type ProfileSettings = {
  assistant_name: string;
  language: "pt-BR" | "en-US";
  response_tone: "natural" | "curto" | "expressivo";
  voice: string;
};

export type AdvancedSettings = {
  wifi_ssid: string;
  wifi_enabled: boolean;
  bridge_host: string;
  bridge_port: number;
  ota_channel: "stable" | "beta" | "manual";
  logs_enabled: boolean;
  log_level: "DEBUG" | "INFO" | "WARNING" | "ERROR";
  servos_enabled: boolean;
  timezone: string;
  location: string;
  device_name: string;
};

export type AppData = {
  routine: {
    items: RoutineItem[];
    summary: RoutineSummary;
  };
  settings: BasicSettings;
  profile: ProfileSettings;
  advanced: AdvancedSettings;
};

type HealthResponse = {
  status: string;
  uptime_s: number;
  updated_at: string;
};

type AiStatusResponse = {
  connected: boolean;
  mode: string;
  provider: string;
  model: string;
  stt_status: string;
  llm_status: string;
  tts_status: string;
  last_error: { kind?: string } | null;
  last_turn_id: number;
  last_transcript: string;
  last_reply: string;
  last_route: string;
  updated_at: string;
};

const SERVER_URL = import.meta.env.VITE_NOISEBOT_SERVER_URL
  ?? (window.location.port === "5173" ? "/server-api" : "");

export const defaultAppData: AppData = {
  routine: {
    items: [],
    summary: {
      next: "Nenhum compromisso ativo",
      timers: 0,
      alarms: 0,
      reminders: 0,
    },
  },
  settings: {
    volume: 62,
    display_brightness: 74,
    led_brightness: 48,
    silent_mode: false,
    do_not_disturb: false,
    night_mode: false,
    reduce_brightness_at_night: true,
    confirm_loud_sounds: true,
    subtle_leds: false,
  },
  profile: {
    assistant_name: "NoiseBot",
    language: "pt-BR",
    response_tone: "natural",
    voice: "Piper Faber",
  },
  advanced: {
    wifi_ssid: "",
    wifi_enabled: true,
    bridge_host: "127.0.0.1",
    bridge_port: 8765,
    ota_channel: "stable",
    logs_enabled: true,
    log_level: "INFO",
    servos_enabled: false,
    timezone: "America/Sao_Paulo",
    location: "Brasil",
    device_name: "NoiseBot",
  },
};

export async function loadSnapshot(): Promise<DashboardSnapshot> {
  try {
    const [health, status] = await Promise.all([
      getJson<HealthResponse>("/health"),
      getJson<AiStatusResponse>("/ai/status"),
    ]);
    return snapshotFromServer(health, status);
  } catch {
    return fallbackSnapshot(false);
  }
}

export async function loadAppData(): Promise<AppData> {
  try {
    const data = await getJson<AppData>("/api/app/state");
    return normalizeAppData(data);
  } catch {
    return defaultAppData;
  }
}

export async function sendDebugTranscript(text: string, token: string): Promise<void> {
  const response = await fetch(`${SERVER_URL}/debug/transcript`, {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ text }),
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({}));
    throw new Error(body.error ?? `server ${response.status}`);
  }
}

export async function createAgendaItem(
  kind: RoutineKind,
  payload: Record<string, unknown>,
  token: string,
): Promise<AppData["routine"]> {
  const routes: Record<RoutineKind, string> = {
    timer: "/api/agenda/timers",
    alarm: "/api/agenda/alarms",
    reminder: "/api/agenda/reminders",
  };
  const body = await authedJson<{ agenda: AppData["routine"] }>(routes[kind], token, {
    method: "POST",
    body: payload,
  });
  return body.agenda;
}

export async function updateAgendaItem(
  id: string,
  payload: Record<string, unknown>,
  token: string,
): Promise<AppData["routine"]> {
  const body = await authedJson<{ agenda: AppData["routine"] }>(
    `/api/agenda/items/${encodeURIComponent(id)}`,
    token,
    {
      method: "PATCH",
      body: payload,
    },
  );
  return body.agenda;
}

export async function deleteAgendaItem(id: string, token: string): Promise<AppData["routine"]> {
  const body = await authedJson<{ agenda: AppData["routine"] }>(
    `/api/agenda/items/${encodeURIComponent(id)}`,
    token,
    { method: "DELETE" },
  );
  return body.agenda;
}

export async function saveBasicSettings(
  settings: BasicSettings,
  token: string,
): Promise<BasicSettings> {
  const body = await authedJson<{ settings: BasicSettings }>("/api/settings/basic", token, {
    method: "PUT",
    body: settings,
  });
  return body.settings;
}

export async function saveProfile(
  profile: ProfileSettings,
  token: string,
): Promise<ProfileSettings> {
  const body = await authedJson<{ profile: ProfileSettings }>("/api/profile", token, {
    method: "PUT",
    body: profile,
  });
  return body.profile;
}

export async function testProfileVoice(token: string): Promise<{ spoken: boolean; applied: string }> {
  return authedJson<{ spoken: boolean; applied: string }>("/api/profile/test-voice", token, {
    method: "POST",
  });
}

export async function saveAdvancedSettings(
  advanced: AdvancedSettings,
  token: string,
): Promise<AdvancedSettings> {
  const body = await authedJson<{ advanced: AdvancedSettings }>("/api/settings/advanced", token, {
    method: "PUT",
    body: advanced,
  });
  return body.advanced;
}

export async function observeVision(): Promise<VisionObservation> {
  const body = await getJson<{ observation: VisionObservation }>("/api/vision/observe");
  return body.observation;
}

export async function analyzeVision(): Promise<VisionAnalysis> {
  const body = await getJson<{ analysis: VisionAnalysis }>("/api/vision/analyze");
  return body.analysis;
}

export async function loadDevData(): Promise<DevData> {
  const [metrics, errors, logs, config, device, vision, diagnostics] = await Promise.all([
    getJson<AiMetrics>("/ai/metrics"),
    getJson<{ errors: AiErrorEntry[] }>("/ai/errors?limit=20"),
    getJsonOr<{ logs: ServerLogEntry[] }>("/api/logs?limit=80", { logs: [] }),
    getJson<AiConfig>("/ai/config"),
    getJson<DeviceStatus>("/api/device/status"),
    getJson<VisionStatus>("/api/vision/status"),
    getJsonOr<FirmwareDiagnostics>("/api/device/diagnostics", {
      available: false,
      source: "unavailable",
      errors: { server: "endpoint de diagnostico indisponivel" },
    }),
  ]);
  return {
    metrics,
    errors: errors.errors,
    logs: logs.logs,
    config,
    device,
    vision,
    diagnostics,
  };
}

export async function resetMetrics(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/ai/metrics/reset", token, { method: "POST" });
}

export async function restartServer(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/ai/restart", token, { method: "POST" });
}

export function visionSnapshotUrl(): string {
  const separator = SERVER_URL.includes("?") ? "&" : "?";
  return `${SERVER_URL}/api/vision/snapshot${separator}ts=${Date.now()}`;
}

export async function loadAudioSampleFiles(): Promise<AudioSampleFile[]> {
  const body = await getJson<{ files?: AudioSampleFile[] }>("/api/device/audio/files");
  return Array.isArray(body.files) ? body.files : [];
}

export function audioSampleDownloadUrl(name: string): string {
  return `${SERVER_URL}/api/device/audio/files/${encodeURIComponent(name)}`;
}

export async function startAudioProcessorShadow(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/api/device/audio/processor/shadow/start", token, { method: "POST" });
}

export async function stopAudioProcessorShadow(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/api/device/audio/processor/shadow/stop", token, { method: "POST" });
}

export async function startAudioProcessorBridge(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/api/device/audio/processor/bridge/start", token, { method: "POST" });
}

export async function stopAudioProcessorBridge(token: string): Promise<void> {
  await authedJson<{ ok: boolean }>("/api/device/audio/processor/bridge/stop", token, { method: "POST" });
}

async function getJson<T>(path: string): Promise<T> {
  const response = await fetch(`${SERVER_URL}${path}`, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`${path} ${response.status}`);
  }
  return response.json() as Promise<T>;
}

async function getJsonOr<T>(path: string, fallback: T): Promise<T> {
  try {
    return await getJson<T>(path);
  } catch {
    return fallback;
  }
}

async function authedJson<T>(
  path: string,
  token: string,
  options: { method: string; body?: Record<string, unknown> | BasicSettings | ProfileSettings | AdvancedSettings },
): Promise<T> {
  const response = await fetch(`${SERVER_URL}${path}`, {
    method: options.method,
    headers: {
      "Authorization": `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({}));
    throw new Error(body.error ?? `server ${response.status}`);
  }
  return response.json() as Promise<T>;
}

function snapshotFromServer(health: HealthResponse, status: AiStatusResponse): DashboardSnapshot {
  const firmwareOnline = Boolean(status.connected);
  const serverOnline = health.status === "ok";
  return {
    ...fallbackSnapshot(serverOnline),
    robot: {
      name: "NoiseBot",
      state: firmwareOnline ? "online" : "offline",
      mood: firmwareOnline ? "conectado e pronto" : "aguardando firmware",
      batteryLabel: `server ${Math.round(health.uptime_s)}s`,
      serverOnline,
      firmwareOnline,
      sttStatus: status.stt_status,
      llmStatus: status.llm_status,
      ttsStatus: status.tts_status,
      mode: status.mode,
      provider: status.provider,
      model: status.model,
      lastError: status.last_error?.kind ?? "",
      lastTranscript: status.last_transcript,
      lastReply: status.last_reply,
      lastRoute: status.last_route,
      lastTurnId: status.last_turn_id,
      lastUpdatedAt: status.updated_at,
    },
  };
}

function normalizeAppData(data: AppData): AppData {
  return {
    routine: {
      items: Array.isArray(data.routine?.items) ? data.routine.items : [],
      summary: data.routine?.summary ?? defaultAppData.routine.summary,
    },
    settings: {
      ...defaultAppData.settings,
      ...(data.settings ?? {}),
    },
    profile: {
      ...defaultAppData.profile,
      ...(data.profile ?? {}),
    },
    advanced: {
      ...defaultAppData.advanced,
      ...(data.advanced ?? {}),
    },
  };
}

function fallbackSnapshot(serverOnline: boolean): DashboardSnapshot {
  return {
    robot: {
      name: "NoiseBot",
      state: serverOnline ? "online" : "offline",
      mood: serverOnline ? "calmo e atento" : "aguardando server",
      batteryLabel: "energia externa",
      serverOnline,
      firmwareOnline: serverOnline,
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
      next: "Nenhum compromisso ativo",
      timers: 0,
      alarms: 0,
      reminders: 0,
    },
    vision: {
      mode: "idle",
      lastObservation: "Visao pronta para capturar",
      light: "normal",
      motion: "sem movimento recente",
      frameUrl: null,
    },
  };
}
