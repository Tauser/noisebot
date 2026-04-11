#include "boot_manager.h"
#include "logger.h"
#include "config_manager.h"
#include "event_bus.h"
#include "watchdog_service.h"
#include "power_manager.h"
#include "persistence_manager.h"
#include "error_policy.h"

#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "boot_manager";

static RTC_DATA_ATTR nb_rtc_state_t s_rtc;
static nb_boot_phase_t s_phase = NB_BOOT_PHASE_0_CORE;

/* ------------------------------------------------------------------ */

static esp_err_t prv_nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        NB_LOGW(TAG, "NVS erase necessario: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void prv_stable_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(NB_STABLE_UPTIME_MS));
    boot_manager_mark_stable();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

esp_err_t boot_manager_init(void) {
    /* Validar RTC memory */
    if (s_rtc.magic != NB_RTC_MAGIC) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = NB_RTC_MAGIC;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    s_rtc.last_reset = reason;
    s_rtc.boot_count++;

    switch (reason) {
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_PANIC:
            s_rtc.crash_count++;
            break;
        case ESP_RST_BROWNOUT:
            s_rtc.crash_count++;
            s_rtc.brownout_occurred = true;
            break;
        default:
            break;
    }

    if (s_rtc.crash_count >= NB_SAFE_MODE_CRASH_THRESH) {
        s_rtc.safe_mode_active = true;
    }

    /* Hardware WDT */
    esp_task_wdt_config_t wdt = {
        .timeout_ms    = 10000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_init(&wdt);
    esp_task_wdt_add(NULL);

    /* Logger UART */
    logger_init();

    NB_LOGI(TAG, "=== NoiseBot v%s ===", NOISEBOT_VERSION);
    NB_LOGI(TAG, "Boot #%lu | Reset: %s | Crashes: %lu | SafeMode: %s",
            (unsigned long)s_rtc.boot_count,
            boot_manager_reset_reason_str(reason),
            (unsigned long)s_rtc.crash_count,
            s_rtc.safe_mode_active ? "SIM" : "nao");

    if (s_rtc.brownout_occurred) {
        NB_LOGW(TAG, "Boot anterior terminou em BROWNOUT");
        s_rtc.brownout_occurred = false;
    }

    /* NVS */
    esp_err_t ret = prv_nvs_init();
    if (ret != ESP_OK) {
        NB_LOGE(TAG, "NVS falhou: %s — reiniciando", esp_err_to_name(ret));
        esp_restart();
    }

    return ESP_OK;
}

esp_err_t boot_manager_run(void) {
    esp_err_t ret;

    /* PHASE 1 */
    s_phase = NB_BOOT_PHASE_1_INFRA;
    NB_LOGI(TAG, "Phase 1: infra");
    ESP_ERROR_CHECK(config_manager_init());
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(watchdog_service_init());

    /* PHASE 2 */
    s_phase = NB_BOOT_PHASE_2_POWER;
    NB_LOGI(TAG, "Phase 2: power");
    ret = power_manager_init();
    NB_HANDLE_INIT_FAIL(TAG, ret, NB_ERR_POLICY_POWER_MANAGER);
    power_manager_start();

    if (s_rtc.safe_mode_active) {
        NB_LOGW(TAG, "SAFE MODE — apenas infra e display iniciados");
        s_phase = NB_BOOT_PHASE_COMPLETE;
        EVENT_BUS_PUBLISH_SIMPLE(EVT_SYSTEM_SAFE_MODE, TAG);
        return ESP_OK;
    }

    /* PHASE 3 */
    s_phase = NB_BOOT_PHASE_3_STORAGE;
    NB_LOGI(TAG, "Phase 3: storage / display / LEDs");
    ret = persistence_init();
    NB_HANDLE_INIT_FAIL(TAG, ret, NB_ERR_POLICY_PERSISTENCE);
    persistence_start();
    /* TODO (Etapa 1.1): display_service_init() */
    /* TODO (Etapa 1.2): led_service_init()     */
    /* TODO (Etapa 1.3): storage_service_init() */

    /* PHASE 4 */
    s_phase = NB_BOOT_PHASE_4_SENSORS;
    NB_LOGI(TAG, "Phase 4: sensores");
    /* TODO (Etapa 4.1): touch_service_init() */
    /* TODO (Etapa 4.2): imu_service_init()   */

    /* PHASE 5 */
    s_phase = NB_BOOT_PHASE_5_SERVOS;
    NB_LOGI(TAG, "Phase 5: servos");
    if (power_manager_is_safe_to_start_servos()) {
        /* TODO (Etapa 3.1): servo_driver_init()        */
        /* TODO (Etapa 3.2): motion_service_init()      */
    } else {
        NB_LOGW(TAG, "Bateria critica — servos nao iniciados");
    }

    /* PHASE 6 */
    s_phase = NB_BOOT_PHASE_6_AUDIO;
    NB_LOGI(TAG, "Phase 6: audio");
    /* TODO (Etapa 5.1): audio_capture_service_init()  */
    /* TODO (Etapa 5.2): audio_playback_service_init() */

    /* PHASE 7 */
    s_phase = NB_BOOT_PHASE_7_CAMERA;
    NB_LOGI(TAG, "Phase 7: camera");
    /* TODO (Etapa 6.1): camera_service_init() */

    /* PHASE 8 */
    s_phase = NB_BOOT_PHASE_8_APP;
    NB_LOGI(TAG, "Phase 8: aplicacao");
    /* TODO (Etapa 7.1): behavior_fsm_init() */

    s_phase = NB_BOOT_PHASE_COMPLETE;
    NB_LOGI(TAG, "Boot completo");
    EVENT_BUS_PUBLISH_SIMPLE(EVT_SYSTEM_BOOT_COMPLETE, TAG);

    /* Task que marca sistema como estavel apos NB_STABLE_UPTIME_MS */
    xTaskCreatePinnedToCore(prv_stable_task, "nb_stable", 2048, NULL, 1, NULL, 1);

    return ESP_OK;
}

void boot_manager_mark_stable(void) {
    if (s_rtc.crash_count > 0) {
        NB_LOGI(TAG, "Sistema estavel — crash_count zerado");
        s_rtc.crash_count = 0;
        s_rtc.safe_mode_active = false;
    }
}

bool               boot_manager_is_safe_mode(void)      { return s_rtc.safe_mode_active; }
uint32_t           boot_manager_get_boot_count(void)    { return s_rtc.boot_count; }
uint32_t           boot_manager_get_crash_count(void)   { return s_rtc.crash_count; }
esp_reset_reason_t boot_manager_get_last_reset(void)    { return s_rtc.last_reset; }
nb_boot_phase_t    boot_manager_get_phase(void)         { return s_phase; }

const char *boot_manager_reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        default:                return "UNKNOWN";
    }
}
