/*
 * main.c — Ponto de entrada do NoiseBot
 *
 * Responsabilidade única: disparar boot_manager_run() e manter o loop
 * principal após o boot. Nenhuma lógica de negócio aqui.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "boot_manager.h"
#include "watchdog_service.h"

static const char *TAG = "nb_main";

void app_main(void)
{
    esp_err_t err = boot_manager_run();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot_manager_run retornou erro: %s", esp_err_to_name(err));
        /* Nunca deveria acontecer — falhas fatais fazem esp_restart() internamente. */
    }

    /* ── Loop de manutenção de app_main ─────────────────────────────────── */
    /*
     * app_main permanece viva e alimenta o TWDT.
     * Quando os serviços do Bloco 5 estiverem rodando, esta task pode ser
     * deletada com vTaskDelete(NULL) pois as outras tasks sustentam o sistema.
     */
    ESP_LOGI(TAG, "app_main entrando em loop de manutencao");

    bool feed_warning_logged = false;
    while (1) {
        err = nb_watchdog_feed();
        if (err != ESP_OK && !feed_warning_logged) {
            ESP_LOGW(TAG, "feed do watchdog falhou: %s", esp_err_to_name(err));
            feed_warning_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
