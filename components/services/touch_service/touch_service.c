/*
 * touch_service.c — Serviço de touch do NoiseBot (Layer 4)
 *
 * ── Filtragem de sinal ───────────────────────────────────────────────────
 *   filtered_raw = filtered_raw × 0.8 + raw × 0.2  (EMA, α=SIGNAL_EMA_ALPHA)
 *   A FSM e os thresholds operam sobre filtered_raw, não sobre raw bruto.
 *   raw bruto é preservado para log interno e recalibração da janela de ruído.
 *
 * ── Histerese ────────────────────────────────────────────────────────────
 *   threshold_on  = baseline × (1 + sensitivity)
 *   threshold_off = baseline × (1 + sensitivity × HYST_FACTOR)
 *   HYST_FACTOR = 0.40 → gap entre on e off é 60% do span de sensitivity.
 *   Evita chatter quando sinal oscila perto do limite de detecção.
 *
 * ── Debounce de entrada ───────────────────────────────────────────────────
 *   Exige DEBOUNCE_ON_COUNT (2) amostras consecutivas de filtered_raw acima
 *   de threshold_on para confirmar toque e sair de IDLE.
 *   Tradeoff: latência real de TAP ≈ 2×20ms = 40ms (vs. spec <20ms),
 *   mas elimina disparo por pico isolado de ruído.
 *
 * ── Debounce de release ───────────────────────────────────────────────────
 *   Exige DEBOUNCE_OFF_COUNT (2) amostras consecutivas abaixo de threshold_off
 *   para confirmar liberação do toque.
 *
 * ── Recalibração lenta ────────────────────────────────────────────────────
 *   Quando IDLE e sinal estável (spread < NOISE_SPREAD_PCT% do baseline):
 *     baseline ← baseline × (1 - RECALIB_ALPHA) + filtered_raw × RECALIB_ALPHA
 *   RECALIB_ALPHA = 0.001 por tick → τ ≈ 20s (mais conservador).
 *   Não recalibra durante toque ativo nem nas primeiras RECALIB_BOOT_MS.
 *
 * ── Baseline poisoning protection ─────────────────────────────────────────
 *   Recalibração só ocorre quando filtered_raw < threshold_off.
 *   Janela NOISE_WINDOW=12 amostras, spread máx. 2% do baseline.
 *
 * ── Boot stabilization ────────────────────────────────────────────────────
 *   Primeiros BOOT_STABLE_MS = 1000ms: leituras coletadas mas nenhum evento
 *   disparado. Evita falso toque durante energização do circuito.
 *
 * ── Máquina de estados ───────────────────────────────────────────────────
 *   IDLE
 *     └─(debounce_on OK: 2× filtered_raw ≥ thr_on)──► TOUCHING [TAP/WAKE]
 *
 *   TOUCHING
 *     ├─(debounce_off OK)──────► IDLE
 *     └─(press_ms ≥ 800)───────► LONG_PRESSING  [LONG_PRESS]
 *
 *   LONG_PRESSING
 *     ├─(debounce_off OK)──────► IDLE
 *     └─(press_ms ≥ 3000)──────► SUSTAINED_ACTIVE [SUSTAINED]
 *
 *   SUSTAINED_ACTIVE
 *     └─(debounce_off OK)──────► IDLE
 */

#include "touch_service.h"
#include "touch_hal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "nb_touch_svc";

/* ── Constantes ──────────────────────────────────────────────────────────── */

#define LONG_PRESS_MS         800U
#define SUSTAINED_MS         3000U

#define SENS_DEFAULT           0.20f
/*
 * SENS_MIN abaixado para 0.002f (0.2% do baseline).
 * O pad de cobre do hardware atual gera variação de ~1% no toque, muito abaixo
 * dos 20-50% esperados para um pad comercial. Com o inteiro percentual da NVS
 * (mínimo 1%) o threshold ficou alto demais. Quando o schema migrar para
 * resolução sub-percentual (float ou u16 em 0.1%), este floor pode subir.
 */
#define SENS_MIN               0.002f
#define SENS_MAX               1.00f
#define HYST_FACTOR            0.40f   /* threshold_off = span × HYST_FACTOR (mais gap) */

/*
 * DEBOUNCE_ON_COUNT = 2: exige 2 amostras consecutivas (40ms a 50Hz) acima de
 * threshold_on para confirmar toque. Filtra picos isolados de ruído sem adicionar
 * latência perceptível. Threshold de 0.8% já afasta a maioria do noise floor;
 * o debounce de 2 cobre os picos residuais que ainda cruzam o limiar.
 * Tradeoff: latência de TAP ≈ 40ms — imperceptível ao toque humano.
 */
#define DEBOUNCE_ON_COUNT      2U      /* amostras para confirmar toque (ver nota acima) */
#define DEBOUNCE_OFF_COUNT     2U      /* amostras consecutivas para confirmar release  */

#define SIGNAL_EMA_ALPHA       0.20f   /* suavização do sinal (α=0.2 → τ≈4 ticks)     */

#define BOOT_STABLE_MS       1000U     /* período de estabilização pós-boot             */
#define RECALIB_BOOT_MS      2000U     /* início da recalibração (ms pós-boot)          */
#define RECALIB_ALPHA          0.001f  /* EMA coefficient por tick (τ ≈ 20s, conservador) */
#define NOISE_WINDOW          12U      /* janela de amostras para ruído                 */
#define NOISE_SPREAD_PCT       2U      /* spread máx. aceito para recalib (%)           */
/*
 * RECALIB_STUCK_MS: se a FSM ficar em SUSTAINED_ACTIVE por mais de este tempo
 * com sinal estável (NOISE_SPREAD_PCT ok), assume que a baseline inicial foi
 * capturada muito cedo (drift físico do pad) e força re-baseline + volta a IDLE.
 */
#define RECALIB_STUCK_MS   10000U     /* 10s preso em SUSTAINED → re-baseline          */

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef struct {
    bool                    initialized;

    /* Máquina de estados */
    nb_touch_state_t        state;
    uint32_t                press_ms;         /* duração do toque atual        */

    /* Thresholds */
    float                   sensitivity;
    uint32_t                baseline;         /* baseline dinâmico             */
    uint32_t                threshold_on;
    uint32_t                threshold_off;

    /* Debounce de entrada e release */
    uint8_t                 debounce_on;      /* amostras consecutivas acima thr_on     */
    uint8_t                 debounce_off;     /* amostras consecutivas abaixo thr_off   */

    /* Filtragem de sinal */
    float                   filtered_raw;     /* EMA do raw (usado pela FSM)            */

    /* Boot stabilization */
    uint32_t                uptime_ms;        /* ms desde init                 */
    bool                    boot_stable;      /* pode disparar eventos         */

    /* Recalibração lenta */
    uint32_t                noise_history[NOISE_WINDOW];
    uint8_t                 noise_idx;
    bool                    noise_history_full;

    /* Misc */
    bool                    sleeping;
    nb_touch_event_cb_t     event_cb;
    bool                    pending_event;
    nb_touch_event_t        pending_evt;
    uint32_t                last_raw;

    /* Auto-recalibração de emergência (baseline drift pós-boot) */
    uint32_t                stuck_ms;   /* ms consecutivos em SUSTAINED_ACTIVE  */

    SemaphoreHandle_t       mutex;
} touch_svc_t;

static touch_svc_t s_svc = {0};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void recompute_thresholds(void)
{
    float span         = s_svc.baseline * s_svc.sensitivity;
    s_svc.threshold_on  = s_svc.baseline + (uint32_t)span;
    s_svc.threshold_off = s_svc.baseline + (uint32_t)(span * HYST_FACTOR);
}

static void fire_event(nb_touch_event_t evt)
{
    static const char *const k_evt_name[] = { "TAP", "LONG_PRESS", "SUSTAINED", "WAKE" };
    ESP_LOGI(TAG, "event=%s raw=%lu fraw=%lu baseline=%lu thr_on=%lu",
             k_evt_name[evt],
             (unsigned long)s_svc.last_raw,
             (unsigned long)s_svc.filtered_raw,
             (unsigned long)s_svc.baseline,
             (unsigned long)s_svc.threshold_on);
    if (s_svc.event_cb != NULL) {
        s_svc.event_cb(evt);
    }
}

static void queue_event(nb_touch_event_t evt)
{
    s_svc.pending_evt   = evt;
    s_svc.pending_event = true;
}

/*
 * Verifica se as últimas NOISE_WINDOW amostras indicam sinal estável.
 * Retorna true se spread < NOISE_SPREAD_PCT% do baseline.
 */
static bool is_signal_stable(void)
{
    if (!s_svc.noise_history_full) return false;

    uint32_t hi = 0, lo = UINT32_MAX;
    for (int i = 0; i < (int)NOISE_WINDOW; i++) {
        if (s_svc.noise_history[i] > hi) hi = s_svc.noise_history[i];
        if (s_svc.noise_history[i] < lo) lo = s_svc.noise_history[i];
    }
    uint32_t spread = hi - lo;
    uint32_t limit  = (s_svc.baseline * NOISE_SPREAD_PCT) / 100U;
    return (spread <= limit);
}

/* Adiciona amostra à janela de ruído. */
static void noise_push(uint32_t raw)
{
    s_svc.noise_history[s_svc.noise_idx] = raw;
    s_svc.noise_idx = (s_svc.noise_idx + 1U) % NOISE_WINDOW;
    if (s_svc.noise_idx == 0) s_svc.noise_history_full = true;
}

/* ── update interno (já dentro do mutex) ────────────────────────────────── */

static void service_tick(uint32_t dt_ms)
{
    s_svc.uptime_ms += dt_ms;

    /* Período de estabilização pós-boot. */
    if (!s_svc.boot_stable) {
        if (s_svc.uptime_ms >= BOOT_STABLE_MS) {
            s_svc.boot_stable = true;
            ESP_LOGI(TAG, "boot stable — deteccao ativa (thr_on=%lu thr_off=%lu)",
                     (unsigned long)s_svc.threshold_on,
                     (unsigned long)s_svc.threshold_off);
        }
        /* Coleta amostras mas não dispara eventos. */
        uint32_t raw = 0;
        nb_touch_hal_read(&raw);
        noise_push(raw);
        s_svc.last_raw = raw;
        return;
    }

    /* Leitura bruta. */
    uint32_t raw = 0;
    if (nb_touch_hal_read(&raw) != ESP_OK) return;
    s_svc.last_raw = raw;
    noise_push(raw);

    /* EMA de sinal — suaviza ruído antes da FSM. */
    if (s_svc.filtered_raw == 0.0f) {
        s_svc.filtered_raw = (float)raw;   /* inicializa na primeira amostra */
    } else {
        s_svc.filtered_raw = s_svc.filtered_raw * (1.0f - SIGNAL_EMA_ALPHA)
                           + (float)raw           * SIGNAL_EMA_ALPHA;
    }
    uint32_t fraw = (uint32_t)s_svc.filtered_raw;


    /* ── Recalibração lenta (usa fraw — mais estável que raw) ───────────── */
    if (s_svc.uptime_ms >= RECALIB_BOOT_MS
        && s_svc.state == NB_TOUCH_STATE_IDLE
        && fraw < s_svc.threshold_off
        && is_signal_stable()) {

        float new_baseline = s_svc.baseline * (1.0f - RECALIB_ALPHA)
                           + s_svc.filtered_raw   * RECALIB_ALPHA;
        uint32_t nb = (uint32_t)new_baseline;
        if (nb != s_svc.baseline) {
            s_svc.baseline = nb;
            recompute_thresholds();
        }
    }

    /* ── Máquina de estados (opera sobre fraw) ──────────────────────────── */

    bool above_on  = (fraw >= s_svc.threshold_on);
    bool below_off = (fraw <  s_svc.threshold_off);

    /* Debounce de entrada: acumula amostras acima de threshold_on. */
    if (above_on) {
        if (s_svc.debounce_on < DEBOUNCE_ON_COUNT) s_svc.debounce_on++;
    } else {
        s_svc.debounce_on = 0;
    }

    /* Debounce de release: acumula amostras abaixo de threshold_off. */
    if (below_off) {
        if (s_svc.debounce_off < DEBOUNCE_OFF_COUNT) s_svc.debounce_off++;
    } else {
        s_svc.debounce_off = 0;
    }

    bool confirmed_touch   = (s_svc.debounce_on  >= DEBOUNCE_ON_COUNT);
    bool confirmed_release = (s_svc.debounce_off >= DEBOUNCE_OFF_COUNT);

    switch (s_svc.state) {

    case NB_TOUCH_STATE_IDLE:
        if (confirmed_touch) {
            s_svc.state        = NB_TOUCH_STATE_TOUCHING;
            s_svc.press_ms     = 0;
            s_svc.debounce_off = 0;
            s_svc.debounce_on  = 0;
            /* TAP/WAKE: latência = DEBOUNCE_ON_COUNT × dt_ms (≈40ms). */
            queue_event(s_svc.sleeping ? NB_TOUCH_EVT_WAKE : NB_TOUCH_EVT_TAP);
        }
        break;

    case NB_TOUCH_STATE_TOUCHING:
        if (confirmed_release) {
            s_svc.state    = NB_TOUCH_STATE_IDLE;
            s_svc.press_ms = 0;
        } else {
            s_svc.press_ms += dt_ms;
            if (s_svc.press_ms >= LONG_PRESS_MS) {
                s_svc.state = NB_TOUCH_STATE_LONG_PRESSING;
                queue_event(NB_TOUCH_EVT_LONG_PRESS);
            }
        }
        break;

    case NB_TOUCH_STATE_LONG_PRESSING:
        if (confirmed_release) {
            s_svc.state    = NB_TOUCH_STATE_IDLE;
            s_svc.press_ms = 0;
        } else {
            s_svc.press_ms += dt_ms;
            if (s_svc.press_ms >= SUSTAINED_MS) {
                s_svc.state = NB_TOUCH_STATE_SUSTAINED_ACTIVE;
                queue_event(NB_TOUCH_EVT_SUSTAINED);
            }
        }
        break;

    case NB_TOUCH_STATE_SUSTAINED_ACTIVE:
        if (confirmed_release) {
            s_svc.state    = NB_TOUCH_STATE_IDLE;
            s_svc.press_ms = 0;
            s_svc.stuck_ms = 0;
        } else {
            s_svc.press_ms += dt_ms;
            s_svc.stuck_ms += dt_ms;
            /* Auto-recalibração: se preso em SUSTAINED por muito tempo com sinal
             * estável, é drift de baseline (não toque real) — força re-baseline. */
            if (s_svc.stuck_ms >= RECALIB_STUCK_MS && is_signal_stable()) {
                s_svc.baseline = (uint32_t)s_svc.filtered_raw;
                recompute_thresholds();
                s_svc.state    = NB_TOUCH_STATE_IDLE;
                s_svc.press_ms = 0;
                s_svc.stuck_ms = 0;
                s_svc.debounce_on  = 0;
                s_svc.debounce_off = 0;
                ESP_LOGW(TAG, "auto-recalib: baseline drift → novo base=%lu thr_on=%lu",
                         (unsigned long)s_svc.baseline,
                         (unsigned long)s_svc.threshold_on);
            }
        }
        break;
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t touch_service_init(void)
{
    if (s_svc.initialized) {
        ESP_LOGW(TAG, "touch_service_init chamado mais de uma vez");
        return ESP_OK;
    }

    s_svc.mutex = xSemaphoreCreateMutex();
    if (s_svc.mutex == NULL) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nb_touch_hal_init();
    if (err != ESP_OK) return err;

    s_svc.sensitivity       = SENS_DEFAULT;
    s_svc.baseline          = nb_touch_hal_get_baseline();
    s_svc.state             = NB_TOUCH_STATE_IDLE;
    s_svc.press_ms          = 0;
    s_svc.debounce_on       = 0;
    s_svc.debounce_off      = 0;
    s_svc.filtered_raw      = 0.0f;
    s_svc.uptime_ms         = 0;
    s_svc.boot_stable       = false;
    s_svc.noise_idx         = 0;
    s_svc.noise_history_full = false;
    s_svc.sleeping          = false;
    s_svc.event_cb          = NULL;
    s_svc.pending_event     = false;

    recompute_thresholds();

    s_svc.initialized = true;
    ESP_LOGI(TAG, "touch_service OK — baseline %lu, thr_on %lu, thr_off %lu, sens %.2f",
             (unsigned long)s_svc.baseline,
             (unsigned long)s_svc.threshold_on,
             (unsigned long)s_svc.threshold_off,
             (double)s_svc.sensitivity);
    return ESP_OK;
}

void touch_service_update(uint32_t dt_ms)
{
    if (!s_svc.initialized) return;

    bool has_event = false;
    nb_touch_event_t evt = NB_TOUCH_EVT_TAP;

    if (xSemaphoreTake(s_svc.mutex, 0) != pdTRUE) return;
    service_tick(dt_ms);
    if (s_svc.pending_event) {
        evt = s_svc.pending_evt;
        s_svc.pending_event = false;
        has_event = true;
    }
    xSemaphoreGive(s_svc.mutex);

    if (has_event) {
        fire_event(evt);
    }
}

void touch_service_set_event_cb(nb_touch_event_cb_t cb)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_svc.event_cb = cb;
    xSemaphoreGive(s_svc.mutex);
}

void touch_service_set_sensitivity(float factor)
{
    if (factor < SENS_MIN) factor = SENS_MIN;
    if (factor > SENS_MAX) factor = SENS_MAX;

    if (!s_svc.initialized) {
        s_svc.sensitivity = factor;
        return;
    }
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_svc.sensitivity = factor;
    recompute_thresholds();
    ESP_LOGI(TAG, "sensitivity %.2f → thr_on %lu, thr_off %lu",
             (double)factor,
             (unsigned long)s_svc.threshold_on,
             (unsigned long)s_svc.threshold_off);
    xSemaphoreGive(s_svc.mutex);
}

void touch_service_set_sleeping(bool sleeping)
{
    if (!s_svc.initialized) return;
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    s_svc.sleeping = sleeping;
    xSemaphoreGive(s_svc.mutex);
}

bool touch_service_is_pressed(void)
{
    if (!s_svc.initialized) return false;
    return (s_svc.state != NB_TOUCH_STATE_IDLE);
}

uint32_t touch_service_get_duration_ms(void)
{
    if (!s_svc.initialized) return 0;
    return s_svc.press_ms;
}

void touch_service_get_debug(nb_touch_debug_t *debug_out)
{
    if (debug_out == NULL) return;
    if (!s_svc.initialized) {
        memset(debug_out, 0, sizeof(*debug_out));
        return;
    }
    if (xSemaphoreTake(s_svc.mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    debug_out->raw               = s_svc.last_raw;
    debug_out->filtered_raw      = (uint32_t)s_svc.filtered_raw;
    debug_out->baseline          = s_svc.baseline;
    debug_out->threshold_on      = s_svc.threshold_on;
    debug_out->threshold_off     = s_svc.threshold_off;
    debug_out->state             = s_svc.state;
    debug_out->touch_duration_ms = s_svc.press_ms;
    xSemaphoreGive(s_svc.mutex);
}
