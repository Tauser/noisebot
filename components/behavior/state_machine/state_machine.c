/*
 * state_machine.c — State machine do NoiseBot (Layer 6)
 *
 * Toda transição é loggada com timestamp, estado anterior, estado novo e motivo.
 * Thread-safe via portMUX_TYPE (seções críticas curtas — apenas leitura/escrita
 * de s_state e contadores de timer).
 */

#include "state_machine.h"
#include "audio_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "nb_sm"

/* ── Constantes ──────────────────────────────────────────────────────────── */

/** Duração do estado TOUCH_REACTING antes de retornar a IDLE. */
#define TOUCH_REACT_DURATION_MS  2000U

/** Timer de inatividade em ATTENTIVE: sem sessão de escuta ativa → IDLE. */
#define ATTENTIVE_IDLE_TIMEOUT_MS  8000U

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_robot_state_t      s_state                  = NB_STATE_BOOT_UP;
static uint32_t              s_idle_elapsed_ms         = 0;
static uint32_t              s_idle_timeout_ms         = 120000U; /* default 2 min */
static uint32_t              s_touch_elapsed_ms        = 0;
static uint32_t              s_attentive_elapsed_ms    = 0;
static nb_state_change_cb_t  s_change_cb               = NULL;
static portMUX_TYPE          s_mux                     = portMUX_INITIALIZER_UNLOCKED;
static bool                  s_initialized             = false;

/* ── Helpers internos ────────────────────────────────────────────────────── */

const char *state_machine_state_name(nb_robot_state_t s)
{
    switch (s) {
        case NB_STATE_BOOT_UP:        return "BOOT_UP";
        case NB_STATE_IDLE:           return "IDLE";
        case NB_STATE_ATTENTIVE:      return "ATTENTIVE";
        case NB_STATE_RESPONDING:     return "RESPONDING";
        case NB_STATE_TOUCH_REACTING: return "TOUCH_REACTING";
        case NB_STATE_SLEEPING:       return "SLEEPING";
        case NB_STATE_ERROR:          return "ERROR";
        case NB_STATE_SAFE_MODE:      return "SAFE_MODE";
        case NB_STATE_MEDITATION:     return "MEDITATION";
        case NB_STATE_SILENT_COMPANY: return "SILENT_COMPANY";
        default:                      return "UNKNOWN";
    }
}

/*
 * Executa a transição de estado.
 * Deve ser chamado fora de critical section.
 */
static void do_transition(nb_robot_state_t new_state, const char *reason)
{
    nb_robot_state_t old_state;

    taskENTER_CRITICAL(&s_mux);
    old_state = s_state;
    s_state   = new_state;
    taskEXIT_CRITICAL(&s_mux);

    if (old_state == new_state) return;

    uint32_t ts_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    ESP_LOGI(TAG, "[%lu ms] %s → %s (%s)",
             (unsigned long)ts_ms,
             state_machine_state_name(old_state),
             state_machine_state_name(new_state),
             reason);

    if (s_change_cb) {
        s_change_cb(new_state, old_state, reason);
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t state_machine_init(uint32_t              idle_timeout_s,
                              bool                  start_safe_mode,
                              nb_state_change_cb_t  change_cb)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_idle_timeout_ms  = idle_timeout_s * 1000U;
    s_change_cb        = change_cb;
    s_state            = NB_STATE_BOOT_UP;
    s_idle_elapsed_ms  = 0;
    s_touch_elapsed_ms = 0;
    s_initialized      = true;

    ESP_LOGI(TAG, "init: idle_timeout=%lus safe_mode=%d",
             (unsigned long)idle_timeout_s, (int)start_safe_mode);

    if (start_safe_mode) {
        do_transition(NB_STATE_SAFE_MODE, "safe mode no boot");
    }

    return ESP_OK;
}

nb_robot_state_t state_machine_get_state(void)
{
    nb_robot_state_t s;
    taskENTER_CRITICAL(&s_mux);
    s = s_state;
    taskEXIT_CRITICAL(&s_mux);
    return s;
}

void state_machine_set_idle_timeout_s(uint32_t s)
{
    taskENTER_CRITICAL(&s_mux);
    s_idle_timeout_ms = s * 1000U;
    taskEXIT_CRITICAL(&s_mux);
}

void state_machine_update(uint32_t dt_ms)
{
    if (!s_initialized) return;

    nb_robot_state_t cur = state_machine_get_state();

    /* Avança idle timeout apenas em IDLE. */
    if (cur == NB_STATE_IDLE) {
        s_idle_elapsed_ms += dt_ms;
        if (s_idle_elapsed_ms >= s_idle_timeout_ms) {
            s_idle_elapsed_ms = 0;
            do_transition(NB_STATE_SLEEPING, "idle timeout");
        }
    } else {
        s_idle_elapsed_ms = 0;
    }

    /* Avança touch reacting timer. */
    if (cur == NB_STATE_TOUCH_REACTING) {
        s_touch_elapsed_ms += dt_ms;
        if (s_touch_elapsed_ms >= TOUCH_REACT_DURATION_MS) {
            s_touch_elapsed_ms = 0;
            do_transition(NB_STATE_IDLE, "touch reaction complete");
        }
    } else {
        s_touch_elapsed_ms = 0;
    }

    /* Timer de inatividade em ATTENTIVE: conta enquanto não há sessão de escuta.
     * Fallback para quando a sessão encerrou sem emitir VOICE_END para SM. */
    if (cur == NB_STATE_ATTENTIVE) {
        if (!audio_service_is_listening()) {
            s_attentive_elapsed_ms += dt_ms;
            if (s_attentive_elapsed_ms >= ATTENTIVE_IDLE_TIMEOUT_MS) {
                s_attentive_elapsed_ms = 0;
                do_transition(NB_STATE_IDLE, "attentive timeout");
            }
        } else {
            s_attentive_elapsed_ms = 0;
        }
    } else {
        s_attentive_elapsed_ms = 0;
    }
}

void state_machine_on_boot_complete(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_BOOT_UP) {
        do_transition(NB_STATE_IDLE, "boot completo");
    }
}

void state_machine_on_touch_tap(void)
{
    if (!s_initialized) return;
    nb_robot_state_t cur = state_machine_get_state();
    switch (cur) {
        case NB_STATE_IDLE:
            /* Touch-to-Listen (12.4): tap em IDLE abre sessão de escuta. */
            do_transition(NB_STATE_ATTENTIVE, "tap");
            break;
        case NB_STATE_ATTENTIVE:
        case NB_STATE_RESPONDING:
            /* Tap durante ATTENTIVE/RESPONDING: reação de toque breve. */
            s_touch_elapsed_ms = 0;
            do_transition(NB_STATE_TOUCH_REACTING, "tap");
            break;
        case NB_STATE_SLEEPING:
            do_transition(NB_STATE_IDLE, "tap em SLEEPING");
            break;
        case NB_STATE_MEDITATION:
            do_transition(NB_STATE_IDLE, "tap sai de MEDITATION");
            break;
        case NB_STATE_SILENT_COMPANY:
            do_transition(NB_STATE_IDLE, "tap sai de SILENT_COMPANY");
            break;
        default:
            break;
    }
}

void state_machine_on_touch_long_press(void)
{
    if (!s_initialized) return;
    nb_robot_state_t cur = state_machine_get_state();
    if (cur == NB_STATE_IDLE || cur == NB_STATE_ATTENTIVE ||
        cur == NB_STATE_SLEEPING) {
        s_touch_elapsed_ms = 0;
        do_transition(NB_STATE_TOUCH_REACTING, "long press");
    } else if (cur == NB_STATE_MEDITATION || cur == NB_STATE_SILENT_COMPANY) {
        do_transition(NB_STATE_IDLE, "long press sai de modo especial");
    }
}

void state_machine_on_touch_wake(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_SLEEPING) {
        do_transition(NB_STATE_IDLE, "touch wake");
    }
}

void state_machine_on_voice_start(void)
{
    /* 12.4: VAD não ativa ATTENTIVE. Sessão só abre via touch (ou wake-word no
     * futuro). Esta função mantida para vad_semantic_service e outros consumidores
     * do evento, mas não gera transição de estado. */
    (void)s_initialized;
}

void state_machine_on_voice_end(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_ATTENTIVE) {
        do_transition(NB_STATE_IDLE, "voice end");
    }
}

void state_machine_on_audio_started(void)
{
    if (!s_initialized) return;
    nb_robot_state_t cur = state_machine_get_state();
    if (cur == NB_STATE_IDLE || cur == NB_STATE_ATTENTIVE ||
        cur == NB_STATE_TOUCH_REACTING) {
        do_transition(NB_STATE_RESPONDING, "audio started");
    }
}

void state_machine_on_audio_ended(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_RESPONDING) {
        do_transition(NB_STATE_IDLE, "audio ended");
    }
}

void state_machine_on_motion_fault(void)
{
    if (!s_initialized) return;
    do_transition(NB_STATE_ERROR, "motion fault");
}

void state_machine_on_meditation_enter(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_IDLE) {
        do_transition(NB_STATE_MEDITATION, "toque profundo — meditação");
    }
}

void state_machine_on_meditation_exit(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_MEDITATION) {
        do_transition(NB_STATE_IDLE, "saída de meditação");
    }
}

void state_machine_on_silent_company_enter(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_IDLE) {
        do_transition(NB_STATE_SILENT_COMPANY, "2h sem interação");
    }
}

void state_machine_on_silent_company_exit(void)
{
    if (!s_initialized) return;
    if (state_machine_get_state() == NB_STATE_SILENT_COMPANY) {
        do_transition(NB_STATE_IDLE, "interação retorna companhia");
    }
}
