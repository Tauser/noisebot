/*
 * error_policy.h — Política de erros e macros de assert do NodeBot
 *
 * Três níveis de severidade:
 *
 *   NB_ASSERT        — não-fatal. Loga e continua. Para verificações de runtime
 *                      onde a operação pode ser degradada mas o sistema sobrevive.
 *
 *   NB_ASSERT_FATAL  — fatal. Loga com stack info e reinicia. Para estados
 *                      inconsistentes onde continuar operando é incorreto.
 *
 *   NB_ASSERT_SAFETY — segurança. Loga, futuras versões desabilitam motion antes
 *                      de reiniciar. Para violações que envolvem hardware físico.
 *
 * Uso:
 *   NB_ASSERT(ptr != NULL, TAG, "buffer nulo em %s", __func__);
 *   NB_ASSERT_FATAL(err == ESP_OK, TAG, "NVS init falhou: %s", esp_err_to_name(err));
 *   NB_ASSERT_SAFETY(pos <= limit, TAG, "servo %d excede limite: %d > %d", id, pos, limit);
 */

#ifndef NB_ERROR_POLICY_H
#define NB_ERROR_POLICY_H

#include "esp_log.h"
#include "esp_system.h"

/* ── NB_ASSERT — não-fatal ────────────────────────────────────────────────── */

#define NB_ASSERT(cond, tag, fmt, ...)                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ESP_LOGW(tag, "ASSERT falhou (%s:%d): " fmt,                        \
                     __FILE__, __LINE__, ##__VA_ARGS__);                        \
        }                                                                       \
    } while (0)

/* ── NB_ASSERT_FATAL — reinicia o sistema ────────────────────────────────── */

#define NB_ASSERT_FATAL(cond, tag, fmt, ...)                                    \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ESP_LOGE(tag, "FATAL (%s:%d): " fmt,                                \
                     __FILE__, __LINE__, ##__VA_ARGS__);                        \
            ESP_LOGE(tag, "Reiniciando em 100ms...");                           \
            vTaskDelay(pdMS_TO_TICKS(100));                                     \
            esp_restart();                                                      \
        }                                                                       \
    } while (0)

/* ── NB_ASSERT_SAFETY — desabilita motion e reinicia ─────────────────────── */
/*
 * TODO(etapa-3.2): antes do esp_restart(), inserir:
 *     motion_safety_emergency_stop();
 * O stub atual tem o mesmo comportamento de FATAL.
 */

#define NB_ASSERT_SAFETY(cond, tag, fmt, ...)                                   \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ESP_LOGE(tag, "SAFETY VIOLATION (%s:%d): " fmt,                     \
                     __FILE__, __LINE__, ##__VA_ARGS__);                        \
            ESP_LOGE(tag, "[TODO] motion_safety_emergency_stop() pendente");    \
            ESP_LOGE(tag, "Reiniciando em 100ms...");                           \
            vTaskDelay(pdMS_TO_TICKS(100));                                     \
            esp_restart();                                                      \
        }                                                                       \
    } while (0)

/* ── NB_ERROR_CHECK — espelho de ESP_ERROR_CHECK com log de contexto ──────── */

#define NB_ERROR_CHECK(err, tag, fmt, ...)                                      \
    do {                                                                        \
        esp_err_t _e = (err);                                                   \
        if (_e != ESP_OK) {                                                     \
            ESP_LOGE(tag, "erro %s (%s:%d): " fmt,                              \
                     esp_err_to_name(_e), __FILE__, __LINE__, ##__VA_ARGS__);   \
        }                                                                       \
    } while (0)

#define NB_ERROR_CHECK_FATAL(err, tag, fmt, ...)                                \
    do {                                                                        \
        esp_err_t _e = (err);                                                   \
        NB_ASSERT_FATAL(_e == ESP_OK, tag,                                      \
                        "esp_err=%s: " fmt, esp_err_to_name(_e), ##__VA_ARGS__);\
    } while (0)

#endif /* NB_ERROR_POLICY_H */
