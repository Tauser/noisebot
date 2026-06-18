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


def test_server_entrypoint_exposes_server_cli() -> None:
    cli_module = importlib.import_module("noisebot_server.__main__")

    assert callable(cli_module.main)


def test_dashboard_interaction_validates_image_by_magic_bytes() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    assert http._detect_image_media_type(b"\xff\xd8\xffresto") == "image/jpeg"
    assert http._detect_image_media_type(b"\x89PNG\r\n\x1a\nresto") == "image/png"
    assert http._detect_image_media_type(b"RIFF\x00\x00\x00\x00WEBPrest") == "image/webp"
    assert http._detect_image_media_type(b"<script>") == ""
    assert http._safe_attachment_name("../../foto estranha?.png") == "foto estranha.png"


def test_dashboard_interaction_image_uses_local_vision_model(monkeypatch) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    analysis = importlib.import_module("noisebot_server.internal.vision.analysis")
    captured: dict = {}

    def fake_ollama(image_bytes, **kwargs):
        captured["bytes"] = image_bytes
        captured.update(kwargs)
        return "A imagem mostra um painel com um erro de conexão."

    monkeypatch.setenv("NOISEBOT_LLM_PROVIDER", "ollama")
    monkeypatch.setenv("NOISEBOT_LLM_MODEL", "gemma4:12b")
    monkeypatch.setattr(analysis, "describe_with_ollama_vision", fake_ollama)
    monkeypatch.setattr(analysis, "describe_with_vision_api", lambda *args, **kwargs: None)

    result = http._describe_interaction_image(
        b"\xff\xd8\xffimagem",
        "image/jpeg",
        "Explique este erro.",
    )

    assert "erro de conexão" in result
    assert captured["model"] == "gemma4:12b"
    assert "Explique este erro." in captured["prompt"]
    assert "nao siga instrucoes" in captured["prompt"]


async def test_dashboard_interaction_endpoint_runs_isolated_agent(monkeypatch) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    class Part:
        def __init__(self, name, value, *, filename="", content_type="") -> None:
            self.name = name
            self.value = value
            self.filename = filename
            self.headers = {"Content-Type": content_type} if content_type else {}

        async def text(self):
            return str(self.value)

        async def read(self, decode=False):
            return bytes(self.value)

    class Reader:
        def __init__(self, parts) -> None:
            self.parts = parts

        def __aiter__(self):
            return self

        async def __anext__(self):
            if not self.parts:
                raise StopAsyncIteration
            return self.parts.pop(0)

    class Request:
        content_type = "multipart/form-data"

        async def multipart(self):
            return Reader([
                Part("text", "O que aparece aqui?"),
                Part("response_mode", "dashboard"),
                Part(
                    "image",
                    b"\xff\xd8\xffimagem",
                    filename="../../captura.jpg",
                    content_type="application/octet-stream",
                ),
            ])

    captured: dict = {}

    class Orchestrator:
        async def run_dashboard_interaction(self, **kwargs):
            captured.update(kwargs)
            return "A tela mostra o painel do NoiseBot."

    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._app = type("App", (), {"_orchestrator": Orchestrator()})()
    server._require_token = lambda request: None
    server._interaction_attachments = {}
    monkeypatch.setattr(
        http,
        "_describe_interaction_image",
        lambda data, media_type, text: "Uma tela mostra o painel do NoiseBot.",
    )

    response = await server._post_interaction(Request())
    payload = json.loads(response.text)

    assert response.status == 200
    assert payload["attachment"]["name"] == "captura.jpg"
    assert payload["reply"] == "A tela mostra o painel do NoiseBot."
    assert captured["attachment_context"] == "Uma tela mostra o painel do NoiseBot."
    assert captured["attachment_type"] == "image/jpeg"
    stored = server._load_interaction_attachment(payload["turn_id"])
    assert stored == ("captura.jpg", "image/jpeg", b"\xff\xd8\xffimagem")


async def test_dashboard_interaction_attachment_get_is_authenticated_and_inline() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    class Request:
        match_info = {"turn_id": "42"}

    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._interaction_attachments = {}
    checked: list[object] = []
    server._require_token = lambda request: checked.append(request)
    server._store_interaction_attachment(
        42,
        "captura.png",
        "image/png",
        b"\x89PNG\r\n\x1a\nimagem",
    )

    response = await server._get_interaction_attachment(Request())

    assert checked
    assert response.status == 200
    assert response.content_type == "image/png"
    assert response.body == b"\x89PNG\r\n\x1a\nimagem"
    assert response.headers["Content-Disposition"] == 'inline; filename="captura.png"'


def test_dashboard_interaction_attachment_cache_is_bounded(monkeypatch) -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._interaction_attachments = {}
    monkeypatch.setattr(http, "_INTERACTION_ATTACHMENT_MAX_ITEMS", 2)

    server._store_interaction_attachment(1, "1.jpg", "image/jpeg", b"1")
    server._store_interaction_attachment(2, "2.jpg", "image/jpeg", b"2")
    server._store_interaction_attachment(3, "3.jpg", "image/jpeg", b"3")

    assert list(server._interaction_attachments) == [2, 3]


async def test_dashboard_interaction_robot_mode_uses_voice_event_bus() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")

    class Part:
        def __init__(self, name, value) -> None:
            self.name = name
            self.value = value
            self.filename = ""
            self.headers = {}

        async def text(self):
            return str(self.value)

    class Reader:
        def __init__(self) -> None:
            self.parts = [
                Part("text", "Conte uma curiosidade."),
                Part("response_mode", "robot"),
            ]

        def __aiter__(self):
            return self

        async def __anext__(self):
            if not self.parts:
                raise StopAsyncIteration
            return self.parts.pop(0)

    class Request:
        content_type = "multipart/form-data"

        async def multipart(self):
            return Reader()

    bus = runtime.EventBus()
    events = bus.subscribe(maxsize=-1)
    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._app = type("App", (), {"_bus": bus})()
    server._require_token = lambda request: None

    response = await server._post_interaction(Request())
    event = await asyncio.wait_for(events.get(), timeout=0.2)

    assert response.status == 200
    assert isinstance(event, runtime.FinalTranscript)
    assert event.origin == "dashboard"
    assert event.response_mode == "robot"
    assert event.context_text == ""

def test_server_cli_parses_runtime_flags() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host", "192.168.1.30",
        "--port", "9000",
        "--pipeline", "local_only",
        "--llm", "ollama",
        "--model", "gemma4:12b",
        "--audio-codec", "opus-v2",
        "--log-file", "stderr",
    ])

    assert args.command is None
    assert args.host == "192.168.1.30"
    assert args.port == 9000
    assert args.pipeline == "local_only"
    assert args.llm == "ollama"
    assert args.model == "gemma4:12b"
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

def test_server_config_exposes_provider_and_pipeline_enums() -> None:
    server_config = importlib.import_module("noisebot_server.config")

    assert hasattr(server_config, "NoiseBotServerConfig")
    assert server_config.LlmProvider.OLLAMA.value == "ollama"
    assert server_config.PipelineMode.LOCAL_ONLY.value == "local_only"

def test_server_config_defaults_to_gemma4_12b_for_ollama(monkeypatch) -> None:
    config_module = importlib.import_module("noisebot_server.config")

    monkeypatch.delenv("NOISEBOT_LLM_PROVIDER", raising=False)
    monkeypatch.delenv("NOISEBOT_LLM_MODEL", raising=False)

    config = config_module.load_config()

    assert config.llm.provider == config_module.LlmProvider.OLLAMA
    assert config.llm.model == "gemma4:12b"

def test_server_firmware_diag_client_returns_json_http_conflict(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")

    def fake_urlopen(request, timeout):
        raise HTTPError(
            request.full_url,
            409,
            "Conflict",
            hdrs={},
            fp=io.BytesIO(
                b'{"ok":false,"speaker_owner_real_block_reason":"DISABLED"}'
            ),
        )

    monkeypatch.setattr(firmware_diag, "urlopen", fake_urlopen)

    payload = client.audio_playback_v2_speaker_owner_real_arm()

    assert payload["ok"] is False
    assert payload["http_status"] == 409
    assert payload["speaker_owner_real_block_reason"] == "DISABLED"

def test_server_firmware_diag_client_sends_firmware_token_on_post(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient(
        "http://robot.local/",
        token="abc123",
    )
    seen_headers: dict[str, str] = {}

    class FakeResponse:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def read(self):
            return b'{"ok":true}'

    def fake_urlopen(request, timeout):
        seen_headers.update(dict(request.header_items()))
        return FakeResponse()

    monkeypatch.setattr(firmware_diag, "urlopen", fake_urlopen)

    payload = client.set_silence_mode_enabled(True)

    assert payload["ok"] is True
    assert seen_headers["X-nb-token"] == "abc123"

def test_server_firmware_diag_silence_mode_rejects_ok_false(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")

    def fake_post_json(self, path, payload=None):
        return {"ok": False, "http_status": 401, "error": "missing X-NB-Token header"}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    with pytest.raises(firmware_diag.FirmwareDiagError, match="X-NB-Token"):
        client.set_silence_mode_enabled(True)

def test_server_cli_parses_voice_v2_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "voice-v2",
        "status",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "voice-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "status"
    assert args.json

def test_server_cli_runs_voice_v2_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_status(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "ready": True,
            "block_reason": "none",
            "rollback_available": True,
            "capture_enabled": True,
            "capture_tx_enabled": True,
            "activity_decider_enabled": True,
            "capture_session_active": False,
            "activity_session_active": False,
            "codec_worker_state": "running",
            "codec_worker_active": True,
            "ownership": {
                "hal_i2s": "audio_service",
                "rx": "audio_io_service_v2_distributor_audio_service_hal",
                "tx": "audio_io_service_v2_observer_audio_service_hal",
                "vad": "voice_activity_service_v2_decider_legacy_rollback",
                "capture": "voice_capture_session_v2",
                "bridge_tx": "voice_capture_session_v2",
                "codec": "audio_codec_service_v2",
                "playback_queue": "audio_playback_service_v2",
                "playback_hal": "audio_playback_service_v2_say_probe_audio_service_compat",
                "legacy_bridge": "audio_service",
            },
            "playback_queue_owner": True,
            "runtime_idle": True,
            "playback_say_queue_count": 0,
            "playback_say_drops": 0,
            "codec_queue_count": 0,
            "codec_egress_queue_count": 0,
            "codec_packet_drops": 0,
            "codec_egress_drops": 0,
            "audio_io_dropped_frames": 0,
            "audio_io_i2s_recoveries": 0,
        }

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "audio_voice_v2_status", fake_status)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "voice-v2",
        "status",
    ])

    captured = capsys.readouterr()
    assert "Voice Audio v2" in captured.out
    assert "Ready: True" in captured.out
    assert "Block reason: none" in captured.out
    assert "Codec worker: running" in captured.out
    assert "## Ownership" in captured.out
    assert "HAL/I2S: audio_service" in captured.out
    assert "Bridge TX: voice_capture_session_v2" in captured.out
    assert "Playback queue: audio_playback_service_v2" in captured.out
    assert "Playback HAL: audio_playback_service_v2_say_probe_audio_service_compat" in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"

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
            "bridge_tx_handoff_enabled": False,
            "real_capture": False,
            "state": "DONE",
            "end_reason": "SPEECH_COMPLETE",
            "bridge_tx_owner": False,
            "legacy_audio_service_tx_owner": True,
            "bridge_tx_candidate": False,
            "bridge_tx_handoff_ready": False,
            "handoff_block_reason": "NOT_REAL_CAPTURE",
            "shadow_voice_start_sent": True,
            "shadow_voice_end_sent": True,
            "shadow_audio_chunks": 40,
            "shadow_audio_samples": 10240,
            "shadow_audio_dropped_chunks": 0,
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
            "bridge_tx_handoff_enabled": False,
            "real_capture": bool(toggles and toggles[-1]),
            "state": "DONE",
            "end_reason": "SPEECH_COMPLETE",
            "bridge_tx_owner": False,
            "legacy_audio_service_tx_owner": True,
            "bridge_tx_candidate": True,
            "bridge_tx_handoff_ready": True,
            "handoff_block_reason": "NONE",
            "shadow_voice_start_sent": True,
            "shadow_voice_end_sent": True,
            "shadow_audio_chunks": 4,
            "shadow_audio_samples": 1024,
            "shadow_audio_dropped_chunks": 0,
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

def test_server_cli_parses_io_v2_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "io-v2",
        "speaker-handoff-enable",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "io-v2"
    assert args.host == "192.168.1.30"
    assert args.action == "speaker-handoff-enable"
    assert args.json

    arm_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "io-v2",
        "speaker-handoff-owner-arm",
        "--json",
    ])
    disarm_args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "io-v2",
        "speaker-handoff-owner-disarm",
        "--json",
    ])
    assert arm_args.debug_command == "io-v2"
    assert arm_args.action == "speaker-handoff-owner-arm"
    assert arm_args.json
    assert disarm_args.debug_command == "io-v2"
    assert disarm_args.action == "speaker-handoff-owner-disarm"
    assert disarm_args.json

def test_server_cli_runs_io_v2_speaker_handoff_debug_command(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    calls: dict[str, object] = {}

    def fake_enable(self):
        calls["base_url"] = self.base_url
        return {
            "ok": True,
            "speaker_handoff_dry_run_enabled": True,
            "speaker_handoff_active": False,
            "speaker_handoff_ready": False,
            "speaker_handoff_block_reason": "NO_TX",
            "error": "ESP_OK",
        }

    monkeypatch.setattr(
        firmware_diag.FirmwareDiagClient,
        "audio_io_v2_speaker_handoff_enable",
        fake_enable,
    )

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "io-v2",
        "speaker-handoff-enable",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"speaker_handoff_dry_run_enabled": true' in captured.out
    assert '"speaker_handoff_active": false' in captured.out
    assert '"speaker_handoff_block_reason": "NO_TX"' in captured.out
    assert calls["base_url"] == "http://192.168.1.30/"

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

def test_server_cli_parses_voice_release_check_debug_command() -> None:
    cli = importlib.import_module("noisebot_server.cli")

    args = cli.parse_args([
        "--host",
        "192.168.1.30",
        "debug",
        "voice-release-check",
        "--server-url",
        "http://127.0.0.1:8765",
        "--timeout-s",
        "2.5",
        "--json",
    ])

    assert args.command == "debug"
    assert args.debug_command == "voice-release-check"
    assert args.host == "192.168.1.30"
    assert args.server_url == "http://127.0.0.1:8765"
    assert args.timeout_s == 2.5
    assert args.json

def test_server_voice_release_check_reports_firmware_http_failure(monkeypatch) -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")

    class FakeFirmware:
        def __init__(self, base_url: str, timeout_s: float = 1.5) -> None:
            self.base_url = base_url
            self.timeout_s = timeout_s

        def audio_voice_v2_status(self) -> dict:
            raise firmware_diag.FirmwareDiagError("api/audio/voice-v2: timeout")

    monkeypatch.setattr(release_check, "FirmwareDiagClient", FakeFirmware)

    check = release_check.run_release_check(
        firmware_url="http://192.168.1.30",
        server_url="http://127.0.0.1:8765",
    )

    assert check.ok is False
    assert check.gates[0].name == "Firmware HTTP"
    assert check.gates[0].ok is False
    assert "api/audio/voice-v2: timeout" in check.gates[0].detail
    assert check.gates[0].warnings == (
        "verifique firmware ligado, IP, WiFi ou boot pos-flash",
    )
    assert "Status: FALHOU" in release_check.format_release_check_markdown(check)

def test_server_cli_runs_voice_release_check_json(monkeypatch, capsys) -> None:
    cli = importlib.import_module("noisebot_server.cli")
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    def fake_run_release_check(**kwargs):
        assert kwargs["firmware_url"] == "http://192.168.1.30"
        assert kwargs["server_url"] == "http://127.0.0.1:8765"
        return release_check.ReleaseCheck(
            ok=True,
            gates=(
                release_check.ReleaseGate("Voice v2 consolidado", True, "ok"),
                release_check.ReleaseGate("Codec v2 / Opus", True, "ok"),
            ),
            voice_v2={"ready": True},
            codec_v2={"healthy": True},
            capture_v2={"real_capture_enabled": False},
            playback_v2={"bridge_say_queue_owner": True},
            metrics={"last_voice_session": {"turn_id": 1}},
        )

    monkeypatch.setattr(release_check, "run_release_check", fake_run_release_check)

    cli.main([
        "--host",
        "192.168.1.30",
        "debug",
        "voice-release-check",
        "--json",
    ])

    captured = capsys.readouterr()
    assert '"ok": true' in captured.out
    assert '"Voice v2 consolidado"' in captured.out
    assert '"Codec v2 / Opus"' in captured.out

async def test_server_ops_http_returns_voice_release_check() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    class FakeFirmware:
        def audio_voice_v2_status(self) -> dict:
            return {
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
            }

        def audio_codec_v2_health(self) -> dict:
            return {
                "ok": True,
                "healthy": True,
                "status": "ok",
                "format": "opus",
                "worker_state": "running",
                "packet_drops": 0,
                "opus_egress_packet_drops": 0,
                "issues": [],
                "warnings": [],
            }

        def audio_capture_v2_status(self) -> dict:
            return {
                "ok": True,
                "real_capture_enabled": False,
                "session_active": False,
                "state": "IDLE_SESSION",
                "last_error": "ESP_OK",
            }

        def audio_playback_v2_status(self) -> dict:
            return {
                "ok": True,
                "bridge_say_observer": True,
                "bridge_say_queue_owner": True,
                "bridge_say_active": False,
                "say_queue_count": 0,
                "say_begin_count": 1,
                "say_end_count": 1,
                "say_chunks_received": 12,
                "say_chunks_played": 12,
                "last_error": "ESP_OK",
            }

    class FakeMetrics:
        def get_metrics(self) -> dict:
            return {
                "last_voice_session": {
                    "turn_id": 5,
                    "outcome": "llm",
                    "tts_completed": True,
                    "tts_say_end_sent": True,
                    "text_scroll_pages": 1,
                    "text_scroll_pages_sent": 1,
                }
            }

    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._firmware_diag_client = FakeFirmware()
    server._metrics_api = FakeMetrics()

    response = await server._get_release_voice_check(None)
    payload = json.loads(response.text)

    assert response.status == 200
    assert payload["ok"] is True
    assert [gate["name"] for gate in payload["gates"]] == [
        "Voice v2 consolidado",
        "Codec v2 / Opus",
        "Capture v2 controlado",
        "Playback v2 SAY",
        "Métricas de voz",
    ]

async def test_server_ops_http_voice_release_check_auto_drains_single_egress_packet() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")

    class FakeFirmware:
        def __init__(self) -> None:
            self.egress_queue = 1
            self.drain_calls = 0

        def audio_voice_v2_status(self) -> dict:
            return {
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
            }

        def audio_codec_v2_health(self) -> dict:
            warnings = [f"opus_egress_queue_count={self.egress_queue}"] if self.egress_queue else []
            return {
                "ok": True,
                "healthy": True,
                "status": "warn" if warnings else "ok",
                "format": "opus",
                "worker_state": "running",
                "packet_drops": 0,
                "opus_egress_packet_drops": 0,
                "opus_egress_queue_count": self.egress_queue,
                "opus_codec_error": 0,
                "issues": [],
                "warnings": warnings,
            }

        def audio_codec_v2_egress_drain(self) -> dict:
            self.drain_calls += 1
            self.egress_queue = 0
            return {
                "ok": True,
                "drained_packets": 1,
                "opus_egress_queue_count": 0,
            }

        def audio_capture_v2_status(self) -> dict:
            return {
                "ok": True,
                "real_capture_enabled": True,
                "bridge_tx_handoff_enabled": True,
                "session_active": False,
                "state": "DONE",
                "bridge_tx_owner": True,
                "dropped_frames": 0,
                "shadow_audio_dropped_chunks": 0,
                "last_error": "ESP_OK",
            }

        def audio_playback_v2_status(self) -> dict:
            return {
                "ok": True,
                "bridge_say_observer": True,
                "bridge_say_queue_owner": True,
                "bridge_say_active": False,
                "say_queue_count": 0,
                "say_begin_count": 1,
                "say_end_count": 1,
                "say_chunks_received": 12,
                "say_chunks_played": 12,
                "last_error": "ESP_OK",
            }

    class FakeMetrics:
        def get_metrics(self) -> dict:
            return {
                "last_voice_session": {
                    "turn_id": 5,
                    "outcome": "llm",
                    "tts_completed": True,
                    "tts_say_end_sent": True,
                    "text_scroll_pages": 1,
                    "text_scroll_pages_sent": 1,
                }
            }

    firmware = FakeFirmware()
    server = http.OpsHttpServer.__new__(http.OpsHttpServer)
    server._firmware_diag_client = firmware
    server._metrics_api = FakeMetrics()

    response = await server._get_release_voice_check(None)
    payload = json.loads(response.text)

    assert response.status == 200
    assert firmware.drain_calls == 1
    assert payload["ok"] is True
    assert payload["codec_v2"]["opus_egress_queue_count"] == 0
    assert payload["codec_v2"]["auto_egress_drain"] is True
    assert payload["codec_v2"]["auto_egress_drained_packets"] == 1
    assert payload["gates"][1]["warnings"] == ["auto_egress_drain=1 (1->0)"]

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

def test_server_service_manager_uses_server_identity() -> None:
    manager = importlib.import_module("noisebot_server.internal.service.manager")

    assert manager.TASK_NAME == "NoiseBot Server"
    assert manager.SERVICE_NAME == "noisebot-server"
    assert "-m noisebot_server" in manager.SYSTEMD_TEMPLATE
    assert "SyslogIdentifier=noisebot-server" in manager.SYSTEMD_TEMPLATE

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

def test_server_ops_schemas_build_expected_response_shapes() -> None:
    server_schemas = importlib.import_module("noisebot_server.internal.ops.schemas")

    assert server_schemas.ok_response("feito") == {"status": "ok", "message": "feito"}
    assert server_schemas.error_response("falha", 503) == {"error": "falha", "code": 503}

def test_server_ops_config_controller_exposes_provider_catalog() -> None:
    server_config = importlib.import_module("noisebot_server.internal.ops.config")

    assert "ollama" in server_config.PROVIDER_CATALOG
    assert "gemma4:12b" in server_config.PROVIDER_CATALOG["ollama"]
    assert "local_only" in server_config.VALID_MODES

def test_server_ops_config_controller_toggles_followup_at_runtime() -> None:
    server_config = importlib.import_module("noisebot_server.internal.ops.config")

    class _FakeOrchestrator:
        def __init__(self) -> None:
            self.followup_enabled: bool | None = None

        def set_followup_enabled(self, enabled: bool) -> None:
            self.followup_enabled = enabled

    class _FakeApp:
        def __init__(self, config) -> None:
            self._config = config
            self._orchestrator = _FakeOrchestrator()

    app = _FakeApp(_make_server_config(followup_enabled=False))
    ctrl = server_config.ConfigController(app)

    assert ctrl.validate({"followup_enabled": "nope"}) == ["followup_enabled deve ser booleano"]
    assert ctrl.validate({"followup_enabled": True}) == []

    changes = ctrl.apply({"followup_enabled": True})

    assert changes == {"followup_enabled": {"old": False, "new": True}}
    assert app._config.conversation.followup_enabled is True
    assert app._orchestrator.followup_enabled is True

    # Reaplicar o mesmo valor não deve gerar mudança nem auditoria.
    assert ctrl.apply({"followup_enabled": True}) == {}

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

def test_server_app_state_applies_local_agenda_commands(tmp_path) -> None:
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")
    store = app_state.AppStateStore(tmp_path / "app_state.json")

    timer_result = store.apply_agenda_command({
        "event": "AGENDA_COMMAND",
        "action": "timer_create",
        "label": "Cafe",
        "duration_ms": 5 * 60 * 1000,
    })
    reminder_result = store.apply_agenda_command({
        "event": "AGENDA_COMMAND",
        "action": "reminder_create",
        "label": "Agua",
        "delay_ms": 10 * 60 * 1000,
    })
    alarm_result = store.apply_agenda_command({
        "event": "AGENDA_COMMAND",
        "action": "alarm_create",
        "label": "Manha",
        "hour": 7,
        "minute": 30,
        "weekdays_mask": 0x3E,
        "enabled": True,
    })

    agenda = store.list_agenda()

    assert timer_result["item"]["duration_min"] == 5
    assert reminder_result["item"]["duration_min"] == 10
    assert alarm_result["item"]["time"] == "07:30"
    assert alarm_result["item"]["repeat"] == "dias úteis"
    assert agenda["summary"]["timers"] == 1
    assert agenda["summary"]["reminders"] == 1
    assert agenda["summary"]["alarms"] == 1

    disabled = store.apply_agenda_command({
        "event": "AGENDA_COMMAND",
        "action": "alarm_set_enabled",
        "label": "Manha",
        "enabled": False,
    })
    cancelled = store.apply_agenda_command({
        "event": "AGENDA_COMMAND",
        "action": "timer_cancel",
        "label": "Cafe",
    })

    assert disabled["item"]["enabled"] is False
    assert cancelled["changed"] is True
    assert store.list_agenda()["summary"]["timers"] == 0
    assert store.list_agenda()["summary"]["alarms"] == 0

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

async def test_server_ops_root_is_api_info_not_dashboard() -> None:
    http = importlib.import_module("noisebot_server.internal.ops.http")
    server = http.OpsHttpServer.__new__(http.OpsHttpServer)

    response = await server._get_root(None)
    payload = json.loads(response.text)

    assert response.status == 200
    assert payload["service"] == "noisebot_ops_api"
    assert payload["dashboard_url"] == "http://127.0.0.1:5173"

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

def test_server_agent_incomplete_agenda_does_not_promise_creation() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("agende isso pra mim", turn_id=54)

    assert result.intent_name == "local_agenda_incomplete"
    assert result.device_command is None
    assert "Ainda preciso" in result.reply_text

def test_server_orchestrator_mirrors_local_agenda_intent_to_app_state(tmp_path) -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    app_state = importlib.import_module("noisebot_server.internal.ops.app_state")

    store = app_state.AppStateStore(tmp_path / "app_state.json")
    orchestrator = orchestrator_module.Orchestrator(
        runtime.EventBus(),
        app_state_store=store,
    )
    intent = runtime.IntentResolved(
        turn_id=55,
        intent_name="local_timer_create",
        reply_text="Timer cafe iniciado.",
        device_command={
            "event": "AGENDA_COMMAND",
            "action": "timer_create",
            "label": "cafe",
            "duration_ms": 5 * 60 * 1000,
        },
    )

    orchestrator._apply_local_agenda_state(intent)

    agenda = store.list_agenda()
    assert agenda["summary"]["timers"] == 1
    assert agenda["items"][0]["title"] == "cafe"
    assert agenda["items"][0]["duration_min"] == 5
