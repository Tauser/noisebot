/*
 * web_ota.c — OTA via HTTP (Layer 2)
 *
 * Extraído de web_service.c (F09). Este TU não inclui nenhum header de Layer
 * 5-7 (condutores, persona, emotion_model, etc.) — só o necessário para OTA.
 *
 * Validação F18: project_name da imagem é verificado antes do primeiro write.
 */

#include "web_ota.h"
#include "logger.h"
#include "led_service.h"

#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "nb_ota"

typedef struct { char url[256]; } ota_task_arg_t;

static void ota_task(void *arg)
{
    ota_task_arg_t *a = (ota_task_arg_t *)arg;
    char url[256];
    strlcpy(url, a->url, sizeof(url));
    heap_caps_free(a);

    NB_LOGI(TAG, "OTA: iniciando de %s", url);
    led_base_set(NB_LED_BASE_SAFE_MODE, true);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        NB_LOGE(TAG, "OTA: nenhuma particao OTA disponivel");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t hcfg = {
        .url        = url,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        NB_LOGE(TAG, "OTA: http open falhou");
        if (client) esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        NB_LOGE(TAG, "OTA: ota_begin falhou");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        NB_LOGE(TAG, "OTA: sem memoria");
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    int last_pct = -1;
    esp_err_t write_err = ESP_OK;
    bool first_block = true;

    while (write_err == ESP_OK) {
        int n = esp_http_client_read(client, (char *)buf, 4096);
        if (n < 0) { write_err = ESP_FAIL; break; }
        if (n == 0) break;

        /* Validar project_name no primeiro bloco antes de qualquer escrita (F18). */
        if (first_block) {
            first_block = false;
            const size_t k_desc_offset = sizeof(esp_image_header_t)
                                       + sizeof(esp_image_segment_header_t);
            if ((size_t)n >= k_desc_offset + sizeof(esp_app_desc_t)) {
                const esp_app_desc_t *desc =
                    (const esp_app_desc_t *)(buf + k_desc_offset);
                if (desc->magic_word == 0xABCD5432U &&
                    strncmp(desc->project_name, "noisebot",
                            sizeof(desc->project_name)) != 0) {
                    NB_LOGE(TAG, "OTA rejeitada: project_name invalido ('%.*s')",
                            (int)sizeof(desc->project_name), desc->project_name);
                    write_err = ESP_ERR_OTA_VALIDATE_FAILED;
                    break;
                }
            }
        }

        write_err = esp_ota_write(ota_handle, buf, (size_t)n);
        total += n;
        int pct = (content_len > 0) ? (total * 100 / content_len) : 50;
        if (pct > 99) pct = 99;
        if (pct != last_pct) {
            NB_LOGI(TAG, "OTA: %d%%", pct);
            last_pct = pct;
        }
    }

    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (write_err != ESP_OK || total == 0) {
        esp_ota_abort(ota_handle);
        NB_LOGE(TAG, "OTA: download falhou (err=%d total=%d)", write_err, total);
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        NB_LOGE(TAG, "OTA: finalizacao falhou");
        vTaskDelete(NULL);
        return;
    }

    NB_LOGI(TAG, "OTA: OK (%d bytes) — reiniciando em 3s", total);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

esp_err_t web_ota_start(const char *url)
{
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;

    ota_task_arg_t *arg = (ota_task_arg_t *)heap_caps_malloc(
        sizeof(ota_task_arg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arg) {
        arg = (ota_task_arg_t *)heap_caps_malloc(
            sizeof(ota_task_arg_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!arg) return ESP_ERR_NO_MEM;

    strlcpy(arg->url, url, sizeof(arg->url));

    if (xTaskCreate(ota_task, "nb_ota", 8192, arg, 5, NULL) != pdPASS) {
        heap_caps_free(arg);
        return ESP_FAIL;
    }
    return ESP_OK;
}
