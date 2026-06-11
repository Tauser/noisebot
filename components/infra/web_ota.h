/*
 * web_ota.h — OTA via HTTP (Layer 2, extração de web_service — F09)
 *
 * Isola todo o código de OTA de web_service.c. Caminho para migração futura
 * para components/interface/web_ota/ (sem dependência de HAL ou serviços de layer 4+).
 */

#ifndef NB_WEB_OTA_H
#define NB_WEB_OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * web_ota_start() — Inicia OTA a partir de uma URL HTTP em task separada.
 *
 * Retorna imediatamente. A task de OTA faz download, valida project_name,
 * grava na partição OTA e reinicia.
 *
 * @param url  URL HTTP da imagem (máx 255 chars).
 * @return ESP_OK se a task foi criada, ESP_FAIL/ESP_ERR_NO_MEM em falha.
 */
esp_err_t web_ota_start(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* NB_WEB_OTA_H */
