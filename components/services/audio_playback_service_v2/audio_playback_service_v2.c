/*
 * audio_playback_service_v2.c - Playback v2 staged downlink owner.
 */

#include "audio_playback_service_v2.h"
#include "audio_hal.h"
#include "audio_io_service_v2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"
#include <string.h>

#define NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS   32U
#define NB_AUDIO_PLAYBACK_V2_SAMPLE_RATE_HZ  16000U
#define NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES   256U
#define NB_AUDIO_PLAYBACK_V2_CHUNK_MS        16U
#define NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS NB_AUDIO_PLAYBACK_V2_CHUNK_MS
#define NB_AUDIO_PLAYBACK_V2_SAY_IDLE_END_MS 1200U
#define NB_AUDIO_PLAYBACK_V2_PROBE_HZ        440U

#define PLAYBACK_PROBE_MIN_DURATION_MS  16U
#define PLAYBACK_PROBE_MAX_DURATION_MS  2000U
#define PLAYBACK_PROBE_DEFAULT_AMP      1200U
#define PLAYBACK_PROBE_MAX_AMP          6000U
#define PLAYBACK_SPEAKER_MAX_VOLUME     100U

typedef struct {
    int16_t samples[NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES];
    uint16_t count;
} nb_audio_playback_v2_say_chunk_t;

static nb_audio_playback_v2_status_t s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_phase;
static QueueHandle_t s_say_q;
static uint8_t s_say_q_storage[NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS *
                               sizeof(nb_audio_playback_v2_say_chunk_t)];
static StaticQueue_t s_say_q_static;

static void playback_v2_clear_real_owner_locked(void)
{
    s_status.speaker_owner_real_requested = false;
    s_status.speaker_owner_real_armed = false;
    s_status.speaker_owner_real_block_reason =
        (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED;
    s_status.speaker_owner_real_window_active = false;
    s_status.speaker_owner_real_window_completed = false;
    s_status.speaker_owner_real_auto_disarm_count = 0;
    s_status.speaker_owner_real_write_frames = 0;
    s_status.speaker_owner_real_write_samples = 0;
    s_status.speaker_owner_real_write_failures = 0;
    s_status.speaker_owner_real_last_result = ESP_OK;
}

static void playback_v2_begin_real_owner_window_locked(void)
{
    s_status.speaker_owner_real_window_active = true;
    s_status.speaker_owner_real_window_completed = false;
    s_status.speaker_owner_real_write_frames = 0;
    s_status.speaker_owner_real_write_samples = 0;
    s_status.speaker_owner_real_write_failures = 0;
    s_status.speaker_owner_real_last_result = ESP_OK;
}

static void playback_v2_finish_real_owner_window_locked(void)
{
    if (!s_status.speaker_owner_real_armed ||
        !s_status.speaker_owner_real_window_active) {
        return;
    }

    s_status.speaker_owner_real_requested = false;
    s_status.speaker_owner_real_armed = false;
    s_status.speaker_owner_real_block_reason =
        (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED;
    s_status.speaker_owner_real_window_active = false;
    s_status.speaker_owner_real_window_completed = true;
    s_status.speaker_owner_real_auto_disarm_count++;
}

static void playback_v2_note_queue_count(uint32_t queue_count)
{
    s_status.bridge_say_observer = true;
    s_status.bridge_say_queue_owner = (s_say_q != NULL);
    s_status.say_queue_depth = NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS;
    s_status.say_queue_count = queue_count;
    if (queue_count > s_status.say_queue_high_watermark) {
        s_status.say_queue_high_watermark = queue_count;
    }
    s_status.say_accept_wait_ms = NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS;
}

static void playback_v2_reset_speaker_empty_locked(void)
{
    s_status.speaker_empty_ms = 0;
}

static void playback_v2_finish_say_locked(void)
{
    if (s_status.bridge_say_active) {
        s_status.bridge_say_active = false;
        s_status.say_end_count++;
    }
}

static void playback_v2_mirror_speaker_owner(nb_audio_playback_v2_status_t *status,
                                             const nb_audio_io_v2_status_t *io)
{
    if (status == NULL || io == NULL) {
        return;
    }

    status->speaker_owner_dry_run_enabled = io->speaker_handoff_dry_run_enabled;
    status->speaker_owner_requested = io->speaker_handoff_owner_requested;
    status->speaker_owner_ready = io->speaker_handoff_owner_ready;
    status->speaker_owner_active = io->speaker_handoff_active;
    status->speaker_owner_candidate = io->speaker_handoff_candidate;
    status->speaker_owner_handoff_ready = io->speaker_handoff_ready;
    status->speaker_owner_block_reason = (uint32_t)io->speaker_handoff_block_reason;
    if (!status->speaker_owner_real_requested && !status->speaker_owner_real_armed) {
        status->speaker_owner_real_block_reason =
            (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED;
    }
    status->speaker_owner_frames = io->speaker_handoff_frames;
    status->speaker_owner_samples = io->speaker_handoff_samples;
    status->speaker_owner_silence_frames = io->speaker_handoff_silence_frames;
    status->speaker_owner_failures = io->speaker_handoff_failures;
    status->speaker_owner_recoveries = io->speaker_handoff_recoveries;
    status->speaker_owner_last_samples = io->speaker_handoff_last_samples;
    status->speaker_owner_last_result = io->speaker_handoff_last_result;
}

esp_err_t audio_playback_service_v2_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_say_q = xQueueCreateStatic(NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS,
                                 sizeof(nb_audio_playback_v2_say_chunk_t),
                                 s_say_q_storage,
                                 &s_say_q_static);
    if (s_say_q == NULL) {
        s_status.last_error = ESP_ERR_NO_MEM;
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    s_status.initialized = true;
    s_status.bridge_say_observer = true;
    s_status.bridge_say_queue_owner = true;
    s_status.say_queue_depth = NB_AUDIO_PLAYBACK_V2_QUEUE_PACKETS;
    s_status.say_accept_wait_ms = NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS;
    s_status.last_error = ESP_OK;
    playback_v2_clear_real_owner_locked();
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_playback_service_v2_deinit(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_say_q = NULL;
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_playback_service_v2_is_initialized(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_mux);
    return initialized;
}

void audio_playback_service_v2_get_status(nb_audio_playback_v2_status_t *out)
{
    if (out == NULL) {
        return;
    }

    nb_audio_io_v2_status_t io;
    audio_io_service_v2_get_status(&io);

    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
    playback_v2_mirror_speaker_owner(out, &io);
}

esp_err_t audio_playback_service_v2_probe_start(uint32_t duration_ms, uint16_t amplitude)
{
    if (duration_ms < PLAYBACK_PROBE_MIN_DURATION_MS ||
        duration_ms > PLAYBACK_PROBE_MAX_DURATION_MS ||
        amplitude > PLAYBACK_PROBE_MAX_AMP) {
        return ESP_ERR_INVALID_ARG;
    }

    if (amplitude == 0U) {
        amplitude = PLAYBACK_PROBE_DEFAULT_AMP;
    }

    uint32_t chunks = (duration_ms + 15U) / 16U;
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.initialized) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.initialized = true;
        s_status.bridge_say_observer = true;
    }
    if (s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.playing = true;
    s_status.stop_requested = false;
    s_status.probe_duration_ms = duration_ms;
    s_status.probe_elapsed_ms = 0;
    s_status.queued_chunks = chunks;
    s_status.played_chunks = 0;
    s_status.dropped_chunks = 0;
    s_status.amplitude = amplitude;
    s_status.last_error = ESP_OK;
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_playback_service_v2_probe_stop(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.playing = false;
    s_status.stop_requested = true;
    s_status.queued_chunks = 0;
    s_status.cancel_count++;
    s_phase = 0;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool audio_playback_service_v2_is_playing(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool playing = s_status.playing;
    taskEXIT_CRITICAL(&s_mux);
    return playing;
}

bool audio_playback_service_v2_say_is_active(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool active = s_status.bridge_say_active;
    taskEXIT_CRITICAL(&s_mux);
    return active;
}

bool audio_playback_service_v2_fill_probe_chunk(int16_t *out, uint16_t sample_count)
{
    if (out == NULL || sample_count == 0U) {
        return false;
    }

    taskENTER_CRITICAL(&s_mux);
    bool playing = s_status.playing;
    uint32_t amplitude = s_status.amplitude;
    uint32_t phase = s_phase;
    taskEXIT_CRITICAL(&s_mux);
    if (!playing) {
        return false;
    }

    uint32_t half_period = NB_AUDIO_PLAYBACK_V2_SAMPLE_RATE_HZ /
                           (NB_AUDIO_PLAYBACK_V2_PROBE_HZ * 2U);
    if (half_period == 0U) {
        half_period = 1U;
    }

    for (uint16_t i = 0; i < sample_count; i++) {
        out[i] = ((phase / half_period) & 1U) == 0U
               ? (int16_t)amplitude
               : (int16_t)(-(int32_t)amplitude);
        phase++;
    }

    taskENTER_CRITICAL(&s_mux);
    if (!s_status.playing) {
        taskEXIT_CRITICAL(&s_mux);
        memset(out, 0, sample_count * sizeof(int16_t));
        return false;
    }

    s_phase = phase;
    s_status.played_chunks++;
    if (s_status.queued_chunks > 0U) {
        s_status.queued_chunks--;
    }
    s_status.probe_elapsed_ms += 16U;
    if (s_status.queued_chunks == 0U ||
        s_status.probe_elapsed_ms >= s_status.probe_duration_ms) {
        s_status.playing = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return true;
}

bool audio_playback_service_v2_probe_write_next_frame(uint16_t *sample_count,
                                                      esp_err_t *result)
{
    int16_t frame[NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES];

    if (sample_count != NULL) {
        *sample_count = 0U;
    }
    if (result != NULL) {
        *result = ESP_OK;
    }

    if (!audio_playback_service_v2_fill_probe_chunk(
            frame,
            NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES)) {
        return false;
    }

    esp_err_t wr = audio_hal_spk_write(
        frame,
        NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES,
        pdMS_TO_TICKS(100));
    if (sample_count != NULL) {
        *sample_count = NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES;
    }
    if (result != NULL) {
        *result = wr;
    }
    return true;
}

esp_err_t audio_playback_service_v2_speaker_owner_arm(void)
{
    esp_err_t err = audio_io_service_v2_set_speaker_handoff_owner_requested(true);

    taskENTER_CRITICAL(&s_mux);
    if (err == ESP_OK) {
        s_status.speaker_owner_requested = true;
    }
    s_status.last_error = err;
    taskEXIT_CRITICAL(&s_mux);
    return err;
}

esp_err_t audio_playback_service_v2_speaker_owner_disarm(void)
{
    esp_err_t err = audio_io_service_v2_set_speaker_handoff_owner_requested(false);

    taskENTER_CRITICAL(&s_mux);
    if (err == ESP_OK) {
        s_status.speaker_owner_requested = false;
        s_status.speaker_owner_ready = false;
        s_status.speaker_owner_active = false;
        playback_v2_clear_real_owner_locked();
    }
    s_status.last_error = err;
    taskEXIT_CRITICAL(&s_mux);
    return err;
}

esp_err_t audio_playback_service_v2_speaker_owner_real_arm(void)
{
    nb_audio_io_v2_status_t io;
    audio_io_service_v2_get_status(&io);

    uint32_t block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_NONE;
    esp_err_t err = ESP_OK;
    uint32_t non_silence_frames = 0U;
    if (io.speaker_handoff_frames > io.speaker_handoff_silence_frames) {
        non_silence_frames = io.speaker_handoff_frames -
                             io.speaker_handoff_silence_frames;
    }

    if (!io.speaker_handoff_dry_run_enabled ||
        !io.speaker_handoff_owner_requested) {
        block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_DISABLED;
        err = ESP_ERR_INVALID_STATE;
    } else if (io.speaker_handoff_failures > 0U) {
        block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_TX_ERROR;
        err = ESP_ERR_INVALID_STATE;
    } else if (s_status.say_chunks_dropped > 0U ||
               s_status.say_chunks_dropped_listening > 0U ||
               s_status.speaker_write_failures > 0U ||
               s_status.speaker_commit_failures > 0U) {
        block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_TX_ERROR;
        err = ESP_ERR_INVALID_STATE;
    } else if (io.speaker_handoff_recoveries > 0U) {
        block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_I2S_RECOVERY;
        err = ESP_ERR_INVALID_STATE;
    } else if (!io.speaker_handoff_ready ||
               !io.speaker_handoff_active ||
               non_silence_frames == 0U) {
        block = (uint32_t)NB_AUDIO_IO_V2_SPEAKER_HANDOFF_BLOCK_NO_TX;
        err = ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_mux);
    s_status.speaker_owner_real_requested = true;
    s_status.speaker_owner_real_armed = (err == ESP_OK);
    s_status.speaker_owner_real_block_reason = block;
    if (err == ESP_OK) {
        playback_v2_begin_real_owner_window_locked();
    }
    s_status.last_error = err;
    taskEXIT_CRITICAL(&s_mux);
    return err;
}

esp_err_t audio_playback_service_v2_speaker_owner_real_disarm(void)
{
    taskENTER_CRITICAL(&s_mux);
    playback_v2_clear_real_owner_locked();
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t audio_playback_service_v2_say_accept(const int16_t *samples, uint16_t count)
{
    if (samples == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES) {
        count = NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES;
    }

    QueueHandle_t queue = s_say_q;
    if (queue == NULL) {
        taskENTER_CRITICAL(&s_mux);
        s_status.last_error = ESP_ERR_INVALID_STATE;
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    nb_audio_playback_v2_say_chunk_t item;
    item.count = count;
    memcpy(item.samples, samples, count * sizeof(int16_t));

    bool waited_for_room = false;
    BaseType_t sent = xQueueSend(queue, &item, 0);
    if (sent != pdTRUE) {
        waited_for_room = true;
        sent = xQueueSend(
            queue,
            &item,
            pdMS_TO_TICKS(NB_AUDIO_PLAYBACK_V2_SAY_ACCEPT_WAIT_MS));
    }

    if (sent == pdTRUE) {
        uint32_t queue_count = (uint32_t)uxQueueMessagesWaiting(queue);
        taskENTER_CRITICAL(&s_mux);
        s_status.say_chunks_received++;
        if (waited_for_room) {
            s_status.say_chunks_queue_full++;
            s_status.say_chunks_queue_wait_recovered++;
        }
        playback_v2_reset_speaker_empty_locked();
        playback_v2_note_queue_count(queue_count);
        s_status.last_error = ESP_OK;
        taskEXIT_CRITICAL(&s_mux);
        return ESP_OK;
    }

    uint32_t queue_count = (uint32_t)uxQueueMessagesWaiting(queue);
    taskENTER_CRITICAL(&s_mux);
    s_status.say_chunks_dropped++;
    s_status.say_chunks_dropped_queue_full++;
    s_status.say_chunks_queue_full++;
    playback_v2_note_queue_count(queue_count);
    s_status.last_error = ESP_ERR_NO_MEM;
    taskEXIT_CRITICAL(&s_mux);
    return ESP_ERR_NO_MEM;
}

void audio_playback_service_v2_say_begin(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.bridge_say_observer = true;
    s_status.bridge_say_queue_owner = (s_say_q != NULL);
    if (!s_status.bridge_say_active) {
        s_status.bridge_say_active = true;
        s_status.say_begin_count++;
    }
    playback_v2_reset_speaker_empty_locked();
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
}

static bool playback_v2_say_dequeue(nb_audio_playback_v2_say_chunk_t *out)
{
    if (out == NULL || s_say_q == NULL) {
        return false;
    }

    if (xQueueReceive(s_say_q, out, 0) != pdTRUE) {
        return false;
    }

    uint32_t queue_count = (uint32_t)uxQueueMessagesWaiting(s_say_q);
    taskENTER_CRITICAL(&s_mux);
    s_status.say_chunks_played++;
    playback_v2_note_queue_count(queue_count);
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return true;
}

static bool playback_v2_speaker_next_frame(nb_audio_playback_v2_say_chunk_t *out,
                                           uint8_t volume_percent)
{
    if (out == NULL) {
        taskENTER_CRITICAL(&s_mux);
        s_status.last_error = ESP_ERR_INVALID_ARG;
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }

    if (!playback_v2_say_dequeue(out)) {
        return false;
    }

    if (out->count > NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES) {
        out->count = NB_AUDIO_PLAYBACK_V2_CHUNK_SAMPLES;
    }
    if (volume_percent > PLAYBACK_SPEAKER_MAX_VOLUME) {
        volume_percent = PLAYBACK_SPEAKER_MAX_VOLUME;
    }

    uint32_t mult = ((uint32_t)volume_percent * 256U) / 100U;
    for (uint16_t i = 0; i < out->count; i++) {
        int32_t v = ((int32_t)out->samples[i] * (int32_t)mult) >> 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        out->samples[i] = (int16_t)v;
    }

    taskENTER_CRITICAL(&s_mux);
    s_status.speaker_frames_prepared++;
    s_status.speaker_samples_prepared += out->count;
    s_status.speaker_last_samples = out->count;
    s_status.speaker_last_volume = volume_percent;
    playback_v2_reset_speaker_empty_locked();
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return true;
}

static void playback_v2_speaker_commit_frame(uint16_t sample_count,
                                             esp_err_t result);

bool audio_playback_service_v2_speaker_write_next_frame(
    uint8_t volume_percent,
    uint16_t *sample_count,
    esp_err_t *result)
{
    nb_audio_playback_v2_say_chunk_t frame;

    if (sample_count != NULL) {
        *sample_count = 0U;
    }
    if (result != NULL) {
        *result = ESP_OK;
    }

    if (!playback_v2_speaker_next_frame(&frame, volume_percent)) {
        return false;
    }

    esp_err_t wr = audio_hal_spk_write(frame.samples, frame.count, pdMS_TO_TICKS(100));
    playback_v2_speaker_commit_frame(frame.count, wr);

    taskENTER_CRITICAL(&s_mux);
    if (s_status.speaker_owner_real_armed) {
        s_status.speaker_owner_real_window_active = true;
        s_status.speaker_owner_real_write_frames++;
        s_status.speaker_owner_real_write_samples += frame.count;
        s_status.speaker_owner_real_last_result = wr;
        if (wr != ESP_OK) {
            s_status.speaker_owner_real_write_failures++;
        }
    }
    s_status.speaker_write_requests++;
    s_status.speaker_write_samples += frame.count;
    s_status.speaker_last_write_samples = frame.count;
    s_status.speaker_last_write_result = wr;
    if (wr != ESP_OK) {
        s_status.speaker_write_failures++;
    }
    s_status.last_error = wr;
    taskEXIT_CRITICAL(&s_mux);

    if (sample_count != NULL) {
        *sample_count = frame.count;
    }
    if (result != NULL) {
        *result = wr;
    }
    return true;
}

static void playback_v2_speaker_commit_frame(uint16_t sample_count,
                                             esp_err_t result)
{
    audio_io_service_v2_speaker_handoff_note_playback_frame(false, result);

    taskENTER_CRITICAL(&s_mux);
    s_status.speaker_frames_committed++;
    s_status.speaker_samples_committed += sample_count;
    s_status.speaker_last_commit_samples = sample_count;
    s_status.speaker_last_commit_result = result;
    if (result != ESP_OK) {
        s_status.speaker_commit_failures++;
    }
    s_status.last_error = result;
    taskEXIT_CRITICAL(&s_mux);
}

static bool playback_v2_speaker_note_empty(uint32_t chunk_ms,
                                           uint32_t idle_end_ms)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.speaker_empty_polls++;
    bool should_end = idle_end_ms > 0U &&
                      s_status.speaker_empty_ms >= idle_end_ms;
    if (should_end) {
        s_status.speaker_idle_end_count++;
    } else {
        s_status.speaker_empty_ms += chunk_ms;
    }
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return should_end;
}

bool audio_playback_service_v2_speaker_should_end_idle(void)
{
    return playback_v2_speaker_note_empty(
        NB_AUDIO_PLAYBACK_V2_CHUNK_MS,
        NB_AUDIO_PLAYBACK_V2_SAY_IDLE_END_MS);
}

uint32_t audio_playback_service_v2_say_cancel_active(void)
{
    QueueHandle_t queue = s_say_q;
    if (queue == NULL) {
        return 0U;
    }

    uint32_t pending = (uint32_t)uxQueueMessagesWaiting(queue);
    xQueueReset(queue);

    taskENTER_CRITICAL(&s_mux);
    s_status.say_chunks_cancelled += pending;
    if (pending > 0U) {
        s_status.say_cancel_count++;
    }
    playback_v2_finish_say_locked();
    playback_v2_reset_speaker_empty_locked();
    playback_v2_note_queue_count(0U);
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
    return pending;
}

void audio_playback_service_v2_say_drop_listening(void)
{
    QueueHandle_t queue = s_say_q;
    uint32_t pending = 0U;
    if (queue != NULL) {
        pending = (uint32_t)uxQueueMessagesWaiting(queue);
        xQueueReset(queue);
    }

    taskENTER_CRITICAL(&s_mux);
    s_status.bridge_say_observer = true;
    s_status.bridge_say_queue_owner = (s_say_q != NULL);
    s_status.say_chunks_dropped++;
    s_status.say_chunks_dropped_listening++;
    s_status.say_chunks_cancelled += pending;
    if (pending > 0U) {
        s_status.say_cancel_count++;
    }
    playback_v2_finish_say_locked();
    s_status.say_queue_count = 0;
    playback_v2_reset_speaker_empty_locked();
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
}

void audio_playback_service_v2_say_end_idle(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.bridge_say_observer = true;
    s_status.bridge_say_queue_owner = (s_say_q != NULL);
    playback_v2_finish_real_owner_window_locked();
    playback_v2_finish_say_locked();
    playback_v2_reset_speaker_empty_locked();
    s_status.say_queue_count = 0;
    s_status.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_mux);
}
