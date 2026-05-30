from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = ROOT / "components" / "services"
BOOT_MANAGER = ROOT / "components" / "infra" / "boot_manager.c"
CONFIG_H = ROOT / "components" / "infra" / "config_manager.h"
CONFIG_C = ROOT / "components" / "infra" / "config_manager.c"
CONFIG_KEYS = ROOT / "components" / "infra" / "nb_config_keys.h"

V2_COMPONENTS = [
    "audio_io_service_v2",
    "audio_playback_service_v2",
    "voice_activity_service_v2",
    "voice_capture_session_v2",
    "audio_codec_service_v2",
]


def test_audio_v2_components_exist_but_are_not_booted():
    boot = BOOT_MANAGER.read_text(encoding="utf-8")

    for name in V2_COMPONENTS:
        component = COMPONENTS / name
        assert (component / "CMakeLists.txt").exists()
        assert (component / f"{name}.h").exists()
        assert (component / f"{name}.c").exists()
        assert f'#include "{name}.h"' not in boot
        assert f"{name}_init()" not in boot


def test_audio_v2_contract_keeps_pcm16_default_and_opus_opt_in():
    codec_h = (COMPONENTS / "audio_codec_service_v2" / "audio_codec_service_v2.h").read_text(
        encoding="utf-8"
    )
    capture_h = (
        COMPONENTS / "voice_capture_session_v2" / "voice_capture_session_v2.h"
    ).read_text(encoding="utf-8")
    io_h = (COMPONENTS / "audio_io_service_v2" / "audio_io_service_v2.h").read_text(
        encoding="utf-8"
    )

    assert "#define NB_AUDIO_IO_V2_CHUNK_SAMPLES       256U" in io_h
    assert "#define NB_AUDIO_CODEC_V2_OPUS_FRAME_MS      60U" in codec_h
    assert "#define NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES 960U" in codec_h
    assert "#define NB_AUDIO_CODEC_V2_OPUS_BITRATE       32000U" in codec_h
    assert "NB_AUDIO_CODEC_V2_FORMAT_PCM16 = 0" in codec_h
    assert "#define NB_VOICE_CAPTURE_V2_WAIT_FOR_SPEECH_MS  8000U" in capture_h
    assert "#define NB_VOICE_CAPTURE_V2_END_SILENCE_MS      900U" in capture_h
    assert "#define NB_VOICE_CAPTURE_V2_MAX_SPEECH_MS       9200U" in capture_h
    assert "#define NB_VOICE_CAPTURE_V2_PREROLL_CHUNKS      20U" in capture_h


def test_audio_io_v2_probe_is_explicit_and_passive():
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    io_h = (COMPONENTS / "audio_io_service_v2" / "audio_io_service_v2.h").read_text(
        encoding="utf-8"
    )

    assert "audio_io_service_v2_probe_feed_rx_frame(s_sa_buf, (uint16_t)mic_n);" in audio_service
    assert "audio_io_service_v2_probe_note_tx_silence(wr);" in audio_service
    assert '{ .uri = "/api/audio/io-v2"' in web
    assert '{ .uri = "/api/audio/io-v2/probe"' in web
    assert "audio_service_is_busy()" in web
    assert "esp_err_t audio_io_service_v2_probe_start(uint32_t duration_ms);" in io_h
    assert "void audio_io_service_v2_probe_feed_rx_frame(" in io_h


def test_audio_playback_v2_probe_is_explicit_and_hal_owned_by_audio_service():
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    playback_c = (
        COMPONENTS / "audio_playback_service_v2" / "audio_playback_service_v2.c"
    ).read_text(encoding="utf-8")
    playback_h = (
        COMPONENTS / "audio_playback_service_v2" / "audio_playback_service_v2.h"
    ).read_text(encoding="utf-8")

    assert "audio_playback_service_v2_fill_probe_chunk(" in audio_service
    assert 'audio_note_spk_result(wr, "playback_v2_probe");' in audio_service
    assert '{ .uri = "/api/audio/playback-v2"' in web
    assert '{ .uri = "/api/audio/playback-v2/probe"' in web
    assert '{ .uri = "/api/audio/playback-v2/stop"' in web
    assert "audio_service_is_busy()" in web
    assert "esp_err_t audio_playback_service_v2_probe_start(" in playback_h
    assert "bool audio_playback_service_v2_fill_probe_chunk(" in playback_h
    assert "audio_hal_" not in playback_c


def test_voice_capture_v2_replay_is_explicit_and_does_not_touch_bridge():
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    capture_c = (
        COMPONENTS / "voice_capture_session_v2" / "voice_capture_session_v2.c"
    ).read_text(encoding="utf-8")
    capture_h = (
        COMPONENTS / "voice_capture_session_v2" / "voice_capture_session_v2.h"
    ).read_text(encoding="utf-8")

    assert '{ .uri = "/api/audio/capture-v2"' in web
    assert '{ .uri = "/api/audio/capture-v2/replay"' in web
    assert '{ .uri = "/api/audio/capture-v2/cancel"' in web
    assert "audio_service_is_busy()" in web
    assert "esp_err_t voice_capture_session_v2_replay_start(" in capture_h
    assert "esp_err_t voice_capture_session_v2_cancel(void);" in capture_h
    assert "bridge_service" not in capture_c
    assert "VOICE_START" not in capture_c
    assert "AUDIO_CHUNK" not in capture_c
    assert "VOICE_END" not in capture_c


def test_voice_capture_v2_real_path_is_opt_in_config_flag():
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    config_h = CONFIG_H.read_text(encoding="utf-8")
    config_c = CONFIG_C.read_text(encoding="utf-8")
    config_keys = CONFIG_KEYS.read_text(encoding="utf-8")

    assert '#define NB_CFG_KEY_V2_CAP_EN  "v2cap_en"' in config_keys
    assert "#define NB_CFG_DEFAULT_V2_CAP_EN          0" in config_keys
    assert "NB_CFG_SCHEMA_VERSION  3U" in config_keys
    assert "bool      config_get_voice_audio_v2_capture_enabled(void);" in config_h
    assert "esp_err_t config_set_voice_audio_v2_capture_enabled(bool enabled);" in config_h
    assert "ensure_voice_audio_v2_defaults" in config_c
    assert "voice_audio_v2_capture_enabled" in web
    assert '\\"real_capture_enabled\\":%s,' in web
    assert "config_get_voice_audio_v2_capture_enabled()" in audio_service
    assert "voice_capture_session_v2_begin_real_pcm16(" in audio_service
    assert "capture v2 real indisponivel" in audio_service
