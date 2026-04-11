#include "power_manager.h"
#include "config_manager.h"
#include "event_bus.h"
#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "power_mgr";

static nb_power_state_t s_state      = NB_POWER_UNKNOWN;
static uint8_t          s_soc_pct    = 50;   /* default conservador */
static uint16_t         s_voltage_mv = 3700;
static bool             s_charging   = false;

static uint8_t  s_low_pct;
static uint8_t  s_critical_pct;
static uint16_t s_shutdown_mv;

static void prv_update_state(void) {
    nb_power_state_t new_state;

    if (s_charging) {
        new_state = NB_POWER_CHARGING;
    } else if (s_voltage_mv <= s_shutdown_mv || s_soc_pct <= s_critical_pct) {
        new_state = NB_POWER_CRITICAL;
    } else if (s_soc_pct <= s_low_pct) {
        new_state = NB_POWER_LOW;
    } else {
        new_state = NB_POWER_DISCHARGING;
    }

    if (new_state != s_state) {
        s_state = new_state;
        nb_event_t evt = {
            .timestamp_ms = pdTICKS_TO_MS(xTaskGetTickCount()),
            .source       = TAG,
        };
        switch (new_state) {
            case NB_POWER_CHARGING:     evt.type = EVT_POWER_CHARGING;    break;
            case NB_POWER_DISCHARGING:  evt.type = EVT_POWER_DISCHARGING; break;
            case NB_POWER_LOW:          evt.type = EVT_POWER_LOW;         break;
            case NB_POWER_CRITICAL:     evt.type = EVT_POWER_CRITICAL;    break;
            default: return;
        }
        evt.data.power.soc_pct    = s_soc_pct;
        evt.data.power.voltage_mv = s_voltage_mv;
        evt.data.power.is_charging = s_charging;
        event_bus_publish(&evt);

        NB_LOGI(TAG, "Estado: soc=%d%% v=%dmV charging=%d",
                s_soc_pct, s_voltage_mv, s_charging);
    }
}

static void prv_monitor_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* TODO (Etapa 0.3): ler MAX17048 via I2C */
        /* s_soc_pct    = max17048_read_soc();    */
        /* s_voltage_mv = max17048_read_voltage(); */

        /* TODO (Etapa 0.3): ler bq25185 via I2C  */
        /* s_charging   = bq25185_is_charging();   */

        prv_update_state();
    }
}

esp_err_t power_manager_init(void) {
    config_get_u8 (CFG_NS_POWER, "low_pct",     &s_low_pct,      NB_POWER_LOW_PCT_DEFAULT);
    config_get_u8 (CFG_NS_POWER, "critical_pct", &s_critical_pct, NB_POWER_CRITICAL_PCT_DEFAULT);
    config_get_u16(CFG_NS_POWER, "shutdown_mv",  &s_shutdown_mv,  NB_POWER_SHUTDOWN_MV_DEFAULT);

    /* TODO (Etapa 0.3): inicializar I2C e drivers MAX17048 / bq25185 */
    NB_LOGW(TAG, "Drivers I2C pendentes (Etapa 0.3) — usando valores padrao");
    NB_LOGI(TAG, "PowerManager OK (low=%d%% critical=%d%% shutdown=%dmV)",
            s_low_pct, s_critical_pct, s_shutdown_mv);
    return ESP_OK;
}

esp_err_t power_manager_start(void) {
    xTaskCreatePinnedToCore(prv_monitor_task, "nb_power", 3072, NULL, 7, NULL, 1);
    return ESP_OK;
}

nb_power_state_t power_manager_get_state(void)      { return s_state; }
uint8_t          power_manager_get_soc_pct(void)    { return s_soc_pct; }
uint16_t         power_manager_get_voltage_mv(void) { return s_voltage_mv; }
bool             power_manager_is_charging(void)    { return s_charging; }

bool power_manager_is_safe_to_start_servos(void) {
    return (s_state != NB_POWER_CRITICAL && s_state != NB_POWER_UNKNOWN);
}

bool power_manager_is_safe_to_start_camera(void) {
    return (s_state == NB_POWER_CHARGING || s_state == NB_POWER_DISCHARGING);
}
