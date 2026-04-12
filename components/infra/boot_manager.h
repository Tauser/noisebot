/*
 * boot_manager.h — Gerenciador de boot do NoiseBot
 *
 * Centraliza a sequência de inicialização em fases ordenadas.
 * Cada fase reporta sucesso ou falha. Falha em fase crítica → safe mode.
 *
 * Fases e etapas de implementação:
 *
 *   PHASE_EARLY     (Etapa 0.1) — logger, watchdog, NVS básico, reset reason
 *   PHASE_POWER     (Etapa 0.4) — brownout callback, power monitor
 *   PHASE_STORAGE   (Etapa 0.3) — microSD, persistence manager
 *   PHASE_HAL       (Bloco 1-2) — display, LEDs, touch, servo PING
 *   PHASE_SAFETY    (Etapa 3.2) — motion_safety (servos sem torque ainda)
 *   PHASE_SERVICES  (Bloco 4-5) — render, audio, behavior, conductor
 *   PHASE_MOTION    (Etapa 3.3) — servos ARMED (só após safety verde)
 *   PHASE_COMPLETE  (—)         — boot_count reset, idle behavior start
 *
 * Safe mode:
 *   Se boot_count (sem sucesso consecutivo) ≥ NB_BOOT_SAFE_MODE_THRESHOLD,
 *   o sistema entra em safe mode: fases SAFETY, SERVICES e MOTION são puladas.
 *   O robot opera com display + logs, mas sem movimento.
 *
 *   boot_manager_report_success() deve ser chamado quando o sistema estiver
 *   operacional para resetar boot_count.
 */

#ifndef NB_BOOT_MANAGER_H
#define NB_BOOT_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Configuração ─────────────────────────────────────────────────────────── */

/** Número de boots sem sucesso consecutivos antes de ativar safe mode. */
#define NB_BOOT_SAFE_MODE_THRESHOLD  3U

/* ── Tipos ───────────────────────────────────────────────────────────────── */

typedef enum {
    NB_BOOT_PHASE_NONE      = 0,
    NB_BOOT_PHASE_EARLY     = 1,
    NB_BOOT_PHASE_POWER     = 2,
    NB_BOOT_PHASE_STORAGE   = 3,
    NB_BOOT_PHASE_HAL       = 4,
    NB_BOOT_PHASE_SAFETY    = 5,
    NB_BOOT_PHASE_SERVICES  = 6,
    NB_BOOT_PHASE_MOTION    = 7,
    NB_BOOT_PHASE_COMPLETE  = 8,
} nb_boot_phase_t;

typedef struct {
    nb_boot_phase_t current_phase;   /**< Fase atual ou última fase concluída */
    bool            safe_mode;       /**< Sistema em safe mode */
    bool            sd_degraded;     /**< SD não disponível */
    uint32_t        boot_count;      /**< Boots sem sucesso consecutivos */
    uint8_t         reset_reason;    /**< esp_reset_reason_t do último reset */
} nb_boot_status_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Executa a sequência completa de boot.
 *
 * Ponto de entrada chamado por app_main(). Executa todas as fases em ordem.
 * Nunca retorna em operação normal.
 *
 * Em safe mode (3 boots consecutivos com falha), executa apenas as fases
 * básicas e retorna para o loop de safe mode em app_main().
 *
 * @return ESP_OK se safe mode ativo e chamador deve gerenciar o loop.
 *         Não retorna em operação normal.
 */
esp_err_t boot_manager_run(void);

/**
 * @brief Retorna o status atual do boot.
 *
 * Ponteiro para struct interna — não modificar.
 *
 * @return Ponteiro para nb_boot_status_t, ou NULL se boot_manager não iniciado.
 */
const nb_boot_status_t *boot_manager_get_status(void);

/**
 * @brief Retorna a fase atual do boot.
 */
nb_boot_phase_t boot_manager_get_phase(void);

/**
 * @brief Verifica se o sistema está em safe mode.
 */
bool boot_manager_is_safe_mode(void);

/**
 * @brief Reporta que o sistema está operacional.
 *
 * Reseta boot_count para 0 e limpa safe_mode_flag no NVS.
 * Deve ser chamado quando o sistema estiver completamente operacional —
 * tipicamente ao final de PHASE_COMPLETE, após o primeiro idle behavior.
 */
void boot_manager_report_success(void);

#endif /* NB_BOOT_MANAGER_H */
