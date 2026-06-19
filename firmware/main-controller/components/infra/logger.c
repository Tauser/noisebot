/*
 * logger.c — Implementação do logger do NoiseBot
 */

#include "logger.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nb_logger";

static nb_log_flush_cb_t s_flush_cb = NULL;

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t nb_logger_init(nb_log_level_t default_level)
{
    /* Configura nível global para todos os módulos. */
    esp_log_level_set("*", (esp_log_level_t)default_level);

    /*
     * Módulos de safety recebem nível mais verboso para facilitar diagnóstico.
     * Outros módulos específicos podem ser ajustados via nb_logger_set_module_level().
     */
    esp_log_level_set("nb_boot",   ESP_LOG_DEBUG);
    esp_log_level_set("nb_wdog",   ESP_LOG_INFO);
    esp_log_level_set("nb_safety", ESP_LOG_DEBUG);

    /* Reduz ruído de componentes de infraestrutura HTTP/WS */
    esp_log_level_set("httpd_txrx",  ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri",   ESP_LOG_ERROR);
    esp_log_level_set("httpd_sess",  ESP_LOG_ERROR);
    esp_log_level_set("nb_web",      ESP_LOG_WARN);
    esp_log_level_set("nb_render",   ESP_LOG_WARN);

    ESP_LOGI(TAG, "Logger inicializado — nivel global: %d", (int)default_level);
    return ESP_OK;
}

void nb_logger_set_module_level(const char *tag, nb_log_level_t level)
{
    esp_log_level_set(tag != NULL ? tag : "*", (esp_log_level_t)level);
}

void nb_logger_register_flush_cb(nb_log_flush_cb_t cb)
{
    s_flush_cb = cb;
    if (cb != NULL) {
        ESP_LOGI(TAG, "Callback de flush para SD registrado");
    } else {
        ESP_LOGI(TAG, "Callback de flush removido");
    }
}

/*
 * Nota sobre o flush callback:
 *
 * O ESP-IDF não expõe um hook de intercepção de todas as mensagens de log
 * sem substituir o vprintf padrão (esp_log_set_vprintf). Quando a Etapa 0.3
 * for implementada, o flush para SD será feito via:
 *
 *   1. Buffer circular em PSRAM acumulando mensagens de log
 *   2. persistence_task faz flush periódico para SD (a cada 60s ou no shutdown)
 *
 * Essa abordagem é preferível a intercepção do vprintf pois não adiciona
 * latência ao caminho crítico de logging.
 *
 * O s_flush_cb acima é um placeholder para uso pontual (ex: flush forçado
 * em crash) quando implementado na Etapa 0.3.
 */
