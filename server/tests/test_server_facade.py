from __future__ import annotations

import asyncio
import importlib
import io
import json
import logging
import math
import sys
import struct
from pathlib import Path
from urllib.error import HTTPError

import pytest


def _ensure_bridgev2_path() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    bridge_v2_path = repo_root / "bridge_v2"
    bridge_v2_str = str(bridge_v2_path)
    if bridge_v2_path.exists() and bridge_v2_str not in sys.path:
        sys.path.insert(0, bridge_v2_str)


def _make_server_config(
    *,
    host: str | None = None,
    port: int = 9000,
    uart: str | None = None,
    dry_run: bool = True,
    piper_model: str = "",
    max_utterance_samples: int = 160000,
    default_codec: str = "pcm16",
):
    config_module = importlib.import_module("noisebot_server.config")

    return config_module.NoiseBotServerConfig(
        transport=config_module.TransportConfig(
            host=host,
            port=port,
            uart=uart,
            baudrate=1000000,
        ),
        llm=config_module.LlmConfig(
            provider=config_module.LlmProvider.NONE,
            model="none",
            timeout_s=10.0,
            temperature=0.7,
            max_output_tokens=256,
            max_reply_chars=180,
            ollama_base_url="http://127.0.0.1:11434",
            ollama_think=False,
            openai_key_configured=False,
            gemini_key_configured=False,
        ),
        pipeline_mode=config_module.PipelineMode.LOCAL_ONLY,
        stt=config_module.SttConfig(
            model="small",
            backend="faster",
            device="cpu",
            compute_type="int8",
        ),
        tts=config_module.TtsConfig(
            piper_executable="piper",
            piper_model=piper_model,
            cache_size=64,
            sample_rate=16000,
            target_peak=12000,
        ),
        audio=config_module.AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            default_codec=default_codec,
            min_transcribe_rms=140.0,
            min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_utterance_samples=max_utterance_samples,
            max_no_speech_prob=0.75,
            min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=config_module.ReconnectConfig(
            delay_s=0.05,
            max_delay_s=0.2,
            connect_timeout_s=2.0,
        ),
        ops=config_module.OpsConfig(
            port=8765,
            token_configured=False,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=dry_run,
        replay_path=None,
    )


def test_bridgev2_reference_path_allows_application_import() -> None:
    _ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")

    assert hasattr(app_module, "NoiseBotServer")


def test_stt_repetition_loop_guard_detects_whisper_hallucination() -> None:
    stt = importlib.import_module("noisebot_server.internal.agent.stt")

    assert stt._looks_like_repetition_loop(
        "o que e que e que e que e que e que e que e que e que e que e"
    )
    assert not stt._looks_like_repetition_loop("acenda a luz da mesa por favor")


def test_llm_prompt_includes_recent_replies_to_avoid_repetition() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    messages = llm.build_messages(
        "Me conte uma piada.",
        {"recent_replies": ["Por que o livro foi ao médico? Porque tinha muitos problemas de capa."]},
    )

    system = messages[0]["content"]
    assert "Respostas recentes a evitar repetir" in system
    assert "livro foi ao médico" in system
    assert "nunca repita" in system


def test_llm_language_guard_replaces_foreign_script_reply() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "绿是程序员的最爱，因为蓝（绿）！",
        "Me conte uma piada.",
    )

    assert replaced
    assert "Por que" in reply
    assert "绿" not in reply


def test_llm_language_guard_replaces_english_reply() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "Did you know that penguins can't fly? They're amazing swimmers instead!",
        "Me diga uma curiosidade curta.",
    )

    assert replaced
    assert "Curiosidade:" in reply
    assert "penguins" not in reply


def test_llm_language_guard_replaces_english_curiosity_with_fact() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    reply, replaced = llm.enforce_pt_br_reply(
        "Did you know that penguins can't fly? They're amazing swimmers instead!",
        "Me conte uma curiosidade.",
    )

    assert replaced
    assert "Curiosidade:" in reply
    assert "idioma errado" not in reply
    assert "penguins" not in reply


def test_server_entrypoint_exposes_server_cli() -> None:
    _ensure_bridgev2_path()

    cli_module = importlib.import_module("noisebot_server.__main__")

    assert callable(cli_module.main)


def test_server_cli_parses_runtime_flags() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host", "192.168.1.30",
        "--port", "9000",
        "--pipeline", "local_only",
        "--llm", "ollama",
        "--model", "qwen2.5:7b",
        "--audio-codec", "opus-v2",
        "--log-file", "stderr",
    ])

    assert args.command is None
    assert args.host == "192.168.1.30"
    assert args.port == 9000
    assert args.pipeline == "local_only"
    assert args.llm == "ollama"
    assert args.model == "qwen2.5:7b"
    assert args.audio_codec == "opus-v2"
    assert args.log_file == "stderr"


def test_server_cli_applies_env_overrides(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")

    for key in (
        "NOISEBOT_HOST",
        "NOISEBOT_PORT",
        "NOISEBOT_DRY_RUN",
        "NOISEBOT_PIPELINE_MODE",
        "NOISEBOT_LLM_PROVIDER",
        "NOISEBOT_LLM_MODEL",
        "NOISEBOT_AUDIO_DEFAULT_CODEC",
    ):
        monkeypatch.delenv(key, raising=False)

    args = cli.parse_args([
        "--host", "10.0.0.2",
        "--port", "9010",
        "--dry-run",
        "--pipeline", "local_only",
        "--llm", "none",
        "--model", "none",
        "--audio-codec", "opus-v2",
    ])

    cli.apply_env_overrides(args)

    import os

    assert os.environ["NOISEBOT_HOST"] == "10.0.0.2"
    assert os.environ["NOISEBOT_PORT"] == "9010"
    assert os.environ["NOISEBOT_DRY_RUN"] == "true"
    assert os.environ["NOISEBOT_PIPELINE_MODE"] == "local_only"
    assert os.environ["NOISEBOT_LLM_PROVIDER"] == "none"
    assert os.environ["NOISEBOT_LLM_MODEL"] == "none"
    assert os.environ["NOISEBOT_AUDIO_DEFAULT_CODEC"] == "opus-v2"


def test_server_config_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_config = importlib.import_module("noisebot_server.config")
    bridge_config = importlib.import_module("bridgev2.config")

    assert hasattr(server_config, "NoiseBotServerConfig")
    assert not hasattr(server_config, "BridgeV2Config")
    assert server_config.LlmProvider.OLLAMA.value == bridge_config.LlmProvider.OLLAMA.value
    assert server_config.PipelineMode.LOCAL_ONLY.value == (
        bridge_config.PipelineMode.LOCAL_ONLY.value
    )


def test_server_config_loads_audio_default_codec(monkeypatch) -> None:
    config_module = importlib.import_module("noisebot_server.config")

    monkeypatch.setenv("NOISEBOT_AUDIO_DEFAULT_CODEC", "opus-v2")

    config = config_module.load_config()

    assert config.audio.default_codec == "opus-v2"
    assert config.safe_dict()["audio"]["default_codec"] == "opus-v2"


def test_server_config_invalid_audio_default_codec_falls_back(monkeypatch) -> None:
    config_module = importlib.import_module("noisebot_server.config")

    monkeypatch.setenv("NOISEBOT_AUDIO_DEFAULT_CODEC", "banana")

    config = config_module.load_config()

    assert config.audio.default_codec == "pcm16"


def test_server_cli_runs_debug_transcript_without_bridge_entrypoint(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    manual = importlib.import_module("noisebot_server.internal.debug.manual")

    calls: dict[str, object] = {}

    async def fake_run_transcript_debug(text: str, turn_id: int = 1) -> int:
        calls["text"] = text
        calls["turn_id"] = turn_id
        return 7

    monkeypatch.setattr(manual, "run_transcript_debug", fake_run_transcript_debug)

    try:
        cli.main(["debug", "transcript", "oi noise", "--turn-id", "42"])
    except SystemExit as exc:
        assert exc.code == 7
    else:
        raise AssertionError("debug command must exit with helper return code")

    assert calls == {"text": "oi noise", "turn_id": 42}


def test_server_cli_parses_audio_report_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "debug",
        "audio-report",
        "voice_samples",
        "--output",
        "report.md",
    ])

    assert args.command == "debug"
    assert args.debug_command == "audio-report"
    assert args.path == "voice_samples"
    assert args.output == "report.md"


def test_server_cli_parses_afe_ab_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "debug",
        "afe-ab",
        "me conte uma piada",
        "--firmware-url",
        "http://192.168.1.30",
        "--repeat",
        "2",
        "--output",
        "afe.md",
    ])

    assert args.command == "debug"
    assert args.debug_command == "afe-ab"
    assert args.phrase == "me conte uma piada"
    assert args.firmware_url == "http://192.168.1.30"
    assert args.repeat == 2
    assert args.output == "afe.md"


def test_server_cli_parses_opus_live_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "opus-live",
        "me diga uma curiosidade",
        "--server-url",
        "http://127.0.0.1:8765",
        "--timeout-s",
        "12",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "opus-live"
    assert args.host == "192.168.1.30"
    assert args.phrase == "me diga uma curiosidade"
    assert args.server_url == "http://127.0.0.1:8765"
    assert args.timeout_s == 12.0
    assert args.json


def test_server_cli_parses_opus_quality_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "debug",
        "opus-quality",
        "voice_samples",
        "--bitrates",
        "16000,24000",
        "--output",
        "opus.md",
    ])

    assert args.command == "debug"
    assert args.debug_command == "opus-quality"
    assert args.path == "voice_samples"
    assert args.bitrates == "16000,24000"
    assert args.output == "opus.md"


def test_server_cli_runs_opus_live_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    opus_live = importlib.import_module("noisebot_server.internal.ops.opus_live")

    calls: dict[str, object] = {}

    def fake_run_opus_live_trial(**kwargs):
        calls.update(kwargs)
        return opus_live.OpusLiveTrial(
            phrase=kwargs["phrase"],
            ok=True,
            turn_id=77,
            outcome="llm",
            transcript_quality="GOOD",
            transcript="me diga uma curiosidade",
            discard_reason="",
            total_samples=32000,
            stt_ms=1234.0,
            duration_ms=4567.0,
            packets_drained=12,
            packet_drops=0,
            encoded_bytes=1776,
            enable_ok=True,
            disable_ok=True,
            server_opus_confirmed=True,
        )

    monkeypatch.setattr(opus_live, "run_opus_live_trial", fake_run_opus_live_trial)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "opus-live",
        "me diga uma curiosidade",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert calls["firmware_url"] == "http://192.168.1.30"
    assert calls["phrase"] == "me diga uma curiosidade"


def test_server_cli_parses_codec_ab_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-ab",
        "me diga uma curiosidade",
        "que horas sao",
        "--repeat",
        "2",
        "--server-url",
        "http://127.0.0.1:8765",
        "--timeout-s",
        "12",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-ab"
    assert args.host == "192.168.1.30"
    assert args.phrases == ["me diga uma curiosidade", "que horas sao"]
    assert args.repeat == 2
    assert args.server_url == "http://127.0.0.1:8765"
    assert args.timeout_s == 12.0
    assert args.json


def test_server_cli_runs_codec_ab_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    codec_ab = importlib.import_module("noisebot_server.internal.ops.codec_ab")

    calls: dict[str, object] = {}

    def fake_run_codec_ab_trials(**kwargs):
        calls.update(kwargs)
        return [
            codec_ab.CodecAbTrial(
                codec="pcm16",
                phrase="me diga uma curiosidade",
                ok=True,
                turn_id=10,
                outcome="llm",
                transcript_quality="good",
                transcript="me diga uma curiosidade",
                discard_reason="",
                total_samples=32000,
                stt_ms=1000.0,
                duration_ms=3000.0,
                packets_drained=0,
                packet_drops=0,
                encoded_bytes=0,
                server_codec_confirmed=True,
                transcript_similarity=1.0,
                transcript_match=True,
            ),
            codec_ab.CodecAbTrial(
                codec="opus",
                phrase="me diga uma curiosidade",
                ok=True,
                turn_id=11,
                outcome="llm",
                transcript_quality="good",
                transcript="me diga uma curiosidade",
                discard_reason="",
                total_samples=32000,
                stt_ms=1000.0,
                duration_ms=3000.0,
                packets_drained=34,
                packet_drops=0,
                encoded_bytes=4896,
                server_codec_confirmed=True,
                transcript_similarity=1.0,
                transcript_match=True,
            ),
        ]

    monkeypatch.setattr(codec_ab, "run_codec_ab_trials", fake_run_codec_ab_trials)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-ab",
        "me diga uma curiosidade",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"codec": "opus"' in captured.out
    assert calls["firmware_url"] == "http://192.168.1.30"
    assert calls["phrases"] == ["me diga uma curiosidade"]


def test_server_codec_ab_summary_keeps_opus_opt_in_on_drops() -> None:
    codec_ab = importlib.import_module("noisebot_server.internal.ops.codec_ab")

    trials = [
        codec_ab.CodecAbTrial(
            codec="pcm16",
            phrase="x",
            ok=True,
            turn_id=1,
            outcome="llm",
            transcript_quality="good",
            transcript="x",
            discard_reason="",
            total_samples=16000,
            stt_ms=1000.0,
            duration_ms=2000.0,
            packets_drained=0,
            packet_drops=0,
            encoded_bytes=0,
            server_codec_confirmed=True,
            transcript_similarity=1.0,
            transcript_match=True,
        ),
        codec_ab.CodecAbTrial(
            codec="opus",
            phrase="x",
            ok=False,
            turn_id=2,
            outcome="llm",
            transcript_quality="good",
            transcript="x",
            discard_reason="",
            total_samples=16000,
            stt_ms=1000.0,
            duration_ms=2000.0,
            packets_drained=10,
            packet_drops=1,
            encoded_bytes=1400,
            server_codec_confirmed=True,
            transcript_similarity=1.0,
            transcript_match=True,
        ),
    ]

    summary = "\n".join(codec_ab.summarize_codec_ab(trials))

    assert "Opus drops: 1" in summary
    assert "Opus permanece opt-in" in summary


def test_server_codec_ab_confirms_opus_from_packet_counters() -> None:
    codec_ab = importlib.import_module("noisebot_server.internal.ops.codec_ab")

    trial = codec_ab._trial_from_payload(
        codec="opus",
        phrase="me diga uma curiosidade",
        previous_turn_id=10,
        metrics={
            "last_voice_session": {
                "turn_id": 11,
                "outcome": "llm",
                "transcript_quality": "good",
                "transcript": "me diga uma curiosidade",
                "total_samples": 32000,
            },
        },
        worker_before={
            "opus_egress_packets_drained": 10,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_bytes_total": 1000,
        },
        worker_after={
            "opus_egress_packets_drained": 44,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_bytes_total": 5896,
        },
        server_codec_confirmed=False,
    )

    assert trial.ok
    assert trial.server_codec_confirmed
    assert trial.packets_drained == 34
    assert trial.encoded_bytes == 4896


def test_server_codec_ab_rejects_semantic_transcript_mismatch() -> None:
    codec_ab = importlib.import_module("noisebot_server.internal.ops.codec_ab")

    trial = codec_ab._trial_from_payload(
        codec="opus",
        phrase="me conte uma piada",
        previous_turn_id=20,
        metrics={
            "last_voice_session": {
                "turn_id": 21,
                "outcome": "llm",
                "transcript_quality": "good",
                "transcript": "me concha em piada",
            },
        },
        worker_before={
            "opus_egress_packets_drained": 0,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_bytes_total": 0,
        },
        worker_after={
            "opus_egress_packets_drained": 52,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_bytes_total": 7502,
        },
        server_codec_confirmed=True,
    )

    assert not trial.ok
    assert not trial.transcript_match
    assert trial.transcript_similarity < 0.72


def test_server_ai_status_exposes_firmware_audio_capabilities() -> None:
    schemas = importlib.import_module("noisebot_server.internal.ops.schemas")

    payload = schemas.ai_status_response(
        connected=True,
        pipeline="v2",
        mode="normal",
        provider="ollama",
        model="qwen2.5:7b",
        api_key_configured=True,
        stt_status="ok",
        llm_status="ok",
        tts_status="ok",
        last_error=None,
        last_turn_id=7,
        last_outcome="llm",
        last_transcript="me diga uma curiosidade",
        last_reply="claro",
        last_route="llm",
        firmware_capabilities={
            "audio": {
                "format": "opus",
                "sample_rate": 16000,
                "channels": 1,
                "chunk_samples": 960,
            },
            "codecs": {"pcm16": False, "opus": True},
            "codec_options": {"opus_tx": True, "opus_default": False},
            "features": ["voice_session_v2", "opus_tx"],
        },
    )

    assert payload["audio"]["format"] == "opus"
    assert payload["codecs"] == {"pcm16": False, "opus": True}
    assert payload["codec_options"] == {"opus_tx": True, "opus_default": False}
    assert payload["firmware"]["codec_options"] == payload["codec_options"]
    assert payload["features"] == ["voice_session_v2", "opus_tx"]
    assert payload["firmware"]["features"] == payload["features"]


def test_server_opus_live_accepts_status_capabilities() -> None:
    opus_live = importlib.import_module("noisebot_server.internal.ops.opus_live")

    assert opus_live._status_confirms_opus({"features": ["opus_tx"]})
    assert opus_live._status_confirms_opus({"audio": {"format": "opus"}})
    assert opus_live._status_confirms_opus({
        "firmware": {"codecs": {"pcm16": False, "opus": True}},
    })
    assert not opus_live._status_confirms_opus({
        "firmware": {"codecs": {"pcm16": True, "opus": False}},
    })


def test_server_firmware_diag_client_exposes_opus_endpoints(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []
    post_paths: list[str] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True}

    def fake_post_json(self, path, payload=None):
        post_paths.append(path)
        return {"ok": True}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    assert client.audio_opus_worker_status()["ok"]
    assert client.audio_opus_worker_probe()["ok"]
    assert client.audio_opus_worker_start()["ok"]
    assert client.audio_opus_worker_stop()["ok"]
    assert client.audio_opus_worker_encode_test()["ok"]
    assert client.audio_opus_worker_drain_packets()["ok"]
    assert client.audio_opus_transport_enable()["ok"]
    assert client.audio_opus_transport_disable()["ok"]

    assert get_paths == ["api/audio/opus/worker"]
    assert post_paths == [
        "api/audio/opus/worker/probe",
        "api/audio/opus/worker/start",
        "api/audio/opus/worker/stop",
        "api/audio/opus/worker/encode-test",
        "api/audio/opus/worker/drain-packets",
        "api/audio/opus/transport/enable",
        "api/audio/opus/transport/disable",
    ]


def test_server_firmware_diag_client_exposes_capture_v2_endpoints(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []
    post_calls: list[tuple[str, dict | None]] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True, "real_capture": False}

    def fake_post_json(self, path, payload=None):
        post_calls.append((path, payload))
        return {"ok": True, "real_capture": False}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    assert client.audio_capture_v2_status()["ok"]
    assert client.audio_capture_v2_replay({"speech_ms": 640})["ok"]
    assert client.audio_capture_v2_cancel()["ok"]
    assert client.set_voice_audio_v2_capture_enabled(True)["ok"]
    assert client.set_voice_audio_v2_capture_enabled(False)["ok"]

    assert get_paths == ["api/audio/capture-v2"]
    assert post_calls == [
        ("api/audio/capture-v2/replay", {"speech_ms": 640}),
        ("api/audio/capture-v2/cancel", None),
        ("api/config", {"key": "voice_audio_v2_capture_enabled", "value": 1}),
        ("api/config", {"key": "voice_audio_v2_capture_enabled", "value": 0}),
    ]


def test_server_firmware_diag_client_exposes_codec_v2_endpoint(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []
    post_paths: list[str] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True, "format": "pcm16"}

    def fake_post_json(self, path, payload=None):
        post_paths.append(path)
        return {"ok": True, "format": "pcm16"}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    assert client.audio_codec_v2_status()["ok"]
    assert client.audio_codec_v2_encode_test()["ok"]
    assert client.audio_codec_v2_drain()["ok"]
    assert client.audio_codec_v2_egress_drain()["ok"]
    assert client.audio_codec_v2_reset()["ok"]
    assert client.audio_codec_v2_opus_encode_test()["ok"]
    assert client.audio_codec_v2_worker_start()["ok"]
    assert client.audio_codec_v2_worker_stop()["ok"]
    assert client.audio_codec_v2_worker_stress_test(10)["ok"]
    assert client.audio_codec_v2_worker_feed_test(10)["ok"]
    assert client.audio_codec_v2_bridge_handoff_test(10)["ok"]
    assert client.audio_codec_v2_transport_enable()["ok"]
    assert client.audio_codec_v2_transport_disable()["ok"]
    assert client.audio_codec_v2_overflow_test(45)["ok"]
    assert get_paths == ["api/audio/codec-v2"]
    assert post_paths == [
        "api/audio/codec-v2/encode-test",
        "api/audio/codec-v2/drain",
        "api/audio/codec-v2/egress/drain",
        "api/audio/codec-v2/reset",
        "api/audio/codec-v2/opus-encode-test",
        "api/audio/codec-v2/worker/start",
        "api/audio/codec-v2/worker/stop",
        "api/audio/codec-v2/worker/stress-test",
        "api/audio/codec-v2/worker/feed-test",
        "api/audio/codec-v2/bridge-handoff-test",
        "api/audio/codec-v2/transport/enable",
        "api/audio/codec-v2/transport/disable",
        "api/audio/codec-v2/overflow-test",
    ]


def test_server_cli_parses_capture_v2_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "capture-v2",
        "live",
        "--speech-ms",
        "320",
        "--silence-ms",
        "900",
        "--source",
        "wake",
        "--no-prompt",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "capture-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "live"
    assert args.speech_ms == 320
    assert args.silence_ms == 900
    assert args.source == "wake"
    assert args.no_prompt
    assert args.json


def test_server_cli_runs_capture_v2_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_replay(self, payload=None):
        calls["base_url"] = self.base_url
        calls["payload"] = payload
        return {
            "ok": True,
            "real_capture_enabled": False,
            "real_capture": False,
            "state": "DONE",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_capture_v2_replay", fake_replay)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "capture-v2",
        "replay",
        "--speech-ms",
        "320",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"real_capture": false' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["payload"] == {
        "speech_ms": 320,
        "silence_ms": 900,
        "source": "debug",
    }


def test_server_cli_runs_capture_v2_live_with_rollback(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    toggles: list[bool] = []

    def fake_status(self):
        return {
            "ok": True,
            "real_capture_enabled": bool(toggles and toggles[-1]),
            "real_capture": bool(toggles and toggles[-1]),
            "state": "DONE",
            "voice_start_sent": True,
            "voice_audio_sent": True,
            "voice_end_sent": True,
            "speech_frames": 4,
            "captured_samples": 1024,
            "dropped_frames": 0,
        }

    def fake_set_enabled(self, enabled):
        toggles.append(enabled)
        return {"ok": True}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_capture_v2_status", fake_status)
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "set_voice_audio_v2_capture_enabled",
        fake_set_enabled,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "capture-v2",
        "live",
        "--no-prompt",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"disabled"' in captured.out
    assert toggles == [True, False]


def test_server_cli_parses_codec_v2_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "encode-test",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "encode-test"
    assert args.json


def test_server_cli_parses_codec_v2_drain_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "drain",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "drain"
    assert args.json


def test_server_cli_parses_codec_v2_egress_drain_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "egress-drain",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "egress-drain"
    assert args.json


def test_server_cli_parses_codec_v2_reset_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "reset",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "reset"
    assert args.json


def test_server_cli_parses_codec_v2_overflow_test_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "overflow-test",
        "--packets",
        "45",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "overflow-test"
    assert args.packets == 45
    assert args.json


def test_server_cli_parses_codec_v2_opus_encode_test_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "opus-encode-test",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "opus-encode-test"
    assert args.json


def test_server_cli_parses_codec_v2_worker_debug_commands() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    start_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-start",
        "--json",
    ])
    stop_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-stop",
        "--json",
    ])
    stress_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-stress-test",
        "--packets",
        "10",
        "--json",
    ])
    feed_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-feed-test",
        "--frames",
        "10",
        "--json",
    ])
    handoff_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "bridge-handoff-test",
        "--frames",
        "10",
        "--json",
    ])
    transport_enable_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "transport-enable",
        "--json",
    ])
    transport_disable_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "transport-disable",
        "--json",
    ])

    assert start_args.command == "debug"
    assert start_args.debug_command == "codec-v2"
    assert start_args.action == "worker-start"
    assert start_args.json
    assert stop_args.command == "debug"
    assert stop_args.debug_command == "codec-v2"
    assert stop_args.action == "worker-stop"
    assert stop_args.json
    assert stress_args.command == "debug"
    assert stress_args.debug_command == "codec-v2"
    assert stress_args.action == "worker-stress-test"
    assert stress_args.packets == 10
    assert stress_args.json
    assert feed_args.command == "debug"
    assert feed_args.debug_command == "codec-v2"
    assert feed_args.action == "worker-feed-test"
    assert feed_args.frames == 10
    assert feed_args.json
    assert handoff_args.command == "debug"
    assert handoff_args.debug_command == "codec-v2"
    assert handoff_args.action == "bridge-handoff-test"
    assert handoff_args.frames == 10
    assert handoff_args.json
    assert transport_enable_args.command == "debug"
    assert transport_enable_args.debug_command == "codec-v2"
    assert transport_enable_args.action == "transport-enable"
    assert transport_enable_args.json
    assert transport_disable_args.command == "debug"
    assert transport_disable_args.debug_command == "codec-v2"
    assert transport_disable_args.action == "transport-disable"
    assert transport_disable_args.json


def test_server_cli_runs_codec_v2_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_encode_test(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "initialized": False,
            "format": "pcm16",
            "worker_supported": False,
            "worker_active": False,
            "worker_state": "not_started",
            "opus_frame_ms": 60,
            "opus_frame_samples": 960,
            "opus_bitrate": 32000,
            "max_queue_packets": 40,
            "queue_count": 1,
            "packets_out": 1,
            "packet_drops": 0,
            "pending_samples": 64,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_encode_test", fake_encode_test)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "encode-test",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"format": "pcm16"' in captured.out
    assert '"worker_state": "not_started"' in captured.out
    assert '"packets_out": 1' in captured.out
    assert '"queue_count": 1' in captured.out
    assert '"opus_bitrate": 32000' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"


def test_server_cli_runs_codec_v2_drain_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_drain(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "initialized": False,
            "format": "pcm16",
            "queue_count": 0,
            "packets_out": 1,
            "packet_drops": 0,
            "pending_samples": 64,
            "drained_packets": 1,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_drain", fake_drain)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "drain",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"queue_count": 0' in captured.out
    assert '"drained_packets": 1' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"


def test_server_cli_runs_codec_v2_egress_drain_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_egress_drain(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "diagnostic": True,
            "test_format": "opus",
            "opus_egress_drain": True,
            "drained_packets": 3,
            "opus_egress_packets_drained": 3,
            "opus_egress_queue_count": 0,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_egress_drain",
        fake_egress_drain,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "egress-drain",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"opus_egress_drain": true' in captured.out
    assert '"drained_packets": 3' in captured.out
    assert '"opus_egress_queue_count": 0' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"


def test_server_cli_runs_codec_v2_reset_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_reset(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "initialized": False,
            "format": "pcm16",
            "sample_rate_hz": 16000,
            "opus_frame_samples": 960,
            "max_queue_packets": 40,
            "pcm_frames_in": 0,
            "packets_out": 0,
            "packet_drops": 0,
            "queue_count": 0,
            "pending_samples": 0,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_reset", fake_reset)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "reset",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"format": "pcm16"' in captured.out
    assert '"packets_out": 0' in captured.out
    assert '"pending_samples": 0' in captured.out
    assert '"max_queue_packets": 40' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"


def test_server_cli_runs_codec_v2_overflow_test_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_overflow_test(self, packets=45):
        calls["base_url"] = self.base_url
        calls["packets"] = packets
        return {
            "ok": True,
            "diagnostic": True,
            "intentional_overflow": True,
            "attempted_packets": packets,
            "accepted_packets": 40,
            "dropped_packets": packets - 40,
            "packet_drops_delta": packets - 40,
            "peak_queue_count": 40,
            "queue_count_after_cleanup": 0,
            "status_packet_drops_after_cleanup": 0,
            "max_queue_packets": 40,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_overflow_test",
        fake_overflow_test,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "overflow-test",
        "--packets",
        "45",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"intentional_overflow": true' in captured.out
    assert '"attempted_packets": 45' in captured.out
    assert '"dropped_packets": 5' in captured.out
    assert '"queue_count_after_cleanup": 0' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["packets"] == 45


def test_server_cli_runs_codec_v2_opus_encode_test_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_opus_encode_test(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "diagnostic": True,
            "test_format": "opus",
            "initialized": False,
            "format": "pcm16",
            "frame_samples": 960,
            "outbuf_bytes": 1024,
            "encoded_bytes": 180,
            "codec_error": 0,
            "opus_encode_tests": 1,
            "opus_encoded_bytes_total": 180,
            "opus_last_packet_bytes": 180,
            "worker_active": False,
            "worker_state": "not_started",
            "queue_count": 0,
            "packet_drops": 0,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_opus_encode_test",
        fake_opus_encode_test,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "opus-encode-test",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"test_format": "opus"' in captured.out
    assert '"encoded_bytes": 180' in captured.out
    assert '"worker_state": "not_started"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"


def test_server_cli_runs_codec_v2_worker_stress_test_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_worker_stress_test(self, packets=10):
        calls["base_url"] = self.base_url
        calls["packets"] = packets
        return {
            "ok": True,
            "diagnostic": True,
            "test_format": "opus",
            "worker_stress": True,
            "attempted_packets": packets,
            "accepted_packets": packets,
            "worker_drained_packets_delta": packets,
            "worker_opus_packets_delta": packets,
            "worker_opus_encoded_bytes_delta": packets * 248,
            "worker_opus_last_packet_bytes": 248,
            "packet_drops_delta": 0,
            "queue_count_after": 0,
            "worker_state_after": "stopped",
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_worker_stress_test",
        fake_worker_stress_test,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-stress-test",
        "--packets",
        "10",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"worker_stress": true' in captured.out
    assert '"worker_opus_packets_delta": 10' in captured.out
    assert '"queue_count_after": 0' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["packets"] == 10


def test_server_cli_runs_codec_v2_worker_feed_test_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_worker_feed_test(self, frames=10):
        calls["base_url"] = self.base_url
        calls["frames"] = frames
        return {
            "ok": True,
            "diagnostic": True,
            "test_format": "opus",
            "worker_feed": True,
            "attempted_frames": frames,
            "attempted_samples": frames * 960,
            "pcm_frames_in_delta": frames,
            "packets_out_delta": frames,
            "worker_drained_packets_delta": frames,
            "worker_opus_packets_delta": frames,
            "worker_opus_encoded_bytes_delta": frames * 248,
            "worker_opus_last_packet_bytes": 248,
            "worker_payload_observer": True,
            "worker_payload_packets_delta": frames,
            "worker_payload_bytes_delta": frames * 248,
            "worker_payload_last_bytes": 248,
            "worker_payload_last_sequence": frames,
            "worker_payload_last_checksum": 123456,
            "worker_payload_preview_len": 16,
            "worker_payload_preview_hex": "00112233445566778899aabbccddeeff",
            "opus_egress_queue": True,
            "opus_egress_packets_delta": frames,
            "opus_egress_bytes_delta": frames * 248,
            "opus_egress_packet_drops_delta": 0,
            "opus_egress_drained_after_test": frames,
            "opus_egress_queue_count_after_cleanup": 0,
            "opus_egress_last_bytes": 248,
            "opus_egress_last_sequence": frames,
            "opus_egress_last_checksum": 123456,
            "opus_egress_preview_len": 16,
            "opus_egress_preview_hex": "00112233445566778899aabbccddeeff",
            "packet_drops_delta": 0,
            "queue_count_after": 0,
            "pending_samples_after": 0,
            "worker_state_after": "stopped",
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_worker_feed_test",
        fake_worker_feed_test,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-feed-test",
        "--frames",
        "10",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"worker_feed": true' in captured.out
    assert '"worker_payload_observer": true' in captured.out
    assert '"opus_egress_queue": true' in captured.out
    assert '"worker_payload_preview_hex": "00112233445566778899aabbccddeeff"' in captured.out
    assert '"opus_egress_queue_count_after_cleanup": 0' in captured.out
    assert '"pcm_frames_in_delta": 10' in captured.out
    assert '"pending_samples_after": 0' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["frames"] == 10


def test_server_cli_runs_codec_v2_bridge_handoff_test_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_bridge_handoff_test(self, frames=10):
        calls["base_url"] = self.base_url
        calls["frames"] = frames
        return {
            "ok": True,
            "diagnostic": True,
            "test_format": "opus",
            "bridge_handoff_stub": True,
            "bridge_transport_unchanged": True,
            "bridge_packet_not_sent": True,
            "attempted_frames": frames,
            "opus_egress_packets_delta": frames,
            "opus_egress_bytes_delta": frames * 248,
            "bridge_handoff_packets_ready_delta": frames,
            "bridge_handoff_bytes_ready_delta": frames * 248,
            "bridge_handoff_last_bytes": 248,
            "bridge_handoff_last_sequence": frames,
            "bridge_handoff_last_checksum": 123456,
            "bridge_handoff_preview_len": 16,
            "bridge_handoff_preview_hex": "00112233445566778899aabbccddeeff",
            "opus_egress_queue_count_after_cleanup": 0,
            "packet_drops_delta": 0,
            "worker_state_after": "stopped",
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_bridge_handoff_test",
        fake_bridge_handoff_test,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "bridge-handoff-test",
        "--frames",
        "10",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"bridge_handoff_stub": true' in captured.out
    assert '"bridge_transport_unchanged": true' in captured.out
    assert '"bridge_packet_not_sent": true' in captured.out
    assert '"bridge_handoff_packets_ready_delta": 10' in captured.out
    assert '"bridge_handoff_preview_hex": "00112233445566778899aabbccddeeff"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["frames"] == 10


def test_server_cli_runs_codec_v2_transport_debug_commands(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: list[str] = []

    def fake_transport_enable(self):
        calls.append(f"enable:{self.base_url}")
        return {
            "ok": True,
            "codec_v2_transport": True,
            "live_bridge_transport": True,
            "transport_worker": "audio_codec_service_v2",
            "compat_worker": "audio_codec_service_v2",
            "pcm16_fallback": True,
            "opus_enabled": True,
            "error": "ESP_OK",
        }

    def fake_transport_disable(self):
        calls.append(f"disable:{self.base_url}")
        return {
            "ok": True,
            "codec_v2_transport": True,
            "live_bridge_transport": False,
            "transport_worker": "audio_codec_service_v2",
            "compat_worker": "audio_codec_service_v2",
            "pcm16_fallback": True,
            "egress_drained_packets": 1,
            "opus_enabled": False,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_transport_enable",
        fake_transport_enable,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_codec_v2_transport_disable",
        fake_transport_disable,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "transport-enable",
        "--json",
    ])
    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "transport-disable",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"codec_v2_transport": true' in captured.out
    assert '"live_bridge_transport": true' in captured.out
    assert '"live_bridge_transport": false' in captured.out
    assert '"transport_worker": "audio_codec_service_v2"' in captured.out
    assert '"compat_worker": "audio_codec_service_v2"' in captured.out
    assert '"egress_drained_packets": 1' in captured.out
    assert '"pcm16_fallback": true' in captured.out
    assert calls == [
        "enable:http://192.168.1.30/",
        "disable:http://192.168.1.30/",
    ]


def test_server_cli_runs_codec_v2_worker_debug_commands(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: list[str] = []

    def fake_worker_start(self):
        calls.append(f"start:{self.base_url}")
        return {
            "ok": True,
            "worker_supported": True,
            "worker_active": True,
            "worker_state": "running",
            "worker_drained_packets": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        }

    def fake_worker_stop(self):
        calls.append(f"stop:{self.base_url}")
        return {
            "ok": True,
            "worker_supported": True,
            "worker_active": False,
            "worker_state": "stopped",
            "worker_drained_packets": 1,
            "queue_count": 0,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_worker_start", fake_worker_start)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_worker_stop", fake_worker_stop)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-start",
        "--json",
    ])
    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "worker-stop",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"worker_state": "running"' in captured.out
    assert '"worker_state": "stopped"' in captured.out
    assert calls == [
        "start:http://192.168.1.30/",
        "stop:http://192.168.1.30/",
    ]


def test_server_cli_parses_barge_live_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "debug",
        "barge-live",
        "me conte uma historia longa",
        "--server-url",
        "http://127.0.0.1:8765",
        "--firmware-url",
        "http://192.168.1.30",
        "--codec",
        "opus-v2",
        "--timeout-s",
        "12",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "barge-live"
    assert args.phrase == "me conte uma historia longa"
    assert args.server_url == "http://127.0.0.1:8765"
    assert args.firmware_url == "http://192.168.1.30"
    assert args.codec == "opus-v2"
    assert args.timeout_s == 12.0
    assert args.json


def test_server_cli_runs_barge_live_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    barge_live = importlib.import_module("noisebot_server.internal.ops.barge_live")

    calls: dict[str, object] = {}

    def fake_run_barge_live_trial(**kwargs):
        calls.update(kwargs)
        return barge_live.BargeLiveTrial(
            phrase=kwargs["phrase"],
            codec=kwargs["codec"],
            ok=True,
            interrupted_turn_id=88,
            interruption_cancel_ms=12.5,
            transcript="me conte uma historia longa",
            reply="era uma vez",
            discard_reason="barge_in",
            outcome="interrupted",
        )

    monkeypatch.setattr(barge_live, "run_barge_live_trial", fake_run_barge_live_trial)

    cli.main([
        "debug",
        "barge-live",
        "me conte uma historia longa",
        "--codec",
        "opus-v2",
        "--firmware-url",
        "http://192.168.1.30",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert calls["phrase"] == "me conte uma historia longa"
    assert calls["codec"] == "opus-v2"
    assert calls["firmware_url"] == "http://192.168.1.30"


def test_server_barge_live_accepts_aggregate_interruption(monkeypatch) -> None:
    barge_live = importlib.import_module("noisebot_server.internal.ops.barge_live")
    codec_v2_live = importlib.import_module("noisebot_server.internal.ops.codec_v2_live")

    payloads = [
        {
            "turns": {"interrupted": 1},
            "latency_ms": {"interruption_cancel": {"count": 1, "p95": 1.0}},
            "recent_voice_sessions": [{"turn_id": 81, "outcome": "llm"}],
        },
        {
            "turns": {"interrupted": 2},
            "latency_ms": {"interruption_cancel": {"count": 2, "p95": 1.0}},
            "recent_voice_sessions": [
                {
                    "turn_id": 82,
                    "outcome": "local_intent",
                    "transcript": "Que horas são?",
                    "reply": "Agora sao 17 horas.",
                    "discard_reason": None,
                },
                {"turn_id": 81, "outcome": "llm"},
            ],
        },
    ]

    def fake_get_json(url):
        return payloads.pop(0) if payloads else payloads[-1]

    monkeypatch.setattr(barge_live, "get_json", fake_get_json)
    monkeypatch.setattr(
        codec_v2_live,
        "get_json",
        lambda _url: {"audio": {"format": "pcm16"}, "codecs": {"opus": False}},
    )

    trial = barge_live.run_barge_live_trial(
        phrase="me conte uma historia longa",
        server_url="http://127.0.0.1:8765",
        timeout_s=1.0,
        codec="pcm16",
        input_fn=lambda _prompt: "",
        print_fn=lambda _text: None,
    )

    assert trial.ok is True
    assert trial.interrupted_turn_id == 82
    assert trial.interruption_cancel_ms == 1.0
    assert trial.transcript == "Que horas são?"


def test_server_barge_live_opus_v2_tracks_codec_stats(monkeypatch) -> None:
    barge_live = importlib.import_module("noisebot_server.internal.ops.barge_live")
    codec_v2_live = importlib.import_module("noisebot_server.internal.ops.codec_v2_live")

    metrics_payloads = [
        {
            "turns": {"interrupted": 0},
            "latency_ms": {"interruption_cancel": {"count": 0}},
            "recent_voice_sessions": [{"turn_id": 100, "outcome": "llm"}],
        },
        {
            "turns": {"interrupted": 1},
            "latency_ms": {"interruption_cancel": {"count": 1, "p95": 2.0}},
            "recent_voice_sessions": [
                {
                    "turn_id": 101,
                    "outcome": "interrupted",
                    "discard_reason": "barge_in",
                    "transcript": "pare",
                }
            ],
        },
    ]

    def fake_metrics_get_json(url):
        assert url.endswith("/ai/metrics")
        return metrics_payloads.pop(0)

    firmware_status = iter([
        {"features": ["opus_tx"]},
        {"opus_egress_packets_drained": 10, "opus_egress_bytes_total": 1000},
        {"opus_egress_packets_drained": 14, "opus_egress_bytes_total": 1600},
    ])
    post_paths: list[str] = []

    def fake_codec_get_json(url):
        return next(firmware_status)

    def fake_post_json(url):
        post_paths.append(url)
        return {"ok": True, "opus_enabled": "transport/enable" in url}

    monkeypatch.setattr(barge_live, "get_json", fake_metrics_get_json)
    monkeypatch.setattr(codec_v2_live, "get_json", fake_codec_get_json)
    monkeypatch.setattr(codec_v2_live, "post_json", fake_post_json)

    trial = barge_live.run_barge_live_trial(
        phrase="me conte uma historia longa",
        server_url="http://127.0.0.1:8765",
        firmware_url="http://192.168.1.30",
        codec="opus-v2",
        timeout_s=1.0,
        input_fn=lambda _prompt: "",
        print_fn=lambda _text: None,
    )

    assert trial.ok is True
    assert trial.codec == "opus-v2"
    assert trial.packets_drained == 4
    assert trial.encoded_bytes == 600
    assert trial.packet_drops == 0
    assert post_paths == [
        "http://192.168.1.30/api/audio/codec-v2/transport/enable",
        "http://192.168.1.30/api/audio/codec-v2/transport/disable",
        "http://192.168.1.30/api/audio/codec-v2/egress/drain",
    ]


def test_server_cli_parses_no_echo_live_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "debug",
        "no-echo-live",
        "me conte uma historia longa",
        "--server-url",
        "http://127.0.0.1:8765",
        "--firmware-url",
        "http://192.168.1.30",
        "--codec",
        "opus-v2",
        "--quiet-window-s",
        "6",
        "--timeout-s",
        "12",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "no-echo-live"
    assert args.phrase == "me conte uma historia longa"
    assert args.server_url == "http://127.0.0.1:8765"
    assert args.firmware_url == "http://192.168.1.30"
    assert args.codec == "opus-v2"
    assert args.quiet_window_s == 6.0
    assert args.timeout_s == 12.0
    assert args.json


def test_server_cli_runs_no_echo_live_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    no_echo = importlib.import_module("noisebot_server.internal.ops.no_echo_live")

    calls: dict[str, object] = {}

    def fake_run_no_echo_live_trial(**kwargs):
        calls.update(kwargs)
        return no_echo.NoEchoLiveTrial(
            phrase=kwargs["phrase"],
            codec=kwargs["codec"],
            ok=True,
            response_turn_id=91,
            unexpected_turn_id=None,
            quiet_window_s=kwargs["quiet_window_s"],
            outcome="llm",
            transcript="me conte uma historia longa",
            discard_reason="",
        )

    monkeypatch.setattr(no_echo, "run_no_echo_live_trial", fake_run_no_echo_live_trial)

    cli.main([
        "debug",
        "no-echo-live",
        "me conte uma historia longa",
        "--codec",
        "opus-v2",
        "--firmware-url",
        "http://192.168.1.30",
        "--quiet-window-s",
        "6",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert calls["phrase"] == "me conte uma historia longa"
    assert calls["codec"] == "opus-v2"
    assert calls["firmware_url"] == "http://192.168.1.30"
    assert calls["quiet_window_s"] == 6.0


def test_server_no_echo_live_pcm16_tracks_response_turn(monkeypatch) -> None:
    no_echo = importlib.import_module("noisebot_server.internal.ops.no_echo_live")
    codec_v2_live = importlib.import_module("noisebot_server.internal.ops.codec_v2_live")

    payloads = [
        {"recent_voice_sessions": [{"turn_id": 200, "outcome": "llm"}]},
        {"recent_voice_sessions": [{"turn_id": 201, "outcome": "llm", "transcript": "historia"}]},
    ]

    def fake_get_json(url):
        assert url.endswith("/ai/metrics")
        return payloads.pop(0) if payloads else {"recent_voice_sessions": [{"turn_id": 201}]}

    monkeypatch.setattr(no_echo, "get_json", fake_get_json)
    monkeypatch.setattr(
        codec_v2_live,
        "get_json",
        lambda _url: {"audio": {"format": "pcm16"}, "codecs": {"opus": False}},
    )

    trial = no_echo.run_no_echo_live_trial(
        phrase="me conte uma historia longa",
        server_url="http://127.0.0.1:8765",
        quiet_window_s=0.01,
        timeout_s=1.0,
        input_fn=lambda _prompt: "",
        print_fn=lambda _text: None,
    )

    assert trial.ok is True
    assert trial.codec == "pcm16"
    assert trial.response_turn_id == 201
    assert trial.unexpected_turn_id is None


def test_server_cli_parses_aec_live_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "aec-live",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "aec-live"
    assert args.host == "192.168.1.30"
    assert args.json


def test_server_cli_runs_aec_live_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    aec_live = importlib.import_module("noisebot_server.internal.ops.aec_live")

    calls: dict[str, object] = {}

    def fake_run_aec_live_probe(**kwargs):
        calls.update(kwargs)
        return aec_live.AecLiveTrial(
            ok=True,
            promotable=False,
            probe_ok=False,
            supported=False,
            blocked_no_reference=True,
            probe_error="ESP_OK",
            internal_free_kb=39,
            dma_largest_kb=38,
            psram_current_kb=7313,
            status_after_ok=True,
            recommendation="Nao promover AEC: placa sem referencia limpa de speaker.",
        )

    monkeypatch.setattr(aec_live, "run_aec_live_probe", fake_run_aec_live_probe)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "aec-live",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"promotable": false' in captured.out
    assert calls["firmware_url"] == "http://192.168.1.30"


def test_aec_live_accepts_firmware_500_diagnostic(monkeypatch) -> None:
    aec_live = importlib.import_module("noisebot_server.internal.ops.aec_live")

    diagnostic = {
        "ok": False,
        "aec_probe_ok": False,
        "aec_supported": False,
        "aec_blocked_no_reference": True,
        "probe_error": "ESP_ERR_NOT_SUPPORTED",
        "internal_free_kb": 31,
        "dma_largest_kb": 30,
        "shadow_psram_current_kb": 7246,
    }

    def fake_urlopen(*_: object, **__: object) -> object:
        body = io.BytesIO(json.dumps(diagnostic).encode("utf-8"))
        raise HTTPError(
            url="http://192.168.1.30/api/audio/processor/aec/probe",
            code=500,
            msg="Internal Server Error",
            hdrs={},
            fp=body,
        )

    monkeypatch.setattr(aec_live, "urlopen", fake_urlopen)
    monkeypatch.setattr(aec_live, "get_json", lambda *_args, **_kwargs: {"ok": True})

    trial = aec_live.run_aec_live_probe(firmware_url="http://192.168.1.30")

    assert trial.ok is True
    assert trial.promotable is False
    assert trial.supported is False
    assert trial.blocked_no_reference is True
    assert trial.probe_error == "ESP_ERR_NOT_SUPPORTED"
    assert "Nao promover AEC" in trial.recommendation


def test_server_cli_runs_service_status_without_bridge_entrypoint(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    manager_module = importlib.import_module("noisebot_server.internal.service.manager")

    class FakeManager:
        def install(self) -> None:
            raise AssertionError("unexpected install")

        def uninstall(self) -> None:
            raise AssertionError("unexpected uninstall")

        def status(self) -> str:
            return "ok server"

        def start(self) -> None:
            raise AssertionError("unexpected start")

        def stop(self) -> None:
            raise AssertionError("unexpected stop")

    monkeypatch.setattr(manager_module, "get_manager", lambda: FakeManager())

    cli.main(["service", "status"])

    captured = capsys.readouterr()
    assert "ok server" in captured.out


def test_server_debug_msg_name_uses_server_boundary() -> None:
    manual = importlib.import_module("noisebot_server.internal.debug.manual")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    assert manual.msg_name(protocol.MSG_HELLO) == "HELLO"
    assert manual.msg_name(0xFE) == "0xFE"


def test_server_service_manager_uses_server_identity() -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    assert manager.TASK_NAME == "NoiseBot Server"
    assert manager.SERVICE_NAME == "noisebot-server"
    assert "-m noisebot_server" in manager.SYSTEMD_TEMPLATE
    assert "SyslogIdentifier=noisebot-server" in manager.SYSTEMD_TEMPLATE


def test_server_service_selects_windows_manager(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    monkeypatch.setattr(manager.platform, "system", lambda: "Windows")

    assert isinstance(manager.get_manager(), manager.WindowsTaskSchedulerManager)


def test_server_service_selects_systemd_manager(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    monkeypatch.setattr(manager.platform, "system", lambda: "Linux")

    assert isinstance(manager.get_manager(), manager.SystemdManager)


def test_server_service_windows_install_uses_noisebot_module(monkeypatch) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")
    calls: list[list[str]] = []

    class Result:
        returncode = 0
        stdout = ""
        stderr = ""

    def fake_run(cmd: list[str], **_: object) -> Result:
        calls.append(cmd)
        return Result()

    monkeypatch.setattr(manager.subprocess, "run", fake_run)
    workdir = manager.Path("D:/NoiseBot")
    monkeypatch.setattr(manager, "service_workdir", lambda: workdir)

    manager.WindowsTaskSchedulerManager().install()

    script = calls[0][-1]
    assert manager.TASK_NAME in script
    assert "-m noisebot_server" in script
    assert str(workdir) in script


def test_server_service_systemd_install_writes_noisebot_unit(
    monkeypatch,
    tmp_path,
) -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")
    calls: list[list[str]] = []

    class Result:
        returncode = 0
        stdout = ""
        stderr = ""

    def fake_run(cmd: list[str], **_: object) -> Result:
        calls.append(cmd)
        return Result()

    service = manager.SystemdManager()
    monkeypatch.setattr(type(service), "_unit_dir", property(lambda _: tmp_path))
    monkeypatch.setattr(manager.subprocess, "run", fake_run)
    workdir = manager.Path("/noisebot")
    monkeypatch.setattr(manager, "service_workdir", lambda: workdir)

    service.install()

    unit_file = tmp_path / f"{manager.SERVICE_NAME}.service"
    content = unit_file.read_text(encoding="utf-8")
    assert unit_file.exists()
    assert "ExecStart=" in content
    assert "-m noisebot_server" in content
    assert f"WorkingDirectory={workdir}" in content
    assert "Restart=on-failure" in content
    assert any("daemon-reload" in call for call in calls)
    assert any("enable" in call for call in calls)


def test_server_healthcheck_is_server_owned(monkeypatch, tmp_path) -> None:
    health = importlib.import_module("noisebot_server.internal.service.healthcheck")

    health_file = tmp_path / "noisebot-server.health"
    monkeypatch.setattr(health, "HEALTHCHECK_FILE", health_file)

    health.write_healthy("ok")

    assert health_file.exists()
    assert health.is_healthy(max_age_s=60.0)
    assert "ok" in health_file.read_text(encoding="utf-8")

    health.write_unhealthy("teste")

    assert not health.is_healthy(max_age_s=60.0)

    health.remove_healthcheck()

    assert not health_file.exists()


def test_server_runtime_uses_noisebot_server_app() -> None:
    runtime = importlib.import_module("noisebot_server.runtime")
    app_module = importlib.import_module("noisebot_server.app")

    assert runtime.NoiseBotServer is app_module.NoiseBotServer


def test_server_app_no_longer_inherits_bridge_application() -> None:
    _ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")
    bridge_app = importlib.import_module("bridgev2.app")

    assert not issubclass(app_module.NoiseBotServer, bridge_app.Application)


def test_server_app_dry_run_suppresses_supervisor() -> None:
    app_module = importlib.import_module("noisebot_server.app")

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=True)
    )

    assert app._supervisor is None


def test_server_app_tcp_config_creates_supervisor() -> None:
    app_module = importlib.import_module("noisebot_server.app")

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=False)
    )

    assert app._supervisor is not None
    assert app._get_adapter() is None


@pytest.mark.asyncio
async def test_server_app_keeps_pcm16_default_without_firmware_call(monkeypatch) -> None:
    app_module = importlib.import_module("noisebot_server.app")
    calls: list[str] = []

    class FakeFirmwareDiagClient:
        @classmethod
        def from_config(cls, config):
            calls.append("from_config")
            return cls()

    monkeypatch.setattr(app_module, "FirmwareDiagClient", FakeFirmwareDiagClient)

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=False, default_codec="pcm16")
    )

    await app._apply_default_audio_codec()

    assert calls == []


@pytest.mark.asyncio
async def test_server_app_can_enable_opus_v2_default_codec(monkeypatch) -> None:
    app_module = importlib.import_module("noisebot_server.app")
    calls: list[str] = []

    class FakeFirmwareDiagClient:
        @classmethod
        def from_config(cls, config):
            calls.append("from_config")
            return cls()

        def audio_codec_v2_transport_enable(self):
            calls.append("transport_enable")
            return {"ok": True, "opus_enabled": True}

    monkeypatch.setattr(app_module, "FirmwareDiagClient", FakeFirmwareDiagClient)

    app = app_module.NoiseBotServer(
        _make_server_config(host="127.0.0.1", dry_run=False, default_codec="opus-v2")
    )

    await app._apply_default_audio_codec()

    assert calls == ["from_config", "transport_enable"]


def test_server_transport_exports_bridge_compatible_protocol() -> None:
    _ensure_bridgev2_path()

    server_protocol = importlib.import_module(
        "noisebot_server.internal.transport.protocol"
    )
    bridge_protocol = importlib.import_module("bridgev2.protocol.framing")
    bridge_messages = importlib.import_module("bridgev2.protocol.messages")

    payload = bridge_messages.encode_expr(3, 1500)

    assert server_protocol.encode_frame(bridge_messages.MSG_EXPR, payload) == (
        bridge_protocol.encode_frame(bridge_messages.MSG_EXPR, payload)
    )


def test_server_transport_protocol_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_protocol = importlib.import_module(
        "noisebot_server.internal.transport.protocol"
    )
    bridge_codec = importlib.import_module("bridgev2.protocol.codec")

    assert server_protocol.FrameDecoder is not bridge_codec.FrameDecoder


def test_server_transport_protocol_decodes_split_frames() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    payload = protocol.encode_text_scroll("ola noise")
    frame = protocol.encode_frame(protocol.MSG_TEXT_SCROLL, payload)
    decoder = protocol.FrameDecoder()

    decoder.feed(frame[:3])

    assert decoder.frames() == []
    assert decoder.buffered_bytes == 3

    decoder.feed(frame[3:])

    assert decoder.frames() == [(protocol.MSG_TEXT_SCROLL, payload)]
    assert decoder.buffered_bytes == 0


def test_server_transport_protocol_discards_bad_crc() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")
    frame = bytearray(protocol.encode_frame(protocol.MSG_VOLUME, protocol.encode_volume(50)))
    frame[-1] ^= 0xFF
    buf = bytearray(frame)

    assert protocol.decode_frames(buf) == []
    assert buf == bytearray()


def test_server_transport_factory_creates_tcp_transport() -> None:
    config_module = importlib.import_module("noisebot_server.config")
    factory_module = importlib.import_module(
        "noisebot_server.internal.transport.factory"
    )

    config = config_module.NoiseBotServerConfig(
        transport=config_module.TransportConfig(
            host="192.168.1.30",
            port=9000,
            uart=None,
            baudrate=1000000,
        ),
        llm=config_module.LlmConfig(
            provider=config_module.LlmProvider.NONE,
            model="none",
            timeout_s=10.0,
            temperature=0.7,
            max_output_tokens=256,
            max_reply_chars=180,
            ollama_base_url="http://127.0.0.1:11434",
            ollama_think=False,
            openai_key_configured=False,
            gemini_key_configured=False,
        ),
        pipeline_mode=config_module.PipelineMode.LOCAL_ONLY,
        stt=config_module.SttConfig(
            model="small",
            backend="faster",
            device="cpu",
            compute_type="int8",
        ),
        tts=config_module.TtsConfig(
            piper_executable="piper",
            piper_model="",
            cache_size=64,
            sample_rate=16000,
            target_peak=12000,
        ),
        audio=config_module.AudioConfig(
            chunk_samples=256,
            sample_rate=16000,
            default_codec="pcm16",
            min_transcribe_rms=140.0,
            min_transcribe_peak=1600,
            min_utterance_samples=8000,
            max_utterance_samples=160000,
            max_no_speech_prob=0.75,
            min_avg_logprob=-1.10,
            max_compression_ratio=2.60,
        ),
        reconnect=config_module.ReconnectConfig(
            delay_s=1.0,
            max_delay_s=30.0,
            connect_timeout_s=5.0,
        ),
        ops=config_module.OpsConfig(
            port=8765,
            token_configured=False,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=True,
        replay_path=None,
    )

    transport = factory_module.create_transport_factory(config)()

    assert transport.description == "TCP 192.168.1.30:9000"


def test_server_transport_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_transport = importlib.import_module("noisebot_server.internal.transport")
    bridge_adapter = importlib.import_module("bridgev2.transport.adapter")
    bridge_tcp = importlib.import_module("bridgev2.transport.tcp")
    bridge_supervisor = importlib.import_module("bridgev2.transport.reconnect")

    assert server_transport.FirmwareAdapter is not bridge_adapter.FirmwareAdapter
    assert server_transport.TcpTransport is not bridge_tcp.TcpTransport
    assert (
        server_transport.ConnectionSupervisor
        is not bridge_supervisor.ConnectionSupervisor
    )


async def test_server_firmware_adapter_dispatches_voice_end_event() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    queue = bus.subscribe(runtime.VoiceActivityEnd)
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)
    payload = struct.pack(
        "<I",
        protocol.NB_EVT_VOICE_ACTIVITY_END,
    ) + bytes([runtime.VoiceEndReason.TIMEOUT])

    await adapter._dispatch_rx(protocol.MSG_EVENT, payload)

    event = await asyncio.wait_for(queue.get(), timeout=0.1)
    assert event.reason == runtime.VoiceEndReason.TIMEOUT


async def test_server_firmware_adapter_rejects_audio_chunk_outside_contract() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    queue = bus.subscribe(runtime.AudioChunkIn)
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)

    await adapter._dispatch_rx(protocol.MSG_AUDIO_CHUNK, _server_loud_pcm(samples=255))
    assert await _drain_queue(queue) == []

    valid_pcm = _server_loud_pcm(samples=256)
    await adapter._dispatch_rx(protocol.MSG_AUDIO_CHUNK, valid_pcm)
    event = await asyncio.wait_for(queue.get(), timeout=0.1)
    assert event.pcm == valid_pcm


async def test_server_firmware_adapter_decodes_opus_audio_chunk_when_negotiated() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")
    opus_codec = importlib.import_module("noisebot_server.internal.transport.opus_codec")

    if not opus_codec.opus_available():
        pytest.skip("PyAV/libopus indisponivel")

    import numpy as np

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    queue = bus.subscribe(runtime.AudioChunkIn)
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)
    adapter._peer_capabilities = {
        "audio": {
            "format": "opus",
            "sample_rate": 16000,
            "channels": 1,
            "frame_ms": 60,
        },
        "codecs": {"pcm16": False, "opus": True},
    }
    t = np.arange(opus_codec.OPUS_FRAME_SAMPLES, dtype=np.float32) / 16000.0
    pcm = (np.sin(2.0 * math.pi * 440.0 * t) * 5000.0).astype(np.int16)
    packet = opus_codec.OpusEncoder().encode_frame(pcm)

    await adapter._dispatch_rx(protocol.MSG_AUDIO_CHUNK, packet)

    event = await asyncio.wait_for(queue.get(), timeout=0.1)
    decoded = np.frombuffer(event.pcm, dtype=np.int16)
    assert decoded.size > 0
    assert decoded.std() > 100


async def test_server_firmware_adapter_decodes_opus_audio_chunk_before_renegotiation() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")
    opus_codec = importlib.import_module("noisebot_server.internal.transport.opus_codec")

    if not opus_codec.opus_available():
        pytest.skip("PyAV/libopus indisponivel")

    import numpy as np

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    queue = bus.subscribe(runtime.AudioChunkIn)
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)
    t = np.arange(opus_codec.OPUS_FRAME_SAMPLES, dtype=np.float32) / 16000.0
    pcm = (np.sin(2.0 * math.pi * 440.0 * t) * 5000.0).astype(np.int16)
    packet = opus_codec.OpusEncoder().encode_frame(pcm)

    await adapter._dispatch_rx(protocol.MSG_AUDIO_CHUNK, packet)

    event = await asyncio.wait_for(queue.get(), timeout=0.1)
    decoded = np.frombuffer(event.pcm, dtype=np.int16)
    assert decoded.size > 0
    assert decoded.std() > 100


async def test_fake_firmware_opus_session_reaches_adapter_as_pcm() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    fake_module = importlib.import_module("noisebot_server.internal.debug.fake_firmware")
    tcp_module = importlib.import_module("noisebot_server.internal.transport.tcp")
    opus_codec = importlib.import_module("noisebot_server.internal.transport.opus_codec")

    if not opus_codec.opus_available():
        pytest.skip("PyAV/libopus indisponivel")

    import numpy as np

    bus = runtime.EventBus()
    queue = bus.subscribe(
        runtime.FirmwareConnected,
        runtime.VoiceActivityStart,
        runtime.AudioChunkIn,
        runtime.VoiceActivityEnd,
    )
    firmware = fake_module.FakeFirmware(port=0, audio_format="opus")
    await firmware.start()
    transport = tcp_module.TcpTransport("127.0.0.1", firmware.port)
    adapter = adapter_module.FirmwareAdapter(transport, bus)
    task = None
    try:
        await transport.connect()
        task = asyncio.create_task(adapter.run())
        connected = await asyncio.wait_for(queue.get(), timeout=1.0)
        assert isinstance(connected, runtime.FirmwareConnected)
        assert connected.peer_capabilities["audio"]["format"] == "opus"
        assert connected.peer_capabilities["codecs"] == {"pcm16": False, "opus": True}

        await firmware.send_voice_start()
        t = np.arange(opus_codec.OPUS_FRAME_SAMPLES, dtype=np.float32) / 16000.0
        pcm = (np.sin(2.0 * math.pi * 440.0 * t) * 5000.0).astype(np.int16)
        await firmware.send_pcm(pcm.tobytes())
        await firmware.send_voice_end()

        start = await asyncio.wait_for(queue.get(), timeout=1.0)
        audio = await asyncio.wait_for(queue.get(), timeout=1.0)
        end = await asyncio.wait_for(queue.get(), timeout=1.0)
        assert isinstance(start, runtime.VoiceActivityStart)
        assert isinstance(audio, runtime.AudioChunkIn)
        assert isinstance(end, runtime.VoiceActivityEnd)
        decoded = np.frombuffer(audio.pcm, dtype=np.int16)
        assert decoded.size > 0
        assert decoded.std() > 100
    finally:
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await transport.disconnect()
        await firmware.stop()


async def test_fake_firmware_opus_session_reaches_orchestrator_stt() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    fake_module = importlib.import_module("noisebot_server.internal.debug.fake_firmware")
    tcp_module = importlib.import_module("noisebot_server.internal.transport.tcp")
    opus_codec = importlib.import_module("noisebot_server.internal.transport.opus_codec")

    if not opus_codec.opus_available():
        pytest.skip("PyAV/libopus indisponivel")

    import numpy as np

    class MockStt:
        def __init__(self) -> None:
            self.finalize_calls = 0
            self.samples = 0

        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            self.finalize_calls += 1
            self.samples = len(full_pcm) // 2
            return runtime.FinalTranscript(
                turn_id=turn_id,
                text="que horas sao agora",
                quality=runtime.TranscriptQuality.GOOD,
            )

        async def close(self) -> None:
            pass

        async def reset(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockStt()
    firmware = fake_module.FakeFirmware(port=0, audio_format="opus")
    await firmware.start()
    transport = tcp_module.TcpTransport("127.0.0.1", firmware.port)
    adapter = adapter_module.FirmwareAdapter(transport, bus)
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(max_utterance_samples=192000),
        get_adapter=lambda: adapter,
        stt_provider=stt,
    )
    intents = bus.subscribe(runtime.IntentResolved)
    adapter_task = None
    orchestrator_task = None
    try:
        await transport.connect()
        adapter_task = asyncio.create_task(adapter.run())
        orchestrator_task = asyncio.create_task(orchestrator.run())
        assert await firmware.wait_connected(timeout=1.0)
        await _wait_until(lambda: adapter.is_connected, timeout_s=1.0)

        await firmware.send_voice_start()
        base = np.arange(opus_codec.OPUS_FRAME_SAMPLES, dtype=np.float32) / 16000.0
        tone = (np.sin(2.0 * math.pi * 440.0 * base) * 5000.0).astype(np.int16)
        for _ in range(10):
            await firmware.send_pcm(tone.tobytes())
        await firmware.send_voice_end()

        await _wait_until(lambda: stt.finalize_calls == 1, timeout_s=1.0)
        intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        assert stt.samples >= 8000
        assert intent.turn_id > 0
        assert intent.reply_text
    finally:
        await orchestrator.shutdown()
        for task in (adapter_task, orchestrator_task):
            if task is not None:
                task.cancel()
        await asyncio.gather(
            *(task for task in (adapter_task, orchestrator_task) if task is not None),
            return_exceptions=True,
        )
        await transport.disconnect()
        await firmware.stop()


async def test_server_firmware_adapter_drops_pending_speech_before_cancel() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    adapter_module = importlib.import_module("noisebot_server.internal.transport.adapter")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    class DummyTransport:
        is_connected = True
        description = "dummy"

        async def connect(self) -> None:
            pass

        async def disconnect(self) -> None:
            pass

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    bus = runtime.EventBus()
    adapter = adapter_module.FirmwareAdapter(DummyTransport(), bus)
    adapter._connected = True
    adapter._peer_capabilities = {"features": []}
    loop = asyncio.get_running_loop()
    pending_say = adapter_module._TxItem(
        protocol.encode_frame(protocol.MSG_SAY, b"\0" * 512),
        loop.create_future(),
    )
    pending_expr = adapter_module._TxItem(
        protocol.encode_frame(protocol.MSG_EXPR, protocol.encode_expr(1)),
        loop.create_future(),
    )
    pending_say_end = adapter_module._TxItem(
        protocol.encode_frame(protocol.MSG_SAY_END, protocol.encode_say_end(7)),
        loop.create_future(),
    )
    adapter._tx_queue.put_nowait(pending_say)
    adapter._tx_queue.put_nowait(pending_expr)
    adapter._tx_queue.put_nowait(pending_say_end)

    cancel_task = asyncio.create_task(adapter.send_speech_cancel(7))
    await asyncio.sleep(0)

    assert isinstance(pending_say.ack.exception(), ConnectionError)
    assert isinstance(pending_say_end.ack.exception(), ConnectionError)
    assert not pending_expr.ack.done()

    kept_expr = adapter._tx_queue.get_nowait()
    cancel_item = adapter._tx_queue.get_nowait()
    assert adapter_module._frame_type(kept_expr.frame) == protocol.MSG_EXPR
    assert adapter_module._frame_type(cancel_item.frame) == protocol.MSG_SPEECH_CANCEL

    kept_expr.ack.set_result(None)
    cancel_item.ack.set_result(None)
    adapter._tx_queue.task_done()
    adapter._tx_queue.task_done()
    await asyncio.wait_for(cancel_task, timeout=0.1)


def test_server_hello_declares_voice_contract() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    hello = protocol.decode_hello(protocol.encode_hello())

    assert hello["audio"] == {
        "format": "pcm16",
        "sample_rate": 16000,
        "channels": 1,
        "chunk_samples": 256,
    }
    assert hello["codecs"] == {"pcm16": True, "opus": False}
    assert hello["codec_options"] == {
        "opus_tx": True,
        "opus_default": False,
        "opus_sample_rate": 16000,
        "opus_channels": 1,
        "opus_frame_duration": 60,
        "opus_frame_samples": 960,
        "opus_bitrate": 32000,
    }
    assert hello["listen"]["mode"] == "auto"
    assert hello["listen"]["max_speech_ms"] == 9200
    assert hello["listen"]["max_utterance_samples"] == 192000


def test_server_metrics_exposes_last_voice_session() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 42,
        "outcome": "stt_rejected",
        "discard_reason": "stt_empty",
        "duration_ms": 736.2,
        "transcript_quality": "empty",
        "tts_chunks_sent": 23,
        "tts_pcm_bytes_sent": 11776,
        "tts_completed": True,
        "text_scroll_truncated": True,
        "secret": "nao deve aparecer",
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["last_voice_session"] == {
        "turn_id": 42,
        "outcome": "stt_rejected",
        "discard_reason": "stt_empty",
        "duration_ms": 736.2,
        "transcript_quality": "empty",
        "tts_chunks_sent": 23,
        "tts_pcm_bytes_sent": 11776,
        "tts_completed": True,
        "text_scroll_truncated": True,
    }
    assert payload["recent_voice_sessions"] == [payload["last_voice_session"]]
    assert payload["voice_alert"] == {
        "level": "warn",
        "title": "Turno de voz descartado",
        "detail": "stt_empty",
    }
    assert payload["voice_diagnosis"] == {
        "title": "Turno de voz descartado",
        "detail": "STT rejeitou ou degradou a transcrição",
        "next_check": "Comparar RMS, peak, clipping e amostra enviada ao STT.",
    }


def test_server_metrics_preserves_full_reply_for_tts_diagnostics() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    reply = "Resposta longa. " + ("texto completo " * 20)
    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 43,
        "outcome": "llm",
        "reply": reply,
        "transcript": "Me conte uma história longa.",
        "tts_completed": True,
        "text_scroll_truncated": True,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["last_voice_session"]["reply"] == reply
    assert payload["last_voice_session"]["transcript"] == "Me conte uma história longa."
    assert payload["last_voice_session"]["text_scroll_truncated"] is True


@pytest.mark.asyncio
async def test_server_tts_records_playback_completion_diagnostics(monkeypatch) -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_PAGE_INTERVAL_S", 0)

    class DummyTts:
        async def synthesize_stream(self, sentences):
            async for _sentence in sentences:
                yield b"\x11\x22" * 300

    class DummyAdapter:
        def __init__(self) -> None:
            self.texts: list[str] = []
            self.say_chunks: list[bytes] = []
            self.say_end: list[int] = []

        async def send_say_begin(self, turn_id: int) -> None:
            pass

        async def send_say(self, pcm: bytes) -> None:
            self.say_chunks.append(pcm)

        async def send_say_end(self, turn_id: int) -> None:
            self.say_end.append(turn_id)

        async def send_text_scroll(self, text: str) -> None:
            self.texts.append(text)

    bus = runtime.EventBus(default_maxsize=512)
    adapter = DummyAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: adapter,
        tts_provider=DummyTts(),
    )
    session = runtime.SessionContext(turn_id=77)
    session.reply_text = "Resposta longa " + ("x" * 180)

    await orchestrator._run_tts_and_speak(77, ["primeira frase"], session)
    for _ in range(5):
        await asyncio.sleep(0)

    assert len(adapter.say_chunks) == 2
    assert adapter.say_end == [77]
    assert adapter.texts
    assert len(adapter.texts[0].encode("utf-8")) <= 128
    assert session.meta["tts_sentence_count"] == 1
    assert session.meta["tts_chunks_sent"] == 2
    assert session.meta["tts_pcm_bytes_in"] == 600
    assert session.meta["tts_pcm_bytes_sent"] == 1024
    assert session.meta["tts_padding_bytes"] == 424
    assert session.meta["tts_completed"] is True
    assert session.meta["text_scroll_truncated"] is True


def test_server_text_scroll_pages_are_utf8_safe() -> None:
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    text = "Olá mundo. " + ("texto comprido " * 20) + "fim"
    pages = orchestrator_module._split_text_scroll_pages(text)

    assert len(pages) > 1
    assert " ".join(pages).replace(" .", ".") != ""
    assert all(len(page.encode("utf-8")) <= 128 for page in pages)
    assert all(len(page) <= 38 for page in pages)
    assert "Olá" in pages[0]


def test_server_text_scroll_pages_split_visually_wide_reply() -> None:
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    pages = orchestrator_module._split_text_scroll_pages(
        "A Terra é o nosso lar! Tem água, ar e muita vida incrível."
    )

    assert pages == [
        "A Terra é o nosso lar! Tem água, ar e",
        "muita vida incrível.",
    ]
    assert all(len(page.encode("utf-8")) <= 128 for page in pages)
    assert all(len(page) <= 38 for page in pages)


@pytest.mark.asyncio
async def test_server_reply_text_scroll_sends_paginated_pages(monkeypatch) -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class DummyAdapter:
        def __init__(self) -> None:
            self.texts: list[str] = []

        async def send_text_scroll(self, text: str) -> None:
            self.texts.append(text)

    adapter = DummyAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        runtime.EventBus(),
        _make_server_config(),
        get_adapter=lambda: adapter,
    )
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_PAGE_INTERVAL_S", 0)
    session = runtime.SessionContext(turn_id=78)
    session.reply_text = "Resposta longa. " + ("texto completo " * 20)

    await orchestrator._send_reply_text_scroll(session)

    assert len(adapter.texts) > 1
    assert all(len(page.encode("utf-8")) <= 128 for page in adapter.texts)
    assert session.meta["text_scroll_truncated"] is True
    assert session.meta["text_scroll_pages"] == len(adapter.texts)
    assert session.meta["text_scroll_pages_sent"] == len(adapter.texts)


def test_server_dashboard_renders_voice_diagnostics_panel() -> None:
    dashboard = importlib.import_module("noisebot_server.internal.ops.dashboard")

    html = dashboard.get_dashboard_html()

    assert "Diagnóstico de Voz" in html
    assert "voice-diagnosis" in html
    assert "renderVoiceDiagnostics" in html
    assert "Histórico recente" in html
    assert "voice_end_to_stt_start_ms" in html


def test_server_metrics_replaces_duplicate_voice_session_turn() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({"turn_id": 7, "outcome": "cancelled", "discard_reason": "barge_in"})
    store.record_voice_session({"turn_id": 7, "outcome": "ok", "duration_ms": 1800})

    api = api_module.MetricsApi(metrics_module.MetricsRegistry(), store)
    payload = api.get_metrics()

    assert payload["last_voice_session"] == {"turn_id": 7, "outcome": "ok", "duration_ms": 1800}
    assert payload["recent_voice_sessions"] == [payload["last_voice_session"]]
    assert payload["voice_alert"] is None

    api.reset()
    reset_payload = api.get_metrics()
    assert reset_payload["last_voice_session"] == {}
    assert reset_payload["recent_voice_sessions"] == []


def test_server_metrics_warns_when_tts_did_not_complete() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 9,
        "outcome": "llm",
        "reply_chars": 220,
        "tts_chunks_sent": 12,
        "tts_say_begin_sent": True,
        "tts_say_end_sent": False,
        "tts_completed": False,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] == {
        "level": "warn",
        "title": "Fala possivelmente incompleta",
        "detail": "TTS/playback não confirmou SAY_END",
    }
    assert payload["voice_diagnosis"] == {
        "title": "Fala possivelmente incompleta",
        "detail": "TTS/playback não confirmou envio completo de fala",
        "next_check": "Checar chunks SAY, SAY_BEGIN/SAY_END e cancelamentos durante playback.",
    }


def test_server_metrics_distinguishes_visual_text_scroll_truncation() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 10,
        "outcome": "llm",
        "reply_chars": 260,
        "tts_completed": True,
        "text_scroll_bytes": 260,
        "text_scroll_payload_bytes": 128,
        "text_scroll_truncated": True,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] is None
    assert payload["voice_diagnosis"] == {
        "title": "Turno de voz concluído",
        "detail": "texto visual foi truncado pelo limite de TEXT_SCROLL; áudio pode estar completo",
        "next_check": "Comparar reply_chars com tts_completed e duração esperada de fala.",
    }


def test_server_metrics_reports_paginated_text_scroll() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 11,
        "outcome": "llm",
        "reply_chars": 260,
        "tts_completed": True,
        "text_scroll_truncated": True,
        "text_scroll_pages": 3,
        "text_scroll_pages_sent": 3,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] is None
    assert payload["voice_diagnosis"] == {
        "title": "Turno de voz concluído",
        "detail": "texto visual longo foi paginado em TEXT_SCROLL; áudio pode estar completo",
        "next_check": "Confirmar no display se as páginas apareceram durante a fala.",
    }


def test_server_metrics_accepts_good_transcript_quality() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 8,
        "outcome": "llm",
        "duration_ms": 9584,
        "transcript_quality": "good",
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] is None


def _server_loud_pcm(samples: int = 256, amplitude: int = 3200) -> bytes:
    return b"".join(struct.pack("<h", amplitude if i % 2 == 0 else -amplitude) for i in range(samples))


async def _simulate_server_voice_session(bus, runtime, chunks: int = 40) -> None:
    await bus.publish(runtime.WakeDetected())
    await asyncio.sleep(0)
    pcm = _server_loud_pcm()
    for seq in range(chunks):
        await bus.publish(runtime.AudioChunkIn(pcm=pcm, seq=seq))
    await asyncio.sleep(0)
    await bus.publish(runtime.VoiceActivityEnd())


async def _wait_until(predicate, timeout_s: float = 1.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout_s
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            raise AssertionError("condicao nao atendida dentro do timeout")
        await asyncio.sleep(0.01)


async def _drain_queue(queue: asyncio.Queue, duration_s: float = 0.05) -> list:
    items = []
    deadline = asyncio.get_running_loop().time() + duration_s
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            return items
        try:
            items.append(await asyncio.wait_for(queue.get(), timeout=remaining))
        except asyncio.TimeoutError:
            return items


async def test_server_discards_overlong_voice_before_stt() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class MockStt:
        def __init__(self) -> None:
            self.finalize_calls = 0

        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            self.finalize_calls += 1
            return runtime.FinalTranscript(
                turn_id=turn_id,
                text="nao deveria transcrever",
                quality=runtime.TranscriptQuality.GOOD,
            )

        async def close(self) -> None:
            pass

        async def reset(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockStt()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(max_utterance_samples=512),
        get_adapter=lambda: None,
        stt_provider=stt,
    )
    intents = bus.subscribe(runtime.IntentResolved)
    task = asyncio.create_task(orchestrator.run())

    try:
        await bus.publish(runtime.WakeDetected())
        await asyncio.sleep(0)
        await bus.publish(runtime.AudioChunkIn(pcm=_server_loud_pcm(samples=768), seq=0))
        await _wait_until(lambda: orchestrator._session is None)
        await bus.publish(runtime.VoiceActivityEnd())
        await asyncio.sleep(0.05)

        assert stt.finalize_calls == 0
        assert await _drain_queue(intents) == []
    finally:
        await orchestrator.shutdown()
        await asyncio.wait_for(task, timeout=1.0)


async def test_server_empty_wake_prompt_is_one_shot_until_real_speech() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class MockSequenceStt:
        def __init__(self, results) -> None:
            self._results = list(results)
            self.finalize_calls = 0

        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            self.finalize_calls += 1
            text, quality = self._results.pop(0)
            return runtime.FinalTranscript(turn_id=turn_id, text=text, quality=quality)

        async def close(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockSequenceStt([
        ("", runtime.TranscriptQuality.EMPTY),
        ("", runtime.TranscriptQuality.EMPTY),
        ("olá", runtime.TranscriptQuality.GOOD),
        ("", runtime.TranscriptQuality.EMPTY),
    ])
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        stt_provider=stt,
    )
    intents = bus.subscribe(runtime.IntentResolved)
    speech_done = bus.subscribe(runtime.SpeechDone)
    task = asyncio.create_task(orchestrator.run())

    try:
        await _simulate_server_voice_session(bus, runtime)
        first_intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert first_intent.intent_name == "local_empty_wake_prompt"

        await _simulate_server_voice_session(bus, runtime)
        await _wait_until(
            lambda: stt.finalize_calls >= 2 and orchestrator._session is None
        )
        assert await _drain_queue(intents) == []

        await _simulate_server_voice_session(bus, runtime)
        greeting_intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert greeting_intent.intent_name == "local_greeting"

        await _simulate_server_voice_session(bus, runtime)
        second_prompt = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert second_prompt.intent_name == "local_empty_wake_prompt"
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def test_server_unclear_transcript_asks_user_to_repeat() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class MockStt:
        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def reset(self) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            return runtime.FinalTranscript(
                turn_id=turn_id,
                text="eslopo diaforo",
                quality=runtime.TranscriptQuality.LOW_LOGPROB,
                avg_logprob=-1.8,
            )

        async def close(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        stt_provider=MockStt(),
    )
    intents = bus.subscribe(runtime.IntentResolved)
    speech_done = bus.subscribe(runtime.SpeechDone)
    task = asyncio.create_task(orchestrator.run())

    try:
        await _simulate_server_voice_session(bus, runtime)
        intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)

        assert intent.intent_name == "local_unclear_transcript_prompt"
        assert intent.reply_text == "Eu ouvi, mas não entendi direito. Pode repetir?"
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def test_server_unclear_transcript_does_not_loop_on_noise() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class MockSequenceStt:
        def __init__(self) -> None:
            self.finalize_calls = 0

        async def initialize(self) -> None:
            pass

        def feed(self, pcm: bytes) -> None:
            pass

        async def reset(self) -> None:
            pass

        async def finalize(self, full_pcm: bytes, turn_id: int):
            self.finalize_calls += 1
            if self.finalize_calls == 1:
                return runtime.FinalTranscript(
                    turn_id=turn_id,
                    text="eslopo diaforo",
                    quality=runtime.TranscriptQuality.LOW_LOGPROB,
                    no_speech_prob=0.20,
                    avg_logprob=-1.8,
                )
            return runtime.FinalTranscript(
                turn_id=turn_id,
                text="E ai",
                quality=runtime.TranscriptQuality.LOW_LOGPROB,
                no_speech_prob=0.72,
                avg_logprob=-1.4,
            )

        async def close(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockSequenceStt()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        stt_provider=stt,
    )
    intents = bus.subscribe(runtime.IntentResolved)
    speech_done = bus.subscribe(runtime.SpeechDone)
    task = asyncio.create_task(orchestrator.run())

    try:
        await _simulate_server_voice_session(bus, runtime)
        intent = await asyncio.wait_for(intents.get(), timeout=1.0)
        await asyncio.wait_for(speech_done.get(), timeout=1.0)
        await _wait_until(lambda: orchestrator._session is None)
        assert intent.intent_name == "local_unclear_transcript_prompt"

        await _simulate_server_voice_session(bus, runtime)
        await _wait_until(
            lambda: stt.finalize_calls >= 2 and orchestrator._session is None
        )
        assert await _drain_queue(intents) == []
    finally:
        await orchestrator.shutdown()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def test_server_barge_in_starts_clean_listening_turn_even_if_cancel_fails() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    class FailingCancelAdapter:
        async def send_speech_cancel(self, turn_id: int) -> None:
            raise ConnectionError(f"cancel failed {turn_id}")

    bus = runtime.EventBus(default_maxsize=512)
    store = status_module.StatusStore()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: FailingCancelAdapter(),
        status_store=store,
    )
    old_session = runtime.SessionContext(turn_id=123)
    old_session.final_text = "fala antiga"
    old_session.reply_text = "resposta antiga"
    old_session.intent_name = "llm_reply"
    old_session.set_deadline(30)
    orchestrator._session = old_session
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=old_session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=old_session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=old_session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.SPEAKING, turn_id=old_session.turn_id)
    orchestrator._turn_task = asyncio.create_task(asyncio.sleep(10))
    orchestrator._watchdog = asyncio.create_task(orchestrator._run_watchdog(old_session))

    try:
        await orchestrator._on_barge_in(runtime.BargeInDetected(turn_id=old_session.turn_id))

        assert orchestrator._fsm.is_listening
        assert orchestrator._session is not None
        assert orchestrator._session.turn_id != old_session.turn_id
        assert orchestrator._watchdog is not None
        assert not orchestrator._watchdog.done()
        assert store.last_voice_session["turn_id"] == old_session.turn_id
        assert store.last_voice_session["outcome"] == "interrupted"
        assert store.last_voice_session["discard_reason"] == "barge_in"
    finally:
        await orchestrator.shutdown()


async def test_server_wake_during_speaking_is_barge_in_signal() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    bus = runtime.EventBus(default_maxsize=512)
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
    )
    barge_events = bus.subscribe(runtime.BargeInDetected)
    session = runtime.SessionContext(turn_id=321)
    orchestrator._session = session
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.SPEAKING, turn_id=session.turn_id)

    try:
        await orchestrator._on_wake(runtime.WakeDetected())
        event = await asyncio.wait_for(barge_events.get(), timeout=1.0)
        assert event.turn_id == session.turn_id
    finally:
        await orchestrator.shutdown()


async def test_server_listening_watchdog_timeout_finishes_without_session_error() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    class CapturingAdapter:
        def __init__(self) -> None:
            self.sessions = []

        async def send_session(self, payload: dict) -> None:
            self.sessions.append(payload)

        async def send_gaze(self, x: float, y: float) -> None:
            pass

        async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
            pass

        async def send_led(self, mode: int, r: int, g: int, b: int) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    adapter = CapturingAdapter()
    store = status_module.StatusStore()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: adapter,
        status_store=store,
    )
    session = runtime.SessionContext(turn_id=321)
    session.set_deadline(-0.1)
    orchestrator._session = session
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=session.turn_id)

    await orchestrator._run_watchdog(session)

    assert orchestrator._session is None
    assert orchestrator._fsm.state == runtime.TurnState.IDLE
    assert store.last_voice_session["turn_id"] == 321
    assert store.last_voice_session["outcome"] == "listen_timeout"
    assert store.last_voice_session["discard_reason"] == "listen_timeout"
    assert adapter.sessions[-1] == {
        "event": "FOLLOWUP_CANCEL",
        "turn_id": 321,
        "reason": "listen_timeout",
    }


async def test_server_speech_done_arms_followup_only_for_real_question() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class CapturingAdapter:
        def __init__(self) -> None:
            self.sessions = []

        async def send_session(self, payload: dict) -> None:
            self.sessions.append(payload)

        async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
            pass

        async def send_action(self, action_id: int) -> None:
            pass

        async def send_emot_event(self, event_id: int) -> None:
            pass

        async def send_gaze(self, x: float, y: float) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    adapter = CapturingAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: adapter,
    )

    question = runtime.SessionContext(turn_id=301)
    question.intent_name = "llm_reply"
    question.reply_text = "Quer que eu continue?"
    question.meta["outcome"] = "llm"
    orchestrator._session = question
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=question.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=question.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=question.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.SPEAKING, turn_id=question.turn_id)

    await orchestrator._on_speech_done(runtime.SpeechDone(turn_id=question.turn_id))

    assert adapter.sessions == [{
        "event": "FOLLOWUP_ARM",
        "turn_id": 301,
        "window_ms": 8000,
        "source": "llm_reply",
    }]

    prompt = runtime.SessionContext(turn_id=302)
    prompt.intent_name = "local_empty_wake_prompt"
    prompt.reply_text = "Oi! Em que posso ajudar?"
    prompt.meta["outcome"] = "local_intent"
    orchestrator._session = prompt
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=prompt.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=prompt.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=prompt.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.SPEAKING, turn_id=prompt.turn_id)

    await orchestrator._on_speech_done(runtime.SpeechDone(turn_id=prompt.turn_id))

    assert adapter.sessions[-1] == {
        "event": "SESSION_DONE",
        "turn_id": 302,
        "outcome": "local_intent",
    }


async def test_server_turn_error_sends_session_error_contract() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class CapturingAdapter:
        def __init__(self) -> None:
            self.sessions = []

        async def send_session(self, payload: dict) -> None:
            self.sessions.append(payload)

    bus = runtime.EventBus(default_maxsize=512)
    adapter = CapturingAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: adapter,
    )
    session = runtime.SessionContext(turn_id=401)
    orchestrator._session = session
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=session.turn_id)

    await orchestrator._on_turn_error(
        runtime.TurnError(turn_id=session.turn_id, stage="llm", reason="timeout")
    )

    assert adapter.sessions[-1] == {
        "event": "SESSION_ERROR",
        "turn_id": 401,
        "stage": "llm",
        "reason": "timeout",
    }


def test_server_transport_factory_creates_uart_transport() -> None:
    factory_module = importlib.import_module(
        "noisebot_server.internal.transport.factory"
    )
    config = _make_server_config(uart="COM9")

    transport = factory_module.create_transport_factory(config)()

    assert transport.description == "UART COM9@1000000"


def test_server_connection_supervisor_backoff_caps() -> None:
    transport = importlib.import_module("noisebot_server.internal.transport")
    config = _make_server_config()
    supervisor = transport.ConnectionSupervisor(
        transport_factory=lambda: transport.TcpTransport("127.0.0.1"),
        bus=object(),
        reconnect=config.reconnect,
    )

    assert supervisor._next_delay(0.2) == 0.2


def test_server_ops_status_store_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_ops = importlib.import_module("noisebot_server.internal.ops")
    bridge_status = importlib.import_module("bridgev2.ops.status_store")

    assert server_ops.StatusStore is not bridge_status.StatusStore

    store = server_ops.StatusStore()
    bridge_store = bridge_status.StatusStore()
    store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )
    bridge_store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )

    assert "sk-abcdef1234567890" not in store.last_transcript
    assert "<redacted>" in store.last_transcript
    assert store.last_transcript == bridge_store.last_transcript


def test_server_ops_exports_bridge_compatible_schemas() -> None:
    _ensure_bridgev2_path()

    server_schemas = importlib.import_module("noisebot_server.internal.ops.schemas")
    bridge_schemas = importlib.import_module("bridgev2.ops.schemas")

    assert server_schemas.ok_response("feito") == bridge_schemas.ok_response("feito")
    assert server_schemas.error_response("falha", 503) == (
        bridge_schemas.error_response("falha", 503)
    )


def test_server_ops_config_controller_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_config = importlib.import_module("noisebot_server.internal.ops.config")
    bridge_config = importlib.import_module("bridgev2.ops.config_controller")

    assert server_config.ConfigController is not bridge_config.ConfigController
    assert server_config.PROVIDER_CATALOG == bridge_config.PROVIDER_CATALOG
    assert server_config.VALID_MODES == bridge_config.VALID_MODES
    assert "ollama" in server_config.PROVIDER_CATALOG
    assert "local_only" in server_config.VALID_MODES


def test_server_app_state_persists_routine_and_basic_settings(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    state_path = tmp_path / "app_state.json"

    store = app_state.AppStateStore(state_path)
    timer = store.create_agenda_item("timer", {"title": "Cafe", "duration_min": 5})
    settings = store.update_basic_settings(
        {
            "volume": 88,
            "display_brightness": 42,
            "led_brightness": 17,
            "night_mode": True,
        }
    )

    assert timer["kind"] == "timer"
    assert timer["status"] == "ativo"
    assert settings["volume"] == 88
    assert settings["display_brightness"] == 42
    assert settings["night_mode"] is True

    reloaded = app_state.AppStateStore(state_path)
    snapshot = reloaded.snapshot()

    assert snapshot["routine"]["summary"]["timers"] == 1
    assert snapshot["routine"]["items"][0]["title"] == "Cafe"
    assert snapshot["settings"]["volume"] == 88


def test_server_app_state_persists_profile_and_advanced_settings(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    state_path = tmp_path / "app_state.json"

    store = app_state.AppStateStore(state_path)
    profile = store.update_profile({
        "assistant_name": "Nina",
        "language": "pt-BR",
        "response_tone": "expressivo",
    })
    advanced = store.update_advanced_settings({
        "wifi_ssid": "NoiseNet",
        "bridge_host": "192.168.1.30",
        "bridge_port": 9000,
        "ota_channel": "manual",
        "log_level": "DEBUG",
        "servos_enabled": True,
    })

    assert profile["assistant_name"] == "Nina"
    assert profile["response_tone"] == "expressivo"
    assert advanced["bridge_port"] == 9000
    assert advanced["servos_enabled"] is True

    reloaded = app_state.AppStateStore(state_path)
    snapshot = reloaded.snapshot()

    assert snapshot["profile"]["assistant_name"] == "Nina"
    assert snapshot["advanced"]["wifi_ssid"] == "NoiseNet"
    assert snapshot["advanced"]["ota_channel"] == "manual"


def test_server_app_state_updates_and_deletes_agenda_items(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")

    alarm = store.create_agenda_item("alarm", {"title": "Acordar", "time": "07:00"})
    updated = store.update_agenda_item(alarm["id"], {"enabled": False})

    assert updated is not None
    assert updated["enabled"] is False
    assert updated["status"] == "desligado"
    assert store.list_agenda()["summary"]["alarms"] == 0
    assert store.delete_agenda_item(alarm["id"]) is True
    assert store.delete_agenda_item(alarm["id"]) is False


def test_server_app_state_maps_alarm_repeat_to_firmware_mask(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")

    daily = store.create_agenda_item("alarm", {"title": "Todo dia", "repeat": "diário"})
    weekdays = store.create_agenda_item("alarm", {"title": "Trabalho", "repeat": "dias úteis"})
    weekend = store.create_agenda_item("alarm", {"title": "Folga", "repeat": "fim de semana"})
    updated = store.update_agenda_item(daily["id"], {"repeat": "uma vez", "time": "08:15"})

    assert daily["weekdays_mask"] == 0x7F
    assert weekdays["weekdays_mask"] == 0x3E
    assert weekend["weekdays_mask"] == 0x41
    assert updated is not None
    assert updated["weekdays_mask"] == 0
    assert updated["detail"] == "uma vez, 08:15"


def test_server_app_state_imports_firmware_agenda_without_duplicates(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")
    store.create_agenda_item("alarm", {"title": "Remedio", "time": "08:05"})

    imported = store.import_firmware_agenda({
        "alarms": [
            {
                "id": 2,
                "label": "Remedio",
                "hour": 8,
                "minute": 5,
                "weekdays_mask": 0,
                "enabled": True,
            }
        ],
        "timers": [
            {
                "id": 1,
                "label": "Cafe",
                "duration_ms": 300000,
                "remaining_ms": 120000,
            }
        ],
        "reminders": [],
    })
    imported_again = store.import_firmware_agenda({
        "alarms": [
            {
                "id": 2,
                "label": "Remedio",
                "hour": 8,
                "minute": 5,
                "weekdays_mask": 0,
                "enabled": False,
            }
        ],
        "timers": [],
        "reminders": [],
    })

    agenda = store.list_agenda()

    assert imported == 2
    assert imported_again == 1
    assert agenda["summary"]["alarms"] == 0
    assert agenda["summary"]["timers"] == 0
    assert len(agenda["items"]) == 1
    assert agenda["items"][0]["id"] == "fw_alarm_2"
    assert agenda["items"][0]["source"] == "firmware"
    assert agenda["items"][0]["firmware_id"] == 2
    assert agenda["items"][0]["time"] == "08:05"
    assert agenda["items"][0]["enabled"] is False

    assert store.import_firmware_agenda({"alarms": [], "timers": [], "reminders": []}) == 0
    assert store.list_agenda()["items"] == []


def test_server_ops_serves_app_dist_when_available(tmp_path, monkeypatch) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "index.html").write_text("<div>NoiseBot App</div>", encoding="utf-8")

    monkeypatch.setenv("NOISEBOT_APP_DIST", str(dist))

    assert http._find_app_dist() == dist.resolve()


def test_server_agenda_payload_recreates_edited_alarm() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    payload = http._agenda_session_payload(
        {
            "kind": "alarm",
            "title": "Remedio",
            "time": "08:15",
            "weekdays_mask": 0x3E,
            "enabled": True,
            "firmware_id": 2,
        },
        "recreate",
    )

    assert isinstance(payload, list)
    assert payload[0]["action"] == "alarm_cancel"
    assert payload[0]["id"] == 2
    assert payload[1]["action"] == "alarm_create"
    assert payload[1]["weekdays_mask"] == 0x3E


def test_server_agent_orchestrator_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_agent = importlib.import_module("noisebot_server.internal.agent")
    bridge_orchestrator = importlib.import_module("bridgev2.runtime.orchestrator")

    assert server_agent.Orchestrator is not bridge_orchestrator.Orchestrator


def test_server_agent_runtime_is_server_owned() -> None:
    _ensure_bridgev2_path()

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    bridge_bus = importlib.import_module("bridgev2.runtime.bus")
    bridge_events = importlib.import_module("bridgev2.runtime.events")
    bridge_turn = importlib.import_module("bridgev2.runtime.turn_manager")

    assert runtime.EventBus is not bridge_bus.EventBus
    assert runtime.VoiceEndReason.SILENCE.value == bridge_events.VoiceEndReason.SILENCE.value
    assert runtime.TurnState.IDLE.name == bridge_turn.TurnState.IDLE.name


def test_server_agent_turn_manager_keeps_transition_rules() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    manager = runtime.TurnManager()

    manager.transition(runtime.TurnState.LISTENING, turn_id=42)
    manager.transition(runtime.TurnState.COMMITTING_TURN)
    manager.transition(runtime.TurnState.THINKING)

    assert manager.current_turn_id == 42
    assert manager.can_interrupt is True
    assert manager.try_transition(runtime.TurnState.COMMITTING_TURN) is False
    assert manager.state == runtime.TurnState.THINKING


def test_server_agent_local_intent_matches_time() -> None:
    _ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("que horas sao", turn_id=44)

    assert result.intent_name == "local_time"
    assert result.reply_text


def test_server_agent_local_intent_answers_curiosity_in_pt_br() -> None:
    _ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("me conte uma curiosidade", turn_id=44)

    assert result.intent_name == "local_curiosity_fact"
    assert result.reply_text
    assert "Curiosidade:" in result.reply_text
    assert "idioma errado" not in result.reply_text
    assert result.expression_id == 4


def test_server_agent_llm_and_intents_are_server_owned() -> None:
    _ensure_bridgev2_path()

    server_agent = importlib.import_module("noisebot_server.internal.agent")
    server_llm = importlib.import_module("noisebot_server.internal.agent.llm")
    bridge_llm = importlib.import_module("bridgev2.llm.base")
    bridge_intents = importlib.import_module("bridgev2.llm.local_intent")

    assert server_llm.StreamingLLMProvider is not bridge_llm.StreamingLLMProvider
    assert server_agent.LocalIntentProvider is not bridge_intents.LocalIntentProvider


def test_server_agent_stt_tts_are_server_owned() -> None:
    _ensure_bridgev2_path()

    server_stt = importlib.import_module("noisebot_server.internal.agent.stt")
    bridge_stt = importlib.import_module("bridgev2.stt.base")
    server_tts = importlib.import_module("noisebot_server.internal.agent.tts")
    bridge_tts = importlib.import_module("bridgev2.tts.base")

    assert server_stt.STTProvider is not bridge_stt.STTProvider
    assert server_tts.TTSProvider is not bridge_tts.TTSProvider


def test_server_agent_sentencizer_keeps_bridge_behavior() -> None:
    _ensure_bridgev2_path()

    agent = importlib.import_module("noisebot_server.internal.agent")
    sentencizer = agent.Sentencizer()

    sentences = list(sentencizer.feed("Ola. Tudo bem?")) + list(sentencizer.flush())

    assert sentences == ["Ola. Tudo bem?"]


def test_server_vision_is_server_owned() -> None:
    _ensure_bridgev2_path()

    server_vision = importlib.import_module("noisebot_server.internal.vision")
    bridge_vision = importlib.import_module("bridgev2.vision")

    assert server_vision.VisionClient is not bridge_vision.VisionClient
    assert server_vision.VisionObservation is not bridge_vision.VisionObservation
    assert server_vision.FaceBox is not bridge_vision.FaceBox


def test_server_vision_observation_parses_firmware_payload() -> None:
    vision = importlib.import_module("noisebot_server.internal.vision")

    observation = vision.VisionObservation.from_payload({
        "ok": True,
        "observation": {
            "valid": True,
            "scene": "normal",
            "timestamp_ms": 1234,
            "width": 640,
            "height": 480,
            "jpeg_bytes": 54233,
            "capture_ms": 897,
            "luma_avg": 122,
            "luma_min": 0,
            "luma_max": 255,
            "contrast": 255,
            "motion_score": 5,
        },
    })

    assert observation.valid is True
    assert observation.width == 640
    assert observation.height == 480
    assert observation.scene == "normal"


def test_server_vision_face_center_normalization() -> None:
    vision = importlib.import_module("noisebot_server.internal.vision")

    observation = vision.VisionObservation.from_payload({
        "valid": True,
        "scene": "normal",
        "width": 640,
        "height": 480,
    })
    face = vision.FaceBox(x=240, y=120, width=160, height=120)
    analysis = vision.VisionAnalysis(
        observation=observation,
        detector="test",
        detector_available=True,
        face_detected=True,
        face_count=1,
        primary_face=face,
    )

    assert analysis.face_center_norm_x == 0.0
    assert analysis.face_center_norm_y == -0.25


def test_server_app_contract_exposes_only_server_paths() -> None:
    api = importlib.import_module("noisebot_server.api")

    endpoints = api.default_app_contract()

    assert endpoints
    assert all(endpoint.path.startswith("/") for endpoint in endpoints)
    assert all(not endpoint.path.startswith("http://") for endpoint in endpoints)
    assert all(not endpoint.path.startswith("https://") for endpoint in endpoints)


def test_server_app_contract_tracks_implemented_endpoints() -> None:
    api = importlib.import_module("noisebot_server.api")

    implemented = api.implemented_endpoints()
    paths = {(endpoint.method, endpoint.path) for endpoint in implemented}

    assert ("GET", "/health") in paths
    assert ("GET", "/ai/status") in paths
    assert ("POST", "/debug/transcript") in paths
    assert all(endpoint.implemented for endpoint in implemented)


def test_server_app_contract_reserves_future_domains() -> None:
    api = importlib.import_module("noisebot_server.api")

    domains = {endpoint.domain for endpoint in api.default_app_contract()}

    assert {"ops", "vision", "agent", "device", "agenda"}.issubset(domains)


def test_server_recent_log_buffer_redacts_and_limits() -> None:
    log_buffer = importlib.import_module("noisebot_server.internal.ops.log_buffer")
    buffer = log_buffer.RecentLogBuffer(max_entries=2)

    for index in range(3):
        record = logging.LogRecord(
            "noisebot.test",
            logging.INFO,
            __file__,
            1,
            "evento %d token=sk-secret-token-value-%d",
            (index, index),
            None,
        )
        record.created = float(index + 1)
        buffer.append_record(record)

    entries = buffer.recent(limit=10)

    assert buffer.count == 2
    assert [entry["ts"] for entry in entries] == [3.0, 2.0]
    assert all("sk-secret" not in entry["message"] for entry in entries)
    assert all("token=<redacted>" in entry["message"] for entry in entries)
