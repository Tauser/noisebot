/*
 * power_monitor.c — Implementação do power monitor do NoiseBot
 */

#include "power_monitor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"

#include "logger.h"
#include "error_policy.h"

#define TAG "nb_power"

/* ── NVS (namespace compartilhado "nb_sys") ──────────────────────────────── */

#define NVS_NS_SYS         "nb_sys"
#define NVS_KEY_BRN_COUNT  "brn_count"
#define NVS_KEY_SAFE_MODE  "safe_mode"

/* ── Estado interno ──────────────────────────────────────────────────────── */

static nb_power_mode_t s_mode           = NB_POWER_NORMAL;
static bool            s_brownout_reset = false;
static uint8_t         s_brownout_count = 0;
static bool            s_initialized   = false;

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
     * Callback de brownout em runtime:
     * esp_brownout_cb_register() não é API pública no ESP-IDF 5.x/6.x.
     * TODO(etapa-3.2): investigar hook disponível (esp_register_shutdown_handler
     * ou acesso interno) para acionar motion_safety_emergency_stop antes do reset.
     * Por enquanto: detecção reativa via esp_reset_reason() no próximo boot.
     */

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
    if (s_mode == mode) return;

    static const char *const mode_names[] = {
        "NORMAL", "SD_DEGRADED", "SAFE_MODE", "EMERGENCY_STOP"
    };

    const char *from = ((unsigned)s_mode < 4u) ? mode_names[s_mode] : "?";
    const char *to   = ((unsigned)mode  < 4u) ? mode_names[mode]  : "?";

    NB_LOGI(TAG, "Modo: %s → %s (%s)", from, to, reason ? reason : "sem motivo");
    s_mode = mode;
}

bool power_monitor_is_brownout_reset(void)
{
    return s_brownout_reset;
}

uint8_t power_monitor_get_brownout_count(void)
{
    return s_brownout_count;
}
