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
#include "nb_camera_protocol.h"
#include "nb_hw_config_main.h"
#include "nb_main_link_service.h"
#include "visual_state_facade.h"

#ifndef CONFIG_NB_DM1_BENCH_PROFILE
#define CONFIG_NB_DM1_BENCH_PROFILE 0
#endif

#ifndef CONFIG_NB_DM1_HEAD_RESET_PROBE
#define CONFIG_NB_DM1_HEAD_RESET_PROBE 0
#endif

#ifndef CONFIG_NB_DM2_DISPLAY_PROBE
#define CONFIG_NB_DM2_DISPLAY_PROBE 0
#endif

#ifndef CONFIG_NB_DM4_CAMERA_PROBE
#define CONFIG_NB_DM4_CAMERA_PROBE 0
#endif

#define NB_DM4_CAMERA_PROBE_REQUESTS 5U
#define NB_DM4_CAMERA_PROBE_WAIT_MS 50U
#define NB_DM4_CAMERA_PROBE_TIMEOUT_MS 2000U

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
        if (link_err == ESP_OK && CONFIG_NB_DM2_DISPLAY_PROBE) {
            const esp_err_t facade_err =
                visual_state_facade_init(nb_main_link_service_queue_display);
            if (facade_err != ESP_OK) {
                ESP_LOGE(TAG, "DM2 facade init falhou: %s",
                         esp_err_to_name(facade_err));
            }
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
            visual_state_facade_set_brightness(180U);
            visual_state_facade_set_expression(2U);
            visual_state_facade_set_gaze(-0.25f, 0.4f);
            visual_state_facade_set_overlay(
                NB_DISPLAY_OVERLAY_LISTENING, true);
            ESP_LOGI(TAG, "DM2 facade probe iniciado");
            for (uint32_t frame = 1U; frame <= 80U; ++frame) {
                visual_state_facade_set_expression(
                    (uint8_t)((frame / 8U) %
                              NB_DISPLAY_EXPRESSION_COUNT));
                visual_state_facade_set_gaze(
                    ((float)(frame % 20U) * 0.1f) - 0.95f,
                    ((frame / 20U) & 1U) ? 0.35f : -0.35f);
                visual_state_facade_set_overlay(
                    NB_DISPLAY_OVERLAY_LISTENING, frame < 20U);
                visual_state_facade_set_overlay(
                    NB_DISPLAY_OVERLAY_SPEAKING,
                    frame >= 20U && frame < 40U);
                visual_state_facade_set_overlay(
                    NB_DISPLAY_OVERLAY_HEART,
                    frame >= 40U && frame < 60U);
                visual_state_facade_set_overlay(
                    NB_DISPLAY_OVERLAY_SLEEPING, frame >= 60U);
                vTaskDelay(pdMS_TO_TICKS(125));
            }
            ESP_LOGI(TAG, "DM2 remote EMO probe concluido");
        }
        if (link_err == ESP_OK && CONFIG_NB_DM4_CAMERA_PROBE) {
            uint32_t waited_ms = 0U;
            while (nb_main_link_service_state() != NB_LINK_STATE_READY &&
                   waited_ms < NB_E6_READY_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(NB_E6_READY_POLL_MS));
                waited_ms += NB_E6_READY_POLL_MS;
            }
            if (nb_main_link_service_state() != NB_LINK_STATE_READY) {
                ESP_LOGE(TAG, "DM4 camera probe abortado: enlace nao chegou a READY");
            } else {
                ESP_LOGI(TAG, "DM4 camera probe iniciado");
                for (uint32_t i = 0U; i < NB_DM4_CAMERA_PROBE_REQUESTS; ++i) {
                    const nb_camera_link_command_t command = {
                        .version = NB_CAMERA_LINK_COMMAND_VERSION,
                        .opcode = NB_CAMERA_LINK_OP_REQUEST_SNAPSHOT,
                        .mode = NB_CAMERA_LINK_MODE_QQVGA,
                        .request_id = i + 1U,
                    };
                    const esp_err_t req_err =
                        nb_main_link_service_request_camera(&command);
                    if (req_err != ESP_OK) {
                        ESP_LOGW(TAG, "DM4 camera request %lu falhou: %s",
                                 (unsigned long)command.request_id,
                                 esp_err_to_name(req_err));
                        vTaskDelay(pdMS_TO_TICKS(NB_DM4_CAMERA_PROBE_WAIT_MS));
                        continue;
                    }

                    nb_camera_link_event_t event;
                    uint32_t event_wait_ms = 0U;
                    esp_err_t evt_err;
                    do {
                        vTaskDelay(pdMS_TO_TICKS(NB_DM4_CAMERA_PROBE_WAIT_MS));
                        event_wait_ms += NB_DM4_CAMERA_PROBE_WAIT_MS;
                        evt_err =
                            nb_main_link_service_get_last_camera_event(&event);
                    } while (evt_err != ESP_OK &&
                            event_wait_ms < NB_DM4_CAMERA_PROBE_TIMEOUT_MS);

                    if (evt_err == ESP_OK && event.request_id == command.request_id) {
                        ESP_LOGI(TAG,
                                 "DM4 camera event request_id=%lu status=%u mode=%u",
                                 (unsigned long)event.request_id,
                                 (unsigned)event.status,
                                 (unsigned)event.mode);
                    } else {
                        ESP_LOGW(TAG, "DM4 camera request %lu sem resposta em %u ms",
                                 (unsigned long)command.request_id,
                                 (unsigned)NB_DM4_CAMERA_PROBE_TIMEOUT_MS);
                    }
                }
                ESP_LOGI(TAG, "DM4 camera probe concluido");
            }
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
