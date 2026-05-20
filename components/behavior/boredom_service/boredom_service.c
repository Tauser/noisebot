/*
 * boredom_service.c — Escalada criativa de ociosidade (Layer 6)
 *
 * Implementação: ver boredom_service.h para contratos e arquitetura.
 *
 * Modelo de concorrência:
 *   s_mux (portMUX_TYPE spinlock) protege s.idle_ms, s.paused,
 *   s.last_reaction_ms e s.last_demon_ms. O timer callback apenas dá
 *   xSemaphoreGive — toda a lógica pesada roda em nb_boredom_task (prio 3),
 *   sem contender com tasks de safety ou render.
 *
 * Gerador de números aleatórios: esp_random() — adequado para decisão
 * de baixa criticidade como o modo demônio.
 */

#include "boredom_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"
#include "expression_service.h"
#include "led_service.h"
#include "synth_service.h"
#include "ui_overlay_service.h"
#include "state_machine.h"

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "nb_boredom"

/* ── Constantes internas ─────────────────────────────────────────────────── */

/** Stack e prioridade da task interna. */
#define BOREDOM_TASK_STACK   4096U
#define BOREDOM_TASK_PRIO    3U

/** Duração e transição de cada reação emocional (ms). */
#define REACT_CURIOUS_DUR_MS    2500.0f
#define REACT_CURIOUS_TRANS_MS   350.0f
#define REACT_SAD_DUR_MS        3000.0f
#define REACT_SAD_TRANS_MS       400.0f
#define REACT_SUSP_DUR_MS       3200.0f
#define REACT_SUSP_TRANS_MS      300.0f
#define REACT_ANGRY_DUR_MS      3000.0f
#define REACT_ANGRY_TRANS_MS     200.0f
#define REACT_DEMON_DUR_MS      5000.0f
#define REACT_DEMON_TRANS_MS     150.0f

/** Duração dos toasts visuais (ms). */
#define TOAST_DUR_MS            4000U

/** Número máximo de subscriptions internas. */
#define BOREDOM_MAX_SUBS        16U

/* ── Notas do glitch demônio ─────────────────────────────────────────────── */

static const nb_note_t s_demon_melody[] = {
    { 880.0f,  80U  },
    { 220.0f,  60U  },
    { 1320.0f, 50U  },
    { 110.0f,  90U  },
    { 660.0f,  70U  },
    { 165.0f,  80U  },
    { 440.0f,  100U },
};
#define DEMON_MELODY_COUNT ((uint8_t)(sizeof(s_demon_melody) / sizeof(s_demon_melody[0])))

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef struct {
    uint64_t idle_ms;           /**< Tempo ocioso acumulado desde a última interação. */
    uint64_t last_reaction_ms;  /**< idle_ms no momento da última reação (cooldown).  */
    uint64_t last_demon_ms;     /**< Uptime (ms) no momento do último modo demônio.   */
    bool     paused;            /**< Quando true, reações não disparam.               */
    SemaphoreHandle_t check_sem;
    esp_timer_handle_t timer;
    nb_event_sub_handle_t subs[BOREDOM_MAX_SUBS];
    uint8_t   n_subs;
} boredom_state_t;

static boredom_state_t s;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/** Retorna uptime monotônico em ms. */
static inline uint64_t _uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

/* ── Reações por nível ───────────────────────────────────────────────────── */

static void _react_level1_curious(void)
{
    NB_LOGI(TAG, "tédio nível 1 — CURIOUS");
    expression_play(NB_EXPR_CURIOUS, REACT_CURIOUS_DUR_MS, REACT_CURIOUS_TRANS_MS);
    synth_chirp(320.0f, 520.0f, 300U);
}

static void _react_level2_sad(void)
{
    NB_LOGI(TAG, "tédio nível 2 — SAD + toast");
    expression_play(NB_EXPR_SAD, REACT_SAD_DUR_MS, REACT_SAD_TRANS_MS);
    ui_overlay_show_toast("Alguem ai?", NB_UI_OVERLAY_INFO, TOAST_DUR_MS);
    synth_blip(280.0f, 350U);
}

static void _react_level3_suspicious(void)
{
    NB_LOGI(TAG, "tédio nível 3 — SUSPICIOUS + toast");
    expression_play(NB_EXPR_SUSPICIOUS, REACT_SUSP_DUR_MS, REACT_SUSP_TRANS_MS);
    ui_overlay_show_toast("Entendi. Fui abandonado.", NB_UI_OVERLAY_WARNING, TOAST_DUR_MS);
    synth_blip(220.0f, 400U);
}

static void _react_level4_angry(void)
{
    NB_LOGI(TAG, "tédio nível 4 — ANGRY teatral + toast");
    expression_play(NB_EXPR_ANGRY, REACT_ANGRY_DUR_MS, REACT_ANGRY_TRANS_MS);
    led_effect_color_pulse(NB_LED_RED, 0.55f, (uint32_t)REACT_ANGRY_DUR_MS);
    ui_overlay_show_toast("Ok. Fingi que nao ligo.", NB_UI_OVERLAY_WARNING, TOAST_DUR_MS);
    synth_chirp(700.0f, 200.0f, 450U);
}

static void _react_demon(void)
{
    NB_LOGW(TAG, "tédio DEMON MODE ativado!");
    expression_play(NB_EXPR_ANGRY, REACT_DEMON_DUR_MS, REACT_DEMON_TRANS_MS);
    led_effect_color_pulse(NB_LED_RED, 1.0f, (uint32_t)REACT_DEMON_DUR_MS);
    ui_overlay_show_toast(">:)", NB_UI_OVERLAY_ERROR, TOAST_DUR_MS);
    synth_set_timbre(NB_SYNTH_TIMBRE_SQUARE);
    synth_melody(s_demon_melody, DEMON_MELODY_COUNT);
}

/* ── Lógica de escalada ──────────────────────────────────────────────────── */

/**
 * Avança o tempo ocioso em dt_ms e decide se/qual reação disparar.
 * Chamado pela task interna a cada NB_BOREDOM_CHECK_INTERVAL_S segundos.
 */
static void _check_and_react(uint64_t dt_ms)
{
    portENTER_CRITICAL(&s_mux);
    bool     paused           = s.paused;
    uint64_t idle_ms          = s.idle_ms;
    uint64_t last_reaction_ms = s.last_reaction_ms;
    uint64_t last_demon_ms    = s.last_demon_ms;
    portEXIT_CRITICAL(&s_mux);

    if (paused) {
        return;
    }

    idle_ms += dt_ms;

    /* Grace period: sem reações nos primeiros NB_BOREDOM_GRACE_MS. */
    if (idle_ms < NB_BOREDOM_GRACE_MS) {
        portENTER_CRITICAL(&s_mux);
        s.idle_ms = idle_ms;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    /* Tempo ocioso líquido (após grace period). */
    uint64_t idle_net = idle_ms - NB_BOREDOM_GRACE_MS;

    /* ── Modo demônio (prioridade máxima, elegível após 60min líquidos) ── */
    if (idle_net >= (NB_BOREDOM_DEMON_ELIGIBLE_MS - NB_BOREDOM_GRACE_MS)) {
        uint64_t now = _uptime_ms();
        bool demon_cd_ok = (last_demon_ms == 0U)
                        || ((now - last_demon_ms) >= NB_BOREDOM_DEMON_CD_MS);
        if (demon_cd_ok) {
            uint32_t roll = esp_random() % 100U;
            if (roll < (uint32_t)NB_BOREDOM_DEMON_PROB_PCT) {
                portENTER_CRITICAL(&s_mux);
                s.idle_ms          = idle_ms;
                s.last_demon_ms    = now;
                s.last_reaction_ms = idle_ms;
                portEXIT_CRITICAL(&s_mux);
                _react_demon();
                return;
            }
        }
    }

    /* ── Escalada comum (cooldown entre reações) ── */
    bool reaction_cd_ok = (last_reaction_ms == 0U)
                       || ((idle_ms - last_reaction_ms) >= NB_BOREDOM_REACTION_CD_MS);

    portENTER_CRITICAL(&s_mux);
    s.idle_ms = idle_ms;
    portEXIT_CRITICAL(&s_mux);

    if (!reaction_cd_ok) {
        return;
    }

    void (*react_fn)(void) = NULL;

    if (idle_net >= (NB_BOREDOM_LEVEL4_MS - NB_BOREDOM_GRACE_MS)) {
        react_fn = _react_level4_angry;
    } else if (idle_net >= (NB_BOREDOM_LEVEL3_MS - NB_BOREDOM_GRACE_MS)) {
        react_fn = _react_level3_suspicious;
    } else if (idle_net >= (NB_BOREDOM_LEVEL2_MS - NB_BOREDOM_GRACE_MS)) {
        react_fn = _react_level2_sad;
    } else if (idle_net >= (NB_BOREDOM_LEVEL1_MS - NB_BOREDOM_GRACE_MS)) {
        react_fn = _react_level1_curious;
    }

    if (react_fn != NULL) {
        portENTER_CRITICAL(&s_mux);
        s.last_reaction_ms = idle_ms;
        portEXIT_CRITICAL(&s_mux);
        react_fn();
    }
}

/* ── Task interna ────────────────────────────────────────────────────────── */

static void nb_boredom_task(void *arg)
{
    (void)arg;
    const uint64_t interval_ms = (uint64_t)NB_BOREDOM_CHECK_INTERVAL_S * 1000ULL;

    for (;;) {
        xSemaphoreTake(s.check_sem, portMAX_DELAY);
        _check_and_react(interval_ms);
    }
}

/* ── Timer callback ──────────────────────────────────────────────────────── */

static void _timer_cb(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s.check_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Handlers de interação via event bus ─────────────────────────────────── */

static void _on_interaction(const nb_event_t *evt, void *ctx)
{
    (void)evt;
    (void)ctx;
    boredom_service_on_interaction();
}

static void _on_state_changed(const nb_event_t *evt, void *ctx)
{
    (void)ctx;
    nb_robot_state_t state = (nb_robot_state_t)(evt->data.u32 & 0xFFU);
    bool pause = (state == NB_STATE_SLEEPING)
              || (state == NB_STATE_MEDITATION)
              || (state == NB_STATE_SILENT_COMPANY)
              || (state == NB_STATE_RESPONDING)
              || (state == NB_STATE_ERROR);
    boredom_service_set_paused(pause);
}

/* ── API pública ─────────────────────────────────────────────────────────── */

void boredom_service_on_interaction(void)
{
    portENTER_CRITICAL(&s_mux);
    s.idle_ms          = 0U;
    s.last_reaction_ms = 0U;
    portEXIT_CRITICAL(&s_mux);
}

void boredom_service_set_paused(bool paused)
{
    portENTER_CRITICAL(&s_mux);
    s.paused = paused;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t boredom_service_init(void)
{
    s.check_sem = xSemaphoreCreateBinary();
    if (s.check_sem == NULL) {
        NB_LOGE(TAG, "falha ao criar semáforo");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(nb_boredom_task, "nb_boredom_task",
                                BOREDOM_TASK_STACK, NULL,
                                BOREDOM_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        NB_LOGE(TAG, "falha ao criar task");
        vSemaphoreDelete(s.check_sem);
        s.check_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback              = _timer_cb,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "nb_boredom_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s.timer);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "falha ao criar timer: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s.timer,
              (uint64_t)NB_BOREDOM_CHECK_INTERVAL_S * 1000000ULL);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "falha ao iniciar timer: %s", esp_err_to_name(err));
        return err;
    }

    /* Eventos que reiniciam o contador de ociosidade. */
    static const nb_event_type_t s_interact_evts[] = {
        NB_EVT_TOUCH_TAP,
        NB_EVT_TOUCH_LONG_PRESS,
        NB_EVT_TOUCH_SUSTAINED,
        NB_EVT_TOUCH_WAKE,
        NB_EVT_TOUCH_DOUBLE_TAP,
        NB_EVT_TOUCH_DEEP,
        NB_EVT_TOUCH_CARESS,
        NB_EVT_TOUCH_WARM_PULSE,
        NB_EVT_VOICE_ACTIVITY_START,
        NB_EVT_WAKE_WORD_DETECTED,
        NB_EVT_BRIDGE_SAY,
        NB_EVT_BRIDGE_SESSION,
        NB_EVT_BRIDGE_CONNECTED,
        NB_EVT_BRIDGE_ACTION,
        NB_EVT_BRIDGE_EXPR,
    };
    const uint8_t n = (uint8_t)(sizeof(s_interact_evts) / sizeof(s_interact_evts[0]));

    for (uint8_t i = 0U; i < n && s.n_subs < BOREDOM_MAX_SUBS; i++) {
        nb_event_subscribe(s_interact_evts[i], _on_interaction, NULL,
                           &s.subs[s.n_subs]);
        s.n_subs++;
    }

    if (s.n_subs < BOREDOM_MAX_SUBS) {
        nb_event_subscribe(NB_EVT_STATE_CHANGED, _on_state_changed, NULL,
                           &s.subs[s.n_subs]);
        s.n_subs++;
    }

    NB_LOGI(TAG, "boredom_service iniciado — %u subs, check %us",
            s.n_subs, NB_BOREDOM_CHECK_INTERVAL_S);
    return ESP_OK;
}
