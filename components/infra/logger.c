#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *TAG = "logger";

#define LOG_SD_BUF_SIZE     4096
#define LOG_FILE_MAX_BYTES  (1 * 1024 * 1024)  /* 1MB por arquivo */

static bool             s_sd_active = false;
static FILE            *s_log_file  = NULL;
static char             s_log_dir[64];
static SemaphoreHandle_t s_mutex;
static char             s_sd_buf[LOG_SD_BUF_SIZE];
static size_t           s_sd_buf_pos = 0;
static int              s_file_index = 0;

static vprintf_like_t   s_orig_vprintf = NULL;

/* ------------------------------------------------------------------ */

static void prv_open_next_file(void) {
    if (s_log_file) { fclose(s_log_file); s_log_file = NULL; }
    char path[80];
    snprintf(path, sizeof(path), "%s/system_%04d.log", s_log_dir, s_file_index++);
    s_log_file = fopen(path, "a");
    if (!s_log_file) {
        ESP_LOGW(TAG, "Nao foi possivel abrir log file: %s", path);
    }
}

static int prv_vprintf_sd(const char *fmt, va_list args) {
    /* Escrever na saida original (UART via USB-JTAG) */
    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    if (!s_sd_active || !s_log_file) return ret;

    /* Buffer para SD */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        va_list args2;
        va_copy(args2, args);
        int n = vsnprintf(s_sd_buf + s_sd_buf_pos,
                          LOG_SD_BUF_SIZE - s_sd_buf_pos,
                          fmt, args2);
        va_end(args2);
        if (n > 0) s_sd_buf_pos += (size_t)n;

        /* Flush quando buffer cheio ou linha terminada */
        if (s_sd_buf_pos >= LOG_SD_BUF_SIZE - 256 ||
            (s_sd_buf_pos > 0 && s_sd_buf[s_sd_buf_pos - 1] == '\n')) {
            fwrite(s_sd_buf, 1, s_sd_buf_pos, s_log_file);
            s_sd_buf_pos = 0;

            /* Rotacionar se arquivo muito grande */
            long pos = ftell(s_log_file);
            if (pos > LOG_FILE_MAX_BYTES) {
                prv_open_next_file();
            }
        }
        xSemaphoreGive(s_mutex);
    }
    return ret;
}

/* ------------------------------------------------------------------ */

esp_err_t logger_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    esp_log_level_set("*", ESP_LOG_INFO);
    NB_LOGI(TAG, "Logger UART iniciado");
    return ESP_OK;
}

esp_err_t logger_init_sd(const char *log_dir_path) {
    if (!log_dir_path) return ESP_ERR_INVALID_ARG;
    strncpy(s_log_dir, log_dir_path, sizeof(s_log_dir) - 1);
    s_file_index = 0;
    prv_open_next_file();
    if (!s_log_file) return ESP_FAIL;

    s_orig_vprintf = esp_log_set_vprintf(prv_vprintf_sd);
    s_sd_active = true;
    NB_LOGI(TAG, "Log SD ativado: %s", log_dir_path);
    return ESP_OK;
}

void logger_flush(void) {
    if (!s_sd_active || !s_log_file) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_sd_buf_pos > 0) {
            fwrite(s_sd_buf, 1, s_sd_buf_pos, s_log_file);
            s_sd_buf_pos = 0;
        }
        fflush(s_log_file);
        xSemaphoreGive(s_mutex);
    }
}

void logger_deinit_sd(void) {
    if (!s_sd_active) return;
    logger_flush();
    if (s_orig_vprintf) esp_log_set_vprintf(s_orig_vprintf);
    if (s_log_file) { fclose(s_log_file); s_log_file = NULL; }
    s_sd_active = false;
    NB_LOGW(TAG, "Log SD desativado");
}

bool logger_sd_active(void) { return s_sd_active; }
