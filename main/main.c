/*
 * main.c — Ponto de entrada do NodeBot
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
    /*
     * boot_manager_run() executa todas as fases de inicialização em ordem.
     * Quando todas as tasks FreeRTOS estiverem rodando (Blocos 1-5),
     * esta função retornará e app_main poderá ser deletada.
     *
     * Por enquanto (Etapa 0.1), retorna imediatamente após o boot stub
     * e entramos no loop de manutenção abaixo.
     */
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
     *
     * Enquanto isso: loop com feed de watchdog e yield para as outras tasks.
     */
    ESP_LOGI(TAG, "app_main entrando em loop de manutencao");

    while (1) {
        nb_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
