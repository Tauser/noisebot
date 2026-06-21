/*
 * power_monitor.h — Monitor de energia e modos de operação do NoiseBot
 *
 * Responsabilidades:
 *   - Detectar reset por brownout via esp_reset_reason() e manter contador
 *     persistente em NVS (namespace "nb_sys", chave "brn_count").
 *   - Activar safe_mode no NVS quando brn_count >= NB_POWER_BROWNOUT_SAFE_THRESHOLD.
 *   - Gerenciar o modo de operação do sistema (nb_power_mode_t).
 *   - Ler o barramento de 5 V via ADC quando o perfil de placa o suportar.
 *
 * Integração com boot_manager:
 *   - power_monitor_init() é chamado em PHASE_POWER.
 *   - boot_manager define o modo final com power_monitor_set_mode() em
 *     PHASE_COMPLETE, após conhecer sd_degraded e safe_mode.
 *
 * Brownout em operação:
 *   O brownout detector do ESP32-S3 aciona um reset de hardware imediato —
 *   não há janela de execução para código de usuário. Proteção de motion é
 *   reativa: power_monitor_init() detecta ESP_RST_BROWNOUT no próximo boot,
 *   incrementa brn_count e, ao atingir o threshold, ativa safe_mode no NVS.
 *   No boot seguinte, PHASE_SAFETY e PHASE_MOTION são puladas → servos sem
 *   torque por design.
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
#define NB_POWER_5V_WARN_MV              4700U
#define NB_POWER_5V_CRITICAL_MV          4500U

/** Correcao empirica de calibracao do ADC de 5V (DMM.3, bancada 2026-06-21):
 *  fator ~1.0223 derivado de adc_mv=2317 vs multimetro=2368 em GPIO7. */
#define NB_POWER_5V_ADC_CAL_NUM          10223U
#define NB_POWER_5V_ADC_CAL_DEN          10000U

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
 * Falhas de NVS são não-fatais: loggadas e execução continua.
 * A instrumentação ADC de 5 V é best-effort: se a calibração falhar, o
 * monitor continua com conversão aproximada; se o perfil não expuser ADC,
 * a API retorna ESP_ERR_NOT_SUPPORTED.
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

/**
 * @brief Retorna true quando o perfil atual expõe um ADC válido para 5 V.
 */
bool power_monitor_has_5v_adc(void);

/**
 * @brief Lê a tensão no pino ADC do divisor resistivo em milivolts.
 *
 * Retorna o valor no ponto médio do divisor, não a tensão real do barramento.
 */
esp_err_t power_monitor_read_5v_adc_mv(uint32_t *adc_mv);

/**
 * @brief Lê a tensão estimada do barramento de 5 V em milivolts.
 *
 * Usa o divisor 68k/56k documentado para reconstruir a tensão do barramento.
 */
esp_err_t power_monitor_read_5v_bus_mv(uint32_t *bus_mv);

/**
 * @brief Retorna true se a leitura atual do barramento 5 V está em aviso.
 */
bool power_monitor_is_5v_warn(void);

/**
 * @brief Retorna true se a leitura atual do barramento 5 V está em nível crítico.
 */
bool power_monitor_is_5v_critical(void);

#endif /* NB_POWER_MONITOR_H */
