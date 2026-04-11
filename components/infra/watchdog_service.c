/*
 * watchdog_service.c — Implementação do serviço de watchdog
 */

#include "watchdog_service.h"
#include "logger.h"
#include "error_policy.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "nb_wdog"

#define NB_WDOG_TASK_STACK_BYTES  2048
#define NB_WDOG_TASK_PRIORITY     24
#define NB_WDOG_FEED_INTERVAL_MS  1000   /* alimenta TWDT a cada 1s */

static TaskHandle_t s_wdog_task_handle = NULL;
static bool         s_initialized      = false;

/* ── Task interna ─────────────────────────────────────────────────────────── */

static void wdog_task(void *arg)
{
    (void)arg;

    /* Registra esta task no TWDT. */
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        NB_LOGE(TAG, "Falha ao registrar nb_wdog_task no TWDT: %s",
                esp_err_to_name(err));
        /* Task crítica — não pode falhar silenciosamente. */
        esp_restart();
    }

    NB_LOGD(TAG, "nb_wdog_task rodando (Core %d, prio %d)",
            xPortGetCoreID(), NB_WDOG_TASK_PRIORITY);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(NB_WDOG_FEED_INTERVAL_MS));
    }

    /* Nunca alcançado. */
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t nb_watchdog_init(void)
{
    if (s_initialized) {
        NB_LOGW(TAG, "nb_watchdog_init chamado mais de uma vez — ignorado");
        return ESP_OK;
    }

    /*
     * O TWDT é inicializado pelo ESP-IDF via sdkconfig (CONFIG_ESP_TASK_WDT_INIT=y,
     * CONFIG_ESP_TASK_WDT_TIMEOUT_S=5, CONFIG_ESP_TASK_WDT_PANIC=y).
     * Não fazemos reinit aqui — confiamos na config do sdkconfig.
     *
     * Se futuramente precisarmos de timeout diferente no boot vs. runtime,
     * usar esp_task_wdt_reconfigure() após PHASE_COMPLETE.
     */

    BaseType_t created = xTaskCreatePinnedToCore(
        wdog_task,
        "nb_wdog_task",
        NB_WDOG_TASK_STACK_BYTES,
        NULL,
        NB_WDOG_TASK_PRIORITY,
        &s_wdog_task_handle,
        0  /* Core 0 */
    );

    if (created != pdPASS) {
        NB_LOGE(TAG, "Falha ao criar nb_wdog_task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    NB_LOGI(TAG, "Watchdog service inicializado");
    return ESP_OK;
}

esp_err_t nb_watchdog_add_task(TaskHandle_t task)
{
    esp_err_t err = esp_task_wdt_add(task);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "Falha ao adicionar task ao TWDT: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nb_watchdog_feed(void)
{
    return esp_task_wdt_reset();
}

esp_err_t nb_watchdog_remove_task(TaskHandle_t task)
{
    esp_err_t err = esp_task_wdt_delete(task);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "Falha ao remover task do TWDT: %s", esp_err_to_name(err));
    }
    return err;
}
