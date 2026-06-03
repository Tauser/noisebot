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
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    infra_cmake = (ROOT / "components" / "infra" / "CMakeLists.txt").read_text(
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
    assert "#define NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS 40U" in codec_h
    assert "NB_AUDIO_CODEC_V2_FORMAT_PCM16 = 0" in codec_h
    assert "NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED = 0" in codec_h
    assert "NB_AUDIO_CODEC_V2_WORKER_STATE_RUNNING" in codec_h
    assert "NB_AUDIO_CODEC_V2_WORKER_STATE_STOPPED" in codec_h
    assert "nb_audio_codec_v2_worker_state_t worker_state;" in codec_h
    assert "bool worker_supported;" in codec_h
    assert "bool worker_active;" in codec_h
    assert "uint32_t worker_drained_packets;" in codec_h
    assert "uint32_t worker_opus_packets;" in codec_h
    assert "uint32_t worker_opus_encoded_bytes_total;" in codec_h
    assert "uint16_t worker_opus_last_packet_bytes;" in codec_h
    assert "NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES 16U" in codec_h
    assert "uint32_t worker_payload_packets;" in codec_h
    assert "uint32_t worker_payload_bytes_total;" in codec_h
    assert "uint32_t worker_payload_last_checksum;" in codec_h
    assert "uint8_t worker_payload_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];" in codec_h
    assert "uint32_t opus_egress_packets_in;" in codec_h
    assert "uint32_t opus_egress_packets_drained;" in codec_h
    assert "uint32_t opus_egress_packet_drops;" in codec_h
    assert "uint32_t opus_egress_queue_count;" in codec_h
    assert "uint8_t opus_egress_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];" in codec_h
    assert "bool bridge_handoff_supported;" in codec_h
    assert "uint32_t bridge_handoff_packets_ready;" in codec_h
    assert "uint8_t bridge_handoff_preview[NB_AUDIO_CODEC_V2_PAYLOAD_PREVIEW_BYTES];" in codec_h
    assert "uint32_t opus_encode_tests;" in codec_h
    assert "uint32_t opus_encoded_bytes_total;" in codec_h
    assert "uint16_t opus_last_packet_bytes;" in codec_h
    assert "int opus_codec_error;" in codec_h
    assert "const char *audio_codec_service_v2_worker_state_name(" in codec_h
    assert "nb_audio_codec_v2_opus_test_result_t" in codec_h
    assert '{ .uri = "/api/audio/codec-v2"' in web
    assert '{ .uri = "/api/audio/codec-v2/encode-test"' in web
    assert '{ .uri = "/api/audio/codec-v2/drain"' in web
    assert '{ .uri = "/api/audio/codec-v2/egress/drain"' in web
    assert '{ .uri = "/api/audio/codec-v2/reset"' in web
    assert '{ .uri = "/api/audio/codec-v2/opus-encode-test"' in web
    assert '{ .uri = "/api/audio/codec-v2/worker/start"' in web
    assert '{ .uri = "/api/audio/codec-v2/worker/stop"' in web
    assert '{ .uri = "/api/audio/codec-v2/worker/feed-test"' in web
    assert '{ .uri = "/api/audio/codec-v2/bridge-handoff-test"' in web
    assert '{ .uri = "/api/audio/codec-v2/transport/enable"' in web
    assert '{ .uri = "/api/audio/codec-v2/transport/disable"' in web
    assert '{ .uri = "/api/audio/codec-v2/overflow-test"' in web
    assert '\\"opus_bitrate\\":%u' in web
    assert '\\"max_queue_packets\\":%u' in web
    assert "NB_AUDIO_CODEC_V2_OPUS_BITRATE" in web
    assert "NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS" in web
    assert "uint16_t pending_samples;" in codec_h
    assert "esp_err_t audio_codec_service_v2_feed_pcm16(" in codec_h
    assert "esp_err_t audio_codec_service_v2_encode_test_once(void);" in codec_h
    assert "esp_err_t audio_codec_service_v2_drain_synthetic(" in codec_h
    assert "esp_err_t audio_codec_service_v2_reset_diagnostics(void);" in codec_h
    assert "NB_AUDIO_CODEC_V2_OVERFLOW_TEST_MAX_PACKETS 200U" in codec_h
    assert "NB_AUDIO_CODEC_V2_WORKER_STRESS_MAX_PACKETS" in codec_h
    assert "nb_audio_codec_v2_overflow_test_result_t" in codec_h
    assert "nb_audio_codec_v2_worker_stress_result_t" in codec_h
    assert "nb_audio_codec_v2_worker_feed_result_t" in codec_h
    codec_c = (
        COMPONENTS / "audio_codec_service_v2" / "audio_codec_service_v2.c"
    ).read_text(encoding="utf-8")
    assert "NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES" in codec_c
    assert "enqueue_synthetic_packet" in codec_c
    assert "s_pcm_queue_count >= NB_AUDIO_CODEC_V2_MAX_QUEUE_PACKETS" in codec_c
    assert "s_status.queue_count = s_pcm_queue_count;" in codec_c
    assert "s_status.packet_drops++" in codec_c
    assert "s_pcm_queue_count++" in codec_c
    assert "*drained_packets = s_status.queue_count;" in codec_c
    assert "clear_pcm_queue();" in codec_c
    assert "was_initialized = s_status.initialized" in codec_c
    assert "worker_active = s_status.worker_active" in codec_c
    assert "worker_state = s_status.worker_state" in codec_c
    assert "s_status.initialized = was_initialized;" in codec_c
    assert "s_status.worker_active = worker_active;" in codec_c
    assert "s_status.worker_state = worker_state;" in codec_c
    assert "s_status.format = NB_AUDIO_CODEC_V2_FORMAT_PCM16;" in codec_c
    assert "reset_worker_status();" in codec_c
    assert "NB_AUDIO_CODEC_V2_WORKER_STATE_NOT_STARTED" in codec_c
    assert 'return "not_started";' in codec_c
    assert "audio_codec_service_v2_overflow_test(" in codec_c
    assert "audio_codec_service_v2_opus_encode_test(" in codec_c
    assert "audio_codec_service_v2_worker_start(" in codec_c
    assert "audio_codec_service_v2_worker_stop(" in codec_c
    assert "audio_codec_service_v2_worker_stress_test(" in codec_c
    assert "audio_codec_service_v2_worker_feed_test(" in codec_c
    assert "audio_codec_service_v2_bridge_handoff_test(" in codec_c
    assert "bridge_handoff_stub = true" in codec_c
    assert "bridge_packet_not_sent = true" in codec_c
    assert '"nb_codec_v2_worker"' in codec_c
    assert "#define CODEC_WORKER_TASK_STACK OPUS_TEST_TASK_STACK" in codec_c
    assert "xTaskCreatePinnedToCoreWithCaps(" in codec_c
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in codec_c
    assert "vTaskDeleteWithCaps(NULL);" in codec_c
    assert "CODEC_WORKER_STOP_TIMEOUT_MS" in codec_c
    assert "s_status.worker_drained_packets++;" in codec_c
    assert "s_status.worker_opus_packets++;" in codec_c
    assert "s_status.worker_opus_encoded_bytes_total += encoded_bytes;" in codec_c
    assert "s_status.worker_opus_last_packet_bytes = encoded_bytes;" in codec_c
    assert "observe_worker_payload(s_worker_opus_out, encoded_bytes);" in codec_c
    assert "enqueue_opus_egress_packet(s_worker_opus_out, encoded_bytes);" in codec_c
    assert "audio_codec_service_v2_drain_opus_egress(" in codec_c
    assert "s_opus_egress_count >= NB_AUDIO_CODEC_V2_MAX_EGRESS_PACKETS" in codec_c
    assert "s_status.opus_egress_packets_drained += s_opus_egress_count;" in codec_c
    assert "audio_codec_service_v2_read_opus_packet(" in codec_c
    assert "checksum_payload(" in codec_c
    assert "esp_opus_enc_open" in codec_c
    assert "esp_opus_enc_process" in codec_c
    assert "esp_opus_enc_close" in codec_c
    assert "ESP_OPUS_ENC_FRAME_DURATION_60_MS" in codec_c
    assert "ESP_OPUS_ENC_APPLICATION_AUDIO" in codec_c
    assert ".enable_dtx         = true" in codec_c
    assert ".enable_vbr         = true" in codec_c
    assert "out->attempted_packets = packets;" in codec_c
    assert "out->status_packet_drops_after_cleanup = s_status.packet_drops;" in codec_c
    assert "CODEC_WORKER_STRESS_TIMEOUT_MS" in codec_c
    assert "out->worker_opus_packets_delta" in codec_c
    assert "out->worker_opus_encoded_bytes_delta" in codec_c
    assert "out->pcm_frames_in_delta" in codec_c
    assert "out->pending_samples_after" in codec_c
    assert "out->worker_payload_packets_delta" in codec_c
    assert "out->worker_payload_last_checksum" in codec_c
    assert '{ .uri = "/api/audio/codec-v2/worker/stress-test"' in web
    assert '{ .uri = "/api/audio/codec-v2/worker/feed-test"' in web
    assert '\\"pending_samples\\":%u' in web
    assert '\\"worker_supported\\":%s' in web
    assert '\\"worker_active\\":%s' in web
    assert '\\"worker_state\\":\\"%s\\"' in web
    assert '\\"worker_drained_packets\\":%lu' in web
    assert '\\"worker_opus_packets\\":%lu' in web
    assert '\\"worker_opus_encoded_bytes_total\\":%lu' in web
    assert '\\"worker_opus_last_packet_bytes\\":%u' in web
    assert '\\"opus_encode_tests\\":%lu' in web
    assert '\\"opus_encoded_bytes_total\\":%lu' in web
    assert '\\"opus_last_packet_bytes\\":%u' in web
    assert '\\"test_format\\":\\"opus\\"' in web
    assert '\\"encoded_bytes\\":%u' in web
    assert '\\"drained_packets\\":%lu' in web
    assert '\\"intentional_overflow\\":true' in web
    assert '\\"worker_stress\\":true' in web
    assert '\\"worker_feed\\":true' in web
    assert '\\"worker_payload_observer\\":true' in web
    assert '\\"opus_egress_queue\\":true' in web
    assert '\\"opus_egress_packets_in\\":%lu' in web
    assert '\\"opus_egress_queue_count\\":%lu' in web
    assert '\\"opus_egress_packets_delta\\":%lu' in web
    assert '\\"opus_egress_drained_after_test\\":%lu' in web
    assert '\\"opus_egress_queue_count_after_cleanup\\":%lu' in web
    assert '\\"opus_egress_drain\\":true' in web
    assert '\\"bridge_handoff_stub\\":%s' in web
    assert '\\"bridge_transport_unchanged\\":%s' in web
    assert '\\"bridge_packet_not_sent\\":%s' in web
    assert '\\"bridge_handoff_packets_ready_delta\\":%lu' in web
    assert '\\"bridge_handoff_preview_hex\\":\\"%s\\"' in web
    assert '\\"codec_v2_transport\\":true' in web
    assert '\\"live_bridge_transport\\":%s' in web
    assert '\\"pcm16_fallback\\":true' in web
    assert '\\"transport_worker\\":\\"audio_codec_service_v2\\"' in web
    assert '\\"compat_worker\\":\\"audio_codec_service_v2\\"' in web
    assert '\\"egress_drained_packets\\":%lu' in web
    assert "audio_codec_service_v2_drain_opus_egress(&egress_drained_packets)" in web
    assert '\\"worker_payload_packets_delta\\":%lu' in web
    assert '\\"worker_payload_preview_hex\\":\\"%s\\"' in web
    assert '\\"pcm_frames_in_delta\\":%lu' in web
    assert '\\"pending_samples_after\\":%u' in web
    assert '\\"worker_opus_packets_delta\\":%lu' in web
    assert '\\"worker_opus_encoded_bytes_delta\\":%lu' in web
    assert '\\"queue_count_after_cleanup\\":%lu' in web
    assert "audio_codec_service_v2_encode_test_once()" in web
    assert "audio_codec_service_v2_init()" not in web
    assert "xTaskCreatePinnedToCore(" in codec_c
    assert '"nb_codec_v2_opus_test"' in codec_c
    assert "OPUS_TEST_TASK_STACK" in codec_c
    assert "OPUS_TEST_TIMEOUT_MS" in codec_c
    assert "audio_codec_service_v2" in infra_cmake
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
    io_c = (COMPONENTS / "audio_io_service_v2" / "audio_io_service_v2.c").read_text(
        encoding="utf-8"
    )

    assert "rx_dispatch_sound_analysis_cb" in audio_service
    assert "rx_dispatch_processor_shadow_cb" in audio_service
    assert "rx_dispatch_io_probe_cb" in audio_service
    assert "rx_dispatch_session_mirror_cb" in audio_service
    assert "rx_dispatch_activity_cb" in audio_service
    assert "rx_dispatch_vad_cb" in audio_service
    assert "rx_dispatch_preroll_cb" in audio_service
    assert "rx_dispatch_bridge_tx_cb" in audio_service
    assert "wake_service_feed(dispatch->wake_samples, frame->sample_count);" in audio_service
    assert "vad_update(dispatch->mic_samples," in audio_service
    assert "bridge_service_send_audio_chunk(s_bridge_buf, frame->sample_count)" in audio_service
    assert "audio_io_service_v2_probe_feed_rx_frame(frame->samples, frame->sample_count);" in audio_service
    assert "audio_io_service_v2_rx_dispatch_frame(s_sa_buf," in audio_service
    assert "NULL);" in audio_service
    assert "const int16_t *rx_samples = rx_frame.samples;" not in audio_service
    assert "uint16_t rx_sample_count = rx_frame.sample_count;" not in audio_service
    assert "/* ── 4c. Bridge mic streaming" not in audio_service
    assert audio_service.index(
        "audio_io_service_v2_rx_dispatch_frame(s_sa_buf,"
    ) < audio_service.index("/* ── 4d. Session timeouts")
    assert "sound_analysis_tick(frame->samples, frame->sample_count);" in audio_service
    assert "audio_processor_service_feed_shadow(frame->samples, frame->sample_count);" in audio_service
    assert "voice_activity_service_v2_feed_frame(frame->samples," in audio_service
    assert "audio_io_service_v2_tx_owner_note_frame(sample_count, silence, err);" in audio_service
    assert "audio_io_service_v2_note_i2s_recovery(err);" in audio_service
    assert "audio_io_service_v2_session_rx_mirror_begin((uint32_t)source);" in audio_service
    assert "audio_io_service_v2_session_rx_mirror_feed(frame->samples," in audio_service
    assert "{ .cb = rx_dispatch_vad_cb, .ctx = &rx_dispatch }" in audio_service
    assert "{ .cb = rx_dispatch_preroll_cb, .ctx = &rx_dispatch }" in audio_service
    assert "{ .cb = rx_dispatch_bridge_tx_cb, .ctx = NULL }" in audio_service
    assert "audio_io_service_v2_session_rx_mirror_finish((uint32_t)reason," in audio_service
    assert "legacy_tx_frames" in audio_service
    assert "legacy_tx_samples" in audio_service
    assert '{ .uri = "/api/audio/io-v2"' in web
    assert '{ .uri = "/api/audio/io-v2/probe"' in web
    assert "audio_service_is_busy()" in web
    assert '\\"rx_owner_active\\":%s' in web
    assert '\\"rx_owner_observed\\":%s' in web
    assert '\\"rx_owner_frames\\":%lu' in web
    assert '\\"rx_distributor_frames\\":%lu' in web
    assert '\\"rx_distributor_last_timestamp_ms\\":%lu' in web
    assert '\\"rx_dispatch_calls\\":%lu' in web
    assert '\\"rx_dispatch_last_consumers\\":%lu' in web
    assert '\\"session_rx_owner_frames\\":%lu' in web
    assert '\\"session_rx_distributor_frames\\":%lu' in web
    assert '\\"session_rx_dispatch_consumers\\":%lu' in web
    assert '\\"tx_owner_observed\\":%s' in web
    assert '\\"tx_owner_frames\\":%lu' in web
    assert '\\"tx_owner_samples\\":%lu' in web
    assert '\\"tx_owner_last_samples\\":%lu' in web
    assert '\\"tx_owner_last_silence\\":%s' in web
    assert '\\"tx_owner_last_result\\":\\"%s\\"' in web
    assert '\\"speaker_handoff_supported\\":%s' in web
    assert '\\"speaker_handoff_dry_run_enabled\\":%s' in web
    assert '\\"speaker_handoff_owner_requested\\":%s' in web
    assert '\\"speaker_handoff_owner_ready\\":%s' in web
    assert '\\"speaker_handoff_active\\":%s' in web
    assert '\\"speaker_handoff_candidate\\":%s' in web
    assert '\\"speaker_handoff_ready\\":%s' in web
    assert '\\"speaker_handoff_block_reason\\":\\"%s\\"' in web
    assert '\\"speaker_handoff_frames\\":%lu' in web
    assert '\\"speaker_handoff_samples\\":%lu' in web
    assert '\\"speaker_handoff_silence_frames\\":%lu' in web
    assert '\\"speaker_handoff_failures\\":%lu' in web
    assert '\\"speaker_handoff_recoveries\\":%lu' in web
    assert '\\"speaker_handoff_last_samples\\":%lu' in web
    assert '\\"speaker_handoff_last_result\\":\\"%s\\"' in web
    assert '\\"session_rx_mirror_active\\":%s' in web
    assert '\\"session_rx_mirror_observed\\":%s' in web
    assert '\\"session_rx_legacy_observed\\":%s' in web
    assert '\\"session_rx_legacy_covered\\":%s' in web
    assert '\\"session_rx_mirror_frames\\":%lu' in web
    assert '\\"session_rx_legacy_frames\\":%lu' in web
    assert '\\"session_rx_compare_sample_delta\\":%lu' in web
    assert "esp_err_t audio_io_service_v2_probe_start(uint32_t duration_ms);" in io_h
    assert "void audio_io_service_v2_rx_owner_accept_frame(" in io_h
    assert "void audio_io_service_v2_rx_dispatch_frame(" in io_h
    assert "nb_audio_io_v2_rx_consumer_t" in io_h
    assert "void audio_io_service_v2_probe_feed_rx_frame(" in io_h
    assert "void audio_io_service_v2_tx_owner_note_frame(" in io_h
    assert "void audio_io_service_v2_speaker_handoff_note_playback_frame(" in io_h
    assert "void audio_io_service_v2_note_i2s_recovery(" in io_h
    assert "esp_err_t audio_io_service_v2_set_speaker_handoff_dry_run(bool enabled);" in io_h
    assert "esp_err_t audio_io_service_v2_set_speaker_handoff_owner_requested(bool requested);" in io_h
    assert "NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED" in io_h
    assert "NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_NO_TX" in io_h
    assert "NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_TX_ERROR" in io_h
    assert "NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_I2S_RECOVERY" in io_h
    assert "esp_err_t audio_io_service_v2_session_rx_mirror_begin(uint32_t source);" in io_h
    assert "void audio_io_service_v2_session_rx_mirror_feed(" in io_h
    assert "void audio_io_service_v2_session_rx_mirror_finish(" in io_h
    assert "bool session_rx_mirror_active;" in io_h
    assert "bool rx_owner_active;" in io_h
    assert "bool session_rx_legacy_covered;" in io_h
    assert "uint32_t rx_owner_frames;" in io_h
    assert "uint32_t rx_distributor_frames;" in io_h
    assert "uint32_t rx_distributor_last_timestamp_ms;" in io_h
    assert "uint32_t rx_dispatch_calls;" in io_h
    assert "uint32_t rx_dispatch_last_consumers;" in io_h
    assert "uint32_t session_rx_owner_samples;" in io_h
    assert "uint32_t session_rx_distributor_samples;" in io_h
    assert "uint32_t session_rx_dispatch_consumers;" in io_h
    assert "bool tx_owner_observed;" in io_h
    assert "uint32_t tx_owner_frames;" in io_h
    assert "uint32_t tx_owner_samples;" in io_h
    assert "uint32_t tx_owner_last_samples;" in io_h
    assert "bool tx_owner_last_silence;" in io_h
    assert "esp_err_t tx_owner_last_result;" in io_h
    assert "bool speaker_handoff_supported;" in io_h
    assert "bool speaker_handoff_dry_run_enabled;" in io_h
    assert "bool speaker_handoff_owner_requested;" in io_h
    assert "bool speaker_handoff_owner_ready;" in io_h
    assert "bool speaker_handoff_active;" in io_h
    assert "bool speaker_handoff_candidate;" in io_h
    assert "bool speaker_handoff_ready;" in io_h
    assert "nb_audio_io_v2_speaker_handoff_block_t speaker_handoff_block_reason;" in io_h
    assert "uint32_t speaker_handoff_frames;" in io_h
    assert "uint32_t speaker_handoff_samples;" in io_h
    assert "uint32_t speaker_handoff_silence_frames;" in io_h
    assert "uint32_t speaker_handoff_failures;" in io_h
    assert "uint32_t speaker_handoff_recoveries;" in io_h
    assert "uint32_t speaker_handoff_last_samples;" in io_h
    assert "esp_err_t speaker_handoff_last_result;" in io_h
    assert "uint32_t session_rx_mirror_frames;" in io_h
    assert "uint32_t session_rx_legacy_samples;" in io_h
    assert "uint32_t session_rx_compare_elapsed_delta_ms;" in io_h
    assert "audio_hal_" not in io_c
    assert "bridge_" not in io_c
    assert "speaker_handoff_active = false;" in io_c
    assert "audio_io_service_v2_speaker_handoff_note_playback_frame(" in io_c
    assert "s_status.speaker_handoff_active = true;" in io_c
    assert "audio_io_service_v2_set_speaker_handoff_dry_run(" in io_c
    assert "audio_io_service_v2_set_speaker_handoff_owner_requested(" in io_c
    assert '{ .uri = "/api/audio/io-v2/speaker-handoff/enable"' in web
    assert '{ .uri = "/api/audio/io-v2/speaker-handoff/disable"' in web
    assert '{ .uri = "/api/audio/io-v2/speaker-handoff/owner/arm"' in web
    assert '{ .uri = "/api/audio/io-v2/speaker-handoff/owner/disarm"' in web


def test_voice_activity_v2_shadow_is_explicit_and_passive():
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    activity_c = (
        COMPONENTS / "voice_activity_service_v2" / "voice_activity_service_v2.c"
    ).read_text(encoding="utf-8")
    activity_h = (
        COMPONENTS / "voice_activity_service_v2" / "voice_activity_service_v2.h"
    ).read_text(encoding="utf-8")

    assert "voice_activity_service_v2_feed_frame(" in audio_service
    assert "voice_activity_service_v2_session_compare_begin()" in audio_service
    assert "voice_activity_service_v2_session_compare_legacy_end(" in audio_service
    assert "activity_session_context" in audio_service
    assert "activity_playback_context" in audio_service
    assert "audio_playback_service_v2_is_playing()" in audio_service
    assert '{ .uri = "/api/audio/activity-v2"' in web
    assert '{ .uri = "/api/audio/activity-v2/shadow"' in web
    assert '{ .uri = "/api/audio/activity-v2/shadow/stop"' in web
    assert '\\"shadow_running\\":%s' in web
    assert '\\"session_compare_active\\":%s' in web
    assert '\\"activity_end_observed\\":%s' in web
    assert '\\"legacy_end_observed\\":%s' in web
    assert '\\"decision_diverged\\":%s' in web
    assert "zcr_last_permille" in web
    assert "session_frames" in web
    assert "speech_run_frames" in web
    assert "silence_run_max_frames" in web
    assert "muted_rms_max" in web
    assert "unmuted_rms_max" in web
    assert "esp_err_t voice_activity_service_v2_shadow_start(" in activity_h
    assert "esp_err_t voice_activity_service_v2_session_compare_begin(void);" in activity_h
    assert "void voice_activity_service_v2_session_compare_legacy_end(" in activity_h
    assert "bool voice_activity_service_v2_session_end_observed(" in activity_h
    assert "void voice_activity_service_v2_note_decider_end(" in activity_h
    assert "void voice_activity_service_v2_feed_frame(" in activity_h
    assert "bool session_compare_active;" in activity_h
    assert "bool decision_diverged;" in activity_h
    assert "bool activity_decider_end_used;" in activity_h
    assert "uint32_t activity_end_silence_ms;" in activity_h
    assert "zcr_last_permille" in activity_h
    assert "session_frames" in activity_h
    assert "speech_run_frames" in activity_h
    assert "silence_run_max_frames" in activity_h
    assert "muted_rms_max" in activity_h
    assert "unmuted_rms_max" in activity_h
    assert "SHADOW_SPEECH_RMS_THRESHOLD" in activity_c
    assert "SESSION_SPEECH_RMS_THRESHOLD" in activity_c
    assert "SESSION_SPEECH_PEAK_THRESHOLD" in activity_c
    assert "? SESSION_SPEECH_RMS_THRESHOLD" in activity_c
    assert "? SESSION_SPEECH_PEAK_THRESHOLD" in activity_c
    assert "#define SESSION_END_SILENCE_MS          600U" in activity_c
    assert "voice_activity_service_v2_session_end_observed(" in activity_c
    assert "voice_activity_service_v2_note_decider_end(" in activity_c
    assert "if (!s_status.shadow_running && !s_status.session_compare_active)" in activity_c
    assert "s_status.activity_end_observed = true;" in activity_c
    assert "#define SHADOW_MAX_DURATION_MS          30000U" in activity_c
    assert "zcr_permille(" in activity_c
    assert "session_active" in activity_c
    assert "speech_run_max_frames" in activity_c
    assert "silence_run_max_frames" in activity_c
    assert "unmuted_frames" in activity_c
    assert "bridge_service" not in activity_c
    assert "wake_service" not in activity_c
    assert "audio_hal_" not in activity_c


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
    assert 'audio_note_spk_result(wr, "playback_v2_probe",' in audio_service
    assert '{ .uri = "/api/audio/playback-v2"' in web
    assert '{ .uri = "/api/audio/playback-v2/probe"' in web
    assert '{ .uri = "/api/audio/playback-v2/stop"' in web
    assert '{ .uri = "/api/audio/playback-v2/speaker-owner/arm"' in web
    assert '{ .uri = "/api/audio/playback-v2/speaker-owner/disarm"' in web
    assert '{ .uri = "/api/audio/playback-v2/speaker-owner/real-arm"' in web
    assert '{ .uri = "/api/audio/playback-v2/speaker-owner/real-disarm"' in web
    assert "audio_service_is_busy()" in web
    assert "esp_err_t audio_playback_service_v2_probe_start(" in playback_h
    assert "bool audio_playback_service_v2_fill_probe_chunk(" in playback_h
    assert "esp_err_t audio_playback_service_v2_speaker_owner_arm(void);" in playback_h
    assert "esp_err_t audio_playback_service_v2_speaker_owner_disarm(void);" in playback_h
    assert "esp_err_t audio_playback_service_v2_speaker_owner_real_arm(void);" in playback_h
    assert "esp_err_t audio_playback_service_v2_speaker_owner_real_disarm(void);" in playback_h
    assert "s_status.say_chunks_dropped > 0U" in playback_c
    assert "s_status.say_chunks_dropped_listening > 0U" in playback_c
    assert "s_status.speaker_write_failures > 0U" in playback_c
    assert "s_status.speaker_commit_failures > 0U" in playback_c
    assert "NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS" in playback_c
    assert "pdMS_TO_TICKS(NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS)" in playback_c
    assert "say_queue_high_watermark" in playback_h
    assert "say_accept_wait_ms" in playback_h
    assert "say_chunks_dropped_queue_full" in playback_h
    assert "say_chunks_queue_full" in playback_h
    assert "say_chunks_queue_wait_recovered" in playback_h
    assert "nb_audio_playback_v2_speaker_write_cb_t" in playback_h
    assert "audio_playback_service_v2_speaker_write_next_frame(" in playback_h
    assert "nb_audio_playback_v2_say_chunk_t" not in playback_h
    assert "NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS" not in playback_h
    assert "NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES" not in playback_h
    assert "NB_AUDIO_PLAYBACK_V2_CHUNK_MS" not in playback_h
    assert "audio_playback_service_v2_speaker_next_frame(" not in playback_h
    assert "audio_playback_service_v2_speaker_commit_frame(" not in playback_h
    assert "audio_playback_service_v2_say_accept(" in playback_h
    assert "audio_playback_service_v2_say_cancel_active(" in playback_h
    assert "audio_playback_service_v2_say_drop_listening(" in playback_h
    assert "audio_playback_service_v2_say_end_idle(" in playback_h
    assert "bridge_say_observer" in playback_h
    assert "bridge_say_queue_owner" in playback_h
    assert "bool speaker_owner_dry_run_enabled;" in playback_h
    assert "bool speaker_owner_requested;" in playback_h
    assert "bool speaker_owner_ready;" in playback_h
    assert "bool speaker_owner_active;" in playback_h
    assert "bool speaker_owner_candidate;" in playback_h
    assert "bool speaker_owner_handoff_ready;" in playback_h
    assert "uint32_t speaker_owner_block_reason;" in playback_h
    assert "bool speaker_owner_real_requested;" in playback_h
    assert "bool speaker_owner_real_armed;" in playback_h
    assert "uint32_t speaker_owner_real_block_reason;" in playback_h
    assert "bool speaker_owner_real_window_active;" in playback_h
    assert "bool speaker_owner_real_window_completed;" in playback_h
    assert "uint32_t speaker_owner_real_auto_disarm_count;" in playback_h
    assert "uint32_t speaker_owner_real_write_frames;" in playback_h
    assert "uint32_t speaker_owner_real_write_samples;" in playback_h
    assert "uint32_t speaker_owner_real_write_failures;" in playback_h
    assert "esp_err_t speaker_owner_real_last_result;" in playback_h
    assert "uint32_t speaker_owner_frames;" in playback_h
    assert "uint32_t speaker_owner_samples;" in playback_h
    assert "uint32_t speaker_owner_silence_frames;" in playback_h
    assert "uint32_t speaker_owner_failures;" in playback_h
    assert "uint32_t speaker_owner_recoveries;" in playback_h
    assert "uint32_t speaker_owner_last_samples;" in playback_h
    assert "esp_err_t speaker_owner_last_result;" in playback_h
    assert "uint32_t speaker_frames_prepared;" in playback_h
    assert "uint32_t speaker_samples_prepared;" in playback_h
    assert "uint32_t speaker_last_samples;" in playback_h
    assert "uint32_t speaker_last_volume;" in playback_h
    assert "uint32_t speaker_frames_committed;" in playback_h
    assert "uint32_t speaker_samples_committed;" in playback_h
    assert "uint32_t speaker_commit_failures;" in playback_h
    assert "uint32_t speaker_last_commit_samples;" in playback_h
    assert "esp_err_t speaker_last_commit_result;" in playback_h
    assert "uint32_t speaker_write_requests;" in playback_h
    assert "uint32_t speaker_write_samples;" in playback_h
    assert "uint32_t speaker_write_failures;" in playback_h
    assert "uint32_t speaker_last_write_samples;" in playback_h
    assert "esp_err_t speaker_last_write_result;" in playback_h
    assert "uint32_t speaker_empty_polls;" in playback_h
    assert "uint32_t speaker_empty_ms;" in playback_h
    assert "uint32_t speaker_idle_end_count;" in playback_h
    assert "say_chunks_received" in playback_h
    assert "audio_playback_service_v2_say_accept(" in audio_service
    assert "audio_playback_service_v2_speaker_write_next_frame(" in audio_service
    assert "audio_service_playback_v2_write_speaker" in audio_service
    assert "audio_playback_service_v2_speaker_commit_frame(n, wr);" not in audio_service
    assert "audio_playback_service_v2_speaker_should_end_idle()" in audio_service
    assert "audio_playback_service_v2_say_cancel_active(" in audio_service
    assert "audio_playback_service_v2_say_drop_listening(" in audio_service
    bridge_say_chunk_start = audio_service.index("void audio_service_bridge_say_chunk(")
    bridge_say_chunk = audio_service[bridge_say_chunk_start:]
    assert "audio_playback_service_v2_say_cancel_active(" not in bridge_say_chunk
    assert "NB_BRIDGE_AUDIO_CHUNK_SAMPLES" not in bridge_say_chunk
    assert "esp_err_t accept_err = audio_playback_service_v2_say_accept(samples, count);" in bridge_say_chunk
    assert "if (accept_err != ESP_OK)" in bridge_say_chunk
    assert bridge_say_chunk.index(
        "audio_playback_service_v2_say_accept(samples, count)"
    ) < bridge_say_chunk.index("s.play_state = PLAY_BRIDGE_SAY;")
    assert "audio_io_service_v2_speaker_handoff_note_playback_frame(false, wr);" not in audio_service
    assert "bridge_say_observer" in web
    assert "bridge_say_queue_owner" in web
    assert '\\"speaker_owner_dry_run_enabled\\":%s' in web
    assert '\\"speaker_owner_requested\\":%s' in web
    assert '\\"speaker_owner_ready\\":%s' in web
    assert '\\"speaker_owner_active\\":%s' in web
    assert '\\"speaker_owner_candidate\\":%s' in web
    assert '\\"speaker_owner_handoff_ready\\":%s' in web
    assert '\\"speaker_owner_block_reason\\":\\"%s\\"' in web
    assert '\\"speaker_owner_real_requested\\":%s' in web
    assert '\\"speaker_owner_real_armed\\":%s' in web
    assert '\\"speaker_owner_real_block_reason\\":\\"%s\\"' in web
    assert '\\"speaker_owner_real_window_active\\":%s' in web
    assert '\\"speaker_owner_real_window_completed\\":%s' in web
    assert '\\"speaker_owner_real_auto_disarm_count\\":%lu' in web
    assert '\\"speaker_owner_real_write_frames\\":%lu' in web
    assert '\\"speaker_owner_real_write_samples\\":%lu' in web
    assert '\\"speaker_owner_real_write_failures\\":%lu' in web
    assert '\\"speaker_owner_real_last_result\\":\\"%s\\"' in web
    assert '\\"speaker_owner_frames\\":%lu' in web
    assert '\\"speaker_owner_samples\\":%lu' in web
    assert '\\"speaker_owner_silence_frames\\":%lu' in web
    assert '\\"speaker_owner_failures\\":%lu' in web
    assert '\\"speaker_owner_recoveries\\":%lu' in web
    assert '\\"speaker_owner_last_samples\\":%lu' in web
    assert '\\"speaker_owner_last_result\\":\\"%s\\"' in web
    assert "audio_io_v2_speaker_handoff_block_name(st.speaker_owner_block_reason)" in web
    assert '\\"speaker_frames_prepared\\":%lu' in web
    assert '\\"speaker_samples_prepared\\":%lu' in web
    assert '\\"speaker_last_samples\\":%lu' in web
    assert '\\"speaker_last_volume\\":%lu' in web
    assert '\\"speaker_frames_committed\\":%lu' in web
    assert '\\"speaker_samples_committed\\":%lu' in web
    assert '\\"speaker_commit_failures\\":%lu' in web
    assert '\\"speaker_last_commit_samples\\":%lu' in web
    assert '\\"speaker_last_commit_result\\":\\"%s\\"' in web
    assert '\\"speaker_write_requests\\":%lu' in web
    assert '\\"speaker_write_samples\\":%lu' in web
    assert '\\"speaker_write_failures\\":%lu' in web
    assert '\\"speaker_last_write_samples\\":%lu' in web
    assert '\\"speaker_last_write_result\\":\\"%s\\"' in web
    assert '\\"speaker_empty_polls\\":%lu' in web
    assert '\\"speaker_empty_ms\\":%lu' in web
    assert '\\"speaker_idle_end_count\\":%lu' in web
    assert "say_chunks_received" in web
    assert "#define NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS   32U" in playback_c
    assert "#define NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES   256U" in playback_c
    assert "#define NB_AUDIO_PLAYBACK_V2_SAY_IDLE_END_MS 1200U" in playback_c
    assert "if (count > NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES)" in playback_c
    assert "count = NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES;" in playback_c
    assert "} nb_audio_playback_v2_say_chunk_t;" in playback_c
    assert "playback_v2_say_dequeue(out)" in playback_c
    assert "static bool playback_v2_speaker_next_frame(" in playback_c
    assert "volume_percent" in playback_c
    assert "write_cb(frame.samples, frame.count, ctx)" in playback_c
    assert "playback_v2_speaker_commit_frame(frame.count, wr)" in playback_c
    assert "static void playback_v2_finish_real_owner_window_locked(void)" in playback_c
    assert "playback_v2_finish_real_owner_window_locked();" in playback_c
    assert "s_status.speaker_owner_real_auto_disarm_count++" in playback_c
    assert "if (s_status.speaker_owner_real_armed)" in playback_c
    assert "s_status.speaker_owner_real_write_frames++" in playback_c
    assert "s_status.speaker_owner_real_write_failures++" in playback_c
    assert "static void playback_v2_speaker_commit_frame(" in playback_c
    assert "audio_io_service_v2_speaker_handoff_note_playback_frame(false, result)" in playback_c
    assert "static bool playback_v2_speaker_note_empty(" in playback_c
    drop_listening = playback_c[
        playback_c.index("void audio_playback_service_v2_say_drop_listening("):
        playback_c.index("void audio_playback_service_v2_say_end_idle(")
    ]
    assert "xQueueReset(queue)" in drop_listening
    assert "s_status.say_chunks_cancelled += pending;" in drop_listening
    assert "audio_io_service_v2_set_speaker_handoff_owner_requested(" in playback_c
    assert "speaker_owner_dry_run_enabled = io->speaker_handoff_dry_run_enabled;" in playback_c
    assert "speaker_owner_candidate = io->speaker_handoff_candidate;" in playback_c
    assert "speaker_owner_handoff_ready = io->speaker_handoff_ready;" in playback_c
    assert "speaker_owner_block_reason = (uint32_t)io->speaker_handoff_block_reason;" in playback_c
    assert "speaker_owner_failures = io->speaker_handoff_failures;" in playback_c
    assert "speaker_owner_recoveries = io->speaker_handoff_recoveries;" in playback_c
    assert "audio_hal_" not in playback_c


def test_voice_capture_v2_replay_is_explicit_and_bridge_tx_is_owner_gated():
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
    assert "esp_err_t voice_capture_session_v2_set_bridge_tx_owner(bool enabled);" in capture_h
    assert "esp_err_t voice_capture_session_v2_send_voice_start(void);" in capture_h
    assert "esp_err_t voice_capture_session_v2_send_audio_chunk(" in capture_h
    assert "esp_err_t voice_capture_session_v2_send_opus_packet(" in capture_h
    assert "esp_err_t voice_capture_session_v2_send_voice_end(" in capture_h
    assert "bool real_capture;" in capture_h
    assert "bool bridge_tx_owner;" in capture_h
    assert "bool legacy_audio_service_tx_owner;" in capture_h
    assert "bool bridge_tx_candidate;" in capture_h
    assert "bool bridge_tx_handoff_ready;" in capture_h
    assert "bool shadow_voice_start_sent;" in capture_h
    assert "uint32_t shadow_audio_chunks;" in capture_h
    assert "uint32_t shadow_audio_samples;" in capture_h
    assert "uint32_t shadow_audio_dropped_chunks;" in capture_h
    assert "nb_voice_capture_v2_end_reason_t end_reason;" in capture_h
    assert "nb_voice_capture_v2_handoff_block_t handoff_block_reason;" in capture_h
    assert "NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_NOT_REAL_CAPTURE" in capture_h
    assert "NB_VOICE_CAPTURE_V2_HANDOFF_BLOCK_DROPPED_AUDIO" in capture_h
    assert '\\"real_capture\\":%s,' in web
    assert '\\"end_reason\\":\\"%s\\"' in web
    assert '\\"bridge_tx_candidate\\":%s' in web
    assert '\\"bridge_tx_handoff_ready\\":%s' in web
    assert '\\"handoff_block_reason\\":\\"%s\\"' in web
    assert '\\"shadow_audio_chunks\\":%lu' in web
    assert "capture_v2_end_reason_name" in web
    assert "capture_v2_handoff_block_name" in web
    assert "update_handoff_gate_locked();" in capture_c
    assert "s_status.bridge_tx_handoff_ready = true;" in capture_c
    assert "NB_VOICE_CAPTURE_V2_END_SPEECH_COMPLETE" in capture_c
    assert "NB_VOICE_CAPTURE_V2_END_NO_SPEECH" in capture_c
    assert "NB_VOICE_CAPTURE_V2_END_CANCELLED" in capture_c
    assert "s_status.speech_elapsed_ms += elapsed_ms;" in capture_c
    assert "s_status.shadow_audio_chunks += frame_units;" in capture_c
    assert "s_status.shadow_audio_samples += sample_count;" in capture_c
    assert "bridge_service_send_audio_chunk(" in capture_c
    assert "bridge_service_send_opus_packet(" in capture_c
    assert "bridge_service_send_event(&marker)" in capture_c
    assert "bridge_service_flush_tx();" in capture_c
    assert "s_status.bridge_tx_owner = enabled;" in capture_c
    assert "s_status.legacy_audio_service_tx_owner = !enabled;" in capture_c


def test_voice_capture_v2_real_path_is_opt_in_config_flag():
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    capture_c = (
        COMPONENTS / "voice_capture_session_v2" / "voice_capture_session_v2.c"
    ).read_text(encoding="utf-8")
    config_h = CONFIG_H.read_text(encoding="utf-8")
    config_c = CONFIG_C.read_text(encoding="utf-8")
    config_keys = CONFIG_KEYS.read_text(encoding="utf-8")

    assert '#define NB_CFG_KEY_V2_CAP_EN  "v2cap_en"' in config_keys
    assert '#define NB_CFG_KEY_V2_CAP_TX  "v2cap_tx_en"' in config_keys
    assert '#define NB_CFG_KEY_V2_ACT_DEC "v2act_dec"' in config_keys
    assert '#define NB_CFG_KEY_V2_ACT_MIG "v2act_mig"' in config_keys
    assert "#define NB_CFG_DEFAULT_V2_CAP_EN          0" in config_keys
    assert "#define NB_CFG_DEFAULT_V2_CAP_TX          0" in config_keys
    assert "#define NB_CFG_DEFAULT_V2_ACT_DEC         1" in config_keys
    assert "#define NB_CFG_DEFAULT_V2_ACT_MIG         0" in config_keys
    assert "NB_CFG_SCHEMA_VERSION  3U" in config_keys
    assert "bool      config_get_voice_audio_v2_capture_enabled(void);" in config_h
    assert "esp_err_t config_set_voice_audio_v2_capture_enabled(bool enabled);" in config_h
    assert "bool      config_get_voice_audio_v2_capture_tx_enabled(void);" in config_h
    assert "esp_err_t config_set_voice_audio_v2_capture_tx_enabled(bool enabled);" in config_h
    assert "bool      config_get_voice_audio_v2_activity_decider_enabled(void);" in config_h
    assert "esp_err_t config_set_voice_audio_v2_activity_decider_enabled(bool enabled);" in config_h
    assert "ensure_voice_audio_v2_defaults" in config_c
    assert "voice_audio_v2_capture_enabled" in web
    assert "voice_audio_v2_capture_tx_enabled" in web
    assert "voice_audio_v2_activity_decider_enabled" in web
    assert 'cfg.max_uri_handlers  = (uint16_t)((sizeof(k_uris) / sizeof(k_uris[0])) + 4U);' in web
    assert "err = config_set_voice_audio_v2_capture_enabled(true);" in web
    assert "(void)voice_capture_session_v2_set_bridge_tx_owner(false);" in web
    assert '\\"real_capture_enabled\\":%s,' in web
    assert '\\"bridge_tx_handoff_enabled\\":%s,' in web
    assert "voice_capture_session_v2_is_active()" in web
    assert "audio_service_end_listen_session(NB_LISTEN_END_CANCELLED)" in web
    assert "config_get_voice_audio_v2_capture_enabled()" in audio_service
    assert "config_get_voice_audio_v2_capture_tx_enabled()" in audio_service
    assert "voice_capture_session_v2_begin_real_pcm16(" in audio_service
    assert "voice_capture_session_v2_set_bridge_tx_owner(true)" in audio_service
    assert "s.listen_capture_v2_tx_owner" in audio_service
    assert "voice_capture_session_v2_send_voice_start()" in audio_service
    assert "voice_capture_session_v2_send_audio_chunk(" in audio_service
    assert "voice_capture_session_v2_send_opus_packet(" in capture_c
    assert "voice_capture_session_v2_send_voice_end(" in audio_service
    assert "s_status.real_capture = true;" in capture_c
    assert "if (enabled && (!s_status.session_active || !s_status.real_capture))" in capture_c
    assert "voice_capture_session_v2_note_voice_start();" in audio_service
    assert "voice_capture_session_v2_note_audio_chunk(" in audio_service
    assert "static uint8_t bridge_drain_opus_packets_if_enabled(bool capture_v2_tx_owner)" in audio_service
    assert "sent_packets * NB_AUDIO_CODEC_V2_OPUS_FRAME_SAMPLES" in audio_service
    assert "voice_capture_session_v2_finish(" in audio_service


def test_voice_activity_v2_decider_flag_is_default_controlled_and_observable():
    web = (ROOT / "components" / "infra" / "web_service.c").read_text(encoding="utf-8")
    audio_service = (COMPONENTS / "audio_service" / "audio_service.c").read_text(
        encoding="utf-8"
    )
    activity_c = (
        COMPONENTS / "voice_activity_service_v2" / "voice_activity_service_v2.c"
    ).read_text(encoding="utf-8")
    config_c = CONFIG_C.read_text(encoding="utf-8")
    config_keys = CONFIG_KEYS.read_text(encoding="utf-8")

    assert '#define NB_CFG_KEY_V2_ACT_DEC "v2act_dec"' in config_keys
    assert '#define NB_CFG_KEY_V2_ACT_MIG "v2act_mig"' in config_keys
    assert "#define NB_CFG_DEFAULT_V2_ACT_DEC         1" in config_keys
    assert "NB_CFG_SCHEMA_VERSION  3U" in config_keys
    assert "NB_CFG_KEY_V2_ACT_DEC" in config_c
    assert "NB_CFG_KEY_V2_ACT_MIG" in config_c
    assert "config_get_voice_audio_v2_activity_decider_enabled" in config_c
    assert "config_set_voice_audio_v2_activity_decider_enabled" in config_c
    assert "voice_audio_v2_activity_decider_enabled" in web
    assert '\\"activity_decider_enabled\\":%s,' in web
    assert '\\"activity_decider_owner_active\\":%s,' in web
    assert '\\"activity_decider_end_used\\":%s,' in web
    assert '\\"activity_decider_end_count\\":%lu,' in web
    assert '\\"activity_end_silence_ms\\":%lu,' in web
    assert "config_get_voice_audio_v2_activity_decider_enabled()" in audio_service
    assert "activity_v2_decider_try_finish()" in audio_service
    assert "voice_activity_service_v2_session_end_observed(&activity_end_elapsed_ms)" in audio_service
    assert "voice_activity_service_v2_note_decider_end(activity_end_elapsed_ms)" in audio_service
    assert "config_get_voice_audio_v2_activity_decider_enabled()" not in activity_c
