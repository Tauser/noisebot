"""NoiseBot server command-line interface."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from collections.abc import Sequence

from .config import LlmProvider, PipelineMode, find_env_file, load_config
from .runtime import DEFAULT_LOG_FILE, run_server


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="noisebot-server",
        description="NoiseBot local companion server",
    )
    sub = parser.add_subparsers(dest="command")

    service = sub.add_parser("service", help="Gerenciar como servico do sistema")
    service_sub = service.add_subparsers(dest="service_command", required=True)
    service_sub.add_parser("install")
    service_sub.add_parser("uninstall")
    service_sub.add_parser("status")
    service_sub.add_parser("start")
    service_sub.add_parser("stop")

    debug = sub.add_parser("debug", help="Ferramentas manuais de teste")
    debug_sub = debug.add_subparsers(dest="debug_command", required=True)
    transcript = debug_sub.add_parser("transcript")
    transcript.add_argument("text")
    transcript.add_argument("--turn-id", type=int, default=1)

    transcript_live = debug_sub.add_parser("transcript-live")
    transcript_live.add_argument("text")
    transcript_live.add_argument("--turn-id", type=int, default=0)
    transcript_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    transcript_live.add_argument("--token", default="")
    transcript_live.add_argument("--json", action="store_true", help="Emitir JSON")

    fake_fw = debug_sub.add_parser("fake-fw")
    fake_fw.add_argument("--host", default="127.0.0.1")
    fake_fw.add_argument("--port", type=int, default=9001)
    fake_fw.add_argument("--features", default="")
    fake_fw.add_argument("--audio-format", choices=["pcm16", "opus"], default="pcm16")
    fake_fw.add_argument("--auto-silence-chunks", type=int, default=0)

    audio_report = debug_sub.add_parser("audio-report")
    audio_report.add_argument("path", help="Pasta ou WAV para analisar")
    audio_report.add_argument("--output", help="Arquivo Markdown de saida")
    audio_report.add_argument("--json", action="store_true", help="Emitir JSON")

    afe_ab = debug_sub.add_parser("afe-ab")
    afe_ab.add_argument("phrase", help="Frase a repetir em RAW e AFE")
    afe_ab.add_argument("--server-url", default="http://127.0.0.1:8765")
    afe_ab.add_argument("--firmware-url", default="")
    afe_ab.add_argument("--repeat", type=int, default=1)
    afe_ab.add_argument("--timeout-s", type=float, default=35.0)
    afe_ab.add_argument("--output", help="Arquivo Markdown de saida")
    afe_ab.add_argument("--json", action="store_true", help="Emitir JSON")

    opus_test = debug_sub.add_parser("opus-selftest")
    opus_test.add_argument("--seconds", type=float, default=1.0)
    opus_test.add_argument("--bitrate", type=int, default=24000)
    opus_test.add_argument("--json", action="store_true", help="Emitir JSON")

    opus_quality = debug_sub.add_parser("opus-quality")
    opus_quality.add_argument("path", help="Pasta ou WAV para medir roundtrip Opus")
    opus_quality.add_argument("--bitrates", default="16000,24000,32000")
    opus_quality.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    opus_quality.add_argument("--json", action="store_true", help="Emitir JSON")

    opus_live = debug_sub.add_parser("opus-live")
    opus_live.add_argument("phrase", nargs="?", default="fale algo curto")
    opus_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    opus_live.add_argument("--firmware-url", default="")
    opus_live.add_argument("--timeout-s", type=float, default=45.0)
    opus_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    opus_live.add_argument("--json", action="store_true", help="Emitir JSON")

    codec_ab = debug_sub.add_parser("codec-ab")
    codec_ab.add_argument(
        "phrases",
        nargs="+",
        help="Frases pareadas para testar em PCM16 e Opus",
    )
    codec_ab.add_argument("--server-url", default="http://127.0.0.1:8765")
    codec_ab.add_argument("--firmware-url", default="")
    codec_ab.add_argument("--repeat", type=int, default=1)
    codec_ab.add_argument("--timeout-s", type=float, default=45.0)
    codec_ab.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    codec_ab.add_argument("--json", action="store_true", help="Emitir JSON")

    barge_live = debug_sub.add_parser("barge-live")
    barge_live.add_argument("phrase", nargs="?", default="me conte uma historia longa")
    barge_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    barge_live.add_argument("--firmware-url", default="")
    barge_live.add_argument("--codec", choices=["pcm16", "opus-v2"], default="pcm16")
    barge_live.add_argument("--timeout-s", type=float, default=45.0)
    barge_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    barge_live.add_argument("--json", action="store_true", help="Emitir JSON")

    no_echo_live = debug_sub.add_parser("no-echo-live")
    no_echo_live.add_argument("phrase", nargs="?", default="me conte uma historia longa")
    no_echo_live.add_argument("--server-url", default="http://127.0.0.1:8765")
    no_echo_live.add_argument("--firmware-url", default="")
    no_echo_live.add_argument("--codec", choices=["pcm16", "opus-v2"], default="pcm16")
    no_echo_live.add_argument("--quiet-window-s", type=float, default=10.0)
    no_echo_live.add_argument("--timeout-s", type=float, default=45.0)
    no_echo_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    no_echo_live.add_argument("--json", action="store_true", help="Emitir JSON")

    release_check = debug_sub.add_parser("voice-release-check")
    release_check.add_argument("--server-url", default="http://127.0.0.1:8765")
    release_check.add_argument("--firmware-url", default="")
    release_check.add_argument("--timeout-s", type=float, default=1.5)
    release_check.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    release_check.add_argument("--json", action="store_true", help="Emitir JSON")

    vision_soak = debug_sub.add_parser("vision-soak")
    vision_soak.add_argument("--firmware-url", default="")
    vision_soak.add_argument("--duration-s", type=float, default=1800.0)
    vision_soak.add_argument("--interval-s", type=float, default=5.0)
    vision_soak.add_argument("--timeout-s", type=float, default=8.0)
    vision_soak.add_argument("--expect-absence", action="store_true")
    vision_soak.add_argument("--min-fps", type=float, default=None)
    vision_soak.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    vision_soak.add_argument("--json", action="store_true", help="Emitir JSON")

    vision_presence = debug_sub.add_parser("vision-presence-trial")
    vision_presence.add_argument("--firmware-url", default="")
    vision_presence.add_argument("--mode", choices=["absence", "presence", "lost"], default="presence")
    vision_presence.add_argument("--duration-s", type=float, default=8.0)
    vision_presence.add_argument("--interval-s", type=float, default=0.2)
    vision_presence.add_argument("--timeout-s", type=float, default=8.0)
    vision_presence.add_argument("--max-latency-ms", type=float, default=None)
    vision_presence.add_argument("--start-delay-s", type=float, default=0.0)
    vision_presence.add_argument("--fps-sample-delay-s", type=float, default=0.0)
    vision_presence.add_argument("--close-each-sample", action="store_true")
    vision_presence.add_argument("--reset-presence", action="store_true")
    vision_presence.add_argument("--min-fps", type=float, default=None)
    vision_presence.add_argument(
        "--require-initial-state",
        choices=["absent", "candidate", "present"],
        default=None,
    )
    vision_presence.add_argument(
        "--require-final-state",
        choices=["absent", "candidate", "present"],
        default=None,
    )
    vision_presence.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    vision_presence.add_argument("--json", action="store_true", help="Emitir JSON")

    voice_v2 = debug_sub.add_parser("voice-v2")
    voice_v2.add_argument("action", choices=["status"], nargs="?", default="status")
    voice_v2.add_argument("--firmware-url", default="")
    voice_v2.add_argument("--json", action="store_true", help="Emitir JSON")

    aec_live = debug_sub.add_parser("aec-live")
    aec_live.add_argument("--firmware-url", default="")
    aec_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    aec_live.add_argument("--json", action="store_true", help="Emitir JSON")

    capture_v2 = debug_sub.add_parser("capture-v2")
    capture_v2.add_argument(
        "action",
        choices=[
            "status",
            "replay",
            "cancel",
            "enable",
            "disable",
            "tx-enable",
            "tx-disable",
            "live",
        ],
        nargs="?",
        default="status",
    )
    capture_v2.add_argument("--firmware-url", default="")
    capture_v2.add_argument("--speech-ms", type=int, default=640)
    capture_v2.add_argument("--silence-ms", type=int, default=900)
    capture_v2.add_argument("--source", default="debug")
    capture_v2.add_argument("--no-prompt", action="store_true", help="Nao aguardar Enter no modo live")
    capture_v2.add_argument("--json", action="store_true", help="Emitir JSON")

    playback_v2 = debug_sub.add_parser("playback-v2")
    playback_v2.add_argument(
        "action",
        choices=[
            "status",
            "delta",
            "speaker-owner-arm",
            "speaker-owner-disarm",
            "speaker-owner-real-arm",
            "speaker-owner-real-disarm",
            "speaker-owner-gate",
            "speaker-owner-real-window-gate",
        ],
        nargs="?",
        default="status",
    )
    playback_v2.add_argument("--firmware-url", default="")
    playback_v2.add_argument("--no-prompt", action="store_true", help="Nao aguardar Enter no modo delta")
    playback_v2.add_argument(
        "--wait-s",
        type=float,
        default=0.0,
        help="Aguardar N segundos no modo delta antes do snapshot final",
    )
    playback_v2.add_argument(
        "--require-say",
        action="store_true",
        help="Falhar o gate se nenhum SAY real passar no intervalo",
    )
    playback_v2.add_argument(
        "--min-say-chunks",
        type=int,
        default=0,
        help="Minimo de chunks SAY recebidos para o speaker-owner-gate",
    )
    playback_v2.add_argument("--json", action="store_true", help="Emitir JSON")

    io_v2 = debug_sub.add_parser("io-v2")
    io_v2.add_argument(
        "action",
        choices=[
            "status",
            "speaker-handoff-enable",
            "speaker-handoff-disable",
            "speaker-handoff-owner-arm",
            "speaker-handoff-owner-disarm",
        ],
        nargs="?",
        default="status",
    )
    io_v2.add_argument("--firmware-url", default="")
    io_v2.add_argument("--json", action="store_true", help="Emitir JSON")

    codec_v2 = debug_sub.add_parser("codec-v2")
    codec_v2.add_argument(
        "action",
        choices=[
            "status",
            "health",
            "encode-test",
            "drain",
            "egress-drain",
            "reset",
            "opus-encode-test",
            "worker-start",
            "worker-stop",
            "worker-stress-test",
            "worker-feed-test",
            "bridge-handoff-test",
            "transport-enable",
            "transport-disable",
            "overflow-test",
        ],
        nargs="?",
        default="status",
    )
    codec_v2.add_argument("--firmware-url", default="")
    codec_v2.add_argument("--packets", type=int, default=45)
    codec_v2.add_argument("--frames", type=int, default=10)
    codec_v2.add_argument("--json", action="store_true", help="Emitir JSON")

    parser.add_argument("--host", help="IP do ESP32")
    parser.add_argument("--port", type=int, help="Porta TCP")
    parser.add_argument("--uart", help="Porta UART")
    parser.add_argument("--dry-run", action="store_true", help="Sem conexao real")
    parser.add_argument(
        "--pipeline",
        choices=[mode.value for mode in PipelineMode],
        help="Modo do pipeline",
    )
    parser.add_argument(
        "--llm",
        choices=[provider.value for provider in LlmProvider],
        help="Provider LLM",
    )
    parser.add_argument("--model", help="Modelo LLM")
    parser.add_argument(
        "--audio-codec",
        choices=["pcm16", "opus-v2"],
        help="Codec de audio ativo no startup do server",
    )
    parser.add_argument(
        "--log-level",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Nivel de log",
    )
    parser.add_argument("--env", help="Caminho para arquivo .env alternativo")
    parser.add_argument(
        "--log-file",
        default=None,
        metavar="PATH",
        help=(
            f"Arquivo de log com rotacao automatica (padrao: {DEFAULT_LOG_FILE}). "
            "Use 'stderr' para desabilitar arquivo."
        ),
    )
    return parser.parse_args(argv)


def apply_env_overrides(args: argparse.Namespace) -> None:
    """Apply CLI flags to environment before loading config."""
    if args.host:
        os.environ["NOISEBOT_HOST"] = args.host
    if args.port:
        os.environ["NOISEBOT_PORT"] = str(args.port)
    if args.uart:
        os.environ["NOISEBOT_UART"] = args.uart
    if args.dry_run:
        os.environ["NOISEBOT_DRY_RUN"] = "true"
    if args.pipeline:
        os.environ["NOISEBOT_PIPELINE_MODE"] = args.pipeline
    if args.llm:
        os.environ["NOISEBOT_LLM_PROVIDER"] = args.llm
    if args.model:
        os.environ["NOISEBOT_LLM_MODEL"] = args.model
    if args.audio_codec:
        os.environ["NOISEBOT_AUDIO_DEFAULT_CODEC"] = args.audio_codec
    if args.log_level:
        os.environ["NOISEBOT_LOG_LEVEL"] = args.log_level


def run_debug_command(args: argparse.Namespace) -> None:
    """Run server-owned debug commands."""
    from .internal.debug.manual import (
        run_fake_firmware_debug,
        run_live_transcript_debug,
        run_transcript_debug,
    )

    if args.debug_command == "transcript":
        raise SystemExit(asyncio.run(run_transcript_debug(args.text, args.turn_id)))
    if args.debug_command == "transcript-live":
        raise SystemExit(
            run_live_transcript_debug(
                text=args.text,
                server_url=args.server_url,
                turn_id=args.turn_id,
                token=args.token,
                emit_json=args.json,
            )
        )
    if args.debug_command == "fake-fw":
        features = [item.strip() for item in args.features.split(",") if item.strip()]
        raise SystemExit(
            asyncio.run(
                run_fake_firmware_debug(
                    args.host,
                    args.port,
                    features,
                    args.audio_format,
                    args.auto_silence_chunks,
                )
            )
        )
    if args.debug_command == "audio-report":
        from .internal.ops.audio_analysis import (
            analyze_audio_samples,
            format_audio_samples_markdown,
            summarize_audio_samples,
        )

        samples = analyze_audio_samples(args.path)
        if args.json:
            payload = {
                "summary": summarize_audio_samples(samples),
                "samples": [sample.to_dict() for sample in samples],
            }
            text = json.dumps(payload, ensure_ascii=False, indent=2)
        else:
            text = format_audio_samples_markdown(samples)

        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        return
    if args.debug_command == "afe-ab":
        from .internal.ops.voice_ab import (
            VoiceAbTrial,
            collect_trial,
            format_voice_ab_markdown,
            reset_afe_counters,
            set_afe_bridge,
            stop_afe_shadow,
            wait_for_new_turn,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        server_url = args.server_url.rstrip("/")
        firmware_url = firmware_url.rstrip("/")
        modes = ["raw", "afe"] * max(1, args.repeat)
        trials: list[VoiceAbTrial] = []
        print(f"Frase alvo: {args.phrase}")
        print("Use a mesma distancia e volume em todos os turnos.")
        try:
            for index, mode in enumerate(modes, start=1):
                reset_afe_counters(firmware_url)
                before = collect_trial(
                    mode=mode,
                    phrase=args.phrase,
                    server_url=server_url,
                    firmware_url=firmware_url,
                )
                if mode == "afe":
                    set_afe_bridge(firmware_url, True)
                else:
                    set_afe_bridge(firmware_url, False)
                    try:
                        stop_afe_shadow(firmware_url)
                    except Exception:
                        pass
                print(f"\n[{index}/{len(modes)}] Modo {mode.upper()}")
                input("Fale a frase agora e pressione Enter quando o robô terminar: ")
                metrics = wait_for_new_turn(
                    server_url=server_url,
                    previous_turn_id=before.turn_id,
                    timeout_s=args.timeout_s,
                )
                processor = collect_trial(
                    mode=mode,
                    phrase=args.phrase,
                    server_url=server_url,
                    firmware_url=firmware_url,
                )
                trial = VoiceAbTrial.from_payload(
                    mode=mode,
                    phrase=args.phrase,
                    metrics=metrics,
                    processor=processor.to_dict(),
                )
                trials.append(trial)
                print(
                    f"turn={trial.turn_id} quality={trial.transcript_quality} "
                    f"no_speech={trial.no_speech_prob} transcript={trial.transcript!r}"
                )
        finally:
            try:
                set_afe_bridge(firmware_url, False)
                stop_afe_shadow(firmware_url)
            except Exception:
                pass

        if args.json:
            text = json.dumps([trial.to_dict() for trial in trials], ensure_ascii=False, indent=2)
        else:
            text = format_voice_ab_markdown(trials)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        return
    if args.debug_command == "opus-selftest":
        import math

        import numpy as np

        from .internal.transport.opus_codec import (
            OPUS_FRAME_MS,
            OPUS_FRAME_SAMPLES,
            OPUS_SAMPLE_RATE_HZ,
            roundtrip_pcm,
        )

        n_samples = max(OPUS_FRAME_SAMPLES, int(args.seconds * OPUS_SAMPLE_RATE_HZ))
        t = np.arange(n_samples, dtype=np.float32) / float(OPUS_SAMPLE_RATE_HZ)
        pcm = (np.sin(2.0 * math.pi * 440.0 * t) * 8000.0).astype(np.int16)
        decoded, stats = roundtrip_pcm(pcm, bitrate=args.bitrate)
        payload = {
            "ok": True,
            "sample_rate_hz": OPUS_SAMPLE_RATE_HZ,
            "frame_ms": OPUS_FRAME_MS,
            "frame_samples": OPUS_FRAME_SAMPLES,
            "bitrate": args.bitrate,
            "input_bytes": stats.input_bytes,
            "packet_count": stats.packet_count,
            "opus_bytes": stats.opus_bytes,
            "decoded_bytes": len(decoded),
            "compression_ratio": round(stats.compression_ratio, 4),
        }
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(
                "Opus OK: "
                f"{payload['packet_count']} packets, "
                f"{payload['opus_bytes']} bytes de {payload['input_bytes']} "
                f"({payload['compression_ratio']:.2%}), "
                f"decoded={payload['decoded_bytes']} bytes"
            )
        return
    if args.debug_command == "opus-quality":
        from .internal.ops.opus_quality import (
            analyze_opus_quality,
            format_opus_quality_json,
            format_opus_quality_markdown,
        )

        bitrates = _parse_bitrates(args.bitrates)
        results = analyze_opus_quality(args.path, bitrates=bitrates)
        text = (
            format_opus_quality_json(results)
            if args.json
            else format_opus_quality_markdown(results)
        )
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        return
    if args.debug_command == "opus-live":
        from .internal.ops.opus_live import (
            format_opus_live_json,
            format_opus_live_markdown,
            run_opus_live_trial,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        trial = run_opus_live_trial(
            phrase=args.phrase,
            server_url=args.server_url,
            firmware_url=firmware_url,
            timeout_s=args.timeout_s,
        )
        text = format_opus_live_json(trial) if args.json else format_opus_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "codec-ab":
        from .internal.ops.codec_ab import (
            format_codec_ab_json,
            format_codec_ab_markdown,
            run_codec_ab_trials,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        trials = run_codec_ab_trials(
            phrases=args.phrases,
            server_url=args.server_url,
            firmware_url=firmware_url,
            repeat=args.repeat,
            timeout_s=args.timeout_s,
        )
        text = format_codec_ab_json(trials) if args.json else format_codec_ab_markdown(trials)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not all(trial.ok for trial in trials):
            raise SystemExit(1)
        return
    if args.debug_command == "barge-live":
        from .internal.ops.barge_live import (
            format_barge_live_json,
            format_barge_live_markdown,
            run_barge_live_trial,
        )

        firmware_url = _resolve_firmware_url(args)
        trial = run_barge_live_trial(
            phrase=args.phrase,
            server_url=args.server_url,
            timeout_s=args.timeout_s,
            codec=args.codec,
            firmware_url=firmware_url,
        )
        text = format_barge_live_json(trial) if args.json else format_barge_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "no-echo-live":
        from .internal.ops.no_echo_live import (
            format_no_echo_live_json,
            format_no_echo_live_markdown,
            run_no_echo_live_trial,
        )

        firmware_url = _resolve_firmware_url(args)
        trial = run_no_echo_live_trial(
            phrase=args.phrase,
            server_url=args.server_url,
            quiet_window_s=args.quiet_window_s,
            timeout_s=args.timeout_s,
            codec=args.codec,
            firmware_url=firmware_url,
        )
        text = format_no_echo_live_json(trial) if args.json else format_no_echo_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "voice-release-check":
        from .internal.ops.release_check import (
            format_release_check_json,
            format_release_check_markdown,
            run_release_check,
        )

        firmware_url = _resolve_firmware_url(args)
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")
        check = run_release_check(
            firmware_url=firmware_url,
            server_url=args.server_url,
            timeout_s=args.timeout_s,
        )
        text = format_release_check_json(check) if args.json else format_release_check_markdown(check)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not check.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "aec-live":
        from .internal.ops.aec_live import (
            format_aec_live_json,
            format_aec_live_markdown,
            run_aec_live_probe,
        )

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        trial = run_aec_live_probe(firmware_url=firmware_url)
        text = format_aec_live_json(trial) if args.json else format_aec_live_markdown(trial)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not trial.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "vision-soak":
        from .internal.ops.vision_soak import (
            format_vision_soak_json,
            format_vision_soak_markdown,
            run_vision_soak,
        )

        firmware_url = _resolve_firmware_url(args)
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")
        result = run_vision_soak(
            firmware_url=firmware_url,
            duration_s=args.duration_s,
            interval_s=args.interval_s,
            timeout_s=args.timeout_s,
            expect_absence=args.expect_absence,
            min_fps_required=args.min_fps,
        )
        text = (
            format_vision_soak_json(result)
            if args.json
            else format_vision_soak_markdown(result)
        )
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not result.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "vision-presence-trial":
        from .internal.ops.vision_presence_trial import (
            format_vision_presence_trial_json,
            format_vision_presence_trial_markdown,
            run_vision_presence_trial,
        )

        firmware_url = _resolve_firmware_url(args)
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")
        result = run_vision_presence_trial(
            firmware_url=firmware_url,
            mode=args.mode,
            duration_s=args.duration_s,
            interval_s=args.interval_s,
            timeout_s=args.timeout_s,
            max_latency_ms=args.max_latency_ms,
            start_delay_s=args.start_delay_s,
            fps_sample_delay_s=args.fps_sample_delay_s,
            close_each_sample=args.close_each_sample,
            reset_presence=args.reset_presence,
            min_fps_required=args.min_fps,
            require_initial_state=args.require_initial_state,
            require_final_state=args.require_final_state,
        )
        text = (
            format_vision_presence_trial_json(result)
            if args.json
            else format_vision_presence_trial_markdown(result)
        )
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as file:
                file.write(text)
        else:
            print(text)
        if not result.ok:
            raise SystemExit(1)
        return
    if args.debug_command == "voice-v2":
        from .internal.ops.firmware_diag import FirmwareDiagClient

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        client = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=1.5)
        payload = client.audio_voice_v2_status()

        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(_format_voice_v2_status(payload))
        if not payload.get("ok", False):
            raise SystemExit(1)
        return
    if args.debug_command == "capture-v2":
        from .internal.ops.firmware_diag import FirmwareDiagClient

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        timeout_s = 1.5
        client = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=timeout_s)
        if args.action == "status":
            payload = client.audio_capture_v2_status()
        elif args.action == "replay":
            payload = client.audio_capture_v2_replay({
                "speech_ms": args.speech_ms,
                "silence_ms": args.silence_ms,
                "source": args.source,
            })
        elif args.action == "enable":
            payload = client.set_voice_audio_v2_capture_enabled(True)
        elif args.action == "disable":
            payload = client.set_voice_audio_v2_capture_enabled(False)
        elif args.action == "tx-enable":
            payload = client.set_voice_audio_v2_capture_tx_enabled(True)
        elif args.action == "tx-disable":
            payload = client.set_voice_audio_v2_capture_tx_enabled(False)
        elif args.action == "live":
            payload = _run_capture_v2_live(client, no_prompt=args.no_prompt)
        else:
            payload = client.audio_capture_v2_cancel()

        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(_format_capture_v2_status(payload))
        if not payload.get("ok", False):
            raise SystemExit(1)
        return
    if args.debug_command == "playback-v2":
        from .internal.ops.firmware_diag import FirmwareDiagClient

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        client = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=1.5)
        if args.action == "delta":
            payload = _run_playback_v2_delta(
                client,
                no_prompt=args.no_prompt,
                wait_s=args.wait_s,
            )
        elif args.action == "speaker-owner-gate":
            payload = _run_playback_v2_speaker_owner_gate(
                client,
                no_prompt=args.no_prompt,
                wait_s=args.wait_s,
                require_say=args.require_say,
                min_say_chunks=args.min_say_chunks,
            )
        elif args.action == "speaker-owner-real-window-gate":
            payload = _run_playback_v2_speaker_owner_real_window_gate(
                client,
                no_prompt=args.no_prompt,
                wait_s=args.wait_s,
                min_say_chunks=args.min_say_chunks,
            )
        elif args.action == "speaker-owner-arm":
            payload = client.audio_playback_v2_speaker_owner_arm()
        elif args.action == "speaker-owner-disarm":
            payload = client.audio_playback_v2_speaker_owner_disarm()
        elif args.action == "speaker-owner-real-arm":
            payload = client.audio_playback_v2_speaker_owner_real_arm()
        elif args.action == "speaker-owner-real-disarm":
            payload = client.audio_playback_v2_speaker_owner_real_disarm()
        else:
            payload = client.audio_playback_v2_status()

        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(_format_playback_v2_status(payload))
        if not payload.get("ok", False):
            raise SystemExit(1)
        return
    if args.debug_command == "io-v2":
        from .internal.ops.firmware_diag import FirmwareDiagClient

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        client = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=1.5)
        if args.action == "speaker-handoff-enable":
            payload = client.audio_io_v2_speaker_handoff_enable()
        elif args.action == "speaker-handoff-disable":
            payload = client.audio_io_v2_speaker_handoff_disable()
        elif args.action == "speaker-handoff-owner-arm":
            payload = client.audio_io_v2_speaker_handoff_owner_arm()
        elif args.action == "speaker-handoff-owner-disarm":
            payload = client.audio_io_v2_speaker_handoff_owner_disarm()
        else:
            payload = client.audio_io_v2_status()

        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print(_format_io_v2_status(payload))
        if not payload.get("ok", False):
            raise SystemExit(1)
        return
    if args.debug_command == "codec-v2":
        from .internal.ops.firmware_diag import FirmwareDiagClient

        firmware_url = args.firmware_url or os.environ.get("NOISEBOT_ROBOT_HTTP_URL", "")
        if not firmware_url:
            host = args.host or os.environ.get("NOISEBOT_HOST", "")
            if host:
                firmware_url = f"http://{host}"
        if not firmware_url:
            raise SystemExit("--firmware-url ou --host/NOISEBOT_HOST e obrigatorio")

        timeout_s = 10.0 if args.action in (
            "opus-encode-test",
            "worker-stress-test",
            "worker-feed-test",
            "bridge-handoff-test",
        ) else 1.5
        client = FirmwareDiagClient(firmware_url.rstrip("/") + "/", timeout_s=timeout_s)
        if args.action == "health":
            payload = client.audio_codec_v2_health()
        elif args.action == "encode-test":
            payload = client.audio_codec_v2_encode_test()
        elif args.action == "drain":
            payload = client.audio_codec_v2_drain()
        elif args.action == "egress-drain":
            payload = client.audio_codec_v2_egress_drain()
        elif args.action == "reset":
            payload = client.audio_codec_v2_reset()
        elif args.action == "opus-encode-test":
            payload = client.audio_codec_v2_opus_encode_test()
        elif args.action == "worker-start":
            payload = client.audio_codec_v2_worker_start()
        elif args.action == "worker-stop":
            payload = client.audio_codec_v2_worker_stop()
        elif args.action == "worker-stress-test":
            payload = client.audio_codec_v2_worker_stress_test(args.packets)
        elif args.action == "worker-feed-test":
            payload = client.audio_codec_v2_worker_feed_test(args.frames)
        elif args.action == "bridge-handoff-test":
            payload = client.audio_codec_v2_bridge_handoff_test(args.frames)
        elif args.action == "transport-enable":
            payload = client.audio_codec_v2_transport_enable()
        elif args.action == "transport-disable":
            payload = client.audio_codec_v2_transport_disable()
        elif args.action == "overflow-test":
            payload = client.audio_codec_v2_overflow_test(args.packets)
        else:
            payload = client.audio_codec_v2_status()
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            if args.action == "health":
                print(_format_codec_v2_health(payload))
            else:
                print(_format_codec_v2_status(payload))
        if not payload.get("ok", False):
            raise SystemExit(1)
        return
    raise SystemExit(2)


def _format_voice_v2_status(payload: dict[str, object]) -> str:
    ownership = payload.get("ownership")
    ownership_lines: list[str] = []
    if isinstance(ownership, dict):
        ownership_lines = [
            "",
            "## Ownership",
            "",
            f"- HAL/I2S: {ownership.get('hal_i2s', '')}",
            f"- RX: {ownership.get('rx', '')}",
            f"- TX: {ownership.get('tx', '')}",
            f"- VAD: {ownership.get('vad', '')}",
            f"- Capture: {ownership.get('capture', '')}",
            f"- Bridge TX: {ownership.get('bridge_tx', '')}",
            f"- Codec: {ownership.get('codec', '')}",
            f"- Playback queue: {ownership.get('playback_queue', '')}",
            f"- Playback HAL: {ownership.get('playback_hal', '')}",
            f"- Bridge legado: {ownership.get('legacy_bridge', '')}",
        ]
    return "\n".join(
        [
            "# Voice Audio v2",
            "",
            f"- OK: {payload.get('ok', False)}",
            f"- Ready: {payload.get('ready', False)}",
            f"- Block reason: {payload.get('block_reason', '')}",
            f"- Rollback disponivel: {payload.get('rollback_available', '')}",
            f"- Capture: enabled={payload.get('capture_enabled', '')}, "
            f"tx={payload.get('capture_tx_enabled', '')}, "
            f"session_active={payload.get('capture_session_active', '')}",
            f"- Activity decider: {payload.get('activity_decider_enabled', '')} "
            f"(session_active={payload.get('activity_session_active', '')})",
            f"- Codec worker: {payload.get('codec_worker_state', '')} "
            f"(active={payload.get('codec_worker_active', '')})",
            f"- Playback queue owner: {payload.get('playback_queue_owner', '')}",
            f"- Runtime idle: {payload.get('runtime_idle', '')}",
            f"- SAY queue/drops: {payload.get('playback_say_queue_count', '')}/"
            f"{payload.get('playback_say_drops', '')}",
            f"- Codec queue/egress: {payload.get('codec_queue_count', '')}/"
            f"{payload.get('codec_egress_queue_count', '')}",
            f"- Drops: codec={payload.get('codec_packet_drops', '')}, "
            f"egress={payload.get('codec_egress_drops', '')}, "
            f"io={payload.get('audio_io_dropped_frames', '')}",
            f"- I2S recoveries: {payload.get('audio_io_i2s_recoveries', '')}",
        ] + ownership_lines
    )


def _format_io_v2_status(payload: dict[str, object]) -> str:
    return "\n".join(
        [
            "# Audio IO v2",
            "",
            f"- OK: {payload.get('ok', False)}",
            f"- Inicializado: {payload.get('initialized', '')}",
            f"- RX dispatch: {payload.get('rx_dispatch_calls', '')} calls / "
            f"{payload.get('rx_dispatch_last_consumers', '')} consumidores",
            f"- TX owner: {payload.get('tx_owner_observed', '')} / "
            f"{payload.get('tx_owner_frames', '')} frames",
            f"- TX real: {payload.get('tx_frames', '')} frames / "
            f"{payload.get('tx_silence_frames', '')} silencio",
            f"- I2S recoveries: {payload.get('i2s_recoveries', '')}",
            f"- Drops: {payload.get('dropped_frames', '')}",
            f"- Speaker handoff dry-run: "
            f"{payload.get('speaker_handoff_dry_run_enabled', '')}",
            f"- Speaker handoff owner armado: "
            f"{payload.get('speaker_handoff_owner_requested', '')}",
            f"- Speaker handoff owner pronto: "
            f"{payload.get('speaker_handoff_owner_ready', '')}",
            f"- Speaker handoff active: {payload.get('speaker_handoff_active', '')}",
            f"- Speaker handoff ready: {payload.get('speaker_handoff_ready', '')} "
            f"({payload.get('speaker_handoff_block_reason', '')})",
            f"- Speaker handoff frames: {payload.get('speaker_handoff_frames', '')} / "
            f"{payload.get('speaker_handoff_silence_frames', '')} silencio",
            f"- Speaker handoff falhas/recoveries: "
            f"{payload.get('speaker_handoff_failures', '')}/"
            f"{payload.get('speaker_handoff_recoveries', '')}",
            f"- Erro: {payload.get('error', '')}",
        ]
    )


def _format_codec_v2_status(payload: dict[str, object]) -> str:
    return "\n".join(
        [
            "# Codec v2",
            "",
            f"- OK: {payload.get('ok', False)}",
            f"- Inicializado: {payload.get('initialized', '')}",
            f"- Formato: {payload.get('format', '')}",
            f"- Worker: {payload.get('worker_state', '')} "
            f"(supported={payload.get('worker_supported', '')}, "
            f"active={payload.get('worker_active', '')})",
            f"- Opus: {payload.get('opus_frame_ms', '')} ms / "
            f"{payload.get('opus_frame_samples', '')} samples / "
            f"{payload.get('opus_bitrate', '')} bps",
            f"- Fila max: {payload.get('max_queue_packets', '')}",
            f"- Fila atual: {payload.get('queue_count', '')}",
            f"- Pacotes/drop: {payload.get('packets_out', '')}/"
            f"{payload.get('packet_drops', '')}",
            f"- Worker drain: {payload.get('worker_drained_packets', '')}",
            f"- Opus teste: {payload.get('opus_encode_tests', '')} / "
            f"{payload.get('opus_last_packet_bytes', payload.get('encoded_bytes', ''))} bytes",
            f"- Samples pendentes: {payload.get('pending_samples', '')}",
            f"- Pacotes drenados: {payload.get('drained_packets', '')}",
            f"- Erro: {payload.get('error', '')}",
        ]
    )


def _format_codec_v2_health(payload: dict[str, object]) -> str:
    issues = payload.get("issues")
    warnings = payload.get("warnings")
    if not isinstance(issues, list):
        issues = []
    if not isinstance(warnings, list):
        warnings = []
    return "\n".join(
        [
            "# Codec v2 health",
            "",
            f"- Saudavel: {payload.get('healthy', False)}",
            f"- Status: {payload.get('status', '')}",
            f"- Formato: {payload.get('format', '')}",
            f"- Worker: {payload.get('worker_state', '')} "
            f"(active={payload.get('worker_active', '')})",
            f"- Drops: packets={payload.get('packet_drops', '')}, "
            f"egress={payload.get('opus_egress_packet_drops', '')}",
            f"- Fila egress: {payload.get('opus_egress_queue_count', '')}",
            f"- Erro Opus: {payload.get('opus_codec_error', '')}",
            f"- Issues: {', '.join(str(item) for item in issues) if issues else 'nenhuma'}",
            f"- Warnings: {', '.join(str(item) for item in warnings) if warnings else 'nenhum'}",
            f"- Rollback: {payload.get('rollback_hint', '')}",
        ]
    )


def _playback_v2_counter_delta(before: dict[str, object],
                               after: dict[str, object],
                               key: str) -> int:
    try:
        return int(after.get(key, 0) or 0) - int(before.get(key, 0) or 0)
    except (TypeError, ValueError):
        return 0


def _run_playback_v2_delta(client, *, no_prompt: bool, wait_s: float = 0.0) -> dict[str, object]:
    from .internal.ops.firmware_diag import codec_v2_health_from_status

    before = client.audio_playback_v2_status()
    io_before = client.audio_io_v2_status()
    codec_before = client.audio_codec_v2_status()
    if wait_s > 0.0:
        time.sleep(wait_s)
    elif not no_prompt:
        input(
            "Snapshot inicial feito. Acione o wake/fale uma frase real e "
            "pressione Enter quando o turno terminar: "
        )
    after = client.audio_playback_v2_status()
    io_after = client.audio_io_v2_status()
    codec_after = client.audio_codec_v2_status()
    codec_health = codec_v2_health_from_status(codec_after)
    delta_keys = [
        "say_chunks_received",
        "say_chunks_played",
        "say_chunks_dropped",
        "say_chunks_dropped_listening",
        "say_chunks_cancelled",
        "say_cancel_count",
        "speaker_write_requests",
        "speaker_write_failures",
        "speaker_frames_committed",
        "speaker_commit_failures",
    ]
    deltas = {
        key: _playback_v2_counter_delta(before, after, key)
        for key in delta_keys
    }
    io_delta_keys = [
        "dropped_frames",
        "i2s_recoveries",
        "speaker_handoff_failures",
        "speaker_handoff_recoveries",
    ]
    io_deltas = {
        key: _playback_v2_counter_delta(io_before, io_after, key)
        for key in io_delta_keys
    }
    codec_delta_keys = [
        "packet_drops",
        "opus_egress_packet_drops",
        "opus_codec_error",
    ]
    codec_deltas = {
        key: _playback_v2_counter_delta(codec_before, codec_after, key)
        for key in codec_delta_keys
    }
    negative_deltas = {
        key: value
        for key, value in {
            **deltas,
            **{f"audio_io.{key}": value for key, value in io_deltas.items()},
            **{f"codec.{key}": value for key, value in codec_deltas.items()},
        }.items()
        if value < 0
    }
    counter_reset_detected = bool(negative_deltas)
    queue_empty = int(after.get("say_queue_count", 0) or 0) == 0
    normal_path_clean = (
        not counter_reset_detected and
        deltas["say_chunks_dropped"] == 0 and
        deltas["say_chunks_dropped_listening"] == 0
    )
    issues: list[str] = []
    warnings: list[str] = []
    if not bool(after.get("ok", False)):
        issues.append("playback-v2 status retornou ok=false")
    if not queue_empty:
        issues.append(f"fila SAY nao vazia: {after.get('say_queue_count')}")
    if counter_reset_detected:
        issues.append("contadores v2 resetaram no intervalo; possivel reboot/reset diagnostico")
    elif not normal_path_clean:
        issues.append("Playback v2 registrou drops SAY no intervalo")
    if deltas["speaker_write_failures"] != 0:
        issues.append(f"speaker_write_failures delta={deltas['speaker_write_failures']}")
    if deltas["speaker_commit_failures"] != 0:
        issues.append(f"speaker_commit_failures delta={deltas['speaker_commit_failures']}")
    if any(io_deltas.get(key, 0) != 0 for key in io_deltas):
        issues.append("Audio IO v2 registrou drops/recoveries/falhas no intervalo")
    if any(codec_deltas.get(key, 0) != 0 for key in codec_deltas):
        issues.append("Codec v2 registrou drops/erros no intervalo")
    if not bool(codec_health.get("healthy", False)):
        issues.extend(str(item) for item in codec_health.get("issues", []) or [])
    codec_warnings = codec_health.get("warnings", []) or []
    warnings.extend(str(item) for item in codec_warnings)
    heap_internal_kb = int(io_after.get("heap_internal_free_kb", 0) or 0)
    if 0 < heap_internal_kb < 16:
        warnings.append(f"heap_internal_free_kb baixo: {heap_internal_kb}")
    ok = not issues
    status = "fail" if issues else "warn" if warnings else "ok"
    return {
        "ok": ok,
        "status": status,
        "issues": issues,
        "warnings": warnings,
        "before": before,
        "after": after,
        "audio_io_before": io_before,
        "audio_io_after": io_after,
        "codec_before": codec_before,
        "codec_after": codec_after,
        "codec_health": codec_health,
        "deltas": deltas,
        "audio_io_deltas": io_deltas,
        "codec_deltas": codec_deltas,
        "negative_deltas": negative_deltas,
        "counter_reset_detected": counter_reset_detected,
        "queue_empty": queue_empty,
        "normal_path_clean": normal_path_clean,
    }


def _playback_v2_int(payload: dict[str, object], key: str) -> int:
    try:
        return int(payload.get(key, 0) or 0)
    except (TypeError, ValueError):
        return 0


def _run_playback_v2_speaker_owner_gate(
    client,
    *,
    no_prompt: bool,
    wait_s: float = 0.0,
    require_say: bool = False,
    min_say_chunks: int = 0,
) -> dict[str, object]:
    issues: list[str] = []
    warnings: list[str] = []
    armed: dict[str, object] = {}
    delta: dict[str, object] = {}
    disarmed: dict[str, object] = {}
    delta_error = ""
    try:
        armed = client.audio_playback_v2_speaker_owner_arm()
        if not bool(armed.get("ok", False)):
            issues.append("speaker-owner-arm retornou ok=false")
        delta = _run_playback_v2_delta(client, no_prompt=no_prompt, wait_s=wait_s)
    except Exception as exc:  # pragma: no cover - exercised through CLI tests
        delta_error = str(exc)
        issues.append(f"playback-v2 gate falhou durante delta: {delta_error}")
    finally:
        try:
            disarmed = client.audio_playback_v2_speaker_owner_disarm()
        except Exception as exc:  # pragma: no cover - defensive rollback reporting
            disarmed = {"ok": False, "error": str(exc)}

    after = delta.get("after")
    deltas = delta.get("deltas")
    if not isinstance(after, dict):
        after = {}
    if not isinstance(deltas, dict):
        deltas = {}

    if delta_error:
        pass
    elif not bool(delta.get("ok", False)):
        issues.append("playback-v2 delta retornou ok=false")
    for item in delta.get("issues", []) or []:
        issues.append(str(item))
    for item in delta.get("warnings", []) or []:
        warnings.append(str(item))

    if delta_error:
        pass
    elif not bool(after.get("speaker_owner_dry_run_enabled", False)):
        issues.append("speaker owner dry-run nao ficou armado")
    if not delta_error and not bool(after.get("speaker_owner_candidate", False)):
        issues.append("Playback v2 nao ficou candidato a speaker owner")
    if not delta_error and not bool(after.get("speaker_owner_handoff_ready", False)):
        issues.append("speaker owner handoff nao ficou pronto")
    if not delta_error and str(after.get("speaker_owner_block_reason", "")) != "NONE":
        issues.append(
            "speaker owner bloqueado: "
            f"{after.get('speaker_owner_block_reason')}"
        )
    if not delta_error and _playback_v2_int(after, "speaker_owner_failures") != 0:
        issues.append(
            "speaker_owner_failures="
            f"{after.get('speaker_owner_failures')}"
        )
    if not delta_error and _playback_v2_int(after, "speaker_owner_recoveries") != 0:
        issues.append(
            "speaker_owner_recoveries="
            f"{after.get('speaker_owner_recoveries')}"
        )
    say_chunks_received = _playback_v2_int(deltas, "say_chunks_received")
    required_say_chunks = max(1 if require_say else 0, min_say_chunks)
    speaker_owner_frames = _playback_v2_int(after, "speaker_owner_frames")
    speaker_owner_silence_frames = _playback_v2_int(after, "speaker_owner_silence_frames")
    speaker_owner_non_silence_frames = max(
        0,
        speaker_owner_frames - speaker_owner_silence_frames,
    )
    if delta_error:
        pass
    elif bool(delta.get("counter_reset_detected", False)):
        issues.append("gate SAY real invalido porque os contadores resetaram")
    elif required_say_chunks > 0 and say_chunks_received < required_say_chunks:
        issues.append(
            "SAY real insuficiente no intervalo: "
            f"{say_chunks_received} < {required_say_chunks}"
        )
    elif require_say and not bool(after.get("speaker_owner_active", False)):
        issues.append("speaker owner nao ficou ativo durante SAY real")
    elif require_say and speaker_owner_non_silence_frames == 0:
        issues.append("speaker owner nao observou frame nao silencioso")
    elif say_chunks_received == 0:
        warnings.append("sem SAY real no intervalo; gate validou apenas idle/TX")
    if not bool(disarmed.get("ok", False)):
        issues.append("speaker-owner-disarm retornou ok=false")

    ok = not issues
    status = "fail" if issues else "warn" if warnings else "ok"
    real_owner_candidate_blockers: list[str] = []
    if not require_say:
        real_owner_candidate_blockers.append("gate executado sem --require-say")
    if issues:
        real_owner_candidate_blockers.extend(str(item) for item in issues)
    if not bool(after.get("speaker_owner_handoff_ready", False)):
        real_owner_candidate_blockers.append("speaker_owner_handoff_ready=false")
    if not bool(after.get("speaker_owner_active", False)):
        real_owner_candidate_blockers.append("speaker_owner_active=false")
    if speaker_owner_non_silence_frames == 0:
        real_owner_candidate_blockers.append("sem frame nao silencioso")
    if delta_error:
        real_owner_candidate_blockers.append("delta_error presente")
    if bool(delta.get("counter_reset_detected", False)):
        real_owner_candidate_blockers.append("counter_reset_detected=true")
    if not bool(disarmed.get("ok", False)):
        real_owner_candidate_blockers.append("rollback/disarm falhou")
    real_owner_candidate = not real_owner_candidate_blockers
    if real_owner_candidate and warnings:
        real_owner_candidate_status = "ready_with_warnings"
    elif real_owner_candidate:
        real_owner_candidate_status = "ready"
    else:
        real_owner_candidate_status = "blocked"
    return {
        "ok": ok,
        "status": status,
        "owner_readiness_gate": True,
        "issues": issues,
        "warnings": warnings,
        "armed": armed,
        "delta": delta,
        "delta_error": delta_error,
        "disarmed": disarmed,
        "ready": bool(after.get("speaker_owner_handoff_ready", False)),
        "block_reason": after.get("speaker_owner_block_reason"),
        "active": bool(after.get("speaker_owner_active", False)),
        "require_say": require_say,
        "min_say_chunks": min_say_chunks,
        "required_say_chunks": required_say_chunks,
        "counter_reset_detected": bool(delta.get("counter_reset_detected", False)),
        "real_owner_candidate": real_owner_candidate,
        "real_owner_candidate_status": real_owner_candidate_status,
        "real_owner_candidate_blockers": real_owner_candidate_blockers,
        "frames": after.get("speaker_owner_frames"),
        "samples": after.get("speaker_owner_samples"),
        "silence_frames": after.get("speaker_owner_silence_frames"),
        "non_silence_frames": speaker_owner_non_silence_frames,
        "say_chunks_received_delta": deltas.get("say_chunks_received"),
        "say_chunks_played_delta": deltas.get("say_chunks_played"),
    }


def _run_playback_v2_speaker_owner_real_window_gate(
    client,
    *,
    no_prompt: bool,
    wait_s: float = 0.0,
    min_say_chunks: int = 1,
) -> dict[str, object]:
    issues: list[str] = []
    warnings: list[str] = []
    dry_issues: list[str] = []
    dry_warnings: list[str] = []
    dry_armed: dict[str, object] = {}
    dry_delta: dict[str, object] = {}
    dry_run_gate: dict[str, object] = {}
    real_armed: dict[str, object] = {}
    real_delta: dict[str, object] = {}
    disarmed: dict[str, object] = {}
    dry_delta_error = ""
    real_delta_error = ""
    dry_after: dict[str, object] = {}
    dry_deltas: dict[str, object] = {}
    dry_candidate = False

    try:
        dry_armed = client.audio_playback_v2_speaker_owner_arm()
        if not bool(dry_armed.get("ok", False)):
            dry_issues.append("speaker-owner-arm retornou ok=false")
        else:
            dry_delta = _run_playback_v2_delta(
                client,
                no_prompt=no_prompt,
                wait_s=wait_s,
            )
    except Exception as exc:  # pragma: no cover - defensive CLI reporting
        dry_delta_error = str(exc)
        dry_issues.append(f"dry-run gate falhou: {dry_delta_error}")

    if dry_delta:
        if not bool(dry_delta.get("ok", False)):
            dry_issues.append("dry-run delta retornou ok=false")
        for item in dry_delta.get("issues", []) or []:
            dry_issues.append(str(item))
        for item in dry_delta.get("warnings", []) or []:
            dry_warnings.append(str(item))

        maybe_after = dry_delta.get("after")
        maybe_deltas = dry_delta.get("deltas")
        if isinstance(maybe_after, dict):
            dry_after = maybe_after
        if isinstance(maybe_deltas, dict):
            dry_deltas = maybe_deltas

        dry_received = _playback_v2_int(dry_deltas, "say_chunks_received")
        required_chunks = max(1, min_say_chunks)
        dry_frames = _playback_v2_int(dry_after, "speaker_owner_frames")
        dry_silence_frames = _playback_v2_int(dry_after, "speaker_owner_silence_frames")
        dry_non_silence_frames = max(0, dry_frames - dry_silence_frames)

        if not bool(dry_after.get("speaker_owner_dry_run_enabled", False)):
            dry_issues.append("speaker owner dry-run nao ficou armado")
        if not bool(dry_after.get("speaker_owner_candidate", False)):
            dry_issues.append("Playback v2 nao ficou candidato a speaker owner")
        if not bool(dry_after.get("speaker_owner_handoff_ready", False)):
            dry_issues.append("speaker owner handoff nao ficou pronto")
        if str(dry_after.get("speaker_owner_block_reason", "")) != "NONE":
            dry_issues.append(
                "speaker owner bloqueado: "
                f"{dry_after.get('speaker_owner_block_reason')}"
            )
        if _playback_v2_int(dry_after, "speaker_owner_failures") != 0:
            dry_issues.append(
                "speaker_owner_failures="
                f"{dry_after.get('speaker_owner_failures')}"
            )
        if _playback_v2_int(dry_after, "speaker_owner_recoveries") != 0:
            dry_issues.append(
                "speaker_owner_recoveries="
                f"{dry_after.get('speaker_owner_recoveries')}"
            )
        if bool(dry_delta.get("counter_reset_detected", False)):
            dry_issues.append("gate SAY real invalido porque os contadores resetaram")
        if dry_received < required_chunks:
            dry_issues.append(
                "SAY real insuficiente no dry-run: "
                f"{dry_received} < {required_chunks}"
            )
        if not bool(dry_after.get("speaker_owner_active", False)):
            dry_issues.append("speaker owner nao ficou ativo durante SAY real")
        if dry_non_silence_frames == 0:
            dry_issues.append("speaker owner nao observou frame nao silencioso")
    elif not dry_delta_error and bool(dry_armed.get("ok", False)):
        dry_issues.append("dry-run delta nao foi coletado")

    dry_candidate = not dry_issues
    dry_run_gate = {
        "ok": dry_candidate,
        "status": "fail" if dry_issues else "warn" if dry_warnings else "ok",
        "owner_readiness_gate": True,
        "issues": dry_issues,
        "warnings": dry_warnings,
        "armed": dry_armed,
        "delta": dry_delta,
        "delta_error": dry_delta_error,
        "ready": bool(dry_after.get("speaker_owner_handoff_ready", False)),
        "block_reason": dry_after.get("speaker_owner_block_reason"),
        "active": bool(dry_after.get("speaker_owner_active", False)),
        "require_say": True,
        "min_say_chunks": min_say_chunks,
        "required_say_chunks": max(1, min_say_chunks),
        "counter_reset_detected": bool(dry_delta.get("counter_reset_detected", False)),
        "real_owner_candidate": dry_candidate,
        "real_owner_candidate_status": (
            "ready_with_warnings" if dry_candidate and dry_warnings
            else "ready" if dry_candidate
            else "blocked"
        ),
        "real_owner_candidate_blockers": dry_issues,
        "frames": dry_after.get("speaker_owner_frames"),
        "samples": dry_after.get("speaker_owner_samples"),
        "silence_frames": dry_after.get("speaker_owner_silence_frames"),
        "non_silence_frames": max(
            0,
            _playback_v2_int(dry_after, "speaker_owner_frames")
            - _playback_v2_int(dry_after, "speaker_owner_silence_frames"),
        ),
        "say_chunks_received_delta": dry_deltas.get("say_chunks_received"),
        "say_chunks_played_delta": dry_deltas.get("say_chunks_played"),
    }

    for item in dry_issues:
        issues.append(str(item))
    for item in dry_warnings:
        warnings.append(str(item))

    try:
        if dry_candidate:
            real_armed = client.audio_playback_v2_speaker_owner_real_arm()
            if not bool(real_armed.get("ok", False)):
                issues.append("speaker-owner-real-arm retornou ok=false")
            elif not bool(real_armed.get("speaker_owner_real_armed", False)):
                issues.append("speaker_owner_real_armed=false apos real-arm")
            else:
                real_delta = _run_playback_v2_delta(
                    client,
                    no_prompt=no_prompt,
                    wait_s=wait_s,
                )
    except Exception as exc:  # pragma: no cover - defensive CLI reporting
        real_delta_error = str(exc)
        issues.append(f"real-window gate falhou: {real_delta_error}")
    finally:
        try:
            disarmed = client.audio_playback_v2_speaker_owner_disarm()
        except Exception as exc:  # pragma: no cover - defensive rollback reporting
            disarmed = {"ok": False, "error": str(exc)}

    if not bool(disarmed.get("ok", False)):
        issues.append("speaker-owner-disarm retornou ok=false")

    real_after = real_delta.get("after")
    real_deltas = real_delta.get("deltas")
    if not isinstance(real_after, dict):
        real_after = {}
    if not isinstance(real_deltas, dict):
        real_deltas = {}

    if real_delta:
        if not bool(real_delta.get("ok", False)):
            issues.append("real-window delta retornou ok=false")
        for item in real_delta.get("issues", []) or []:
            issues.append(str(item))
        for item in real_delta.get("warnings", []) or []:
            warnings.append(str(item))

        real_received = _playback_v2_int(real_deltas, "say_chunks_received")
        real_played = _playback_v2_int(real_deltas, "say_chunks_played")
        real_write_frames = _playback_v2_int(real_after, "speaker_owner_real_write_frames")
        real_write_failures = _playback_v2_int(real_after, "speaker_owner_real_write_failures")

        if bool(real_after.get("speaker_owner_real_armed", False)):
            issues.append("janela real permaneceu armada apos SAY")
        if not bool(real_after.get("speaker_owner_real_window_completed", False)):
            issues.append("janela real nao marcou completed=true")
        if _playback_v2_int(real_after, "speaker_owner_real_auto_disarm_count") < 1:
            issues.append("auto_disarm_count nao incrementou")
        required_chunks = max(1, min_say_chunks)
        if real_received < required_chunks:
            issues.append(
                "SAY real insuficiente na janela real: "
                f"{real_received} < {required_chunks}"
            )
        if real_received != real_played:
            issues.append(f"SAY real received/playback divergiu: {real_received}/{real_played}")
        if real_write_frames <= 0:
            issues.append("speaker_owner_real_write_frames=0")
        if real_write_frames != real_received:
            issues.append(
                "real writes nao acompanharam chunks SAY: "
                f"{real_write_frames} != {real_received}"
            )
        if real_write_failures != 0:
            issues.append(
                "speaker_owner_real_write_failures="
                f"{real_after.get('speaker_owner_real_write_failures')}"
            )

    ok = not issues
    status = "fail" if issues else "warn" if warnings else "ok"
    return {
        "ok": ok,
        "status": status,
        "owner_real_window_gate": True,
        "issues": issues,
        "warnings": warnings,
        "dry_run_gate": dry_run_gate,
        "real_armed": real_armed,
        "real_delta": real_delta,
        "real_delta_error": real_delta_error,
        "disarmed": disarmed,
        "real_window_completed": bool(real_after.get("speaker_owner_real_window_completed", False)),
        "real_auto_disarm_count": real_after.get("speaker_owner_real_auto_disarm_count"),
        "real_write_frames": real_after.get("speaker_owner_real_write_frames"),
        "real_write_samples": real_after.get("speaker_owner_real_write_samples"),
        "real_write_failures": real_after.get("speaker_owner_real_write_failures"),
        "real_say_chunks_received_delta": real_deltas.get("say_chunks_received"),
        "real_say_chunks_played_delta": real_deltas.get("say_chunks_played"),
        "real_say_chunks_dropped_delta": real_deltas.get("say_chunks_dropped"),
    }


def _format_playback_v2_status(payload: dict[str, object]) -> str:
    if payload.get("owner_real_window_gate"):
        issues = payload.get("issues")
        warnings = payload.get("warnings")
        if not isinstance(issues, list):
            issues = []
        if not isinstance(warnings, list):
            warnings = []
        return "\n".join([
            "Playback v2 speaker owner real-window gate:",
            f"- ok: {payload.get('ok')}",
            f"- status: {payload.get('status')}",
            f"- real_window_completed: {payload.get('real_window_completed')}",
            f"- real_auto_disarm_count: {payload.get('real_auto_disarm_count')}",
            f"- real_write: {payload.get('real_write_frames')}/"
            f"{payload.get('real_write_samples')} samples "
            f"failures={payload.get('real_write_failures')}",
            f"- real_delta.received: {payload.get('real_say_chunks_received_delta')}",
            f"- real_delta.played: {payload.get('real_say_chunks_played_delta')}",
            f"- real_delta.dropped: {payload.get('real_say_chunks_dropped_delta')}",
            f"- real_delta_error: {payload.get('real_delta_error') or 'nenhum'}",
            f"- disarmed.ok: {(payload.get('disarmed') or {}).get('ok')}",
            f"- issues: {', '.join(str(item) for item in issues) if issues else 'nenhuma'}",
            f"- warnings: {', '.join(str(item) for item in warnings) if warnings else 'nenhum'}",
        ])
    if payload.get("owner_readiness_gate"):
        issues = payload.get("issues")
        warnings = payload.get("warnings")
        if not isinstance(issues, list):
            issues = []
        if not isinstance(warnings, list):
            warnings = []
        return "\n".join([
            "Playback v2 speaker owner gate:",
            f"- ok: {payload.get('ok')}",
            f"- status: {payload.get('status')}",
            f"- ready: {payload.get('ready')}",
            f"- active: {payload.get('active')}",
            f"- block_reason: {payload.get('block_reason')}",
            f"- require_say: {payload.get('require_say')} "
            f"required_chunks={payload.get('required_say_chunks')}",
            f"- counter_reset_detected: {payload.get('counter_reset_detected')}",
            f"- delta_error: {payload.get('delta_error') or 'nenhum'}",
            f"- real_owner_candidate: {payload.get('real_owner_candidate')} "
            f"status={payload.get('real_owner_candidate_status')}",
            f"- real_owner_candidate_blockers: "
            f"{', '.join(str(item) for item in payload.get('real_owner_candidate_blockers', []) or []) or 'nenhum'}",
            f"- frames/samples: {payload.get('frames')}/{payload.get('samples')}",
            f"- non_silence_frames: {payload.get('non_silence_frames')}",
            f"- delta.received: {payload.get('say_chunks_received_delta')}",
            f"- delta.played: {payload.get('say_chunks_played_delta')}",
            f"- disarmed.ok: {(payload.get('disarmed') or {}).get('ok')}",
            f"- issues: {', '.join(str(item) for item in issues) if issues else 'nenhuma'}",
            f"- warnings: {', '.join(str(item) for item in warnings) if warnings else 'nenhum'}",
        ])
    if "deltas" in payload:
        after = payload.get("after")
        deltas = payload.get("deltas")
        io_deltas = payload.get("audio_io_deltas")
        codec_deltas = payload.get("codec_deltas")
        codec_health = payload.get("codec_health")
        issues = payload.get("issues")
        warnings = payload.get("warnings")
        if not isinstance(after, dict):
            after = {}
        if not isinstance(deltas, dict):
            deltas = {}
        if not isinstance(io_deltas, dict):
            io_deltas = {}
        if not isinstance(codec_deltas, dict):
            codec_deltas = {}
        if not isinstance(codec_health, dict):
            codec_health = {}
        if not isinstance(issues, list):
            issues = []
        if not isinstance(warnings, list):
            warnings = []
        return "\n".join([
            "Playback v2 delta:",
            f"- ok: {payload.get('ok')}",
            f"- status: {payload.get('status')}",
            f"- issues: {', '.join(str(item) for item in issues) if issues else 'nenhuma'}",
            f"- warnings: {', '.join(str(item) for item in warnings) if warnings else 'nenhum'}",
            f"- queue_empty: {payload.get('queue_empty')}",
            f"- normal_path_clean: {payload.get('normal_path_clean')}",
            f"- delta.received: {deltas.get('say_chunks_received')}",
            f"- delta.played: {deltas.get('say_chunks_played')}",
            f"- delta.dropped: {deltas.get('say_chunks_dropped')}",
            f"- delta.dropped_listening: {deltas.get('say_chunks_dropped_listening')}",
            f"- delta.cancelled: {deltas.get('say_chunks_cancelled')}",
            f"- delta.speaker_write_failures: {deltas.get('speaker_write_failures')}",
            f"- delta.speaker_commit_failures: {deltas.get('speaker_commit_failures')}",
            f"- delta.io_dropped_frames: {io_deltas.get('dropped_frames')}",
            f"- delta.io_i2s_recoveries: {io_deltas.get('i2s_recoveries')}",
            f"- delta.codec_packet_drops: {codec_deltas.get('packet_drops')}",
            f"- delta.codec_egress_drops: {codec_deltas.get('opus_egress_packet_drops')}",
            f"- codec_health: healthy={codec_health.get('healthy')} "
            f"status={codec_health.get('status')}",
            f"- after.queue_count: {after.get('say_queue_count')}",
            f"- after.error: {after.get('error')}",
        ])
    return "\n".join([
        "Playback v2:",
        f"- ok: {payload.get('ok')}",
        f"- observer: {payload.get('bridge_say_observer')}",
        f"- queue_owner: {payload.get('bridge_say_queue_owner')}",
        f"- speaker_owner: requested={payload.get('speaker_owner_requested')} "
        f"ready={payload.get('speaker_owner_ready')} "
        f"active={payload.get('speaker_owner_active')}",
        f"- speaker_owner_readiness: dry_run={payload.get('speaker_owner_dry_run_enabled')} "
        f"candidate={payload.get('speaker_owner_candidate')} "
        f"handoff_ready={payload.get('speaker_owner_handoff_ready')} "
        f"block={payload.get('speaker_owner_block_reason')}",
        f"- speaker_owner_real: requested={payload.get('speaker_owner_real_requested')} "
        f"armed={payload.get('speaker_owner_real_armed')} "
        f"block={payload.get('speaker_owner_real_block_reason')}",
        f"- speaker_owner_real_window: active={payload.get('speaker_owner_real_window_active')} "
        f"completed={payload.get('speaker_owner_real_window_completed')} "
        f"auto_disarm={payload.get('speaker_owner_real_auto_disarm_count')}",
        f"- speaker_owner_real_write: {payload.get('speaker_owner_real_write_frames')}/"
        f"{payload.get('speaker_owner_real_write_samples')} samples "
        f"failures={payload.get('speaker_owner_real_write_failures')} "
        f"result={payload.get('speaker_owner_real_last_result')}",
        f"- speaker_owner_frames: {payload.get('speaker_owner_frames')}/"
        f"{payload.get('speaker_owner_samples')} samples "
        f"silence={payload.get('speaker_owner_silence_frames')} "
        f"failures={payload.get('speaker_owner_failures')} "
        f"recoveries={payload.get('speaker_owner_recoveries')} "
        f"last={payload.get('speaker_owner_last_samples')} "
        f"result={payload.get('speaker_owner_last_result')}",
        f"- speaker_prepared: {payload.get('speaker_frames_prepared')}/"
        f"{payload.get('speaker_samples_prepared')} samples "
        f"last={payload.get('speaker_last_samples')} "
        f"volume={payload.get('speaker_last_volume')}",
        f"- speaker_committed: {payload.get('speaker_frames_committed')}/"
        f"{payload.get('speaker_samples_committed')} samples "
        f"failures={payload.get('speaker_commit_failures')} "
        f"last={payload.get('speaker_last_commit_samples')} "
        f"result={payload.get('speaker_last_commit_result')}",
        f"- speaker_write: {payload.get('speaker_write_requests')}/"
        f"{payload.get('speaker_write_samples')} samples "
        f"failures={payload.get('speaker_write_failures')} "
        f"last={payload.get('speaker_last_write_samples')} "
        f"result={payload.get('speaker_last_write_result')}",
        f"- speaker_empty: polls={payload.get('speaker_empty_polls')} "
        f"ms={payload.get('speaker_empty_ms')} "
        f"ends={payload.get('speaker_idle_end_count')}",
        f"- playing: {payload.get('playing')}",
        f"- queue: {payload.get('say_queue_count')}/{payload.get('say_queue_depth')} "
        f"high={payload.get('say_queue_high_watermark')} "
        f"wait_ms={payload.get('say_accept_wait_ms')}",
        f"- received/played: {payload.get('say_chunks_received')}/"
        f"{payload.get('say_chunks_played')}",
        f"- dropped/listening: {payload.get('say_chunks_dropped')}/"
        f"{payload.get('say_chunks_dropped_listening')}",
        f"- queue_full/recovered/dropped: {payload.get('say_chunks_queue_full')}/"
        f"{payload.get('say_chunks_queue_wait_recovered')}/"
        f"{payload.get('say_chunks_dropped_queue_full')}",
        f"- cancelled/cancel_count: {payload.get('say_chunks_cancelled')}/"
        f"{payload.get('say_cancel_count')}",
        f"- error: {payload.get('error')}",
    ])


def _format_capture_v2_status(payload: dict[str, object]) -> str:
    if "after" in payload:
        after = payload.get("after")
        disabled = payload.get("disabled")
        if not isinstance(after, dict):
            after = {}
        if not isinstance(disabled, dict):
            disabled = {}
        return "\n".join([
            "Capture v2 live:",
            f"- ok: {payload.get('ok')}",
            f"- after.real_capture_enabled: {after.get('real_capture_enabled')}",
            f"- after.bridge_tx_handoff_enabled: {after.get('bridge_tx_handoff_enabled')}",
            f"- after.real_capture: {after.get('real_capture')}",
            f"- after.state: {after.get('state')}",
            f"- after.end_reason: {after.get('end_reason')}",
            f"- after.bridge_tx_owner: {after.get('bridge_tx_owner')}",
            f"- after.legacy_audio_service_tx_owner: {after.get('legacy_audio_service_tx_owner')}",
            f"- after.bridge_tx_candidate: {after.get('bridge_tx_candidate')}",
            f"- after.bridge_tx_handoff_ready: {after.get('bridge_tx_handoff_ready')}",
            f"- after.handoff_block_reason: {after.get('handoff_block_reason')}",
            f"- after.voice_start_sent: {after.get('voice_start_sent')}",
            f"- after.voice_audio_sent: {after.get('voice_audio_sent')}",
            f"- after.voice_end_sent: {after.get('voice_end_sent')}",
            f"- after.shadow_voice_start_sent: {after.get('shadow_voice_start_sent')}",
            f"- after.shadow_voice_end_sent: {after.get('shadow_voice_end_sent')}",
            f"- after.shadow_audio_chunks: {after.get('shadow_audio_chunks')}",
            f"- after.shadow_audio_samples: {after.get('shadow_audio_samples')}",
            f"- after.shadow_audio_dropped_chunks: {after.get('shadow_audio_dropped_chunks')}",
            f"- after.speech_frames: {after.get('speech_frames')}",
            f"- after.captured_samples: {after.get('captured_samples')}",
            f"- after.dropped_frames: {after.get('dropped_frames')}",
            f"- disabled.ok: {disabled.get('ok')}",
        ])
    return "\n".join([
        "Capture v2:",
        f"- ok: {payload.get('ok')}",
        f"- real_capture_enabled: {payload.get('real_capture_enabled')}",
        f"- bridge_tx_handoff_enabled: {payload.get('bridge_tx_handoff_enabled')}",
        f"- real_capture: {payload.get('real_capture')}",
        f"- session_active: {payload.get('session_active')}",
        f"- state: {payload.get('state')}",
        f"- source: {payload.get('source')}",
        f"- end_reason: {payload.get('end_reason')}",
        f"- bridge_tx_owner: {payload.get('bridge_tx_owner')}",
        f"- legacy_audio_service_tx_owner: {payload.get('legacy_audio_service_tx_owner')}",
        f"- bridge_tx_candidate: {payload.get('bridge_tx_candidate')}",
        f"- bridge_tx_handoff_ready: {payload.get('bridge_tx_handoff_ready')}",
        f"- handoff_block_reason: {payload.get('handoff_block_reason')}",
        f"- voice_start_sent: {payload.get('voice_start_sent')}",
        f"- voice_audio_sent: {payload.get('voice_audio_sent')}",
        f"- voice_end_sent: {payload.get('voice_end_sent')}",
        f"- shadow_voice_start_sent: {payload.get('shadow_voice_start_sent')}",
        f"- shadow_voice_end_sent: {payload.get('shadow_voice_end_sent')}",
        f"- shadow_audio_chunks: {payload.get('shadow_audio_chunks')}",
        f"- shadow_audio_samples: {payload.get('shadow_audio_samples')}",
        f"- shadow_audio_dropped_chunks: {payload.get('shadow_audio_dropped_chunks')}",
        f"- speech_frames: {payload.get('speech_frames')}",
        f"- captured_samples: {payload.get('captured_samples')}",
        f"- dropped_frames: {payload.get('dropped_frames')}",
        f"- error: {payload.get('error')}",
    ])


def _run_capture_v2_live(client, *, no_prompt: bool) -> dict[str, object]:
    before = client.audio_capture_v2_status()
    enabled: dict[str, object] = {}
    after: dict[str, object] = {}
    disabled: dict[str, object] = {}
    try:
        enabled = client.set_voice_audio_v2_capture_enabled(True)
        if not no_prompt:
            input(
                "Flag capture v2 ligada. Acione o wake/fale uma frase real e "
                "pressione Enter quando o turno terminar: "
            )
        after = client.audio_capture_v2_status()
    finally:
        disabled = client.set_voice_audio_v2_capture_enabled(False)
    return {
        "ok": bool(enabled.get("ok") and after.get("ok") and disabled.get("ok")),
        "before": before,
        "enabled": enabled,
        "after": after,
        "disabled": disabled,
    }


def _resolve_firmware_url(args: argparse.Namespace) -> str:
    firmware_url = getattr(args, "firmware_url", "") or os.environ.get(
        "NOISEBOT_ROBOT_HTTP_URL",
        "",
    )
    if not firmware_url:
        host = args.host or os.environ.get("NOISEBOT_HOST", "")
        if host:
            firmware_url = f"http://{host}"
    return firmware_url.rstrip("/")


def _parse_bitrates(raw: str) -> list[int]:
    bitrates: list[int] = []
    for item in raw.split(","):
        value = item.strip()
        if value:
            bitrates.append(int(value))
    if not bitrates:
        raise SystemExit("--bitrates precisa ter pelo menos um valor")
    return bitrates


def run_service_command(args: argparse.Namespace) -> None:
    """Run server-owned service commands."""
    from .internal.service.manager import get_manager

    manager = get_manager()
    try:
        if args.service_command == "install":
            manager.install()
        elif args.service_command == "uninstall":
            manager.uninstall()
        elif args.service_command == "status":
            print(manager.status())
        elif args.service_command == "start":
            manager.start()
            print("Servico iniciado.")
        elif args.service_command == "stop":
            manager.stop()
            print("Servico parado.")
        else:
            raise SystemExit(2)
    except Exception as exc:
        print(f"Erro: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


def main(argv: Sequence[str] | None = None) -> None:
    args = parse_args(argv)

    if args.command == "service":
        run_service_command(args)
        return
    if args.command == "debug":
        run_debug_command(args)
        return

    apply_env_overrides(args)
    env_file = find_env_file(args.env)
    config = load_config(env_file)
    run_server(config, log_file=args.log_file)
