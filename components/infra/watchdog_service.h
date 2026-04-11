#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATCHDOG_MAX_SERVICES   12

/*
 * watchdog_service_init() — Inicializa o software watchdog.
 * O hardware WDT e configurado em boot_manager_init().
 * Chamado no BOOT_PHASE_0.
 */
esp_err_t watchdog_service_init(void);

/*
 * watchdog_register() — Registra um servico para monitoramento.
 * expected_interval_ms: intervalo maximo esperado entre heartbeats.
 * Chamar uma vez durante init de cada servico.
 */
esp_err_t watchdog_register(const char *name, uint32_t expected_interval_ms);

/*
 * watchdog_heartbeat() — Confirma que o servico esta vivo.
 * Chamar periodicamente dentro do loop principal de cada servico.
 */
esp_err_t watchdog_heartbeat(const char *name);

/*
 * watchdog_unregister() — Remove servico do monitoramento.
 * Chamar ao desligar um servico graciosamente.
 */
esp_err_t watchdog_unregister(const char *name);

/*
 * watchdog_add_current_task_to_hw_wdt() — Registra a task atual
 * no hardware Task WDT do ESP-IDF.
 */
esp_err_t watchdog_add_current_task_to_hw_wdt(void);

/*
 * watchdog_feed_hw() — Reseta o hardware WDT para a task atual.
 * Equivalente a esp_task_wdt_reset().
 */
void watchdog_feed_hw(void);

#ifdef __cplusplus
}
#endif
