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
    assert client.set_voice_audio_v2_capture_tx_enabled(True)["ok"]
    assert client.set_voice_audio_v2_capture_tx_enabled(False)["ok"]

    assert get_paths == ["api/audio/capture-v2"]
    assert post_calls == [
        ("api/audio/capture-v2/replay", {"speech_ms": 640}),
        ("api/audio/capture-v2/cancel", None),
        ("api/config", {"key": "voice_audio_v2_capture_enabled", "value": 1}),
        ("api/config", {"key": "voice_audio_v2_capture_enabled", "value": 0}),
        ("api/config", {"key": "voice_audio_v2_capture_tx_enabled", "value": 1}),
        ("api/config", {"key": "voice_audio_v2_capture_tx_enabled", "value": 0}),
    ]

def test_server_firmware_diag_client_exposes_voice_v2_endpoint(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True, "ready": True, "block_reason": "none"}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)

    payload = client.audio_voice_v2_status()

    assert payload["ok"] is True
    assert payload["ready"] is True
    assert get_paths == ["api/audio/voice-v2"]

def test_server_firmware_diag_client_exposes_io_v2_endpoint(monkeypatch) -> None:
    firmware_diag = importlib.import_module("noisebot_server.internal.ops.firmware_diag")
    client = firmware_diag.FirmwareDiagClient("http://robot.local/")
    get_paths: list[str] = []
    post_paths: list[str] = []

    def fake_get_json(self, path):
        get_paths.append(path)
        return {"ok": True, "speaker_handoff_active": False}

    def fake_post_json(self, path, payload=None):
        post_paths.append(path)
        return {"ok": True, "speaker_handoff_active": False}

    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_get_json", fake_get_json)
    monkeypatch.setattr(firmware_diag.FirmwareDiagClient, "_post_json", fake_post_json)

    assert client.audio_io_v2_status()["ok"]
    assert client.audio_io_v2_speaker_handoff_enable()["ok"]
    assert client.audio_io_v2_speaker_handoff_disable()["ok"]
    assert client.audio_io_v2_speaker_handoff_owner_arm()["ok"]
    assert client.audio_io_v2_speaker_handoff_owner_disarm()["ok"]
    assert get_paths == ["api/audio/io-v2"]
    assert post_paths == [
        "api/audio/io-v2/speaker-handoff/enable",
        "api/audio/io-v2/speaker-handoff/disable",
        "api/audio/io-v2/speaker-handoff/owner/arm",
        "api/audio/io-v2/speaker-handoff/owner/disarm",
    ]

def test_server_voice_release_check_accepts_clean_preflight(monkeypatch) -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    class FakeFirmware:
        def __init__(self, base_url: str, timeout_s: float = 1.5) -> None:
            self.base_url = base_url
            self.timeout_s = timeout_s

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
                "say_chunks_received": 40,
                "say_chunks_played": 40,
                "say_chunks_dropped": 0,
                "say_chunks_dropped_listening": 0,
                "last_error": "ESP_OK",
            }

    monkeypatch.setattr(release_check, "FirmwareDiagClient", FakeFirmware)
    monkeypatch.setattr(
        release_check,
        "get_json",
        lambda _url: {
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

    check = release_check.run_release_check(
        firmware_url="http://192.168.1.30",
        server_url="http://127.0.0.1:8765",
    )

    assert check.ok is True
    assert [gate.name for gate in check.gates] == [
        "Voice v2 consolidado",
        "Codec v2 / Opus",
        "Capture v2 controlado",
        "Playback v2 SAY",
        "Métricas de voz",
    ]
    assert "Status: OK" in release_check.format_release_check_markdown(check)

def test_server_voice_release_check_auto_drains_single_idle_egress_packet(monkeypatch) -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    class FakeFirmware:
        def __init__(self, base_url: str, timeout_s: float = 1.5) -> None:
            self.base_url = base_url
            self.timeout_s = timeout_s
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
                "codec_egress_queue_count": self.egress_queue,
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
                "say_chunks_received": 40,
                "say_chunks_played": 40,
                "say_chunks_dropped": 0,
                "say_chunks_dropped_listening": 0,
                "last_error": "ESP_OK",
            }

    created: list[FakeFirmware] = []

    def fake_firmware(*args, **kwargs):
        firmware = FakeFirmware(*args, **kwargs)
        created.append(firmware)
        return firmware

    monkeypatch.setattr(release_check, "FirmwareDiagClient", fake_firmware)
    monkeypatch.setattr(
        release_check,
        "get_json",
        lambda _url: {
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

    check = release_check.run_release_check(
        firmware_url="http://192.168.1.30",
        server_url="http://127.0.0.1:8765",
    )

    assert check.ok is True
    assert created[0].drain_calls == 1
    assert check.codec_v2["status"] == "ok"
    assert check.codec_v2["opus_egress_queue_count"] == 0
    assert check.codec_v2["auto_egress_drain"] is True
    assert check.codec_v2["auto_egress_drained_packets"] == 1
    codec_gate = check.gates[1]
    assert codec_gate.ok is True
    assert codec_gate.warnings == ("auto_egress_drain=1 (1->0)",)

def test_server_voice_release_check_keeps_larger_egress_queue_as_warning() -> None:
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
            "status": "warn",
            "format": "opus",
            "worker_state": "running",
            "packet_drops": 0,
            "opus_egress_packet_drops": 0,
            "opus_egress_queue_count": 3,
            "opus_codec_error": 0,
            "issues": [],
            "warnings": ["opus_egress_queue_count=3"],
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
        metrics={"last_voice_session": {}},
    )

    assert check.ok is False
    codec_gate = check.gates[1]
    assert codec_gate.ok is False
    assert codec_gate.warnings == ("opus_egress_queue_count=3",)

def test_server_voice_release_check_fails_when_voice_v2_gate_blocks() -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")

    check = release_check.build_release_check(
        voice_v2={
            "ok": True,
            "ready": False,
            "block_reason": "codec_worker_inactive",
            "capture_enabled": True,
            "capture_tx_enabled": True,
            "activity_decider_enabled": True,
            "codec_worker_state": "stopped",
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
            "bridge_say_active": False,
            "say_queue_count": 0,
            "say_begin_count": 1,
            "say_end_count": 1,
            "say_chunks_received": 10,
            "say_chunks_played": 10,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={"last_voice_session": {}},
    )

    assert check.ok is False
    voice_gate = check.gates[0]
    assert voice_gate.name == "Voice v2 consolidado"
    assert voice_gate.ok is False
    assert voice_gate.warnings == ("voice-v2 block_reason=codec_worker_inactive",)

def test_server_voice_release_check_warns_on_unexpected_voice_v2_ownership() -> None:
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
            "ownership": {
                "hal_i2s": "audio_service",
                "rx": "audio_io_service_v2_distributor_audio_service_hal",
                "tx": "audio_io_service_v2_observer_audio_service_hal",
                "vad": "legacy_vad",
                "capture": "voice_capture_session_v2",
                "bridge_tx": "unknown",
                "codec": "audio_codec_service_v2",
                "playback_queue": "audio_service",
                "playback_hal": "audio_playback_service_v2_say_probe_audio_service_compat",
                "legacy_bridge": "audio_service",
            },
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
    assert "bridge_tx=unknown" in voice_gate.detail
    assert voice_gate.warnings == (
        "ownership.vad=legacy_vad esperado=voice_activity_service_v2_decider_legacy_rollback",
        "ownership.playback_queue=audio_service esperado=audio_playback_service_v2",
        "ownership.bridge_tx=unknown",
    )

def test_server_voice_release_check_accepts_retained_capture_done_state() -> None:
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
            "issues": [],
            "warnings": [],
        },
        capture_v2={
            "ok": True,
            "real_capture_enabled": False,
            "session_active": False,
            "state": "DONE",
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
            "say_chunks_received": 10,
            "say_chunks_played": 10,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={"last_voice_session": {}},
    )

    assert check.ok is True
    capture_gate = check.gates[2]
    assert capture_gate.name == "Capture v2 controlado"
    assert capture_gate.ok is True
    assert capture_gate.warnings == (
        "capture-v2 reteve a ultima sessao DONE, mas esta desligado e inativo",
    )

def test_server_voice_release_check_accepts_controlled_capture_handoff() -> None:
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
            "state": "DONE",
            "bridge_tx_owner": True,
            "legacy_audio_service_tx_owner": False,
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
            "say_chunks_received": 10,
            "say_chunks_played": 10,
            "say_chunks_dropped": 0,
            "say_chunks_dropped_listening": 0,
            "last_error": "ESP_OK",
        },
        metrics={"last_voice_session": {}},
    )

    assert check.ok is True
    capture_gate = check.gates[2]
    assert capture_gate.name == "Capture v2 controlado"
    assert capture_gate.ok is True
    assert capture_gate.warnings == (
        "capture-v2 controlado reteve o ownership da ultima sessao DONE",
    )

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

def test_server_debug_msg_name_uses_server_boundary() -> None:
    manual = importlib.import_module("noisebot_server.internal.debug.manual")
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    assert manual.msg_name(protocol.MSG_HELLO) == "HELLO"
    assert manual.msg_name(0xFE) == "0xFE"

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

def test_server_transport_protocol_encodes_frames_per_wire_spec() -> None:
    protocol = importlib.import_module("noisebot_server.internal.transport.protocol")

    payload = protocol.encode_expr(3, 1500)
    frame = protocol.encode_frame(protocol.MSG_EXPR, payload)

    assert frame[0] == protocol.SOF
    assert frame[1] | (frame[2] << 8) == len(payload)
    assert frame[3] == protocol.MSG_EXPR
    assert frame[4:4 + len(payload)] == payload
    assert frame[-1] == protocol.crc8(bytes([protocol.MSG_EXPR]) + payload)
    assert len(frame) == len(payload) + protocol.FRAME_OVERHEAD

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
        conversation=config_module.ConversationConfig(
            followup_enabled=False,
            followup_window_ms=8000,
        ),
        log_level=config_module.LogLevel.INFO,
        dry_run=True,
        replay_path=None,
    )

    transport = factory_module.create_transport_factory(config)()

    assert transport.description == "TCP 192.168.1.30:9000"

async def test_server_connection_supervisor_reconnects_after_disconnect(monkeypatch) -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    supervisor_module = importlib.import_module(
        "noisebot_server.internal.transport.supervisor"
    )

    class FakeTransport:
        def __init__(self, index: int) -> None:
            self.index = index
            self.description = f"fake-{index}"

        async def connect(self) -> None:
            calls.append(f"connect:{self.index}")

        async def disconnect(self) -> None:
            calls.append(f"disconnect:{self.index}")

        async def send(self, data: bytes) -> None:
            pass

        async def recv(self, n: int = 4096) -> bytes:
            return b""

    class FakeAdapter:
        def __init__(self, transport, bus) -> None:
            self.transport = transport
            self.bus = bus
            self.is_connected = True

        async def run(self) -> None:
            calls.append(f"adapter:{self.transport.index}")
            self.is_connected = False

    calls: list[str] = []
    transports = [FakeTransport(1), FakeTransport(2)]

    def transport_factory():
        return transports.pop(0)

    async def fake_sleep(seconds: float) -> None:
        calls.append(f"sleep:{seconds:.2f}")
        if not transports:
            await supervisor.shutdown()

    monkeypatch.setattr(supervisor_module, "FirmwareAdapter", FakeAdapter)

    supervisor = supervisor_module.ConnectionSupervisor(
        transport_factory,
        runtime.EventBus(),
        _make_server_config().reconnect,
    )
    supervisor._sleep = fake_sleep

    await supervisor.run()

    assert calls == [
        "connect:1",
        "adapter:1",
        "disconnect:1",
        "sleep:0.05",
        "connect:2",
        "adapter:2",
        "disconnect:2",
        "sleep:0.05",
    ]
    assert supervisor.adapter is None
    assert supervisor.is_connected is False

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
        "recent_barge_in": True,
        "turn_taking_policy": "post_barge_in",
        "turn_taking_decision": "post_barge_stop",
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
        "recent_barge_in": True,
        "turn_taking_policy": "post_barge_in",
        "turn_taking_decision": "post_barge_stop",
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

def test_server_orchestrator_records_turn_taking_policy() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    bus = runtime.EventBus(default_maxsize=512)
    store = status_module.StatusStore()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(),
        get_adapter=lambda: None,
        status_store=store,
    )
    session = runtime.SessionContext(turn_id=77)
    session.final_text = "Tchup! Bye!"
    session.intent_name = "local_stop"
    session.reply_text = "Pronto, parei."
    session.meta["outcome"] = "local_intent"
    session.meta["recent_barge_in"] = True
    session.meta["turn_taking_policy"] = "post_barge_in"
    session.meta["turn_taking_decision"] = "post_barge_stop"

    orchestrator._record_voice_session(session)

    assert store.last_voice_session["recent_barge_in"] is True
    assert store.last_voice_session["turn_taking_policy"] == "post_barge_in"
    assert store.last_voice_session["turn_taking_decision"] == "post_barge_stop"

def test_server_orchestrator_logs_final_voice_session(caplog) -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    bus = runtime.EventBus(default_maxsize=512)
    store = status_module.StatusStore()
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(default_codec="opus-v2"),
        get_adapter=lambda: None,
        status_store=store,
    )
    session = runtime.SessionContext(turn_id=79)
    session.final_text = "Me diga uma curiosidade."
    session.intent_name = "llm_reply"
    session.reply_text = "Aqui vai uma curiosidade."
    session.meta["outcome"] = "llm"
    session.meta["transcript_quality"] = "good"
    session.meta["voice_end_reason"] = "silence"
    session.meta["tts_chunks_sent"] = 12
    session.meta["tts_completed"] = True
    session.meta["tts_say_begin_sent"] = True
    session.meta["tts_say_end_sent"] = True

    with caplog.at_level(logging.INFO, logger=orchestrator_module.__name__):
        orchestrator._record_voice_session(session)

    assert store.last_voice_session["route"] == "llm"
    assert store.last_voice_session["audio_codec"] == "opus-v2"

    lines = [
        record.message.removeprefix("VOICE_SESSION_FINAL ")
        for record in caplog.records
        if record.message.startswith("VOICE_SESSION_FINAL ")
    ]
    assert len(lines) == 1
    payload = json.loads(lines[0])
    assert payload["turn_id"] == 79
    assert payload["outcome"] == "llm"
    assert payload["route"] == "llm"
    assert payload["state"] == "idle"
    assert payload["audio_codec"] == "opus-v2"
    assert payload["voice_end_reason"] == "silence"
    assert payload["chunk_count"] == 0
    assert payload["transcript_quality"] == "good"
    assert payload["intent_name"] == "llm_reply"
    assert payload["reply_chars"] == len("Aqui vai uma curiosidade.")
    assert payload["tts_chunks_sent"] == 12
    assert payload["tts_completed"] is True
    assert payload["tts_say_begin_sent"] is True
    assert payload["tts_say_end_sent"] is True
    assert isinstance(payload["duration_ms"], float)
    assert "transcript" not in payload
    assert "reply" not in payload

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

def test_server_text_scroll_interval_tracks_reply_length() -> None:
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    short_interval = orchestrator_module._text_scroll_page_interval(
        "Resposta curta para tela.",
        2,
    )
    long_interval = orchestrator_module._text_scroll_page_interval(
        "texto longo " * 60,
        8,
    )

    assert short_interval >= orchestrator_module.TEXT_SCROLL_MIN_PAGE_INTERVAL_S
    assert short_interval < 1.6
    assert long_interval > short_interval
    assert long_interval <= orchestrator_module.TEXT_SCROLL_MAX_PAGE_INTERVAL_S

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
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_MIN_PAGE_INTERVAL_S", 0)
    monkeypatch.setattr(orchestrator_module, "TEXT_SCROLL_MAX_PAGE_INTERVAL_S", 0)
    session = runtime.SessionContext(turn_id=78)
    session.reply_text = "Resposta longa. " + ("texto completo " * 20)

    await orchestrator._send_reply_text_scroll(session)

    assert len(adapter.texts) > 1
    assert all(len(page.encode("utf-8")) <= 128 for page in adapter.texts)
    assert session.reply_text.startswith("Resposta longa.")
    assert session.meta["text_scroll_truncated"] is True
    assert session.meta["text_scroll_pages"] == len(adapter.texts)
    assert session.meta["text_scroll_pages_sent"] == len(adapter.texts)

@pytest.mark.asyncio
async def test_server_reply_text_scroll_preserves_utf8_text() -> None:
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
    session = runtime.SessionContext(turn_id=79)
    session.reply_text = "Olá! São águas incríveis para você."

    await orchestrator._send_reply_text_scroll(session)

    assert adapter.texts == ["Olá! São águas incríveis para você."]
    assert session.reply_text == "Olá! São águas incríveis para você."

def test_server_detects_vision_scene_question_for_pre_feedback() -> None:
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    assert orchestrator_module._looks_like_vision_scene_question(
        "O que você está vendo?"
    )
    assert orchestrator_module._looks_like_vision_scene_question("descreva a cena")
    assert not orchestrator_module._looks_like_vision_scene_question("que horas sao")

@pytest.mark.asyncio
async def test_server_vision_scene_pre_feedback_sends_visual_hint() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class DummyAdapter:
        def __init__(self) -> None:
            self.expressions: list[int] = []
            self.texts: list[str] = []

        async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
            self.expressions.append(expression_id)

        async def send_text_scroll(self, text: str) -> None:
            self.texts.append(text)

    adapter = DummyAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        runtime.EventBus(),
        _make_server_config(),
        get_adapter=lambda: adapter,
    )
    session = runtime.SessionContext(turn_id=80)

    await orchestrator._show_vision_thinking_feedback(session.turn_id, session)

    assert adapter.expressions == [orchestrator_module.VISION_THINKING_EXPR_ID]
    assert adapter.texts == [orchestrator_module.VISION_THINKING_TEXT]
    assert session.meta["vision_thinking_feedback"] is True

@pytest.mark.asyncio
async def test_server_llm_pre_feedback_sends_wait_message() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    class DummyAdapter:
        def __init__(self) -> None:
            self.expressions: list[int] = []
            self.texts: list[str] = []

        async def send_expr(self, expression_id: int, duration_ms: int = 2000) -> None:
            self.expressions.append(expression_id)

        async def send_text_scroll(self, text: str) -> None:
            self.texts.append(text)

    adapter = DummyAdapter()
    orchestrator = orchestrator_module.Orchestrator(
        runtime.EventBus(),
        _make_server_config(),
        get_adapter=lambda: adapter,
    )
    session = runtime.SessionContext(turn_id=81)

    await orchestrator._show_llm_thinking_feedback(session.turn_id, session)

    assert adapter.expressions == [orchestrator_module.LLM_THINKING_EXPR_ID]
    assert adapter.texts == [orchestrator_module.LLM_THINKING_TEXT]
    assert session.meta["llm_thinking_feedback"] is True


def test_server_status_clears_stale_reply_when_new_turn_starts() -> None:
    status_module = importlib.import_module("noisebot_server.internal.ops.status")
    store = status_module.StatusStore()
    store.record_turn_detail(10, transcript="oi", reply="Olá!", route="local_intent")

    store.record_turn_detail(11, transcript="crie um hello world")

    assert store.last_turn_id == 11
    assert store.last_transcript == "crie um hello world"
    assert store.last_reply == ""
    assert store.last_route == ""


def test_server_voice_session_preserves_long_code_reply() -> None:
    status_module = importlib.import_module("noisebot_server.internal.ops.status")
    store = status_module.StatusStore()
    reply = "```java\n" + ("System.out.println(\"NoiseBot\");\n" * 80) + "```"

    store.record_voice_session({"turn_id": 12, "outcome": "ok", "reply": reply})

    assert store.last_voice_session["reply"] == reply
    assert store.recent_voice_sessions[0]["reply"].endswith("```")


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


def test_server_voice_session_exposes_llm_time_and_token_usage() -> None:
    status_module = importlib.import_module("noisebot_server.internal.ops.status")
    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 8,
        "outcome": "ok",
        "llm_first_token_ms": 245.7,
        "llm_total_ms": 1320.4,
        "input_tokens": 418,
        "output_tokens": 73,
    })

    assert store.last_voice_session["llm_first_token_ms"] == 245.7
    assert store.last_voice_session["llm_total_ms"] == 1320.4
    assert store.last_voice_session["input_tokens"] == 418
    assert store.last_voice_session["output_tokens"] == 73


def test_server_metrics_token_total_uses_full_history_beyond_window() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")
    registry = metrics_module.MetricsRegistry(window=2)
    for value in (10, 20, 30):
        registry.record("input_tokens", value)
    for value in (4, 5, 6):
        registry.record("output_tokens", value)

    payload = api_module.MetricsApi(
        registry,
        status_module.StatusStore(),
    ).get_metrics()

    assert payload["tokens"] == {"input": 60, "output": 15}

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

def test_server_metrics_diagnoses_direct_stop_decision() -> None:
    metrics_module = importlib.import_module("noisebot_server.internal.agent.metrics")
    api_module = importlib.import_module("noisebot_server.internal.ops.metrics")
    status_module = importlib.import_module("noisebot_server.internal.ops.status")

    store = status_module.StatusStore()
    store.record_voice_session({
        "turn_id": 13,
        "outcome": "local_intent",
        "intent_name": "local_stop",
        "recent_barge_in": False,
        "turn_taking_policy": "normal",
        "turn_taking_decision": "direct_stop",
        "tts_completed": True,
    })

    payload = api_module.MetricsApi(metrics_module.MetricsRegistry(), store).get_metrics()

    assert payload["voice_alert"] is None
    assert payload["voice_diagnosis"] == {
        "title": "Turno de voz concluído",
        "detail": "comando direto de stop/cancelamento resolvido localmente",
        "next_check": "Confirmar resposta curta e sem chamada LLM.",
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

def test_orchestrator_set_followup_enabled_updates_live_config() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    orchestrator_module = importlib.import_module(
        "noisebot_server.internal.agent.orchestrator"
    )

    bus = runtime.EventBus(default_maxsize=512)
    orchestrator = orchestrator_module.Orchestrator(
        bus,
        _make_server_config(followup_enabled=False),
    )

    question = runtime.SessionContext(turn_id=1)
    question.intent_name = "llm_reply"
    question.reply_text = "Quer que eu continue?"
    question.meta["outcome"] = "llm"

    assert orchestrator._should_arm_followup(question) is False

    orchestrator.set_followup_enabled(True)

    assert orchestrator._config.conversation.followup_enabled is True
    assert orchestrator._should_arm_followup(question) is True

    orchestrator.set_followup_enabled(False)

    assert orchestrator._should_arm_followup(question) is False

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

def test_server_agent_runtime_exposes_voice_end_reason_and_turn_state() -> None:
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")

    assert runtime.VoiceEndReason.SILENCE.value == 0
    assert runtime.TurnState.IDLE.name == "IDLE"

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

def test_server_agent_local_status_emits_visual_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match(
        "qual seu status",
        turn_id=44,
        context={"status": {"health": 99, "attention": 0.42}},
    )

    assert result.intent_name == "local_status"
    assert result.reply_text == "Status: saude 99%, atencao 42%."
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

@pytest.mark.parametrize(
    "phrase",
    [
        "crie um exemplo de hello world em python",
        "monte uma classe hello world em java",
        "explique o programa hello world",
    ],
)
def test_server_agent_hello_world_is_not_a_greeting(phrase: str) -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match(phrase, turn_id=52)

    assert result.intent_name is None


@pytest.mark.parametrize("phrase", ["oi", "olá NoiseBot", "bom dia", "hello"])
def test_server_agent_short_greetings_remain_local(phrase: str) -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match(phrase, turn_id=53)

    assert result.intent_name == "local_greeting"


def test_server_agent_show_status_phrase_emits_visual_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("mostrar status", turn_id=45)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

def test_server_agent_show_the_status_phrase_emits_visual_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("mostra o status", turn_id=46)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

def test_server_agent_display_status_phrase_does_not_emit_text() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("exibir status", turn_id=47)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

def test_server_agent_exiba_status_phrase_does_not_emit_text() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("exiba status", turn_id=48)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

def test_server_agent_wrong_article_status_phrase_does_not_emit_text() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("Mostre a status.", turn_id=49)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

def test_server_agent_ai_particle_status_phrase_does_not_emit_text() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("Mostra aí status.", turn_id=50)

    assert result.intent_name == "local_status"
    assert result.reply_text is None
    assert result.device_command == {
        "event": "STATUS_COMMAND",
        "action": "quick_status",
    }

@pytest.mark.parametrize(
    ("phrase", "intent_name", "expression_id"),
    [
        ("fique feliz", "local_expression_happy", 1),
        ("fique curioso", "local_expression_curious", 2),
        ("fique focado", "local_expression_focused", 4),
    ],
)
def test_server_agent_local_expression_ids_match_firmware(
    phrase: str,
    intent_name: str,
    expression_id: int,
) -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match(phrase, turn_id=51)

    assert result.intent_name == intent_name
    assert result.expression_id == expression_id

def test_server_agent_generic_brightness_short_command_emits_settings_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("diminua o brilho", turn_id=53)

    assert result.intent_name == "local_led_brightness"
    assert result.reply_text == "Brilho dos LEDs em 25 por cento."
    assert result.device_command == {
        "event": "SETTINGS_COMMAND",
        "led_brightness": 64,
    }

def test_server_agent_display_brightness_is_honest_until_backlight_exists() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("diminua o brilho da tela", turn_id=53)

    assert result.intent_name == "local_display_brightness_unsupported"
    assert result.device_command is None
    assert "nao tem backlight ajustavel" in result.reply_text

def test_server_agent_led_brightness_percent_emits_settings_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("diminua o brilho dos leds para 20%", turn_id=53)

    assert result.intent_name == "local_led_brightness"
    assert result.reply_text == "Brilho dos LEDs em 20 por cento."
    assert result.device_command == {
        "event": "SETTINGS_COMMAND",
        "led_brightness": 51,
    }

def test_server_agent_light_reset_emits_led_command() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    provider = agent.LocalIntentProvider()

    result = provider.match("volte a luz normal", turn_id=53)

    assert result.intent_name == "local_light_reset"
    assert result.device_command == {"event": "LED_COMMAND", "action": "reset"}

def test_server_agent_sentencizer_splits_on_sentence_boundaries() -> None:
    agent = importlib.import_module("noisebot_server.internal.agent")
    sentencizer = agent.Sentencizer()

    sentences = list(sentencizer.feed("Ola. Tudo bem?")) + list(sentencizer.flush())

    assert sentences == ["Ola. Tudo bem?"]

def test_firmware_render_metrics_contract_is_exposed() -> None:
    root = Path(__file__).resolve().parents[2]
    render_h = (
        root
        / "components"
        / "services"
        / "render_service"
        / "render_service.h"
    ).read_text(encoding="utf-8")
    render_c = (
        root
        / "components"
        / "services"
        / "render_service"
        / "render_service.cpp"
    ).read_text(encoding="utf-8")
    web_c = (root / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")

    assert "nb_render_metrics_t" in render_h
    assert "render_service_get_metrics" in render_h
    assert "s_full_push_count" in render_c
    assert "avg_clear_ms" in web_c
    assert '"/api/render/status"' in web_c

def test_firmware_overlay_icons_use_generated_masks() -> None:
    root = Path(__file__).resolve().parents[2]
    icons_dir = (
        root
        / "components"
        / "services"
        / "ui_overlay_service"
        / "icons"
    )
    generated_h = (icons_dir / "generated" / "nb_ui_overlay_icons.h").read_text(
        encoding="ascii"
    )
    source_pbm = (icons_dir / "source" / "camera.pbm").read_text(encoding="ascii")
    generator = (icons_dir / "tools" / "generate_overlay_icons.py").read_text(
        encoding="utf-8"
    )

    assert source_pbm.startswith("P1\n")
    assert "24 24" in source_pbm
    assert "typedef struct" in generated_h
    assert "NB_UI_OVERLAY_ICON_CAMERA_MASK" in generated_h
    assert "NB_UI_OVERLAY_ICON_CAMERA" in generated_h
    assert "0x00, 0x3F, 0xC0, 0x00" in generated_h
    assert "--check" in generator

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

def test_translate_expression_id_all_values() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")
    expected = {
        "neutral": 0, "happy": 1, "curious": 2, "sleepy": 3, "focused": 4,
        "suspicious": 5, "surprised": 6, "sad": 7, "alarmed": 8, "angry": 9,
    }

    for expr, expected_int in expected.items():
        assert llm.translate_expression_id(expr) == expected_int, f"'{expr}' deve mapear para {expected_int}"

def test_translate_expression_id_unknown_returns_none() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    assert llm.translate_expression_id("energized") is None
    assert llm.translate_expression_id("xyz") is None

def test_translate_expression_id_none_passthrough() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    assert llm.translate_expression_id(None) is None

def test_translate_expression_id_case_insensitive() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    assert llm.translate_expression_id("HAPPY") == 1
    assert llm.translate_expression_id("Curious") == 2

def test_ollama_provider_default_temperature_is_low() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")
    import inspect

    sig = inspect.signature(llm.OllamaProvider.__init__)
    default_temp = sig.parameters["temperature"].default

    assert default_temp <= 0.3, f"temperatura padrao do Ollama deve ser <= 0.3, era {default_temp}"

def test_build_correction_messages_returns_system_and_user() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    messages = llm.build_correction_messages('{"expression_id":1}')

    assert len(messages) == 2
    assert messages[0]["role"] == "system"
    assert messages[1]["role"] == "user"
    assert "JSON" in messages[1]["content"]
    assert '{"expression_id":1}' in messages[1]["content"]

def test_build_correction_messages_truncates_long_bad_raw() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    long_raw = "x" * 1000
    messages = llm.build_correction_messages(long_raw)

    assert len(messages[1]["content"]) < 800

def test_gemma4_12b_and_qwen35_9b_in_ollama_catalog() -> None:
    """Rollback para qwen3.5:9b deve permanecer disponível."""
    config_ops = importlib.import_module("noisebot_server.internal.ops.config")

    assert "gemma4:12b" in config_ops.PROVIDER_CATALOG["ollama"]
    assert "qwen3.5:9b" in config_ops.PROVIDER_CATALOG["ollama"]

def test_mood_happy_energetic() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.8, activation=0.8)

    assert "animado" in result

def test_mood_calm_happy() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.7, activation=0.2)

    assert "calmo" in result

def test_mood_angry_alarmed() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=-0.6, activation=0.8)

    assert "irritado" in result or "alarmado" in result

def test_mood_sad_withdrawn() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=-0.5, activation=0.2)

    assert "triste" in result or "retraído" in result

def test_mood_neutral() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.1, activation=0.3)

    assert "neutro" in result

def test_mood_focused_modifier() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.1, activation=0.3, attention=0.9)

    assert "focado" in result

def test_mood_familiar_modifier() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.1, activation=0.3, trust=0.9)

    assert "familiaridade" in result

def test_mood_combined_modifiers() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.6, activation=0.6, attention=0.8, trust=0.8)

    assert "animado" in result
    assert "focado" in result
    assert "familiaridade" in result

def test_mood_returns_string_never_float() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    result = mood.describe_mood(valence=0.42, activation=0.17, attention=0.55, trust=0.33)

    assert isinstance(result, str)
    assert "0.42" not in result
    assert "0.17" not in result

def test_mood_boundary_valence_above_threshold() -> None:
    mood = importlib.import_module("noisebot_server.internal.agent.mood")

    # Exatamente no limiar de -0.4: não é negativo forte
    result = mood.describe_mood(valence=-0.4, activation=0.2)

    assert "neutro" in result

def test_payload_contains_turn_info() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(text="oi noise", turn_id=42)

    assert payload["turn"]["id"] == 42
    assert payload["turn"]["user_text"] == "oi noise"
    assert "timestamp_iso" in payload["turn"]

def test_payload_mood_is_text_not_float() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(
        text="teste",
        turn_id=1,
        last_status={"valence": 0.8, "activation": 0.7, "attention": 0.5, "trust": 0.3},
    )

    assert isinstance(payload["mood"], str)
    assert "0.8" not in payload["mood"]
    assert "0.7" not in payload["mood"]

def test_payload_contains_user_profile() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(
        text="oi",
        turn_id=1,
        user_profile={
            "display_name": "Tadeu",
            "relationship": "owner",
            "language": "pt-BR",
            "robot_nickname": "Noise",
            "persona_mode": "companion",
            "interaction_style": "direct_warm",
        },
    )

    assert "user" in payload
    assert payload["user"]["display_name"] == "Tadeu"
    assert payload["user"]["robot_nickname"] == "Noise"

def test_payload_no_user_section_when_profile_empty() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(text="oi", turn_id=1, user_profile=None)

    assert "user" not in payload

def test_payload_hardware_reflects_availability() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(
        text="teste",
        turn_id=1,
        tts_available=True,
        vision_available=False,
        servos_enabled=False,
    )

    assert payload.get("hardware", {}).get("tts_available") is True
    assert "vision_available" not in payload.get("hardware", {})
    assert "servos_enabled" not in payload.get("hardware", {})

def test_payload_no_hardware_section_when_all_off() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(
        text="teste",
        turn_id=1,
        tts_available=False,
        vision_available=False,
        servos_enabled=False,
    )

    assert "hardware" not in payload

def test_payload_firmware_online_in_robot_section() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload_on = pb.build_turn_payload(text="t", turn_id=1, firmware_online=True)
    payload_off = pb.build_turn_payload(text="t", turn_id=1, firmware_online=False)

    assert payload_on["robot"]["firmware_online"] is True
    assert payload_off["robot"]["firmware_online"] is False

def test_payload_conversation_history_bounded_to_3() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    many = [f"msg {i}" for i in range(10)]
    payload = pb.build_turn_payload(
        text="atual",
        turn_id=1,
        recent_user_texts=many,
        recent_robot_replies=many,
    )

    assert len(payload["conversation"]["recent_user"]) == 3
    assert len(payload["conversation"]["recent_robot"]) == 3
    # Deve conter as 3 mais recentes
    assert payload["conversation"]["recent_user"][-1] == "msg 9"

def test_payload_no_conversation_section_when_empty() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    payload = pb.build_turn_payload(text="oi", turn_id=1)

    assert "conversation" not in payload

def test_payload_user_text_truncated_at_300() -> None:
    pb = importlib.import_module("noisebot_server.internal.agent.payload_builder")

    long_text = "a" * 500
    payload = pb.build_turn_payload(text=long_text, turn_id=1)

    assert len(payload["turn"]["user_text"]) == 300

def test_build_messages_uses_turn_payload_when_present() -> None:
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    payload = {
        "turn": {"id": 1, "user_text": "oi", "timestamp_iso": "2026-06-09T00:00:00+00:00"},
        "robot": {"state": "IDLE", "firmware_online": True, "pipeline_mode": "normal"},
        "mood": "calmo e contente",
        "user": {"display_name": "Tadeu", "persona_mode": "companion"},
    }
    messages = llm.build_messages("oi noise", {"turn_payload": payload})

    system = messages[0]["content"]
    assert "Contexto do turno atual:" in system
    assert "Humor do robo: calmo e contente" in system
    assert "firmware: conectado" in system

def test_build_messages_legacy_path_unchanged() -> None:
    """Contexto legado sem turn_payload ainda funciona normalmente."""
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    messages = llm.build_messages(
        "Como estou?",
        {
            "user_profile": {
                "display_name": "Tadeu",
                "relationship": "owner",
                "language": "pt-BR",
                "robot_nickname": "Noise",
                "persona_mode": "companion",
                "interaction_style": "direct_warm",
            }
        },
    )

    system = messages[0]["content"]
    assert "Perfil do usuario atual" in system
    assert "Nome do usuario: Tadeu" in system

def test_build_messages_recent_replies_guard_on_payload_path() -> None:

    """Anti-repetição deve ser injetada também no caminho do payload estruturado."""
    llm = importlib.import_module("noisebot_server.internal.agent.llm")

    payload = {
        "turn": {"id": 1, "user_text": "piada", "timestamp_iso": "2026-06-09T00:00:00+00:00"},
        "robot": {"state": "IDLE", "firmware_online": False, "pipeline_mode": "normal"},
        "mood": "neutro",
    }
    messages = llm.build_messages(
        "Me conta uma piada.",
        {
            "turn_payload": payload,
            "recent_replies": ["Por que o livro foi ao médico? Tinha problemas de capa."],
        },
    )

    system = messages[0]["content"]
    assert "Respostas recentes a evitar repetir" in system
    assert "livro foi ao médico" in system

def test_validate_arguments_set_expression_valid() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_expression"]

    errors = catalog.validate_arguments(spec, {"expression_id": "happy"})

    assert errors == []

def test_validate_arguments_set_expression_unknown_value() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_expression"]

    errors = catalog.validate_arguments(spec, {"expression_id": "energized"})

    assert any("energized" in e for e in errors)

def test_validate_arguments_set_expression_missing_required() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_expression"]

    errors = catalog.validate_arguments(spec, {})

    assert any("expression_id" in e for e in errors)

def test_validate_arguments_set_led_valid_with_brightness() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_led"]

    errors = catalog.validate_arguments(spec, {"color": "#FF0000", "brightness": 80})

    assert errors == []

def test_validate_arguments_set_led_brightness_above_max() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_led"]

    errors = catalog.validate_arguments(spec, {"color": "red", "brightness": 150})

    assert any("brightness" in e for e in errors)
    assert any("100" in e for e in errors)

def test_validate_arguments_set_led_brightness_below_min() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_led"]

    errors = catalog.validate_arguments(spec, {"color": "red", "brightness": -1})

    assert any("brightness" in e for e in errors)

def test_validate_arguments_set_led_brightness_wrong_type() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_led"]

    errors = catalog.validate_arguments(spec, {"color": "red", "brightness": "high"})

    assert any("brightness" in e for e in errors)
    assert any("inteiro" in e for e in errors)

def test_validate_arguments_create_timer_valid() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["create_timer"]

    errors = catalog.validate_arguments(spec, {"duration_s": 300, "label": "macarrao"})

    assert errors == []

def test_validate_arguments_create_timer_too_long() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["create_timer"]

    errors = catalog.validate_arguments(spec, {"duration_s": 100000})

    assert any("duration_s" in e for e in errors)
    assert any("86400" in e for e in errors)

def test_validate_arguments_create_timer_zero_duration() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["create_timer"]

    errors = catalog.validate_arguments(spec, {"duration_s": 0})

    assert any("duration_s" in e for e in errors)

def test_validate_arguments_create_reminder_missing_text() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["create_reminder"]

    errors = catalog.validate_arguments(spec, {"trigger_iso": "2026-06-09T15:00:00"})

    assert any("text" in e for e in errors)

def test_validate_arguments_unknown_field_is_error() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["set_expression"]

    errors = catalog.validate_arguments(
        spec, {"expression_id": "happy", "unknown_field": "xyz"}
    )

    assert any("desconhecido" in e or "unknown_field" in e for e in errors)

def test_request_confirmation_is_confirmation_required() -> None:
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    spec = catalog.CATALOG["request_confirmation"]

    assert spec.risk_level == "confirmation_required"

def test_executor_set_expression_returns_expression_id() -> None:
    executors = importlib.import_module("noisebot_server.internal.agent.tools.executors")

    result = executors.execute_set_expression(
        {"expression_id": "happy"}, {"adapter": None, "turn_id": 1}
    )

    assert result["expression_id"] == "happy"
    assert result["sent"] is False  # sem adapter

def test_executor_set_led_defaults_brightness_to_100() -> None:
    executors = importlib.import_module("noisebot_server.internal.agent.tools.executors")

    result = executors.execute_set_led(
        {"color": "blue"}, {"adapter": None, "turn_id": 1}
    )

    assert result["color"] == "blue"
    assert result["brightness"] == 100

def test_executor_create_timer_without_app_state_returns_partial() -> None:
    executors = importlib.import_module("noisebot_server.internal.agent.tools.executors")

    result = executors.execute_create_timer(
        {"duration_s": 120}, {"app_state": None, "turn_id": 1}
    )

    assert result["duration_s"] == 120
    assert result.get("persisted") is False

def test_executor_request_confirmation_returns_pending() -> None:
    executors = importlib.import_module("noisebot_server.internal.agent.tools.executors")

    result = executors.execute_request_confirmation(
        {"question": "Pode deletar?", "action_description": "apagar arquivo"},
        {"turn_id": 1},
    )

    assert result["pending"] is True
    assert result["question"] == "Pode deletar?"

def test_gateway_empty_name_vetoed() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call({"name": "", "arguments": {}})

    assert result.vetoed is True

def test_gateway_invalid_arguments_vetoed_before_executor() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "set_expression", "arguments": {"expression_id": "nonexistent_expr"}}
    )

    assert result.vetoed is True
    assert result.success is False
    assert "argumentos invalidos" in (result.error or "")

def test_gateway_missing_required_arg_vetoed() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call({"name": "set_expression", "arguments": {}})

    assert result.vetoed is True
    assert "expression_id" in (result.error or "")

def test_gateway_set_expression_executes_successfully() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "set_expression", "arguments": {"expression_id": "happy"}},
        adapter=None,
        turn_id=42,
    )

    assert result.success is True
    assert result.vetoed is False
    assert result.result is not None
    assert result.result["expression_id"] == "happy"

def test_gateway_set_led_executes_with_valid_args() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "set_led", "arguments": {"color": "blue", "brightness": 50}},
        turn_id=1,
    )

    assert result.success is True
    assert result.result["color"] == "blue"
    assert result.result["brightness"] == 50

def test_gateway_confirmation_required_returns_pending() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "request_confirmation", "arguments": {"question": "Tem certeza?"}},
        turn_id=1,
    )

    assert result.requires_confirmation is True
    assert result.success is False
    assert result.vetoed is False
    assert result.error is None

def test_gateway_state_policy_vetoes_restricted_state() -> None:
    """Tool com allowed_states definido deve ser vetada em estado incompatível."""
    catalog = importlib.import_module("noisebot_server.internal.agent.tools.catalog")
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    # Criar spec temporária com allowed_states restrito
    spec = catalog.ToolSpec(
        name="test_restricted",
        description="test",
        arguments_schema={"type": "object", "properties": {}, "required": []},
        risk_level="low",
        allowed_states=frozenset({"IDLE"}),
    )
    # Injetar temporariamente no catálogo
    catalog.CATALOG["test_restricted"] = spec
    from noisebot_server.internal.agent.tools.executors import EXECUTOR_MAP
    EXECUTOR_MAP["test_restricted"] = lambda args, ctx: {"ok": True}

    try:
        result = gateway.execute_tool_call(
            {"name": "test_restricted", "arguments": {}},
            current_state="THINKING",
            turn_id=1,
        )
        assert result.vetoed is True
        assert "THINKING" in (result.error or "")

        result_allowed = gateway.execute_tool_call(
            {"name": "test_restricted", "arguments": {}},
            current_state="IDLE",
            turn_id=1,
        )
        assert result_allowed.success is True
    finally:
        catalog.CATALOG.pop("test_restricted", None)
        EXECUTOR_MAP.pop("test_restricted", None)

def test_gateway_audit_log_always_present() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    # Caso vetado
    r1 = gateway.execute_tool_call({"name": "unknown_xyz", "arguments": {}})
    assert isinstance(r1.audit_log, dict)
    assert r1.audit_log.get("outcome") == "vetoed"
    assert "timestamp_iso" in r1.audit_log

    # Caso sucesso
    r2 = gateway.execute_tool_call(
        {"name": "set_expression", "arguments": {"expression_id": "neutral"}},
        turn_id=5,
    )
    assert isinstance(r2.audit_log, dict)
    assert r2.audit_log.get("outcome") == "success"
    assert r2.audit_log.get("turn_id") == 5

    # Caso confirmation_pending
    r3 = gateway.execute_tool_call(
        {"name": "request_confirmation", "arguments": {"question": "ok?"}},
        turn_id=6,
    )
    assert r3.audit_log.get("outcome") == "confirmation_pending"

def test_gateway_brightness_boundary_values() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    r0 = gateway.execute_tool_call(
        {"name": "set_led", "arguments": {"color": "red", "brightness": 0}}
    )
    assert r0.success is True

    r100 = gateway.execute_tool_call(
        {"name": "set_led", "arguments": {"color": "red", "brightness": 100}}
    )
    assert r100.success is True

    r_over = gateway.execute_tool_call(
        {"name": "set_led", "arguments": {"color": "red", "brightness": 101}}
    )
    assert r_over.vetoed is True

def test_gateway_create_timer_valid_range() -> None:
    gateway = importlib.import_module("noisebot_server.internal.agent.tools.gateway")

    result = gateway.execute_tool_call(
        {"name": "create_timer", "arguments": {"duration_s": 60}},
        app_state=None,
        turn_id=1,
    )

    assert result.success is True
    assert result.result["duration_s"] == 60

def test_build_messages_second_step_has_extra_messages() -> None:
    """build_messages retorna 4 mensagens quando tool_call_result está presente."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    ctx = {
        "tool_call_result": {
            "tool_name": "create_timer",
            "success": True,
            "vetoed": False,
            "result": {"duration_s": 60},
            "error": None,
            "veto_reason": None,
        },
        "first_assistant_json": '{"expression_id":"happy","reply":"Ok!","tool_call":{"name":"create_timer","arguments":{}}}',
    }
    msgs = llm_module.build_messages("crie um timer de 1 minuto", ctx)

    assert len(msgs) == 4
    roles = [m["role"] for m in msgs]
    assert roles == ["system", "user", "assistant", "user"]

def test_build_messages_second_step_without_first_json_has_three_messages() -> None:
    """Sem first_assistant_json, segundo passo tem 3 mensagens (sem assistant)."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    ctx = {
        "tool_call_result": {
            "tool_name": "set_expression",
            "success": True,
            "vetoed": False,
            "result": {"expression_id": "happy"},
            "error": None,
            "veto_reason": None,
        },
        "first_assistant_json": "",  # vazio → sem assistant message
    }
    msgs = llm_module.build_messages("mostre expressão feliz", ctx)

    assert len(msgs) == 3
    roles = [m["role"] for m in msgs]
    assert roles == ["system", "user", "user"]

def test_build_messages_second_step_injection_in_last_user_message() -> None:
    """A última mensagem de usuário contém o resultado da ferramenta."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    ctx = {
        "tool_call_result": {
            "tool_name": "create_reminder",
            "success": True,
            "vetoed": False,
            "result": {"reminder_id": "r42"},
            "error": None,
            "veto_reason": None,
        },
        "first_assistant_json": '{"expression_id":"neutral","reply":"Vou criar.","tool_call":null}',
    }
    msgs = llm_module.build_messages("lembre-me às 9h", ctx)

    last_content = msgs[-1]["content"]
    assert "create_reminder" in last_content
    assert "sucesso" in last_content
    assert "tool_call" in last_content  # instrução anti-loop

def test_build_messages_normal_turn_not_affected() -> None:
    """Turnos sem tool_call_result continuam retornando 2 mensagens."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    msgs = llm_module.build_messages("oi tudo bem?", {})

    assert len(msgs) == 2
    assert msgs[0]["role"] == "system"
    assert msgs[1]["role"] == "user"

def test_build_messages_second_step_vetoed_describes_block() -> None:
    """Mensagem de injeção menciona bloqueio quando tool foi vetada."""
    llm_module = importlib.import_module("noisebot_server.internal.agent.llm")

    ctx = {
        "tool_call_result": {
            "tool_name": "move_servo",
            "success": False,
            "vetoed": True,
            "result": None,
            "error": "motion_safety nao liberada",
            "veto_reason": "motion_safety nao liberada",
        },
        "first_assistant_json": "",
    }
    msgs = llm_module.build_messages("vire para mim", ctx)

    last_content = msgs[-1]["content"]
    assert "bloqueada" in last_content
    assert "motion_safety" in last_content
