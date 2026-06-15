/*
 * config_manager.h — Gerenciador de configuração do NoiseBot
 *
 * Abstração sobre NVS para configuração do produto. Responsabilidades:
 *
 *   - Abrir e manter handles dos namespaces nb_cfg e nb_svc.
 *   - Detectar primeiro boot (via cfg_ver) e aplicar todos os defaults.
 *   - Expor API tipada com validação de range em cada set.
 *   - Fazer commit após cada set bem-sucedido (resiliência a power loss).
 *
 * Dependências de inicialização:
 *   nvs_flash_init() deve ter sido chamado antes de config_manager_init().
 *   O boot_manager garante isso em PHASE_EARLY.
 *
 * Thread safety:
 *   O ESP-IDF garante thread safety dos handles NVS internamente.
 *   Cada getter/setter é seguro para chamada de qualquer task.
 */

#ifndef NB_CONFIG_MANAGER_H
#define NB_CONFIG_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Init / Deinit ───────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o gerenciador de configuração.
 *
 * Abre handles NVS para nb_cfg e nb_svc.
 * Se cfg_ver estiver ausente ou desatualizado, aplica todos os defaults
 * e faz commit (primeiro boot ou migração de schema).
 *
 * Deve ser chamado em PHASE_EARLY, após nvs_flash_init().
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_NO_MEM se NVS sem espaço para defaults.
 *         Outros erros de nvs_open/nvs_set.
 */
esp_err_t config_manager_init(void);

/**
 * @brief Retorna true se a inicialização foi bem-sucedida.
 */
bool config_manager_is_ready(void);

/* ── Servo ───────────────────────────────────────────────────────────────── */
/*
 * servo_id: 1 = pan (horizontal), 2 = tilt (vertical).
 * Posições em unidades brutas SCS0009 (0–1023).
 * Defaults: centro=512, min=410, max=614 (±30°, expandir pós-calibração).
 */

int16_t   config_get_servo_limit_min(uint8_t servo_id);
int16_t   config_get_servo_limit_max(uint8_t servo_id);
int16_t   config_get_servo_center   (uint8_t servo_id);

/**
 * @return ESP_ERR_INVALID_ARG se servo_id inválido ou val fora de 0–1023.
 */
esp_err_t config_set_servo_limit_min(uint8_t servo_id, int16_t val);
esp_err_t config_set_servo_limit_max(uint8_t servo_id, int16_t val);
esp_err_t config_set_servo_center   (uint8_t servo_id, int16_t val);

/* ── Áudio ───────────────────────────────────────────────────────────────── */

uint8_t   config_get_volume(void);

/**
 * @return ESP_ERR_INVALID_ARG se val > 100.
 */
esp_err_t config_set_volume(uint8_t level);

bool      config_get_silence_mode_enabled(void);
esp_err_t config_set_silence_mode_enabled(bool enabled);
bool      config_get_voice_audio_v2_capture_enabled(void);
esp_err_t config_set_voice_audio_v2_capture_enabled(bool enabled);
bool      config_get_voice_audio_v2_capture_tx_enabled(void);
esp_err_t config_set_voice_audio_v2_capture_tx_enabled(bool enabled);
bool      config_get_voice_audio_v2_activity_decider_enabled(void);
esp_err_t config_set_voice_audio_v2_activity_decider_enabled(bool enabled);

/* ── Display ─────────────────────────────────────────────────────────────── */

uint8_t   config_get_brightness(void);
esp_err_t config_set_brightness(uint8_t val);  /* 0–255, sem rejeição */

/* ── Touch ───────────────────────────────────────────────────────────────── */

uint8_t   config_get_touch_sensitivity(void);

/**
 * @return ESP_ERR_INVALID_ARG se val < 1 ou val > 100.
 */
esp_err_t config_set_touch_sensitivity(uint8_t val);

/* ── Comportamento ───────────────────────────────────────────────────────── */

uint32_t  config_get_idle_timeout_s(void);

/**
 * @return ESP_ERR_INVALID_ARG se seconds < 10 ou seconds > 3600.
 */
esp_err_t config_set_idle_timeout_s(uint32_t seconds);

/* ── Presença Social (SPEC_PRESENCA_SOCIAL §2.3) ─────────────────────────── */
/* Todos os timers são u32 em milissegundos. Retornam o default NVS quando a
 * chave ainda não foi escrita (additive — não requer migração de schema). */

uint32_t  config_get_presence_hold_ms(void);   /* hold antes de LEFT_RECENTLY  */
uint32_t  config_get_presence_maybe_ms(void);  /* MAYBE_SOMEONE timeout         */
uint32_t  config_get_presence_heur_ms(void);   /* heurística offline → PRESENT  */
uint32_t  config_get_presence_lrec_ms(void);   /* LEFT_RECENTLY → AWAY          */
uint32_t  config_get_presence_away_ms(void);   /* AWAY → ALONE_SETTLED          */
uint32_t  config_get_presence_engd_ms(void);   /* PRESENT → ENGAGED             */
uint32_t  config_get_presence_rtn_ms(void);    /* ausência mínima p/ RETURNED   */

/* ── Estado de serviços (nb_svc) ─────────────────────────────────────────── */

uint8_t   config_get_last_emotion(void);
esp_err_t config_set_last_emotion(uint8_t emotion);

uint32_t  config_get_persona_seed(void);
/* persona_seed é gerado uma única vez no primeiro boot — sem setter público. */

#endif /* NB_CONFIG_MANAGER_H */
