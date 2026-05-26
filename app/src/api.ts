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
    mode: "idle" | "monitoring" | "analyzing";
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

export type AppData = {
  routine: {
    items: RoutineItem[];
    summary: RoutineSummary;
  };
  settings: BasicSettings;
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

export async function observeVision(): Promise<VisionObservation> {
  const body = await getJson<{ observation: VisionObservation }>("/api/vision/observe");
  return body.observation;
}

export async function analyzeVision(): Promise<VisionAnalysis> {
  const body = await getJson<{ analysis: VisionAnalysis }>("/api/vision/analyze");
  return body.analysis;
}

export function visionSnapshotUrl(): string {
  const separator = SERVER_URL.includes("?") ? "&" : "?";
  return `${SERVER_URL}/api/vision/snapshot${separator}ts=${Date.now()}`;
}

async function getJson<T>(path: string): Promise<T> {
  const response = await fetch(`${SERVER_URL}${path}`, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`${path} ${response.status}`);
  }
  return response.json() as Promise<T>;
}

async function authedJson<T>(
  path: string,
  token: string,
  options: { method: string; body?: Record<string, unknown> | BasicSettings },
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
