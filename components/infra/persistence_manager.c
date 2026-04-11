#include "persistence_manager.h"
#include "event_bus.h"
#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "persistence";

static bool s_sd_available = false;
static bool s_sd_healthy   = false;

/* TODO (Etapa 1.3b): implementar montagem real do microSD via esp_vfs_fat_sdmmc */

static void prv_health_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!s_sd_available) continue;

        /* Verificar saude: tentar ler health file */
        FILE *f = fopen(NB_SD_HEALTH_FILE, "r");
        bool ok = (f != NULL);
        if (f) fclose(f);

        if (ok != s_sd_healthy) {
            s_sd_healthy = ok;
            if (!ok) {
                NB_LOGW(TAG, "microSD health check falhou — modo amnesico");
                EVENT_BUS_PUBLISH_SIMPLE(EVT_STORAGE_DEGRADED, TAG);
            } else {
                NB_LOGI(TAG, "microSD recuperado");
                EVENT_BUS_PUBLISH_SIMPLE(EVT_STORAGE_RECOVERED, TAG);
            }
        }
    }
}

esp_err_t persistence_init(void) {
    /* TODO (Etapa 1.3b): montar microSD, criar /noisebot/ e subdiretorios */
    NB_LOGW(TAG, "microSD pendente (Etapa 1.3b) — modo amnesico");
    s_sd_available = false;
    s_sd_healthy   = false;
    return ESP_OK;
}

esp_err_t persistence_start(void) {
    xTaskCreatePinnedToCore(prv_health_task, "nb_persist", 3072, NULL, 2, NULL, 1);
    return ESP_OK;
}

bool persistence_sd_available(void) { return s_sd_available; }
bool persistence_sd_healthy(void)   { return s_sd_healthy; }

/* Todas as operacoes abaixo retornam NOT_SUPPORTED ate Etapa 1.3b */

esp_err_t persistence_write_snapshot(const nb_system_snapshot_t *snap) {
    if (!s_sd_available) return ESP_ERR_NOT_SUPPORTED;
    /* TODO (Etapa 1.3b): serializar para JSON e escrever atomicamente */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_load_last_snapshot(nb_system_snapshot_t *snap) {
    if (!s_sd_available || !snap) return ESP_ERR_NOT_SUPPORTED;
    memset(snap, 0, sizeof(*snap));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_record_episode(const nb_episode_t *ep) {
    if (!s_sd_available) return ESP_ERR_NOT_SUPPORTED;
    /* TODO (Etapa 7.1): append JSONL em /noisebot/memory/episodic/YYYY-MM.jsonl */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_rotate_episodes(uint32_t max_age_days) {
    (void)max_age_days;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_load_preferences(nb_preferences_t *prefs) {
    if (!prefs) return ESP_ERR_INVALID_ARG;
    memset(prefs, 0, sizeof(*prefs));
    prefs->voice_sensitivity = 65;
    strncpy(prefs->idle_behavior, "calm", sizeof(prefs->idle_behavior));
    return ESP_OK; /* defaults sempre disponiveis */
}

esp_err_t persistence_save_preferences(const nb_preferences_t *prefs) {
    if (!s_sd_available) return ESP_ERR_NOT_SUPPORTED;
    (void)prefs;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_load_context(nb_context_t *ctx) {
    if (!ctx) return ESP_ERR_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    return ESP_OK;
}

esp_err_t persistence_save_context(const nb_context_t *ctx) {
    if (!s_sd_available) return ESP_ERR_NOT_SUPPORTED;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t persistence_load_persona_traits(nb_persona_traits_t *traits) {
    if (!traits) return ESP_ERR_INVALID_ARG;
    traits->energy_level           = 0.7f;
    traits->curiosity              = 0.8f;
    traits->expressiveness         = 0.65f;
    traits->response_latency_bias  = 0.4f;
    traits->calibrated_interactions = 0;
    traits->schema_version         = NB_SCHEMA_PERSONA;
    return ESP_OK;
}

esp_err_t persistence_save_persona_traits(const nb_persona_traits_t *traits) {
    if (!s_sd_available) return ESP_ERR_NOT_SUPPORTED;
    (void)traits;
    return ESP_ERR_NOT_SUPPORTED;
}
