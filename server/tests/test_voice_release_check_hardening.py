import importlib


def test_voice_release_check_reports_server_metrics_failure(monkeypatch) -> None:
    release_check = importlib.import_module("noisebot_server.internal.ops.release_check")
    voice_ab = importlib.import_module("noisebot_server.internal.ops.voice_ab")

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
                "real_capture_enabled": True,
                "bridge_tx_handoff_enabled": True,
                "session_active": False,
                "state": "IDLE_SESSION",
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

    monkeypatch.setattr(release_check, "FirmwareDiagClient", FakeFirmware)

    def fake_get_json(_url: str) -> dict:
        raise voice_ab.VoiceAbError("http://127.0.0.1:8765/ai/metrics: timeout")

    monkeypatch.setattr(release_check, "get_json", fake_get_json)

    check = release_check.run_release_check(
        firmware_url="http://192.168.1.30",
        server_url="http://127.0.0.1:8765",
    )

    assert check.ok is False
    metrics_gate = check.gates[-1]
    assert metrics_gate.name == "Métricas de voz"
    assert metrics_gate.ok is False
    assert "ai/metrics" in metrics_gate.detail
    assert metrics_gate.warnings == ("verifique se o server local esta rodando",)
    assert "Status: FALHOU" in release_check.format_release_check_markdown(check)
