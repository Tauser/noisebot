/*
 * voice_controller.c - Politica de turn-taking de voz do NoiseBot (Layer 6)
 */

#include "voice_controller.h"

#include "audio_service.h"
#include "config_manager.h"
#include "led_service.h"
#include "synth_service.h"
#include "ui_overlay_service.h"
#include "wake_service.h"

#include "esp_log.h"

#define TAG "nb_voice_ctl"

#define FIRST_CONTACT_MIN_RAW_RMS    80U
#define FIRST_CONTACT_MIN_RAW_PEAK  180U
#define FIRST_CONTACT_MIN_POST_PEAK 3000U

typedef enum {
    VOICE_PENDING_NONE = 0,
    VOICE_PENDING_WAKE_WORD,
    VOICE_PENDING_FOLLOWUP,
    VOICE_PENDING_BARGE_IN,
} voice_pending_listen_t;

static bool s_initialized;
static voice_pending_listen_t s_pending_listen = VOICE_PENDING_NONE;

static const char *pending_listen_name(voice_pending_listen_t pending)
{
    switch (pending) {
        case VOICE_PENDING_WAKE_WORD: return "wake_word";
        case VOICE_PENDING_FOLLOWUP:  return "followup";
        case VOICE_PENDING_BARGE_IN:  return "barge_in";
        default:                      return "none";
    }
}

static bool first_contact_wake_is_too_weak(void)
{
    nb_wake_detection_stats_t stats;
    if (!wake_service_get_last_detection_stats(&stats)) {
        return false;
    }

    bool weak = stats.raw_rms < FIRST_CONTACT_MIN_RAW_RMS
             || stats.raw_peak < FIRST_CONTACT_MIN_RAW_PEAK;
    if (weak) {
        ESP_LOGW(TAG,
                 "wake word rejeitada em IDLE — energia baixa raw_rms=%lu raw_peak=%u gain=%u..%u post_peak=%u",
                 (unsigned long)stats.raw_rms,
                 (unsigned)stats.raw_peak,
                 (unsigned)stats.gain_min,
                 (unsigned)stats.gain_max,
                 (unsigned)stats.post_peak);
    }
    return weak;
}

esp_err_t voice_controller_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_listen = VOICE_PENDING_NONE;
    s_initialized = true;
    return ESP_OK;
}

bool voice_controller_on_wake_word_detected(void)
{
    if (config_get_silence_mode_enabled()) {
        ESP_LOGI(TAG, "wake word ignorada: modo silencio ativo");
        wake_service_suspend();
        return false;
    }

    nb_robot_state_t st = state_machine_get_state();
    if (st != NB_STATE_IDLE &&
        st != NB_STATE_SLEEPING &&
        st != NB_STATE_TOUCH_REACTING &&
        st != NB_STATE_RESPONDING) {
        ESP_LOGI(TAG, "wake word ignorada em estado %s",
                 state_machine_state_name(st));
        return false;
    }

    if (st == NB_STATE_IDLE && first_contact_wake_is_too_weak()) {
        wake_service_rearm();
        return false;
    }

    if (st == NB_STATE_RESPONDING) {
        ESP_LOGI(TAG, "wake word durante RESPONDING: interrompendo fala");
        audio_play_stop();
        s_pending_listen = VOICE_PENDING_BARGE_IN;
    } else {
        s_pending_listen = VOICE_PENDING_WAKE_WORD;
    }

    state_machine_on_wake_word();
    if (state_machine_get_state() != NB_STATE_ATTENTIVE) {
        s_pending_listen = VOICE_PENDING_NONE;
    }
    return true;
}

void voice_controller_request_followup_listen(void)
{
    if (config_get_silence_mode_enabled()) {
        ESP_LOGI(TAG, "follow-up de voz ignorado: modo silencio ativo");
        wake_service_suspend();
        return;
    }

    s_pending_listen = VOICE_PENDING_FOLLOWUP;
    state_machine_on_followup_listen();
    if (state_machine_get_state() != NB_STATE_ATTENTIVE) {
        s_pending_listen = VOICE_PENDING_NONE;
    }
}

static void begin_pending_listen(void)
{
    nb_listen_source_t source;

    if (config_get_silence_mode_enabled()) {
        ESP_LOGI(TAG, "escuta pendente cancelada: modo silencio ativo");
        s_pending_listen = VOICE_PENDING_NONE;
        ui_overlay_listening_set(false);
        wake_service_suspend();
        return;
    }

    switch (s_pending_listen) {
        case VOICE_PENDING_WAKE_WORD:
            source = NB_LISTEN_SOURCE_WAKE_WORD;
            break;
        case VOICE_PENDING_FOLLOWUP:
            source = NB_LISTEN_SOURCE_FOLLOWUP;
            break;
        case VOICE_PENDING_BARGE_IN:
            source = NB_LISTEN_SOURCE_BARGE_IN;
            break;
        default:
            return;
    }

    ESP_LOGI(TAG, "abrindo escuta source=%s",
             pending_listen_name(s_pending_listen));
    if (audio_service_begin_listen_session(source) == ESP_OK) {
        ui_overlay_listening_set(true);
    }
    s_pending_listen = VOICE_PENDING_NONE;
}

void voice_controller_on_state_changed(nb_robot_state_t new_state,
                                       nb_robot_state_t old_state)
{
    if (!s_initialized) {
        return;
    }

    if (old_state == NB_STATE_ATTENTIVE && new_state != NB_STATE_ATTENTIVE) {
        led_base_set(NB_LED_BASE_ATTENTIVE, false);
        if (audio_service_is_listening()) {
            ui_overlay_listening_set(false);
            audio_service_end_listen_session(NB_LISTEN_END_CANCELLED);
        }
    }

    if (old_state == NB_STATE_RESPONDING && new_state != NB_STATE_RESPONDING) {
        led_base_set(NB_LED_BASE_RESPONDING, false);
    }

    switch (new_state) {
        case NB_STATE_ATTENTIVE:
            synth_stop();
            begin_pending_listen();
            led_base_set(NB_LED_BASE_ATTENTIVE, true);
            break;
        case NB_STATE_RESPONDING:
            synth_stop();
            led_base_set(NB_LED_BASE_RESPONDING, true);
            wake_service_suspend();
            break;
        case NB_STATE_SLEEPING:
        case NB_STATE_MEDITATION:
        case NB_STATE_SILENT_COMPANY:
        case NB_STATE_MAINTENANCE:
            wake_service_suspend();
            s_pending_listen = VOICE_PENDING_NONE;
            break;
        case NB_STATE_IDLE:
            s_pending_listen = VOICE_PENDING_NONE;
            if (config_get_silence_mode_enabled()) {
                wake_service_suspend();
            } else {
                wake_service_rearm();
            }
            break;
        default:
            break;
    }
}
