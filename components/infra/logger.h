#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * logger_init() — Configura nivel de log e UART.
 * Chamado no BOOT_PHASE_0, antes de qualquer periferico externo.
 */
esp_err_t logger_init(void);

/*
 * logger_init_sd() — Instala handler de log para arquivo no microSD.
 * Chamado em BOOT_PHASE_3, apos microSD montado.
 * A partir dai, todos os logs vao para UART + arquivo com buffer.
 */
esp_err_t logger_init_sd(const char *log_dir_path);

/*
 * logger_flush() — Forca escrita do buffer de log para o SD.
 * Chamar antes de shutdown ou em pontos criticos de persistencia.
 */
void      logger_flush(void);

/*
 * logger_deinit_sd() — Remove handler de SD. Chamado ao detectar
 * remocao do cartao (EVT_STORAGE_DEGRADED).
 */
void      logger_deinit_sd(void);

bool      logger_sd_active(void);

/* ------------------------------------------------------------------ */
/*  Macros de log — wrappers sobre ESP_LOG                            */
/*  Usar sempre estas macros, nunca ESP_LOGx diretamente.             */
/* ------------------------------------------------------------------ */

#define NB_LOGD(tag, fmt, ...)  ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define NB_LOGI(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define NB_LOGW(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define NB_LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)

/*
 * NB_ASSERT_ISR_SAFE — Assert que garante que logger nao e chamado de ISR.
 * Incluir no inicio de funcoes que usam logger se houver risco de chamada de ISR.
 */
#define NB_ASSERT_NOT_ISR()                                         \
    do {                                                            \
        if (xPortInIsrContext()) {                                  \
            ESP_EARLY_LOGE("logger", "LOG chamado de ISR context"); \
            abort();                                                \
        }                                                           \
    } while (0)

#ifdef __cplusplus
}
#endif
