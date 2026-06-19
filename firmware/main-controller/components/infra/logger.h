/*
 * logger.h — Logger do NoiseBot
 *
 * Thin wrapper sobre esp_log com ponto de inicialização centralizado,
 * controle de nível e hook para flush futuro em microSD (Etapa 0.3).
 *
 * Uso:
 *   NB_LOGI(TAG, "inicializado em %u ms", elapsed_ms);
 *   NB_LOGE(TAG, "falha: %s", esp_err_to_name(err));
 *
 * O nível default é configurado em nb_logger_init(). Pode ser alterado em
 * runtime por módulo via nb_logger_set_module_level().
 */

#ifndef NB_LOGGER_H
#define NB_LOGGER_H

#include "esp_log.h"
#include "esp_err.h"
#include <stdint.h>

/* ── Níveis de log ───────────────────────────────────────────────────────── */

typedef enum {
    NB_LOG_LEVEL_NONE    = ESP_LOG_NONE,
    NB_LOG_LEVEL_ERROR   = ESP_LOG_ERROR,
    NB_LOG_LEVEL_WARN    = ESP_LOG_WARN,
    NB_LOG_LEVEL_INFO    = ESP_LOG_INFO,
    NB_LOG_LEVEL_DEBUG   = ESP_LOG_DEBUG,
    NB_LOG_LEVEL_VERBOSE = ESP_LOG_VERBOSE,
} nb_log_level_t;

/* ── Callback de flush para SD (registrado na Etapa 0.3) ─────────────────── */

typedef void (*nb_log_flush_cb_t)(const char *line, size_t len);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o logger com nível global padrão.
 *
 * Deve ser chamado como primeira ação do PHASE_EARLY do boot_manager,
 * antes de qualquer outra função do sistema.
 *
 * @param default_level Nível aplicado a todos os módulos inicialmente.
 * @return ESP_OK sempre (para consistência com outras inits).
 */
esp_err_t nb_logger_init(nb_log_level_t default_level);

/**
 * @brief Altera o nível de log de um módulo específico.
 *
 * @param tag    Tag do módulo (ex: "nb_boot"). NULL altera o nível global.
 * @param level  Novo nível.
 */
void nb_logger_set_module_level(const char *tag, nb_log_level_t level);

/**
 * @brief Registra callback de flush para microSD (Etapa 0.3).
 *
 * Quando registrado, cada linha de log é passada para o callback além
 * de ser enviada para UART. O callback é chamado de forma síncrona —
 * deve ser thread-safe e não bloquear por mais de 1ms.
 *
 * @param cb Função callback. NULL desregistra.
 */
void nb_logger_register_flush_cb(nb_log_flush_cb_t cb);

/* ── Macros de log ───────────────────────────────────────────────────────── */
/*
 * Idênticos ao ESP_LOGX mas com prefixo NB_ para facilitar busca no código
 * e permitir substituição futura da implementação sem alterar call sites.
 */

#define NB_LOGV(tag, fmt, ...)  ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#define NB_LOGD(tag, fmt, ...)  ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define NB_LOGI(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define NB_LOGW(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define NB_LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)

#endif /* NB_LOGGER_H */
