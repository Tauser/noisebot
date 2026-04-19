/*
 * wifi_service.h — WiFi Service (Layer 2, Etapa 9.6)
 *
 * Conecta ao AP configurado em NVS no boot. Inicialização não-bloqueante:
 * wifi_service_init() retorna imediatamente; associação ocorre em background.
 *
 * Sem credenciais em NVS: permanece offline, sistema opera normalmente.
 * Reconexão automática com backoff exponencial (1s → 2s → ... → 60s).
 * mDNS: hostname "noisebot.local" registrado após IP adquirido.
 *
 * Credenciais em NVS (namespace "nb_wifi"):
 *   "ssid"    — string, máx 32 chars
 *   "pass"    — string, máx 64 chars
 *   "enabled" — u8, default 1
 *
 * Thread-safety: getters são thread-safe via spinlock.
 */

#ifndef NB_WIFI_SERVICE_H
#define NB_WIFI_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o WiFi Service e inicia conexão em background.
 *
 * Deve ser chamado em PHASE_SERVICES após diagnostics_init().
 * Retorna ESP_OK imediatamente — sem credenciais, fica offline sem erro.
 *
 * @return ESP_OK em todos os casos sem credenciais ou com init bem-sucedido.
 *         ESP_FAIL se o driver WiFi falhar ao inicializar.
 */
esp_err_t wifi_service_init(void);

/**
 * @brief Retorna true se IP foi adquirido e está ativo.
 *
 * Thread-safe.
 */
bool wifi_service_is_connected(void);

/**
 * @brief Retorna o IP atual como string "x.x.x.x", ou "" se desconectado.
 *
 * Ponteiro para buffer interno — válido apenas enquanto conectado.
 * Thread-safe para leitura.
 */
const char *wifi_service_get_ip(void);

/**
 * @brief Retorna o SSID configurado atualmente, ou "" se sem credenciais.
 */
const char *wifi_service_get_ssid(void);

/**
 * @brief Retorna o RSSI do AP atual (dBm). 0 se desconectado.
 */
int8_t wifi_service_get_rssi(void);

/**
 * @brief Grava novas credenciais em NVS e reconecta ao AP.
 *
 * Thread-safe. A reconexão ocorre em background.
 *
 * @param ssid  SSID do AP (máx 32 chars).
 * @param pass  Senha do AP (máx 64 chars). Pode ser "" para redes abertas.
 * @return ESP_OK se gravado e reconexão iniciada.
 */
esp_err_t wifi_service_set_credentials(const char *ssid, const char *pass);

/**
 * @brief Limpa credenciais do NVS e desabilita WiFi até próximo boot.
 */
esp_err_t wifi_service_clear_credentials(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_WIFI_SERVICE_H */
