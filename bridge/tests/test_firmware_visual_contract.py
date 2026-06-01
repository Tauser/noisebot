from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BEHAVIOR_ENGINE = ROOT / "components" / "behavior" / "behavior_engine" / "behavior_engine.c"


def _source() -> str:
    return BEHAVIOR_ENGINE.read_text(encoding="utf-8")


def test_speech_cancel_clears_stale_text_overlay_before_listening():
    src = _source()

    start = src.index("case NB_EVT_BRIDGE_SPEECH_CANCEL:")
    end = src.index("NB_LOGI(TAG, \"SPEECH_CANCEL aplicado", start)
    block = src[start:end]

    assert "audio_play_stop();" in block
    assert "ui_overlay_clear_text();" in block
    assert block.index("ui_overlay_clear_text();") < block.index("ui_overlay_listening_set(true);")


def test_listen_start_clears_stale_text_overlay():
    src = _source()

    start = src.index('if (strstr(payload, "\\"event\\":\\"LISTEN_START\\""))')
    end = src.index('} else if (strstr(payload, "\\"event\\":\\"LISTEN_STOP\\"")', start)
    block = src[start:end]

    assert "ui_overlay_clear_text();" in block
    assert block.index("ui_overlay_clear_text();") < block.index("ui_overlay_listening_set(true);")
