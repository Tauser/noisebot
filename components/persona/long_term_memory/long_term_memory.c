/*
 * long_term_memory.c — Memória de longo prazo do NoiseBot
 *
 * Formato binário em disco, protegido por CRC32.
 * Todos os dados são mantidos em RAM e periodicamente persistidos.
 *
 * Dois arquivos SD:
 *   ltm_main.bin    (ltm_main_file_t, ~1.2KB)  — estado + histórico 200 entradas
 *   ltm_journal.bin (ltm_journal_file_t, ~6KB) — journal 1000 entradas
 *
 * Familiaridade: score = 1 − exp(−total_touch_count / 50)
 *   score ≥ 0.5 (familiar) a partir de ~35 toques.
 */

#include "long_term_memory.h"
#include "persistence_mgr.h"

#include "esp_log.h"
#include "esp_rom_crc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define TAG "nb_ltm"

/* ── Parâmetros ──────────────────────────────────────────────────────────── */

#define LTM_MAGIC           0x4E424C4DUL   /* "NBLM" */
#define LTM_VERSION         1UL
#define LTM_HISTORY_SIZE    200
#define LTM_JOURNAL_SIZE    1000
#define LTM_FAMILIAR_THR    0.5f
#define LTM_FAMILIAR_DECAY  50.0f          /* toque-meios no score exponencial */

#define LTM_MAIN_PATH       "/sdcard/memory/ltm_main.bin"
#define LTM_JOURNAL_PATH    "/sdcard/memory/ltm_journal.bin"

/* ── Tipos binários em disco ─────────────────────────────────────────────── */

/*
 * Uma entrada de interação: 6 bytes.
 * uptime_s = segundos de uptime acumulado no momento do registro.
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;      /* ltm_iact_type_t */
    uint8_t  _pad;
    uint32_t uptime_s;
} ltm_entry_t;

/*
 * Arquivo principal: persona_state + ring buffer de histórico.
 * Tamanho: 4+4+4+4+4+4+2+2 + 200×6 + 4 = 1232 bytes.
 */
typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    total_touch_count;
    uint32_t    total_sessions;
    uint32_t    cumulative_uptime_s;   /* acumulado de sessões anteriores */
    float       familiarity_score;
    uint16_t    hist_idx;              /* próxima posição de escrita no ring */
    uint16_t    hist_len;              /* entradas válidas (0..LTM_HISTORY_SIZE) */
    ltm_entry_t hist[LTM_HISTORY_SIZE];
    uint32_t    crc32;                 /* CRC de tudo acima deste campo */
} ltm_main_file_t;

/*
 * Journal de eventos: ring buffer de 1000 entradas.
 * Tamanho: 4+4+4+4 + 1000×6 + 4 = 6016 bytes.
 */
typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    journal_idx;
    uint32_t    journal_len;
    ltm_entry_t journal[LTM_JOURNAL_SIZE];
    uint32_t    crc32;
} ltm_journal_file_t;

/* ── Estado em RAM ───────────────────────────────────────────────────────── */

static ltm_main_file_t    s_main;
static ltm_journal_file_t s_journal;
static SemaphoreHandle_t  s_mutex        = NULL;
static bool               s_initialized  = false;
static bool               s_sd_available = false;
static uint32_t           s_session_uptime_ms = 0;   /* uptime desta sessão */

/* ── Helpers de CRC ──────────────────────────────────────────────────────── */

static uint32_t compute_crc(const void *buf, size_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)buf, (uint32_t)len);
}

/* Verifica magic, version e CRC de um arquivo principal. */
static bool main_file_valid(const ltm_main_file_t *f)
{
    if (f->magic != LTM_MAGIC || f->version != LTM_VERSION) return false;
    size_t crc_data_len = offsetof(ltm_main_file_t, crc32);
    return f->crc32 == compute_crc(f, crc_data_len);
}

static bool journal_file_valid(const ltm_journal_file_t *f)
{
    if (f->magic != LTM_MAGIC || f->version != LTM_VERSION) return false;
    size_t crc_data_len = offsetof(ltm_journal_file_t, crc32);
    return f->crc32 == compute_crc(f, crc_data_len);
}

/* ── Helpers de atualização de score ────────────────────────────────────── */

static void recalc_familiarity(void)
{
    float t = (float)s_main.total_touch_count / LTM_FAMILIAR_DECAY;
    s_main.familiarity_score = 1.0f - expf(-t);
}

/* ── Leitura / escrita em SD ─────────────────────────────────────────────── */

static bool load_main(void)
{
    FILE *f = fopen(LTM_MAIN_PATH, "rb");
    if (!f) return false;

    /* Lê direto no global — evita alocar 1232 bytes na stack do boot.
     * Se inválido, reinicializa para defaults (fread pode ter sobrescrito
     * parcialmente s_main, então memset + reinit é necessário). */
    bool ok = (fread(&s_main, sizeof(s_main), 1, f) == 1) && main_file_valid(&s_main);
    fclose(f);

    if (ok) {
        ESP_LOGI(TAG, "ltm_main carregado: touches=%lu sessions=%lu uptime=%lu h score=%.2f",
                 (unsigned long)s_main.total_touch_count,
                 (unsigned long)s_main.total_sessions,
                 (unsigned long)(s_main.cumulative_uptime_s / 3600UL),
                 s_main.familiarity_score);
    } else {
        ESP_LOGW(TAG, "ltm_main ausente ou corrompido — inicializando defaults");
        memset(&s_main, 0, sizeof(s_main));
        s_main.magic   = LTM_MAGIC;
        s_main.version = LTM_VERSION;
    }
    return ok;
}

static bool load_journal(void)
{
    FILE *f = fopen(LTM_JOURNAL_PATH, "rb");
    if (!f) return false;

    /* Lê direto no global — evita alocar 6016 bytes na stack do boot. */
    bool ok = (fread(&s_journal, sizeof(s_journal), 1, f) == 1) && journal_file_valid(&s_journal);
    fclose(f);

    if (!ok) {
        ESP_LOGW(TAG, "ltm_journal ausente ou corrompido — inicializando vazio");
        memset(&s_journal, 0, sizeof(s_journal));
        s_journal.magic   = LTM_MAGIC;
        s_journal.version = LTM_VERSION;
    }
    return ok;
}

/* CRC já computado em ltm_flush() antes de chamar estas funções. */
static void write_main(void)
{
    FILE *f = fopen(LTM_MAIN_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "falha ao abrir " LTM_MAIN_PATH " para escrita");
        return;
    }
    if (fwrite(&s_main, sizeof(s_main), 1, f) != 1) {
        ESP_LOGW(TAG, "falha ao escrever ltm_main.bin");
    }
    fclose(f);
}

static void write_journal(void)
{
    FILE *f = fopen(LTM_JOURNAL_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "falha ao abrir " LTM_JOURNAL_PATH " para escrita");
        return;
    }
    if (fwrite(&s_journal, sizeof(s_journal), 1, f) != 1) {
        ESP_LOGW(TAG, "falha ao escrever ltm_journal.bin");
    }
    fclose(f);
}

/* ── Helpers de ring buffer ──────────────────────────────────────────────── */

static void hist_push(ltm_iact_type_t type, uint32_t uptime_s)
{
    ltm_entry_t e = { .type = (uint8_t)type, ._pad = 0, .uptime_s = uptime_s };
    s_main.hist[s_main.hist_idx] = e;
    s_main.hist_idx = (uint16_t)((s_main.hist_idx + 1u) % LTM_HISTORY_SIZE);
    if (s_main.hist_len < LTM_HISTORY_SIZE) {
        s_main.hist_len++;
    }
}

static void journal_push(ltm_iact_type_t type, uint32_t uptime_s)
{
    ltm_entry_t e = { .type = (uint8_t)type, ._pad = 0, .uptime_s = uptime_s };
    s_journal.journal[s_journal.journal_idx] = e;
    s_journal.journal_idx =
        (uint32_t)((s_journal.journal_idx + 1u) % LTM_JOURNAL_SIZE);
    if (s_journal.journal_len < LTM_JOURNAL_SIZE) {
        s_journal.journal_len++;
    }
}

/* ── API ─────────────────────────────────────────────────────────────────── */

esp_err_t ltm_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "falha ao criar mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Inicializa structs com defaults antes de tentar carregar. */
    memset(&s_main, 0, sizeof(s_main));
    s_main.magic   = LTM_MAGIC;
    s_main.version = LTM_VERSION;

    memset(&s_journal, 0, sizeof(s_journal));
    s_journal.magic   = LTM_MAGIC;
    s_journal.version = LTM_VERSION;

    /* Verificar se SD está disponível via persistence_mgr. */
    s_sd_available = persistence_mgr_is_sd_available();

    if (s_sd_available) {
        load_main();
        load_journal();
    } else {
        ESP_LOGW(TAG, "SD indisponível — LTM só em RAM (sem persistência)");
    }

    /* Incrementar session count para esta sessão. */
    s_main.total_sessions++;

    s_initialized = true;
    ESP_LOGI(TAG, "LTM inicializada — familiar=%s touches=%lu sessions=%lu",
             s_main.familiarity_score >= LTM_FAMILIAR_THR ? "sim" : "nao",
             (unsigned long)s_main.total_touch_count,
             (unsigned long)s_main.total_sessions);
    return ESP_OK;
}

void ltm_record(ltm_iact_type_t type)
{
    if (!s_initialized) return;

    uint32_t uptime_s = s_main.cumulative_uptime_s +
                        (uint32_t)(s_session_uptime_ms / 1000UL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Atualizar contadores. */
    if (type == LTM_IACT_TOUCH_TAP || type == LTM_IACT_TOUCH_LONG) {
        s_main.total_touch_count++;
        recalc_familiarity();
    }

    hist_push(type, uptime_s);
    journal_push(type, uptime_s);

    xSemaphoreGive(s_mutex);
}

void ltm_tick(uint32_t dt_ms)
{
    if (!s_initialized) return;
    s_session_uptime_ms += dt_ms;
}

void ltm_flush(void)
{
    if (!s_initialized || !s_sd_available) return;

    /*
     * Mutex liberado ANTES do fwrite para não bloquear ltm_record() de outras
     * tasks (touch_update_task, audio_task) durante a escrita no SD.
     *
     * CRC é computado sob o mutex sobre o snapshot atual. Se ltm_record()
     * modificar s_main.hist[] durante o fwrite subsequente, o CRC ficará
     * stale — próximo flush regenera CRC válido. Tradeoff aceitável para
     * evitar bloqueio de touch.
     */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_main.cumulative_uptime_s += (uint32_t)(s_session_uptime_ms / 1000UL);
    s_session_uptime_ms         = s_session_uptime_ms % 1000UL;

    s_main.crc32    = compute_crc(&s_main,    offsetof(ltm_main_file_t,    crc32));
    s_journal.crc32 = compute_crc(&s_journal, offsetof(ltm_journal_file_t, crc32));

    xSemaphoreGive(s_mutex);  /* ← libera antes do fwrite */

    write_main();
    write_journal();

    ESP_LOGD(TAG, "flush: touches=%lu uptime=%lu s familiar=%.2f",
             (unsigned long)s_main.total_touch_count,
             (unsigned long)s_main.cumulative_uptime_s,
             s_main.familiarity_score);
}

uint32_t ltm_get_total_touch_count(void)
{
    return s_initialized ? s_main.total_touch_count : 0u;
}

uint32_t ltm_get_total_sessions(void)
{
    return s_initialized ? s_main.total_sessions : 0u;
}

uint32_t ltm_count_iact(ltm_iact_type_t type)
{
    if (!s_initialized) return 0u;
    uint32_t count = 0u;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < s_main.hist_len; i++) {
        if ((ltm_iact_type_t)s_main.hist[i].type == type) count++;
    }
    xSemaphoreGive(s_mutex);
    return count;
}

uint32_t ltm_get_hours_alive(void)
{
    if (!s_initialized) return 0u;
    uint32_t total_s = s_main.cumulative_uptime_s +
                       (uint32_t)(s_session_uptime_ms / 1000UL);
    return total_s / 3600UL;
}

bool ltm_is_user_familiar(void)
{
    return s_initialized &&
           (s_main.familiarity_score >= LTM_FAMILIAR_THR);
}
