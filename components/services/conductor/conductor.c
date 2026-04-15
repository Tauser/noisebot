/*
 * conductor.c — Orquestrador de ações do NoiseBot
 *
 * Cada ação tem 1–3 variações (nb_score_t). A variação é sorteada em
 * hardware RNG a cada disparo para evitar repetitividade.
 *
 * Partitura (score): array de steps com offset_ms relativo ao início da ação.
 * A task itera os steps em ordem, dormindo entre eles. A cada wake-up verifica
 * s_interrupt. Se setado: para áudio, para motion suave, retorna ao idle.
 *
 * Motion: usa os primitivos de alto nível do motion_service (nod, shake, etc.).
 * Gaze: reset ao centro na maioria das ações para evitar conflito com gaze_service.
 * Expression: expression_play() para temporárias; expression_service_set() para
 *   estados duradouros (SLEEP, WAKE_UP).
 */

#include "conductor.h"
#include "expression_service.h"
#include "motion_service.h"
#include "audio_service.h"
#include "gaze_service.h"
#include "state_machine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#define TAG "nb_cond"

/* ── Tipos internos ──────────────────────────────────────────────────────── */

/* Tipo de movimento no step (uso dos primitivos do motion_service). */
typedef enum {
    CM_NONE = 0,
    CM_NOD,            /* motion_neck_nod()          */
    CM_SHAKE,          /* motion_neck_shake()         */
    CM_TILT_CURIOUS,   /* motion_neck_tilt_curious()  */
    CM_PARK,           /* motion_park_all()           */
    CM_CENTER,         /* motion_neck_look_at(0,0,ms) */
} cond_motion_t;

/* Step de partitura. offset_ms relativo ao início da ação. */
typedef struct {
    uint32_t        offset_ms;
    nb_expression_t expr;      /* NB_EXPR_COUNT = sem mudança de expressão */
    float           expr_ms;   /* duração da transição de expressão        */
    bool            expr_play; /* true = expression_play; false = set base */
    float           expr_dur;  /* duração do play (só se expr_play=true)   */
    cond_motion_t   motion;
    uint32_t        motion_ms; /* duração para CM_CENTER (ignorado em gestos) */
    const char     *audio;     /* NULL = sem áudio                         */
} score_step_t;

typedef struct {
    const score_step_t *steps;
    int                 count;
} nb_score_t;

/* ── Partituras ──────────────────────────────────────────────────────────── */

/* Macro auxiliar para preencher step sem motion e sem áudio */
#define STEP(off, ex, ex_ms)  \
    { .offset_ms=(off), .expr=(ex), .expr_ms=(ex_ms),   \
      .expr_play=true, .expr_dur=1000.0f,               \
      .motion=CM_NONE, .motion_ms=0, .audio=NULL }

/* GREET — variação A: nod + happy + áudio greet_01 */
static const score_step_t k_greet_a[] = {
    { 0,    NB_EXPR_HAPPY,   300.0f, true,  1800.0f, CM_NONE, 0, NULL },
    { 150,  NB_EXPR_COUNT,   0.0f,   false, 0.0f,    CM_NOD,  0, "/sdcard/assets/audio/greet_01.wav" },
};

/* GREET — variação B: tilt curious + happy + áudio greet_02 */
static const score_step_t k_greet_b[] = {
    { 0,    NB_EXPR_CURIOUS, 250.0f, true,  1800.0f, CM_NONE,        0, NULL },
    { 100,  NB_EXPR_COUNT,   0.0f,   false, 0.0f,    CM_TILT_CURIOUS,0, "/sdcard/assets/audio/greet_02.wav" },
};

/* GREET — variação C: shake cabeça levemente + surprised + greet_03 */
static const score_step_t k_greet_c[] = {
    { 0,    NB_EXPR_SURPRISED, 200.0f, true,  1500.0f, CM_NONE, 0, NULL },
    { 200,  NB_EXPR_COUNT,     0.0f,   false, 0.0f,    CM_NOD,  0, "/sdcard/assets/audio/greet_03.wav" },
};

/* AGREE — variação A */
static const score_step_t k_agree_a[] = {
    { 0,   NB_EXPR_HAPPY,  200.0f, true, 1200.0f, CM_NOD,  0, NULL },
};
/* AGREE — variação B */
static const score_step_t k_agree_b[] = {
    { 0,   NB_EXPR_CURIOUS, 200.0f, true, 1200.0f, CM_NOD, 0, NULL },
};

/* DISAGREE — variação A */
static const score_step_t k_disagree_a[] = {
    { 0,   NB_EXPR_SUSPICIOUS, 200.0f, true, 1400.0f, CM_SHAKE, 0, NULL },
};
/* DISAGREE — variação B */
static const score_step_t k_disagree_b[] = {
    { 0,   NB_EXPR_FOCUSED, 200.0f, true, 1400.0f, CM_SHAKE, 0, NULL },
};

/* CURIOUS — variação A */
static const score_step_t k_curious_a[] = {
    { 0,    NB_EXPR_CURIOUS, 300.0f, true, 2200.0f, CM_TILT_CURIOUS, 0, NULL },
};
/* CURIOUS — variação B */
static const score_step_t k_curious_b[] = {
    { 0,    NB_EXPR_CURIOUS, 250.0f, true, 2000.0f, CM_NONE,         0, NULL },
    { 400,  NB_EXPR_COUNT,   0.0f,   false,0.0f,    CM_TILT_CURIOUS, 0, NULL },
};

/* TOUCH_WARM — variação A */
static const score_step_t k_touch_warm_a[] = {
    { 0,   NB_EXPR_HAPPY,   200.0f, true, 1500.0f, CM_NONE, 0, "/sdcard/assets/audio/touch_respond_01.wav" },
    { 100, NB_EXPR_COUNT,   0.0f,   false,0.0f,    CM_NOD,  0, NULL },
};
/* TOUCH_WARM — variação B */
static const score_step_t k_touch_warm_b[] = {
    { 0,   NB_EXPR_HAPPY,   150.0f, true, 1500.0f, CM_NONE, 0, "/sdcard/assets/audio/touch_respond_02.wav" },
};
/* TOUCH_WARM — variação C */
static const score_step_t k_touch_warm_c[] = {
    { 0,   NB_EXPR_CURIOUS, 200.0f, true, 1500.0f, CM_TILT_CURIOUS, 0, "/sdcard/assets/audio/touch_respond_03.wav" },
};

/* TOUCH_STARTLE */
static const score_step_t k_startle_a[] = {
    { 0,   NB_EXPR_ALARMED, 50.0f,  true, 800.0f, CM_NONE, 0, NULL },
    { 200, NB_EXPR_COUNT,   0.0f,   false,0.0f,   CM_NOD,  0, NULL },
};
static const score_step_t k_startle_b[] = {
    { 0,   NB_EXPR_SURPRISED, 60.0f, true, 800.0f, CM_SHAKE, 0, NULL },
};

/* SPEAK_LOOP — expressão FOCUSED enquanto fala */
static const score_step_t k_speak_a[] = {
    { 0, NB_EXPR_FOCUSED, 200.0f, false, 0.0f, CM_NONE, 0, NULL },
};

/* SLEEP — transição longa, park servos */
static const score_step_t k_sleep_a[] = {
    { 0,    NB_EXPR_SLEEPY, 1500.0f, false, 0.0f, CM_NONE, 0, NULL },
    { 600,  NB_EXPR_COUNT,  0.0f,    false, 0.0f, CM_PARK, 0, NULL },
};

/* WAKE_UP */
static const score_step_t k_wake_a[] = {
    { 0,   NB_EXPR_NEUTRAL, 800.0f, false, 0.0f, CM_CENTER, 600, NULL },
    { 900, NB_EXPR_COUNT,   0.0f,   false, 0.0f, CM_TILT_CURIOUS, 0, NULL },
};
static const score_step_t k_wake_b[] = {
    { 0,   NB_EXPR_NEUTRAL, 800.0f, false, 0.0f, CM_CENTER, 600, NULL },
};

/* ── Tabela de variações por ação ────────────────────────────────────────── */

#define SCORE(arr) { .steps=(arr), .count=(int)(sizeof(arr)/sizeof((arr)[0])) }

static const nb_score_t k_scores[NB_ACTION_COUNT][3] = {
    [NB_ACTION_NONE]          = { SCORE(k_greet_a), {NULL,0}, {NULL,0} },
    [NB_ACTION_GREET]         = { SCORE(k_greet_a), SCORE(k_greet_b), SCORE(k_greet_c) },
    [NB_ACTION_AGREE]         = { SCORE(k_agree_a), SCORE(k_agree_b), {NULL,0} },
    [NB_ACTION_DISAGREE]      = { SCORE(k_disagree_a), SCORE(k_disagree_b), {NULL,0} },
    [NB_ACTION_CURIOUS]       = { SCORE(k_curious_a), SCORE(k_curious_b), {NULL,0} },
    [NB_ACTION_TOUCH_WARM]    = { SCORE(k_touch_warm_a), SCORE(k_touch_warm_b), SCORE(k_touch_warm_c) },
    [NB_ACTION_TOUCH_STARTLE] = { SCORE(k_startle_a), SCORE(k_startle_b), {NULL,0} },
    [NB_ACTION_SPEAK_LOOP]    = { SCORE(k_speak_a), {NULL,0}, {NULL,0} },
    [NB_ACTION_SLEEP]         = { SCORE(k_sleep_a), {NULL,0}, {NULL,0} },
    [NB_ACTION_WAKE_UP]       = { SCORE(k_wake_a), SCORE(k_wake_b), {NULL,0} },
};

/* Número de variações por ação */
static const int k_num_vars[NB_ACTION_COUNT] = {
    [NB_ACTION_NONE]          = 1,
    [NB_ACTION_GREET]         = 3,
    [NB_ACTION_AGREE]         = 2,
    [NB_ACTION_DISAGREE]      = 2,
    [NB_ACTION_CURIOUS]       = 2,
    [NB_ACTION_TOUCH_WARM]    = 3,
    [NB_ACTION_TOUCH_STARTLE] = 2,
    [NB_ACTION_SPEAK_LOOP]    = 1,
    [NB_ACTION_SLEEP]         = 1,
    [NB_ACTION_WAKE_UP]       = 2,
};

/* ── Estado interno ──────────────────────────────────────────────────────── */

static SemaphoreHandle_t  s_trigger_sem     = NULL;
static portMUX_TYPE       s_mux             = portMUX_INITIALIZER_UNLOCKED;
static volatile nb_action_t s_pending_action = NB_ACTION_NONE;
static volatile nb_action_t s_current_action = NB_ACTION_NONE;
static volatile bool        s_interrupt      = false;
static bool                 s_initialized    = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void execute_motion(cond_motion_t m, uint32_t ms)
{
    switch (m) {
        case CM_NONE:          break;
        case CM_NOD:           motion_neck_nod();                          break;
        case CM_SHAKE:         motion_neck_shake();                        break;
        case CM_TILT_CURIOUS:  motion_neck_tilt_curious();                 break;
        case CM_PARK:          motion_park_all();                          break;
        case CM_CENTER:        motion_neck_look_at(0.0f, 0.0f, ms ? ms : 500); break;
    }
}

/* Dorme até o próximo step ou até interrupção, em chunks de 20ms. */
static bool sleep_until(uint32_t target_ms, uint32_t t0_ms)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now_ms >= t0_ms + target_ms) return false;
    uint32_t remain = (t0_ms + target_ms) - now_ms;
    while (remain > 0 && !s_interrupt) {
        uint32_t chunk = remain > 20 ? 20 : remain;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        remain = remain > chunk ? remain - chunk : 0;
    }
    return s_interrupt;
}

/* ── Conductor task ──────────────────────────────────────────────────────── */

static void conductor_task(void *arg)
{
    (void)arg;

    while (1) {
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        /* Consumir ação pendente */
        taskENTER_CRITICAL(&s_mux);
        nb_action_t action    = s_pending_action;
        s_pending_action      = NB_ACTION_NONE;
        s_current_action      = action;
        s_interrupt           = false;
        taskEXIT_CRITICAL(&s_mux);

        if (action == NB_ACTION_NONE) {
            s_current_action = NB_ACTION_NONE;
            continue;
        }

        /* Selecionar variação aleatória */
        int nvars = k_num_vars[action];
        int var   = (nvars > 1) ? (int)(esp_random() % (uint32_t)nvars) : 0;
        const nb_score_t *score = &k_scores[action][var];

        if (!score->steps || score->count == 0) {
            s_current_action = NB_ACTION_NONE;
            continue;
        }

        ESP_LOGI(TAG, "play action=%d var=%d steps=%d", (int)action, var, score->count);

        uint32_t t0_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        for (int i = 0; i < score->count && !s_interrupt; i++) {
            const score_step_t *step = &score->steps[i];

            /* Aguarda o offset deste step */
            if (sleep_until(step->offset_ms, t0_ms)) break;

            /* Expressão */
            if (step->expr < NB_EXPR_COUNT) {
                if (step->expr_play) {
                    expression_play(step->expr, step->expr_dur, step->expr_ms);
                } else {
                    expression_service_set(step->expr, step->expr_ms);
                }
            }

            /* Motion */
            if (step->motion != CM_NONE) {
                execute_motion(step->motion, step->motion_ms);
            }

            /* Áudio */
            if (step->audio) {
                audio_play_file(step->audio);
            }
        }

        /* Cleanup em caso de interrupção */
        if (s_interrupt) {
            audio_play_stop();
            motion_stop(1);
            motion_stop(2);
            ESP_LOGD(TAG, "action=%d interrompida", (int)action);
        }

        s_current_action = NB_ACTION_NONE;
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t conductor_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    s_trigger_sem = xSemaphoreCreateBinary();
    if (!s_trigger_sem) return ESP_ERR_NO_MEM;

    BaseType_t rc = xTaskCreate(conductor_task, "conductor_task",
                                4096, NULL, 6, NULL);
    if (rc != pdPASS) {
        vSemaphoreDelete(s_trigger_sem);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "conductor inicializado (%d ações)", NB_ACTION_COUNT - 1);
    return ESP_OK;
}

void conductor_play(nb_action_t action)
{
    if (!s_initialized) return;

    taskENTER_CRITICAL(&s_mux);
    bool was_running = (s_current_action != NB_ACTION_NONE);
    s_pending_action = action;
    if (was_running) s_interrupt = true;
    taskEXIT_CRITICAL(&s_mux);

    xSemaphoreGive(s_trigger_sem);
}

nb_action_t conductor_get_current(void)
{
    return s_current_action;
}
