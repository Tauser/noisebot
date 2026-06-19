/*
 * time_service.h — Sincronização SNTP e hora local (Layer 5)
 *
 * Sincroniza via pool.ntp.org quando NB_EVT_WIFI_IP_ACQUIRED chegar.
 * Persiste POSIX TZ, nome do fuso e localização em NVS (namespace nb_time).
 *
 * Defaults: BRT3 / America/Sao_Paulo / Brasilia, DF
 *
 * Eventos publicados:
 *   NB_EVT_TIME_SYNCED       — SNTP completou a primeira sincronização
 *   NB_EVT_TIME_SYNC_FAILED  — (futuro: timeout sem resposta do servidor)
 */

#ifndef NB_TIME_SERVICE_H
#define NB_TIME_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_TIME_TZ_POSIX_MAX   33U   /* string POSIX TZ, incluindo '\0' */
#define NB_TIME_TZ_NAME_MAX    33U   /* nome IANA-style, incluindo '\0'  */
#define NB_TIME_LOCATION_MAX   49U   /* localização display, incl. '\0'  */

/**
 * @brief Inicializa o serviço de tempo.
 *
 * Carrega configuração de NVS, aplica timezone via setenv/tzset,
 * e subscreve NB_EVT_WIFI_IP_ACQUIRED para disparar SNTP automaticamente.
 *
 * Deve ser chamado em PHASE_SERVICES após event_bus estar disponível.
 */
esp_err_t   time_service_init(void);

/** @return true após a primeira sincronização SNTP bem-sucedida. */
bool        time_service_is_synced(void);

/**
 * @brief Preenche *out com a hora local atual (localtime_r).
 * @return ESP_ERR_INVALID_ARG se out == NULL.
 */
esp_err_t   time_service_get_local_time(struct tm *out);

/** Strings retornadas são válidas enquanto o serviço estiver inicializado. */
const char *time_service_get_tz_posix(void);
const char *time_service_get_tz_name(void);
const char *time_service_get_location(void);

/**
 * @brief Altera timezone, persiste em NVS e aplica imediatamente.
 *
 * @param posix_tz  String POSIX (ex: "BRT3", "EST5EDT,M3.2.0,M11.1.0")
 * @param tz_name   Nome display (ex: "America/Sao_Paulo")
 */
esp_err_t   time_service_set_timezone(const char *posix_tz, const char *tz_name);

/** @brief Altera localização (display only), persiste em NVS. */
esp_err_t   time_service_set_location(const char *location);

/**
 * @brief Inicia ou reinicia sincronização SNTP agora.
 *
 * Chamado automaticamente em NB_EVT_WIFI_IP_ACQUIRED; pode ser chamado
 * manualmente via API HTTP.
 */
esp_err_t   time_service_sync_now(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_TIME_SERVICE_H */
