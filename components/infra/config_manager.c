/*
 * config_manager.c — Implementação do gerenciador de configuração do NoiseBot
 */

#include "config_manager.h"
#include "nvs_hal.h"
#include "nb_config_keys.h"
#include "logger.h"
#include "esp_random.h"

#define TAG "nb_cfg"

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nvs_handle_t s_h_cfg;       /* handle nb_cfg — aberto em config_manager_init */
static nvs_handle_t s_h_svc;       /* handle nb_svc */
static bool         s_ready = false;

/* ── Lookup de chaves de servo ───────────────────────────────────────────── */

typedef struct {
    const char *key_min;
    const char *key_max;
    const char *key_ctr;
} servo_keys_t;

static const servo_keys_t s_servo_keys[2] = {
    { NB_CFG_KEY_SRV1_MIN, NB_CFG_KEY_SRV1_MAX, NB_CFG_KEY_SRV1_CTR },  /* ID 1: pan  */
    { NB_CFG_KEY_SRV2_MIN, NB_CFG_KEY_SRV2_MAX, NB_CFG_KEY_SRV2_CTR },  /* ID 2: tilt */
};

/*
 * Retorna ponteiro para as chaves do servo ou NULL se servo_id inválido.
 * servo_id é 1-based (1=pan, 2=tilt).
 */
static const servo_keys_t *servo_keys_for(uint8_t servo_id)
{
    if (servo_id < 1 || servo_id > 2) return NULL;
    return &s_servo_keys[servo_id - 1];
}

/* ── Cache RAM dos limites de servo ──────────────────────────────────────── */
/* Carregado uma vez no init; evita leituras de flash no caminho crítico.     */

typedef struct { int16_t min; int16_t max; int16_t ctr; } servo_cache_t;

static servo_cache_t s_servo_cache[2];  /* índice 0 = servo 1 (pan), 1 = servo 2 (tilt) */

static void servo_cache_load(void)
{
    for (uint8_t id = 1; id <= 2; id++) {
        const servo_keys_t *k = servo_keys_for(id);
        if (!k) continue;
        s_servo_cache[id - 1].min = nvs_hal_get_i16(s_h_cfg, k->key_min, (int16_t)NB_CFG_DEFAULT_SRV_MIN);
        s_servo_cache[id - 1].max = nvs_hal_get_i16(s_h_cfg, k->key_max, (int16_t)NB_CFG_DEFAULT_SRV_MAX);
        s_servo_cache[id - 1].ctr = nvs_hal_get_i16(s_h_cfg, k->key_ctr, (int16_t)NB_CFG_DEFAULT_SRV_CTR);
    }
}

/* ── Aplicação de defaults ───────────────────────────────────────────────── */

/*
 * Escreve todos os defaults no namespace nb_cfg.
 * Chamado apenas quando cfg_ver está ausente ou desatualizado.
 * Não faz commit — o chamador faz commit após esta função.
 */
static esp_err_t write_cfg_defaults(void)
{
    esp_err_t err;

    /* Servo pan (ID 1) */
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV1_MIN, NB_CFG_DEFAULT_SRV_MIN)) != ESP_OK) return err;
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV1_MAX, NB_CFG_DEFAULT_SRV_MAX)) != ESP_OK) return err;
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV1_CTR, NB_CFG_DEFAULT_SRV_CTR)) != ESP_OK) return err;

    /* Servo tilt (ID 2) */
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV2_MIN, NB_CFG_DEFAULT_SRV_MIN)) != ESP_OK) return err;
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV2_MAX, NB_CFG_DEFAULT_SRV_MAX)) != ESP_OK) return err;
    if ((err = nvs_hal_set_i16(s_h_cfg, NB_CFG_KEY_SRV2_CTR, NB_CFG_DEFAULT_SRV_CTR)) != ESP_OK) return err;

    /* Áudio, display, touch, comportamento */
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_VOLUME,    NB_CFG_DEFAULT_VOLUME))    != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_V2_CAP_EN, NB_CFG_DEFAULT_V2_CAP_EN)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_V2_CAP_TX, NB_CFG_DEFAULT_V2_CAP_TX)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_V2_ACT_DEC, NB_CFG_DEFAULT_V2_ACT_DEC)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_V2_ACT_MIG, 1U)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_BRIGHTNESS, NB_CFG_DEFAULT_BRIGHTNESS)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u8 (s_h_cfg, NB_CFG_KEY_TOUCH_SENS, NB_CFG_DEFAULT_TOUCH_SENS)) != ESP_OK) return err;
    if ((err = nvs_hal_set_u32(s_h_cfg, NB_CFG_KEY_IDLE_TMO,   NB_CFG_DEFAULT_IDLE_TIMEOUT_S)) != ESP_OK) return err;

    return ESP_OK;
}

/*
 * Escreve defaults no namespace nb_svc.
 * persona_seed é gerado com esp_random() para unicidade entre unidades.
 */
static esp_err_t write_svc_defaults(void)
{
    esp_err_t err;

    if ((err = nvs_hal_set_u8(s_h_svc, NB_SVC_KEY_EMOTION, NB_SVC_DEFAULT_EMOTION)) != ESP_OK) return err;

    uint32_t seed = esp_random();
    if ((err = nvs_hal_set_u32(s_h_svc, NB_SVC_KEY_PERSONA, seed)) != ESP_OK) return err;

    NB_LOGI(TAG, "persona_seed gerado: 0x%08lX", (unsigned long)seed);
    return ESP_OK;
}

/* Teste manual de SLEEPING: defina >0 para forçar idle_timeout_s curto.
 * Produção/default: 0 mantém/restaura NB_CFG_DEFAULT_IDLE_TIMEOUT_S. */
#define NB_SLEEP_TEST_IDLE_TIMEOUT_S 0u

static esp_err_t restore_idle_timeout_if_legacy(void)
{
    uint32_t idle_timeout_s = nvs_hal_get_u32(s_h_cfg,
                                              NB_CFG_KEY_IDLE_TMO,
                                              NB_CFG_DEFAULT_IDLE_TIMEOUT_S);

#if NB_SLEEP_TEST_IDLE_TIMEOUT_S > 0
    if (idle_timeout_s == NB_SLEEP_TEST_IDLE_TIMEOUT_S) {
        return ESP_OK;
    }

    NB_LOGI(TAG, "TESTE SLEEPING: idle_timeout=%lus -> %lus",
            (unsigned long)idle_timeout_s,
            (unsigned long)NB_SLEEP_TEST_IDLE_TIMEOUT_S);

    esp_err_t test_err = nvs_hal_set_u32(s_h_cfg,
                                         NB_CFG_KEY_IDLE_TMO,
                                         NB_SLEEP_TEST_IDLE_TIMEOUT_S);
    if (test_err == ESP_OK) {
        nvs_hal_commit(s_h_cfg);
    }
    return test_err;
#endif

    if (idle_timeout_s >= NB_CFG_DEFAULT_IDLE_TIMEOUT_S) {
        return ESP_OK;
    }

    NB_LOGI(TAG, "idle_timeout legado=%lus — restaurando para %lus",
            (unsigned long)idle_timeout_s,
            (unsigned long)NB_CFG_DEFAULT_IDLE_TIMEOUT_S);

    esp_err_t err = nvs_hal_set_u32(s_h_cfg,
                                    NB_CFG_KEY_IDLE_TMO,
                                    NB_CFG_DEFAULT_IDLE_TIMEOUT_S);
    if (err == ESP_OK) {
        nvs_hal_commit(s_h_cfg);
    }
    return err;
}

static esp_err_t restore_touch_sensitivity_if_legacy(void)
{
    uint8_t touch_sens = nvs_hal_get_u8(s_h_cfg,
                                        NB_CFG_KEY_TOUCH_SENS,
                                        NB_CFG_DEFAULT_TOUCH_SENS);

    /*
     * Histórico de defaults (todos medidos em hardware):
     *   10 → primeiro default  (2%   threshold) — sem medição; alto demais na época
     *   25 → segundo default   (5%   threshold) — sem medição; alto demais na época
     *    5 → terceiro default  (1%   threshold) — baixo demais; fio dispara por proximidade
     *    8 → quarto default    (1,6% threshold) — baixo demais; fita+fio disparam por proximidade
     *   15 → quinto default    (3%   threshold) — baixo demais; proximidade fraw ~6% > 3%
     *  100 → default atual     (20%  threshold) — calibrado com log real: proximidade fraw ~6%,
     *                                             toque real fraw 130–200%; margem ampla.
     *
     * Migrar qualquer dispositivo que ainda tenha um dos valores legados.
     */
    if (touch_sens != 10U && touch_sens != 25U && touch_sens != 5U
        && touch_sens != 8U && touch_sens != 15U) {
        return ESP_OK;
    }

    NB_LOGI(TAG, "touch_sens legado=%u — restaurando para %u",
            (unsigned)touch_sens,
            (unsigned)NB_CFG_DEFAULT_TOUCH_SENS);

    esp_err_t err = nvs_hal_set_u8(s_h_cfg,
                                   NB_CFG_KEY_TOUCH_SENS,
                                   NB_CFG_DEFAULT_TOUCH_SENS);
    if (err == ESP_OK) {
        nvs_hal_commit(s_h_cfg);
    }
    return err;
}

static esp_err_t ensure_voice_audio_v2_defaults(void)
{
    bool changed = false;
    esp_err_t err = ESP_OK;

    if (!nvs_hal_key_exists(s_h_cfg, NB_CFG_KEY_V2_CAP_EN)) {
        err = nvs_hal_set_u8(s_h_cfg,
                             NB_CFG_KEY_V2_CAP_EN,
                             NB_CFG_DEFAULT_V2_CAP_EN);
        if (err != ESP_OK) {
            return err;
        }
        changed = true;
    }

    if (!nvs_hal_key_exists(s_h_cfg, NB_CFG_KEY_V2_CAP_TX)) {
        err = nvs_hal_set_u8(s_h_cfg,
                             NB_CFG_KEY_V2_CAP_TX,
                             NB_CFG_DEFAULT_V2_CAP_TX);
        if (err != ESP_OK) {
            return err;
        }
        changed = true;
    }

    if (!nvs_hal_key_exists(s_h_cfg, NB_CFG_KEY_V2_ACT_DEC)) {
        err = nvs_hal_set_u8(s_h_cfg,
                             NB_CFG_KEY_V2_ACT_DEC,
                             NB_CFG_DEFAULT_V2_ACT_DEC);
        if (err != ESP_OK) {
            return err;
        }
        changed = true;
    }

    if (!nvs_hal_key_exists(s_h_cfg, NB_CFG_KEY_V2_ACT_MIG)) {
        err = nvs_hal_set_u8(s_h_cfg,
                             NB_CFG_KEY_V2_ACT_DEC,
                             NB_CFG_DEFAULT_V2_ACT_DEC);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_hal_set_u8(s_h_cfg, NB_CFG_KEY_V2_ACT_MIG, 1U);
        if (err != ESP_OK) {
            return err;
        }
        changed = true;
    }

    if (changed) {
        nvs_hal_commit(s_h_cfg);
    }
    return ESP_OK;
}

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t config_manager_init(void)
{
    esp_err_t err;

    /* Abrir handles. */
    if ((err = nvs_hal_open(NB_CFG_NS, NVS_READWRITE, &s_h_cfg)) != ESP_OK) return err;
    if ((err = nvs_hal_open(NB_SVC_NS, NVS_READWRITE, &s_h_svc)) != ESP_OK) {
        nvs_hal_close(s_h_cfg);
        return err;
    }

    /* Verificar versão de schema → aplicar defaults se necessário. */
    uint8_t ver = nvs_hal_get_u8(s_h_cfg, NB_CFG_KEY_VERSION, 0);

    if (ver < NB_CFG_SCHEMA_VERSION) {
        NB_LOGI(TAG, "cfg_ver=%u < %u — aplicando defaults", ver, NB_CFG_SCHEMA_VERSION);

        if ((err = write_cfg_defaults()) != ESP_OK) {
            NB_LOGE(TAG, "write_cfg_defaults falhou: %s", esp_err_to_name(err));
            nvs_hal_close(s_h_cfg);
            nvs_hal_close(s_h_svc);
            return err;
        }
        if ((err = write_svc_defaults()) != ESP_OK) {
            NB_LOGE(TAG, "write_svc_defaults falhou: %s", esp_err_to_name(err));
            nvs_hal_close(s_h_cfg);
            nvs_hal_close(s_h_svc);
            return err;
        }

        nvs_hal_set_u8(s_h_cfg, NB_CFG_KEY_VERSION, NB_CFG_SCHEMA_VERSION);
        nvs_hal_commit(s_h_cfg);
        nvs_hal_commit(s_h_svc);

        NB_LOGI(TAG, "Defaults aplicados — primeiro boot ou migração de schema");
    } else {
        NB_LOGI(TAG, "Configuração carregada (cfg_ver=%u)", ver);
    }

    if ((err = restore_idle_timeout_if_legacy()) != ESP_OK) {
        NB_LOGE(TAG, "restore_idle_timeout_if_legacy falhou: %s", esp_err_to_name(err));
        nvs_hal_close(s_h_cfg);
        nvs_hal_close(s_h_svc);
        return err;
    }

    if ((err = restore_touch_sensitivity_if_legacy()) != ESP_OK) {
        NB_LOGE(TAG, "restore_touch_sensitivity_if_legacy falhou: %s", esp_err_to_name(err));
        nvs_hal_close(s_h_cfg);
        nvs_hal_close(s_h_svc);
        return err;
    }

    if ((err = ensure_voice_audio_v2_defaults()) != ESP_OK) {
        NB_LOGE(TAG, "ensure_voice_audio_v2_defaults falhou: %s", esp_err_to_name(err));
        nvs_hal_close(s_h_cfg);
        nvs_hal_close(s_h_svc);
        return err;
    }

    servo_cache_load();
    s_ready = true;
    return ESP_OK;
}

bool config_manager_is_ready(void)
{
    return s_ready;
}

/* ── Servo ───────────────────────────────────────────────────────────────── */

int16_t config_get_servo_limit_min(uint8_t servo_id)
{
    if (servo_id < 1 || servo_id > 2) return (int16_t)NB_CFG_DEFAULT_SRV_MIN;
    return s_servo_cache[servo_id - 1].min;
}

int16_t config_get_servo_limit_max(uint8_t servo_id)
{
    if (servo_id < 1 || servo_id > 2) return (int16_t)NB_CFG_DEFAULT_SRV_MAX;
    return s_servo_cache[servo_id - 1].max;
}

int16_t config_get_servo_center(uint8_t servo_id)
{
    if (servo_id < 1 || servo_id > 2) return (int16_t)NB_CFG_DEFAULT_SRV_CTR;
    return s_servo_cache[servo_id - 1].ctr;
}

esp_err_t config_set_servo_limit_min(uint8_t servo_id, int16_t val)
{
    const servo_keys_t *k = servo_keys_for(servo_id);
    if (!k) return ESP_ERR_INVALID_ARG;
    if (val < NB_CFG_SRV_POS_ABS_MIN || val > NB_CFG_SRV_POS_ABS_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_i16(s_h_cfg, k->key_min, val);
    if (err == ESP_OK) { nvs_hal_commit(s_h_cfg); s_servo_cache[servo_id - 1].min = val; }
    return err;
}

esp_err_t config_set_servo_limit_max(uint8_t servo_id, int16_t val)
{
    const servo_keys_t *k = servo_keys_for(servo_id);
    if (!k) return ESP_ERR_INVALID_ARG;
    if (val < NB_CFG_SRV_POS_ABS_MIN || val > NB_CFG_SRV_POS_ABS_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_i16(s_h_cfg, k->key_max, val);
    if (err == ESP_OK) { nvs_hal_commit(s_h_cfg); s_servo_cache[servo_id - 1].max = val; }
    return err;
}

esp_err_t config_set_servo_center(uint8_t servo_id, int16_t val)
{
    const servo_keys_t *k = servo_keys_for(servo_id);
    if (!k) return ESP_ERR_INVALID_ARG;
    if (val < NB_CFG_SRV_POS_ABS_MIN || val > NB_CFG_SRV_POS_ABS_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_i16(s_h_cfg, k->key_ctr, val);
    if (err == ESP_OK) { nvs_hal_commit(s_h_cfg); s_servo_cache[servo_id - 1].ctr = val; }
    return err;
}

/* ── Áudio ───────────────────────────────────────────────────────────────── */

uint8_t config_get_volume(void)
{
    return nvs_hal_get_u8(s_h_cfg, NB_CFG_KEY_VOLUME, NB_CFG_DEFAULT_VOLUME);
}

esp_err_t config_set_volume(uint8_t level)
{
    if (level > NB_CFG_VOLUME_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_u8(s_h_cfg, NB_CFG_KEY_VOLUME, level);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

bool config_get_voice_audio_v2_capture_enabled(void)
{
    return nvs_hal_get_u8(s_h_cfg,
                          NB_CFG_KEY_V2_CAP_EN,
                          NB_CFG_DEFAULT_V2_CAP_EN) != 0U;
}

esp_err_t config_set_voice_audio_v2_capture_enabled(bool enabled)
{
    esp_err_t err = nvs_hal_set_u8(s_h_cfg,
                                   NB_CFG_KEY_V2_CAP_EN,
                                   enabled ? 1U : 0U);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

bool config_get_voice_audio_v2_capture_tx_enabled(void)
{
    return nvs_hal_get_u8(s_h_cfg,
                          NB_CFG_KEY_V2_CAP_TX,
                          NB_CFG_DEFAULT_V2_CAP_TX) != 0U;
}

esp_err_t config_set_voice_audio_v2_capture_tx_enabled(bool enabled)
{
    esp_err_t err = nvs_hal_set_u8(s_h_cfg,
                                   NB_CFG_KEY_V2_CAP_TX,
                                   enabled ? 1U : 0U);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

bool config_get_voice_audio_v2_activity_decider_enabled(void)
{
    return nvs_hal_get_u8(s_h_cfg,
                          NB_CFG_KEY_V2_ACT_DEC,
                          NB_CFG_DEFAULT_V2_ACT_DEC) != 0U;
}

esp_err_t config_set_voice_audio_v2_activity_decider_enabled(bool enabled)
{
    esp_err_t err = nvs_hal_set_u8(s_h_cfg,
                                   NB_CFG_KEY_V2_ACT_DEC,
                                   enabled ? 1U : 0U);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

/* ── Display ─────────────────────────────────────────────────────────────── */

uint8_t config_get_brightness(void)
{
    return nvs_hal_get_u8(s_h_cfg, NB_CFG_KEY_BRIGHTNESS, NB_CFG_DEFAULT_BRIGHTNESS);
}

esp_err_t config_set_brightness(uint8_t val)
{
    /* 0–255: toda a faixa é válida (sem rejeição). */
    esp_err_t err = nvs_hal_set_u8(s_h_cfg, NB_CFG_KEY_BRIGHTNESS, val);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

/* ── Touch ───────────────────────────────────────────────────────────────── */

uint8_t config_get_touch_sensitivity(void)
{
    return nvs_hal_get_u8(s_h_cfg, NB_CFG_KEY_TOUCH_SENS, NB_CFG_DEFAULT_TOUCH_SENS);
}

esp_err_t config_set_touch_sensitivity(uint8_t val)
{
    if (val < NB_CFG_TOUCH_SENS_MIN || val > NB_CFG_TOUCH_SENS_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_u8(s_h_cfg, NB_CFG_KEY_TOUCH_SENS, val);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

/* ── Comportamento ───────────────────────────────────────────────────────── */

uint32_t config_get_idle_timeout_s(void)
{
    return nvs_hal_get_u32(s_h_cfg, NB_CFG_KEY_IDLE_TMO, NB_CFG_DEFAULT_IDLE_TIMEOUT_S);
}

esp_err_t config_set_idle_timeout_s(uint32_t seconds)
{
    if (seconds < NB_CFG_IDLE_TMO_MIN_S || seconds > NB_CFG_IDLE_TMO_MAX_S) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_hal_set_u32(s_h_cfg, NB_CFG_KEY_IDLE_TMO, seconds);
    if (err == ESP_OK) nvs_hal_commit(s_h_cfg);
    return err;
}

/* ── Estado de serviços ──────────────────────────────────────────────────── */

uint8_t config_get_last_emotion(void)
{
    return nvs_hal_get_u8(s_h_svc, NB_SVC_KEY_EMOTION, NB_SVC_DEFAULT_EMOTION);
}

esp_err_t config_set_last_emotion(uint8_t emotion)
{
    esp_err_t err = nvs_hal_set_u8(s_h_svc, NB_SVC_KEY_EMOTION, emotion);
    if (err == ESP_OK) nvs_hal_commit(s_h_svc);
    return err;
}

uint32_t config_get_persona_seed(void)
{
    return nvs_hal_get_u32(s_h_svc, NB_SVC_KEY_PERSONA, 0);
}
