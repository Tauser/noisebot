/*
 * power_monitor.h — Monitor de energia e modos de operação do NoiseBot
 *
 * Responsabilidades:
 *   - Detectar reset por brownout via esp_reset_reason() e manter contador
 *     persistente em NVS (namespace "nb_sys", chave "brn_count").
 *   - Activar safe_mode no NVS quando brn_count >= NB_POWER_BROWNOUT_SAFE_THRESHOLD.
 *   - Registrar callback de brownout em runtime (ISR-safe, detalhe abaixo).
 *   - Gerenciar o modo de operação do sistema (nb_power_mode_t).
 *
 * Integração com boot_manager:
 *   - power_monitor_init() é chamado em PHASE_POWER.
 *   - boot_manager define o modo final com power_monitor_set_mode() em
 *     PHASE_COMPLETE, após conhecer sd_degraded e safe_mode.
 *
 * Callback de brownout:
 *   Chamado em contexto de ISR. Por enquanto apenas loga.
 *   TODO(etapa-3.2): chamar motion_safety_emergency_stop_from_isr().
 *   TODO(etapa-0.5): publicar NB_EVT_POWER_BROWNOUT_WARN no event bus.
 */

#ifndef NB_POWER_MONITOR_H
#define NB_POWER_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Modos de operação ────────────────────────────────────────────────────── */

typedef enum {
    NB_POWER_NORMAL         = 0,  /**< Tudo habilitado                          */
    NB_POWER_SD_DEGRADED    = 1,  /**< SD ausente — logging só UART             */
    NB_POWER_SAFE_MODE      = 2,  /**< Motion desabilitado — só display + logs  */
    NB_POWER_EMERGENCY_STOP = 3,  /**< Tudo desabilitado exceto logging          */
} nb_power_mode_t;

/* ── Configuração ─────────────────────────────────────────────────────────── */

/** Número de brownouts consecutivos antes de forçar safe_mode no próximo boot. */
#define NB_POWER_BROWNOUT_SAFE_THRESHOLD  3U

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o power monitor.
 *
 * Deve ser chamado em PHASE_POWER, após nvs_flash_init() concluído.
 *
 * Ações:
 *   1. Detecta reset por brownout (esp_reset_reason() == ESP_RST_BROWNOUT).
 *   2. Se brownout: incrementa "brn_count" em NVS "nb_sys".
 *      Se brn_count >= threshold: seta "safe_mode"=1 (boot_manager lê no
 *      próximo boot via boot_nvs_load_and_update).
 *   3. Registra callback de brownout (ESP_EARLY_LOGE — ISR-safe).
 *
 * Falhas de NVS são não-fatais: loggadas e execução continua.
 *
 * @return ESP_OK em sucesso ou ESP_FAIL se já inicializado.
 */
esp_err_t power_monitor_init(void);

/**
 * @brief Retorna o modo de operação atual.
 */
nb_power_mode_t power_monitor_get_mode(void);

/**
 * @brief Define o modo de operação e loga a transição.
 *
 * Chamadas com o mesmo modo atual são ignoradas (sem log).
 *
 * @param mode    Novo modo.
 * @param reason  Motivo da transição (ex: "safe_mode ativo no boot"). Pode ser NULL.
 */
void power_monitor_set_mode(nb_power_mode_t mode, const char *reason);

/**
 * @brief Retorna true se o último reset foi causado por brownout.
 *
 * Valor definido em power_monitor_init(). Seguro chamar após init.
 */
bool power_monitor_is_brownout_reset(void);

/**
 * @brief Retorna o contador de brownouts consecutivos (lido do NVS no boot).
 *
 * Se o boot atual foi brownout, este valor já inclui o incremento feito em init.
 * Se não foi brownout, reflete o valor armazenado antes deste boot.
 */
uint8_t power_monitor_get_brownout_count(void);

#endif /* NB_POWER_MONITOR_H */
