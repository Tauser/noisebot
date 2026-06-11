/*
 * diagnostics_service.c — Observabilidade do sistema NoiseBot (Layer 2)
 *
 * Coleta métricas de heap, FPS e stack watermark por task. Calcula um
 * health_score composto e publica eventos quando o sistema degrada.
 *
 * Health score (começa em 100, decrementos cumulativos):
 *   PSRAM < 300KB  → -30 pts
 *   PSRAM < 150KB  → -20 pts adicionais
 *   SRAM  < 50KB   → -10 pts
 *   SRAM  < 30KB   → -10 pts adicionais
 *   FPS   < 25     → -20 pts  (ignorado se FPS = 0, serviço não iniciado)
 *   FPS   < 20     → -10 pts adicionais
 *   Stack watermark < 200 words em qualquer task → -10 pts (máx -20)
 *
 * Eventos publicados (nb_events.h):
 *   NB_EVT_HEALTH_WARNING  — score caiu abaixo de NB_DIAG_HEALTH_WARN_THRESHOLD
 *   NB_EVT_HEAP_LOW        — PSRAM livre abaixo de NB_DIAG_HEAP_LOW_PSRAM_KB
 *
 * Ambos os eventos são publicados apenas na transição (não repetem a cada coleta).
 */

#include "diagnostics_service.h"
#include "event_bus.h"
#include "nb_events.h"
#include "logger.h"
#include "servo_hal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#define TAG "nb_diag"

/* ── Tasks monitoradas ───────────────────────────────────────────────────── */

static const char *const k_task_names[] = {
    "nb_render_task",   /* render_service.cpp  */
    "nb_push_task",     /* render_service.cpp  */
    "audio_task",       /* audio_service.c     */
    "motion_task",      /* motion_service.c    */
    "safety_task",      /* motion_safety.c     */
    "conductor_task",   /* conductor.c         */
    "behav_task",       /* boot_manager.c      */
    "led_task",         /* boot_manager.c      */
    "touch_task",       /* boot_manager.c      */
    "persist_task",     /* persistence_mgr.c   */
    "wdog_task",        /* watchdog_service.c  */
};

#define K_NTASKS  ((int)(sizeof(k_task_names) / sizeof(k_task_names[0])))

/* ── Estado interno ──────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    uint32_t    watermark_words;
} nb_diag_task_t;

static struct {
    bool                     initialized;
    nb_diag_fps_provider_t   get_fps;

    /* snapshot da última coleta — protegido por s_mux */
    float                    fps;
    uint32_t                 psram_free_kb;
    uint32_t                 sram_free_kb;
    uint8_t                  health_score;
    nb_diag_task_t           tasks[K_NTASKS];

    /* flags de transição para evitar publicar eventos repetidos */
    bool                     health_warn_active;
    bool                     heap_low_active;
} s_state;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint8_t compute_health_score(float fps,
                                    uint32_t psram_kb,
                                    uint32_t sram_kb,
                                    const nb_diag_task_t *tasks,
                                    int ntasks)
{
    int score = 100;

    if (psram_kb < NB_DIAG_HEAP_LOW_PSRAM_KB) {
        score -= 30;
        if (psram_kb < 150U) {
            score -= 20;
        }
    }

    if (sram_kb < 50U) {
        score -= 10;
        if (sram_kb < 30U) {
            score -= 10;
        }
    }

    /* FPS de 0 significa que render ainda não iniciou — não penalizar */
    if (fps > 0.1f) {
        if (fps < 25.0f) {
            score -= 20;
            if (fps < 20.0f) {
                score -= 10;
            }
        }
    }

    int stack_penalties = 0;
    for (int i = 0; i < ntasks && stack_penalties < 20; i++) {
        if (tasks[i].watermark_words > 0U &&
            tasks[i].watermark_words < 200U) {
            score        -= 10;
            stack_penalties += 10;
        }
    }

    if (score < 0) score = 0;
    return (uint8_t)score;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t diagnostics_init(const nb_diagnostics_config_t *cfg)
{
    if (cfg == NULL)             return ESP_ERR_INVALID_ARG;
    if (s_state.initialized)     return ESP_ERR_INVALID_STATE;

    memset(&s_state, 0, sizeof(s_state));
    s_state.get_fps      = cfg->get_fps;
    s_state.health_score = 100U;
    s_state.initialized  = true;

    /* Preenche nomes das tasks no snapshot inicial */
    for (int i = 0; i < K_NTASKS; i++) {
        s_state.tasks[i].name            = k_task_names[i];
        s_state.tasks[i].watermark_words = 0U;
    }

    NB_LOGI(TAG, "diagnostics_service inicializado");
    return ESP_OK;
}

void diagnostics_collect(void)
{
    if (!s_state.initialized) return;

    /* ── Coleta de heap ── */
    uint32_t psram_kb = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024U;
    uint32_t sram_kb  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U;
    float    fps      = (s_state.get_fps != NULL) ? s_state.get_fps() : 0.0f;

    /* ── Coleta de stack watermarks ── */
    nb_diag_task_t tasks[K_NTASKS];
    for (int i = 0; i < K_NTASKS; i++) {
        tasks[i].name = k_task_names[i];
        TaskHandle_t h = xTaskGetHandle(k_task_names[i]);
        tasks[i].watermark_words = h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0U;
    }

    /* ── Calcula health score ── */
    uint8_t score = compute_health_score(fps, psram_kb, sram_kb, tasks, K_NTASKS);

    /* ── Log de diagnóstico detalhado: útil em debug, ruidoso no monitor normal. */
    NB_LOGD(TAG, "--- DIAG ---  PSRAM=%luKB  SRAM=%luKB  FPS=%.1f  health=%u/100",
            (unsigned long)psram_kb, (unsigned long)sram_kb, fps, (unsigned)score);

    for (int i = 0; i < K_NTASKS; i++) {
        if (tasks[i].watermark_words == 0U) continue;
        NB_LOGD(TAG, "  %-24s watermark=%4lu bytes",
                tasks[i].name,
                (unsigned long)(tasks[i].watermark_words * sizeof(StackType_t)));
    }

    /* ── Atualiza estado protegido ── */
    taskENTER_CRITICAL(&s_mux);
    s_state.fps          = fps;
    s_state.psram_free_kb = psram_kb;
    s_state.sram_free_kb  = sram_kb;
    s_state.health_score  = score;
    for (int i = 0; i < K_NTASKS; i++) {
        s_state.tasks[i].watermark_words = tasks[i].watermark_words;
    }
    taskEXIT_CRITICAL(&s_mux);

    /* ── Publica eventos em transição ── */
    if (score < NB_DIAG_HEALTH_WARN_THRESHOLD && !s_state.health_warn_active) {
        s_state.health_warn_active = true;
        nb_event_t evt = { .type = NB_EVT_HEALTH_WARNING, .data.u32 = score };
        nb_event_publish(&evt);
        NB_LOGW(TAG, "HEALTH WARNING: score=%u", (unsigned)score);
    } else if (score >= NB_DIAG_HEALTH_WARN_THRESHOLD) {
        s_state.health_warn_active = false;
    }

    if (psram_kb < NB_DIAG_HEAP_LOW_PSRAM_KB && !s_state.heap_low_active) {
        s_state.heap_low_active = true;
        nb_event_t evt = { .type = NB_EVT_HEAP_LOW, .data.u32 = psram_kb };
        nb_event_publish(&evt);
        NB_LOGW(TAG, "HEAP LOW: PSRAM=%luKB (threshold=%uKB)",
                (unsigned long)psram_kb, NB_DIAG_HEAP_LOW_PSRAM_KB);
    } else if (psram_kb >= NB_DIAG_HEAP_LOW_PSRAM_KB) {
        s_state.heap_low_active = false;
    }
}

void diagnostics_dump_to_sd(void)
{
    if (!s_state.initialized) return;

    uint32_t uptime_s = diagnostics_get_uptime_s();

    char path[64];
    snprintf(path, sizeof(path), "/sdcard/logs/diag_%lus.txt", (unsigned long)uptime_s);

    FILE *f = fopen(path, "w");
    if (!f) {
        NB_LOGD(TAG, "dump_to_sd: fopen falhou (SD indisponível)");
        return;
    }

    taskENTER_CRITICAL(&s_mux);
    uint32_t psram_kb    = s_state.psram_free_kb;
    uint32_t sram_kb     = s_state.sram_free_kb;
    float    fps         = s_state.fps;
    uint8_t  score       = s_state.health_score;
    nb_diag_task_t tasks[K_NTASKS];
    for (int i = 0; i < K_NTASKS; i++) tasks[i] = s_state.tasks[i];
    taskEXIT_CRITICAL(&s_mux);

    servo_hal_bus_stats_t bus_stats = {0};
    servo_hal_get_bus_stats(&bus_stats);

    fprintf(f, "NoiseBot Diagnostics — uptime=%lus\n", (unsigned long)uptime_s);
    fprintf(f, "  health_score : %u/100\n", (unsigned)score);
    fprintf(f, "  PSRAM free   : %lu KB\n", (unsigned long)psram_kb);
    fprintf(f, "  SRAM  free   : %lu KB\n", (unsigned long)sram_kb);
    fprintf(f, "  FPS          : %.1f\n",   fps);
    fprintf(f, "\nServo bus stats (EMI):\n");
    fprintf(f, "  rx_timeouts  : %lu\n", (unsigned long)bus_stats.timeouts);
    fprintf(f, "  rx_errors    : %lu\n", (unsigned long)bus_stats.errors);
    fprintf(f, "\nTask stack watermarks:\n");
    for (int i = 0; i < K_NTASKS; i++) {
        if (tasks[i].watermark_words == 0U) continue;
        fprintf(f, "  %-24s %4lu bytes\n",
                tasks[i].name,
                (unsigned long)(tasks[i].watermark_words * sizeof(StackType_t)));
    }

    fclose(f);
    NB_LOGI(TAG, "dump gravado em %s", path);
}

/* ── Getters (thread-safe) ───────────────────────────────────────────────── */

uint8_t diagnostics_get_health_score(void)
{
    uint8_t v;
    taskENTER_CRITICAL(&s_mux);
    v = s_state.health_score;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

float diagnostics_get_fps(void)
{
    float v;
    taskENTER_CRITICAL(&s_mux);
    v = s_state.fps;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

uint32_t diagnostics_get_heap_free(uint32_t caps)
{
    return (uint32_t)heap_caps_get_free_size((uint32_t)caps);
}

uint32_t diagnostics_get_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}
