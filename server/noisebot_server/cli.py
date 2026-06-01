"""NoiseBot server command-line interface."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
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

    aec_live = debug_sub.add_parser("aec-live")
    aec_live.add_argument("--firmware-url", default="")
    aec_live.add_argument("--output", help="Arquivo Markdown/JSON de saida")
    aec_live.add_argument("--json", action="store_true", help="Emitir JSON")

    capture_v2 = debug_sub.add_parser("capture-v2")
    capture_v2.add_argument(
        "action",
        choices=["status", "replay", "cancel", "enable", "disable", "live"],
        nargs="?",
        default="status",
    )
    capture_v2.add_argument("--firmware-url", default="")
    capture_v2.add_argument("--speech-ms", type=int, default=640)
    capture_v2.add_argument("--silence-ms", type=int, default=900)
    capture_v2.add_argument("--source", default="debug")
    capture_v2.add_argument("--no-prompt", action="store_true", help="Nao aguardar Enter no modo live")
    capture_v2.add_argument("--json", action="store_true", help="Emitir JSON")

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
    from .internal.debug.manual import run_fake_firmware_debug, run_transcript_debug

    if args.debug_command == "transcript":
        raise SystemExit(asyncio.run(run_transcript_debug(args.text, args.turn_id)))
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
            f"- after.real_capture: {after.get('real_capture')}",
            f"- after.state: {after.get('state')}",
            f"- after.end_reason: {after.get('end_reason')}",
            f"- after.bridge_tx_owner: {after.get('bridge_tx_owner')}",
            f"- after.legacy_audio_service_tx_owner: {after.get('legacy_audio_service_tx_owner')}",
            f"- after.voice_start_sent: {after.get('voice_start_sent')}",
            f"- after.voice_audio_sent: {after.get('voice_audio_sent')}",
            f"- after.voice_end_sent: {after.get('voice_end_sent')}",
            f"- after.speech_frames: {after.get('speech_frames')}",
            f"- after.captured_samples: {after.get('captured_samples')}",
            f"- after.dropped_frames: {after.get('dropped_frames')}",
            f"- disabled.ok: {disabled.get('ok')}",
        ])
    return "\n".join([
        "Capture v2:",
        f"- ok: {payload.get('ok')}",
        f"- real_capture_enabled: {payload.get('real_capture_enabled')}",
        f"- real_capture: {payload.get('real_capture')}",
        f"- session_active: {payload.get('session_active')}",
        f"- state: {payload.get('state')}",
        f"- source: {payload.get('source')}",
        f"- end_reason: {payload.get('end_reason')}",
        f"- bridge_tx_owner: {payload.get('bridge_tx_owner')}",
        f"- legacy_audio_service_tx_owner: {payload.get('legacy_audio_service_tx_owner')}",
        f"- voice_start_sent: {payload.get('voice_start_sent')}",
        f"- voice_audio_sent: {payload.get('voice_audio_sent')}",
        f"- voice_end_sent: {payload.get('voice_end_sent')}",
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
