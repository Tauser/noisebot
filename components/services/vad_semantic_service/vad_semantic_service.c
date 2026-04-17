/*
 * vad_semantic_service.c — VAD semântico (Layer 5)
 *
 * Fluxo por sessão de voz:
 *   VOICE_START  → inicia cronômetro + amostragem de RMS
 *   VOICE_END    → calcula duração → SHORT ou LONG
 *                  calcula RMS médio → LOUD ou SOFT
 *                  inicia followup timer (8-12s)
 *   Tick         → decrementa followup timer; conta SHORTs em 30s
 *
 * Thread safety:
 *   Flags de start/end são escritas pelo dispatcher task e lidas pelo
 *   behavior_task. portMUX garante leitura atômica dos flags.
 */

#include "vad_semantic_service.h"

#include "event_bus.h"
#include "nb_events.h"
#include "sound_analysis_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_random.h"
#include "esp_log.h"

#define TAG "nb_vadsem"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

#define VOICE_SHORT_MS       500U    /* < 500ms → interjeição                  */
#define VOICE_LONG_MS        4000U   /* > 4s → discurso                        */
#define LOUD_RMS_THRESHOLD   0.55f   /* RMS médio → LOUD                       */
#define SOFT_RMS_THRESHOLD   0.12f   /* RMS médio → SOFT                       */
#define MIN_RMS_SAMPLES      3U      /* mínimo de amostras para classificar     */
#define FOLLOWUP_MIN_MS      8000U   /* início da janela pós-voz               */
#define FOLLOWUP_RANGE_MS    4000U   /* variação adicional (8-12s)             */
#define REPEAT_WINDOW_MS     30000U  /* janela de repetição em ms              */
#define REPEAT_COUNT         3U      /* nº de SHORTs para REPEATED             */

/* ── Estado interno ──────────────────────────────────────────────────────── */

/* Flags dispatcher → tick (protegidas por s_mux) */
static volatile bool s_start_flag;
static volatile bool s_end_flag;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Sessão de voz atual */
static bool     s_voice_active;
static uint32_t s_voice_ms;        /* duração da sessão corrente */
static float    s_rms_acc;         /* acumulador de RMS */
static uint32_t s_rms_cnt;         /* amostras de RMS coletadas */

/* Followup timer */
static uint32_t s_followup_ms;     /* contagem regressiva; 0 = inativo */

/* Uptime para janela de repetição */
static uint32_t s_uptime_ms;

/* Ring buffer de timestamps de VOICE_SHORT (em uptime_ms) */
static uint32_t s_short_ts[REPEAT_COUNT];
static uint8_t  s_short_head;

static bool s_initialized;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void publish_simple(nb_event_type_t type)
{
    nb_event_t ev = { .type = type };
    nb_event_publish(&ev);
}

/* Inicia followup timer com jitter 8-12s */
static void start_followup(void)
{
    uint32_t jitter = (uint32_t)((float)FOLLOWUP_RANGE_MS *
                      ((float)(esp_random() & 0xFFFFU) / 65535.0f));
    s_followup_ms = FOLLOWUP_MIN_MS + jitter;
    ESP_LOGD(TAG, "followup timer: %lums", (unsigned long)s_followup_ms);
}

/* ── Event handlers (dispatcher task) ───────────────────────────────────── */

static void on_voice_start(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    taskENTER_CRITICAL(&s_mux);
    s_start_flag = true;
    taskEXIT_CRITICAL(&s_mux);
}

static void on_voice_end(const nb_event_t *ev, void *ctx)
{
    (void)ev; (void)ctx;
    taskENTER_CRITICAL(&s_mux);
    s_end_flag = true;
    taskEXIT_CRITICAL(&s_mux);
}

/* ── Lógica de fim de sessão (chamada do tick) ───────────────────────────── */

static void handle_voice_end(void)
{
    /* Duração */
    if (s_voice_ms < VOICE_SHORT_MS) {
        ESP_LOGD(TAG, "VOICE_SHORT (%lums)", (unsigned long)s_voice_ms);
        publish_simple(NB_EVT_VOICE_SHORT);

        /* Repeat detection */
        s_short_ts[s_short_head] = s_uptime_ms;
        s_short_head = (s_short_head + 1) % REPEAT_COUNT;

        /* Conta quantos estão dentro da janela */
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < REPEAT_COUNT; i++) {
            if (s_uptime_ms - s_short_ts[i] <= REPEAT_WINDOW_MS) cnt++;
        }
        if (cnt >= REPEAT_COUNT) {
            ESP_LOGI(TAG, "VOICE_REPEATED (%u em %us)", cnt, REPEAT_WINDOW_MS / 1000);
            publish_simple(NB_EVT_VOICE_REPEATED);
            /* Reseta timestamps para não disparar repetidamente */
            for (uint8_t i = 0; i < REPEAT_COUNT; i++) s_short_ts[i] = 0;
        }

    } else if (s_voice_ms >= VOICE_LONG_MS) {
        ESP_LOGD(TAG, "VOICE_LONG (%lums)", (unsigned long)s_voice_ms);
        publish_simple(NB_EVT_VOICE_LONG);
    }

    /* Energia */
    if (s_rms_cnt >= MIN_RMS_SAMPLES) {
        float avg = s_rms_acc / (float)s_rms_cnt;
        if (avg >= LOUD_RMS_THRESHOLD) {
            ESP_LOGD(TAG, "VOICE_LOUD (rms=%.2f)", avg);
            publish_simple(NB_EVT_VOICE_LOUD);
        } else if (avg <= SOFT_RMS_THRESHOLD) {
            ESP_LOGD(TAG, "VOICE_SOFT (rms=%.2f)", avg);
            publish_simple(NB_EVT_VOICE_SOFT);
        }
    }

    /* Inicia followup timer */
    start_followup();
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t vad_semantic_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    nb_event_subscribe(NB_EVT_VOICE_ACTIVITY_START, on_voice_start, NULL, NULL);
    nb_event_subscribe(NB_EVT_VOICE_ACTIVITY_END,   on_voice_end,   NULL, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "vad_semantic_service inicializado");
    return ESP_OK;
}

void vad_semantic_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;

    s_uptime_ms += dt_ms;

    /* Consumir flags do dispatcher */
    bool start = false, end = false;
    taskENTER_CRITICAL(&s_mux);
    start        = s_start_flag; s_start_flag = false;
    end          = s_end_flag;   s_end_flag   = false;
    taskEXIT_CRITICAL(&s_mux);

    if (start) {
        s_voice_active = true;
        s_voice_ms     = 0;
        s_rms_acc      = 0.0f;
        s_rms_cnt      = 0;
        s_followup_ms  = 0;  /* nova voz cancela followup */
        ESP_LOGD(TAG, "voice session start");
    }

    if (s_voice_active) {
        s_voice_ms += dt_ms;
        /* Amostra RMS somente quando classe é VOICE */
        if (sound_analysis_get_class() == NB_SOUND_VOICE) {
            s_rms_acc += sound_analysis_get_rms();
            s_rms_cnt++;
        }
    }

    if (end && s_voice_active) {
        s_voice_active = false;
        handle_voice_end();
        s_voice_ms = 0;
    }

    /* Followup timer */
    if (s_followup_ms > 0) {
        if (s_followup_ms <= dt_ms) {
            s_followup_ms = 0;
            ESP_LOGI(TAG, "VOICE_FOLLOWUP_TIMEOUT");
            publish_simple(NB_EVT_VOICE_FOLLOWUP_TIMEOUT);
        } else {
            s_followup_ms -= dt_ms;
        }
    }
}
