/*
 * emotion_model.c — Modelo emocional do NoiseBot (Layer 6)
 *
 * Vetor emocional (valência, ativação) mapeado por nearest-neighbor
 * aos 9 anchors das expressões base, com histerese para evitar flickering
 * na fronteira entre regiões de Voronoi.
 *
 * Decaimento:
 *   v(t) = v0 * (1 - DECAY_RATE_PER_MS)^t
 *   Com DECAY_RATE_PER_MS = 5e-5, em ~60 000ms: v ≈ 0.05 * v0 (<5%). ✓
 *
 * Expressão só é atualizada quando muda — evita resetar a interpolação
 * do expression_service a cada tick.
 */

#include "emotion_model.h"
#include "expression_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"

#define TAG "nb_emot"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

/** Taxa de decaimento por ms: 1 - 5e-5 ≈ 0.995 por tick de 100ms. */
#define DECAY_RATE_PER_MS   5e-5f

/** Duração da transição de expressão ao mudar emoção. */
#define TRANSITION_MS       400.0f

/**
 * Margem de histerese em distância² para troca de expressão.
 * O novo anchor deve ser mais próximo que o atual por pelo menos
 * HYSTERESIS_D2 — equivale a ~0.09u de vantagem linear.
 * Suprime flickering quando o vetor oscila na fronteira de Voronoi
 * durante o decaimento (DECAY ≈ 0.005u/tick), sem atrasar transições
 * causadas por eventos (deltas >> 0.1u).
 */
#define HYSTERESIS_D2       0.008f

/* ── Anchors (valência, ativação) ────────────────────────────────────────── */

typedef struct {
    float           v;
    float           a;
    nb_expression_t expr;
} nb_emotion_anchor_t;

/*
 * Anchors calibrados para separação máxima no espaço 2D.
 * Cada expressão tem um ponto de referência único.
 */
static const nb_emotion_anchor_t k_anchors[NB_EXPR_COUNT] = {
    [NB_EXPR_NEUTRAL   ] = {  0.0f,  0.0f, NB_EXPR_NEUTRAL    },
    [NB_EXPR_HAPPY     ] = {  0.7f,  0.4f, NB_EXPR_HAPPY      },
    [NB_EXPR_CURIOUS   ] = {  0.3f,  0.5f, NB_EXPR_CURIOUS    },
    [NB_EXPR_FOCUSED   ] = {  0.1f,  0.7f, NB_EXPR_FOCUSED    },
    [NB_EXPR_SUSPICIOUS] = { -0.5f,  0.2f, NB_EXPR_SUSPICIOUS },
    [NB_EXPR_SLEEPY    ] = {  0.0f, -0.8f, NB_EXPR_SLEEPY     },
    [NB_EXPR_SURPRISED ] = {  0.2f,  0.9f, NB_EXPR_SURPRISED  },
    [NB_EXPR_SAD       ] = { -0.6f, -0.4f, NB_EXPR_SAD        },
    [NB_EXPR_ALARMED   ] = { -0.5f,  0.8f, NB_EXPR_ALARMED    },
};

/* ── Deltas por evento ───────────────────────────────────────────────────── */

typedef struct { float dv; float da; } nb_emotion_delta_t;

static const nb_emotion_delta_t k_event_deltas[NB_EMOT_EVT_COUNT] = {
    [NB_EMOT_EVT_TOUCH_TAP     ] = {  0.40f,  0.20f },
    [NB_EMOT_EVT_TOUCH_LONG    ] = {  0.60f,  0.30f },
    [NB_EMOT_EVT_VOICE_START   ] = {  0.10f,  0.50f },
    [NB_EMOT_EVT_AUDIO_STARTED ] = {  0.20f,  0.30f },
    [NB_EMOT_EVT_ENTERING_SLEEP] = {  0.00f, -0.80f },
    [NB_EMOT_EVT_WAKING_UP     ] = {  0.10f,  0.30f },
    [NB_EMOT_EVT_MOTION_FAULT  ] = { -0.80f,  0.90f },
    [NB_EMOT_EVT_IDLE_LONG     ] = { -0.35f, -0.30f },
    [NB_EMOT_EVT_VOICE_LOUD       ] = { -0.50f,  0.80f },
    [NB_EMOT_EVT_VOICE_SOFT       ] = {  0.15f,  0.40f },
    [NB_EMOT_EVT_TOUCH_WARM_PULSE ] = {  0.20f,  0.10f }, /* calor lento acumulando */
    [NB_EMOT_EVT_TOUCH_DEEP       ] = {  0.70f,  0.30f }, /* calor intenso           */
    [NB_EMOT_EVT_TOUCH_CARESS     ] = {  0.90f,  0.15f }, /* satisfação máxima       */
};

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_emotion_vec_t       s_vec          = { 0.0f, 0.0f };
static nb_expression_t        s_expr         = NB_EXPR_NEUTRAL;
static nb_emotion_change_cb_t s_change_cb    = NULL;
static portMUX_TYPE           s_mux          = portMUX_INITIALIZER_UNLOCKED;
static bool                   s_initialized  = false;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Mapeamento por nearest-neighbor com histerese.
 *
 * Encontra o anchor absoluto mais próximo. Se ele for diferente de `current`,
 * só troca quando a vantagem de distância² for > HYSTERESIS_D2 — cria uma
 * zona morta na fronteira de Voronoi que suprime flickering por decaimento.
 */
static nb_expression_t vec_to_expr(float v, float a, nb_expression_t current)
{
    float best_d2       = 1e9f;
    nb_expression_t best = NB_EXPR_NEUTRAL;

    float cv  = v - k_anchors[current].v;
    float ca  = a - k_anchors[current].a;
    float curr_d2 = cv * cv + ca * ca;

    for (int i = 0; i < NB_EXPR_COUNT; i++) {
        float dv = v - k_anchors[i].v;
        float da = a - k_anchors[i].a;
        float d2 = dv * dv + da * da;
        if (d2 < best_d2) {
            best_d2 = d2;
            best    = k_anchors[i].expr;
        }
    }

    /* Histerese: só sai do anchor atual se o melhor for visivelmente mais próximo. */
    if (best != current && best_d2 + HYSTERESIS_D2 >= curr_d2) {
        best = current;
    }

    return best;
}

/*
 * Avalia nova expressão com histerese e, se mudou, chama expression_service
 * e callback. Deve ser chamado fora de critical section.
 */
static void maybe_update_expr(float v, float a)
{
    taskENTER_CRITICAL(&s_mux);
    nb_expression_t curr = s_expr;
    taskEXIT_CRITICAL(&s_mux);

    nb_expression_t new_expr = vec_to_expr(v, a, curr);

    if (new_expr != curr) {
        taskENTER_CRITICAL(&s_mux);
        s_expr = new_expr;
        taskEXIT_CRITICAL(&s_mux);

        expression_service_set(new_expr, TRANSITION_MS);
        ESP_LOGI(TAG, "expr %d → %d (v=%.2f a=%.2f)",
                 (int)curr, (int)new_expr, v, a);
        if (s_change_cb) {
            s_change_cb(new_expr);
        }
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t emotion_model_init(nb_expression_t         initial_expr,
                              nb_emotion_change_cb_t  change_cb)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    /* Inicializar vetor no anchor da expressão inicial. */
    nb_expression_t safe_expr = (initial_expr < NB_EXPR_COUNT)
                                  ? initial_expr
                                  : NB_EXPR_NEUTRAL;

    s_vec.valence    = k_anchors[safe_expr].v;
    s_vec.activation = k_anchors[safe_expr].a;
    s_expr           = safe_expr;
    s_change_cb      = change_cb;
    s_initialized    = true;

    expression_service_set(s_expr, TRANSITION_MS);

    ESP_LOGI(TAG, "init: expr=%d v=%.2f a=%.2f",
             (int)s_expr, s_vec.valence, s_vec.activation);
    return ESP_OK;
}

void emotion_model_update(uint32_t dt_ms)
{
    if (!s_initialized) return;

    float decay = 1.0f - DECAY_RATE_PER_MS * (float)dt_ms;
    if (decay < 0.0f) decay = 0.0f;

    taskENTER_CRITICAL(&s_mux);
    s_vec.valence    *= decay;
    s_vec.activation *= decay;
    float v = s_vec.valence;
    float a = s_vec.activation;
    taskEXIT_CRITICAL(&s_mux);

    maybe_update_expr(v, a);
}

void emotion_model_on_event(nb_emotion_event_t evt)
{
    if (!s_initialized || (int)evt < 0 || evt >= NB_EMOT_EVT_COUNT) return;

    /* WAKING_UP: reseta vetor para NEUTRAL antes de aplicar o delta.
     * Sem o reset, o vetor acumulado do sono (próximo de SLEEPY) pode
     * continuar selecionando SLEEPY durante a animação de wake do conductor,
     * sobrepondo a transição de olhos abrindo com olhos fechados. */
    if (evt == NB_EMOT_EVT_WAKING_UP) {
        taskENTER_CRITICAL(&s_mux);
        s_vec.valence    = k_anchors[NB_EXPR_NEUTRAL].v;
        s_vec.activation = k_anchors[NB_EXPR_NEUTRAL].a;
        s_expr           = NB_EXPR_NEUTRAL;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGD(TAG, "waking_up: vetor resetado para NEUTRAL");
        return;
    }

    const nb_emotion_delta_t *d = &k_event_deltas[evt];

    taskENTER_CRITICAL(&s_mux);
    s_vec.valence    = clampf(s_vec.valence    + d->dv, -1.0f, 1.0f);
    s_vec.activation = clampf(s_vec.activation + d->da, -1.0f, 1.0f);
    float v = s_vec.valence;
    float a = s_vec.activation;
    taskEXIT_CRITICAL(&s_mux);

    ESP_LOGD(TAG, "evt=%d: v=%.2f a=%.2f", (int)evt, v, a);
    maybe_update_expr(v, a);
}

nb_expression_t emotion_model_get_expression(void)
{
    nb_expression_t e;
    taskENTER_CRITICAL(&s_mux);
    e = s_expr;
    taskEXIT_CRITICAL(&s_mux);
    return e;
}

nb_emotion_vec_t emotion_model_get_vec(void)
{
    nb_emotion_vec_t v;
    taskENTER_CRITICAL(&s_mux);
    v = s_vec;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}
