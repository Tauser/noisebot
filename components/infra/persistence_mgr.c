/*
 * persistence_mgr.c — Implementação do gerenciador de persistência do NoiseBot
 */

#include "persistence_mgr.h"
#include "sd_hal.h"
#include "nb_hw_config.h"
#include "logger.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define TAG "nb_persist"

/* ── Configuração ─────────────────────────────────────────────────────────── */

#define QUEUE_DEPTH           32U
#define LOG_LINE_MAX          192U     /* bytes máx por entrada de log */
#define LOG_FILE_PATH         NB_SD_MOUNT_POINT "/logs/log.txt"
#define LOG_FILE_MAX_BYTES    (512UL * 1024UL)  /* rotacionar após 512KB */
#define TASK_STACK_SIZE       4096U
#define TASK_PRIORITY         5U
#define TASK_CORE             0
#define FLUSH_TIMEOUT_MS      5000U

/* ── Tipos internos ───────────────────────────────────────────────────────── */

typedef enum {
    NB_PERSIST_OP_LOG,      /* escrever linha de log no SD */
    NB_PERSIST_OP_FLUSH,    /* sinalizar que flush foi solicitado */
} nb_persist_op_t;

typedef struct {
    nb_persist_op_t op;
    uint16_t        data_len;
    char            data[LOG_LINE_MAX];
} nb_persist_item_t;

/* ── Estado interno ───────────────────────────────────────────────────────── */

static QueueHandle_t    s_queue         = NULL;
static SemaphoreHandle_t s_flush_sem    = NULL;
static bool             s_initialized  = false;
static volatile bool    s_sd_available = false;

/* Vprintf original do ESP-IDF — salvo ao instalar o hook. */
static vprintf_like_t   s_orig_vprintf = NULL;

/* ── Hook de log ─────────────────────────────────────────────────────────── */

/*
 * Substitui o vprintf padrão do ESP-IDF.
 * Chama o vprintf original (output UART) e enfileira para SD se disponível.
 * Usa va_copy para não consumir args antes do vprintf original.
 */
static int nb_log_vprintf(const char *fmt, va_list args)
{
    /* Cópia de args antes do uso — vprintf consome o va_list. */
    va_list args_copy;
    va_copy(args_copy, args);

    /* 1. Output UART via vprintf original. */
    int ret = s_orig_vprintf(fmt, args);

    /* 2. Enfileirar para SD se ativo. */
    if (s_sd_available && s_queue != NULL) {
        nb_persist_item_t item;
        item.op = NB_PERSIST_OP_LOG;
        int n = vsnprintf(item.data, sizeof(item.data), fmt, args_copy);
        if (n > 0) {
            item.data_len = (uint16_t)(n < (int)sizeof(item.data)
                                       ? n
                                       : (int)sizeof(item.data) - 1);
            /* Non-blocking — descarta se fila cheia. */
            xQueueSend(s_queue, &item, 0);
        }
    }

    va_end(args_copy);
    return ret;
}

/* ── Escrita em arquivo ──────────────────────────────────────────────────── */

/*
 * Rotaciona o arquivo de log se ultrapassou o tamanho máximo.
 * Estratégia simples: renomear log.txt → log_old.txt, iniciar novo log.txt.
 */
static void maybe_rotate_log(void)
{
    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (f == NULL) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    if (size >= (long)LOG_FILE_MAX_BYTES) {
        const char *old_path = NB_SD_MOUNT_POINT "/logs/log_old.txt";
        remove(old_path);
        rename(LOG_FILE_PATH, old_path);
        ESP_LOGI(TAG, "Log rotacionado (>%luKB)", (unsigned long)(LOG_FILE_MAX_BYTES / 1024));
    }
}

/*
 * Escreve uma linha no arquivo de log do SD.
 * Abre → escreve → fecha por operação (sem arquivo aberto por longo período).
 * Em falha: marca SD como indisponível e loga no UART.
 */
static void write_log_line(const char *data, uint16_t len)
{
    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f == NULL) {
        /* SD removido durante operação. */
        if (s_sd_available) {
            s_sd_available = false;
            ESP_LOGW(TAG, "SD removido — modo degradado ativado");
        }
        return;
    }

    fwrite(data, 1, len, f);
    /* Garantir newline se ausente. */
    if (len > 0 && data[len - 1] != '\n') {
        fputc('\n', f);
    }
    fclose(f);
}

/* ── Task ────────────────────────────────────────────────────────────────── */

static void persistence_task(void *arg)
{
    (void)arg;

    nb_persist_item_t item;
    uint32_t remount_ticks  = 0;
    uint32_t log_count      = 0;

    ESP_LOGI(TAG, "persistence_task iniciada");

    while (1) {
        /* Processar itens da fila com timeout de 1s. */
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (item.op) {
                case NB_PERSIST_OP_LOG:
                    if (s_sd_available) {
                        if ((log_count % 100) == 0) {
                            maybe_rotate_log();
                        }
                        write_log_line(item.data, item.data_len);
                        log_count++;
                    }
                    break;

                case NB_PERSIST_OP_FLUSH:
                    /* Drena a fila e sinaliza o semáforo de flush. */
                    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
                        if (item.op == NB_PERSIST_OP_LOG && s_sd_available) {
                            write_log_line(item.data, item.data_len);
                        }
                    }
                    if (s_flush_sem) {
                        xSemaphoreGive(s_flush_sem);
                    }
                    break;
            }
        }

        /* Re-mount periódico quando SD não está disponível. */
        remount_ticks++;
        if (!s_sd_available && remount_ticks >= NB_SD_REMOUNT_INTERVAL_S) {
            remount_ticks = 0;
            if (sd_hal_try_remount() == ESP_OK) {
                s_sd_available = true;
                log_count      = 0;
                ESP_LOGI(TAG, "SD disponivel novamente — escrita de log reativada");
            }
        }
    }
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t persistence_mgr_init(void)
{
    /* Fila de operações. */
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(nb_persist_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate falhou — sem memoria");
        return ESP_ERR_NO_MEM;
    }

    /* Semáforo para flush síncrono. */
    s_flush_sem = xSemaphoreCreateBinary();
    if (s_flush_sem == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "xSemaphoreCreateBinary falhou");
        return ESP_ERR_NO_MEM;
    }

    /* Verificar se SD está disponível. */
    s_sd_available = sd_hal_is_mounted();
    if (s_sd_available) {
        ESP_LOGI(TAG, "SD disponivel — escrita de log ativada");
    } else {
        ESP_LOGW(TAG, "SD nao disponivel — modo degradado (re-mount em %us)",
                 NB_SD_REMOUNT_INTERVAL_S);
    }

    /* Instalar hook de vprintf para capturar logs do ESP-IDF. */
    s_orig_vprintf = esp_log_set_vprintf(nb_log_vprintf);

    /* Criar persistence_task. */
    BaseType_t ret = xTaskCreatePinnedToCore(
        persistence_task,
        "nb_persist",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        NULL,
        TASK_CORE
    );

    if (ret != pdPASS) {
        esp_log_set_vprintf(s_orig_vprintf);  /* restaurar vprintf */
        vSemaphoreDelete(s_flush_sem);
        vQueueDelete(s_queue);
        s_queue     = NULL;
        s_flush_sem = NULL;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore falhou");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

bool persistence_mgr_is_sd_available(void)
{
    return s_sd_available;
}

void persistence_mgr_enqueue_log(const char *line, size_t len)
{
    if (!s_initialized || s_queue == NULL) return;

    nb_persist_item_t item;
    item.op       = NB_PERSIST_OP_LOG;
    item.data_len = (uint16_t)(len > LOG_LINE_MAX ? LOG_LINE_MAX : len);
    memcpy(item.data, line, item.data_len);

    xQueueSend(s_queue, &item, 0);
}

void persistence_mgr_flush_sync(void)
{
    if (!s_initialized || s_queue == NULL) return;

    nb_persist_item_t flush_item = { .op = NB_PERSIST_OP_FLUSH, .data_len = 0 };
    xQueueSend(s_queue, &flush_item, pdMS_TO_TICKS(1000));

    /* Aguardar sinal de conclusão com timeout. */
    xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(FLUSH_TIMEOUT_MS));
}
