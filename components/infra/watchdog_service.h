/*
 * watchdog_service.h — Serviço de watchdog do NodeBot
 *
 * Wrapper sobre o Task Watchdog Timer (TWDT) do ESP-IDF.
 * O TWDT é pré-configurado via sdkconfig (timeout 5s, panic habilitado).
 * Este serviço gerencia o registro/desregistro de tasks e cria a
 * nb_wdog_task — task de alta prioridade que alimenta o watchdog do sistema.
 *
 * Modelo de uso:
 *
 *   1. nb_watchdog_init() — chamado pelo boot_manager em PHASE_EARLY.
 *      Cria a nb_wdog_task que roda em Core 0 @ prioridade 24.
 *
 *   2. Cada task crítica chama nb_watchdog_add_task(NULL) ao iniciar
 *      e nb_watchdog_feed() no seu loop principal.
 *
 *   3. Se uma task trava (não chama nb_watchdog_feed em ~5s), o TWDT
 *      dispara panic e o HW WDT reseta o sistema.
 *
 * Tasks registradas no watchdog (por etapa de implementação):
 *   Etapa 0.1: nb_wdog_task (auto-registrada)
 *   Etapa 3.2: nb_servo_safety_task
 *   Etapa 3.3: nb_motion_task
 *   Etapa 4.2: nb_audio_task
 *   Etapa 1.2: nb_render_task
 *   Etapa 5.1: nb_behavior_task
 */

#ifndef NB_WATCHDOG_SERVICE_H
#define NB_WATCHDOG_SERVICE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Configuração ─────────────────────────────────────────────────────────── */

/** Timeout do TWDT durante fase de boot (antes de PHASE_COMPLETE). */
#define NB_WATCHDOG_TIMEOUT_BOOT_MS    30000U

/** Timeout do TWDT durante operação normal (após PHASE_COMPLETE). */
#define NB_WATCHDOG_TIMEOUT_RUNTIME_MS  5000U

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o serviço de watchdog e cria nb_wdog_task.
 *
 * Deve ser chamado em PHASE_EARLY, logo após nb_logger_init().
 * Adiciona a task chamadora ao TWDT automaticamente.
 *
 * @return ESP_OK em sucesso, ESP_ERR_NO_MEM se task não puder ser criada.
 */
esp_err_t nb_watchdog_init(void);

/**
 * @brief Registra uma task no TWDT.
 *
 * @param task Handle da task. NULL registra a task atual.
 * @return ESP_OK ou código de erro do TWDT.
 */
esp_err_t nb_watchdog_add_task(TaskHandle_t task);

/**
 * @brief Alimenta o watchdog para a task atual.
 *
 * Deve ser chamado periodicamente no loop de cada task registrada.
 * Período máximo: NB_WATCHDOG_TIMEOUT_RUNTIME_MS / 2 = ~2.5s.
 *
 * @return ESP_OK ou ESP_ERR_NOT_FOUND se task não está registrada.
 */
esp_err_t nb_watchdog_feed(void);

/**
 * @brief Remove uma task do TWDT.
 *
 * @param task Handle da task. NULL remove a task atual.
 * @return ESP_OK ou código de erro.
 */
esp_err_t nb_watchdog_remove_task(TaskHandle_t task);

#endif /* NB_WATCHDOG_SERVICE_H */
