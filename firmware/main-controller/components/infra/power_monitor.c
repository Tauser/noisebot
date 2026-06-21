/*
 * power_monitor.c — Implementação do power monitor do NoiseBot
 */

#include "power_monitor.h"

#include "nb_hw_config_profile.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"

#include "logger.h"
#include "error_policy.h"
#include "event_bus.h"
#include "nb_events.h"

#define TAG "nb_power"

/* ── NVS (namespace compartilhado "nb_sys") ──────────────────────────────── */

#define NVS_NS_SYS         "nb_sys"
#define NVS_KEY_BRN_COUNT  "brn_count"
#define NVS_KEY_SAFE_MODE  "safe_mode"

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_power_mode_t s_mode           = NB_POWER_NORMAL;
static portMUX_TYPE    s_mode_mux       = portMUX_INITIALIZER_UNLOCKED;
static bool            s_brownout_reset = false;
static uint8_t         s_brownout_count = 0;
static bool            s_initialized   = false;

#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
#if !defined(NB_POWER_5V_DIVIDER_R1_OHM) || !defined(NB_POWER_5V_DIVIDER_R2_OHM)
#error "NB_POWER_5V_DIVIDER_R1_OHM/R2_OHM must be defined when 5V ADC monitor is enabled"
#endif
static adc_oneshot_unit_handle_t s_adc_unit = NULL;
static adc_cali_handle_t         s_adc_cali = NULL;
static bool                      s_adc_ready = false;
static bool                      s_adc_cali_ready = false;
#endif

/* ── Helpers NVS ──────────────────────────────────────────────────────────── */

/*
 * Incrementa brn_count. Se atingir threshold, seta safe_mode=1 para o
 * próximo boot. Erros de NVS são não-fatais.
 */
static void nvs_increment_brownout_count(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "nvs_open(%s) falhou: %s — brn_count nao atualizado",
                NVS_NS_SYS, esp_err_to_name(err));
        return;
    }

    uint8_t count = 0;
    nvs_get_u8(handle, NVS_KEY_BRN_COUNT, &count);  /* default 0 se ausente */

    count++;
    s_brownout_count = count;
    nvs_set_u8(handle, NVS_KEY_BRN_COUNT, count);

    NB_LOGW(TAG, "Reset por brownout — brn_count: %u (limite: %u)",
            count, NB_POWER_BROWNOUT_SAFE_THRESHOLD);

    if (count >= NB_POWER_BROWNOUT_SAFE_THRESHOLD) {
        NB_LOGE(TAG, "Limite de brownouts atingido (%u/%u) — safe_mode sera ativado",
                count, NB_POWER_BROWNOUT_SAFE_THRESHOLD);
        nvs_set_u8(handle, NVS_KEY_SAFE_MODE, 1);
    }

    nvs_commit(handle);
    nvs_close(handle);
}

/*
 * Lê brn_count sem incrementar (boot normal — só para informação).
 */
static void nvs_read_brownout_count(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS_SYS, NVS_READONLY, &handle) != ESP_OK) return;
    nvs_get_u8(handle, NVS_KEY_BRN_COUNT, &s_brownout_count);
    nvs_close(handle);
}

#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
static esp_err_t power_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = NB_POWER_5V_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_unit);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "adc_oneshot_new_unit falhou: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc_unit, NB_POWER_5V_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        NB_LOGW(TAG, "adc_oneshot_config_channel falhou: %s", esp_err_to_name(err));
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = NB_POWER_5V_ADC_UNIT,
        .chan = NB_POWER_5V_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali);
    if (err == ESP_OK) {
        s_adc_cali_ready = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = NB_POWER_5V_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali);
    if (err == ESP_OK) {
        s_adc_cali_ready = true;
    }
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif

    if (err != ESP_OK) {
        NB_LOGW(TAG, "calibracao ADC 5V indisponivel: %s — usando aproximacao",
                esp_err_to_name(err));
    }

    s_adc_ready = true;
    return ESP_OK;
}

static esp_err_t read_adc_pin_mv(uint32_t *adc_mv)
{
    if (adc_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_adc_ready || s_adc_unit == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_unit, NB_POWER_5V_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
        return err;
    }

    int mv = 0;
    if (s_adc_cali_ready && s_adc_cali != NULL) {
        err = adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        mv = (raw * 3100) / 4095;
    }

    if (mv < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *adc_mv = (uint32_t)mv;
    return ESP_OK;
}
#endif

/* ── API pública ─────────────────────────────────────────────────────────── */

esp_err_t power_monitor_init(void)
{
    if (s_initialized) {
        NB_ASSERT(false, TAG, "power_monitor_init chamado mais de uma vez");
        return ESP_FAIL;
    }
    s_initialized = true;

    /* 1. Detectar reset por brownout. */
    s_brownout_reset = (esp_reset_reason() == ESP_RST_BROWNOUT);

    if (s_brownout_reset) {
        nvs_increment_brownout_count();
    } else {
        nvs_read_brownout_count();
    }

    /*
     * Brownout em runtime: o detector de hardware do ESP32-S3 gera reset imediato.
     * Não há API pública no ESP-IDF 5.x para registrar um callback ISR-safe de
     * brownout (esp_brownout_cb_register é interno). A proteção de motion é
     * reativa: na detecção de ESP_RST_BROWNOUT aqui, brn_count é incrementado e,
     * após threshold, safe_mode=1 é gravado no NVS. No próximo boot, PHASE_MOTION
     * é pulada, impedindo que servos armem sem intervenção manual.
     */

#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
    power_adc_init();
#endif

    NB_LOGI(TAG, "Power monitor iniciado — brn_count=%u, brownout_reset=%s, modo=NORMAL",
            s_brownout_count, s_brownout_reset ? "sim" : "nao");

    return ESP_OK;
}

nb_power_mode_t power_monitor_get_mode(void)
{
    return s_mode;
}

void power_monitor_set_mode(nb_power_mode_t mode, const char *reason)
{
    static const char *const mode_names[] = {
        "NORMAL", "SD_DEGRADED", "SAFE_MODE", "EMERGENCY_STOP"
    };

    nb_power_mode_t prev;
    taskENTER_CRITICAL(&s_mode_mux);
    prev   = s_mode;
    s_mode = mode;
    taskEXIT_CRITICAL(&s_mode_mux);

    if (prev == mode) return;

    const char *from = ((unsigned)prev < 4u) ? mode_names[prev] : "?";
    const char *to   = ((unsigned)mode < 4u) ? mode_names[mode]  : "?";
    NB_LOGI(TAG, "Modo: %s → %s (%s)", from, to, reason ? reason : "sem motivo");

    nb_event_t evt = { .type = NB_EVT_POWER_MODE_CHANGED };
    nb_event_publish_async(&evt);
}

bool power_monitor_is_brownout_reset(void)
{
    return s_brownout_reset;
}

uint8_t power_monitor_get_brownout_count(void)
{
    return s_brownout_count;
}

bool power_monitor_has_5v_adc(void)
{
#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
    return s_adc_ready;
#else
    return false;
#endif
}

esp_err_t power_monitor_read_5v_adc_mv(uint32_t *adc_mv)
{
#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
    return read_adc_pin_mv(adc_mv);
#else
    (void)adc_mv;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t power_monitor_read_5v_bus_mv(uint32_t *bus_mv)
{
#if defined(NB_POWER_PIN_5V_ADC) && defined(NB_POWER_5V_ADC_UNIT) && defined(NB_POWER_5V_ADC_CHANNEL)
    if (bus_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t adc_mv = 0;
    esp_err_t err = read_adc_pin_mv(&adc_mv);
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t numerator = adc_mv * (NB_POWER_5V_DIVIDER_R1_OHM + NB_POWER_5V_DIVIDER_R2_OHM);
    *bus_mv = (numerator + (NB_POWER_5V_DIVIDER_R2_OHM / 2U)) / NB_POWER_5V_DIVIDER_R2_OHM;
    return ESP_OK;
#else
    (void)bus_mv;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool power_monitor_is_5v_warn(void)
{
    uint32_t bus_mv = 0;
    if (power_monitor_read_5v_bus_mv(&bus_mv) != ESP_OK) {
        return false;
    }
    return bus_mv < NB_POWER_5V_WARN_MV;
}

bool power_monitor_is_5v_critical(void)
{
    uint32_t bus_mv = 0;
    if (power_monitor_read_5v_bus_mv(&bus_mv) != ESP_OK) {
        return false;
    }
    return bus_mv < NB_POWER_5V_CRITICAL_MV;
}
