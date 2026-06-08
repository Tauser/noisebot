from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
AUDIO_SERVICE = ROOT / "components" / "services" / "audio_service" / "audio_service.c"


def _source() -> str:
    return AUDIO_SERVICE.read_text(encoding="utf-8")


def test_barge_in_opens_capture_without_forcing_vad_active():
    src = _source()

    marker = "} else if (listen_start_bridge_capture()) {"
    start = src.index(marker)
    end = src.index("ESP_LOGI(TAG, \"bridge captura aberta imediatamente source=%s\"", start)
    block = src[start:end]

    assert "s.listen_voice_detected = true;" in block
    assert "s.listen_phase = LISTEN_PHASE_CAPTURING_SPEECH;" in block
    assert "s.listen_wait_remaining_ms = 0;" in block
    assert "s.listen_speech_elapsed_ms = 0;" in block
    assert "s.vad_state = VAD_ACTIVE;" not in block


def test_wake_word_still_waits_for_vad_before_bridge_capture():
    src = _source()

    wake_branch = (
        "if (source == NB_LISTEN_SOURCE_WAKE_WORD) {\n"
        "            ESP_LOGI(TAG, \"captura bridge aguardando VAD source=%s\","
    )

    assert wake_branch in src


def test_barge_in_still_suppresses_preroll():
    src = _source()

    assert "s.listen_skip_preroll         = (source == NB_LISTEN_SOURCE_BARGE_IN);" in src
    assert "ESP_LOGI(TAG, \"pre-roll suprimido para barge-in\");" in src
