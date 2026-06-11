import { useState, useEffect, useRef, useCallback } from "react";
import {
  Activity, Camera, Clock3, Cpu, Database, HardDrive, Mic, Terminal, Thermometer, Wifi,
} from "lucide-react";
import type { AudioSampleFile, DevData, DashboardSnapshot, VisionPipelineStatus } from "../api";
import { loadAudioSampleFiles, loadVisionPipelineStatus, visionSnapshotUrl } from "../api";
import { cardClass } from "../lib/classes";
import {
  asRecord, boolValue, bytesValue, formatLatency, formatSeconds,
  numberValue, readNumber, textValue,
} from "../lib/format";
import {
  summarizeVoiceSession, voiceLatencyBottleneck, voiceStageDetail, voiceStageState,
} from "../lib/voice";
import { DiagnosticCard } from "../components/DiagnosticCard";
import { Metric } from "../components/Metric";
import { TurnBubble } from "../components/TurnBubble";
import { InfoRow } from "../components/InfoRow";
import { VoiceStage } from "../components/VoiceStage";
import { VoiceAlertBanner } from "../components/VoiceAlertBanner";
import { VoiceSessionHistory } from "../components/VoiceSessionHistory";
import { AudioSamplesPanel } from "../components/AudioSamplesPanel";

export function DevTelemetryView({
  devData,
  snapshot,
}: {
  devData: DevData;
  snapshot: DashboardSnapshot;
}) {
  const totalTurns = devData.metrics.turns.total ?? 0;
  const sttLatency = formatLatency(devData.metrics.latency_ms.stt);
  const llmLatency = formatLatency(devData.metrics.latency_ms.llm_total);
  const ttsLatency = formatLatency(devData.metrics.latency_ms.tts_first_audio);
  const [audioFiles, setAudioFiles] = useState<AudioSampleFile[]>([]);
  const [audioFilesStatus, setAudioFilesStatus] = useState("Não carregado");
  const [audioFilesLoading, setAudioFilesLoading] = useState(false);

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

  const [visionStatus, setVisionStatus] = useState<VisionPipelineStatus | null>(null);
  const [visionFrameOk, setVisionFrameOk] = useState(false);
  const imgRef = useRef<HTMLImageElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const lastFaceBoxRef = useRef<VisionPipelineStatus["last_face_box"]>(null);

  const drawFaceBox = useCallback((fb: VisionPipelineStatus["last_face_box"]) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.clearRect(0, 0, 240, 240);
    if (!fb || !fb.w) return;
    ctx.strokeStyle = "#3b82f6";
    ctx.lineWidth = 2;
    ctx.strokeRect(fb.x, fb.y, fb.w, fb.h);
  }, []);

  useEffect(() => {
    let cancelled = false;
    const tick = async () => {
      try {
        const s = await loadVisionPipelineStatus();
        if (!cancelled) {
          setVisionStatus(s);
          // preserve last known box — only clear when pipeline resets to IDLE/DISABLED
          if (s.last_face_box) {
            lastFaceBoxRef.current = s.last_face_box;
          } else if (s.state === "IDLE" || s.state === "DISABLED") {
            lastFaceBoxRef.current = null;
          }
          drawFaceBox(lastFaceBoxRef.current);
          // update snapshot src imperatively — avoids key remount and black flash
          if (s.state !== "DISABLED" && imgRef.current) {
            imgRef.current.src = `${visionSnapshotUrl()}?t=${Date.now()}`;
          }
        }
      } catch { /* server offline */ }
    };
    void tick();
    const id = window.setInterval(() => void tick(), 1000);
    return () => { cancelled = true; window.clearInterval(id); };
  }, [drawFaceBox]);

  const refreshAudioFiles = async () => {
    setAudioFilesLoading(true);
    setAudioFilesStatus("Carregando...");
    try {
      const files = await loadAudioSampleFiles();
      setAudioFiles(files);
      setAudioFilesStatus(
        files.length ? `${files.length} arquivo(s)` : "Nenhum WAV encontrado",
      );
    } catch (error) {
      setAudioFilesStatus(error instanceof Error ? error.message : "Falha ao listar");
    } finally {
      setAudioFilesLoading(false);
    }
  };

  return (
    <div className="grid gap-4 xl:grid-cols-2">
      {/* Ciclo de voz */}
      <section className={`${cardClass} xl:col-span-2`}>
        <div className="mb-4 flex flex-wrap items-start justify-between gap-3">
          <div>
            <h2 className="text-lg font-semibold">Ciclo de voz</h2>
            <p className="text-sm text-slate-400">
              O que aconteceu quando o robô ouviu, pensou e respondeu.
            </p>
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
          <TurnBubble
            label="Transcrição"
            text={voice.transcript || snapshot.robot.lastTranscript || "Sem transcrição recente."}
          />
          <TurnBubble
            label="Resposta"
            text={voice.reply || snapshot.robot.lastReply || "Sem resposta recente."}
          />
          <article className="rounded-xl bg-black/[0.18] p-3">
            <span className="text-xs font-bold uppercase text-slate-400">Gargalo provável</span>
            <strong className="mt-1 block text-sm text-white">{latencyBottleneck.label}</strong>
            <p className="mt-1 text-sm text-slate-300">{latencyBottleneck.detail}</p>
          </article>
        </div>
      </section>

      {/* Hardware e build */}
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

      {/* Memória */}
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

      {/* Armazenamento */}
      <DiagnosticCard icon={HardDrive} title="Armazenamento">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="SD montado" value={boolValue(storage.sd_mounted)} />
          <Metric label="SD livre" value={bytesValue(storage.sd_free_bytes)} />
          <Metric label="Config" value={firmware.config ? "exposta" : "não exposta"} />
          <Metric label="LTM" value={firmware.ltm ? "exposta" : "não exposta"} />
        </div>
        <AudioSamplesPanel
          files={audioFiles}
          loading={audioFilesLoading}
          onRefresh={() => void refreshAudioFiles()}
          status={audioFilesStatus}
        />
      </DiagnosticCard>

      {/* Rede e bridge */}
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

      {/* Câmera */}
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
        </div>
      </DiagnosticCard>

      {/* Visão — pipeline do server */}
      <DiagnosticCard icon={Camera} title="Visão (server)">
        <div className="flex gap-4 flex-wrap items-start">
          {/* snapshot + face box */}
          <div className="relative shrink-0 rounded overflow-hidden bg-black" style={{ width: 240, height: 240 }}>
            {/* img persiste — src atualizado via ref para evitar flash preto */}
            <img
              ref={imgRef}
              src=""
              alt="snapshot"
              width={240}
              height={240}
              className="block object-contain w-full h-full"
              style={{ display: visionFrameOk && visionStatus?.state !== "DISABLED" ? "block" : "none" }}
              onLoad={() => setVisionFrameOk(true)}
              onError={() => setVisionFrameOk(false)}
            />
            {(!visionFrameOk || visionStatus?.state === "DISABLED") && (
              <div className="absolute inset-0 flex flex-col items-center justify-center gap-1 text-slate-500 text-xs text-center px-3">
                <Camera className="w-8 h-8 mb-1 opacity-40" />
                <span>
                  {!visionStatus
                    ? "carregando..."
                    : visionStatus.state === "DISABLED"
                    ? "pipeline desabilitado"
                    : !visionStatus.detector_available
                    ? "detector indisponível"
                    : "sem sinal"}
                </span>
              </div>
            )}
            <canvas
              ref={canvasRef}
              width={240}
              height={240}
              className="absolute inset-0 pointer-events-none"
            />
          </div>
          {/* métricas */}
          <div className="grid gap-3 md:grid-cols-2 flex-1 min-w-40">
            <Metric
              label="Pipeline"
              value={
                <span className={
                  visionStatus?.state === "TRACK" ? "text-green-400"
                  : visionStatus?.state === "ACQUIRE" ? "text-yellow-400"
                  : visionStatus?.state === "LOST" ? "text-red-400"
                  : "text-slate-400"
                }>
                  {visionStatus?.state ?? "—"}
                </span>
              }
            />
            <Metric label="Detector" value={boolValue(visionStatus?.detector_available)} />
            <Metric label="Firmware" value={boolValue(visionStatus?.adapter_connected)} />
            <Metric label="Detecções" value={numberValue(visionStatus?.detections, "")} />
            <Metric label="Gaze env." value={numberValue(visionStatus?.gaze_sends, "")} />
            <Metric label="Erros cap." value={numberValue(visionStatus?.capture_errors, "")} />
          </div>
        </div>
      </DiagnosticCard>

      {/* Áudio e wake */}
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

      {/* Última sessão de voz */}
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

      {/* Histórico de voz */}
      <DiagnosticCard icon={Clock3} title="Histórico de voz">
        <VoiceSessionHistory sessions={recentVoice} />
      </DiagnosticCard>

      {/* Touch, uso e sensores */}
      <DiagnosticCard icon={Activity} title="Touch, uso e sensores">
        <div className="grid gap-3 md:grid-cols-2">
          <Metric label="Touch pressed" value={boolValue(touch.pressed)} />
          <Metric label="Touch state" value={textValue(touch.state)} />
          <Metric label="Touch raw" value={numberValue(touch.raw, "")} />
          <Metric label="Touch filtered" value={numberValue(touch.filtered, "")} />
          <Metric label="Touch baseline" value={numberValue(touch.baseline, "")} />
          <Metric label="Touch threshold on" value={numberValue(touch.threshold_on, "")} />
          <Metric label="Touch threshold off" value={numberValue(touch.threshold_off, "")} />
          <Metric label="Último touch" value={textValue(touch.last_event)} />
          <Metric label="Sessões" value={numberValue(ltm.sessions, "")} />
          <Metric label="Horas vivo" value={numberValue(ltm.hours_alive, " h")} />
          <Metric label="Toques" value={numberValue(ltm.touch_count ?? diag.touch_count, "")} />
          <Metric label="Temperatura" value={<span className="inline-flex items-center gap-1"><Thermometer size={14} /> não exposta</span>} />
        </div>
      </DiagnosticCard>

      {/* Métricas de turnos */}
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

      {/* Latência */}
      <DiagnosticCard icon={Clock3} title="Latência">
        <div className="grid gap-3 md:grid-cols-3">
          <Metric label="STT" value={sttLatency} />
          <Metric label="LLM total" value={llmLatency} />
          <Metric label="TTS áudio" value={ttsLatency} />
        </div>
      </DiagnosticCard>

      {/* Endpoints sem resposta */}
      {diagErrors.length > 0 && (
        <DiagnosticCard icon={Terminal} title="Endpoints sem resposta" wide>
          <div className="grid gap-2 md:grid-cols-2">
            {diagErrors.map(([key, value]) => (
              <InfoRow key={key} label={key} value={String(value)} />
            ))}
          </div>
        </DiagnosticCard>
      )}
    </div>
  );
}
