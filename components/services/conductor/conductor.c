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

#include <string.h>
#include <stdatomic.h>
#if CONFIG_NB_SD_SCORES
#include <stdio.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

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
/* AGREE — variação C: happy + pausa + nod (mais entusiasmado) */
static const score_step_t k_agree_c[] = {
    { 0,   NB_EXPR_HAPPY,  180.0f, true, 1400.0f, CM_NONE, 0, NULL },
    { 200, NB_EXPR_COUNT,  0.0f,   false, 0.0f,   CM_NOD,  0, NULL },
};

/* DISAGREE — variação A */
static const score_step_t k_disagree_a[] = {
    { 0,   NB_EXPR_SUSPICIOUS, 200.0f, true, 1400.0f, CM_SHAKE, 0, NULL },
};
/* DISAGREE — variação B */
static const score_step_t k_disagree_b[] = {
    { 0,   NB_EXPR_FOCUSED, 200.0f, true, 1400.0f, CM_SHAKE, 0, NULL },
};
/* DISAGREE — variação C: SAD + shake (discordância resignada/melancólica) */
static const score_step_t k_disagree_c[] = {
    { 0,   NB_EXPR_SAD,    300.0f, true, 1600.0f, CM_SHAKE, 0, NULL },
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
/* CURIOUS — variação C: foca primeiro, depois vira curioso (transição dupla) */
static const score_step_t k_curious_c[] = {
    { 0,   NB_EXPR_FOCUSED, 200.0f, true,  800.0f,  CM_NONE,         0, NULL },
    { 600, NB_EXPR_CURIOUS, 300.0f, true, 2000.0f,  CM_TILT_CURIOUS, 0, NULL },
};

/* TOUCH_WARM — variação A */
static const score_step_t k_touch_warm_a[] = {
    { 0,   NB_EXPR_HAPPY,   200.0f, true, 1500.0f, CM_NONE, 0, NULL },
    { 100, NB_EXPR_COUNT,   0.0f,   false,0.0f,    CM_NOD,  0, NULL },
};
/* TOUCH_WARM — variação B */
static const score_step_t k_touch_warm_b[] = {
    { 0,   NB_EXPR_HAPPY,   150.0f, true, 1500.0f, CM_NONE, 0, NULL },
};
/* TOUCH_WARM — variação C */
static const score_step_t k_touch_warm_c[] = {
    { 0,   NB_EXPR_CURIOUS, 200.0f, true, 1500.0f, CM_TILT_CURIOUS, 0, NULL },
};

/* TOUCH_STARTLE */
static const score_step_t k_startle_a[] = {
    { 0,   NB_EXPR_ALARMED, 50.0f,  true, 800.0f, CM_NONE, 0, NULL },
    { 200, NB_EXPR_COUNT,   0.0f,   false,0.0f,   CM_NOD,  0, NULL },
};
static const score_step_t k_startle_b[] = {
    { 0,   NB_EXPR_SURPRISED, 60.0f, true, 800.0f, CM_SHAKE, 0, NULL },
};
/* TOUCH_STARTLE — variação C: ALARMED + shake (mais intenso) */
static const score_step_t k_startle_c[] = {
    { 0,   NB_EXPR_ALARMED, 40.0f, true, 700.0f, CM_SHAKE, 0, NULL },
    { 300, NB_EXPR_COUNT,   0.0f,  false, 0.0f,  CM_NONE,  0, NULL },
};

/* SPEAK_LOOP — variação A: FOCUSED enquanto fala */
static const score_step_t k_speak_a[] = {
    { 0, NB_EXPR_FOCUSED, 200.0f, false, 0.0f, CM_NONE, 0, NULL },
};
/* SPEAK_LOOP — variação B: CURIOUS enquanto fala (parece mais engajado/pensativo) */
static const score_step_t k_speak_b[] = {
    { 0, NB_EXPR_CURIOUS, 250.0f, false, 0.0f, CM_NONE, 0, NULL },
};

/* SLEEP — transição longa, park servos */
static const score_step_t k_sleep_a[] = {
    { 8200,  NB_EXPR_COUNT,  0.0f, false, 0.0f, CM_PARK, 0, NULL },
    { 13000, NB_EXPR_SLEEPY, 0.0f, false, 0.0f, CM_NONE, 0, NULL },
};

/*
 * WAKE_UP — inspirado nos frames 0–20 de _mosaic_faces.jpg:
 *   1. ancora no SLEEPY atual;
 *   2. abre para um estado acordando, ainda pequeno/contido;
 *   3. estabiliza em NEUTRAL/CURIOUS antes de qualquer emoção social.
 *
 * Evita saltar SLEEPY -> HAPPY: os dois têm olhos fechados/arqueados e esse
 * corte parece continuação do sono em vez de despertar.
 */

/* WAKE_UP — variação A: sequência facial dedicada + gesto curioso */
static const score_step_t k_wake_a[] = {
    {    0, NB_EXPR_COUNT, 0.0f, false, 0.0f, CM_CENTER,       900, NULL },
    { 2100, NB_EXPR_COUNT, 0.0f, false, 0.0f, CM_TILT_CURIOUS,   0, NULL },
};
/* WAKE_UP — variação B: sequência facial dedicada + centralização calma */
static const score_step_t k_wake_b[] = {
    {    0, NB_EXPR_COUNT, 0.0f, false, 0.0f, CM_CENTER, 1100, NULL },
};
/* WAKE_UP — variação C: sequência facial dedicada + pequeno nod final */
static const score_step_t k_wake_c[] = {
    {    0, NB_EXPR_COUNT, 0.0f, false, 0.0f, CM_CENTER, 700, NULL },
    { 1900, NB_EXPR_COUNT, 0.0f, false, 0.0f, CM_NOD,      0, NULL },
};

/* CELEBRATE — marco especial: HAPPY prolongado + nod entusiasmado */
static const score_step_t k_celebrate_a[] = {
    { 0,    NB_EXPR_SURPRISED, 150.0f, true,  300.0f,  CM_NONE, 0, NULL },
    { 400,  NB_EXPR_HAPPY,     300.0f, false, 0.0f,    CM_NOD,  0, "/sdcard/assets/audio/greet_01.wav" },
    { 900,  NB_EXPR_SURPRISED, 150.0f, true,  300.0f,  CM_NONE, 0, NULL },
    { 1300, NB_EXPR_HAPPY,     400.0f, false, 0.0f,    CM_NOD,  0, NULL },
};

/* STRETCH — espreguiçar ao sair de DAWN: HAPPY + nod + volta ao neutro */
static const score_step_t k_stretch_a[] = {
    { 0,    NB_EXPR_HAPPY,   400.0f, true, 1200.0f, CM_NOD,    600, NULL },
    { 1400, NB_EXPR_NEUTRAL, 600.0f, false,   0.0f, CM_CENTER, 500, NULL },
};

/* ── Tabela de variações por ação ────────────────────────────────────────── */

#define SCORE(arr) { .steps=(arr), .count=(int)(sizeof(arr)/sizeof((arr)[0])) }

static const nb_score_t k_scores[NB_ACTION_COUNT][3] = {
    [NB_ACTION_NONE]          = { SCORE(k_greet_a), {NULL,0}, {NULL,0} },
    [NB_ACTION_GREET]         = { SCORE(k_greet_a), SCORE(k_greet_b), SCORE(k_greet_c) },
    [NB_ACTION_AGREE]         = { SCORE(k_agree_a), SCORE(k_agree_b), SCORE(k_agree_c) },
    [NB_ACTION_DISAGREE]      = { SCORE(k_disagree_a), SCORE(k_disagree_b), SCORE(k_disagree_c) },
    [NB_ACTION_CURIOUS]       = { SCORE(k_curious_a), SCORE(k_curious_b), SCORE(k_curious_c) },
    [NB_ACTION_TOUCH_WARM]    = { SCORE(k_touch_warm_a), SCORE(k_touch_warm_b), SCORE(k_touch_warm_c) },
    [NB_ACTION_TOUCH_STARTLE] = { SCORE(k_startle_a), SCORE(k_startle_b), SCORE(k_startle_c) },
    [NB_ACTION_SPEAK_LOOP]    = { SCORE(k_speak_a), SCORE(k_speak_b), {NULL,0} },
    [NB_ACTION_SLEEP]         = { SCORE(k_sleep_a), {NULL,0}, {NULL,0} },
    [NB_ACTION_WAKE_UP]       = { SCORE(k_wake_a), SCORE(k_wake_b), SCORE(k_wake_c) },
    [NB_ACTION_STRETCH]       = { SCORE(k_stretch_a),   {NULL,0}, {NULL,0} },
    [NB_ACTION_CELEBRATE]     = { SCORE(k_celebrate_a), {NULL,0}, {NULL,0} },
};

/* Número de variações por ação */
static const int k_num_vars[NB_ACTION_COUNT] = {
    [NB_ACTION_NONE]          = 1,
    [NB_ACTION_GREET]         = 3,
    [NB_ACTION_AGREE]         = 3,
    [NB_ACTION_DISAGREE]      = 3,
    [NB_ACTION_CURIOUS]       = 3,
    [NB_ACTION_TOUCH_WARM]    = 3,
    [NB_ACTION_TOUCH_STARTLE] = 3,
    [NB_ACTION_SPEAK_LOOP]    = 2,
    [NB_ACTION_SLEEP]         = 1,
    [NB_ACTION_WAKE_UP]       = 3,
    [NB_ACTION_STRETCH]       = 1,
    [NB_ACTION_CELEBRATE]     = 1,
};

/* ── Loader de partituras do SD (F45) ────────────────────────────────────── */
/* Habilitado por CONFIG_NB_SD_SCORES (default n). Apenas para iteração de   */
/* animações sem reflash. Suporta sobrescrever 1 ação por boot.              */

#if CONFIG_NB_SD_SCORES

#define SD_SCORE_MAX_STEPS   8u
#define SD_SCORE_MAX_AUDIO   64u

static score_step_t  s_dyn_steps[3][SD_SCORE_MAX_STEPS];
static char          s_dyn_audio[3][SD_SCORE_MAX_STEPS][SD_SCORE_MAX_AUDIO];
static nb_score_t    s_dyn_scores[3];
static nb_action_t   s_dyn_action  = NB_ACTION_NONE;
static bool          s_dyn_loaded  = false;

static bool parse_score_line(const char *line, score_step_t *out, char *audio_out)
{
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        return false;

    unsigned offset_u, motion_ms_u;
    int expr, expr_play, motion;
    float expr_ms, expr_dur;
    char audio[SD_SCORE_MAX_AUDIO];

    int n = sscanf(line, "%u %d %f %d %f %d %u %63s",
                   &offset_u, &expr, &expr_ms, &expr_play,
                   &expr_dur, &motion, &motion_ms_u, audio);
    if (n < 8) return false;

    out->offset_ms  = (uint32_t)offset_u;
    out->expr       = (nb_expression_t)expr;
    out->expr_ms    = expr_ms;
    out->expr_play  = (bool)expr_play;
    out->expr_dur   = expr_dur;
    out->motion     = (cond_motion_t)motion;
    out->motion_ms  = (uint32_t)motion_ms_u;

    if (audio[0] == '-' && audio[1] == '\0') {
        out->audio = NULL;
    } else {
        strncpy(audio_out, audio, SD_SCORE_MAX_AUDIO - 1u);
        audio_out[SD_SCORE_MAX_AUDIO - 1u] = '\0';
        out->audio = audio_out;
    }
    return true;
}

static int load_variant(nb_action_t action, int var)
{
    char path[48];
    snprintf(path, sizeof(path), "/sdcard/scores/%d_%d.txt", (int)action, var);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[128];
    int count = 0;
    while (fgets(line, sizeof(line), f) && (unsigned)count < SD_SCORE_MAX_STEPS) {
        if (parse_score_line(line, &s_dyn_steps[var][count],
                             s_dyn_audio[var][count])) {
            count++;
        }
    }
    fclose(f);
    return count;
}

static void load_sd_scores(void)
{
    for (int action = 1; action < NB_ACTION_COUNT; action++) {
        bool any = false;
        for (int var = 0; var < 3; var++) {
            int count = load_variant((nb_action_t)action, var);
            if (count > 0) {
                s_dyn_scores[var].steps = s_dyn_steps[var];
                s_dyn_scores[var].count = count;
                any = true;
                ESP_LOGI(TAG, "SD score: action=%d var=%d steps=%d", action, var, count);
            } else {
                s_dyn_scores[var].steps = NULL;
                s_dyn_scores[var].count = 0;
            }
        }
        if (any) {
            s_dyn_action = (nb_action_t)action;
            s_dyn_loaded = true;
            ESP_LOGI(TAG, "SD scores ativos para action=%d", action);
            return;  /* apenas 1 ação por vez */
        }
    }
}

#endif /* CONFIG_NB_SD_SCORES */

static const nb_score_t *get_score(nb_action_t action, int var)
{
#if CONFIG_NB_SD_SCORES
    if (s_dyn_loaded && action == s_dyn_action &&
        var < 3 && s_dyn_scores[var].steps != NULL) {
        return &s_dyn_scores[var];
    }
#endif
    return &k_scores[action][var];
}

static int get_num_vars(nb_action_t action)
{
#if CONFIG_NB_SD_SCORES
    if (s_dyn_loaded && action == s_dyn_action) {
        int n = 0;
        for (int v = 0; v < 3; v++) {
            if (s_dyn_scores[v].steps != NULL) n = v + 1;
        }
        if (n > 0) return n;
    }
#endif
    return k_num_vars[action];
}

/* ── Estado interno ──────────────────────────────────────────────────────── */

static SemaphoreHandle_t  s_trigger_sem     = NULL;
static portMUX_TYPE       s_mux             = portMUX_INITIALIZER_UNLOCKED;
static volatile nb_action_t s_pending_action = NB_ACTION_NONE;
static volatile nb_action_t s_current_action = NB_ACTION_NONE;
static volatile bool        s_interrupt      = false;
static bool                 s_initialized    = false;
static _Atomic uint32_t     s_pause_count    = 0;

/* Anti-repeat: última variação jogada por ação (-1 = nunca jogou). */
static int8_t s_last_var[NB_ACTION_COUNT];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void execute_motion(cond_motion_t m, uint32_t ms)
{
    if (!motion_service_is_ready()) return;

    switch (m) {
        case CM_NONE:          break;
        case CM_NOD:           motion_neck_nod();                          break;
        case CM_SHAKE:         motion_neck_shake();                        break;
        case CM_TILT_CURIOUS:  motion_neck_tilt_curious();                 break;
        case CM_PARK:          motion_park_all();                          break;
        case CM_CENTER:        motion_neck_look_at(0.0f, 0.0f, ms ? ms : 500); break;
    }
}

/* Dorme até o próximo step ou até interrupção, em chunks de 20ms.
 * Usa elapsed = now - t0 para lidar corretamente com wrap de uint32
 * (ocorre após ~49 dias de uptime). */
static bool sleep_until(uint32_t target_ms, uint32_t t0_ms)
{
    while (!s_interrupt) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t elapsed = now_ms - t0_ms;
        if (elapsed >= target_ms) return false;
        uint32_t remain = target_ms - elapsed;
        uint32_t chunk = remain > 20u ? 20u : remain;
        vTaskDelay(pdMS_TO_TICKS(chunk));
    }
    return true;
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
            continue;
        }

        /* Selecionar variação — sem repetir a última (anti-repeat) */
        int nvars = get_num_vars(action);
        int var;
        if (nvars <= 1) {
            var = 0;
        } else {
            var = (int)(esp_random() % (uint32_t)nvars);
            if (var == (int)s_last_var[action]) {
                var = (var + 1) % nvars;   /* avança para próxima */
            }
        }
        s_last_var[action] = (int8_t)var;
        const nb_score_t *score = get_score(action, var);

        if (!score->steps || score->count == 0) {
            taskENTER_CRITICAL(&s_mux);
            s_current_action = NB_ACTION_NONE;
            taskEXIT_CRITICAL(&s_mux);
            continue;
        }

        ESP_LOGI(TAG, "play action=%d var=%d steps=%d", (int)action, var, score->count);

        /* Micro-expressão: flash emocional breve antes da expressão principal */
        switch (action) {
            case NB_ACTION_TOUCH_WARM:
                expression_play(NB_EXPR_SURPRISED, 80.0f, 40.0f);
                break;
            case NB_ACTION_TOUCH_STARTLE:
            case NB_ACTION_GREET:
                expression_play(NB_EXPR_SURPRISED, 100.0f, 40.0f);
                break;
            case NB_ACTION_SPEAK_LOOP:
                expression_play(NB_EXPR_ALARMED, 120.0f, 40.0f);
                break;
            default: break;
        }
        if (action == NB_ACTION_WAKE_UP) {
            expression_service_play_wake_sequence();
        }

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

        taskENTER_CRITICAL(&s_mux);
        s_current_action = NB_ACTION_NONE;
        taskEXIT_CRITICAL(&s_mux);
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t conductor_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    memset(s_last_var, -1, sizeof(s_last_var));
    s_trigger_sem = xSemaphoreCreateBinary();
    if (!s_trigger_sem) return ESP_ERR_NO_MEM;

    BaseType_t rc;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    rc = xTaskCreateWithCaps(conductor_task, "conductor_task",
                             4096, NULL, 6, NULL,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    rc = xTaskCreate(conductor_task, "conductor_task",
                     4096, NULL, 6, NULL);
#endif
    if (rc != pdPASS) {
        vSemaphoreDelete(s_trigger_sem);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

#if CONFIG_NB_SD_SCORES
    load_sd_scores();
#endif

    ESP_LOGI(TAG, "conductor inicializado (%d ações)", NB_ACTION_COUNT - 1);
    return ESP_OK;
}

void conductor_pause(bool pause)
{
    if (pause) {
        atomic_fetch_add(&s_pause_count, 1u);
    } else if (atomic_load(&s_pause_count) > 0u) {
        atomic_fetch_sub(&s_pause_count, 1u);
    }
}

void conductor_play(nb_action_t action)
{
    if (!s_initialized || atomic_load(&s_pause_count) > 0u) return;
    if ((int)action < 0 || action >= NB_ACTION_COUNT) {
        ESP_LOGE(TAG, "conductor_play: action=%d inválida", (int)action);
        return;
    }
    if (action == NB_ACTION_CELEBRATE) {
        expression_service_overlay_heart(2200U);
    }

    taskENTER_CRITICAL(&s_mux);
    bool was_running = (s_current_action != NB_ACTION_NONE);
    s_pending_action = action;
    if (was_running) s_interrupt = true;
    taskEXIT_CRITICAL(&s_mux);

    xSemaphoreGive(s_trigger_sem);
}

nb_action_t conductor_get_current(void)
{
    taskENTER_CRITICAL(&s_mux);
    nb_action_t a = s_current_action;
    taskEXIT_CRITICAL(&s_mux);
    return a;
}
