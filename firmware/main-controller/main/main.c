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
#include "nb_hw_config_main.h"
#include "nb_main_link_service.h"

#ifndef CONFIG_NB_DM1_BENCH_PROFILE
#define CONFIG_NB_DM1_BENCH_PROFILE 0
#endif

#ifndef CONFIG_NB_DM1_HEAD_RESET_PROBE
#define CONFIG_NB_DM1_HEAD_RESET_PROBE 0
#endif

#ifndef CONFIG_NB_DM2_DISPLAY_PROBE
#define CONFIG_NB_DM2_DISPLAY_PROBE 0
#endif

#define NB_E6_READY_TIMEOUT_MS 10000U
#define NB_E6_READY_POLL_MS 20U
#define NB_E6_ARM_DELAY_MS 5000U

static const char *TAG = "nb_main";

void app_main(void)
{
    if (CONFIG_NB_DM1_BENCH_PROFILE) {
        ESP_LOGW(TAG,
                 "DM1 bench profile ativo — boot monolitico legado ignorado");
        const esp_err_t link_err = nb_main_link_service_init();
        if (link_err != ESP_OK) {
            ESP_LOGE(TAG, "enlace DM1 falhou: %s",
                     esp_err_to_name(link_err));
        }
        if (link_err == ESP_OK && CONFIG_NB_DM1_HEAD_RESET_PROBE) {
            uint32_t waited_ms = 0U;
            while (nb_main_link_service_state() != NB_LINK_STATE_READY &&
                   waited_ms < NB_E6_READY_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(NB_E6_READY_POLL_MS));
                waited_ms += NB_E6_READY_POLL_MS;
            }
            if (nb_main_link_service_state() != NB_LINK_STATE_READY) {
                ESP_LOGE(TAG, "E6 probe abortado: enlace nao chegou a READY");
            } else {
                ESP_LOGI(TAG, "E6 probe armado; HEAD_RESET em %u ms",
                         (unsigned)NB_E6_ARM_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(NB_E6_ARM_DELAY_MS));
                const esp_err_t first = nb_main_link_service_reset_head();
                ESP_LOGI(TAG, "E6 primeiro HEAD_RESET: %s",
                         esp_err_to_name(first));
                const esp_err_t second = nb_main_link_service_reset_head();
                ESP_LOGI(TAG, "E6 segundo HEAD_RESET imediato: %s",
                         esp_err_to_name(second));
            }
        }
        if (link_err == ESP_OK && CONFIG_NB_DM2_DISPLAY_PROBE) {
            uint32_t waited_ms = 0U;
            while (nb_main_link_service_state() != NB_LINK_STATE_READY &&
                   waited_ms < NB_E6_READY_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(NB_E6_READY_POLL_MS));
                waited_ms += NB_E6_READY_POLL_MS;
            }
            const nb_display_command_t command = {
                .version = NB_DISPLAY_COMMAND_VERSION,
                .opcode = NB_DISPLAY_OP_SET_SCENE,
                .expression = 2U,
                .brightness = 180U,
                .gaze_x_milli = -250,
                .gaze_y_milli = 400,
                .overlay_flags = 0x0003U,
                .reserved = 0U,
                .generation = 1U,
            };
            const esp_err_t probe_err =
                nb_main_link_service_queue_display(&command);
            ESP_LOGI(TAG,
                     "DM2 display probe generation=%lu result=%s",
                     (unsigned long)command.generation,
                     esp_err_to_name(probe_err));
        }
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

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
