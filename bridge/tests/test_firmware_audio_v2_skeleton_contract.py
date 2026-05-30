from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = ROOT / "components" / "services"
BOOT_MANAGER = ROOT / "components" / "infra" / "boot_manager.c"

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
