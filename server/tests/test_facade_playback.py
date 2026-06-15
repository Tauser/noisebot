from __future__ import annotations
import asyncio
import importlib
import io
import json
import logging
import math
import struct
from pathlib import Path
from urllib.error import HTTPError
import pytest

from _facade_common import _drain_queue, _make_server_config, _server_loud_pcm, _simulate_server_voice_session, _wait_until


def test_stt_repetition_loop_guard_detects_whisper_hallucination() -> None:
    stt = importlib.import_module("noisebot_server.internal.agent.stt")

    assert stt._looks_like_repetition_loop(
        "o que e que e que e que e que e que e que e que e que e que e"
    )
    assert not stt._looks_like_repetition_loop("acenda a luz da mesa por favor")

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

def test_server_cli_runs_debug_transcript_live(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    manual = importlib.import_module("noisebot_server.internal.debug.manual")

    calls: dict[str, object] = {}

    def fake_run_live_transcript_debug(
        *,
        text: str,
        server_url: str,
        turn_id: int,
        token: str,
        emit_json: bool,
    ) -> int:
        calls["text"] = text
        calls["server_url"] = server_url
        calls["turn_id"] = turn_id
        calls["token"] = token
        calls["emit_json"] = emit_json
        return 9

    monkeypatch.setattr(manual, "run_live_transcript_debug", fake_run_live_transcript_debug)

    try:
        cli.main([
            "debug",
            "transcript-live",
            "que horas são?",
            "--server-url",
            "http://127.0.0.1:8765",
            "--turn-id",
            "77",
            "--token",
            "secret",
            "--json",
        ])
    except SystemExit as exc:
        assert exc.code == 9
    else:
        raise AssertionError("debug command must exit with helper return code")

    assert calls == {
        "text": "que horas são?",
        "server_url": "http://127.0.0.1:8765",
        "turn_id": 77,
        "token": "secret",
        "emit_json": True,
    }

def test_server_live_transcript_debug_posts_to_ops_http(monkeypatch, capsys) -> None:
    manual = importlib.import_module("noisebot_server.internal.debug.manual")

    captured: dict[str, object] = {}

    class FakeResponse:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def read(self) -> bytes:
            return b'{"status":"ok","turn_id":77}'

    def fake_urlopen(request, timeout: float):
        captured["url"] = request.full_url
        captured["timeout"] = timeout
        captured["headers"] = dict(request.header_items())
        captured["body"] = request.data.decode("utf-8")
        return FakeResponse()

    monkeypatch.setattr(manual, "urlopen", fake_urlopen)

    code = manual.run_live_transcript_debug(
        text="olá, são 10h",
        server_url="http://127.0.0.1:8765/",
        turn_id=77,
        token="secret",
    )

    assert code == 0
    assert captured["url"] == "http://127.0.0.1:8765/debug/transcript"
    assert captured["timeout"] == 5.0
    assert captured["headers"]["Authorization"] == "Bearer secret"
    assert json.loads(captured["body"]) == {"text": "olá, são 10h", "turn_id": 77}
    assert "transcript injetado: turn_id=77" in capsys.readouterr().out

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
        playback_config={
            "firmware_say_queue_target": 4,
            "say_send_interval_ms": 16.0,
            "chunk_duration_ms": 16.0,
            "startup_chunks": 0,
            "startup_interval_ms": 24.0,
        },
    )

    assert payload["audio"]["format"] == "opus"
    assert payload["codecs"] == {"pcm16": False, "opus": True}
    assert payload["codec_options"] == {"opus_tx": True, "opus_default": False}
    assert payload["firmware"]["codec_options"] == payload["codec_options"]
    assert payload["features"] == ["voice_session_v2", "opus_tx"]
    assert payload["firmware"]["features"] == payload["features"]
    assert payload["tts_output_scheduler"]["firmware_say_queue_target"] == 4
    assert payload["tts_output_scheduler"]["say_send_interval_ms"] == 16.0
    assert (
        payload["tts_output_scheduler"]["say_send_interval_ms"]
        == payload["tts_output_scheduler"]["chunk_duration_ms"]
    )

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
    assert client.audio_codec_v2_health()["ok"]
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
    assert get_paths == ["api/audio/codec-v2", "api/audio/codec-v2"]
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

def test_server_firmware_diag_client_exposes_playback_v2_owner_endpoint(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []
    post_paths: list[str] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True, "speaker_owner_active": False}

    def fake_post_json(self, path, payload=None):
        post_paths.append(path)
        return {"ok": True, "speaker_owner_active": False}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    assert client.audio_playback_v2_status()["ok"]
    assert client.audio_playback_v2_speaker_owner_arm()["ok"]
    assert client.audio_playback_v2_speaker_owner_disarm()["ok"]
    assert client.audio_playback_v2_speaker_owner_real_arm()["ok"]
    assert client.audio_playback_v2_speaker_owner_real_disarm()["ok"]
    assert get_paths == ["api/audio/playback-v2"]
    assert post_paths == [
        "api/audio/playback-v2/speaker-owner/arm",
        "api/audio/playback-v2/speaker-owner/disarm",
        "api/audio/playback-v2/speaker-owner/real-arm",
        "api/audio/playback-v2/speaker-owner/real-disarm",
    ]

def test_server_codec_v2_health_flags_transport_issues() -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")

    health = firmware_diag.codec_v2_health_from_status({
        "ok": True,
        "format": "opus",
        "worker_active": False,
        "worker_state": "stopped",
        "packet_drops": 2,
        "opus_egress_packet_drops": 1,
        "opus_egress_queue_count": 3,
        "opus_codec_error": -1,
        "error": "ESP_OK",
    })

    assert health["ok"] is True
    assert health["healthy"] is False
    assert health["status"] == "degraded"
    assert "transporte Opus ativo sem worker ativo" in health["issues"]
    assert "packet_drops=2" in health["issues"]
    assert "opus_egress_packet_drops=1" in health["issues"]
    assert "opus_codec_error=-1" in health["issues"]
    assert health["warnings"] == ["opus_egress_queue_count=3"]
    assert "NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16" in health["rollback_hint"]

def test_server_codec_v2_health_accepts_clean_opus_status() -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")

    health = firmware_diag.codec_v2_health_from_status({
        "ok": True,
        "format": "opus",
        "worker_active": True,
        "worker_state": "running",
        "packet_drops": 0,
        "opus_egress_packet_drops": 0,
        "opus_egress_queue_count": 0,
        "opus_codec_error": 0,
        "error": "ESP_OK",
    })

    assert health["healthy"] is True
    assert health["status"] == "ok"
    assert health["issues"] == []
    assert health["warnings"] == []

def test_server_cli_parses_playback_v2_delta_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "delta",
        "--no-prompt",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "playback-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "delta"
    assert args.no_prompt
    assert args.json

def test_server_cli_parses_playback_v2_speaker_owner_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-arm",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "playback-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "speaker-owner-arm"
    assert args.json

    real_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-real-arm",
        "--json",
    ])

    assert real_args.action == "speaker-owner-real-arm"
    assert real_args.json

def test_server_cli_parses_playback_v2_speaker_owner_gate_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-gate",
        "--wait-s",
        "1",
        "--require-say",
        "--min-say-chunks",
        "10",
        "--no-prompt",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "playback-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "speaker-owner-gate"
    assert args.wait_s == 1.0
    assert args.require_say
    assert args.min_say_chunks == 10
    assert args.no_prompt
    assert args.json

def test_server_cli_parses_playback_v2_real_window_gate_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-real-window-gate",
        "--wait-s",
        "1",
        "--min-say-chunks",
        "10",
        "--no-prompt",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "playback-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "speaker-owner-real-window-gate"
    assert args.wait_s == 1.0
    assert args.min_say_chunks == 10
    assert args.no_prompt
    assert args.json

def test_server_playback_v2_real_window_gate_rolls_back(monkeypatch) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    calls: list[str] = []

    class FakeClient:
        def audio_playback_v2_speaker_owner_arm(self):
            calls.append("arm")
            return {
                "ok": True,
                "speaker_owner_dry_run_enabled": True,
            }

        def audio_playback_v2_speaker_owner_real_arm(self):
            calls.append("real-arm")
            return {
                "ok": True,
                "speaker_owner_real_armed": True,
            }

        def audio_playback_v2_speaker_owner_disarm(self):
            calls.append("disarm")
            return {
                "ok": True,
                "speaker_owner_dry_run_enabled": False,
            }

    deltas = iter([
        {
            "ok": True,
            "issues": [],
            "warnings": [],
            "counter_reset_detected": False,
            "after": {
                "speaker_owner_dry_run_enabled": True,
                "speaker_owner_candidate": True,
                "speaker_owner_handoff_ready": True,
                "speaker_owner_block_reason": "NONE",
                "speaker_owner_failures": 0,
                "speaker_owner_recoveries": 0,
                "speaker_owner_active": True,
                "speaker_owner_frames": 12,
                "speaker_owner_samples": 3072,
                "speaker_owner_silence_frames": 0,
            },
            "deltas": {
                "say_chunks_received": 12,
                "say_chunks_played": 12,
                "say_chunks_dropped": 0,
            },
        },
        {
            "ok": True,
            "issues": [],
            "warnings": [],
            "after": {
                "speaker_owner_real_armed": False,
                "speaker_owner_real_window_completed": True,
                "speaker_owner_real_auto_disarm_count": 1,
                "speaker_owner_real_write_frames": 12,
                "speaker_owner_real_write_samples": 3072,
                "speaker_owner_real_write_failures": 0,
            },
            "deltas": {
                "say_chunks_received": 12,
                "say_chunks_played": 12,
                "say_chunks_dropped": 0,
            },
        },
    ])

    def fake_delta(client, **kwargs):
        calls.append("delta")
        return next(deltas)

    monkeypatch.setattr(cli, "_run_playback_v2_delta", fake_delta)

    payload = cli._run_playback_v2_speaker_owner_real_window_gate(
        FakeClient(),
        no_prompt=True,
        wait_s=1.0,
        min_say_chunks=10,
    )

    assert payload["ok"] is True
    assert payload["owner_real_window_gate"] is True
    assert payload["real_window_completed"] is True
    assert payload["real_write_frames"] == 12
    assert payload["real_say_chunks_received_delta"] == 12
    assert payload["disarmed"]["ok"] is True
    assert calls == ["arm", "delta", "real-arm", "delta", "disarm"]

def test_server_cli_runs_playback_v2_delta_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 16,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 3,
            "say_chunks_dropped_listening": 1,
            "say_chunks_cancelled": 2,
            "say_cancel_count": 1,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 16,
            "say_chunks_received": 180,
            "say_chunks_played": 180,
            "say_chunks_dropped": 3,
            "say_chunks_dropped_listening": 1,
            "say_chunks_cancelled": 2,
            "say_cancel_count": 1,
            "speaker_write_requests": 180,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 180,
            "speaker_commit_failures": 0,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 12,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_status(self):
        calls["base_url"] = self.base_url
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "delta",
        "--no-prompt",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"status": "warn"' in captured.out
    assert '"say_chunks_received": 80' in captured.out
    assert '"say_chunks_dropped": 0' in captured.out
    assert '"speaker_write_failures": 0' in captured.out
    assert '"audio_io_deltas"' in captured.out
    assert '"codec_deltas"' in captured.out
    assert '"heap_internal_free_kb baixo: 12"' in captured.out
    assert '"normal_path_clean": true' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"

def test_server_cli_runs_playback_v2_speaker_owner_gate_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": False,
            "speaker_owner_handoff_ready": False,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NO_TX",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 0,
            "speaker_owner_samples": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 180,
            "say_chunks_played": 180,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 180,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 180,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": True,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 80,
            "speaker_owner_silence_frames": 10,
            "speaker_owner_samples": 20480,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {
            "ok": True,
            "speaker_owner_requested": False,
            "speaker_owner_dry_run_enabled": False,
        }

    def fake_status(self):
        calls["sequence"].append("status")
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-gate",
        "--no-prompt",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"status": "ok"' in captured.out
    assert '"owner_readiness_gate": true' in captured.out
    assert '"ready": true' in captured.out
    assert '"active": true' in captured.out
    assert '"block_reason": "NONE"' in captured.out
    assert '"required_say_chunks": 0' in captured.out
    assert '"real_owner_candidate": false' in captured.out
    assert '"gate executado sem --require-say"' in captured.out
    assert '"non_silence_frames": 70' in captured.out
    assert '"say_chunks_received_delta": 80' in captured.out
    assert '"say_chunks_played_delta": 80' in captured.out
    assert '"disarmed"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_marks_playback_v2_speaker_owner_gate_as_real_candidate(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 4,
            "speaker_owner_silence_frames": 4,
            "speaker_owner_samples": 1024,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 481,
            "say_chunks_played": 481,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 481,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 481,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": True,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 7520,
            "speaker_owner_silence_frames": 7139,
            "speaker_owner_samples": 1925120,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {"ok": True, "speaker_owner_requested": False}

    def fake_status(self):
        calls["sequence"].append("status")
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-gate",
        "--require-say",
        "--no-prompt",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"real_owner_candidate": true' in captured.out
    assert '"real_owner_candidate_status": "ready"' in captured.out
    assert '"real_owner_candidate_blockers": []' in captured.out
    assert '"say_chunks_received_delta": 381' in captured.out
    assert '"non_silence_frames": 381' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_fails_playback_v2_speaker_owner_gate_without_active_say(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 2,
            "speaker_owner_silence_frames": 2,
            "speaker_owner_samples": 512,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 120,
            "say_chunks_played": 120,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 120,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 120,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 20,
            "speaker_owner_silence_frames": 20,
            "speaker_owner_samples": 5120,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {"ok": True, "speaker_owner_requested": False}

    def fake_status(self):
        calls["sequence"].append("status")
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    with pytest.raises(SystemExit) as excinfo:
        cli.main([
            "--host",
            "192.168.1.30",
            "debug",
            "playback-v2",
            "speaker-owner-gate",
            "--require-say",
            "--no-prompt",
            "--json",
        ])

    captured = capsys.readouterr()
    assert excinfo.value.code == 1
    assert '"ok": false' in captured.out
    assert '"say_chunks_received_delta": 20' in captured.out
    assert '"non_silence_frames": 0' in captured.out
    assert '"real_owner_candidate": false' in captured.out
    assert '"speaker owner nao ficou ativo durante SAY real"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_reports_playback_v2_speaker_owner_gate_delta_error(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 2,
            "speaker_owner_silence_frames": 2,
            "speaker_owner_samples": 512,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {
            "ok": True,
            "speaker_owner_requested": False,
            "speaker_owner_dry_run_enabled": False,
        }

    def fake_status(self):
        calls["sequence"].append("status")
        if snapshots:
            return snapshots.pop(0)
        raise RuntimeError("api/audio/playback-v2: timeout")

    def fake_io_status(self):
        return {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        }

    def fake_codec_status(self):
        return {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    with pytest.raises(SystemExit) as excinfo:
        cli.main([
            "--host",
            "192.168.1.30",
            "debug",
            "playback-v2",
            "speaker-owner-gate",
            "--require-say",
            "--no-prompt",
            "--json",
        ])

    captured = capsys.readouterr()
    assert excinfo.value.code == 1
    assert '"ok": false' in captured.out
    assert '"delta_error": "api/audio/playback-v2: timeout"' in captured.out
    assert '"playback-v2 gate falhou durante delta: api/audio/playback-v2: timeout"' in captured.out
    assert '"disarmed"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_fails_playback_v2_speaker_owner_gate_without_required_say(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 2,
            "speaker_owner_samples": 512,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 100,
            "say_chunks_played": 100,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 100,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 100,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 6,
            "speaker_owner_samples": 1536,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {"ok": True, "speaker_owner_requested": False}

    def fake_status(self):
        calls["sequence"].append("status")
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    with pytest.raises(SystemExit) as excinfo:
        cli.main([
            "--host",
            "192.168.1.30",
            "debug",
            "playback-v2",
            "speaker-owner-gate",
            "--require-say",
            "--no-prompt",
            "--json",
        ])

    captured = capsys.readouterr()
    assert excinfo.value.code == 1
    assert '"ok": false' in captured.out
    assert '"required_say_chunks": 1' in captured.out
    assert '"SAY real insuficiente no intervalo: 0 < 1"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_reports_playback_v2_counter_reset_in_speaker_owner_gate(
    monkeypatch,
    capsys,
) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {"sequence": []}
    snapshots = [
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 1889,
            "say_chunks_played": 1889,
            "say_chunks_dropped": 29,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 1889,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 1889,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": True,
            "speaker_owner_candidate": True,
            "speaker_owner_handoff_ready": True,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "NONE",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 3,
            "speaker_owner_samples": 768,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "say_queue_count": 0,
            "say_queue_depth": 32,
            "say_chunks_received": 0,
            "say_chunks_played": 0,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "say_chunks_cancelled": 0,
            "say_cancel_count": 0,
            "speaker_write_requests": 0,
            "speaker_write_failures": 0,
            "speaker_frames_committed": 0,
            "speaker_commit_failures": 0,
            "speaker_owner_dry_run_enabled": False,
            "speaker_owner_candidate": False,
            "speaker_owner_handoff_ready": False,
            "speaker_owner_active": False,
            "speaker_owner_block_reason": "DISABLED",
            "speaker_owner_failures": 0,
            "speaker_owner_recoveries": 0,
            "speaker_owner_frames": 0,
            "speaker_owner_samples": 0,
            "error": "ESP_OK",
        },
    ]
    io_snapshots = [
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 20,
        },
        {
            "ok": True,
            "dropped_frames": 0,
            "i2s_recoveries": 0,
            "speaker_handoff_failures": 0,
            "speaker_handoff_recoveries": 0,
            "heap_internal_free_kb": 0,
        },
    ]
    codec_snapshots = [
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
        {
            "ok": True,
            "format": "opus",
            "worker_state": "running",
            "worker_active": True,
            "transport_enabled": True,
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_codec_error": 0,
            "opus_egress_queue_count": 0,
            "queue_count": 0,
            "error": "ESP_OK",
        },
    ]

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["sequence"].append("arm")
        return {"ok": True, "speaker_owner_requested": True}

    def fake_disarm(self):
        calls["sequence"].append("disarm")
        return {"ok": True, "speaker_owner_requested": False}

    def fake_status(self):
        calls["sequence"].append("status")
        return snapshots.pop(0)

    def fake_io_status(self):
        return io_snapshots.pop(0)

    def fake_codec_status(self):
        return codec_snapshots.pop(0)

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )
    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_disarm",
        fake_disarm,
    )
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_playback_v2_status", fake_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_io_v2_status", fake_io_status)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_status", fake_codec_status)

    with pytest.raises(SystemExit) as excinfo:
        cli.main([
            "--host",
            "192.168.1.30",
            "debug",
            "playback-v2",
            "speaker-owner-gate",
            "--require-say",
            "--no-prompt",
            "--json",
        ])

    captured = capsys.readouterr()
    assert excinfo.value.code == 1
    assert '"counter_reset_detected": true' in captured.out
    assert '"say_chunks_received": -1889' in captured.out
    assert '"contadores v2 resetaram no intervalo; possivel reboot/reset diagnostico"' in captured.out
    assert '"gate SAY real invalido porque os contadores resetaram"' in captured.out
    assert '"Playback v2 registrou drops SAY no intervalo"' not in captured.out
    assert '"SAY real insuficiente no intervalo' not in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["sequence"] == ["arm", "status", "status", "disarm"]

def test_server_cli_runs_playback_v2_speaker_owner_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_arm(self):
        calls["base_url"] = self.base_url
        calls["arm"] = True
        return {
            "ok": True,
            "bridge_say_observer": True,
            "bridge_say_queue_owner": True,
            "speaker_owner_requested": True,
            "speaker_owner_ready": False,
            "speaker_owner_active": False,
            "speaker_frames_prepared": 12,
            "speaker_samples_prepared": 3072,
            "speaker_last_samples": 256,
            "speaker_last_volume": 80,
            "speaker_frames_committed": 12,
            "speaker_samples_committed": 3072,
            "speaker_commit_failures": 0,
            "speaker_last_commit_samples": 256,
            "speaker_last_commit_result": "ESP_OK",
            "speaker_write_requests": 12,
            "speaker_write_samples": 3072,
            "speaker_write_failures": 0,
            "speaker_last_write_samples": 256,
            "speaker_last_write_result": "ESP_OK",
            "speaker_empty_polls": 4,
            "speaker_empty_ms": 64,
            "speaker_idle_end_count": 0,
            "playing": False,
            "say_queue_count": 0,
            "say_queue_depth": 16,
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_playback_v2_speaker_owner_arm",
        fake_arm,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "playback-v2",
        "speaker-owner-arm",
    ])

    captured = capsys.readouterr()
    assert "- speaker_owner: requested=True ready=False active=False" in captured.out
    assert "- speaker_prepared: 12/3072 samples last=256 volume=80" in captured.out
    assert "- speaker_committed: 12/3072 samples failures=0 last=256 result=ESP_OK" in captured.out
    assert "- speaker_write: 12/3072 samples failures=0 last=256 result=ESP_OK" in captured.out
    assert "- speaker_empty: polls=4 ms=64 ends=0" in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"
    assert calls["arm"] is True

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

def test_server_cli_parses_codec_v2_health_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "health",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "codec-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "health"
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

def test_server_cli_runs_codec_v2_health_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")

    def fake_health(self):
        return {
            "ok": True,
            "diagnostic": True,
            "healthy": False,
            "status": "degraded",
            "format": "opus",
            "worker_active": False,
            "worker_state": "stopped",
            "packet_drops": 1,
            "opus_egress_packet_drops": 0,
            "opus_egress_queue_count": 0,
            "opus_codec_error": 0,
            "issues": ["transporte Opus ativo sem worker ativo"],
            "warnings": [],
            "rollback_hint": "codec-v2 transport-disable ou NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16",
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_codec_v2_health", fake_health)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "codec-v2",
        "health",
    ])

    captured = capsys.readouterr()
    assert "Codec v2 health" in captured.out
    assert "Saudavel: False" in captured.out
    assert "transporte Opus ativo sem worker ativo" in captured.out
    assert "NOISEBOT_AUDIO_DEFAULT_CODEC=pcm16" in captured.out

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

def test_server_voice_release_check_reports_runtime_codec_transport() -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    check = release_check.build_release_check(
        voice_v2={
            "ok": True,
            "ready": True,
            "block_reason": "none",
            "capture_enabled": True,
            "capture_tx_enabled": True,
            "activity_decider_enabled": True,
            "codec_worker_state": "running",
            "playback_say_queue_count": 0,
            "playback_say_drops": 0,
            "codec_packet_drops": 0,
            "codec_egress_drops": 0,
            "runtime_idle": True,
        },
        codec_v2={
            "ok": True,
            "healthy": True,
            "status": "ok",
            "format": "pcm16",
            "worker_state": "running",
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_queue_count": 0,
            "opus_codec_error": 0,
            "issues": [],
            "warnings": [],
        },
        capture_v2={
            "ok": True,
            "real_capture_enabled": True,
            "bridge_tx_handoff_enabled": True,
            "session_active": False,
            "state": "IDLE_SESSION",
            "dropped_frames": 0,
            "shadow_audio_dropped_chunks": 0,
            "last_error": "ESP_OK",
        },
        playback_v2={
            "ok": True,
            "bridge_say_observer": True,
            "bridge_say_queue_owner": True,
            "bridge_say_active": False,
            "say_queue_count": 0,
            "say_begin_count": 1,
            "say_end_count": 1,
            "say_chunks_received": 40,
            "say_chunks_played": 40,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={
            "last_voice_session": {
                "turn_id": 10,
                "outcome": "llm",
                "turn_taking_decision": "llm",
                "audio_codec": "opus-v2",
                "tts_completed": True,
                "tts_say_end_sent": True,
                "text_scroll_pages": 2,
                "text_scroll_pages_sent": 2,
            }
        },
    )

    assert check.ok is True
    assert check.codec_v2["firmware_format"] == "pcm16"
    assert check.codec_v2["transport_format"] == "opus-v2"
    assert "format=pcm16, transport=opus-v2" in check.gates[1].detail

def test_server_voice_release_check_warns_on_low_audio_io_heap() -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    check = release_check.build_release_check(
        voice_v2={
            "ok": True,
            "ready": True,
            "block_reason": "none",
            "capture_enabled": True,
            "capture_tx_enabled": True,
            "activity_decider_enabled": True,
            "codec_worker_state": "running",
            "playback_say_queue_count": 0,
            "playback_say_drops": 0,
            "codec_packet_drops": 0,
            "codec_egress_drops": 0,
            "runtime_idle": True,
            "audio_io_heap_internal_free_bytes": 5500,
            "audio_io_heap_dma_free_bytes": 7100,
            "audio_io_heap_internal_largest_free_block": 4096,
            "audio_io_heap_dma_largest_free_block": 3072,
            "audio_io_heap_internal_free_kb": 5,
            "audio_io_heap_dma_free_kb": 6,
        },
        codec_v2={
            "ok": True,
            "healthy": True,
            "status": "ok",
            "format": "opus",
            "worker_state": "running",
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "issues": [],
            "warnings": [],
        },
        capture_v2={
            "ok": True,
            "real_capture_enabled": False,
            "session_active": False,
            "state": "IDLE_SESSION",
            "last_error": "ESP_OK",
        },
        playback_v2={
            "ok": True,
            "bridge_say_observer": True,
            "bridge_say_queue_owner": True,
            "bridge_say_active": False,
            "say_queue_count": 0,
            "say_begin_count": 1,
            "say_end_count": 1,
            "say_chunks_received": 40,
            "say_chunks_played": 40,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={
            "last_voice_session": {
                "turn_id": 10,
                "outcome": "llm",
                "turn_taking_decision": "llm",
                "tts_completed": True,
                "tts_say_end_sent": True,
                "text_scroll_pages": 2,
                "text_scroll_pages_sent": 2,
            }
        },
    )

    assert check.ok is True
    voice_gate = check.gates[0]
    assert voice_gate.ok is True
    assert voice_gate.warnings == (
        "audio_io_heap_internal_free_bytes baixo: 5500 (largest=4096)",
        "audio_io_heap_dma_free_bytes baixo: 7100 (largest=3072)",
    )

def test_server_voice_release_check_fails_when_playback_say_lifecycle_is_open() -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    check = release_check.build_release_check(
        voice_v2={
            "ok": True,
            "ready": True,
            "block_reason": "none",
            "capture_enabled": True,
            "capture_tx_enabled": True,
            "activity_decider_enabled": True,
            "codec_worker_state": "running",
            "playback_say_queue_count": 0,
            "playback_say_drops": 0,
            "codec_packet_drops": 0,
            "codec_egress_drops": 0,
            "runtime_idle": True,
        },
        codec_v2={
            "ok": True,
            "healthy": True,
            "status": "ok",
            "format": "opus",
            "worker_state": "running",
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "issues": [],
            "warnings": [],
        },
        capture_v2={
            "ok": True,
            "real_capture_enabled": True,
            "bridge_tx_handoff_enabled": True,
            "session_active": False,
            "state": "IDLE_SESSION",
            "dropped_frames": 0,
            "shadow_audio_dropped_chunks": 0,
            "last_error": "ESP_OK",
        },
        playback_v2={
            "ok": True,
            "bridge_say_observer": True,
            "bridge_say_queue_owner": True,
            "bridge_say_active": True,
            "say_queue_count": 0,
            "say_begin_count": 2,
            "say_end_count": 1,
            "say_chunks_received": 345,
            "say_chunks_played": 345,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={"last_voice_session": {}},
    )

    assert check.ok is False
    playback_gate = check.gates[3]
    assert playback_gate.name == "Playback v2 SAY"
    assert playback_gate.ok is False
    assert playback_gate.warnings == (
        "bridge_say_active=true",
        "lifecycle SAY aberto begin/end=2/1",
    )

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

def test_server_transport_protocol_decodes_status_with_optional_volume() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    legacy_payload = bytes([3]) + struct.pack("<fff", 0.1, 0.2, 0.3) + bytes([91])
    current_payload = legacy_payload + bytes([42])

    legacy_status = protocol.decode_status(legacy_payload)
    current_status = protocol.decode_status(current_payload)

    assert legacy_status["state"] == 3
    assert legacy_status["health"] == 91
    assert legacy_status["volume"] is None
    assert current_status["volume"] == 42

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

    # _enqueue() do MSG_SPEECH_CANCEL usa asyncio.wait_for(queue.put(...)),
    # que cria uma Task interna — precisa de mais um tick do loop para o
    # put() efetivamente acontecer antes de drenarmos a fila.
    await asyncio.sleep(0)

    kept_expr = adapter._tx_queue.get_nowait()
    cancel_item = adapter._tx_queue.get_nowait()
    assert adapter_module._frame_type(kept_expr.frame) == protocol.MSG_EXPR
    assert adapter_module._frame_type(cancel_item.frame) == protocol.MSG_SPEECH_CANCEL

    kept_expr.ack.set_result(None)
    cancel_item.ack.set_result(None)
    adapter._tx_queue.task_done()
    adapter._tx_queue.task_done()
    await asyncio.wait_for(cancel_task, timeout=0.1)

async def test_server_orchestrator_marks_post_barge_stop_decision() -> None:
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
    session = runtime.SessionContext(turn_id=78)
    orchestrator._session = session
    orchestrator._t_barge_in = orchestrator_module.time.monotonic()
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=session.turn_id)

    await orchestrator._on_final_transcript(
        runtime.FinalTranscript(turn_id=session.turn_id, text="Tchup! Bye!")
    )

    assert session.intent_name == "local_stop"
    assert session.meta["recent_barge_in"] is True
    assert session.meta["turn_taking_policy"] == "post_barge_in"
    assert session.meta["turn_taking_decision"] == "post_barge_stop"

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
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_MIN_PAGE_INTERVAL_S", 0)
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_MAX_PAGE_INTERVAL_S", 0)

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

def test_server_metrics_diagnoses_post_barge_stop_decision() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 12,
        "outcome": "local_intent",
        "intent_name": "local_stop",
        "recent_barge_in": True,
        "turn_taking_policy": "post_barge_in",
        "turn_taking_decision": "post_barge_stop",
        "tts_completed": True,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] is None
    assert payload["voice_diagnosis"] == {
        "title": "Turno de voz concluído",
        "detail": "comando curto tratado como stop contextual após barge-in",
        "next_check": "Confirmar SPEECH_CANCEL, fila SAY zerada e ausência de resposta LLM.",
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

async def test_server_empty_wake_prompt_is_suppressed_right_after_tts() -> None:
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
                text="",
                quality=runtime.TranscriptQuality.NO_SPEECH,
                no_speech_prob=0.92,
            )

        async def close(self) -> None:
            pass

    bus = runtime.EventBus(default_maxsize=512)
    stt = MockStt()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        stt_provider=stt,
    )
    orchestrator._last_completed_speech_at = orchestrator_module.time.monotonic()
    intents = bus.subscribe(runtime.IntentResolved)
    task = asyncio.create_task(orchestrator.run())

    try:
        await _simulate_server_voice_session(bus, runtime)
        await _wait_until(
            lambda: stt.finalize_calls >= 1 and orchestrator._session is None
        )

        assert await _drain_queue(intents) == []
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
        _make_server_config(followup_enabled=True, followup_window_ms=6000),
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
        "window_ms": 6000,
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

async def test_server_speech_done_keeps_followup_disabled_by_default() -> None:
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

    session = runtime.SessionContext(turn_id=303)
    session.intent_name = "llm_reply"
    session.reply_text = "Quer que eu continue?"
    session.meta["outcome"] = "llm"
    orchestrator._session = session
    orchestrator._fsm.transition(runtime.TurnState.LISTENING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.COMMITTING_TURN, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.THINKING, turn_id=session.turn_id)
    orchestrator._fsm.transition(runtime.TurnState.SPEAKING, turn_id=session.turn_id)

    await orchestrator._on_speech_done(runtime.SpeechDone(turn_id=session.turn_id))

    assert adapter.sessions == [{
        "event": "SESSION_DONE",
        "turn_id": 303,
        "outcome": "llm",
    }]

def test_server_ops_status_store_redacts_secrets_from_transcript() -> None:
    server_ops = importlib.import_module("noisebot_server.internal.ops")

    store = server_ops.StatusStore()
    store.record_turn(
        11,
        "llm",
        transcript="minha chave sk-abcdef1234567890",
        reply="ok",
        route="llm",
    )

    assert "sk-abcdef1234567890" not in store.last_transcript
    assert "<redacted>" in store.last_transcript

def test_server_agent_local_intent_treats_vale_as_stop_only_after_barge_in() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    normal = provider.match("Vale.", turn_id=45, context={"recent_barge_in": False})
    after_barge = provider.match("Vale.", turn_id=46, context={"recent_barge_in": True})

    assert normal.intent_name is None
    assert after_barge.intent_name == "local_stop"
    assert after_barge.reply_text == "Pronto, parei."
    assert after_barge.resolution_reason == "post_barge_stop"

def test_server_agent_local_intent_treats_farewell_as_stop_only_after_barge_in() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    normal = provider.match("Tchau.", turn_id=47, context={"recent_barge_in": False})
    after_barge = provider.match("Tchau.", turn_id=48, context={"recent_barge_in": True})

    assert normal.intent_name == "local_farewell"
    assert after_barge.intent_name == "local_stop"
    assert after_barge.reply_text == "Pronto, parei."
    assert after_barge.resolution_reason == "post_barge_stop"

def test_server_agent_local_intent_treats_transcribed_bye_as_stop_after_barge_in() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    normal = provider.match("Tchup! Bye!", turn_id=49, context={"recent_barge_in": False})
    after_barge = provider.match("Tchup! Bye!", turn_id=50, context={"recent_barge_in": True})

    assert normal.intent_name == "local_farewell"
    assert after_barge.intent_name == "local_stop"
    assert after_barge.reply_text == "Pronto, parei."
    assert after_barge.resolution_reason == "post_barge_stop"

def test_server_agent_local_volume_intent_emits_device_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("aumente o volume", turn_id=51, context={"status": {"volume": 55}})

    assert result.intent_name == "local_volume_set"
    assert result.reply_text == "Volume em 65 por cento."
    assert result.device_command == {"event": "VOLUME_COMMAND", "percent": 65}

async def test_server_robot_output_routes_volume_command_to_adapter() -> None:
    output_module = importlib.import_module("noisebot_server.internal.agent.output")
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")

    class CapturingAdapter:
        def __init__(self) -> None:
            self.volumes = []

        async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
            pass

        async def send_emot_event(self, event_id: int) -> None:
            pass

        async def send_volume(self, percent: int) -> None:
            self.volumes.append(percent)

    bus = runtime.EventBus()
    adapter = CapturingAdapter()
    provider = output_module.RobotOutputProvider(bus)
    intent = runtime.IntentResolved(
        turn_id=52,
        intent_name="local_volume_set",
        reply_text="Volume em 70 por cento.",
        expression_id=1,
        emot_event_id=2,
        device_command={"event": "VOLUME_COMMAND", "percent": 70},
    )

    await provider.emit_for_intent(intent, adapter, include_reply_text=False)

    assert adapter.volumes == [70]

def test_firmware_expression_uses_dynamic_dirty_rect_for_normal_face() -> None:
    root = Path(__file__).resolve().parents[2]
    expression_cpp = (
        root
        / "components"
        / "services"
        / "expression_service"
        / "expression_service.cpp"
    ).read_text(encoding="utf-8")

    assert "mark_face_dirty_normal" in expression_cpp
    assert "mark_face_dirty_rotated" in expression_cpp
    assert "mark_face_dirty_full" in expression_cpp
    assert "s_prev_face_dirty_valid" in expression_cpp
    assert "render_service_mark_dirty(s_prev_face_dirty_x" in expression_cpp
    assert "bool has_face_fx = sleep_anim || wake_anim || speaking_anim ||" in expression_cpp
    assert "} else if (use_rot) {" in expression_cpp
    assert "mark_face_dirty_normal(left_cx, cy_l_f, face.open_l" in expression_cpp
